// SPDX-License-Identifier: GPL-3.0-or-later
// The edit core of the pattern (OS-009 P1a.1): the actions that change the step under the cursor or the selected block. Each is a thin
// wrapper that picks a rule of model/step_edit.h (the edit rules on the model's encoding) and applies it with pe::edit_selection:
// one undo unit per action, the pattern created in that unit when it does not exist yet, steps past their track's end refused.
#include "pattern_edit_util.h"
#include "param_access.h"

#include <o3os/app/pad_note.h>
#include <o3os/app/text_format.h>
#include <o3os/app/track_state.h>
#include <o3os/model/params.h>

namespace o3os::app {

namespace {

using model::edit::Last;
using model::edit::Rule;

constexpr int kMaxOctaves = 10;
// OS-SHIFTENC: Shift + encoder in the Instr, Vol and FX value columns; the detents of one event are clamped, then multiplied (the model clamps the result to the column's range).
constexpr int kMaxCoarse = 16;
constexpr int kInstrCoarse = 8;  // places in the instrument order
constexpr int kVolCoarse = 10;
constexpr int kFxCoarse = 16;    // one nibble

bool can_edit(const ActionContext& ctx) noexcept { return ctx.edit != nullptr && pe::edit_enabled(ctx); }

// Mutes or solos a track at the SOUND level (OS-MUTE, new Community OS behaviour): the Track parameter Mute / Solo, the Mixer's own, written through the normal parameter path, so it is an
// undo unit, it is the project's (the TrackSlot flag, which the native writer and the OG exporter keep carrying), and the graph's strip is told (write_param sends the ParamChange).
// The sequencer no longer skips the track: it keeps firing notes and FX, so an unmute is heard at once, mid-note.
bool track_arg_ok(const ActionContext& ctx, core::ActionArg arg) noexcept {
  return ctx.edit != nullptr && (ctx.view.screen == ScreenId::Pattern || ctx.view.screen == ScreenId::Perform || ctx.view.screen == ScreenId::Song) && arg >= 0 &&
         static_cast<std::uint32_t>(arg) < ctx.edit->project().active_tracks() && static_cast<unsigned>(arg) < kMixTracks;
}

bool toggle_track_param(ActionContext& ctx, core::ActionArg arg, unsigned field) noexcept {
  if (!track_arg_ok(ctx, arg)) {
    return false;
  }
  // OS-SONG3 S4: a hand mute / solo while a PREVIEW audition runs ends the audition first (the saved solo set comes back), so this toggle and its undo unit act on the
  // player's state, never on the audition's mask.
  (void)song_preview_end(ctx.session != nullptr ? ctx.session->state_editor() : nullptr, ctx.view, ctx.params);
  const core::ParamId id = core::param_id(core::ParamKind::Track, static_cast<unsigned>(arg), field);
  float now = 0.0f;
  if (!read_param(ctx.model, id, now)) {
    return false;
  }
  return write_param(ctx, id, now != 0.0f ? 0.0f : 1.0f) == WriteResult::Changed;
}

// The thunk context of the FX edits (OS-FLASHDIET T1): the FX column under the cursor and the action's argument.
struct FxArg {
  int column;
  core::ActionArg arg;
};

}  // namespace

// ---- notes ------------------------------------------------------------------------------------------------------------

bool step_note(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx) || arg < 0 || !model::edit::is_enterable_note(static_cast<unsigned>(arg))) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last& l, void* p) { pe::write_note(s, static_cast<unsigned>(*static_cast<const core::ActionArg*>(p)), r, l); }, &arg);
  return pe::finish_entry(ctx, e, true);
}

