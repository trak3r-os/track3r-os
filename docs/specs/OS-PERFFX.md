# OS-PERFFX: make every Perform effect act, and add BIT DEPTH and OVERDRIVE

Status: spec, 2026-10-18. Backlog: APPROVED row 3. **Level C**: the audio path (`Engine::dispatch` in SRAM_L, and the voices).

- **Risk review:** folded in on 2026-10-18 (§8). Its result:
  - P4 (the perf LFOs) was **PARKED** and is **UN-PARKED** on 2026-10-25 (§11, OS-PERF-LFO). P6 (reverse and SMP END) was **PARKED** and is done as P7 (SMP END, OS-PERF-SMP) and P8 (PLAYBACK, OS-PERF-REVERSE, §10.5);
  - Q3 is changed to b.
  - Both decisions were taken by the orchestrator under the maintainer's delegation.
- **Process:** each slice gets one independent review. `tools/ci.sh qemu` runs before each merge, and each slice measures its CPU on the unit before it merges (`docs/architecture/cpu-budget.md` §5).
- **Timing:** P0 starts in lane A after OS-FILL2. ODD-11 slice 2 has merged.
- **Format:** no version bump. The new FX ids 20 and 21 fit the existing byte of the PERF chunk, and older builds already read an unknown id as none. A downgrade is not lossless, though, and the format doc says so (§8, item 9).
- **Evidence:**
  - every mapping below is **new Community OS behaviour**;
  - what the official firmware's Perform effects do is an **open research question** (the manual is not read, maintainer 2026-10-18);
  - nothing here claims parity.

## 0. What exists (dev bba531a; the fact sheet of 2026-10-18)

- **The table.** `seq/include/o3os/seq/perform.h:63-126` lists 19 FX. They are per track: 12 tracks (`kPerfTracks`) and an `affected_` mask.
  - **Six act:** 4-6 LOWPASS, HIPASS and BANDPASS; 17 STUTTER; 18 PLAYMODE; 19 PAT LEN. They are exactly the `live = true` rows. The screen dims the others (`app/src/screens/perform_screen.cpp:128`).
  - Names are at most 8 characters (`perform.h:90`; `tests/seq_perform_test.cpp:96`).
- **The path.**
  1. `PerformBank::apply` → `perf_command` (`seq/src/perform.cpp:59-92`) builds a `PerfControl {track, group, int16 value, end}` for each FX that has a control group.
  2. The command goes onto the sequencer's ring, then to `ControlTable::set_perf` (`seq/src/control.cpp:152-181`), which puts the perf layer over the two step-FX columns (`resolve`, `:32-52`).
  3. `emit_changes` sends `ModSet` (`a` = the step FX type underneath, `b` = its value, `c` = the layer, `flags` = the group, `value` = the perf int16) or `ModEnd` (`audio/include/o3os/audio/events.h:26-29`).
  4. `Engine::dispatch` (`audio/src/engine.cpp:171-187`, in SRAM_L) sends the Slice, Filter and Filter-LFO groups to their own handlers. It sends every other group to `voices_->fx_event(track, flags, a, b)`, **dropping `c` and `value`**.
- **The 13 that do not act.**
  - **11 are sent and ignored:** 1 VOLUME, 2 PANNING, 7 DELAY, 8 REVERB, 9 SMP POS, 11 PLAYBACK, and the LFOs 12 VOL, 13 PAN, 14 TUNE, 15 FLT and 16 POS. `VoicePool::fx_event` (`audio/src/voices.cpp:1528`) acts only on Slide, then on Glide, Overdrive and Bit depth through `ifx_event` (`:1741`). The perf PANNING is refused at `audio/src/tracker_fx.cpp:180` (`e.c != kLayerPerf`).
  - **2 send nothing:** 3 TUNE and 10 SMP END have no control group (`perform.cpp:78-80`).
- **The step-FX columns of the same groups are ignored too.** The fact sheet found no consumer for 22 Position, 23 Vol LFO, 24 Pan LFO, 26 Reverse, 30 Delay send, 32 Reverb send, 33 Fine LFO or 36 Pos LFO. Micro-tune (34) acts only on MIDI tracks. The step Panning (31) acts.
- **Targets that exist** (`audio/include/o3os/audio/graph.h:185-235`):
  - `Graph::set_track_gain(track, gain, ramp)`, a strip multiplier that `TrackerFx` uses for channel volume;
  - `set_track_pan` / `set_track_pan_mod`, used by the panbrello;
  - `set_voice_sends(track, delay, reverb, ramp)`, set again at every note start;
  - `set_filter_lfo(track, on, rate)`;
  - the instrument LFO generators (`mg[kMgPan]`, `mg[kMgTune]`, tremolo; `voices.h:607-611`);
  - the pitch offset `slide_q16`.
- **Targets that do not exist:**
  - a sampler start offset (`move_position` covers only the wavetable and granular windows);
  - a sample-end override;
  - reverse sample playback (`voices.h:49`: "reversed-sample variants are not here").
- **Overdrive and Bit depth** are per track and applied per voice (`shape_set`, `voices.cpp:1683`). They cost about 17 / 8 / 20 cycles a sample (OS-IFX1, [est]). Off, they cost two byte tests a span. `ifx_event` turns them on only when `a` is the Overdrive or Bit depth step type (`:1750-1756`). A perf-only `ModSet` has `a = 0`, so today it would **end** the FX.
- **FxReset.** It wipes what P1 and P2 would use (§8, item 1):
  - `engine.cpp:207` `clear_slide_fxs()` zeroes `ifx_on_`;
  - `tracker_fx.cpp:61, 63` `end_volumes(true)` resets the track gain and the pan.
- **Offline render.** `offline.cpp:116` builds a fresh Sequencer, while Perform stays on in Master and Song (`app.cpp:1983`).
- **Strip saturation.** `strip_simd` saturates to int16 before MAIN (`kernels_impl.h:216, 234`). The stems, the meters and the export copy that block, so the limiter does not catch a strip's peaks.
- **Persistence.**
  - The PERF chunk is 99 B (`docs/formats/o3-project.md:197`).
  - The reader turns `fx > kPerfFxMax` into none, with row 0 and values 0 (`formats/src/native_state.cpp:198-221`). This is the documented tolerance for later versions.
  - Raising `kPerfFxMax` from 19 to 21 changes no layout. It does change:
    - `PerformBank::sent_` (480 → 528 B);
    - `tools/o3project.py:68`;
    - `tests/tools/o3pj_tool.cpp:300`;
    - `tests/project_state_test.cpp:605-613`;
    - `sim/src/screens_mixer.cpp:310`, which uses `kPerfFxCount`.
- **SRAM_L.** `.ramfunc_l` has about 60 B of headroom. Read the map at the start of every slice.

## 1. The rule for every slice

- **Hot path: net zero bytes.** No slice adds net bytes to `.ramfunc_l`. P0 rewrites the hot call so that it costs no more than today. Every new handler is `O3OS_AUDIO_COLD` (flash) and is called at most once per event. `tools/check_ramfunc.py` must pass.
- **Per-block work reuses what exists:** strip gain, pan, sends and `shape_span`. No slice adds a per-sample loop.
- **Layers.**
  - Perf over step is `ControlTable::resolve`'s existing arbitration: the perf value holds while the pad is down or latched.
  - Each group has one slot plus a perf flag. When perf ends, the step value is sent again, and if there is none the instrument's value comes back (§8, item 5).
- **Perf state is its own.** Perf values live in their own `Strip` and `VoicePool` fields:
  - FxReset (`clear_slide_fxs`, `end_volumes`) never touches them;
  - the offline render saves, clears and restores them (§8, items 1 and 2).
- **Memory.** The new state is SRAM_U. Fields are appended at the end of `Strip`, `Voice` and `VoicePool`, and the map is read every slice (§8, item 10).
- **The screen.** When an FX starts to act, its row in `kPerfFxInfo` becomes `live = true`, so the screen no longer dims it. Its goldens are regenerated: `gallery/perform_*` and the mixer states that use `kPerfFxCount`.

## 2. Slices

| Slice | What | Target | Hot-path cost | Unit check |
|---|---|---|---|---|
| **P0** plumbing | `Engine::dispatch` hands the voices the whole event: `fx_event(track, const SeqEvent&)` in place of 4 scalar args, so `c` and `value` reach them; the non-Slide branch goes to one COLD `layer_event`. Perf fields in `Strip` and `VoicePool` that FxReset leaves alone. `offline_enter` / `offline_leave` save, clear and restore the perf state, and the perf filter too if it has the same hole today (§8, item 2) | — | net ≤ 0 B (a reference in place of 4 args); the map delta is reported | `diag cpu` idle and heavy song: no change |
| **P1** BITDEPTH (20), OVERDRV (21) | `kPerfFxMax` 19 → 21; two table rows; one slot per group plus a perf flag; `shape_set` takes the perf value when the perf flag is set, and a perf end sends the step value again. BITDEPTH 0..15, where the value is 16 − bits: 0 is clean and higher values crush more. OVERDRV 0..100. Tools, doc lines and tests updated | `shape_span` (exists) | 0 | the OS-IFX1 kernels `diag bench voice_od / voice_bit`, and an A/B with perf on **12 tracks**; ≤ OS-IFX1's figures |
| **P2** VOLUME, PANNING | separate `perf_gain` and `perf_pan` fields on the strip, composed in `recompute_strip` (`graph.cpp:534, 547`); the tracker FX keep `trk_gain` and `pan_mod`. VOLUME (Q3 b): −100..0 only cuts, with `gain = (1 + v/100)²` for v < 0; v ≥ 0 is 0 dB. PANNING adds `perf_pan` to the pan, clamped. The `tracker_fx.cpp:180` guard becomes layer-aware | strip gain and pan (exist) | 0 | `diag cpu` with 12 tracks under perf: no change beyond noise |
| **P3** DELAY, REVERB (and step 30, 32: Q1 a) | a per-track send offset kept beside the instrument's sends; every `set_voice_sends` caller adds it, at note start and when it changes; ±100 = −100..+100 % of a full send, clamped | sends (exist) | 0 | by ear plus `diag cpu` |
| **P4** the LFOs | **UN-PARKED** by the maintainer's ask of 2026-10-25, designed in §11 (OS-PERF-LFO): a per-track reconcile of the voices' modulators in flash, about 7 % of a block on 12 tracks accepted | — | — | — |
| **P5** TUNE (and SMP POS if flash-only) | TUNE: a new control group `Tune` (seq and audio ids, `tests/audio_model_ids_test.cpp`); ±48 semitones as a per-track pitch base that every `slide_q16` writer adds, kept in flash (a `kMmSlide` first block). It merges only if its unit CPU passes. SMP POS: a note-start offset of ±100 % of the playable span (and step 22), applied in flash after `note_on`, if it needs no SRAM_L byte | the pitch offset (exists); note start | 0 | by ear, `diag cpu` and `diag mod` |
| **P6** SMP END, PLAYBACK | superseded 2026-10-25: SMP END is done as P7 (OS-PERF-SMP, §10) and PLAYBACK as P8 (OS-PERF-REVERSE, §10.5); the PARKED decision of 2026-10-18 (§8, item 8) no longer applies | — | — | — |
| **P7** SMP POS, SMP END | at note start, no render change, +48 B of `.ramfunc_l` measured (§10.4, OS-PERF-SMP, 2026-10-25); PLAYBACK is costed there (§10.5) | note start | 0 | Dense8 with both on twelve tracks, by ear and `diag cpu` |
| **P8** PLAYBACK | at note start (`mods_start`, flash) plus one flag and the stop in `run<true, true>`: `.ramfunc_l` +52 B measured (§10.5, OS-PERF-REVERSE, 2026-10-25) | note start | est. under 3 pp | Dense8 with PLAYBACK on twelve tracks, by ear and `diag cpu` |

