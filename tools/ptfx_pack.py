#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ptfx_pack.py: our packer for the OS-CEPTFX S0 test plugins (docs/specs/OS-CEPTFX.md 7, S0).

Standard library only; run it with `python3 -I`.

  ptfx_pack.py SRC.c SIDECAR.json -o OUT.ptfx [--toolchain PREFIX] [--abi-dir DIR]
  ptfx_pack.py SRC.c -o OUT.ptfx --abi-dir modules/ptfx/sdk     (OS-PTFXPACK: the header's fields from the source itself)

Everything here is written from the format facts of docs/specs/OS-CEPTFX.md 2.1 (which cites
tracker-ce@7fd6c2c:editor-fx-sdk/tracker_plugin.h and ptfx_builder.py as its evidence; no tracker-ce text is
copied): the file is a 472-byte header then the payload at code_offset; the header is magic "PTFX", version u16,
flags u16, name[32], author[16], num_params u8, scratch u32, code_offset/code_size/bss_size u32 and the three
entry offsets u32, then 12 params of 32 bytes (label[8], min/max/default/step f32, unit[6], display type u8,
pad u8); the payload is the linked image based at 0 with no relocation table, .bss of bss_size bytes following
align4(code_size) in RAM and zeroed by the loader; the entries are the offsets of plugin_init,
plugin_set_param and plugin_process_block from `nm`; a list parameter's unit field holds a u16 offset into the
payload and a u8 item count of NUL-terminated strings of up to 10 characters, up to 16 of them; the build is for
the Cortex-M4F hard-float, position-independent, long calls, no libc (the flags below). Our own choices are the
section names of the link script, the JSON sidecar as the header's source, and the string table as a C array in
a .ptfx_strings section.

It cross-compiles one of tests/ptfx/*.c with those flags, links it at address 0 with our own script below, takes
the entry offsets from `nm`, and writes the 472-byte header from the JSON sidecar plus those offsets, then the
payload. List parameters ('l') put their option strings into the .ptfx_strings section; the header's unit field
then holds the u16 offset into the payload and the u8 item count.

Without a sidecar the header's fields come from the source: the `// PTFX_NAME|AUTHOR|FLAGS|SCRATCH: value` comment
lines and the `PLUGIN_PARAMS[]` array of tracker_plugin.h's own convenience macros (PTFX_PARAM_FLOAT, PTFX_PARAM_INT,
PTFX_PARAM_LIST) or brace initializers (label, min, max, default, step, unit, type), read by side_from_source() below
(written from the SDK header's own documentation of those macros and comments, spec 2.1; the SDK's own builder is
not used). That is how modules/ptfx/ builds the tracker-ce plugins and our own ones from one file each.

The toolchain is found like cmake/arm-none-eabi-toolchain.cmake does: --toolchain PREFIX, else ARM_GNU_ROOT or
O3_ARM_TOOLCHAIN (<root>/bin/arm-none-eabi-), else arm-none-eabi-gcc on PATH. Nothing is written outside OUT.ptfx
and a temporary directory; no .ptfx is ever committed (build-fixtures/ is untracked). The file written is parsed
back with tools/ptfx_inspect.py's checks before the run succeeds.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

HEADER_SIZE = 472          # spec 2.1: 88-byte prefix + 12 * 32-byte params
PARAM_DEF_SIZE = 32
MAX_PARAMS = 12
MAX_LIST_ITEMS = 16
MAX_LIST_ITEM_LEN = 10
MAX_IMAGE = 32 * 1024      # align4(code_size) + bss_size, the MK66 limit (3.1)
MAGIC = 0x58465450         # "PTFX", little endian
VERSION = 1

FLAG_MAP = {               # spec 2.1 flags
    "EFFECT": 0x0000,
    "GENERATOR": 0x0001,
    "STEREO": 0x0002,
    "WAVETABLE": 0x0004,
    "CONVERT_TO_MONO": 0x0008,
    "IMX_ONLY": 0x0010,
}

# The compile flags the .ptfx format prescribes for its payload (spec 2.1, "Build"): the Cortex-M4F with the
# hard-float ABI and fpv4-sp-d16, Thumb, position-independent with text-relative data and long calls, no libc
# and no builtins in the payload, and no loop-to-memset/memcpy rewrite (the payload links against no libc: OS-PTFXPACK,
# the same flag the SDK's builder passes). Nothing else.
BUILD_FLAGS = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16",
               "-O3", "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-fPIC", "-mpic-data-is-text-relative",
               "-mlong-calls", "-nostdlib"]

# Our own link layout, written from what the format requires of the payload (spec 2.1): the image is based at 0
# and carries no relocation table; everything with contents (code, read-only data, initialised data) goes into
# one section, the list strings into their own section after it (their header offset is absolute in the payload),
# and .bss follows 4-aligned as a NOLOAD section, so that objcopy -O binary yields exactly the payload and the
# loader's align4(code_size) lands on the bss start. Toolchain metadata sections are dropped.
LINKER_SCRIPT = """
SECTIONS
{
  . = 0;
  .o3_image : { *(.text*) *(.rodata*) *(.data*) }
  . = ALIGN(4);
  o3_ptfx_strings_start = .;
  .o3_strings : { *(.ptfx_strings*) }
  . = ALIGN(4);
  o3_ptfx_bss_start = .;
  .o3_bss (NOLOAD) : { *(.bss*) *(COMMON) }
  o3_ptfx_bss_end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) *(.ARM.exidx*) *(.eh_frame*) }
}
"""

# The string table of the list parameters, as one byte array in its own section.
STRINGS_C = """#include <stdint.h>
__attribute__((section(".ptfx_strings"), aligned(4), used))
const uint8_t ptfx_test_string_table[] = { %s };
"""


class PackError(Exception):
    pass


def find_toolchain(override=None):
    """The arm-none-eabi- prefix, found like cmake/arm-none-eabi-toolchain.cmake: --toolchain, ARM_GNU_ROOT,
    O3_ARM_TOOLCHAIN (<root>/bin/), then PATH."""
    if override:
        return override if override.endswith("-") else override + "-"
    for var in ("ARM_GNU_ROOT", "O3_ARM_TOOLCHAIN"):
        root = os.environ.get(var, "")
        if root:
            return os.path.join(root, "bin", "arm-none-eabi-")
    if shutil.which("arm-none-eabi-gcc"):
        return "arm-none-eabi-"
    raise PackError("no Arm GNU Toolchain found: pass --toolchain PREFIX, or export ARM_GNU_ROOT / O3_ARM_TOOLCHAIN, "
                    "or put arm-none-eabi-gcc on PATH (see docs/toolchain.md)")


def run(cmd, what):
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if res.returncode != 0:
        raise PackError("%s failed: %s" % (what, res.stderr.decode(errors="replace").strip()[:2000]))
    return res.stdout


def load_sidecar(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            side = json.load(f)
    except (OSError, ValueError) as e:
        raise PackError("cannot read the sidecar %s: %s" % (path, e))
    return check_sidecar(side, path)


def check_sidecar(side, path):
    for key in ("name", "flags", "params"):
        if key not in side:
            raise PackError("the sidecar %s lacks %r" % (path, key))
    name = side["name"]
    if not isinstance(name, str) or not name or len(name.encode("ascii", "replace")) > 31:
        raise PackError("the sidecar %s: 'name' must be 1..31 ASCII characters" % path)
    author = side.get("author", "")
    if len(author.encode("ascii", "replace")) > 15:
        raise PackError("the sidecar %s: 'author' must be at most 15 ASCII characters" % path)
    flags = 0
    for flag in side["flags"]:
        if flag not in FLAG_MAP:
            raise PackError("the sidecar %s: unknown flag %r (known: %s)" % (path, flag, ", ".join(sorted(FLAG_MAP))))
        flags |= FLAG_MAP[flag]
    scratch = side.get("scratch", 0)
    if not isinstance(scratch, int) or scratch < 0:
        raise PackError("the sidecar %s: 'scratch' must be a non-negative integer" % path)
    params = side["params"]
    if not isinstance(params, list) or len(params) > MAX_PARAMS:
        raise PackError("the sidecar %s: 'params' must be a list of at most %d" % (path, MAX_PARAMS))
    for p in params:
        for key in ("label", "type"):
            if key not in p:
                raise PackError("the sidecar %s: a parameter lacks %r" % (path, key))
        if len(p["label"].encode("ascii", "replace")) > 7:
            raise PackError("the sidecar %s: the label %r is longer than 7 characters" % (path, p["label"]))
        if p["type"] not in ("f", "d", "l"):
            raise PackError("the sidecar %s: the type of %r must be 'f', 'd' or 'l'" % (path, p["label"]))
        if p["type"] == "l":
            # A list is an integer 0..len(options)-1 with step 1, as the builder of spec 2.1 writes it.
            options = p.get("options")
            if not isinstance(options, list) or not options or len(options) > MAX_LIST_ITEMS:
                raise PackError("the sidecar %s: the list %r needs 1..%d options" % (path, p["label"], MAX_LIST_ITEMS))
            for o in options:
                if not o or len(o.encode("ascii", "replace")) > MAX_LIST_ITEM_LEN:
                    raise PackError("the sidecar %s: the option %r of %r is empty or longer than %d characters"
                                    % (path, o, p["label"], MAX_LIST_ITEM_LEN))
            p.setdefault("min", 0.0)
            p["max"] = float(len(options) - 1)
            p.setdefault("default", 0.0)
            p["step"] = 1.0
            if not (0 <= int(p["default"]) < len(options)):
                raise PackError("the sidecar %s: the default of %r is outside the list" % (path, p["label"]))
            p["default"] = float(int(p["default"]))
        else:
            for key in ("min", "max", "default", "step"):
                if key not in p:
                    raise PackError("the sidecar %s: the parameter %r lacks %r" % (path, p["label"], key))
            if len(str(p.get("unit", "")).encode("ascii", "replace")) > 5:
                raise PackError("the sidecar %s: the unit of %r is longer than 5 characters" % (path, p["label"]))
    return side, flags, scratch, params


SOURCE_TAG = re.compile(r"^//\s*PTFX_(NAME|AUTHOR|FLAGS|SCRATCH)\s*:\s*(.*?)\s*$")
NUMBER = re.compile(r"\(?\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)[fF]?\s*\)?")
PARAM_TYPES = {"PARAM_FLOAT": "f", "PARAM_INT": "d", "PARAM_LIST": "l"}


def _split_top(text):
    """Split at the commas outside strings, parentheses and braces."""
    out, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "({":
            depth += 1
        elif c in ")}":
            depth -= 1
        if c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def _string(tok, src):
    m = re.fullmatch(r'"([^"\\]*)"', tok.strip())
    if not m:
        raise PackError("%s: PLUGIN_PARAMS: %r is not a plain string literal" % (src, tok))
    return m.group(1)


def _number(tok, src):
    m = NUMBER.fullmatch(tok.strip())
    if not m:
        raise PackError("%s: PLUGIN_PARAMS: %r is not a number literal" % (src, tok))
    return float(m.group(1))


def side_from_source(src):
    """The sidecar dict of a plugin source that carries its own metadata (see the module docstring)."""
    try:
        with open(src, "r", encoding="utf-8") as f:
            text = f.read()
    except (OSError, ValueError) as e:
        raise PackError("cannot read %s: %s" % (src, e))
    meta = {}
    for line in text.splitlines():
        m = SOURCE_TAG.match(line)
        if m:
            meta[m.group(1)] = m.group(2)
    if "NAME" not in meta:
        raise PackError("%s has no '// PTFX_NAME:' line" % src)
    flags = [f.strip().upper() for f in re.split(r"[,|]", meta.get("FLAGS", "EFFECT")) if f.strip()]
    try:
        scratch = int(meta.get("SCRATCH", "0"))
    except ValueError:
        raise PackError("%s: '// PTFX_SCRATCH:' is not an integer" % src)
    bare = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    bare = re.sub(r"//[^\n]*", "", bare)
    m = re.search(r"PluginParam\s+PLUGIN_PARAMS\s*\[[^\]]*\]\s*=\s*\{", bare)
    if not m:
        raise PackError("%s has no PLUGIN_PARAMS[] array" % src)
    depth, i, in_str = 1, m.end(), False
    while i < len(bare) and depth:
        c = bare[i]
        if c == '"':
            in_str = not in_str
        elif not in_str:
            depth += (c == "{") - (c == "}")
        i += 1
    if depth:
        raise PackError("%s: PLUGIN_PARAMS[] is not closed" % src)
    params = []
    for entry in _split_top(bare[m.end():i - 1]):
        mm = re.fullmatch(r"PTFX_PARAM_(FLOAT|INT|LIST)\s*\((.*)\)", entry, flags=re.S)
        if mm:
            kind, args = mm.group(1), _split_top(mm.group(2))
            if kind == "LIST":
                if len(args) != 3:
                    raise PackError("%s: PTFX_PARAM_LIST takes 3 arguments: %r" % (src, entry))
                params.append({"label": _string(args[0], src), "type": "l", "default": _number(args[1], src),
                               "options": _string(args[2], src).split("|")})
                continue
            if len(args) != 6:
                raise PackError("%s: PTFX_PARAM_%s takes 6 arguments: %r" % (src, kind, entry))
            ptype = "f" if kind == "FLOAT" else "d"
        else:
            mb = re.fullmatch(r"\{(.*)\}", entry, flags=re.S)
            if not mb:
                raise PackError("%s: PLUGIN_PARAMS: cannot read the entry %r" % (src, entry))
            args = _split_top(mb.group(1))
            if len(args) != 7 or args[6].strip() not in PARAM_TYPES:
                raise PackError("%s: PLUGIN_PARAMS: a brace entry is label, min, max, default, step, unit, type: %r"
                                % (src, entry))
            ptype = PARAM_TYPES[args[6].strip()]
            if ptype == "l":
                params.append({"label": _string(args[0], src), "type": "l", "default": _number(args[3], src),
                               "options": _string(args[5], src).split("|")})
                continue
        params.append({"label": _string(args[0], src), "type": ptype, "min": _number(args[1], src),
                       "max": _number(args[2], src), "default": _number(args[3], src),
                       "step": _number(args[4], src), "unit": _string(args[5], src)})
    return {"name": meta["NAME"], "author": meta.get("AUTHOR", ""), "flags": flags, "scratch": scratch,
            "params": params}


def build(src, sidecar_path, out_path, toolchain, abi_dir):
    if sidecar_path:
        side, flags, scratch, params = load_sidecar(sidecar_path)
    else:
        side, flags, scratch, params = check_sidecar(side_from_source(src), src)
    prefix = find_toolchain(toolchain)
    for tool in ("gcc", "ld", "nm", "objcopy"):
        if shutil.which(prefix + tool) is None:
            raise PackError("the toolchain prefix %r has no %s" % (prefix, tool))

    # The list strings of the 'l' parameters, packed into one byte array in the .ptfx_strings section.
    string_table = bytearray()
    for p in params:
        p["_list_rel"] = 0
        p["_list_count"] = 0
        if p["type"] == "l":
            p["_list_rel"] = len(string_table)
            p["_list_count"] = len(p["options"])
            for o in p["options"]:
                string_table.extend(o.encode("ascii", "replace")[:MAX_LIST_ITEM_LEN] + b"\0")

    with tempfile.TemporaryDirectory(prefix="ptfx-pack-") as tmp:
        ld_path = os.path.join(tmp, "plugin.ld")
        with open(ld_path, "w", encoding="utf-8") as f:
            f.write(LINKER_SCRIPT)
        obj = os.path.join(tmp, "plugin.o")
        run([prefix + "gcc"] + BUILD_FLAGS + ["-I" + abi_dir, "-c", src, "-o", obj], "the compile of %s" % src)
        objs = [obj]
        if string_table:
            s_c = os.path.join(tmp, "strings.c")
            with open(s_c, "w", encoding="utf-8") as f:
                f.write(STRINGS_C % ", ".join("0x%02x" % b for b in string_table))
            s_o = os.path.join(tmp, "strings.o")
            run([prefix + "gcc"] + BUILD_FLAGS + ["-c", s_c, "-o", s_o], "the compile of the string table")
            objs.append(s_o)
        elf = os.path.join(tmp, "plugin.elf")
        run([prefix + "ld", "-T", ld_path, "-e", "0"] + objs + ["-o", elf], "the link")
        nm_out = run([prefix + "nm", elf], "nm").decode()
        symbols = {}
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                symbols[parts[2]] = int(parts[0], 16)
        entries = {}
        for entry in ("plugin_init", "plugin_set_param", "plugin_process_block"):
            if entry not in symbols:
                raise PackError("%s does not define %s" % (src, entry))
            entries[entry] = symbols[entry]
        strings_base = symbols.get("o3_ptfx_strings_start", 0)
        bss_size = symbols.get("o3_ptfx_bss_end", 0) - symbols.get("o3_ptfx_bss_start", 0)
        bin_path = os.path.join(tmp, "plugin.bin")
        run([prefix + "objcopy", "-O", "binary", elf, bin_path], "objcopy")
        with open(bin_path, "rb") as f:
            payload = f.read()

    code_size = len(payload)
    if code_size == 0:
        raise PackError("%s produced an empty payload" % src)
    if entries["plugin_init"] == entries["plugin_set_param"] or entries["plugin_init"] == entries["plugin_process_block"] \
            or entries["plugin_set_param"] == entries["plugin_process_block"]:
        raise PackError("%s: two entries share one offset" % src)
    for entry, off in entries.items():
        if (off & ~1) >= code_size:  # bit 0 masked, as the parser of 3.1 does
            raise PackError("%s: the entry %s is outside the payload" % (src, entry))
    image = (code_size + 3) & ~3
    if image + bss_size > MAX_IMAGE:
        raise PackError("%s: align4(code) + bss = %d bytes, over the %d-byte limit" % (src, image + bss_size, MAX_IMAGE))

    header = bytearray()
    header += struct.pack("<IHH32s16sB3sIIIIIII",
                          MAGIC, VERSION, flags,
                          side["name"].encode("ascii", "replace")[:31].ljust(32, b"\0"),
                          side.get("author", "").encode("ascii", "replace")[:15].ljust(16, b"\0"),
                          len(params), b"\0\0\0",
                          scratch, HEADER_SIZE, code_size, bss_size,
                          entries["plugin_init"], entries["plugin_set_param"], entries["plugin_process_block"])
    assert len(header) == 88
    for i in range(MAX_PARAMS):
        if i < len(params):
            p = params[i]
            label = p["label"].encode("ascii", "replace")[:7].ljust(8, b"\0")
            if p["type"] == "l":
                unit = struct.pack("<HB3s", strings_base + p["_list_rel"], p["_list_count"], b"\0\0\0")
            else:
                unit = str(p.get("unit", "")).encode("ascii", "replace")[:5].ljust(6, b"\0")
            header += struct.pack("<8sffff6sBB", label, float(p["min"]), float(p["max"]), float(p["default"]),
                                  float(p["step"]), unit, ord(p["type"]), 0)
        else:
            header += bytes(PARAM_DEF_SIZE)
    assert len(header) == HEADER_SIZE

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(payload)

    # Read the file back through the 3.1 checks of tools/ptfx_inspect.py: a pack that does not parse is a bug here.
    spec = importlib.util.spec_from_file_location("ptfx_inspect", os.path.join(HERE, "ptfx_inspect.py"))
    insp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(insp)
    with open(out_path, "rb") as f:
        blob = f.read()
    info = insp.parse_header(blob)
    if info["status"] != "Ok":
        raise PackError("%s: the packed file is refused by the 3.1 checks: %s" % (out_path, info["status"]))

    print("packed %s: %s, flags 0x%04X, %d param(s), code %d B, bss %d B, %d bytes total"
          % (out_path, info["name"], flags, len(params), code_size, bss_size, len(blob)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", help="the plugin's C source (tests/ptfx/*.c)")
    ap.add_argument("sidecar", nargs="?", help="its JSON sidecar with the header fields (without it: from the source)")
    ap.add_argument("-o", "--out", required=True, help="the .ptfx file to write (under build-fixtures/)")
    ap.add_argument("--toolchain", help="arm-none-eabi- prefix (else ARM_GNU_ROOT / O3_ARM_TOOLCHAIN / PATH)")
    ap.add_argument("--abi-dir", default=os.path.join(ROOT, "tests", "ptfx"),
                    help="where ptfx_plugin_abi.h lives (default: tests/ptfx)")
    a = ap.parse_args(argv)
    try:
        return build(a.src, a.sidecar, a.out, a.toolchain, a.abi_dir)
    except PackError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
