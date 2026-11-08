// SPDX-License-Identifier: GPL-3.0-or-later
// The EVENTS overlay's actions (OS-MIDIREC S1b/S1c/S2b, docs/specs/OS-MIDIREC.md sections 3.1, 3.4, 3.5, 3.7): open (from the
// TOOLS list's EVENTS entry, tools_actions.cpp), move, page, field, back, F1 FILTER (the ALL / NOTES / CC cycle of 3.2, S1c),
// F5 PREVIEW (the row's step sounds through the grid's own preview path, PreviewSink::hold_note, A14; S1c), and S2b's field
// edits: the encoder and Shift + encoder on the cursor row's field (events.edit / events.coarse) and the pads (events.pad,
// A14). Each edit is planned by the pure model::events::plan (S2a) and applied as ONE dispatched edit through Editor::set_step
// (3.5, A12): one undo unit per detent in the PATTERN ring, a refusal on the strip with nothing written and no unit. Edits
// need STEP: in WATCH the key says TURN EDIT ON (A13, QA-PAT-424). S2c: Insert / Shift + Insert and Delete (A9, A10) are edits of the same
// kind, and F7 / Shift + F7 run the grid's UNDO / REDO inside the list (A12) and project it again. S3: F2 QUANT (the row) and Shift + F2
// (every NOTE and end row the filter shows, one unit). The cursor and the window are kept as positions
// in the projected list (events_view.h), and every action projects at most one window from `top`, so nothing walks a full
// scope per frame (the S1a review's rule); after an edit the cursor is re-anchored on its row's key (3.2), following a moved
// row, else the nearest row in time.
#include "action_handlers.h"
#include "pattern_edit_util.h"

#include <o3os/app/events_view.h>
#include <o3os/app/pad_note.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/model/event_list.h>
#include <o3os/model/project.h>

