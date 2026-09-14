#pragma once

#include <cstdint>
#include <ostream>
#include <vector>

namespace kmshot
{

// YUV4MPEG2 container helpers for full-range 16-bit YUV444 planar frames.
bool write_y4m_header(
    std::ostream &os,
    uint32_t width,
    uint32_t height,
    int fps_num,
    int fps_den);

// `samples` is the number of 16-bit samples to write per plane, which may be
// smaller than the vectors (a pooled buffer can be cropped in place).
bool write_y4m_frame(
    std::ostream &os,
    const std::vector<uint16_t> &y,
    const std::vector<uint16_t> &u,
    const std::vector<uint16_t> &v,
    size_t samples);

} // namespace kmshot
