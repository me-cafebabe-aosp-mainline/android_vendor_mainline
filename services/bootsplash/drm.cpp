/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineBootsplash"

#include <android-base/logging.h>
#include <dirent.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bootsplash.h"

namespace bootsplash {
namespace {

constexpr uint64_t kMaxBufferBytes = 128ULL * 1024 * 1024;
constexpr uint32_t kMaxDimension = 8192;
constexpr uint32_t kFormats[] = {
    DRM_FORMAT_XRGB8888,    DRM_FORMAT_XBGR8888,    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_ABGR8888,    DRM_FORMAT_RGB565,      DRM_FORMAT_XRGB2101010,
    DRM_FORMAT_XBGR2101010, DRM_FORMAT_ARGB2101010, DRM_FORMAT_ABGR2101010,
};

struct Candidate {
    uint32_t connector;
    uint32_t crtc;
    uint32_t crtc_index;
    drmModeModeInfo mode;
    bool internal;
    bool preferred;
};

bool Property(int fd, uint32_t object, uint32_t type, const char* name, uint32_t* id,
              uint64_t* value = nullptr) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, object, type);
    if (!props) return false;
    bool found = false;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (prop) {
            if (std::strcmp(prop->name, name) == 0) {
                *id = prop->prop_id;
                if (value) *value = props->prop_values[i];
                found = true;
            }
            drmModeFreeProperty(prop);
        }
        if (found) break;
    }
    drmModeFreeObjectProperties(props);
    return found;
}

bool InternalConnector(uint32_t type) {
    return type == DRM_MODE_CONNECTOR_eDP || type == DRM_MODE_CONNECTOR_LVDS ||
           type == DRM_MODE_CONNECTOR_DSI;
}

std::vector<Candidate> GetCandidates(int fd) {
    std::vector<Candidate> candidates;
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) return candidates;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            int mode_index = -1;
            for (int m = 0; m < conn->count_modes; ++m) {
                const auto& mode = conn->modes[m];
                if (!mode.hdisplay || !mode.vdisplay || mode.hdisplay > kMaxDimension ||
                    mode.vdisplay > kMaxDimension ||
                    static_cast<uint64_t>(mode.hdisplay) * mode.vdisplay * 2 > kMaxBufferBytes)
                    continue;
                if (mode_index < 0 || (mode.type & DRM_MODE_TYPE_PREFERRED)) {
                    mode_index = m;
                    if (mode.type & DRM_MODE_TYPE_PREFERRED) break;
                }
            }
            if (mode_index >= 0) {
                const auto& mode = conn->modes[mode_index];
                // Try the active encoder first, then the connector's other encoders.
                std::vector<uint32_t> encoders;
                if (conn->encoder_id) encoders.push_back(conn->encoder_id);
                for (int e = 0; e < conn->count_encoders; ++e) {
                    if (conn->encoders[e] != conn->encoder_id)
                        encoders.push_back(conn->encoders[e]);
                }
                bool selected = false;
                for (uint32_t encoder_id : encoders) {
                    drmModeEncoderPtr enc = drmModeGetEncoder(fd, encoder_id);
                    if (!enc) continue;
                    for (int pass = 0; pass < 2 && !selected; ++pass) {
                        for (int c = 0; c < res->count_crtcs && c < 32; ++c) {
                            if (!(enc->possible_crtcs & (1U << c)) ||
                                (pass == 0 && res->crtcs[c] != enc->crtc_id) ||
                                (pass == 1 && res->crtcs[c] == enc->crtc_id))
                                continue;
                            candidates.push_back({conn->connector_id, res->crtcs[c],
                                                  static_cast<uint32_t>(c), mode,
                                                  InternalConnector(conn->connector_type),
                                                  (mode.type & DRM_MODE_TYPE_PREFERRED) != 0});
                            selected = true;
                            break;
                        }
                    }
                    drmModeFreeEncoder(enc);
                    if (selected) break;
                }
                if (selected) {
                    const Candidate chosen = candidates.back();
                    for (int m = 0; m < conn->count_modes; ++m) {
                        const auto& alternate = conn->modes[m];
                        if (m == mode_index || !alternate.hdisplay || !alternate.vdisplay ||
                            alternate.hdisplay > kMaxDimension ||
                            alternate.vdisplay > kMaxDimension ||
                            static_cast<uint64_t>(alternate.hdisplay) * alternate.vdisplay * 2 >
                                kMaxBufferBytes)
                            continue;
                        Candidate fallback = chosen;
                        fallback.mode = alternate;
                        fallback.preferred = (alternate.type & DRM_MODE_TYPE_PREFERRED) != 0;
                        candidates.push_back(fallback);
                    }
                }
            }
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.internal != b.internal) return a.internal;
                         return a.preferred && !b.preferred;
                     });
    return candidates;
}

