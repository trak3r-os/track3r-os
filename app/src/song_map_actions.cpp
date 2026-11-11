// SPDX-License-Identifier: GPL-3.0-or-later
// The Song map's actions (OS-SONGMAP, docs/specs/SONG-UX.md section 1; NEW COMMUNITY OS BEHAVIOUR, no format change): the tracks of the rectangle extended, copied, pasted, cleared, transformed by the TOOLS
// dialog and made UNIQUE, and the question that asks first when an edit would also change a slot outside the rectangle. The arithmetic is song_edit.h's (a planner and an executor over one walk); this file is the
// keys: which rectangle, which message, which limit. Every write is the caller's one undo unit (the actions are kActionUndoable: dispatch() opened it), the copies of UNIQUE and the edit after them included.
#include "action_handlers.h"

#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/session.h>
#include <o3os/app/song_build.h>
#include <o3os/app/song_clip.h>
#include <o3os/app/song_edit.h>
#include <o3os/app/song_source.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/app/text_format.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/model/step_edit.h>

namespace o3os::app {

namespace {

constexpr int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// (upsert: the notice of one key replaces the one of the key before, which would otherwise show for its whole time while the user works on; the question's text is rewritten the same way)
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept { return ctx.view.notify.upsert(severity, text, "song", ViewState::kMessageMs); }

// The unit's ceiling (1.7): half the Song ring, 48 KiB on the device. A unit that outgrew its ring would silently reset the whole Song history (undo.h), so a bigger edit writes nothing.
std::size_t undo_limit(const ActionContext& ctx) noexcept { return song_undo_limit(ctx.edit != nullptr ? ctx.edit->journal(model::UndoArea::Song).capacity_bytes() : 0u); }

void append_count(AskText& text, unsigned n, const char* one, const char* many) noexcept {
  append_uint(text, n);
  text.append(n == 1u ? one : many);
}

// The rectangle the keys act on: the selected slots (the cursor's alone with no range) and, in the map, the selected tracks; on PAT (all = true) every active track. False, said, with no project or no slot.
bool rectangle(ActionContext& ctx, bool all, SongRect& rect) noexcept {
  const model::Project* p = ctx.model.project;
  if (p == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT") && false;
  }
  const int count = static_cast<int>(p->song_length());
  const int active = static_cast<int>(p->active_tracks());
  if (count <= 0 || active <= 0) {
    return say(ctx, core::Severity::Info, "SONG IS EMPTY") && false;
  }
  const SongView& sv = ctx.view.song;
  rect.s0 = clamp_int(sv.first(), 0, count - 1);
  rect.s1 = clamp_int(sv.last(), rect.s0, count - 1);
  if (all || !sv.in_map()) {
    rect.t0 = 0;
    rect.t1 = active - 1;
  } else {
    rect.t0 = clamp_int(sv.tfirst(), 0, active - 1);
    rect.t1 = clamp_int(sv.tlast(), rect.t0, active - 1);
  }
  return true;
}

// Is there a rectangle of more than one cell selected (a slot range or a track range)? A paste then tiles the clip over it (Q3).
bool selected(const SongView& sv, const SongRect& rect) noexcept { return (sv.selecting() || sv.track_selecting()) && !rect.single(); }

// ---- the question (1.6) ---------------------------------------------------------------------------------------------------------

// The text of the question: the head the table's keys follow. One pattern names it and the slots outside that hold it; several say how many (the spec's wording, cut to what the strip's 48 characters leave beside
// the keys: D11). Above 99 patterns the word is PATS: with PATTERNS and a count of three digits for the slots too the head is 30 characters and, with the 19 of ": F1 ALL, F2 UNIQUE", 49: one over the strip.
void ask_head(const SongAsk& a, AskText& out) noexcept {
  out.clear();
  if (a.confirm) {  // OS-AUDIT-69: "CLEAR 3 PATTERNS?" (kMaxPatterns is 128: three digits at most)
    out.assign("CLEAR ");
    append_uint(out, a.shared);
    out.append(" PATTERNS?");
    return;
  }
  if (a.shared == 1u) {
    out.assign("PATTERN ");
    append_dec(out, a.first, a.first > 99u ? 3u : 2u);
    out.append(" ALSO IN ");
  } else {
    append_uint(out, a.shared);
    out.append(a.shared > 99u ? " PATS ALSO IN " : " PATTERNS ALSO IN ");
  }
  append_count(out, a.others, " SLOT", " SLOTS");
}

// `confirm` (OS-AUDIT-69): the clear of 2 or more patterns asks first, F1 YES and any other key withdraws; `shared` then holds how many patterns, and the shared-pattern fields are unused.
void ask(ActionContext& ctx, const SongEdit& edit, const SongPlan& plan, bool confirm = false, bool cut = false) noexcept {
  SongAsk& a = ctx.view.song.ask;
  a.live = true;
  a.confirm = confirm;
  a.cut = confirm && cut;
  a.edit = edit;
  a.generation = ctx.model.project->song_generation();
  const unsigned named = confirm ? plan.patterns : plan.shared;
  a.first = static_cast<std::uint8_t>(confirm ? 0u : plan.first_shared);
  a.shared = static_cast<std::uint8_t>(named > 255u ? 255u : named);
  a.others = static_cast<std::uint8_t>(confirm ? 0u : (plan.shared_slots > 255u ? 255u : plan.shared_slots));
  AskText head;
  AskText text;
  song_ask_head(a, head);
  ask_text(song_ask_kind(a), head.c_str(), text);
  (void)ctx.view.notify.ask(core::Severity::Warn, text.c_str(), "song", SongAsk::kMs);
}

// What an edit says it did.
void say_done(ActionContext& ctx, const SongEdit& e, const SongDone& done, const SongPlan& plan, unsigned copies) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  switch (e.op) {
    case SongOp::Paste: text.assign("PASTED: "); break;
    case SongOp::Clear: text.assign("CLEARED: "); break;
    case SongOp::Transpose: text.assign("TRANSPOSED: "); break;
    case SongOp::Instrument: text.assign("INSTRUMENT SET: "); break;
  }
  append_count(text, done.patterns, " PATTERN", " PATTERNS");
  if (copies != 0u) {
    text.append(" + ");
    append_count(text, copies, " COPY", " COPIES");
  }
  if (plan.clipped) {
    text.append(copies != 0u ? ", CLIPPED" : ", CLIPPED AT TRACK END");  // (with the copies the short form: the long one would not fit the strip's 48 characters)
  }
  (void)say(ctx, core::Severity::Info, text.c_str());
}

// Writes the edit (after UNIQUE's copies when `unique`) as the caller's one unit, after the checks that can refuse it before a byte is written: the pattern pool, and the unit's size against the ceiling (1.7).
// The copies carry the very steps of what they copy, so the edit over them is the edit planned over the originals: the journal bytes are the two plans' sum (a missing pattern that UNIQUE copies is created by the copy and
// then not again by the paste: the sum is then a few bytes over, never under).
bool write_edit(ActionContext& ctx, const SongEdit& edit, const SongPlan& plan, bool unique) noexcept {
  const model::Project& p = ctx.edit->project();
  SongUnique u;
  if (unique) {
    song_unique_plan(p, edit.rect.s0, edit.rect.s1, &plan.shared_ids, u, ctx.edit->lanes());
    if (u.no_room) {
      return say(ctx, core::Severity::Warn, "NO FREE PATTERN");
    }
  }
  const unsigned pool_left = model::kMaxPatterns - static_cast<unsigned>(p.pattern_count()) - u.copies;
  if (plan.creates > pool_left) {
    return say(ctx, core::Severity::Warn, "NO FREE PATTERN");
  }
  if (song_bytes_sum(u.bytes, plan.bytes) > undo_limit(ctx)) {
    return say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: SELECT LESS");
  }
  if (unique) {
    song_unique_apply(*ctx.edit, edit.rect.s0, edit.rect.s1, &plan.shared_ids, u);
  }
  SongDone done;
  song_apply(*ctx.edit, edit, ctx.song_clip, done);
  // `done.failed` cannot be set here: the pool check above left room for UNIQUE's copies (u.copies) and for every creation of the plan, and song_apply creates exactly plan.creates patterns. Were it ever set, UNIQUE's
  // copies (and the part of the edit written so far) would stay applied as this action's one undo unit (F8 takes them back) and the strip would say NO FREE PATTERN: no rollback is built for an unreachable path.
  if (done.failed || done.writes == 0u) {
    return say(ctx, core::Severity::Warn, done.failed ? "NO FREE PATTERN" : "NOTHING TO CHANGE");
  }
  say_done(ctx, edit, done, plan, u.copies);
  return true;
}

// The rectangle's cells into the song clip (the clip exists); see copy_cells.
bool copy_rect(ActionContext& ctx, const SongRect& rect, bool& filled) noexcept {
  filled = false;
  if (static_cast<unsigned>(rect.slots()) * static_cast<unsigned>(rect.tracks()) > SongClip::kCells) {
    return say(ctx, core::Severity::Info, "TOO BIG TO COPY");  // the old clip stays
  }
  if (!song_copy_cells(*ctx.model.project, rect, *ctx.song_clip)) {
    return say(ctx, core::Severity::Info, "NOTHING TO COPY");
  }
  filled = true;
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("COPIED ");
  append_count(text, static_cast<unsigned>(rect.slots()), " SLOT", " SLOTS");
  text.append(" X ");
  append_count(text, static_cast<unsigned>(rect.tracks()), " TRACK", " TRACKS");
  return say(ctx, core::Severity::Info, text.c_str());
}

// Does this clear (from Delete or Cut, never from the TOOLS dialog, which has its own F1 APPLY) ask "CLEAR n PATTERNS?" first? Only when nothing refuses it and no shared-pattern question comes first.
bool asks_confirm(const ActionContext& ctx, const SongEdit& edit, const SongPlan& plan) noexcept {
  return edit.op == SongOp::Clear && plan.shared == 0u && plan.patterns >= 2u && plan.bytes <= undo_limit(ctx);
}

// The edit, from the keys: plans it, refuses what cannot be done, asks when it would also change a slot outside the rectangle, else writes it. `from_keys` (Delete, Cut: OS-AUDIT-69) lets a clear of 2 or more
// patterns ask first; the TOOLS dialog's apply passes false (its own F1 APPLY was the confirmation). `cut` defers the clip's fill to the answer.
bool run_edit(ActionContext& ctx, const SongEdit& edit, bool from_keys = false, bool cut = false) noexcept {
  const model::Project& p = ctx.edit->project();
  SongPlan plan;
  song_plan(p, edit, ctx.song_clip, plan);
  if (plan.patterns == 0u) {
    return say(ctx, core::Severity::Info, "NOTHING TO CHANGE");
  }
  if (plan.creates > model::kMaxPatterns - static_cast<unsigned>(p.pattern_count())) {
    return say(ctx, core::Severity::Warn, "NO FREE PATTERN");
  }
  if (plan.bytes > undo_limit(ctx)) {
    return say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: SELECT LESS");  // not even the plain edit fits: no question
  }
  if (plan.shared != 0u) {
    ask(ctx, edit, plan);
    return true;
  }
  if (from_keys && asks_confirm(ctx, edit, plan)) {  // OS-AUDIT-69: a clear over several patterns is confirmed (a single pattern stays immediate; undo is unchanged)
    ask(ctx, edit, plan, true, cut);
    return true;
  }
  return write_edit(ctx, edit, plan, false);
}

}  // namespace

