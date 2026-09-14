// Encodes frames with the in-process libavif writer and decodes the result back
// to verify the CICP metadata, chroma subsampling, content light level and the
// still-vs-sequence choice.
#include "color_transform.hpp"
#include "encoder.hpp"

#include <avif/avif.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

void check_eq(long long actual, long long expected, const std::string &what)
{
    if (actual == expected)
    {
        std::cout << "  ok   " << what << "\n";
    }
    else
    {
        std::cout << "  FAIL " << what << " (got " << actual << ", expected " << expected << ")\n";
        ++g_failures;
    }
}

ColorTransformConfig make_config(bool hdr, bool bt2020)
{
    ColorTransformConfig cfg;
    cfg.source = hdr ? SourceEncoding::HdrPqBt2020 : SourceEncoding::SdrDisplayNative;
    cfg.display_to_target = Mat3::identity();
    cfg.target_rgb_to_yuv = target_rgb_to_yuv_matrix(bt2020);
    cfg.target_bt2020 = bt2020;
    cfg.display_decode_gamma = 2.2;
    return cfg;
}

std::vector<float> make_gradient(uint32_t width, uint32_t height)
{
    std::vector<float> rgba(static_cast<size_t>(width) * height * 4u);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
            rgba[i + 0] = width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0f;
            rgba[i + 1] = height > 1 ? static_cast<float>(y) / static_cast<float>(height - 1) : 0.0f;
            rgba[i + 2] = 0.5f;
            rgba[i + 3] = 1.0f;
        }
    }
    return rgba;
}

bool file_starts_with_ftyp(const std::string &path, std::string &brand)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;

    unsigned char header[12] = {};
    const size_t read = std::fread(header, 1, sizeof(header), f);
    std::fclose(f);

    if (read != sizeof(header))
        return false;
    if (header[4] != 'f' || header[5] != 't' || header[6] != 'y' || header[7] != 'p')
        return false;

    brand.assign(reinterpret_cast<const char *>(header + 8), 4);
    return true;
}

struct DecodedInfo
{
    bool ok{false};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{0};
    avifPixelFormat format{AVIF_PIXEL_FORMAT_NONE};
    int primaries{-1};
    int transfer{-1};
    int matrix{-1};
    int range{-1};
    int image_count{0};
    uint16_t max_cll{0};
    uint16_t max_pall{0};
};

DecodedInfo decode_still(const std::string &path)
{
    DecodedInfo info;

    avifDecoder *decoder = avifDecoderCreate();
    avifImage *image = avifImageCreateEmpty();
    if (!decoder || !image)
    {
        if (decoder)
            avifDecoderDestroy(decoder);
        if (image)
            avifImageDestroy(image);
        return info;
    }

    const avifResult res = avifDecoderReadFile(decoder, image, path.c_str());
    if (res != AVIF_RESULT_OK)
    {
        std::cerr << "decode of " << path << " failed: " << avifResultToString(res) << "\n";
    }
    else
    {
        info.ok = true;
        info.width = image->width;
        info.height = image->height;
        info.depth = image->depth;
        info.format = image->yuvFormat;
        info.primaries = image->colorPrimaries;
        info.transfer = image->transferCharacteristics;
        info.matrix = image->matrixCoefficients;
        info.range = image->yuvRange;
        info.max_cll = image->clli.maxCLL;
        info.max_pall = image->clli.maxPALL;
    }

    avifImageDestroy(image);
    avifDecoderDestroy(decoder);
    return info;
}

int count_frames(const std::string &path)
{
    avifDecoder *decoder = avifDecoderCreate();
    if (!decoder)
        return -1;

    int frames = -1;
    if (avifDecoderSetIOFile(decoder, path.c_str()) == AVIF_RESULT_OK &&
        avifDecoderParse(decoder) == AVIF_RESULT_OK)
    {
        frames = 0;
        while (avifDecoderNextImage(decoder) == AVIF_RESULT_OK)
            ++frames;
    }

    avifDecoderDestroy(decoder);
    return frames;
}

