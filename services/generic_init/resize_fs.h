/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

// Grows the filesystem of the device that holds the android dir to fill it, for user data kept
// there: an image written to a bigger partition leaves the rest unused.
namespace ResizeFs {

// For an unmounted EXT4 or F2FS device that is a lot bigger than its filesystem: EXT4 is checked
// and resized with resize2fs, F2FS with resize.f2fs, both from /system/bin. A failure is logged and
// the boot goes on.
void GrowToDevice(const std::string& devpath, const std::string& fs_type);

}  // namespace ResizeFs
