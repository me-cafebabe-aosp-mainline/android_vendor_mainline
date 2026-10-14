/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ventoy_image.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace VentoyImage {

using android::base::ReadFileToString;
using android::base::unique_fd;

namespace {

// ventoy_os_param of include/grub/ventoy.h in Ventoy's GRUB, packed
#pragma pack(push, 1)
struct OsParam {
    uint8_t guid[16];
    uint8_t chksum;
    uint8_t disk_guid[16];
    uint64_t disk_size;
    uint16_t disk_part_id;
    uint16_t disk_part_type;
    char img_path[384];
    uint64_t img_size;
    uint64_t img_location_addr;
    uint32_t img_location_len;
    uint8_t reserved[32];
    uint8_t disk_signature[4];
    uint8_t reserved2[27];
};
#pragma pack(pop)

static_assert(sizeof(OsParam) == 512);

// "  www.ventoy.net"
constexpr uint8_t kVentoyGuid[16] = {0x20, 0x20, 0x77, 0x77, 0x77, 0x2e, 0x76, 0x65,
                                     0x6e, 0x74, 0x6f, 0x79, 0x2e, 0x6e, 0x65, 0x74};

// Set by Ventoy's EFI application before it boots the image, and left for the system
constexpr char kEfivars[] = "/sys/firmware/efi/efivars";
constexpr char kVariable[] =
        "/sys/firmware/efi/efivars/VentoyOsParam-77772020-2e77-6576-6e74-6f792e6e6574";
// efivarfs puts the attributes of the variable first
constexpr size_t kAttributesSize = 4;

OsParam os_param;
bool found = false;

}  // namespace

bool ReadParams() {
    if (access(kEfivars, F_OK) != 0) return false;
    // Mounted only for this: second stage init mounts it again
    bool mounted = mount("efivarfs", kEfivars, "efivarfs",
                         MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "") == 0;
    std::string data;
    bool read = ReadFileToString(kVariable, &data);
    if (mounted) umount(kEfivars);
    if (!read) return false;

    auto param = reinterpret_cast<const uint8_t*>(data.data()) + kAttributesSize;
    uint8_t sum = 0;
    if (data.size() >= kAttributesSize + sizeof(OsParam)) {
        for (size_t i = 0; i < sizeof(OsParam); i++) sum += param[i];
    }
    if (data.size() < kAttributesSize + sizeof(OsParam) ||
        memcmp(param, kVentoyGuid, sizeof(kVentoyGuid)) != 0 || sum != 0) {
        LOG(ERROR) << "Ventoy: invalid parameters in " << kVariable;
        return false;
    }
    memcpy(&os_param, param, sizeof(os_param));
    found = true;
    LOG(INFO) << "Ventoy: booted from " << Path() << " on partition " << PartitionNumber();
    return true;
}

bool IsOnDisk(const std::string& devpath) {
    if (!found) return false;
    unique_fd fd(TEMP_FAILURE_RETRY(open(devpath.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd.get() == -1) return false;
    uint8_t mbr[512];
    uint64_t size = 0;
    if (TEMP_FAILURE_RETRY(pread(fd.get(), mbr, sizeof(mbr), 0)) != sizeof(mbr) ||
        ioctl(fd.get(), BLKGETSIZE64, &size) != 0) {
        return false;
    }
    // Ventoy writes its own disk GUID at 0x180 of the first sector, GPT or MBR
    return size == os_param.disk_size &&
           memcmp(mbr + 0x180, os_param.disk_guid, sizeof(os_param.disk_guid)) == 0 &&
           memcmp(mbr + 0x1b8, os_param.disk_signature, sizeof(os_param.disk_signature)) == 0;
}

int PartitionNumber() {
    return os_param.disk_part_id;
}

std::string Path() {
    // Starts with "/"
    return std::string(os_param.img_path, strnlen(os_param.img_path, sizeof(os_param.img_path)));
}

}  // namespace VentoyImage
