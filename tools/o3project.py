#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference reader and writer of the native project format (stdlib only; run it with `python3 -I`).

The format is specified in docs/formats/o3-project.md; the C++ side is formats/native. This module is an
independent implementation of the same bytes: ctest `project_python` writes fixtures with C++, reads and
rewrites them here, reads the result with C++ again and compares bytes and a text dump.

  o3project.py limits            print the limits table (`name=value`), compared with `o3pj_tool limits`
  o3project.py dump FILE         print the project as text (the format of `o3pj_tool dump`)
  o3project.py rewrite IN OUT    parse IN, write it again to OUT
  o3project.py check FILE        parse FILE; exit 0 when it is a valid project, else print the error

As a library: parse(data) -> project dict, build(project) -> bytes, dump(project) -> str. A project is plain
dicts and lists; text fields are bytes, floats are carried as their 32-bit pattern (never converted), steps are
7-byte records, so nothing is changed on the way through. FormatError carries the same typed kinds as the C++
reader: truncated, bad magic, bad CRC, newer major version, limit exceeded, malformed.
"""
import struct
import sys
import zlib

# Same names and order as `o3pj_tool limits` (tests/tools/o3pj_tool.cpp); model/limits.h is the source.
LIMITS = [
    ("tracks", 16),
    ("default_active_tracks", 12),
    ("max_output_bus", 16),
    ("min_pattern_length", 1),
    ("max_pattern_length", 128),
    ("default_pattern_length", 32),
    ("max_pattern_id", 255),
    ("max_patterns", 128),
    ("max_song_positions", 255),
    ("max_instruments", 64),
    ("max_modules", 16),
    ("module_param_bytes", 256),
    ("envelopes", 6),
    ("lfos", 6),
    ("slices", 48),
    ("project_name_max", 32),
    ("pattern_name_max", 30),
    ("track_name_max", 20),
    ("instrument_name_max", 32),
    ("sample_path_max", 96),
    ("module_name_max", 31),
    ("max_unknown_chunks", 16),
    ("unknown_pool_bytes", 16384),
    ("min_tempo_x100", 4000),
    ("max_tempo_x100", 80000),
    ("default_tempo_x100", 13000),
    ("min_swing", 25),
    ("max_swing", 75),
    ("default_swing", 50),
    ("max_percent", 100),
    ("max_file_bytes", 4 << 20),
    ("chunk_version", 0x0100),
    ("inst_version", 0x0101),
    # OS-004d: the project-level state (model/project_state.h) and INST 1.2
    ("inst_version_engine", 0x0102),
    # OS-INSTUI S8b + S9: INST 1.3 (the MIDI instrument's fields and VEL>, docs/formats/INST-1.3-diff.md)
    ("inst_version_midi", 0x0103),
    # OS-LFOSINE: INST 1.4, the 1.3 layout with no new byte; the version only says that an LFO shape of 5 is Sine (docs/specs/OS-LFOSINE.md)
    ("inst_version_lfo_sine", 0x0104),
    # OS-PERFFX P1 (decision D5): PERF and SCEN 1.1, written only when a slot holds the Perform FX 20 or 21
    ("state_version_fx", 0x0101),
    # OS-APPUX S3: APPM 1.1, written only when the project holds more than app_param_sets_v10 apps (formats/src/native_state.h kAppsVersionV11)
    ("apps_version_v11", 0x0101),
    ("midi_ports", 2),
    ("midi_cc_slots", 6),
    ("max_midi_offset_dms", 1000),
    ("stem_slots", 11),
    ("mix_strips", 17),
    ("perf_slots", 12),
    ("max_perf_fx", 21),  # OS-PERFFX P1: PERF / SCEN 1.1 (1.0 ended at 19: PERF_FX_MAX_V10)
    ("max_app_param_sets", 12),  # OS-APPUX S3: APPM 1.1 (1.0 held at most app_param_sets_v10)
    ("app_param_sets_v10", 6),
    ("app_params_bytes_max", 1015),  # the longest APPM payload: one writer piece
    ("max_app_params", 32),
    # OS-PERSIST2: the session state (model/project_state.h): NOTE, MUTE, SCEN, SNFX
    ("max_note_root", 11),
    ("max_note_scale", 11),
    ("max_note_octave", 9),
    ("max_note_jump", 32),
    ("max_mute_mode", 2),
    ("perf_scenes", 3),
    ("send_fx_rows", 12),
    ("send_fx_rows_max", 32),
]
L = dict(LIMITS)

CHUNK_VERSION = L["chunk_version"]
INST_VERSION = L["inst_version"]  # INST is written as 1.1 (OS-002c): 1.0 plus the sample rate, appended
INST_VERSION_ENGINE = L["inst_version_engine"]  # OS-004d: 1.2 appends the playback engine (u8); written only for an instrument whose engine is not automatic (0)
STATE_VERSION_FX = L["state_version_fx"]  # OS-PERFFX P1: PERF / SCEN 1.1 (no new byte: the version says that the Perform FX 20 BITDEPTH and 21 OVERDRV are valid); written only when a slot (of a stored scene) holds one
APPS_VERSION_V11 = L["apps_version_v11"]  # OS-APPUX S3: APPM 1.1 (the same layout, more than 6 apps); mirrors C++ state_chunks::kAppsVersionV11, compared by the limits table
PERF_FX_MAX_V10 = 19  # the last Perform FX id of PERF / SCEN 1.0 (formats/src/native_state.h kPerfFxMaxV10); 1.1 goes to L["max_perf_fx"]
INST_VERSION_LFO_SINE = L["inst_version_lfo_sine"]  # OS-LFOSINE: 1.4 = the 1.3 layout whole; written only for an instrument with an LFO of shape 5 (Sine)
INST_VERSION_MIDI = L["inst_version_midi"]  # OS-INSTUI S8b + S9: 1.3 appends twelve bytes after the engine byte (which is then always present); written only for an instrument one of whose twelve is not its default
# The twelve bytes of INST 1.3 in file order: velocity, program, out, pb range, offset, CC A..F, VEL>: (largest value, default) of each, and the smallest value of velocity (1).
INST13 = [(127, 127), (128, 0), (3, 0), (24, 2), (100, 0), (131, 0), (131, 0), (131, 0), (131, 0), (131, 0), (131, 0), (100, 100)]
INST13_DEFAULT = [d for _, d in INST13]
INST_ENGINE_MAX = 4  # model::kInstEngineMax: 0 auto, 1 Modern, 2 Amiga, 3 12-bit, 4 8-bit; a larger value reads as 0
STEP_BYTES = 7
EMPTY_STEP = bytes([0, 0, 255, 0, 0, 0, 0])
SAMPLER_HEAD = struct.Struct(">16B8HI2I")  # u8 x16, u16 x8, wavetable window, cutoff and resonance bits
ENVELOPE = struct.Struct(">IHHHHIHBB")
LFO = struct.Struct(">BBI")
SLICES = struct.Struct(">48H")
SAMPLER_BYTES = SAMPLER_HEAD.size + 6 * ENVELOPE.size + 6 * LFO.size + SLICES.size
assert SAMPLER_BYTES == 296

RANKS = {b"PHDR": 1, b"TRAK": 2, b"MODR": 3, b"SONG": 4, b"PATT": 5, b"INST": 6, b"SETS": 7, b"LRNB": 8,
         b"MIDI": 9, b"USBS": 10, b"MIXR": 11, b"PERF": 12, b"APPM": 13, b"NOTE": 14, b"MUTE": 15, b"SCEN": 16, b"SNFX": 17, b"LAW ": 18, b"CRC ": 19}

# The project-level state of OS-004d (docs/formats/o3-project.md section 3, model/project_state.h): the chunks MIDI, USBS, MIXR, PERF, APPM.
USB_PROFILE_CHANNELS = [2, 8, 10, 11, 0, 4, 6]  # usb::Profile P2, P8, P10, P11, (P4S: not stored), P4, P6 (usb::profile_channels)
USB_PROFILE_P8, USB_PROFILE_P4, USB_PROFILE_P6 = 1, 5, 6
USB_VERSION_COUNT = 0x0101  # OS-USBOUTS: USBS 1.1 for 4 or 6 outputs: P8 with 8 slots (the real ones first, then P8's factory masks), then the real count
USB_P8_FACTORY = [0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x1000, 0x2000]
MIX_ROUTE_MASK, MIX_ROUTE_USB = 63, 32
STATE_PARTS = (b"MIDI", b"USBS", b"MIXR", b"PERF", b"APPM")

# The session state of OS-PERSIST2 (docs/formats/o3-project.md section 3, "The session state"): NOTE, MUTE, SCEN, SNFX, read tolerantly (a value out of its range is its default).
SESSION_PARTS = (b"NOTE", b"MUTE", b"SCEN", b"SNFX")
LAW_PARTS = (b"LAW ",)  # a name so that a test can emulate the build before it
# OS-BUG-PASTE24: the LAW chunk (docs/formats/o3-project.md, "The LAW chunk"): the instrument volume law, 0 the 1.2+ law (no chunk) or 1 the 1.1.14 table; a value above 1 is Malformed in every minor.
NOTE_FIELDS = [(11, 0), (11, 0), (9, 3), (1, 0), (32, 1)]  # root, scale, octave, lock, jump: (largest value, default)
MUTE_MODE_MAX = 2
SEND_FX = [(0, 1, 0), (0, 1, 0), (0, 18, 0), (1, 3500, 500), (0, 70, 50), (0, 100, 100), (0, 1, 0), (0, 100, 50), (0, 100, 50), (0, 100, 50), (0, 100, 80), (0, 1, 0)]  # (min, max, default) by row

# The project's settings and Learn bindings (OS-002d, docs/formats/o3-settings.md): the settings of kind Project, a copy of model/params.h's
# table (field: default, min, max; all four are off / on or two-way choices), and the rules of core/learn.h that a binding in the project's store follows.
PROJECT_SETTINGS = [(1, 0, 1), (0, 0, 1), (0, 0, 1)]  # anti-aliasing, limiter mode, sends mode
KIND_INSTRUMENT, KIND_PROJECT = 1, 4
LEARN_SLOTS = 32
SETS_ENTRY_CAP = LRNB_ENTRY_CAP = 64  # what a chunk of this minor may hold (formats/settings_chunks.h); this version keeps LEARN_SLOTS bindings


class FormatError(Exception):
    def __init__(self, kind, detail, offset=0):
        super().__init__("%s (%s)" % (kind, detail))
        self.kind = kind
        self.detail = detail
        self.offset = offset


def new_project():
    return {
        "name": b"", "tempo": L["default_tempo_x100"], "swing": L["default_swing"],
        "deflen": L["default_pattern_length"], "active": L["default_active_tracks"],
        "master": [86, 86, 86, 86, 0],
        "tracks": [{"type": 0, "flags": 0, "vol": 86, "pan": 50, "bus": 0, "name": b""}
                   for _ in range(L["tracks"])],
        "modules": [],      # (name, abi major, abi minor)
        "song": [],         # pattern ids
        "patterns": {},     # id -> {"lengths": [16 ints], "name", "rows": [bytes of 128 steps] * 16}
        "instruments": {},  # slot -> dict, see parse_inst
        "settings": [d for d, _, _ in PROJECT_SETTINGS],  # the value of each setting of kind Project
        "learn": [None] * LEARN_SLOTS,                    # (param id, cc, channel, mode, flags) or None, by slot
        "midi": None,       # OS-004d, each None while the project has no such part: dicts, see parse_state
        "usb": None,
        "mix": None,
        "perf": None,
        "note": None,       # OS-PERSIST2, None while the project has no such part: a list of 5, a list of 16, (latch, [(stored, slots) x 3]), a list of 12
        "mute": None,
        "scenes": None,
        "send": None,
        "law": 0,           # OS-BUG-PASTE24: the instrument volume law; the LAW chunk is written only when it is 1
        "apps": [],         # (module name, abi major, abi minor, [value bits]) oldest first
        "unknown": [],      # (fourcc bytes, payload)
        "unknown_dropped": 0,
        "unknown_dropped_bytes": 0,
    }


def be16(data, at):
    return (data[at] << 8) | data[at + 1]


def be32(data, at):
    return (data[at] << 24) | (data[at + 1] << 16) | (data[at + 2] << 8) | data[at + 3]


# ---- validation (the same rules as model/project.cpp and model/instrument.cpp) ---------------------------

def finite(bits):
    return (bits & 0x7F800000) != 0x7F800000


def safe_char(c):
    return c >= 0x20 and c != 0x7F and c != 0x5C and c != 0x3A  # not control, DEL, backslash, colon


def safe_relative_path(text):
    if not text:
        return True
    if not all(safe_char(c) for c in text):
        return False
    return all(part not in (b"", b".", b"..") for part in text.split(b"/"))


def safe_module_name(text):
    if not 1 <= len(text) <= L["module_name_max"]:
        return False
    if not all(safe_char(c) and c != 0x2F for c in text):
        return False
    return text not in (b".", b"..")


def invalid_instrument(i, module_count):
    """None when valid, else why not (same strings as the C++ validator where a test looks at them)."""
    if i["kind"] not in (1, 2, 3):
        return "bad instrument kind"
    if i["engine"] > INST_ENGINE_MAX:
        return "bad playback engine"
    if len(i["m13"]) != len(INST13) or i["m13"][0] < 1 or any(not 0 <= x <= hi for x, (hi, _) in zip(i["m13"], INST13)):
        return "bad MIDI field"
    if not safe_relative_path(i["path"]):
        return "unsafe sample path"
    if i["kind"] == 2:
        if not 1 <= i["midi"] <= 16:
            return "bad MIDI channel"
    elif i["midi"] != 0:
        return "MIDI channel on a non-MIDI instrument"
    if i["kind"] == 3:
        if i["module"] >= module_count:
            return "module index outside the module table"
        if len(i["blob"]) > L["module_param_bytes"]:
            return "module parameter blob too long"
    elif i["module"] != 0xFF or i["blob"]:
        return "module data on a non-module instrument"
    s = i["head"]
    if not finite(s[25]) or not finite(s[26]):
        return "sampler: non-finite filter value"
    if any(not finite(e[0]) or not finite(e[5]) for e in i["env"]):
        return "sampler: non-finite envelope value"
    if any(not finite(x[2]) for x in i["lfo"]):
        return "sampler: non-finite LFO value"
    if s[0] > 1:
        return "sampler: bad sample type"
    if s[1] > 7:
        return "sampler: bad play mode"
    if s[2] > 2:
        return "sampler: bad filter type"
    if max(s[6], s[7], s[8], s[9], s[10]) > L["max_percent"]:
        return "sampler: level above 100"
    if not 4 <= s[11] <= 16:
        return "sampler: bad bit depth"
    if s[12] > L["slices"] or s[13] >= L["slices"]:
        return "sampler: bad slice count"
    if s[14] > 2 or s[15] > 2:
        return "sampler: bad granular mode"
    return None


class Cursor:
    """Reads fields from a payload; running past the end latches `bad` and returns zeros."""

    def __init__(self, data):
        self.data = data
        self.at = 0
        self.bad = False

    def take(self, n):
        if self.bad or n > len(self.data) - self.at:
            self.bad = True
            return b"\0" * n
        out = self.data[self.at:self.at + n]
        self.at += n
        return out

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack(">H", self.take(2))[0]

    def u32(self):
        return struct.unpack(">I", self.take(4))[0]

    def text(self, maximum):
        n = self.u8()
        if n > maximum:
            self.bad = True
            return b""
        out = self.take(n)
        if b"\0" in out:
            self.bad = True
        return out

    def left(self):
        return len(self.data) - self.at


def malformed(detail):
    raise FormatError("malformed", detail)


# ---- reading ----------------------------------------------------------------------------------------------

# Payload length bounds per known chunk as version 1.0 defines them (the C++ reader's kPhdrFixed..kInstMax). The
# lower bound holds for every minor version; the upper one only for the chunk's own version (INST 1.1 is four bytes
# longer than 1.0; a newer minor appends fields, which are skipped). Checked after the chunk's version, which comes first.
LENGTH_RANGE = {
    b"PHDR": (19, 19 + 32),
    b"TRAK": (3, 3 + 16 * (6 + 20)),
    b"MODR": (3, 3 + 16 * (3 + 31)),
    b"SONG": (3, 3 + 255),
    b"PATT": (5 + 2, 5 + 30 + 16 * (2 + 128 * STEP_BYTES)),
    b"INST": (306, 306 + 32 + 96 + 256),
    b"SETS": (4, 4 + SETS_ENTRY_CAP * 8),
    b"LRNB": (4, 4 + LRNB_ENTRY_CAP * 8),
    b"MIDI": (20, 20),
    b"USBS": (5, 5 + 2 * 11),  # 1.1 adds one byte (the count), below
    b"MIXR": (25, 25),
    b"PERF": (99, 99),
    b"APPM": (3, 3 + 6 * (4 + 31 + 4 * 32)),  # 1.0; APPM 1.1 (OS-APPUX S3) is bounded by 1015, below
    b"NOTE": (7, 7),
    b"MUTE": (3, 3 + 16),
    b"SCEN": (295, 295),
    b"SNFX": (3, 3 + 2 * 32),
    b"LAW ": (3, 3),
    b"CRC ": (6, 6),
}


def parse(data):
    """Reads a whole file. Errors found in a file that is both truncated and invalid may be reported in a
    different order than the C++ reader (which works piece by piece); a valid or merely truncated file gets the
    same answer from both."""
    data = bytes(data)
    p = new_project()
    if len(data) < 12:
        raise FormatError("truncated", "the file ends before its FORM does", len(data))
    if data[0:4] != b"FORM" or data[8:12] != b"O3PJ":
        raise FormatError("bad magic", "not an O3PJ project file")
    flen = be32(data, 4)
    if flen > L["max_file_bytes"] - 8:
        raise FormatError("limit exceeded", "file larger than the limit")
    if flen < 4 or flen & 1:
        malformed("bad FORM length")
    form_left = flen - 4
    pos = 12
    rank = 0
    expected = [0, 0, 0]  # patterns, instruments, modules announced by PHDR
    seen_patterns = seen_instruments = 0
    unknown_used = 0
    law = 0
    while True:
        if form_left == 0:
            malformed("the FORM ends without a CRC chunk")
        if form_left < 8:
            malformed("chunk header overruns the FORM")
        chunk_at = pos
        if pos + 8 > len(data):
            raise FormatError("truncated", "the file ends before its FORM does", len(data))
        cid = data[pos:pos + 4]
        clen = be32(data, pos + 4)
        pos += 8
        form_left -= 8
        pad = clen & 1
        if clen > form_left or clen + pad > form_left:
            malformed("chunk overruns the FORM")
        form_left -= clen + pad
        if rank == 0 and cid != b"PHDR":
            malformed("the first chunk must be PHDR")
        r = RANKS.get(cid, 0)
        if r:
            if r < rank or (r == rank and r not in (5, 6)):
                malformed("chunk out of order or repeated")
            rank = r
            if clen < 2:
                malformed("chunk length out of range")
            if pos + 2 > len(data):
                raise FormatError("truncated", "the file ends before its FORM does", len(data))
            version = be16(data, pos)
            newer = check_version(version, cid)  # before any length rule: a newer major may have any layout
            lo, hi = LENGTH_RANGE[cid]
            if cid == b"INST" and (version & 0xFF) >= 1:
                hi += 4  # the sample rate of version 1.1
            if cid == b"INST" and (version & 0xFF) >= 2:
                hi += 1  # the playback engine of version 1.2
            if cid == b"INST" and (version & 0xFF) >= 3:
                hi += len(INST13)  # the MIDI fields and VEL> of version 1.3
            if cid == b"USBS" and (version & 0xFF) >= 1:
                hi += 1  # version 1.1: the channel count of a 4 or 6 output project (OS-USBOUTS)
            if cid == b"APPM" and (version & 0xFF) >= 1:
                hi = L["app_params_bytes_max"]  # version 1.1: up to 12 apps, as many as one writer piece holds (OS-APPUX S3)
            if clen < lo or (not newer and clen > hi):
                malformed("chunk length out of range")
        elif not all(0x20 <= c <= 0x7E for c in cid):
            malformed("chunk id is not four printable characters")
        if pos + clen > len(data):
            raise FormatError("truncated", "the file ends before its FORM does", len(data))
        payload = data[pos:pos + clen]
        pos += clen

        if cid == b"PHDR":
            parse_phdr(p, payload, expected)
        elif cid == b"TRAK":
            parse_trak(p, payload)
        elif cid == b"MODR":
            parse_modr(p, payload)
        elif cid == b"SONG":
            parse_song(p, payload)
        elif cid == b"PATT":
            parse_patt(p, payload)
            seen_patterns += 1
        elif cid == b"INST":
            parse_inst(p, payload)
            seen_instruments += 1
        elif cid == b"SETS":
            parse_sets(p, payload, check_version(be16(payload, 0), cid))
        elif cid == b"LRNB":
            parse_lrnb(p, payload, check_version(be16(payload, 0), cid))
        elif cid in STATE_PARTS:
            parse_state(p, cid, payload, check_version(be16(payload, 0), cid))
        elif cid in SESSION_PARTS:
            parse_session(p, cid, payload, check_version(be16(payload, 0), cid))
        elif cid in LAW_PARTS:
            law = parse_law(payload, check_version(be16(payload, 0), cid))  # applied when the CRC has matched, like the C++ reader does at Done
        elif cid == b"CRC ":
            c = Cursor(payload)
            check_version(c.u16())
            stored = c.u32()
            if stored != (zlib.crc32(data[:chunk_at]) & 0xFFFFFFFF):
                raise FormatError("bad CRC", "the CRC chunk does not match the file")
            if form_left != 0:
                malformed("data after the CRC chunk")
            if (seen_patterns, seen_instruments, len(p["modules"])) != tuple(expected):
                malformed("chunk counts differ from the header")
            p["law"] = law
            return p
        else:
            unknown_used = keep_unknown(p, cid, payload, unknown_used)
        if pad:
            if pos >= len(data):
                raise FormatError("truncated", "the file ends before its FORM does", len(data))
            if data[pos] != 0:
                malformed("pad byte is not zero")
            pos += 1


def keep_unknown(p, cid, payload, used):
    """Keeps an unknown chunk within the bounded pool, else counts it as dropped; returns the pool use."""
    if len(p["unknown"]) >= L["max_unknown_chunks"] or len(payload) > L["unknown_pool_bytes"] - used:
        p["unknown_dropped"] += 1
        p["unknown_dropped_bytes"] = min(p["unknown_dropped_bytes"] + len(payload), 0xFFFFFFFF)
        return used
    p["unknown"].append((cid, payload))
    return used + len(payload)


def known_minor(cid):
    """The newest minor version of a chunk this reader knows: INST 1.4, PERF and SCEN 1.1, APPM 1.1, USBS 1.1, the others 1.0."""
    if cid == b"INST":
        return INST_VERSION_LFO_SINE & 0xFF
    if cid in (b"PERF", b"SCEN"):
        return STATE_VERSION_FX & 0xFF
    return 1 if cid in (b"APPM", b"USBS") else CHUNK_VERSION & 0xFF


def check_version(version, cid=None):
    major, minor = version >> 8, version & 0xFF
    if major == 0:
        malformed("chunk version 0")
    if major > CHUNK_VERSION >> 8:
        raise FormatError("newer major version", "chunk written by a newer major version of the format")
    return minor > known_minor(cid)  # newer minor: unknown trailing fields are skipped


def finish_fields(c, minor_newer):
    if c.bad or (c.left() != 0 and not minor_newer):
        malformed("chunk fields do not fit (short chunk, text too long or containing NUL)")


def parse_phdr(p, payload, expected):
    c = Cursor(payload)
    newer = check_version(c.u16())
    active, deflen, tempo, swing = c.u8(), c.u8(), c.u32(), c.u8()
    master = [c.u8() for _ in range(5)]
    patterns, instruments, modules = c.u16(), c.u8(), c.u8()
    name = c.text(L["project_name_max"])
    finish_fields(c, newer)
    if patterns > L["max_patterns"] or instruments > L["max_instruments"] or modules > L["max_modules"]:
        raise FormatError("limit exceeded", "the header announces more than the model holds")
    expected[:] = [patterns, instruments, modules]
    if not 1 <= active <= L["tracks"]:
        malformed("PHDR: bad active track count")
    if not L["min_pattern_length"] <= deflen <= L["max_pattern_length"]:
        malformed("PHDR: bad default pattern length")
    if not L["min_tempo_x100"] <= tempo <= L["max_tempo_x100"]:
        malformed("PHDR: tempo out of range")
    if not L["min_swing"] <= swing <= L["max_swing"]:
        malformed("PHDR: swing out of range")
    if max(master[:4]) > L["max_percent"] or master[4] & ~15:
        malformed("PHDR: bad master settings")
    p.update(active=active, deflen=deflen, tempo=tempo, swing=swing, master=master, name=name)


def parse_trak(p, payload):
    c = Cursor(payload)
    newer = check_version(c.u16())
    slots = c.u8()
    if c.bad or not 1 <= slots <= L["tracks"]:
        malformed("TRAK: bad slot count")
    for i in range(slots):
        t = {"type": c.u8(), "flags": c.u8(), "vol": c.u8(), "pan": c.u8(), "bus": c.u8(),
             "name": c.text(L["track_name_max"])}
        if c.bad:
            malformed("chunk fields do not fit (short chunk, text too long or containing NUL)")
        if (t["type"] > 2 or t["flags"] & ~3 or t["vol"] > 100 or t["pan"] > 100
                or t["bus"] > L["max_output_bus"]):
            malformed("TRAK: bad track slot")
        p["tracks"][i] = t
    finish_fields(c, newer)


def parse_modr(p, payload):
    c = Cursor(payload)
    newer = check_version(c.u16())
    count = c.u8()
    if c.bad:
        malformed("chunk fields do not fit (short chunk, text too long or containing NUL)")
    if count > L["max_modules"]:
        raise FormatError("limit exceeded", "MODR: more modules than the table holds")
    for _ in range(count):
        major, minor = c.u8(), c.u8()
        name = c.text(L["module_name_max"])
        if c.bad:
            malformed("chunk fields do not fit (short chunk, text too long or containing NUL)")
        if not safe_module_name(name) or any(m[0] == name for m in p["modules"]):
            malformed("MODR: bad or duplicate module")
        p["modules"].append((name, major, minor))
    finish_fields(c, newer)


def parse_song(p, payload):
    c = Cursor(payload)
    newer = check_version(c.u16())
    count = c.u8()
    ids = c.take(count)
    finish_fields(c, newer)
    if 0 in ids:
        malformed("SONG: pattern id 0")
    p["song"] = list(ids)


def parse_patt(p, payload):
    head = 5
    c = Cursor(payload[:head])
    newer = check_version(c.u16())
    pid, tracks, nlen = c.u8(), c.u8(), c.u8()
    if pid < 1 or not 1 <= tracks <= L["tracks"] or nlen > L["pattern_name_max"]:
        malformed("PATT: bad id, track count or name length")
    c = Cursor(payload[head:])
    name = c.take(nlen)
    if c.bad:
        malformed("PATT ends inside its name")
    if pid in p["patterns"]:
        malformed("PATT: duplicate id or pattern pool full")
    if len(p["patterns"]) >= L["max_patterns"]:
        raise FormatError("limit exceeded", "PATT: duplicate id or pattern pool full")
    if b"\0" in name:
        malformed("PATT: NUL in the name")
    # Tracks the chunk does not store keep the project's default pattern length.
    lengths = [p["deflen"]] * L["tracks"]
    rows = [bytearray(EMPTY_STEP * L["max_pattern_length"]) for _ in range(L["tracks"])]
    for t in range(tracks):
        if c.left() < 2:
            malformed("PATT ends inside its track list")
        length, n = c.u8(), c.u8()
        if not L["min_pattern_length"] <= length <= L["max_pattern_length"]:
            malformed("PATT: bad track length")
        if n > L["max_pattern_length"]:
            malformed("PATT: more than 128 steps in a track")
        if c.left() < n * STEP_BYTES:
            malformed("PATT ends inside a track")
        lengths[t] = length
        rows[t][:n * STEP_BYTES] = c.take(n * STEP_BYTES)
    if c.left() != 0 and not newer:
        malformed("unused bytes at the end of PATT")
    p["patterns"][pid] = {"lengths": lengths, "name": name, "rows": [bytes(r) for r in rows]}


def parse_sets(p, payload, newer):
    """SETS: u16 version, u16 count, count x (u32 ParamId, f32 value). An entry for a setting the project does not have, or with a value
    outside its range (NaN too), is skipped and the setting keeps its default; only the shape of the chunk is an error."""
    c = Cursor(payload)
    c.u16()
    count = c.u16()
    take = min(count, SETS_ENTRY_CAP)  # a newer minor may carry more than this version can name: the rest is skipped
    if c.bad or (count > SETS_ENTRY_CAP and not newer) or (c.left() < take * 8 if newer else c.left() != count * 8):
        malformed("SETS does not fit its entry count")
    for _ in range(take):
        pid = c.u32()
        value = struct.unpack(">f", struct.pack(">I", c.u32()))[0]
        kind, index, field = pid >> 24, (pid >> 16) & 0xFF, pid & 0xFFFF
        if kind != KIND_PROJECT or index != 0 or field >= len(PROJECT_SETTINGS):
            continue
        _, lo, hi = PROJECT_SETTINGS[field]
        if value != value or value < lo or value > hi:
            continue
        p["settings"][field] = int(value + 0.5)  # an integer field holds the nearest whole value


def learn_valid(b):
    pid, cc, channel, mode, flags = b
    kind = pid >> 24
    if kind not in (KIND_INSTRUMENT, KIND_PROJECT) or cc > 127 or channel > 16 or mode > 2 or flags & ~1:
        return False  # kinds Track, Master and Device belong to the device's store
    return not (flags & 1) or (kind == KIND_INSTRUMENT and (pid >> 16) & 0xFF == 0)


def parse_lrnb(p, payload, newer):
    """LRNB: u16 version, u16 count, count x (u32 ParamId, u8 cc, u8 channel, u8 mode, u8 flags). core::learn_plan's rules, entry by entry:
    the same parameter again takes its slot, every other binding that listens to the same message is displaced (the first of them gives its slot
    to a new parameter), a new one with nothing displaced takes the first free slot."""
    c = Cursor(payload)
    c.u16()
    count = c.u16()
    take = min(count, LRNB_ENTRY_CAP)
    if c.bad or (count > LRNB_ENTRY_CAP and not newer) or (c.left() < take * 8 if newer else c.left() != count * 8):
        malformed("LRNB does not fit its entry count")
    slots = p["learn"]
    for _ in range(take):
        b = (c.u32(), c.u8(), c.u8(), c.u8(), c.u8())
        if b[0] == 0 or not learn_valid(b):
            continue
        same = lambda s: s is not None and s[0] == b[0] and (s[4] ^ b[4]) & 1 == 0
        overlap = lambda s: s is not None and s[1] == b[1] and (s[2] == 0 or b[2] == 0 or s[2] == b[2])
        write = next((i for i, s in enumerate(slots) if same(s)), -1)
        clear = [i for i, s in enumerate(slots) if i != write and overlap(s)]  # every other binding that listens to the same message
        if write < 0:
            if clear:
                write = clear.pop(0)
            else:
                write = next((i for i, s in enumerate(slots) if s is None), -1)
        if write < 0:
            continue  # the store is full
        slots[write] = b
        for i in clear:
            slots[i] = None


def s16(c):
    v = c.u16()
    return v - 65536 if v > 32767 else v


def parse_state(p, cid, payload, newer):
    """MIDI, USBS, MIXR, PERF, APPM (OS-004d): strict, like every known chunk: a value out of its range, a count the layout does not fit and (in a
    chunk of this minor) unused bytes are malformed. The rules of model/project_state.cpp's validate_*."""
    c = Cursor(payload)
    version = c.u16()
    minor = version & 0xFF
    short = "chunk fields do not fit (short chunk, text too long or containing NUL)"

    def shape():
        if c.bad or (c.left() != 0 and not newer):
            malformed(short)

    if cid == b"MIDI":
        m = {"out": c.u8(), "note": c.u8(), "chan": c.u8(), "follow": c.u8(), "cc": [c.u8() for _ in range(6)],
             "in_off": [s16(c) for _ in range(2)], "out_off": [s16(c) for _ in range(2)]}
        shape()
        if (m["out"] | m["note"]) & ~3:
            malformed("MIDI: a port outside DIN and USB")
        if m["chan"] > 15 or m["follow"] > 2 or any(n > 127 and n != 255 for n in m["cc"]):
            malformed("MIDI: bad channel, follow or CC number")
        if any(abs(v) > L["max_midi_offset_dms"] for v in m["in_off"] + m["out_off"]):
            malformed("MIDI: offset out of range")
        p["midi"] = m
    elif cid == b"USBS":
        mode, profile, slots = c.u8(), c.u8(), c.u8()
        if c.bad or slots > L["stem_slots"]:
            malformed(short if c.bad else "USBS: more slots than a profile has")
        masks = [c.u16() for _ in range(slots)]
        count = c.u8() if (version & 0xFF) >= 1 else 0  # USBS 1.1 (OS-USBOUTS): the real channel count follows the masks
        shape()
        if mode > 1 or profile > 3:  # the chunk's profile byte is 0..3 (P4S, and the model's P4 / P6, are not stored)
            malformed("USBS: bad mode, profile or slot count")
        if (version & 0xFF) >= 1:
            if profile != USB_PROFILE_P8 or slots != 8 or count not in (4, 6):
                malformed("USBS: bad channel count")
            profile = USB_PROFILE_P4 if count == 4 else USB_PROFILE_P6
            masks = masks[:count]
        if slots != USB_PROFILE_CHANNELS[profile] and (version & 0xFF) == 0:
            malformed("USBS: bad mode, profile or slot count")
        p["usb"] = {"mode": mode, "profile": profile, "masks": masks}
    elif cid == b"MIXR":
        strips = c.u8()
        m = {"route": [c.u8() for _ in range(L["mix_strips"])], "ret": [c.u8() for _ in range(2)], "out_mode": c.u8(), "cue": c.u8(), "in": c.u8()}
        shape()
        if strips != L["mix_strips"]:
            malformed("MIXR: not 17 strips")
        if (any(r & ~MIX_ROUTE_MASK for r in m["route"]) or m["route"][-1] & MIX_ROUTE_USB or max(m["ret"]) > 3 or m["out_mode"] > 1
                or m["cue"] > L["max_percent"] or m["in"] > L["max_percent"]):
            malformed("MIXR: bad route, out mode or level")
        p["mix"] = m
    elif cid == b"PERF":
        slots = c.u8()
        s = []
        for _ in range(L["perf_slots"]):
            s.append((c.u8(), c.u8(), [s16(c) for _ in range(3)]))
        shape()
        if slots != L["perf_slots"]:
            malformed("PERF: not 12 slots")
        # tolerant like the C++ reader: an FX id the chunk's version does not have (above 19 in 1.0, above 21 in 1.1) is none (row and values reset), a row above 3 is off
        fx_max = L["max_perf_fx"] if minor >= 1 else PERF_FX_MAX_V10
        s = [(0, 0, [0, 0, 0]) if fx > fx_max else (fx, 0 if row > 3 else row, vals) for fx, row, vals in s]
        if any(abs(v) > 100 for _, _, vals in s for v in vals):
            malformed("PERF: value out of range")
        p["perf"] = s
    else:  # APPM
        count = c.u8()
        if c.bad or not 1 <= count <= (L["max_app_param_sets"] if (version & 0xFF) >= 1 else L["app_param_sets_v10"]):  # 1.0 held at most 6, 1.1 up to 12
            malformed(short if c.bad else "APPM: bad app count")
        apps = []
        for _ in range(count):
            name = c.text(L["module_name_max"])
            abi_major, abi_minor, n = c.u8(), c.u8(), c.u8()  # (the app's ABI, not the chunk's minor above)
            if c.bad or not 1 <= n <= L["max_app_params"]:
                malformed(short if c.bad else "APPM: bad parameter count")
            values = [c.u32() for _ in range(n)]
            if c.bad:
                malformed(short)
            if not safe_module_name(name) or any(a[0] == name for a in apps) or not all(finite(v) for v in values):
                malformed("APPM: bad module name, duplicate app or non-finite value")
            apps.append((name, abi_major, abi_minor, values))
        shape()
        p["apps"] = apps