// A pad enters the note the pad board gives it: then the cursor moves on. The pad also SOUNDS (OS-BUG-STEPSND: it wrote the note and stayed silent when the pattern
// was stopped): the live note on the selected track, held until the pad's release (App::run_once ends it); the sequencer's next note on that track cuts it (shared voice).
bool step_note_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 0 || arg >= static_cast<core::ActionArg>(model::edit::kPadCount)) {
    return false;
  }
  if (rec_active(ctx)) {
    return rec_pad(ctx, static_cast<unsigned>(arg));  // Rec (OS-FU2): the playing pattern takes the note at the playhead, not at the cursor (it sounds the pad itself)
  }
  // Edit, the cursor on the Instr column: the pad picks instrument pad + 1 (OG habit, OS-AUDIT E-PAT-12; reverses OS-PADHOLD for this column): one undo unit, no note from the pad (an empty
  // step takes the default note as the Instr popup's pick does). The pad sounds the picked instrument, held until its release.
  if (ctx.view.pattern.column() == Column::Instr && can_edit(ctx) && !rec_counting_in(ctx)) {
    const bool changed = step_instrument(ctx, arg + 1);
    if (ctx.preview != nullptr) {  // the picked instrument itself: with a block selected the edit leaves the strip's instrument alone
      ctx.preview->pad_down(static_cast<unsigned>(arg), static_cast<unsigned>(ctx.view.pattern.track()), static_cast<unsigned>(arg) + 1u,
                            pad_note(ctx.model.project, arg + 1, ctx.view.edit.pads, static_cast<unsigned>(arg)));
    }
    return changed;
  }
  // The note of the pad for the current instrument: the board's, or (a beat slice instrument) 24 + pad, the note that names the pad's slice (OS-SLICEFIX).
  const unsigned note = pad_note(ctx.model.project, ctx.view.instrument, ctx.view.edit.pads, static_cast<unsigned>(arg));
  // Sound first, on the track the cursor is on: the write below moves the cursor.
  if (ctx.preview != nullptr) {
    ctx.preview->pad_down(static_cast<unsigned>(arg), static_cast<unsigned>(ctx.view.pattern.track()), static_cast<unsigned>(ctx.view.instrument), note);
  }
  if (rec_counting_in(ctx)) {
    return false;  // the count-in (OS-METRO): the pad sounds, nothing is written yet
  }
  ctx.view.edit.last_note = static_cast<std::uint8_t>(note);  // OS-PADS-B: the STEP mode writes the last note played
  return step_note(ctx, static_cast<core::ActionArg>(note + 1u));
}

// A note from the MIDI keyboard in Edit (OS-FU1; the Edit-mode branch, not the Rec one): the
// note goes into the cursor step (an empty step also takes the last used instrument) and the cursor stays where it is, stopped or playing; with a block of more than one
// step selected nothing is written (the note only plays). One undo unit. The sound is the live note's (PreviewSink::midi_note), not this action's. arg = MIDI note 0..127.
bool step_midi_note(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx) || arg < 0 || arg > 127) {
    return false;
  }
  if (rec_active(ctx)) {
    return rec_note(ctx, static_cast<unsigned>(arg), ctx.view.edit.last_velocity);  // Rec (OS-FU2): at the playhead (the sound is the live note's, App::poll_midi_notes); its velocity is REC VELOCITY's (OS-RECMICRO)
  }
  if (rec_counting_in(ctx)) {
    return false;  // the count-in (OS-METRO): the key sounds, nothing is written yet
  }
  const Selection::Block b = ctx.view.edit.selection.block(ctx.view.pattern.row(), ctx.view.pattern.track());
  if (b.row0 != b.row1 || b.track0 != b.track1) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx,
      [](model::Step& s, const Rule& r, Last& l, void* p) { pe::write_note(s, static_cast<unsigned>(*static_cast<const core::ActionArg*>(p)) + 1u, r, l); }, &arg);
  return pe::finish_entry(ctx, e, false);
}

bool step_note_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx)) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last& l, void* p) { model::edit::note_delta(s, *static_cast<const core::ActionArg*>(p), r, l); }, &arg);
  return pe::finish_entry(ctx, e, false);
}

// Shift + encoder (PAR-052): the same rule an octave at a time. The clamp is per note (Q-P1-4).
bool step_note_octave(ActionContext& ctx, core::ActionArg arg) noexcept {
  return step_note_delta(ctx, pe::clampi(arg, -kMaxOctaves, kMaxOctaves) * 12);
}

// ---- instruments ------------------------------------------------------------------------------------------------------

bool step_instrument(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx) || arg < 1 || arg > static_cast<core::ActionArg>(model::edit::kInstrumentSlots)) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last& l, void* p) { model::edit::instrument_set(s, static_cast<unsigned>(*static_cast<const core::ActionArg*>(p)), r, l); }, &arg);
  return pe::finish_entry(ctx, e, true);
}

bool step_instrument_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx)) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last& l, void* p) { model::edit::instrument_delta(s, *static_cast<const core::ActionArg*>(p), r, l); }, &arg, false, true);
  return pe::finish_entry(ctx, e, false);
}

