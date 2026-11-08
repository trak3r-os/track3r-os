# OS-MODRATE: a selectable render rate for instrument modules (AMIGA hold only)

Status: spec for review, 2026-11-08 · **Level C** (persistent format, published ABI, audio path) · new Community OS behaviour, no official-firmware material · scope (maintainer, 2026-11-08): **AMIGA only**, RATE = FULL, 1/2, 1/4, each sample held, no filter; no CLEAN, no half-band slice; instrument (sound) modules only, effects out.

Repo state read: `dev` a0ac130e. Line numbers are of that state.

## Maintainer decisions

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | Where does the opt-in flag live | (a) a new bit `O3M_FX_RATE = 2` in the existing `fx_flags` word (offset 88), valid for instruments only, ABI 1.13; (b) a new appended descriptor member (100 to 104 bytes) | (a) | The word exists and is "every other bit 0" today, so no size change, no `O3M_MODULE_SIZE(13)` change. Cost: the name says FX for a bit that an instrument sets; the header comment is reworded ("module flags"), the name `O3M_FX_WIDENS` stays |
| Q2 | ABI number | 1.13 (the minor is 12 today, not 11) | 1.13 | `O3M_ABI_MINOR` is 12 (`module/sdk/o3module.h:115`). A 1.12 OS refuses a 1.13 file, which is the wanted behaviour |
| Q3 | RATE change on a live instrument | (a) re-bind that one slot at once (notes of it cut); (b) apply at the next note-on when the slot is idle, else at its next idle; (c) cut with a 2 ms fade | (b), with (a) as the fallback when the slot never goes idle for 1 s | `init()` caches `host` and computes rate-dependent coefficients, so a rate change is a re-init, not a flag. (b) never clicks |
| Q4 | Where the value is stored | (a) INST 1.8, one appended byte, written only when not FULL; (b) inside the module param blob | (a) | The blob is opaque and never interpreted by the OS; (a) keeps every existing project byte for byte identical |
| Q5 | Where the row sits | (a) a host row of the instrument, like MIDI CH / PLAY STEREO / PLAY THRU, shown only for a flagged module; (b) a row on the module's own first page | (a) | Same machinery as the other host rows (`instrument_param::PlayThru` etc.), undo for free, the module cannot misdraw it |
| Q6 | Built-in flag set | flag sine, fm, tb303, sid; flag chip only after its fix (R2) | as stated | Section 5 |
| Q7 | Pitch-ramp and per-frame effects at 1/4 | scale `per_frame` by R in the host | yes | The module sees one frame per R output frames, so a per-frame step must be R times larger to keep the pitch glide in time |

## 1. Goal

