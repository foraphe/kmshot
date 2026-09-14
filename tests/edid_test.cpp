// Verifies the EDID decoder against the BOE NE160QDM-NM7 panel. The expected
// values below were cross-checked with edid-decode.
#include "edid.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace kmshot;

namespace
{

int g_failures = 0;

void check(bool condition, const std::string &what)
{
    if (condition)
    {
        std::cout << "  ok   " << what << "\n";
    }
    else
    {
        std::cout << "  FAIL " << what << "\n";
        ++g_failures;
    }
}

void check_near(double actual, double expected, double tolerance, const std::string &what)
{
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (!ok)
    {
        std::cout << "  FAIL " << what << " (got " << actual << ", expected " << expected
                  << " +/- " << tolerance << ")\n";
        ++g_failures;
    }
    else
    {
        std::cout << "  ok   " << what << "\n";
    }
}

// EDID stores chromaticities as 10-bit values. edid-decode only prints four
// decimal places (truncated), so compare the raw codes instead: the panel's
// codes were confirmed with single-bit probes against edid-decode.
void check_raw_10bit(double value, int expected, const std::string &what)
{
    const long actual = std::lround(value * 1024.0);
    if (actual == expected)
    {
        std::cout << "  ok   " << what << " (raw " << actual << ")\n";
    }
    else
    {
        std::cout << "  FAIL " << what << " (got raw " << actual << ", expected " << expected << ")\n";
        ++g_failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    const std::string path = argc > 1 ? argv[1] : "tests/data/ne160qdm-nm7.edid";

    std::vector<uint8_t> blob;
    std::string error;
    if (!read_edid_file(path, blob, error))
    {
        std::cerr << "cannot read fixture: " << error << "\n";
        return 2;
    }

    EdidInfo info;
    if (!parse_edid(blob.data(), blob.size(), info))
    {
        std::cerr << "parse_edid rejected the fixture\n";
        return 1;
    }

    std::cout << describe_edid(info);

    check(info.valid, "EDID is valid");
    check(info.checksum_ok, "checksums are valid");
    check(info.manufacturer == "BOE", "manufacturer is BOE");
    check(info.product_code == 0x0c24, "product code is 0x0c24");
    check(info.product_name == "NE160QDM-NM7", "product name");
    check(info.serial_text == "BOE CQ", "serial text");
    check(info.extension_count == 2, "two extension blocks");
    check(info.has_gamma, "gamma present");
    check_near(info.gamma, 2.20, 1e-9, "gamma is 2.20");

    check(info.has_chromaticities, "chromaticities present");
    // edid-decode prints these as R (0.6796, 0.3173) G (0.2421, 0.7167)
    // B (0.1416, 0.0527) W (0.3183, 0.3339); compare the underlying codes.
    check_raw_10bit(info.chroma.red_x, 696, "red x");
    check_raw_10bit(info.chroma.red_y, 325, "red y");
    check_raw_10bit(info.chroma.green_x, 248, "green x");
    check_raw_10bit(info.chroma.green_y, 734, "green y");
    check_raw_10bit(info.chroma.blue_x, 145, "blue x");
    check_raw_10bit(info.chroma.blue_y, 54, "blue y");
    check_raw_10bit(info.chroma.white_x, 326, "white x");
    check_raw_10bit(info.chroma.white_y, 342, "white y");

    check(info.colorimetry_bt2020_rgb, "colorimetry BT2020RGB");
    check(info.colorimetry_st2113_rgb, "colorimetry ST2113RGB");

    check(info.hdr.present, "HDR static metadata present");
    check((info.hdr.eotf_flags & 0x01u) != 0, "EOTF: traditional gamma SDR");
    check((info.hdr.eotf_flags & 0x04u) != 0, "EOTF: SMPTE ST2084");
    check((info.hdr.metadata_flags & 0x01u) != 0, "static metadata type 1");
    check(info.hdr.has_max_luminance, "max luminance present");
    check_near(info.hdr.max_luminance, 1260.785, 0.01, "max luminance in cd/m^2");
    check(info.hdr.has_max_frame_avg_luminance, "maxFALL present");
    check_near(info.hdr.max_frame_avg_luminance, 603.666, 0.01, "maxFALL in cd/m^2");
    check(info.hdr.has_min_luminance, "min luminance present");
    check_near(info.hdr.min_luminance, 0.04964, 1e-3, "min luminance in cd/m^2");

    // Malformed input must be rejected rather than trusted.
    EdidInfo bad;
    std::vector<uint8_t> short_blob(64, 0);
    check(!parse_edid(short_blob.data(), short_blob.size(), bad), "short blob rejected");

    std::vector<uint8_t> garbage(128, 0x5a);
    check(!parse_edid(garbage.data(), garbage.size(), bad), "garbage blob rejected");

    std::vector<uint8_t> broken = blob;
    broken[127] ^= 0xff; // deliberately break the base block checksum
    EdidInfo unchecked;
    check(parse_edid(broken.data(), broken.size(), unchecked), "broken-checksum blob still parses");
    check(!unchecked.checksum_ok, "broken checksum is reported");

    if (g_failures != 0)
    {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "all EDID checks passed\n";
    return 0;
}
