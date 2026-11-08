#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""midi_clock_tool: MIDI clock out / in checks of the Tracker over a raw ALSA MIDI device node (stdlib only; Linux). Helper of MANUAL.md, not a scenario.

    midi_clock_tool.py list                                   the raw MIDI nodes (/dev/snd/midiC*D*) and what `amidi -l` calls them
    midi_clock_tool.py measure DEV [--seconds 20]             count the 0xF8 clocks the unit sends (and FA / FB / FC / FE bytes) and say the BPM, in 1 s windows too
    midi_clock_tool.py send DEV --profile 120:10,140:10 [--start] [--stop]
                                                              be the MIDI clock master: 24 clocks a beat at the BPM of each segment ("bpm:seconds,..."),
                                                              FA (Start) first with --start (FB Continue with --continue), FC (Stop) at the end with --stop
    midi_clock_tool.py sync DEV --profile "120:30" [--json out.json] [options]
                                                              OS-MIDISYNC: the host is the clock master AND listens to the notes the unit plays back; says how well the unit followed
                                                              (docs: platform/mk66/tools/hil/MIDISYNC.md; `sync --help`)
    midi_clock_tool.py mtc DEV [--seconds N] [--fps F] [--expect HH:MM:SS:FF] [--song midisync-song|mtc-tempo] [--json out.json]
                                                              OS-MTC A2: listen and analyse the MTC the unit sends (docs: platform/mk66/tools/hil/MTC.md): the rate code; piece continuity
                                                              (dropped / repeated / mixed, the time of a cycle +2 frames); the quarter-frame interval mean against the host clock, in ppm, and the
                                                              straight-line fit of the decoded time against the arrival (drift in ppm, residual); the full frames against --expect; the notes against
                                                              the time code (--song's plan, --expect as the start time)
    midi_clock_tool.py mtc-send DEV --profile "at:01:00:00:00,run:30" [--fps 25] [--rate R] [--jitter MS] [--drop K/N] [--json out.json] [options]
                                                              OS-MTC B1: the host is the MTC master (quarter frames and full frames at absolute deadlines) AND listens to the notes
                                                              the unit plays back; lock time, phase against the master's time, drift, the stop latency after a quiet, the relocation
                                                              time after a jump (docs: platform/mk66/tools/hil/MTC.md; `mtc-send --help`)
    midi_clock_tool.py --self-test                            send and measure against a FIFO, the sync analysis against a synthetic unit, the MTC
                                                              analysis against synthetic streams with fed timestamps, and the mtc-send master against a
                                                              synthetic chaser (also over FIFOs): no MIDI device

DEV is /dev/snd/midiC<card>D<device> (the card of `amidi -l`: hw:1,0,0 is /dev/snd/midiC1D0). The node must be readable / writable by you (group audio).
Timestamps are the host's monotonic clock when read() returned: a USB MIDI packet is 1 ms, so single clocks are only known to about +-1-2 ms; the mean over 20 s is good to 0.01 %.

SYNC MODE
  The master sends the clock at absolute deadlines (monotonic_ns; sleeps to 1 ms before the deadline, then spins; SCHED_FIFO when the OS allows it (--rtprio N, default 20; capped at the
  user's RLIMIT_RTPRIO, no root needed, no limit changed), and the report line says which scheduler it ran under: "SCHED_FIFO 20" or "SCHED_OTHER (no rtprio)") and listens on the same node in the same loop, so the unit's notes are stamped with the clock the clocks were sent by. Profile = comma-separated tokens, in order:
      120:30            steady tempo, BPM:seconds (a second steady token with another tempo is a jump)
      ramp:100-180:30   linear tempo ramp over 30 s
      quiet:5           no clock for 5 s, no Stop (the ideal grid goes on at the last tempo)
      start cont stop   FA, FB, FC (the byte goes out 2 ms before the next clock, --lead-ms)
      spp:20            F2 Song Position Pointer, 20 sixteenths (MIDI beats); put it before `cont`
  Without a start/cont token `start` is put first; without a final stop/quiet token `stop` is put last (--no-stop to keep the master running at the end).
  Master faults: --jitter MS (uniform +-MS on every clock's send time, seeded by --seed), --drop K/N (K of every N clocks are not sent, after the first 48),
  --drop-burst AT:LEN (LEN clocks from clock AT are not sent). The unit's notes (the OS-MIDISYNC test projects: one MIDI track, a note on every 16th named
  base + step) are matched to the clock that should have started them: a 16th is 6 clocks.
  Known limits: a run's note window opens 2 ms before its Start/Continue byte, so a previous run's note within ~2 ms before the byte can be taken as that run's first
  step; on a real rawmidi node a blocking write delays the reads (and their timestamps) -- the summary reports the per-run write time when a write stalls.

MTC-SEND MODE (OS-MTC B1; the unit follows the time code, MTC IN)
  The master sends quarter frames (`F1 0nnn dddd`, piece = q mod 8) and full frames (`F0 7F 7F 01 01 hh mm ss ff F7`) at absolute deadlines, as `sync` sends clocks, and listens on
  the same node for the notes of the test song (--song, default midisync-song: the note names the step; the master's clock at --start, default 01:00:00:00, is the song's start).
  The master's clock runs --rate times the host's (default 1: 1.001 is a master 0.1 % fast). Profile = comma-separated tokens, in order:
      at:01:00:00:00    a full frame, and the clock is placed there (the first token; without it `at:<--start>` is put first). A pre-roll is `at:00:59:58:00,run:...`
      run:30            quarter frames for 30 s, forward, the first one on the quarter-frame grid at least 4 ms after the full frame (at whatever piece comes)
      quiet:0.1         the clock runs and nothing is sent. Shorter than 3 frames: a drop-out the unit must hold over; longer: the master has stopped (the unit must be silent
                        within 4 frames and a block of the last quarter frame). After the last token the master stops and the tool listens --tail seconds (default 3): the stop test
      jump:01:00:20:00  a full frame while running, the clock goes there and the quarter frames go on from it (the unit must relocate: the time from the full frame to its first note)
      reverse:2         quarter frames run backward (pieces 7..0, the time falling) for 2 s; reported, not judged (the spec does not follow a reverse)
  --fps 24, 25, 29.97 (drop-frame) or 30 sets the rate code and the labels. Master faults: --jitter MS (uniform +-MS on every quarter frame's send time, seeded by --seed), --drop K/N
  (K of every N quarter frames are not sent, never in the first 16). --song none sends only (no listening). Exit code 0 when every check passes (the limits are MTC_SEND_LIMITS; --limit key=value).
"""
import argparse
import bisect
import glob
import json
import math
import os
import random
import select
import statistics
import subprocess
import sys
import tempfile
import threading
import time


def measure(dev, seconds, quiet=False, stop_event=None):
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    stamps = []
    events = []
    start = time.monotonic()
    try:
        while time.monotonic() - start < seconds and not (stop_event and stop_event.is_set()):
            ready, _, _ = select.select([fd], [], [], 0.05)
            if not ready:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            now = time.monotonic()
            if not data:  # a FIFO with no writer (yet, or any more) reads as end of file; a real MIDI node never does
                if stamps or events:
                    break
                time.sleep(0.01)
                continue
            for b in data:
                if b == 0xF8:
                    stamps.append(now)
                elif b in (0xFA, 0xFB, 0xFC, 0xFE, 0xFF):
                    events.append((now - start, b))
    finally:
        os.close(fd)
    result = {"clocks": len(stamps), "events": events}
    if len(stamps) >= 25:
        span = stamps[-1] - stamps[0]
        result["bpm"] = (len(stamps) - 1) / 24.0 / span * 60.0
        windows = []
        i = 0
        for w in range(int(span)):
            n = sum(1 for s in stamps if stamps[0] + w <= s < stamps[0] + w + 1)
            windows.append(n / 24.0 * 60.0)
        result["windows"] = windows
    if not quiet:
        names = {0xFA: "Start", 0xFB: "Continue", 0xFC: "Stop", 0xFE: "ActiveSense", 0xFF: "Reset"}
        print("clocks %d in %.1f s" % (len(stamps), seconds))
        for t, b in events[:20]:
            print("  %7.3f s  %s" % (t, names[b]))
        if "bpm" in result:
            print("mean BPM %.3f (first to last clock)" % result["bpm"])
            print("BPM per second window: %s" % " ".join("%.1f" % w for w in result["windows"]))
        else:
            print("fewer than 25 clocks: no BPM")
    return result


def send(dev, profile, start=False, stop=False, cont=False):
    fd = os.open(dev, os.O_WRONLY)
    try:
        if start:
            os.write(fd, b"\xFA")
        elif cont:
            os.write(fd, b"\xFB")
        t0 = time.perf_counter()
        t = t0
        total = 0
        for bpm, seconds in profile:
            interval = 60.0 / (24.0 * bpm)
            n = int(seconds / interval)
            for _ in range(n):
                while True:
                    left = t - time.perf_counter()
                    if left <= 0:
                        break
                    time.sleep(left - 0.0015 if left > 0.003 else 0)
                os.write(fd, b"\xF8")
                total += 1
                t += interval
        if stop:
            os.write(fd, b"\xFC")
    finally:
        os.close(fd)
    return total


def parse_profile(text):
    out = []
    for part in text.split(","):
        bpm, _, seconds = part.partition(":")
        out.append((float(bpm), float(seconds)))
    return out


def list_nodes():
    for node in sorted(glob.glob("/dev/snd/midiC*D*")):
        print(node)
    try:
        print(subprocess.run(["amidi", "-l"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10).stdout)
    except (OSError, subprocess.TimeoutExpired) as error:
        print("amidi: %s" % error)


# ======================================================================================================================================================
# OS-MIDISYNC: `sync`. The master, the reader and the analysis.
#
# Times are seconds on the master's own grid: 0 is the ideal time of clock 0 (the first clock the profile makes, whether it is sent or not), and the run's
# origin t0_ns is that instant on time.monotonic_ns(). `ideal[c]` is when clock c SHOULD go out, `sent[c]` when it did (None: never sent: dropped or quiet).
# The unit is asked to follow the ideal grid, not the jittered one: the master's jitter is a fault that the follower must absorb.
# ======================================================================================================================================================

CLOCKS_PER_STEP = 6      # 24 clocks a quarter note, a step is a 16th
STALE_CLOCKS = 48        # seq/midi_link.h kStaleClocks: the unit lets go of a master that has been quiet this many clock periods
SETTLE_CLOCKS = 480      # the window after a jump / ramp / burst / quiet that is judged separately (20 beats), and left out of the steady statistics
LOCK_STREAK = 8          # lock: the first run of this many consecutive steps all within the band of the offset

# The PASS thresholds (documented in MIDISYNC.md). Starting points of OS-MIDISYNC phase 1: the steady ones are the maintainer's brief; the rest are
# judgements to be tuned from the first measurements on the unit. Override one with --limit key=value.
LIMITS = {
    "std_ms": 1.5,             # steady, after lock: standard deviation of the note-to-clock error
    "worst_ms": 4.0,           # steady, after lock: the largest |error - mean|
    "drift_ms": 0.5,           # steady, after lock: |slope of the error x the span|; the effective limit is max(this, 3 * phase std * sqrt(12 / steps)) so a short
                               # noisy run is not failed by chance (the slope noise of n points is about sigma * sqrt(12/n) over the span)
    "offset_ms": 0.0,          # steady: |absolute offset|; 0 = automatic: half a step at the run's tempo (a unit a whole step late must fail); a number = an absolute ms limit
    "missing": 0,              # expected steps that never sounded
    "extra": 0,                # notes that match no step (duplicates, wrong notes, notes far from their step)
    "lock_steps": 12,          # steps from the run's first to the start of the first good streak (3 beats)
    "min_steps": 16,           # a run with fewer steps after lock proves nothing
    "band_ms": 4.0,            # what "within the band" means for lock and for settling
    "jump_settle_ms": 3000.0,  # a tempo jump: time until the error stays in the band
    "burst_settle_ms": 2000.0,  # a burst of dropped clocks: the same
    "ramp_worst_ms": 8.0,      # a tempo ramp: largest |error - offset| (the loop's constant lag on a ramp is about 2 ms at 100 to 180 BPM in 30 s)
    "quiet_late_ms": 100.0,    # a master that goes quiet: the last note-on may come this long after 48 clock periods past the last clock
    "stop_silence_ms": 50.0,   # Stop: the note-offs are out within this long
    "stop_late_noteon_ms": 5.0,  # Stop: a note-on later than this after the byte is a failure
    "offset_spread_ms": 4.0,   # the offsets of several runs of one test must agree (Start, Stop, Continue)
}

TRANSPORT_BYTES = {"start": b"\xFA", "cont": b"\xFB", "stop": b"\xFC"}


def parse_sync_profile(text):
    """Tokens: ('seg', b0, b1, seconds), ('quiet', seconds), ('start',), ('cont',), ('stop',), ('spp', n)."""
    tokens = []
    for raw in text.split(","):
        part = raw.strip().lower()
        if not part:
            continue
        if part in TRANSPORT_BYTES:
            tokens.append((part,))
        elif part.startswith("spp:"):
            n = int(part[4:])
            if not 0 <= n < 16384:
                raise ValueError("spp:N takes 0..16383")
            tokens.append(("spp", n))
        elif part.startswith("quiet:"):
            tokens.append(("quiet", float(part[6:])))
        elif part.startswith("ramp:"):
            rng, _, seconds = part[5:].partition(":")
            b0, _, b1 = rng.partition("-")
            tokens.append(("seg", float(b0), float(b1), float(seconds)))
        else:
            bpm, _, seconds = part.partition(":")
            if not seconds:
                raise ValueError("token %r is not BPM:seconds" % part)
            tokens.append(("seg", float(bpm), float(bpm), float(seconds)))
    for t in tokens:
        if t[0] == "seg" and (t[1] < 20 or t[2] < 20 or t[1] > 400 or t[2] > 400 or t[3] <= 0):
            raise ValueError("tempo outside 20..400 BPM or no duration in %r" % (t,))
    if not any(t[0] in ("seg", "quiet") for t in tokens):
        raise ValueError("the profile has no timed segment")
    return tokens


class Plan:
    """The master's schedule: `events` in send order as [deadline_s, bytes, kind, index], and what the analysis needs to know about the grid."""

    def __init__(self):
        self.events = []
        self.ideal = []       # per clock index: the ideal time
        self.bpm_at = []      # per clock index: the tempo the grid had there
        self.sent = []        # per clock index: the actual send time, None while unsent
        self.will_send = []   # per clock index: planned to be sent
        self.transports = []  # dicts: kind, t (planned, then actual), c_next, spp, ev (index into events)
        self.dist = []        # disturbances: kind jump | ramp | quiet | burst, c_from, c_to (exclusive), plus details
        self.tokens = []
        self.jitter_ms = 0.0
        self.seed = 1


def build_plan(tokens, jitter_ms=0.0, seed=1, drop=None, burst=None, lead_ms=2.0, auto_transport=True):
    toks = list(tokens)
    if auto_transport:
        if not any(t[0] in ("start", "cont") for t in toks):
            toks.insert(0, ("start",))
        if toks[-1][0] not in ("stop", "quiet"):
            toks.append(("stop",))
    plan = Plan()
    plan.tokens = toks
    plan.jitter_ms = jitter_ms
    plan.seed = seed
    rng = random.Random(seed)
    drop_rng = random.Random(seed + 1)
    lead = lead_ms / 1000.0
    jitter = jitter_ms / 1000.0
    window_drops = {}
    pending = []
    state = {"t": 0.0, "c": 0}

    def flush():
        n = len(pending)
        for i, (kind, arg) in enumerate(pending):
            ts = state["t"] - lead * (n - i)
            data = TRANSPORT_BYTES[kind] if kind in TRANSPORT_BYTES else bytes([0xF2, arg & 0x7F, (arg >> 7) & 0x7F])
            plan.events.append([ts, data, kind, state["c"]])
            plan.transports.append({"kind": kind, "t": ts, "c_next": state["c"], "spp": arg if kind == "spp" else None, "ev": len(plan.events) - 1})
        pending.clear()

    def emit_clock(bpm_now, quiet):
        t, c = state["t"], state["c"]
        plan.ideal.append(t)
        plan.bpm_at.append(bpm_now)
        send = not quiet
        if send and burst is not None and burst[0] <= c < burst[0] + burst[1]:
            send = False
        if send and drop is not None and c >= 48:
            k, n = drop
            w = c // n
            if w not in window_drops:
                window_drops[w] = set(drop_rng.sample(range(n), min(k, n)))
            if c % n in window_drops[w]:
                send = False
        j = rng.uniform(-jitter, jitter) if jitter > 0.0 else 0.0
        plan.will_send.append(send)
        if send:
            plan.events.append([t + j, b"\xF8", "clock", c])
        state["c"] = c + 1
        state["t"] = t + 60.0 / (24.0 * bpm_now)

    prev_bpm = None
    for tok in toks:
        kind = tok[0]
        if kind in ("start", "cont", "stop", "spp"):
            if kind in ("start", "cont"):
                prev_bpm = None  # a new run at another tempo is not a jump of the follower
            pending.append((kind, tok[1] if kind == "spp" else None))
            continue
        flush()
        if kind == "seg":
            _, b0, b1, sec = tok
            if prev_bpm is not None and abs(b0 - prev_bpm) / prev_bpm > 0.005:
                plan.dist.append({"kind": "jump", "c_from": state["c"], "c_to": state["c"], "from_bpm": prev_bpm, "to_bpm": b0})
            end = state["t"] + sec
            ts = state["t"]
            c_from = state["c"]
            while state["t"] < end - 1e-9:
                emit_clock(b0 + (b1 - b0) * min(1.0, (state["t"] - ts) / sec), False)
            if b0 != b1:
                plan.dist.append({"kind": "ramp", "c_from": c_from, "c_to": state["c"], "from_bpm": b0, "to_bpm": b1})
            prev_bpm = b1
        else:  # quiet
            end = state["t"] + tok[1]
            c_from = state["c"]
            while state["t"] < end - 1e-9:
                emit_clock(prev_bpm if prev_bpm is not None else 120.0, True)
            plan.dist.append({"kind": "quiet", "c_from": c_from, "c_to": state["c"]})
    flush()
    if burst is not None:
        plan.dist.append({"kind": "burst", "c_from": burst[0], "c_to": burst[0] + burst[1]})
    plan.dist.sort(key=lambda d: d["c_from"])
    # the jitter never reorders: a byte is never sent before the one that precedes it
    prev = -1e9
    for ev in plan.events:
        if ev[0] < prev + 50e-6:
            ev[0] = prev + 50e-6
        prev = ev[0]
    plan.sent = [None] * len(plan.ideal)
    return plan


class MidiParser:
    """Timestamps what comes in: ('on' | 'off' | 'cc' | 'other' | 'clock' | 'start' | 'cont' | 'stop' | 'rt', t, channel, d1, d2). Running status, real-time bytes anywhere."""

    RT = {0xF8: "clock", 0xFA: "start", 0xFB: "cont", 0xFC: "stop"}

    def __init__(self):
        self.status = 0
        self.need = 0
        self.data = []
        self.sysex = False
        self.events = []

    def feed(self, chunk, t):
        for b in chunk:
            if b >= 0xF8:
                self.events.append((self.RT.get(b, "rt"), t, 0, 0, 0))
            elif b >= 0x80:
                if b == 0xF0:
                    self.sysex = True
                    self.status = 0
                elif b == 0xF7:
                    self.sysex = False
                elif b >= 0xF0:
                    self.status = 0
                    self.sysex = False
                else:
                    self.sysex = False
                    self.status = b
                    self.need = 1 if (b & 0xF0) in (0xC0, 0xD0) else 2
                    self.data = []
            elif self.status and not self.sysex:
                self.data.append(b)
                if len(self.data) == self.need:
                    hi, ch = self.status & 0xF0, self.status & 0x0F
                    d1 = self.data[0]
                    d2 = self.data[1] if self.need == 2 else 0
                    kind = "on" if hi == 0x90 and d2 > 0 else "off" if hi in (0x80, 0x90) else "cc" if hi == 0xB0 else "other"
                    self.events.append((kind, t, ch, d1, d2))
                    self.data = []


RT_PRIO = 20  # the master's SCHED_FIFO priority when the OS allows it (--rtprio)


def enter_realtime(prio=RT_PRIO, max_prio=None):
    """The master asks for SCHED_FIFO at `prio`, never needing root and never changing a limit: a PermissionError is retried once at the user's RLIMIT_RTPRIO (`max_prio`
    overrides it, for the self-test) when that is lower and above 0. Returns {"fifo": bool, "prio": int, "sched": the words for the report line}."""
    if prio <= 0:
        return {"fifo": False, "prio": 0, "sched": "SCHED_OTHER (--rtprio 0)"}
    if not hasattr(os, "sched_setscheduler") or not hasattr(os, "SCHED_FIFO"):
        return {"fifo": False, "prio": 0, "sched": "SCHED_OTHER (no SCHED_FIFO on this OS)"}
    if max_prio is None:
        try:
            import resource
            max_prio = resource.getrlimit(resource.RLIMIT_RTPRIO)[0]
            if max_prio == resource.RLIM_INFINITY:
                max_prio = prio
        except (ImportError, AttributeError, OSError, ValueError):
            max_prio = 0
    tries = [prio]
    if 0 < max_prio < prio:
        tries.append(max_prio)
    for p in tries:
        try:
            os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(p))
            return {"fifo": True, "prio": p, "sched": "SCHED_FIFO %d" % p}
        except (OSError, PermissionError):
            continue
    return {"fifo": False, "prio": 0, "sched": "SCHED_OTHER (no rtprio)"}


def leave_realtime():
    try:
        os.sched_setscheduler(0, os.SCHED_OTHER, os.sched_param(0))  # lowering is always allowed: the rest of the process (the analysis, a self-test) runs as before
    except (OSError, AttributeError, PermissionError):
        pass


def run_master(plan, wfd, rfd, tail_s=3.0, lead_in_s=0.5, rtprio=RT_PRIO):
    """Sends the plan on `wfd` at absolute deadlines and reads `rfd` in the same loop. Returns (reader events, info); fills plan.sent and the transports' actual times."""
    ns = time.monotonic_ns
    rt = enter_realtime(rtprio)
    info = {"sched_fifo": rt["fifo"], "rtprio": rt["prio"], "sched": rt["sched"]}
    parser = MidiParser()
    eof = [rfd is None]
    t0 = ns() + int(lead_in_s * 1e9)

    def drain():
        while not eof[0]:
            try:
                data = os.read(rfd, 4096)
            except BlockingIOError:
                return
            stamp = ns()
            if not data:
                eof[0] = True  # a FIFO whose writer is gone (the self-test); a MIDI node never ends
                return
            parser.feed(data, (stamp - t0) / 1e9)

    def wait_until(deadline):
        while True:
            rem = deadline - ns()
            if rem <= 0:
                return
            if rem > 1_500_000:
                wait = (rem - 1_000_000) / 1e9
                if eof[0]:
                    time.sleep(wait)
                elif select.select([rfd], [], [], wait)[0]:
                    drain()
            else:
                drain()  # the last millisecond is a spin (and still listens)

    import gc
    gc.disable()
    late = []
    writes = []  # (ms the write took, the event): on a real rawmidi node a blocking write delays the reads (and their timestamps) too (known limit, see the help above)
    try:
        sent_ns = []
        for ev in plan.events:
            deadline = t0 + int(ev[0] * 1e9)
            wait_until(deadline)
            before = ns()
            os.write(wfd, ev[1])
            writes.append(((ns() - before) / 1e6, ev))
            sent_ns.append(before)
            late.append((before - deadline) / 1e6)
        end = ns() + int(tail_s * 1e9)
        wait_until(end)
        drain()
    finally:
        gc.enable()
        if rt["fifo"]:
            leave_realtime()
    for ev, s in zip(plan.events, sent_ns):
        if ev[2] == "clock":
            plan.sent[ev[3]] = (s - t0) / 1e9
    for tr in plan.transports:
        tr["t"] = (sent_ns[tr["ev"]] - t0) / 1e9
    clock_late = [l for ev, l in zip(plan.events, late) if ev[2] == "clock"]
    if clock_late:
        srt = sorted(clock_late)
        info["lateness_ms"] = {"mean": statistics.fmean(clock_late), "p99": srt[int(0.99 * (len(srt) - 1))], "max": srt[-1], "over_1ms": sum(1 for l in clock_late if l > 1.0)}
    per_run = []
    for r in _build_runs(plan):
        ws = [w for w, ev in writes if ev[2] == "clock" and r["c0"] <= ev[3] < (r["c_end"] if r["c_end"] is not None else len(plan.ideal))]
        if ws:
            per_run.append({"kind": r["kind"], "reason": r["reason"], "mean_ms": round(statistics.fmean(ws), 3), "max_ms": round(max(ws), 3)})
    info["write_lateness_per_run"] = per_run
    info["relayed_clocks"] = sum(1 for e in parser.events if e[0] == "clock")
    return parser.events, info


# ---- analysis -------------------------------------------------------------------------------------------------------------------------------------

def _nearest(seq, t):
    i = bisect.bisect_left(seq, t)
    if i == 0:
        return 0
    if i == len(seq):
        return len(seq) - 1
    return i if seq[i] - t < t - seq[i - 1] else i - 1


def _build_runs(plan):
    runs = []
    cur = None
    pend = None
    for tr in plan.transports:
        k = tr["kind"]
        if k == "spp":
            pend = tr["spp"]
        elif k in ("start", "cont"):
            if cur is not None:
                cur.update(t_end=tr["t"], c_end=tr["c_next"], reason="restart")
            cur = {"kind": k, "t_begin": tr["t"], "c0": tr["c_next"], "s0_exp": 0 if k == "start" else pend, "spp": pend if k == "cont" else None,
                   "t_end": None, "c_end": None, "reason": "end"}
            runs.append(cur)
            pend = None
        elif k == "stop":
            if cur is not None:
                cur.update(t_end=tr["t"], c_end=tr["c_next"], reason="stop")
                cur = None
            pend = None
    for r in runs:
        if r["t_end"] is None:
            r["c_end"] = len(plan.ideal)
            r["t_end"] = plan.ideal[-1] if plan.ideal else r["t_begin"]
        for d in plan.dist:  # a master that goes quiet inside a run ends it: the unit lets go of it
            if d["kind"] == "quiet" and r["c0"] < d["c_from"] < r["c_end"]:
                r.update(c_end=d["c_from"], reason="quiet")
                last = max((plan.sent[c] for c in range(d["c_from"] - 1, r["c0"] - 1, -1) if plan.sent[c] is not None), default=plan.ideal[d["c_from"] - 1])
                r["t_end"] = last
                r["quiet"] = d
                break
    return runs


def _linfit(xs, ys):
    n = len(xs)
    if n < 3:
        return 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0.0:
        return 0.0
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx


def _windows(plan, run):
    """The stretches of one run that are judged on their own (a jump, a ramp, a burst) and left out of its steady statistics."""
    return [(d["c_from"], d["c_to"] + SETTLE_CLOCKS) for d in plan.dist if run["c0"] <= d["c_from"] < run["c_end"]]


def _analyze_run(plan, run, events_on, base, cycle, lim, prev_abs=None):
    """`prev_abs`: the absolute step the previous run ended on (None before the first run); a bare Continue must resume on the step after it."""
    ideal = plan.ideal
    c0 = run["c0"]
    band = lim["band_ms"] / 1000.0
    if run["reason"] == "quiet":
        d = run["quiet"]
        period = ideal[d["c_from"]] - ideal[d["c_from"] - 1]
        win_end = run["t_end"] + STALE_CLOCKS * period + 0.5
    elif run["reason"] in ("stop", "restart"):
        win_end = run["t_end"]  # a Stop or a restart ends the run at the byte: the notes after it belong to the next run (or are late)
    else:
        win_end = run["t_end"] + 0.5
    # the window opens 2 ms early on purpose (the transport byte goes out before the next clock): a previous run's note within ~2 ms before the byte can be taken as this
    # run's first note (known limit, see the SYNC MODE help above)
    notes = [(t, n) for (t, n) in events_on if run["t_begin"] - 0.002 <= t < win_end]
    res = {"kind": run["kind"], "c0": c0, "reason": run["reason"], "notes": len(notes)}
    if run["spp"] is not None:
        res["spp"] = run["spp"]
    # which step comes first: Start 0, Continue after an SPP that, anything else is read off the first note -- except a bare Continue, which must resume on the step after
    # the previous run's last step (otherwise a unit that restarts from step 0 on Continue passes)
    s0 = run["s0_exp"]
    first_ok = True
    foreign = 0
    valid = [(t, n) for (t, n) in notes if 0 <= n - base < cycle]
    foreign += len(notes) - len(valid)
    if not valid:
        res.update(steps_expected=0, missing=0, extra=foreign, lock_steps=None, error="no note of the project heard in this run")
        res["_n_exp"] = 0
        return res, []
    c_first = _nearest(ideal, valid[0][0])
    n_first = max(0, int(round((c_first - c0) / float(CLOCKS_PER_STEP))))
    s0_obs = (valid[0][1] - base - n_first) % cycle
    res["first_step_heard"] = s0_obs
    if s0 is None:
        if prev_abs is not None:
            exp = (prev_abs + 1) % cycle
            res["first_step_expected"] = exp
            first_ok = (s0_obs == exp)
        else:
            res["first_step_expected"] = None
        s0 = s0_obs  # analyse what was played; the wrong first step is its own check
    else:
        res["first_step_expected"] = s0 % cycle
        first_ok = (s0_obs == s0 % cycle)
        s0 = s0_obs  # analyse what was played; the wrong first step is its own check
    res["first_step_ok"] = first_ok
    errs = {}
    extra = foreign
    for t, note in valid:
        v = note - base
        c_n = _nearest(ideal, t)
        n_t = (c_n - c0) / float(CLOCKS_PER_STEP)
        r = (v - s0) % cycle
        n = r + cycle * int(round((n_t - r) / cycle))
        c = c0 + CLOCKS_PER_STEP * n
        if n < 0 or c >= len(ideal):
            extra += 1
            continue
        if n in errs:
            extra += 1
            continue
        errs[n] = t - ideal[c]
    wins = _windows(plan, run)

    def steady(n):
        c = c0 + CLOCKS_PER_STEP * n
        return not any(a <= c < b for a, b in wins)

    steady_vals = [e for n, e in errs.items() if steady(n)]
    pool = steady_vals if steady_vals else list(errs.values())
    off = statistics.median(pool)
    for n in list(errs):  # a note that is not within half a step of its step is no note of it
        c = c0 + CLOCKS_PER_STEP * n
        half = 0.5 * (ideal[min(c + CLOCKS_PER_STEP, len(ideal) - 1)] - ideal[c]) if c + 1 < len(ideal) else 0.05
        if abs(errs[n] - off) > half:
            del errs[n]
            extra += 1
    # the steps that had to sound: a Stop, a restart (a Start/Continue cutting the run) or a quiet master ends the run at the byte, so only a step whose ideal time is
    # within 25 ms of the end may be missing; a run that runs to the end of the plan must sound every step
    n_max = -1
    n = 0
    while True:
        c = c0 + CLOCKS_PER_STEP * n
        if c >= run["c_end"]:
            break
        if run["reason"] in ("stop", "restart", "quiet") and ideal[c] > run["t_end"] - 0.025:
            break
        elif run["reason"] not in ("stop", "restart", "quiet") and c > run["c_end"] - 12:
            break
        n_max = n
        n += 1
    missing_list = [n for n in range(n_max + 1) if n not in errs]
    res["steps_expected"] = n_max + 1
    res["_n_exp"] = n_max + 1
    res["_last_abs"] = s0 + n_max  # the absolute step this run ended on (for the next bare Continue)
    res["missing"] = len(missing_list)
    res["missing_steps"] = missing_list[:20]
    res["extra"] = extra
    res["offset_ms"] = off * 1000.0
    # lock: the first streak of LOCK_STREAK consecutive steady steps within the band of the offset
    good = {n for n, e in errs.items() if steady(n) and abs(e - off) <= band}
    lock_n = None
    for n in sorted(good):
        if all((n + k) in good for k in range(LOCK_STREAK)):
            lock_n = n
            break
    res["lock_steps"] = lock_n
    t_first_clock = plan.sent[c0] if c0 < len(plan.sent) and plan.sent[c0] is not None else ideal[c0]
    if lock_n is not None and lock_n in errs:
        res["lock_ms"] = (ideal[c0 + CLOCKS_PER_STEP * lock_n] + errs[lock_n] - t_first_clock) * 1000.0
    res["start_to_first_note_ms"] = (valid[0][0] - run["t_begin"]) * 1000.0
    res["missing_before_lock"] = sum(1 for n in missing_list if lock_n is not None and n < lock_n)
    after = sorted((n, e) for n, e in errs.items() if lock_n is not None and n >= lock_n and steady(n))
    res["steps_after_lock"] = len(after)
    series = []
    if after:
        ys = [e for _, e in after]
        xs = [ideal[c0 + CLOCKS_PER_STEP * n] for n, _ in after]
        mean = sum(ys) / len(ys)
        res["phase_mean_ms"] = mean * 1000.0
        res["phase_std_ms"] = statistics.pstdev(ys) * 1000.0
        res["phase_worst_ms"] = max(abs(y - mean) for y in ys) * 1000.0
        res["phase_min_ms"] = min(ys) * 1000.0
        res["phase_max_ms"] = max(ys) * 1000.0
        slope = _linfit(xs, ys)  # seconds of error per second
        res["drift_ms_per_min"] = slope * 60.0 * 1000.0
        res["drift_ms"] = slope * (xs[-1] - xs[0]) * 1000.0
        res["span_s"] = xs[-1] - xs[0]
    series = [(ideal[c0 + CLOCKS_PER_STEP * n], errs[n] * 1000.0) for n in sorted(errs)]
    res["_errs"] = errs
    res["_off"] = off
    return res, series


def _disturbances(plan, runs_res, runs, lim):
    ideal = plan.ideal
    band = lim["band_ms"] / 1000.0
    out = []
    for d in plan.dist:
        if d["kind"] == "quiet":
            continue
        rec = {"kind": d["kind"], "c_from": d["c_from"], "c_to": d["c_to"]}
        for k in ("from_bpm", "to_bpm"):
            if k in d:
                rec[k] = d[k]
        c_hi = d["c_to"] + SETTLE_CLOCKS
        best = None
        for run, rr in zip(runs, runs_res):
            if "_errs" not in rr or not (run["c0"] <= d["c_from"] < run["c_end"]):
                continue
            c0 = run["c0"]
            steps = sorted(n for n in rr["_errs"] if d["c_from"] <= c0 + CLOCKS_PER_STEP * n < c_hi)
            last_c = min(c_hi, run["c_end"])
            n_last = (last_c - 1 - c0) // CLOCKS_PER_STEP
            devs = {n: abs(rr["_errs"][n] - rr["_off"]) for n in steps}
            best = (c0, steps, devs, n_last, run["c_end"] >= c_hi or run["reason"] == "end")
            break
        if best is None or not best[1]:
            rec["error"] = "no note in the window"
            out.append(rec)
            continue
        c0, steps, devs, n_last, full = best
        rec["max_dev_ms"] = max(devs.values()) * 1000.0
        bad = [n for n in steps if devs[n] > band]
        if d["kind"] == "ramp":
            tail = [n for n in steps if c0 + CLOCKS_PER_STEP * n >= (d["c_from"] + d["c_to"]) // 2 and c0 + CLOCKS_PER_STEP * n < d["c_to"]]
            if tail:
                rec["lag_ms"] = statistics.fmean(plan_err for plan_err in (best_err(runs_res, runs, d, n) for n in tail)) * 1000.0
        if bad:
            lastbad = max(bad)
            rec["settle_ms"] = (ideal[c0 + CLOCKS_PER_STEP * lastbad] - ideal[d["c_from"]]) * 1000.0
            rec["settled"] = lastbad < n_last - LOCK_STREAK + 1
        else:
            rec["settle_ms"] = 0.0
            rec["settled"] = True
        rec["window_complete"] = bool(full)  # information only: a window cut short by the end of the run passes when every step heard in it is in the band
        out.append(rec)
    return out


def best_err(runs_res, runs, d, n):
    for run, rr in zip(runs, runs_res):
        if "_errs" in rr and run["c0"] <= d["c_from"] < run["c_end"] and n in rr["_errs"]:
            return rr["_errs"][n] - rr["_off"]
    return 0.0


def _analyze_stops(plan, events, lim):
    out = []
    ons = [(e[1], e[2], e[3]) for e in events if e[0] == "on"]
    offs = [(e[1], e[2], e[3]) for e in events if e[0] == "off"]
    alloff = [e[1] for e in events if e[0] == "cc" and e[3] in (120, 123)]
    trs = plan.transports
    for i, tr in enumerate(trs):
        if tr["kind"] != "stop":
            continue
        t = tr["t"]
        nxt = min([x["t"] for x in trs[i + 1:] if x["kind"] in ("start", "cont")] + [t + 3.0])
        late = [x[0] for x in ons if t < x[0] < nxt]
        closing = [x[0] for x in offs if t < x[0] < min(nxt, t + 2.0)] + [x for x in alloff if t < x < min(nxt, t + 2.0)]
        rec = {"t": t, "notes_after_ms": [round((x - t) * 1000.0, 2) for x in late[:5]], "late_noteons": sum(1 for x in late if x - t > lim["stop_late_noteon_ms"] / 1000.0)}
        if closing:
            rec["silence_ms"] = (max(closing + late) - t) * 1000.0
        # notes still sounding one second after the Stop (or at the next Start); the sounding set is built from the previous transport byte, not from the capture start,
        # so a note-off of an earlier run cannot cancel a note of this one
        probe = min(nxt, t + 1.0)
        t_prev = max((x["t"] for x in trs[:i]), default=0.0)
        sounding = set()
        for ev in sorted([(a, 1, ch, nn) for a, ch, nn in ons if t_prev < a <= probe] + [(a, 0, ch, nn) for a, ch, nn in offs if t_prev < a <= probe]):
            if ev[1]:
                sounding.add((ev[2], ev[3]))
            else:
                sounding.discard((ev[2], ev[3]))
        if alloff and any(t < x <= probe for x in alloff):
            sounding.clear()
        rec["hung_notes"] = len(sounding)
        out.append(rec)
    return out


def _analyze_quiet(plan, runs, events, lim):
    out = []
    ons = [e[1] for e in events if e[0] == "on"]
    offs = [e[1] for e in events if e[0] == "off"] + [e[1] for e in events if e[0] == "cc" and e[3] in (120, 123)]
    for d in plan.dist:
        if d["kind"] != "quiet":
            continue
        c_from, c_to = d["c_from"], d["c_to"]
        last = max((plan.sent[c] for c in range(c_from - 1, -1, -1) if plan.sent[c] is not None), default=None)
        if last is None:
            continue
        period = plan.ideal[c_from] - plan.ideal[c_from - 1] if c_from > 0 else 0.5 / 24.0
        resume = next((plan.sent[c] for c in range(c_to, len(plan.sent)) if plan.sent[c] is not None), None)
        horizon = last + STALE_CLOCKS * period
        stop = resume if resume is not None else 1e9
        after = [t for t in ons if last + 0.001 < t < stop]
        rec = {"last_clock_t": last, "resume_t": resume, "stale_horizon_s": STALE_CLOCKS * period, "notes_after_last_clock": len(after)}
        rec["last_note_after_ms"] = (max(after) - last) * 1000.0 if after else None
        rec["late_ms"] = (max(after) - horizon) * 1000.0 if after else 0.0
        end_marks = [t for t in offs if t >= (max(after) if after else last) and t < stop]
        rec["closed"] = bool(end_marks)  # a note-off or an all-notes-off after the last note-on (or after the last clock when nothing played on)
        rec["release_ms"] = (max(end_marks) - last) * 1000.0 if end_marks else None
        if resume is not None:
            windows = [(r["t_begin"], r["t_end"] + 0.5) for r in runs]
            stray = [t for t in ons if t > resume + 0.05 and not any(a <= t < b for a, b in windows)]
            rec["restart_notes_without_start"] = len(stray)
        out.append(rec)
    return out


def analyze(plan, events, base=36, cycle=16, channel=1, limits=None):
    lim = dict(LIMITS)
    if limits:
        lim.update(limits)
    ons = [(e[1], e[3]) for e in events if e[0] == "on" and e[2] == channel - 1]
    runs = _build_runs(plan)
    runs_res = []
    series = []
    prev_abs = None  # the absolute step the previous run ended on (a bare Continue must resume after it)
    for run in runs:
        rr, s = _analyze_run(plan, run, ons, base, cycle, lim, prev_abs)
        runs_res.append(rr)
        series.append(s)
        if "_last_abs" in rr:
            prev_abs = rr["_last_abs"]
    dists = _disturbances(plan, runs_res, runs, lim)
    stops = _analyze_stops(plan, events, lim)
    quiets = _analyze_quiet(plan, runs, events, lim)
    checks = []

    def check(name, value, limit, ok, scope=""):
        checks.append({"name": name, "scope": scope, "value": None if value is None else round(value, 3) if isinstance(value, float) else value, "limit": limit, "ok": bool(ok)})

    for i, rr in enumerate(runs_res):
        sc = "run %d (%s)" % (i + 1, rr["kind"])
        if "error" in rr:
            check("notes heard", 0, ">0", False, sc)
            continue
        check("first step", rr.get("first_step_heard"), rr.get("first_step_expected"), rr["first_step_ok"], sc)
        check("missing steps", rr["missing"], "<=%d" % lim["missing"], rr["missing"] <= lim["missing"], sc)
        check("extra notes", rr["extra"], "<=%d" % lim["extra"], rr["extra"] <= lim["extra"], sc)
        check("lock (steps)", rr["lock_steps"], "<=%d" % lim["lock_steps"], rr["lock_steps"] is not None and rr["lock_steps"] <= lim["lock_steps"], sc)
        # the absolute offset: a unit a whole step late has a fine phase spread but a whole step of offset; the limit is half a step at the run's tempo unless --limit says otherwise
        bpm = plan.bpm_at[min(rr["c0"], len(plan.bpm_at) - 1)]
        off_lim = lim["offset_ms"] if lim["offset_ms"] > 0 else 7.5 / bpm * 1000.0  # half a step = 3 clocks = 7.5 / BPM s
        rr["offset_limit_ms"] = round(off_lim, 3)
        check("offset (ms)", rr["offset_ms"], "<%.1f" % off_lim, abs(rr["offset_ms"]) < off_lim, sc)
        if rr.get("steps_after_lock", 0) < lim["min_steps"]:
            check("steps after lock", rr.get("steps_after_lock", 0), ">=%d" % lim["min_steps"], False, sc)
        else:
            check("phase std (ms)", rr["phase_std_ms"], "<%.2f" % lim["std_ms"], rr["phase_std_ms"] < lim["std_ms"], sc)
            check("phase worst (ms)", rr["phase_worst_ms"], "<%.2f" % lim["worst_ms"], rr["phase_worst_ms"] < lim["worst_ms"], sc)
            # the drift limit grows with the run's own noise and shrinks with its length: the slope noise of n points over the span is about sigma * sqrt(12/n), so a short
            # noisy run is judged against 3 sigma * sqrt(12/n), never below the base limit -- and the limit used is reported
            n_after = rr["steps_after_lock"]
            drift_lim = max(lim["drift_ms"], 3.0 * rr["phase_std_ms"] * math.sqrt(12.0 / n_after))
            rr["drift_limit_ms"] = round(drift_lim, 3)
            check("drift over the run (ms)", rr["drift_ms"], "|x|<%.2f" % drift_lim, abs(rr["drift_ms"]) < drift_lim, sc)
    offsets = [rr["offset_ms"] for rr in runs_res if "offset_ms" in rr]
    if len(offsets) > 1:
        spread = max(offsets) - min(offsets)
        check("offset spread between runs (ms)", spread, "<%.2f" % lim["offset_spread_ms"], spread < lim["offset_spread_ms"], "all runs")
    for d in dists:
        sc = "%s at clock %d" % (d["kind"], d["c_from"])
        if "error" in d:
            check("notes in the window", 0, ">0", False, sc)
            continue
        if d["kind"] == "ramp":
            check("ramp worst |error - offset| (ms)", d["max_dev_ms"], "<%.1f" % lim["ramp_worst_ms"], d["max_dev_ms"] < lim["ramp_worst_ms"], sc)
        else:
            key = "jump_settle_ms" if d["kind"] == "jump" else "burst_settle_ms"
            ok = d["settled"] and d["settle_ms"] <= lim[key]
            check("settle (ms)" + ("" if d["window_complete"] else ", window cut short"), d["settle_ms"], "<=%.0f" % lim[key], ok, sc)
    for i, s in enumerate(stops):
        sc = "stop %d" % (i + 1)
        check("note-ons after Stop", s["late_noteons"], 0, s["late_noteons"] == 0, sc)
        check("hung notes after Stop", s["hung_notes"], 0, s["hung_notes"] == 0, sc)
        if "silence_ms" in s:
            check("stop to silence (ms)", s["silence_ms"], "<=%.0f" % lim["stop_silence_ms"], s["silence_ms"] <= lim["stop_silence_ms"], sc)
    for q in quiets:
        sc = "quiet master"
        check("last note-on past the stale horizon (ms)", q["late_ms"], "<=%.0f" % lim["quiet_late_ms"], q["late_ms"] <= lim["quiet_late_ms"], sc)
        check("notes closed after the master went quiet", q["closed"], True, q["closed"], sc)
    result = {
        "runs": [{k: (round(v, 3) if isinstance(v, float) else v) for k, v in rr.items() if not k.startswith("_")} for rr in runs_res],
        "disturbances": [{k: (round(v, 3) if isinstance(v, float) else v) for k, v in d.items()} for d in dists],
        "stops": [{k: (round(v, 3) if isinstance(v, float) else v) for k, v in s.items()} for s in stops],
        "quiet": [{k: (round(v, 3) if isinstance(v, float) else v) for k, v in q.items()} for q in quiets],
        "checks": checks,
        "pass": all(c["ok"] for c in checks) and bool(checks),
        "limits": lim,
    }
    result["_series"] = series
    return result


def summary(report):
    lines = []
    m = report["master"]
    lines.append("profile %s | %d clocks planned, %d sent, %d not sent | jitter +-%.2f ms | master lateness mean %.3f p99 %.3f max %.3f ms (%d clocks over 1 ms) | master %s" % (
        report["profile"], m["clocks_planned"], m["clocks_sent"], m["clocks_planned"] - m["clocks_sent"], report["jitter_ms"],
        m.get("lateness_ms", {}).get("mean", 0.0), m.get("lateness_ms", {}).get("p99", 0.0), m.get("lateness_ms", {}).get("max", 0.0),
        m.get("lateness_ms", {}).get("over_1ms", 0), m.get("sched") or ("SCHED_FIFO" if m.get("sched_fifo") else "SCHED_OTHER (no rtprio)")))
    for i, w in enumerate(m.get("write_lateness_per_run", [])):
        if w["max_ms"] > 1.0:  # a blocking write on a real rawmidi node: the note reads (and their timestamps) are delayed by it
            lines.append("  run %d (%s, ends by %s): a write to the node took up to %.2f ms (mean %.2f)" % (i + 1, w["kind"], w["reason"], w["max_ms"], w["mean_ms"]))
    for i, rr in enumerate(report["runs"]):
        if "error" in rr:
            lines.append("run %d (%s): %s" % (i + 1, rr["kind"], rr["error"]))
            continue
        lines.append("run %d (%s, ends by %s): %d steps expected, missing %d, extra %d | first step %s | start->first note %.1f ms | lock %s steps (%s ms) | offset %.2f ms (limit %.1f ms)" % (
            i + 1, rr["kind"], rr["reason"], rr["steps_expected"], rr["missing"], rr["extra"], rr.get("first_step_heard"), rr.get("start_to_first_note_ms", 0.0),
            rr.get("lock_steps"), "%.0f" % rr["lock_ms"] if "lock_ms" in rr else "-", rr.get("offset_ms", 0.0), rr.get("offset_limit_ms", 0.0)))
        if "phase_std_ms" in rr:
            lines.append("      after lock (%d steps, %.1f s): phase mean %.2f ms, std %.3f ms, worst %.3f ms (range %.2f..%.2f), drift %.3f ms over the run (%.3f ms/min, limit %.2f ms)" % (
                rr["steps_after_lock"], rr["span_s"], rr["phase_mean_ms"], rr["phase_std_ms"], rr["phase_worst_ms"], rr["phase_min_ms"], rr["phase_max_ms"], rr["drift_ms"], rr["drift_ms_per_min"],
                rr.get("drift_limit_ms", 0.0)))
    for d in report["disturbances"]:
        lines.append("%s at clock %d: %s" % (d["kind"], d["c_from"], ", ".join("%s %s" % (k, v) for k, v in d.items() if k not in ("kind", "c_from", "c_to"))))
    for s in report["stops"]:
        lines.append("stop: note-ons after it %s, silence %s ms, hung notes %d" % (s["notes_after_ms"] or "none", "%.1f" % s["silence_ms"] if "silence_ms" in s else "-", s["hung_notes"]))
    for q in report["quiet"]:
        lines.append("quiet master: notes after the last clock %d (last %s ms; horizon %.0f ms), closed %s, release %s ms%s" % (
            q["notes_after_last_clock"], "%.0f" % q["last_note_after_ms"] if q["last_note_after_ms"] is not None else "-", q["stale_horizon_s"] * 1000.0, q["closed"],
            "%.0f" % q["release_ms"] if q["release_ms"] is not None else "-", ", restarted without Start: %d notes" % q["restart_notes_without_start"] if q.get("restart_notes_without_start") else ""))
    if report["unit_clocks_relayed"]:
        lines.append("the unit sent %d clocks back (relay): ignored" % report["unit_clocks_relayed"])
    bad = [c for c in report["checks"] if not c["ok"]]
    for c in bad:
        lines.append("FAIL  %s: %s = %s (limit %s)" % (c["scope"], c["name"], c["value"], c["limit"]))
    lines.append("%s (%d checks, %d failed)" % ("PASS" if report["pass"] else "FAIL", len(report["checks"]), len(bad)))
    return "\n".join(lines)


def sync(out_dev, in_dev, profile_text, jitter_ms=0.0, seed=1, drop=None, burst=None, base=36, cycle=16, channel=1, lead_ms=2.0, tail=3.0,
         auto_transport=True, limits=None, json_path=None, raw=False, quiet=False, rtprio=RT_PRIO):
    tokens = parse_sync_profile(profile_text)
    plan = build_plan(tokens, jitter_ms, seed, drop, burst, lead_ms, auto_transport)
    rfd = os.open(in_dev, os.O_RDONLY | os.O_NONBLOCK) if in_dev else None
    wfd = os.open(out_dev, os.O_WRONLY)
    try:
        events, info = run_master(plan, wfd, rfd, tail, rtprio=rtprio)
    finally:
        os.close(wfd)
        if rfd is not None:
            os.close(rfd)
    analysis = analyze(plan, events, base, cycle, channel, limits)
    series = analysis.pop("_series")
    report = {
        "tool": "midi_clock_tool sync", "format": 1, "profile": profile_text, "jitter_ms": jitter_ms, "seed": seed, "drop": drop, "burst": burst,
        "base": base, "cycle": cycle, "channel": channel, "lead_ms": lead_ms,
        "master": {"clocks_planned": len(plan.ideal), "clocks_sent": sum(1 for s in plan.sent if s is not None), "sched_fifo": info["sched_fifo"], "sched": info["sched"],
                   "lateness_ms": {k: round(v, 3) for k, v in info.get("lateness_ms", {}).items()},
                   "write_lateness_per_run": info.get("write_lateness_per_run", [])},
        "unit_clocks_relayed": info["relayed_clocks"],
        "notes_heard": sum(1 for e in events if e[0] == "on"),
    }
    report.update(analysis)
    if raw:
        report["raw"] = {"error_series_ms": [[[round(t, 4), round(e, 4)] for t, e in s] for s in series], "events": [[e[0], round(e[1], 5), e[2], e[3], e[4]] for e in events if e[0] != "clock"]}
    if json_path:
        with open(json_path, "w") as f:
            json.dump(report, f, indent=1)
    if not quiet:
        print(summary(report))
    return report


# ---- the synthetic unit of the self-test ------------------------------------------------------------------------------------------------------------

def fake_unit_events(plan, delay=0.003, noise=0.0, seed=7, base=36, cycle=16, s0=0, drift_per_s=0.0, skip=(), dup=(), channel=1, follow_clocks=STALE_CLOCKS,
                     lag=None, spp_shift=0, cont_restart=False):
    """The reader events of a unit that follows the plan perfectly: the note of step n at the ideal time of its clock + delay + noise. `lag(t)` adds a time-dependent error.
    A bare Continue resumes on the step after the previous run's last step (a good unit); `cont_restart` restarts from step 0 (a bad unit)."""
    rng = random.Random(seed)
    ev = []
    runs = _build_runs(plan)
    last_abs = -1
    for run in runs:
        n = 0
        if run["kind"] == "start":
            s = 0
        elif run["spp"] is not None:
            s = run["spp"] + spp_shift
        elif last_abs < 0:
            s = s0
        else:
            s = 0 if cont_restart else last_abs + 1
        last_note = None
        while True:
            c = run["c0"] + CLOCKS_PER_STEP * n
            if c >= run["c_end"] + (follow_clocks if run["reason"] == "quiet" else 0):
                break
            if run["reason"] == "stop" and plan.ideal[min(c, len(plan.ideal) - 1)] >= run["t_end"]:
                break
            if c >= len(plan.ideal):
                break
            t = plan.ideal[c] + delay + (rng.gauss(0.0, noise) if noise else 0.0) + drift_per_s * plan.ideal[c] + (lag(plan.ideal[c]) if lag else 0.0)
            if n not in skip:
                note = base + (s + n) % cycle
                if last_note is not None:
                    ev.append(("off", t - 1e-4, channel - 1, last_note, 0))
                ev.append(("on", t, channel - 1, note, 100))
                last_note = note
                if n in dup:
                    ev.append(("on", t + 0.004, channel - 1, note, 100))
            n += 1
        t_close = (run["t_end"] if run["reason"] != "quiet" else plan.ideal[run["c_end"]] + follow_clocks * (plan.ideal[run["c_end"]] - plan.ideal[run["c_end"] - 1])) + 0.001
        if last_note is not None:
            ev.append(("off", t_close, channel - 1, last_note, 0))
        last_abs = s + n - 1  # the absolute step this run ended on
    ev.sort(key=lambda e: e[1])
    return ev


def _fifo_unit(clock_path, note_path, ready, stop, delay=0.003, noise=0.0004, base=36, channel=1):
    """The unit of the live self-test: counts the clocks after a Start, plays note base + n % 16 `delay` after clock 6n (+- noise), ends the note on Stop."""
    rfd = os.open(clock_path, os.O_RDONLY | os.O_NONBLOCK)
    ready.set()
    wfd = os.open(note_path, os.O_WRONLY)
    rng = random.Random(3)
    running = False
    count = 0
    pending = []  # (due, bytes)
    cur = None
    while not stop.is_set():
        timeout = 0.01
        if pending:
            timeout = max(0.0, min(0.01, pending[0][0] - time.monotonic()))
        ready_r, _, _ = select.select([rfd], [], [], timeout)
        if ready_r:
            try:
                data = os.read(rfd, 4096)
            except BlockingIOError:
                data = b""
            now = time.monotonic()
            for b in data:
                if b == 0xFA:
                    running, count, cur = True, 0, None
                elif b == 0xFC:
                    running = False
                    if cur is not None:
                        pending.append((now + 0.002, bytes([0x80 | (channel - 1), cur, 0])))
                        cur = None
                elif b == 0xF8 and running:
                    if count % CLOCKS_PER_STEP == 0:
                        note = base + (count // CLOCKS_PER_STEP) % 16
                        due = now + delay + rng.uniform(-noise, noise)
                        if cur is not None:
                            pending.append((due, bytes([0x80 | (channel - 1), cur, 0])))
                        pending.append((due, bytes([0x90 | (channel - 1), note, 100])))
                        cur = note
                    count += 1
            pending.sort(key=lambda x: x[0])
        now = time.monotonic()
        while pending and pending[0][0] <= now:
            os.write(wfd, pending.pop(0)[1])
    os.close(rfd)
    os.close(wfd)


# ======================================================================================================================================================
# OS-MTC A2: the MTC listen and analyse tool (docs/specs/OS-MTC.md slice A2; docs: platform/mk66/tools/hil/MTC.md).
# ======================================================================================================================================================

# The MTC rate codes, of quarter-frame piece 7 (`0111 0 rr h`) and of the full frame's hh (rate << 5 | hours).
MTC_RATE_CODES = {0: 24, 1: 25, 2: 29.97, 3: 30}
MTC_RATE_NAMES = {0: "24 fps", 1: "25 fps", 2: "29.97 drop-frame", 3: "30 fps"}

# The checks of an `mtc` run (starting proposals of slice A2, to confirm in the A3 HIL runs).
MTC_LIMITS = {
    "interval_ppm": 100.0,     # the quarter-frame interval mean against the host clock, in ppm
    "residual_p2p_ms": 2.0,    # the residual of the straight-line fit, peak to peak
    "full_frame_frames": 0.5,  # a full frame against --expect, in frames (equal to the frame)
    "note_mean_ms": 1.0,       # the notes against the time code, mean
    "note_spread_ms": 1.5,     # ... spread (std)
    "note_worst_ms": 4.0,      # ... worst
}


def parse_mtc_time(time_str):
    """HH:MM:SS:FF -> (hours, minutes, seconds, frames)."""
    parts = time_str.split(":")
    if len(parts) != 4:
        raise ValueError("MTC time must be HH:MM:SS:FF")
    h, m, s, f = (int(p) for p in parts)
    if not (0 <= h <= 23 and 0 <= m <= 59 and 0 <= s <= 59 and 0 <= f):
        raise ValueError("MTC time %r is out of range" % time_str)
    return h, m, s, f


def mtc_time_to_frame(t, fps):
    """(hours, minutes, seconds, frames) -> frames since 00:00:00:00 at fps."""
    h, m, s, f = t
    return f + s * fps + m * fps * 60 + h * fps * 3600


def frame_to_mtc_time(total, fps):
    """Frames since 00:00:00:00 at fps -> (hours, minutes, seconds, frames), wrapped at 24 h."""
    frames = total % fps
    total_seconds = total // fps
    return int((total_seconds // 3600) % 24), int((total_seconds // 60) % 60), int(total_seconds % 60), int(frames)


def mtc_quarter_frame_piece(piece, nibble):
    """One quarter-frame message F1 0nnn dddd."""
    return bytes((0xF1, (piece << 4) | (nibble & 0x0F)))


def mtc_time_pieces(t, rate_code):
    """The eight quarter-frame messages (pieces 0..7) spelling HH:MM:SS:FF. Piece 7 is `0111 0 rr h`:
    rate = (d >> 1) & 3, hours bit 4 = d & 1."""
    h, m, s, f = t
    nib = (f & 0xF, (f >> 4) & 0xF, s & 0xF, (s >> 4) & 0xF, m & 0xF, (m >> 4) & 0xF, h & 0xF, ((rate_code & 3) << 1) | ((h >> 4) & 1))
    return [mtc_quarter_frame_piece(p, nib[p]) for p in range(8)]


def mtc_full_frame_bytes(t, rate_code):
    """The full-frame SysEx F0 7F 7F 01 01 hh mm ss ff F7, with hh = rate << 5 | hours (the hours-high bits kept)."""
    h, m, s, f = t
    return bytes((0xF0, 0x7F, 0x7F, 0x01, 0x01, ((rate_code & 3) << 5) | (h & 0x1F), m & 0x7F, s & 0x7F, f & 0x7F, 0xF7))


class MTCStream:
    """Byte-level parse of raw MIDI reads: quarter frames (F1 d), the full-frame SysEx (F0 7F 7F 01 01 hh mm ss ff F7;
    any other SysEx is skipped to its F7), note-ons. A message cut by the end of a read chunk is carried into the
    next feed()."""

    def __init__(self):
        self._carry = b""
        self.qf = []     # (ts, piece, data byte)
        self.full = []   # (ts, (hours, minutes, seconds, frames, rate code))
        self.notes = []  # (ts, note, channel 1..16)

    def feed(self, chunk, ts):
        data = self._carry + bytes(chunk)
        self._carry = b""
        i, n = 0, len(data)
        while i < n:
            b = data[i]
            if b == 0xF1:  # quarter frame: one data byte
                if i + 1 >= n:
                    self._carry = data[i:]
                    return
                self.qf.append((ts, (data[i + 1] >> 4) & 0x07, data[i + 1]))
                i += 2
            elif b == 0xF0:  # SysEx
                if i + 5 > n:
                    self._carry = data[i:]
                    return
                if data[i + 1:i + 5] == b"\x7f\x7f\x01\x01":  # an MTC full frame is exactly 10 bytes and ends F7
                    if i + 10 > n:
                        self._carry = data[i:]
                        return
                    if data[i + 9] == 0xF7:
                        hh = data[i + 5]
                        self.full.append((ts, (hh & 0x1F, data[i + 6], data[i + 7], data[i + 8], (hh >> 5) & 0x03)))
                        i += 10
                        continue
                j = data.find(b"\xf7", i + 1)  # some other SysEx: skip to its end
                if j < 0:
                    self._carry = data[i:]
                    return
                i = j + 1
            elif 0x80 <= b <= 0xEF:  # channel message
                need = 1 if (b & 0xF0) in (0xC0, 0xD0) else 2
                if i + 1 + need > n:
                    self._carry = data[i:]
                    return
                if (b & 0xF0) == 0x90 and data[i + 2] > 0:
                    self.notes.append((ts, data[i + 1], (b & 0x0F) + 1))
                i += 1 + need
            elif b == 0xF2:  # Song Position Pointer
                if i + 3 > n:
                    self._carry = data[i:]
                    return
                i += 3
            elif b == 0xF3:
                if i + 2 > n:
                    self._carry = data[i:]
                    return
                i += 2
            else:  # real-time bytes and the rest: single bytes
                i += 1


class MTCDecoder:
    """Quarter-frame pieces to times. A full time needs pieces 0..7 in order within one cycle (eight pieces span two
    frames); the time they spell is the time at piece 0, so the reported time is it +2 frames. A piece out of order is
    counted (dropped when the next-but-one arrives, repeated when the same one arrives again, anything else as a mixed
    cycle) and breaks the cycle: no time comes out of it."""

    def __init__(self):
        self.expected = 0   # the piece a clean cycle wants next
        self.cycle = {}     # piece -> data nibble
        self.piece0_ts = 0.0
        self.clean = True
        self.last = None
        self.cycles = []    # (piece 0's ts, piece 7's ts, frames spelled, rate code)
        self.rate_code = None
        self.dropped = 0
        self.repeated = 0
        self.mixed = 0

    def feed(self, ts, piece, data):
        nib = data & 0x0F
        if piece == self.expected:
            if piece == 0:
                self.cycle, self.piece0_ts, self.clean = {}, ts, True
            self.cycle[piece] = nib
            self.last = piece
            self.expected = (piece + 1) % 8
            if piece == 7:
                if self.clean and len(self.cycle) == 8:
                    self._assemble(ts)
                self.cycle, self.expected, self.clean = {}, 0, True
            return
        if piece == self.last:
            self.repeated += 1
            return
        if (piece - self.expected) % 8 == 1:
            self.dropped += 1  # the piece in between never arrived
        self.mixed += 1
        self.clean = False
        self.last = piece
        if piece == 0:
            self.cycle, self.piece0_ts, self.clean, self.expected = {0: nib}, ts, True, 1
        else:
            self.expected = (piece + 1) % 8

    def _assemble(self, ts7):
        n = self.cycle
        rate = (n[7] >> 1) & 0x03          # piece 7: 0111 0 rr h
        hours = ((n[7] & 0x01) << 4) | n[6]  # hours bit 4 rides in piece 7
        frames = (n[1] << 4) | n[0]
        seconds = (n[3] << 4) | n[2]
        minutes = (n[5] << 4) | n[4]
        fps = MTC_RATE_CODES[rate]
        spelled = frames + seconds * fps + minutes * fps * 60 + hours * fps * 3600
        self.rate_code = rate
        self.cycles.append((self.piece0_ts, ts7, spelled, rate))


def mtc_song_plan(song, cap=4096):
    """The expected time of every step of a test song from its first step, as (wire note, seconds), mirroring the A1
    walk (seq/src/song_time.cpp): a run starts at the project tempo and swing; each row's timing FX apply from the row
    they stand in (Tempo 15: value x 2 BPM, below 40 is 40; Swing 17: 25..75, straight at 50; Tempo fine 44: (v - 128) / 4
    BPM, folded in and staying; Break 13 / Order jump 45 end the pattern); a step lasts 15 / BPM s times (150 - s) / 100
    (even) or (50 + s) / 100 (odd) of the swing pair. The note names the step: 36 + 16 x (pattern - 1) + step."""
    if song == "midisync-song":
        tempo_x100, swing, order, fx = 12000, 50, [1, 2, 3], {}
    elif song == "mtc-tempo":
        tempo_x100, swing, order, fx = 12000, 50, [1, 2, 3], {
            (2, 1): [(15, 70), (0, 0)],   # Tempo 140 BPM (value x 2, docs/formats/med.md 3.5)
            (2, 2): [(17, 50), (0, 0)],   # Swing, straight
            (3, 1): [(44, 132), (0, 0)],  # Tempo fine +1.00 BPM ((v - 128) / 4)
            (3, 2): [(13, 1), (0, 0)],    # Break pattern: the song loops from slot 3 back to slot 1
        }
    else:
        raise ValueError("unknown song %r (midisync-song, mtc-tempo)" % song)
    fx_tempo = 0  # a run starts at the project tempo (a start resets the FX tempo)
    swing_fx = 0
    odd = False
    t = 0.0
    op, pat, step = 0, order[0], 0
    out = []
    for _ in range(cap):
        fine = 0
        brk = False
        jump = None
        for col in (1, 0):  # the columns run 1 then 0
            ft, fv = fx.get((pat, step), ((0, 0), (0, 0)))[col]
            if ft == 15:
                fx_tempo = max(40, min(800, fv * 2)) * 100
            elif ft == 17:
                swing_fx = fv
            elif ft == 44:
                fine += (fv - 128) * 25
            elif ft == 13:
                brk = True
            elif ft == 45:
                jump = fv
        if fine:
            fx_tempo = max(4000, min(80000, (fx_tempo or tempo_x100) + fine))
        tempo = fx_tempo or tempo_x100
        s = max(25, min(75, swing_fx or swing))
        dur = 15.0 / (tempo / 100.0) * ((50.0 + s) if odd else (150.0 - s)) / 100.0
        out.append((36 + 16 * (pat - 1) + step, t))
        t += dur
        odd = not odd
        step += 1
        if step >= 16 or brk or jump is not None:  # pattern_end: the next order slot (the song loops)
            op = jump if (jump is not None and 0 <= jump < len(order)) else (op + 1) % len(order)
            pat, step = order[op], 0
    return out


def mtc_analyze(stream, fps_arg=None, expect=None, song=None):
    """The A2 analysis of a captured stream: the rate code; piece continuity (dropped / repeated / mixed, the time of a
    cycle +2 frames); the quarter-frame interval mean against the host clock, in ppm; the straight-line fit of the
    decoded time against the arrival (drift in ppm, residual peak to peak and RMS); the full frames against --expect;
    and the notes against the time code (each step's note should arrive at its song time + the expected start time),
    with the song's plan when --song says so."""
    dec = MTCDecoder()
    for ts, piece, data in stream.qf:
        dec.feed(ts, piece, data)
    rate = dec.rate_code if dec.rate_code is not None else (stream.full[0][1][4] if stream.full else None)
    fps = MTC_RATE_CODES[rate] if rate is not None else (fps_arg if fps_arg else 25.0)
    checks = []

    def check(name, value, limit, ok):
        checks.append({"name": name, "value": value, "limit": limit, "ok": bool(ok)})

    if fps_arg is not None and rate is not None:
        check("rate code vs --fps", "%d (%s)" % (rate, MTC_RATE_NAMES[rate]), fps_arg, abs(MTC_RATE_CODES[rate] - fps_arg) < 0.01)

    # the straight-line fit of the decoded time (the time at piece 0) against its arrival
    intercept, slope = 0.0, 1.0
    fit = None
    if len(dec.cycles) >= 2:
        xs = [c[0] for c in dec.cycles]
        ys = [c[2] / fps for c in dec.cycles]
        n = len(xs)
        mx = sum(xs) / n
        my = sum(ys) / n
        sxx = sum((x - mx) ** 2 for x in xs)
        sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        slope = sxy / sxx if sxx > 0 else 1.0
        intercept = my - slope * mx
        resid = [y - (intercept + slope * x) for x, y in zip(xs, ys)]
        fit = {
            "cycles": n,
            "slope": slope,
            "drift_ppm": (slope - 1.0) * 1e6,
            "residual_p2p_s": max(resid) - min(resid),
            "residual_rms_s": math.sqrt(sum(r * r for r in resid) / n),
        }
        check("fit residual peak to peak (ms)", fit["residual_p2p_s"] * 1000.0, MTC_LIMITS["residual_p2p_ms"],
              fit["residual_p2p_s"] * 1000.0 <= MTC_LIMITS["residual_p2p_ms"])

    # the quarter-frame interval mean against the host clock
    interval = None
    if len(stream.qf) >= 2:
        ts = [q[0] for q in stream.qf]
        iv = [ts[i + 1] - ts[i] for i in range(len(ts) - 1)]
        mean = sum(iv) / len(iv)
        want = 1.0 / (fps * 4.0)
        interval = {"count": len(iv), "mean_s": mean, "expected_s": want, "mean_ppm": (mean - want) / want * 1e6}
        check("quarter-frame interval mean (ppm)", interval["mean_ppm"], MTC_LIMITS["interval_ppm"], abs(interval["mean_ppm"]) <= MTC_LIMITS["interval_ppm"])

    check("dropped pieces", dec.dropped, 0, dec.dropped == 0)
    check("repeated pieces", dec.repeated, 0, dec.repeated == 0)
    check("mixed cycles", dec.mixed, 0, dec.mixed == 0)

    # the full frames: value, when, and against --expect
    expect_frames = mtc_time_to_frame(expect, fps) if expect is not None else None
    full = []
    for ts, (h, m, s, f, rc) in stream.full:
        entry = {"at_s": ts, "time": "%02d:%02d:%02d:%02d" % (h, m, s, f), "rate_code": rc, "frames": mtc_time_to_frame((h, m, s, f), fps)}
        if expect_frames is not None:
            entry["delta_frames_vs_expect"] = entry["frames"] - expect_frames
        full.append(entry)
    if expect is not None and full:
        deltas = [e["delta_frames_vs_expect"] for e in full]
        check("full frames vs --expect (frames)", deltas, 0, all(abs(d) <= MTC_LIMITS["full_frame_frames"] for d in deltas))

    # the notes against the time code and --expect: each step's note at its song time + the expected start time
    notes = None
    if song is not None and expect is not None and fit is not None and stream.notes:
        plan = {}
        for note, t in mtc_song_plan(song):
            plan.setdefault(note, []).append(t)
        seen = {}
        offsets = []
        for ts, note, _channel in stream.notes:
            k = seen.get(note, 0)
            seen[note] = k + 1
            if note in plan and k < len(plan[note]):
                tc_s = intercept + slope * ts  # the time code at the note's arrival, from the fit
                offsets.append((tc_s - (expect_frames / fps + plan[note][k])) * 1000.0)
        if offsets:
            notes = {
                "heard": len(stream.notes),
                "matched": len(offsets),
                "mean_ms": statistics.fmean(offsets),
                "spread_ms": statistics.pstdev(offsets) if len(offsets) > 1 else 0.0,
                "worst_ms": max(abs(o) for o in offsets),
            }
            check("notes vs time code: mean (ms)", notes["mean_ms"], MTC_LIMITS["note_mean_ms"], abs(notes["mean_ms"]) <= MTC_LIMITS["note_mean_ms"])
            check("notes vs time code: spread (ms)", notes["spread_ms"], MTC_LIMITS["note_spread_ms"], notes["spread_ms"] <= MTC_LIMITS["note_spread_ms"])
            check("notes vs time code: worst (ms)", notes["worst_ms"], MTC_LIMITS["note_worst_ms"], notes["worst_ms"] <= MTC_LIMITS["note_worst_ms"])

    result = {
        "tool": "midi_clock_tool mtc",
        "format": 2,
        "rate_code": rate,
        "rate_name": MTC_RATE_NAMES.get(rate),
        "fps": fps,
        "quarter_frames": len(stream.qf),
        "cycles": [{"at_s": c[0], "time_plus_2_frames": c[2] + 2, "rate_code": c[3]} for c in dec.cycles],
        "dropped_pieces": dec.dropped,
        "repeated_pieces": dec.repeated,
        "mixed_cycles": dec.mixed,
        "interval": interval,
        "fit": fit,
        "full_frames": full,
        "notes": notes,
        "checks": checks,
    }
    if expect is not None:
        result["expect"] = "%02d:%02d:%02d:%02d" % expect
        result["expect_frames"] = expect_frames
    if song is not None:
        result["song"] = song
    result["pass"] = all(c["ok"] for c in checks)
    return result


def _print_mtc(result):
    lines = ["MTC analysis"]
    if result["rate_code"] is not None:
        lines.append("  rate: %s (code %d, %.2f fps)" % (result["rate_name"], result["rate_code"], result["fps"]))
    lines.append("  quarter frames: %d, cycles: %d, dropped %d, repeated %d, mixed %d" % (
        result["quarter_frames"], len(result["cycles"]), result["dropped_pieces"], result["repeated_pieces"], result["mixed_cycles"]))
    if result["interval"]:
        lines.append("  interval mean %.6f s (expected %.6f, %+.1f ppm)" % (
            result["interval"]["mean_s"], result["interval"]["expected_s"], result["interval"]["mean_ppm"]))
    if result["fit"]:
        lines.append("  fit: drift %+.1f ppm, residual %.3f ms p2p, %.3f ms rms" % (
            result["fit"]["drift_ppm"], result["fit"]["residual_p2p_s"] * 1000.0, result["fit"]["residual_rms_s"] * 1000.0))
    if result["cycles"]:
        last = result["cycles"][-1]
        t = frame_to_mtc_time(last["time_plus_2_frames"] % (result["fps"] * 86400), result["fps"])
        lines.append("  last decoded time: %02d:%02d:%02d:%02d (piece 0 at %.3f s, +2 frames)" % (t[0], t[1], t[2], t[3], last["at_s"]))
    for e in result["full_frames"]:
        line = "  full frame at %.3f s: %s (code %d)" % (e["at_s"], e["time"], e["rate_code"])
        if "delta_frames_vs_expect" in e:
            line += ", %+.1f frames vs --expect" % e["delta_frames_vs_expect"]
        lines.append(line)
    if result["notes"]:
        n = result["notes"]
        lines.append("  notes: %d heard, %d matched, vs time code mean %.3f ms, spread %.3f ms, worst %.3f ms" % (
            n["heard"], n["matched"], n["mean_ms"], n["spread_ms"], n["worst_ms"]))
    failed = [c for c in result["checks"] if not c["ok"]]
    lines.append("  %s (%d checks, %d failed)" % ("PASS" if not failed else "FAIL", len(result["checks"]), len(failed)))
    for c in failed:
        lines.append("    FAIL %s = %r (limit %r)" % (c["name"], c["value"], c["limit"]))
    print("\n".join(lines))


def mtc(dev, seconds=20.0, fps=None, expect=None, song=None, json_path=None):
    """OS-MTC A2: listen on the unit's raw MIDI node for `seconds` and analyse the MTC it sends (MTC.md)."""
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    stream = MTCStream()
    start = time.monotonic()
    try:
        while time.monotonic() - start < seconds:
            ready, _, _ = select.select([fd], [], [], 0.05)
            if not ready:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if not data:
                break
            stream.feed(data, time.monotonic())
    finally:
        os.close(fd)
    result = mtc_analyze(stream, fps, expect, song)
    result["duration_seconds"] = seconds
    if json_path:
        with open(json_path, "w") as f:
            json.dump(result, f, indent=1)
    _print_mtc(result)
    return result


def _synth_mtc(start_time, rate_code, fps, n_qf, t0=100.0, drift=0.0, drop=(), repeat=(), reverse=()):
    """A synthetic MTC stream as (t, bytes) events: a full frame at t0, then n_qf quarter frames, one per 1/(4*fps) s of
    arrival (the arrival grid stretched by drift), piece g mod 8, cycle c spelling the time of frame start + 2c.
    Pieces at the indices in `drop` are not sent, at `repeat` are sent twice, and the cycle in `reverse` goes out 7..0."""
    events = [(t0, mtc_full_frame_bytes(start_time, rate_code))]
    interval = 1.0 / (fps * 4.0)
    q0 = mtc_time_to_frame(start_time, fps)
    for g in range(n_qf):
        if g in drop:
            continue
        c, p = g // 8, g % 8
        pieces = mtc_time_pieces(frame_to_mtc_time(q0 + 2 * c, fps), rate_code)
        q = (7 - p) if c in reverse else p
        ts = t0 + g * interval * (1.0 + drift)
        events.append((ts, pieces[q]))
        if g in repeat:
            events.append((ts + interval * 0.5, pieces[q]))
    events.sort(key=lambda e: e[0])
    return events


def _self_test_mtc():
    """The A2 self-test, fed straight into the parser and the analysis with explicit timestamps (no wall clock, no
    FIFO): known times at 24/25/30 fps decode (+2 frames reported); a known ppm drift comes back from the fit and the
    interval mean; a dropped piece, a repeated piece and a mixed (reversed) cycle are counted; a full frame and a
    quarter frame cut across two reads both parse (and a SysEx that is not a full frame is skipped); and the
    mtc-tempo plan sees the tempo step (its notes measure ~0 against the time code)."""
    problems = []

    def measured(name, value, want, tol):
        if value is None or abs(value - want) > tol:
            problems.append("%s: measured %r, wanted %.6f +-%.6f" % (name, value, want, tol))

    def run(events):
        s = MTCStream()
        for t, data in events:
            s.feed(data, t)
        return s

    # known times decode at 24/25/30 fps (piece 7's rate code and hours bit 4 included), the reported time +2 frames
    for fps, rc, start in ((24, 0, (1, 2, 3, 5)), (25, 1, (17, 58, 59, 24)), (30, 3, (23, 59, 58, 29))):
        q0 = mtc_time_to_frame(start, fps)
        res = mtc_analyze(run(_synth_mtc(start, rc, fps, 64)), fps_arg=fps, expect=start)
        if res["rate_code"] != rc:
            problems.append("%d fps: rate code %r, wanted %d" % (fps, res["rate_code"], rc))
        if len(res["cycles"]) != 8:
            problems.append("%d fps: %d cycles decoded, wanted 8" % (fps, len(res["cycles"])))
        else:
            measured("%d fps: first decoded time (frames, +2)" % fps, res["cycles"][0]["time_plus_2_frames"], q0 + 2, 0.01)
            measured("%d fps: last decoded time (frames, +2)" % fps, res["cycles"][-1]["time_plus_2_frames"], q0 + 2 * 7 + 2, 0.01)
        if len(res["full_frames"]) != 1 or res["full_frames"][0]["time"] != "%02d:%02d:%02d:%02d" % start or res["full_frames"][0]["delta_frames_vs_expect"] != 0:
            problems.append("%d fps: full frame %r" % (fps, res["full_frames"]))
        if not res["pass"]:
            problems.append("%d fps: the clean stream does not pass: %r" % (fps, [c for c in res["checks"] if not c["ok"]]))

    # a known ppm drift: the arrival grid stretched, the spelled times exact
    drift = 300e-6
    res = mtc_analyze(run(_synth_mtc((1, 0, 0, 0), 1, 25, 400, drift=drift)), fps_arg=25)
    measured("ppm drift: fit drift (ppm)", res["fit"]["drift_ppm"], (1.0 / (1.0 + drift) - 1.0) * 1e6, 0.5)
    measured("ppm drift: interval mean (ppm)", res["interval"]["mean_ppm"], drift * 1e6, 1.0)
    if res["fit"]["residual_p2p_s"] > 1e-6:
        problems.append("ppm drift: residual p2p %.2e s on an exact grid" % res["fit"]["residual_p2p_s"])

    # a dropped piece, a repeated piece and a mixed (reversed) cycle are counted
    res = mtc_analyze(run(_synth_mtc((1, 0, 0, 0), 1, 25, 64, drop=(20,))), fps_arg=25)
    if res["dropped_pieces"] != 1 or res["mixed_cycles"] < 1 or len(res["cycles"]) != 7:
        problems.append("a dropped piece: dropped %d, mixed %d, cycles %d (wanted 1, >=1, 7)" % (
            res["dropped_pieces"], res["mixed_cycles"], len(res["cycles"])))
    res = mtc_analyze(run(_synth_mtc((1, 0, 0, 0), 1, 25, 64, repeat=(20,))), fps_arg=25)
    if res["repeated_pieces"] != 1 or res["dropped_pieces"] or res["mixed_cycles"] or len(res["cycles"]) != 8:
        problems.append("a repeated piece: repeated %d, dropped %d, mixed %d, cycles %d (wanted 1, 0, 0, 8)" % (
            res["repeated_pieces"], res["dropped_pieces"], res["mixed_cycles"], len(res["cycles"])))
    res = mtc_analyze(run(_synth_mtc((1, 0, 0, 0), 1, 25, 64, reverse=(2,))), fps_arg=25)
    if res["mixed_cycles"] < 1 or res["dropped_pieces"] or len(res["cycles"]) != 7:
        problems.append("a mixed (reversed) cycle: mixed %d, dropped %d, cycles %d (wanted >=1, 0, 7)" % (
            res["mixed_cycles"], res["dropped_pieces"], len(res["cycles"])))

    # a full frame and a quarter frame cut across two reads both parse; a SysEx that is not a full frame is skipped
    s = MTCStream()
    ff = mtc_full_frame_bytes((1, 2, 3, 4), 1)
    s.feed(ff[:5], 10.0)
    s.feed(ff[5:], 10.0)
    qf = mtc_time_pieces((1, 2, 3, 4), 1)[0]
    s.feed(qf[:1], 10.0)
    s.feed(qf[1:], 10.0)
    s.feed(bytes((0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 3, 4, 5, 0x00, 0xF7)), 10.0)  # not F7 where the full frame ends
    if len(s.full) != 1 or s.full[0][1] != (1, 2, 3, 4, 1):
        problems.append("split full frame: %r" % (s.full,))
    if len(s.qf) != 1 or s.qf[0][1:] != (0, qf[1]):
        problems.append("split quarter frame: %r" % (s.qf,))

    # the mtc-tempo plan sees the tempo step, and its notes measure ~0 against the time code
    plan = mtc_song_plan("mtc-tempo")
    by_note = {}
    for note, t in plan:
        by_note.setdefault(note, []).append(t)
    measured("mtc-tempo plan: a step at 120 BPM (s)", by_note[37][0] - by_note[36][0], 0.125, 1e-9)                 # slot 1 step 0 -> 1
    measured("mtc-tempo plan: the step before the Tempo FX (s)", by_note[53][0] - by_note[52][0], 0.125, 1e-9)        # slot 2 step 0 -> 1: still 120
    measured("mtc-tempo plan: a step after the Tempo FX (s)", by_note[54][0] - by_note[53][0], 15.0 / 140.0, 1e-9)     # slot 2 step 1 -> 2: 140 BPM
    measured("mtc-tempo plan: a step after the Tempo fine (s)", by_note[70][0] - by_note[69][0], 15.0 / 141.0, 1e-9)  # slot 3 step 1 -> 2: 141 BPM
    fps, rc, expect, t0 = 25, 1, (1, 0, 0, 0), 100.0
    events = _synth_mtc(expect, rc, fps, 8 * 130, t0=t0)  # 10 s of time code
    for note, t in plan[:35]:  # one loop of the song (the Break in slot 3 ends it)
        events.append((t0 + t, bytes((0x90, note, 100))))
    events.sort(key=lambda e: e[0])
    res = mtc_analyze(run(events), fps_arg=fps, expect=expect, song="mtc-tempo")
    if res["notes"] is None or res["notes"]["matched"] != 35:
        problems.append("mtc-tempo notes: %r" % (res["notes"],))
    else:
        measured("mtc-tempo notes vs time code: mean (ms)", res["notes"]["mean_ms"], 0.0, 0.05)
        if res["notes"]["worst_ms"] > 0.1:
            problems.append("mtc-tempo notes vs time code: worst %.3f ms" % res["notes"]["worst_ms"])

    note = "decode 24/25/30 fps (+2 frames), drift %+.1f ppm measured, dropped/repeated/mixed counted, split reads parse, mtc-tempo tempo step seen" % (
        (1.0 / (1.0 + drift) - 1.0) * 1e6)
    return problems, note


# ======================================================================================================================================================
# OS-MTC B1: the host as the MTC master (docs/specs/OS-MTC.md slice B1; docs: platform/mk66/tools/hil/MTC.md).
# ======================================================================================================================================================

# --fps -> (rate code, frames a second the time labels count, real frames a second, drop-frame). 29.97 is drop-frame time: the labels count 30 a second
# and skip frame numbers 00 and 01 of every minute but each tenth, so the real rate is 30000/1001.
MTC_SEND_RATES = {24.0: (0, 24, 24.0, False), 25.0: (1, 25, 25.0, False), 29.97: (2, 30, 30000.0 / 1001.0, True), 30.0: (3, 30, 30.0, False)}
MTC_CODE_INFO = {info[0]: info for info in MTC_SEND_RATES.values()}
MTC_SEND_GAP_S = 0.004   # spec A-2: a run starts its quarter frames at least 4 ms after its full frame
MTC_SEND_MIN_PLAIN = 16  # --drop never takes one of the first 16 quarter frames

# The checks of an `mtc-send` run: starting values from the B3 acceptance (docs/specs/OS-MTC.md), to confirm in the B3 HIL runs. Override one with --limit key=value.
MTC_SEND_LIMITS = {
    "lock_ms": 250.0,        # B3: from the first quarter frame (the pre-roll before the song start excluded) to the first note, at a start and after a stop
    "reloc_ms": 300.0,       # B3: a jump, from its full frame to the first note of the new position
    "phase_mean_ms": 1.0,    # B3: the notes against the master's time, mean
    "phase_spread_ms": 3.0,  # ... spread (standard deviation)
    "phase_worst_ms": 10.0,  # ... worst |error| (not in the spec; three spreads and a bit)
    "drift_ms": 1.0,         # the slope of the error times the span; the effective limit is max(this, 3 * spread * sqrt(12 / n)), as `sync`'s
    "missing": 0,            # expected steps (after the lock of their run) that never sounded
    "extra": 0,              # notes that match no step and are not the unit's lock, relocation or stop
    "stop_frames": 4.0,      # B3: a master that stops is followed by silence within this many frames ...
    "stop_slack_ms": 5.0,    # ... plus a block and the USB frame
    "hold_frames": 3.0,      # a `quiet` shorter than this is a drop-out the unit must freewheel over (no stop expected); a longer one is a stopped master
    "min_steps": 16,         # fewer matched notes prove nothing about the phase
    "match_ms": 40.0,        # a note this close to a step's expected time, with that step's note, is that step
}


def mtc_rate_info(fps):
    """--fps -> (rate code, label frames a second, real frames a second, drop-frame)."""
    for key, info in MTC_SEND_RATES.items():
        if abs(key - fps) < 0.005:
            return info
    raise ValueError("--fps takes 24, 25, 29.97 or 30")


def mtc_label_to_frames(t, rate_code):
    """(hours, minutes, seconds, frames) -> real frames since 00:00:00:00. At 29.97 drop-frame the frame numbers 00 and 01 of each minute but every tenth do not exist."""
    h, m, s, f = t
    _, nominal, _, drop = MTC_CODE_INFO[rate_code]
    if not (0 <= h <= 23 and 0 <= m <= 59 and 0 <= s <= 59 and 0 <= f < nominal):
        raise ValueError("%02d:%02d:%02d:%02d is out of range at %d frames a second" % (h, m, s, f, nominal))
    if drop and s == 0 and f < 2 and m % 10 != 0:
        raise ValueError("%02d:%02d:%02d:%02d does not exist in drop-frame time (frames 00 and 01 of a minute are skipped, except every tenth minute)" % (h, m, s, f))
    minutes = 60 * h + m
    n = (minutes * 60 + s) * nominal + f
    if drop:
        n -= 2 * (minutes - minutes // 10)
    return n


def mtc_frames_to_label(n, rate_code):
    """Real frames since 00:00:00:00 -> (hours, minutes, seconds, frames), wrapped at 24 hours (the inverse of mtc_label_to_frames)."""
    _, nominal, _, drop = MTC_CODE_INFO[rate_code]
    n %= nominal * 86400 - (2 * (1440 - 144) if drop else 0)
    if drop:  # 17982 frames in ten minutes, 1798 in a minute that drops
        d, r = divmod(n, 17982)
        n += 18 * d + (2 * ((r - 2) // 1798) if r >= 2 else 0)
    return (n // nominal // 3600, n // nominal // 60 % 60, n // nominal % 60, n % nominal)


def mtc_qf_message(q, rate_code):
    """Quarter frame number q (counted from 00:00:00:00, four to a frame; spec A-2: piece = q mod 8) -> `F1 0nnn dddd`. The eight pieces of a cycle spell the frame
    piece 0 stands at, 2 * (q // 8): a cycle starts on an even frame, and a receiver adds two frames when piece 7 arrives."""
    return mtc_time_pieces(mtc_frames_to_label(2 * (q // 8), rate_code), rate_code)[q % 8]


def parse_mtc_send_profile(text):
    """Tokens, in order: ('at', label) a full frame (a locate), ('jump', label) a full frame while running, ('run', s), ('quiet', s), ('reverse', s). A label is HH:MM:SS:FF."""
    tokens = []
    for raw in text.split(","):
        part = raw.strip().lower()
        if not part:
            continue
        name, _, arg = part.partition(":")
        if name in ("at", "jump"):
            tokens.append((name, parse_mtc_time(arg)))
        elif name in ("run", "quiet", "reverse"):
            try:
                seconds = float(arg)
            except ValueError:
                raise ValueError("token %r is not %s:SECONDS" % (part, name))
            if not 0.0 < seconds < 86400.0:
                raise ValueError("token %r: the duration must be above 0 and under a day" % part)
            tokens.append((name, seconds))
        else:
            raise ValueError("unknown profile token %r (at:HH:MM:SS:FF, run:S, quiet:S, jump:HH:MM:SS:FF, reverse:S)" % part)
    if not any(t[0] in ("run", "reverse") for t in tokens):
        raise ValueError("the profile has no run or reverse segment")
    return tokens


class MtcPlan:
    """The master's schedule. `events` in send order as [deadline_s, bytes, kind, number] (kind "full": the frame count the full frame names; "qf": q), `sent_t` their
    actual send times once played. `segments` is the master's clock in pieces, with the quarter frames each sent (indexes into `events`)."""

    def __init__(self):
        self.events = []
        self.sent_t = []
        self.segments = []   # dicts: kind at|jump|run|quiet|reverse|tail, t0, t1, n0, n1 (frames), dir (+1 forward, -1 reverse, 0 stopped), qf_first, qf_last, ev (a full frame)
        self.tokens = []
        self.rate_code = 1
        self.fps_real = 25.0
        self.rate = 1.0
        self.start_frames = 0
        self.t_end = 0.0     # the end of the profile
        self.t_total = 0.0   # the end of the listening tail
        self.qf_planned = 0
        self.qf_dropped = 0
        self.jitter_ms = 0.0
        self.seed = 1


def build_mtc_plan(tokens, fps=25.0, rate=1.0, jitter_ms=0.0, seed=1, drop=None, start=(1, 0, 0, 0), tail=3.0):
    """The schedule of a profile. The master's clock runs `rate` times the host's: position n (real frames) is a label at 00:00:00:00 + n / fps, and quarter frame q
    is due when the position reaches q / 4 (forward) or falls to it (reverse). `at` and `jump` send a full frame and place the clock; a run starts its quarter frames
    on the grid at least 4 ms later, at whatever piece comes. `quiet` lets the clock run and sends nothing. A profile that does not begin with `at` begins with
    `at:<start>` (the song's MTC START); after the last token the master stops and the tool listens for `tail` seconds more."""
    code, _, fps_real, _ = mtc_rate_info(fps)
    if not 0.5 <= rate <= 1.5:
        raise ValueError("--rate takes 0.5..1.5")
    toks = list(tokens)
    if toks[0][0] != "at":
        toks.insert(0, ("at", start))
    plan = MtcPlan()
    plan.tokens, plan.rate_code, plan.fps_real, plan.rate, plan.jitter_ms, plan.seed = toks, code, fps_real, rate, jitter_ms, seed
    plan.start_frames = mtc_label_to_frames(start, code)
    slope = rate * fps_real  # real frames per host second
    rng = random.Random(seed)
    drop_rng = random.Random(seed + 1)
    jit = jitter_ms / 1000.0
    window_drops = {}
    t, n, last_q, not_before = 0.0, 0.0, None, 0.0
    for tok in toks:
        kind = tok[0]
        if kind in ("at", "jump"):
            frames = mtc_label_to_frames(tok[1], code)
            plan.events.append([t, mtc_full_frame_bytes(tok[1], code), "full", frames])
            n, last_q, not_before = float(frames), None, t + MTC_SEND_GAP_S
            plan.segments.append({"kind": kind, "t0": t, "t1": t, "n0": n, "n1": n, "dir": 0, "qf_first": None, "qf_last": None, "ev": len(plan.events) - 1, "label": tok[1]})
            continue
        seconds = tok[1]
        direction = -1 if kind == "reverse" else 1
        seg = {"kind": kind, "t0": t, "t1": t + seconds, "n0": n, "dir": direction, "qf_first": None, "qf_last": None}
        if kind != "quiet":
            if direction > 0:
                q = math.ceil(4.0 * n - 1e-9)
                if last_q is not None and q <= last_q:
                    q = last_q + 1
            else:
                q = math.floor(4.0 * n + 1e-9)
                if last_q is not None and q >= last_q:
                    q = last_q - 1
            while True:
                tq = t + (q / 4.0 - n) / slope if direction > 0 else t + (n - q / 4.0) / slope
                if tq >= t + seconds - 1e-12:
                    break
                if q < 0:
                    raise ValueError("the reverse run goes back past 00:00:00:00")
                if tq >= not_before - 1e-12:
                    idx = plan.qf_planned
                    plan.qf_planned += 1
                    dropped = False
                    if drop is not None and idx >= MTC_SEND_MIN_PLAIN:
                        k, per = drop
                        if idx // per not in window_drops:
                            window_drops[idx // per] = set(drop_rng.sample(range(per), k))
                        dropped = idx % per in window_drops[idx // per]
                    if dropped:
                        plan.qf_dropped += 1
                    else:
                        plan.events.append([tq + (rng.uniform(-jit, jit) if jit > 0.0 else 0.0), mtc_qf_message(q, code), "qf", q])
                        if seg["qf_first"] is None:
                            seg["qf_first"] = len(plan.events) - 1
                        seg["qf_last"] = len(plan.events) - 1
                    last_q = q
                q += direction
        n += direction * slope * seconds
        if n < 0.0:
            raise ValueError("the reverse run goes back past 00:00:00:00")
        t += seconds
        seg["n1"] = n
        plan.segments.append(seg)
    plan.t_end = t
    if tail > 0.0:
        plan.segments.append({"kind": "tail", "t0": t, "t1": t + tail, "n0": n, "n1": n, "dir": 0, "qf_first": None, "qf_last": None})
    plan.t_total = t + tail
    prev = -1e9  # the jitter never reorders: a message is never sent before the one that precedes it
    for ev in plan.events:
        if ev[0] < prev + 50e-6:
            ev[0] = prev + 50e-6
        prev = ev[0]
    return plan


def run_mtc_master(plan, wfd, rfd, lead_in_s=0.5, rtprio=RT_PRIO):
    """Sends the plan on `wfd` at absolute deadlines and reads `rfd` in the same loop (the loop of `run_master`, for a plan of quarter frames). Returns (reader events,
    info); fills plan.sent_t."""
    ns = time.monotonic_ns
    rt = enter_realtime(rtprio)
    info = {"sched_fifo": rt["fifo"], "rtprio": rt["prio"], "sched": rt["sched"]}
    parser = MidiParser()
    eof = [rfd is None]
    t0 = ns() + int(lead_in_s * 1e9)

    def drain():
        while not eof[0]:
            try:
                data = os.read(rfd, 4096)
            except BlockingIOError:
                return
            stamp = ns()
            if not data:
                eof[0] = True  # a FIFO whose writer is gone (the self-test); a MIDI node never ends
                return
            parser.feed(data, (stamp - t0) / 1e9)

    def wait_until(deadline):
        while True:
            rem = deadline - ns()
            if rem <= 0:
                return
            if rem > 1_500_000:
                wait = (rem - 1_000_000) / 1e9
                if eof[0]:
                    time.sleep(wait)
                elif select.select([rfd], [], [], wait)[0]:
                    drain()
            else:
                drain()  # the last millisecond is a spin (and still listens)

    import gc
    gc.disable()
    late = []
    sent_ns = []
    try:
        for ev in plan.events:
            deadline = t0 + int(ev[0] * 1e9)
            wait_until(deadline)
            before = ns()
            os.write(wfd, ev[1])
            sent_ns.append(before)
            late.append((before - deadline) / 1e6)
        wait_until(t0 + int(plan.t_total * 1e9))
        drain()
    finally:
        gc.enable()
        if rt["fifo"]:
            leave_realtime()
    plan.sent_t = [(s - t0) / 1e9 for s in sent_ns]
    qf_late = sorted(l for ev, l in zip(plan.events, late) if ev[2] == "qf")
    if qf_late:
        info["lateness_ms"] = {"mean": statistics.fmean(qf_late), "p99": qf_late[int(0.99 * (len(qf_late) - 1))], "max": qf_late[-1]}
    return parser.events, info


# ---- analysis -------------------------------------------------------------------------------------------------------------------------------------

def _mtc_send_t(plan, idx):
    return plan.sent_t[idx] if idx < len(plan.sent_t) else plan.events[idx][0]


def _mtc_song_steps(plan, song):
    """The song's steps as (wire note, master seconds): start_s + the song time of the step (mtc_song_plan), as many as the plan's master time reaches."""
    start_s = plan.start_frames / plan.fps_real
    span = max([s["n0"] / plan.fps_real + plan.rate * (s["t1"] - s["t0"]) for s in plan.segments if s["dir"] > 0] + [start_s])
    steps = mtc_song_plan(song, int(max(0.0, span - start_s) / 0.05) + 64)
    return [(note, start_s + t) for note, t in steps]


def analyze_mtc_send(plan, ons, song, limits=None):
    """The unit's notes (`ons`: [(arrival s on the master's clock, wire note)]) against the master's time. Every step of the song has an expected arrival: the host time at
    which the master's clock passes (MTC START + the step's song time); the forward stretches of the profile (run, and a quiet shorter than `hold_frames`) are cut into
    regions (a start, a jump, a resume after a stop), each judged from its own first note: lock (start, resume) or relocation (jump), the steps missing after it. The
    notes' errors against their expected times are the phase (mean, spread, worst, drift). A quiet longer than `hold_frames`, and the tail, are the stopped master: the
    stop latency is the last note after the last quarter frame. A reverse run is reported, not judged (the spec: reverse is not followed)."""
    lim = dict(MTC_SEND_LIMITS)
    if limits:
        lim.update(limits)
    fps, rate = plan.fps_real, plan.rate
    start_s = plan.start_frames / fps
    hold_s = lim["hold_frames"] / fps
    stop_limit_ms = lim["stop_frames"] / fps * 1000.0 + lim["stop_slack_ms"]
    match_s = lim["match_ms"] / 1000.0
    steps = _mtc_song_steps(plan, song)
    targets = [s for _, s in steps]

    regions, gaps, reverses = [], [], []
    cur, pend, last_qf = None, None, None

    def new_region(kind, label, loc_t):
        return {"kind": kind, "label": label, "loc_t": loc_t, "first_qf": None, "last_qf": None, "s0": None, "reach": None, "entries": []}

    def close():
        nonlocal cur
        if cur is not None:
            lq = cur["last_qf"]
            cur["entries"] = [e for e in cur["entries"] if lq is not None and e["t_e"] <= lq]  # a step due after the master's last quarter frame is not asked of the unit
            regions.append(cur)
            cur = None

    for seg in plan.segments:
        kind = seg["kind"]
        if kind == "tail" or (kind == "quiet" and seg["t1"] - seg["t0"] >= hold_s):  # the master has stopped
            close()
            if pend is None:
                if last_qf is not None:
                    pend = {"t0": seg["t0"], "t1": seg["t1"], "last_qf": last_qf}
            else:
                pend["t1"] = seg["t1"]
            continue
        if pend is not None:
            gaps.append(pend)
            pend = None
        if kind in ("at", "jump"):
            close()
            cur = new_region(kind, "%02d:%02d:%02d:%02d" % seg["label"], _mtc_send_t(plan, seg["ev"]))
            continue
        if seg["qf_last"] is not None:
            last_qf = _mtc_send_t(plan, seg["qf_last"])
        if kind == "reverse":
            close()
            reverses.append({"t0": seg["t0"], "t1": seg["t1"], "quarter_frames": sum(1 for i in range(seg["qf_first"], seg["qf_last"] + 1) if plan.events[i][2] == "qf") if seg["qf_first"] is not None else 0})
            continue
        if cur is None:
            cur = new_region("resume", "", seg["t0"])
        if seg["qf_first"] is not None:
            tf = _mtc_send_t(plan, seg["qf_first"])
            cur["first_qf"] = tf if cur["first_qf"] is None else cur["first_qf"]
            cur["last_qf"] = _mtc_send_t(plan, seg["qf_last"])
        s_a = seg["n0"] / fps
        s_b = s_a + rate * (seg["t1"] - seg["t0"])
        if cur["s0"] is None:
            cur["s0"] = s_a
        if cur["s0"] < start_s and cur["reach"] is None and s_a <= start_s <= s_b:
            cur["reach"] = seg["t0"] + (start_s - s_a) / rate
        for i in range(bisect.bisect_left(targets, s_a - 1e-9), bisect.bisect_left(targets, s_b - 1e-9)):
            cur["entries"].append({"note": steps[i][0], "t_e": seg["t0"] + (targets[i] - s_a) / rate, "t_a": None})
    close()
    if pend is not None:
        gaps.append(pend)

    # match: every expected step to the nearest unused note-on of its note within the window
    by_note = {}
    for t, note in sorted(ons):
        by_note.setdefault(note, []).append(t)
    used = set()
    for e in sorted((e for r in regions for e in r["entries"]), key=lambda e: e["t_e"]):
        lst = by_note.get(e["note"], [])
        i = bisect.bisect_left(lst, e["t_e"])
        best = None
        for j in range(max(0, i - 2), min(len(lst), i + 2)):
            if (e["note"], j) not in used and abs(lst[j] - e["t_e"]) <= match_s and (best is None or abs(lst[j] - e["t_e"]) < abs(lst[best] - e["t_e"])):
                best = j
        if best is not None:
            used.add((e["note"], best))
            e["t_a"] = lst[best]
    unmatched = [t for note, lst in by_note.items() for j, t in enumerate(lst) if (note, j) not in used]
    free = [(g["last_qf"], g["t1"] + 1e-9) for g in gaps] + [(rv["t0"], rv["t1"] + stop_limit_ms / 1000.0) for rv in reverses]  # accounted as the stop, or not judged

    checks = []

    def check(name, value, limit, ok, scope=""):
        checks.append({"name": name, "scope": scope, "value": None if value is None else round(value, 3) if isinstance(value, float) else value, "limit": limit, "ok": bool(ok)})

    out_regions, series = [], []
    for r in regions:
        if not r["entries"]:
            continue
        number = len(out_regions) + 1
        scope = "region %d (%s)" % (number, {"at": "start at " + r["label"], "jump": "jump to " + r["label"], "resume": "resume after a stop"}[r["kind"]])
        matched = [e for e in r["entries"] if e["t_a"] is not None]
        res = {"kind": r["kind"], "label": r["label"], "steps_expected": len(r["entries"]), "steps_heard": len(matched)}
        out_regions.append(res)
        if not matched:
            check("notes heard", 0, ">0", False, scope)
            continue
        first = min(matched, key=lambda e: e["t_a"])
        first_te = min(e["t_e"] for e in matched)
        reach = r["reach"] if r["reach"] is not None else -1e9
        if r["kind"] == "jump":
            base = max(r["loc_t"], reach)
            res["reloc_ms"] = max(0.0, (first["t_a"] - base) * 1000.0)
            check("relocation (ms)", res["reloc_ms"], "<=%.0f" % lim["reloc_ms"], res["reloc_ms"] <= lim["reloc_ms"], scope)
            free.append((r["loc_t"], first["t_a"]))  # the old position's last notes, until the new one sounds
        else:
            base = max(r["first_qf"] if r["first_qf"] is not None else r["loc_t"], reach)
            res["lock_ms"] = max(0.0, (first["t_a"] - base) * 1000.0)
            res["qf_to_first_note_ms"] = (first["t_a"] - r["first_qf"]) * 1000.0 if r["first_qf"] is not None else None
            check("lock (ms)", res["lock_ms"], "<=%.0f" % lim["lock_ms"], res["lock_ms"] <= lim["lock_ms"], scope)
        missing = [e for e in r["entries"] if e["t_a"] is None and e["t_e"] >= first_te]
        res["missing"] = len(missing)
        check("missing steps", len(missing), "<=%d" % lim["missing"], len(missing) <= lim["missing"], scope)
        series.extend((e["t_e"], (e["t_a"] - e["t_e"]) * 1000.0) for e in matched)

    extra = [t for t in unmatched if not any(a <= t <= b for a, b in free)]
    check("extra notes", len(extra), "<=%d" % lim["extra"], len(extra) <= lim["extra"], "all notes")

    phase = None
    if out_regions:
        series.sort()
        errs = [e for _, e in series]
        if len(errs) < lim["min_steps"]:
            check("steps measured", len(errs), ">=%d" % lim["min_steps"], False, "phase")
        else:
            xs = [t for t, _ in series]
            mean = statistics.fmean(errs)
            spread = statistics.pstdev(errs)
            worst = max(abs(e) for e in errs)
            slope = _linfit(xs, errs)  # ms per second
            drift = slope * (xs[-1] - xs[0])
            drift_lim = max(lim["drift_ms"], 3.0 * spread * math.sqrt(12.0 / len(errs)))
            phase = {"steps": len(errs), "mean_ms": mean, "spread_ms": spread, "worst_ms": worst, "drift_ms": drift, "drift_limit_ms": drift_lim, "span_s": xs[-1] - xs[0]}
            check("phase mean (ms)", mean, "|x|<=%.2f" % lim["phase_mean_ms"], abs(mean) <= lim["phase_mean_ms"], "phase")
            check("phase spread (ms)", spread, "<=%.2f" % lim["phase_spread_ms"], spread <= lim["phase_spread_ms"], "phase")
            check("phase worst (ms)", worst, "<=%.2f" % lim["phase_worst_ms"], worst <= lim["phase_worst_ms"], "phase")
            check("drift over the run (ms)", drift, "|x|<=%.2f" % drift_lim, abs(drift) <= drift_lim, "phase")
    else:
        check("notes heard", 0, ">0", False, "no step of the song was due")

    out_gaps = []
    for g in gaps:
        notes_after = [t for note, lst in by_note.items() for t in lst if g["last_qf"] < t <= g["t1"] + 1e-9]
        latency = max(0.0, (max(notes_after) - g["last_qf"]) * 1000.0) if notes_after else 0.0
        res = {"after_t": round(g["last_qf"], 3), "gap_s": round(g["t1"] - g["last_qf"], 3), "notes_after": len(notes_after), "stop_latency_ms": latency}
        out_gaps.append(res)
        check("stop latency (ms)", latency, "<=%.1f" % stop_limit_ms, latency <= stop_limit_ms, "stop after the quarter frame at %.2f s" % g["last_qf"])
    for rv in reverses:
        rv["notes_heard"] = sum(1 for note, lst in by_note.items() for t in lst if rv["t0"] <= t <= rv["t1"])
    result = {
        "regions": [{k: (round(v, 3) if isinstance(v, float) else v) for k, v in r.items()} for r in out_regions],
        "stops": [{k: (round(v, 3) if isinstance(v, float) else v) for k, v in g.items()} for g in out_gaps],
        "reverse": [{k: (round(v, 3) if isinstance(v, float) else v) for k, v in rv.items()} for rv in reverses],
        "phase": {k: (round(v, 3) if isinstance(v, float) else v) for k, v in phase.items()} if phase else None,
        "extra_notes": len(extra),
        "checks": checks,
        "pass": all(c["ok"] for c in checks) and bool(checks),
        "limits": lim,
    }
    return result


def summary_mtc_send(report):
    m = report["master"]
    lines = ["profile %s | %.2f fps, rate %.4f | %d quarter frames planned, %d sent, %d not sent | jitter +-%.2f ms | master lateness mean %.3f p99 %.3f max %.3f ms%s" % (
        report["profile"], report["fps"], report["rate"], m["qf_planned"], m["qf_sent"], m["qf_planned"] - m["qf_sent"], report["jitter_ms"],
        m.get("lateness_ms", {}).get("mean", 0.0), m.get("lateness_ms", {}).get("p99", 0.0), m.get("lateness_ms", {}).get("max", 0.0),
        " | " + (m.get("sched") or ("SCHED_FIFO" if m.get("sched_fifo") else "SCHED_OTHER (no rtprio)")))]
    if report.get("song") == "none":
        lines.append("sent, not analysed (--song none)")
        return "\n".join(lines)
    for i, r in enumerate(report["regions"]):
        what = {"at": "start at " + r["label"], "jump": "jump to " + r["label"], "resume": "resume after a stop"}[r["kind"]]
        lines.append("region %d (%s): %d steps due, %d heard%s" % (
            i + 1, what, r["steps_expected"], r["steps_heard"],
            "".join([", relocated %.1f ms after the jump" % r["reloc_ms"] if "reloc_ms" in r else "", ", lock %.1f ms" % r["lock_ms"] if "lock_ms" in r else "",
                     " (first quarter frame to first note %.1f ms)" % r["qf_to_first_note_ms"] if r.get("qf_to_first_note_ms") is not None else "",
                     ", missing %d" % r["missing"] if "missing" in r else ""])))
    if report["phase"]:
        p = report["phase"]
        lines.append("phase over %d notes (%.1f s): mean %.3f ms, spread %.3f ms, worst %.3f ms, drift %.3f ms over the run (limit %.2f ms)" % (
            p["steps"], p["span_s"], p["mean_ms"], p["spread_ms"], p["worst_ms"], p["drift_ms"], p["drift_limit_ms"]))
    for g in report["stops"]:
        lines.append("stop after the quarter frame at %.2f s (%.2f s without one): %d notes after it, the last %.1f ms later" % (g["after_t"], g["gap_s"], g["notes_after"], g["stop_latency_ms"]))
    for rv in report["reverse"]:
        lines.append("reverse %.2f..%.2f s: %d quarter frames sent, %d notes heard (not judged: the spec does not follow a reverse)" % (rv["t0"], rv["t1"], rv["quarter_frames"], rv["notes_heard"]))
    lines.append("notes that matched no step and are no lock, relocation or stop: %d" % report["extra_notes"])
    bad = [c for c in report["checks"] if not c["ok"]]
    for c in bad:
        lines.append("FAIL  %s: %s = %s (limit %s)" % (c["scope"], c["name"], c["value"], c["limit"]))
    lines.append("%s (%d checks, %d failed)" % ("PASS" if report["pass"] else "FAIL", len(report["checks"]), len(bad)))
    return "\n".join(lines)


def mtc_send(out_dev, in_dev, profile_text, fps=25.0, rate=1.0, jitter_ms=0.0, seed=1, drop=None, start=(1, 0, 0, 0), song="midisync-song", channel=1, tail=3.0,
             limits=None, json_path=None, quiet=False, rtprio=RT_PRIO):
    """OS-MTC B1: be the MTC master on `out_dev` for the profile and listen to the unit's notes on `in_dev` (MTC.md)."""
    tokens = parse_mtc_send_profile(profile_text)
    plan = build_mtc_plan(tokens, fps, rate, jitter_ms, seed, drop, start, tail)
    listen = in_dev if song != "none" else None
    rfd = os.open(listen, os.O_RDONLY | os.O_NONBLOCK) if listen else None
    wfd = os.open(out_dev, os.O_WRONLY)
    try:
        events, info = run_mtc_master(plan, wfd, rfd, rtprio=rtprio)
    finally:
        os.close(wfd)
        if rfd is not None:
            os.close(rfd)
    report = {
        "tool": "midi_clock_tool mtc-send", "format": 1, "profile": profile_text, "fps": fps, "rate": rate, "jitter_ms": jitter_ms, "seed": seed, "drop": drop,
        "start": "%02d:%02d:%02d:%02d" % start, "song": song, "channel": channel, "tail_s": tail,
        "master": {"qf_planned": plan.qf_planned, "qf_sent": plan.qf_planned - plan.qf_dropped, "sched_fifo": info["sched_fifo"], "sched": info["sched"],
                   "lateness_ms": {k: round(v, 3) for k, v in info.get("lateness_ms", {}).items()}},
        "notes_heard": sum(1 for e in events if e[0] == "on"),
        "pass": True,
    }
    if song != "none":
        ons = [(e[1], e[3]) for e in events if e[0] == "on" and e[2] == channel - 1]
        report.update(analyze_mtc_send(plan, ons, song, limits))
    if json_path:
        with open(json_path, "w") as f:
            json.dump(report, f, indent=1)
    if not quiet:
        print(summary_mtc_send(report))
    return report


# ---- the synthetic chaser of the self-test -----------------------------------------------------------------------------------------------------------

class _MtcChaser:
    """A synthetic B3 (a unit that follows MTC), fed what the master sent. It takes a full frame as a provisional time, decodes cycles of eight pieces on its own (so
    the drop-frame labels go through mtc_label_to_frames), reads the master's time at piece 7 as the spelled frame + 1.75, keeps a line (time, position, rate) fitted over
    the last six cycles, plays the song's steps on it up to `hold_frames` past the last quarter frame that continued the forward order (then it is silent), and relocates
    when a cycle disagrees with the line by more than a frame or a full frame arrives, after `relock_s`. Reverse order (piece n-1 after n) unlocks it."""

    def __init__(self, steps, fps_real, delay=0.0005, noise=0.0, seed=7, hold_frames=3.0, relock_s=0.0):
        self.steps = steps                       # (wire note, target position in real frames)
        self.targets = [p for _, p in steps]
        self.fps, self.delay, self.noise, self.hold, self.relock_s = fps_real, delay, noise, hold_frames, relock_s
        self.rng = random.Random(seed)
        self.stream = MTCStream()
        self.nq = self.nf = 0
        self.locked = False
        self.line = None                         # (host time, position, frames a second)
        self.cyc = []
        self.idx = 0
        self.last_piece = None
        self.pieces = {}
        self.arm_t = 0.0
        self.out = []                            # [due, note]

    def _unlock(self, t):
        self.locked = False
        self.cyc = []
        self.out = [o for o in self.out if o[0] <= t]

    def _lock(self, t, pos, slope):
        self.line = (t, pos, slope)
        self.locked = True
        self.idx = bisect.bisect_left(self.targets, pos)
        self.arm_t = t + self.relock_s

    def _cycle(self, t, frames):
        pos = frames + 1.75
        if self.locked:
            ts, p, sl = self.line
            if abs(pos - (p + sl * (t - ts))) > 1.0:
                self._unlock(t)
        if not self.locked:
            self._lock(t, pos, self.fps)
            self.cyc = [(t, pos)]
            return
        self.cyc = (self.cyc + [(t, pos)])[-6:]
        if len(self.cyc) >= 3:
            xs = [c[0] for c in self.cyc]
            ys = [c[1] for c in self.cyc]
            mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
            self.line = (mx, my, _linfit(xs, ys))
        else:
            self.line = (t, pos, self.line[2])

    def _schedule(self, t):
        ts, p, sl = self.line
        while self.idx < len(self.targets):
            due = ts + (self.targets[self.idx] - p) / sl
            if due > t + self.hold / self.fps:
                break
            if due >= t - 0.002:
                self.out.append([due + self.delay + (self.rng.gauss(0.0, self.noise) if self.noise else 0.0), self.steps[self.idx][0]])
            self.idx += 1

    def feed(self, t, chunk):
        self.stream.feed(chunk, t)
        while self.nf < len(self.stream.full):
            ts, (h, m, s, f, rc) = self.stream.full[self.nf]
            self.nf += 1
            try:
                frames = mtc_label_to_frames((h, m, s, f), rc)
            except ValueError:
                continue
            self._unlock(ts)
            self._lock(ts, float(frames), self.fps)  # provisional: the time the full frame names, at the nominal rate
            self.last_piece, self.pieces = None, {}
        while self.nq < len(self.stream.qf):
            ts, piece, data = self.stream.qf[self.nq]
            self.nq += 1
            cont = self.last_piece is not None and piece == (self.last_piece + 1) % 8
            if self.last_piece is not None and piece == (self.last_piece - 1) % 8:
                self._unlock(ts)
            self.last_piece = piece
            if piece == 0:
                self.pieces = {0: data & 15}
            elif cont and self.pieces:
                self.pieces[piece] = data & 15
            else:
                self.pieces = {}
            if piece == 7 and len(self.pieces) == 8:
                n = self.pieces
                try:
                    frames = mtc_label_to_frames((((n[7] & 1) << 4) | n[6], (n[5] << 4) | n[4], (n[3] << 4) | n[2], (n[1] << 4) | n[0]), (n[7] >> 1) & 3)
                except ValueError:
                    frames = None
                self.pieces = {}
                if frames is not None:
                    self._cycle(ts, frames)
            if cont and self.locked and ts >= self.arm_t:
                self._schedule(ts)

    def pop_due(self, now):
        self.out.sort()
        due = []
        while self.out and self.out[0][0] <= now:
            due.append(self.out.pop(0))
        return due


def _chaser_steps(plan, song):
    """The steps as the chaser wants them: (wire note, position in real frames)."""
    return [(note, s * plan.fps_real) for note, s in _mtc_song_steps(plan, song)]


def _fake_chaser_ons(plan, song, **kw):
    """The note-ons of a synthetic chaser that heard the whole plan: [(arrival s, wire note)]."""
    ch = _MtcChaser(_chaser_steps(plan, song), plan.fps_real, **kw)
    for i, ev in enumerate(plan.events):
        ch.feed(_mtc_send_t(plan, i), ev[1])
    return sorted((due, note) for due, note in ch.out)


def _fifo_mtc_chaser(qf_path, note_path, ready, stop, steps, fps_real, **kw):
    """The chaser of the live self-test: reads the master's bytes from a FIFO, writes its notes to another."""
    rfd = os.open(qf_path, os.O_RDONLY | os.O_NONBLOCK)
    ready.set()
    wfd = os.open(note_path, os.O_WRONLY)
    ch = _MtcChaser(steps, fps_real, **kw)
    while not stop.is_set():
        timeout = 0.01
        if ch.out:
            timeout = max(0.0, min(0.01, min(o[0] for o in ch.out) - time.monotonic()))
        if select.select([rfd], [], [], timeout)[0]:
            try:
                data = os.read(rfd, 4096)
            except BlockingIOError:
                data = b""
            if data:
                ch.feed(time.monotonic(), data)
        for _due, note in ch.pop_due(time.monotonic()):
            os.write(wfd, bytes((0x90, note, 100)))
    os.close(rfd)
    os.close(wfd)


def _self_test_mtc_send():
    """The B1 self-test. No wall clock except the live loop at the end: the plans are built, played into a synthetic chaser with their own times, and analysed.
    The frame maths against A2's decoder and analysis (24, 25, 30 fps) and against the drop-frame rule; the analysis against a chaser with a known delay, noise, rate,
    jump, drop-out and stop, and against faulty chasers (late, slow to relocate, deaf to a drop-out, slow to stop); the master's faults (jitter, dropped pieces); and
    the live loop over FIFOs. The frame maths alone, against fixed vectors, is tests/mtc_send_test.py (ctest mtc_send_python)."""
    problems = []

    def measured(name, value, want, tol):
        if value is None or abs(value - want) > tol:
            problems.append("%s: measured %r, wanted %.4f +-%.4f" % (name, value, want, tol))

    def offline(profile, fps=25.0, song="midisync-song", build=None, chaser=None, limits=None):
        plan = build_mtc_plan(parse_mtc_send_profile(profile), fps, **(build or {}))
        plan.sent_t = [ev[0] for ev in plan.events]
        return plan, analyze_mtc_send(plan, _fake_chaser_ons(plan, song, **(chaser or {})), song, limits)

    def verdict(name, rep, want_pass, fails=()):
        failed = {c["name"] for c in rep["checks"] if not c["ok"]}
        if rep["pass"] != want_pass:
            problems.append("%s: pass=%s, wanted %s (%s)" % (name, rep["pass"], want_pass, sorted(failed)))
        for f in fails:
            if f not in failed:
                problems.append("%s: check %r did not fail (%s)" % (name, f, sorted(failed)))

    # the frame maths against A2's decoder and analysis: what the master sends at 24, 25 and 30 fps is what the A2 tool reads back and passes
    for fps, start in ((24.0, (1, 2, 3, 5)), (25.0, (17, 58, 59, 24)), (30.0, (22, 59, 58, 29))):
        plan = build_mtc_plan(parse_mtc_send_profile("at:%02d:%02d:%02d:%02d,run:3" % start), fps, start=start, tail=0.0)
        s = MTCStream()
        started = False
        for ev in plan.events:
            started = started or (ev[2] == "qf" and ev[3] % 8 == 0)  # A2's decoder counts a leading part cycle as a dropped piece (a run may start at any piece, spec A-2)
            if ev[2] == "full" or started:
                s.feed(ev[1], 100.0 + ev[0])
        res = mtc_analyze(s, fps_arg=fps, expect=start)
        if not res["pass"] or len(res["cycles"]) < int(3 * fps / 2) - 2:
            problems.append("%.0f fps: the A2 analysis of the master's stream: %r (cycles %d)" % (fps, [c for c in res["checks"] if not c["ok"]], len(res["cycles"])))
        if res["dropped_pieces"] or res["repeated_pieces"] or res["mixed_cycles"] or len(res["full_frames"]) != 1:
            problems.append("%.0f fps: pieces dropped %d repeated %d mixed %d, full frames %d" % (fps, res["dropped_pieces"], res["repeated_pieces"], res["mixed_cycles"], len(res["full_frames"])))
        first_qf = next(ev for ev in plan.events if ev[2] == "qf")
        if first_qf[0] < MTC_SEND_GAP_S - 1e-9:
            problems.append("%.0f fps: the first quarter frame at %.4f s, under the 4 ms after the full frame" % (fps, first_qf[0]))

    # a clean master and a perfect chaser (0.5 ms of output delay): the song starts 2 s after the pre-roll begins
    plan, rep = offline("at:00:59:58:00,run:10")
    verdict("pre-roll, 25 fps", rep, True)
    r = rep["regions"][0]
    measured("pre-roll: lock (ms)", r.get("lock_ms"), 0.5, 0.1)  # the first step sounds at the song start, the pre-roll is not the lock time
    measured("pre-roll: phase mean (ms)", rep["phase"]["mean_ms"], 0.5, 0.05)
    measured("pre-roll: phase spread (ms)", rep["phase"]["spread_ms"], 0.0, 0.05)
    if r["missing"] or rep["extra_notes"] or r["steps_heard"] != r["steps_expected"] or r["steps_expected"] < 60:
        problems.append("pre-roll: %r extra %d" % (r, rep["extra_notes"]))
    # the same song started with the master's clock already running: the lock is the time to the first note
    _, rep = offline("at:01:00:00:00,run:6")
    verdict("start on the downbeat", rep, True)
    if not 0.0 <= rep["regions"][0]["lock_ms"] <= 250.0:
        problems.append("start on the downbeat: lock %r" % rep["regions"][0])
    # all four rates, drop-frame included (the chaser decodes through mtc_label_to_frames, so a wrong label would put every note off by the difference)
    for fps in (24.0, 29.97, 30.0):
        _, rep = offline("at:00:59:58:00,run:8", fps=fps)
        verdict("%.2f fps" % fps, rep, True)
        measured("%.2f fps: phase mean (ms)" % fps, rep["phase"]["mean_ms"], 0.5, 0.1)
    # a master at 1.001 times the host's clock (and a chaser with noise): the notes follow the master's time, not the host's
    _, rep = offline("at:00:59:58:00,run:20", build={"rate": 1.001, "jitter_ms": 1.0, "seed": 4}, chaser={"noise": 0.0004})
    verdict("rate 1.001, jitter 1 ms", rep, True)
    measured("rate 1.001: phase mean (ms)", rep["phase"]["mean_ms"], 0.5, 0.4)
    # the same master against a chaser that cannot fit the rate would drift: here it can; and a chaser a whole step late fails
    _, rep = offline("at:00:59:58:00,run:10", chaser={"delay": 0.012})
    verdict("a chaser 12 ms late", rep, False, ("phase mean (ms)", "phase worst (ms)"))
    measured("a chaser 12 ms late: phase mean (ms)", rep["phase"]["mean_ms"], 12.0, 0.2)
    _, rep = offline("at:00:59:58:00,run:10", chaser={"delay": 0.0, "noise": 0.004, "seed": 3})
    verdict("a noisy chaser (4 ms)", rep, False, ("phase spread (ms)",))
    measured("a noisy chaser: phase spread (ms)", rep["phase"]["spread_ms"], 4.0, 0.8)
    # the master's own faults: +-2 ms on every quarter frame, one in twenty dropped: a chaser that holds over them stays in time
    plan, rep = offline("at:00:59:58:00,run:20", build={"jitter_ms": 2.0, "seed": 5, "drop": (1, 20)})
    verdict("jitter +-2 ms, 1/20 dropped", rep, True)
    if abs(plan.qf_dropped - (plan.qf_planned - 16) / 20.0) > 2.0:
        problems.append("drop 1/20: %d of %d quarter frames dropped" % (plan.qf_dropped, plan.qf_planned))
    if not all(plan.events[i][0] < plan.events[i + 1][0] for i in range(len(plan.events) - 1)):
        problems.append("the jitter reordered the events")

    # a jump: the full frame and the quarter frames from the new time; the relocation is measured from the full frame
    plan, rep = offline("at:00:59:59:00,run:3,jump:01:00:02:00,run:3")
    verdict("a jump", rep, True)
    if len(rep["regions"]) != 2 or rep["regions"][1]["kind"] != "jump" or not 50.0 < rep["regions"][1]["reloc_ms"] < 300.0:
        problems.append("a jump: %r" % (rep["regions"],))
    _, rep = offline("at:00:59:59:00,run:3,jump:01:00:02:00,run:3", chaser={"relock_s": 0.6})
    verdict("a chaser slow to relocate", rep, False, ("relocation (ms)",))
    # a drop-out of 0.1 s is a freewheel (the steps in it are due); a stop of a second is the stop (the latency is measured, the steps in it are not due)
    _, rep = offline("at:01:00:00:00,run:2.03,quiet:0.1,run:2")
    verdict("a drop-out of 0.1 s", rep, True)
    if len(rep["regions"]) != 1 or rep["stops"][0]["gap_s"] < 2.9:
        problems.append("a drop-out of 0.1 s: %r %r" % (rep["regions"], rep["stops"]))
    _, rep = offline("at:01:00:00:00,run:2.03,quiet:0.1,run:2", chaser={"hold_frames": 1.0})
    verdict("a chaser deaf to the drop-out", rep, False, ("missing steps",))
    plan, rep = offline("at:01:00:00:00,run:2.03,quiet:1,run:2.03,quiet:1")
    verdict("two stops", rep, True)
    if len(rep["regions"]) != 2 or rep["regions"][1]["kind"] != "resume" or len(rep["stops"]) != 2:
        problems.append("two stops: %r %r" % (rep["regions"], rep["stops"]))
    else:
        measured("stop latency (ms)", rep["stops"][0]["stop_latency_ms"], 105.0, 15.0)  # the chaser holds 3 frames (120 ms) past the last quarter frame; the step due at 2.125 s sounds
    _, rep = offline("at:01:00:00:00,run:2.03,quiet:1,run:2", chaser={"hold_frames": 12.0})
    verdict("a chaser slow to stop", rep, False, ("stop latency (ms)",))
    measured("a chaser slow to stop: latency (ms)", rep["stops"][0]["stop_latency_ms"], 420.0, 65.0)  # held 12 frames (480 ms): the last step due in it sounds, 355 to 480 ms after
    # a master that ends without a final quiet is followed by the tail: the stop latency is measured there
    _, rep = offline("at:01:00:00:00,run:2.03")
    if len(rep["stops"]) != 1 or rep["stops"][0]["gap_s"] < 2.9:
        problems.append("the tail: %r" % (rep["stops"],))
    # reverse: sent, reported, not judged; the plan runs back and the pieces go 7..0
    plan = build_mtc_plan(parse_mtc_send_profile("at:01:00:05:00,reverse:1"), 25.0, tail=0.0)
    qs = [ev[3] for ev in plan.events if ev[2] == "qf"]
    if len(qs) < 95 or any(b != a - 1 for a, b in zip(qs, qs[1:])) or mtc_frames_to_label(2 * (qs[0] // 8), 1)[:3] != (1, 0, 4):
        problems.append("reverse: %d quarter frames, first %r, descending %s" % (len(qs), qs[:3], all(b == a - 1 for a, b in zip(qs, qs[1:]))))
    _, rep = offline("at:01:00:01:00,run:2.03,reverse:1,run:2", limits={"lock_ms": 500.0})
    if len(rep["reverse"]) != 1 or rep["reverse"][0]["notes_heard"] != 0 or rep["reverse"][0]["quarter_frames"] < 95:
        problems.append("reverse: %r" % (rep["reverse"],))
    # errors in the profile
    for bad in ("run:5,bogus:1", "at:25:00:00:00,run:1", "at:01:00:00:00", "run:-1", "run:x", "jump:01:00:00:00,run:1,at"):
        try:
            parse_mtc_send_profile(bad)
        except ValueError:
            continue
        problems.append("the profile %r was accepted" % bad)
    build_mtc_plan(parse_mtc_send_profile("at:01:10:00:00,run:1"), 29.97)  # frames 00 and 01 of a tenth minute exist
    try:
        build_mtc_plan(parse_mtc_send_profile("at:01:01:00:00,run:1"), 29.97)
        problems.append("a drop-frame label that does not exist was accepted")
    except ValueError:
        pass

    # the live loop: the master over a FIFO, the synthetic chaser on the other
    with tempfile.TemporaryDirectory() as tmp:
        qfs = os.path.join(tmp, "to_unit")
        notes = os.path.join(tmp, "from_unit")
        os.mkfifo(qfs)
        os.mkfifo(notes)
        plan = build_mtc_plan(parse_mtc_send_profile("at:00:59:59:00,run:3"), 25.0, jitter_ms=0.3, seed=2, tail=1.0)
        ready, stop = threading.Event(), threading.Event()
        rfd = os.open(notes, os.O_RDONLY | os.O_NONBLOCK)
        th = threading.Thread(target=_fifo_mtc_chaser, args=(qfs, notes, ready, stop, _chaser_steps(plan, "midisync-song"), plan.fps_real), kwargs={"delay": 0.003})
        th.start()
        ready.wait(5)
        wfd = os.open(qfs, os.O_WRONLY)
        try:
            events, info = run_mtc_master(plan, wfd, rfd, 0.3)
        finally:
            stop.set()
            th.join()
            os.close(wfd)
            os.close(rfd)
    rep = analyze_mtc_send(plan, [(e[1], e[3]) for e in events if e[0] == "on"], "midisync-song")
    if not rep["regions"] or rep["regions"][0].get("missing") or rep["extra_notes"] or rep["regions"][0]["steps_heard"] < 8:
        problems.append("live loop: %r extra %d" % (rep["regions"], rep["extra_notes"]))
    elif not 1.0 <= rep["phase"]["mean_ms"] <= 6.0 or rep["phase"]["spread_ms"] > 2.5 or rep["regions"][0]["lock_ms"] > 250.0:
        problems.append("live loop: phase %r (the chaser delays 3 ms), lock %r" % (rep["phase"], rep["regions"][0]))
    if not rep["stops"] or rep["stops"][0]["stop_latency_ms"] > 200.0:
        problems.append("live loop: stop %r" % (rep["stops"],))
    if not str(info.get("sched", "")).startswith("SCHED_") or info.get("sched_fifo") != info["sched"].startswith("SCHED_FIFO"):
        problems.append("live loop: the master's scheduler not reported: %r" % (info,))
    note = "frames against A2's decoder at 24/25/30 fps, drop-frame, pre-roll lock, rate 1.001, jitter and drops, jump, drop-out, stop, reverse, live loop"
    return problems, note


def _self_test_sync():
    problems = []

    def expect(name, report, want_pass, fails=()):
        failed = {c["name"] for c in report["checks"] if not c["ok"]}
        if report["pass"] != want_pass:
            problems.append("%s: pass=%s, wanted %s (%s)" % (name, report["pass"], want_pass, sorted(failed)))
        for f in fails:
            if f not in failed:
                problems.append("%s: check %r did not fail (%s)" % (name, f, sorted(failed)))

    def measured(name, value, want, tol):
        """Assert a number the analysis measured, not just that nothing raised."""
        if value is None or abs(value - want) > tol:
            problems.append("%s: measured %r, wanted %.3f +-%.3f" % (name, value, want, tol))

    def offline(profile, **kw):
        fk = {k: kw.pop(k) for k in list(kw) if k in ("delay", "noise", "s0", "drift_per_s", "skip", "dup", "lag", "cycle", "seed", "follow_clocks", "spp_shift", "cont_restart")}
        build_kw = {k: kw.pop(k) for k in list(kw) if k in ("jitter_ms", "drop", "burst")}
        plan = build_plan(parse_sync_profile(profile), **build_kw)
        for ev in plan.events:  # the send time of every clock that went out (the jitter included); a dropped or quiet clock stays None
            if ev[2] == "clock":
                plan.sent[ev[3]] = ev[0] if plan.will_send[ev[3]] else None
        cycle = fk.get("cycle", 16)
        events = fake_unit_events(plan, **fk)
        return plan, analyze(plan, events, 36, cycle, 1)

    # the grid, the drops, the jitter
    plan = build_plan(parse_sync_profile("120:2"), jitter_ms=1.0, seed=5, drop=(1, 24), burst=(100, 3))
    if len(plan.ideal) != 96 or abs(plan.ideal[24] - 0.5) > 1e-9:
        problems.append("grid: %d clocks, clock 24 at %.4f s" % (len(plan.ideal), plan.ideal[24]))
    sent_n = sum(plan.will_send)
    if sent_n != 94:
        # 96 clocks; --drop 1/24 drops one clock in each 24-clock window past the first 48 (two windows here); the burst at 100 is past the end
        problems.append("drop 1/24: sent %d of 96, wanted 94" % sent_n)
    if not all(plan.events[i][0] <= plan.events[i + 1][0] for i in range(len(plan.events) - 1)):
        problems.append("the jitter reordered the events")
    if [e[2] for e in plan.events if e[2] != "clock"] != ["start", "stop"]:
        problems.append("auto start / stop missing")
    burst_plan = build_plan(parse_sync_profile("240:4"), burst=(100, 6))
    if sum(burst_plan.will_send) != 384 - 6 or [(d["kind"], d["c_from"], d["c_to"]) for d in burst_plan.dist] != [("burst", 100, 106)]:
        problems.append("burst 100:6: sent %d of 384, dist %r" % (sum(burst_plan.will_send), burst_plan.dist))
    ramp = build_plan(parse_sync_profile("ramp:100-180:30,quiet:2"), auto_transport=False)
    if not ramp.dist or ramp.dist[0]["kind"] != "ramp" or abs(len([1 for w in ramp.will_send if w]) - 30 * 140 * 24 / 60.0) > 60:
        problems.append("ramp 100-180 over 30 s: %d clocks" % len([1 for w in ramp.will_send if w]))
    jump = build_plan(parse_sync_profile("120:5,140:5"))
    if [d["kind"] for d in jump.dist] != ["jump"]:
        problems.append("a second tempo is not a jump")

    # the analysis against a unit with a known delay, noise, drift, a skipped and a duplicated step
    _, rep = offline("240:12", delay=0.003, noise=0.0004)
    expect("perfect unit", rep, True)
    r0 = rep["runs"][0]
    measured("perfect unit: offset (ms)", r0["offset_ms"], 3.0, 0.2)
    measured("perfect unit: phase mean (ms)", r0["phase_mean_ms"], 3.0, 0.3)
    measured("perfect unit: phase std (ms)", r0["phase_std_ms"], 0.4, 0.15)
    if r0["missing"] or r0["extra"] or r0["lock_steps"] != 0 or r0["steps_after_lock"] != 192:
        problems.append("perfect unit: missing %d, extra %d, lock %s, after lock %d (wanted 0, 0, 0, 192)" % (
            r0["missing"], r0["extra"], r0["lock_steps"], r0["steps_after_lock"]))
    _, rep = offline("240:12", noise=0.002)
    expect("noisy unit (2 ms)", rep, False, ("phase std (ms)",))
    measured("noisy unit: phase std (ms)", rep["runs"][0]["phase_std_ms"], 2.0, 0.4)
    _, rep = offline("240:12", drift_per_s=0.0004)
    expect("drifting unit (0.4 ms/s)", rep, False, ("drift over the run (ms)",))
    r = rep["runs"][0]
    measured("drifting unit: drift over the run (ms)", r["drift_ms"], 0.0004 * r["span_s"] * 1000.0, 0.3)
    _, rep = offline("240:12", skip=(20,))
    expect("a step missing", rep, False, ("missing steps",))
    r = rep["runs"][0]
    if r["missing"] != 1 or r["missing_steps"] != [20]:
        problems.append("a step missing: missing %d %r, wanted 1 [20]" % (r["missing"], r["missing_steps"]))
    _, rep = offline("240:12", dup=(20,))
    expect("a step doubled", rep, False, ("extra notes",))
    if rep["runs"][0]["extra"] != 1:
        problems.append("a step doubled: extra %d, wanted 1" % rep["runs"][0]["extra"])
    # the master's own faults (jitter, dropped clocks, a burst) are the follower's to absorb: a unit on the ideal grid passes
    _, rep = offline("240:12", jitter_ms=2.0, seed=3, drop=(1, 12), delay=0.003, noise=0.0004)
    expect("master jitter +-2 ms and 1/12 clocks dropped, perfect follower", rep, True)
    if rep["runs"][0]["missing"] or rep["runs"][0]["extra"]:
        problems.append("master faults: missing %d, extra %d" % (rep["runs"][0]["missing"], rep["runs"][0]["extra"]))
    _, rep = offline("240:4", burst=(100, 6), delay=0.003, noise=0.0004)
    expect("a burst of 6 dropped clocks, perfect follower", rep, True)
    d = rep["disturbances"][0]
    if d["kind"] != "burst" or d["settle_ms"] != 0.0 or not d["settled"]:
        problems.append("burst: %r" % (d,))
    _, rep = offline("120:3,start,120:10,stop,120:1,spp:20,cont,120:10,stop", s0=20, cycle=48)
    expect("Start, Stop, SPP 20 + Continue (cycle 48)", rep, True)
    if len(rep["runs"]) != 2 or rep["runs"][1].get("first_step_heard") != 20 or not rep["runs"][1].get("first_step_ok"):
        problems.append("SPP run: %r" % (rep["runs"],))
    _, rep = offline("120:3,start,120:10,stop,120:1,spp:20,cont,120:10,stop", spp_shift=1, cycle=48)
    expect("SPP at the wrong step", rep, False, ("first step",))
    if rep["runs"][0].get("first_step_ok") is not True or rep["runs"][1].get("first_step_ok") is not False:
        problems.append("SPP at the wrong step not seen: %r" % (rep["runs"][1],))
    _, rep = offline("start,120:10,spp:8,cont,120:10")  # a run ended by the restart: the next run's first notes must not count as its extras
    expect("SPP 8 + Continue in Pattern (cycle 16)", rep, True)
    if len(rep["runs"]) != 2 or rep["runs"][1].get("first_step_heard") != 8 or rep["runs"][0].get("extra") or rep["runs"][1].get("extra"):
        problems.append("SPP pattern run: %r" % (rep["runs"],))
    # a bare Continue (no SPP): the expected first step is the step after the previous run's last step (40 steps at 120 BPM: step 40 = 8 mod 16)
    _, rep = offline("start,120:5,cont,120:5")
    expect("a bare Continue that resumes after the previous run's last step", rep, True)
    if len(rep["runs"]) != 2 or rep["runs"][1].get("first_step_expected") != 8 or not rep["runs"][1].get("first_step_ok"):
        problems.append("bare Continue, good unit: %r" % (rep["runs"],))
    _, rep = offline("start,120:5,cont,120:5", cont_restart=True)
    expect("a bare Continue that restarts from step 0", rep, False, ("first step",))
    if rep["runs"][1].get("first_step_expected") != 8 or rep["runs"][1].get("first_step_heard") != 0:
        problems.append("bare Continue from step 0 not seen: %r" % (rep["runs"][1],))
    # a short noisy run: with ~1 ms of noise over 19 steps the slope noise alone is about 0.8 ms, over the old fixed 0.5 ms limit; the limit grows to 3*sigma*sqrt(12/n)
    _, rep = offline("240:1.2", noise=0.001, seed=3)
    expect("a short noisy run (19 steps, 1 ms noise) against the adaptive drift limit", rep, True)
    r = rep["runs"][0]
    if r["steps_after_lock"] < 16:
        problems.append("short noisy run: only %d steps after lock" % r["steps_after_lock"])
    if not abs(r["drift_ms"]) > 0.5:
        problems.append("short noisy run: drift %.3f ms, wanted |drift| above the old 0.5 ms limit (the case must be one the old limit failed by chance)" % r["drift_ms"])
    want_lim = max(0.5, 3.0 * r["phase_std_ms"] * math.sqrt(12.0 / r["steps_after_lock"]))
    measured("short noisy run: the drift limit used (ms)", r.get("drift_limit_ms"), want_lim, 0.05)
    if not r.get("drift_limit_ms", 0.0) > 0.5:
        problems.append("short noisy run: drift limit %.3f ms, wanted above 0.5" % r.get("drift_limit_ms", -1))
    # a run cut by a Continue must sound its last steps too: a dropped final step is a missing step now
    _, rep = offline("start,120:5,cont,120:5", skip=(39,))
    expect("the last step of a run cut by a Continue dropped", rep, False, ("missing steps",))
    if rep["runs"][0]["missing_steps"] != [39]:
        problems.append("dropped final step: missing %r, wanted [39]" % (rep["runs"][0]["missing_steps"],))
    # a unit a whole step (125 ms at 120 BPM) late on the Continue run: the phase spread is fine, the absolute offset (half a step = 62.5 ms at 120 BPM) catches it
    _, rep = offline("start,120:5,cont,120:4,start,120:3", lag=lambda t: 0.125 if 5.0 <= t < 9.0 else 0.0)
    expect("a unit a whole step late on the Continue run", rep, False, ("offset (ms)",))
    r = rep["runs"][1]
    if not r.get("offset_ms", 0.0) > r.get("offset_limit_ms", 1e9):
        problems.append("whole step late: offset %.1f ms, limit %.1f ms" % (r.get("offset_ms", -1), r.get("offset_limit_ms", -1)))
    _, rep = offline("120:10,140:15", lag=lambda t: 0.012 * math.exp(-(t - 10.0) / 0.5) if t >= 10.0 else 0.0)
    expect("a jump that settles in a second", rep, True)
    measured("jump settle, fast (ms)", rep["disturbances"][0].get("settle_ms"), 1000.0 * 0.5 * math.log(3.0), 150.0)  # 12 ms decaying into the 4 ms band: tau * ln(12/4)
    _, rep = offline("120:10,140:15", lag=lambda t: 0.012 * math.exp(-(t - 10.0) / 3.0) if t >= 10.0 else 0.0)
    expect("a jump that settles in 10 s", rep, False, ("settle (ms)",))
    measured("jump settle, slow (ms)", rep["disturbances"][0].get("settle_ms"), 1000.0 * 3.0 * math.log(3.0), 150.0)
    _, rep = offline("120:6,quiet:6", follow_clocks=48)
    expect("a master that goes quiet, the unit stops after two beats", rep, True)
    q = rep["quiet"][0]
    if q["notes_after_last_clock"] != STALE_CLOCKS // CLOCKS_PER_STEP or q["late_ms"] > 0.0:
        problems.append("quiet, in time: %r" % (q,))
    _, rep = offline("120:6,quiet:6", follow_clocks=160)
    expect("a master that goes quiet, the unit plays on", rep, False, ("last note-on past the stale horizon (ms)",))
    q = rep["quiet"][0]
    measured("quiet, stale follow: notes after the last clock", q["notes_after_last_clock"], 27, 0)  # 160 clocks = 26 2/3 steps
    if q["late_ms"] <= 2000.0:
        problems.append("quiet, stale follow: late %.1f ms, wanted above 2000" % q["late_ms"])
    _, rep = offline("100:20,ramp:100-180:30,180:10", delay=0.003, noise=0.0004, lag=lambda t: 0.0)
    expect("a ramp followed exactly", rep, True)
    d = rep["disturbances"][0]
    if d["kind"] != "ramp" or d["max_dev_ms"] >= 3.0:
        problems.append("ramp: %r" % (d,))

    # the master's scheduler: no root needed, a refusal is said in the report words, the user's limit caps the priority (os.sched_setscheduler faked: nothing is changed on this machine)
    real_set = os.sched_setscheduler if hasattr(os, "sched_setscheduler") else None
    tried = []

    def fake_set(allowed):
        def f(pid, policy, param):
            tried.append(param.sched_priority)
            if param.sched_priority > allowed:
                raise PermissionError(1, "Operation not permitted")
        return f
    try:
        if real_set is not None:
            os.sched_setscheduler = fake_set(0)
            got = enter_realtime(20, max_prio=0)
            if got != {"fifo": False, "prio": 0, "sched": "SCHED_OTHER (no rtprio)"} or tried != [20]:
                problems.append("rt: refused: %r after %r" % (got, tried))
            tried.clear()
            os.sched_setscheduler = fake_set(5)
            got = enter_realtime(20, max_prio=5)
            if got != {"fifo": True, "prio": 5, "sched": "SCHED_FIFO 5"} or tried != [20, 5]:
                problems.append("rt: capped by the limit: %r after %r" % (got, tried))
            tried.clear()
            os.sched_setscheduler = fake_set(99)
            got = enter_realtime(20, max_prio=99)
            if got != {"fifo": True, "prio": 20, "sched": "SCHED_FIFO 20"} or tried != [20]:
                problems.append("rt: granted: %r after %r" % (got, tried))
            tried.clear()
        if enter_realtime(0)["sched"] != "SCHED_OTHER (--rtprio 0)" or tried:
            problems.append("rt: --rtprio 0 must not try")
    finally:
        if real_set is not None:
            os.sched_setscheduler = real_set

    # the live loop: the master over a FIFO, the synthetic unit on the other
    with tempfile.TemporaryDirectory() as tmp:
        clocks = os.path.join(tmp, "to_unit")
        notes = os.path.join(tmp, "from_unit")
        os.mkfifo(clocks)
        os.mkfifo(notes)
        ready, stop = threading.Event(), threading.Event()
        rfd = os.open(notes, os.O_RDONLY | os.O_NONBLOCK)
        th = threading.Thread(target=_fifo_unit, args=(clocks, notes, ready, stop))
        th.start()
        ready.wait(5)
        wfd = os.open(clocks, os.O_WRONLY)
        plan = build_plan(parse_sync_profile("240:1,start,240:4"), jitter_ms=0.5, seed=2)
        try:
            events, info = run_master(plan, wfd, rfd, 1.0, 0.3)
        finally:
            stop.set()
            th.join()
            os.close(wfd)
            os.close(rfd)
    rep = analyze(plan, events, 36, 16, 1)
    if not str(info.get("sched", "")).startswith("SCHED_") or "over_1ms" not in info.get("lateness_ms", {}):
        problems.append("live loop: the master's scheduler / lateness not reported: %r %r" % (info.get("sched"), info.get("lateness_ms")))
    if len(rep["runs"]) != 1 or rep["runs"][0].get("missing") or rep["runs"][0].get("extra"):
        problems.append("live loop: %r" % (rep["runs"],))
    else:
        r = rep["runs"][0]
        if not 2.0 <= r["offset_ms"] <= 5.0 or r.get("phase_std_ms", 9.0) > 1.5:
            problems.append("live loop: offset %.2f ms (the unit delays 3 ms), std %.3f ms" % (r["offset_ms"], r.get("phase_std_ms", -1)))
    if not rep["stops"] or rep["stops"][0]["hung_notes"] or "silence_ms" not in rep["stops"][0]:
        problems.append("live loop: stop %r" % (rep["stops"],))
    return problems, rep


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        fifo = os.path.join(tmp, "midi")
        os.mkfifo(fifo)
        out = {}

        def reader():
            out["r"] = measure(fifo, 6.0, quiet=True)

        th = threading.Thread(target=reader)
        th.start()
        time.sleep(0.2)
        n = send(fifo, parse_profile("120:2,150:2"), start=True, stop=True)
        th.join()
        r = out["r"]
        problems = []
        if r["clocks"] != n:
            problems.append("sent %d clocks, read %d" % (n, r["clocks"]))
        if not any(b == 0xFA for _, b in r["events"]) or not any(b == 0xFC for _, b in r["events"]):
            problems.append("Start / Stop not seen: %r" % (r["events"],))
        w = r.get("windows", [])
        if len(w) < 3 or not (115 <= w[0] <= 125) or not (145 <= w[-1] <= 155):
            problems.append("windows %r are not 120 then 150 BPM" % (w,))
    sync_problems, rep = _self_test_sync()
    problems += sync_problems
    mtc_problems, mtc_note = _self_test_mtc()
    problems += mtc_problems
    send_problems, send_note = _self_test_mtc_send()
    problems += send_problems
    if problems:
        print("midi_clock_tool self-test FAILED: " + "; ".join(problems))
        return 1
    r0 = rep["runs"][0]
    print("midi_clock_tool self-test passed (%d clocks, windows %s; sync: live loop offset %.2f ms std %.3f ms; mtc: %s; mtc-send: %s)" % (
        n, " ".join("%.1f" % x for x in w), r0["offset_ms"], r0["phase_std_ms"], mtc_note, send_note))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("list")
    m = sub.add_parser("measure")
    m.add_argument("dev")
    m.add_argument("--seconds", type=float, default=20.0)
    s = sub.add_parser("send")
    s.add_argument("dev")
    s.add_argument("--profile", required=True)
    s.add_argument("--start", action="store_true")
    s.add_argument("--stop", action="store_true")
    s.add_argument("--continue", dest="cont", action="store_true", help="FB (Continue) first instead of FA (OS-HIL2)")
    y = sub.add_parser("sync", help="OS-MIDISYNC: master and listener, analysis of the unit's clock follow")
    y.add_argument("dev", help="the raw MIDI node, written (clock) and read (notes)")
    y.add_argument("--in", dest="in_dev", help="read the notes from this node instead (the self-test uses a second FIFO)")
    y.add_argument("--profile", required=True, help="tokens: BPM:SEC, ramp:B0-B1:SEC, quiet:SEC, start, cont, stop, spp:N (see the top of this file)")
    y.add_argument("--jitter", type=float, default=0.0, metavar="MS", help="uniform +-MS on each clock's send time")
    y.add_argument("--seed", type=int, default=1)
    y.add_argument("--drop", metavar="K/N", help="K of every N clocks are not sent (never in the first 48)")
    y.add_argument("--drop-burst", metavar="AT:LEN", help="LEN clocks from clock AT are not sent")
    y.add_argument("--base", type=int, default=36, help="the note of step 0 (the test projects: 36; pattern 2 of the song project: 52)")
    y.add_argument("--cycle", type=int, default=16, help="steps before the notes repeat: 16 for a pattern, 48 for the three-pattern song")
    y.add_argument("--channel", type=int, default=1, help="the MIDI channel of the test instrument (1..16)")
    y.add_argument("--lead-ms", type=float, default=2.0, help="transport bytes go out this long before the next clock")
    y.add_argument("--tail", type=float, default=3.0, help="seconds to listen after the last byte")
    y.add_argument("--rtprio", type=int, default=RT_PRIO, metavar="N", help="the master's SCHED_FIFO priority (default %d; capped at the user's RLIMIT_RTPRIO, no root needed; 0: stay SCHED_OTHER); the report line says what it got" % RT_PRIO)
    y.add_argument("--no-auto-transport", action="store_true", help="do not add a Start first / a Stop last")
    y.add_argument("--limit", action="append", default=[], metavar="KEY=VALUE", help="override a threshold (%s)" % ", ".join(sorted(LIMITS)))
    y.add_argument("--json", metavar="FILE", help="write the full report here")
    y.add_argument("--raw", action="store_true", help="put the per-step error series and the unit's events in the JSON")
    # OS-MTC A2: the MTC listen and analyse subcommand
    m = sub.add_parser("mtc", help="OS-MTC A2: listen and analyse the MTC the unit sends (MTC.md)")
    m.add_argument("dev", help="the raw MIDI node to read from")
    m.add_argument("--seconds", type=float, default=20.0, help="duration to listen (default 20 seconds)")
    m.add_argument("--fps", type=float, default=None, metavar="F", help="expected fps rate, checked against the rate code (24, 25, 29.97, 30)")
    m.add_argument("--expect", metavar="HH:MM:SS:FF", help="expected MTC time at the run's start: the full frames are checked against it and the notes against it (default with --song: 01:00:00:00, MTC START)")
    m.add_argument("--song", choices=["midisync-song", "mtc-tempo"], help="the song playing: the tool knows its plan and computes the expected time of every step, so each note is measured against its song time + --expect")
    m.add_argument("--json", metavar="FILE", help="write the full report here")
    # OS-MTC B1: the host as the MTC master
    ms = sub.add_parser("mtc-send", help="OS-MTC B1: be the MTC master and listen to the unit's notes (MTC.md)")
    ms.add_argument("dev", help="the raw MIDI node, written (time code) and read (notes)")
    ms.add_argument("--in", dest="in_dev", help="read the notes from this node instead")
    ms.add_argument("--profile", required=True, help="tokens: at:HH:MM:SS:FF, run:SEC, quiet:SEC, jump:HH:MM:SS:FF, reverse:SEC (see the top of this file)")
    ms.add_argument("--fps", type=float, default=25.0, help="24, 25, 29.97 (drop-frame) or 30 (default 25)")
    ms.add_argument("--rate", type=float, default=1.0, metavar="R", help="the master's clock against the host's (default 1; 1.001 = 0.1 %% fast; 0.5..1.5)")
    ms.add_argument("--jitter", type=float, default=0.0, metavar="MS", help="uniform +-MS on each quarter frame's send time")
    ms.add_argument("--seed", type=int, default=1)
    ms.add_argument("--drop", metavar="K/N", help="K of every N quarter frames are not sent (never in the first 16)")
    ms.add_argument("--start", default="01:00:00:00", metavar="HH:MM:SS:FF", help="the song's start on the master's clock (MTC START; default 01:00:00:00)")
    ms.add_argument("--song", choices=["midisync-song", "mtc-tempo", "none"], default="midisync-song", help="the song playing (its plan gives every step's expected time); none: send only")
    ms.add_argument("--channel", type=int, default=1, help="the MIDI channel of the test instrument (1..16)")
    ms.add_argument("--tail", type=float, default=3.0, help="seconds to listen after the master stops (the stop test; default 3)")
    ms.add_argument("--limit", action="append", default=[], metavar="KEY=VALUE", help="override a threshold (%s)" % ", ".join(sorted(MTC_SEND_LIMITS)))
    ms.add_argument("--json", metavar="FILE", help="write the full report here")
    ms.add_argument("--rtprio", type=int, default=RT_PRIO, metavar="N", help="the master's SCHED_FIFO priority (default %d; capped at the user's RLIMIT_RTPRIO, no root needed; 0: stay SCHED_OTHER); the report line says what it got" % RT_PRIO)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "list":
        list_nodes()
        return 0
    if args.command == "measure":
        measure(args.dev, args.seconds)
        return 0
    if args.command == "send":
        n = send(args.dev, parse_profile(args.profile), args.start, args.stop, args.cont)
        print("sent %d clocks%s%s" % (n, " after Start" if args.start else "", ", then Stop" if args.stop else ""))
        return 0
    if args.command == "sync":
        try:
            limits = {}
            for item in args.limit:
                key, _, value = item.partition("=")
                if key not in LIMITS:
                    raise ValueError("unknown limit %r" % key)
                limits[key] = float(value)
            drop = tuple(int(x) for x in args.drop.split("/")) if args.drop else None
            burst = tuple(int(x) for x in args.drop_burst.split(":")) if args.drop_burst else None
            if drop is not None and not (len(drop) == 2 and 0 < drop[0] < drop[1]):
                raise ValueError("--drop takes K/N with 0 < K < N")
            if burst is not None and len(burst) != 2:
                raise ValueError("--drop-burst takes AT:LEN")
            report = sync(args.dev, args.in_dev or args.dev, args.profile, args.jitter, args.seed, drop, burst, args.base, args.cycle, args.channel, args.lead_ms,
                          args.tail, not args.no_auto_transport, limits, args.json, args.raw, False, args.rtprio)
        except ValueError as error:
            print("sync: %s" % error, file=sys.stderr)
            return 2
        return 0 if report["pass"] else 1
    if args.command == "mtc":
        try:
            expect = parse_mtc_time(args.expect) if args.expect else ((1, 0, 0, 0) if args.song else None)  # MTC START's default 01:00:00:00
            result = mtc(args.dev, args.seconds, args.fps, expect, args.song, args.json)
        except ValueError as error:
            print("mtc: %s" % error, file=sys.stderr)
            return 2
        return 0 if result["pass"] else 1
    if args.command == "mtc-send":
        try:
            limits = {}
            for item in args.limit:
                key, _, value = item.partition("=")
                if key not in MTC_SEND_LIMITS:
                    raise ValueError("unknown limit %r" % key)
                limits[key] = float(value)
            drop = tuple(int(x) for x in args.drop.split("/")) if args.drop else None
            if drop is not None and not (len(drop) == 2 and 0 < drop[0] < drop[1]):
                raise ValueError("--drop takes K/N with 0 < K < N")
            if not 1 <= args.channel <= 16:
                raise ValueError("--channel takes 1..16")
            start = parse_mtc_time(args.start)
            mtc_label_to_frames(start, mtc_rate_info(args.fps)[0])  # the start must exist at this rate
            report = mtc_send(args.dev, args.in_dev or args.dev, args.profile, args.fps, args.rate, args.jitter, args.seed, drop, start, args.song, args.channel, args.tail,
                              limits, args.json, rtprio=args.rtprio)
        except ValueError as error:
            print("mtc-send: %s" % error, file=sys.stderr)
            return 2
        return 0 if report["pass"] else 1
    parser.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
