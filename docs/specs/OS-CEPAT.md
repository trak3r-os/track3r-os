# OS-CEPAT: read tracker-ce v6 patterns (OS-CEFXIDS is slice 0)

Status: spec, 2026-11-05, for the maintainer's decisions. Slices S0 and S1 are Level C (persistent formats). Docs only; nothing in the tree was changed. Our facts come from dev a6fc0b57. The tracker-ce facts come from `tracker-ce@7fd6c2c` (Polyend's published firmware source, cited as facts; no code taken).

## Maintainer decisions

**Decided by the maintainer 2026-11-05: every recommended default (Q1 a: remap at the OG boundary, OG 43..74 stored as 96..127; Q2 a: micromove into a free FX column as FX 2; Q3 a with c meanwhile: the maintainer saves the tracker-ce fixture on the unit, synthetic files until then; Q4 a: drop and count on v5 Export; Q5 a: an IDEA row OS-CEPATW).**

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| Q1 | OS-CEFXIDS: how to stop tracker-ce FX ids (43..50 today) from meaning our core FX 43..62 | (a) Remap at the OG boundary. An OG id 43..74 is stored in the model and `.o3pj` as 96..127 (OG id + 53). OG ids 75..127 are cleared and counted. Our core FX stay at 43..62, with room to grow in 63..95. (b) Move our core FX to a high block (for example 80..99) and keep OG ids verbatim. This needs PATT 1.0 → 2.0 with a loader that translates 43..62, older builds refuse the new files, and about 15 files change (fx.h, fx_table, tiles, fx_text, the MOD/S3M/MED constants, `o3project.py`, tests). | (a) | No change to the native layout or version: reserved ids get a meaning, as OS-COREFX did. Older builds keep the new ids inert and verbatim. The importers, sequencer and editor do not change. Size S, not M. |
| Q2 | Where the v6 micromove column `m` goes | (a) Into a free FX column as FX 2 Micro-move. It is dropped and counted when no column is free or when the other column holds Random value 16. When a column already holds FX 2, `m` is superseded: tracker-ce's FX column wins, so nothing audible is lost. (b) A new step field and editor column: Step grows from 7 to 8 bytes, PATT major bump, model arena +262,144 B, editor, clipboard, undo, and the module ABI's step POD all change. Size L. (c) A hidden per-pattern chunk that plays but cannot be edited. | (a) | It plays the same and needs no format change. REC MICRO (OS-RECMICRO) already writes micro-moves to FX 2. |
| Q3 | Where the tracker-ce-saved fixture (OS-CECHECK input) comes from | (a) The maintainer saves a test project with tracker-ce on his unit, through the official update path and back again, following the recipe in S3. (b) Ask a public tracker-ce user for one, with a licence grant. (c) Synthetic files only for now. | (a), with (c) until it arrives | S0 to S2 merge on synthetic files built from the documented layout. v6 stays marked "from the source" until a real file is surveyed. |
| Q4 | v5 Export of tracker-ce FX (model 96..127) | (a) Drop and count them in a new `ce_fx` counter, summed into the existing FX line of the Export screen. (b) Write the OG id (43..74): tracker-ce would play it, but what official 1.9 does with ids 43..50 is unknown. | (a) | The Export target is official 1.9.x, which has no such FX. |
| Q5 | A v6 writer | (a) Open an IDEA row OS-CEPATW now, ordered after OS-CEWTENV. (b) Not planned. | (a) | Reading is enough for compatibility. Writing v6 is a product choice: the folder would then stop opening on official 1.9. |

## Assumptions (decided here; technical, reversible)

- **A1.** v6 is a known version. `kPatternVerified` goes from 5 to 6 on the source evidence, and the S3 fixture survey confirms it. A v7+ file whose declared size is a v6 size is read with the v6 layout and flagged `unverified_version` (ADR 0006 amendment 1). Any other size is refused.
- **A2.** Framing:
  - For v ≥ 6, the declared size (u16 LE at offset 12) gives the layout. It must be 8,232, 12,332 or 16,432 (8, 12 or 16 tracks) and must equal the length of the stream.
  - For v ≤ 5, the stream detection of today stays and is extended to 16 tracks (12,336 B): after row 12 comes either the CRC and the end, or a 13th row.
