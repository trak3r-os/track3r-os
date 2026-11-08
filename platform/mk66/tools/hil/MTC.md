# MTC.md — the MTC host tools (OS-MTC slices A2 and B1)

Two subcommands of `midi_clock_tool.py`: `mtc` listens to the time code the unit sends and analyses it (A2, MTC OUT); `mtc-send` is the host as the time-code master and listens to the notes the unit plays back (B1, MTC IN). DIN is not covered by either: this host has no MIDI interface.

## `mtc`: listen and analyse (A2)

`midi_clock_tool.py mtc` listens on the unit's USB MIDI node and analyses the MIDI Time Code the unit sends there: it decodes the quarter frames (`F1 0nnn dddd`, piece 7 is `0111 0 rr h`) and the full-frame SysEx (`F0 7F 7F 01 01 hh mm ss ff F7`), timestamps everything with the host's monotonic clock, and reports. A USB MIDI packet is 1 ms, so single events are known to about ±1–2 ms; the means and fits over a run are much better. DIN is not covered: this host has no MIDI interface.

```
DEV=/dev/snd/midiC2D0                       # from `midi_clock_tool.py list` (the node of the unit, group audio)
T="python3 -I platform/mk66/tools/hil/midi_clock_tool.py"
$T --self-test                              # no device: synthetic streams with fed timestamps (ctest midi_clock_tool_selftest)
$T mtc $DEV --seconds 60 --fps 25 --expect 01:00:00:00 --song mtc-tempo --json out/mtc.json
```

On the unit: the `mtc-tempo` song on the card (`<build>/o3pj_tool gen mtc-tempo /media/<card>/Projects/MTCTempo/project.o3pj`), **MTC OUT** set on the Config MIDI page (A3) to the fps of `--fps`, **MTC START** 01 (the default, what `--expect` names), the project open on the Song screen, then Play. `--song mtc-tempo` makes the tool walk the song's plan (the same walk as `seq/src/song_time.cpp`: the project tempo 120 BPM, the Tempo FX 140 BPM at slot 2 step 1, the Tempo fine +1.00 BPM and the Break in slot 3) and measure every note against its song time + `--expect`.

What the report says (the summary on stdout, the full JSON with `--json`):

- **rate code** — piece 7's / the full frame's rate code (0 = 24, 1 = 25, 2 = 29.97 drop, 3 = 30 fps), checked against `--fps`;
- **piece continuity** — dropped, repeated and mixed cycles counted (a full time needs pieces 0..7 in order within one cycle; the time they spell is the time at piece 0, so it is reported +2 frames);
- **quarter-frame interval mean** — against the host clock, in ppm;
- **the straight-line fit** of the decoded time against the arrival — drift in ppm, residual peak to peak and RMS;
- **the full frames** — value, when, and the delta against `--expect` (to the frame);
- **the notes against the time code** — each step's note should arrive at its song time + `--expect` (the plan of `--song`): mean, spread, worst.

The exit code is 0 when every check passes (the limits are `MTC_LIMITS` in the tool: interval within 100 ppm, residual under 2 ms peak to peak, full frames equal to `--expect` to the frame, no dropped / repeated / mixed piece, notes within 1 ms mean / 1.5 ms spread / 4 ms worst). The test song is proven by `tests/mtc_tempo_song_test.py` (ctest `mtc_tempo_song_python`): it builds, loads back, resaves byte for byte, and carries the naming note on every step plus exactly the four timing FX of the spec.

## `mtc-send`: the host as the MTC master (B1)

`midi_clock_tool.py mtc-send` plays the master for a unit that follows MIDI Time Code (MTC IN, slice B3): it sends quarter frames and full frames to the unit's USB MIDI node at absolute deadlines (the loop of `sync`: sleeps to 1 ms before the deadline, then spins; SCHED_FIFO when the OS allows it: `--rtprio N`, default 20, capped at the user's RLIMIT_RTPRIO, no root needed; the report line says which scheduler it ran under), listens on the same node for the notes of the test song, and says how the unit followed. No firmware is involved: it exercises the unit, it does not change it.

```
DEV=/dev/snd/midiC2D0                       # the unit's node, from `midi_clock_tool.py list`
T="python3 -I platform/mk66/tools/hil/midi_clock_tool.py"
$T mtc-send $DEV --profile "at:00:59:58:00,run:30" --fps 25 --json out/mtc_send.json     # a two-second pre-roll, then the song for 28 s
$T mtc-send $DEV --profile "at:01:00:00:00,run:20,quiet:0.1,run:10,quiet:1,run:10" --jitter 2 --drop 1/8
$T mtc-send $DEV --profile "at:00:59:59:00,run:5,jump:01:00:20:00,run:5" --song mtc-tempo
```