def parse_law(payload, newer):
    """LAW (OS-BUG-PASTE24): u16 version, u8 law. A law this version does not know is Malformed, not a default: its levels would play up to 24 dB wrong."""
    c = Cursor(payload)
    c.u16()
    law = c.u8()
    finish_fields(c, newer)
    if law > 1:
        malformed("LAW: unknown law")
    return law


def parse_session(p, cid, payload, newer):
    """NOTE, MUTE, SCEN, SNFX (OS-PERSIST2): the shape of the chunk is the only error; a value out of its range is its default (model/project_state.cpp's sanitise_*)."""
    c = Cursor(payload)
    minor = c.u16() & 0xFF
    short = "chunk fields do not fit (short chunk, text too long or containing NUL)"

    def shape():
        if c.bad or (c.left() != 0 and not newer):
            malformed(short)

    if cid == b"NOTE":
        n = [c.u8() for _ in range(5)]
        shape()
        p["note"] = [v if v <= top else default for v, (top, default) in zip(n, NOTE_FIELDS)]
    elif cid == b"MUTE":
        count = c.u8()
        if c.bad or not 1 <= count <= L["tracks"]:
            malformed(short if c.bad else "MUTE: bad track count")
        modes = [c.u8() for _ in range(count)]
        shape()
        p["mute"] = [m if m <= MUTE_MODE_MAX else 0 for m in modes] + [0] * (L["tracks"] - count)  # the tracks the chunk does not have are Immediate
    elif cid == b"SCEN":
        latch, count = c.u8(), c.u8()
        if c.bad:
            malformed(short)
        if count != L["perf_scenes"]:
            malformed("SCEN: not 3 scenes")
        scenes = []
        for _ in range(count):
            stored = c.u8()
            scenes.append((stored, [(c.u8(), c.u8(), [s16(c) for _ in range(3)]) for _ in range(L["perf_slots"])]))
        shape()
        out = []
        for stored, slots in scenes:
            if stored != 1:  # 0, or a value this version does not know: not stored, and the bytes of a scene that is not stored mean nothing
                out.append((0, [(0, 0, [0, 0, 0]) for _ in range(L["perf_slots"])]))
                continue
            fixed = []
            for fx, row, vals in slots:
                if fx > (L["max_perf_fx"] if minor >= 1 else PERF_FX_MAX_V10):  # an FX the chunk's version does not have (a later version's, or 20 and 21 in a 1.0 chunk): none, with its row and values reset
                    fixed.append((0, 0, [0, 0, 0]))
                else:
                    fixed.append((fx, row if row <= 3 else 0, [v if abs(v) <= 100 else 0 for v in vals]))
            out.append((1, fixed))
        p["scenes"] = (latch if latch <= 1 else 1, out)
    else:  # SNFX
        rows = c.u8()
        if c.bad or not 1 <= rows <= L["send_fx_rows_max"]:
            malformed(short if c.bad else "SNFX: bad row count")
        values = [c.u16() for _ in range(rows)]
        shape()
        p["send"] = [(values[i] if lo <= values[i] <= hi else default) if i < rows else default
                     for i, (lo, hi, default) in enumerate(SEND_FX)]  # rows past this version's are skipped, rows the chunk does not have are defaults