- **A3.** The CRC is enforced for v6, as for v1..v5 today (OS policy). tracker-ce writes it but never checks it (`tracker-ce@7fd6c2c:core/FileManager/fileManagerPattern.cpp:302`).
- **A4.** A v ≤ 5 file without a CRC (tracker-ce accepts one whose length + 4 equals the declared size) stays refused. This is recorded as an open question; no such file is known.
- **A5.** Volume:
  - a negative value → `Step::vol` 255 (unset);
  - 0..100 → `floor(v × 127 / 100)`, the same as `velocity_from_fx`;
  - 101..127 → 127, counted as sanitised.
  - A future v6 writer inverts this with `ceil(vol × 100 / 127)`, i.e. `(vol × 100 + 126) / 127`, which is lossless over 0..100 (checked numerically).
- **A6.** Micromove:
  - The value goes verbatim into the first FX slot of type 0 (`fx[0]`, then `fx[1]`), with the Q2 (a) exceptions.
  - Values 101..127 are kept as stored; `fx_sanitize` plays them as 100.
- **A7.** The id map of Q1 (a) applies to every OG pattern version (v1..v6). On export:
  - model 43..95 are dropped as `core_fx`, as today;
  - model 96..127 are dropped as `ce_fx`;
  - engine FX 128..255 are dropped as `engine_fx`, as today.
- **A8.** All 16 tracks of a v6 file go into the model's 16 tracks. Tracks the file does not hold take track 1's length (already the case for 8-track files, `formats/src/og_pattern.cpp:92`).
- **A9.** The header's type, firmware bytes, tempo, swing and reserved bytes are ignored, as today.
- **A10.** The pattern reader reports three new counters:
  - `micromoves_dropped`;
  - `ce_fx_cleared` (OG ids 75..127);
  - `values_clamped` (volume above 100).
  `OgFolderLoader` sums them into `OgLoadReport`, and the load strip names them (decided in S2): `n MICRO-MOVES NOT KEPT`, and the clamped volumes and the cleared ce FX together as one `n VALUES CHANGED`.
- **A11.** The reader's row buffer grows from 769 to 1,025 B (+256 B). The writer keeps writing v5 rows of 769 B.
- **A12.** The pattern sniffer accepts 6,184, 9,260 and 12,336 (v5) and 8,232, 12,332 and 16,432 (v6).
- **A13.** The source folder is never written. Saving a loaded folder adds `project.o3pj` only (QA-FIL-104), so tracker-ce can still open the folder.

## 1. Goal

A project folder saved by tracker-ce (pattern structure v6) opens in this OS. Its notes, instruments, FX, volume column and micro-move column play as they did on tracker-ce, as far as this OS has the features. Nothing is lost silently. tracker-ce FX that this OS does not have yet (Amp Env, Filter Env, Granular Length) are kept inert and shown as `?` until OS-CEWTENV gives them a meaning. Native projects keep their meaning and their bytes.

Out of scope:
- writing v6 (Q5);
- the WT envelopes themselves (OS-CEWTENV);
- tracker-ce project v17 and instrument v9 fields (OS-CECHECK).

## 2. Facts

### 2.1 The tracker-ce v6 pattern file

- **Versions.** Pattern 6, project 17, instrument 9 (`tracker-ce@7fd6c2c:core/source/mtStructs.h:43-45`).
- **Track count.** Always 16 tracks, 8 sample + 8 MIDI, on every device (`tracker-ce@7fd6c2c:core/source/mtStructs.h:87-90`; `static_assert` at `core/source/mtSequencer.h:348`).
- **Layout** (`tracker-ce@7fd6c2c:core/source/mtSequencer.h:282-336`):
  - a 16-byte header: `KS`, type u16, firmware [4], structure version [4] at offset 8, size u16 at offset 12, 2 padding bytes;
  - tempo f32, swing f32 and 4 reserved bytes, which make a 28-byte head;
  - 16 tracks of one length byte plus 128 steps of 8 bytes, 1,025 B each;
  - a CRC-32 over everything before it.
  - A step is note i8, instrument u8, fx[2] (type u8, value u8), volume i8, micromove i8 (`:305-316`). −1 means none for both volume and micromove (`:40-50`, `fileManagerPattern.cpp:526-559`).
