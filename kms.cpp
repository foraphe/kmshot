#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <iomanip>

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <set>
#include <linux/dma-buf.h>
#include <algorithm>
#include <cmath>

#include "color_transform.hpp"


#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

static uint16_t float01_to_u16_sat(float v)
{
    if (!(v >= 0.0f))
        return 0; // includes NaN
    if (v >= 1.0f)
        return 65535;
    return static_cast<uint16_t>(std::llround(static_cast<double>(v) * 65535.0));
}

static uint16_t float01_to_u12_msb16_sat(float v)
{
    if (!(v >= 0.0f))
        return 0; // includes NaN
    if (v >= 1.0f)
        return 0xFFF0u; // 12-bit full scale, MSB-aligned in 16-bit
    const uint16_t q12 = static_cast<uint16_t>(
        std::llround(static_cast<double>(v) * 4095.0));
    return static_cast<uint16_t>(q12 << 4);
}

static const char *egl_error_string(EGLint e)
{
    switch (e)
    {
    case EGL_SUCCESS:
        return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
        return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
        return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
        return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
        return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:
        return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:
        return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:
        return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
        return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:
        return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:
        return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:
        return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:
        return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
        return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:
        return "EGL_CONTEXT_LOST";
    default:
        return "EGL_UNKNOWN_ERROR";
    }
}

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_CONTEXT_LOST
#define GL_CONTEXT_LOST 0x0507
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif

#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#endif
#ifndef GL_TEXTURE_RED_SIZE
#define GL_TEXTURE_RED_SIZE 0x805C
#endif
#ifndef GL_TEXTURE_GREEN_SIZE
#define GL_TEXTURE_GREEN_SIZE 0x805D
#endif
#ifndef GL_TEXTURE_BLUE_SIZE
#define GL_TEXTURE_BLUE_SIZE 0x805E
#endif
#ifndef GL_TEXTURE_ALPHA_SIZE
#define GL_TEXTURE_ALPHA_SIZE 0x805F
#endif

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

static bool has_gl_extension(const char *ext_list, const char *ext)
{
    if (!ext_list || !ext)
        return false;
    const std::string all(ext_list);
    const std::string needle = std::string(" ") + ext + " ";
    return (std::string(" ") + all + " ").find(needle) != std::string::npos;
}

static bool has_egl_extension(const char *ext_list, const char *ext)
{
    if (!ext_list || !ext)
        return false;
    const std::string all(ext_list);
    const std::string needle = std::string(" ") + ext + " ";
    return (std::string(" ") + all + " ").find(needle) != std::string::npos;
}

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

enum class PlaneType
{
    Unknown,
    Overlay,
    Primary,
    Cursor
};

static PlaneType get_plane_type(int card_fd, uint32_t plane_id)
{
    ObjPropsPtr props(drmModeObjectGetProperties(card_fd, plane_id, DRM_MODE_OBJECT_PLANE), drmModeFreeObjectProperties);
    if (!props)
        return PlaneType::Unknown;

    for (uint32_t i = 0; i < props->count_props; ++i)
    {
        PropPtr p(drmModeGetProperty(card_fd, props->props[i]), drmModeFreeProperty);
        if (!p)
            continue;
        if (std::strcmp(p->name, "type") == 0)
        {
            switch (props->prop_values[i])
            {
            case DRM_PLANE_TYPE_PRIMARY:
                return PlaneType::Primary;
            case DRM_PLANE_TYPE_CURSOR:
                return PlaneType::Cursor;
            case DRM_PLANE_TYPE_OVERLAY:
                return PlaneType::Overlay;
            default:
                return PlaneType::Unknown;
            }
        }
    }
    return PlaneType::Unknown;
}

static std::vector<uint32_t> connected_crtcs(int card_fd)
{
    std::vector<uint32_t> out;
    std::set<uint32_t> seen;

    ResPtr res(drmModeGetResources(card_fd), drmModeFreeResources);
    if (!res)
        return out;

    for (int i = 0; i < res->count_connectors; ++i)
    {
        ConnPtr conn(drmModeGetConnector(card_fd, res->connectors[i]), drmModeFreeConnector);
        if (!conn || conn->connection != DRM_MODE_CONNECTED)
            continue;
        if (!conn->encoder_id)
            continue;

        EncPtr enc(drmModeGetEncoder(card_fd, conn->encoder_id), drmModeFreeEncoder);
        if (!enc || !enc->crtc_id)
            continue;

        if (seen.insert(enc->crtc_id).second)
            out.push_back(enc->crtc_id);
    }
    return out;
}

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

static std::optional<uint64_t> get_object_prop_u64(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
    ObjPropsPtr props(drmModeObjectGetProperties(fd, obj_id, obj_type), drmModeFreeObjectProperties);
    if (!props)
        return std::nullopt;

    for (uint32_t i = 0; i < props->count_props; ++i)
    {
        PropPtr p(drmModeGetProperty(fd, props->props[i]), drmModeFreeProperty);
        if (!p)
            continue;
        if (std::strcmp(p->name, name) == 0)
        {
            return props->prop_values[i];
        }
    }
    return std::nullopt;
}

struct ObjectPropInfo
{
    uint64_t value{};
    bool has_enum_name{false};
    std::string enum_name;
};

static std::optional<ObjectPropInfo> get_object_prop_info(
    int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
    ObjPropsPtr props(drmModeObjectGetProperties(fd, obj_id, obj_type), drmModeFreeObjectProperties);
    if (!props)
        return std::nullopt;

    for (uint32_t i = 0; i < props->count_props; ++i)
    {
        PropPtr p(drmModeGetProperty(fd, props->props[i]), drmModeFreeProperty);
        if (!p)
            continue;
        if (std::strcmp(p->name, name) != 0)
            continue;

        ObjectPropInfo out{};
        out.value = props->prop_values[i];

        if (p->flags & DRM_MODE_PROP_ENUM)
        {
            for (int e = 0; e < p->count_enums; ++e)
            {
                if (p->enums[e].value == out.value)
                {
                    out.has_enum_name = true;
                    out.enum_name = p->enums[e].name;
                    break;
                }
            }
        }

        return out;
    }

    return std::nullopt;
}

static std::optional<uint32_t> find_connector_for_crtc(int card_fd, uint32_t crtc_id)
{
    ResPtr res(drmModeGetResources(card_fd), drmModeFreeResources);
    if (!res)
        return std::nullopt;

    for (int i = 0; i < res->count_connectors; ++i)
    {
        ConnPtr conn(drmModeGetConnector(card_fd, res->connectors[i]), drmModeFreeConnector);
        if (!conn || conn->connection != DRM_MODE_CONNECTED || !conn->encoder_id)
            continue;

        EncPtr enc(drmModeGetEncoder(card_fd, conn->encoder_id), drmModeFreeEncoder);
        if (!enc)
            continue;
        if (enc->crtc_id == crtc_id)
            return conn->connector_id;
    }

    return std::nullopt;
}

