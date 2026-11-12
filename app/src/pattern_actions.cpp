// SPDX-License-Identifier: GPL-3.0-or-later
// Pattern-screen actions: the cursor's column, the current instrument, the modes, the selection and the pads' preview (OS-009 P1a.1).
// The edit actions are in step_actions.cpp, the choosers in chooser_actions.cpp.
#include "pattern_edit_util.h"

#include <o3os/app/pad_lights.h>
#include <o3os/app/pad_note.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/screens/wave_screen.h>
#include <o3os/app/song_source.h>

namespace o3os::app {

// OS-ZOOM: a column the zoom level hides is refused with a hint naming the zoom key (the hold popups still open and enter into the cursor step).
constexpr const char* kHiddenColumn = "FIELD HIDDEN: NOTE + INSTR ZOOMS";

bool pattern_column(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < static_cast<core::ActionArg>(Column::Note) || arg > static_cast<core::ActionArg>(Column::Fx2)) {
    return false;
  }
  if (!ctx.view.pattern.column_visible(static_cast<Column>(arg))) {
    return pe::say(ctx, core::Severity::Info, kHiddenColumn);
  }
  return ctx.view.pattern.select_column(static_cast<Column>(arg));
}

bool pattern_fx_column(ActionContext& ctx, core::ActionArg) noexcept {
  if (!ctx.view.pattern.column_visible(Column::Fx1)) {
    return pe::say(ctx, core::Severity::Info, kHiddenColumn);
  }
  return ctx.view.pattern.cycle_fx_column();
}

// Note + Instr (OS-ZOOM, new Community OS behaviour; docs/specs/PAT-NAV.md): the zoom of the Pattern grid, 5, 8, 12 tracks and back. The level is the view's, RAM only. Also in the step popup (the held Note).
bool pattern_zoom(ActionContext& ctx, core::ActionArg) noexcept {
  return ctx.view.pattern.cycle_zoom(pattern_dims(ctx.model, ctx.view));
}

bool instrument_select(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 1 || arg > kInstruments) {
    return false;
  }
  const bool changed = arg != ctx.view.instrument;
  if (changed) {
    release_live_pads(ctx);  // OS-INSTUI O2: a note the Instrument screen's pads hold ends on the instrument that started it
  }
  ctx.view.instrument = arg;
  ctx.view.params.insert = ParamView::kNoInsert;  // OS-INSFX S5: an instrument chosen on an insert's parameter page takes the page over
  if (changed && (ctx.view.screen == ScreenId::Wave || ctx.view.screen == ScreenId::SampleEdit)) {
    wave_view_reset(ctx.view);  // OS-INSTPICK: a pad of Sample edit chooses the sample to edit; its window and selection were the old one's
  }
  return changed;
}

// ---- modes ------------------------------------------------------------------------------------------------------------

namespace {

// OS-METRO, OS-RECMODE2: the bars the transport counts in (the device setting COUNT-IN, 0 = off) when Shift + REC starts it from stop (transport_start_live). Play never counts in, and Shift + Play never does
// either (OS-PLAYCUR: it plays from the cursor's row; it is also the Focus Loop's key, a loop restarts, it does not record).
unsigned count_in_bars(const ActionContext& ctx) noexcept {
  if (ctx.model.project == nullptr || ctx.model.device.kind() != core::ParamKind::Device) {
    return 0;
  }
  const float v = ctx.model.device.get(model::device_setting::CountIn);
  return v >= 1.5f ? 2u : (v >= 0.5f ? 1u : 0u);
}


// Shift + REC from stopped: the transport starts with the pattern being edited, after its count-in when COUNT-IN is on. False: no transport, or it did not start (the view is left alone).
bool transport_start_live(ActionContext& ctx) noexcept {
  if (ctx.transport == nullptr) {
    return false;
  }
  const unsigned bars = count_in_bars(ctx);
  if (!(bars != 0u ? ctx.transport->play_count_in(ctx.view.edit.pattern, bars) : ctx.transport->play(ctx.view.edit.pattern))) {
    return false;
  }
  ctx.view.playing = true;
  return true;
}

// REC or Shift + REC while LIVE is counting in (OS-COUNTIN; the orchestrator's default, to be confirmed by the maintainer): the count-in is cancelled and the transport stops, back to the mode the Shift + REC began
// in (OS-UXF-PAT item 8, PatternEdit::live_from_step: STEP stopped, or WATCH stopped), with nothing recorded or played. True: it was a count-in and is cancelled (or the transport refused to stop and nothing changed: then false).
bool cancel_live_count_in(ActionContext& ctx) noexcept {
  ViewState& v = ctx.view;
  const bool live = pattern_mode_edits(v.mode[screen_index(ScreenId::Pattern)]) && !v.edit.rec_free;
  if (!live || !v.play.playing || v.play.count_in == 0u || ctx.transport == nullptr || !ctx.transport->stop()) {
    return false;
  }
  v.playing = false;
  if (v.edit.live_from_step) {
    v.edit.rec_free = true;  // LIVE -> STEP: the same Edit mode
    v.edit.live_from_step = false;
  } else {
    (void)pattern_set_mode(v, PatternMode::Browse);
  }
  return true;
}

}  // namespace

