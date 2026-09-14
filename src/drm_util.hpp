#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <drm_fourcc.h>
#include <xf86drmMode.h>

namespace kmshot
{

// Owning file descriptor wrapper.
struct ScopedFd
{
    int fd{-1};
    ScopedFd() = default;
    explicit ScopedFd(int v) : fd(v) {}
    ~ScopedFd()
    {
        if (fd >= 0)
            ::close(fd);
    }
    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;
    ScopedFd(ScopedFd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    ScopedFd &operator=(ScopedFd &&o) noexcept
    {
        if (this != &o)
        {
            if (fd >= 0)
                ::close(fd);
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }
};

template <typename T, void (*FreeFn)(T *)>
using DrmPtr = std::unique_ptr<T, decltype(FreeFn)>;

using PlaneResPtr = DrmPtr<drmModePlaneRes, drmModeFreePlaneResources>;
using PlanePtr = DrmPtr<drmModePlane, drmModeFreePlane>;
using ResPtr = DrmPtr<drmModeRes, drmModeFreeResources>;
using ConnPtr = DrmPtr<drmModeConnector, drmModeFreeConnector>;
using EncPtr = DrmPtr<drmModeEncoder, drmModeFreeEncoder>;
using CrtcPtr = DrmPtr<drmModeCrtc, drmModeFreeCrtc>;
using ObjPropsPtr = DrmPtr<drmModeObjectProperties, drmModeFreeObjectProperties>;
using PropPtr = DrmPtr<drmModePropertyRes, drmModeFreeProperty>;
using BlobPtr = DrmPtr<drmModePropertyBlobRes, drmModeFreePropertyBlob>;

enum class PlaneType
{
    Unknown,
    Overlay,
    Primary,
    Cursor
};

PlaneType get_plane_type(int card_fd, uint32_t plane_id);

struct PlaneGeometry
{
    int32_t crtc_x{0};
    int32_t crtc_y{0};
    uint32_t crtc_w{0};
    uint32_t crtc_h{0};
    float src_x{0.0f};
    float src_y{0.0f};
    float src_w{0.0f};
    float src_h{0.0f};
};

PlaneGeometry get_plane_geometry(int fd, uint32_t plane_id, uint32_t fb_w, uint32_t fb_h);

struct PlaneSelection
{
    uint32_t plane_id{0};
    uint32_t crtc_id{0};
    uint32_t fb_id{0};
};

std::optional<PlaneSelection> find_capture_plane(int card_fd, int monitor_index);
std::optional<uint32_t> find_connector_for_crtc(int card_fd, uint32_t crtc_id);

std::optional<uint64_t> get_object_prop_u64(int fd, uint32_t obj_id, uint32_t obj_type, const char *name);

struct ObjectPropInfo
{
    uint64_t value{};
    bool has_enum_name{false};
    std::string enum_name;
};

std::optional<ObjectPropInfo> get_object_prop_info(
    int fd, uint32_t obj_id, uint32_t obj_type, const char *name);

// Snapshot of the currently scanned-out framebuffer of a plane.
struct FramebufferInfo
{
    uint32_t fb_id{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t fourcc{0};
    uint64_t modifier{DRM_FORMAT_MOD_INVALID};
    std::array<uint32_t, 4> handles{{0, 0, 0, 0}};
    std::array<uint32_t, 4> pitches{{0, 0, 0, 0}};
    std::array<uint32_t, 4> offsets{{0, 0, 0, 0}};

    static std::optional<FramebufferInfo> load(int card_fd, uint32_t fb_id);
};

std::string fourcc_to_string(uint32_t f);
bool is_single_plane_rgb_fourcc(uint32_t f);

// Read the raw EDID blob exposed by the connector's "EDID" property.
bool read_connector_edid(int card_fd,
                         uint32_t connector_id,
                         std::vector<uint8_t> &out,
                         std::string &error);

} // namespace kmshot