struct Plane {
    uint32_t id = 0;
    uint32_t format = 0;
};

Plane PickPlane(int fd, const Candidate& candidate, uint32_t format) {
    Plane selected;
    drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
    if (!planes) return selected;
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
        drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[i]);
        if (!plane) continue;
        uint32_t id;
        uint64_t type;
        bool primary = Property(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &id, &type) &&
                       type == DRM_PLANE_TYPE_PRIMARY;
        if (primary && (plane->possible_crtcs & (1U << candidate.crtc_index)) &&
            (!plane->crtc_id || plane->crtc_id == candidate.crtc)) {
            for (uint32_t j = 0; j < plane->count_formats; ++j) {
                if (plane->formats[j] == format) {
                    selected = {plane->plane_id, format};
                    break;
                }
            }
        }
        drmModeFreePlane(plane);
        if (selected.id) break;
    }
    drmModeFreePlaneResources(planes);
    return selected;
}

class DrmOutput final : public Output {
   public:
    explicit DrmOutput(int fd) : fd_(fd) {}

    ~DrmOutput() override {
        if (fb_) drmModeRmFB(fd_, fb_);
        if (map_ != MAP_FAILED) munmap(map_, size_);
        if (handle_) {
            drm_mode_destroy_dumb destroy = {};
            destroy.handle = handle_;
            drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
        close(fd_);
    }

    int Width() const override { return width_; }
    int Height() const override { return height_; }

    bool Present(const Image& image) override {
        if (image.width != width_ || image.height != height_ ||
            image.pixels.size() < static_cast<size_t>(width_) * height_)
            return false;
        for (int y = 0; y < height_; ++y) {
            const uint32_t* src = image.pixels.data() + static_cast<size_t>(y) * width_;
            uint8_t* row = static_cast<uint8_t*>(map_) + static_cast<size_t>(y) * pitch_;
            if (format_ == DRM_FORMAT_RGB565) {
                auto* dst = reinterpret_cast<uint16_t*>(row);
                for (int x = 0; x < width_; ++x) {
                    uint32_t p = src[x];
                    dst[x] = static_cast<uint16_t>(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) |
                                                   ((p >> 3) & 0x001f));
                }
            } else {
                auto* dst = reinterpret_cast<uint32_t*>(row);
                for (int x = 0; x < width_; ++x) {
                    uint32_t p = src[x];
                    uint32_t red = (p >> 16) & 0xff;
                    uint32_t green = (p >> 8) & 0xff;
                    uint32_t blue = p & 0xff;
                    bool bgr = format_ == DRM_FORMAT_XBGR8888 || format_ == DRM_FORMAT_ABGR8888 ||
                               format_ == DRM_FORMAT_XBGR2101010 ||
                               format_ == DRM_FORMAT_ABGR2101010;
                    if (bgr) std::swap(red, blue);
                    bool ten_bit =
                        format_ == DRM_FORMAT_XRGB2101010 || format_ == DRM_FORMAT_XBGR2101010 ||
                        format_ == DRM_FORMAT_ARGB2101010 || format_ == DRM_FORMAT_ABGR2101010;
                    dst[x] = ten_bit ? (3U << 30) | ((red * 1023 + 127) / 255 << 20) |
                                           ((green * 1023 + 127) / 255 << 10) |
                                           ((blue * 1023 + 127) / 255)
                                     : 0xff000000 | (red << 16) | (green << 8) | blue;
                }
            }
        }
        if (damage_property_) {
            drm_mode_rect rect = {0, 0, width_, height_};
            uint32_t blob = 0;
            if (drmModeCreatePropertyBlob(fd_, &rect, sizeof(rect), &blob) != 0) return false;
            drmModeAtomicReqPtr request = drmModeAtomicAlloc();
            bool ok = request &&
                      drmModeAtomicAddProperty(request, plane_, damage_property_, blob) >= 0 &&
                      drmModeAtomicCommit(fd_, request, 0, nullptr) == 0;
            if (request) drmModeAtomicFree(request);
            drmModeDestroyPropertyBlob(fd_, blob);
            return ok;
        }
        // Legacy drivers with a shadow scanout may implement dirtyfb instead.
        drmModeClip clip = {0, 0, static_cast<uint16_t>(width_), static_cast<uint16_t>(height_)};
        if (drmModeDirtyFB(fd_, fb_, &clip, 1) == 0) return true;
        return errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL || errno == ENOTTY;
    }

