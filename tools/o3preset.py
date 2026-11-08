#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference reader and writer of the module preset file `.o3mp` (`O3MP` 1.0; stdlib only; run it with `python3 -I`).

The format is specified in docs/formats/o3-preset.md; the C++ side is formats/preset.h (OS-MODPRESET P1). This module is an independent
implementation of the same bytes: ctest `preset_python` holds its output equal to the hex fixture tests/data/preset_basic.hex, which the C++
test (`preset_format_test`) holds equal too, and checks that it refuses the same damaged files with the same error kinds.

  o3preset.py dump FILE                       print every field of a preset file; exit 1 and say why when it is not a valid one
  o3preset.py make --module STEM --kind instrument|effect [--module-version N] --set Name=value ... -o FILE
                                              write one preset file (the values in the order given)
  o3preset.py pack PRESETS.json OUTDIR        write every preset of a pack as OUTDIR/<name>.o3mp (OUTDIR is the module's folder on the card:
                                              modules/presets/<stem>/); prints one line a file

A pack (`modules/<id>/presets.json`, OS-MODPRESET P5):

    {"module": "tb303", "kind": "instrument", "module_version": 65536,
     "presets": [{"name": "Acid Bass", "values": {"Cutoff": 0.4, "Resonance": 0.8}}, ...]}

`module_version` is optional (0). A preset's file name is its name: 1..30 letters, digits, space, `_`, `-` or `.`, the first a letter or digit,
the last neither a space nor a dot, no `..`, not a Windows device name (CON, PRN, AUX, NUL, COM1..9, LPT1..9, with or without an extension), and no
two that FAT would take for one (case-insensitive). Refused, with nothing written: a value that is not a finite number or does not fit a 32-bit
float, a parameter name that is not 1..15 printable ASCII characters, more than 32 values, a bad stem or kind, a bad file name, a file name twice,
a JSON key twice. A parameter name may repeat (a card module may have two parameters of one name): write `values` as a list of `[name, value]`
pairs, in the module's order, instead of an object (an object cannot hold a key twice).