// ---- the question's lifecycle ------------------------------------------------------------------------------------------------------

void song_ask_head(const SongAsk& a, AskText& out) noexcept { ask_head(a, out); }

// A question is taken back (and its text with it) when the strip no longer says it, the screen is left, a dialog comes up, or anything edited the project since it was asked. The App calls it once a pass.
void song_question_drop(ViewState& view) noexcept {
  SongAsk& a = view.song.ask;
  if (!a.live) {
    return;
  }
  AskText head;
  AskText text;
  ask_head(a, head);
  ask_text(song_ask_kind(a), head.c_str(), text);
  view.notify.dismiss("song", text.c_str());
  a = SongAsk{};
}

bool song_question_sync(ViewState& view, const Model& model) noexcept {
  const SongAsk& a = view.song.ask;
  if (!a.live) {
    return false;
  }
  const bool kept = view.screen == ScreenId::Song && view.overlay == OverlayId::None && model.project != nullptr && model.project->song_generation() == a.generation && song_question_live(view);
  if (kept) {
    return false;
  }
  song_question_drop(view);
  return true;
}

// Does this input take the question back? Any key but the answers (F1 and F2 without Shift, which are the answers while it is up) and Shift itself, an encoder turn and a pad: the input then acts as usual.
bool song_question_taken_back(const ViewState& view, const core::InputEvent& event) noexcept {
  if (!view.song.ask.live) {
    return false;
  }
  switch (event.kind) {
    case core::InputKind::Encoder: return true;
    case core::InputKind::Pad: return event.value > 0;
    case core::InputKind::Button: {
      if (event.value == 0 || event.id == static_cast<std::uint8_t>(Button::Shift)) {
        return false;
      }
      const bool answer = (event.id == static_cast<std::uint8_t>(Button::F1) || (event.id == static_cast<std::uint8_t>(Button::F2) && !view.song.ask.confirm)) && !view.shift;  // (the confirm has F1 YES only: F2 withdraws and acts)
      return !answer;
    }
    default: return false;
  }
}

