#include "color_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>

namespace kmshot
{

namespace
{

constexpr const char *kFallbackGamut = "ne160qdm-nm7";

std::string to_upper(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool desktop_is_kde()
{
    const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (!desktop)
        return false;

    const std::string value(desktop);
    size_t start = 0;
    while (start <= value.size())
    {
        const size_t end = value.find(':', start);
        const std::string part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (to_upper(part) == "KDE")
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

bool resolve_pq_input_is_gamma22(const Options &opts)
{
    switch (opts.pq_input)
    {
    case PqInput::Gamma22:
        return true;
    case PqInput::Pq:
        return false;
    case PqInput::Auto:
    default:
        return desktop_is_kde();
    }
}

Mat3 mat3_from_array(const std::array<double, 9> &v)
{
    Mat3 m;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m.m[r][c] = v[static_cast<size_t>(r) * 3 + static_cast<size_t>(c)];
    return m;
}

std::string describe_edid_origin(const EdidInfo &edid)
{
    std::string name;
    if (!edid.manufacturer.empty())
        name += edid.manufacturer;
    if (!edid.product_name.empty())
    {
        if (!name.empty())
            name += " ";
        name += edid.product_name;
    }
    if (name.empty())
        name = "unknown panel";
    return "EDID (" + name + ")";
}

} // namespace

DisplayProfile resolve_display_profile(const Options &opts, const std::optional<EdidInfo> &edid)
{
    DisplayProfile profile;

    const GamutPreset *requested =
        opts.display_gamut.empty() ? nullptr : find_gamut_preset(opts.display_gamut);

    if (requested)
    {
        profile.chroma = requested->chroma;
        profile.origin = std::string("--display-gamut ") + requested->name;
    }
    else if (opts.use_edid && edid && edid->valid && edid->has_chromaticities)
    {
        profile.chroma = edid->chroma;
        profile.origin = describe_edid_origin(*edid);
    }
    else if (const GamutPreset *fallback = find_gamut_preset(kFallbackGamut))
    {
        profile.chroma = fallback->chroma;
        profile.origin = "built-in fallback";
    }
    else
    {
        // The preset table is a compile-time constant, so this cannot happen.
        profile.origin = "built-in fallback";
    }

    if (opts.display_primaries)
    {
        const auto &p = *opts.display_primaries;
        profile.chroma.red_x = p[0];
        profile.chroma.red_y = p[1];
        profile.chroma.green_x = p[2];
        profile.chroma.green_y = p[3];
        profile.chroma.blue_x = p[4];
        profile.chroma.blue_y = p[5];
        profile.origin += " + --display-primaries";
    }

    if (opts.display_white)
    {
        profile.chroma.white_x = opts.display_white->first;
        profile.chroma.white_y = opts.display_white->second;
        profile.origin += " + --display-white";
    }

    return profile;
}

std::optional<ColorTransformConfig> build_color_transform(
    const Options &opts,
    const DisplayProfile &profile,
    int colorspace_idx,
    double max_nits,
    std::string &error)
{
    const bool hdr_pq = colorspace_idx == 9;
    const bool sdr_native = colorspace_idx == 0;

    ColorTransformConfig cfg;
    cfg.source = hdr_pq ? SourceEncoding::HdrPqBt2020
                        : (sdr_native ? SourceEncoding::SdrDisplayNative : SourceEncoding::AssumedTarget);
    cfg.display_decode_gamma = opts.display_gamma;
    cfg.pq_scale = std::max(0.0, max_nits) / 10000.0;

    const bool target_bt2020 = hdr_pq || opts.sdr_target_bt2020;
    const GamutPreset *target = find_gamut_preset(target_bt2020 ? "bt2020" : "srgb");

    cfg.target_rgb_to_yuv = target_rgb_to_yuv_matrix(target_bt2020);

    if (cfg.source == SourceEncoding::SdrDisplayNative)
    {
        if (opts.color_matrix)
        {
            cfg.display_to_target = mat3_from_array(*opts.color_matrix);
            std::cerr << "Display transform: user supplied 3x3 matrix\n";
        }
        else
        {
            auto matrix = display_to_display_matrix(profile.chroma, target->chroma);
            if (!matrix)
            {
                error = "failed to build the display -> " + std::string(target->name) +
                        " colour transform with LittleCMS2";
                return std::nullopt;
            }
            cfg.display_to_target = *matrix;
        }
    }
    else if (opts.color_matrix)
    {
        std::cerr << "Warning: --color-matrix is only applied to Colorspace=0 (SDR native) captures; "
                     "ignoring it for Colorspace=" << colorspace_idx << "\n";
    }

    // KDE blends with pure gamma 2.2 at "default" instead of PQ and then re-encodes using VCGT.
    // We take the capture before the VCGT step, so a gamma 2.2 -> PQ conversion is needed.
    // Other compositors tested (Gnome, Hyprland) does not do this.
    if (hdr_pq)
        cfg.pq_input_is_gamma22 = resolve_pq_input_is_gamma22(opts);

    std::cerr << "Colour pipeline:\n"
              << "  display profile: " << profile.origin << "\n"
              << "    R (" << profile.chroma.red_x << ", " << profile.chroma.red_y << ")\n"
              << "    G (" << profile.chroma.green_x << ", " << profile.chroma.green_y << ")\n"
              << "    B (" << profile.chroma.blue_x << ", " << profile.chroma.blue_y << ")\n"
              << "    W (" << profile.chroma.white_x << ", " << profile.chroma.white_y << ")\n"
              << "  display decode gamma: " << opts.display_gamma
              << (opts.display_gamma_explicit ? " (user)" : " (default)") << "\n"
              << "  decoder: ";

    switch (cfg.source)
    {
    case SourceEncoding::SdrDisplayNative:
        std::cerr << "SDR native primaries -> " << target->name << " (sRGB transfer)\n";
        std::cerr << "  display -> target linear RGB matrix:\n" << cfg.display_to_target.to_string() << "\n";
        break;
    case SourceEncoding::HdrPqBt2020:
        std::cerr << "HDR PQ BT.2020"
                  << (cfg.pq_input_is_gamma22 ? " (gamma 2.2 input, re-encoded to PQ)"
                                              : " (input already PQ)")
                  << "\n";
        std::cerr << "  PQ scale: " << cfg.pq_scale << " (" << max_nits << " cd/m^2 reference)\n";
        break;
    case SourceEncoding::AssumedTarget:
        std::cerr << "assumed " << target->name << " (no gamut conversion)\n";
        break;
    }
    std::cerr << "  output: full-range YUV444 16-bit, " << (target_bt2020 ? "BT.2020" : "BT.709")
              << " matrix\n";

    return cfg;
}

} // namespace kmshot