// OS-SHIFTENC: Shift + encoder, 8 places of the instrument order a detent (the walk of one fine call stays 5; this one lifts it to 64, the order's length).
bool step_instrument_coarse(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx)) {
    return false;
  }
  int d = pe::clampi(arg, -kMaxCoarse, kMaxCoarse) * kInstrCoarse; // (the thunk context: non-const, edit_selection takes void*)
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last& l, void* p) { model::edit::instrument_delta(s, *static_cast<const int*>(p), r, l, 64); }, &d, false, true);
  return pe::finish_entry(ctx, e, false);
}

// ---- volume (OS-new) ----------------------------------------------------------------------------------------------------

bool step_vol(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx) || arg < 0 || arg > model::kVolumeMax) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last&, void* p) { model::edit::volume_set(s, static_cast<unsigned>(*static_cast<const core::ActionArg*>(p)), r); }, &arg);
  return pe::finish_entry(ctx, e, false);
}

bool step_vol_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx) || arg < 0 || arg >= static_cast<core::ActionArg>(model::edit::kPadCount)) {
    return false;
  }
  int value = model::edit::map_int(arg, 0, 47, 0, model::kVolumeMax); // (the thunk context: non-const)
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last&, void* p) { model::edit::volume_set(s, static_cast<unsigned>(*static_cast<const int*>(p)), r); }, &value);
  return pe::finish_entry(ctx, e, true);
}

bool step_vol_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx)) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule& r, Last&, void* p) { model::edit::volume_delta(s, *static_cast<const core::ActionArg*>(p), r); }, &arg, false, true);
  return pe::finish_entry(ctx, e, false);
}

// OS-SHIFTENC: Shift + encoder, 10 a detent.
bool step_vol_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return step_vol_delta(ctx, pe::clampi(arg, -kMaxCoarse, kMaxCoarse) * kVolCoarse); }

// ---- FX: on the FX column under the cursor ------------------------------------------------------------------------------

bool step_fx_type(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int fx = pe::fx_column(ctx.view);
  if (!can_edit(ctx) || fx < 0 || arg < 0 || arg > 255) {
    return false;
  }
  FxArg fx_arg{fx, arg}; // the thunk context (non-const: edit_selection takes void*)
  const pe::Edited e = pe::edit_selection(
      ctx,
      [](model::Step& s, const Rule&, Last&, void* p) {
        const FxArg& c = *static_cast<const FxArg*>(p);
        model::edit::fx_type_set(s, static_cast<unsigned>(c.column), static_cast<unsigned>(c.arg));
      },
      &fx_arg);
  return pe::finish_entry(ctx, e, false);
}

bool step_fx_type_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int fx = pe::fx_column(ctx.view);
  if (!can_edit(ctx) || fx < 0) {
    return false;
  }
  FxArg fx_arg{fx, arg}; // the thunk context (non-const: edit_selection takes void*)
  const pe::Edited e = pe::edit_selection(
      ctx,
      [](model::Step& s, const Rule&, Last&, void* p) {
        const FxArg& c = *static_cast<const FxArg*>(p);
        model::edit::fx_type_delta(s, static_cast<unsigned>(c.column), c.arg);
      },
      &fx_arg);
  return pe::finish_entry(ctx, e, false);
}

bool step_fx_value(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int fx = pe::fx_column(ctx.view);
  if (!can_edit(ctx) || fx < 0) {
    return false;
  }
  FxArg fx_arg{fx, arg}; // the thunk context (non-const: edit_selection takes void*)
  const pe::Edited e = pe::edit_selection(
      ctx,
      [](model::Step& s, const Rule& r, Last& l, void* p) {
        const FxArg& c = *static_cast<const FxArg*>(p);
        model::edit::fx_value_set(s, static_cast<unsigned>(c.column), c.arg, r, l);
      },
      &fx_arg);
  return pe::finish_entry(ctx, e, false);
}

bool step_fx_value_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int fx = pe::fx_column(ctx.view);
  if (!can_edit(ctx) || fx < 0) {
    return false;
  }
  FxArg fx_arg{fx, arg}; // the thunk context (non-const: edit_selection takes void*)
  const pe::Edited e = pe::edit_selection(
      ctx,
      [](model::Step& s, const Rule& r, Last& l, void* p) {
        const FxArg& c = *static_cast<const FxArg*>(p);
        model::edit::fx_value_delta(s, static_cast<unsigned>(c.column), c.arg, r, l);
      },
      &fx_arg, false, true);
  return pe::finish_entry(ctx, e, false);
}

