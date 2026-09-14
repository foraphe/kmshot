#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <utility>

#include "encoder.hpp"

namespace kmshot
{

enum class PqInput
{
    Auto,   // detect from XDG_CURRENT_DESKTOP
    Gamma22, // compositor hands out gamma 2.2 encoded values
    Pq,      // values are already PQ encoded
};

struct Options
{
    std::string card_path{"/dev/dri/card0"};
    std::string out_path{"frames.rgba64le"};
    bool out_explicit{false}; // --out was given
    int frames{120};
    int fps{30};
    int monitor{0};
    bool dmabuf_sync{false};
    bool write_to_stdout{false};
    bool use_slurp{false};
    double slurp_scale_x{1.0};
    double slurp_scale_y{1.0};
    bool pp_y4m{false};
    float pp_max_nits{0.0f}; // 0 means "auto: take the EDID maximum luminance"
    bool pp_max_nits_explicit{false};
    bool sdr_linear_12bpc{false};

    // --- AVIF output ---
    std::string avif_out; // --avif-out PATH
    AvifSettings avif;    // --avif-yuv / --avif-cicp / --avif-clli

    // --- colour handling ---
    std::string edid_path;                     // --edid PATH
    bool use_edid{true};                       // --no-edid
    std::string display_gamut;                 // --display-gamut NAME
    std::optional<std::array<double, 6>> display_primaries; // --display-primaries
    std::optional<std::pair<double, double>> display_white; // --display-white
    bool display_gamma_explicit{false};
    double display_gamma{2.2};                 // --display-gamma
    std::optional<std::array<double, 9>> color_matrix; // --color-matrix
    bool sdr_target_bt2020{true};              // --sdr-target bt709|bt2020
    std::optional<int> colorspace_override;    // --colorspace N
    PqInput pq_input{PqInput::Auto};           // --pq-input
    bool force_cpu_color{false};               // --cpu-color

    bool show_help{false};
    bool list_gamuts{false};
    bool print_edid{false};
};

enum class ParseStatus
{
    Ok,
    ExitSuccess, // --help / --list-gamuts handled; exit with code 0
    Error,
};

ParseStatus parse_options(int argc, char **argv, Options &opts, std::string &error);
void print_usage(std::ostream &os, const char *argv0);

} // namespace kmshot
