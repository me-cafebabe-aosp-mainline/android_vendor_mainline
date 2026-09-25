/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ft2build.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "bootsplash.h"
#include FT_FREETYPE_H
#include <libyuv/convert_argb.h>
#include <libyuv/scale_argb.h>

namespace bootsplash {
namespace {

constexpr size_t kMaxBmpSize = 16 * 1024 * 1024;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

int64_t Signed32(uint32_t value) {
    return value <= INT32_MAX ? int64_t(value) : int64_t(value) - (int64_t(1) << 32);
}

// Invalid UTF-8 is displayed as '?' rather than passed to FreeType as an arbitrary codepoint.
uint32_t NextCodepoint(const std::string& text, size_t limit, size_t* index) {
    const uint8_t first = static_cast<uint8_t>(text[(*index)++]);
    if (first < 0x80) return first;
    int extra = first >= 0xc2 && first <= 0xdf   ? 1
                : first >= 0xe0 && first <= 0xef ? 2
                : first >= 0xf0 && first <= 0xf4 ? 3
                                                 : 0;
    if (!extra || limit - *index < static_cast<size_t>(extra)) return '?';
    uint32_t codepoint = first & ((1u << (6 - extra)) - 1);
    for (int i = 0; i < extra; ++i) {
        uint8_t byte = static_cast<uint8_t>(text[*index + i]);
        if ((byte & 0xc0) != 0x80) return '?';
        codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if (codepoint < (extra == 1   ? 0x80u
                     : extra == 2 ? 0x800u
                                  : 0x10000u) ||
        codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return '?';
    *index += extra;
    return codepoint;
}

void DrawText(Image* canvas, FT_Face face, const std::string& text, int baseline, uint32_t color) {
    if (!face || text.empty()) return;
    // Limit both glyph work and the pen advance for untrusted status strings.
    const size_t limit = std::min(text.size(), size_t(4096));
    int64_t advance = 0;
    for (size_t i = 0; i < limit;) {
        uint32_t cp = NextCodepoint(text, limit, &i);
        if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0) {
            advance += face->glyph->advance.x >> 6;
        }
    }
    int64_t pen = (int64_t(canvas->width) - advance) / 2;
    for (size_t i = 0; i < limit;) {
        uint32_t cp = NextCodepoint(text, limit, &i);
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) continue;
        FT_GlyphSlot glyph = face->glyph;
        const FT_Bitmap& bitmap = glyph->bitmap;
        if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY && bitmap.buffer && bitmap.pitch != 0) {
            int64_t left = pen + glyph->bitmap_left;
            int64_t top = int64_t(baseline) - glyph->bitmap_top;
            int64_t x0 = std::max<int64_t>(0, -left);
            int64_t y0 = std::max<int64_t>(0, -top);
            int64_t x1 = std::min<int64_t>(bitmap.width, int64_t(canvas->width) - left);
            int64_t y1 = std::min<int64_t>(bitmap.rows, int64_t(canvas->height) - top);
            for (int64_t y = y0; y < y1; ++y) {
                const uint8_t* row =
                    bitmap.buffer +
                    (bitmap.pitch > 0 ? y : int64_t(bitmap.rows - 1) - y) *
                        (bitmap.pitch > 0 ? int64_t(bitmap.pitch) : -int64_t(bitmap.pitch));
                for (int64_t x = x0; x < x1; ++x) {
                    unsigned alpha = row[x];
                    uint32_t& dst =
                        canvas->pixels[size_t(top + y) * canvas->width + size_t(left + x)];
                    unsigned inv = 255 - alpha;
                    unsigned r =
                        (((color >> 16) & 255) * alpha + ((dst >> 16) & 255) * inv + 127) / 255;
                    unsigned g =
                        (((color >> 8) & 255) * alpha + ((dst >> 8) & 255) * inv + 127) / 255;
                    unsigned b = ((color & 255) * alpha + (dst & 255) * inv + 127) / 255;
                    dst = 0xff000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
        pen += glyph->advance.x >> 6;
    }
}

void FillRect(Image* canvas, int64_t x, int64_t y, int64_t width, int64_t height, uint32_t color) {
    int64_t x0 = std::clamp<int64_t>(x, 0, canvas->width);
    int64_t y0 = std::clamp<int64_t>(y, 0, canvas->height);
    int64_t x1 = std::clamp<int64_t>(x + width, 0, canvas->width);
    int64_t y1 = std::clamp<int64_t>(y + height, 0, canvas->height);
    for (int64_t row = y0; row < y1; ++row) {
        auto begin = canvas->pixels.begin() + size_t(row) * canvas->width;
        std::fill(begin + x0, begin + x1, color);
    }
}

}  // namespace

bool LoadBmp(const std::string& path, Image* image) {
    if (!image) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<uint8_t> bytes;
    std::array<char, 8192> chunk;
    while (file.read(chunk.data(), chunk.size()) || file.gcount() != 0) {
        size_t count = static_cast<size_t>(file.gcount());
        if (count > kMaxBmpSize - bytes.size()) return false;
        bytes.insert(bytes.end(), chunk.data(), chunk.data() + count);
    }
    if (!file.eof() || bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') return false;

    uint32_t file_size = Le32(bytes.data() + 2);
    uint32_t pixel_offset = Le32(bytes.data() + 10);
    uint32_t dib_size = Le32(bytes.data() + 14);
    if (dib_size < 40 || uint64_t(14) + dib_size > bytes.size() || file_size > bytes.size() ||
        pixel_offset < uint64_t(14) + dib_size || pixel_offset > file_size ||
        Le16(bytes.data() + 26) != 1 || Le32(bytes.data() + 30) != 0)
        return false;

    int64_t width = Signed32(Le32(bytes.data() + 18));
    int64_t signed_height = Signed32(Le32(bytes.data() + 22));
    int64_t height = signed_height < 0 ? -signed_height : signed_height;
    uint16_t bpp = Le16(bytes.data() + 28);
    if (width <= 0 || height == 0 || width > INT32_MAX / 4 || height > INT32_MAX ||
        (bpp != 24 && bpp != 32))
        return false;

    uint64_t stride = ((uint64_t(width) * bpp + 31) / 32) * 4;
    uint64_t payload = stride * uint64_t(height);
    uint32_t image_size = Le32(bytes.data() + 34);
    if (stride > INT32_MAX || payload > uint64_t(file_size) - pixel_offset ||
        (image_size && (image_size < payload || image_size > uint64_t(file_size) - pixel_offset))) {
        return false;
    }

    Image decoded;
    decoded.width = static_cast<int>(width);
    decoded.height = static_cast<int>(height);
    decoded.pixels.resize(size_t(width) * size_t(height));
    const uint8_t* src = bytes.data() + pixel_offset;
    if (bpp == 24) {
        if (libyuv::RGB24ToARGB(src, static_cast<int>(stride),
                                reinterpret_cast<uint8_t*>(decoded.pixels.data()),
                                decoded.width * 4, decoded.width,
                                signed_height > 0 ? -decoded.height : decoded.height) != 0)
            return false;
    } else {
        for (int y = 0; y < decoded.height; ++y) {
            const uint8_t* row =
                src + size_t(signed_height > 0 ? decoded.height - 1 - y : y) * stride;
            for (int x = 0; x < decoded.width; ++x) {
                const uint8_t* p = row + size_t(x) * 4;
                decoded.pixels[size_t(y) * decoded.width + x] =
                    0xff000000u | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
            }
        }
    }
    *image = std::move(decoded);
    return true;
}

void Render(Image* canvas, const Image* logo, int logo_x, int logo_y,
            const std::string& fallback_text, const std::string& progress_text, int percent,
            uint32_t color) {
    if (!canvas || canvas->width <= 0 || canvas->height <= 0 || canvas->width > INT32_MAX / 4 ||
        uint64_t(canvas->width) * canvas->height > canvas->pixels.size())
        return;
    const int w = canvas->width;
    const int h = canvas->height;
    std::fill(canvas->pixels.begin(), canvas->pixels.begin() + size_t(w) * h, 0xff080b12u);
    color = 0xff000000u | (color & 0x00ffffffu);

    bool has_logo = logo && logo->width > 0 && logo->height > 0 && logo->width <= INT32_MAX / 4 &&
                    uint64_t(logo->width) * logo->height <= logo->pixels.size();
    if (has_logo) {
        int draw_w = logo->width;
        int draw_h = logo->height;
        const uint32_t* pixels = logo->pixels.data();
        std::vector<uint32_t> scaled;
        if (draw_w > w || draw_h > h) {
            // Preserve aspect ratio, rounding down while keeping both dimensions nonzero.
            if (int64_t(w) * draw_h <= int64_t(h) * draw_w) {
                draw_h = std::max(1, int(int64_t(draw_h) * w / draw_w));
                draw_w = w;
            } else {
                draw_w = std::max(1, int(int64_t(draw_w) * h / draw_h));
                draw_h = h;
            }
            scaled.resize(size_t(draw_w) * draw_h);
            if (libyuv::ARGBScale(reinterpret_cast<const uint8_t*>(pixels), logo->width * 4,
                                  logo->width, logo->height,
                                  reinterpret_cast<uint8_t*>(scaled.data()), draw_w * 4, draw_w,
                                  draw_h, libyuv::kFilterBilinear) != 0) {
                has_logo = false;
            } else {
                pixels = scaled.data();
            }
        }
        if (has_logo) {
            int x = logo_x >= 0 && int64_t(logo_x) + draw_w <= w ? logo_x : (w - draw_w) / 2;
            int y = logo_y >= 0 && int64_t(logo_y) + draw_h <= h ? logo_y : (h - draw_h) / 2;
            for (int row = 0; row < draw_h; ++row) {
                std::copy_n(pixels + size_t(row) * draw_w, draw_w,
                            canvas->pixels.begin() + size_t(y + row) * w + x);
            }
        }
    }

    FT_Library library = nullptr;
    FT_Face face = nullptr;
    if (FT_Init_FreeType(&library) == 0) {
        if (FT_New_Face(library, "/system/fonts/Roboto-Regular.ttf", 0, &face) == 0) {
            if (FT_Set_Pixel_Sizes(face, 0, std::clamp(h / 36, 16, 32)) != 0) {
                FT_Done_Face(face);
                face = nullptr;
            }
        }
    }
    if (!has_logo) DrawText(canvas, face, fallback_text, h / 2, 0xffeeeeeeu);

    int64_t bar_width = std::min<int64_t>(480, std::max<int64_t>(0, int64_t(w) - 32));
    int64_t bar_x = (w - bar_width) / 2;
    int64_t bar_y = int64_t(h) * 3 / 4;
    FillRect(canvas, bar_x, bar_y, bar_width, 12, 0xff30343bu);
    FillRect(canvas, bar_x, bar_y, bar_width * std::clamp(percent, 0, 100) / 100, 12, color);
    DrawText(canvas, face, progress_text, static_cast<int>(bar_y - 12), 0xffeeeeeeu);
    DrawText(canvas, face, std::to_string(std::clamp(percent, 0, 100)) + "%",
             static_cast<int>(std::min<int64_t>(INT32_MAX, bar_y + 42)), 0xffeeeeeeu);
    if (face) FT_Done_Face(face);
    if (library) FT_Done_FreeType(library);
}

}  // namespace bootsplash