- **Sizes.**
  - v6: 28 + n × 1,025 + 4 = 8,232 / 12,332 / 16,432 B.
  - v5: 6,184 / 9,260 / 12,336 B (`tracker-ce@7fd6c2c:core/include/patternFileValidation.h:9-15`).
- **Writing.** tracker-ce always writes the 16-track v6 file. The CRC covers everything before it (`fileManagerPattern.cpp:276-283`), using FastCRC32, the standard CRC-32 (`platforms/mk66/source/CrcCalculatorImpl.cpp`). The header is `KS`, with 6 in all four version bytes and size = sizeof (`core/source/mtSequencer_patternEdit.cpp:1367-1385`).
- **Loading.**
  - tracker-ce checks sizes only, never the CRC (`fileManagerPattern.cpp:302-316`, `patternFileValidation.h:24-59`).
  - A v ≤ 5 file without a CRC is accepted when its length + 4 equals its declared size (`patternFileValidation.h:52-56`).
  - v5 is upgraded in memory with volume and micromove set to −1 (`fileManagerPattern.cpp:526-559`).
  - Tracks a file does not hold are cleared and take track 1's length (`:561-591`).
- **Notice.** Saving on tracker-ce converts a project to v6, which older firmware cannot read (`tracker-ce@7fd6c2c:CHANGELOG.md:8-14`).
- **No known writer for the 8- and 12-track v6 sizes.** tracker-ce accepts them but never writes them.

### 2.2 tracker-ce FX ids (and a correction to the backlog)

- **The enum** (`tracker-ce@7fd6c2c:core/source/mtSequencer.h:122-186`) ends:
  - `SLIDE_DOWN` = 42, matching our `kSlideDown` (`seq/include/o3os/seq/fx.h:77`);
  - `R30` = `AMP_ENV` = **43** (`:172-173`);
  - `R31` = `FILTER_ENV` = **44** (`:174-175`);
  - `R1` = `GRANULAR_LENGTH` = **45** (`:178-179`);
  - `R2`, `R3`, `R7`, `R8`, `R9` = 46..50 (`:180-184`). A comment at `:177` calls the `R` names leftovers of old effects, kept so that nothing falls apart.
- **The backlog is off by one.** The OS-CEFXIDS row says "42..44"; the new ids are **43..45**. They collide exactly with our 43 Speed, 44 Tempo fine and 45 Order jump (`seq/include/o3os/seq/fx.h:78-80`). Our 46..62 (OS-COREFX tracker FX) sit on tracker-ce's reserved R ids and on its path for new ids.
- **Value ranges** (`core/source/mtSequencer_patternEdit.cpp:1745-1790`, `core/interface/core/interfaceFxDesc.h:229-246`, `CHANGELOG.md:28-32, 64-67`):
  - Amp Env and Filter Env take 0..255: two hex nibbles A (attack, 0 = 0 ms .. F = 8 s) and R (decay/release);
  - Granular Length takes 0..100: 0-50 % → 1-100 ms, 50-100 % → 100-1,000 ms;
  - the step letters are `w`, `y` and `z`.
- **Wider ranges on existing ids** (record only): Chance up to 174 (play/skip cycles), Chord up to 67 (inversions; ours 0..29) (`mtSequencer_patternEdit.cpp:1745-1790`).
- **Playback.** The audio engine handles Granular Length and ignores R9 (`core/AudioEngine/mtAudioSequencer.cpp:47, 50`). The UI list shows the three new FX (`core/interface/core/interfaceDefs.h:400-402`).

### 2.3 How tracker-ce plays the two columns

- **Order of work.** The micromove column, then the volume column (`tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:471-486`), then the FX columns, `fx[1]` before `fx[0]` (`:490`). An FX column therefore overrides both columns:
  - FX 2 Micro-move: `:518-527`, which also calls `killFxOnSlot`;
  - FX 18 Volume: `:533-541`;
  - Random volume: `:707-716`.
- **Volume column.**
  - On a step with a note, it sets the step's volume, as FX 18 does (`:477-482`).
  - On an empty step of a sample track, it calls `setStepVelocity` on the playing voice (`:483-486`).
  - A MIDI note's velocity is `mapClamped(volume, 0, 100, 0, 127)`, an integer floor (`:2445`; `core/source/util_math.h:17-21`).
