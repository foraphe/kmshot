# KMShot
KMShot is an experimental screenshot tool for Linux, written in C++. It reads from the DRM subsystem similar to how [Sunshine](https://github.com/LizardByte/Sunshine)'s `kmsgrab` capture works.
This tool can be used to capture wide color gamut screenshots or HDR content in higher-than-8-bit color depths, tested on KDE Plasma, Gnome, and Hyprland.

### Building
KMShot uses CMake as its build system. To build the project, headers for DRM, GBM, EGL, GLES2, and LittleCMS 2 need to be installed. You can build it using the following commands:
```bash
cmake -S . -B build
cmake --build build
```
This will generate the executable in the `build` directory.

The build also produces a few self-check executables. Run them with:
```bash
ctest --test-dir build --output-on-failure
```
The checks cover the EDID decoder (against a real panel, cross-checked with `edid-decode`), the colour matrix computation (LittleCMS2 against an independent primaries + Bradford reference implementation), the RGBA -> YUV conversion, and the slurp region geometry. They can be disabled with `-DKMSHOT_BUILD_TESTS=OFF`.

### Project layout
| Path | Contents |
|------|----------|
| `src/main.cpp` | CLI entry point and wiring |
| `src/cli.*` | Option parsing and `--help` |
| `src/drm_util.*` | DRM property/plane/framebuffer helpers, EDID blob read |
| `src/drm_debug.*` | Diagnostic dumps of connector/HDR/colorspace state |
| `src/dmabuf_gl.*` | DMA-BUF import into EGL/GLES and float readback |
| `src/capture.*` | Capture loop, slurp crop geometry, raw RGBA output |
| `src/color_math.*` | 3x3 matrix math, gamut presets, LittleCMS2 matrix extraction |
| `src/color_profile.*` | Resolves display primaries + target space into a transform |
| `src/color_transform.*` | RGBA -> YUV444P10 conversion and PQ helpers |
| `src/edid.*` | EDID base block + CTA-861 extension parser |
| `src/y4m.*` | YUV4MPEG2 container writer |
| `tests/`, `tools/` | Self-checks |

### Usage
To use this tool, the executable will need to either have `cap_sys_admin` or run as root.
Example usage (assuming `slurp` and `avifenc` are installed and in PATH):
SDR 10bpc YUV444 capture into AVIF (needs caution, see "Important Notes on Color Accuracy" below):
```bash
slurp |\
sudo ./kms_capture --card /dev/dri/card0 --frames 1 \
--stdout --pp-y4m --slurp --slurp-scale <display scaling> | \
avifenc -q 100 --stdin output_sdr.avif --depth 10 --yuv 444 --cicp 9/13/9 --range full
```

HDR capture (needs HDR to be enabled in the display settings, and the tool only works when DRM reports BT.2020):
```bash
slurp | \
sudo ./kms_capture --card /dev/dri/card0 --frames 1 \
--max-nits <max-nits> --stdout --pp-y4m --slurp --slurp-scale <display scaling> | \
avifenc -q 100 --stdin output_hdr.avif --depth 10 --yuv 444 --cicp 9/16/9 --clli <MaxCLL,MaxFALL> --range full
```
Being a screenshot tool, the captured frame would likely not be a well behaved, singular HDR image and instead might contain a mix of SDR (e.g. UI) and HDR content. In this case, it's currently recommended to set the MaxCLL/MaxFALL values according to the monitor's capabilities, so that the screenshot would look similar to the source content, when viewed on a 10000-nit reference display (or displays that have better capabilities than the monitor used for capture). However, this needs further testing, and no testing has been done on setting the values other than the monitor's capabilities.

Experimental 12bpc capture of linear RGB data into an 16bpc PNG (requires `ffmpeg`, and this WILL look wrong perceptually):
```bash
sudo ./kms_capture --card /dev/dri/card0 --frames 1 --sdr-linear-12bpc --stdout | \
ffmpeg -f rawvideo -video_size <width>x<height> -pix_fmt rgba64le -i - \
 -c:v png -pix_fmt rgba64be ./output.png -y
 ```

[slurp](https://github.com/emersion/slurp) can be used to select the capture area, and without `--slurp` this tool will capture the entire framebuffer. Since `slurp` outputs logical coordinates, the `--slurp-scale` option is used to scale the coordinates to the actual framebuffer size. This needs to match the scaling of the current workspace (display scaling on single-monitor systems).

### Colour handling
For SDR captures (DRM connector `Colorspace = 0`) the compositor blends in the display's native primaries, so the tool has to convert those values into a well defined colour space before writing them out. Instead of carrying a hardcoded matrix, the display primaries and white point are now read from the monitor's EDID and the conversion matrix is computed by LittleCMS 2 (relative colorimetric intent, i.e. Bradford chromatic adaptation between the two white points).

The display profile is resolved in this order:

1. `--color-matrix` — an explicit display-linear-RGB -> target-linear-RGB 3x3 matrix
2. `--display-primaries` / `--display-white` — explicit chromaticities
3. `--display-gamut NAME` — a built-in preset (see `--list-gamuts`)
4. the connector's EDID (`--edid FILE` overrides the source)
5. a built-in fallback (`--no-edid`, or when no EDID is available)

`--sdr-target bt709|bt2020` picks the target space (default `bt2020`). The target transfer function is sRGB. For HDR captures (`Colorspace = 9`) the content is already BT.2020/PQ, so no gamut conversion is applied, the YUV matrix is always BT.2020, and only the PQ handling described below is used. Any other `Colorspace` value is passed through unchanged (no transfer or gamut conversion) and encoded with the matrix selected by `--sdr-target`; the tool logs this explicitly so it is visible in the output.

Inspect what was resolved without taking a screenshot:
```bash
sudo ./kms_capture --print-edid
./kms_capture --print-edid --edid /usr/lib/firmware/edid/NE160QDM-NM7.edid.bin   # offline
./kms_capture --list-gamuts
```

> **Note on the previously hardcoded values.** The old `color_transform.cpp` hardcoded R (0.6874, 0.3104), G (0.2378, 0.7271), B (0.1427, 0.0543), W (0.3121, 0.3299). Those are *not* the values in the panel's firmware EDID, which reports R (0.6797, 0.3174), G (0.2422, 0.7168), B (0.1416, 0.0527), W (0.3184, 0.3340) (raw 10-bit codes 696/325, 248/734, 145/54, 326/342). The resulting Rec.709 matrices differ by up to about 0.015 in the coefficients. The datasheet values are kept as the `ne160qdm-nm7` preset and as the built-in fallback, so `--no-edid` reproduces the old behaviour exactly, while the default now follows the EDID.

#### Manual colour space or matrix
```bash
# Force the display primaries / white point (CIE 1931 xy)
./kms_capture --display-primaries 0.6797,0.3174,0.2422,0.7168,0.1416,0.0527 \
              --display-white 0.3184,0.3340

# Use a built-in gamut instead
./kms_capture --display-gamut display-p3 --sdr-target bt709

# Supply the linear RGB conversion matrix directly (row-major, 9 values).
# The example below is the NE160QDM-NM7 EDID -> Rec.709 matrix, so pair it with
# --sdr-target bt709 to keep the output matrix consistent.
./kms_capture --sdr-target bt709 \
              --color-matrix 1.3667,-0.3346,-0.0320,-0.0500,1.0571,-0.0070,-0.0187,-0.0846,1.1033

# Override the compositor's transfer/colourspace interpretation
./kms_capture --display-gamma 2.2 --colorspace 0 --pq-input auto
```

Available gamut presets: `srgb` (aliases `bt709`, `rec709`), `bt2020` (alias `rec2020`), `display-p3`, `dci-p3`, `adobe-rgb`, `ne160qdm-nm7`.

#### HDR PQ handling
For `Colorspace = 9`, KDE hands out gamma 2.2 encoded values that have to be converted back to PQ, while Gnome and Hyprland hand out values that are already PQ encoded. `--pq-input auto` (the default) keeps the previous behaviour of detecting this from `XDG_CURRENT_DESKTOP`; `gamma22` and `pq` force it. The PQ reference luminance (`--max-nits`) now defaults to the EDID's "desired content max luminance" and falls back to 1261 cd/m² when the EDID does not provide one.

### Options
```
Capture
  --card PATH             DRM card node (default /dev/dri/card0)
  --monitor N             Index of the connected CRTC to capture (default 0)
  --frames N              Number of frames to capture (default 120)
  --fps N                 Frame pacing used while capturing (default 30)
  --out PATH              Output file (default frames.rgba64le)
  --stdout                Write capture data to stdout
  --dmabuf-sync           Issue DMA_BUF_IOCTL_SYNC around readback
  --slurp                 Read an 'x,y wxh' region from stdin (slurp output)
  --slurp-scale S|SX,SY   Scale slurp's logical coordinates to framebuffer pixels

Output format
  --pp-y4m                Write full-range 10-bit YUV444 (Y4M) instead of RGBA64
  --sdr-linear-12bpc      Raw path only: decode gamma 2.2 and store 12-bit MSB-aligned
  --max-nits N            HDR PQ scaling reference in cd/m^2 (default: EDID max luminance)

Colour handling
  --edid PATH             Read the display EDID from a file instead of the connector
  --no-edid               Ignore EDID; fall back to --display-* or built-in values
  --display-gamut NAME    Force display primaries (see --list-gamuts)
  --display-primaries R   Six values rx,ry,gx,gy,bx,by overriding the primaries
  --display-white X,Y     White point overriding the EDID white point
  --display-gamma G       Decode exponent for native SDR values (default 2.2)
  --color-matrix M        Nine row-major values: display linear RGB -> target linear RGB
  --sdr-target bt709|bt2020
                          Target space for SDR captures (default bt2020)
  --colorspace N          Override the connector Colorspace property value
  --pq-input auto|gamma22|pq
                          How HDR (Colorspace 9) pixels are encoded (default auto)

Misc
  --list-gamuts           List the built-in gamut presets and exit
  --print-edid            Print the parsed EDID and exit (offline when --edid is given)
  -h, --help              Show this help and exit
```

### Important Notes on Color Accuracy
**TL;DR: Color accuracy is bad in SDR mode, but HDR should be fine**

KDE Plasma blends to the monitor's native profile primaries when DRM reports Colorspace = 0 (Default), but uses (likely) a pure gamma 2.2 transfer function. It's hard to map these colors back into well defined color spaces since profiles might contain non-linear LUTs that are not easily invertible. The tool therefore derives the primaries from the monitor's EDID and computes a 3x3 matrix from the monitor's colour space to Rec.709/Rec.2020 with sRGB transfer (see `src/color_profile.cpp` and `src/color_math.cpp`). The EDID primaries describe the panel as shipped, not a calibrated measurement, so this is an approximation; use `--display-primaries` / `--display-white` with a measured profile if you have one, or `--color-matrix` with a matrix from a calibration tool. In testing with KDE Plasma set to "prefer color accuracy" (16bpc max) using grayscale and RGB ramps from 0 to 1023, up to 10% error was observed in the green channel on a calibrated NE160QDM-NM7 monitor panel, in "prefer efficiency" accuracy is better but still not pixel-perfect. In HDR mode, where KDE agreed on using BT.2020 and PQ, color accuracy is better since the colorspace is much more well defined, and less transformation needs to be done on the pixel values.

### Examples and sanity checks
Example of a synthetic luminance block pattern, its screenshot on a 1261-nit monitor, and the screenshot histogram:
|Synthetic pattern|Monitor screenshot|
|:---------------:|:----------------:|
|![Synthetic pattern](assets/bars_ramp.avif)|![Monitor screenshot](assets/bars_ramp_screenshot.avif)|

Histogram:
![Histogram](assets/bars_ramp_histogram.png)

The synthetic image is tagged with MaxCLL/MaxFALL of 10,000/1125 nits. The screenshot is tagged with MaxCLL/MaxFALL of 1261/604 nits (the EDID MaxCLL and preferred MaxFALL values of the source monitor), so that on HDR displays that have better capabilities, the screenshot should look the same as when it was taken, but won't be the same as the original image viewed on another monitor. On SDR displays and HDR displays that report lower peak luminance levels, the scrennshot should look similar to the original image.
To verify if the tool works properly on a given system, it's possible to take a screenshot of an arbitrary HDR image (e.g., this test pattern) and compare the screenshot side by side with the original image on a viewer that supported HDR (like Chromium), and they should look the same.

`--print-edid` and `--list-gamuts` are handy for confirming which colour pipeline the tool will use on a given machine without capturing anything.

### Notes
###### (and also personal thoughts and rants)
- This tool is written with extensive use of LLMs, so expect some weird code here and there.
- It seems either I'm making mistakes, or that Gwenview is not very well color managed at this point. I'm treating the display results on Chromium as HDR ground truths since it seems to be  more accurate (though it also introduces its own color shifts in SDR).
- VRAM framebuffers are usually not directly in a decodable format, and this tool used DMA-BUF to map the images into a GL context and read the pixels back. The implementation is not very optimized and can incur significant CPU overhead compared to the Sunshine kmsgrab implementation.
- I have little knowledge about color science,  if you see any mistakes or have suggestions on improving the project, please let me know!
- However, this is a project coming out of a sudden burst of curiosity, and it might not be maintained in the long run (also see my other abandoned projects). Hopefully, soon we will have proper protocols and APIs for such functionalities in Wayland.
- Technically, this tool is built with support for streaming frames in mind, but frame synchronization is not implemented and the current implementation is too CPU-heavy for real-time capture, so it's discouraged to run it with `--frames` set to more than 1 for now. The tool also doesn't support capturing from multiple planes, so OSDs and cursors that are rendered on separate planes won't be captured.
