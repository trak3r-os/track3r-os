// SPDX-License-Identifier: GPL-3.0-or-later
// The F7 verb menu's actions (OS-PTOOLS2 S3, docs/specs/OS-PTOOLS2.md section 3.1 point 2 and section 3.2, mock-up A): F7 on Pattern opens the menu (verb.open), a pad 1-16 in it
// picks a verb directly (verb.pick), F1 runs the verb under the cursor (verb.run) and F8 (ui.back) closes it. The rows are the constexpr table of app/verb_menu.h; a verb that
// exists on this head runs what it runs today (the same action its own key runs). NEW COMMUNITY OS BEHAVIOUR.
//
// OS-PTOOLS2 S3b: the five S4 verbs are wired too — INTERPOLATE, ROTATE UP / DOWN parameterless and RANDOMISE / HUMANISE with the Plan's fixed default amounts (no dialog in
// this slice): each runs model::verbs::apply_block over the selection's block as ONE undo unit, refused the same way the other block verbs are, and each pick records the
// last verb so F6 REPEAT (tools.repeat) runs it again.
#include "action_handlers.h"
#include "pattern_edit_util.h"

#include <o3os/app/actions.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/session.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/text_format.h>
#include <o3os/app/verb_menu.h>
#include <o3os/app/view_state.h>
#include <o3os/model/pattern_verbs.h>
#include <o3os/model/step_edit.h>

#include <cstdint>

