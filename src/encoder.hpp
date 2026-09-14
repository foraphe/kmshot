#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "color_transform.hpp"

namespace kmshot
{

// Settings for the in-process AVIF encoder. Quality and speed are deliberately
// not configurable: a screenshot is encoded once, at maximum quality.
struct AvifSettings
{
    // Chroma subsampling of the output: "444", "422" or "420". The downsampling
    // itself is done by libavif while it converts the target RGB image to YUV.
    std::string subsampling{"444"};

    // CICP overrides. When unset they are derived from the colour pipeline:
    // primaries and matrix follow the target space (BT.2020 for HDR), and the
    // transfer function follows the source encoding (sRGB for SDR, PQ for HDR).
    std::optional<int> cicp_primaries;
    std::optional<int> cicp_transfer;
    std::optional<int> cicp_matrix;

    // Content light level in cd/m^2 as (MaxCLL, MaxPALL). When unset the clli
    // box is written for HDR captures using the EDID values and omitted
    // otherwise.
    std::optional<std::pair<uint16_t, uint16_t>> clli;
};

// Encodes captured RGBA float frames to AVIF with libavif. A single frame
// produces a still image, several frames produce an image sequence.
class AvifWriter
{
public:
    AvifWriter();
    ~AvifWriter();
    AvifWriter(const AvifWriter &) = delete;
    AvifWriter &operator=(const AvifWriter &) = delete;

    // `timescale` is the sequence frame rate in Hz and is only used when
    // `sequence` is true.
    bool open(const std::string &path,
              uint32_t width,
              uint32_t height,
              const AvifSettings &settings,
              const ColorTransformConfig &color,
              bool sequence,
              uint32_t timescale,
              std::string &error);

    bool is_open() const;

    bool add_frame(const float *rgba, const ColorTransformConfig &color, std::string &error);

    // Encodes and writes the file. Safe to call when nothing was opened.
    bool finish(std::string &error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kmshot
