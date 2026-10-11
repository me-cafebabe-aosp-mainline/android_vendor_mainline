/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineBootsplash"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "bootsplash.h"

namespace {

constexpr char kBgrtImage[] = "/sys/firmware/acpi/bgrt/image";
constexpr char kProductPng[] = "/product/etc/bootsplash.png";
constexpr char kProductImage[] = "/product/etc/bootsplash.bmp";

int ReadOffset(const char* path) {
    std::ifstream input(path);
    int result = -1;
    if (!(input >> result) || result < 0) return -1;
    return result;
}

// The BGRT offsets are relative to the mode the firmware drew in, which the
// kernel may have since replaced with another one. Firmware centers the logo,
// so that mode is 2 * offset + logo size. Only trust it when it yields a
// common aspect ratio, which a non-centered logo will not; then keep the
// logo's center at the same fraction of the current display.
bool LooksLikeDisplayMode(int64_t width, int64_t height) {
    if (width < 320 || height < 200 || width > 16384 || height > 16384) return false;
    constexpr int kRatios[][2] = {{4, 3}, {5, 4}, {3, 2}, {16, 10}, {16, 9}, {21, 9}};
    for (const auto& ratio : kRatios) {
        const int64_t lhs = width * ratio[1];
        const int64_t rhs = height * ratio[0];
        if ((lhs > rhs ? lhs - rhs : rhs - lhs) * 100 <= rhs) return true;
    }
    return false;
}

void MapBgrtOffsets(const bootsplash::Image& logo, int screen_width, int screen_height, int* x,
                    int* y) {
    if (*x <= 0 || *y <= 0) return;
    const int64_t old_width = 2 * int64_t(*x) + logo.width;
    const int64_t old_height = 2 * int64_t(*y) + logo.height;
    if (!LooksLikeDisplayMode(old_width, old_height)) return;
    if (old_width == screen_width && old_height == screen_height) return;
    LOG(INFO) << "Mapping BGRT logo from " << old_width << "x" << old_height << " to "
              << screen_width << "x" << screen_height;
    // Center fraction cx = (x + w / 2) / old_width, so x' = cx * new_width - w / 2.
    *x = static_cast<int>(std::max<int64_t>(
        0, (2 * int64_t(*x) + logo.width) * screen_width / (2 * old_width) - logo.width / 2));
    *y = static_cast<int>(std::max<int64_t>(
        0, (2 * int64_t(*y) + logo.height) * screen_height / (2 * old_height) - logo.height / 2));
}

struct Progress {
    int percent;
    uint32_t foreground_color;
    uint32_t background_color;
    uint32_t canvas_color;
    std::string text;
    std::string fallback_text;

    bool operator==(const Progress& other) const {
        return percent == other.percent && foreground_color == other.foreground_color &&
               background_color == other.background_color && canvas_color == other.canvas_color &&
               text == other.text && fallback_text == other.fallback_text;
    }
};

Progress ReadProgress() {
    const auto read_color = [](const char* key, uint32_t fallback) {
        uint32_t parsed;
        const std::string value = android::base::GetProperty(key, "");
        return android::base::ParseUint(value, &parsed, uint32_t(0xffffff)) ? parsed : fallback;
    };
    return {android::base::GetIntProperty<int>("sys.bootsplash.percent", 0, 0, 100),
            read_color("sys.bootsplash.color", 0x53b8df),
            read_color("sys.bootsplash.background_color", 0x30343b),
            read_color("sys.bootsplash.canvas_color", 0x000000),
            android::base::GetProperty("sys.bootsplash.text", ""),
            android::base::GetProperty("sys.bootsplash.logo_text", "")};
}

std::unique_ptr<bootsplash::Output> OpenOutput() {
    const std::string device = android::base::GetProperty("sys.bootsplash.device", "");
    if (!device.empty()) {
        if (device.compare(0, 8, "/dev/dri") == 0) return bootsplash::OpenDrm(device);
        if (device.compare(0, 7, "/dev/fb") == 0 ||
            device.compare(0, 16, "/dev/graphics/fb") == 0) {
            return bootsplash::OpenFbdev(device);
        }
        LOG(ERROR) << "Unsupported bootsplash device: " << device;
        return nullptr;
    }
    bool master_busy = false;
    auto output = bootsplash::OpenDrm("", &master_busy);
    if (!output && !master_busy) output = bootsplash::OpenFbdev("");
    return output;
}

}  // namespace

int main(int, char* argv[]) {
    android::base::InitLogging(argv, &android::base::KernelLogger);
    std::unique_ptr<bootsplash::Output> output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        output = OpenOutput();
        if (output) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!output) {
        LOG(WARNING) << "No available boot display";
        return 0;
    }

    bootsplash::Image logo;
    int logo_x = -1;
    int logo_y = -1;
    bool bgrt_logo = false;
    int bgrt_status = -1;
    if (bootsplash::LoadBmp(kBgrtImage, &logo)) {
        bgrt_logo = true;
        logo_x = ReadOffset("/sys/firmware/acpi/bgrt/xoffset");
        logo_y = ReadOffset("/sys/firmware/acpi/bgrt/yoffset");
        // Bits 1-2 of status are the orientation; the logo is not rotated here.
        bgrt_status = ReadOffset("/sys/firmware/acpi/bgrt/status");
        LOG(INFO) << "Using BGRT boot image";
    } else if (bootsplash::LoadPng(kProductPng, &logo)) {
        LOG(INFO) << "Using product PNG boot image";
    } else if (bootsplash::LoadBmp(kProductImage, &logo)) {
        LOG(INFO) << "Using product BMP boot image";
    } else {
        LOG(INFO) << "Using property boot text";
    }

    bootsplash::Image canvas;
    canvas.width = output->Width();
    canvas.height = output->Height();
    if (canvas.width <= 0 || canvas.height <= 0 ||
        static_cast<uint64_t>(canvas.width) * canvas.height > 32U * 1024U * 1024U) {
        LOG(ERROR) << "Boot display is too large";
        return 0;
    }
    canvas.pixels.resize(static_cast<size_t>(canvas.width) * canvas.height);
    if (bgrt_logo && bgrt_status >= 0 && ((bgrt_status >> 1) & 3) == 0) {
        MapBgrtOffsets(logo, canvas.width, canvas.height, &logo_x, &logo_y);
    }
    const int density = android::base::GetIntProperty<int>("ro.sf.lcd_density", 160, 1, 1000);
    auto progress = ReadProgress();
    while (true) {
        if (logo.pixels.empty()) {
            if (bootsplash::LoadPng(kProductPng, &logo)) {
                LOG(INFO) << "Using product PNG boot image";
            } else if (bootsplash::LoadBmp(kProductImage, &logo)) {
                        LOG(INFO) << "Using product BMP boot image";
            }
        }
        bootsplash::Render(
            &canvas, logo.pixels.empty() ? nullptr : &logo, logo_x, logo_y, bgrt_logo,
            progress.fallback_text, progress.text, progress.percent, progress.foreground_color,
            progress.background_color, progress.canvas_color, density);
        if (!output->Present(canvas)) {
            LOG(ERROR) << "Cannot present bootsplash";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto updated = ReadProgress();
        if (updated == progress) continue;
        progress = std::move(updated);
    }
    return 0;
}