def parse_inst(p, payload):
    c = Cursor(payload)
    version = c.u16()
    newer = check_version(version, b"INST")
    index, kind, midi, module = c.u8(), c.u8(), c.u8(), c.u8()
    name = c.text(L["instrument_name_max"])
    path = c.text(L["sample_path_max"])
    head = list(SAMPLER_HEAD.unpack(c.take(SAMPLER_HEAD.size)))
    env = [list(ENVELOPE.unpack(c.take(ENVELOPE.size))) for _ in range(6)]
    lfo = [list(LFO.unpack(c.take(LFO.size))) for _ in range(6)]
    slices = list(SLICES.unpack(c.take(SLICES.size)))
    blob_len = c.u16()
    if not c.bad and blob_len > L["module_param_bytes"]:
        malformed("INST: module parameter blob too long")
    blob = c.take(blob_len)
    rate = c.u32() if (version & 0xFF) >= 1 else 0  # version 1.1 appends the sample rate; 1.0 did not record it
    engine = c.u8() if (version & 0xFF) >= 2 else 0  # version 1.2 appends the playback engine; before it was automatic
    if engine > INST_ENGINE_MAX:
        engine = 0  # an engine a later version added: it plays as automatic here
    m13 = list(INST13_DEFAULT)
    if (version & 0xFF) >= 3:  # version 1.3 appends the MIDI fields and VEL>; a value out of its range is the field's default (a later version may add values)
        got = [c.u8() for _ in INST13]
        m13 = [x if (x >= 1 if n == 0 else True) and x <= hi else d for n, (x, (hi, d)) in enumerate(zip(got, INST13))]
    last_shape = 5 if (version & 0xFF) >= 4 else 4  # OS-LFOSINE: the LFO shapes are 0..4 before INST 1.4 and 0..5 from it; a larger one plays as Triangle (2)
    for x in lfo:
        if x[0] > last_shape:
            x[0] = 2
    finish_fields(c, newer)
    if index >= L["max_instruments"] or kind not in (1, 2, 3):
        malformed("INST: bad slot index or kind")
    if index in p["instruments"]:
        malformed("INST: duplicate slot")
    inst = {"kind": kind, "midi": midi, "module": module, "name": name, "path": path, "rate": rate, "engine": engine, "m13": m13, "head": head,
            "env": env, "lfo": lfo, "slices": slices, "blob": blob}
    why = invalid_instrument(inst, len(p["modules"]))
    if why:
        malformed(why)
    p["instruments"][index] = inst


