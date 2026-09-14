// Cross-checks the LittleCMS2 derived linear RGB -> linear RGB matrices against
// a self-contained primaries + Bradford chromatic adaptation implementation.
#include "color_math.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

using namespace kmshot;

namespace
{

int g_failures = 0;

// Solves A * x = b for a 3x3 system with partial pivoting.
std::array<double, 3> solve3(std::array<std::array<double, 3>, 3> a,
                             std::array<double, 3> b)
{
    for (int col = 0; col < 3; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col]))
                pivot = r;

        std::swap(a[col], a[pivot]);
        std::swap(b[col], b[pivot]);

        const double d = a[col][col];
        for (int c = col; c < 3; ++c)
            a[col][c] /= d;
        b[col] /= d;

        for (int r = 0; r < 3; ++r)
        {
            if (r == col)
                continue;
            const double factor = a[r][col];
            for (int c = col; c < 3; ++c)
                a[r][c] -= factor * a[col][c];
            b[r] -= factor * b[col];
        }
    }
    return b;
}

std::array<double, 3> xy_to_xyz(double x, double y)
{
    return {x / y, 1.0, (1.0 - x - y) / y};
}

// Columns are the XYZ coordinates of the primaries, scaled so that (1,1,1)
// lands on the white point with Y = 1.
Mat3 primaries_to_xyz(const Chromaticities &c)
{
    Mat3 chroma;
    chroma.m = {{
        {{c.red_x / c.red_y, c.green_x / c.green_y, c.blue_x / c.blue_y}},
        {{1.0, 1.0, 1.0}},
        {{(1.0 - c.red_x - c.red_y) / c.red_y,
          (1.0 - c.green_x - c.green_y) / c.green_y,
          (1.0 - c.blue_x - c.blue_y) / c.blue_y}},
    }};

    const auto white = xy_to_xyz(c.white_x, c.white_y);
    const auto scale = solve3(chroma.m, white);

    return chroma * Mat3::diagonal(scale[0], scale[1], scale[2]);
}

Mat3 bradford_adaptation(double src_x, double src_y, double dst_x, double dst_y)
{
    // Bradford cone response matrix.
    Mat3 m;
    m.m = {{
        {{0.8951, 0.2664, -0.1614}},
        {{-0.7502, 1.7135, 0.0367}},
        {{0.0389, -0.0685, 1.0296}},
    }};

    const auto src = xy_to_xyz(src_x, src_y);
    const auto dst = xy_to_xyz(dst_x, dst_y);

    const auto src_cone = m * src;
    const auto dst_cone = m * dst;

    Mat3 ratio = Mat3::diagonal(
        dst_cone[0] / src_cone[0],
        dst_cone[1] / src_cone[1],
        dst_cone[2] / src_cone[2]);

    auto m_inv = m.inverse();
    if (!m_inv)
    {
        std::cerr << "bradford matrix is singular\n";
        std::exit(2);
    }
    return *m_inv * ratio * m;
}

Mat3 reference_matrix(const Chromaticities &src, const Chromaticities &dst)
{
    const Mat3 src_xyz = primaries_to_xyz(src);
    const Mat3 dst_xyz = primaries_to_xyz(dst);
    const Mat3 adaptation = bradford_adaptation(src.white_x, src.white_y, dst.white_x, dst.white_y);

    auto dst_inv = dst_xyz.inverse();
    if (!dst_inv)
    {
        std::cerr << "destination primaries matrix is singular\n";
        std::exit(2);
    }
    return *dst_inv * adaptation * src_xyz;
}

void compare(const std::string &label, const Chromaticities &src, const Chromaticities &dst)
{
    const auto lcms = display_to_display_matrix(src, dst);
    if (!lcms)
    {
        std::cout << "  FAIL " << label << " (lcms2 could not build the transform)\n";
        ++g_failures;
        return;
    }

    const Mat3 reference = reference_matrix(src, dst);
    const double diff = lcms->max_abs_difference(reference);

    if (diff <= 1e-5)
    {
        std::cout << "  ok   " << label << " (max |lcms2 - reference| = " << diff << ")\n";
    }
    else
    {
        std::cout << "  FAIL " << label << " (max |lcms2 - reference| = " << diff << ")\n";
        std::cout << "lcms2:\n" << lcms->to_string() << "\nreference:\n" << reference.to_string() << "\n";
        ++g_failures;
    }
}

const GamutPreset &preset(const char *name)
{
    const GamutPreset *p = find_gamut_preset(name);
    if (!p)
    {
        std::cerr << "missing preset " << name << "\n";
        std::exit(2);
    }
    return *p;
}

} // namespace

int main()
{
    if (!have_lcms2())
    {
        std::cout << "lcms2 is not available in this build\n";
        return 1;
    }

    const Chromaticities panel{0.6797, 0.3174, 0.2422, 0.7168, 0.1416, 0.0527, 0.3184, 0.3340};
    const Chromaticities &srgb = preset("srgb").chroma;
    const Chromaticities &bt2020 = preset("bt2020").chroma;
    const Chromaticities &p3 = preset("display-p3").chroma;
    const Chromaticities &adobe = preset("adobe-rgb").chroma;

    std::cout << "Comparing lcms2 matrices against the primaries + Bradford reference:\n";
    compare("panel -> srgb", panel, srgb);
    compare("panel -> bt2020", panel, bt2020);
    compare("panel -> display-p3", panel, p3);
    compare("srgb -> bt2020", srgb, bt2020);
    compare("bt2020 -> srgb", bt2020, srgb);
    compare("adobe-rgb -> display-p3", adobe, p3);
    compare("display-p3 -> srgb", p3, srgb);

    // Identity and round-trip sanity checks.
    auto identity = display_to_display_matrix(panel, panel);
    if (identity && identity->max_abs_difference(Mat3::identity()) <= 1e-6)
    {
        std::cout << "  ok   panel -> panel is the identity\n";
    }
    else
    {
        std::cout << "  FAIL panel -> panel is not the identity\n";
        ++g_failures;
    }

    auto forward = display_to_display_matrix(panel, bt2020);
    auto backward = display_to_display_matrix(bt2020, panel);
    if (forward && backward)
    {
        const Mat3 roundtrip = (*backward) * (*forward);
        const double diff = roundtrip.max_abs_difference(Mat3::identity());
        if (diff <= 1e-5)
        {
            std::cout << "  ok   panel <-> bt2020 round-trip (max error " << diff << ")\n";
        }
        else
        {
            std::cout << "  FAIL panel <-> bt2020 round-trip (max error " << diff << ")\n";
            ++g_failures;
        }
    }
    else
    {
        std::cout << "  FAIL panel <-> bt2020 round-trip could not be built\n";
        ++g_failures;
    }

    if (g_failures != 0)
    {
        std::cout << g_failures << " comparison(s) failed\n";
        return 1;
    }

    std::cout << "all colour matrix checks passed\n";
    return 0;
}
