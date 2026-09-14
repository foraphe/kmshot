#include "capture.hpp"

#include "dmabuf_gl.hpp"
#include "drm_util.hpp"
#include "encoder.hpp"
#include "y4m.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
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

// Crops an interleaved 16-bit buffer (1 channel for a YUV plane, 3 for packed
// RGB) the same way the RGBA path crops its float buffer.
std::vector<uint16_t> crop_interleaved_16(
    const std::vector<uint16_t> &src, uint32_t src_width, uint32_t channels, const CropRect &crop)
{
    std::vector<uint16_t> out;
    out.reserve(static_cast<size_t>(crop.w) * crop.h * channels);

    for (uint32_t row = 0; row < crop.h; ++row)
    {
        const uint16_t *begin =
            src.data() + (static_cast<size_t>(crop.y + row) * src_width + crop.x) * channels;
        out.insert(out.end(), begin, begin + static_cast<size_t>(crop.w) * channels);
    }

    return out;
}

// Compacts the rows of an interleaved 16-bit buffer in place, keeping the
// vector's size and capacity. Safe because every destination row is at or above
// its source row. Used for pooled buffers that are cropped every frame.
void crop_interleaved_16_in_place(std::vector<uint16_t> &plane,
                                  uint32_t src_width,
                                  uint32_t channels,
                                  const CropRect &crop)
{
    const size_t row_elems = static_cast<size_t>(crop.w) * channels;

    for (uint32_t row = 0; row < crop.h; ++row)
    {
        uint16_t *dst = plane.data() + static_cast<size_t>(row) * row_elems;
        const uint16_t *src =
            plane.data() + (static_cast<size_t>(crop.y + row) * src_width + crop.x) * channels;

        if (dst != src)
            std::memmove(dst, src, row_elems * sizeof(uint16_t));
    }
}

// Writes Y4M frames from a separate thread so that a slow consumer (an encoder
// reading the pipe) cannot stall the capture loop. The bounded queue keeps the
// additional latency and memory in check.
class AsyncY4mWriter
{
public:
    // Buffers handed out by acquire() and filled in place by the capture loop.
    struct Planes
    {
        std::vector<uint16_t> y;
        std::vector<uint16_t> u;
        std::vector<uint16_t> v;
    };

    AsyncY4mWriter(std::ostream &os, int fps, size_t slots)
        : os_(os), fps_(std::max(1, fps))
    {
        pool_.reserve(slots);
        for (size_t i = 0; i < slots; ++i)
        {
            pool_.push_back(std::make_unique<Planes>());
            free_.push_back(pool_.back().get());
        }

        thread_ = std::thread(&AsyncY4mWriter::run, this);
    }

    ~AsyncY4mWriter()
    {
        // Harmless if close() already ran; prevents a joinable thread from
        // terminating the process.
        close();
    }

    AsyncY4mWriter(const AsyncY4mWriter &) = delete;
    AsyncY4mWriter &operator=(const AsyncY4mWriter &) = delete;

    // Blocks until a buffer set is free. Returns nullptr if the writer failed.
    // Reusing the buffers avoids allocating and zero-filling ~25 MB per frame,
    // which the profile showed was a quarter of the capture CPU time.
    Planes *acquire()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return !free_.empty() || failed_; });
        if (failed_ || free_.empty())
            return nullptr;

        Planes *planes = free_.front();
        free_.pop_front();
        return planes;
    }

    // Queues `planes` for writing. `samples` is the number of 16-bit samples
    // per plane (a buffer may have been cropped in place).
    void submit(Planes *planes, uint32_t width, uint32_t height, size_t samples)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(Job{planes, width, height, samples});
        }
        not_empty_.notify_one();
    }

    // Returns a buffer that was acquired but not submitted.
    void release(Planes *planes)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free_.push_back(planes);
        }
        not_full_.notify_one();
    }

    // Drains the queue and joins the writer thread.
    bool close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();

        if (thread_.joinable())
            thread_.join();

        return !failed_;
    }

private:
    struct Job
    {
        Planes *planes{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        size_t samples{0};
    };

    void run()
    {
        bool header_written = false;

        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
                if (queue_.empty())
                    break; // closed and fully drained

                job = queue_.front();
                queue_.pop_front();
            }

            bool ok = true;
            if (!header_written)
            {
                ok = write_y4m_header(os_, job.width, job.height, fps_, 1);
                header_written = ok;
            }
            if (ok)
                ok = write_y4m_frame(os_, job.planes->y, job.planes->u, job.planes->v, job.samples);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ok)
                    failed_ = true;
                free_.push_back(job.planes);
            }
            not_full_.notify_one();

            if (!ok)
                break;
        }
    }

    std::ostream &os_;
    int fps_;

    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::vector<std::unique_ptr<Planes>> pool_;
    std::deque<Planes *> free_;
    std::deque<Job> queue_;
    bool closed_{false};
    bool failed_{false};
    std::thread thread_;
};