// F1 ALL (arg 0) or F2 UNIQUE (arg 1), while the question is up (the field context SongAsk binds them): the edit that waited is made, over the shared patterns themselves or, for UNIQUE, over copies that the slots of the
// rectangle are first pointed at (1.5). The project is looked at again: what was asked about may no longer hold.
bool song_answer(ActionContext& ctx, core::ActionArg arg) noexcept {
  const SongAsk a = ctx.view.song.ask;  // (a copy: the question is over now)
  song_question_drop(ctx.view);
  if (!a.live || ctx.model.project == nullptr || ctx.model.project->song_generation() != a.generation) {
    return true;  // (the drop redraws the strip)
  }
  SongPlan plan;
  song_plan(ctx.edit->project(), a.edit, ctx.song_clip, plan);
  if (plan.patterns == 0u) {
    return say(ctx, core::Severity::Info, "NOTHING TO CHANGE");
  }
  if (a.cut) {  // a Cut's clear: the clip takes the cells now (a withdrawn cut never touched it); a copy that fails clears nothing
    bool filled = false;
    const bool shown = copy_rect(ctx, a.edit.rect, filled);
    if (!filled) {
      return shown;
    }
  }
  return write_edit(ctx, a.edit, plan, !a.confirm && arg == 1 && plan.shared != 0u);
}

