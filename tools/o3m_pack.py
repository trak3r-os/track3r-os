#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Packs a Community OS module (ADR 0002, module/sdk/o3module.h) into a .tmod file (stdlib only).

    o3m_pack.py build --cxx arm-none-eabi-g++ --src a.cpp [--src b.cpp] --out x.tmod [--elf x.elf] [-I dir] [-D NAME=V] [--opt -O2] [--version X.Y.Z] [--caps midi_in,read_project,edit_project]
    o3m_pack.py pack  --elf x.elf --out x.tmod [--version X.Y.Z] [--caps ...]
    o3m_pack.py raw   --elf x.elf --out x.bin        the hot + cold bytes of an ELF linked at some bases (the loader test's oracle)
    o3m_pack.py info  x.tmod
    o3m_pack.py --self-test

`build` is the one place the module build flags are written: compile with the Cortex-M4F flags the SDK promises (hard float,
-mlong-calls, no PIC, no unwind tables, no libc), link with sdk/o3module.ld (static, --emit-relocs, --gc-sections) and pack.
With --hot-at / --cold-at it links the module at those bases instead of 0 (what the loader's output must equal, byte for
byte, once it has relocated the module linked at 0 to the same bases: the relocation test's oracle).

`pack` reads the linked ELF: the hot section (.o3m_hot), the cold section (.o3m_cold), the symbol `o3m_entry` (the module's
descriptor) and the relocations the linker kept (--emit-relocs). It writes the file the OS loads: a 64-byte header, the image
(hot, then cold), the table of the image offsets of every 32-bit absolute address (R_ARM_ABS32). It REFUSES, naming the cause:
a writable global, a constructor, an undefined symbol (a libc or libgcc call), an absolute symbol, a relocation of any other
kind than an absolute word or a PC-relative one inside one section (a movw/movt pair, a call from the hot code to the cold
code), and a descriptor the loader would refuse. Kind 3 (an importer, OS-FLASHDIET I1) packs with its rules: it must declare
ABI 1.11 (the minor that published the kind), its descriptor carries the importer table (o3m_entry.importer, checked here like
the loader checks it), and the header's word 44 is written with the importer layout tag -- 0 for every other kind. A module of ABI 1.12 (OS-INSFX-ABI2) has the
fourth descriptor tail (render_st, fx_flags, factory presets, the KEY parameter flag), checked here with the loader's rules. From 1.13 (OS-MODRATE) an instrument may set O3M_FX_RATE in fx_flags (the rule is per kind and minor, like the loader's). The loader
checks all of it again: this tool is a convenience, not a gate.

--version X.Y.Z (ABI 1.2, OS-MODVER) is the module's own version, written to the header's module_version word as major << 16 | minor << 8 | patch (major 0..65535, minor and
patch 0..255; no leading zeros). The default, 0.0.0, is "unversioned" (the word is 0). The OS prefers a card copy to the built-in module of the same name only when the card copy's
version is higher, so a module that is meant to be shipped and updated carries one. The header's ABI minor is the module's own (the SDK it was built with): a version needs a module
that declares ABI 1.2 (module.json: "abi": "1.2"), and the packer refuses one on a module that declares less, because an OS of ABI 1.1 would refuse the file anyway.

--caps NAME[,NAME...] (ABI 1.3, OS-H1, ADR 0014) is an app's request for host services, written to the header's capability word (byte 40): midi_in, read_project, edit_project, audio_tap (ABI 1.4, OS-H2a), export_files (ABI 1.7, OS-H4). An app that asks must
declare ABI 1.3, 1.4 for audio_tap or 1.7 for export_files (module.json: "capabilities": [...], which makes the generator write minor 3 or 4); the packer refuses capabilities on an instrument or an effect, on a module that declares less than 1.3,
and any name it does not know. The OS lends a table only for a capability the file asks for.
"""
import argparse
import math
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

# ---- the file format (o3module.h) -------------------------------------------------------------------------------------------
MAGIC = 0x444D334F
HEADER_SIZE = 64
TARGET_ARMV7EM = 1
ABI_MAJOR, ABI_MINOR = 1, 13
VERSION_ABI_MINOR = 2             # the ABI minor that put the module version in the header (o3m_file_header::module_version, byte 36)
CAPS_ABI_MINOR = 3                # the ABI minor that put the capability mask in the header (reserved2[0], byte 40; apps only)
IMPORTER_ABI_MINOR = 11           # OS-FLASHDIET I1: the ABI minor that published kind 3 and made word 44 the importer layout tag
STEREO_ABI_MINOR = 12             # OS-INSFX-ABI2 A1: the ABI minor that published render_st, fx_flags, the factory presets and the KEY flag (the descriptor is 100 bytes from minor 12)
CAPS = {"midi_in": 1, "read_project": 2, "audio_tap": 4, "edit_project": 8, "export_files": 16, "play_notes": 64}  # O3M_CAP_* of ABI 1.3, audio_tap of 1.4, export_files of 1.7 and play_notes of 1.10 (the bits of later slices are assigned in o3module.h and refused until their minor)
CAPS_ABI_1_4 = 4                  # the capability bits that need ABI 1.4 in the file
CAPS_ABI_1_7 = 16                 # the capability bits that need ABI 1.7 in the file (export_files)
CAPS_ABI_1_10 = 64                # the capability bits that need ABI 1.10 in the file (play_notes)
CAPS_FIELD = 15                   # index of the capability word (reserved2[0]) in the unpacked header
TAG_FIELD = 16                    # index of word 44 (reserved2[1]) in the unpacked header: the importer layout tag, kind 3 only
KIND_INSTRUMENT, KIND_EFFECT, KIND_IMPORTER, KIND_APP = 1, 2, 3, 4
HEADER = struct.Struct("<IHHBBBBIIIIIHHI2II3I")  # ... param_count, view_count, module_version (byte 36), reserved2 x2, image_crc (48), reserved3 x3
assert HEADER.size == HEADER_SIZE
VERSION_FIELD, CRC_FIELD = 14, 17                 # indexes of module_version and image_crc in the unpacked header
DESC = struct.Struct("<16I")      # o3m_module on a 32-bit target (the last two words: the views' table and the view count, ABI 1.1); 64 bytes up to ABI 1.7
DESC_EXT = struct.Struct("<4I")   # ABI 1.8: pitch, pages, page_count, labels, appended (the descriptor is 80 bytes from minor 8: o3module.h O3M_MODULE_SIZE)
DESC_IMP = struct.Struct("<I")    # ABI 1.11: importer, the importer kind's table, appended (the descriptor is 84 bytes from minor 11)
DESC_FX = struct.Struct("<4I")    # ABI 1.12: render_st, fx_flags, presets, preset_count, appended (the descriptor is 100 bytes from minor 12)
PRESET = struct.Struct("<III")    # o3m_preset on a 32-bit target: name, values, count
PRESETS_MAX, PRESET_NAME_MAX = 32, 16
FX_WIDENS = 1                     # O3M_FX_WIDENS, an effect's fx_flags bit (ABI 1.12)
FX_RATE = 2                       # O3M_FX_RATE, an instrument's fx_flags bit (ABI 1.13, OS-MODRATE): render() is correct at half and quarter sample rate
RATE_ABI_MINOR = 13               # the ABI minor that defined FX_RATE
PARAM_KEY = 32                    # O3M_PARAM_KEY (ABI 1.12): an effect's INT parameter 0..16, the sidechain key; at most two a module
IMPORTER = struct.Struct("<8I")   # o3m_importer on a 32-bit target: size, formats, format_count, begin, step, read_pcm, report, close
FORMAT = struct.Struct("<I")      # o3m_format_desc: the extension string
IMPORTER_FORMATS_MAX, EXT_MAX = 8, 8
PAGE = struct.Struct("<II")       # o3m_page: name, first_param
LABELS = struct.Struct("<II")     # o3m_labels: names, count
PAGES_MAX, PAGE_PARAMS_MAX, PAGE_NAME_MAX, LABELS_MIN, LABELS_MAX, LABEL_NAME_MAX = 4, 8, 8, 2, 16, 12
PARAM_INT = 2                     # O3M_PARAM_INT
PARAM = struct.Struct("<IfffI")   # o3m_param
UI = struct.Struct("<5I")         # o3m_ui up to ABI 1.8 (20 bytes); ABI 1.9 appends `keys` (UI_KEYS, 24 bytes: o3module.h O3M_UI_SIZE)
UI_KEYS = struct.Struct("<I")
KEYS_ABI_MINOR = 9
VIEWS_MAX = 8
NAME_MAX, PARAM_NAME_MAX, PARAMS_MAX = 24, 16, 32
MAX_IMAGE, MAX_STATE = 256 * 1024, 256 * 1024

# ---- OS-FLASHDIET I1/I2: the importer layout tag (model/include/o3os/model/import_layout.h) -----------------------------------
# The tag is the hash model::kImportLayoutTag computes over the import-facing model layout (the arena, Project and the structs the importer
# writes). It lives in model/import_layout_tag.txt, checked in next to the header: this file (and o3m_gen.py) read it as the default, so a
# layout change is one edit there. ctest o3m_layout_tag_pin (host builds) prints model::kImportLayoutTag with a host-compiled one-liner
# (tools/o3m_layout_tag.cpp) and holds the file to it, and tests/module_importer_test.cpp holds the built tracker.tmod's word 44 to the C++
# constant. The fallback pin is for --self-test without the repository beside the script.
_TAG_FILE = Path(__file__).resolve().parent.parent / "model" / "import_layout_tag.txt"
try:
    IMPORT_LAYOUT_TAG = int(_TAG_FILE.read_text().strip(), 0)
except OSError:
    IMPORT_LAYOUT_TAG = 0x21022380  # pinned fallback (see above)

# ---- ELF ------------------------------------------------------------------------------------------------------------------
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 1, 2, 3, 4, 8, 9
SHF_ALLOC = 2
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_WEAK = 2
ET_EXEC, EM_ARM = 2, 40

R_ARM_ABS32 = 2
# PC-relative kinds: fine when the place and the target are in the same output section (the linker already resolved them).
PC_RELATIVE = {
    3: "R_ARM_REL32", 10: "R_ARM_THM_CALL", 11: "R_ARM_THM_PC8", 13: "R_ARM_THM_ALU_PREL_11_0", 14: "R_ARM_THM_PC12",
    30: "R_ARM_THM_JUMP24", 49: "R_ARM_THM_MOVW_PREL_NC", 50: "R_ARM_THM_MOVT_PREL", 51: "R_ARM_THM_JUMP19",
    102: "R_ARM_THM_JUMP11", 103: "R_ARM_THM_JUMP8",
}
KNOWN_BAD = {
    47: "R_ARM_THM_MOVW_ABS_NC", 48: "R_ARM_THM_MOVT_ABS",
    28: "R_ARM_CALL", 29: "R_ARM_JUMP24", 43: "R_ARM_MOVW_ABS_NC", 44: "R_ARM_MOVT_ABS", 38: "R_ARM_TARGET1", 42: "R_ARM_PREL31",
    1: "R_ARM_PC24", 4: "R_ARM_PC13", 5: "R_ARM_ABS16", 8: "R_ARM_ABS8", 21: "R_ARM_GOT_BREL", 24: "R_ARM_GLOB_DAT", 27: "R_ARM_PLT32",
}


class PackError(Exception):
    pass


class Section:
    def __init__(self, index, name, type_, flags, addr, offset, size, link, info, entsize):
        self.index, self.name, self.type, self.flags, self.addr = index, name, type_, flags, addr
        self.offset, self.size, self.link, self.info, self.entsize = offset, size, link, info, entsize


class Symbol:
    def __init__(self, name, value, size, info, shndx):
        self.name, self.value, self.size, self.shndx = name, value, size, shndx
        self.type, self.bind = info & 15, info >> 4


class Elf:
    def __init__(self, data):
        self.data = data
        if len(data) < 52 or data[:4] != b"\x7fELF":
            raise PackError("not an ELF file")
        if data[4] != 1 or data[5] != 1:
            raise PackError("not a 32-bit little-endian ELF")
        (e_type, e_machine, _v, _entry, _phoff, shoff, _flags, _ehsize, _phentsize, _phnum, shentsize, shnum, shstrndx) = struct.unpack_from(
            "<HHIIIIIHHHHHH", data, 16)
        if e_machine != EM_ARM:
            raise PackError("not an ARM ELF (e_machine %d)" % e_machine)
        if e_type != ET_EXEC:
            raise PackError("not a linked executable (e_type %d): link with o3module.ld, not -r" % e_type)
        if shentsize != 40 or shnum == 0 or shoff == 0 or shoff + shnum * 40 > len(data):
            raise PackError("no usable section header table")
        raw = [struct.unpack_from("<IIIIIIIIII", data, shoff + 40 * i) for i in range(shnum)]
        if shstrndx >= shnum:
            raise PackError("bad section name table index")
        str_off, str_size = raw[shstrndx][4], raw[shstrndx][5]
        if str_off + str_size > len(data):
            raise PackError("section name table runs past the file")
        self.sections = []
        for i, (name, type_, flags, addr, off, size, link, info, _align, entsize) in enumerate(raw):
            if type_ != SHT_NOBITS and type_ != 0 and off + size > len(data):
                raise PackError("section %d runs past the end of the file" % i)
            self.sections.append(Section(i, self._cstr(str_off + name, str_off + str_size), type_, flags, addr, off, size, link, info, entsize))
        self.symbols = []
        for s in self.sections:
            if s.type == SHT_SYMTAB:
                strtab = self.sections[s.link] if s.link < len(self.sections) else None
                if strtab is None or strtab.type != SHT_STRTAB or s.entsize != 16:
                    raise PackError("bad symbol table")
                for k in range(s.size // 16):
                    n, value, size, info, _other, shndx = struct.unpack_from("<IIIBBH", data, s.offset + 16 * k)
                    self.symbols.append(Symbol(self._cstr(strtab.offset + n, strtab.offset + strtab.size), value, size, info, shndx))
                break

    def _cstr(self, start, limit):
        end = self.data.find(b"\0", start, limit)
        if end < 0:
            raise PackError("an unterminated name in the ELF")
        return self.data[start:end].decode("latin-1")

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def bytes_of(self, s):
        return self.data[s.offset:s.offset + s.size]


def read_cstr(image, addr, limit, what):
    if addr >= len(image):
        raise PackError("%s points outside the image" % what)
    end = image.find(b"\0", addr, min(len(image), addr + limit))
    if end < 0:
        raise PackError("%s is not a NUL-terminated string of at most %d bytes" % (what, limit - 1))
    return image[addr:end].decode("latin-1")


def check_importer_table(image, base, hot_size, image_size, imp_p):
    """OS-FLASHDIET I1/I2: o3m_entry.importer (a link address), checked like the loader checks it: the table inside the cold part, its size exact,
    the format list 1..IMPORTER_FORMATS_MAX entries inside the image with printable extensions, and begin, step, read_pcm, report and close Thumb
    functions inside the image (a missing function refuses the file). Every address is checked with a 0 <= lower bound: a pointer below the image's
    base would otherwise wrap negative and pass the `< image_size` test (review note 3)."""
    i_off = imp_p - base
    if not hot_size <= i_off or i_off % 4 or i_off + IMPORTER.size > image_size:  # (a negative i_off fails hot_size <= i_off too)
        raise PackError("o3m_entry.importer is misplaced")
    size, formats_p, format_count, begin_p, step_p, read_pcm_p, report_p, close_p = IMPORTER.unpack_from(image, i_off)
    if size != IMPORTER.size:
        raise PackError("the importer's table size is %d, sizeof(o3m_importer) is %d" % (size, IMPORTER.size))
    if not (1 <= format_count <= IMPORTER_FORMATS_MAX) or formats_p == 0:
        raise PackError("o3m_importer.format_count %d: 1..%d formats, and a table when there are any" % (format_count, IMPORTER_FORMATS_MAX))
    f_off = formats_p - base
    if not hot_size <= f_off or f_off % 4 or f_off + FORMAT.size * format_count > image_size:
        raise PackError("o3m_importer.formats is misplaced")
    for i in range(format_count):
        ext_p = struct.unpack_from("<I", image, f_off + FORMAT.size * i)[0]
        if not 0 <= ext_p - base < image_size:
            raise PackError("the extension of format %d points outside the image" % i)
        ext = read_cstr(image, ext_p - base, EXT_MAX, "the extension of format %d" % i)
        if not ext or any(ord(c) < 32 or ord(c) > 126 for c in ext):
            raise PackError("the extension of format %d is empty or not printable ASCII" % i)
    for label, ptr in (("begin", begin_p), ("step", step_p), ("read_pcm", read_pcm_p), ("report", report_p), ("close", close_p)):
        if ptr == 0 or not (ptr & 1) or not 0 <= (ptr & ~1) - base < image_size:
            raise PackError("o3m_importer.%s is missing or not a Thumb function inside the image" % label)


def link_to_physical_check(value, hot_size, image_size):
    return value < hot_size or value <= image_size


def parse_version(text):
    """'X.Y.Z' -> major << 16 | minor << 8 | patch (o3module.h O3M_VERSION). Raises PackError when it is not three plain numbers in range."""
    m = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", text)
    if not m:
        raise PackError("version '%s' is not X.Y.Z (three whole numbers, no leading zeros)" % text)
    major, minor, patch = (int(g) for g in m.groups())
    if major > 0xFFFF or minor > 0xFF or patch > 0xFF:
        raise PackError("version %s is out of range (major 0..65535, minor 0..255, patch 0..255)" % text)
    return major << 16 | minor << 8 | patch


def parse_caps(text):
    """'midi_in,edit_project' -> the O3M_CAP_* mask. Raises PackError on a name that is not a capability of ABI 1.3."""
    mask = 0
    for name in text.split(","):
        name = name.strip()
        if name == "":
            continue
        if name not in CAPS:
            raise PackError("unknown capability '%s' (ABI 1.%d has: %s)" % (name, ABI_MINOR, ", ".join(sorted(CAPS))))
        mask |= CAPS[name]
    return mask


def format_version(version):
    return "unversioned" if version == 0 else "%d.%d.%d" % (version >> 16, (version >> 8) & 0xFF, version & 0xFF)


def pack_elf(data, version=0, caps=0, layout_tag=IMPORT_LAYOUT_TAG):
    """Returns (file bytes, info dict). `version` is the module version word (parse_version), 0 = unversioned; `caps` the capability mask (parse_caps);
    `layout_tag` is written to header word 44 for kind 3 only (OS-FLASHDIET: the build passes --layout-tag-file, printed from model::kImportLayoutTag). Raises PackError with the cause."""
    elf = Elf(data)
    hot, cold = elf.section(".o3m_hot"), elf.section(".o3m_cold")
    if cold is None or cold.type != SHT_PROGBITS or cold.size == 0:
        raise PackError("no .o3m_cold section: link with sdk/o3module.ld")
    if hot is not None and hot.size == 0:
        hot = None  # a module with no hot code (an app: it renders nothing): the linker leaves an empty section behind, with flags of its own
    if hot is None:
        hot_size, hot_addr = 0, cold.addr
    elif hot.type != SHT_PROGBITS:
        raise PackError(".o3m_hot is not progbits")
    else:
        hot_size, hot_addr = hot.size, hot.addr
    base = hot_addr
    if hot is not None and cold.addr != base + hot.size:
        raise PackError(".o3m_cold does not follow .o3m_hot (cold at 0x%X, hot ends at 0x%X)" % (cold.addr, base + hot.size))
    if hot_size % 16 or cold.size % 16:
        raise PackError("section sizes are not multiples of 16 (the linker script's ALIGN is gone)")
    image_size = hot_size + cold.size
    if image_size > MAX_IMAGE:
        raise PackError("the image is %d bytes, the limit is %d" % (image_size, MAX_IMAGE))
    for s in elf.sections:
        if s.flags & SHF_ALLOC and s.size != 0 and s.name not in (".o3m_hot", ".o3m_cold"):
            why = {".o3m_forbidden_data": "writable data (a global or static variable, .data/.bss): keep all state in the instance block",
                   ".o3m_forbidden_init": "a constructor or destructor table (.init_array, .ctors): no global constructors"}.get(s.name, "a section the loader does not place")
            raise PackError("section %s (%d bytes): %s" % (s.name, s.size, why))
    if (hot is not None and (hot.flags & 1)) or cold.flags & 1:
        raise PackError("a module section is writable")

    image = (elf.bytes_of(hot) if hot is not None else b"") + elf.bytes_of(cold)
    seg_of = {}
    if hot is not None:
        seg_of[hot.index] = "hot"
    seg_of[cold.index] = "cold"

    def link_addr_to_offset(addr):
        return addr - base

    # ---- relocations
    offsets = set()
    for rs in elf.sections:
        if rs.type == SHT_RELA:
            raise PackError("a RELA relocation section (%s): ARM uses REL" % rs.name)
        if rs.type != SHT_REL:
            continue
        if rs.info not in seg_of:
            tgt = elf.sections[rs.info] if rs.info < len(elf.sections) else None
            if tgt is not None and tgt.flags & SHF_ALLOC and tgt.size:
                raise PackError("relocations against %s, which the loader does not place" % tgt.name)
            continue  # debug sections and the like
        symtab = elf.sections[rs.link]
        if symtab.type != SHT_SYMTAB:
            raise PackError("relocation section %s has no symbol table" % rs.name)
        for k in range(rs.size // 8):
            r_offset, r_info = struct.unpack_from("<II", elf.data, rs.offset + 8 * k)
            rtype, rsym = r_info & 0xFF, r_info >> 8
            if rsym >= len(elf.symbols):
                raise PackError("a relocation names symbol %d, which does not exist" % rsym)
            sym = elf.symbols[rsym]
            place_off = link_addr_to_offset(r_offset)
            where = "%s+0x%X" % (rs.name[4:], r_offset - elf.sections[rs.info].addr)
            if rtype == R_ARM_ABS32:
                if sym.shndx == SHN_UNDEF or sym.shndx == SHN_COMMON:
                    raise PackError("undefined symbol '%s' (referenced at %s): a libc / libgcc function, or a global the module forgot to define" % (sym.name, where))
                if sym.shndx == SHN_ABS:
                    raise PackError("an absolute address (symbol '%s' at %s): a pointer to a fixed place is not relocatable" % (sym.name, where))
                if sym.shndx not in seg_of:
                    raise PackError("a pointer to '%s', which is in a section the loader does not place (%s)" % (sym.name, where))
                if place_off % 4 or not (0 <= place_off <= image_size - 4):
                    raise PackError("an unaligned or out-of-image absolute word at %s" % where)
                value = struct.unpack_from("<I", image, place_off)[0]
                if not link_to_physical_check(value - base, hot_size, image_size):
                    raise PackError("the absolute word at %s holds 0x%X, outside the image" % (where, value))
                offsets.add(place_off)
            elif rtype in PC_RELATIVE:
                if sym.shndx == SHN_UNDEF or sym.shndx == SHN_COMMON:
                    raise PackError("undefined symbol '%s' (called at %s): a libc / libgcc function, or a function the module forgot to define" % (sym.name, where))
                if sym.shndx == SHN_ABS:
                    raise PackError("a PC-relative reference to the absolute symbol '%s' at %s" % (sym.name, where))
                if sym.shndx not in seg_of or seg_of[sym.shndx] != seg_of[rs.info]:
                    raise PackError("%s at %s crosses between the hot and the cold part ('%s'): the two are loaded far apart; "
                                    "build with -mlong-calls and keep data reached by hot code out of PC-relative range" % (PC_RELATIVE[rtype], where, sym.name))
            else:
                name = KNOWN_BAD.get(rtype)
                hint = {47: " (a movw/movt pair holds an absolute address: use the literal-pool code generation, no -mpure-code / -mslow-flash-data)",
                        48: " (a movw/movt pair holds an absolute address: use the literal-pool code generation, no -mpure-code / -mslow-flash-data)"}.get(rtype, "")
                raise PackError("unsupported relocation %s (type %d) at %s against '%s'%s" % (name or "R_ARM_?", rtype, where, sym.name, hint))
    reloc_offsets = sorted(offsets)

    # ---- the descriptor
    entry = [s for s in elf.symbols if s.name == "o3m_entry"]
    if len(entry) != 1 or entry[0].shndx != cold.index or entry[0].type != 1:
        raise PackError("no `o3m_entry` object in .o3m_cold (a const o3m_module with C linkage, in section .o3m_entry)")
    desc_off = link_addr_to_offset(entry[0].value)
    if desc_off % 4 or desc_off < hot_size or desc_off + DESC.size > image_size:
        raise PackError("o3m_entry is misplaced")
    (abi_major, abi_minor, kind, struct_size, name_p, state_bytes, param_count, params_p, init_p, render_p, set_param_p, note_on_p, note_off_p,
     shutdown_p, ui_p, view_count) = DESC.unpack_from(image, desc_off)
    # Pointers in the image are link addresses (offset + base); the descriptor's own words are relocated like any other, so a
    # pointer outside the image has been refused above.
    if abi_major != ABI_MAJOR or abi_minor > ABI_MINOR:
        raise PackError("module ABI %d.%d, this packer's SDK is %d.%d" % (abi_major, abi_minor, ABI_MAJOR, ABI_MINOR))
    desc_size = DESC.size + (DESC_EXT.size if abi_minor >= 8 else 0) + (DESC_IMP.size if abi_minor >= IMPORTER_ABI_MINOR else 0) + (DESC_FX.size if abi_minor >= STEREO_ABI_MINOR else 0)  # per minor (o3module.h O3M_MODULE_SIZE): 64 up to 1.7, 80 from 1.8, 84 from 1.11, 100 from 1.12
    if struct_size != desc_size:
        raise PackError("o3m_entry.struct_size is %d, the SDK's o3m_module of ABI 1.%d is %d bytes (a module built against another header, or a hand-written sizeof instead of O3M_MODULE_SIZE?)" % (
            struct_size, abi_minor, desc_size))
    if desc_off + desc_size > image_size:
        raise PackError("o3m_entry is misplaced")
    pitch_p, pages_p, page_count, labels_p = DESC_EXT.unpack_from(image, desc_off + DESC.size) if abi_minor >= 8 else (0, 0, 0, 0)
    imp_p = DESC_IMP.unpack_from(image, desc_off + DESC.size + DESC_EXT.size)[0] if abi_minor >= IMPORTER_ABI_MINOR else 0
    fx_render_p, fx_flags, presets_p, preset_count = DESC_FX.unpack_from(image, desc_off + DESC.size + DESC_EXT.size + DESC_IMP.size) if abi_minor >= STEREO_ABI_MINOR else (0, 0, 0, 0)
    if version and abi_minor < VERSION_ABI_MINOR:
        raise PackError("a module version needs ABI 1.%d and the module declares ABI %d.%d (module.json: \"abi\": \"1.%d\", then regenerate; by hand: build against SDK 1.%d)" % (
            VERSION_ABI_MINOR, abi_major, abi_minor, VERSION_ABI_MINOR, VERSION_ABI_MINOR))
    if caps:
        if kind != KIND_APP:
            raise PackError("capabilities are an app's: this module is not one")
        if abi_minor < CAPS_ABI_MINOR:
            raise PackError("capabilities need ABI 1.%d and the module declares ABI %d.%d (module.json: \"capabilities\" makes the generator write it; by hand: build against SDK 1.%d)" % (
                CAPS_ABI_MINOR, abi_major, abi_minor, CAPS_ABI_MINOR))
        if caps & CAPS_ABI_1_4 and abi_minor < 4:
            raise PackError("audio_tap needs ABI 1.4 and the module declares ABI %d.%d (module.json: \"capabilities\": [\"audio_tap\"] makes the generator write it)" % (abi_major, abi_minor))
        if caps & CAPS_ABI_1_7 and abi_minor < 7:
            raise PackError("export_files needs ABI 1.7 and the module declares ABI %d.%d (module.json: \"capabilities\": [\"export_files\"] makes the generator write it)" % (abi_major, abi_minor))
        if caps & CAPS_ABI_1_10 and abi_minor < 10:
            raise PackError("play_notes needs ABI 1.10 and the module declares ABI %d.%d (module.json: \"capabilities\": [\"play_notes\"] makes the generator write it)" % (abi_major, abi_minor))
        if caps & ~sum(CAPS.values()):
            raise PackError("capability mask %#x has bits ABI 1.%d does not define" % (caps, ABI_MINOR))
    if view_count > VIEWS_MAX or (view_count == 0) != (ui_p == 0):
        raise PackError("o3m_entry.view_count %d / ui: 0..%d views, and a ui table exactly when there are views" % (view_count, VIEWS_MAX))
    if ui_p:
        u_off = ui_p - base
        if u_off < hot_size or u_off % 4 or u_off + UI.size > image_size:
            raise PackError("o3m_entry.ui is misplaced")
        ui_expect = UI.size + (UI_KEYS.size if abi_minor >= KEYS_ABI_MINOR else 0)  # the table is sized per minor, like the descriptor (o3module.h O3M_UI_SIZE)
        if u_off + ui_expect > image_size:
            raise PackError("o3m_entry.ui is misplaced")
        ui_size, ui_enter, ui_input, ui_draw, ui_exit = UI.unpack_from(image, u_off)
        ui_keys = UI_KEYS.unpack_from(image, u_off + UI.size)[0] if abi_minor >= KEYS_ABI_MINOR else 0
        for label, ptr, optional in (("view_draw", ui_draw, False), ("enter", ui_enter, True), ("input", ui_input, True), ("exit", ui_exit, True), ("keys", ui_keys, True)):
            if ui_size != ui_expect or (ptr == 0 and not optional) or (ptr != 0 and (not (ptr & 1) or not ((ptr & ~1) - base) < image_size)):
                raise PackError("o3m_entry.ui.%s is missing or not a Thumb function inside the image (or ui.struct_size is wrong: it is O3M_UI_SIZE(minor), %d bytes for ABI 1.%d)" % (label, ui_expect, abi_minor))
    if kind not in (KIND_INSTRUMENT, KIND_EFFECT, KIND_IMPORTER) and not (kind == KIND_APP and abi_minor >= 1):
        raise PackError("module kind %d is not an instrument, an effect, an importer or (ABI 1.1) an app" % kind)
    if kind == KIND_APP and view_count == 0:
        raise PackError("an app has no view: o3m_entry.view_count is 0 (an app is its view)")
    if kind == KIND_IMPORTER:  # OS-FLASHDIET I1: the importer kind is published at ABI 1.11, with its table in the descriptor
        if abi_minor < IMPORTER_ABI_MINOR:
            raise PackError("an importer (kind 3) needs ABI 1.%d and the module declares ABI %d.%d (module.json: \"abi\": \"1.11\", then regenerate)" % (
                IMPORTER_ABI_MINOR, abi_major, abi_minor))
        if param_count or view_count:
            raise PackError("an importer (kind 3, OS-FLASHDIET I1) has no parameters and no views: it serves files, it is not heard or seen")
        if imp_p == 0:
            raise PackError("an importer (kind 3) needs o3m_entry.importer: the importer's table (o3m_importer)")
        check_importer_table(image, base, hot_size, image_size, imp_p)
    elif imp_p != 0:
        raise PackError("o3m_entry.importer is set: the importer's table is kind 3's (NULL in every other kind)")
    if state_bytes == 0 or state_bytes > MAX_STATE:
        raise PackError("state_bytes %d is out of range (1..%d)" % (state_bytes, MAX_STATE))
    if param_count > PARAMS_MAX:
        raise PackError("%d parameters, the limit is %d" % (param_count, PARAMS_MAX))
    name = read_cstr(image, name_p - base, NAME_MAX, "the module name")
    if not name or any(ord(c) < 32 or ord(c) > 126 for c in name):
        raise PackError("the module name is empty or not printable ASCII")
    for i, ptr in enumerate((init_p, render_p, set_param_p, note_on_p, note_off_p, shutdown_p)):
        label = ("init", "render", "set_param", "note_on", "note_off", "shutdown")[i]
        if kind in (KIND_APP, KIND_IMPORTER) and 1 <= i <= 4 and ptr == 0:
            continue  # an app or an importer makes no sound: render, set_param, note_on and note_off may be NULL
        if ptr == 0 or not (ptr & 1) or not ((ptr & ~1) - base) < image_size:
            raise PackError("o3m_entry.%s is missing or not a Thumb function inside the image" % label)
    names = []
    param_info = []
    if param_count:
        p_off = params_p - base
        if p_off < hot_size or p_off % 4 or p_off + PARAM.size * param_count > image_size:
            raise PackError("o3m_entry.params is misplaced")
        for i in range(param_count):
            pname_p, pmin, pmax, pdef, pflags = PARAM.unpack_from(image, p_off + PARAM.size * i)
            pname = read_cstr(image, pname_p - base, PARAM_NAME_MAX, "the name of parameter %d" % i)
            if not pname or any(ord(c) < 32 or ord(c) > 126 for c in pname):
                raise PackError("parameter %d has an empty or non-printable name" % i)
            if not (pmin <= pdef <= pmax):
                raise PackError("parameter '%s': default %g is outside [%g, %g]" % (pname, pdef, pmin, pmax))
            names.append(pname)
            param_info.append({"name": pname, "min": pmin, "max": pmax, "def": pdef, "flags": pflags})
    elif params_p != 0:
        raise PackError("o3m_entry.params is set but param_count is 0")
    # ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): the pitch call, the page table and the label table, with the limits of o3module.h (the loader checks them again)
    if pitch_p and (not (pitch_p & 1) or not ((pitch_p & ~1) - base) < image_size):
        raise PackError("o3m_entry.pitch is not a Thumb function inside the image")
    if (pages_p == 0) != (page_count == 0) or page_count > PAGES_MAX or (param_count == 0 and (page_count or labels_p)):
        raise PackError("o3m_entry.pages / page_count: 0..%d pages, a table exactly when there are pages, and none for a module without parameters" % PAGES_MAX)
    if page_count:
        g_off = pages_p - base
        if g_off < hot_size or g_off % 4 or g_off + PAGE.size * page_count > image_size:
            raise PackError("o3m_entry.pages is misplaced")
        firsts = [PAGE.unpack_from(image, g_off + PAGE.size * k) for k in range(page_count)]
        for k, (gname_p, first) in enumerate(firsts):
            end = firsts[k + 1][1] if k + 1 < page_count else param_count
            if (k == 0 and first != 0) or not (first < end <= param_count) or end - first > PAGE_PARAMS_MAX:
                raise PackError("page %d: the first page starts at parameter 0, the pages ascend, hold 1..%d parameters and together cover the %d parameters" % (k, PAGE_PARAMS_MAX, param_count))
            gname = read_cstr(image, gname_p - base, PAGE_NAME_MAX, "the name of page %d" % k)
            if not gname or any(ord(c) < 32 or ord(c) > 126 for c in gname):
                raise PackError("page %d has an empty or non-printable name" % k)
    if labels_p:
        l_off = labels_p - base
        if l_off < hot_size or l_off % 4 or l_off + LABELS.size * param_count > image_size:
            raise PackError("o3m_entry.labels is misplaced")
        for i in range(param_count):
            names_p, count = LABELS.unpack_from(image, l_off + LABELS.size * i)
            if count == 0:
                if names_p:
                    raise PackError("labels of parameter %d: a table but no count" % i)
                continue
            info = param_info[i]
            if not (info["flags"] & PARAM_INT) or not (LABELS_MIN <= count <= LABELS_MAX) or info["min"] != int(info["min"]) or info["max"] != int(info["max"]) or int(info["max"]) - int(info["min"]) + 1 != count:
                raise PackError("labels of parameter '%s': an int parameter only, one text for each value from min to max (%d..%d of them)" % (info["name"], LABELS_MIN, LABELS_MAX))
            n_off = names_p - base
            if n_off < hot_size or n_off % 4 or n_off + 4 * count > image_size:
                raise PackError("the label table of parameter '%s' is misplaced" % info["name"])
            for v in range(count):
                text = read_cstr(image, struct.unpack_from("<I", image, n_off + 4 * v)[0] - base, LABEL_NAME_MAX, "label %d of parameter '%s'" % (v, info["name"]))
                if not text or any(ord(c) < 32 or ord(c) > 126 for c in text):
                    raise PackError("label %d of parameter '%s' is empty or not printable" % (v, info["name"]))

    # ABI 1.12 (OS-INSFX-ABI2 A1): the fourth tail, like the loader checks it (module/src/image.cpp fx_ok). Below 1.12 the four members are not read and a KEY bit is not honoured.
    if abi_minor >= STEREO_ABI_MINOR:
        if fx_render_p and (kind != KIND_EFFECT or not (fx_render_p & 1) or not 0 <= (fx_render_p & ~1) - base < image_size):
            raise PackError("o3m_entry.render_st is set on a module that is not an effect, or is not a Thumb function inside the image (the stereo render is an effect's)")
        # Like module/include/o3os/module/image.h fx_flags_allowed: WIDENS for an effect (and it needs render_st), RATE for an instrument of 1.13 or later.
        allowed = FX_WIDENS if kind == KIND_EFFECT else (FX_RATE if kind == KIND_INSTRUMENT and abi_minor >= RATE_ABI_MINOR else 0)
        if fx_flags & ~allowed or (fx_flags & FX_WIDENS and not fx_render_p):
            raise PackError("o3m_entry.fx_flags %#x: an effect may set O3M_FX_WIDENS (1, with render_st), an instrument of ABI 1.13 O3M_FX_RATE (2); nothing else is defined" % fx_flags)
        keys = 0
        for i, info in enumerate(param_info):
            if info["flags"] & PARAM_KEY:
                keys += 1
                if kind != KIND_EFFECT or not (info["flags"] & PARAM_INT) or info["min"] != 0 or info["max"] != 16 or keys > 2:
                    raise PackError("parameter '%s' is O3M_PARAM_KEY: an effect's INT parameter with min 0 and max 16, at most two a module" % info["name"])
        if (presets_p == 0) != (preset_count == 0) or preset_count > PRESETS_MAX:
            raise PackError("o3m_entry.presets / preset_count: 0..%d presets, a table exactly when there are presets" % PRESETS_MAX)
        if preset_count:
            if kind not in (KIND_INSTRUMENT, KIND_EFFECT) or param_count == 0:
                raise PackError("factory presets are an instrument's or an effect's with parameters (an app and an importer carry none)")
            t_off = presets_p - base
            if t_off < hot_size or t_off % 4 or t_off + PRESET.size * preset_count > image_size:
                raise PackError("o3m_entry.presets is misplaced")
            for k in range(preset_count):
                pname_p, values_p, count = PRESET.unpack_from(image, t_off + PRESET.size * k)
                v_off = values_p - base
                if not 1 <= count <= param_count or v_off < hot_size or v_off % 4 or v_off + 4 * count > image_size:
                    raise PackError("preset %d: 1..%d values (one a parameter in order), and a value table inside the image" % (k, param_count))
                if not 0 <= pname_p - base < image_size:
                    raise PackError("the name of preset %d points outside the image" % k)
                pname = read_cstr(image, pname_p - base, PRESET_NAME_MAX, "the name of preset %d" % k)
                if not pname or any(ord(c) < 32 or ord(c) > 126 for c in pname):
                    raise PackError("preset %d has an empty or non-printable name" % k)
                for i in range(count):
                    value = struct.unpack_from("<f", image, v_off + 4 * i)[0]
                    if not math.isfinite(value) or not (param_info[i]["min"] <= value <= param_info[i]["max"]):
                        raise PackError("preset '%s': value %d (%g) is outside [%g, %g] of parameter '%s'" % (pname, i, value, param_info[i]["min"], param_info[i]["max"], param_info[i]["name"]))

    body = image + b"".join(struct.pack("<I", o) for o in reloc_offsets)
    fields = [MAGIC, HEADER_SIZE, TARGET_ARMV7EM, kind, abi_major, abi_minor, 0, hot_size, image_size, desc_off, state_bytes,
              len(reloc_offsets), param_count, view_count, version, caps, layout_tag if kind == KIND_IMPORTER else 0, 0, 0, 0, 0]
    # (word 44, fields[TAG_FIELD]: the importer layout tag for kind 3 only (OS-FLASHDIET I1/I2); 0 for every other kind, as the loader requires)
    crc = zlib.crc32(body, zlib.crc32(HEADER.pack(*fields))) & 0xFFFFFFFF  # the header with its CRC field zero, then the body
    fields[CRC_FIELD] = crc
    header = HEADER.pack(*fields)
    info = {"name": name, "kind": kind, "hot": hot_size, "cold": cold.size, "relocs": len(reloc_offsets), "state": state_bytes,
            "params": names, "views": view_count, "size": len(header) + len(body), "abi": (abi_major, abi_minor), "version": version, "param_info": param_info,
            "state_bytes": state_bytes, "caps": caps, "layout_tag": layout_tag if kind == KIND_IMPORTER else 0}
    return header + body, info


def raw_image(data):
    """The hot then cold bytes of an ELF (linked at any bases)."""
    elf = Elf(data)
    hot, cold = elf.section(".o3m_hot"), elf.section(".o3m_cold")
    if cold is None:
        raise PackError("no .o3m_cold section")
    return (elf.bytes_of(hot) if hot is not None else b"") + elf.bytes_of(cold)


# ---- build ------------------------------------------------------------------------------------------------------------------
SDK_DIR = Path(__file__).resolve().parent.parent / "module" / "sdk"
CFLAGS = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
          "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-fno-pic",
          "-mlong-calls", "-ffunction-sections", "-fdata-sections", "-fsingle-precision-constant", "-fno-tree-loop-distribute-patterns",
          "-fno-math-errno", "-ffp-contract=off", "-Wall", "-Wextra", "-Werror", "-Wdouble-promotion", "-std=gnu++17"]
LDFLAGS = ["-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16", "-nostdlib", "-nostartfiles", "-static",
           "-Wl,--gc-sections", "-Wl,--emit-relocs", "-Wl,--no-warn-rwx-segments", "-Wl,--orphan-handling=place"]


def run(cmd):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        raise PackError("%s failed:\n%s" % (Path(cmd[0]).name, proc.stdout.strip()))
    if proc.stdout.strip():
        sys.stderr.write(proc.stdout)


def build(args):
    work = Path(tempfile.mkdtemp(prefix="o3m_build_"))
    objs = []
    for i, src in enumerate(args.src):
        obj = work / ("%d_%s.o" % (i, Path(src).stem))
        cmd = [args.cxx] + CFLAGS + [args.opt] + ["-I%s" % SDK_DIR] + ["-I%s" % d for d in args.include] + ["-D%s" % d for d in args.define] + ["-c", src, "-o", str(obj)]
        run(cmd)
        objs.append(str(obj))
    elf_path = Path(args.elf) if args.elf else work / "module.elf"
    ld = [args.cxx] + LDFLAGS
    if args.hot_at is not None:  # before -T: DEFINED() in the script only sees a symbol given earlier on the command line
        ld.append("-Wl,--defsym=O3M_HOT_AT=%d" % args.hot_at)
    if args.cold_at is not None:
        ld.append("-Wl,--defsym=O3M_COLD_AT=%d" % args.cold_at)
    ld.append("-Wl,-T,%s" % (SDK_DIR / "o3module.ld"))
    run(ld + objs + ["-o", str(elf_path)])
    return elf_path


def layout_tag_arg(args):
    """OS-FLASHDIET: the layout tag of word 44: --layout-tag-file (the build's printed model::kImportLayoutTag) or --layout-tag, else the pinned default."""
    if getattr(args, "layout_tag_file", None):
        return int(Path(args.layout_tag_file).read_text().strip(), 0)
    if getattr(args, "layout_tag", None) is not None:
        return args.layout_tag
    return IMPORT_LAYOUT_TAG


def cmd_build(args):
    elf_path = build(args)
    if args.hot_at is not None or args.cold_at is not None:
        Path(args.out).write_bytes(raw_image(elf_path.read_bytes()))
        return 0
    return write_pack(elf_path.read_bytes(), args.out, args.version, args.caps, layout_tag_arg(args))


def write_pack(elf_bytes, out, version=0, caps=0, layout_tag=IMPORT_LAYOUT_TAG):
    blob, info = pack_elf(elf_bytes, version, caps, layout_tag)
    Path(out).write_bytes(blob)
    print("o3m_pack: %s: %s, %d B file: hot %d B, cold %d B, %d relocations, state %d B, %d parameters, %s%s" % (
        out, info["name"], info["size"], info["hot"], info["cold"], info["relocs"], info["state"], len(info["params"]), format_version(version),
        ", capabilities %#x" % caps if caps else ""))
    return 0


def cmd_info(args):
    data = Path(args.file).read_bytes()
    if len(data) < HEADER_SIZE:
        raise PackError("shorter than a header")
    f = HEADER.unpack_from(data)
    names = "magic header_size target kind abi_major abi_minor flags hot_size image_size desc_offset state_bytes reloc_count param_count view_count module_version capabilities r2b image_crc r3a r3b r3c".split()
    for n, v in zip(names, f):
        print("%-14s 0x%08X (%d)" % (n, v, v))
    print("%-14s %s" % ("version", format_version(f[VERSION_FIELD])))
    return 0


# ---- self-test: synthetic ELFs ----------------------------------------------------------------------------------------------
def make_elf(sections, symbols, rels=(), e_type=ET_EXEC, machine=EM_ARM, rela=False):
    """A minimal linked ELF. sections: list of (name, type, flags, addr, bytes_or_size); symbols: (name, value, size, info, shndx);
    rels: (target section name, offset, type, symbol index). Section 0 is NULL; the order after it follows the arguments."""
    shstr = b"\0"
    names = {}

    def add_name(n):
        nonlocal shstr
        names[n] = len(shstr)
        shstr += n.encode() + b"\0"
    for n in [s[0] for s in sections] + [".symtab", ".strtab", ".shstrtab"] + ["%s.%s" % (".rela" if rela else ".rel", t) for t in sorted({r[0] for r in rels})]:
        add_name(n)
    strtab = b"\0"
    symtab = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)
    for n, value, size, info, shndx in symbols:
        symtab += struct.pack("<IIIBBH", len(strtab), value, size, info, 0, shndx)
        strtab += n.encode() + b"\0"
    blobs = []  # (name, type, flags, addr, data, link, info, entsize)
    for n, t, fl, addr, content in sections:
        blobs.append((n, t, fl, addr, content if t != SHT_NOBITS else b"", 0, 0, 0, content if t == SHT_NOBITS else len(content)))
    symtab_index = len(blobs) + 1
    blobs.append((".symtab", SHT_SYMTAB, 0, 0, symtab, symtab_index + 1, 0, 16, len(symtab)))
    blobs.append((".strtab", SHT_STRTAB, 0, 0, strtab, 0, 0, 0, len(strtab)))
    blobs.append((".shstrtab", SHT_STRTAB, 0, 0, shstr, 0, 0, 0, len(shstr)))
    for t in sorted({r[0] for r in rels}):
        target = [i for i, s in enumerate(sections) if s[0] == t][0] + 1
        body = b"".join(struct.pack("<II", o, (si << 8) | ty) + (struct.pack("<I", 0) if rela else b"") for tn, o, ty, si in rels if tn == t)
        blobs.append(("%s.%s" % (".rela" if rela else ".rel", t), SHT_RELA if rela else SHT_REL, 0x40, 0, body, symtab_index, target, 12 if rela else 8, len(body)))
    out = bytearray(52)
    headers = [struct.pack("<IIIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for n, t, fl, addr, content, link, info, entsize, size in blobs:
        while len(out) % 4:
            out.append(0)
        offset = len(out)
        out += content
        headers.append(struct.pack("<IIIIIIIIII", names[n], t, fl, addr, offset, size, link, info, 4, entsize))
    while len(out) % 4:
        out.append(0)
    shoff = len(out)
    for h in headers:
        out += h
    # the section name table is the one after the symbol and string tables
    struct.pack_into("<16sHHIIIIIHHHHHH", out, 0, b"\x7fELF\x01\x01\x01" + b"\0" * 9, e_type, machine, 1, 0, 0, shoff, 0, 52, 0, 0, 40,
                     len(headers), symtab_index + 2)
    return bytes(out)


HOT_LEN = 16


def good_module():
    """A synthetic module: 16 bytes of hot code; cold (image offsets from 16): the descriptor (64 bytes), one parameter (20), two strings."""
    hot = bytes(range(HOT_LEN))
    cold = bytearray(112)
    # name at image 100, params at 80, the functions at hot offsets 0..10 with the Thumb bit
    cold[0:64] = DESC.pack(1, 0, 1, 64, 100, 64, 1, 80, 0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0, 0)
    cold[64:84] = PARAM.pack(104, 0.0, 1.0, 0.5, 0)
    cold[84:88] = b"Syn\0"
    cold[88:92] = b"P0\0\0"
    return hot, bytes(cold)


def cold_rels():
    """The descriptor's pointers as relocations (r_offset is a link address: the cold part starts at HOT_LEN)."""
    rels = [(".o3m_cold", HOT_LEN + 16, R_ARM_ABS32, 2), (".o3m_cold", HOT_LEN + 28, R_ARM_ABS32, 2)]
    rels += [(".o3m_cold", HOT_LEN + 32 + 4 * i, R_ARM_ABS32, 3) for i in range(6)]
    rels += [(".o3m_cold", HOT_LEN + 64, R_ARM_ABS32, 2)]
    return rels


def good_module_18():
    """A synthetic module of ABI 1.8: the 80-byte descriptor (pitch, one page, labels), one INT parameter 0..1 with two labels. Cold offsets: descriptor 0, parameter 80, page 100, label entry 108,
    the two label pointers 116, strings from 124 (Syn, P0, Pg, Lo, Hi)."""
    hot = bytes(range(HOT_LEN))
    cold = bytearray(144)
    a = HOT_LEN
    cold[0:64] = DESC.pack(1, 8, 1, 80, a + 124, 64, 1, a + 80, 0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0, 0)
    cold[64:80] = DESC_EXT.pack(0x0D, a + 100, 1, a + 108)
    cold[80:100] = PARAM.pack(a + 128, 0.0, 1.0, 0.0, PARAM_INT)
    cold[100:108] = PAGE.pack(a + 132, 0)
    cold[108:116] = LABELS.pack(a + 116, 2)
    cold[116:124] = struct.pack("<II", a + 136, a + 140)
    for at, text in ((124, b"Syn"), (128, b"P0"), (132, b"Pg"), (136, b"Lo"), (140, b"Hi")):
        cold[at:at + len(text)] = text
    return hot, bytes(cold)


def cold_rels_18():
    a = HOT_LEN
    rels = [(".o3m_cold", a + 16, R_ARM_ABS32, 2), (".o3m_cold", a + 28, R_ARM_ABS32, 2)]
    rels += [(".o3m_cold", a + 32 + 4 * i, R_ARM_ABS32, 3) for i in range(6)]
    rels += [(".o3m_cold", a + 64, R_ARM_ABS32, 3)]  # pitch
    rels += [(".o3m_cold", a + off, R_ARM_ABS32, 2) for off in (68, 76, 80, 100, 112, 116, 120)]  # pages, labels, the parameter's name, the page's name, the label table, its two entries
    return rels


def build_synthetic_18(mutate=None, **kw):
    hot, cold = good_module_18()
    cold = bytearray(cold)
    if mutate:
        mutate(cold)
    sections = [(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 6, HOT_LEN, bytes(cold))]
    symbols = [("", 0, 0, 3, 1), ("o3m_entry", HOT_LEN, 80, (1 << 4) | 1, 2), ("fn", 1, 2, (1 << 4) | 2, 1)]
    return make_elf(sections, symbols, cold_rels_18(), **kw)


def good_module_app(minor=9, ui_size=None, keys=0x07):
    """A synthetic app (kind 4, one view) of ABI 1.<minor> with the 80-byte descriptor and a views' table (OS-KEYROWDLG S5): `ui_size` is its struct_size (default: O3M_UI_SIZE(minor), 20 up to 1.8, 24
    from 1.9), `keys` its sixth word. Cold offsets: descriptor 0 (80 bytes), table 80 (24 bytes of room), name 104."""
    hot = bytes(range(HOT_LEN))
    cold = bytearray(112)
    a = HOT_LEN
    cold[0:64] = DESC.pack(1, minor, KIND_APP, 80, a + 104, 64, 0, 0, 0x01, 0, 0, 0, 0, 0x03, a + 80, 1)
    cold[64:80] = DESC_EXT.pack(0, 0, 0, 0)
    cold[80:100] = UI.pack(ui_size if ui_size is not None else (24 if minor >= KEYS_ABI_MINOR else 20), 0, 0, 0x05, 0)
    cold[100:104] = UI_KEYS.pack(keys)
    cold[104:108] = b"App\0"
    return hot, bytes(cold)


def cold_rels_app():
    a = HOT_LEN
    rels = [(".o3m_cold", a + 16, R_ARM_ABS32, 2), (".o3m_cold", a + 56, R_ARM_ABS32, 2)]    # the name, the views' table
    rels += [(".o3m_cold", a + 32, R_ARM_ABS32, 3), (".o3m_cold", a + 52, R_ARM_ABS32, 3)]    # init, shutdown
    rels += [(".o3m_cold", a + 92, R_ARM_ABS32, 3), (".o3m_cold", a + 100, R_ARM_ABS32, 3)]  # the table's view_draw and keys
    return rels


def build_synthetic_app(minor=9, ui_size=None, keys=0x07, **kw):
    hot, cold = good_module_app(minor, ui_size, keys)
    rels = cold_rels_app()
    if keys == 0:
        rels = [r for r in rels if r[1] != HOT_LEN + 100]  # a NULL keys word is no pointer
    sections = [(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 6, HOT_LEN, cold)]
    symbols = [("", 0, 0, 3, 1), ("o3m_entry", HOT_LEN, 80, (1 << 4) | 1, 2), ("fn", 1, 2, (1 << 4) | 2, 1)]
    return make_elf(sections, symbols, rels, **kw)


def build_synthetic(mutate=None, rels=None, extra_sections=(), symbols_extra=(), **kw):
    hot, cold = good_module()
    cold = bytearray(cold)
    if mutate:
        mutate(cold)
    sections = [(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 6, HOT_LEN, bytes(cold))] + list(extra_sections)
    symbols = [("", 0, 0, 3, 1), ("o3m_entry", HOT_LEN, 64, (1 << 4) | 1, 2), ("fn", 1, 2, (1 << 4) | 2, 1)] + list(symbols_extra)
    return make_elf(sections, symbols, cold_rels() if rels is None else rels, **kw)


def good_module_importer():
    """A synthetic importer (kind 3, ABI 1.11, OS-FLASHDIET I1/I2): hot 16 bytes; cold (image offsets from 16): the 84-byte descriptor at 0,
    the importer's table at 84 (32 bytes, eight words), three format entries at 116, strings from 128 (Imp, mod, s3m, med). No parameters, no views,
    no sound entries (render, set_param, note_on, note_off are 0, as an importer's may be)."""
    hot = bytes(range(HOT_LEN))
    cold = bytearray(160)
    a = HOT_LEN
    cold[0:64] = DESC.pack(1, IMPORTER_ABI_MINOR, KIND_IMPORTER, 84, a + 128, 64, 0, 0, 0x01, 0, 0, 0, 0, 0x09, 0, 0)
    cold[64:80] = DESC_EXT.pack(0, 0, 0, 0)
    cold[80:84] = DESC_IMP.pack(a + 84)
    cold[84:116] = IMPORTER.pack(IMPORTER.size, a + 116, 3, 0x01, 0x03, 0x05, 0x07, 0x09)
    cold[116:128] = struct.pack("<3I", a + 132, a + 136, a + 140)
    for at, text in ((128, b"Imp"), (132, b"mod"), (136, b"s3m"), (140, b"med")):
        cold[at:at + len(text)] = text
    return hot, bytes(cold)


def cold_rels_importer():
    a = HOT_LEN
    # the descriptor's pointers (name, init, shutdown, importer), the table's formats pointer, the three extensions
    return [(".o3m_cold", a + 16, R_ARM_ABS32, 2), (".o3m_cold", a + 32, R_ARM_ABS32, 3), (".o3m_cold", a + 44, R_ARM_ABS32, 3),
            (".o3m_cold", a + 80, R_ARM_ABS32, 2), (".o3m_cold", a + 88, R_ARM_ABS32, 2),
            (".o3m_cold", a + 116, R_ARM_ABS32, 2), (".o3m_cold", a + 120, R_ARM_ABS32, 2), (".o3m_cold", a + 124, R_ARM_ABS32, 2)]


def build_synthetic_importer(mutate=None, rels=None, **kw):
    hot, cold = good_module_importer()
    cold = bytearray(cold)
    if mutate:
        mutate(cold)
    sections = [(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 6, HOT_LEN, bytes(cold))]
    symbols = [("", 0, 0, 3, 1), ("o3m_entry", HOT_LEN, 84, (1 << 4) | 1, 2), ("fn", 1, 2, (1 << 4) | 2, 1)]
    return make_elf(sections, symbols, cold_rels_importer() if rels is None else rels, **kw)


def good_module_fx():
    """A synthetic effect of ABI 1.12 (OS-INSFX-ABI2 A1): the 100-byte descriptor with the fourth tail (render_st in the hot part, WIDENS, one preset), two parameters (0..1 and a KEY, INT 0..16, 17 values
    named by no label). Cold offsets: descriptor 0, parameters 100 and 120, the preset 140, its two values 152, strings from 172 (Fx, P0, Key, Pre)."""
    hot = bytes(range(HOT_LEN))
    cold = bytearray(192)
    a = HOT_LEN
    cold[0:64] = DESC.pack(1, STEREO_ABI_MINOR, KIND_EFFECT, 100, a + 172, 64, 2, a + 100, 0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0, 0)
    cold[64:80] = DESC_EXT.pack(0, 0, 0, 0)
    cold[80:84] = DESC_IMP.pack(0)
    cold[84:100] = DESC_FX.pack(0x0D, FX_WIDENS, a + 140, 1)
    cold[100:120] = PARAM.pack(a + 176, 0.0, 1.0, 0.5, 0)
    cold[120:140] = PARAM.pack(a + 180, 0.0, 16.0, 0.0, PARAM_INT | PARAM_KEY)
    cold[140:152] = PRESET.pack(a + 184, a + 152, 2)
    cold[152:160] = struct.pack("<ff", 0.25, 3.0)
    for at, text in ((172, b"Fx"), (176, b"P0"), (180, b"Key"), (184, b"Pre")):
        cold[at:at + len(text)] = text
    return hot, bytes(cold)


def cold_rels_fx():
    a = HOT_LEN
    rels = [(".o3m_cold", a + off, R_ARM_ABS32, 2) for off in (16, 28, 92, 100, 120, 140, 144)]  # the name, the parameters, the presets, the parameters' names, the preset's name and values
    rels += [(".o3m_cold", a + 32 + 4 * i, R_ARM_ABS32, 3) for i in range(6)]                    # init .. shutdown
    rels += [(".o3m_cold", a + 84, R_ARM_ABS32, 3)]                                               # render_st
    return rels


def build_synthetic_fx(mutate=None, rels=None, **kw):
    hot, cold = good_module_fx()
    cold = bytearray(cold)
    if mutate:
        mutate(cold)
    sections = [(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 6, HOT_LEN, bytes(cold))]
    symbols = [("", 0, 0, 3, 1), ("o3m_entry", HOT_LEN, 100, (1 << 4) | 1, 2), ("fn", 1, 2, (1 << 4) | 2, 1)]
    return make_elf(sections, symbols, cold_rels_fx() if rels is None else rels, **kw)


def self_test():
    failures = []

    def expect_ok(label, elf, version=0, caps=0):
        try:
            return pack_elf(elf, version, caps)
        except PackError as exc:
            failures.append("%s: unexpected refusal: %s" % (label, exc))
            return None

    def expect_refused(label, elf, fragment, version=0, caps=0):
        try:
            pack_elf(elf, version, caps)
        except PackError as exc:
            if fragment not in str(exc):
                failures.append("%s: refused for another reason: %s (wanted '%s')" % (label, exc, fragment))
            return
        failures.append("%s: was accepted" % label)

    def poke(offset, value):
        def f(cold):
            struct.pack_into("<I", cold, offset, value)
        return f

    got = expect_ok("good", build_synthetic())
    if got:
        blob, info = got
        h = HEADER.unpack_from(blob)
        if h[0] != MAGIC or h[7] != 16 or h[8] != 128 or h[9] != 16 or info["relocs"] != 9 or info["name"] != "Syn" or info["params"] != ["P0"]:
            failures.append("good: header %r info %r" % (h, info))
        if len(blob) != HEADER_SIZE + 128 + 9 * 4:
            failures.append("good: size %d" % len(blob))
        zeroed = blob[:48] + b"\0\0\0\0" + blob[52:HEADER_SIZE]  # the header with image_crc (bytes 48..51) read as zero
        if zlib.crc32(blob[HEADER_SIZE:], zlib.crc32(zeroed)) & 0xFFFFFFFF != h[CRC_FIELD]:
            failures.append("good: crc")
        if h[VERSION_FIELD] != 0 or blob[36:40] != b"\0\0\0\0" or info["version"] != 0:
            failures.append("good: an unversioned module has a version word")
        table = struct.unpack_from("<9I", blob, HEADER_SIZE + 128)
        if list(table) != sorted(table) or table[0] != HOT_LEN + 16:
            failures.append("good: reloc table %r" % (table,))

    expect_refused("not elf", b"hello", "not an ELF")
    expect_refused("relocatable", build_synthetic(e_type=1), "not a linked executable")
    expect_refused("wrong machine", build_synthetic(machine=3), "not an ARM ELF")
    expect_refused("rela", build_synthetic(rela=True), "RELA")
    expect_refused("bss", build_synthetic(extra_sections=[(".o3m_forbidden_data", SHT_NOBITS, 3, 256, 8)]), "writable data")
    expect_refused("data", build_synthetic(extra_sections=[(".o3m_forbidden_data", SHT_PROGBITS, 3, 256, b"\1\2\3\4")]), "writable data")
    expect_refused("ctors", build_synthetic(extra_sections=[(".o3m_forbidden_init", SHT_PROGBITS, 3, 256, b"\0\0\0\0")]), "constructor")
    expect_refused("stray section", build_synthetic(extra_sections=[(".mystery", SHT_PROGBITS, 2, 512, b"\0\0\0\0")]), "does not place")

    word = HOT_LEN + 96  # a free word in the cold part's padding
    undef = [("memcpy", 0, 0, 0x10, 0)]
    expect_refused("undefined symbol", build_synthetic(symbols_extra=undef, rels=cold_rels() + [(".o3m_cold", word, R_ARM_ABS32, 4)]), "undefined symbol 'memcpy'")
    expect_refused("undefined call", build_synthetic(symbols_extra=undef, rels=cold_rels() + [(".o3m_hot", 0, 10, 4)]), "undefined symbol 'memcpy'")
    expect_refused("absolute symbol", build_synthetic(symbols_extra=[("fixed", 0x1000, 0, 0x10, SHN_ABS)], rels=cold_rels() + [(".o3m_cold", word, R_ARM_ABS32, 4)]),
                   "absolute address")
    expect_refused("movw", build_synthetic(rels=cold_rels() + [(".o3m_cold", word, 47, 2)]), "R_ARM_THM_MOVW_ABS_NC")
    expect_refused("unknown reloc", build_synthetic(rels=cold_rels() + [(".o3m_cold", word, 200, 2)]), "unsupported relocation")
    expect_refused("call across", build_synthetic(rels=cold_rels() + [(".o3m_hot", 0, 10, 2)]), "crosses between the hot and the cold part")
    expect_refused("bad symbol index", build_synthetic(rels=cold_rels() + [(".o3m_cold", word, R_ARM_ABS32, 99)]), "does not exist")
    expect_refused("unaligned abs", build_synthetic(rels=cold_rels() + [(".o3m_cold", word + 1, R_ARM_ABS32, 2)]), "unaligned")
    expect_refused("abs past image", build_synthetic(rels=cold_rels() + [(".o3m_cold", 4000, R_ARM_ABS32, 2)]), "out-of-image")
    expect_refused("abs value past image", build_synthetic(mutate=poke(96, 5000), rels=cold_rels() + [(".o3m_cold", word, R_ARM_ABS32, 2)]), "outside the image")
    expect_ok("pc-relative inside one part", build_synthetic(rels=cold_rels() + [(".o3m_hot", 0, 10, 3)]))

    expect_refused("bad struct_size", build_synthetic(mutate=poke(12, 56)), "struct_size")
    expect_refused("struct_size 80 below 1.8", build_synthetic(mutate=poke(12, 80)), "struct_size")  # the descriptor's size is per minor
    # ABI 1.8: the 80-byte descriptor, pitch, pages and labels
    got18 = expect_ok("abi 1.8", build_synthetic_18())
    if got18 and (got18[1]["abi"] != (1, 8) or got18[1]["relocs"] != 16 or got18[1]["name"] != "Syn"):
        failures.append("abi 1.8: info %r" % (got18[1],))
    expect_refused("1.8 with the 1.7 size", build_synthetic_18(mutate=poke(12, 64)), "struct_size")
    expect_ok("1.8 without pitch, pages and labels", build_synthetic_18(mutate=lambda c: c.__setitem__(slice(64, 80), DESC_EXT.pack(0, 0, 0, 0))))
    # ABI 1.9 (OS-KEYROWDLG S5): the views' table is sized per minor, 20 bytes up to 1.8 and 24 from 1.9, and its `keys` word is NULL or a Thumb function inside the image
    got19 = expect_ok("abi 1.9 app", build_synthetic_app(9))
    if got19 and (got19[1]["abi"] != (1, 9) or got19[1]["name"] != "App"):
        failures.append("abi 1.9 app: info %r" % (got19[1],))
    expect_ok("abi 1.9 app without keys", build_synthetic_app(9, keys=0))
    # ABI 1.10 (OS-APPUX S3): the ui table stays 24 bytes; play_notes (64) needs a 1.10 file
    expect_ok("abi 1.10 app", build_synthetic_app(10))
    expect_ok("abi 1.10 app with play_notes", build_synthetic_app(10), 0, 64)
    expect_refused("play_notes in a 1.9 app", build_synthetic_app(9), "play_notes needs ABI 1.10", 0, 64)
    expect_ok("abi 1.8 app with the 20-byte table", build_synthetic_app(8))
    expect_refused("1.9 with the 20-byte table", build_synthetic_app(9, ui_size=20), "struct_size")
    expect_refused("1.8 with the 24-byte table", build_synthetic_app(8, ui_size=24), "struct_size")
    expect_refused("1.9 with a table of 28 bytes", build_synthetic_app(9, ui_size=28), "struct_size")
    expect_refused("1.9 keys not Thumb", build_synthetic_app(9, keys=0x06), "ui.keys")
    expect_refused("pitch not a function", build_synthetic_18(mutate=poke(64, 0x0C)), "pitch")
    expect_refused("pages without a count", build_synthetic_18(mutate=poke(72, 0)), "pages")
    expect_refused("a count without pages", build_synthetic_18(mutate=lambda c: c.__setitem__(slice(64, 80), DESC_EXT.pack(0x0D, 0, 1, HOT_LEN + 108))), "pages")
    expect_refused("five pages", build_synthetic_18(mutate=poke(72, 5)), "pages")
    expect_refused("first page not at 0", build_synthetic_18(mutate=poke(104, 1)), "page 0")
    expect_refused("page name too long", build_synthetic_18(mutate=lambda c: c.__setitem__(slice(132, 140), b"Abcdefgh")), "name of page")
    expect_refused("label count mismatch", build_synthetic_18(mutate=poke(112, 3)), "labels of parameter")
    expect_refused("labels on a float parameter", build_synthetic_18(mutate=poke(96, 0)), "labels of parameter")  # flags 0: not an INT parameter
    expect_refused("label too long", build_synthetic_18(mutate=lambda c: c.__setitem__(slice(136, 144), b"Abcdefgh")), "label")
    expect_refused("labels table past the image", build_synthetic_18(mutate=poke(76, 5000)), "outside the image")
    expect_refused("abi major", build_synthetic(mutate=poke(0, 2)), "module ABI")
    expect_refused("abi minor", build_synthetic(mutate=poke(4, ABI_MINOR + 1)), "module ABI")
    expect_ok("abi 1.1", build_synthetic(mutate=poke(4, 1)))
    expect_ok("abi 1.2", build_synthetic(mutate=poke(4, 2)))
    expect_ok("abi 1.3", build_synthetic(mutate=poke(4, 3)))
    expect_ok("abi 1.4", build_synthetic(mutate=poke(4, 4)))
    expect_ok("abi 1.5", build_synthetic(mutate=poke(4, 5)))
    expect_ok("abi 1.6", build_synthetic(mutate=poke(4, 6)))
    expect_ok("abi 1.7", build_synthetic(mutate=poke(4, 7)))
    # OS-MODVER: the module version. parse_version: the encoding and every refusal.
    for text, want in (("1.2.3", 0x010203), ("0.0.0", 0), ("0.0.1", 1), ("1.0.0", 0x010000), ("65535.255.255", 0xFFFFFFFF), ("10.20.30", 0x0A141E)):
        try:
            if parse_version(text) != want:
                failures.append("version %s parsed as %#x" % (text, parse_version(text)))
        except PackError as exc:
            failures.append("version %s refused: %s" % (text, exc))
    for text in ("", "1", "1.2", "1.2.3.4", "a.b.c", "1.2.x", "-1.0.0", "+1.0.0", "1..3", "01.2.3", "1.02.3", "1.2.03", " 1.2.3", "1.2.3 ", "1.2.3\n", "1,2,3", "0x1.0.0",
                 "65536.0.0", "1.256.0", "1.0.256", "99999999999.0.0", "\u0661.0.0"):
        try:
            parse_version(text)
            failures.append("version %r was accepted" % text)
        except PackError:
            pass
    abi12 = build_synthetic(mutate=poke(4, 2))
    got = expect_ok("version at ABI 1.2", abi12, parse_version("1.2.3"))
    if got:
        blob, info = got
        h = HEADER.unpack_from(blob)
        zeroed = blob[:48] + b"\0\0\0\0" + blob[52:HEADER_SIZE]
        if h[5] != 2 or h[VERSION_FIELD] != 0x010203 or blob[36:40] != bytes([3, 2, 1, 0]) or info["version"] != 0x010203:
            failures.append("version: the header does not carry it at byte 36 (little endian): %r" % (h,))
        if (h[15], h[16], h[18], h[19], h[20]) != (0, 0, 0, 0, 0) or blob[40:48] != bytes(8):
            failures.append("version: a reserved word is not zero")
        if zlib.crc32(blob[HEADER_SIZE:], zlib.crc32(zeroed)) & 0xFFFFFFFF != h[CRC_FIELD]:
            failures.append("version: the CRC does not cover the header with the version")
        other = pack_elf(abi12, parse_version("1.2.4"))[0]
        if other[48:52] == blob[48:52]:
            failures.append("version: two versions, one CRC")
    got = expect_ok("no version at ABI 1.2", abi12)
    if got and (HEADER.unpack_from(got[0])[VERSION_FIELD] != 0 or got[0][36:40] != bytes(4)):
        failures.append("version: a module built without one has a version word")
    expect_ok("the highest version at ABI 1.2", abi12, parse_version("65535.255.255"))
    expect_refused("version on ABI 1.0", build_synthetic(), "needs ABI 1.2", parse_version("1.0.0"))
    expect_refused("version on ABI 1.1", build_synthetic(mutate=poke(4, 1)), "needs ABI 1.2", parse_version("0.0.1"))
    expect_ok("no version on ABI 1.1 is the file it always was", build_synthetic(mutate=poke(4, 1)), parse_version("0.0.0"))

    # OS-H1: the capability word. An instrument or an effect cannot ask; a module below ABI 1.3 cannot; an unknown name or bit is refused; parse_caps maps the names to the header bits.
    for text, want in (("", 0), ("midi_in", 1), ("read_project", 2), ("audio_tap", 4), ("edit_project", 8), ("export_files", 16), ("midi_in,edit_project", 9), ("midi_in, read_project,edit_project", 11),
                       ("read_project,export_files", 18), ("play_notes", 64), ("read_project,play_notes", 66)):
        if parse_caps(text) != want:
            failures.append("capabilities %r parsed as %#x" % (text, parse_caps(text)))
    for text in ("midi_out", "Midi_In", "nothing"):
        try:
            parse_caps(text)
            failures.append("capability %r was accepted" % text)
        except PackError:
            pass
    expect_refused("caps on an instrument", build_synthetic(mutate=poke(4, 3)), "an app's", 0, 1)
    expect_refused("caps below ABI 1.3", build_synthetic(mutate=poke(4, 2)), "an app's", 0, 1)
    expect_refused("kind", build_synthetic(mutate=poke(8, 5)), "kind")
    expect_refused("kind 3 below ABI 1.11 (the importer kind is published at 1.11)", build_synthetic_importer(mutate=lambda c: (poke(4, 10)(c), poke(12, 80)(c))), "needs ABI 1.11")
    imp = expect_ok("kind 3, an importer (OS-FLASHDIET I1: ABI 1.11, the importer's table, word 44 the layout tag)", build_synthetic_importer())
    if imp:
        h = HEADER.unpack_from(imp[0])
        if h[TAG_FIELD] != IMPORT_LAYOUT_TAG or imp[1]["layout_tag"] != IMPORT_LAYOUT_TAG:
            failures.append("importer: word 44 is %#x, the tag is %#x" % (h[TAG_FIELD], IMPORT_LAYOUT_TAG))
        if h[3] != KIND_IMPORTER or h[5] != IMPORTER_ABI_MINOR:
            failures.append("importer: header kind/abi %r" % (h[3:6],))
        if not _TAG_FILE.exists() and IMPORT_LAYOUT_TAG != 0x21022380:
            failures.append("the fallback layout tag pin is %#x, not 0x21022380" % IMPORT_LAYOUT_TAG)
    imp2 = expect_ok("kind 3 with --layout-tag", build_synthetic_importer())
    if imp2:
        blob2, info2 = pack_elf(build_synthetic_importer(), 0, 0, 0x12345678)
        if HEADER.unpack_from(blob2)[TAG_FIELD] != 0x12345678 or info2["layout_tag"] != 0x12345678:
            failures.append("--layout-tag: word 44 is %#x, not 0x12345678" % HEADER.unpack_from(blob2)[TAG_FIELD])
    # word 44 is 0 for every other kind, at every minor
    for label, elf in (("the good module", build_synthetic()), ("a 1.8 module", build_synthetic_18()), ("an app", build_synthetic_app())):
        got_kind = expect_ok(label, elf)
        if got_kind and HEADER.unpack_from(got_kind[0])[TAG_FIELD] != 0:
            failures.append("%s: word 44 is %#x, not 0" % (label, HEADER.unpack_from(got_kind[0])[TAG_FIELD]))
    expect_refused("an instrument with an importer table", build_synthetic_importer(mutate=poke(8, KIND_INSTRUMENT)), "kind 3's")
    for label, mut, fragment in (
        ("an importer with no table", poke(80, 0), "importer"),
        ("an importer with a wrong table size", lambda c: struct.pack_into("<I", c, 84, 20), "table size"),
        ("an importer with no formats", lambda c: struct.pack_into("<I", c, 88, 0), "format_count"),
        ("an importer with too many formats", lambda c: struct.pack_into("<I", c, 92, 9), "format_count"),
        ("an importer with the formats in the hot part", lambda c: struct.pack_into("<I", c, 88, 4), "misplaced"),
        ("an importer with a missing function (begin)", lambda c: struct.pack_into("<I", c, 84 + 12, 0), "begin"),
        ("an importer with a missing function (report)", lambda c: struct.pack_into("<I", c, 84 + 24, 0), "report"),
        ("an importer with a missing function (close)", lambda c: struct.pack_into("<I", c, 84 + 28, 0), "close"),
        ("an importer with a non-Thumb function", lambda c: struct.pack_into("<I", c, 84 + 16, 4), "Thumb"),
        ("an importer with a function below the image's base (review note 3: the 0 <= bound)", lambda c: struct.pack_into("<I", c, 84 + 12, 0xFFFFFFFC | 1), "begin"),
        ("an importer with an extension below the image's base", lambda c: struct.pack_into("<I", c, 116, 0xFFFFFFFC), "outside the image"),
        ("an importer with an unterminated extension", lambda c: c.__setitem__(slice(132, 144), b"m" * 12), "NUL-terminated"),
    ):
        expect_refused(label, build_synthetic_importer(mutate=mut), fragment)
    # ABI 1.12 (OS-INSFX-ABI2 A1): the 100-byte descriptor and its fourth tail
    fx = expect_ok("abi 1.12 effect (render_st, WIDENS, a preset, a KEY parameter)", build_synthetic_fx())
    if fx:
        h = HEADER.unpack_from(fx[0])
        if h[5] != STEREO_ABI_MINOR or h[3] != KIND_EFFECT or fx[1]["abi"] != (1, 12) or fx[1]["params"] != ["P0", "Key"]:
            failures.append("abi 1.12: header %r info %r" % (h, fx[1]))
    expect_ok("1.12 with no tail at all (a mono effect with no presets)", build_synthetic_fx(mutate=lambda c: c.__setitem__(slice(84, 100), DESC_FX.pack(0, 0, 0, 0)), rels=[r for r in cold_rels_fx() if r[1] not in (HOT_LEN + 84, HOT_LEN + 92)]))
    expect_ok("1.12 instrument with presets and no render_st", build_synthetic_fx(mutate=lambda c: (poke(8, KIND_INSTRUMENT)(c), poke(84, 0)(c), poke(88, 0)(c), poke(120 + 16, 2)(c))))
    inst = lambda c: (poke(8, KIND_INSTRUMENT)(c), poke(84, 0)(c), poke(120 + 16, 2)(c))
    expect_ok("1.13 instrument with O3M_FX_RATE (OS-MODRATE)", build_synthetic_fx(mutate=lambda c: (inst(c), poke(4, 13)(c), poke(88, FX_RATE)(c))))
    expect_refused("O3M_FX_RATE on a 1.12 instrument", build_synthetic_fx(mutate=lambda c: (inst(c), poke(88, FX_RATE)(c))), "fx_flags")
    expect_refused("O3M_FX_RATE on an effect", build_synthetic_fx(mutate=lambda c: poke(88, FX_RATE | FX_WIDENS)(c)), "fx_flags")
    expect_refused("O3M_FX_WIDENS on a 1.13 instrument", build_synthetic_fx(mutate=lambda c: (inst(c), poke(4, 13)(c), poke(88, FX_WIDENS)(c))), "fx_flags")
    expect_ok("1.11 module: the 1.12 members are not read, a KEY bit is not honoured",
              build_synthetic_fx(mutate=lambda c: (poke(4, 11)(c), poke(12, 84)(c), poke(8, KIND_INSTRUMENT)(c))))
    expect_refused("1.12 with the 1.11 size", build_synthetic_fx(mutate=poke(12, 84)), "struct_size")
    expect_refused("1.11 with the 1.12 size", build_synthetic_fx(mutate=poke(4, 11)), "struct_size")
    expect_refused("module ABI above the packer's", build_synthetic_fx(mutate=poke(4, ABI_MINOR + 1)), "module ABI")
    for label, mut, fragment in (
        ("render_st not a Thumb function", poke(84, 0x0C), "render_st"),
        ("render_st on an instrument", poke(8, KIND_INSTRUMENT), "render_st"),
        ("an unknown fx_flags bit", poke(88, 3), "fx_flags"),
        ("WIDENS without render_st", lambda c: (poke(84, 0)(c), poke(88, FX_WIDENS)(c)), "fx_flags"),
        ("a KEY with a range of 0..8", lambda c: struct.pack_into("<f", c, 120 + 8, 8.0), "O3M_PARAM_KEY"),
        ("a KEY with a range of 1..16", lambda c: (struct.pack_into("<f", c, 120 + 4, 1.0), struct.pack_into("<f", c, 120 + 12, 1.0)), "O3M_PARAM_KEY"),
        ("a KEY that is not INT", poke(120 + 16, PARAM_KEY), "O3M_PARAM_KEY"),
        ("a KEY on an instrument", lambda c: (poke(8, KIND_INSTRUMENT)(c), poke(84, 0)(c), poke(88, 0)(c)), "O3M_PARAM_KEY"),
        ("two KEYs are legal", lambda c: (struct.pack_into("<f", c, 100 + 8, 16.0), poke(100 + 16, PARAM_INT | PARAM_KEY)(c)), None),
        ("presets with no count", poke(96, 0), "presets / preset_count"),
        ("a count with no presets", lambda c: poke(92, 0)(c), "presets / preset_count"),
        ("33 presets", poke(96, 33), "presets / preset_count"),
        ("a preset table in the hot part", poke(92, 4), "misplaced"),
        ("a preset with no values", poke(148, 0), "preset 0"),
        ("a preset with more values than parameters", poke(148, 3), "preset 0"),
        ("a preset value table in the hot part", poke(144, 4), "preset 0"),
        ("a preset value above its range", lambda c: struct.pack_into("<f", c, 152, 1.5), "is outside"),
        ("a preset value below its range", lambda c: struct.pack_into("<f", c, 156, -1.0), "is outside"),
        ("a NaN preset value", lambda c: struct.pack_into("<f", c, 152, float("nan")), "is outside"),
        ("an infinite preset value", lambda c: struct.pack_into("<f", c, 156, float("inf")), "is outside"),
        ("a preset with an empty name", lambda c: c.__setitem__(184, 0), "empty or non-printable"),
        ("a preset with an unterminated name", lambda c: c.__setitem__(slice(184, 192), b"A" * 8), "NUL-terminated"),
        ("a preset name of 16 characters", lambda c: (poke(140, HOT_LEN + 172)(c), c.__setitem__(slice(172, 188), b"A" * 16)), "NUL-terminated"),
    ):
        if fragment is None:  # an accepted shape (two KEYs; the third is refused by the C++ tests, which have a three-parameter fixture)
            expect_ok(label, build_synthetic_fx(mutate=mut))
        else:
            expect_refused(label, build_synthetic_fx(mutate=mut), fragment)
    expect_refused("app of ABI 1.0", build_synthetic(mutate=poke(8, 4)), "kind")
    expect_refused("app with no view", build_synthetic(mutate=lambda c: (poke(4, 1)(c), poke(8, 4)(c))), "an app has no view")
    expect_refused("state 0", build_synthetic(mutate=poke(20, 0)), "state_bytes")
    expect_refused("state huge", build_synthetic(mutate=poke(20, 0x10000000)), "state_bytes")
    expect_refused("too many params", build_synthetic(mutate=poke(24, 33)), "parameters, the limit")
    expect_refused("init null", build_synthetic(mutate=poke(32, 0)), "o3m_entry.init")
    expect_refused("init not thumb", build_synthetic(mutate=poke(32, 2)), "o3m_entry.init")
    expect_refused("render outside", build_synthetic(mutate=poke(36, 0x10001)), "outside the image")
    expect_refused("param default", build_synthetic(mutate=lambda c: struct.pack_into("<f", c, 64 + 12, 5.0)), "default")
    expect_refused("empty name", build_synthetic(mutate=lambda c: c.__setitem__(slice(84, 88), b"\0\0\0\0")), "module name")
    expect_refused("unterminated name", build_synthetic(mutate=lambda c: c.__setitem__(slice(84, 112), b"A" * 28)), "NUL-terminated")
    expect_refused("params with no count", build_synthetic(mutate=poke(24, 0)), "param_count is 0")
    expect_refused("ui table without views", build_synthetic(mutate=poke(56, 108)), "o3m_entry.view_count")
    expect_refused("views without a ui table", build_synthetic(mutate=poke(60, 2)), "o3m_entry.view_count")
    expect_refused("too many views", build_synthetic(mutate=lambda c: (poke(56, 108)(c), poke(60, 9)(c))), "o3m_entry.view_count")

    def with_views(draw=0x01, size=20, count=2, ui=108):
        def f(c):
            poke(56, ui)(c)
            poke(60, count)(c)
            c[92:112] = UI.pack(size, 0, 0, draw, 0)
        return f
    ui_rels = cold_rels() + [(".o3m_cold", HOT_LEN + 56, R_ARM_ABS32, 2), (".o3m_cold", HOT_LEN + 104, R_ARM_ABS32, 3)]
    good_views = expect_ok("views", build_synthetic(mutate=with_views(), rels=ui_rels))
    if good_views and (HEADER.unpack_from(good_views[0])[13] != 2 or good_views[1]["views"] != 2):
        failures.append("views: the header does not carry the view count")
    expect_refused("ui view_draw null", build_synthetic(mutate=with_views(draw=0), rels=ui_rels), "o3m_entry.ui.view_draw")
    expect_refused("ui struct size", build_synthetic(mutate=with_views(size=16), rels=ui_rels), "o3m_entry.ui.view_draw")
    expect_refused("ui misplaced", build_synthetic(mutate=with_views(ui=2), rels=ui_rels), "o3m_entry.ui is misplaced")

    hot, cold = good_module()
    expect_refused("no entry", make_elf([(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 6, HOT_LEN, cold)], [("", 0, 0, 3, 1)], []), "o3m_entry")
    expect_refused("no cold", make_elf([(".o3m_hot", SHT_PROGBITS, 6, 0, hot)], [("", 0, 0, 3, 1)], []), ".o3m_cold")
    expect_refused("cold not after hot", make_elf([(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 6, 32, cold)], [("", 0, 0, 3, 1)], []),
                   "does not follow")
    expect_refused("writable cold", make_elf([(".o3m_hot", SHT_PROGBITS, 6, 0, hot), (".o3m_cold", SHT_PROGBITS, 7, HOT_LEN, cold)], [("", 0, 0, 3, 1)], []), "writable")
    # Cut the file everywhere: every refusal is a PackError, never another exception.
    good = build_synthetic()
    for cut in range(0, len(good), 5):
        try:
            pack_elf(good[:cut])
        except PackError:
            pass
        except Exception as exc:  # the test's point
            failures.append("truncated at %d raised %r" % (cut, exc))
            break
    for f in failures:
        print("FAIL", f)
    print("o3m_pack self-test: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


def version_arg(text):
    try:
        return parse_version(text)
    except PackError as exc:
        raise argparse.ArgumentTypeError(str(exc))


def caps_arg(text):
    try:
        return parse_caps(text)
    except PackError as exc:
        raise argparse.ArgumentTypeError(str(exc))


def main(argv):
    if argv[1:2] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--cxx", required=True)
    b.add_argument("--src", action="append", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--elf")
    b.add_argument("-I", dest="include", action="append", default=[])
    b.add_argument("-D", dest="define", action="append", default=[])
    b.add_argument("--opt", default="-Os")
    b.add_argument("--hot-at", type=lambda v: int(v, 0))
    b.add_argument("--cold-at", type=lambda v: int(v, 0))
    b.add_argument("--version", type=version_arg, default=0, help="the module's version X.Y.Z (default 0.0.0: unversioned); needs a module that declares ABI 1.2")
    b.add_argument("--caps", type=caps_arg, default=0, help="an app's capabilities, comma separated (midi_in, read_project, edit_project, audio_tap, export_files); needs an app that declares ABI 1.3 (audio_tap 1.4, export_files 1.7)")
    b.add_argument("--layout-tag", type=lambda v: int(v, 0), default=None, help="OS-FLASHDIET: the importer layout tag for header word 44 of a kind 3 file (default: --layout-tag-file, else the pinned IMPORT_LAYOUT_TAG)")
    b.add_argument("--layout-tag-file", default=None, help="a file holding the tag printed from model::kImportLayoutTag (the build writes it)")
    p = sub.add_parser("pack")
    p.add_argument("--elf", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--version", type=version_arg, default=0, help="the module's version X.Y.Z (default 0.0.0: unversioned)")
    p.add_argument("--caps", type=caps_arg, default=0, help="an app's capabilities, comma separated (midi_in, read_project, edit_project)")
    p.add_argument("--layout-tag", type=lambda v: int(v, 0), default=None, help="OS-FLASHDIET: the importer layout tag for word 44 of a kind 3 file (default: --layout-tag-file, else the pinned IMPORT_LAYOUT_TAG)")
    p.add_argument("--layout-tag-file", default=None, help="a file holding the tag printed from model::kImportLayoutTag")
    r = sub.add_parser("raw")
    r.add_argument("--elf", required=True)
    r.add_argument("--out", required=True)
    i = sub.add_parser("info")
    i.add_argument("file")
    args = ap.parse_args(argv[1:])
    try:
        if args.cmd == "build":
            return cmd_build(args)
        if args.cmd == "pack":
            return write_pack(Path(args.elf).read_bytes(), args.out, args.version, args.caps, layout_tag_arg(args))
        if args.cmd == "raw":
            Path(args.out).write_bytes(raw_image(Path(args.elf).read_bytes()))
            return 0
        return cmd_info(args)
    except PackError as exc:
        print("o3m_pack: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