static void log_panel_orientation(int card_fd, uint32_t plane_id)
{
    auto rot = get_object_prop_u64(card_fd, plane_id, DRM_MODE_OBJECT_PLANE, "rotation");
    if (!rot)
    {
        std::cerr << "Panel orientation: rotation property not available\n";
        return;
    }

    std::string flags;
    auto add = [&](const char *s)
    {
        if (!flags.empty())
            flags += "|";
        flags += s;
    };

    if (*rot & DRM_MODE_ROTATE_0)
        add("ROTATE_0");
    if (*rot & DRM_MODE_ROTATE_90)
        add("ROTATE_90");
    if (*rot & DRM_MODE_ROTATE_180)
        add("ROTATE_180");
    if (*rot & DRM_MODE_ROTATE_270)
        add("ROTATE_270");
#ifdef DRM_MODE_REFLECT_X
    if (*rot & DRM_MODE_REFLECT_X)
        add("REFLECT_X");
#endif
#ifdef DRM_MODE_REFLECT_Y
    if (*rot & DRM_MODE_REFLECT_Y)
        add("REFLECT_Y");
#endif
    if (flags.empty())
        flags = "UNKNOWN";

    std::cerr << "Panel orientation (plane rotation): 0x" << std::hex << *rot << std::dec
              << " [" << flags << "]\n";
}

static void log_hdr_metadata(int card_fd, uint32_t connector_id)
{
    auto hdr_blob_id = get_object_prop_u64(card_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA");
    if (!hdr_blob_id)
    {
        std::cerr << "HDR_OUTPUT_METADATA: property not available on connector " << connector_id << "\n";
        return;
    }

    if (*hdr_blob_id == 0)
    {
        std::cerr << "HDR_OUTPUT_METADATA: blob_id=0 (no active static HDR metadata)\n";
        return;
    }

    BlobPtr blob(drmModeGetPropertyBlob(card_fd, static_cast<uint32_t>(*hdr_blob_id)), drmModeFreePropertyBlob);
    if (!blob)
    {
        std::cerr << "drmModeGetPropertyBlob(" << *hdr_blob_id << ") failed\n";
        return;
    }

    std::cerr << "HDR_OUTPUT_METADATA: blob_id=" << *hdr_blob_id
              << " length=" << blob->length << "\n";

    if (blob->length < sizeof(uint32_t) + sizeof(hdr_metadata_infoframe))
    {
        std::cerr << "HDR_OUTPUT_METADATA: blob too small to parse\n";
        return;
    }

    auto *raw = reinterpret_cast<const hdr_output_metadata *>(blob->data);
    std::cerr << "HDR metadata_type=" << raw->metadata_type
              << " eotf=" << static_cast<uint32_t>(raw->hdmi_metadata_type1.eotf)
              << " metadata_type1=" << static_cast<uint32_t>(raw->hdmi_metadata_type1.metadata_type)
              << " max_cll=" << raw->hdmi_metadata_type1.max_cll
              << " max_fall=" << raw->hdmi_metadata_type1.max_fall
              << " max_luma=" << raw->hdmi_metadata_type1.max_display_mastering_luminance
              << " min_luma=" << raw->hdmi_metadata_type1.min_display_mastering_luminance
              << "\n";
}

static void log_prop_if_present(
    int card_fd, uint32_t obj_id, uint32_t obj_type, const char *obj_name, const char *prop_name)
{
    auto info = get_object_prop_info(card_fd, obj_id, obj_type, prop_name);
    if (!info)
        return;

    std::cerr << obj_name << " property " << prop_name << "=" << info->value;
    if (info->has_enum_name)
    {
        std::cerr << " (" << info->enum_name << ")";
    }
    std::cerr << "\n";
}

static void log_color_and_range_info(
    int card_fd, uint32_t plane_id, uint32_t crtc_id, const std::optional<uint32_t> &connector_id)
{
    std::cerr << "Color/range capability probe:\n";

    if (connector_id)
    {
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "Colorspace");
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "Broadcast RGB");
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "max bpc");
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "content type");
    }

    log_prop_if_present(card_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "crtc", "Colorspace");
    log_prop_if_present(card_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "crtc", "COLOR_ENCODING");
    log_prop_if_present(card_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "crtc", "COLOR_RANGE");

    log_prop_if_present(card_fd, plane_id, DRM_MODE_OBJECT_PLANE, "plane", "COLOR_ENCODING");
    log_prop_if_present(card_fd, plane_id, DRM_MODE_OBJECT_PLANE, "plane", "COLOR_RANGE");
}

