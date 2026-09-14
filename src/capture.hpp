#pragma once

#include <cstdint>
#include <optional>

#include "cli.hpp"
#include "color_transform.hpp"
#include "drm_util.hpp"

namespace kmshot
{

struct SlurpRegion
{
    int32_t x{0};
    int32_t y{0};
    uint32_t w{0};
    uint32_t h{0};
};

struct CropRect
{
    uint32_t x{0}; // buffer-space x
    uint32_t y{0}; // buffer-space y (top-origin row index)
    uint32_t w{0};
    uint32_t h{0};
};

// Map a slurp global top-left rect to a local capture rect in buffer space.
// Handles the plane offset and scaling (CRTC_X/Y/W/H) without flipping.
std::optional<CropRect> compute_crop_rect_for_buffer(
    const SlurpRegion &slurp,
    int32_t capture_global_x,
    int32_t capture_global_y,
    uint32_t capture_global_w,
    uint32_t capture_global_h,
    uint32_t buf_w,
    uint32_t buf_h,
    double slurp_scale_x,
    double slurp_scale_y);

// Read "x,y wxh" (slurp output) from stdin.
bool read_region_info(SlurpRegion &region);

// Capture `opts.frames` frames from the plane currently selected on the card
// and write them to opts.out_path / stdout. Returns a process exit code.
int run_capture(const Options &opts,
                int card_fd,
                const ColorTransformConfig &color,
                int colorspace_idx,
                const SlurpRegion &region,
                const PlaneSelection &initial_plane);

} // namespace kmshot
