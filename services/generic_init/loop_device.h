/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>

// Loop devices set up from inside ueventd (generic_init), which is why it does not use the usual
// setup: that waits for ueventd to create the node, and would hang
namespace LoopDevice {

// Attaches the file to a free loop device and makes its node, returns its path or "". With
// partscan the kernel adds the partitions found in it, as devices named <loop>p<N>.
std::string Attach(int fd, bool read_only, bool partscan, uint32_t block_size = 0);

}  // namespace LoopDevice
