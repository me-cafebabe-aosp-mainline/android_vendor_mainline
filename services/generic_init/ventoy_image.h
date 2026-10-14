/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

// The image Ventoy booted under UEFI, from the parameters it leaves in an EFI variable. Under
// BIOS they can not be read: the image is found by its UUID instead (iso_file.h).
namespace VentoyImage {

// Reads the parameters, false if Ventoy did not boot this system
bool ReadParams();

// Whether the whole disk is the Ventoy drive
bool IsOnDisk(const std::string& devpath);

// The number of the partition holding the image, from 1
int PartitionNumber();

// The path of the image in that partition
std::string Path();

}  // namespace VentoyImage
