# OS-INSFX-ABI2 — Effect ABI 1.11: stereo effects, the clock, an audio sidechain key, factory presets

Status: specified 2026-11-05, decisions taken the same day (every recommended default), not implemented · **Level C** (a published ABI, the audio path, the render context) · Backlog row OS-INSFX-ABI2, approved by the maintainer 2026-11-05 (the sidechain by audio) · Facts from `dev` at c1dc3125 and the open branches `feat/OS-INSFX-S5` (f907cded, the insert parameter page) and `feat/OS-INSFX-FX1-drive` (4bfa21b9) · Order: after OS-INSFX S1 to S5 are on `dev`; slice A7 also after OS-INSFX S6 (the master insert) · Everything here is **new Community OS behaviour**: the official firmware has no insert effects and no user sidechain (F1).

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 b, Q3 a, Q4 a, Q5 a, Q6 a, Q7 a (every recommended default).**

Only what the maintainer must decide (a published ABI's shape, a CPU budget, scope). Every technical detail is decided in this spec and listed under Assumptions (section 6).

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | The planes of the stereo render | a) left and right: `render_st(state, in_l, in_r, out_l, out_r, frames)`; the OS converts the track's mid M and side S to L / R and back · b) mid and side planes | a | Every published DSP recipe and every author thinks in L / R; the master is L / R already; the conversion folds into the Q15 loops the adapter runs anyway (about 1 cycle a sample) |
| Q2 | Stereo shapes | a) one: a stereo effect always runs stereo, every stereo track insert holds one of the four stereo slots · b) two: **WIDENS** (makes S from a mono input: always stereo, holds a slot) and **stereo-capable** (runs `render_st` only on blocks where the track brings S, its mono `render` otherwise, holds no slot) | b | A compressor, filter or drive that keeps a stereo voice's S should not cost a stereo slot or the S work on a mono track; only effects that create width need the slot |
| Q3 | Where the key is measured | a) on the key track's mono block after its inserts, **before the fader**: a muted or faded key track still keys (a "ghost kick"; its voices keep rendering while it is a key); measured by a tap in insert slot B (flash, SRAM_L +0 B) · b) the meters' post-fader figure, as the row's lead says: a muted key keys nothing; a store in the hot strip pass (SRAM_L about +20 B) | a | The ghost key is the common sidechain use; a is free in SRAM_L, b eats the last hot-code bytes |
| Q4 | How a stereo insert counts against the stereo cap (OS-STEREO `kStMax` 4) | a) each un-bypassed WIDENS track insert reserves one of the four side blocks; new stereo notes beyond the rest play M, notes already sounding keep S; the master insert reserves none · b) the same, but at most 2 reservations (a third WIDENS insert is refused `STEREO FULL`) · c) no reservation; the insert governor alone | a | What the row asks, with the user in control: four widening inserts mean mono samples, said in the manual and on the page |
| Q5 | Where the clock's beat counts from | a) step 0 of the pattern that plays (bar-aligned whatever the Play position; it restarts with each pattern, flag `WRAPPED`) · b) the run's start (monotonic, misaligned after a Play from the middle) | a | A tracker's bar is the pattern grid; a synced autopan or LFO lands on the downbeat |
| Q6 | Factory presets on the unit in this item | a) an F5 PRESET list on a module's parameter page (instrument or insert) with the factory presets now; OS-MODPRESET adds the card's user presets to the same list · b) console only until OS-MODPRESET | a | The FX2 effects ship with presets and are usable at once; MODPRESET extends one list instead of building a second |
| Q7 | CPU gate of the stereo track adapter (M and S to L / R, back, the S pass) | a) accept up to 6k cycles a block (2.3 %) measured, before the effect's own render; above it the slice stops for a decision · b) require at most 3k (about a stereo voice, as the row estimates), which needs a fused SRAM_L kernel (+300 to 500 B of hot code, over the budget) | a | The mono adapter measured 2.24k cycles (1.7x its estimate); a stereo one is about twice that plus the S pass (about 1.0k): 4.5k to 5.5k [est]. b has no SRAM_L room |

## 1. Goal

