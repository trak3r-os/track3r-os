#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the HIL scenarios of this directory (python3 gen_hil.py [outdir]). Not a scenario itself (o3hil reads only *.json)."""
import json
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

# ---- step constructors -------------------------------------------------------------------------------------------------------------


def _extra(s, kw):
    for k, v in kw.items():
        if v is not None:
            s[k] = v
    return s


def K(*keys, gap=None, repeat=None, **kw):
    return _extra(_extra({"keys": list(keys)}, {"gap": gap, "repeat": repeat}), kw)


def ST(state, ne=None, within=None, poll=None, snap=None, **kw):
    return _extra(_extra({"state": state}, {"ne": ne, "within": within, "poll": poll, "snap": snap}), kw)


def PAD(n, gesture=None, vel=None, **kw):
    return _extra(_extra({"pad": n}, {"gesture": gesture, "vel": vel}), kw)


def ENC(n, **kw):
    return _extra({"enc": n}, kw)


def W(sec, **kw):
    return _extra({"wait": sec}, kw)


def CMD(cmd, expect=None, expect_not=None, within=None, poll=None, timeout=None, fail_on=None, **kw):
    return _extra(_extra({"cmd": cmd}, {"expect": expect, "expect_not": expect_not, "within": within, "poll": poll, "timeout": timeout, "fail_on": fail_on}), kw)


def MIX(spec, **kw):
    return _extra({"mix": spec}, kw)


def LEDS(spec, **kw):
    return _extra({"leds": spec}, kw)


def VOX(spec, **kw):
    return _extra({"voices": spec}, kw)


def SHOT(name, compare=None, non_blank=True, **kw):
    s = {"shot": name}
    if compare is not None:
        s["compare"] = compare
    if non_blank:
        s["non_blank"] = True
    return _extra(s, kw)


def DISK(op, within=None, ro=None, size_mb=None, **kw):
    """OS-USBDISK U1d, the PC side (o3disk.py: Linux only, the unit's own removable disk only)."""
    return _extra(_extra({"disk": op}, {"within": within, "ro": ro, "size_mb": size_mb}), kw)


def STAT(name, min_n=None, **kw):
    """The count, mean, min and max of the numbers the CMD steps' `collect` kept under `name` (o3hil)."""
    return _extra({"stat": name}, dict(kw, min_n=min_n))


def RUN(name, **kw):
    return _extra({"run": name}, kw)


def SAY(text):
    return {"say": text}


KEYROW = ["0,458,800,22"]


def CMP(scenario, snap):
    """A comparison of the key-cap row only (the part of a screen that does not depend on the project) with the simulator's picture."""
    return {"sim_scenario": "sim/scenarios/%s.txt" % scenario, "snap": snap, "regions": KEYROW, "tol": 24, "aa": 1, "max_bad": 0.005}


OVERLAY_CAPTURING = "~^(LEARN|PATTERN_CHOOSER|LENGTH_CHOOSER|JUMP_CHOOSER|MODULE_PICKER|STEP_POPUP)$"


def clear_overlays():
    return [K("f7", gap=0.3, if_state={"overlay": "NAVIGATOR"}, note="(a leftover Navigator is closed: F7 is CLOSE, F8 would GO since OS-NAV2)"),
            K("f8", gap=0.3, if_state={"overlay": OVERLAY_CAPTURING}, note="(a leftover overlay is put away: F8 is Back)"),
            K("f7", gap=0.3, if_state={"overlay": "TEXT_ENTRY"}, note="(the on-screen keyboard is cancelled with F7, never accepted)")]


def to_pattern():
    return [K("pattern", gap=0.4), ST({"screen": "PATTERN"}, within=3)]


def start_on_pattern(load=True):
    """The common beginning: the project, the Pattern screen, no overlay, stopped, Shift up."""
    steps = []
    if load:
        steps.append(RUN("_load_tv16"))
    steps += [K("-shift", gap=0.2, note="(Shift released, in case a former run left it down)")] + clear_overlays() + to_pattern()
    steps += [RUN("_stop"), CMD("cpu reset", note="(the cpu counters are since boot or since this reset: a scenario that checks underruns / over-budget starts from zero)")]
    return steps


def pad_mode_notes():
    return K("+shift", "f2", "-shift", gap=0.3)  # OS-PADOCT: Shift + F2 is NOTES (F1 is the octave)


def undo_many(n, gap=0.25):
    return K("f8", repeat=n, gap=gap)


def redo_many(n, gap=0.25):
    return K("+shift", "f8", "-shift", repeat=n, gap=gap)


# OS-UNDOGRP: F8 on an empty history says NOTHING TO UNDO HERE / NOTHING TO REDO HERE (state line: underscores). The journal of a long scenario is deeper than a fixed count of
# presses guesses, so these press until the strip says so (at most `max_rounds` rounds).
NOTHING_UNDO = "~^NOTHING_TO_UNDO_HERE$"
NOTHING_REDO = "~^NOTHING_TO_REDO_HERE$"


def undo_until_empty(gap=0.25, max_rounds=120):
    return K("f8", gap=gap, until={"msg": NOTHING_UNDO}, max=max_rounds)


def redo_until_empty(gap=0.25, max_rounds=120):
    return K("+shift", "f8", "-shift", gap=gap, until={"msg": NOTHING_REDO}, max=max_rounds)


def scenario(name, title, steps, finally_=None, needs=None, vars_=None):
    d = {"name": name, "title": title}
    if needs:
        d["needs"] = needs
    if vars_:
        d["vars"] = vars_
    d["steps"] = steps
    if finally_:
        d["finally"] = finally_
    return d


def write(d):
    path = os.path.join(OUT, d["name"] + ".json")
    lines = ['{', ' "name": %s,' % json.dumps(d["name"]), ' "title": %s,' % json.dumps(d["title"])]
    if "needs" in d:
        lines.append(' "needs": %s,' % json.dumps(d["needs"]))
    if "vars" in d:
        lines.append(' "vars": %s,' % json.dumps(d["vars"]))
    for section in ("steps", "finally"):
        if section in d:
            body = ",\n".join("  " + json.dumps(s) for s in d[section])
            lines.append(' "%s": [\n%s\n ]%s' % (section, body, "," if section == "steps" and "finally" in d else ""))
    lines.append("}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


SCENARIOS = []


def add(*a, **kw):
    SCENARIOS.append(scenario(*a, **kw))


STANDARD_FINALLY = [K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_stop")]

# ======================================================================================================================================
# 1. nav_all_screens
# ======================================================================================================================================
nav = start_on_pattern()
nav += [
    SAY("PATTERN"),
    ST({"screen": "PATTERN", "overlay": "NONE"}, within=3),
    SHOT("pattern", compare=CMP("screens", "pattern_start"), soft=True, note="(soft: the key-cap row against the simulator's golden; needs --sim or --golden-dir)"),
    # SONG, two pages
    K("song", gap=0.5), ST({"screen": "SONG"}, within=3),
    K("song", gap=0.4, if_state={"page": "1"}, note="(page 0 first: Song again cycles ORDER / ARRANGEMENT)"), ST({"screen": "SONG", "page": "0"}, within=2),
    SHOT("song_order", compare=CMP("screens", "song_start"), soft=True),
    K("song", gap=0.4), ST({"screen": "SONG", "page": "1"}, within=2),
    SHOT("song_arrangement", compare=CMP("screens", "song_page2"), soft=True),
    K("song", gap=0.4), ST({"screen": "SONG", "page": "0"}, within=2),
    # INSTRUMENT (Params): F1 the SOURCE page, F2..F5 the four pages (the engine picker is the F4 of a free slot, OS-INSTUI S5: below)
    K("params", gap=0.5), ST({"screen": "INSTRUMENT"}, within=3),
    K("f2", gap=0.4), ST({"screen": "INSTRUMENT", "page": "0", "overlay": "NONE"}, within=2),
    SHOT("params_voice", compare=CMP("params", "voice"), soft=True),
    K("f3", gap=0.4), ST({"screen": "INSTRUMENT", "page": "1"}, within=2),
    SHOT("params_filter", compare=CMP("params", "filter"), soft=True),
    K("f4", gap=0.4), ST({"screen": "INSTRUMENT", "page": "2"}, within=2),
    SHOT("params_env", compare=CMP("params", "env_cutoff"), soft=True),
    K("f5", gap=0.4), ST({"screen": "INSTRUMENT", "page": "3"}, within=2),
    SHOT("params_sends", compare=CMP("params", "sends"), soft=True),
    K("f1", gap=0.5), ST({"screen": "INSTRUMENT", "overlay": "NONE"}, within=3, note="F1 is the SOURCE page (SAMPLE), not the engine picker since OS-INSTUI"),
    K("+vol", "+fx", gap=0.0, note="(the Learn chord, FX1 + FX2 within 40 ms: soft, the console round trip may be slower than the chord window)"),
    ST({"overlay": "LEARN"}, within=2, soft=True),
    K("-fx", "-vol", gap=0.1),
    SHOT("params_learn", soft=True),
    K("f8", gap=0.4, if_state={"overlay": "LEARN"}), ST({"overlay": "NONE"}, within=3),
    SAY("the engine picker: F4 on a free slot (instrument 17 of t-v16), then back to instrument 1"),
    K("f6", gap=0.08, repeat=70, note="(to instrument 1: F6 stops there, no wrap)"),
    K("f7", gap=0.1, repeat=16), ST({"instrument": "17"}, within=3),
    K("f4", gap=0.5), ST({"overlay": "MODULE_PICKER"}, within=3),
    SHOT("params_engine_picker"),
    K("f8", gap=0.4), ST({"overlay": "NONE"}, within=3),
    K("f6", gap=0.08, repeat=70),
    # WAVE / CHOP
    K("sampleplay", gap=0.5), ST({"screen": "WAVE"}, within=3),
    K("sampleplay", gap=0.4, if_state={"page": "1"}), ST({"screen": "WAVE", "page": "0"}, within=2),
    SHOT("wave"),
    K("sampleplay", gap=0.4), ST({"screen": "WAVE", "page": "1"}, within=2),
    SHOT("chop"),
    K("sampleplay", gap=0.4), ST({"screen": "WAVE", "page": "0"}, within=2),
    # SAMPLE EDIT, SAMPLE LOAD
    K("sampleedit", gap=0.5), ST({"screen": "SAMPLE_EDIT"}, within=3),
    SHOT("sample_edit"),
    K("sampleload", gap=0.5), ST({"screen": "SAMPLE_LOAD"}, within=3),
    W(1.5, note="(the listing is a scan of the card)"),
    SHOT("sample_load"),
    # RECORD, three pages (never armed: F1 and Rec are not pressed)
    K("samplerec", gap=0.5), ST({"screen": "RECORD"}, within=3),
    K("samplerec", gap=0.4, if_state={"page": "1"}), K("samplerec", gap=0.4, if_state={"page": "2"}), ST({"screen": "RECORD", "page": "0"}, within=2),
    SHOT("record_sources"),
    K("samplerec", gap=0.4), ST({"screen": "RECORD", "page": "1"}, within=2),
    SHOT("record_resample"),
    K("samplerec", gap=0.4), ST({"screen": "RECORD", "page": "2"}, within=2),
    SHOT("record_radio"),
    K("samplerec", gap=0.4), ST({"screen": "RECORD", "page": "0"}, within=2),
    # MIXER, four pages (F1..F4)
    K("master", gap=0.5), ST({"screen": "MIXER"}, within=3),
    K("f1", gap=0.4), ST({"screen": "MIXER", "page": "0"}, within=2), SHOT("mixer_mixer"),
    K("f2", gap=0.4), ST({"screen": "MIXER", "page": "1"}, within=2), SHOT("mixer_routing"),
    K("f3", gap=0.4), ST({"screen": "MIXER", "page": "2"}, within=2), SHOT("mixer_master_fx"),
    K("f4", gap=0.4), ST({"screen": "MIXER", "page": "3"}, within=2), SHOT("mixer_stems"),
    K("f1", gap=0.4), ST({"screen": "MIXER", "page": "0"}, within=2),
    # PERFORM, FILE
    K("performance", gap=0.5), ST({"screen": "PERFORM"}, within=3), SHOT("perform"),
    K("file", gap=0.5), ST({"screen": "FILE"}, within=3), W(1.0), SHOT("file"),
    # CONFIG, four pages
    K("config", gap=0.5), ST({"screen": "CONFIG"}, within=3),
    K("f1", gap=0.4), ST({"screen": "CONFIG", "page": "0"}, within=2), SHOT("config_midi"),
    K("f2", gap=0.4), ST({"screen": "CONFIG", "page": "1"}, within=2), SHOT("config_display"),
    K("f3", gap=0.4), ST({"screen": "CONFIG", "page": "2"}, within=2), SHOT("config_sd"),
    K("f4", gap=0.4), ST({"screen": "CONFIG", "page": "3"}, within=2), SHOT("config_info"),
    K("f1", gap=0.4), ST({"screen": "CONFIG", "page": "0"}, within=2),
    # NAVIGATOR and MODULES (OS-NAV2, 0.4.9: Shift + Pattern, tapped or held, opens the grid and it STAYS open; F7 or Pattern closes with no view change; F8 or F1 = GO; Shift + F5 = the last cell, MODULES)
    K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=3),
    K("+shift", "+pattern", "-pattern", "-shift", gap=0.5, note="(Shift + Pattern tapped: the grid opens and stays open after both are released)"),
    ST({"overlay": "NAVIGATOR"}, within=3),
    SHOT("navigator"),
    K("f7", gap=0.4), ST({"screen": "PATTERN", "overlay": "NONE"}, within=3, note="F7 = CLOSE: no view change"),
    K("+shift", "+pattern", "-pattern", "-shift", gap=0.5), ST({"overlay": "NAVIGATOR"}, within=3),
    K("pattern", gap=0.4), ST({"screen": "PATTERN", "overlay": "NONE"}, within=3, note="Pattern again closes it"),
    K("+shift", "+pattern", "-pattern", "-shift", gap=0.5), ST({"overlay": "NAVIGATOR"}, within=3),
    ENC(32, gap=0.4, note="(the encoder moves one cell a detent and clamps at the last cell, MODULES; Shift + F5 is unbound in the Navigator since OS-SHIFTROW)"),
    K("f8", gap=0.5, note="F8 = GO"),
    ST({"screen": "MODULES", "overlay": "NONE"}, within=3),
    SHOT("modules"),
    K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=3),
]
add("nav_all_screens", "every screen and page the buttons reach: state screen/page, a shot that is not blank, the key-cap row against the simulator where it has a snap (soft)", nav,
    finally_=[K("-pattern", "-shift", gap=0.2), K("-vol", "-fx", gap=0.1)] + clear_overlays() + [K("pattern", gap=0.5)])

