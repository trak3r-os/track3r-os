#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generates the descriptor header of a Community OS module from its manifest, and checks a built module against it (stdlib only).

    o3m_gen.py gen    module.json [--out FILE]      the C header: parameter table, enums, entry-point macros (stdout without --out)
    o3m_gen.py check  module.json HEADER            exit 1 when HEADER is not what `gen` writes (a stale generated file)
    o3m_gen.py expect module.json [--out FILE]      what a loader must read back from the built file (one fact a line)
    o3m_gen.py new --kind instrument|effect|effect_stereo|effect_key|app --id ID [--name NAME] --out DIR     a module directory from the template of the kind
    o3m_gen.py dump   FILE.tmod                     the descriptor of a packed module, read with this tool's own reader (JSON)
    o3m_gen.py verify module.json FILE.tmod         exit 1 when the built file's descriptor is not what the manifest says
    o3m_gen.py --self-test

The manifest (module.json) is the specification of a module; the descriptor in its C source is generated from it so that the two cannot drift:

    {
      "name": "TB-303",                  display name, 1..23 printable ASCII characters
      "id": "tb303",                     file stem and C identifier prefix: [a-z][a-z0-9_]*, at most 31 characters
      "kind": "instrument",              instrument | effect | importer | app
      "state_bytes": 400,                the instance block the OS gives init() (zeroed, 16-byte aligned, in SDRAM): what the source's State needs, 1..262144
      "params": [                        at most 32; parameter i is id i in set_param() and canvas->params[i]
        {"name": "Cutoff",               1..15 characters
         "min": 314, "max": 2394, "default": 1000,
         "type": "float",                float (default) | int (whole numbers) | bool (0 or 1: min 0, max 1) | key (ABI 1.12, effects only: the sidechain key, an INT parameter 0 = none, 1..16 = the key track;
                                         min 0, max 16 and default 0 when omitted; no unit, curve, learnable, mod or labels; at most two a module; the OS shows it as OFF, T1..T16)
         "unit": "hz",                   none (default) | percent | hz | ms | db | semitones | seconds | cents
         "curve": "log",                 linear (default) | log (min > 0): the editing curve
         "learnable": true,              default true: MIDI Learn may bind a controller to it
         "mod": false,                   default false: a modulation target
         "labels": ["Saw", "Square"]}    optional, int parameters only (ABI 1.8, OS-INSTUI S10): the text of each value from min to max, max - min + 1 = 2..16 of them, 1..11 characters each;
                                         an instrument's, or an effect's (OS-INSFX-FX2: it makes the effect ABI 1.12, its descriptor then carries the label table through <ID>_ENTRY_FX)
      ],
      "views": [                         at most 8; the order is the view index in view_draw()
        {"name": "filter", "min_size": [160, 100], "preferred_size": [380, 140]}
      ],
      "pitch": true,                     optional, instruments (ABI 1.8, OS-MODPITCH): the module exports a pitch(state, semitones, per_frame) function; the entry macro takes its name (see the generated header)
      "pages": [{"name": "Osc", "first": 0}, ...]   optional, instruments (ABI 1.8, OS-INSTUI S10): the params pages, at most 4, a name of 1..7 characters, "first" the page's first parameter (the first page
                                         starts at 0, ascending, 1..8 parameters a page, together all the parameters)
      "abi": "1.2",                      optional: "1.0" | "1.1" | "1.2" | "1.3" | "1.4" | "1.5" | "1.6" | "1.7" | "1.8" | "1.9" | "1.10" | "1.11" | "1.12" | "1.13". The generator writes the lowest ABI the module needs (see below); "1.2" says the module is shipped with a
                                         version (the packer's --version, OS-MODVER), which only an ABI 1.2 module may carry
      "stereo": "widens",                optional, effects only (ABI 1.12, OS-INSFX-ABI2): "widens" (the effect makes a stereo image from a mono input: the OS calls render_st on every block) or "capable" (render_st
                                         on a block whose input has a side signal, render on the others). Makes the module ABI 1.12; the entry macro <ID>_ENTRY_FX(render_st) takes the stereo render
      "presets": [                       optional, instruments and effects with parameters (ABI 1.12): the factory presets, at most 32; a preset sets every parameter, the ones it does not name take their default
        {"name": "Slow",                 1..15 printable ASCII characters, each name once
         "values": {"Depth": 80}}        parameter name -> value: each name is one of the params, each value finite and inside that parameter's range (whole for int, bool and key)
      ],                                 Makes the module ABI 1.12; the entry macro <ID>_ENTRY_FX (an instrument's, after <ID>_ENTRY_TAIL) points the descriptor at the table
      "capabilities": ["midi_in"],       optional, apps only (ABI 1.3, OS-H1, ADR 0014): midi_in | read_project | edit_project (ABI 1.3), audio_tap (ABI 1.4, OS-H2a), export_files (ABI 1.7, OS-H4). Makes the module ABI 1.3, 1.4 with audio_tap, or 1.7 with export_files; the file carries them
                                         (the packer's --caps, o3m_add_module(... CAPS ...)) and the OS lends the matching host tables
      "voices": 1,                       design-only: ABI 1.1 runs one instance per instrument slot, so only 1 is accepted
      "memory": {"hot_bytes": 2400, "sdram_bytes": 0}     advisory (the packer measures the real ones); emitted as constants, not carried by the file
    }

What of the manifest the file carries (ABI 1.1): name, kind, state_bytes, the parameters (name, range, default, flags: log / int / bool, learn, modulation, unit) and the
NUMBER of views. What it does not (it is emitted as constants for the source's own use and documented as design-only): view names and sizes, voices, the memory
advice. The OS picks the rectangle of a view; `min_size` says what the view needs to be readable.

The header also holds the ABI minor the module needs (0 for an instrument or an effect with no views, which then loads on an OS of ABI 1.0; 1 otherwise; 2 when the manifest
declares "abi": "1.2", 3 when it asks for capabilities or declares "abi": "1.3", 4 when it asks for audio_tap or declares "abi": "1.4", 5 when it declares "abi": "1.5": an app that uses the step and edit-group entries of OS-H3A, 6 when it declares "abi": "1.6": an app that uses the instrument-parameter entries of OS-H3B, 7 when it asks for export_files or declares "abi": "1.7": an app that writes files, or reads a MIDI instrument's channel, 8 when an instrument has "pitch", "pages" or "labels" or declares "abi": "1.8", 9 when an app declares "abi": "1.9": its ui table is 24 bytes (O3M_UI_SIZE) and may carry keys(), OS-KEYROWDLG S5; 10 when it declares "abi": "1.10"; 11 for an importer (kind 3, OS-FLASHDIET I1: always 11, whatever is declared) so that the descriptor does not claim more than the module uses, 12 when an effect has "stereo" or a key parameter, an instrument or effect has "presets", or it declares "abi": "1.12" (OS-INSFX-ABI2).
The descriptor's struct_size is O3M_MODULE_SIZE(minor) (64 bytes up to ABI 1.7, 80 from 1.8, 84 from 1.11, 100 from 1.12): the generated ENTRY_HEAD writes it, never sizeof(o3m_module).
"""
import argparse
import json
import re
import struct
import sys
import zlib
from pathlib import Path

# ---- the ABI's numbers (sdk/o3module.h) ------------------------------------------------------------------------------------------
ABI_MAJOR = 1
ABI_MINOR = 13
STEREO_ABI_MINOR = 12    # OS-INSFX-ABI2 A1: the ABI minor that made the descriptor 100 bytes (render_st, fx_flags, presets, preset_count)
RATE_ABI_MINOR = 13      # OS-MODRATE: the ABI minor that made O3M_FX_RATE valid for an instrument
IMPORTER_ABI_MINOR = 11  # OS-FLASHDIET I1: the ABI minor that published kind 3 and made header word 44 the importer layout tag
CAPS = {"midi_in": 1, "read_project": 2, "audio_tap": 4, "edit_project": 8, "export_files": 16, "play_notes": 64}  # O3M_CAP_* of ABI 1.3, 1.4 (audio_tap), 1.7 (export_files) and 1.10 (play_notes) (o3module.h)
CAPS_ABI_1_4 = 4  # the capability bits that need ABI 1.4
CAPS_ABI_1_7 = 16  # the capability bits that need ABI 1.7
CAPS_ABI_1_10 = 64  # the capability bits that need ABI 1.10
KINDS = {"instrument": 1, "effect": 2, "importer": 3, "app": 4}


# OS-FLASHDIET I1/I2: the importer layout tag (model::kImportLayoutTag, over the whole import-facing model layout), read from
# model/import_layout_tag.txt like o3m_pack.py does; ctest o3m_layout_tag_pin holds that file to the C++ constant.
_TAG_FILE = Path(__file__).resolve().parent.parent / "model" / "import_layout_tag.txt"
try:
    IMPORT_LAYOUT_TAG = int(_TAG_FILE.read_text().strip(), 0)
except OSError:
    IMPORT_LAYOUT_TAG = 0x21022380  # pinned fallback (no repository beside the script)
KIND_CONST = {"instrument": "O3M_KIND_INSTRUMENT", "effect": "O3M_KIND_EFFECT", "importer": "O3M_KIND_IMPORTER", "app": "O3M_KIND_APP"}
UNITS = {"none": 0, "percent": 1, "hz": 2, "ms": 3, "db": 4, "semitones": 5, "seconds": 6, "cents": 7}
UNIT_CONST = {"percent": "O3M_UNIT_PERCENT", "hz": "O3M_UNIT_HZ", "ms": "O3M_UNIT_MS", "db": "O3M_UNIT_DB", "semitones": "O3M_UNIT_SEMITONES",
              "seconds": "O3M_UNIT_SECONDS", "cents": "O3M_UNIT_CENTS"}
F_LOG, F_INT, F_BOOL, F_NO_LEARN, F_MOD, F_KEY = 1, 2, 4, 8, 16, 32  # F_KEY: O3M_PARAM_KEY (ABI 1.12)
UNIT_SHIFT = 8
NAME_MAX, PARAM_NAME_MAX, PARAMS_MAX, VIEWS_MAX = 24, 16, 32, 8
PRESETS_MAX, PRESET_NAME_MAX, KEYS_MAX, KEY_TRACKS = 32, 16, 2, 16  # O3M_PRESETS_MAX, O3M_PRESET_NAME_MAX (with the NUL), at most two KEY parameters, 1..16 = the key track
FX_WIDENS = 1  # O3M_FX_WIDENS
FX_RATE = 2    # O3M_FX_RATE (ABI 1.13, an instrument's: it renders at the half and quarter rate the OS asks for)
STEREO_SHAPES = {"widens": FX_WIDENS, "capable": 0}
MAX_STATE = 256 * 1024
FILE_MAGIC = 0x444D334F
HEADER = struct.Struct("<IHHBBBBIIIIIHH3II3I")
DESC = struct.Struct("<16I")
DESC_EXT = struct.Struct("<4I")  # ABI 1.8: pitch, pages, page_count, labels
DESC_IMP = struct.Struct("<I")    # ABI 1.11: importer, the importer kind's table (the descriptor is 84 bytes from minor 11)
DESC_FX = struct.Struct("<4I")    # ABI 1.12: render_st, fx_flags, presets, preset_count (the descriptor is 100 bytes from minor 12)
PRESET = struct.Struct("<III")  # ABI 1.12: o3m_preset on the 32-bit target: name, values, count
PAGE = struct.Struct("<II")
LABELS = struct.Struct("<II")
PAGES_MAX, PAGE_PARAMS_MAX, PAGE_NAME_MAX, LABELS_MIN, LABELS_MAX, LABEL_NAME_MAX = 4, 8, 8, 2, 16, 12
PARAM = struct.Struct("<IfffI")
UI = struct.Struct("<5I")
TOP = Path(__file__).resolve().parent.parent
TEMPLATES = TOP / "modules" / "templates"
TEMPLATE_NAMES = sorted(list(KINDS) + ["effect_stereo", "effect_key"])  # `new --kind`: the directories of modules/templates (importer has none: it is first-party only)


class ManifestError(Exception):
    pass


def f32(x):
    """The float32 a value becomes in the descriptor."""
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def _num(spec, key, where, default=None):
    if key not in spec:
        if default is None:
            raise ManifestError("%s: '%s' is missing" % (where, key))
        return default
    v = spec[key]
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise ManifestError("%s: '%s' is not a number" % (where, key))
    if v != v or v in (float("inf"), float("-inf")):
        raise ManifestError("%s: '%s' is not finite" % (where, key))
    return v


def _printable(text, limit, where, what):
    if not isinstance(text, str) or not (1 <= len(text) < limit) or any(ord(c) < 32 or ord(c) > 126 for c in text):
        raise ManifestError("%s: %s must be 1..%d printable ASCII characters" % (where, what, limit - 1))


def identifier(name):
    ident = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").upper()
    return ident if ident and ident[0].isalpha() else "P_" + ident


def load_manifest(path):
    try:
        spec = json.loads(Path(path).read_text())
    except (OSError, ValueError) as exc:
        raise ManifestError("%s: %s" % (path, exc))
    return validate(spec)


def validate(spec):
    """Returns the manifest with every default filled in and every number checked, or raises ManifestError naming the cause."""
    if not isinstance(spec, dict):
        raise ManifestError("the manifest is not an object")
    known = {"name", "id", "kind", "state_bytes", "params", "views", "voices", "memory", "abi", "description", "capabilities", "pitch", "pages", "stereo", "presets", "rate"}
    for key in spec:
        if key not in known:
            raise ManifestError("unknown key '%s' (known: %s)" % (key, ", ".join(sorted(known))))
    _printable(spec.get("name"), NAME_MAX, "name", "the name")
    mid = spec.get("id")
    if not isinstance(mid, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,30}", mid):
        raise ManifestError("id must be [a-z][a-z0-9_]* of at most 31 characters (it is the file stem and the C prefix)")
    kind = spec.get("kind")
    if kind not in KINDS:
        raise ManifestError("kind must be one of %s" % ", ".join(sorted(KINDS)))
    state = _num(spec, "state_bytes", "state_bytes")
    if int(state) != state or not (1 <= state <= MAX_STATE):
        raise ManifestError("state_bytes must be a whole number of bytes, 1..%d" % MAX_STATE)
    if "abi" in spec and spec["abi"] not in ("1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "1.10", "1.11", "1.12", "1.13"):
        raise ManifestError("abi must be \"1.0\", \"1.1\", \"1.2\", \"1.3\", \"1.4\", \"1.5\", \"1.6\", \"1.7\", \"1.8\", \"1.9\", \"1.10\", \"1.11\", \"1.12\" or \"1.13\" (this tool writes ABI 1.%d modules)" % ABI_MINOR)
    params = spec.get("params", [])
    if not isinstance(params, list) or len(params) > PARAMS_MAX:
        raise ManifestError("params must be a list of at most %d" % PARAMS_MAX)
    out_params, idents, keys = [], set(), 0
    for i, p in enumerate(params):
        where = "params[%d]" % i
        if not isinstance(p, dict):
            raise ManifestError("%s is not an object" % where)
        for key in p:
            if key not in {"name", "min", "max", "default", "type", "unit", "curve", "learnable", "mod", "labels"}:
                raise ManifestError("%s: unknown key '%s'" % (where, key))
        _printable(p.get("name"), PARAM_NAME_MAX, where, "the name")
        ptype = p.get("type", "float")
        if ptype not in ("float", "int", "bool", "key"):
            raise ManifestError("%s: type must be float, int, bool or key" % where)
        if ptype == "key":  # ABI 1.12 (OS-INSFX-ABI2): the sidechain key, 0 = none, 1..16 = the key track
            if kind != "effect":
                raise ManifestError("%s ('%s'): a key parameter is an effect's (ABI 1.12)" % (where, p["name"]))
            for banned in ("unit", "curve", "learnable", "mod", "labels"):
                if banned in p:
                    raise ManifestError("%s ('%s'): a key parameter takes no '%s' (it is never learnable and the OS shows it as OFF, T1..T%d)" % (where, p["name"], banned, KEY_TRACKS))
            keys += 1
            if keys > KEYS_MAX:
                raise ManifestError("%s ('%s'): at most %d key parameters a module" % (where, p["name"], KEYS_MAX))
            lo, hi, dflt = _num(p, "min", where, 0), _num(p, "max", where, KEY_TRACKS), _num(p, "default", where, 0)
            if lo != 0 or hi != KEY_TRACKS:
                raise ManifestError("%s ('%s'): a key parameter is min 0 (no key), max %d (track %d)" % (where, p["name"], KEY_TRACKS, KEY_TRACKS))
        else:
            lo, hi = _num(p, "min", where), _num(p, "max", where)
            dflt = _num(p, "default", where)
        if not (f32(lo) <= f32(dflt) <= f32(hi)) or not f32(lo) < f32(hi):
            raise ManifestError("%s ('%s'): need min < max and min <= default <= max (as float32)" % (where, p["name"]))
        if ptype == "bool" and (lo != 0 or hi != 1 or dflt not in (0, 1)):
            raise ManifestError("%s ('%s'): a bool has min 0, max 1 and a default of 0 or 1" % (where, p["name"]))
        if ptype in ("int", "key") and any(float(v) != int(v) for v in (lo, hi, dflt)):
            raise ManifestError("%s ('%s'): an %s parameter has whole min, max and default" % (where, p["name"], ptype))
        unit = p.get("unit", "none")
        if unit not in UNITS:
            raise ManifestError("%s: unit must be one of %s" % (where, ", ".join(UNITS)))
        curve = p.get("curve", "linear")
        if curve not in ("linear", "log"):
            raise ManifestError("%s: curve must be linear or log" % where)
        if curve == "log" and not f32(lo) > 0:
            raise ManifestError("%s ('%s'): a log curve needs min > 0" % (where, p["name"]))
        for flag in ("learnable", "mod"):
            if flag in p and not isinstance(p[flag], bool):
                raise ManifestError("%s: %s must be true or false" % (where, flag))
        ident = identifier(p["name"])
        if ident in idents:
            raise ManifestError("%s ('%s'): its C name %s is taken by an earlier parameter" % (where, p["name"], ident))
        idents.add(ident)
        labels = p.get("labels")
        if labels is not None:  # ABI 1.8 (OS-INSTUI S10): the text of an int parameter's values
            if kind not in ("instrument", "effect"):
                raise ManifestError("%s ('%s'): labels are an instrument's or an effect's (ABI 1.8)" % (where, p["name"]))
            if ptype != "int":
                raise ManifestError("%s ('%s'): labels are for int parameters only" % (where, p["name"]))
            if not isinstance(labels, list) or len(labels) != int(hi) - int(lo) + 1 or not (LABELS_MIN <= len(labels) <= LABELS_MAX):
                raise ManifestError("%s ('%s'): labels need one text for each value from min to max (max - min + 1 = %d..%d of them)" % (where, p["name"], LABELS_MIN, LABELS_MAX))
            for text in labels:
                _printable(text, LABEL_NAME_MAX, where, "a label")
        out_params.append({"name": p["name"], "min": lo, "max": hi, "default": dflt, "type": ptype, "unit": unit, "curve": curve,
                           "learnable": p.get("learnable", True), "mod": p.get("mod", False), "ident": ident, "labels": labels})
    views = spec.get("views", [])
    if not isinstance(views, list) or len(views) > VIEWS_MAX:
        raise ManifestError("views must be a list of at most %d" % VIEWS_MAX)
    if kind == "app" and not views:
        raise ManifestError("an app is its view: it needs at least one view")
    if kind == "importer" and (params or views):
        raise ManifestError("an importer (kind 3, OS-FLASHDIET I0) has no parameters and no views: it serves files, it is not heard or seen")
    out_views, vidents = [], set()
    for i, v in enumerate(views):
        where = "views[%d]" % i
        if not isinstance(v, dict):
            raise ManifestError("%s is not an object" % where)
        for key in v:
            if key not in {"name", "min_size", "preferred_size"}:
                raise ManifestError("%s: unknown key '%s'" % (where, key))
        if not isinstance(v.get("name"), str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9 _-]{0,23}", v["name"]):
            raise ManifestError("%s: name must be a short word (letters, digits, space, _ and -)" % where)
        sizes = {}
        for key, default in (("min_size", [16, 16]), ("preferred_size", None)):
            s = v.get(key, default)
            if s is None:
                s = v.get("min_size", [16, 16])
            if (not isinstance(s, list) or len(s) != 2 or any(isinstance(n, bool) or not isinstance(n, int) for n in s) or not (16 <= s[0] <= 800) or not (16 <= s[1] <= 480)):
                raise ManifestError("%s: %s must be [width, height], 16..800 by 16..480 pixels" % (where, key))
            sizes[key] = s
        if sizes["preferred_size"][0] < sizes["min_size"][0] or sizes["preferred_size"][1] < sizes["min_size"][1]:
            raise ManifestError("%s: preferred_size is smaller than min_size" % where)
        ident = identifier(v["name"])
        if ident in vidents:
            raise ManifestError("%s: its C name %s is taken by an earlier view" % (where, ident))
        vidents.add(ident)
        out_views.append({"name": v["name"], "min_size": sizes["min_size"], "preferred_size": sizes["preferred_size"], "ident": ident})
    voices = spec.get("voices", 1)
    if voices != 1:
        raise ManifestError("voices must be 1: ABI %d.%d runs one instance of a module per instrument slot (more voices are design-only)" % (ABI_MAJOR, ABI_MINOR))
    memory = spec.get("memory", {})
    if not isinstance(memory, dict) or any(k not in ("hot_bytes", "sdram_bytes") for k in memory) or any(isinstance(v, bool) or not isinstance(v, int) or v < 0 for v in memory.values()):
        raise ManifestError("memory is {\"hot_bytes\": n, \"sdram_bytes\": n} with whole, non-negative numbers (advisory)")
    needs_minor = 1 if (kind == "app" or out_views) else 0
    declared = spec.get("abi")
    if declared == "1.0" and needs_minor == 1:
        raise ManifestError("abi 1.0 cannot have views or be an app")
    if declared == "1.2":
        needs_minor = 2  # the module is declared as one that carries a version (o3m_pack.py --version), which the header has room for from ABI 1.2
    # ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): the pitch call, the params pages and the value labels, an instrument's
    pitch = spec.get("pitch", False)
    if not isinstance(pitch, bool):
        raise ManifestError("pitch must be true or false")
    pages_in = spec.get("pages", [])
    if not isinstance(pages_in, list) or len(pages_in) > PAGES_MAX:
        raise ManifestError("pages must be a list of at most %d" % PAGES_MAX)
    out_pages = []
    for i, pg in enumerate(pages_in):
        where = "pages[%d]" % i
        if not isinstance(pg, dict) or set(pg) != {"name", "first"}:
            raise ManifestError("%s must be {\"name\": ..., \"first\": ...}" % where)
        _printable(pg["name"], PAGE_NAME_MAX, where, "the name")
        if isinstance(pg["first"], bool) or not isinstance(pg["first"], int):
            raise ManifestError("%s: first must be a whole number" % where)
        out_pages.append({"name": pg["name"], "first": pg["first"]})
    if out_pages:
        if not out_params:
            raise ManifestError("pages: a module with no parameters has no pages")
        for i, pg in enumerate(out_pages):
            end = out_pages[i + 1]["first"] if i + 1 < len(out_pages) else len(out_params)
            if (i == 0 and pg["first"] != 0) or not (pg["first"] < end <= len(out_params)) or end - pg["first"] > PAGE_PARAMS_MAX:
                raise ManifestError("pages[%d]: the first page starts at parameter 0, the pages ascend, hold 1..%d parameters each and together cover all %d" % (i, PAGE_PARAMS_MAX, len(out_params)))
    effect_labels = kind == "effect" and any(p["labels"] is not None for p in out_params)  # OS-INSFX-FX2: an effect's labels ride in the 1.12 descriptor (ENTRY_FX), not in an ENTRY_TAIL
    uses_18 = pitch or bool(out_pages) or (any(p["labels"] is not None for p in out_params) and not effect_labels)
    if uses_18 and kind != "instrument":
        raise ManifestError("pitch, pages and labels are an instrument's (ABI 1.8)")
    if uses_18 and declared in ("1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7"):
        raise ManifestError("pitch, pages and labels need abi 1.8 or no abi key: the manifest declares %s" % declared)
    # ABI 1.12 (OS-INSFX-ABI2 A2): the stereo shape of an effect, the factory presets, and (above) the key parameter
    stereo = spec.get("stereo")
    if stereo is not None:
        if stereo not in STEREO_SHAPES:
            raise ManifestError("stereo must be \"widens\" or \"capable\"")
        if kind != "effect":
            raise ManifestError("stereo is an effect's (ABI 1.12): this module is not one")
    presets_in = spec.get("presets", [])
    if not isinstance(presets_in, list) or len(presets_in) > PRESETS_MAX:
        raise ManifestError("presets must be a list of at most %d" % PRESETS_MAX)
    if presets_in and kind not in ("instrument", "effect"):
        raise ManifestError("presets are an instrument's and an effect's (ABI 1.12, OS-INSFX-ABI2 Q8): an app and an importer have none")
    if presets_in and not out_params:
        raise ManifestError("presets: a module with no parameters has nothing to set")
    by_name = {p["name"]: i for i, p in enumerate(out_params)}
    out_presets, preset_names = [], set()
    for k, pr in enumerate(presets_in):
        where = "presets[%d]" % k
        if not isinstance(pr, dict) or set(pr) != {"name", "values"}:
            raise ManifestError("%s must be {\"name\": ..., \"values\": {parameter: value}}" % where)
        _printable(pr["name"], PRESET_NAME_MAX, where, "the name")
        if pr["name"] in preset_names:
            raise ManifestError("%s: the name '%s' is taken by an earlier preset" % (where, pr["name"]))
        preset_names.add(pr["name"])
        vals = pr["values"]
        if not isinstance(vals, dict) or not vals:
            raise ManifestError("%s ('%s'): values is {parameter name: value} with at least one parameter" % (where, pr["name"]))
        row = [None] * len(out_params)
        for pname, v in vals.items():
            if pname not in by_name:
                raise ManifestError("%s ('%s'): '%s' names no parameter (the params are: %s)" % (where, pr["name"], pname, ", ".join(p["name"] for p in out_params)))
            q = out_params[by_name[pname]]
            v = _num({"v": v}, "v", "%s ('%s'), parameter '%s'" % (where, pr["name"], pname))
            if not (f32(q["min"]) <= f32(v) <= f32(q["max"])):
                raise ManifestError("%s ('%s'): %s is %s, outside its range %s..%s" % (where, pr["name"], pname, v, q["min"], q["max"]))
            if q["type"] != "float" and float(v) != int(v):
                raise ManifestError("%s ('%s'): %s is a whole-number parameter, %s is not" % (where, pr["name"], pname, v))
            row[by_name[pname]] = v
        count = max(i for i, v in enumerate(row) if v is not None) + 1  # values[count] in parameter order; a gap before the last named one takes the parameter's default, the parameters past it the OS's
        out_presets.append({"name": pr["name"], "values": [out_params[i]["default"] if row[i] is None else row[i] for i in range(count)], "count": count})
    uses_12 = stereo is not None or bool(out_presets) or keys > 0 or effect_labels
    if uses_12 and declared in ("1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "1.10", "1.11"):
        raise ManifestError("stereo, presets and key parameters need abi 1.12 or no abi key: the manifest declares %s" % declared)
    # ABI 1.13 (OS-MODRATE): an instrument that renders correctly at the half and quarter rate the OS may ask for (host->sample_rate, frames)
    rate = spec.get("rate", False)
    if not isinstance(rate, bool):
        raise ManifestError("rate must be true or false")
    if rate and kind != "instrument":
        raise ManifestError("rate is an instrument's (ABI 1.13, OS-MODRATE): this module is not one")
    if rate and declared in ("1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "1.10", "1.11", "1.12"):
        raise ManifestError("rate needs abi 1.13 or no abi key: the manifest declares %s" % declared)
    caps_in = spec.get("capabilities", [])
    if not isinstance(caps_in, list) or any(c not in CAPS for c in caps_in) or len(set(caps_in)) != len(caps_in):
        raise ManifestError("capabilities is a list of distinct names from: %s" % ", ".join(sorted(CAPS)))
    caps = 0
    for c in caps_in:
        caps |= CAPS[c]
    if caps and kind != "app":
        raise ManifestError("capabilities are an app's (ABI 1.3): this module is not one")
    if caps or declared in ("1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9", "1.10"):
        if declared in ("1.0", "1.1", "1.2") or (declared == "1.3" and caps & CAPS_ABI_1_4) or (declared in ("1.3", "1.4", "1.5", "1.6") and caps & CAPS_ABI_1_7) or (declared in ("1.3", "1.4", "1.5", "1.6", "1.7", "1.8", "1.9") and caps & CAPS_ABI_1_10):
            raise ManifestError("capabilities need abi 1.3 (audio_tap: 1.4) or no abi key: the manifest declares %s" % declared)
        needs_minor = 10 if (declared == "1.10" or caps & CAPS_ABI_1_10) else 9 if declared == "1.9" else 8 if declared == "1.8" else 7 if (declared == "1.7" or caps & CAPS_ABI_1_7) else 6 if declared == "1.6" else 5 if declared == "1.5" else (4 if (caps & CAPS_ABI_1_4 or declared == "1.4") else 3)  # the header's capability word (reserved2[0]) and the host tables exist from ABI 1.3; the audio tap from 1.4; the steps and edit groups from 1.5, the instrument parameters from 1.6 (declared: no capability bit)
    if uses_18 or declared == "1.8":
        needs_minor = 8
    if declared == "1.9":
        needs_minor = 9  # ABI 1.9 (OS-KEYROWDLG S5): the app's ui table is 24 bytes (O3M_UI_SIZE) and may carry keys()
    if declared == "1.10":
        needs_minor = 10  # ABI 1.10 (OS-APPUX S3): the io table's get_transport, set_own_param, play_note and stop_note; the ui table is still 24 bytes
    if declared == "1.11":
        needs_minor = 11  # ABI 1.11 (OS-FLASHDIET I1): the importer kind (kind 3), its table and header word 44's layout tag
    if declared == "1.12" or uses_12:
        needs_minor = 12  # ABI 1.12 (OS-INSFX-ABI2): the descriptor's fourth tail (render_st, fx_flags, presets), the KEY flag and the two host members; the manifest keys for them are "stereo", "presets" and a "key" parameter
    if declared == "1.13" or rate:
        needs_minor = RATE_ABI_MINOR  # ABI 1.13 (OS-MODRATE): the RATE bit of fx_flags for an instrument (the descriptor stays 100 bytes); the manifest key is "rate"
    if kind == "importer":
        needs_minor = max(needs_minor, IMPORTER_ABI_MINOR)  # an importer is published at ABI 1.11: a lower declared minor cannot load it
    return {"name": spec["name"], "id": mid, "kind": kind, "state_bytes": int(state), "params": out_params, "views": out_views, "voices": 1, "pitch": pitch, "pages": out_pages,
            "memory": {"hot_bytes": memory.get("hot_bytes", 0), "sdram_bytes": memory.get("sdram_bytes", 0)}, "abi_minor": needs_minor,
            "description": spec.get("description", ""), "caps": caps, "stereo": stereo, "presets": out_presets, "rate": rate}


def param_flags(p):
    flags = 0
    flags |= F_LOG if p["curve"] == "log" else 0
    flags |= F_INT if p["type"] in ("int", "key") else 0
    flags |= F_BOOL if p["type"] == "bool" else 0
    flags |= F_KEY if p["type"] == "key" else 0
    flags |= 0 if p["learnable"] else F_NO_LEARN
    flags |= F_MOD if p["mod"] else 0
    flags |= UNITS[p["unit"]] << UNIT_SHIFT
    return flags


def flags_text(p):
    parts = []
    if p["curve"] == "log":
        parts.append("O3M_PARAM_LOG")
    if p["type"] in ("int", "key"):
        parts.append("O3M_PARAM_INT")
    if p["type"] == "bool":
        parts.append("O3M_PARAM_BOOL")
    if p["type"] == "key":
        parts.append("O3M_PARAM_KEY")
    if not p["learnable"]:
        parts.append("O3M_PARAM_NO_LEARN")
    if p["mod"]:
        parts.append("O3M_PARAM_MOD")
    if p["unit"] != "none":
        parts.append("O3M_PARAM_WITH_UNIT(%s)" % UNIT_CONST[p["unit"]])
    return " | ".join(parts) if parts else "0"


def c_float(x):
    text = "%.9g" % f32(x)
    if "." not in text and "e" not in text and "n" not in text:
        text += ".0"
    return text + "f"


def generate(m):
    pre = m["id"].upper()
    lines = []
    add = lines.append
    add("/* SPDX-License-Identifier: MIT */")
    add("/* GENERATED by tools/o3m_gen.py from module.json: do not edit (regenerate; `o3m_gen.py check` finds a stale copy). */")
    add("#ifndef %s_GEN_H" % pre)
    add("#define %s_GEN_H" % pre)
    add("")
    add("#include <o3module.h>")
    add("")
    add("#define %s_NAME \"%s\"" % (pre, m["name"]))
    add("#define %s_KIND %s" % (pre, KIND_CONST[m["kind"]]))
    add("/* The ABI minor this module needs: 0 for an instrument or effect with no views (it loads on an OS of ABI 1.0 too), 1 for views or an app, 2 when the manifest declares abi 1.2 (a module shipped with a version, o3m_pack.py --version). */")
    add("#define %s_ABI_MINOR %d" % (pre, m["abi_minor"]))
    if m["caps"]:  # (only for an app that asks: the committed headers of the modules that do not stay as they were)
        add("/* The capabilities the manifest asks for (O3M_CAP_*), written to the file by the packer (--caps), not by the source. */")
        add("#define %s_CAPS %du" % (pre, m["caps"]))
    add("#define %s_STATE_BYTES %d /* what the manifest says init() is given: the source's State must fit (static_assert it) */" % (pre, m["state_bytes"]))
    add("")
    add("/* Parameter ids: set_param(state, %s_PARAM_x, value), canvas->params[%s_PARAM_x]. */" % (pre, pre))
    if m["params"]:
        add("enum {")
        for i, p in enumerate(m["params"]):
            add("  %s_PARAM_%s = %d," % (pre, p["ident"], i))
        add("  %s_PARAM_COUNT = %d" % (pre, len(m["params"])))
        add("};")
        add("")
        add("static const o3m_param %s_params[%s_PARAM_COUNT] = {" % (m["id"], pre))
        for p in m["params"]:
            add("  {\"%s\", %s, %s, %s, %s}," % (p["name"], c_float(p["min"]), c_float(p["max"]), c_float(p["default"]), flags_text(p)))
        add("};")
    else:
        add("enum { %s_PARAM_COUNT = 0 };" % pre)
    add("")
    labelled = [(i, p) for i, p in enumerate(m["params"]) if p["labels"] is not None]
    if m["pages"] or labelled:  # ABI 1.8 (OS-INSTUI S10): the params pages and the text of int values; the descriptor's pages / labels members
        add("/* ABI 1.8, the params screen: page names and the text of int values (o3module.h PAGES AND LABELS). */")
        for i, p in labelled:
            add("static const char* const %s_labels_%s[%d] = {%s};" % (m["id"], p["ident"].lower(), len(p["labels"]), ", ".join("\"%s\"" % t for t in p["labels"])))
        if labelled:
            add("static const o3m_labels %s_labels[%s_PARAM_COUNT] = {" % (m["id"], pre))
            for i, p in enumerate(m["params"]):
                if p["labels"] is not None:
                    add("  {%s_labels_%s, %d}," % (m["id"], p["ident"].lower(), len(p["labels"])))
                else:
                    add("  {0, 0},")
            add("};")
        if m["pages"]:
            add("static const o3m_page %s_pages[%d] = {" % (m["id"], len(m["pages"])))
            for pg in m["pages"]:
                add("  {\"%s\", %d}," % (pg["name"], pg["first"]))
            add("};")
        add("")
    if m["presets"]:  # ABI 1.12 (OS-INSFX-ABI2): the factory presets, values in parameter order (a preset sets every parameter: those past `count` take their default)
        add("/* ABI 1.12, the factory presets (o3module.h FACTORY PRESETS): values in parameter order, the parameters past a preset's count take their default. */")
        for k, pr in enumerate(m["presets"]):
            add("static const float %s_preset_%d[%d] = {%s};" % (m["id"], k, pr["count"], ", ".join(c_float(v) for v in pr["values"])))
        add("static const o3m_preset %s_presets[%d] = {" % (m["id"], len(m["presets"])))
        for k, pr in enumerate(m["presets"]):
            add("  {\"%s\", %s_preset_%d, %d}," % (pr["name"], m["id"], k, pr["count"]))
        add("};")
        add("enum { %s_PRESET_COUNT = %d };" % (pre, len(m["presets"])))
        add("")
    add("/* View indices: view_draw(state, %s_VIEW_x, canvas). */" % pre)
    if m["views"]:
        add("enum {")
        for i, v in enumerate(m["views"]):
            add("  %s_VIEW_%s = %d," % (pre, v["ident"], i))
        add("  %s_VIEW_COUNT = %d" % (pre, len(m["views"])))
        add("};")
        add("")
        add("/* What each view needs to be readable (the OS gives it a rectangle at least this big where the page has the room); the file does not carry them. */")
        for v in m["views"]:
            add("#define %s_VIEW_%s_MIN_W %d" % (pre, v["ident"], v["min_size"][0]))
            add("#define %s_VIEW_%s_MIN_H %d" % (pre, v["ident"], v["min_size"][1]))
            add("#define %s_VIEW_%s_PREFERRED_W %d" % (pre, v["ident"], v["preferred_size"][0]))
            add("#define %s_VIEW_%s_PREFERRED_H %d" % (pre, v["ident"], v["preferred_size"][1]))
    else:
        add("enum { %s_VIEW_COUNT = 0 };" % pre)
    add("")
    add("/* Advice from the manifest, not carried by the file. */")
    add("#define %s_VOICES %d" % (pre, m["voices"]))
    add("#define %s_HOT_BYTES_ADVICE %d" % (pre, m["memory"]["hot_bytes"]))
    add("#define %s_SDRAM_BYTES_ADVICE %d" % (pre, m["memory"]["sdram_bytes"]))
    add("")
    add("/* The descriptor, in the order of o3m_module:")
    tail = (", %s_ENTRY_TAIL(pitch)" % pre) if (m["abi_minor"] >= 8 and m["kind"] == "instrument") else ""
    if m["kind"] == "importer":
        tail += ", %s_ENTRY_IMP(&importer_table)" % pre
    has_fx = m["kind"] == "effect" and (m["stereo"] is not None or bool(m["presets"]) or labelled) or m["kind"] == "instrument" and (bool(m["presets"]) or m["rate"])
    if has_fx:
        tail += (", %s_ENTRY_FX(render_st)" if m["stereo"] is not None else ", %s_ENTRY_FX") % pre
    add(" *   o3m_entry = { %s_ENTRY_HEAD, sizeof(State), %s_ENTRY_PARAMS, init, render, set_param, note_on, note_off, shutdown, %s_ENTRY_VIEWS(&ui_table)%s };" % (pre, pre, pre, tail))
    add(" * (an app passes NULL for render, note_on and note_off and set_param if it has none; a module without views passes NULL for the table; an importer (kind 3, ABI 1.11) adds its table with ENTRY_IMP and has no parameters, no views and no sound entries). */")
    add("#define %s_ENTRY_HEAD O3M_ABI_MAJOR, %s_ABI_MINOR, %s_KIND, O3M_MODULE_SIZE(%s_ABI_MINOR), %s_NAME" % (pre, pre, pre, pre, pre))
    if m["params"]:
        add("#define %s_ENTRY_PARAMS %s_PARAM_COUNT, %s_params" % (pre, pre, m["id"]))
    else:
        add("#define %s_ENTRY_PARAMS 0, 0" % pre)
    add("#define %s_ENTRY_VIEWS(ui) (ui), %s_VIEW_COUNT" % (pre, pre))
    if m["kind"] == "importer":
        add("/* ABI 1.11 (OS-FLASHDIET I1): after the views, the four 1.8 members (an importer has none: NULL, NULL, 0, NULL) and then its table, the last member of the descriptor (kind 3 only; the packer writes the layout tag to header word 44). */")
        add("#define %s_ENTRY_IMP(imp) 0, 0, 0, 0, (imp)" % pre)
    if has_fx:  # ABI 1.12: the members past the views, written positionally (C++17 has no designated initialisers): an effect's 1.8 tail and `importer` are NULL, an instrument's follow ENTRY_TAIL
        pres = ("%s_presets, %s_PRESET_COUNT" % (m["id"], pre)) if m["presets"] else "0, 0"
        if m["kind"] == "effect":
            lab = ("%s_labels" % m["id"]) if labelled else "0"  # OS-INSFX-FX2: the one member of the 1.8 tail an effect may fill, the label table
            add("/* ABI 1.12 (OS-INSFX-ABI2), after the views: the four members of 1.8 (an effect has no pitch and no pages: NULL, NULL, 0; the label table when a parameter has labels, else NULL) and `importer` (NULL), then render_st, fx_flags (%s), presets, preset_count. */" % ("O3M_FX_WIDENS" if m["stereo"] == "widens" else "0"))
            if m["stereo"] is not None:
                add("#define %s_ENTRY_FX(render_st_fn) 0, 0, 0, %s, 0, (render_st_fn), %s, %s" % (pre, lab, "O3M_FX_WIDENS" if m["stereo"] == "widens" else "0", pres))
            else:
                add("#define %s_ENTRY_FX 0, 0, 0, %s, 0, 0, 0, %s" % (pre, lab, pres))
        else:
            add("/* ABI 1.12 (OS-INSFX-ABI2), after %s_ENTRY_TAIL: `importer`, render_st (an instrument has none: NULL, NULL), fx_flags (%s), then presets and preset_count. */" % (pre, "O3M_FX_RATE, OS-MODRATE" if m["rate"] else "0"))
            add("#define %s_ENTRY_FX 0, 0, %s, %s" % (pre, "O3M_FX_RATE" if m["rate"] else "0", pres))
    if m["abi_minor"] >= 8 and m["kind"] == "instrument":
        add("/* ABI 1.8, after the views: the pitch call (NULL for none) and the manifest's page and label tables. */")
        add("#define %s_ENTRY_TAIL(pitch_fn) (pitch_fn), %s, %s" % (pre, ("%s_pages, %d" % (m["id"], len(m["pages"]))) if m["pages"] else "0, 0", ("%s_labels" % m["id"]) if labelled else "0"))
        if m["pitch"]:
            add("#define %s_HAS_PITCH 1 /* module.json: \"pitch\": true: pass your pitch function to %s_ENTRY_TAIL */" % (pre, pre))
    add("")
    add("#endif /* %s_GEN_H */" % pre)
    return "\n".join(lines) + "\n"


def expectations(m):
    """What a loader reads back from the built file, one fact a line: the loader test and `verify` compare with it."""
    out = ["name %s" % m["name"], "kind %d" % KINDS[m["kind"]], "abi %d.%d" % (ABI_MAJOR, m["abi_minor"]), "params %d" % len(m["params"]), "views %d" % len(m["views"]), "caps %d" % m["caps"]]
    for i, p in enumerate(m["params"]):
        out.append("param %d %s %s %s %s %d" % (i, p["name"], "%.9g" % f32(p["min"]), "%.9g" % f32(p["max"]), "%.9g" % f32(p["default"]), param_flags(p)))
    if m["abi_minor"] >= 8:  # ABI 1.8: the three optional members (tests/module_gen_test.cpp compares them with the loader's)
        out.append("pitch %d" % (1 if m["pitch"] else 0))
        for k, pg in enumerate(m["pages"]):
            out.append("page %d %s %d" % (k, pg["name"], pg["first"]))
        for i, p in enumerate(m["params"]):
            if p["labels"] is not None:
                out.append("labels %d %s" % (i, "|".join(p["labels"])))
    if m["abi_minor"] >= STEREO_ABI_MINOR:  # ABI 1.12: the stereo shape and the presets, "preset K COUNT v0 .. vN NAME"
        out.append("stereo %s" % (m["stereo"] or "none"))
        if m["rate"]:
            out.append("rate 1")
        for k, pr in enumerate(m["presets"]):
            out.append("preset %d %d %s %s" % (k, pr["count"], " ".join("%.9g" % f32(v) for v in pr["values"]), pr["name"]))
    return "\n".join(out) + "\n"


# ---- the independent reader of a .tmod ---------------------------------------------------------------------------------------------
def read_tmod(data):
    """The descriptor of a packed module, read with its own relocation walk (not the packer's, not the loader's)."""
    if len(data) < HEADER.size:
        raise ManifestError("shorter than a header")
    h = HEADER.unpack_from(data)
    magic, hsize, target, kind, major, minor, flags, hot, image, desc_off, state, relocs, pcount, vcount = h[:14]
    crc = h[17]
    if magic != FILE_MAGIC or hsize != HEADER.size:
        raise ManifestError("not a module file")
    if len(data) != HEADER.size + image + 4 * relocs:
        raise ManifestError("file length does not match the header")
    zeroed = data[:48] + b"\0\0\0\0" + data[52:HEADER.size]
    if zlib.crc32(data[HEADER.size:], zlib.crc32(zeroed)) & 0xFFFFFFFF != crc:
        raise ManifestError("checksum mismatch")
    img = bytearray(data[HEADER.size:HEADER.size + image])
    table = struct.unpack_from("<%dI" % relocs, data, HEADER.size + image)
    for off in table:  # linked at address 0: an absolute address in the image is an offset in it (the relocation to the load place is the loader's)
        if off % 4 or off + 4 > image:
            raise ManifestError("relocation outside the image")
    d = DESC.unpack_from(img, desc_off)
    (dmajor, dminor, dkind, dsize, name_p, dstate, dparams, params_p, init, render, set_param, note_on, note_off, shutdown, ui_p, dviews) = d
    if dsize != (DESC.size + (DESC_EXT.size if dminor >= 8 else 0) + (DESC_IMP.size if dminor >= IMPORTER_ABI_MINOR else 0) + (DESC_FX.size if dminor >= STEREO_ABI_MINOR else 0)):
        raise ManifestError("struct_size %d is not the size of the descriptor of ABI 1.%d" % (dsize, dminor))
    pitch_p, pages_p, page_count, labels_p = DESC_EXT.unpack_from(img, desc_off + DESC.size) if dminor >= 8 else (0, 0, 0, 0)
    fx_at = desc_off + DESC.size + (DESC_EXT.size if dminor >= 8 else 0) + (DESC_IMP.size if dminor >= IMPORTER_ABI_MINOR else 0)
    render_st, fx_flags, presets_p, preset_count = DESC_FX.unpack_from(img, fx_at) if dminor >= STEREO_ABI_MINOR else (0, 0, 0, 0)

    def cstr(at):
        end = img.index(b"\0", at)
        return img[at:end].decode("ascii")
    params = []
    for i in range(dparams):
        npt, pmin, pmax, pdef, pflags = PARAM.unpack_from(img, params_p + PARAM.size * i)
        params.append({"name": cstr(npt), "min": pmin, "max": pmax, "default": pdef, "flags": pflags})
    pages = [{"name": cstr(PAGE.unpack_from(img, pages_p + PAGE.size * k)[0]), "first": PAGE.unpack_from(img, pages_p + PAGE.size * k)[1]} for k in range(page_count)]
    labels = {}
    if labels_p:
        for i in range(dparams):
            names_p, count = LABELS.unpack_from(img, labels_p + LABELS.size * i)
            if count:
                labels[i] = [cstr(struct.unpack_from("<I", img, names_p + 4 * v)[0]) for v in range(count)]
    presets = []
    for k in range(preset_count):
        preset_name_p, values_p, count = PRESET.unpack_from(img, presets_p + PRESET.size * k)
        presets.append({"name": cstr(preset_name_p), "values": list(struct.unpack_from("<%df" % count, img, values_p))})
    ui = None
    if ui_p:
        usize, enter, inp, draw, leave = UI.unpack_from(img, ui_p)
        ui = {"enter": bool(enter), "input": bool(inp), "view_draw": bool(draw), "exit": bool(leave)}
    return {"name": cstr(name_p), "kind": dkind, "abi": [dmajor, dminor], "header_abi": [major, minor], "state_bytes": dstate, "hot_size": hot, "image_size": image,
            "params": params, "views": dviews, "ui": ui, "entries": {"render": bool(render), "set_param": bool(set_param), "note_on": bool(note_on), "note_off": bool(note_off)},
            "header_params": pcount, "header_views": vcount, "header_kind": kind, "header_state": state, "header_caps": h[15], "header_tag": h[16],
            "pitch": bool(pitch_p), "pages": pages, "labels": labels,
            "render_st": bool(render_st), "fx_flags": fx_flags, "presets": presets}


def verify(m, data, layout_tag=IMPORT_LAYOUT_TAG):
    """Differences between a built module and its manifest (an empty list: the same)."""
    got = read_tmod(data)
    diffs = []

    def cmp(what, a, b):
        if a != b:
            diffs.append("%s: manifest %r, file %r" % (what, a, b))
    cmp("name", m["name"], got["name"])
    cmp("kind", KINDS[m["kind"]], got["kind"])
    cmp("kind in the header", KINDS[m["kind"]], got["header_kind"])
    cmp("abi", [ABI_MAJOR, m["abi_minor"]], got["abi"])
    cmp("abi in the header", [ABI_MAJOR, m["abi_minor"]], got["header_abi"])
    cmp("capabilities in the header", m["caps"], got["header_caps"])
    cmp("parameter count", len(m["params"]), len(got["params"]))
    cmp("parameter count in the header", len(m["params"]), got["header_params"])
    cmp("view count", len(m["views"]), got["views"])
    cmp("view count in the header", len(m["views"]), got["header_views"])
    if got["state_bytes"] < 1 or got["state_bytes"] > MAX_STATE:
        diffs.append("state_bytes %d is out of range" % got["state_bytes"])
    if got["state_bytes"] > m["state_bytes"]:
        diffs.append("the module asks for %d bytes of state, the manifest says %d" % (got["state_bytes"], m["state_bytes"]))
    if m["kind"] in ("app", "importer"):
        cmp("a %s's render entry (NULL: it makes no sound)" % m["kind"], False, got["entries"]["render"])
    elif not got["entries"]["render"]:
        diffs.append("no render entry")
    if m["kind"] == "importer":
        cmp("header word 44, the layout tag", layout_tag, got["header_tag"])
    if m["views"] and not (got["ui"] and got["ui"]["view_draw"]):
        diffs.append("views declared but no view_draw in the file")
    if m["abi_minor"] >= 8:
        cmp("pitch", m["pitch"], got["pitch"])
        cmp("pages", m["pages"], got["pages"])
        cmp("labels", {i: p["labels"] for i, p in enumerate(m["params"]) if p["labels"] is not None}, got["labels"])
    if m["abi_minor"] >= STEREO_ABI_MINOR:  # ABI 1.12: the stereo render, the flag and the factory presets (names and every value, as float32)
        cmp("render_st (a stereo effect has one)", m["stereo"] is not None, got["render_st"])
        cmp("fx_flags", (FX_WIDENS if m["stereo"] == "widens" else 0) | (FX_RATE if m["rate"] else 0), got["fx_flags"])
        cmp("presets", [{"name": pr["name"], "values": [f32(v) for v in pr["values"]]} for pr in m["presets"]], got["presets"])
    for i, (p, g) in enumerate(zip(m["params"], got["params"])):
        cmp("param %d name" % i, p["name"], g["name"])
        cmp("param %d min" % i, f32(p["min"]), g["min"])
        cmp("param %d max" % i, f32(p["max"]), g["max"])
        cmp("param %d default" % i, f32(p["default"]), g["default"])
        cmp("param %d flags" % i, param_flags(p), g["flags"])
    return diffs


# ---- scaffolding from the templates --------------------------------------------------------------------------------------------------
def scaffold(kind, mid, name, out):
    src = TEMPLATES / kind
    if not src.is_dir():
        raise ManifestError("no template for kind '%s' in %s" % (kind, TEMPLATES))
    try:
        old = json.loads((src / "module.json").read_text())["id"]  # the template's own id is the text to replace: example_<kind>, or example_stereo for effect_stereo and example_key for effect_key
    except (OSError, ValueError, KeyError):
        raise ManifestError("the template %s has no readable module.json with an id" % src)
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,30}", mid):
        raise ManifestError("id must be [a-z][a-z0-9_]* of at most 31 characters")
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for f in sorted(src.iterdir()):
        if not f.is_file():
            continue
        text = f.read_text().replace(old.upper(), mid.upper()).replace(old, mid)
        target = out / f.name.replace(old, mid)
        if target.exists():
            raise ManifestError("%s exists: nothing was overwritten" % target)
        target.write_text(text)
    manifest = out / "module.json"
    spec = json.loads(manifest.read_text())
    if name:
        spec["name"] = name
    manifest.write_text(json.dumps(spec, indent=2) + "\n")
    m = validate(spec)
    (out / (mid + "_gen.h")).write_text(generate(m))
    return sorted(p.name for p in out.iterdir())


# ---- the command line --------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def refused(spec, fragment):
        try:
            validate(spec)
        except ManifestError as exc:
            expect(fragment in str(exc), "refused for another reason: %s (wanted '%s')" % (exc, fragment))
            return
        failures.append("accepted: %r (wanted '%s')" % (spec, fragment))
    base = {"name": "T", "id": "t", "kind": "instrument", "state_bytes": 64, "params": [{"name": "A", "min": 0, "max": 1, "default": 0.5}]}
    ok = validate(base)
    expect(ok["abi_minor"] == 0 and ok["params"][0]["learnable"] is True, "defaults")
    refused({**base, "bogus": 1}, "unknown key")
    refused({**base, "name": ""}, "name")
    refused({**base, "id": "T"}, "id")
    refused({**base, "kind": "widget"}, "kind")
    imp = validate({"name": "T", "id": "t", "kind": "importer", "state_bytes": 4096})
    expect(imp["kind"] == "importer" and imp["abi_minor"] == IMPORTER_ABI_MINOR and imp["params"] == [] and imp["views"] == [], "importer defaults (ABI 1.11, whatever is declared)")
    expect(validate({"name": "T", "id": "t", "kind": "importer", "state_bytes": 4096, "abi": "1.2"})["abi_minor"] == IMPORTER_ABI_MINOR, "an importer is 1.11 even declaring 1.2")
    imp_text = generate(imp)
    for fragment in ("#define T_ABI_MINOR 11", "#define T_ENTRY_IMP(imp) 0, 0, 0, 0, (imp)", "T_ENTRY_IMP(&importer_table)", "O3M_MODULE_SIZE(T_ABI_MINOR)"):
        expect(fragment in imp_text, "the importer's header lacks '%s'" % fragment)
    refused({"name": "T", "id": "t", "kind": "importer", "state_bytes": 4096, "params": [{"name": "A", "min": 0, "max": 1, "default": 0.5}]}, "no parameters and no views")
    refused({"name": "T", "id": "t", "kind": "importer", "state_bytes": 4096, "views": [{"name": "V"}]}, "no parameters and no views")
    refused({**base, "state_bytes": 0}, "state_bytes")
    refused({**base, "state_bytes": 1.5}, "state_bytes")
    refused({**base, "params": [{"name": "A", "min": 1, "max": 1, "default": 1}]}, "min < max")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 1, "default": 2}]}, "default")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 1, "default": 0, "curve": "log"}]}, "log")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 5, "default": 0, "type": "bool"}]}, "bool")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 5.5, "default": 0, "type": "int"}]}, "int")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 1, "default": 0, "unit": "furlongs"}]}, "unit")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 1, "default": 0}, {"name": "a", "min": 0, "max": 1, "default": 0}]}, "C name")
    refused({**base, "params": [{"name": "A" * 16, "min": 0, "max": 1, "default": 0}]}, "name")
    refused({**base, "params": [{"name": "A", "min": 0, "max": float("nan"), "default": 0}]}, "finite")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 1, "default": 0}] * 33}, "at most 32")
    refused({**base, "kind": "app"}, "needs at least one view")
    refused({**base, "views": [{"name": "v", "min_size": [8, 8]}]}, "16..800")
    refused({**base, "views": [{"name": "v", "min_size": [100, 100], "preferred_size": [50, 50]}]}, "smaller")
    refused({**base, "views": [{"name": "v"}] * 9}, "at most 8")
    refused({**base, "views": [{"name": "v"}, {"name": "V"}]}, "C name")
    refused({**base, "voices": 4}, "voices")
    refused({**base, "abi": "1.0", "views": [{"name": "v"}]}, "abi 1.0")
    refused({**base, "abi": "1.14"}, "abi must be")
    expect(validate({**base, "abi": "1.11"})["abi_minor"] == 11, "abi 1.11 may be declared")
    expect(validate({**base, "abi": "1.12"})["abi_minor"] == 12 and validate({**base, "abi": "1.12", "kind": "effect"})["abi_minor"] == 12, "abi 1.12 may be declared (OS-INSFX-ABI2 A1)")
    expect("#define T_ABI_MINOR 12" in generate(validate({**base, "abi": "1.12"})) and "O3M_MODULE_SIZE(T_ABI_MINOR)" in generate(validate({**base, "abi": "1.12"})), "a 1.12 module writes its size through the macro")
    expect(validate({"name": "T", "id": "t", "kind": "importer", "state_bytes": 4096, "abi": "1.12"})["abi_minor"] == 12, "an importer may declare 1.12 (the four new members zero)")
    expect(validate({**base, "abi": "1.2"})["abi_minor"] == 2 and validate({**base, "abi": "1.2", "views": [{"name": "v"}]})["abi_minor"] == 2, "abi 1.2 is declared, whatever the module uses")
    expect(validate({**base, "abi": "1.1"})["abi_minor"] == 0, "a declared 1.1 is only a ceiling: the module needs what it uses")
    refused({**base, "memory": {"stack": 1}}, "memory")
    # ABI 1.12 (OS-INSFX-ABI2 A2): the stereo shape, the factory presets and the key parameter
    fx = {"name": "T", "id": "t", "kind": "effect", "state_bytes": 64,
          "params": [{"name": "Sync", "min": 0, "max": 1, "default": 1, "type": "bool"}, {"name": "Cycle", "min": 50, "max": 5000, "default": 500, "unit": "ms"},
                     {"name": "Beats", "min": 1, "max": 16, "default": 1, "type": "int"}, {"name": "Depth", "min": 0, "max": 100, "default": 60, "unit": "percent"}]}
    wide = validate({**fx, "stereo": "widens"})
    expect(wide["abi_minor"] == 12 and wide["stereo"] == "widens" and wide["presets"] == [] and validate({**fx, "stereo": "capable"})["stereo"] == "capable", "stereo makes an effect ABI 1.12")
    expect(validate(fx)["abi_minor"] == 0 and validate(fx)["stereo"] is None, "an effect without the new keys stays as it was")
    wide_text = generate(wide)
    for fragment in ("#define T_ABI_MINOR 12", "#define T_ENTRY_FX(render_st_fn) 0, 0, 0, 0, 0, (render_st_fn), O3M_FX_WIDENS, 0, 0", "T_ENTRY_FX(render_st)", "O3M_MODULE_SIZE(T_ABI_MINOR)"):
        expect(fragment in wide_text, "the stereo header lacks '%s'" % fragment)
    expect("T_ENTRY_FX(render_st_fn) 0, 0, 0, 0, 0, (render_st_fn), 0, 0, 0" in generate(validate({**fx, "stereo": "capable"})), "a stereo-capable effect writes fx_flags 0")
    expect("T_ENTRY_FX" not in generate(validate(fx)) and "_presets" not in generate(validate(fx)), "no stereo, no presets, no tail")
    both = validate({**fx, "stereo": "widens", "presets": [{"name": "Slow", "values": {"Sync": 0, "Cycle": 2000}}, {"name": "Quarter", "values": {"Beats": 1, "Depth": 100}}]})
    expect(both["presets"][0] == {"name": "Slow", "values": [0, 2000], "count": 2} and both["presets"][1]["values"] == [1, 500, 1, 100] and both["presets"][1]["count"] == 4, "a preset is the values to the last named parameter, a gap takes the default")
    both_text = generate(both)
    for fragment in ("static const float t_preset_0[2] = {0.0f, 2000.0f};", "static const float t_preset_1[4] = {1.0f, 500.0f, 1.0f, 100.0f};", "static const o3m_preset t_presets[2] = {", "  {\"Slow\", t_preset_0, 2},",
                     "enum { T_PRESET_COUNT = 2 };", "#define T_ENTRY_FX(render_st_fn) 0, 0, 0, 0, 0, (render_st_fn), O3M_FX_WIDENS, t_presets, T_PRESET_COUNT"):
        expect(fragment in both_text, "the preset header lacks '%s'" % fragment)
    expect("stereo widens" in expectations(both) and "preset 0 2 0 2000 Slow" in expectations(both) and "preset 1 4 1 500 1 100 Quarter" in expectations(both) and "stereo" not in expectations(validate(fx)), "the expectations carry the stereo shape and the presets")
    presets_only = generate(validate({**fx, "presets": [{"name": "P", "values": {"Depth": 10}}]}))
    expect("#define T_ENTRY_FX 0, 0, 0, 0, 0, 0, 0, t_presets, T_PRESET_COUNT" in presets_only and "#define T_ABI_MINOR 12" in presets_only, "presets alone: an effect's entry macro with no argument")
    ins_presets = validate({**base, "presets": [{"name": "P", "values": {"A": 1}}], "pitch": True})
    ins_text = generate(ins_presets)
    expect(ins_presets["abi_minor"] == 12 and "#define T_ENTRY_FX 0, 0, 0, t_presets, T_PRESET_COUNT" in ins_text and "T_ENTRY_TAIL(pitch_fn)" in ins_text and "T_ENTRY_TAIL(pitch), T_ENTRY_FX };" in ins_text, "an instrument's presets follow its 1.8 tail")
    expect(validate({**base, "presets": [{"name": "P", "values": {"A": 1}}], "abi": "1.12"})["abi_minor"] == 12, "presets with abi 1.12 declared")
    # OS-MODRATE: an instrument's RATE flag: ABI 1.13, O3M_FX_RATE in the instrument's fx_flags through ENTRY_FX
    rated = validate({**base, "rate": True})
    rated_text = generate(rated)
    expect(rated["abi_minor"] == 13 and "#define T_ENTRY_FX 0, 0, O3M_FX_RATE, 0, 0" in rated_text and "#define T_ABI_MINOR 13" in rated_text, "rate makes an instrument ABI 1.13 and sets O3M_FX_RATE")
    expect("rate 1" in expectations(rated) and "rate" not in expectations(validate(base)), "the expectations carry the rate flag")
    expect(validate({**base, "rate": True, "abi": "1.13"})["abi_minor"] == 13 and validate({**base, "abi": "1.13"})["abi_minor"] == 13, "abi 1.13 may be declared")
    expect("#define T_ENTRY_FX 0, 0, 0, t_presets, T_PRESET_COUNT" not in rated_text and validate({**base, "rate": False})["abi_minor"] == 0, "no rate, no 1.13")
    refused({**base, "rate": True, "abi": "1.12"}, "needs abi 1.13")
    refused({**fx, "rate": True}, "instrument's")
    refused({**base, "rate": 1}, "true or false")
    # OS-INSFX-FX2: an effect's labels (PING-PONG's Sync): ABI 1.12, the label table through ENTRY_FX
    lab_fx = {**fx, "params": [{"name": "Sync", "min": 0, "max": 2, "default": 1, "type": "int", "labels": ["FREE", "1", "2"]}] + fx["params"][1:]}
    lab = validate({**lab_fx, "stereo": "widens"})
    lab_text = generate(lab)
    expect(lab["abi_minor"] == 12 and "#define T_ENTRY_FX(render_st_fn) 0, 0, 0, t_labels, 0, (render_st_fn), O3M_FX_WIDENS, 0, 0" in lab_text and "static const o3m_labels t_labels[T_PARAM_COUNT] = {" in lab_text, "an effect's labels fill the 1.8 tail of its 1.12 descriptor")
    expect(validate(lab_fx)["abi_minor"] == 12 and "#define T_ENTRY_FX 0, 0, 0, t_labels, 0, 0, 0, 0, 0" in generate(validate(lab_fx)), "labels alone make an effect 1.12")
    expect("labels 0 FREE|1|2" in expectations(lab), "the expectations carry an effect's labels")
    expect("0, 0, 0, 0, 0, (render_st_fn)" in generate(wide), "an effect without labels keeps its NULL tail")
    refused({**lab_fx, "abi": "1.8"}, "need abi 1.12")
    refused({**fx, "params": [{"name": "Sync", "min": 0, "max": 1, "default": 1, "type": "bool", "labels": ["A", "B"]}]}, "int parameters only")
    refused({"name": "T", "id": "t", "kind": "app", "state_bytes": 8, "params": [{"name": "A", "min": 0, "max": 1, "default": 0, "type": "int", "labels": ["A", "B"]}]}, "instrument's or an effect's")
    refused({**fx, "stereo": "mid_side"}, "stereo must be")
    refused({**fx, "stereo": True}, "stereo must be")
    refused({**base, "stereo": "widens"}, "effect's")
    refused({**fx, "stereo": "widens", "abi": "1.11"}, "need abi 1.12")
    refused({**fx, "presets": [{"name": "P", "values": {"Depth": 10}}], "abi": "1.8"}, "need abi 1.12")
    refused({**fx, "presets": [{"name": "P", "values": {"Rate": 1}}]}, "names no parameter")
    refused({**fx, "presets": [{"name": "P", "values": {"Depth": 101}}]}, "outside its range")
    refused({**fx, "presets": [{"name": "P", "values": {"Beats": 2.5}}]}, "whole-number")
    refused({**fx, "presets": [{"name": "P", "values": {"Depth": float("nan")}}]}, "finite")
    refused({**fx, "presets": [{"name": "P", "values": {"Depth": "10"}}]}, "not a number")
    refused({**fx, "presets": [{"name": "P", "values": {}}]}, "at least one parameter")
    refused({**fx, "presets": [{"name": "P"}]}, "must be")
    refused({**fx, "presets": [{"name": "", "values": {"Depth": 1}}]}, "name")
    refused({**fx, "presets": [{"name": "A" * 16, "values": {"Depth": 1}}]}, "name")
    refused({**fx, "presets": [{"name": "P", "values": {"Depth": 1}}, {"name": "P", "values": {"Depth": 2}}]}, "taken by an earlier preset")
    refused({**fx, "presets": [{"name": "P%d" % i, "values": {"Depth": 1}} for i in range(33)]}, "at most 32")
    refused({**fx, "presets": "none"}, "presets must be a list")
    refused({**fx, "params": [], "presets": [{"name": "P", "values": {"Depth": 1}}]}, "no parameters")
    refused({"name": "T", "id": "t", "kind": "app", "state_bytes": 8, "views": [{"name": "body"}], "params": [{"name": "A", "min": 0, "max": 1, "default": 0}], "presets": [{"name": "P", "values": {"A": 1}}]}, "an app and an importer have none")
    expect(len(validate({**fx, "presets": [{"name": "P%d" % i, "values": {"Depth": 1}} for i in range(32)]})["presets"]) == 32, "32 presets are the most")
    keyed = validate({**fx, "params": fx["params"] + [{"name": "Key", "type": "key"}]})
    kp = keyed["params"][-1]
    expect(keyed["abi_minor"] == 12 and kp["type"] == "key" and kp["min"] == 0 and kp["max"] == 16 and kp["default"] == 0 and kp["learnable"] is True, "a key parameter makes an effect ABI 1.12: 0..16, default 0")
    expect(param_flags(kp) == F_INT | F_KEY and "O3M_PARAM_INT | O3M_PARAM_KEY" in generate(keyed) and "{\"Key\", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY}," in generate(keyed), "the key parameter's flags are INT and KEY")
    expect("T_ENTRY_FX" not in generate(keyed), "a key alone needs no tail: the members default")
    expect(validate({**fx, "params": fx["params"] + [{"name": "Key", "type": "key", "min": 0, "max": 16, "default": 3}, {"name": "Key 2", "type": "key"}]})["params"][-2]["default"] == 3, "two keys, a default")
    refused({**base, "params": [{"name": "Key", "type": "key"}]}, "effect's")
    refused({**fx, "params": fx["params"] + [{"name": "K1", "type": "key"}, {"name": "K2", "type": "key"}, {"name": "K3", "type": "key"}]}, "at most 2")
    refused({**fx, "params": fx["params"] + [{"name": "Key", "type": "key", "max": 8}]}, "min 0 (no key), max 16")
    refused({**fx, "params": fx["params"] + [{"name": "Key", "type": "key", "default": 17}]}, "default")
    refused({**fx, "params": fx["params"] + [{"name": "Key", "type": "key", "default": 1.5}]}, "whole")
    for banned in ("unit", "curve", "learnable", "mod", "labels"):
        refused({**fx, "params": fx["params"] + [{"name": "Key", "type": "key", banned: "x"}]}, "takes no '%s'" % banned)
    refused({**fx, "params": fx["params"] + [{"name": "Key", "type": "key"}], "abi": "1.11"}, "need abi 1.12")
    refused({**fx, "params": fx["params"] + [{"name": "Key", "type": "pan"}]}, "type must be")
    view = validate({**base, "views": [{"name": "Filter Curve", "min_size": [120, 80]}]})
    expect(view["abi_minor"] == 1 and view["views"][0]["ident"] == "FILTER_CURVE" and view["views"][0]["preferred_size"] == [120, 80], "a view makes it ABI 1.1")
    app = validate({"name": "Tool", "id": "tool", "kind": "app", "state_bytes": 8, "views": [{"name": "body"}]})
    expect(app["abi_minor"] == 1, "an app is ABI 1.1")
    # OS-H1 (ABI 1.3): the capabilities of an app
    tool = {"name": "Tool", "id": "tool", "kind": "app", "state_bytes": 8, "views": [{"name": "body"}]}
    capped = validate({**tool, "capabilities": ["midi_in", "edit_project"]})
    expect(capped["caps"] == 9 and capped["abi_minor"] == 3 and app["caps"] == 0, "capabilities make an app ABI 1.3 and set the mask")
    expect(validate({**tool, "capabilities": ["read_project"], "abi": "1.3"})["caps"] == 2 and validate({**tool, "abi": "1.3"})["abi_minor"] == 3, "abi 1.3 may be declared")
    expect(validate({**tool, "capabilities": ["play_notes"]})["abi_minor"] == 10 and validate({**tool, "capabilities": ["play_notes"]})["caps"] == 64, "play_notes makes an app ABI 1.10 (OS-APPUX S3)")
    expect(validate({**tool, "abi": "1.10"})["abi_minor"] == 10 and validate({**tool, "abi": "1.10", "capabilities": ["read_project", "play_notes"]})["caps"] == 66, "abi 1.10 may be declared, with the bit")
    refused({**tool, "abi": "1.9", "capabilities": ["play_notes"]}, "capabilities need abi")
    expect("#define TOOL_CAPS 9u" in generate(capped) and "#define TOOL_ABI_MINOR 3" in generate(capped) and "caps 9" in expectations(capped), "the header and the expectations carry the mask")
    tap = validate({**tool, "capabilities": ["audio_tap", "midi_in"]})
    expect(tap["caps"] == 5 and tap["abi_minor"] == 4 and validate({**tool, "abi": "1.4"})["abi_minor"] == 4, "audio_tap makes an app ABI 1.4")
    expect(validate({**tool, "capabilities": ["read_project", "edit_project"], "abi": "1.5"})["abi_minor"] == 5 and validate({**tool, "abi": "1.5"})["abi_minor"] == 5, "abi 1.5 may be declared (the step and edit-group entries)")
    expect(validate({**tool, "capabilities": ["audio_tap"], "abi": "1.5"})["abi_minor"] == 5, "1.5 covers 1.4's capability")
    expect(validate({**tool, "capabilities": ["read_project", "edit_project"], "abi": "1.6"})["abi_minor"] == 6 and validate({**tool, "abi": "1.6"})["abi_minor"] == 6, "abi 1.6 may be declared (the instrument-parameter entries)")
    refused({**tool, "capabilities": ["audio_tap"], "abi": "1.3"}, "need abi 1.3")
    exp = validate({**tool, "capabilities": ["read_project", "export_files"]})
    expect(exp["caps"] == 18 and exp["abi_minor"] == 7 and validate({**tool, "abi": "1.7"})["abi_minor"] == 7, "export_files makes an app ABI 1.7")
    refused({**tool, "capabilities": ["export_files"], "abi": "1.6"}, "need abi")
    refused({**tool, "capabilities": ["midi_out"]}, "capabilities")
    refused({**tool, "capabilities": ["midi_in", "midi_in"]}, "distinct")
    refused({**tool, "capabilities": "midi_in"}, "list")
    refused({**tool, "capabilities": ["midi_in"], "abi": "1.2"}, "need abi 1.3")
    refused({**base, "capabilities": ["midi_in"]}, "an app's")
    # ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): pitch, pages and labels
    five = [{"name": "Wave", "min": 0, "max": 2, "default": 0, "type": "int", "labels": ["Saw", "Square", "Tri"]}] + [{"name": "P%d" % i, "min": 0, "max": 1, "default": 0} for i in range(1, 10)]
    p18 = validate({**base, "params": five, "pitch": True, "pages": [{"name": "Osc", "first": 0}, {"name": "Filter", "first": 8}]})
    expect(p18["abi_minor"] == 8 and p18["pitch"] is True and len(p18["pages"]) == 2, "pitch, pages and labels make an instrument ABI 1.8")
    expect(validate({**base, "pitch": True})["abi_minor"] == 8 and validate({**base, "abi": "1.8"})["abi_minor"] == 8 and validate(base)["abi_minor"] == 0, "pitch makes it 1.8, a plain module stays as it was")
    t18 = generate(p18)
    for fragment in ("#define T_ABI_MINOR 8", "O3M_MODULE_SIZE(T_ABI_MINOR)", "static const char* const t_labels_wave[3] = {\"Saw\", \"Square\", \"Tri\"};", "static const o3m_labels t_labels[T_PARAM_COUNT] = {",
                     "  {t_labels_wave, 3},", "  {0, 0},", "static const o3m_page t_pages[2] = {", "  {\"Filter\", 8},", "#define T_ENTRY_TAIL(pitch_fn) (pitch_fn), t_pages, 2, t_labels", "#define T_HAS_PITCH 1"):
        expect(fragment in t18, "the 1.8 header lacks '%s'" % fragment)
    expect("T_ENTRY_TAIL" not in generate(validate(base)) and "O3M_MODULE_SIZE(T_ABI_MINOR)" in generate(validate(base)), "a plain module has no tail and still writes the size through the macro")
    expect("pitch 1" in expectations(p18) and "page 1 Filter 8" in expectations(p18) and "labels 0 Saw|Square|Tri" in expectations(p18) and "pitch" not in expectations(validate(base)), "the expectations carry the 1.8 members")
    refused({**base, "pitch": 1}, "pitch must be")
    refused({**base, "pitch": True, "abi": "1.7"}, "need abi 1.8")
    refused({**tool, "pitch": True}, "instrument's")
    refused({**base, "pages": [{"name": "A", "first": 0}] * 5}, "at most 4")
    refused({**base, "pages": [{"name": "Abcdefgh", "first": 0}]}, "name")
    refused({**base, "pages": [{"name": "A", "first": 1}]}, "first page starts at parameter 0")
    refused({**base, "params": five, "pages": [{"name": "A", "first": 0}]}, "1..8 parameters")  # ten parameters on one page
    refused({**base, "pages": [{"name": "A", "first": 0}, {"name": "B", "first": 0}]}, "ascend")
    refused({**base, "pages": [{"name": "A", "first": 0}, {"name": "B", "first": 1}]}, "cover")  # B would start at the end: an empty page
    refused({**base, "params": [], "pages": [{"name": "A", "first": 0}]}, "no parameters")
    refused({**base, "pages": [{"name": "A"}]}, "must be")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 1, "default": 0, "labels": ["a", "b"]}]}, "int parameters only")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 2, "default": 0, "type": "int", "labels": ["a", "b"]}]}, "one text for each value")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 0, "default": 0, "type": "int", "labels": ["a"]}]}, "min < max")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 16, "default": 0, "type": "int", "labels": ["a"] * 17}]}, "one text for each value")
    refused({**base, "params": [{"name": "A", "min": 0, "max": 1, "default": 0, "type": "int", "labels": ["a", "abcdefghijkl"]}]}, "a label")
    refused({**tool, "params": [{"name": "A", "min": 0, "max": 1, "default": 0, "type": "int", "labels": ["a", "b"]}]}, "instrument's")
    # flags and the header text
    m = validate({**base, "params": [{"name": "Cutoff", "min": 20, "max": 20000, "default": 1000, "curve": "log", "unit": "hz", "mod": True},
                                      {"name": "On", "min": 0, "max": 1, "default": 1, "type": "bool", "learnable": False},
                                      {"name": "Steps", "min": -12, "max": 12, "default": 0, "type": "int", "unit": "semitones"}]})
    text = generate(m)
    for fragment in ("T_PARAM_CUTOFF = 0,", "T_PARAM_ON = 1,", "T_PARAM_COUNT = 3", "O3M_PARAM_LOG | O3M_PARAM_MOD | O3M_PARAM_WITH_UNIT(O3M_UNIT_HZ)",
                     "O3M_PARAM_BOOL | O3M_PARAM_NO_LEARN", "O3M_PARAM_INT | O3M_PARAM_WITH_UNIT(O3M_UNIT_SEMITONES)", "{\"Cutoff\", 20.0f, 20000.0f, 1000.0f,",
                     "#define T_ABI_MINOR 0", "T_VIEW_COUNT = 0", "#define T_ENTRY_VIEWS(ui) (ui), T_VIEW_COUNT", "O3M_MODULE_SIZE(T_ABI_MINOR)"):
        expect(fragment in text, "generated header lacks '%s'" % fragment)
    expect(param_flags(m["params"][0]) == (F_LOG | F_MOD | (2 << 8)), "flag value of Cutoff")
    expect(param_flags(m["params"][1]) == (F_BOOL | F_NO_LEARN), "flag value of On")
    expect(generate(m) == text, "the generator is deterministic")
    expect(c_float(0.1) == "0.100000001f" and c_float(3) == "3.0f" and c_float(-48) == "-48.0f", "float literals")
    # the reader against a hand-made file
    img = bytearray(160)
    DESC.pack_into(img, 0, 1, 1, 1, 64, 100, 64, 1, 80, 0x1, 0x3, 0x5, 0x7, 0x9, 0xB, 0, 0)
    PARAM.pack_into(img, 80, 104, 0.0, 1.0, 0.5, 0)
    img[100:104] = b"Syn\0"
    img[104:108] = b"P0\0\0"
    header = HEADER.pack(FILE_MAGIC, 64, 1, 1, 1, 1, 0, 0, 160, 0, 64, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0)
    crc = zlib.crc32(bytes(img), zlib.crc32(header)) & 0xFFFFFFFF
    header = HEADER.pack(FILE_MAGIC, 64, 1, 1, 1, 1, 0, 0, 160, 0, 64, 0, 1, 0, 0, 0, 0, crc, 0, 0, 0)
    got = read_tmod(header + bytes(img))
    expect(got["name"] == "Syn" and got["params"][0]["name"] == "P0" and got["params"][0]["default"] == 0.5 and got["kind"] == 1, "reader")
    try:
        read_tmod(header + bytes(img)[:-4] + b"\1\0\0\0")
        failures.append("reader accepted a damaged file")
    except ManifestError:
        pass
    for f in failures:
        print("FAIL", f)
    print("o3m_gen self-test: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


def main(argv):
    if argv[1:2] == ["--self-test"]:
        return self_test()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("manifest")
    g.add_argument("--out")
    c = sub.add_parser("check")
    c.add_argument("manifest")
    c.add_argument("header")
    e = sub.add_parser("expect")
    e.add_argument("manifest")
    e.add_argument("--out")
    n = sub.add_parser("new")
    n.add_argument("--kind", required=True, choices=TEMPLATE_NAMES)
    n.add_argument("--id", required=True)
    n.add_argument("--name")
    n.add_argument("--out", required=True)
    d = sub.add_parser("dump")
    d.add_argument("file")
    v = sub.add_parser("verify")
    v.add_argument("manifest")
    v.add_argument("file")
    v.add_argument("--layout-tag", type=lambda x: int(x, 0), default=None, help="OS-FLASHDIET: the importer layout tag of word 44 (default: --layout-tag-file, else the pinned IMPORT_LAYOUT_TAG)")
    v.add_argument("--layout-tag-file", default=None, help="a file holding the tag printed from model::kImportLayoutTag")
    args = ap.parse_args(argv[1:])
    try:
        if args.cmd == "gen":
            text = generate(load_manifest(args.manifest))
            if args.out:
                Path(args.out).write_text(text)
            else:
                sys.stdout.write(text)
            return 0
        if args.cmd == "check":
            want = generate(load_manifest(args.manifest))
            have = Path(args.header).read_text()
            if want != have:
                print("o3m_gen: %s is not what %s generates: run `o3m_gen.py gen %s --out %s`" % (args.header, args.manifest, args.manifest, args.header), file=sys.stderr)
                return 1
            return 0
        if args.cmd == "expect":
            text = expectations(load_manifest(args.manifest))
            if args.out:
                Path(args.out).write_text(text)
            else:
                sys.stdout.write(text)
            return 0
        if args.cmd == "new":
            for name in scaffold(args.kind, args.id, args.name, args.out):
                print(name)
            return 0
        if args.cmd == "dump":
            print(json.dumps(read_tmod(Path(args.file).read_bytes()), indent=2))
            return 0
        tag = IMPORT_LAYOUT_TAG
        if getattr(args, "layout_tag_file", None):
            tag = int(Path(args.layout_tag_file).read_text().strip(), 0)
        elif getattr(args, "layout_tag", None) is not None:
            tag = args.layout_tag
        diffs = verify(load_manifest(args.manifest), Path(args.file).read_bytes(), tag)
        for line in diffs:
            print("o3m_gen: %s" % line, file=sys.stderr)
        return 1 if diffs else 0
    except (ManifestError, OSError) as exc:
        print("o3m_gen: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