// OS-SHIFTENC: Shift + encoder, 16 a detent (one nibble of the value), clamped to the type's range.
bool step_fx_value_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return step_fx_value_delta(ctx, pe::clampi(arg, -kMaxCoarse, kMaxCoarse) * kFxCoarse); }

bool step_fx_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int fx = pe::fx_column(ctx.view);
  if (!can_edit(ctx) || fx < 0 || arg < 0 || arg >= static_cast<core::ActionArg>(model::edit::kPadCount)) {
    return false;
  }
  FxArg fx_arg{fx, arg}; // the thunk context (non-const: edit_selection takes void*)
  const pe::Edited e = pe::edit_selection(
      ctx,
      [](model::Step& s, const Rule& r, Last& l, void* p) {
        const FxArg& c = *static_cast<const FxArg*>(p);
        model::edit::fx_pad(s, static_cast<unsigned>(c.column), static_cast<unsigned>(c.arg), r, l);
      },
      &fx_arg);
  return pe::finish_entry(ctx, e, true);
}

// ---- clear, insert, pull ---------------------------------------------------------------------------------------------

// Delete. `arg` is the elements (kEl*); 0 takes them from the block's columns (OS-AUDIT-56: a block of one track's columns clears those, one of whole steps clears every
// element) or, with no block, from the cursor's column: an FX column clears that FX, the Vol column the volume, Note and Instr the whole step. Clearing a block clears the highlighted columns of its steps (a Shift+Down block in the Note column: Note and Instr only), whatever the
// pattern's length (PAR-073: a track of 128 steps clears whole and keeps its length, PAR-093: no instrument is left behind). Then the
// cursor moves on by the edit step, and the block collapses after every delete. NEW COMMUNITY OS BEHAVIOUR (no parity claimed, OS-DELREPEAT): the cursor stops at the last row
// instead of wrapping to the first, wherever the key was pressed or held from, so a held Delete never clears from the top.
bool step_clear(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!can_edit(ctx) || arg < 0) {
    return false;
  }
  unsigned elements = static_cast<unsigned>(arg) & model::edit::kElMask;
  const Selection& sel = ctx.view.edit.selection;
  if (arg == 0 && sel.active) {  // OS-AUDIT-56: a block clears the columns it covers (all of them for whole steps)
    elements = pe::column_elements(sel.block(ctx.view.pattern.row(), ctx.view.pattern.track()));
  } else if (arg == 0) {
    switch (ctx.view.pattern.column()) {
      case Column::Fx1:
        elements = model::edit::kElFx1;
        break;
      case Column::Fx2:
        elements = model::edit::kElFx2;
        break;
      case Column::Vol:
        elements = model::edit::kElVol;
        break;
      case Column::Note:
      case Column::Instr:
        elements = model::edit::kElAll;
        break;
    }
  }
  if (elements == 0) {
    return false;
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const Rule&, Last&, void* p) { model::edit::clear(s, *static_cast<const unsigned*>(p)); }, &elements);
  if (e.refused > 0 && !e.changed && !e.failed) {
    return pe::say(ctx, core::Severity::Info, "PAST TRACK END");
  }
  pe::move_cursor_by_step(ctx, 0, false);  // always, with a range too: it also ends the selection; a clear never wraps (a hold may start on the last row: OS-DELREPEAT)
  return true;
}

