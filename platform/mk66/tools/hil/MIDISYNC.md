# MIDISYNC.md — the MIDI clock bench (OS-MIDISYNC, part 1: the host tool)

`midi_clock_tool.py sync` makes **this computer the MIDI clock master over the unit's USB MIDI port** and measures how solidly the unit follows it. The master sends the clock at absolute deadlines (`time.monotonic_ns`; it sleeps to 1 ms before each deadline and then spins, with SCHED_FIFO when the OS allows it: `--rtprio N`, default 20, capped at the user's `RLIMIT_RTPRIO`, no root needed and no limit changed; the report line says `SCHED_FIFO 20` or `SCHED_OTHER (no rtprio)`), plus Start / Continue / Stop / Song Position Pointer, and in the same loop it timestamps every note the unit plays back, so each note is measured against the clock grid that was actually sent. The unit's USB MIDI node appears as a raw ALSA node (`/dev/snd/midiC<card>D<device>`); `midi_clock_tool.py list` shows the nodes and what `amidi -l` calls them — use the node of the unit (the one that appears when it is plugged in), readable and writable by you (group audio). A USB MIDI packet is 1 ms, so single events are only known to about ±1–2 ms; the means over a run are much better. DIN is not covered: this host has no MIDI interface.

What the bench measures, all of it in the report (the summary on stdout, the full JSON with `--json FILE`, the per-step error series and the raw events with `--raw`):

- **lock time** — the first run of 8 consecutive steps whose error is within the band (4 ms) of the offset: `lock_steps` / `lock_ms`, and the Start → first note latency (`start_to_first_note_ms`);
- **phase offset** — the note-to-clock error of every step after lock: `phase_mean_ms`, `phase_std_ms` (the spread), `phase_worst_ms` (the worst deviation from the mean), with the min/max range;
- **tempo error / drift** — the slope of the error over the run: `drift_ms` over `span_s` and `drift_ms_per_min`; in ppm that is `drift_ms_per_min / 0.06` (1 ppm accumulates 0.06 ms per minute);
- **exact steps per clock** — a step is 6 clocks (24 ppqn, 16th-note steps): `missing` steps that never sounded and `extra` notes that match no step, both must be 0;
- **start / stop / locate latency** — `first_step_heard` against the expected step after Start / SPP + Continue, `start_to_first_note_ms`, and for Stop: note-ons after the byte, time to silence, hung notes;
- **settling after a disturbance** — a tempo jump, a ramp or a burst of dropped clocks: `settle_ms` until the error stays in the band, `max_dev_ms`, the ramp's `lag_ms`;
- **a quiet master** — how long the unit plays on past the stale horizon (48 clock periods, `seq/midi_link.h kStaleClocks`) and whether it closes its notes;
- the **master's own quality** — clocks planned/sent/not sent, the send lateness (mean/p99/max and how many clocks were more than 1 ms late) and the scheduler the master ran under (`master.sched` in the JSON), so a failure of the unit is never confused with a late master.

The exit code is 0 when every check passes, 1 when the report is FAIL. `--self-test` runs the whole thing against a synthetic unit over FIFOs — no MIDI device, no root, no SCHED_FIFO needed — and asserts the measured numbers (a known 3 ms delay comes back as the offset, 2 ms of noise as the phase std, 0.4 ms/s of drift as the drift, a skipped step as `missing 1`, a duplicated one as `extra 1`, a stale-clock follow as a late note-on), and that each of those faults makes the report FAIL. In CI: `ctest midi_clock_tool_selftest` (TIMEOUT 120, runs in under 15 s here).

## The test songs

