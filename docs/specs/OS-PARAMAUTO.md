# OS-PARAMAUTO — record any encoder parameter as CC A..F step FX

Status: spec, not implemented · Level B · **no format change (route A, section 2)** · Size M, three S slices · Evidence: **new Community OS behaviour**; the official firmware has no counterpart. Builds on OS-ENCLEARN (`docs/specs/OS-ENCLEARN.md`), whose `REC FX: <PARAM> HAS NO FX` case it replaces for the parameters it can cover. BACKLOG row: `docs/roadmap/BACKLOG.md:131`.

## 1. Goal

A parameter with no step FX of its own (VOLUME, PITCH, an envelope time, a module knob) can still be recorded from the encoder (or a Learn CC) as one of the six free step FX, CC A..F (types 8..12 and 37, value 0..127: `model/include/o3os/model/fx_table.h:57-61,86`, `seq/include/o3os/seq/fx.h:46,181`). The sequencer then plays the FX back as a move of that parameter. The maintainer's letter rule (fixed, BACKLOG:131):

- No letter at assign. The first recorded move takes the first free letter of the project; the strip says `REC FX: <PARAM>` then `CC x`.
- A parameter that already has a letter reuses it. A parameter with an FX of its own uses it and takes no letter.
- Six letters taken: `REC FX: NO FREE CC LETTER`; the move still sounds, nothing is recorded.
- The letter belongs to the parameter, not to the encoder. Re-targeting the encoder leaves the old letter as it is; the new parameter takes a free letter at its first recorded move; re-targeting onto the same parameter reuses its letter.

## 2. Where the map lives (route A: decided, feasible)

Today `Sequencer::emit_cc` (`seq/src/sequencer.cpp:269-309`) looks at the instrument the step plays. A MIDI instrument maps letter L through `Instrument::midi_cc[L]` (`model/include/o3os/model/instrument.h:39,137`: 0 = the Config number, 1..128 a controller, 129 program, 130 and 131 aftertouch). Any other instrument (or none) sends `EventKind::MidiCc` with the Config page number; that is the current behaviour of CC FX on sample tracks.

The six bytes are stored for **every** instrument kind: written at `formats/src/native_writer.cpp:~384` (INST 1.3 whenever any byte is non-default), read at `formats/src/native_reader.cpp:735-746`, validated at `model/src/instrument.cpp:112,154-155` (values above `kMidiCcMax` = 131 are an error). `docs/formats/o3-project.md:155` says the bytes of the other kinds stay at their defaults, so on a Sample or Module instrument they are always 0 today and free.

**Decision.** On a non-MIDI instrument `j`, `midi_cc[L]` (L = 0..5 for A..F) is the **parameter code** of the field of instrument j's own parameters that owns letter L; 0 means "no letter". Letters are project-wide: at most one instrument holds a given L (checked when assigning; at load, if a hand-edited file has a letter twice, the lowest instrument slot wins and the later byte is ignored, not rewritten).

| Kind | Code | Range |
|---|---|---|
| Sample | `field + 1` of a learnable instrument field `0x00..0x77` (`model/include/o3os/model/params.h`: base 0..13, source rows, envelopes, mod rows) | 1..120 |
| Module | module parameter index + 1 (field `kModuleFirst + i`) | 1..32 |

Both stay inside 1..131, so the reader and validator accept them unchanged. The decode is `field = code - 1` (Sample) or `kModuleFirst + code - 1` (Module), then `ParamId{Instrument, slot, field}` (`core/include/o3os/core/param.h:47-56`) and `describe_param` for range and the learnable flag. No table is stored; a code whose field is no longer learnable decodes to "none".

**Layout and size: unchanged** (same chunk, same bytes, same 1.3 rule). What changes is the meaning of always-default bytes of non-MIDI instruments. Required with the first slice: the `o3-project.md:155` sentence is corrected, and a round-trip test pins it (section 8). A pre-PARAMAUTO reader keeps and rewrites the bytes (it has no check on the kind) and the letter does nothing there; MIDI instruments are untouched.

**Not coverable under route A** (code space and field space do not fit): VelSens (`0x8B`), the wavetable rows (`0x90..`), and every parameter of kind Track, Master, Insert, Project or Device (mixer volume, pan, sends). They keep the ENCLEARN behaviour, `REC FX: <PARAM> HAS NO FX`. **Maintainer decision needed (optional, not designed here):** covering them needs a new INST minor with a wider code, or a project-level map chunk. Do not start it without a decision; route A as specified stops at the table above.