# ---- writing ----------------------------------------------------------------------------------------------

def chunk(cid, payload):
    return cid + struct.pack(">I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")


def text(value):
    return bytes([len(value)]) + value


def stored_steps(row):
    n = L["max_pattern_length"]
    while n > 0 and row[(n - 1) * STEP_BYTES:n * STEP_BYTES] == EMPTY_STEP:
        n -= 1
    return n


def build_state(p, v):
    """The chunks of the project-level state the project has, in the canonical order MIDI, USBS, MIXR, PERF, APPM (OS-004d)."""
    out = []
    if p["midi"] is not None:
        m = p["midi"]
        out.append(chunk(b"MIDI", v + bytes([m["out"], m["note"], m["chan"], m["follow"]] + m["cc"])
                         + struct.pack(">4h", *m["in_off"], *m["out_off"])))
    if p["usb"] is not None:
        u = p["usb"]
        if u["profile"] in (USB_PROFILE_P4, USB_PROFILE_P6):  # 1.1: P8 with 8 slots, the real masks first, then the P8 factory ones, then the count
            full = u["masks"] + USB_P8_FACTORY[len(u["masks"]):]
            out.append(chunk(b"USBS", struct.pack(">H", USB_VERSION_COUNT) + bytes([u["mode"], USB_PROFILE_P8, 8])
                             + b"".join(struct.pack(">H", x) for x in full) + bytes([len(u["masks"])])))
        else:
            out.append(chunk(b"USBS", v + bytes([u["mode"], u["profile"], len(u["masks"])]) + b"".join(struct.pack(">H", x) for x in u["masks"])))
    if p["mix"] is not None:
        m = p["mix"]
        out.append(chunk(b"MIXR", v + bytes([len(m["route"])] + m["route"] + m["ret"] + [m["out_mode"], m["cue"], m["in"]])))
    if p["perf"] is not None:
        pv = struct.pack(">H", STATE_VERSION_FX) if any(fx > PERF_FX_MAX_V10 for fx, _, _ in p["perf"]) else v  # PERF is 1.1 only when a slot holds the FX 20 or 21
        out.append(chunk(b"PERF", pv + bytes([len(p["perf"])]) + b"".join(
            bytes([fx, row]) + struct.pack(">3h", *vals) for fx, row, vals in p["perf"])))
    if p["apps"]:
        out.append(chunk(b"APPM", (struct.pack(">H", APPS_VERSION_V11) if len(p["apps"]) > L["app_param_sets_v10"] else v) + bytes([len(p["apps"])]) + b"".join(
            text(name) + bytes([major, minor, len(values)]) + b"".join(struct.pack(">I", x) for x in values)
            for name, major, minor, values in p["apps"])))
    return out