- **Micromove column and FX 2** both map the value with `map(v + 1, 0, 100, 1, 48)`, then clamp 0..96 (`:472-473`, `:524`).
- **Editing.** Both columns clamp to 0..100 when edited (`mtSequencer_patternEdit.cpp:849, 916`). The `[m | V]` view swaps the FX columns for them (`core/interface/patternEditor/fPatternEdit.cpp:770-800`). Record Micro-move and Record Velocity write into them (`CHANGELOG.md:57-62`).

### 2.4 This OS today

- **`model::Step`** is 7 bytes: note, instr, vol (0..127 velocity, 255 unset) and fx[2] (`model/include/o3os/model/step.h:28-40, 60`). `fx_class` returns Og for 0..42, Reserved for 43..127 and Engine for 128..255 (`:38-40`).
- **FX ids.**
  - 0..42 are the OG ids; 43..45 are the importers' Speed, Tempo fine and Order jump; 46..62 are the tracker FX (`seq/include/o3os/seq/fx.h:77-101`).
  - The table has 63 rows (`:148`). `fx_kind` makes types 63..127 Inert (`:218`), and `fx_sanitize` turns an Inert type into (0, 0) where it is used (`:235`).
  - ADR 0004 amendments 1 and 2 assigned 43..45 and 46..62 without a version bump, because the ids were reserved and inert.
- **Micro-move.** `micromove_start(v) = (v + 1) × 48 / 101 + 1`, held to 1..47 (`fx.h:265`). `velocity_from_fx(v) = min(v, 100) × 127 / 100` (`fx.h:316`).
- **The sequencer.**
  - A note starts with `vol` (or the default when it is unset) (`seq/src/sequencer.cpp:1628`).
  - FX 18 overrides it (`:1853-1858`).
  - FX 2 sets the micro-move (`:1847`).
  - Random value 16 randomises the other column's value (`:1820-1834`).
- **The OG pattern reader** (`formats/src/og_pattern.cpp`).
  - It accepts `KS` or `PM` magic, and flags `unverified_version` above `kPatternVerified` = 5 (`:54`; `formats/include/o3os/formats/og.h:35`).
  - It reads rows of 769 B (`og.h:47`) and tells 8 from 12 tracks by the stream (`og_pattern.cpp:138-147`).
  - It enforces the CRC and ignores the declared size.
  - It copies FX verbatim and leaves `vol` unset (`formats/src/og_codec.h:343`).
  - A v6 file fails today, on the CRC or on its framing. `OgFolderLoader` counts it in `patterns_failed` and goes on (`app/include/o3os/app/og_folder.h:70`).
- **The sniffer** accepts sizes 0, 6,184 and 9,260 only (`formats/src/builtin_formats.cpp:51-54`).
- **OG export** (`formats/src/og_codec.h:361-381`) writes v5 with 12 tracks.
  - FX 43..127 → (0, 0), counted in `core_fx`; 128..255 → `engine_fx` (`formats/include/o3os/formats/og.h:245-249`).
  - `vol` becomes FX 18 through `volume_to_fx18 = (v × 100 + 63) / 127` (`og_codec.h:354`).
  - The screen sums these losses into one FX line (`app/src/og_export.cpp:141`).
- **The module SDK's step POD** documents types 1..42 and 128..255, with 43..127 reserved and refused on a write (`module/sdk/o3module.h:412, 425`).
- **FX glyphs.** A type without a name is drawn as `?` plus its value (`app/src/pattern_data.cpp:12-26`). There is one RAM_G tile per id below 63 (`ui/include/o3os/ui/assets.h:92`).

## 3. Design

### 3.1 OS-CEFXIDS (slice 0): the id map

Q1 (a). An OG id `t` read from any OG pattern version:

| OG id | Model / `.o3pj` id | Note | Label |
|---|---|---|---|
| 0..42 | the same | the shared OG ids | verified parity (`tracker-ce@7fd6c2c:core/source/mtSequencer.h:122-171` against `seq/include/o3os/seq/fx.h:38-77`) |
| 43..74 | `t + 53` = 96..127 | tracker-ce FX, Inert until defined; shown as `?` and the value; kept on save | new Community OS behaviour |
| 75..127 | (0, 0) | counted as `ce_fx_cleared` | new Community OS behaviour |
| 128..255 | the same | engine FX, as today | unchanged |

