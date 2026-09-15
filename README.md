# KMShot
KMShot is an experimental screenshot tool for Linux, written in C++. It reads from the DRM subsystem similar to how [Sunshine](https://github.com/LizardByte/Sunshine)'s `kmsgrab` capture works.
This tool can be used to capture wide color gamut screenshots or HDR content in higher-than-8-bit color depths, tested on KDE Plasma, Gnome, and Hyprland. Captures can be written as raw RGBA, as Y4M, or encoded straight to AVIF in-process with [libavif](https://github.com/AOMediaCodec/libavif).
### Notes and caveats
- The tool doesn't support capturing from multiple planes, so OSDs and cursors that are rendered on separate planes won't be captured.
- The tool is experimental and may not work as expected in all scenarios and/or on all compositors. Multi-monitor support has not been tested, and the tool is only tested on AMD and Intel GPUs as of now.
### Building
KMShot uses CMake as its build system. To build the project, headers for DRM, GBM, EGL, GLES2, LittleCMS 2, and libavif need to be installed. You can build it using the following commands:
```bash
cmake -S . -B build
cmake --build build
```
This will generate the executable in the `build` directory.

The build also produces a few self-check executables. Run them with:
```bash
ctest --test-dir build --output-on-failure
```
The checks cover the EDID decoder (against a real panel, cross-checked with `edid-decode`), the colour matrix computation (LittleCMS2 against an independent primaries + Bradford reference implementation), the RGBA -> YUV/RGB conversion, the slurp region geometry, and the AVIF encoder (the output is decoded back and its CICP, subsampling and content-light-level metadata are verified). They can be disabled with `-DKMSHOT_BUILD_TESTS=OFF`.

### Project layout
| Path | Contents |
|------|----------|
| `src/main.cpp` | CLI entry point and wiring |
| `src/cli.*` | Option parsing and `--help` |
| `src/drm_util.*` | DRM property/plane/framebuffer helpers, EDID blob read |
| `src/drm_debug.*` | Diagnostic dumps of connector/HDR/colorspace state |
| `src/dmabuf_gl.*` | DMA-BUF import into EGL/GLES, GPU colour transform, planar/interleaved readback |
| `src/capture.*` | Capture loop, slurp crop geometry, raw RGBA output |
| `src/color_math.*` | 3x3 matrix math, gamut presets, LittleCMS2 matrix extraction |
| `src/color_profile.*` | Resolves display primaries + target space into a transform |
| `src/color_transform.*` | RGBA -> YUV444P16 conversion and PQ helpers |
| `src/edid.*` | EDID base block + CTA-861 extension parser |
| `src/encoder.*` | AVIF encoding via libavif (single still image) |
| `src/y4m.*` | YUV4MPEG2 container writer |
| `tests/`, `tools/` | Self-checks |

### Usage
To use this tool, the executable will need to either have `cap_sys_admin` or be run as root. In the future there will be a separated helper binary that prevents the main binary from needing excessive permissions.

Example usage:
SDR capture encoded to a 10-bit AVIF by the tool itself (needs caution, see "Important Notes on Color Accuracy" below):
```bash
sudo ./kms_capture --card /dev/dri/card0 --frames 1 \
  --avif-out output_sdr.avif --avif-yuv 420
```

HDR capture (needs HDR to be enabled in the display settings, and the tool only works when DRM reports BT.2020):
```bash
sudo ./kms_capture --card /dev/dri/card0 --frames 1 \
  --avif-out output_hdr.avif --avif-yuv 420
```

MaxCLL/MaxPALL default to the monitor's EDID values; pass `--avif-clli <MaxCLL,MaxFALL>` to override them.

[slurp](https://github.com/emersion/slurp) can be used to select the capture area when used with compatible Wayland compositors. Since `slurp` will output logical coordinates after display scaling, the `--slurp-scale` option can be used to scale the coordinates to framebuffer pixels. For example, on a display with 1.25x scaling, the following command will capture a single frame of the selected area and encode it to AVIF: 
```bash
slurp | sudo ./kms_capture --card /dev/dri/card0 --frames 1 \
  --avif-out output_hdr.avif --avif-yuv 420 \
  --slurp --slurp-scale 1.25
```

Being a screenshot tool, the captured frame would likely not be a well behaved, singular HDR image and instead might contain a mix of SDR (e.g. UI) and HDR content. In this case, it's currently recommended to set the MaxCLL/MaxFALL values according to the monitor's capabilities, so that the screenshot would look similar to the source content, when viewed on a 10000-nit reference display (or displays that have better capabilities than the monitor used for capture). However, this needs further testing, and no testing has been done on setting the values other than the monitor's capabilities.

> **Y4M note:** `--pp-y4m` is still available for piping into other tools, but it writes 16-bit samples (`C444p16`) and `avifenc`'s Y4M reader only accepts 8, 10 or 12-bit input. Use `--avif-out` when you want an AVIF; `ffmpeg` reads the 16-bit Y4M fine. See "HDR10 video capture (Y4M → ffmpeg)" below for a complete ffmpeg command line, including the colour signalling that ffmpeg needs to be given explicitly.

Experimental 12bpc capture of linear RGB data into an 16bpc PNG (requires `ffmpeg`, and this WILL look wrong perceptually). The gamma used to linearize the values follows `--display-gamma` (default 2.2):
```bash
sudo ./kms_capture --card /dev/dri/card0 --frames 1 --sdr-linear-12bpc --stdout | \
ffmpeg -f rawvideo -video_size <width>x<height> -pix_fmt rgba64le -i - \
 -c:v png -pix_fmt rgba64be ./output.png -y
 ```

### AVIF output
`--avif-out PATH` encodes the capture directly with [libavif](https://github.com/AOMediaCodec/libavif)

- **Depth:** 10-bit.
- **Chroma subsampling:** `--avif-yuv 444|422|420` (default `444`). The downsampling is done by libavif while it converts the target-space RGB image to YUV, so the colour conversion happens at full resolution and no chroma is discarded before it.
- **Colour metadata (CICP):** derived from the colour pipeline. The `nclx` (`colr`) box records the target primaries and matrix (BT.2020 for HDR captures, otherwise whatever `--sdr-target` selected), the transfer function (PQ for HDR, sRGB otherwise) and full range. `--avif-cicp P/T/M` overrides all three if a specific consumer needs something else.
- **Content light level:** for HDR captures MaxCLL/MaxPALL default to the monitor's EDID values; `--avif-clli MAXCLL,MAXFALL` overrides them. SDR captures get no `clli` box.

AVIF output is always a single still image (`ftyp` brand `avif`): `--avif-out` forces `--frames 1` and says so if a different count was requested. (libavif 1.4 crashes in `avifEncoderFinish` when an image sequence carries content light level metadata, which every HDR capture sets, so sequences are not offered.) `--avif-out` is mutually exclusive with `--stdout`, `--pp-y4m` and `--sdr-linear-12bpc`.

### HDR10 video capture (Y4M → ffmpeg)
`--pp-y4m` writes 16-bit 4:4:4 YUV (`C444p16`, full range) that `ffmpeg` can pipe straight into an HDR10 HEVC encode:

```bash
sudo sleep 1 && sudo ./kms_capture --card /dev/dri/card0 --frames 1200 --fps 60 --pp-y4m --stdout \
| ffmpeg -f yuv4mpegpipe -r 60 \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range pc -i pipe:0 \
    -vf "scale=out_range=tv:out_color_matrix=bt2020nc" \
    -c:v libx265 -preset veryfast -pix_fmt yuv420p10le \
    -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv \
    -x265-params "master-display='G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(12610000,50)':max-cll='1261,604':hdr10=1:hdr10-opt=1" \
    -bf 0 -crf 21 output.mkv
```

The capture itself can never exceed the PQ code for `--max-nits` (or the EDID peak if the option is unset): the shader clamps the source to `[0, 1]` and maps 1.0 to that reference, so the luma plane stops at the code for that luminance. If a player still reports a higher peak, those extra samples came from the encoder — `libx265` at `-crf 21 -preset veryfast` overshoots slightly on very bright high-contrast detail (a handful of pixels out of billions; `-preset medium` or a lower `-crf` helps reduce it).

### Colour handling
For SDR captures (DRM connector `Colorspace = 0`) the compositor blends in the display's native primaries, so the tool has to convert those values into a well defined colour space before writing them out. Instead of carrying a hardcoded matrix, the display primaries and white point are now read from the monitor's EDID and the conversion matrix is computed by LittleCMS 2 (relative colorimetric intent, i.e. Bradford chromatic adaptation between the two white points).

#### Where the conversion runs
For Y4M and AVIF output the colour management runs in a fragment shader while the DMA-BUF is still on the GPU. The shader does the source decode, the display→target matrix and the target transfer function in both cases; for Y4M it also applies the RGB→YUV matrix and the finished 16-bit YUV planes are read back, while for AVIF it stops at target RGB (still 16-bit) and lets libavif do the RGB→YUV conversion and the chroma downsampling. On a desktop PC with a 2560x1600 display and a dedicated graphics card, this is roughly an order of magnitude cheaper than doing it on the CPU, which is what makes real-time capture feasible.

The GPU path needs a GLES3 context with `GL_EXT_color_buffer_float` (the same requirement as the FP32 readback path); the colour shader is only built when that is available. If it is missing, if the shader fails to build, or if a frame fails at runtime, the capture automatically falls back to the CPU transform and says so on stderr; `--cpu-color` forces the CPU path. The CPU fallback evaluates the transfer functions through sqrt-indexed lookup tables (this is still much faster than doing raw `pow` operations). Raw RGBA output uses the CPU path, because it needs the untouched framebuffer values.

For Y4M the shader does not stop at an interleaved target: the fragment shader writes Y, U and V into three separate `R16` colour attachments (multi-render-target) and each plane is read back on its own with `glReadPixels(GL_RED, GL_UNSIGNED_SHORT)`. The samples arrive quantized *and* already planar, so no CPU-side de-interleave is needed at all. This can also yield ~5-10x better performance (~3 ms per 2560×1600 frame, versus ~17 ms for a float readback and ~91 ms for the full CPU transform).

Multi-render-target `R16` support is probed at runtime rather than assumed: at startup the reader renders known values into three `R16` targets, reads them back and compares, and logs `Planar YUV writeback: available|unavailable`. When it is unavailable — or when a frame fails in the planar path — the capture falls back to the interleaved `RGBA16` target (and, if the driver cannot render to `RGBA16` either, to the FP32 target; AVIF then uses the float RGB path). `KMSHOT_NO_PLANAR=1` disables the planar path explicitly, which is useful when A/B testing a driver.

#### Frame pacing and output threads
Frames are paced against an absolute deadline (`sleep_until(start + i / fps)`) rather than sleeping a full interval after each frame's work.

Y4M output is written by a separate thread, so a slow consumer — an encoder reading the pipe — cannot stall the capture loop; only a sustained backlog does. The writer owns a small pool of YUV buffer sets (3) that it hands to the capture loop and takes back after writing, and it takes an explicit sample count so a cropped frame can stay in a full-size buffer. This matters more than it sounds: profiling the capture loop showed that allocating and zero-filling those ~25 MB buffers once per frame, plus the `std::vector`-based de-interleave, was about half of the total CPU time. Reusing them and removing the de-interleave entirely (the planar writeback above) took the 2560×1600 capture rate from 34 fps to ~110 fps and then to ~320 fps, i.e. about 3 ms per frame with the capture loop using well under one core. Raw and AVIF output are still written inline.

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

> **Note on the previously hardcoded values.** The old `color_transform.cpp` hardcoded R (0.6874, 0.3104), G (0.2378, 0.7271), B (0.1427, 0.0543), W (0.3121, 0.3299). Those are *not* the values in the panel's firmware EDID, and are instead values measured using a colorimeter at 70 nits backlight. The values are kept as the `ne160qdm-nm7` preset and as the built-in fallback, so `--no-edid` reproduces the old behaviour exactly.

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
  --pp-y4m                Write full-range 16-bit YUV444 (Y4M) instead of RGBA64
  --sdr-linear-12bpc      Raw path only: decode --display-gamma and store 12-bit MSB-aligned
  --max-nits N            HDR PQ scaling reference in cd/m^2 (default: EDID max luminance)

AVIF output
  --avif-out PATH         Encode one still frame to an AVIF file instead of raw RGBA/Y4M
  --avif-yuv 444|422|420  Chroma subsampling; libavif does the downsampling (default 444)
  --avif-cicp P/T/M       Override the CICP primaries/transfer/matrix metadata
  --avif-clli MAXCLL,MAXFALL
                          Content light level in cd/m^2 (default: EDID values for HDR)

Color handling
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
  --cpu-color             Force the CPU colour transform instead of the GL shader

Misc
  --list-gamuts           List the built-in gamut presets and exit
  --print-edid            Print the parsed EDID and exit (offline when --edid is given)
  -h, --help              Show this help and exit

```

### Important Notes on Color Accuracy
**TL;DR: Color accuracy is bad in SDR mode, but HDR should be fine**

KDE Plasma blends to the monitor's native profile primaries when DRM reports Colorspace = 0 (Default), but uses a pure gamma 2.2 transfer function. It's hard to map these colors back into well defined color spaces since profiles might contain non-linear LUTs that are not easily invertible. The tool therefore derives the primaries from the monitor's EDID and computes a 3x3 matrix from the monitor's colour space to Rec.709/Rec.2020 with sRGB transfer (see `src/color_profile.cpp` and `src/color_math.cpp`). The EDID primaries describe the panel as shipped, not a calibrated measurement, so this is an approximation; use `--display-primaries` / `--display-white` with a measured profile if you have one, or `--color-matrix` with a matrix from a calibration tool. In testing with KDE Plasma set to "prefer color accuracy" (16bpc max) using grayscale and RGB ramps from 0 to 1023, up to 10% error was observed in the green channel on a calibrated NE160QDM-NM7 monitor panel, in "prefer efficiency" accuracy is better but still not pixel-perfect. In HDR mode, where KDE agreed on using BT.2020 and PQ, color accuracy is better since the colorspace is much more well defined, and less transformation needs to be done on the pixel values.

### Convert AVIF to JPEG
`avif_to_jpeg.py` converts an HDR AVIF into an [Ultra HDR](https://en.wikipedia.org/wiki/Ultra_HDR) JPEG (a JPEG base image plus a gain map), which Android and other compatible viewers render as HDR while JPEG readers not supporting Ultra HDR still shows the SDR base image.

Dependencies:

- `numpy`, `pillow` and `pillow-heif` (`pip install numpy pillow pillow-heif`). Without `pillow-heif` the HDR frame is decoded through Pillow at 8 bits, which costs shadow detail.
- `ffmpeg` built with `zscale` (libzimg).
- A Display P3 ICC profile (e.g. `/usr/share/color/icc/colord/Display P3.icc` from the `colord` or `icc-profiles-free` package, or any path passed to `--icc`). Without one the base image would be Display P3 data with no profile, which readers interpret as sRGB and render oversaturated.

```bash
python3 avif_to_jpeg.py -o output.jpg input.avif
```

The input has to be BT.2020 + PQ (what `kms_capture --avif-out` writes). The script reads the CICP metadata of the AVIF and refuses anything else unless `--assume-pq` is passed. What it does:

1. `ffmpeg` tone maps the HDR frame to Display P3 with the sRGB transfer function (`--tonemap`, default `mobius`; `--sdr-white`, default 203 nits = BT.2408 diffuse white).
2. The original AVIF is decoded back to PQ code values and converted to linear P3 nits, keeping 10/12-bit depth when `pillow-heif` is available.
3. `log2(HDR/SDR)` is evaluated per pixel on the P3 luminance and stored as an 8-bit gain map. The ratio is computed with the same `(value + Offset)` form the decoder applies, and against the decoded base JPEG rather than the pre-compression image, so the reconstruction stays correct in the deep shadows and matches what a decoder actually sees.
4. The `hdrgm` XMP is written into both the primary image and the gain map image (readers differ in which one they parse), plus an MPF segment pointing at the gain map.

Notes and limitations:

- The gain map is single channel, so one recovery value is applied to R, G and B. Heavily saturated highlights can drift slightly in chroma; in exchange the map works with every decoder.
- The gain map range is `[p0.01, max]` of the log2 ratio by default (`--gm-trim-low` / `--gm-trim-high`), i.e. the brightest highlights are kept instead of being clipped to a percentile.
- `HDRCapacityMin` is 0, so an SDR display shows the base image untouched; only displays with HDR headroom apply the map.
- The base image is encoded at quality 95 with 4:4:4 chroma by default (better for text and UI edges than the 4:2:0 default; `--sdr-subsampling` changes it). A 2560x1600 screenshot lands at roughly 1.8 MB: ~1.3 MB base + ~480 KB gain map.
- The script hasn't been very thoroughly tested yet, and in the future this feature may be integrated into the main program. If `libultrahdr` happens to be installed, its reference tool can inspect the result on Linux: `ultrahdr_app -m 1 -j output.jpg -P` prints the gain map metadata, and `ultrahdr_app -m 1 -j output.jpg -o 2 -O 5 -z out.raw` decodes it back to PQ.

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

### Notes and caveats
- This tool is written with extensive use of LLMs, so expect some weird code here and there.
- It seems either I'm making mistakes, or that Gwenview is not very well color managed at this point. I'm treating the display results on mpv as HDR ground truths since it seems to be more accurate.
- VRAM framebuffers are usually not directly in a decodable format, and this tool used DMA-BUF to map the images into a GL context and read the pixels back. The colour conversion now runs in a fragment shader and the readback is planar 16-bit, so the remaining CPU cost is little more than the `glReadPixels` transfer itself; the frame content still has to leave the GPU, which is the main difference from a zero-copy encoder feed such as Sunshine's kmsgrab.
- I have little knowledge about color science,  if you see any mistakes or have suggestions on improving the project, please let me know!
- However, this is a project coming out of a sudden burst of curiosity, and it might not be maintained in the long run (also see my other abandoned projects). Hopefully, soon we will have proper protocols and APIs for such functionalities in Wayland.
- The tool doesn't support capturing from multiple planes, so OSDs and cursors that are rendered on separate planes won't be captured.