static PlaneGeometry get_plane_geometry(int fd, uint32_t plane_id, uint32_t fb_w, uint32_t fb_h)
{
    PlaneGeometry g;
    g.crtc_w = fb_w;
    g.crtc_h = fb_h;
    g.src_w = static_cast<float>(fb_w);
    g.src_h = static_cast<float>(fb_h);

    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X"))
        g.crtc_x = static_cast<int32_t>(static_cast<uint32_t>(*v));
    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y"))
        g.crtc_y = static_cast<int32_t>(static_cast<uint32_t>(*v));

    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W"))
        g.crtc_w = static_cast<uint32_t>(*v);
    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H"))
        g.crtc_h = static_cast<uint32_t>(*v);

    // SRC_* are 16.16 fixed point
    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X"))
        g.src_x = static_cast<float>(*v) / 65536.0f;
    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y"))
        g.src_y = static_cast<float>(*v) / 65536.0f;
    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W"))
        g.src_w = static_cast<float>(*v) / 65536.0f;
    if (auto v = get_object_prop_u64(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H"))
        g.src_h = static_cast<float>(*v) / 65536.0f;

    if (g.crtc_w == 0)
        g.crtc_w = fb_w;
    if (g.crtc_h == 0)
        g.crtc_h = fb_h;
    if (g.src_w <= 0.0f)
        g.src_w = static_cast<float>(fb_w);
    if (g.src_h <= 0.0f)
        g.src_h = static_cast<float>(fb_h);

    return g;
}

struct PlaneSelection
{
    uint32_t plane_id{};
    uint32_t crtc_id{};
    uint32_t fb_id{};
};

static std::optional<PlaneSelection> find_capture_plane(int card_fd, int monitor_index)
{
    auto crtcs = connected_crtcs(card_fd);
    uint32_t target_crtc = 0;

    if (!crtcs.empty())
    {
        if (monitor_index < 0 || monitor_index >= static_cast<int>(crtcs.size()))
        {
            std::cerr << "Invalid --monitor " << monitor_index
                      << " (available 0.." << (crtcs.size() - 1) << ")\n";
            return std::nullopt;
        }
        target_crtc = crtcs[monitor_index];
    }

    PlaneResPtr pres(drmModeGetPlaneResources(card_fd), drmModeFreePlaneResources);
    if (!pres)
        return std::nullopt;

    std::optional<PlaneSelection> fallback_non_cursor;

    for (uint32_t i = 0; i < pres->count_planes; ++i)
    {
        PlanePtr plane(drmModeGetPlane(card_fd, pres->planes[i]), drmModeFreePlane);
        if (!plane || plane->fb_id == 0)
            continue;
        if (target_crtc && plane->crtc_id != target_crtc)
            continue;

        PlaneType t = get_plane_type(card_fd, plane->plane_id);
        if (t == PlaneType::Cursor)
            continue;

        if (!fallback_non_cursor)
        {
            fallback_non_cursor = PlaneSelection{plane->plane_id, plane->crtc_id, plane->fb_id};
        }

        if (t == PlaneType::Primary)
        {
            return PlaneSelection{plane->plane_id, plane->crtc_id, plane->fb_id};
        }
    }

    return fallback_non_cursor;
}

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

    static uint32_t infer_legacy_fourcc(const drmModeFB *fb)
    {
        if (!fb)
            return 0;
        // Legacy KMS FB doesn't expose fourcc/modifier.
        // Best-effort inference for common scanout formats.
        if (fb->bpp == 32 && fb->depth == 30)
            return DRM_FORMAT_XRGB2101010;
        if (fb->bpp == 32)
            return DRM_FORMAT_XRGB8888;
        return 0;
    }

    static std::optional<FramebufferInfo> load(int card_fd, uint32_t fb_id)
    {
        if (fb_id == 0)
            return std::nullopt;

        if (auto *fb2 = drmModeGetFB2(card_fd, fb_id))
        {
            FramebufferInfo out;
            out.fb_id = fb2->fb_id;
            out.width = fb2->width;
            out.height = fb2->height;
            out.fourcc = fb2->pixel_format;
            out.modifier = fb2->modifier;
            for (int i = 0; i < 4; ++i)
            {
                out.handles[i] = fb2->handles[i];
                out.pitches[i] = fb2->pitches[i];
                out.offsets[i] = fb2->offsets[i];
            }
            drmModeFreeFB2(fb2);
            return out;
        }

        if (auto *fb = drmModeGetFB(card_fd, fb_id))
        {
            FramebufferInfo out;
            out.fb_id = fb->fb_id;
            out.width = fb->width;
            out.height = fb->height;
            out.fourcc = infer_legacy_fourcc(fb);
            out.modifier = DRM_FORMAT_MOD_INVALID;
            out.handles[0] = fb->handle;
            out.pitches[0] = fb->pitch;
            out.offsets[0] = 0;
            drmModeFreeFB(fb);
            return out;
        }

        return std::nullopt;
    }
};

static void print_display_metadata(int card_fd)
{
    ResPtr res(drmModeGetResources(card_fd), drmModeFreeResources);
    if (!res)
    {
        std::cerr << "drmModeGetResources failed\n";
        return;
    }

    std::cerr << "Connectors: " << res->count_connectors << "\n";
    for (int i = 0; i < res->count_connectors; ++i)
    {
        ConnPtr conn(drmModeGetConnector(card_fd, res->connectors[i]), drmModeFreeConnector);
        if (!conn)
            continue;

        std::cerr << "- connector_id=" << conn->connector_id
                  << " type=" << conn->connector_type
                  << " connected=" << (conn->connection == DRM_MODE_CONNECTED ? "yes" : "no")
                  << " modes=" << conn->count_modes << "\n";

        if (conn->connection == DRM_MODE_CONNECTED)
        {
            for (int m = 0; m < conn->count_modes && m < 5; ++m)
            {
                const auto &mode = conn->modes[m];
                std::cerr << "    mode[" << m << "]: " << mode.name << " "
                          << mode.hdisplay << "x" << mode.vdisplay
                          << "@" << mode.vrefresh << "\n";
            }

            if (conn->encoder_id)
            {
                EncPtr enc(drmModeGetEncoder(card_fd, conn->encoder_id), drmModeFreeEncoder);
                if (enc && enc->crtc_id)
                {
                    CrtcPtr crtc(drmModeGetCrtc(card_fd, enc->crtc_id), drmModeFreeCrtc);
                    if (crtc)
                    {
                        std::cerr << "    current CRTC " << crtc->crtc_id
                                  << " pos=(" << crtc->x << "," << crtc->y << ")"
                                  << " size=" << crtc->width << "x" << crtc->height << "\n";
                    }
                }
            }
        }
    }
}

static std::string fourcc_to_string(uint32_t f)
{
    char s[5] = {
        static_cast<char>(f & 0xff),
        static_cast<char>((f >> 8) & 0xff),
        static_cast<char>((f >> 16) & 0xff),
        static_cast<char>((f >> 24) & 0xff),
        0};
    return std::string(s);
}

static bool is_single_plane_rgb_fourcc(uint32_t f)
{
    switch (f)
    {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_ABGR16161616:
        return true;
#ifdef DRM_FORMAT_XRGB16161616F
    case DRM_FORMAT_XRGB16161616F:
#endif
#ifdef DRM_FORMAT_ARGB16161616F
    case DRM_FORMAT_ARGB16161616F:
#endif
#ifdef DRM_FORMAT_XBGR16161616F
    case DRM_FORMAT_XBGR16161616F:
#endif
#ifdef DRM_FORMAT_ABGR16161616F
    case DRM_FORMAT_ABGR16161616F:
#endif
        return true;
    default:
        return false;
    }
}

class DmabufGlReader
{
public:
    ~DmabufGlReader() { shutdown(); }

    bool init(int card_fd)
    {
        gbm_ = gbm_create_device(card_fd);
        if (!gbm_)
        {
            std::cerr << "gbm_create_device failed\n";
            return false;
        }

        auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!get_platform_display)
        {
            std::cerr << "eglGetPlatformDisplayEXT not available\n";
            return false;
        }

        egl_dpy_ = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
        if (egl_dpy_ == EGL_NO_DISPLAY)
        {
            std::cerr << "eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR) failed\n";
            return false;
        }

        if (!eglInitialize(egl_dpy_, nullptr, nullptr))
        {
            EGLint err = eglGetError();
            std::cerr << "eglInitialize failed: " << egl_error_string(err)
                      << " (0x" << std::hex << err << std::dec << ")\n";
            return false;
        }
        if (!eglBindAPI(EGL_OPENGL_ES_API))
        {
            EGLint err = eglGetError();
            std::cerr << "eglBindAPI(EGL_OPENGL_ES_API) failed: " << egl_error_string(err)
                      << " (0x" << std::hex << err << std::dec << ")\n";
            return false;
        }

        const char *exts = eglQueryString(egl_dpy_, EGL_EXTENSIONS);
        const bool surfaceless_ok = has_egl_extension(exts, "EGL_KHR_surfaceless_context");

        const EGLint cfg_attrs_strict[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE};
        const EGLint cfg_attrs_relaxed[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE};
        const EGLint cfg_attrs_surfaceless[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE};

        EGLConfig cfg{};
        EGLint num{};

        bool have_cfg = eglChooseConfig(egl_dpy_, cfg_attrs_strict, &cfg, 1, &num) && num > 0;
        if (!have_cfg)
        {
            have_cfg = eglChooseConfig(egl_dpy_, cfg_attrs_relaxed, &cfg, 1, &num) && num > 0;
        }
        if (!have_cfg && surfaceless_ok)
        {
            have_cfg = eglChooseConfig(egl_dpy_, cfg_attrs_surfaceless, &cfg, 1, &num) && num > 0;
            surfaceless_ = have_cfg;
        }

        if (!have_cfg)
        {
            EGLint err = eglGetError();
            std::cerr << "eglChooseConfig failed: " << egl_error_string(err)
                      << " (0x" << std::hex << err << std::dec << ")\n";
            return false;
        }

        if (!surfaceless_)
        {
            const EGLint pb_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
            egl_surf_ = eglCreatePbufferSurface(egl_dpy_, cfg, pb_attrs);
            if (egl_surf_ == EGL_NO_SURFACE)
            {
                if (surfaceless_ok)
                {
                    surfaceless_ = true;
                }
                else
                {
                    EGLint err = eglGetError();
                    std::cerr << "eglCreatePbufferSurface failed: " << egl_error_string(err)
                              << " (0x" << std::hex << err << std::dec << ")\n";
                    return false;
                }
            }
        }

        // Prefer GLES3 for high-bit-depth readback path, fallback to GLES2.
        const EGLint ctx_attrs3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLint ctx_attrs2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        egl_ctx_ = eglCreateContext(egl_dpy_, cfg, EGL_NO_CONTEXT, ctx_attrs3);
        if (egl_ctx_ != EGL_NO_CONTEXT)
        {
            gles_major_ = 3;
        }
        else
        {
            egl_ctx_ = eglCreateContext(egl_dpy_, cfg, EGL_NO_CONTEXT, ctx_attrs2);
            gles_major_ = 2;
        }

        if (egl_ctx_ == EGL_NO_CONTEXT)
        {
            EGLint err = eglGetError();
            std::cerr << "eglCreateContext failed: " << egl_error_string(err)
                      << " (0x" << std::hex << err << std::dec << ")\n";
            return false;
        }

        EGLSurface s = surfaceless_ ? EGL_NO_SURFACE : egl_surf_;
        if (!eglMakeCurrent(egl_dpy_, s, s, egl_ctx_))
        {
            EGLint err = eglGetError();
            std::cerr << "eglMakeCurrent failed: " << egl_error_string(err)
                      << " (0x" << std::hex << err << std::dec << ")\n";
            return false;
        }

        const char *gl_exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
        if (!has_gl_extension(gl_exts, "GL_OES_EGL_image_external"))
        {
            std::cerr << "Missing GL_OES_EGL_image_external\n";
            return false;
        }

        const bool has_float_color =
            has_gl_extension(gl_exts, "GL_EXT_color_buffer_float");

        high_precision_path_ = (gles_major_ >= 3) && has_float_color;
        std::cerr << "Readback path: "
                  << (high_precision_path_ ? "GPU FP32 (RGBA32F -> float)" : "fallback (RGBA8 -> software float)")
                  << "\n";

        eglCreateImageKHR_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        eglDestroyImageKHR_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        glEGLImageTargetTexture2DOES_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));

        if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ || !glEGLImageTargetTexture2DOES_)
        {
            std::cerr << "Required EGL/GL DMA-BUF import symbols not available\n";
            return false;
        }

        glGenTextures(1, &import_tex_);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenTextures(1, &readback_tex_);
        glBindTexture(GL_TEXTURE_2D, readback_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &fbo_);

        if (!build_blit_program())
        {
            std::cerr << "Failed to build blit shader\n";
            return false;
        }

        const GLfloat quad[] = {
            -1.f,
            -1.f,
            0.f,
            0.f,
            1.f,
            -1.f,
            1.f,
            0.f,
            -1.f,
            1.f,
            0.f,
            1.f,
            1.f,
            1.f,
            1.f,
            1.f,
        };
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

        return true;
    }

    bool context_lost() const
    {
        std::cerr << "GPU context lost\n";
        return context_lost_;
    }

    bool read_dmabuf_to_rgba32f(
        int dmabuf_fd,
        uint32_t fb_width,
        uint32_t fb_height,
        uint32_t out_width,
        uint32_t out_height,
        float uv_off_x,
        float uv_off_y,
        float uv_scale_x,
        float uv_scale_y,
        uint32_t fourcc,
        uint32_t pitch0,
        uint32_t offset0,
        uint64_t modifier,
        bool dmabuf_sync,
        std::vector<float> &out_rgba32f)
    {
        auto dmabuf_sync_ioctl = [&](uint64_t flags)
        {
            if (!dmabuf_sync)
                return;
            dma_buf_sync s{};
            s.flags = flags;
            if (::ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &s) != 0)
            {
                if (!sync_warned_ && errno != ENOTTY && errno != EINVAL && errno != ENOSYS)
                {
                    sync_warned_ = true;
                    std::cerr << "DMA_BUF_IOCTL_SYNC failed: " << std::strerror(errno) << "\n";
                }
            }
        };

        dmabuf_sync_ioctl(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);

        std::vector<EGLint> attrs = {
            EGL_WIDTH,
            static_cast<EGLint>(fb_width),
            EGL_HEIGHT,
            static_cast<EGLint>(fb_height),
            EGL_LINUX_DRM_FOURCC_EXT,
            static_cast<EGLint>(fourcc),
            EGL_DMA_BUF_PLANE0_FD_EXT,
            dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            static_cast<EGLint>(offset0),
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            static_cast<EGLint>(pitch0),
        };
        if (modifier != DRM_FORMAT_MOD_INVALID)
        {
            attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
            attrs.push_back(static_cast<EGLint>(modifier & 0xFFFFFFFFu));
            attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
            attrs.push_back(static_cast<EGLint>((modifier >> 32) & 0xFFFFFFFFu));
        }
        attrs.push_back(EGL_NONE);

        EGLImageKHR image = eglCreateImageKHR_(
            egl_dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
        if (image == EGL_NO_IMAGE_KHR)
        {
            std::cerr << "eglCreateImageKHR failed for fourcc=" << fourcc_to_string(fourcc) << "\n";
            dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            return false;
        }

        if (!ensure_readback_target(out_width, out_height))
        {
            eglDestroyImageKHR_(egl_dpy_, image);
            dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            return false;
        }

        glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_EXTERNAL_OES, image);
        if (gl_has_error("glEGLImageTargetTexture2DOES"))
        {
            eglDestroyImageKHR_(egl_dpy_, image);
            dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            return false;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readback_tex_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            std::cerr << "Framebuffer incomplete\n";
            eglDestroyImageKHR_(egl_dpy_, image);
            dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            return false;
        }

        glViewport(0, 0, static_cast<GLsizei>(out_width), static_cast<GLsizei>(out_height));
        glUseProgram(prog_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(static_cast<GLuint>(loc_pos_));
        glEnableVertexAttribArray(static_cast<GLuint>(loc_uv_));
        glVertexAttribPointer(static_cast<GLuint>(loc_pos_), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(0));
        glVertexAttribPointer(static_cast<GLuint>(loc_uv_), 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(2 * sizeof(GLfloat)));
        glUniform2f(loc_uv_off_, uv_off_x, uv_off_y);
        glUniform2f(loc_uv_scale_, uv_scale_x, uv_scale_y);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, import_tex_);
        glUniform1i(loc_tex_, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(static_cast<GLuint>(loc_pos_));
        glDisableVertexAttribArray(static_cast<GLuint>(loc_uv_));

        if (gl_has_error("glDrawArrays"))
        {
            eglDestroyImageKHR_(egl_dpy_, image);
            dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            return false;
        }

        const size_t px_count = static_cast<size_t>(out_width) * out_height;
        out_rgba32f.resize(px_count * 4u);

        if (high_precision_path_)
        {
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(
                0, 0,
                static_cast<GLsizei>(out_width),
                static_cast<GLsizei>(out_height),
                GL_RGBA,
                GL_FLOAT,
                out_rgba32f.data());
            glFinish();

            if (gl_has_error("glReadPixels(GL_FLOAT)/glFinish"))
            {
                eglDestroyImageKHR_(egl_dpy_, image);
                dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                return false;
            }
        }
        else
        {
            std::vector<uint8_t> tmp8(px_count * 4u);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(
                0, 0,
                static_cast<GLsizei>(out_width),
                static_cast<GLsizei>(out_height),
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                tmp8.data());
            glFinish();

            if (gl_has_error("glReadPixels(GL_UNSIGNED_BYTE)/glFinish"))
            {
                eglDestroyImageKHR_(egl_dpy_, image);
                dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                return false;
            }

            for (size_t i = 0; i < tmp8.size(); ++i)
            {
                out_rgba32f[i] = static_cast<float>(tmp8[i]) * (1.0f / 255.0f);
            }
        }

        eglDestroyImageKHR_(egl_dpy_, image);
        dmabuf_sync_ioctl(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        return true;
    }

private:
    static uint16_t float01_to_u16_sat(float v)
    {
        if (!(v >= 0.0f))
            return 0; // includes NaN
        if (v >= 1.0f)
            return 65535;
        return static_cast<uint16_t>(std::llround(static_cast<double>(v) * 65535.0));
    }

    bool ensure_readback_target(uint32_t w, uint32_t h)
    {
        if (rb_w_ == w && rb_h_ == h)
            return true;

        glBindTexture(GL_TEXTURE_2D, readback_tex_);

        if (high_precision_path_)
        {
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA32F,
                static_cast<GLsizei>(w),
                static_cast<GLsizei>(h),
                0,
                GL_RGBA,
                GL_FLOAT,
                nullptr);

            if (gl_has_error("glTexImage2D(readback RGBA32F)"))
            {
                std::cerr << "FP32 readback target unsupported at runtime; "
                             "falling back to RGBA8 path\n";
                high_precision_path_ = false;
            }
            else
            {
                GLint prev_fb = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, readback_tex_, 0);

                bool hp_ok = false;
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
                {
                    glViewport(0, 0, 1, 1);
                    const float testv = 0.001f; // < 1/255 so 8-bit will quantize to 0, half-float will keep it
                    glClearColor(testv, testv, testv, testv);
                    glClear(GL_COLOR_BUFFER_BIT);

                    float tmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, tmp);
                    glFinish();

                    if (!gl_has_error("sanity-readback(glReadPixels)") && tmp[0] > 5e-4f)
                    {
                        hp_ok = true;
                        std::cerr << "FP32 readback target verification passed (got " << tmp[0] << ")\n";
                    }
                }

                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fb));

                if (!hp_ok)
                {
                    std::cerr << "Readback: RGBA32F appears not usable at runtime -> falling back\n";
                    high_precision_path_ = false;
                }
            }
        }

        if (!high_precision_path_)
        {
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA,
                static_cast<GLsizei>(w),
                static_cast<GLsizei>(h),
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                nullptr);

            if (gl_has_error("glTexImage2D(readback RGBA8)"))
                return false;
        }

        rb_w_ = w;
        rb_h_ = h;
        return true;
    }

    GLuint compile_shader(GLenum type, const char *src)
    {
        GLuint s = glCreateShader(type);
        if (!s)
            return 0;

        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);

        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {};
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::cerr << "Shader compile failed: " << log << "\n";
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool gl_has_error(const char *stage)
    {
        bool failed = false;
        for (;;)
        {
            GLenum e = glGetError();
            if (e == GL_NO_ERROR)
                break;
            failed = true;
            if (e == GL_CONTEXT_LOST)
            {
                context_lost_ = true;
                std::cerr << stage << ": GL_CONTEXT_LOST\n";
            }
            else
            {
                std::cerr << stage << ": gl error 0x" << std::hex << e << std::dec << "\n";
            }
        }
        return failed;
    }

    bool build_blit_program()
    {
        static const char *kVs = R"(
      attribute vec2 a_pos;
      attribute vec2 a_uv;
      varying vec2 v_uv;
      void main() {
        v_uv = a_uv;
        gl_Position = vec4(a_pos, 0.0, 1.0);
      }
    )";

        static const char *kFs = R"(
      #extension GL_OES_EGL_image_external : require
      precision highp float;
      varying vec2 v_uv;
      uniform samplerExternalOES u_tex;
      uniform vec2 u_uv_off;
      uniform vec2 u_uv_scale;
      void main() {
        vec2 uv = u_uv_off + (v_uv * u_uv_scale);
        gl_FragColor = texture2D(u_tex, uv);
      }
    )";

        GLuint vs = compile_shader(GL_VERTEX_SHADER, kVs);
        if (!vs)
            return false;
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFs);
        if (!fs)
        {
            glDeleteShader(vs);
            return false;
        }

        prog_ = glCreateProgram();
        glAttachShader(prog_, vs);
        glAttachShader(prog_, fs);
        glBindAttribLocation(prog_, 0, "a_pos");
        glBindAttribLocation(prog_, 1, "a_uv");
        glLinkProgram(prog_);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint ok = 0;
        glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {};
            glGetProgramInfoLog(prog_, sizeof(log), nullptr, log);
            std::cerr << "Program link failed: " << log << "\n";
            glDeleteProgram(prog_);
            prog_ = 0;
            return false;
        }

        loc_pos_ = glGetAttribLocation(prog_, "a_pos");
        loc_uv_ = glGetAttribLocation(prog_, "a_uv");
        loc_tex_ = glGetUniformLocation(prog_, "u_tex");
        loc_uv_off_ = glGetUniformLocation(prog_, "u_uv_off");
        loc_uv_scale_ = glGetUniformLocation(prog_, "u_uv_scale");
        return loc_pos_ >= 0 && loc_uv_ >= 0 && loc_tex_ >= 0 && loc_uv_off_ >= 0 && loc_uv_scale_ >= 0;
    }

