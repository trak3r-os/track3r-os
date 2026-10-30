# OS-STEREO: stereo samples, opt-in per instrument

Level C overall (the audio path, a new instrument field). Approved by the maintainer on 2026-10-30 ("ok D": approach D with every recommended default). Delivered in steps; this spec settles step 1 in full and records the decisions the later steps build on. New Community OS behaviour.

## Facts (2026-10-30, dev 0a9b5ed3)

- Today a stereo file becomes one mono sample at load: the mean (L + R) / 2, or the left or right channel alone with `F8` (**CH MIX / CH LEFT / CH RIGHT**, OS-LOADCH; docs/manual/sample_load.md:135-139, QA-SLD-011).
- The readers can already keep a stereo file stereo (`ReadParams::keep_stereo`, formats/include/o3os/formats/format.h:97-104; the WAV and IFF readers honour it), but nothing downstream uses it: the arena slot, the voice kernels and the mixer strip are mono.
- The official firmware's mono import is documented, not verified on a unit: open research question.
- CPU: Dense8 runs at about 86 % mean on the unit, so a stereo voice has no headroom there; its cost is unknown until measured (estimate +20 to +30 % of a voice).

## Decisions (the maintainer, 2026-10-30)

| # | Question | Answer |
|---|---|---|
| 1 | How it is turned on | Opt-in per instrument; MIX (today's behaviour) stays the default |
| 2 | Voices | At most 4 stereo voices at once; beyond the cap a stereo instrument plays its mid (mono) |
| 3 | Play modes | ONE SHOT, LOOP FWD, SLICE and BEAT SLICE (the chops: the maintainer added them the same day), played forward, Modern engine only; the other modes (LOOP BWD, PING-PONG, WAVETABLE, GRANULAR) and a note played backward play the mid |
| 4 | Outputs | The USB stems and the sends stay mono (the mid); the offline stems (export) stay stereo |
| 5 | Pan | Pan becomes balance, with the same law |
| 6 | Format | A new instrument field (version bump, backward-compatible loader, round-trip test); the `.pti` export downmixes |
| 7 | Order | Document the two-track workaround first |

## Design direction (later steps; re-planned after step 1's figures)

The sample is stored as its mid M = (L + R) / 2, the same buffer a mono load makes today, plus a side-car S = (L - R) / 2 in the arena; L = M + S, R = M - S. Every mono path (the cap, other play modes, USB stems, sends, `.pti` export) keeps reading M alone. A flash stereo kernel (`.ramfunc_l` about 0 B) resamples M and S with one phase; the strip takes a second feed with balance. Editor operations act on both blocks.

**Recording in stereo (proposed; the maintainer asked on 2026-10-30, defaults to confirm).** Today a take is mono: the Record screen averages left and right and `F7` saves a 16-bit mono WAV (docs/manual/record.md:94, :137; QA-REC-034, QA-REC-039). A stereo take fits the same layout: the take is the mid, as today, and the side goes to a second block of the sample memory. Defaults: a MONO / STEREO choice on the Record screen, MONO by default (a stereo take halves **TIME LEFT**); `F7` saves a stereo WAV; `F3` makes a stereo instrument once the instrument field exists, and the mid until then; the looper stays mono. It comes after the stereo voice and the instrument field. Open: the L / R round trip through M and S can lose one LSB when L + R is odd; the step that adds the side-car settles how S is stored so that the saved WAV is bit-exact.

## Step 1 (Level B: manual plus a bench-only spike)

1. **The manual:** a short "Stereo samples" section in docs/manual/sample_load.md, after the CH MIX / LEFT / RIGHT steps: load the file twice into two instruments, one with **CH LEFT**, one with **CH RIGHT**; put them on two tracks with the same notes; pan 0 and 100. Say what it costs (two voices) and that stereo instruments are planned. QA rows `QA-FIL-120..124` in docs/qa/catalogue/sample_load.md.
2. **`voice_st` (bench only):** a new `VoiceMode::Stereo` row in platform/mk66/src/bench.cpp (`run_voice`, :322-324, next to `voice` and `voice_pan`) and a matching host row in tests/bench/audio_bench.cpp. It renders one voice reading M and S (two int16 blocks in the bench's scratch, planar) with the plain voice's interpolation and one phase, writes L and R, and is compiled only when `O3OS_BENCH` is on: the release image's flash and `.ramfunc_l` do not change. Rows at 44.1 kHz: `voice` and `voice_st` with the kernel in flash; one row with it in `.ramfunc_l`, if the bench can place it there without touching the release map, to show what the placement is worth.
3. **Output:** the per-voice cycles of `voice_st` against `voice` on the unit (tester, bench image through `fw-gate-check`), recorded in docs/architecture/cpu-budget.md. They decide whether the cap of 4 holds and how step 2 is cut.

## Step 1 outcome (2026-10-30)

Merged (dev 28b7b355). On the unit the stereo loop costs 2.15x the mono one, +2.1k cycles a 64-frame block, about +0.8 pp a stereo voice in flash; the cap of 4 holds (at most about +3.2 pp) (`docs/architecture/cpu-budget.md`, OS-STEREO step 1). Nit for step 2: the console prints `BENCH voice_st_eq OK` with no line feed (`OKBENCH done`).

**Step 2 (Level C, to spec):** the instrument field (version bump, backward-compatible loader, round-trip test, `.pti` export downmix), the side-car S in the arena at load, the stereo voice for the play modes of decision 3 with the cap of 4, the strip's second feed with balance, the mono paths (USB stems, sends) reading M. A risk review before the engineer.

## Step 2 (Level C): the stereo instrument

Design pass 2026-10-30 (dev 34dc272a, read only). Every slice keeps mono projects bit-identical.

### Facts

- **The field.** `Instrument` (model/include/o3os/model/instrument.h:112-146) is 716 B with no spare byte; pinned by its `static_assert` (:147), the pool (project.cpp:88, 45,824 B) and the undo unit (editor.cpp:90, 1,464 B). The INST payload (native_codec.h:36-49) is versioned per chunk (native.h:57-70: 1.1 rate, 1.2 engine, 1.3 MIDI, 1.4 Sine LFO); the writer picks the lowest version that holds the data (`inst_version`, native_writer.cpp:42-47); the reader gates each field on `chunk_minor_` (native_reader.cpp:721-745; `known_minor` :54, `v1_bounds` :75). A UI choice row follows PLAY ENGINE (model/params.cpp:113, params.h:50, params_screen.cpp:229).
- **`.pti`.** An import goes through `init_instrument` (og_instrument.cpp:29), so it stays mono; the export (app/src/og_export.cpp, `pti_check`) writes the bank's M, so it already downmixes.
- **The arena.** A bump allocator (`reserve`, `commit_load`, app/src/sample_bank.cpp:~165-205); the reader gets `plan.pcm` and `pcm_capacity` (app.cpp:2166-2168). `keep_stereo` interleaves and nothing uses it. Project samples are saved mono (`rq.channels = 1`, save_actions.cpp:260, :284; og_samples.cpp:686 refuses a non-mono project WAV).
- **The voice.** The plain loop is `fast_body` (audio/src/voices.cpp:238) through `fast_steady` / `fast_ramp` from `run` (:317-345); `render_seg` picks the loop (:799-805), with `run_gran` as the precedent of a flash branch. `mods_start` (:1694, flash) decides a note's paths; a reversed note becomes `kModeReverse` (:1701). `post_stages` (filter, drive, crush, tremolo) act on one block.
- **The mix.** A voice block reaches its track at `end_block` (voices.cpp:1268) -> `Graph::feed` (engine.cpp:422) -> the strip pass (graph.cpp:~880-935); the pan law is `pan_gains_q16` (smoothing.h:154), already a balance law (the louder side stays at unity). Inserts, sends (graph.cpp:897), the CUE and REC taps (:974-994) and the USB track stems (`finish_track`, :999) read the track's mono signal; the master, the headphones, the USB master pair, MIX and the offline stems read MAIN.
- `.ramfunc_l` headroom is 320 B after OS-PERF-CPU3 (27,648 - 27,328; STATUS's 176 B is stale).

### Decisions (orchestrator, 2026-10-30; defaults, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| S1 | The field | INST 1.5 appends one byte `stereo` after `vel_sens` (`Instrument` 716 -> 720 B; the undo unit and pool grow with it); the writer emits 1.5 only when a stereo flag is set, so mono projects keep their bytes; older chunks read 0; values above 1 clamp |
| S2 | Where S is saved | `samples/<stem>.wav` stays M (older builds and other tools read it unchanged); S goes to a sibling `samples/<stem>_s.wav`; a missing or mismatched S file loads the instrument as mono with a strip message |
| S3 | S | S = L - M, clamped to int16: L = M + S is exact; R = 2M - L is 1 LSB off when L + R is odd (only on an export back to stereo; stable after one pass) |
| S4 | Who decides that S exists | The bank slot (`Slot::side`), never the model flag alone: a flag with no S plays M (no read past the block); `adopt`, `edit_commit`, a take and a paste clear or carry it |
| S5 | A stereo note with the filter, drive, crush or tremolo on | Plays M (mono) for step 2: the effects run on one block. Stereo effects (both blocks, about +3.1k cycles a voice-block for the filter) are a later option |
| S6 | Decision 4 (outputs) in detail | The track's own outputs stay mono (the USB track stems, the sends, the inserts, CUE and REC taps); the master, the headphones, the USB master pair, MIX and the offline stems are stereo (they read MAIN) |
| S7 | Editor operations on a stereo sample | Crop, Reverse, Fade in / out and Amplify act on both blocks; Normalise takes its gain from the L / R peak; DC remove and Filter run per block; Compressor and the Amiga ops are refused with a strip message ("STEREO: MAKE MONO FIRST") |
| S8 | The load screen | A fourth choice on `F8`: CH MIX, CH LEFT, CH RIGHT, **CH STEREO** (only for a stereo file; it sets the instrument's flag) |

### Slices (each merges on its own)

- **(a) The field:** INST 1.5, the loader, the writer, the param row and the UI toggle; no audio change. Tests: the round trip, 1.4 files read 0, fixture CRCs unchanged; docs/formats/o3-project.md, tools/o3project.py (`project_python`).
- **(b) The side-car:** a reader mode (M at `pcm[i]`, S at `pcm[even(frames) + i]`; M bit-identical to today's mono load), `commit_load` of both blocks as one unit (RECLAIM, sharing and swaps unchanged), `Slot::side` / `Audio::side`, the S file save and load, CH STEREO.
- **(c) The voice and the mix:** `st_off` in the sample descriptor; `v.st` decided in `mods_start` (mode ONE SHOT / LOOP FWD / SLICE / BEAT SLICE forward, engine Modern, S present, fewer than 4 stereo voices, no effect of S5); `run_st` in flash (the step 1 kernel plus a checked tail for the end and the loop wrap); one branch in `render_seg`; `Graph::feed_side` and a flash `gain_pan_st` into MAIN. An effect that turns on mid-note drops S with a block fade. Tests: a stereo voice with S = 0 equals the mono render; L / R against a reference; the cap; steals.
- **(d) The editor operations** of S7.
- **(e) The `.pti` export:** a "stereo, mid written" count in its report.

### Costs [est]

Flash +0.8 to 1.2 KB; SRAM_L +24 to 48 B (dispatch branches); SRAM_U +0.9 to 1.1 KB (the side blocks 4 x 64 x 2 B, `st_off`, voice fields, feed pointers: check the alignment slack); SDRAM +256 B (`Instrument` +4 B x 64) and 2x the arena for a stereo sample; CPU +0.8 pp a stereo voice, at most about 3.2 pp at the cap.

### Risks for the risk review

A read past the block when the flag and the audio disagree (undo, paste, edit); `.ramfunc_l`; the block fade when an effect turns on mid-note; an S file that does not match its M; branches on the hot path that cost mono voices cycles; the cap against a steal and a note-on in one block; the MAIN taps; the undo unit growth; the SRAM_U alignment step.

## Done when (step 1)

The manual section and its QA rows merged; `voice_st` in the bench image only (the release build's sizes unchanged, from the mk66 log); `tools/remote_ci.sh host` and `mk66` green; the unit's `voice` / `voice_st` figures in cpu-budget.md.

## Out of scope (step 1)

The instrument field, the loader, the stereo voice in the engine, the mixer, the editor. They follow the measurement.
