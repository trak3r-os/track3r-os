# OS-FXREC — record MIDI Learn moves into the step FX during LIVE recording (shape A)

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a, Q3 a, Q4 a), not implemented · Level B (no format change: the FX are ordinary step FX of the pattern file) · Backlog row OS-FXREC, approved by the maintainer 2026-11-05 (IDEA triage): **shape A only**; shape B (Learn on an FX cell, an LRNB meaning change, Level C) stays an idea and is not specified here · Facts from `dev` at c1dc3125 and `tracker-ce@7fd6c2c` · Everything here is **new Community OS behaviour**: the official firmware's CC handlers write no step FX (F12).

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a, Q4 a (every recommended default).**

The row's proposal is kept: one value per step, only on steps where the knob moved, the column that already holds the FX, else the first free one, never over another FX (3.3). What the row left open:

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | Undo of a recorded pass, and the knob's own move of the instrument parameter during it | a) one Undo removes the whole take: the notes **and** the recorded FX; the knob's moves of the parameter during the take are final (applied, not journalled), so the parameter stays where the knob is · b) the parameter moves join the take too: one Undo also puts the parameter back (instrument records in the PATTERN ring, about twice the journal) · c) as today: each CC message closes the take and is its own SOUND unit; the FX are written as their own PATTERN units | a | Keeps the maintainer's whole-take rule (one Undo = the pass) and the knob and the parameter agree afterwards (Pickup stays caught). Precedent: `mixer.master_vol` already keeps a take open with a final edit (F10). b mixes instrument records into the PATTERN ring, where a later SOUND unit on the same parameter would undo out of order; c splits a pass into hundreds of units |
| Q2 | Tune 40 in the map | a) left out of shape A · b) written as 24 + (the knob's TUNE − the instrument's TUNE at the take's start) on steps that hold a note, and the instrument's TUNE put back at the take's end | a | Tune 40 shifts the note **on top of** the instrument's TUNE (F6), which the knob also moved, so a plain value plays the shift twice; it also acts only on a step with a note. b is the only correct form and needs a take-end hook and a second rule set |
| Q3 | Which bindings record | a) a binding whose parameter is of the **selected** instrument (the one the take records notes with: a follow binding always, OS-LEARNFOLLOW; a fixed one when its instrument is selected) · b) any instrument binding, whatever the cursor's track plays | a | The FX acts on the voice of the cursor's track, which plays the selected instrument's notes in the take; a binding of another instrument would put that instrument's motion on the wrong sound |
| Q4 | A switch for it | a) none: always on during a LIVE take for a mapped binding (the strip says `REC FX: CUTOFF` at the take's first FX) · b) a device setting REC FX Off / On (default Off) appended to `settings.o3st` (Level C: the device block is full, OS-MTC's raise) | a | Shape A was approved as Level B with no format change; one Undo removes an unwanted pass; the strip says it happened |

## 1. Goal