On the unit: the test song on the card (`o3pj_tool gen midisync-song ...` or `gen mtc-tempo ...`, as in MIDISYNC.md and above), **MTC IN** on (the setup `cfg_mtc_in.json` sets USB and MTC START 01, and puts MTC IN back to Off at its end), the project open on the Song screen. The unit's Play, Stop and slot jumps do nothing while it follows (Q8). `--song` names the song playing, so the tool knows the expected arrival of every step's note; **MTC START** (the master's time at which the song starts) is `--start`, default 01:00:00:00.

**Profile** (comma-separated tokens, in order; a profile that does not begin with `at:` begins with `at:<--start>`):

| token | meaning |
|---|---|
| `at:HH:MM:SS:FF` | a full frame; the master's clock is placed there. The first quarter frame follows on the quarter-frame grid at least 4 ms later (spec A-2), at whatever piece comes |
| `run:SECONDS` | quarter frames, forward, at the master's rate |
| `quiet:SECONDS` | the clock runs, nothing is sent. Shorter than 3 frames: a drop-out the unit must freewheel over. Longer: the master has stopped |
| `jump:HH:MM:SS:FF` | a full frame while running; the clock goes there and the quarter frames go on from it |
| `reverse:SECONDS` | quarter frames run backward (pieces 7..0, the time falling); reported, not judged (the spec does not follow a reverse) |

After the last token the master stops and the tool listens `--tail` seconds (default 3): that is the stop test.

Options: `--fps 24|25|29.97|30` (the rate code and the labels; 29.97 is drop-frame, real rate 30000/1001, frames 00 and 01 of each minute but every tenth do not exist), `--rate R` (the master's clock against the host's, 0.5 to 1.5: `1.001` is a master 0.1 % fast, the unit must follow it), `--jitter MS` (uniform +-MS on every quarter frame's send time), `--drop K/N` (K of every N quarter frames are not sent, never in the first 16), `--seed`, `--start`, `--song midisync-song|mtc-tempo|none` (`none` only sends), `--channel`, `--tail`, `--in DEV` (read the notes from another node), `--limit key=value`, `--json FILE`.

The frame maths: quarter frame number q counts from 00:00:00:00, four to a frame; its piece is q mod 8 and the eight pieces of a cycle spell the frame piece 0 stands at, always an even frame (`2 x (q // 8)`); a receiver adds two frames when piece 7 arrives. The master's clock runs `--rate` times the host's: quarter frame q is due when the clock reaches q / 4 frames. A `quiet` loses the pieces of its length and no time.

What the report says (the summary on stdout, the full JSON with `--json`):

- **lock** (a start, and a resume after a stop) — from the first quarter frame to the first note, with the pre-roll before the song start left out (the clock reaching the start counts from there); the raw first quarter frame to first note is given too;
- **relocation** (a jump) — from the jump's full frame to the first note of the new position;
- **phase** — each step's note against the host time at which the master's clock passes MTC START + the step's song time (the walk of `seq/src/song_time.cpp`, as `mtc --song`): mean, spread, worst, and the drift over the run (slope times span);
- **missing** steps, after the lock of their region, and **extra** notes (matching no step and no lock, relocation or stop);
- **stop latency** — for each stop (a long `quiet`, the tail): the last note after the master's last quarter frame;
- the master's own lateness (mean, p99, max of the send against the deadline).

The exit code is 0 when every check passes. The limits are `MTC_SEND_LIMITS` in the tool, from the B3 acceptance: lock at most 250 ms, relocation at most 300 ms, phase mean within 1 ms, spread at most 3 ms, worst at most 10 ms (the last is not in the spec), drift at most 1 ms (or 3 spreads times the square root of 12/n when that is larger), no missing step, no extra note, stop within 4 frames and 5 ms. They are starting values, to confirm in the B3 HIL runs.

Proof: `midi_clock_tool.py --self-test` (ctest `midi_clock_tool_selftest`) runs the master against a synthetic chaser (a unit model that decodes the bytes, fits the master's rate and plays the song on it), with its delay, noise, rate, jump, drop-out and stop known, against faulty chasers (late, noisy, slow to relocate, deaf to a drop-out, slow to stop), with the master's jitter and dropped pieces, and live over FIFOs; `tests/mtc_send_test.py` (ctest `mtc_send_python`) checks the frame maths against fixed vectors and an independent drop-frame count.
