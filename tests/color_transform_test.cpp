// Numeric checks for the RGBA -> YUV444P16 conversion pipeline.
#include "color_math.hpp"
#include "color_transform.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace kmshot;

namespace
{

int g_failures = 0;

void expect_yuv_tol(const std::string &label,
                    const ColorTransformConfig &cfg,
                    float r,
                    float g,
                    float b,
                    uint16_t ey,
                    uint16_t eu,
                    uint16_t ev,
                    int tolerance)
{
    const float rgba[4] = {r, g, b, 1.0f};
    std::vector<uint16_t> y, u, v;
    if (!transform_rgba32f_to_yuv444p16(rgba, 1, 1, cfg, y, u, v) || y.size() != 1)
    {
        std::cout << "  FAIL " << label << " (transform failed)\n";
        ++g_failures;
        return;
    }

    const auto near = [tolerance](uint16_t actual, uint16_t expected)
    {
        return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= tolerance;
    };

    if (near(y[0], ey) && near(u[0], eu) && near(v[0], ev))
    {
        std::cout << "  ok   " << label << " -> YUV(" << y[0] << ", " << u[0] << ", " << v[0] << ")\n";
    }
    else
    {
        std::cout << "  FAIL " << label << " -> YUV(" << y[0] << ", " << u[0] << ", " << v[0]
                  << "), expected YUV(" << ey << ", " << eu << ", " << ev << ")\n";
        ++g_failures;
    }
}

void expect_yuv(const std::string &label,
                const ColorTransformConfig &cfg,
                float r,
                float g,
                float b,
                uint16_t ey,
                uint16_t eu,
                uint16_t ev)
{
    expect_yuv_tol(label, cfg, r, g, b, ey, eu, ev, 0);
}

// Checks the target-space RGB output that is handed to libavif, which applies
// the RGB -> YUV matrix and the chroma downsampling itself.
void expect_rgb(const std::string &label,
                const ColorTransformConfig &cfg,
                float r,
                float g,
                float b,
                uint16_t er,
                uint16_t eg,
                uint16_t eb,
                int tolerance = 0)
{
    const float rgba[4] = {r, g, b, 1.0f};
    std::vector<uint16_t> rgb;
    if (!transform_rgba32f_to_rgb10(rgba, 1, 1, cfg, rgb) || rgb.size() != 3)
    {
        std::cout << "  FAIL " << label << " (rgb transform failed)\n";
        ++g_failures;
        return;
    }

    const auto near = [tolerance](uint16_t actual, uint16_t expected)
    {
        return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= tolerance;
    };

    if (near(rgb[0], er) && near(rgb[1], eg) && near(rgb[2], eb))
    {
        std::cout << "  ok   " << label << " -> RGB(" << rgb[0] << ", " << rgb[1] << ", " << rgb[2] << ")\n";
    }
    else
    {
        std::cout << "  FAIL " << label << " -> RGB(" << rgb[0] << ", " << rgb[1] << ", " << rgb[2]
                  << "), expected RGB(" << er << ", " << eg << ", " << eb << ")\n";
        ++g_failures;
    }
}

ColorTransformConfig sdr_identity_config()
{
    ColorTransformConfig cfg;
    cfg.source = SourceEncoding::SdrDisplayNative;
    cfg.display_to_target = Mat3::identity();
    cfg.target_rgb_to_yuv = target_rgb_to_yuv_matrix(false);
    cfg.display_decode_gamma = 2.2;
    return cfg;
}

} // namespace

