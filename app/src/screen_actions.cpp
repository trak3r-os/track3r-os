// SPDX-License-Identifier: GPL-3.0-or-later
// Screen actions: switching the base screen, cycling pages and modes, and the Navigator overlay (OS-002b).
#include "action_handlers.h"

#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/song_source.h>
#include <o3os/model/limits.h>

namespace o3os::app {

namespace {

// The mode table of the screen in view; empty for a screen without modes (or one that is not implemented).
ModeTable modes_in_view(const ViewState& view) noexcept {
  const ScreenEntry* e = find_screen(screen_registry(), view.screen);
  return e != nullptr && e->screen != nullptr ? e->screen->modes() : ModeTable{nullptr, 0};
}

}  // namespace

// A screen button: another screen, or (the current screen's own button) its next page.
// OS-QAFIX-E-SONG-06 (new Community OS behaviour; an owner of the official firmware expects it, E-SONG-06): the Pattern button on Song opens the pattern of the slot under the Song cursor. The view
// is only set when the move succeeds; a playing pattern that the view follows (WATCH, Edit with follow) still pulls the view to the playhead on the next frame, as before.
bool nav_screen(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!screen_id_valid(arg)) {
    return false;
  }
  const bool song_to_pattern = ctx.view.screen == ScreenId::Song && static_cast<ScreenId>(arg) == ScreenId::Pattern && ctx.model.project != nullptr;
  if (!navigate_to(screen_registry(), ctx.model, ctx.view, static_cast<ScreenId>(arg))) {
    return false;
  }
  if (song_to_pattern && ctx.view.screen == ScreenId::Pattern) {
    const SongSource src(ctx.model);
    const int last = src.count() - 1;  // (the cursor's slot as the Song actions read it: inside the list; -1 for an empty one, which has no pattern)
    const int pos = ctx.view.song.pos() < last ? ctx.view.song.pos() : last;
    const unsigned id = src.pattern(pos);
    if (id >= 1u && id <= model::kMaxPatternId && id != ctx.view.edit.pattern) {
      ctx.view.edit.pattern = static_cast<std::uint8_t>(id);
      ctx.view.edit.selection.clear();  // (its rows belong to the pattern it was made in)
      (void)pattern_sync(ctx.model, ctx.view);  // the cursor stays where it was, inside the new pattern
    }
  }
  return true;
}

// OS-SONGPEEK, a screen button held (the Hold of Song): the screen is shown until the key is let go. Nothing to peek at when it is the screen in view or does not exist yet; the Tap of that
// button (the next page, the "not available" strip) is gone with the hold, as a Hold takes the Tap's place.
bool nav_peek(ActionContext& ctx, core::ActionArg arg) noexcept {
  ctx.view.peeking = false;  // a peek whose HoldEnd was captured by a modal overlay under the held key never ended: it must not outlive this hold, whose release would jump back
  const ScreenId from = ctx.view.screen;
  if (!screen_id_valid(arg) || from == static_cast<ScreenId>(arg) || !navigate_to(screen_registry(), ctx.model, ctx.view, static_cast<ScreenId>(arg))) {
    return false;
  }
  if (ctx.view.screen == static_cast<ScreenId>(arg)) {  // (a screen that is not implemented only posted its strip)
    ctx.view.peek_from = from;
    ctx.view.peeking = true;
  }
  return true;
}

// The release of that button (HoldEnd): one level back, to the screen the peek came from. Only if the peeked screen is still the one in view: a screen button pressed under the held key has
// already ended the peek (switch_screen), and the user stays where they went.
bool nav_peek_end(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!ctx.view.peeking || !screen_id_valid(arg) || ctx.view.screen != static_cast<ScreenId>(arg)) {
    return false;
  }
  const ScreenId back = ctx.view.peek_from;
  ctx.view.peeking = false;
  return switch_screen(screen_registry(), ctx.model, ctx.view, back);
}

bool view_mode(ActionContext& ctx, core::ActionArg arg) noexcept {
  const ModeTable modes = modes_in_view(ctx.view);
  if (arg < 0 || static_cast<unsigned>(arg) >= modes.count || modes.items[arg].retired) {
    return false;
  }
  if (ctx.view.screen == ScreenId::Pattern) {  // leaving Edit drops the free state (pattern_set_mode)
    return pattern_set_mode(ctx.view, static_cast<PatternMode>(arg));
  }
  std::uint8_t& mode = ctx.view.mode[screen_index(ctx.view.screen)];
  const bool changed = mode != arg;
  mode = static_cast<std::uint8_t>(arg);
  return changed;
}

bool view_mode_next(ActionContext& ctx, core::ActionArg) noexcept {
  const ModeTable modes = modes_in_view(ctx.view);
  if (modes.count < 2) {
    return false;
  }
  std::uint8_t& mode = ctx.view.mode[screen_index(ctx.view.screen)];
  unsigned next = (mode + 1u) % modes.count;
  while (modes.items[next].retired) {  // (OS-004d: Follow; at least one mode is not retired, or the screen has none)
    next = (next + 1u) % modes.count;
  }
  if (ctx.view.screen == ScreenId::Pattern) {
    return pattern_set_mode(ctx.view, static_cast<PatternMode>(next));
  }
  mode = static_cast<std::uint8_t>(next);
  return true;
}

// The Navigator opens on the screen in view and stays open (OS-NAV2) until F8 / F1 go to the chosen screen or F7 / Pattern close it.
bool navigator_open(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::None) {
    return false;
  }
  const int at = screen_position(navigator_screens(), ctx.view.screen);
  ctx.view.overlay = OverlayId::Navigator;
  ctx.view.navigator = at >= 0 ? at : 0;
  return true;
}

// GO (F8, F1): opens the chosen screen and closes the Navigator. A screen that is not implemented stays unchosen: the Navigator
// stays open (its cell is drawn disabled; a notification would wait behind it).
bool navigator_accept(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::Navigator) {
    return false;
  }
  const ScreenTable table = navigator_screens();
  const ScreenEntry* chosen = screen_at(table, ctx.view.navigator);
  if (chosen == nullptr || chosen->screen == nullptr) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  (void)switch_screen(table, ctx.model, ctx.view, chosen->id);
  return true;
}

// The INPUT TEST screen forgets its counts (the hardware session's "clear": the press counts, the encoder total, the latency).
bool input_test_reset(ActionContext& ctx, core::ActionArg) noexcept {
  ctx.view.input_test.reset();
  return true;
}

// Back: closes the overlay in view (Learn; the Navigator's CLOSE on F7 and Pattern). There is no Back button on the device: F8, in the
// overlays' own contexts (the Navigator's F8 is GO).
bool ui_back(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay == OverlayId::None) {
    return false;
  }
  if (ctx.view.overlay == OverlayId::StepPopup) {
    (void)ctx.view.pattern.snap_column();  // OS-ZOOM: the popup of a field the zoom level hides moved the cursor's column there; back in sight
  }
  if (ctx.view.overlay == OverlayId::PatternChooser || ctx.view.overlay == OverlayId::LengthChooser || ctx.view.overlay == OverlayId::JumpChooser) {
    ctx.view.edit.chooser_held = false;  // OS-UXF-PAT 6: F8 closes a tap-opened chooser; the flag ("the live PATTERN chooser") must not outlive it
  }
  ctx.view.overlay = OverlayId::None;
  return true;
}

}  // namespace o3os::app