Give effect modules the four things the next effect packs need, as one additive ABI minor that every module of 1.0 to 1.10 still loads under: (1) **stereo**: an effect declares a stereo shape; on a track it takes the mono block M (and the side S a stereo voice or an earlier stereo insert brings) and writes M plus a side S into the lane stereo voices already use (`Graph::side_pass`), so a mono source can come out wide and a stereo voice keeps its S; on the master one stereo instance replaces the dual mono pair; (2) **the clock**: the tempo in force and the beat position, for synced delays and LFOs (instruments get it too); (3) **an audio sidechain key**: an effect reads another track's level every block, at most one block (1.45 ms) late; the key track is a module parameter, so it is stored in the `INSX` blob with no project format change; (4) **factory presets**: the descriptor lists named parameter sets; the OS applies one through `set_param` and writes the blob.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | The official firmware has no insert effects and no user sidechain: its only sidechain is the output limiter's own detector input | `tracker-ce@7fd6c2c:core/AudioEngine/AudioOutput.cpp:5-20` (a search of `core/` for sidechain or ducking finds nothing else) | verified (tracker-ce source); no parity claim |
| F2 | ABI rule: a module loads when its major equals the OS's and its minor is not above; an additive change bumps the minor; the current minor is 10; the descriptor's size is per minor (`O3M_MODULE_SIZE`: 64 bytes to 1.7, 80 from 1.8) and members past a minor's size are not read | `module/sdk/o3module.h:42-43`, `:98-99`, `:748-783`; `module/include/o3os/module/image.h:139-156` (`kPitchAbiMinor`, `DescExt32`, `desc_size`) | code fact |
| F3 | The host table `o3m_host` is 52 bytes (`io` at 48), members only appended, found with `O3M_HOST_HAS`; one table, `ModuleVoices::host_`, serves instruments and the inserts | `o3module.h:694-715`, `:861-862`; `audio/src/module_voices.cpp:97-106` | code fact |
| F4 | The loader requires `render`, `set_param`, `note_on`, `note_off` for an effect; checks the 1.8 tail (pages, labels) only when the module declares 1.8 | `module/src/image.cpp:240-309` (`read_desc`), `module/src/loader.cpp:121-128` (`add_native`) | code fact |
| F5 | The strip pass: the source block (mono Q15) goes through INS A then INS B in place (`InsertFn(state, io, n)`), then the sends, then the fader; the track's S pointer `side_[t]` is read **after** the inserts and handed to `side_pass`, which adds S through the strip's level and pan gains into MAIN (L += S, R -= S) at once, before the next track's inserts run | `audio/src/graph.cpp:899-904`, `:928-933`, `:961-982`; `audio/include/o3os/audio/graph.h:136` | code fact |
| F6 | `side_pass` scales S by `255 - wet` of each un-bypassed insert (a mono insert drops S, OS-INSFX D6); `Graph::Insert` is 12 bytes with no padding left (`fn`, `state`, `bypass`, `tail`, `wet0`, `wet1`) | `graph.cpp:961-982`, `graph.h:389-400` | code fact |
| F7 | `Engine::feed_sides` (flash) writes `side_[t]` for the tracks with voice S now or in the last block; `play_block` (SRAM_L) calls it only when `side_mask() | side_fed_` is non-zero | `audio/src/engine.cpp:78-90`, `:476-477`; `engine.h:179-181` | code fact |
| F8 | The insert adapter `InsertHost::run` (flash): q15 to float into `Graph::insert_scratch(0)`, `render` in place, back by the module rule (NaN 0, saturation counted), a one-block dry / wet ramp in a transition, an idle skip, the S2 tail; the Graph has two 64-float scratch planes | `audio/src/insert_host.cpp:753-810`; `graph.h:186`, `:588` | code fact |
| F9 | Measured on the unit (OS-INSFX S8): `insert_effect` 1,666 cycles, `insert` 3,905, the mono adapter about 2,240 (0.8 % of the 261,225-cycle block; estimate was 1.2k) | `docs/specs/OS-INSFX.md` S8 as built | measured (unit) |
| F10 | Stereo voices: at most 4 side blocks (`kStMax`); `st_start` takes a free one at a note start; measured +3.87 pp at four stereo voices, accepted by the maintainer; S reaches MAIN only (sends, CUE, REC, USB track stems and inserts read M) | `audio/include/o3os/audio/voices.h:1319`; `audio/src/voices.cpp:618-640`; `docs/specs/OS-STEREO.md:64`, `:181`, `:190` | code fact; measured |
| F11 | The tempo in force (what plays, half and double time included) reaches the engine every block through `EventSource::tempo`; voices and module instruments get `set_tempo_x100`; the send delay gets it when it changes. The sequencer keeps `step_start_` (Q32 samples) and 48 microsteps a step, 4 steps a beat | `audio/src/engine.cpp:402-411`; `audio/include/o3os/audio/event_source.h:30-46`; `seq/include/o3os/seq/sequencer.h:12-16`, `:348-349`, `:581` | code fact |
| F12 | Track meters are post-fader (what the StemTap carries); a channel is a running peak and sum of squares over a 22-block window; no per-block figure is kept | `audio/include/o3os/audio/meters.h:6-21`; `graph.cpp:1049-1053` (`finish_track`) | code fact |
| F13 | OS-ODD11 gates the voices of a strip whose level and both sends are closed; `Engine::gate_off_` (only tests set it today) exempts tracks | `engine.cpp:419`; `engine.h:116`, `:215` | code fact |
| F14 | A module's parameters are at most 32 (`O3M_PARAMS_MAX`), stored as a float blob in order; a module has at most 4 pages (`O3M_PAGES_MAX`), so F5 on a module's parameter page names no page; the graph drains at most 64 ParamChanges a block | `o3module.h:144-147`; `app/src/instrument_actions.cpp:54-63`; `core/include/o3os/core/param.h:69` | code fact |
| F15 | SRAM: `.ramfunc_l` is at its budget's edge (exceptions recorded one by one); the SRAM_U margin is 1,968 B; the module state arena (4 KiB SRAM_U) has about 640 B left, then state goes to SDRAM; the module region in SDRAM is 768 KiB; an uncached SDRAM access is about 0.25 to 0.33 % of a block per access a frame | `audio/CLAUDE.md` (OS-INSFX S2, OS-PERF-CPU2 lines); `docs/architecture/cpu-budget.md:431`, `:481`; `platform/mk66/include/o3os/platform/mk66/mem_map.h:103` | code fact; measured |

