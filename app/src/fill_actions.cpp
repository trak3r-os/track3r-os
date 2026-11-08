// SPDX-License-Identifier: GPL-3.0-or-later
// The Fill dialog's actions (OS-PTOOLS part 1; docs/specs/OS-PTOOLS.md). F4 opens the dialog (view state: the rows are FillView, fill_view.h), Up / Down and Left / Right / the encoder
// change them, F1 fills and F8 closes. (Shift + Fx repeated the last fill until OS-QJUMP: pattern.fill is registered with no key.) A fill is ONE undo unit: it walks the tracks of the selection (OS-PTOOLS2 S3c: the selection is the target), builds each track's range in a buffer
// (model::fill::apply, the generators of model/pattern_fill.h) and writes the changed range through the Editor with set_steps, the pattern created in that unit if it does not exist
// (as the other edit actions). NEW COMMUNITY OS BEHAVIOUR.
#include "pattern_edit_util.h"

#include <o3os/app/pattern_data.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/session.h>
#include <o3os/app/text_format.h>
#include <o3os/app/verb_menu_view.h>  // OS-PTOOLS2 S2: fill.apply records itself as what F6 REPEAT runs (verb_menu.last_verb)
#include <o3os/app/view_state.h>

namespace o3os::app {

namespace {

namespace fill = model::fill;

bool can_edit(const ActionContext& ctx) noexcept { return ctx.edit != nullptr && pe::edit_enabled(ctx); }

std::uint8_t v_pattern(const ActionContext& ctx) noexcept { return ctx.view.edit.pattern; }

// The current value (the note, instrument or volume in hand): the note under the cursor when it has one, else the last note played, else 60;
// the instrument in hand; the volume under the cursor, else the first-entry value; the FX types of the cursor step. The dialog opens with FROM and TO at these (OS-FILL2 Q2).
fill::Value current_value(const ActionContext& ctx) noexcept {
  const ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  const model::Step here = src.step(v.pattern.track(), v.pattern.row());
  fill::Value value;
  value.note = static_cast<std::uint8_t>(here.note >= 1u && here.note <= 128u ? here.note : (v.edit.last_note != kNoNote ? v.edit.last_note + 1u : model::edit::kDefaultNote + 1u));
  value.instr = static_cast<std::uint8_t>(pe::clampi(v.instrument, 1, static_cast<int>(model::kMaxInstruments)));
  value.vol = here.vol <= model::kVolumeMax ? here.vol : model::edit::kFirstVolume;
  value.fx_type[0] = here.fx[0].type;
  value.fx_type[1] = here.fx[1].type;
  return value;
}

// A FROM or TO in the strip's words: a note by its name (the grid's, under MIDDLE C), the rest as a plain number.
void append_range_value(core::FixedString<core::kNotifyTextChars>& text, const fill::Plan& plan, unsigned value, int octave_shift) noexcept {
  if (plan.what == fill::What::Note) {
    char name[4];
    note_name(value, octave_shift, name);
    text.append(name);
  } else {
    append_uint(text, value);
  }
}

void say_fill(ActionContext& ctx, const FillView& f, bool whole_steps) noexcept {
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("FILL: ");
  if (whole_steps) {  // OS-AUDIT-56: a block of columns fills as whole steps (the dialog's WHAT picks the fields)
    text.append("WHOLE STEPS, ");
  }
  switch (f.plan.type) {
    case fill::Type::Every: {
      text.append("EVERY ");
      append_uint(text, f.plan.n);
      text.append(f.plan.n == 1u ? " STEP" : " STEPS");
      break;
    }
    case fill::Type::Random:
      text.append("RANDOM ");
      append_uint(text, f.plan.density);
      text.append(" %");
      break;
    case fill::Type::Euclid:
      text.append("EUCLID ");
      append_uint(text, f.plan.events);
      text.append(f.plan.events == 1u ? " EVENT" : " EVENTS");
      break;
    case fill::Type::Scale:
      text.append("SCALE RUN");
      break;
  }
  // OS-FILL2: what was written, its mode and its range ("VOLUME RAMP 127-0", "FX 1 RANDOM 0-100", "NOTE RAMP C-5 TO C-6"). Today's texts stay for a constant note, instrument or volume and for the note window of TYPE RANDOM and SCALE.
  const bool fx = f.plan.what == fill::What::Fx1 || f.plan.what == fill::What::Fx2;
  if (fill_row_used(f, FillRow::Mode) && (fx || f.plan.mode != fill::Mode::Const)) {
    char word[16];
    text.append(", ");
    text.append(word, fill_text(f, FillRow::What, word, sizeof word));
    text.append(" ");
    text.append(word, fill_text(f, FillRow::Mode, word, sizeof word));
    text.append(" ");
    const int shift = pattern_octave_shift(ctx.model);
    append_range_value(text, f.plan, f.plan.from, shift);
    if (f.plan.mode != fill::Mode::Const) {
      text.append(f.plan.what == fill::What::Note ? " TO " : "-");
      append_range_value(text, f.plan, f.plan.to, shift);
    }
  }
  (void)pe::say(ctx, core::Severity::Info, text.c_str());
}

// Fills the selection with the plan (OS-PTOOLS2 S3c: the selection is the target, as in every tool — a block, a column block, the cursor step; Shift + Up at row 1 the whole track, again the whole pattern). One undo unit (the
// dispatcher's); false when nothing was written.
bool run_fill(ActionContext& ctx, FillView& f) noexcept {
  ViewState& v = ctx.view;
  const PatternSource src(ctx.model, v.edit.pattern);
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  int t0 = b.track0;
  int t1 = b.track1;
  int from = b.row0;
  int to = b.row1;
  bool exists = src.exists();
  bool changed = false;
  int refused = 0;
  for (int t = t0; t <= t1 && t < src.tracks(); ++t) {
    const int len = src.track_length(t);
    const int r0 = from;
    const int r1 = to < len - 1 ? to : len - 1;  // the pattern's length: a step past the track's end is never written (D7)
    if (r0 > r1) {
      ++refused;
      continue;
    }
    const int n = r1 - r0 + 1;
    model::Step before[model::kMaxPatternLength];
    model::Step buf[model::kMaxPatternLength];
    for (int k = 0; k < n; ++k) {
      before[k] = buf[k] = src.step(t, r0 + k);
    }
    model::edit::Last last{static_cast<std::uint8_t>(pe::clampi(v.instrument, 1, static_cast<int>(model::kMaxInstruments))), v.edit.last_fx};
    (void)fill::apply(f.plan, buf, static_cast<unsigned>(n), f.rng, last);
    if (std::memcmp(before, buf, sizeof(model::Step) * static_cast<std::size_t>(n)) == 0) {
      continue;
    }
    if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(src.length()))) {
      return false;
    }
    changed = ctx.edit->set_steps(v.edit.pattern, static_cast<std::uint32_t>(t), static_cast<std::uint32_t>(r0), buf, static_cast<std::uint32_t>(n)) == model::Status::Ok || changed;
  }
  if (!changed) {
    return pe::say(ctx, core::Severity::Info, refused > 0 ? "PAST TRACK END" : "FILL: NOTHING TO CHANGE");
  }
  say_fill(ctx, f, v.edit.selection.active && !b.whole_steps());  // OS-AUDIT-56: a block of columns fills as whole steps (the dialog's WHAT picks the fields)
  return true;
}