namespace o3os::app {

namespace {

namespace ev = model::events;

const model::Pattern* events_pattern(const ActionContext& ctx) noexcept {
  return ctx.model.project != nullptr ? ctx.model.project->pattern(ctx.view.edit.pattern) : nullptr;
}

ev::Env events_env(const ActionContext& ctx) noexcept {
  return ctx.model.project != nullptr ? ev::env_of(*ctx.model.project) : ev::Env{};
}

// Keeps the window on the cursor: the cursor is always inside the kEventsRows rows from `top`.
void events_follow(ViewState& v) noexcept {
  const std::int32_t c = static_cast<std::int32_t>(v.events.cursor);
  std::int32_t top = static_cast<std::int32_t>(v.events.top);
  if (c < top) {
    top = c;
  } else if (c >= top + kEventsRows) {
    top = c - kEventsRows + 1;
  }
  v.events.top = static_cast<std::uint32_t>(top < 0 ? 0 : top);
}

// The field snaps to one the row's kind has, when the cursor moved to a row of another kind.
void events_field_snap(ActionContext& ctx) noexcept {
  ev::Row row;
  if (!events_cursor_row(*ctx.model.project, ctx.view.edit.pattern, ctx.view.events, row)) {
    return;
  }
  const auto f = static_cast<EventsView::Field>(ctx.view.events.field);
  if (!events_field_ok(row.key.kind, f)) {
    ctx.view.events.field = static_cast<std::uint8_t>(EventsView::Field::Time);
  }
}

std::uint32_t events_total(const ActionContext& ctx) noexcept {
  const model::Pattern* pat = events_pattern(ctx);
  return pat != nullptr ? ev::count_rows(*pat, events_env(ctx), ctx.view.events.scope, ctx.view.events.filter) : 0u;
}

// The key the cursor's row has after the plan's writes (3.2): a TIME move carries the row to another step (A21) — a NOTE
// or end row to the write whose note column holds its kind, a CC row to the write whose FX column holds its CC. A plan that
// did not move the row leaves the key alone.
ev::Key events_moved_key(const ev::Plan& p, const ev::Key& old) noexcept {
  for (unsigned i = 0; i < p.n; ++i) {
    const ev::StepWrite& w = p.writes[i];
    if (w.track != old.track || w.step == old.step) {
      continue;
    }
    if (old.kind == ev::Kind::Cc) {
      for (unsigned c = 0; c < 2u; ++c) {
        if (ev::is_cc_type(w.value.fx[c].type)) {
          return ev::Key{old.track, w.step, old.kind, static_cast<std::uint8_t>(c)};
        }
      }
    } else if (old.kind == ev::Kind::Note && ev::note_is_note(w.value.note)) {
      return ev::Key{old.track, w.step, old.kind, 0u};
    } else if (ev::is_end(old.kind) && ev::end_kind_of(w.value.note) == old.kind) {
      return ev::Key{old.track, w.step, old.kind, 0u};
    }
  }
  return old;
}

// After a write (an edit, and S2c's undo): the list projects again from the new generation and the cursor keeps its key
// (3.2, 3.6); a key that is gone goes to the nearest row in time — the first row at or after its step, else the last row.
void events_reanchor(ActionContext& ctx, const ev::Key& key) noexcept {
  ViewState& v = ctx.view;
  const model::Pattern* pat = events_pattern(ctx);
  if (pat == nullptr || ctx.model.project == nullptr) {
    return;
  }
  const ev::Env env = events_env(ctx);
  const std::int32_t idx = ev::index_of_key(*pat, env, v.events.scope, v.events.filter, key);
  v.events.cursor = idx >= 0 ? static_cast<std::uint32_t>(idx)
                             : events_start_cursor(*pat, env, v.events.scope, v.events.filter, static_cast<int>(key.step));
  v.events.anchor_gen = ctx.model.project->pattern_generation(v.edit.pattern);
  v.events.anchor_valid = true;
  v.events.anchor_step = key.step;
  events_follow(v);
}

// One edit, planned by events::plan and applied as one dispatched edit (3.5): the plan's writes go through Editor::set_step
// inside the unit dispatch() opened, a refusal says its text on the strip and writes nothing (so no unit is committed), and
// A11's one-voice notice is said with its writes. The session's last used follows the planner's `last`.
bool events_apply(ActionContext& ctx, const ev::Request& req, const ev::Key& old_key) noexcept {
  ViewState& v = ctx.view;
  const model::Pattern* pat = events_pattern(ctx);
  if (pat == nullptr || ctx.edit == nullptr) {
    return false;
  }
  model::edit::Last last{static_cast<std::uint8_t>(pe::clampi(v.instrument, 1, static_cast<int>(model::kMaxInstruments))), v.edit.last_fx};
  const ev::Plan p = ev::plan(*pat, events_env(ctx), v.events.scope, last, req);
  if (p.n == 0u) {
    if (p.text[0] != '\0') {
      return pe::say(ctx, core::Severity::Info, p.text);  // the refusal: nothing written, no unit
    }
    return false;  // a no-op (a stale key, a delta of 0): silent
  }
  bool changed = false;
  for (unsigned i = 0; i < p.n; ++i) {
    const ev::StepWrite& w = p.writes[i];
    changed = ctx.edit->set_step(v.edit.pattern, static_cast<std::uint32_t>(w.track), static_cast<std::uint32_t>(w.step), w.value) == model::Status::Ok || changed;
  }
  v.instrument = p.last.instrument;
  v.edit.last_fx = p.last.fx;
  if (p.text[0] != '\0') {
    (void)pe::say(ctx, core::Severity::Info, p.text);  // A11's one-voice notice: the writes stand and the strip says where the note above ends
  }
  if (changed) {
    events_reanchor(ctx, p.has_new_key ? p.new_key : events_moved_key(p, old_key));
    events_field_snap(ctx);
  }
  return changed;
}

// The encoder on the cursor row's field, one detent (events.edit) or the coarse step of Shift + encoder (events.coarse,
// OS-SHIFTENC; 3.4). The field is the view's. In WATCH the key says TURN EDIT ON and writes nothing (A13, QA-PAT-424).
bool events_field_edit(ActionContext& ctx, core::ActionArg arg, bool coarse) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, pe::kEditOff);
  }
  const model::Pattern* pat = events_pattern(ctx);
  if (pat == nullptr || ctx.edit == nullptr) {
    return false;
  }
  ev::Row row;
  const bool have = events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row);
  if (!have) {
    // an empty list: the edit keys say what adds one (A9); a cursor off the window's rows edits nothing
    return events_total(ctx) == 0u ? pe::say(ctx, core::Severity::Info, "NO EVENTS: INSERT ADDS ONE") : false;
  }
  ev::Request req{};
  req.op = ev::Op::Edit;
  req.field = static_cast<ev::Field>(v.events.field);
  req.has_key = have;
  req.key = row.key;
  req.cursor_track = static_cast<std::uint8_t>(v.pattern.track() < 0 ? 0 : v.pattern.track());
  req.delta = static_cast<int>(arg);
  req.coarse = coarse;
  return events_apply(ctx, req, row.key);
}

}  // namespace

