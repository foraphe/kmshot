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

bool write_y4m_frame(
    std::ostream &os,
    const std::vector<uint16_t> &y,
    const std::vector<uint16_t> &u,
    const std::vector<uint16_t> &v);

} // namespace kmshot