// REC (OS-RECMODE2, new Community OS behaviour; it replaces OS-RECSWAP's mapping): step edit, on and off. The three states are Browse (WATCH), Edit free (STEP) and Edit following (LIVE, which exists only
// while the transport runs). From WATCH it enters STEP on the spot: the cursor, which rode the playhead, stops on its row (playing or stopped, no playback is started). From STEP it goes back to WATCH.
// From LIVE it goes to WATCH (OS-LIVEEXIT: the playing continues, the view keeps following the play line, and the take ends: rec_take_sync sees the state leave LIVE and closes the undo unit).
bool pattern_edit_toggle(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.screen != ScreenId::Pattern) {
    return false;
  }
  if (cancel_live_count_in(ctx)) {
    return true;
  }
  if (pattern_mode_edits(v.mode[screen_index(ScreenId::Pattern)])) {
    return pattern_set_mode(v, PatternMode::Browse);  // LIVE -> WATCH (OS-LIVEEXIT) and STEP -> WATCH (leaving Edit clears rec_free)
  }
  (void)pattern_set_mode(v, PatternMode::Edit);
  v.edit.rec_free = true;  // (after the mode: leaving Edit clears it, entering does not)
  return true;
}

// SHIFT + REC (OS-RECMODE2; OS-UXF-PAT item 8): live recording, on and off, from any mode. Playing, WATCH or STEP become LIVE (Edit following: the cursor rides the playhead, notes are written at the play line);
// LIVE goes back to the mode it was entered from (PatternEdit::live_from_step): STEP (the cursor stays on the row the play line is on, OS-LIVEEXIT) or WATCH (the view keeps following). Recording ends either way and
// the playing goes on. REC in LIVE is the other exit, always WATCH (Browse), as REC always toggles Browse and Edit. Stopped, it starts the transport: the count-in (COUNT-IN, OS-METRO) and LIVE together; with COUNT-IN off, LIVE at once. A transport that does not start leaves the state as it was.
// The action keeps its OS-LIVEREC name (`pattern.rec_free`, a keymap may name it).
bool pattern_rec_free(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.screen != ScreenId::Pattern) {
    return false;
  }
  if (cancel_live_count_in(ctx)) {
    return true;
  }
  const bool edits = pattern_mode_edits(v.mode[screen_index(ScreenId::Pattern)]);
  if (edits && !v.edit.rec_free) {  // LIVE
    (void)pattern_follow_play(ctx.model, v);  // OS-LIVEEXIT: the cursor is on the play line's row now, not a pass behind
    if (v.edit.live_from_step) {
      v.edit.rec_free = true;  // LIVE -> STEP: the same Edit mode, the view stops following
    } else {
      (void)pattern_set_mode(v, PatternMode::Browse);  // LIVE -> WATCH: the view keeps following the play line
    }
    return true;
  }
  if (!v.playing) {
    if (!transport_start_live(ctx)) {
      return false;
    }
  }
  (void)pattern_set_mode(v, PatternMode::Edit);
  v.edit.rec_free = false;
  v.edit.live_from_step = edits;  // not LIVE here, so Edit means STEP (after the mode: leaving Edit clears it)
  (void)pattern_follow_play(ctx.model, v);  // on the playhead at once (a no-op until the position is published)
  return true;
}

