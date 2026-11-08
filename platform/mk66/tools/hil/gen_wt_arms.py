#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the two WT arms of the OS-CEWTENV W3a unit measurement (docs/specs/OS-CEWTENV.md 3.10 item 1) from the Dense8 fixture
(docs/architecture/cpu-budget.md section 4; `o3pj_tool gen dense8 <dir>/Dense8/project.o3pj`):

  Dense8wt6   the Dense8 song as it is, with a cutoff WT Env (loop 2, contour 37, no warps, sustain 40) on its six sampler voices
  Dense8wt12  the same song with six more sampler voices (clones on tracks 6..11, the module tracks moved to 12..13), all twelve
              with a cutoff WT Env, both warps (+7/+7)
  Dense8adsr12  the control of Dense8wt12: the same twelve voices on a cutoff ADSR. Q3 a compares "the same voices on ADSR/LFO"
  Dense8a12x, Dense8w12x, Dense8n12x  the twelve voices without the module tracks, so the block is not overrun and the guard sheds
              nothing: cutoff ADSR, WT Env with both warps, WT Env with no warps

Dense8p12x, Dense8q12x  the twelve voices with pan, cutoff and finetune all WT with both warps (W3b), and the same three on ADSR. The folders are written
under build-sdtest/ (build*/ is gitignored, nothing goes into git); copy one into Projects/ on the card per run of
platform/mk66/tools/hil/qa_cpu_wt.json (the third arm, no WT, is the Dense8 folder itself).

  python3 -I platform/mk66/tools/hil/gen_wt_arms.py [SRC] [OUT]

  SRC  a Dense8 project folder (default <repo>/build-hil/dense/Dense8). When it is not there, a small synthetic project
       (two sampler voices, one pattern) is built in memory instead, so the tool can be tested anywhere; the run says so.
  OUT  the output folder (default <repo>/build-sdtest)

The arms keep the fixture's instrument panning (o3pj_tool gives voice i a panning of 10 + 7 i, so the first voices sit well to the left with the track pan at 50: the strip's right gain is
panning / 50 x the fader, about 0.17 for voice 0 at fader 86, with every pan modulator off). That is the fixture, not a pan modulator: read a pan arm against its own control.

The projects are read and written with tools/o3project.py (the WT bytes of INST 1.6); each arm is parsed back and checked
before it is written.
"""
import os
import shutil
import struct
import sys

HIL = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HIL, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import o3project as o3  # noqa: E402  (the reference reader/writer of the native format, with the WT bytes)

STEP = o3.STEP_BYTES
EMPTY = o3.EMPTY_STEP
EMPTY_ROW = EMPTY * o3.L["max_pattern_length"]

# The arms' cutoff WT Env (slot 2): the contour, the warp byte and the sustain byte of the LFO entry.
CONTOUR = 37          # F038, a factory contour
SUST = 40             # freeze 40 %
BEND_A = 0x00         # arm a: no warps
BEND_B = 0x77         # arm b: warp X +7, warp Y +7 (wt_pack_warp(7, 7)): both divisions of the warp law run

ARM_A, ARM_B = "Dense8wt6", "Dense8wt12"
ARM_ADSR = "Dense8adsr12"
ARM_XA, ARM_XW, ARM_XN = "Dense8a12x", "Dense8w12x", "Dense8n12x"
ARM_PW, ARM_PA = "Dense8p12x", "Dense8q12x"  # W3b: pan, cutoff and finetune all WT with both warps, and the same three on ADSR
MOD_SLOTS = (1, 2, 5)  # pan, cutoff, finetune: the WT slots a plain sampler voice runs (position needs a wavetable or granular voice)
VOICES_B = 12


def f32(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def set_cutoff_wt(inst, contour, bend, sust):
    """Slot 2 (the cutoff, envelope order amp, pan, cutoff, ...) as a WT Env: loop 2, the contour in the LFO entry's
    shape, the warp and sustain bytes (INST 1.6 wt[4], wt[5])."""
    e = inst["env"][2]
    e[7] = 2  # loop: 2 = WT Env (OS-CEWTENV F1)
    e[8] = 1  # enable
    inst["lfo"][2][0] = contour
    inst["wt"][4] = bend
    inst["wt"][5] = sust


def set_cutoff_adsr(inst):
    """Slot 2 as a running ADSR (the control arms): loop 0, enabled, the synthetic project's envelope."""
    e = inst["env"][2]
    e[0] = f32(0.5)
    e[2] = 100
    e[4] = 400
    e[5] = f32(0.4)
    e[7] = 0
    e[8] = 1


def set_mods_wt(inst):
    """MOD_SLOTS as WT Envs with both warps, the amount set so that each one runs."""
    for n in MOD_SLOTS:
        e = inst["env"][n]
        e[0] = f32(0.5)
        e[7] = 2
        e[8] = 1
        inst["lfo"][n][0] = CONTOUR
        inst["wt"][2 * n] = BEND_B
        inst["wt"][2 * n + 1] = SUST


