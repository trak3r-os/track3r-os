#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The OS-MTC A2 test song: `o3pj_tool gen mtc-tempo` (tests/tools/o3pj_tool.cpp fill_mtctempo).

The song builds, is written twice with the same bytes, resaves byte for byte and loads back through the C++ reader (`dump`), and it is the `midisync-song` (three
16-step patterns in Song order, the Sync track playing the note that names every step: wire note 36 + 16 * (pattern - 1) + step, stored byte one more) with the
timing FX of docs/specs/OS-MTC.md slice A2, checked here against the step layout of docs/formats/o3-project.md (note, instrument, volume, FX 1 type/value, FX 2
type/value; the FX types of ADR 0004) and the values docs/formats/med.md gives them:

  (pattern 2, step 1)  FX 15 Tempo, value 70   = 140 BPM (the value is BPM / 2, below 40 BPM reads as 40; med.md 3.5, PAR-082)
  (pattern 2, step 2)  FX 17 Swing, value 50   (25..75, straight at 50)
  (pattern 3, step 1)  FX 44 Tempo fine, value 132 = 128 + 4 x 1, so +1.00 BPM (adds (v - 128) / 4 BPM; med.md section 4)
  (pattern 3, step 2)  FX 13 Break, value 1    (ends the pattern at this step: the song loops from slot 3 back to slot 1)

Every other step carries no FX. midi_clock_tool.py's `mtc --song mtc-tempo` walks the same plan.

Usage: mtc_tempo_song_test.py <o3pj_tool>
"""
import os
import re
import subprocess
import sys
import tempfile

SYNC_SLOT = 48   # instrument 49 on the Sync track (track 0)
NOTE_BASE = 36   # the wire note of step 0 of pattern 1 (stored byte = 1 + NOTE_BASE + ...: model::kNoteFirst)
STEP_BYTES = 7
PATTERNS = 3
STEPS = 16
# The timing FX of the song, (pattern, step) -> (FX 1 type, FX 1 value); FX 2 stays empty. The numbers and values as documented above.
WANT_FX = {
    (2, 1): (15, 70),   # Tempo 140 BPM (value 70 = 140 / 2)
    (2, 2): (17, 50),   # Swing, straight
    (3, 1): (44, 132),  # Tempo fine +1.00 BPM
    (3, 2): (13, 1),    # Break pattern
}


def hexstr(h):
    return bytes.fromhex(h).decode("ascii")


def gen(tool, root):
    os.makedirs(os.path.join(root, "samples"))
    path = os.path.join(root, "project.o3pj")
    subprocess.run([tool, "gen", "mtc-tempo", path], check=True)
    found = {}
    for base, _dirs, files in os.walk(root):
        for name in files:
            p = os.path.join(base, name)
            with open(p, "rb") as f:
                found[os.path.relpath(p, root)] = f.read()
    return path, found


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    tool = argv[1]
    problems = []

    def need(ok, what):
        if not ok:
            problems.append(what)

    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        path, files = gen(tool, a)
        _path, again = gen(tool, b)
        need(files == again, "two runs of `gen mtc-tempo` differ")
        need(sorted(files) == ["project.o3pj"], "the files written are %s" % sorted(files))
        resaved = os.path.join(a, "resaved.o3pj")
        subprocess.run([tool, "resave", path, resaved], check=True)
        with open(path, "rb") as f1, open(resaved, "rb") as f2:
            need(f1.read() == f2.read(), "the project does not resave byte for byte")
        dump = subprocess.run([tool, "dump", path], check=True, capture_output=True, text=True).stdout.splitlines()

    m = re.match(r"project name=(\w+) tempo=(\d+) swing=(\d+) deflen=(\d+) active=(\d+) master=", dump[0])
    need(m is not None, "no project line: %r" % dump[0])
    if m:
        need(hexstr(m.group(1)) == "MTC Tempo", "the project's name is %r" % hexstr(m.group(1)))
        need(int(m.group(2)) == 12000 and int(m.group(3)) == 50 and int(m.group(4)) == STEPS and int(m.group(5)) == 1,
             "tempo %s swing %s deflen %s active %s (wanted 12000, 50, %d, 1)" % (m.group(2), m.group(3), m.group(4), m.group(5), STEPS))
    line = next((l for l in dump if l.startswith("song ")), None)
    need(line is not None and [int(x) for x in line.split()[2:]] == list(range(1, PATTERNS + 1)), "song order: %r" % line)
    m = re.match(r"track 0 type=\d+ flags=\d+ vol=\d+ pan=\d+ bus=\d+ name=(\w+)", dump[1])
    need(m is not None and hexstr(m.group(1)) == "Sync", "track 0 is not named Sync: %r" % dump[1])
    line = next((l for l in dump if l.startswith("instrument %d " % SYNC_SLOT)), None)
    need(line is not None, "no instrument %d" % SYNC_SLOT)
    if line:
        m = re.match(r"instrument 48 kind=(\d) midi=(\d+) module=(\d+) name=(\w+) .*m13=(\d+),(\d+),(\d+)", line)
        need(m is not None, "the Sync instrument line is %r" % line)
        if m:
            need(m.group(1) == "2" and m.group(2) == "1" and hexstr(m.group(4)) == "SYNC",
                 "instrument 48 is kind=%s midi=%s name=%r (wanted MIDI, channel 1, SYNC)" % (m.group(1), m.group(2), hexstr(m.group(4))))
            need(m.group(7) == "2", "the Sync instrument's MIDI out is %s, wanted 2 (USB): %r" % (m.group(7), line))

    # the steps, read back with tools/o3project.py: every step names itself, and the four FX steps carry exactly the FX above
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
    try:
        import o3project
    finally:
        sys.path.pop(0)
    p = o3project.parse(files["project.o3pj"])
    need(sorted(p["patterns"]) == list(range(1, PATTERNS + 1)), "patterns %s" % sorted(p["patterns"]))
    need(p["song"] == list(range(1, PATTERNS + 1)), "song order %s" % p["song"])
    for pid in range(1, PATTERNS + 1):
        pat = p["patterns"][pid]
        need(all(n == STEPS for n in pat["lengths"]), "pattern %d lengths %s" % (pid, pat["lengths"]))
        rows = pat["rows"][0]
        need(len(rows) == 128 * STEP_BYTES, "pattern %d track 0 has %d bytes" % (pid, len(rows)))  # the format stores 128 steps a track
        for k in range(STEPS):
            note, instr, vol, t1, v1, t2, v2 = rows[k * STEP_BYTES:(k + 1) * STEP_BYTES]
            want_note = 1 + NOTE_BASE + 16 * (pid - 1) + k
            want_fx = WANT_FX.get((pid, k), (0, 0))
            need(note == want_note and instr == SYNC_SLOT + 1 and vol == 255 and (t1, v1, t2, v2) == (want_fx[0], want_fx[1], 0, 0),
                 "pattern %d step %d is note=%d instr=%d vol=%d fx=%d,%d,%d,%d (wanted note=%d instr=49 vol=255 fx=%d,%d,0,0)"
                 % (pid, k, note, instr, vol, t1, v1, t2, v2, want_note, want_fx[0], want_fx[1]))

    for p_ in problems:
        print("mtc_tempo_song: " + p_, file=sys.stderr)
    if problems:
        return 1
    print("mtc_tempo_song: ok (mtc-tempo: a note naming every step, Tempo 15/70, Swing 17/50, Tempo fine 44/132, Break 13/1)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
