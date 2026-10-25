/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "uevent.h"

#include <string>
#include <vector>

namespace DynamicMountHandler {

void OnPreBlockDevices(void);

void OnBlockDeviceAdd(const android::init::Uevent& uevent, const std::string& devpath,
                      const std::vector<std::string>& links);

bool CanQuitUeventd(bool print_log);

// After ueventd, before the root is switched: the tools of the ramdisk are still there
void OnPreSwitchRoot(void);

void OnPostBlockDevices(bool is_recovery_mode = false);

}  // namespace DynamicMountHandler