// ---- the cells ---------------------------------------------------------------------------------------------------------------------------

// Shift + Left / Right, in the map: the tracks of the rectangle grow from where Shift was first used (arg = +-1). On PAT they do nothing. OS-SONG3 S2: on TIMELINE, where Left / Right are the slots, they grow the range of slots.
bool song_select_tracks(ActionContext& ctx, core::ActionArg arg) noexcept {
  ctx.view.song.set_tempo_focus(false);
  if (arg != 1 && arg != -1) {
    return false;
  }
  if (song_on_timeline(ctx.view)) {
    return ctx.view.song.extend(arg, SongSource(ctx.model).count());
  }
  return ctx.view.song.extend_tracks(arg, song_map_cells(ctx.model, ctx.view));
}

namespace {

// The copy of Copy and of Cut (OS-SONGSHARE S2): the rectangle's cells (no selection: the cursor's) into the song clip. `filled` says whether the clip took them; a refusal says why on the strip and leaves the old clip.
bool copy_cells(ActionContext& ctx, bool& filled) noexcept {
  filled = false;
  SongRect rect;
  if (!rectangle(ctx, false, rect) || ctx.song_clip == nullptr) {
    return true;  // (said)
  }
  return copy_rect(ctx, rect, filled);
}

}  // namespace

// Copy in the map: the rectangle's cells (no selection: the cursor's) into the song clip, each the steps of that track of the slot's pattern up to the track's length.
bool song_cells_copy(ActionContext& ctx) noexcept {
  bool filled = false;
  return copy_cells(ctx, filled);
}

