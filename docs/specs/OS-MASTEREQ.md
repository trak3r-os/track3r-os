# OS-MASTEREQ — master output EQ and bass boost in the codec

Level C (persistent format, codec output path). Public issue #6. Approved by the maintainer 2026-11-12 (5-band EQ and bass boost together, on a new Mixer page).

## Goal

A five-band graphic EQ and a bass boost on the master output, processed inside the SGTL5000 codec's DAP, at no MK66 audio CPU cost. Edited on a new Mixer page `MST EQ`, stored per project.

## Decisions

| # | Decision | Source |
|---|---|---|
| D1 | The codec's 5-band graphic EQ (not the 7-band PEQ) and Bass Enhance | Maintainer 2026-11-12 |
| D2 | A new, sixth Mixer page `MST EQ`, not rows on MST FX | Maintainer 2026-11-12 |
| D3 | The EQ and bass boost act on the analogue outputs only (headphones, line out). They are not in renders, stems, USB audio or resampling. The page says so | Maintainer 2026-11-12; closes the ADR question of `rt-optimize` section 4 for these two stages |
| D4 | Stored per project in a new `.o3pj` chunk `MEQ ` | Orchestrator, confirmed by the maintainer 2026-11-12; the official firmware also stores them per project |
| D5 | With every value at its default the codec stays exactly as today: DAP disabled, `CHIP_SSS_CTRL` = 0x0010, no extra write. (The official firmware keeps the DAP in the path always.) | Orchestrator, confirmed by the maintainer 2026-11-12: zero regression for projects that do not use it |
| D6 | No automatic headroom trim when bands are boosted | Orchestrator, confirmed by the maintainer 2026-11-12; same as the official firmware; hardware check H3 measures the clipping |
| D7 | Not automatable: no param lane, MIDI learn, modulation, Perform or scene recall; no undo (the Mixer keeps no history) | Orchestrator: a sweep would be a stream of blocking I2C writes |

## Codec facts

Sources: DS = NXP SGTL5000 datasheet rev. 7 (table numbers); CE = `tracker-ce@1343677b` (facts only, no code taken).

| Fact | Value | Source |
|---|---|---|
| DAP power | `CHIP_DIG_POWER` bit 4; already set by `kDigPower` 0x0073 | DS table 17; `sgtl5000.h:106` |
| Routing I2S in → DAP → DAC, I2S out from the ADC | `CHIP_SSS_CTRL` (0x000A) = 0x0070; without the DAP 0x0010 | DS table 20; CE `platforms/mk66/3rdparty/Audio/control_sgtl5000.cpp:851-855` |
| DAP enable | `DAP_CONTROL` (0x0100) bit 0. Enabled with every sub-block off, audio passes unchanged; disabled, no audio passes through the DAP | DS table 40 |
| EQ select | `DAP_AUDIO_EQ` (0x0108) bits 1:0: 0 off, 3 five-band GEQ | DS table 44 |
| Bands | 0x0116 115 Hz, 0x0118 330 Hz, 0x011A 990 Hz, 0x011C 3 kHz, 0x011E 9.9 kHz; bits 6:0, 0.25 dB per LSB, 0x2F = 0 dB (reset), 0x5F = +12 dB, 0x00 = −11.75 dB | DS tables 49-53 |
| Bass enhance | `DAP_BASS_ENHANCE` (0x0104): EN bit 0, CUTOFF bits 6:4 (0 = 80 Hz), BYPASS_HPF bit 8. `DAP_BASS_ENHANCE_CTRL` (0x0106): LR_LEVEL bits 13:8, BASS_LEVEL bits 6:0 (0x7F least, 0x00 most) | DS tables 42-43 |
| Click guidance | Mute the outputs before enabling or disabling the DAP; move a band in 0.5 dB steps; bring BASS_LEVEL to 0x7F before toggling bass EN | DS, DAP section and the GEQ volume-change example |
| Official firmware | Uses these same DAP blocks for its master EQ (±12 dB shown, floor −11.75 dB) and Bass Boost 0..100 (linear to BASS_LEVEL 0x7F..about 0x19, CUTOFF 80 Hz, HPF bypassed, LR_LEVEL fixed); DAP always in the path; no ramp, no trim | CE `core/AudioEngine/mtAudioEngine.cpp:257-261`, `core/AudioEngine/mtMasterEnhancer.cpp:6-32`, `platforms/mk66/source/Sgtl5000Impl.cpp:87-133` |

