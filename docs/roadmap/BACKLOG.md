# track3r-os backlog

What is not finished. One row per package. Finished work (DONE) is not listed here: see [`docs/CHANGELOG.md`](../CHANGELOG.md) for what changed for users, and the git history for the rest.

**States.** IDEA (a proposal; nobody codes an IDEA), APPROVED (the maintainer wants it, in order), QUEUED, IN PROGRESS, UNIT PENDING (merged; only a check on the real hardware is left), PARKED (on hold, with the reason) and DROPPED (will not be done, with the reason). The maintainer moves rows from IDEA to APPROVED, PARKED or DROPPED.

**Columns.** Parity: OG = aims at compatibility with the official firmware's file formats or documented behaviour (verified only by an observation on an official unit, with evidence); N = new Community OS behaviour; R = research needed. Cost: S, M, L (small, medium, large). Rows keep their `OS-*` IDs; specs, the QA catalogue and the manual cite them.

**Want something here, or something else?** Open an issue with the feature-request form (`.github/ISSUE_TEMPLATE/`). Feature requests become IDEA rows. Before you start on a row, ask in the issue: an APPROVED row can have a spec, a format constraint (`CLAUDE.md`, "Persistent formats") or a CPU budget that changes the plan.

## IN PROGRESS

| ID | What | Parity | Cost | Note |
|---|---|---|---|---|
| OS-STEREO | Stereo samples (opt-in per instrument) | N | L | +20-30 % per stereo voice [est] | Approved by the maintainer 2026-10-30 (ok D), approach D, in stages. The maintainer's answers (all the recommended defaults): opt-in per instrument, MIX stays the default; a cap of 4 stereo voices, beyond which the mid plays; forward one-shot and loop, Modern engine only; USB stems and sends stay mono (the mid), offline stems stay stereo; pan becomes balance with the same law; a new INST field, and `.pti` export downmixes; document the two-track workaround first. Today a stereo file is mixed to mono, or to L or R with F8 (OS-LOADCH); the reader can already keep stereo, but nothing downstream uses it. Design: a planar mid plus side-car in the arena; a flash stereo kernel (SRAM_L about 0); a second strip feed with balance; editor ops on both blocks. Step 1 merged (dev 28b7b355: the manual's two-track workaround, QA-FIL-120..124, and the bench-only `voice_st`). On the unit, `voice_st` (bench image `v1.0.1-stereo1.1-bench`) is 2.15x mono (about +0.8 pp a stereo voice). Step 2 (the stereo instrument, Level C) is specified with its risk review (`docs/specs/OS-STEREO.md`, section "Step 2": defaults S1-S8, design rules 1-11, slices a-e); Step 2 slice (a) merged (dev bf82a880; commit 27e9a6d0, review fold a841128f): INST 1.5, the instrument's stereo flag and the STEREO row. Next: slices (b) side-car, (c) voice and mix, (d) editor ops, (e) `.pti`. The maintainer has not answered S2, S5, S6 and S8; the defaults apply. Open: Dense8 has no CPU headroom (86 % mean); the official firmware's mono import is documented but not verified on a unit. Slice (b) merged 2026-10-30 (dev 70d1225d; review APPROVE after one fold): the side-car `samples/<stem>_s.wav`, the STEREO choice on F8 of the load screen (renamed from CH STEREO), rule 4 reconcile, collisions never silent; flash +3,312 B (over the spec's estimate for the whole step: check slice (c)), sample arena about 0.8 KB less. Latent: the load screen's collision check assumes `samples/instrNN.wav` from `state_.wav.slot` (the save still catches a mismatch); orphan `_s.wav` files are never deleted. Unit checks for (b): save and reopen a stereo instrument with and without its `_s.wav`, the strip texts. Next: slice (c), the voice and the mix (audio path, unit CPU). Slice (b) on the unit (`v1.0.1-stereo.1`, 2026-10-31): QA-FIL-131..135 PASS; QA-FIL-136 (a taken side name) needs a hand-edited project, since saves name samples by slot. |

## UNIT PENDING

Merged and covered by host or simulator tests; the check on the real unit is the only thing left. Each check is a row in `docs/qa/catalogue/`. If you own a unit and want to help, report PASS or FAIL in an issue.

| ID | What | Check left on the unit |
|---|---|---|
| OS-SONG-VIEW2 | STAGE leaves the Song cycle (its countdown moves to TIMELINE's info line); a simpler TIMELINE (neutral blocks, the pattern colour as a thin strip, a thin energy row); a per-pattern note map (an FT812 bitmap, one tick per note) replaces the density thumbnails | N | S+M | 0 (main loop) | IN PROGRESS (`feat/OS-SONG-VIEW2`). Approved by the maintainer 2026-10-30 ("dale"), after the unit check of OS-SONG-VIEW and a Fable design opinion. New Community OS behaviour. Level B, no format change. Slices 1 and 2 (STAGE leaves the cycle, the countdown on TIMELINE's info line, the de-clutter) are in progress; slice 3 (the note map, starts with the RAM_G map) follows. Spec `docs/specs/OS-SONG-VIEW2.md` (V1-V5). Back to QUEUED on 2026-10-30 (one lane; slices 1-2 merged and unit PASS): slice 3, the note map, waits here. Slice 3 (the note map, V4) merged 2026-10-31 (dev 7636b1ea; review APPROVE after one fold): one L1 bitmap per pattern in RAM_G 0x71000..0x89000, at most 8 uploads a frame, DL worst case 891 words. Defaults to confirm: 384 B pictures (a blank row between tracks), tracks past 12 not shown, a mixed frame possible on an edit while the page is open. Unit: QA-SNG-088..096 (the map on the panel in three themes, the first draw of a 40-pattern song, diag shot). |
| OS-BUG-CHIPWAVELABEL | Chip's Wave view draws the wave's name over the trace, as SID's did | N | S | 0 | Found in the OS-BUG-SIDWAVELABEL fix (2026-10-31): the same layout (a 20 px label at y=3, the trace from row 16). Same fix: start the trace below the label (modules/chip; a module change, so qemu). Merged 2026-10-31 (dev, review APPROVE): the trace starts at row 26. Unit: a look at Chip's Wave view (QA-MOD-237). |
| OS-BUG-SIDWAVELABEL | SID's Wave view draws the wave's name over its first edge | N | S | 0 | Found on the unit 2026-10-30 (`v1.0.1-sid.2`, the tester's shots in build-images/v1.0.1-sid.2/shots/). Cosmetic: move the label or the trace (modules/sid, the Wave view). Merged 2026-10-31 (dev 2c329185): the trace starts below the label. Unit: a look at the SID Wave view. |
| OS-BUG-ONCARD | The engine list labels the built-in modules `ON CARD` | N | S | 0 | Found on the unit 2026-10-30 (`v1.0.1-sid.2`). The TB-303, Chip, FM and SID are embedded in flash; the label should say they are built in (find where ENGINE's list builds its tag). Cosmetic. Merged 2026-10-31 (dev 2c329185): embedded modules read BUILT IN, a card copy keeps ON CARD. Unit: QA-MOD-236. |
| OS-BUG-CFGARM | Config's `APPLY?` mark on F7 stays 9 s and more with no key (the row says 2 s, as `RESET?` does) | N | S | 0 | Found on the unit 2026-10-30 (`v1.0.1-sid.2`, QA-CFG-034, a pending USB AUDIO value; a turn clears it). The simulator passes `qa_cfg_003`, so look for what differs on the unit (the arm's timeout, or the strip it follows). Same batch: the stale "nothing in slots 1-4" comments at platform/mk66/src/app_main.cpp:1362 and tests/play_after_load_test.cpp:286 (now 1-48). Merged 2026-10-31 (dev 2c329185): the apply and reset arms are honoured only while their question is on the strip, and a later F7 asks again; the question takes the head at once. Unit: QA-CFG-034. |
| OS-SID | A SID-style instrument as a new `sid.tmod` (Chip unchanged): saw, triangle, pulse with PWM, noise, combined waves, sync and ring from an internal modulator, the stepped ADSR, an LP/BP/HP filter with a 6581 / 8580 model switch; 31 of the 32 parameters the module ABI allows | N | M | est. 4.9 % of a block a voice (3.9 % with the filter off); Chip measured 2.3 to 2.8 % | Queued by the maintainer 2026-10-30 ("añade el modulo sid, ponlo en cola"). Spec `docs/specs/OS-SID.md` (Level B, defaults S1-S8 to confirm: built in like Chip with an 18 KB flash cap, own combined-wave tables with no emulator data, numbers from the C64 Programmer's Reference Guide and the 6581 datasheet). Radar card `docs/vision/radar/sid-instrument.md`. Next in the lane after OS-PERF-HPBP slice 1c; merges after the unit CPU measurement. New Community OS behaviour. Started 2026-10-30 evening: slice 1 (the voice without the filter) on `feat/OS-SID`. Merged 2026-10-30 (dev 2cf9741b; reviews APPROVE after two folds; host 312/312, mk66 20/20, qemu 62/62; flash +14.7 KB, built in; hot part 4,448 B). Unit CPU PASS on `v1.0.1-sid.1`: 4.2-5.1 % a voice with the filter. UNIT PENDING: the ear check (waves, filter curves, resonance; a person), the Instrument pages and views on the panel, module instruments in slots 1-4 with an OS-saved project. Unit on `v1.0.1-sid.2` (2026-10-30): the four pages and three views PASS; Drive bounded (at most +4.8 dB rms); Sweep, vibrato and Arp measured right; the slots 1-4 finding is OS-BUG-DEMOKIT, not the module. Still open: the ear check (a person). |
| OS-SONG-STAGE | Song STAGE page: the playing slot and the next one in large type, a bar countdown to the next slot, a progress bar (live use, low vision) | N | S | Merged 2026-10-30; checks on the unit pending: legibility at a distance, the bar countdown against the audio. New Community OS behaviour; defaults to confirm by the maintainer (`docs/specs/OS-SONG-VIEW.md`). After the unit check the maintainer drops STAGE from the Song cycle (OS-SONG-VIEW2); its countdown moves to TIMELINE. |
| OS-SONG-TIMELINE | Song TIMELINE: slots as blocks scaled to bars, a colour per pattern, per-track note density, an energy curve over the song | N | M | Merged 2026-10-30; checks on the unit pending: how the colours, the density thumbnails and the energy curve read. The display list is bounded by construction. New Community OS behaviour; defaults to confirm by the maintainer (`docs/specs/OS-SONG-VIEW.md`). |
| OS-PERF-LFO-STEP | The step-effect LFO columns 23, 24, 33 and 36 start the matching LFO on a sampler track, as FX 35 does | Passed on the unit (`v1.0.1-lfostep.1`): sound by capture for 23, 24 and 33; FX 24 wins over FX 31 in both orders; step rows cost no more than the layer rows; FX 36 on plain samples +0.14 pp; underrun 0; the export carries the LFO and two exports are byte-identical. Left: FX 36 on wavetable and granular, by ear; QA-PRF-186 and 189; "the next Play ends it"; "layer at 0". The step-against-layer CPU gate held only for VOL (the other layer rows measured were default slots); re-check in OS-PERF-CPU3 step 0. |
| OS-PERF-CPU2 | Cheaper Perform effects, after v1.0.0: reversed voices take a fast path, bit-identical (REV12 +0.23 pp on the unit, against +6.19 pp; gate 3 pp) | Left: by ear |
| OS-BUG-FWLIST | The update dialog (Config, INFO, `F8`) reaches every file of `Firmware/`: it pages through them twelve at a time and shows `FILE n / total` | QA-CFG-265: 19 `.ptf` files on a card, page through them and choose the 18th; how long a page change takes with a large folder |
| OS-CFG-ARROWS | Left and Right edit the value under the cursor on Config and Record (Shift: the coarse step); the pages change only with the F-keys or the screen button | Passed on the unit (tester image `v1.0.1-arrows.1`): install, smoke and the changed HIL steps. Left: the feel and auto-repeat of the arrows on Config and Record, Shift + arrow (coarse), the USB pending value with F7, and the READ ONLY row |
| OS-USBOUTS | USB audio outputs of 2, 4, 6, 8 or 10 channels, chosen per project on Mixer › STEMS (row OUTPUTS); the USB return only below 10 channels | Left: macOS and Windows enumeration (reports welcome) |
| OS-BUG-MODDRIVE | Overdrive and Bit depth shape module instruments (TB-303, FM, Chip); issue [#2](https://github.com/trak3r-os/track3r-os/issues/2) | Fixed on dev (2026-10-30), in the next release. The cost with the shaping off passed (Dense8 +0.3 % against v1.0.1-usbouts.1). Left: listening for QA-MOD-207 and QA-MOD-208 |
| OS-PERF-SMP | Perform SMP POS and SMP END act at note start (spec: `docs/specs/OS-PERFFX.md` section 10) | Passed on the unit (final release candidate): note start and stop measured on a one-shot and a looped sample, CPU with both held on twelve tracks +0.39 and +0.48 pp (noise), 0 underruns, export byte-identical to Perform off. Left: a multi-slice instrument, and the +100 / -100 values (QA-PRF-151, QA-PRF-152) |
| OS-PERF-REVERSE | Perform PLAYBACK acts: the next notes play backward from the end of the window to its start and stop there; one-shot and beat-slice sampler voices only; the step effect Reverse (26) does the same (spec: `docs/specs/OS-PERFFX.md` section 10.5). Shipped in v1.0.0 | Passed on the unit: one-shot, window and beat slice play backward, effect 26 on one step, export byte-identical. **CPU:** the reversed voices were over their 3 pp gate (+5.34 pp, QA-PRF-154) and were accepted as a known issue; OS-PERF-CPU2 brought the cost to +0.23 pp on the unit. Left: live pad notes played from Master or Song, Retrigger (55) on a backward note |
| OS-PERF-LFO | Perform LFO rows act: VOL, PAN, TUNE, FLT and POS LFO change the speed of the track's LFO for that target; a slot that is Off or an envelope runs the instrument's stored LFO, or a 50 % triangle when the stored amount is under 5 % (spec: `docs/specs/OS-PERFFX.md` section 11). Shipped in v1.0.0 | Passed on the unit: effect on sound measured for each row on an instrument with no LFO, export byte-identical. **CPU over its estimate:** the five rows with a held LOWPASS cost +28.39 pp on twelve tracks (`v1.0.1-cpu2.1`; the estimate was 6 to 7 %); PAN LFO alone +4.8 pp. Accepted as a known issue; OS-PERF-CPU2 moved it from +29.09 pp to +28.39 pp, and the corrected figures are proposed as OS-PERF-CPU3 (IDEA). Left: an instrument with its own LFO, POS LFO on wavetable and granular instruments, step effect 35 with a held FLT LFO |
| OS-SCRATCH | DJ platter scratch: velocity from count timing, 25 ms inertia, Hermite read | the feel; CPU with SCR on |
| OS-SCRATCH-IDLE, OS-SCRATCH-IDLE2 | Scratch with the hand still skips the read loop (bit-exact; the second makes the idle path cheaper) | CPU passed on the unit (SCR on with the hand still: +0.3 pp over off). Left: listen at still-to-moving (QA-PRF-150) |
| OS-INSTR-ARROWS | Instrument: Left and Right edit the row by the fine step, Shift + arrows by the coarse step | the feel |
| OS-LFO-GRAPH | The LFO picture shows the speed over a 16-step window | readability |
| OS-A9-SEAM | The FILTER and COMPRESSOR edits warm over the loop tail when the range starts at the loop start | listen at the loop wrap (limit: a sustain loop in the middle of a sample processed whole keeps a seam) |
| OS-AUDIT-A9 | Sample edit FILTER and COMPRESSOR (triage row 15) | The core passed on the unit, and cancelling a running compressor works. Left: the RATIO / ATTACK / RELEASE / MAKEUP / DETECTOR variations (HIGH PASS, BAND PASS and a selection range passed on earlier images) |
| OS-PRFDEFAULTS | Perform: default values for the three rows of every slot | Every slot reads non-empty defaults on a fresh unit. Left: a row-by-row comparison with the table in `docs/specs/OS-PERFFX.md`, and the values by ear |
| OS-PERFFX | Perform effects act (19 ids, plus BIT DEPTH and OVERDRIVE as ids 20 and 21) | per-effect checks in the Perform catalogue |
| OS-SNDMOD | Sound modules TB-303, Chip, FM (16 algorithms, Detune) | CPU per voice measured on the unit: TB-303 about 15 % of the audio budget (over its 12 % row), FM about 11 % (over its 8 % row), Chip about 2 %; kept, not cut. Left: eight TB-303 at once and a polyphonic chord (not measured) |
| MOD-ABI (OS-MODPITCH) | Module instrument ABI 1.8: a pitch-slide path (Glide, Slide, Arp reach TB-303 and FM), page names | QA-MOD-100 to 104: TB-303 glide CPU and the FM zipper, by ear |
| OS-MODPAGEKEYS | A module's key row shows only the pages it has | QA-MOD-107 |
| OS-APPUX 3.1 | Module app pages declare their key labels (ABI 1.9); `F8` is BACK | QA-MOD-118 |
| OS-FXMODULE | Document the FX that module instruments ignore | none (manual only) |
| ADR 0014 H4 | Export writes (sandbox); SMF EXPORT | QA-MOD-090 to 098 |
| OS-MIDI2 + INSTUI S8b | Instrument MIDI pages (program change, CC) | a real MIDI sink, OFFSET over USB, bend range, VEL> by ear |
| OS-FWCONFIG | Update the firmware from Config, INFO, `F8` | a real install on a unit (Level C) |
| OS-POWER | Hold Power for 2 s to power off; a press wakes | off-current draw (no meter); the click at power off and on is a known gap |
| OS-SCREENSHOT | Shift + Power saves `Screenshots/SHOT_nnnn.bmp` | QA-GLB-130 to 140 |
| OS-LOOPER | Loop, overdub and save on Sample Record | by ear: clicks at punch in and out and at undo, layer alignment, count-in |
| OS-BUG-HOLD, OS-STAMPCLAMP | Button holds stop firing after hours of uptime; input stamps clamped to the clock | a physical-button hold after about 40 minutes idle |
| ODD-11 | Muted tracks keep rendering: voices are skipped when inaudible | by ear (no click), the export byte compare, a dense-event scene |
| OS-CPUOPT1, OS-LFOLIVE (+ OS-BUG-WTLFO) | CPU work on LFO edits heard live and on the looper; wavetable LFO fix | `diag inst` and `diag voices` while it sounds |
| OS-UNDOGRP | Undo granularity and reach per screen | a slow cutoff sweep is one undo unit; Mixer edits have no undo |
| OS-FILES | Delete, rename, recents, purge unused samples, import MOD/S3M/MED, export to OG and `.pti` | the checks of part B |
| SONG-UX (OS-SONGMAP, OS-SONGPADS) | Song editing: the track map becomes an editable grid (copy, paste, clear and transform tracks across the patterns of a range of slots); the pads get two modes on Song | the pads' LEDs in SLOTS; a paste over 30 or more slots while the song plays; QA-SNG-026, 027, 046 |
| OS-KEYROWDLG | The F-key row says what the keys do while a dialog or a question is up | QA-FIL-095 to 097, QA-GLB-118, 120, 121 |
| OS-PATCENTER, OS-OPENTOPAT | Pattern cursor stays centred at both ends; after open, RECOVER or NEW the screen goes to Pattern | the moving loop; QA-FIL-096 |
| OS-PATSWITCH | Change the playing pattern without stopping (hold F1 on Pattern) | by ear: no gap, no double hit, no hung note |
| OS-NEWKEEP | File > NEW asks ALL or KEEP INSTRUMENTS | QA-FIL-070 to 073 |
| OS-BUG-PASTE24 | An OG instrument at VOLUME 100 jumped +24 dB at its first Params edit | none open for the fix; the Paula overshoot is by design (pt2-clone parity) |
| OS-BUG-PROMPTQ, OS-BUG-MEMDISCARD | A question takes the strip's head ahead of news; a discarded take frees its memory at once | `qa_files_b`; the MEM figure after F4 |
| OS-EXPNOTE | An export that finds a held note names the note | export with a pad held on a sampler and on a MIDI instrument |
| PAT-UX part 1 and 2 | Pattern input: REC sign, held Delete, row wrap, pad on Instr column, STEP pads, paste, Insert and Shift+Delete | the checks of both batches |
| REC-FLOW | Count-in on row 1; REC in LIVE goes to WATCH; Shift+Play from the cursor | `qa_playcur.json` and the count-in checks |
| PAT-NAV (OS-ZOOM, OS-QJUMP) | Pattern navigation keys: zoom on Note+Instr, track moves, quarter jumps | 12-track legibility, fast Note+Instr roll, boot time |
| PAT-OPS | Duplicate, expand and shrink a pattern; copy and paste a whole pattern | specified; the unit checks |
| OS-SEDFX (reduced) | Sample editor: normalise and DC removal | `sample_edit_dc.json` |
| OS-AUDIT-B2, B3 | Pad in the Instr column picks the instrument; filter off dims CUTOFF and RESONANCE; loop marks only in loop modes | rows 53, 55, 32, 63 |
| OS-BUG-APPLYSILENT, OS-BUG-TOOLSPLAY, OS-SILENTMSG, OS-SLD-RANGE-ALL, OS-BUG-R3SEL | Silent keys now give a message; Sample edit pads 1..64; a pad note stays inside the selection | one check each on the unit |
| OS-BUG-PREVLAT | The Sample load preview (F5) takes long to start | a 16-bit file starts in 50 to 80 ms on the unit; a 24-bit 96 kHz file takes about 0.75 s more (accepted) |
| OS-BUG-SLDOVER, OS-SLDTARGET | Sample load asks before it replaces a used slot and refuses a module slot; the target instrument is shown and can be changed | on the unit |
| OS-BUG-ADDSLICE, OS-BUG-SLICEPITCH, OS-BUG-WATCHSEL | Chop: ADD SLICE splits the selected slice; slices from the pads sound at the right octave; Shift + arrows in WATCH | on the unit |
| OS-PADOCT | Pattern pads: an octave chooser on Shift+F1, the pad modes one key along | on the unit |

## QUEUED

| ID | What | Parity | Cost | Note |
|---|---|---|---|---|
| OS-GATE-BOOTCRASH | The image gate cannot see a boot crash on the target: every check passed for `v1.0.1-cosm.1` | N | S | 0 | Found with OS-BUG-COSMBOOT (2026-10-31). Ideas: a check_image rule that fails when a `.noinit` symbol moves; a host boot-to-first-frame test on the mk66 boot sequence under ASan/UBSan; a crash-loop guard (after a crash record, boot safe with USB and the console only) so a unit never needs the reset button. The guard is a product change: an IDEA for the maintainer. Partly covered 2026-10-31: `check_image` has a 9th check, `sector_end` ("image end multiple of 4096"), the condition behind OS-BUG-COSMBOOT (fixed on dev by a zero pad to the next 4 KiB flash sector, verified on the unit). Open follow-up: the on-device update validator does not yet apply `sector_end`. The row stays open. |
| OS-PERF-HPBP | The HIPASS and BANDPASS Perform rows cost about 18k cycles a block in the voices | N | S | Slice 1b merged (HIPASS row -16 % on the unit, BANDPASS unchanged). Slice 1c (align the loop heads) measured on the unit 2026-10-30 and dropped: no BANDPASS gain, HIPASS and LOWPASS worse. Slice 2 (a two-voice loop, bench first) waits in the queue. |

## APPROVED (in order)

| ID | What | Parity | Cost | Note |
|---|---|---|---|---|
| OS-QANET | A wider test net that catches classes of bugs, not single cases: rules over every bound action (A), a random user in the simulator (B), a differential audio matrix (C), the simulator nearer the unit (D), an audit of what an owner of the official firmware expects (E), planted mutants to measure the net (F) | N | M-L | Parts A, C, D, E and most of B are merged; findings are in `docs/qa/qanet-findings.md`. Left: B3 (with the SD work), B7 (only if OS-BUG-LOOPPAD returns), the B8 voice-pool and 2000-file-folder fixtures, E3 and D5 (after 1.0), F2 onwards. B6 is dropped. Spec: `docs/specs/OS-QANET.md`. Not in progress since v1.0.0 (2026-10-29): the parts for 1.0 are merged; the rest waits here. |
| OS-AUDIT10 | The remaining minor rows of the audit of what an owner of the official firmware expects (`docs/qa/og-user-audit/triage.md`) | N | M-L | Done: batches A1 to A9, B1 to B6, rows 13, 62, 68, 72. Row 59 (Perform SMP POS, SMP END, PLAYBACK and the LFO speeds) shipped in v1.0.0: OS-PERF-SMP, OS-PERF-REVERSE and OS-PERF-LFO are UNIT PENDING above. |
| OS-BUG-HOTSWAP | Swapping the SD card with a project loaded can crash the unit | R | ? | Seen once with the Dense8 project loaded after about 90 minutes: a memory-protection fault in the render interrupt, then a restart. A swap with an empty project worked. Cause unknown. Workaround: switch off before changing the card. Known issue of v1.0.0. Planned after v1.0.0. |
| OS-SONG-FORM | FORM: build a song from 1-3 patterns in one go, by subtraction (intro with the drums only, a break without the kick...); v1 forms LOOP>SONG, POP, TECHNO | N | S-M | Approved by the maintainer 2026-10-30, in this order. 3 of 6. In Song TOOLS. No format change: slots plus pattern copies; refuses when there are not enough free patterns. |
| OS-SONG-BRIDGE | BRIDGE: N transition slots between pattern A and pattern B whose notes and tracks move step by step from A to B | N | M | Approved by the maintainer 2026-10-30, in this order. 4 of 6. Shares a branch with OS-SONG-FORM. Pattern logic with host tests. |
| OS-SONG-SLOTDATA | Per-slot data: repeats x1..x16, a mute mask, a tempo, a transpose, a Perform scene, a section label and colour | N | L | Approved by the maintainer 2026-10-30, in this order. 5 of 6. Level C: one new optional project chunk (version, backward-compatible loader, round-trip test); the order list keeps its meaning. The OG export expands repeats into slots and warns about what the OG format cannot hold. |
| OS-SONG-JAM | JAM CAPTURE: record on Song while playing; the slots you queue, the mutes and the Perform scenes become slots with per-slot data | N | M | Approved by the maintainer 2026-10-30, in this order. 6 of 6, after OS-SONG-SLOTDATA. |

## IDEA

Proposals. The maintainer decides; nobody codes an IDEA.

| ID | What | Parity | Cost | Note |
|---|---|---|---|---|
| OS-MIDIREC | MIDI tracks: record many notes and CCs, CC automation per track, an event-list editor | N | L | CPU is not the limit (about 0.4 % at the DIN limit of about 1,040 messages a second); the limits are the DIN wire (thin the data, cap per block, notes first), a new per-track event list with sub-step time (a project-format change, Level C; the OG export cannot carry it) and the UI. Radar cards: `docs/vision/radar/cc-automation.md`, `midi-poly-record.md`, `midi-event-list.md`. |
| OS-STATUSLED | An option to turn off the status LED's heartbeat | N | S | Reported by a user (2026-10-26): the LED blinking behind the F keys is a bit annoying. Fault blink codes would still show. A saved setting needs a settings version bump. |
| OS-PROJBLEND | Blend from one project into the next while playing | N | L | Suggested by a user (2026-10-26). The DJ page crossfades track groups inside one project; two projects do not fit in RAM at once, so a candidate is a stem or mix of the outgoing project streamed from the card while the next one loads. Open research question. |
| OS-INSFX | Effect `.tmod` modules in the reserved per-track insert slots and one master insert | N | ? | Level C: audio path, a published ABI, the project stores the inserts (version bump and round trip). Each module measured on the unit, with a cap on active inserts. |
| OS-FXREC | Record MIDI Learn moves into the step FX columns during LIVE recording | N | ? | Shape A (no format change): a binding with an FX equivalent writes that FX at the play line on the cursor's track. Shape B (Learn on an FX cell binds a CC) changes the LRNB chunk: Level C. |
| OS-E2E (later) | Later slices of OS-E2E: MIDI journey, modules in the simulator, format generators, HIL re-scope, card and power chaos | N | M each | Design: `docs/specs/OS-E2E.md` section 2 D8. |
| OS-MODPAGEKEYS follow-up | A module with no parameters still shows F1 `1-8` | N | ? | |
| OS-BUG-REMOUNT L1 to L3 | Latent items from the review: `save_service.cpp` skips slots but still clears `revalidate_`; `og_samples.cpp` takes its stats after bind; the `~epoch` collision at 2^31 remounts is theoretical | N | ? | Not scheduled. |
| Research: a test image did not boot after the update | A bench image ended 0x7C into its last 4 KiB sector and did not boot after an update, 2 of 2; earlier images ending at least 0x114 into it booted. No source defect found. | R | ? | Lead only: the threshold, if real, lies between 0x7C and 0x114. |
| Scales: the 28 other scales | Scales beyond the 12 of the held-Note panel | N | M | Changes the meaning of NOTE `scale` 0..11: needs NOTE 1.1. |
| Song clip preview | One track of one slot, played once | N | M | After the Song track map. |
| Official MIDI CC map | A fixed CC map for mixer, perform and levels (32 CCs, as the official manual documents it) | N | M | A static table, not Learn slots. Documented behaviour, not verified on a unit. |
| MIDI transport on its own port | MIDI transport separate from the clock: its own port, and Off | N | M | Level C, render side. |
| MIDI synth mode: Learn follows the instrument | Learn follows the selected instrument | N | S-M | `kLearnFollow` exists, but `learn_bind` never sets it (`core/include/o3os/core/learn.h`). |
| Perform custom play modes | Custom play modes C1 to C20 | N | M | The orders must be sourced first; there are no tables in the repository. |
| Perform per-track remix | Per-track remix in Perform | N | M | The sequencer already takes a pattern per track; the UI is missing. |
| OS-VOLLAW | Measure the official firmware's level laws against this OS at the defaults and per control | R | S | Real projects store 86 for the track faders and mix levels; this OS puts 100 at unity, so 86 is about -5.6 dB. Hypothesis: 86 is unity on the official firmware. Procedure: `docs/hw/volume-law-measurement.md`. |
| OS-AMPATK0 | The default attack of the amplitude envelope | R | S | In 93 real OG instruments, 55 one-shot and beat-slice ones have 0 ms and about 12 have 10 ms; this OS defaults to 10 ms. First check whether 10 ms was a deliberate anti-click choice. |
| OS-WTLFO-DEFAULT | Wavetable position automation on by default | R | S | One real wavetable instrument has envelope slot 3 enabled as a triangle LFO; this OS leaves it off. Needs a look on the unit first. |
| OG-16TRACK-MTP | Read Tracker+ and Mini 16-track pattern files (reading only) | OG | M | The track block size follows from the published pattern layout (MIT tracker-lib); a real file and the CRC rule are missing. Touches an OG format: the maintainer decides. |
| OG-V17-EVIDENCE | Update `docs/formats/og.md` with project v17 / firmware 1.9.2 facts from the tracker-lib template | OG | S | Level A, after the maintainer agrees. Cite the source and its MIT licence in `THIRD_PARTY.md`. |
| OS-SONG-FLOW | Song flow rows: LOOP n, HALT (wait for the player), JUMP, a chance per slot | N | M | Kept out of OS-SONG-SLOTDATA for now; its chunk keeps room for them. |
| OS-SONG-IDEAS | More Song ideas from the 2026-10-30 research: tempo ramps; a pattern per track per slot (lanes); a living song with a fixed seed and FREEZE into a plain order list; arrangement versions A/B; a fill on a queued jump; cues on the CUE output; a rule-based song co-pilot; text to arrangement on the computer; browsing the song by ear; a high-contrast theme | N | ? | The maintainer moves pieces out. |
| OS-GTIMPORT | Import GoatTracker 2 `.sng` songs as projects with SID tracks (notes, patterns, order, tempo, static instruments). Radar card `docs/vision/radar/goattracker-import.md` | N | M | After OS-SID. Lossy without the per-frame tables, which would need new persisted data. |

Not a source: the unlicensed reverse-engineering repository polyend-tracker-re (no licence, cites firmware addresses). Nothing from it goes into this repository (`CLAUDE.md`, hard rule).

## PARKED

| ID | What | Why parked |
|---|---|---|
| OS-FILES-C (C1b) | A whole project exported in the OG format | C1 and C2 are merged; C3, the export checked on an official unit, is parked. The export keeps its EXPERIMENTAL label. Spec: `docs/specs/OS-FILES-C.md`. |
| OS-MODMX | Modulation matrix (2 LFO + 2 ENV, 16 routes) | The maintainer chose not to add it; ADR 0014 H3c stays parked with it. |
| ADR 0014 H3c | Edit-table route entries and a MOD RECIPES app | After OS-MODMX. |
| OS-APPMIDI C1, C2 | Apps send MIDI (`midi_out` capability, rate limit, chunked SysEx in); read access to the app's own export folder | App modules stay out of the plan for now. |
| OS-BUG-APPEXIT | Module app exit (F7+F8 chord, F8 alone) | Needs a test battery on the unit first. |
| OS-RHYTHMUX | RHYTHM GEN usability (target, while playing, start row) | Needs a walk-through on the unit first. |
| OS-BUG-TUNER | The tuner reads NO SIGNAL on a clean tone from the master | App modules stay out of the plan for now. Scope and Loudness VU read the same tap fine. |
| OS-BUG-SPECQ | The scope's SPECTRUM view is quarantined under load | Check the draw budget under audio load, or draw the bars in two frames. |
| OS-XMIT | XM and IT import | On hold (2026-10-30: other formats go through OpenMPT on a computer, saved as S3M or MOD). |
| OS-SDTHEME | Themes, layouts and keymaps from the SD card (ADR 0009) | On hold. A user keymap holds 32 bindings against 515 built in, so a whole-table keymap would get PoolFull; there is no device loader yet. |
| Perf E1, M3 and the rest of `cpu-budget.md` section 3 | Delay, reverb and limiter loops into fast RAM; FM per-block envelope segments; other kernels | Brought back only by a unit measurement above 80 % render max or any underrun. |
| OS-CPUOPT2 | Second CPU pass: strips update | The expected gain (about 0.2 % of the block) is below the target; the resume conditions are in `docs/specs/OS-CPUOPT2.md`. |
| Automation lanes, Phrases | Per-track automation; phrase sequencing | The modulation matrix would have covered most of it. |
| OS-SMFSONG | SMF EXPORT of the whole song (v1 exports the pattern in view or the selection) | Needs ADR 0014 H4 (merged). |
| OS-SHOTSLOW | The console `shot` takes 9 to 14 s while a song plays | Tooling only. |
| OS-RENAME2 | Safe folder rename | Needs a crash-consistent FAT rename or a mount-time repair of shared-cluster folder pairs. Use SAVE AS and DELETE. |
| OS-CONFILES | Extend the console's `fw put` into put, get, ls and rm for sample folders | On hold. |
| OS-TRASHUI | Restore and really erase folders marked `.deleted` and samples no project references | Real erasure needs a decision. |
| OS-CONVERT44 | A sample-edit action that resamples a sample to 44.1 kHz | On hold. |
| OS-BSSSEG | The MK66 link prints "section `.bss' can't be allocated in segment 3" | To find: whether `.bss` shares a load segment with initialised data (a zeroing or overlap risk) or it is cosmetic. Linker script and boot: Level C if the layout changes. |

## DROPPED

| ID | What | Reason |
|---|---|---|
| OS-STEPPAD | Review the STEP pad mode (Shift+F5) | The STEP pad mode goes; OS-PADOCT takes the Pattern Shift layer over. |
| OS-SONGLOOP | F1 PLAY PAT with a range of slots loops those slots | Dropped when SONG-UX was approved. |
| OS-OGEXPORT-LAW | The OG export of a project below v7 writes v16 with the levels unchanged (plays +24 dB) | Duplicate: folded into OS-FILES-C section 5. |
| OS-BUG-LOOPPAD | A pad on the loop path was silent while the Pattern step sounded | Not seen again. |
| OS-SLOTPROPS | Per-slot REPEAT, mutes, transpose, named sections | A project-format change (Level C) that OG files cannot hold. |
| Link | Ableton Link through a desktop bridge | The Tracker has no network; USB MIDI clock covers it. |
| OS-TRK16 | 16 tracks in Pattern | 12 visible tracks; format intact. |
| OS-GOV2 | Steal sampler voices before an underrun | Only extreme stress fails. |
| Master FX phases 1 and 2 | Headroom, saturation, bass stages; 5-band EQ, Space | Discarded (the limiter stays; the reserved stages are hidden). |
| OS-SYS (light) | Shift+Power capture, generated key reference | The QA catalogue serves as the key reference. |
| Drum synth module, MIDI file import | — | Dropped (chords and voices do not map). |
| Tier-2 scripts | Berry or Lua on the SD card | ADR 0002 amendment 4. |
