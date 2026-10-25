/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "resize_fs.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ResizeFs {

using android::base::unique_fd;

namespace {

constexpr char kE2fsck[] = "/system/bin/e2fsck";
constexpr char kResize2fs[] = "/system/bin/resize2fs";
constexpr char kResizeF2fs[] = "/system/bin/resize.f2fs";

// A filesystem that is shorter than the device by less is not worth a check and a resize
constexpr uint64_t kMinGainBytes = 64ull << 20;

constexpr off_t kSuperblockOffset = 1024;
constexpr uint32_t kExt4Magic = 0xef53;
constexpr uint32_t kExt4Incompat64Bit = 0x80;
constexpr uint32_t kF2fsMagic = 0xf2f52010;

template <typename T>
T ReadLe(const uint8_t* buf, size_t offset) {
    T value;
    memcpy(&value, buf + offset, sizeof(value));
    return value;
}

// The size in bytes of the filesystem by its superblock, 0 if unknown
uint64_t FilesystemBytes(int fd, const std::string& fs_type) {
    uint8_t sb[0x200];
    if (TEMP_FAILURE_RETRY(pread(fd, sb, sizeof(sb), kSuperblockOffset)) !=
        static_cast<ssize_t>(sizeof(sb))) {
        return 0;
    }
    if (fs_type == "ext4") {
        if (ReadLe<uint16_t>(sb, 0x38) != kExt4Magic) return 0;
        uint64_t blocks = ReadLe<uint32_t>(sb, 0x04);
        if (ReadLe<uint32_t>(sb, 0x60) & kExt4Incompat64Bit) {
            blocks |= static_cast<uint64_t>(ReadLe<uint32_t>(sb, 0x150)) << 32;
        }
        return blocks << (10 + ReadLe<uint32_t>(sb, 0x18));
    }
    if (fs_type == "f2fs") {
        if (ReadLe<uint32_t>(sb, 0) != kF2fsMagic) return 0;
        return ReadLe<uint64_t>(sb, 36) << ReadLe<uint32_t>(sb, 16);
    }
    return 0;
}

// Runs the program like the first stage console does (posix_spawn() with no environment), logs
// what it prints, returns its exit status or -1
int Run(const std::vector<std::string>& args) {
    // We need to wait for our child, so don't use SA_NOCLDWAIT
    struct sigaction chld_act{};
    chld_act.sa_handler = SIG_DFL;
    sigemptyset(&chld_act.sa_mask);
    sigaction(SIGCHLD, &chld_act, nullptr);

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        PLOG(ERROR) << "pipe2() failed";
        return -1;
    }
    unique_fd read_end(fds[0]), write_end(fds[1]);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDERR_FILENO);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const char* envp[] = {nullptr};
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(),
                         const_cast<char* const*>(envp));
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        errno = rc;
        PLOG(ERROR) << "Failed to spawn '" << args[0] << "'";
        return -1;
    }
    write_end.reset();

    std::string output;
    char buf[512];
    ssize_t n;
    while ((n = TEMP_FAILURE_RETRY(read(read_end.get(), buf, sizeof(buf)))) > 0) {
        output.append(buf, n);
    }
    // The tools update a progress line with carriage returns
    for (auto& c : output) {
        if (c == '\r') c = '\n';
    }
    for (const auto& line : android::base::Split(output, "\n")) {
        if (!android::base::Trim(line).empty()) LOG(INFO) << args[0] << ": " << line;
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

void GrowToDevice(const std::string& devpath, const std::string& fs_type) {
    if (fs_type != "ext4" && fs_type != "f2fs") return;

    unique_fd fd(TEMP_FAILURE_RETRY(open(devpath.c_str(), O_RDONLY | O_CLOEXEC)));
    uint64_t device_bytes = 0;
    if (fd.get() == -1 || ioctl(fd.get(), BLKGETSIZE64, &device_bytes) != 0) {
        PLOG(ERROR) << "Cannot get the size of " << devpath;
        return;
    }
    uint64_t fs_bytes = FilesystemBytes(fd.get(), fs_type);
    fd.reset();
    if (fs_bytes == 0 || device_bytes < fs_bytes + kMinGainBytes) return;

    LOG(INFO) << "The " << fs_type << " filesystem of " << devpath << " is " << (fs_bytes >> 20)
              << " MiB on a device of " << (device_bytes >> 20) << " MiB, growing it";
    if (fs_type == "ext4") {
        for (const char* tool : {kE2fsck, kResize2fs}) {
            if (access(tool, X_OK) != 0) {
                LOG(WARNING) << tool << " not found, not growing " << devpath;
                return;
            }
        }
        // resize2fs wants a filesystem that was just checked. 0 and 1: no errors, or corrected.
        int status = Run({kE2fsck, "-f", "-y", devpath});
        if (status != 0 && status != 1) {
            LOG(ERROR) << "e2fsck of " << devpath << " failed with " << status
                       << ", not growing it";
            return;
        }
        status = Run({kResize2fs, devpath});
        if (status != 0) LOG(ERROR) << "resize2fs of " << devpath << " failed with " << status;
    } else {
        if (access(kResizeF2fs, X_OK) != 0) {
            LOG(WARNING) << kResizeF2fs << " not found, not growing " << devpath;
            return;
        }
        int status = Run({kResizeF2fs, devpath});
        if (status != 0) LOG(ERROR) << "resize.f2fs of " << devpath << " failed with " << status;
    }
}

}  // namespace ResizeFs