- **What the native ids now mean.** In `.o3pj` and the model, 43..95 are core FX (43..62 assigned, 63..95 free), 96..127 are tracker-ce FX (OG id + 53), and 128..255 are engine FX.
- **No layout change.** 96..127 were reserved and inert, and an older build already keeps them verbatim and plays nothing. This is the same rule as ADR 0004 amendment 2, so PATT stays at 1.0. It is recorded as ADR 0004 amendment 3.
- **Export.** 96..127 → (0, 0), counted in `ce_fx` (Q4 a) and summed into the existing FX line (`app/src/og_export.cpp:141`).
- **The module ABI does not change.** Modules never see 96..127 as meaningful; they are still "reserved, refused on a write".
- **A side fix.** Until now an OG file holding 43..62 played as our core FX: Speed, Tempo fine, Order jump and the tracker FX. After S0 those ids are inert.

### 3.2 v6 framing

The head is 28 bytes, then n rows of 1,025 B, then 4 bytes of CRC.

- **Choosing the layout.** On the head, the reader reads the version at offset 8 and, for v ≥ 6, the declared size at offset 12.
  - The size must be one of the three v6 sizes, and it gives n.
  - Otherwise the file is refused with `Malformed`: "v6 pattern of an unknown size".
- **Checks.**
  - The stream must end exactly after the CRC: bytes left over are `Malformed`, a short file is `Truncated`.
  - The CRC is checked (A3).
- **Track lengths.** The length byte must be ≤ 127, as today. Tracks n+1..16 take track 1's length (A8).
- **Memory.** The row buffer grows to 1,025 B (A11). The streaming state machine keeps its shape; only the row size and the end condition depend on the version.

### 3.3 Step mapping (v6 → model)

| v6 field | Model | Label |
|---|---|---|
| note, instrument | as v5 (`step_from_og`) | verified parity (unchanged) |
| fx[0], fx[1] | type through the id map of 3.1; value verbatim | see 3.1 |
| volume < 0 | `vol` = 255 | verified parity: −1 is none (`tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:477`) |
| volume 0..100 | `vol` = `floor(v × 127 / 100)` | **Verified parity for MIDI velocity**: tracker-ce sends `floor(v × 127 / 100)` (`mtSequencer.cpp:2445`), and this OS sends `vol`. **Sample tracks** play the value as an FX 18 of the same value, since `velocity_from_fx` is the same formula (`fx.h:316`); that inherits FX 18's own parity. **The FX column wins over the volume column** on both sides: verified parity (`mtSequencer.cpp:490` vs `seq/src/sequencer.cpp:1853`). |
| volume 101..127 | `vol` = 127, counted in `values_clamped` | new Community OS behaviour (tracker-ce never writes it; its editor clamps to 100) |
| volume on a step with no note | kept in `vol`, plays nothing | **open research question**: tracker-ce changes the playing sample voice's velocity (`mtSequencer.cpp:483-486`). This OS ignores `vol` without a note. |
| micromove < 0 | nothing | verified parity |
| micromove 0..127, a free FX slot, no FX 2 and no Random value 16 in the other slot | FX 2 with the same value in the first free slot | new Community OS behaviour (placement). The timing is the FX 2 path's: see the formula difference below. |
| micromove, one slot already FX 2 | not stored; superseded | verified parity: tracker-ce's FX column runs after the column and wins (`mtSequencer.cpp:471-474, 518-527`) |
| micromove, no free slot, or the other slot holds Random value 16 | not stored; counted in `micromoves_dropped` | new Community OS behaviour (a listed loss). With Random value 16, our sequencer would randomise the moved value (`seq/src/sequencer.cpp:1820-1834`). |

- **The micro-move formula differs (open research question, existing).** tracker-ce maps `(v + 1) × 47 / 100 + 1` (up to 48). This OS maps `(v + 1) × 48 / 101 + 1`, held to 47 (`fx.h:265`). They differ by one microstep on 27 of the 101 values (16, 18, 33, ...). This already holds for FX 2 read from v5. It is recorded for OS-CECHECK and not changed here.
- **Edge (documented).** As an FX-column Micro-move, `m` behaves like FX 2 in our sequencer, including how FX 2 interacts with a held voice FX in that slot. tracker-ce's FX-column Micro-move calls `killFxOnSlot` (`mtSequencer.cpp:520`), but its column path does not. The difference is audible only when a Voice FX is held in the slot that `m` lands in, which needs that slot to be empty on this step: a held FX carried from an earlier step. Recorded for the S3 survey.