# ======================================================================================================================================
# 2. pattern_edit_undo
# ======================================================================================================================================
HOME = K("+shift", "insert", "-shift", gap=0.3)
edit = start_on_pattern() + [
    K("rec", gap=0.4, if_state={"mode": "3"}, note="(Rec toggles Browse <-> Edit: only when it is in Browse)"),
    ST({"mode": "0"}, within=2, note="mode 0 = Edit"),
    HOME, ST({"row": "0"}, within=2),
    K("+shift", "f8", "-shift", gap=0.3, note="(redo on an empty journal says NOTHING TO REDO: it makes the last message a known one)"),
    ST({"msg": NOTHING_REDO}, within=2, soft=True, note="(soft: the journal is empty after a load)"),
    SHOT("edit_before"),
    K("note", gap=0.4), ST({"col": "NOTE"}, within=2),
    PAD(17, note="E1: a pad enters a note (and the cursor moves on by JUMP)"),
    ST({"mode": "0"}, ne={"row": "0"}, within=2, soft=True, note="(soft: JUMP may be 0 in this project)"),
    HOME, ST({"row": "0"}, within=2),
    ENC(3, note="E2: the encoder raises the note of the step by 3 semitones"),
    K("instr", gap=0.4), ST({"col": "INSTR"}, within=2),
    PAD(4, note="E3: a pad in the INSTR column picks instrument pad + 1 (here 5) and writes no note of its own (OS-AUDIT E-PAT-12; one undo unit)"),
    HOME, ST({"row": "0"}, within=2),
    K("vol", gap=0.4), ST({"col": "VOL"}, within=2),
    PAD(9, note="E4: a pad in the VOL column enters a volume"),
    HOME, ST({"row": "0"}, within=2),
    K("fx", gap=0.4), ST({"col": "~^FX[12]$"}, within=2),
    PAD(3, note="E5: a pad in an FX column enters an FX value"),
    HOME,
    K("+fx", gap=0.1, note="E6: Fx held + the encoder is the FX type (soft: it depends on the held-encoder gesture)"), ENC(2, soft=True), K("-fx", gap=0.3),
    ST({"screen": "PATTERN", "overlay": "~^(NONE|NOTIFY)$"}, within=2, soft=True),
    SHOT("edit_after"),
    SAY("undo: the first F8 must not say NOTHING TO UNDO (an edit is in the journal), then F8 is pressed until it does (at most 120 times: the history may hold more than this scenario's edits)"),
    K("f8", gap=0.4),
    ST({}, ne={"msg": NOTHING_UNDO}, within=2),
    undo_until_empty(),
    ST({"msg": NOTHING_UNDO}, within=3, note="the journal is empty (drained by the until above, whatever else the history held: mutes, sample edits): every edit of this scenario is undone"),
    SHOT("edit_undone"),
    SAY("redo: one redo must work (the last message stays NOTHING TO UNDO), then the redo stack is drained"),
    K("+shift", "f8", "-shift", gap=0.4),
    ST({"msg": NOTHING_UNDO}, within=2, note="a redo that worked says nothing"),
    redo_until_empty(),
    ST({"msg": NOTHING_REDO}, within=3, note="every edit is done again"),
    SHOT("edit_redone"),
    undo_until_empty(),
    ST({"msg": NOTHING_UNDO}, within=3, note="and undone again: the pattern is back as loaded, nothing was saved"),
]
add("pattern_edit_undo", "Pattern in Edit: note, instrument, volume and FX of a step by pads and encoder, undo to the empty journal, redo to the empty stack, undo again (nothing saved)", edit,
    finally_=[K("-fx", "-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), undo_until_empty(), K("rec", gap=0.4, if_state={"mode": "0"}), RUN("_stop")])

# ======================================================================================================================================
# 3. pad_modes
# ======================================================================================================================================
# Instrument 1 selected on the Params screen: Shift + pad 0 (OS-INSTUI S2: a bare pad there PLAYS the instrument in view, so the old "pad 0 selects it" held only when instrument 1 was already in view,
# which a start from another instrument's page (the MIDI page of 49, say) broke: OS-HILSCEN2).
SELECT_1 = [K("+shift", gap=0.3), PAD(0, note="Shift + pad selects instrument 1"), K("-shift", gap=0.3)]


def shift_pick(fkey):
    return K("+shift", fkey, "-shift", gap=0.3)


pm = start_on_pattern() + [
    pad_mode_notes(), ST({"pads": "NOTES", "shift": "0"}, within=2),
    LEDS({"online": True, "within": 3, "snap": "notes"}),
    shift_pick("f3"), ST({"pads": "PIANO"}, within=2), LEDS({"differs_from": "notes", "within": 3, "snap": "piano"}),
    PAD(12, "down", note="PIANO: a pad plays the instrument"), VOX({"min_in_use": 1}, soft=True), PAD(12, "up"),
    shift_pick("f5"), ST({"pads": "TRACKS"}, within=2), LEDS({"differs_from": "piano", "within": 3, "snap": "tracks"}),
    shift_pick("f4"), ST({"pads": "ISO"}, within=2), LEDS({"differs_from": "tracks", "within": 3, "snap": "iso"}),
    PAD(20, "down", note="ISO: a pad plays"), VOX({"min_in_use": 1}, soft=True), PAD(20, "up"),
    shift_pick("f1"), ST({"pads": "ISO"}, within=2, note="QA-PAT-313 (OS-PADOCT): Shift + F1 is the octave, not a mode: the pads stay ISO (the STEP mode has no key, QA-PAT-250)"),
    shift_pick("f6"), ST({"pads": "PATS"}, within=2), LEDS({"differs_from": "iso", "within": 3, "snap": "pats"}),
    PAD(2, note="PATS, stopped: a pad selects pattern n+1 for editing"), ST({"pattern": "3"}, within=2),
    PAD(0), ST({"pattern": "1"}, within=2),
    RUN("_play"), W(2.0),
    shift_pick("f7"), ST({"pads": "VU"}, within=2),
    LEDS({"min_lit": 1, "within": 4, "snap": "vu"}, note="VU lights while the pattern plays"),
    MIX({"channels": {"MST L": {"min_rms_db": -60}}, "within": 4, "snap": "base"}),
    shift_pick("f7"), ST({"pads": "FADERS"}, within=2),
    LEDS({"differs_from": "vu", "within": 3}, soft=True, note="(faders show the volumes, VU the meters: soft because both may coincide for an instant)"),
]
for t in range(12):
    pm.append(PAD(36 + t, note="FADERS: row 4 of track %d sets its volume to 25 %%" % (t + 1) if t == 0 else None))
pm += [
    MIX({"drop": {"channel": "MST L", "field": "hold", "from": "base", "min_db": 3}, "within": 8, "snap": "faded"}, soft=True, note="(soft: needs every track above 25 % at load)"),
    undo_many(12, gap=0.25),
    MIX({"rise": {"channel": "MST L", "field": "hold", "from": "faded", "min_db": 3}, "within": 8}, soft=True, note="the volumes are back (12 undo units)"),
    RUN("_stop"),
    SAY("a lone Shift hold is TRACKS on the pads for as long as it is held"),
    pad_mode_notes(), ST({"pads": "NOTES"}, within=2),
    LEDS({"within": 3, "snap": "n1"}),
    K("+shift", gap=0.3), ST({"shift": "1", "pads": "NOTES"}, within=2, note="the fixed mode is still NOTES: TRACKS is momentary"),
    LEDS({"differs_from": "n1", "within": 3, "snap": "momentary"}),
    K("-shift", gap=0.3), ST({"shift": "0"}, within=2),
    LEDS({"differs_from": "momentary", "within": 3}, note="released: the pads are NOTES again"),
]
add("pad_modes", "the pad modes NOTES PIANO TRACKS ISO PATS VU FADERS by Shift + F2..F7 (Shift + F1 is the octave: the pads stay as they are, QA-PAT-313) and the Shift hold: state pads, the LEDs differ per mode, VU lights while playing, a FADERS tap lowers the master and undo restores", pm,
    finally_=[K("-shift", gap=0.2), PAD(12, "up"), PAD(20, "up")] + clear_overlays() + [K("pattern", gap=0.4), undo_many(13, gap=0.2), shift_pick("f6"), PAD(0), pad_mode_notes(), RUN("_stop")])

# ======================================================================================================================================
# 4. mute_solo
# ======================================================================================================================================
SOUNDING = list(range(12))  # T1..T12: _load_tv16 unmutes the saved-muted 1, 6, 7, 8, and _play_song plays the whole song


def layer_taps(pads, note=None):
    out = [K("+shift", gap=0.3)]
    for i, p in enumerate(pads):
        out.append(PAD(p, note=note if i == 0 else None))
    out.append(K("-shift", gap=0.3))
    return out


ms = start_on_pattern() + [
    RUN("_play_song"), W(3.0),
    MIX({"min_tracks": 4, "channels": {"MST L": {"min_rms_db": -90}}, "within": 6, "snap": "base"}, note="the pattern is sounding (tracks 1, 6, 7, 8 were saved muted)"),
    CMD("mix", expect=[r"(?m)^\s*T02\s", r"(?m)^\s*T03\s"], within=6, poll=0.5, soft=True, note="(soft: T02 and T03 have notes in the loop of t-v16)"),
    SAY("MUTE one track: TRACKS layer (Shift held), row 2 of the pads = mute, pad 12 + track"),
] + layer_taps([13], "mute T02") + [
    CMD("mix", expect_not=r"(?m)^\s*T02\s", within=8, poll=0.5, soft=True, note="T02's meter is gone from the list once its 1 s peak hold is empty"),
    CMD("mix", expect=r"(?m)^\s*T03\s", soft=True, note="the others sound on"),
    SAY("MUTE every sounding track: the master falls"),
] + layer_taps([p + 12 for p in SOUNDING if p != 1], "mute the rest") + [
    MIX({"drop": {"channel": "MST L", "field": "hold", "from": "base", "min_db": 12}, "within": 8, "snap": "silent"}),
    CMD("mix", expect_not=r"(?m)^\s*T(0[1-9]|1[0-2])\s", within=6, poll=0.5, soft=True, note="no sounding track is metering"),
    SAY("UNMUTE all: tap the same pads again"),
] + layer_taps([p + 12 for p in SOUNDING], "unmute") + [
    MIX({"rise": {"channel": "MST L", "field": "hold", "from": "silent", "min_db": 12}, "within": 8}, note="the master is back"),
    CMD("mix", expect=[r"(?m)^\s*T02\s", r"(?m)^\s*T03\s"], within=6, poll=0.5, soft=True),
    SAY("SOLO T02: row 4 of the pads (36 + track); only it sounds"),
] + layer_taps([37], "solo T02") + [
    CMD("mix", expect_not=r"(?m)^\s*T(0[13-9]|1[0-2])\s", within=8, poll=0.5, soft=True, note="the other tracks are silent"),
    CMD("mix", expect=r"(?m)^\s*T02\s", soft=True, note="T02 still sounds"),
    MIX({"drop": {"channel": "MST L", "field": "hold", "from": "base", "min_db": 3}, "within": 4}, soft=True, note="(soft: with one track the master is lower than with all)"),
    SAY("UNSOLO: tap again"),
] + layer_taps([37], "unsolo") + [
    CMD("mix", expect=[r"(?m)^\s*T03\s", r"(?m)^\s*T04\s"], within=8, poll=0.5, soft=True, note="the other tracks come back"),
    RUN("_stop"),
]
add("mute_solo", "TRACKS layer (Shift held): mute and unmute sounding tracks, solo isolates one: the track meters and the master follow; every change is undone at the end", ms,
    finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), undo_many(14, gap=0.2), RUN("_stop")])

# ======================================================================================================================================
# 4b. cpu_mute_ab (OS-ODD11): the cost of a muted track. One pass = the four arms of docs/specs/OS-ODD11.md section 5 once (U nothing muted, M every track muted, H half, S one soloed),
# each a 60 s window after 5 s of settling; the tester runs it at least four times per image and alternates the images (A B A B), `cpu perf off` so the governor's counter reads do not colour the figures.
# The scene is controlled (the tester's CPULAYOUT scene): t-v16 with FILTER On on its 16 instruments (the project saves it Off: with no filter there is almost nothing for the gate to skip) and
# pattern 9 looping, and every arm starts from a stopped transport, so U, M, H and S run the same material from the same position (consecutive windows of one song would not). The mute and solo state is set
# explicitly: read back from the pad LEDs (the MUTE row 12..23 and the SOLO row 36..47 of the TRACKS layer, Shift held, on the Pattern screen only; `state` does not carry it), toggled only where it is wrong, asserted.
# ======================================================================================================================================
TRACKS12 = list(range(12))  # the 12 columns of the TRACKS layer
MUTE_PAD, SOLO_PAD = 12, 36


def set_tracks(muted, solo=None, name=""):
    """Sets the mutes of tracks `muted` (0-based) and the solo of track `solo` (None: none) from whatever state they are in, and asserts it. Stopped, Pattern screen, Shift held for the layer."""
    unsolo = [SOLO_PAD + t for t in TRACKS12]
    steps = [ST({"screen": "PATTERN"}, within=3, note="arm %s: the TRACKS layer works on the Pattern screen only" % name), K("+shift", gap=0.4)]
    steps += [PAD(SOLO_PAD + t, if_leds={"lit": [SOLO_PAD + t]}, note="a solo left on is tapped off (only a lit pad is tapped)" if t == 0 else None) for t in TRACKS12]
    steps += [LEDS({"unlit": unsolo, "within": 3}, note="no track is soloed")]
    steps += [PAD(MUTE_PAD + t, if_leds={"unlit" if t in muted else "lit": [MUTE_PAD + t]}, note="a mute is tapped only where it is wrong (MUTE row: lit = muted)" if t == 0 else None) for t in TRACKS12]
    steps += [LEDS({"lit": [MUTE_PAD + t for t in muted], "unlit": [MUTE_PAD + t for t in TRACKS12 if t not in muted], "within": 3},
                   note="the mute state is as wanted (%s)" % ("none" if not muted else "T%02d..T%02d" % (muted[0] + 1, muted[-1] + 1)))]
    if solo is not None:
        steps += [PAD(SOLO_PAD + solo, note="solo T%02d" % (solo + 1)), LEDS({"lit": [SOLO_PAD + solo], "unlit": [p for p in unsolo if p != SOLO_PAD + solo], "within": 3}, note="only that track is soloed")]
    return steps + [K("-shift", gap=0.4)]


def cpu_window(arm):
    """A 60 s window after 5 s of settling: `cpu reset` (then `cpu perf off`, which a reset undoes), 12 reads of `cpu` 5 s apart, and their mean (the console prints last and max only)."""
    reads = []
    for i in range(12):
        last = i == 11
        reads += [W(5.0), CMD("cpu", expect=["underrun 0", "shed 0 blocks"] if last else "underrun 0", collect={"as": arm, "re": r"render last (\d+)"}, show=True if last else None,
                              note="arm %s: render last / max, underruns, over-budget (the last read of the 12 is kept in the report)" % arm if last else None)]
    return [W(5.0, note="settle"), CMD("voices", note="arm " + arm + ": voices at the start of the window (OS-ODD11 slice 2, section 13: `advanced` is read here and at the end; it must rise in M and H and stay equal in U)"),
            CMD("cpu reset", note="arm " + arm + ": the 60 s window starts"), CMD("cpu perf off")] + reads + [
        STAT(arm, 12, show=True, note="arm " + arm + ": the mean of the 12 reads of `render last` (cycles)"),
        CMD("voices", note="arm " + arm + ": in use / peak / steals, and `advanced` at the end of the window (compare with the start)"), CMD("mod", soft=True, note="arm " + arm + ": module instances and cycles per voice (soft: no module in t-v16)")]


def filter_on_16():
    steps = [pad_mode_notes(), ST({"pads": "NOTES"}, within=2), K("params", gap=0.4), ST({"screen": "INSTRUMENT"}, within=3),
             SAY("FILTER On on the 16 instruments of t-v16 (saved Off): Shift + pad selects the instrument, F2 then F3 puts the cursor on the FILTER row, one detent turns it On (a second one changes nothing)")]
    for n in range(1, 17):
        steps += [K("+shift", gap=0.3), PAD(n - 1), K("-shift", gap=0.3), ST({"screen": "INSTRUMENT", "instrument": str(n)}, within=2),
                  K("f2", gap=0.3), K("f3", gap=0.3), ST({"page": "~^[12]$"}, within=2, note="(FILTER page, cursor on its first row: FILTER; page 1 on a BEAT SLICE instrument, page 2 on a Source-first one)" if n == 1 else None), ENC(1, gap=0.3)]
    return steps + [SHOT("filter_on_16", note="(a picture for the report: instrument 16, FILTER On)"), K("pattern", gap=0.4), ST({"screen": "PATTERN"}, within=3)]


def loop_pattern_9():
    return [shift_pick("f6"), ST({"pads": "PATS"}, within=2), PAD(8, note="PATS: pad n selects pattern n+1, 9 is the dense one"), ST({"pattern": "9"}, within=2),
            pad_mode_notes(), ST({"pads": "NOTES"}, within=2)]


def mute_arm(arm, say, muted, solo, checks):
    return [SAY(say), RUN("_stop")] + set_tracks(muted, solo, arm) + [RUN("_play"), ST({"play_pattern": "9"}, within=3, soft=True, note="(the pattern loop: every arm starts it from the stopped transport)"),
                                                                       W(5.0, note="settle for the mix check")] + checks + cpu_window(arm) + [RUN("_stop")]


def mix_sounding(arm):
    return [MIX({"channels": {"MST L": {"min_rms_db": -70}}, "within": 6}, note="arm %s: the pattern sounds" % arm),
            MIX({"min_tracks": 4}, soft=True, note="(soft: pattern 9 is expected to play at least four tracks)")]


MIX_SILENT = [MIX({"channels": {"MST L": {"max_peak_db": -80}}, "within": 8}, note="arm M is silent: the master has no signal (a mute that is not on would make the figures below meaningless)"),
              CMD("mix", expect_not=r"(?m)^\s*T(0[1-9]|1[0-2])\s", within=8, poll=0.5, note="and no track is metering")]

cm = start_on_pattern() + filter_on_16() + loop_pattern_9() + [CMD("cpu perf off")]
cm += mute_arm("U", "arm U: nothing muted", [], None, mix_sounding("U"))
cm += mute_arm("M", "arm M: every sounding track muted (T01..T12)", TRACKS12, None, MIX_SILENT)
cm += mute_arm("H", "arm H: half the tracks muted (T01..T06)", list(range(6)), None, mix_sounding("H"))
cm += mute_arm("S", "arm S: T02 soloed, none muted", [], 1, mix_sounding("S"))
add("cpu_mute_ab", "OS-ODD11 A/B: the CPU of t-v16 (FILTER On x16, pattern 9 looping, each arm from a stopped start) with nothing muted (U), every track muted (M, checked silent), half muted (H), one soloed (S): four 60 s windows, `cpu` mean of 12 reads, `voices`, `mod` each; run it four times per image, alternating the images", cm,
    finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_stop"), undo_until_empty(), pad_mode_notes()])

# ======================================================================================================================================
# 4c. qa_cpu_heavy (OS-FINALQA FQA-7, QA-GLB-156) and qa_cpu_dense (OS-DENSE): the generated songs of docs/architecture/cpu-budget.md section 4.
# The songs are GENERATED (`o3pj_tool gen heavy|dense10|dense8`, tests/tools/o3pj_tool.cpp): looped sampler voices with filter, cutoff source, both sends and drive on some, a TB-303 line, an FM voice, the
# Master FX page set and the limiter on. Heavy12 (6 Modern + 4 Amiga + 303 + FM, 12 tracks) is the STRESS test since 2026-10-23 (maintainer): it runs over the budget on the unit, so the scenario asserts what the
# main-loop guard keeps (the console answers within 1 s at every read, Stop works, the UI responds) and only REPORTS underruns, shed blocks and the mean. Dense10 (6 + 2 + 303 + FM) and Dense8 (4 + 2 + 303 + FM)
# are the candidates of the 1.0 CPU reference: qa_cpu_dense asserts zero underruns and zero shed blocks, and reports the mean against the 85 % target. They are NOT on the card: the tester puts the
# folder (project.o3pj and samples/) into Projects/ first (a card reader, or the unit's USB Disk mode; o3console --put is for firmware only).
# The console `load` reads OG folders only, so the project is opened from the File screen: RESCAN, the cursor to the first entry, Down until the strip's `sel` is the folder, F1.
# ======================================================================================================================================
CPU_READ_RE = r"render last (\d+)"


def open_song(sel, load, opened, before, pcm=True):
    """Opens a generated project from the File screen and starts it: `sel` is the regex of the strip's `sel`, `load` that of the `sd` line, `opened` that of the strip message, `before` the SAY; `pcm` False for a song without samples (it prints no pcm line)."""
    return [SAY(before),
            K("-shift", gap=0.2, note="(Shift released, in case a former run left it down)")] + clear_overlays() + [
        K("file", gap=0.5), ST({"screen": "FILE"}, within=3),
        K("f2", gap=0.5, note="RESCAN"), W(2.0),
        K("+shift", "f1", "-shift", gap=0.4, note="the cursor to the first entry"),
        K("down", gap=0.3, until={"sel": sel}, max=150, note="Down until the cursor is on the project (150 rounds at most: more projects than that on the card and it stops here, nothing opens)"),
        ST({"sel": sel}, within=2),
        K("f1", gap=0.5), K("f1", gap=0.5, if_state={"msg": "~^UNSAVED_CHANGES"}, note="(only when the strip asks)"),
        ST({"msg": opened}, within=4, note="the song is opening (the strip may already read PROJECT_<name>_OPENED on the unit)"),
        CMD("sd", expect=load, within=180, poll=2.0, fail_on="FAILED"),
        ] + ([CMD("sd", expect="pcm:\\s+.*loaded", within=120, poll=2.0, fail_on="FAILED")] if pcm else []) + [
        CMD("sd", expect_not="[1-9]\\d* values reset", soft=True, note="(the load line says `N values reset` when a chunk had values out of range)"),
        W(3.0), K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=5), RUN("_stop"),
        CMD("mod", expect="tb303", soft=True, note="(soft: the TB-303 and the FM instance are bound; a muted module says so here)")]


def cpu_reads(n, every, arm, **kw):
    """`n` reads of `cpu`, `every` seconds apart, each kept for the `stat` of the arm; the last one is shown. `kw` goes to every CMD (expect, timeout)."""
    reads = []
    for i in range(n):
        last = i == n - 1
        reads += [W(every), CMD("cpu", collect={"as": arm, "re": CPU_READ_RE}, show=True if last else None,
                                note="arm %s: render last / max, underruns, the guard's shed blocks (the last read is kept in the report)" % arm if last else None, **kw)]
    return reads


STRESS_WINDOW = 24  # reads of 2.5 s: the 60 s window of the stress and the dense scenarios

ch = open_song("~^Heavy$", "load:\\s+Projects/Heavy", "~(?i)^(OPENING_HEAVY|PROJECT_HEAVY_OPENED)",
               "OS-DENSE: Heavy12 is the STRESS test (cpu-budget.md section 4): 12 tracks (6 Modern + 4 Amiga sampler voices, TB-303, FM). It is over the CPU budget on the unit (mean about 113 %, worst about 147 % on v1.0.0-fqa.8, 477d5d9, unit run of 2026-10-23), "
               "so this scenario asserts that the unit stays controllable (the main-loop guard of OS-MAINGUARD) and REPORTS underruns, shed blocks and the mean. BEFORE the run: build the song (`o3pj_tool gen heavy "
               "<dir>/Heavy/project.o3pj`, with the folder <dir>/Heavy/samples made first) and copy the folder Heavy into Projects/ on the card (card reader or the unit's USB Disk mode). Use the release image, "
               "then the bench image (-DO3OS_BENCH=ON) of the same build id: cpu-budget.md section 5.") + [
    RUN("_play"), W(5.0, note="settle"),
    VOX({"min_in_use": 10}, soft=True, note="(soft: the sampler voices sound)"),
    MIX({"channels": {"MST L": {"min_rms_db": -60}}, "within": 6}, soft=True, note="(soft: the song sounds; under the guard a shed block is silent)"),
    CMD("voices", timeout=1.0, note="stress: voices at the start of the window; the console answers within 1 s"),
    CMD("cpu reset", timeout=1.0, note="QA-GLB-156: the 60 s window starts"), CMD("cpu perf off", timeout=1.0)] + cpu_reads(
        STRESS_WINDOW, 2.5, "S", timeout=1.0, expect=[r"render last \d+", r"guard\s+shed \d+ blocks"]) + [
    STAT("S", min_n=STRESS_WINDOW, show=True, note="stress: the mean of the %d reads of `render last` (cycles; the block is 261,120 cycles, so 100 %% = 261,120). REPORTED, not asserted" % STRESS_WINDOW),
    CMD("voices", timeout=1.0, note="stress: in use / peak / steals, and `advanced` at the end of the window"),
    CMD("mod", soft=True, note="stress: module instances and cycles per voice"),
    CMD("bench worst", soft=True, show=True, note="QA-GLB-156: the stage split of the worst block since `cpu reset` (soft: only a bench image has `bench`; a release image answers `not in this image`)"),
    ST({"screen": "PATTERN"}, within=2, note="stress: the UI responds (the state answers and the screen is still Pattern)"),
    K("play", gap=0.2, note="stress: Stop works, the Play key stops the transport under load"),
    ST({"playing": "0"}, within=4, poll=0.3, note="QA-GLB-156: stress: playing 0 after the Play key"),
    RUN("_stop")]
add("qa_cpu_heavy", "OS-FINALQA FQA-7, QA-GLB-156, OS-DENSE: the STRESS test, Heavy12 (6 Modern + 4 Amiga sampler voices, TB-303, FM, delay, plate HALF, limiter) opened from File and played for a 60 s window "
    "(24 reads of `cpu`, 2.5 s apart) after 5 s of settling. ASSERTS the controllability the main-loop guard keeps: every read answers within 1 s, the guard line is read, the UI responds, Stop works. "
    "Underruns, shed blocks and the mean are REPORTED, not asserted. The project must be on the card first (`o3pj_tool gen heavy`)",
    ch, finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_stop")])

# qa_cpu_dense (OS-DENSE, maintainer 2026-10-23): the 1.0 CPU target on the dense but realistic reference project. The scenario opens Dense10 or Dense8 (whichever folder is on the card; put one, then the other,
# for the two runs): the one measured on the unit becomes the reference. Zero underruns and zero shed blocks are asserted at every read; the mean of the 60 s window is a reported figure against 85 % of the block
# (221,952 of 261,120 cycles).
cd = open_song("~^Dense(10|8)$", "load:\\s+Projects/Dense(10|8)", "~(?i)^(OPENING_DENSE(10|8)|PROJECT_DENSE(10|8)_OPENED)",
               "OS-DENSE: the 1.0 CPU reference candidates of cpu-budget.md section 4: Dense10 (6 Modern + 2 Amiga sampler voices, TB-303, FM = 10 tracks) and Dense8 (4 Modern + 2 Amiga, TB-303, FM = 8 tracks), "
               "both with the delay and reverb sends on and the typical per-voice FX. BEFORE the run: build the song (`o3pj_tool gen dense10 <dir>/Dense10/project.o3pj` or `gen dense8 <dir>/Dense8/project.o3pj`, "
               "with the folder <dir>/<name>/samples made first) and copy that folder into Projects/ on the card (only one of the two on the card per run). Use the release image.") + [
    RUN("_play"), W(5.0, note="settle"),
    VOX({"min_in_use": 4}, soft=True, note="(soft: the Modern sampler voices sound; `voices` counts only those: Dense10 6 in use, Dense8 4)"),
    MIX({"channels": {"MST L": {"min_rms_db": -60}}, "within": 6}, note="the song sounds"),
    CMD("voices", note="dense: voices at the start of the window"),
    CMD("cpu reset", note="QA-GLB-158: the 60 s window starts"), CMD("cpu perf off")] + cpu_reads(
        STRESS_WINDOW, 2.5, "D", expect=[r"underrun 0 \(", r"guard\s+shed 0 blocks"]) + [
    STAT("D", min_n=STRESS_WINDOW, show=True, note="QA-GLB-158: the mean of the %d reads of `render last` (cycles). REPORTED against the 1.0 target: 85 %% of the block is 221,952 of 261,120 cycles" % STRESS_WINDOW),
    CMD("voices", note="dense: in use / peak / steals, and `advanced` at the end of the window"),
    CMD("mod", soft=True, note="dense: module instances and cycles per voice"),
    CMD("bench worst", soft=True, show=True, note="the stage split of the worst block since `cpu reset` (soft: only a bench image has `bench`)"),
    RUN("_stop")]
add("qa_cpu_dense", "OS-DENSE: the 1.0 CPU target on the dense reference project, Dense10 (6 Modern + 2 Amiga sampler voices, TB-303, FM) or Dense8 (4 Modern + 2 Amiga, TB-303, FM), delay and reverb sends on: "
    "opened from File, `cpu reset`, Play, 24 reads of `cpu` 2.5 s apart over 60 s. ASSERTS underrun 0 and guard shed 0 at every read; the mean (target 85 % of the block, 221,952 cycles) is reported, not asserted. "
    "The project must be on the card first (`o3pj_tool gen dense10` or `dense8`)",
    cd, finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_stop")])

# qa_cpu_moddrive (OS-BUG-MODDRIVE, Level C, unit-gated): the cost of Overdrive and Bit depth (step FX 38 / 39) on module instruments. One run per project: ModDrive1, ModDrive4, ModDrive8 (1, 4, 8 TB-303 tracks
# with the FX on step 1) and the same three with `off` in the folder name (no FX: the control). Compare the mean `render last` of each pair; underrun 0 and guard shed 0 are asserted.
md = open_song("~^${PROJ}$", "load:\\s+Projects/${PROJ}", "~(?i)^(OPENING_|PROJECT_)${PROJ}",
               "QA-MOD-209 (OS-BUG-MODDRIVE): the CPU of FX 38 / 39 on module tracks. BEFORE the run: build the songs (`o3pj_tool gen moddrive4 <dir>/ModDrive4/project.o3pj`, `gen moddrive4off <dir>/ModDrive4off/project.o3pj`, "
               "and 1 and 8 the same; no samples) and copy ONE folder into Projects/ per run, 1, 4 and 8 with the FX, then each control. Run with `--var PROJ=<folder name>`. Use the release image.", pcm=False) + [
    RUN("_play"), W(5.0, note="settle"),
    MIX({"channels": {"MST L": {"min_rms_db": -60}}, "within": 6}, note="the song sounds"),
    CMD("cpu reset", note="the window starts"), CMD("cpu perf off")] + cpu_reads(
        8, 2.5, "M", expect=[r"underrun 0 \(", r"guard\s+shed 0 blocks"]) + [
    STAT("M", min_n=8, show=True, note="the mean of the 8 reads of `render last` (cycles): REPORTED, compare the FX project with its `off` control"),
    CMD("mod", soft=True, show=True, note="module instances and cycles per voice"),
    CMD("mod gov", soft=True, show=True, note="the governor: how many instances it admits (the shaping lands in its base)"),
    RUN("_stop")]
add("qa_cpu_moddrive", "OS-BUG-MODDRIVE: the CPU of the step FX 38 / 39 on 1, 4 and 8 TB-303 tracks against the same song without them: opened from File, `cpu reset`, Play, 8 reads of `cpu` 2.5 s apart. "
    "ASSERTS underrun 0 and guard shed 0; the mean `render last` is reported. The project must be on the card first (`o3pj_tool gen moddrive1|4|8` and the `off` controls)",
    md, finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_stop")], vars_={"PROJ": "ModDrive1"})

# ======================================================================================================================================
# 5. slices_pads (OS-SLICEFIX)
# ======================================================================================================================================
sl = start_on_pattern() + [pad_mode_notes(), ST({"pads": "NOTES"}, within=2), RUN("_play"), W(1.0), RUN("_stop"),
                           SAY("every instrument 1..16 of t-v16: selected on PARAMS by Shift + its pad, three pads played on Pattern; `inst n` soft-reports which are beat slices")]
for n in range(1, 17):
    sl += [
        K("params", gap=0.4), K("+shift", gap=0.3), PAD(n - 1, note="instrument %d (Shift + pad selects it: a bare pad on Params plays the instrument in view, OS-INSTUI S2)" % n), K("-shift", gap=0.3),
        ST({"screen": "INSTRUMENT", "instrument": str(n)}, within=2),
        K("pattern", gap=0.4), ST({"screen": "PATTERN"}, within=2),
        CMD("inst %d" % n, expect=r"inst %02d: mode \d+" % n, soft=True, note="(soft: a sample instrument is bound)"),
        CMD("inst %d" % n, expect=r"beat slices \d+ first-note 24", soft=True, note="(soft by design: only the beat slice instruments match)"),
        PAD(0, "down"), PAD(5, "down"), PAD(11, "down"),
        VOX({"min_in_use": 1}, soft=True),
        MIX({"channels": {"MST L": {"min_rms_db": -70}}, "within": 3}, soft=True),
        PAD(0, "up"), PAD(5, "up"), PAD(11, "up"),
        W(0.6),
    ]
sl += [
    CMD("voices", expect=r"in use \d+/12\s+peak \d+\s+steals \d+", note="the pool never exceeds 12"),
    VOX({"max_steals": 0}, soft=True, note="(soft: the counter is since boot; three pads of one instrument must not steal)"),
    CMD("cpu", expect=r"underrun 0\b"),
]
add("slices_pads", "OS-SLICEFIX: for each of the 16 instruments of t-v16 select it, play three NOTES pads: voices rise, `inst` tells the beat slices, nothing steals, no underrun", sl,
    finally_=[K("-shift", gap=0.2), PAD(0, "up"), PAD(5, "up"), PAD(11, "up")] + clear_overlays() + [K("pattern", gap=0.4), RUN("_stop")])

# ======================================================================================================================================
# 6. song_edit
# ======================================================================================================================================
song = start_on_pattern() + [
    K("song", gap=0.5), ST({"screen": "SONG"}, within=3),
    K("song", gap=0.4, if_state={"page": "1"}), ST({"page": "0"}, within=2),
    K("+shift", "f1", "-shift", gap=0.3, note="Shift + F1: the cursor to the first slot"),
    K("+shift", "f8", "-shift", gap=0.3, note="(redo on an empty journal: the last message becomes NOTHING TO REDO)"),
    ST({"msg": NOTHING_REDO}, within=2, soft=True),
    SHOT("song_before"),
    SAY("PLAY PAT (F1) plays the pattern: no song position"),
    K("f1", gap=0.3), ST({"playing": "1", "seq": "1", "order": "-"}, within=6, soft=True), RUN("_stop"),
    SAY("ADD SLOT (F3) twice: the cursor goes to the new slot, Play starts the song there"),
    K("f3", gap=0.5),
    K("play", gap=0.3), ST({"playing": "1", "seq": "1"}, ne={"order": "-"}, within=6), ST({"order": "1"}, within=2, soft=True, note="(soft: the order position is the cursor's slot)"), RUN("_stop"),
    K("f3", gap=0.5),
    K("play", gap=0.3), ST({"playing": "1", "seq": "1"}, ne={"order": "-"}, within=6), ST({"order": "2"}, within=2, soft=True), RUN("_stop"),
    SAY("PAT + (the encoder, one click; F6 is TOOLS since OS-SONGMAP) on the new slot, then DEL SLOT (F4): the cursor falls back one slot"),
    ENC(1),
    K("f4", gap=0.5),
    K("play", gap=0.3), ST({"playing": "1", "seq": "1"}, ne={"order": "-"}, within=6), ST({"order": "1"}, within=2, soft=True), RUN("_stop"),
    SHOT("song_edited"),
    SAY("UNDO to the empty journal (8 units: 4 unmutes from the loader + add, add, pattern, delete), REDO to the empty stack, UNDO again"),
    K("f8", gap=0.4), ST({}, ne={"msg": NOTHING_UNDO}, within=2),
    undo_many(12), ST({"msg": NOTHING_UNDO}, within=3),
    K("+shift", "f8", "-shift", gap=0.4), ST({"msg": NOTHING_UNDO}, within=2),
    redo_many(12), ST({"msg": NOTHING_REDO}, within=3),
    undo_many(12), ST({"msg": NOTHING_UNDO}, within=3, note="the song is as loaded; nothing was saved"),
    SAY("Play from the first slot of the restored song"),
    K("+shift", "f1", "-shift", gap=0.3),
    K("play", gap=0.3), ST({"playing": "1", "seq": "1"}, ne={"order": "-"}, within=6), ST({"order": "0"}, within=2, soft=True), RUN("_stop"),
]
add("song_edit", "Song screen: ADD SLOT twice, the pattern + (encoder), DEL SLOT, play the song from the cursor's slot (state order), undo to the empty journal, redo, undo again; nothing saved", song,
    finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("song", gap=0.4, if_state={"screen": "~^(?!SONG$)"}), undo_many(12, gap=0.2), K("pattern", gap=0.4), RUN("_stop")])

# ======================================================================================================================================
# 7. perform_full
# ======================================================================================================================================
TEMPO_SET = [
    K("song", gap=0.5), ST({"screen": "SONG"}, within=3),
    K("f7", gap=0.4, note="F7 TEMPO: the encoder is on the tempo field now (and the press is a tap-tempo tap)"),
    ENC(-64), ENC(-64), ENC(-64), ENC(-64, note="40 BPM, the floor"),
    ENC(64), ENC(16, note="+80 detents of 1 BPM: 120.00 BPM"),
]
pf = start_on_pattern() + TEMPO_SET + [
    K("performance", gap=0.5), ST({"screen": "PERFORM"}, within=3),
    SHOT("perform_start"),
    SAY("NEXT PAT while stopped says so"),
    K("f7", gap=0.4), ST({"msg": "~^NOT_PLAYING$"}, within=2),
    SAY("SCENES: a scene key tapped on an empty scene says so; held 1 s it stores"),
    K("f8", gap=0.4, note="RESET: every FX off"),
    LEDS({"within": 3, "snap": "clean"}),
    K("f2", gap=0.4), ST({"msg": "~^SCENE_IS_EMPTY"}, within=2, soft=True, note="(soft: scenes are runtime state; a load empties them)"),
    PAD(12, note="slot 1, row 1: the FX of the slot on at its first value"),
    LEDS({"differs_from": "clean", "within": 3, "snap": "fx_on"}, soft=True, note="(soft: whether a tap latches depends on the latch rule of the bank)"),
    K("f1:1300", gap=0.2, note="F1 held 1.3 s stores scene 1"),
    ST({"msg": "~^SCENE_1_STORED$"}, within=3),
    K("f8", gap=0.4),
    LEDS({"differs_from": "fx_on", "within": 3, "snap": "reset"}, soft=True),
    K("f1", gap=0.4, note="F1 tapped recalls scene 1"),
    ST({}, ne={"msg": "~^SCENE_IS_EMPTY"}, within=2),
    LEDS({"differs_from": "reset", "within": 3}, soft=True, note="the slot's FX is back"),
    K("f8", gap=0.4),
    SAY("LATCH (F6) toggles; twice is the state it was"),
    K("f6", gap=0.4), SHOT("perform_latch", soft=True), K("f6", gap=0.4),
    SAY("HALF (F4) and DOUBLE (F5) are rates while held: the tempo of `state` follows"),
    RUN("_play"), W(1.0),
    ST({"tempo": "12000"}, within=4, soft=True, note="120 BPM (soft: a Tempo FX in the pattern would move it)"),
    K("+f4", gap=0.2), ST({"tempo": "6000"}, within=4, soft=True, note="half time while F4 is down"),
    ST({}, ne={"tempo": "12000"}, within=1, soft=True),
    K("-f4", gap=0.2), ST({"tempo": "12000"}, within=4, soft=True, note="released: the song's tempo"),
    K("+f5", gap=0.2), ST({"tempo": "24000"}, within=4, soft=True, note="double time while F5 is down"),
    K("-f5", gap=0.2), ST({"tempo": "12000"}, within=4, soft=True),
    K("+f4", "+f5", gap=0.2), ST({"tempo": "12000"}, within=4, soft=True, note="both: the song's own rate"),
    K("-f5", "-f4", gap=0.2),
    SAY("NEXT PAT (F7) while playing queues the next pattern of the song; again cancels"),
    K("f7", gap=0.4), ST({"msg": "~^(NEXT_PATTERN_QUEUED|PATTERN_IS_NOT_IN_THE_SONG|SONG_IS_EMPTY)$"}, within=2),
    K("f7", gap=0.4), ST({"msg": "~^(NEXT_PATTERN_CANCELLED|PATTERN_IS_NOT_IN_THE_SONG|SONG_IS_EMPTY)$"}, within=2, soft=True),
    MIX({"channels": {"MST L": {"min_rms_db": -70, "max_clip": 0}}, "within": 4}, note="the audio is intact through all of it"),
    RUN("_stop"),
    CMD("cpu", expect=r"underrun 0\b"),
    K("pattern", gap=0.4), ST({"screen": "PATTERN"}, within=3),
]
add("perform_full", "Perform: scene store and recall, RESET, LATCH, HALF / DOUBLE time (tempo in `state` while the key is held), NEXT PAT; the project tempo is set to 120 BPM on Song first and undone at the end", pf,
    finally_=[K("-f4", "-f5", gap=0.1), PAD(12, "up")] + clear_overlays() +
             [K("f8", gap=0.3, if_state={"screen": "PERFORM"}), K("song", gap=0.4), undo_many(10, gap=0.2), K("pattern", gap=0.4), RUN("_stop")])

# ======================================================================================================================================
# 8. mixer_sends (OS-SENDFX)
# ======================================================================================================================================
def params_send_rows(value_steps):
    """On PARAMS page SENDS (F5): row 0 DELAY SEND, row 1 REVERB SEND; each one set by `value_steps` (encoder counts, or 'default')."""
    out = [K("params", gap=0.5), ST({"screen": "INSTRUMENT"}, within=3), K("f5", gap=0.4), ST({"page": "3"}, within=2),
           K("up", repeat=6, gap=0.12)]
    for row in range(2):
        if row:
            out.append(K("down", gap=0.3))
        if value_steps == "default":
            out.append(K("delete", gap=0.3))
        else:
            out += [ENC(value_steps), ENC(value_steps)]
    return out


def mixer_returns(value_steps):
    """On MIXER page 0: the SND A column (the master DELAY return, 13th) and the SND B column (REVERB, 14th)."""
    out = [K("master", gap=0.5), ST({"screen": "MIXER"}, within=3), K("f1", gap=0.4), ST({"page": "0"}, within=2), K("up", gap=0.2),
           K("left", repeat=20, gap=0.1), K("right", repeat=13, gap=0.12)]
    for col in range(2):
        if col:
            out.append(K("right", gap=0.3))
        if value_steps == "default":
            out.append(K("delete", gap=0.3))
        else:
            out += [ENC(value_steps), ENC(value_steps)]
    return out


def note_tail(snap, drop_from=None, rise_from=None, soft=True):
    out = [K("pattern", gap=0.4), PAD(12, "down"), W(0.8), PAD(12, "up"), W(2.5)]
    if drop_from:
        out.append(MIX({"drop": {"channel": "MST L", "field": "hold", "from": drop_from, "min_db": 6}, "snap": snap}, soft=soft))
    elif rise_from:
        out.append(MIX({"rise": {"channel": "MST L", "field": "hold", "from": rise_from, "min_db": 6}, "snap": snap}, soft=soft))
    else:
        out.append(MIX({"channels": {"MST L": {}}, "snap": snap}))
    return out


ms2 = start_on_pattern() + [
    RUN("_play"), W(1.0), RUN("_stop"), W(1.0, note="(the outputs are open now: the first Play opens them)"),
    K("params", gap=0.5), *SELECT_1, ST({"screen": "INSTRUMENT", "instrument": "1"}, within=2),
    CMD("inst 1", expect=r"inst 01: mode \d+", soft=True, note="(soft: instrument 1 is a sample; the whole scenario is only as good as that)"),
] + params_send_rows(64) + [
    SHOT("sends_up", soft=True),
    K("pattern", gap=0.4),
    PAD(12, "down"), W(0.8),
    MIX({"channels": {"SNDA": {}, "SNDB": {}}, "within": 3}, soft=True, note="the send buses carry the note once the sends are up (SNDA = delay, SNDB = reverb)"),
    PAD(12, "up"), W(2.5),
    MIX({"channels": {"MST L": {}}, "snap": "tail_on"}, note="(the master 2.5 s after the note: the send effects' tails)"),
    SAY("the master returns to 0: the tail goes"),
] + mixer_returns(-64) + note_tail("tail_returns_off", drop_from="tail_on") + [
    SAY("returns back to default, sends to 0: no tail either"),
] + mixer_returns("default") + params_send_rows("default") + note_tail("tail_sends_off", drop_from="tail_on") + [
    SAY("sends up again: the tail is back (a rise from the silent run)"),
] + params_send_rows(64) + note_tail("tail_again", rise_from="tail_sends_off") + params_send_rows("default")
add("mixer_sends", "OS-SENDFX: instrument 1's DELAY / REVERB sends up put the note on SNDA / SNDB and leave a tail on the master after the note; the master returns at 0, or the sends at 0, take the tail away; all restored", ms2,
    finally_=[PAD(12, "up")] + clear_overlays() + [K("pattern", gap=0.4), undo_many(40, gap=0.2), RUN("_stop")])

# ======================================================================================================================================
# 9. limiter_controls (OS-LIMIT)
# ======================================================================================================================================
def master_fx_limiter_row():
    return [K("master", gap=0.5), ST({"screen": "MIXER"}, within=3), K("f3", gap=0.4), ST({"page": "2"}, within=2),
            K("up", repeat=14, gap=0.1), K("down", repeat=7, gap=0.15, note="row 7 of MASTER FX: LIMITER")]


lim = start_on_pattern() + [
    RUN("_play"), W(8.0),
    CMD("mix", expect=[r"(?m)MST L\s+pk\s+\S+\s+hold\s+\S+\s+rms\s+\S+\s+clip 0\b", r"(?m)MST R\s+pk\s+\S+\s+hold\s+\S+\s+rms\s+\S+\s+clip 0\b"],
        note="the master (after the limiter) has never clipped since boot"),
    CMD("mix", expect=r"(?m)MAIN L\s+pk\s+\S+\s+hold\s+\S+\s+rms\s+\S+\s+clip [1-9]\d*", soft=True, note="MAIN is metered before the limiter: on t-v16 it clips (the overload the limiter caught); soft because it depends on the project"),
    MIX({"channels": {"MST L": {"max_peak_db": -0.2, "max_clip": 0}, "MST R": {"max_peak_db": -0.2, "max_clip": 0}}, "snap": "off"}, note="the base limiter's ceiling is -0.3 dBFS, switch or not"),
] + master_fx_limiter_row() + [
    SHOT("master_fx_limiter_off", soft=True),
    ENC(1, note="LIMITER On (the slow stage's threshold 92 %, attack 10 ms, release 100 ms come into force)"),
    W(3.0),
    MIX({"channels": {"MST L": {"max_peak_db": -0.2, "max_clip": 0}, "MST R": {"max_peak_db": -0.2, "max_clip": 0}}, "within": 4, "snap": "on"}),
    MIX({"drop": {"channel": "MST L", "field": "hold", "from": "off", "min_db": 0.3}, "within": 6}, soft=True, note="(soft: LIMITER On pulls the 1 s peak hold down by a few tenths of dB when the signal is over 92 %; content dependent)"),
    CMD("cpu", expect=[r"underrun 0\b", r"over-budget 0\b"], note="the limiter's cost shows in the master stage of cpu"),
    ENC(-1, note="LIMITER Off again"),
    W(3.0),
    MIX({"channels": {"MST L": {"max_peak_db": -0.2, "max_clip": 0}}, "within": 4}),
    SAY("a controller of the master volume: the limiter must hold the ceiling at the maximum master volume too"),
    K("f1", gap=0.4), K("left", repeat=20, gap=0.1), K("right", repeat=16, gap=0.12, note="the MASTER column of the MIXER page"),
    ENC(64), ENC(64),
    W(2.0),
    MIX({"channels": {"MST L": {"max_peak_db": -0.2, "max_clip": 0}, "MST R": {"max_peak_db": -0.2, "max_clip": 0}}, "within": 4}, note="master volume at the maximum: still not above the ceiling"),
    K("delete", gap=0.4, note="Delete: the default master volume"),
    RUN("_stop"),
]
add("limiter_controls", "OS-LIMIT: the master LIMITER switch (MASTER FX page) and the master volume at the top: the master meter never exceeds the -0.3 dBFS ceiling and never clips, the MAIN meter shows the overload the limiter caught (soft), cpu stays in budget", lim,
    finally_=clear_overlays() + [K("pattern", gap=0.4), undo_many(10, gap=0.2), RUN("_stop")])

# ======================================================================================================================================
# 10. modules
# ======================================================================================================================================
mods = [
    SAY("the outputs must be open for a module note: the first Play opens them"),
    K("-shift", gap=0.2),
] + clear_overlays() + [
    RUN("_play"), W(1.0), RUN("_stop"), W(1.0),
    CMD("mod", expect=r"mod: slab \d+/\d+ B, module memory", note="the module system's first line"),
    CMD("mod notice", expect=r"(?i)licen[cs]e|copyright|MIT|GPL|BSD", soft=True, note="(soft: the notices of the built-in modules)"),
]
# Free slots of t-v16 (instruments 33..35): a console bind on a slot that holds a project instrument (t-v16's 49..64 are MIDI) is refused (OS-BUGS4: the gate follows the project)
for slot, stem, note_no in ((33, "tb303", 36), (34, "chip", 48), (35, "fm", 48), (36, "sid", 48)):
    mods += [
        SAY("built-in module %s on instrument %d" % (stem, slot)),
        CMD("mod load %s" % stem, expect=r"mod: %s: (started|already loaded)" % stem),
        CMD("mod", expect=r"mod: %s (v\d+\.\d+\.\d+|unversioned) ABI" % stem, within=10, poll=1.0, note="it is in the listing once its (built-in) load is done"),
        CMD("mod bind %d %s" % (slot, stem), expect=r"mod: bind (ok|that instrument is bound already)", note="(a second run finds it bound already: fine)"),
        MIX({"channels": {"MST L": {}}, "snap": "before_" + stem}),
        CMD("mod note %d %d 110" % (slot, note_no), expect=r"mod: note queued"),
        CMD("mod", expect=r"(?i)mod: instrument %d: %s (sounding|held)" % (slot, stem), soft=True, within=3, poll=0.5),
        MIX({"channels": {"T16": {"min_rms_db": -75}, "MAIN L": {"min_rms_db": -75}}, "within": 4}, note="the audition plays on track 16: its meter and the main bus rise"),
        MIX({"rise": {"channel": "MST L", "field": "hold", "from": "before_" + stem, "min_db": 6}, "within": 3}, soft=True, note="(soft: only if nothing else was sounding)"),
        CMD("mod set %d 0 0.5" % slot, expect=r"mod: queued", soft=True, note="parameter 0 of the module (soft: the bind may not have a parameter 0 of that range)"),
        W(0.5),
        CMD("mod off %d" % slot, expect=r"mod: off queued"),
        W(2.5),
        CMD("mix", expect_not=r"(?m)^\s*T16\s", within=5, poll=0.5, soft=True, note="the audition's meter is empty after its release"),
    ]
mods += [
    SAY("the same through the UI: PARAMS, a free slot (instrument 17), F4 opens the engine picker listing the modules (OS-INSTUI S5: F1 is the SOURCE page now)"),
    K("params", gap=0.5), ST({"screen": "INSTRUMENT"}, within=3),
    K("f6", gap=0.08, repeat=70, note="(to instrument 1: F6 stops there, no wrap)"),
    K("f7", gap=0.1, repeat=16), ST({"instrument": "17"}, within=3),
    K("f4", gap=0.5), ST({"overlay": "MODULE_PICKER"}, within=3), SHOT("module_picker"),
    K("f8", gap=0.4), ST({"overlay": "NONE"}, within=3),
    K("f6", gap=0.08, repeat=70),
    CMD("cpu", expect=r"underrun 0\b"),
    K("pattern", gap=0.4),
]
add("modules", "built-in modules: `mod` listing, load tb303 / chip / fm / sid, bind each to a free instrument (33..36), `mod note` sounds on track 16 (meters rise), `mod off`, the engine picker opens", mods,
    finally_=clear_overlays() + [K("pattern", gap=0.4), CMD("mod off 33"), CMD("mod off 34"), CMD("mod off 35"), CMD("mod off 36"), RUN("_stop")])

# ======================================================================================================================================
# 11. sample_edit_undo
# ======================================================================================================================================
def echo_inst(n, label):
    """`inst n` prints on a failed match only: this step fails on purpose (soft) so that the runner prints the reply, whose `sum` the human compares."""
    return CMD("inst %d" % n, expect=r"@@never@@", soft=True, note="(%s: this soft failure PRINTS the unit's `inst %d` line: compare its `sum` and `len` by eye)" % (label, n))


se = start_on_pattern() + [
    K("sampleedit", gap=0.5), ST({"screen": "SAMPLE_EDIT"}, within=3),
    PAD(0, note="a pad on the Sample edit screen picks the instrument: 1"), ST({"instrument": "1"}, within=2),
    CMD("inst 1", expect=r"inst 01: mode \d+ start \d+ len \d+", note="the sample of instrument 1 is bound (else this scenario has nothing to edit)"),
    SHOT("sample_edit_before"),
    echo_inst(1, "before"),
    K("f8", gap=0.4, note="the sample's own undo on an empty history"),
    ST({"msg": "~^NOTHING_TO_UNDO$"}, within=2, soft=True),
    SAY("REVERSE (F3): a job on the main loop, committed when it is done; the engine takes it with the transport stopped"),
    K("f3", gap=0.5), W(4.0),
    ST({}, ne={"msg": "~^NO_SAMPLE_IN_THIS_INSTRUMENT$"}, within=2),
    echo_inst(1, "after REVERSE"),
    SHOT("sample_edit_reversed"),
    K("f8", gap=0.1), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, note="UNDO: the reverse was a step of the sample's history (the peaks job that follows an edit posts PEAKS_<name>_DONE over UNDONE within a second: both are right)"),
    W(1.5), echo_inst(1, "after UNDO: the sum is the first one again"),
    K("+shift", "f8", "-shift", gap=0.1), ST({"msg": "~^(REDONE|PEAKS_.*_DONE)$"}, within=3),
    K("f8", gap=0.1), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3),
    SAY("NORMALISE (F2): then UNDO (soft: an already normalised sample says NOTHING TO NORMALISE and makes no step)"),
    K("f2", gap=0.5), W(4.0),
    K("f8", gap=0.1), ST({"msg": "~^(UNDONE|NOTHING_TO_UNDO|PEAKS_.*_DONE)$"}, within=3),
    ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=1, soft=True, note="(soft: UNDONE or the peaks job after it means normalise made a step)"),
    W(1.5), echo_inst(1, "after UNDO of NORMALISE: the same sum again"),
    SHOT("sample_edit_restored"),
    K("f8", gap=0.5), ST({"msg": "~^NOTHING_TO_UNDO$"}, within=3, soft=True, note="the history is empty: the sample is as loaded"),
]
add("sample_edit_undo", "Sample edit on instrument 1 of t-v16: REVERSE, UNDO, REDO, UNDO, NORMALISE, UNDO; the history says so (UNDONE / REDONE / NOTHING TO UNDO); `inst 1` is printed before / after for the sum; never saved", se,
    finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("sampleedit", gap=0.5, if_state={"screen": "~^(?!SAMPLE_EDIT$)"}), K("f8", repeat=4, gap=0.4), K("pattern", gap=0.4), RUN("_stop")])