def build_session(p, v):
    """The chunks of the session state the project has, in the canonical order NOTE, MUTE, SCEN, SNFX (OS-PERSIST2)."""
    out = []
    if p["note"] is not None:
        out.append(chunk(b"NOTE", v + bytes(p["note"])))
    if p["mute"] is not None:
        out.append(chunk(b"MUTE", v + bytes([len(p["mute"])]) + bytes(p["mute"])))
    if p["scenes"] is not None:
        latch, scenes = p["scenes"]
        sv = struct.pack(">H", STATE_VERSION_FX) if any(stored and fx > PERF_FX_MAX_V10 for stored, slots in scenes for fx, _, _ in slots) else v  # SCEN, likewise, for a stored scene
        out.append(chunk(b"SCEN", sv + bytes([latch, len(scenes)]) + b"".join(
            bytes([stored]) + b"".join(bytes([fx, row]) + struct.pack(">3h", *vals) for fx, row, vals in slots) for stored, slots in scenes)))
    if p["send"] is not None:
        out.append(chunk(b"SNFX", v + bytes([len(p["send"])]) + b"".join(struct.pack(">H", x) for x in p["send"])))
    return out


def build(p):
    v = struct.pack(">H", CHUNK_VERSION)
    chunks = []
    chunks.append(chunk(b"PHDR", v + struct.pack(">BBIB", p["active"], p["deflen"], p["tempo"], p["swing"])
                        + bytes(p["master"]) + struct.pack(">HBB", len(p["patterns"]), len(p["instruments"]),
                                                            len(p["modules"])) + text(p["name"])))
    chunks.append(chunk(b"TRAK", v + bytes([len(p["tracks"])]) + b"".join(
        bytes([t["type"], t["flags"], t["vol"], t["pan"], t["bus"]]) + text(t["name"]) for t in p["tracks"])))
    chunks.append(chunk(b"MODR", v + bytes([len(p["modules"])]) + b"".join(
        bytes([major, minor]) + text(name) for name, major, minor in p["modules"])))
    chunks.append(chunk(b"SONG", v + bytes([len(p["song"])]) + bytes(p["song"])))
    for pid in sorted(p["patterns"]):
        pat = p["patterns"][pid]
        body = v + bytes([pid, len(pat["rows"])]) + text(pat["name"])
        for length, row in zip(pat["lengths"], pat["rows"]):
            n = stored_steps(row)
            body += bytes([length, n]) + row[:n * STEP_BYTES]
        chunks.append(chunk(b"PATT", body))
    for index in sorted(p["instruments"]):
        i = p["instruments"][index]
        # 1.3 only for an instrument one of whose twelve MIDI bytes is not its default, 1.2 (the engine byte) only for an instrument that chose an engine: every other is the 1.1 chunk it always was
        # 1.4 (the 1.3 layout whole) for an instrument with a Sine LFO
        if any(x[0] == 5 for x in i["lfo"]):
            version = INST_VERSION_LFO_SINE
        else:
            version = INST_VERSION_MIDI if i["m13"] != INST13_DEFAULT else (INST_VERSION_ENGINE if i["engine"] else INST_VERSION)
        body = struct.pack(">H", version) + bytes([index, i["kind"], i["midi"], i["module"]]) + text(i["name"]) + text(i["path"])
        body += SAMPLER_HEAD.pack(*i["head"])
        body += b"".join(ENVELOPE.pack(*e) for e in i["env"]) + b"".join(LFO.pack(*x) for x in i["lfo"])
        body += SLICES.pack(*i["slices"]) + struct.pack(">H", len(i["blob"])) + i["blob"] + struct.pack(">I", i["rate"])
        if version >= INST_VERSION_ENGINE:
            body += bytes([i["engine"]])
        if version >= INST_VERSION_MIDI:
            body += bytes(i["m13"])
        chunks.append(chunk(b"INST", body))
    if p["settings"] != [d for d, _, _ in PROJECT_SETTINGS]:  # SETS: every setting, only when one is not its default
        chunks.append(chunk(b"SETS", v + struct.pack(">H", len(p["settings"])) + b"".join(
            struct.pack(">If", (KIND_PROJECT << 24) | f, float(x)) for f, x in enumerate(p["settings"]))))
    used = [b for b in p["learn"] if b is not None]
    if used:  # LRNB: the bindings in use, in slot order
        chunks.append(chunk(b"LRNB", v + struct.pack(">H", len(used)) + b"".join(struct.pack(">IBBBB", *b) for b in used)))
    chunks.extend(build_state(p, v))
    chunks.extend(build_session(p, v))
    if p["law"]:  # LAW: the last known chunk, only for the 1.1.14 table
        chunks.append(chunk(b"LAW ", v + b"\x01"))
    for cid, payload in p["unknown"]:
        chunks.append(chunk(cid, payload))
    body = b"".join(chunks)
    form = b"FORM" + struct.pack(">I", 4 + len(body) + 14) + b"O3PJ" + body
    return form + b"CRC " + struct.pack(">IHI", 6, CHUNK_VERSION, zlib.crc32(form) & 0xFFFFFFFF)


