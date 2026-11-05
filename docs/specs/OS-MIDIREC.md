# OS-MIDIREC phase 0: the EVENTS list

Status: design, 2026-11-05. Level B (no format change, main loop only, no audio path). Scope cut by the maintainer on 2026-11-05: phase 0 only; phase 1 (lanes, recording, the `MEVT` chunk) is parked as OS-MIDIREC-P1. Read-only study of `dev` (a6fc0b57). Radar card: `docs/vision/radar/midi-event-list.md`.

## Decisions for the maintainer

**Decided by the maintainer 2026-11-05: every recommended default (Q1 b, Q2 a, Q3 a, Q4 a, Q5 a).**

Only what the decisions of 2026-11-05 (backlog row OS-MIDIREC, commit 88c874f3) leave open. Everything else is decided below as an assumption.

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| Q1 | The CH column in phase 0. The channel belongs to the instrument (`Instrument::midi_channel`), not to the step, so "the channel per event" cannot be stored before phase 1 | a) read-only: the channel of the step's instrument; the user changes it through INS · b) editable on NOTE rows: a channel picks the step's instrument, the MIDI instrument on that channel (the step's own when it has it, else the lowest slot that has it; none: `NO MIDI INSTRUMENT ON CH 05`) · c) no CH column until phase 1 | b | It gives the per-event channel the maintainer decided, today, with no format change. In an OG project slot 48 + n is channel n, so the pick is unambiguous there. Phase 1 keeps the column and stores the value |
| Q2 | Which steps the list shows | a) every note and CC FX in the scope; rows of a non-MIDI instrument dim, CH `--` · b) rows of MIDI instruments only · c) EVENTS refused on a track with no MIDI instrument | a | A hidden note would still block a move (one note per step). The CC FX of a sample instrument do go out as MIDI (the Config numbers). Micro-move editing is useful on drums too |
| Q3 | Layout | a) the whole grid pane: 16 rows, every column; BACK puts the grid cursor on the event's step · b) the OS-PTOOLS2 right-hand panel (300 px, x 500-800): TIME, TYPE, DATA and LEN only, the grid visible at the left | a | The nine columns need about 50 characters (500 px); the panel holds 30 |
| Q4 | EVENTS before OS-PTOOLS2 S3 (the 2x8 verb menu) is built | a) ship it as an entry of today's F7 TOOLS list (after SHRINK /2); S3 moves it into the menu · b) wait for S3 | a | No dependency (one table row); the grammar, select then F7 then verb, is the same. S3's menu already holds 18 verbs for 16 pads (with COPY / PASTE PATTERN), so S3's layout must place EVENTS anyway |
| Q5 | The list while the song plays | a) it stays where the user is; a marker shows the rows of the playing step · b) in WATCH the window follows the playhead, as the grid does | a | It is an editor (the STEP model): a window that follows would move the row under an edit. The marker shows where the song is |

## Decided on 2026-11-05 (maintainer, backlog row OS-MIDIREC)

- EVENTS is a verb of the OS-PTOOLS2 F7 menu, over the cursor's track or the selection. It has no key of its own.
- A note is one row with a LEN, not an on row and an off row.
- The channel is per event. In phase 0 it can only be derived; see Q1.
- Phase 0 first: an edit-through list over the existing steps, CC FX A..F and micro-moves, with no format change. Phase 1 (lanes, `MEVT`, Level C) is parked (OS-MIDIREC-P1).
- The OG `.mt` export rule (lane events dropped and counted) belongs to phase 1. Phase 0 writes ordinary steps, so every export is unchanged.

## Assumptions (decided in this spec; reversible, each covered by a test)

- **A1 Time unit.** TIME and LEN read `SSS.PP`: the step (1-based, three digits) and the percent of a step (00..96), on the sequencer's 48 microsteps (`offset x 100 / 48`, rounded). The percent is within one of the Micro-move value the grid shows (value 50 plays at microstep 24, which reads `.50`). The encoder moves TIME by one microstep (about 2 %); Shift + encoder moves it by one step.
- **A2 Scope.** With no block, the scope is the cursor's track, every step of its length. With a block, it is the block's tracks and rows as whole steps (a column block is widened). The scope (pattern, tracks, rows) is fixed when the list opens. TIME edits and Insert stay inside it. A LEN end may fall below the block's rows, because the end belongs to the note.
- **A3 Row order.** Rows are sorted by step, then microstep, then kind (CC rows before a NOTE row at the same time: the sequencer sends the row's FX before the note), then track, then FX column 2 before column 1 (the sequencer's order, so the lower of two CC rows on one step is the value that wins on the wire).
- **A4 Ownership.** Every byte of a step belongs to at most one row:
  - a NOTE row owns the note, instrument and volume of its step, every FX column that is not a CC FX (Micro-move included), and its end (A5);
  - a CC row owns its one FX column;
  - an OFF, CUT or FADE that ends no note above it is a row of its own (TYPE `OFF`, `CUT`, `FADE`) and owns its note column and its Micro-move;
  - FX on a step with nothing in its note column, other than CC FX, belong to no row and are not listed.
