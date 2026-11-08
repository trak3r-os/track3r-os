#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The boot logo: the project's PNG turned into an FT81x bitmap whose raw-deflate stream lives in flash (stdlib only).

OS-FLASHDIET A5: the bitmap's bytes are deflated by tools/deflate_pack.py (never Python's zlib) and the generated file
stores the stream; the device inflates it at boot with core::Inflater.

OS-003f, docs/specs/OS-003-ui-toolkit.md "003f - Boot logo". The source is ui/assets/boot_logo.png, a byte-exact copy
of the project's logo.png and the only source of truth. The pipeline, every step deterministic (integer arithmetic only):

  1. decode      a PNG decoder on zlib: non-interlaced 8-bit gray, gray + alpha, RGB and RGBA, every filter type, CRCs
                 checked; alpha is composited on black (the boot screen is black); then luminance (ITU-R BT.601, integer)
  2. crop        the bounding box of everything brighter than CROP_THRESHOLD: the black margin costs flash and size, and
                 cropping centres the picture itself, not its margin
  3. grid        detect the pixel-art grid (an integer block size and its phase, per axis) from where the edges of the
                 picture lie, and take the centre of every block. The fit is accepted only if at least GRID_MIN_PERMILLE
                 of all the edge energy lies on block boundaries. A picture whose "pixels" are not a regular grid (an
                 AI-drawn one: the blocks of the project's logo are 8 px in one place and 11 in another) is not forced onto one
  4. fallback    no grid: the crop is resampled to FALLBACK_CELLS tall (the width follows) with box filtering over the
                 central half of each cell (BOX_NUM / BOX_DEN), which keeps a checkerboard dither's contrast where a full-cell
                 box would average it into flat grey
  5. format      the smallest of L1, L2, L4 whose quantisation error (RMS in grey levels, against the native image) is at most
                 FORMAT_MAX_RMS; L4 if none is. Also reported: the error of the whole picture against the source, both at the
                 size it is shown on the screen (the native grid times the integer scale)
  6. output      a C++ source with the pixels packed rows MSB first and deflated into a word-aligned const stream
                 (OS-FLASHDIET A5: tools/deflate_pack.py makes the raw-deflate bytes, never Python's zlib), plus
                 static_asserts that tie it to ui/boot_logo.h (the constants of the draw code); a ctest regenerates it
                 and fails when the committed one differs

Usage:
  logo_convert.py --png boot_logo.png --out boot_logo_data.cpp     (rewritten only when it differs)
  logo_convert.py --png boot_logo.png --check boot_logo_data.cpp   (exit 1 when the committed file is stale)
  logo_convert.py --png boot_logo.png --report                     (what it decided, no output file)
  logo_convert.py --png boot_logo.png --preview native.png         (the native bitmap at screen size, to look at)
  logo_convert.py --self-test
"""
import argparse
import hashlib
import itertools
import math
import operator
import struct
import sys
import tempfile
import zlib
from pathlib import Path

# The build runs the converters as `python3 -I tools/logo_convert.py`, and -I keeps this script's directory off
# sys.path, so add it to import the A2 encoder that sits beside this script.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import deflate_pack  # noqa: E402  the OS-FLASHDIET A2 deterministic raw-deflate encoder

# ---- Tunables (each is a decision, written down here and echoed in the generated file) ---------------------------------

CROP_THRESHOLD = 24          # luminance above this is picture; the logo's own margin is black (0..1)
GRID_MIN_PITCH = 2           # the block sizes tried, in source pixels
GRID_MAX_PITCH = 64
GRID_MIN_PERMILLE = 850      # share of the edge energy that must lie on block boundaries for a grid to be accepted
GRID_TIE_PERMILLE = 20       # a larger block size within this of the best score wins (a grid of 7 is also one of 14 if it can be)
FALLBACK_CELLS = 124         # native height without a grid: 124 x 3 = 372 px. The shutter's checkerboard is 7.9 px per square in the
                             # source and the crop is 973 px tall, so a cell of 7.85 px samples it about once a square
BOX_NUM, BOX_DEN = 1, 2      # the central half of each cell is averaged (1 would be a full box)
SCREEN_MIN_PX, SCREEN_MAX_PX = 360, 420  # the integer scale makes the picture this tall on the 480 px screen (spec 003f)
FORMAT_MAX_RMS = 6           # grey levels (of 255): above this a format does not keep the dither
TONE_BLOCK = 4               # the tone error compares the mean of every 4x4 patch of native pixels

# FT81x BITMAP_LAYOUT formats (Programmers Guide v1.2 Table 7, p.97) in the order they are tried: fewest bits first.
FORMATS = (("L1", 1, 1), ("L2", 17, 2), ("L4", 2, 4))  # name, BITMAP_LAYOUT value, bits per pixel

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}   # colour type -> samples per pixel (8-bit only)
MAX_SIDE = 4096                        # a side: with MAX_DECODED_BYTES, a decode never needs more than about 64 MiB (4096 x 4096 RGBA)
MAX_DECODED_BYTES = 64 * 1024 * 1024   # width x height x samples a pixel


class LogoError(Exception):
    pass


# ---- 1. PNG ------------------------------------------------------------------------------------------------------------

def read_chunks(data):
    """The chunks of a PNG as (type, body), CRC checked, up to and including IEND."""
    if data[:8] != PNG_SIGNATURE:
        raise LogoError("not a PNG (bad signature)")
    pos = 8
    chunks = []
    while True:
        if pos + 8 > len(data):
            raise LogoError("PNG truncated: no IEND")
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        end = pos + 8 + length
        if end + 4 > len(data):
            raise LogoError("PNG truncated inside a %s chunk" % ctype.decode("latin-1"))
        body = data[pos + 8:end]
        (crc,) = struct.unpack(">I", data[end:end + 4])
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise LogoError("bad CRC in a %s chunk" % ctype.decode("latin-1"))
        chunks.append((ctype, body))
        pos = end + 4
        if ctype == b"IEND":
            return chunks


def unfilter(raw, width, height, bpp):
    """The scanlines of a decompressed non-interlaced image: filter types 0..4 undone (PNG spec section 9)."""
    stride = width * bpp
    if len(raw) != height * (1 + stride):
        raise LogoError("image data is %d bytes, %d expected" % (len(raw), height * (1 + stride)))
    rows = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub: the byte bpp to the left, per channel
            for c in range(bpp):
                line[c::bpp] = bytearray(itertools.accumulate(line[c::bpp], lambda a, b: (a + b) & 255))
        elif ftype == 2:  # Up
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif ftype == 3:  # Average of left and up
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        else:
            raise LogoError("unknown PNG filter type %d" % ftype)
        rows.append(line)
        prev = line
    return rows


def decode_png(data):
    """-> (width, height, colour type, rows): each row a bytearray of width * channels samples."""
    chunks = read_chunks(data)
    if not chunks or chunks[0][0] != b"IHDR" or len(chunks[0][1]) != 13:
        raise LogoError("PNG: the first chunk must be a 13-byte IHDR")
    width, height, depth, ctype, comp, filt, interlace = struct.unpack(">IIBBBBB", chunks[0][1])
    if width < 1 or height < 1 or width > MAX_SIDE or height > MAX_SIDE:
        raise LogoError("PNG: %dx%d is outside 1..%d" % (width, height, MAX_SIDE))
    if depth != 8:
        raise LogoError("PNG: bit depth %d is not supported (8 only)" % depth)
    if ctype not in CHANNELS:
        raise LogoError("PNG: colour type %d is not supported (gray, gray + alpha, RGB, RGBA; not palette)" % ctype)
    if comp != 0 or filt != 0:
        raise LogoError("PNG: unknown compression or filter method")
    if interlace != 0:
        raise LogoError("PNG: interlaced images are not supported")
    bpp = CHANNELS[ctype]
    if width * height * bpp > MAX_DECODED_BYTES:
        raise LogoError("PNG: %dx%d at %d samples a pixel is more than %d MiB decoded" % (width, height, bpp, MAX_DECODED_BYTES >> 20))
    idat = b"".join(body for t, body in chunks if t == b"IDAT")
    if not idat:
        raise LogoError("PNG: no image data")
    expected = height * (1 + width * bpp)
    inflater = zlib.decompressobj()
    try:
        raw = inflater.decompress(idat, expected + 1)  # bounded: a bomb stops one byte past the size it claims
    except zlib.error as e:
        raise LogoError("PNG: image data does not inflate (%s)" % e)
    if len(raw) != expected or not inflater.eof:
        raise LogoError("PNG: image data is not %d bytes" % expected)
    return width, height, ctype, unfilter(raw, width, height, bpp)


def luminance(width, ctype, rows):
    """Rows of 0..255 luminance, integer BT.601 (299 R + 587 G + 114 B), alpha composited on black."""
    out = []
    for row in rows:
        if ctype == 0:
            y = list(row)
        elif ctype == 4:
            y = [(v * a + 127) // 255 for v, a in zip(row[0::2], row[1::2])]
        elif ctype == 2:
            y = [(299 * r + 587 * g + 114 * b + 500) // 1000 for r, g, b in zip(row[0::3], row[1::3], row[2::3])]
        else:
            y = [((299 * r + 587 * g + 114 * b + 500) // 1000 * a + 127) // 255
                 for r, g, b, a in zip(row[0::4], row[1::4], row[2::4], row[3::4])]
        out.append(y)
    return out


# ---- 2. Crop and the summed-area table -----------------------------------------------------------------------------------

def crop_box(rows, threshold=CROP_THRESHOLD):
    """(x0, y0, x1, y1), exclusive ends, of the pixels brighter than `threshold`."""
    ys = [y for y, row in enumerate(rows) if max(row) > threshold]
    if not ys:
        raise LogoError("the picture is blank (nothing brighter than %d)" % threshold)
    col_max = list(map(max, zip(*rows)))
    xs = [x for x, v in enumerate(col_max) if v > threshold]
    return xs[0], ys[0], xs[-1] + 1, ys[-1] + 1


def cropped(rows, box):
    x0, y0, x1, y1 = box
    return [row[x0:x1] for row in rows[y0:y1]]


def integral(rows):
    """Summed-area table: sat[y][x] is the sum of rows[:y][:x]."""
    sat = [[0] * (len(rows[0]) + 1)]
    for row in rows:
        acc = list(itertools.accumulate(row, initial=0))
        sat.append(list(map(operator.add, acc, sat[-1])))
    return sat


def window_mean(sat, x0, y0, x1, y1):
    area = (x1 - x0) * (y1 - y0)
    total = sat[y1][x1] - sat[y0][x1] - sat[y1][x0] + sat[y0][x0]
    return (2 * total + area) // (2 * area)


# ---- 3. Grid ---------------------------------------------------------------------------------------------------------------

def edge_profiles(rows):
    """gx[x]: the summed |difference| between column x - 1 and column x over all rows (gx[0] = 0); gy likewise for rows."""
    cols = list(zip(*rows))
    gx = [0] + [sum(map(abs, map(operator.sub, cols[x], cols[x - 1]))) for x in range(1, len(cols))]
    gy = [0] + [sum(map(abs, map(operator.sub, rows[y], rows[y - 1]))) for y in range(1, len(rows))]
    return gx, gy


def detect_axis(profile):
    """-> (pitch, phase, permille, best_permille): the block size and phase whose boundaries carry the largest share of the edge
    energy, accepted only if that share reaches GRID_MIN_PERMILLE (else pitch is None, and the best share seen is reported).
    A cell starts at phase + k * pitch."""
    total = sum(profile)
    if total == 0:
        return None, 0, 0, 0
    scored = []
    for pitch in range(GRID_MIN_PITCH, GRID_MAX_PITCH + 1):
        if len(profile) < 4 * pitch:  # a grid of fewer than four cells says nothing
            break
        for phase in range(pitch):
            scored.append((sum(profile[phase::pitch]) * 1000 // total, pitch, phase))
    if not scored:
        return None, 0, 0, 0
    best = max(s for s, _, _ in scored)
    if best < GRID_MIN_PERMILLE:
        return None, 0, 0, best
    candidates = [(p, -ph, s) for s, p, ph in scored if s >= best - GRID_TIE_PERMILLE]
    p, neg_phase, s = max(candidates)
    return p, -neg_phase, s, best


def sample_grid(rows, px, py, phx, phy):
    """The centre pixel of every whole block: rows of luminance, one per block row."""
    h, w = len(rows), len(rows[0])
    nx, ny = (w - phx) // px, (h - phy) // py
    if nx < 1 or ny < 1:
        raise LogoError("the grid leaves no whole block")
    return [[rows[phy + j * py + py // 2][phx + i * px + px // 2] for i in range(nx)] for j in range(ny)]


# ---- 4. Fallback: box filter over the centre of each cell ------------------------------------------------------------------

def resample_box(sat, w, h, out_w, out_h, num=BOX_NUM, den=BOX_DEN):
    """Mean over the central num/den of each of out_w x out_h cells that tile the w x h picture (integer boundaries)."""
    def span(i, n, size):
        lo = ((2 * den * i + (den - num)) * size) // (2 * den * n)
        hi = ((2 * den * i + (den + num)) * size) // (2 * den * n)
        lo = min(lo, size - 1)
        return lo, max(hi, lo + 1)
    xs = [span(i, out_w, w) for i in range(out_w)]
    ys = [span(j, out_h, h) for j in range(out_h)]
    return [[window_mean(sat, x0, y0, x1, y1) for x0, x1 in xs] for y0, y1 in ys]


# ---- 5. Format ----------------------------------------------------------------------------------------------------------------

def quantise(rows, bits):
    """(codes, reconstruction): the nearest of the format's levels (0, 255 / (2^bits - 1), ...) for each pixel."""
    top = (1 << bits) - 1
    codes = [[(v * top + 127) // 255 for v in row] for row in rows]
    return codes, [[c * 255 // top for c in row] for row in codes]


def sum_sq(a, b):
    return sum(sum((x - y) * (x - y) for x, y in zip(ra, rb)) for ra, rb in zip(a, b))


def rms_x100(sumsq, n):
    """sqrt(sumsq / n) times 100, as an integer (floats are never printed: the generated file must not vary by machine)."""
    return math.isqrt(sumsq * 10000 // n)


def pick_format(native):
    """-> (name, id, bits, codes, rms table): the smallest format that keeps the dither, else L4."""
    n = len(native) * len(native[0])
    table = []
    chosen = None
    for name, fid, bits in FORMATS:
        codes, recon = quantise(native, bits)
        ss = sum_sq(native, recon)
        table.append((name, rms_x100(ss, n)))
        if chosen is None and ss <= FORMAT_MAX_RMS * FORMAT_MAX_RMS * n:
            chosen = (name, fid, bits, codes)
    if chosen is None:
        name, fid, bits = FORMATS[-1]
        chosen = (name, fid, bits, quantise(native, bits)[0])
    return chosen + (table,)


def pick_scale(height):
    """The integer scale that makes the picture SCREEN_MIN_PX..SCREEN_MAX_PX tall (the largest one that fits)."""
    scale = SCREEN_MAX_PX // height
    if scale < 1 or height * scale < SCREEN_MIN_PX:
        raise LogoError("a native height of %d has no integer scale giving %d..%d px" % (height, SCREEN_MIN_PX, SCREEN_MAX_PX))
    return scale


def pack(codes, bits):
    """Rows of pixel codes into bytes: pixel 0 in the most significant bits, rows padded to whole bytes. -> (bytes, stride)."""
    per_byte = 8 // bits
    stride = (len(codes[0]) + per_byte - 1) // per_byte
    out = bytearray()
    for row in codes:
        for b in range(stride):
            v = 0
            for k in range(per_byte):
                i = b * per_byte + k
                v = (v << bits) | (row[i] if i < len(row) else 0)
            out.append(v)
    return bytes(out), stride


def screen_error(sat, w, h, native_recon, scale):
    """RMS (x100) of the shown picture against the source: the native bitmap at `scale`, nearest, against the crop
    box-averaged to the same size. What the dither and the resampling cost, whatever the format."""
    out_w, out_h = len(native_recon[0]) * scale, len(native_recon) * scale
    ref = resample_box(sat, w, h, out_w, out_h, 1, 1)
    ss = 0
    for y in range(out_h):
        r, q = ref[y], native_recon[y // scale]
        ss += sum((r[x] - q[x // scale]) ** 2 for x in range(out_w))
    return rms_x100(ss, out_w * out_h)


def tone_error(sat, w, h, recon, block=TONE_BLOCK):
    """RMS (x100) of the mean grey of every block x block patch of the native picture against the same patch of the source:
    the tone, which a dither is there to carry, without asking for its pixels."""
    n_w, n_h = len(recon[0]) // block, len(recon) // block
    if n_w == 0 or n_h == 0:
        raise LogoError("a native picture of %dx%d is smaller than one %dx%d patch" % (len(recon[0]), len(recon), block, block))
    ss = 0
    for j in range(n_h):
        for i in range(n_w):
            mine = sum(sum(recon[j * block + dy][i * block:(i + 1) * block]) for dy in range(block)) // (block * block)
            x0, x1 = i * block * w // len(recon[0]), (i + 1) * block * w // len(recon[0])
            y0, y1 = j * block * h // len(recon), (j + 1) * block * h // len(recon)
            ss += (mine - window_mean(sat, x0, y0, x1, y1)) ** 2
    return rms_x100(ss, n_w * n_h)


# ---- The whole conversion ---------------------------------------------------------------------------------------------------

class Result:
    pass


def check_native(width, height):
    """The native picture must hold at least one TONE_BLOCK patch each way (it is what the error measure, and any useful logo, needs)."""
    if width < TONE_BLOCK or height < TONE_BLOCK:
        raise LogoError("the native picture would be %dx%d: under %d pixels a side is no logo" % (width, height, TONE_BLOCK))


def convert(png_bytes, cells=FALLBACK_CELLS):
    res = Result()
    res.sha256 = hashlib.sha256(png_bytes).hexdigest()
    width, height, ctype, raw_rows = decode_png(png_bytes)
    res.source = (width, height, ctype)
    lum = luminance(width, ctype, raw_rows)
    res.box = crop_box(lum)
    crop = cropped(lum, res.box)
    cw, ch = len(crop[0]), len(crop)
    gx, gy = edge_profiles(crop)
    px, phx, sx, bx = detect_axis(gx)
    py, phy, sy, by = detect_axis(gy)
    res.grid_best = (bx, by)
    sat = integral(crop)
    if px is not None and py is not None:
        res.mode = "grid"
        res.grid = (px, py, phx, phy, sx, sy)
        native = sample_grid(crop, px, py, phx, phy)
        check_native(len(native[0]), len(native))
    else:
        res.mode = "box"
        res.grid = None
        out_h = cells
        out_w = max(1, (cw * out_h + ch // 2) // ch) if out_h > 0 else 0
        check_native(out_w, out_h)
        if out_w > cw or out_h > ch:
            raise LogoError("the picture is %dx%d, smaller than the %dx%d native size it was asked for" % (cw, ch, out_w, out_h))
        native = resample_box(sat, cw, ch, out_w, out_h)
    res.crop_size = (cw, ch)
    res.native = native
    res.width, res.height = len(native[0]), len(native)
    name, fid, bits, codes, table = pick_format(native)
    res.format, res.format_id, res.bits, res.formats = name, fid, bits, table
    res.scale = pick_scale(res.height)
    res.data, res.stride = pack(codes, bits)
    pad = (-len(res.data)) % 4
    res.data += bytes(pad)
    # OS-FLASHDIET A5: in flash the bitmap is a raw-deflate stream of these (whole-word) bytes, made by
    # tools/deflate_pack.py with the 4 KiB window -- never Python's zlib, whose output changes with the library.
    # boot_logo() hands the stream over; the boot upload and the few raw readers inflate it (core::Inflater).
    stats = {}
    res.z = deflate_pack.deflate(res.data, stats)
    if stats["max_distance"] > deflate_pack.WINDOW:
        raise LogoError("a match distance of %d, over the %d window" % (stats["max_distance"], deflate_pack.WINDOW))
    res.z_size = len(res.z)
    res.recon = [[c * 255 // ((1 << bits) - 1) for c in row] for row in codes]
    res.screen_rms = screen_error(sat, cw, ch, res.recon, res.scale)
    res.tone_rms = tone_error(sat, cw, ch, res.recon)
    return res


def describe(res):
    """The decisions, as lines of text (the report, and the comment block of the generated file)."""
    w, h, ctype = res.source
    x0, y0, x1, y1 = res.box
    lines = ["source: %dx%d, colour type %d, sha256 %s" % (w, h, ctype, res.sha256),
             "crop: %d,%d %dx%d (everything above luminance %d)" % (x0, y0, res.crop_size[0], res.crop_size[1], CROP_THRESHOLD)]
    if res.mode == "grid":
        px, py, phx, phy, sx, sy = res.grid
        lines.append("grid: found, block %dx%d phase %d,%d (%d and %d permille of the edge energy on the boundaries, at least %d)"
                     % (px, py, phx, phy, sx, sy, GRID_MIN_PERMILLE))
        lines.append("native: %dx%d blocks, the centre pixel of each" % (res.width, res.height))
    else:
        lines.append("grid: none (the best fit holds %d and %d permille of the edge energy, %d needed): not a regular pixel grid"
                     % (res.grid_best[0], res.grid_best[1], GRID_MIN_PERMILLE))
        lines.append("native: %dx%d, box filter over the central %d/%d of each cell" % (res.width, res.height, BOX_NUM, BOX_DEN))
    lines.append("format: %s (bits %d, BITMAP_LAYOUT %d), stride %d, %d bytes (%d with the padding to a word)"
                 % (res.format, res.bits, res.format_id, res.stride, res.stride * res.height, len(res.data)))
    lines.append("stored: raw deflate (tools/deflate_pack.py, the %d-byte window), %d stream bytes for the %d raw bytes"
                 % (deflate_pack.WINDOW, res.z_size, len(res.data)))
    lines.append("quantisation rms against the native image: " + ", ".join("%s %d.%02d" % (n, r // 100, r % 100) for n, r in res.formats)
                 + " (a format is kept at %d.00 or less)" % FORMAT_MAX_RMS)
    lines.append("against the source: tone (mean of each %dx%d patch) rms %d.%02d; pixel by pixel at the shown size x%d (%dx%d px) rms %d.%02d,"
                 " which includes the dither's own contrast"
                 % (TONE_BLOCK, TONE_BLOCK, res.tone_rms // 100, res.tone_rms % 100, res.scale, res.width * res.scale,
                    res.height * res.scale, res.screen_rms // 100, res.screen_rms % 100))
    return lines


def emit_cpp(res, source_name):
    out = ["// Generated by tools/logo_convert.py from %s -- do not edit; a ctest (logo_convert_fresh) regenerates this file and fails when it differs." % source_name,
           "// The logo is the project's (docs/specs/OS-003-ui-toolkit.md, 003f); it is GPL-3.0-or-later with the rest of the tree.",
           "// SPDX-License-Identifier: GPL-3.0-or-later",
           "//"]
    out += ["// " + line for line in describe(res)]
    out += ["#include <o3os/ui/boot_logo.h>",
            "",
            "namespace o3os::ui {",
            "namespace {",
            "",
            "// OS-FLASHDIET A5: the bitmap lives in flash as a raw-deflate stream of its %d whole-word bytes" % len(res.data),
            "// (tools/deflate_pack.py, the 4 KiB window), inflated at boot by core::Inflater like a glyph face",
            "// (ui/src/assets.cpp). Word aligned like the faces' streams (ADR 0011): declared on the type and asserted.",
            "template <unsigned N>",
            "struct alignas(4) Stream {",
            "  std::uint8_t bytes[N];",
            "};",
            "",
            "// %dx%d %s, stride %d, rows top to bottom, the leftmost pixel in the most significant bits: %d raw bytes," % (res.width, res.height, res.format, res.stride, len(res.data)),
            "// %d stream bytes." % res.z_size]
    n = (res.z_size + 3) & ~3  # whole words, as deflate_pack.py's emit_cpp pads (ADR 0011)
    body = res.z + b"\x00" * (n - res.z_size)
    out.append("constexpr Stream<%d> k_boot_logo_z = {{" % n)
    for j in range(0, n, 16):
        out.append("    " + ", ".join("0x%02X" % b for b in body[j:j + 16]) + ",")
    raw = len(res.data)
    out += ["}};",
            "static_assert(alignof(Stream<%d>) >= 4 && sizeof(Stream<%d>) == %d, \"boot logo stream: word aligned, whole words\");" % (n, n, n),
            "static_assert(kBootLogoW == %d && kBootLogoH == %d, \"ui/boot_logo.h and tools/logo_convert.py disagree on the size: update the header\");" % (res.width, res.height),
            "static_assert(static_cast<unsigned>(kBootLogoFormat) == %d, \"ui/boot_logo.h and tools/logo_convert.py disagree on the format\");" % res.format_id,
            "static_assert(kBootLogoStride == %d && kBootLogoBytes == %d, \"ui/boot_logo.h and tools/logo_convert.py disagree on the layout\");" % (res.stride, raw),
            "static_assert(kBootLogoScale == %d, \"ui/boot_logo.h and tools/logo_convert.py disagree on the scale\");" % res.scale,
            "",
            "}  // namespace",
            "",
            "BootLogo boot_logo() noexcept { return BootLogo{k_boot_logo_z.bytes, %d, kBootLogoBytes}; }" % res.z_size,
            "",
            "}  // namespace o3os::ui",
            ""]
    return "\n".join(out)


# ---- A PNG encoder for the preview and the self-test ---------------------------------------------------------------------------

def filter_line(ftype, line, prev, bpp):
    """The forward filter of one scanline (the inverse of unfilter), for the self-test."""
    out = bytearray(len(line))
    for i in range(len(line)):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if ftype == 0:
            pred = 0
        elif ftype == 1:
            pred = a
        elif ftype == 2:
            pred = b
        elif ftype == 3:
            pred = (a + b) >> 1
        else:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
        out[i] = (line[i] - pred) & 255
    return out


def chunk(ctype, body):
    return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF)


def encode_png(width, height, colour_type, rows, filters=(0,), depth=8, interlace=0, bpp=None):
    """A PNG from rows of samples. `filters` is cycled over the scanlines; `bpp` (bytes a pixel) is the colour type's unless given."""
    bpp = bpp or CHANNELS.get(colour_type, 1)
    raw = bytearray()
    prev = bytearray(width * bpp)
    for y, row in enumerate(rows):
        ftype = filters[y % len(filters)]
        raw.append(ftype)
        raw += filter_line(ftype, bytearray(row), prev, bpp)
        prev = bytearray(row)
    ihdr = struct.pack(">IIBBBBB", width, height, depth, colour_type, 0, 0, interlace)
    return PNG_SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def write_preview(path, res):
    rows = []
    for row in res.recon:
        big = bytearray(v for v in row for _ in range(res.scale))
        rows += [big] * res.scale
    Path(path).write_bytes(encode_png(res.width * res.scale, res.height * res.scale, 0, rows, filters=(1, 2, 4)))


# ---- Self-test ---------------------------------------------------------------------------------------------------------------------

def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    def expect_error(what, fn):
        try:
            fn()
        except LogoError:
            return
        except Exception as e:  # anything but a clean LogoError is a bug
            failures.append("%s: raised %r, not LogoError" % (what, e))
            return
        failures.append("%s: accepted" % what)

    # -- The decoder: every colour type with every filter, round-trip exact. A small picture with gradients and edges, so a
    # wrong predictor does not cancel out.
    for ctype, chans in sorted(CHANNELS.items()):
        w, h = 13, 9
        rows = [bytes((x * 19 + y * 7 + c * 61 + (x * y) % 11) & 255 for x in range(w) for c in range(chans)) for y in range(h)]
        for ftype in range(5):
            png = encode_png(w, h, ctype, rows, filters=(ftype,))
            dw, dh, dct, drows = decode_png(png)
            check((dw, dh, dct) == (w, h, ctype), "decode type %d filter %d: header" % (ctype, ftype))
            check([bytes(r) for r in drows] == rows, "decode type %d filter %d: pixels" % (ctype, ftype))
        mixed = encode_png(w, h, ctype, rows, filters=(0, 1, 2, 3, 4, 4, 3, 2, 1))
        check([bytes(r) for r in decode_png(mixed)[3]] == rows, "decode type %d: every filter in one image" % ctype)
    # -- The decoder refuses what it cannot or must not take, cleanly.
    good = encode_png(4, 4, 0, [bytes(range(4))] * 4)
    expect_error("bad signature", lambda: decode_png(b"GIF89a" + good[6:]))
    expect_error("truncated", lambda: decode_png(good[:-5]))
    expect_error("empty", lambda: decode_png(b""))
    crc_broken = bytearray(good)
    crc_broken[20] ^= 0xFF  # inside IHDR
    expect_error("bad crc", lambda: decode_png(bytes(crc_broken)))
    expect_error("16-bit", lambda: decode_png(encode_png(4, 4, 0, [bytes(8)] * 4, depth=16, bpp=2)))
    expect_error("palette", lambda: decode_png(encode_png(4, 4, 3, [bytes(4)] * 4)))
    expect_error("interlaced", lambda: decode_png(encode_png(4, 4, 0, [bytes(4)] * 4, interlace=1)))
    expect_error("filter 9", lambda: decode_png(encode_png(4, 4, 0, [bytes(4)] * 4, filters=(9,))))
    short = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 7)) + chunk(b"IEND", b"")
    expect_error("short image data", lambda: decode_png(short))
    bomb = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 10_000_000)) + chunk(b"IEND", b"")
    expect_error("image data bomb", lambda: decode_png(bomb))
    # A picture that would take more than about 64 MiB to decode is refused from its header, before anything is allocated.
    check(MAX_SIDE * MAX_SIDE * 4 <= 64 * 1024 * 1024 and MAX_DECODED_BYTES == 64 * 1024 * 1024, "the decode bound is 64 MiB")
    huge_side = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", MAX_SIDE + 1, 1, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0")) + chunk(b"IEND", b"")
    expect_error("a side past MAX_SIDE", lambda: decode_png(huge_side))
    huge_area = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", MAX_SIDE, MAX_SIDE, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 100)) + chunk(b"IEND", b"")
    expect_error("4096 x 4096 RGBA with no data behind it (it is allowed by size, and not a PNG)", lambda: decode_png(huge_area))
    over = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", MAX_SIDE, MAX_SIDE, 8, 6, 0, 0, 0)) + chunk(b"IEND", b"")
    expect_error("no image data", lambda: decode_png(over))
    check(MAX_SIDE * MAX_SIDE * 4 == MAX_DECODED_BYTES, "4096 x 4096 RGBA is exactly at the bound, and so allowed by size")
    # Ancillary chunks are skipped and alpha is composited on black.
    extra = good[:33] + chunk(b"tEXt", b"Comment\0hello") + good[33:]
    check(decode_png(extra)[0] == 4, "an ancillary chunk is skipped")
    lum = luminance(2, 6, [bytearray([255, 255, 255, 255, 255, 255, 255, 0])])
    check(lum == [[255, 0]], "RGBA composites on black: %r" % lum)
    check(luminance(1, 4, [bytearray([200, 128])]) == [[100]], "gray + alpha composites on black")
    check(luminance(1, 0, [bytearray([77])]) == [[77]], "gray is its own luminance")
    check(luminance(1, 4, [bytearray([255, 255])]) == [[255]], "opaque gray + alpha is unchanged")
    check(luminance(1, 6, [bytearray([255, 255, 255, 255])]) == [[255]], "opaque white stays 255")
    check(luminance(1, 2, [bytearray([255, 255, 255])]) == [[255]], "RGB white is 255")
    check(luminance(1, 6, [bytearray([255, 0, 0, 255])]) == [[76]], "BT.601 red is 76: %r" % (luminance(1, 6, [bytearray([255, 0, 0, 255])]),))

    # -- The crop.
    pic = [[0] * 10 for _ in range(8)]
    pic[2][3] = pic[5][7] = 200
    check(crop_box(pic) == (3, 2, 8, 6), "crop box %r" % (crop_box(pic),))
    expect_error("blank picture", lambda: crop_box([[0] * 4] * 4))

    # -- The grid: a made-up pixel-art picture, scaled up by 7 with a margin and a phase, is found exactly and recovered.
    art = [[(x * 37 + y * 91 + (x * y) % 13 * 29) % 256 for x in range(20)] for y in range(16)]
    art = [[max(v, 30) for v in row] for row in art]   # nothing near black, so the crop keeps every block
    block, margin_x, margin_y = 7, 5, 3
    big = [[0] * (20 * block + 2 * margin_x) for _ in range(16 * block + 2 * margin_y)]
    for y in range(16 * block):
        for x in range(20 * block):
            big[margin_y + y][margin_x + x] = art[y // block][x // block]
    png = encode_png(len(big[0]), len(big), 0, [bytes(r) for r in big], filters=(4,))
    res = convert(png)
    check(res.mode == "grid", "a pixel-art picture upscaled by 7 is recognised as a grid (%s)" % res.mode)
    if res.mode == "grid":
        check(res.grid[:4] == (7, 7, 0, 0), "block 7 phase 0 after the crop: %r" % (res.grid,))
        check(res.native == art, "the native picture is the art, exactly")
        check(res.box == (margin_x, margin_y, margin_x + 20 * block, margin_y + 16 * block), "crop box of the scaled art %r" % (res.box,))
    # A larger block size within the tie margin wins: a picture whose runs are all 2 cells wide is also a grid of 2 x the block.
    wide = [[row[x // 2] for x in range(40)] for row in art for _ in range(2)]
    pw = encode_png(40 * block, 32 * block, 0, [bytes(v for v in r for _ in range(block)) for r in wide for _ in range(block)])
    rw = convert(pw)
    check(rw.mode == "grid" and rw.grid[0] == 2 * block, "the largest block that fits is taken: %r" % (rw.grid,))
    # A phase: the crop box is at a block boundary by construction, so shift the picture and its content by 3 blocks of garbage-free black.
    # Irregular "pixels" (the block size drifts from 7 to 11) are no grid: the fallback is taken and says so.
    xs = []
    for i in range(30):
        size = 7 + (i * 4) // 30
        xs += [i] * size
    ys = []
    for j in range(30):
        size = 7 + (j * 4) // 30
        ys += [j] * size
    cells = [[max(30, (i * 53 + j * 101 + (i * j) % 7 * 17) % 256) for i in range(30)] for j in range(30)]
    irregular = [bytes(cells[ys[y]][xs[x]] for x in range(len(xs))) for y in range(len(ys))]
    ri = convert(encode_png(len(xs), len(ys), 0, irregular), cells=30)
    check(ri.mode == "box", "a drifting block size is not accepted as a grid (best %r permille)" % (ri.grid_best,))
    check((ri.width, ri.height) == (30, 30), "the fallback follows the requested size %r" % ((ri.width, ri.height),))
    # Pictures under four native pixels are no logo: a clean LogoError, never a ZeroDivisionError.
    irregular_png = encode_png(len(xs), len(ys), 0, irregular)  # no grid: the fallback takes the size it is asked for
    tiny = encode_png(3, 3, 0, [bytes([200, 100, 200])] * 3)
    expect_error("a 3 x 3 picture", lambda: convert(tiny))
    expect_error("a 3 pixel native size", lambda: convert(irregular_png, cells=3))
    expect_error("a 0 pixel native size", lambda: convert(irregular_png, cells=0))
    expect_error("a negative native size", lambda: convert(irregular_png, cells=-5))
    expect_error("a native size larger than the picture", lambda: convert(encode_png(40, 40, 0, [bytes([200] * 40)] * 40), cells=124))
    expect_error("check_native 3 x 10", lambda: check_native(3, 10))
    expect_error("check_native 10 x 3", lambda: check_native(10, 3))
    check_native(TONE_BLOCK, TONE_BLOCK)
    expect_error("tone_error of a native picture under one patch", lambda: tone_error(integral([[9] * 8 for _ in range(8)]), 8, 8, [[9] * 3 for _ in range(3)]))
    # The edge detector alone: boundaries every 5 from phase 2 and nothing else.
    prof = [0] * 60
    for x in range(2, 60, 5):
        prof[x] = 100
    check(detect_axis(prof)[:3] == (5, 2, 1000), "detect_axis finds pitch 5 phase 2: %r" % (detect_axis(prof),))
    check(detect_axis([10] * 60)[0] is None, "a flat profile has no grid")
    check(detect_axis([0] * 60)[0] is None, "an empty profile has no grid")
    check(detect_axis([5] * 6)[0] is None, "a profile shorter than four blocks has no grid")

    # -- The fallback sampler: exact on a constant picture, and the central half of each cell on a pattern.
    flat = integral([[77] * 40 for _ in range(40)])
    check(resample_box(flat, 40, 40, 10, 10) == [[77] * 10 for _ in range(10)], "box filter keeps a constant picture")
    rows = [[0] * 4 + [255] * 4 for _ in range(8)]  # cells of 4 px across: the central half of cell 0 is 0, of cell 1 is 255
    check(resample_box(integral(rows), 8, 8, 2, 2) == [[0, 255], [0, 255]], "central half of each cell")
    edge = [[0, 0, 255, 255] for _ in range(4)]  # one cell of 4 px: the central half (px 1..3) averages 2 of 3 pixels... integer window
    check(resample_box(integral(edge), 4, 4, 1, 1)[0][0] == 128, "window mean rounds to nearest: %r" % (resample_box(integral(edge), 4, 4, 1, 1),))

    # -- The error measures: zero for a picture against itself, and what a known difference costs.
    check(tone_error(integral([[77] * 40 for _ in range(40)]), 40, 40, [[77] * 8 for _ in range(8)]) == 0, "tone error of an exact copy is 0")
    check(tone_error(integral([[77] * 40 for _ in range(40)]), 40, 40, [[87] * 8 for _ in range(8)]) == 1000, "tone error of a flat +10 is 10.00")
    check(rms_x100(0, 10) == 0 and rms_x100(36 * 10, 10) == 600 and rms_x100(2, 1) == 141, "rms in hundredths: %d" % rms_x100(2, 1))
    check(screen_error(integral([[100] * 20 for _ in range(20)]), 20, 20, [[100] * 4 for _ in range(4)], 2) == 0, "screen error of a flat copy is 0")

    # -- Quantisation and packing: levels, rounding, bit order, row padding.
    codes, recon = quantise([[0, 127, 128, 255]], 1)
    check(codes == [[0, 0, 1, 1]] and recon == [[0, 0, 255, 255]], "L1 levels %r" % (codes,))
    codes, recon = quantise([[0, 42, 43, 85, 128, 212, 213, 255]], 2)
    check(codes == [[0, 0, 1, 1, 2, 2, 3, 3]] and recon == [[0, 0, 85, 85, 170, 170, 255, 255]], "L2 levels %r %r" % (codes, recon))
    codes, recon = quantise([[0, 8, 9, 17, 255]], 4)
    check(codes == [[0, 0, 1, 1, 15]] and recon == [[0, 0, 17, 17, 255]], "L4 levels %r" % (codes,))
    check(pack([[1, 0, 1, 1, 0, 0, 0, 1, 1]], 1) == (bytes([0b10110001, 0b10000000]), 2), "L1 packs MSB first and pads the row")
    check(pack([[1, 2, 3, 0, 3]], 2) == (bytes([0b01101100, 0b11000000]), 2), "L2 packs two bits a pixel, first pixel on top")
    check(pack([[0xA, 0x5, 0xF]], 4) == (bytes([0xA5, 0xF0]), 2), "L4 packs the first pixel in the high nibble")
    check(pack([[1, 0], [0, 1]], 1)[0] == bytes([0x80, 0x40]), "one padded byte a row")
    # The format choice: the smallest that keeps the picture.
    two = [[0, 255] * 8 for _ in range(8)]
    check(pick_format(two)[0] == "L1", "a black and white picture is L1")
    four = [[0, 85, 170, 255] * 4 for _ in range(8)]
    check(pick_format(four)[0] == "L2", "a four-tone picture is L2")
    ramp = [[(x * 255) // 15 for x in range(16)] for _ in range(8)]
    check(pick_format(ramp)[0] == "L4", "sixteen tones are L4")
    smooth = [[(x * 255) // 63 for x in range(64)] for _ in range(8)]
    check(pick_format(smooth)[0] == "L4", "more tones than L4 holds: L4 is the last resort, not an error")
    check(pick_scale(124) == 3 and pick_scale(100) == 4 and pick_scale(190) == 2, "scales 124 -> 3, 100 -> 4, 190 -> 2")
    expect_error("no scale", lambda: pick_scale(250))
    expect_error("a native picture too small for scale 1", lambda: pick_scale(300))
    expect_error("a native picture taller than the screen's share", lambda: pick_scale(421))

    # -- The generated file: deterministic, aligned, tied to the header, and the staleness check sees a change.
    text = emit_cpp(res, "logo.png")
    check(text == emit_cpp(convert(png), "logo.png"), "the output is deterministic")
    zn = (res.z_size + 3) & ~3
    check(text.count("struct alignas(4) Stream") == 1 and ("constexpr Stream<%d> k_boot_logo_z" % zn) in text, "emit: one aligned stream array")
    check("static_assert(alignof(Stream<%d>) >= 4" % zn in text, "emit: alignment asserted")
    check("kBootLogoW == %d && kBootLogoH == %d" % (res.width, res.height) in text, "emit: the header's size is asserted")
    check("kBootLogoStride == %d && kBootLogoBytes == %d" % (res.stride, len(res.data)) in text, "emit: the header's raw layout is asserted")
    check("BootLogo{k_boot_logo_z.bytes, %d, kBootLogoBytes}" % res.z_size in text, "emit: boot_logo() returns the stream and the raw size")
    check(text.endswith("}  // namespace o3os::ui\n") and "\r" not in text, "emit: LF line ends, final newline")
    check(len(res.data) % 4 == 0 and zn % 4 == 0, "the raw bitmap and the stream are whole words")
    # -- OS-FLASHDIET A5: the stored stream is deflate_pack's (never zlib's), deterministic, inside the 4 KiB window,
    # and it inflates back to the exact raw bitmap (zlib is used only here, in the self-test).
    stats = {}
    z = deflate_pack.deflate(res.data, stats)
    check(z == res.z and z == deflate_pack.deflate(res.data), "the logo's stream is deflate_pack's and deterministic")
    check(stats["max_distance"] <= deflate_pack.WINDOW, "the logo's stream stays in the %d-byte window" % deflate_pack.WINDOW)
    check(zlib.decompress(z, -12) == res.data, "the logo's stream round-trips to the raw bitmap byte for byte")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "logo.png"
        out = Path(tmp) / "data.cpp"
        src.write_bytes(png)
        check(run_check(src, out, quiet=True) == 1, "--check: a missing file is stale")
        check(main_args(["--png", str(src), "--out", str(out)]) == 0 and out.exists(), "--out writes")
        check(run_check(src, out, quiet=True) == 0, "--check: a fresh file passes")
        out.write_text(out.read_text().replace("0x", "0X", 1))
        check(run_check(src, out, quiet=True) == 1, "--check: an edited file is stale")
        main_args(["--png", str(src), "--out", str(out)])
        src.write_bytes(encode_png(len(big[0]), len(big), 0, [bytes(r) for r in big], filters=(0,)))  # same pixels, other bytes
        check(run_check(src, out, quiet=True) == 1, "--check: a changed source (same picture, other file) is stale: the hash is part of the output")
        prev_png = Path(tmp) / "preview.png"
        src.write_bytes(png)
        check(main_args(["--png", str(src), "--preview", str(prev_png)]) == 0 and decode_png(prev_png.read_bytes())[0] == res.width * res.scale,
              "--preview writes a PNG of the screen size")

    print("logo_convert self-test: %s" % ("FAILED" if failures else "ok"))
    for f in failures:
        print("  " + f)
    return 1 if failures else 0


# ---- Command line ---------------------------------------------------------------------------------------------------------------------

def run_check(png_path, cpp_path, quiet=False):
    res = convert(Path(png_path).read_bytes())
    fresh = emit_cpp(res, "ui/assets/boot_logo.png")
    try:
        committed = Path(cpp_path).read_text()
    except OSError:
        if not quiet:
            print("logo_convert: %s is missing: run tools/logo_convert.py --out" % cpp_path, file=sys.stderr)
        return 1
    if committed != fresh:
        if not quiet:
            print("logo_convert: %s is stale (the converter or %s changed): regenerate it with\n"
                  "  python3 -I tools/logo_convert.py --png %s --out %s" % (cpp_path, png_path, png_path, cpp_path), file=sys.stderr)
        return 1
    return 0


def main_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--png", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", type=Path)
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--cells", type=int, default=FALLBACK_CELLS)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.png or not (args.out or args.check or args.report or args.preview):
        parser.error("--png and one of --out, --check, --report, --preview are required")
    try:
        if args.check:
            return run_check(args.png, args.check)
        res = convert(args.png.read_bytes(), args.cells)
    except (LogoError, OSError) as e:
        print("logo_convert: %s" % e, file=sys.stderr)
        return 1
    if args.report:
        print("\n".join(describe(res)))
    if args.preview:
        write_preview(args.preview, res)
    if args.out:
        text = emit_cpp(res, "ui/assets/boot_logo.png")
        args.out.parent.mkdir(parents=True, exist_ok=True)
        if not args.out.exists() or args.out.read_text() != text:  # keep the mtime when nothing changed
            args.out.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main_args(sys.argv[1:]))
