#include "cli.hpp"

#include "color_math.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace kmshot
{

namespace
{

bool parse_int(const std::string &text, int &out)
{
    try
    {
        size_t consumed = 0;
        const int v = std::stoi(text, &consumed);
        if (consumed != text.size())
            return false;
        out = v;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool parse_double(const std::string &text, double &out)
{
    try
    {
        size_t consumed = 0;
        const double v = std::stod(text, &consumed);
        // Reject inf/nan so downstream arithmetic stays well defined.
        if (consumed != text.size() || !std::isfinite(v))
            return false;
        out = v;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::string to_lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Parses "A/B/C" into three integers.
bool parse_slash_triple(const std::string &text, int &a, int &b, int &c)
{
    const size_t first = text.find('/');
    if (first == std::string::npos)
        return false;
    const size_t second = text.find('/', first + 1);
    if (second == std::string::npos)
        return false;

    return parse_int(text.substr(0, first), a) &&
           parse_int(text.substr(first + 1, second - first - 1), b) &&
           parse_int(text.substr(second + 1), c);
}

// Parses "A,B" into two integers.
bool parse_comma_pair(const std::string &text, int &a, int &b)
{
    const size_t comma = text.find(',');
    if (comma == std::string::npos)
        return false;
    return parse_int(text.substr(0, comma), a) && parse_int(text.substr(comma + 1), b);
}

} // namespace

void print_usage(std::ostream &os, const char *argv0)
{
    os << "Usage: " << argv0 << " [options]\n"
       << "\n"
       << "Capture\n"
       << "  --card PATH             DRM card node (default /dev/dri/card0)\n"
       << "  --monitor N             Index of the connected CRTC to capture (default 0)\n"
       << "  --frames N              Number of frames to capture (default 120)\n"
       << "  --fps N                 Frame pacing used while capturing (default 30)\n"
       << "  --out PATH              Output file (default frames.rgba64le)\n"
       << "  --stdout                Write capture data to stdout\n"
       << "  --dmabuf-sync           Issue DMA_BUF_IOCTL_SYNC around readback\n"
       << "  --slurp                 Read an 'x,y wxh' region from stdin (slurp output)\n"
       << "  --slurp-scale S|SX,SY   Scale slurp's logical coordinates to framebuffer pixels\n"
       << "\n"
       << "Output format\n"
       << "  --pp-y4m                Write full-range 16-bit YUV444 (Y4M) instead of RGBA64\n"
       << "  --sdr-linear-12bpc      Raw path only: decode --display-gamma and store 12-bit MSB-aligned\n"
       << "  --max-nits N            HDR PQ scaling reference in cd/m^2 (default: EDID max luminance)\n"
       << "\n"
       << "AVIF output\n"
       << "  --avif-out PATH         Encode one still frame to an AVIF file instead of raw RGBA/Y4M\n"
       << "  --avif-yuv 444|422|420  Chroma subsampling; libavif does the downsampling (default 444)\n"
       << "  --avif-cicp P/T/M       Override the CICP primaries/transfer/matrix metadata\n"
       << "  --avif-clli MAXCLL,MAXFALL\n"
       << "                          Content light level in cd/m^2 (default: EDID values for HDR)\n"
       << "\n"
       << "Color handling\n"
       << "  --edid PATH             Read the display EDID from a file instead of the connector\n"
       << "  --no-edid               Ignore EDID; fall back to --display-* or built-in values\n"
       << "  --display-gamut NAME    Force display primaries (see --list-gamuts)\n"
       << "  --display-primaries R   Six values rx,ry,gx,gy,bx,by overriding the primaries\n"
       << "  --display-white X,Y     White point overriding the EDID white point\n"
       << "  --display-gamma G       Decode exponent for native SDR values (default 2.2)\n"
       << "  --color-matrix M        Nine row-major values: display linear RGB -> target linear RGB\n"
       << "  --sdr-target bt709|bt2020\n"
       << "                          Target space for SDR captures (default bt2020)\n"
       << "  --colorspace N          Override the connector Colorspace property value\n"
       << "  --pq-input auto|gamma22|pq\n"
       << "                          How HDR (Colorspace 9) pixels are encoded (default auto)\n"
       << "  --cpu-color             Force the CPU colour transform instead of the GL shader\n"
       << "\n"
       << "Misc\n"
       << "  --list-gamuts           List the built-in gamut presets and exit\n"
       << "  --print-edid            Print the parsed EDID and exit (offline when --edid is given)\n"
       << "  -h, --help              Show this help and exit\n";
}

ParseStatus parse_options(int argc, char **argv, Options &opts, std::string &error)
{
    auto need_value = [&](int &i, const std::string &flag, std::string &out) -> bool
    {
        if (i + 1 >= argc)
        {
            error = flag + " requires a value";
            return false;
        }
        out = argv[++i];
        return true;
    };

    bool avif_tuning_given = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        std::string value;

        if (a == "-h" || a == "--help")
        {
            opts.show_help = true;
            return ParseStatus::ExitSuccess;
        }
        if (a == "--list-gamuts")
        {
            opts.list_gamuts = true;
            return ParseStatus::ExitSuccess;
        }
        if (a == "--print-edid")
        {
            opts.print_edid = true;
        }
        else if (a == "--card")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            opts.card_path = value;
        }
        else if (a == "--out")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            opts.out_path = value;
            opts.out_explicit = true;
        }
        else if (a == "--frames")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            if (!parse_int(value, opts.frames) || opts.frames <= 0)
            {
                error = "--frames expects a positive integer";
                return ParseStatus::Error;
            }
        }
        else if (a == "--fps")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            if (!parse_int(value, opts.fps) || opts.fps <= 0)
            {
                error = "--fps expects a positive integer";
                return ParseStatus::Error;
            }
        }
        else if (a == "--monitor")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            if (!parse_int(value, opts.monitor) || opts.monitor < 0)
            {
                error = "--monitor expects a non-negative integer";
                return ParseStatus::Error;
            }
        }
        else if (a == "--dmabuf-sync")
        {
            opts.dmabuf_sync = true;
        }
        else if (a == "--stdout")
        {
            opts.write_to_stdout = true;
        }
        else if (a == "--slurp")
        {
            opts.use_slurp = true;
        }
        else if (a == "--slurp-scale")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            const auto comma = value.find(',');
            if (comma == std::string::npos)
            {
                if (!parse_double(value, opts.slurp_scale_x) || opts.slurp_scale_x <= 0.0)
                {
                    error = "--slurp-scale expects a positive number or SX,SY";
                    return ParseStatus::Error;
                }
                opts.slurp_scale_y = opts.slurp_scale_x;
            }
            else
            {
                if (!parse_double(value.substr(0, comma), opts.slurp_scale_x) ||
                    !parse_double(value.substr(comma + 1), opts.slurp_scale_y) ||
                    opts.slurp_scale_x <= 0.0 || opts.slurp_scale_y <= 0.0)
                {
                    error = "--slurp-scale expects a positive number or SX,SY";
                    return ParseStatus::Error;
                }
            }
        }
        else if (a == "--pp-y4m")
        {
            opts.pp_y4m = true;
        }
        else if (a == "--max-nits")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            double max_nits = 0.0;
            if (!parse_double(value, max_nits) || !(max_nits > 0.0))
            {
                error = "--max-nits expects a positive number";
                return ParseStatus::Error;
            }
            opts.pp_max_nits = static_cast<float>(max_nits);
            opts.pp_max_nits_explicit = true;
        }
        else if (a == "--sdr-linear-12bpc")
        {
            opts.sdr_linear_12bpc = true;
        }
        else if (a == "--edid")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            opts.edid_path = value;
        }
        else if (a == "--no-edid")
        {
            opts.use_edid = false;
        }
        else if (a == "--display-gamut")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            if (!find_gamut_preset(value))
            {
                error = "unknown gamut '" + value + "' (known: " + gamut_preset_names() + ")";
                return ParseStatus::Error;
            }
            opts.display_gamut = value;
        }
        else if (a == "--display-primaries")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            auto parsed = parse_six_values(value);
            if (!parsed)
            {
                error = "--display-primaries expects six values: rx,ry,gx,gy,bx,by";
                return ParseStatus::Error;
            }
            opts.display_primaries = parsed;
        }
        else if (a == "--display-white")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            auto parsed = parse_two_values(value);
            if (!parsed)
            {
                error = "--display-white expects two values: wx,wy";
                return ParseStatus::Error;
            }
            opts.display_white = parsed;
        }
        else if (a == "--display-gamma")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            if (!parse_double(value, opts.display_gamma) || !(opts.display_gamma > 0.0) ||
                !(opts.display_gamma < 10.0))
            {
                error = "--display-gamma expects a number between 0 and 10";
                return ParseStatus::Error;
            }
            opts.display_gamma_explicit = true;
        }
        else if (a == "--color-matrix")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            auto parsed = parse_nine_values(value);
            if (!parsed)
            {
                error = "--color-matrix expects nine row-major values";
                return ParseStatus::Error;
            }
            opts.color_matrix = parsed;
        }
        else if (a == "--sdr-target")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            const std::string v = to_lower(value);
            if (v == "bt2020" || v == "rec2020")
                opts.sdr_target_bt2020 = true;
            else if (v == "bt709" || v == "rec709" || v == "srgb")
                opts.sdr_target_bt2020 = false;
            else
            {
                error = "--sdr-target expects bt709 or bt2020";
                return ParseStatus::Error;
            }
        }
        else if (a == "--colorspace")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            int cs = 0;
            if (!parse_int(value, cs) || cs < 0 || cs > 255)
            {
                error = "--colorspace expects an integer between 0 and 255";
                return ParseStatus::Error;
            }
            opts.colorspace_override = cs;
        }
        else if (a == "--pq-input")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            const std::string v = to_lower(value);
            if (v == "auto")
                opts.pq_input = PqInput::Auto;
            else if (v == "gamma22" || v == "gamma2.2")
                opts.pq_input = PqInput::Gamma22;
            else if (v == "pq")
                opts.pq_input = PqInput::Pq;
            else
            {
                error = "--pq-input expects auto, gamma22 or pq";
                return ParseStatus::Error;
            }
        }
        else if (a == "--cpu-color")
        {
            opts.force_cpu_color = true;
        }
        else if (a == "--avif-out")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            opts.avif_out = value;
        }
        else if (a == "--avif-yuv")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            const std::string v = to_lower(value);
            if (v != "444" && v != "422" && v != "420")
            {
                error = "--avif-yuv expects 444, 422 or 420";
                return ParseStatus::Error;
            }
            opts.avif.subsampling = v;
            avif_tuning_given = true;
        }
        else if (a == "--avif-cicp")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            int primaries = 0;
            int transfer = 0;
            int matrix = 0;
            if (!parse_slash_triple(value, primaries, transfer, matrix) ||
                primaries < 0 || primaries > 255 || transfer < 0 || transfer > 255 ||
                matrix < 0 || matrix > 255)
            {
                error = "--avif-cicp expects three integers as P/T/M";
                return ParseStatus::Error;
            }
            opts.avif.cicp_primaries = primaries;
            opts.avif.cicp_transfer = transfer;
            opts.avif.cicp_matrix = matrix;
            avif_tuning_given = true;
        }
        else if (a == "--avif-clli")
        {
            if (!need_value(i, a, value)) return ParseStatus::Error;
            int max_cll = 0;
            int max_pall = 0;
            if (!parse_comma_pair(value, max_cll, max_pall) ||
                max_cll < 0 || max_cll > 65535 || max_pall < 0 || max_pall > 65535)
            {
                error = "--avif-clli expects MAXCLL,MAXFALL between 0 and 65535";
                return ParseStatus::Error;
            }
            opts.avif.clli = std::make_pair(static_cast<uint16_t>(max_cll),
                                            static_cast<uint16_t>(max_pall));
            avif_tuning_given = true;
        }
        else
        {
            error = "unknown argument '" + a + "'";
            return ParseStatus::Error;
        }
    }

    const bool avif_mode = !opts.avif_out.empty();

    if (avif_mode && opts.pp_y4m)
    {
        error = "--avif-out cannot be combined with --pp-y4m";
        return ParseStatus::Error;
    }

    if (avif_mode && opts.write_to_stdout)
    {
        error = "--avif-out cannot be combined with --stdout";
        return ParseStatus::Error;
    }

    if (avif_mode && opts.sdr_linear_12bpc)
    {
        error = "--sdr-linear-12bpc is only for the raw RGBA output path (without --avif-out)";
        return ParseStatus::Error;
    }

    if (!avif_mode && avif_tuning_given)
    {
        error = "--avif-yuv/--avif-cicp/--avif-clli require --avif-out";
        return ParseStatus::Error;
    }

    if (avif_mode && opts.frames != 1)
    {
        // AVIF output is a single still image. Multi-frame encoding is not
        // supported: libavif crashes in avifEncoderFinish when an image
        // sequence carries content light level metadata, which HDR captures
        // always set. Clamp instead of failing so the common
        // "--avif-out shot.avif" invocation works with the default --frames.
        std::cerr << "note: --avif-out always writes a single frame (ignoring --frames "
                  << opts.frames << ")\n";
        opts.frames = 1;
    }

    if (opts.pp_y4m && opts.sdr_linear_12bpc)
    {
        error = "--sdr-linear-12bpc is only for the raw RGBA output path (without --pp-y4m)";
        return ParseStatus::Error;
    }

    if (opts.print_edid && !opts.use_edid)
    {
        error = "--print-edid cannot be combined with --no-edid";
        return ParseStatus::Error;
    }

    // Give the default output file an extension that matches the format, so a
    // forgotten --stdout does not silently write Y4M data into "*.rgba64le".
    if (!opts.out_explicit && opts.pp_y4m)
        opts.out_path = "frames.y4m";

    return ParseStatus::Ok;
}

} // namespace kmshot
