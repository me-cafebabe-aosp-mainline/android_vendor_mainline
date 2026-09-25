/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineBootsplash"

#include <android-base/logging.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>

#include "bootsplash.h"

namespace bootsplash {
namespace {

constexpr uint64_t kMaxFramebufferBytes = 256ULL * 1024 * 1024;

bool ReadSysfs(const std::string& path, std::string* value) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[256];
    ssize_t count = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (count <= 0) return false;
    value->assign(buffer, static_cast<size_t>(count));
    return true;
}

bool FbconBound() {
    for (int i = 0; i < 64; ++i) {
        std::string base = "/sys/class/vtconsole/vtcon" + std::to_string(i) + "/";
        struct stat entry;
        if (stat(base.c_str(), &entry) != 0) {
            if (errno == ENOENT) continue;
            return true;
        }
        if (!S_ISDIR(entry.st_mode)) return true;
        std::string name;
        std::string bind;
        if (!ReadSysfs(base + "name", &name) || !ReadSysfs(base + "bind", &bind)) return true;
        if (bind[0] != '0' && bind[0] != '1') return true;
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ((name.find("frame buffer") != std::string::npos ||
             name.find("fbcon") != std::string::npos) &&
            bind[0] == '1') {
            return true;
        }
    }
    return false;
}

bool ValidField(const fb_bitfield& field, uint32_t bits, uint32_t* occupied) {
    if (field.msb_right || field.length > bits || field.offset > bits - field.length) return false;
    if (!field.length) return true;
    uint32_t mask = static_cast<uint32_t>(((1ULL << field.length) - 1) << field.offset);
    if (*occupied & mask) return false;
    *occupied |= mask;
    return true;
}

bool EmptyBitfield(const fb_bitfield& field) {
    return field.offset == 0 && field.length == 0 && field.msb_right == 0;
}

uint32_t FourccBitsPerPixel(uint32_t format) {
    switch (format) {
        case V4L2_PIX_FMT_RGB565:
            return 16;
        case V4L2_PIX_FMT_RGB24:
            return 24;
        case V4L2_PIX_FMT_XRGB32:
        case V4L2_PIX_FMT_ARGB32:
        case V4L2_PIX_FMT_XBGR32:
        case V4L2_PIX_FMT_ABGR32:
        case V4L2_PIX_FMT_ARGB2101010:
            return 32;
        default:
            return 0;
    }
}

uint32_t ScaleChannel(uint32_t component, const fb_bitfield& field) {
    if (!field.length) return 0;
    uint64_t maximum = (1ULL << field.length) - 1;
    return static_cast<uint32_t>(((component * maximum + 127) / 255) << field.offset);
}

class FbdevOutput final : public Output {
   public:
    FbdevOutput(int fd, uint8_t* mapping, size_t mapping_size, int width, int height, size_t stride,
                size_t first_pixel, size_t row_bytes, uint32_t bytes_per_pixel, fb_bitfield red,
                fb_bitfield green, fb_bitfield blue, fb_bitfield alpha, uint32_t fourcc,
                bool requires_write_flush, bool check_console)
        : fd_(fd),
          mapping_(mapping),
          mapping_size_(mapping_size),
          width_(width),
          height_(height),
          stride_(stride),
          first_pixel_(first_pixel),
          row_bytes_(row_bytes),
          bytes_per_pixel_(bytes_per_pixel),
          red_(red),
          green_(green),
          blue_(blue),
          alpha_(alpha),
          fourcc_(fourcc),
          requires_write_flush_(requires_write_flush),
          check_console_(check_console) {}

    ~FbdevOutput() override {
        munmap(mapping_, mapping_size_);
        close(fd_);
    }

    int Width() const override { return width_; }
    int Height() const override { return height_; }

