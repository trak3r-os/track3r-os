// SPDX-License-Identifier: GPL-3.0-or-later
// The Pattern screen's TOOLS dialog (OS-PTOOLS2 S2, docs/specs/OS-PTOOLS2.md section 3.1 point 3, the S2 row of the slice plan): the menu's TRANSPOSE and INSTRUMENT rows
// (pads 1 and 2, F1 RUN, or F7 held + the pad) open it (ptool.open) over the selection's block — the block, a column block or the cursor step. Its keys: Up / Down choose a row,
// Left / Right or the encoder change the row's value (Shift + the encoder the coarse step: an octave on AMOUNT), F1 APPLY (ptool.apply) runs the tool as ONE undo unit through
// pe::edit_selection and F8 BACK (ui.back) leaves the project untouched. The rules are the Song TOOLS dialog's (song_edit.cpp's apply_tool): the multi-cell rule, so a note is never
// made from nothing and cut / fade / off steps are untouched by TRANSPOSE and INSTRUMENT. F6 REPEAT in the verb menu (tools.repeat) re-runs the last APPLY with the same amount.
#include "action_handlers.h"
#include "pattern_edit_util.h"

#include <o3os/app/actions.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_tools_view.h>
#include <o3os/app/screens/ids.h>
#include <o3os/app/session.h>
#include <o3os/app/song_tools_view.h>
#include <o3os/app/text_format.h>
#include <o3os/app/verb_menu.h>
#include <o3os/app/verb_menu_view.h>
#include <o3os/app/view_state.h>
#include <o3os/model/editor.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/pattern.h>
#include <o3os/model/project.h>
#include <o3os/model/step_edit.h>

#include <cstdint>

namespace o3os::app {

namespace {

// verb_actions.cpp opens this dialog with static_cast<core::ActionArg>(e.verb) for the TRANSPOSE and INSTRUMENT menu rows, and ptool_open reads that arg as a PTool.
static_assert(static_cast<unsigned>(PTool::Transpose) == static_cast<unsigned>(Verb::Transpose) && static_cast<unsigned>(PTool::Instrument) == static_cast<unsigned>(Verb::Instrument),
              "ptool.open's arg is a Verb value: PTool 0 and 1 must stay TRANSPOSE and INSTRUMENT");

// The instrument of the first note of the block, 0 when it holds none: the TO row's value when the dialog opens (the session's TO stays when the block has no note).
unsigned first_note_instrument(const ActionContext& ctx, const Selection::Block& b) noexcept {
  const model::Project* p = ctx.model.project;
  if (p == nullptr) {
    return 0;
  }
  const model::Pattern* pat = p->pattern(ctx.view.edit.pattern);
  if (pat == nullptr) {
    return 0;
  }
  const int tracks = static_cast<int>(p->active_tracks());
  for (int t = b.track0; t <= b.track1 && t < tracks; ++t) {
    const int r1 = b.row1 < static_cast<int>(pat->track_length[t]) - 1 ? b.row1 : static_cast<int>(pat->track_length[t]) - 1;
    for (int r = b.row0; r <= r1; ++r) {
      const model::Step& s = pat->steps[t][r];
      if (s.note >= 1u && s.note <= 128u && s.instr >= 1u && s.instr <= model::edit::kInstrumentSlots) {
        return s.instr;
      }
    }
  }
  return 0;
}

// What the tool cannot do on this block, nullptr when it can: a column block takes TRANSPOSE on the Note column and INSTRUMENT on the Instr column only, and CLEAR on none (pe::edit_selection
// ignores the columns, so a CLEAR on a column block would wipe whole steps while the footer names one column). Asked when the dialog opens and again when the tool runs.
const char* column_refusal(const ViewState& v, PTool tool, const Selection::Block& b) noexcept {
  if (b.whole_steps()) {
    return nullptr;
  }
  const Column col = v.pattern.column();
  switch (tool) {
    case PTool::Transpose:
      return col != Column::Note ? "TRANSPOSE: WHOLE STEPS OR THE NOTE COLUMN" : nullptr;
    case PTool::Instrument:
      return col != Column::Instr ? "INSTRUMENT: WHOLE STEPS OR THE INSTR COLUMN" : nullptr;
    case PTool::Clear:
      return "CLEAR: WHOLE STEPS ONLY";
  }
  return nullptr;
}

}  // namespace

// The dialog opens on the selection (arg = the OP it opens with: PTool::Transpose or PTool::Instrument). The rows are session state (Q6): AMOUNT, TO and WHAT keep their values
// between opens, TO takes the block's first note's instrument when it has one. The same refusals as the verbs it replaces: outside Edit TURN EDIT ON, with no document NO PROJECT.
bool ptool_open(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::None || v.screen != ScreenId::Pattern) {
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, pe::kEditOff);
  }
  if (ctx.session == nullptr || ctx.session->document() == nullptr) {
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  PatternToolsView& t = v.ptools;
  const PTool opened = arg == static_cast<core::ActionArg>(PTool::Instrument) ? PTool::Instrument : PTool::Transpose;
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  if (const char* refused = column_refusal(v, opened, b)) {  // refused here, not at APPLY after the user has set the rows
    return pe::say(ctx, core::Severity::Info, refused);
  }
  t.tool = opened;
  const unsigned instr = first_note_instrument(ctx, b);
  if (instr != 0u) {
    t.instrument = static_cast<std::uint8_t>(instr);
  }
  t.row = 0;
  v.overlay = OverlayId::PatternTools;  // (OS-PTOOLS2 S3p: PatternView::draw narrows and scrolls the grid beside the panel; the stored window is not moved)
  return true;
}

