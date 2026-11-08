#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-FLASHDIET A2: our own deterministic raw-deflate (RFC 1951) encoder, for the image's compressed assets.

The image stores its assets (the built-in modules, the glyph faces, the boot logo, the help text) as raw
deflate streams with a 4 KiB window, decoded on the device by core::Inflater (docs/specs/OS-FLASHDIET.md
3.2). The streams are made at build time by this script, not by Python's zlib: zlib's deflate output changes
with the zlib library it links (measured on the same bytes: built against 1.2.11, running 1.3), and the
reproducible-image gate must not depend on the build machine's zlib. So the encoder here is written from
scratch, deterministic across machines, Python versions and runs, standard library only:

- greedy-lazy LZ77 over a 4 KiB window (a match distance never exceeds 4,096, what core::Inflater accepts),
- one dynamic-Huffman block per at most BLOCK_RAW input bytes, code lengths limited to 15 (7 for the
  code-length code), built with package-merge,
- a stored block instead when it is smaller.

zlib is used only in --self-test and --check-fixture, to check the streams round-trip through
zlib.decompress(data, -12) and to measure the encoder against zlib level 9 (reported, not gated: the gate
must not depend on the build machine's zlib). Later slices (A3 to A7) import this module and call deflate()
and emit_cpp().

Usage:
  deflate_pack.py --self-test [--check-fixture TEST_CPP]
                                                   (the ctest deflate_pack_selftest runs this)
  deflate_pack.py --check-fixture TEST_CPP        (deflate the kPackRaw fixture committed in TEST_CPP and
                                                   compare byte for byte with its kPackZ)
  deflate_pack.py INPUT                           (print raw size, stream size, ratio)
  deflate_pack.py --out FILE INPUT                (write the raw-deflate stream of INPUT to FILE)
  deflate_pack.py --emit-cpp SYMBOL --out FILE INPUT   (write emit_cpp(SYMBOL, raw, z): a word-aligned
                                                      const array plus raw_size and z_size)
"""
import argparse
import random
import re
import sys
import zlib
from pathlib import Path

WINDOW = 4096        # the decoder's window (core::Inflater): no match distance ever exceeds this
MIN_MATCH = 3
MAX_MATCH = 258
BLOCK_RAW = 16384    # raw bytes per block at most (the Huffman table is rebuilt per block)
MAX_CHAIN = 512      # match candidates tried per position
NICE_LEN = 192       # stop the search at a match this long
MAX_CODE_LEN = 15    # lit/len and distance code lengths (RFC 1951 limit)
MAX_CL_LEN = 7       # code-length code lengths (RFC 1951 limit)
SAMPLE_BUDGET = 3420  # the self-test's absolute byte budget for the three built-in samples: their exact total
                     # today. The encoder is deterministic, so a different total is a deliberate encoder change
                     # (the zlib level 9 ratio is only reported, never gated).

# RFC 1951 3.2.5, symbols 257..285 and 0..29.
LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
               67, 83, 99, 115, 131, 163, 195, 227, 258]
LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                4, 4, 4, 4, 5, 5, 5, 5, 0]
DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
             1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577]
DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
              9, 9, 10, 10, 11, 11, 12, 12, 13, 13]
CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]


def _build_lookups():
    """length (3..258) -> length symbol, distance (1..WINDOW) -> distance symbol (largest symbol first,
    so 258 maps to 285 and 4096 to 23, as RFC 1951 intends)."""
    len_sym = [0] * 259
    for s in range(285, 256, -1):
        base, extra = LENGTH_BASE[s - 257], LENGTH_EXTRA[s - 257]
        for length in range(base + (1 << extra) - 1, base - 1, -1):
            if 3 <= length <= 258 and len_sym[length] == 0:
                len_sym[length] = s
    dist_sym = [0] * (WINDOW + 1)
    for s in range(29, -1, -1):
        base, extra = DIST_BASE[s], DIST_EXTRA[s]
        for dist in range(min(WINDOW, base + (1 << extra) - 1), base - 1, -1):
            if 1 <= dist <= WINDOW and dist_sym[dist] == 0:
                dist_sym[dist] = s
    return len_sym, dist_sym


LEN_SYM, DIST_SYM = _build_lookups()
assert LEN_SYM[3] == 257 and LEN_SYM[258] == 285 and LEN_SYM[257] == 284
assert DIST_SYM[1] == 0 and DIST_SYM[4096] == 23 and DIST_SYM[4] == 3


class BitWriter:
    """Packs a deflate bitstream: write() emits LSB-first (headers, extra bits), code() MSB-first
    (Huffman codes, RFC 1951 3.1.1)."""

    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.nbits = 0
        self.total = 0  # bits ever written, alignment padding included, for per-block size comparisons

    def write(self, value, n):
        self.acc |= (value & ((1 << n) - 1)) << self.nbits
        self.nbits += n
        self.total += n
        while self.nbits >= 8:
            self.buf.append(self.acc & 0xFF)
            self.acc >>= 8
            self.nbits -= 8

    def code(self, value, n):
        rev = 0
        for i in range(n):
            rev = (rev << 1) | ((value >> i) & 1)
        self.write(rev, n)

    def align(self):
        if self.nbits:
            self.total += 8 - self.nbits  # the padding to the byte boundary is part of the bit position
            self.buf.append(self.acc & 0xFF)
            self.acc = 0
            self.nbits = 0

    def snapshot(self):
        return (len(self.buf), self.acc, self.nbits, self.total)

    def restore(self, snap):
        length, acc, nbits, total = snap
        del self.buf[length:]
        self.acc, self.nbits, self.total = acc, nbits, total

    def out(self):
        self.align()
        return bytes(self.buf)


def _hash3(raw, i):
    return (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2]


def _insert(heads, raw, i):
    if i + 2 < len(raw):
        heads.setdefault(_hash3(raw, i), []).append(i)


def _search(raw, heads, i):
    """The longest match for position i, as (length, distance); (0, 0) when none reaches MIN_MATCH.
    Candidates come most recent first, so a tie keeps the smallest distance; the chain stops at
    MAX_CHAIN candidates, at a match of NICE_LEN or more, and at the window's edge."""
    n = len(raw)
    if i + 2 >= n:
        return (0, 0)
    chain = heads.get(_hash3(raw, i))
    if not chain:
        return (0, 0)
    max_len = min(MAX_MATCH, n - i)
    limit = i - WINDOW
    best_len = 0
    best_dist = 0
    steps = MAX_CHAIN
    for pos in reversed(chain):
        if pos >= i:
            continue
        if pos < limit:
            break
        steps -= 1
        if steps <= 0:
            break
        if best_len and (pos + best_len >= n or raw[pos + best_len] != raw[i + best_len]):
            continue  # cannot beat the best length
        length = 0
        while length < max_len and raw[i + length] == raw[pos + length]:
            length += 1
        if length > best_len:
            best_len = length
            best_dist = i - pos
            if length >= max_len or length >= NICE_LEN:
                break
    if best_len < MIN_MATCH:
        return (0, 0)
    return (best_len, best_dist)


def _lz77(raw):
    """Greedy-lazy LZ77 over the WINDOW-byte window: ('lit', byte) and ('match', length, distance) tokens."""
    heads = {}
    i = 0
    pending = (0, 0)  # a match already found at the current position
    n = len(raw)
    while i < n:
        _insert(heads, raw, i)
        if pending[0] >= MIN_MATCH:
            best_len, best_dist = pending
            pending = (0, 0)
        else:
            best_len, best_dist = _search(raw, heads, i)
            if MIN_MATCH <= best_len < MAX_MATCH and i + 1 < n:
                nxt_len, nxt_dist = _search(raw, heads, i + 1)
                if nxt_len > best_len:  # lazy: the next position matches better, emit this byte raw
                    yield ("lit", raw[i])
                    i += 1
                    pending = (nxt_len, nxt_dist)
                    continue
        if best_len >= MIN_MATCH:
            yield ("match", best_len, best_dist)
            for k in range(i + 1, min(i + best_len, n)):
                _insert(heads, raw, k)
            i += best_len
        else:
            yield ("lit", raw[i])
            i += 1


def _limited_lengths(freqs, limit):
    """Length-limited Huffman code lengths by package-merge (optimal for the limit). Deterministic:
    ties are broken by symbol. Symbols with frequency 0 get length 0."""
    syms = [i for i, f in enumerate(freqs) if f > 0]
    lengths = [0] * len(freqs)
    if not syms:
        return lengths
    if len(syms) == 1:
        lengths[syms[0]] = 1
        return lengths
    leaves = sorted((freqs[i], (i,)) for i in syms)
    lists = [leaves]
    for _ in range(limit - 1):
        prev = lists[-1]
        packaged = [(prev[k][0] + prev[k + 1][0], prev[k][1] + prev[k + 1][1])
                    for k in range(0, len(prev) - 1, 2)]
        lists.append(sorted(packaged + leaves))
    for _, symbols in lists[-1][:2 * len(syms) - 2]:
        for s in symbols:
            lengths[s] += 1
    return lengths


def _force_two(freqs, count):
    """At least two used symbols, as zlib's encoder guarantees (a tree of one code is not complete)."""
    used = [i for i in range(count) if freqs[i] > 0]
    for i in range(count):
        if len(used) >= 2:
            break
        if freqs[i] == 0:
            freqs[i] = 1
            used.append(i)


def _canonical(lengths):
    """Canonical Huffman codes (RFC 1951 3.2.2) from code lengths."""
    max_len = max(lengths)
    bl_count = [0] * (max_len + 1)
    for l in lengths:
        if l:
            bl_count[l] += 1
    next_code = [0] * (max_len + 1)
    code = 0
    for bits in range(1, max_len + 1):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    codes = [0] * len(lengths)
    for sym, l in enumerate(lengths):
        if l:
            codes[sym] = next_code[l]
            next_code[l] += 1
    return codes


def _rle(seq):
    """The code lengths of a tree as (symbol, extra value, extra bits) triples, with 16/17/18 runs."""
    out = []
    i, n = 0, len(seq)
    while i < n:
        v = seq[i]
        run = 1
        while i + run < n and seq[i + run] == v:
            run += 1
        i += run
        if v == 0:
            while run >= 11:
                r = min(run, 138)
                out.append((18, r - 11, 7))
                run -= r
            if run >= 3:
                out.append((17, run - 3, 3))
                run = 0
            while run > 0:
                out.append((0, 0, 0))
                run -= 1
        else:
            out.append((v, 0, 0))
            run -= 1
            while run >= 3:
                r = min(run, 6)
                out.append((16, r - 3, 2))
                run -= r
            while run > 0:
                out.append((v, 0, 0))
                run -= 1
    return out


def _emit_code_lengths(seq, w, cl_lengths, cl_codes):
    for sym, extra, extra_bits in _rle(seq):
        w.code(cl_codes[sym], cl_lengths[sym])
        if extra_bits:
            w.write(extra, extra_bits)


def _freq_tables(tokens):
    """The lit/len and distance frequency tables of a block's tokens: the end-of-block symbol counts, and
    at least two symbols are used (a tree of one code is not complete)."""
    lit_freq = [0] * 288
    dist_freq = [0] * 30
    for t in tokens:
        if t[0] == "lit":
            lit_freq[t[1]] += 1
        else:
            lit_freq[LEN_SYM[t[1]]] += 1
            dist_freq[DIST_SYM[t[2]]] += 1
    lit_freq[256] += 1  # end of block
    _force_two(lit_freq, 286)  # 286 and 287 never occur
    _force_two(dist_freq, 30)
    return lit_freq, dist_freq


def _code_lengths(lit_freq, dist_freq):
    """(lit_lengths, dist_lengths, hlit, hdist, seq): the code lengths limited to MAX_CODE_LEN, the HLIT /
    HDIST counts, and the lit/len + distance sequence a dynamic block header sends them as (cut at the
    last used symbol, but at least HLIT 257 and HDIST 1)."""
    lit_lengths = _limited_lengths(lit_freq, MAX_CODE_LEN)
    dist_lengths = _limited_lengths(dist_freq, MAX_CODE_LEN)
    hlit = max(257, max(i for i, l in enumerate(lit_lengths) if l) + 1)
    hdist = max(1, max(i for i, l in enumerate(dist_lengths) if l) + 1)
    return lit_lengths, dist_lengths, hlit, hdist, lit_lengths[:hlit] + dist_lengths[:hdist]


def _emit_dynamic(w, tokens, final):
    lit_freq, dist_freq = _freq_tables(tokens)
    lit_lengths, dist_lengths, hlit, hdist, seq = _code_lengths(lit_freq, dist_freq)

    cl_freq = [0] * 19
    for sym, _, _ in _rle(seq):
        cl_freq[sym] += 1
    _force_two(cl_freq, 19)
    cl_lengths = _limited_lengths(cl_freq, MAX_CL_LEN)
    cl_codes = _canonical(cl_lengths)
    hclen = 4
    for k in range(19):
        if cl_lengths[CL_ORDER[k]]:
            hclen = max(hclen, k + 1)

    lit_codes = _canonical(lit_lengths)
    dist_codes = _canonical(dist_lengths)
    w.write(final, 1)
    w.write(2, 2)  # BTYPE = dynamic
    w.write(hlit - 257, 5)
    w.write(hdist - 1, 5)
    w.write(hclen - 4, 4)
    for k in range(hclen):
        w.write(cl_lengths[CL_ORDER[k]], 3)
    _emit_code_lengths(seq, w, cl_lengths, cl_codes)
    for t in tokens:
        if t[0] == "lit":
            w.code(lit_codes[t[1]], lit_lengths[t[1]])
        else:
            ls = LEN_SYM[t[1]]
            w.code(lit_codes[ls], lit_lengths[ls])
            w.write(t[1] - LENGTH_BASE[ls - 257], LENGTH_EXTRA[ls - 257])
            ds = DIST_SYM[t[2]]
            w.code(dist_codes[ds], dist_lengths[ds])
            w.write(t[2] - DIST_BASE[ds], DIST_EXTRA[ds])
    w.code(lit_codes[256], lit_lengths[256])


def _emit_stored(w, data, final):
    w.write(final, 1)
    w.write(0, 2)  # BTYPE = stored
    w.align()
    w.write(len(data) & 0xFF, 8)
    w.write((len(data) >> 8) & 0xFF, 8)
    nlen = (~len(data)) & 0xFFFF
    w.write(nlen & 0xFF, 8)
    w.write((nlen >> 8) & 0xFF, 8)
    for b in data:
        w.write(b, 8)


def deflate(raw: bytes, stats: dict = None) -> bytes:
    """The raw-deflate stream of raw (a 4 KiB window, deterministic). When stats is a dict it receives
    max_distance (never over WINDOW) and blocks."""
    tokens = list(_lz77(raw))
    blocks = []
    cur, cur_raw, pos = [], 0, 0
    for t in tokens:
        cur.append(t)
        cur_raw += t[1] if t[0] == "match" else 1
        if cur_raw >= BLOCK_RAW:
            blocks.append((raw[pos:pos + cur_raw], cur))
            pos += cur_raw
            cur, cur_raw = [], 0
    if cur:
        blocks.append((raw[pos:pos + cur_raw], cur))

    w = BitWriter()
    max_dist = 0
    for bi, (bslice, btokens) in enumerate(blocks):
        final = 1 if bi == len(blocks) - 1 else 0
        snap = w.snapshot()
        _emit_dynamic(w, btokens, final)
        dyn_bits = w.total - snap[3]
        stored_bits = 3 + ((8 - (snap[3] + 3) % 8) % 8) + 32 + 8 * len(bslice)
        if stored_bits < dyn_bits:  # a stored block when it is smaller
            w.restore(snap)
            _emit_stored(w, bslice, final)
        for t in btokens:
            if t[0] == "match" and t[2] > max_dist:
                max_dist = t[2]
    if not blocks:
        _emit_stored(w, b"", 1)
    if stats is not None:
        stats["max_distance"] = max_dist
        stats["blocks"] = max(1, len(blocks))
    return w.out()


def emit_cpp(symbol: str, raw: bytes, z: bytes) -> str:
    """A word-aligned const array of the stream plus raw_size and z_size, in the style of
    logo_convert.py's emit_cpp (ADR 0011: whole words)."""
    n = (len(z) + 3) & ~3
    body = z + b"\x00" * (n - len(z))
    out = ["// Generated by tools/deflate_pack.py -- do not edit.",
           "// SPDX-License-Identifier: GPL-3.0-or-later",
           "",
           "#include <cstddef>",
           "#include <cstdint>",
           "",
           "namespace {",
           "",
           "template <unsigned N>",
           "struct alignas(4) Blob {",
           "  std::uint8_t bytes[N];",
           "};",
           "",
           "// The raw-deflate stream of the %d raw bytes (the 4 KiB window), padded to whole words." % len(raw),
           "constexpr Blob<%d> k%s = {{" % (n, symbol)]
    for j in range(0, n, 16):
        out.append("    " + ", ".join("0x%02X" % b for b in body[j:j + 16]) + ",")
    out += ["}};",
            "static_assert(alignof(Blob<%d>) >= 4 && sizeof(Blob<%d>) == %d, "
            "\"deflate_pack: word aligned, whole words\");" % (n, n, n),
            "inline constexpr std::size_t k%sRawSize = %d;" % (symbol, len(raw)),
            "inline constexpr std::size_t k%sZSize = %d;" % (symbol, len(z)),
            "",
            "}  // namespace",
            ""]
    return "\n".join(out)


def _zlib9(raw: bytes) -> bytes:
    c = zlib.compressobj(9, zlib.DEFLATED, -12)
    return c.compress(raw) + c.flush()


def _cpp_byte_array(text, name):
    """The bytes of a `constexpr std::uint8_t NAME[] = { ... };` array in a C++ source, or None."""
    m = re.search(r"constexpr std::uint8_t %s\[\] = \{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        return None
    return bytes(int(x) for x in re.findall(r"\d+", m.group(1)))


def _fixture_failures(test_cpp):
    """The committed-fixture check as failure strings: deflate() the kPackRaw array committed in test_cpp
    and compare byte for byte with its kPackZ (the fixture tests/core_inflate_test.cpp carries)."""
    text = Path(test_cpp).read_text()
    raw = _cpp_byte_array(text, "kPackRaw")
    z = _cpp_byte_array(text, "kPackZ")
    if raw is None or z is None:
        return ["%s carries no kPackRaw/kPackZ fixture arrays" % test_cpp]
    out = []
    regen = deflate(raw)
    if regen != z:
        out.append("the fixture: deflate(kPackRaw) is %d bytes, the committed kPackZ is %d bytes"
                   % (len(regen), len(z)))
    try:
        if zlib.decompress(z, -12) != raw:
            out.append("the committed kPackZ does not inflate to kPackRaw through zlib.decompress(data, -12)")
    except zlib.error as e:
        out.append("zlib.decompress(data, -12) refuses the committed kPackZ: %s" % e)
    return out


def check_fixture(test_cpp):
    """--check-fixture alone: 0 when deflate(kPackRaw) is the kPackZ committed in test_cpp, byte for byte."""
    failures = _fixture_failures(test_cpp)
    print("deflate_pack fixture check: %s (%s)" % ("FAILED" if failures else "ok", test_cpp))
    for f in failures:
        print("  " + f)
    return 1 if failures else 0


def _samples():
    """The built-in samples of the self-test: a text, a bitmap, a code blob, all made here."""
    line = (b"PATTERN  the grid of steps: UP and DOWN move the cursor, the note keys play and insert, "
            b"DEL clears the step under the cursor, SHIFT with a note mutes the whole row. ")
    text = bytearray()
    for page in range(12):
        text += b"== HELP PAGE %d: PATTERN, SONG, MIXER ==\n" % page
        for para in range(7):
            text += b"| " + line + b"\n"
        text += b"\n"
    bitmap = bytearray()  # a glyph-face-like L1 bitmap: 8x16 cells, mostly blank rows
    rng = random.Random(20261105)
    for cell in range(96):
        for row in range(16):
            if row < 2 or row > 13:
                bitmap += b"\x00\x00"
            else:
                bitmap += bytes((rng.getrandbits(8) & 0xF8, rng.getrandbits(8) & 0x1F))
    code = bytearray()  # a code blob: Thumb-ish opcodes with repeated 32-byte prologues
    prologue = bytes((0xB5, 0x00, 0x46, 0xF0, 0x01, 0x20, 0xBD, 0x00)) * 4
    for block in range(48):
        code += prologue if block % 4 == 0 else bytes(rng.getrandbits(8) for _ in range(32))
    return {"text": bytes(text), "bitmap": bytes(bitmap), "code": bytes(code)}


def self_test(fixture_cpp=None):
    """The --self-test run; fixture_cpp, when given (--check-fixture), is checked as one more step."""
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    rng = random.Random(20261105)
    fixed = {
        "empty": b"",
        "one_byte": b"x",
        "two_bytes": b"ab",
        "run_10k": b"A" * 10240,
        "pattern_4k": bytes((i * 7 + (i >> 3)) & 0xFF for i in range(4096)),
        "text_repeat": b"the tracker's own words: pattern, instrument, song, step, row. " * 64,
    }
    random_inputs = {"random_%d" % size: bytes(rng.getrandbits(8) for _ in range(size))
                     for size in (2, 3, 17, 100, 4095, 4096, 4097, 20000, 65536)}
    samples = _samples()

    max_dist_seen = 0
    for name, raw in list(fixed.items()) + list(random_inputs.items()) + list(samples.items()):
        z = deflate(raw)
        check(deflate(raw) == z, "%s: two runs give different bytes" % name)
        stats = {}
        check(deflate(raw, stats) == z, "%s: the stats run differs" % name)
        check(stats["max_distance"] <= WINDOW,
              "%s: a distance of %d, over the %d window" % (name, stats["max_distance"], WINDOW))
        max_dist_seen = max(max_dist_seen, stats["max_distance"])
        try:
            back = zlib.decompress(z, -12)
        except zlib.error as e:
            check(False, "%s: zlib.decompress(data, -12) refuses the stream: %s" % (name, e))
            continue
        check(back == raw, "%s: the round trip through zlib.decompress(data, -12) differs" % name)
    check(max_dist_seen <= WINDOW, "a distance over the window somewhere (%d)" % max_dist_seen)

    # Length limiting (package-merge): Fibonacci frequencies are the worst case for code lengths (the
    # unlimited tree is a comb), so they must still give a complete code (Kraft sum 1) within the limit.
    def fib(n):
        out, a, b = [], 1, 1
        for _ in range(n):
            out.append(a)
            a, b = b, a + b
        return out

    for limit in (15, 7):
        lengths = _limited_lengths(fib(17), limit)
        used = [l for l in lengths if l]
        check(len(used) == 17, "_limited_lengths(limit %d): a used symbol got length 0" % limit)
        check(max(lengths) <= limit,
              "_limited_lengths(limit %d): a length of %d, over the limit" % (limit, max(lengths)))
        check(sum(1 << (limit - l) for l in used) == (1 << limit),
              "_limited_lengths(limit %d): the Kraft sum is not 1" % limit)

    # An input with no matches at all: no 3-gram of 0..255 repeats, so the block is all literals.
    raw_nomatch = bytes(range(256))
    check(all(t[0] == "lit" for t in _lz77(raw_nomatch)),
          "bytes(range(256)) produced a match, but no 3-gram of it repeats")
    try:
        check(zlib.decompress(deflate(raw_nomatch), -12) == raw_nomatch,
              "the no-match input does not round-trip through zlib.decompress(data, -12)")
    except zlib.error as e:
        check(False, "the no-match input: zlib.decompress(data, -12) refuses the stream: %s" % e)

    # An input whose code lengths carry a run of 138+ zeros, what the RLE symbol 18 (a run capped at 138)
    # exists for: one literal byte, so lit/len symbols 6..255 all get length 0.
    raw_sparse = b"\x05" * 9000
    lit_freq, dist_freq = _freq_tables(list(_lz77(raw_sparse)))
    seq = _code_lengths(lit_freq, dist_freq)[-1]
    check(any(s == 18 and extra >= 127 for s, extra, _ in _rle(seq)),
          "the sparse input produced no 138+ run of zero code lengths")
    try:
        check(zlib.decompress(deflate(raw_sparse), -12) == raw_sparse,
              "the sparse input does not round-trip through zlib.decompress(data, -12)")
    except zlib.error as e:
        check(False, "the sparse input: zlib.decompress(data, -12) refuses the stream: %s" % e)

    # The committed fixture: deflate its kPackRaw and compare byte for byte with its kPackZ.
    if fixture_cpp is not None:
        failures += _fixture_failures(fixture_cpp)

    # The built-in samples against a pinned absolute byte budget (the encoder is deterministic, so the
    # budget does not depend on the build machine's zlib; the zlib level 9 ratio is only reported).
    total = sum(len(deflate(s)) for s in samples.values())
    ztotal = sum(len(_zlib9(s)) for s in samples.values())
    check(total <= SAMPLE_BUDGET,
          "the built-in samples: %d bytes, over the pinned budget of %d" % (total, SAMPLE_BUDGET))

    text = emit_cpp("Sample", samples["text"], deflate(samples["text"]))
    check("struct alignas(4) Blob" in text and "constexpr Blob<" in text, "emit_cpp: the aligned array")
    check("kSampleRawSize = %d" % len(samples["text"]) in text, "emit_cpp: raw_size")
    check("kSampleZSize = %d" % len(deflate(samples["text"])) in text, "emit_cpp: z_size")
    check(text.count("static_assert(alignof(Blob<") == 1, "emit_cpp: the alignment is asserted")

    print("deflate_pack self-test: %s (%d inputs round-tripped through zlib.decompress(data, -12), "
          "max distance %d, samples %d bytes of the %d budget, zlib -9's %d = %.2f%%%s)"
          % ("FAILED" if failures else "ok", len(fixed) + len(random_inputs) + len(samples),
             max_dist_seen, total, SAMPLE_BUDGET, ztotal, 100.0 * total / ztotal,
             ", fixture pinned" if fixture_cpp is not None else ""))
    for f in failures:
        print("  " + f)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", type=Path, nargs="?", help="the file to deflate")
    parser.add_argument("--out", type=Path, help="where to write (the stream, or the C++ with --emit-cpp)")
    parser.add_argument("--emit-cpp", metavar="SYMBOL",
                        help="emit a word-aligned const array of the stream plus raw_size and z_size")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--check-fixture", metavar="TEST_CPP",
                        help="deflate the kPackRaw fixture committed in TEST_CPP and compare byte for "
                             "byte with its kPackZ (with --self-test: a self-test step; alone: the run)")
    args = parser.parse_args()
    if args.self_test:
        return self_test(args.check_fixture)
    if args.check_fixture:
        return check_fixture(args.check_fixture)
    if not args.input:
        parser.error("input is required (or --self-test)")
    raw = args.input.read_bytes()
    z = deflate(raw)
    if args.emit_cpp:
        text = emit_cpp(args.emit_cpp, raw, z)
        if args.out:
            args.out.write_text(text)
        else:
            sys.stdout.write(text)
    elif args.out:
        args.out.write_bytes(z)
    else:
        print("%s: %d raw bytes -> %d stream bytes (%.1f%%)"
              % (args.input, len(raw), len(z), 100.0 * len(z) / max(1, len(raw))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
