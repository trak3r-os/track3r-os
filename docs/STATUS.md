# Status

Handoff state for a session that starts cold. Keep this file short; the history lives in git, the plan in `docs/roadmap/BACKLOG.md`, the user-visible changes in `docs/CHANGELOG.md` and the release notes in `docs/release/`.

Every behaviour carries one of three labels: **verified parity** (compared with the official firmware, with evidence), **new Community OS behaviour**, or **open research question**. "Verified on the unit" means a person or the HIL tools ran it on real hardware; a host or simulator test is not that.

## Where things stand

**v1.0.1 is published and fixes the pads on later Trackers (issue #1, now closed). v1.0.0 (2026-10-25) was the first release.** Merged, checks on the unit pending: OS-CFG-ARROWS, OS-BUG-FWLIST, OS-USBOUTS (USB outputs of 2, 4, 6, 8 or 10 channels per project) OS-PERF-CPU2 (cheaper reversed voices; the listening is left) and OS-PERF-LFO-STEP (step-effect LFOs 23, 24, 33 and 36). Fixed on dev, in the next release: OS-BUG-MODDRIVE (issue #2), and OS-BUG-COSMBOOT (a development image crashed at boot because the card update left a short final flash sector unwritten; images are now padded to a whole 4 KiB sector and `check_image` has a 9th check, `sector_end`; verified on the unit). Perform PLAYBACK and the five Perform LFO rows are in. Downloads on the Releases page: `v1.0.1` and `v1.0.0` (each the firmware file and `SHA256SUMS`), `apps-v1.0.0` (the app modules) and `fixtures-1` (test data for `tools/testdata`). What is open is checks on the unit (below) and the rows in progress or queued.

| Item | State |
|---|---|
| Version | v1.0.1 (2026-10-27; Config, INFO shows the BUILD ID). v1.0.0 (2026-10-25) was the first release; the CI and size rows below are v1.0.0's |
| Target | Polyend Tracker hardware (NXP MK66FX1M0, 1 MiB flash, 256 KiB SRAM, 16 MiB SDRAM, FT812 display, SGTL5000 codec) |
| Licence | GPL-3.0-or-later; the module SDK under `module/sdk/` is MIT |
| Final CI (host, e2e, QEMU, MK66) | host 305/305, e2e 88/88, QEMU 61/61, MK66 19/19, on the code of the release |
| Release image | v1.0.1: `track3r-os-v1.0.1.ptf`, 2,618,578 B; SHA-256 `bc1cf7b8ca02d5b5f9f8e4abffe2d5b9f6943fe6a3ba1ad94bd26a5ff775e2de`. v1.0.0: `track3r-os-v1.0.0.ptf`, 2,610,089 B; SHA-256 `379bad017e1829c23040b2387e5e100cf4324c7ce6bcf9126161f90876336986`; CRC-32 `A383D7D0`. Flash image 927,932 B (FLASH 927,196 B); SRAM_L 49,608 B, SRAM_U 135,392 B, SDRAM static 154,080 B |
| Image gate (`fw-gate-check`) | PASS: built twice from scratch, byte-identical; `check_image` 8/8; flash configuration field exact; 0 warnings or undefined references. Nothing was flashed by the gate |
| Pass on the unit | A full pass on the final release candidate, whose code is identical to the release (only the build id and version strings differ; every library member is byte-identical). No firmware FAIL that blocks the release; two CPU figures are over their numbers (Known issues). See "Verified on the unit" and "Open on the unit" |

### Perform PLAYBACK and the Perform LFO rows (shipped in v1.0.0)

- **PLAYBACK (reverse), new Community OS behaviour.** Hold the pad and the next notes of the affected tracks play backward, from the end of the window to its start, and stop there. Sampler one-shots and beat slices only; looped, granular, wavetable, Amiga, module and MIDI instruments ignore it. The step effect Reverse (26) does the same. Cost in code: FLASH +260 B, `.ramfunc_l` +52 B. Cost in CPU while held: about 0.89 % of the audio budget per sounding reversed voice (about 2,300 cycles a block); six reversed voices cost +5.34 pp, over the 3 pp gate set for it (accepted as a known issue in v1.0.0; OS-PERF-CPU2 later measured +0.23 pp on the unit, against +6.19 pp before). Verified on the unit: one-shot, window and beat slice play backward, effect 26 on one step, an export with it held is byte-identical to one without.
- **The five LFO rows (VOL, PAN, TUNE, FLT, POS LFO), new Community OS behaviour.** The value is a signed offset on the speed of the track's LFO for that target. Where the slot is Off or an envelope, an LFO runs with the shape and amount stored on the instrument (a triangle at 50 % when that amount is under 5 %). Cost in code: FLASH +1,244 B, SRAM_U +512 B (a 512-byte alignment step of the USB buffer table; 184 B of real growth). Cost in CPU while held: the five rows together with a held LOWPASS cost +18.11 pp on twelve tracks of a light scene (fqa.15; the per-row split of that run is void). That is above the 6 to 7 % estimate (accepted as a known issue in v1.0.0; on v1.0.1-cpu2.1 the same arm measured +28.39 pp, against +29.09 pp before OS-PERF-CPU2). Verified on the unit: each row changes the sound as specified on an instrument with no LFO; the export compare is byte-identical.
- **Perform SMP POS and SMP END** (in the same release): FLASH +240 B, SRAM_U +32 B, `.ramfunc_l` +48 B. CPU with both held on twelve tracks: +0.39 and +0.48 pp, inside the noise.

## What the OS does

A complete operating system for the Tracker, written from scratch in C++17. Everything below is **new Community OS behaviour** unless a manual chapter or a catalogue row says "verified parity" and gives the evidence. The manual starts at `docs/manual/index.md`; every function has a row in `docs/qa/catalogue/`.

- **Sequencer and Pattern.** Step and live entry, WATCH mode, count-in, Fill (Euclidean and rule-based), block and column selection, a long list of step effects with a plain-words readout, MOD and S3M effects, undo for pattern edits.
- **Song, Mixer, Perform, DJ.** Order list with edits while the song plays; track mixer with mute, solo and an always-on master limiter; Perform slots (scenes, filters, stutter, bit depth, overdrive, tune, per-track next pattern, SMP POS and SMP END); a DJ page (scratch platter, tape stop, crossfader pads).
- **Instruments and sound.** Sampler, wavetable and granular play modes, envelopes, LFOs, filter, MIDI instruments, an Amiga-style (Paula) sampler engine, and sound modules (TB-303, Chip, FM with sixteen algorithms) loaded from the card as `.tmod` files. The audio clock is exactly 44,100 Hz.
- **Samples.** Load WAV, AIFF and 8SVX (several files at once); record with START ON LEVEL; loop record and overdub; edit (normalise, DC removal, convert, FILTER and COMPRESSOR with cancel and undo); wavetable and chop views.
- **Files.** Native project format (`.o3pj`); original-firmware (OG) project and instrument files read and written in their own format (OG export is marked experimental); MOD, S3M and OctaMED import; WAV export of song, pattern, block and stems; MIDI file export; the card as a USB disk; screenshots; help pages; update from the unit (Config, INFO, `F8`).
- **MIDI and USB.** DIN and USB MIDI in and out, clock and transport in and out, USB audio, a serial diagnostic console (`diag`) for computers.
- **Modules and apps.** The `.tmod` format (instrument, effect, importer, app) with an SDK under `module/sdk/`; twelve bundled apps.
- **Tooling.** A headless simulator (`sim/`) with scenarios and screenshots, a QEMU target, hardware-in-the-loop tools (`platform/mk66/tools/`: `o3hil`, `o3console`), and a QA catalogue that feeds the tests and the manual.

## Verified on the unit

Run on real hardware by a person or `o3hil` (names are QA catalogue rows or backlog IDs):

- **CPU.** The Dense8 test project (8 dense tracks) runs at a mean of 86.6 % of the audio budget on the final release candidate, worst block 107.9 %, 0 underruns, 0 shed (86.7 % and 86.8 % on the two candidates before it). Dense8 is the CPU reference and is re-measured on every image (`docs/architecture/cpu-budget.md`). Per voice, about 14.8 % of the budget for the TB-303, 10.3 % for FM and 2.3 % for Chip, unchanged on the final release candidate. The scratch platter with the hand still costs +0.3 pp over off.
- **Perform.** SMP POS and SMP END (note start and stop measured on a one-shot and a looped sample), PLAYBACK and effect 26, and the five LFO rows work as specified; three export compares (SMP, PLAYBACK and the LFO rows held, each against Perform off) are byte-identical. Costs and limits: "Perform PLAYBACK and the Perform LFO rows" above and Known issues.
- **Regression smoke on the final release candidate.** Cancelling a running 45 s compressor (the sample stays as it was), Record F6 shows `-`, project save and reload, New project ALL, and Dense8 played through once.
- **Sample load and edit.** The multi-load summary (OS-AUDIT-A7-SUMMARY); the FILTER and COMPRESSOR core path and the cancel of a running job (OS-AUDIT-A9, OS-SEDIT-CANCEL); a replaced sample gives its memory back (OS-ARENA-RECLAIM); the seam at the loop wrap is gone for a range that starts at the loop start (OS-A9-SEAM, on an earlier candidate).
- **Storage and power.** Running USB disk mode (Linux only); the low-power off and wake (OS-POWER).
- **Not reproduced.** OS-BUG-NEWLIST (Sample load empty after File NEW) did not reproduce on the unit.
- **Update from the unit.** Config, INFO, `F8` lists the files, asks, checks the release candidate (`IMAGE OK`, size and CRC match the file) and offers INSTALL. See Open on the unit for the last step.

## Open on the unit

Not yet verified on a unit (host and simulator tests exist for most). The rows marked UNIT PENDING in `docs/roadmap/BACKLOG.md` name each check.

- Config and Record arrows (OS-CFG-ARROWS, tester image `v1.0.1-arrows.1`): install, smoke and the changed HIL steps passed. By hand: the feel and auto-repeat of the arrows on Config and Record, Shift + arrow (coarse), the USB pending value with F7, and the READ ONLY row.
- USB outputs of 2, 4, 6, 8 or 10 channels per project (OS-USBOUTS, Mixer › STEMS): checked on Linux (2026-10-30). Left: macOS and Windows enumeration (reports welcome).
- Module instrument drive (OS-BUG-MODDRIVE, issue #2): fixed on dev (2026-10-30), in the next release. The cost with the shaping off passed (Dense8 +0.3 % against v1.0.1-usbouts.1). Left: listening for QA-MOD-207 and QA-MOD-208.
- Reversed voices and the held PAN LFO (OS-PERF-CPU2, QA-PRF-170 and 173): listening by ear.
- Feel of the DJ scratch and of encoder acceleration; scratch still-to-moving sound (OS-SCRATCH, OS-SCRATCH-IDLE, OS-SCRATCH-IDLE2, QA-PRF-150). The Instrument arrow keys (OS-INSTR-ARROWS) and the LFO picture (OS-LFO-GRAPH) are by hand.
- HIGH PASS and BAND PASS were checked on earlier candidates; the compressor variations (RATIO, ATTACK, RELEASE, MAKEUP, DETECTOR) and the "back to size before the load" half of QA-SLD-035 stay open.
- Perform: a multi-slice instrument with SMP POS and SMP END, the +100 and -100 values, live pad notes with PLAYBACK from Master or Song, Retrigger (55) on a backward note, an instrument with its own LFO under the LFO rows, POS LFO on wavetable and granular instruments, step effect 35 with a held FLT LFO (QA-PRF-151, 153, 160). QA-INS-219.
- Eight TB-303 voices at once and a polyphonic chord (CPU not measured).
- FILL on a column block; DIN out and Song Position (E-CFG-03); line-in and PRE-ROLL (OS-AUDIT-72); Random and long-track landing of the per-track pattern switch (E-PERF-07); LIST STILL READING.
- **The INSTALL step of Update from the unit.** The test console refuses key presses while an update is armed, so a person has to press `F1` within 30 s; the final release candidate was put on the unit by the console path instead. Also: install from the official firmware, the Windows and macOS disk copy, going back to the official firmware.
- Card timing was measured with one SD card brand only.

## Known issues

- **Swapping the SD card can crash the unit (OS-BUG-HOTSWAP).** Seen once: with the Dense8 project loaded (8 instruments, samples in memory), after about 90 minutes of uptime, a card swap raised a memory-protection fault in the render interrupt and the unit restarted. A later swap with an empty project worked. The cause is unknown (open research question). Workaround: switch the unit off before changing the card.
- **Very heavy projects can exceed the CPU budget.** The Heavy12 stress project (12 tracks of sampler, Amiga, TB-303 and FM voices with effects) runs at 113 % mean and a worst block of 147 % on the unit (an earlier test image); sound can drop out while it plays, while keys and Stop still answer. Dense8 is the reference; Heavy12 is a stress test only. The release ships with the limit documented, not with features cut.
- **Some Perform effects cost CPU while they are held.** The reversed-voice cost of PLAYBACK was cut by OS-PERF-CPU2 (+0.23 pp on the unit, against +6.19 pp before; gate 3 pp). The five LFO rows with LOWPASS held cost +28.39 pp on twelve tracks (`v1.0.1-cpu2.1`), PAN LFO alone +4.8 pp. On a project already near the budget, holding them on many tracks can make the sound drop out; release the pad. Accepted for 1.0; a follow-up is proposed (OS-PERF-CPU3, IDEA).
- **The pads did not answer on one user's unit with v1.0.0 (OS-BUG-PADS, fixed in v1.0.1).** Plastic-back units (later board revisions; issue #1). The pad-bus bring-up for later boards is in v1.0.1; the user confirmed pads and note entry work (2026-10-26). Only that one unit has been reported; other later revisions are not checked.
- **The update list shows only the first 12 files of `Firmware/` (OS-BUG-FWLIST).** Later files are not listed and cannot be chosen. Keep 12 or fewer `.ptf` files there. Fixed on `dev` (2026-10-30), in the next release.
- **A cancelled effect leaves the free sample memory lower.** After cancelling a long compressor the FREE figure stays about 3.9 MB lower until the next effect reuses the buffer (documented in the Sample edit chapter of the manual).
- **SMF EXPORT** (MIDI file export) is host-tested only.
- **A looped sample processed whole can keep a seam.** A FILTER or COMPRESSOR edit over a sustain loop in the middle of a sample keeps a seam at the loop wrap (OS-A9-SEAM limit).
- **OG export.** The screen can pause for up to about 330 ms at a time (slow card writes and directory operations), and the progress box is see-through. The export keeps its EXPERIMENTAL label because it was not checked on an official unit.
- **USB disk.** Tested on Linux only. Writes go one block at a time: a large copy ran at about 250 KB/s while the unit played.
- **Module apps.** The tuner reads NO SIGNAL on a sustained clean tone from the master; the scope's SPECTRUM view can be stopped ("APP STOPPED: too slow") when it starts under load, until the unit reboots; RHYTHM GEN opened from MODULES writes nothing (open it from TOOLS on the Pattern screen).
- **Power button.** A click may be heard at power off and at power on (accepted gap).
- **The DJ fine tempo is not an undo unit** (by design; QA-PRF-122).
- **Compatibility.** Whether the official firmware opens projects saved by this OS, and what the unit's stored settings do when you switch firmware, are open research questions. The USB identity is the official Tracker pair 16D0:0E9F (a deliberate decision; the accepted risks are in the OS-USBDISK spec).

## Next

Ordered for a fresh session:

1. A person presses INSTALL in Update from the unit on the release file, and runs the checks under "Open on the unit"; record each result as PASS, FAIL or NOT RUN in the backlog rows.
2. After 1.0: OS-BUG-HOTSWAP (the card-swap crash), then OS-BUG-FWLIST (merged, unit check pending), then OS-BUG-MODDRIVE (fixed on dev, unit check pending), then OS-PERF-CPU2 (merged; listening left) and OS-PERF-LFO-STEP (merged; unit checks left), then OS-PERF-CPU3 (in progress, approved 2026-10-30), then OS-STEREO (queued), then the pending rows in `docs/roadmap/BACKLOG.md` and the latent issues below.

Outside contributors: issues come in through the forms in `.github/ISSUE_TEMPLATE/`; pull requests target `dev` (see `CONTRIBUTING.md`).

## Latent issues

Recorded once; no backlog row yet.

- `Engine::offline_leave` hands the export's Amiga state to the live render; the full parameter ring drops the clear in `seq/src/offline.cpp`; there is no sample rebind at export start.
- The Mixer ROUTING SND A/B column gates nothing a user sets.
- `SampleBank::referenced()` ignores the undo and redo step buffers (paste, edit, load order).
- A reclaim leaves intermediate holes until reload; a replace kept the old buffer once (QA-SLD-028).
- Pattern: a PingPong long track can land up to one pattern early (E-PERF-07); line-in PRE-ROLL has an odd start (OS-AUDIT-72).
- File screen: `file_screen` room=1 clip; `pad.audition` kFocus overlay on Sample load.
- The e2e fix is keyed on `Button::Play`, and no test covers a user chord.
- A9: a sustain loop processed whole keeps a seam; a fix needs a second pass over the loop head.
- The simulator's audition and peak carve-out formulas are copied from `platform/mk66/src/app_main.cpp`; move them to a shared header. `draw_lfo` has no host test, only the simulator sweep.
- `q15_half_slow` in `audio/src/master_scratch.cpp`: a NaN input is an out-of-range float-to-int cast (the cast predates OS-SCRATCH-IDLE2).
- FM algorithms 3, 9, 10, 11, 12 and 14 show a small DC offset (about 1.5 % of peak) in a unit capture; a host-oracle DC check would catch it.
- OS-PERF-SMP: moving the `note_on` tail to flash would win back 48 B of fast code; a live loop edit can move `loop_end` below a voice's SMP POS start, and a ping-pong or backward voice then relies on the render loop's turn handling (not traced).
- A DJ tempo change records a project undo unit that the DJ screen cannot reach.
- Tests: `project_python` was flaky once; mutant M22 has not been run; QANET M20 and M21 have gaps; `remote_ci` has no mutant mode; the QANET rule 1 blind spot (a WAV load credited over the history slot at partial depth) is open (`docs/qa/qanet-findings.md`); the voice-pool and 2000-file-folder fixtures are not done.
- Manual chapters over their word caps (Instrument, Pattern, Record, Wave, Perform, Config, Export, File, Sample load): split after 1.0. `docs/manual/fx.md` has five research TODOs.

## Decisions in force

- CPU: Dense8 (about 85 % mean, zero underruns) is the reference; Heavy12 is a stress test. Features are not cut for CPU.
- The OG volume law (101-entry table) is our model; the repository holds no capture behind its curve (ADR 0013).
- Undo covers pattern edits, parameter changes and sample edits; destructive actions that ask for confirmation get no undo; the Mixer has no undo.
- Any SD card may be written by the tools (never a fixed disk); firmware reaches the unit only through a documented update path after `fw-gate-check`.
- Defaults chosen while the specs were written are listed in each spec under `docs/specs/` and in the QA catalogue rows; change one by editing the spec and the row together.
