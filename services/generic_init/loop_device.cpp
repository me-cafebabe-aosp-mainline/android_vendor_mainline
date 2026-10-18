/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "loop_device.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <vector>

namespace LoopDevice {

using android::base::ParseUint;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::Trim;
using android::base::unique_fd;

std::string Attach(int fd, bool read_only, bool partscan, uint32_t block_size) {
    unique_fd control(TEMP_FAILURE_RETRY(open("/dev/loop-control", O_RDWR | O_CLOEXEC)));
    int number = control.get() == -1 ? -1 : ioctl(control.get(), LOOP_CTL_GET_FREE);
    if (number < 0) {
        PLOG(ERROR) << "No free loop device";
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
    struct loop_config config = {.fd = static_cast<uint32_t>(fd),
                                 .block_size = block_size,
                                 .info = {.lo_flags = (read_only ? LO_FLAGS_READ_ONLY : 0u) |
                                                      (partscan ? LO_FLAGS_PARTSCAN : 0u)}};
    if (loop_fd.get() == -1 || ioctl(loop_fd.get(), LOOP_CONFIGURE, &config) != 0) {
        PLOG(ERROR) << "Cannot configure " << path;
        return "";
    }
    return path;
}

}  // namespace LoopDevice
