# OS-BUG-MODDRIVE: Overdrive and Bit depth shape module instruments

Level C (the audio path). New Community OS behaviour (module instruments are this OS's own; the fix makes them follow the sampler's effects). Public issue #2, queued by the maintainer on 2026-10-29.

## Goal

Overdrive (step FX 38), Bit depth (step FX 39) and the Perform rows OVERDRV and BITDEPTH (ids 20 and 21) shape the sound of a module instrument track (TB-303, FM, Chip) exactly as they shape a sampler track. Today the drive and bit-depth pass runs only in the sampler voices (`post_stages`, audio/src/voices.cpp:810-814).

## Decisions (orchestrator, 2026-10-30; the maintainer is away: defaults, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| D1 | Where the shaping goes | After the module's render, so after its own filter. The sampler also shapes after its filter (voices.cpp:807-814): the same order |
| D2 | An instrument OVERDRIVE / BIT DEPTH row for module instruments | No. Module slots show only their own parameters (`rows_of`, app/src/screens/params_screen.cpp:190-197); the instrument value of a module track is neutral (0 / 16 bits). The manual says so |
| D3 | Does Stopped end a held 38 / 39 on a module track | No, the same as the sampler: the next FxReset ends it |
| D4 | Timing | Once per audio block, with the values in force at the end of the block (an event inside a block can act up to one block early, about 1.45 ms). Per segment would break the COLD call-rate rule (`hot.h`) |
| D5 | Does the module governor count the shaping | No. It lands in the governor's `base` (engine cycles less instance cycles), so admission stays conservative and a shaped track is not shed for it |

## Design

- **One curve.** `VoicePool::shape_pcm(io, n, drive_q11, crush, round)` (flash, `O3OS_AUDIO_COLD`) holds today's `shape_span` switch over `shape_loop<Drive, Crush, Round>` (voices.cpp:1965); `shape_span` becomes a thin wrapper. The module path is bit-identical to the sampler's by construction.
- **One resolution.** `VoicePool::shape_track(track, od_inst, bits_inst, ...)` holds `shape_set`'s rule (the Perform layer over the step FX over the instrument, voices.cpp:1939-1957); `shape_set` calls it too. A module track passes the neutral instrument values.
- **The values already exist per track for every track:** `Engine::dispatch` sends every ModSet / ModEnd to `voices_->fx_event` (engine.cpp:171-194): `ifx_val_` / `ifx_on_` (voices.h:1284-1285) and `perf_.val` / `perf_.on` (voices.h:827-829).
- **The hook.** In `Engine::play_block` (engine.cpp:333), after the module `end_block` (:393) and before the feed loop (:396-406): one inline gate test on `voices_->shape_mask()` only; zero means no call. Non-zero calls one flash function (`Engine::shape_modules`, COLD) that finds the module tracks itself and shapes each one's instance block in place (a mutable `ModuleVoices` accessor). `play_block` also serves the export (`render_offline`, engine.cpp:629), so an export matches playback.
- Out of scope: the Amiga voices' feed (`amiga_->feed`, engine.cpp:399; whether they shape today is an open research question), MIDI tracks (no audio), a module instrument row (D2).

## Design rules from the risk review (2026-10-30, folded)

