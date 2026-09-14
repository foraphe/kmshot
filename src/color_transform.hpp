#pragma once

#include <cstdint>
#include <vector>

#include "color_math.hpp"

namespace kmshot
{

// How the pixel values produced by the compositor should be interpreted.
enum class SourceEncoding
{
    // Connector Colorspace = 0 (Default): gamma-encoded values in the display's
    // native primaries, which is what most compositors blend in for SDR.
    SdrDisplayNative,
    // Connector Colorspace = 9: BT.2020 primaries, already PQ encoded unless
    // the compositor hands out gamma 2.2 encoded data (observed on KDE).
    HdrPqBt2020,
    // Any other colorspace: no gamut conversion is performed, the values are
    // assumed to already live in the target space.
    AssumedTarget,
};

struct ColorTransformConfig
{
    SourceEncoding source{SourceEncoding::SdrDisplayNative};

    // Display native linear RGB -> target linear RGB. Unused for HdrPqBt2020
    // and AssumedTarget.
    Mat3 display_to_target{Mat3::identity()};

    // Target linear RGB -> full-range YUV (rows are Y, U, V).
    Mat3 target_rgb_to_yuv{Mat3::identity()};

    // Decode exponent applied to display native values (SDR path).
    double display_decode_gamma{2.2};

    // PQ scaling factor (max_nits / 10000).
    double pq_scale{1.0};

    // True when an HDR source still carries gamma 2.2 encoded values that need
    // to be converted back to PQ (KDE behaviour).
    bool pq_input_is_gamma22{false};
};

// PQ (ST-2084) OETF for linear light scaled to the 0..1 range of 10000 nits.
float linear_to_pq(float linear);

// Full-range RGB -> YUV matrix for the given target primaries.
Mat3 target_rgb_to_yuv_matrix(bool bt2020);

// Input: interleaved RGBA float buffer, normalized to ~[0..1] per channel.
// Output: full-range YUV444 10-bit planar (stored in uint16_t, values 0..1023).
bool transform_rgba32f_to_yuv444p10(
    const float *rgba,
    uint32_t width,
    uint32_t height,
    const ColorTransformConfig &config,
    std::vector<uint16_t> &y,
    std::vector<uint16_t> &u,
    std::vector<uint16_t> &v);

} // namespace kmshot
