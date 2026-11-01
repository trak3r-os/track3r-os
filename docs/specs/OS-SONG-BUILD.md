# OS-SONG-BUILD: FORM and BRIDGE, two ways to build a song from patterns in one go

Level B (main loop only; no audio path, no sequencer change). **No persistent format change**: a build writes slots (`insert_order`) and new patterns (`create_pattern`, `set_steps`, `set_track_length`, `set_pattern_name`), all through the model's existing API. Batch SONG-BUILD, two items and two commits: OS-SONG-FORM first, OS-SONG-BRIDGE second (BRIDGE reuses FORM's free-number, copy, slot-insert and refusal code). Both are **new Community OS behaviour**: the maintainer approved them on 2026-10-30 (BACKLOG rows OS-SONG-FORM and OS-SONG-BRIDGE, orders 3 and 4 of the Song research), with Fable's defaults. Follows `docs/specs/SONG-UX.md` (the TOOLS dialog, 1.4; UNIQUE and the undo cap, 1.5 and 1.7; D15's free number) and `docs/specs/OS-SONG-VIEW2.md` (the ORDER list stays the Song screen's first page).

## Intent

A song is slots of patterns. Today the order list is built by hand, slot by slot. Two quick builders on the ORDER page, as two more values of the TOOLS dialog's OP row (F6, no new key):

- **FORM** turns 1 to 3 patterns into a whole song shape from a const table of forms, **by subtraction**: the first slot has some tracks cleared (LOOP>SONG: the music, so drums only), a break is the pattern without the kick, and so on. The copies are new patterns with some tracks left empty, so the originals are never touched.
- **BRIDGE** puts N new slots between two adjacent slots A and B. Slot by slot, the steps of A are replaced by the steps of B, so the music moves from one pattern to the other instead of jumping.

Both refuse before writing anything when the pool, the order list or the undo ring cannot take the result, and both are one undo unit.

## Acceptance criteria

**OS-SONG-FORM**

1. TOOLS (F6) on the Song ORDER page has the OP value FORM, with the rows FORM (LOOP>SONG, POP, TECHNO) and DRUMS (1..8 tracks, default 4). F1 APPLY builds the song shape, F8 BACK leaves.
2. The source patterns are the distinct patterns of the dialog's slots (the selection, or the cursor's slot) in slot order, at most 3: A, B, C. A missing B or C is A.
3. The new slots go after the last slot of the dialog's rectangle; the original slots stay. The cursor moves to the first new slot and the selection collapses.
4. A slot of the form whose cleared groups hold no data in its source points at the source itself (no copy). Otherwise it points at a copy of the source with those groups' tracks empty. Slots that empty the same tracks of the same source share one copy, whatever groups name them (with DRUMS = 1 the kick and the rest of the drums are one track, so POP's K+P and K make one copy).
5. A copy keeps the source's name and every track's length; the tracks of the cleared groups are empty and the others hold the source's steps up to the track's length.
6. Every refusal of the table below writes nothing (journal and model unchanged, checked by hash in the tests).
7. One undo (F8 on Song) removes the slots and the copies together; redo restores them.
8. Allowed while the song plays (as F3 and UNIQUE are).

**OS-SONG-BRIDGE**

1. TOOLS has the OP value BRIDGE, with the row SLOTS (1..16, default 4).
2. A is the first slot of the dialog's rectangle and B the slot after it: the rectangle is the cursor's slot (B is the next slot) or exactly two slots. Anything else refuses.
3. N new slots go between A and B; slot k (k = 1..N) holds a new pattern built by the rules of "BRIDGE rules". The cursor moves to the first bridge slot.
4. The result is reproducible: the same A, B, N and the same project give the same bytes every time (a fixed seed, no clock, no `rand`).
5. Each cell (track, step) of a bridge pattern is the A cell or the B cell as a whole step, never a mix of their fields. Once a cell has taken B's step it keeps it in every later bridge slot.
6. The first bridge slot is mostly A and the last mostly B. N = 1 is about half and half.
7. The refusals, the undo unit and the playing rule are FORM's.

## UX

TOOLS stays one dialog (`SongToolsOverlay`, 520 px wide). OP's values: TRANSPOSE, INSTRUMENT, CLEAR, EXPORT WAV (unchanged order and numbers), then **FORM**, **BRIDGE** (appended, so the muscle memory of the old four does not move). The dialog has three rows now (it grows by one row for every OP; the third shows only for FORM).