Let the owner trade fidelity for CPU per instrument: a flagged instrument module renders `frames / R` frames at `host->sample_rate / R` (R = 2 or 4) and the OS holds each result sample for R output frames. The sound gets crunchy and aliased on purpose, the module costs about 1/R of its CPU. TB-303 (per-sample cost, 14.8 to 21.6% a voice, `docs/architecture/cpu-budget.md:9`) and FM (10.7 to 11.7%) are the targets. Default FULL: nothing changes for any existing project.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | `O3M_ABI_MINOR` is 12; the descriptor is 100 bytes, `O3M_MODULE_SIZE(12) == sizeof(o3m_module)` | `module/sdk/o3module.h:115`, `:848-850`, assert `:975-976` | verified (read) |
| F2 | `uint32_t fx_flags` at offset 88, "effects only; every other bit 0"; only bit `O3M_FX_WIDENS = 1` is defined | `o3module.h:888`, `:176-178` | verified |
| F3 | The loader rejects any unknown `fx_flags` bit: `fx_ok`, the loader check and `DescFx32` | `module/src/image.cpp:192-203`, `module/src/loader.cpp:40-44`, `:234`, `module/include/o3os/module/image.h:181-186` | verified |
| F4 | `render(state, in, frames)` may be called with any `frames` 1..`O3M_BLOCK_MAX` (128); "no state kept per call" | `o3module.h:126-128`, `:867` | verified |
| F5 | One `o3m_host host_` is shared by all instances; `sample_rate = 44100.0f` at construction, `set_sample_rate` rewrites it | `audio/src/module_voices.cpp:86-106` (`:95`, `:97`), `audio/include/o3os/audio/module_voices.h:106` | verified |
| F6 | `bind()` calls `mod.init(state, &host_)` then applies the stored param blob; modules cache the `host` pointer for their whole life (chip: `v.host = host`) | `module_voices.cpp:127-189` (`:154`, `:156-164`) | verified |
| F7 | `render(begin, end)` is called per segment of a block (split at note starts); n = 1..64 (a rated module call gets k = ceil(n / 2^shift), at most 32); it renders into a float scratch of `kBlock` (64) and converts to Q15 | `module_voices.cpp:344-390` (`:370`, `:376`), `module_voices.h:76` | verified |
| F8 | Up to `kMaxModuleInstances = 8` instances, one per instrument slot, mono; per-instance hot fields in `Info`; SRAM_L is nearly full (24 B margin at OS-MODPITCH time) | `module_voices.h:288-310`, comment near `:320` | verified; margin to re-measure [est] |
| F9 | A command ring (SPSC, `kMaxCommandsPerBlock = 32`) carries main-loop changes to the audio side | `module_voices.h:259` (`Cmd`) | verified |
| F10 | Module parameters live in the opaque INST blob (`params[kModuleParamBytes = 256]`); INST is written at versions 1.1 to 1.7, newest `kInstVersionPlayThru = 0x0107`; the reader's known minor is 7 | `model/include/o3os/model/instrument.h:130`, `limits.h:36`, `formats/include/o3os/formats/native.h:58-88`, `formats/src/native_reader.cpp:57` | verified |
| F11 | `Instrument` ends with `stereo`, `slice_thru`, `reserved[2]` (never written to the file); size 720 | `instrument.h:140-153` | verified |
| F12 | The writer picks the INST version from the fields in use, so old projects stay byte identical | `formats/src/native_writer.cpp:44-65`, field writes `:375-401` | verified |
| F13 | Policy: a newer minor is read leniently (the older reader skips the tail byte, loses it on its next save); the same version uses exact bounds | `docs/formats/o3-project.md` (INST table, Versions, section 10) | verified |
| F14 | Host rows of an instrument (MIDI CH, PLAY STEREO, PLAY THRU) are `ParamKind::Instrument` fields with their own descriptors, described in `describe_param` | `app/src/module_ui.cpp:335-363`, `model/include/o3os/model/params.h:69`, `:304` | verified |
| F15 | The built-ins read `host->sample_rate` at init (tb303 `voice_init`, fm `rate_ms` / `inc_unit`, sid `:519`, sine `s->rate`, chip `:364`); no `44100` literal in their sources except chip_ce.h (F16) | `modules/*/` | verified |
| F16 | chip has absolute-Hz clamps: `kMaxHz = 18000` (`chip_ce.h:39`), SVF `fc` 40..18000 (`:179`), noise LFSR bound `kLfsrMax = 7` from 18000/44100*16 = 6.53 steps (`:42-46`, `:218-227`, `CHIP_CE_ASSERT :227`) | `modules/chip/chip_ce.h` | verified |
| F17 | The ABI pins in tests assert the minor is 12 | `tests/module_edit_test.cpp:547-548`, `:3499`, `:3618`, `tests/module_view_test.cpp:605`, `:841` | verified |
| F18 | `o3m_gen.py` knows manifest keys (`"stereo": "widens"` at `:40`, `STEREO_ABI_MINOR = 12` at `:71`, `needs_minor` at `:375`) | `tools/o3m_gen.py` | verified |

## 3. Design

### 3.1 ABI (new Community OS behaviour)

- `O3M_ABI_MINOR` 12 to 13. New bit `O3M_FX_RATE = 2` in `fx_flags`: "an instrument module that works at any `host->sample_rate` (no hard-coded 44100, no absolute-Hz limit that breaks at 11025); the OS may offer RATE and call it at 1/2 or 1/4 of the engine rate". Descriptor size unchanged (100).
- Image and loader rules, per kind: an effect may set `O3M_FX_WIDENS` only (as today); an instrument may set `O3M_FX_RATE` only and only with `abi_minor >= 13`; any other combination and every other kind: bits must be 0. `fx_ok` / `DescFx32` / the loader line `:234` gain the kind and minor argument. A 1.12 image keeps its old rule exactly.
- `o3m_gen.py`: manifest key `"rate": true` sets the bit and requires `"abi": "1.13"` (error otherwise); `o3m_pack.py` / the checker need no change unless they list flag bits (implementer greps).
- `module/sdk/README.md` and the version history in `o3module.h` get the 1.13 line; the module's contract text: "your `sample_rate` may be 22050 or 11025 when `O3M_FX_RATE` is set; frames per call may be 1..32; stay correct and silent-safe".