Each slice is one commit with its tests, its QA rows and its unit numbers, merged on its own.

## 3. Decisions

One table. **Q** rows were the maintainer's. **D** rows are defaults, or decisions recorded as such. The risk review was folded in on 2026-10-18 (§8).

| # | Question or default | Options (recommended first) | Applied |
|---|---|---|---|
| Q1 | The step-FX columns of the same groups are ignored too (22, 23, 24, 26, 30, 32, 33, 36) | a) fix both layers in each slice, since the consumer is the same code, and give both their QA rows · b) the perf layer only; open a bug row for the step columns | a, decided by the maintainer 2026-10-18 (the LFO columns 23, 24, 33 and 36 wait with P4, and 26 with P6) |
| Q2 | Names, at most 8 characters | a) `BITDEPTH` and `OVERDRV` · b) `CRUSH` and `DRIVE` | a, decided by the maintainer 2026-10-18 |
| Q3 | The VOLUME mapping | a) −100 is silence, 0 unchanged, +100 is +6 dB · b) −100..0 only cuts; positive values are 0 dB | the maintainer chose a on 2026-10-18; **changed to b** by the orchestrator under the maintainer's delegation, 2026-10-18, after the risk review: the limiter does not catch a strip's peaks (§0, strip saturation). Curve: `(1 + v/100)²` for v < 0 |
| Q4 | PLAYBACK reverse needs new render code (and SMP END a new group) | a) last slice P6, kept only if it meets its budget, else PARKED with the row dim · b) out of this item; they stay dim | a, decided by the maintainer 2026-10-18; P6 is PARKED under that rule (§8, item 8) |
| Q5 | Order of the slices | a) P0, P1, P2, P3, P4, P5, P6 · b) the maintainer's own order | a, decided by the maintainer 2026-10-18; with P4 and P6 parked: P0, P1, P2, P3, P5 |
| D1 | Hot path | net zero `.ramfunc_l` bytes a slice, COLD handlers called once per event, `check_ramfunc.py` green | applied |
| D2 | PANNING with a panbrello | `perf_pan` is its own field; `recompute_strip` adds it to the tracker's `pan_mod`, clamped to the pan range | decided by the orchestrator under the maintainer's delegation, 2026-10-18 |
| D3 | LFO perf values (±23 / ±28) | moot: P4 is un-parked on 2026-10-25 (§11) | — |
| D4 | BITDEPTH range | 0..15, the value is 16 − bits: 0 is clean, higher values crush more; 0 also ends the FX, so the clamp keeps "end" (`perform.cpp:143, 376`) | decided by the orchestrator under the maintainer's delegation, 2026-10-18 |
| D5 | Persistence | `kPerfFxMax` 21; PERF/SCEN 1.1 only when ids 20/21 are used (maintainer, 2026-10-19, option A); the writer writes 1.0 byte for byte otherwise, the reader loads 1.0 and 1.1 (20 and 21 are valid only in 1.1); the doc line becomes 0..21 (1.1); round-trip tests with ids 20 and 21 and for the unchanged 1.0; a test that an id above 21, and 20/21 in a 1.0 chunk, still read as none; the downgrade note of §8, item 9 | applied |
| D6 | Parity | every mapping here is new Community OS behaviour; the official firmware's Perform semantics stay an open research question | applied |

## 4. Real time and hazards

- **Hot-path bytes.** P0 changes `Engine::dispatch`, the one SRAM_L function on this path. The map delta is part of P0's acceptance.
- **Layer races.** `ModSet` and `ModEnd` arrive at block start from the sequencer's ring, which has a single writer. The state they change is read in the same render thread, so there is no ISR sharing beyond today's.
- **FxReset and Stopped.** A reset or a stop must not wipe a held perf value. That is why the perf fields are their own (§1).
- **Offline.** An export runs a fresh Sequencer while Perform may be on. Without the save, clear and restore in P0, a held perf value would leak into the export, or be lost after it.
- **Sends at note start.** `set_voice_sends` is called again from several places (`engine.cpp:92, 113-134, 233, 284, 295`). Every caller must add the offset, or the offset disappears at the next note.
- **The value clamp.** `load_slots`, `recall_scene` and `turn` clamp to `[min, max]`. Every new range keeps 0 as "end".
- **Tracks.** The Perform bank covers 12 tracks. In a project with 16, tracks 13-16 stay outside Perform, as today.

## 5. Done when (per slice)

