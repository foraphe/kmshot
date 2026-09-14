#include "y4m.hpp"

#include <string>

namespace kmshot
{

bool write_y4m_header(
    std::ostream &os,
    uint32_t width,
    uint32_t height,
    int fps_num,
    int fps_den)
{
    if (width == 0 || height == 0 || fps_num <= 0 || fps_den <= 0)
        return false;

    const std::string hdr =
        "YUV4MPEG2 W" + std::to_string(width) +
        " H" + std::to_string(height) +
        " F" + std::to_string(fps_num) + ":" + std::to_string(fps_den) +
        " Ip C444p10 XYSCSS=444P10 XCOLORRANGE=FULL\n";
    os.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    return static_cast<bool>(os);
}

bool write_y4m_frame(
    std::ostream &os,
    const std::vector<uint16_t> &y,
    const std::vector<uint16_t> &u,
    const std::vector<uint16_t> &v)
{
    if (y.size() != u.size() || y.size() != v.size())
        return false;

    static constexpr char kFrame[] = "FRAME\n";
    os.write(kFrame, sizeof(kFrame) - 1);
    os.write(reinterpret_cast<const char *>(y.data()), static_cast<std::streamsize>(y.size() * sizeof(uint16_t)));
    os.write(reinterpret_cast<const char *>(u.data()), static_cast<std::streamsize>(u.size() * sizeof(uint16_t)));
    os.write(reinterpret_cast<const char *>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(uint16_t)));
    return static_cast<bool>(os);
}

} // namespace kmshot