### 3.2 Host side

- **Per-instance host.** Each `Instance` holds its own `o3m_host` (60 bytes after 1.12 [est]) with a stable address; at bind it is a copy of `host_` (same `size`, function pointers, `abi_minor`) with `sample_rate = engine_rate / R`. `mod.init(state, &inst.host)`. `set_sample_rate` rewrites every instance's copy only while the OS is stopped (as now). Importers (`ModuleImport::build_host`, `app/src/module_import.cpp:575-586`) are not touched.
- **Rate state per instance** (SRAM_U, cold side): `shift` (0 FULL, 1 = 1/2, 2 = 1/4), `ph` (output frames of the current held sample already given out, 0..R-1), `hold` (the current reduced sample, one float). Nothing else: AMIGA has no filter and no history.
- **Expansion in `render(begin, end)`.** For a segment of m output frames of one instance: (1) give out `min(m, R - ph)` copies of `hold` while `ph != 0`; (2) for the rest r, ask the module for `ceil(r / R)` frames into the scratch (at most 32 at R = 2, 16 at R = 4, inside F4); (3) write each reduced sample R times into the Q15 block, the last one possibly partly (`ph = r mod R`, `hold` = that sample). The conversion `to_q15` runs on the output frames as today. `ph = 0` and `hold = 0` at note-on, so a note starts on its exact output sample.
- **Idle.** If `render` returns 0 the instance goes idle as today and `hold` is zeroed after the last held sample is given out; there is no filter tail to flush (the reason AMIGA is cheap).
- **Pitch and ramps.** The note-on sample offset stays in output frames; the module's pitch ramp `per_frame` (OS-MODPITCH `ramp_block` / `pitch_call`) is multiplied by R (Q7). Any host-side per-frame count handed to the module (frames, clock `get_clock`, `track_level`) is stated in module frames or output frames as before: `get_clock` stays in output frames (the OS clock), documented in the SDK contract.
- **RATE change (Q3).** The main loop writes the instrument field (undoable) and posts a command `SetRate{slot, shift}` through the ring (F9). The audio side stores a pending value. At the next moment the instance is idle (or at the next note-on of an idle instance) it runs the existing bind path for that slot: shutdown, copy host with the new rate, `init`, re-apply the param blob (F6). **As built (S2, review fold): no hard cut.** A pending RATE waits while the instance sounds (`active != 0`); asking for the current shift again cancels it. At idle the gate is shut and a Release queued (one block start), then the instance is withdrawn (one more), then after two more block starts the module is shut down, `init`ed with the new host and the blob re-applied, and the gate restored. A note-on asked for in that window of about three blocks is dropped (the gate is shut: a trade-off, recorded; a note that started inside the window sends the change back to pending). **As built (S3b):** `Instrument::rate` is the only copy. A bind always starts at FULL and the main loop's `ModuleUi::sync_slots` (every instrument edit, undo and redo, a project load, once a pass) applies the stored byte through `set_rate`; there is no `take_rebound` bit and no session copy. A stored non-FULL RATE of a module that cannot restart plays FULL and keeps the byte. A module whose `init` refuses the restart is dropped: the slot is unbound, the gate stays closed, nothing renders (`Rate::Failed`). A project load and an instrument switch bind with the stored value, so they need no extra path. Retire needs the existing two-block-start discipline (`module_voices.h:148-156`); the implementer reuses it per slot.
- **Offer.** `describe`/UI shows the RATE row only when the instance's module has `O3M_FX_RATE`. A project that holds a non-FULL rate for a module without the flag (module replaced on the card) plays FULL and keeps the stored byte (no data loss, no silent rewrite).
- **Accounting.** OS-GOV and the perf counters meter the instance's `render` cycles as today, so a cheaper instance shows as cheaper with no change.

### 3.3 Persistence (Level C, exact)