// Shift + Delete in the map (OS-SONGSHARE S2, D1): a track cut. The clip is filled at the press, as Copy does, and then the rectangle is cleared by Delete's rules, the question of 1.6 included (another key at the question
// leaves the clip filled and the cells alone). A copy that was refused (TOO BIG TO COPY) clears nothing: a cut never loses what it could not keep.
// OS-AUDIT-69: when the clear asks "CLEAR n PATTERNS?" the clip is filled by F1 YES, not at the press: a withdrawn cut leaves the earlier copy in the clip.
bool song_cells_cut(ActionContext& ctx) noexcept {
  SongRect rect;
  if (!rectangle(ctx, false, rect)) {
    return true;  // (said)
  }
  SongEdit edit;
  edit.op = SongOp::Clear;
  edit.rect = rect;
  edit.elements = static_cast<std::uint8_t>(model::edit::kElAll);
  SongPlan plan;
  song_plan(ctx.edit->project(), edit, ctx.song_clip, plan);
  if (ctx.song_clip != nullptr && plan.patterns != 0u && asks_confirm(ctx, edit, plan) &&
      static_cast<unsigned>(rect.slots()) * static_cast<unsigned>(rect.tracks()) <= SongClip::kCells) {
    return run_edit(ctx, edit, true, true);  // (asks; the too-big copy is refused below, as before)
  }
  bool filled = false;
  const bool shown = copy_cells(ctx, filled);
  if (!filled) {
    return shown;
  }
  return song_cells_clear(ctx) || shown;
}

// Shift + Copy in the map: with no selection the clip's top-left lands on the cursor's cell (what runs past the last slot or track is dropped, never added); with a selection the clip is tiled to fill the rectangle (Q3).
bool song_cells_paste(ActionContext& ctx) noexcept {
  if (ctx.song_clip == nullptr || ctx.song_clip->empty()) {
    return say(ctx, core::Severity::Info, "NOTHING COPIED");
  }
  SongRect rect;
  if (!rectangle(ctx, false, rect)) {
    return true;  // (said)
  }
  const SongView& sv = ctx.view.song;
  if (!selected(sv, rect)) {
    const int count = static_cast<int>(ctx.model.project->song_length());
    const int cells = song_map_cells(ctx.model, ctx.view);
    rect.s1 = clamp_int(rect.s0 + ctx.song_clip->rows() - 1, rect.s0, count - 1);
    rect.t1 = clamp_int(rect.t0 + ctx.song_clip->cols() - 1, rect.t0, cells - 1);
  }
  SongEdit edit;
  edit.op = SongOp::Paste;
  edit.rect = rect;
  return run_edit(ctx, edit);
}

// Delete in the map: every element of the rectangle's tracks in those patterns goes, the track lengths stay.
bool song_cells_clear(ActionContext& ctx) noexcept {
  SongRect rect;
  if (!rectangle(ctx, false, rect)) {
    return true;  // (said)
  }
  SongEdit edit;
  edit.op = SongOp::Clear;
  edit.rect = rect;
  edit.elements = static_cast<std::uint8_t>(model::edit::kElAll);
  return run_edit(ctx, edit, true);
}

// F5 UNIQUE (1.5). With no range of slots: the cursor slot's pattern, if other slots hold it too, is copied to the first free number and the slot pointed at the copy. With a range: one copy for each pattern the range
// and the slots outside it both hold, the range's slots pointed at it (slots that shared it keep sharing the copy).
bool song_unique(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.song.set_tempo_focus(false);  // (as the slot's pattern keys F5 / F6 were: the encoder is back on the slot)
  SongRect rect;
  if (!rectangle(ctx, true, rect)) {
    return true;  // (said)
  }
  const model::Project& p = ctx.edit->project();
  const bool one = !ctx.view.song.selecting();
  SongUnique u;
  song_unique_plan(p, rect.s0, rect.s1, nullptr, u, ctx.edit->lanes());
  if (u.no_room) {
    return say(ctx, core::Severity::Warn, "NO FREE PATTERN");
  }
  core::FixedString<core::kNotifyTextChars> text;
  if (u.copies == 0u) {
    if (one) {
      text.assign("PATTERN ");
      const unsigned id = p.order(static_cast<std::uint32_t>(rect.s0));
      append_dec(text, id, id > 99u ? 3u : 2u);
      text.append(" IS ONLY IN THIS SLOT");
    } else {
      text.assign("NOTHING IS SHARED");
    }
    return say(ctx, core::Severity::Info, text.c_str());
  }
  if (u.bytes > undo_limit(ctx)) {
    return say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: SELECT LESS");
  }
  const unsigned from = u.first_from;
  song_unique_apply(*ctx.edit, rect.s0, rect.s1, nullptr, u);
  if (one) {
    text.assign("SLOT ");
    append_dec(text, static_cast<unsigned>(rect.s0), rect.s0 > 99 ? 3u : 2u);
    text.append(": PATTERN ");
    append_dec(text, u.first_to, u.first_to > 99u ? 3u : 2u);
    text.append(" (A COPY OF ");
    append_dec(text, from, from > 99u ? 3u : 2u);
    text.append(")");
  } else {
    text.assign("UNIQUE: ");
    append_count(text, u.copies, " COPY", " COPIES");
  }
  return say(ctx, core::Severity::Info, text.c_str());
}