// The row under the cursor, from the window the view draws (one projection from `top`): the actions and the base screen's info
// line read the same row the overlay draws under the cursor.
bool events_cursor_row(const model::Project& project, unsigned pattern, const EventsView& ev, model::events::Row& out) noexcept {
  const model::Pattern* pat = project.pattern(pattern);
  if (pat == nullptr) {
    return false;
  }
  ev::Row rows[kEventsRows];
  const std::uint32_t n = ev::window(*pat, ev::env_of(project), ev.scope, ev.filter, ev.top, rows, kEventsRows, nullptr);
  const std::int32_t i = static_cast<std::int32_t>(ev.cursor) - static_cast<std::int32_t>(ev.top);
  if (i < 0 || static_cast<std::uint32_t>(i) >= n) {
    return false;
  }
  out = rows[i];
  return true;
}

// The TOOLS entry EVENTS (A18): opens the list over the grid pane. NO PROJECT is tools_open's refusal, said when F7 was pressed; the follow rule is the list's own since OS-PTOOLS2 S1 (the TOOLS list opens in WATCH
// while playing, the events list does not: its rows are read from the cursor's track, which the playhead holds), plus PATTERN IS EMPTY for a pattern the project lacks (A13). The
// scope is fixed here (A2): the cursor's track, or the block's tracks and rows as whole steps (a column block is widened —
// Selection::block gives whole steps to a block that spans tracks, and the scope has no columns). The cursor starts on the
// first row at or after the grid cursor's step (3.1).
bool events_open(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.screen != ScreenId::Pattern || (v.overlay != OverlayId::None && v.overlay != OverlayId::VerbMenu)) {
    return false;
  }
  if (pe::select_follows(ctx)) {
    return pe::say(ctx, core::Severity::Info, "STOP PLAYING TO OPEN EVENTS");
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  const model::Pattern* pat = events_pattern(ctx);
  if (pat == nullptr) {
    return pe::say(ctx, core::Severity::Info, "PATTERN IS EMPTY");
  }
  EventsView& ev = v.events;
  ev.scope = events_scope_of(*pat, v.edit, v.pattern.row(), v.pattern.track());
  ev.filter = ev::Filter::All;
  ev.field = static_cast<std::uint8_t>(EventsView::Field::Time);
  ev.cursor = 0;
  ev.top = 0;
  ev.anchor_gen = ctx.model.project->pattern_generation(v.edit.pattern);
  ev.anchor_valid = true;
  // The first row at or after the grid cursor's step (3.1); none: the last row.
  ev.cursor = events_start_cursor(*pat, events_env(ctx), ev.scope, ev.filter, v.pattern.row());
  events_follow(v);
  ev.anchor_step = static_cast<std::uint8_t>(v.pattern.row());
  // Info news still queued from the grid must not outlive the list: the strip waits behind this modal overlay and a hidden
  // head never expires, so it would mask the list's own refusals after BACK (qa_pat_419's snap shows the refusal, not the
  // stale notice). Every Info entry goes, whatever its source; warnings and errors (UNDO HISTORY CLEARED BY THE LOAD,
  // MACRO FULL) stay.
  v.notify.dismiss(core::Severity::Info);
  v.overlay = OverlayId::Events;
  return true;
}