// F5 PREVIEW (OS-CFGWIRE; playing the whole selection through the sequencer is OS-PTOOLS): the step under the cursor sounds on the cursor's track for as long as the key is
// down, as the pad does: its note on its own instrument (the last used one when the step has none), the engine's live note with the Preview key's voice (PreviewSink::hold_note). The key's release is
// a raw event: App::run_once ends it (ViewState::step_preview). A step with no note says so. Without an audio path (the simulator) it says that.
bool pattern_preview(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Pattern) {
    return false;
  }
  if (ctx.preview == nullptr) {
    return ctx.view.notify.post(core::Severity::Info, "PREVIEW: NO AUDIO OUTPUT", "preview", ViewState::kMessageMs);
  }
  const PatternSource src(ctx.model, ctx.view.edit.pattern);
  const int track = ctx.view.pattern.track();
  const Step step = src.step(track, ctx.view.pattern.row());
  if (step.note < 1u || step.note > 128u) {
    return ctx.view.notify.post(core::Severity::Info, "PREVIEW: NO NOTE ON THIS STEP", "preview", ViewState::kMessageMs);
  }
  const unsigned instrument = step.instr >= 1u && step.instr <= model::kMaxInstruments ? step.instr : static_cast<unsigned>(ctx.view.instrument);
  ctx.preview->hold_note(true, static_cast<unsigned>(track), instrument, step.note - 1u);
  ctx.view.step_preview = true;
  return false;  // nothing to redraw: the sound is the engine's
}

// EXPORT (the pattern, a selection or the song to a WAV) is Pattern F7 TOOLS / EXPORT PATTERN now (OS-WAVEXP). The action says so; no key is bound to it (Shift + Fx was FILL, which said the same outside
// Edit), so a keymap or a scenario names it.
bool pattern_export(ActionContext& ctx, core::ActionArg) noexcept {
  return ctx.view.screen == ScreenId::Pattern && ctx.view.notify.post(core::Severity::Info, "EXPORT: IN F7 TOOLS", "export", ViewState::kMessageMs);
}

bool pattern_screen_in_view(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Pattern; }

// Row 1 of the TRACKS layer (OS-PADS-A), arg = the pad's column = track 0..11. On Pattern: the cursor goes to that track (a plain move: the block ends, as the arrows). On Perform
// the row is AFFECTED: the track joins or leaves the performance layer (perform.track). A track that is not in the pattern does nothing. On Song (OS-SONGMAP) the cursor goes to that track of the map.
bool pads_track(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 0 || arg >= static_cast<core::ActionArg>(kPadCols)) {
    return false;
  }
  if (ctx.view.screen == ScreenId::Perform) {
    return ctx.view.dj_page() ? dj_group(ctx, arg) : perform_track(ctx, arg);  // (OS-DJ A1: on the DJ page the row is the groups)
  }
  if (ctx.view.screen == ScreenId::Song) {  // OS-SONGMAP: the map's track cursor (OS-SONGPADS: the TRACKS layer is in force on Song); with no map drawn it does nothing
    return ctx.view.song.set_track(arg, song_map_cells(ctx.model, ctx.view));
  }
  if (ctx.view.screen != ScreenId::Pattern || arg >= pattern_dims(ctx.model, ctx.view).tracks) {
    return false;
  }
  return kPatternScreen.track(ctx.model, ctx.view, arg - ctx.view.pattern.track());
}