bool ptool_move(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::PatternTools || arg == 0) {
    return false;
  }
  const int to = pe::clampi(static_cast<int>(ctx.view.ptools.row) + (arg < 0 ? -1 : 1), 0, static_cast<int>(kPatternToolsRows) - 1);
  const bool changed = to != ctx.view.ptools.row;
  ctx.view.ptools.row = static_cast<std::uint8_t>(to);
  return changed;
}

bool ptool_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::PatternTools) {
    return false;
  }
  PatternToolsView& t = ctx.view.ptools;
  return pattern_tools_step(t, static_cast<PatternToolsRow>(t.row < kPatternToolsRows ? t.row : 0u), pe::clampi(arg, -1000, 1000));
}

// Shift + encoder: each detent is the row's coarse step (AMOUNT 12 semitones — an octave; TO 8 instruments; the OP and WHAT rows 1).
bool ptool_coarse(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.view.overlay != OverlayId::PatternTools) {
    return false;
  }
  PatternToolsView& t = ctx.view.ptools;
  const PatternToolsRow row = static_cast<PatternToolsRow>(t.row < kPatternToolsRows ? t.row : 0u);
  return pattern_tools_step(t, row, pe::clampi(arg, -1000, 1000) * static_cast<int>(pattern_tools_desc(t, row).coarse));
}

