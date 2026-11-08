#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ptfx_inspect.py: print a .ptfx header and run the 3.1 checks of docs/specs/OS-CEPTFX.md on it, for people
and the HIL. Standard library only; run it with `python3 -I`.

  ptfx_inspect.py FILE...          print every file's header and checks (exit 0 iff all are Ok)
  ptfx_inspect.py --check FILE...  one line per file, exit 0 iff all are Ok
  ptfx_inspect.py --all DIR        every *.ptfx in DIR (the extension case-insensitively, as the scan of 2.2)

The checks are the same rules module/src/ptfx_format.cpp implements for the firmware (3.1): the refusals (short
file, magic, version, IMX_ONLY, unknown flags, damaged code range or entries, over the 32 KiB image), the
tolerated fields (num_params clamped to 12, scratch to 640 KiB, list strings only inside the payload and capped at
16, unknown display types as floats) and the sanitising (non-finite or reversed range as a non-editable integer
0..0, non-printables as '?', a default clamped into the range, a zero, negative or non-finite step as 1). The list
items are read from the payload at code_offset + list_offset.
"""
import argparse
import os
import struct
import sys

HEADER_SIZE = 472
PARAM_SIZE = 32
MAGIC = 0x58465450
MAX_PARAMS = 12
MAX_LIST_ITEMS = 16
MAX_LIST_ITEM_LEN = 10
MAX_SCRATCH = 640 * 1024
MAX_IMAGE = 32 * 1024
KNOWN_FLAGS = 0x1F

FLAG_NAMES = [(0x01, "GENERATOR"), (0x02, "STEREO"), (0x04, "WAVETABLE"), (0x08, "CONVERT_TO_MONO"), (0x10, "IMX_ONLY")]

STATUS_TEXT = {
    "NotPtfx": "NOT A PTFX PLUGIN",
    "VersionUnsupported": "PTFX VERSION {version} NOT SUPPORTED",
    "NotForThisTracker": "NOT FOR THIS TRACKER",
    "NeedsNewerOs": "NEEDS NEWER OS",
    "FileDamaged": "PLUGIN FILE DAMAGED",
    "TooBig": "PLUGIN TOO BIG",
    "Ok": "OK",
}


class Refused(Exception):
    def __init__(self, status, version=0):
        super().__init__(status)
        self.status = status
        self.version = version


def _text(raw, cap):
    """A fixed field that may lack its NUL: up to the first NUL, non-printables as '?'."""
    out = []
    for i in range(min(cap, len(raw))):
        c = raw[i]
        if c == 0:
            break
        out.append(chr(c) if 0x20 <= c < 0x7F else "?")
    return "".join(out)


def _f32(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def _finite(x):
    return x == x and abs(x) != float("inf")


def parse_header(blob):
    """The 472-byte header of a .ptfx file, with the 3.1 checks. Returns a dict; 'status' is 'Ok' or the refusal,
    and 'message' is the strip text of 3.1."""
    info = {"status": "NotPtfx", "message": STATUS_TEXT["NotPtfx"], "version": 0, "flags": 0, "name": "",
            "author": "", "num_params": 0, "scratch_size": 0, "code_offset": 0, "code_size": 0, "bss_size": 0,
            "entries": {}, "params": [], "file_size": len(blob)}
    if len(blob) < HEADER_SIZE:
        return info
    magic, version, flags = struct.unpack_from("<IHH", blob, 0)
    if magic != MAGIC:
        return info
    info["version"] = version
    info["flags"] = flags
    if version != 1:
        info["status"] = "VersionUnsupported"
        info["message"] = STATUS_TEXT["VersionUnsupported"].format(version=version)
        return info
    if flags & 0x10:
        info["status"] = "NotForThisTracker"
        info["message"] = STATUS_TEXT["NotForThisTracker"]
        return info
    if flags & ~KNOWN_FLAGS:
        info["status"] = "NeedsNewerOs"
        info["message"] = STATUS_TEXT["NeedsNewerOs"]
        return info
    info["name"] = _text(blob[8:40], 32)
    info["author"] = _text(blob[40:56], 16)
    info["num_params"] = min(blob[56], MAX_PARAMS)
    info["scratch_size"] = min(struct.unpack_from("<I", blob, 60)[0], MAX_SCRATCH)
    code_offset, code_size, bss_size, init, set_param, process = struct.unpack_from("<IIIIII", blob, 64)
    info["code_offset"], info["code_size"], info["bss_size"] = code_offset, code_size, bss_size
    info["entries"] = {"init": init & ~1, "set_param": set_param & ~1, "process_block": process & ~1}

    def damaged():
        info["status"] = "FileDamaged"
        info["message"] = STATUS_TEXT["FileDamaged"]
        return info

    if code_size == 0 or code_offset < HEADER_SIZE or code_offset + code_size > len(blob):
        return damaged()
    if ((code_size + 3) & ~3) + bss_size > MAX_IMAGE:
        info["status"] = "TooBig"
        info["message"] = STATUS_TEXT["TooBig"]
        return info
    e = info["entries"]
    if e["init"] >= code_size or e["set_param"] >= code_size or e["process_block"] >= code_size or \
            len(set(e.values())) != 3:
        return damaged()

    payload = blob[code_offset:code_offset + code_size]
    for i in range(info["num_params"]):
        q = 88 + PARAM_SIZE * i
        p = {"label": _text(blob[q:q + 8], 8), "min": _f32(struct.unpack_from("<I", blob, q + 8)[0]),
             "max": _f32(struct.unpack_from("<I", blob, q + 12)[0]),
             "def": _f32(struct.unpack_from("<I", blob, q + 16)[0]),
             "step": _f32(struct.unpack_from("<I", blob, q + 20)[0]),
             "type": chr(blob[q + 30]), "editable": True, "unit": "", "list_offset": 0, "list_count": 0,
             "items": []}
        if p["type"] not in ("d", "l"):
            p["type"] = "f"
        if p["type"] == "l":
            p["list_offset"] = struct.unpack_from("<H", blob, q + 24)[0]
            p["list_count"] = min(blob[q + 26], MAX_LIST_ITEMS)
        else:
            p["unit"] = _text(blob[q + 24:q + 30], 6)
        if not _finite(p["min"]) or not _finite(p["max"]) or p["min"] > p["max"]:
            p.update({"type": "d", "min": 0.0, "max": 0.0, "def": 0.0, "step": 1.0, "editable": False,
                      "list_count": 0, "items": []})
        else:
            if not _finite(p["def"]) or p["def"] < p["min"]:
                p["def"] = p["min"]
            elif p["def"] > p["max"]:
                p["def"] = p["max"]
        if not _finite(p["step"]) or p["step"] <= 0.0:
            p["step"] = 1.0
        if p["type"] == "l":
            at = p["list_offset"]
            n = 0
            while n < p["list_count"] and at < len(payload):
                end = payload.find(b"\0", at)
                if end < 0:
                    break
                p["items"].append(_text(payload[at:end], MAX_LIST_ITEM_LEN))
                at = end + 1
                n += 1
        info["params"].append(p)

    info["status"] = "Ok"
    info["message"] = STATUS_TEXT["Ok"]
    return info


def flag_names(flags):
    names = [n for bit, n in FLAG_NAMES if flags & bit]
    return "|".join(names) if names else "EFFECT"


def print_file(path, blob, info, out):
    out.write("%s: %d bytes, status %s (%s)\n" % (path, len(blob), info["status"], info["message"]))
    if info["status"] != "Ok":
        return
    out.write('  name "%s"  author "%s"  version %d  flags 0x%04X (%s)\n'
              % (info["name"], info["author"], info["version"], info["flags"], flag_names(info["flags"])))
    out.write("  num_params %d  scratch %d B (max %d)  code offset %d size %d B  bss %d B  image %d B (limit %d)\n"
              % (info["num_params"], info["scratch_size"], MAX_SCRATCH, info["code_offset"], info["code_size"],
                 info["bss_size"], (info["code_size"] + 3) & ~3, MAX_IMAGE))
    out.write("  entries: init %d, set_param %d, process_block %d (Thumb bit masked, inside the payload)\n"
              % (info["entries"]["init"], info["entries"]["set_param"], info["entries"]["process_block"]))
    for i, p in enumerate(info["params"]):
        line = '  param[%d] "%s" type %r %g..%g def %g step %g' % (i, p["label"], p["type"], p["min"], p["max"],
                                                                     p["def"], p["step"])
        if p["type"] == "l":
            line += " list at payload+%d, %d item(s): %s" % (p["list_offset"], len(p["items"]),
                                                             ", ".join('"%s"' % s for s in p["items"]))
        else:
            line += ' unit "%s"' % p["unit"]
        if not p["editable"]:
            line += "  NOT EDITABLE"
        out.write(line + "\n")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*", help=".ptfx files to inspect")
    ap.add_argument("--check", action="store_true", help="one line per file, no header dump")
    ap.add_argument("--all", metavar="DIR", help="inspect every *.ptfx in DIR (case-insensitive extension)")
    a = ap.parse_args(argv)
    paths = list(a.files)
    if a.all:
        if not os.path.isdir(a.all):
            print("error: %s is not a directory" % a.all, file=sys.stderr)
            return 1
        paths += sorted(os.path.join(a.all, f) for f in os.listdir(a.all)
                        if f.lower().endswith(".ptfx") and os.path.isfile(os.path.join(a.all, f)))
    if not paths:
        ap.print_usage(sys.stderr)
        return 1
    bad = 0
    for path in paths:
        try:
            with open(path, "rb") as f:
                blob = f.read()
        except OSError as e:
            print("error: cannot read %s: %s" % (path, e), file=sys.stderr)
            bad += 1
            continue
        info = parse_header(blob)
        if a.check:
            print("%s %s: %s (%s)" % ("OK" if info["status"] == "Ok" else "REFUSED", path, info["message"],
                                      info.get("name", "")))
        else:
            print_file(path, blob, info, sys.stdout)
        if info["status"] != "Ok":
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
