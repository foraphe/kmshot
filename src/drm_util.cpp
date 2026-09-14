#include "drm_util.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

#include <drm_fourcc.h>
#include <xf86drm.h>

namespace kmshot
{

namespace
{

std::vector<uint32_t> connected_crtcs(int card_fd)
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

} // namespace

PlaneType get_plane_type(int card_fd, uint32_t plane_id)
{
    ObjPropsPtr props(
        drmModeObjectGetProperties(card_fd, plane_id, DRM_MODE_OBJECT_PLANE),
        drmModeFreeObjectProperties);
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

PlaneGeometry get_plane_geometry(int fd, uint32_t plane_id, uint32_t fb_w, uint32_t fb_h)
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

std::optional<uint64_t> get_object_prop_u64(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
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

std::optional<ObjectPropInfo> get_object_prop_info(
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

std::optional<PlaneSelection> find_capture_plane(int card_fd, int monitor_index)
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

std::optional<uint32_t> find_connector_for_crtc(int card_fd, uint32_t crtc_id)
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

std::optional<FramebufferInfo> FramebufferInfo::load(int card_fd, uint32_t fb_id)
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
        // Legacy KMS FB doesn't expose fourcc/modifier.
        // Best-effort inference for common scanout formats.
        if (fb->bpp == 32 && fb->depth == 30)
            out.fourcc = DRM_FORMAT_XRGB2101010;
        else if (fb->bpp == 32)
            out.fourcc = DRM_FORMAT_XRGB8888;
        out.modifier = DRM_FORMAT_MOD_INVALID;
        out.handles[0] = fb->handle;
        out.pitches[0] = fb->pitch;
        out.offsets[0] = 0;
        drmModeFreeFB(fb);
        return out;
    }

    return std::nullopt;
}

std::string fourcc_to_string(uint32_t f)
{
    char s[5] = {
        static_cast<char>(f & 0xff),
        static_cast<char>((f >> 8) & 0xff),
        static_cast<char>((f >> 16) & 0xff),
        static_cast<char>((f >> 24) & 0xff),
        0};
    return std::string(s);
}

bool is_single_plane_rgb_fourcc(uint32_t f)
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

bool read_connector_edid(int card_fd,
                         uint32_t connector_id,
                         std::vector<uint8_t> &out,
                         std::string &error)
{
    auto blob_id = get_object_prop_u64(card_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "EDID");
    if (!blob_id)
    {
        error = "connector " + std::to_string(connector_id) + " has no EDID property";
        return false;
    }
    if (*blob_id == 0)
    {
        error = "connector " + std::to_string(connector_id) + " has an empty EDID blob";
        return false;
    }

    BlobPtr blob(drmModeGetPropertyBlob(card_fd, static_cast<uint32_t>(*blob_id)), drmModeFreePropertyBlob);
    if (!blob || !blob->data || blob->length == 0)
    {
        error = "drmModeGetPropertyBlob(" + std::to_string(*blob_id) + ") failed";
        return false;
    }

    const auto *bytes = reinterpret_cast<const uint8_t *>(blob->data);
    out.assign(bytes, bytes + blob->length);
    return true;
}

} // namespace kmshot
