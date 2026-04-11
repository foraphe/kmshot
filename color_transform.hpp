#pragma once

#include <cstdint>
#include <ostream>
#include <vector>

namespace kmshot
{
float linear_to_pq(float linear);

// Input: interleaved RGBA float buffer, normalized to ~[0..1] per channel.
// Output: full-range YUV444 10-bit planar (stored in uint16_t, values 0..1023).
bool transform_rgba32f_to_yuv444p10(
    const float *rgba,
    uint32_t width,
    uint32_t height,
    int colorspace_idx,
    float max_nits,
    uint32_t bpc,
    std::vector<uint16_t> &y,
    std::vector<uint16_t> &u,
    std::vector<uint16_t> &v);

bool write_y4m_header(
    std::ostream &os,
    uint32_t width,
    uint32_t height,
    int fps_num,
    int fps_den);

bool write_y4m_frame(
    std::ostream &os,
    const std::vector<uint16_t> &y,
    const std::vector<uint16_t> &u,
    const std::vector<uint16_t> &v);

} // namespace kmshot