# ======================================================================================================================================
# 12. record_screen
# ======================================================================================================================================
rec = [K("-shift", gap=0.2)] + clear_overlays() + to_pattern() + [
    RUN("_stop"),
    CMD("mon off", expect=r"monitor gain"),
    K("samplerec", gap=0.5), ST({"screen": "RECORD"}, within=3),
    K("samplerec", gap=0.4, if_state={"page": "1"}), K("samplerec", gap=0.4, if_state={"page": "2"}), ST({"page": "0"}, within=2),
    SHOT("record_sources"),
    SAY("the input meters: `mon` routes the line input through the engine into MAIN and CUE"),
    MIX({"channels": {"MST L": {}}, "snap": "mon_off"}),
    CMD("mon 100", expect=r"monitor gain \(Q16, 65536 = unity\) \d+"),
    W(1.5),
    CMD("mix", expect=r"(?m)^\s*IN L\s", within=4, poll=0.5, soft=True, note="(soft: the line input shows in the IN L meter once it carries anything: a tone, or its own noise)"),
    MIX({"rise": {"channel": "MST L", "field": "hold", "from": "mon_off", "min_db": 3}, "within": 4}, soft=True, note="(soft: needs a signal on the LINE IN: connect a source for this one)"),
    CMD("mon off", expect=r"monitor gain \(Q16, 65536 = unity\) 0\b", within=3, poll=0.5, note="the monitor level reads back 0"),
    SAY("the pages RESAMPLE and RADIO: the screens come up; the radio chip answers `radio`"),
    K("samplerec", gap=0.4), ST({"screen": "RECORD", "page": "1"}, within=2), SHOT("record_resample"),
    K("samplerec", gap=0.4), ST({"screen": "RECORD", "page": "2"}, within=2), SHOT("record_radio"),
    CMD("radio", expect=r"pins: RST#", note="the driver's text and the pins' levels"),
    CMD("radio", expect=r"(?i)(state|step|tuned|ok|ready|fail)", soft=True),
    K("samplerec", gap=0.4), ST({"screen": "RECORD", "page": "0"}, within=2),
    SAY("no take is armed or saved here: F1, Rec, F3 and F7 are never pressed"),
    ST({"screen": "RECORD"}, ne={"msg": "~^(SAVED|TAKE)"}, soft=True),
    K("pattern", gap=0.4), ST({"screen": "PATTERN"}, within=3),
]
add("record_screen", "Record screen: three pages, the input meter / master follow `mon`, the radio's `radio` report; nothing is recorded or saved", rec,
    finally_=[CMD("mon off")] + clear_overlays() + [K("pattern", gap=0.4)])

