#include "encoder.hpp"

#include <avif/avif.h>

#include <fstream>
#include <vector>

namespace kmshot
{

namespace
{

// The encoder is intentionally not tunable from the command line: a screenshot
// is encoded once, so it uses maximum quality and a middle-of-the-road speed.
// The RGB buffer fed to libavif is 10-bit, matching the AVIF output depth.
constexpr int kQuality = 100;
constexpr int kSpeed = 6;
constexpr uint32_t kDepth = 10;
constexpr uint32_t kRgbBytesPerSample = 2; // kDepth > 8

bool parse_subsampling(const std::string &name, avifPixelFormat &format)
{
    if (name == "444")
    {
        format = AVIF_PIXEL_FORMAT_YUV444;
        return true;
    }
    if (name == "422")
    {
        format = AVIF_PIXEL_FORMAT_YUV422;
        return true;
    }
    if (name == "420")
    {
        format = AVIF_PIXEL_FORMAT_YUV420;
        return true;
    }
    return false;
}

} // namespace

struct AvifWriter::Impl
{
    std::string path;
    uint32_t width{0};
    uint32_t height{0};
    bool added{false};
    std::vector<uint16_t> rgb;
    uint32_t rgb_depth{kDepth};
    avifImage *image{nullptr};
    avifEncoder *encoder{nullptr};

    ~Impl()
    {
        if (encoder)
            avifEncoderDestroy(encoder);
        if (image)
            avifImageDestroy(image);
    }
};

AvifWriter::AvifWriter() = default;
AvifWriter::~AvifWriter() = default;

bool AvifWriter::is_open() const
{
    return impl_ && impl_->encoder != nullptr;
}

bool AvifWriter::open(const std::string &path,
                      uint32_t width,
                      uint32_t height,
                      const AvifSettings &settings,
                      const ColorTransformConfig &color,
                      std::string &error)
{
    if (width == 0 || height == 0)
    {
        error = "invalid AVIF frame dimensions";
        return false;
    }

    avifPixelFormat format = AVIF_PIXEL_FORMAT_NONE;
    if (!parse_subsampling(settings.subsampling, format))
    {
        error = "unsupported AVIF chroma subsampling '" + settings.subsampling +
                "' (expected 444, 422 or 420)";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->width = width;
    impl->height = height;
    impl->rgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);

    impl->image = avifImageCreate(width, height, kDepth, format);
    if (!impl->image)
    {
        error = "avifImageCreate failed";
        return false;
    }

    if (!impl->image->yuvPlanes[0])
    {
        const avifResult res = avifImageAllocatePlanes(impl->image, AVIF_PLANES_YUV);
        if (res != AVIF_RESULT_OK)
        {
            error = std::string("avifImageAllocatePlanes: ") + avifResultToString(res);
            return false;
        }
    }

    // CICP metadata. The RGB handed to libavif is already in the target space,
    // so these values also drive libavif's own RGB -> YUV conversion.
    const int primaries = settings.cicp_primaries.value_or(
        color.target_bt2020 ? AVIF_COLOR_PRIMARIES_BT2020 : AVIF_COLOR_PRIMARIES_BT709);
    const int transfer = settings.cicp_transfer.value_or(
        color.source == SourceEncoding::HdrPqBt2020 ? AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084
                                                    : AVIF_TRANSFER_CHARACTERISTICS_SRGB);
    const int matrix = settings.cicp_matrix.value_or(
        color.target_bt2020 ? AVIF_MATRIX_COEFFICIENTS_BT2020_NCL : AVIF_MATRIX_COEFFICIENTS_BT709);

    impl->image->colorPrimaries = static_cast<avifColorPrimaries>(primaries);
    impl->image->transferCharacteristics = static_cast<avifTransferCharacteristics>(transfer);
    impl->image->matrixCoefficients = static_cast<avifMatrixCoefficients>(matrix);
    impl->image->yuvRange = AVIF_RANGE_FULL;

    if (settings.clli)
    {
        impl->image->clli.maxCLL = settings.clli->first;
        impl->image->clli.maxPALL = settings.clli->second;
    }

    impl->encoder = avifEncoderCreate();
    if (!impl->encoder)
    {
        error = "avifEncoderCreate failed";
        return false;
    }

    impl->encoder->quality = kQuality;
    impl->encoder->qualityAlpha = kQuality;
    impl->encoder->speed = kSpeed;

    impl_ = std::move(impl);
    return true;
}

bool AvifWriter::submit(std::string &error)
{
    if (impl_->rgb.size() < static_cast<size_t>(impl_->width) * impl_->height * 3u)
    {
        error = "captured frame has the wrong size";
        return false;
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, impl_->image);
    rgb.depth = impl_->rgb_depth;
    rgb.format = AVIF_RGB_FORMAT_RGB;
    rgb.pixels = reinterpret_cast<uint8_t *>(impl_->rgb.data());
    rgb.rowBytes = impl_->width * 3u * kRgbBytesPerSample;
    // libavif performs the RGB -> YUV conversion (including the chroma
    // downsampling) for the format the image was created with.
    rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_AUTOMATIC;

    avifResult res = avifImageRGBToYUV(impl_->image, &rgb);
    if (res != AVIF_RESULT_OK)
    {
        error = std::string("avifImageRGBToYUV: ") + avifResultToString(res);
        return false;
    }

    res = avifEncoderAddImage(impl_->encoder, impl_->image, 1, AVIF_ADD_IMAGE_FLAG_SINGLE);
    if (res != AVIF_RESULT_OK)
    {
        error = std::string("avifEncoderAddImage: ") + avifResultToString(res);
        return false;
    }

    impl_->added = true;
    return true;
}

bool AvifWriter::add_frame(const float *rgba, const ColorTransformConfig &color, std::string &error)
{
    if (!is_open())
    {
        error = "AVIF encoder is not open";
        return false;
    }

    if (!transform_rgba32f_to_rgb10(rgba, impl_->width, impl_->height, color, impl_->rgb))
    {
        error = "failed to convert the captured frame to RGB";
        return false;
    }
    impl_->rgb_depth = kDepth;

    return submit(error);
}

bool AvifWriter::add_frame_rgb16(std::vector<uint16_t> &&rgb, std::string &error)
{
    if (!is_open())
    {
        error = "AVIF encoder is not open";
        return false;
    }

    if (rgb.size() != static_cast<size_t>(impl_->width) * impl_->height * 3u)
    {
        error = "GPU frame has the wrong size";
        return false;
    }

    impl_->rgb = std::move(rgb);
    impl_->rgb_depth = 16;

    return submit(error);
}

bool AvifWriter::finish(std::string &error)
{
    if (!is_open())
        return true;

    if (!impl_->added)
    {
        error = "no frames were encoded";
        return false;
    }

    avifRWData output = AVIF_DATA_EMPTY;
    const avifResult res = avifEncoderFinish(impl_->encoder, &output);
    if (res != AVIF_RESULT_OK)
    {
        error = std::string("avifEncoderFinish: ") + avifResultToString(res);
        avifRWDataFree(&output);
        return false;
    }

    bool ok = true;
    {
        std::ofstream file(impl_->path, std::ios::binary);
        if (!file)
        {
            error = "cannot open " + impl_->path + " for writing";
            ok = false;
        }
        else
        {
            file.write(reinterpret_cast<const char *>(output.data),
                       static_cast<std::streamsize>(output.size));
            if (!file)
            {
                error = "failed while writing " + impl_->path;
                ok = false;
            }
        }
    }

    avifRWDataFree(&output);
    impl_.reset();
    return ok;
}

} // namespace kmshot
