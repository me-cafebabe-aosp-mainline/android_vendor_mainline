/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "iso_file.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <vector>

namespace IsoFile {

using android::base::EndsWithIgnoreCase;
using android::base::ParseUint;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::StringPrintf;
using android::base::Trim;
using android::base::unique_fd;

namespace {

constexpr char kMountTarget[] = "/mnt/vendor/generic_init/iso";

// Bounds of the search, as it walks every partition without an Android directory: images are
// usually near the root, and a system partition holds many files
constexpr int kMaxDepth = 3;
constexpr int kMaxEntries = 20000;

// A loop device without waiting for its node: generic_init is ueventd, and runs this while
// handling a uevent
std::string AttachLoop(const std::string& image) {
    unique_fd image_fd(TEMP_FAILURE_RETRY(open(image.c_str(), O_RDONLY | O_CLOEXEC)));
    if (image_fd.get() == -1) {
        PLOG(ERROR) << "Cannot open " << image;
        return "";
    }
    unique_fd control(TEMP_FAILURE_RETRY(open("/dev/loop-control", O_RDWR | O_CLOEXEC)));
    int number = control.get() == -1 ? -1 : ioctl(control.get(), LOOP_CTL_GET_FREE);
    if (number < 0) {
        PLOG(ERROR) << "No free loop device for " << image;
        return "";
    }
    std::string name = "loop" + std::to_string(number);
    std::string dev;
    std::vector<std::string> numbers;
    if (ReadFileToString("/sys/block/" + name + "/dev", &dev)) numbers = Split(Trim(dev), ":");
    unsigned int major, minor;
    std::string path = "/dev/block/" + name;
    if (numbers.size() != 2 || !ParseUint(numbers[0], &major) || !ParseUint(numbers[1], &minor) ||
        (mknod(path.c_str(), S_IFBLK | 0600, makedev(major, minor)) != 0 && errno != EEXIST)) {
        PLOG(ERROR) << "Cannot create " << path;
        return "";
    }
    unique_fd loop_fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
    struct loop_config config = {.fd = static_cast<uint32_t>(image_fd.get()),
                                 .info = {.lo_flags = LO_FLAGS_READ_ONLY}};
    if (loop_fd.get() == -1 || ioctl(loop_fd.get(), LOOP_CONFIGURE, &config) != 0) {
        PLOG(ERROR) << "Cannot attach " << image << " to " << path;
        return "";
    }
    return path;
}

std::string FindIn(const std::string& dir, const std::string& relative, const std::string& uuid,
                   int depth, int* entries) {
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir((dir + relative).c_str()), closedir);
    if (!d) return "";
    std::vector<std::string> subdirs;
    while (struct dirent* entry = readdir(d.get())) {
        if (++*entries > kMaxEntries) return "";
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string path = relative + "/" + name;
        unsigned char type = entry->d_type;
        struct stat st;
        if (type == DT_UNKNOWN && lstat((dir + path).c_str(), &st) == 0) {
            type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        if (type == DT_DIR) {
            subdirs.push_back(path);
        } else if (type == DT_REG && EndsWithIgnoreCase(name, ".iso") &&
                   ReadUuid(dir + path) == uuid) {
            return path;
        }
    }
    if (depth >= kMaxDepth) return "";
    for (const auto& subdir : subdirs) {
        std::string found = FindIn(dir, subdir, uuid, depth + 1, entries);
        if (!found.empty()) return found;
    }
    return "";
}

}  // namespace

std::string ReadUuid(const std::string& path) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd.get() == -1) return "";
    // The primary volume descriptor, in sector 16. Its modification date, 16 digits at 830, is
    // what GRUB and blkid show as the UUID.
    char pvd[2048];
    if (TEMP_FAILURE_RETRY(pread(fd.get(), pvd, sizeof(pvd), 16 * 2048)) != sizeof(pvd) ||
        pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        return "";
    }
    const char* d = pvd + 830;
    static const char kNoDate[16] = {};
    if (memcmp(d, kNoDate, sizeof(kNoDate)) == 0) return "";
    return StringPrintf("%.4s-%.2s-%.2s-%.2s-%.2s-%.2s-%.2s", d, d + 4, d + 6, d + 8, d + 10,
                        d + 12, d + 14);
}

std::string Find(const std::string& dir, const std::string& uuid) {
    int entries = 0;
    return FindIn(dir, "", uuid, 0, &entries);
}

std::string Attach(const std::string& partition_devpath, const std::string& fs_type,
                   const std::string& path) {
    mkdir(kMountTarget, 0755);
    if (mount(partition_devpath.c_str(), kMountTarget, fs_type.c_str(), MS_RDONLY, "") != 0) {
        PLOG(ERROR) << "Cannot mount " << partition_devpath << " for " << path;
        return "";
    }
    std::string image = kMountTarget + path;
    std::string loop = AttachLoop(image);
    if (loop.empty()) {
        umount(kMountTarget);
        return "";
    }
    LOG(INFO) << "Attached " << path << " of " << partition_devpath << " to " << loop;
    return loop;
}

}  // namespace IsoFile