# ======================================================================================================================================
# 13. persistence (OS-PERSIST2)
# ======================================================================================================================================
# The saved folder is the OS's own proposal: SAVE AS opens the keyboard on the first free "Track NN" and F8 accepts it (as qa_files_b does), so there is no fixed name to run out of. The old
# "0hilp<digit>" names did run out (--var RUN, ten folders): the OS has no erase, and DELETE only hides a folder (its name stays taken). The project just saved is the newest of the RECENT group, the
# first rows of the File list, so it is reopened from the first row, and the first row is checked to be that very "Track NN" ({{SAVED}}) before F1 (`sel`). At the end it is hidden by DELETE (an empty .deleted in its folder;
# the folder, about 8 MB, stays: remove the Projects/Track NN folders that hold a .deleted from a computer now and then); the DELETE presses are guarded by the NN of the save-done message (o3hil `keep`, {{SAVED}}),
# so no other Track NN can be hidden. The File page is normalised to PROJECTS (F6 twice) before the first File key, as the OG PROJECT scenarios do.
persist = [
    SAY("a card is in the unit, not write protected (any SD card; --expect-volume is optional), or nothing below runs"),
    {"card": {}},
] + start_on_pattern() + [
    SAY("1. a MUTE MODE: the TRACKS layer (Shift held), row 3 of the pads, track 3; the pads' lights change"),
    K("+shift", gap=0.4), ST({"shift": "1"}, within=2),
    LEDS({"within": 3, "snap": "mm0"}),
    PAD(26, note="MUTE MODE of track 3"),
    LEDS({"differs_from": "mm0", "within": 3, "snap": "mm1"}, soft=True),
    K("-shift", gap=0.4),
    SAY("2. a PERFORM scene: RESET, slot 1 row 1 on, F1 held 1.3 s stores scene 1"),
    K("performance", gap=0.5), ST({"screen": "PERFORM"}, within=3),
    K("f8", gap=0.4), LEDS({"within": 3, "snap": "pf0"}),
    PAD(12), LEDS({"differs_from": "pf0", "within": 3, "snap": "pf1"}, soft=True),
    K("f1:1300", gap=0.3), ST({"msg": "~^SCENE_1_STORED$"}, within=3),
    K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=3),
    SAY("3. SAVE AS: File F5 opens the keyboard on the first free 'Track NN'; F8 accepts that name (nothing to run out of)"),
    K("+shift", "fx", "-shift", gap=0.4, note="(Shift + Fx says FILL / EXPORT: NOT YET: the last message is a known one, not an old SAVE ... DONE)"),
    ST({"msg": "~^FILL:"}, within=2, soft=True),
    K("file", gap=0.5), ST({"screen": "FILE"}, within=3),
    K("f6", "f6", gap=0.4, note="(the File screen keeps its page when it is left, EXPORT after qa_wavexp: F6 twice ends on PROJECTS from EXPORT or PROJECTS; steps 5 and 7 below find the page as it is left here)"),
    K("f5", gap=0.5), ST({"overlay": "TEXT_ENTRY"}, within=3),
    SHOT("save_as_keyboard"),
    K("f8", gap=0.5, note="F8 accepts the proposed name (F7 would cancel)"),
    ST({"overlay": "~^(NONE|NOTIFY)$"}, within=3),
    ST({"msg": "~^SAVE_TRACK_[0-9]+_DONE$"}, within=240, poll=1.0, keep={"as": "SAVED", "field": "msg", "re": "^SAVE_TRACK_([0-9]+)_DONE$"},
       note="the project's own save job is through (the SAVE_INSTRnn_DONE messages are the samples', one each, and come first; an Open before the end aborts the save); its NN is kept as {{SAVED}} for the DELETE guard of step 7"),
    W(3.0),
    SAY("4. put the runtime state back to what the file does NOT have: mute mode of track 3, scene slot"),
    K("pattern", gap=0.4),
    K("+shift", gap=0.4), PAD(26), LEDS({"within": 3, "snap": "mm2"}), K("-shift", gap=0.3),
    K("performance", gap=0.5), K("f8", gap=0.4), LEDS({"within": 3, "snap": "pf2"}), K("pattern", gap=0.5),
    SAY("5. open the saved project from File: it is the newest of the RECENT group, the first row after a RESCAN; F1 opens the project under the cursor"),
    K("file", gap=0.5), K("f2", gap=0.5, note="RESCAN"), W(2.0),
    K("+shift", "f1", "-shift", gap=0.4, note="the cursor to the first entry"),
    ST({"sel": "~^Track_{{SAVED}}$"}, within=3, note="the first row is the Track NN saved in step 3 ({{SAVED}}), not any Track NN (if not, the RECENT group is not first or another project is newer: nothing is opened)"),
    K("f1", gap=0.5),
    K("f1", gap=0.5, if_state={"msg": "~^UNSAVED_CHANGES"}, note="(only when the strip asks: the state put back counts as a change, and this open is meant to discard it)"),
    ST({"msg": "~(?i)^OPENING_TRACK_{{SAVED}}(?![0-9])"}, within=4, note="the project just saved ({{SAVED}}) is opening"),
    CMD("sd", expect=r"load:\s+Projects/Track [0-9]+", within=180, poll=2.0, fail_on="FAILED", soft=True),
    CMD("sd", expect=r"pcm:\s+.*loaded", within=120, poll=2.0, fail_on="FAILED", soft=True),
    CMD("sd", expect_not=r"[1-9]\d* values reset", soft=True, note="(the load line says `N values reset` when a chunk had values out of range)"),
    W(3.0),
    ST({"screen": "PATTERN"}, within=5, soft=True, note="QA-FIL-095 (OS-OPENTOPAT): a project that opened shows the Pattern view by itself"),
    K("pattern", gap=0.5, note="(already there since OS-OPENTOPAT; the Pattern button on its own screen changes nothing)"), ST({"screen": "PATTERN", "overlay": "~^(NONE|NOTIFY)$"}, within=5),
    SAY("6. the saved values are back: the lights are not the ones of the state put back"),
    K("+shift", gap=0.4), LEDS({"differs_from": "mm2", "within": 3}, soft=True, note="MUTE MODE of track 3 is back (the layer's row 3 differs from the state put back)"), K("-shift", gap=0.3),
    K("performance", gap=0.5), ST({"screen": "PERFORM"}, within=3),
    LEDS({"differs_from": "pf2", "within": 3}, soft=True, note="the scene's slot FX is not in the lights, but scene recall is: F1 recalls it"),
    K("f1", gap=0.5), ST({}, ne={"msg": "~^SCENE_IS_EMPTY"}, within=2, note="scene 1 is stored in the loaded project (an empty scene says SCENE IS EMPTY)"),
    LEDS({"differs_from": "pf2", "within": 3}, soft=True, note="the recalled scene lights its slot"),
    K("f8", gap=0.4), K("pattern", gap=0.5),
    SAY("7. hide the scratch project (the OS has no erase: DELETE writes an empty .deleted in the folder and the File list stops showing it). It is the open project, which DELETE refuses, so t-v16 is loaded first. "
        "Every press is guarded by `sel`, the name under the cursor, and the second Delete by the question in the strip: both must name the Track NN saved in step 3 ({{SAVED}}), nothing else can be hidden"),
    RUN("_load_tv16"),
    K("file", gap=0.5), ST({"screen": "FILE"}, within=3), K("f2", gap=0.5, note="RESCAN"), W(2.0),
    K("+shift", "f1", "-shift", gap=0.4, note="the cursor to the first entry"),
    K("down", gap=0.4, if_state={"sel": "~^(?!Track_{{SAVED}}$)"}, note="(t-v16 may be the newest row: one Down to the project just saved, at most two)"),
    K("down", gap=0.4, if_state={"sel": "~^(?!Track_{{SAVED}}$)"}),
    ST({"sel": "~^Track_{{SAVED}}$"}, within=2, soft=True, note="(soft: the cursor is on the Track NN saved in step 3; if not, nothing below presses Delete)"),
    K("delete", gap=0.6, if_state={"sel": "~^Track_{{SAVED}}$"}, note="the question"),
    ST({"msg": "~^DELETE_TRACK_{{SAVED}}\\?"}, within=3, soft=True),
    K("delete", gap=0.8, if_state={"msg": "~^DELETE_TRACK_{{SAVED}}\\?", "sel": "~^Track_{{SAVED}}$"}, note="the second press marks it deleted (sent only while the strip shows the question for the project just saved)"),
    ST({"msg": "~^DELETED:_TRACK_{{SAVED}}$"}, within=4, soft=True, note="(soft: the project is hidden; if not, the Track NN folder stays listed)"),
    K("pattern", gap=0.5),
]
add("persistence", "OS-PERSIST2: change a MUTE MODE and a PERFORM scene, SAVE AS the automatic name Track NN (writes the card), put the state back, open the project from File (the newest of the RECENT group), the values are back; the scratch project is hidden by DELETE at the end (its folder stays on the card)", persist,
    needs=["card"],
    finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.5), RUN("_load_tv16"), K("pattern", gap=0.5)])

# ======================================================================================================================================
# 14. stress_voices
# ======================================================================================================================================
stress = start_on_pattern() + [
    pad_mode_notes(), ST({"pads": "NOTES"}, within=2),
    RUN("_play"), W(1.0), RUN("_stop"), W(1.0),
    CMD("cpu", expect=r"underrun 0\b", note="baseline: no underrun since boot (the counters are since boot: reboot between unrelated sessions)"),
    SAY("A: twelve pads held down at once (the pool has 12 voices)"),
]
for i in range(12):
    stress.append(PAD(i, "down"))
