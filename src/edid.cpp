#include "edid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace kmshot
{

namespace
{

constexpr size_t kBlockSize = 128;
constexpr uint8_t kCta861Tag = 0x02;

uint8_t block_checksum(const uint8_t *block)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < kBlockSize; ++i)
        sum = static_cast<uint8_t>(sum + block[i]);
    return sum;
}

std::string decode_pnp_id(uint16_t raw)
{
    std::string out;
    for (int shift : {10, 5, 0})
    {
        const uint8_t c = static_cast<uint8_t>((raw >> shift) & 0x1f);
        if (c == 0)
            return {};
        out.push_back(static_cast<char>('A' + c - 1));
    }
    return out;
}

std::string decode_descriptor_string(const uint8_t *data, size_t len)
{
    std::string out;
    for (size_t i = 0; i < len; ++i)
    {
        const uint8_t c = data[i];
        if (c == 0x00 || c == 0x0a)
            break;
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    return out;
}

// EDID packs each 10-bit chromaticity as (low_byte << 2) | high_2_bits, where
// the two extra bits for red/green live in byte 25 and for blue/white in byte
// 26. Verified against edid-decode for several panels.
Chromaticities decode_chromaticities(const uint8_t *d)
{
    const uint8_t packed_rg = d[25];
    const uint8_t packed_bw = d[26];

    auto combine = [](uint8_t low_byte, uint8_t packed, int shift) -> double
    {
        const unsigned value = (static_cast<unsigned>(low_byte) << 2) |
                               ((packed >> shift) & 0x03u);
        return static_cast<double>(value) / 1024.0;
    };

    Chromaticities c;
    c.red_x = combine(d[27], packed_rg, 6);
    c.red_y = combine(d[28], packed_rg, 4);
    c.green_x = combine(d[29], packed_rg, 2);
    c.green_y = combine(d[30], packed_rg, 0);
    c.blue_x = combine(d[31], packed_bw, 6);
    c.blue_y = combine(d[32], packed_bw, 4);
    c.white_x = combine(d[33], packed_bw, 2);
    c.white_y = combine(d[34], packed_bw, 0);
    return c;
}

// CTA-861.3 encodes luminances with a 50 cd/m^2 base and a 2^(code/32) curve.
double luminance_code_to_nits(uint8_t code)
{
    return 50.0 * std::pow(2.0, static_cast<double>(code) / 32.0);
}

void parse_base_block(const uint8_t *d, EdidInfo &out)
{
    out.version = d[18];
    out.revision = d[19];
    out.extension_count = d[126];

    out.manufacturer = decode_pnp_id(static_cast<uint16_t>((d[8] << 8) | d[9]));
    out.product_code = static_cast<uint16_t>(d[10] | (d[11] << 8));

    const double raw_gamma = static_cast<double>(d[23]);
    if (d[23] != 0xff)
    {
        out.has_gamma = true;
        out.gamma = (raw_gamma + 100.0) / 100.0;
    }

    out.chroma = decode_chromaticities(d);
    out.has_chromaticities = is_valid_chromaticities(out.chroma);
    if (!out.has_chromaticities)
        out.warnings.emplace_back("base block chromaticities are out of range");

    for (int i = 0; i < 4; ++i)
    {
        const uint8_t *x = d + 54 + static_cast<size_t>(i) * 18;
        if (x[0] != 0x00 || x[1] != 0x00 || x[2] != 0x00)
            continue; // detailed timing descriptor

        switch (x[3])
        {
        case 0xfc:
            if (out.product_name.empty())
                out.product_name = decode_descriptor_string(x + 5, 13);
            break;
        case 0xfe:
            if (out.serial_text.empty())
                out.serial_text = decode_descriptor_string(x + 5, 13);
            break;
        default:
            break;
        }
    }
}

void parse_hdr_static_metadata_block(const uint8_t *payload, size_t len, EdidInfo &out)
{
    if (len < 1)
        return;

    auto &hdr = out.hdr;
    if (hdr.present)
        return; // CTA-861 allows at most one of these

    hdr.present = true;
    hdr.eotf_flags = payload[0];
    if (len >= 2)
        hdr.metadata_flags = payload[1];

    if (len >= 3)
    {
        hdr.has_max_luminance = true;
        hdr.max_luminance = luminance_code_to_nits(payload[2]);
    }
    if (len >= 4)
    {
        hdr.has_max_frame_avg_luminance = true;
        hdr.max_frame_avg_luminance = luminance_code_to_nits(payload[3]);
    }
    if (len >= 5 && hdr.has_max_luminance)
    {
        const double normalized = static_cast<double>(payload[4]) / 255.0;
        hdr.has_min_luminance = true;
        hdr.min_luminance = hdr.max_luminance * normalized * normalized / 100.0;
    }
}

void parse_colorimetry_block(const uint8_t *payload, size_t len, EdidInfo &out)
{
    if (len < 1)
        return;

    out.colorimetry_bt2020_rgb = out.colorimetry_bt2020_rgb || (payload[0] & 0x80u) != 0;
    if (len >= 2)
    {
        out.colorimetry_srgb = out.colorimetry_srgb || (payload[1] & 0x20u) != 0;
        out.colorimetry_st2113_rgb = out.colorimetry_st2113_rgb || (payload[1] & 0x80u) != 0;
    }
}

void parse_cta_block(const uint8_t *block, EdidInfo &out)
{
    const uint8_t revision = block[1];
    const uint8_t dtd_offset = block[2];

    if (revision < 3 || dtd_offset < 4 || dtd_offset > kBlockSize)
        return;

    size_t i = 4;
    while (i < dtd_offset)
    {
        const uint8_t header = block[i];
        const uint8_t tag = static_cast<uint8_t>((header >> 5) & 0x07u);
        const size_t len = header & 0x1fu;

        if (i + 1 + len > kBlockSize)
        {
            out.warnings.emplace_back("truncated CTA-861 data block");
            break;
        }

        const uint8_t *payload = block + i + 1;
        if (tag == 0x07 && len >= 1)
        {
            const uint8_t extended_tag = payload[0];
            const uint8_t *ext_payload = payload + 1;
            const size_t ext_len = len - 1;

            if (extended_tag == 0x05)
                parse_colorimetry_block(ext_payload, ext_len, out);
            else if (extended_tag == 0x06)
                parse_hdr_static_metadata_block(ext_payload, ext_len, out);
        }

        i += 1 + len;
    }
}

} // namespace