## 3. Design

### 3.1 The ABI surface (1.11)

`O3M_ABI_MINOR` 10 -> 11. Four members appended to the descriptor (`struct_size` 96 for a 1.11 module, `O3M_MODULE_SIZE(11)`; read only when the module declares 1.11):

| Offset | Member | Meaning |
|---|---|---|
| 80 | `uint32_t (*render_st)(void* state, const float* in_l, const float* in_r, float* out_l, float* out_r, uint32_t frames)` | NULL: a mono effect. Non-NULL (effects only): the stereo render; `in_*` and `out_*` may be the same buffers; return value as `render` (1 while it may sound, 0 when silent and idle); frames 1..64 |
| 84 | `uint32_t fx_flags` | `O3M_FX_WIDENS` (1): the effect makes a stereo image from a mono input; the OS calls `render_st` on every block. Without it, a module with `render_st` is **stereo-capable**: `render_st` on a block whose input has S, `render` otherwise, one state for both (the module makes the switch click-free). Other bits 0. WIDENS needs `render_st` |
| 88 | `const o3m_preset* presets` | the factory presets, NULL when none |
| 92 | `uint32_t preset_count` | 0..`O3M_PRESETS_MAX` (32), zero exactly when `presets` is NULL |

`o3m_preset { const char* name; const float* values; uint32_t count; }` (12 bytes): `name` 1..15 printable ASCII (`O3M_PRESET_NAME_MAX` 16), `values[count]` in parameter order, `count` 1..`param_count`, each finite and within its parameter's range; **a preset sets every parameter**: those past `count` take their default. Any kind may have presets.

A parameter flag `O3M_PARAM_KEY` (32): an INT parameter, min 0, max 16, `labels` entry NULL: 0 = no key, 1..16 = the key track. Effects only, at most two a module; not learnable (the OS treats it as `NO_LEARN`); the OS shows it as `OFF`, `T1`..`T16`. A module of 1.10 or below never has it honoured.

Two members appended to `o3m_host` (size 52 -> 60), for every kind:

- 52: `int32_t (*get_clock)(o3m_clock* out)` — `o3m_clock { uint32_t size; uint32_t flags; uint32_t tempo_x100; float beat; }` (16 bytes, `out->size` set by the caller as in `get_transport`). `flags`: `O3M_CLOCK_PLAYING` 1, `O3M_CLOCK_STARTED` 2 (a run started in this block), `O3M_CLOCK_WRAPPED` 4 (the beat went back: a new pattern or a jump). `tempo_x100`: what plays (half and double time included); the project tempo while stopped. `beat`: the playhead at this block's first sample, in beats (4 steps) from step 0 of the pattern that plays, the fraction inside a microstep interpolated (piecewise linear under swing); 0 while stopped. Values are those of the current block; returns 0, or 1 when the OS has no sequencer.
- 56: `float (*track_level)(uint32_t track)` — track 0..15: the linear peak (0..1) of that track's mono block after its inserts, before the fader (Q3 a), of this block when that track's strip already ran in it, else of the previous block; 0 for a silent track, a bad index, or a track no bound insert's KEY parameter names. Render context.

Compatibility: every module of 1.0 to 1.10 loads and runs bit for bit as before (its descriptor is 64 or 80 bytes; the new members are never read; the host table only grew). An OS of 1.10 refuses a 1.11 module (`NEEDS NEWER OS`, the minor rule): a project whose insert names one still opens there and plays it dry (the OS-INSFX S3b skip rule; the `INSX` record and the blob are kept). A module that can live without the clock or the key may declare a lower minor and test `O3M_HOST_HAS`; one that needs them, or has `render_st`, presets or a KEY parameter, declares 1.11. `O3M_ABI_ASSERT` lines: `module_1_11` (render_st 80, fx_flags 84, presets 88, preset_count 92, size 96), `host_1_11` (get_clock 52, track_level 56, size 60), `preset_pod` (12), `clock_pod` (16).

### 3.2 Stereo on a track (the stereo adapter)

`InsertHost::run` keeps the `InsertFn` signature; the instance knows its shape. For a stereo instance (WIDENS, or stereo-capable on a block whose input has S):