stress += [
    VOX({"min_in_use": 6}, soft=True, note="(soft: one-shot samples end by themselves, the held pads keep looped ones)"),
    CMD("voices", expect=r"in use (?:\d|1[0-2])/12\s+peak (?:\d|1[0-2])\s", note="never more than the pool"),
]
for i in range(12):
    stress.append(PAD(i, "up"))
stress += [
    W(1.5),
    SAY("B: twenty pads held: eight more than the pool: the oldest voices are stolen, never an overflow"),
]
for i in range(20):
    stress.append(PAD(i, "down"))
stress += [
    CMD("voices", expect=r"in use (?:\d|1[0-2])/12\s+peak (?:\d|1[0-2])\s+steals \d+", note="in use and peak stay inside 12"),
    CMD("voices", expect=r"@@never@@", soft=True, note="(this soft failure PRINTS the voices line: steals after twenty pads on a pool of twelve is expected to be above 0 when the samples sustain)"),
    CMD("cpu", expect=r"underrun 0\b"),
]
for i in range(20):
    stress.append(PAD(i, "up"))
stress += [
    W(2.0),
    SAY("C: while the pattern plays, 36 taps over the pads (rapid previews on top of 12 tracks)"),
    RUN("_play"), W(2.0),
]
for i in range(36):
    stress.append(PAD((i * 5) % 48, "tap"))
stress += [
    CMD("voices", expect=r"in use (?:\d|1[0-2])/12\s+peak (?:\d|1[0-2])\s"),
    MIX({"channels": {"MST L": {"min_rms_db": -70, "max_clip": 0}, "MST R": {"max_clip": 0}}, "within": 4}, note="the audio is intact and the master never clipped"),
    CMD("cpu", expect=[r"underrun 0\b", "shed 0 blocks"], note="underruns stay strict: a missed audio block is a failure"),
    CMD("cpu", expect=r"over-budget \d+", show=True, note="REPORTED, not a failure: over-budget is the render governor's count of blocks over its budget, and a burst of 36 taps over 12 playing tracks may cost one (0.4.12 on the unit: over-budget 1, render max 60.6 %, 0 underruns, 0 mutes). Read the line in the log"),
    RUN("_stop"),
    CMD("cpu", expect=r"underrun 0\b", note="and none after"),
]
add("stress_voices", "rapid pad bursts: 12 and 20 pads held, 36 taps over the playing pattern; the voice pool stays inside 12, `cpu` shows no underrun, the master never clips", stress,
    finally_=[PAD(i, "up") for i in range(20)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_stop")])

# ======================================================================================================================================
# 15. config_display
# ======================================================================================================================================
def to_theme_row():
    return [K("config", gap=0.5, if_state={"screen": "~^(?!CONFIG$)"}), ST({"screen": "CONFIG"}, within=3), K("f2", gap=0.4), ST({"page": "1"}, within=2),
            K("up", repeat=6, gap=0.12), K("down", repeat=4, gap=0.2, note="row 5 of DISPLAY: THEME (after DISPLAY, PAD LIGHTS, NUMBERING, MIDDLE C)")]


CARD_STEP = {"card": {}, "note": "a device setting is written to Settings/device.o3st on the card 1.5 s after the change"}
cd = [CARD_STEP] + start_on_pattern(load=False) + to_theme_row() + [
    K("delete", gap=0.4, note="THEME back to its default, Amiga: the run starts from a known theme"),
    SHOT("config_amiga"),
    K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=3),
    SHOT("pattern_amiga", compare=CMP("themes", "amiga"), soft=True, note="(soft: the key-cap row against the simulator's Amiga theme)"),
] + to_theme_row() + [
    ENC(1, note="THEME: Black"),
    SHOT("config_black"),
    K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=3),
    SHOT("pattern_black", compare=CMP("themes", "black_by_key"), soft=True, note="(soft: the key-cap row now matches the simulator's BLACK theme, and the earlier Amiga shot did not)"),
] + to_theme_row() + [
    ENC(1, note="THEME: White"),
    K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=3),
    SHOT("pattern_white", compare=CMP("themes", "white_by_key"), soft=True),
] + to_theme_row() + [
    K("delete", gap=0.4, note="Amiga again"),
    K("pattern", gap=0.5), ST({"screen": "PATTERN"}, within=3),
    SHOT("pattern_amiga_again", compare=CMP("themes", "amiga_again"), soft=True),
    SAY("the Shift + Master shortcut cycles the same themes (global, from any screen)"),
    K("+shift", "master", "-shift", gap=0.5), SHOT("pattern_by_key_black", compare=CMP("themes", "black_by_key"), soft=True),
    K("+shift", "master", "-shift", gap=0.5), K("+shift", "master", "-shift", gap=0.5), SHOT("pattern_by_key_amiga", compare=CMP("themes", "amiga_again"), soft=True),
]
add("config_display", "Config DISPLAY page: THEME Amiga -> Black -> White -> Amiga (and Shift + Master): each theme's screen is not blank and its key-cap row matches the simulator's theme (soft); the theme is put back to Amiga", cd,
    finally_=[K("-shift", gap=0.2)] + clear_overlays() + to_theme_row() + [K("delete", gap=0.4), K("pattern", gap=0.5)], needs=["card"])

# ======================================================================================================================================
# 16. tracks_9_12 (OS-TRK912)
# ======================================================================================================================================
t912 = start_on_pattern() + [
    K("left", repeat=130, gap=0.1, note="to track 1, NOTE column (the cursor stops at the edge)"),
    ST({"screen": "PATTERN", "track": "0", "col": "NOTE"}, within=3),
    SHOT("tracks_1_5", compare=CMP("pattern_basic", "initial"), soft=True, note="(soft: the key-cap row against the simulator's first view)"),
    K("right", repeat=40, gap=0.1, note="5 columns a track: 40 presses is track 9"),
    ST({"track": "8", "col": "NOTE"}, within=3),
    SHOT("tracks_9_12_first"),
    K("right", repeat=15, gap=0.1), ST({"track": "11", "col": "NOTE"}, within=3),
    SHOT("track_12"),
    K("right", repeat=20, gap=0.1, note="past the end: the cursor stays on the last column of the last track"),
    ST({"track": "11", "col": "FX2"}, within=3),
    SHOT("tracks_9_12_last", compare=CMP("pattern_basic", "tracks_8_12"), soft=True, note="(soft: the key-cap row against the simulator's last view)"),
    SAY("(`state` has no track names: the names of tracks 9-12 are checked on the shots by eye)"),
    K("left", repeat=130, gap=0.1), ST({"track": "0", "col": "NOTE"}, within=3),
]
add("tracks_9_12", "OS-TRK912: the Pattern view scrolls to tracks 9-12 (state track 8 .. 11, col FX2 at the end) and back; shots of each window are not blank", t912,
    finally_=clear_overlays() + [K("pattern", gap=0.4), K("left", repeat=130, gap=0.1), RUN("_stop")])

# ======================================================================================================================================
# helpers for MANUAL.md: the clock settings on the Config MIDI page
# ======================================================================================================================================
def midi_row(row):
    return [K("config", gap=0.5, if_state={"screen": "~^(?!CONFIG$)"}), ST({"screen": "CONFIG"}, within=3), K("f1", gap=0.4), ST({"page": "0"}, within=2),
            K("up", repeat=10, gap=0.1)] + ([K("down", repeat=row, gap=0.2)] if row else [])


# rows of the MIDI page (the kGroupMidi rows of model/src/params.cpp kDevice, in table order): 0 CLOCK IN, 1 CLOCK SYNC, 2 CLOCK MODE, 3 MIDI IN, 4 MIDI OUT, 5 IN CHANNEL, 6 OUT CHANNEL.
# (METRO VOL left this page with OS-METRO.) `up` x10 clamps at the first row, which is CLOCK IN: the scenarios start from the top and count only from there. If a row is added above
# CLOCK IN, midi_row(0) still lands on the first row: re-check the `down` counts that follow.
add("cfg_clock_master", "setup for MANUAL.md: Config MIDI, CLOCK IN Internal, CLOCK MODE Master, MIDI OUT DIN + USB: the unit sends MIDI clock while it plays (writes Settings/device.o3st)", [CARD_STEP] + clear_overlays() + to_pattern() + [
    ] + midi_row(0) + [K("delete", gap=0.3)] + [K("down", repeat=2, gap=0.2), K("delete", gap=0.3), ENC(1, note="CLOCK MODE: Master"), K("down", repeat=2, gap=0.2), K("delete", gap=0.3, note="MIDI OUT: DIN + USB"), SHOT("clock_master"),
    K("pattern", gap=0.5)], finally_=clear_overlays() + [K("pattern", gap=0.4)], needs=["card"])
add("cfg_clock_slave_usb", "setup for MANUAL.md: Config MIDI, CLOCK IN USB (the unit follows the MIDI clock of the computer over USB; writes Settings/device.o3st)", [CARD_STEP] + clear_overlays() + to_pattern() +
    midi_row(0) + [K("delete", gap=0.3), ENC(1, note="CLOCK IN: USB"), K("down", repeat=2, gap=0.2), K("delete", gap=0.3, note="CLOCK MODE Off: CLOCK IN rules"), SHOT("clock_slave_usb"), K("pattern", gap=0.5)],
    finally_=clear_overlays() + [K("pattern", gap=0.4)], needs=["card"])
add("cfg_clock_internal", "setup for MANUAL.md: Config MIDI, CLOCK IN Internal, CLOCK MODE Off (the defaults; writes Settings/device.o3st)", [CARD_STEP] + clear_overlays() + to_pattern() +
    midi_row(0) + [K("delete", gap=0.3), K("down", repeat=2, gap=0.2), K("delete", gap=0.3), SHOT("clock_internal"), K("pattern", gap=0.5)],
    finally_=clear_overlays() + [K("pattern", gap=0.4)], needs=["card"])


# ======================================================================================================================================
# helpers for MANUAL.md: playing for the PC's capture, the cursor for MIDI notes, the Learn overlay
# ======================================================================================================================================
add("play_start", "setup for MANUAL.md: t-v16 loaded and its SONG playing (the load opens on the project's saved pattern, 5, where only T01 sounds), left playing (for the USB AUDIO capture on the PC); play_stop ends it", start_on_pattern() + [pad_mode_notes(), ST({"pads": "NOTES"}, within=2, note="(the pads mode is set before the Shift layer, as cpu_mute_ab does: a former run may have left another one)"),
                          SAY("the project saves tracks 1 6 7 8 muted: no mute is left to chance (a blind tap toggles): the MUTE row is read back from the pad LEDs and only a wrong pad is tapped, then asserted")]
    + set_tracks([], None, "play_start") + [RUN("_play_song"), W(3.0), MIX({"min_tracks": 4, "channels": {"MST L": {"min_rms_db": -60}}, "within": 8})])
add("play_stop", "setup for MANUAL.md: stop the playback and go back to Pattern", clear_overlays() + to_pattern() + [RUN("_stop")])
add("midi_notes_ready", "setup for MANUAL.md: t-v16 loaded, Pattern in Browse with the cursor on track 2 (a sounding track) and instrument 1: USB / DIN MIDI notes then play instrument 1 on track 2",
    start_on_pattern() + [RUN("_play"), W(1.0), RUN("_stop"), W(1.0, note="(the outputs are open now)"),
                          K("rec", gap=0.4, if_state={"mode": "0"}), ST({"mode": "3"}, within=2),
                          K("left", repeat=130, gap=0.1), K("right", repeat=5, gap=0.15), ST({"track": "1", "col": "NOTE"}, within=3),
                          K("params", gap=0.5), *SELECT_1, ST({"instrument": "1"}, within=2), K("pattern", gap=0.5), ST({"screen": "PATTERN", "mode": "3", "track": "1"}, within=3)])
LEARN_ROW = [K("params", gap=0.5), ST({"screen": "INSTRUMENT"}, within=3), K("f2", gap=0.4), ST({"page": "0"}, within=2), K("up", repeat=6, gap=0.12), *SELECT_1, ST({"instrument": "1"}, within=2)]
add("learn_open", "setup for MANUAL.md: Params page VOICE, row VOLUME of instrument 1, the Learn overlay open (FX1 + FX2); then send a CC from the PC and run learn_close", start_on_pattern() + LEARN_ROW + [
    K("+vol", "+fx", gap=0.0), K("-fx", "-vol", gap=0.1), ST({"overlay": "LEARN"}, within=3), SHOT("learn_open")])
add("learn_close", "setup for MANUAL.md: close the Learn overlay (F8) and show the page: the bound CC moves VOLUME of instrument 1 with the pickup", [
    K("f8", gap=0.4, if_state={"overlay": "LEARN"}), ST({"overlay": "~^(NONE|NOTIFY)$"}, within=3), SHOT("learn_closed")])

# ======================================================================================================================================
# OS-USBDISK U1d: the Linux scenarios of the QA rows QA-GLB-015..017 and 019..021 (docs/qa/catalogue/global.md)
# ======================================================================================================================================
# THE PC SIDE reads and writes ONLY the unit's own removable USB disk (o3disk.py proves the disk is the unit's 16d0:0e9f, removable, not mounted at /, and the volume serial of --expect-volume when that is given, before it touches a file; any SD card, no marker),
# and only on Linux: Windows and macOS are UNTESTED, o3hil.py skips these scenarios there ("not run, not failed"). Each scenario starts with the unit ALREADY in the boot disk mode (a person held
# Shift + F1 + F7 at power-up; the runner cannot power-cycle the unit), an SD card in the slot and the cable to the Linux computer; run with --port (and, if you want the check, --expect-volume XXXX-XXXX).
DISK_HEAD = ("OS-USBDISK U1d, rows QA-GLB-015..017, 019..021. Linux only (Windows and macOS: untested, not run). THE UNIT MUST ALREADY BE IN THE BOOT DISK MODE: power it off, hold Shift + F1 + F7 while powering on until the strip "
             "'USB DISK - EJECT ON THE COMPUTER OR PRESS A KEY' shows, then start this run (--expect-volume XXXX-XXXX is optional); the PC side touches nothing else. "
             "NO KEY IS PRESSED until the last step (a key restarts the unit; while the computer has the disk writable and mounted it takes two presses)")


# ======================================================================================================================================
# OS-FILES-C C2c: the OG PROJECT row of the File screen's EXPORT page (rows QA-FIL-082..089 of docs/qa/catalogue/file.md), T10 and T16 of docs/specs/OS-FILES-C.md
# ======================================================================================================================================
OG_ROW = [
    K("-shift", gap=0.2)] + clear_overlays() + [
    K("file", gap=0.5), ST({"screen": "FILE"}, within=4),
    K("f6", "f6", gap=0.4, note="(the File screen keeps its page when it is left: F6 twice ends on PROJECTS from EXPORT or PROJECTS, so that the F7 below is EXPORT, not PROJECTS back)"),
    K("f7", gap=0.4), K("down", repeat=4, gap=0.3, note="SONG, PATTERN, SONG STEMS, PATT STEMS, OG PROJECT: the fifth row (four Downs end on it whatever row the page had: it stops at the last)"),
]
OG_START = [
    CMD("cpu reset", note="(T16: the cpu counters count from here)"),
    CMD("ogexp status", show=True, note="the last export of this boot, if any (IDLE before the first)"),
    K("f1", gap=0.7, note="F1: with something left out the strip asks first (LOSES ..: F1 AGAIN), else the export starts at once"),
    ST({}, ne={"msg": "~^NO_CARD_WRITE_LAYER$"}, within=2, note="the unit has the card write layer"),
    K("f1", gap=0.6, if_state={"msg": "~^LOSES_"}, note="the second F1 (only when the question was asked: a project that loses nothing has started already)"),
    ST({"overlay": "EXPORT_PROGRESS"}, within=8, note="the modal progress overlay: the transport was stopped first"),
]
og = OG_ROW + [
    SAY("OS-FILES-C C2c, rows QA-FIL-082..089 of docs/qa/catalogue/file.md (docs/specs/OS-FILES-C.md section 8, T10 and T16): the EXPORT page's OG PROJECT row. It WRITES on the card in the unit: new folders Export/<project> OG, Export/<project> OG 2 ..; it never replaces or removes a file. The project is t-v16 (16 samples, about 7.6 MB: the maintainer's, on the card). No official unit has opened an export yet: the row says EXPERIMENTAL"),
    {"card": {}},
    RUN("_load_tv16"),
] + OG_ROW + [
    SHOT("og_row", compare=CMP("qa/qa_file", "qa_fil_082"), soft=True, note="QA-FIL-082: the key row is the page's (the compare is soft: on v0.6.8 91% of the key row's pixels differed from the golden although the labels read the same, so look at the shot; the card and the loss lines are the unit's: a folder name, MB, files, what is left out)"),
    SAY("QA-FIL-082: read the shot: FILE Export/<project> OG, SIZE about n MB, n FILES, EXPERIMENTAL on the row, the LOSES lines (t-v16 may lose none: then it says KEEPS everything the OG format has a place for)"),
] + OG_START + [
    SHOT("og_progress", soft=True, note="the overlay: FILE n OF m, the bar moves by files"),
    SAY("T16: the strip stays alive while it runs: the CPU chip and the clock move, a key does nothing but F8"),
    CMD("ogexp status", expect=r"ogexp: DONE (\d+)/\1 files Export/", within=180, poll=2.0, note="QA-FIL-084 (a project with nothing left out writes exactly the set of section 6) or QA-FIL-083 (it asked first): DONE n/n files"),
    ST({"overlay": "NONE"}, within=10, note="the overlay is gone when the set has ended"),
    CMD("ogexp status", expect=r"step max \d+ us, overruns \d+", show=True, note="T16: the longest single step of any file's job and the steps that overran their slice: RECORD the figures (the cost table of docs/architecture/cpu-budget.md)"),
    CMD("ogexp status", expect=r"time worst \d+ us in \w+", show=True, note="T16: where the time went: the worst step and its stage, then one line a stage (survey, plan, convert, write, sync, rename, dir, verify, other): max us, count, sum ms. RECORD them: the stage with the 263-360 ms is the one to bound"),
    CMD("cpu", expect=r"underrun 0\b", note="T16: no missed audio block while the export ran"),
    CMD("cpu", expect=r"over-budget 0\b", note="T16: the render governor's over-budget count stays 0 (the export is main-loop work, never the render's)"),
    SAY("T16 by hand: MIDI-in jitter during the export (midi_clock_tool.py, a 120 BPM clock in on the DIN: the interval's spread against the same run with no export), and the JOB stats of a project with a 22,050 Hz and an 8,287 Hz sample (add them to t-v16 first, then rerun this scenario). Record the figures in docs/STATUS.md; they decide whether the survey needs slicing"),
    SAY("QA-FIL-083: a project that loses something asks first: the strip says LOSES <n kinds>: F1 AGAIN, another key, a move, leaving the screen or 4 s takes it back; the second F1 counts again. Try it on a project with a module instrument or a 12 BIT instrument (by hand)"),
    SAY("QA-FIL-114 (OS-LFOSINE): an instrument with an LFO of shape Sine in slots 1-48: OG PROJECT, F1: the strip and the page say `1 SINE LFO` (SINE LFO SHAPES WRITTEN AS TRIANGLE); after the export the file holds shape Triangle for that LFO (`load Export/<name> OG`, then the MOD page reads Triangle) and the project in the unit still says Sine (by hand)"),
    SAY("QA-FIL-085 and 086 by ear and `inst n`: `load Export/<name> OG`, then `diag inst n` of a resampled sample and of an Amiga one: pitch and gain against the native project (T8 on the unit); a wavetable instrument is untouched; a project of the 1.1.14 law keeps its gain"),
    SAY("QA-FIL-088: `diag load Export/<name> OG` loads the folder just made (the name is in `diag ogexp status`)"),
    SAY("QA-FIL-089 (hardware, official unit): copy the folder into Projects/ of a card for an official 1.9.x unit and open it there: T9. Until the maintainer has done it the row stays EXPERIMENTAL"),
    SAY("QA-FIL-087: a second export, F8 once it is running: the set stops after the file in hand, the strip says OG EXPORT CANCELLED: n OF m FILES, the folder has no project.mt (look at the card from a computer)"),
] + OG_ROW + OG_START[:-1] + [
    CMD("ogexp status", expect=r"ogexp: RUNNING", within=20, poll=0.3, note="it is running (an F8 before this would cancel an export that has not started: the transport is still being stopped)"),
    K("f8", gap=0.5, note="F8: stop after the file in hand"),
    CMD("ogexp status", expect=r"ogexp: CANCELLED", within=60, poll=1.0, note="QA-FIL-087: CANCELLED n/m files, no project.mt (by hand on the card)"),
    ST({"overlay": "NONE"}, within=10),
    ST({"msg": "~^OG_EXPORT_CANCELLED:"}, within=6, soft=True, note="(soft: the strip may have gone on to the next line)"),
    CMD("cpu", expect=r"underrun 0\b", note="and no underrun after the cancelled one either"),
]
add("qa_files_c", "OS-FILES-C C2c: the File screen's EXPORT page, OG PROJECT row on the card: the row, the question, the overlay and the strip, a whole export of t-v16 with its cost (T16: step max, cpu underruns and over-budget 0), and a cancel; writes Export/<project> OG folders, never replaces a file", og,
    needs=["card"],
    finally_=[K("-shift", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.5)])