### 3.4 16-track v5

- tracker-ce reads v5 files of 12,336 B (`patternFileValidation.h:12`). This OS extends the stream detection: after row 12, a 4-byte CRC then the end means 12 tracks; a 13th row means 16.
- The sniffer accepts 12,336.
- Tracks 13..16 go into the model's tracks 13..16. OG-16TRACK-MTP is folded here.

### 3.5 Report and UI (slice S2)

- **Load.** The three counters of A10 are summed into `OgLoadReport`. The load strip shows them after the existing parts, e.g. `3 MICRO-MOVES NOT KEPT`; values clamped and ce FX cleared share one `n VALUES CHANGED`.
- **Pattern view.** A tracker-ce FX (96..127) shows `?` and its value, which is already the rule for unnamed types (`app/src/pattern_data.cpp:12-26`). It plays nothing.
- **Export.** The FX line also counts `ce_fx`.

### 3.6 What older builds of this OS do

- **A v6 folder.** Its patterns are still refused and counted as failed; project.mt and the instruments load.
- **A `.o3pj` saved by the new build** that holds 96..127: an older build keeps those ids verbatim, Inert, shown as `?`. Byte identity is tested.

## 4. Format changes and tests

| File | Change | Version | Loader | Tests |
|---|---|---|---|---|
| OG `.mtp` (read) | v6 read; 16-track v5 read | none (we read their format exactly) | the reader of 3.2 and 3.4 | `og_pattern_test`, `og_hostile_test`, `og_roundtrip_test`, `og_folder_test` |
| OG `.mtp` (write, Export) | none: still v5 with 12 tracks; adds the `ce_fx` count | none | n/a | `og_export_test`: tracker-ce ids are dropped and counted |
| Native `.o3pj` PATT | meaning of reserved ids 96..127 assigned (ADR 0004 amendment 3) | none: PATT stays 1.0 (precedent: amendment 2) | n/a: older builds keep them verbatim | a `.o3pj` holding 96..127 round-trips byte-identically; `o3project_roundtrip.py` unchanged |

Round trips:
- v6 → model → `.o3pj` → model is equal, with the micro-move in FX 2.
- v6 → model → v5 Export loses exactly the counted items.
- v5 → model → v5 Export keeps what `og_roundtrip_test` checks today (unchanged).

What the OS-CECHECK fixtures need (S3; Q3):
- one tracker-ce-saved project (`Projects/t-ce1`, set `fixtures-2` through `tools/testdata/testdata.py`, licence `Maintainer-own`);
- patterns that hold:
  - volume 0, 1, 50 and 100;
  - m 0, 50 and 100;
  - both FX columns full plus m;
  - FX 2 plus m;
  - FX 43, 44 and 45;
  - data on tracks 13-16;
  - tracks of different lengths;
  - a volume on an empty sample step;
  - a held Voice FX followed by an m on an empty slot;
- the same project's v17 `project.mt` and one v9 instrument, for OS-CECHECK (a);
- a short audio recording of the m and V steps, for the formula questions.

## 5. Slices