bool parse_edid(const uint8_t *data, size_t size, EdidInfo &out)
{
    out = EdidInfo{};

    if (!data || size < kBlockSize)
    {
        out.warnings.emplace_back("EDID blob is shorter than 128 bytes");
        return false;
    }

    static const uint8_t kHeader[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    for (size_t i = 0; i < 8; ++i)
    {
        if (data[i] != kHeader[i])
        {
            out.warnings.emplace_back("invalid EDID header");
            return false;
        }
    }

    out.valid = true;
    out.checksum_ok = block_checksum(data) == 0;
    if (!out.checksum_ok)
        out.warnings.emplace_back("base block checksum mismatch");

    parse_base_block(data, out);

    const size_t blocks = size / kBlockSize;
    const size_t expected = std::min<size_t>(blocks, static_cast<size_t>(out.extension_count) + 1);
    for (size_t b = 1; b < expected; ++b)
    {
        const uint8_t *block = data + b * kBlockSize;
        if (block_checksum(block) != 0)
            out.warnings.emplace_back("extension block " + std::to_string(b) + " checksum mismatch");
        if (block[0] == kCta861Tag)
            parse_cta_block(block, out);
    }

    return true;
}

bool read_edid_file(const std::string &path, std::vector<uint8_t> &out, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }

    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
    {
        error = "failed while reading " + path;
        return false;
    }
    if (out.empty())
    {
        error = path + " is empty";
        return false;
    }
    return true;
}

std::string describe_edid(const EdidInfo &info)
{
    std::ostringstream os;
    if (!info.valid)
    {
        os << "EDID: invalid";
        if (!info.warnings.empty())
            os << " (" << info.warnings.front() << ")";
        return os.str();
    }

    os.setf(std::ios::fixed);
    os.precision(4);

    os << "EDID: ";
    if (!info.manufacturer.empty())
        os << info.manufacturer << " ";
    if (!info.product_name.empty())
        os << info.product_name;
    else
        os << "product 0x" << std::hex << info.product_code << std::dec;
    os << " (version 1." << static_cast<unsigned>(info.revision)
       << ", " << info.extension_count << " extension block(s)"
       << (info.checksum_ok ? ", checksum OK" : ", CHECKSUM BAD") << ")\n";

    if (!info.serial_text.empty())
        os << "  serial: " << info.serial_text << "\n";

    if (info.has_chromaticities)
    {
        os << "  chromaticities (CIE 1931 xy):\n"
           << "    R (" << info.chroma.red_x << ", " << info.chroma.red_y << ")\n"
           << "    G (" << info.chroma.green_x << ", " << info.chroma.green_y << ")\n"
           << "    B (" << info.chroma.blue_x << ", " << info.chroma.blue_y << ")\n"
           << "    W (" << info.chroma.white_x << ", " << info.chroma.white_y << ")\n";
    }

    if (info.has_gamma)
        os << "  gamma: " << info.gamma << "\n";

    std::string colorimetry;
    auto add_colorimetry = [&colorimetry](const char *name)
    {
        if (!colorimetry.empty())
            colorimetry += ", ";
        colorimetry += name;
    };
    if (info.colorimetry_bt2020_rgb)
        add_colorimetry("BT2020RGB");
    if (info.colorimetry_st2113_rgb)
        add_colorimetry("ST2113RGB");
    if (info.colorimetry_srgb)
        add_colorimetry("sRGB");
    if (!colorimetry.empty())
        os << "  colorimetry: " << colorimetry << "\n";

    const auto &hdr = info.hdr;
    if (hdr.present)
    {
        std::string eotfs;
        auto add_eotf = [&eotfs](const char *name)
        {
            if (!eotfs.empty())
                eotfs += ", ";
            eotfs += name;
        };
        if (hdr.eotf_flags & 0x01u)
            add_eotf("SDR");
        if (hdr.eotf_flags & 0x02u)
            add_eotf("HDR-gamma");
        if (hdr.eotf_flags & 0x04u)
            add_eotf("ST2084");
        if (hdr.eotf_flags & 0x08u)
            add_eotf("HLG");
        if (eotfs.empty())
            eotfs = "none";

        os << "  HDR static metadata: EOTF [" << eotfs << "]";
        if (hdr.metadata_flags & 0x01u)
            os << ", static metadata type 1";
        if (hdr.has_max_luminance)
            os << ", max " << hdr.max_luminance << " cd/m^2";
        if (hdr.has_max_frame_avg_luminance)
            os << ", maxFALL " << hdr.max_frame_avg_luminance << " cd/m^2";
        if (hdr.has_min_luminance)
            os << ", min " << hdr.min_luminance << " cd/m^2";
        os << "\n";
    }

    for (const auto &warning : info.warnings)
        os << "  warning: " << warning << "\n";

    return os.str();
}

} // namespace kmshot
