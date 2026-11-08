#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static checks of an MK66 firmware image in Intel HEX form (stdlib only): the static part of flashing-safety Gate C.

    check_image.py check HEX [--expect-sp 0x20030000] [--vectors 116] [--ramfunc-l 0x1FFF0000:0xC000] [--json OUT] [--delete-on-fail]
                           [--allow-last-sector]
    check_image.py flashconfig HEX --expect ffffffffffffffffffffffffdef9ffff
    check_image.py noinit ELF --nm arm-none-eabi-nm --expect platform/mk66/tools/noinit_layout.txt [--write]
    check_image.py --self-test

`check` runs every check below and exits 1 if any fails (nothing is flashed, copied or handed over by this tool):

  hex_parse      every record is well formed and its checksum is right, an EOF record ends the file, no byte is given twice,
                 only record types 00, 01, 02 and 04 (no start-address record: the SD bootloader's handling of one is unknown)
  address_range  every populated address is <= 0x000FFFFF (the MK66FX1M0's 1 MiB of program flash)
  head_present   addresses 0..0x40F are all populated (vector table, startup, flash configuration field)
  contiguous     the image is one run from address 0 to its last byte: no hole anywhere (whether the SD bootloader
                 tolerates one is unknown)
  sector_end     the image ends on a 4 KiB flash sector boundary (last address + 1 is a multiple of 4096): the SD bootloader leaves
                 a final sector that the image fills only in part neither erased nor programmed, so it keeps the bytes of the
                 previous image there (OS-BUG-COSMBOOT, docs/hw/tracker-hardware-notes.md section 3). The linker pads with zeros
  last_sector_free  the image ends at or below 0xFF000 (last address + 1 <= 0xFF000): the last 4 KiB flash sector (0xFF000) stays empty.
                 The first image that filled it (b5-bench-417fbe4a, end 0x100000) was armed through the console, handed over to the SD
                 bootloader and the unit never came back on USB for over 10 minutes; b4-bench-7e2567d0, ending at 0xFF000, booted
                 (OS-BUG-LASTSECTOR, docs/hw/tracker-hardware-notes.md section 3). The cause is an open research question; the leading
                 hypothesis is that the SD bootloader mishandles an image that fills the last sector (an end-address bound, or the
                 sector reserved). `--allow-last-sector` waives the check (only the CI build of a bench image, which is not for the unit)
  initial_sp     the word at 0 is 8-byte aligned and inside internal SRAM (0x1FFF0000..0x20030000), and, with --expect-sp,
                 exactly that
  reset_vector   the word at 4 is a Thumb address (bit 0 set) that points at populated flash, past the flash configuration
  vector_table   every other entry of the table is either an unused (reserved) slot holding 0 or a Thumb address into
                 populated flash; the reserved slots are exactly the Cortex-M4's (7-10 and 13). With --ramfunc-l BASE:SIZE
                 (the SRAM_L code region that `.ramfunc_l` is copied into at reset, OS-005a: the audio ISRs run from there) an
                 entry may instead be a Thumb address inside that region whose offset from BASE lies within the image (the load
                 image of the hot code is part of it); anything else in SRAM, the module slab above the region included, is
                 refused. The reset vector (entry 1) must still be in flash.
  flash_config   bytes 0x400..0x40F read back exactly `ffffffffffffffffffffffffdef9ffff`. FSEC (0x40C) = 0xDE leaves the MCU
                 unsecured; a wrong value can permanently lock it, so any difference is a failure

With --delete-on-fail a failing HEX is removed, so no image that failed its gate is left lying around to be flashed.

`noinit` (OS-GATE-BOOTCRASH) is the one check that reads the ELF, not the HEX: the symbols of the `.noinit` output section (what survives a
reset: the crash block first, the power record) must be exactly the layout pinned in noinit_layout.txt: name, address and size of every
object, and the section's two ends. An object that moves, grows, shrinks, appears or goes is a FAIL until the manifest is changed on purpose
(`--write` regenerates it from the ELF; the commit must then say what an older image's record at the old place does to the new image).
It needs the toolchain's `nm` (`--nm`), so it runs where the image is built, never on a bare HEX.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

FLASH_LAST = 0x000FFFFF
FLASH_SECTOR = 4096    # mem_map.h kFlashSectorSize
IMAGE_END_LIMIT = 0xFF000  # OS-BUG-LASTSECTOR: an image ends at or below this; the last flash sector (0xFF000..0xFFFFF) stays empty
SRAM_FIRST = 0x1FFF0000
SRAM_END = 0x20030000  # one past the last byte; also the legal initial SP of an image that uses all of SRAM
CONFIG_ADDRESS = 0x400
CONFIG_SIZE = 16
# The gate's value (the flashing-safety rules, skill fw-gate-check): 12 erased bytes, FSEC 0xDE, FOPT 0xF9, FEPROT/FDPROT erased.
REQUIRED_CONFIG_HEX = "ffffffffffffffffffffffffdef9ffff"
RESERVED_VECTORS = (7, 8, 9, 10, 13)  # Cortex-M4: reserved exception slots
DEFAULT_VECTOR_COUNT = 16 + 100       # 16 system vectors and the MK66's 100 device interrupts


class ImageError(Exception):
    pass


def parse_ihex(text):
    """Strict Intel HEX parser: returns {address: byte}. Raises ImageError on anything that is not a clean image."""
    memory = {}
    base = 0
    eof_seen = False
    lines = text.split("\n")
    for number, raw_line in enumerate(lines, 1):
        line = raw_line.rstrip("\r")
        if not line:
            continue
        if eof_seen:
            raise ImageError("line %d: data after the EOF record" % number)
        if not line.startswith(":"):
            raise ImageError("line %d: does not begin with ':'" % number)
        try:
            record = bytes.fromhex(line[1:])
        except ValueError:
            raise ImageError("line %d: not hexadecimal" % number)
        if len(record) < 5:
            raise ImageError("line %d: shorter than a record header" % number)
        count, offset, kind = record[0], (record[1] << 8) | record[2], record[3]
        if len(record) != count + 5:
            raise ImageError("line %d: byte count %d but %d data bytes" % (number, count, len(record) - 5))
        if sum(record) & 0xFF:
            raise ImageError("line %d: checksum mismatch" % number)
        data = record[4:-1]
        if kind == 0x00:
            for i, byte in enumerate(data):
                address = base + offset + i
                if address >= 1 << 32:
                    raise ImageError("line %d: address beyond 32 bits" % number)
                if address in memory:
                    raise ImageError("line %d: address 0x%08X given twice" % (number, address))
                memory[address] = byte
        elif kind == 0x01:
            if count != 0:
                raise ImageError("line %d: EOF record with data" % number)
            eof_seen = True
        elif kind == 0x02:
            if count != 2:
                raise ImageError("line %d: extended segment address record must hold 2 bytes" % number)
            base = ((data[0] << 8) | data[1]) << 4
        elif kind == 0x04:
            if count != 2:
                raise ImageError("line %d: extended linear address record must hold 2 bytes" % number)
            base = ((data[0] << 8) | data[1]) << 16
        elif kind in (0x03, 0x05):
            # objcopy writes one when the ELF has an entry point. The repository's own HEX tool
            # (tools/o3ihex.py) refuses it, and nothing shows that the bootloader's loader accepts it: not in an image.
            raise ImageError("line %d: start-address record (type 0x%02X): the bootloader's "
                             "handling of one is unknown; build with objcopy --set-start=0" % (number, kind))
        else:
            raise ImageError("line %d: unknown record type 0x%02X" % (number, kind))
    if not eof_seen:
        raise ImageError("no EOF record")
    if not memory:
        raise ImageError("the image holds no data")
    return memory


def word(memory, address):
    """The little-endian word at `address`, or None if any of its four bytes is not populated."""
    try:
        return sum(memory[address + i] << (8 * i) for i in range(4))
    except KeyError:
        return None


def check_image(memory, expect_sp=None, vector_count=DEFAULT_VECTOR_COUNT, required_config=REQUIRED_CONFIG_HEX, ramfunc_l=None,
                allow_last_sector=False):
    """Runs the checks; returns [(name, ok, detail)]. `memory` comes from parse_ihex."""
    results = []

    def add(name, ok, detail):
        results.append((name, bool(ok), detail))

    low, high = min(memory), max(memory)
    add("address_range", high <= FLASH_LAST,
        "populated 0x%08X..0x%08X, limit 0x%08X" % (low, high, FLASH_LAST))

    missing = [a for a in range(0, CONFIG_ADDRESS + CONFIG_SIZE) if a not in memory]
    add("head_present", not missing,
        "0x0..0x40F populated" if not missing else "%d byte(s) missing from 0x0..0x40F, first at 0x%X" % (len(missing), missing[0]))

    # One run from 0 to the last byte. Found from the sorted addresses (an image may be far larger than flash if it is wrong).
    missing_bytes = high + 1 - len(memory)
    first_hole = None
    if low != 0:
        first_hole = 0
    else:
        ordered = sorted(memory)
        for previous, current in zip(ordered, ordered[1:]):
            if current != previous + 1:
                first_hole = previous + 1
                break
    if first_hole is not None:
        add("contiguous", False, "%d byte(s) missing between 0x0 and 0x%08X, the first at 0x%08X" % (missing_bytes, high, first_hole))
    else:
        add("contiguous", True, "one run, 0x0..0x%08X" % high)

    # The image must end on a flash sector boundary (OS-BUG-COSMBOOT).
    tail = (high + 1) % FLASH_SECTOR
    if tail == 0:
        add("sector_end", True, "image end 0x%08X is a multiple of %d" % (high + 1, FLASH_SECTOR))
    else:
        add("sector_end", False, "image end 0x%08X is not a multiple of %d: the last sector holds only %d byte(s), and the SD bootloader "
            "leaves a short final sector neither erased nor programmed (OS-BUG-COSMBOOT); pad the image to the sector end"
            % (high + 1, FLASH_SECTOR, tail))

    # The last flash sector must stay free (OS-BUG-LASTSECTOR, observed on the unit 2026-11-08).
    end = high + 1
    if end <= IMAGE_END_LIMIT:
        add("last_sector_free", True, "image end 0x%08X <= 0x%08X" % (end, IMAGE_END_LIMIT))
    elif allow_last_sector:
        add("last_sector_free", True, "WAIVED (--allow-last-sector): image end 0x%08X is %d byte(s) over 0x%08X; do not install this image on a unit "
            "(OS-BUG-LASTSECTOR)" % (end, end - IMAGE_END_LIMIT, IMAGE_END_LIMIT))
    else:
        add("last_sector_free", False, "image end 0x%08X is %d byte(s) over 0x%08X: it reaches the last flash sector (0xFF000). The unit hung after "
            "the update with b5-bench-417fbe4a (end 0x100000) and never came back on USB; b4-bench-7e2567d0 (end 0xFF000) booted "
            "(OS-BUG-LASTSECTOR, open research question: leading hypothesis, the SD bootloader mishandles an image that fills the last sector). "
            "Shrink the image (flash diet, O3OS_BENCH=OFF)" % (end, end - IMAGE_END_LIMIT, IMAGE_END_LIMIT))

    def in_image(address):
        return address in memory

    sp = word(memory, 0)
    if sp is None:
        add("initial_sp", False, "the word at 0 is not populated")
    else:
        ok = SRAM_FIRST <= sp <= SRAM_END and sp % 8 == 0
        if expect_sp is not None:
            ok = ok and sp == expect_sp
        add("initial_sp", ok, "0x%08X (SRAM 0x%08X..0x%08X%s)" % (
            sp, SRAM_FIRST, SRAM_END, ", expected 0x%08X" % expect_sp if expect_sp is not None else ""))

    reset = word(memory, 4)
    if reset is None:
        add("reset_vector", False, "the word at 4 is not populated")
    else:
        target = reset & ~1
        ok = (reset & 1) == 1 and target >= CONFIG_ADDRESS + CONFIG_SIZE and target <= FLASH_LAST and in_image(target)
        add("reset_vector", ok, "0x%08X (%s, %s)" % (
            reset, "Thumb" if reset & 1 else "NOT Thumb",
            "in the image past the flash configuration" if ok or in_image(target) else "NOT inside the populated image"))

    problems = []
    ramfunc_vectors = 0
    for index in range(2, vector_count):
        entry = word(memory, index * 4)
        if entry is None:
            problems.append("vector %d is not populated" % index)
            continue
        if index in RESERVED_VECTORS:
            if entry != 0:
                problems.append("reserved vector %d is 0x%08X, not 0" % (index, entry))
            continue
        target = entry & ~1
        in_ramfunc = ramfunc_l is not None and ramfunc_l[0] <= target < ramfunc_l[0] + ramfunc_l[1]
        if (entry & 1) == 0:
            problems.append("vector %d = 0x%08X is not a Thumb address" % (index, entry))
        elif in_ramfunc:
            ramfunc_vectors += 1
            if target - ramfunc_l[0] > high:
                problems.append("vector %d = 0x%08X is in the SRAM_L code region but beyond the image's own length" % (index, entry))
        elif not (target >= CONFIG_ADDRESS + CONFIG_SIZE and in_image(target)):
            problems.append("vector %d = 0x%08X does not point into the populated image" % (index, entry))
    detail = "%d entries checked" % vector_count
    if ramfunc_vectors:
        detail += " (%d in the SRAM_L code region, copied from flash at reset)" % ramfunc_vectors
    add("vector_table", not problems, detail if not problems else "; ".join(problems[:6]) + (" ..." if len(problems) > 6 else ""))

    config = [memory.get(CONFIG_ADDRESS + i) for i in range(CONFIG_SIZE)]
    if None in config:
        add("flash_config", False, "bytes 0x400..0x40F are not all populated")
    else:
        got = bytes(config).hex()
        fsec = config[12]
        add("flash_config", got == required_config and (fsec & 3) == 2,
            "read back %s (FSEC 0x%02X, SEC=%d, must be 2 = unsecure); required %s" % (got, fsec, fsec & 3, required_config))
    return results


def describe(memory):
    blob = bytes(memory.get(a, 0xFF) for a in range(0, max(memory) + 1))
    return {
        "first_address": min(memory), "end_address_inclusive": max(memory), "populated_bytes": len(memory),
        "sha256_of_contiguous_image": hashlib.sha256(blob).hexdigest(),
        "flash_config_hex": bytes(memory.get(CONFIG_ADDRESS + i, 0xFF) for i in range(CONFIG_SIZE)).hex(),
        "initial_sp": word(memory, 0), "reset_vector": word(memory, 4),
    }


def run_check(path, expect_sp, vectors, json_out, delete_on_fail, ramfunc_l=None, allow_last_sector=False):
    text = Path(path).read_text(encoding="ascii", errors="strict")
    try:
        memory = parse_ihex(text)
    except ImageError as exc:
        results = [("hex_parse", False, str(exc))]
        memory = None
    else:
        results = [("hex_parse", True, "%d bytes in %s" % (len(memory), path))] + check_image(memory, expect_sp, vectors, ramfunc_l=ramfunc_l, allow_last_sector=allow_last_sector)
    failed = [r for r in results if not r[1]]
    for name, ok, detail in results:
        print("%s %-14s %s" % ("PASS" if ok else "FAIL", name, detail))
    if memory is not None:
        info = describe(memory)
        print("image: 0x%08X..0x%08X, %d bytes, sha256 %s" % (
            info["first_address"], info["end_address_inclusive"], info["populated_bytes"], info["sha256_of_contiguous_image"]))
        if json_out:
            Path(json_out).write_text(json.dumps({"checks": [{"name": n, "ok": ok, "detail": d} for n, ok, d in results], "image": info}, indent=2) + "\n")
    if failed:
        print("check_image: %d check(s) FAILED" % len(failed))
        if delete_on_fail:
            try:
                os.remove(path)
                print("check_image: removed %s" % path)
            except OSError as exc:
                print("check_image: could not remove %s: %s" % (path, exc))
        return 1
    print("check_image: all %d checks passed (nothing was flashed or handed over)" % len(results))
    return 0


def run_flashconfig(path, expect):
    expect = expect.lower()
    if len(expect) != 2 * CONFIG_SIZE or any(c not in "0123456789abcdef" for c in expect):
        print("check_image: --expect must be 32 hex digits")
        return 2
    try:
        memory = parse_ihex(Path(path).read_text(encoding="ascii", errors="strict"))
    except ImageError as exc:
        print("FAIL flashconfig: %s" % exc)
        return 1
    config = [memory.get(CONFIG_ADDRESS + i) for i in range(CONFIG_SIZE)]
    if None in config:
        print("FAIL flashconfig: bytes 0x400..0x40F are not all present in %s" % path)
        return 1
    got = bytes(config).hex()
    print("flash configuration field 0x400..0x40F read back: %s" % got)
    print("required:                                         %s" % expect)
    if got != expect:
        print("FAIL flashconfig: differs; this image could lock the MCU")
        return 1
    print("PASS flashconfig: exactly the required bytes")
    return 0


# ---- noinit layout: the symbols of `.noinit` against a pinned manifest -------------------------------------------------
NOINIT_START_SYMBOL = "__noinit_start"
NOINIT_END_SYMBOL = "__noinit_end"
NOINIT_FIRST_ADDRESS = SRAM_FIRST + 0x10000  # 0x20000000: the crash block is the first thing at the base of SRAM_U (mk66.ld.in)


def parse_nm(text):
    """(symbols, sized): `nm -S -n` output as {name: address} for every symbol and [(address, size, name)] for the sized ones."""
    symbols, sized = {}, []
    for line in text.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 3:
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        if len(parts[1]) == 1:  # no size: address, type, name
            symbols[line.split(None, 2)[2]] = address
        elif len(parts) == 4:   # address, size, type, name
            try:
                size = int(parts[1], 16)
            except ValueError:
                continue
            sized.append((address, size, parts[3]))
            symbols[parts[3]] = address
    return symbols, sized


def noinit_layout(nm_text):
    """The layout of the image the `nm -S -n` text describes: {start, end, objects: [(address, size, name)]}. Raises ImageError when the
    two section symbols are missing or the objects are not in a clean layout (overlap, outside the section, a duplicate name)."""
    symbols, sized = parse_nm(nm_text)
    for name in (NOINIT_START_SYMBOL, NOINIT_END_SYMBOL):
        if name not in symbols:
            raise ImageError("%s is not in the symbol table (a stripped ELF, or the wrong file)" % name)
    start, end = symbols[NOINIT_START_SYMBOL], symbols[NOINIT_END_SYMBOL]
    if end < start:
        raise ImageError("%s 0x%08X is below %s 0x%08X" % (NOINIT_END_SYMBOL, end, NOINIT_START_SYMBOL, start))
    objects = sorted(o for o in sized if start <= o[0] < end and o[1] > 0)
    names = [o[2] for o in objects]
    for name in sorted(set(n for n in names if names.count(n) > 1)):
        raise ImageError("two objects in .noinit are named '%s'" % name)
    previous_end = start
    for address, size, name in objects:
        if address < previous_end:
            raise ImageError("'%s' at 0x%08X overlaps the object before it" % (name, address))
        if address + size > end:
            raise ImageError("'%s' (0x%08X, %d bytes) runs past %s" % (name, address, size, NOINIT_END_SYMBOL))
        previous_end = address + size
    return {"start": start, "end": end, "objects": objects}


def format_noinit_manifest(layout):
    lines = ["# The pinned layout of .noinit (what survives a reset). Checked by `check_image.py noinit` (OS-GATE-BOOTCRASH).",
             "# A change here is a change of what an older image leaves in RAM for the next one: say in the commit what it does to",
             "# the crash record and the power record. Regenerate with `check_image.py noinit ELF --nm NM --expect THIS-FILE --write`.",
             "start 0x%08X" % layout["start"], "end   0x%08X" % layout["end"]]
    lines += ["object 0x%08X 0x%04X %s" % o for o in layout["objects"]]
    return "\n".join(lines) + "\n"


def parse_noinit_manifest(text):
    layout = {"start": None, "end": None, "objects": []}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 3)
        try:
            if parts[0] in ("start", "end") and len(parts) == 2:
                layout[parts[0]] = int(parts[1], 0)
            elif parts[0] == "object" and len(parts) == 4:
                layout["objects"].append((int(parts[1], 0), int(parts[2], 0), parts[3]))
            else:
                raise ValueError
        except (ValueError, IndexError):
            raise ImageError("manifest line %d is not 'start A', 'end A' or 'object ADDR SIZE NAME': %s" % (number, raw))
    if layout["start"] is None or layout["end"] is None or not layout["objects"]:
        raise ImageError("manifest has no start, no end or no object (an empty manifest would pass anything)")
    layout["objects"].sort()
    return layout


def noinit_differences(actual, pinned):
    """What moved: a list of sentences, empty when the layout is the pinned one."""
    found = []
    if actual["start"] != NOINIT_FIRST_ADDRESS:
        found.append("%s is 0x%08X, not 0x%08X: the crash block is no longer at the base of SRAM_U" % (NOINIT_START_SYMBOL, actual["start"], NOINIT_FIRST_ADDRESS))
    for key, label in (("start", NOINIT_START_SYMBOL), ("end", NOINIT_END_SYMBOL)):
        if actual[key] != pinned[key]:
            found.append("%s moved: 0x%08X, pinned 0x%08X" % (label, actual[key], pinned[key]))
    have = {name: (address, size) for address, size, name in actual["objects"]}
    want = {name: (address, size) for address, size, name in pinned["objects"]}
    for name in sorted(want):
        if name not in have:
            found.append("object gone: %s (pinned at 0x%08X, %d bytes)" % (name, want[name][0], want[name][1]))
        elif have[name][0] != want[name][0]:
            found.append("object moved: %s is at 0x%08X, pinned 0x%08X" % (name, have[name][0], want[name][0]))
        elif have[name][1] != want[name][1]:
            found.append("object resized: %s is %d bytes, pinned %d" % (name, have[name][1], want[name][1]))
    for name in sorted(have):
        if name not in want:
            found.append("object new: %s at 0x%08X, %d bytes" % (name, have[name][0], have[name][1]))
    return found


def run_noinit(elf, nm, expect, write):
    try:
        text = subprocess.run([nm, "-S", "-n", "--demangle", elf], capture_output=True, text=True, check=True).stdout
        actual = noinit_layout(text)
        if write:
            Path(expect).write_text(format_noinit_manifest(actual), encoding="ascii")
            print("noinit: wrote %s (%d objects, 0x%08X..0x%08X)" % (expect, len(actual["objects"]), actual["start"], actual["end"]))
            return 0
        pinned = parse_noinit_manifest(Path(expect).read_text(encoding="ascii"))
    except (OSError, UnicodeError, subprocess.CalledProcessError) as exc:
        print("FAIL noinit: %s" % exc)
        return 1
    except ImageError as exc:
        print("FAIL noinit: %s" % exc)
        return 1
    found = noinit_differences(actual, pinned)
    for line in found:
        print("FAIL noinit: " + line)
    if found:
        print("check_image: the .noinit layout is not the pinned one (%s); if the change is meant, regenerate the manifest with --write and say in the commit what an older image's records do to this one" % expect)
        return 1
    print("PASS noinit: %d objects in 0x%08X..0x%08X, exactly the pinned layout" % (len(actual["objects"]), actual["start"], actual["end"]))
    return 0


# ---- self-test: the checker must accept a clean image and reject every kind of break ---------------------------------
def ihex_record(kind, offset, data):
    body = bytes([len(data), (offset >> 8) & 0xFF, offset & 0xFF, kind]) + bytes(data)
    return ":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def make_hex(segments, eof=True, extra=()):
    """segments: {address: bytes}. Writes an extended linear address record whenever the upper 16 bits change."""
    lines = []
    upper = None
    for address in sorted(segments):
        data = segments[address]
        for chunk in range(0, len(data), 16):
            absolute = address + chunk
            if (absolute >> 16) != upper:
                upper = absolute >> 16
                lines.append(ihex_record(0x04, 0, [(upper >> 8) & 0xFF, upper & 0xFF]))
            lines.append(ihex_record(0x00, absolute & 0xFFFF, data[chunk:chunk + 16]))
    lines.extend(extra)
    if eof:
        lines.append(ihex_record(0x01, 0, b""))
    return "\n".join(lines) + "\n"


def good_segments():
    def w(value):
        return value.to_bytes(4, "little")
    vectors = bytearray()
    vectors += w(0x20030000)       # initial SP
    vectors += w(0x00000501)       # reset
    for index in range(2, DEFAULT_VECTOR_COUNT):
        vectors += w(0 if index in RESERVED_VECTORS else 0x00000511)
    vectors += b"\xFF" * (CONFIG_ADDRESS - len(vectors))
    config = bytes.fromhex(REQUIRED_CONFIG_HEX)
    # The code runs to the end of the first 4 KiB flash sector (the image must end on a sector boundary: `sector_end`).
    code = (bytes(range(256)) * 16)[:FLASH_SECTOR - CONFIG_ADDRESS - CONFIG_SIZE]
    return {0: bytes(vectors), CONFIG_ADDRESS: config, CONFIG_ADDRESS + CONFIG_SIZE: code}


def self_test():
    failures = []
    cases = [0]

    def verdicts(segments=None, **kwargs):
        memory = parse_ihex(make_hex(segments if segments is not None else good_segments()))
        return {name: ok for name, ok, _ in check_image(memory, **kwargs)}

    def expect_ok(label, results):
        cases[0] += 1
        bad = [n for n, ok in results.items() if not ok]
        if bad:
            failures.append("%s: unexpectedly failed %s" % (label, bad))

    def expect_fail(label, results, check):
        cases[0] += 1
        if results.get(check, True):
            failures.append("%s: %s did not fail" % (label, check))

    def parse_fails(label, text):
        cases[0] += 1
        try:
            parse_ihex(text)
        except ImageError:
            return
        failures.append("%s: parse accepted it" % label)

    def modified(address, value):
        segments = good_segments()
        # Rebuild the image around one changed byte.
        flat = {}
        for base, blob in segments.items():
            for i, b in enumerate(blob):
                flat[base + i] = b
        flat[address] = value
        return {a: bytes([b]) for a, b in flat.items()}

    expect_ok("good image", verdicts(expect_sp=0x20030000))

    # Parser.
    good = make_hex(good_segments())
    corrupt = good.split("\n")
    corrupt[1] = corrupt[1][:-2] + ("00" if corrupt[1][-2:] != "00" else "01")  # line 0 is the address record, 1 the first data
    parse_fails("bad checksum", "\n".join(corrupt))
    parse_fails("no EOF record", make_hex(good_segments(), eof=False))
    parse_fails("data after EOF", good + ihex_record(0x00, 0, [1]) + "\n")
    parse_fails("unknown record type", make_hex(good_segments(), extra=[ihex_record(0x09, 0, [0])]))
    parse_fails("overlapping records", make_hex({0: b"\x01\x02\x03\x04", 2: b"\x09"}))
    parse_fails("not hex", ":zz\n" + ihex_record(0x01, 0, b""))
    parse_fails("missing colon", "00000001FF\n")
    parse_fails("short count", ":0400000001FB\n" + ihex_record(0x01, 0, b""))
    parse_fails("empty file", "")
    parse_fails("EOF only", ihex_record(0x01, 0, b"") + "\n")
    parse_fails("start segment address record", make_hex(good_segments(), extra=[ihex_record(0x03, 0, [0, 0, 4, 0x2D])]))
    parse_fails("start linear address record", make_hex(good_segments(), extra=[ihex_record(0x05, 0, [0, 0, 4, 0x2D])]))
    segment_form = make_hex({}, extra=[ihex_record(0x02, 0, [0x10, 0x00]), ihex_record(0x00, 0, [1, 2])])
    try:
        if parse_ihex(segment_form) != {0x10000: 1, 0x10001: 2}:
            failures.append("extended segment address record misread")
    except ImageError as exc:
        failures.append("extended segment address record rejected: %s" % exc)

    # Address range: one byte past 1 MiB is a failure.
    segments = good_segments()
    segments[0x00100000] = b"\x00"
    expect_fail("address 0x100000", verdicts(segments), "address_range")
    segments = good_segments()
    segments[0x00100000 - 1] = b"\x00"
    legal = verdicts(segments)
    cases[0] += 1
    if not legal["address_range"]:
        failures.append("address 0xFFFFF is the last legal one: address_range failed")
    expect_fail("a lone byte at 0xFFFFF leaves a hole", legal, "contiguous")

    # Contiguity: the bootloader's tolerance of a hole is unknown. A hole anywhere fails:
    # inside the code, between the head and the code, right at the end of the vector table, at the flash configuration, one
    # byte, four bytes (the alignment gap this check was added for), and an image that does not start at 0.
    code_end = CONFIG_ADDRESS + CONFIG_SIZE + len(good_segments()[CONFIG_ADDRESS + CONFIG_SIZE])
    for label, hole_at, hole_len in (("inside the code", 0x500, 1), ("four bytes inside the code", 0x500, 4),
                                     ("just past the flash configuration", CONFIG_ADDRESS + CONFIG_SIZE, 1),
                                     ("at the end of the vector table", DEFAULT_VECTOR_COUNT * 4 - 1, 1),
                                     ("the second-to-last byte of the code", code_end - 2, 1)):
        flat = {}
        for base, blob in good_segments().items():
            for i, b in enumerate(blob):
                flat[base + i] = b
        for a in range(hole_at, hole_at + hole_len):
            del flat[a]
        verdict = verdicts({a: bytes([b]) for a, b in flat.items()})
        expect_fail("hole %s" % label, verdict, "contiguous")
    # A hole after the code's end is not a hole: it is a longer image. A gap between two data records that is filled passes.
    segments = good_segments()
    segments[code_end + 4] = b"\xAA" * 8   # leaves code_end..code_end+3 empty: a hole
    expect_fail("hole between the code and a later record", verdicts(segments), "contiguous")
    segments = good_segments()
    segments[code_end] = b"\xFF" * 4 + b"\xAA" * (FLASH_SECTOR - 4)   # the same gap, filled, the image ending on the next sector boundary
    expect_ok("a filled gap is no hole", verdicts(segments))
    expect_fail("an image that does not start at 0", verdicts({a + 4: b for a, b in good_segments().items()}), "contiguous")

    # Sector end (OS-BUG-COSMBOOT): the good image ends on a boundary; a short last sector, even by one byte, fails, and so does
    # one byte more than a boundary; a second full sector passes.
    expect_ok("image ends on a sector boundary", verdicts())
    for label, extra in (("one byte into a new sector", 1), ("124 bytes into a new sector (the cosm.1 tail)", 124), ("one byte short of a boundary", FLASH_SECTOR - 1)):
        segments = good_segments()
        segments[code_end] = b"\xFF" * extra
        expect_fail("short tail: %s" % label, verdicts(segments), "sector_end")
    segments = good_segments()
    segments[code_end] = b"\xFF" * FLASH_SECTOR
    expect_ok("two full sectors", verdicts(segments))
    segments = good_segments()
    segments[CONFIG_ADDRESS + CONFIG_SIZE] = segments[CONFIG_ADDRESS + CONFIG_SIZE][:-1]
    expect_fail("short tail: the last byte of the image removed", verdicts(segments), "sector_end")

    # Last sector free (OS-BUG-LASTSECTOR): an image ending at 0xFF000 passes, one ending at 0x100000 (the last sector filled) fails;
    # --allow-last-sector waives it, and only it.
    def filled_to(end):
        segments = good_segments()
        start = CONFIG_ADDRESS + CONFIG_SIZE
        segments[start] = segments[start] + b"\x5A" * (end - start - len(segments[start]))
        return segments
    expect_ok("image ends at 0xFF000", verdicts(filled_to(IMAGE_END_LIMIT)))
    expect_ok("image ends one sector below 0xFF000", verdicts(filled_to(IMAGE_END_LIMIT - FLASH_SECTOR)))
    expect_fail("image ends at 0x100000", verdicts(filled_to(0x100000)), "last_sector_free")
    cases[0] += 1
    waived = verdicts(filled_to(0x100000), allow_last_sector=True)
    if not waived["last_sector_free"] or not all(waived.values()):
        failures.append("--allow-last-sector did not waive last_sector_free for an image ending at 0x100000: %s" % waived)
    expect_fail("the waiver does not hide a byte past flash", verdicts({**filled_to(0x100000), 0x100000: b"\x00"}, allow_last_sector=True), "address_range")

    # Head and initial SP.
    segments = good_segments()
    del segments[CONFIG_ADDRESS + CONFIG_SIZE]
    expect_fail("an image that is only the head has no code to reset into", verdicts(segments), "reset_vector")
    for label, sp in (("zero", 0), ("flash", 0x00001000), ("below SRAM", 0x1FFEFFF8), ("past SRAM", 0x20030008), ("unaligned", 0x20030004), ("peripheral", 0x40000000)):
        sp_bytes = sp.to_bytes(4, "little")
        segments = good_segments()
        segments[0] = sp_bytes + segments[0][4:]
        expect_fail("initial SP %s" % label, verdicts(segments), "initial_sp")
    segments = good_segments()
    segments[0] = (0x2002FFF8).to_bytes(4, "little") + segments[0][4:]
    expect_ok("SP inside SRAM", verdicts(segments))
    expect_fail("SP not the expected one", verdicts(segments, expect_sp=0x20030000), "initial_sp")

    # Reset vector.
    for label, value in (("even", 0x00000500), ("outside the image", 0x00200001), ("in the vector table", 0x00000101), ("in the config field", 0x00000405),
                         ("at the unpopulated gap", 0x000FF001), ("zero", 0)):
        segments = good_segments()
        segments[0] = segments[0][:4] + value.to_bytes(4, "little") + segments[0][8:]
        expect_fail("reset vector %s" % label, verdicts(segments), "reset_vector")

    # Vector table.
    for index in (2, 3, 15, 16, 60, 115):
        for label, value in (("even", 0x00000510), ("zero", 0), ("outside", 0x00300001), ("in the table", 0x00000041)):
            segments = good_segments()
            vectors = bytearray(segments[0])
            vectors[index * 4:index * 4 + 4] = value.to_bytes(4, "little")
            segments[0] = bytes(vectors)
            expect_fail("vector %d %s" % (index, label), verdicts(segments), "vector_table")
    for index in RESERVED_VECTORS:
        segments = good_segments()
        vectors = bytearray(segments[0])
        vectors[index * 4:index * 4 + 4] = (0x00000511).to_bytes(4, "little")
        segments[0] = bytes(vectors)
        expect_fail("reserved vector %d filled" % index, verdicts(segments), "vector_table")
    # Vectors into the SRAM_L code region (OS-005a: the audio ISRs run from SRAM_L): only with --ramfunc-l, only inside the region,
    # only as Thumb addresses, never as the reset vector, never into SRAM_U or the module slab above the region.
    region = (0x1FFF0000, 0xC000)
    for index in (16 + 15, 16 + 64, 115):
        segments = good_segments()
        vectors = bytearray(segments[0])
        vectors[index * 4:index * 4 + 4] = (0x1FFF0101).to_bytes(4, "little")
        segments[0] = bytes(vectors)
        expect_ok("vector %d in SRAM_L with the region given" % index, verdicts(segments, ramfunc_l=region))
        expect_fail("vector %d in SRAM_L without the region" % index, verdicts(segments), "vector_table")
        for label, value in (("even", 0x1FFF0100), ("module slab", 0x1FFFC001), ("just past the region", 0x1FFFC000 | 1),
                             ("SRAM_U", 0x20000101), ("beyond the image's length", 0x1FFF8001), ("below SRAM_L", 0x1FFEFFFF)):
            segments = good_segments()
            vectors = bytearray(segments[0])
            vectors[index * 4:index * 4 + 4] = value.to_bytes(4, "little")
            segments[0] = bytes(vectors)
            expect_fail("vector %d %s with the region given" % (index, label), verdicts(segments, ramfunc_l=region), "vector_table")
    segments = good_segments()
    segments[0] = segments[0][:4] + (0x1FFF0101).to_bytes(4, "little") + segments[0][8:]
    expect_fail("reset vector in SRAM_L with the region given", verdicts(segments, ramfunc_l=region), "reset_vector")
    # The last table entry's upper half in a record of its own: words are read across record boundaries.
    segments = good_segments()
    tail = segments[0][DEFAULT_VECTOR_COUNT * 4:]
    segments[0] = segments[0][:DEFAULT_VECTOR_COUNT * 4 - 2]
    segments[DEFAULT_VECTOR_COUNT * 4 - 2] = b"\x00\x00"
    segments[DEFAULT_VECTOR_COUNT * 4] = tail
    expect_ok("table split across records still reads", verdicts(segments))

    # Flash configuration: every one of the 16 bytes, flipped one bit at a time and set to each of the other tempting
    # values, must fail. FSEC 0xDC is a "locked" variant.
    required = bytes.fromhex(REQUIRED_CONFIG_HEX)
    for position in range(CONFIG_SIZE):
        for label, value in (("bit flip", required[position] ^ 0x01), ("inverted", required[position] ^ 0xFF), ("zero", 0x00), ("erased", 0xFF)):
            if value == required[position]:
                continue
            expect_fail("flash config byte %d %s" % (position, label), verdicts(modified(CONFIG_ADDRESS + position, value)), "flash_config")
    expect_fail("FSEC 0xDC (secured)", verdicts(modified(CONFIG_ADDRESS + 12, 0xDC)), "flash_config")
    expect_fail("FSEC 0xFE", verdicts(modified(CONFIG_ADDRESS + 12, 0xFE)), "flash_config")
    segments = good_segments()
    del segments[CONFIG_ADDRESS]
    expect_fail("config field absent", verdicts(segments), "flash_config")
    segments = good_segments()
    segments[CONFIG_ADDRESS] = required[:15]
    expect_fail("config field one byte short", verdicts(segments), "flash_config")
    expect_fail("head hole", verdicts(segments), "head_present")

    # The .noinit layout: a synthetic `nm -S -n --demangle` of a clean image, and every way the layout can move.
    block = "o3os::platform::mk66::(anonymous namespace)::g_block"
    record = "o3os::platform::mk66::power::device::(anonymous namespace)::g_rec"
    nm_ok = ("20000000 B __noinit_start\n20000000 00000080 b %s\n20000080 0000001c b %s\n200000a0 B __noinit_end\n"
             "200002a0 B __fault_stack_top\n20000400 00000400 b o3os::g_rx\n") % (block, record)
    pinned = parse_noinit_manifest(format_noinit_manifest(noinit_layout(nm_ok)))

    def noinit_found(nm_text):
        return noinit_differences(noinit_layout(nm_text), pinned)

    def noinit_expect(label, nm_text, fragment):
        cases[0] += 1
        try:
            found = noinit_found(nm_text)
        except ImageError as exc:
            found = [str(exc)]
        if not any(fragment in f for f in found):
            failures.append("noinit %s: no finding with '%s' (got %s)" % (label, fragment, found))

    cases[0] += 1
    if noinit_found(nm_ok):
        failures.append("noinit: the clean layout was refused: %s" % noinit_found(nm_ok))
    cases[0] += 1
    if len(noinit_layout(nm_ok)["objects"]) != 2:
        failures.append("noinit: an object outside the section (g_rx) was counted, or one inside was missed")
    grown = nm_ok.replace("200000a0 B __noinit_end", "200000a8 B __noinit_end")  # (a move inside the section moves its end too)
    noinit_expect("record moved", grown.replace("20000080 0000001c b", "20000088 0000001c b"), "object moved")
    noinit_expect("record moved, end not", nm_ok.replace("20000080 0000001c b", "20000088 0000001c b"), "runs past")
    noinit_expect("record resized", nm_ok.replace("20000080 0000001c b", "20000080 00000020 b"), "object resized")
    noinit_expect("record gone", nm_ok.replace("20000080 0000001c b %s\n" % record, ""), "object gone")
    noinit_expect("object new in front", grown.replace("20000080 0000001c b %s" % record,
                  "20000080 00000008 b o3os::g_new\n20000088 0000001c b %s" % record), "object new")
    noinit_expect("crash block moved", grown.replace("20000000 B __noinit_start", "20000008 B __noinit_start")
                  .replace("20000000 00000080 b", "20000008 00000080 b").replace("20000080 0000001c b", "20000088 0000001c b"),
                  "no longer at the base of SRAM_U")
    noinit_expect("section grew", nm_ok.replace("200000a0 B __noinit_end", "200000b0 B __noinit_end"), "__noinit_end moved")
    noinit_expect("no section symbols", "20000000 00000080 b %s\n" % block, "is not in the symbol table")
    noinit_expect("overlap", nm_ok.replace("20000080 0000001c b", "20000070 0000001c b"), "overlaps")
    noinit_expect("duplicate name", nm_ok.replace(record, block), "two objects")
    for label, text in (("empty manifest", "# nothing\n"), ("a line that is nothing", "start 0x20000000\nend 0x200000a0\nobject 1\n"),
                        ("no object", "start 0x20000000\nend 0x200000a0\n")):
        cases[0] += 1
        try:
            parse_noinit_manifest(text)
            failures.append("noinit manifest: accepted %s" % label)
        except ImageError:
            pass

    # The CLI paths, on disk.
    quiet = io.StringIO()  # the CLI paths print their verdicts; the self-test only wants the exit codes
    with contextlib.redirect_stdout(quiet), tempfile.TemporaryDirectory() as tmp:
        hex_path = Path(tmp) / "ok.hex"
        hex_path.write_text(make_hex(good_segments()), encoding="ascii")
        if run_check(str(hex_path), 0x20030000, DEFAULT_VECTOR_COUNT, str(Path(tmp) / "ok.json"), False) != 0:
            failures.append("CLI: a good image failed")
        if run_flashconfig(str(hex_path), REQUIRED_CONFIG_HEX) != 0:
            failures.append("CLI: flashconfig failed on a good image")
        if run_flashconfig(str(hex_path), "ffffffffffffffffffffffffdcf9ffff") == 0:
            failures.append("CLI: flashconfig accepted a different expectation")
        if run_flashconfig(str(hex_path), "nothex") != 2:
            failures.append("CLI: flashconfig accepted a malformed expectation")
        bad_path = Path(tmp) / "bad.hex"
        bad_path.write_text(make_hex(modified(CONFIG_ADDRESS + 12, 0xDC)), encoding="ascii")
        if run_check(str(bad_path), None, DEFAULT_VECTOR_COUNT, None, False) == 0 or not bad_path.exists():
            failures.append("CLI: a locked image passed, or was removed without --delete-on-fail")
        if run_check(str(bad_path), None, DEFAULT_VECTOR_COUNT, None, True) == 0 or bad_path.exists():
            failures.append("CLI: --delete-on-fail did not remove the failing image")
        # noinit through the CLI, with a fake nm that prints a prepared text
        nm_file = Path(tmp) / "nm.txt"
        fake_nm = Path(tmp) / "fake_nm.sh"
        fake_nm.write_text("#!/bin/sh\ncat '%s'\n" % nm_file)
        fake_nm.chmod(0o755)
        manifest = Path(tmp) / "noinit.txt"
        nm_file.write_text(nm_ok)
        if run_noinit("unused.elf", str(fake_nm), str(manifest), True) != 0 or not manifest.exists():
            failures.append("CLI: noinit --write did not write the manifest")
        if run_noinit("unused.elf", str(fake_nm), str(manifest), False) != 0:
            failures.append("CLI: noinit failed on the layout it just pinned")
        nm_file.write_text(nm_ok.replace("20000080 0000001c b", "20000088 0000001c b"))
        if run_noinit("unused.elf", str(fake_nm), str(manifest), False) == 0:
            failures.append("CLI: noinit passed a moved object")
        if run_noinit("unused.elf", str(Path(tmp) / "no_such_nm"), str(manifest), False) == 0:
            failures.append("CLI: noinit passed with an nm that does not exist")

    for failure in failures:
        print("self-test FAILED: " + failure)
    print("check_image self-test: %s (%d cases)" % ("ok" if not failures else "%d failure(s)" % len(failures), cases[0]))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    check = sub.add_parser("check")
    check.add_argument("hex")
    check.add_argument("--expect-sp", type=lambda s: int(s, 0), default=None)
    check.add_argument("--vectors", type=int, default=DEFAULT_VECTOR_COUNT, help="vector table entries to check (default 116)")
    check.add_argument("--ramfunc-l", default=None, metavar="BASE:SIZE",
                       help="SRAM_L code region vectors may point into (the hot code copied there at reset), e.g. 0x1FFF0000:0xC000")
    check.add_argument("--json", default=None, help="also write the results and image facts here")
    check.add_argument("--delete-on-fail", action="store_true")
    check.add_argument("--allow-last-sector", action="store_true",
                       help="waive last_sector_free (OS-BUG-LASTSECTOR): only for the CI build of a bench image that is not meant for a unit")
    fc = sub.add_parser("flashconfig")
    fc.add_argument("hex")
    fc.add_argument("--expect", required=True, help="the 16 bytes expected at 0x400, as 32 hex digits")
    ni = sub.add_parser("noinit")
    ni.add_argument("elf")
    ni.add_argument("--nm", required=True, help="the toolchain's nm (arm-none-eabi-nm)")
    ni.add_argument("--expect", required=True, help="the pinned layout (platform/mk66/tools/noinit_layout.txt)")
    ni.add_argument("--write", action="store_true", help="write the layout of ELF to --expect instead of checking it (a deliberate change)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "check":
        try:
            ramfunc_l = None
            if args.ramfunc_l:
                base, size = args.ramfunc_l.split(":")
                ramfunc_l = (int(base, 0), int(size, 0))
            return run_check(args.hex, args.expect_sp, args.vectors, args.json, args.delete_on_fail, ramfunc_l, args.allow_last_sector)
        except (OSError, UnicodeError, ValueError) as exc:
            print("FAIL hex_parse: cannot read %s: %s" % (args.hex, exc))
            return 1
    if args.command == "flashconfig":
        try:
            return run_flashconfig(args.hex, args.expect)
        except (OSError, UnicodeError) as exc:
            print("FAIL flashconfig: cannot read %s: %s" % (args.hex, exc))
            return 1
    if args.command == "noinit":
        return run_noinit(args.elf, args.nm, args.expect, args.write)
    parser.error("a command or --self-test is required")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