private:
    void shutdown()
    {
        if (egl_dpy_ != EGL_NO_DISPLAY)
        {
            EGLSurface s = surfaceless_ ? EGL_NO_SURFACE : egl_surf_;
            if (egl_ctx_ != EGL_NO_CONTEXT)
            {
                eglMakeCurrent(egl_dpy_, s, s, egl_ctx_);
            }

            if (vbo_)
                glDeleteBuffers(1, &vbo_);
            if (prog_)
                glDeleteProgram(prog_);
            if (fbo_)
                glDeleteFramebuffers(1, &fbo_);
            if (readback_tex_)
                glDeleteTextures(1, &readback_tex_);
            if (import_tex_)
                glDeleteTextures(1, &import_tex_);

            if (egl_ctx_ != EGL_NO_CONTEXT)
            {
                eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(egl_dpy_, egl_ctx_);
            }
            if (egl_surf_ != EGL_NO_SURFACE)
                eglDestroySurface(egl_dpy_, egl_surf_);
            eglTerminate(egl_dpy_);
        }

        if (gbm_)
            gbm_device_destroy(gbm_);

        gbm_ = nullptr;
        egl_dpy_ = EGL_NO_DISPLAY;
        egl_ctx_ = EGL_NO_CONTEXT;
        egl_surf_ = EGL_NO_SURFACE;
        import_tex_ = 0;
        readback_tex_ = 0;
        fbo_ = 0;
        prog_ = 0;
        vbo_ = 0;
        rb_w_ = 0;
        rb_h_ = 0;
    }

