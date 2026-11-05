# OS-CEPLAYTHRU — Play Thru for Slice and Beat Slice instruments

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a), not implemented · Level C for P1 (OG instrument byte, native INST minor), Level C for P2 (the voices' note start) · Backlog row OS-CEPLAYTHRU, approved by the maintainer 2026-11-05 ("after OS-CECHECK (a)": what that means is in F7 and Q1) · Facts from `dev` at c1dc3125 and from `tracker-ce@7fd6c2c` (Polyend's published firmware source, cited as facts; no code taken).

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a (every recommended default).**

Only what the maintainer must decide. Every technical detail is decided in this spec and listed under Assumptions (section 6).

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | Wait for OS-CECHECK (a) (an instrument saved by tracker-ce) before P1 | a) no: P1 and P2 merge on the source facts and synthetic files; CECHECK (a) later turns the label "from the source" into "verified on a file" · b) yes: wait for the fixture | a | The byte's place is fixed by the layout (F2, F3: our offsets 278 and 280 already match tracker-ce's neighbours) and tracker-ce reads it without a version check (F5), so a fixture can confirm but not change the design |
| Q2 | Where the toggle goes | a) the instrument's SOURCE page (Params F1), a row PLAY THRU OFF/ON after SLICE, in the Slice and Beat Slice modes only · b) the Wave screen's slice tools | a | It is a play setting of the instrument, next to the slice rows it acts on; the page stays at six rows in the slice modes (limit eight) |

## 1. Goal

A Slice or Beat Slice instrument can play a slice on through the following slices to the end of the sample (until note off or the sample end) instead of stopping at the next slice, with the setting stored where tracker-ce stores it, so an instrument from tracker-ce keeps it and a project saved here gives it back.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | tracker-ce adds `slicePlayThru` (u8; 0 = stop at the next slice, 1 = play to the sample end) right after `delaySend` and before the 48 u16 slice points; default 0; the struct stays 372 B and the instrument version stays 9 | `tracker-ce@7fd6c2c:core/source/mtStructs.h:653-654`, `:763`, `:749`, versions `:43-45` | verified (tracker-ce source) |
| F2 | Our OG offsets: delay send at file offset 278, slice points at 280; byte 279 is alignment padding today, read as nothing and written 0 | `formats/src/og_codec.h:300-311`; `docs/formats/og.md:175-181` | code fact |
| F3 | So tracker-ce's `slicePlayThru` is file byte 279 (header 16 + its struct offset), in the padding the official layout had before the u16 slice table | F1 + F2 | inference from the two layouts (open research question until a tracker-ce file is surveyed, OS-CECHECK (a)) |
| F4 | In tracker-ce, both slice players end a slice at the next slice point, unless `slicePlayThru` is set: then at the sample end; the Slice FX (forced slice) and the note's slice behave the same; a reversed note starts at that end | `tracker-ce@7fd6c2c:core/libs/Audio/play_memory_slice.cpp:36-56`, `core/libs/Audio/play_memory_beat_slice.cpp:42-61` | verified (tracker-ce source) |
| F5 | tracker-ce loads an instrument with a raw copy of the struct, with no version rule for this byte; so any non-zero byte means On, in every version | `tracker-ce@7fd6c2c:core/FileManager/fileManagerInstrument.cpp:663-706`; `core/FileManager/mtFileManagerOldVersionRecovery.cpp` (does not touch it) | verified (tracker-ce source) |
| F6 | The UI: an On/Off value on the "More" page of the Sample Playback screen | `tracker-ce@7fd6c2c:core/interface/samplePlayback/gSamplePlay.cpp:718`; `core/interface/samplePlayback/fSamplePlay.cpp:935`, `:2611-2615` | verified (tracker-ce source); we do not copy the screen (Q2) |
| F7 | OS-CECHECK (a) checks instrument version 9 "with the new fields" on files saved by tracker-ce. Since the version did not change (F1), a tracker-ce file cannot be told from an official one by its header: the only dependency is evidence (F3), not code | `docs/roadmap/BACKLOG.md` row OS-CECHECK | code fact (backlog) |
| F8 | Fixture survey: 70 unique official instruments in the maintainer's fixtures (v9 28, v8 21, v6 8, v4 13): byte 279 is 0 in all 70 | a survey of the fixture folders (not in the repository) | measured; supports "official 1.9 writes 0 there" |
| F9 | Ours: Slice mode plays the selected slice from `slices[k]` to the next point (65535 for the last) at the note's pitch; Beat Slice gives the voices the whole sample and a slice table, and `beat_slice_span` (the one rule of both engines) ends a slice at the next bound | `app/src/og_samples.cpp:66-72`, `:152-185`; `audio/include/o3os/audio/voices.h:487-541`, `:555-578` | code fact |
| F10 | Our Instrument has `reserved[3]` (always 0, the canonical form; struct 720 B); native INST is at minor 1.5 (the stereo byte), each minor appends bytes and is written only when needed; the source param ids 0x14..0x1C are taken, 0x1D is free, 0x1E and 0x1F are virtual fields (CHANNEL, STEREO) | `model/include/o3os/model/instrument.h:134-139`; `formats/src/native_writer.cpp:40-57`, `formats/src/native_reader.cpp:50-56`, `:749-751`; `model/include/o3os/model/params.h:57-76` | code fact |
| F11 | The source page lists PLAY MODE, the mode's rows (Slice modes: SLICES, SLICE), PLAY ENGINE, STEREO | `app/src/screens/params_screen.cpp:212-231` | code fact |