Rejected stores, for the record: `LRNB` bindings (`core/include/o3os/core/learn.h`: `learn_valid` rejects unknown flags, so a spare marker would change the meaning of an entry older readers drop); `SETS` (16 settings per scope, new rows show in the UI); `MidiState.cc_number[6]` (one u8 per letter, project wide, used by MIDI tracks).

## 3. Old projects

A project written before this item has no non-MIDI `midi_cc` byte set, so no letter is mapped and **nothing changes**: CC FX on sample or non-MIDI tracks keep sending MIDI CC on the Config number, exactly as today (zero migration, no scan). Note for the user (manual and QA row): once a letter is assigned, the *old* CC x steps on non-MIDI tracks of that project move the parameter instead of sending MIDI. A MIDI instrument's CC FX never change. No warning is planned (flash); it is a recorded risk, not a bug.

## 4. Playback path and CPU bound

`Engine::dispatch` (`audio/src/engine.cpp`, SRAM_L) has no room for a new event case, and the graph's ParamChange ring has the main loop as its only producer. So the render side only **publishes**; the main loop applies. Same pattern as `play_position()` / `published_` (`seq/include/o3os/seq/sequencer.h:429-430,756`) and `mute_pending()`.

Render side (in `emit_cc`, for `in == nullptr || kind != Midi`):

- `volatile std::uint8_t auto_mask_` (bit L: letter L is mapped), written by the main loop only.
- If the bit is set the sequencer does not emit `MidiCc`; it stores `auto_val_[L] = value` and sets bit L of `auto_dirty_`. Render is the single writer of `auto_val_`; the main loop clears dirty bits. One byte store plus an OR: about 10 cycles per CC FX step, no allocation, no SD I/O, no loop. Slot 37 (CC F, `sequencer.cpp:~2457-2465`) takes the same branch.
- Cost bound: at most 6 letters x one step per track per block is already the bound of FX per block today; the added work per CC FX step is a constant. No per-block cost when no letter is mapped (mask 0, the existing path).

Main loop (`App::run_once`, once per pass):

1. If `Project::generation()` (`model/include/o3os/model/project.h:221`) changed, rebuild the map: scan 64 instruments x 6 bytes, fill `ParamId auto_ids_[6]` and the mask (a letter seen twice: lowest slot). Not per step, not per pass.
2. Read and clear `auto_dirty_` in a critical section; for each set bit (at most 6 per pass) take `v = auto_val_[L]`, `norm = v / 127`, `param_denorm(*describe_param(id), norm)` and apply it through the `param.auto` action (`param_auto`, `app/src/param_actions.cpp`) to the audio side only: `send_param_change` pushes the value to the audio ring and nothing is written to the Project (as built, OS-BUG-PARAMAUTO-PLAY: see the as-built note at the end of section 4).

Decisions (new Community OS behaviour): the mailbox is written only while the transport plays; the move lands one block plus one main-loop pass after the step (not sample accurate); two steps of one letter inside one pass collapse to the later value; values are 7 bit (127 steps), so a float parameter is quantised to `v/127` of its range. A mapped letter on a non-MIDI instrument no longer sends a MIDI CC.

**As built (OS-BUG-PARAMAUTO-PLAY, 2026-11-12; supersedes the `write_param` apply of step 2).** The first build applied a played letter with `write_param` and `set_final(true)`, which is a Project write: it moved the Project generation, so playing alone marked the project unsaved, replaced the saved base value of the parameter, and made a save that ran during a sweep fail with CHANGED: TRY AGAIN (the writer checks the generation between slices). Now `param_auto` sends the played value with `send_param_change` only and never writes the Project (new Community OS behaviour):

