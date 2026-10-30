# OS-PERF-LFO-STEP: the step FX Volume, Panning, Finetune and Position LFO act

Level C (the audio path). New Community OS behaviour, modelled on this OS's step FX 35 (Filter LFO). Follow-up of OS-PERF-LFO (2026-10-25); queued by the maintainer on 2026-10-29.

## Goal

Step FX 23 (Volume LFO, 0..23), 24 (Panning LFO, 0..28), 33 (Finetune LFO, 0..28) and 36 (Position LFO, 0..28) start the matching LFO on the track's sampler voice, at the rate the value names, as the instrument's own LFO and the Perform layer's LFO rows do.

## Facts (design pass, 2026-10-30, dev 599c4b7c)

- The rows: seq/include/o3os/seq/fx.h:173, :174, :183, :186 (groups `LfoVolume`, `LfoPanning`, `LfoFinetune`, `LfoPosition`, :121-125); model/include/o3os/model/fx_table.h:72-85. Their identity (slot, name, range) is verified parity (docs/ui/official-manual-map.md:188, :189, :198, :201, "same"). **What they do in the official firmware is an open research question** (no QA row; only the position LFO's moving while stopped has one, QA-INS-087).
- The sequencer already emits them (`note_start_fx`, seq/src/sequencer.cpp:1801 -> `apply_columns` -> `emit_changes`): ModSet with `a` = the FX type, `b` = the value, `c` = the layer, `flags` = group 11, 12, 13 or 15; a ModEnd when a later note step drops the FX (control.h:8-14).
- Nothing consumes them: `Engine::dispatch` (audio/src/engine.cpp:198-201) -> `VoicePool::fx_event` -> `layer_event` (voices.cpp:2024), where a step event only clears the layer's slot.
- The machinery exists: the layer's LFO slots (`PerfSlots`, voices.h:815-830; `perf_slot_of` :839-846), the destinations `kLfoDest` (voices.cpp:2332), `perf_index` (:2338), `perf_dest` (:2356), `perf_sync` (:2390), `lfo_rerate` (:2523), the evaluation in `mod_block` (:2221) / `flfo_eval` (:1025). FX 35's own state is `fx_lfo_` (voices.h:1305).

## Decisions (orchestrator, 2026-10-30; the maintainer is away: defaults, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| D1 | The value | An absolute rate index of the LFO table, as FX 35 (VOL keeps the tremolo table's offset: 0..23 -> index 5..28) |
| D2 | The Perform layer and the step FX on one track | The layer wins while held and the step FX comes back when the layer ends (today's FX 35 behaviour, tests/audio_perflfo_test.cpp:482; the control table already says so) |
| D3 | FxReset / Stopped / export | FxReset ends them; Stopped leaves them (the same as 38 / 39 and FX 35, OS-BUG-MODDRIVE D3): a live pad note on that track after the stop still gets the LFO until the next FxReset (documented in the manual); an export puts them away and back (`perf_clear` / `perf_restore`, as `step_rev_`), not relying on its FxReset |
| D4 | Step FX 24 and a Panning FX 31 on the same track | The LFO wins while it runs (whichever came first), the same as the Perform layer's PAN LFO today (OS-PERF-CPU2 rule 1): while the LFO runs it sets the strip's pan every block around the instrument's pan, so FX 31 is overridden; when FX 24 ends the instrument's pan comes back and FX 31 acts again only at its next ModSet. Pinned by tests and documented |
| D5 | Module and MIDI tracks | Ignore them (sampler voices only, as the layer's LFO rows) |
| D6 | The editor | No rate name in the step popup (unchanged) |

## Design (all flash)

1. voices.h: `std::uint8_t step_lfo_[4][kVoiceTracks]` (rate + 1; VOL, PAN, TUNE, POS; FLT keeps `fx_lfo_`) in SRAM_U (`voice_state`, mem_map.h:205). Constants `kFxLfoVolume` = 23, `kFxLfoPanning` = 24, `kFxLfoFinetune` = 33, `kFxLfoPosition` = 36 beside events.h:199, held by `audio_model_ids_test`.
2. `layer_event` (voices.cpp:2057-2066): for groups 11, 12, 13 and 15, store the step rate from `e.a` / `e.b` (whatever `e.c`: `a` and `b` carry the FX under a winning layer) before the existing `perf_sync` call (:2061). A ModEnd clears it. The value is clamped to its range (FX bytes are untrusted).
3. `perf_index` (:2338): when the layer does not hold the row and the step rate is non-zero, the index is `rate - 1 + rate0`, clamped.
4. `mods_start` (:1717): a held step rate counts as held, so a new note starts the LFO.
5. FxReset: clear the state in the all-tracks branch of `lfo_set_impl` (:1130), which `clear_filter_fxs` already calls (engine.cpp:216), and `perf_sync` the sounding voices, as `filter_apply` does for FX 35.

## Design rules from the risk review (2026-10-30, folded)

1. **D4** as above (the LFO wins while it runs; review 2026-10-30: "last writer wins" was wrong when FX 31 comes after FX 24); tests for both ends: FX 24 starting over a held FX 31, and FX 24 ending while FX 31 is held.
2. **Store only the step FX's own event.** In `layer_event`, store the rate only when the event sets and `e.a` is that row's FX (`kFxLfoVolume` and so on), as FX 35 does (voices.cpp:2060); a step ModSet whose `a` is another FX (`a == 0`: the layer only) or a ModEnd clears it.
3. **The layer at 0.** `perf_index` (voices.cpp:2341) returns -1 when the layer holds the slot with value 0: fall through to the step rate in that case too, not only when the layer does not hold the row.
4. **`perf_lfo_held`** (voices.h:1236) gates the `mods_start` loop: add the step state there. It is inlined only into `mods_start` (flash); keep it out of `.ramfunc_l`.
5. **FxReset inside `lfo_set_impl`.** `clear_filter_fxs` is inline in `Engine::dispatch` (SRAM_L): add nothing to that wrapper. Clear `step_lfo_` and `perf_sync` the sounding voices inside `lfo_set_impl`'s all-tracks branch (voices.cpp:1130-1160), before its `continue`s.
6. **The export** puts `step_lfo_` away and back in `perf_clear` / `perf_restore` (voices.cpp:2090-2102), as it does `step_rev_`; the export-`cmp` test holds a step FX across the export.
7. **SRAM_U from the build.** The 168 B slack figure is stale (P5 and P7 grew `PerfSlots`), and +64 B can cross another 512 B alignment step of `voice_state` (as OS-PERF-LFO did): read `nm` / the map before and after and record the real delta.
8. **CPU gate, set now.** The `cpu` arm (the four step FX held on twelve tracks, the one-shot scene) costs no more than the same layer rows held (within the paired noise, ±1.2 pp), underrun 0 on the one-shot scene. The rows' own cost is OS-PERF-CPU2's subject (which touches the same PAN code: this item merges after it and takes its changes first).
9. Tests also: the VOL clamp at 23; POS on a plain sample is a no-op (the guard at voices.cpp:2346).

## Real-time rules and budget

- No new per-block code: the evaluation is the layer's. With no step FX held the cost is zero (one byte test at a note start). Held, a row costs what the layer's row costs, measured on the unit on 2026-10-30 (docs/architecture/cpu-budget.md, the fqa.15 correction): VOL +4.1 pp, TUNE +7.3, FLT +6.2, POS +13.0 (on a plain sample, where it moves nothing), PAN +4.8; LFO12 (all five) +28 pp. The earlier figures of this spec (VOL +0.5, PAN +4.3, TUNE +0.4, POS +0.3) were wrong. A step FX 36 on a plain sample starts no modulator and sets no `mmask` bit (`perf_index` returns -1 at the guard, `step lfo: the FX 36 on a plain sample`): it costs only the step state's byte test at a note start, not the layer's +13 pp. The CPU gate (rule 8) stays relative: a step row costs no more than the same layer row.
- `.ramfunc_l`: 0 B expected (all edits in flash functions); read the map before and after; any growth stops for a decision. FLASH about +250 to +400 B [est]. SRAM_U about +64 B [est]: watch the `voice_state` alignment step (about 168 B of slack) and report the real figure.

## Tests

- tests/audio_perflfo_test.cpp (a new section): each of 23, 24, 33, 36 is bit-identical to the instrument's LFO at the same index; the VOL offset; the clamp; POS on wavetable and granular only; FxReset ends, Stopped keeps; the layer wins and the step FX returns; a live SPEED edit; an export byte-identical with a step FX held; D4.
- tests/audio_model_ids_test.cpp: the four constants. A sequencer-to-voice test (the ModSet from a pattern reaches `step_lfo_`).
- HIL: `qa_lfostep.json` (sound, the export `cmp`, a `cpu` arm); QA rows `QA-PRF-180..189`.

## Docs

The manual's step FX list (the four FX act now; D1, D2), audio/CLAUDE.md, docs/architecture/cpu-budget.md (the cost is the layer's rows).

## Done when

Host tests green; `tools/ci.sh` host, qemu and mk66 green; flash, SRAM_U and `.ramfunc_l` deltas in the commit; the HIL rows written. Hardware: the sound of each FX and the export `cmp` on the unit.

## Hardware-only checks

By ear on the unit: each FX on a held note at a slow and a fast rate; the layer over the step FX; the export `cmp`. CPU: the `cpu` arm with the four held on twelve tracks against off.
