#pragma once

#include <optional>
#include <string>

#include "cli.hpp"
#include "color_math.hpp"
#include "color_transform.hpp"
#include "edid.hpp"

namespace kmshot
{

struct DisplayProfile
{
    Chromaticities chroma{};
    std::string origin; // human readable description of where the values came from
    bool from_edid{false};
};

// Resolve the display primaries / white point from the command line overrides
// and (optionally) a parsed EDID. Never fails: falls back to the panel values
// baked into the tool when nothing else is available.
DisplayProfile resolve_display_profile(const Options &opts, const std::optional<EdidInfo> &edid);

// Build the transform configuration for the given connector colorspace value.
// Logs the resolved decision to stderr. Returns std::nullopt (with `error` set)
// when the requested conversion cannot be computed.
std::optional<ColorTransformConfig> build_color_transform(
    const Options &opts,
    const DisplayProfile &profile,
    int colorspace_idx,
    double max_nits,
    std::string &error);

} // namespace kmshot