# ---- the chunks of the OG formats (docs/formats/og.md): kept verbatim like any unknown chunk, decoded for the dump --

OGPM_GROUP_LEN = {1: 4, 2: 9, 3: 16, 4: 10, 5: 144, 6: 16, 8: 306, 9: 460}  # delay limiter reverb inputs performance
# velocity (versions 2-7) 1.3 block 1.7 block: the DropGroup bit numbers of formats/og.h
# OGPM 1.1 (OS-004c): group 9 is one byte longer, the byte at 0x80C of a v17 project (open research question, unverified: docs/formats/og.md section 2). Group 9 is the highest
# group, so that byte is the last of the chunk; a newer minor than a reader knows only appends, as for every chunk.
OGPM_TAIL_GROUP = 9
OGPN_MAX_FILE = 16 + 50 * 256


def ogpm_info(payload):
    """(valid, groups, tail) of an OGPM payload (OG project settings): u16 version, u16 group bits, the groups in bit order.
    `tail` is the byte of 0x80C that an OGPM 1.1 chunk holds, 0 when it holds none."""
    if len(payload) < 4:
        return (False, 0, 0)
    version, groups = be16(payload, 0), be16(payload, 2)
    if (version >> 8) != 1 or groups & ~sum(1 << b for b in OGPM_GROUP_LEN):
        return (False, 0, 0)
    expect = 4 + sum(n for b, n in OGPM_GROUP_LEN.items() if groups >> b & 1)
    tail_at = None
    if (version & 0xFF) >= 1 and groups >> OGPM_TAIL_GROUP & 1:
        tail_at, expect = expect, expect + 1
    if len(payload) < expect or ((version & 0xFF) == 0 and len(payload) != expect):
        return (False, 0, 0)
    return (True, groups, payload[tail_at] if tail_at is not None else 0)