std::string encode(const std::string &path,
                   const ColorTransformConfig &color,
                   const AvifSettings &settings,
                   const std::vector<float> &rgba,
                   uint32_t width,
                   uint32_t height,
                   bool sequence,
                   int frame_count)
{
    std::string error;
    AvifWriter writer;
    if (!writer.open(path, width, height, settings, color, sequence,
                     static_cast<uint32_t>(30), error))
    {
        return error;
    }

    for (int i = 0; i < frame_count; ++i)
    {
        if (!writer.add_frame(rgba.data(), color, error))
            return error;
    }

    if (!writer.finish(error))
        return error;
    return {};
}

} // namespace

int main()
{
    const uint32_t width = 16;
    const uint32_t height = 8;
    const std::vector<float> gradient = make_gradient(width, height);

    // --- Still image, 4:4:4, SDR BT.2020 with an explicit content light level.
    {
        std::cout << "SDR BT.2020 4:4:4 still:\n";
        const std::string path = "kmshot_avif_test_444.avif";
        AvifSettings settings;
        settings.subsampling = "444";
        settings.clli = std::make_pair(static_cast<uint16_t>(1000), static_cast<uint16_t>(400));

        const std::string err = encode(path, make_config(false, true), settings,
                                       gradient, width, height, false, 1);
        check(err.empty(), "encoding succeeded" + (err.empty() ? "" : ": " + err));

        const DecodedInfo info = decode_still(path);
        check(info.ok, "output decodes");
        check_eq(info.width, width, "width");
        check_eq(info.height, height, "height");
        check_eq(info.depth, 10, "depth is 10-bit");
        check_eq(info.format, AVIF_PIXEL_FORMAT_YUV444, "4:4:4");
        check_eq(info.primaries, AVIF_COLOR_PRIMARIES_BT2020, "CICP primaries BT.2020");
        check_eq(info.transfer, AVIF_TRANSFER_CHARACTERISTICS_SRGB, "CICP transfer sRGB");
        check_eq(info.matrix, AVIF_MATRIX_COEFFICIENTS_BT2020_NCL, "CICP matrix BT.2020 NCL");
        check_eq(info.range, AVIF_RANGE_FULL, "full range");
        check_eq(info.max_cll, 1000, "MaxCLL");
        check_eq(info.max_pall, 400, "MaxPALL");

        std::string brand;
        check(file_starts_with_ftyp(path, brand) && brand == "avif",
              "ISOBMFF ftyp brand is avif");
        std::remove(path.c_str());
    }

    // --- Still image, 4:2:0 downsampled by libavif, SDR BT.709.
    {
        std::cout << "SDR BT.709 4:2:0 still:\n";
        const std::string path = "kmshot_avif_test_420.avif";
        AvifSettings settings;
        settings.subsampling = "420";

        const std::string err = encode(path, make_config(false, false), settings,
                                       gradient, width, height, false, 1);
        check(err.empty(), "encoding succeeded" + (err.empty() ? "" : ": " + err));

        const DecodedInfo info = decode_still(path);
        check(info.ok, "output decodes");
        check_eq(info.format, AVIF_PIXEL_FORMAT_YUV420, "4:2:0");
        check_eq(info.primaries, AVIF_COLOR_PRIMARIES_BT709, "CICP primaries BT.709");
        check_eq(info.transfer, AVIF_TRANSFER_CHARACTERISTICS_SRGB, "CICP transfer sRGB");
        check_eq(info.matrix, AVIF_MATRIX_COEFFICIENTS_BT709, "CICP matrix BT.709");
        check_eq(info.max_cll, 0, "no clli box without --avif-clli");

        std::remove(path.c_str());
    }

    // --- Still image, HDR PQ, 4:2:2 with CICP overrides.
    {
        std::cout << "HDR PQ 4:2:2 still with CICP override:\n";
        const std::string path = "kmshot_avif_test_hdr.avif";
        AvifSettings settings;
        settings.subsampling = "422";
        settings.cicp_primaries = AVIF_COLOR_PRIMARIES_BT2020;
        settings.cicp_transfer = AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084;
        settings.cicp_matrix = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
        settings.clli = std::make_pair(static_cast<uint16_t>(1261), static_cast<uint16_t>(604));

        const std::string err = encode(path, make_config(true, true), settings,
                                       gradient, width, height, false, 1);
        check(err.empty(), "encoding succeeded" + (err.empty() ? "" : ": " + err));

        const DecodedInfo info = decode_still(path);
        check(info.ok, "output decodes");
        check_eq(info.format, AVIF_PIXEL_FORMAT_YUV422, "4:2:2");
        check_eq(info.transfer, AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084, "CICP transfer PQ");
        check_eq(info.max_cll, 1261, "MaxCLL");
        check_eq(info.max_pall, 604, "MaxPALL");

        std::remove(path.c_str());
    }

    // --- Image sequence.
    {
        std::cout << "Three-frame sequence:\n";
        const std::string path = "kmshot_avif_test_seq.avif";
        AvifSettings settings;
        settings.subsampling = "420";

        const std::string err = encode(path, make_config(false, true), settings,
                                       gradient, width, height, true, 3);
        check(err.empty(), "encoding succeeded" + (err.empty() ? "" : ": " + err));
        check_eq(count_frames(path), 3, "decoded frame count");

        std::string brand;
        check(file_starts_with_ftyp(path, brand) && brand == "avis",
              "ISOBMFF ftyp brand is avis");

        std::remove(path.c_str());
    }

    // --- Pixel round-trip: a solid mid gray must survive encoding/decoding.
    // Metadata checks alone would not catch a broken RGB -> YUV conversion.
    {
        std::cout << "Pixel round-trip (solid mid gray):\n";
        const std::string path = "kmshot_avif_test_pixels.avif";
        AvifSettings settings;
        settings.subsampling = "444";

        ColorTransformConfig cfg;
        cfg.source = SourceEncoding::AssumedTarget; // values pass through unchanged
        cfg.display_to_target = Mat3::identity();
        cfg.target_rgb_to_yuv = target_rgb_to_yuv_matrix(true);
        cfg.target_bt2020 = true;

        std::vector<float> solid(static_cast<size_t>(width) * height * 4u, 0.5f);
        for (size_t i = 3; i < solid.size(); i += 4u)
            solid[i] = 1.0f;

        const std::string err = encode(path, cfg, settings, solid, width, height, false, 1);
        check(err.empty(), "encoding succeeded" + (err.empty() ? "" : ": " + err));

        avifDecoder *decoder = avifDecoderCreate();
        avifImage *image = avifImageCreateEmpty();
        const bool decoded = decoder && image &&
                             avifDecoderReadFile(decoder, image, path.c_str()) == AVIF_RESULT_OK;
        check(decoded, "output decodes");
        if (decoded)
        {
            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, image);
            rgb.format = AVIF_RGB_FORMAT_RGB;

            if (avifRGBImageAllocatePixels(&rgb) == AVIF_RESULT_OK &&
                avifImageYUVToRGB(image, &rgb) == AVIF_RESULT_OK)
            {
                const auto *px = reinterpret_cast<const uint16_t *>(rgb.pixels);
                const int r = px[0];
                const int g = px[1];
                const int b = px[2];
                std::cout << "       decoded RGB(" << r << ", " << g << ", " << b
                          << "), expected ~(512, 512, 512)\n";
                check(std::abs(r - 512) <= 8 && std::abs(g - 512) <= 8 && std::abs(b - 512) <= 8,
                      "solid mid gray round-trips");
                avifRGBImageFreePixels(&rgb);
            }
            else
            {
                check(false, "YUV -> RGB conversion succeeded");
            }
        }

        avifImageDestroy(image);
        avifDecoderDestroy(decoder);
        std::remove(path.c_str());
    }

    if (g_failures != 0)
    {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "all AVIF checks passed\n";
    return 0;
}