namespace o3os::app {

namespace {

// Runs the verb of entry `index`, the pad's pick. The menu closes first, so the verb's notices are the strip's (which waits behind a modal overlay) and its dialog (FILL) or
// list (APPS >) opens over the bare screen; a verb that is not built yet changes nothing and leaves the menu up, only the cursor moves to it.
bool run_verb(ActionContext& ctx, unsigned index) noexcept {
  const VerbEntry& e = kVerbEntries[index];
  ViewState& v = ctx.view;
  v.verb_menu.cursor = static_cast<std::uint8_t>(index);  // the menu remembers its last verb
  if (!e.built) {
    return true;  // dim: nothing runs
  }
  v.overlay = OverlayId::None;
  ctx.focus = true;  // the menu is closed: the base screen owns the input again, and the verb's actions are the Pattern's own (a kStep action needs the focus)
  switch (e.verb) {
    case Verb::Transpose:  // OS-PTOOLS2 S2: the TOOLS dialog (the OP row, AMOUNT, the footer over the block), not the one-step encoder verb; F1 APPLY is the one undo unit
    case Verb::Instrument:  // the same, opening on INSTRUMENT
      if (!pe::edit_enabled(ctx)) {
        return pe::say(ctx, core::Severity::Info, pe::kEditOff);
      }
      (void)dispatch(action_registry(), core::action_id("ptool.open"), static_cast<core::ActionArg>(e.verb), ctx);
      return true;
    case Verb::Fill:  // the F4 dialog, as the key runs it (it says its own refusals: FILL: TURN EDIT ON, NO PROJECT)
      (void)dispatch(action_registry(), core::action_id("fill.open"), 0, ctx);
      return true;
    case Verb::Invert:  // pad 9: pattern.invert, one undo unit (it says SELECT A BLOCK TO INVERT / TURN EDIT ON itself)
      (void)dispatch(action_registry(), core::action_id("pattern.invert"), 0, ctx);
      return true;
    case Verb::Duplicate:
    case Verb::Expand:
    case Verb::Shrink:  // as the TOOLS list's rows run them (pattern.op 0, 1, 2), one undo unit
      if (!pe::edit_enabled(ctx)) {
        (void)pe::say(ctx, core::Severity::Info, pe::kEditOff);
      } else {
        const core::ActionArg op = static_cast<core::ActionArg>(e.verb) - static_cast<core::ActionArg>(Verb::Duplicate);
        (void)dispatch(action_registry(), core::action_id("pattern.op"), op, ctx);
      }
      return true;
    case Verb::Clear:  // the Delete key's clear of the block (step.clear, arg 0: the block's columns), one undo unit
      if (!pe::edit_enabled(ctx)) {
        return pe::say(ctx, core::Severity::Info, pe::kEditOff);
      }
      (void)dispatch(action_registry(), core::action_id("step.clear"), 0, ctx);
      return true;
    case Verb::ExportSelection:  // as the TOOLS list's row (its own refusals say why on the strip)
      return export_selection(ctx, 0);
    case Verb::ExportPattern:
      return export_pattern(ctx, 0);
    case Verb::Apps:  // the menu's APPS page (OS-PTOOLS2 S6b): the OS's own entries and the tool apps, in the panel
      return tools_open(ctx, 0);
    case Verb::Interpolate:  // OS-PTOOLS2 S3b: pad 4, one undo unit over the block (model::verbs::apply_block)
      if (!pe::edit_enabled(ctx)) {
        return pe::say(ctx, core::Severity::Info, pe::kEditOff);
      }
      (void)dispatch(action_registry(), core::action_id("verb.interpolate"), 0, ctx);
      return true;
    case Verb::Randomise:  // pad 5: the fixed default AMOUNT / CHANCE / SEED (no dialog in this slice), one undo unit
      if (!pe::edit_enabled(ctx)) {
        return pe::say(ctx, core::Severity::Info, pe::kEditOff);
      }
      (void)dispatch(action_registry(), core::action_id("verb.randomise"), 0, ctx);
      return true;
    case Verb::Humanise:  // pad 6: the fixed default TIME / VOL / SEED (no dialog in this slice), one undo unit
      if (!pe::edit_enabled(ctx)) {
        return pe::say(ctx, core::Severity::Info, pe::kEditOff);
      }
      (void)dispatch(action_registry(), core::action_id("verb.humanise"), 0, ctx);
      return true;
    case Verb::RotateUp:  // pad 7: one undo unit over the block
      if (!pe::edit_enabled(ctx)) {
        return pe::say(ctx, core::Severity::Info, pe::kEditOff);
      }
      (void)dispatch(action_registry(), core::action_id("verb.rotate_up"), 0, ctx);
      return true;
    case Verb::RotateDown:  // pad 8
      if (!pe::edit_enabled(ctx)) {
        return pe::say(ctx, core::Severity::Info, pe::kEditOff);
      }
      (void)dispatch(action_registry(), core::action_id("verb.rotate_down"), 0, ctx);
      return true;
  }
  return false;
}

// OS-PTOOLS2 S3b: one of the five S4 verbs (model/pattern_verbs.h) as a block verb. The target is the selection's block (the cursor step when there is no range);
// INTERPOLATE and RANDOMISE ramp the block's own column (a whole-step block ramps VOL, spec 3.2), ROTATE moves the block's fields (whole steps, or the block's
// columns), RANDOMISE and HUMANISE take the Plan's fixed default amounts with the next seed (no dialog in this slice). The run is ONE undo unit (the dispatcher's group
// names the action; apply_block's own group nests inside it). Refused the same way the other block verbs are refused: outside Edit the precondition and run_verb say
// TURN EDIT ON, with no document the action says NO PROJECT, a pattern the project lacks is empty (PATTERN IS EMPTY, as pattern.op), and a track whose part of the block
// starts past its end is left alone and counted (PAST TRACK END when nothing changed).
bool run_block_verb(ActionContext& ctx, core::ActionId id, model::verbs::Verb verb, const char* done) noexcept {
  ViewState& v = ctx.view;
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  model::verbs::Plan plan;
  plan.verb = verb;
  if (verb == model::verbs::Verb::Interpolate || verb == model::verbs::Verb::Randomise) {
    const Column col = b.whole_steps() ? Column::Vol : static_cast<Column>(b.col0);  // the block's first column; a whole-step block ramps VOL
    plan.column = static_cast<model::verbs::Column>(static_cast<std::uint8_t>(col));
  }
  if (verb == model::verbs::Verb::RotateUp || verb == model::verbs::Verb::RotateDown) {
    plan.elements = static_cast<std::uint8_t>(b.whole_steps() ? model::edit::kElMask : pe::column_elements(b));
  }
  if (verb == model::verbs::Verb::Randomise || verb == model::verbs::Verb::Humanise) {
    v.verb_menu.seed = v.verb_menu.seed >= model::verbs::kMaxSeed ? 1u : static_cast<std::uint16_t>(v.verb_menu.seed + 1u);  // each run draws the next seed, so REPEAT re-rolls
    plan.seed = v.verb_menu.seed;
  }
  const model::verbs::Block blk{static_cast<std::uint32_t>(b.track0), static_cast<std::uint32_t>(b.track1 - b.track0 + 1), static_cast<std::uint32_t>(b.row0),
                                static_cast<std::uint32_t>(b.row1 - b.row0 + 1)};
  const model::verbs::BlockResult r = model::verbs::apply_block(*ctx.edit, id, 0, v.edit.pattern, blk, plan);
  if (r.status == model::Status::NotFound) {
    return pe::say(ctx, core::Severity::Info, "PATTERN IS EMPTY");
  }
  if (r.status != model::Status::Ok) {
    return pe::say(ctx, core::Severity::Info, "REFUSED");
  }
  if (r.count.changed == 0u) {
    return pe::say(ctx, core::Severity::Info, r.refused > 0u ? "PAST TRACK END" : "NOTHING TO CHANGE");
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign(done);
  text.append(": ");
  append_uint(text, r.count.changed);
  text.append(r.count.changed == 1u ? " STEP" : " STEPS");
  return pe::say(ctx, core::Severity::Info, text.c_str());
}

}  // namespace

// The menu's TRANSPOSE and INSTRUMENT rows, registered as verb.transpose / verb.instrument (run through dispatch, one undo unit each). (OS-PTOOLS2 S3): the block's encoder verbs, but as BLOCK verbs — a block that does not cover
// whole steps is refused unless the cursor is on the verb's own column (the encoder fires only there), and a single step takes the
// multi-cell rule, so a single EMPTY step is never given a note (edit_selection's multi_cell). One undo unit.
// OS-PTOOLS2 S2: since this slice the menu's rows open the TOOLS dialog instead (run_verb dispatches ptool.open); these two stay registered for the keymap files and the
// tests, and run the one-step encoder verb as before.
bool verb_transpose(ActionContext& ctx, core::ActionArg arg) noexcept {
  const ViewState& v = ctx.view;
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  if (!b.whole_steps() && v.pattern.column() != Column::Note) {
    return pe::say(ctx, core::Severity::Info, "TRANSPOSE: WHOLE STEPS OR THE NOTE COLUMN");
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const model::edit::Rule& r, model::edit::Last& l, void* p) { model::edit::note_delta(s, *static_cast<const core::ActionArg*>(p), r, l); }, &arg, true);
  return pe::finish_entry(ctx, e, false);
}

bool verb_instrument(ActionContext& ctx, core::ActionArg arg) noexcept {
  const ViewState& v = ctx.view;
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  if (!b.whole_steps() && v.pattern.column() != Column::Instr) {
    return pe::say(ctx, core::Severity::Info, "INSTRUMENT: WHOLE STEPS OR THE INSTR COLUMN");
  }
  const pe::Edited e = pe::edit_selection(
      ctx, [](model::Step& s, const model::edit::Rule& r, model::edit::Last& l, void* p) { model::edit::instrument_delta(s, *static_cast<const core::ActionArg*>(p), r, l); }, &arg, true);
  return pe::finish_entry(ctx, e, false);
}

// OS-PTOOLS2 S3b: the five S4 verbs of the menu (pads 4..8), each one undo unit over the selection's block (run_block_verb above).
bool verb_interpolate(ActionContext& ctx, core::ActionArg) noexcept { return run_block_verb(ctx, core::action_id("verb.interpolate"), model::verbs::Verb::Interpolate, "INTERPOLATED"); }

bool verb_randomise(ActionContext& ctx, core::ActionArg) noexcept { return run_block_verb(ctx, core::action_id("verb.randomise"), model::verbs::Verb::Randomise, "RANDOMISED"); }

bool verb_humanise(ActionContext& ctx, core::ActionArg) noexcept { return run_block_verb(ctx, core::action_id("verb.humanise"), model::verbs::Verb::Humanise, "HUMANISED"); }

bool verb_rotate_up(ActionContext& ctx, core::ActionArg) noexcept { return run_block_verb(ctx, core::action_id("verb.rotate_up"), model::verbs::Verb::RotateUp, "ROTATED"); }

bool verb_rotate_down(ActionContext& ctx, core::ActionArg) noexcept { return run_block_verb(ctx, core::action_id("verb.rotate_down"), model::verbs::Verb::RotateDown, "ROTATED"); }

// F7 on Pattern. The same refusals as the TOOLS list's key had (OS-H3A): refused silently under an overlay or on another screen, with no document open there is nothing for the verbs to run on,
// and refused only in LIVE while playing (OS-PTOOLS2 S1, below). The menu remembers its last verb.
bool verb_open(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || v.screen != ScreenId::Pattern) {
    return false;
  }
  // OS-PTOOLS2 S3b: a pad ran while F7 was held (verb.hold_pad set the mark) and F7 was released before the Hold fired: the Tap that follows is not a
  // menu open — the verb already ran. Ignore that one Tap and clear the mark.
  if (v.verb_menu.hold_pad_ran) {
    v.verb_menu.hold_pad_ran = false;
    return false;
  }
  // OS-PTOOLS2 S1 (maintainer decision Q5): the menu opens in STEP and in WATCH while the transport plays, because a verb's target is the block or the cursor step, which the user's own arrows and
  // Shift set; it is refused only in LIVE, where the playhead holds the cursor (it said STOP PLAYING TO OPEN TOOLS in WATCH too, OS-BUG-TOOLSPLAY). tools_open (the APPS > row) keeps the same rule.
  if (pattern_editable(v) && !v.edit.rec_free && v.play.playing) {  // LIVE: Edit following the playhead (OS-RECMODE2)
    return pe::say(ctx, core::Severity::Info, "THE PLAYHEAD HOLDS THE CURSOR IN LIVE");
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {  // the verbs edit the project: with none open (the demo) there is nothing to run them on
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  v.overlay = OverlayId::VerbMenu;  // (OS-PTOOLS2 S3p: PatternView::draw narrows and scrolls the grid beside the panel; the stored window is not moved)
  v.verb_menu.apps = false;         // the grid, not the APPS page (S6b)
  return true;
}

// OS-PTOOLS2 S3b: F7 held (the same held-key + pad grammar as F1 PATTERN, but without opening the menu). The hold itself opens nothing — it makes the tap wait (a tap of F7 still opens the
// menu) and refuses exactly like the menu's open does: in LIVE the strip says THE PLAYHEAD HOLDS THE CURSOR IN LIVE, with no document NO PROJECT. The pads under the held key are bound
// separately (verb.hold_pad, tap pad + F7 in bindings.cpp).
bool verb_hold(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || v.screen != ScreenId::Pattern) {
    return false;
  }
  v.verb_menu.hold_pad_ran = false;  // OS-PTOOLS2 S3b: a fresh F7 press: no stale pad mark survives into this hold
  if (pattern_editable(v) && !v.edit.rec_free && v.play.playing) {  // LIVE, as verb_open
    return pe::say(ctx, core::Severity::Info, "THE PLAYHEAD HOLDS THE CURSOR IN LIVE");
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  return true;  // nothing to open: the pads under the held F7 run the verbs
}

// The release of a held F7 after its Hold fired: ends the hold, clears the pad mark (a pad that ran under the hold must not eat the next Tap of F7).
bool verb_hold_end(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::Pattern) {
    return false;
  }
  ctx.view.verb_menu.hold_pad_ran = false;
  return true;
}

// OS-PTOOLS2 S3b: F7 held + a pad 1-16 (arg = the pad's index 0..15): run the verb of that pad WITHOUT opening the menu — the same run_verb a pick in the open menu makes. A
// parameterless verb (INVERT, INTERPOLATE, ROTATE UP / DOWN, the length ops, CLEAR) runs at once; a pad of a verb with parameters opens what that verb opens today (FILL's dialog,
// APPS >'s list; RANDOMISE / HUMANISE run with the fixed default amounts, no dialog in this slice). Refused like the menu in LIVE.
bool verb_hold_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || v.screen != ScreenId::Pattern || arg < 0 || static_cast<unsigned>(arg) >= kVerbCount) {
    return false;
  }
  v.verb_menu.hold_pad_ran = true;  // OS-PTOOLS2 S3b: the Tap of F7 that may follow this pad (released before the Hold) is not a menu open
  if (pattern_editable(v) && !v.edit.rec_free && v.play.playing) {  // LIVE, as verb_open
    return pe::say(ctx, core::Severity::Info, "THE PLAYHEAD HOLDS THE CURSOR IN LIVE");
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  const bool ran = run_verb(ctx, static_cast<unsigned>(arg));
  if (v.overlay != OverlayId::None) {
    v.verb_menu.hold_pad_ran = false;  // the verb opened a modal (FILL's dialog, APPS >'s list): that overlay captures the F7 release, so the mark must not eat a later Tap
  }
  return ran;
}

// A pad of the menu (arg = the pad's index, 0..15 = pads 1-16): pick the verb and run it.
bool verb_pick(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::VerbMenu || ctx.view.verb_menu.apps || arg < 0 || static_cast<unsigned>(arg) >= kVerbCount) {  // (the APPS page has no pad picks)
    return false;
  }
  return run_verb(ctx, static_cast<unsigned>(arg));
}

// F1: run the verb under the cursor; on the APPS page, the entry under its cursor (tools_run).
bool verb_run(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::VerbMenu) {
    return false;
  }
  if (ctx.view.verb_menu.apps) {
    return tools_run(ctx, 0);
  }
  return run_verb(ctx, ctx.view.verb_menu.cursor);
}

// F6 in the menu, on the grid and on the APPS page (OS-TOOLSDEDUP; it was the APPS page's EVENTS row): the menu closes first (the strip waits behind a modal overlay) so that events_open's own
// refusals (STOP PLAYING TO OPEN EVENTS, PATTERN IS EMPTY) say why on the strip, then the EVENTS list opens over the grid pane.
bool verb_events(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::VerbMenu) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  ctx.view.modules.tools_msg = "";
  return events_open(ctx, 0);
}

}  // namespace o3os::app
