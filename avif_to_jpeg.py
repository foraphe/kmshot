#!/usr/bin/env python3
"""
AVIF (HDR10 / PQ / BT.2020) -> UltraHDR JPEG
ffmpeg 做色调映射, Python 算 gain map 并组装 UltraHDR。

依赖:
    pip install numpy pillow pillow-heif
    ffmpeg (需 zscale 支持, 即 libzimg)

流程:
    ffmpeg 把 HDR AVIF 色调映射成 Display P3 的 SDR 基础图 (sRGB 传递函数),
    Python 读回原始 AVIF 的 PQ 码值 -> 线性 P3 (nits), 与"基础图 JPEG 解码
    后的结果"相除得到 gain map, 再把 XMP 和 MPF 塞进 JPEG。

gain map 的数学与 libultrahdr 保持一致:
    hdr_linear = (sdr_linear + OffsetSDR) * 2^log2_recovery - OffsetHDR
    log2_recovery = GainMapMin + (GainMapMax - GainMapMin) * gain_map
其中 sdr/hdr 都是线性光, 归一化到 SDR 白点 = 1.0。
"""

import argparse
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

try:
    import pillow_heif

    pillow_heif.register_heif_opener()
    HAS_HEIF = True
except ImportError:
    HAS_HEIF = False


# ============================================================
# PQ EOTF (ST 2084): PQ 编码 [0,1] -> 线性亮度 (nits)
# 常数与 libultrahdr 的 pqInvOetf() 一致。
# ============================================================
PQ_M1 = 2610.0 / 16384.0
PQ_M2 = 2523.0 / 4096.0 * 128.0
PQ_C1 = 3424.0 / 4096.0
PQ_C2 = 2413.0 / 4096.0 * 32.0
PQ_C3 = 2392.0 / 4096.0 * 32.0


def pq_eotf(pq_norm: np.ndarray) -> np.ndarray:
    E = np.clip(pq_norm, 0.0, 1.0).astype(np.float64)
    Ep = np.power(E, 1.0 / PQ_M2)
    num = np.maximum(Ep - PQ_C1, 0.0)
    den = PQ_C2 - PQ_C3 * Ep
    return 10000.0 * np.power(num / den, 1.0 / PQ_M1)


def srgb_to_linear(srgb: np.ndarray) -> np.ndarray:
    a, thr = 0.055, 0.04045
    x = np.clip(srgb, 0.0, 1.0)
    return np.where(x <= thr, x / 12.92, np.power((x + a) / (1 + a), 2.4))


# ============================================================
# 色域矩阵: 线性 BT.2020 (D65) -> 线性 P3-D65
# 与 libultrahdr 的 kBt2100ToP3 以及独立 primaries+Bradford 计算一致 (~5e-6)。
# ============================================================
M_BT2020_TO_P3_D65 = np.array([
    [ 1.343578, -0.282183, -0.061393],
    [-0.065294,  1.075788, -0.010494],
    [ 0.002824, -0.019597,  1.016773],
], dtype=np.float64)

# Display P3 的亮度系数 (SMPTE EG 432-1, libultrahdr 的 kP3R/G/B)。
# 注意不是 Rec.709 的 (0.2126, 0.7152, 0.0722): gain map 工作在 P3 上。
LUMA_P3 = np.array([0.2289746, 0.6917385, 0.0792869], dtype=np.float64)

# UltraHDR 的位移量。单位是 SDR 满量程的比例 (SDR 白 = 1.0), 不是 nits。
OFFSET_SDR = 1.0 / 64.0
OFFSET_HDR = 1.0 / 64.0


def bt2020_to_p3(linear_rgb: np.ndarray) -> np.ndarray:
    flat = linear_rgb.reshape(-1, 3)
    return (flat @ M_BT2020_TO_P3_D65.T).reshape(linear_rgb.shape)


def luma_p3(rgb: np.ndarray) -> np.ndarray:
    return np.sum(rgb * LUMA_P3, axis=-1)