// Shift + F2..F7 (OS-PADS-A, keys moved one along by OS-PADOCT): the pads' mode that stays, arg = the mode 0..6 (pad_layer.h: F2 NOTES, F3 PIANO, F4 ISO, F5 TRACKS, F6 PATS, F7 VU; STEP has no key since OS-STEPPADOFF, so it is only reached by invoking this with 4). Pattern only.
bool pads_mode(ActionContext& ctx, core::ActionArg arg) noexcept {
  return ctx.view.screen == ScreenId::Pattern && arg >= 0 && ctx.view.padmode.pick(static_cast<unsigned>(arg));
}

// Shift + F2 PADS (OS-UXF-PAT 7): the list of the pads' modes opens over the Pattern screen. The mode stays what it is until a key picks one.
bool pads_picker_open(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Pattern || ctx.view.overlay != OverlayId::None) {
    return false;
  }
  ctx.view.overlay = OverlayId::PadsPicker;
  return true;
}

// The list's F1..F6: the mode of arg (0..6, pad_layer.h) and the list closes, whether or not the mode changed (VU again toggles VU and FADERS, as it did on its Shift key).
bool pads_picker_pick(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::PadsPicker || arg < 0) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  (void)ctx.view.padmode.pick(static_cast<unsigned>(arg));
  return true;
}

// A pad in Browse: it plays the pad's note on the last used instrument, in every column (OS-PADHOLD, new Community OS behaviour, replaces the per-column pad: the Instr column no longer turns a
// pad into the instrument, the instrument is chosen with the Instr hold popup or the encoder on the Instr column). The sound is the engine's (OS-005g): the note is held until the pad is
// released (App::run_once hands the release to PreviewSink::pad_up); until a PreviewSink is attached nothing sounds.
bool pad_preview(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 0 || arg >= static_cast<core::ActionArg>(model::edit::kPadCount) || ctx.view.screen != ScreenId::Pattern) {
    return false;
  }
  const unsigned note = pad_note(ctx.model.project, ctx.view.instrument, ctx.view.edit.pads, static_cast<unsigned>(arg));  // a beat slice instrument: 24 + pad (OS-SLICEFIX)
  ctx.view.edit.last_note = static_cast<std::uint8_t>(note);  // OS-PADS-B: the STEP mode writes the last note played
  if (ctx.preview != nullptr) {
    ctx.preview->pad_down(static_cast<unsigned>(arg), static_cast<unsigned>(ctx.view.pattern.track()), static_cast<unsigned>(ctx.view.instrument), note);
  }
  return false;
}

// ---- the transport ---------------------------------------------------------------------------------------------------------

// Play (the Play / Stop button): stopped, the pattern being edited plays from its first step (Pattern mode, as
// the pattern); playing, it stops. On the Song screen it is the song, from the selected row; F2 PLAY SNG is the same. The sound and the
// timeline are the engine's (a TransportSink: the sequencer's command ring on the device); the badge follows the last request that went through.
// The row is the one of the list the Song screen shows (SongSource: the project's order list, the demo's with no project), kept inside it; a song with
// no positions has no row to start from, so Play says so and starts nothing (OS-005f; a Stop still goes through).
// Shift + Play is transport_play_cursor below (OS-PLAYCUR); the Song screen's F1 PLAY PAT is a later slice.
bool transport_play(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.transport == nullptr) {
    return false;
  }
  bool ok = false;
  if (ctx.view.playing) {
    ok = ctx.transport->stop();
    if (ok) {
      // OS-SONG3 S4: Stop ends a PREVIEW audition (F1 in a TIMELINE lane): the saved solo set comes back, exactly once.
      (void)song_preview_end(ctx.session != nullptr ? ctx.session->state_editor() : nullptr, ctx.view, ctx.params);
    }
  } else if (ctx.view.screen == ScreenId::Song) {
    const int count = SongSource(ctx.model).count();
    if (count <= 0) {
      return ctx.view.notify.post(core::Severity::Info, "SONG IS EMPTY", "play", ViewState::kMessageMs);
    }
    const int pos = ctx.view.song.pos();
    ok = ctx.transport->play_song(static_cast<unsigned>(pos < 0 ? 0 : (pos >= count ? count - 1 : pos)));
  } else {
    ok = ctx.transport->play(ctx.view.edit.pattern);
  }
  if (!ok) {
    return false;
  }
  ctx.view.playing = !ctx.view.playing;
  if (!ctx.view.playing && pattern_mode_edits(ctx.view.mode[screen_index(ScreenId::Pattern)]) && !ctx.view.edit.rec_free) {
    (void)pattern_set_mode(ctx.view, PatternMode::Browse);  // OS-RECMODE2: LIVE ends with the transport (STEP stays); the take closes (rec_take_sync)
  }
  return true;
}