1. **Host tests, in every slice:**
   - a perf value held across FxReset and across Stopped;
   - perf over a step, then the perf end restoring the step value;
   - perf set while gated, then ungated (`tests/audio_gate_test.cpp`, `tests/audio_advance_test.cpp`);
   - an export compared byte for byte with perf held against perf off (they are equal, P0's offline rule);
   - the "FxReset leaves the layer alone" test in `tests/audio_perform_test.cpp`, extended to each FX of the slice.
2. **Host tests, by area:**
   - `tests/seq_perform_test.cpp` (the table, names ≤ 8, the new ids, the clamp);
   - `tests/audio_voices_test.cpp` (P1, P5);
   - `tests/audio_tracker_fx_test.cpp` (P2 pan);
   - `tests/project_state_test.cpp` and `tests/persist2_format_test.cpp` (P1 round trip);
   - for Q1 a, the step-FX case of each group in scope.
3. **QEMU target suites:** `audio_perform_test` joins them (it is not there today), and `tools/ci.sh qemu` is green before each merge.
4. **Hot path:** `tools/check_ramfunc.py` is green, and the commit body records the `.ramfunc_l` delta (≤ 0) and the SRAM_U delta.
5. **Screen:** the FX's `live` flag is set. The `gallery/perform_*` goldens and the mixer goldens that depend on `kPerfFxCount` are regenerated.
6. **QA and docs:**
   - QA rows QA-PRF-010..049 in `docs/qa/catalogue/perform.md` (005-009 are left for fixes);
   - `docs/formats/o3-project.md` (P1: the 0..21 line and the downgrade note at about lines 193 and 248);
   - the ui-map Perform row;
   - the CHANGELOG Unreleased entry.
7. **The unit:** the CPU numbers of §2's "Unit check" column, recorded in the commit body and in `docs/architecture/cpu-budget.md` before the merge.

## 6. Budget [est]

- **FLASH:** about +1 KB a slice, about +4-5 KB for P0-P3 and P5.
- **SRAM_L:** 0 net.
- **SRAM_U:**
  - +48 B for `sent_`;
  - the appended perf fields: about 12 × (gain, pan, two sends, tune) on the strips;
  - one slot and a flag per group in `VoicePool`.

  Altogether, under 200 B.
- **CPU:**
  - 0 with the FX off.
  - With one on, the cost of the mechanism it reuses: about 0.2-0.5 % per voice for P1, noise for P2 and P3, and per event for P5.
  - P4 would have cost about 7 % of a block on 12 tracks, which is why it was parked (un-parked 2026-10-25, §11).

## 7. State for implementer

### General

- **Order:** P0, P1, P2, P3, then P5. P4 (un-parked 2026-10-25, §11) and P6 (done as P7 and P8, §10) are no longer parked.
- **Before code:** a failing host test for each group in scope, to confirm the fact sheet's "ignored" reading.
- **Each slice:** read the `.ramfunc_l` and SRAM_U free bytes from the map before you touch `engine.cpp`, `voices` or `graph`.
- **Stop and report** if a slice needs an SRAM_L byte, a per-sample loop, or a format change.
- **QA ids:** QA-PRF-010..049.

### P0 packet

- **Goal:** the perf value reaches the voices, survives FxReset and Stopped, and never leaks into an export. No FX acts yet.
- **Files:**
  - `audio/src/engine.cpp` (`dispatch` :171-187, the FxReset at :207);
  - `audio/include/o3os/audio/voices.h` and `audio/src/voices.cpp` (`fx_event` :1528, `ifx_event` :1741; a perf slot and flag per group, appended at the end of `VoicePool`);
  - `audio/include/o3os/audio/graph.h` and `audio/src/graph.cpp` (`perf_gain` and `perf_pan` appended to `Strip`, not yet composed);
  - `audio/src/tracker_fx.cpp` (`end_volumes` must not touch the perf fields);
  - `seq/src/offline.cpp` (:116) and the `offline_enter` / `offline_leave` path.
- **Change:**
  1. `fx_event(track, const SeqEvent&)` in place of 4 scalar args. The non-Slide branch goes to one COLD `layer_event` that reads `c` and `value`.
  2. `offline_enter` saves and clears the perf state, and `offline_leave` restores it. Check whether today's perf filter (Filter group) has the same hole; if it does, fix it in this slice.
- **Tests:**
  - the extended "FxReset leaves the layer alone";
  - perf across Stopped;
  - the export byte-compare (perf held against off: equal);
  - gated, then ungated;
  - `audio_perform_test` added to the qemu suites.
- **Acceptance:**
  - `.ramfunc_l` net ≤ 0 B and `check_ramfunc.py` green;
  - `diag cpu` idle and heavy song unchanged on the unit;
  - `tools/ci.sh qemu` green;
  - a risk review and one reviewer.

### P0 as built (2026-10-18, branch `feat/OS-PERFFX-P0`)

- **The call.** `VoicePool::fx_event(track, const SeqEvent&)`; every group but Slide goes to the COLD `layer_event`, which keeps the layer's value and then tells the step layer (`ifx_event`) what is underneath. `Engine::dispatch` is the same size as before (1,120 B) and `.ramfunc_l` is unchanged (27,588 B).
- **Slots.** `VoicePool::perf_` (`PerfSlots`, 36 B): an int8 value a track for two groups (`kModGroupOverdrive`, `kModGroupBitDepth`; `perf_slot_of`) and a held bit a track for each. P1 reads them in `shape_set`. A later slice that needs another group (Position and Tune, P5) adds a slot there. The row `voice_state` of `mem_map.h` (28 KiB) has **4 B left** for `VoicePool` plus `AmigaVoices`: that slice raises the row from the ADR's slack, with its reason.
- **Strips.** `Strip::perf_gain` and `perf_pan` (int8, the layer's value, 0 none) sit in the padding before `volume`, not at the end: `sizeof(Strip)` and every other offset are unchanged (0 B of SRAM_U). P2 composes them and marks the strips dirty in `Graph::perf_restore`.
- **The export.** `Engine::offline_enter` saves and clears the voices' slots, the performance filter of every track and the strips' fields (`perf_voices_`, `perf_strips_`, 100 B in the `Engine`); `offline_leave` restores them after `offline_quiet()`. **The perf filter had both holes**: a held filter coloured the export's notes, and the export's own Cutoff FX (or its end) dropped the held filter for good. Tests: `audio_perform_test` (the export is byte for byte what it is with Perform off, in both cases).
- **Tests.** `audio_perform_test` (the slots across FxReset and Stopped, the strips, the export), `audio_gate_test` and `audio_advance_test` (the layer's filter arrives while the voice is gated or advanced in closed form, then the gate lifts); `qemu_audio_perform` joins the target suites.
- **SRAM_U is tight in practice.** The linker keeps 32 KiB free between `.bss` and the stack and the margin on dev is about 1.2 KB (34,040 B against 32,768 B), so every slice states its SRAM_U bytes.

### P1 as built (2026-10-19, branch `feat/OS-PERFFX-P1`, stacked on P0)

- **Table.** `PerfFx::BitDepth` = 20 and `PerfFx::Overdrive` = 21, rows `BITDEPTH` 0..15 (group `BitDepth`) and `OVERDRV` 0..100 (group `Overdrive`), both `live`, no labels (the screen shows the number, `---` at 0; `tests/seq_perform_test.cpp` now names the four labelled FX instead of inferring them from the range). `model::kPerfFxMax` 21, `tools/o3project.py` `max_perf_fx` 21; `PerformBank::sent_` 528 B (+48).
- **Voices.** The slots and the perf flag are P0's (`perf_`, 36 B): P1 adds no field, so `VoicePool` is the same size and the `voice_state` row of `mem_map.h` is **not raised** (item 13: it needs raising only when a field is added; 4 B stay). `shape_set` (flash) reads the layer's value when `perf_held`: OVERDRV is the amount (held to 0..100), BITDEPTH is the shift, so `bits = 16 - value` (value held to 0..15). The priority is Perform, then the step FX, then the instrument; the engine caps (Bits12, Bits8) are unchanged. The sounding voice follows through `ifx_event`'s existing `shape_set` call, and the layer's end arrives from the control table as the step FX again (a ModSet of the column's layer) or a ModEnd, so no new code was needed for "a perf end sends the step value again": it is `ControlTable::resolve`'s arbitration, now tested for the two groups (`tests/seq_control_test.cpp`). No change to `Engine::dispatch` or any SRAM_L function.
- **Item 14, decided: a project load clears the layer, by the ordinary road.** `App::apply_project_state` calls `PerformBank::load_slots`, which lets every pad go and forgets the affected tracks (`held_`, `affected_` = 0); the next `poll_perform()` `apply()` sends the end of each FX the bank had sent (`sent_` is kept), the control table sends a ModEnd, and `layer_event` lets the slot go (and the Filter group's handler lets the performance filter go). The live Sequencer is built once at boot, so there is no silent `ControlTable::reset` that could skip the ModEnd. Nothing was added to the voices; the answer is tested at both seams (`tests/seq_perform_test.cpp`: the load sends the ends, once; `tests/audio_perform_test.cpp`: a ModEnd lets the slot go and the next note is the instrument's). QA-PRF-016.
- **Export.** P0's `perf_save` / `perf_clear` / `perf_restore` cover the two slots; `perf_clear` / `perf_restore` do not re-shape a sounding voice because `offline_quiet()` has stopped every voice at both ends (the next note takes the slots). Test: the export with OVERDRV and BITDEPTH held equals the one with Perform off byte for byte, and the live note after it equals the one with no export between.
- **Sizes** (mk66 release image, `ci.sh mk66`, against P0 at 10cf50f built the same way): FLASH +224 B (812,824 -> 813,048), SRAM_L 0 (49,864), `.ramfunc_l` 0 (27,588), SRAM_U 0 (137,992; `PerformBank::sent_` +48 B fits the slack before the next object), SDRAM_STATIC 0 (144,320).
- **Format (D5, option A, maintainer 2026-10-19).** No layout or length change: ids 20 and 21 use the PERF and SCEN id byte; the values fit the existing -100..100 field. **PERF and SCEN are version 1.1 only when a slot (of a stored scene, for SCEN) holds 20 or 21; every other bank is written as 1.0, byte for byte as before** (`state_chunks::version_of`, the pattern of INST 1.2). The reader loads 1.0 and 1.1: 20 and 21 are valid only in 1.1, an id above 21 and 20/21 in a 1.0 chunk read as none (row and values reset). An older build reads a 1.1 chunk as a newer minor and empties the slot (item 9). `docs/formats/o3-project.md` has the 0..21 (1.1) line, the version rules, the downgrade note and a section 10 entry. Round trips: `tests/project_state_test.cpp` and `tests/persist2_format_test.cpp` (1.0 unchanged, 1.1 round trip, a 1.1 file loads, 20/21 in 1.0 and id 22 read as none), `tests/o3project_roundtrip.py` (C++ against the Python reference).
### P2 as built (2026-10-19, branch `feat/OS-PERFFX-P2`)

- **Where it lives.** `TrackerFx::event` (flash, reached through `Engine::tracker_event`; `takes()` accepts the Volume group, `kModGroupVolume = 1`, beside the Panning group) hands the layer's value to `Graph::set_perf_gain` / `set_perf_pan` (a ModSet whose layer is `kLayerPerf` carries it in `value`; a ModSet of a step layer or a ModEnd sets 0). `Engine::dispatch` is untouched (no SRAM_L byte). `recompute_strip` composes: `trk = trk_gain x ((100 + v)/100)^2` for `perf_gain` v < 0 (so the sends carry it too, pre- or post-fader, like the tracker's channel gain), and `pan_gains_q16(pan + pan_offset + pan_mod + perf_pan)`.
- **The guard.** `tracker_fx.cpp`'s `e.c != kLayerPerf` is gone: the Panning FX applies whichever layer won (the performance ModSet carries the step FX in `a` and `b`), and the layer's value goes to the strip. The step FX ending under a held layer (a ModSet with `a == 0` from the layer) now ends the tracker's pan; before it was left until the layer ended.
- **Export.** `Graph::perf_clear` and `perf_restore` mark the strips they change dirty with ramp 1 (§8, item 15): the export's first block has the gains it has with Perform off and the live render's first block has the layer whole. The test "an export is byte for byte what it is with Perform off, whatever VOLUME and PANNING are held" fails without the marks (checked by removing them).
- **Range.** The table keeps -100..100 for both (no stored value changes meaning); VOLUME above 0 is stored and shown but is 0 dB. The PANNING sum is in pan units, so +-50 already reaches a side from a centred track.
- **Gate.** VOLUME -100 closes the strip's level and sends in the smoothers, so the voice gate (OS-ODD11) gates the track's voices and lifts through its ramp when the layer ends (a test).

### P3 as built (2026-10-19, branch `feat/OS-PERFFX-P3`)

- **Where it lives.** The same road as P2: `TrackerFx::event` (flash; `takes()` accepts the Delay send and Reverb send groups, `kModGroupDelaySend = 6`, `kModGroupReverbSend = 7`) hands the layer's value to `Graph::set_perf_send` (a ModSet whose layer is `kLayerPerf` carries it in `value`; a step layer's ModSet or a ModEnd sets 0) and the step FX under whichever layer won to `Graph::set_step_send` (`a` is 30 or 32, `b` the value; otherwise none, -1). `Engine::dispatch` is untouched (no SRAM_L byte).
- **Deviation from the P3 row (same behaviour, a different place).** The row says every `set_voice_sends` caller adds the offset. The offset is composed instead in `recompute_strip`, which every caller already reaches (`set_voice_sends` marks the strip dirty): a note start, the Amiga and module note starts, the live edit of the instrument's send (`instrument_hook`) and the project load all get it, with no list of callers to keep in step and no edit of `engine.cpp`. A test covers the note start and the live edit.
- **The composition.** `level = clamp(clamp(strip send (route-gated) + (step FX if held, else instrument's), 0, 100) + perf offset, 0, 100)`. The offset is added after the sum is held to 100, so -100 closes the send and +100 opens it whole whatever the strip's own send and the instrument's are (a sum of up to 200 would otherwise leave 100 at -100). Like the instrument's send, the offset and the step FX are not gated by the routing bit; the master levels and mutes, the track's mute, a solo elsewhere and the track's VOLUME layer (the tracker gain scales the sends) still close them.
- **The step FX 30 and 32 (Q1 a).** New Community OS behaviour (no consumer existed): the value 0..100 is the track's send in place of the instrument's while the sequencer holds the FX (its step until the next note step, FX Off or another FX in its column: the sequencer's ModEnd), at the notes after it too; the layer is added on top. FxReset ends it (`TrackerFx::reset` -> `Graph::clear_step_sends`: the sequencer's table was reset); so does a project load (`master::VoiceSendsReset`) and an export's start and end (`Engine::offline_quiet` clears them with ramp 1, then runs `TrackerFx::reset`). Stopped leaves it, as it leaves the Cutoff FX (a note played by hand after a Stop takes the held send; `audio_perform_test` asserts it); the next run's FxReset ends it. The official firmware's rule for FX 30 and 32 is an open research question.
- **Memory.** `Strip` keeps its 216 B: the instrument's two sends are bytes now (they are 0..100 integers), which pays for `step_send_a/b` and `perf_send_a/b`. `Graph::PerfSave` +32 B (the export's copy, in `Engine`; the linker's SRAM_U figure is 137,992 B in both builds). `.ramfunc_l` net 0 B (27,608 B in both). FLASH +520 B (814,092 to 814,612 B), SRAM_L and SDRAM_STATIC +0 B.
- **Export.** `perf_save` / `perf_clear` / `perf_restore` carry the two send offsets: the strips they change are marked dirty with ramp 1, as P2's. The test "an export is byte for byte what it is with Perform off, whatever DELAY and REVERB are held" fails without the clear (checked by removing it) and compares the output of two pass-through send effects, so the sends are audible in the file; a step FX held live does not colour the export either, also with a note ringing on its track: `offline_quiet` ends the step sends with ramp 1 as `perf_clear` does (§8, item 15; with ramp 0 the first block of the export climbed down from the FX's send), the case fails without it (checked by removing it). The step FX are not in `PerfSave` on purpose: an export is requested only with the live transport stopped (`OfflineRender::request` refuses `STOP THE TRANSPORT FIRST`, and the export is modal), so the only step send held at its start is one a Stop left, which the next run's FxReset would end anyway; the export forgets it a little earlier, and the Cutoff FX a Stop left is forgotten the same way.
- **Tests.** `audio_perform_test` (7 cases named "(P3)": the offset against the instrument's send for seven pairs, the strip's own send and the routing bit, the next note and the live edit, the step FX and its endings per send, FxReset / Stopped / project load across the fields, the gate (OS-ODD11) lifting when the layer opens a shut track's send and closing again, the export byte compare); `audio_model_ids_test` (groups and FX numbers); `seq_perform_send_test` (2 cases: a DELAY or REVERB slot of the real `PerformBank` through the sequencer's ring and control table and the Engine to the strip, and the step FX 30 and 32 of a real pattern with both slots over them). With the send groups left out of `takes()` all 7 fail, which is the "ignored" reading of the fact sheet.
- **Goldens and docs.** `gallery/perform_*` (12 keys) and `perform_walk/walk_2..7` regenerated; the manual's `perform_walk_2..7` and `perform_qa_prf_001` pictures regenerated (DELAY and REVERB are no longer dim); QA-PRF-026 to 030; the changelog; `official-manual-map.md` and `ui-map.md`.

### P5 as built (2026-10-19, branch `feat/OS-PERFFX-P1`, on top of P1; TUNE only, SMP POS is out)

- **Ids.** `seq::FxGroup::Tune` = 21 (appended after BitDepth, `kFxGroupCount` 22) and `audio::kModGroupTune` = 21; `tests/audio_model_ids_test.cpp` holds them equal and holds the count. The TUNE row of `kPerfFxInfo` is group Tune, live, -48..48; `perf_command` now sends it (0 is the end). No step FX has the group, so the layer is the only source.
- **Voices.** `PerfSlots` has a third slot (`kPerfTuneSlot` = 2, held to -48..48 in `layer_event`, the other two to a byte): 36 B to 54 B. `VoicePool::tune_base_q16(track)` (flash) is the slot's semitones x 65536, and it is the zero of the pitch offset: `mods_start` starts a note at the base (and sets `kMmSlide | mneed` when it is not 0, so the first span makes the pitch, as for a Slide FX), `slide_start` ramps to the target + base, the Slide FX's end in `fx_event` goes to the base, `glide_start` runs from the previous note's offset + base to the base, `tracker_set` lands on offset + base. A TUNE event while a voice sounds adds the difference to `slide_q16` and `slide_tgt` and sets `kMmSlide | mneed`; `mod_block` makes the pitch of it at the next span with the existing `repitch` and lets the flag go. Nothing is in SRAM_L: no render loop, no `mod_block` pass and no `repitch` line changed. FxReset and Stopped leave the slot, the export puts it away with the others (`perf_save` / `perf_clear` / `perf_restore` take `PerfSlots` whole). Sampler voices only: the Amiga voices and the module instruments (`ModuleVoices::fx_event` ignores the group) do not follow it; the manual and QA-PRF-031 say so.
- **Sizes (mk66 release image, against the same tree without the P5 changes, `ci.sh mk66` at -j4 outside the lock).** FLASH +296 B (819,908 to 820,204), SRAM_L 0 (49,880), `.ramfunc_l` 0 (27,608), SRAM_U +32 B (138,120 to 138,152: the control table's value for the new group, 16 tracks x 2 B; the voices' 18 B, `PerfSlots` 36 to 54 B, sit inside the fixed `voice_state` row, which the linker table does not count, as in OS-CPUOPT1), SDRAM_STATIC 0 (146,528). `mem_map.h`: the `voice_state` row is raised from 28 KiB to 28 KiB + 64 B (P0 left 4 B; the slot needs 18 B), taken from the ADR's slack (9.3 KiB are still left over the 32 KiB floor of `kSramUMinFree`); the row's two `static_assert`s in `platform/mk66/src/audio.cpp` pass.
- **SMP POS is not in P5.** It is not a flash-only change. Step FX 22 (Position) has no consumer anywhere in the voices today, so the slice would have to define it: where a negative offset starts (from the end? wrapping?), what it means for a looped sample, a beat slice, a wavetable's window and a granular voice, and what a retrigger does. It also needs a fourth `PerfSlots` slot (more SRAM_U), and a start offset written into `Voice::phase` has to agree with the gate's closed-form advance (OS-ODD11). Parked with SMP END and PLAYBACK (§8, item 8): it returns as its own item with a spec, if the maintainer wants it. The SMP POS row stays dim.
- **Tests.** `audio_voices_test` (3 cases named "(P5)": the note moved by whole semitones bit for bit for 10 values and notes, the ends of the pitch table at +48 on note 100 and -48 on note 10, the value held to 48; the sounding note at the next block, back and forth, across FxReset and Stopped, a ModEnd, another track; the base kept by the Slide FX before the note, its end and a Slide of 0, a TUNE that changes mid-ramp, the Glide, the tracker FX), `audio_perform_test` (the slot per track, the range, a step ModSet and a ModEnd, the export's put-away; an export byte for byte what Perform off makes with TUNE held), `seq_perform_test` (the command, the clamp, the end), `seq_perform_send_test` (a TUNE slot of the real bank through the sequencer's ring and control table and the Engine: the note is the note s semitones away, for +-12, +-48, 7 and -5, bit for bit), the ids. With the base left out of `glide_start`, `slide_start` and the live delta, the "(P5) base survives" case fails in five places.
- **Unit.** Arm TUNE12 in `docs/architecture/cpu-budget.md` (P5): the U scene, the third slot held at +12 on the twelve AFFECTED tracks, read against U on the same image; one visit list with OD12, BIT12 and BOTH12 of P1. By ear and `diag cpu`: QA-PRF-031 and 032. It merges only if that passes (§2, P5 row).
- **Goldens and docs.** `gallery/perform_*` (9: off, typical and edit in three themes; TUNE is no longer dim) and `perform_walk/walk_2..7` regenerated; the manual's `perform_qa_prf_001` and `perform_walk_2..7` pictures regenerated; QA-PRF-031 and 032, the manual's Good to know line, the changelog, `ui-map.md`, `official-manual-map.md`, `cpu-budget.md`.

## 8. Risk review fold (2026-10-18)

Every item was decided by the orchestrator under the maintainer's delegation, 2026-10-18.

1. **P0. Perf state survives FxReset.** `engine.cpp:207` `clear_slide_fxs()` zeroes `ifx_on_`, and `tracker_fx.cpp:61, 63` `end_volumes(true)` resets the track gain and pan. So the perf values go in their own `Strip` and `VoicePool` fields, which FxReset leaves alone. The "FxReset leaves the layer alone" test in `tests/audio_perform_test.cpp` is extended per FX.
2. **P0. Export and offline.** `offline.cpp:116` builds a fresh Sequencer, and Perform stays on in Master and Song (`app.cpp:1983`). `offline_enter` and `offline_leave` save, clear and restore the perf state. If the perf filter has the same hole today, P0 fixes it.
3. **P2. Separate fields.** `perf_gain` and `perf_pan`, composed in `recompute_strip` (`graph.cpp:534, 547`). The tracker FX keep `trk_gain` and `pan_mod`.
4. **Q3 changed to b.** The limiter does not catch a strip's peaks: `strip_simd` saturates to int16 before MAIN (`kernels_impl.h:216, 234`), and the stems, the meters and the export copy that block. So VOLUME −100..0 only cuts, with `gain = (1 + v/100)²` for v < 0, and positive values stay at 0 dB.
5. **P1. Slots, and BITDEPTH's direction.** One slot per group plus a perf flag; a perf end sends the step value again. BITDEPTH: 0 is clean and higher values crush more (value = 16 − bits). The unit A/B runs with perf on 12 tracks, not 8.
6. **P4 (the perf LFOs) was PARKED** (superseded 2026-10-25: §11). It needs a per-track override of `FilterMod` and a depth source, and costs about 7 % of a block on 12 tracks. The rows stay dim.
7. **P5. TUNE.** A per-track pitch base in every `slide_q16` writer, kept in flash (a `kMmSlide` first block). It is conditional on its unit CPU.
8. **P6 (reverse, SMP END) is PARKED.** It cannot meet 0 B of `.ramfunc_l` with about 60 B of headroom. SMP POS may stay in P5 or P6 if it is flash-only, after `note_on`.
9. **Format.** PERF and SCEN are written as 1.1 only when a slot uses the id 20 or 21 (D5, option A, the pattern of INST 1.2) and as 1.0, byte for byte, otherwise. An older build meets the 1.1 chunk as a newer minor: its reader (`known_minor` 0 for these chunks, `kPerfFxMax` 19) accepts it (the length is the one of 1.0), sets `newer_minor` (the load line says "newer fields skipped"), reads the ids above 19 as none with the row and values reset, and its next save writes 1.0 with those slots empty. `docs/formats/o3-project.md` says so. Readers are safe, but a downgrade through a save is not lossless.
10. **Memory.** The new state is SRAM_U. Fields are appended at the end of `Strip`, `Voice` and `VoicePool`, and the map is read every slice. More goldens move than `perform_*`: `sim/src/screens_mixer.cpp:310` uses `kPerfFxCount`.
11. **Tests per slice:**
    - perf held across FxReset and Stopped;
    - perf over a step, then the perf end restoring the step value;
    - perf set while gated, then ungated (`audio_gate_test`, `audio_advance_test`);
    - an export byte-compare with perf held against perf off;
    - `audio_perform_test` in the qemu suites.

### P0 review minors (2026-10-18; the risk and the independent review, both APPROVE WITH MINORS)

12. **Latent, older than P0, not fixed.** An export that ends with its own Cutoff FX still held leaves that FX in `fx_filter_`, and `perf_filter_to` (`voices.cpp:1804`, called by `perf_restore`) re-applies the layer over it, so the live pool keeps the export's cutoff after the export. It predates P0 (the export's Cutoff FX was never put away), so P0 leaves it; recorded as a follow-up.
13. **P1.** It raises the `voice_state` row of `mem_map.h` from the ADR's slack: P0 left 4 B, and a new slot (or any field) needs more.
14. **P1, open question.** Does a project load clear `perf_` and the old performance filter? Only the export's `perf_clear` clears `perf_` today (by grep); P1 decides, and tests the answer.
15. **P2.** `Graph::perf_clear` (`graph.cpp:320`) and `perf_restore` must mark the strips dirty with ramp 1, once `recompute_strip` composes `perf_gain` and `perf_pan`. P0 marks nothing, because nothing reads the fields yet. The test "perform layer (P2 guard)" in `audio_perform_test` cannot fail until P2 writes them.
16. **Option.** The 100 B export copy (`perf_voices_`, `perf_strips_` in the `Engine`) could live in the caller's stack or in SDRAM instead of in SRAM_U. It stays in the `Engine` for now: it must live from `offline_enter` to `offline_leave`, which a stack copy would have to span.

