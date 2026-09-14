#pragma once

#include <cstdint>
#include <optional>

namespace kmshot
{

// Human readable dumps of the DRM state, written to stderr. These are purely
// diagnostic and never change the capture behaviour.
void print_display_metadata(int card_fd);
void log_panel_orientation(int card_fd, uint32_t plane_id);
void log_hdr_metadata(int card_fd, uint32_t connector_id);
void log_color_and_range_info(int card_fd,
                              uint32_t plane_id,
                              uint32_t crtc_id,
                              const std::optional<uint32_t> &connector_id);

} // namespace kmshot