// Insert: every selected track shifts down from the block's first row (the cursor step with no block; OS-QAFIX-17) through step 127 and that step is cleared; what is
// pushed past the track's length is kept (the model keeps steps past the length). A track that does not reach that row is left alone. The cursor stays.
bool step_insert(ActionContext& ctx, core::ActionArg) noexcept {
  if (!can_edit(ctx)) {
    return false;
  }
  ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  bool exists = src.exists();
  bool changed = false;
  int refused = 0;
  for (int t = b.track0; t <= b.track1 && t < src.tracks(); ++t) {
    const int first = b.row0;
    if (first >= src.track_length(t)) {
      ++refused;
      continue;
    }
    const int n = static_cast<int>(model::kMaxPatternLength) - first;  // steps first..127
    model::Step buf[model::kMaxPatternLength];
    bool differs = false;
    for (int k = 0; k < n; ++k) {  // (OS-AUDIT-56: only the block's columns move; a whole-step block moves whole steps)
      const model::Step now = src.step(t, first + k);
      buf[k] = pe::with_columns(now, k == 0 ? model::kEmptyStep : src.step(t, first + k - 1), b);
      differs = differs || std::memcmp(&buf[k], &now, sizeof now) != 0;
    }
    if (!differs) {
      continue;
    }
    if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
      return false;
    }
    changed = ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(first), buf,
                                  static_cast<std::uint32_t>(n)) == model::Status::Ok || changed;
  }
  if (refused > 0 && !changed) {
    return pe::say(ctx, core::Severity::Info, "PAST TRACK END");
  }
  return changed;
}

// Pull (Shift + Delete; NEW COMMUNITY OS BEHAVIOUR, no parity claimed, OS-DELPULL): the exact inverse of Insert. The step at the block's first row (the cursor step with no block; OS-QAFIX-17) is deleted and every selected track is pulled up from there through
// step 127, whose step is cleared; what lies past the track's length is pulled too (the model keeps it). After a pull that changed something the cursor goes down by the JUMP like Delete
// (OS-DELJUMP: no wrap, the block ends, JUMP 0 stays); a refused pull or one that changed nothing does not move. Insert then Pull gives the pattern back (the step Insert
// pushed past 127, if it had a note, is gone). It replaces the backspace (the steps before the cursor pulled up, the cursor following), which is not an inverse of anything.
bool step_pull(ActionContext& ctx, core::ActionArg) noexcept {
  if (!can_edit(ctx)) {
    return false;
  }
  ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  bool exists = src.exists();
  bool changed = false;
  int refused = 0;
  for (int t = b.track0; t <= b.track1 && t < src.tracks(); ++t) {
    const int first = b.row0;
    if (first >= src.track_length(t)) {
      ++refused;
      continue;
    }
    const int n = static_cast<int>(model::kMaxPatternLength) - first;  // steps first..127
    model::Step buf[model::kMaxPatternLength];
    bool differs = false;
    for (int k = 0; k < n; ++k) {  // (OS-AUDIT-56: only the block's columns move)
      const model::Step now = src.step(t, first + k);
      buf[k] = pe::with_columns(now, k + 1 < n ? src.step(t, first + k + 1) : model::kEmptyStep, b);
      differs = differs || std::memcmp(&buf[k], &now, sizeof now) != 0;
    }
    if (!differs) {
      continue;
    }
    if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
      return false;
    }
    changed = ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(first), buf,
                                  static_cast<std::uint32_t>(n)) == model::Status::Ok || changed;
  }
  if (refused > 0 && !changed) {
    return pe::say(ctx, core::Severity::Info, "PAST TRACK END");
  }
  if (changed) {
    pe::move_cursor_by_step(ctx, 0, false);  // OS-DELJUMP: after a pull the cursor goes down by the JUMP like Delete (no wrap; it ends the selection; JUMP 0: stays)
  }
  return changed;
}

// ---- copy and paste (OS-PATCOPY) -----------------------------------------------------------------------------------------------