- INST version **1.8** (`kInstVersionRate = 0x0108`): version 1.7's fields whole, then **one byte** `rate`: 0 FULL, 1 = 1/2, 2 = 1/4. A value above 2 reads as 0 (as the stereo byte does). Meaningful for a Module instrument only.
- Writer (`formats/src/native_writer.cpp:44-65`): 1.8 only for a Module instrument with `rate != 0`; every other instrument is written at the version it gets today, so every existing project stays byte for byte identical. The chunk size grows by 1 for 1.8 only.
- Reader (`native_reader.cpp:57`): known INST minor 7 to 8. Version 1.8 reads the byte; every older version leaves `rate = 0`. A reader of 1.7 meeting 1.8 follows F13: it skips the tail byte, reads FULL and loses it on its next save (documented, as for earlier minors).
- Model: `Instrument::rate` takes one of the two `reserved[2]` bytes (struct stays 720 bytes, static assert kept); `Project::set_instrument` canonical form clears `rate` for any kind other than Module; undo copies the whole instrument already.
- Docs: `docs/formats/o3-project.md` INST table, version paragraph, Versions rule, section 10 Evolution, section 8 Limits; any Python reference of INST (`tools/o3project.py` and its test, if it models INST) gets the same byte.
- Tests (all in the slice): (1) round trip of a Module instrument with rate 1 and 2 (write, read, equal); (2) the hex fixture of a 1.7 project reads `rate == 0` and, when re-written, is byte identical to the input; (3) a hand-built 1.8 chunk with rate 3 reads 0; (4) a 1.8 chunk read by the exact-bound path with a wrong size is refused; (5) a FULL instrument and a non-Module instrument write no 1.8; (6) `set_instrument` clears `rate` on a non-Module kind; (7) the 1.7 reader contract: a 1.8 file loads with a tail-skip (existing lenient-minor test pattern).
- Session-only until S3b: S2 kept the rate in the UI (not saved); S3 added the saved byte (INST 1.8), S3b makes `Instrument::rate` the single source (set, undo, redo, load all apply it). S2 is not offered to users before S3 merges (hold the row behind the slice order; both merge in one batch).

### 3.4 UI (Q5)

- A host row **RATE** on the instrument page, under PLAY THRU, shown only for a Module instrument whose module has the flag; one field in `instrument_param` (next free id), descriptor `rate_desc()` beside `play_thru_desc()` in `model/params.h`, hooked in `describe_param` (`app/src/module_ui.cpp:343-363`), reading and writing the whole instrument as one undo record like PLAY THRU. The exact screen/row placement follows PLAY THRU; the implementer locates it with `build-map/screens.md` (not verified in this spec).
- Labels (enum, wrap off): `FULL`, `1/2`, `1/4` (hint line: "AMIGA HOLD, CRUNCHY, LESS CPU"). Encoder cycles the three values; the change takes effect as in Q3.
- QA catalogue rows and a manual line ("new in Community OS: RATE") with the slice.

## 4. Slices

| Slice | Content | Files | Tests | Flash / SRAM [est] | Risk |
|---|---|---|---|---|---|
| S2 | ABI 1.13 and the bit; per-instance host; AMIGA expander; RATE command and re-bind; model field `rate`; UI row; session only (not saved) | `module/sdk/o3module.h`, `module/src/image.cpp`, `module/src/loader.cpp`, `module/include/o3os/module/image.h`, `tools/o3m_gen.py`, `audio/include/o3os/audio/module_voices.h`, `audio/src/module_voices.cpp`, `model/include/o3os/model/instrument.h`, `model/include/o3os/model/params.h`, `model/src/params*.cpp`, `app/src/module_ui.cpp`, `app/src/param_actions.cpp` (as PLAY THRU), the ABI pins F17 | image/loader tests: flag accepted for instrument at 1.13, refused on effect, on 1.12, with other bits; module_voices test: a test sine module at R = 2 and 4 gives held pairs/quads, exact note start, split segments (m = 1, 3, 5, 63, 64) equal the single-call result; RATE change at idle and under a held note; FULL output bit-identical to before (golden); UI/param test with undo | flash 1 to 1.5 KiB (backlog 1 to 2 KiB); SRAM_U +8 x (60 host + 8 state) = ~550 B [est] against ~1.8 KB arena margin (OS-INSFX-ABI2 F15); SRAM_L: the expander is on the render path, so put only the hold loop there and measure the `.ramfunc_l` margin (24 B at OS-MODPITCH time); setup/re-bind code in COLD | high: audio path, shared host pointer cached by modules, SRAM_L headroom |
| S3 | persistence INST 1.8, reader, writer, docs, Python reference | `formats/include/o3os/formats/native.h`, `formats/src/native_writer.cpp`, `formats/src/native_reader.cpp`, `model/src/project.cpp`, `docs/formats/o3-project.md`, `tools/o3project.py` (if it models INST), tests | the seven tests of 3.3 | flash ~150 B; SRAM 0 | high: Level C format; mitigated by byte-identical old output and the fixtures |
| S4 | CLEAN: **dropped** by the maintainer 2026-11-08 (no filter, no send-reverb half-band reuse) | none | none | none | none |
| S5 | Flag the built-ins: chip fix first (R2), then `"rate": true` and `"abi": "1.13"` in `modules/{sine,fm,tb303,sid,chip}/module.json`, goldens regenerated, per-module hold tests at R = 2, 4 (no NaN, no overflow, silent after note-off), by-ear check list | `modules/*/module.json`, `modules/chip/chip_ce.h`, `modules/chip/chip_dsp.h`, golden files, `tests/chip_golden_test*` | module tests host-side, `tools/ci.sh qemu` gate (module change, target goldens run only there), then unit: TB-303 and FM cost at 1/2 and 1/4, by ear | flash ~100 B (chip clamp), SRAM 0 | medium: chip noise bound; module `abi` bump changes the image hash of every built-in |