constexpr const char* kNeedEdit = "FILL: TURN EDIT ON";

}  // namespace

// F4. The dialog opens on the selection as the target (OS-PTOOLS2 S3c: no SCOPE row — a block, a column block or the cursor step; the footer of the dialog names it).
bool fill_open(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, kNeedEdit);
  }
  if (ctx.session == nullptr || ctx.session->document() == nullptr) {  // the fill edits the project: with none open there is nothing to fill (the demo is read-only)
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  FillView& f = v.fill;
  if (f.row >= kFillRows) {
    f.row = 0;
  }
  f.open = current_value(ctx);  // OS-FILL2 Q2: FROM and TO start from what the cursor holds (the first open; a change of WHAT starts them again)
  if (!f.primed) {
    fill::range_default(f.plan, f.open);
    f.primed = true;
  }  // OS-TOOLSDEDUP: a reopen keeps every row of the session as it was, FROM of a constant included (it took the cursor's value again before)
  v.overlay = OverlayId::Fill;
  return true;
}

bool fill_move(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::Fill || arg == 0) {
    return false;
  }
  const int to = pe::clampi(static_cast<int>(ctx.view.fill.row) + (arg < 0 ? -1 : 1), 0, static_cast<int>(kFillRows) - 1);
  const bool changed = to != ctx.view.fill.row;
  ctx.view.fill.row = static_cast<std::uint8_t>(to);
  return changed;
}

