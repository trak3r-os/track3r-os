// SPDX-License-Identifier: GPL-3.0-or-later
// Whole-pattern operations (PAT-OPS, docs/specs/PAT-OPS.md; NEW COMMUNITY OS BEHAVIOUR; documented in the release notes and manual of the official firmware, not verified on a unit: OS-009:151):
//   PAT-OPS-A  pattern.op      DUPLICATE x2, EXPAND x2 and SHRINK /2 of the pattern in view (Pattern F7 TOOLS runs it, tools_run), the pure rules in model/pattern_ops.h
//   PAT-OPS-B  pattern.clip_*  the whole pattern into the clipboard and back over a chosen pattern number, with F1 held (the PATTERN chooser): Copy and Shift + Copy
// Each is ONE undo unit (the dispatcher's): the steps go through set_steps (only the span of a track that changed), the lengths through set_track_length, the name through set_pattern_name.
#include "pattern_edit_util.h"

#include <o3os/app/pattern_clip.h>
#include <o3os/app/text_format.h>
#include <o3os/model/pattern_ops.h>

namespace o3os::app {

namespace {

constexpr std::size_t kColumnBytes = model::kMaxPatternLength * sizeof(model::Step);

// The PATTERN chooser as the hold of F1 opened it: the only place the whole-pattern clip is reached (the LENGTH and JUMP choosers share the context and do nothing with Copy).
bool pattern_chooser_held(const ViewState& v) noexcept { return v.screen == ScreenId::Pattern && v.overlay == OverlayId::PatternChooser && v.edit.chooser_held; }

// The news of the whole-pattern clip. The chooser is a modal overlay, so the strip runs only after the F1 hold is over; a Copy and a paste in one hold must not queue two lines behind each other, the
// later one rewrites the earlier (upsert, a source of its own).
bool clip_say(ActionContext& ctx, const char* text) noexcept { return ctx.view.notify.upsert(core::Severity::Info, text, "clip", ViewState::kMessageMs); }

// "PATTERN 05 COPIED": the number as the strip shows it (two digits, three from 100).
void append_pattern(core::FixedString<core::kNotifyTextChars>& text, unsigned id) noexcept {
  text.append("PATTERN ");
  append_dec(text, id, id >= 100u ? 3u : 2u);
}

// The first and the last index of the steps of `a` and `b` (kMaxPatternLength of each) that differ; false when they are the same.
bool differing_span(const model::Step* a, const model::Step* b, unsigned& first, unsigned& last) noexcept {
  first = model::kMaxPatternLength;
  last = 0;
  for (unsigned i = 0; i < model::kMaxPatternLength; ++i) {
    if (std::memcmp(&a[i], &b[i], sizeof(model::Step)) != 0) {
      first = first < i ? first : i;
      last = i;
    }
  }
  return first < model::kMaxPatternLength;
}

// Writes `steps` (a whole column) over track `t` of pattern `id`: the changed span only. True if anything was written.
bool write_column(ActionContext& ctx, unsigned id, unsigned t, const model::Step* steps) noexcept {
  const model::Pattern* pat = ctx.edit->project().pattern(id);
  unsigned first = 0;
  unsigned last = 0;
  if (pat == nullptr || !differing_span(pat->steps[t], steps, first, last)) {
    return false;
  }
  return ctx.edit->set_steps(id, t, first, steps + first, last - first + 1u) == model::Status::Ok;
}

// The cursor and the block back inside what the pattern is now (a shorter one), the block over.
void settle_view(ActionContext& ctx) noexcept {
  ctx.view.edit.selection.clear();
  (void)pattern_sync(ctx.model, ctx.view);
}

}  // namespace

// PAT-OPS-A. arg = model::ops::Op (0 DUPLICATE, 1 EXPAND, 2 SHRINK). The scope is the whole pattern, every active track by its own length; with a block, the block's tracks over their whole length (Q1a).
// Any track that would pass 128 steps refuses the whole operation and nothing is written. The cursor keeps its row (clamped to the new length), the block ends. A pattern the project lacks is not made.
bool pattern_op(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.edit == nullptr || arg < 0 || arg > static_cast<core::ActionArg>(model::ops::Op::Shrink)) {
    return false;
  }
  ViewState& v = ctx.view;
  const unsigned id = v.edit.pattern;
  const model::Pattern* pat = ctx.edit->project().pattern(id);
  if (pat == nullptr) {
    return pe::say(ctx, core::Severity::Info, "PATTERN IS EMPTY");
  }
  const model::ops::Op op = static_cast<model::ops::Op>(arg);
  int t0 = 0;
  int t1 = static_cast<int>(ctx.edit->project().active_tracks()) - 1;
  bool columns = false;  // OS-AUDIT-56: a block of columns is widened to whole steps (whole tracks here); the strip says so
  if (v.edit.selection.active) {
    const Selection::Block b = v.edit.selection.block(0, 0);
    columns = !b.whole_steps();
    t0 = b.track0;
    t1 = b.track1 < t1 ? b.track1 : t1;
  }
  unsigned longest = 0;  // the longest result, which the strip names
  for (int t = t0; t <= t1; ++t) {
    const unsigned len = model::ops::result_length(op, pat->track_length[t]);
    if (len > model::kMaxPatternLength) {
      return pe::say(ctx, core::Severity::Info, "TOO LONG: 128 STEPS AT MOST");
    }
    longest = longest > len ? longest : len;
  }
  {
    // OS-MIDIREC-P1 S2 fold: every track's new length is checked against the undo ring BEFORE any write (a refusal half way would leave a column written, a length kept and tracks apart).
    std::uint8_t lengths[model::kMaxTracks];
    for (unsigned t = 0; t < model::kMaxTracks; ++t) {
      lengths[t] = pat->track_length[t];
      if (static_cast<int>(t) >= t0 && static_cast<int>(t) <= t1) {
        lengths[t] = static_cast<std::uint8_t>(model::ops::result_length(op, pat->track_length[t]));
      }
    }
    // OS-MIDIREC-P1 S7d: EXPAND and SHRINK scale the MIDI lane ticks with the rows (A16), so nothing is cut but what passes the new end after scaling; the lane records
    // (before and after, per track) replace the cut check, with the step images of the same unit. DUPLICATE only lengthens, so it keeps the cut check (which finds none).
    if (op == model::ops::Op::Duplicate) {
      if (!ctx.edit->lengths_fit_undo(id, lengths)) {
        return pe::say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: USE THE LENGTH CHOOSER");
      }
    } else {
      std::uint16_t scale_mask = 0;
      for (int t = t0; t <= t1; ++t) {
        scale_mask = static_cast<std::uint16_t>(scale_mask | (lengths[t] != pat->track_length[t] ? 1u << t : 0u));
      }
      if (!ctx.edit->lanes_scale_fit(id, scale_mask, pe::step_bytes(t1 - t0 + 1, static_cast<int>(model::kMaxPatternLength)) + model::kMaxTracks * 32u)) {
        return pe::say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: USE THE LENGTH CHOOSER");
      }
    }
  }
  bool changed = false;
  bool too_big = false;
  for (int t = t0; t <= t1; ++t) {
    model::Step out[model::kMaxPatternLength];
    unsigned out_len = 0;
    const unsigned len = pat->track_length[t];
    if (!model::ops::apply(op, pat->steps[t], len, out, out_len)) {
      continue;  // (checked above)
    }
    changed = write_column(ctx, id, static_cast<unsigned>(t), out) || changed;
    if (out_len != len) {
      if (op != model::ops::Op::Duplicate) {
        // S7d: the lane ticks follow the rows, before the length changes (A17 then has nothing left to cut); the pre-check above made it fit
        const model::Status ls = ctx.edit->lane_scale(id, static_cast<unsigned>(t), out_len, op == model::ops::Op::Expand);
        too_big = too_big || ls == model::Status::Full;
      }
      const model::Status st = ctx.edit->set_track_length(id, static_cast<unsigned>(t), out_len);
      changed = changed || st == model::Status::Ok;
      too_big = too_big || st == model::Status::Full;
    }
  }
  settle_view(ctx);
  if (!changed) {
    // OS-MIDIREC-P1 S2: the cut MIDI lane events do not fit the undo ring (the Editor changed nothing); the length chooser is where that op can be confirmed without undo.
    return pe::say(ctx, too_big ? core::Severity::Warn : core::Severity::Info, too_big ? "TOO BIG TO UNDO: USE THE LENGTH CHOOSER" : "NOTHING TO CHANGE");
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign(op == model::ops::Op::Duplicate ? "DUPLICATED: " : (op == model::ops::Op::Expand ? "EXPANDED: " : "SHRUNK: "));
  append_uint(text, longest);
  text.append(longest == 1u ? " STEP" : " STEPS");
  if (columns) {
    text.append(" - WHOLE STEPS");
  }
  return pe::say(ctx, core::Severity::Info, text.c_str());
}

// PAT-OPS-B. Copy with F1 held: the chooser's pattern whole (16 tracks x 128 steps, the 16 lengths, the name) into the clipboard. Works in every mode (it changes nothing). A later Copy of the grid makes a block clip again.
bool pattern_clip_copy(ActionContext& ctx, core::ActionArg) noexcept {
  const ViewState& v = ctx.view;
  if (!pattern_chooser_held(v) || ctx.clip == nullptr || ctx.model.project == nullptr) {
    return false;
  }
  const model::Pattern* pat = ctx.model.project->pattern(v.edit.pattern);
  if (pat == nullptr || !ctx.clip->begin(static_cast<int>(model::kMaxTracks), static_cast<int>(model::kMaxPatternLength))) {
    return clip_say(ctx, "NOTHING TO COPY");  // (a pattern the project lacks has nothing in it)
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    std::memcpy(ctx.clip->column(static_cast<int>(t)), pat->steps[t], kColumnBytes);
  }
  ctx.clip->set_whole(pat->track_length, pat->name, static_cast<std::uint8_t>(v.edit.pattern));
  // OS-MIDIREC-P1 S7c (A16): the pattern's MIDI lane events travel with it (the clip's 1,024-event buffer; more than that and the clip carries none, which the strip says).
  const pe::LaneCopy lanes = pe::lanes_to_clip(*ctx.clip, ctx.model.lanes, v.edit.pattern, 0, 0);
  core::FixedString<core::kNotifyTextChars> text;
  append_pattern(text, v.edit.pattern);
  text.append(" COPIED");
  if (lanes == pe::LaneCopy::TooMany) {
    text.append(" - NO LANES: TOO MANY");
  }
  return clip_say(ctx, text.c_str());
}

// PAT-OPS-B. Shift + Copy with F1 held: the whole clip over the chooser's pattern, steps, lengths and name (Q5a), one undo unit; a pattern the project lacks is made first (structural: the fence of
// OS-UNDOGRP D4). Edit only, also over the pattern that plays (Q4a: a row may be half old and half new, as a block paste; OS-BUG-PASTEHANG: the notes that sound end at the next step, as at a switch). Only the clip a whole Copy made (a block clip pastes in the grid).
bool pattern_clip_paste(ActionContext& ctx, core::ActionArg) noexcept {
  ViewState& v = ctx.view;
  if (ctx.edit == nullptr || !pattern_chooser_held(v)) {
    return false;
  }
  if (!pe::edit_enabled(ctx)) {
    return clip_say(ctx, pe::kEditOff);
  }
  if (ctx.clip == nullptr || !ctx.clip->whole()) {
    return clip_say(ctx, "NO PATTERN COPIED");
  }
  const unsigned id = v.edit.pattern;
  // OS-MIDIREC-P1 S7c (A16): the clip's MIDI lane events replace the pattern's over each track's new length. A paste the pool or the undo ring would not take is refused whole, first (before
  // the pattern is made); the dry run counts each old lane whole (the lengths change around the replace) and the step images at their largest.
  int take[model::kMaxTracks];
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    take[t] = ctx.clip->lengths()[t];
  }
  pe::LanePaste lanes(ctx, id, 0, 0, take, true);
  if (!lanes.fits(pe::step_bytes(static_cast<int>(model::kMaxTracks), static_cast<int>(model::kMaxPatternLength)) + model::kMaxTracks * 16u)) {
    return clip_say(ctx, "LANES DO NOT FIT: NOTHING CHANGED");
  }
  const bool existed = ctx.edit->project().pattern(id) != nullptr;
  bool exists = existed;
  if (!pe::create_if_missing(ctx, exists, static_cast<unsigned>(PatternSource(ctx.model, id).length()))) {
    return false;  // (said: no room)
  }
  const model::Pattern* pat = ctx.edit->project().pattern(id);
  bool changed = !existed;  // making the pattern is a change of its own
  bool grid = false;        // a step or a length changed: what the sequencer plays differs (a name alone does not)
  bool too_big = false;     // OS-MIDIREC-P1 S2: a length change was refused because the cut MIDI lane events do not fit the undo ring
  if (!ctx.edit->lengths_fit_undo(id, ctx.clip->lengths())) {
    // OS-MIDIREC-P1 S2 fold: checked before any write (a pattern just made has no lanes, so only an existing one is refused here).
    return clip_say(ctx, "TOO BIG TO UNDO: USE THE LENGTH CHOOSER");
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    grid = write_column(ctx, id, t, ctx.clip->column(static_cast<int>(t))) || grid;
    if (pat->track_length[t] != ctx.clip->lengths()[t]) {
      const model::Status st = ctx.edit->set_track_length(id, t, ctx.clip->lengths()[t]);
      grid = grid || st == model::Status::Ok;
      too_big = too_big || st == model::Status::Full;
    }
  }
  changed = lanes.apply() || changed || grid;  // (the lengths are set: each lane is replaced over its track's new length)
  if (std::strcmp(pat->name, ctx.clip->name()) != 0 && ctx.edit->set_pattern_name(id, ctx.clip->name(), std::strlen(ctx.clip->name())) == model::Status::Ok) {
    changed = true;
  }
  settle_view(ctx);
  if (!changed) {
    return clip_say(ctx, too_big ? "TOO BIG TO UNDO: USE THE LENGTH CHOOSER" : "NOTHING TO CHANGE");
  }
  if (grid && ctx.transport != nullptr && v.play.playing && v.play.count_in == 0u) {
    (void)ctx.transport->release_pattern(id);  // OS-BUG-PASTEHANG: the paste took the OFF rows of what played; every track that plays this pattern ends its notes at the next step (no mask of the tracks that changed; a refusal leaves the note to the next Stop)
  }
  core::FixedString<core::kNotifyTextChars> text;
  append_pattern(text, id);
  text.append(" REPLACED BY ");
  append_dec(text, ctx.clip->source(), ctx.clip->source() >= 100u ? 3u : 2u);
  return clip_say(ctx, text.c_str());
}

}  // namespace o3os::app