Order: S2, S3 in one batch (S2 alone is session only), S5 after. The backlog's S5 list ("chip, phaser, pingpong, comp") mixes effects in: the instrument modules are sine, fm, tb303, sid, chip only; the effects (phaser, pingpong, comp, fold, drive, wide, chorus, autopan, envfilter) are out of scope.

## 5. Per built-in verdict

| Module | Verdict | Why |
|---|---|---|
| tb303 | can take the flag | `voice_init(v, sv, host->sample_rate)` (`modules/tb303/tb303.cpp:33`); the 4th-order decimator and the 2x oversampling scale with `fs` (`tb303_dsp.h:378-414`). The cutoff ceiling is pi/4 of the 2x rate (`:20`) so the top cutoff and the control rate (`kCtl = 4` samples, `:37`) fall with R: darker and stepped at 1/4, by design. Check by ear |
| fm | can take the flag | rates from `host->sample_rate` (`fm_dsp.h:268-270`); operators past 0.4 of the rate fade and are silent at 0.5 (`:156`), so no extra aliasing control is needed |
| sine | can take the flag | `s->rate = host->sample_rate` (`modules/sine/sine.cpp:39`); the sample module, also the test vehicle of S2 |
| sid | can take the flag | `sid_dsp.h:519` reads the rate; `kControl = 32` samples (`:28`) is 1.4 ms at 44.1 kHz, 5.8 ms at 1/4: audible only on fast sweeps. Confirm by ear and the mip/table choice by `dt` in S5 |
| chip | needs a fix first (R2) | `chip_ce.h` clamps to 18000 Hz (`:39`, `:179`), above Nyquist at 22050 and 11025, and the noise LFSR bound `kLfsrMax = 7` assumes at most 6.53 steps a sample (`:42-46`, `:218-227`), true only at 44100 (at 11025 up to ~26 steps; the `CHIP_CE_ASSERT` at `:227` would fire or the noise would be wrong). Fix: derive the clamp from `host->sample_rate` (0.4 x rate), size `kLfsrMax` for the lowest rate (or loop bounded by it), re-run `chip_golden_test` (FULL output must stay identical) |
| any card module | opt-in by the author | the flag is the author's promise; the SDK states the contract (3.1) |

## 6. Real-time rules

- Expander: no allocation, no SD, no blocking; loops bounded by 64 output / 32 reduced frames; float only, no libm, no `double`.
- Placement: the hold loop and the per-segment arithmetic are the only new hot code (O3OS_AUDIO_HOT if it fits `.ramfunc_l`); bind, `SetRate` handling and host copy are `O3OS_AUDIO_COLD`.
- ISR-shared state: the main loop never touches an instance's host or rate state; it posts `SetRate` through the SPSC ring (F9, within `kMaxCommandsPerBlock`); the audio side owns `ph`, `hold`, `pending`.
- A bound instance's `o3m_host` address is stable until shutdown (F6); re-bind changes it only after `shutdown`, so no module keeps a stale pointer.
- FULL path: zero added work per segment beyond one branch on `shift == 0`; the golden of a FULL render must be bit-identical.

## 7. Assumptions

