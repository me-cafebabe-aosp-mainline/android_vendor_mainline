/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dynamic_mount_handler.h"
#include "dynamic_mount_helpers.h"
#include "iso_file.h"
#include "loop_device.h"
#include "partition_table.h"
#include "usb_gadget.h"
#include "util.h"
#include "ventoy_image.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <fs_mgr.h>
#include <fstab/fstab.h>
#include <libdm/loop_control.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

extern bool fs_mgr_get_boot_config(const std::string& key, std::string* out_val);

namespace android {
namespace fs_mgr {
bool ParseFstabFromString(const std::string& fstab_str, bool proc_mounts, Fstab* fstab_out);
}  // namespace fs_mgr
}  // namespace android

namespace DynamicMountHandler {

namespace fs = std::filesystem;
using namespace ::android::fs_mgr;
using android::base::EndsWithIgnoreCase;
using android::base::EqualsIgnoreCase;
using android::base::Join;
using android::base::ReadFileToString;
using android::base::Split;
using android::base::StartsWith;
using android::base::StartsWithIgnoreCase;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::dm::LoopControl;
using android::fs_mgr::ParseFstabFromString;
using android::init::mkdir_recursive;
using DynamicMountHelpers::TryAccessDir;
using DynamicMountHelpers::TryAccessFile;
using std::chrono_literals::operator""s;

namespace {

std::string config_android_dir;
// androidboot.android_dir_dev: the only device for the android dir, as UUID, PARTUUID or PARTLABEL
// and its value
std::string config_android_dir_dev_key;
std::string config_android_dir_dev_value;
// androidboot.android_iso_uuid: the UUID of the ISO image holding the android dir, when it may be
// a file of a partition
std::string config_android_iso_uuid;

enum class ImageType {
    UNKNOWN = 0,
    FIRMWARE = 1,
    INITRD = 2,
    SYSTEM = 3,
    USERDATA = 4,
    OVERLAY = 5,
};

struct BlockDeviceInfo {
    // uevent fields
    std::string devname;
    std::string devpath;
    std::string partname;
    std::string partuuid;
    // The whole disk: devname itself for a disk
    std::string disk;

    // uevent handler fields
    std::vector<std::string> links;

    // our fields
    bool is_partition;
    bool rw;
    std::string fs_type;
    std::string fs_uuid;
    std::string mountopts;

    std::string android_system_partition;
    std::vector<std::string> android_system_partition_have_subdirs;