// Up / Down / the encoder: the previous or next row (arg = rows, + down). Held keys repeat (kActionRepeatable).
bool events_move(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events) {
    return false;
  }
  const std::uint32_t total = events_total(ctx);
  if (total == 0u) {
    return false;
  }
  std::int64_t to = static_cast<std::int64_t>(v.events.cursor) + static_cast<std::int64_t>(arg);
  if (to < 0) {
    to = 0;
  }
  if (to >= static_cast<std::int64_t>(total)) {
    to = static_cast<std::int64_t>(total) - 1;
  }
  const std::uint32_t cursor = static_cast<std::uint32_t>(to);
  const bool changed = cursor != v.events.cursor;
  v.events.cursor = cursor;
  events_follow(v);
  events_field_snap(ctx);
  return changed;
}

// Shift + Up / Down / the encoder: a page of kEventsRows rows (OS-NAV3).
bool events_page(ActionContext& ctx, core::ActionArg arg) noexcept {
  return events_move(ctx, arg * kEventsRows);
}

// Left / Right: the previous or next field of the cursor's row, skipping the ones the row's kind does not have (3.4).
bool events_field(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events) {
    return false;
  }
  ev::Row row;
  if (!events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row)) {
    return false;
  }
  const int step = arg < 0 ? -1 : 1;
  unsigned f = v.events.field;
  for (unsigned k = 0; k < static_cast<unsigned>(EventsView::Field::kCount); ++k) {
    f = (f + static_cast<unsigned>(step) + static_cast<unsigned>(EventsView::Field::kCount)) % static_cast<unsigned>(EventsView::Field::kCount);
    if (events_field_ok(row.key.kind, static_cast<EventsView::Field>(f))) {
      break;
    }
  }
  const bool changed = f != v.events.field;
  v.events.field = static_cast<std::uint8_t>(f);
  return changed;
}

// F8 BACK (3.1): closes the list and puts the grid cursor on the row's step, track and column — the note column for a NOTE or
// end row, the CC's FX column for a CC row.
bool events_back(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events) {
    return false;
  }
  ev::Row row;
  const bool have = events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row);
  v.overlay = OverlayId::None;
  if (have) {
    const PatternView::Dims d = pattern_dims(ctx.model, v);
    (void)v.pattern.jump_row(row.key.step, d);
    (void)v.pattern.move_track(static_cast<int>(row.key.track) - v.pattern.track(), d);
    (void)v.pattern.select_column(row.key.kind == ev::Kind::Cc ? (row.key.column == 0u ? Column::Fx1 : Column::Fx2) : Column::Note);
  }
  return true;
}

// F1 FILTER (3.2, S1c): ALL -> NOTES -> CC -> ALL. The header's count and the key row's F1 label follow the filter; the cursor
// stays inside the list the filter leaves (it is a position, not a key), the window and the field follow it. Nothing is written,
// no unit is opened.
bool events_filter(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events) {
    return false;
  }
  const auto next = static_cast<ev::Filter>((static_cast<unsigned>(v.events.filter) + 1u) % 3u);
  v.events.filter = next;
  const std::uint32_t total = events_total(ctx);
  if (v.events.cursor >= total) {
    v.events.cursor = total > 0u ? total - 1u : 0u;
  }
  events_follow(v);
  events_field_snap(ctx);
  return true;
}

