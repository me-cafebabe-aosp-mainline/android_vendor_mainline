/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineBootsplash"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "bootsplash.h"

namespace {

constexpr char kBgrtImage[] = "/sys/firmware/acpi/bgrt/image";
constexpr char kProductImage[] = "/product/etc/bootsplash.bmp";

int ReadOffset(const char* path) {
    std::ifstream input(path);
    int result = -1;
    if (!(input >> result) || result < 0) return -1;
    return result;
}

struct Progress {
    int percent;
    uint32_t foreground_color;
    uint32_t background_color;
    std::string text;
    std::string fallback_text;

    bool operator==(const Progress& other) const {
        return percent == other.percent && foreground_color == other.foreground_color &&
               background_color == other.background_color && text == other.text &&
               fallback_text == other.fallback_text;
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

int main() {
    android::base::InitLogging(nullptr);
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
    if (bootsplash::LoadBmp(kBgrtImage, &logo)) {
        logo_x = ReadOffset("/sys/firmware/acpi/bgrt/xoffset");
        logo_y = ReadOffset("/sys/firmware/acpi/bgrt/yoffset");
        LOG(INFO) << "Using BGRT boot image";
    } else if (bootsplash::LoadBmp(kProductImage, &logo)) {
        LOG(INFO) << "Using product boot image";
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
    const int density = android::base::GetIntProperty<int>("ro.sf.lcd_density", 160, 1, 1000);
    auto progress = ReadProgress();
    while (true) {
        if (logo.pixels.empty() && bootsplash::LoadBmp(kProductImage, &logo)) {
            LOG(INFO) << "Using late-mounted product boot image";
        }
        bootsplash::Render(&canvas, logo.pixels.empty() ? nullptr : &logo, logo_x, logo_y,
                           progress.fallback_text, progress.text, progress.percent,
                           progress.foreground_color, progress.background_color, density);
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