Turn a knob that MIDI Learn bound to an instrument parameter while the Pattern screen records LIVE, and the move is written as step FX at the play line of the cursor's track, so the pass plays back the sweep: CUTOFF as a Low-, High- or Band-pass FX by the instrument's filter type, PANNING as FX 31, DELAY SEND 30, REVERB SEND 32, OVERDRIVE 38, BIT DEPTH 39. Entering FX step by step on each track is the pain this removes (the maintainer, 2026-10-21).

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | LIVE recording: only the Pattern screen in Edit following, playing, past the count-in (`pattern_recording`); a note goes to the cursor's track at the playhead's step rounded by REC QUANTIZE (Nearest by default; Playing), with the pattern's loop and a short track's own length; not when the playhead is in another pattern | `app/include/o3os/app/screens/pattern_screen.h:51-64`; `app/include/o3os/app/rec_place.h:8-21`, `:64` (`quantize_row`); `app/src/rec_actions.cpp:53-101` | code fact |
| F2 | The take: one undo group (PATTERN) from the moment recording begins to the moment it ends; an undoable action without `kActionRecords` closes it first and is its own unit; a take holding half the PATTERN ring (160 KiB) is split | `app/include/o3os/app/rec_place.h:19-21`; `app/src/actions.cpp:454-459`; `app/src/rec_actions.cpp:31-35`, `:182-201`; `model/include/o3os/model/undo.h:46` | code fact |
| F3 | `midi.cc` is `kActionUndoableIfOpen` without `kActionRecords`: today a CC during a take closes the take, and its parameter write is a SOUND unit (click-merged per parameter) | `app/src/actions.cpp:169`; `app/src/param_actions.cpp:972-984` | code fact |
| F4 | A step has two FX columns (type, value); FX 27/28/29 are one group (the filter), each of 30, 31, 32, 38, 39 is a group of its own; ranges 0..100, Bit depth 1..16 | `model/include/o3os/model/step.h:42-55`; `seq/include/o3os/seq/fx.h:109-132`, `:149-212` | code fact |
| F5 | Value laws equal the parameter's: the Cutoff FX value / 100 is the cutoff (the instrument's CUTOFF is 0..1); the Panning FX 0..100 replaces the track's pan while held (ModEnd gives the instrument's back); FX 30 / 32 held replace the instrument's sends (0..100); Overdrive 0..100, Bit depth 1..16 as the instrument's fields | `audio/include/o3os/audio/voices.h:92-103`; `audio/include/o3os/audio/events.h:119-131`, `:167-177`; `audio/include/o3os/audio/graph.h:288-292`; `model/src/params.cpp:100-112` | code fact |
| F6 | Tune 40 is read when a note starts and shifts it by value − 24 semitones on top of the instrument's TUNE; on a step without a note it does nothing | `seq/src/sequencer.cpp:46`, `:1922-1927`, `:2127-2131` | code fact |
| F7 | A Voice FX on a step without a note goes to the sounding voice; a note step replaces both columns' FX (a column without one ends its FX), a step without a note replaces only the columns that hold a voice FX or Off | `seq/src/sequencer.cpp:1972-1984`; `seq/include/o3os/seq/control.h:7-14` | code fact (the row's precedent, moved from `:1752`) |
| F8 | REC MICRO writes Micro-move into FX 2 only when FX 2 is free (or already a Micro-move); a user FX is never overwritten | `app/include/o3os/app/rec_place.h:85`, `:100-112`; `tests/rec_test.cpp:273-300` | code fact (the row's precedent) |
| F9 | `apply_cc` resolves the binding (follow: the selected instrument), writes the parameter with `write_param` and knows whether it changed; Pickup returns before any write until the knob has caught the parameter | `app/src/param_actions.cpp:934-985` | code fact |
| F10 | `mixer.master_vol` is `kEditOnceRec`: a LIVE take stays open under it and its edit is final (adds nothing to the take) | `app/src/actions.cpp:24`, `:283` | code fact (precedent for Q1 a) |
| F11 | An Editor edit that changes nothing is applied and not recorded; `set_final(true)` applies without recording | `model/include/o3os/model/editor.h:20-28`, `:46-47` | code fact |
| F12 | The official firmware's CC handlers move the mixer, Perform and instrument values; none writes a pattern step | `tracker-ce@7fd6c2c:core/source/mtMidi.cpp:271-303` and the handlers it calls (`fMasterParams.cpp:3071-3098`, `fPerformance.cpp:880-904`, `fIinstrEdit.cpp:2126-2154`) | verified (tracker-ce): no parity to claim |

## 3. Design

### 3.1 The map (`app/include/o3os/app/rec_fx.h`, pure)

`rec_fx_map(field, filter_type, value, out)` gives the step FX of an instrument parameter's new value, or nothing:

| Parameter (field) | FX | Value |
|---|---|---|
| CUTOFF (7) | 27 Low-pass, 28 High-pass, 29 Band-pass by the instrument's FILTER TYPE 0 / 1 / 2 | `round(cutoff * 100)`, 0..100 |
| PANNING (1) | 31 | the pan, 0..100 |
| DELAY SEND (9) | 30 | 0..100 |
| REVERB SEND (10) | 32 | 0..100 |
| OVERDRIVE (11) | 38 | 0..100 |
| BIT DEPTH (12) | 39 | 4..16 (the FX takes 1..16) |

Tune 40 is not in the map (Q2 a). Every other parameter writes nothing.

### 3.2 When a move records

In `apply_cc`, after `write_param` returned Changed, all of: `rec_active(ctx)` (F1); the resolved parameter is of kind Instrument, its instrument is `ViewState::instrument` (Q3 a) and of kind Sample; `rec_fx_map` gives an FX; `view.play.pattern == view.edit.pattern`. The value is read back from the parameter (what was stored, so what is heard). A message that changes nothing (Pickup not caught, the same value) records nothing, which is "only on steps where the knob moved".

### 3.3 Where it goes

The cursor's track (REC POLY Spill does not spill FX); the row `rec::quantize_row` gives a note at that moment (REC QUANTIZE, the loop, the track's own length, the count-in rule): one shared helper with `rec_note`. The column (`rec_fx_place(Step&, type, value)`, pure): the first column that holds an FX of the same group (for the filter any of 27/28/29, its type replaced by the current filter type), else the first column whose type is 0, else nothing (both hold other FX, a Micro-move included). Later messages on the same step overwrite the value in the same column, so the step keeps the last value of the pass on it: **one value per step**. The step's note, instrument and volume are left alone. Written with `Editor::set_steps` inside the open take (a value equal to the cell's is not recorded, F11).