// F5 PREVIEW (A14, S1c): the row's step sounds, through the grid's own preview path (pattern.preview): its note on the row's
// instrument (the resolved one the IN column shows, the last used one when the row reads '?'), held until the key is let go —
// the release is a raw event App::run_once ends (ViewState::step_preview), as on the grid. A step with no note says so; no audio
// path says that. Nothing is written, no unit is opened, nothing new in the render.
bool events_preview(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (ctx.preview == nullptr) {
    return v.notify.post(core::Severity::Info, "PREVIEW: NO AUDIO OUTPUT", "preview", ViewState::kMessageMs);
  }
  const model::Pattern* pat = events_pattern(ctx);
  ev::Row row;
  if (pat == nullptr || !events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row)) {
    return false;  // an empty list: nothing to sound
  }
  const model::Step& st = pat->steps[row.key.track][row.key.step];
  if (!ev::note_is_note(st.note)) {
    return v.notify.post(core::Severity::Info, "PREVIEW: NO NOTE ON THIS STEP", "preview", ViewState::kMessageMs);
  }
  const unsigned instrument = row.instr >= 1u && row.instr <= model::kMaxInstruments ? row.instr : static_cast<unsigned>(v.instrument);
  ctx.preview->hold_note(true, row.key.track, instrument, st.note - 1u);
  v.step_preview = true;
  return false;  // nothing to redraw: the sound is the engine's
}

// ---- S2b: the field edits (3.5, A13-A14, A21) -----------------------------------------------------------------------------------------

// The encoder on the cursor row's field (3.4): one detent, planned by events::plan (TIME a microstep, a CC a step, NOTE a
// semitone, VAL 1, LEN a step, TYPE the next CC letter, CH the next channel, IN the next instrument). One dispatched edit,
// one undo unit a detent.
bool events_edit(ActionContext& ctx, core::ActionArg arg) noexcept { return events_field_edit(ctx, arg, false); }

// Shift + encoder (OS-SHIFTENC): the coarse step (TIME a step, NOTE an octave, VAL 10, LEN 4 steps; a CC row's TIME 4
// steps, new Community OS behaviour).
bool events_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return events_field_edit(ctx, arg, true); }

// A pad (A14): on the DATA field of a NOTE row the pad's note, entered as the grid's pad entry does in STEP (the pad board's
// note through pad_note, stored as the model byte) — and sounded, with the session's last note following it, as on the grid
// (OS-PADS-B, QA-PAT-412 "heard as on the grid"); on VAL the 0..127 ramp over the pad, as the Vol pads, sounding the row's
// note; anywhere else the row's step sounds (a pad preview: pad_down, the pad's release ends it). The writing pads need STEP
// like the encoder; the sounding pad is a preview and works in every mode. Each writing pad is one dispatched edit, one undo unit.
bool events_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (arg < 0 || arg >= static_cast<core::ActionArg>(model::edit::kPadCount)) {
    return false;
  }
  const model::Pattern* pat = events_pattern(ctx);
  if (pat == nullptr) {
    return false;
  }
  ev::Row row;
  if (!events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row)) {
    // an empty list: the pads are edit keys there (A9 names what adds one); in WATCH any edit key says TURN EDIT ON (A13)
    if (!pe::edit_enabled(ctx)) {
      return pe::say(ctx, core::Severity::Info, pe::kEditOff);
    }
    return events_total(ctx) == 0u ? pe::say(ctx, core::Severity::Info, "NO EVENTS: INSERT ADDS ONE") : false;
  }
  const auto field = static_cast<EventsView::Field>(v.events.field);
  const bool note_field = field == EventsView::Field::Data && row.key.kind == ev::Kind::Note;
  const bool val_field = field == EventsView::Field::Val && (row.key.kind == ev::Kind::Note || row.key.kind == ev::Kind::Cc);
  const unsigned instrument = row.instr >= 1u && row.instr <= model::kMaxInstruments ? row.instr : static_cast<unsigned>(v.instrument);
  if (!note_field && !val_field) {
    // A14: the row's step sounds. A pad preview, not an edit: it works in WATCH too.
    if (ctx.preview == nullptr) {
      return false;
    }
    const model::Step& st = pat->steps[row.key.track][row.key.step];
    if (!ev::note_is_note(st.note)) {
      return false;
    }
    ctx.preview->pad_down(static_cast<unsigned>(arg), row.key.track, instrument, st.note - 1u);
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, pe::kEditOff);  // A13, QA-PAT-424
  }
  if (ctx.edit == nullptr) {
    return false;
  }
  // The note and the sound agree on the instrument that sounds — the row's resolved one (the IN column, the one F5 sounds
  // and the one the step keeps on a DATA write), not the strip's current instrument: the pad board is read for that
  // instrument too, so a beat slice instrument's pads (24 + pad, OS-SLICEFIX) are what is entered and heard.
  const unsigned pad_note_byte = pad_note(ctx.model.project, static_cast<int>(instrument), v.edit.pads, static_cast<unsigned>(arg));
  // OS-PADS-B, QA-PAT-412 ("heard as on the grid"): the writing pad sounds as the grid's pad entry does in STEP — the pad's
  // own note on DATA, the row's note on VAL. Sound first: the write below re-anchors.
  unsigned sound = 0;
  bool sound_ok = false;
  if (note_field) {
    sound = pad_note_byte;
    sound_ok = true;
  } else {
    const model::Step& st = pat->steps[row.key.track][row.key.step];
    if (ev::note_is_note(st.note)) {
      sound = st.note - 1u;
      sound_ok = true;
    }
  }
  if (sound_ok && ctx.preview != nullptr) {
    ctx.preview->pad_down(static_cast<unsigned>(arg), row.key.track, instrument, sound);
  }
  ev::Request req{};
  req.op = ev::Op::Edit;
  req.field = note_field ? ev::Field::Data : ev::Field::Val;
  req.has_key = true;
  req.key = row.key;
  req.cursor_track = static_cast<std::uint8_t>(v.pattern.track() < 0 ? 0 : v.pattern.track());
  req.pad = val_field;
  req.absolute = note_field;
  req.value = note_field ? pad_note_byte + 1u : static_cast<unsigned>(arg);
  const bool changed = events_apply(ctx, req, row.key);
  // The session's last note follows the write, as on the grid: a refused write (nothing written, no unit) leaves it alone.
  if (changed && sound_ok) {
    v.edit.last_note = static_cast<std::uint8_t>(sound);
  }
  return changed;
}