| OP | Row 2 (Left/Right, encoder) | Row 3 | Footer (48 characters at most) |
|---|---|---|---|
| FORM | FORM: `LOOP>SONG`, `POP`, `TECHNO` (rolls over) | DRUMS: `TRACKS 1-4` (1..8) | `FROM 02 05 07  8 SLOTS  3 NEW PATTERNS` |
| BRIDGE | SLOTS: 1..16 (Shift: 4) | none | `PATTERN 02 TO 05  4 SLOTS  4 NEW PATTERNS` |

The footer is the plan: a FORM line is `FROM <ids>  <n> SLOTS  <m> NEW PATTERNS` (`PATTERN` for 1), at most 43 characters with three 3-digit ids; the BRIDGE line is at most 43 too. A help line under the rows names what the OP does (`A SONG SHAPE BY SUBTRACTION`, `A RUN OF SLOTS FROM A TO B`). The footer is computed when the dialog opens and after a value change (the planner, no write). Dialog settings live for the session (the Fill dialog's rule). A refusal closes the dialog and goes to the strip (the existing `songtools_apply` flow: the strip waits behind a modal overlay).

**Refusals and results** (Warn unless marked Info; every text is at most 48 characters; the texts hold no F-key names, so `tools/check_keytext.py` stays quiet):

| When | Text |
|---|---|
| no project / empty song | `NO PROJECT` / `SONG IS EMPTY` (existing) |
| FORM, more than 3 distinct patterns | `SELECT 1-3 PATTERNS` |
| FORM, a dialog slot names no pattern | `SLOT 03 HAS NO PATTERN` |
| BRIDGE, not 1 or 2 slots | `SELECT 2 SLOTS` |
| BRIDGE, the cursor is on the last slot | `NO NEXT SLOT` |
| BRIDGE, A and B are the same pattern | `SAME PATTERN IN BOTH SLOTS` |
| BRIDGE, A or B names no pattern | `SLOT 03 HAS NO PATTERN` |
| the order list cannot take the new slots | `SONG FULL: NEEDS 10 SLOTS, 4 FREE` |
| the pool or the numbers cannot take the copies | `NEEDS 9 FREE PATTERNS, 4 FREE` (`PATTERN` for 1) |
| the unit is over half of the Song ring | `TOO BIG TO UNDO: SELECT LESS` (the existing Song cap text, D7) |
| FORM done (Info) | `FORM POP: 9 SLOTS, 4 NEW PATTERNS` |
| BRIDGE done (Info) | `BRIDGE: 4 SLOTS, 4 NEW PATTERNS` |

A result with no new pattern says `0 NEW PATTERNS`.

## The forms (v1; D2 to D6)

Groups of tracks, from DRUMS = N (default 4): **K** = track 1 (the kick), **P** = tracks 2..N (the rest of the drums), **M** = tracks N+1..16 (the music). A row is the source role (A, B, C) and the groups it clears.

| # | LOOP>SONG (A) | POP | TECHNO |
|---|---|---|---|
| 1 | A, clear M (drums only) | A, clear K+P (music only) | A, clear P+M (kick only) |
| 2 | A, clear K | A | A, clear M |
| 3 | A | A, clear K | A |
| 4 | A | B | A |
| 5 | A, clear K+P | A | B, clear K+P |
| 6 | A | B | B, clear K |
| 7 | A | C, clear K+P | B |
| 8 | A, clear M | B | C |
| 9 | | B, clear K+P | A, clear M |
| 10 | | | A, clear P+M |

LOOP>SONG makes 8 slots and at most 3 copies, POP 9 slots and at most 4, TECHNO 10 slots and at most 4 (fewer when a cleared group holds no data, or a role repeats A). The table is `constexpr` data: per form a list of `{role, cleared groups}` bytes.

## BRIDGE rules (D13 to D18)

For bridge slot k of N, `t16 = k * 65536 / (N + 1)`. Everything is integer, no float. For track `tr` (0..15) and step `i`:

- `phase(tr)` = 2048 x the rank of `hash(seed, tr)` among the 16 tracks' hashes (ties by track number): the tracks start to move at different times, spread over the first half of the range (0..30720).
- `jitter(tr, i)` = `hash(seed, tr, i) >> 17` (0..32767): within a track the steps move one by one.
- `thr(tr, i) = phase(tr) + jitter(tr, i)`. The cell takes **B's step when `thr < t16`**, else A's.
- A cell reads `A.steps[tr][i % lenA]` and `B.steps[tr][i % lenB]` (a short track loops, as the sequencer plays it). A step is taken whole: note, instrument, volume and both FX together.
- The bridge track's length is A's while `2 x t16 <= 65536` and B's after, so the pattern's feel changes at the middle.
- `hash` is a 32-bit integer mix (xorshift-multiply, a few lines) over the constant `kBridgeSeed` = 0x0B91D6E5.
- The pattern name is `BRIDGE k/N`.

Because `thr` does not depend on k, a cell that took B's step at slot k keeps it later: the bridge is monotone. Because empty steps move like notes, a note of A that B lacks thins out and a note of B that A lacks fills in. At the last slot `t16` is N/(N+1) of the range, so a few percent of cells still hold A (the next slot is B itself). Tracks 13 to 16 and every track past the active count are processed the same way (they are usually empty), so the result does not depend on the active-track count.

## Where the code lives

New: `app/include/o3os/app/song_build.h`, `app/src/song_build.cpp`: pure over the model, in the shape of `song_edit.h`: `song_form_plan` / `song_form_apply`, `song_bridge_plan` / `song_bridge_apply`, the form table, `bridge_hash`, `bridge_cell`. Planner and executor share one walk (as `walk` and `unique_walk` do), so the pre-check, the bytes and the writes cannot disagree. The plan carries `slots`, `copies`, `free_numbers`, `pool_left`, `bytes`, a refusal enum (None, Select, NoPattern, SongFull, NoRoom, NoNext, Same) and one number for the text.

Changed:
- `app/include/o3os/app/song_edit.h`, `app/src/song_edit.cpp`: `free_number` and `copy_pattern_body` leave the anonymous namespace (declared in a private header); a `song_copy_pattern_masked(editor, from, to, track_mask)` beside `song_copy_pattern`. UNIQUE and DUP PAT keep their exact copy (steps past the length too).
- `app/include/o3os/app/song_tools_view.h`, `app/src/song_tools_view.cpp`: `SongTool::Form = 4`, `Bridge = 5`; `SongToolsRow::Drums = 2`, `kSongToolsRows = 3`; fields `form`, `drums`, `bridge_slots`; the descriptors, text, step and get/set of the new rows.
- `app/src/song_map_actions.cpp`: `songtools_apply` cases that call `run_build` (the pre-check, the refusal texts, one undo unit, the cursor, the result text); `song_tools_open` fills the footer data.
- `app/src/screens/song_tools_overlay.cpp`: the third row, the help line and the footer.
- Docs, tests and sim: `docs/ui/ui-map.md`, `docs/qa/catalogue/song.md`, `docs/manual/song.md` (and its sync lock), `docs/CHANGELOG.md`, `platform/mk66/tools/hil/qa_song.json`, `sim/scenarios/qa/qa_song_build.txt` (new), `sim/src/screens_app.cpp` (dialog states), `tests/song_build_test.cpp`.

No new action, key or binding: `songtools.apply` already carries `kActionUndoable`, so a build is one undo unit of the Song area. It inherits OS-UNDOGRP D4: a Song-made `create_pattern` clears the Pattern history on undo, with the strip notice.

**The unit.** Per build: create each copy and fill it, then insert the slots that name it inside an `Editor::OrderBatch` (a playing song sees the new slots as one change, as DROP SLOT does). A slot never names a missing pattern.

## The free-pattern pre-check

Before any write, the planner counts the copies it needs (distinct (source, cleared groups) that clear something for FORM; N for BRIDGE) and takes the **free numbers** by SONG-UX D15's rule: ascending from 1, not held by a pattern, not named by any slot, not already taken by this plan. Then, in this order (the cheap checks first):

1. Slots: `song_length() + new slots <= kMaxSongPositions` (255), else `SONG FULL: NEEDS n SLOTS, m FREE`.
2. Patterns: `need <= min(kMaxPatterns - pattern_count(), free numbers)` (the pool is 128, the ids 255), else `NEEDS n FREE PATTERNS, m FREE`, m being that minimum.
3. Undo: `bytes` (exact journal bytes, record by record as `copy_pattern_body` counts; the new slots' `insert_order` records; the unit's header and trailer) `<= song_undo_limit(Song ring)` (48 KiB), else the cap text. A copy that writes each track's span up to its length costs about 12 + 2 x 7 x length bytes a track (a 32-step track is about 460 B, a 128-step track 1.8 KB), so a 12-track 32-step pattern is about 5.5 KB.

Nothing is written when one fails.

## Host tests planned

`tests/song_build_test.cpp` (doctest, the area label of `song_map_edit_test`, registered in `tests/CMakeLists.txt`), each case named after its behaviour:

- FORM: every form's slot count and (role, cleared groups) per row; a cleared group with no data gives no copy and the slot shares the source; slots that empty the same tracks of the same source share one copy (DRUMS 1: K+P and K are one); the copy holds the right tracks, lengths and name, the cleared tracks empty and steps past the length not copied; DRUMS 1 and 8 move K, P and M; one, two and three sources (B and C fall back to A); more than 3 distinct patterns refuses; a missing pattern refuses; the new slots follow the rectangle and the cursor lands on the first.
- Pre-check: pool full by one, numbers full, a slot naming a missing pattern holds its number back, the exact counts in the texts, song one slot short, undo cap: each refusal leaves the project's hash and the journal unchanged.
- Undo: one F8 takes slots and copies back (song length, pattern count, order bytes), redo restores them; the Pattern-history fence notice.
- BRIDGE: the hash and `thr` pinned by literal values (the seed is part of the contract); the same inputs twice give the same bytes; N = 1, 4, 16; monotone per cell across k; every cell equals A's or B's step as a whole; the count of B cells does not decrease with k, the first slot is below half and the last above; A and B of different lengths (the loop rule and the length switch); A empty and B full fills in, A full and B empty thins out; A equals B, no next slot and 3 selected slots refuse; the names; nothing outside the new patterns and slots changes.
- While the transport runs: a build inserts slots before the playing slot, and the playing position and a queued jump follow (the engineer checks the sequencer's handling of an insert as F3's, and adds DROP SLOT's queue rule if it needs one).
- The tools-view rows and texts for FORM and BRIDGE (extend the existing tools-view test); `budget_test` keeps the new dialog states under 1792 words.

## QA rows planned

`docs/qa/catalogue/song.md`, ids from QA-SNG-097 (the next free one): FORM 097..106 (the dialog, one row per form, DRUMS, sharing without a copy, the refusals, the pre-check texts, one undo, while playing); BRIDGE 107..115 (the dialog, the monotone result, reproducible, the A and B refusals, the pre-check, undo, while playing). Sim: `qa/qa_song_build.txt` with snaps of the dialog on each OP and one of each result. Unit steps in `qa_song.json`: FORM POP on a 12-track demo song by ear (the intro has no music, the break no kick), BRIDGE 4 between two different patterns by ear, F8 after each, no dropout while the song plays. `manual:` cells only for the by-ear parts.

## Size and cost

Estimates, measured by the engineer on the mk66 build: FLASH +2.5 to 3.5 KB (planner and executors about 1.4 KB, tools-view rows and strings about 0.8 KB, overlay about 0.3 KB, forms table about 40 B, bridge hash and cell about 0.4 KB), SRAM 0, SDRAM +3 B (the view fields), stack: one 896 B step buffer in the bridge and the copy (main loop). CPU per audio block: 0. A build runs once on F1: at most 16 patterns x 16 tracks x 128 steps of hash and copy (about 33 k cells, a few ms).

## Decision table

| ID | Question | Options | Default chosen | Why |
|---|---|---|---|---|
| D1 | Where the entry points live | a) TOOLS OP values · b) a new key · c) a new screen | a) FORM and BRIDGE are OP values after EXPORT WAV | The backlog says a sub-page of TOOLS on the ORDER list, not a new key; appending keeps the old four in place |
| D2 | Which tracks count as drums for FORM | a) by instrument kind · b) track names · c) tracks 1..N, N in the dialog | c) DRUMS row, 1..8, default 4; K = track 1, P = 2..N, M = N+1..16 | The model has no drum marker (`InstrumentKind` is Sample, Midi, Module); a row costs one descriptor and is honest |
| D3 | The v1 form table | free per form · one shared shape | the table above: LOOP>SONG 8 slots, POP 9, TECHNO 10 | Short songs a user can then edit; fewest copies (3, 4, 4) |
| D4 | The sources | a) the dialog's slots, 1 to 3 distinct · b) pick patterns by number in the dialog | a) distinct patterns of the selection in slot order; more than 3 refuses | No new picker; select what you want in the order list |
| D5 | A missing B or C | refuse · repeat A · repeat the last | repeat A | One pattern is enough to make any form |
| D6 | A cleared group that holds no data | copy anyway · share the source | share the source (no copy); copies are shared by the tracks they really empty (source, `cleared & data`), not by the group names | Saves patterns and undo bytes; the slot sounds the same |
| D7 | The undo-cap text | the existing text · a new one per tool | `TOO BIG TO UNDO: SELECT LESS` | The backlog says the Song cap message applies; one string |
| D8 | Where the new slots go | a) replace the selection · b) append at the end · c) after the rectangle | c) after the last slot of the dialog's rectangle (BRIDGE: between A and B) | Nothing the user built is lost; the cursor lands on the first new slot |
| D9 | Where the new patterns are numbered | the first free pool slot · the first free number | the first numbers no pattern has and no slot names, ascending (SONG-UX D15) | A slot naming a not-yet-existing pattern would start playing the copy |
| D10 | Song-length overflow | a) cut the form · b) refuse | b) refuse with `SONG FULL: NEEDS n SLOTS, m FREE`, nothing written | A cut form is not the form; the number says how much to remove |
| D11 | A copy's contents | exact (as UNIQUE) · steps up to each track's length | up to each track's length; name and lengths kept | Steps past the length never sound; at 1.8 KB of undo a track they would make most forms hit the 48 KiB cap |
| D12 | The copy's name | source name · a form name | the source's name (FORM); `BRIDGE k/N` (BRIDGE) | A reader sees where a copy came from; a bridge slot says what it is |
| D13 | Which cells change first in BRIDGE | notes before rests · tracks in order · seeded | seeded by track, then by step: tracks start at staggered times, steps within a track one by one | Needs no notion of drums; reads as a gradual change |
| D14 | How N slots are spread | linear · eased | linear: `t = k / (N + 1)`, k = 1..N | A and B themselves are the ends; easy to reason about and test |
| D15 | The seed | a clock or `rand` · a fixed constant | `kBridgeSeed` = 0x0B91D6E5, integer hash, no state | The backlog asks for a result that can be made again; the pinned test makes the seed a contract |
| D16 | What moves as a unit | fields separately · the whole step | the whole step | A bridge never makes a note of A with B's instrument |
| D17 | A and B of different lengths | A's length · B's · switch | cells loop (`i % len`); the track length is A's up to the middle and B's after | A short track loops as it plays; the length changes at the middle |
| D18 | BRIDGE's track range | the rectangle's tracks · all | all 16 tracks (the map's track range is ignored) | A bridge of some tracks would copy the others from A and then jump |
| D19 | BRIDGE with A = B | allow · refuse | refuse `SAME PATTERN IN BOTH SLOTS` | Every bridge slot would equal A: a waste of patterns |
| D20 | The dialog's third row | hide it (the dialog resizes) · a fixed three-row dialog | fixed three rows for every OP | `Overlay::rect` is static; the dialog's goldens change once |
| D21 | Playing | allow · refuse | allow, inside one `OrderBatch` | As F3 and UNIQUE; the playing position follows the insert (tested) |

## Hardware-only checks

By ear on the unit: the three forms on a real 12-track project (the intro, the break and the outro say what they are), a BRIDGE of 4 between two different patterns, F8 after each, nothing dropped while the song plays and the build is made, and the dialog's readability on the panel (the added row, the 43-character footer).

## Out of scope

DNB, CHIP and DJ LOOP forms (after v1, per the backlog); forms the user edits or saves; a bridge between non-adjacent slots; an eased curve; picking the sources by number; the manual beyond the Song chapter; any change to the sequencer.
