// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the pattern's edit actions share (OS-009 P1a.1; private to app/src): walking the selection through the Editor, the cursor
// step after an entry, and the notifications that say why nothing happened. Every edit is made through ActionContext::edit, which
// only an undoable action is given, so the edits of one action are one undo unit named by the action and its argument.

#include "action_handlers.h"

#include <o3os/app/pattern_source.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/model/editor.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/step_edit.h>

#include <cstddef>
#include <cstring>

namespace o3os::app::pe {

constexpr int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

inline bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return say_tag(ctx, severity, text, "edit");
}

// What the whole-pattern operations (PAT-OPS: TOOLS DUPLICATE / EXPAND / SHRINK, the paste of a held F1) say outside Edit; the Fill dialog names itself in its own (FILL: TURN EDIT ON).
inline constexpr const char* kEditOff = "TURN EDIT ON";

// The edit actions are for the Pattern screen, in Edit (F4, F6, F7, F8, Copy, Insert and Delete are unbound outside edit mode).
inline bool edit_enabled(const ActionContext& ctx) noexcept {
  return pattern_editable(ctx.view);  // the one predicate (OS-RECSIGN): the REC signs draw from it too
}
// Muting and soloing a track works in every mode: the Pattern screen is the condition.
inline bool track_enabled(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Pattern || ctx.view.screen == ScreenId::Perform || ctx.view.screen == ScreenId::Song; }  // (Perform: Shift + F1..F8, OS-011b; Song: the TRACKS pads, OS-SONGPADS)
// Selecting works in every mode (Shift + the arrows). While the cursor rides the playhead (select_follows: Browse, and Edit in follow, Shift + Rec) the arrows still select, but the block's end rows are
// the arrows' own and the playhead does not move them (Selection::follow; OS-BUG-WATCHSEL): a playhead passing step 0 must not be able to select everything (PAR-093). A block made stopped or in free Edit is
// cleared when the pattern plays with edit off (OS-009c).
inline bool select_enabled(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Pattern; }
inline bool select_follows(const ActionContext& ctx) noexcept { return ctx.view.play.playing && pattern_cursor_follows(ctx.view); }

struct Edited {
  bool changed = false;  // at least one step was written
  int refused = 0;       // selected tracks whose steps all lie past the track's end (D7)
  bool failed = false;   // the pattern could not be created: the pool is full
};

// The pattern does not exist yet: the first edit creates it, in the unit that is open (D1). False: no room (said).
inline bool create_if_missing(ActionContext& ctx, bool& exists, unsigned length) noexcept {
  if (exists) {
    return true;
  }
  const model::Status st = ctx.edit->create_pattern(ctx.view.edit.pattern, length);
  if (st != model::Status::Ok) {
    (void)say(ctx, core::Severity::Warn, "NO ROOM FOR A NEW PATTERN");
    return false;
  }
  exists = true;
  return true;
}

// The thunk a call site passes to edit_selection: captureless, so it converts to this pointer type; `fn_ctx` carries what the
// lambda captured (one value, or a small struct declared at the call site).
using EditFn = void (*)(model::Step&, const model::edit::Rule&, model::edit::Last&, void*);

// Runs `fn(step, rule, last, fn_ctx)` over every step of the selection (the cursor step when there is no range) that lies inside its track,
// track by track, and writes what changed through the Editor: the span from the first to the last changed step of a track, one record.
// The pattern is created first if it does not exist, but only when there is something to write. `last` is the session's last used
// instrument and FX, which the rules read and update; they go back to the view when the walk ends. One out-of-line function
// (pattern_edit_util.cpp, OS-FLASHDIET T1), not one instantiation per edit.
// `multi_cell` (OS-PTOOLS2 S3): the caller is a block verb, not the encoder — a single step of the range takes the multi-cell rule, so a
// single EMPTY step is never given a note or an instrument (the encoder's own single-cell insert stays with the plain call).
// `block_cols` (OS-SELCOL, the encoder's value edits): of a column block (OS-AUDIT-56) only the block's columns take what `fn` wrote, the others of each step stay as they were.
Edited edit_selection(ActionContext& ctx, EditFn fn, void* fn_ctx, bool multi_cell = false, bool block_cols = false) noexcept;

