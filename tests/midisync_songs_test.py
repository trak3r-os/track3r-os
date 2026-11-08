#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The test songs of the OS-MIDISYNC MIDI clock bench (platform/mk66/tools/hil/midi_clock_tool.py `sync`): `o3pj_tool gen midisync|midisync-song|midisync-load` (tests/tools/o3pj_tool.cpp fill_midisync).

Each song builds, is written twice with the same bytes, resaves byte for byte and loads back through the C++ reader (`dump`), and its Sync track plays a note on EVERY step of every pattern in Song order, the note naming
the step: on the wire MIDI note 36 + 16 * (pattern - 1) + step (the stored byte is one more, model::kNoteFirst), so the tool knows which step it heard whatever happened to the clock. `midisync` is one 16-step pattern
(Pattern mode, --cycle 16), `midisync-song` three of them in Song order (48 steps before the notes repeat: --cycle 48, pattern 2 starts at note 52), `midisync-load` the Dense8 stress song (fill_heavy: 4 Modern + 2
Paula voices, TB-303, FM, send FX and limiter) with the Sync track as its ninth track, for the bench runs with the load playing.

Usage: midisync_songs_test.py <o3pj_tool>
"""
import os
import re
import subprocess
import sys
import tempfile

KINDS = ("midisync", "midisync-song", "midisync-load")
KIT_WAVS = ("sine.wav", "saw.wav", "pluck.wav", "hat.wav")
SYNC_SLOT = 48          # instrument 49 on the Sync track
SYNC_TRACK = {"midisync": 0, "midisync-song": 0, "midisync-load": 8}
PATTERNS = {"midisync": 1, "midisync-song": 3, "midisync-load": 1}
NOTE_BASE = 36          # the wire note of step 0 of pattern 1 (stored byte = 1 + NOTE_BASE + ...: model::kNoteFirst)
STEP_BYTES = 7


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
    """Generates twice, resaves and dumps one song; returns (dump lines, written files)."""
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        path, files = gen(tool, a, kind)
        _path, again = gen(tool, b, kind)
        need(files == again, "%s: two runs of `gen` differ" % kind)
        resaved = os.path.join(a, "resaved.o3pj")
        subprocess.run([tool, "resave", path, resaved], check=True)
        with open(path, "rb") as f1, open(resaved, "rb") as f2:
            need(f1.read() == f2.read(), "%s: the project does not resave byte for byte" % kind)
        dump = subprocess.run([tool, "dump", path], check=True, capture_output=True, text=True).stdout.splitlines()
    return dump, files


def head(dump, kind, need):
    m = re.match(r"project name=(\w+) tempo=(\d+) swing=\d+ deflen=(\d+) active=(\d+) master=", dump[0])
    need(m is not None, "%s: no project line" % kind)
    return (hexstr(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))) if m else ("", 0, 0, 0)


def track_name(dump, track, need, kind):
    m = re.match(r"track %d type=\d+ flags=\d+ vol=\d+ pan=\d+ bus=\d+ name=(\w+)" % track, dump[1 + track])  # the project line is dump[0]
    need(m is not None, "%s: no line for track %d" % (kind, track))
    return hexstr(m.group(1)) if m else ""


def song_of(dump, need, kind):
    line = next(l for l in dump if l.startswith("song "))  # "song <count> <order ids>"
    return [int(x) for x in line.split()[2:]]


def check_sync_instrument(dump, kind, need):
    line = next((l for l in dump if l.startswith("instrument %d " % SYNC_SLOT)), None)
    need(line is not None, "%s: no instrument %d" % (kind, SYNC_SLOT))
    if line is None:
        return
    m = re.match(r"instrument 48 kind=(\d) midi=(\d+) module=(\d+) name=(\w+) .*m13=(\d+),(\d+),(\d+)", line)
    need(m is not None, "%s: the Sync instrument line is %r" % (kind, line))
    if m:
        need(m.group(1) == "2" and m.group(2) == "1" and hexstr(m.group(4)) == "SYNC",
             "%s: instrument 48 is kind=%s midi=%s name=%r (wanted MIDI, channel 1, SYNC)" % (kind, m.group(1), m.group(2), hexstr(m.group(4))))
        need(m.group(7) == "2", "%s: the Sync instrument's MIDI out is %s, wanted 2 (USB): %r" % (kind, m.group(7), line))  # m13 = velocity, program, out, ...


def check_steps(files, kind, need):
    """Reads the written project with tools/o3project.py: every step of the Sync track of every pattern plays instrument 49 with the note that names it, 1 + 36 + 16*(pattern-1) + step."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
    try:
        import o3project
    finally:
        sys.path.pop(0)
    p = o3project.parse(files["project.o3pj"])
    track = SYNC_TRACK[kind]
    need(sorted(p["patterns"]) == list(range(1, PATTERNS[kind] + 1)), "%s: patterns %s" % (kind, sorted(p["patterns"])))
    need(p["song"] == list(range(1, PATTERNS[kind] + 1)), "%s: song order %s" % (kind, p["song"]))
    for pid in sorted(p["patterns"]):
        pat = p["patterns"][pid]
        need(all(n == 16 for n in pat["lengths"]), "%s: pattern %d lengths %s" % (kind, pid, pat["lengths"]))
        rows = pat["rows"][track]
        for k in range(16):
            note, instr, vol, t1, v1, t2, v2 = rows[k * STEP_BYTES:(k + 1) * STEP_BYTES]
            want = 1 + NOTE_BASE + 16 * (pid - 1) + k  # model::kNoteFirst + 36 + 16*(pattern-1) + step; on the wire: note - 1
            need(note == want and instr == SYNC_SLOT + 1 and vol == 255 and (t1, v1, t2, v2) == (0, 0, 0, 0),
                 "%s: pattern %d track %d step %d is note=%d instr=%d vol=%d fx=%d,%d,%d,%d (wanted note=%d instr=49, no FX)"
                 % (kind, pid, track, k, note, instr, vol, t1, v1, t2, v2, want))


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    tool = argv[1]
    problems = []

    def need(ok, what):
        if not ok:
            problems.append(what)

    for kind in KINDS:
        dump, files = load(tool, kind, need)
        name, tempo, deflen, active = head(dump, kind, need)
        want_name = {"midisync": "MidiSync", "midisync-song": "MidiSync Song", "midisync-load": "MidiSync Load"}[kind]
        need(name == want_name, "%s: the project's name is %r, wanted %r" % (kind, name, want_name))
        need(tempo == (12500 if kind == "midisync-load" else 12000) and deflen == 16,
             "%s: tempo %d, pattern length %d (wanted %s, 16)" % (kind, tempo, deflen, "125.00 BPM (Dense8)" if kind == "midisync-load" else "120.00 BPM"))
        need(active == (1 if kind != "midisync-load" else 9), "%s: %d active tracks" % (kind, active))
        need(track_name(dump, SYNC_TRACK[kind], need, kind) == "Sync", "%s: track %d is not named Sync" % (kind, SYNC_TRACK[kind]))
        need(song_of(dump, need, kind) == list(range(1, PATTERNS[kind] + 1)), "%s: song order wrong" % kind)
        check_sync_instrument(dump, kind, need)
        want_files = ["project.o3pj"] + ([os.path.join("samples", n) for n in KIT_WAVS] if kind == "midisync-load" else [])
        need(sorted(files) == sorted(want_files), "%s: the files written are %s" % (kind, sorted(files)))
        if kind == "midisync-load":  # the Dense8 song under it (fill_heavy kDense8): 6 sampler + 2 module instruments, two modules, the Acid and FM track names
            ins = [l for l in dump if l.startswith("instrument ")]
            need(any(re.match(r"instrument [0-5] kind=1 ", l) for l in ins) and sum(1 for l in ins if re.match(r"instrument [0-5] kind=1 ", l)) == 6,
                 "%s: the six sampler instruments are missing" % kind)
            need(sum(1 for l in ins if re.match(r"instrument [67] kind=3 ", l)) == 2, "%s: the two module instruments are missing" % kind)
            need(len([l for l in dump if l.startswith("module ")]) == 2, "%s: the two modules are missing" % kind)
            need(track_name(dump, 6, need, kind) == "Acid" and track_name(dump, 7, need, kind) == "FM", "%s: the Acid / FM track names" % kind)
        check_steps(files, kind, need)
    for p in problems:
        print("midisync_songs: " + p, file=sys.stderr)
    if problems:
        return 1
    print("midisync_songs: ok (midisync, midisync-song, midisync-load: a note naming every step, 36 + 16*(pattern-1) + step)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
