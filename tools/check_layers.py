#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks for the source tree (stdlib only). ctest runs it as `layers` and `layers_selftest`.

1. Layer order: core <- usb <- hal <- eve <- ui <- app. A file in one of these layers may include
   <o3os/X/...> only for its own or a lower layer. platform/<name> and sim implement the HAL:
   platform may include core, usb and hal; sim may include every layer and platform. No layer may include
   platform or sim; tests may include anything.
   The project model sits beside that chain: core <- model <- formats. model may include core; formats may
   include core and model; app, sim and tests may include model and formats; core, hal, eve, ui and platform
   may not (the display never knows what a file is, the model never knows a screen).
   The audio engine (OS-005a) sits beside it too: audio may include core, usb and hal; app, platform, sim and tests
   may include audio; core, usb, hal, eve, ui, model and formats may not (the HAL contract never knows the engine).
   The MIDI timing core (OS-005c, `midi`) sits above the audio engine's event struct: midi may include core, usb, hal and audio; the sequencer
   (`seq`) may include core, usb, hal, audio, model and midi. app, platform (midi only: the MK66 MIDI drivers of OS-005c2 bind the emitter), sim and tests
   may include them; core, usb, hal, eve, ui, model, formats and audio may not.
   The module loader (OS-012a, `module`: the SD modules' file format, relocation and set) sits below the audio engine, which runs the loaded modules:
   module may include core, usb and hal; audio may include module; app, sim and tests may include it; core, usb, hal, eve, ui, model, formats, midi, seq and
   platform may not (the composition root, below, wires the loader's hooks to the hardware). One exception, by header name (LAYER_HEADER_EXCEPTIONS below):
   OS-FLASHDIET I1/I2 version-locks importers (kind 3) to the model's import-facing layout through the file header's word 44, so module may include exactly
   <o3os/model/import_layout.h>, the tag; nothing else of model.
   One exception, by file name (COMPOSITION_ROOTS): platform/mk66/src/app_main.cpp, the image's composition root, may include
   every layer; platform/qemu/tests/ptfx_gate_test.cpp (OS-CEPTFX S3b) may include the app, to run the plugin job on the target.
2. Rules for core, hal, eve, ui, app, model, formats: no standard header that owns heap memory, does I/O or blocks, no
   malloc/new, no `double` (software-emulated on the MK66's single-precision FPU) and no unsuffixed
   floating literal (it is a double: `a / 3.0` would promote silently). The build adds
   -fsingle-precision-constant to the same layers as a second line of defence.
3. Colours: in ui and app, colour values are written only in the theme tables, ui/src/themes.cpp (ADR 0001, ADR 0010
   section 8: a fixed set of 16 tokens, valued by the theme; style.h only names them). Elsewhere a six-digit hex
   literal (0xRRGGBB), color_rgb()/clear_color_rgb() with a number as an argument, or constructing an Rgb is a
   violation: name a ui::Color instead.
4. The document's editor: `app/src/session_access.h` is how dispatch() and the edit actions reach the editor of the
   session (OS-002c, ADR 0009: every model edit is an undo unit named by an action). No other file may include it, so no
   handler can make a unit of its own.
5. Licence: every first-party source file (.h .hpp .cpp .cc .c .py .cmake, CMakeLists.txt) carries an
   SPDX-License-Identifier (GPL-3.0-or-later; MIT for the module SDK) in its first lines.
6. The SD card's writer (OS-006c, platform/mk66/update): FatFs's write-class calls (f_write f_sync f_unlink f_rename f_truncate f_mkdir
   f_expand f_setlabel f_mkfs f_chmod f_utime f_forward) and the open flags that write (FA_WRITE FA_CREATE_NEW FA_CREATE_ALWAYS FA_OPEN_APPEND
   FA_OPEN_ALWAYS) are named in one source only, update/src/fat_firmware_store.cpp, which applies the path
   guard before each of them (rule 8 adds the save layer's writer, and OS-USBDISK U1d the bench's scratch file); the glue's disk_write / disk_ioctl are defined in storage/src/fat_storage.cpp only. Tests are not scanned.
7. The bootloader request pin (PTA1, board::kBootloaderRequest) is named in board.h, which defines it, and update/src/fw_host_mk66.cpp, the
   one place that drives it (the `fw update` hand-over). Tests are not scanned.
8. The save layer's write side (OS-007n): FatFs's write-class calls are also named in platform/mk66/save/src/fat_storage_writer.cpp (rule 6's second
   user), which applies hal/save_guard.h before each of them, and in platform/mk66/save/src/bench_scratch.cpp (OS-USBDISK U1d, Q14: the bench's one scratch file
   `o3-bench/disk-scratch.bin`, written only behind the Tracker-owns-the-card check; its path is shown by static_assert not to be a path of the guard). The headers that carry the write contract, <o3os/hal/storage_write.h> and
   <o3os/hal/save_guard.h>, are included only by SAVE_WRITE_USERS: the contract and the guard themselves, the FatFs writer, the save job and service of the app
   (app/atomic_save, app/save_service, app/recent_list, the list the service keeps and writes, and app/og_export, the names and plan of its OG export) and the image's composition root. Nothing else can ask for a change of the card: not a screen, not an action, not a module.
   Tests are not scanned.
9. Raw block writes (OS-USBDISK F6): `BlockDevice::write(lba, count, src)` sits below the write guard and the card gate. Its callers are the FatFs glue
   (platform/mk66/storage/src/fat_storage.cpp, disk_write) and the USB mass-storage bridge (platform/mk66/usb/src/msc_bridge.cpp, U1c), which writes only while the
   card's owner is the host and READ ONLY is off. Any other member call `.write(` / `->write(` with three arguments under platform/ is a violation. Tests are not scanned.
10. The bench's scratch file (OS-USBDISK U1d, Q14): <o3os/platform/mk66/save/bench_scratch.h> (the one writer of `diag bench disk`, which names FatFs write calls behind the
   Tracker-owns-the-card gate) is included only by the bench (platform/mk66/src/bench.cpp), its own source and header. No save path, screen or module can reach it. Tests are not scanned.

Usage: check_layers.py [--root os] | --self-test       Exit 0 clean, 1 violations.
"""
import argparse
import re
import sys
import tempfile
from pathlib import Path

LAYERS = ["core", "usb", "hal", "eve", "ui", "app"]
RANK = {name: i for i, name in enumerate(LAYERS)}
# Who may include what. Ranked layers: own layer and below (computed). The others are listed.
# The project model and its formats (OS-004) and the audio engine (OS-005a): not part of the ranked chain. app may use all
# of them; hal, eve and ui may not (a ranked layer only reaches what RANKED_EXTRA lists). The audio engine reaches down to
# the HAL contract it implements the render side of, never up to eve or ui.
DATA_LAYERS = ["model", "formats", "audio", "midi", "seq", "module"]
ALLOWED = {
    "model": {"core", "model"},
    "formats": {"core", "model", "formats"},
    "audio": {"core", "usb", "hal", "audio", "module"},
    "module": {"core", "usb", "hal", "module"},
    "midi": {"core", "usb", "hal", "audio", "midi"},
    "seq": {"core", "usb", "hal", "audio", "model", "midi", "seq"},
    "platform": {"core", "usb", "hal", "audio", "midi", "platform"},
    "sim": set(LAYERS) | set(DATA_LAYERS) | {"platform", "sim"},
    "tests": set(LAYERS) | set(DATA_LAYERS) | {"platform", "sim", "tests"},
}
RANKED_EXTRA = {"app": set(DATA_LAYERS)}
# Composition roots (OS-007c): the named files that wire the app to a platform's real devices, so they see both. A path, not
# a directory: nothing else under platform/ may include the app or ui. Keep this list at one entry per device image.
COMPOSITION_ROOTS = {Path("platform/mk66/src/app_main.cpp"): set(LAYERS) | set(DATA_LAYERS) | {"platform"},
                     # OS-CEPTFX S3b: the QEMU suite that runs the app's plugin job (app/src/ptfx_job.cpp) through the real call gate: the app header only
                     Path("platform/qemu/tests/ptfx_gate_test.cpp"): {"app"}}
# OS-FLASHDIET I1: (source layer, destination layer, header name) triples that are allowed although the layers may not include each other.
# The module loader compares a kind 3 file's header word 44 with the model's step-layout tag, so it reads exactly one model header.
LAYER_HEADER_EXCEPTIONS = {("module", "model", "import_layout.h")}
# Layers held to the no-heap / no-double / single-precision rules (2 below).
RULED = set(LAYERS) | set(DATA_LAYERS)
KNOWN = set(LAYERS) | set(ALLOWED)

BANNED_HEADERS = {
    # heap-owning containers and helpers
    "vector", "string", "map", "set", "unordered_map", "unordered_set", "deque", "list", "forward_list",
    "memory", "functional", "any", "regex",
    # I/O
    "iostream", "istream", "ostream", "sstream", "fstream", "cstdio", "stdio.h",
    # blocking / threads
    "thread", "mutex", "condition_variable", "future",
}
BANNED_TOKENS = [
    (re.compile(r"\bdouble\b"), "`double` is banned (single-precision FPU)"),
    (re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\("), "heap call is banned"),
    (re.compile(r"\bnew\b"), "`new` is banned (no heap)"),
]
# Colour values belong to the theme tables alone (ui and app only; eve encodes whatever it is given).
PALETTE_FILES = {Path("ui/src/themes.cpp")}
COLOUR_LAYERS = {"ui", "app"}
COLOUR_LITERALS = [
    (re.compile(r"\b0[xX][0-9A-Fa-f]{6}\b"), "colour literal 0xRRGGBB outside the theme tables (ui/src/themes.cpp): name a ui::Color"),
    (re.compile(r"\b(?:clear_)?color_rgb\s*\(\s*(?:0[xX][0-9A-Fa-f]+|\d)"),
     "colour_rgb() with a numeric colour outside the theme tables: pass the theme's value"),
    # (the declaration `struct Rgb {` in style.h is not a construction)
    (re.compile(r"(?<!struct )\bRgb\s*[{(]"), "colour (Rgb) constructed outside the theme tables: add it to a theme"),
]
# A floating literal (1.5, .5, 2., 1e3) with its optional suffix; only an f/F suffix makes it single precision.
# The lookarounds keep it off identifiers, member access (a.x, v[0].y) and hex integers (0x1e5).
FLOAT_LITERAL = re.compile(
    r"(?<![\w.])((?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)([A-Za-z_]\w*)?(?![\w.])")

# Who may include app/src/session_access.h (the key to the document's editor).
SESSION_ACCESS_USERS = {Path("app/src/actions.cpp"), Path("app/src/edit_actions.cpp")}

# Rule 6 and 7 (OS-006c): who may name what. Paths relative to the repository root.
WRITE_API = re.compile(r"\b(?:f_write|f_sync|f_unlink|f_rename|f_truncate|f_mkdir|f_expand|f_setlabel|f_mkfs|f_chmod|f_utime|f_forward|FA_WRITE|FA_CREATE_NEW|FA_CREATE_ALWAYS|FA_OPEN_APPEND|FA_OPEN_ALWAYS)\b")
WRITE_API_USERS = {Path("platform/mk66/update/src/fat_firmware_store.cpp"), Path("platform/mk66/save/src/fat_storage_writer.cpp"),
                   Path("platform/mk66/save/src/bench_scratch.cpp")}  # OS-USBDISK U1d (Q14): the bench's one scratch file, behind the Tracker-owns-the-card gate
DISK_WRITE_API = re.compile(r"\b(?:disk_write|disk_ioctl)\b")
DISK_WRITE_USERS = {Path("platform/mk66/storage/src/fat_storage.cpp")}
# Rule 9 (OS-USBDISK F6): who may call BlockDevice::write (a member call with three arguments: lba, count, source).
BLOCK_WRITE_CALL = re.compile(r"(?:\.|->)\s*write\s*\(")
BLOCK_WRITE_USERS = {Path("platform/mk66/storage/src/fat_storage.cpp"), Path("platform/mk66/usb/src/msc_bridge.cpp")}


def block_write_calls(code):
    """Line numbers of member calls `.write(a, b, c)` with exactly three top-level arguments."""
    lines = []
    for m in BLOCK_WRITE_CALL.finditer(code):
        depth, commas, i = 1, 0, m.end()
        while i < len(code) and depth > 0:
            ch = code[i]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            elif ch == "," and depth == 1:
                commas += 1
            i += 1
        if depth == 0 and commas == 2:
            lines.append(code.count("\n", 0, m.start()) + 1)
    return lines


# Rule 8 (OS-007n): who may include the write contract and its guard (by file name of the include).
SAVE_WRITE_HEADERS = {"storage_write.h", "save_guard.h"}
SAVE_WRITE_USERS = {
    Path("hal/include/o3os/hal/storage_write.h"), Path("hal/include/o3os/hal/save_guard.h"),
    Path("platform/mk66/save/include/o3os/platform/mk66/save/fat_storage_writer.h"), Path("platform/mk66/save/src/fat_storage_writer.cpp"),
    Path("app/include/o3os/app/atomic_save.h"), Path("app/src/atomic_save.cpp"),
    Path("app/include/o3os/app/save_service.h"), Path("app/src/save_service.cpp"),
    Path("app/include/o3os/app/recent_list.h"), Path("app/src/recent_list.cpp"),  # OS-FILES part B: the RECENT list is the save service's data and takes its names from the guard
    Path("app/include/o3os/app/og_export.h"), Path("app/src/og_export.cpp"),  # OS-FILES-C C2a: the names and the plan of an OG export are the save service's data and take their names from the guard
    Path("platform/mk66/save/src/bench_scratch.cpp"),  # OS-USBDISK U1d (Q14): the bench's scratch path must be shown not to be a path of the guard (static_assert)
    Path("platform/host/include/o3os/platform/host/dir_card.h"), Path("platform/host/src/dir_card.cpp"),  # OS-E2E-1b: the simulator's card, a scratch directory with a write side that applies the real guard before every change
    Path("platform/host/include/o3os/platform/host/host_card.h"),  # OS-QANET D1: the interface the simulator holds a card by (DirCard's and FatFsStorage's write contracts in one)
    Path("platform/host/fat/include/o3os/platform/host/fat_fs_storage.h"), Path("platform/host/fat/src/fat_fs_storage.cpp"),  # OS-QANET D1: the same card on a FAT32 image, through the unit's FatStorageWriter (the real guard)
    Path("platform/mk66/usb/include/o3os/platform/mk66/usb/screenshot_service.h"), Path("platform/mk66/usb/src/screenshot_service.cpp"),  # OS-SCREENSHOT: Shift + Power writes Screenshots/SHOT_nnnn.bmp (a Temp, then the NewOnly rename)
    Path("platform/mk66/src/app_main.cpp"),
}
# Rule 10 (OS-USBDISK U1d, Q14): who may include the bench's scratch file writer (by file name of the include).
BENCH_SCRATCH_HEADER = "bench_scratch.h"
BENCH_SCRATCH_USERS = {
    Path("platform/mk66/src/bench.cpp"), Path("platform/mk66/save/src/bench_scratch.cpp"),
    Path("platform/mk66/save/include/o3os/platform/mk66/save/bench_scratch.h"),
}
BOOTLOADER_REQUEST = re.compile(r"\bkBootloaderRequest\b")
BOOTLOADER_REQUEST_USERS = {Path("platform/mk66/include/o3os/platform/mk66/board.h"), Path("platform/mk66/update/src/fw_host_mk66.cpp")}

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".c", ".py", ".cmake"}
SPDX = re.compile(r"SPDX-License-Identifier:\s*(?:GPL-3\.0-or-later|MIT)\b")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
# Comments are blanked; string/char literals are kept intact so a `//` inside one is not a comment.
COMMENT_OR_LITERAL = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL)
LITERAL = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')


def blank(match):
    """Replace with spaces, keeping newlines so line numbers stay valid."""
    return re.sub(r"[^\n]", " ", match.group(0))


def strip_comments(text):
    return COMMENT_OR_LITERAL.sub(lambda m: m.group(0) if m.group(0)[0] in "\"'" else blank(m), text)


def strip_literals(text):
    return LITERAL.sub(blank, text)


def layer_of(rel):
    """Layer of a path relative to the repository root, or None for files outside the layered tree."""
    top = rel.parts[0] if rel.parts else ""
    return top if top in KNOWN else None


def may_include(src_layer, dst_layer):
    if src_layer in RANK:
        return (dst_layer in RANK and RANK[dst_layer] <= RANK[src_layer]) or dst_layer in RANKED_EXTRA.get(src_layer, ())
    return dst_layer in ALLOWED[src_layer]


# Directories that are not first-party source. Matched against directory names only: a file or a directory
# that merely starts with "build" (builder/, build_info.h) is still checked.
SKIPPED_DIR = re.compile(r"^(?:build(?:-.*)?|\..*|__pycache__|third_party)$")


def source_files(root):
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if not path.is_file() or any(SKIPPED_DIR.match(part) for part in rel.parts[:-1]):
            continue
        if path.suffix in SOURCE_SUFFIXES or path.name == "CMakeLists.txt":
            yield path, rel


def check_tree(root):
    """Return a sorted list of (relative path, line, message)."""
    found = []
    for path, rel in source_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")

        if not any(SPDX.search(line) for line in text.splitlines()[:5]):
            found.append((str(rel), 1, "missing SPDX-License-Identifier (GPL-3.0-or-later or MIT) in the first lines"))

        layer = layer_of(rel)
        if layer is None or path.suffix not in {".h", ".hpp", ".cpp", ".cc", ".c"}:
            continue
        code = strip_comments(text)

        if layer != "tests" and "tests" not in rel.parts:
            named = strip_literals(code)  # a name inside a string is not a call
            for pattern, users, message in (
                    (WRITE_API, WRITE_API_USERS, "FatFs write-class call `%s` outside update/src/fat_firmware_store.cpp, save/src/fat_storage_writer.cpp and save/src/bench_scratch.cpp (rule 6)"),
                    (DISK_WRITE_API, DISK_WRITE_USERS, "`%s` outside storage/src/fat_storage.cpp (rule 6)"),
                    (BOOTLOADER_REQUEST, BOOTLOADER_REQUEST_USERS, "`%s`, the bootloader request pin, outside board.h and update/src/fw_host_mk66.cpp (rule 7)")):
                if rel in users:
                    continue
                for m in pattern.finditer(named):
                    found.append((str(rel), named.count("\n", 0, m.start()) + 1, message % m.group(0)))

        if layer == "platform" and "tests" not in rel.parts and rel not in BLOCK_WRITE_USERS:  # (the usb layer's msc::BlockStore is the engine's own seam, adapted by the bridge)
            for line in block_write_calls(strip_literals(code)):
                found.append((str(rel), line, "BlockDevice::write outside storage/src/fat_storage.cpp and usb/src/msc_bridge.cpp (rule 9)"))

        for m in INCLUDE.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            quote, target = m.group(1), m.group(2)
            dest = None
            if (layer != "tests" and "tests" not in rel.parts and Path(target).name in SAVE_WRITE_HEADERS and rel not in SAVE_WRITE_USERS):
                found.append((str(rel), line, "%s (the card's write contract and guard) is for the save layer only (rule 8)" % Path(target).name))
            if (layer != "tests" and "tests" not in rel.parts and Path(target).name == BENCH_SCRATCH_HEADER and rel not in BENCH_SCRATCH_USERS):
                found.append((str(rel), line, "bench_scratch.h (the bench's scratch file writer) is for platform/mk66/src/bench.cpp only (rule 10)"))
            if Path(target).name == "session_access.h" and rel not in SESSION_ACCESS_USERS:
                found.append((str(rel), line, "session_access.h (the document's editor) is for dispatch() and the edit actions only"))
            if target.startswith("o3os/"):
                dest = target.split("/")[1]
                if dest not in KNOWN:
                    found.append((str(rel), line, "unknown o3os layer '%s' in include" % dest))
                    continue
            elif quote == '"' and ".." in Path(target).parts:
                resolved = (path.parent / target).resolve()
                try:
                    dest = layer_of(resolved.relative_to(root.resolve()))
                except ValueError:
                    dest = None
            elif quote == "<" and "/" not in target and layer in RULED and target in BANNED_HEADERS:
                found.append((str(rel), line, "<%s> is banned in %s (heap, I/O or blocking)" % (target, layer)))
            if dest is not None and not may_include(layer, dest) and (layer, dest, Path(target).name) not in LAYER_HEADER_EXCEPTIONS and dest not in COMPOSITION_ROOTS.get(rel, ()):
                found.append((str(rel), line, "layer violation: %s must not include %s (%s)" % (layer, dest, target)))

        if layer in RULED:
            bare = strip_literals(code)
            for pattern, message in BANNED_TOKENS:
                for m in pattern.finditer(bare):
                    found.append((str(rel), bare.count("\n", 0, m.start()) + 1, message))
            if layer in COLOUR_LAYERS and rel not in PALETTE_FILES:
                for pattern, message in COLOUR_LITERALS:
                    for m in pattern.finditer(bare):
                        found.append((str(rel), bare.count("\n", 0, m.start()) + 1, message))
            for m in FLOAT_LITERAL.finditer(bare):
                if (m.group(2) or "") not in ("f", "F"):
                    found.append((str(rel), bare.count("\n", 0, m.start()) + 1,
                                  "floating literal `%s` is a double: write it with an f suffix" % m.group(0)))
    return sorted(found)


def report(found):
    for rel, line, message in found:
        print("%s:%d: %s" % (rel, line, message))
    print("check_layers: %d violation(s)" % len(found) if found else "check_layers: ok")
    return 1 if found else 0


def self_test():
    """The check must accept a clean tree and reject each kind of break; a vacuous check fails here."""
    head = "// SPDX-License-Identifier: GPL-3.0-or-later\n"
    clean = {
        # Banned words inside a comment or a string literal must not trigger the token checks.
        "core/include/o3os/core/a.h": head + "#include <cstdint>\n#include <o3os/core/b.h>\n"
                                      "// double new malloc( <vector>\n"
                                      'static const char* s = "double new // not a comment";\n',
        "usb/include/o3os/usb/u.h": head + "#include <o3os/core/a.h>\n#include <o3os/usb/v.h>\n",
        "hal/include/o3os/hal/h.h": head + "#include <o3os/core/a.h>\n#include <o3os/usb/u.h>\n",
        "eve/src/e.cpp": head + "#include <o3os/hal/h.h>\n",
        "ui/ui.h": head + "#include <o3os/eve/e.h>\n",
        "app/app.h": head + "#include <o3os/ui/ui.h>\n",
        "app/src/actions.cpp": head + '#include "session_access.h"\n',
        "app/src/edit_actions.cpp": head + '#include "session_access.h"\n',
        "app/src/session_access.h": head + "#pragma once\n",
        # Rule 9: the two users of BlockDevice::write, and two-argument writes (a log, a CDC writer) elsewhere.
        "platform/mk66/storage/src/fat_storage.cpp": head + "int f() { return g_device->write(lba, count, buff); }\n",
        "platform/mk66/usb/src/msc_bridge.cpp": head + "int f() { return dev.write(lba, 1, buf); }\n",
        "platform/mk66/usb/src/console_ok.cpp": head + "void f() { cdc.write(data, n); g_log.write(a(b, c), d); }\n",
        "platform/host/p.cpp": head + "#include <vector>\n#include <o3os/hal/h.h>\n",
        "platform/mk66/usbdrv.cpp": head + "#include <o3os/core/a.h>\n#include <o3os/usb/u.h>\n#include <o3os/hal/h.h>\n",
        "sim/s.cpp": head + "#include <o3os/app/app.h>\n#include <o3os/platform/host/p.h>\n",
        "tests/t.cpp": head + "#include <o3os/app/app.h>\n#include <map>\n",
        "third_party/x/x.h": "no header needed here\n",
        # Skipped build output, but names that merely start with "build" are real source and are checked.
        "build-x/gen.h": "no header, #include <vector>, double d = 1.5;\n",
        "app/builder/ok.h": head + "#include <o3os/ui/ui.h>\n",
        "core/build_info.h": head + "#pragma once\n",
        # Single-precision literals, hex integers and member access are fine.
        "ui/float_ok.cpp": head + "float f(float a) { return a * 1.5f + 2.f + .5F + 1e-3f; }\n"
                                  "int h = 0x1e5; int y = v[0].y + a.x;\n",
        # Colours: the theme tables may hold them; naming a colour, a mask that is not 6 digits, comments and
        # strings, and the layers that only encode (eve) are fine.
        "ui/include/o3os/ui/style.h": head + "struct Rgb { unsigned char r, g, b; };\nenum class Color { A };\n",
        "ui/src/themes.cpp": head + "struct Rgb { unsigned char r, g, b; };\n"
                             "inline constexpr Rgb kP[] = {Rgb{1, 2, 3}};\nint k = 0xFF8800;\n",
        "ui/src/ok_colour.cpp": head + "void f(const Rgb& v) { dl::color_rgb(v.r, v.g, v.b); dl::clear_color_rgb(c.r, 0 + c.g, c.b); }\n",
        "app/ok_hex.cpp": head + "int mask = 0xFFFF; int w = 0x12345678; // 0xFF8800\nconst char* s = \"0xFF8800\";\n",
        "eve/dl_colour.h": head + "int x = color_rgb(255, 0, 0); int y = 0xFF8800; Rgb z{1, 2, 3};\n",
        "CMakeLists.txt": "# SPDX-License-Identifier: GPL-3.0-or-later\n",
        # The data layers: model sees core, formats sees core and model, app / sim / tests may see both. The colour
        # rule is for ui and app only, so a six-digit hex value is just a number in model.
        "model/include/o3os/model/m.h": head + "#include <o3os/core/a.h>\n#include <cstdint>\nint k = 0xFF8800;\n",
        "formats/src/f.cpp": head + "#include <o3os/model/m.h>\n#include <o3os/core/a.h>\n#include \"local.h\"\n",
        "app/uses_data.cpp": head + "#include <o3os/model/m.h>\n#include <o3os/formats/f.h>\n",
        "sim/uses_data.cpp": head + "#include <o3os/model/m.h>\n#include <o3os/formats/f.h>\n",
        "tests/uses_data.cpp": head + "#include <o3os/model/m.h>\n#include <o3os/formats/f.h>\n#include <vector>\n",
        # The audio engine: core, usb and hal below it; app, platform, sim and tests above.
        "audio/include/o3os/audio/a.h": head + "#include <o3os/core/a.h>\n#include <o3os/usb/u.h>\n#include <o3os/hal/h.h>\n"
                                        "#include <o3os/audio/b.h>\n#include <atomic>\n",
        "platform/mk66/audio_drv.cpp": head + "#include <o3os/audio/a.h>\n#include <o3os/hal/h.h>\n",
        "app/uses_audio.cpp": head + "#include <o3os/audio/a.h>\n",
        "sim/uses_audio.cpp": head + "#include <o3os/audio/a.h>\n",
        "tests/uses_audio.cpp": head + "#include <o3os/audio/a.h>\n#include <vector>\n",
        # The MIDI timing core: core, usb, hal and audio below it; the sequencer sees those and the model and midi; app, platform (midi), sim, tests above.
        "midi/include/o3os/midi/m.h": head + "#include <o3os/core/a.h>\n#include <o3os/usb/u.h>\n#include <o3os/hal/h.h>\n#include <o3os/audio/a.h>\n"
                                      "#include <o3os/midi/n.h>\n#include <atomic>\n",
        "seq/include/o3os/seq/s.h": head + "#include <o3os/core/a.h>\n#include <o3os/model/m.h>\n#include <o3os/audio/a.h>\n#include <o3os/midi/m.h>\n"
                                    "#include <o3os/seq/t.h>\n",
        "platform/mk66/midi_drv.cpp": head + "#include <o3os/midi/m.h>\n#include <o3os/hal/h.h>\n",
        "app/uses_seq.cpp": head + "#include <o3os/seq/s.h>\n#include <o3os/midi/m.h>\n",
        "sim/uses_seq.cpp": head + "#include <o3os/seq/s.h>\n#include <o3os/midi/m.h>\n",
        "tests/uses_seq.cpp": head + "#include <o3os/seq/s.h>\n#include <o3os/midi/m.h>\n#include <vector>\n",
        # The module loader: core, usb and hal below it; the audio engine, app, sim and tests above.
        "module/include/o3os/module/m.h": head + "#include <o3os/core/a.h>\n#include <o3os/usb/u.h>\n#include <o3os/hal/h.h>\n#include <o3os/module/n.h>\n",
        "module/src/uses_tag.cpp": head + "#include <o3os/model/import_layout.h>\n",  # OS-FLASHDIET I1: the one model header module may read
        "audio/src/uses_module.cpp": head + "#include <o3os/module/m.h>\n",
        "app/uses_module.cpp": head + "#include <o3os/module/m.h>\n",
        "sim/uses_module.cpp": head + "#include <o3os/module/m.h>\n",
        "tests/uses_module.cpp": head + "#include <o3os/module/m.h>\n#include <vector>\n",
        # OS-006c: the one writer and the one pin driver may name their calls; a comment or a string that mentions them is nothing; tests are not scanned.
        "platform/mk66/update/src/fat_firmware_store.cpp": head + "void f() { f_write(0, 0, 0, 0); f_sync(0); f_unlink(0); f_rename(0, 0); int m = FA_WRITE | FA_CREATE_NEW; }\n",
        "platform/mk66/storage/src/fat_storage.cpp": head + "int disk_write(); int disk_ioctl();\n",
        "platform/mk66/include/o3os/platform/mk66/board.h": head + "inline constexpr int kBootloaderRequest = 1;\n",
        "platform/mk66/update/src/fw_host_mk66.cpp": head + "void f() { use(board::kBootloaderRequest); }\n",
        "platform/mk66/storage/src/mentions.cpp": head + "// f_write f_unlink disk_write kBootloaderRequest\nconst char* s = \"f_sync kBootloaderRequest\";\nint f_writeable = 0;\n",
        "platform/mk66/tests/uses_writer.cpp": head + "void f() { f_write(0, 0, 0, 0); use(board::kBootloaderRequest); }\n",
        "tests/uses_writer.cpp": head + "void f() { f_write(0, 0, 0, 0); f_unlink(0); disk_write(); use(board::kBootloaderRequest); }\n",
        # OS-007n: the save layer's writer names FatFs's write calls too; the save job, the service, the writer and the composition root see the write contract.
        "platform/mk66/save/src/fat_storage_writer.cpp": head + "#include <o3os/hal/storage_write.h>\nvoid f() { f_write(0, 0, 0, 0); f_rename(0, 0); f_unlink(0); f_mkdir(0); int m = FA_CREATE_NEW | FA_WRITE; }\n",
        "platform/mk66/save/src/bench_scratch.cpp": head + "#include <o3os/hal/storage_write.h>\n#include <o3os/hal/save_guard.h>\nvoid f() { f_mkdir(0); f_write(0, 0, 0, 0); f_sync(0); f_unlink(0); int m = FA_WRITE | FA_OPEN_ALWAYS; }\n",
        "platform/mk66/save/include/o3os/platform/mk66/save/bench_scratch.h": head + "#include <o3os/hal/storage.h>\n",
        "platform/mk66/src/bench.cpp": head + "#include <o3os/platform/mk66/save/bench_scratch.h>\n",
        "platform/mk66/tests/uses_bench_scratch.cpp": head + "#include <o3os/platform/mk66/save/bench_scratch.h>\n",
        "tests/uses_bench_scratch.cpp": head + "#include <o3os/platform/mk66/save/bench_scratch.h>\n",
        "app/src/atomic_save.cpp": head + "#include <o3os/hal/storage_write.h>\n#include <o3os/hal/save_guard.h>\n",
        "app/src/save_service.cpp": head + "#include <o3os/hal/storage_write.h>\n",
        "platform/host/src/dir_card.cpp": head + "#include <o3os/hal/storage_write.h>\n#include <o3os/hal/save_guard.h>\nint f() { return ::write(1, nullptr, 0); }\n",
        "hal/include/o3os/hal/storage_write.h": head + "#include <o3os/hal/save_guard.h>\n",
        "tests/uses_save.cpp": head + "#include <o3os/hal/storage_write.h>\n#include <o3os/hal/save_guard.h>\nvoid f() { f_rename(0, 0); }\n",
        # The composition root of the MK66 image sees the app and ui; it is that one file.
        "platform/mk66/src/app_main.cpp": head + "#include <o3os/app/app.h>\n#include <o3os/ui/assets.h>\n#include <o3os/hal/h.h>\n",
    }
    cases = [  # (file, content, expected message fragment)
        ("core/include/o3os/core/up.h", head + "#include <o3os/eve/dl.h>\n", "core must not include eve"),
        ("hal/include/o3os/hal/up.h", head + "#include <o3os/ui/x.h>\n", "hal must not include ui"),
        ("core/up_usb.h", head + "#include <o3os/usb/u.h>\n", "core must not include usb"),
        ("usb/up.h", head + "#include <o3os/hal/h.h>\n", "usb must not include hal"),
        ("usb/up2.cpp", head + "#include <o3os/platform/host/p.h>\n", "usb must not include platform"),
        ("usb/dbl.cpp", head + "double x = 1.0;\n", "`double` is banned"),
        ("usb/dbl2.cpp", head + "float f(float a) { return a * 0.5; }\n", "floating literal `0.5`"),
        ("usb/heap.h", head + "#include <vector>\n", "<vector> is banned"),
        ("eve/src/up.cpp", head + "#include <o3os/app/x.h>\n", "eve must not include app"),
        ("app/up.cpp", head + "#include <o3os/platform/host/p.h>\n", "app must not include platform"),
        ("ui/up.cpp", head + "#include <o3os/sim/s.h>\n", "ui must not include sim"),
        ("platform/up.cpp", head + "#include <o3os/ui/x.h>\n", "platform must not include ui"),
        ("platform/up2.cpp", head + "#include <o3os/eve/e.h>\n", "platform must not include eve"),
        ("core/up2.h", head + '#include "../hal/include/o3os/hal/h.h"\n', "core must not include hal"),
        ("core/up3.h", head + "#include <o3os/nope/x.h>\n", "unknown o3os layer"),
        ("core/heap.h", head + "#include <vector>\n", "<vector> is banned"),
        ("app/heap.cpp", head + "#include <iostream>\n", "<iostream> is banned"),
        ("eve/dbl.cpp", head + "double x = 1.0;\n", "`double` is banned"),
        ("ui/new.cpp", head + "int* p = new int;\n", "`new` is banned"),
        ("hal/mal.cpp", head + "void f() { malloc(4); }\n", "heap call is banned"),
        ("eve/dbl2.cpp", head + "float f(float a) { return a / 3.0; }\n", "floating literal `3.0`"),
        ("ui/dbl3.cpp", head + "int g(int a) { return a * 1.5; }\n", "floating literal `1.5`"),
        ("hal/dbl4.cpp", head + "float k = 1e3;\n", "floating literal `1e3`"),
        ("core/dbl5.h", head + "constexpr long double kX = 2.0L;\n", "floating literal `2.0L`"),
        ("app/builder/up.h", head + "#include <o3os/platform/host/p.h>\n", "app must not include platform"),
        ("app/colour_hex.cpp", head + "int c = 0xFF8800;\n", "colour literal 0xRRGGBB"),
        ("ui/other_style.h", head + "int c = 0x102030;\n", "colour literal 0xRRGGBB"),
        ("ui/include/o3os/ui/style.h", head + "int c = 0x102030;\n", "colour literal 0xRRGGBB"),  # not a palette: it only names the tokens
        ("ui/src/other_theme.cpp", head + "auto c = Rgb{1, 2, 3};\n", "colour (Rgb) constructed"),
        ("ui/colour_call.cpp", head + "void f() { dl::color_rgb(255, 0, 0); }\n", "colour_rgb() with a numeric"),
        ("ui/colour_clear.cpp", head + "void f() { dl::clear_color_rgb(0x10, g, b); }\n", "colour_rgb() with a numeric"),
        ("app/colour_rgb.cpp", head + "auto c = ui::Rgb{1, 2, 3};\n", "colour (Rgb) constructed"),
        ("app/colour_rgb2.cpp", head + "auto c = Rgb(1, 2, 3);\n", "colour (Rgb) constructed"),
        ("app/src/ui_actions.cpp", head + '#include "session_access.h"\n', "session_access.h (the document's editor)"),
        ("app/src/screens/overlays.cpp", head + '#include "../session_access.h"\n', "session_access.h (the document's editor)"),
        ("tests/sneaky.cpp", head + "#include <app/src/session_access.h>\n", "session_access.h (the document's editor)"),
        ("core/build_nolic.h", "#pragma once\n", "missing SPDX"),
        ("app/builder/bad.h", head + "double d;\n", "`double` is banned"),
        ("core/nolic.h", "#pragma once\n", "missing SPDX"),
        ("model/up.h", head + "#include <o3os/formats/f.h>\n", "model must not include formats"),
        ("model/up2.h", head + "#include <o3os/ui/ui.h>\n", "model must not include ui"),
        ("model/up3.h", head + "#include <o3os/hal/h.h>\n", "model must not include hal"),
        ("formats/up.cpp", head + "#include <o3os/app/app.h>\n", "formats must not include app"),
        ("formats/up2.cpp", head + "#include <o3os/hal/h.h>\n", "formats must not include hal"),
        ("formats/up3.cpp", head + "#include <o3os/platform/host/p.h>\n", "formats must not include platform"),
        ("core/up_model.h", head + "#include <o3os/model/m.h>\n", "core must not include model"),
        ("hal/up_model.h", head + "#include <o3os/model/m.h>\n", "hal must not include model"),
        ("eve/up_formats.cpp", head + "#include <o3os/formats/f.h>\n", "eve must not include formats"),
        ("ui/up_model.cpp", head + "#include <o3os/model/m.h>\n", "ui must not include model"),
        ("platform/up_model.cpp", head + "#include <o3os/model/m.h>\n", "platform must not include model"),
        ("model/heap.h", head + "#include <vector>\n", "<vector> is banned"),
        ("formats/heap.cpp", head + "#include <string>\n", "<string> is banned"),
        ("formats/io.cpp", head + "#include <cstdio>\n", "<cstdio> is banned"),
        ("model/dbl.cpp", head + "double x = 1.0;\n", "`double` is banned"),
        ("formats/dbl2.cpp", head + "float f(float a) { return a / 3.0; }\n", "floating literal `3.0`"),
        ("formats/new.cpp", head + "int* p = new int;\n", "`new` is banned"),
        ("model/mal.cpp", head + "void f() { malloc(4); }\n", "heap call is banned"),
        ("model/nolic.h", "#pragma once\n", "missing SPDX"),
        ("formats/nolic.py", "print(1)\n", "missing SPDX"),
        ("tools/nolic.py", "print(1)\n", "missing SPDX"),
        ("platform/mk66/storage/src/sneaky_write.cpp", head + "void f() { f_write(0, 0, 0, 0); }\n", "FatFs write-class call `f_write`"),
        ("platform/mk66/src/sneaky_unlink.cpp", head + "void f() { f_unlink(0); }\n", "FatFs write-class call `f_unlink`"),
        ("platform/mk66/usb/src/sneaky_sync.cpp", head + "void f() { f_sync(0); }\n", "FatFs write-class call `f_sync`"),
        ("app/sneaky_rename.cpp", head + "void f() { f_rename(0, 0); }\n", "FatFs write-class call `f_rename`"),
        ("platform/mk66/storage/src/sneaky_flag.cpp", head + "int m = FA_READ | FA_WRITE;\n", "FatFs write-class call `FA_WRITE`"),
        ("app/sneaky_create.cpp", head + "int m = FA_CREATE_ALWAYS;\n", "FatFs write-class call `FA_CREATE_ALWAYS`"),
        ("platform/mk66/update/src/other.cpp", head + "void f() { f_mkdir(0); }\n", "FatFs write-class call `f_mkdir`"),
        ("platform/mk66/update/src/other2.cpp", head + "void f() { f_mkfs(0, 0, 0, 0); }\n", "FatFs write-class call `f_mkfs`"),
        ("platform/mk66/usb/src/sneaky_block.cpp", head + "void f() { dev.write(lba, 1, buf); }\n", "BlockDevice::write outside"),
        ("platform/mk66/update/src/sneaky_block2.cpp", head + "void f() { g_device->write(static_cast<unsigned>(lba), count, buff); }\n", "BlockDevice::write outside"),
        ("platform/mk66/update/src/sneaky_disk.cpp", head + "int disk_write();\n", "`disk_write` outside storage/src/fat_storage.cpp"),
        ("platform/mk66/update/src/sneaky_ioctl.cpp", head + "int x = disk_ioctl;\n", "`disk_ioctl` outside storage/src/fat_storage.cpp"),
        ("platform/mk66/src/sneaky_pin.cpp", head + "void f() { gpio(board::kBootloaderRequest); }\n", "bootloader request pin"),
        ("platform/mk66/update/src/fw_service.cpp", head + "void f() { gpio(board::kBootloaderRequest); }\n", "bootloader request pin"),
        ("app/pin.cpp", head + "int x = kBootloaderRequest;\n", "bootloader request pin"),
        ("module/up_audio.h", head + "#include <o3os/audio/a.h>\n", "module must not include audio"),
        ("module/up_model.cpp", head + "#include <o3os/model/m.h>\n", "module must not include model"),
        ("module/up_step.cpp", head + "#include <o3os/model/step.h>\n", "module must not include model"),  # only import_layout.h is excepted
        ("module/up_platform.cpp", head + "#include <o3os/platform/mk66/p.h>\n", "module must not include platform"),
        ("module/dbl.cpp", head + "double x = 1.0;\n", "`double` is banned"),
        ("module/heap.cpp", head + "#include <vector>\n", "<vector> is banned"),
        ("core/up_module.h", head + "#include <o3os/module/m.h>\n", "core must not include module"),
        ("hal/up_module.h", head + "#include <o3os/module/m.h>\n", "hal must not include module"),
        ("midi/up_module.cpp", head + "#include <o3os/module/m.h>\n", "midi must not include module"),
        ("seq/up_module.cpp", head + "#include <o3os/module/m.h>\n", "seq must not include module"),
        ("platform/up_module.cpp", head + "#include <o3os/module/m.h>\n", "platform must not include module"),
        ("audio/up_eve.cpp", head + "#include <o3os/eve/e.h>\n", "audio must not include eve"),
        ("audio/up_ui.h", head + "#include <o3os/ui/ui.h>\n", "audio must not include ui"),
        ("audio/up_model.cpp", head + "#include <o3os/model/m.h>\n", "audio must not include model"),
        ("audio/up_platform.cpp", head + "#include <o3os/platform/mk66/p.h>\n", "audio must not include platform"),
        ("audio/dbl.cpp", head + "double x = 1.0;\n", "`double` is banned"),
        ("audio/dbl2.cpp", head + "float f(float a) { return a * 0.5; }\n", "floating literal `0.5`"),
        ("audio/heap.cpp", head + "#include <vector>\n", "<vector> is banned"),
        ("audio/io.cpp", head + "#include <cstdio>\n", "<cstdio> is banned"),
        ("hal/up_audio.h", head + "#include <o3os/audio/a.h>\n", "hal must not include audio"),
        ("core/up_audio.h", head + "#include <o3os/audio/a.h>\n", "core must not include audio"),
        ("usb/up_audio.h", head + "#include <o3os/audio/a.h>\n", "usb must not include audio"),
        ("eve/up_audio.cpp", head + "#include <o3os/audio/a.h>\n", "eve must not include audio"),
        ("ui/up_audio.cpp", head + "#include <o3os/audio/a.h>\n", "ui must not include audio"),
        ("model/up_audio.h", head + "#include <o3os/audio/a.h>\n", "model must not include audio"),
        ("formats/up_audio.cpp", head + "#include <o3os/audio/a.h>\n", "formats must not include audio"),
        ("audio/nolic.h", "#pragma once\n", "missing SPDX"),
        ("midi/up_model.cpp", head + "#include <o3os/model/m.h>\n", "midi must not include model"),
        ("midi/up_seq.cpp", head + "#include <o3os/seq/s.h>\n", "midi must not include seq"),
        ("midi/up_ui.h", head + "#include <o3os/ui/ui.h>\n", "midi must not include ui"),
        ("midi/up_platform.cpp", head + "#include <o3os/platform/mk66/p.h>\n", "midi must not include platform"),
        ("midi/dbl.cpp", head + "double x = 1.0;\n", "`double` is banned"),
        ("midi/heap.cpp", head + "#include <vector>\n", "<vector> is banned"),
        ("seq/up_app.cpp", head + "#include <o3os/app/app.h>\n", "seq must not include app"),
        ("seq/up_formats.cpp", head + "#include <o3os/formats/f.h>\n", "seq must not include formats"),
        ("seq/up_platform.cpp", head + "#include <o3os/platform/mk66/p.h>\n", "seq must not include platform"),
        ("seq/dbl2.cpp", head + "float f(float a) { return a * 0.5; }\n", "floating literal `0.5`"),
        ("seq/new.cpp", head + "int* p = new int;\n", "`new` is banned"),
        ("audio/up_midi.cpp", head + "#include <o3os/midi/m.h>\n", "audio must not include midi"),
        ("audio/up_seq.cpp", head + "#include <o3os/seq/s.h>\n", "audio must not include seq"),
        ("model/up_seq.h", head + "#include <o3os/seq/s.h>\n", "model must not include seq"),
        ("core/up_midi.h", head + "#include <o3os/midi/m.h>\n", "core must not include midi"),
        ("hal/up_seq.h", head + "#include <o3os/seq/s.h>\n", "hal must not include seq"),
        ("platform/up_seq.cpp", head + "#include <o3os/seq/s.h>\n", "platform must not include seq"),
        ("midi/nolic.h", "#pragma once\n", "missing SPDX"),
        ("seq/nolic.h", "#pragma once\n", "missing SPDX"),
        ("app/src/save_actions.cpp", head + "#include <o3os/hal/storage_write.h>\n", "write contract and guard) is for the save layer only (rule 8)"),
        ("app/src/screens/record_screen.cpp", head + "#include <o3os/hal/save_guard.h>\n", "write contract and guard) is for the save layer only (rule 8)"),
        ("platform/mk66/update/src/fat_firmware_store2.cpp", head + "#include <o3os/hal/storage_write.h>\n", "write contract and guard) is for the save layer only (rule 8)"),
        ("platform/mk66/save/src/fat_storage_writer2.cpp", head + '#include "o3os/platform/mk66/save/bench_scratch.h"\n', "bench's scratch file writer) is for platform/mk66/src/bench.cpp only (rule 10)"),
        ("platform/mk66/src/app_main2.cpp", head + "#include <o3os/platform/mk66/save/bench_scratch.h>\n", "bench's scratch file writer) is for platform/mk66/src/bench.cpp only (rule 10)"),
        ("app/src/screens/bench_screen.cpp", head + "#include <o3os/platform/mk66/save/bench_scratch.h>\n", "bench's scratch file writer) is for platform/mk66/src/bench.cpp only (rule 10)"),
        ("platform/mk66/save/src/sneaky_remove.cpp", head + "void f() { f_unlink(0); }\n", "FatFs write-class call `f_unlink`"),
        ("sim/src/sneaky_save.cpp", head + "#include <o3os/hal/storage_write.h>\n", "write contract and guard) is for the save layer only (rule 8)"),
        # The composition-root exception is one file: its neighbours and namesakes elsewhere stay inside the rule.
        ("platform/mk66/src/main.cpp", head + "#include <o3os/app/app.h>\n", "platform must not include app"),
        ("platform/mk66/display/src/device.cpp", head + "#include <o3os/ui/assets.h>\n", "platform must not include ui"),
        ("platform/host/src/app_main.cpp", head + "#include <o3os/app/app.h>\n", "platform must not include app"),
    ]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, content in clean.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(content)
        baseline = check_tree(root)
        if baseline:
            failures += 1
            print("self-test: clean tree reported violations: %r" % (baseline,))
        for name, content, fragment in cases:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
            messages = [m for rel, _, m in check_tree(root) if rel == name]
            if not any(fragment in m for m in messages):
                failures += 1
                print("self-test: %s: expected '%s', got %r" % (name, fragment, messages))
            target.unlink()
    print("check_layers self-test: %s" % ("FAILED (%d)" % failures if failures else "ok (%d cases)" % len(cases)))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    return report(check_tree(args.root))


if __name__ == "__main__":
    sys.exit(main())
