// Geometry checks for the slurp -> buffer crop mapping.
#include "capture.hpp"

#include <iostream>
#include <string>

using namespace kmshot;

namespace
{

int g_failures = 0;

void check_rect(const std::string &label,
                const std::optional<CropRect> &actual,
                const std::optional<CropRect> &expected)
{
    const bool same = actual.has_value() == expected.has_value() &&
                      (!actual || (actual->x == expected->x && actual->y == expected->y &&
                                   actual->w == expected->w && actual->h == expected->h));
    if (same)
    {
        std::cout << "  ok   " << label << "\n";
    }
    else
    {
        std::cout << "  FAIL " << label << "\n";
        ++g_failures;
    }
}

CropRect rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return CropRect{x, y, w, h};
}

} // namespace

int main()
{
    std::cout << "Slurp -> buffer crop mapping:\n";

    // Whole capture area, no offset or scaling.
    check_rect("full frame",
               compute_crop_rect_for_buffer({0, 0, 100, 50}, 0, 0, 100, 50, 100, 50, 1.0, 1.0),
               rect(0, 0, 100, 50));

    // Capture plane placed at a global offset.
    check_rect("plane offset",
               compute_crop_rect_for_buffer({200, 250, 100, 50}, 100, 100, 1000, 1000, 1000, 1000, 1.0, 1.0),
               rect(100, 150, 100, 50));

    // Framebuffer larger than the CRTC size (fractional scaling).
    check_rect("buffer downscale",
               compute_crop_rect_for_buffer({10, 10, 10, 10}, 0, 0, 100, 50, 200, 100, 1.0, 1.0),
               rect(20, 20, 20, 20));

    // slurp logical -> physical scaling.
    check_rect("slurp scale",
               compute_crop_rect_for_buffer({10, 10, 10, 10}, 0, 0, 1000, 1000, 1000, 1000, 2.0, 2.0),
               rect(20, 20, 20, 20));

    // Partially off-screen regions are clamped, not rejected.
    check_rect("clamped at origin",
               compute_crop_rect_for_buffer({-50, -50, 100, 100}, 0, 0, 1000, 1000, 1000, 1000, 1.0, 1.0),
               rect(0, 0, 50, 50));

    // Fully off-screen regions are rejected.
    check_rect("outside capture area",
               compute_crop_rect_for_buffer({2000, 2000, 10, 10}, 0, 0, 1000, 1000, 1000, 1000, 1.0, 1.0),
               std::nullopt);

    // Degenerate inputs.
    check_rect("zero-size region",
               compute_crop_rect_for_buffer({0, 0, 0, 10}, 0, 0, 100, 50, 100, 50, 1.0, 1.0),
               std::nullopt);
    check_rect("zero-size buffer",
               compute_crop_rect_for_buffer({0, 0, 10, 10}, 0, 0, 100, 50, 0, 50, 1.0, 1.0),
               std::nullopt);
    check_rect("non-positive scale",
               compute_crop_rect_for_buffer({0, 0, 10, 10}, 0, 0, 100, 50, 100, 50, 0.0, 1.0),
               std::nullopt);

    if (g_failures != 0)
    {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "all crop checks passed\n";
    return 0;
}
