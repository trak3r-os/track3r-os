#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-FLASHDIET A1: generate the core::Inflater test fixtures at configure time.

zlib's deflate output changes between zlib library versions, so the fixtures are never committed: CMake
runs this script at configure time (tests/CMakeLists.txt) and it writes a header with every good stream
-- zlib.compress(..., wbits=-12), the raw-deflate form with the 4 KiB window the image's encoder (A2) will
use -- next to its raw bytes, plus the malformed streams the host tests expect to end failed(). The good
streams come from zlib; the malformed ones are crafted bit by bit here, because zlib would never make
them.

Usage: inflate_fixtures.py --root <repo root> --out <generated header>
"""
import argparse
import random
import zlib
from pathlib import Path


def deflate12(raw: bytes, level: int = 9, strategy: int = zlib.Z_DEFAULT_STRATEGY) -> bytes:
    """Raw deflate with a 4 KiB window (wbits=-12), exactly what the build's encoder produces."""
    c = zlib.compressobj(level, zlib.DEFLATED, -12, 9, strategy)
    return c.compress(raw) + c.flush()


class BitWriter:
    """Packs a deflate bitstream: write() emits LSB-first (headers, extra bits), code() MSB-first
    (Huffman codes, RFC 1951 3.1.1)."""

    def __init__(self) -> None:
        self._bits: list[int] = []

    def write(self, value: int, n: int) -> None:
        for i in range(n):
            self._bits.append((value >> i) & 1)

    def code(self, value: int, n: int) -> None:
        for i in range(n - 1, -1, -1):
            self._bits.append((value >> i) & 1)

    def out(self) -> bytes:
        data = bytearray()
        acc = 0
        k = 0
        for bit in self._bits:
            acc |= bit << k
            k += 1
            if k == 8:
                data.append(acc)
                acc = 0
                k = 0
        if k:
            data.append(acc)
        return bytes(data)


def fixed_lit(w: BitWriter, sym: int) -> None:
    """The fixed literal/length code of a symbol (RFC 1951 3.2.6)."""
    if sym < 144:
        w.code(0x30 + sym, 8)
    elif sym < 256:
        w.code(0x190 + sym - 144, 9)
    elif sym < 280:
        w.code(sym - 256, 7)
    else:
        w.code(0xC0 + sym - 280, 8)


def good_fixtures(root: Path) -> list[tuple[str, bytes, bytes]]:
    """(name, raw bytes, raw-deflate stream) pairs; each stream must inflate to its raw bytes."""
    fixtures: list[tuple[str, bytes, bytes]] = []

    text = (b"the tracker's own words: pattern, instrument, song, step, row, effect column, "
            b"sample, note, key, screen, page. " * 8)
    fixtures.append(("stored", text, deflate12(text, 0)))  # level 0 emits stored blocks

    small = b"OS-FLASHDIET A1: one small fixed-Huffman block.\n"
    fixtures.append(("fixed", small, deflate12(small, 9, zlib.Z_FIXED)))

    dyn = bytes((i * 7 + (i >> 3)) & 0xFF for i in range(4096))
    fixtures.append(("dynamic", dyn, deflate12(dyn, 9)))

    fixtures.append(("empty", b"", deflate12(b"", 9)))
    fixtures.append(("one_byte", b"x", deflate12(b"x", 9)))

    # 64 KiB of random bytes: incompressible, so zlib falls back to stored blocks (more than one, a
    # stored block holds at most 65535 bytes). Deterministic across machines and Python versions.
    rng = random.Random(20250607)
    rnd = bytes(rng.getrandbits(8) for _ in range(65536))
    fixtures.append(("random_64k", rnd, deflate12(rnd, 9)))

    run = b"A" * 10240  # long matches at the window's edge (distance 4096)
    fixtures.append(("run_10k", run, deflate12(run, 9)))

    font = root / "third_party/fonts/terminus/ter-u20b.bdf"  # the Data face the image uploads
    if font.is_file():
        data = font.read_bytes()[:65536]
        fixtures.append(("font_bytes", data, deflate12(data, 9)))
    return fixtures