    bool Present(const Image& image) override {
        if (check_console_ && FbconBound()) return false;
        if (image.width != width_ || image.height != height_ ||
            image.pixels.size() < static_cast<size_t>(width_) * height_) {
            return false;
        }

        for (int y = 0; y < height_; ++y) {
            uint8_t* row = mapping_ + first_pixel_ + static_cast<size_t>(y) * stride_;
            const uint32_t* source = image.pixels.data() + static_cast<size_t>(y) * width_;
            for (int x = 0; x < width_; ++x) {
                uint32_t argb = source[x];
                uint32_t red = (argb >> 16) & 0xff;
                uint32_t green = (argb >> 8) & 0xff;
                uint32_t blue = argb & 0xff;
                uint32_t pixel;
                switch (fourcc_) {
                    case V4L2_PIX_FMT_RGB565:
                        pixel = ((red * 31 + 127) / 255 << 11) | ((green * 63 + 127) / 255 << 5) |
                                ((blue * 31 + 127) / 255);
                        break;
                    case V4L2_PIX_FMT_RGB24:
                        pixel = red | (green << 8) | (blue << 16);
                        break;
                    case V4L2_PIX_FMT_XRGB32:
                    case V4L2_PIX_FMT_ARGB32:
                        pixel = UINT8_MAX | (red << 8) | (green << 16) | (blue << 24);
                        break;
                    case V4L2_PIX_FMT_XBGR32:
                    case V4L2_PIX_FMT_ABGR32:
                        pixel = blue | (green << 8) | (red << 16) | (UINT32_C(0xff) << 24);
                        break;
                    case V4L2_PIX_FMT_ARGB2101010:
                        pixel = (3U << 30) | ((red * 1023 + 127) / 255 << 20) |
                                ((green * 1023 + 127) / 255 << 10) | ((blue * 1023 + 127) / 255);
                        break;
                    default:
                        pixel = ScaleChannel(red, red_) | ScaleChannel(green, green_) |
                                ScaleChannel(blue, blue_) | ScaleChannel(255, alpha_);
                        break;
                }
                uint8_t* dest = row + static_cast<size_t>(x) * bytes_per_pixel_;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                for (uint32_t byte = 0; byte < bytes_per_pixel_; ++byte) {
                    dest[bytes_per_pixel_ - 1 - byte] = static_cast<uint8_t>(pixel >> (byte * 8));
                }
#else
                for (uint32_t byte = 0; byte < bytes_per_pixel_; ++byte) {
                    dest[byte] = static_cast<uint8_t>(pixel >> (byte * 8));
                }
#endif
            }
        }

        // Some fbdev drivers track writes rather than mmap dirties for deferred updates.
        for (int y = 0; y < height_; ++y) {
            size_t offset = first_pixel_ + static_cast<size_t>(y) * stride_;
            size_t written = 0;
            while (written < row_bytes_) {
                ssize_t count = pwrite(fd_, mapping_ + offset + written, row_bytes_ - written,
                                       static_cast<off_t>(offset + written));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) {
                    if (count < 0 && written == 0 && !requires_write_flush_ &&
                        (errno == ENOTTY || errno == EINVAL || errno == ENOSYS ||
                         errno == EOPNOTSUPP)) {
                        // Ordinary fbdev mmap can directly address scanout and
                        // need neither a write nor an msync operation.
                        msync(mapping_, mapping_size_, MS_SYNC);
                        return true;
                    }
                    return false;
                }
                written += static_cast<size_t>(count);
            }
        }
        return true;
    }

   private:
    int fd_;
    uint8_t* mapping_;
    size_t mapping_size_;
    int width_;
    int height_;
    size_t stride_;
    size_t first_pixel_;
    size_t row_bytes_;
    uint32_t bytes_per_pixel_;
    fb_bitfield red_;
    fb_bitfield green_;
    fb_bitfield blue_;
    fb_bitfield alpha_;
    uint32_t fourcc_;
    bool requires_write_flush_;
    bool check_console_;
};