// Hands a pooled buffer back to the writer if the frame was dropped before it
// could be submitted, so a skipped frame cannot leak a slot.
class PlaneGuard
{
public:
    PlaneGuard(AsyncY4mWriter *writer, AsyncY4mWriter::Planes *planes)
        : writer_(writer), planes_(planes)
    {
    }

    ~PlaneGuard()
    {
        if (writer_ && planes_)
            writer_->release(planes_);
    }

    PlaneGuard(const PlaneGuard &) = delete;
    PlaneGuard &operator=(const PlaneGuard &) = delete;

    AsyncY4mWriter::Planes *get() const { return planes_; }
    bool valid() const { return planes_ != nullptr; }

    void submit(uint32_t width, uint32_t height, size_t samples)
    {
        writer_->submit(planes_, width, height, samples);
        planes_ = nullptr;
    }

private:
    AsyncY4mWriter *writer_{nullptr};
    AsyncY4mWriter::Planes *planes_{nullptr};
};

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

    const bool avif_mode = !opts.avif_out.empty();

    std::ofstream out;
    if (!opts.write_to_stdout && !avif_mode)
    {
        out.open(opts.out_path, std::ios::binary);
        if (!out)
        {
            std::cerr << "Cannot open output file: " << opts.out_path << "\n";
            return 1;
        }
    }

    AvifWriter avif;
    std::string encode_error;

    const auto frame_delay = std::chrono::milliseconds(1000 / std::max(1, opts.fps));
    std::vector<float> rgba32f;
    std::vector<uint16_t> rgba16;
    std::vector<uint16_t> rgb16;

    // Y4M output is written on its own thread so that a slow pipe consumer does
    // not throttle the capture. The buffers are pooled: reusing them removes the
    // per-frame allocation and zero-fill that dominated the capture CPU.
    std::unique_ptr<AsyncY4mWriter> y4m_writer;
    if (opts.pp_y4m)
    {
        std::ostream &os = opts.write_to_stdout ? static_cast<std::ostream &>(std::cout)
                                                : static_cast<std::ostream &>(out);
        y4m_writer = std::make_unique<AsyncY4mWriter>(os, opts.fps, 3);
    }

    uint32_t out_w = 0, out_h = 0;
    uint32_t last_written_w = 0, last_written_h = 0;
    uint64_t frames_written = 0;
    bool crop_warned = false;
    bool warned_non_sdr = false;

    // The GPU colour pipeline covers Y4M (YUV output) and AVIF (target RGB
    // output). The raw path needs the untouched RGB(A) frame and stays on the
    // CPU. It degrades to the CPU transform at any point.
    const bool gpu_color_requested = (opts.pp_y4m || avif_mode) && !opts.force_cpu_color;
    bool gpu_color = gpu_color_requested && reader->supports_gpu_color();
    if (gpu_color)
        std::cerr << "Colour transform: GPU (fragment shader)\n";
    else if (gpu_color_requested)
        std::cerr << "Colour transform: CPU (GPU pipeline unavailable, or --cpu-color)\n";
    else if (opts.force_cpu_color)
        std::cerr << "Colour transform: CPU (--cpu-color)\n";
    else
        std::cerr << "Colour transform: CPU (this output needs the raw RGB(A) frame)\n";

    SlurpRegion region = slurp_region;
    std::optional<PlaneSelection> plane = initial_plane;

    // Absolute-deadline pacing. Sleeping a full frame interval *after* the work
    // (as this used to) makes the effective rate 1/(work + interval) and adds the
    // whole interval to the capture latency. Waiting until the next slot instead
    // keeps the requested rate as long as one frame fits in the budget, and
    // catches up immediately when it does not.
    auto next_frame_time = std::chrono::steady_clock::now();

    for (int i = 0; i < opts.frames; ++i)
    {
        if (i > 0)
        {
            next_frame_time += frame_delay;
            std::this_thread::sleep_until(next_frame_time);
        }

        // Take a pooled output buffer for this frame. Blocks only when the
        // writer is behind; a dropped frame returns it through PlaneGuard.
        PlaneGuard y4m_buffers(y4m_writer.get(),
                               y4m_writer ? y4m_writer->acquire() : nullptr);
        if (y4m_writer && !y4m_buffers.valid())
        {
            std::cerr << "error: failed to write the Y4M output\n";
            return 1;
        }
        AsyncY4mWriter::Planes *const planes = y4m_buffers.get();

        PlanePtr current(drmModeGetPlane(card_fd, plane->plane_id), drmModeFreePlane);
        if (!current || current->fb_id == 0)
        {
            plane = find_capture_plane(card_fd, opts.monitor);
            if (!plane)
                break;
            continue;
        }

        auto fb = FramebufferInfo::load(card_fd, current->fb_id);
        if (!fb || fb->handles[0] == 0)
        {
            continue;
        }

        if (!is_single_plane_rgb_fourcc(fb->fourcc))
        {
            std::cerr << "Unsupported framebuffer format: " << fourcc_to_string(fb->fourcc) << "\n";
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
            continue;
        }
        ScopedFd dma(dmabuf_fd);

        // Prefer the GPU colour pipeline: it produces the finished YUV or RGB
        // samples directly, so the pixels never pass through the CPU.
        bool have_yuv = false;
        bool have_rgb16 = false;
        if (gpu_color)
        {
            if (avif_mode)
            {
                have_rgb16 = reader->read_dmabuf_to_rgb16(
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
                    color,
                    rgb16);
            }
            else
            {
                have_yuv = reader->read_dmabuf_to_yuv444p16(
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
                    color,
                    planes->y, planes->u, planes->v);
            }

            if (!have_yuv && !have_rgb16)
            {
                gpu_color = false;
                std::cerr << "GPU colour pipeline failed; falling back to the CPU colour transform\n";
            }
        }

        if (!have_yuv && !have_rgb16 && !reader->read_dmabuf_to_rgba32f(
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
                gpu_color = gpu_color_requested && reader->supports_gpu_color();
            }
            continue;
        }

        uint32_t frame_w = out_w;
        uint32_t frame_h = out_h;

        if (opts.use_slurp)
        {
            CrtcPtr crtc(drmModeGetCrtc(card_fd, plane->crtc_id), drmModeFreeCrtc);
            if (!crtc)
            {
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
                continue;
            }

            if (have_yuv)
            {
                crop_interleaved_16_in_place(planes->y, out_w, 1, *crop);
                crop_interleaved_16_in_place(planes->u, out_w, 1, *crop);
                crop_interleaved_16_in_place(planes->v, out_w, 1, *crop);
            }
            else if (have_rgb16)
            {
                rgb16 = crop_interleaved_16(rgb16, out_w, 3, *crop);
            }
            else
            {
                std::vector<float> cropped;
                cropped.reserve(static_cast<size_t>(crop->w) * crop->h * 4u);

                for (uint32_t y = 0; y < crop->h; ++y)
                {
                    const float *src = rgba32f.data() +
                                       (((crop->y + y) * out_w + crop->x) * static_cast<size_t>(4));
                    cropped.insert(cropped.end(), src, src + static_cast<size_t>(crop->w) * 4u);
                }
                rgba32f = std::move(cropped);
            }

            frame_w = crop->w;
            frame_h = crop->h;
        }

        if (avif_mode)
        {
            if (!avif.is_open())
            {
                if (!avif.open(opts.avif_out, frame_w, frame_h, opts.avif, color, encode_error))
                {
                    std::cerr << "error: " << encode_error << "\n";
                    return 1;
                }
                std::cerr << "Encoding AVIF: " << frame_w << "x" << frame_h
                          << " depth=10 yuv=" << opts.avif.subsampling << "\n";
            }

            // The GPU path already produced the target RGB for this frame;
            // libavif still does the RGB -> YUV conversion and the subsampling.
            if (have_rgb16)
            {
                if (!avif.add_frame_rgb16(std::move(rgb16), encode_error))
                {
                    std::cerr << "error: " << encode_error << "\n";
                    return 1;
                }
            }
            else if (!avif.add_frame(rgba32f.data(), color, encode_error))
            {
                std::cerr << "error: " << encode_error << "\n";
                return 1;
            }
        }
        else if (opts.pp_y4m)
        {
            // The GPU path already produced the YUV planes for this frame.
            if (!have_yuv && !transform_rgba32f_to_yuv444p16(
                    rgba32f.data(), frame_w, frame_h, color, planes->y, planes->u, planes->v))
            {
                continue;
            }

            y4m_buffers.submit(frame_w, frame_h, static_cast<size_t>(frame_w) * frame_h);
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

    }

    const uint32_t final_w = last_written_w ? last_written_w : out_w;
    const uint32_t final_h = last_written_h ? last_written_h : out_h;

    if (frames_written == 0)
    {
        std::cerr << "Capture produced no frames (no usable plane/framebuffer)\n";
        return 1;
    }

    // Flush whatever the writer thread still has queued.
    if (y4m_writer && !y4m_writer->close())
    {
        std::cerr << "error: failed while writing the Y4M output\n";
        return 1;
    }

    if (avif_mode)
    {
        if (!avif.finish(encode_error))
        {
            std::cerr << "error: " << encode_error << "\n";
            return 1;
        }
        std::cerr << "Wrote AVIF: " << opts.avif_out << "\n";
        return 0;
    }

    if (!opts.pp_y4m)
    {
        std::cerr << "ffplay -f rawvideo -pixel_format rgba64le -video_size "
                  << final_w << "x" << final_h << " " << opts.out_path << "\n";
    }

    return 0;
}

} // namespace kmshot