### 3.4 Undo and the take (Q1 a)

`midi.cc` gets `kActionRecords`, so a CC never closes a take. While a take is open, every parameter write of `midi.cc` is final (`set_final(true)` around `write_param`, as for a Mixer parameter today), and the FX writes are recorded into the take. One Undo on the Pattern screen removes the take's notes, OFFs and FX; the instrument parameter stays where the knob left it, and the knob's Pickup state stays caught (the FX playback never moves the parameter or its generation). Outside a take `midi.cc` behaves exactly as today.

### 3.5 The strip

The take's first recorded FX posts `REC FX: <PARAMETER>` once (a flag in `PatternEdit`, cleared when the take ends); a write dropped for want of a column posts `REC FX: NO FREE FX COLUMN` once per take. Nothing else (a knob sends up to about 100 messages a second).

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| S1 | The map and the column rule (pure) | S | B | +0.4 KiB | — |
| S2 | Wiring, the take rule, scenario, QA rows, manual | S | B | +0.6 KiB | S1 |

**S1** (S, Level B). Files: `app/include/o3os/app/rec_fx.h` (new: `rec_fx_map`, `rec_fx_place`), `tests/rec_fx_test.cpp` (new), the tests' CMake list. Acceptance: 3.1 and the column rule of 3.3. Host tests: every mapped field at both ends of its range and the middle (CUTOFF 0, 0.5, 1 -> 0, 50, 100 for each filter type; BIT DEPTH 4 and 16); TUNE, RESONANCE, VOLUME map to nothing; place: empty step -> column 1; column 1 holds Low-pass 40 -> column 1, the type follows the filter type; column 1 holds Chance, column 2 empty -> column 2; column 1 Chance, column 2 Micro-move -> nothing; column 2 holds Delay send, column 1 empty -> column 2 (the column of the group wins); the note, instrument and volume untouched; a generated table check that every mapped FX type's group is its own or the filter's (`fx_group`). No UI, no QA rows. Unit check: none.