// Copy (NEW COMMUNITY OS BEHAVIOUR, no parity claimed): the selected block, or the step under the cursor, into the pattern clipboard (pattern_clip.h), every track of the block
// its own column; a column block (OS-AUDIT-56) marks its columns on the clip. In any mode of the Pattern screen (it changes nothing); a later copy replaces the clip. There is no cut: Copy, then Delete.
bool block_copy(ActionContext& ctx, core::ActionArg) noexcept {
  const ViewState& v = ctx.view;
  if (v.screen != ScreenId::Pattern || ctx.clip == nullptr) {
    return false;
  }
  const PatternSource src(ctx.model, v.edit.pattern);
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  const int last_track = b.track1 < src.tracks() - 1 ? b.track1 : src.tracks() - 1;
  const int last_row = b.row1 < static_cast<int>(model::kMaxPatternLength) - 1 ? b.row1 : static_cast<int>(model::kMaxPatternLength) - 1;
  if (b.track0 > last_track || b.row0 > last_row || !ctx.clip->begin(last_track - b.track0 + 1, last_row - b.row0 + 1)) {
    return pe::say(ctx, core::Severity::Info, "NOTHING TO COPY");
  }
  ctx.clip->set_columns(b.col0, b.col1);  // (OS-AUDIT-56: the columns of a column block; all five otherwise)
  for (int i = 0; i < ctx.clip->tracks(); ++i) {
    model::Step* col = ctx.clip->column(i);
    for (int r = 0; r < ctx.clip->rows(); ++r) {
      col[r] = src.step(b.track0 + i, b.row0 + r);
    }
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("COPIED ");
  append_uint(text, static_cast<unsigned>(ctx.clip->tracks() * ctx.clip->rows()));
  text.append(ctx.clip->tracks() * ctx.clip->rows() == 1 ? " STEP" : " STEPS");
  return pe::say(ctx, core::Severity::Info, text.c_str());
}

// Shift + Copy (NEW COMMUNITY OS BEHAVIOUR): the clip is written with its top-left step on the cursor (the cursor's track and row, whatever the selection), whole steps over whatever is there
// (a clip of a column block, OS-AUDIT-56, writes its columns only, in the same columns: the cursor's column does not matter).
// Clipped at each track's end and at the last track (what does not fit is dropped, the pattern's lengths never change). One undo unit, in Edit (STEP or LIVE), also while the pattern
// plays. The cursor and the block stay.
bool block_paste(ActionContext& ctx, core::ActionArg) noexcept {
  if (!can_edit(ctx)) {
    return false;
  }
  if (ctx.clip == nullptr || ctx.clip->empty()) {
    return pe::say(ctx, core::Severity::Info, "NOTHING TO PASTE");
  }
  ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  const int row = v.pattern.row();
  bool exists = src.exists();
  bool changed = false;
  int refused = 0;
  for (int i = 0; i < ctx.clip->tracks(); ++i) {
    const int t = v.pattern.track() + i;
    if (t >= src.tracks()) {
      break;  // past the last track
    }
    const int len = src.track_length(t);
    if (row >= len) {
      ++refused;
      continue;
    }
    const int n = ctx.clip->rows() < len - row ? ctx.clip->rows() : len - row;  // clipped at this track's end
    const model::Step* col = ctx.clip->column(i);
    model::Step merged[model::kMaxPatternLength];  // OS-AUDIT-56: a column clip writes its columns only, the target's other columns stay
    if (!ctx.clip->whole_steps()) {
      for (int k = 0; k < n; ++k) {
        merged[k] = src.step(t, row + k);
        pe::copy_columns(merged[k], col[k], ctx.clip->col0(), ctx.clip->col1());
      }
      col = merged;
    }
    bool differs = false;
    for (int k = 0; k < n && !differs; ++k) {
      const model::Step now = src.step(t, row + k);
      differs = std::memcmp(&col[k], &now, sizeof now) != 0;
    }
    if (!differs) {
      continue;
    }
    if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
      return false;
    }
    changed = ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(row), col, static_cast<std::uint32_t>(n)) == model::Status::Ok || changed;
  }
  if (refused > 0 && !changed) {
    return pe::say(ctx, core::Severity::Info, "PAST TRACK END");
  }
  return changed;
}

// ---- invert and fill (OS-CFGWIRE) -------------------------------------------------------------------------------------------

// INVERT (F6): the steps of the block are put in the reverse order, each track on its own, whole
// steps (note, instrument, volume and both FX), or only the columns of a column block (OS-AUDIT-56). A block of one step has
// nothing to turn round: the key says so, and outside Edit it says TURN EDIT ON (OS-QAFIX-12). Rows past a track's own length are left out (D7). One undo unit.
bool pattern_invert(ActionContext& ctx, core::ActionArg) noexcept {
  if (!can_edit(ctx)) {
    return pe::say(ctx, core::Severity::Info, ctx.edit == nullptr ? "NO PROJECT" : pe::kEditOff);
  }
  ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  if (b.row0 == b.row1 && b.track0 == b.track1) {
    return pe::say(ctx, core::Severity::Info, "SELECT A BLOCK TO INVERT");
  }
  bool exists = src.exists();
  bool changed = false;
  int refused = 0;
  for (int t = b.track0; t <= b.track1 && t < src.tracks(); ++t) {
    const int r1 = b.row1 < src.track_length(t) - 1 ? b.row1 : src.track_length(t) - 1;
    if (b.row0 > r1) {
      ++refused;
      continue;
    }
    const int n = r1 - b.row0 + 1;
    model::Step buf[model::kMaxPatternLength];
    bool differs = false;
    for (int k = 0; k < n; ++k) {
      const model::Step now = src.step(t, b.row0 + k);
      buf[k] = pe::with_columns(now, src.step(t, r1 - k), b);  // (OS-AUDIT-56: a column block turns its own columns only)
      differs = differs || std::memcmp(&buf[k], &now, sizeof now) != 0;
    }
    if (!differs) {
      continue;
    }
    if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
      return false;
    }
    changed = ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(b.row0), buf, static_cast<std::uint32_t>(n)) == model::Status::Ok || changed;
  }
  if (refused > 0 && !changed) {
    return pe::say(ctx, core::Severity::Info, "PAST TRACK END");
  }
  return changed;
}

