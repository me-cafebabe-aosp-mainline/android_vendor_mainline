/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bootsplash {

struct Image {
    int width = 0;
    int height = 0;
    // 0xAARRGGBB; libyuv ARGB is BGRA in little-endian memory.
    std::vector<uint32_t> pixels;
};

bool LoadBmp(const std::string& path, Image* image);
bool LoadPng(const std::string& path, Image* image);
void Render(Image* canvas, const Image* logo, int logo_x, int logo_y,
            const std::string& fallback_text, const std::string& progress_text, int percent,
            uint32_t foreground_color, uint32_t background_color, uint32_t canvas_color,
            int density);

class Output {
   public:
    virtual ~Output() = default;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual bool Present(const Image& image) = 0;
};

std::unique_ptr<Output> OpenDrm(const std::string& path, bool* master_busy = nullptr);
std::unique_ptr<Output> OpenFbdev(const std::string& path);

}  // namespace bootsplash