def disk_boot_checks():
    return [
        SAY("USBDISK-BOOT (QA-GLB-015): no mount, no project, no outputs, the card is the computer's"),
        CMD("log 400", expect=["boot: chord Shift\\+F1\\+F7, USB DISK mode", "usb: pull-up on, DISK", "usb: disk opened, writable"], expect_not=["boot: sd mounted", "boot: loading Workspace", "audio: outputs open"], within=5, poll=1.0,
            note="the log: the chord was read, the DISK set is on the bus, the card was opened for the computer, writable (the writable step); no mount, no project load, no output opened"),
        CMD("sd", expect=["mount:\\s+not (mounted|tried)", "owner:\\s+host"], expect_not=["mount:\\s+mounted"], note="the card is the computer's: nothing is mounted by FatFs", show=True),
        ST({"msg": "~^USB_DISK_-_EJECT_ON_THE_COMPUTER_OR_PRESS_A_KEY$"}, within=5, poll=0.5, soft=True, note="the strip (soft: the notice is re-posted every second and may have just expired)"),
        SHOT("disk_strip", non_blank=True, soft=True, note="(soft: the screen is the app's own with the persistent strip)"),
    ]


def disk_tail_eject():
    return [
        SAY("USBDISK-EJECT (QA-GLB-017): the computer ejects the medium (udisksctl power-off), the unit says so"),
        DISK("eject", within=20, note="the computer's next command gets NOT READY / MEDIUM NOT PRESENT: the block device is gone or reads 0 blocks"),
        ST({"msg": "~^DISK_EJECTED_-_PRESS_ANY_KEY_TO_RESTART$"}, within=8, poll=0.5, note="the strip: DISK EJECTED - PRESS ANY KEY TO RESTART (read at once: udisksctl power-off then removes the device on the computer's side, and the console goes with it)"),
        SAY("udisksctl power-off also turns the computer's port off: the unit leaves the bus (kernel 'USB disconnect'), so nothing more is read here. "
            "A person presses any key on the unit: it restarts into the normal boot with the card mounted; run boot_version next"),
    ]


add("qa_usbdisk", "OS-USBDISK U1d, Linux only, unit already in the boot disk mode: boot checks, enumerate, copies only off the card, remount, eject; a person holds the chord at power-up",
    [SAY(DISK_HEAD)] + disk_boot_checks() + [
        SAY("USBDISK-ENUM (QA-GLB-019): the computer lists the unit's disk, writable (ro=0) since the writable step, with a medium. This scenario copies only off the card"),
        DISK("enumerate", within=30, ro=False, note="USB 16d0:0e9f bcdDevice 0002, a block device with blocks, the write-protect bit clear (the disk is exported writable; a card that write-protects itself would show ro=1 and fail here)"),
        DISK("mount", within=30, note="mounted by udisksctl if it was not; proves it is the unit's removable disk (and --expect-volume's, when that is given)"),
        SAY("USBDISK-COPY (QA-GLB-020): files off the card with a checksum, two reads that must agree with the copy"),
        DISK("read", size_mb=8, note="the largest files up to 8 MiB are copied to a scratch directory; each copy equals a second, cache-dropped read of the card"),
        SAY("USBDISK-REMOUNT (QA-GLB-021): the computer unmounts and mounts again: the unit does not notice, the card reads the same"),
        DISK("remount", within=30),
        DISK("read", size_mb=1),
        CMD("sd", expect=["owner:\\s+host"], note="the unit still says host"),
    ] + disk_tail_eject(),
    needs=["card", "disk"])

add("qa_usbdisk_rw", "OS-USBDISK U1d, Linux only, the WRITABLE step: copy both ways with a checksum (SIZE_MB, default 16; the release pass is 100), remount, eject",
    [SAY(DISK_HEAD)] + disk_boot_checks() + [
        SAY("USBDISK-ENUM (QA-GLB-019): the computer lists the unit's disk, writable (ro=0)"),
        DISK("enumerate", within=30, ro=False, note="the write-protect bit is clear: the boot disk is exported writable (since 2026-10-17)"),
        DISK("mount", within=30),
        SAY("USBDISK-COPY (QA-GLB-020): both ways with a checksum. The write goes only into o3-usbdisk-test/ on the card"),
        DISK("write", size_mb="${SIZE_MB}", note="writes a deterministic file, fsync, unmounts and mounts again (the computer's cache is gone), reads it back off the card, compares sha256, deletes it"),
        DISK("read", size_mb="${SIZE_MB}", note="and the card's own files off to the computer with two-read agreement"),
        SAY("USBDISK-REMOUNT (QA-GLB-021)"),
        DISK("remount", within=30),
        CMD("sd", expect=["owner:\\s+host"]),
        SAY("QA-GLB-023 is a separate BY-HAND check, not a step of this run (the runner cannot press the unit's keys): in a boot disk session with the disk mounted and NOT ejected, ONE key on the unit "
            "must not restart it: the strip warns 'THE COMPUTER MAY STILL BE WRITING: KEY AGAIN' (and the run above still passes); 6 s later the usual strip is back; a second key inside 5 s restarts. "
            "After the eject below one key restarts at once"),
    ] + disk_tail_eject(),
    needs=["card", "disk"], vars_={"SIZE_MB": "16"})


def disk_chord(case, held, disk, note):
    """One boot per case: the person holds `held` at power-up, then the runner reads what the unit did (QA-GLB-016)."""
    head = ("OS-USBDISK U1d, row QA-GLB-016, case %s: hold %s while powering the unit on, then start this run. Nothing is injected, nothing is written" % (case, held))
    if disk:
        checks = [CMD("log 400", expect=["boot: chord Shift\\+F1\\+F7, USB DISK mode"], expect_not=["boot: sd mounted"], within=5, poll=1.0, note="only Shift + F1 + F7 opens the disk"),
                  CMD("sd", expect=["owner:\\s+host", "mount:\\s+not (mounted|tried)"], show=True)]
    else:
        checks = [CMD("log 400", expect=["boot: chord keys held"], expect_not=["USB DISK mode"], within=5, poll=1.0, note="the chord was read and is not the disk chord"),
                  CMD("sd", expect=["owner:\\s+tracker"], show=True, note="a normal boot: the Tracker owns the card (a card in the slot is mounted)")]
    steps = [SAY(head)] + checks
    if case == "all4":
        steps.append(ST({"screen": "~^INPUT"}, within=5, soft=True, note="Shift + F1 + F7 + F8 is INPUT TEST (soft: the screen name)"))
    add("qa_usbdisk_chord_" + case, "OS-USBDISK U1d, QA-GLB-016: power on holding %s: %s" % (held, "the boot disk opens" if disk else "no disk mode"), steps)


disk_chord("exact", "Shift + F1 + F7", True, "")
disk_chord("shift_f1", "Shift + F1 only", False, "")
disk_chord("shift_f7", "Shift + F7 only", False, "")
disk_chord("f1_f7", "F1 + F7 without Shift", False, "")
disk_chord("all4", "Shift + F1 + F7 + F8", False, "")


# OS-POWER P1 (docs/specs/OS-POWER.md section 8, QA-GLB-100..116): the Power switch. The console injection `power press|release` feeds core::PowerHold the way the pin does, so the line, the hint and the
# commit need no finger; the physical switch rows (the polarity and the pull, a real hold, a hold through the boot, Shift + Power, the blocking-call gap) are the person's, the SAY steps say
# which. THE LAST STEP RESTARTS THE UNIT: `power off dry` runs the stages 0 to E (the USB goes at D) and restarts after Power is released (none is held), so the run ends there: nothing after it can be read.
# The first run on the unit is `power` and `power off dry` with NO button held (QA-GLB-113), before any real hold.
POWER_HEAD = ("OS-POWER P1, rows QA-GLB-100..116 of docs/qa/catalogue/global.md. The unit is idle, the Pattern screen, a card in the slot if QA-GLB-106/107 are to be run by hand. NO BUTTON is held "
              "for the console steps. The scenario injects the switch (`power press|release`: the debounced hold, the line, the hint) and ends with `power off dry`, which restarts the unit")

add("qa_power", "OS-POWER P1: the Power switch: the pin in the three pulls (M4), an injected hold (the line, the hint), and the shutdown's stages up to USB off, ending in a restart. No button held",
    [SAY(POWER_HEAD)] + clear_overlays() + to_pattern() + [
        RUN("_stop"),
        CMD("power", expect=["power: PTC3 (high|low) \\(pull (none|up|down)\\), the switch reads (released|pressed)", "power: read now: no pull (high|low), pull-up (high|low), pull-down (high|low)",
                             "power: board: pressed is (low|high), internal pull (none|up|down)", "power: hold (Booting|Armed)", "power: shutdown idle"], show=True,
            note="QA-GLB-113 / M4: the raw level of PTC3 in the three pulls with Power UP (released). Then hold Power and run `power` again (the next SAY): the level that flips is the polarity, the pull that keeps it steady is the pull. A hold in Booting: wait a moment after the boot"),
        CMD("power", expect=["power: hold Armed"], within=10, poll=1.0, note="the hold is armed (the app has the screen and the switch was released for 100 ms)"),
        SAY("QA-GLB-113 / M4, by hand: HOLD Power with a finger and keep it down for 3 s, then run `power` in a terminal (not this runner) and note the three readings; let go. The scenario goes on without a finger"),
        K("-shift", gap=0.2),
        CMD("power release", expect=["power: the console holds the switch up"]),
        W(0.5),
        CMD("power press", expect=["power: the console holds the switch down"], note="QA-GLB-100: an injected hold, as the debounced pin would feed it. The unit commits at 2 s: no screenshot is taken during it (a shot takes longer than the hold)"),
        W(1.0),
        CMD("power", expect=["power: hold Holding, debounced down, line [3-7]\\d\\d/1000", "power: shutdown idle"], show=True,
            note="QA-GLB-100: the hold fills (about half after 1 s) and has not committed"),
        SAY("LOOK AT THE SCREEN NOW (QA-GLB-100): a line across the top edge about half full and the strip says HOLD TO SWITCH OFF (UNSAVED WORK - LET GO TO KEEP IT when the project has changes)"),
        CMD("power release", expect=["power: the console holds the switch up"]),
        W(0.3),
        CMD("power", expect=["power: hold Armed", "power: presses [1-9]\\d* holds [1-9]\\d* taps [1-9]\\d* commits 0 refusals 0"], show=True,
            note="QA-GLB-100/101: released after about 1.2 s: back to Armed, one tap counted, no commit; the strip says HOLD POWER 2 S TO SWITCH OFF for 1.5 s (look at it)"),
        SAY("QA-GLB-116 (the latch): start a song first by hand if the audio checks are wanted (Play), with a scope or a recorder on the line out and the status LED in view. `power off dry` follows: from the commit the LED keeps its heartbeat and never shows the 11-pulse audio-fault code, the sound fades in about 50 ms with no click, then USB goes (this console ends), the screen fades, the pads go out, and the unit restarts about 100 ms after Power is released (it is not held). The stage stamps are the `log` lines after it. With Power still UP the unit restarts about 100 ms after the stages; to try keys and pads against the lock (QA-GLB-102) hold Power by hand instead of running this step"),
        CMD("power off dry", expect=["power: off dry"], timeout=10, show=True, note="QA-GLB-113 / 116: the unit restarts after Power is released (the first run of slice 1 is this, with NO button held)"),
        CMD("log 30", expect=["power: A quiet \\+0 ms", "power: A2 mute \\+\\d+ ms", "power: B card \\+\\d+ ms", "power: C unmount \\+\\d+ ms", "power: D usb \\+\\d+ ms"], timeout=3, soft=True, show=True,
            note="QA-GLB-113 / 116: the stage stamps in order (ms from stage A) up to USB off, read at once: the console ends at D (about 250 ms after the command), so this is a race that a slow host loses; the lines are gone after the restart (the log is cleared at boot). Soft: a lost race is not a failure of the shutdown"),
    ],
    needs=[])

# OS-POWER P2 (QA-GLB-117, QA-GLB-103): the real off and the wake. The off is a command; the wake is a PRESS of the physical switch, which only a person can make (`power press` feeds the hold, not the off
# loop, which reads the pin). Run `qa_power` (with `power off dry`, no button held) first; this one is the second run. THE LAST STEP TAKES THE UNIT DARK: the console ends at USB off and nothing after it can be read.
add("qa_power_off", "OS-POWER P2: the real off (VLPR, a polled wake): `power off` with no button held; the unit stays dark, a press boots it. The draw and the press are by hand",
    [SAY("OS-POWER P2, rows QA-GLB-117 and QA-GLB-103. Run qa_power (`power off dry` ends in a restart) FIRST. The unit is idle, NO BUTTON held. Have an inline USB power meter ready (M3) and, for the wake rows, a gate-checked "
         "build under test as a .ptf in Firmware/ (then run the wake again with no card). The register facts of the VLPR entry are confirmed on NXP's documents (spec 4.7) but this is their first run on a unit: if the unit restarts instead of staying dark, `power` after the boot "
         "names the step (`power: last off RESTARTED at step: ...`): send that line")] + clear_overlays() + to_pattern() + [
        RUN("_stop"),
        CMD("power", expect=["power: hold Armed", "power: shutdown idle"], within=10, poll=1.0, show=True, note="Power is released and the hold is armed: the off is allowed (it answers ERR power: Power is down otherwise)"),
        CMD("power off", expect=["power: off: the stages 0 to F, USB off, the lights out, then the VLPR off after Power is released"], timeout=10, show=True,
            note="QA-GLB-117: the stages run (about 0.4 s), USB goes (this console ends), the screen fades, the pads go out; the unit then goes dark and silent and stays so. A build without the VLPR off says `then a restart` here"),
        SAY("NOW, BY HAND (QA-GLB-117, M3): the unit is dark and silent and stays so; read the USB meter for a minute and write the figure down. A restart within a second means a clock or mode step failed (see the first SAY). "
            "Then PRESS Power for 1 s and RELEASE it (QA-GLB-103; the unit boots at the release while kWakeAfterRelease is true, the default): the unit boots (logo, project); `diag power` then says `power: last off ended by a press (the wake)`. "
            "Repeat with Power held for 10 s, and with a .ptf in Firmware/ and with no card: write down whether the bootloader reacts to a held button (it is up at the reset while kWakeAfterRelease is true). A brush of less than 30 ms must not boot the unit. "
            "PTA24 stays HIGH in the off (kOffCutsSdram is false in power_off.h): the draw is the figure with the SDRAM powered; a build with it true is the M3 comparison and must wake too. Then run qa_power_last: it reads the record of the shutdown and of the off"),
    ],
    needs=[])

# OS-POWER P2: the record of the last shutdown (power_record.h) is what a run with no hands reads: it lives in the noinit SRAM, so it survives the reset that ends the shutdown, where the diag log (SDRAM) and
# the console (gone at USB off) do not. Run it as the next scenario after qa_power (`power off dry`) or qa_power_off (a real off and a wake), once the unit has booted again.
add("qa_power_last", "OS-POWER P2: the record of the last shutdown: the stage reached, the ms of each stage, the notes, the result of the off. Run it alone AFTER A BOOT that follows qa_power / qa_power_off (not straight after qa_power: the unit is still restarting)",
    [SAY("OS-POWER P2, rows QA-GLB-113 and QA-GLB-117. Run this as its own run AFTER A BOOT that follows qa_power (the dry run restarts the unit: straight after it the unit is still restarting and the run fails) or after qa_power_off and the wake: it reads the record that the shutdown left in the "
         "noinit SRAM. Nothing is changed on the unit. An empty record (`last shutdown` missing) after a restart means the SRAM did not hold through the reset, or the unit was powered up from cold"),
         CMD("version", within=90, poll=2.0, note="precondition: the unit answers on the console (it has booted again after the restart of qa_power)"), W(3.0)] + [
        CMD("power", expect=["power: last shutdown (dry|off), reached (A quiet|A2 mute|B card|C unmount|D usb|E lights|F off|done)",
                             "power: ms from A: A quiet 0, A2 mute \\d+, B card \\d+, C unmount \\d+, D usb \\d+, E lights \\d+, F off \\d+, done \\d+"], within=30, poll=1.0, show=True,
            note="QA-GLB-113: the stages of the last shutdown with the ms from A at which each was entered (the whole run, to the call of enter_off), read with no hands. `reached done` is a run that got to the restart or the off"),
        CMD("power", expect=["power: last off (ended by a press \\(the wake\\)|RESTARTED at step: .+|began and left no result.*)"], soft=True, show=True,
            note="QA-GLB-117 / 103: after a real off only. `ended by a press` is the wake that booted the unit; `RESTARTED at step` names the clock or mode step that failed (a restart came instead of an off); `began and left no result` is a hang or a cable pull. Nothing after a dry run: soft"),
    ],
    needs=[])

# ======================================================================================================================================
# OS-FINALQA FQA-5a: qa_perform (Perform: slots page and DJ page) and qa_record (Record: SOURCES, RADIO, MONITOR, take, SAVE WAV)
# ======================================================================================================================================
DJ_SNAPS = set(range(100, 107)) | set(range(109, 128)) | {129, 133}  # the snaps of sim/scenarios/qa/qa_perform_dj.txt (key row only: the rest of the page depends on the project)


def prf_shot(n, note=None, snap=True):
    cmp_ = CMP("qa/qa_perform_dj", "qa_prf_%03d" % n) if snap and n in DJ_SNAPS else None
    return SHOT("qa_prf_%03d" % n, compare=cmp_, soft=True, note=note)


def perform_page(n):
    """The Perform screen on page n (0 slots, 1 DJ): the button toggles the page, so a page left from a former run is turned back."""
    return [K("performance", gap=0.5), ST({"screen": "PERFORM"}, within=3),
            K("performance", gap=0.5, if_state={"page": str(1 - n)}), ST({"screen": "PERFORM", "page": str(n)}, within=3)]


def play_song_stay():
    """The whole song (F2 PLAY SNG) from the Song screen, and the scenario stays on Song: the _play_song macro ends on Pattern,
    and leaving Perform for Pattern ends the Perform FX (only Master and Song keep them; measured on the unit)."""
    return [K("song", gap=0.5, note="Song, not Pattern: Perform keeps its FX on Song only"), ST({"screen": "SONG"}, within=3),
            K("f2", gap=0.2), ST({"playing": "1", "seq": "1"}, within=8, poll=0.3)]


def sounding(snap):
    return MIX({"min_tracks": 4, "channels": {"MST L": {"min_rms_db": -90}}, "within": 6, "snap": snap})


def silent_vs(base, snap, note=None):
    return MIX({"drop": {"channel": "MST L", "field": "hold", "from": base, "min_db": 12}, "within": 8, "snap": snap}, note=note)


def back_vs(silent, note=None):
    return MIX({"rise": {"channel": "MST L", "field": "hold", "from": silent, "min_db": 12}, "within": 8}, note=note)


PRF_PADS_UP = [PAD(p, "up") for p in (14, 16, 17, 24, 25, 26, 27, 31, 12, 13)]
ALL12 = list(range(12))