// Runs the tool over the selection's block as ONE undo unit through pe::edit_selection (the caller's unit: ptool.apply's, or tools.repeat's), with the Song TOOLS dialog's
// rules (song_edit.cpp's apply_tool: the multi-cell rule — no note is made from nothing, cut / fade / off are untouched) and the block verbs' column rules (TRANSPOSE on the
// Note column, INSTRUMENT on the Instr column, a whole-step block either). `record` keeps it as what the verb menu's F6 REPEAT runs with the same amount. The result names the target's steps.
bool pattern_tools_run(ActionContext& ctx, PTool tool, int amount, unsigned instrument, SongWhat what, bool record) noexcept {
  ViewState& v = ctx.view;
  if (!pe::edit_enabled(ctx)) {
    return pe::say(ctx, core::Severity::Info, pe::kEditOff);
  }
  if (ctx.session == nullptr || ctx.session->document() == nullptr) {
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  const Selection::Block b = v.edit.selection.block(v.pattern.row(), v.pattern.track());
  if (const char* refused = column_refusal(v, tool, b)) {
    return pe::say(ctx, core::Severity::Info, refused);
  }
  if (tool == PTool::Transpose && amount == 0) {
    return pe::say(ctx, core::Severity::Info, "NOTHING TO DO");
  }
  // OS-MIDIREC-P1 S7a, CLEAR LANE (new Community OS behaviour): CLEAR of ALL over every step of the cursor's track also clears that track's MIDI take lane (Editor::lane_clear, in
  // this action's one undo unit). A block that does not cover the whole track leaves the lane alone: the lane is the track's, not a step's. Done first: a refusal (the undo ring) changes nothing.
  bool lane_cleared = false;
  const model::LaneStore* lane_store = ctx.edit != nullptr ? ctx.edit->lanes() : nullptr;
  if (tool == PTool::Clear && what == SongWhat::All && b.whole_steps() && lane_store != nullptr) {
    const int track = v.pattern.track();
    const model::Pattern* pat = ctx.model.project->pattern(v.edit.pattern);
    if (pat != nullptr && track >= 0 && track < static_cast<int>(model::kMaxTracks) && b.track0 <= track && track <= b.track1 && b.row0 == 0 &&
        b.row1 >= static_cast<int>(pat->track_length[track]) - 1 && lane_store->lane(v.edit.pattern, static_cast<std::uint32_t>(track)).count != 0u) {
      if (ctx.edit->lane_clear(v.edit.pattern, static_cast<std::uint32_t>(track)) != model::Status::Ok) {
        return pe::say(ctx, core::Severity::Info, "TOO BIG TO UNDO: NOTHING CHANGED");
      }
      lane_cleared = true;
    }
  }
  // S7d, TRANSPOSE: the lane NOTE events of a whole-step block move by the same amount as the steps' notes (A16; CC untouched), only with a selection (a bare cursor moves its step only), made first and after a dry run, like CLEAR LANE.
  bool lane_moved = false;
  if (tool == PTool::Transpose && v.edit.selection.active && b.whole_steps() && lane_store != nullptr) {
    const PatternSource lsrc(ctx.model, v.edit.pattern);
    const int lr = pe::lanes_transpose_block(ctx, v.edit.pattern, b, lsrc, amount, pe::step_bytes(b.track1 - b.track0 + 1, static_cast<int>(model::kMaxPatternLength)));
    if (lr < 0) {
      return true;  // (said: TOO BIG TO UNDO, nothing changed)
    }
    lane_moved = lr > 0;
  }
  pe::Edited e;
  switch (tool) {
    case PTool::Transpose:
      e = pe::edit_selection(
          ctx, [](model::Step& s, const model::edit::Rule& r, model::edit::Last& l, void* p) { model::edit::note_delta(s, *static_cast<const int*>(p), r, l); }, &amount, true);
      break;
    case PTool::Instrument:
      e = pe::edit_selection(
          ctx, [](model::Step& s, const model::edit::Rule& r, model::edit::Last& l, void* p) { model::edit::instrument_set(s, *static_cast<const unsigned*>(p), r, l); }, &instrument, true);
      break;
    case PTool::Clear: {
      std::uint8_t elements = song_tools_elements(what);
      e = pe::edit_selection(
          ctx, [](model::Step& s, const model::edit::Rule&, model::edit::Last&, void* p) { model::edit::clear(s, *static_cast<const std::uint8_t*>(p)); }, &elements, true);
      break;
    }
  }
  if (!e.changed && !lane_cleared && !lane_moved) {
    if (e.failed) {
      return true;  // (said: NO ROOM FOR A NEW PATTERN)
    }
    return pe::say(ctx, core::Severity::Info, e.refused > 0 ? "PAST TRACK END" : "NOTHING TO CHANGE");
  }
  if (record) {
    v.verb_menu.last_verb = VerbMenuView::LastVerb::Tool;
    v.verb_menu.tool_last.tool = tool;
    v.verb_menu.tool_last.amount = static_cast<std::int8_t>(amount);
    v.verb_menu.tool_last.instrument = static_cast<std::uint8_t>(instrument);
    v.verb_menu.tool_last.what = what;
  }
  unsigned steps = 0;
  unsigned notes = 0;
  pattern_tools_count(ctx.model.project, v.edit.pattern, b, steps, notes);
  core::FixedString<core::kNotifyTextChars> text;
  switch (tool) {
    case PTool::Transpose:
      text.assign("TRANSPOSED: ");
      append_uint(text, steps);
      text.append(steps == 1u ? " STEP " : " STEPS ");
      text.append(amount < 0 ? "-" : "+");
      append_uint(text, static_cast<unsigned>(amount < 0 ? -amount : amount));
      text.append(" ST");
      break;
    case PTool::Instrument:
      text.assign("INSTRUMENT SET: ");
      append_uint(text, steps);
      text.append(steps == 1u ? " STEP " : " STEPS ");
      append_dec(text, instrument, 2);
      break;
    case PTool::Clear:
      text.assign("CLEARED: ");
      append_uint(text, steps);
      text.append(steps == 1u ? " STEP" : " STEPS");
      if (lane_cleared) {
        text.append(" + LANE");
      }
      break;
  }
  return pe::say(ctx, core::Severity::Info, text.c_str());
}

// F1 APPLY. The dialog closes first (the notice of the result is the strip's, which waits behind a modal overlay), then the tool runs over the selection as it then stands (the
// target is read here, not frozen when the dialog opened; the dialog is modal, so the cursor and the selection do not move under it) as this action's one undo unit.
bool ptool_apply(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (v.overlay != OverlayId::PatternTools) {
    return false;
  }
  v.overlay = OverlayId::None;
  const PatternToolsView& t = v.ptools;
  return pattern_tools_run(ctx, t.tool, t.amount, t.instrument, t.what, true);
}

}  // namespace o3os::app
