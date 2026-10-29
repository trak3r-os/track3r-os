#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest `project_python`: the native project format, C++ against the Python reference (stdlib only).

usage: o3project_roundtrip.py O3PJ_TOOL O3PROJECT_PY WORKDIR

For the empty (default), the small, the maximal, the settings (project settings and Learn bindings: its SETS and LRNB chunks, OS-002d),
the state fixture (the project-level state of OS-004d: its MIDI, USBS, MIXR, PERF and APPM chunks and instruments of INST 1.2), the persist2 fixture (the
session state of OS-PERSIST2: its NOTE, MUTE, SCEN and SNFX chunks), the OG fixture (an OG import: its OGPM and OGPN chunks) and the OG v17 fixture (the same from a v17 project, the layout assumed for 1.9.2, whose OGPM chunk
is 1.1 and carries the byte at 0x80C, OS-004c):
  1. C++ writes the file; Python reads and rewrites it; the two files are byte-identical;
  2. C++ reads the Python-written file and saves it again: still byte-identical (C++ -> Python -> C++);
  3. the text dumps of C++ and of Python agree line by line (the fields mean the same on both sides).
Also: both sides print the same limits table, reject the same damaged files with the same error kind, refuse a newer
major version at any chunk length and read a newer minor with appended fields (a CRC chunk too) at any length.
"""
import importlib.util
import os
import struct
import subprocess
import sys
import zlib


def run(cmd, expect_ok=True):
    done = subprocess.run(cmd, capture_output=True)
    if expect_ok and done.returncode != 0:
        sys.exit("FAILED: %s\n%s" % (" ".join(cmd), done.stderr.decode("utf-8", "replace")))
    return done


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def first_difference(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i
    return min(len(a), len(b))


def error_kind(stderr):
    """The error name in `...load failed: <name> (<detail>)`, from either tool."""
    text = stderr.decode("utf-8", "replace")
    marker = "load failed: "
    if marker not in text:
        return None
    return text.split(marker, 1)[1].split(" (", 1)[0]


def split_chunks(data):
    """(id, payload) of every chunk of a well-formed file, without the CRC chunk."""
    out = []
    pos = 12
    while pos + 8 <= len(data):
        cid, length = data[pos:pos + 4], struct.unpack(">I", data[pos + 4:pos + 8])[0]
        pos += 8
        if cid == b"CRC ":
            break
        out.append((cid, data[pos:pos + length]))
        pos += length + (length & 1)
    return out


def join_chunks(chunks, crc_version=0x0100, crc_tail=0):
    """A file from chunks, with the FORM length and the CRC chunk made right (the CRC chunk may be a newer minor)."""
    body = b"".join(c + struct.pack(">I", len(p)) + p + (b"\0" if len(p) & 1 else b"") for c, p in chunks)
    crc_len = 6 + crc_tail
    form = b"FORM" + struct.pack(">I", 4 + len(body) + 8 + crc_len + (crc_len & 1)) + b"O3PJ" + body
    crc = zlib.crc32(form) & 0xFFFFFFFF
    tail = b"\xcd" * crc_tail
    return form + b"CRC " + struct.pack(">IHI", crc_len, crc_version, crc) + tail + (b"\0" if crc_len & 1 else b"")


# Size and CRC-32 of the fixtures written by the build before OS-BUG-PASTE24 (the commit of the S1 commit's parent), from its o3pj_tool; the `law` fixture is the og fixture with the LAW chunk and is checked below.
PRE_LAW = {"empty": (186, 0xE8816302), "small": (2648, 0x3F04F4E7), "maximal": (1888426, 0xC58BE9E8),  # (OS-LFOSINE: same size, its LFO shape bytes are 0..4 now: the reader reads a larger one as Triangle)
 "settings": (2720, 0x230EA0D8), "state": (3372, 0xA8D283E1),
           "midi13": (3336, 0x9276CAC9), "persist2": (3032, 0x40AD3678), "og": (24864, 0x1C13F03D), "og17": (24864, 0xBE53BEBB)}


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    tool, ref, work = sys.argv[1:4]
    py = [sys.executable, "-I", ref]
    os.makedirs(work, exist_ok=True)

    cpp_limits = run([tool, "limits"]).stdout
    py_limits = run(py + ["limits"]).stdout
    if cpp_limits != py_limits:
        sys.exit("FAILED: the limits tables differ\nC++:\n%s\nPython:\n%s" % (cpp_limits.decode(), py_limits.decode()))

    for kind in ("empty", "small", "maximal", "settings", "state", "midi13", "persist2", "lfosine", "og", "og17", "law"):
        a, b, c = (os.path.join(work, "%s.%s.o3p" % (kind, tag)) for tag in ("cpp", "py", "cpp2"))
        run([tool, "gen", kind, a])
        if kind in PRE_LAW:  # OS-BUG-PASTE24: a project whose law is 0 writes no LAW chunk: the file is the one the build before it wrote (size and CRC-32 taken from that build's o3pj_tool)
            if (len(read(a)), zlib.crc32(read(a)) & 0xFFFFFFFF) != PRE_LAW[kind]:
                sys.exit("FAILED: %s: the file is no longer the one of the build before the LAW chunk (%d bytes, crc %08x; wanted %r)"
                         % (kind, len(read(a)), zlib.crc32(read(a)) & 0xFFFFFFFF, PRE_LAW[kind]))
        run(py + ["rewrite", a, b])
        if read(a) != read(b):
            sys.exit("FAILED: %s: Python rewrote the C++ file differently (first difference at byte %d)"
                     % (kind, first_difference(read(a), read(b))))
        run([tool, "resave", b, c])
        if read(a) != read(c):
            sys.exit("FAILED: %s: C++ -> Python -> C++ changed the file (first difference at byte %d)"
                     % (kind, first_difference(read(a), read(c))))
        cpp_dump = run([tool, "dump", a]).stdout.decode().splitlines()
        py_dump = run(py + ["dump", a]).stdout.decode().splitlines()
        if cpp_dump != py_dump:
            for n, (x, y) in enumerate(zip(cpp_dump, py_dump)):
                if x != y:
                    sys.exit("FAILED: %s: dumps differ at line %d\nC++:    %s\nPython: %s" % (kind, n + 1, x[:300], y[:300]))
            sys.exit("FAILED: %s: dumps differ in length (%d vs %d lines)" % (kind, len(cpp_dump), len(py_dump)))
        print("%s: %d bytes, %d dump lines: identical" % (kind, len(read(a)), len(cpp_dump)))

    # OS-BUG-PASTE24: the LAW chunk. The `law` fixture is the og fixture (an OG v16 import) with the volume law 1: the same chunks, and between SNFX's place and the OG chunks the 12 bytes of LAW; both
    # sides read it (the dumps above are identical and say `law=1`), and a project of law 0 says nothing.
    law_chunks = split_chunks(read(os.path.join(work, "law.cpp.o3p")))
    og_only = split_chunks(read(os.path.join(work, "og.cpp.o3p")))
    if [c for c, _ in law_chunks if c != b"LAW "] != [c for c, _ in og_only] or [(c, p) for c, p in law_chunks if c == b"LAW "] != [(b"LAW ", b"\x01\x00\x01")]:
        sys.exit("FAILED: the law fixture is not the og fixture plus one LAW chunk of 1.0, law 1")
    kinds_law = [c for c, _ in law_chunks]
    if kinds_law.index(b"LAW ") != kinds_law.index(b"OGPM") - 1:
        sys.exit("FAILED: LAW is not the last known chunk, right before the unknown chunks (%r)" % kinds_law)
    if "law=1" not in run([tool, "dump", os.path.join(work, "law.cpp.o3p")]).stdout.decode().splitlines():
        sys.exit("FAILED: the law fixture does not dump law=1")
    if any(l.startswith("law") for l in run([tool, "dump", os.path.join(work, "og.cpp.o3p")]).stdout.decode().splitlines()):
        sys.exit("FAILED: a project of law 0 dumps a law line")

    # The chunks of the OG formats: both sides decode them alike, valid or not (a damaged one is still kept verbatim)
    og = read(os.path.join(work, "og.cpp.o3p"))
    og_chunks = split_chunks(og)
    kinds = [c for c, _ in og_chunks]
    if b"OGPM" not in kinds or b"OGPN" not in kinds:
        sys.exit("FAILED: the OG fixture holds no OGPM or no OGPN chunk (%r)" % kinds)
    og_lines = [l for l in run([tool, "dump", os.path.join(work, "og.cpp.o3p")]).stdout.decode().splitlines()
                if l.startswith(("ogpm", "ogpn"))]
    if len(og_lines) != 2 or any("valid=1" not in l for l in og_lines):
        sys.exit("FAILED: the OG fixture's chunks are not valid: %r" % og_lines)
    if " tail=0" not in og_lines[0]:
        sys.exit("FAILED: the OG (v16) fixture's OGPM chunk carries a byte of 0x80C: %r" % og_lines)

    def check_og_chunks(chunks, edits, pairs=2):
        """Each edit damages or changes a chunk; both sides must rewrite the file alike and decode the chunks alike."""
        for name, edit in edits.items():
            edited = join_chunks([(c, edit(c, p)) for c, p in chunks])
            a, b = (os.path.join(work, "ogdamaged.%s.o3p" % tag) for tag in ("a", "b"))
            write(a, edited)
            run(py + ["rewrite", a, b])
            if read(a) != read(b):
                sys.exit("FAILED: %s: Python rewrote the chunk differently" % name)
            cpp_lines = [l for l in run([tool, "dump", a]).stdout.decode().splitlines() if l.startswith(("ogpm", "ogpn"))]
            py_lines = [l for l in run(py + ["dump", a]).stdout.decode().splitlines() if l.startswith(("ogpm", "ogpn"))]
            if cpp_lines != py_lines or len(cpp_lines) != pairs:
                sys.exit("FAILED: %s: C++ says %r, Python says %r" % (name, cpp_lines, py_lines))
            print("%s: %s" % (name, " / ".join(cpp_lines)))

    check_og_chunks(og_chunks, {
        "OGPM one byte short": lambda c, p: p[:-1] if c == b"OGPM" else p,
        "OGPM one byte long": lambda c, p: p + b"\0" if c == b"OGPM" else p,
        "OGPM major 2": lambda c, p: b"\x02" + p[1:] if c == b"OGPM" else p,
        "OGPM unknown group": lambda c, p: p[:2] + b"\x04" + p[3:] if c == b"OGPM" else p,
        "OGPM newer minor": lambda c, p: b"\x01\x01" + p[2:] + b"\xee\xef" if c == b"OGPM" else p,
        "OGPM 1.1 with its byte": lambda c, p: b"\x01\x01" + p[2:] + b"\x5a" if c == b"OGPM" else p,
        "OGPM 1.1 without its byte": lambda c, p: b"\x01\x01" + p[2:] if c == b"OGPM" else p,
        "OGPN not a PAMD": lambda c, p: p[:6] + b"X" + p[7:] if c == b"OGPN" else p,
        "OGPN too long a file": lambda c, p: p[:2] + b"\x00\x01\xff\xff" + p[6:] if c == b"OGPN" else p,
        "OGPN too short": lambda c, p: p[:5] if c == b"OGPN" else p,
    })

    # OS-004c: the OGPM chunk of a v17 project (the layout assumed for 1.9.2) is 1.1: group 9 is one byte longer, the byte at 0x80C (group 9 is the
    # highest group, so it is the last byte of the chunk). Both sides read it, and a newer minor's appended bytes, alike.
    path17 = os.path.join(work, "og17.cpp.o3p")
    og17_chunks = split_chunks(read(path17))
    ogpm17 = [p for c, p in og17_chunks if c == b"OGPM"]
    if len(ogpm17) != 1 or ogpm17[0][:2] != b"\x01\x01" or len(ogpm17[0]) != 954 or ogpm17[0][-1] == 0:
        sys.exit("FAILED: the OG v17 fixture's OGPM chunk is not a 1.1 chunk with a byte at 0x80C (%r)" % [p[:2] for p in ogpm17])
    lines17 = [l for l in run([tool, "dump", path17]).stdout.decode().splitlines() if l.startswith(("ogpm", "ogpn"))]
    if len(lines17) != 2 or any("valid=1" not in l for l in lines17) or (" tail=%d" % ogpm17[0][-1]) not in lines17[0]:
        sys.exit("FAILED: the OG v17 fixture's chunks do not decode to the byte at 0x80C: %r" % lines17)
    check_og_chunks(og17_chunks, {
        "OGPM 1.1 one byte short": lambda c, p: p[:-1] if c == b"OGPM" else p,
        "OGPM 1.1 one byte long": lambda c, p: p + b"\x42" if c == b"OGPM" else p,
        "OGPM 1.1 a zero byte": lambda c, p: p[:-1] + b"\0" if c == b"OGPM" else p,
        "OGPM 1.1 labelled 1.0": lambda c, p: b"\x01\x00" + p[2:] if c == b"OGPM" else p,
        "OGPM 1.2 with more bytes": lambda c, p: b"\x01\x02" + p[2:] + b"\xab\xcd" if c == b"OGPM" else p,
        "OGPM 1.1 with group 9 cleared": lambda c, p: p[:2] + bytes([p[2] & ~2]) + p[3:] if c == b"OGPM" else p,
        "OGPM 2.1": lambda c, p: b"\x02" + p[1:] if c == b"OGPM" else p,
    })

    # OS-002d: the settings and Learn chunks. Entries a reader cannot use (an unknown setting, a value out of range or NaN, an invalid or
    # conflicting binding) are skipped, the setting keeps its default, and both sides skip alike; a chunk whose length does not fit its count
    # is refused by both; a newer minor with appended bytes is read.
    sett = split_chunks(read(os.path.join(work, "settings.cpp.o3p")))
    if [c for c, _ in sett if c in (b"SETS", b"LRNB")] != [b"SETS", b"LRNB"]:
        sys.exit("FAILED: the settings fixture holds no SETS and LRNB chunks (%r)" % [c for c, _ in sett])
    if [l for l in run([tool, "dump", os.path.join(work, "settings.cpp.o3p")]).stdout.decode().splitlines()
            if l.startswith(("setting", "learn"))].__len__() != 5:
        sys.exit("FAILED: the settings fixture does not dump two settings and three bindings")

    def setting(field, value, kind=4, index=0):
        return struct.pack(">If", (kind << 24) | (index << 16) | field, value)

    def binding(param, cc, channel=0, mode=0, flags=0):
        return struct.pack(">IBBBB", param, cc, channel, mode, flags)

    cutoff, resonance, volume = (1 << 24) | 7, (1 << 24) | 8, (1 << 24) | 0
    sets_head, lrnb_head = b"\x01\x00", b"\x01\x00"
    cases = {
        "SETS unknown setting": (b"SETS", sets_head + struct.pack(">H", 3) + setting(2, 1.0) + setting(99, 1.0) + setting(3, 1.0)),
        "SETS value out of range": (b"SETS", sets_head + struct.pack(">H", 2) + setting(2, 2.0) + setting(3, 1.0)),
        "SETS NaN": (b"SETS", sets_head + struct.pack(">H", 2) + struct.pack(">II", (4 << 24) | 2, 0x7FC00000) + setting(3, 1.0)),
        "SETS another scope": (b"SETS", sets_head + struct.pack(">H", 2) + setting(2, 1.0, kind=5) + setting(3, 1.0)),
        "SETS an index": (b"SETS", sets_head + struct.pack(">H", 2) + setting(2, 1.0, index=3) + setting(3, 1.0)),
        "SETS rounds": (b"SETS", sets_head + struct.pack(">H", 2) + setting(0, 0.4) + setting(3, 0.6)),
        "SETS twice the same": (b"SETS", sets_head + struct.pack(">H", 2) + setting(2, 1.0) + setting(2, 0.0)),
        "SETS newer minor": (b"SETS", b"\x01\x01" + struct.pack(">H", 1) + setting(2, 1.0) + b"\xab" * 33),
        "LRNB cc 200": (b"LRNB", lrnb_head + struct.pack(">H", 2) + binding(cutoff, 200) + binding(resonance, 71)),
        "LRNB channel 17": (b"LRNB", lrnb_head + struct.pack(">H", 2) + binding(cutoff, 74, 17) + binding(resonance, 71)),
        "LRNB bad mode": (b"LRNB", lrnb_head + struct.pack(">H", 2) + binding(cutoff, 74, 0, 3) + binding(resonance, 71)),
        "LRNB bad flag": (b"LRNB", lrnb_head + struct.pack(">H", 2) + binding(cutoff, 74, 0, 0, 2) + binding(resonance, 71)),
        "LRNB device scope": (b"LRNB", lrnb_head + struct.pack(">H", 2) + binding((2 << 24) | 0, 74) + binding(resonance, 71)),
        "LRNB follow of an indexed one": (b"LRNB", lrnb_head + struct.pack(">H", 2) + binding((1 << 24) | (3 << 16) | 7, 74, 0, 0, 1) + binding(resonance, 71)),
        "LRNB the same cc displaces": (b"LRNB", lrnb_head + struct.pack(">H", 3) + binding(cutoff, 74) + binding(resonance, 74) + binding(volume, 7)),
        "LRNB the same param again": (b"LRNB", lrnb_head + struct.pack(">H", 3) + binding(cutoff, 74) + binding(volume, 7) + binding(cutoff, 75, 2, 1)),
        "LRNB overlap by channel": (b"LRNB", lrnb_head + struct.pack(">H", 3) + binding(cutoff, 74, 1) + binding(resonance, 74, 2) + binding(volume, 74, 0)),
        "LRNB newer minor": (b"LRNB", b"\x01\x01" + struct.pack(">H", 1) + binding(cutoff, 74) + b"\xcd" * 40),
        "SETS newer minor, count above the cap": (b"SETS", b"\x01\x01" + struct.pack(">H", 70) + setting(2, 1.0) + b"\0" * (69 * 8)),
        "LRNB newer minor, count above the cap": (b"LRNB", b"\x01\x01" + struct.pack(">H", 70) + binding(cutoff, 74) + b"\0" * (69 * 8)),
        "LRNB full store": (b"LRNB", lrnb_head + struct.pack(">H", 32) + b"".join(binding((1 << 24) | (i << 16) | 7, i) for i in range(32))),
    }
    for name, (kind, payload) in cases.items():
        edited = join_chunks([(c, payload if c == kind else p) for c, p in sett])
        a, b, c2 = (os.path.join(work, "settings.%s.o3p" % tag) for tag in ("edit", "py", "cpp2"))
        write(a, edited)
        run(py + ["rewrite", a, b])
        run([tool, "resave", a, c2])
        if read(b) != read(c2):
            sys.exit("FAILED: %s: Python and C++ rewrote it differently (first difference at byte %d)"
                     % (name, first_difference(read(b), read(c2))))
        cpp_lines = [l for l in run([tool, "dump", a]).stdout.decode().splitlines() if l.startswith(("setting", "learn"))]
        py_lines = [l for l in run(py + ["dump", a]).stdout.decode().splitlines() if l.startswith(("setting", "learn"))]
        if cpp_lines != py_lines:
            sys.exit("FAILED: %s: C++ says %r, Python says %r" % (name, cpp_lines, py_lines))
    print("settings: %d damaged or foreign SETS and LRNB cases read alike" % len(cases))
    for name, (kind, payload) in {
        "SETS count above its length": (b"SETS", sets_head + struct.pack(">H", 3) + setting(2, 1.0)),
        "SETS count above 64": (b"SETS", sets_head + struct.pack(">H", 65) + b"\0" * (65 * 8)),
        "SETS a byte long": (b"SETS", sets_head + struct.pack(">H", 1) + setting(2, 1.0) + b"\0"),
        "LRNB count above its length": (b"LRNB", lrnb_head + struct.pack(">H", 2) + binding(cutoff, 74)),
        "LRNB a byte long": (b"LRNB", lrnb_head + struct.pack(">H", 1) + binding(cutoff, 74) + b"\0"),
        "LRNB count above 64": (b"LRNB", lrnb_head + struct.pack(">H", 65) + b"\0" * (65 * 8)),
        "SETS major 2": (b"SETS", b"\x02\x00" + struct.pack(">H", 1) + setting(2, 1.0)),
        "LRNB major 2": (b"LRNB", b"\x02\x00" + struct.pack(">H", 1) + binding(cutoff, 74)),
    }.items():
        write(path := os.path.join(work, "settings.bad.o3p"), join_chunks([(c, payload if c == kind else p) for c, p in sett]))
        cpp = run([tool, "dump", path], expect_ok=False)
        pyr = run(py + ["check", path], expect_ok=False)
        if cpp.returncode != 1 or pyr.returncode != 1 or error_kind(cpp.stderr) != error_kind(pyr.stderr):
            sys.exit("FAILED: %s: C++ says %r (%d), Python says %r (%d)"
                     % (name, error_kind(cpp.stderr), cpp.returncode, error_kind(pyr.stderr), pyr.returncode))
    print("settings: 8 malformed SETS and LRNB chunks refused alike")

    # The same verdict on damaged files.
    good = read(os.path.join(work, "small.cpp.o3p"))
    damaged = {}
    for cut in (0, 5, 11, 12, 20, len(good) // 2, len(good) - 1):
        damaged["truncated at %d" % cut] = good[:cut]
    flipped = bytearray(good)
    flipped[len(good) // 3] ^= 0x01
    damaged["one flipped bit"] = bytes(flipped)
    damaged["wrong magic"] = b"LIST" + good[4:]
    damaged["wrong type"] = good[:8] + b"O3PX" + good[12:]
    newer = bytearray(good)
    newer[12 + 8] = 2  # PHDR version 2.x
    struct.pack_into(">I", newer, len(newer) - 4, zlib.crc32(bytes(newer[:-14])) & 0xFFFFFFFF)
    damaged["newer major version"] = bytes(newer)
    bad_pad = bytearray(good)
    bad_pad[12 + 8 + 29] = 1  # the pad byte after the odd-sized PHDR
    struct.pack_into(">I", bad_pad, len(bad_pad) - 4, zlib.crc32(bytes(bad_pad[:-14])) & 0xFFFFFFFF)
    damaged["pad byte not zero"] = bytes(bad_pad)
    for name, data in damaged.items():
        path = os.path.join(work, "damaged.o3p")
        write(path, data)
        cpp = run([tool, "dump", path], expect_ok=False)
        pyr = run(py + ["check", path], expect_ok=False)
        if cpp.returncode != 1 or pyr.returncode != 1:
            sys.exit("FAILED: %s: expected both to reject (C++ %d, Python %d)" % (name, cpp.returncode, pyr.returncode))
        if error_kind(cpp.stderr) != error_kind(pyr.stderr):
            sys.exit("FAILED: %s: C++ says %r, Python says %r" % (name, error_kind(cpp.stderr), error_kind(pyr.stderr)))
    print("damaged files: %d rejected alike" % len(damaged))

    # Versions: a newer major is refused whatever the chunk's length, a newer minor with appended fields is read
    # at any length and written back without them. Both sides must agree.
    chunks = split_chunks(good)

    def with_payload(kind, nth, payload):
        out, seen = [], 0
        for cid, p in chunks:
            if cid == kind and seen == nth:
                p = payload(p)
            seen += cid == kind
            out.append((cid, p))
        return out

    refused = {
        "PHDR 2.0, 100 bytes": with_payload(b"PHDR", 0, lambda p: b"\x02\x00" + p[2:] + b"\0" * 71),
        "PHDR 2.0, 10 bytes": with_payload(b"PHDR", 0, lambda p: b"\x02\x00" + p[2:10]),
        "PATT 2.0, 20000 bytes": with_payload(b"PATT", 0, lambda p: b"\x02\x00" + p[2:] + b"\0" * (20000 - len(p))),
        "INST 2.0, 900 bytes": with_payload(b"INST", 0, lambda p: b"\x02\x00" + p[2:] + b"\0" * (900 - len(p))),
    }
    for name, cs in refused.items():
        write(path, join_chunks(cs))
        cpp = run([tool, "dump", path], expect_ok=False)
        pyr = run(py + ["check", path], expect_ok=False)
        if error_kind(cpp.stderr) != "newer major version" or error_kind(pyr.stderr) != "newer major version":
            sys.exit("FAILED: %s: C++ says %r, Python says %r" % (name, error_kind(cpp.stderr), error_kind(pyr.stderr)))
    write(path, join_chunks(chunks, crc_version=0x0200, crc_tail=100))
    for who in (run([tool, "dump", path], expect_ok=False), run(py + ["check", path], expect_ok=False)):
        if error_kind(who.stderr) != "newer major version":
            sys.exit("FAILED: CRC chunk 2.0: %r" % error_kind(who.stderr))

    def appended(cid):
        def edit(p):
            minor = b"\x05" if cid == b"INST" else b"\x01"  # INST is at 1.4 (1.3 is the sample rate, the engine, the MIDI fields; 1.4 is OS-LFOSINE's, no field): its next minor is 1.5 (the tail of 0xAB bytes reads as out-of-range fields: the defaults)
            return b"\x01" + minor + p[2:] + b"\xab" * 20000
        return [(c, edit(p) if c == cid else p) for c, p in chunks]

    accepted = {kind.decode().strip(): appended(kind) for kind in (b"PHDR", b"TRAK", b"MODR", b"SONG", b"PATT", b"INST")}
    for name, cs in accepted.items():
        out1, out2 = os.path.join(work, "minor.cpp.o3p"), os.path.join(work, "minor.py.o3p")
        write(path, join_chunks(cs))
        run([tool, "resave", path, out1])
        run(py + ["rewrite", path, out2])
        if read(out1) != good or read(out2) != good:
            sys.exit("FAILED: %s 1.1 with appended fields: not read back as the original" % name)
    for tail in (1, 100, 20000):
        out1, out2 = os.path.join(work, "minor.cpp.o3p"), os.path.join(work, "minor.py.o3p")
        write(path, join_chunks(chunks, crc_version=0x0101, crc_tail=tail))
        run([tool, "resave", path, out1])
        run(py + ["rewrite", path, out2])
        if read(out1) != good or read(out2) != good:
            sys.exit("FAILED: CRC chunk 1.1 with %d appended bytes: not read back as the original" % tail)
    # INST 1.0, a file from before the sample rate existed: read as rate 0 and written back as 1.1, by both sides alike.
    old_inst = [(c, b"\x01\x00" + p[2:-4] if c == b"INST" else p) for c, p in chunks]
    out1, out2 = os.path.join(work, "inst10.cpp.o3p"), os.path.join(work, "inst10.py.o3p")
    write(path, join_chunks(old_inst))
    run([tool, "resave", path, out1])
    run(py + ["rewrite", path, out2])
    if read(out1) != read(out2) or read(out1) == good:
        sys.exit("FAILED: INST 1.0 (no sample rate): C++ and Python do not agree, or the rate was not dropped to 0")
    dump_cpp = run([tool, "dump", path]).stdout
    if b"rate=0 " not in dump_cpp or b"rate=22050 " in dump_cpp:
        sys.exit("FAILED: INST 1.0 did not read as rate 0")
    # OS-004d: the project-level state. The `state` fixture has all five chunks and instruments of INST 1.2.
    state = read(os.path.join(work, "state.cpp.o3p"))
    sc = split_chunks(state)
    kinds = [c for c, _ in sc]
    parts = [b"MIDI", b"USBS", b"MIXR", b"PERF", b"APPM"]
    m0 = kinds.index(b"MIDI")  # the state chunks come after the known ones and before the unknown (XTRA, EMPT of the small fixture)
    if [k for k in kinds if k in parts] != parts or kinds[m0:m0 + 5] != parts or kinds[m0 + 5:] != [b"XTRA", b"EMPT"] or kinds.count(b"INST") != 4:
        sys.exit("FAILED: the state fixture does not hold INST x4 and MIDI, USBS, MIXR, PERF, APPM in that order (%r)" % kinds)
    inst_versions = [p[:2] for c, p in sc if c == b"INST"]
    if inst_versions != [b"\x01\x02", b"\x01\x02", b"\x01\x01", b"\x01\x01"]:
        sys.exit("FAILED: INST 1.2 only for an instrument that chose an engine (%r)" % inst_versions)
    dump_state = run([tool, "dump", os.path.join(work, "state.cpp.o3p")]).stdout.decode().splitlines()
    for prefix in ("midi ", "usb ", "mix ", "perf 0 ", "perf 11 ", "app 0 ", "app 1 "):
        if not any(l.startswith(prefix) for l in dump_state):
            sys.exit("FAILED: the state fixture's dump has no %r line" % prefix)

    def rewrite_alike(name, cs, expect=None):
        """Both sides read the file and write the same bytes back (what `expect` says, when it is given)."""
        a, b, c2 = (os.path.join(work, "state.%s.o3p" % tag) for tag in ("edit", "py", "cpp2"))
        write(a, join_chunks(cs))
        run(py + ["rewrite", a, b])
        run([tool, "resave", a, c2])
        if read(b) != read(c2):
            sys.exit("FAILED: %s: Python and C++ rewrote it differently (first difference at byte %d)" % (name, first_difference(read(b), read(c2))))
        if expect is not None and read(c2) != expect:
            sys.exit("FAILED: %s: not written back as expected (first difference at byte %d)" % (name, first_difference(read(c2), expect)))
        cpp_dump = run([tool, "dump", a]).stdout.decode().splitlines()
        if cpp_dump != run(py + ["dump", a]).stdout.decode().splitlines():
            sys.exit("FAILED: %s: the dumps differ" % name)
        return cpp_dump

    def refused_alike(name, cs, kind="malformed"):
        write(path, join_chunks(cs))
        cpp = run([tool, "dump", path], expect_ok=False)
        pyr = run(py + ["check", path], expect_ok=False)
        if error_kind(cpp.stderr) != kind or error_kind(pyr.stderr) != kind:
            sys.exit("FAILED: %s: C++ says %r, Python says %r (expected %r)" % (name, error_kind(cpp.stderr), error_kind(pyr.stderr), kind))

    def edit_part(cid, fn):
        return [(c, fn(p) if c == cid else p) for c, p in sc]

    # An older file (no state chunks, INST 1.1) reads as the defaults on both sides, and the state fixture minus its chunks is the file an older build wrote.
    without = [(c, p) for c, p in sc if c not in parts]
    older = [(c, (b"\x01\x01" + p[2:-1] if c == b"INST" and p[:2] == b"\x01\x02" else p)) for c, p in without]  # (engines of 1.2 dropped)
    dump_older = rewrite_alike("no state chunks", older)
    if any(l.startswith(("midi ", "usb ", "mix ", "perf ", "app ")) for l in dump_older):
        sys.exit("FAILED: a file with no state chunks reads as having some")
    # A newer minor of each state chunk with appended bytes is read at any length and written back without them (the state fixture again).
    for cid in parts:
        newer = b"\x01\x02" if cid in (b"PERF", b"APPM", b"USBS") else b"\x01\x01"  # PERF (OS-PERFFX P1), APPM (OS-APPUX S3) and USBS (OS-USBOUTS) are at 1.1: their next minor is 1.2
        if cid == b"USBS":
            pass  # a USBS 1.1 chunk holds one more byte, so its newer-minor case is the one with the count, below
        else:
            rewrite_alike("%s %d.%d with appended fields" % (cid.decode(), newer[0], newer[1]), edit_part(cid, lambda p: newer + p[2:] + b"\xab" * 5000), expect=state)
        refused_alike("%s 2.0" % cid.decode(), edit_part(cid, lambda p: b"\x02\x00" + p[2:] + b"\0" * 33), "newer major version")
        refused_alike("%s version 0" % cid.decode(), edit_part(cid, lambda p: b"\x00\x00" + p[2:]))
        refused_alike("%s a byte long" % cid.decode(), edit_part(cid, lambda p: p + b"\0\0"))
        refused_alike("%s a byte short" % cid.decode(), edit_part(cid, lambda p: p[:-1]))
    # OS-USBOUTS: USBS 1.1 (4 or 6 outputs), written by hand here from the description: P8 with 8 slots (the real masks first, then P8's factory masks), then the count.
    def usbs11(count, masks, version=b"\x01\x01", mode=1, profile=1, slots=8, tail=b""):
        return version + bytes([mode, profile, slots]) + b"".join(struct.pack(">H", m) for m in masks) + bytes([count]) + tail
    p8_factory = [0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x1000, 0x2000]
    for count, real in ((4, [0x0003, 0x0002, 0x1000, 0x2000]), (6, [0x0001, 0x0002, 0x0004, 0x0008, 0x1000, 0x2000])):
        chunk11 = usbs11(count, real + p8_factory[count:])
        want = join_chunks(edit_part(b"USBS", lambda p: chunk11))
        dump11 = rewrite_alike("USBS 1.1 with %d outputs" % count, edit_part(b"USBS", lambda p: chunk11), expect=want)
        if not any(l.startswith("usb mode=1 profile=%d slots=%d masks=" % (5 if count == 4 else 6, count)) for l in dump11):
            sys.exit("FAILED: USBS 1.1 with %d outputs reads as %r" % (count, [l for l in dump11 if l.startswith("usb ")]))
        rewrite_alike("USBS 1.2 with appended fields (%d outputs)" % count, edit_part(b"USBS", lambda p: usbs11(count, real + p8_factory[count:], b"\x01\x02", tail=b"\xab" * 5000)), expect=want)
        refused_alike("USBS 2.0 (%d outputs)" % count, edit_part(b"USBS", lambda p: usbs11(count, real + p8_factory[count:], b"\x02\x00")), "newer major version")
        refused_alike("USBS 1.1 a byte short", edit_part(b"USBS", lambda p: chunk11[:-1]))
        refused_alike("USBS 1.1 a byte long", edit_part(b"USBS", lambda p: chunk11 + b"\0"))
        # The view of a v1.0 reader (the count is skipped as a newer-minor field): P8 with the real masks first. Both sides read the count-less 1.0 chunk as P8.
        dump10 = rewrite_alike("USBS 1.1 read as 1.0 (%d outputs)" % count, edit_part(b"USBS", lambda p: b"\x01\x00" + chunk11[2:-1]))
        if not any(l.startswith("usb mode=1 profile=1 slots=8 masks=") for l in dump10):
            sys.exit("FAILED: the 1.0 view of USBS 1.1 is not P8")
    for name, count, profile, slots in (("count 0", 0, 1, 8), ("count 2", 2, 1, 8), ("count 5", 5, 1, 8), ("count 8", 8, 1, 8), ("count 255", 255, 1, 8),
                                        ("P10 with a count", 4, 2, 8), ("P2 with a count", 4, 0, 8)):
        refused_alike("USBS 1.1 " + name, edit_part(b"USBS", lambda p: usbs11(count, p8_factory, profile=profile, slots=slots)))
    for profile in (4, 5, 6, 7, 255):  # a 1.0 chunk holds the profile byte 0..3 only
        refused_alike("USBS 1.0 profile byte %d" % profile, edit_part(b"USBS", lambda p: p[:3] + bytes([profile]) + p[4:]))
    # Order: the state chunks come after LRNB and before the CRC in the order MIDI, USBS, MIXR, PERF, APPM; an unknown chunk may be anywhere after PHDR.
    midi, usbs, mixr, perf, appm_c = sc[m0:m0 + 5]
    refused_alike("MIXR before USBS", sc[:m0] + [midi, mixr, usbs, perf, appm_c] + sc[m0 + 5:])
    refused_alike("MIDI twice", sc[:m0] + [midi, midi, usbs, mixr, perf, appm_c] + sc[m0 + 5:])
    refused_alike("MIDI before INST", [sc[0], midi] + sc[1:m0] + [usbs, mixr, perf, appm_c] + sc[m0 + 5:])
    rewrite_alike("an unknown chunk between the state chunks", sc[:m0 + 3] + [(b"XYZW", b"abc")] + sc[m0 + 3:],
                  expect=join_chunks(sc[:m0 + 5] + [(b"XYZW", b"abc")] + sc[m0 + 5:]))  # unknown chunks are written in the order they were read, after the known
    # Damaged values: both sides say malformed.
    for name, edit in {
        "MIDI a port outside DIN and USB": lambda p: p[:2] + b"\x04" + p[3:],
        "MIDI channel 16": lambda p: p[:4] + b"\x10" + p[5:],
        "MIDI follow 3": lambda p: p[:5] + b"\x03" + p[6:],
        "MIDI CC 128": lambda p: p[:6] + b"\x80" + p[7:],
        "MIDI offset 1001": lambda p: p[:12] + struct.pack(">h", 1001) + p[14:],
        "MIDI offset -1001": lambda p: p[:16] + struct.pack(">h", -1001) + p[18:],
    }.items():
        refused_alike(name, edit_part(b"MIDI", edit))
    for name, edit in {
        "USBS mode 2": lambda p: p[:2] + b"\x02" + p[3:],
        "USBS profile 4": lambda p: p[:3] + b"\x04" + p[4:],
        "USBS slots 7 on P8": lambda p: p[:4] + b"\x07" + p[5:-2],
        "USBS slots 12": lambda p: p[:4] + b"\x0c" + p[5:] + b"\0\0\0\0\0\0\0\0",
        "USBS P10 with 8 slots": lambda p: p[:3] + b"\x02" + p[4:],
    }.items():
        refused_alike(name, edit_part(b"USBS", edit))
    for name, edit in {
        "MIXR 16 strips": lambda p: p[:2] + b"\x10" + p[3:],
        "MIXR route bit 6": lambda p: p[:3] + b"\x40" + p[4:],
        "MIXR the input strip in USB": lambda p: p[:3 + 16] + b"\x20" + p[3 + 17:],
        "MIXR return 4": lambda p: p[:3 + 17] + b"\x04" + p[3 + 18:],
        "MIXR out mode 2": lambda p: p[:3 + 19] + b"\x02" + p[3 + 20:],
        "MIXR cue 101": lambda p: p[:3 + 20] + b"\x65" + p[3 + 21:],
        "MIXR input level 101": lambda p: p[:3 + 21] + b"\x65" + p[3 + 22:],
    }.items():
        refused_alike(name, edit_part(b"MIXR", edit))
    for name, edit in {
        "PERF 11 slots": lambda p: p[:2] + b"\x0b" + p[3:],
        "PERF value 101": lambda p: p[:5] + struct.pack(">h", 101) + p[7:],
        "PERF value -101": lambda p: p[:5] + struct.pack(">h", -101) + p[7:],
    }.items():
        refused_alike(name, edit_part(b"PERF", edit))
    appm = dict(sc)[b"APPM"]
    for name, edit in {
        "APPM no apps": lambda p: p[:2] + b"\x00",
        "APPM 7 apps": lambda p: p[:2] + b"\x07" + p[3:],
        "APPM a name with a slash": lambda p: p[:4] + b"/" + p[5:],
        "APPM an empty name": lambda p: p[:3] + b"\x00" + p[4 + appm[3]:],
        "APPM no values": lambda p: p[:4 + appm[3] + 2] + b"\x00" + p[4 + appm[3] + 3:],
        "APPM 33 values": lambda p: p[:4 + appm[3] + 2] + b"\x21" + p[4 + appm[3] + 3:],
        "APPM NaN": lambda p: p[:4 + appm[3] + 3] + struct.pack(">I", 0x7FC00000) + p[4 + appm[3] + 7:],
        "APPM the same app twice": lambda p: p[:2] + b"\x03" + p[3:3 + 4 + appm[3] + 4 * appm[4 + appm[3] + 2]] + p[3:],
    }.items():
        refused_alike(name, edit_part(b"APPM", edit))

    # APPM 1.1 (OS-APPUX S3): up to twelve apps, written as 1.1 beyond six and as 1.0 up to six; 1.0 never held more than six; both sides agree on every case.
    def appm_apps(n, version=b"\x01\x01"):
        body = bytearray(version + bytes([n]))
        for i in range(n):
            nm = ("app%02d" % i).encode()
            body += bytes([len(nm)]) + nm + bytes([1, i % 5, 2]) + struct.pack(">ff", 1.5 + i, -2.25)
        return bytes(body)
    for n in (7, 12):
        rewrite_alike("APPM 1.1 with %d apps" % n, edit_part(b"APPM", lambda p, n=n: appm_apps(n)))
    rewrite_alike("APPM 1.0 with 6 apps", edit_part(b"APPM", lambda p: appm_apps(6, b"\x01\x00")))
    rewrite_alike("APPM 1.1 with 6 apps (written back as 1.0)", edit_part(b"APPM", lambda p: appm_apps(6)))
    refused_alike("APPM 1.0 with 7 apps", edit_part(b"APPM", lambda p: appm_apps(7, b"\x01\x00")))
    refused_alike("APPM 1.1 with 13 apps", edit_part(b"APPM", lambda p: appm_apps(13)))
    refused_alike("APPM 1.1 with no apps", edit_part(b"APPM", lambda p: appm_apps(0)))
    # PERF: an FX id above the table reads as none (row and values reset) and a row above 3 as off, alike on both sides (a later version may extend the table).
    perf_dump = "\n".join(rewrite_alike("PERF FX 22 and row 4", edit_part(b"PERF", lambda p: p[:3] + b"\x16" + p[4:])))
    if "perf 0 fx=0 row=0 values=0,0,0" not in perf_dump:
        sys.exit("FAILED: PERF FX 22 did not read as none")
    # OS-PERFFX P1, decision D5: the ids 20 (BITDEPTH) and 21 (OVERDRV) are valid in PERF 1.1 only. In 1.1 they are known, with their row and values, and the chunk is written back as 1.1 byte for byte;
    # in a 1.0 chunk they read as none (row and values reset), as an older build reads every 1.1 file; a 1.1 chunk that holds neither is written back as 1.0; an id above 21 is none in 1.1 too. Both sides alike.
    for fx in (20, 21):
        with11 = edit_part(b"PERF", lambda p, fx=fx: b"\x01\x01" + p[2:3] + bytes([fx]) + p[4:])
        perf_dump = "\n".join(rewrite_alike("PERF 1.1 FX %d" % fx, with11, expect=join_chunks(with11)))
        if "perf 0 fx=%d row=2 values=-100,100,1" % fx not in perf_dump:
            sys.exit("FAILED: PERF 1.1 FX %d did not read as itself" % fx)
        perf_dump = "\n".join(rewrite_alike("PERF 1.0 FX %d" % fx, edit_part(b"PERF", lambda p, fx=fx: p[:3] + bytes([fx]) + p[4:])))
        if "perf 0 fx=0 row=0 values=0,0,0" not in perf_dump:
            sys.exit("FAILED: PERF FX %d in a 1.0 chunk did not read as none" % fx)
    rewrite_alike("PERF 1.1 without the ids is written back as 1.0", edit_part(b"PERF", lambda p: b"\x01\x01" + p[2:]), expect=state)
    perf_dump = "\n".join(rewrite_alike("PERF 1.1 FX 22", edit_part(b"PERF", lambda p: b"\x01\x01" + p[2:3] + b"\x16" + p[4:])))
    if "perf 0 fx=0 row=0 values=0,0,0" not in perf_dump:
        sys.exit("FAILED: PERF 1.1 FX 22 did not read as none")
    refused_alike("PERF 1.1 a byte long", edit_part(b"PERF", lambda p: b"\x01\x01" + p[2:] + b"\0"))
    perf_dump = "\n".join(rewrite_alike("PERF row 4", edit_part(b"PERF", lambda p: p[:36] + b"\x04" + p[37:])))
    if "perf 4 fx=5 row=0 " not in perf_dump:
        sys.exit("FAILED: PERF row 4 did not read as off")
    # INST 1.2: an engine above the table reads as automatic (the chunk is then written as 1.1); the engine byte survives in a 1.2 chunk.
    inst0 = [p for c, p in sc if c == b"INST"][0]
    if inst0[-1] != 2:
        sys.exit("FAILED: the first instrument of the state fixture does not end with its engine (Amiga, 2): %r" % inst0[-1:])
    first = True

    def bad_engine(p):
        nonlocal first
        if first and p[:2] == b"\x01\x02":
            first = False
            return p[:-1] + b"\x05"
        return p
    dump_bad = "\n".join(rewrite_alike("INST engine 5 reads as automatic", [(c, bad_engine(p) if c == b"INST" else p) for c, p in sc]))
    if "engine=5" in dump_bad or dump_bad.count("engine=0") != 3 or dump_bad.count("engine=4") != 1:
        sys.exit("FAILED: INST engine 5 did not read as automatic")
    print("state: the malformed state chunks, newer minors, the order and older files are read alike on both sides")

    # OS-INSTUI S8b + S9, INST 1.3: the `midi13` fixture has a sample instrument with an engine and VEL> (1.3 with the engine byte), a MIDI instrument with all twelve fields away from their defaults and one with
    # the offset only. Both sides read and write the same bytes, the version is the smallest that holds what the instrument has, an older file (1.1, 1.2) reads as the defaults, a value out of its range reads
    # as the field's default alike on both sides, a chunk with a byte too many or too few is refused by both, and a newer minor is read as far as 1.3 and written back as 1.3 without its tail.
    m13 = read(os.path.join(work, "midi13.cpp.o3p"))
    mc = split_chunks(m13)
    versions = {p[2]: p[:2] for c, p in mc if c == b"INST"}
    if versions.get(0) != b"\x01\x03" or versions.get(20) != b"\x01\x03" or versions.get(21) != b"\x01\x03" or versions.get(48) != b"\x01\x01":
        sys.exit("FAILED: INST 1.3 only for an instrument with a MIDI field or VEL> that is not its default (%r)" % versions)
    inst20 = [p for c, p in mc if c == b"INST" and p[2] == 20][0]
    if inst20[-12:] != bytes([100, 6, 3, 12, 40, 75, 129, 130, 131, 1, 128, 33]) or inst20[-13] != 0:
        sys.exit("FAILED: the twelve bytes of INST 1.3 are not the description's: %r" % inst20[-13:])
    inst0 = [p for c, p in mc if c == b"INST" and p[2] == 0][0]
    if inst0[-13] != 2 or inst0[-1] != 7:
        sys.exit("FAILED: the 1.3 chunk of an Amiga instrument with VEL> 7 has not its engine and its VEL> in place: %r" % inst0[-13:])
    dump13 = "\n".join(run([tool, "dump", os.path.join(work, "midi13.cpp.o3p")]).stdout.decode().splitlines())
    if "m13=100,6,3,12,40,75,129,130,131,1,128,33" not in dump13 or "m13=127,0,0,2,100,0,0,0,0,0,0,100" not in dump13 or "m13=127,0,0,2,0,0,0,0,0,0,0,7" not in dump13:
        sys.exit("FAILED: the dump of the midi13 fixture does not show the fields")

    def alike13(name, edit, expect_m13=None):
        a, b, c2 = (os.path.join(work, "midi13.%s.o3p" % tag) for tag in ("edit", "py", "cpp2"))
        write(a, join_chunks([(c, edit(c, p)) for c, p in mc]))
        run(py + ["rewrite", a, b])
        run([tool, "resave", a, c2])
        if read(b) != read(c2):
            sys.exit("FAILED: %s: Python and C++ rewrote it differently (first difference at byte %d)" % (name, first_difference(read(b), read(c2))))
        cpp_dump = run([tool, "dump", a]).stdout.decode().splitlines()
        if cpp_dump != run(py + ["dump", a]).stdout.decode().splitlines():
            sys.exit("FAILED: %s: the dumps differ" % name)
        return read(c2), "\n".join(cpp_dump)

    # an older file: the twelve (and the engine byte) taken off, labelled 1.2 / 1.1: defaults, written back as the older minor
    older, dump_old = alike13("INST 1.2", lambda c, p: b"\x01\x02" + p[2:-12] if c == b"INST" and p[:2] == b"\x01\x03" else p)
    if "m13=127,0,0,2,0,0,0,0,0,0,0,100" not in dump_old or "m13=100," in dump_old or [p[:2] for c, p in split_chunks(older) if c == b"INST"].count(b"\x01\x03") != 0:
        sys.exit("FAILED: an INST 1.2 file did not read as the defaults and write back as 1.2 / 1.1")
    older11, _ = alike13("INST 1.1", lambda c, p: b"\x01\x01" + p[2:-13] if c == b"INST" and p[:2] == b"\x01\x03" else p)
    if any(p[:2] == b"\x01\x03" for c, p in split_chunks(older11) if c == b"INST"):
        sys.exit("FAILED: an INST 1.1 file did not write back as 1.1")
    # a value out of its range is the field's default
    def damage(at, value):
        return lambda c, p: p[:-12 + at] + bytes([value]) + p[-11 + at:] if c == b"INST" and p[2] == 20 and at < 11 else (p[:-1] + bytes([value]) if c == b"INST" and p[2] == 20 else p)
    for at, value, want in ((0, 0, "127"), (0, 200, "127"), (1, 129, "0"), (2, 4, "0"), (3, 25, "2"), (4, 101, "0"), (5, 132, "0"), (10, 255, "0"), (11, 101, "100")):
        _, dump_d = alike13("INST 1.3 byte %d = %d" % (at, value), damage(at, value))
        line = [l for l in dump_d.splitlines() if l.startswith("instrument 20 ")][0]
        fields = line.split("m13=")[1].split(",")
        if fields[at] != want:
            sys.exit("FAILED: INST 1.3 byte %d = %d did not read as %s (%r)" % (at, value, want, fields))
    # the length is exact: a byte long or short is malformed on both sides
    refused_13 = {"a byte long": lambda c, p: p + b"\0" if c == b"INST" and p[2] == 20 else p,
                  "a byte short": lambda c, p: p[:-1] if c == b"INST" and p[2] == 20 else p}
    for name, edit in refused_13.items():
        write(path, join_chunks([(c, edit(c, p)) for c, p in mc]))
        cpp = run([tool, "dump", path], expect_ok=False)
        pyr = run(py + ["check", path], expect_ok=False)
        if error_kind(cpp.stderr) != "malformed" or error_kind(pyr.stderr) != "malformed":
            sys.exit("FAILED: INST 1.3 %s: C++ says %r, Python says %r" % (name, error_kind(cpp.stderr), error_kind(pyr.stderr)))
    # a newer minor (1.5; 1.4 is OS-LFOSINE's, no field) with a tail: read as far as 1.3, the tail lost, the twelve kept
    newer, dump_new = alike13("INST 1.5", lambda c, p: b"\x01\x05" + p[2:] + b"\xcd" * 700 if c == b"INST" and p[:2] == b"\x01\x03" else p)
    if newer != m13 or "m13=100,6,3,12,40,75,129,130,131,1,128,33" not in dump_new:
        sys.exit("FAILED: INST 1.5 was not read as far as 1.3 and written back as the 1.3 chunk without its tail")
    # OS-LFOSINE: the LFO shape bytes out of range, on both readers alike. The six LFOs sit 36 bytes before the 96 of the slices and the 19 of the tail (blob length 2, rate 4, engine 1, the twelve
    # of 1.3; a sample instrument has no blob). A 1.3 chunk with a shape of 5 or more, and a 1.4 chunk with one of 6 or more, reads as Triangle (2); a 1.4 chunk keeps 5.
    def shapes_of(p):
        at = len(p) - 19 - 96 - 36
        return [p[at + 6 * k] for k in range(6)]

    def with_shapes(p, shapes, minor):
        at = len(p) - 19 - 96 - 36
        q = bytearray(p)
        q[1] = minor
        for k, s in enumerate(shapes):
            q[at + 6 * k] = s
        return bytes(q)

    def sample_inst(c, p):
        return c == b"INST" and p[:2] == b"\x01\x03" and p[len(p) - 19:len(p) - 17] == b"\x00\x00"

    cases = [(3, [5, 6, 100, 255, 4, 3], [2, 2, 2, 2, 4, 3]), (3, [0, 1, 2, 3, 4, 5], [0, 1, 2, 3, 4, 2]), (4, [5, 6, 7, 255, 0, 1], [5, 2, 2, 2, 0, 1]), (4, [0, 1, 2, 3, 4, 5], [0, 1, 2, 3, 4, 5])]
    for minor, put, want in cases:
        name = "INST 1.%d shapes %r" % (minor, put)
        out, dump = alike13(name, lambda c, p: with_shapes(p, put, minor) if sample_inst(c, p) else p)
        got = [shapes_of(p) for c, p in split_chunks(out) if c == b"INST" and p[1] == minor and p[len(p) - 19:len(p) - 17] == b"\x00\x00"]
        if not got or any(g != want for g in got):
            sys.exit("FAILED: %s: read as %r, wanted %r on every sample instrument" % (name, got, want))
    print("midi13: INST 1.3 is written for a non-default MIDI field or VEL> only, older files read as the defaults, damaged values read as defaults, a newer minor is read alike on both sides")

    # OS-PERSIST2: the session state, NOTE, MUTE, SCEN and SNFX. Tolerant: a value out of its range is its default, alike on both sides (the dumps show what was taken); the shape of a chunk is
    # the only error. An older build keeps the four as unknown chunks and writes them back byte for byte (emulated below, and checked against the real previous reader by hand).
    p2 = read(os.path.join(work, "persist2.cpp.o3p"))
    pc = split_chunks(p2)
    kinds2 = [c for c, _ in pc]
    session = [b"NOTE", b"MUTE", b"SCEN", b"SNFX"]
    s0 = kinds2.index(b"NOTE")
    if kinds2[s0:s0 + 4] != session or kinds2[s0 + 4:] != [b"XTRA", b"EMPT"] or [k for k in kinds2 if k in session] != session:
        sys.exit("FAILED: the persist2 fixture does not hold NOTE, MUTE, SCEN, SNFX in that order before the unknown chunks (%r)" % kinds2)
    if {c: (len(p), p[:2]) for c, p in pc if c in session} != {b"NOTE": (7, b"\x01\x00"), b"MUTE": (19, b"\x01\x00"), b"SCEN": (295, b"\x01\x00"), b"SNFX": (27, b"\x01\x00")}:
        sys.exit("FAILED: the session chunks are not 7, 19, 295 and 27 bytes of version 1.0")
    dump2 = run([tool, "dump", os.path.join(work, "persist2.cpp.o3p")]).stdout.decode().splitlines()
    for line in ("note root=7 scale=9 octave=5 lock=1 jump=4", "mute modes=1,0,0,2,0,0,0,0,0,0,0,0,0,0,0,1", "scenes latch=0 stored=1,0,1",
                 "sendfx values=1,1,18,3500,70,0,1,100,0,77,15,1", "scene 0 slot 0 fx=1 row=2 values=-100,100,1"):
        if line not in dump2:
            sys.exit("FAILED: the persist2 fixture's dump has no %r line" % line)
    if sum(1 for l in dump2 if l.startswith("scene ")) != 24:
        sys.exit("FAILED: the persist2 fixture's dump has not the 24 slots of its two stored scenes")

    def edit2(cid, fn):
        return [(c, fn(p) if c == cid else p) for c, p in pc]

    # An older file (none of the four) reads as no part on both sides and is written back as it was.
    base2 = [(c, p) for c, p in pc if c not in session]
    dump_old = rewrite_alike("no session chunks", base2, expect=join_chunks(base2))
    if any(l.startswith(("note ", "mute ", "scenes ", "scene ", "sendfx ")) for l in dump_old):
        sys.exit("FAILED: a file with no session chunks reads as having some")
    # The build before OS-PERSIST2 (emulated: the four ids are unknown to it): the file loads, the chunks are kept and written back byte for byte, in the order they were read.
    spec = importlib.util.spec_from_file_location("o3project_previous", ref)
    prev = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prev)
    for cid in session:
        prev.RANKS.pop(cid)
        prev.LENGTH_RANGE.pop(cid)
    prev.SESSION_PARTS = ()
    older_view = prev.parse(p2)
    if [c for c, _ in older_view["unknown"]] != session + [b"XTRA", b"EMPT"] or prev.build(older_view) != p2:
        sys.exit("FAILED: a reader that does not know the session chunks does not keep them and write them back byte for byte")
    # The strict chunks of OS-004d next to them are not disturbed: the state fixture's file with the four added after APPM is the same on both sides.
    both = rewrite_alike("state and session chunks together", sc[:m0 + 5] + [x for x in pc if x[0] in session] + sc[m0 + 5:])
    if not (any(l.startswith("midi ") for l in both) and any(l.startswith("sendfx ") for l in both)):
        sys.exit("FAILED: the OS-004d and OS-PERSIST2 chunks together are not both read")

    # OS-PERFFX P1 (PERF and SCEN 1.1, written only with the Perform FX 20 or 21) and OS-APPUX S3 (APPM 1.1, written only with more than six apps) in one project: the gates are independent, each chunk
    # carries its own version. Both sides read every combination and write the file back byte for byte (each chunk as the version its content needs); with all three at 1.1 the dump holds the ids and the seven apps.
    def gates(perf_fx, scen_fx, apps):
        out = []
        for c, p in sc[:m0 + 5] + [x for x in pc if x[0] in session] + sc[m0 + 5:]:
            if c == b"PERF" and perf_fx:
                p = b"\x01\x01" + p[2:3] + bytes([perf_fx]) + p[4:]
            elif c == b"SCEN" and scen_fx:
                p = b"\x01\x01" + p[2:5] + bytes([scen_fx]) + p[6:]
            elif c == b"APPM" and apps:
                p = appm_apps(apps)
            out.append((c, p))
        return out
    for perf_fx, scen_fx, apps in ((0, 0, 0), (20, 21, 0), (0, 0, 7), (20, 21, 7)):
        cs3 = gates(perf_fx, scen_fx, apps)
        dump3 = rewrite_alike("PERF/SCEN 1.1 (FX %d, %d) and APPM 1.1 (%d apps) in one project" % (perf_fx, scen_fx, apps), cs3, expect=join_chunks(cs3))
        if "perf 0 fx=%d row=2 values=-100,100,1" % (perf_fx or 1) not in dump3 or "scene 0 slot 0 fx=%d row=2 values=-100,100,1" % (scen_fx or 1) not in dump3:
            sys.exit("FAILED: the Perform FX of the combined project were not read as themselves (FX %d, %d)" % (perf_fx, scen_fx))
        if sum(1 for l in dump3 if l.startswith("app ")) != (apps or 2):
            sys.exit("FAILED: the combined project does not read %d apps" % (apps or 2))

    def taken(name, cid, edit, line, absent=None):
        """Both sides read the damaged value alike and the dump says what was taken."""
        dumped = rewrite_alike(name, edit2(cid, edit))
        if line not in dumped or (absent is not None and absent in dumped):
            sys.exit("FAILED: %s: the dump does not say %r (%r)" % (name, line, [l for l in dumped if l.startswith(("note", "mute", "scenes", "sendfx"))]))

    def put(payload, at, data):
        return payload[:at] + data + payload[at + len(data):]

    note_ok = "note root=7 scale=9 octave=5 lock=1 jump=4"
    taken("NOTE root 12", b"NOTE", lambda x: put(x, 2, b"\x0c"), "note root=0 scale=9 octave=5 lock=1 jump=4")
    taken("NOTE root 255", b"NOTE", lambda x: put(x, 2, b"\xff"), "note root=0 scale=9 octave=5 lock=1 jump=4")
    taken("NOTE scale 12", b"NOTE", lambda x: put(x, 3, b"\x0c"), "note root=7 scale=0 octave=5 lock=1 jump=4")
    taken("NOTE octave 10", b"NOTE", lambda x: put(x, 4, b"\x0a"), "note root=7 scale=9 octave=3 lock=1 jump=4")
    taken("NOTE lock 2", b"NOTE", lambda x: put(x, 5, b"\x02"), "note root=7 scale=9 octave=5 lock=0 jump=4")
    taken("NOTE jump 33", b"NOTE", lambda x: put(x, 6, b"\x21"), "note root=7 scale=9 octave=5 lock=1 jump=1")
    taken("NOTE everything out", b"NOTE", lambda x: x[:2] + b"\xff" * 5, "note root=0 scale=0 octave=3 lock=0 jump=1")
    taken("NOTE the edges are valid", b"NOTE", lambda x: x[:2] + bytes([11, 11, 9, 1, 32]), "note root=11 scale=11 octave=9 lock=1 jump=32")
    taken("MUTE mode 3", b"MUTE", lambda x: put(x, 3 + 3, b"\x03"), "mute modes=1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1")
    taken("MUTE mode 255", b"MUTE", lambda x: put(x, 3 + 15, b"\xff"), "mute modes=1,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0")
    taken("MUTE 4 tracks", b"MUTE", lambda x: x[:2] + b"\x04" + x[3:3 + 4], "mute modes=1,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0")
    taken("MUTE 1 track", b"MUTE", lambda x: x[:2] + b"\x01" + x[3:4], "mute modes=1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0")
    scen = dict(pc)[b"SCEN"]
    taken("SCEN latch 2", b"SCEN", lambda x: put(x, 2, b"\x02"), "scenes latch=1 stored=1,0,1")
    taken("SCEN stored 2", b"SCEN", lambda x: put(x, 4, b"\x02"), "scenes latch=0 stored=0,0,1", absent="scene 0 slot 0 fx=1 row=2 values=-100,100,1")
    taken("SCEN a scene that is not stored holds junk", b"SCEN", lambda x: put(x, 4 + 97 + 1, b"\x07\x03\x00\x01\x00\x02\x00\x03"), "scenes latch=0 stored=1,0,1")
    taken("SCEN FX 22", b"SCEN", lambda x: put(x, 4 + 1, b"\x16"), "scene 0 slot 0 fx=0 row=0 values=0,0,0")
    # OS-PERFFX P1, decision D5: the ids 20 and 21 are kept, with their row and values, in a 1.1 chunk (written back as 1.1, byte for byte) and are none in a 1.0 one, on both sides.
    for fx in (20, 21):
        taken("SCEN 1.1 FX %d" % fx, b"SCEN", lambda x, fx=fx: put(b"\x01\x01" + x[2:], 4 + 1, bytes([fx])), "scene 0 slot 0 fx=%d row=2 values=-100,100,1" % fx)
        with11 = edit2(b"SCEN", lambda x, fx=fx: put(b"\x01\x01" + x[2:], 4 + 1, bytes([fx])))
        rewrite_alike("SCEN 1.1 FX %d written back as 1.1" % fx, with11, expect=join_chunks(with11))
        taken("SCEN 1.0 FX %d" % fx, b"SCEN", lambda x, fx=fx: put(x, 4 + 1, bytes([fx])), "scene 0 slot 0 fx=0 row=0 values=0,0,0")
    rewrite_alike("SCEN 1.1 without the ids is written back as 1.0", edit2(b"SCEN", lambda x: b"\x01\x01" + x[2:]), expect=p2)
    taken("SCEN 1.1 FX 22", b"SCEN", lambda x: put(b"\x01\x01" + x[2:], 4 + 1, b"\x16"), "scene 0 slot 0 fx=0 row=0 values=0,0,0")
    taken("SCEN row 4", b"SCEN", lambda x: put(x, 4 + 1 + 1, b"\x04"), "scene 0 slot 0 fx=1 row=0 values=-100,100,1")
    taken("SCEN value 101", b"SCEN", lambda x: put(x, 4 + 1 + 2, struct.pack(">h", 101)), "scene 0 slot 0 fx=1 row=2 values=0,100,1")
    taken("SCEN value -101", b"SCEN", lambda x: put(x, 4 + 1 + 4, struct.pack(">h", -101)), "scene 0 slot 0 fx=1 row=2 values=-100,0,1")
    taken("SCEN value 32767", b"SCEN", lambda x: put(x, 4 + 1 + 6, struct.pack(">h", 32767)), "scene 0 slot 0 fx=1 row=2 values=-100,100,0")
    taken("SCEN the edges are valid", b"SCEN", lambda x: put(x, 4 + 1 + 2, struct.pack(">3h", 100, -100, 0)), "scene 0 slot 0 fx=1 row=2 values=100,-100,0")
    sendfx = "sendfx values=1,1,18,3500,70,0,1,100,0,77,15,1"
    taken("SNFX DLY RATE 19", b"SNFX", lambda x: put(x, 3 + 2 * 2, struct.pack(">H", 19)), "sendfx values=1,1,0,3500,70,0,1,100,0,77,15,1")
    taken("SNFX DLY TIME 0", b"SNFX", lambda x: put(x, 3 + 2 * 3, struct.pack(">H", 0)), "sendfx values=1,1,18,500,70,0,1,100,0,77,15,1")
    taken("SNFX DLY TIME 3501", b"SNFX", lambda x: put(x, 3 + 2 * 3, struct.pack(">H", 3501)), "sendfx values=1,1,18,500,70,0,1,100,0,77,15,1")
    taken("SNFX DLY FEEDBACK 71", b"SNFX", lambda x: put(x, 3 + 2 * 4, struct.pack(">H", 71)), "sendfx values=1,1,18,3500,50,0,1,100,0,77,15,1")
    taken("SNFX DLY MODE 2", b"SNFX", lambda x: put(x, 3 + 2 * 6, struct.pack(">H", 2)), "sendfx values=1,1,18,3500,70,0,0,100,0,77,15,1")
    taken("SNFX RVB DIFFUSE 65535", b"SNFX", lambda x: put(x, 3 + 2 * 10, struct.pack(">H", 65535)), "sendfx values=1,1,18,3500,70,0,1,100,0,77,80,1")
    taken("SNFX RVB QUALITY 2", b"SNFX", lambda x: put(x, 3 + 2 * 11, struct.pack(">H", 2)), "sendfx values=1,1,18,3500,70,0,1,100,0,77,15,0")
    taken("SNFX 5 rows", b"SNFX", lambda x: x[:2] + b"\x05" + x[3:3 + 10], "sendfx values=1,1,18,3500,70,100,0,50,50,50,80,0")
    taken("SNFX 14 rows (two of a later version)", b"SNFX", lambda x: x[:2] + b"\x0e" + x[3:] + struct.pack(">2H", 7, 9), sendfx)
    taken("SNFX 32 rows", b"SNFX", lambda x: x[:2] + b"\x20" + x[3:] + b"\x00\x07" * 20, sendfx)
    taken("SNFX 1 row", b"SNFX", lambda x: x[:2] + b"\x01" + x[3:5], "sendfx values=1,0,0,500,50,100,0,50,50,50,80,0")
    taken("SNFX the edges are valid", b"SNFX", lambda x: x[:3] + struct.pack(">12H", 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0), "sendfx values=0,0,0,1,0,0,0,0,0,0,0,0")
    # The shape of a chunk is the only error: both sides say malformed (a newer major: refused).
    for name, edit in {
        "NOTE a byte short": (b"NOTE", lambda x: x[:-1]),
        "NOTE a byte long": (b"NOTE", lambda x: x + b"\0"),
        "NOTE a byte over, labelled 1.0": (b"NOTE", lambda x: x + b"\x01\x02\x03"),
        "NOTE version 0": (b"NOTE", lambda x: b"\x00\x00" + x[2:]),
        "MUTE no tracks": (b"MUTE", lambda x: x[:2] + b"\x00" + x[3:]),
        "MUTE 17 tracks": (b"MUTE", lambda x: x[:2] + b"\x11" + x[3:] + b"\0"),
        "MUTE count above its length": (b"MUTE", lambda x: x[:2] + b"\x10" + x[3:-1]),
        "MUTE a byte long": (b"MUTE", lambda x: x + b"\0"),
        "MUTE version 0": (b"MUTE", lambda x: b"\x00\x00" + x[2:]),
        "SCEN 2 scenes": (b"SCEN", lambda x: x[:3] + b"\x02" + x[4:]),
        "SCEN 4 scenes": (b"SCEN", lambda x: x[:3] + b"\x04" + x[4:]),
        "SCEN no scenes": (b"SCEN", lambda x: x[:3] + b"\x00" + x[4:]),
        "SCEN a byte short": (b"SCEN", lambda x: x[:-1]),
        "SCEN a byte long": (b"SCEN", lambda x: x + b"\0"),
        "SCEN much too short": (b"SCEN", lambda x: x[:10]),
        "SCEN version 0": (b"SCEN", lambda x: b"\x00\x00" + x[2:]),
        "SNFX no rows": (b"SNFX", lambda x: x[:2] + b"\x00"),
        "SNFX 33 rows": (b"SNFX", lambda x: x[:2] + b"\x21" + x[3:] + b"\0" * 42),
        "SNFX rows above its length": (b"SNFX", lambda x: x[:2] + b"\x0d" + x[3:]),
        "SNFX rows below its length": (b"SNFX", lambda x: x[:2] + b"\x0b" + x[3:]),
        "SNFX an odd tail": (b"SNFX", lambda x: x + b"\0"),
        "SNFX version 0": (b"SNFX", lambda x: b"\x00\x00" + x[2:]),
    }.items():
        refused_alike(name, edit2(edit[0], edit[1]))
    for cid in session:
        refused_alike("%s 2.0" % cid.decode(), edit2(cid, lambda x: b"\x02\x00" + x[2:] + b"\0" * 33), "newer major version")
        refused_alike("%s 2.0, 3 bytes" % cid.decode(), edit2(cid, lambda x: b"\x02\x00\x00"), "newer major version")
        # A newer minor only appends: read as far as 1.0 goes, written back without the rest, at any length.
        nm = b"\x01\x02" if cid == b"SCEN" else b"\x01\x01"  # SCEN is at 1.1 (OS-PERFFX P1): its next minor is 1.2
        rewrite_alike("%s %d.%d with appended fields" % (cid.decode(), nm[0], nm[1]), edit2(cid, lambda x: nm + x[2:] + b"\xab" * 5000), expect=p2)
        rewrite_alike("%s %d.%d with one appended byte" % (cid.decode(), nm[0], nm[1]), edit2(cid, lambda x: nm + x[2:] + b"\xab"), expect=p2)
        refused_alike("%s %d.%d shorter than 1.0" % (cid.decode(), nm[0], nm[1]), edit2(cid, lambda x: nm + x[2:3]))
    # Order: the session chunks follow APPM (and the other state chunks) in the order NOTE, MUTE, SCEN, SNFX, each once, before the unknown chunks and the CRC.
    note_c, mute_c, scen_c, snfx_c = pc[s0:s0 + 4]
    refused_alike("MUTE before NOTE", pc[:s0] + [mute_c, note_c, scen_c, snfx_c] + pc[s0 + 4:])
    refused_alike("SNFX before SCEN", pc[:s0] + [note_c, mute_c, snfx_c, scen_c] + pc[s0 + 4:])
    refused_alike("NOTE twice", pc[:s0] + [note_c, note_c, mute_c, scen_c, snfx_c] + pc[s0 + 4:])
    refused_alike("SNFX twice", pc[:s0] + [note_c, mute_c, scen_c, snfx_c, snfx_c] + pc[s0 + 4:])
    refused_alike("NOTE before INST", [pc[0], note_c] + pc[1:s0] + [mute_c, scen_c, snfx_c] + pc[s0 + 4:])
    refused_alike("NOTE before APPM", sc[:m0 + 4] + [note_c] + sc[m0 + 4:m0 + 5] + [mute_c, scen_c, snfx_c] + sc[m0 + 5:])
    # Unknown chunks are kept next to them: one anywhere among the known chunks is written back after them, in the order it was read.
    rewrite_alike("an unknown chunk between the session chunks", pc[:s0 + 2] + [(b"ZZZZ", b"abc")] + pc[s0 + 2:],
                  expect=join_chunks(pc[:s0 + 4] + [(b"ZZZZ", b"abc")] + pc[s0 + 4:]))
    keep = rewrite_alike("an unknown chunk before the session chunks", pc[:s0] + [(b"ZZZZ", b"abc")] + pc[s0:],
                         expect=join_chunks(pc[:s0 + 4] + [(b"ZZZZ", b"abc")] + pc[s0 + 4:]))
    if not any(l.startswith("unknown ") and "id=ZZZZ" in l for l in keep):
        sys.exit("FAILED: an unknown chunk next to the session chunks was not kept")
    print("session state: the tolerant readings, the malformed chunks, newer minors, the order, older files, a previous reader and unknown chunks are read alike on both sides")
    # OS-BUG-PASTE24: the LAW chunk, read alike on both sides, damaged or not. The base is the og fixture with the law (an OG v16 import: OGPM and OGPN after LAW) and the persist2 fixture (NOTE..SNFX before it).
    lw = split_chunks(read(os.path.join(work, "law.cpp.o3p")))
    law_file = read(os.path.join(work, "law.cpp.o3p"))
    law_at = [c for c, _ in lw].index(b"LAW ")

    def law_with(payload):
        return lw[:law_at] + [(b"LAW ", payload)] + lw[law_at + 1:]

    if join_chunks(lw) != law_file:
        sys.exit("FAILED: the law fixture is not what its chunks say")
    no_law = rewrite_alike("law 0 present", law_with(b"\x01\x00\x00"), expect=join_chunks(lw[:law_at] + lw[law_at + 1:]))
    if any(l.startswith("law") for l in no_law):
        sys.exit("FAILED: a LAW chunk of law 0 is read as law 1")
    for name, payload in {"law 2": b"\x01\x00\x02", "law 255": b"\x01\x00\xff", "length 2": b"\x01\x00", "length 4 at 1.0": b"\x01\x00\x01\x00", "version 0": b"\x00\x00\x01",
                          "law 2 in a newer minor": b"\x01\x01\x02\xab\xcd", "a newer minor shorter than 1.0": b"\x01\x01"}.items():
        refused_alike("LAW " + name, law_with(payload))
    refused_alike("LAW 2.0", law_with(b"\x02\x00\x01"), "newer major version")
    rewrite_alike("LAW 1.1 with appended fields", law_with(b"\x01\x01\x01" + b"\xab" * 1000), expect=law_file)
    rewrite_alike("LAW 1.1 with one appended byte", law_with(b"\x01\x01\x01\xab"), expect=law_file)
    refused_alike("LAW twice", lw[:law_at] + [lw[law_at], lw[law_at]] + lw[law_at + 1:])
    # Order: LAW is the last known chunk; before SNFX or anything of a lower rank it is refused, after the unknown chunks it is legal (a build older than this one re-saves it so) and is written back before them.
    p2_law = [(b"LAW ", b"\x01\x00\x01")]
    refused_alike("LAW before NOTE", pc[:s0] + p2_law + pc[s0:])
    refused_alike("LAW before SNFX", pc[:s0 + 3] + p2_law + pc[s0 + 3:])
    lawful = rewrite_alike("LAW after SNFX", pc[:s0 + 4] + p2_law + pc[s0 + 4:], expect=join_chunks(pc[:s0 + 4] + p2_law + pc[s0 + 4:]))
    if "law=1" not in lawful:
        sys.exit("FAILED: LAW after SNFX is not read")
    rewrite_alike("LAW after the unknown chunks", pc + p2_law, expect=join_chunks(pc[:s0 + 4] + p2_law + pc[s0 + 4:]))
    # The build before this slice (emulated: LAW is unknown to it): the law-1 file loads, LAW is kept as the first unknown chunk and written back byte for byte, in the order it was read
    # (the real previous o3pj_tool and o3project.py were run on this file by hand: the commit and the result are in the commit body of the slice).
    spec_law = importlib.util.spec_from_file_location("o3project_before_law", ref)
    prev = importlib.util.module_from_spec(spec_law)
    spec_law.loader.exec_module(prev)
    prev.RANKS.pop(b"LAW ")
    prev.LENGTH_RANGE.pop(b"LAW ")
    prev.LAW_PARTS = ()
    prev.RANKS[b"CRC "] = 18
    for what, data in (("the law fixture", law_file), ("persist2 with LAW", join_chunks(pc[:s0 + 4] + p2_law + pc[s0 + 4:]))):
        older_law = prev.parse(data)
        if [c for c, _ in older_law["unknown"]][0] != b"LAW " or prev.build(older_law) != data:
            sys.exit("FAILED: %s: a reader that does not know LAW does not keep it and write it back byte for byte" % what)
    print("law: the LAW chunk is read alike on both sides (law 0 or 1, damaged forms, newer minors, order), the files of law 0 are the bytes of the build before, a previous reader keeps it byte for byte")
    print("versions: %d newer-major cases refused, %d newer-minor cases read, INST 1.0 read as rate 0, alike" % (len(refused) + 1, len(accepted) + 3))
    print("OK")


if __name__ == "__main__":
    main()