// Shift + Play on Pattern (OS-PLAYCUR, new Community OS behaviour): stopped, the pattern being edited starts at the cursor's row; playing, the play line jumps to the cursor's row at the next step
// boundary and goes on from there (the transport, the take and the view's mode are untouched). Never a count-in, as Play. Another pattern than the playing one: refused.
// With a block selected it is the Focus Loop (OS-UXF row 2, new Community OS behaviour): the pattern plays from the block's first row and loops at its last (the sequencer's row loop, TransportSink::loop_rows; the
// strip says LOOP nn-nn). While the loop runs, Shift + Play again stops, as Play does (the sequencer clears the range with the run). Playing but not looping, the line jumps to the block's first row and the loop begins there.
bool transport_play_cursor(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (ctx.transport == nullptr) {
    return false;
  }
  if (v.screen != ScreenId::Pattern) {
    return transport_play(ctx, arg);  // (bound on Pattern only; a keymap that names it elsewhere gets Play)
  }
  const bool block = v.edit.selection.active;
  if (v.playing && v.play.row_looping() && v.play.pattern == v.edit.pattern) {
    return transport_play(ctx, arg);  // the loop runs: the key that began it ends it
  }
  unsigned row = static_cast<unsigned>(v.pattern.row() < 0 ? 0 : v.pattern.row());
  unsigned last = row;
  if (block) {
    const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
    row = static_cast<unsigned>(b.row0 < 0 ? 0 : b.row0);
    last = static_cast<unsigned>(b.row1 < b.row0 ? b.row0 : b.row1);
  }
  if (v.playing) {
    if (v.play.pattern != v.edit.pattern) {  // the line is in another pattern (a free cursor in STEP, a queued pattern): its row is not this pattern's
      return pe::say(ctx, core::Severity::Info, "THE PLAYHEAD IS IN ANOTHER PATTERN");
    }
    if (!ctx.transport->locate_row(row)) {
      return false;
    }
    // The range is set at once and the jump lands at the next step boundary, before the loop's own wrap can act on the row the line is on now (Sequencer::end_step).
    return !block || ctx.transport->loop_rows(row, last);
  }
  if (!ctx.transport->play_from(v.edit.pattern, row)) {
    return false;
  }
  v.playing = true;
  if (block) {
    if (!ctx.transport->loop_rows(row, last)) {  // behind the start on the one ring, so it follows the start's own clearing of a range
      return pe::say(ctx, core::Severity::Info, "LOOP NOT SET (BUSY)");  // (ring full: the pattern plays on without the loop)
    }
  }
  return true;
}

bool transport_available(const ActionContext& ctx) noexcept { return ctx.transport != nullptr; }

// ---- the selection ------------------------------------------------------------------------------------------------------

