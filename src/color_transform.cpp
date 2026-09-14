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

// Linear -> sRGB signal encoding, used for the SDR target encode.
inline double srgb_oetf(double lin)
{
    const double x = std::max(0.0, lin);
    if (x <= 0.0031308)
        return 12.92 * x;
    return 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}

// ST-2084 (PQ) OETF in double precision.
inline double pq_oetf(double linear)
{
    constexpr double m1 = 2610.0 / 16384.0; // 0.1593017578125
    constexpr double m2 = 2523.0 / 32.0;    // 78.84375
    constexpr double c1 = 3424.0 / 4096.0;  // 0.8359375
    constexpr double c2 = 2413.0 / 128.0;   // 18.8515625
    constexpr double c3 = 2392.0 / 128.0;   // 18.6875

    const double l = clamp01d(linear);
    const double lp = std::pow(l, m1);
    const double num = c1 + c2 * lp;
    const double den = 1.0 + c3 * lp;
    return std::pow(num / den, m2);
}

// The per-channel transfer functions are the hot spot of the capture loop:
// evaluated directly they cost six double pow() calls per pixel, which
// dominates the frame time (~220 ms for a 2560x1600 SDR frame and ~410 ms for
// HDR on a modern CPU). They are therefore evaluated through lookup tables.
//
// The tables are sampled on a grid that is uniform in sqrt(t) rather than in t.
// The gamma and especially the PQ curve are extremely steep near zero (PQ rises
// from 0 to 0.15 within t = 1e-4), so a uniform grid would need an impractical
// number of entries there. With this grid the worst-case interpolation error is
// far below one 16-bit LSB for every transfer function involved.
class TransferLut
{
public:
    template <typename Fn>
    void build(Fn fn)
    {
        values_.resize(kSize + 1);
        for (int i = 0; i <= kSize; ++i)
        {
            const double u = static_cast<double>(i) / kSize;
            values_[static_cast<size_t>(i)] = static_cast<float>(fn(u * u));
        }
    }

    double eval(double x) const
    {
        if (!(x > 0.0)) // also catches NaN
            return values_.front();
        if (x >= 1.0)
            return values_.back();

        const double pos = std::sqrt(x) * kSize;
        const size_t i = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(i);
        const double lo = values_[i];
        return lo + frac * (static_cast<double>(values_[i + 1]) - lo);
    }

private:
    // 4096 entries on the sqrt grid keep the worst-case interpolation error at
    // about 0.06 of a 16-bit LSB for every transfer function while still fitting
    // in L1, which matters because each pixel does up to six lookups.
    static constexpr int kSize = 4096;
    std::vector<float> values_;
};

struct TransferTables
{
    TransferLut decode;        // gamma encoded -> linear (SDR path)
    TransferLut encode_srgb;   // linear -> sRGB (SDR path)
    // Fused gamma 2.2 -> PQ for the KDE HDR path. Fusing matters: the bare PQ
    // curve spans ~14 decades of luminance near zero and would need a much finer
    // table on its own, while the composite with the gamma decode is smooth.
    TransferLut gamma22_to_pq;
};

void build_transfer_tables(const ColorTransformConfig &config, TransferTables &tables)
{
    if (config.source == SourceEncoding::SdrDisplayNative)
    {
        const double gamma = config.display_decode_gamma;
        tables.decode.build([gamma](double v) { return std::pow(v, gamma); });
        tables.encode_srgb.build([](double lin) { return srgb_oetf(lin); });
    }
    else if (config.source == SourceEncoding::HdrPqBt2020 && config.pq_input_is_gamma22)
    {
        const double scale = config.pq_scale;
        tables.gamma22_to_pq.build([scale](double v) { return pq_oetf(std::pow(v, 2.2) * scale); });
    }
}

// Decodes one source pixel (4 interleaved floats) into target-space RGB encoded
// with the target transfer function. No RGB -> YUV matrix is applied, so this is
// shared by the YUV and the RGB output paths.
inline void decode_to_target_rgb(const float *pixel,
                                 const ColorTransformConfig &config,
                                 const TransferTables &tables,
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
            r = tables.gamma22_to_pq.eval(r);
            g = tables.gamma22_to_pq.eval(g);
            b = tables.gamma22_to_pq.eval(b);
        }
        // Otherwise the values are already PQ encoded.
    }
    else if (config.source == SourceEncoding::SdrDisplayNative)
    {
        const double r_lin = tables.decode.eval(r);
        const double g_lin = tables.decode.eval(g);
        const double b_lin = tables.decode.eval(b);

        const std::array<double, 3> target =
            config.display_to_target * std::array<double, 3>{r_lin, g_lin, b_lin};

        r = clamp01d(tables.encode_srgb.eval(target[0]));
        g = clamp01d(tables.encode_srgb.eval(target[1]));
        b = clamp01d(tables.encode_srgb.eval(target[2]));
    }
}

} // namespace

float linear_to_pq(float linear)
{
    return static_cast<float>(pq_oetf(static_cast<double>(linear)));
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
    TransferTables tables;
    build_transfer_tables(config, tables);

    for (size_t i = 0; i < px; ++i)
    {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        decode_to_target_rgb(rgba + i * 4u, config, tables, r, g, b);

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

    TransferTables tables;
    build_transfer_tables(config, tables);

    for (size_t i = 0; i < px; ++i)
    {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        decode_to_target_rgb(rgba + i * 4u, config, tables, r, g, b);

        out[i * 3u + 0] = quantize_u16(r, 1023.0);
        out[i * 3u + 1] = quantize_u16(g, 1023.0);
        out[i * 3u + 2] = quantize_u16(b, 1023.0);
    }

    return true;
}

bool quantize_yuv444p16(
    const float *yuv,
    uint32_t width,
    uint32_t height,
    uint32_t channels,
    std::vector<uint16_t> &y,
    std::vector<uint16_t> &u,
    std::vector<uint16_t> &v)
{
    if (!yuv || width == 0 || height == 0 || channels < 3)
        return false;

    const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    y.resize(px);
    u.resize(px);
    v.resize(px);

    for (size_t i = 0; i < px; ++i)
    {
        const size_t base = i * channels;
        y[i] = q16(yuv[base + 0]);
        u[i] = q16(yuv[base + 1]);
        v[i] = q16(yuv[base + 2]);
    }

    return true;
}

bool split_yuv444p16(
    const uint16_t *yuv,
    uint32_t width,
    uint32_t height,
    uint32_t channels,
    std::vector<uint16_t> &y,
    std::vector<uint16_t> &u,
    std::vector<uint16_t> &v)
{
    if (!yuv || width == 0 || height == 0 || channels < 3)
        return false;

    const size_t px = static_cast<size_t>(width) * static_cast<size_t>(height);
    y.resize(px);
    u.resize(px);
    v.resize(px);

    for (size_t i = 0; i < px; ++i)
    {
        const size_t base = i * channels;
        y[i] = yuv[base + 0];
        u[i] = yuv[base + 1];
        v[i] = yuv[base + 2];
    }

    return true;
}

} // namespace kmshot
