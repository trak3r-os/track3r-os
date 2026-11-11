// SPDX-License-Identifier: GPL-3.0-or-later
// The choosers of F1..F3 on the Pattern screen (OS-009 P1a.1): a strip over the info line that picks the pattern, its length or the step
// jump with the encoder, Up and Down, the defined values on Left and Right, and the pads. The chooser selects while the F-key is down
// and enters at the release; here a tap keeps LENGTH and JUMP open (the same key again or F8
// closes) and a hold opens any of them until the release. OS-PATSWITCH: the PATTERN chooser is the hold only (a tap of F1 leaves nothing open).
// Choosing a pattern stopped, and a jump, is view state; the length is the pattern's, an undo unit. Held while the sequencer plays, a pick switches the playing pattern (pick_pattern).
#include "pattern_edit_util.h"

namespace o3os::app {

namespace {

enum Kind : core::ActionArg { kPattern = 0, kLength = 1, kJump = 2 };

constexpr OverlayId overlay_of_kind(core::ActionArg kind) noexcept {
  return kind == kPattern ? OverlayId::PatternChooser : (kind == kLength ? OverlayId::LengthChooser : OverlayId::JumpChooser);
}

// The values Left and Right step through.
constexpr int kLengthValues[] = {1, 2, 4, 8, 16, 32, 48, 64, 80, 96, 128};
constexpr int kJumpValues[] = {0, 1, 2, 4, 8, 16, 32};

template <std::size_t N>
int next_defined(const int (&values)[N], int current, int dir) noexcept {
  if (dir > 0) {
    for (std::size_t a = 0; a < N; ++a) {
      if (current < values[a]) {
        return values[a];
      }
    }
  } else {
    for (std::size_t a = N; a-- > 0;) {
      if (current > values[a]) {
        return values[a];
      }
    }
  }
  return current;
}

// The pattern's length chosen: all tracks. A pattern that does not exist yet is created at that length.
bool set_length(ActionContext& ctx, int length) noexcept {
  if (ctx.edit == nullptr) {
    return pe::say(ctx, core::Severity::Info, "NO PROJECT");
  }
  length = pe::clampi(length, static_cast<int>(model::kMinPatternLength), static_cast<int>(model::kMaxPatternLength));
  const PatternSource src(ctx.model, ctx.view.edit.pattern);
  if (length == src.length() && src.exists()) {
    return false;
  }
  const unsigned id = ctx.view.edit.pattern;
  PatternEdit& e = ctx.view.edit;
  model::Status st = src.exists() ? ctx.edit->set_pattern_length(id, static_cast<unsigned>(length))
                                  : ctx.edit->create_pattern(id, static_cast<unsigned>(length));
  const std::uint32_t generation = ctx.edit->project().generation();
  const std::uint16_t asked = static_cast<std::uint16_t>((id << 8) | static_cast<unsigned>(length));
  if (st == model::Status::Full && src.exists() && ctx.edit->lanes() != nullptr && ctx.edit->lanes()->total_events() != 0u) {
    // OS-MIDIREC-P1 S2 (maintainer, 2026-11-10): the cut MIDI lane events do not fit the undo ring (the Editor refused, nothing changed). The first press asks; the same length again, with no edit
    // in between, goes on without undo and forgets the whole undo history (what the ring holds names a project that is not the one that results).
    if (e.undo_ask != asked || e.undo_ask_generation != generation) {
      e.undo_ask = asked;
      e.undo_ask_generation = generation;
      return pe::say(ctx, core::Severity::Warn, "TOO BIG TO UNDO: AGAIN TO CUT THE LANES");
    }
    const bool was_final = ctx.edit->final_edits();  // as the dispatcher does (actions.cpp): put back what it was, not false
    ctx.edit->set_final(true);                       // the unit records nothing
    st = ctx.edit->set_pattern_length(id, static_cast<unsigned>(length));
    ctx.edit->set_final(was_final);
    if (st == model::Status::Ok) {
      ctx.edit->history_reset();
    }
  }
  e.undo_ask = 0;
  if (st != model::Status::Ok) {
    return pe::say(ctx, core::Severity::Warn, "NO ROOM FOR A NEW PATTERN");
  }
  ctx.view.edit.selection.clear();
  return true;
}

bool set_pattern(ActionContext& ctx, int id) noexcept {
  id = pe::clampi(id, 1, static_cast<int>(model::kMaxPatternId));  // 1..255
  if (id == ctx.view.edit.pattern) {
    return false;
  }
  ctx.view.edit.pattern = static_cast<std::uint8_t>(id);
  ctx.view.edit.selection.clear();
  return true;
}

// A pick in the PATTERN chooser (a pad, the encoder, Up, Down). OS-PATSWITCH (new Community OS behaviour; docs/specs/OS-PATSWITCH.md): with F1 held, the sequencer playing and no count-in, the pick switches
// the PLAYING pattern at the next step boundary (TransportSink::switch_pattern; the row is kept, the sequencer's rule). Everything else is a view pick, as before: stopped, in a count-in, with no sink.
// A pick is compared with where the sequencer is going, not with the view: the pending switch if there is one, else the pattern that plays (so the view parked on another pattern, as in STEP, still
// switches to it, and a pick back to the playing pattern takes back a switch that waits). The playing pattern again does nothing in Pattern mode and leaves a Song to loop it. A refusing sink changes nothing.
bool pick_pattern(ActionContext& ctx, int id) noexcept {
  id = pe::clampi(id, 1, static_cast<int>(model::kMaxPatternId));
  PatternEdit& e = ctx.view.edit;
  const audio::PlayPosition::Value& play = ctx.view.play;
  if (e.chooser_held && ctx.transport != nullptr && ctx.model.project != nullptr && play.playing && play.count_in == 0u) {
    const unsigned want = static_cast<unsigned>(id);
    const bool song = play.order != audio::PlayPosition::kNoOrder;
    if (ctx.model.device.get(model::device_setting::PatSwitch) >= 1.0f) {
      // E-PAT-35 (PAT SWITCH = Pattern end; new Community OS behaviour): the pick is the PATS pads' queue (TransportSink::queue_pattern): it plays when the pattern that plays has ended. The playing pattern
      // (Pattern mode) or the one already queued, picked again, takes the queue back. The view holds the pick while F1 is held (pattern_follow_play), then goes on following the playing pattern.
      const bool cancel = (want == play.pattern && !song) || want == e.queued_pattern;
      const bool needs_command = !cancel || e.queued_pattern != 0u;  // (nothing waits: nothing to take back)
      if (needs_command && !ctx.transport->queue_pattern(cancel ? 0u : want)) {
        return false;  // no sequencer behind the sink, or its ring is full: the pick is not applied
      }
      e.queued_pattern = cancel ? std::uint8_t{0} : static_cast<std::uint8_t>(want);
      e.switch_to = 0;
      e.pattern = static_cast<std::uint8_t>(want);
      e.selection.clear();
      return true;
    }
    const unsigned there = e.switch_to != 0u ? e.switch_to : play.pattern;
    if (want != there || (song && e.switch_to == 0u)) {
      if (!ctx.transport->switch_pattern(want)) {
        return false;  // no sequencer behind the sink, or its ring is full: the pick is not applied
      }
      e.switch_to = want == play.pattern && !song ? std::uint8_t{0} : static_cast<std::uint8_t>(want);  // back to the playing pattern: nothing waits
      e.switch_age = 0;
      e.queued_pattern = 0;  // the sequencer dropped a PATS queue (the newest intent wins)
      e.pattern = static_cast<std::uint8_t>(want);
      e.selection.clear();
      return true;
    }
  }
  return set_pattern(ctx, id);
}

bool set_jump(ActionContext& ctx, int step) noexcept {
  step = pe::clampi(step, 0, static_cast<int>(model::edit::kMaxEditStep));
  const bool changed = step != ctx.view.edit.edit_step;
  ctx.view.edit.edit_step = static_cast<std::uint8_t>(step);
  return changed;
}

}  // namespace

// F1..F3 tapped. Without a document there is nothing to choose among (the Pattern screen shows the demo, read-only): the key says its
// label as it did before the choosers existed.
bool chooser_toggle(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < kPattern || arg > kJump || ctx.view.screen != ScreenId::Pattern) {
    return false;
  }
  if (ctx.model.project == nullptr) {
    return ui_softkey(ctx, arg);
  }
  if (arg == kPattern) {
    return false;  // OS-PATSWITCH (maintainer, 2026-10-17): the PATTERN chooser is the hold of F1 only; a tap leaves nothing open
  }
  const OverlayId want = overlay_of_kind(arg);
  ctx.view.edit.chooser_held = false;
  if (ctx.view.overlay == want) {
    ctx.view.overlay = OverlayId::None;
    return true;
  }
  if (ctx.view.overlay != OverlayId::None && !is_chooser(ctx.view.overlay)) {
    return false;  // Learn or the Navigator is up: not ours
  }
  ctx.view.overlay = want;
  return true;
}

