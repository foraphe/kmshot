#include "color_math.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include <lcms2.h>

namespace kmshot
{

namespace
{

constexpr double kD65x = 0.31270;
constexpr double kD65y = 0.32900;

std::vector<std::string> tokenize_numbers(const std::string &text)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text)
    {
        if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::optional<std::vector<double>> parse_numbers(const std::string &text, size_t expected)
{
    const auto tokens = tokenize_numbers(text);
    if (tokens.size() != expected)
        return std::nullopt;

    std::vector<double> out;
    out.reserve(tokens.size());
    for (const auto &t : tokens)
    {
        try
        {
            size_t consumed = 0;
            const double v = std::stod(t, &consumed);
            if (consumed != t.size() || !std::isfinite(v))
                return std::nullopt;
            out.push_back(v);
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }
    return out;
}

const std::vector<GamutPreset> &preset_table()
{
    static const std::vector<GamutPreset> kPresets = {
        {"srgb",
         "sRGB / BT.709 primaries with a D65 white point",
         {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, kD65x, kD65y}},
        {"bt2020",
         "BT.2020 / Rec.2020 primaries with a D65 white point",
         {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, kD65x, kD65y}},
        {"display-p3",
         "Display P3 (DCI-P3 primaries with a D65 white point)",
         {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, kD65x, kD65y}},
        {"dci-p3",
         "DCI-P3 theatrical (DCI white point)",
         {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.314, 0.351}},
        {"adobe-rgb",
         "Adobe RGB (1998)",
         {0.640, 0.330, 0.210, 0.710, 0.150, 0.060, kD65x, kD65y}},
        {"ne160qdm-nm7",
         "Built-in datasheet values for the BOE NE160QDM-NM7 panel",
         {0.6874, 0.3104, 0.2378, 0.7271, 0.1427, 0.0543, 0.3121, 0.3299}},
    };
    return kPresets;
}

} // namespace

Mat3 Mat3::identity()
{
    Mat3 out;
    out.m[0][0] = out.m[1][1] = out.m[2][2] = 1.0;
    return out;
}

Mat3 Mat3::diagonal(double r, double g, double b)
{
    Mat3 out;
    out.m[0][0] = r;
    out.m[1][1] = g;
    out.m[2][2] = b;
    return out;
}

std::array<double, 3> Mat3::operator*(const std::array<double, 3> &v) const
{
    return {
        m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
        m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
        m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
    };
}

Mat3 Mat3::operator*(const Mat3 &rhs) const
{
    Mat3 out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.m[r][c] = m[r][0] * rhs.m[0][c] + m[r][1] * rhs.m[1][c] + m[r][2] * rhs.m[2][c];
    return out;
}

double Mat3::determinant() const
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

std::optional<Mat3> Mat3::inverse() const
{
    const double det = determinant();
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        return std::nullopt;

    const double inv = 1.0 / det;
    Mat3 out;
    out.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    out.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    out.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    out.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    out.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    out.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    out.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    out.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    out.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    return out;
}

bool Mat3::all_finite() const
{
    for (const auto &row : m)
        for (double v : row)
            if (!std::isfinite(v))
                return false;
    return true;
}

double Mat3::max_abs_difference(const Mat3 &other) const
{
    double worst = 0.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs(m[r][c] - other.m[r][c]));
    return worst;
}

std::string Mat3::to_string() const
{
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(6);
    for (int r = 0; r < 3; ++r)
    {
        os << (r == 0 ? "[[" : " [");
        for (int c = 0; c < 3; ++c)
            os << m[r][c] << (c == 2 ? "" : ", ");
        os << (r == 2 ? "]]" : "]\n");
    }
    return os.str();
}

const std::vector<GamutPreset> &gamut_presets()
{
    return preset_table();
}

const GamutPreset *find_gamut_preset(const std::string &name)
{
    std::string key;
    key.reserve(name.size());
    for (char c : name)
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // Common aliases.
    if (key == "bt709" || key == "rec709" || key == "rec.709" || key == "srgb")
        key = "srgb";
    else if (key == "rec2020" || key == "rec.2020" || key == "bt.2020")
        key = "bt2020";
    else if (key == "p3" || key == "p3-d65")
        key = "display-p3";
    else if (key == "adobe")
        key = "adobe-rgb";
    else if (key == "ne160qdm" || key == "ne160qdm-nm7")
        key = "ne160qdm-nm7";

    for (const auto &p : preset_table())
        if (key == p.name)
            return &p;
    return nullptr;
}

std::string gamut_preset_names()
{
    std::string out;
    for (const auto &p : preset_table())
    {
        if (!out.empty())
            out += ", ";
        out += p.name;
    }
    return out;
}

std::optional<std::array<double, 6>> parse_six_values(const std::string &text)
{
    auto v = parse_numbers(text, 6);
    if (!v)
        return std::nullopt;
    return std::array<double, 6>{(*v)[0], (*v)[1], (*v)[2], (*v)[3], (*v)[4], (*v)[5]};
}

std::optional<std::pair<double, double>> parse_two_values(const std::string &text)
{
    auto v = parse_numbers(text, 2);
    if (!v)
        return std::nullopt;
    return std::make_pair((*v)[0], (*v)[1]);
}

std::optional<std::array<double, 9>> parse_nine_values(const std::string &text)
{
    auto v = parse_numbers(text, 9);
    if (!v)
        return std::nullopt;
    std::array<double, 9> out{};
    std::copy(v->begin(), v->end(), out.begin());
    return out;
}

bool is_valid_chromaticities(const Chromaticities &c)
{
    const std::array<std::pair<double, double>, 4> pts = {{
        {c.red_x, c.red_y},
        {c.green_x, c.green_y},
        {c.blue_x, c.blue_y},
        {c.white_x, c.white_y},
    }};
    for (const auto &[x, y] : pts)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
            return false;
        if (x <= 0.0 || y <= 0.0 || x >= 1.0 || y >= 1.0)
            return false;
        // BT.2020 and DCI-P3 place the red primary exactly on the spectral
        // locus (x + y == 1), so only a clear overshoot is invalid.
        if (x + y > 1.0 + 1e-9)
            return false;
    }
    return true;
}

