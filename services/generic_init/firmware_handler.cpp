/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "firmware_handler.h"

#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/wait.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <thread>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <7zCrc.h>
#include <Xz.h>
#include <XzCrc64.h>
#include <zstd.h>

#include "exthandler/exthandler.h"

using android::base::GetProperty;
using android::base::ReadFdToString;
using android::base::Socketpair;
using android::base::Split;
using android::base::Timer;
using android::base::Trim;
using android::base::unique_fd;
using android::base::WaitForProperty;
using android::base::WriteFully;

namespace android {
namespace init {

namespace {
bool PrefixMatch(const std::string& pattern, const std::string& path) {
    return android::base::StartsWith(path, pattern);
}

bool FnMatch(const std::string& pattern, const std::string& path) {
    return fnmatch(pattern.c_str(), path.c_str(), 0) == 0;
}

bool EqualMatch(const std::string& pattern, const std::string& path) {
    return pattern == path;
}
}  // namespace

enum class FirmwareCompression { kNone, kZstd, kXz };

// Suffixes the kernel's firmware loader (CONFIG_FW_LOADER_COMPRESS) also falls back to, in the
// same order: the raw file is tried first.
static const struct {
    const char* suffix;
    FirmwareCompression compression;
} kFirmwareSuffixes[] = {
        {"", FirmwareCompression::kNone},
        {".zst", FirmwareCompression::kZstd},
        {".xz", FirmwareCompression::kXz},
};

static constexpr size_t kDecompressChunkSize = 64 * 1024;

// The kernel does not decompress firmware that is delivered through the sysfs fallback, so do it
// here and stream the result into the firmware data node.
static bool DecompressZstd(const std::string& in, int data_fd) {
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> ctx(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!ctx) return false;

    std::vector<char> out(kDecompressChunkSize);
    ZSTD_inBuffer in_buf = {in.data(), in.size(), 0};
    size_t ret;
    do {
        ZSTD_outBuffer out_buf = {out.data(), out.size(), 0};
        ret = ZSTD_decompressStream(ctx.get(), &out_buf, &in_buf);
        if (ZSTD_isError(ret)) {
            LOG(ERROR) << "firmware: zstd decompression failed: " << ZSTD_getErrorName(ret);
            return false;
        }
        if (!WriteFully(data_fd, out.data(), out_buf.pos)) {
            PLOG(ERROR) << "firmware: write failed";
            return false;
        }
        // Keep going while the input is not consumed or the output buffer was filled, as more
        // data may be pending in the decoder.
        if (ret != 0 && in_buf.pos == in_buf.size && out_buf.pos < out.size()) {
            LOG(ERROR) << "firmware: truncated zstd data";
            return false;
        }
    } while (ret != 0);
    return true;
}

static bool DecompressXz(const std::string& in, int data_fd) {
    static std::once_flag crc_initialized;
    std::call_once(crc_initialized, []() {
        CrcGenerateTable();
        Crc64GenerateTable();
    });

    ISzAlloc alloc;
    alloc.Alloc = [](ISzAllocPtr, size_t size) { return malloc(size); };
    alloc.Free = [](ISzAllocPtr, void* ptr) { free(ptr); };

    CXzUnpacker state{};
    XzUnpacker_Construct(&state, &alloc);
    auto free_state = android::base::make_scope_guard([&state]() { XzUnpacker_Free(&state); });

    std::vector<Byte> out(kDecompressChunkSize);
    const Byte* src = reinterpret_cast<const Byte*>(in.data());
    size_t src_remaining = in.size();
    while (true) {
        SizeT dest_len = out.size();
        SizeT src_len = src_remaining;
        ECoderStatus status;
        SRes res = XzUnpacker_Code(&state, out.data(), &dest_len, src, &src_len,
                                   /*srcFinished=*/true, CODER_FINISH_ANY, &status);
        if (res != SZ_OK) {
            LOG(ERROR) << "firmware: xz decompression failed: " << res;
            return false;
        }
        src += src_len;
        src_remaining -= src_len;
        if (!WriteFully(data_fd, out.data(), dest_len)) {
            PLOG(ERROR) << "firmware: write failed";
            return false;
        }
        // All input consumed and the output buffer not filled: nothing is pending anymore.
        if (src_remaining == 0 && dest_len < out.size()) {
            if (!XzUnpacker_IsStreamWasFinished(&state)) {
                LOG(ERROR) << "firmware: truncated xz data";
                return false;
            }
            return true;
        }
        if (src_len == 0 && dest_len == 0) {
            LOG(ERROR) << "firmware: xz decompression made no progress";
            return false;
        }
    }
}

static void LoadFirmware(const std::string& firmware, const std::string& root, int fw_fd,
                         size_t fw_size, FirmwareCompression compression, int loading_fd,
                         int data_fd) {
    // Start transfer.
    WriteFully(loading_fd, "1", 1);

    // Copy the firmware.
    bool ok;
    if (compression == FirmwareCompression::kNone) {
        ok = sendfile(data_fd, fw_fd, nullptr, fw_size) != -1;
        if (!ok) {
            PLOG(ERROR) << "firmware: sendfile failed { '" << root << "', '" << firmware << "' }";
        }
    } else {
        std::string compressed;
        ok = ReadFdToString(fw_fd, &compressed);
        if (!ok) {
            PLOG(ERROR) << "firmware: read failed { '" << root << "', '" << firmware << "' }";
        } else {
            ok = compression == FirmwareCompression::kZstd ? DecompressZstd(compressed, data_fd)
                                                           : DecompressXz(compressed, data_fd);
            if (!ok) {
                LOG(ERROR) << "firmware: decompression failed { '" << root << "', '" << firmware
                           << "' }";
            }
        }
    }

    // Tell the firmware whether to abort or commit.
    const char* response = ok ? "0" : "-1";
    WriteFully(loading_fd, response, strlen(response));
}

ExternalFirmwareHandler::ExternalFirmwareHandler(std::string devpath, uid_t uid, gid_t gid,
                                                 std::string handler_path)
    : devpath(std::move(devpath)), uid(uid), gid(gid), handler_path(std::move(handler_path)) {
    auto wildcard_position = this->devpath.find('*');
    if (wildcard_position != std::string::npos) {
        if (wildcard_position == this->devpath.length() - 1) {
            this->devpath.pop_back();
            match = std::bind(PrefixMatch, this->devpath, std::placeholders::_1);
        } else {
            match = std::bind(FnMatch, this->devpath, std::placeholders::_1);
        }
    } else {
        match = std::bind(EqualMatch, this->devpath, std::placeholders::_1);
    }
}

ExternalFirmwareHandler::ExternalFirmwareHandler(std::string devpath, uid_t uid,
                                                 std::string handler_path)
    : ExternalFirmwareHandler(devpath, uid, 0, handler_path) {}

FirmwareHandler::FirmwareHandler(std::vector<std::string> firmware_directories,
                                 std::vector<ExternalFirmwareHandler> external_firmware_handlers,
                                 bool serial_handler_after_coldboot)
    : firmware_directories_(std::move(firmware_directories)),
      external_firmware_handlers_(std::move(external_firmware_handlers)),
      serial_handler_after_coldboot_(serial_handler_after_coldboot) {}

void FirmwareHandler::ColdbootDone() {
    if (serial_handler_after_coldboot_) {
        enables_parallel_handlers_ = false;
    }
}

std::string FirmwareHandler::GetFirmwarePath(const Uevent& uevent) const {
    for (const auto& external_handler : external_firmware_handlers_) {
        if (external_handler.match(uevent.path)) {
            LOG(INFO) << "Launching external firmware handler '" << external_handler.handler_path
                      << "' for devpath: '" << uevent.path << "' firmware: '" << uevent.firmware
                      << "'";

            std::unordered_map<std::string, std::string> envs_map;
            envs_map["FIRMWARE"] = uevent.firmware;
            envs_map["DEVPATH"] = uevent.path;

            auto result = RunExternalHandler(external_handler.handler_path, external_handler.uid,
                                             external_handler.gid, envs_map);
            if (!result.ok()) {
                LOG(ERROR) << "Using default firmware; External firmware handler failed: "
                           << result.error();
                return uevent.firmware;
            }
            if (result->find("..") != std::string::npos) {
                LOG(ERROR) << "Using default firmware; External firmware handler provided an "
                              "invalid path, '"
                           << *result << "'";
                return uevent.firmware;
            }
            LOG(INFO) << "Loading firmware '" << *result << "' in place of '" << uevent.firmware
                      << "'";
            return *result;
        }
    }
    LOG(INFO) << "firmware: loading '" << uevent.firmware << "' for '" << uevent.path << "'";
    return uevent.firmware;
}

void FirmwareHandler::ProcessFirmwareEvent(const std::string& path, const std::string& firmware,
                                           bool in_thread_pool, Timer t) const {
    std::string root = "/sys" + path;
    std::string loading = root + "/loading";
    std::string data = root + "/data";

    unique_fd loading_fd(open(loading.c_str(), O_WRONLY | O_CLOEXEC));
    if (loading_fd == -1) {
        PLOG(ERROR) << "couldn't open firmware loading fd for " << firmware;
        return;
    }

    unique_fd data_fd(open(data.c_str(), O_WRONLY | O_CLOEXEC));
    if (data_fd == -1) {
        PLOG(ERROR) << "couldn't open firmware data fd for " << firmware;
        return;
    }

    std::vector<std::string> attempted_paths_and_errors;
    auto TryLoadFirmware = [&](const std::string& firmware_directory) {
        for (const auto& [suffix, compression] : kFirmwareSuffixes) {
            std::string file = firmware_directory + firmware + suffix;
            unique_fd fw_fd(open(file.c_str(), O_RDONLY | O_CLOEXEC));
            if (fw_fd == -1) {
                attempted_paths_and_errors.emplace_back("firmware: attempted " + file +
                                                        ", open failed: " + strerror(errno));
                continue;
            }
            struct stat sb;
            if (fstat(fw_fd.get(), &sb) == -1) {
                attempted_paths_and_errors.emplace_back("firmware: attempted " + file +
                                                        ", fstat failed: " + strerror(errno));
                continue;
            }
            LOG(INFO) << "found " << file << " for " << path;
            LoadFirmware(firmware, root, fw_fd.get(), sb.st_size, compression, loading_fd.get(),
                         data_fd.get());
            return true;
        }
        return false;
    };

    if (ForEachFirmwareDirectory(TryLoadFirmware)) {
        LOG(INFO) << "loading " << path << " took " << t;
        return;
    }

    LOG(ERROR) << "firmware: could not find firmware for " << firmware;
    for (const auto& message : attempted_paths_and_errors) {
        LOG(ERROR) << message;
    }

    // Write "-1" as our response to the kernel's firmware request, since we have nothing for it.
    write(loading_fd.get(), "-1", 2);
}

static bool ShouldLookInBootstrapApexes() {
    auto status = GetProperty("apexd.status", "");
    if (status == "activated" || status == "ready") {
        return false;
    }
    return true;
}

bool FirmwareHandler::ForEachFirmwareDirectory(
        std::function<bool(const std::string&)> handler) const {
    for (const std::string& firmware_directory : firmware_directories_) {
        if (std::invoke(handler, firmware_directory)) {
            return true;
        }
    }

    std::string pattern = "/apex/*/etc/firmware/";
    // Let's look in bootstrap APEXes when APEXes are not ready in the default
    // mount namespace.
    if (ShouldLookInBootstrapApexes()) {
        pattern = "/bootstrap-apex/*/etc/firmware/";
    }

    glob_t glob_result;
    glob(pattern.c_str(), GLOB_MARK, nullptr, &glob_result);
    auto free_glob = android::base::make_scope_guard(std::bind(&globfree, &glob_result));
    for (size_t i = 0; i < glob_result.gl_pathc; i++) {
        char* apex_firmware_directory = glob_result.gl_pathv[i];
        // Filter-out /apex/<name>@<ver> paths. The paths are bind-mounted to
        // /apex/<name> paths, so unless we filter them out, we will look into the
        // same apex twice.
        if (strchr(apex_firmware_directory, '@')) {
            continue;
        }
        if (std::invoke(handler, apex_firmware_directory)) {
            return true;
        }
    }

    return false;
}

void FirmwareHandler::HandleUeventInternal(const Uevent& uevent, bool in_thread_pool) const {
    Timer t;
    auto firmware = GetFirmwarePath(uevent);
    ProcessFirmwareEvent(uevent.path, firmware, in_thread_pool, t);
}

void FirmwareHandler::HandleUevent(const Uevent& uevent) {
    if (uevent.subsystem != "firmware" || uevent.action != "add") return;

    if (enables_parallel_handlers_) {
        // Loading the firmware in a child means we can do that in parallel...
        auto pid = fork();
        if (pid == -1) {
            PLOG(ERROR) << "could not fork to process firmware event for " << uevent.firmware;
        } else if (pid == 0) {
            // Child does the actual work
            HandleUeventInternal(uevent, /*in_thread_pool=*/false);
            _exit(EXIT_SUCCESS);
        } else {
            // The main process returns here. Let the child do the actual work in parallel.
            return;
        }
    }
    HandleUeventInternal(uevent, /*in_thread_pool=*/false);
}

void FirmwareHandler::EnqueueUevent(const Uevent& uevent, ThreadPool& thread_pool) {
    if (uevent.subsystem != "firmware" || uevent.action != "add") return;

    thread_pool.Enqueue(kPriorityFirmware,
                        [&uevent, this] { HandleUeventInternal(uevent, /*in_thread_pool=*/true); });
}

}  // namespace init
}  // namespace android