// ---- mute and solo -------------------------------------------------------------------------------------------------------

// MUTE honours the track's MUTE MODE (app/track_state.h). Immediate, or a transport that is not running: as above (a toggle still waiting for a boundary is dropped: the strip is told now). Bar or
// Pattern: the flag is written at once (the pad shows what the user did, the undo unit is this press) but the strip and the sequencer keep the old mute until the boundary, which the sequencer
// applies on its exact sample (MuteQuant); a second press while it waits puts the flag back and cancels it, and nothing ever reached the graph.
bool track_mute(ActionContext& ctx, core::ActionArg arg) noexcept {
  // OS-SONG3 S4: a hand mute while a PREVIEW audition runs ends the audition first, on the deferred path too (it writes the param directly below).
  (void)song_preview_end(ctx.session != nullptr ? ctx.session->state_editor() : nullptr, ctx.view, ctx.params);
  TrackSink* sink = ctx.mix != nullptr ? ctx.mix->tracks : nullptr;
  if (sink == nullptr || !track_arg_ok(ctx, arg)) {
    return toggle_track_param(ctx, arg, model::track_param::Mute);
  }
  const unsigned t = static_cast<unsigned>(arg);
  const MuteMode mode = static_cast<MuteMode>(ctx.mix->mute_mode[t] < kMuteModes ? ctx.mix->mute_mode[t] : 0u);
  const bool waiting = ((sink->mute_pending() >> t) & 1u) != 0u;
  if (mode == MuteMode::Immediate || !sink->playing()) {
    if (waiting) {
      (void)sink->mute_cancel(t);
    }
    return toggle_track_param(ctx, arg, model::track_param::Mute);
  }
  const core::ParamId id = core::param_id(core::ParamKind::Track, t, model::track_param::Mute);
  float now = 0.0f;
  if (!read_param(ctx.model, id, now)) {
    return false;
  }
  if (write_param(ctx, id, now != 0.0f ? 0.0f : 1.0f, false) != WriteResult::Changed) {
    return false;
  }
  if (waiting) {
    if (!sink->mute_cancel(t)) {
      send_param_change(ctx, id, now != 0.0f ? 0.0f : 1.0f);  // could not withdraw it: the flag decides, as at the boundary
    }
  } else if (!sink->mute_defer(t, mode, now != 0.0f)) {
    send_param_change(ctx, id, now != 0.0f ? 0.0f : 1.0f);  // no boundary to wait for: at once
  }
  return true;
}

// Cycles the track's MUTE MODE: Immediate, Bar, Pattern. Not an undo unit (the project keeps it as state: OS-PERSIST2, the MUTE chunk). A toggle that is waiting keeps the boundary it was given.
bool track_mute_mode(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.mix == nullptr || arg < 0 || static_cast<unsigned>(arg) >= kMixTracks ||
      (ctx.model.project != nullptr && static_cast<std::uint32_t>(arg) >= ctx.model.project->active_tracks())) {
    return false;
  }
  std::uint8_t& m = ctx.mix->mute_mode[arg];
  m = static_cast<std::uint8_t>(next_mute_mode(static_cast<MuteMode>(m < kMuteModes ? m : 0u)));
  return true;
}
bool track_solo(ActionContext& ctx, core::ActionArg arg) noexcept { return toggle_track_param(ctx, arg, model::track_param::Solo); }

}  // namespace o3os::app
