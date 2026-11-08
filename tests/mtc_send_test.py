#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The frame maths of the OS-MTC B1 host master: `midi_clock_tool.py mtc-send` (platform/mk66/tools/hil/midi_clock_tool.py, section OS-MTC B1).

What the master sends is checked here against values fixed by hand from the MIDI Time Code definition (a quarter frame is `F1 0nnn dddd`, piece n = 0..7 carrying frames low and
high, seconds low and high, minutes low and high, hours low, and hours high with the rate code `0 rr h`; a full frame is `F0 7F 7F 01 01 hh mm ss ff F7` with hh = rate << 5 | hours),
and against an independent drop-frame count (labels walked frame by frame with the 29.97 rule, no formula shared with the tool):

  - time label <-> frame count at 24, 25, 29.97 drop-frame and 30 fps (every frame of the first twenty minutes and a sample of the day, the day's wrap, labels that do not exist refused);
  - the eight pieces and the full frame of known times at the four rates, hours 17 (bit 4) and 23, a cycle on a day's wrap, drop-frame;
  - the stream the master plans: A2's decoder reads back what it sends at 24/25/30 fps (a run starting at any piece, spec A-2); the grid (every quarter frame at q / 4 frames of the
    master's clock: 10 ms apart at 25 fps, 1/1.001 of it at --rate 1.001, 1/(4 x 30000/1001) s at 29.97), the first one at least 4 ms after its full frame, a quiet that skips
    pieces and loses no time, a jump that restarts the grid, a reverse run (q falling by one, pieces 7..0), the jitter bounded and never reordering, --drop K/N dropping K of N
    after the first 16, the same seed the same plan;
  - the profile parser.

The synthetic chaser, the analysis and the live loop are the tool's own `--self-test` (ctest midi_clock_tool_selftest).

Usage: mtc_send_test.py <midi_clock_tool.py>
"""
import importlib.util
import sys


def load(path):
    spec = importlib.util.spec_from_file_location("midi_clock_tool_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    t = load(argv[1])
    problems = []

    def need(ok, what):
        if not ok:
            problems.append(what)

    def hexs(b):
        return " ".join("%02X" % x for x in b)

    def plan_of(profile, fps=25.0, **kw):
        kw.setdefault("tail", 0.0)
        return t.build_mtc_plan(t.parse_mtc_send_profile(profile), fps, **kw)

    def qfs(plan):
        return [e for e in plan.events if e[2] == "qf"]

    # ---- the time labels ----------------------------------------------------------------------------------------------------------------------------
    need(t.mtc_label_to_frames((1, 0, 0, 0), 1) == 90000, "01:00:00:00 at 25 fps is %d frames, wanted 90000" % t.mtc_label_to_frames((1, 0, 0, 0), 1))
    need(t.mtc_label_to_frames((23, 59, 59, 24), 1) == 25 * 86400 - 1, "the last frame of the day at 25 fps")
    need(t.mtc_label_to_frames((23, 59, 59, 23), 0) == 24 * 86400 - 1, "the last frame of the day at 24 fps")
    need(t.mtc_label_to_frames((1, 0, 0, 0), 3) == 108000, "01:00:00:00 at 30 fps is %d frames, wanted 108000" % t.mtc_label_to_frames((1, 0, 0, 0), 3))
    # drop-frame: known counts (30 a second, frames 00 and 01 of a minute skipped except every tenth minute)
    for label, want in (((0, 0, 59, 29), 1799), ((0, 1, 0, 2), 1800), ((0, 9, 59, 29), 17981), ((0, 10, 0, 0), 17982), ((0, 10, 0, 1), 17983), ((0, 11, 0, 2), 17982 + 1800),
                        ((1, 0, 0, 0), 107892), ((23, 59, 59, 29), 2589407)):
        got = t.mtc_label_to_frames(label, 2)
        need(got == want, "drop-frame %02d:%02d:%02d:%02d is %d frames, wanted %d" % (label + (got, want)))
    for rc in (0, 1, 2, 3):
        day = {0: 24 * 86400, 1: 25 * 86400, 2: 2589408, 3: 30 * 86400}[rc]
        need(t.mtc_frames_to_label(day, rc) == (0, 0, 0, 0), "rate code %d: the day's last frame + 1 is %r, wanted 00:00:00:00" % (rc, t.mtc_frames_to_label(day, rc)))
        need(t.mtc_frames_to_label(day - 1, rc) == {0: (23, 59, 59, 23), 1: (23, 59, 59, 24), 2: (23, 59, 59, 29), 3: (23, 59, 59, 29)}[rc], "rate code %d: the day's last frame" % rc)
        need(t.mtc_frames_to_label(3 * day + 5, rc) == t.mtc_frames_to_label(5, rc), "rate code %d: whole days wrap" % rc)
        for n in list(range(0, 30000)) + list(range(30000, day, 97)):  # round trip: every frame of the first 20 minutes, then a sample of the day
            try:
                ok = t.mtc_label_to_frames(t.mtc_frames_to_label(n, rc), rc) == n
            except ValueError:
                ok = False
            if not ok:
                problems.append("rate code %d: frame %d does not survive label and back" % (rc, n))
                break
    # an independent walk of the drop-frame count: every label of nearly three hours, in order, from the rule itself
    h = m = s = f = 0
    bad = None
    for n in range(320000):  # a little under three hours of 29.97 frames, past the first full hour at frame 107892
        try:
            ok = t.mtc_frames_to_label(n, 2) == (h, m, s, f) and t.mtc_label_to_frames((h, m, s, f), 2) == n
        except ValueError:
            ok = False
        if not ok:
            bad = (n, (h, m, s, f), t.mtc_frames_to_label(n, 2))
            break
        f += 1
        if f == 30:
            f, s = 0, s + 1
            if s == 60:
                s, m = 0, m + 1
                if m == 60:
                    m, h = 0, h + 1
                if m % 10 != 0:
                    f = 2  # a new minute that is not a tenth skips frame numbers 00 and 01
    need(bad is None, "the drop-frame walk disagrees at frame %r" % (bad,))
    for label, rc in (((0, 1, 0, 0), 2), ((0, 1, 0, 1), 2), ((0, 59, 0, 1), 2), ((0, 0, 0, 25), 1), ((0, 0, 0, 24), 0), ((0, 0, 0, 30), 3), ((24, 0, 0, 0), 1), ((0, 60, 0, 0), 1)):
        try:
            t.mtc_label_to_frames(label, rc)
            problems.append("the label %r at rate code %d was accepted" % (label, rc))
        except ValueError:
            pass
    for label in ((0, 0, 0, 0), (0, 0, 0, 1), (0, 10, 0, 0), (0, 10, 0, 1), (0, 20, 0, 1)):
        try:
            t.mtc_label_to_frames(label, 2)
        except ValueError:
            problems.append("the drop-frame label %r exists and was refused" % (label,))

    # ---- the pieces and the full frame: fixed vectors -----------------------------------------------------------------------------------------------
    vectors = (
        # (rate code, label, the eight pieces, the full frame)
        (0, (1, 2, 3, 4), "F1 04 F1 10 F1 23 F1 30 F1 42 F1 50 F1 61 F1 70", "F0 7F 7F 01 01 01 02 03 04 F7"),
        (3, (17, 58, 59, 24), "F1 08 F1 11 F1 2B F1 33 F1 4A F1 53 F1 61 F1 77", "F0 7F 7F 01 01 71 3A 3B 18 F7"),
        (2, (0, 10, 0, 0), "F1 00 F1 10 F1 20 F1 30 F1 4A F1 50 F1 60 F1 74", "F0 7F 7F 01 01 40 0A 00 00 F7"),
        (2, (0, 1, 0, 2), "F1 02 F1 10 F1 20 F1 30 F1 41 F1 50 F1 60 F1 74", "F0 7F 7F 01 01 40 01 00 02 F7"),
        (1, (0, 0, 0, 0), "F1 00 F1 10 F1 20 F1 30 F1 40 F1 50 F1 60 F1 72", "F0 7F 7F 01 01 20 00 00 00 F7"),
        (1, (23, 59, 58, 24), "F1 08 F1 11 F1 2A F1 33 F1 4B F1 53 F1 67 F1 73", "F0 7F 7F 01 01 37 3B 3A 18 F7"),
    )
    for rc, label, pieces, full in vectors:
        n = t.mtc_label_to_frames(label, rc)
        need(n % 2 == 0, "the vector %r is not on an even frame" % (label,))
        got = [t.mtc_qf_message(4 * n + p, rc) for p in range(8)]
        need(" ".join(hexs(b) for b in got) == pieces, "pieces of %r at rate code %d: %s, wanted %s" % (label, rc, " ".join(hexs(b) for b in got), pieces))
        need(hexs(t.mtc_full_frame_bytes(label, rc)) == full, "full frame of %r at rate code %d: %s, wanted %s" % (label, rc, hexs(t.mtc_full_frame_bytes(label, rc)), full))
    # every piece of a cycle spells the cycle's even frame, and the next cycle two frames on (a cycle on the day's wrap included)
    for rc in (0, 1, 2, 3):
        day = {0: 24 * 86400, 1: 25 * 86400, 2: 2589408, 3: 30 * 86400}[rc]
        for q in (4 * day - 8, 4 * day, 4 * day + 8, 4 * 5000 + 8):
            want = t.mtc_time_pieces(t.mtc_frames_to_label(2 * (q // 8), rc), rc)
            need([t.mtc_qf_message(q + p, rc) for p in range(8)] == want, "the cycle at q = %d, rate code %d" % (q, rc))
    # A2's decoder reads a stretch of pieces that starts at any piece: the cycles come out as the even frames, at the three integer rates
    for rc, fps in ((0, 24), (1, 25), (3, 30)):
        n0 = t.mtc_label_to_frames((22, 59, 58, 20), rc)
        dec = t.MTCDecoder()
        for i, q in enumerate(range(4 * n0 + 3, 4 * n0 + 3 + 8 * 6)):  # 3 pieces of a part cycle first, then five whole cycles
            piece = t.mtc_qf_message(q, rc)
            dec.feed(float(i), (piece[1] >> 4) & 7, piece[1])
        spelled = [c[2] for c in dec.cycles]
        need(spelled == [n0 + 2 * k for k in range(1, 6)] and dec.rate_code == rc, "A2's decoder at %d fps: %r (wanted %r)" % (fps, spelled, [n0 + 2 * k for k in range(1, 6)]))

    # ---- the planned stream -------------------------------------------------------------------------------------------------------------------------
    p = plan_of("at:01:00:00:00,run:1")
    q0 = 4 * 90000
    need(p.events[0][2] == "full" and p.events[0][0] == 0.0 and p.events[0][1] == t.mtc_full_frame_bytes((1, 0, 0, 0), 1), "the first event is not the full frame at 0")
    qs = qfs(p)
    need(len(qs) == 99, "run:1 at 25 fps: %d quarter frames, wanted 99 (q0 + 1 .. q0 + 99; q0 itself is due with the full frame, inside the 4 ms)" % len(qs))
    need([e[3] for e in qs] == list(range(q0 + 1, q0 + 100)), "the quarter frames are not consecutive from q0 + 1")
    need(all(abs(e[0] - (e[3] - q0) / 100.0) < 1e-9 for e in qs), "a quarter frame is off the 10 ms grid")
    need(all(e[1] == t.mtc_qf_message(e[3], 1) for e in qs), "a quarter frame's bytes are not the message of its q")
    need(qs[0][0] >= t.MTC_SEND_GAP_S, "the first quarter frame is %.4f s after the full frame" % qs[0][0])
    p = plan_of("at:01:00:00:00,run:2", rate=1.001)
    qs = qfs(p)
    need(all(abs(e[0] - (e[3] - q0) / (100.0 * 1.001)) < 1e-9 for e in qs), "--rate 1.001: a quarter frame is off the grid of a clock 0.1 % fast")
    p = plan_of("at:01:00:00:00,run:2", fps=29.97)
    qs = qfs(p)
    q0d = 4 * 107892
    need(qs and all(abs(e[0] - (e[3] - q0d) * 1001.0 / 120000.0) < 1e-9 for e in qs), "29.97 drop-frame: a quarter frame is off the 1/(4 x 30000/1001) s grid")
    need(all(e[1] == t.mtc_qf_message(e[3], 2) for e in qs), "29.97: the bytes of a quarter frame")
    for fps, rc, n0 in ((24.0, 0, 86400), (30.0, 3, 108000)):
        p = plan_of("at:01:00:00:00,run:1", fps=fps)
        qs = qfs(p)
        need(all(abs(e[0] - (e[3] - 4 * n0) / (4.0 * fps)) < 1e-9 for e in qs) and all(e[1] == t.mtc_qf_message(e[3], rc) for e in qs), "%g fps: the grid and the bytes" % fps)
    # a run that starts mid-frame of the clock: the start is a label, but the first quarter frame is the first grid point after the gap
    p = plan_of("at:01:00:00:00,run:1", start=(1, 0, 0, 0))
    need(p.start_frames == 90000, "start_frames")
    # a profile without `at` begins with the song's start
    p = plan_of("run:1", start=(2, 0, 0, 0))
    need(p.events[0][2] == "full" and p.events[0][1] == t.mtc_full_frame_bytes((2, 0, 0, 0), 1), "a profile without at: does not begin with the start's full frame")
    # a quiet: the clock runs, nothing is sent, the grid goes on (the pieces in between are lost, no time is)
    p = plan_of("at:01:00:00:00,run:1,quiet:0.5,run:1")
    qs = qfs(p)
    need(not any(1.0 - 1e-9 <= e[0] < 1.5 - 1e-9 for e in qs), "a quarter frame was sent in the quiet")
    need(all(abs(e[0] - (e[3] - q0) / 100.0) < 1e-9 for e in qs), "after a quiet the quarter frames are off the master's clock")
    need(len(qs) == 99 + 100 and qs[99][3] == q0 + 150, "after run:1,quiet:0.5: %d quarter frames, the first after the quiet is q0 + %d (wanted 199, q0 + 150)" % (len(qs), qs[99][3] - q0))
    need([s["kind"] for s in p.segments] == ["at", "run", "quiet", "run"], "segments %r" % [s["kind"] for s in p.segments])
    # a jump: a second full frame, the clock is placed, the grid restarts after it
    p = plan_of("at:01:00:00:00,run:1,jump:01:00:10:00,run:1")
    fulls = [e for e in p.events if e[2] == "full"]
    need(len(fulls) == 2 and fulls[1][1] == t.mtc_full_frame_bytes((1, 0, 10, 0), 1) and abs(fulls[1][0] - 1.0) < 1e-9, "the jump's full frame: %r" % (fulls,))
    after = [e for e in qfs(p) if e[0] > 1.0 - 1e-9]
    qj = 4 * (90000 + 250)
    need(after and all(abs(e[0] - (1.0 + (e[3] - qj) / 100.0)) < 1e-9 for e in after) and after[0][0] - 1.0 >= t.MTC_SEND_GAP_S and after[0][3] == qj + 1, "after the jump the grid is the new time's: %r" % (after[:2],))
    # a reverse run: q falls by one, the pieces go 7..0, the time falls
    p = plan_of("at:01:00:05:00,reverse:1")
    qs = qfs(p)
    qr = 4 * (90000 + 125)
    need(len(qs) == 99 and [e[3] for e in qs] == list(range(qr - 1, qr - 100, -1)), "reverse: %d quarter frames, first %r" % (len(qs), [e[3] for e in qs[:3]]))
    need(all(abs(e[0] - (qr - e[3]) / 100.0) < 1e-9 for e in qs), "reverse: off the grid")
    need([(e[1][1] >> 4) & 7 for e in qs[:9]] == [(qr - 1 - i) % 8 for i in range(9)], "reverse: the pieces do not go 7..0")
    p = plan_of("at:01:00:05:00,run:1,reverse:1,run:1")  # the turn-arounds repeat no piece
    qs = [e[3] for e in qfs(p)]
    need(all(abs(a - b) == 1 for a, b in zip(qs, qs[1:])), "turn-around: a quarter frame repeats or skips: %r" % [(a, b) for a, b in zip(qs, qs[1:]) if abs(a - b) != 1][:3])
    try:
        plan_of("at:00:00:00:10,reverse:2")
        problems.append("a reverse run back past 00:00:00:00 was accepted")
    except ValueError:
        pass
    # across the day's wrap the labels wrap
    p = plan_of("at:23:59:59:00,run:3")
    qs = qfs(p)
    labels = {t.mtc_frames_to_label(2 * (e[3] // 8), 1) for e in qs}
    need((0, 0, 0, 0) in labels and (23, 59, 59, 23) in labels, "across midnight the cycles do not wrap: %r" % sorted(labels)[:4])
    # the jitter is bounded and never reorders; the same seed is the same plan; another seed is not
    p = plan_of("at:01:00:00:00,run:5", jitter_ms=2.0, seed=3)
    qs = qfs(p)
    need(all(abs(e[0] - (e[3] - q0) / 100.0) <= 0.0025 for e in qs), "the jitter exceeds +-2 ms (and the 50 us a reordering fix may add)")
    need(all(a[0] < b[0] for a, b in zip(p.events, p.events[1:])), "the jitter reordered the events")
    need(plan_of("at:01:00:00:00,run:5", jitter_ms=2.0, seed=3).events == p.events and plan_of("at:01:00:00:00,run:5", jitter_ms=2.0, seed=4).events != p.events, "the seed")
    need(max(abs(e[0] - (e[3] - q0) / 100.0) for e in qs) > 0.001, "a jitter of 2 ms never moved a message by 1 ms")
    # --drop K/N: K of every N quarter frames, never one of the first 16
    p = plan_of("at:01:00:00:00,run:10", drop=(1, 8), seed=2)
    qs = qfs(p)
    need(p.qf_planned == 999 and p.qf_dropped + len(qs) == 999, "drop 1/8: planned %d dropped %d sent %d" % (p.qf_planned, p.qf_dropped, len(qs)))
    need(abs(p.qf_dropped - (999 - 16) / 8.0) <= 2.0, "drop 1/8: %d dropped, about %.0f expected" % (p.qf_dropped, (999 - 16) / 8.0))
    need([e[3] for e in qs[:16]] == list(range(q0 + 1, q0 + 17)), "drop: one of the first 16 was dropped")
    need(len({e[3] for e in qs}) == len(qs), "drop: a quarter frame twice")
    # the tail and the segments
    p = t.build_mtc_plan(t.parse_mtc_send_profile("at:01:00:00:00,run:2"), 25.0, tail=3.0)
    need(abs(p.t_end - 2.0) < 1e-9 and abs(p.t_total - 5.0) < 1e-9 and p.segments[-1]["kind"] == "tail", "the tail: t_end %r t_total %r" % (p.t_end, p.t_total))

    # ---- the profile parser -------------------------------------------------------------------------------------------------------------------------
    tokens = t.parse_mtc_send_profile(" AT:00:59:58:00, run:10,quiet:0.1 ,jump:01:00:20:00,reverse:2.5,")
    need(tokens == [("at", (0, 59, 58, 0)), ("run", 10.0), ("quiet", 0.1), ("jump", (1, 0, 20, 0)), ("reverse", 2.5)], "the profile parsed as %r" % (tokens,))
    for bad in ("", "quiet:1", "run:0", "run:-1", "run:x", "run", "at:1:2:3,run:1", "at:25:00:00:00,run:1", "wait:3,run:1", "at,run:1", "jump:,run:1"):
        try:
            t.parse_mtc_send_profile(bad)
            problems.append("the profile %r was accepted" % bad)
        except ValueError:
            pass
    for bad_fps in (23.976, 29.0, 60.0):
        try:
            t.mtc_rate_info(bad_fps)
            problems.append("--fps %r was accepted" % bad_fps)
        except ValueError:
            pass
    need([t.mtc_rate_info(f)[0] for f in (24, 25, 29.97, 30)] == [0, 1, 2, 3], "the rate codes of 24, 25, 29.97, 30")

    for pr in problems:
        print("mtc_send: " + pr, file=sys.stderr)
    if problems:
        return 1
    print("mtc_send: ok (labels at the four rates and the drop-frame walk, pieces and full frames of fixed vectors, A2's decoder, the grid, quiet, jump, reverse, jitter, drop, the parser)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