**S2** (S, Level B). Files: `app/src/param_actions.cpp` (`apply_cc`: 3.2 and the final write in a take), `app/src/rec_actions.cpp` and its header (a shared row helper, `rec_fx_write`), `app/src/actions.cpp` (`midi.cc` flags), `app/include/o3os/app/view_state.h` (`PatternEdit`: the two strip flags); `sim/scenarios/qa/qa_pattern.txt` › `qa_pat_324` (LIVE, a follow or fixed binding on CUTOFF, `midicc` at three play positions, the grid shows the FX; golden); `docs/qa/catalogue/pattern.md` QA-PAT-324 (a bound CUTOFF knob records Low-pass FX on the play line of the cursor's track), QA-PAT-325 (the column rule: never over another FX, the strip message), QA-PAT-326 (one Undo removes the take's notes and FX; the parameter stays), QA-PAT-327 (no FX: Edit free, stopped, count-in, another instrument's binding); `docs/manual/pattern.md` (the live recording section: "Record knob moves as FX"), `docs/manual/cheat_sheet.md` (one line). Acceptance: 3.2 to 3.5; with no take open nothing differs from today (the existing Learn and rec tests unchanged). Host tests, `tests/rec_test.cpp` (or `tests/rec_fx_app_test.cpp`) on the app rig with a playing sequencer stub: a binding on CUTOFF (Jump), LIVE: `midicc` at step 3 writes Low-pass on the cursor's track row 3 and nothing on the other tracks; a second message on row 3 changes the value, not the column; REC QUANTIZE Nearest past half the step writes row 4; a High-pass instrument writes 28; a Pickup binding not yet caught writes nothing; a binding of instrument 2 with instrument 1 selected writes nothing (Q3 a); a follow binding writes; Edit free, stopped, count-in, the playhead in another pattern: nothing; a take of notes and FX is one undo unit and Undo leaves the CUTOFF where the knob put it; the journal holds no SOUND unit from the take; outside a take the CC is a SOUND unit as before; TUNE writes nothing. Unit check (the tester): with a USB controller bound to CUTOFF on a sample instrument, record a sweep over one bar LIVE, stop, play: the sweep is heard from the FX alone (knob still), Undo removes it; and a 30 s continuous sweep does not split the take ("UNDO: LONG TAKE IS SPLIT" absent).

Totals [est]: flash about +1 KiB, SRAM 0 (two bytes in the view state, in SDRAM with it), render CPU 0 (main loop only); the journal grows by one step record (about 30 B) per changed value of a step, so a continuous sweep at 50 messages a second fills the take's half ring in about a minute (F2).

## 5. Assumptions (decided here; local, testable, reversible)

- A-1 The FX type of CUTOFF follows the instrument's FILTER TYPE at the moment of the message, whether FILTER is On or Off (the Cutoff FX turns the filter on for the voice, F5).
- A-2 The row is the note's row at that moment (REC QUANTIZE, F1); the count-in's last half step under Nearest writes the start step, as for a note.
- A-3 FX go to the cursor's track only, also with REC POLY Spill.
- A-4 The filter group is matched by group (27/28/29 one parameter); the other mapped FX by their type.
- A-5 Only a Sample instrument records (the mapped fields are sampler fields); MIDI and module instruments record nothing.
- A-6 No new setting, no format change; the recorded FX are ordinary step FX (the OG pattern file carries 27..39 as they are).
- A-7 Mixer and Perform moves (OS-MIDICCMAP, Learn on a track) record nothing; with Q1 a they no longer close a take either.

## 6. Risks

- R1 **One knob, every track**: the knob moves the instrument parameter for every track that plays the instrument, the FX only the cursor's track (the row's open point); after the pass the parameter stays where the knob is. The manual says it; a per-track live override (the Perform layer's control) instead of the parameter is a possible follow-up.
- R2 **Journal growth** of a long sweep (section 4 totals): the existing take split protects the history; the unit check measures a 30 s sweep. A coalescing write once per step (at the step's end) is the follow-up if it splits in normal use.
- R3 **Q1 a changes the take rule for CCs**: a parameter moved by a knob during a take can no longer be undone on its own; the manual says so; QA-PAT-326 pins it.
- R4 **REC MICRO competes for column 2**: a recorded FX in column 2 leaves no room for a later note's Micro-move (F8 skips it); first come, first served, documented.

## 7. Evidence labels

- Recording Learn moves into step FX, the map, the column rule, the undo rule, the strip messages: **new Community OS behaviour** (F12: the official firmware has nothing to compare).
- The FX value laws equal to the parameters' (F5): code facts of this OS; that they **sound** the same as the parameter is checked by the unit check of S2.
- Shape B (Learn on an FX cell): not specified; stays an IDEA.

## 8. State for implementer

- **S1.** Start with: `kFxInfo` and `fx_group` `seq/include/o3os/seq/fx.h:149-232`; `Step` `model/include/o3os/model/step.h:42-55`; `rec::write_note` and `micro_slot_free` `app/include/o3os/app/rec_place.h:85-112` (the style to follow); instrument field ids `model/include/o3os/model/params.h:30-45`. Rules: pure constexpr, no app state; never writes a column that holds another FX.
- **S2.** Start with: `apply_cc` `app/src/param_actions.cpp:934`; `rec_note` `app/src/rec_actions.cpp:53` (the row and the take write); the take close in dispatch `app/src/actions.cpp:454-459`; `mixer.master_vol` flags `app/src/actions.cpp:283`. Rules: main loop only; outside a take `midi.cc` is unchanged; inside one the parameter write is final and only FX steps are journalled; no format, sequencer or render change. Unit check: the tester's sweep, Undo and the 30 s sweep above.
- Report per slice: flash, SRAM deltas; `ctest -R "rec|params_app|learn"`; the unit check left.
- Follow-ups to record, not build: Tune by Q2 b; a per-track live override instead of the instrument parameter (R1); a once-per-step coalesced write (R2); shape B.