## 3. Design

### 3.1 Model and formats (P1)

- `Instrument::slice_thru` (0 or 1) takes `reserved[0]`; `reserved` becomes `[2]`; the struct stays 720 B (static_assert). `init_instrument` gives 0; `Project::set_instrument`'s canonical form clamps it to 0..1 and keeps it for any play mode (A2).
- OG read (`formats/src/og_instrument.cpp:40-105`): byte 279 non-zero → 1, in every version (F5, verified parity). OG write (`:340-389`): byte 279 = `slice_thru` (0 or 1). Official 1.9 writes 0 there (F8); what it does with a 1 is an open research question (most likely nothing: padding), so an Export counts nothing. `docs/formats/og.md` section 5 gets the row and drops 279 from the "Not kept" list.
- Native `.o3pj`: INST minor 1.6 = the 1.5 layout, then one byte `slice_thru` (after the stereo byte). The writer picks 1.6 only for a Sample instrument with the flag set (the "smallest version that holds it" rule); older readers skip the byte (newer minor, as for 1.5). Reader: a value above 1 reads 0 (the stereo rule). `tools/o3project.py` (the INST version constants, `:58-103`) and `docs/formats/o3-project.md` (INST minors) follow. Whichever of OS-CEPLAYTHRU and OS-CEWTENV W1 lands first takes 1.6; the other takes 1.7 (append-only).

### 3.2 Engine (P2)

- Slice mode (`og_samples.cpp:152-167`): with `slice_thru`, the slice's end is the sample end (`to` = 65535).
- Beat Slice: a `SampleDesc` flag `slice_thru` (in an existing padding byte: `sizeof(SampleDesc)` unchanged); `beat_slice_span` ends at `s.length` when it is set. The Slice FX (forced slice), the live chop pads and the Amiga voices follow because they all use `beat_slice_span` (F9).
- PLAYBACK reverse (OS-PERF-REVERSE) plays the extended part from the sample end down (F4, parity). A stereo side signal (`st_off`) covers the whole sample, so the longer span reads defined frames. No per-block cost: the span is computed at the note's start only.

### 3.3 UI (P2, Q2 a)

A virtual field `instrument_param::PlayThru = 0x1D` (like STEREO: no table row, no generation slot, not learnable, not modulatable, not in the app SDK); descriptor `play_thru_desc()` (PLAY THRU: OFF / ON, default OFF); the source page adds it after SLICE in both slice modes; an edit goes through an action that writes the whole instrument as one undo record and rebinds the slot (as `write_play_stereo`).

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| P1 | Model field, OG byte 279 read/write, INST 1.6, Python mirror, round trips | S | C | +0.2 KiB | — |
| P2 | Engine (Slice, Beat Slice), SOURCE row, QA rows, manual | S | C | +0.4 KiB | P1 |