// OS-AUDIT-56 (column-level block, new Community OS behaviour). The fields of the columns c0..c1 (Column values) of `from` over `to`; the others of `to` stay. Note and Instr are one pair (as
// in Delete): a range that holds either takes both, so a note never loses its instrument.
inline void copy_columns(model::Step& to, const model::Step& from, int c0, int c1) noexcept {
  const auto in = [&](Column c) { return c0 <= static_cast<int>(c) && static_cast<int>(c) <= c1; };
  const bool pair = in(Column::Note) || in(Column::Instr);
  to.note = pair ? from.note : to.note;
  to.instr = pair ? from.instr : to.instr;
  to.vol = in(Column::Vol) ? from.vol : to.vol;
  to.fx[0] = in(Column::Fx1) ? from.fx[0] : to.fx[0];
  to.fx[1] = in(Column::Fx2) ? from.fx[1] : to.fx[1];
}
// `cur` with the columns of block `b` taken from `from` (a whole-step block: `from` itself).
inline model::Step with_columns(const model::Step& cur, const model::Step& from, const Selection::Block& b) noexcept {
  model::Step out = cur;
  copy_columns(out, from, b.col0, b.col1);
  return out;
}
// The model::edit elements a column range clears: Note and Instr are one element (the pair is cleared together, as everywhere), so either column takes both.
inline unsigned column_elements(const Selection::Block& b) noexcept {
  const auto in = [&](Column c) { return b.col0 <= static_cast<int>(c) && static_cast<int>(c) <= b.col1; };
  return (in(Column::Note) || in(Column::Instr) ? model::edit::kElNote : 0u) | (in(Column::Vol) ? model::edit::kElVol : 0u) | (in(Column::Fx1) ? model::edit::kElFx1 : 0u) |
         (in(Column::Fx2) ? model::edit::kElFx2 : 0u);
}

// A note written by hand (STEP, a pad, the MIDI keyboard) over a step that holds a note replaces the note AND the instrument (the current one) and keeps the volume and both FX
// (OS-AUDIT E-PAT-14, matches the OG convention; an empty step already took the current instrument). The model rule alone keeps the old instrument.
inline void write_note(model::Step& s, unsigned note, const model::edit::Rule& r, model::edit::Last& l) noexcept {
  model::edit::note_set(s, note, r, l);
  if (note <= 128u && s.note == note) {  // a real note the rule wrote (not fade, cut or off, which carry no instrument; an empty step of a block leaves s.note as it was)
    s.instr = l.instrument;
  }
}

// The cursor goes down by the edit step (plus `extra`) and wraps at the end of
// the pattern; with a step of 0 it stays. The selection collapses to the cursor step. (A step longer than the pattern
// wraps and stays inside it.) An auto-repeat stops at the last row, and so does a clearing action
// (`wrap` false: step.clear, wherever the hold starts, OS-DELREPEAT).
inline void move_cursor_by_step(ActionContext& ctx, int extra, bool wrap = true) noexcept {
  const int step = static_cast<int>(ctx.view.edit.edit_step) + extra;
  if (step <= 0) {
    return;
  }
  const PatternView::Dims d = pattern_dims(ctx.model, ctx.view);
  const int row = ctx.view.pattern.row();
  // A held key never wraps (OS-DELREPEAT: ctx.repeat, an auto-repeat of the key): its repeats stop at the last row instead of going on from row 0; a fresh press keeps the wrap above, except a clearing
  // action (`wrap` false), whose press may be the start of a hold on the last row.
  const int to = row + step <= d.steps - 1 ? row + step : (wrap && !ctx.repeat ? (row + step) % d.steps : d.steps - 1);
  (void)ctx.view.pattern.jump_row(to, d);
  ctx.view.edit.selection.clear();
}

// The part of an entry that follows the edit: "PAST TRACK END" when nothing could be edited, else (for an entry with no range) the
// cursor moves on. `advance` is false for the deltas (the encoder does not move the cursor) and the absolute values.
inline bool finish_entry(ActionContext& ctx, const Edited& e, bool advance) noexcept {
  bool changed = e.changed;
  if (e.refused > 0 && !e.changed && !e.failed) {
    changed = say(ctx, core::Severity::Info, "PAST TRACK END") || changed;
    return changed;
  }
  if (advance && !e.failed && !ctx.view.edit.selection.active) {
    move_cursor_by_step(ctx, 0);
    changed = true;
  }
  return changed;
}

// 0 for the FX1 column, 1 for FX2, -1 when the cursor is on another column.
inline int fx_column(const ViewState& view) noexcept {
  return view.pattern.column() == Column::Fx1 ? 0 : (view.pattern.column() == Column::Fx2 ? 1 : -1);
}

}  // namespace o3os::app::pe