- The action is `kActionRecords` only (no undo unit) and always returns Unchanged; `Session::auto_id[6]` and `auto_live` remember which parameters a letter has moved.
- Stop restores the base: at the end of `App::poll_param_auto`, after the pass's takes, whenever the transport is stopped (the key and the sequencer) and a letter is live, `param.auto` runs with letter `0xFF` (`kAutoRestore`) and re-sends the Project's base value of every moved parameter. A letter remapped during play restores its old parameter first.
- Limit: the restore happens when the transport is stopped, not on a pattern restart or a rewind; the last played value keeps sounding until stop. A value taken in the very pass the song ends is restored in that pass. A restore the audio side drops (full ring, no module instance) keeps its bit and is retried on the next pass.
- Source and descriptor fields (the ones the engine rebuilds from the Project through `request_rebind`) do not sound the played value during play, because the rebuild reads the Project. WtPos, GrainPos, the MOD fields, module params and the plain table do.
- Export: the offline render builds its own sequencer with no letter mailbox, so the mapped letters' moves are not rendered. When an export ends well and the project maps at least one letter, the strip says `CC LETTER MOVES NOT RENDERED` (4 s, Info). Rendering the moves (option A) would cross the seq, app, mk66 and sim wiring of the export; the note is the smaller safe change. Follow-up if wanted: apply the moves in `OfflineRender`.
- Tests: `tests/save_app_test.cpp` (`paramauto play: ...`, two tests), `tests/params_app_test.cpp` (`param auto: ...`), `tests/rec_fx_app_test.cpp` (`paramauto poll`), `tests/file_export_test.cpp` (`export: a project with a mapped CC letter ...`). Hardware check: a sweep and a save during playback, and an export of a project with a mapped letter.

## 5. Recording and strips

`rec_fx_write` (`app/src/rec_actions.cpp:193-283`) is reached from `apply_cc` (`param_actions.cpp:1297`) and `enc_turn` (`:1352`). Today it gates: recording active (LIVE, or STEP through `enc_take`), playhead in the edited pattern, kind Instrument, the selected instrument (Q3a), Sample kind (A-5), then `rec_fx_map` (`app/include/o3os/app/rec_fx.h:48`); a miss is `no_fx()` (`REC FX: <PARAM> HAS NO FX`, once per gesture, `v.edit.enc_said`).

New path, taken only after `rec_fx_map` returns no FX, for an Instrument-kind parameter of a **Sample or Module** instrument (any slot: the Q3a selection rule is for own-FX only) whose field is codable (section 2). Anything else stays `no_fx()`.

1. Find the letter: the instrument's own `midi_cc[L] == code` (reuse), else the first L whose byte is 0 in every non-MIDI instrument and that no MIDI instrument is asked to share (MIDI instruments' own bytes do not count: their letters are controller maps, a separate namespace).
2. None free: strip `REC FX: NO FREE CC LETTER` once per take or gesture (reuse `enc_said` / `rec_fx_said`); the move sounds, nothing is written.
3. A new letter is a write of `instrument.midi_cc[L] = code` through `Editor::set_instrument`, **inside the take's undo group** (LIVE take, or the STEP `enc_take` unit), so Undo of the take removes the steps and the letter together. Then refresh the mask and ids (generation change does it).
4. The step FX: `type = kCcA + L` for L 0..4, type 37 for L 5 (CC F), `value = round(norm * 127)`, placed with `rec_fx_place` (`rec_fx.h:85`: same column rule, `NO FREE FX COLUMN` once per take), written with `Editor::set_steps` inside the open take (`note_take_write`).
5. Strip on the first recorded FX of a take: `REC FX: <PARAM> -> CC x` (once per take, replacing the plain `REC FX: <PARAM>` post).

Modes as ENCLEARN: LIVE (`pattern_recording`) one value per step along the play line; STEP the cursor's step of the cursor's track, one hold gesture one undo unit; WATCH records nothing and assigns no letter. A letter is taken only by a recorded move, never by an assign or by a move in WATCH.

## 6. UI, toasts, undo, evidence

- Assign, use and toasts are ENCLEARN's (`ENCODER: <PARAM> INS <n>`, `<PARAM> <value>`); nothing new to learn. New strips: `REC FX: <PARAM> -> CC x`, `REC FX: NO FREE CC LETTER`, `REC FX: MIDI TRACK` (the instrument the step sounds is a MIDI one: a letter there would send a CC; nothing is written). The letter is journalled as its own 4-byte record (`Editor::set_instrument_cc`) in the take's unit.
- The Fx column shows the step as `CCA`..`CCF` as today. No screen lists the letter map in this item; a later row may (open question 3).
- Undo: the letter write and the steps are one unit per take or gesture (section 5). The parameter value is not restored by Undo (as in a LIVE take today).
- Playback moves of a parameter write no undo record (section 4).
- Evidence: every behaviour here is **new Community OS behaviour**; no parity claim. The letter-to-code store is a documented use of existing bytes (route A).

## 7. Risks and open questions