# ============================================================
# Display P3 ICC profile 加载
# ============================================================
def load_display_p3_icc(explicit_path: str | None = None) -> bytes | None:
    """尝试加载 Display P3 ICC profile; 找不到时返回 None。"""
    if explicit_path:
        with open(explicit_path, "rb") as f:
            return f.read()

    candidates = [
        # macOS
        "/System/Library/ColorSync/Profiles/Display P3.icc",
        "/Library/ColorSync/Profiles/Display P3.icc",
        os.path.expanduser("~/Library/ColorSync/Profiles/Display P3.icc"),
        # Linux (colord / icc-profiles-free)
        "/usr/share/color/icc/colord/Display P3.icc",
        "/usr/share/color/icc/Display P3.icc",
        "/usr/share/color/icc/display-p3.icc",
        "/usr/share/color/icc/colord/Display-P3.icc",
        "./Display P3.icc",
        # Windows
        os.path.expandvars(
            r"%SystemRoot%\System32\spool\drivers\color\Display P3.icc"
        ),
    ]
    for p in candidates:
        if p and os.path.exists(p):
            with open(p, "rb") as f:
                return f.read()
    return None


# ============================================================
# ffmpeg 色调映射 (默认 mobius)
# ============================================================
def run_ffmpeg_tonemap(input_path, output_png, sdr_white=203.0,
                       tonemap="mobius", filter_chain=None):
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("未找到 ffmpeg")

    if filter_chain is None:
        # npl=<sdr_white> 让 zscale 输出的线性光以 sdr_white nit 为 1.0;
        # 随后的 tonemap 把它压到 [0,1], 即 SDR 白 = sdr_white nits。
        filter_chain = (
            f"zscale=t=linear:npl={sdr_white}:r=full,"
            "format=gbrpf32le,"
            "zscale=p=smpte432,"
            f"tonemap=tonemap={tonemap}:desat=0,"
            "zscale=t=iec61966-2-1:p=smpte432:r=full,"
            "format=rgb24"
        )

    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-i", input_path,
        "-vf", filter_chain,
        "-frames:v", "1",
        "-pix_fmt", "rgb24",
        "-y", output_png,
    ]
    print(f"      执行: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


# ============================================================
# 读原 AVIF -> PQ 码值 -> 线性 P3 (nits)
# ============================================================
def _mode_dtype(mode):
    """pillow-heif 的 16-bit 模式名 -> (numpy dtype, 通道数)。"""
    m = str(mode).upper()
    if ";" in m:  # e.g. "RGB;16" 或 PIL 的 "I;16L"
        base, depth = m.split(";", 1)
        depth = int("".join(c for c in depth if c.isdigit()) or 8)
    else:
        base, depth = m, 8
    channels = {"L": 1, "LA": 2, "I": 1, "F": 1,
                "RGB": 3, "RGBA": 4, "BGR": 3, "BGRA": 4}.get(base, 3)
    return (np.uint8 if depth <= 8 else np.uint16), channels


def _image_array(img):
    """不经过 PIL 的 convert(): 那会把 16-bit 直接砍成 8-bit。"""
    try:
        return np.asarray(img)
    except Exception:
        pass
    dt, channels = _mode_dtype(getattr(img, "mode", "RGB"))
    w, h = img.size
    itemsize = np.dtype(dt).itemsize
    stride = getattr(img, "stride", w * channels * itemsize)
    raw = np.frombuffer(img.data, dtype=dt)
    return raw.reshape(h, stride // itemsize)[:, : w * channels].reshape(h, w, channels)


def _cicp_from_info(info):
    """尽力从 pillow-heif 的 info 里取出 (primaries, transfer); 取不到返回 None。"""
    nclx = (info or {}).get("nclx_profile")
    if nclx is None:
        return None

    def field(names, idx):
        for name in names:
            v = getattr(nclx, name, None)
            if v is not None:
                return int(v)
        if isinstance(nclx, dict):
            for name in names:
                if name in nclx:
                    return int(nclx[name])
        if isinstance(nclx, (tuple, list)) and len(nclx) > idx:
            return int(nclx[idx])
        return None

    return (field(("primaries", "colour_primaries", "color_primaries"), 0),
            field(("transfer_characteristics", "transfer"), 1))


def _pq_codes(arr, bit_depth):
    """把解码出来的整数样本归一化成 PQ 码值 [0,1]。"""
    if arr.dtype == np.uint8:
        return arr.astype(np.float32) / 255.0
    if arr.dtype != np.uint16:
        raise ValueError(f"Unsupported dtype: {arr.dtype}")

    # 10/12-bit 源可能以原始码值存在 16-bit 容器里, 也可能被左移到满量程
    # (libheif 通常会左移)。用文件报告的位深判断, 数据最大值只作为兜底。
    if bit_depth in (10, 12):
        full = (1 << bit_depth) - 1
        if int(arr.max()) <= full:
            print(f"      [提示] {bit_depth}-bit 源按原始码值处理 (max={int(arr.max())})")
            return arr.astype(np.float32) / full
        return arr.astype(np.float32) / 65535.0
    if bit_depth in (8, 16):
        return arr.astype(np.float32) / (255.0 if bit_depth == 8 else 65535.0)

    m = int(arr.max())
    if m <= 1023:
        return arr.astype(np.float32) / 1023.0
    if m <= 4095:
        return arr.astype(np.float32) / 4095.0
    return arr.astype(np.float32) / 65535.0


def read_avif_hdr_p3_nits(path, assume_pq=False):
    """读 HDR AVIF -> 线性 P3 (nits)。"""
    if HAS_HEIF:
        # convert_hdr_to_8bit=False: 保留 10/12-bit, 否则 gain map 只剩 8-bit 精度
        heif = pillow_heif.open_heif(path, convert_hdr_to_8bit=False)
        img = heif[0]
    else:
        img = Image.open(path)
        print("      [警告] 没有 pillow-heif, 用 Pillow 解码; HDR 精度会掉到 8-bit")
        print("             建议: pip install pillow-heif")

    info = dict(getattr(img, "info", {}) or {})
    arr = _image_array(img)
    if arr.ndim == 2:
        arr = np.stack([arr] * 3, axis=-1)
    arr = arr[:, :, :3]
    bit_depth = info.get("bit_depth")

    cicp = _cicp_from_info(info)
    if cicp is not None and cicp[0] is not None:
        primaries, transfer = cicp
        # 9 = BT.2020 primaries, 16 = PQ (ST 2084)
        if (primaries, transfer) != (9, 16):
            msg = (f"输入 AVIF 的 CICP 是 primaries={primaries} transfer={transfer}, "
                   f"但本脚本只支持 BT.2020 + PQ (9/16)")
            if assume_pq:
                print(f"      [警告] {msg}; 已按 --assume-pq 继续")
            else:
                raise RuntimeError(msg + "; 确认无误可加 --assume-pq")
    elif not assume_pq:
        print("      [提示] 读不到 CICP 元数据, 按 BT.2020 + PQ 处理 (可用 --assume-pq 消除此提示)")

    pq = _pq_codes(arr, bit_depth)
    hdr_2020 = pq_eotf(pq)
    return bt2020_to_p3(hdr_2020), {"size": img.size, "bit_depth": bit_depth,
                                    "cicp": cicp, "mode": getattr(img, "mode", "")}


# ============================================================
# 基础图 JPEG
# ============================================================
def encode_base_jpeg(srgb_p3_u8, icc_bytes, quality, subsampling):
    buf = io.BytesIO()
    kwargs = dict(format="JPEG", quality=quality, optimize=True, subsampling=subsampling)
    if icc_bytes:
        kwargs["icc_profile"] = icc_bytes
    Image.fromarray(srgb_p3_u8, "RGB").save(buf, **kwargs)
    return buf.getvalue()


def decode_base_jpeg(jpeg_bytes):
    """解码基础图, 得到的是解码端真正会看到的 8-bit sRGB 值。"""
    return np.asarray(Image.open(io.BytesIO(jpeg_bytes)).convert("RGB")).astype(np.float64) / 255.0


# ============================================================
# Gain map 计算
# ============================================================
def compute_gainmap(hdr_p3_nits, sdr_p3_nits, sdr_white,
                    trim_low=1.0, trim_high=0.0, debug=True):
    """返回 (uint8 gain map, metadata dict)。

    解码端公式 (libultrahdr / Android):
        hdr = (sdr + OffsetSDR) * 2^log2_recovery - OffsetHDR
    所以编码端必须用完全相同的位移形式求 log2_recovery, 否则暗部会系统性偏移。
    """
    hdr_l = np.maximum(luma_p3(hdr_p3_nits / sdr_white), 1e-6)
    sdr_l = np.maximum(luma_p3(sdr_p3_nits / sdr_white), 1e-6)

    if debug:
        def pct(a):
            return [np.percentile(a, p) for p in (0.1, 1, 5, 50, 95, 99, 99.9)]
        for name, arr in (("HDR (SDR=1.0)", hdr_l), ("SDR (SDR=1.0)", sdr_l)):
            ps = pct(arr)
            print(f"      {name}: "
                  f"p0.1={ps[0]:.5f} p1={ps[1]:.5f} p5={ps[2]:.5f} p50={ps[3]:.5f} "
                  f"p95={ps[4]:.5f} p99={ps[5]:.5f} p99.9={ps[6]:.5f}")

    log_ratio = np.log2((hdr_l + OFFSET_HDR) / (sdr_l + OFFSET_SDR))

    if debug:
        ps = [np.percentile(log_ratio, p) for p in (0.1, 1, 5, 50, 95, 99, 99.9)]
        print(f"      log2(HDR/SDR): "
              f"p0.1={ps[0]:.2f} p1={ps[1]:.2f} p5={ps[2]:.2f} p50={ps[3]:.2f} "
              f"p95={ps[4]:.2f} p99={ps[5]:.2f} p99.9={ps[6]:.2f} "
              f"min={log_ratio.min():.2f} max={log_ratio.max():.2f}")

    log_min = (float(np.percentile(log_ratio, trim_low)) if trim_low > 0
               else float(log_ratio.min()))
    log_max = (float(np.percentile(log_ratio, 100.0 - trim_high)) if trim_high > 0
               else float(log_ratio.max()))
    if log_max - log_min < 0.5:
        log_max = log_min + 0.5

    clipped_lo = float(np.mean(log_ratio < log_min))
    clipped_hi = float(np.mean(log_ratio > log_max))
    step = (log_max - log_min) / 255.0
    print(f"      范围 {log_min:+.3f} .. {log_max:+.3f} log2 "
          f"({log_max - log_min:.2f} stops, 峰值 {2 ** log_max:.2f}x); "
          f"8-bit 每级 {step:.4f} log2 = {100 * (2 ** step - 1):.2f}%")
    print(f"      被裁剪: 低 {100 * clipped_lo:.3f}% / 高 {100 * clipped_hi:.3f}%")

    norm = np.clip((log_ratio - log_min) / (log_max - log_min), 0.0, 1.0)
    gm_uint8 = np.round(norm * 255.0).astype(np.uint8)

    meta = {
        "GainMapMin": log_min,
        "GainMapMax": log_max,
        "Gamma": 1.0,
        "OffsetSDR": OFFSET_SDR,
        "OffsetHDR": OFFSET_HDR,
        # HDRCapacity 是"显示设备余量"(线性 boost, 1.0 = 无余量), 以 log2 写进 XMP。
        # 下限必须是 0: 否则 SDR 显示 (boost=1) 也会按比例应用 gain map,
        # 基础图会被改得比原始 SDR 更暗。
        "HDRCapacityMin": 0.0,
        "HDRCapacityMax": max(log_max, 0.0),
        "BaseRenditionIsHDR": "False",
    }
    return gm_uint8, meta


# ============================================================
# UltraHDR 组装 (XMP with GContainer + MPF)
# ============================================================
def jpeg_segment(marker, payload):
    length = len(payload) + 2
    if length > 0xFFFF:
        raise ValueError(f"JPEG segment too large: {length}")
    return bytes([0xFF, marker]) + struct.pack(">H", length) + payload


def _hdrgm_attributes(meta):
    return "".join([
        f'\n    hdrgm:Version="1.0"',
        f'\n    hdrgm:GainMapMin="{meta["GainMapMin"]:.6f}"',
        f'\n    hdrgm:GainMapMax="{meta["GainMapMax"]:.6f}"',
        f'\n    hdrgm:Gamma="{meta["Gamma"]:.6f}"',
        f'\n    hdrgm:OffsetSDR="{meta["OffsetSDR"]:.6f}"',
        f'\n    hdrgm:OffsetHDR="{meta["OffsetHDR"]:.6f}"',
        f'\n    hdrgm:HDRCapacityMin="{meta["HDRCapacityMin"]:.6f}"',
        f'\n    hdrgm:HDRCapacityMax="{meta["HDRCapacityMax"]:.6f}"',
        f'\n    hdrgm:BaseRenditionIsHDR="{meta["BaseRenditionIsHDR"]}"',
    ])


def build_primary_xmp_segment(meta, gainmap_length):
    """主图 XMP: Container:Directory (语义 + gain map 长度) 与 hdrgm 参数。

    参考实现把 hdrgm 参数放在 gain map 图的 XMP 里, 主图只放 Container。
    这里两边都写一份: 多出的属性没有副作用, 而只读其中一边的解码器都能工作。
    """
    xmp = f'''<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Python UltraHDR Encoder">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/"
    xmlns:Container="http://ns.google.com/photos/1.0/container/"
    xmlns:Item="http://ns.google.com/photos/1.0/container/item/"{_hdrgm_attributes(meta)}>
   <Container:Directory>
    <rdf:Seq>
     <rdf:li rdf:parseType="Resource">
      <Container:Item Item:Semantic="Primary" Item:Mime="image/jpeg"/>
     </rdf:li>
     <rdf:li rdf:parseType="Resource">
      <Container:Item Item:Semantic="GainMap" Item:Mime="image/jpeg" Item:Length="{gainmap_length}"/>
     </rdf:li>
    </rdf:Seq>
   </Container:Directory>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>'''.encode("utf-8")
    return jpeg_segment(0xE1, b"http://ns.adobe.com/xap/1.0/\x00" + xmp)


def build_gainmap_xmp_segment(meta):
    """gain map 图自己的 XMP: 只有 hdrgm 参数 (与 libultrahdr 的 secondary XMP 一致)。

    libultrahdr 的解码器从 gain map 图的 XMP 里读 GainMapMax 等参数, 找不到就报错,
    所以这一份不能省。
    """
    xmp = f'''<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Python UltraHDR Encoder">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/"{_hdrgm_attributes(meta)}>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>'''.encode("utf-8")
    return jpeg_segment(0xE1, b"http://ns.adobe.com/xap/1.0/\x00" + xmp)


def insert_xmp_segment(jpeg_bytes, xmp_segment):
    """把 APP1 插到 SOI 之后 (SOI 位置不变, MPF 的偏移量不受影响)。"""
    if jpeg_bytes[:2] != b"\xff\xd8":
        raise ValueError("not a JPEG")
    return jpeg_bytes[:2] + xmp_segment + jpeg_bytes[2:]


def build_mpf_data(primary_size, gainmap_size, gainmap_offset):
    ifd_offset = 8
    n_entries = 3
    ifd_size = 2 + n_entries * 12 + 4
    mpentry_offset = ifd_offset + ifd_size

    primary_entry = struct.pack("<IIIHH", 0x20000000, primary_size, 0, 0, 0)
    gm_entry = struct.pack("<IIIHH", 0x00000000, gainmap_size,
                           gainmap_offset, 0, 0)
    mpentry_data = primary_entry + gm_entry

    ifd = struct.pack("<H", n_entries)
    ifd += struct.pack("<HHI", 0xB001, 4, 1) + struct.pack("<I", 2)
    ifd += struct.pack("<HHI", 0xB002, 7, len(mpentry_data)) \
         + struct.pack("<I", mpentry_offset)
    ifd += struct.pack("<HHI", 0xB000, 7, 4) + b"0100"
    ifd += struct.pack("<I", 0)

    tiff = b"II" + struct.pack("<H", 0x002A) \
         + struct.pack("<I", ifd_offset) + ifd + mpentry_data
    return b"MPF\x00" + tiff


def build_mpf_segment(primary_size, gainmap_size, gainmap_offset):
    payload = build_mpf_data(primary_size, gainmap_size, gainmap_offset)
    return jpeg_segment(0xE2, payload)


def assemble_ultrahdr(primary_jpeg, gainmap_jpeg, primary_xmp_segment):
    """SOI + XMP + MPF + 主图(去掉自己的 SOI) + gain map JPEG。

    主图大小和 gain map 偏移都是"文件里实际存储的字节数/位置":
        primary_size   = len(XMP) + len(MPF) + len(primary_jpeg)   (SOI 到主图 EOI)
        gainmap_offset = len(MPF payload) + len(primary_jpeg) - 6  (相对 MP 头的 "II")
    后者的 -6 来自 MPF payload = "MPF\0" + TIFF, TIFF 头在段内偏移 8, 而 MP Entry
    的偏移基准是 TIFF 头本身。
    """
    mpf_data_len = len(build_mpf_data(0, len(gainmap_jpeg), 0))
    mpf_segment = build_mpf_segment(0, len(gainmap_jpeg), 0)
    primary_size = len(primary_xmp_segment) + len(mpf_segment) + len(primary_jpeg)
    gainmap_offset = mpf_data_len + len(primary_jpeg) - 6

    mpf_segment = build_mpf_segment(primary_size, len(gainmap_jpeg), gainmap_offset)

    out = bytearray()
    out += b"\xFF\xD8"
    out += primary_xmp_segment
    out += mpf_segment
    out += primary_jpeg[2:]
    out += gainmap_jpeg
    return bytes(out)


# ============================================================
# 主流程
# ============================================================
def convert(input_path, output_path,
            sdr_white=203.0,
            tonemap="mobius",
            sdr_quality=95,
            sdr_subsampling=0,
            gm_quality=90,
            gm_trim_low=0.01,
            gm_trim_high=0.0,
            filter_chain=None,
            icc_path=None,
            assume_pq=False,
            keep_temp=False):
    print(f"[1/6] ffmpeg 色调映射 (tonemap={tonemap}, SDR 白={sdr_white:g} nits)")
    tmp_dir = tempfile.mkdtemp(prefix="ultrahdr_")
    tmp_png = str(Path(tmp_dir) / "sdr_p3.png")
    run_ffmpeg_tonemap(input_path, tmp_png,
                       sdr_white=sdr_white,
                       tonemap=tonemap,
                       filter_chain=filter_chain)
    print(f"      中间图: {tmp_png} ({Path(tmp_png).stat().st_size} bytes)")

    print(f"[2/6] 读原 AVIF -> PQ EOTF -> 线性 P3 nits")
    hdr_p3_nits, hdr_info = read_avif_hdr_p3_nits(input_path, assume_pq=assume_pq)
    print(f"      HDR size={hdr_info['size']}, mode={hdr_info['mode']!r}, "
          f"bit_depth={hdr_info['bit_depth']}, "
          f"peak={hdr_p3_nits.max():.1f} nits")

    print(f"[3/6] 编码 SDR 基础图 JPEG (Display P3 + sRGB 传递函数)")
    srgb_p3 = np.asarray(Image.open(tmp_png).convert("RGB")).astype(np.float64) / 255.0
    sdr_u8 = np.round(srgb_p3 * 255.0).astype(np.uint8)

    icc_bytes = load_display_p3_icc(icc_path)
    if icc_bytes is None:
        print("      [警告] 未找到 Display P3 ICC profile, JPEG 将不带 ICC "
              "(解码端会按 sRGB 解释, 颜色会偏饱和)")
    else:
        print(f"      嵌入 ICC: {len(icc_bytes)} bytes")

    primary_jpeg = encode_base_jpeg(sdr_u8, icc_bytes, sdr_quality, sdr_subsampling)
    print(f"      SDR 基础图: {len(primary_jpeg)} bytes "
          f"(quality={sdr_quality}, subsampling={['444', '422', '420'][sdr_subsampling]})")

    if hdr_p3_nits.shape[:2] != sdr_u8.shape[:2]:
        raise RuntimeError(
            f"尺寸不匹配: HDR {hdr_p3_nits.shape[:2]} vs SDR {sdr_u8.shape[:2]}"
        )

    print(f"[4/6] 计算 gain map")
    # 用"解码后的基础图"作为 SDR 参考: 解码端看到的就是这些值, 这样重建最准。
    sdr_ref_u8 = decode_base_jpeg(primary_jpeg)
    sdr_p3_nits = srgb_to_linear(sdr_ref_u8) * sdr_white
    print(f"      SDR peak={sdr_p3_nits.max():.1f} nits")
    gm_u8, gm_meta = compute_gainmap(hdr_p3_nits, sdr_p3_nits, sdr_white,
                                     trim_low=gm_trim_low, trim_high=gm_trim_high,
                                     debug=True)

    buf = io.BytesIO()
    Image.fromarray(gm_u8, "L").save(
        buf, format="JPEG", quality=gm_quality, optimize=True
    )
    gm_jpeg = buf.getvalue()

    print(f"[5/6] 组装 UltraHDR JPEG")
    gm_jpeg = insert_xmp_segment(gm_jpeg, build_gainmap_xmp_segment(gm_meta))
    xmp_seg = build_primary_xmp_segment(gm_meta, len(gm_jpeg))
    out = assemble_ultrahdr(primary_jpeg, gm_jpeg, xmp_seg)
    print(f"      gain map: {len(gm_jpeg)} bytes (含 XMP); 总计 {len(out)} bytes")

    Path(output_path).write_bytes(out)
    print(f"[6/6] 完成: {output_path}")

    if not keep_temp:
        shutil.rmtree(tmp_dir, ignore_errors=True)
    else:
        print(f"中间文件: {tmp_dir}")


def main():
    ap = argparse.ArgumentParser(
        description="AVIF(HDR10/PQ) -> UltraHDR JPEG"
    )
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--sdr-white", type=float, default=203.0,
                    help="SDR 白点亮度 nits (默认 203, 即 BT.2408 的 diffuse white)")
    ap.add_argument("--tonemap", type=str, default="mobius",
                    choices=["mobius", "hable", "reinhard", "linear"],
                    help="ffmpeg 色调映射算法 (默认 mobius)")
    ap.add_argument("--sdr-quality", type=int, default=95)
    ap.add_argument("--sdr-subsampling", type=int, default=0, choices=[0, 1, 2],
                    help="基础图 JPEG 色度抽样: 0=4:4:4 (默认, 截图更合适), 1=4:2:2, 2=4:2:0")
    ap.add_argument("--gm-quality", type=int, default=90)
    ap.add_argument("--gm-trim-low", type=float, default=0.01,
                    help="gain map 低端裁剪百分位 (默认 0.01; 深暗部是色调映射噪声)")
    ap.add_argument("--gm-trim-high", type=float, default=0.0,
                    help="gain map 高端裁剪百分位 (默认 0, 用绝对最大值保高光)")
    ap.add_argument("--filter", type=str, default=None,
                    help="自定义 ffmpeg -vf 滤镜链")
    ap.add_argument("--icc", type=str, default=None,
                    help="Display P3 ICC 文件路径 (缺省自动搜索)")
    ap.add_argument("--assume-pq", action="store_true",
                    help="跳过输入 AVIF 的 CICP 检查, 强制按 BT.2020 + PQ 处理")
    ap.add_argument("--keep-temp", action="store_true",
                    help="保留中间 P3 PNG 便于调试")
    args = ap.parse_args()

    try:
        convert(
            args.input, args.output,
            sdr_white=args.sdr_white,
            tonemap=args.tonemap,
            sdr_quality=args.sdr_quality,
            sdr_subsampling=args.sdr_subsampling,
            gm_quality=args.gm_quality,
            gm_trim_low=args.gm_trim_low,
            gm_trim_high=args.gm_trim_high,
            filter_chain=args.filter,
            icc_path=args.icc,
            assume_pq=args.assume_pq,
            keep_temp=args.keep_temp,
        )
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