- **A5 LEN.** A note ends at the first row below it on its track whose note column holds anything (a note, OFF, CUT or FADE; see fact F8). An OFF, CUT or FADE found there is the note's *end*. It is not listed and it moves with LEN. LEN never wraps: a note with nothing below it reads `>` plus the distance to the track's end (it plays on past the end). A note whose step holds a Gate FX shows the gated length with the flag `G`.
- **A6 Micro-move writes** use the recording rule (OS-RECMICRO 2.3-2.4, `rec::micro_value`). An offset of 0 removes the Micro-move; `m02 00` is never written. A new Micro-move goes to FX 2 when it is free, else to FX 1, else the edit is refused with `NO FREE FX COLUMN FOR THE MICRO-MOVE`. An existing Micro-move keeps its column.
- **A7 VAL of a note** is the step's volume column (0..127). An unset volume reads `---` (the instrument's default velocity plays). The first encoder detent starts from 127 (`kFirstVolume`), as on the grid. A Velocity FX on the step is shown by the `+` flag; it is not edited here.
- **A8 Inherited instrument.** A step with instrument 0 plays the track's last instrument. The list resolves it by looking up the track inside the pattern and draws it dim. At the top of the track it is unknown (`?`), because it depends on what played before.
- **A9 Insert.**
  - On a NOTE row: a copy of the note (note, instrument and volume; no Micro-move and no other FX) on the row's end step. When that step holds the row's own OFF, the copy replaces it (the row still ends there). When the row runs into the next note, or the end lies outside the scope, nothing is written and the strip says `NO FREE STEP AFTER THIS NOTE`.
  - On a CC row: the same letter and value on the next step of the scope that has a free FX column.
  - Shift + Insert adds the other kind on the row's step: a `CC A 000` in the first free FX column, or a NOTE (the note and instrument in force, as the pads enter them) when the step's note column is empty.
  - On an empty list, Insert adds a NOTE and Shift + Insert a `CC A`, at the scope's first step on the grid cursor's track.
  - The cursor goes to the new row.
- **A10 Delete.**
  - A NOTE row: its owned bytes (A4) are cleared, end included.
  - A CC row: its FX column is cleared.
  - An OFF, CUT or FADE row: its note column and Micro-move are cleared.
  - If the deleted note was the end of the note above it, an OFF is written at the deleted note's time, so the note above keeps its LEN (invariant I2).
- **A11 Invariants of every edit.** I1: only the steps its rule names change. I2: no other note's start or LEN changes. The one exception is the one-voice rule (fact F8): a note placed inside another note's span ends that note there, and the strip says `ENDS THE NOTE AT 005.00`. I3: the edit's undo unit gives the pattern back byte for byte.
- **A12 Undo.** Every list action that writes is one dispatched action, so it is one undo unit in the PATTERN ring. Encoder detents are one unit each, as on the grid (no click group). F7 UNDO and Shift + F7 REDO work inside the list; the grid's F8 undoes the same units after BACK.
- **A13 Edit rules.** Edits need STEP (Edit on). In WATCH the list is read-only and an edit key says `TURN EDIT ON`, as the grid and PAT-OPS do. Opening follows `tools_open` exactly (today refused while the view follows the play line; OS-PTOOLS2 S1 widens it), plus `PATTERN IS EMPTY` for a pattern the project lacks.
- **A14 Pads and preview.**
  - Pads on the DATA field of a NOTE row: the note, entered as the grid's pad entry does in STEP.
  - Pads on VAL: the 0..127 ramp, as the Vol pads.
  - Pads anywhere else: the row's step sounds.
  - F5 held: PREVIEW, the row's step sounds (the grid's `pattern.preview`).
- **A15 No row table.** The list is projected from the steps on each redraw. The view caches the first visible row's anchor (step and index) with the pattern's generation. The worst case is 16 tracks x 128 steps x 3 cells = 6,144 cells read; the sort inside one step uses at most 48 keys on the stack.
- **A16 Ids.** `OverlayId::Events = 15` (15..19 are free; `ids.h:58`). `Context::Events = 41` (41, 42 and 50 are free; `bindings.h`). The list captures input, like ToolsList.
- **A17 QA ids.** QA-PAT-400..429 are reserved for this item, so they cannot collide with rows that OS-PTOOLS2 adds at the same time.
- **A18 TOOLS entry.** `EVENTS` is the fifth fixed entry, after SHRINK /2. EXPORT SELECTION (a block only) moves from index 4 to 5; scenarios that reach it with Down presses are updated in S1.
- **A19 Gate.** A LEN edit on a gated note is refused with `THE GATE FX SETS THIS LENGTH`. A sub-step LEN through a Gate FX waits for phase 1 (true lengths).
- **A20 LEN edits** move the end by whole steps, between the note's next step and the next note (or the track's end). Moving past that removes the end: the note then runs to the next note. An end keeps its kind (OFF, CUT, FADE). A new end is an OFF with no Micro-move. When an end leaves a step, its Micro-move goes with it.
- **A21 TIME edits.**
  - A NOTE moves with its owned bytes and its LEN (its end moves by the same whole steps). Its CC rows stay on their step.
  - Moving a note needs an empty note column at the target, and enough free FX columns for the FX it carries. Each FX keeps its column when it is free, else takes the other one.
  - A CC row moves by whole steps only, because a CC has no sub-step time (fact F6). It keeps its column when that is free, else takes the other one, else the move is refused with `STEP 010: NO FREE FX COLUMN`.
