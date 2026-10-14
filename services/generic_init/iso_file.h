/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

// An ISO image booted as a file of a partition (Ventoy, GRUB loopback...)
namespace IsoFile {

// The UUID of an ISO9660 image as GRUB and blkid show it, or ""
std::string ReadUuid(const std::string& path);

// Looks for an image with that UUID under the directory, returns its path relative to it or ""
std::string Find(const std::string& dir, const std::string& uuid);

// Mounts the partition read-only on a directory of its own and attaches the image to a loop
// device, returns the loop device or ""
std::string Attach(const std::string& partition_devpath, const std::string& fs_type,
                   const std::string& path);

}  // namespace IsoFile
