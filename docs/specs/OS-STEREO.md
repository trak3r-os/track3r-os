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
| S1 | The field | INST 1.5 appends one byte `stereo` after `vel_sens` (`Instrument` 716 -> 720 B; the undo unit and pool grow with it); the writer emits 1.5 only when a stereo flag is set, so mono projects keep their bytes; older chunks read 0; a value above 1 reads 0, as `engine` does (native_reader.cpp:721-724) |
| S2 | Where S is saved | `samples/<stem>.wav` stays M (older builds and other tools read it unchanged); S goes to a sibling `samples/<stem>_s.wav`; a missing or mismatched S file loads the instrument as mono with a strip message |
| S3 | S | S = L - M, clamped to int16: L = M + S is exact; R = 2M - L is 1 LSB off when L + R is odd (only on an export back to stereo; stable after one pass) |
| S4 | Who decides that S exists | Stereo is the flag AND the bank slot's `Slot::side`: a flag with no S plays M (no read past the block); `adopt`, `edit_commit`, a take and a paste clear or carry it; the flag is reconciled at save and at load (rule 4) |
| S5 | A stereo note with the filter, drive, crush or tremolo on | Plays M (mono) for step 2: the effects run on one block. Stereo effects (both blocks, about +3.1k cycles a voice-block for the filter) are a later option |
| S6 | Decision 4 (outputs) in detail | The track's own outputs stay mono (the USB track stems, the sends, the inserts, CUE and REC taps); the master, the headphones, the USB master pair, MIX and the offline stems are stereo (they read MAIN) |
| S7 | Editor operations on a stereo sample | Crop, Reverse, Fade in / out and Amplify act on both blocks; Normalise takes its gain from the L / R peak; DC remove and Filter run per block; Compressor and the Amiga ops are refused with a strip message ("STEREO: MAKE MONO FIRST") |
| S8 | The load screen | A fourth choice on `F8`: CH MIX, CH LEFT, CH RIGHT, **STEREO** (the key label is at most 8 characters; only for a stereo file; it sets the instrument's flag) |

### Slices (each merges on its own)

- **(a) The field:** INST 1.5, the loader, the writer, the param row and the UI toggle; no audio change. Tests: the round trip, 1.4 files read 0, fixture CRCs unchanged; docs/formats/o3-project.md, tools/o3project.py (`project_python`).
- **(b) The side-car:** a reader mode (M at `pcm[i]`, S at `pcm[even(frames) + i]`; M bit-identical to today's mono load), `commit_load` of both blocks as one unit (RECLAIM, sharing and swaps unchanged), `Slot::side` / `Audio::side`, the S file save and load, CH STEREO.
- **(c) The voice and the mix:** `st_off` in the sample descriptor; `v.st` decided in `mods_start` (mode ONE SHOT / LOOP FWD / SLICE / BEAT SLICE forward, engine Modern, S present, fewer than 4 stereo voices, no effect of S5); `run_st` in flash (the step 1 kernel plus a checked tail for the end and the loop wrap); one branch in `render_seg`; `Graph::feed_side` and a flash `gain_pan_st` into MAIN. An effect that turns on mid-note drops S with a block fade. Tests: a stereo voice with S = 0 equals the mono render; L / R against a reference; the cap; steals.
- **(d) The editor operations** of S7.
- **(e) The `.pti` export:** a "stereo, mid written" count in its report.

### Slice (a) as built (defaults the engineer added, 2026-10-30)

- **The row is virtual, not a table row.** STEREO is `instrument_param::PlayStereo = 0x1F` (like `MidiChannel`): `describe_param` answers `play_stereo_desc()` (MONO / STEREO), the value is the instrument's byte, the edit is `write_play_stereo` (param_actions.cpp). A table row would add a generation slot (`kCount` 95 -> 96, 64 x 4 B = +256 B more in `kModelArenaBytes`, shifting every slot number and the pins of the param tests); the virtual field keeps the arena at 1,933,760 B as rule 3 says.
- **The stub** is `SampleBank::has_side(slot)` (always false, TODO for slice (b)); STEREO is refused with `STEREO NEEDS A STEREO LOAD`, MONO is always allowed.
- **The flag is not reconciled at load or save in this slice** (the writer takes a const project): a hand-made 1.5 file with the flag at 1 loads as 1 and saves as 1; MONO on the row clears it. Rule 4's reconcile (flag AND S) belongs to slice (b), where `Slot::side` exists. No build writes a 1 before it.
- **Review fold (2026-10-30).** A 1.5 chunk of a MIDI or Module instrument with the flag at 1 reads 0 and loads (the reader, C++ and Python; `validate_instrument` still refuses it in memory). `Instrument::reserved[3]` makes the 3 tail-padding bytes explicit and zero (`init_instrument`, `Project::set_instrument`), so the whole-instrument `memcmp` of `Editor::set_instrument`, PASTE and the instrument-file load is defined; `sizeof` stays 720.
- **Python mirror and fixture:** `o3project.py` knows INST 1.5; `o3pj_tool gen stereo` (small project, flag on instrument 0 plus a Sine LFO) and the `stereo` block of `o3project_roundtrip.py`; the dump line of an instrument ends with `stereo=<n>`.

### Slice (b) as built (defaults the engineer chose, 2026-10-30)

- **Layout.** A stereo sample is one arena block of 2 x even(frames) samples: M at `pcm[i]` (bit for bit the mono load, dither stream included), S = L - M at `pcm[even(frames) + i]`, L taken to 16 bits with no dither and S clamped (S3). `Slot::side`, `Step::side`, `Audio::side` and `Pcm::side` point at S; `SampleBank::has_side` replaces the stub. `commit_load(slot, pcm, frames, side)` and `adopt(..., block, side)` take both as one unit and refuse an S that is not `pcm + even(frames)`. `own_len` of a stereo block is the whole 2 x even.
- **What carries and what drops S.** A paste and the clipboard share both (`referenced`, `block_in_history` and the swap records test M and S); undo and redo of a load, a paste and an edit put the version's S back. A mono result (an edit, a take, a mono load) drops S, and its undo brings it back; until slice (d) edits S, an edit of a stereo sample makes it mono.
- **RECLAIM.** Rule 5 as the simple form: `reclaim` refuses a slot whose new audio has S. A mono load over a stereo block is reclaimed as before (the whole old block is the room: the check covers S through `referenced`).
- **Reader.** `ReadParams::side` / `wav::Importer::begin(..., side)` (WAV only; the other readers ignore it and the app says `NO SIDE SIGNAL: LOADED MONO`). The offset comes from the data chunk's frame count; room for 2 x even(frames) is checked before a byte is written and fails `LimitExceeded` (never a clip); `FormatReport::pcm_side` says S is there. A mono file, or `keep_stereo`, reads as ever.
- **Load screen (S8).** F8 cycles MIX, LEFT, RIGHT, **STEREO**, MIX, only on a stereo file (the existing guard). A STEREO load keeps `channel` 0 for the reader, passes `side`, and `finish_import` sets the instrument's flag when the reader says S is there. The audition plays the mix.
- **Save (S2, rule 8).** `samples/<stem>_s.wav` is a 16-bit mono WAV at the instrument's rate. The plan writes every mid, then every side file, then the project file; a failed side job ends the plan like a failed sample. `held` tests S too: `known_side_[64]` (the bank's version last written, 256 B of SDRAM in `SaveService`) and, for a slot read from the folder, `card_version`; after a remount nothing is believed about S (no stamp is kept), so it is written again once. Names (`stereo_side_path`, save_service.cpp, the one rule of the loader, the save and the load screen): the path must be `samples/<name>.wav`, `<name>_s` must fit the guard's 31 characters and must not be another instrument's `sample_path` in any case; otherwise that instrument saves without S and without the flag, and the strip says `STEREO SAVED MONO: <NAME>` (`+N` for more than one; `SaveService::take_stereo_mono`), never silently. The load screen refuses STEREO with `STEREO REFUSED: SIDE FILE NAME TAKEN` when the name the load gives the instrument (`samples/instrNN.wav`) has no side name. No load or save writes over another instrument's file: a side file is only written under a name `stereo_side_path` accepted.
- **Rule 4 at save.** The project is not touched: `formats::native::Writer::begin(project, sink, stereo_mask)` writes the flag only where the mask allows it (`SaveService::side_.ok`, built by `samples_to_write` and `advance_plan`: the flag only where the plan found the side file held or wrote it, so an undo that brings S back between the plan's start and the project job cannot make a flag without its file; a side job that is skipped takes its bit back). With the flag clear, or the mask all ones (the default), the bytes are the file they were (`inst_midi_format_test`: byte for byte). A document whose flag outlived its S (after an edit) therefore saves as mono.
- **Rule 4 at load.** `OgSampleLoader`: after an instrument's mid (native folders only), `Stage::SideNext` / `SideRead` read the side file straight after it into the arena (the slice table goes after both). A missing, unreadable, not mono, shorter, longer, other-rate or no-room S binds the mid alone and counts `SampleLoadReport::stereo_lost` (the summary line says `N stereo lost`, the console logs it); at the end of the load `SampleBank::reconcile_stereo` clears the flag of every instrument without S in the document's own write (no undo unit). A card fault ends the load as for any file. There is no strip text at project load (none exists for the other sample problems); the log line and `diag sd` carry it.
- **Older builds.** An older build never opens `<stem>_s.wav` (no instrument's `sample_path` names it): it plays the mid, drops the flag at its next save, and the side file stays unused on the card. The mid file is unchanged, so every other tool reads the sample as before.
- **Costs, measured (mk66 log, dev 1d1917f9 against this slice).** FLASH 939,596 -> 942,828 B (+3,232 B: the side reader and loader stages, the save plan's side jobs and the bank's S bookkeeping; above the 0.8 to 1.2 KB the step estimated, to look at again in slice (c)); SRAM_L 49,672 B and SRAM_U 135,520 B unchanged; `.ramfunc_l` 27,396 B unchanged; SDRAM_STATIC 159,232 -> 159,264 B. The arena (SDRAM) gives about 0.8 KB more to the bank's and the save service's tables (`SampleBankMem` +540 B: `Slot::side` 64 x 4, `Step`, `Audio`; `SaveService::known_side_` +256 B), so the free-memory figure of Sample load and Sample edit reads 1 KB lower (four `qa_sample_load` and ten `qa_sample_edit` goldens regenerated for that and nothing else). A stereo sample takes 2 x the arena of its mono load. No render-path change.
- **Not in this slice.** No voice or mix change (a stereo instrument still plays M), no editor operation on S (slice (d)), the `.pti` export writes M as before. A stereo take (Record) is the later step in the design direction.

- **Review fold (2026-10-30).** (1) A name collision or a too long stem saves mono with a strip message and the load screen refuses STEREO (above). (2) The flag mask is the plan's, not the live bank's. (3) Known limit, in o3-project.md: a power cut between the new mid and the project file can pair an old project (flag 1) with a new mid of the same length and the old side file; orphan `_s.wav` files are never deleted (latent issue, a cleanup is a later item). (4) The F8 label is `STEREO` (the style guide allows 8 characters). (5) `stereo_side_path` is the one naming helper (the save's `side_file_of` is gone) and `start_side` is `start_sample(slot, side)`. Flash after the fold, mk66 log: dev a29d749e FLASH 954,268 B, this branch 957,580 B (+3,312 B: the collision strip, the plan's side mask and the load-screen refusal cost about what the single helper and the folded `start_sample` saved); SRAM_U 135,520 B and SDRAM_STATIC 159,264 B (+32 B) unchanged by the fold.
- **Defaults to confirm by the maintainer.** Item 1: the sibling name `<stem>_s.wav` is kept, and a colliding instrument saves mono with a strip message instead of getting another name (default, to confirm by the maintainer). Item 4: the label `STEREO` replaces `CH STEREO` (default, to confirm by the maintainer).

### Design rules from the risk review (2026-10-30, folded)

1. INST 1.5 raises `known_minor` (native_reader.cpp:54) AND extends `v1_bounds`' upper bound (:75) by one byte for minor >= 5, or a 1.5 chunk is Malformed (:262, :940). Update `inst_size`, the encoder and `kInstMax`. A 1.4 build reads 1.5 as newer and skips the byte (safe); its next save drops the flag (by design).
2. 1.5 carries the full 1.3 block (engine byte and the 12 MIDI bytes); the Sine LFO gate stays `minor >= 4`. Emit 1.5 only for a Sample instrument with the flag set; `validate_instrument` rejects the flag on other kinds.
3. Pins: `kModelArenaBytes` (project.cpp:89, 1,933,504 -> 1,933,760) and its mirror (mem_map.h:107), mem_map_test:109, model_test:41-42, inst_midi_format_test:107, os-tree.md, o3-project.md:335; the undo unit 1,464 -> 1,472 B (editor.cpp:90; undo is RAM only).
4. Stereo = flag AND `Slot::side`. Reconcile before `native_.set` at save, and at load when S is missing: clear the flag, so a 1 is never saved without its file. The toggle row is refused without S.
5. RECLAIM (`reclaim` in sample_bank.cpp) moves only `frames` and tests `pcm + even == used_`; `rebind_now`, `stage_step` and `make_sample_desc` take only `(pcm, frames)`. All must use the whole block (`own_len` = 2 x even) and `side`; the simplest is to refuse a reclaim of a slot with `side`.
6. `Step`, `Audio`, `Swap`, `clip_audio`, `referenced()` and the `block_in_history` ranges carry and cover the S block, or undo, paste and a spare-buffer reuse can overwrite S or leave it dangling. The project load (`OgSampleLoader::bind`, og_samples.cpp:511-518) is a second path besides app.cpp:2166: the slice table goes after S, and S is a second open per slot.
7. The reader takes S's offset from the header's frame count; it fails with LimitExceeded unless the capacity is at least 2 x even(frames) (never a clip) and passes an explicit `side` pointer to `commit_load`.
8. Save order: the M job, the S job, then the project file; a failed S write aborts the project write. `held` (`samples_to_write`) tests S too. `<stem>_s` fits `kNameMax` and never equals another instrument's `sample_path`.
9. The voice: S gets its own residual and declick, so a stop, a retrigger (`tracker_retrigger`), a steal or a one-shot end does not step S; the voice stays live while S's residual is non-zero; S's slots are freed at `end_block` only. S is dropped when a module or Amiga feed replaces the track, when the voice is gated or a track insert is active; a dropped S stays dropped for the note.
10. `Graph` gets an S centre accumulator and applies the strip's level ramp `lv` to S; a centred stereo strip does not take the mono centre path; mute and a closed fader skip S. Read `.ramfunc_l` from the mk66 log (the 320 B figure is only a starting point).
11. The edit pool (S7) needs 2x per step: refuse cleanly when room runs out, and swap both blocks in one handover.

### Costs [est]

Flash +0.8 to 1.2 KB; SRAM_L +24 to 48 B (dispatch branches); SRAM_U +0.9 to 1.1 KB (the side blocks 4 x 64 x 2 B, `st_off`, voice fields, feed pointers: check the alignment slack); SDRAM +256 B (`Instrument` +4 B x 64) and 2x the arena for a stereo sample; CPU +0.8 pp a stereo voice, at most about 3.2 pp at the cap.

### Risks for the risk review

A read past the block when the flag and the audio disagree (undo, paste, edit); `.ramfunc_l`; the block fade when an effect turns on mid-note; an S file that does not match its M; branches on the hot path that cost mono voices cycles; the cap against a steal and a note-on in one block; the MAIN taps; the undo unit growth; the SRAM_U alignment step.

## Done when (step 1)

The manual section and its QA rows merged; `voice_st` in the bench image only (the release build's sizes unchanged, from the mk66 log); `tools/remote_ci.sh host` and `mk66` green; the unit's `voice` / `voice_st` figures in cpu-budget.md.

## Out of scope (step 1)

The instrument field, the loader, the stereo voice in the engine, the mixer, the editor. They follow the measurement.