// ---- S2c: Insert, Delete and the history inside the list (A9, A10, A12, A13) ----------------------------------------------

namespace {

// The cursor row (none on an empty list) as a request's key, for the edit keys that act on a row or, with none, on the scope.
ev::Request events_row_request(const ActionContext& ctx, ev::Op op, bool have, const ev::Row& row) noexcept {
  ev::Request req{};
  req.op = op;
  req.has_key = have;
  req.key = row.key;
  req.cursor_track = static_cast<std::uint8_t>(ctx.view.pattern.track() < 0 ? 0 : ctx.view.pattern.track());
  return req;
}

}  // namespace

// Insert (arg 0) and Shift + Insert (arg 1), A9: planned by events::plan, one dispatched edit, one undo unit, the cursor on the
// new row (the plan's new key). In WATCH the key says TURN EDIT ON (A13).
bool events_insert(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, pe::kEditOff);
  }
  if (ctx.edit == nullptr || events_pattern(ctx) == nullptr) {
    return false;
  }
  ev::Row row;
  const bool have = events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row);
  ev::Request req = events_row_request(ctx, ev::Op::Insert, have, row);
  req.shift = arg != 0;
  return events_apply(ctx, req, row.key);
}

// Delete, A10: the row's owned bytes are cleared (an OFF keeps the note above's LEN); the cursor goes to the nearest row in time.
// On an empty list the key says what adds a row, as the other edit keys do (A9).
bool events_delete(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, pe::kEditOff);
  }
  if (ctx.edit == nullptr || events_pattern(ctx) == nullptr) {
    return false;
  }
  ev::Row row;
  if (!events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row)) {
    return events_total(ctx) == 0u ? pe::say(ctx, core::Severity::Info, "NO EVENTS: INSERT ADDS ONE") : false;
  }
  return events_apply(ctx, events_row_request(ctx, ev::Op::Delete, true, row), row.key);
}