**P1** (S, Level C). Files: `model/include/o3os/model/instrument.h`, `model/src/project.cpp` (canonical form), `formats/src/og_instrument.cpp`, `formats/src/native_codec.h` (`kInstPlayThru` = 1, the size asserts), `formats/src/native_writer.cpp`, `formats/src/native_reader.cpp`, `tools/o3project.py`, `docs/formats/og.md`, `docs/formats/o3-project.md`. Tests: `og_instrument_test` (byte 279 = 0, 1, 7 reads 0, 1, 1 in v4, v6, v8, v9 synthetic files; write gives 0/1; an official fixture still round-trips byte for byte), `native_format_test` (a flagged Sample instrument writes 1.6 and reads back; an unflagged one stays 1.5 or lower, byte-identical to today; a 1.6 chunk read by a 1.5 reader skips the byte; 2 reads 0), `tests/o3project_roundtrip.py` (1.6). SRAM 0.

**P2** (S, Level C). Files: `app/src/og_samples.cpp`, `audio/include/o3os/audio/voices.h` (`SampleDesc::slice_thru`, `beat_slice_span`), `model/include/o3os/model/params.h` (0x1D), `model/src/params.cpp` (`play_thru_desc`), `app/src/module_ui.cpp`, `app/src/param_actions.cpp`, `app/src/screens/params_screen.cpp`, `docs/qa/catalogue/instrument.md`, `docs/manual/instrument.md` (the slice section: one paragraph), sim scenario `sim/scenarios/qa/qa_instrument.txt` › `qa_ins_280`..`qa_ins_283`. Tests: `audio_voices_test` and `amiga_voices_test` (`beat_slice_span` with the flag: the last bound is the length; a forced slice too), `og_samples_test` (Slice mode `length` = frames − start with the flag; Beat Slice passes the flag), `instrument_source_test` (the row in Slice and Beat Slice only, after SLICE; seven rows at most; one undo record; the slot rebinds). QA rows QA-INS-280..283: the row's place and values (sim), toggling plays on (sim render: note length), Slice FX with Play Thru (sim), save and reload keeps it (sim). Render CPU 0.

## 5. Unit check

After P2, hands: a Beat Slice loop of 8 slices with PLAY THRU ON, a held pad plays from its slice to the end; OFF stops at the next slice; a project and a `.pti` saved here keep it after a reboot.

## 6. Assumptions (decided here; local, testable, reversible)

- A1. Native reader: a flag value above 1 reads 0 (as the stereo byte); the OG reader follows tracker-ce (any non-zero is On), because OG files come from tracker-ce.
- A2. The flag is kept when the play mode changes and ignored outside the two slice modes (tracker-ce stores it the same way, F1).
- A3. The row is shown only in the slice modes; INIT of the instrument sets it Off.
- A4. Play Thru does not change the slice table, the Wave screen or the chop tools.

## 7. Risks

- R1 If official 1.9.x read byte 279 as something (F3 is an inference), an exported instrument with ON could change on the official firmware; F8 (0 in 70 of 70 files) makes that unlikely; OS-CECHECK (a) confirms.
- R2 The INST minor race with OS-CEWTENV: the rule in 3.1 and the round-trip tests catch a clash.

## 8. Evidence labels

- The setting, its default, its meaning in both slice modes, reverse from the sample end, any non-zero byte read as On: **verified parity** (F1, F4, F5).
- Its file byte 279: **open research question** until OS-CECHECK (a) surveys a tracker-ce file (F3, F7).
- The SOURCE page row, INST 1.6, a native value above 1 reading 0: **new Community OS behaviour**.

## 9. State for implementer

**P1.** Start with: `Instrument::reserved` `model/include/o3os/model/instrument.h:139`; `instr::kDelaySend` `formats/src/og_codec.h:308`; `inst_version` `formats/src/native_writer.cpp:44`; the stereo read `formats/src/native_reader.cpp:749`. Rules: OG files without the flag stay byte-identical on write; INST 1.5 and older stay byte-identical for every instrument without the flag; the struct size static_assert stays 720; Level C: risk review plus one independent review.

**P2.** Start with: `slice_points` `app/src/og_samples.cpp:68`; `beat_slice_span` `audio/include/o3os/audio/voices.h:555`; `instrument_param::PlayStereo` `model/include/o3os/model/params.h:75` and `write_play_stereo` `app/src/param_actions.cpp:493`; the source rows `app/src/screens/params_screen.cpp:212`. Rules: no read outside [0, length); nothing new per block; `sizeof(SampleDesc)` unchanged; the row is not learnable and has no generation slot.
