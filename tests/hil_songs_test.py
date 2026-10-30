#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The songs of the unit's step-FX CPU runs (`o3pj_tool gen lfostepNN|lfostepoff|moddriveN[off]`, tests/tools/o3pj_tool.cpp) are what the HIL scenarios qa_lfostep and qa_cpu_moddrive expect.

OS-PERF-LFO-STEP follow-up: `lfostep23|24|33|36` and the control `lfostepoff` are twelve sampler tracks (four one-shot, four wavetable, four granular) with the step FX on every note step; the control has the same
instruments and no FX. OS-BUG-MODDRIVE: `moddrive1|4|8` are 1, 4 or 8 TB-303 tracks with the step FX 38 and 39, `moddriveNoff` the same without them. For each song this test runs the tool twice (the same bytes),
loads the project through the C++ reader (`resave`: byte for byte the same file, `dump`: the model as text) and holds the dump to the recipe. The step FX themselves are not in the dump (a pattern row is a hash): a song with the FX
must differ from its control on every track in use and agree with it on the others, and the songs of different FX must differ from one another.

Usage: hil_songs_test.py <o3pj_tool>
"""
import os
import re
import subprocess
import sys
import tempfile

LFOSTEP = ("lfostep23", "lfostep24", "lfostep33", "lfostep36")
LFOSTEP_TRACKS = 12
MODDRIVE = (("moddrive1", "moddrive1off", 1), ("moddrive4", "moddrive4off", 4), ("moddrive8", "moddrive8off", 8))


def hexstr(h):
    return bytes.fromhex(h).decode("ascii")


def gen(tool, root, kind):
    """Writes the song into root/<kind>/ (with its samples/ folder); returns the project's path and what was written."""
    os.makedirs(os.path.join(root, kind, "samples"))
    path = os.path.join(root, kind, "project.o3pj")
    subprocess.run([tool, "gen", kind, path], check=True)
    found = {}
    for base, _dirs, files in os.walk(os.path.join(root, kind)):
        for name in files:
            p = os.path.join(base, name)
            with open(p, "rb") as f:
                found[os.path.relpath(p, os.path.join(root, kind))] = f.read()
    return path, found


def load(tool, kind, need):
    """Generates twice, resaves and dumps one song; returns (dump lines, written files, rows of pattern 1 or None)."""
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        path, files = gen(tool, a, kind)
        _path, again = gen(tool, b, kind)
        need(files == again, "%s: two runs of `gen` differ" % kind)
        resaved = os.path.join(a, "resaved.o3pj")
        subprocess.run([tool, "resave", path, resaved], check=True)
        with open(path, "rb") as f1, open(resaved, "rb") as f2:
            need(f1.read() == f2.read(), "%s: the project does not resave byte for byte" % kind)
        dump = subprocess.run([tool, "dump", path], check=True, capture_output=True, text=True).stdout.splitlines()
    rows = None
    for line in dump:
        m = re.match(r"pattern 1 lens=\S+ name=\w* rows=(\S+)", line)
        if m:
            rows = m.group(1).split(",")
    need(rows is not None and len(rows) == 16, "%s: no pattern 1" % kind)
    return dump, files, rows if rows is not None and len(rows) == 16 else None


def head(dump, kind, need):
    m = re.match(r"project name=(\w+) tempo=(\d+) swing=\d+ deflen=(\d+) active=(\d+) master=", dump[0])
    need(m is not None, "%s: no project line" % kind)
    return (hexstr(m.group(1)), int(m.group(3)), int(m.group(4))) if m else ("", 0, 0)


def instruments(dump):
    """The instrument lines: (slot, kind, path, sampler fields) for every instrument."""
    found = []
    for i, line in enumerate(dump):
        m = re.match(r"instrument (\d+) kind=(\d) midi=\d+ module=(\d+) name=\w+ path=(\w*) rate=\d+", line)
        if m:
            found.append((int(m.group(1)), int(m.group(2)), hexstr(m.group(4)), dump[i + 1].split()))
    return found