bool fill_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::Fill) {
    return false;
  }
  FillView& f = ctx.view.fill;
  return fill_step(f, static_cast<FillRow>(f.row < kFillRows ? f.row : 0u), pe::clampi(arg, -1000, 1000));
}

// OS-SHIFTENC (Shift + encoder): each detent is the row's coarse step (N 4, DENSITY 10, FROM and TO 12 for a note and 10 for the others, the root of KEY; OS-FILL2). The other enumerated rows (WHAT, TYPE, MODE, FX TYPE) have a coarse step of 1: as the fine step.
bool fill_coarse(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::Fill) {
    return false;
  }
  FillView& f = ctx.view.fill;
  return fill_step_coarse(f, static_cast<FillRow>(f.row < kFillRows ? f.row : 0u), pe::clampi(arg, -1000, 1000));
}

// F1. The dialog closes first (the notice of the result is the strip's, which waits behind a modal overlay), then the fill runs on the selection and the rows as they were.
bool fill_apply(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::Fill) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  if (!can_edit(ctx)) {
    return pe::say(ctx, core::Severity::Info, ctx.edit == nullptr ? "NO PROJECT" : kNeedEdit);
  }
  FillView& f = ctx.view.fill;
  f.valid = true;
  const bool done = run_fill(ctx, f);
  if (done) {
    ctx.view.verb_menu.last_verb = VerbMenuView::LastVerb::Fill;  // OS-PTOOLS2 S2: what F6 REPEAT runs after a fill (the TOOLS dialog's APPLY records Tool)
  }
  f.vary_pattern = 0;
  if (done && ctx.edit != nullptr) {  // OS-PTOOLS2 S1: what F2 VARY may take back: this fill, while its pattern is as it left it
    f.vary_pattern = v_pattern(ctx);
    f.vary_generation = ctx.edit->project().pattern_generation(f.vary_pattern);
  }
  return done;
}

// F2 VARY, live while the fill draws at random (the row says so: fill_random). "Next seed, re-applied": the fill the dialog made last is taken back if it is still the newest unit of the Pattern and its pattern has not
// been touched since (so VARY never undoes anything else), then the dialog's fill is made again: the generator has moved on, so it draws again. One undo unit, as F1, and the dialog stays up, so F2 again re-rolls. With nothing to take back it is a plain F1.
bool fill_vary(ActionContext& ctx, core::ActionArg) noexcept {
  FillView& f = ctx.view.fill;
  if (ctx.view.overlay != OverlayId::Fill || !fill_random(f)) {
    return false;
  }
  const model::Editor* doc = ctx.session != nullptr ? ctx.session->document() : nullptr;
  core::ActionId newest = core::ActionId::None;
  core::ActionArg arg = 0;
  if (doc != nullptr && f.vary_pattern != 0u && f.vary_pattern == v_pattern(ctx) && doc->project().pattern_generation(f.vary_pattern) == f.vary_generation &&
      doc->journal(model::UndoArea::Pattern).newest_undo(newest, arg) && newest == core::action_id("fill.apply")) {
    const bool focus = ctx.focus;
    ctx.focus = true;  // (a modal dialog has none; edit.undo asks for it, and this one is the dialog's own take-back)
    (void)dispatch(action_registry(), core::action_id("edit.undo"), 0, ctx);
    ctx.focus = focus;
  }
  const DispatchResult res = dispatch(action_registry(), core::action_id("fill.apply"), 0, ctx);
  if (res == DispatchResult::Changed && ctx.view.overlay == OverlayId::None) {
    ctx.view.overlay = OverlayId::Fill;  // (review fold of OS-PTOOLS2 S1: fill.apply closes the dialog; VARY puts it back, so F2 again re-rolls without F4 in between)
  }
  return res == DispatchResult::Changed;
}