// ---- F6 TOOLS ----------------------------------------------------------------------------------------------------------------------------

namespace {

// The instrument of the first note in the cursor's cell (the cursor slot, the cursor's track, or every track on PAT), else 1: the TO row's first value.
unsigned first_instrument(const ActionContext& ctx, const SongRect& rect) noexcept {
  const model::Project& p = *ctx.model.project;
  const model::Pattern* pat = p.pattern(p.order(static_cast<std::uint32_t>(rect.s0)));
  if (pat == nullptr) {
    return 1;
  }
  const int t0 = ctx.view.song.in_map() ? ctx.view.song.track() : 0;
  const int t1 = ctx.view.song.in_map() ? ctx.view.song.track() : static_cast<int>(p.active_tracks()) - 1;
  for (int t = t0; t <= t1; ++t) {
    for (unsigned k = 0; k < pat->track_length[t]; ++k) {
      const model::Step& s = pat->steps[t][k];
      if (s.note != model::kNoteEmpty && s.instr >= 1u && s.instr <= model::edit::kInstrumentSlots) {
        return s.instr;
      }
    }
  }
  return 1;
}

// A build of new slots and patterns (FORM, BRIDGE; song_build.h) from its plan, as the caller's one undo unit: the refusals first (the planner's, then the unit against the ceiling), nothing written when one fires; then
// `apply` writes the copies and the slots, the cursor goes to the first new slot with the selection collapsed, and the strip says what was made. `name` is the label of the result (`FORM POP`, `BRIDGE`).
bool run_build(ActionContext& ctx, const SongBuildPlan& plan, SongBuildKind kind, const char* name, bool (*apply)(model::Editor&, const SongBuildPlan&) noexcept) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  if (plan.info.refusal != SongBuildRefusal::None) {
    song_build_text(kind, plan.info, text);
    return say(ctx, core::Severity::Warn, text.c_str());
  }
  if (plan.bytes > undo_limit(ctx)) {
    return say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: SELECT LESS");
  }
  if (!apply(*ctx.edit, plan)) {
    // (the planner checked the pool and the list: a refusal here is a bug; what was written stays as this action's one unit, F8 takes it back, as run_edit's unreachable failure)
    return say(ctx, core::Severity::Warn, "NO FREE PATTERN");
  }
  SongView& sv = ctx.view.song;
  sv.leave_edit();
  (void)sv.set(static_cast<int>(plan.at), static_cast<int>(ctx.edit->project().song_length()));
  text.assign(name);
  text.append(": ");
  append_count(text, plan.info.slots, " SLOT", " SLOTS");
  text.append(", ");
  append_count(text, plan.info.copies, " NEW PATTERN", " NEW PATTERNS");
  return say(ctx, core::Severity::Info, text.c_str());
}

}  // namespace

// F6: the dialog over the rectangle (no selection: the cursor's cell; on PAT the cursor slot's tracks, all of them). Its rows are session state (the Fill dialog's rule).
bool song_tools_open(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || v.screen != ScreenId::Song) {
    return false;
  }
  v.song.set_tempo_focus(false);
  if (ctx.session == nullptr || ctx.session->document() == nullptr) {
    return say(ctx, core::Severity::Info, "NO PROJECT");
  }
  SongRect rect;
  if (!rectangle(ctx, false, rect)) {
    return true;  // (said)
  }
  SongToolsView& t = v.tools;
  t.rect = rect;
  t.whole = !v.song.in_map() || !(v.song.selecting() || v.song.track_selecting());  // (EXPORT WAV: the cursor's cell alone is no choice of tracks)
  t.patterns = static_cast<std::uint8_t>(song_distinct(*ctx.model.project, rect));  // (the patterns that exist: the tools skip the others, D10, so the footer counts what the result will)
  t.instrument = static_cast<std::uint8_t>(first_instrument(ctx, rect));
  song_tools_refresh(t, *ctx.model.project);
  t.row = 0;
  v.overlay = OverlayId::SongTools;
  return true;
}