Open research questions: DAP latency (no figure in the DS); the LR_LEVEL code the official firmware ends up with (0x0B by our arithmetic, unconfirmed).

## Behaviour

Parameters (`new Community OS behaviour` in a codec stage the official firmware also uses; ranges are `verified parity` with the CE references above, resolution is ours):

| Parameter | Range | Default | Register value |
|---|---|---|---|
| EQ 115 HZ, 330 HZ, 990 HZ, 3 KHZ, 9.9 KHZ | −12..+12 dB, 1 dB steps | 0 | `0x2F + 4 × dB`, floor 0x00 (−12 gives −11.75 dB) |
| BASS BOOST | 0..100 | 0 (off) | 0: EN = 0. 1..100: EN = 1, CUTOFF = 0, BYPASS_HPF = 1, LR_LEVEL = 0x0B, BASS_LEVEL = `0x7F − round(v × 0.8 × 127 / 100)` |

Codec sequencing (main loop only; I2C is blocking and never called from an ISR):

1. **Idle state.** All six values default and the DAP not routed: nothing is written. This is today's register state, byte for byte.
2. **Route in**, on the first non-default value while the outputs are on: DAC soft mute, `DAP_CONTROL` = 1, `DAP_AUDIO_EQ` = 3, the five bands and the bass registers at their targets, `CHIP_SSS_CTRL` = 0x0070, unmute. It runs as a state machine over several main-loop passes (the two 50 ms DAC ramps are waited by timestamp, never by a blocking wait; at most 3 writes per pass), so the main loop is never held (the starve guard of OS-MAINGUARD trips near 93 ms). Route in always writes the full DAP state and trusts no shadow (codec registers survive an MCU reset). A failed I2C write abandons the sequence, restores `CHIP_SSS_CTRL` = 0x0010 if it can, always ends with the DAC unmuted, and is not retried every pass (one retry on the next value change). While the outputs are muted or not started, only record the values; apply them inside `unmute`, before the outputs come up.
3. **Live change** while routed in: each band moves toward its target by at most 2 LSB (0.5 dB) per write; BASS_LEVEL by a bounded step; bass EN toggles only with BASS_LEVEL at 0x7F. At most 3 register writes per main-loop pass, from one dirty flag per value.
4. **Route out** is never done on a live edit (a band passing through 0 dB must not dip the output). It is evaluated only at `unmute`/start: if all values are default there, the codec goes back to the idle state before the outputs come up. A project load or New with the DAP routed in leaves it routed in with flat values (audio passes unchanged).
5. `mute`, `stop`, the power-off sequence and the line-in source switch keep their present register order; the DAP writes are added around them, never inside them.

HAL: one struct `OutputEq { int8_t band_db[5]; uint8_t bass; }` and one virtual `set_output_eq(const OutputEq&)` on `hal::audio::Device` with a no-op default (simulator, QEMU). The MK66 main loop compares the project's values with a shadow each pass (beside `sync_metro`), so a project load needs no extra hook. The sync runs only with the load phase idle and under the conditions the outputs already need (started, enabled, no power-off latch, no firmware update), and before the boot unmute.

## UI

Sixth `MixerPage` `MasterEq`, appended (the page order is append-only), name `MST EQ`, reached by the Master button's page cycle like the other pages. Six rows: the five bands with a signed dB value (`+3 dB`, `0 dB`, `-12 dB`) and a bar centred on 0, then `BASS BOOST` 0..100 with a bar. Up/down moves the cursor, the encoder changes the value by 1, Shift follows the page family's fast-value convention. A label card reads `ANALOGUE OUTPUTS ONLY` / `NOT IN RENDERS, USB AUDIO OR RESAMPLING`. F-key labels as on the other master pages. Existing widgets only.

## Persistence

New chunk `MEQ ` in `.o3pj`: `u16` chunk version 0x0100, five `int8` band values in dB, one `u8` bass boost: 8 bytes. Written only when a value is non-default, so a project that does not use it stays byte-identical to today's output. The reader clamps to −12..+12 and 0..100, accepts a newer minor (reads the fields it knows) and a missing chunk (flat, bass 0). An older OS keeps and re-emits the chunk as an unknown chunk, after its own known chunks (so after `MEVT`): the chunk is therefore written in its own unit after the lanes unit and before the unknown chunks, with reader rank 21 (CRC moves to 22), and it is not a `Part`. The reader stores the values and applies them at Done (`Reader::begin` resets the live project). `Project::reset` clears them, also with keep-instruments. The Python reader (`tools/o3project.py`) mirrors the rank, the bounds (8 bytes exactly for minor 0) and the clamp. **Importer ABI:** `sizeof(Project)` and `kImportLayoutTag` (`model/import_layout.h`) must not change, or every importer `.tmod` on users' cards is refused; the six bytes go into existing padding or spare bytes. If no room exists, stop and report: a tag bump is the maintainer's decision. Update `docs/formats/o3-project.md`, `tools/o3pj_tool.cpp` and `tests/o3project_roundtrip.py`. OG project files are untouched (reading the official project's EQ and bass values is out of scope).

