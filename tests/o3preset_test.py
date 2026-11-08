#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest `preset_python` (OS-MODPRESET P2): tools/o3preset.py against the hex fixture the C++ reader and writer share (standard library only).

usage: o3preset_test.py REPO_ROOT [CORPUS_DIR]

  - `make` of the reference preset gives the bytes of tests/data/preset_basic.hex (which tests/preset_format_test.cpp holds the C++ writer equal to,
    so the two implementations write the same bytes), `dump` of it prints every field, `parse` and `build` give each other back;
  - the CRC is zlib.crc32;
  - good and damaged files (cut at every byte, every bit flipped, bad magic, CRC off by one bit, newer major, newer minor, a NaN, a 16-byte name, a
    count of 33, two MPST, repeated names, an unknown chunk, over 1,024 bytes, ...) give the error kinds the spec says (each is a check below);
  - with CORPUS_DIR, every file it tried is written there as corpus.txt with the outcome this reader gave (checked above): one line a file,
    `<n> <ok|bad_magic|truncated|bad_CRC|newer_major_version|malformed|too_big> <hex|-> [summary]`, where the summary of an ok file is
    `<module hex>:<kind>:<module version>:<duplicates>:<unknown chunks>:<newer minor 0|1>` and one `<name hex>:<value bits hex>` per entry.
    tests/preset_format_test.cpp (ctest fixture `preset_corpus`, set up by this test) reads the file and holds the C++ reader to the same outcomes;
  - `pack` refuses a non-finite value, a name over 15 bytes, more than 32 entries, a bad file name (`..`, a Windows device name), a file name
    twice, and writes nothing then; what it writes reads back.
"""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import zlib

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAILED: " + what)


def load_module(root):
    import importlib.util
    spec = importlib.util.spec_from_file_location("o3preset", os.path.join(root, "tools", "o3preset.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_hex(path):
    """tests/data/preset_basic.hex: hex digit pairs, whitespace and lines starting with # ignored (the C++ test's reader)."""
    out = bytearray()
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            out += bytes(int(w, 16) for w in line.split())
    return bytes(out)


# A file from the spec, with its own writer (not the tool's): chunks as (id, payload), then the CRC chunk, which may be a newer minor.
def build(chunks, crc_version=0x0100, crc_tail=0):
    body = b"".join(c + struct.pack(">I", len(p)) + p + (b"\0" if len(p) & 1 else b"") for c, p in chunks)
    crc_len = 6 + crc_tail
    form = b"FORM" + struct.pack(">I", 4 + len(body) + 8 + crc_len + (crc_len & 1)) + b"O3MP" + body
    crc = zlib.crc32(form) & 0xFFFFFFFF
    return form + b"CRC " + struct.pack(">IHI", crc_len, crc_version, crc) + b"\xcd" * crc_tail + (b"\0" if crc_len & 1 else b"")


def s(text):
    raw = text if isinstance(text, bytes) else text.encode("latin-1")
    return bytes([len(raw)]) + raw


def mpst(stem="tb303", kind=1, modver=7, ents=None, count=None, version=0x0100, tail=b""):
    ents = REF_ENTRIES if ents is None else ents
    p = struct.pack(">H", version) + s(stem) + bytes([kind]) + struct.pack(">I", modver) + bytes([len(ents) if count is None else count])
    for name, bits in ents:
        p += s(name) + struct.pack(">I", bits)
    return p + tail


def fbits(f):
    return struct.unpack(">I", struct.pack(">f", f))[0]


REF_ENTRIES = [("Cutoff", fbits(0.5)), ("Resonance", fbits(0.25)), ("Env Mod", fbits(1.0)), ("Decay", fbits(2.5))]
REF_PRESET = {"module": "tb303", "kind": 1, "module_version": 0x00010000,
              "entries": [("Cutoff", 0.5), ("Resonance", 0.25), ("Env Mod", 1.0), ("Decay", 2.5)]}