    std::string have_android_dir;
    std::string have_firmware_dir;
    std::vector<std::pair<ImageType, std::string>> android_dir_have_images;
    std::vector<std::string> android_dir_have_subdirs;
    // The ISO image of androidboot.android_iso_uuid, as a file of this filesystem
    std::string have_iso_file;
};

// devname, BlockDeviceInfo
using BlockDevices = std::unordered_map<std::string, std::shared_ptr<BlockDeviceInfo>>;
std::shared_ptr<BlockDevices> block_devices;

bool need_android_dir = false;
bool need_firmware_dir = false;
bool need_mount_cache = false;
// An ISO image file is attached as a device, once
bool iso_file_attached = false;
// The drive Ventoy booted from, once seen
std::string ventoy_disk;
// Loop devices attached to a partition holding a partition table, with the disk of that partition
std::unordered_map<std::string, std::string> nested_table_disks;

constexpr char kMountOptsRo[] = "";
constexpr char kMountOptsRw[] = "discard";

Fstab addon_fstab;

std::shared_ptr<BlockDeviceInfo> block_device_for_android_dir = nullptr;
std::shared_ptr<BlockDeviceInfo> block_device_for_firmware_dir = nullptr;

const std::list<std::string> fs_types_for_firmware = {"ext4", "f2fs", "erofs", "squashfs", "vfat"};

const std::list<std::string> fs_types_for_system_partitions = {"ext4", "erofs", "squashfs"};

const std::list<std::string> fs_types_for_userdata_partitions = {"ext4", "f2fs"};

const std::list<std::string> android_system_partitions = {"product",    "system",     "system_dlkm",
                                                          "system_ext", "odm",        "odm_dlkm",
                                                          "vendor",     "vendor_dlkm"};

const std::list<std::string> android_userdata_partitions = {"cache", "userdata", "metadata"};

const std::list<std::string> possible_subdirs_in_system = {"product", "system_dlkm", "system_ext",
                                                           "vendor"};

const std::list<std::string> possible_subdirs_in_vendor = {"odm", "odm_dlkm", "vendor_dlkm"};

const std::list<std::string> possible_firmware_image_names = {
        "firmware",
        "linux-firmware",
};

const std::list<std::string> possible_initrd_image_names = {
        "initrd",
        "ramdisk",
};

// Devices found for a slot, in uevent order. PickDevice() chooses one.
using Candidates = std::vector<std::shared_ptr<BlockDeviceInfo>>;

std::map<std::string, Candidates> android_system_part_candidates = {
        {"system", {}},  {"vendor", {}},

        {"product", {}}, {"system_dlkm", {}}, {"system_ext", {}},

        {"odm", {}},     {"odm_dlkm", {}},    {"vendor_dlkm", {}},
};

// Overlay partitions found, by the system partition they are for (all optional)
std::map<std::string, Candidates> android_overlay_part_candidates;

std::map<std::string, Candidates> android_userdata_part_candidates = {
        {"cache", {}},
        {"metadata", {}},
        {"userdata", {}},
};

constexpr char kAddonFstabSuffixParam[] = "addon_fstab_suffix";
constexpr char kAndroidDirParam[] = "android_dir";
constexpr char kAndroidDirDevParam[] = "android_dir_dev";
constexpr char kAndroidIsoUuidParam[] = "android_iso_uuid";
constexpr char kMountFirmwareParam[] = "mount_firmware";
constexpr char kMountOverlayParam[] = "mount_overlay";
constexpr char kMountSystemParam[] = "mount_system";
constexpr char kMountUserdataParam[] = "mount_userdata";

enum class MountFirmwareParam {
    ALL_POSSIBLE_DIRS = 0,
    DISABLE = 1,
    ONLY_ANDROID_DIR = 2,
    IMAGE = 3,
    IMAGE_COPY_TO_RAM = 4,
};

enum class MountOverlayParam {
    DISABLE = 0,
    STANDARD_PARTITIONS_WITH_PARTNAME = 1,
    IMAGES = 2,
    IMAGES_COPY_TO_RAM = 3,
    DIRS = 4,
};

enum class MountSystemParam {
    STANDARD_PARTITIONS_WITH_PARTNAME = 0,
    ANY_BLOCK_DEVICES_AS_PARTITION = 1,
    IMAGES = 2,
    IMAGES_COPY_TO_RAM = 3,
};

enum class MountUserdataParam {
    STANDARD_PARTITIONS_WITH_PARTNAME = 0,
    IMAGES = 1,
    BIND_MOUNT_DIR = 2,
    TMPFS = 3,
};

const std::unordered_map<std::string, MountFirmwareParam> kStringToMountFirmwareParamMap = {
        {"all_possible_dirs", MountFirmwareParam::ALL_POSSIBLE_DIRS},
        {"disable", MountFirmwareParam::DISABLE},
        {"false", MountFirmwareParam::DISABLE},
        {"img", MountFirmwareParam::IMAGE},
        {"img_ram", MountFirmwareParam::IMAGE_COPY_TO_RAM},
        {"only_android_dir", MountFirmwareParam::ONLY_ANDROID_DIR},
        {"true", MountFirmwareParam::ALL_POSSIBLE_DIRS},
};

const std::unordered_map<std::string, MountOverlayParam> kStringToMountOverlayParamMap = {
        {"disable", MountOverlayParam::DISABLE},
        {"std_parts", MountOverlayParam::STANDARD_PARTITIONS_WITH_PARTNAME},
        {"imgs", MountOverlayParam::IMAGES},
        {"imgs_ram", MountOverlayParam::IMAGES_COPY_TO_RAM},
        {"dirs", MountOverlayParam::DIRS},
};

const std::unordered_map<std::string, MountSystemParam> kStringToMountSystemParamMap = {
        {"std_parts", MountSystemParam::STANDARD_PARTITIONS_WITH_PARTNAME},
        {"blk_devices", MountSystemParam::ANY_BLOCK_DEVICES_AS_PARTITION},
        {"imgs", MountSystemParam::IMAGES},
        {"imgs_ram", MountSystemParam::IMAGES_COPY_TO_RAM},
};

const std::unordered_map<std::string, MountUserdataParam> kStringToMountUserdataParamMap = {
        {"std_parts", MountUserdataParam::STANDARD_PARTITIONS_WITH_PARTNAME},
        {"imgs", MountUserdataParam::IMAGES},
        {"bind_mount_dir", MountUserdataParam::BIND_MOUNT_DIR},
        {"tmpfs", MountUserdataParam::TMPFS},
};

MountFirmwareParam param_mount_firmware = MountFirmwareParam::ALL_POSSIBLE_DIRS;
MountOverlayParam param_mount_overlay = MountOverlayParam::DISABLE;
MountSystemParam param_mount_system = MountSystemParam::STANDARD_PARTITIONS_WITH_PARTNAME;
MountUserdataParam param_mount_userdata = MountUserdataParam::STANDARD_PARTITIONS_WITH_PARTNAME;

constexpr char kFirmwareFstabMountPoint[] = "/mnt/vendor/firmware";

const std::string kAddonFstabPrefix = "fstab.generic_init.addon.";
const std::string kAndroidMountTarget = "/mnt/vendor/generic_init/android";
const std::string kFirmwareMountTarget = "/mnt/vendor/generic_init/firmware";
const std::string kOverlayMountTarget = "/mnt/vendor/generic_init/overlay";
const std::string kTmpfsImgDir = "/mnt/vendor/generic_init/img";
const std::string kTryMountTarget = "/mnt/vendor/generic_init/try";
const std::string kImageSuffix = ".img";
const std::string kOverlaySuffix = "_overlay";

// How long to wait, after everything required was found, for overlay partitions
// (mount_overlay=std_parts), whose existence is unknown, and for partitions on the disk of the
// android dir, when only another disk had them so far
constexpr auto kPartitionsWait = 5s;
std::chrono::steady_clock::time_point overlay_partitions_deadline;
std::chrono::steady_clock::time_point android_disk_deadline;

Fstab fstab_userdata_on_tmpfs = {{.blk_device = "cache",
                                  .mount_point = "/cache",
                                  .fs_type = "tmpfs",
                                  .fs_mgr_flags =
                                          {
                                                  .no_fail = true,
                                                  .first_stage_mount = true,
                                          }},
                                 {.blk_device = "data",
                                  .mount_point = "/data",
                                  .fs_type = "tmpfs",
                                  .fs_mgr_flags =
                                          {
                                                  .first_stage_mount = true,
                                          }},
                                 {.blk_device = "metadata",
                                  .mount_point = "/metadata",
                                  .fs_type = "tmpfs",
                                  .fs_mgr_flags = {
                                          .no_fail = true,
                                          .first_stage_mount = true,
                                  }}};

bool Contains(const std::list<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

// Whether images of the type are mounted from the android dir
bool MountsImages(ImageType type) {
    switch (type) {
        case ImageType::FIRMWARE:
            return param_mount_firmware == MountFirmwareParam::IMAGE ||
                   param_mount_firmware == MountFirmwareParam::IMAGE_COPY_TO_RAM;
        case ImageType::SYSTEM:
            return param_mount_system == MountSystemParam::IMAGES ||
                   param_mount_system == MountSystemParam::IMAGES_COPY_TO_RAM;
        case ImageType::USERDATA:
            return param_mount_userdata == MountUserdataParam::IMAGES;
        case ImageType::OVERLAY:
            return param_mount_overlay == MountOverlayParam::IMAGES ||
                   param_mount_overlay == MountOverlayParam::IMAGES_COPY_TO_RAM;
        default:
            return false;
    }
}

bool CopiesImagesToRam(ImageType type) {
    switch (type) {
        case ImageType::FIRMWARE:
            return param_mount_firmware == MountFirmwareParam::IMAGE_COPY_TO_RAM;
        case ImageType::SYSTEM:
            return param_mount_system == MountSystemParam::IMAGES_COPY_TO_RAM;
        case ImageType::OVERLAY:
            return param_mount_overlay == MountOverlayParam::IMAGES_COPY_TO_RAM;
        default:
            return false;
    }
}

bool MountsAnyImages() {
    return MountsImages(ImageType::FIRMWARE) || MountsImages(ImageType::SYSTEM) ||
           MountsImages(ImageType::USERDATA) || MountsImages(ImageType::OVERLAY);
}

std::string MountPointOf(const std::string& partition) {
    return partition == "userdata" ? "/data" : "/" + partition;
}

template <typename T>
void ReadModeParam(const char* name, const std::unordered_map<std::string, T>& values, T* out) {
    std::string value;
    if (!fs_mgr_get_boot_config(name, &value)) {
        LOG(INFO) << "Parameter " << name << " is unset";
    } else if (values.contains(value)) {
        *out = values.at(value);
    } else {
        LOG(ERROR) << "Parameter " << name << " value is invalid";
    }
}

// Copy of ReadFstabFromFileCommon() from libfstab/fstab.cpp
// In order to get rid of calling SkipMountingPartitions()
bool ReadFstabFromFstabFile(const std::string& path, Fstab* fstab) {
    std::string fstab_str;
    if (!android::base::ReadFileToString(path, &fstab_str, /* follow_symlinks = */ true)) {
        PLOG(ERROR) << __FUNCTION__ << "(): failed to read file: '" << path << "'";
        return false;
    }

    if (!ParseFstabFromString(fstab_str, false, fstab)) {
        LOG(ERROR) << __FUNCTION__ << "(): failed to load fstab from : '" << path << "'";
        return false;
    }

    return true;
}

void ParseConfig(void) {
    std::string tmp;

    if (fs_mgr_get_boot_config(kAddonFstabSuffixParam, &tmp)) {
        std::vector<std::string> config_addon_fstab_suffix = Split(tmp, ",");
        for (const auto& fstab_suffix : config_addon_fstab_suffix) {
            const std::string addon_fstab_name = kAddonFstabPrefix + fstab_suffix;
            Fstab tmp_fstab;
            if (!ReadFstabFromFstabFile("/system/etc/" + addon_fstab_name, &tmp_fstab)) {
                LOG(ERROR) << "Failed to load addon fstab " << addon_fstab_name;
                continue;
            }

            LOG(INFO) << "Importing addon fstab " << addon_fstab_name;
            for (auto& entry : tmp_fstab) {
                addon_fstab.push_back(std::move(entry));
            }
        }
    }
    fs_mgr_get_boot_config(kAndroidDirParam, &config_android_dir);
    fs_mgr_get_boot_config(kAndroidIsoUuidParam, &config_android_iso_uuid);

    if (fs_mgr_get_boot_config(kAndroidDirDevParam, &tmp)) {
        auto equal_sign = tmp.find('=');
        std::string key = equal_sign == std::string::npos ? "" : tmp.substr(0, equal_sign);
        if ((key == "UUID" || key == "PARTUUID" || key == "PARTLABEL") &&
            equal_sign + 1 < tmp.size()) {
            config_android_dir_dev_key = key;
            config_android_dir_dev_value = tmp.substr(equal_sign + 1);
        } else {
            LOG(ERROR) << "Parameter " << kAndroidDirDevParam << " value is invalid";
        }
    }

    ReadModeParam(kMountFirmwareParam, kStringToMountFirmwareParamMap, &param_mount_firmware);
    ReadModeParam(kMountOverlayParam, kStringToMountOverlayParamMap, &param_mount_overlay);
    ReadModeParam(kMountSystemParam, kStringToMountSystemParamMap, &param_mount_system);
    ReadModeParam(kMountUserdataParam, kStringToMountUserdataParamMap, &param_mount_userdata);

    need_firmware_dir = param_mount_firmware == MountFirmwareParam::ALL_POSSIBLE_DIRS ||
                        param_mount_firmware == MountFirmwareParam::ONLY_ANDROID_DIR;
    need_android_dir = MountsAnyImages() ||
                       param_mount_userdata == MountUserdataParam::BIND_MOUNT_DIR ||
                       param_mount_overlay == MountOverlayParam::DIRS;
}

// The system partition that name (of a partition, an image without suffix, or a directory) is
// the overlay for, or ""
std::string GetOverlayTarget(const std::string& name) {
    if (!name.ends_with(kOverlaySuffix)) return "";
    std::string partition = name.substr(0, name.size() - kOverlaySuffix.size());
    return Contains(android_system_partitions, partition) ? partition : "";
}

std::string DetermineAndroidPartitionFromBuildProp(const std::string& path) {
    std::string buf;
    if (!ReadFileToString(path, &buf)) {
        LOG(ERROR) << "Failed to read build.prop " << path;
        return "";
    }
    for (const auto& part : android_system_partitions) {
        std::string prop = "ro." + part + ".build.id=";
        if (buf.find(prop) != std::string::npos) return part;
    }
    LOG(ERROR) << "Failed to determine android partition from build.prop " << path;
    return "";
}

std::string TryMountAndReturnFsType(const std::string& devpath, unsigned long mountflags) {
    bool ro = mountflags & MS_RDONLY;
    int ret, save_errno;
    const std::list<std::string> try_mount_fs_types = {"ext4",  "f2fs", "erofs", "squashfs", "vfat",
                                                       "exfat", "ntfs", "ntfs3", "iso9660",  "udf"};
    for (const auto& try_fs_type : try_mount_fs_types) {
        errno = 0;
        ret = mount(devpath.c_str(), kTryMountTarget.c_str(), try_fs_type.c_str(), mountflags,
                    ro ? kMountOptsRo : kMountOptsRw);
        save_errno = errno;
        if (ret && (save_errno == EINVAL || save_errno == ENODEV)) continue;
        if (ret) {
            PLOG(ERROR) << "Unable to try to mount " << devpath;
            return "";
        } else {
            return try_fs_type;
        }
    }
    if (ret) {
        LOG(ERROR) << "No known filesystem detected for " << devpath;
        return "";
    }
    return "";
}

// The filesystem UUID of a device as blkid and GRUB show it, or "": EXT4, F2FS, EROFS (the UUID),
// FAT and exFAT (the volume ID, XXXX-XXXX), NTFS (the serial number, 16 hex digits)
std::string ReadFsUuid(const std::string& devpath, const std::string& fs_type) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(devpath.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd.get() == -1) return "";
    auto read_at = [&fd](off_t offset, void* buf, size_t len) {
        return TEMP_FAILURE_RETRY(pread(fd.get(), buf, len, offset)) == static_cast<ssize_t>(len);
    };
    auto uuid16 = [](const uint8_t* u) {
        return StringPrintf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                            u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
                            u[11], u[12], u[13], u[14], u[15]);
    };
    uint8_t buf[16];
    if (fs_type == "ext4" && read_at(1024 + 0x68, buf, 16)) return uuid16(buf);
    if (fs_type == "f2fs" && read_at(1024 + 0x6c, buf, 16)) return uuid16(buf);
    if (fs_type == "erofs" && read_at(1024 + 0x30, buf, 16)) return uuid16(buf);
    if (fs_type == "vfat") {
        // FAT32 has a sector count of 0 in the FAT12/16 field at 0x16
        uint8_t fat_sectors[2];
        if (!read_at(0x16, fat_sectors, 2)) return "";
        if (!read_at(fat_sectors[0] || fat_sectors[1] ? 0x27 : 0x43, buf, 4)) return "";
        return StringPrintf("%02X%02X-%02X%02X", buf[3], buf[2], buf[1], buf[0]);
    }
    if (fs_type == "exfat" && read_at(0x64, buf, 4)) {
        return StringPrintf("%02X%02X-%02X%02X", buf[3], buf[2], buf[1], buf[0]);
    }
    if ((fs_type == "ntfs" || fs_type == "ntfs3") && read_at(0x48, buf, 8)) {
        return StringPrintf("%02X%02X%02X%02X%02X%02X%02X%02X", buf[7], buf[6], buf[5], buf[4],
                            buf[3], buf[2], buf[1], buf[0]);
    }
    return "";
}

// Whether the device may hold the android dir: the one of androidboot.android_dir_dev, if set
bool IsAndroidDirDev(const BlockDeviceInfo& info) {
    const std::string& value = config_android_dir_dev_value;
    if (config_android_dir_dev_key == "UUID") return EqualsIgnoreCase(info.fs_uuid, value);
    if (config_android_dir_dev_key == "PARTUUID") return EqualsIgnoreCase(info.partuuid, value);
    if (config_android_dir_dev_key == "PARTLABEL") return info.partname == value;
    return true;
}

ImageType GetImageTypeFromFilename(const std::string& filename) {
    std::string name = filename.substr(0, filename.find_last_of('.'));

    if (Contains(possible_firmware_image_names, name)) return ImageType::FIRMWARE;
    if (Contains(possible_initrd_image_names, name)) return ImageType::INITRD;
    if (Contains(android_system_partitions, name)) return ImageType::SYSTEM;
    if (Contains(android_userdata_partitions, name)) return ImageType::USERDATA;
    if (!GetOverlayTarget(name).empty()) return ImageType::OVERLAY;
    return ImageType::UNKNOWN;
}

void ParseBlockDevice_CheckDirs(BlockDeviceInfo* info) {
    std::string& devpath = info->devpath;

    if (!info->android_system_partition.empty()) return;

    // Only the configured one when set: another installation or media must not be used instead
    std::list<std::string> possible_android_dirs = {"android", "boot/android"};
    if (!config_android_dir.empty()) possible_android_dirs = {config_android_dir};
    for (const auto& android_dir : possible_android_dirs) {
        if (android_dir.empty()) continue;
        if (TryAccessDir(kTryMountTarget + "/" + android_dir)) {
            LOG(INFO) << "Block device " << devpath << " have android directory: " << android_dir;
            info->have_android_dir = android_dir;
            break;
        }
    }

    if (info->have_android_dir.empty() && info->have_iso_file.empty() &&
        !config_android_iso_uuid.empty() && !iso_file_attached && info->fs_type != "iso9660" &&
        info->fs_type != "udf") {
        info->have_iso_file = IsoFile::Find(kTryMountTarget, config_android_iso_uuid);
        if (!info->have_iso_file.empty()) {
            LOG(INFO) << "Block device " << devpath << " have the ISO image "
                      << info->have_iso_file;
        }
    }

    if (!info->have_android_dir.empty()) {
        for (const auto& entry :
             fs::directory_iterator(kTryMountTarget + "/" + info->have_android_dir)) {
            std::string name = entry.path().filename();
            if (entry.is_regular_file() && EndsWithIgnoreCase(name, kImageSuffix)) {
                info->android_dir_have_images.push_back({GetImageTypeFromFilename(name), name});
            } else if (entry.is_directory() && name != "firmware") {
                info->android_dir_have_subdirs.push_back(name);
            }
        }
    }

    std::list<std::string> possible_firmware_dirs = {
            info->have_android_dir.empty() ? "" : info->have_android_dir + "/firmware",
            "usr/lib/firmware", "lib/firmware", "linux-firmware", "firmware"};
    for (const auto& fw_dir : possible_firmware_dirs) {
        if (fw_dir.empty()) continue;
        if (TryAccessDir(kTryMountTarget + "/" + fw_dir)) {
            LOG(INFO) << "Block device " << devpath << " have firmware directory: " << fw_dir;
            info->have_firmware_dir = fw_dir;
            break;
        }
        if (param_mount_firmware == MountFirmwareParam::ONLY_ANDROID_DIR) {
            // android dir is the first one in the list
            break;
        }
    }
}

bool ParseApfsBlockDevice(BlockDeviceInfo* info) {
    bool success = false;
    int ret, save_errno;
    unsigned int apfs_vol = 0;
    unsigned long mountflags = MS_RDONLY;
    std::string mountopts;
    std::string& devpath = info->devpath;

    for (apfs_vol = 0; apfs_vol < 20; ++apfs_vol) {
        errno = 0;
        mountopts = "vol=" + std::to_string(apfs_vol);
        ret = mount(devpath.c_str(), kTryMountTarget.c_str(), "apfs", mountflags,
                    mountopts.c_str());
        save_errno = errno;
        if (ret) {
            if (save_errno == ENODEV) {
                LOG(ERROR) << "APFS filesystem is likely unsupported";
                return false;
            }

            // In this case, the filesystem is likely not APFS
            if (save_errno == EINVAL && apfs_vol == 0) return false;

            PLOG(ERROR) << "Unable to try to mount APFS filesystem"
                        << " volume " << std::to_string(apfs_vol) << " from " << devpath;
        } else {
            LOG(INFO) << "Mounted APFS filesystem volume " << std::to_string(apfs_vol) << " from "
                      << devpath;

            ParseBlockDevice_CheckDirs(info);
            success = !info->have_android_dir.empty();
            if (success) break;

            if (umount(kTryMountTarget.c_str()) == -1) {
                PLOG(FATAL) << "Failed to umount " << kTryMountTarget << " from block device "
                            << devpath;
            }
            info->have_firmware_dir.clear();
            continue;
        }
    }

    if (!success) return false;

    info->fs_type = "apfs";
    info->mountopts = "vol=" + std::to_string(apfs_vol);
    info->rw = false;

    // Cleanup and exit
    if (umount(kTryMountTarget.c_str()) == -1) {
        PLOG(FATAL) << "Failed to umount " << kTryMountTarget << " from block device " << devpath;
    }
    return success;
}

void ParseBlockDevice(BlockDeviceInfo* info) {
    unsigned long mountflags = MS_RDONLY;
    std::string tmp_path;
    std::string& devpath = info->devpath;

    // Mount as RO, and fill fs_type
    info->fs_type = TryMountAndReturnFsType(devpath, mountflags);
    if (info->fs_type.empty()) return;
    info->fs_uuid = ReadFsUuid(devpath, info->fs_type);
    if (!info->fs_uuid.empty()) {
        LOG(INFO) << "Block device " << devpath << " has filesystem UUID " << info->fs_uuid;
    }

    // Check the contents
    if (TryAccessFile(kTryMountTarget + "/system/build.prop")) {
        info->android_system_partition = "system";
    } else {
        const std::list<std::string> possible_build_props = {"build.prop", "etc/build.prop"};
        for (const auto& build_prop : possible_build_props) {
            std::string build_prop_path = kTryMountTarget + "/" + build_prop;
            if (TryAccessFile(build_prop_path)) {
                info->android_system_partition =
                        DetermineAndroidPartitionFromBuildProp(build_prop_path);
            }
        }
    }

    if (!info->android_system_partition.empty()) {
        LOG(INFO) << "Block device " << devpath << " detected as system partition "
                  << info->android_system_partition;
        if (info->android_system_partition == "system") {
            for (const auto& subdir : possible_subdirs_in_system) {
                tmp_path = kTryMountTarget + "/system/" + subdir;
                if (TryAccessDir(tmp_path)) {
                    info->android_system_partition_have_subdirs.push_back(subdir);
                }
            }
            for (const auto& subdir : possible_subdirs_in_vendor) {
                tmp_path = kTryMountTarget + "/system/vendor/" + subdir;
                if (TryAccessDir(tmp_path)) {
                    info->android_system_partition_have_subdirs.push_back(subdir);
                }
            }
            if (TryAccessDir(kTryMountTarget + "/cache")) {
                LOG(INFO) << "/cache is a directory, will need to be mounted";
                need_mount_cache = true;
            }
        } else if (info->android_system_partition == "vendor") {
            for (const auto& subdir : possible_subdirs_in_vendor) {
                tmp_path = kTryMountTarget + "/" + subdir;
                if (TryAccessDir(tmp_path)) {
                    info->android_system_partition_have_subdirs.push_back(subdir);
                }
            }
        }
        if (!info->android_system_partition_have_subdirs.empty()) {
            LOG(INFO) << "Block device " << devpath << " have subdirs: "
                      << Join(info->android_system_partition_have_subdirs, ", ");
        }
    }

    ParseBlockDevice_CheckDirs(info);

    // Try remount as RW
    mountflags &= ~MS_RDONLY;
    mountflags |= MS_REMOUNT;
    info->rw = mount(NULL, kTryMountTarget.c_str(), NULL, mountflags, kMountOptsRw) == 0;
    LOG(INFO) << "Block device " << devpath << " can be mounted as "
              << (info->rw ? "read-write" : "read-only");

    info->mountopts = info->rw ? kMountOptsRw : kMountOptsRo;

    // Cleanup and exit
    if (umount(kTryMountTarget.c_str()) == -1) {
        PLOG(FATAL) << "Failed to umount " << kTryMountTarget << " from block device " << devpath;
    }
}

// The device for a slot: the last one found on the disk of the android dir, since another
// installation may be on another disk. Otherwise, unless android_disk_only, the last one found.
std::shared_ptr<BlockDeviceInfo> PickDevice(const Candidates& candidates, bool android_disk_only) {
    const auto& android = block_device_for_android_dir;
    if (android != nullptr) {
        auto it = std::find_if(candidates.rbegin(), candidates.rend(),
                               [&](const auto& c) { return c->disk == android->disk; });
        if (it != candidates.rend()) return *it;
        if (android_disk_only) return nullptr;
    }
    return candidates.empty() ? nullptr : candidates.back();
}

void WarnIfOtherDisk(const Candidates& candidates, const std::string& what) {
    if (PickDevice(candidates, true) != nullptr) return;
    LOG(WARNING) << "Using " << PickDevice(candidates, false)->devname << " for " << what
                 << ", on another disk than the android dir";
}

void AddSystemPartition(const std::string& partition, std::shared_ptr<BlockDeviceInfo> bdinfo) {
    LOG(INFO) << "Found block device " << bdinfo->devname << " for system partition " << partition;
    android_system_part_candidates[partition].push_back(bdinfo);
    for (const auto& subdir : bdinfo->android_system_partition_have_subdirs) {
        LOG(INFO) << "Found block device " << bdinfo->devname << " for system partition subdir "
                  << subdir;
        android_system_part_candidates[subdir].push_back(bdinfo);
    }
}

void UpdateMountInfo(std::shared_ptr<BlockDeviceInfo> bdinfo) {
    // Overlay partitions are nothing else, even with a build.prop in them
    if (!GetOverlayTarget(bdinfo->partname).empty()) {
        if (param_mount_overlay != MountOverlayParam::STANDARD_PARTITIONS_WITH_PARTNAME) return;
        std::string partition = GetOverlayTarget(bdinfo->partname);
        LOG(INFO) << "Found block device " << bdinfo->devname
                  << " for the overlay of system partition " << partition;
        android_overlay_part_candidates[partition].push_back(bdinfo);
        return;
    }

    if (param_mount_system == MountSystemParam::STANDARD_PARTITIONS_WITH_PARTNAME &&
        Contains(android_system_partitions, bdinfo->partname)) {
        AddSystemPartition(bdinfo->partname, bdinfo);
        return;
    }

    if (param_mount_system == MountSystemParam::ANY_BLOCK_DEVICES_AS_PARTITION &&
        Contains(android_system_partitions, bdinfo->android_system_partition)) {
        AddSystemPartition(bdinfo->android_system_partition, bdinfo);
        return;
    }

    if (param_mount_userdata == MountUserdataParam::STANDARD_PARTITIONS_WITH_PARTNAME &&
        Contains(android_userdata_partitions, bdinfo->partname)) {
        LOG(INFO) << "Found block device " << bdinfo->devname << " for userdata partition "
                  << bdinfo->partname;
        android_userdata_part_candidates[bdinfo->partname].push_back(bdinfo);
        return;
    }

    if (need_android_dir && !bdinfo->have_android_dir.empty() && !IsAndroidDirDev(*bdinfo)) {
        LOG(INFO) << "Ignoring android dir " << bdinfo->have_android_dir << " of "
                  << bdinfo->devname << ", not " << config_android_dir_dev_key << "="
                  << config_android_dir_dev_value;
    } else if (need_android_dir && !bdinfo->have_android_dir.empty()) {
        LOG(INFO) << "Set block device with android dir to " << bdinfo->devname;
        block_device_for_android_dir = bdinfo;
    }
    if (need_firmware_dir && !bdinfo->have_firmware_dir.empty()) {
        LOG(INFO) << "Set block device with firmware dir to " << bdinfo->devname;
        block_device_for_firmware_dir = bdinfo;
    }
}

std::string SetupLoopDevice(const std::string& image, bool rw) {
    unique_fd image_fd(TEMP_FAILURE_RETRY(
            open(image.c_str(), (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC, (rw ? 0600 : 0400))));
    if (image_fd.get() == -1) {
        PLOG(ERROR) << "Cannot open image path: " << image;
        return "";
    }

    LoopControl loop_control;
    std::string loop_device;
    if (!loop_control.Attach(image_fd.get(), 5s, &loop_device)) {
        return "";
    }

    unique_fd loop_fd(TEMP_FAILURE_RETRY(open(loop_device.c_str(), O_RDWR | O_CLOEXEC)));
    if (loop_fd.get() == -1) {
        PLOG(ERROR) << "Cannot open " << loop_device;
        return "";
    }

    struct loop_info64 info = {.lo_flags = 0};
    if (!rw) info.lo_flags |= LO_FLAGS_READ_ONLY;
    if (ioctl(loop_fd.get(), LOOP_SET_STATUS64, &info)) {
        PLOG(ERROR) << "Failed set loop flags for " << loop_device;
        return "";
    }

    LoopControl::EnableDirectIo(loop_fd.get());

    return loop_device;
}

// Mounts the overlay of a system partition read-only, returns the mount point, or "" on failure
std::string MountOverlaySource(const std::string& partition, const std::string& devpath) {
    std::string target = kOverlayMountTarget + "/" + partition;
    mkdir_recursive(target, 0755);
    for (const auto& fs_type : fs_types_for_system_partitions) {
        if (mount(devpath.c_str(), target.c_str(), fs_type.c_str(), MS_RDONLY, kMountOptsRo) == 0) {
            LOG(INFO) << "Mounted the overlay of system partition " << partition << " from "
                      << devpath << " as " << fs_type;
            return target;
        }
    }
    LOG(ERROR) << "Failed to mount the overlay of system partition " << partition << " from "
               << devpath << ", ignoring it";
    return "";
}

std::string SetupLoopDeviceOrDie(const std::string& image, bool rw) {
    std::string loop_device = SetupLoopDevice(image, rw);
    if (loop_device.empty()) LOG(FATAL) << "Failed to setup loop device for image " << image;
    return loop_device;
}

// Lower directory of the overlay of each system partition
using OverlayLowerdirs = std::map<std::string, std::string>;

// One entry per filesystem: MountPartitions() tries the entries of a mount point in order
void AddEntries(Fstab* fstab, FstabEntry entry, const std::list<std::string>& fs_types) {
    for (const auto& fs_type : fs_types) {
        entry.fs_type = fs_type;
        fstab->push_back(entry);
    }
}

void AddSystemEntries(Fstab* fstab, FstabEntry entry) {
    for (const auto& fs_type : fs_types_for_system_partitions) {
        entry.fs_type = fs_type;
        fstab->push_back(entry);
        // Fallback for a writable device with an EXT4 that only mounts read-only
        if (fs_type == "ext4" && !(entry.flags & MS_RDONLY)) {
            fstab->push_back(entry);
            fstab->back().flags |= MS_RDONLY;
        }
    }
}

void AddUserdataEntries(Fstab* fstab, FstabEntry entry) {
    // Only /data is required
    entry.fs_mgr_flags.no_fail = entry.mount_point != "/data";
    AddEntries(fstab, entry, fs_types_for_userdata_partitions);
}

// Returns the path of the android dir
std::string MountAndroidDir() {
    const auto& bdinfo = block_device_for_android_dir;
    if (mount(bdinfo->devpath.c_str(), kAndroidMountTarget.c_str(), bdinfo->fs_type.c_str(),
              bdinfo->rw ? 0 : MS_RDONLY, bdinfo->mountopts.c_str())) {
        PLOG(FATAL) << "Unable to mount block device for android dir";
    }
    return kAndroidMountTarget + "/" + bdinfo->have_android_dir;
}

void MountOverlayPartitions(OverlayLowerdirs* lowerdirs) {
    if (param_mount_overlay != MountOverlayParam::STANDARD_PARTITIONS_WITH_PARTNAME) return;
    for (const auto& [partition, candidates] : android_overlay_part_candidates) {
        // Optional: not from another disk than the android dir
        auto bdinfo = PickDevice(candidates, true);
        if (bdinfo == nullptr) continue;
        std::string lowerdir = MountOverlaySource(partition, bdinfo->devpath);
        if (!lowerdir.empty()) (*lowerdirs)[partition] = lowerdir;
    }
}

void AddSystemPartitionEntries(Fstab* fstab) {
    if (param_mount_system != MountSystemParam::STANDARD_PARTITIONS_WITH_PARTNAME &&
        param_mount_system != MountSystemParam::ANY_BLOCK_DEVICES_AS_PARTITION) {
        return;
    }
    for (const auto& [partition, candidates] : android_system_part_candidates) {
        auto bdinfo = PickDevice(candidates, false);
        // Subdirs are mounted with the partition holding them
        if (bdinfo == nullptr || partition != bdinfo->android_system_partition) continue;
        if (param_mount_system == MountSystemParam::STANDARD_PARTITIONS_WITH_PARTNAME) {
            WarnIfOtherDisk(candidates, "system partition " + partition);
        }
        FstabEntry entry = {.blk_device = bdinfo->devpath,
                            .mount_point = "/" + partition,
                            .fs_mgr_flags = {.first_stage_mount = true}};
        if (!bdinfo->rw) entry.flags |= MS_RDONLY;
        AddSystemEntries(fstab, entry);
    }
}

void AddUserdataPartitionEntries(Fstab* fstab) {
    if (param_mount_userdata != MountUserdataParam::STANDARD_PARTITIONS_WITH_PARTNAME) return;
    for (const auto& [partition, candidates] : android_userdata_part_candidates) {
        auto bdinfo = PickDevice(candidates, false);
        if (bdinfo == nullptr || (partition == "cache" && !need_mount_cache)) continue;
        WarnIfOtherDisk(candidates, "userdata partition " + partition);
        AddUserdataEntries(fstab, {.blk_device = bdinfo->devpath,
                                   .mount_point = MountPointOf(partition),
                                   .fs_mgr_flags = {.first_stage_mount = true}});
    }
}

void AddImageEntries(Fstab* fstab, const std::string& android_dir_path, bool is_recovery_mode,
                     OverlayLowerdirs* lowerdirs) {
    if (!MountsAnyImages()) return;
    const auto& bdinfo = block_device_for_android_dir;
    struct PartitionImage {
        ImageType type;
        std::string partition;
        std::string path;
    };
    std::vector<PartitionImage> partition_images;
    std::string firmware_image;

    for (const auto& [type, name] : bdinfo->android_dir_have_images) {
        // Recovery mode only mounts the firmware
        if (!MountsImages(type) || (is_recovery_mode && type != ImageType::FIRMWARE)) continue;
        std::string partition = name.substr(0, name.find_last_of('.'));
        std::string path = android_dir_path + "/" + name;

        if (CopiesImagesToRam(type)) {
            LOG(INFO) << "Copying image " << name << " to RAM";
            std::string copy = kTmpfsImgDir + "/" + name;
            std::error_code ec;
            fs::copy(path, copy, ec);
            if (ec) LOG(FATAL) << "Failed to copy image " << name << " to RAM";
            path = copy;
        }

        if (type == ImageType::FIRMWARE) {
            firmware_image = path;
        } else if (type == ImageType::OVERLAY) {
            std::string loop_device = SetupLoopDevice(path, false);
            if (loop_device.empty()) {
                LOG(ERROR) << "Failed to setup loop device for image " << path << ", ignoring it";
                continue;
            }
            std::string target = GetOverlayTarget(partition);
            std::string lowerdir = MountOverlaySource(target, loop_device);
            if (!lowerdir.empty()) (*lowerdirs)[target] = lowerdir;
        } else {
            partition_images.push_back({type, partition, path});
        }
    }

    if (MountsImages(ImageType::FIRMWARE)) {
        if (firmware_image.empty()) LOG(FATAL) << "Firmware image was not found";
        FstabEntry entry = {.blk_device = SetupLoopDeviceOrDie(firmware_image, bdinfo->rw),
                            .mount_point = kFirmwareFstabMountPoint,
                            .fs_mgr_flags = {.no_fail = true, .first_stage_mount = true}};
        if (!bdinfo->rw) entry.flags |= MS_RDONLY;
        AddEntries(fstab, entry, fs_types_for_firmware);
    }

    for (const auto& image : partition_images) {
        if (image.partition == "cache" && !need_mount_cache) continue;
        FstabEntry entry = {.blk_device = SetupLoopDeviceOrDie(image.path, bdinfo->rw),
                            .mount_point = MountPointOf(image.partition),
                            .fs_mgr_flags = {.first_stage_mount = true}};
        if (!bdinfo->rw) entry.flags |= MS_RDONLY;
        if (image.type == ImageType::USERDATA) {
            AddUserdataEntries(fstab, entry);
        } else {
            AddSystemEntries(fstab, entry);
        }
    }
}

void AddUserdataDirEntries(Fstab* fstab, const std::string& android_dir_path) {
    if (param_mount_userdata != MountUserdataParam::BIND_MOUNT_DIR) return;
    for (const auto& subdir : block_device_for_android_dir->android_dir_have_subdirs) {
        if (!Contains(android_userdata_partitions, subdir)) continue;
        if (subdir == "cache" && !need_mount_cache) continue;
        fstab->push_back({.blk_device = android_dir_path + "/" + subdir,
                          .mount_point = MountPointOf(subdir),
                          .fs_type = "none",
                          .flags = MS_BIND,
                          .fs_mgr_flags = {.first_stage_mount = true}});
    }
}

void UseOverlayDirs(const std::string& android_dir_path, OverlayLowerdirs* lowerdirs) {
    if (param_mount_overlay != MountOverlayParam::DIRS) return;
    for (const auto& subdir : block_device_for_android_dir->android_dir_have_subdirs) {
        std::string partition = GetOverlayTarget(subdir);
        if (partition.empty()) continue;
        LOG(INFO) << "Using directory " << subdir << " as the overlay of system partition "
                  << partition;
        (*lowerdirs)[partition] = android_dir_path + "/" + subdir;
    }
}

void AddUserdataTmpfsEntries(Fstab* fstab) {
    if (param_mount_userdata != MountUserdataParam::TMPFS) return;
    fstab->insert(fstab->end(), fstab_userdata_on_tmpfs.begin(), fstab_userdata_on_tmpfs.end());
}

void AddFirmwareDirEntry(Fstab* fstab) {
    if (!need_firmware_dir) return;
    // The android dir's device first: the Linux system it is installed beside, not another one
    auto bdinfo = block_device_for_firmware_dir;
    const auto& android = block_device_for_android_dir;
    if (android != nullptr && !android->have_firmware_dir.empty()) bdinfo = android;
    std::string firmware_dir_path;
    if (bdinfo == block_device_for_android_dir) {
        firmware_dir_path = kAndroidMountTarget + "/" + bdinfo->have_firmware_dir;
    } else {
        if (mount(bdinfo->devpath.c_str(), kFirmwareMountTarget.c_str(), bdinfo->fs_type.c_str(),
                  MS_RDONLY, bdinfo->mountopts.c_str())) {
            PLOG(FATAL) << "Unable to mount block device for firmware dir";
        }
        firmware_dir_path = kFirmwareMountTarget + "/" + bdinfo->have_firmware_dir;
    }
    fstab->push_back({.blk_device = firmware_dir_path,
                      .mount_point = kFirmwareFstabMountPoint,
                      .fs_type = "none",
                      .flags = MS_BIND,
                      .fs_mgr_flags = {.no_fail = true, .first_stage_mount = true}});
}

// Read-only, before the addon fstab entries: those for the same mount points go on top of them.
// Mounted after SwitchRoot(), the one of /system on the /system directory.
void AddOverlayEntries(Fstab* fstab, const OverlayLowerdirs& lowerdirs) {
    for (const auto& [partition, lowerdir] : lowerdirs) {
        fstab->push_back({.blk_device = partition + kOverlaySuffix,
                          .mount_point = "/" + partition,
                          .fs_type = "overlay",
                          .flags = MS_RDONLY,
                          .lowerdir = lowerdir + ":/" + partition,
                          .fs_mgr_flags = {.no_fail = true, .first_stage_mount = true}});
    }
}

// A partition with no filesystem that holds a partition table (a disk image written to it): its
// partitions are found through a loop device, as devices of the same disk
void AttachNestedPartitionTable(const std::string& devpath, const std::string& disk) {
    uint32_t sector_size = PartitionTable::Find(devpath);
    if (sector_size == 0) return;
    bool read_only = false;
    unique_fd fd(TEMP_FAILURE_RETRY(open(devpath.c_str(), O_RDWR | O_CLOEXEC)));
    if (fd.get() == -1) {
        read_only = true;
        fd.reset(TEMP_FAILURE_RETRY(open(devpath.c_str(), O_RDONLY | O_CLOEXEC)));
    }
    if (fd.get() == -1) {
        PLOG(ERROR) << "Cannot open " << devpath;
        return;
    }
    std::string loop = LoopDevice::Attach(fd.get(), read_only, true, sector_size);
    if (loop.empty()) return;
    LOG(INFO) << "Block device " << devpath << " has a partition table, attached to " << loop;
    nested_table_disks[fs::path(loop).filename()] = disk;
}

// The disk of the partition holding the table, for a partition found in one, otherwise ""
std::string NestedTableDisk(const android::init::Uevent& uevent) {
    auto it = nested_table_disks.find(fs::path(uevent.path).parent_path().filename());
    return it == nested_table_disks.end() ? "" : it->second;
}

// Probes the device and records what it can be used for
std::shared_ptr<BlockDeviceInfo> AddBlockDevice(BlockDeviceInfo info) {
    if (!ParseApfsBlockDevice(&info)) ParseBlockDevice(&info);
    auto bdinfo = std::make_shared<BlockDeviceInfo>(std::move(info));
    block_devices->insert({bdinfo->devpath, bdinfo});
    UpdateMountInfo(bdinfo);
    return bdinfo;
}

}  // namespace

void OnPreBlockDevices(void) {
    mkdir_recursive(kAndroidMountTarget, 0755);
    mkdir_recursive(kFirmwareMountTarget, 0755);
    mkdir_recursive(kOverlayMountTarget, 0755);
    mkdir_recursive(kTmpfsImgDir, 0755);
    mkdir_recursive(kTryMountTarget, 0755);
    block_devices = std::make_shared<BlockDevices>();
    ParseConfig();
    VentoyImage::ReadParams();
    UsbGadget::Init();
}

void OnBlockDeviceAdd(const android::init::Uevent& uevent, const std::string& devpath,
                      const std::vector<std::string>& links) {
    if (block_devices->contains(devpath)) {
        LOG(ERROR) << "Block device " << devpath << " has already been parsed";
        return;
    }

    std::string nested_disk = NestedTableDisk(uevent);
    static const std::list<std::string> kIgnoredDevnamePrefixs = {"dm-", "loop", "ram", "zram"};
    for (const auto& prefix : kIgnoredDevnamePrefixs) {
        if (nested_disk.empty() && StartsWith(uevent.device_name, prefix)) {
            return;
        }
    }

    BlockDeviceInfo info;
    info.devname = uevent.device_name;
    info.devpath = devpath;
    info.partname = uevent.partition_name;
    info.partuuid = uevent.partition_uuid;
    // DEVPATH of a partition is .../block/<disk>/<partition>
    std::string parent = fs::path(uevent.path).parent_path().filename();
    info.disk = !nested_disk.empty() ? nested_disk : parent == "block" ? info.devname : parent;
    info.links = links;
    // Not by the name: a disk can end with a digit (mmcblk0, nvme0n1, usbblk0)
    info.is_partition = info.disk != info.devname;
    // An ISO image booted as a file of this partition: a device of its own, like a CD. Ventoy
    // tells which under UEFI (a partition comes after its disk), otherwise its UUID is searched.
    if (!iso_file_attached && !ventoy_disk.empty() && info.disk == ventoy_disk &&
        uevent.partition_num == VentoyImage::PartitionNumber()) {
        info.have_iso_file = VentoyImage::Path();
    }
    auto bdinfo = AddBlockDevice(info);
    if (info.disk == info.devname && VentoyImage::IsOnDisk(devpath)) ventoy_disk = info.devname;
    // Not inside another one
    if (bdinfo->fs_type.empty() && info.is_partition && nested_disk.empty()) {
        AttachNestedPartitionTable(devpath, info.disk);
    }

    if (iso_file_attached) return;
    std::string iso_path = bdinfo->have_iso_file;
    if (iso_path.empty()) return;
    std::string loop = IsoFile::Attach(devpath, bdinfo->fs_type, iso_path);
    if (loop.empty()) return;
    iso_file_attached = true;
    AddBlockDevice({.devname = "iso", .devpath = loop, .disk = "iso"});
}

// TODO: Handle block device removal?

bool CanQuitUeventd(bool print_log) {
    bool ret = true;
    // Returns whether to look for more missing devices, to log them all
    auto missing = [&, function = __FUNCTION__](const std::string& what) {
        if (print_log) LOG(INFO) << function << ": Missing block device for " << what;
        ret = false;
        return print_log;
    };
    // Whether a slot found by name only has devices on another disk than the android dir. Not
    // for blk_devices, where every disk may hold one partition.
    bool other_disk = false;
    auto found = [&](const Candidates& candidates, const std::string& what, bool by_name) {
        if (PickDevice(candidates, false) == nullptr) return missing(what);
        if (by_name && PickDevice(candidates, true) == nullptr) other_disk = true;
        return true;
    };
    // Starts the wait at the first call, returns whether it is over
    auto waited = [&, function = __FUNCTION__](std::chrono::steady_clock::time_point* deadline,
                                               const std::string& what) {
        auto now = std::chrono::steady_clock::now();
        if (*deadline == std::chrono::steady_clock::time_point()) {
            LOG(INFO) << function << ": Waiting for " << what;
            *deadline = now + kPartitionsWait;
        }
        return now >= *deadline;
    };

    if (param_mount_system == MountSystemParam::STANDARD_PARTITIONS_WITH_PARTNAME ||
        param_mount_system == MountSystemParam::ANY_BLOCK_DEVICES_AS_PARTITION) {
        for (const auto& [part, candidates] : android_system_part_candidates) {
            if (!found(candidates, "system partition " + part,
                       param_mount_system == MountSystemParam::STANDARD_PARTITIONS_WITH_PARTNAME)) {
                return false;
            }
        }
    }
    if (param_mount_userdata == MountUserdataParam::STANDARD_PARTITIONS_WITH_PARTNAME) {
        for (const auto& [part, candidates] : android_userdata_part_candidates) {
            if (part == "cache" && !need_mount_cache) continue;
            if (!found(candidates, "userdata partition " + part, true)) return false;
        }
    }
    if (need_android_dir && block_device_for_android_dir == nullptr && !missing("android dir")) {
        return false;
    }
    if (need_firmware_dir && block_device_for_firmware_dir == nullptr && !missing("firmware dir")) {
        return false;
    }
    if (ret && other_disk &&
        !waited(&android_disk_deadline, "partitions on the disk of the android dir")) {
        return false;
    }
    if (ret && param_mount_overlay == MountOverlayParam::STANDARD_PARTITIONS_WITH_PARTNAME) {
        // Unless every system partition has one
        bool all_found = false;
        for (const auto& [part, candidates] : android_system_part_candidates) {
            auto bdev = PickDevice(candidates, false);
            if (bdev == nullptr || part != bdev->android_system_partition) continue;
            auto overlay = android_overlay_part_candidates.find(part);
            all_found = overlay != android_overlay_part_candidates.end() &&
                        PickDevice(overlay->second, true) != nullptr;
            if (!all_found) break;
        }
        if (!all_found && !waited(&overlay_partitions_deadline, "overlay partitions")) {
            return false;
        }
    }
    return ret;
}

void OnPostBlockDevices(bool is_recovery_mode) {
    Fstab fstab;
    OverlayLowerdirs overlay_lowerdirs;
    std::string android_dir_path = need_android_dir ? MountAndroidDir() : "";

    // Recovery mode only mounts the firmware
    if (!is_recovery_mode) {
        MountOverlayPartitions(&overlay_lowerdirs);
        AddSystemPartitionEntries(&fstab);
        AddUserdataPartitionEntries(&fstab);
    }
    AddImageEntries(&fstab, android_dir_path, is_recovery_mode, &overlay_lowerdirs);
    if (!is_recovery_mode) {
        AddUserdataDirEntries(&fstab, android_dir_path);
        UseOverlayDirs(android_dir_path, &overlay_lowerdirs);
        AddUserdataTmpfsEntries(&fstab);
    }
    AddFirmwareDirEntry(&fstab);

    // Unmount the android dir when unused, so that live boot media can be ejected. Overlay dirs
    // are used once mounted.
    bool overlay_dirs_used =
            param_mount_overlay == MountOverlayParam::DIRS && !overlay_lowerdirs.empty();
    if (!is_recovery_mode && !overlay_dirs_used && umount(kAndroidMountTarget.c_str()) == 0) {
        LOG(INFO) << "umount " << kAndroidMountTarget << " successfully";
    }

    AddOverlayEntries(&fstab, overlay_lowerdirs);
    if (!is_recovery_mode) {
        for (auto& entry : addon_fstab) {
            fstab.push_back(std::move(entry));
        }
    }

    for (const auto& entry : fstab) {
        LOG(INFO) << "Fstab entry: blk_device=" << entry.blk_device
                  << " mount_point=" << entry.mount_point << " fs_type=" << entry.fs_type;
    }

    if (!DynamicMountHelpers::MountPartitions(fstab)) {
        LOG(FATAL) << "Failed to mount partitions";
    }
}

}  // namespace DynamicMountHandler