// Shift + an arrow, in every mode (the Select mode of OS-009 is gone: OS-LIVEREC). The block
// is anchored where the first arrow was pressed and its end follows the cursor; Shift + Up at step 0 selects the cursor's track, and
// again every track (the column and all). The argument is 10 * dx + dy. OS-AUDIT-56 (new Community OS behaviour): the block also keeps the columns from where it began to the cursor's column, as long as it
// stays in one track; Shift + Left / Right move a column at a time inside the anchor track; crossing into another track makes the block whole steps, and from there they step by tracks (Selection::block).
bool select_extend(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int dx = arg >= 5 ? 1 : (arg <= -5 ? -1 : 0);
  const int dy = arg - 10 * dx;
  if (!pe::select_enabled(ctx) || (dx == 0) == (dy == 0) || dy < -1 || dy > 1) {
    return false;
  }
  ViewState& v = ctx.view;
  Selection& sel = v.edit.selection;
  PatternView& view = v.pattern;
  const PatternView::Dims d = pattern_dims(ctx.model, v);
  // OS-SELSHIFT: a downward quarter jump leaves the block's end one row above the cursor; while that holds (`end_lags`), Shift + Down keeps the lag so the block grows by one row, not two.
  const bool lag = sel.end_lags && sel.extending && !sel.column && !sel.follow && sel.end_row == view.row() - 1;  // (a block made while following is dropped below once the cursor is stopped: no lag to carry)
  sel.end_lags = false;
  // OS-AUDIT-56: Left / Right grow the block a column at a time inside the anchor track; once the block spans two tracks (whole steps), and for the whole-column block, they step by whole tracks as before.
  const auto sideways = [&] {
    const bool by_track = sel.column || view.track() != sel.start_track;
    (void)(by_track ? view.move_track(dx, d) : (dx > 0 ? view.move_right(d) : view.move_left(d)));
  };
  if (pe::select_follows(ctx)) {  // OS-BUG-WATCHSEL: the playhead owns the cursor's row, so the arrows set the block's own end (the block is anchored where the first one was pressed)
    if (!sel.extending || !sel.follow) {
      sel.start_row = sel.end_row = static_cast<std::int16_t>(view.row());
      sel.start_track = sel.end_track = static_cast<std::int8_t>(view.track());
      sel.start_col = sel.end_col = static_cast<std::int8_t>(view.column());
      sel.extending = true;
      sel.column = false;
    }
    if (dx != 0) {
      sideways();
      sel.end_track = static_cast<std::int8_t>(view.track());
      if (!sel.column) {  // (the whole-column block keeps every column)
        sel.end_col = static_cast<std::int8_t>(view.column());
      }
    } else if (dy < 0 && (sel.column || sel.end_row == 0)) {
      (void)select_column_all(ctx, 0);  // (Shift + Up at step 0 selects the column, as stopped)
    } else {
      if (sel.column) {
        sel.start_col = 0;  // (the one-column block of OS-SELCOL becomes whole steps too)
        sel.end_col = Selection::kLastColumn;
      }
      sel.column = false;  // (Shift + Down from a column block: it becomes a plain block, as stopped)
      sel.end_row = static_cast<std::int16_t>(pe::clampi(sel.end_row + dy, 0, d.steps - 1));
    }
    if (sel.column) {
      sel.end_row = static_cast<std::int16_t>(d.steps - 1);
    }
    sel.active = true;
    sel.follow = true;
    return true;
  }
  if (sel.follow) {  // a block made while following does not carry over to a stopped cursor: it starts afresh
    sel.clear();
  }
  const auto start_here = [&] {
    sel.start_row = static_cast<std::int16_t>(view.row());
    sel.start_track = static_cast<std::int8_t>(view.track());
    sel.start_col = static_cast<std::int8_t>(view.column());
    sel.extending = true;
  };
  const auto end_here = [&] {
    sel.end_row = static_cast<std::int16_t>(view.row());
    sel.end_track = static_cast<std::int8_t>(view.track());
    sel.end_col = static_cast<std::int8_t>(view.column());
    sel.active = true;
  };
  if (dx != 0) {
    if (!sel.extending) {
      start_here();
    }
    sideways();
    if (sel.column) {  // the whole-column selection grows by tracks
      sel.end_row = static_cast<std::int16_t>(d.steps - 1);
      sel.end_track = static_cast<std::int8_t>(view.track());
      sel.active = true;
      sel.extending = true;
    } else {
      end_here();
    }
  } else if (dy < 0) {
    if (view.row() == 0) {
      (void)select_column_all(ctx, 0);
    }
    if (!sel.extending) {
      start_here();
    }
    (void)view.jump_row(view.row() - 1, d);
    if (!sel.column) {
      end_here();
    }
  } else {
    if (!sel.extending) {
      start_here();
    }
    (void)view.jump_row(view.row() + 1, d);
    const bool was_column = sel.column;
    end_here();
    if (lag) {
      sel.end_row = static_cast<std::int16_t>(view.row() - 1);  // (the cursor moved on by one, so did the end: the lag stays)
      sel.end_lags = true;
    }
    if (was_column) {  // (a whole-column block that Shift + Down turns into a plain block keeps every column; so does the one-column block of OS-SELCOL)
      sel.start_col = 0;
      sel.end_col = Selection::kLastColumn;
    }
    sel.column = false;
  }
  return true;
}