int main()
{
    const ColorTransformConfig identity = sdr_identity_config();

    std::cout << "SDR pipeline with an identity display matrix:\n";
    expect_yuv("black", identity, 0.0f, 0.0f, 0.0f, 0, 32768, 32768);
    expect_yuv("white", identity, 1.0f, 1.0f, 1.0f, 65535, 32768, 32768);
    expect_yuv("red", identity, 1.0f, 0.0f, 0.0f, 13933, 25259, 65535);
    expect_yuv("green", identity, 0.0f, 1.0f, 0.0f, 46871, 7508, 3005);
    expect_yuv("blue", identity, 0.0f, 0.0f, 1.0f, 4732, 65535, 29763);

    // pow(0.5, 1/2.2) decodes back to linear 0.5, which sRGB-encodes to
    // 0.735357 and stays perfectly neutral.
    const float mid_gray = static_cast<float>(std::pow(0.5, 1.0 / 2.2));
    expect_yuv("neutral mid gray", identity, mid_gray, mid_gray, mid_gray, 48192, 32768, 32768);

    // Out of range values must be clamped by the transfer functions.
    expect_yuv("negative clamps to black", identity, -1.0f, -1.0f, -1.0f, 0, 32768, 32768);
    expect_yuv("over-range clamps to white", identity, 2.0f, 2.0f, 2.0f, 65535, 32768, 32768);

    std::cout << "BT.2020 output matrix:\n";
    ColorTransformConfig bt2020 = identity;
    bt2020.target_rgb_to_yuv = target_rgb_to_yuv_matrix(true);
    expect_yuv("white stays neutral", bt2020, 1.0f, 1.0f, 1.0f, 65535, 32768, 32768);

    std::cout << "HDR (PQ) pipeline:\n";
    ColorTransformConfig hdr;
    hdr.source = SourceEncoding::HdrPqBt2020;
    hdr.target_rgb_to_yuv = target_rgb_to_yuv_matrix(true);
    hdr.pq_input_is_gamma22 = true;
    hdr.pq_scale = 1260.785 / 10000.0;

    {
        // Already-PQ input is passed through untouched.
        ColorTransformConfig passthrough = hdr;
        passthrough.pq_input_is_gamma22 = false;
        const float rgba[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        std::vector<uint16_t> y, u, v;
        transform_rgba32f_to_yuv444p16(rgba, 1, 1, passthrough, y, u, v);
        const uint16_t expected_y = static_cast<uint16_t>(std::llround(0.5 * 65535.0));
        if (y[0] == expected_y && u[0] == 32768 && v[0] == 32768)
        {
            std::cout << "  ok   already-PQ input passes through\n";
        }
        else
        {
            std::cout << "  FAIL already-PQ passthrough (got " << y[0] << ")\n";
            ++g_failures;
        }
    }

    {
        // KDE-style gamma 2.2 HDR input: white must still map to a neutral
        // colour, and the PQ encode of 1260.785/10000 nits is deterministic.
        const float linear = 1.0f * static_cast<float>(1260.785 / 10000.0);
        const uint16_t expected_y = static_cast<uint16_t>(
            std::llround(std::clamp(static_cast<double>(linear_to_pq(linear)), 0.0, 1.0) * 65535.0));

        const float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::vector<uint16_t> y, u, v;
        transform_rgba32f_to_yuv444p16(rgba, 1, 1, hdr, y, u, v);
        if (y[0] == expected_y && u[0] == 32768 && v[0] == 32768)
        {
            std::cout << "  ok   gamma 2.2 white re-encodes to PQ (Y=" << y[0] << ")\n";
        }
        else
        {
            std::cout << "  FAIL gamma 2.2 white re-encode (got Y=" << y[0]
                      << ", expected " << expected_y << ")\n";
            ++g_failures;
        }
    }

    // With the real panel -> target matrix, native white must stay white and
    // the neutral axis must not pick up a colour cast. LittleCMS2 is a hard
    // build dependency, so this always runs.
    {
        std::cout << "Panel native -> Rec.709 (LittleCMS2):\n";
        ColorTransformConfig panel;
        panel.source = SourceEncoding::SdrDisplayNative;
        panel.display_decode_gamma = 2.2;
        panel.target_rgb_to_yuv = target_rgb_to_yuv_matrix(false);

        const Chromaticities chroma{0.6796875, 0.3173828, 0.2421875, 0.7167969,
                                    0.1416016, 0.0527344, 0.3183594, 0.3339844};
        const GamutPreset *srgb = find_gamut_preset("srgb");
        auto matrix = display_to_display_matrix(chroma, srgb->chroma);
        if (!matrix)
        {
            std::cout << "  FAIL could not build the panel -> srgb matrix\n";
            ++g_failures;
        }
        else
        {
            panel.display_to_target = *matrix;
            // The lcms2 matrix carries ~1e-8 error, which can land a neutral
            // chroma on either side of the 32767.5 rounding boundary.
            expect_yuv_tol("panel white -> neutral white", panel, 1.0f, 1.0f, 1.0f, 65535, 32768, 32768, 1);
            expect_yuv_tol("panel black -> neutral black", panel, 0.0f, 0.0f, 0.0f, 0, 32768, 32768, 1);
            expect_rgb("panel white -> neutral RGB", panel, 1.0f, 1.0f, 1.0f, 1023, 1023, 1023, 1);
        }
    }

    // Target-space RGB for encoders (libavif) that do their own RGB -> YUV.
    std::cout << "Target RGB (10-bit) output:\n";
    expect_rgb("black", identity, 0.0f, 0.0f, 0.0f, 0, 0, 0);
    expect_rgb("white", identity, 1.0f, 1.0f, 1.0f, 1023, 1023, 1023);
    expect_rgb("red", identity, 1.0f, 0.0f, 0.0f, 1023, 0, 0);
    expect_rgb("green", identity, 0.0f, 1.0f, 0.0f, 0, 1023, 0);
    expect_rgb("blue", identity, 0.0f, 0.0f, 1.0f, 0, 0, 1023);
    expect_rgb("neutral mid gray", identity, mid_gray, mid_gray, mid_gray, 752, 752, 752);
    expect_rgb("negative clamps to black", identity, -1.0f, -1.0f, -1.0f, 0, 0, 0);
    expect_rgb("over-range clamps to white", identity, 2.0f, 2.0f, 2.0f, 1023, 1023, 1023);

    if (g_failures != 0)
    {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "all colour transform checks passed\n";
    return 0;
}
