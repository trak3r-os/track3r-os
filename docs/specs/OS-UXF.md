# OS-UXF — UI and workflow fixes from the Fable review (2026-11-11)

The maintainer chose these items, numbered as in the Fable review: 1, 2, 3, 5, 6, 7, 8, 11, 12, 17, 18 and 23 (2026-11-11). They are grouped in three backlog rows, one engineer each, with one commit per item:

- **OS-UXF-KEYS**: items 1, 5 and 17.
- **OS-UXF-PAT**: items 2, 6, 7, 8 and 12.
- **OS-UXF-SMP**: items 3, 11, 18 and 23.

Every item is NEW COMMUNITY OS BEHAVIOUR unless its row says otherwise. Decisions marked (orch.) are the orchestrator's: delegated by the maintainer, simplest and reversible. An engineer may change one when the code shows a conflict, and records the change in the item's "as built" note at the end of this file.

Rules for every item:
- Each item gets QA rows, the ui-map lines and the manual text.
- When a key that the manual draws moves, regenerate with `tools/manual_diagrams.py` and `tools/manual_tables.py`, then run the manual gate (see `app/CLAUDE.md`).
- `tests/builtin_bindings_test.cpp` is updated for every binding change.
- Flash delta per item.

## OS-UXF-KEYS

| # | Item | Decision |
|---|---|---|
| 1a | Save from any screen | `Shift`+`File` (free today) = SAVE on every screen: the same path as File `F4` SAVE. The strip says `SAVED <name>`. A project that has no name yet opens the File save-as text entry. Refused while a save or an import job runs, with the existing notice. While playing, it does what File `F4` does while playing. |
| 1b | Optional autosave (**DROPPED by the maintainer 2026-11-11 (no autosave)**; not implemented) | A Config row AUTOSAVE with the values OFF (default) and ON STOP. When the transport stops, a named project with unsaved changes is saved through the normal save job, never during playback. The strip says `AUTOSAVED`. **Level C slice, a separate commit:** if the setting needs a change of the `.o3st` layout, it needs a version bump, a loader for the old version and a round-trip test, plus a risk review before merge. If the settings file has no room for a field without a bump, stop and report the plan first. |
| 5 | One accept/back grammar | On every overlay, dialog and text entry, F1 = OK/GO/accept and F8 = BACK/cancel. Known exceptions today: text entry (`F8` OK / `F7` CANCEL), Navigator (`F8` GO). Audit every overlay and list in the "as built" note each one that changed. Parity: the official firmware's text-entry keys are not verified on a unit, so this is new Community OS behaviour. |
| 17 | Fewer column-button chords | `Vol`+`Fx` stays (opens Learn). `Instr`+`Fx` (`enc.assign`) is removed as a chord and becomes an F-key ENC inside the Learn overlay, which assigns the same parameter to the encoder. The behaviour of OS-ENCLEARN is unchanged otherwise. Pick the overlay's free F-key (orch.). This removes the chord timing hazard of ui-map §3 item 11. |

## OS-UXF-PAT

