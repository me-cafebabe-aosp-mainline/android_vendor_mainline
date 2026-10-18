/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>

// A partition table inside a partition, like a disk image written to it
namespace PartitionTable {

// Returns the sector size (512 or 4096) of the GPT or MBR the device holds, 0 if none
uint32_t Find(const std::string& devpath);

}  // namespace PartitionTable