def ogpn_info(payload):
    """(valid, file length) of an OGPN payload (the PAMD file as read): u16 version, u32 length, the file without its zeros."""
    if len(payload) < 10:
        return (False, 0)
    version, total = be16(payload, 0), be32(payload, 2)
    if (version >> 8) != 1 or total < 16 or total > OGPN_MAX_FILE or len(payload) - 6 > total or payload[6:10] != b"PAMD":
        return (False, 0)
    return (True, total)


# ---- text dump (the same lines as `o3pj_tool dump`) -------------------------------------------------------

def signed(b):
    return b - 256 if b > 127 else b


def dump(p):
    out = []
    m = p["master"]
    out.append("project name=%s tempo=%d swing=%d deflen=%d active=%d master=%d,%d,%d,%d,%d"
               % (p["name"].hex(), p["tempo"], p["swing"], p["deflen"], p["active"], m[0], m[1], m[2], m[3], m[4]))
    for i, t in enumerate(p["tracks"]):
        out.append("track %d type=%d flags=%d vol=%d pan=%d bus=%d name=%s"
                   % (i, t["type"], t["flags"], t["vol"], t["pan"], t["bus"], t["name"].hex()))
    for i, (name, major, minor) in enumerate(p["modules"]):
        out.append("module %d abi=%d.%d name=%s" % (i, major, minor, name.hex()))
    out.append("song %d" % len(p["song"]) + "".join(" %d" % x for x in p["song"]))
    for pid in sorted(p["patterns"]):
        pat = p["patterns"][pid]
        out.append("pattern %d lens=%s name=%s rows=%s" % (
            pid, ",".join("%d" % n for n in pat["lengths"]), pat["name"].hex(),
            ",".join("%08x" % (zlib.crc32(r) & 0xFFFFFFFF) for r in pat["rows"])))
    for index in sorted(p["instruments"]):
        i = p["instruments"][index]
        h = i["head"]
        out.append("instrument %d kind=%d midi=%d module=%d name=%s path=%s rate=%d engine=%d blob=%d/%08x m13=%s" % (
            index, i["kind"], i["midi"], i["module"], i["name"].hex(), i["path"].hex(), i["rate"], i["engine"], len(i["blob"]),
            zlib.crc32(i["blob"]) & 0xFFFFFFFF, ",".join("%d" % x for x in i["m13"])))
        line = "  sampler %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d" % (
            h[0], h[1], h[2], h[3], signed(h[4]), signed(h[5]), h[6], h[7], h[8], h[9], h[10], h[11], h[12],
            h[13], h[14], h[15])
        line += " | %d %d %d %d %d %d %d %d | %d | %08x %08x" % (
            h[16], h[17], h[18], h[19], h[20], h[21], h[22], h[23], h[24], h[25], h[26])
        for e in i["env"]:
            line += " | env %08x %d %d %d %d %08x %d %d %d" % tuple(e)
        for x in i["lfo"]:
            line += " | lfo %d %d %08x" % tuple(x)
        line += " | slices" + "".join(" %d" % s for s in i["slices"])
        out.append(line)
    for f, (d, _, _) in enumerate(PROJECT_SETTINGS):
        if p["settings"][f] != d:
            out.append("setting %d %d" % (f, p["settings"][f]))
    for slot, b in enumerate(p["learn"]):
        if b is not None:
            out.append("learn %d param=%08x cc=%d channel=%d mode=%d flags=%d" % ((slot,) + tuple(b)))
    if p["midi"] is not None:
        m = p["midi"]
        out.append("midi out=%d note=%d chan=%d follow=%d cc=%s in_off=%d,%d out_off=%d,%d" % (
            m["out"], m["note"], m["chan"], m["follow"], ",".join("%d" % x for x in m["cc"]), m["in_off"][0], m["in_off"][1], m["out_off"][0], m["out_off"][1]))
    if p["usb"] is not None:
        u = p["usb"]
        out.append("usb mode=%d profile=%d slots=%d masks=%s" % (u["mode"], u["profile"], len(u["masks"]), ",".join("%04x" % x for x in u["masks"])))
    if p["mix"] is not None:
        m = p["mix"]
        out.append("mix route=%s ret=%d,%d out_mode=%d cue=%d in=%d" % (",".join("%d" % x for x in m["route"]), m["ret"][0], m["ret"][1], m["out_mode"], m["cue"], m["in"]))
    if p["perf"] is not None:
        for s, (fx, row, vals) in enumerate(p["perf"]):
            out.append("perf %d fx=%d row=%d values=%d,%d,%d" % (s, fx, row, vals[0], vals[1], vals[2]))
    for n, (name, major, minor, values) in enumerate(p["apps"]):
        out.append("app %d name=%s abi=%d.%d values=%s" % (n, name.hex(), major, minor, ",".join("%08x" % x for x in values)))
    if p["note"] is not None:
        out.append("note root=%d scale=%d octave=%d lock=%d jump=%d" % tuple(p["note"]))
    if p["mute"] is not None:
        out.append("mute modes=%s" % ",".join("%d" % x for x in p["mute"]))
    if p["scenes"] is not None:
        latch, scenes = p["scenes"]
        out.append("scenes latch=%d stored=%s" % (latch, ",".join("%d" % stored for stored, _ in scenes)))
        for i, (stored, slots) in enumerate(scenes):
            if stored:
                for s, (fx, row, vals) in enumerate(slots):
                    out.append("scene %d slot %d fx=%d row=%d values=%d,%d,%d" % (i, s, fx, row, vals[0], vals[1], vals[2]))
    if p["send"] is not None:
        out.append("sendfx values=%s" % ",".join("%d" % x for x in p["send"]))
    if p["law"]:
        out.append("law=%d" % p["law"])
    for n, (cid, payload) in enumerate(p["unknown"]):
        out.append("unknown %d id=%s len=%d crc=%08x" % (n, cid.decode("ascii"), len(payload),
                                                        zlib.crc32(payload) & 0xFFFFFFFF))
        if cid == b"OGPM":
            valid, groups, tail = ogpm_info(payload)
            out.append("ogpm %d valid=%d groups=%04x tail=%d" % (n, valid, groups, tail))
        elif cid == b"OGPN":
            valid, total = ogpn_info(payload)
            out.append("ogpn %d valid=%d length=%d" % (n, valid, total))
    out.append("unknown_dropped=%d" % p["unknown_dropped"])
    return "\n".join(out) + "\n"


# ---- command line -------------------------------------------------------------------------------------------

def main(argv):
    if len(argv) >= 2 and argv[1] == "limits":
        sys.stdout.write("".join("%s=%d\n" % item for item in LIMITS))
        return 0
    if len(argv) in (3, 4) and argv[1] in ("dump", "rewrite", "check"):
        try:
            with open(argv[2], "rb") as f:
                project = parse(f.read())
        except FormatError as e:
            sys.stderr.write("o3project.py: load failed: %s (%s)\n" % (e.kind, e.detail))
            return 1
        if argv[1] == "dump" and len(argv) == 3:
            sys.stdout.write(dump(project))
            return 0
        if argv[1] == "rewrite" and len(argv) == 4:
            with open(argv[3], "wb") as f:
                f.write(build(project))
            return 0
        if argv[1] == "check" and len(argv) == 3:
            return 0
    sys.stderr.write("usage: o3project.py limits | dump FILE | rewrite IN OUT | check FILE\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