| Slice | Size, level | Files | Acceptance | Host tests | QA rows | Flash / SRAM / CPU |
|---|---|---|---|---|---|---|
| **S0 OS-CEFXIDS** | S, Level C | `formats/src/og_codec.h` (the id map in `step_from_og`, the `ce_fx` rule in `step_to_og`), `formats/include/o3os/formats/og.h` (`ExportDrops::ce_fx`), `model/include/o3os/model/step.h` (constants `kFxCeFirst` 96, `kFxCeOffset` 53, `kFxOgCeLast` 74; comments), `app/src/og_export.cpp:141`, `docs/adr/0004-step-format.md` amendment 3, `docs/formats/og.md` sections 4 and 7, `docs/formats/o3-project.md` PATT line | 3.1 holds for v1..v5; Export drops and counts 96..127; native files are unchanged | `og_pattern_test`: OG 42 → 42, 43 → 96, 50 → 103, 74 → 127, 75 and 127 → (0, 0) counted, 128 verbatim. `og_export_test`: `ce_fx` and `core_fx` separate. `model_test`: `fx_class` unchanged. A `.o3pj` with 96..127 is byte-identical after load and save. `og_fixture_survey_test` counts OG FX > 42 (expected 0 on `fixtures-1`). | QA-FIL-139: an OG pattern with FX 43 opens with `?` and the value, and plays nothing | about +150 B flash, 0 SRAM, load and export time only |
| **S1 v6 reader, 16-track v5** | M, Level C | `formats/include/o3os/formats/og.h` (v6 constants, row 1,025, `kPatternVerified` 6), `formats/src/og_pattern.cpp` (v6 path, 16-track stream detection, vol and m mapping, counters), `formats/src/og_codec.h` (`step_from_og_v6`), `formats/src/builtin_formats.cpp` (sniffer), `docs/formats/og.md` sections 2, 4, 9 and 10 | A synthetic v6 file of 8, 12 or 16 tracks loads as section 3 says; a 12,336 B v5 file loads 16 tracks; hostile input is refused with the pattern removed; the counters are right | `tests/support/og_fixture.h` gains a v6 builder from the byte map. `og_pattern_test`: 8, 12 and 16 tracks; every row of 3.3. `og_hostile_test`: wrong declared size, v6 at a v5 size, v5 at a v6 size, truncated, one extra byte, bad CRC, length byte > 127, volume and m of 127 and −2. `og_roundtrip_test`: v6 → `.o3pj` → model is equal, and v6 → v5 Export loses only what is counted. `og_folder_test`: a folder with v6 patterns. | QA-FIL-140: a tracker-ce project opens with its V and m columns playing. QA-FIL-141: a 16-track v5 pattern opens with tracks 13-16. | about +1.0-1.5 KB flash; +256 B in the reader object (measure where it sits: `.bss` or SDRAM); load time only |
| **S2 Load report, strip, QA** | S, Level B | `app/include/o3os/app/og_folder.h` (`OgLoadReport` counters), `app/src/og_folder.cpp`, the strip text where the load report is drawn, `docs/qa/catalogue/file.md` and `pattern.md`, `sim` scenario `qa_file_ce.json` | The counters reach the strip; a damaged v6 pattern is counted while the rest of the folder loads | `og_folder_test`: the sums; `overlay_test` or the sim strip check | QA-FIL-145: a damaged v6 pattern is counted and the rest loads. QA-FIL-146: the strip names dropped micro-moves. QA-PAT-335: a tracker-ce FX shows `?` and the value and plays nothing. | about +300 B flash (strings), +12 B SRAM, none per block |
| **S3 tracker-ce fixture survey (OS-CECHECK input)** | S, Level C verification; waits on Q3 | `tools/testdata/` manifest (`fixtures-2`), `tests/og_fixture_survey_test.cpp`, `docs/formats/og.md` section 10 (evidence) | The real file reads as the synthetic one; any difference becomes a fix row; v6 changes from "from the source" to verified | `og_fixture_survey_test` skips without the set | QA-FIL-140 and 141 move from sim to unit | 0 firmware |

S0 merges first. S1 needs S0 so that v6 ids land on the right model ids. S2 follows S1. S3 is independent of the code and closes the evidence. Unit check after S1/S2: open a tracker-ce folder (or a synthetic v6 one) on the unit, and play the V and m steps against the same project on tracker-ce, when Q3 allows.

## 6. What OS-CEWTENV needs (not designed here)

- The ids: Amp Env = model 96, Filter Env = 97, Granular Length = 98 (OG + 53).
- FX table rows for 96..98: `seq/include/o3os/seq/fx.h` `kFxInfo` stops at 63 rows, so it needs a small second table or a wider one. Also `model/include/o3os/model/fx_table.h` (picker), `fx_kind`, and `fx_sanitize` ranges (0..255 for the envelopes, 0..100 for the granular length).
- FX tiles: 96..98 → tile indices 63..65 (`ui/include/o3os/ui/assets.h:92`, RAM_G budget), the fx_text names, and the step letters (`w`, `y`, `z` on tracker-ce).
- The value semantics: AR nibbles with A in 0 ms .. 8 s, and the granular length curve (`tracker-ce@7fd6c2c:core/interface/core/interfaceFxDesc.h:229-246`).
- The back-map 96 → 43 for a v6 writer (Q5), and the Q4 Export rule if official 1.9 support ever changes.
- The instrument v9 wavetable envelope fields, from OS-CECHECK (a).
- The CPU cost measured on the unit (per-voice envelopes on the wavetable and granular paths).