private:
    gbm_device *gbm_{nullptr};
    EGLDisplay egl_dpy_{EGL_NO_DISPLAY};
    EGLContext egl_ctx_{EGL_NO_CONTEXT};
    EGLSurface egl_surf_{EGL_NO_SURFACE};
    bool surfaceless_{false};
    int gles_major_{2};
    bool high_precision_path_{false};

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_{nullptr};

    GLuint import_tex_{0};
    GLuint readback_tex_{0};
    GLuint fbo_{0};
    GLuint prog_{0};
    GLuint vbo_{0};

    GLint loc_pos_{-1};
    GLint loc_uv_{-1};
    GLint loc_tex_{-1};
    GLint loc_uv_off_{-1};
    GLint loc_uv_scale_{-1};

    bool sync_warned_{false};
    bool context_lost_{false};
    uint32_t rb_w_{0};
    uint32_t rb_h_{0};
};

struct SlurpRegion
{
    int32_t x{0};
    int32_t y{0};
    uint32_t w{0};
    uint32_t h{0};
};

struct CropRect
{
    uint32_t x{0}; // buffer-space x
    uint32_t y{0}; // buffer-space y (top-origin row index)
    uint32_t w{0};
    uint32_t h{0};
};

// Map slurp global top-left rect -> local capture rect in rgba16 buffer space.
// Handles plane offset and scaling (CRTC_X/Y/W/H), without flipping the image.
static std::optional<CropRect> compute_crop_rect_for_buffer(
    const SlurpRegion &slurp,
    int32_t capture_global_x,
    int32_t capture_global_y,
    uint32_t capture_global_w,
    uint32_t capture_global_h,
    uint32_t buf_w,
    uint32_t buf_h,
    double slurp_scale_x,
    double slurp_scale_y)
{
    if (capture_global_w == 0 || capture_global_h == 0 || buf_w == 0 || buf_h == 0)
        return std::nullopt;
    if (slurp.w == 0 || slurp.h == 0)
        return std::nullopt;
    if (!(slurp_scale_x > 0.0) || !(slurp_scale_y > 0.0))
        return std::nullopt;

    // slurp logical -> physical/global
    const double slurp_x0 = static_cast<double>(slurp.x) * slurp_scale_x;
    const double slurp_y0 = static_cast<double>(slurp.y) * slurp_scale_y;
    const double slurp_x1 = static_cast<double>(slurp.x + static_cast<int32_t>(slurp.w)) * slurp_scale_x;
    const double slurp_y1 = static_cast<double>(slurp.y + static_cast<int32_t>(slurp.h)) * slurp_scale_y;

    const double sx = static_cast<double>(buf_w) / static_cast<double>(capture_global_w);
    const double sy = static_cast<double>(buf_h) / static_cast<double>(capture_global_h);

    const double lx0 = (slurp_x0 - static_cast<double>(capture_global_x)) * sx;
    const double ly0 = (slurp_y0 - static_cast<double>(capture_global_y)) * sy;
    const double lx1 = (slurp_x1 - static_cast<double>(capture_global_x)) * sx;
    const double ly1 = (slurp_y1 - static_cast<double>(capture_global_y)) * sy;

    const int64_t x0i = static_cast<int64_t>(std::floor(lx0));
    const int64_t y0i = static_cast<int64_t>(std::floor(ly0));
    const int64_t x1i = static_cast<int64_t>(std::ceil(lx1));
    const int64_t y1i = static_cast<int64_t>(std::ceil(ly1));

    const int64_t x0 = std::max<int64_t>(0, x0i);
    const int64_t y0 = std::max<int64_t>(0, y0i);
    const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(buf_w), x1i);
    const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(buf_h), y1i);

    if (x1 <= x0 || y1 <= y0)
        return std::nullopt;

    return CropRect{
        static_cast<uint32_t>(x0),
        static_cast<uint32_t>(y0),
        static_cast<uint32_t>(x1 - x0),
        static_cast<uint32_t>(y1 - y0)};
}

