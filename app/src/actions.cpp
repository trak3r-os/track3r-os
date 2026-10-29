// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/actions.h>

#include "action_handlers.h"

#include <o3os/app/sample_bank.h>
#include "pattern_edit_util.h"
#include "session_access.h"

namespace o3os::app {

namespace {

// The cursor actions work on whatever owns the input (the grid, the list, the Navigator while it is up), so they do not
// need the base screen's focus; the pattern's own actions do.
constexpr core::ActionFlags kNav = core::kActionRepeatable;
constexpr core::ActionFlags kFocus = core::kActionNeedsFocus;
constexpr core::ActionFlags kEdit = core::kActionRepeatable | core::kActionUndoableIfOpen | core::kActionNeedsFocus;
constexpr core::ActionFlags kEditOnce = core::kActionUndoableIfOpen | core::kActionNeedsFocus;
// OS-009: the pattern's edit actions are undoable (Disabled without a document, which says NO PROJECT) and enabled in Edit.
constexpr core::ActionFlags kStep = core::kActionUndoable | core::kActionNeedsFocus | core::kActionSaysNoProject;
constexpr core::ActionFlags kStepRepeat = kStep | core::kActionRepeatable;
constexpr core::ActionFlags kStepRec = kStep | core::kActionRecords;                                // the live recording's own (OS-LIVEREC): it joins the take
constexpr core::ActionFlags kEditOnceRec = kEditOnce | core::kActionRecords;
constexpr core::ActionFlags kSongEdit = kStep;  // OS-SONG1: a slot edit is a step edit's kind of action
constexpr core::ActionFlags kChoose = core::kActionUndoableIfOpen;  // a chooser edits the project only for the length

// The registry. One line per action; bindings.cpp says which gestures run them.
constexpr ActionDef kActions[] = {
    make_action("nav.left", nav_left, kNav),
    make_action("nav.right", nav_right, kNav),
    make_action("nav.up", nav_up, kNav),
    make_action("nav.down", nav_down, kNav),
    make_action("nav.scroll", nav_scroll, kNav),
    make_action("cursor.jump_fraction", cursor_jump_fraction),
    make_action("cursor.jump_end", cursor_jump_end),
    make_action("cursor.page", cursor_page, kNav),
    make_action("cursor.track", cursor_track, kNav),
    make_action("cursor.home", cursor_home),
    make_action("cursor.end", cursor_end),
    make_action("nav.screen", nav_screen),
    make_action("nav.peek", nav_peek),  // OS-SONGPEEK
    make_action("nav.peek_end", nav_peek_end),
    make_action("view.mode", view_mode, kFocus),
    make_action("view.mode_next", view_mode_next, kFocus),
    make_action("navigator.open", navigator_open, kFocus),
    make_action("navigator.accept", navigator_accept),
    make_action("ui.back", ui_back),
    make_action("input_test.reset", input_test_reset),
    make_action("pattern.column", pattern_column, kFocus),
    make_action("pattern.fx_column", pattern_fx_column, kFocus),
    make_action("pattern.zoom", pattern_zoom, 0, pattern_screen_in_view),  // OS-ZOOM: no focus needed, the step popup's held Note also zooms
    make_action("instrument.select", instrument_select),
    make_action("instrument.page", instrument_page, kFocus),  // OS-INSTUI S1/S2
    make_action("instrument.turn", instrument_turn, kFocus | core::kActionRepeatable),  // OS-INSTUI S1: Left / Right, wrapping only on a fresh press
    make_action("instrument.step", instrument_step, kFocus),
    make_action("instrument.used", instrument_used, kFocus),
    make_action("instrument.pad", instrument_pad, kFocus),
    make_action("instrument.delete", instrument_delete, kStep),  // OS-INSTUI S5
    make_action("instrument.name", instrument_name, kFocus),  // OS-INSTUI S7: opens the on-screen keyboard; text.accept makes the rename an undo unit
    make_action("instrument.copy", instrument_copy, kFocus),
    make_action("instrument.paste", instrument_paste, kStep),
    make_action("instrument.save", instrument_save, kFocus),
    make_action("instrument.load", instrument_load, kFocus),
    make_action("instrument.init", instrument_init, kStep),
    make_action("instrument.new_midi", instrument_new_midi, kStep),
    make_action("instrument.sample", instrument_sample, kFocus),
    make_action("instrument.engine", instrument_engine, core::kActionUndoable | core::kActionSaysNoProject),  // OS-004d: the instrument's playback engine (INST 1.2); no binding yet, a keymap or a scenario names it
    make_action("instrument.play_mode", instrument_play_mode, core::kActionUndoable | core::kActionSaysNoProject),  // OS-INSTUI S3 / S4: the instrument's play mode, validated
    make_action("ui.none", ui_none, core::kActionRepeatable),  // does nothing, so a held key repeating it is harmless: the FX page binds Shift + Up / Down to it (OS-AUDIT-15; test: every Tap on a repeat button is repeatable)
    make_action("ui.softkey", ui_softkey),
    make_action("learn.toggle", learn_toggle),
    make_action("ui.theme.next", ui_theme_next),
    make_action("edit.undo", edit_undo, kFocus),
    make_action("edit.redo", edit_redo, kFocus),
    make_action("project.tempo", project_tempo, core::kActionUndoable),
    make_action("macro.record", macro_record),
    make_action("macro.play", macro_play, kFocus),
    make_action("job.cancel", job_cancel),
    // OS-012b: the SD modules. The pick is an undo unit (Disabled without a document, which says NO PROJECT) and runs while the picker is up (no focus).
    make_action("module.engine", module_engine, kFocus),
    make_action("module.pick", module_pick, core::kActionUndoable | core::kActionSaysNoProject),
    make_action("module.rescan", module_rescan, kFocus),
    make_action("module.launch", module_launch, kFocus),
    make_action("module.exit", module_exit, kFocus),
    make_action("module.app_edit", module_app_edit, kFocus),
    make_action("module.app_reset", module_app_reset, kFocus),
    make_action("module.app_tempo", module_app_tempo, core::kActionUndoable),
    make_action("module.app_group", module_app_group, core::kActionUndoable),  // OS-H3A: an app's edit group; the unit's argument is the app's catalog entry (UNDO says its name)
    make_action("tools.open", tools_open, kFocus, pattern_screen_in_view),     // OS-H3A: Pattern F7 TOOLS
    make_action("tools.run", tools_run, 0),  // (the list is a modal overlay: no focus)
    // OS-010a: the File screen.
    make_action("file.open", file_open, kFocus, file_open_available),
    make_action("file.rescan", file_rescan, kFocus),
    make_action("file.new", file_new, kFocus, file_open_available),  // (the same availability: a project load is attached)
    make_action("file.name", file_name, kFocus),                   // OS-NAMES: Shift + F6, the keyboard on the project's title
    make_action("file.page", file_page, kFocus),                   // OS-FILES: F6, the IMPORT page of the File screen
    make_action("file.engine", file_engine, kFocus),               // F7 (IMPORT page)
    make_action("file.report", file_report, kFocus),               // F8 (IMPORT page)
    make_action("file.delete", file_delete, kFocus),               // OS-FILES part B: the Delete key (PROJECTS page): the folder is marked .deleted
    make_action("file.purge", file_purge, kStep),                  // Shift + F7 (PROJECTS page): the unused instruments, one undo unit
    // OS-WAVEXP: the WAV export. file.page (F6) and file.engine (F7) act by the page; export.cancel and export.run run under the progress overlay (no focus).
    make_action("file.export", file_export, kFocus),               // F1 (EXPORT page)
    make_action("export.pattern", export_pattern, kFocus),         // the pattern in view, from anywhere (the Pattern TOOLS entry)
    make_action("export.selection", export_selection, kFocus),     // the block of the Pattern screen (the Pattern TOOLS entry, only while a block is active)
    make_action("export.value", export_value, kFocus),
    make_action("export.coarse", export_coarse, kFocus),  // OS-SHIFTENC             // the encoder on the EXPORT page
    make_action("export.cancel", export_cancel),                   // F8 of the progress overlay
    make_action("export.run", export_run),                         // the App's pass, once the transport has stopped
    // OS-007n: the saves. They run while the screen is in view; each says why when it cannot (no card write layer, no writable card, ...).
    make_action("file.save", file_save, kFocus),
    make_action("file.save_as", file_save_as, kFocus),
    make_action("sedit.save_wav", sedit_save_wav, kFocus),
    // OS-011a: the Sample load screen.
    make_action("sample.load", sample_load, kFocus),
    make_action("sample.enter", sample_enter, kFocus),
    make_action("sample.up", sample_up, kFocus),
    make_action("sample.rescan", sample_rescan, kFocus),
    make_action("sample.play", sample_play, kFocus),
    make_action("sample.wt", sample_wt, kFocus),
    make_action("sample.channel", sample_channel, kFocus),
    make_action("sample.target", sample_target, kFocus),
    make_action("sample.mark", sample_mark, kFocus | core::kActionRepeatable),  // Shift+Left / Right: a held key marks a run of files
    // OS-011a: the Wave / Chop screen. The edits are undo units (Disabled without a document, which says NO PROJECT); the rest is view state.
    make_action("wave.nudge", wave_nudge, kStep, wave_enabled),
    make_action("wave.nudge_coarse", wave_nudge_coarse, kStep, wave_enabled),
    make_action("wave.point", wave_point, kFocus),
    make_action("wave.zoom", wave_zoom_action, kFocus),
    make_action("wave.zc", wave_zc, kFocus),
    make_action("wave.even_n", wave_even_n, kFocus | core::kActionRepeatable),  // OS-DELREPEAT: bound to the arrows, which repeat
    make_action("wave.add", wave_add, kStep, wave_enabled),
    make_action("wave.del", wave_del, kStep, wave_enabled),
    make_action("wave.even", wave_even, kStep, wave_enabled),
    make_action("wave.mode", wave_mode_open, kFocus),  // OS-INSTUI S4: Shift + F1 MODE
    make_action("wave.mode_pick", wave_mode_pick, core::kActionUndoable | core::kActionSaysNoProject),  // the list's F1: play_mode_set, one undo unit
    make_action("wave.auto", wave_auto, kFocus, wave_enabled),
    make_action("wave.auto_apply", wave_auto_apply, kStep, wave_enabled),
    make_action("wave.pad", wave_pad, kFocus),
    make_action("wave.pad_select", wave_pad_select, kFocus),
    // OS-011a: the Sample edit screen. sample.commit, undo and redo are undo units of the project (the points a crop moves); the rest is view state.
    make_action("sedit.move", sedit_move, kFocus, sedit_enabled),
    make_action("sedit.move_coarse", sedit_move_coarse, kFocus, sedit_enabled),
    make_action("sedit.all", sedit_all, kFocus),
    make_action("sedit.amp", sedit_amp, kFocus | core::kActionRepeatable),
    make_action("sedit.run", sedit_run, kFocus, sedit_enabled),
    make_action("sedit.fx", sedit_fx, kFocus, sedit_enabled),
    make_action("sedit.fx_coarse", sedit_fx_coarse, kFocus, sedit_enabled),
    make_action("sedit.amiga", sedit_amiga, kFocus, sedit_enabled),
    make_action("sedit.q8", sedit_q8, kFocus, sedit_enabled),
    make_action("sedit.instrument", sedit_instrument, kFocus),
    make_action("sample.commit", sample_commit, kStep, sedit_enabled),
    make_action("sedit.undo", sedit_undo, kStep, sedit_enabled),
    make_action("sedit.redo", sedit_redo, kStep, sedit_enabled),
    // OS-002d: parameters and MIDI Learn. An edit is undoable when a project is open and still works on a device setting without one.
    make_action("param.edit", param_edit, kEdit),
    make_action("param.coarse", param_coarse, kEdit),
    make_action("param.reset", param_reset, kEditOnce),
    make_action("param.group", param_group, kFocus),
    make_action("config.page_default", config_page_default, kFocus),  // OS-SHIFTROW: no undo either (device settings)
    make_action("param.apply", param_apply, kFocus),  // OS-USBAUDIO: no undo (a device setting; the dispatcher opens none for it)
    // OS-FWCONFIG: the firmware update dialog. firmware.open runs from the INFO page (focus); the others run while the dialog is up (no focus). No undo: nothing here edits the project.
    make_action("firmware.open", firmware_open, kFocus),
    make_action("firmware.go", firmware_go),
    make_action("firmware.back", firmware_back),
    make_action("learn.clear", learn_clear, core::kActionUndoableIfOpen),
    make_action("learn.mode", learn_mode, core::kActionUndoableIfOpen),
    make_action("learn.delete", learn_delete, core::kActionUndoableIfOpen),
    make_action("learn.clear_all", learn_clear_all, core::kActionUndoableIfOpen),
    make_action("midi.cc", midi_cc, core::kActionUndoableIfOpen),
    // OS-009 P1a.1: the pattern's edit core. Modes, the selection and the pad preview are view state; the step actions are undo units.
    make_action("pattern.edit_toggle", pattern_edit_toggle, kFocus),
    make_action("pattern.rec_free", pattern_rec_free, kFocus),
    make_action("pattern.preview", pattern_preview),
    make_action("pattern.export", pattern_export, kFocus),  // no binding: EXPORT is Pattern F7 TOOLS now (OS-WAVEXP); a keymap or a scenario can name it, and the stub says where it is
    make_action("pattern.fill", pattern_fill, kStep, pattern_screen_in_view),
    make_action("fill.open", fill_open, kFocus, pattern_screen_in_view),  // OS-PTOOLS: the dialog; its rows are view state, the fill is the undo unit
    make_action("fill.move", fill_move, kNav),
    make_action("fill.delta", fill_delta, kNav),
    make_action("fill.coarse", fill_coarse, kNav),  // OS-SHIFTENC
    make_action("fill.apply", fill_apply, core::kActionUndoable | core::kActionSaysNoProject),
    make_action("pattern.invert", pattern_invert, kStep, pattern_screen_in_view),  // outside Edit the handler says TURN EDIT ON (OS-QAFIX-12)
    make_action("pads.mode", pads_mode, kFocus),
    make_action("pads.octave", pads_octave, kFocus),  // OS-PADOCT
    make_action("pads.track", pads_track, kFocus),
    make_action("pads.play", pads_play, kEditOnceRec),  // OS-LIVEREC: a PIANO / ISO pad records into the take (the other modes do not: dispatch)
    make_action("pads.step_up", pads_step_up, kEditOnce),
    make_action("pads.step_delta", pads_step_delta, kEdit),
    make_action("pad.preview", pad_preview, kFocus),
    make_action("pad.audition", pad_audition, kFocus),
    make_action("transport.play", transport_play, kFocus, transport_available),
    make_action("transport.play_cursor", transport_play_cursor, kFocus, transport_available),
    make_action("select.extend", select_extend, kFocus | kNav, pe::select_enabled),
    make_action("select.column_all", select_column_all, kFocus, pe::select_enabled),
    make_action("select.clear", select_clear, kFocus),
    // OS-SONG1: the Song screen's editing. The edits are undo units (Disabled without a document, which says NO PROJECT); the range, the tempo field and the clipboard are view state.
    make_action("song.add_slot", song_add_slot, kSongEdit, song_enabled),
    make_action("song.delete_slot", song_delete_slot, kSongEdit, song_enabled),
    make_action("song.pattern_delta", song_pattern_delta, kSongEdit | core::kActionRepeatable, song_enabled),
    make_action("song.turn", song_turn, kSongEdit, song_enabled),
    make_action("song.tempo_hold", song_tempo_hold, kStep),  // OS-PATTEMPO: Pattern held + the encoder, any screen (the edit lands on the history of the screen in view, as the Song screen's tempo does)
    make_action("song.tempo_tap", song_tempo_tap, kEditOnce),  // OS-PATTEMPO: Pattern held + a pad
    make_action("song.turn_fine", song_turn_fine, kSongEdit, song_enabled),
    make_action("song.tempo", song_tempo, core::kActionUndoableIfOpen | kFocus, song_enabled),
    make_action("song.select", song_select, kFocus | kNav, song_enabled),
    make_action("song.copy", song_copy, kFocus, song_enabled),
    make_action("song.paste", song_paste, kSongEdit, song_enabled),
    make_action("song.name", song_name, kFocus, song_enabled),  // OS-NAMES: opens the on-screen keyboard on the slot's pattern
    make_action("song.play_pattern", song_play_pattern, kFocus, song_play_enabled),
    make_action("song.queue_jump", song_queue_jump, kFocus, song_play_enabled),
    make_action("song.next_use", song_next_use, kFocus, song_enabled),  // OS-SONGSHARE S4: Note / Shift + Note; the cursor only, no undo unit
    make_action("song.move", song_move, kStepRepeat, song_enabled),  // OS-SONGSHARE S3: Copy held + Up / Down; repeatable, one unit a step
    make_action("song.cut", song_cut, kSongEdit, song_enabled),  // OS-SONGSHARE S2: Shift + Delete, not repeatable: a held key cuts once
    make_action("song.clear", song_clear, kSongEdit, song_enabled),  // OS-SONGMAP: Delete (the cells in the map, the slot on PAT); not repeatable: a held key does it once
    make_action("song.select_tracks", song_select_tracks, kFocus | kNav, song_enabled),
    make_action("song.unique", song_unique, kSongEdit, song_enabled),
    make_action("help.open", help_open, kFocus),  // OS-HELP; closing is ui.back, the scroll is nav.* and cursor.page (HelpOverlay is a Cursor)
    make_action("song.tools", song_tools_open, kFocus, song_enabled),
    make_action("song.answer", song_answer, kSongEdit, song_enabled),
    make_action("songtools.move", songtools_move, kNav),
    make_action("songtools.delta", songtools_delta, kNav),
    make_action("songtools.coarse", songtools_coarse, kNav),
    make_action("songtools.apply", songtools_apply, core::kActionUndoable | core::kActionSaysNoProject),
    make_action("song.pads", song_pads, kFocus, song_enabled),  // OS-SONGPADS: Shift + F7, the Song pads' mode (TRACKS / SLOTS), runtime state
    make_action("song.pad", song_pad, kFocus, song_enabled),    // OS-SONGPADS: a pad in SLOTS mode
    make_action("step.note", step_note, kStep, pe::edit_enabled),
    make_action("step.note_pad", step_note_pad, kStepRec, pe::edit_enabled),
    make_action("step.midi_note", step_midi_note, kStepRec, pe::edit_enabled),
    make_action("text.pad", text_pad),
    make_action("text.move", text_move, core::kActionRepeatable),
    make_action("text.scroll", text_scroll),
    make_action("text.key", text_key),
    make_action("text.caps", text_caps),
    make_action("text.backspace", text_backspace),
    make_action("text.auto", text_auto),
    make_action("text.accept", text_accept, core::kActionUndoableIfOpen),  // OS-INSTUI S7: the Instrument name is an undo unit (the Save As does not edit anything)
    make_action("rec.note_off", rec_note_off, kStepRec),  // OS-FU2: the release of a note recorded at the playhead
    make_action("step.note_delta", step_note_delta, kStep, pe::edit_enabled),
    make_action("step.note_octave", step_note_octave, kStep, pe::edit_enabled),
    make_action("step.instrument", step_instrument, kStep, pe::edit_enabled),
    make_action("step.instrument_delta", step_instrument_delta, kStep, pe::edit_enabled),
    make_action("step.instrument_coarse", step_instrument_coarse, kStep, pe::edit_enabled),  // OS-SHIFTENC
    make_action("step.vol", step_vol, kStep, pe::edit_enabled),
    make_action("step.vol_pad", step_vol_pad, kStep, pe::edit_enabled),
    make_action("step.vol_delta", step_vol_delta, kStep, pe::edit_enabled),
    make_action("step.vol_coarse", step_vol_coarse, kStep, pe::edit_enabled),  // OS-SHIFTENC
    make_action("step.fx_type", step_fx_type, kStep, pe::edit_enabled),
    make_action("step.fx_type_delta", step_fx_type_delta, kStep, pe::edit_enabled),
    make_action("step.fx_value", step_fx_value, kStep, pe::edit_enabled),
    make_action("step.fx_value_delta", step_fx_value_delta, kStep, pe::edit_enabled),
    make_action("step.fx_value_coarse", step_fx_value_coarse, kStep, pe::edit_enabled),  // OS-SHIFTENC
    make_action("step.fx_pad", step_fx_pad, kStep, pe::edit_enabled),
    make_action("step.clear", step_clear, kStepRepeat, pe::edit_enabled),
    make_action("block.copy", block_copy, kFocus),  // OS-PATCOPY: any mode of Pattern (nothing is changed)
    make_action("block.paste", block_paste, kStep, pe::edit_enabled),
    make_action("pattern.op", pattern_op, core::kActionUndoable | core::kActionSaysNoProject, pe::edit_enabled),  // PAT-OPS-A: run by the TOOLS list (a modal overlay: no focus), which says TURN EDIT ON before it gets here
    make_action("pattern.clip_copy", pattern_clip_copy),                                                            // PAT-OPS-B: the PATTERN chooser's Copy (a modal overlay: no focus; it changes nothing)
    make_action("pattern.clip_paste", pattern_clip_paste, core::kActionUndoable | core::kActionSaysNoProject),     // PAT-OPS-B: the PATTERN chooser's Shift + Copy
    make_action("step.insert", step_insert, kStepRepeat, pe::edit_enabled),
    make_action("step.pull", step_pull, kStepRepeat, pe::edit_enabled),
    make_action("track.mute", track_mute, kStep, pe::track_enabled),
    make_action("track.solo", track_solo, kStep, pe::track_enabled),
    make_action("track.mute_mode", track_mute_mode, kFocus, pe::track_enabled),
    make_action("chooser.toggle", chooser_toggle),
    make_action("chooser.hold", chooser_hold),
    make_action("chooser.release", chooser_release),
    make_action("chooser.delta", chooser_delta, kChoose | core::kActionRepeatable),
    make_action("chooser.coarse", chooser_coarse, kChoose),  // OS-SHIFTENC
    make_action("chooser.defined", chooser_defined, kChoose | core::kActionRepeatable),
    make_action("chooser.pad", chooser_pad, kChoose),
    // OS-FU3: the step popups (a held Instr, Fx or Note button). The release enters the choice in the cursor step, an undo unit (a project is open: UndoableIfOpen, as the choosers).
    make_action("popup.open", popup_open, kFocus),
    make_action("popup.release", popup_release, kChoose),
    make_action("popup.move", popup_move, kNav),
    make_action("popup.page", popup_page, kNav),  // OS-NAV3
    make_action("popup.delta", popup_delta, kNav),
    make_action("popup.pad", popup_pad),
    // OS-FU3: the held Preview key (the sound is the App's pass: no undo, no focus needed beyond the screen).
    make_action("preview.hold", preview_hold, kFocus),
    make_action("record.preview", record_preview, kFocus),
    // OS-011b: the Mixer screen. The model's parameters are edited through the generic edit (undoable); the engine's own fields are the board's.
    make_action("mixer.edit", mixer_edit, kEdit),
    make_action("mixer.coarse", mixer_coarse, kEdit),
    make_action("mixer.master_vol", mixer_master_vol, kEditOnceRec),  // OS-MASTERVOL (kActionRecords: a LIVE take stays open under it; the edit is final, so it adds nothing to the take)
    make_action("mixer.reset", mixer_reset, kEditOnce),
    make_action("mixer.stem_set", mixer_stem_set, kEditOnce),
    make_action("mixer.mute", mixer_mute, kEditOnce),
    make_action("mixer.solo", mixer_solo, kEditOnce),
    make_action("mixer.name", mixer_name, kFocus),  // OS-NAMES: opens the on-screen keyboard; text.accept makes the name an undo unit
    make_action("mixer.mute_none", mixer_mute_none, kEditOnce),
    make_action("mixer.solo_none", mixer_solo_none, kEditOnce),
    make_action("mixer.page", mixer_page, kFocus),
    make_action("mixer.cue", mixer_cue, kFocus),
    make_action("mixer.apply", mixer_apply, kFocus),
    // OS-011b: the Perform screen. The slots are runtime state (not undoable); the bank tells the engine.
    make_action("perform.pad", perform_pad, kFocus),
    make_action("perform.pad_up", perform_pad_up),
    make_action("perform.turn", perform_turn, kNav | kFocus),
    make_action("perform.track", perform_track, kFocus),
    make_action("perform.key", perform_key, kFocus),
    make_action("perform.key_up", perform_key_up),
    make_action("perform.edit", perform_edit, kFocus),
    make_action("perform.move", perform_move, kNav | kFocus),
    make_action("perform.swap", perform_swap, kNav | kFocus),
    make_action("perform.fx", perform_fx, kNav | kFocus),
    make_action("perform.copy", perform_copy, kFocus),
    make_action("perform.paste", perform_paste, kFocus),
    // OS-DJ A1: the DJ page of Perform (a field context of its own, bindings.cpp). Runtime state, not undoable.
    make_action("dj.pad", dj_pad, kFocus),
    make_action("dj.turn", dj_turn, kNav | kFocus),
    make_action("dj.fine", dj_fine, kNav | kFocus),
    make_action("dj.group", dj_group, kFocus),
    make_action("dj.key", dj_key, kFocus),
    make_action("dj.filter", dj_filter, kNav | kFocus),
    make_action("dj.stop_len", dj_stop_len, kNav | kFocus),
    make_action("dj.pad_up", dj_pad_up),
    make_action("dj.key_up", dj_key_up),
    make_action("dj.nudge", dj_nudge, kNav | kFocus),  // the arrows repeat: a repeat of the held key is the same nudge
    make_action("dj.nudge_up", dj_nudge_up),
    make_action("dj.tempo", dj_tempo, core::kActionUndoableIfOpen | core::kActionRepeatable | kFocus),  // the project tempo by 0.1 BPM (one undo unit a press, repeating while held)
    // OS-011c: the Record screen. The recorder's state is runtime (not undoable); SAVE makes an instrument of the take and ends the history as an import does.
    make_action("record.edit", record_edit, kNav | kFocus),
    make_action("record.coarse", record_coarse, kNav | kFocus),
    make_action("record.reset", record_reset, kFocus),
    make_action("record.toggle", record_toggle, kFocus),
    make_action("record.pad", record_pad, kFocus),
    make_action("record.monitor", record_monitor, kFocus),
    make_action("record.save", record_save, kFocus),
    make_action("record.discard", record_discard, kFocus),
    make_action("record.crop", record_crop, kFocus),
    make_action("record.seek", record_seek, kFocus),
    make_action("record.wav", record_wav, kFocus),
};
constexpr std::size_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);
static_assert(registry_valid(kActions, kActionCount), "action registry: duplicate or malformed entry");

// Case-insensitive comparison of a registry name with `length` characters of `s`.
bool name_equals(const char* name, const char* s, std::size_t length) noexcept {
  for (std::size_t i = 0; i < length; ++i) {
    const char c = (s[i] >= 'A' && s[i] <= 'Z') ? static_cast<char>(s[i] + ('a' - 'A')) : s[i];
    if (name[i] == '\0' || name[i] != c) {  // never reads past the name's terminator
      return false;
    }
  }
  return name[length] == '\0';
}

// The actions whose area is the first record's, whatever the screen (D2): a controller message resolves to a parameter wherever it is, Learn writes the bindings, an app's group says
// what it touched.
constexpr core::ActionId kAutoArea[] = {core::action_id("midi.cc"), core::action_id("learn.clear"), core::action_id("learn.mode"), core::action_id("learn.delete"),
                                        core::action_id("learn.clear_all"), core::action_id("module.app_group")};

model::UndoArea undo_area_of(core::ActionId id, ScreenId screen) noexcept {
  for (const core::ActionId a : kAutoArea) {
    if (id == a) {
      return model::UndoArea::Auto;
    }
  }
  return screen_undo_area(screen);
}

// The Mixer's own actions (D10): their edits are final, whatever screen is in view and whether a macro plays them.
constexpr core::ActionId kMixerActions[] = {core::action_id("mixer.edit"), core::action_id("mixer.coarse"), core::action_id("mixer.master_vol"), core::action_id("mixer.reset"), core::action_id("mixer.stem_set"),
                                            core::action_id("mixer.mute"), core::action_id("mixer.solo"), core::action_id("mixer.mute_none"), core::action_id("mixer.solo_none")};

bool is_mixer_action(core::ActionId id) noexcept {
  for (const core::ActionId a : kMixerActions) {
    if (id == a) {
      return true;
    }
  }
  return false;
}

constexpr core::ActionId kMacroControls[] = {core::action_id("macro.record"), core::action_id("macro.play"),
                                             core::action_id("edit.undo"), core::action_id("edit.redo"), core::action_id("midi.cc"),
                                             core::action_id("module.app_group")};  // (an app's group lives in the module system's buffer, not in the macro)

// While a macro records, every action that ran is appended, except the controls of the macro and of the history, and midi.cc: a
// controller message is a gesture of the hardware (and its effect depends on where the knob and the parameter are, which a replay
// does not reproduce), so it is not recorded; the edits it makes are. A macro that fills up stops recording and says so.
void record_macro_step(ActionContext& ctx, core::ActionId id, core::ActionArg arg) noexcept {
  if (ctx.session == nullptr || !ctx.session->macro.recording || ctx.session->macro.playing) {
    return;
  }
  for (const core::ActionId control : kMacroControls) {
    if (id == control) {
      return;
    }
  }
  Macro& m = ctx.session->macro;
  if (m.count == kMacroSteps) {
    m.recording = false;
    m.full = true;
    (void)ctx.view.notify.post(core::Severity::Warn, "MACRO FULL", "history", ViewState::kMessageMs);
    return;
  }
  m.steps[m.count++] = MacroStep{id, arg};
}

}  // namespace