def check_step_fx(files, kind, fx, need):
    """Reads the written project with tools/o3project.py: every note step of tracks 1..12 holds the instrument of its track and, in FX column 1, the id `fx` at rate 8 (nothing for the control)."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
    try:
        import o3project
    finally:
        sys.path.pop(0)
    p = o3project.parse(files["project.o3pj"])
    rows = p["patterns"][1]["rows"]
    sb = o3project.STEP_BYTES
    for t in range(LFOSTEP_TRACKS):
        for k in range(16):
            note, instr, _vol, t1, v1, t2, v2 = rows[t][k * sb:(k + 1) * sb]
            if k % 2 == 0:
                ok = note != 0 and instr == t + 1 and (t1, v1) == ((fx, 8) if fx else (0, 0)) and (t2, v2) == (0, 0)
            else:
                ok = rows[t][k * sb:(k + 1) * sb] == o3project.EMPTY_STEP
            need(ok, "%s: track %d step %d is %r (wanted %s)" % (kind, t + 1, k + 1, rows[t][k * sb:(k + 1) * sb].hex(),
                 "a note of instrument %d with FX %d at rate 8" % (t + 1, fx) if k % 2 == 0 else "empty"))


def check_lfostep(tool, need, rows_of):
    for kind in LFOSTEP + ("lfostepoff",):
        dump, files, rows = load(tool, kind, need)
        name, deflen, active = head(dump, kind, need)
        need(name == ("LfoStepOff" if kind == "lfostepoff" else "LfoStep" + kind[7:]), "%s: the project's name is %r" % (kind, name))
        need(deflen == 16 and active == LFOSTEP_TRACKS, "%s: pattern length %d, %d active tracks" % (kind, deflen, active))
        need(sorted(files) == sorted(["project.o3pj"] + [os.path.join("samples", n) for n in ("sine.wav", "saw.wav", "pluck.wav", "hat.wav")]),
             "%s: the files written are %s" % (kind, sorted(files)))
        ins = [i for i in instruments(dump) if i[1] == 1]
        need([i[0] for i in ins] == list(range(LFOSTEP_TRACKS)), "%s: sample instruments in slots %s" % (kind, [i[0] for i in ins]))
        for slot, _k, path, s in ins:
            # sampler sample_type play_mode ...: tracks 1..4 one shot, 5..8 wavetable (play mode 6), 9..12 granular (play mode 7)
            want = ("0", "0") if slot < 4 else ("1", "6") if slot < 8 else ("0", "7")
            need((s[1], s[2]) == want, "%s: instrument %d has sample type %s, play mode %s; wanted %s" % (kind, slot + 1, s[1], s[2], want))
            need(path == ("samples/sine.wav" if 4 <= slot < 8 else "samples/pluck.wav"), "%s: instrument %d names %s" % (kind, slot + 1, path))
        check_step_fx(files, kind, 0 if kind == "lfostepoff" else int(kind[7:]), need)
        if rows:
            empty = rows[15]
            need(all(rows[t] == empty for t in range(LFOSTEP_TRACKS, 15)), "%s: tracks 13 to 16 are not empty" % kind)
            need(all(rows[t] != empty for t in range(LFOSTEP_TRACKS)), "%s: a track in use has no steps" % kind)
        rows_of[kind] = rows
    off = rows_of["lfostepoff"]
    for kind in LFOSTEP:
        rows = rows_of[kind]
        if rows and off:
            need(all(rows[t] != off[t] for t in range(LFOSTEP_TRACKS)), "%s: a track holds no step FX (the same row as the control)" % kind)
            need(rows[12:] == off[12:], "%s: tracks 13 to 16 differ from the control" % kind)
        for other in LFOSTEP:
            if other < kind and rows and rows_of[other]:
                need(all(rows[t] != rows_of[other][t] for t in range(LFOSTEP_TRACKS)), "%s and %s hold the same step FX" % (kind, other))


def check_moddrive(tool, need):
    for fx_kind, off_kind, n in MODDRIVE:
        dump, _files, rows = load(tool, fx_kind, need)
        dump_off, _files_off, rows_off = load(tool, off_kind, need)
        for d, kind in ((dump, fx_kind), (dump_off, off_kind)):
            name, _deflen, active = head(d, kind, need)
            need(active == n and name.startswith("ModDrive%d" % n), "%s: %d active tracks, name %r" % (kind, active, name))
            mods = [i for i in instruments(d) if i[1] == 3]
            need([i[0] for i in mods] == list(range(n)), "%s: module instruments in slots %s" % (kind, [i[0] for i in mods]))
        if rows and rows_off:
            need(all(rows[t] != rows_off[t] for t in range(n)), "%s: a track holds no step FX (the same row as the control)" % fx_kind)
            need(rows[n:] == rows_off[n:], "%s: the tracks after %d differ from the control" % (fx_kind, n))


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    tool = argv[1]
    problems = []

    def need(ok, what):
        if not ok:
            problems.append(what)

    rows_of = {}
    check_lfostep(tool, need, rows_of)
    check_moddrive(tool, need)
    for p in problems:
        print("hil_songs: " + p, file=sys.stderr)
    if problems:
        return 1
    print("hil_songs: ok (lfostep23|24|33|36|off, moddrive1|4|8 and their controls)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