// Reads stdin for region info in the format x,y\swxh, e.g. "100,200 1280x720"
void read_region_info(SlurpRegion &region)
{
    std::string line;
    if (!std::getline(std::cin, line))
    {
        std::cerr << "Failed to read region info from stdin\n";
        return;
    }

    size_t space = line.find(' ');
    if (space == std::string::npos)
    {
        std::cerr << "Invalid region info format\n";
        return;
    }

    std::string pos = line.substr(0, space);
    std::string size = line.substr(space + 1);

    size_t comma = pos.find(',');
    if (comma == std::string::npos)
    {
        std::cerr << "Invalid position format\n";
        return;
    }

    try
    {
        region.x = static_cast<int32_t>(std::stol(pos.substr(0, comma)));
        region.y = static_cast<int32_t>(std::stol(pos.substr(comma + 1)));

        size_t x = size.find('x');
        if (x == std::string::npos)
        {
            std::cerr << "Invalid size format\n";
            return;
        }

        region.w = static_cast<uint32_t>(std::stoul(size.substr(0, x)));
        region.h = static_cast<uint32_t>(std::stoul(size.substr(x + 1)));
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error parsing region info: " << e.what() << "\n";
    }
}


static void usage(const char *argv0)
{
    std::cerr << "Usage: " << argv0
              << " [--card /dev/dri/card0] [--monitor N] [--frames N] [--fps N] [--out frames.rgba64le] [--dmabuf-sync] [--stdout] [--slurp] [--slurp-scale S|SX,SY]"
              << " [--pp-y4m] [--max-nits N] [--sdr-linear-12bpc]\n";
}