As a library: parse(data) -> preset dict, build(preset) -> bytes, dump(preset) -> str, validate(preset). A preset is
{"module": str, "kind": 1|2, "module_version": int, "entries": [(name, value), ...]} with each value a Python float that is exactly a 32-bit one
(-0.0 and denormals are carried as they are). parse() also reports "duplicates", "unknown_chunks" and "newer_minor". FormatError carries the same
typed kinds as the C++ reader: truncated, bad magic, bad CRC, newer major version, malformed, too big.
"""
import argparse
import json
import math
import os
import struct
import sys
import zlib

# The limits of the format (formats/preset.h; the module SDK's: o3module.h and module/loader.h).
MAX_FILE_BYTES = 1024
STEM_MAX = 31
NAME_MAX = 15
MAX_ENTRIES = 32
VERSION = 0x0100
KIND_INSTRUMENT = 1
KIND_EFFECT = 2
KINDS = {"instrument": KIND_INSTRUMENT, "effect": KIND_EFFECT}
FILE_NAME_MAX = 30  # a preset's file name: modules/presets/<31>/<30>.o3mp is 84 bytes, inside the save guard's 96


class FormatError(Exception):
    """kind is one of: truncated, bad magic, bad CRC, newer major version, malformed, too big (the C++ reader's names)."""

    def __init__(self, kind, detail=""):
        Exception.__init__(self, "%s (%s)" % (kind, detail) if detail else kind)
        self.kind = kind
        self.detail = detail


class PackError(Exception):
    pass


def bits_of(value):
    return struct.unpack(">I", struct.pack(">f", value))[0]


def float_of(bits):
    return struct.unpack(">f", struct.pack(">I", bits))[0]


def finite_bits(bits):
    return (bits & 0x7F800000) != 0x7F800000


def stem_ok(text):
    """module::is_stem: 1..31 characters, none of / \\ : * ? \" < > | or a control character, not '.' or '..'."""
    raw = text.encode("latin-1", "replace") if isinstance(text, str) else text
    if not 1 <= len(raw) <= STEM_MAX:
        return False
    for c in raw:
        if c < 0x20 or c == 0x7F or chr(c) in '/\\:*?"<>|':
            return False
    return raw not in (b".", b"..")


def name_ok(text):
    """A parameter name: 1..15 printable ASCII characters."""
    return 1 <= len(text) <= NAME_MAX and all(0x20 <= ord(c) <= 0x7E for c in text)


def validate(preset):
    """Raise ValueError unless the preset is one the C++ writer would write (formats::preset::valid)."""
    if not isinstance(preset["module"], str) or not stem_ok(preset["module"]) or not all(ord(c) < 256 for c in preset["module"]):
        raise ValueError("the module stem %r is not valid (1..%d characters, no path character or control character)" % (preset["module"], STEM_MAX))
    if preset["kind"] not in (KIND_INSTRUMENT, KIND_EFFECT):
        raise ValueError("the kind must be instrument (1) or effect (2), not %r" % (preset["kind"],))
    if not 0 <= preset["module_version"] <= 0xFFFFFFFF:
        raise ValueError("the module version does not fit 32 bits")
    entries = preset["entries"]
    if len(entries) > MAX_ENTRIES:
        raise ValueError("%d entries: at most %d" % (len(entries), MAX_ENTRIES))
    for name, value in entries:  # a name may repeat: a card module may have two parameters of one name, and the entries keep their order
        if not isinstance(name, str) or not name_ok(name):
            raise ValueError("the parameter name %r is not 1..%d printable ASCII characters" % (name, NAME_MAX))
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError("the value of %r is not a number" % name)
        try:
            bits = bits_of(float(value))
        except (OverflowError, struct.error):
            raise ValueError("the value of %r does not fit a 32-bit float" % name)
        if not finite_bits(bits):
            raise ValueError("the value of %r is not finite" % name)


def _str(text):
    raw = text.encode("latin-1")
    return bytes([len(raw)]) + raw


def build(preset):
    """The bytes of the file (validate() first)."""
    validate(preset)
    payload = struct.pack(">H", VERSION) + _str(preset["module"]) + bytes([preset["kind"]]) + struct.pack(">I", preset["module_version"])
    payload += bytes([len(preset["entries"])])
    for name, value in preset["entries"]:
        payload += _str(name) + struct.pack(">f", float(value))
    chunk = b"MPST" + struct.pack(">I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")
    body = b"O3MP" + chunk
    form = b"FORM" + struct.pack(">I", len(body) + 14) + body
    crc = zlib.crc32(form) & 0xFFFFFFFF
    data = form + b"CRC " + struct.pack(">IHI", 6, VERSION, crc)
    if len(data) > MAX_FILE_BYTES:
        raise ValueError("the file would be %d bytes: at most %d" % (len(data), MAX_FILE_BYTES))
    return data


def _printable_fourcc(cid):
    return all(0x20 <= c <= 0x7E for c in cid)


def parse(data):
    """The preset of a file, as the C++ reader gives it, plus "duplicates", "unknown_chunks" and "newer_minor"; FormatError otherwise."""
    size = len(data)
    if size < 12:
        raise FormatError("bad magic" if size >= 4 and data[:4] != b"FORM" else "truncated", "shorter than the FORM header")
    if data[:4] != b"FORM" or data[8:12] != b"O3MP":
        raise FormatError("bad magic", "not an O3MP preset file")
    (length,) = struct.unpack(">I", data[4:8])
    if size > MAX_FILE_BYTES or length > MAX_FILE_BYTES - 8:
        raise FormatError("too big", "file larger than the limit")
    if length < 4 or length & 1:
        raise FormatError("malformed", "bad FORM length")
    if size < length + 8:
        raise FormatError("truncated", "the file ends before its FORM does")
    end = length + 8
    pos = 12
    mpst = None
    unknown = 0
    newer_minor = False
    crc_seen = False
    while pos < end and not crc_seen:
        if end - pos < 8:
            raise FormatError("malformed", "chunk header overruns the FORM")
        cid = data[pos:pos + 4]
        (plen,) = struct.unpack(">I", data[pos + 4:pos + 8])
        pad = plen & 1
        if plen + pad > end - pos - 8:
            raise FormatError("malformed", "chunk overruns the FORM")
        payload = pos + 8
        if pad and data[payload + plen] != 0:
            raise FormatError("malformed", "pad byte is not zero")
        if cid in (b"CRC ", b"MPST"):
            if plen < 2:
                raise FormatError("malformed", "chunk length out of range")
            (version,) = struct.unpack(">H", data[payload:payload + 2])
            if version >> 8 == 0:
                raise FormatError("malformed", "chunk version 0")
            if version >> 8 > VERSION >> 8:
                raise FormatError("newer major version", "chunk written by a newer major version of the format")
            newer = (version & 0xFF) > (VERSION & 0xFF)
            newer_minor = newer_minor or newer
            if cid == b"CRC ":
                if plen < 6 or (not newer and plen != 6):
                    raise FormatError("malformed", "chunk length out of range")
                if pos + 8 + plen + pad != end:
                    raise FormatError("malformed", "the CRC chunk is not the last")
                if struct.unpack(">I", data[payload + 2:payload + 6])[0] != (zlib.crc32(data[:pos]) & 0xFFFFFFFF):
                    raise FormatError("bad CRC", "the CRC chunk does not match the file")
                crc_seen = True
            else:
                if mpst is not None:
                    raise FormatError("malformed", "chunk repeated")
                mpst = (data[payload:payload + plen], newer)
        else:
            if not _printable_fourcc(cid):
                raise FormatError("malformed", "chunk id is not four printable characters")
            unknown += 1
        pos += 8 + plen + pad
    if not crc_seen:
        raise FormatError("malformed", "the FORM ends without a CRC chunk")
    if mpst is None:
        raise FormatError("malformed", "no MPST chunk")

    body, newer = mpst
    at = 2  # the version, checked above

    def take(n):
        nonlocal at
        if n > len(body) - at:
            raise FormatError("malformed", "MPST does not fit its entry count")
        chunk = body[at:at + n]
        at += n
        return chunk

    def string(limit, what):
        (n,) = take(1)
        if n > limit:
            raise FormatError("malformed", what)
        raw = take(n)
        if 0 in raw:
            raise FormatError("malformed", what)
        return raw

    stem = string(STEM_MAX, "the module stem is not valid")
    if not stem_ok(stem):
        raise FormatError("malformed", "the module stem is not valid")
    (kind,) = take(1)
    if kind not in (KIND_INSTRUMENT, KIND_EFFECT):
        raise FormatError("malformed", "the kind is neither instrument nor effect")
    (module_version,) = struct.unpack(">I", take(4))
    (count,) = take(1)
    if count > MAX_ENTRIES:
        raise FormatError("malformed", "more than 32 entries")
    entries = []
    duplicates = 0
    for _ in range(count):
        raw = string(NAME_MAX, "an entry's name is not valid")
        if not name_ok(raw.decode("latin-1")):
            raise FormatError("malformed", "an entry's name is not valid")
        (bits,) = struct.unpack(">I", take(4))
        if not finite_bits(bits):
            raise FormatError("malformed", "an entry's value is not finite")
        name = raw.decode("latin-1")
        if any(n == name for n, _ in entries):
            duplicates += 1  # information only: every entry is kept, in file order
        entries.append((name, float_of(bits)))
    if at != len(body) and not newer:
        raise FormatError("malformed", "bytes after the last entry")
    return {"module": stem.decode("latin-1"), "kind": kind, "module_version": module_version, "entries": entries,
            "duplicates": duplicates, "unknown_chunks": unknown, "newer_minor": newer_minor}


def fmt_value(value):
    return "%.9g" % value


def dump(preset):
    """The text `dump` prints: every field, one a line."""
    kinds = {KIND_INSTRUMENT: "instrument", KIND_EFFECT: "effect"}
    lines = ["module: %s" % preset["module"],
             "kind: %s (%d)" % (kinds.get(preset["kind"], "?"), preset["kind"]),
             "module_version: 0x%08x" % preset["module_version"],
             "count: %d" % len(preset["entries"])]
    for i, (name, value) in enumerate(preset["entries"]):
        lines.append("  [%d] %s = %s (0x%08x)" % (i, name, fmt_value(value), bits_of(value)))
    for key in ("duplicates", "unknown_chunks"):
        if preset.get(key):
            lines.append("%s: %d" % (key, preset[key]))
    if preset.get("newer_minor"):
        lines.append("newer_minor: yes (fields a newer minor appends were not read)")
    return "\n".join(lines) + "\n"


# ---- pack ----------------------------------------------------------------------------------------------------------------------------------------

def valid_file_name(name):
    """A preset's file name: the save guard's valid_name (hal/include/o3os/hal/save_guard.h) with the length of section 5 of the format document:
    letters, digits, space, '_', '-', '.', the first a letter or digit, the last neither a space nor a dot, no '..' anywhere, and not a name
    Windows reserves for a device (CON, PRN, AUX, NUL, COM1..9, LPT1..9, in any case, with or without an extension)."""
    if not isinstance(name, str) or not 1 <= len(name) <= FILE_NAME_MAX:
        return False
    if not all(c.isascii() and (c.isalnum() or c in " _-.") for c in name):
        return False
    if not name[0].isalnum() or name[-1] in " ." or ".." in name:
        return False
    stem = name.split(".", 1)[0].lower()  # the part before the first dot
    if stem in ("con", "prn", "aux", "nul"):
        return False
    return not (len(stem) == 4 and stem[:3] in ("com", "lpt") and stem[3] in "123456789")


def _no_constant(text):
    raise PackError("the JSON constant %s is not a finite number" % text)


def _pairs(pairs):
    seen = set()
    for key, _ in pairs:
        if key in seen:
            raise PackError("the JSON key %r is twice in one object" % key)
        seen.add(key)
    return dict(pairs)


def load_pack(path):
    """The presets of a pack file as [(file name, preset dict)], every one validated; PackError when any is refused."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f, parse_constant=_no_constant, object_pairs_hook=_pairs)
    except (OSError, ValueError) as exc:
        raise PackError("cannot read %s: %s" % (path, exc))
    if not isinstance(doc, dict):
        raise PackError("the pack is not a JSON object")
    unknown = sorted(set(doc) - {"module", "kind", "module_version", "presets"})
    if unknown:
        raise PackError("unknown key(s) in the pack: %s" % ", ".join(unknown))
    kind = KINDS.get(doc.get("kind"))
    if kind is None:
        raise PackError('"kind" must be "instrument" or "effect"')
    version = doc.get("module_version", 0)
    if isinstance(version, bool) or not isinstance(version, int) or not 0 <= version <= 0xFFFFFFFF:
        raise PackError('"module_version" must be a whole number of 32 bits')
    presets = doc.get("presets")
    if not isinstance(presets, list) or not presets:
        raise PackError('"presets" must be a list with at least one preset')
    out = []
    taken = {}
    for i, item in enumerate(presets):
        if not isinstance(item, dict) or set(item) != {"name", "values"} or not isinstance(item["values"], (dict, list)):
            raise PackError('preset %d: expected {"name": ..., "values": {...}} (or a list of [name, value] pairs)' % i)
        if isinstance(item["values"], dict):
            pairs = list(item["values"].items())
        else:
            pairs = item["values"]
            if not all(isinstance(pair, list) and len(pair) == 2 for pair in pairs):
                raise PackError("preset %d: the values list holds [name, value] pairs" % i)
            pairs = [tuple(pair) for pair in pairs]
        name = item["name"]
        if not valid_file_name(name):
            raise PackError("preset %d: the name %r is not a file name (1..%d letters, digits, space, _ - . ; first a letter or digit, last not space or dot)"
                            % (i, name, FILE_NAME_MAX))
        if name.lower() in taken:
            raise PackError("preset %d: the name %r is the same as %r (FAT does not tell them apart)" % (i, name, taken[name.lower()]))
        taken[name.lower()] = name
        preset = {"module": doc.get("module"), "kind": kind, "module_version": version, "entries": pairs}
        try:
            validate(preset)
        except ValueError as exc:
            raise PackError("preset %d (%s): %s" % (i, name, exc))
        out.append((name, preset))
    return out


def pack(path, outdir):
    """Write the files of a pack; nothing is written when any preset is refused. Returns the paths."""
    files = []
    for name, preset in load_pack(path):
        files.append((os.path.join(outdir, name + ".o3mp"), build(preset)))
    os.makedirs(outdir, exist_ok=True)
    for target, data in files:
        with open(target, "wb") as f:
            f.write(data)
    return [t for t, _ in files]


# ---- command line --------------------------------------------------------------------------------------------------------------------------------

def parse_set(text):
    name, eq, raw = text.partition("=")
    if not eq:
        raise argparse.ArgumentTypeError("--set wants Name=value, not %r" % text)
    try:
        value = float(raw)
    except ValueError:
        raise argparse.ArgumentTypeError("the value of %r is not a number: %r" % (name, raw))
    return name, value


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("dump", help="print every field of a preset file")
    p.add_argument("file")
    p = sub.add_parser("make", help="write one preset file")
    p.add_argument("--module", required=True)
    p.add_argument("--kind", required=True, choices=sorted(KINDS))
    p.add_argument("--module-version", type=lambda s: int(s, 0), default=0)
    p.add_argument("--set", dest="sets", action="append", default=[], type=parse_set, metavar="Name=value")
    p.add_argument("-o", "--out", required=True)
    p = sub.add_parser("pack", help="write the files of a pack")
    p.add_argument("pack")
    p.add_argument("outdir")
    args = ap.parse_args(argv[1:])
    try:
        if args.cmd == "dump":
            with open(args.file, "rb") as f:
                data = f.read()
            sys.stdout.write(dump(parse(data)))
        elif args.cmd == "make":
            preset = {"module": args.module, "kind": KINDS[args.kind], "module_version": args.module_version, "entries": args.sets}
            data = build(preset)
            with open(args.out, "wb") as f:
                f.write(data)
        else:
            for target in pack(args.pack, args.outdir):
                print(target)
    except FormatError as exc:
        sys.stderr.write("o3preset: %s: %s\n" % (getattr(args, "file", ""), exc))
        return 1
    except (PackError, ValueError, OSError) as exc:
        sys.stderr.write("o3preset: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