bool songtools_move(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::SongTools || arg == 0) {
    return false;
  }
  const int to = clamp_int(static_cast<int>(ctx.view.tools.row) + (arg < 0 ? -1 : 1), 0, static_cast<int>(song_tools_rows(ctx.view.tools)) - 1);
  const bool changed = to != ctx.view.tools.row;
  ctx.view.tools.row = static_cast<std::uint8_t>(to);
  return changed;
}

bool songtools_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::SongTools) {
    return false;
  }
  SongToolsView& t = ctx.view.tools;
  const bool changed = song_tools_step(t, static_cast<SongToolsRow>(t.row < song_tools_rows(t) ? t.row : 0u), clamp_int(arg, -1000, 1000));
  if (changed && ctx.model.project != nullptr) {
    song_tools_refresh(t, *ctx.model.project);  // (the footer is the plan)
  }
  return changed;
}

// Shift + encoder: each detent is the row's coarse step (AMOUNT 12 semitones, TO 8 instruments; the OP and WHAT rows have a coarse step of 1).
bool songtools_coarse(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::SongTools) {
    return false;
  }
  SongToolsView& t = ctx.view.tools;
  const SongToolsRow row = static_cast<SongToolsRow>(t.row < song_tools_rows(t) ? t.row : 0u);
  const bool changed = song_tools_step(t, row, clamp_int(arg, -1000, 1000) * static_cast<int>(song_tools_desc(t, row).coarse));
  if (changed && ctx.model.project != nullptr) {
    song_tools_refresh(t, *ctx.model.project);
  }
  return changed;
}

// F1 APPLY. The dialog closes first (the notice of the result is the strip's, which waits behind a modal overlay), then the tool runs over the rectangle it was opened on, with the question of 1.6 first when it must.
bool songtools_apply(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::SongTools) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  const SongToolsView& t = ctx.view.tools;
  SongEdit edit;
  edit.rect = t.rect;
  switch (t.tool) {
    case SongTool::Transpose:
      if (t.amount == 0) {
        return say(ctx, core::Severity::Info, "NOTHING TO DO");
      }
      edit.op = SongOp::Transpose;
      edit.amount = t.amount;
      break;
    case SongTool::Instrument:
      edit.op = SongOp::Instrument;
      edit.instrument = t.instrument;
      break;
    case SongTool::Clear:
      edit.op = SongOp::Clear;
      edit.elements = song_tools_elements(t.what);
      break;
    case SongTool::ExportWav: {  // OS-SONGSHARE S5: no edit; the slots go to a WAV file, with the rectangle's tracks (every track, the project's own mixer, with nothing selected in the map and on PAT)
      std::uint16_t mask = audio::OfflineRequest::kAllTracks;
      if (!t.whole) {
        mask = 0;
        for (int k = clamp_int(t.rect.t0, 0, 16); k <= t.rect.t1 && k < 16; ++k) {  // (k is in 0..15 in the body: a negative t0 would make the shift undefined)
          mask = static_cast<std::uint16_t>(mask | (1u << k));
        }
        if (mask == 0u) {  // 0 is kAllTracks (no choice): a rectangle with no track in 0..15 must not become every track
          return say(ctx, core::Severity::Info, "NO TRACK TO EXPORT");
        }
      }
      return export_slots(ctx, static_cast<unsigned>(t.rect.s0), static_cast<unsigned>(t.rect.s1), mask);
    }
    case SongTool::Form: {  // OS-SONG-BUILD: the slots of the form after the rectangle, with the copies they need (song_build.h)
      SongBuildPlan plan;
      song_form_plan(ctx.edit->project(), t.rect, t.form, t.drums, plan, ctx.edit->lanes());
      core::FixedString<core::kNotifyTextChars> name;
      name.assign("FORM ");
      name.append(song_form_name(t.form));
      return run_build(ctx, plan, SongBuildKind::Form, name.c_str(), song_form_apply);
    }
    case SongTool::Bridge: {  // OS-SONG-BRIDGE: N new slots between A (the rectangle's first slot) and the slot after it
      SongBuildPlan plan;
      song_bridge_plan(ctx.edit->project(), t.rect, t.bridge_slots, plan);
      return run_build(ctx, plan, SongBuildKind::Bridge, "BRIDGE", song_bridge_apply);
    }
  }
  return run_edit(ctx, edit);
}

}  // namespace o3os::app