- **A22 Fixed pattern.** The pattern is fixed while the list is open. F1 is FILTER there, so the PATTERN chooser cannot be reached.
- **A23 Not in phase 0:** lanes, recording, sub-step CC, more than one note per step, pitch-bend rows (Micro-tune), quantise strength, copy and paste of rows, a range of rows, Standard MIDI File import or export.

## 1. Goal

A precise editor for the MIDI side of a pattern, without leaving the tracker model. One list shows a track (or a block) as MIDI events in time order: notes with their exact start (step plus Micro-move) and length, and the CC A..F the steps send, with the controller and channel they really reach. Every edit writes ordinary steps through the Editor, so undo, save, the OG export and the sequencer see nothing new. Phase 1 can later put lanes behind the same screen.

## 2. Facts from our code

- **F1 Step model.** A step is 7 bytes: note, instrument, volume, and two FX columns of (type, value) (`model/include/o3os/model/step.h:47-58`). A track holds one note per step. A pattern is 16 tracks of up to 128 steps, each track with its own length (`model/include/o3os/model/pattern.h:12-20`, `limits.h:14,21`). This is this OS's model (ADR 0004). It matches the OG step fields: verified parity of the OG file format (`docs/formats/og.md:135`).
- **F2 CC FX.** Types 8..12 are MIDI CC A..E. Type 37 is CC F (`model/include/o3os/model/fx_table.h:57-61,86`; `seq/include/o3os/seq/fx.h:46,105`: `kSlot37IsProgramChange = false`). What the official firmware sends for type 37 is an open research question (`fx_table.h:24`: the release notes call it MIDI CC F from 1.3.0, not verified).
- **F3 Where a CC goes** (`seq/src/sequencer.cpp:257-290` `emit_cc`; `seq/include/o3os/seq/midi_route.h:13,55-69`). This is new Community OS behaviour (OS-INSTUI S8b).
  - On a MIDI instrument, `midi_cc[slot]` names the target: a controller 0..127, PROG, channel aftertouch, or poly aftertouch on the note the track is sounding (ignored and counted when there is none). The CC goes out on the instrument's channel, port and offset.
  - The Config value (0), and any instrument that is not MIDI, use the project's number for the slot (`MidiState::cc_number`, `model/include/o3os/model/project_state.h:43`) on the device's notes channel. An unassigned number sends nothing.