1. **Input S.** `Graph::side(track)` (the voice's S, or an earlier stereo insert's), scaled by `(255 - wet) / 255` of an un-bypassed mono insert before it in the same strip (ramped over the block from `wet0` to `wet1`: a steady mono insert gives 0, so D6 holds). Null or a zero gain: no S (a stereo-capable effect then runs `render`, the mono path, unchanged).
2. **In:** one loop: `L = (M + S) / 32768`, `R = (M - S) / 32768` into the Graph's two scratch planes (exact: a 17-bit integer times 2^-15).
3. `render_st(state, L, R, L, R, n)` between the platform's enter / leave stamps.
4. **Transition ramp** (bind, bypass, forced bypass, clear): the dry pair is recomputed from `io` and the input S (both still intact), blended per sample; no extra plane.
5. **Out:** one loop: `M' = (L + R) / 2`, `S' = (L - R) / 2`, each to Q15 by the module rule (NaN 0, saturation counted) into `io` and the host's one **shared S block** (64 x int16, SRAM_U). Shared is safe: `side_pass` consumes a track's S before the next track's inserts run (F5); a second stereo insert on the same track reads it as its input and overwrites it after converting.
6. `Graph::feed_side(track, s_block)`, so the strip's `side_pass` adds S' to MAIN.
7. **`side_pass` rule** (flash): walking the slots in order, a stereo insert resets the S gain to unity (S is its output), a mono one multiplies it by `255 - wet` as today. The stereo bit lives in a new `Graph` mask (`st_ins_`, one bit per slot, written by the render at a block boundary; `Strip` and `Insert` do not grow).
8. **No stale S:** `InsertHost::begin_block` (flash, after the block) sets to null every `side_[t]` that holds the host's S block; `feed_sides` keeps writing the voices' S as today. `play_block` is untouched (SRAM_L +0 B).

Idle skip: the input is silent when M and S are. Tail (S2): the zero-output count needs M' and S' both zero. Outputs other than MAIN stay mono: the sends, CUE, REC and the USB track stems carry M' (OS-STEREO decision 4, F10). Cost [est, F9]: about 4.5k to 5.5k cycles a block for the adapter and the S pass (1.7 to 2.1 %) before the effect's own render; Q7 sets the gate. `diag bench insert` gains `insert_st` (the stereo adapter around an identity) and `insert_st_effect`.

**The cap (Q4 a).** `InsertHost::begin_block` counts the WIDENS track inserts that run (phases In, On and the two ramps out; not Bypassed or Off) and calls `VoicePool::set_st_reserved(n)` when it changes (flash): `st_start` then sees `kStMax - n` side blocks; voices that hold a block keep it to the end of their note. The INSERTS cell and the picker line show `ST` for a stereo effect; `ins` lists the reservation.

### 3.3 Stereo on the master (after OS-INSFX S6)

A stereo effect (either shape) on the master is **one** instance: `render_st(l, r, l, r, n)` in place on the float planes, the S6 scrub of NaN and infinity after it, counting 1 of the insert cap; a mono effect stays dual mono (2 instances, S6). No reservation (the master adds no S path). OS-INSFX D4 becomes "dual mono for a mono effect, one stereo instance for a stereo one".

### 3.4 The clock

`EventSource` gains `ClockFn` (the sequencer's `clock_at(sample)`: flags and the beat as Q16.16, from `step_start_`, the step length and the microstep, in flash). `Engine::block_end` (flash) writes a `module::ClockState` (16 bytes, SRAM_U) for the next block: `tempo_x100`, beat, flags; `render_offline` writes it at the start of each export block. `get_clock` reads it. Instruments get it too (OS-SNDMOD's "a synced LFO needs an ABI minor").

### 3.5 The sidechain key

- **Which tracks are keys.** At bind (the blob) and on every `ParamChange{Insert}` to a KEY parameter, the host records the instance's key tracks; `begin_block` folds them into `key_mask` (16 bits), published for the console.
- **Measuring (Q3 a).** For a keyed track, insert slot B measures: an empty slot B gets the host's **key tap** (a flash `InsertFn` that writes the block's peak and changes nothing); a module in slot B measures its own output in `run`. When a module leaves slot B of a keyed track the tap goes back; when the track stops being a key the tap is cleared. The tap is not an instance (no cap, no governor; about 0.15k cycles a block, in `base`).
- **The word.** `key_[t] = (block & 0xFFFF) << 16 | peak` (peak 0..32768, one store); `track_level(t)` takes it when its block stamp is this block or the previous one, else 0. Strips run in track order, so a key on a lower track is this block's, on a higher one (or the same) the previous block's: at most one block late; the master insert always reads this block's.
- **Ghost keys.** `Engine::block_end` adds `key_mask` to the tracks the voice gate may not act on (`gate_off_`, flash), so a muted key track's voices still render and its tap still measures; documented CPU cost.
- **Export.** `prepare_export` clears the key words, so two exports give the same bits.
- **Persistence.** The key is the parameter's value in the `INSX` blob: no format change. A project whose key names a track that makes no audio reads 0.