## Out of scope (recorded, not built)

Space (the codec's surround stage), the 7-band parametric EQ, a software copy of the EQ for renders, EQ changes per pattern, pad entry of band values, a Master + key shortcut to the page, importing the EQ of official projects.

## Done when

- Host test on the register model (`platform/mk66/tests/mk66_sgtl5000_test.cpp`): default values write nothing; first non-default gives mute, DAP enable, EQ select, bands, bass, 0x0070, unmute in that order; a live band change moves by at most 2 LSB per write and at most 3 writes per pass; bass EN toggles only at BASS_LEVEL 0x7F; values set while muted apply at unmute; route out happens only at unmute/start or project load with all defaults; the power-up, mute, stop and input plans are unchanged.
- Format tests (`tests/native_format_test.cpp`, `tests/o3project_roundtrip.py`): round trip; an old file loads flat with bass 0; a default project is byte-identical to the output before this change; out-of-range values clamp; a newer-minor chunk loads; a file as an older OS re-emits it (`MEQ ` after `MEVT`, among the unknown chunks) loads; the import layout tag test is unchanged and passes.
- The `MST EQ` page has a golden; the changed Mixer goldens differ only in the page strip.
- QA rows QA-MIX-113.. in `docs/qa/catalogue/mixer.md`; `docs/ui/official-manual-map.md` rows for the master EQ and bass boost updated; `docs/manual/mixer.md` and `app/help/pages.txt` describe the page and the analogue-only limit.
- Scoped host tests pass; `mk66` builds; flash and SRAM deltas recorded.

## Budget

Flash at most 1.5 KiB, SRAM at most 64 B. Audio CPU per block: none. Main loop: at most 3 blocking I2C register writes per pass while a value settles, none otherwise.

## Hardware-only checks

| # | Check |
|---|---|
| H1 | Each band audibly boosts and cuts at its frequency on headphones and line out; bass boost audible from 1 to 100 |
| H2 | No click on route in (first non-default value while playing), on live band and bass changes, on project load with and without an EQ, on stop/start |
| H3 | A full-scale mix with bands at +12 dB: how much it clips (recorded for D6) |
| H4 | Power-off pop, mute/unmute and the line-in source switch behave as before with the DAP routed in |
| H5 | A project that never touched the page: output identical to the previous image |
| H6 | USB audio and a render do not carry the EQ (as designed) |

## State for implementer

Leads: `hal/include/o3os/hal/audio.h:152` (Device); `platform/mk66/include/o3os/platform/mk66/sgtl5000.h` (addresses 35-54, `kSoftMute` 183, `kDacRampWaitMs` 123, `run_stop` 195); `platform/mk66/src/audio.cpp` (`codec_write` 183, `plan_write` 187, `unmute` 345, `mute` 364, `apply_input` 398); `platform/mk66/src/app_main.cpp` (`sync_metro` pattern near 1150-1176, called near 3899); `app/include/o3os/app/mixer_view.h:24-31`; `app/src/screens/mixer_screen.cpp` (`kPageNames` 29, tab strip 828-838, `draw_master_page` 535-614); `app/src/mixer_actions.cpp` (144-190, 548-584); `formats/include/o3os/formats/native.h` (chunk ids 40-55), `native_reader.cpp` (ranks 21-50, `known_minor`, `v1_bounds` 92, payload 497, the rank check 436), `native_writer.cpp` (measure pass 179, units 459-546: a new unit after the lanes unit), `tools/o3project.py:145`; `model/import_layout.h:57-72`, `tests/module_importer_test.cpp:113`, `model/src/project.cpp:119` (`reset`); `model/include/o3os/model/project.h:194-199, 394` (`volume_law` is the template for the member). Do not add `master_param` entries and do not make `MEQ ` a `Part`. `kMixerPages` becomes 6: every switch on `MixerPage` (about 23 references), the page cycle and the page-name function must handle `MasterEq`.