bool chooser_hold(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < kPattern || arg > kJump || ctx.view.screen != ScreenId::Pattern || ctx.model.project == nullptr) {
    return false;
  }
  const OverlayId want = overlay_of_kind(arg);
  if (ctx.view.overlay != OverlayId::None && !is_chooser(ctx.view.overlay)) {
    return false;
  }
  const bool changed = ctx.view.overlay != want;
  ctx.view.overlay = want;
  ctx.view.edit.chooser_held = true;
  return changed;
}

// The release of the F-key whose hold opened the chooser: it closes. A chooser a tap opened stays (the release of that tap is silent).
bool chooser_release(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < kPattern || arg > kJump || ctx.view.overlay != overlay_of_kind(arg) || !ctx.view.edit.chooser_held) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  ctx.view.edit.chooser_held = false;
  return true;
}

// Up, Down and the encoder: one place at a time, or the detents turned.
bool chooser_delta(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int d = pe::clampi(arg, -1000, 1000);
  switch (ctx.view.overlay) {
    case OverlayId::PatternChooser:
      return d != 0 && pick_pattern(ctx, ctx.view.edit.pattern + d);  // (counted from the view, which holds the last pick: a fast turn does not start again from the playing number)
    case OverlayId::LengthChooser:
      return set_length(ctx, PatternSource(ctx.model, ctx.view.edit.pattern).length() + d);
    case OverlayId::JumpChooser:
      return set_jump(ctx, ctx.view.edit.edit_step + d);
    default:
      return false;
  }
}