### 3.6 Factory presets

`app::ParamSet` (`app/include/o3os/app/param_set.h`, pure: a name, a source Factory or Card, a 32-bit mask of the values given, 32 floats in the module's order) is the one parameter-set model; OS-MODPRESET reuses it. `ModuleUi::factory_presets(target, out[])` builds them from the descriptor; `ModuleUi::apply_preset(target, set)` writes every parameter (a masked value clamped to the descriptor and rounded for INT, else the default): on an **instrument** through `write_module_param` inside one `begin_group` / `end_group` (one undo unit); on an **insert** into the `INSX` blob through `Editor::set_insert` and `InsertHost::set_param` for each (no undo, OS-INSFX D9). At most 32 ParamChanges: one block's drain (F14). The UI (Q6 a): **F5 PRESET** on a module's parameter page (instrument or insert) opens a list overlay (Up / Down / encoder, F1 LOAD, F8 BACK) with the factory presets; a module with none says `NO PRESETS`. Toast `PRESET <NAME> LOADED`.

## 4. Slices

| # | Slice | Size | Level | Depends on | Flash [est] | SRAM [est] |
|---|---|---|---|---|---|---|
| A1 | ABI 1.11 header, loader and image checks, host members as stubs | S | C | OS-INSFX S1-S5 on dev | +0.6 KB | 0 |
| A2 | Generator (`module.json`: `stereo`, `presets`, KEY type), stereo template, SDK docs | S | B | A1 | 0 (tools, card) | 0 |
| A3 | The clock: sequencer `clock_at`, `ClockState`, `get_clock` live | S | C | A1 | +0.4 KB | SRAM_U +16 B |
| A4 | Stereo track adapter, shared S block, `side_pass` rule, stale-S clear, bench rows | S | C | A1, A2 | +1.2 KB | SRAM_U +140 B, SRAM_L 0 |
| A5 | The stereo cap reservation, `ST` marks, QA | S | C | A4 | +0.3 KB | SRAM_U +4 B |
| A6 | Sidechain key: KEY tracking, tap, key words, `track_level` live, gate exemption, KEY display, key template | S | C | A1, A2 | +1.0 KB | SRAM_U +72 B |
| A7 | Master stereo insert (one instance) | S | C | A4, OS-INSFX S6 | +0.4 KB | 0 |
| A8 | `ParamSet`, `factory_presets`, `apply_preset`, console `mod preset` / `ins preset` | S | B | A1 | +0.8 KB | 0 |
| A9 | F5 PRESET overlay (factory list) on the module parameter pages | S | B | A8 | +1.2 KB | 0 |

Totals [est]: flash about +5.9 KB, SRAM_U about +232 B (margin 1,968 B, F15), SRAM_L +0 B in every slice (any growth stops the slice for a decision, as OS-INSFX D8); render CPU: nothing for a project without a 1.11 effect; a stereo track insert per Q7; a key about 0.15k cycles a block; the clock about 0.1k a block.

**A1 — ABI 1.11 surface** (S, Level C). Files: `module/sdk/o3module.h` (3.1: members, flags, PODs, `O3M_MODULE_SIZE(11)` = 96, the 1.11 history entry, `O3M_ABI_ASSERT`s), `module/include/o3os/module/image.h` (`kStereoAbiMinor` 11, `DescExt32` grows, `desc_size`), `module/src/image.cpp` (`read_desc`: `render_st` code, `fx_flags` known bits and WIDENS needs `render_st`, `render_st` and KEY only for an effect, KEY's range and NULL label, at most two KEYs, the preset table: alignment, count, names, values finite and in range), `module/src/loader.cpp` (`add_native` the same rules), `audio/src/module_voices.cpp` (`host_.size` 60, stubs: `get_clock` fills zeros and the project tempo 12000, `track_level` 0). Acceptance: a 1.10 module and every built-in loads and runs as before (the existing module tests unchanged); a 1.11 module with each feature loads; each bad field is refused `BadDescriptor`. Tests: `tests/module_image_test.cpp` (layout of 1.11, every refusal, a 1.10 file with a non-zero byte past 80 still loads since it is not read), `tests/module_loader_test.cpp` (`add_native` rules), `tests/module_edit_test.cpp` (64-bit host offsets of the new members). QA rows: none (no UI). Unit check: `mod load` of the biquad (1.2) and a built-in after the image change; `diag mod` unchanged.

**A2 — SDK tooling and the stereo template** (S, Level B). Files: `tools/o3m_gen.py` (`"stereo": "widens" | "capable"`, `"presets": [{"name", "values": {param: value}}]` validated against the params, `"type": "key"`), `tests/module_gen_test.cpp` and `tests/o3m_gen_scaffold_test.py` cases, `modules/templates/effect_stereo/` (`example_stereo.cpp`, `module.json`: a WIDENS autopan-lite, Rate in beats or Hz, Depth, synced through `get_clock` with a 120 BPM fallback, two factory presets), `modules/CMakeLists.txt` (builds the template's `.tmod`), `module/sdk/README.md` (section 3 render and shapes, a new "Effects: stereo, clock, key, presets" section, section 8 the 1.11 row), `docs/guides/module-sdk.md`, `module/CLAUDE.md`. Acceptance: the generator writes the 1.11 descriptor tail; the template packs and loads; a manifest preset naming an unknown parameter or out of range fails generation. QA rows: none.

**A3 — The clock** (S, Level C: the render's per-block path). Files: `audio/include/o3os/audio/event_source.h` (`ClockFn`), `seq/include/o3os/seq/sequencer.h`, `seq/src/sequencer.cpp` (`clock_at`), the platform's and the offline sequencer's `EventSource` wiring, `audio/src/engine.cpp` (`block_end`, `render_offline`), `module/` or `audio/` `ClockState` and the real `get_clock`. Tests: `tests/seq_clock_test.cpp` (beat at every step boundary equals row / 4 exactly; within a step linear to 1e-4 beat; swing pairs; half and double time; Tempo FX; WRAPPED at the pattern's end and at a jump; STARTED once; stopped gives the project tempo and beat 0), `tests/audio_insert_test.cpp` (a clock probe effect reads the block's values; two exports equal). Unit check: the A2 template synced at 1/4 on a 120 BPM song pans on the beat; change the tempo, it follows.

**A4 — Stereo track adapter** (S, Level C). Files: `audio/include/o3os/audio/insert_host.h`, `audio/src/insert_host.cpp` (3.2 steps 1 to 8, shape per instance, idle and tail with S), `audio/include/o3os/audio/graph.h`, `audio/src/graph.cpp` (`st_ins_` mask and its flash setter, `side(track)` accessor, `side_pass` rule 7), `platform/mk66/src/app_main.cpp` (`diag bench insert` rows `insert_st`, `insert_st_effect`). Acceptance: 3.2; with no stereo effect bound every render is bit for bit today's. Tests (`tests/audio_insert_test.cpp`, new `stereo:` cases, native test effects `st_identity` (capable) and `st_widen` (WIDENS, S = M / 2)): identity on a stereo voice keeps M and S bit for bit; identity on a mono track adds no S; `st_widen` on a mono track gives L = M + S, R = M - S at MAIN; a mono insert before or after a stereo one (D6 both ways, ramps); bind and bypass ramps of S monotonic and one block long; no stale S after unbind or an idle strip; two stereo inserts on one track; two stereo tracks in one block; idle skip; tail; counting allocator in `run`; export equal bits; `tests/audio_stereo_test.cpp` unchanged. QA rows: `docs/qa/catalogue/mixer.md` (a stereo insert on a mono and a stereo track, unit rows). Unit check: `diag bench insert` (`insert_st` against Q7's gate), the template on a mono track sounds wide, Dense8 with one stereo insert: 0 underruns, `diag cpu`.

**A5 — The cap** (S, Level C). Files: `audio/include/o3os/audio/voices.h`, `audio/src/voices.cpp` (`set_st_reserved`, `st_start` skips reserved blocks), `insert_host.*` (the count, `attach` takes the `VoicePool`), `app/src/screens/mixer_screen.cpp` and the picker line (`ST`), console `ins` (the reservation). Tests: `tests/audio_insert_test.cpp` (`cap:` two WIDENS inserts leave two stereo voices, the third note plays M, a note that holds a block keeps it, bypass returns the slot, a capable insert reserves none, the master none), `tests/mixer_inserts_test.cpp` (`ST` mark), sim `qa_inserts.txt` golden. QA rows: mixer.md. Manual: `docs/manual/mixer.md` (stereo inserts and the cap). Unit check: four stereo-sample tracks and two WIDENS inserts: two tracks play wide samples, two play M.

**A6 — Sidechain key** (S, Level C). Files: `insert_host.*` (3.5), `audio/src/engine.cpp` (`block_end`: `gate_off_` with `key_mask`), the key tap, `track_level`, the params page's KEY display (`app/src/screens/params_screen.cpp`, `module_ui.cpp` describe: `OFF`, `T1`..`T16`, not learnable), `modules/templates/effect_key/` (a ducker: gain = 1 - depth x level, Attack / Release at block rate, stereo-capable), CMake. Tests: `tests/audio_insert_test.cpp` (`key:` a key on a lower track is this block's, on a higher one the previous block's, on the master this block's; a silent key 0; a muted key track still keys and its voices render; slot B tap and slot B module both measure; the tap is cleared when no key names the track; a KEY change by ParamChange moves the tap; export equal bits; no allocation), `tests/mixer_inserts_test.cpp` (KEY row shows `T3`, Learn refuses it). QA rows: mixer.md. Unit check: the key template on a pad track keyed by a muted kick track ducks audibly; `ins` shows the key mask.

**A7 — Master stereo insert** (S, Level C). Files: the S6 master adapter in `insert_host.*`, the Mixer master cell (`ST`). Tests: identity stereo on the master is bit for bit L and R; a NaN from `render_st` never reaches the limiter; one instance counts 1 of the cap, a mono effect still 2. Unit check: the template on the master, `diag cpu` delta, limiter still catches peaks, 0 underruns.

**A8 — Factory presets, model and apply** (S, Level B). Files: `app/include/o3os/app/param_set.h` (new), `app/src/module_ui.cpp` and `module_ui.h` (`factory_presets`, `apply_preset`), `app/src/param_actions.cpp` (the instrument group, the insert path of S5), console `mod preset <slot> <n>` and `ins preset <track> <slot> <n>` (`platform/mk66/src/app_main.cpp`). Tests: `tests/param_set_test.cpp` (mask, defaults past `count`, clamp, INT rounding), `tests/module_ui_test.cpp` (apply to an instrument is one undo unit, undo restores every value; to an insert writes the blob, sends 32 ParamChanges with the generation, no undo), `tests/mixer_inserts_test.cpp`. QA rows: `docs/qa/catalogue/modules.md`, mixer.md (console rows).

**A9 — PRESET overlay** (S, Level B). Files: `app/include/o3os/app/view_state.h` (the overlay's view), `app/src/bindings.cpp` (F5 on a module page, the overlay's keys), `app/src/param_actions.cpp` or a new `preset_actions.cpp`, `app/src/screens/params_screen.cpp` (key row `PRESET` on F5 for a module target, the list), sim `sim/scenarios/qa/qa_presets.txt` and goldens (regenerated on the remote machine and looked at), `sim/src/sim_apps.cpp` (a native module with presets in the sim). Tests: `tests/mixer_inserts_test.cpp`, `tests/module_ui_test.cpp` (open, move, load, back, `NO PRESETS`, a sample instrument's F5 unchanged). QA rows: modules.md, mixer.md, instrument.md. Manual: `docs/manual/modules.md` (presets), `docs/manual/mixer.md`; ui-map lines.

## 5. Real-time rules

`run`, `begin_block`, the key tap, `apply_param`, `get_clock`, `track_level`: no allocation, no I/O, no blocking, no double, bounded loops (16 tracks, 4 instances, 2 slots). New render state in SRAM_U, code in flash (`O3OS_AUDIO_COLD`); `play_block` and `run_tracks` are not edited. Single writers: the render writes the Graph's slots and masks, `side_[t]` (with `feed_sides`, same context, in order), the key words and the clock store; the main loop writes only the command ring and ParamChanges, as OS-INSFX section 3.

## 6. Assumptions (decided here; local, testable, reversible)

- A-1 One minor, 1.11, for all four features; A1 publishes the whole header shape, the later slices wire it; the minor is frozen only at a release.
- A-2 A stereo effect keeps a mono `render` (the loader's rule for effects stays): used by a stereo-capable effect on mono blocks; a WIDENS effect's `render` is called by no host of this OS but must be valid.
- A-3 S stays MAIN only, as for stereo voices; sends, CUE, REC and USB track stems carry M'.
- A-4 One shared S block for every stereo track insert (F5 ordering); the stereo bit of a slot is a Graph mask, not a Strip field.
- A-5 The clock's beat counts from step 0 of the playing pattern (Q5); STARTED and WRAPPED flags; `tempo_x100` what plays; while stopped the project tempo and beat 0.
- A-6 The key is a block peak (0..32768), not RMS and not the key's audio; at most two KEY parameters a module; KEY is never learnable.
- A-7 The key is measured in slot B (the tap, or the module there); a key track's voices are exempt from the OS-ODD11 gate while it is a key.
- A-8 Factory presets: at most 32 a module, names up to 15 characters, a preset sets every parameter; applying is one undo unit on an instrument, none on an insert (D9).
- A-9 F5 is PRESET on a module target's parameter page (a module has at most 4 pages, F14); Shift + F5 is left for OS-MODPRESET's SAVE.
- A-10 Built-in modules (TB-303, Chip, FM, SID) get no factory presets in this item (flash); OS-MODPRESET decides how they get presets.

## 7. Risks

- R1 **CPU of a stereo track insert** (Q7): the adapter is about twice the mono one; measured in A4 before merge; a fused SRAM_L kernel is the fallback only if SRAM_L room appears.
- R2 **Stale S on MAIN** (a pointer left at the shared block): the begin_block clear and its tests (idle strip, unbind, export).
- R3 **The cap surprises users** (Q4 a): four WIDENS inserts make every stereo sample mono; said on the page (`ST`), in `ins` and the manual.
- R4 **Ghost keys cost CPU**: a muted key track's voices render; documented; the user unkeys it to save the CPU.
- R5 **Key resolution**: one level a block (1.45 ms); a sidechain attack under about 3 ms is not exact, and a key on a higher track is a block late; documented in the SDK.
- R6 **ABI mistakes are permanent** once a release ships 1.11: Level C risk review of A1 plus one independent review; the layout asserts.
- R7 **SRAM_U margin** (1,968 B): about +232 B here; checked per slice with `size -A`.
- R8 **Module switching shape** (stereo-capable, S appears mid-note): the module's job, the SDK says so; the key template shows the pattern.

## 8. Evidence labels

- Every behaviour in sections 3.1 to 3.6: **new Community OS behaviour** (F1: the official firmware has no inserts and no user sidechain, verified from tracker-ce).
- Costs marked [est] are estimates; A4's `insert_st`, A6's tap and A3's clock figures are replaced by the unit's bench rows.
- Facts F2 to F15: code facts at c1dc3125, F9 and F10 measured on the unit.

## 9. State for implementer

Every slice: SRAM_L +0 B (`size -A` of the mk66 image; any growth stops the slice), new state in SRAM_U, code in flash; `tools/remote_ci.sh` scoped (`O3OS_CI_SCOPE=changed`), one `tools/ci.sh host` per branch, `tools/ci.sh qemu` before merge (module and DSP changes); Level C: a risk review and one independent review; report flash, SRAM_L, SRAM_U, SDRAM deltas.

- **A1.** Start with: `o3m_module` and `O3M_MODULE_SIZE` (`module/sdk/o3module.h:748-783`), the asserts (`:845-866`), `read_desc` (`module/src/image.cpp:240-309`), `add_native` (`module/src/loader.cpp:121-128`), `DescExt32` / `desc_size` (`module/include/o3os/module/image.h:139-156`). Rules: nothing moves in 1.0..1.10 layouts; `struct_size` per minor; C++ default member initialisers (`O3M_DEFAULT`) for the four new descriptor members; the history comment says what changed and that every older module loads unchanged.
- **A2.** Start with: `tools/o3m_gen.py` (the `labels` and `pages` emission), `modules/templates/effect/example_effect.cpp:55-119`, `modules/CMakeLists.txt:186-193` (the biquad's rows), `module/sdk/README.md` sections 2, 3 and 8. Rules: the template is float only, no libm, `O3M_HOT` render, state in its block; the manifest's presets are checked against the params at generation.
- **A3.** Start with: `EventSource` (`audio/include/o3os/audio/event_source.h:30-46`), `Engine::play_block` tempo (`audio/src/engine.cpp:402-411`), `Engine::block_end` (`engine.cpp:158`), `Sequencer::step_start_` and `tempo_x100()` (`seq/include/o3os/seq/sequencer.h:348`, `:581`). Rules: computed in flash at the block boundary, never in `play_block`; Q32 arithmetic, no double; the offline export writes it too.
- **A4.** Start with: `InsertHost::run` (`audio/src/insert_host.cpp:753-810`), `Graph::run_tracks` inserts and side read (`audio/src/graph.cpp:899-904`, `:928-933`), `Graph::side_pass` (`graph.cpp:961-982`), `Engine::feed_sides` (`engine.cpp:78-90`). Rules: rule 7 of 3.2 in flash; no change to `run_tracks` or `play_block`; the mono path stays bit for bit (`audio_insert_test` existing cases); unit check line in A4.
- **A5.** Start with: `VoicePool::st_start` (`audio/src/voices.cpp:618-640`), `kStMax` (`voices.h:1319`), `InsertHost::attach` (`insert_host.h:92`), `draw_inserts_page` (`app/src/screens/mixer_screen.cpp`). Rules: a reservation never takes a block from a sounding voice; the master reserves none.
- **A6.** Start with: `Graph::install_insert` / `set_insert_bypass` (`graph.cpp:135-145`), `Engine` `gate_off_` (`engine.cpp:419`, `engine.h:116`), `InsertHost::apply_param` (`insert_host.h:156`), `InsertHost::prepare_export`. Rules: the tap only in an empty slot B; keys reset at export start; KEY not learnable.
- **A7.** Start with: the S6 master adapter as built (`docs/specs/OS-INSFX.md` S6), `kMasterChain` (`audio/include/o3os/audio/graph.h:118-129`). Rules: one instance for a stereo effect, scrub after it.
- **A8.** Start with: `write_module_param` (`app/src/param_actions.cpp:303-345`), `Editor::begin_group` (`model/include/o3os/model/editor.h:85`), S5's `write_insert_param` (`feat/OS-INSFX-S5` f907cded, `app/src/param_actions.cpp`), `ModuleUi::send_param`. Rules: clamp and round as the page does; one undo unit on an instrument, none on an insert.
- **A9.** Start with: `instrument_page` (`app/src/instrument_actions.cpp:54-63`), the ModulePicker overlay bindings (`app/src/bindings.cpp:1012-1019`), S5's params page target (`ViewState::params.insert`). Rules: F5 only on a module target; goldens regenerated on the remote and looked at.
- Unit checks (tester, a gated image): A1 module loads; A3 synced pan; A4 bench and Dense8; A5 the cap with four stereo samples; A6 ghost-kick ducking; A7 master stereo with the limiter.