namespace
{

cmsHPROFILE make_linear_rgb_profile(const Chromaticities &c)
{
    cmsCIExyY white{c.white_x, c.white_y, 1.0};
    cmsCIExyYTRIPLE primaries{
        {c.red_x, c.red_y, 1.0},
        {c.green_x, c.green_y, 1.0},
        {c.blue_x, c.blue_y, 1.0},
    };

    cmsToneCurve *gamma = cmsBuildGamma(nullptr, 1.0);
    if (!gamma)
        return nullptr;

    cmsToneCurve *curves[3] = {gamma, gamma, gamma};
    cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
    cmsFreeToneCurve(gamma);
    return profile;
}

} // namespace

std::optional<Mat3> display_to_display_matrix(const Chromaticities &src,
                                              const Chromaticities &dst)
{
    if (!is_valid_chromaticities(src) || !is_valid_chromaticities(dst))
        return std::nullopt;

    cmsHPROFILE src_profile = make_linear_rgb_profile(src);
    cmsHPROFILE dst_profile = make_linear_rgb_profile(dst);
    if (!src_profile || !dst_profile)
    {
        if (src_profile)
            cmsCloseProfile(src_profile);
        if (dst_profile)
            cmsCloseProfile(dst_profile);
        return std::nullopt;
    }

    // NOOPTIMIZE keeps lcms2 on the matrix/curve pipeline instead of collapsing
    // to a sampled LUT, so the transform stays exactly linear.
    cmsHTRANSFORM xform = cmsCreateTransform(
        src_profile, TYPE_RGB_DBL,
        dst_profile, TYPE_RGB_DBL,
        INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_NOOPTIMIZE);

    cmsCloseProfile(src_profile);
    cmsCloseProfile(dst_profile);

    if (!xform)
        return std::nullopt;

    Mat3 out;
    for (int col = 0; col < 3; ++col)
    {
        double in[3] = {0.0, 0.0, 0.0};
        double result[3] = {0.0, 0.0, 0.0};
        in[col] = 1.0;
        cmsDoTransform(xform, in, result, 1);
        out.m[0][col] = result[0];
        out.m[1][col] = result[1];
        out.m[2][col] = result[2];
    }

    cmsDeleteTransform(xform);

    if (!out.all_finite())
        return std::nullopt;
    return out;
}

} // namespace kmshot