    bool Init(const Candidate& candidate, bool atomic, Plane plane, uint32_t format) {
        width_ = candidate.mode.hdisplay;
        height_ = candidate.mode.vdisplay;
        format_ = format;
        uint32_t bpp = format_ == DRM_FORMAT_RGB565 ? 16 : 32;
        if (static_cast<uint64_t>(width_) * height_ * (bpp / 8) > kMaxBufferBytes) return false;
        drm_mode_create_dumb create = {};
        create.width = width_;
        create.height = height_;
        create.bpp = bpp;
        if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) return false;
        handle_ = create.handle;
        pitch_ = create.pitch;
        size_ = create.size;
        if (size_ == 0 || size_ > kMaxBufferBytes ||
            pitch_ < static_cast<uint64_t>(width_) * (bpp / 8) ||
            size_ < static_cast<uint64_t>(pitch_) * height_ ||
            size_ > std::numeric_limits<size_t>::max())
            return false;

        uint32_t handles[4] = {handle_, 0, 0, 0};
        uint32_t pitches[4] = {pitch_, 0, 0, 0};
        uint32_t offsets[4] = {};
        if (drmModeAddFB2(fd_, width_, height_, format_, handles, pitches, offsets, &fb_, 0) != 0)
            return false;
        drm_mode_map_dumb mapping = {};
        mapping.handle = handle_;
        if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &mapping) != 0) return false;
        map_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, mapping.offset);
        if (map_ == MAP_FAILED) return false;
        std::memset(map_, 0, size_);

        if (!atomic) {
            std::vector<uint32_t> connectors{candidate.connector};
            drmModeResPtr resources = drmModeGetResources(fd_);
            if (!resources) return false;
            for (int i = 0; i < resources->count_connectors; ++i) {
                uint32_t id = resources->connectors[i];
                if (id == candidate.connector) continue;
                drmModeConnectorPtr connector = drmModeGetConnectorCurrent(fd_, id);
                if (!connector) continue;
                if (connector->encoder_id) {
                    drmModeEncoderPtr encoder = drmModeGetEncoder(fd_, connector->encoder_id);
                    if (encoder) {
                        if (encoder->crtc_id == candidate.crtc) connectors.push_back(id);
                        drmModeFreeEncoder(encoder);
                    }
                }
                drmModeFreeConnector(connector);
            }
            drmModeFreeResources(resources);
            drmModeModeInfo mode = candidate.mode;
            return drmModeSetCrtc(fd_, candidate.crtc, fb_, 0, 0, connectors.data(),
                                  static_cast<int>(connectors.size()), &mode) == 0;
        }

        uint32_t conn_crtc, crtc_mode, crtc_active, plane_fb, plane_crtc;
        uint32_t src_x, src_y, src_w, src_h, crtc_x, crtc_y, crtc_w, crtc_h;
        if (!Property(fd_, candidate.connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", &conn_crtc) ||
            !Property(fd_, candidate.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID", &crtc_mode) ||
            !Property(fd_, candidate.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE", &crtc_active) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "FB_ID", &plane_fb) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", &plane_crtc) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "SRC_X", &src_x) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "SRC_Y", &src_y) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "SRC_W", &src_w) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "SRC_H", &src_h) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "CRTC_X", &crtc_x) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", &crtc_y) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "CRTC_W", &crtc_w) ||
            !Property(fd_, plane.id, DRM_MODE_OBJECT_PLANE, "CRTC_H", &crtc_h))
            return false;

        uint32_t blob = 0;
        if (drmModeCreatePropertyBlob(fd_, &candidate.mode, sizeof(candidate.mode), &blob) != 0)
            return false;
        drmModeAtomicReqPtr request = drmModeAtomicAlloc();
        bool ok = request &&
                  drmModeAtomicAddProperty(request, candidate.connector, conn_crtc,
                                           candidate.crtc) >= 0 &&
                  drmModeAtomicAddProperty(request, candidate.crtc, crtc_mode, blob) >= 0 &&
                  drmModeAtomicAddProperty(request, candidate.crtc, crtc_active, 1) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, plane_fb, fb_) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, plane_crtc, candidate.crtc) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, src_x, 0) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, src_y, 0) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, src_w,
                                           static_cast<uint64_t>(width_) << 16) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, src_h,
                                           static_cast<uint64_t>(height_) << 16) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, crtc_x, 0) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, crtc_y, 0) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, crtc_w, width_) >= 0 &&
                  drmModeAtomicAddProperty(request, plane.id, crtc_h, height_) >= 0;
        if (ok) {
            ok = drmModeAtomicCommit(fd_, request,
                                     DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
                                     nullptr) == 0 &&
                 drmModeAtomicCommit(fd_, request, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) == 0;
        }
        if (request) drmModeAtomicFree(request);
        drmModeDestroyPropertyBlob(fd_, blob);
        if (ok) {
            plane_ = plane.id;
            Property(fd_, plane_, DRM_MODE_OBJECT_PLANE, "FB_DAMAGE_CLIPS", &damage_property_);
        }
        return ok;
    }

   private:
    int fd_;
    int width_ = 0;
    int height_ = 0;
    uint32_t format_ = 0;
    uint32_t handle_ = 0;
    uint32_t fb_ = 0;
    uint32_t plane_ = 0;
    uint32_t damage_property_ = 0;
    uint32_t pitch_ = 0;
    size_t size_ = 0;
    void* map_ = MAP_FAILED;
};

