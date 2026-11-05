# OS-SCALES28: the 27 other scales on the held-Note panel, and NOTE 1.1

Level C: a persistent format change (the `NOTE` chunk of the native project, `docs/formats/o3-project.md` section 3, "The session state"). New Community OS behaviour on top of an official list: the scales are the official firmware's 39, taken as facts from tracker-ce (no code taken). Nothing runs on the audio path or in an interrupt: every reader of the scale is the main loop.

Backlog row: "Scales: the 28 other scales" (APPROVED, maintainer 2026-11-05, IDEA triage). The count: our scale table has 40 rows, the panel offers 12, so 28 are left; one of them is "No scale", which has Chromatic's notes. The panel therefore gains **27 scales** and offers **39**, the official list.

## Decisions for the maintainer

**Decided by the maintainer 2026-11-05: every recommended default (Q1 a: NOTE 1.1 appends one byte; Q2 a; Q3 a; Q4 c: one name table shared with Fill; Q5 a and Q6 a: the OG scale byte, Random Note in scale and Fill opening on the NOTE key are separate IDEA rows).** Slices S3 and S4 are therefore not built.

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| 1 | How NOTE 1.1 stores a scale outside the twelve (format) | a) **append one byte** after `jump`: the scale in the official order 0..38. The 1.0 `scale` byte keeps its meaning (the panel's twelve) and holds 0 (Chromatic) when the scale is not one of them. 1.1 is written only for those 27 scales · b) give the `scale` byte the meaning 0..38 under NOTE **2.0**: every published build refuses the whole project (`NewerMajor`) · c) give the byte the new meaning under 1.1: a published build reads the new number with the old meaning (2 = Major becomes Minor), with no warning | a | It follows the format's own rule: a minor bump only appends (`o3-project.md` section 10), as `USBS` 1.1 and `INST` 1.5 did. Every file saved today stays the same, byte for byte. v1.0.0..v1.1.0 open a 1.1 file and play Chromatic, and the load line says "newer fields skipped". Only the scale is lost, and only if such a build saves the project |
| 2 | How the panel reaches 39 scales (UX) | a) pad row 2 keeps the twelve unchanged. While `Note` is held, **the encoder and `Up` / `Down` step through all 39**. The top line names the scale. No row-2 pad lights for one of the 27, as row 3 already does for a JUMP above 11 · b) row 2 becomes 4 pages of 12, and `Shift` + a row-2 pad (or the encoder) turns the page · c) a list of 39 names in the panel, like the FX list | a | Nothing a user already knows moves. Today the encoder and the arrows do nothing in the panel (`bump` returns false for Note), so no new binding is needed |
| 3 | The order the encoder walks (UX) | a) the official order, as a ring: Chromatic, Minor, Major, Dorian, Lydian, Lyd. minor, ... (the official Config "Notes scale" list, and our Fill KEY ring without No scale) · b) the twelve first, in the panel's order, then the other 27 | a | One order everywhere: the Fill dialog, the official firmware, an OG file's byte and our new byte |
| 4 | Names of the 27 (UX) | a) readable names in the panel's style, at most 11 characters (section 3.1: `Bebop major`, `Hung. minor`); the Fill dialog keeps the official short names · b) the official short names (`BeBopMaj`, `Hung Min`), as Fill shows them today · c) **a, and the Fill dialog shows the same names**: one table of 40 instead of two | c | One scale has one name across the OS, and one table is 64 B smaller than today's two. The cost is the Fill goldens that show a scale name, and the text of QA-PAT-304 |
| 5 | Scope: the scale byte of an OG project (0x115, in the same official order) | a) **not in this item**: an IDEA row for importing and exporting the OG key (scale, root, layout; mapping the root and the octave needs research) · b) import and export the scale byte here; the root stays out | a | It keeps the Level C slice to one chunk. The numbering of Q1 makes the later mapping a plain copy |
| 6 | Scope: official behaviours that read the scale (the Random Note FX stays in the scale, `tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:893-905`; Fill opens on the pad key, `tracker-ce@7fd6c2c:core/interface/patternEditor/gPatternEdit.cpp:581-586`) | a) **not in this item**: two IDEA rows · b) Fill opens on the NOTE key here (main loop, Level B) · c) b, plus Random Note in the scale (sequencer, Level C, CPU measured on the unit) | a | Each one changes what a project plays or how a dialog opens. Separate rows keep this item's review on the format |

### Assumptions (decided here: local, testable, reversible)

