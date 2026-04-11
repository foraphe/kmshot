#include "color_transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <iostream>
#include <cstdlib>

#if __has_include(<lcms2.h>)
#include <lcms2.h>
#define KMSHOT_HAVE_LCMS2 1
#else
#define KMSHOT_HAVE_LCMS2 0
#endif

namespace kmshot
{
static inline double clamp01d(double v)
{
    return std::min(1.0, std::max(0.0, v));
}

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

static inline uint16_t q10(double v01)
{
    const double q = clamp01d(v01) * 1023.0;
    return static_cast<uint16_t>(std::llround(q));
}

static inline double gamma22_to_linear(double v)
{
    return std::pow(clamp01d(v), 2.2);
}

// For linear -> signal encoding.
static inline double srgb_oetf(double lin)
{
    const double x = std::max(0.0, lin);
    if (x <= 0.0031308)
        return 12.92 * x;
    return 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
}

#if KMSHOT_HAVE_LCMS2
struct SdrCms
{
    cmsHPROFILE src_lin_monitor{nullptr};
    cmsHPROFILE dst_lin709{nullptr};
    cmsHPROFILE dst_lin2020{nullptr};
    cmsHTRANSFORM xform_mon_to_709_lin{nullptr};
    cmsHTRANSFORM xform_mon_to_2020_lin{nullptr};