prf = start_on_pattern() + [
    SAY("OS-FINALQA FQA-5a: the Perform rows of docs/qa/catalogue/perform.md that keys, pads and the console reach. The slots page (QA-PRF-001, 015, 020, 021, 023), then the DJ page with the song playing (QA-PRF-100..140). "
        "By ear, by export compare and by pad feel are `manual:` in the catalogue. Needs t-v16. The AFFECTED tracks and the DJ groups are runtime state: a run on a unit that was not rebooted starts from the state the former run left (this one ends with the groups at THRU)"),
] + perform_page(0) + [
    K("f8", gap=0.3, note="RESET: every slot off"),
    SAY("QA-PRF-001: Shift held on Perform shows STORE 1..3 on F1..F3 and nothing on F4..F8; Shift + F7 does not fall through to NEXT PAT"),
    K("+shift", gap=0.4), ST({"shift": "1"}, within=2),
    SHOT("qa_prf_001", compare=CMP("qa/qa_perform", "qa_prf_001"), soft=True, note="QA-PRF-001: the key row of the Shift layer against the simulator (soft)"),
    K("f7", gap=0.4), ST({"screen": "PERFORM"}, ne={"msg": "~^NOT_PLAYING$"}, within=2, note="QA-PRF-001: Shift + F7 does nothing (the plain F7 would say NOT PLAYING)"),
    K("-shift", gap=0.3),
    SAY("QA-PRF-015: EDIT (Rec), slot 1 holds VOLUME by default: Down wraps to OVERDRV, Down again to BITDEPTH; both on the list in full and not dimmed"),
    K("rec", gap=0.4), K("down", gap=0.4),
    SHOT("qa_prf_015_overdrv", note="QA-PRF-015: slot 1 reads OVERDRV in full, drawn like the other names"),
    K("down", gap=0.4),
    SHOT("qa_prf_015_bitdepth", note="QA-PRF-015: slot 1 reads BITDEPTH in full"),
    K("up", repeat=2, gap=0.4, note="back to VOLUME"), K("rec", gap=0.4, note="EDIT off"),
    SAY("QA-PRF-020 / 021 / 023: with the song playing, the AFFECTED tracks (Shift + the top row of pads) and VOLUME -100 (slot 1, row 1, pad 12, the encoder to the end, stored with Rec / EDIT on: a live value ends with the pad) silence the master; the stored silence survives Stop and Play; RESET brings it back; PANNING +100 (pad 13) lowers the left"),
    ] + play_song_stay() + [W(3.0), sounding("base")] + perform_page(0) + layer_taps(ALL12, "T1..T12 join the performance layer (AFFECTED)") + [
    K("rec", gap=0.4, note="EDIT on: the encoder STORES the value in the row, so that it stays latched after the pad's release (QA-PRF-025: a live value ends with the pad, `PerformBank::pad_up`)"),
    PAD(12, "down", note="slot 1 (VOLUME), row 1 held"), ENC(-64), ENC(-64, note="-100: the end of the range"),
    silent_vs("base", "vol_cut", "QA-PRF-020: the master falls by at least 12 dB with VOLUME at -100 on every track"),
    SHOT("qa_prf_020", note="QA-PRF-020: the slot's value reads -100 (picture only)"),
    PAD(12, "up"),
    K("rec", gap=0.4, note="EDIT off: the stored -100 stays (the header reads `latched until another row`)"),
    RUN("_stop"),
] + play_song_stay() + [W(3.0),
    silent_vs("base", "vol_cut2", "QA-PRF-023: still silent after Stop and Play (the stored value is latched; the performance layer is its own, the tracker's volume reset does not undo it)"),
] + perform_page(0) + [
    K("f8", gap=0.4, note="RESET"),
    back_vs("vol_cut", "QA-PRF-023: RESET brings the master back"),
    PAD(13, "down", note="slot 2 (PANNING), row 1 held"), ENC(64), ENC(64, note="+100"),
    MIX({"drop": {"channel": "MST L", "field": "hold", "from": "base", "min_db": 3}, "within": 8}, soft=True, note="QA-PRF-021: (soft: the left falls when every track is moved to the right; the picture and the ear are the check)"),
    SHOT("qa_prf_021", note="QA-PRF-021: the PANNING slot's value (picture only)"),
    PAD(13, "up"), K("f8", gap=0.4, note="RESET"),
    # ---- the DJ page, the song playing
    SAY("QA-PRF-100: the DJ page opens (page 1): PAGE DJ, CURVE SMOOTH, twelve THRU boxes, the key row"),
] + perform_page(1) + [
    prf_shot(100),
    SAY("QA-PRF-103: F3 toggles CURVE SMOOTH / CUT"),
    K("f3", gap=0.4), ST({"msg": "~^CURVE:_CUT$"}, within=2), prf_shot(103, "CUT"),
    K("f3", gap=0.4), ST({"msg": "~^CURVE:_SMOOTH$"}, within=2),
    SAY("QA-PRF-104: Shift + a pad of row 1 cycles a track THRU, A, B, THRU; then all twelve tracks go to A"),
    K("+shift", gap=0.3), PAD(0), ST({"msg": "~^TRACK_1:_GROUP_A$"}, within=2), PAD(0), ST({"msg": "~^TRACK_1:_GROUP_B$"}, within=2), PAD(0), ST({"msg": "~^TRACK_1:_THRU$"}, within=2), K("-shift", gap=0.3),
] + layer_taps(ALL12, "QA-PRF-104: every track to group A") + [
    ST({"msg": "~^TRACK_12:_GROUP_A$"}, within=2), prf_shot(104),
    SAY("QA-PRF-101 / 102: the encoder moves the fader (six detents left, Shift = one position); pad 12 of row 4 jumps all B, which silences group A; pad 1 jumps back"),
    ENC(-1, repeat=6, gap=0.15), prf_shot(101), K("+shift", gap=0.2), ENC(1), K("-shift", gap=0.3),
    PAD(47, note="row 4, last pad: the fader all B"), silent_vs("base", "fader_b", "QA-PRF-102: group A (every track) is silent with the fader all B"),
    prf_shot(102),
    PAD(36, note="row 4, first pad: all A"), back_vs("fader_b", "QA-PRF-102: and back at full with the fader all A"),
    SAY("QA-PRF-105: RESET centres the fader, the groups stay (the master is at the centre's -3 dB, not silent)"),
    PAD(47), K("f8", gap=0.4), ST({"msg": "~^DJ_RESET$"}, within=2), prf_shot(105),
    sounding("centre"),
    SAY("QA-PRF-125 / 123 / 124: CUT A (row 2, pad 6 = pad 17) silences group A while held; REP 1/16 A (pad 3 = 14) and ECHO A (pad 5 = 16) light their pads"),
    PAD(17, "down"), silent_vs("centre", "cut_a", "QA-PRF-125: group A is silent while CUT is down"),
    LEDS({"lit": [17], "within": 3}, soft=True, note="QA-PRF-125: the pad is bright"), prf_shot(125),
    PAD(17, "up"), back_vs("cut_a", "QA-PRF-125: and back at the release"),
    PAD(14, "down"), LEDS({"lit": [14], "within": 3}, soft=True, note="QA-PRF-123: REP 1/16 of A lit while down"), prf_shot(123), PAD(14, "up"),
    LEDS({"unlit": [14], "within": 3}, soft=True, note="QA-PRF-123: dim at the release"),
    PAD(16, "down"), LEDS({"lit": [16], "within": 3}, soft=True, note="QA-PRF-124: ECHO of A lit while down (the strip may say DELAY LEVEL IS 0: then the master delay level is 0)"), prf_shot(124), PAD(16, "up"),
    SAY("QA-PRF-126 / 140: LATCH (F6) keeps a row 2 pad, RESET ends it and keeps LATCH"),
    K("f6", gap=0.4), ST({"msg": "~^LATCH_ON:_FILTER_AND_PAD_EFFECTS_STAY$"}, within=2, note="QA-PRF-140"),
    PAD(14, "down"), PAD(14, "up"), LEDS({"lit": [14], "within": 3}, soft=True, note="QA-PRF-126: the REP pad stays lit after the release"), prf_shot(126),
    K("f8", gap=0.4), LEDS({"unlit": [14], "within": 3}, soft=True, note="QA-PRF-126: RESET ends it"),
    SAY("QA-PRF-115 / 116 / 117: KILL pads (row 3, pads 1..3 = 24..26) light while down; with LATCH a tap keeps MID, a second tap ends it, RESET ends a kill"),
    PAD(24, "down"), PAD(26, "down"), LEDS({"lit": [24, 26], "within": 3}, soft=True, note="QA-PRF-115: LOW and HI kills lit"), prf_shot(115),
    PAD(24, "up"), PAD(26, "up"),
    PAD(25), LEDS({"lit": [25], "within": 3}, soft=True, note="QA-PRF-116: MID stays after the tap (LATCH on)"), prf_shot(116),
    PAD(25), LEDS({"unlit": [25], "within": 3}, soft=True, note="QA-PRF-116: the second tap ends it"),
    PAD(25), K("f8", gap=0.4), LEDS({"unlit": [25], "within": 3}, soft=True, note="QA-PRF-117: RESET ends a latched kill"), prf_shot(117),
    SAY("QA-PRF-111 / 112 / 113: the filter, LATCH on: F4 held + the encoder five detents left, let go of F4, the filter stays; RESET clears it; the FILT pad (pad 4 of row 3 = 27) is the filter too"),
    K("+f4", gap=0.3), ENC(-1, repeat=5, gap=0.2), K("-f4", gap=0.4), prf_shot(111, "FILTER LP 50 LATCH"),
    K("f8", gap=0.4), prf_shot(112, "FILTER OFF, LATCH still on"),
    PAD(27, "down"), ENC(-1, repeat=6, gap=0.2), LEDS({"lit": [27], "within": 3}, soft=True, note="QA-PRF-113: the FILT pad is bright while the filter is away from 0"), prf_shot(113),
    PAD(27, "up"),
    K("f6", gap=0.4), ST({"msg": "~^LATCH_OFF$"}, within=2, note="LATCH off again"),
    SAY("QA-PRF-109 / 110: LATCH off: F4 held + the encoder three detents right reads HP 30, the release springs back to OFF"),
    K("+f4", gap=0.3), ENC(1, repeat=3, gap=0.2), prf_shot(109, "FILTER HP 30"), K("-f4", gap=0.4), prf_shot(110, "FILTER OFF"),
    SAY("QA-PRF-118 / 119 / 120: the pattern row and the bank keys; a tap on a pad of row 1 queues that pattern, a second tap cancels"),
    prf_shot(118, "NOW PAT n, NEXT -, BANK 1"),
    K("f2", gap=0.4), prf_shot(120, "BANK 2, patterns 13..24"), K("f1", gap=0.4),
    PAD(1, note="QA-PRF-119: pattern 2 of bank 1 queued"), W(0.6), prf_shot(119, "NEXT PAT 2 and the pad blinks"), PAD(1, note="the second tap cancels"), W(0.6),
    SAY("QA-PRF-121 / 122: Right held nudges the tempo while playing (the tempo of `state` moves, then returns); Shift + Right / Left move the project tempo by 0.1 BPM and back"),
    ST({"screen": "PERFORM"}, keep={"as": "BPM", "field": "tempo", "re": "^(\\d+)$"}),
    K("+right", gap=0.3), ST({"screen": "PERFORM"}, ne={"tempo": "{{BPM}}"}, within=3, soft=True, note="QA-PRF-121: the nudged tempo differs while Right is down (soft: it follows the sequencer's publish)"), prf_shot(121, "NUDGE +4%"),
    K("-right", gap=0.3), ST({"tempo": "{{BPM}}"}, within=4, soft=True, note="QA-PRF-121: the tempo is back at the release"),
    K("+shift", "right", "-shift", gap=0.4), ST({"screen": "PERFORM"}, ne={"tempo": "{{BPM}}"}, within=3, soft=True, note="QA-PRF-122: +0.1 BPM"), prf_shot(122),
    K("+shift", "left", "-shift", gap=0.4), ST({"tempo": "{{BPM}}"}, within=3, soft=True, note="QA-PRF-122: and back to the first value"),
    SAY("QA-PRF-129 / 131 / 132: TRANS HP (row 3, pad 5 = 28) and TRANS ECHO (pad 7 = 30) start a transition, any TRANS pad cancels it"),
    PAD(28), W(0.6), prf_shot(129, "(TRANS HP n %) in the NEXT line"), PAD(28), ST({"msg": "~^TRANS_CANCELLED$"}, within=2, soft=True, note="QA-PRF-132: the second tap cancels (soft: a refusal such as PATTERN IS NOT IN THE SONG says why none started)"),
    PAD(30), W(0.6), SHOT("qa_prf_131", soft=True, note="QA-PRF-131: (TRANS ECHO ARMED)"), PAD(30), ST({"msg": "~^TRANS_CANCELLED$"}, within=2, soft=True),
    SAY("QA-PRF-133 / 138: SCR makes the encoder the platter, F5 again ends it; RESET with SCR on ends it. QA-PRF-136: TAPE STOP (row 3, pad 8 = 31) stops the mix while held"),
    K("f5", gap=0.4), ST({"msg": "~^(SCR_ON:_THE_ENCODER_IS_THE_PLATTER|NO_ROOM.*)$"}, within=2), ENC(5), prf_shot(133, "SCR on"),
    K("f5", gap=0.6), ST({"msg": "~^SCR_OFF$"}, within=2, soft=True),
    K("f5", gap=0.4), K("f8", gap=0.6), ST({"msg": "~^DJ_RESET$"}, within=2, note="QA-PRF-138: RESET ends the scratch"),
    PAD(31, "down"), W(1.5), silent_vs("centre", "tape", "QA-PRF-136: the mix is stopped while TAPE STOP is held"), SHOT("qa_prf_136", soft=True, note="QA-PRF-136"),
    PAD(31, "up"), back_vs("tape", "QA-PRF-136: live again at the release"),
    SAY("QA-PRF-106: the Song restarts at order 14 (patterns 8, 9, 9, 2: at least 4 tracks sound for 40 s) so that the check below does not fall on pattern 5, which holds T01 only"),
    RUN("_stop"), K("song", gap=0.5), ST({"screen": "SONG"}, within=3), K("down", repeat=14, gap=0.2, note="the cursor on order 14"), K("f2", gap=0.2),
    ST({"playing": "1", "seq": "1", "order": "14"}, within=8, poll=0.3),
] + perform_page(1) + [
    SAY("QA-PRF-106 / 114 / 117 / 127: leaving the page ends every DJ effect: fader all B, LATCH on, CUT A latched, then the slots page: the master is back, and the DJ page finds no LATCH"),
    PAD(47), K("f6", gap=0.4), PAD(17), PAD(17, "up"), silent_vs("centre", "left_cut", "QA-PRF-106: group A is cut by the fader and the latched CUT"),
] + perform_page(0) + [
    sounding("left_back"),
    SAY("QA-PRF-106 / 114 / 127: the master sounds again on the slots page (the fader, the CUT and LATCH ended)"),
] + perform_page(1) + [
    K("f6", gap=0.4), ST({"msg": "~^LATCH_ON"}, within=2, note="QA-PRF-114: LATCH was off when the page came back, so F6 turns it ON"), prf_shot(114, "FILTER OFF, the fader at the centre"),
    K("f6", gap=0.4), ST({"msg": "~^LATCH_OFF$"}, within=2),
    K("+shift", gap=0.3),
] + [PAD(p) for p in ALL12 for _ in (0, 1)] + [K("-shift", gap=0.3), ST({"msg": "~^TRACK_12:_THRU$"}, within=2, note="the groups are back to THRU (two more taps each: B, THRU)"),
    RUN("_stop"),
    CMD("cpu", expect=r"underrun 0\b"),
    K("pattern", gap=0.4), ST({"screen": "PATTERN"}, within=3),
]
add("qa_perform", "OS-FINALQA FQA-5a: Perform: the Shift key row, the FX list (OVERDRV, BITDEPTH), VOLUME / PANNING over the playing song, then the DJ page: CURVE, groups, fader pads, CUT / REP / ECHO, KILL, FILTER, LATCH, banks, tempo, TRANS, SCR, TAPE STOP, leaving the page; no file is written",
    prf, finally_=[K("-f4", "-right", "-left", "-shift", gap=0.1)] + PRF_PADS_UP + [PAD(47, "up")] + clear_overlays() + [K("f8", gap=0.3, if_state={"screen": "PERFORM"}), K("pattern", gap=0.4), RUN("_stop")])

rec2 = [K("-shift", gap=0.2)] + clear_overlays() + to_pattern() + [
    RUN("_stop"),
    SAY("OS-FINALQA FQA-5a: the Record rows that keys and the console reach (QA-REC-001, 036, 037, 038, 039). The loop rows are qa_looper.json, USB is qa_usbrec.json, the Shift tuning and the page turn are qa_shiftrow.json. "
        "QA-REC-039 WRITES the card: one Samples/REC_NNN.wav. Needs the line input idle (a take of silence is fine)"),
    CMD("mon off", expect=r"monitor gain"),
    K("samplerec", gap=0.5), ST({"screen": "RECORD"}, within=3),
    K("samplerec", gap=0.4, if_state={"page": "1"}), K("samplerec", gap=0.4, if_state={"page": "2"}), ST({"page": "0"}, within=2),
    SAY("QA-REC-001: Shift held on SOURCES: all eight keys read '-'; on RADIO F5 CH-10 and F6 CH+10"),
    K("+shift", gap=0.4), ST({"shift": "1"}, within=2),
    SHOT("qa_rec_001", compare=CMP("qa/qa_record", "qa_rec_001"), soft=True, note="QA-REC-001: SOURCES, the Shift key row against the simulator"),
    K("-shift", gap=0.3),
    K("samplerec", gap=0.3), K("samplerec", gap=0.3), ST({"page": "2"}, within=2),
    K("+shift", gap=0.4),
    SHOT("qa_rec_001_radio", compare=CMP("qa/qa_record", "qa_rec_001_radio"), soft=True, note="QA-REC-001: RADIO, the Shift key row against the simulator"),
    K("-shift", gap=0.3),
    SAY("QA-REC-038: on RADIO F6 asks the chip for the next station (the radio report names the step; needs the chip, an antenna for a real station)"),
    CMD("radio", show=True, soft=True, note="the radio before"),
    K("f6", gap=1.0), CMD("radio", expect=r"(?i)seek", soft=True, show=True, within=4, poll=0.5, note="QA-REC-038: the driver reports a seek (soft: the chip may answer too fast to catch)"),
    SHOT("qa_rec_038", soft=True, note="QA-REC-038: the radio status reads SEEKING or LISTENING"),
    K("f5", gap=1.0), CMD("radio", show=True, soft=True),
    K("samplerec", gap=0.3), ST({"page": "0"}, within=2),
    SAY("QA-REC-037: F2 MONITOR flips ON / OFF; the input strip's gain follows (`mon` reads it back)"),
    K("f2", gap=0.5), CMD("mon", expect=r"unity\) \d+, routes MAIN \+ CUE", soft=True, within=3, poll=0.5, note="QA-REC-037: MONITOR ON (the unit prints the input strip's graph gain, 65536 in both states: the F2 state itself is in the shots)"),
    SHOT("qa_rec_037_on", soft=True, note="QA-REC-037: MONITOR reads ON"),
    K("f2", gap=0.5), CMD("mon", expect=r"unity\) \d+, routes MAIN \+ CUE; mon on = \d+, mon off = 0", soft=True, within=3, poll=0.5, note="QA-REC-037: MONITOR OFF (same readout line; the gain stays 65536 on the unit, see the ON step)"),
    SAY("QA-REC-039: F7 with no take says so; then a take of two seconds (F1, F1), F7 writes Samples/REC_NNN.wav, the strip says SAVE REC_NNN DONE and the file is listed"),
    K("f7", gap=0.4), ST({"msg": "~^NO_TAKE_TO_SAVE:_F1_RECORDS_ONE$"}, within=2, note="QA-REC-039: no take"),
    SHOT("qa_rec_036_idle", soft=True, note="QA-REC-036: MEM in the top bar before the take"),
    K("f1", gap=0.5), W(2.0), SHOT("qa_rec_036_taking", soft=True, note="QA-REC-036: MEM reads 100 % while the take records (the take reserves the free arena)"),
    K("f1", gap=1.0), SHOT("qa_rec_036_review", soft=True, note="QA-REC-036: MEM is the share in use plus the take's size"),
    K("f7", gap=0.6), ST({"msg": "~^SAVE_REC_[0-9]+_DONE$"}, within=45, poll=1.0, keep={"as": "WAV", "field": "msg", "re": "^SAVE_(REC_[0-9]+)_DONE$"}, note="QA-REC-039: the strip names the file (a 1.2 MB take is at SAVE REC 001 50% after 6 s and DONE inside 40 s on the unit)"),
    CMD("ls Samples", expect=r"(?i)REC_[0-9]+\.wav", soft=True, within=4, poll=1.0, note="QA-REC-039: a REC_NNN.wav is on the card (soft: `ls` is the console's own listing; the cmd line expands {{NAME}}, an expect does not, so the name the strip gave is not repeated here)"),
    K("f4", gap=0.6), SHOT("qa_rec_036_discard", soft=True, note="QA-REC-036: after F4 DISCARD MEM is back at the level from before the take"),
    SAY("QA-REC-045..049 (E-SREC-11, START ON LEVEL): START ON to LEVEL adds THRESHOLD and PRE-ROLL; F1 listens (WAITING FOR LEVEL) with the transport stopped; F1 again cancels with no take. THRESHOLD is set to -6 DB first so that a quiet line input does not start the take; the level start itself (a tone at the line input) is a hand check. Runtime settings only: START ON is put back to BAR at the end"),
    # the cursor to the top row (six Ups), then three Downs: the third row, START ON (one key a step: the file is kept as the unit run left it)
    *[K("up", gap=0.2) for _ in range(6)], *[K("down", gap=0.2) for _ in range(3)],
    ENC(2, note="START ON: BAR, PATTERN, LEVEL"), K("down", gap=0.3),
    SHOT("qa_rec_045", soft=True, note="QA-REC-045: START ON LEVEL, THRESHOLD -30 DB and PRE-ROLL 50 MS under it, the cursor on THRESHOLD"),
    ENC(2, note="THRESHOLD +6 DB: -24 DB"), K("down", gap=0.3), ENC(1, note="PRE-ROLL: 100 MS"),
    SHOT("qa_rec_046", soft=True, note="QA-REC-046: THRESHOLD -24 DB, PRE-ROLL 100 MS"),
    K("up", gap=0.3), ENC(20, note="THRESHOLD to its top, -6 DB"), K("f1", gap=0.6), W(1.0),
    SHOT("qa_rec_047", soft=True, note="QA-REC-047/049: the card says WAITING FOR LEVEL, the clock is 0.0 S, the transport is stopped"),
    K("f1", gap=0.6),
    SHOT("qa_rec_048", soft=True, note="QA-REC-048: F1 again cancels: READY, no take"),
    K("up", gap=0.2), K("delete", gap=0.3, note="START ON back to BAR (its Delete), the two rows go"),
    K("pattern", gap=0.4), ST({"screen": "PATTERN"}, within=3),
]
add("qa_record", "OS-FINALQA FQA-5a: Record: the Shift key rows (SOURCES, RADIO), the radio seek, F2 MONITOR with the input gain read back, MEM around a take, F7 SAVE WAV (writes Samples/REC_NNN.wav); the loop is qa_looper, USB is qa_usbrec",
    rec2, finally_=[K("-shift", gap=0.2), CMD("mon off")] + clear_overlays() + [K("pattern", gap=0.4)])