namespace {

// F7 UNDO / Shift + F7 REDO inside the list (A12): the grid's own undo of the PATTERN area (edit.undo / edit.redo, which are
// refused under a modal overlay), then the list projects again from the new generation and the cursor keeps its key, else goes
// to the nearest row in time (3.2, 3.6).
bool events_history(ActionContext& ctx, bool undo) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events || v.screen != ScreenId::Pattern) {
    return false;
  }
  ev::Row row;
  const bool have = events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row);
  const bool done = undo ? edit_undo(ctx, 0) : edit_redo(ctx, 0);
  if (events_pattern(ctx) == nullptr) {
    return done;
  }
  if (have) {
    events_reanchor(ctx, row.key);
  } else {
    const std::uint32_t total = events_total(ctx);
    v.events.cursor = v.events.cursor >= total ? (total > 0u ? total - 1u : 0u) : v.events.cursor;
    events_follow(v);
  }
  events_field_snap(ctx);
  return done;
}

}  // namespace

bool events_undo(ActionContext& ctx, core::ActionArg) noexcept { return events_history(ctx, true); }
bool events_redo(ActionContext& ctx, core::ActionArg) noexcept { return events_history(ctx, false); }

// ---- S3: QUANT (spec S3, QA-PAT-426..428) --------------------------------------------------------------------------------------

namespace {

// quant_all's apply: a plan's writes through the Editor, inside the unit the dispatch opened.
bool events_quant_apply(void* p, const ev::Plan& plan) noexcept {
  ActionContext& ctx = *static_cast<ActionContext*>(p);
  for (unsigned i = 0; i < plan.n; ++i) {
    const ev::StepWrite& w = plan.writes[i];
    if (ctx.edit->set_step(ctx.view.edit.pattern, w.track, w.step, w.value) != model::Status::Ok) {
      return false;  // stop at the first failure: no half-applied move
    }
  }
  return true;
}

}  // namespace

// F2 QUANT the row (arg 0) and Shift + F2 every NOTE and end row the filter shows (arg 1). The row: events::plan's Op::Quant (an
// offset under 24 microsteps goes back to .00, else on to the next step; a refusal says why, a CC says CC IS ON THE STEP), one
// dispatched edit like every other. Every row: one pass of quant_all inside the one unit, and the strip says QUANTISED n, m LEFT
// (STEP TAKEN). Needs STEP, like every edit (A13).
bool events_quant(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::Events || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, pe::kEditOff);
  }
  if (ctx.edit == nullptr || events_pattern(ctx) == nullptr) {
    return false;
  }
  ev::Row row;
  const bool have = events_cursor_row(*ctx.model.project, v.edit.pattern, v.events, row);
  if (arg == 0) {
    if (!have) {
      return events_total(ctx) == 0u ? pe::say(ctx, core::Severity::Info, "NO EVENTS: INSERT ADDS ONE") : false;
    }
    return events_apply(ctx, events_row_request(ctx, ev::Op::Quant, true, row), row.key);
  }
  const model::Pattern* pat = events_pattern(ctx);
  const model::edit::Last last{static_cast<std::uint8_t>(pe::clampi(v.instrument, 1, static_cast<int>(model::kMaxInstruments))), v.edit.last_fx};
  const std::uint32_t gen = ctx.model.project->pattern_generation(v.edit.pattern);
  const ev::QuantResult r = ev::quant_all(*pat, events_env(ctx), v.events.scope, v.events.filter, last, &events_quant_apply, &ctx);
  char text[core::kNotifyTextChars];
  ev::quant_message(r, text, sizeof text);
  (void)pe::say(ctx, core::Severity::Info, text);
  const bool changed = ctx.model.project->pattern_generation(v.edit.pattern) != gen;
  if (changed) {
    // the cursor keeps its key, else goes to the nearest row in time (3.2); the key is read before the writes
    events_reanchor(ctx, have ? row.key : ev::Key{});
    events_field_snap(ctx);
  }
  return changed;
}

}  // namespace o3os::app