- **A1** The appended byte numbers the scales in the **official order 0..38** (tracker-ce's `scaleNames`). It does not use our core index 0..39, which has "No scale" at 0. The same number then means the same scale in an OG project's byte (F4), and our core index is always `official + 1`.
- **A2** In a 1.1 chunk the 1.0 `scale` byte is **0 (Chromatic)**. Chromatic contains every note, so an older build snaps no note to a pitch outside the user's scale. A "nearest of the twelve" would snap some notes wrongly.
- **A3** The writer writes NOTE **1.0, byte for byte as today, for the twelve**, and 1.1 (8 bytes) only for the 27. A 1.1 chunk that holds one of the twelve loads and is written back as 1.0. This is the rule of `PERF` 1.1, `APPM` 1.1 and `USBS` 1.1.
- **A4** In RAM, `model::NoteState::scale` and `app::NoteEntry::scale` hold the **official index 0..38** (`kNoteScaleMax` 38). Only the codec, in C++ and in Python, knows the 1.0 byte's panel numbering. The default 0 is Chromatic in both numberings.
- **A5** Reading a 1.1 chunk: the appended byte wins when it is 0..38. Above 38, the scale is the 1.0 byte's (mapped), and the value is counted in "N values reset". The 1.0 byte is checked field by field, as today. A newer minor (1.2 and later) is read as far as 1.1 goes.
- **A6** "No scale" is not offered on the panel; Chromatic is the same notes. It stays row 0 of the core table for the Fill dialog, where it means "no filter".
- **A7** The official list has duplicates, and we keep them: Hung. minor = Gypsy minor, and Diminished = Whole-half have the same notes. A musician looks a scale up by its name.
- **A8** Todi keeps the standard definition (OS-PTOOLS). The official row is a copy of its Phrygian (F5). The name and the number match the official ones; the notes do not.
- **A9** The encoder ring wraps (as Fill's KEY ring). `Up` is -1 and `Down` is +1, as in every popup list. The step is one scale per detent.
- **A10** Changing the scale is not an undo unit (as with the pads today). It marks the project unsaved through `App::sync_project_state`, as every NOTE change does.
- **A11** The table of the twelve (pad -> official index) lives in `model/project_state.h`, because it is a format fact: it is what the 1.0 byte means. `app/note_entry.h` uses it, so no second copy exists.
- **A12** `NoteEntry::og_scale()` is renamed `core_scale()`. It returns the core index, which is not the OG number (F7).

## 1. Goal

Make every scale of the official firmware usable for note entry. Today the panel (hold `Note` on Pattern) offers 12 scales. The other 27 are already in the OS, in the scale table that the Fill dialog and the pad board use, but the NOTE chunk can only store the twelve. After this item:

- the panel reaches all 39 scales;
- SCALE LOCK, the NOTES/PIANO/ISO pads, the MIDI keyboard's snap and the info line work for every one of them;
- a project stores and restores any of them;
- every project saved today opens unchanged, and a published build opens the new files.

## 2. Facts

Evidence labels: **verified parity** (with evidence) · **new Community OS behaviour** · **open research question**. A `tracker-ce@7fd6c2c:<path>:<line>` reference is evidence of the official firmware's source (`CLAUDE.md`). No code was taken.

| # | Fact | Evidence | Label |
|---|---|---|---|
| F1 | The official firmware has 39 scales, index 0..38 (`MAX_SCALE` 38). A scale is a 12-bit mask in which bit k is the note k semitones above the root, with the root always in. A scale above 38 is read as 38 | `tracker-ce@7fd6c2c:core/source/scales.h:6-7` (bounds), `:11-51` (masks), `:53-60` (names); `core/source/scales.cpp:11-12, 17-35` | verified parity (source) |
| F2 | An official project stores the pad scale as one byte, `padBoardScale`, after `actualPattern`, followed by the note offset, the root note and the max voices. A new project holds 0 (Chromatic). The official Config menu "Notes scale" lists the 39 names | `tracker-ce@7fd6c2c:core/source/mtStructs.h:846-851`; `core/FileManager/fileManagerProject.cpp:241`; `core/interface/configEditor/configMenu.cpp:66, 77` | verified parity (source) |
| F3 | The official sequencer's Random Note FX picks a note in the pad scale (up to 100 draws). Our Random Note ignores scales | `tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:893-905`; ours `seq/src/sequencer.cpp:2033-2037` | verified (difference); out of scope, Q6 |
| F4 | Our OG codec knows the byte at file offset 0x115 (`proj::kPadBoard`). The importer does not read it (it reads only 0x114, `report_.start_pattern`). The exporter writes 0 (Chromatic) there and the defaults 11, 24, 8 after it. So an OG project's key is lost on import today | `formats/src/og_codec.h:72`; `formats/src/og_project.cpp:150, 489-492`; `docs/formats/og.md:110` | fact of this tree; mapping is Q5 |
| F5 | Our core table (`core/scales.h`) is "No scale" followed by the official 39 in the official order: **core index = official index + 1**. 38 of the 39 masks are equal. **Todi differs**: the official Todi is a copy of its Phrygian (`0 1 3 5 7 8 10`, scales.h lines 19 and 34), ours is the standard `0 1 3 6 7 8 11` (OS-PTOOLS). Melo Min, Blues Maj and Gypsy minor, three of our four "corrected" rows, **equal** the official ones, so the header's "no byte parity is claimed for these four rows" is now stale for three of them. Name difference: `Gipsy` against the official `GypsyMin` | `core/include/o3os/core/scales.h:4-13, 24-29, 38-79`; `tracker-ce@7fd6c2c:core/source/scales.h:11-60`; compared row by row for this spec | verified parity for 38 rows; Todi = new Community OS behaviour (kept, A8) |
| F6 | The official list has three pairs of rows with the same notes: Phrygian/Todi (7, 22), Hung Min/GypsyMin (19, 35), Dim/WholeHalf (24, 30). Ours: No scale/Chromatic (0, 1), Hung Min/Gipsy (20, 36), Dim/WholeHalf (25, 31) | the two tables | fact |
| F7 | The panel's twelve are core indices `{1, 3, 2, 4, 8, 5, 10, 19, 18, 16, 12, 24}` under the name `kNoteScaleOg`. They are not OG numbers: those are one less (F5). The name is a misnomer (A12) | `app/include/o3os/app/note_entry.h:18-23, 36` | fact |
| F8 | NOTE 1.0 is 7 bytes: `u16 version`, `root`, `scale` (0..11, the panel's twelve), `octave`, `lock`, `jump`. It is read tolerantly: a value out of range becomes its default and is counted | `docs/formats/o3-project.md:235, 258`; `model/include/o3os/model/project_state.h:142, 148-154`; `model/src/project_state.cpp:134-135, 216-228`; `formats/src/native_state.cpp:57, 176-183, 341-350`; `tools/o3project.py:137, 333, 801-804, 939-940, 1110` | fact |
| F9 | A **newer minor** of a known chunk is read as far as the reader knows it, and the rest is skipped. A chunk of 1.1 needs at least the 1.0 length, and `newer_minor` makes the load line say "newer fields skipped". A **newer major** fails the whole load (`NewerMajor`) | `formats/src/native_reader.cpp:55-64, 162-179, 255-262, 930-952`; `docs/formats/o3-project.md:338` | fact (why Q1 is not b) |
| F10 | Every published build (v1.0.0, v1.0.1, v1.1.0) knows NOTE 1.0: OS-PERSIST2 merged on 2026-10-13. Older pre-release builds keep NOTE as an unknown chunk and write it back byte for byte | `docs/formats/o3-project.md:262`; `docs/STATUS.md` | fact |
| F11 | The panel: pad row 2 sets `entry.scale = column` (`entry_pad`). The LEDs light each row's choice and leave a row dark when the choice is past its 12 pads (row 3, JUMP above 11). The encoder and `Up` / `Down` reach `bump`, which returns false for Note. `Note` + `Left` / `Right` moves a track, and `Note` + `Instr` zooms | `app/include/o3os/app/pattern_edit.h:111-130`; `app/src/pad_lights.cpp:244-254`; `app/src/step_popup_actions.cpp:10-11, 69-88`; `app/src/bindings.cpp:1055-1066` | new Community OS behaviour (OS-PADS-B) |
| F12 | Text room: the panel's top line is `FixedString<48>`. The longest line with an 11-character name is 44 characters (`C# Harm. minor - Jump 32 - Oct -1 - Lock off`). The info line is `FixedString<64>` (`scale <name> <root> ...`), and Fill's KEY value is `FixedString<24>` (`C# ` + 11) | `app/src/screens/step_popup_overlay.cpp:233-242`; `app/src/pattern_view.cpp:597-608`; `app/src/fill_view.cpp:237-250` | fact |
| F13 | `snap`, `next`, `PadBoard::set` and `is_in_scale` take any core index; the pad board is tested over all 40. The widest gap among the 39 is 4 semitones (Hirajoshi, In sen, Iwato, Kumoi, Indian); `snap` searches up to 6. The comment "no gap is wider than 3 semitones" is stale | `app/include/o3os/app/note_entry.h:47-72, 59`; `app/src/pad_board.cpp:36`; `docs/specs/OS-009-pattern-editing.md:103` | fact |
| F14 | The `persist2` fixture stores scale 9 (Blues). Its size and CRC are pinned (3,032 B, `0x40AD3678`), so the fixture guards the 1.0 path | `tests/support/project_fixture.cpp:302`; `tests/o3project_roundtrip.py:85` | fact |
| F15 | Whether a unit running the official 1.9.x firmware has exactly tracker-ce's 39 rows has not been observed on a unit. It matters only for an OG import of the byte (Q5) | none | open research question |

## 3. Design

### 3.1 The table (data, from F1 and F5)

The official order is the order of the panel's encoder (Q3a), of our new byte (A1) and of an OG file's byte (F2). Degrees are semitones above the root. "Pad" is the pad of row 2 (1..12) for the twelve; the 1.0 byte is that pad minus 1. "Core" is our `core::kScaleDef` index.

| Official | tracker-ce name | Degrees | Core | Pad | Display name (Q4 a/c) | Note |
|---|---|---|---|---|---|---|
| 0 | Chromatic | 0..11 | 1 | 1 | Chromatic | |
| 1 | Minor | 0 2 3 5 7 8 10 | 2 | 3 | Minor | |
| 2 | Major | 0 2 4 5 7 9 11 | 3 | 2 | Major | |
| 3 | Dorian | 0 2 3 5 7 9 10 | 4 | 4 | Dorian | |
| 4 | Lyd Maj | 0 2 4 6 7 9 11 | 5 | 6 | Lydian | |
| 5 | Lyd Min | 0 2 4 6 7 8 10 | 6 | | Lyd. minor | |
| 6 | Locrian | 0 1 3 5 6 8 10 | 7 | | Locrian | |
| 7 | Phrygian | 0 1 3 5 7 8 10 | 8 | 5 | Phrygian | |
| 8 | PhrygDom | 0 1 4 5 7 8 10 | 9 | | Phryg. dom. | |
| 9 | Mixlydian | 0 2 4 5 7 9 10 | 10 | 7 | Mixolydian | |
| 10 | Melo Min | 0 2 3 5 7 9 11 | 11 | | Mel. minor | |
| 11 | Harm Min | 0 2 3 5 7 8 11 | 12 | 11 | Harm. minor | |
| 12 | BeBopMaj | 0 2 4 5 7 8 9 11 | 13 | | Bebop major | |
| 13 | BeBopDor | 0 2 3 4 5 7 9 10 | 14 | | Bebop dor. | |
| 14 | BeBopMix | 0 2 4 5 7 9 10 11 | 15 | | Bebop mix. | |
| 15 | Blues Min | 0 3 5 6 7 10 | 16 | 10 | Blues | |
| 16 | Blues Maj | 0 2 3 4 7 9 | 17 | | Blues major | |
| 17 | Penta Min | 0 3 5 7 10 | 18 | 9 | Pent. min | |
| 18 | Penta Maj | 0 2 4 7 9 | 19 | 8 | Pent. maj | |
| 19 | Hung Min | 0 2 3 6 7 8 11 | 20 | | Hung. minor | = Gypsy minor |
| 20 | Ukrainian | 0 2 3 6 7 9 10 | 21 | | Ukrainian | |
| 21 | Marva | 0 1 4 6 7 9 11 | 22 | | Marva | |
| 22 | Todi | official 0 1 3 5 7 8 10 (its Phrygian); **ours 0 1 3 6 7 8 11** | 23 | | Todi | A8 |
| 23 | Wholetone | 0 2 4 6 8 10 | 24 | 12 | Whole tone | |
| 24 | Dim | 0 2 3 5 6 8 9 11 | 25 | | Diminished | = Whole-half |
| 25 | SLocrian | 0 1 3 4 6 8 10 | 26 | | Super loc. | |
| 26 | Hirajoshi | 0 2 3 7 8 | 27 | | Hirajoshi | |
| 27 | In Sen | 0 1 5 7 10 | 28 | | In sen | |
| 28 | Yo | 0 2 5 7 9 | 29 | | Yo | |
| 29 | Iwato | 0 1 5 6 10 | 30 | | Iwato | |
| 30 | WholeHalf | 0 2 3 5 6 8 9 11 | 31 | | Whole-half | = Diminished |
| 31 | Kumoi | 0 2 3 7 9 | 32 | | Kumoi | |
| 32 | Overtone | 0 2 4 6 7 9 10 | 33 | | Overtone | |
| 33 | DoubHarm | 0 1 4 5 7 8 11 | 34 | | Dbl. harm. | |
| 34 | Indian | 0 4 5 7 8 10 | 35 | | Indian | |
| 35 | GypsyMin | 0 2 3 6 7 8 11 | 36 | | Gypsy minor | = Hung. minor |
| 36 | NeapoMaj | 0 1 3 5 7 9 11 | 37 | | Neap. major | |
| 37 | NeapoMin | 0 1 3 5 7 8 11 | 38 | | Neap. minor | |
| 38 | Enigmatic | 0 1 4 6 8 10 11 | 39 | | Enigmatic | |

The twelve, pad by pad (the 1.0 byte is pad - 1), in official numbers: **`{0, 2, 1, 3, 7, 4, 9, 18, 17, 15, 11, 23}`**. This is `model::kNotePanelScales[12]` (A11) and replaces `kNoteScaleOg`.

**Layout and flash.** The masks do not change: `core::kScaleDef`, 40 x u16 = 80 B, already in flash. With Q4c, `core::kScaleNames` becomes 40 x 12 B = 480 B: "No scale" plus the 39 display names, `kScaleNameChars` 11. The panel's own name table (`kNoteScaleNames`, 12 x 12 = 144 B) goes. The net change is **-64 B**. With Q4a it is +324 B (a second table of 39); Q4b adds no table. The pad map stays 12 B. `NoteEntry::scale_name()` returns `core::kScaleNames[scale + 1]`.

### 3.2 The panel (Q2a, Q3a)

While `Note` is held on Pattern (the NOTE ENTRY dialog, `step_popup_overlay.cpp`):

- **Encoder, `Up`, `Down`:** these step the scale by one in the official order, as a ring of 39. In `bump`, the Note case becomes `entry.scale = (scale + delta) mod 39`, then `apply_entry()`; it returns true when the scale changed. `popup_move` and `popup_delta` already route there (F11); a test proves it with `Note` held, since `Note` + `Left` / `Right` must still move a track. `Shift` + encoder and `Shift` + `Up` / `Down` stay as they are: `popup_page` returns false, because the panel has no list.
- **Pad row 2:** the same twelve, in the same places. A pad sets `entry.scale = kNotePanelScales[col]`. The pad's LED is Front when the scale in force is that pad's; for one of the 27, no pad of row 2 is Front, as JUMP above 11 leaves row 3 dark.
- **Text:** the top line keeps its shape and shows the new name (`C Hung. minor - Jump 1 - Oct 3 - Lock ON`). The row-2 caption becomes `PAD ROW 2  SCALE (ENCODER: ALL 39)`. That is one string; the line count does not change.
- **Info line** (Pattern): `scale Hung. minor C`, unchanged code.
- **Everything else follows the scale in force:** the NOTES pads with SCALE LOCK on (`PadBoard::set(core index)`), PIANO/ISO lights, the MIDI keyboard's snap, `snap` and `next`. No code changes there: they take the core index (F13).

Display list: the same number of text commands in the panel. The caption is 9 glyphs longer, about 9 more words; `overlay_test` re-measures it.

### 3.3 NOTE 1.1 (Q1a)

`NOTE` 1.1 is 8 bytes: `u16 version` (0x0101), `u8 root`, `u8 scale10`, `u8 octave`, `u8 lock`, `u8 jump`, **`u8 scale`**.

- `scale10` is the 1.0 field with its 1.0 meaning (0..11, the panel's twelve). In a 1.1 chunk it is **0** (A2).
- `scale` is 0..38 in the official order (A1, section 3.1).

**Writer** (`version_of`, `payload_size`, `encode` in `formats/src/native_state.cpp`):

- If the scale is one of the twelve, write NOTE 1.0, 7 bytes, `scale10` = its pad - 1. These are the bytes of today.
- Otherwise write NOTE 1.1, 8 bytes, `scale10` = 0.
- The chunk is still written only when the project has the part (`kStateNote`); that rule is unchanged.

**Reader** (`decode`, Part::Note; `known_minor` NOTE -> 1; `v1_bounds` NOTE: lo 7, hi 7 for 1.0 and 8 for 1.1):

| Chunk | Read as | Counted in "values reset" |
|---|---|---|
| 1.0, `scale10` 0..11 | `kNotePanelScales[scale10]` | no |
| 1.0, `scale10` > 11 | 0, Chromatic | 1 |
| 1.1, `scale` 0..38 | `scale` (it wins over `scale10`) | `scale10` > 11 counts 1, as every 1.0 field |
| 1.1, `scale` > 38 | `scale10`'s scale, mapped (A5) | 1 (+1 if `scale10` > 11 too) |
| 1.1 of 7 bytes | Malformed (short), as every chunk of a known version that is too short | n/a |
| 1.2 and later (newer minor) | as 1.1; the rest is skipped and `newer_minor` is set | as 1.1 |

**Python reference** (`tools/o3project.py`): the same mapping in `parse_session`, `NOTE_FIELDS` (the scale's top becomes 38), the bounds `NOTE: (7, 8)`, `known_minor(NOTE) = 1`, and the writer's version condition. The dump line `note root= scale= ...` prints the official index, which is the model's meaning.

**Model:** `kNoteScaleMax` 38. `validate_note_state` and `sanitise_note_state` use it. `NoteState` stays 5 bytes, so the model arena does not change.

### 3.4 What each build does with each file

| Build | NOTE 1.0 (every file of today) | NOTE 1.1 (one of the 27) | When it saves |
|---|---|---|---|
| This item and later | the same scale as before | that scale | 1.0 for the twelve, 1.1 for the 27 |
| Every published build (v1.0.0..v1.1.0) and every build since OS-PERSIST2 | as today | a newer minor: root, octave, lock and jump are read; the scale is Chromatic (`scale10` = 0); the last byte is skipped; the load line says "newer fields skipped" | NOTE 1.0 with Chromatic: **the scale is lost**, the other four fields kept |
| Builds before OS-PERSIST2 (pre-release) | an unknown chunk, written back byte for byte | the same, 8 bytes. The pool is 15,296 of 16,384 B (+1 B on the "Older builds" sum of section 3) | the same bytes |

This is the documented price of every newer minor (the PERF 1.1 paragraph of `o3-project.md`): readers are safe, but a downgrade through a save is not lossless.

### 3.5 Round-trip tests (S1)

C++ (`tests/persist2_format_test.cpp`, or a new `tests/note11_format_test.cpp` added to the format list of `tests/CMakeLists.txt:267`):

1. Each of the 39 scales goes through model -> write -> read -> the same model. The twelve come back as NOTE 1.0 and the 27 as 1.1.
2. Exact bytes: Blues writes `NOTE` 1.0 with `scale10` = 9 (as today); Hirajoshi writes 1.1 with `scale10` = 0 and `scale` = 26.
3. A 1.0 chunk as the build before wrote it reads the mapped scale (all twelve, plus 12..255 -> Chromatic, counted).
4. A 1.1 chunk that holds one of the twelve is written back as 1.0.
5. `scale` 39..255 in 1.1 falls back to `scale10` and is counted. Every combination of a bad `scale10` and a bad `scale` gives the count of the reader table above.
6. A 1.2 chunk of 9 bytes reads as 1.1 and sets `newer_minor`. A 1.1 chunk of 7 bytes is Malformed.
7. **Every existing fixture is byte-identical:** the sizes and CRC-32s pinned in `persist2_format_test.cpp` and `o3project_roundtrip.py:85` do not move. The `persist2` fixture's `n.scale = 9` becomes 15 (Blues in the official order) and the file stays 3,032 B, `0x40AD3678`.

Python (ctest `project_python`): a new `scales` fixture (the NOTE 1.1 chunk, Hirajoshi, lock on) goes both ways, C++ against `o3project.py`, with the same reading of the damaged NOTE cases. Its size and CRC are pinned.

Older build (the Level C proof of 3.4): build the dev commit before S1 from its sources. Its `o3pj_tool resave` and its `o3project.py rewrite` read the `scales` fixture, report `newer_minor`, and write NOTE 1.0 with scale byte 0 and the other four fields kept. They write every other fixture unchanged.

App (`tests/persist2_app_test.cpp`): loading a 1.1 project sets `NoteEntry::scale`, and the NOTES pads are laid out for Hirajoshi with the lock on (`pads.scale() == entry.core_scale()`). Saving after a pad of row 2 writes 1.0.

## 4. Slices

| # | Slice | Size | Level | Files | Acceptance | Host tests | QA rows | Flash / SRAM / CPU |
|---|---|---|---|---|---|---|---|---|
| S1 | **NOTE 1.1 and the official numbering.** The model and `NoteEntry` hold the official index. The NOTE 1.1 writer, reader and Python reference, with the loader for 1.0. The pad map in `model`; LEDs and pad row 2 go through it. No UI gain yet: a 1.1 file works everywhere, but the panel still offers twelve | S/M | C | `model/include/o3os/model/project_state.h`, `model/src/project_state.cpp`, `formats/src/native_state.{h,cpp}`, `formats/src/native_reader.cpp`, `tools/o3project.py`, `app/include/o3os/app/note_entry.h`, `app/include/o3os/app/pattern_edit.h`, `app/src/pad_lights.cpp`, `app/src/project_state.cpp`, `core/include/o3os/core/scales.h` (header comment: F5 facts, the Todi divergence; no data change), tests, `docs/formats/o3-project.md`, `docs/formats/og.md:110` (one line: 0x115 is the official order, unread, written 0) | Section 3.5 all green; every existing fixture byte-identical; the older-build check of 3.5; `tools/ci.sh host`, then `qemu` with the batch | 3.5's list; `pad_modes_test` (pad row 2 and LEDs through the map, a scale from the 27 lights no row-2 pad); `pattern_edit_rules_test` pins the 39 masks against section 3.1 (the official rows, Todi excepted) | QA-FIL-139: a project saved with Hung. minor loads with it (unit); the same file on a published build reads Chromatic with "newer fields skipped" (host proof, unit optional) | about +0.3 KiB [est]; SRAM 0 (NoteState 5 B, NoteEntry 4 B unchanged); CPU 0 (main loop) |
| S2 | **The panel walks 39, one name table.** `bump` for Note (ring of 39), the row-2 caption, the display names (Q4), Fill's names (Q4c), the manual section, scenarios | S | B | `app/src/step_popup_actions.cpp` (bump, header comment), `app/src/screens/step_popup_overlay.cpp`, `core/include/o3os/core/scales.h` (names), `app/include/o3os/app/note_entry.h` (scale_name), `app/src/fill_view.cpp` (none if names stay fixed width), `sim/scenarios/qa/qa_pattern.txt`, `sim/goldens.txt`, `platform/mk66/tools/hil/qa_pattern.json`, `docs/qa/catalogue/pattern.md`, `docs/manual/pattern.md` (a short "Choose the key: hold `Note`" section, `manual-writing` skill) | The encoder, `Up` and `Down` with `Note` held walk the 39 in the official order and wrap; pad row 2 unchanged; the line and the info line name every scale; DL re-measured in `overlay_test`; Fill shows the same names (Q4c); every other Fill and panel golden unchanged | `step_popup` bump cases (+1, -1, wrap both ways, a pad after the encoder); `fill_view_test` names; `pattern_edit_rules_test` names (no trailing space, at most 11 characters); `overlay_test` words | QA-PAT-324: encoder walk and wrap · 325: `Up` / `Down` · 326: pad row 2 from one of the 27 · 327: SCALE LOCK on with Hirajoshi, NOTES pads and the MIDI keyboard (`amidi`) · 328: the info line names it · 329: save, reload, scale back · QA-PAT-304 text (names, Q4c) | about +0.2 KiB code, -64 B names (Q4c) [est]; SRAM 0; DL about +9 words in the Note panel; CPU 0 |
| S3 | Only if Q5 = b: import 0x115 into NOTE (`og_project.cpp:150`, present bit), export NOTE's scale to 0x115 (`:489`); `docs/formats/og.md` | S | C (OG format) | `formats/src/og_project.cpp`, tests `og_*` | an OG project with scale 19 imports as Hung. minor; the export writes the scale; every OG fixture otherwise byte-identical | og round trips | QA-FIL row | about +0.1 KiB |
| S4 | Only if Q6 = b/c: Fill opens on the NOTE key; Random Note in the scale | S each | B / C | `app/src/fill_view.cpp`; `seq/src/sequencer.cpp` | the official behaviours of F3 and Q6 | fill_view, sequencer goldens | QA-PAT rows | Random Note: CPU measured on the unit |

**Order:** S1, then S2. S2 needs S1's meaning; they can share one batch branch with two commits. S1 gets the risk review and one independent reviewer; S2 gets one reviewer. One image after S2; the unit checks are QA-FIL-139 and S2's QA-PAT-324..329. A host test is not hardware verification. Still needed on the unit: the panel walk, the LEDs, SCALE LOCK on the NOTES pads and the MIDI keyboard with a 5-note scale, and a save/power-cycle/load.

## 5. Risks

- **R1, a missed reader of the old numbering (the main risk).** In RAM, `scale` changes from the panel index to the official index. A site that still reads it as 0..11 would pick a wrong scale with no warning. Mitigation: the complete list of sites below; `static_assert`s on `kNoteScaleMax` and on the map's size; the pinned fixture bytes, which fail if the 1.0 byte changes; and a test per site (pads, LEDs, codec, Python).
- **R2, downgrade loss.** A published build that saves a 1.1 project writes Chromatic. This is accepted under Q1a and said on the load line. It is the documented behaviour of every newer minor.
- **R3, Python drift.** `o3project.py` must map exactly as C++. `project_python` (both ways, damaged cases) catches a difference.
- **R4, UX.** The encoder and `Up` / `Down` did nothing in the panel, so no habit breaks. A scale from the 27 leaves row 2 dark, which may read as "no scale". The top line names the scale, and the manual says so.
- **R5, duplicates and Todi.** Two pairs of scales sound the same (A7), and our Todi is not the official one (A8). Both are stated in the manual's table. An OG import (Q5) must say that Todi plays the standard notes here.
- **R6, Fill names (Q4c).** Fill goldens that show a scale other than No scale or the twelve change. QA-PAT-304's example (`C# Dorian`) does not.
- **Latent items, not fixed here:** the stale comments (`note_entry.h:59` "no gap wider than 3", the "four corrected rows" header of `core/scales.h`; S1 updates both); our Random Note ignores the scale (F3, Q6); the OG key is not imported (F4, Q5).

## 6. State for implementer

- **Read first:** this spec's sections 3.1, 3.3 and 3.5; `docs/formats/o3-project.md:201` (USBS 1.1, the model to copy) and `:207` (PERF 1.1's write rule).
- **Sites that read or write the NOTE scale (R1).** Every one changes or is checked in S1:
  - `model/include/o3os/model/project_state.h:142, 150` (max 38, comment)
  - `model/src/project_state.cpp:134-135, 216-228`
  - `formats/src/native_state.h:63` (add `kNoteBytesV11` = 8 and `kNoteVersionScale` = 0x0101)
  - `formats/src/native_state.cpp:57` (`payload_size`), `:96-110` (`version_of`), `:176-183` (`encode`), `:341-350` (`decode`; `chunk_minor` is read at `:241`)
  - `formats/src/native_reader.cpp:55-64` (`known_minor`), `:70-90` (`v1_bounds`)
  - `tools/o3project.py:137, 333, 474-489, 801-804, 939-940, 1110`
  - `app/include/o3os/app/note_entry.h:18-39`
  - `app/include/o3os/app/pattern_edit.h:111, 113-130`
  - `app/src/pad_lights.cpp:244-254`
  - `app/src/project_state.cpp:40, 120-136`
  - `app/src/screens/pattern_screen.cpp:146`
  - `app/src/screens/step_popup_overlay.cpp:233-237`
  - `tests/support/project_fixture.cpp:302` (9 -> 15)
  - `tests/persist2_app_test.cpp:130, 173`
  - `tests/pad_modes_test.cpp:114, 552-570, 615`
  - `tests/o3project_roundtrip.py:85, 100` (add `scales`)
- **Do not touch:** `core::kScaleDef` (no mask changes; Todi stays); Fill's index 0..39 (`fill_view.cpp:36, 139, 244`, session state: it keeps "No scale" at 0); the OG codec (unless Q5 = b); the sequencer.
- **Docs in S1:** `o3-project.md`:
  - line 42 (NOTE row: "1.1 for a scale outside the twelve")
  - line 235 (the 1.1 layout)
  - line 258 (lengths: 1.1 exactly 8)
  - line 262 (pool +1 B)
  - line 338 (NOTE in the newer-minor list: 1)
  - section 10 (the change-log entry: compatibility both ways, the round trips)

  Also `docs/architecture/os-tree.md:437` (the core table is the official 39 + No scale; only Todi differs).
- **Checks:** scoped `ctest -R 'persist2|note11|project_python|pad_modes|pattern_edit_rules|fill_view|overlay'` while iterating; one `tools/ci.sh host` per branch; `qemu` with the batch; the older-build check (3.5) recorded in the commit body; size delta (`fw-build`) in the commit body.