// The column selection, one more step at each press: the cursor's column of the cursor's track from step 0 to the end (OS-SELCOL, only on the Instr, Vol, Fx1 and Fx2 columns; on Note the first
// press is the track), then the whole track, then every track.
bool select_column_all(ActionContext& ctx, core::ActionArg) noexcept {
  if (!pe::select_enabled(ctx)) {
    return false;
  }
  ViewState& v = ctx.view;
  Selection& sel = v.edit.selection;
  const PatternView::Dims d = pattern_dims(ctx.model, v);
  sel.follow = pe::select_follows(ctx);  // (made while following: the follow keeps it, OS-BUG-WATCHSEL)
  sel.start_row = 0;
  sel.end_row = static_cast<std::int16_t>(d.steps - 1);
  const bool fresh = !sel.column;
  const bool own_column = sel.column_only() && sel.start_track == sel.end_track;  // (stage 1 of OS-SELCOL, not yet widened by Shift + Right)
  // OS-AUDIT-56: the track and every-track steps are whole steps, also after Shift is let go and `column` is reset; stage 1 keeps its column range for the same reason.
  const int col = fresh ? static_cast<int>(v.pattern.column()) : 0;
  sel.start_col = sel.end_col = static_cast<std::int8_t>(col);
  if (col == 0) {
    sel.end_col = Selection::kLastColumn;
  }
  if (fresh || own_column) {
    sel.column = true;
    sel.start_track = sel.end_track = static_cast<std::int8_t>(fresh ? v.pattern.track() : sel.start_track);
  } else {
    sel.start_track = 0;
    sel.end_track = static_cast<std::int8_t>(d.tracks - 1);
  }
  sel.active = true;
  sel.extending = true;
  return true;
}

// Shift + Note, Instr, Vol, Fx on Pattern (OS-SELSHIFT, new Community OS behaviour): the quarter jump of OS-QJUMP (arg 0..3), and in Edit it also selects the rows it crossed, from the lower row up to one row above the
// higher one, in the cursor's track and column like a Shift + arrow range (an active block is replaced; the same row selects nothing). Upward the block ends on the cursor, so a Shift + Up goes on from there;
// downward the cursor is one row past the block's end, so a Shift + Down keeps that lag (Selection::end_lags) and grows the block by one row. Outside Edit it is the jump alone.
bool select_jump(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  const int from = v.pattern.row();
  if (!cursor_jump_fraction(ctx, arg)) {
    return false;
  }
  const int to = v.pattern.row();
  if (!pe::edit_enabled(ctx) || to == from) {
    return true;
  }
  Selection& sel = v.edit.selection;
  sel.follow = pe::select_follows(ctx);  // (as the arrows: a block made while the cursor rides the playhead is kept by the follow)
  sel.start_row = static_cast<std::int16_t>(to < from ? from - 1 : from);
  sel.end_row = static_cast<std::int16_t>(to < from ? to : to - 1);
  sel.start_track = sel.end_track = static_cast<std::int8_t>(v.pattern.track());
  sel.start_col = sel.end_col = static_cast<std::int8_t>(v.pattern.column());
  sel.column = false;
  sel.extending = true;
  sel.active = true;
  sel.end_lags = to > from;  // (the end is one row above the cursor on a downward jump only)
  return true;
}

bool select_clear(ActionContext& ctx, core::ActionArg) noexcept {
  const bool had = ctx.view.edit.selection.active || ctx.view.edit.selection.extending;
  ctx.view.edit.selection.clear();
  return had;
}

}  // namespace o3os::app