- A1 The engine rate stays 44100 on the unit; the reduced rate is `engine_rate / R` so a different engine rate still works.
- A2 Held samples at the segment edge (split at a note start) are handled by `ph`/`hold`, not by asking the module for a fractional frame.
- A3 A module with the flag renders correctly for any `frames` 1..32 (F4 already promises 1..128).
- A4 The cost saving is close to 1/R for TB-303 and FM (per-sample cost); modules with a large per-block fixed cost save less. Measured on the unit, not assumed [est].
- A5 The row location follows PLAY THRU (F14); not verified against the screen files.

## 8. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Modules cache the host pointer and compute coefficients at init, so a RATE change is a re-init; a bug there leaves a stale pointer or a wrong rate | Per-instance host with a stable address; re-bind only through shutdown + init; tests at idle and under a held note |
| R2 | chip clamps and noise LFSR bound are 44100-specific (F16) | Fix before the flag (S5); chip stays unflagged until its golden test and a low-rate test pass |
| R3 | Level C format and ABI: INST 1.8 and ABI 1.13 are irreversible once shipped | Byte-identical old output, fixtures, lenient-minor contract, flag refused below 1.13 |
| R4 | SRAM_L has 24 bytes of margin (F8) and SRAM_U arena margin is ~1.8 KB; the expander and 8 host copies may not fit | Measure on the S2 build; hot code minimal; if SRAM_L is short, run the expander from flash (a few cycles per segment) |
| R5 | Pitch ramps, control-rate counters and `get_clock` behave differently at R = 4 | Q7 scaling; SDK contract text; by-ear check on tb303 slides |
| R6 | Aliasing is the intended sound; a module that sums a high note near the reduced Nyquist may overflow or click | Module tests at R = 2, 4 for no NaN, no overflow; Q15 conversion already clamps |

## 9. Evidence labels

- New Community OS behaviour: RATE, the flag, the per-instance host, the expander, INST 1.8, the UI row, all of sections 3 and 4.
- Verified (read in the tree): every row of section 2.
- Estimates [est]: all CPU, flash and SRAM figures; to be replaced by builds and unit measurements (S2 build, S5 unit run).
- S2 measured sizes (mk66 build of `feat/OS-MODRATE` vs dev 2a5f547f, linker usage table, fold of the review, 2026-11-08): FLASH `.text` +2,184 B (990,816 -> 993,000; image to the pad +2,212 B), `.ramfunc_l` +28 B (26,916 -> 26,944), SRAM_L total +24 B (49,192 -> 49,216, the table's 96.08 -> 96.12 %), SRAM_U +264 B (138,536 -> 138,800; 70.46 -> 70.60 %), SDRAM_STATIC 0. The hot path grew only by the FULL branch in `render` (`render_rated`, `set_rate`, `bind`'s rebound bit are flash).
- Open research question: by-ear acceptability of TB-303 at 1/4 (cutoff ceiling), sid at 1/4, the SRAM_L margin on the S2 build.
- No parity claim is made; the official firmware has no such setting.

## 10. State for implementer

- ABI minor is 12 now; this work is 1.13. The only descriptor change is a bit in `fx_flags` (offset 88); the size stays 100. Update the pins in F17 and `kOsAbiMinor`.
- `ModuleVoices::host_` is shared today; make a per-instance copy in `Instance` (stable address), `init` gets that. Copy `size`, function pointers, `abi_minor`; set `sample_rate = engine / R`.
- Expansion state is `ph` and `hold` only (AMIGA, no filter). Segments are 1..64 frames (module calls 1..32) and split at note starts: test m = 1, 3, 5, 63, 64 against the one-call result.
- FULL must stay bit-identical (golden before and after); rate state costs one branch.
- RATE change: command through the ring, apply at idle by the existing bind path (shutdown, init, re-apply the blob); no cut (a sounding instrument keeps the change pending).
- INST 1.8 appends one byte, written only for a Module instrument with `rate != 0`; use one `reserved` byte of `Instrument`; reader known minor 8; value above 2 reads 0; update `docs/formats/o3-project.md` in the same slice.
- UI row follows PLAY THRU (`app/src/module_ui.cpp:343-363`, `model/params.h:304`); labels FULL, 1/2, 1/4.
- Do S2 and S3 as one batch (S2 alone is session only); S5 last, with the chip fix first; run `tools/ci.sh qemu` for the module change; record flash and SRAM deltas per slice, SRAM_L margin included.
- There is no CLEAN and no half-band reuse (dropped by the maintainer 2026-11-08); do not touch `send_reverb`.
- Hardware check left after S5: TB-303 and FM cost at 1/2 and 1/4 on the unit, by ear (`docs/architecture/cpu-budget.md` method).