### P2 review minors (2026-10-19; the risk and the independent review, both APPROVE WITH MINORS)

17. **Latent, older than P2, not fixed.** `Sequencer::emit_changes` (`seq/src/sequencer.cpp:107-117`) ignores the result of `emit`, which is false on a queue overflow (`stats_.queue_overflow` counts it). A lost ModEnd of the Volume group would leave `Strip::perf_gain` at its last value (-100: the track silent) until the slot is touched again; a lost ModSet leaves the layer one value behind. A fix would resend the group's last change at the next step (the `apply` rule of `perform.h`), or count it in `diag`; recorded as a follow-up, no fix here.
18. **Folded in the branch.** Audio-level tests for an off-centre track (the Panning FX at 80 plus the layer's +20 and -30) and for the audible return within three blocks of the ModEnd; `Graph::set_perf_gain` / `set_perf_pan` share one clamp-and-mark helper; `TrackerFx::event` tests the Panning group explicitly; the stale comments (`graph.h`, `audio/CLAUDE.md`, `official-manual-map.md`) are updated.

## 9. Default values of the three rows (OS-PRFDEFAULTS, new Community OS behaviour, 2026-10-23)

Rows 1..3 of a slot hold a value each (row 0 is off). Today a new project has 0 (`---`) in all of them. The table below is what each FX gets instead, **in a new project** and **when a slot's FX is changed in EDIT** (Up / Down on the slot, and a pasted or swapped slot keeps what it carries). A saved project keeps its stored values, 0s included: the loader does not apply the table, and no byte of the PERF or SCEN chunk changes meaning (a value is the same int16 field; an old file reads as before). The three values go from mild to strong. A value 0 would end the FX, so the table has none for a live FX.

| Id | FX | Range | Row 1 | Row 2 | Row 3 | Why |
|---|---|---|---|---|---|---|
| 1 | VOLUME | -100..100 (only cuts) | -25 | -50 | -100 | about -5 dB, -12 dB, silence: a duck, a dip and a kill |
| 2 | PANNING | -100..100 | -100 | +100 | -50 | hard left, hard right, half left (a ping-pong by hand) |
| 3 | TUNE | -48..48 semitones | -12 | +12 | +7 | an octave down, an octave up, a fifth up |
| 4 | LOWPASS | -100..100 (cutoff offset) | -30 | -60 | -90 | a darker sound, a muffled one, nearly shut |
| 5 | HIPASS | -100..100 (cutoff offset) | +30 | +60 | +90 | a thinner sound, a tinny one, only the air left |
| 6 | BANDPASS | -100..100 (cutoff offset) | -50 | +30 | +70 | a low band, a mid-high band, a high band |
| 7 | DELAY | -100..100 (send offset) | +30 | +60 | +100 | a touch of echo, a lot, the send wide open |
| 8 | REVERB | -100..100 (send offset) | +30 | +60 | +100 | a touch of room, a lot, the send wide open |
| 9 | SMP POS | -100..100 (percent of the part) | 25 | 50 | 75 | a quarter, half and three quarters in (P7, §10; filled when the FX went live) |
| 10 | SMP END | -100..100 (percent of the part) | 75 | 50 | 25 | the part cut to three quarters, half, a quarter (P7, §10) |
| 11 | PLAYBACK | 0..1 | 1 | 1 | 1 | backward (`<<<`); 0 is forward (P8, §10.5; filled when the FX went live; only one non-zero value exists) |
| 12 | VOL LFO | -23..23 | -8 | -3 | +3 | a slow, a medium and a fast LFO relative to the index in force (P4, §11; filled when the FX went live) |
| 13 | PAN LFO | -28..28 | -8 | -3 | +3 | as 12 |
| 14 | TUNE LFO | -28..28 | -8 | -3 | +3 | as 12 |
| 15 | FLT LFO | -28..28 | -8 | -3 | +3 | as 12 |
| 16 | POS LFO | -28..28 | -8 | -3 | +3 | as 12 |
| 17 | STUTTER | 0..15 (labels) | 11 = 1/4 | 13 = 1/8 | 15 = 1/16 | the retrigger gets faster: four, eight, sixteen to a step |
| 18 | PLAYMODE | 0..3 (labels) | 1 = BACK | 2 = PING | 3 = RND | the three modes that are not the normal one |
| 19 | PAT LEN | 0..8 (labels) | 4 = 8 | 3 = 4 | 2 = 2 | a loop of eight steps, four, two: shorter is stronger |
| 20 | BITDEPTH | 0..15 (bits taken away) | 4 = 12 bit | 8 = 8 bit | 12 = 4 bit | lo-fi light, a sampler, a crunch |
| 21 | OVERDRV | 0..100 | 20 | 50 | 80 | a warm edge, a push, a hard drive |

- **The FX with no consumer keep 0.** A value that does nothing would light the slot (`active`) and the pad for an effect the screen shows dimmed. Their rows are filled when the FX goes live.
- **Where it lives.** The table is data of the model (`model::kPerfDefaultValues`, one row of three per FX id), so a new project (`init_perf_state`) and the bank (`PerformBank::change_fx`, `reset`) read the same numbers. `load_slots` does not.
- **Undo.** None, as for every Perform edit: the slots are runtime state that the App copies into the project outside the undo history (`App::sync_project_state`, `Editor::set_perf_state` is not journalled; `app/src/perform_actions.cpp` header). The FX change and its three values are one operation of the bank (`change_fx`), so they cannot be half applied. Making Perform undoable is a separate design decision.
- **Files.** No format change. A project file that stores a PERF chunk keeps every value it stores, 0s included (round-trip test). A file with no PERF chunk (older or imported projects) reads as the new-project defaults, as it already read as the default FX.

## 10. P7 SMP POS and SMP END (OS-PERF-SMP, new Community OS behaviour, 2026-10-24)

Audit row 59 (E-PERF-01; the maintainer, 2026-10-23): SMP POS, SMP END and PLAYBACK act after the CPU work, or are replaced by other effects if they do not fit. The CPU work is done (Dense8 is the reference: fqa.14, 86.6 % mean, 0 underruns) and `.ramfunc_l` has 364 B of headroom. P7 gives SMP POS (id 9) and SMP END (id 10) a meaning; PLAYBACK (id 11) is a later slice, costed in §10.5. The official firmware's Perform semantics stay an open research question (D6): every mapping here is `new Community OS behaviour`, not parity.

### 10.1 What the two rows do

Both act **at note start only**. The layer's value is read in `VoicePool::note_on` (which itself runs from SRAM_L; the arithmetic is in the flash helpers `smp_window` and `smp_frame`), which sets the voice's first position (`Voice::phase`) and its last frame (`Voice::end`) before the voice sounds. A voice that is already sounding is not moved: the render loops, the closed-form advance of a gated voice (OS-ODD11) and `mod_block` are not touched, so there is no `.ramfunc_l` byte (D1). The "part" below is the frames the note plays from: the slice the note names for a beat-slice instrument, else the whole sample; `L` is its length in frames.

| Value | SMP POS (id 9, -100..100) | SMP END (id 10, -100..100) |
|---|---|---|
| 0 | off (0 ends the FX, as for every Perform row) | off |
| +1..+100 | the note starts at `v` % of the part (+50: the middle; +100: at its end, so the note is silent) | the note stops at `v` % of the part (+100: the whole part, the same as off) |
| -1..-100 | the note starts so that the last `|v|` % plays (-25: the last quarter; -100: the whole part, the same as off) | the last `|v|` % is cut off (-25: the note stops at 75 %; -100: nothing plays, silent) |

- **The arithmetic.** `f` = `v` for a positive value, `100 - |v|` for a negative one; the frame is `(L / 100) * f + ((L % 100) * f) / 100`, floor, in 32 bits (`L` is at most the SDRAM sample size, so nothing overflows). Values outside -100..100 are held to it.
- **Start after end.** If the start frame is at or past the end frame (or the end frame is 0), the note plays nothing: `note_on` cuts the track's sounding note, counts `ignored` and returns false, as it does for a beat slice with no length. The pitch, the envelope and the instrument's other settings are not involved.
- **The pitch** is the note's own; the start offset does not change the rate. The gain ramp of the start (`kAttackSamples`) covers the jump. The stop at the end frame is a hard cut at the sample, as a beat slice's end is (no render change can fade it; follow-up if the unit shows a click).

### 10.2 Modes

| Instrument | SMP POS | SMP END |
|---|---|---|
| One shot (no loop) | start frame as above | end frame as above (`Voice::end`) |
| Beat slice | within the slice the note names (`L` is its span) | within the slice |
| Looped (forward, backward, ping-pong) | `L` is `loop_end`: the note starts inside the first pass, before or inside the loop, and the loop runs as ever (a start at or past `loop_end` is the silent case) | **ignored**: a loop has no end, and moving its end would change the loop; the note plays as with the FX off |
| Granular | ignored (its position is the grain window's: POS LFO, `move_position`) | ignored |
| Wavetable | ignored (its position is the window) | ignored |
| Amiga voices, module instruments, MIDI tracks | not followed (sampler voices only, as TUNE) | not followed |

- **Other FX.** The Slice FX (step 25) sets the position itself after the note starts and wins over SMP POS. A tracker Retrigger (FX 55) restarts the voice at the start of its window, the layer's SMP POS frame the note began at (`Voice::gstart` of a non-granular voice, set by `mods_start` in flash; 0 without SMP POS, so the old behaviour), and SMP END keeps applying (`tracker_retrigger`; a later change of the layer value does not move it); the Perform stutter sends new notes, so each repeat starts at the offset. The step FX 22 Position stays without a consumer (its column is not this item; the audit row concerns the Perform rows).
- **Held layer and a sounding note.** Changing the value does nothing to the note that sounds; the next note starts at the new offset (the layer's end, row 0, goes back to the full part at the next note). Nothing is retriggered.

### 10.3 Mechanism

- **Ids.** `seq::FxGroup::Position` (9, exists, no consumer before) carries SMP POS: `audio::kModGroupPosition` = 9. SMP END gets a new group `seq::FxGroup::SampleEnd` = 22 (appended, `kFxGroupCount` 23), `audio::kModGroupSampleEnd` = 22. No step FX has the new group, so the layer is its only source; the step FX 22 shares group 9 and is still ignored. `tests/audio_model_ids_test.cpp` holds the numbers equal and the count.
- **Voices.** `PerfSlots` gets two slots, `kPerfPosSlot` = 3 and `kPerfEndSlot` = 4 (`val` is an int8 already, -100..100 fits; `on` two more masks): 54 B to 90 B. `layer_event` keeps them like the others (held to -100..100); `note_on` reads them through two flash helpers. The export puts them away with the others (`perf_save` / `perf_clear` / `perf_restore` take `PerfSlots` whole), FxReset and Stopped leave them (§8, item 1).
- **Table.** `kPerfFxInfo`: SMP POS `live` (group Position), SMP END `live` (group SampleEnd); `perf_command` sends both (0 is the end). The Perform screen needs no change of its own: it draws a row dim by the table's `live` flag, so the two rows are no longer dim and the key and help text are unchanged (no `app/` file is touched).
- **Defaults** (OS-PRFDEFAULTS, §9; a new project and an FX change in EDIT, never a stored value): SMP POS 25 / 50 / 75 (a quarter, half and three quarters into the part), SMP END 75 / 50 / 25 (the part cut to three quarters, half, a quarter): mild to strong, none 0 (0 would end the FX).
- **Format.** No stored byte changes meaning: a Perform value is the same int16 under the same id; ids 9 and 10 were storable before (PERF 1.0) and their stored values (all 0 in files written by the defaults of §9) read as before, off. No version bump. Files that stored a non-zero value in id 9 or 10 (hand-edited; nothing wrote one) now act. The control group is runtime only.

### 10.4 Export, determinism, budget

- **Export.** `offline_enter` clears the layer and `offline_leave` restores it, so an export with SMP POS or SMP END held is byte for byte the export with Perform off (the TUNE rule, tested). The start and end frames are pure integer functions of the layer value and the sample, so a live note is deterministic too.
- **Measured (mk66 release image, `ci.sh mk66`, against dev's fqa.14 tree, 2026-10-25):** FLASH +224 B (925,460 to 925,684), SRAM_U +32 B (134,848 to 134,880; against the +68 B estimate below the symbols did grow, `nm -S` on both images: `g_voices` +40 B (23,064 to 23,104: `PerfSlots` +36 B and tail padding), `g_engine` +32 B (the control table's group) and `g_seq_storage` +32 B, but the `.bss` objects are placed by alignment and the three growths are taken up by the padding between them, so `.bss` grows 130,752 to 130,784 B only; `PerfSlots` fits the `voice_state` row, which is not raised; the `sequencer` row of `mem_map.h` is raised by 64 B because the sequencer is 8,856 B against the 8,832 B its assert allowed), SRAM_L +48 B: `.ramfunc_l` 27,284 to 27,332 B, headroom 364 to 316 B. The only changed symbol is `VoicePool::note_on` (1,064 to 1,112 B): `note_on` runs from SRAM_L (it is a hot symbol, not flash as the estimate below assumed), so the mask test, the call and the register spills of the new branch count; the work itself (`smp_window`, `smp_frame`) is flash. This breaks D1 (net zero) by 48 B; the cost is recorded here, a follow-up can shave it by moving the whole `note_on` tail into flash. The review fold (Retrigger from the window's start, `tracker_retrigger` and `mods_start` in flash, no new field: a first `Voice` field made the `Voice` 8 B larger and moved +20 B of `.ramfunc_l` through the stride) leaves `.ramfunc_l` at 27,332 B and SRAM_U at 134,880 B, FLASH 925,700 B (+16 B).
- **Budget [est, before the measurement]:** `.ramfunc_l` 0 B (the target; checked in the mk66 map); FLASH about +400 B (two helpers, the `note_on` branch and the slots); SRAM_U +68 B (`PerfSlots` +36 B inside the `voice_state` row, `ControlTable::perf_val_` +32 B for the new group), SRAM_L 0, SDRAM_STATIC 0. CPU: two byte tests at a note when the slots are off, a few dozen cycles at a note when they are on; none per block. A voice with a late start does less work, not more. Unit check: Dense8 with SMP POS and SMP END on twelve tracks, by ear and `diag cpu`, against the same scene with them off.
- **Hardware checks still open (not done by a host test).** (1) Dense8 with SMP POS and SMP END held on twelve tracks: `diag cpu` against the same scene with the rows off, and by ear. (2) QA-PRF-151 and QA-PRF-152 by ear on the unit; the export check is two exports, one with the rows held and one with Perform off, compared with `cmp` (identical).

### 10.5 PLAYBACK (id 11, reverse): mechanism, cost, recommendation

- **Mechanism.** The backward loop already exists: `run<Ramp, true>` (`run_dir`) walks the position as a signed Q32.32 between `loop_start` and `loop_end` and reflects at the ends. A reversed one shot is that path with the loop being the whole part, started at the last frame (`phase = (end - 1) << 32`, `bwd = true`) and **stopping** where a backward loop would wrap to the end: one test of a new voice mode (a "backward one shot", a fourth `loop_mode` value, so `v.mode != 0` already sends it to `run_dir` and out of the gate's closed-form advance) at the `q <= lim_lo` branch. The note start sets the mode, the slots (a third new `PerfSlots` slot, +16 B) and the beat slice's span; the granular, wavetable and looped instruments are skipped as in 10.2.
- **Cost [est, not measured].** `.ramfunc_l` +40 to +80 B (the flag, the stop and the start state; a runtime flag, not a new template instance, which would add about 600 B), inside the 364 B headroom. Cycles: 0 per forward voice; a reversed voice runs the checked Dir loop and has no fast path (§ OS-PERF3), so it costs about 0.4 to 0.6 k cycles per voice-block (about 6 to 9 more cycles a sample than the forward fast path, times 64 frames; 0.15 to 0.22 % of the 261,120-cycle block), and a gated reversed voice is not advanced in closed form (it renders like slice 1). On twelve tracks at once that is 1.8 to 2.6 pp of a block, inside the 3 pp gate and Dense8's margin; to be measured on the unit (`diag cpu`, REV12 of docs/architecture/cpu-budget.md).
- **Recommendation: implement**, as slice P8 after P7 is on the unit, with the Dir-path stop and a unit CPU check on Dense8 (reverse on twelve tracks). It is a few dozen bytes of hot code and no new loop, so a replacement effect is not needed; if its unit CPU fails, replace the row with a cheap existing effect rather than park it: the nearest is the step column's already-working Glide / Slide pair (a down-slide at the note start), which the voices run in flash today. SMP POS and SMP END do not depend on it.

#### As built (P8, OS-PERF-REVERSE, 2026-10-25, branch `feat/OS-PERF-REVERSE`)

- **Built as designed, with two changes to the text above (new Community OS behaviour).** (1) The "fourth `loop_mode` value" is `Voice::mode` 4 (`kModeReverse`; 3 is already `kModeGranular`). (2) No new `PerfSlots` slot: the layer's PLAYBACK is a 16-bit mask `perf_rev_` (put away by `perf_save` / `perf_clear` / `perf_restore` through `PerfSave::rev`), the step FX 26 another, `step_rev_` (cleared by `tracker_reset`, `perf_clear`, `perf_restore`). `PerfSlots` stays 90 B. The note-start work is in `mods_start` (flash), so `note_on` (SRAM_L) is untouched; the only hot change is `run<true, true>` (`rev` flag, one entry check, the stop branch) and `tracker_retrigger` (flash). Step FX 26 (Q1 a) is covered at no hot cost: `layer_event` reads `ModSet a == 26, b != 0` and the layer's `value != 0`; the SMP window reverses with the same path (window = `[gstart, end)`). Defaults: PLAYBACK rows 1 / 1 / 1 (`kPerfDefaultValues`; `seq_perform_test` relaxed to "three distinct values only if the range has three non-zero values").
- **Measured (mk66 release image, `ci.sh mk66`, against 71ef26b7, the P7 merge):** `.ramfunc_l` +52 B (27,332 to 27,384 B, headroom 316 to 264 B; the estimate was +40 to +80 B; the second exception to the net-zero rule after P7, D1, recorded in `audio/CLAUDE.md`), FLASH +260 B (925,700 to 925,960), SRAM_L +48 B (49,608 to 49,656, the same code), SRAM_U 0 (134,880 both: the `PerfSave` +2 B and the two masks +4 B fall inside the alignment padding between the `.bss` objects), SDRAM_STATIC 0. No `mem_map.h` row moved and no assert fired. CPU: not measured. The one estimate [est] is 6 to 9 cycles a sample on top of the forward fast path, so 0.4 to 0.6 k cycles per reversed voice-block (0.15 to 0.22 % of the 261,120-cycle block) and 1.8 to 2.6 pp for twelve reversed one-shot tracks (docs/architecture/cpu-budget.md, OS-PERF-REVERSE). QA-PRF-154 and the REV12 arm stay open (nothing was measured on the unit); the gate that applies is REV12's: within 3 pp of the same scene with PLAYBACK off, underruns 0, over-budget windows not above off's. If it fails the row is replaced by a cheap existing effect (above).
- **Tests.** `tests/audio_voices_test.cpp` (6 P8 cases: bit for bit the reversed frames forward, the stop at a block end and inside a block, the half-frame step, 0 and ModEnd, the step FX 26 and `tracker_reset`, SMP POS / END and beat-slice windows, Retrigger, looped / granular / wavetable ignored, a gated reversed voice), `tests/audio_perform_test.cpp` (2), `tests/audio_model_ids_test.cpp` (`kModGroupReverse` = 8, `kFxReverse` = 26). A voice that ends exactly at the end of a call stops at the next call, as a forward one shot does (the 500-frame window needs eight blocks of 64: the earlier failure of the SMP-window case was its test checking after seven).
- **Format.** No layout, length or version change: the PERF chunk keeps its id byte (11) and its int16 values, and a value of 1 is inside the range 0..1 (P7 precedent, §10.3). Only the defaults move: `kPerfDefaultValues` (model/include/o3os/model/project_state.h:105) makes a new project, and a slot whose FX is changed to PLAYBACK, store PLAYBACK 1 / 1 / 1 where it stored 0 / 0 / 0. A file saved before reads as it did (its stored 0 is forward, and a stored 1 acts); no loader change. Nothing else in a saved file changes meaning, so the earlier "nothing changes in saved files" is exact only for files already saved.
- **Held step FX 26 and Stop.** The step FX 26 is held per track, like the Slide FX: `Stopped` does not end it and the next fresh Play (`FxReset`, `tracker_reset`) does. A pad note played after Stop is therefore still backward until then; the layer's PLAYBACK is separate and survives both. Covered by `tests/seq_perform_send_test.cpp` (a real pattern step with FX 26 through the Sequencer and the Engine is bit for bit the reversed frames played forward; the voice after Stop is backward; the next Play ends it) and by `tests/audio_voices_test.cpp` (layer 1 over a step FX 26 of 0 is backward).
- **Latent (not changed; hot code).** The stop stores `q < lim_lo` in `v.phase`, so for one block the Wave screen's `live_position()` of a voice that stopped on a call's last sample can read the end of the part (a playhead flash at the end). No sound is affected. Fix only if the flash is seen on the unit.
- **Goldens.** `perform_walk` walk_2 / 4 / 5 / 7 and the `perform_off`, `perform_typical` and `perform_edit` gallery pictures in the three themes (the PLAYBACK row is no longer dim and reads `<<<`); the manual's five Perform pictures follow.
- **Hardware checks still open (not done by a host test).** (1) Dense8 with PLAYBACK held on twelve tracks: `diag cpu` against the same scene with it off (QA-PRF-154). (2) By ear (QA-PRF-153): the backward note, the slice, SMP window, the step FX 26. (3) Export check with PLAYBACK held against Perform off, `cmp` (identical).
- **QA numbers:** QA-PRF-153 / 154 are this slice's; OS-PERF-LFO (section 11) has 160 / 161 / 162.

## 11. P4 the Perform LFOs (OS-PERF-LFO, new Community OS behaviour, decided by the orchestrator, 2026-10-25)

The maintainer (2026-10-25): "the Perform LFO speeds do nothing". P4 was PARKED (§8, item 6) for two costs: a per-track override of `FilterMod` and a depth source, and about 7 % of a block on twelve tracks. The orchestrator took the decisions below under the maintainer's delegation (he is away); the 7 % is accepted (features are not cut for CPU). **Level C** (the audio path). Every mapping is `new Community OS behaviour`; what the official firmware's LFO speeds do is an `open research question` (D6). Nothing here claims parity.

### 11.1 What the five rows do

The row's value `o` is the LFO speed as a **signed offset on the speed index** of the track's LFO for that destination (+ faster, clamped to the tempo-sync table: 0..28, the amp slot's 0..23). A value 0 ends the FX, as for every row. Rows 12..16 are VOL LFO (id 12, -23..23), PAN LFO (13), TUNE LFO (14), FLT LFO (15), POS LFO (16) (each -28..28). The destinations are the instrument's modulation slots 0 (amp), 1 (pan), 5 (finetune), 2 (cutoff) and 3 / 4 (wavetable window / granular position).

| The instrument's slot for the destination | What the row does |
|---|---|
| an **LFO** (source LFO) | its shape and amount stay; only the speed index moves: `clamp(own index + o)`. The LFO keeps its phase (the transport's), the period is made again as a live SPEED edit does (`lfo_live`) |
| **Off** or an **envelope** (ADSR) | a Perform LFO runs on that destination for the track: the **slot's stored LFO shape and amount** (`FilterMod::shape`, `FilterMod::lfo_amount`; a new instrument stores triangle and 50 %), at the index `14 + o` clamped (index 14 is "1 step", `kLfoRate[14]` = 1.0). The envelope, if the slot has one, is replaced for FLT / PAN / TUNE / POS (one modulator a slot: the same rule as the Filter LFO FX 35) and **kept** for VOL (below) |

- **VOL LFO** is a tremolo (`kMgTrem`, `trem_span`: the voice multiplied by the LFO's level, 0..amount). With an LFO in the amp slot the speed index moves (the amp table: index `own + o`, 0..23). With the amp slot Off or with an amp **envelope** the tremolo runs on top of the plain gain or of the ADSR (the player's envelope is a separate stage; the tremolo is the pass after it). The level swings between 0 and `amount` (amount 50 %: the voice breathes between silence and half): that is the instrument's own tremolo law, kept; the slot's LFO AMOUNT is the depth.
- **PAN LFO** is the autopan (`kMgPan`): the track's pan swings by `amount x 100` each way (amount 50 %: +-50 of 0..100); the pan goes to the strip as the instrument's pan modulator does (`pan_val_`, `Graph::set_voice_pan`). When the layer ends the instrument's pan comes back at the next block.
- **TUNE LFO** is the vibrato (`kMgTune`, the finetune modulator): `amount x 100` cents each way (50 % = +-50 cents, a semitone at 100 %), added to the instrument's fine tune, over the layer's TUNE and the slides.
- **FLT LFO** is the wah / filter sweep (`kMgCut`): it moves the cutoff of **the filter that runs** (the instrument's own filter, a Cutoff FX, or a held LOWPASS / HIPASS / BANDPASS: the layer's filter is the natural pair) by `amount` each way. On a track with no filter running there is nothing to move: ignored (the Filter LFO FX 35 has the same rule). The row wins over the step FX 35 on the same track (layer over step, as for every group); at the layer's end the step FX 35 comes back.
- **POS LFO** moves the wavetable window or the granular position only (`kMgPos`); on any other sampler instrument it is ignored (the descriptor has no position modulator, `set_sample`).
- **Sampler voices only** (as TUNE): module instruments, MIDI tracks and the Amiga voices ignore the five rows (`ModuleVoices::fx_event` already returns for these groups).
- **Tempo sync and stopped.** The LFOs are the transport's: the tick count, the cursor step's shift (`mod_start`), and they run while the transport is stopped (OS-BUG-WTLFO, OS-LFOLIVE). A held row follows a tempo change like the instrument's LFO.
- **Step FX.** The step-FX LFO columns 23 (volume), 24 (panning), 33 (finetune) and 36 (position) keep **no consumer**: the Perform layer is the only source of these groups (it reads only the events whose layer is Perform). **Follow-up** (not this slice): give the four columns the same mechanism. Column 35 (filter) keeps its own consumer.
- **When.** The layer acts on the sounding note at once (the LFO starts, its speed moves, or it ends and the instrument's own modulator comes back, as a live SOURCE edit does) and on every later note of the track (`mods_start`). FxReset (transport start) and Stopped leave the layer (§8, item 1).

### 11.2 Mechanism

The state is one more set of `PerfSlots` and a reconcile function in flash; no per-sample code is added and no hot function grows.

- **Ids.** The groups exist (`seq::FxGroup::LfoVolume` 11, `LfoPanning` 12, `LfoFinetune` 13, `LfoFilter` 14, `LfoPosition` 15; `kPerfFxInfo` rows 12..16 already name them). `audio/events.h` gets `kModGroupLfoVolume` = 11, `kModGroupLfoPanning` = 12, `kModGroupLfoFinetune` = 13, `kModGroupLfoPosition` = 15 beside the existing `kModGroupLfoFilter` = 14, held equal to the seq ids by `tests/audio_model_ids_test.cpp`. **No new group, no change of `kFxGroupCount`** (PLAYBACK's group will be the only new one of the two branches).
- **Slots.** `VoicePool::PerfSlots` grows from 5 to 10 slots (`kPerfSlots`): slots 5..9 = VOL, PAN, TUNE, FLT, POS LFO (`perf_slot_of`: group - 6 for the groups 11..15); `val` is the int8 offset (+-28), `on` the held mask. 90 B to 180 B (+90 B: `val` is 12 tracks x 10 slots, `on` ten masks). `layer_event` (flash) keeps them like the other slots (held to +-28) and then reconciles the sounding voice of the track. `perf_save` / `perf_clear` / `perf_restore` take `PerfSlots` whole: the export needs no new code.
- **Dispatch.** `Engine::dispatch` handles the group 14 in two `else if` branches today (`engine.cpp:189`, `:206`: `set_filter_lfo`). Those two branches are removed (the group goes to the `else` that calls `VoicePool::fx_event`), and `layer_event` (flash) calls `lfo_set_impl` for it, so the Filter LFO FX 35 behaves as before and the layer's FLT LFO reaches the voices. This makes `.ramfunc_l` **smaller**, not larger (D1).
- **Reconcile (flash).** One function, `VoicePool::perf_lfos(Voice&)`, brings a voice's five generators to what the instrument and the layer ask for. For each destination it computes the wanted LFO index (above) and compares it with the voice's `ModGen`: not running: `mod_start(g, m, cursor_step_, 0, index + 1, ...)` (the `lfo_fx` argument is already the override of the rate: no new code in the hot `mod_start`), mask bit and `mneed` set (for the cutoff `Voice::fmod` is set when `v.filt`); running as an LFO: only the period and its reciprocal (and the phase shift) are made again (a helper factored from `lfo_live`); the layer gone and the instrument has nothing: the generator ends and the destination goes back to the instrument's value (the pan through `pan_val_` / `pan_dirty_`, the fine tune through `repitch`, the position through `move_position(0)`; the tremolo ramps back to unity by itself). It is called from `mods_start` (note start), `layer_event`, and after `remod`, `lfo_live`, `refilter`, `filter_set_impl` and `lfo_set_impl`, the six places that start, end or re-rate a generator today, so a live edit of the instrument cannot unseat the layer.
- **Evaluation is unchanged.** `mod_block` (flash) reads `g.kind` for the pan, finetune and position already; the tremolo's `on` is `g.kind == kFmodLfo` in place of `m.kind == kFmodLfo` (the same value in every existing path: `remod` and `mods_start` keep `g.kind` equal to the descriptor's). The cutoff is evaluated by the existing `filter_run` (SRAM_L), whose `m` pointer is the descriptor's modulator whatever its kind: the LFO reads `m.shape` / `m.lfo_amount`, which the descriptor holds for every slot (`og_samples.cpp put_mod`), and `g.l_ovr` (the tracker FX's waveform) stays the tracker's.
- **Depth and shape, the variant taken.** The brief asked for a triangle at "a mild default depth per destination". A per-generator amount would add a load and a select to `flfo_level` / `flfo_eval` (SRAM_L, +20 to +30 B) for every LFO; instead the Off / envelope case uses the **slot's stored LFO shape and amount** (the rule of the Filter LFO FX 35, OS-FU2), which is a triangle at 50 % in a new instrument (`kLfo` rows, `model/src/params.cpp`), so the depth is the instrument's own, editable on the instrument's LFO rows. **A slot that stores no audible amount** (an old project or an OG file whose Off / ADSR slot holds an LFO amount of 0, a negative one or a NaN: below `kPerfLfoMinAmount`) would make the row silent; for it the voice plays `kPerfLfoMod` (a triangle at 50 %) instead of the descriptor's modulator. `Voice::pmask` (bit d: destination d) is set by `perf_dest` and cleared at a note's start; `VoicePool::dmod` picks the descriptor's modulator or `kPerfLfoMod` in `mod_block` (pan, tune, position, the tremolo's level) and in `post_stages` for the cutoff (one byte test on the hot path, counted in the "as built" figures). The shared descriptor is never patched (tested). At the default 50 % the VOL LFO swings the level between 0 and 0.5 (the instrument's tremolo law): a peak of -6 dB, stated in the manual.
- **Export.** `offline_enter` clears the layer and `offline_leave` restores it (voices cut by `offline_quiet` in between), so an export with the rows held is byte for byte the export with Perform off (tested).

### 11.3 Budget [est, before the measurement]

- `.ramfunc_l`: **0 B or less** (two dispatch branches removed; target net zero for the pair of this slice and PLAYBACK's +40..80 B inside the 316 B headroom, D1). Any exception is recorded with the number here and in `audio/CLAUDE.md`.
- FLASH about +900 B (`perf_lfos`, the period helper, the `layer_event` and `remod` hooks); SRAM_U +90 B (`PerfSlots` 90 to 180 B, and the export's `PerfSave` copy in the engine) and `ControlTable::perf_val_` unchanged (the groups exist); SRAM_L 0; SDRAM_STATIC 0.
- **CPU, rows on [est]:** per voice-block, tremolo about 1.0 k cycles (eight level evaluations plus the 128 multiplies), cutoff about 1.1 k (eight `flfo_eval` + `svf_set_cutoff` and the chunked filter), finetune about 0.4 k (`repitch`), pan about 0.15 k, position small (wavetable / granular only): about 2.7 k cycles with all five, about 6 to 7 % of a 128-sample block at 180 MHz on twelve tracks (the figure of §8, item 6; **stale, OS-PERF-HPBP**: the block is 64 frames, hal/audio.h:25, about 261k cycles at 180 MHz, and the per-voice figures were derived for 128 samples (their per-sample parts halve at 64), so the share is somewhere between 6 and 12 % of a block on twelve voices; not re-derived, the measured rows are in cpu-budget.md). **Rows off: 0** (the `mmask` of a voice is what it was; the layer costs a byte test of nothing per block, since the reconcile runs at events, not per block). Accepted by the orchestrator (features are not cut for CPU). **Unit check to name:** Dense8 with the five rows held on twelve tracks, `diag cpu` against the same scene with the rows off, and by ear.

### 11.4 Tests

`tests/audio_perflfo_test.cpp` (ctest label `audio`), plus the extended `audio_model_ids_test`, `seq_perform_test`, `perform_screen_test` and goldens: rows on and off (the generator runs and ends, the voice is what it was after); the offset clamps at both ends of the table and the amp table (0..23); slot LFO (shape and amount stay, only the period moves) against Off against an envelope; sampler-only (a module / MIDI track is not moved); POS on a wavetable and a granular instrument and ignored on a plain one; held across FxReset and Stopped, and let go by a step ModSet / ModEnd; the layer over the step FX 35 and the step FX back; a live instrument edit (SOURCE, SPEED) does not unseat the layer; an export with all five held is byte for byte the export with Perform off, and the live note after it is the same as without an export; determinism (twice the same).

### 11.5 Decisions (decided by the orchestrator, 2026-10-25; new Community OS behaviour, parity an open research question)

| # | Question | Decision | Why |
|---|---|---|---|
| 1 | What the value means | a signed offset on the speed index of the track's LFO for that destination; 0 ends the FX | the screen's range (+-23 / +-28) is the index table |
| 2 | Slot is an LFO | shape and amount stay, the speed index moves, clamped | the least surprising edit of a running LFO |
| 3 | Slot is Off or an envelope | a Perform LFO at index `14 + o`, the slot's stored shape and amount; VOL on top of the envelope | the row is heard; the depth is the instrument's (variant, see 11.2) |
| 4 | Which voices | sampler voices; POS only on wavetable / granular | as TUNE; the other instruments have no such position |
| 5 | Clock | tempo-synced, runs while stopped | OS-LFOLIVE / OS-BUG-WTLFO |
| 6 | Step FX columns 23, 24, 33, 36 | stay without a consumer (follow-up) | Perform layer only |
| 7 | Export, FxReset, Stopped | the export is byte for byte Perform off; the layer survives FxReset and Stopped | §8, item 1 and 2 |
| 8 | CPU | zero with the rows off; about 7 % of a block with all five on twelve tracks, accepted | features are not cut for CPU |
| 9 | FLT LFO with no filter running | ignored (nothing to move) | the Filter LFO FX 35 rule; a held LOWPASS gives it a filter |
| 10 | Defaults of the three rows (§9) | each row: -8, -3, +3 (a slow sweep of 16 steps, a medium one of 3 steps, a fast one of 0.375 steps relative to the index in force); VOL LFO, PAN LFO, TUNE LFO, FLT LFO, POS LFO alike | none is 0 (0 would end the FX); the three are slow / medium / fast, all valid for +-23 |

### 11.6 As built (OS-PERF-LFO, 2026-10-25)

(Follow-up OS-PERF-CPU2, `docs/specs/OS-PERF-CPU2.md`: the reversed-voice fast path, bit-identical (a pan skip in `Engine::apply_pan_mods` was tried and dropped: no gain), the figures in `docs/architecture/cpu-budget.md`, QA-PRF-170..175.)

**Code.** `audio/include/o3os/audio/events.h` (`kModGroupLfo*`), `voices.h` / `voices.cpp` (`PerfSlots` of 10 slots, `perf_index` / `perf_dest` / `perf_sync` / `lfo_rerate` / `remod_k`, `Voice::pmask`, `kPerfLfoMod`, `dmod`; the hooks in `mods_start`, `layer_event`, `remod`, `lfo_live`, `refilter`, `filter_set_impl`, `lfo_set_impl`), `engine.cpp` (the two group-14 branches of `dispatch` removed: `layer_event` calls `lfo_set_impl` with the Filter LFO FX's own rules), `seq/perform.h` (rows 12..16 `live`), `model/project_state.h` (defaults -8 / -3 / +3).

**Risk review (coordinator, findings 1 to 8).** 1 a slot that stores no audible amount plays `kPerfLfoMod` (`pmask`, `dmod`; test "a slot that stores no audible amount"); 2 the FLT LFO sets `Voice::fmod` when a filter runs and its end goes through `refilter` / `lfo_set_impl` (test "the layer's end puts the cutoff back"); 3 the SRAM_U arithmetic is below; 4 the amp table's rate base is absolute (`perf_index`), the clamps of both tables and the three defaults (indices 6, 11, 17) are tested; 5 the layer's value is held to +-28 (`lim`); 6 the group 14 keeps the Filter LFO FX's rules (a ModSet starts the LFO only when `a` is 35, a ModEnd ends it; test); 7 the extra tests are in `tests/audio_perflfo_test.cpp`; 8 the manual row says that VOL LFO at the default 50 % peaks at -6 dB.

**Tests.** `audio_perflfo_test` (host and target): the layer is bit for bit the instrument's LFO at index 14 + offset (all six destinations, Off and ADSR slots, six offsets), over an LFO slot only the speed moves, the +-28 hold and the per-track slots, FxReset and Stopped, POS on wavetable and granular only, FLT only where a filter runs, the layer starting, re-rating and ending on a sounding voice, a retrigger, a steal, a gated track, FLT over the step FX 35, and an export with the five rows held that is byte for byte the export with Perform off (the export source sends the sequencer's FxReset and Step, which restart the LFOs' tick count: without them the export's phase is the live clock's, which differs by the blocks played before). The cutoff is probed by the filter's own cutoff, not by the rms of a noise table (that rms varies by itself). `audio_filter_mod_test` has the new FLT rule (the layer's FLT row alone runs the slot's LFO at index 14 + offset; the FX 35 alone keeps its own rate). Review fold: a live edit of a slot's LFO AMOUNT while the layer holds the destination runs `perf_dest` again on the sounding voices (`lfo_live`), so `Voice::pmask` follows the stored amount (an OG Off pan slot at amount 0 played the 50 % fallback until the next note; test "a live edit of the slot's amount" is bit for bit a note that had the amount); the SHAPE edit needs nothing (the fallback does not depend on it); the export test plays a note under the layer before the export is asked (the first export block holds that note's declick fade, the rest is byte for byte the export with Perform off).

**Measured (mk66 release image, `ci.sh mk66`, against 71ef26b7 built the same way).** FLASH +1,180 B (925,700 to 926,880), SRAM_L -56 B (49,608 to 49,552), `.ramfunc_l` **-52 B** (27,332 to 27,280; headroom 316 to 368 B: the two removed group-14 branches pay for the `dmod` test in `post_stages`, so D1 holds and `audio/CLAUDE.md` needs no exception), SRAM_U +512 B (134,880 to 135,392) and SDRAM_STATIC 0 (154,080). `PerfSlots` is 180 B (90 B more, and the engine's export copy `PerfSave` as much). **Attribution** (`arm-none-eabi-nm -S -n` on both `o3os.elf`, 71ef26b7 and this branch; `.bss` 130,784 to 131,296 B): the real growth is 184 B, `g_voices` +88 B (23,104 to 23,192: `PerfSlots` +90 B, less the tail padding) and `g_engine` +96 B (15,608 to 15,704: `PerfSave` +90 B and padding); nothing else changed size (the sum of all `.bss`/`.data` symbols: 309,799 to 309,983 B). The other 328 B are padding: `g_bdt` (the USB buffer descriptor table, 512 B, aligned to 512) sat 116 B above the end of `g_pipe` at 71ef26b7 (the slack the earlier growths of P3 and P7 had left), the 184 B overran it by 68 B, and `g_bdt` moved up one 512-byte step (0x2000f400 to 0x2000f600), so 444 B of padding now lie in front of it. The next growth of up to 444 B before `g_bdt` is free of SRAM_U; the figure still counts it as used. `voice_state`: `g_voices` + `g_amiga` = 23,192 + 5,376 = 28,568 B of the 28,736 B row (168 B left). The `voice_state` row of `mem_map.h` and the 154,368 B sum of `mk66_diag_report_test.cpp` are **not raised**: the `static_assert`s of `audio.cpp` hold.

**Hardware checks still open (not done by a host test).** (1) Dense8 with the five rows held on twelve tracks (`diag cpu`, arm LFO12 of `docs/architecture/cpu-budget.md`) against the same scene with them off: estimate 6 to 7 % of a block. (2) QA-PRF-160 to 162 by ear on the unit; the export check is two exports (rows held, Perform off) compared with `cmp`.
