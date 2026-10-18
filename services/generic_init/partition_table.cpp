/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "partition_table.h"

#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>

namespace PartitionTable {

using android::base::unique_fd;

namespace {

constexpr uint32_t kSectorSizes[] = {512, 4096};
constexpr size_t kMbrTableOffset = 446;
constexpr size_t kMbrEntrySize = 16;
constexpr int kMbrEntries = 4;
constexpr uint8_t kMbrTypeProtective = 0xee;

uint32_t Le32(const uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

// Many things end with 0x55 0xAA, so the entries must make sense: this is only asked of a device
// with no filesystem found, but one with a filesystem we do not know is not rare
bool ValidMbr(const uint8_t* sector, uint64_t sectors) {
    if (sector[510] != 0x55 || sector[511] != 0xaa) return false;
    int used = 0;
    for (int i = 0; i < kMbrEntries; i++) {
        const uint8_t* entry = sector + kMbrTableOffset + i * kMbrEntrySize;
        uint8_t status = entry[0], type = entry[4];
        uint64_t start = Le32(entry + 8), size = Le32(entry + 12);
        if (type == 0 && size == 0) continue;
        if ((status != 0 && status != 0x80) || type == 0 || size == 0 || start + size > sectors) {
            return false;
        }
        // Protective MBR of a GPT that is not there
        if (type != kMbrTypeProtective) used++;
    }
    return used > 0;
}

}  // namespace

uint32_t Find(const std::string& devpath) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(devpath.c_str(), O_RDONLY | O_CLOEXEC)));
    uint64_t size = 0;
    if (fd.get() == -1 || ioctl(fd.get(), BLKGETSIZE64, &size) != 0) return 0;
    uint8_t sector[4096];
    // The GPT header is the sector after the first, of whatever size the image was made for
    for (uint32_t sector_size : kSectorSizes) {
        if (TEMP_FAILURE_RETRY(pread(fd.get(), sector, 8, sector_size)) == 8 &&
            memcmp(sector, "EFI PART", 8) == 0) {
            return sector_size;
        }
    }
    if (TEMP_FAILURE_RETRY(pread(fd.get(), sector, 512, 0)) == 512 &&
        ValidMbr(sector, size / 512)) {
        return 512;
    }
    return 0;
}

}  // namespace PartitionTable
