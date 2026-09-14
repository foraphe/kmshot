#include "capture.hpp"

#include "dmabuf_gl.hpp"
#include "drm_util.hpp"
#include "y4m.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace kmshot
{

namespace
{

uint16_t float01_to_u16_sat(float v)
{
    if (!(v >= 0.0f))
        return 0; // includes NaN
    if (v >= 1.0f)
        return 65535;
    return static_cast<uint16_t>(std::llround(static_cast<double>(v) * 65535.0));
}

uint16_t float01_to_u12_msb16_sat(float v)
{
    if (!(v >= 0.0f))
        return 0; // includes NaN
    if (v >= 1.0f)
        return 0xFFF0u; // 12-bit full scale, MSB-aligned in 16-bit
    const uint16_t q12 = static_cast<uint16_t>(
        std::llround(static_cast<double>(v) * 4095.0));
    return static_cast<uint16_t>(q12 << 4);
}

bool parse_i64_strict(const std::string &text, int64_t &out)
{
    try
    {
        size_t consumed = 0;
        const long long v = std::stoll(text, &consumed);
        if (consumed != text.size())
            return false;
        out = static_cast<int64_t>(v);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool parse_u64_strict(const std::string &text, uint64_t &out)
{
    if (text.empty() || text.front() == '-')
        return false;
    try
    {
        size_t consumed = 0;
        const unsigned long long v = std::stoull(text, &consumed);
        if (consumed != text.size())
            return false;
        out = static_cast<uint64_t>(v);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// Final quantization for the raw RGBA64 output path.
void quantize_rgba_output(
    const std::vector<float> &rgba32f,
    bool use_linear12_path,
    double decode_gamma,
    std::vector<uint16_t> &rgba16)
{
    rgba16.resize(rgba32f.size());
    const size_t pixels = rgba32f.size() / 4u;

    for (size_t p = 0; p < pixels; ++p)
    {
        const float *s = rgba32f.data() + p * 4u;
        uint16_t *d = rgba16.data() + p * 4u;

        if (use_linear12_path)
        {
            // gamma-encoded blending-space RGB -> linear, quantized to 12-bit
            // and stored MSB-aligned in 16-bit lanes.
            const float r_lin = static_cast<float>(
                std::pow(std::clamp(static_cast<double>(s[0]), 0.0, 1.0), decode_gamma));
            const float g_lin = static_cast<float>(
                std::pow(std::clamp(static_cast<double>(s[1]), 0.0, 1.0), decode_gamma));
            const float b_lin = static_cast<float>(
                std::pow(std::clamp(static_cast<double>(s[2]), 0.0, 1.0), decode_gamma));

            d[0] = float01_to_u12_msb16_sat(r_lin);
            d[1] = float01_to_u12_msb16_sat(g_lin);
            d[2] = float01_to_u12_msb16_sat(b_lin);
            d[3] = 0xFFF0u;
        }
        else
        {
            d[0] = float01_to_u16_sat(s[0]);
            d[1] = float01_to_u16_sat(s[1]);
            d[2] = float01_to_u16_sat(s[2]);
            d[3] = 65535u;
        }
    }
}

} // namespace

std::optional<CropRect> compute_crop_rect_for_buffer(
    const SlurpRegion &slurp,
    int32_t capture_global_x,
    int32_t capture_global_y,
    uint32_t capture_global_w,
    uint32_t capture_global_h,
    uint32_t buf_w,
    uint32_t buf_h,
    double slurp_scale_x,
    double slurp_scale_y)
{
    if (capture_global_w == 0 || capture_global_h == 0 || buf_w == 0 || buf_h == 0)
        return std::nullopt;
    if (slurp.w == 0 || slurp.h == 0)
        return std::nullopt;
    if (!(slurp_scale_x > 0.0) || !(slurp_scale_y > 0.0))
        return std::nullopt;

    // slurp logical -> physical/global (64-bit to avoid int32 overflow)
    const double slurp_x0 = static_cast<double>(slurp.x) * slurp_scale_x;
    const double slurp_y0 = static_cast<double>(slurp.y) * slurp_scale_y;
    const double slurp_x1 = static_cast<double>(
        static_cast<int64_t>(slurp.x) + static_cast<int64_t>(slurp.w)) * slurp_scale_x;
    const double slurp_y1 = static_cast<double>(
        static_cast<int64_t>(slurp.y) + static_cast<int64_t>(slurp.h)) * slurp_scale_y;

    if (!std::isfinite(slurp_x0) || !std::isfinite(slurp_y0) ||
        !std::isfinite(slurp_x1) || !std::isfinite(slurp_y1))
        return std::nullopt;

    const double sx = static_cast<double>(buf_w) / static_cast<double>(capture_global_w);
    const double sy = static_cast<double>(buf_h) / static_cast<double>(capture_global_h);

    const double lx0 = (slurp_x0 - static_cast<double>(capture_global_x)) * sx;
    const double ly0 = (slurp_y0 - static_cast<double>(capture_global_y)) * sy;
    const double lx1 = (slurp_x1 - static_cast<double>(capture_global_x)) * sx;
    const double ly1 = (slurp_y1 - static_cast<double>(capture_global_y)) * sy;

    const int64_t x0i = static_cast<int64_t>(std::floor(lx0));
    const int64_t y0i = static_cast<int64_t>(std::floor(ly0));
    const int64_t x1i = static_cast<int64_t>(std::ceil(lx1));
    const int64_t y1i = static_cast<int64_t>(std::ceil(ly1));

    const int64_t x0 = std::max<int64_t>(0, x0i);
    const int64_t y0 = std::max<int64_t>(0, y0i);
    const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(buf_w), x1i);
    const int64_t y1 = std::min<int64_t>(static_cast<int64_t>(buf_h), y1i);

    if (x1 <= x0 || y1 <= y0)
        return std::nullopt;

    return CropRect{
        static_cast<uint32_t>(x0),
        static_cast<uint32_t>(y0),
        static_cast<uint32_t>(x1 - x0),
        static_cast<uint32_t>(y1 - y0)};
}

bool read_region_info(SlurpRegion &region)
{
    std::string line;
    if (!std::getline(std::cin, line))
    {
        std::cerr << "Failed to read region info from stdin\n";
        return false;
    }

    const size_t space = line.find(' ');
    if (space == std::string::npos)
    {
        std::cerr << "Invalid region info format (expected \"x,y wxh\")\n";
        return false;
    }

    const std::string pos = line.substr(0, space);
    const std::string size = line.substr(space + 1);

    const size_t comma = pos.find(',');
    if (comma == std::string::npos)
    {
        std::cerr << "Invalid position format\n";
        return false;
    }

    const size_t x_pos = size.find('x');
    if (x_pos == std::string::npos)
    {
        std::cerr << "Invalid size format\n";
        return false;
    }

    int64_t x = 0;
    int64_t y = 0;
    uint64_t w = 0;
    uint64_t h = 0;
    if (!parse_i64_strict(pos.substr(0, comma), x) ||
        !parse_i64_strict(pos.substr(comma + 1), y) ||
        !parse_u64_strict(size.substr(0, x_pos), w) ||
        !parse_u64_strict(size.substr(x_pos + 1), h))
    {
        std::cerr << "Error parsing region info (expected \"x,y wxh\")\n";
        return false;
    }

    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX ||
        w == 0 || w > UINT32_MAX || h == 0 || h > UINT32_MAX)
    {
        std::cerr << "Region info is out of range\n";
        return false;
    }

    region.x = static_cast<int32_t>(x);
    region.y = static_cast<int32_t>(y);
    region.w = static_cast<uint32_t>(w);
    region.h = static_cast<uint32_t>(h);
    return true;
}

int run_capture(const Options &opts,
                int card_fd,
                const ColorTransformConfig &color,
                int colorspace_idx,
                const SlurpRegion &slurp_region,
                const PlaneSelection &initial_plane)
{
    auto reader = std::make_unique<DmabufGlReader>();
    if (!reader->init(card_fd))
        return 1;

    std::ofstream out;
    if (!opts.write_to_stdout)
    {
        out.open(opts.out_path, std::ios::binary);
        if (!out)
        {
            std::cerr << "Cannot open output file: " << opts.out_path << "\n";
            return 1;
        }
    }

    const auto frame_delay = std::chrono::milliseconds(1000 / std::max(1, opts.fps));
    std::vector<float> rgba32f;
    std::vector<uint16_t> rgba16;
    std::vector<uint16_t> y10, u10, v10;
    bool y4m_header_written = false;

    uint32_t out_w = 0, out_h = 0;
    uint32_t last_written_w = 0, last_written_h = 0;
    uint64_t frames_written = 0;
    bool crop_warned = false;
    bool warned_non_sdr = false;

    SlurpRegion region = slurp_region;
    std::optional<PlaneSelection> plane = initial_plane;

    for (int i = 0; i < opts.frames; ++i)
    {
        PlanePtr current(drmModeGetPlane(card_fd, plane->plane_id), drmModeFreePlane);
        if (!current || current->fb_id == 0)
        {
            plane = find_capture_plane(card_fd, opts.monitor);
            if (!plane)
                break;
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        auto fb = FramebufferInfo::load(card_fd, current->fb_id);
        if (!fb || fb->handles[0] == 0)
        {
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        if (!is_single_plane_rgb_fourcc(fb->fourcc))
        {
            std::cerr << "Unsupported framebuffer format: " << fourcc_to_string(fb->fourcc) << "\n";
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        const auto geom = get_plane_geometry(card_fd, plane->plane_id, fb->width, fb->height);

        const uint32_t cap_w = geom.crtc_w;
        const uint32_t cap_h = geom.crtc_h;

        const float uv_off_x = std::clamp(geom.src_x / std::max(1.0f, static_cast<float>(fb->width)), 0.0f, 1.0f);
        const float uv_off_y = std::clamp(geom.src_y / std::max(1.0f, static_cast<float>(fb->height)), 0.0f, 1.0f);
        const float uv_scale_x = std::clamp(geom.src_w / std::max(1.0f, static_cast<float>(fb->width)), 0.0f, 1.0f);
        const float uv_scale_y = std::clamp(geom.src_h / std::max(1.0f, static_cast<float>(fb->height)), 0.0f, 1.0f);

        if (out_w == 0)
        {
            out_w = cap_w;
            out_h = cap_h;
        }
        if (cap_w != out_w || cap_h != out_h)
        {
            std::cerr << "Geometry changed from " << out_w << "x" << out_h
                      << " to " << cap_w << "x" << cap_h << " (stop/reinit)\n";
            break;
        }

        if (!opts.use_slurp)
        {
            region.x = 0;
            region.y = 0;
            region.w = out_w;
            region.h = out_h;
        }

        int dmabuf_fd = -1;
        if (drmPrimeHandleToFD(card_fd, fb->handles[0], DRM_CLOEXEC, &dmabuf_fd) != 0)
        {
            std::this_thread::sleep_for(frame_delay);
            continue;
        }
        ScopedFd dma(dmabuf_fd);

        if (!reader->read_dmabuf_to_rgba32f(
                dma.fd,
                fb->width, fb->height,
                cap_w, cap_h,
                uv_off_x, uv_off_y,
                uv_scale_x, uv_scale_y,
                fb->fourcc,
                fb->pitches[0],
                fb->offsets[0],
                fb->modifier,
                opts.dmabuf_sync,
                rgba32f))
        {
            if (reader->context_lost())
            {
                reader = std::make_unique<DmabufGlReader>();
                if (!reader->init(card_fd))
                    return 1;
            }
            std::this_thread::sleep_for(frame_delay);
            continue;
        }

        uint32_t frame_w = out_w;
        uint32_t frame_h = out_h;

        if (opts.use_slurp)
        {
            CrtcPtr crtc(drmModeGetCrtc(card_fd, plane->crtc_id), drmModeFreeCrtc);
            if (!crtc)
            {
                std::this_thread::sleep_for(frame_delay);
                continue;
            }

            const int32_t capture_global_x = crtc->x + geom.crtc_x;
            const int32_t capture_global_y = crtc->y + geom.crtc_y;

            auto crop = compute_crop_rect_for_buffer(
                region,
                capture_global_x,
                capture_global_y,
                geom.crtc_w,
                geom.crtc_h,
                out_w,
                out_h,
                opts.slurp_scale_x,
                opts.slurp_scale_y);

            if (!crop)
            {
                if (!crop_warned)
                {
                    crop_warned = true;
                    std::cerr << "Slurp region is outside selected monitor/capture area; skipping frames\n";
                }
                std::this_thread::sleep_for(frame_delay);
                continue;
            }

            std::vector<float> cropped;
            cropped.reserve(static_cast<size_t>(crop->w) * crop->h * 4u);

            for (uint32_t y = 0; y < crop->h; ++y)
            {
                const float *src = rgba32f.data() +
                                   (((crop->y + y) * out_w + crop->x) * static_cast<size_t>(4));
                cropped.insert(cropped.end(), src, src + static_cast<size_t>(crop->w) * 4u);
            }
            rgba32f = std::move(cropped);

            frame_w = crop->w;
            frame_h = crop->h;
        }

        if (opts.pp_y4m)
        {
            if (!transform_rgba32f_to_yuv444p10(
                    rgba32f.data(), frame_w, frame_h, color, y10, u10, v10))
            {
                std::this_thread::sleep_for(frame_delay);
                continue;
            }

            std::ostream &os = opts.write_to_stdout ? static_cast<std::ostream &>(std::cout)
                                                    : static_cast<std::ostream &>(out);

            if (!y4m_header_written)
            {
                if (!write_y4m_header(os, frame_w, frame_h, std::max(1, opts.fps), 1))
                    return 1;
                y4m_header_written = true;
            }

            if (!write_y4m_frame(os, y10, u10, v10))
                return 1;
        }
        else
        {
            const bool use_linear12_path = opts.sdr_linear_12bpc && (colorspace_idx == 0);
            if (opts.sdr_linear_12bpc && colorspace_idx != 0 && !warned_non_sdr)
            {
                warned_non_sdr = true;
                std::cerr << "--sdr-linear-12bpc requested, but connector Colorspace="
                          << colorspace_idx << " (not SDR enum 0). Falling back to RGBA64 path.\n";
            }

            quantize_rgba_output(rgba32f, use_linear12_path, color.display_decode_gamma, rgba16);

            std::ostream &os = opts.write_to_stdout ? static_cast<std::ostream &>(std::cout)
                                                    : static_cast<std::ostream &>(out);
            os.write(
                reinterpret_cast<const char *>(rgba16.data()),
                static_cast<std::streamsize>(rgba16.size() * sizeof(uint16_t)));
            if (!os)
                return 1;
        }

        last_written_w = frame_w;
        last_written_h = frame_h;
        ++frames_written;

        std::this_thread::sleep_for(frame_delay);
    }

    const uint32_t final_w = last_written_w ? last_written_w : out_w;
    const uint32_t final_h = last_written_h ? last_written_h : out_h;

    if (frames_written == 0)
    {
        std::cerr << "Capture produced no frames (no usable plane/framebuffer)\n";
        return 1;
    }

    if (!opts.pp_y4m)
    {
        std::cerr << "ffplay -f rawvideo -pixel_format rgba64le -video_size "
                  << final_w << "x" << final_h << " " << opts.out_path << "\n";
    }

    return 0;
}

} // namespace kmshot