def kind_of(op):
    """The FormatError kind of op() ('ok' when it does not raise)."""
    return outcome_of(op)[0]


def outcome_of(op):
    """(the FormatError kind of op(), its detail text); ('ok', '') when it does not raise."""
    try:
        op()
    except OP.FormatError as exc:
        return exc.kind, exc.detail
    return "ok", ""


def run_tool(args, expect_rc=0):
    done = subprocess.run([sys.executable, "-I", TOOL] + args, capture_output=True)
    check(done.returncode == expect_rc, "o3preset %s: exit %d, wanted %d (%s)" % (" ".join(args[:1]), done.returncode, expect_rc, done.stderr.decode("utf-8", "replace").strip()))
    return done


CORPUS = {}  # data -> outcome, in the order tried: what main() writes for the C++ test


def write_corpus(path):
    names = {"ok": "ok", "bad magic": "bad_magic", "truncated": "truncated", "bad CRC": "bad_CRC", "newer major version": "newer_major_version",
             "malformed": "malformed", "too big": "too_big"}
    lines = []
    for n, (data, kind, detail) in enumerate(CORPUS.values()):
        line = "%d %s %s" % (n, names[kind], data.hex() or "-")
        if detail:  # a case that pins the branch of the reader that refuses it: the text both readers give
            line += " detail=" + detail.encode("latin-1").hex()
        if kind == "ok":
            p = OP.parse(data)
            line += " %s:%d:%d:%d:%d:%d" % (p["module"].encode("latin-1").hex(), p["kind"], p["module_version"], p["duplicates"], p["unknown_chunks"],
                                             1 if p["newer_minor"] else 0)
            for name, value in p["entries"]:
                line += " %s:%08x" % (name.encode("latin-1").hex(), OP.bits_of(value))
        lines.append(line)
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, "corpus.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    global OP, TOOL
    root = sys.argv[1]
    TOOL = os.path.join(root, "tools", "o3preset.py")
    OP = load_module(root)
    ref = read_hex(os.path.join(root, "tests", "data", "preset_basic.hex"))
    check(len(ref) == 96, "the fixture is 96 bytes, not %d" % len(ref))
    rd = lambda data: OP.parse(data)

    def k(data, detail=None):
        """The FormatError kind of reading `data` ('ok' when it reads); the file and the outcome go into the corpus. With `detail`, the reader's
        text for the refusal must be that one (the branch it takes), and the C++ reader is held to it too."""
        kind, text = outcome_of(lambda: rd(data))
        if detail is not None:
            check(text == detail, "the refusal of %s is %r, wanted the branch %r" % (data[:24].hex(), text, detail))
        old = CORPUS.get(bytes(data))
        CORPUS[bytes(data)] = (bytes(data), kind, detail or (old[2] if old else None))
        return kind

    with tempfile.TemporaryDirectory() as tmp:
        # --- the reference file: make, dump, parse, build
        out = os.path.join(tmp, "ref.o3mp")
        run_tool(["make", "--module", "tb303", "--kind", "instrument", "--module-version", "0x10000", "--set", "Cutoff=0.5", "--set", "Resonance=0.25",
                  "--set", "Env Mod=1.0", "--set", "Decay=2.5", "-o", out])
        with open(out, "rb") as f:
            made = f.read()
        check(made == ref, "make of the reference preset is not the bytes of the hex fixture")
        check(OP.build(REF_PRESET) == ref, "build of the reference preset is not the hex fixture")
        check(build([(b"MPST", mpst(modver=0x00010000))]) == ref, "the test's own writer disagrees with the hex fixture")
        check(ref[-4:] == struct.pack(">I", zlib.crc32(ref[:-14]) & 0xFFFFFFFF), "the CRC is not zlib.crc32 of every byte before the CRC chunk")
        parsed = rd(ref)
        check(parsed["module"] == "tb303" and parsed["kind"] == 1 and parsed["module_version"] == 0x00010000, "parse: the header fields")
        check(parsed["entries"] == REF_PRESET["entries"], "parse: the entries")
        check(parsed["duplicates"] == 0 and parsed["unknown_chunks"] == 0 and not parsed["newer_minor"], "parse: nothing to report on the reference")
        check(OP.build(parsed) == ref, "build(parse(reference)) is not the reference")
        text = run_tool(["dump", out]).stdout.decode()
        for needle in ("module: tb303", "kind: instrument (1)", "module_version: 0x00010000", "count: 4", "[0] Cutoff = 0.5 (0x3f000000)", "[1] Resonance = 0.25 (0x3e800000)",
                       "[2] Env Mod = 1 (0x3f800000)", "[3] Decay = 2.5 (0x40200000)"):
            check(needle in text, "dump lacks %r:\n%s" % (needle, text))
        check(text == OP.dump(parsed), "the command's dump is the library's")

        # --- round trips of what the C++ test round-trips
        def entries(n, long_names):
            return [(("p" * 12 + "P" + chr(97 + i // 26) + chr(97 + i % 26)) if long_names else ("P" + chr(65 + i // 26) + chr(97 + i % 26)),
                     struct.unpack(">f", struct.pack(">I", 0x3F800000 + i * 0x1234))[0]) for i in range(n)]
        for n in (0, 1, 2, 31, 32):
            for long_names in (False, True):
                p = {"module": "0123456789012345678901234567890" if long_names else "fm", "kind": 2, "module_version": 0xFFFFFFFF, "entries": entries(n, long_names)}
                data = OP.build(p)
                back = rd(data)
                check(back["entries"] == p["entries"] and back["module"] == p["module"] and back["kind"] == 2, "round trip of %d entries" % n)
                check(k(data) == "ok", "the written file of %d entries reads" % n)
                check(OP.build(back) == data, "rewrite of %d entries" % n)
                check(data == build([(b"MPST", mpst(p["module"], 2, 0xFFFFFFFF, [(k, fbits(v)) for k, v in p["entries"]]))]), "build of %d entries is not the spec's bytes" % n)
        check(len(OP.build({"module": "0123456789012345678901234567890", "kind": 2, "module_version": 0, "entries": entries(32, True)})) == 714, "the largest file is 714 bytes")
        check(k(ref) == "ok", "the reference file reads")
        check(len(OP.build({"module": "a", "kind": 1, "module_version": 0, "entries": []})) == 44, "the smallest file is 44 bytes")
        pats = [0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x80800000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F000000,
                0x3DCCCCCD, 0x42C80000, 0x40490FDB]
        p = {"module": "chip", "kind": 1, "module_version": 0, "entries": [("v%d" % i, OP.float_of(b)) for i, b in enumerate(pats)]}
        back = rd(OP.build(p))
        k(OP.build(p))
        check([OP.bits_of(v) for _, v in back["entries"]] == pats, "float bit patterns (-0.0, denormals, extremes) are not carried bit for bit")

        # --- the refusals of the C++ test, with the same kinds
        mk = lambda **kw: build([(b"MPST", mpst(**kw))])
        for n in range(len(ref)):
            if k(ref[:n]) != "truncated":
                check(False, "a cut at %d bytes is %s, not truncated" % (n, k(ref[:n])))
                break
        crc_minor = len(ref) - 6 + 1
        for i in range(len(ref)):
            for bit in range(8):
                f = bytearray(ref)
                f[i] ^= 1 << bit
                got = k(bytes(f))
                if i == crc_minor:
                    if got != "ok" or not rd(bytes(f))["newer_minor"]:
                        check(False, "a newer minor of the CRC chunk (byte %d bit %d) was %s" % (i, bit, got))
                elif got == "ok":
                    check(False, "flipping bit %d of byte %d was not refused" % (bit, i))
        check(k(b"G" + ref[1:]) == "bad magic", "bad magic")
        check(k(ref[:8] + b"O3ST" + ref[12:]) == "bad magic", "an O3ST FORM")
        check(k(b"hello world!!!!") == "bad magic" and k(b"hi!!") == "bad magic", "text is bad magic")
        check(k(b"hello") == "bad magic" and k(b"hello world!" * 200) == "bad magic", "the magic is checked first: five bytes or 2,400 bytes of text are bad magic, not too big")
        check(k(b"FORM") == "truncated" and k(b"FORM\0\0\0\x58O3M") == "truncated" and k(b"FORM\0\0\0\x58O3MP") == "truncated", "FORM and fewer than twelve bytes is truncated")
        # the container's own branches, each pinned by the text it is refused with (the C++ reader must take the same one)
        body = ref[:-14]  # the reference without its CRC chunk: FORM header, MPST
        unknown = b"XTRA" + struct.pack(">I", 0)
        notlast = bytearray(ref + unknown)  # an unknown chunk AFTER the CRC chunk, inside the FORM
        notlast[4:8] = struct.pack(">I", len(notlast) - 8)
        check(k(bytes(notlast), "the CRC chunk is not the last") == "malformed", "the CRC chunk not last")
        for stray in (2, 4, 6):  # fewer than eight bytes after MPST where the next chunk's header should be (an even FORM length)
            f = bytearray(body + b"\0" * stray)
            f[4:8] = struct.pack(">I", len(f) - 8)
            check(k(bytes(f), "chunk header overruns the FORM") == "malformed", "chunk header overruns the FORM: %d stray bytes" % stray)
        for stray in (1, 3):  # an odd FORM length
            f = bytearray(body + b"\0" * stray)
            f[4:8] = struct.pack(">I", len(f) - 8)
            check(k(bytes(f), "bad FORM length") == "malformed", "an odd FORM length: %d stray bytes" % stray)
        check(k(b"RIFF" + ref[4:]) == "bad magic" and k(b"FORM" + ref[4:8] + b"WAVE" + ref[12:]) == "bad magic", "another container")
        check(k(ref + b"Z" * 3000) == "too big", "the right magic and over 1,024 bytes is too big")
        check(k(b"") == "truncated" and k(b"FOR") == "truncated", "empty and short are truncated")
        for bit in range(32):
            f = bytearray(ref)
            f[-4 + (bit >> 3)] ^= 1 << (bit & 7)
            check(k(bytes(f)) == "bad CRC", "a CRC off by bit %d" % bit)
        f = bytearray(ref)
        f[40] ^= 1
        check(k(bytes(f)) == "bad CRC", "a damaged body is a bad CRC")
        for last, want in ((0x57, "malformed"), (0x02, "malformed"), (0x5A, "truncated")):
            f = bytearray(ref)
            f[7] = last
            check(k(bytes(f)) == want, "FORM length low byte %#x is %s, wanted %s" % (last, k(bytes(f)), want))
        f = bytearray(ref)
        f[8 + 12 + 61] = 1
        check(k(bytes(f)) == "malformed", "a pad byte that is not zero")
        f = bytearray(ref)
        f[19] = 0x7F
        check(k(bytes(f)) == "malformed", "a chunk length past the FORM")
        f = bytearray(ref[:-14])
        f[7] = len(f) - 8
        check(k(bytes(f)) == "malformed", "no CRC chunk")
        check(k(build([(b"MPST", mpst())], 0x0100, 2)) == "malformed", "a CRC chunk of the wrong length at 1.0")
        check(k(build([(b"\x01ABC", b"\0\0"), (b"MPST", mpst())])) == "malformed", "an unprintable chunk id")
        check(k(build([])) == "malformed" and k(build([(b"XYZW", b"\0\0")])) == "malformed", "no MPST")
        check(k(build([(b"MPST", mpst()), (b"MPST", mpst())])) == "malformed", "two MPST")
        for version, want in ((0x0000, "malformed"), (0x0050, "malformed"), (0x0200, "newer major version"), (0xFF00, "newer major version")):
            check(k(mk(version=version)) == want, "MPST version %#06x is %s, wanted %s" % (version, k(mk(version=version)), want))
        check(k(build([(b"MPST", b"\x02\x00")])) == "newer major version" and k(build([(b"MPST", b"\x01")])) == "malformed", "short MPST chunks")
        check(k(build([(b"MPST", mpst())], 0x0200)) == "newer major version" and k(build([(b"MPST", mpst())], 0x0000)) == "malformed", "the CRC chunk's version")
        for stem in ("", ".", "..", "a" * 32, "a/b", "a\\b", "a:b", "a*b", "a?b", 'a"b', "a<b", "a>b", "a|b", "a\tb", "a\x7f", "a\0b"):
            check(k(mk(stem=stem)) == "malformed", "stem %r" % stem)
        for stem in ("a" * 31, "a.b", "My Module"):
            check(k(mk(stem=stem)) == "ok", "stem %r is fine" % stem)
        for kind in (0, 3, 4, 255):
            check(k(mk(kind=kind)) == "malformed", "kind %d" % kind)
        check(k(mk(kind=2)) == "ok", "kind effect")
        many = [("p%d" % i, fbits(1.0)) for i in range(33)]
        check(k(mk(ents=many)) == "malformed" and k(mk(ents=many[:32])) == "ok", "33 entries, 32 entries")
        check(k(mk(count=5)) == "malformed" and k(mk(count=255)) == "malformed" and k(mk(count=3)) == "malformed", "a count that is not the entries")
        check(k(mk(ents=[], count=0)) == "ok" and k(mk(ents=[], count=1)) == "malformed", "no entries")
        for name in ("", "n" * 16, "a\x1f", "a\x7f", "\xe9", "a\0b", "\n"):
            check(k(mk(ents=[("ok", fbits(1.0)), (name, fbits(1.0))])) == "malformed", "entry name %r" % name)
        check(k(mk(ents=[("n" * 15, fbits(1.0)), (" ", fbits(1.0)), ("~", fbits(1.0))])) == "ok", "15-byte, space and tilde names are fine")
        for bits in (0x7FC00000, 0xFFC00000, 0x7F800001, 0x7FFFFFFF, 0x7F800000, 0xFF800000):
            check(k(mk(ents=[("a", fbits(1.0)), ("b", bits)])) == "malformed", "value %#010x" % bits)
        check(k(mk(ents=[("a", 0x80000000), ("b", 1), ("c", 0x7F7FFFFF)])) == "ok", "-0.0, a denormal and the largest float are fine")
        whole = mpst()
        check(all(k(build([(b"MPST", whole[:n])])) == "malformed" for n in range(len(whole))), "a payload cut anywhere is malformed")
        check(k(mk(tail=b"\x01")) == "malformed" and k(mk(tail=b"\0\0")) == "malformed", "bytes after the last entry at 1.0")
        # repeated names: every entry is kept in file order, the repeats are counted (a card module may have two parameters of one name)
        dups = [("Cutoff", fbits(0.5)), ("Decay", fbits(1.0)), ("Cutoff", fbits(0.9)), ("Decay", fbits(2.0)), ("Cutoff", fbits(0.1)), ("cutoff", fbits(3.0))]
        check(k(mk(ents=dups)) == "ok", "repeated names read")
        got = rd(mk(ents=dups))
        check(got["duplicates"] == 3 and got["entries"] == [(n, OP.float_of(b)) for n, b in dups], "duplicates: every entry kept in order, repeats counted, names compared exactly")
        check(OP.build(got) == mk(ents=dups), "a preset with repeated names writes and reads back")
        same32 = [("x", fbits(float(i))) for i in range(32)]
        got = rd(mk(ents=same32))
        check(k(mk(ents=same32)) == "ok" and got["duplicates"] == 31 and len(got["entries"]) == 32, "32 entries of one name: all kept, 31 counted")
        # newer minor: read as 1.0
        for extra in (1, 2, 3, 7, 100):
            got = rd(mk(version=0x0101, tail=b"\xee" * extra))
            check(got["newer_minor"] and got["entries"] == REF_PRESET["entries"], "a newer minor with %d appended bytes" % extra)
        check(rd(mk(version=0x01FF))["newer_minor"], "1.255")
        check(k(mk(kind=3, version=0x0101, tail=b"\0" * 4)) == "malformed" and k(mk(count=9, version=0x0101, tail=b"\0" * 4)) == "malformed", "a newer minor does not excuse a bad field")
        for tail in (0, 1, 2, 3, 30):
            check(rd(build([(b"MPST", mpst())], 0x0101, tail))["newer_minor"], "a newer-minor CRC chunk with a %d byte tail" % tail)
        # an unknown chunk: ignored and counted
        got = rd(build([(b"NAME", b"hi!"), (b"MPST", mpst()), (b"XTRA", b"\1\2\3\4")]))
        check(got["unknown_chunks"] == 2 and got["entries"] == REF_PRESET["entries"], "unknown chunks are ignored and counted")
        check(OP.build(got) == build([(b"MPST", mpst())]), "unknown chunks are not kept")
        # too big
        check(k(build([(b"BIG ", b"\7" * 1000), (b"MPST", mpst())])) == "too big", "over 1,024 bytes")
        check(k(build([(b"BIG ", b"\7" * 920), (b"MPST", mpst())])) == "ok" and k(build([(b"BIG ", b"\7" * 922), (b"MPST", mpst())])) == "too big", "exactly 1,024 bytes")
        check(k((ref + b"\0" * 2000)) == "too big" and k(ref + b"\1\2\3") == "ok", "bytes after the FORM")
        huge = bytearray(ref)
        huge[4:8] = b"\xff\xff\xff\xfe"
        check(k(bytes(huge)) == "too big", "a FORM of 4 GiB")
        big = bytearray(ref)
        big[16:20] = b"\xff\xff\xff\xff"
        check(k(bytes(big)) == "malformed", "a chunk of 4 GiB")

        # --- the writer's refusals (formats::preset::valid)
        def refused(why, **edit):
            p = dict(REF_PRESET)
            p["entries"] = list(REF_PRESET["entries"])
            p.update(edit)
            try:
                OP.build(p)
            except ValueError:
                return
            check(False, "build accepted: " + why)
        refused("empty stem", module="")
        refused("dot stem", module="..")
        refused("a slash in the stem", module="a/b")
        refused("kind 0", kind=0)
        refused("kind 3", kind=3)
        refused("33 entries", entries=[("p%d" % i, 1.0) for i in range(33)])
        refused("empty name", entries=[("", 1.0)])
        refused("a 16-byte name", entries=[("x" * 16, 1.0)])
        refused("a control character in a name", entries=[("a\x1fb", 1.0)])
        refused("a non-ASCII name", entries=[("\xe9", 1.0)])
        refused("NaN", entries=[("a", float("nan"))])
        refused("+infinity", entries=[("a", float("inf"))])
        refused("-infinity", entries=[("a", float("-inf"))])
        refused("a value past the 32-bit floats", entries=[("a", 1e39)])
        refused("a module version past 32 bits", module_version=1 << 32)
        refused("a string for a value", entries=[("a", "1.0")])
        check(OP.build({"module": "x", "kind": 1, "module_version": 0, "entries": [("a", 1), ("A", -0.0)]}) != b"", "an int value and a case-different name are fine")
        check(OP.build({"module": "x", "kind": 1, "module_version": 0, "entries": [("a", 1.0), ("a", 2.0)]}) == mk(stem="x", modver=0, ents=[("a", fbits(1.0)), ("a", fbits(2.0))]),
              "a name twice is written")

        # --- the command line
        done = run_tool(["make", "--module", "a/b", "--kind", "effect", "-o", os.path.join(tmp, "bad.o3mp")], expect_rc=1)
        check(b"stem" in done.stderr and not os.path.exists(os.path.join(tmp, "bad.o3mp")), "make says why and writes nothing for a bad stem")
        run_tool(["make", "--module", "x", "--kind", "effect", "--set", "a=nan", "-o", os.path.join(tmp, "nan.o3mp")], expect_rc=1)
        run_tool(["make", "--module", "x", "--kind", "effect", "--set", "a", "-o", os.path.join(tmp, "x.o3mp")], expect_rc=2)
        damaged = os.path.join(tmp, "damaged.o3mp")
        with open(damaged, "wb") as f:
            f.write(ref[:-1] + bytes([ref[-1] ^ 1]))
        done = run_tool(["dump", damaged], expect_rc=1)
        check(b"bad CRC" in done.stderr, "dump of a damaged file says bad CRC: %r" % done.stderr)
        run_tool(["dump", os.path.join(tmp, "missing.o3mp")], expect_rc=1)

        # --- pack
        def write_pack(name, doc):
            path = os.path.join(tmp, name)
            with open(path, "w") as f:
                f.write(doc if isinstance(doc, str) else json.dumps(doc))
            return path

        good = {"module": "tb303", "kind": "instrument", "module_version": 65536,
                "presets": [{"name": "Acid Bass", "values": {"Cutoff": 0.5, "Resonance": 0.25, "Env Mod": 1.0, "Decay": 2.5}},
                            {"name": "Squelch-2", "values": {"Cutoff": 0.1}}, {"name": "Empty", "values": {}}]}
        outdir = os.path.join(tmp, "out", "tb303")
        done = run_tool(["pack", write_pack("good.json", good), outdir])
        files = sorted(os.listdir(outdir))
        check(files == ["Acid Bass.o3mp", "Empty.o3mp", "Squelch-2.o3mp"], "pack wrote %r" % files)
        check(len(done.stdout.decode().split("\n")) == 4, "pack prints a line a file")
        with open(os.path.join(outdir, "Acid Bass.o3mp"), "rb") as f:
            check(f.read() == ref, "the pack's first preset is the reference file")
        with open(os.path.join(outdir, "Empty.o3mp"), "rb") as f:
            check(rd(f.read())["entries"] == [], "an empty preset reads back")
        with open(os.path.join(outdir, "Squelch-2.o3mp"), "rb") as f:
            check(rd(f.read())["entries"] == [("Cutoff", OP.float_of(fbits(0.1)))], "a value is the nearest 32-bit float")

        def pack_refused(why, doc, needle):
            outd = os.path.join(tmp, "refused_" + str(abs(hash(why))))
            done = run_tool(["pack", write_pack("r.json", doc), outd], expect_rc=1)
            check(needle.encode() in done.stderr, "%s: the message should contain %r: %r" % (why, needle, done.stderr))
            check(not os.path.exists(outd), "%s: nothing is written" % why)

        def variant(**edit):
            d = json.loads(json.dumps(good))
            d.update(edit)
            return d

        def preset(name, values):
            return variant(presets=[{"name": "ok", "values": {"a": 1}}, {"name": name, "values": values}])

        pack_refused("NaN", '{"module":"x","kind":"effect","presets":[{"name":"n","values":{"a":NaN}}]}', "NaN")
        pack_refused("Infinity", '{"module":"x","kind":"effect","presets":[{"name":"n","values":{"a":Infinity}}]}', "Infinity")
        pack_refused("1e999", '{"module":"x","kind":"effect","presets":[{"name":"n","values":{"a":1e999}}]}', "finite")
        pack_refused("1e39", preset("n", {"a": 1e39}), "32-bit")
        pack_refused("a bool", preset("n", {"a": True}), "not a number")
        pack_refused("a string", preset("n", {"a": "0.5"}), "not a number")
        pack_refused("a null", preset("n", {"a": None}), "not a number")
        pack_refused("a 16-byte parameter name", preset("n", {"x" * 16: 1.0}), "1..15")
        pack_refused("an empty parameter name", preset("n", {"": 1.0}), "1..15")
        pack_refused("33 values", preset("n", {"p%d" % i: 1.0 for i in range(33)}), "at most 32")
        pack_refused("a JSON key twice", '{"module":"x","kind":"effect","presets":[{"name":"n","values":{"a":1,"a":2}}]}', "twice")
        pack_refused("a bad stem", variant(module="a/b"), "stem")
        pack_refused("no stem", {"kind": "effect", "presets": good["presets"]}, "stem")
        pack_refused("a bad kind", variant(kind="importer"), "kind")
        pack_refused("a bad module version", variant(module_version=-1), "module_version")
        pack_refused("no presets", variant(presets=[]), "presets")
        pack_refused("an extra key", variant(extra=1), "unknown key")
        pack_refused("a bad file name", preset("a/b", {"a": 1}), "file name")
        pack_refused("a name with a leading space", preset(" n", {"a": 1}), "file name")
        pack_refused("a name ending in a dot", preset("n.", {"a": 1}), "file name")
        pack_refused("a 31-byte file name", preset("n" * 31, {"a": 1}), "file name")
        for bad in ("a..b", "..", "a...", "CON", "con", "Prn", "AUX", "nul", "COM1", "com9", "LPT1", "lpt9", "con.txt", "NUL.x", "Com5.a.b", "aux."):
            pack_refused("the file name %r" % bad, preset(bad, {"a": 1}), "file name")
        pack_refused("a name twice", variant(presets=[{"name": "n", "values": {}}, {"name": "n", "values": {}}]), "same as")
        pack_refused("a name twice in another case", variant(presets=[{"name": "n", "values": {}}, {"name": "N", "values": {}}]), "same as")
        pack_refused("a values list that is not pairs", preset("n", [["a", 1, 2]]), "pairs")
        pack_refused("a values list of strings", preset("n", ["a"]), "pairs")
        pack_refused("a value in a pair that is NaN-like", preset("n", [["a", "x"]]), "not a number")
        pack_refused("33 pairs", preset("n", [["a", 1]] * 33), "at most 32")
        pack_refused("a preset without values", variant(presets=[{"name": "n"}]), "expected")
        pack_refused("not an object", "[1, 2]", "JSON object")
        pack_refused("not JSON", "{", "cannot read")
        # a refused preset after a good one writes nothing at all
        outd = os.path.join(tmp, "partial")
        run_tool(["pack", write_pack("p.json", preset("n", {"x" * 16: 1})), outd], expect_rc=1)
        check(not os.path.exists(outd), "a pack with one refused preset writes none")
        # names that look like the guard's refusals but are fine
        okname = variant(presets=[{"name": n, "values": {"a": 1}} for n in ("Console", "com0", "com10", "aux x", "a.b.c", "Acid-Bass_2")])
        run_tool(["pack", write_pack("okn.json", okname), os.path.join(tmp, "okn")])
        check(len(os.listdir(os.path.join(tmp, "okn"))) == 6, "names that are not reserved are written")
        # repeated parameter names: a list of pairs keeps them in order
        dup_pack = variant(presets=[{"name": "Twice", "values": [["a", 1], ["a", 2.5], ["b", 3]]}])
        run_tool(["pack", write_pack("dup.json", dup_pack), os.path.join(tmp, "dup")])
        with open(os.path.join(tmp, "dup", "Twice.o3mp"), "rb") as f:
            check(rd(f.read())["entries"] == [("a", 1.0), ("a", 2.5), ("b", 3.0)], "a pack's repeated parameter names are written in order")
        # 30-byte file names fit
        long_ok = variant(module="a" * 31, presets=[{"name": "n" * 30, "values": {"a": 1}}])
        run_tool(["pack", write_pack("l.json", long_ok), os.path.join(tmp, "long")])
        check(os.path.exists(os.path.join(tmp, "long", "n" * 30 + ".o3mp")), "a 30-byte file name is written")

    if len(sys.argv) > 2:
        write_corpus(sys.argv[2])
    if FAILS:
        print("%d check(s) failed" % len(FAILS))
        return 1
    print("o3preset_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