1. **Neutral guard.** `shape_span` has none (voices.cpp:1985): with `sh == 0` it runs `shape_loop<true, false, false>`, and with a gain of 0 that zeroes the block. `shape_pcm` returns at once when drive and crush are both 0; a test sets the layer's BITDEPTH to 0 on a module track and gets the block unchanged.
2. **No new work in the HOT render.** No touched mask in `ModuleVoices::render` or `end_block`: the gate in `play_block` is `shape_mask()` alone (`ifx_on_[1] | ifx_on_[2] | perf_.on[od] | perf_.on[bits]`), and the track set is computed inside the COLD function.
3. **Track selection as `feed()` does it** (module_voices.h:237): `owner_[t]` with `in_[i].track == t` and the instance touched this block. A released instance is touched but unfed (`track == kNone`): skip it. The mapping is one to one, so no block is shaped twice.
4. **`.ramfunc_l` headroom from the build.** The figures in the docs disagree (264 B in audio/CLAUDE.md, 368 B recorded at OS-PERFFX; region at platform/mk66/mk66.ld.in:177). Read the real figure from the current mk66 build before and after; stop above +48 B.
5. **Named exceptions.** A per-block COLD call from the render breaks the rule of audio/include/o3os/audio/hot.h:25-29 and audio/CLAUDE.md ("no flash call on the per-block path"). `shape_modules` and `shape_pcm` are listed as exceptions where `shape_span` already is (voices.cpp:1916), with the call rate (once per block, plus once per shaped module track).
6. `shape_track` for a module track uses engine 0 and round 0; the Perform BITDEPTH value is the shift (16 − bits) (voices.cpp:1953).
7. FxReset clears `ifx_on_` (`clear_slide_fxs`, engine.cpp:213) and leaves the layer in `perf_`; the export saves, clears and restores `perf_` and does not touch `ifx_on_`. Test: a layer held live, an export, then back.
8. Docs: the cost lands in the governor's `base`, so it narrows admission for every module, not only the shaped track's; and D4's early action is up to one block on the tail of the previous note.
9. Optional, the engineer's call: skip a strip that is quiet (`graph_.quiet_tracks()`).

## Real-time rules and budget

- Bounded: 64 samples per block, at most `kMaxModuleInstances` (8) instances. No allocation, no new state in SRAM_L.
- `.ramfunc_l`: the gate test and one call in `play_block` only. Expected +20 to +40 B, a third exception to the net-zero rule (after P7 and P8), accepted by the orchestrator up to 48 B and recorded for the maintainer; anything larger stops for a decision. Everything else is in flash.
- CPU, estimated from the kernel's own note (voices.cpp:1962: about 17 cycles a sample for drive, 8 for depth, 20 for both): about 1.1k / 0.5k / 1.3k cycles a block per shaped module track (0.42 / 0.20 / 0.49 % of 261,120), at worst about 3.9 % with eight shaped module tracks. Zero when off. **Unit-gated:** the cost is measured on the unit before the merge (`docs/architecture/cpu-budget.md`).

## Tests

- tests/module_voices_test.cpp (the engine with a stub module, about :656): ModSet 38 / 39 on a module track; the strip feed equals `shape_pcm` over the unshaped block for drive 1, 50, 100 and bits 16, 12, 1; the layer wins over the step FX.
- Neutral: with no FX the block is byte-equal to today's output (tests/module_voices_test.cpp), and the gate is closed (`shape_mask() == 0`, tests/audio_perform_test.cpp).
- Equivalence: the same input and values give the sampler's `shape_loop` output (fixtures of tests/audio_voices_test.cpp:1464).
- Lifecycle: FxReset ends the shaping; the export's `perf_save` / `perf_clear` / `perf_restore` (voices.cpp:2063-2088) turn the layer off during an export and back after (model: tests/audio_perform_test.cpp:634-642); Stopped leaves a held FX (D3); an audition track is shaped; a track without a module is untouched.
- HIL: a CPU step after platform/mk66/tools/hil/cpu_play.json with a module pattern and 38 / 39 on 1, 4 and 8 module tracks against off (underrun 0, over-budget 0, the `cpu` figure); `bench voice_od|voice_bit|voice_odbit` (platform/mk66/src/bench.cpp:324) for the per-sample cost; `diag mod gov` to see the governor.

## Docs

docs/manual/module-instrument-fx.md (38 and 39 now work; D2), the QA rows (block `QA-MOD-200..209`), audio/CLAUDE.md (the hook and the `.ramfunc_l` exception), docs/architecture/cpu-budget.md (the measured figures).

## Done when

- Host tests above green; `tools/ci.sh` host, qemu (audio path) and mk66 green; flash, SRAM_L and `.ramfunc_l` deltas in the commit.
- The unit measurement recorded: 1, 4 and 8 shaped module tracks, underrun 0.

## Hardware-only checks

The CPU steps above; by ear, a TB-303 line with FX 38 at 30 and 100 and FX 39 at 8 and 4 bits, and the Perform OVERDRV and BITDEPTH rows on a module track.