    ~SdrCms()
    {
        if (xform_mon_to_709_lin) cmsDeleteTransform(xform_mon_to_709_lin);
        if (xform_mon_to_2020_lin) cmsDeleteTransform(xform_mon_to_2020_lin);
        if (src_lin_monitor) cmsCloseProfile(src_lin_monitor);
        if (dst_lin709) cmsCloseProfile(dst_lin709);
        if (dst_lin2020) cmsCloseProfile(dst_lin2020);
    }
};

static cmsHPROFILE create_rgb_profile_gamma(
    const cmsCIExyY& wp, const cmsCIExyYTRIPLE& primaries, double gamma)
{
    cmsToneCurve* g = cmsBuildGamma(nullptr, gamma);
    if (!g) return nullptr;
    cmsToneCurve* curves[3] = {g, g, g};
    cmsHPROFILE p = cmsCreateRGBProfile(&wp, &primaries, curves);
    cmsFreeToneCurve(g);
    return p;
}

static SdrCms& sdr_cms()
{
    static SdrCms cms;
    static bool initialized = false;
    if (initialized) return cms;
    initialized = true;

    static constexpr cmsCIExyY kD65{0.3127, 0.3290, 1.0};
    static constexpr cmsCIExyY kMonitorWhite{/*x*/0.3121, /*y*/0.3299, 1.0};
    static constexpr cmsCIExyYTRIPLE kMonitorPrimaries{
        {0.6874, 0.3104, 1.0}, // R
        {0.2378, 0.7271, 1.0}, // G
        {0.1427, 0.0543, 1.0}  // B
    };
    static constexpr cmsCIExyYTRIPLE kRec709Primaries{
        {0.640, 0.330, 1.0},
        {0.300, 0.600, 1.0},
        {0.150, 0.060, 1.0}
    };
    static constexpr cmsCIExyYTRIPLE kBt2020Primaries{
        {0.708, 0.292, 1.0},
        {0.170, 0.797, 1.0},
        {0.131, 0.046, 1.0}
    };

    // Linear to SDR-like gamut conversions
    cms.src_lin_monitor = create_rgb_profile_gamma(kMonitorWhite, kMonitorPrimaries, 1.0);
    cms.dst_lin709      = create_rgb_profile_gamma(kD65, kRec709Primaries, 1.0);
    cms.dst_lin2020     = create_rgb_profile_gamma(kD65, kBt2020Primaries, 1.0);

    constexpr uint32_t flags = cmsFLAGS_NOOPTIMIZE;

    if (cms.src_lin_monitor && cms.dst_lin709) {
        cms.xform_mon_to_709_lin = cmsCreateTransform(
            cms.src_lin_monitor, TYPE_RGB_FLT,
            cms.dst_lin709, TYPE_RGB_FLT,
            INTENT_RELATIVE_COLORIMETRIC, flags);
    }

    if (cms.src_lin_monitor && cms.dst_lin2020) {
        cms.xform_mon_to_2020_lin = cmsCreateTransform(
            cms.src_lin_monitor, TYPE_RGB_FLT,
            cms.dst_lin2020, TYPE_RGB_FLT,
            INTENT_RELATIVE_COLORIMETRIC, flags);
    }

    std::cerr << "CMS(synthetic linear): mon->709="
              << (cms.xform_mon_to_709_lin ? "OK" : "MISSING")
              << " mon->2020="
              << (cms.xform_mon_to_2020_lin ? "OK" : "MISSING")
              << "\n";
    return cms;
}

static inline bool cms_transform_monitor_lin(double& r, double& g, double& b, bool to_bt2020)
{
    auto& cms = sdr_cms();
    cmsHTRANSFORM t = to_bt2020 ? cms.xform_mon_to_2020_lin : cms.xform_mon_to_709_lin;
    if (!t) return false;

    float in[3] = {
        static_cast<float>(std::max(0.0, r)),
        static_cast<float>(std::max(0.0, g)),
        static_cast<float>(std::max(0.0, b))
    };
    float out[3] = {0.f, 0.f, 0.f};
    cmsDoTransform(t, in, out, 1);

    r = out[0];
    g = out[1];
    b = out[2];
    return true;
}
#endif

bool transform_rgba32f_to_yuv444p10(
    const float *rgba,
    uint32_t width,
    uint32_t height,
    int colorspace_idx,
    float max_nits,
    uint32_t bpc,
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

    // 9 = BT.2020 RGB HDR (PQ), 0 = monitor primaries SDR
    const bool use_hdr_pq_bt2020 = (colorspace_idx == 9);
    const bool use_monitor_primaries_sdr = (colorspace_idx == 0) && !use_hdr_pq_bt2020;
    const double pq_scale = std::max(0.0, static_cast<double>(max_nits)) / 10000.0;

    // Higher-precision coefficients (full-range YUV, Rec.709 / Rec.2020)
    const std::array<std::array<double, 3>, 3> m_bt2020{{
        {{0.2627000000, 0.6780000000, 0.0593000000}},
        {{-0.1396300627, -0.3603699373, 0.5000000000}},
        {{0.5000000000, -0.4597857046, -0.0402142954}},
    }};
    const std::array<std::array<double, 3>, 3> m_bt709{{
        {{0.2126000000, 0.7152000000, 0.0722000000}},
        {{-0.1145721061, -0.3854278939, 0.5000000000}},
        {{0.5000000000, -0.4541529083, -0.0458470917}},
    }};

    // monitor primaries (WCG) linear RGB -> linear Rec.709 RGB
    const std::array<std::array<double, 3>, 3> monitor_to_bt709{{
        {{ 1.37161037, -0.34344087, -0.02816950 }},
        {{-0.06575496,  1.06994514, -0.00419019 }},
        {{-0.01779032, -0.09240594,  1.11019626 }},
    }};
    
    // Hardcoded SDR target switch:
    // false => Rec.709 matrix, true => BT.2020 matrix
    static constexpr bool kSdrTargetBt2020 = true;
    const bool sdr_target_bt2020 = kSdrTargetBt2020;

    const auto &m = (use_hdr_pq_bt2020 || (use_monitor_primaries_sdr && sdr_target_bt2020))
                        ? m_bt2020
                        : m_bt709;

    std::cerr << "PQ: " << (use_hdr_pq_bt2020 ? "ON" : "OFF")
              << ", SDR CMS target: "
              << (use_monitor_primaries_sdr ? (sdr_target_bt2020 ? "BT.2020" : "Rec.709") : "N/A")
              << "\n";

    for (size_t i = 0; i < px; ++i)
    {
        double r = clamp01d(static_cast<double>(rgba[i * 4 + 0]));
        double g = clamp01d(static_cast<double>(rgba[i * 4 + 1]));
        double b = clamp01d(static_cast<double>(rgba[i * 4 + 2]));

        if (use_hdr_pq_bt2020)
        {
            r = std::pow(r, 2.2);
            g = std::pow(g, 2.2);
            b = std::pow(b, 2.2);

            r *= pq_scale; g *= pq_scale; b *= pq_scale;

            r = static_cast<double>(linear_to_pq(static_cast<float>(r)));
            g = static_cast<double>(linear_to_pq(static_cast<float>(g)));
            b = static_cast<double>(linear_to_pq(static_cast<float>(b)));
        }
        else if (use_monitor_primaries_sdr)
        {
            // Decode monitor gamma 2.2 -> monitor linear
            double r_lin = gamma22_to_linear(r);
            double g_lin = gamma22_to_linear(g);
            double b_lin = gamma22_to_linear(b);

#if KMSHOT_HAVE_LCMS2
            // monitor-linear gamut -> target-linear gamut
            const bool ok = cms_transform_monitor_lin(r_lin, g_lin, b_lin, sdr_target_bt2020);
            if (!ok)
#endif
            {
                // Fallback to old matrix path if CMS unavailable/fails
                const double rr = monitor_to_bt709[0][0] * r_lin + monitor_to_bt709[0][1] * g_lin + monitor_to_bt709[0][2] * b_lin;
                const double gg = monitor_to_bt709[1][0] * r_lin + monitor_to_bt709[1][1] * g_lin + monitor_to_bt709[1][2] * b_lin;
                const double bb = monitor_to_bt709[2][0] * r_lin + monitor_to_bt709[2][1] * g_lin + monitor_to_bt709[2][2] * b_lin;
                r_lin = rr; g_lin = gg; b_lin = bb;
            }

            // Encoding to sRGB (sRGB-like 709 or 2020, depending on target)
            r = clamp01d(srgb_oetf(r_lin));
            g = clamp01d(srgb_oetf(g_lin));
            b = clamp01d(srgb_oetf(b_lin));
        }

        const double yy = r * m[0][0] + g * m[0][1] + b * m[0][2];
        const double uu = r * m[1][0] + g * m[1][1] + b * m[1][2];
        const double vv = r * m[2][0] + g * m[2][1] + b * m[2][2];

        y[i] = q10(yy);
        u[i] = q10(uu + 0.5);
        v[i] = q10(vv + 0.5);
    }

    return true;
}

bool write_y4m_header(
    std::ostream &os,
    uint32_t width,
    uint32_t height,
    int fps_num,
    int fps_den)
{
    if (width == 0 || height == 0 || fps_num <= 0 || fps_den <= 0)
        return false;

    const std::string hdr =
        "YUV4MPEG2 W" + std::to_string(width) +
        " H" + std::to_string(height) +
        " F" + std::to_string(fps_num) + ":" + std::to_string(fps_den) +
        " Ip C444p10 XYSCSS=444P10 XCOLORRANGE=FULL\n";
    os.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
    return static_cast<bool>(os);
}

bool write_y4m_frame(
    std::ostream &os,
    const std::vector<uint16_t> &y,
    const std::vector<uint16_t> &u,
    const std::vector<uint16_t> &v)
{
    if (y.size() != u.size() || y.size() != v.size())
        return false;

    static constexpr char kFrame[] = "FRAME\n";
    os.write(kFrame, sizeof(kFrame) - 1);
    os.write(reinterpret_cast<const char *>(y.data()), static_cast<std::streamsize>(y.size() * sizeof(uint16_t)));
    os.write(reinterpret_cast<const char *>(u.data()), static_cast<std::streamsize>(u.size() * sizeof(uint16_t)));
    os.write(reinterpret_cast<const char *>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(uint16_t)));
    return static_cast<bool>(os);
}

} // namespace kmshot