// OS-SHIFTENC (Shift + encoder): 8 places a detent, clamped at the ends as the fine step is (the pattern 1..255, the length, the jump).
bool chooser_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return chooser_delta(ctx, pe::clampi(arg, -125, 125) * 8); }

// Left and Right: the next defined value of the length and of the jump; in the PATTERN chooser the previous and the next pattern the project holds (OS-QAFIX-6, new Community OS behaviour: Up and
// Down, the encoder and the pads step by one id; this skips the unused ids, as the length's defined values do). None further on: nothing changes.
bool chooser_defined(ActionContext& ctx, core::ActionArg arg) noexcept {
  const int dir = arg > 0 ? 1 : (arg < 0 ? -1 : 0);
  switch (ctx.view.overlay) {
    case OverlayId::PatternChooser: {
      if (dir == 0 || ctx.model.project == nullptr) {
        return false;
      }
      const int last = static_cast<int>(model::kMaxPatternId);
      for (int id = ctx.view.edit.pattern + dir; id >= 1 && id <= last; id += dir) {  // (at most 255 lookups, on a key press)
        if (ctx.model.project->pattern(static_cast<std::uint32_t>(id)) != nullptr) {
          return pick_pattern(ctx, id);
        }
      }
      return pe::say(ctx, core::Severity::Info, dir > 0 ? "NO NEXT PATTERN" : "NO PREVIOUS PATTERN");  // (a key never does nothing in silence)
    }
    case OverlayId::LengthChooser:
      return dir != 0 && set_length(ctx, next_defined(kLengthValues, PatternSource(ctx.model, ctx.view.edit.pattern).length(), dir));
    case OverlayId::JumpChooser:
      return dir != 0 && set_jump(ctx, next_defined(kJumpValues, ctx.view.edit.edit_step, dir));
    default:
      return false;
  }
}

// A pad: pattern pad + 1; length 4 + pad * 188 / 47 held to the last step, so 4..128 steps; jump = the pad (0..32).
bool chooser_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (arg < 0 || arg >= static_cast<core::ActionArg>(model::edit::kPadCount)) {
    return false;
  }
  switch (ctx.view.overlay) {
    case OverlayId::PatternChooser:
      return pick_pattern(ctx, arg + 1);
    case OverlayId::LengthChooser:
      return set_length(ctx, pe::clampi(model::edit::map_int(arg, 0, 47, 3, 191), 0, 127) + 1);
    case OverlayId::JumpChooser:
      return set_jump(ctx, arg);
    default:
      return false;
  }
}

}  // namespace o3os::app
