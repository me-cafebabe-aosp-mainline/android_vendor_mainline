/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_gadget.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <mutex>

extern bool fs_mgr_get_boot_config(const std::string& key, std::string* out_val);

namespace UsbGadget {

using android::base::ReadFileToString;
using android::base::Trim;
using android::base::WriteStringToFile;

namespace {

constexpr char kParam[] = "usb_gadget";
constexpr char kConfigfs[] = "/config";
constexpr char kGadgets[] = "/config/usb_gadget";
// Not "g1": the name Android's USB HAL uses
constexpr char kGadget[] = "/config/usb_gadget/generic_init";

// Linux Foundation, Multifunction Composite Gadget
constexpr char kVendorId[] = "0x1d6b";
constexpr char kProductId[] = "0x0104";
constexpr char kLanguage[] = "0x409";

struct Function {
    // Directory name under functions/: <function>.<instance>
    std::string instance;
    std::string product;
    // Let a Windows host bind WinUSB to it without a driver package
    bool winusb;
};

const std::map<std::string, Function> kFunctions = {
        {"blk", {"blk.0", "USB block device", true}},
};

std::mutex lock;
const Function* function = nullptr;
bool configured = false;

bool WriteValue(const std::string& path, const std::string& value) {
    if (!WriteStringToFile(value, path)) {
        PLOG(ERROR) << "Cannot write \"" << value << "\" to " << path;
        return false;
    }
    return true;
}

bool MakeDir(const std::string& path) {
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        PLOG(ERROR) << "Cannot create " << path;
        return false;
    }
    return true;
}

bool MakeLink(const std::string& target, const std::string& link) {
    if (symlink(target.c_str(), link.c_str()) != 0 && errno != EEXIST) {
        PLOG(ERROR) << "Cannot link " << link << " to " << target;
        return false;
    }
    return true;
}

bool MountConfigfs() {
    if (access(kGadgets, F_OK) == 0) return true;
    mkdir(kConfigfs, 0755);
    if (mount("configfs", kConfigfs, "configfs", MS_NODEV | MS_NOEXEC | MS_NOSUID, "") != 0) {
        PLOG(ERROR) << "Cannot mount configfs on " << kConfigfs;
        return false;
    }
    if (access(kGadgets, F_OK) != 0) {
        LOG(ERROR) << "The kernel has no USB gadget support (CONFIG_USB_CONFIGFS)";
        return false;
    }
    return true;
}

// WinUSB, as in Documentation/usb/gadget-blk.rst. A host that is not Windows ignores it, so a
// failure only costs a driver package there.
void SetUpWinUsb(const std::string& gadget, const std::string& config, const Function& f) {
    std::string guid_dir =
            gadget + "/functions/" + f.instance + "/os_desc/interface.blk/DeviceInterfaceGUID";
    if (!WriteValue(gadget + "/os_desc/use", "1") ||
        !WriteValue(gadget + "/os_desc/b_vendor_code", "0xcd") ||
        !WriteValue(gadget + "/os_desc/qw_sign", "MSFT100") ||
        !MakeLink(config, gadget + "/os_desc/c.1") || !MakeDir(guid_dir) ||
        !WriteValue(guid_dir + "/type", "1") ||
        !WriteValue(guid_dir + "/data", "{4c2c7e7b-8b8f-4bd4-9b5f-6f0a5d8b0b1e}")) {
        LOG(WARNING) << "Cannot set up the Microsoft OS descriptors";
    }
}

bool Configure(const Function& f) {
    if (configured) return true;
    if (!MountConfigfs()) return false;

    std::string gadget = kGadget;
    std::string strings = gadget + "/strings/" + kLanguage;
    std::string config = gadget + "/configs/c.1";
    std::string function_dir = gadget + "/functions/" + f.instance;
    std::string serial;
    ReadFileToString("/proc/sys/kernel/random/uuid", &serial);

    if (!MakeDir(gadget) || !WriteValue(gadget + "/idVendor", kVendorId) ||
        !WriteValue(gadget + "/idProduct", kProductId) || !MakeDir(strings) ||
        !WriteValue(strings + "/serialnumber", Trim(serial)) ||
        !WriteValue(strings + "/manufacturer", "Mainline") ||
        !WriteValue(strings + "/product", f.product) || !MakeDir(config) ||
        !MakeDir(function_dir) || !MakeLink(function_dir, config + "/" + f.instance)) {
        return false;
    }
    if (f.winusb) SetUpWinUsb(gadget, config, f);
    configured = true;
    return true;
}

}  // namespace

void Init() {
    std::string name;
    if (!fs_mgr_get_boot_config(kParam, &name) || name.empty()) return;
    auto it = kFunctions.find(name);
    if (it == kFunctions.end()) {
        LOG(ERROR) << "Unknown USB gadget function " << name << ", ignoring it";
        return;
    }
    function = &it->second;
    LOG(INFO) << "USB gadget function " << name << " requested";
}

void OnUdcAdd(const std::string& udc) {
    std::lock_guard<std::mutex> guard(lock);
    if (function == nullptr || !Configure(*function)) return;

    // The controller of a role switch can come back: the kernel binds the gadget to it again by
    // the name, which is still set
    std::string bound;
    ReadFileToString(std::string(kGadget) + "/UDC", &bound);
    bound = Trim(bound);
    if (!bound.empty()) {
        if (bound != udc) LOG(INFO) << "USB gadget already bound to " << bound << ", not " << udc;
        return;
    }
    if (WriteValue(std::string(kGadget) + "/UDC", udc)) {
        LOG(INFO) << "USB gadget " << function->instance << " bound to " << udc
                  << ", waiting for the USB host";
    }
}

}  // namespace UsbGadget