// No key since OS-QJUMP (it was Shift + Fx; the action stays registered, for the keymap files and the tests). Repeats the last fill of the dialog on the selection as it is now (a random fill draws again). Before any: the OS-CFGWIRE fill, the
// current note on every Nth step (N is the edit step, JUMP; 0 counts as 1) of the selection — the cursor step with nothing selected, the track or the pattern of Shift + Up at row 1 — counting from the first step of the range; the dialog's rows
// are not touched.
bool pattern_fill(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::None) {  // a modal overlay owns the input: the repeat does not run behind the Fill dialog (tools.repeat delegates here, so it checks too)
    return false;
  }
  if (!can_edit(ctx)) {  // (outside Edit it says what it needs, and where EXPORT is)
    return pe::say(ctx, core::Severity::Info, "FILL: TURN EDIT ON (EXPORT: F7 APPS >)");
  }
  ViewState& v = ctx.view;
  if (v.fill.valid) {
    return run_fill(ctx, v.fill);
  }
  FillView plain;
  plain.rng = v.fill.rng;
  plain.plan.n = static_cast<std::uint8_t>(v.edit.edit_step == 0u ? 1u : v.edit.edit_step);
  fill::range_default(plain.plan, current_value(ctx));  // (a NOTE fill: FROM is the note in hand)
  return run_fill(ctx, plain);
}

// OS-PTOOLS2 S3 (Q1): tools.repeat, the last verb again, on F6. F6 repeats the last PARAMETERISED apply, one of two: the last FILL dialog (fill.apply sets v.fill.valid and
// LastVerb::Fill; the key row reads RE FILL) or the last TOOLS dialog APPLY (ptool.apply records LastVerb::Tool with its amount; the key row reads REPEAT, pattern_soft_keys):
// the later of the two wins, on the selection as it is now, one undo unit. A parameterless menu verb — ROTATE, an export, any other pad — never becomes the repeat.
// Both are the session's: a NEW or an opened project clears them (app.cpp). With neither run yet it says NOTHING TO REPEAT and writes nothing (the OS-CFGWIRE
// every-Nth-step fill stays pattern.fill's own, keyless behaviour). Orchestrator decision 2026-11-07 (maintainer may revisit): the TOOLS APPLY is a sanctioned F6 target.
bool tools_repeat(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::None) {  // a modal overlay owns the input (pattern_fill refuses here too; checked first so the no-verb notice does not queue behind the dialog)
    return false;
  }
  if (ctx.view.verb_menu.last_verb == VerbMenuView::LastVerb::Tool) {  // OS-PTOOLS2 S2: the last APPLY of the TOOLS dialog, with the same amount
    const VerbMenuView::ToolLast& t = ctx.view.verb_menu.tool_last;
    return pattern_tools_run(ctx, t.tool, t.amount, t.instrument, t.what, true);
  }
  if (!ctx.view.fill.valid) {
    return pe::say(ctx, core::Severity::Info, "NOTHING TO REPEAT");
  }
  return pattern_fill(ctx, arg);
}

}  // namespace o3os::app
