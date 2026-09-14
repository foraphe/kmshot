#include "capture.hpp"
#include "cli.hpp"
#include "color_math.hpp"
#include "color_profile.hpp"
#include "drm_debug.hpp"
#include "drm_util.hpp"
#include "edid.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

using namespace kmshot;

namespace
{

// Fallback used when neither --max-nits nor a usable EDID is available.
constexpr double kDefaultMaxNits = 1261.0; // BOE NE160QDM-NM7 EDID maximum luminance

void list_gamuts()
{
    std::cout << "Built-in gamut presets:\n";
    for (const auto &preset : gamut_presets())
        std::cout << "  " << preset.name << "\n      " << preset.description << "\n";
}

std::optional<EdidInfo> load_edid(const Options &opts,
                                  int card_fd,
                                  const std::optional<uint32_t> &connector_id)
{
    if (!opts.use_edid)
    {
        std::cerr << "EDID disabled by --no-edid; using the fallback display profile\n";
        return std::nullopt;
    }

    std::vector<uint8_t> blob;
    std::string error;
    bool ok = false;

    if (!opts.edid_path.empty())
    {
        ok = read_edid_file(opts.edid_path, blob, error);
    }
    else if (connector_id)
    {
        ok = read_connector_edid(card_fd, *connector_id, blob, error);
    }
    else
    {
        error = "no connected connector matches the selected CRTC";
    }

    if (!ok)
    {
        std::cerr << "EDID unavailable (" << error << "); using the fallback display profile\n";
        return std::nullopt;
    }

    EdidInfo info;
    parse_edid(blob.data(), blob.size(), info);
    // When --print-edid was requested the caller writes the report to stdout.
    if (!opts.print_edid)
        std::cerr << describe_edid(info);
    return info;
}

} // namespace

int main(int argc, char **argv)
{
    Options opts;
    std::string error;

    switch (parse_options(argc, argv, opts, error))
    {
    case ParseStatus::ExitSuccess:
        if (opts.show_help)
            print_usage(std::cout, argv[0]);
        else if (opts.list_gamuts)
            list_gamuts();
        return 0;
    case ParseStatus::Error:
        std::cerr << "error: " << error << "\n\n";
        print_usage(std::cerr, argv[0]);
        return 1;
    case ParseStatus::Ok:
        break;
    }

    // Offline EDID inspection does not need a DRM node or stdin.
    if (opts.print_edid && !opts.edid_path.empty())
    {
        std::vector<uint8_t> blob;
        if (!read_edid_file(opts.edid_path, blob, error))
        {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        EdidInfo info;
        parse_edid(blob.data(), blob.size(), info);
        std::cout << describe_edid(info);
        return info.valid ? 0 : 1;
    }

    SlurpRegion region{};
    if (opts.use_slurp)
    {
        std::cerr << "Waiting for region info on stdin...\n";
        if (!read_region_info(region))
            return 1;
        std::cerr << "Got region: x=" << region.x << " y=" << region.y
                  << " w=" << region.w << " h=" << region.h
                  << " (slurp-scale " << opts.slurp_scale_x << "," << opts.slurp_scale_y << ")\n";
    }

    ScopedFd card(::open(opts.card_path.c_str(), O_RDWR | O_CLOEXEC));
    if (card.fd < 0)
    {
        std::cerr << "open(" << opts.card_path << ") failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    drmSetClientCap(card.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(card.fd, DRM_CLIENT_CAP_ATOMIC, 1);

    print_display_metadata(card.fd);

    auto selected = find_capture_plane(card.fd, opts.monitor);
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
    if (connector_id)
    {
        if (auto v = get_object_prop_info(card.fd, *connector_id, DRM_MODE_OBJECT_CONNECTOR, "Colorspace"))
            colorspace_idx = static_cast<int>(v->value);
    }
    if (opts.colorspace_override)
    {
        std::cerr << "Colorspace overridden by --colorspace: " << colorspace_idx
                  << " -> " << *opts.colorspace_override << "\n";
        colorspace_idx = *opts.colorspace_override;
    }

    auto edid = load_edid(opts, card.fd, connector_id);

    if (opts.print_edid)
    {
        if (edid && edid->valid)
        {
            std::cout << describe_edid(*edid);
            return 0;
        }
        return 1;
    }

    const DisplayProfile profile = resolve_display_profile(opts, edid);

    double max_nits = opts.pp_max_nits_explicit ? static_cast<double>(opts.pp_max_nits) : 0.0;
    if (!opts.pp_max_nits_explicit)
    {
        if (edid && edid->hdr.has_max_luminance)
        {
            max_nits = edid->hdr.max_luminance;
            std::cerr << "Using EDID HDR max luminance " << max_nits
                      << " cd/m^2 as the PQ reference (override with --max-nits)\n";
        }
        else
        {
            max_nits = kDefaultMaxNits;
            std::cerr << "No EDID HDR max luminance available; defaulting the PQ reference to "
                      << max_nits << " cd/m^2 (override with --max-nits)\n";
        }
    }

    auto color = build_color_transform(opts, profile, colorspace_idx, max_nits, error);
    if (!color)
    {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    // Default the AVIF content light level to the EDID values for HDR captures
    // unless the user supplied --avif-clli.
    if (!opts.avif.clli && color->source == SourceEncoding::HdrPqBt2020 &&
        edid && edid->hdr.has_max_luminance)
    {
        const auto to_u16 = [](double v)
        {
            return static_cast<uint16_t>(std::lround(std::min(65535.0, std::max(0.0, v))));
        };
        const uint16_t max_cll = to_u16(edid->hdr.max_luminance);
        const uint16_t max_pall = edid->hdr.has_max_frame_avg_luminance
                                      ? to_u16(edid->hdr.max_frame_avg_luminance)
                                      : max_cll;
        opts.avif.clli = std::make_pair(max_cll, max_pall);
        std::cerr << "AVIF CLLI from EDID: MaxCLL=" << max_cll
                  << " MaxPALL=" << max_pall << " cd/m^2\n";
    }

    return run_capture(opts, card.fd, *color, colorspace_idx, region, *selected);
}