int main(int argc, char **argv)
{
    std::string card_path = "/dev/dri/card0";
    std::string out_path = "frames.rgba64le";
    int frames = 120;
    int fps = 30;
    int monitor = 0;
    bool dmabuf_sync = false;
    bool write_to_stdout = false;
    bool use_slurp = false;
    double slurp_scale_x = 1.0;
    double slurp_scale_y = 1.0;
    bool pp_y4m = false;
    float pp_max_nits = 1261.0f;
    bool sdr_linear_12bpc = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--card" && i + 1 < argc)
            card_path = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            out_path = argv[++i];
        else if (a == "--frames" && i + 1 < argc)
            frames = std::stoi(argv[++i]);
        else if (a == "--fps" && i + 1 < argc)
            fps = std::stoi(argv[++i]);
        else if (a == "--monitor" && i + 1 < argc)
            monitor = std::stoi(argv[++i]);
        else if (a == "--dmabuf-sync")
            dmabuf_sync = true;
        else if (a == "--stdout")
            write_to_stdout = true;
        else if (a == "--slurp")
            use_slurp = true;
        else if (a == "--slurp-scale" && i + 1 < argc)
        {
            std::string v = argv[++i];
            auto comma = v.find(',');
            if (comma == std::string::npos)
            {
                slurp_scale_x = std::stod(v);
                slurp_scale_y = slurp_scale_x;
            }
            else
            {
                slurp_scale_x = std::stod(v.substr(0, comma));
                slurp_scale_y = std::stod(v.substr(comma + 1));
            }
        }
        else if (a == "--pp-y4m")
            pp_y4m = true;
        else if (a == "--max-nits" && i + 1 < argc)
            pp_max_nits = std::stof(argv[++i]);
        else if (a == "--sdr-linear-12bpc")
            sdr_linear_12bpc = true;
        else
        {
            usage(argv[0]);
            return 1;
        }
    }

    if (pp_y4m && sdr_linear_12bpc)
    {
        std::cerr << "--sdr-linear-12bpc is only for raw RGBA output path (without --pp-y4m)\n";
        return 1;
    }

    SlurpRegion region{};
    if (use_slurp)
    {
        std::cerr << "Waiting for region info on stdin...\n";
        read_region_info(region);
        std::cerr << "Got region: x=" << region.x << " y=" << region.y
                  << " w=" << region.w << " h=" << region.h
                  << " (slurp-scale " << slurp_scale_x << "," << slurp_scale_y << ")\n";
    }

    ScopedFd card(::open(card_path.c_str(), O_RDWR | O_CLOEXEC));
    if (card.fd < 0)
    {
        std::cerr << "open(" << card_path << ") failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    drmSetClientCap(card.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(card.fd, DRM_CLIENT_CAP_ATOMIC, 1);

    print_display_metadata(card.fd);

    auto selected = find_capture_plane(card.fd, monitor);
    if (!selected)
    {
        std::cerr << "No active capture plane found\n";
        return 1;
    }

    std::cerr << "Selected plane_id=" << selected->plane_id
              << " crtc_id=" << selected->crtc_id
              << " fb_id=" << selected->fb_id << "\n";

    log_panel_orientation(card.fd, selected->plane_id);

    auto connector_id = find_connector_for_crtc(card.fd, selected->crtc_id);
    if (connector_id)
    {
        std::cerr << "Selected connector_id=" << *connector_id << "\n";
        log_hdr_metadata(card.fd, *connector_id);
    }
    else
    {
        std::cerr << "No connected connector matched selected CRTC\n";
    }

    log_color_and_range_info(card.fd, selected->plane_id, selected->crtc_id, connector_id);

    int colorspace_idx = 0;
    int broadcast_rgb_idx = 0;
    int max_bpc = 8;

    if (connector_id)
    {
        if (auto v = get_object_prop_info(card.fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "Colorspace"))
            colorspace_idx = static_cast<int>(v->value);
        if (auto v = get_object_prop_info(card.fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "Broadcast RGB"))
            broadcast_rgb_idx = static_cast<int>(v->value);
        if (auto v = get_object_prop_info(card.fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "max bpc"))
            max_bpc = static_cast<int>(v->value);
    }

    auto reader = std::make_unique<DmabufGlReader>();
    if (!reader->init(card.fd))
        return 1;

    std::ofstream out;

    if (!write_to_stdout)
    {
        out.open(out_path, std::ios::binary);
        if (!out)
        {
            std::cerr << "Cannot open output file: " << out_path << "\n";
            return 1;
        }
    }

    const auto frame_delay = std::chrono::milliseconds(1000 / std::max(1, fps));
    std::vector<float> rgba32f;
    std::vector<uint16_t> rgba16;
    std::vector<uint16_t> y10, u10, v10;
    bool y4m_header_written = false;

    uint32_t out_w = 0, out_h = 0;
    uint32_t last_written_w = 0, last_written_h = 0;
    bool crop_warned = false;

    for (int i = 0; i < frames; ++i)
    {
        PlanePtr plane(drmModeGetPlane(card.fd, selected->plane_id), drmModeFreePlane);
        if (!plane || plane->fb_id == 0)
        {
            selected = find_capture_plane(card.fd, monitor);
            if (!selected)
                break;
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        auto fb = FramebufferInfo::load(card.fd, plane->fb_id);
        if (!fb || fb->handles[0] == 0)
        {
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        if (!is_single_plane_rgb_fourcc(fb->fourcc))
        {
            std::cerr << "Unsupported framebuffer format: " << fourcc_to_string(fb->fourcc) << "\n";
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        auto geom = get_plane_geometry(card.fd, selected->plane_id, fb->width, fb->height);

        uint32_t cap_w = geom.crtc_w;
        uint32_t cap_h = geom.crtc_h;

        float uv_off_x = std::clamp(geom.src_x / std::max(1.0f, static_cast<float>(fb->width)), 0.0f, 1.0f);
        float uv_off_y = std::clamp(geom.src_y / std::max(1.0f, static_cast<float>(fb->height)), 0.0f, 1.0f);
        float uv_scale_x = std::clamp(geom.src_w / std::max(1.0f, static_cast<float>(fb->width)), 0.0f, 1.0f);
        float uv_scale_y = std::clamp(geom.src_h / std::max(1.0f, static_cast<float>(fb->height)), 0.0f, 1.0f);

        if (out_w == 0)
        {
            out_w = cap_w;
            out_h = cap_h;
        }
        if (cap_w != out_w || cap_h != out_h)
        {
            std::cerr << "Geometry changed from " << out_w << "x" << out_h
                      << " to " << cap_w << "x" << cap_h << " (stop/reinit)\n";
            break;
        }

        if (!use_slurp)
        {
            region.x = 0;
            region.y = 0;
            region.w = out_w;
            region.h = out_h;
        }

        int dmabuf_fd = -1;
        if (drmPrimeHandleToFD(card.fd, fb->handles[0], DRM_CLOEXEC, &dmabuf_fd) != 0)
        {
            std::this_thread::sleep_for(frame_delay);
            continue;
        }
        ScopedFd dma(dmabuf_fd);

        if (!reader->read_dmabuf_to_rgba32f(
                dma.fd,
                fb->width, fb->height,
                cap_w, cap_h,
                uv_off_x, uv_off_y,
                uv_scale_x, uv_scale_y,
                fb->fourcc,
                fb->pitches[0],
                fb->offsets[0],
                fb->modifier,
                dmabuf_sync,
                rgba32f))
        {
            if (reader->context_lost())
            {
                reader = std::make_unique<DmabufGlReader>();
                if (!reader->init(card.fd))
                    return 1;
            }
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        uint32_t frame_w = out_w;
        uint32_t frame_h = out_h;

        if (use_slurp)
        {
            CrtcPtr crtc(drmModeGetCrtc(card.fd, selected->crtc_id), drmModeFreeCrtc);
            if (!crtc)
            {
                std::this_thread::sleep_for(frame_delay);
                continue;
            }

            const int32_t capture_global_x = crtc->x + geom.crtc_x;
            const int32_t capture_global_y = crtc->y + geom.crtc_y;

            auto crop = compute_crop_rect_for_buffer(
                region,
                capture_global_x,
                capture_global_y,
                geom.crtc_w,
                geom.crtc_h,
                out_w,
                out_h,
                slurp_scale_x,
                slurp_scale_y);

            if (!crop)
            {
                if (!crop_warned)
                {
                    crop_warned = true;
                    std::cerr << "Slurp region is outside selected monitor/capture area; skipping frames\n";
                }
                std::this_thread::sleep_for(frame_delay);
                continue;
            }

            std::vector<float> cropped;
            cropped.reserve(static_cast<size_t>(crop->w) * crop->h * 4u);

            for (uint32_t y = 0; y < crop->h; ++y)
            {
                const float *src = rgba32f.data() + ((crop->y + y) * out_w + crop->x) * 4u;
                cropped.insert(cropped.end(), src, src + static_cast<size_t>(crop->w) * 4u);
            }
            rgba32f = std::move(cropped);

            frame_w = crop->w;
            frame_h = crop->h;
        }

        if (pp_y4m)
        {
            if (!kmshot::transform_rgba32f_to_yuv444p10(
                    rgba32f.data(),
                    frame_w,
                    frame_h,
                    colorspace_idx,
                    pp_max_nits,
                    max_bpc,
                    y10, u10, v10))
            {
                std::this_thread::sleep_for(frame_delay);
                continue;
            }

            std::ostream &os = write_to_stdout ? static_cast<std::ostream &>(std::cout)
                                               : static_cast<std::ostream &>(out);

            if (!y4m_header_written)
            {
                if (!kmshot::write_y4m_header(os, frame_w, frame_h, std::max(1, fps), 1))
                    return 1;
                y4m_header_written = true;
            }

            if (!kmshot::write_y4m_frame(os, y10, u10, v10))
                return 1;
        }
        else
        {
            // Final quantization just before output
            rgba16.resize(rgba32f.size());
            const size_t pixels = rgba32f.size() / 4u;

            const bool use_linear12_path = sdr_linear_12bpc && (colorspace_idx == 0);
            if (sdr_linear_12bpc && colorspace_idx != 0)
            {
                static bool warned_non_sdr = false;
                if (!warned_non_sdr)
                {
                    warned_non_sdr = true;
                    std::cerr << "--sdr-linear-12bpc requested, but connector Colorspace="
                              << colorspace_idx << " (not SDR enum 0). Falling back to RGBA64 path.\n";
                }
            }

                        for (size_t p = 0; p < pixels; ++p)
            {
                const float *s = rgba32f.data() + p * 4u;
                uint16_t *d = rgba16.data() + p * 4u;

                if (use_linear12_path)
                {
                    // gamma-encoded blending-space RGB -> linear (gamma 2.2 decode),
                    // quantized to 12-bit and stored MSB-aligned in 16-bit lanes.
                    const float r_lin = std::pow(std::clamp(s[0], 0.0f, 1.0f), 2.2f);
                    const float g_lin = std::pow(std::clamp(s[1], 0.0f, 1.0f), 2.2f);
                    const float b_lin = std::pow(std::clamp(s[2], 0.0f, 1.0f), 2.2f);

                    d[0] = float01_to_u12_msb16_sat(r_lin);
                    d[1] = float01_to_u12_msb16_sat(g_lin);
                    d[2] = float01_to_u12_msb16_sat(b_lin);
                    d[3] = 0xFFF0u;
                }
                else
                {
                    d[0] = float01_to_u16_sat(s[0]);
                    d[1] = float01_to_u16_sat(s[1]);
                    d[2] = float01_to_u16_sat(s[2]);
                    d[3] = 65535u;
                }
            }

            constexpr int FN_W = 8;
            constexpr int DIM_W = 6;
            constexpr int FMT_W = 8;
            constexpr int SMALL_W = 3;

            /*
            std::cout << "FRAME ";
            std::cout << std::setfill(' ') << std::setw(FN_W) << i << ' ';
            std::cout << std::setw(DIM_W) << frame_w << ' ' << std::setw(DIM_W) << frame_h << ' ';
            std::cout << std::setw(FMT_W) << "RGBA64" << "\n";

            std::cout << "SPACE " << std::setw(SMALL_W) << colorspace_idx << "\n";
            std::cout << "BROADCAST_RGB " << std::setw(SMALL_W) << broadcast_rgb_idx << "\n";
            std::cout << "MAX_BPC " << std::setw(4) << max_bpc << "\n";
            std::cout << std::flush;
            */

            if (write_to_stdout)
            {
                std::cout.write(
                    reinterpret_cast<const char *>(rgba16.data()),
                    static_cast<std::streamsize>(rgba16.size() * sizeof(uint16_t)));
                if (!std::cout)
                    return 1;
            }
            else
            {
                out.write(
                    reinterpret_cast<const char *>(rgba16.data()),
                    static_cast<std::streamsize>(rgba16.size() * sizeof(uint16_t)));
                if (!out)
                    return 1;
            }
        }

        last_written_w = frame_w;
        last_written_h = frame_h;

        std::this_thread::sleep_for(frame_delay);
    }

    const uint32_t final_w = last_written_w ? last_written_w : out_w;
    const uint32_t final_h = last_written_h ? last_written_h : out_h;

    std::cerr << "ffplay -f rawvideo -pixel_format rgba64le -video_size "
              << final_w << "x" << final_h << " " << out_path << "\n";
    return 0;
}