The unit must send a note for every step it plays, and the note names the step, so the tool knows which step it heard whatever happened to the clock: **wire MIDI note 36 + 16 × (pattern − 1) + step** on the Sync track (a MIDI instrument, channel 1, out on USB). Three songs, all made with `o3pj_tool` (the host build's `tests/tools/o3pj_tool.cpp`):

```
mkdir -p /tmp/midisync/MidiSync /tmp/midisync/MidiSyncSong /tmp/midisync/MidiSyncLoad
<build>/o3pj_tool gen midisync       /tmp/midisync/MidiSync/project.o3pj
<build>/o3pj_tool gen midisync-song   /tmp/midisync/MidiSyncSong/project.o3pj
<build>/o3pj_tool gen midisync-load   /tmp/midisync/MidiSyncLoad/project.o3pj   # writes samples/ beside it (the Dense8 kit)
cp -r /tmp/midisync/MidiSync /tmp/midisync/MidiSyncSong /tmp/midisync/MidiSyncLoad /media/<card>/Projects/
```

- `midisync` — one 16-step pattern, one Sync track. Open it on the **Pattern** screen; the bench runs with the default `--cycle 16`.
- `midisync-song` — three 16-step patterns in Song order (notes 36..83). Open it on the **Song** screen; run with `--cycle 48` (pattern 2 starts at note 52).
- `midisync-load` — the Dense8 stress song (`gen dense8`: 4 Modern + 2 Paula voices, TB-303, FM, send FX and limiter, about 86 % of the block on the unit) with the Sync track as its ninth track. For the runs with the load playing; `--cycle 16`.

On the unit: Config → F1 (the MIDI page) → **CLOCK IN: USB** (turn the encoder; the `cfg_clock_slave_usb.json` scenario does the same and writes `Settings/device.o3st`), **TRANSPORT IN: On** (the default, so Start/Stop/Continue/SPP are followed; the clock is followed either way). Then open the project from File (RESCAN, down to the folder, F1). `tests/midisync_songs_test.py` (ctest `midisync_songs_python`) proves the songs build, load back and resave byte for byte, and that every step carries its naming note.

## The commands

```
DEV=/dev/snd/midiC2D0                       # from `midi_clock_tool.py list`
T="python3 -I platform/mk66/tools/hil/midi_clock_tool.py"
$T --self-test                              # no device: the synthetic unit (ctest midi_clock_tool_selftest)
$T sync $DEV --profile "120:30" --json out/120.json
```

Profile tokens, in order: `BPM:seconds` (a steady segment; a second one at another tempo is a jump), `ramp:B0-B1:seconds`, `quiet:seconds` (no clock, no Stop; the ideal grid goes on), `start`, `cont`, `stop`, `spp:N` (Song Position Pointer, N sixteenths; put it before `cont`). Without a start/cont token `start` is put first; without a final stop/quiet token `stop` is put last. Master faults: `--jitter MS` (uniform ±MS on every clock's send time, seeded by `--seed`), `--drop K/N` (K of every N clocks not sent, never in the first 48), `--drop-burst AT:LEN`. Options: `--base 36` (the note of step 0), `--cycle 16|48`, `--channel 1`, `--limit key=value` (override one threshold), `--json FILE`, `--raw`, `--tail 3`, `--no-auto-transport`.

## Reading the report

The summary prints the master line (clocks planned/sent/not sent, jitter, lateness, SCHED_FIFO), then per run: steps expected / missing / extra, the first step heard, Start→first note, lock (steps and ms), the offset, and the after-lock line (steps, span, phase mean / std / worst with the range, drift over the run and per minute). Then the disturbances (jump / ramp / burst: settle, max deviation, ramp lag), the stops (note-ons after the byte, silence, hung notes), the quiet master (notes after the last clock, the last note-on against the stale horizon, closed). Every line that is a check is also in `checks` in the JSON with its `value`, `limit` and `ok`; the last line is `PASS (N checks, 0 failed)` or `FAIL` plus one `FAIL <scope>: <name> = <value> (limit <limit>)` line per failed check. `--raw` adds the per-step error series (`raw.error_series_ms`) to draw the follow over time.

The thresholds (`LIMITS` in the tool, each overridable with `--limit`), all **starting proposals of OS-MIDISYNC M1 to confirm on the unit**: steady phase std < 1.5 ms, worst < 4.0 ms, |drift over a run| < 0.5 ms (the effective limit is `max(0.5, 3 * phase std * sqrt(12 / steps after lock))`, so a short noisy run is not failed by chance; the limit used is reported per run), |absolute offset| < half a step at the run's tempo (`offset_ms`, 0 = automatic; a unit a whole step late must fail), missing = extra = 0, lock ≤ 12 steps, ≥ 16 steps after lock, the band 4 ms, jump settle ≤ 3000 ms, burst settle ≤ 2000 ms, ramp worst < 8 ms, a quiet master's last note-on ≤ 100 ms past the stale horizon, Stop: no note-on > 5 ms after the byte and silence within 50 ms, the offsets of several runs within 4 ms of each other. A bare Continue (no SPP before it) must resume on the step after the previous run's last step, and a run cut by a restart sounds its last steps too. Two known limits of the tool itself: a previous run's note within ~2 ms before a Start/Continue byte can be taken as that run's first step, and on a real rawmidi node a blocking write delays the note reads (the summary reports the per-run write time when a write stalls).

## The battery (part 2 of the row — run on the unit, by the tester)

Setup: the card with the three songs in `Projects/`, CLOCK IN USB, TRANSPORT IN On, the project open (Pattern screen for `midisync` and `midisync-load`, Song screen for `midisync-song`). Every command writes its JSON (`--json out/NN.json`, kept as the record) and must exit 0 (PASS). The steady limits of every steady run are the ones above: missing 0, extra 0, lock ≤ 12 steps, phase std < 1.5 ms, worst < 4.0 ms, |drift| < 0.5 ms. Each limit is a starting proposal to confirm on the unit.

1. Steady 60 BPM: `$T sync $DEV --profile "60:60" --json out/01.json`
2. Steady 90 BPM: `--profile "90:60" --json out/02.json`
3. Steady 120 BPM: `--profile "120:60" --json out/03.json`
4. Steady 180 BPM: `--profile "180:60" --json out/04.json`
5. Steady 240 BPM: `--profile "240:60" --json out/05.json`
6. Steady 300 BPM: `--profile "300:60" --json out/06.json`
7. Tempo jumps: `--profile "120:30,180:30,90:30" --json out/07.json` — both jumps settle ≤ 3000 ms (window complete), steady limits on each segment after its settle window.
8. A ramp: `--profile "120:10,ramp:120-240:30,240:10" --json out/08.json` — ramp worst |error − offset| < 8 ms across the ramp, steady limits on the 240 segment after it.
9. Start / Stop / Continue: `--profile "start,120:10,stop,120:2,cont,120:20" --json out/09.json` — on the Stop: no note-on more than 5 ms after the byte, silence within 50 ms, hung notes 0; the Continue run plays on and the offsets of the runs agree within 4 ms; steady limits on the last run.
10. SPP locate in Pattern (`midisync`, Pattern screen): `--profile "start,120:10,spp:8,cont,120:10" --json out/10.json` — the Continue run's first step must be heard as 8 (`first step` check), then steady limits.
11. SPP locate in Song (`midisync-song`, Song screen): `--profile "start,120:10,spp:20,cont,120:10" --cycle 48 --json out/11.json` — the Continue run's first step must be heard as 20.
12. Jitter ±2 ms: `--profile "120:30" --jitter 2 --seed 1 --json out/12.json` (repeat with `--seed 2`) — the steady limits: the unit must absorb the master's jitter (the master's own lateness is printed on the first line; a FAIL with a bad master lateness is the host's fault, not the unit's).
13. Dropped clocks: `--profile "120:30" --drop 1/24 --json out/13.json` — missing 0 and the steady limits; and a burst: `--profile "120:30" --drop-burst 300:24 --json out/13b.json` — burst settle ≤ 2000 ms, missing 0.
14. A quiet master: `--profile "120:20,quiet:10" --json out/14.json` — the last note-on at most 100 ms past 48 clock periods after the last clock, and the notes closed after it.
15. The 30-minute soak: `--profile "120:1800" --json out/15.json` — missing 0, extra 0, phase std < 1.5 ms, |drift over the 30 min| < 0.5 ms (the ppm figure is `drift_ms_per_min / 0.06`).
16. Load, steady: `midisync-load` open, `--profile "120:60" --json out/16.json` — the steady limits, and on the unit's console `diag cpu reset` before and `diag cpu` after: `underrun 0` for the whole run.
17. Load, jitter: `--profile "120:60" --jitter 2 --seed 1 --json out/17.json` — the steady limits and `underrun 0` in `diag cpu` as in 16.

`measure` and `send` (the subcommands MANUAL.md already uses) are unchanged; `send --profile 120:10 --start --stop` is the quick manual check, `sync` is the bench.