def set_mods_adsr(inst):
    """MOD_SLOTS as running ADSRs: the control of set_mods_wt."""
    for n in MOD_SLOTS:
        e = inst["env"][n]
        e[0] = f32(0.5)
        e[2] = 100
        e[4] = 400
        e[5] = f32(0.4)
        e[7] = 0
        e[8] = 1


def rebind_row(row, instr):
    """A copy of a pattern row with every non-empty step's instrument byte set to `instr` (1-based)."""
    out = bytearray(row)
    for s in range(o3.L["max_pattern_length"]):
        at = s * STEP
        if row[at:at + STEP] != EMPTY:
            out[at + 1] = instr
    return bytes(out)


def sampler_slots(p):
    return sorted(i for i, inst in p["instruments"].items() if inst["kind"] == o3.KIND_INSTRUMENT)


def clone_inst(inst, name):
    c = {k: (list(v) if isinstance(v, list) else v) for k, v in inst.items()}
    c["name"] = name[: o3.L["instrument_name_max"]]
    return c


def make_arm_a(p):
    """Six voices, cutoff WT Env, no warps: the Dense8 song with the WT slot on its sampler instruments."""
    for i in sampler_slots(p):
        set_cutoff_wt(p["instruments"][i], CONTOUR, BEND_A, SUST)


def make_arm_b(p, voices, cutoff, modules=True):
    """Twelve voices: the sampler voices cloned up to `voices` tracks, the other sounding tracks (the module tracks)
    moved behind them or, without `modules`, left out; `cutoff` sets the cutoff slot of every sampler instrument."""
    slots = sampler_slots(p)
    n = len(slots)
    if n == 0:
        raise SystemExit("no sampler instrument in the source project")
    if voices < n:
        raise SystemExit("arm b wants fewer voices than the source has")
    extra = voices - n
    # The fixture's layout: track i carries sampler instrument slots[i] (checked below on the first pattern).
    first_pat = p["patterns"][sorted(p["patterns"])[0]]
    for i in range(n):
        row = first_pat["rows"][i]
        if row == EMPTY_ROW or all(row[s * STEP + 1] != slots[i] + 1 for s in range(o3.L["max_pattern_length"])):
            raise SystemExit("track %d does not play instrument %d: not the Dense8 layout" % (i, slots[i] + 1))
    next_inst = max(p["instruments"]) + 1
    for k in range(extra):
        src = p["instruments"][slots[k % n]]
        p["instruments"][next_inst + k] = clone_inst(src, src["name"][:30] + b"w")
    others = [t for t in range(n, o3.L["tracks"]) if any(p["patterns"][pid]["rows"][t] != EMPTY_ROW for pid in p["patterns"])] if modules else []
    for pat in p["patterns"].values():
        rows = pat["rows"]
        new_rows = [EMPTY_ROW] * o3.L["tracks"]
        for i in range(n):
            new_rows[i] = rows[i]
        for k in range(extra):
            new_rows[n + k] = rebind_row(rows[k % n], next_inst + k + 1)
        for j, t in enumerate(others):
            new_rows[voices + j] = rows[t]
        pat["rows"] = new_rows
    tracks = p["tracks"]
    new_tracks = [dict(t) for t in tracks]
    for k in range(extra):
        new_tracks[n + k] = dict(tracks[k % n])
    for j, t in enumerate(others):
        new_tracks[voices + j] = dict(tracks[t])
    for t in range(voices + len(others), o3.L["tracks"]):
        new_tracks[t] = {"type": 0, "flags": 0, "vol": 86, "pan": 50, "bus": 0, "name": b""}
    p["tracks"] = new_tracks
    p["active"] = voices + len(others)
    for i in sampler_slots(p):
        cutoff(p["instruments"][i])


