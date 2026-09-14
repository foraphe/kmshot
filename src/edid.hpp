#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "color_math.hpp"

namespace kmshot
{

// HDR Static Metadata Data Block (CTA-861 extended tag 0x06).
struct EdidHdrStaticMetadata
{
    bool present{false};
    uint8_t eotf_flags{0};     // bit0 gamma SDR, bit1 gamma HDR, bit2 ST2084, bit3 HLG
    uint8_t metadata_flags{0}; // bit0 static metadata type 1
    bool has_max_luminance{false};
    double max_luminance{0.0}; // cd/m^2
    bool has_max_frame_avg_luminance{false};
    double max_frame_avg_luminance{0.0}; // cd/m^2
    bool has_min_luminance{false};
    double min_luminance{0.0}; // cd/m^2
};

struct EdidInfo
{
    bool valid{false};
    bool checksum_ok{false};
    uint8_t version{0};
    uint8_t revision{0};
    std::string manufacturer; // PNP ID, e.g. "BOE"
    uint16_t product_code{0};
    std::string product_name;
    std::string serial_text;

    bool has_chromaticities{false};
    Chromaticities chroma{};

    bool has_gamma{false};
    double gamma{0.0};

    bool colorimetry_bt2020_rgb{false};
    bool colorimetry_st2113_rgb{false};
    bool colorimetry_srgb{false};

    EdidHdrStaticMetadata hdr;

    size_t extension_count{0};
    std::vector<std::string> warnings;
};

// Parse a raw EDID blob (base block plus extensions). `out.valid` is set when
// the base block header and structure are usable; malformed optional parts are
// reported through `out.warnings`.
bool parse_edid(const uint8_t *data, size_t size, EdidInfo &out);

// Read a raw EDID blob from a file (e.g. /usr/lib/firmware/edid/*.bin).
bool read_edid_file(const std::string &path, std::vector<uint8_t> &out, std::string &error);

// Human readable multi-line summary.
std::string describe_edid(const EdidInfo &info);

} // namespace kmshot