| # | Item | Decision |
|---|---|---|
| 2 | Focus Loop | On Pattern with a block selected, `Shift`+`Play` plays from the block's first row and loops at its last row. The strip says `LOOP 04-11`. Play, or `Shift`+`Play` again, stops. With no block, `Shift`+`Play` is as today (OS-PLAYCUR, from the cursor's row) (orch.: this keeps the current gesture). The loop range lives in the sequencer and gets host tests: the wrap, a block on a track shorter than the pattern, a pattern change and a stop. |
| 6 | F1 PATTERN tap | On Pattern, a tap of `F1` opens the same pattern chooser that the hold opens today (OS-PATSWITCH), so tap and hold do the same thing, as `F2`/`F3` do. If the hold has a meaning a tap would break, keep the hold and make the tap open the chooser. |
| 7 | One pads picker | The pad modes on `Shift`+`F2`..`F7` become one `Shift`+`F2` PADS picker: an overlay whose F-key row lists the modes. One press picks, F8 goes back (item 5 grammar). `Shift`+`F1` (octave, OS-PADOCT) stays. `Shift`+`F3`..`F7` become free, and blank in the Shift row. |
| 8 | `Shift`+`Rec` | `Shift`+`Rec` always toggles Edit following (LIVE) on and off, from any mode. `Rec` always toggles Browse and Edit (OS-RECSWAP). In LIVE, today `Rec` = WATCH and `Shift`+`Rec` = STEP, which inverts the stopped meaning; that inversion goes. Write the mode-transition table in the "as built" note. |
| 12 | Song F2 | (Revised by the orchestrator, 2026-11-11.) `F1` PLAY PAT and the Play button are unchanged. `F2` = DUP PAT, the same action as `Shift`+`Insert`, which stays (the old `F2` PLAY SNG was the Play button's action). Only the F2 binding, the Song key-row label, the binding test, QA, ui-map and the manual change. Does not touch the OS-SONG3 work (the hold of `F2` is still the slot loop). |

## OS-UXF-SMP

| # | Item | Decision |
|---|---|---|
| 3 | Chop in one screen | (a) From the Wave main page, `SamplePlay` again opens CHOP. If the page cycle already does this, nothing changes. (b) CHOP AUTO and EVEN set the instrument's play mode to BEAT SLICE themselves, in the same undo unit, and the strip says `BEAT SLICE`. (c) CHOP `Shift`+`F4` TO PAT writes slice 1..n on consecutive steps of the Pattern cursor's track, from the Pattern cursor's row, one slice a step, stopping at the pattern end. It uses the way a step addresses a slice today, and is one undo unit. It is refused, with a notice, when the instrument has no slices. |
| 11 | One "other instrument" grammar | `Shift`+`F7`/`F8` = INST -/+ on every sample screen (Wave, Sample edit, Sample load already has it, Record if it has an instrument). Sample edit `Shift`+`F5` NEXT INS moves there. If a Shift key is taken, move the smaller function and record the move. The Instrument screen's `F6`/`F7` stay. |
| 18 | Sample load MARK | The Sample load Shift row `F6` = MARK, which toggles the mark on the file under the cursor, the same as `Shift`+`Right`. Labelled, so multi-mark can be found. |
| 23 | Sample edit FX page to `.ptfx` | FILTER and COMPRESS, and the lo-fi Shift keys AMG 8K, AMG 16K and Q8 if they live on that page, become `.ptfx` plugins (our own code on the editor-fx-sdk API that the OS-CEPTFX host loads) and ship in the SD content zip (`Effects/`, OS-PTFXPACK). The FX page and its code leave the core. Measure the flash saved. Keep what OS-AUDIT-A9 fixed (the warm-up over the loop tail), or state the difference. If the `.ptfx` host lacks something the page had (parameters, undo, a range), stop and report before removing the page. |

## As built

(Each engineer adds one note per item here: what changed against the table, the files, the tests, and the flash delta.)

### Item 6, F1 PATTERN tap (OS-UXF-PAT)

As the table says, no deviation. `chooser.toggle` (`app/src/chooser_actions.cpp`) no longer refuses `F1`: the tap opens the PATTERN chooser, a second tap of `F1` or `F8` closes it, and a tap of `F2`/`F3` switches to LENGTH/JUMP. The hold is unchanged and keeps its meaning (it closes on the release). A tap-opened PATTERN chooser sets `edit.chooser_held`, which now reads "the live PATTERN chooser": the pads, the encoder, the queue of PAT SWITCH, the pad lights and `Copy`/`Shift`+`Copy` work the same under a tap and a hold; only a `hold_end` closes by release, so the release of a tap does nothing. The chooser's hint line lost the word "release" (`F1: close`). Binding table unchanged (the `Pattern | tap F1` row already existed). Tests: `pattern_switch_app_test`, `pattern_edit_app_test`, `input_wrap_test` (the old "a tap opens nothing" cases now say the opposite). QA-PAT-260 reworded, QA-PAT-527 added. Flash delta: see the item 2 note (one mk66 run at the end).

### Item 8, Shift+Rec toggles LIVE from any mode (OS-UXF-PAT)

New Community OS behaviour. `Rec` (`pattern.edit_toggle`) always toggles Browse and Edit: WATCH -> STEP, STEP -> WATCH, LIVE -> WATCH. `Shift`+`Rec` (`pattern.rec_free`) toggles LIVE on and off. The old inversion in LIVE (`Shift`+`Rec` = STEP, `Rec` = WATCH, OS-LIVEEXIT) goes: LIVE off goes back to the mode LIVE was entered from. That is the spec's reading of "toggles on and off" (an assumption: the spec does not name the target). `PatternEdit::live_from_step` (`app/include/o3os/app/pattern_edit.h`) remembers it, set when LIVE is entered (true from STEP, false from WATCH), cleared when Edit is left (`pattern_set_mode`). Code: `pattern_rec_free` in `app/src/pattern_actions.cpp`. The other exit of LIVE, `Rec`, stays WATCH. Stopped, `Shift`+`Rec` is unchanged (play + count-in + LIVE); the entry mode is the one it was pressed in.

Mode transitions (after item 8; a cell that changed against OS-RECMODE2 §2 is marked `*`):

| Now | Rec | Shift+Rec | Play |
|---|---|---|---|
| **WATCH**, playing | STEP, the cursor stops on the play line's row | LIVE (entered from WATCH) | stop, WATCH |
| **STEP**, playing | WATCH, following again | LIVE (entered from STEP) | stop, stays STEP |
| **LIVE** from WATCH | WATCH; the take ends, the view keeps following | * WATCH (was STEP) | stop; the take ends, WATCH |
| **LIVE** from STEP | WATCH; the take ends, the view keeps following | STEP; the take ends, the cursor stays on the current row | stop; the take ends, WATCH |
| **WATCH**, stopped | STEP at the cursor | play + count-in + LIVE (from WATCH) | play, WATCH |
| **STEP**, stopped | WATCH | play + count-in + LIVE (from STEP) | play, stays STEP |

The only behavioural change of the table is the starred cell. The count-in cancel (`Rec` or `Shift`+`Rec` during the count-in) also goes back to the mode `Shift`+`Rec` began in (`cancel_live_count_in`): LIVE begun from STEP stops and returns to STEP stopped, from WATCH to WATCH stopped (before it was always WATCH; `tests/recmode_test.cpp`, both modes and both keys).

Tests: `tests/recmode_test.cpp` (the table, both entry modes, the count-in), `tests/rec_test.cpp`, `tests/play_position_test.cpp`, `tests/pattern_switch_app_test.cpp`, `tests/screens_test.cpp` (cases that entered LIVE from WATCH and expected STEP now enter it from STEP, or use `Rec`); `tests/builtin_bindings_test.cpp` needs no change (no binding moved). Scenarios: `sim/scenarios/qa/qa_pattern.txt` (QA-PAT-003..005 enter LIVE from STEP, QA-PAT-528 new), `sim/scenarios/pattern_record.txt`. QA-PAT-528 added, QA-PAT-005 reworded; `docs/ui/ui-map.md` (2 rows), `docs/manual/pattern.md`. Goldens: only `qa_pat_528` is new, no other picture moved. MK66 run at the end of items 6, 8, 12 and 7 (2026-11-11, base without a recorded earlier size): FLASH 1004 KB (98.05 %), SRAM_U 138,832 B (70.61 %); the link overflowed SDRAM_STATIC by 3,904 B, which is dev's S7c clip buffer (fix/OS-MIDIREC-P1-CLIPSDRAM), not this change. Item 2 (Focus Loop) is not done. Unit check: Shift+Rec twice from WATCH and from STEP while playing.

### Item 12, Song F2 DUP PAT (OS-UXF-PAT)

New Community OS behaviour. As the revised table row says. `F2` on Song is `song.add_slot` with argument 1 (DUP PAT: a slot after the cursor holding a copy of the pattern at the first free number, one undo unit), the action of `Shift`+`Insert`. The key row says `DUP PAT` on F2, on the LIST and in a TIMELINE lane (`kSongSoftKeys`, `kSongLaneSoftKeys`). F1 PLAY PAT, the Play button and the hold of F2 (OS-SONG3 S7 loop) are unchanged; the tap of F2 still fires at the release because of that hold. Code: `app/src/bindings.cpp` (one line plus its header comment), `app/include/o3os/app/screens/song_screen.h`. Help page SONG: the F2 line (`app/help/pages.txt`, `tests/data/help_old_pages.txt`; a PLAY line would overflow the page buffer). Tests: `builtin_bindings_test` (the row), `song_edit_test` (new case: F2 = DUP PAT, one undo unit); the tests and HIL steps that tapped F2 to play the song on Song now press Play (`song_edit_test`, `song_preview_wire_test`, `pattern_edit_app_test`, `_play_song.json`, `gen_hil.py` and the regenerated `qa_perform.json`). Scenario `qa/qa_song_s3.txt` (QA-SNG-212). QA-SNG-212 added, QA-SNG-038 reworded; `docs/ui/ui-map.md` Song row; manual `song.md`, `advanced.md` (tables regenerated). MK66 run at the end of items 6, 8, 12 and 7 (2026-11-11, base without a recorded earlier size): FLASH 1004 KB (98.05 %), SRAM_U 138,832 B (70.61 %); the link overflowed SDRAM_STATIC by 3,904 B, which is dev's S7c clip buffer (fix/OS-MIDIREC-P1-CLIPSDRAM), not this change. Item 2 (Focus Loop) is not done.

### Item 7, PADS picker (OS-UXF-PAT)

New Community OS behaviour. `Shift`+`F2` on Pattern opens the PADS picker (`pads.picker`, `OverlayId::PadsPicker`, `Context::PadsPicker`, `app/src/screens/pads_picker_overlay.cpp`), a six-row dialog with the mode now in use marked. Its keys: `F1` NOTES, `F2` PIANO, `F3` ISO, `F4` TRACKS, `F5` PATS, `F6` VU (`pads.pick` with the PadMode number as argument; the pick closes the picker and calls `PadLayer::pick`, so a second VU pick still toggles faders), `F8` BACK. `Shift`+`F3`..`F7` are blank ("-") and resolve to `ui.none`; `Shift`+`F1` (octave) and `Shift`+`F8` (REDO) stay. `kPatternShiftKeys` is `OCTAVE PADS - - - - - REDO`. The Pattern key row no longer dims the mode key under Shift (the `kPatternPickerDim` block is gone). The old `pads.mode` action stays registered (scenarios and tests invoke it by name; it is no longer bound). `kPadModeKey` in `pad_layer.h` is now unused: follow-up to remove it. The picker is refused while another overlay owns input (`kFocus`).

Tests: `tests/pad_modes_test.cpp` (the picker opens over Pattern and changes nothing, `F8` keeps the mode, `F7` is no key, a pick closes it; `Rig::shift_tap` goes through the picker), `tests/builtin_bindings_test.cpp`, `tests/qanet/action_states.cpp`. Scenario `qa/qa_pattern.txt` (QA-PAT-529). HIL: `gen_hil.py` `pad_pick` (the pad-mode scenarios and the regenerated JSON). QA-PAT-529 added, QA-PAT-250 reworded; `docs/ui/ui-map.md` Pattern row; manual `pattern.md`, `config.md`, `cheat_sheet.md` (tables regenerated). MK66 run at the end of items 6, 8, 12 and 7 (2026-11-11, base without a recorded earlier size): FLASH 1004 KB (98.05 %), SRAM_U 138,832 B (70.61 %); the link overflowed SDRAM_STATIC by 3,904 B, which is dev's S7c clip buffer (fix/OS-MIDIREC-P1-CLIPSDRAM), not this change. Item 2 (Focus Loop) is not done.

### OS-UXF-KEYS item 1a: `Shift`+`File` = SAVE

- Binding: Global `Shift`+`File` -> `file.save_any` (`app/src/bindings.cpp`, handler `file_save_any` in `app/src/save_actions.cpp`). It calls the same `save_project` path as File `F4`. A project with no folder yet (`SaveService::has_folder` false: new or imported, nothing saved) opens the File save-as text entry instead, as `F5` does; F4 itself is unchanged and still auto-names "Track NN". The busy-job and load-running refusals, the card-write refusals and the playing behaviour are those of `F4`.
- Deviation from the table: the strip keeps the job's own text (`SAVE <NAME> DONE`, as F4 shows), not a new `SAVED <name>`; a second text for the same path would diverge from F4. `SAVED <name>` is the OG export text.
- Modal overlays and app pages capture keys, so the chord does not reach them (as every Global binding).
- Tests: `tests/save_app_test.cpp` (Shift+File from Pattern saves, no folder opens the keyboard, busy refusal), `tests/builtin_bindings_test.cpp`. QA-GLB-243. No goldens move.

### OS-UXF-KEYS item 5: F1 = OK/GO, F8 = BACK/cancel on every overlay and text entry

New Community OS behaviour (the official firmware's text-entry keys are unverified). Overlays that own the F-key row and have an accept action now read F1 = accept, F8 = back/cancel:

| Overlay | Before | Now |
|---|---|---|
| Text entry (save as, rename, instrument/preset name) | F8 accept, F7 cancel, F1 TYPE, F2 CAPS | F1 = SAVE/OK (`text.accept`), F8 = CANCEL (`back`), F7 and Insert = TYPE the cursor character (`text.key`), F2 CAPS and F6 AUTO unchanged |
| Navigator | F8 GO, F7 BACK | F1 GO (`navigator.accept`), F8 BACK; F7 is nothing; Shift+F1 GO, Shift+F8 BACK, Shift+F2..F7 read "-" |
| Apps list (module picker) | F1 and F8 both LAUNCH | F1 LAUNCH, F8 BACK; the `module.picker_f8` action is removed |

- Not changed (no accept action, or F1 already means something else): Learn (F1 UNBIND, F8 BACK), LEARN MAP, Events, Help (F8 CLOSE), the Song ask question (F1 ALL / F2 UNIQUE) and the File export page.
- Key rows: `app/src/key_prompt.cpp` (`kShiftF1` added; Text, Navigator and Apps tables), comments in `overlays.cpp`, `overlay.h`, `text_actions.cpp`. Bindings: `app/src/bindings.cpp` (`kTextEntryCtx`, `kNavigatorCtx`, `kModulePickerCtx`).
- Tests: `key_prompt_test`, `overlay_test`, `builtin_bindings_test`, `actions_test`, `screens_test` and the tests that pressed the old keys; sim scenarios and HIL (`qa_keyrowdlg.json`, `nav_all_screens.json`, `gen_hil.py`) moved to the new keys. QA-GLB-244 (new), QA-GLB-118/119, QA-FIL-060/113, QA-MIX-010/011, QA-SNG-010, QA-REC-044, QA-MOD-002/252/253, QA-INS-050 reworded.
- Item 1b (AUTOSAVE) is dropped by the maintainer (2026-11-11); nothing was built.

### OS-UXF-KEYS item 17: `Instr`+`Fx` chord removed, F7 = ENC in the Learn overlay

New Community OS behaviour. `Vol`+`Fx` stays (opens Learn). The Global chord `Instr`+`Fx` (`enc.assign`) is no longer bound; the same action is bound to F7 in the Learn context. The overlay's free F-key was F7 (F6 is MAP, F8 BACK), so the Learn key row is now UNBIND MODE DELETE CLR ALL FOLLOW MAP ENC BACK. F7 is always labelled `ENC` (with no target it says `NO TARGET FOR ENCODER`: a bound key never acts under a blank label, which qanet_keys checks); it assigns the Learn row to the encoder and, as before, keeps the target on the same row/instrument. `enc_assign` also clears `learn.confirm` (a pending CLR ALL? question) so the closing overlay leaves no state. Everything else of OS-ENCLEARN is unchanged (`Instr` + `Vol` held + encoder turn). The no-target strip text is now `NO ENCODER TARGET (SET IT IN LEARN)`. This removes the chord timing hazard of ui-map section 3 item 11 for `Instr`+`Fx`.
- Code: `app/src/bindings.cpp` (chord removed, `bind(kLearnCtx, F7, kEncAssign)`), `param_actions.cpp` (`enc_assign`, `enc_turn` text), `key_prompt.cpp` (`kLearnOptions`, `owner_state`). Tests: `enc_learn_test` (the rig opens Learn and taps F7), `builtin_bindings_test` (chord gone, F7 bound in Learn only), `key_prompt_test`. Docs: ui-map, manual (panel, instrument, cheat sheet, glossary, llms), OS-ENCLEARN, QA-GLB-118/228/229. Unit check: open Learn on a CUTOFF row, press F7, then `Instr`+`Vol` and turn.

### OS-UXF-FLOOP item 2: Focus Loop (`Shift`+`Play` with a block loops the block)

New Community OS behaviour. On Pattern, `Shift`+`Play` with a block selected plays from the block's first row and loops at its last (the block stays selected); `Play` or `Shift`+`Play` again stops. With no block it is OS-PLAYCUR unchanged (play from the cursor row, no loop). Playing already: the transport jumps to the block's first row and sets the loop (the playhead in another pattern than the edited one is refused, as OS-PLAYCUR does).
- Sequencer: `Command::Kind::SetRowLoop`, `Sequencer::set_row_loop(first,last)` and `looping_rows()` (`seq/src/sequencer.cpp`, `row_loop_wrap()` runs in `end_step` after the pending switch and before the pattern-end test). It is the OS-SONG3 S7 slot-loop command path: SPSC command ring drained at block start, a wrap is a locate to the loop's first row, so MTC and clock behave as at an S7 wrap. The range clears on stop, on `play_pattern`/`play_song`, on a pattern switch, and when a queued pattern, a queued jump or a mode request is pending at the wrap (the loop ends there instead of wrapping). A block on a track shorter than the pattern wraps by the pattern's rows, not the track's (the song row is the clock of every track). A Break or jump inside the range wraps too. `PlayPosition::publish_row_loop` (relaxed atomics, `row_first/row_last/row_looping()`) feeds the UI.
- App: `TransportSink::loop_rows` (default false; `sim_audio.cpp` and `platform/mk66/src/app_main.cpp` implement it), `transport_play_cursor` in `app/src/pattern_actions.cpp`.
- Deviations from the table: the strip text is on the Pattern info line (`LOOP 05-12  PLAY: STOP`, rows as the screen shows them), not the message strip, because the strip is a transient notice; `Shift`+`Play` while a loop runs stops it even with no block.
- Tests: `tests/seq_metro_test.cpp` (6 "focus loop seq" cases: wrap, short track, pattern change, stop, queued pattern, Break and bad range), `tests/play_cursor_test.cpp` (4 gesture cases), sim scenario `qa/qa_pattern_focusloop.txt` (3 goldens). QA-PAT-233 (reworded), QA-PAT-540 to 543. Manual: `docs/manual/pattern.md`, "Loop a block (Focus Loop)".
- Size (mk66 unit image, vs dev f3b24d0a, code-identical to 6c22970b): .text +812 B (996,116 to 996,928), SRAM_U +8 B (138,832 to 138,840), SDRAM_STATIC unchanged; image ends 0xFB000 (limit 0xFF000).
- Unit check left: loop a block on the unit by ear (wrap is gapless, MIDI clock out and a slaved MTC receiver keep time at the wrap).

### OS-UXF-SMP item 18: Sample load Shift row F6 = MARK

New Community OS behaviour. `bind(kSampleLoadCtx, Shift+F6, sample.mark 0)` (`app/src/bindings.cpp`), the same action and argument as `Shift`+`Right`; label `MARK` in `kSampleLoadShiftKeys` (`sample_load_screen.h`). No new code, no flash beyond the table row. Tests: `builtin_bindings_test`, `sample_load_test` (Shift+F6 marks the file and steps down, as Shift+Right). QA-SLD-036. Goldens: `qa_sample_load` (qa_sld_001, qa_sld_021), `screens` (shift_sample_load). qanet: one baseline line for the key (`shift+sample.mark`, the strip hidden behind the playing transport like the other R2 lines).

### OS-UXF-SMP item 11: Shift+F7 / F8 = INST - / INST + on every sample screen

New Community OS behaviour. Wave: Shift+F7 / F8 = `sedit.instrument` -1 / +1 (the Sample load grammar). Sample edit: the same two keys; Record has no instrument, so it is skipped. Displaced keys, all recorded here:
- Wave Shift+F8 REDO moved to Shift+F5 (free).
- Sample edit Shift+F5 NEXT INS went to Shift+F8 INST + (and INST - is new on F7); Shift+F8 REDO moved to Shift+F5; Shift+F7 PRV EDGE moved to Shift+Play, unlabelled because the row is full. Hardware check: Shift+Play must not collide with a transport chord on the unit.
Labels: `kWaveShiftKeys`, `kSampleEditShiftKeys`; hint of Sample edit "pad / Shift+F7/F8: change". Tests: `builtin_bindings_test`, `wave_screen_test`, `sample_edit_screen_test`, `ptfx_app_test`. QA-WAV-001 and 028, QA-SED-001, 015, 017. HIL: `qa_sample_edit.json`, `gen_hil.py`.

### OS-UXF-SMP item 3: Chop in one screen

(a) and (b) were already true and are pinned by existing tests (`wave_screen_test`: the button again opens CHOP; AUTO and EVEN set BEAT SLICE in the same undo unit and the strip says `BEAT SLICE`); nothing changed. (c) is new: CHOP Shift+F4 TO PAT (`wave.to_pat`, `wave_actions.cpp`, undoable, enabled as `wave_enabled`) writes slice k (k = 0..n-1) as note `audio::kSliceRootNote + k` with the instrument (slot + 1) on step `cursor row + k` of the Pattern cursor's track, which is how a BEAT SLICE instrument addresses a slice. One `set_steps` call, so one undo unit; the Pattern cursor stays. Stops at the track end (`SLICES TO PAT: PATTERN END`). Refusals: `NO SLICES TO WRITE`, `PAST TRACK END`, `THE PATTERN REFUSED THE STEPS`, and one beyond the table: `SET BEAT SLICE MODE FIRST` when the instrument's play mode is not BEAT SLICE (the note-to-slice mapping only holds in that mode; SLICE mode has its own). The CHOP page has its own Shift row (`kChopShiftKeys`, `WaveScreen::shift_row`). Tests: `wave_screen_test` (two cases), `builtin_bindings_test`, `actions_test` (undoable list). QA-WAV-029 (and QA-WAV-002 text). Goldens: `qa_wave` (qa_wav_002, qa_wav_029).
