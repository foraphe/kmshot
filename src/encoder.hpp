#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// Encodes one captured frame to AVIF with libavif. AVIF output is always a
// still image: image sequences written by libavif 1.4 crash in
// avifEncoderFinish when content light level metadata is present, and a single
// screenshot is what this tool is for.
class AvifWriter
{
public:
    AvifWriter();
    ~AvifWriter();
    AvifWriter(const AvifWriter &) = delete;
    AvifWriter &operator=(const AvifWriter &) = delete;

    bool open(const std::string &path,
              uint32_t width,
              uint32_t height,
              const AvifSettings &settings,
              const ColorTransformConfig &color,
              std::string &error);

    bool is_open() const;

    bool add_frame(const float *rgba, const ColorTransformConfig &color, std::string &error);

    // Adds a frame whose target-space RGB has already been produced (the GPU
    // colour shader), as interleaved 16-bit samples. libavif converts it to YUV
    // and performs the chroma downsampling exactly like the float path.
    bool add_frame_rgb16(std::vector<uint16_t> &&rgb, std::string &error);

    // Encodes and writes the file. Safe to call when nothing was opened.
    bool finish(std::string &error);

private:
    bool submit(std::string &error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kmshot