1. Old steps of a letter change meaning once a letter is assigned (section 3). Accepted, documented.
2. 7-bit quantisation and one-block-plus-pass latency (section 4): a fine sweep of a float parameter plays back stepped.
3. VelSens, wavetable rows and the mixer-kind parameters cannot be recorded (route A limit). Maintainer: leave as is, or approve a format design later. A letter-map screen is also unscheduled.
4. A letter is owned by one instrument's parameter: two instruments cannot both record CUTOFF on A. Six letters in the whole project is the maintainer's rule.
5. A module whose parameter list changes between module versions can make a stored code decode to a different parameter; the code is the module's index, as `kModuleFirst + i` already is for Learn bindings.
6. CPU on the unit is not measured: needs the `cpu-budget.md` check with several CC FX steps per block (hardware check pending). Flash numbers below are estimates, to be measured.

## 8. Slices (each about S) and acceptance

| Slice | Content | Tests | Flash (to be measured) |
|---|---|---|---|
| S1 model and format | `auto_code(ParamId)` and `auto_param(slot, code)` helpers (codable test, Sample and Module), letter scan: `auto_find(project, param)` and `auto_first_free(project)`, load rule for a doubled letter; correct `o3-project.md:155`. | Host: code/field round trip for every Sample field and 32 module slots, uncodable VelSens/WT/mixer rows, first-free with 0..6 taken, doubled letter at load. Format: project with letters on a Sample and a Module instrument round-trips byte-exact (`tests/inst_midi_format_test.cpp` style); existing fixtures keep size and CRC-32; MIDI instrument unchanged. | about 0.3 KB |
| S2 playback | `auto_mask_`, `auto_val_`, `auto_dirty_` in `Sequencer`; the `emit_cc` branch (CC A..E and slot 37); main-loop rebuild on `generation()` and apply (as built: engine only, no Project write; OS-BUG-PARAMAUTO-PLAY). | Host (`seq` test): mapped letter on a sample track emits no `MidiCc` and sets the mailbox; unmapped letter and MIDI instrument unchanged (old-project case); apply writes the denormalised value and no undo record; letter collapse in one pass. Sim scenario in `sim/scenarios/qa/qa_pattern.txt`: pattern with CC A steps moves a Volume value, screenshot. | about 0.6 KB |
| S3 recording | Letter path in `rec_fx_write`; strips; letter write inside the take's undo group; QA rows QA-GLB-233.. in `docs/qa/catalogue/global.md` (QA-GLB-232 is the last). | Host (`tests/enc_learn_test.cpp` style): first move takes A and writes CC A, strip text; second param takes B; same param reuses A; six taken gives NO FREE CC LETTER and no write; re-target keeps old letter; STEP gesture and LIVE take undo in one unit and remove the letter; WATCH assigns nothing; uncodable row still says HAS NO FX. | about 0.6 KB |

Total estimate about 1.5 KB of the 3.7 KB left in the bench build; each slice records its measured delta (`fw-build`). `sizeof(Sequencer)` grows by about 8 B (mask, six values, dirty bits); the ceiling is the `static_assert` at `platform/mk66/src/app_main.cpp:233`, which has room (about 800 B under it since OS-CEWTENV W6a); S2 records the measured size. Hardware check after S3: CPU with CC FX on all tracks, and a listen test of a recorded sweep.

## 9. State for implementer

- Start from `dev` after OS-ENCLEARN is merged (its `enc_take`, `enc_said`, `rec_fx_write` STEP path are the base).
- Files: `model/include/o3os/model/instrument.h` and a small new header (or `params.h`) for the code helpers; `seq/include/o3os/seq/sequencer.h`, `seq/src/sequencer.cpp` (`emit_cc`, dispatch ~2448-2465); `app/src/app.cpp` (`run_once`), `app/src/rec_actions.cpp`, `app/src/param_actions.cpp`, `app/include/o3os/app/rec_fx.h`; docs `docs/formats/o3-project.md:155`, QA catalogue.
- Do not touch `Engine::dispatch`, `LRNB`, `SETS`, `MidiState`, or the INST layout. Do not design anything for the uncodable rows.
- Use `Editor::set_instrument` for the letter write; call the map rebuild after load and after every `generation()` change; MIDI instruments are skipped in the scan.
- Strip texts exactly as in sections 1 and 5. Run `tools/code_map.py` first; scoped `ctest -R "enc_learn|rec_fx|inst_midi|seq"`, one full `tools/ci.sh host` per branch, `tools/ci.sh qemu` before the merge of S2 (sequencer change).
