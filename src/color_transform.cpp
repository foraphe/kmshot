#include "color_transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace kmshot
{

namespace
{

inline double clamp01d(double v)
{
    return std::min(1.0, std::max(0.0, v));
}

inline uint16_t quantize_u16(double v01, double max_value)
{
    const double q = clamp01d(v01) * max_value;
    return static_cast<uint16_t>(std::llround(q));
}

inline uint16_t q16(double v01)
{
    return quantize_u16(v01, 65535.0);
}

inline double gamma_to_linear(double v, double gamma)
{
    return std::pow(clamp01d(v), gamma);
}

// Linear -> sRGB signal encoding, used for the SDR target encode.
inline double srgb_oetf(double lin)
{
    const double x = std::max(0.0, lin);
    if (x <= 0.0031308)
        return 12.92 * x;
    return 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}

// Decodes one source pixel (4 interleaved floats) into target-space RGB encoded
// with the target transfer function. No RGB -> YUV matrix is applied, so this is
// shared by the YUV and the RGB output paths.
inline void decode_to_target_rgb(const float *pixel,
                                 const ColorTransformConfig &config,
                                 double &r,
                                 double &g,
                                 double &b)
{
    // Values are deliberately not clamped here: compositors that blend in
    // scRGB can hand out values outside [0, 1], and clamping is left to the
    // per-path transfer functions.
    r = static_cast<double>(pixel[0]);
    g = static_cast<double>(pixel[1]);
    b = static_cast<double>(pixel[2]);

    if (config.source == SourceEncoding::HdrPqBt2020)
    {
        if (config.pq_input_is_gamma22)
        {
            r = static_cast<double>(linear_to_pq(static_cast<float>(
                gamma_to_linear(r, 2.2) * config.pq_scale)));
            g = static_cast<double>(linear_to_pq(static_cast<float>(
                gamma_to_linear(g, 2.2) * config.pq_scale)));
            b = static_cast<double>(linear_to_pq(static_cast<float>(
                gamma_to_linear(b, 2.2) * config.pq_scale)));
        }
        // Otherwise the values are already PQ encoded.
    }
    else if (config.source == SourceEncoding::SdrDisplayNative)
    {
        const double r_lin = gamma_to_linear(r, config.display_decode_gamma);
        const double g_lin = gamma_to_linear(g, config.display_decode_gamma);
        const double b_lin = gamma_to_linear(b, config.display_decode_gamma);

        const std::array<double, 3> target =
            config.display_to_target * std::array<double, 3>{r_lin, g_lin, b_lin};

        r = clamp01d(srgb_oetf(target[0]));
        g = clamp01d(srgb_oetf(target[1]));
        b = clamp01d(srgb_oetf(target[2]));
    }
}

} // namespace

float linear_to_pq(float linear)
{
    // ST-2084 constants (exact rationals, computed in double precision)
    constexpr double m1 = 2610.0 / 16384.0; // 0.1593017578125
    constexpr double m2 = 2523.0 / 32.0;    // 78.84375
    constexpr double c1 = 3424.0 / 4096.0;  // 0.8359375
    constexpr double c2 = 2413.0 / 128.0;   // 18.8515625
    constexpr double c3 = 2392.0 / 128.0;   // 18.6875

    const double l = clamp01d(static_cast<double>(linear));
    const double lp = std::pow(l, m1);
    const double num = c1 + c2 * lp;
    const double den = 1.0 + c3 * lp;
    return static_cast<float>(std::pow(num / den, m2));
}

Mat3 target_rgb_to_yuv_matrix(bool bt2020)
{
    // Full-range YUV (BT.709 / BT.2020), computed to higher precision than the
    // specification's rounded coefficients.
    if (bt2020)
    {
        Mat3 m;
        m.m = {{
            {{0.2627000000, 0.6780000000, 0.0593000000}},
            {{-0.1396300627, -0.3603699373, 0.5000000000}},
            {{0.5000000000, -0.4597857046, -0.0402142954}},
        }};
        return m;
    }

    Mat3 m;
    m.m = {{
        {{0.2126000000, 0.7152000000, 0.0722000000}},
        {{-0.1145721061, -0.3854278939, 0.5000000000}},
        {{0.5000000000, -0.4541529083, -0.0458470917}},
    }};
    return m;
}

bool transform_rgba32f_to_yuv444p16(
    const float *rgba,
    uint32_t width,
    uint32_t height,
    const ColorTransformConfig &config,
    std::vector<uint16_t> &y,
    std::vector<uint16_t> &u,
    std::vector<uint16_t> &v)
{
    if (!rgba || width == 0 || height == 0)
        return false;

    const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    y.resize(px);
    u.resize(px);
    v.resize(px);

    const auto &m = config.target_rgb_to_yuv;

    for (size_t i = 0; i < px; ++i)
    {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        decode_to_target_rgb(rgba + i * 4u, config, r, g, b);

        const double yy = r * m.m[0][0] + g * m.m[0][1] + b * m.m[0][2];
        const double uu = r * m.m[1][0] + g * m.m[1][1] + b * m.m[1][2];
        const double vv = r * m.m[2][0] + g * m.m[2][1] + b * m.m[2][2];

        y[i] = q16(yy);
        u[i] = q16(uu + 0.5);
        v[i] = q16(vv + 0.5);
    }

    return true;
}

bool transform_rgba32f_to_rgb10(
    const float *rgba,
    uint32_t width,
    uint32_t height,
    const ColorTransformConfig &config,
    std::vector<uint16_t> &out)
{
    if (!rgba || width == 0 || height == 0)
        return false;

    const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    out.resize(px * 3u);

    for (size_t i = 0; i < px; ++i)
    {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        decode_to_target_rgb(rgba + i * 4u, config, r, g, b);

        out[i * 3u + 0] = quantize_u16(r, 1023.0);
        out[i * 3u + 1] = quantize_u16(g, 1023.0);
        out[i * 3u + 2] = quantize_u16(b, 1023.0);
    }

    return true;
}

} // namespace kmshot