## 7. Risks and recorded findings

1. **The backlog's OS-CEFXIDS row** says 42..44; the real ids are 43..45. The backlog owner corrects the row.
2. **No real v6 file has been read yet.** The layout is from the source. Mitigation: synthetic fixtures from the documented byte map, and S3.
3. **8,232 and 12,332 B v6 files have no known writer.** They are read because tracker-ce reads them, and are covered by synthetic tests only.
4. **Behaviour gaps (open research questions):**
   - the volume on a no-note step (3.3);
   - the micro-move formula, one microstep on 27 values;
   - the `killFxOnSlot` edge (3.3);
   - Chance values up to 174 and Chord values up to 67, which are played clamped by `fx_sanitize`;
   - what official 1.9 does with ids 43..50.
5. **A latent issue in v5 Export (recorded, not fixed here).** `volume_to_fx18` rounds, so `vol` = `floor(w × 127 / 100)` exports back to `w − 1` for 36 of the 101 values. The exact inverse is `ceil(vol × 100 / 127)` (A5). It is a one-line fix in its own bug row, and it changes Export bytes for projects with a volume column, so their goldens move.
6. **OG ids 43..127 in existing native files.** A native project imported from an OG file before S0 may hold OG junk 43..62 as core FX, or 96..127 as ids that S0 now names tracker-ce. Official 1.9 writes neither, so this is expected to be empty. After OS-CEWTENV, a stray 96..98 in such a file would start to play. The S0 survey count (expected 0) documents it.
7. **Memory.** The reader's buffer grows by +256 B. Measure the reader object's placement in S1.
8. **The rest of the folder.** project.mt v17 is read as "observed only"; a v17 layout difference ends the folder load. That is OS-CECHECK (a), not this item. A v6 folder can still fail there until OS-CECHECK lands.
9. **A legacy CRC-less v ≤ 5 file** stays refused (A4). Open, and no such file is known.

## 8. State for implementer

- **Read first:**
  - `formats/src/og_pattern.cpp` (the state machine, `:42-175`);
  - `formats/src/og_codec.h:343-385`;
  - `formats/include/o3os/formats/og.h:35-58, 245-262`;
  - `tests/support/og_fixture.h` (`empty_pattern`, `rich_pattern`, `pattern_bytes`).
  Use `python3 -I tools/code_map.py` and LSP before grep.
- **S0:**
  - Add the map in one place, a `fx_from_og(type)` next to `step_from_og`, and use it for every version.
  - Existing tests that read OG FX > 42 verbatim must change. Check `tests/og_pattern_test.cpp:528` (`core_fx == 9`) for model ids that move to `ce_fx`.
  - `fx_class` and the SDK text stay as they are.
- **S1:**
  - Branch on the version in `on_head`. For v ≥ 6, read the declared size at offset 12 and fix n. Keep the v ≤ 5 stream detection, adding the row-12 fork.
  - Each step is mapped from 8 bytes: bytes 0-5 as v5, then volume (byte 6) and micromove (byte 7) as in 3.3.
  - The CRC covers bytes 0..size−5.
  - There is no tracker-ce sample file in the repo. Build every fixture from the byte map in 2.1; never copy tracker-ce code.
- **Evidence labels.** Every new row in `docs/formats/og.md` carries the label given in 3.1 and 3.3, and cites `tracker-ce@7fd6c2c:<path>:<line>`.
- **Gates.**
  - Scoped ctest: `og_` and `model_`.
  - Level C: a risk review plus one independent reviewer.
  - One `tools/ci.sh host` per branch.
  - No qemu gate is needed: there is no DSP or module change.
- **Remaining hardware check:** open a v6 folder on the unit and listen to the V and m steps (S3 / Q3).
