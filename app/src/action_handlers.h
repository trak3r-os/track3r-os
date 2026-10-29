// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The handlers of the built-in actions, by feature file. Private to app/: the registry (actions.cpp) is the only
// user. Each returns true if something visible changed.

#include <o3os/app/actions.h>
#include <o3os/app/save_service.h>  // (save_report names SaveService::Start)

namespace o3os::app {

// nav_actions.cpp: moving the cursor of whatever owns the input: the pattern grid, the song list, or the Navigator's
// grid while it is up (OS-002b). Lists have one axis: left / right and the track actions do nothing on the song list.
bool nav_left(ActionContext& ctx, core::ActionArg arg) noexcept;
bool nav_right(ActionContext& ctx, core::ActionArg arg) noexcept;
bool nav_up(ActionContext& ctx, core::ActionArg arg) noexcept;
bool nav_down(ActionContext& ctx, core::ActionArg arg) noexcept;
bool nav_scroll(ActionContext& ctx, core::ActionArg arg) noexcept;  // arg = detents, + down
bool cursor_jump_fraction(ActionContext& ctx, core::ActionArg arg) noexcept;  // arg = n: to n/4 of the list, 0..3
bool cursor_jump_end(ActionContext& ctx, core::ActionArg arg) noexcept;       // to the last item
bool cursor_page(ActionContext& ctx, core::ActionArg arg) noexcept;           // arg = pages, + down
bool cursor_track(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = tracks, + right (pattern only)
bool cursor_home(ActionContext& ctx, core::ActionArg arg) noexcept;           // pattern: first track; lists: first item
bool cursor_end(ActionContext& ctx, core::ActionArg arg) noexcept;            // pattern: last track; lists: last item

// screen_actions.cpp: screens, pages, modes and the Navigator.
bool nav_screen(ActionContext& ctx, core::ActionArg arg) noexcept;       // arg = ScreenId: switch, or cycle the pages
bool nav_peek(ActionContext& ctx, core::ActionArg arg) noexcept;         // OS-SONGPEEK, the Hold of a screen button: arg = ScreenId, shown until the release
bool nav_peek_end(ActionContext& ctx, core::ActionArg arg) noexcept;     // OS-SONGPEEK, the HoldEnd: back to the screen the peek came from, if the peek's screen is still in view
bool view_mode(ActionContext& ctx, core::ActionArg arg) noexcept;        // arg = mode index of the screen in view
bool view_mode_next(ActionContext& ctx, core::ActionArg arg) noexcept;   // the next mode, wrapping
bool navigator_open(ActionContext& ctx, core::ActionArg arg) noexcept;   // Shift + Pattern (OS-NAV2: it stays open)
bool navigator_accept(ActionContext& ctx, core::ActionArg arg) noexcept; // GO (F8, F1): open the chosen screen, close the Navigator
bool ui_back(ActionContext& ctx, core::ActionArg arg) noexcept;          // close the overlay in view
bool input_test_reset(ActionContext& ctx, core::ActionArg arg) noexcept; // the INPUT TEST screen's counts back to zero (OS-007d)

// pattern_actions.cpp: what the pattern screen's cursor, mode, selection and instrument do.
bool pattern_column(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = Column (0 Note .. 4 FX2)
bool pattern_zoom(ActionContext& ctx, core::ActionArg arg) noexcept;         // Note + Instr: the Pattern grid's zoom, 5 -> 8 -> 12 -> 5 tracks (OS-ZOOM)
bool pattern_fx_column(ActionContext& ctx, core::ActionArg arg) noexcept;   // FX1, FX2, FX1, ...
bool instrument_select(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = instrument 1..48
// instrument_actions.cpp (OS-INSTUI S1, S2): the Instrument screen's page keys, neighbour keys and pads.
bool instrument_page(ActionContext& ctx, core::ActionArg arg) noexcept;     // F1..F5, arg = key 0..4: the page the key means for the instrument in view
bool instrument_turn(ActionContext& ctx, core::ActionArg arg) noexcept;     // Left / Right of the empty slot's type chooser only (a used slot's are param.edit), arg = -1 / +1: the previous / next page (a fresh press wraps, an auto-repeat stops at the end)
bool instrument_step(ActionContext& ctx, core::ActionArg arg) noexcept;     // F6 / F7, arg = -1 / +1: one slot, no wrap
bool instrument_used(ActionContext& ctx, core::ActionArg arg) noexcept;     // Shift + F6 / F7, arg = -1 / +1: the nearest instrument in use
bool instrument_name(ActionContext& ctx, core::ActionArg arg) noexcept;     // Shift + F1 NAME (OS-INSTUI S7): opens the on-screen keyboard on the instrument's name
bool instrument_rename_accept(ActionContext& ctx) noexcept;                 // the keyboard's F8 for that target (called by text.accept): the typed name is an undo unit
bool instrument_copy(ActionContext& ctx, core::ActionArg arg) noexcept;     // Shift + F2 COPY: the clipboard takes the instrument (and its audio, by reference)
bool instrument_paste(ActionContext& ctx, core::ActionArg arg) noexcept;    // Shift + F3 PASTE, the chooser's F3: into an empty slot or over a used one, one undo unit
bool instrument_save(ActionContext& ctx, core::ActionArg arg) noexcept;     // Shift + F5 SAVE: a sample instrument as instruments/<name>.pti (save_actions.cpp)
bool instrument_load(ActionContext& ctx, core::ActionArg arg) noexcept;     // the chooser's F5 LOAD: Sample load listing instruments/*.pti
bool instrument_delete(ActionContext& ctx, core::ActionArg arg) noexcept;   // Shift + F4 (OS-INSTUI S5): the instrument leaves its slot, one undo unit
bool instrument_init(ActionContext& ctx, core::ActionArg arg) noexcept;     // Shift + Delete: every parameter back to its default, kind / sample / module / name kept
bool instrument_new_midi(ActionContext& ctx, core::ActionArg arg) noexcept; // the chooser's F2: a MIDI instrument on the empty slot
bool instrument_sample(ActionContext& ctx, core::ActionArg arg) noexcept;   // the chooser's F1: Sample load with this slot as the target, back to Params after the load
bool instrument_pad(ActionContext& ctx, core::ActionArg arg) noexcept;      // a pad, arg = 0..47: the instrument in view sounds (held until the pad's release)
void release_live_pads(ActionContext& ctx) noexcept;                        // O2: the notes the pads of the Instrument screen hold end (on the instrument that started them)
bool instrument_engine(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = model::kInstEngine* 0..4 (0 automatic), or -1: the next one (OS-004d)
// The playback engine of instrument `slot` set to `target` (model::kInstEngine*), one undo unit in the caller's, the slot rebound; the instrument action and the PLAY ENGINE row of the
// Params page (param_actions.cpp write_param) both come here (OS-FU5). An Amiga the sample's rate cannot play (project_state.cpp) says CONVERT TO AMIGA FIRST: refused when `dir` is 0
// (an explicit choice), skipped over in the direction of `dir` (+1 up, -1 down) for a step of the encoder. True when the engine changed.
bool play_engine_set(ActionContext& ctx, unsigned slot, unsigned target, int dir) noexcept;
// The play mode of the sample instrument in slot `slot` (0-based) set to `target` (play_mode.h) with the validation of project_state.cpp: the source page's PLAY MODE row and instrument.play_mode.
// True when the mode changed.
bool play_mode_set(ActionContext& ctx, unsigned slot, unsigned target) noexcept;
bool instrument_play_mode(ActionContext& ctx, core::ActionArg arg) noexcept;  // arg 0..7: that mode; -1 / -2: the previous / next one
bool pattern_edit_toggle(ActionContext& ctx, core::ActionArg arg) noexcept; // Rec: Browse <-> Edit, free (OS-RECSWAP)
bool pattern_rec_free(ActionContext& ctx, core::ActionArg arg) noexcept;    // Shift + Rec: Edit following / free (OS-RECSWAP)
bool pattern_preview(ActionContext& ctx, core::ActionArg arg) noexcept;     // F5: the cursor step sounds on its track while the key is down (OS-CFGWIRE)
bool pattern_export(ActionContext& ctx, core::ActionArg arg) noexcept;      // EXPORT, which is Pattern F7 TOOLS now (OS-WAVEXP): says so; no key (OS-CFGWIRE)
bool pattern_screen_in_view(const ActionContext& ctx) noexcept;             // the Pattern screen is in view (the `enabled` of pattern.fill: outside Edit it says why it did nothing)
bool pads_track(ActionContext& ctx, core::ActionArg arg) noexcept;          // TRACKS row 1, arg = track 0..11: the Pattern cursor goes there; on Perform it joins / leaves the layer
bool pads_play(ActionContext& ctx, core::ActionArg arg) noexcept;           // a pad (0..47) of a fixed PIANO / ISO / STEP / PATS / VU mode, by the mode (OS-PADS-B)
bool pads_step_up(ActionContext& ctx, core::ActionArg arg) noexcept;        // the release of a STEP pad (arg = pad): a lit step nothing changed is cleared
bool pads_step_delta(ActionContext& ctx, core::ActionArg arg) noexcept;     // the encoder with a STEP pad held: that step's note
bool pads_mode(ActionContext& ctx, core::ActionArg arg) noexcept;           // Shift + F2..F7, arg = the mode 0..6: the pads' mode that stays (OS-PADS-A, OS-PADOCT)
bool pads_octave(ActionContext& ctx, core::ActionArg arg) noexcept;         // Shift + F1 (OS-PADOCT): the pads' base C goes up an octave, 9 wraps to 0
bool pad_preview(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = pad 0..47: Browse (the engine hook is OS-005)
bool pad_audition(ActionContext& ctx, core::ActionArg arg) noexcept;        // arg = pad 0..47: Sample load (also the target instrument), Mixer, Config, File: the current instrument sounds (OS-AUDIT-40, OS-AUDIT-26)
bool transport_play(ActionContext& ctx, core::ActionArg arg) noexcept;      // Play: stopped, play the pattern being edited; playing, stop (OS-005d)
bool transport_play_cursor(ActionContext& ctx, core::ActionArg arg) noexcept;  // Shift + Play on Pattern (OS-PLAYCUR): from the cursor's row, or the line jumps to it
bool transport_available(const ActionContext& ctx) noexcept;                // a transport is attached (the device); none in the simulator
bool select_extend(ActionContext& ctx, core::ActionArg arg) noexcept;       // arg = 10 * dx + dy: the block grows (Shift + arrows)
bool select_column_all(ActionContext& ctx, core::ActionArg arg) noexcept;   // the cursor's column, again every track
bool select_clear(ActionContext& ctx, core::ActionArg arg) noexcept;        // back to the cursor step

// step_actions.cpp: the edit core of the pattern (OS-009 P1a.1). Each is one undo unit, enabled in Edit and Select, and works on the
// block (or the cursor step) with the single-step and multi-step rules (model/step_edit.h).
bool step_note(ActionContext& ctx, core::ActionArg arg) noexcept;               // arg = a model note byte
bool step_note_pad(ActionContext& ctx, core::ActionArg arg) noexcept;           // arg = pad 0..47 through the pad board
bool step_midi_note(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = a MIDI note 0..127 from the keyboard (the cursor step, no advance; Rec: at the playhead)
bool pattern_invert(ActionContext& ctx, core::ActionArg arg) noexcept;          // F6: the block's steps in the reverse order, each track on its own (OS-CFGWIRE)
// fill_actions.cpp: the Fill dialog (OS-PTOOLS part 1; docs/specs/OS-PTOOLS.md). The fill is one undo unit.
bool fill_open(ActionContext& ctx, core::ActionArg arg) noexcept;               // F4: the dialog (Edit only; outside Edit it says so)
bool fill_move(ActionContext& ctx, core::ActionArg arg) noexcept;               // arg = -1 / +1: the cursor row (Up / Down)
bool fill_delta(ActionContext& ctx, core::ActionArg arg) noexcept;              // arg = detents: the row's value (Left / Right, the encoder)
bool fill_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;             // arg = detents, each the row's coarse step (Shift + encoder; OS-SHIFTENC)
bool fill_apply(ActionContext& ctx, core::ActionArg arg) noexcept;              // F1: fills with the dialog's rows and closes it
bool pattern_fill(ActionContext& ctx, core::ActionArg arg) noexcept;            // no key since OS-QJUMP (was Shift + Fx): repeats the last fill; before one, the OS-CFGWIRE fill (the current note on every Nth step, N = the edit step) of the block or the track
// song_actions.cpp: the Song screen's editing (OS-SONG1). The edits are undo units; the range, the tempo field and the clipboard are view state (song_view.h).
bool song_enabled(const ActionContext& ctx) noexcept;                       // the Song screen is in view
bool song_play_enabled(const ActionContext& ctx) noexcept;                  // ... and a transport is attached
bool song_add_slot(ActionContext& ctx, core::ActionArg arg) noexcept;       // F3 / Insert: a slot after the cursor's with its pattern; arg 1 (Shift + F3): with a copy of it as a new pattern
bool song_delete_slot(ActionContext& ctx, core::ActionArg arg) noexcept;    // F4 / Delete: the cursor's slot or the selected range
bool song_pattern_delta(ActionContext& ctx, core::ActionArg arg) noexcept;  // F5 / F6: arg = +-1, the cursor slot's pattern number
bool song_turn(ActionContext& ctx, core::ActionArg arg) noexcept;           // the encoder: arg = detents; the slot's pattern, or the tempo (F7)
bool song_turn_fine(ActionContext& ctx, core::ActionArg arg) noexcept;      // Shift + encoder: a tenth of a BPM on the tempo
bool song_tempo(ActionContext& ctx, core::ActionArg arg) noexcept;          // F7: the encoder on the tempo field, and a tap of the tap tempo
bool song_select(ActionContext& ctx, core::ActionArg arg) noexcept;         // Shift + Up / Down: arg = +-1, grows the range of slots
bool song_copy(ActionContext& ctx, core::ActionArg arg) noexcept;           // Copy: the range (or the slot) into the clipboard
bool song_name(ActionContext& ctx, core::ActionArg arg) noexcept;           // Shift + F6 NAME (OS-NAMES): the keyboard on the name of the pattern in the slot under the cursor
bool song_paste(ActionContext& ctx, core::ActionArg arg) noexcept;          // Shift + Copy: the clipboard's slots inserted at the cursor
bool song_play_pattern(ActionContext& ctx, core::ActionArg arg) noexcept;   // F1: the cursor slot's pattern plays, or the transport stops
bool song_queue_jump(ActionContext& ctx, core::ActionArg arg) noexcept;     // Rec, while playing: the cursor's slot plays next, at the end of the pattern (again: cancels)
bool song_clear(ActionContext& ctx, core::ActionArg arg) noexcept;         // Delete: the cells of the map's rectangle (in the map), else the slot or range (as F4)
bool song_move(ActionContext& ctx, core::ActionArg arg) noexcept;          // Copy held + Up / Down (OS-SONGSHARE S3): arg = -1 up, +1 down, the range of slots moves one position
bool song_next_use(ActionContext& ctx, core::ActionArg arg) noexcept;      // Note / Shift + Note (OS-SONGSHARE S4): arg = +1 next, -1 previous slot that holds the cursor's pattern, wrapping
bool song_cut(ActionContext& ctx, core::ActionArg arg) noexcept;           // Shift + Delete (OS-SONGSHARE S2): CUT, the slots into the slot clipboard and out of the list (in the map: the cells, as song_cells_cut)
bool song_pads(ActionContext& ctx, core::ActionArg arg) noexcept;           // Shift + F7 PADS (OS-SONGPADS): the pads switch between TRACKS and SLOTS
bool song_pad(ActionContext& ctx, core::ActionArg arg) noexcept;            // a pad in SLOTS mode, arg = the pad 0..47: the slot's jump (playing) or the cursor goes there (stopped)
// song_map_actions.cpp: the Song map (OS-SONGMAP; docs/specs/SONG-UX.md section 1). The cells' keys are song_actions.cpp's Copy, Shift + Copy and Delete, which call the three below in the map.
bool song_cells_copy(ActionContext& ctx) noexcept;                          // Copy in the map: the rectangle's cells into the song clip
bool song_cells_paste(ActionContext& ctx) noexcept;                         // Shift + Copy in the map: the clip at the cursor's cell, or tiled over the selection
bool song_cells_clear(ActionContext& ctx) noexcept;                         // Delete in the map: every element of the rectangle's tracks
bool song_cells_cut(ActionContext& ctx) noexcept;                           // Shift + Delete in the map (OS-SONGSHARE S2): the cells into the song clip, then cleared as Delete does
bool song_select_tracks(ActionContext& ctx, core::ActionArg arg) noexcept;  // Shift + Left / Right: arg = +-1, grows the range of tracks (in the map)
bool song_unique(ActionContext& ctx, core::ActionArg arg) noexcept;         // F5 UNIQUE: the slot's pattern (or the range's shared ones) copied, the slots pointed at the copies
bool help_open(ActionContext& ctx, core::ActionArg arg) noexcept;             // OS-HELP: Shift + Config, the help page of the screen in view
bool song_tools_open(ActionContext& ctx, core::ActionArg arg) noexcept;     // F6 TOOLS: the dialog over the map's rectangle
bool song_answer(ActionContext& ctx, core::ActionArg arg) noexcept;         // F1 / F2 while the question is up: arg 0 = ALL, 1 = UNIQUE
bool songtools_move(ActionContext& ctx, core::ActionArg arg) noexcept;      // the dialog: Up / Down choose the row
bool songtools_delta(ActionContext& ctx, core::ActionArg arg) noexcept;     // ... Left / Right / the encoder change its value
bool songtools_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;    // ... Shift + encoder: the row's coarse step
bool songtools_apply(ActionContext& ctx, core::ActionArg arg) noexcept;     // ... F1: close and make the edit (one undo unit)
void song_question_drop(ViewState& view) noexcept;                          // takes the question and its strip text back
bool song_question_sync(ViewState& view, const Model& model) noexcept;      // once a pass: a question that no longer holds goes
bool song_question_taken_back(const ViewState& view, const core::InputEvent& event) noexcept;  // does this input take the question back?

// text_actions.cpp / save_actions.cpp: the on-screen keyboard (OS-FU2). Overlay actions: no focus flag.
bool text_pad(ActionContext& ctx, core::ActionArg arg) noexcept;        // arg = pad 0..47: the key under it is typed
bool text_move(ActionContext& ctx, core::ActionArg arg) noexcept;       // arg = 0 left, 1 right, 2 up, 3 down
bool text_scroll(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = detents: along the row (or the name)
bool text_key(ActionContext& ctx, core::ActionArg arg) noexcept;        // Insert / F1: the key in focus
bool text_caps(ActionContext& ctx, core::ActionArg arg) noexcept;
bool text_backspace(ActionContext& ctx, core::ActionArg arg) noexcept;
bool text_auto(ActionContext& ctx, core::ActionArg arg) noexcept;       // F6: the automatic name
// The keyboard opened on a name (OS-NAMES): `current` prefilled and selected, at most `cap` characters, every key; `subject` is the track slot or the pattern id the accept will name. False (nothing opened) when
// there is no document to write the name into, after saying so on the strip.
bool name_open(ActionContext& ctx, TextEntryView::Target target, unsigned subject, const char* current, unsigned cap) noexcept;
bool name_accept(ActionContext& ctx) noexcept;                            // F8 for the Track, Pattern and Project targets (OS-NAMES; called by text.accept): the typed name is an undo unit
bool text_accept(ActionContext& ctx, core::ActionArg arg) noexcept;     // F8: the typed name goes to the save (Save As)

// rec_actions.cpp: the live recording (OS-FU2, redone by OS-LIVEREC, rec_place.h has the rules). rec_active: Edit on the Pattern screen with the sequencer playing; rec_note / rec_pad write
// at the playhead's step on the cursor's track (the callers above delegate to them when it holds); rec.note_off is the release of a recorded note (arg = the note 0..127).
bool rec_active(const ActionContext& ctx) noexcept;
bool rec_counting_in(const ActionContext& ctx) noexcept;  // OS-METRO: Edit is armed and the transport counts in: the pads sound, nothing is written
bool rec_note(ActionContext& ctx, unsigned note, unsigned velocity) noexcept;
bool rec_pad(ActionContext& ctx, unsigned pad) noexcept;
bool rec_note_off(ActionContext& ctx, core::ActionArg arg) noexcept;
// The take (OS-LIVEREC, maintainer decision: the whole take): one undo unit for everything recorded from the moment the Pattern screen records (Edit while it plays, follow or free) until it stops
// (Rec off, Stop, another screen). rec_take_sync opens the undo group when recording starts and closes it when it ends (App calls it after every pass and every action); rec_take_end closes it
// at once (an edit of another kind, Undo and Redo, a macro, a new document). A take with nothing recorded leaves no entry (the journal writes a unit's header with its first record).
void rec_take_sync(model::Editor* editor, const Model& model, ViewState& view) noexcept;
void rec_take_end(model::Editor* editor, PatternEdit& edit) noexcept;
// Whether this action records into the take (kActionRecords, and for pads.play the PIANO / ISO modes only: the STEP pads edit).
bool rec_keeps_take(const ActionDef& action, const ViewState& view) noexcept;
bool step_note_delta(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = detents
bool step_note_octave(ActionContext& ctx, core::ActionArg arg) noexcept;        // arg = octaves (+-12 notes each)
bool step_instrument(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = 1..64
bool step_instrument_delta(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = detents
bool step_instrument_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;  // arg = detents, x8 places (OS-SHIFTENC, Shift + encoder)
bool step_vol(ActionContext& ctx, core::ActionArg arg) noexcept;                // arg = 0..127
bool step_vol_pad(ActionContext& ctx, core::ActionArg arg) noexcept;            // arg = pad 0..47 mapped to 0..127
bool step_vol_delta(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = detents
bool step_vol_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = detents, x10 (OS-SHIFTENC)
bool step_fx_type(ActionContext& ctx, core::ActionArg arg) noexcept;            // arg = an executable FX type (0 clears), on the FX column
bool step_fx_type_delta(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = detents (their sign): one picker place
bool step_fx_value(ActionContext& ctx, core::ActionArg arg) noexcept;           // arg = a value, clamped to the type's range
bool step_fx_value_delta(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = detents
bool step_fx_value_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = detents, x16 (OS-SHIFTENC)
bool step_fx_pad(ActionContext& ctx, core::ActionArg arg) noexcept;             // arg = pad 0..47 mapped over the type's range
bool step_clear(ActionContext& ctx, core::ActionArg arg) noexcept;              // arg = elements (model/step_edit.h kEl*), 0 = what the field says
bool block_copy(ActionContext& ctx, core::ActionArg arg) noexcept;               // OS-PATCOPY: the block (or the cursor step) into the pattern clipboard
bool block_paste(ActionContext& ctx, core::ActionArg arg) noexcept;              // OS-PATCOPY: the clipboard at the cursor, clipped, one undo unit
bool pattern_op(ActionContext& ctx, core::ActionArg arg) noexcept;               // PAT-OPS-A: DUPLICATE x2 / EXPAND x2 / SHRINK /2 of the pattern in view (arg model::ops::Op), one undo unit; Pattern F7 TOOLS runs it
bool pattern_clip_copy(ActionContext& ctx, core::ActionArg arg) noexcept;        // PAT-OPS-B: with F1 held (the PATTERN chooser) the chooser's pattern whole into the clipboard
bool pattern_clip_paste(ActionContext& ctx, core::ActionArg arg) noexcept;       // PAT-OPS-B: with F1 held, Shift + Copy: the whole clip over the chooser's pattern (steps, lengths, name), one undo unit
bool step_insert(ActionContext& ctx, core::ActionArg arg) noexcept;             // shifts the block's tracks down from the cursor
bool step_pull(ActionContext& ctx, core::ActionArg arg) noexcept;          // OS-DELPULL: deletes the cursor step and pulls the block's tracks up from there, the inverse of Insert
bool track_mute(ActionContext& ctx, core::ActionArg arg) noexcept;              // arg = track 0..15: toggles it (an undo unit)
bool track_solo(ActionContext& ctx, core::ActionArg arg) noexcept;
bool track_mute_mode(ActionContext& ctx, core::ActionArg arg) noexcept;                  // arg = track: Immediate, Bar, Pattern (OS-MUTE; the project's state, OS-PERSIST2)

// chooser_actions.cpp: the strip choosers of F1..F3 (pattern, length, step jump).
bool chooser_toggle(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = 0 pattern, 1 length, 2 jump: open, close, or switch to it
bool chooser_hold(ActionContext& ctx, core::ActionArg arg) noexcept;      // the F-key was held: open it until the release
bool chooser_release(ActionContext& ctx, core::ActionArg arg) noexcept;   // the release of that hold closes what it opened
bool chooser_delta(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = +- places (Up, Down, the encoder)
bool chooser_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = detents, 8 places each (Shift + encoder; OS-SHIFTENC)
bool chooser_defined(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = +-1: the next defined value (Left, Right)
bool chooser_pad(ActionContext& ctx, core::ActionArg arg) noexcept;       // arg = pad 0..47

// step_popup_actions.cpp: the step popups (OS-FU3): a held Instr, Fx or Note button on Pattern (Instr on the Instrument page) opens a list that lasts as long as the button is down.
bool popup_open(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = StepPopupView::Kind: the button was held
bool popup_release(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = the kind of the button let go: closes it and, in Edit, enters the choice in the cursor step
bool popup_move(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = 1 Up, 2 Right, 3 Down, 4 Left
bool popup_page(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = pages, + down (OS-NAV3, Shift + Up / Down): a page is the entries of the list in view
bool popup_delta(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = detents (the encoder)
bool popup_pad(ActionContext& ctx, core::ActionArg arg) noexcept;       // arg = pad 0..47

// preview_actions.cpp: the held Preview key of Wave / Chop, Sample edit and the Record take (OS-FU3).
bool preview_hold(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = the key's Button id, | 0x100 for Shift + Preview (from where the screen is)
bool record_preview(ActionContext& ctx, core::ActionArg arg) noexcept;  // arg = the key's Button id

// ui_actions.cpp: global interface actions.
bool ui_none(ActionContext& ctx, core::ActionArg arg) noexcept;             // Shift + F1..F8 with no function: nothing
bool ui_softkey(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = 0..7: notify F1..F8's label
bool learn_toggle(ActionContext& ctx, core::ActionArg arg) noexcept;        // open / close the Learn overlay
bool ui_theme_next(ActionContext& ctx, core::ActionArg arg) noexcept;      // Amiga, Black, White, Amiga, ...

// edit_actions.cpp: history and macros (OS-002c).
// OS-UNDOGRP: the history a screen's edits belong to (D1-D3): Pattern, Song, Sound (Instrument, Wave, Sample edit); Auto for the Mixer (its own actions are final, per action) and the screens that
// have none of their own (the first record of the unit decides). F8 undoes the area of the screen it is pressed on.
model::UndoArea screen_undo_area(ScreenId screen) noexcept;
bool edit_undo(ActionContext& ctx, core::ActionArg arg) noexcept;       // the previous undo unit, through the Project's mutators
bool edit_redo(ActionContext& ctx, core::ActionArg arg) noexcept;       // what undo took back
bool project_tempo(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = BPM delta; undoable
bool macro_record(ActionContext& ctx, core::ActionArg arg) noexcept;    // start / stop recording
bool macro_play(ActionContext& ctx, core::ActionArg arg) noexcept;      // replay the recorded (action, arg) pairs as one undo unit

// param_actions.cpp: parameters, their generated page and MIDI Learn (OS-002d). All work on the parameter under the base screen's cursor
// (Screen::focus_param) through the descriptors, so they serve every screen that has one.
bool param_edit(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = detents, + up: the fine step; undoable if a project is open
bool param_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = detents, each the descriptor's coarse step (Shift + encoder)
bool param_reset(ActionContext& ctx, core::ActionArg arg) noexcept;     // the parameter back to its default
bool param_group(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = page of the screen in view (F1..F4 of the parameter page)
bool config_page_default(ActionContext& ctx, core::ActionArg arg) noexcept;  // Shift + F7 on Config (OS-SHIFTROW): every row of the page to its default, on a second press within 2 s
bool param_apply(ActionContext& ctx, core::ActionArg arg) noexcept;     // F7 of Config: the pending values (kParamApply rows) are written, on the second press (OS-USBAUDIO)
bool learn_clear(ActionContext& ctx, core::ActionArg arg) noexcept;     // unbind the Learn target (F1 in the Learn overlay)
bool learn_mode(ActionContext& ctx, core::ActionArg arg) noexcept;      // the target's binding: pickup, jump, relative, pickup (F2)
bool learn_delete(ActionContext& ctx, core::ActionArg arg) noexcept;    // delete the binding the list cursor is on (F3; OS-013a)
bool learn_clear_all(ActionContext& ctx, core::ActionArg arg) noexcept; // clear both stores: the first press arms, the second clears (F4; OS-013a)
// A control change message: arg = (channel << 16) | (cc << 8) | value, channel 1..16 (0: unknown), cc and value 0..127. With the Learn
// overlay up it binds the controller to the target; otherwise it moves what it is bound to. MIDI input enqueues it, it never runs in an ISR.
bool midi_cc(ActionContext& ctx, core::ActionArg arg) noexcept;

// file_actions.cpp: the File screen (OS-010a).
bool file_open(ActionContext& ctx, core::ActionArg arg) noexcept;              // F1: load the project folder under the cursor (ProjectSink)
bool file_open_available(const ActionContext& ctx) noexcept;                   // a project load is attached (the device); none in the simulator
bool file_rescan(ActionContext& ctx, core::ActionArg arg) noexcept;            // F2: read the folder again
bool file_new(ActionContext& ctx, core::ActionArg arg) noexcept;               // F3: a fresh project in the document (asks ALL (F3) or KEEP INSTRUMENTS (F1); OS-007o, OS-NEWKEEP); UP on the IMPORT page
bool file_name(ActionContext& ctx, core::ActionArg arg) noexcept;              // Shift + F6 NAME (OS-NAMES): the keyboard on the open project's title (not its folder)
bool file_page(ActionContext& ctx, core::ActionArg arg) noexcept;              // F6: the IMPORT page and back (OS-FILES)
bool file_engine(ActionContext& ctx, core::ActionArg arg) noexcept;            // F7 (IMPORT page): where the import's commands go (OS-FILES)
bool file_report(ActionContext& ctx, core::ActionArg arg) noexcept;            // F8 (IMPORT page): the last import's report (OS-FILES)
// file_ops_actions.cpp: OS-FILES part B, on the PROJECTS page. Nothing erases or renames a folder, nothing touches the open project.
bool file_delete(ActionContext& ctx, core::ActionArg arg) noexcept;            // the Delete key: the folder gets an empty .deleted file (the list hides it), on the second press
bool file_purge(ActionContext& ctx, core::ActionArg arg) noexcept;             // Shift + F7: the unused instruments leave their slots (one undo unit); the card is not touched
// save_actions.cpp: the saves (OS-007n), through the SaveService and the card's safety gate (hal/storage_write.h).
bool file_save(ActionContext& ctx, core::ActionArg arg) noexcept;              // F4: the project into its folder, the old file kept as project.o3pj.bak
bool file_save_as(ActionContext& ctx, core::ActionArg arg) noexcept;           // F5: the project into a new folder, Projects/Track NN
bool sedit_save_wav(ActionContext& ctx, core::ActionArg arg) noexcept;         // Shift + F7 on Sample edit: the sample of the instrument as Samples/SMP_NNN.wav
bool save_report(ActionContext& ctx, SaveService::Start start) noexcept;       // how a save's start went, on the strip (Started says nothing: the job's own line takes over); the export's actions use it too
// export_actions.cpp: the WAV export (OS-WAVEXP), the File screen's EXPORT page and the progress overlay.
bool file_export(ActionContext& ctx, core::ActionArg arg) noexcept;            // F1 on the EXPORT page: the row in view (SONG, or PATTERN nn)
bool export_pattern(ActionContext& ctx, core::ActionArg arg) noexcept;         // the pattern in view, from anywhere (Pattern F7 TOOLS / EXPORT PATTERN runs it, tools_run)
bool export_selection(ActionContext& ctx, core::ActionArg arg) noexcept;       // the Pattern's block: its tracks, from its first row to its last (Pattern F7 TOOLS / EXPORT SELECTION, tools_run; OS-WAVEXP slice B)
bool export_slots(ActionContext& ctx, unsigned first, unsigned last, std::uint16_t track_mask) noexcept;  // the Song's slots first..last (order positions), once, with the tail: Song F6 TOOLS / EXPORT WAV (songtools_apply; OS-SONGSHARE S5)
bool export_run(ActionContext& ctx, core::ActionArg arg) noexcept;             // the App's: the transport has stopped, start the armed export
bool export_cancel(ActionContext& ctx, core::ActionArg arg) noexcept;          // F8 of the progress overlay
bool export_value(ActionContext& ctx, core::ActionArg arg) noexcept;           // the encoder on the PATTERN row: the next / previous existing pattern
bool export_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;          // Shift + encoder: 8 existing patterns a detent (OS-SHIFTENC)

// export_actions.cpp: the upper-case copy of a text, cut at `cap` - 1 characters; the file names of an export and the fault texts of the firmware dialog.
void upper_copy(char* out, std::size_t cap, const char* in) noexcept;

// firmware_actions.cpp: the firmware update dialog of Config's INFO page (OS-FWCONFIG, firmware_view.h), over the FirmwareSink port.
bool firmware_open(ActionContext& ctx, core::ActionArg arg) noexcept;          // F8 UPDATE on the INFO page: the checks of the spec, the list copied in one pass, the dialog
bool firmware_go(ActionContext& ctx, core::ActionArg arg) noexcept;            // F1: UPDATE (question 1), YES (the check), INSTALL (the hand-over)
bool firmware_back(ActionContext& ctx, core::ActionArg arg) noexcept;          // F8: BACK closes the list, CANCEL takes a question or a running check back; nothing cancels an install that was taken
bool firmware_question_taken_back(const ViewState& view, const core::InputEvent& event) noexcept;  // does this input take the open question back (any control but F1 / F8)?
void firmware_question_drop(ViewState& view, hal::firmware::FirmwareSink* sink) noexcept;         // ... and take it back: cancel() when a good check was waiting, then the list
bool firmware_poll(ViewState& view, hal::firmware::FirmwareSink& sink, core::Tick now) noexcept;  // the App's pass: the check and the install are followed a slice at a time; true when the picture changed
bool firmware_abandon(ViewState& view, hal::firmware::FirmwareSink* sink) noexcept;          // the card was taken out or put in: a dialog not yet past INSTALL is closed (cancel() first)

// sample_actions.cpp: the Sample load screen (OS-011a).
bool sample_load(ActionContext& ctx, core::ActionArg arg) noexcept;     // F1 / F4 (arg 0 / 1): the WAV under the cursor into the target / the first free instrument
bool sample_enter(ActionContext& ctx, core::ActionArg arg) noexcept;    // F3: the folder under the cursor
bool sample_up(ActionContext& ctx, core::ActionArg arg) noexcept;       // F2: the folder above
bool sample_rescan(ActionContext& ctx, core::ActionArg arg) noexcept;   // F6: read the folder again
bool sample_play(ActionContext& ctx, core::ActionArg arg) noexcept;     // F5: the audition while held (OS-FU2)
bool sample_wt(ActionContext& ctx, core::ActionArg arg) noexcept;       // F7: how a WAV is read as a wavetable (OS-GRAN)
bool sample_channel(ActionContext& ctx, core::ActionArg arg) noexcept;  // F8: which channel of a stereo file is loaded: MIX, LEFT, RIGHT (OS-LOADCH)
bool sample_mark(ActionContext& ctx, core::ActionArg arg) noexcept;     // Shift + Right / Left (arg 0 / 1): mark the file under the cursor and step down / clear the marks (OS-AUDIT-A7, SLD-MULTI)
bool sample_target(ActionContext& ctx, core::ActionArg arg) noexcept;  // Shift + F7 / F8 (arg -1 / +1): the target instrument one back / on, 1..48 round (OS-SLDTARGET)

// wave_actions.cpp: the Wave / Chop screen (OS-011a). The edits are undo units; the zoom, ZC, the point and the EVEN count are view state.
bool wave_enabled(const ActionContext& ctx) noexcept;                       // the Wave screen is in view and there is sample memory
bool wave_nudge(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = encoder clicks: the selected point (WAVE) or slice (CHOP)
bool wave_nudge_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;   // the same, 16 columns a click (Shift)
bool wave_point(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = PointId: which point the encoder moves
bool wave_zoom_action(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = +1 in, -1 out
bool wave_zc(ActionContext& ctx, core::ActionArg arg) noexcept;             // zero-crossing snap on / off
bool wave_even_n(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = +-1: how many slices EVEN makes
bool wave_add(ActionContext& ctx, core::ActionArg arg) noexcept;            // splits the selected slice at its middle
bool wave_del(ActionContext& ctx, core::ActionArg arg) noexcept;            // the selected slice goes
bool wave_even(ActionContext& ctx, core::ActionArg arg) noexcept;           // n equal slices
bool wave_auto(ActionContext& ctx, core::ActionArg arg) noexcept;           // starts the transient slicer (the App runs the job)
bool wave_auto_apply(ActionContext& ctx, core::ActionArg arg) noexcept;     // the slicer's answer, from the App
bool wave_mode_open(ActionContext& ctx, core::ActionArg arg) noexcept;      // OS-INSTUI S4, Shift + F1 MODE: the list of the eight play modes, on the instrument's own
bool wave_mode_pick(ActionContext& ctx, core::ActionArg arg) noexcept;      // the list's F1: play_mode_set for the chosen mode (one undo unit), then the view that mode has
bool wave_pad(ActionContext& ctx, core::ActionArg arg) noexcept;            // arg = pad 0..47: plays slice n and selects it (CHOP)
bool wave_pad_select(ActionContext& ctx, core::ActionArg arg) noexcept;     // the same, silently (Shift + pad)

// sample_edit_actions.cpp: the Sample edit screen (OS-011a). Moving the selection, the factor and the requests are view state; commit, undo and redo are undo units.
bool sedit_enabled(const ActionContext& ctx) noexcept;
bool sedit_move(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = encoder clicks: the selected edge of the selection
bool sedit_move_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;  // 16 columns a click (Shift)
bool sedit_all(ActionContext& ctx, core::ActionArg arg) noexcept;          // no selection: the effects act on all of it
bool sedit_amp(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = +-1: Amplify's percentage, 10 at a time
bool sedit_run(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = EditOp: ask for the effect (the App runs the job)
bool sedit_fx(ActionContext& ctx, core::ActionArg arg) noexcept;           // OS-AUDIT-15: the FX page's encoder, arg = clicks: the row's value
bool sedit_fx_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;    // the same with Shift: the coarse step
bool sedit_amiga(ActionContext& ctx, core::ActionArg arg) noexcept;     // Shift + F1 / F2: the AMIGA conversion, arg 0: 8,287 Hz, 1: 16,574 Hz (OS-AMG1)
bool sedit_q8(ActionContext& ctx, core::ActionArg arg) noexcept;          // Shift + F3: its 8-bit option
bool sedit_instrument(ActionContext& ctx, core::ActionArg arg) noexcept;  // Shift + F5: the next instrument, 1..64 round (OS-QAFIX-27, OS-SLD-RANGE-ALL; a pad picks one of 1..48 too)
bool sample_commit(ActionContext& ctx, core::ActionArg arg) noexcept;      // the finished effect's result becomes the slot's audio (the App invokes it)
bool sedit_undo(ActionContext& ctx, core::ActionArg arg) noexcept;         // the sample's own history, one step back
bool sedit_redo(ActionContext& ctx, core::ActionArg arg) noexcept;

// module_actions.cpp: the SD modules (OS-012b). The picker, the MODULES screen and an app's page are view state and the module system's; the pick is an undo unit.
bool module_engine(ActionContext& ctx, core::ActionArg arg) noexcept;    // F1 on the Instrument page: opens the engine picker
bool module_pick(ActionContext& ctx, core::ActionArg arg) noexcept;      // F1 in the picker: binds the chosen module to the instrument (undoable)
bool module_rescan(ActionContext& ctx, core::ActionArg arg) noexcept;    // MODULES F2: reads modules/apps again
bool module_launch(ActionContext& ctx, core::ActionArg arg) noexcept;    // MODULES F1: launches the app under the cursor
bool module_exit(ActionContext& ctx, core::ActionArg arg) noexcept;      // an app page's exit chord (F7 + F8)
bool module_app_edit(ActionContext& ctx, core::ActionArg arg) noexcept;  // Shift + encoder on an app page: arg = detents
bool module_app_reset(ActionContext& ctx, core::ActionArg arg) noexcept; // Shift + Delete: the default
bool module_app_tempo(ActionContext& ctx, core::ActionArg arg) noexcept; // OS-H1: an app's queued set_tempo (BPM x 100), one undo unit; App::poll_modules runs it
bool module_app_group(ActionContext& ctx, core::ActionArg arg) noexcept;  // OS-H3A: an app's closed edit group (edit_begin .. edit_end), one undo unit named by the app; the sink App::attach_modules gives ModuleUi runs it
bool tools_open(ActionContext& ctx, core::ActionArg arg) noexcept;        // OS-H3A, Pattern F7: the TOOLS list (refused silently while the view follows the play line)
bool tools_run(ActionContext& ctx, core::ActionArg arg) noexcept;         // F1 in the list: launches the app under the cursor with the Pattern's block as its context
ScreenId app_exit_screen(ViewState& view) noexcept;                       // OS-H3A: where an app's page returns to (Pattern when it was opened from TOOLS, else MODULES); clears the mark

// job_actions.cpp: the job runner.
bool job_cancel(ActionContext& ctx, core::ActionArg arg) noexcept;      // cancel every running job

// mixer_actions.cpp: the Mixer screen (OS-011b). They act on what the cursor is on (mixer_view.h); the model's parameters go through the generic edit.
bool mixer_edit(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = detents: a level, a route bit, OUT MODE, a stem source; the encoder
bool mixer_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;  // Shift + encoder
bool mixer_master_vol(ActionContext& ctx, core::ActionArg arg) noexcept;  // OS-MASTERVOL, Master held + the encoder, on any screen: arg = detents of the master VOLUME; the strip says the value
bool mixer_reset(ActionContext& ctx, core::ActionArg arg) noexcept;   // Delete: the default (the engine's for a routing value); STEMS: arg 0 clears the cursor's source, arg 1 (Shift) the factory map
bool mixer_stem_set(ActionContext& ctx, core::ActionArg arg) noexcept;  // Insert on STEMS: the cursor's source joins the slot
bool mixer_mute(ActionContext& ctx, core::ActionArg arg) noexcept;    // F7: the track's mute (SND A / SND B: the effect's)
bool mixer_solo(ActionContext& ctx, core::ActionArg arg) noexcept;    // F8: the track's solo
bool mixer_name(ActionContext& ctx, core::ActionArg arg) noexcept;      // Shift + F1 NAME (OS-NAMES): the keyboard on the name of the track under the cursor (TRACKS ONLY elsewhere)
bool mixer_mute_none(ActionContext& ctx, core::ActionArg arg) noexcept; // Shift + F7 (OS-SHIFTROW): every strip's mute off, one undo unit
bool mixer_solo_none(ActionContext& ctx, core::ActionArg arg) noexcept; // Shift + F8: every strip's solo off
bool mixer_page(ActionContext& ctx, core::ActionArg arg) noexcept;    // F1..F4: arg = MixerPage
bool mixer_apply(ActionContext& ctx, core::ActionArg arg) noexcept;   // F6 APPLY on the STEMS page: the pending OUTPUTS value to the USB driver, pressed twice (OS-USBOUTS)
bool mixer_cue(ActionContext& ctx, core::ActionArg arg) noexcept;     // F5 CUE LVL: the MASTER FX page on the CUE level

// perform_actions.cpp: the Perform screen (OS-011b), on the slot bank of seq/perform.h.
bool perform_pad(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = pad 0..47: its slot's row is selected and held
bool perform_pad_up(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = pad: the release (the App sends it)
bool perform_turn(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = detents: the live value of the held slots
bool perform_track(ActionContext& ctx, core::ActionArg arg) noexcept;    // arg = track 0..11: joins or leaves the layer (the TRACKS pads' row 1, OS-PADS-A)
bool perform_key(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = F1..F8 (0..7) pressed: scenes, half / double time, LATCH, NEXT PATTERN, RESET (OS-PERF2)
bool perform_key_up(ActionContext& ctx, core::ActionArg arg) noexcept;   // arg = the key: its release (the App sends it): the rate back, a tapped scene recalled
bool perform_edit(ActionContext& ctx, core::ActionArg arg) noexcept;     // Rec: the EDIT state
bool perform_move(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = +-1: the edit place (EDIT)
bool perform_swap(ActionContext& ctx, core::ActionArg arg) noexcept;     // arg = +-1: the slot under the edit place swaps with its neighbour (EDIT)
bool perform_fx(ActionContext& ctx, core::ActionArg arg) noexcept;       // arg = +-1: the slot's FX (EDIT)
bool perform_copy(ActionContext& ctx, core::ActionArg arg) noexcept;     // Copy (EDIT)
bool perform_paste(ActionContext& ctx, core::ActionArg arg) noexcept;    // Shift + Copy (EDIT)

// dj_actions.cpp: the DJ page of Perform (OS-DJ A1, dj.h). Runtime state in ViewState::dj, nothing undoable; the engine hears it from App::poll_dj.
bool dj_pad(ActionContext& ctx, core::ActionArg arg) noexcept;           // arg = pad 0..47: row 4 is the crossfader in 12 steps
bool dj_turn(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = detents: the crossfader, 4 positions a detent
bool dj_fine(ActionContext& ctx, core::ActionArg arg) noexcept;          // arg = detents (Shift): one position a detent
bool dj_group(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = track 0..11 (the TRACKS layer's row 1): THRU -> A -> B
bool dj_key(ActionContext& ctx, core::ActionArg arg) noexcept;           // arg = F1..F8 (0..7): F3 CURVE, F6 LATCH, F8 RESET
bool dj_stop_len(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = detents (OS-DJSTOPLEN): the TAPE STOP's length while its pad is held (the App routes that one)
bool dj_filter(ActionContext& ctx, core::ActionArg arg) noexcept;        // arg = detents (A2): the filter, 10 a detent (F4 held, or the FILT pad held: the App routes that one)
bool dj_pad_up(ActionContext& ctx, core::ActionArg arg) noexcept;        // arg = the pad: its release (the App sends it): the FILT pad up springs the filter back
bool dj_key_up(ActionContext& ctx, core::ActionArg arg) noexcept;        // arg = the key 0..7: its release (the App sends it): F4 up springs the filter back
bool dj_nudge(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = -1 / +1 (A4): the left / right arrow down - the tempo nudge while held (the App tells the sequencer: dj_tempo_publish)
bool dj_nudge_up(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = the button id (Left / Right): its release (the App sends it) ends the nudge
bool dj_tempo(ActionContext& ctx, core::ActionArg arg) noexcept;         // arg = -1 / +1 (A4): Shift + the arrow, the project tempo by 0.1 BPM (one undo unit)
bool pattern_queue_tap(ActionContext& ctx, unsigned id) noexcept;        // pad_mode_actions.cpp: the PATS tap on pattern `id` (1..255): queue it, a second tap cancels, stopped: select it
const char* next_song_row(const ActionContext& ctx, unsigned& row) noexcept;  // perform_actions.cpp: the order position after the pattern that plays (NEXT PATTERN, the DJ TRANS pads), or why there is none
bool clock_slave(const ActionContext& ctx) noexcept;                     // song_actions.cpp: CLOCK MODE is Slave: the tempo is the master's, not ours to edit
bool song_tempo_hold(ActionContext& ctx, core::ActionArg arg) noexcept;  // OS-PATTEMPO, Pattern held + the encoder, any screen: arg = detents (a BPM each, Shift a tenth); the strip says the tempo
bool song_tempo_tap(ActionContext& ctx, core::ActionArg arg) noexcept;   // OS-PATTEMPO, Pattern held + a pad (the App takes the pad): a tempo tap at view.tap_tempo.stamp
bool tempo_fine_step(ActionContext& ctx, int tenths) noexcept;           // song_actions.cpp: the project tempo by `tenths` x 0.1 BPM through the editor and the sequencer's tempo param

// record_actions.cpp: the Record screen (OS-011c), on the recorder (recorder.h).
bool record_edit(ActionContext& ctx, core::ActionArg arg) noexcept;      // arg = detents: the row under the cursor (source, gain, monitor, start, tap, frequency, region, trim)
bool record_coarse(ActionContext& ctx, core::ActionArg arg) noexcept;    // Shift + encoder
bool record_reset(ActionContext& ctx, core::ActionArg arg) noexcept;     // Delete: the row's default
bool record_toggle(ActionContext& ctx, core::ActionArg arg) noexcept;    // F1 and Rec: arm / stop
bool record_pad(ActionContext& ctx, core::ActionArg arg) noexcept;       // a pad: plays the take under review chromatically (OS-REC2); arg = the pad 0..47
bool record_monitor(ActionContext& ctx, core::ActionArg arg) noexcept;   // F2
bool record_save(ActionContext& ctx, core::ActionArg arg) noexcept;      // F3: the take's window into the first free instrument slot
bool record_discard(ActionContext& ctx, core::ActionArg arg) noexcept;   // F4 (asks first when the take is not saved: OS-QAFIX-E-SREC-09)
bool record_crop(ActionContext& ctx, core::ActionArg arg) noexcept;      // Shift + F3: the take becomes its window (OS-RECCROP; asks first)
bool record_seek(ActionContext& ctx, core::ActionArg arg) noexcept;      // F5 / F6: arg = -1 / +1 (the RADIO page)
bool record_wav(ActionContext& ctx, core::ActionArg arg) noexcept;       // F7: the take's window as Samples/REC_NNN.wav (save_actions.cpp)

}  // namespace o3os::app