# ======================================================================================================================================
# OS-FINALQA FQA-5b: qa_wave (Wave: the Shift list, the point edits, CHOP, the WAVETABLE and GRANULAR views) and qa_sample_edit (Sample edit: the edits, AMP, AMIGA, SAVE WAV, the previews)
# ======================================================================================================================================
def wave_page(n):
    """The Wave screen on page n (0 WAVE, 1 CHOP): its button toggles the page, so a page left from a former run is turned to the right one."""
    return [K("sampleplay", gap=0.5), ST({"screen": "WAVE"}, within=3),
            K("sampleplay", gap=0.4, if_state={"page": str(1 - n)}), ST({"screen": "WAVE", "page": str(n)}, within=3)]


def sample_pick_1():
    """Instrument 1 picked on the Sample edit screen (a pad), so that the Wave and Sample edit screens show its sample."""
    return [K("sampleedit", gap=0.5), ST({"screen": "SAMPLE_EDIT"}, within=3),
            PAD(0, note="a pad on the Sample edit screen picks the instrument: 1"), ST({"instrument": "1"}, within=2),
            CMD("inst 1", expect=r"inst 01: mode \d+ start \d+ len \d+", note="the sample of instrument 1 is bound (else this scenario has nothing to edit)")]


wv = start_on_pattern() + sample_pick_1() + wave_page(0) + [
    SAY("OS-FINALQA FQA-5b: the Wave rows of docs/qa/catalogue/wave.md that keys and the console reach (QA-WAV-009..023; 008 needs a /WTABLE file: qa_instrument.json). Instrument 1 of t-v16 is edited and its mode changed; the finally run reloads t-v16. "
        "The shots are the check of the screen's words; the waveform lane is drawn on the unit only. ZC is a view switch: a run starts with it ON after a boot, a former run may leave it OFF"),
    shift_pick("f1"), ST({"overlay": "PLAY_MODE"}, within=2, note="QA-WAV-009: Shift + F1 opens the list"),
    SHOT("qa_wav_009", soft=True, note="QA-WAV-009: the list's key row: SELECT on F1, BACK on F8; the dialog line says the same"),
    K("f8", gap=0.4), ST({"overlay": "NONE"}, within=2, note="QA-WAV-009: F8 BACK closes it, nothing changed"),
    SAY("QA-WAV-010 / 011 / 012: F3 picks LOOP END, F4 picks END (the highlighted row), F5 turns ZC off (the header reads ZC OFF)"),
    K("f3", gap=0.4), SHOT("qa_wav_010", soft=True, note="QA-WAV-010: the LOOP END row is highlighted"),
    K("f4", gap=0.4), SHOT("qa_wav_011", soft=True, note="QA-WAV-011: the END row is highlighted"),
    K("f5", gap=0.4), SHOT("qa_wav_012", soft=True, note="QA-WAV-012: the header reads ZC OFF after the press (ON before it)"),
    SAY("QA-WAV-013: hold F6 PREVIEW: the instrument sounds while the key is down (the voice is checked here, the sound by ear)"),
    K("+f6", gap=0.2), VOX({"min_in_use": 1}, soft=True, note="QA-WAV-013: a voice while F6 is down (by ear: the sound of the instrument)"), K("-f6", gap=0.4),
    SAY("QA-WAV-014: F7 ZOOM- widens the window, Up zooms in again around the picked point"),
    K("f7", gap=0.5), SHOT("qa_wav_014", soft=True, note="QA-WAV-014: the window is wider (the lane is drawn on the unit)"),
    K("up", gap=0.5, note="Up zooms in around the picked point"),
    SAY("QA-WAV-015 / 016: END is picked (F4): the encoder -5 moves it 5 frames (ZC off): END 195 of 200 in the simulator's fixture; F8 UNDO puts it back, Shift + F8 redoes it"),
    K("f4", gap=0.3), ENC(-5), W(0.3), SHOT("qa_wav_015", soft=True, note="QA-WAV-015: END 5 frames before the edit; the loop points follow"),
    K("f8", gap=0.5), SHOT("qa_wav_016", soft=True, note="QA-WAV-016: the screen as before the edit"),
    K("+shift", "f8", "-shift", gap=0.5, note="QA-WAV-016: Shift + F8 redoes the point edit"),
    K("f8", gap=0.5, note="(and UNDO again: the point is back where it was)"),
] + wave_page(1) + [
    SAY("QA-WAV-017 / 019 / 020 / 021: the CHOP page: F2 EVEN cuts 16 equal slices (BEAT SLICE), F4 ADD puts one at the middle, F5 DEL removes the selected one, F3 ZC snaps the slice starts"),
    K("f2", gap=0.5), W(0.5), SHOT("qa_wav_017", soft=True, note="QA-WAV-017: MODE BEAT SLICE, SLICES 16, 16 lit pads"),
    K("f4", gap=0.4), SHOT("qa_wav_019", soft=True, note="QA-WAV-019: SLICES 17, the new slice selected"),
    K("f5", gap=0.4), SHOT("qa_wav_020", soft=True, note="QA-WAV-020: SLICES 16 again"),
    K("f3", gap=0.4), SHOT("qa_wav_021", soft=True, note="QA-WAV-021: the header's ZC switch (the same as the WAVE page's F5)"),
    K("f3", gap=0.4, note="(ZC back)"),
    SAY("QA-WAV-018: F1 AUTO runs the slicer as a job: the strip shows the slicing, then one slice per hit (the simulator has no hits; a real sample has them); one F8 UNDO takes it back"),
    K("f1", gap=0.5), ST({"msg": "~^SLICE_"}, within=4, soft=True, note="QA-WAV-018: the job's strip message"),
    W(3.0), SHOT("qa_wav_018", soft=True, note="QA-WAV-018: one slice per detected hit"),
    K("f8", gap=0.5, note="QA-WAV-018: one undo unit: the 16 EVEN slices come back"),
    SAY("QA-WAV-022: the WAVETABLE view: Shift + F1, the list's cursor clamps at the top (up 8), six Down are WAVETABLE, F1 picks it"),
    shift_pick("f1"), K("up", repeat=8, gap=0.15), K("down", repeat=6, gap=0.15), K("f1", gap=0.6), ST({"overlay": "NONE"}, within=3),
    SHOT("qa_wav_022", soft=True, note="QA-WAV-022: the keys POSITION WINDOW - - - PREVIEW ZOOM- UNDO"),
    K("f7", gap=0.5, note="QA-WAV-022: F7 ZOOM- (wavetable view)"), K("f8", gap=0.5, note="QA-WAV-022: F8 UNDO"),
    SAY("QA-WAV-023: the GRANULAR view (one Down from WAVETABLE): F2 LENGTH picked, the encoder -1 edits it, F8 UNDO undoes it; F5 does nothing"),
    shift_pick("f1"), K("down", gap=0.15), K("f1", gap=0.6), ST({"overlay": "NONE"}, within=3),
    K("f5", gap=0.4, note="QA-WAV-023: F5 is blank"),
    SHOT("qa_wav_023", soft=True, note="QA-WAV-023: the keys POSITION LENGTH SHAPE LOOP - PREVIEW ZOOM- UNDO"),
    K("f2", gap=0.4), ENC(-1), W(0.3), K("f8", gap=0.5, note="QA-WAV-023: the encoder's edit is undone"),
]
add("qa_wave", "OS-FINALQA FQA-5b: Wave, the unit-pending rows QA-WAV-009..023 (the Shift list, the point edits, CHOP EVEN / ADD / DEL / ZC / AUTO, the WAVETABLE and GRANULAR views, F6 preview held, zoom): instrument 1 of t-v16; instrument 1's mode is changed and the finally run reloads t-v16", wv,
    finally_=[K("-shift", gap=0.2), K("-f6", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_load_tv16"), K("pattern", gap=0.4), RUN("_stop")])

sed = start_on_pattern() + sample_pick_1() + [
    SAY("OS-FINALQA FQA-5b: the Sample edit rows of docs/qa/catalogue/sample_edit.md that keys and the console reach (QA-SED-002, 004..016; 003 is sample_edit_dc.json). Instrument 1 of t-v16 is edited and undone; the finally run reloads t-v16. "
        "`inst 1` is printed (soft) after each process: its len and sum are compared by eye, the strip words are checked"),
    SAY("QA-SED-016: Delete clears the selection (ALL), Right picks END, the encoder -5 moves it 5 frames"),
    K("delete", gap=0.4), K("right", gap=0.3), ENC(-5), W(0.3), SHOT("qa_sed_016", soft=True, note="QA-SED-016: the END edge is 5 frames nearer the start"),
    SAY("QA-SED-005: F1 CROP to the selection (the END edge moved): the sample keeps the selected frames, one undo unit"),
    CMD("inst 1", expect=r"@@never@@", soft=True, note="(before the crop: prints the unit's `inst 1` line: its len)"),
    K("f1", gap=0.5), W(4.0), ST({}, ne={"msg": "~^NO_SAMPLE_IN_THIS_INSTRUMENT$"}, within=2),
    CMD("inst 1", expect=r"@@never@@", soft=True, note="(after the crop: the len is the selection's)"),
    SHOT("qa_sed_005", soft=True, note="QA-SED-005: the crop's result: the selection stays on the kept frames"),
    K("f8", gap=0.5), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, soft=True, note="QA-SED-005: UNDO 1 brings the frames back"),
    SAY("QA-SED-002 / 004: F2 NORMALISE (all of the sample: Delete cleared the selection); a second press says NOTHING TO NORMALISE and makes no step; F8 UNDO takes back the first"),
    K("delete", gap=0.4), K("f2", gap=0.5), W(4.0),
    CMD("inst 1", expect=r"@@never@@", soft=True, note="(after NORMALISE: the sum differs from the first print)"),
    K("f2", gap=0.5), ST({"msg": "~^NOTHING_TO_NORMALISE$"}, within=4, note="QA-SED-002: nothing to do the second time"),
    SHOT("qa_sed_002", soft=True, note="QA-SED-002: the peak is at full scale"),
    K("f8", gap=0.5), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, soft=True, note="QA-SED-004: UNDO brings the original back (the sum of the first print)"),
    SAY("QA-SED-006: F3 REVERSE of all of the sample (no selection), then UNDO"),
    K("f3", gap=0.5), W(4.0), ST({}, ne={"msg": "~^NO_SAMPLE_IN_THIS_INSTRUMENT$"}, within=2),
    SHOT("qa_sed_006", soft=True, note="QA-SED-006: the sample runs backwards; the strip says EDIT REVERSE DONE"),
    K("f8", gap=0.5), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, soft=True),
    SAY("QA-SED-007 / 008 / 009: F4 FADE IN, F5 FADE OUT, F6 AMPLIFY (AMP 200 %), each one undo unit, undone after its shot"),
    K("f4", gap=0.5), W(4.0), SHOT("qa_sed_007", soft=True, note="QA-SED-007: the start ramps up from silence"),
    K("f8", gap=0.5), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, soft=True),
    K("f5", gap=0.5), W(4.0), SHOT("qa_sed_008", soft=True, note="QA-SED-008: the end ramps down to silence"),
    K("f8", gap=0.5), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, soft=True),
    K("f6", gap=0.5), W(4.0), SHOT("qa_sed_009", soft=True, note="QA-SED-009: the peaks are clipped at full scale, not wrapped"),
    K("f8", gap=0.5), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, soft=True),
    SAY("QA-SED-011: Shift + Up twice reads AMP 220 %; the sample is not touched; Shift + Down twice puts it back to 200 %"),
    K("+shift", "up", "-shift", repeat=2, gap=0.3), SHOT("qa_sed_011", soft=True, note="QA-SED-011: AMP 220 % on the line under the sample"),
    K("+shift", "down", "-shift", repeat=2, gap=0.3, note="(AMP back to 200 %)"),
    SAY("QA-SED-017: the header reads INSTRUMENT nn NAME with the hint pad / Shift+F5: change; Shift + F5 (NEXT INS) moves to the next instrument and the header and the strip's INS follow"),
    K("+shift", "f5", "-shift", gap=0.3), SHOT("qa_sed_017", soft=True, note="QA-SED-017: INSTRUMENT 02 in the header"),
    K("+shift", "f5", "-shift", gap=0.3, repeat=47, note="(48 presses in all: 48 goes round to 1)"), ST({"instrument": "1"}, within=2, note="QA-SED-017: the step wrapped from 48 to 1"),
    SAY("QA-SED-012: Shift + F1 converts the whole sample to 8287 Hz as one job (200 frames become 37), one undo unit; the strip names the Amiga engine"),
    shift_pick("f1"), W(4.0), CMD("inst 1", expect=r"@@never@@", soft=True, note="(after AMIGA 8287: the len is the converted one)"),
    ST({"msg": "~^AMIGA_8287_HZ,_ENGINE_AMIGA$"}, within=3, soft=True),
    SHOT("qa_sed_012", soft=True, note="QA-SED-012: the instrument's rate and engine follow"),
    K("f8", gap=0.5), ST({"msg": "~^(UNDONE|PEAKS_.*_DONE)$"}, within=3, soft=True, note="QA-SED-012: undo brings the audio and the fields back"),
    SAY("QA-SED-013: Shift + F3 switches the AMIGA 8-bit option: the line reads 8-BIT ON; it converts nothing by itself"),
    shift_pick("f3"), ST({"msg": "~^AMIGA:_8-BIT_ON$"}, within=2, soft=True), SHOT("qa_sed_013", soft=True, note="QA-SED-013: 8-BIT ON on the AMIGA line"),
    shift_pick("f3"), ST({"msg": "~^AMIGA:_8-BIT_OFF$"}, within=2, soft=True, note="(off again)"),
    SAY("QA-SED-014: Shift + F6 writes the sample as a WAV into Samples/ on the card (one file, prefix SMP); the strip names the result. The file itself is checked on the card by hand"),
    shift_pick("f6"), W(3.0), SHOT("qa_sed_014", soft=True, note="QA-SED-014: the strip's result (NO CARD WRITE LAYER when no card is written to)"),
    SAY("QA-SED-010 / 015: F7 PREVIEW and Shift + F7 PRV EDGE sound while the key is down (the voice is checked here; the sound by ear)"),
    K("+f7", gap=0.2), VOX({"min_in_use": 1}, soft=True, note="QA-SED-010: a voice while F7 is down (by ear: the sound)"), K("-f7", gap=0.4),
    K("+shift", gap=0.2), K("+f7", gap=0.2), VOX({"min_in_use": 1}, soft=True, note="QA-SED-015: a voice while Shift + F7 is down (by ear: from the edge)"), K("-f7", gap=0.2), K("-shift", gap=0.4),
]
add("qa_sample_edit", "OS-FINALQA FQA-5b: Sample edit, the unit-pending rows QA-SED-002, 004..016: the selection's edge, CROP, NORMALISE / UNDO, REVERSE, FADE IN / OUT, AMPLIFY, AMP %, AMIGA 8287 and its 8-bit option, SAVE WAV (writes Samples/SMP*.wav), the held previews; instrument 1 of t-v16, undone; the finally run reloads t-v16", sed,
    finally_=[K("-shift", gap=0.2), K("-f7", gap=0.2)] + clear_overlays() + [K("pattern", gap=0.4), RUN("_load_tv16"), K("pattern", gap=0.4), RUN("_stop")])

# ======================================================================================================================================
# qa_lfostep (OS-PERF-LFO-STEP, Level C, unit-gated; QA-PRF-180..189): the step FX Volume LFO 23, Panning LFO 24, Finetune LFO 33 and Position LFO 36 on twelve sampler tracks (one-shot, wavetable and granular), the CPU of each against
# the same song with no step FX. One run per project: LfoStep23, LfoStep24, LfoStep33, LfoStep36 and the control LfoStepOff (`o3pj_tool gen lfostep23|24|33|36|off`). The AFFECTED tracks are set once on Performance (QA-PRF-185).
ls = open_song("~^${PROJ}$", "load:\\s+Projects/${PROJ}", "~(?i)^(OPENING_|PROJECT_)${PROJ}",
               "OS-PERF-LFO-STEP (QA-PRF-180..189): the step FX Volume LFO 23, Panning LFO 24, Finetune LFO 33 and Position LFO 36 on twelve sampler tracks (four one-shot, four wavetable, four granular; the FX is on every note step at rate 8). "
               "BEFORE the run: build the songs (`o3pj_tool gen lfostep23 <dir>/LfoStep23/project.o3pj`, and `lfostep24`, `lfostep33`, `lfostep36` the same, and the control `lfostepoff` into `<dir>/LfoStepOff/project.o3pj`; each writes its samples/ folder beside it) "
               "and copy ONE folder into Projects/ per run: the four with the FX, then the control. Run with `--var PROJ=<folder name>`. Use the release image. By ear: the FX of the song at its rate (the FX 36 acts on the wavetable and granular tracks only); "
               "the Perform VOL LFO row over the FX 23 and its release (QA-PRF-185). The exports of the control and of a song are compared on the PC with `cmp` (QA-PRF-187). QA-PRF-188: the mean `render last` of each song against the control "
               "(no more than the same Perform rows, 1.2 pp, paired noise).") + [
    K("performance", gap=0.5), ST({"screen": "PERFORM"}, within=3),
    K("performance", gap=0.5, if_state={"page": "1"}), ST({"screen": "PERFORM", "page": "0"}, within=3),
    K("+shift", gap=0.3)] + [PAD(i, **({"note": "T1..T12 join the performance layer (AFFECTED), set once"} if i == 0 else {})) for i in range(12)] + [
    K("-shift", gap=0.3),
    RUN("_play"), W(5.0, note="settle"),
    MIX({"channels": {"MST L": {"min_rms_db": -60}}, "within": 6}, note="the song sounds"),
    CMD("cpu reset", note="the window starts"), CMD("cpu perf off")] + cpu_reads(
        8, 2.5, "L", expect=[r"underrun 0 \(", r"guard\s+shed 0 blocks"]) + [
    STAT("L", min_n=8, show=True, note="QA-PRF-188: the mean of the 8 reads of `render last` (cycles): REPORTED, compare each FX song with the control `LfoStepOff`"),
    RUN("_stop")]
add("qa_lfostep", "OS-PERF-LFO-STEP: the step FX 23, 24, 33 and 36 on twelve sampler tracks, one song per FX and the control without them (`--var PROJ=LfoStep23|24|33|36|Off`): opened from File, the AFFECTED tracks set once on Performance, "
    "`cpu reset`, Play, 8 reads of `cpu` 2.5 s apart. ASSERTS underrun 0 and guard shed 0; the mean `render last` is reported. The project must be on the card first (`o3pj_tool gen lfostep23|24|33|36|off`)",
    ls, finally_=[K("-f4", "-right", "-left", "-shift", gap=0.1), K("f8", gap=0.3, if_state={"screen": "PERFORM"}), K("pattern", gap=0.4), RUN("_stop")], vars_={"PROJ": "LfoStep23"})

for d in SCENARIOS:
    write(d)
print("wrote %d scenarios to %s" % (len(SCENARIOS), OUT))