def synthetic_project():
    """A small project to test the tool where the Dense8 fixture is not: two sampler voices (sine and saw), one
    16-step pattern, both voices sounding on steps 0 and 8, as the Dense recipe does."""
    p = o3.new_project()
    p["name"] = b"WtArms synthetic"
    p["tempo"] = 12500
    p["deflen"] = 16
    p["active"] = 2
    p["song"] = [1]
    head = [0] * 27  # SAMPLER_HEAD ">16B8HI2I": 16 bytes, 8 u16 (start, loop_start, loop_end, end, glide, window size, grain len, grain pos), the window, cutoff and resonance bits
    head[1] = 1       # play mode: forward loop
    head[3] = 1       # filter enable
    head[6] = 30      # volume
    head[8] = 30      # delay send
    head[9] = 30      # reverb send
    head[11] = 16     # bit depth
    head[18] = 65535  # loop end
    head[19] = 65535  # end
    head[25] = f32(0.6)  # cutoff
    head[26] = f32(0.3)  # resonance
    for i, (name, path, rate, engine) in enumerate(((b"SYN1", b"samples/sine.wav", 44100, 0), (b"SYN2", b"samples/saw.wav", 8287, 2))):
        p["instruments"][i] = {
            "kind": o3.KIND_INSTRUMENT, "midi": 0, "module": 0xFF, "name": name, "path": path, "rate": rate, "engine": engine,
            "m13": list(o3.INST13_DEFAULT), "stereo": 0, "head": list(head),
            "env": [[f32(1.0), 0, 10, 0, 0, f32(1.0), 0x1000, 0, 0] for _ in range(6)],
            "lfo": [[2, 0, f32(0.5)] for _ in range(6)],
            "slices": [0] * o3.L["slices"], "blob": b"", "wt": [0] * 12,
        }
        e = p["instruments"][i]["env"][2]
        e[0] = f32(0.5); e[2] = 100; e[4] = 400; e[5] = f32(0.4); e[8] = 1
    rows = [EMPTY_ROW] * o3.L["tracks"]
    for t in range(2):
        r = bytearray(EMPTY_ROW)
        for step, note in ((0, 60 + 4 * t), (8, 65 + 4 * t)):
            r[step * STEP:step * STEP + STEP] = bytes([note, t + 1, 255, 0, 0, 0, 0])
        rows[t] = bytes(r)
    p["patterns"][1] = {"lengths": [16] * o3.L["tracks"], "name": b"", "rows": rows}
    return p


def load_source(src):
    """The source project: the Dense8 folder when it is there, else the synthetic one (said so)."""
    path = os.path.join(src, "project.o3pj")
    if os.path.isfile(path):
        with open(path, "rb") as f:
            return o3.parse(f.read()), os.path.join(src, "samples"), "Dense8 fixture %s" % path
    print("NOTE: no Dense8 fixture at %s: building the arms from a small synthetic project instead "
          "(a real unit run wants `o3pj_tool gen dense8 <dir>/Dense8/project.o3pj` as SRC)" % path)
    return synthetic_project(), None, "synthetic project"


def check_arm(data, bend):
    """Parses an arm back and checks the cutoff slot of every sampler instrument (`bend` None: an ADSR)."""
    p = o3.parse(data)
    for i in sampler_slots(p):
        inst = p["instruments"][i]
        if bend is None:
            assert inst["env"][2][7] == 0 and inst["env"][2][8] == 1, "slot %d is not an ADSR" % i
            continue
        assert inst["env"][2][7] == 2 and inst["env"][2][8] == 1, "slot %d is not a WT Env" % i
        assert inst["lfo"][2][0] == CONTOUR and inst["wt"][4] == bend and inst["wt"][5] == SUST, "slot %d WT bytes wrong" % i
    return p


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-hil", "dense", "Dense8")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build-sdtest")
    p, samples, what = load_source(src)
    base = o3.parse(o3.build(p))  # a clean round trip before the arms are derived
    wt_b = lambda inst: set_cutoff_wt(inst, CONTOUR, BEND_B, SUST)
    wt_a = lambda inst: set_cutoff_wt(inst, CONTOUR, BEND_A, SUST)
    arms = ((ARM_A, lambda q: make_arm_a(q), BEND_A),
            (ARM_B, lambda q: make_arm_b(q, VOICES_B, wt_b), BEND_B),
            (ARM_ADSR, lambda q: make_arm_b(q, VOICES_B, set_cutoff_adsr), None),
            (ARM_XA, lambda q: make_arm_b(q, VOICES_B, set_cutoff_adsr, False), None),
            (ARM_XW, lambda q: make_arm_b(q, VOICES_B, wt_b, False), BEND_B),
            (ARM_XN, lambda q: make_arm_b(q, VOICES_B, wt_a, False), BEND_A),
            (ARM_PW, lambda q: make_arm_b(q, VOICES_B, set_mods_wt, False), BEND_B),
            (ARM_PA, lambda q: make_arm_b(q, VOICES_B, set_mods_adsr, False), None))
    for name, build, bend in arms:
        q = o3.parse(o3.build(base))  # each arm from the same clean source
        build(q)
        data = o3.build(q)
        q = check_arm(data, bend)
        folder = os.path.join(out, name)
        os.makedirs(os.path.join(folder, "samples"), exist_ok=True)
        with open(os.path.join(folder, "project.o3pj"), "wb") as f:
            f.write(data)
        if samples and os.path.isdir(samples):
            for w in os.listdir(samples):
                shutil.copy2(os.path.join(samples, w), os.path.join(folder, "samples", w))
        slot = "cutoff ADSR" if bend is None else "cutoff WT Env contour %d bend 0x%02X sust %d" % (CONTOUR, bend, SUST)
        print("%s: %s (%d bytes, %d sampler instruments, %d active tracks, %s) -> %s"
              % (name, what, len(data), len(sampler_slots(q)), q["active"], slot, folder))


if __name__ == "__main__":
    main()