std::vector<std::string> CardPaths(const std::string& path) {
    if (!path.empty())
        return path == "/dev/null" ? std::vector<std::string>{} : std::vector<std::string>{path};
    std::vector<std::pair<unsigned int, std::string>> cards;
    DIR* dir = opendir("/dev/dri");
    if (!dir) return {};
    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (std::strncmp(name, "card", 4) != 0 || !name[4]) continue;
        unsigned int number = 0;
        bool valid = true;
        for (const char* p = name + 4; *p; ++p) {
            if (*p < '0' || *p > '9' ||
                number > (std::numeric_limits<unsigned int>::max() - (*p - '0')) / 10) {
                valid = false;
                break;
            }
            number = number * 10 + (*p - '0');
        }
        if (valid) cards.emplace_back(number, "/dev/dri/" + std::string(name));
    }
    closedir(dir);
    std::sort(cards.begin(), cards.end());
    std::vector<std::string> paths;
    for (const auto& card : cards) paths.push_back(card.second);
    return paths;
}

}  // namespace

std::unique_ptr<Output> OpenDrm(const std::string& path, bool* master_busy) {
    if (master_busy) *master_busy = false;
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    return nullptr;
#endif
    for (const auto& card : CardPaths(path)) {
        int fd = open(card.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st;
        // An existing compositor's DRM master cannot be taken by this process.
        if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
            close(fd);
            continue;
        }
        if (drmIsMaster(fd) != 1 && drmSetMaster(fd) != 0) {
            if (master_busy) *master_busy = true;
            close(fd);
            continue;
        }
        uint64_t dumb = 0;
        if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &dumb) != 0 || !dumb) {
            close(fd);
            continue;
        }
        bool atomic = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0 &&
                      drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
        for (const Candidate& candidate : GetCandidates(fd)) {
            for (uint32_t format : kFormats) {
                Plane plane = atomic ? PickPlane(fd, candidate, format) : Plane{};
                if (atomic && !plane.id) continue;
                int output_fd = dup(fd);
                if (output_fd < 0) break;
                auto output = std::make_unique<DrmOutput>(output_fd);
                if (output->Init(candidate, atomic, plane, format)) {
                    LOG(INFO) << "Using DRM " << card << " connector " << candidate.connector
                              << " at " << output->Width() << "x" << output->Height() << " format "
                              << format;
                    close(fd);
                    return output;
                }
            }
        }
        close(fd);
    }
    return nullptr;
}

}  // namespace bootsplash
