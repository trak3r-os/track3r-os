#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-check of the device's HEX validator against tools/check_image.py (OS-006c; stdlib only).

    fw_hex_cross.py --tool O3OS_FW_HEX_VERDICT --check-image check_image.py [--hex REAL.hex]

Builds a corpus of HEX files from check_image.py's own helpers (the good image of its self-test and the same families of mutations: every record
defect, holes, address range, the head, SP, reset and vector cases, the SRAM_L window, every byte of the flash configuration field), runs both
checkers on every file and requires the SAME verdict: check_image.py with --expect-sp 0x20030000 --ramfunc-l 0x1FFF0000:0xC000 (how the build runs it)
against the device validator. For an accepted file the image length and the CRC-32 of the file must agree as well. With --hex the real built image
and a family of mutations of it join the corpus. One documented difference is checked as such: records out of ascending order, which check_image.py
accepts and the device refuses (it keeps O(1) state; objcopy never writes them). check_image.py's `sector_end` (the image ends on a 4 KiB flash
sector boundary, OS-BUG-COSMBOOT) is a build-time rule that the device validator does not apply: the reference verdict here leaves it out. Exit 0 all agree, 1 a disagreement, 2 a usage or tool error.
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile
import zlib


def load_check_image(path):
    spec = importlib.util.spec_from_file_location("check_image", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_corpus(ci, real_text):
    corpus = []   # (label, text, device_stricter)

    def add(label, text, stricter=False):
        corpus.append((label, text, stricter))

    def flat_of(segments):
        flat = {}
        for base, blob in segments.items():
            for i, b in enumerate(blob):
                flat[base + i] = b
        return flat

    def hex_of_flat(flat):
        return ci.make_hex({a: bytes([b]) for a, b in sorted(flat.items())})

    good = ci.good_segments()
    good_text = ci.make_hex(good)
    add("good image", good_text)

    # Parser families.
    lines = good_text.split("\n")
    bad = list(lines)
    bad[1] = bad[1][:-2] + ("00" if bad[1][-2:] != "00" else "01")
    add("bad checksum", "\n".join(bad))
    add("no EOF record", ci.make_hex(good, eof=False))
    add("data after EOF", good_text + ci.ihex_record(0x00, 0, [1]) + "\n")
    add("junk after EOF", good_text + "x\n")
    add("blank lines after EOF", good_text + "\n\n")
    add("CR LF line ends", good_text.replace("\n", "\r\n"))
    add("no final line end", good_text[:-1])
    add("lower case digits", good_text.lower())
    add("unknown record type", ci.make_hex(good, extra=[ci.ihex_record(0x09, 0, [0])]))
    add("overlapping records", ci.make_hex({0: b"\x01\x02\x03\x04", 2: b"\x09"}))
    add("not hex", ":zz\n" + ci.ihex_record(0x01, 0, b""))
    add("missing colon", "00000001FF\n")
    add("short count", ":0400000001FB\n" + ci.ihex_record(0x01, 0, b""))
    add("empty file", "")
    add("EOF only", ci.ihex_record(0x01, 0, b"") + "\n")
    add("start segment address record", ci.make_hex(good, extra=[ci.ihex_record(0x03, 0, [0, 0, 4, 0x2D])]))
    add("start linear address record", ci.make_hex(good, extra=[ci.ihex_record(0x05, 0, [0, 0, 4, 0x2D])]))
    add("extended segment record, valid", ci.make_hex({}, extra=[ci.ihex_record(0x02, 0, [0x00, 0x00])]) .replace(ci.ihex_record(0x01, 0, b"") + "\n", "") + good_text.split("\n", 1)[1])
    add("EOF record with data", ci.ihex_record(0x01, 0, [1]) + "\n")

    # Address range and holes.
    segments = dict(good)
    segments[0x00100000] = b"\x00"
    add("address 0x100000", ci.make_hex(segments))
    segments = dict(good)
    segments[0x00100000 - 1] = b"\x00"
    add("a lone byte at 0xFFFFF", ci.make_hex(segments))
    code_end = ci.CONFIG_ADDRESS + ci.CONFIG_SIZE + len(good[ci.CONFIG_ADDRESS + ci.CONFIG_SIZE])
    for label, hole_at, hole_len in (("inside the code", 0x500, 1), ("four bytes inside the code", 0x500, 4),
                                     ("just past the flash configuration", 0x410, 1), ("at the end of the vector table", 116 * 4 - 1, 1),
                                     ("second-to-last byte", code_end - 2, 1)):
        flat = flat_of(good)
        for a in range(hole_at, hole_at + hole_len):
            del flat[a]
        add("hole " + label, hex_of_flat(flat))
    segments = dict(good)
    segments[code_end + 4] = b"\xAA" * 8
    add("hole after the code", ci.make_hex(segments))
    segments = dict(good)
    segments[code_end] = b"\xFF" * 4 + b"\xAA" * 8
    add("a filled gap", ci.make_hex(segments))
    add("image not starting at 0", ci.make_hex({a + 4: b for a, b in good.items()}))

    # Head, SP, reset, vectors.
    segments = dict(good)
    del segments[ci.CONFIG_ADDRESS + ci.CONFIG_SIZE]
    add("only the head", ci.make_hex(segments))
    for label, sp in (("zero", 0), ("flash", 0x1000), ("below SRAM", 0x1FFEFFF8), ("past SRAM", 0x20030008), ("unaligned", 0x20030004),
                      ("peripheral", 0x40000000), ("inside SRAM", 0x2002FFF8)):
        segments = dict(good)
        segments[0] = sp.to_bytes(4, "little") + segments[0][4:]
        add("SP " + label, ci.make_hex(segments))
    for label, value in (("even", 0x500), ("outside the image", 0x200001), ("in the vector table", 0x101), ("in the config field", 0x405),
                         ("unpopulated", 0xFF001), ("zero", 0), ("first byte past the field", 0x411)):
        segments = dict(good)
        segments[0] = segments[0][:4] + value.to_bytes(4, "little") + segments[0][8:]
        add("reset vector " + label, ci.make_hex(segments))
    for index in (2, 3, 15, 16, 60, 115):
        for label, value in (("even", 0x510), ("zero", 0), ("outside", 0x300001), ("in the table", 0x41), ("in the config", 0x401)):
            segments = dict(good)
            vectors = bytearray(segments[0])
            vectors[index * 4:index * 4 + 4] = value.to_bytes(4, "little")
            segments[0] = bytes(vectors)
            add("vector %d %s" % (index, label), ci.make_hex(segments))
    for index in ci.RESERVED_VECTORS:
        segments = dict(good)
        vectors = bytearray(segments[0])
        vectors[index * 4:index * 4 + 4] = (0x511).to_bytes(4, "little")
        segments[0] = bytes(vectors)
        add("reserved vector %d filled" % index, ci.make_hex(segments))
    for index in (31, 80, 115):
        for label, value in (("in SRAM_L", 0x1FFF0101), ("even in SRAM_L", 0x1FFF0100), ("module slab", 0x1FFFC001), ("just past the window", 0x1FFFC001),
                             ("SRAM_U", 0x20000101), ("beyond the image's length", 0x1FFF8001), ("below SRAM_L", 0x1FFEFFFF)):
            segments = dict(good)
            vectors = bytearray(segments[0])
            vectors[index * 4:index * 4 + 4] = value.to_bytes(4, "little")
            segments[0] = bytes(vectors)
            add("vector %d %s" % (index, label), ci.make_hex(segments))
    segments = dict(good)
    segments[0] = segments[0][:4] + (0x1FFF0101).to_bytes(4, "little") + segments[0][8:]
    add("reset vector in SRAM_L", ci.make_hex(segments))
    segments = dict(good)
    tail = segments[0][116 * 4:]
    segments[0] = segments[0][:116 * 4 - 2]
    segments[116 * 4 - 2] = b"\x00\x00"
    segments[116 * 4] = tail
    add("table split across records", ci.make_hex(segments))

    # The flash configuration field, every byte.
    required = bytes.fromhex(ci.REQUIRED_CONFIG_HEX)
    for position in range(ci.CONFIG_SIZE):
        for label, value in (("bit flip", required[position] ^ 1), ("inverted", required[position] ^ 0xFF), ("zero", 0), ("erased", 0xFF)):
            if value == required[position]:
                continue
            flat = flat_of(good)
            flat[ci.CONFIG_ADDRESS + position] = value
            add("config byte %d %s" % (position, label), hex_of_flat(flat))
    for value in (0xDC, 0xFE, 0xDF, 0xDD):
        flat = flat_of(good)
        flat[ci.CONFIG_ADDRESS + 12] = value
        add("FSEC 0x%02X" % value, hex_of_flat(flat))
    segments = dict(good)
    del segments[ci.CONFIG_ADDRESS]
    add("config field absent", ci.make_hex(segments))
    segments = dict(good)
    segments[ci.CONFIG_ADDRESS] = required[:15]
    add("config field one byte short", ci.make_hex(segments))

    # The one documented difference: records in another order.
    flat = flat_of(good)
    data_lines = [l for l in good_text.split("\n") if l.startswith(":10")]
    swapped = list(good_text.split("\n"))
    first = swapped.index(data_lines[1])
    swapped[first], swapped[first + 1] = swapped[first + 1], swapped[first]
    add("records out of order", "\n".join(swapped), stricter=True)

    # The real image and what can go wrong with it.
    if real_text is not None:
        add("REAL image", real_text)
        memory = ci.parse_ihex(real_text)
        for position in range(ci.CONFIG_SIZE):
            flat = dict(memory)
            flat[ci.CONFIG_ADDRESS + position] ^= 0x01
            add("REAL config byte %d flipped" % position, hex_of_flat(flat))
        flat = dict(memory)
        flat[ci.CONFIG_ADDRESS + 12] = 0xDC
        add("REAL FSEC 0xDC", hex_of_flat(flat))
        for offset, label in ((0, "SP"), (4, "reset"), (16 * 4, "vector 16"), (60 * 4, "vector 60"), (115 * 4, "vector 115")):
            flat = dict(memory)
            flat[offset + 1] ^= 0x01 if offset else 0x00
            flat[offset] ^= 0x01
            add("REAL %s changed" % label, hex_of_flat(flat))
        flat = dict(memory)
        del flat[0x1000]
        add("REAL hole at 0x1000", hex_of_flat(flat))
        flat = dict(memory)
        top = max(flat)
        del flat[top - 1]
        add("REAL hole before the last byte", hex_of_flat(flat))
        flat = dict(memory)
        flat[0x100000] = 0
        add("REAL byte at 0x100000", hex_of_flat(flat))
        add("REAL without EOF", real_text.replace(ci.ihex_record(0x01, 0, b"") + "\n", ""))
        add("REAL truncated half", real_text[: len(real_text) // 2])
        add("REAL CR LF", real_text.replace("\n", "\r\n"))
        rl = real_text.split("\n")
        middle = len(rl) // 2
        rl[middle] = rl[middle][:-2] + ("00" if rl[middle][-2:] != "00" else "01")
        add("REAL checksum in the middle", "\n".join(rl))
    return corpus


def reference_verdict(ci, text):
    """(ok, image length) by check_image.py's own functions, with the options of the build."""
    try:
        memory = ci.parse_ihex(text)
    except ci.ImageError:
        return False, 0
    results = ci.check_image(memory, expect_sp=0x20030000, ramfunc_l=(0x1FFF0000, 0xC000))
    ok = all(r[1] for r in results if r[0] != "sector_end")   # a build-time rule only: the device validator does not apply it
    return ok, max(memory) + 1


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--check-image", required=True)
    parser.add_argument("--hex", default=None, help="a real built image to cross-check as well (with mutations)")
    args = parser.parse_args(argv)
    ci = load_check_image(args.check_image)
    real_text = None
    if args.hex:
        with open(args.hex, "r", encoding="ascii") as f:
            real_text = f.read()
    corpus = build_corpus(ci, real_text)
    problems = []
    accepted = 0
    with tempfile.TemporaryDirectory() as tmp:
        paths = []
        for i, (label, text, stricter) in enumerate(corpus):
            path = os.path.join(tmp, "case%04d.hex" % i)
            with open(path, "w", encoding="ascii", newline="") as f:
                f.write(text)
            paths.append(path)
        verdicts = {}
        for start in range(0, len(paths), 200):
            batch = paths[start:start + 200]
            run = subprocess.run([args.tool] + batch, capture_output=True, text=True)
            if run.returncode != 0:
                print("fw_hex_cross: the tool failed: %s" % run.stderr.strip(), file=sys.stderr)
                return 2
            for line in run.stdout.splitlines():
                fields = line.split("\t")
                verdicts[fields[0]] = fields[1:]
        agree = 0
        for path, (label, text, stricter) in zip(paths, corpus):
            ref_ok, ref_len = reference_verdict(ci, text)
            got = verdicts.get(path)
            if got is None:
                problems.append("%s: no verdict from the tool" % label)
                continue
            dev_ok = got[0] == "ok"
            if stricter:
                if not (ref_ok and not dev_ok):
                    problems.append("%s: expected check_image to accept and the device to refuse, got reference %s, device %s" % (label, ref_ok, got))
                else:
                    agree += 1
                continue
            if ref_ok != dev_ok:
                problems.append("%s: check_image %s, device %s" % (label, "accepts" if ref_ok else "refuses", got))
                continue
            if ref_ok:
                accepted += 1
                crc = "%08X" % (zlib.crc32(text.encode("ascii")) & 0xFFFFFFFF)
                if int(got[1]) != ref_len or got[2] != crc:
                    problems.append("%s: both accept but length/crc differ: device %s, expected %d %s" % (label, got, ref_len, crc))
                    continue
            agree += 1
    for p in problems:
        print("fw_hex_cross FAILED: " + p)
    print("fw_hex_cross: %d case(s) (%d accepted by both), %d agree, %d problem(s)%s" % (len(corpus), accepted, agree, len(problems), " (with the real image)" if real_text else ""))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
