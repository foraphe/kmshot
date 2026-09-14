#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kmshot
{

// Row-major 3x3 matrix.
struct Mat3
{
    std::array<std::array<double, 3>, 3> m{};

    static Mat3 identity();
    static Mat3 diagonal(double r, double g, double b);

    std::array<double, 3> operator*(const std::array<double, 3> &v) const;
    Mat3 operator*(const Mat3 &rhs) const;

    double determinant() const;
    std::optional<Mat3> inverse() const;

    bool all_finite() const;
    double max_abs_difference(const Mat3 &other) const;
    std::string to_string() const;
};

// CIE 1931 xy chromaticities.
struct Chromaticities
{
    double red_x{0.0}, red_y{0.0};
    double green_x{0.0}, green_y{0.0};
    double blue_x{0.0}, blue_y{0.0};
    double white_x{0.0}, white_y{0.0};
};

struct GamutPreset
{
    const char *name;
    const char *description;
    Chromaticities chroma;
};

// Well-known gamuts that can be selected with --display-gamut / --sdr-target.
const std::vector<GamutPreset> &gamut_presets();
const GamutPreset *find_gamut_preset(const std::string &name);
std::string gamut_preset_names();

// Parsers for the comma/space separated lists accepted on the command line.
std::optional<std::array<double, 6>> parse_six_values(const std::string &text);
std::optional<std::pair<double, double>> parse_two_values(const std::string &text);
std::optional<std::array<double, 9>> parse_nine_values(const std::string &text);

bool is_valid_chromaticities(const Chromaticities &c);

// Linear RGB -> linear RGB matrix from one set of display primaries to
// another. Backed by LittleCMS2 (relative colorimetric intent, which applies
// Bradford chromatic adaptation between the two white points). Returns
// std::nullopt when the profiles cannot be built.
std::optional<Mat3> display_to_display_matrix(const Chromaticities &src,
                                              const Chromaticities &dst);

// Whether the LittleCMS2 backed path is available in this build.
bool have_lcms2();

} // namespace kmshot
