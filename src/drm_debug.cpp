#include "drm_debug.hpp"

#include "drm_util.hpp"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace kmshot
{

namespace
{

const char *hdr_eotf_to_string(uint8_t eotf)
{
    switch (eotf)
    {
    case 0: return "TRADITIONAL_GAMMA_SDR";
    case 1: return "TRADITIONAL_GAMMA_HDR";
    case 2: return "SMPTE_ST2084";
    case 3: return "ARIB_STD_B67";
    default: return "UNKNOWN";
    }
}

const char *hdr_metadata_type_to_string(uint8_t metadata_type)
{
    switch (metadata_type)
    {
    case 0: return "STATIC_METADATA_TYPE_1";
    default: return "UNKNOWN";
    }
}

void log_prop_if_present(
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

} // namespace

void print_display_metadata(int card_fd)
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

void log_panel_orientation(int card_fd, uint32_t plane_id)
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

void log_hdr_metadata(int card_fd, uint32_t connector_id)
{
    auto hdr_blob_id = get_object_prop_u64(
        card_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA");
    if (!hdr_blob_id)
    {
        std::cerr << "HDR_OUTPUT_METADATA: property not available on connector "
                  << connector_id << "\n";
        return;
    }

    if (*hdr_blob_id == 0)
    {
        std::cerr << "HDR_OUTPUT_METADATA: blob_id=0 (no active static HDR metadata)\n";
        return;
    }

    BlobPtr blob(
        drmModeGetPropertyBlob(card_fd, static_cast<uint32_t>(*hdr_blob_id)),
        drmModeFreePropertyBlob);
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

    const auto *raw = reinterpret_cast<const hdr_output_metadata *>(blob->data);
    const auto eotf = static_cast<uint8_t>(raw->hdmi_metadata_type1.eotf);
    const auto metadata_type = static_cast<uint8_t>(raw->hdmi_metadata_type1.metadata_type);
    const auto max_cll = static_cast<uint32_t>(raw->hdmi_metadata_type1.max_cll);
    const auto max_fall = static_cast<uint32_t>(raw->hdmi_metadata_type1.max_fall);
    const auto max_luma = static_cast<uint32_t>(raw->hdmi_metadata_type1.max_display_mastering_luminance);
    const auto min_luma = static_cast<uint32_t>(raw->hdmi_metadata_type1.min_display_mastering_luminance);

    std::cerr << "HDR metadata:"
              << " metadata_type=" << static_cast<uint32_t>(metadata_type)
              << " (" << hdr_metadata_type_to_string(metadata_type) << ")"
              << " eotf=" << static_cast<uint32_t>(eotf)
              << " (" << hdr_eotf_to_string(eotf) << ")"
              << " max_cll=" << max_cll
              << " max_fall=" << max_fall
              << " max_luma=" << max_luma
              << " min_luma=" << min_luma
              << "\n";
}

void log_color_and_range_info(
    int card_fd, uint32_t plane_id, uint32_t crtc_id, const std::optional<uint32_t> &connector_id)
{
    std::cerr << "Color/range capability probe:\n";

    if (connector_id)
    {
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "Colorspace");
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "Broadcast RGB");
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "max bpc");
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "content type");
        log_prop_if_present(card_fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "connector", "EDID");
    }

    log_prop_if_present(card_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "crtc", "Colorspace");
    log_prop_if_present(card_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "crtc", "COLOR_ENCODING");
    log_prop_if_present(card_fd, crtc_id, DRM_MODE_OBJECT_CRTC, "crtc", "COLOR_RANGE");

    log_prop_if_present(card_fd, plane_id, DRM_MODE_OBJECT_PLANE, "plane", "COLOR_ENCODING");
    log_prop_if_present(card_fd, plane_id, DRM_MODE_OBJECT_PLANE, "plane", "COLOR_RANGE");
}

} // namespace kmshot