- **F4 The channel belongs to the instrument** (`model/include/o3os/model/instrument.h:114`; `midi_route.h:28-31`). In an OG project, slots 49..64 are the MIDI instruments of channels 1..16 (`docs/formats/og.md:61`; `formats/src/og_project.cpp:219-228`): verified parity of the OG project layout. Our type chooser can also make any free slot a MIDI instrument on any channel (`app/src/instrument_actions.cpp:372`): new Community OS behaviour. The channel a step plays on is therefore derived, never stored in the step.
- **F5 The instrument a step plays** is its own, else the track's last (`sequencer.cpp:259`: `k.row.instr != 0u ? ... : k.last_slot`). The last one can come from an earlier pattern.
- **F6 Micro-move delays only the note.** Its value 0..100 starts the note at microstep `(v + 1) x 48 / 101 + 1` of 48, which is a delay of 0..46 microsteps (`seq/include/o3os/seq/fx.h:262-269`; `seq/include/o3os/seq/sequencer.h:111`). The row's FX run at its first microstep, so the CCs of a micro-moved step still go out at the step's start (`sequencer.cpp:2254-2271` `play_microstep`; the CC case `:1885-1892`; Micro-move `:1847-1851`). This is this OS's sequencer rule. Whether the official firmware delays the CCs of a micro-moved step is an open research question.
- **F7 Recording writes Micro-move** with `rec::micro_value(offset)` (`app/include/o3os/app/rec_place.h:76-84`): FX 2 only, a zero offset writes none (OS-RECMICRO 2.2-2.4; this OS's scaling, new Community OS behaviour). The spec author checked the two formulas: for offsets 0..46, `micromove_start(micro_value(o)) - 1 = o`. Offset 47 cannot be reached, because the FX's top is 46 microsteps. S2 turns this check into a host test.
- **F8 How long a MIDI note sounds.**
  - A track sounds one note at a time. A new note ends the sounding one (`sequencer.cpp:1662,2008-2012`; PAR-063, "this OS's choice").
  - An OFF, CUT or FADE row sends the MIDI note-off (`start_step` `:2089-2104`; `send_voice_off` `:1738-1745`).
  - A Gate FX makes the note `gate` percent of one step long (`:2054-2061`; the mapping is an open item of the sequencer).
  - A mute ends the note, and so does the 9,999-microstep limit.
  This is new Community OS behaviour; the official firmware's rule is an open research question.
- **F9 Undo.**
  - The Editor journals a step edit as 48 bytes (`model/include/o3os/model/editor.h:53,161`); the PATTERN ring holds 160 KiB (`model/include/o3os/model/undo.h:46`).
  - Every dispatched undoable action is one unit, in the area of its screen (`app/src/actions.cpp:445-470`). Only parameter edits use click groups (`app/src/param_actions.cpp:152-155`); an encoder detent on the grid is a unit of its own.
  - Undo covers pattern edits, parameter changes and sample edits (maintainer, 2026-10-22, OS-QANET row).
- **F10 TOOLS today.**
  - The list has four fixed entries, plus EXPORT SELECTION while a block is active (`app/include/o3os/app/tools_list.h:22-42`).
  - `tools_open` refuses while the view follows the play line (`app/src/tools_actions.cpp:19-37`), and the length ops need Edit (`:73-83`).
  - Its context binds Up / Down, the encoder, the page keys, F1 and F8 (`app/src/bindings.cpp:1029-1036`).
- **F11 An overlay can cover the grid.** It declares a cover rectangle and the base screen leaves out what lies inside it (`app/src/screens/help_overlay.cpp:4,72`; `PatternScreen::render` hands `covered` to the grid draw, `app/src/screens/pattern_screen.cpp:131,187`).
- **F12 Display-list budget.** Every state stays at or below 1,792 words in every theme and both translate modes (`docs/ui/style-guide.md` section 8; `sim/include/o3os/sim/screens.h:22`). A note is one tile word.
- **F13 Flash.** v1.1.0 used 94.14 % (`docs/STATUS.md:7`).
- **F14 The official firmware.** Its Pattern pages have no event list (OS-PTOOLS2 section 1, an inventory of tracker-ce's three F-key pages). EVENTS as a whole is new Community OS behaviour.

## 3. Design

### 3.1 Opening and leaving

The user selects a block (or none), then presses F7 TOOLS, picks EVENTS and presses F1. The list covers the grid pane: the strip, the info line and the key row stay. The cursor starts on the first row at or after the grid cursor's step. F8 BACK closes the list and puts the grid cursor on the row's step, track and column (the note column for a NOTE or end row, the CC's FX column for a CC row). Screen buttons are swallowed, as under every modal overlay (`docs/ui/ui-map.md` section 3 item 10).

### 3.2 Rows

A row is one of the following:

- NOTE: a note step, with the start from the step and its Micro-move, LEN from A5, VAL from the volume column, and the instrument (own, or inherited and dim);
- CC: one FX column of type 8..12 or 37;
- OFF, CUT or FADE: an end that ends no note above it.

The row's identity is the key (track, step, kind, column). The cursor keeps it across edits, undo and re-projection; when the key is gone, it goes to the nearest row in time. FILTER (F1) shows ALL, NOTES (NOTE and end rows) or CC.

### 3.3 Columns and layout (Q3 a)

```
+------------------------------------------------------------------------------------------------+
| PAT 01/1 | BPM 120.0 | PLAY | REC | LEN 064 JUMP 01 | INS 51 MIDI 3              CPU 41 MEM 12  |
| EVENTS   PATTERN 01   TRACK 4   STEPS 001-064     23 EVENTS   ALL                      05/23   |
|    TIME    TYPE  CH  DATA  VAL  LEN     IN FL                                                   |
|    001.00  CC A  03  #074  064          51                                                      |
|    001.00  NOTE  03  C-4   100  04.00   51                                                      |
|  > 005.00  CC B  03  PROG  012          51       <- the playing step (Accent bar, Q5 a)         |
|    009.00  CC C  03  #071  100          51                                                      |
|   [009.50] NOTE  03  E-4   ---  02.00   51 +     <- the cursor row, the TIME field boxed        |
|    013.00  NOTE  03  G-4   090  >52.00  51       (> : plays on past the track's end)          |
|    017.00  OFF   03                     51       (an OFF that ends no note above it)            |
|    ...                                                                                           |
|------------------------------------------------------------------------------------------------|
| TIME 009.50  ENC: 1/48 STEP  SHIFT+ENC: A STEP  ALSO: CHORD 47           STEP     undo 12       |
| F1 ALL 1/3  F2 QUANT  F3 -  F4 -  F5 PREVIEW  F6 -  F7 UNDO  F8 BACK                            |
+------------------------------------------------------------------------------------------------+
```

| Column | Chars | NOTE row | CC row | OFF / CUT / FADE row |
|---|---|---|---|---|
| TIME | 6 | step.percent (A1) | step`.00` | step.percent |
| TRK | 2 | only when the scope has more than one track | same | same |
| TYPE | 4 | `NOTE` | `CC A`..`CC F` | `OFF`, `CUT`, `FADE` |
| CH | 2 | the instrument's channel; `--` when not MIDI (Q2 a) | the channel it goes out on (F3), read-only | dim |
| DATA | 4 | the note (a note tile) | `#074`, `PROG`, `CHAT`, `PAT`, `--` (F3) | - |
| VAL | 3 | the volume, `---` unset (A7) | the value 0..127 | - |
| LEN | 6 | `SS.PP`, `>SS.PP` (A5) | - | - |
| IN | 2 | the instrument, dim when inherited, `?` unknown (A8) | the step's instrument, read-only | - |
| FL | 2 | `+` other FX on the step (the info line names them), `G` Gate | - | - |

Rows of a non-MIDI instrument are dim (Q2 a). The header gives the scope, the count under the filter and the position.

### 3.4 Keys

| Key | In the list |
|---|---|
| Up / Down | the previous or next row (held: repeats, as in every list) |
| Shift + Up / Down | a page of 16 rows (OS-NAV3) |
| Left / Right | the previous or next field of the row (TIME, TYPE, CH, DATA, VAL, LEN, IN); read-only fields are skipped |
| encoder | the field's value: TIME one microstep (a CC: one step), NOTE a semitone, VAL 1, LEN a step, TYPE the next CC letter, CH the next channel (Q1 b), IN the next instrument |
| Shift + encoder | coarse: TIME a step, NOTE an octave, VAL 10, LEN 4 steps |
| pads | A14 |
| Insert / Shift + Insert | A9 |
| Delete | A10 |
| F1 | FILTER: ALL 1/3, NOTES 2/3, CC 3/3 |
| F2 / Shift + F2 | QUANT the row / every NOTE and end row the filter shows (S3) |
| F5 (held) | PREVIEW: the row's step sounds |
| F7 / Shift + F7 | UNDO / REDO in the PATTERN area |
| F8 | BACK (3.1) |
| Rec | STEP on or off, as on the grid. Shift + Rec (LIVE) closes the list first, because TOOLS are refused in LIVE |

Shift row: `- QUANT ALL - - - - REDO -`. Every Shift binding has its label (`shift_labels_test`).

### 3.5 Edit-through rules (S2)

| Field | NOTE row | CC row | End row (OFF / CUT / FADE) |
|---|---|---|---|
| TIME | one microstep through the Micro-move (A6). Crossing `.96` or `.00` moves the note to the next or previous step at `.00` or `.96`, when the move rule allows it (A21; else `STEP 010 HOLDS A NOTE` or the column message). Shift + encoder: a whole step, with its LEN | whole steps (A21) | as a NOTE, without LEN |
| TYPE | - | CC A..F: types 8..12 and 37; the value stays | - |
| CH | Q1 b: writes the step's instrument | read-only | - |
| DATA | the note (`model::edit::note_delta`, `note_set`; pads) | read-only (the Instrument's CC page sets the target) | - |
| VAL | the volume column (`volume_delta`, `volume_set`; pad ramp) | the FX value 0..127 (`fx_value_delta`, `fx_value_set`; pad ramp) | - |
| LEN | A20; refused on a Gate (A19) | - | - |
| IN | `instrument_delta` (the grid's Instr walk) | read-only | - |

Each edit is planned by a pure function in `model/` as either a refusal (with its text) or at most four step writes. The action applies the writes through `Editor::set_step` inside its dispatch unit. The planner checks invariants I1-I3 (A11). When I2's one-voice exception applies, the strip says so.

### 3.6 Undo

A12. The unit's name is the action's (`UNDO EVENT TIME`, `UNDO EVENT INSERT`, ...), as other pattern units name theirs. After an undo or a redo the list projects again from the generation and the cursor keeps its key (3.2).

### 3.7 While playing (Q5 a)

When the pattern on show is the one that plays, a 4 px Accent bar in the left margin marks the rows of each track's playing step (the song step modulo the track's length). The list redraws when that step changes, not every microstep: at 130 BPM and four steps a beat, about nine redraws a second. The cursor and the window never move on their own. Edits work as on the grid: the sequencer reads a row by its generation at the row's first microstep, so an edit to the playing step is heard on the next pass.

### 3.8 Display list

- Estimate: the chrome (strip, info line, key row) about 400 words, the header row about 40, and 16 rows of about 34 words each (about 29 glyphs and the colour changes; a note is one tile word), about 550 in all.
- Total: about 1,000 words translated and about 1,250 with the VERTEX2F fallback [est], against the 1,792 limit.
- The grid draws nothing under the cover (F11).
- S1 adds an `events_worst` sweep to the budget harness: every row kind, the widest values, dim rows, the TRK column, the cursor on every field, the play marker, every theme, both modes.

### 3.9 Memory, flash, CPU

- **SRAM:** about +24 B static (the view state in `ViewState`), and at most 256 B of main-loop stack while projecting (A15). SDRAM: 0.
- **Flash:** about 3 KB (S1), 3 KB (S2) and 0.6 KB (S3) [est], about 6.6 KB in all. Cap: 8 KB for the item (risk R1).
- **CPU:** nothing in the render or in an ISR. In the main loop, one projection reads at most 6,144 cells, about 0.3 ms at 180 MHz [est], once per redraw. An edit is at most four `set_step` calls.

### 3.10 What phase 1 keeps

- The screen, the keys and the row key (track, step, kind, column), with a lane index added later.
- In phase 1 the CH column becomes stored, LEN becomes a true length, and a time may hold several notes.
- Phase 0 writes no phase 1 code and no source abstraction beyond the projection function.

## 4. Slices

| # | Slice | Size | Level | Acceptance (short) | Flash / SRAM / CPU [est] |
|---|---|---|---|---|---|
| S1 OS-MIDIREC-A | EVENTS read-only: projection, overlay, open and BACK, scroll, fields, FILTER, play marker, PREVIEW | M | B | QA-PAT-400..411 sim PASS; host tests green; budget sweep at or below 1,792; no undo unit from any key | +3 KB / +24 B / 0 audio, at most 0.3 ms per redraw |
| S2 OS-MIDIREC-B | Edit-through: every field, Insert, Delete, pads, UNDO and REDO inside, refusals | M | B | QA-PAT-412..425; invariants I1-I3 hold in a property test; the MIDI out checked on the unit | +3 KB / 0 / 0 audio |
| S3 OS-MIDIREC-C | QUANT (the row, every row) and the manual section completed | S | B | QA-PAT-426..428; one undo unit; count message | +0.6 KB / 0 / 0 |

Order: S1, S2, S3, one branch each (`feat/OS-MIDIREC-A-events-view`, ...). S3 can share S2's branch as a second commit. Every slice is Level B: no format, no audio path, no ABI. One `firmware-reviewer` per slice; one full `tools/ci.sh host` per branch.

### S1: EVENTS read-only (M, B)

- **Files:**
  - `model/include/o3os/model/event_list.h` and `model/src/event_list.cpp` (new, pure): scope, projection, order, ownership, LEN, the inputs of CC resolution;
  - `app/include/o3os/app/events_view.h` (new): the view state (scope, cursor key, top, field, filter, anchor);
  - `app/src/screens/events_overlay.cpp` (new): render, cover rectangle, play marker; `app/src/screens/overlays.cpp`;
  - `app/src/events_actions.cpp` (new): open, move, page, field, filter, back, preview;
  - `app/include/o3os/app/tools_list.h` and `app/src/tools_actions.cpp`: `Kind::Events` (A18);
  - `app/include/o3os/app/screens/ids.h`, `app/include/o3os/app/bindings.h`, `app/src/bindings.cpp` (A16);
  - `app/include/o3os/app/view_state.h`, `app/src/help_text.cpp`;
  - `sim/src/screens_app.cpp` (the budget sweep), `sim/scenarios/qa/qa_pattern_events.txt` (new), `sim/goldens.txt`;
  - `tests/event_list_test.cpp` (new), `tests/events_app_test.cpp` (new), `tests/key_prompt_test.cpp:308` (the overlay list), `tests/CMakeLists.txt`;
  - `docs/qa/catalogue/pattern.md`, `docs/ui/ui-map.md` (section 1, Overlays), `docs/manual/pattern_tools.md` (the view).
- **Host tests:**
  - `event_list_test`:
    - order (A3);
    - ownership and ends (A4, A5): an OFF folded, an end row shown when nothing above it, CUT and FADE;
    - LEN: the next note, `>`, Gate;
    - CC resolution: controller, PROG, CHAT, PAT, Config number, unassigned, non-MIDI;
    - CH and IN: own, inherited, `?`;
    - scope: track, block, column block widened, track lengths;
    - filter counts;
    - index to key and key to index round trips;
    - a full 16 x 128 pattern counts 6,144 cells and stays inside the stack bound.
  - `events_app_test`: opening with and without a block, the refusals (NO PROJECT, PATTERN IS EMPTY, the follow rule of `tools_open`), the BACK landing, the FILTER labels, no undo unit, the key row and Shift row labels.
- **Sim:** `qa_pattern_events.txt`, snaps `qa_pat_400`..`qa_pat_410`. Budget `events_worst`.

### S2: Edit-through (M, B)

- **Files:**
  - `model/src/event_list.cpp`: the edit planners (TIME, TYPE, CH, DATA, VAL, LEN, IN, Insert, Delete), each giving a refusal or at most four step writes;
  - `app/src/events_actions.cpp` and `app/src/bindings.cpp`: the edit actions, the pads, F7 and Shift + F7;
  - the tests, the scenario, the catalogue, the help text and the manual;
  - `platform/mk66/tools/hil/qa_events.json` (new).
- **Host tests:**
  - `event_list_test`:
    - every rule of 3.5 and A6, A9, A10, A19-A21, with each refusal text;
    - `micromove_start(micro_value(o)) - 1 == o` for o in 0..46 (F7);
    - a property test: random patterns (notes, ends, CC FX, Micro-move, other FX, two instruments, short tracks) and random edits, checking I1 (only the planned steps differ), I2 (every other note's start and LEN are equal, unless the one-voice exception is reported) and I3 (apply, then undo through an Editor: byte-identical).
  - `events_app_test`: one unit per action with its name, `TURN EDIT ON` in WATCH, the cursor follows a moved row, F7 / Shift + F7 inside, Q1 b (the instrument pick, the refusal).
- **Sim:** snaps `qa_pat_412`..`qa_pat_424`.
- **Unit:** `qa_events.json` › `qa_pat_425`: an `amidi` capture of a step with a CC and a micro-moved note (F6). This is the remaining hardware check.

### S3: QUANT (S, B)

- **Rule:**
  - On a NOTE or end row: an offset under 24 microsteps snaps back to `.00` (the Micro-move goes); 24 or more moves the start to the next step at `.00`, by the TIME move rule.
  - A refused move leaves the row as it is and is counted.
  - A CC row says `CC IS ON THE STEP`.
  - Owned ends are not moved (starts only).
  - Shift + F2 applies the rule to every NOTE and end row the filter shows, as one unit, and the strip says `QUANTISED 12, 2 LEFT (STEP TAKEN)`.
- **Files:** `event_list.cpp`, `events_actions.cpp`, `bindings.cpp`, the tests, the scenario, the catalogue, the manual, `qa_events.json`.
- **Host tests:** the snap boundary (23 and 24 microsteps), a refused move, all rows as one unit, I1-I3 for the whole operation.
- **Sim:** `qa_pat_426`..`qa_pat_428`.

## 5. QA catalogue rows (to add to `docs/qa/catalogue/pattern.md`)

The Sim, Unit, Spec (`OS-MIDIREC`) and Status cells are filled when each slice merges (`tools/qa_cover.py --check`).

| ID | Function | How (gesture) | Expected | Slice |
|---|---|---|---|---|
| QA-PAT-400 | Open EVENTS on the cursor's track | no block, `F7`, EVENTS, `F1` | the pane lists the track's events in time order; the header reads the pattern, track, steps, count and `ALL`; the cursor is on the first row at or after the grid cursor's step; no undo unit | S1 |
| QA-PAT-401 | Open over a block | a block over tracks 2-4, rows 1-16, then EVENTS | a TRK column; only those rows; a column block lists whole steps | S1 |
| QA-PAT-402 | A note is one row with a LEN | C-4 on step 1, OFF on step 5 | one NOTE row, LEN `04.00`, no OFF row; a note with nothing below reads `>` and the steps to the track's end; an OFF with no note above is an OFF row | S1 |
| QA-PAT-403 | Micro-move in TIME | a note on step 9 with Micro-move 50 | TIME `009.50`, listed after the CC rows of step 9 | S1 |
| QA-PAT-404 | A CC row says what goes out | CC A on a MIDI instrument whose CC A is controller 74; CC B set to PROG; CC C on Config (number 71); a CC on a sample instrument | `#074`, `PROG`, `#071`; the sample row dim with the Config number and the notes channel; an unassigned number reads `--` | S1 |
| QA-PAT-405 | CH and IN | notes of instrument 51 (MIDI channel 3) and 01 (sample); a step with no instrument | CH `03`; the sample row dim, CH `--`; the inherited instrument dim, `?` at the top | S1 |
| QA-PAT-406 | Other FX on a note | Chord 47 on the note's step | the flag `+`; the info line on the row says `ALSO: CHORD 47`; a Gate shows `G` and the gated LEN | S1 |
| QA-PAT-407 | FILTER | `F1` three times | ALL 1/3, NOTES 2/3, CC 3/3, ALL; the header count follows | S1 |
| QA-PAT-408 | BACK | `F8` on a CC B row | the grid, the cursor on that step, track and the FX column that holds CC B | S1 |
| QA-PAT-409 | Refusals and an empty scope | no project; a pattern the project lacks; WATCH while playing; an empty track | `NO PROJECT`; `PATTERN IS EMPTY`; the `tools_open` refusal; an empty list with `NO EVENTS: INSERT ADDS ONE` | S1 |
| QA-PAT-410 | While playing | STEP, `Play`, the list open | the marker moves with the playing step; the cursor and the window stay (Q5 a) | S1 |
| QA-PAT-411 | Display list | budget sweep `events_worst` | at or below 1,792 words in every theme, both modes | S1 |
| QA-PAT-412 | Edit a note | STEP, DATA of a NOTE row: encoder, Shift + encoder, a pad | a semitone, an octave, the pad's note; heard as on the grid; one undo unit each | S2 |
| QA-PAT-413 | Edit the velocity | VAL from `---`, encoder +1 | 127, then down; pads ramp 0..127 | S2 |
| QA-PAT-414 | Move a note by microsteps | TIME of `009.00`, encoder +24 | `009.50`; the grid shows a Micro-move in FX 2; back to `.00` removes it; past `.96` the note goes to step 10 when its note column is free, else `STEP 010 HOLDS A NOTE` | S2 |
| QA-PAT-415 | Move a note by steps | Shift + encoder on TIME | the note and its end move one step; its CC rows stay; the note above keeps its LEN | S2 |
| QA-PAT-416 | Move a CC | encoder on a CC row's TIME | one step at a time; the same column, else the other, else `STEP 010: NO FREE FX COLUMN` | S2 |
| QA-PAT-417 | LEN | encoder on LEN | the OFF moves a step; up to the next note the OFF goes; a gated note says `THE GATE FX SETS THIS LENGTH` | S2 |
| QA-PAT-418 | CC letter and value | encoder on TYPE, then on VAL | CC A..F (37 for F); 0..127, pads ramp | S2 |
| QA-PAT-419 | Channel of a note (Q1 b) | encoder on CH | the step's instrument becomes the MIDI instrument on that channel (IN follows); none: `NO MIDI INSTRUMENT ON CH 05`; CH of a CC row does not move | S2 |
| QA-PAT-420 | Instrument | encoder on IN | the grid's Instr walk | S2 |
| QA-PAT-421 | Insert | `Insert` on a NOTE row, a CC row, an empty list; `Shift + Insert` | A9; the cursor on the new row | S2 |
| QA-PAT-422 | Delete | `Delete` on a NOTE, CC and OFF row | A10; the note above keeps its LEN | S2 |
| QA-PAT-423 | Undo inside the list | `F7`, `Shift + F7`; `F8` on the grid after BACK | each edit is one named unit; the list projects again; the cursor stays on the event | S2 |
| QA-PAT-424 | Edits need STEP | WATCH, any edit key | `TURN EDIT ON`; nothing written; no unit | S2 |
| QA-PAT-425 | The MIDI out after list edits | unit: a step with a CC and a note moved to `.50`, `Play`, an `amidi` capture | the CC at the step's start, the note-on about half a step later, the note-off at the LEN's end | S2 |
| QA-PAT-426 | QUANT a row | `F2` on `009.50`, then on `011.20` | `010.00` (step 10 free); `011.00` | S3 |
| QA-PAT-427 | QUANT every row | `Shift + F2` | every NOTE and end row the filter shows; one unit; `QUANTISED n, m LEFT (STEP TAKEN)` | S3 |
| QA-PAT-428 | QUANT on a CC row | `F2` on a CC row | `CC IS ON THE STEP`; nothing written | S3 |

## 6. Risks

- **R1 Flash.** At 94 % used (F13), about 6.6 KB [est] is about a tenth of what is left. Each slice records its delta. The reviewer flags a slice over +3.5 KB, and the item stops at 8 KB for the orchestrator to decide. The levers: share the field formatters with the grid's info line, and keep the refusal texts few.
- **R2 Rules that surprise.** Edit-through over one note per step and two FX columns means refusals (`HOLDS A NOTE`, `NO FREE FX COLUMN`) and moves that leave CC rows behind. Mitigations: each refusal says why; I1-I3 are property-tested; the grid shows the truth after BACK; the manual section explains the model in five lines.
- **R3 Static and running instrument differ.** Instrument 0 inherits the last one played, which can come from another pattern (F5). The list shows the pattern's own guess dim, or `?`.
- **R4 Not the whole step.** FX on a step with no note (a Micro-tune bend, a filter) are not listed (A4). The `+` flag covers note steps only. The manual says so.
- **R5 Polymetric tracks.** The list shows stored steps, not the repeats of a short track. The marker uses each track's own step.
- **R6 Edits at the playhead** behave as grid edits do: a note moved across the playing step can be skipped or heard twice on one pass.
- **R7 OS-PTOOLS2 coupling.** S1 adds an entry to a list that S3 of OS-PTOOLS2 replaces. Its menu must find a pad for EVENTS (Q4). The TOOLS-while-playing change of OS-PTOOLS2 S1 reaches EVENTS through the shared open path.
- **R8 Undo memory.** An encoder sweep is one unit per detent (48 B each; the 160 KiB ring holds about 3,400). A TIME move is at most four records.
- **R9 Phase 1 creep.** No lane, `MEVT`, recording or multi-note code in phase 0 (A23). The row key leaves room for a lane index.

## 7. Evidence

- Facts about our code: F1-F13, with `path:line`.
- Verified parity: the OG step fields and MIDI instruments 49..64 for channels 1..16 (the OG file format, `docs/formats/og.md:61,135`).
- Open research questions, none claimed: what the official firmware sends for FX 37 (F2), whether it delays the CCs of a micro-moved step (F6), how it ends a MIDI note (F8).
- New Community OS behaviour: the EVENTS list, its rows, keys, rules (A1-A23) and messages. The official firmware has no event list (F14).
- Every flash, SRAM, CPU and display-list number in sections 3.8, 3.9 and 4 is an estimate [est]. S1 measures the display list in the budget harness; the unit measures the main-loop time (bench B1 of the radar card) during S2's hardware check.

## 8. State for implementer

**Read first (excerpts):**

- this spec: the decision table, A1-A23 and section 3;
- `app/src/tools_actions.cpp:19-104` (open and run), `app/include/o3os/app/tools_list.h:22-73`;
- `app/src/screens/help_overlay.cpp:1-80` (a cover rectangle over the whole pane), `app/src/screens/overlays.cpp:40-65`;
- `app/src/bindings.cpp:1029-1036` (a capturing overlay context);
- `app/include/o3os/app/pattern_edit.h:22-58` (`Selection::block`);
- `model/include/o3os/model/step.h`, `model/include/o3os/model/step_edit.h:27-86` (`note_delta`, `volume_delta`, `fx_value_set`, `instrument_delta`, `clear`), `model/include/o3os/model/editor.h:15-60,161`;
- `app/include/o3os/app/rec_place.h:51-110` (`micro_value`, the FX 2 rule);
- `seq/include/o3os/seq/fx.h:255-270`, `seq/include/o3os/seq/midi_route.h`, `seq/src/sequencer.cpp:257-290,1813-1900,2003-2110,2225-2275` (what plays when).

**Facts to keep in mind:**

- A step is 7 packed bytes with one note and two FX columns.
- CC FX are types 8..12 and 37. Micro-move is type 2 and delays the note only; the CCs of the step go at the step's start.
- OFF, CUT and FADE all end a MIDI note. A new note ends the previous one on the track.
- Instrument 0 inherits.
- The channel comes from the instrument (OG slots 49..64 = channels 1..16).
- Undo is one unit per dispatched action, and `set_step` costs 48 B of journal.
- The display list may hold at most 1,792 words. The base grid skips everything under the cover rectangle.

**Build order:**

1. S1, pure first: `event_list.h/.cpp` with `event_list_test` (projection, order, ownership, LEN, CC targets, scope). Then the view state, the overlay and the actions, the bindings, the TOOLS entry, the budget sweep and the scenario.
2. S2: the planners as pure functions returning a refusal or at most four `(track, step, Step)` writes. The property test (I1-I3) comes before the actions.
3. S3: QUANT reuses the TIME planner.

**Do not:**

- change any format or `Step`;
- touch the sequencer or the render path;
- add a key to the Pattern grid;
- allocate;
- keep a row table in SRAM (A15);
- add phase 1 code;
- read the official manual (maintainer rule).

**Measure:** the flash and SRAM delta of each slice (`fw-build`), the `budget` ctest with `events_worst`, and, on the unit in S2, `qa_events.json` with an `amidi` capture.

**Docs per slice:** the catalogue rows (section 5), the help text, `docs/manual/pattern_tools.md` (an EVENTS section, in the manual's voice: what is new for an owner of the official firmware), and `docs/ui/ui-map.md` section 1 (the overlay and its keys).