std::unique_ptr<Output> OpenDevice(const std::string& path, bool check_console) {
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return nullptr;

    struct stat statbuf;
    fb_fix_screeninfo fix = {};
    fb_var_screeninfo var = {};
    if (fstat(fd, &statbuf) != 0 || !S_ISCHR(statbuf.st_mode) ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
        close(fd);
        return nullptr;
    }

    if (var.xres_virtual == 0) var.xres_virtual = var.xres;
    if (var.yres_virtual == 0) var.yres_virtual = var.yres;
    uint32_t occupied = 0;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const bool fourcc = (fix.capabilities & FB_CAP_FOURCC) != 0 && fix.type == FB_TYPE_FOURCC &&
                        fix.visual == FB_VISUAL_FOURCC && fix.type_aux == 0 &&
                        FourccBitsPerPixel(var.grayscale) == var.bits_per_pixel &&
                        EmptyBitfield(var.red) && EmptyBitfield(var.green) &&
                        EmptyBitfield(var.blue) && EmptyBitfield(var.transp);
#else
    const bool fourcc = false;
#endif
    const bool truecolor = fix.type == FB_TYPE_PACKED_PIXELS && fix.visual == FB_VISUAL_TRUECOLOR &&
                           var.grayscale == 0 && var.red.length && var.green.length &&
                           var.blue.length && ValidField(var.red, var.bits_per_pixel, &occupied) &&
                           ValidField(var.green, var.bits_per_pixel, &occupied) &&
                           ValidField(var.blue, var.bits_per_pixel, &occupied) &&
                           ValidField(var.transp, var.bits_per_pixel, &occupied);
    if ((!fourcc && !truecolor) || var.nonstd ||
        (var.bits_per_pixel != 16 && var.bits_per_pixel != 24 && var.bits_per_pixel != 32) ||
        !var.xres || !var.yres ||
        var.xres > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        var.yres > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        var.xres > var.xres_virtual || var.yres > var.yres_virtual ||
        var.xoffset > var.xres_virtual - var.xres || var.yoffset > var.yres_virtual - var.yres ||
        !fix.line_length || fix.line_length > kMaxFramebufferBytes ||
        static_cast<uint64_t>(var.xres) * var.yres * sizeof(uint32_t) > kMaxFramebufferBytes) {
        close(fd);
        return nullptr;
    }

    uint64_t bytes_per_pixel = var.bits_per_pixel / 8;
    uint64_t row_bytes = static_cast<uint64_t>(var.xres) * bytes_per_pixel;
    uint64_t stride = fix.line_length;
    uint64_t first_pixel = static_cast<uint64_t>(var.yoffset) * stride +
                           static_cast<uint64_t>(var.xoffset) * bytes_per_pixel;
    uint64_t end = first_pixel + static_cast<uint64_t>(var.yres - 1) * stride + row_bytes;
    if ((static_cast<uint64_t>(var.xoffset) + var.xres) * bytes_per_pixel > stride ||
        end > kMaxFramebufferBytes || end > SIZE_MAX ||
        end > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        (fix.smem_len != 0 && end > fix.smem_len)) {
        close(fd);
        return nullptr;
    }

    void* mapping =
        mmap(nullptr, static_cast<size_t>(end), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    const std::string id(fix.id, strnlen(fix.id, sizeof(fix.id)));
    const bool requires_write_flush =
        id == "efidrmdrmfb" || id == "ofdrmdrmfb" || id == "simpledrmdrmfb" || id == "vesadrmdrmfb";
    LOG(INFO) << "Using fbdev " << path << " at " << var.xres << "x" << var.yres << " "
              << var.bits_per_pixel << "bpp";
    return std::make_unique<FbdevOutput>(
        fd, static_cast<uint8_t*>(mapping), static_cast<size_t>(end), static_cast<int>(var.xres),
        static_cast<int>(var.yres), static_cast<size_t>(stride), static_cast<size_t>(first_pixel),
        static_cast<size_t>(row_bytes), static_cast<uint32_t>(bytes_per_pixel), var.red, var.green,
        var.blue, var.transp, fourcc ? var.grayscale : 0, requires_write_flush, check_console);
}

}  // namespace

std::unique_ptr<Output> OpenFbdev(const std::string& path) {
    if (!path.empty()) return OpenDevice(path, false);
    if (FbconBound()) return nullptr;

    for (int i = 0; i < 16; ++i) {
        for (const char* prefix : {"/dev/graphics/fb", "/dev/fb"}) {
            if (auto output = OpenDevice(std::string(prefix) + std::to_string(i), true))
                return output;
        }
    }
    return nullptr;
}

}  // namespace bootsplash