ActionTable action_registry() noexcept { return ActionTable{kActions, kActionCount}; }

model::UndoArea screen_undo_area(ScreenId screen) noexcept {
  switch (screen) {
    case ScreenId::Pattern: return model::UndoArea::Pattern;
    case ScreenId::Song: return model::UndoArea::Song;
    case ScreenId::Instrument:
    case ScreenId::Wave:
    case ScreenId::SampleEdit: return model::UndoArea::Sound;
    // (the Mixer has no area: its own actions are final, per action: is_mixer_action. Another action on its screen, an app's tempo, is a unit like anywhere)
    default: return model::UndoArea::Auto;
  }
}

const ActionDef* find_action(ActionTable table, core::ActionId id) noexcept {
  for (std::size_t i = 0; i < table.count; ++i) {
    if (table.items[i].id == id) {
      return &table.items[i];
    }
  }
  return nullptr;
}

const ActionDef* find_action(ActionTable table, const char* name, std::size_t length) noexcept {
  for (std::size_t i = 0; i < table.count; ++i) {
    if (name_equals(table.items[i].name, name, length)) {
      return &table.items[i];
    }
  }
  return nullptr;
}

DispatchResult dispatch(ActionTable table, core::ActionId id, core::ActionArg arg, ActionContext& ctx) noexcept {
  const ActionDef* a = find_action(table, id);
  if (a == nullptr) {
    return DispatchResult::Unknown;
  }
  if (ctx.repeat && (a->flags & core::kActionRepeatable) == 0) {
    return DispatchResult::Unchanged;  // OS-DELREPEAT: a held button's repeat reaches only an action that repeats (the press already ran it): nothing runs, nothing is said or recorded
  }
  if (((a->flags & core::kActionNeedsFocus) != 0 && !ctx.focus) || (a->enabled != nullptr && !a->enabled(ctx))) {
    return DispatchResult::Disabled;
  }
  // An undoable action edits the document, as one undo unit; it is handed the editor and nothing else is (OS-002c).
  const bool undoable_if_open = (a->flags & core::kActionUndoableIfOpen) != 0;
  model::Editor* const editor = ctx.session != nullptr ? SessionAccess::editor(*ctx.session) : nullptr;
  if ((a->flags & core::kActionUndoable) != 0 && editor == nullptr) {
    if ((a->flags & core::kActionSaysNoProject) != 0) {
      (void)ctx.view.notify.post(core::Severity::Info, "NO PROJECT", "edit", ViewState::kMessageMs);
    }
    return DispatchResult::Disabled;  // no document, nothing to edit
  }
  const bool undoable = editor != nullptr && ((a->flags & core::kActionUndoable) != 0 || undoable_if_open);
  // OS-LIVEREC, the take: while the Pattern screen records, one undo group is open (rec_take_sync). What records into it joins it; any other edit closes it first and is a unit of its own, and
  // the next recorded note opens a new take.
  if (undoable && ctx.view.edit.take_open && !rec_keeps_take(*a, ctx.view)) {
    rec_take_end(editor, ctx.view.edit);
  }
  model::Editor* const outer = ctx.edit;  // a macro dispatches from inside an action
  ctx.edit = undoable ? editor : nullptr;
  if (undoable) {
    editor->begin_group(id, arg, undo_area_of(id, ctx.view.screen));
  }
  const ScreenId screen_before = ctx.view.screen;
  const bool was_final = undoable && editor->final_edits();
  if (undoable && is_mixer_action(id)) {
    editor->set_final(true);
  }
  const unsigned redo_before = undoable ? editor->journal(model::UndoArea::Sound).redo_depth() : 0u;  // (the audio swaps are the SOUND area's)
  const bool changed = a->handler(ctx, arg);
  if (undoable) {
    editor->set_final(was_final);
    editor->end_group();
    if (redo_before != 0u && editor->journal(model::UndoArea::Sound).redo_depth() == 0u && ctx.samples != nullptr) {
      ctx.samples->drop_undone_swaps();  // OS-INSTUI S7: a new unit discarded the redo units: the audio swaps of the undone ones go with them
    }
  }
  if (editor != nullptr && ctx.view.screen != screen_before) {
    editor->merge_break();  // a screen change ends a click group (D8)
  }
  ctx.edit = outer;
  record_macro_step(ctx, id, arg);
  return changed ? DispatchResult::Changed : DispatchResult::Unchanged;
}

}  // namespace o3os::app