def bad_fixtures(goods: list[tuple[str, bytes, bytes]]) -> list[tuple[str, bytes, int]]:
    """(name, stream, raw_size to declare to begin()) pairs; each must end failed()."""
    bads: list[tuple[str, bytes, int]] = []
    _, dyn_raw, dyn_z = next(g for g in goods if g[0] == "dynamic")

    bads.append(("truncated_tail", dyn_z[:-1], len(dyn_raw)))
    bads.append(("truncated_half", dyn_z[: len(dyn_z) // 2], len(dyn_raw)))

    payload = bytes((i * 31 + 7) & 0xFF for i in range(64))
    bads.append(("overlong_output", deflate12(payload, 9), 32))  # 64 bytes of output, 32 declared
    bads.append(("overlong_trailing", dyn_z + b"\x00", len(dyn_raw)))  # one byte after the final block

    w = BitWriter()  # BTYPE 3 is reserved
    w.write(1, 1)
    w.write(3, 2)
    w.write(0, 5)
    bads.append(("bad_block_type", w.out(), 1))

    w = BitWriter()  # a dynamic header whose code-length code is over-subscribed (three 1-bit codes)
    w.write(1, 1)   # BFINAL
    w.write(2, 2)   # BTYPE = dynamic
    w.write(0, 5)   # HLIT = 257
    w.write(0, 5)   # HDIST = 1
    w.write(15, 4)  # HCLEN = 19
    for i in range(19):
        w.write(1 if i < 3 else 0, 3)  # lengths of symbols 16, 17, 18, 0, 8, ... in that order
    w.write(0, 8)
    bads.append(("bad_huffman_table", w.out(), 1))

    # A fixed block with 5000 literals and then a match at distance 4097: inside the stream's history
    # but over the 4 KiB window, so the decoder must refuse it.
    w = BitWriter()
    w.write(1, 1)  # BFINAL
    w.write(1, 2)  # BTYPE = fixed
    for i in range(5000):
        fixed_lit(w, i & 0xFF)
    w.code(257 - 256, 7)  # length symbol 257: a match of 3
    w.code(24, 5)         # distance symbol 24: base 4097
    w.write(0, 11)        # 11 extra bits of 0 -> distance 4097
    bads.append(("distance_over_4k", w.out(), 5003))

    return bads


def c_array(name: str, data: bytes) -> list[str]:
    body = data if data else b"\x00"  # a zero-length array is ill-formed; the size constant says 0
    lines = [f"inline const std::uint8_t {name}[] = {{"]
    for i in range(0, len(body), 16):
        lines.append("    " + ", ".join(str(b) for b in body[i : i + 16]) + ",")
    lines.append("};")
    return lines


def emit(goods: list[tuple[str, bytes, bytes]], bads: list[tuple[str, bytes, int]]) -> str:
    out: list[str] = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Generated by tests/inflate_fixtures.py at configure time -- do not edit, do not commit.",
        "// Every good stream is zlib.compress(..., wbits=-12) of its raw bytes (the 4 KiB window of the",
        "// build's encoder); the bad ones are crafted in the script. zlib's deflate bytes differ between",
        "// library versions, which is why they are generated, never committed.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace inflate_fixtures {",
        "",
        "struct Good {",
        "  const char* name;",
        "  const std::uint8_t* raw;",
        "  std::size_t raw_size;",
        "  const std::uint8_t* z;",
        "  std::size_t z_size;",
        "};",
        "",
        "struct Bad {",
        "  const char* name;",
        "  const std::uint8_t* z;",
        "  std::size_t z_size;",
        "  std::size_t raw_size;  // what the test declares to begin()",
        "};",
        "",
    ]
    for name, raw, z in goods:
        out += c_array(f"k{camel(name)}Raw", raw)
        out.append(f"inline constexpr std::size_t k{camel(name)}RawSize = {len(raw)};")
        out += c_array(f"k{camel(name)}Z", z)
        out.append(f"inline constexpr std::size_t k{camel(name)}ZSize = {len(z)};")
        out.append("")
    for name, z, raw_size in bads:
        out += c_array(f"k{camel(name)}Z", z)
        out.append(f"inline constexpr std::size_t k{camel(name)}ZSize = {len(z)};")
        out.append(f"inline constexpr std::size_t k{camel(name)}RawSize = {raw_size};")
        out.append("")
    out.append("inline const Good kGood[] = {")
    for name, raw, z in goods:
        c = camel(name)
        out.append(f'    {{"{name}", k{c}Raw, k{c}RawSize, k{c}Z, k{c}ZSize}},')
    out.append("};")
    out.append("inline constexpr std::size_t kGoodCount = sizeof(kGood) / sizeof(kGood[0]);")
    out.append("")
    out.append("inline const Bad kBad[] = {")
    for name, z, raw_size in bads:
        c = camel(name)
        out.append(f'    {{"{name}", k{c}Z, k{c}ZSize, k{c}RawSize}},')
    out.append("};")
    out.append("inline constexpr std::size_t kBadCount = sizeof(kBad) / sizeof(kBad[0]);")
    out.append("")
    out.append("}  // namespace inflate_fixtures")
    out.append("")
    return "\n".join(out)


def camel(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, type=Path, help="the repository root")
    ap.add_argument("--out", required=True, type=Path, help="the header to write")
    args = ap.parse_args()

    goods = good_fixtures(args.root)
    bads = bad_fixtures(goods)

    # Self-check: every good stream must inflate to exactly its raw bytes, and every crafted bad stream
    # must be one zlib itself refuses (the over-long ones are valid deflate that the C++ side rejects
    # through the declared raw_size or the trailing byte, which zlib does not check).
    for name, raw, z in goods:
        if zlib.decompress(z, -12) != raw:
            raise SystemExit(f"fixture {name}: the stream does not inflate to its raw bytes")
    for name, z, _ in bads:
        if name.startswith("overlong_"):
            continue
        try:
            zlib.decompress(z, -12)
        except zlib.error:
            continue
        raise SystemExit(f"bad fixture {name}: zlib accepts the stream, it is not malformed")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(emit(goods, bads))
    print(f"inflate_fixtures.py: {len(goods)} good fixtures, {len(bads)} bad streams -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
