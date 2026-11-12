// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboard's actions (OS-FU2; app/text_entry.h has the machine, screens/text_entry_overlay.cpp the picture): they only edit the overlay's text and focus (view state, no
// undo unit). F1's text.accept (OS-UXF-KEYS item 5; it was F8) is the maintainer's: for Save As it is in save_actions.cpp.
#include "action_handlers.h"

#include <o3os/app/save_service.h>
#include <o3os/app/text_entry.h>

namespace o3os::app {

namespace {

bool up(const ActionContext& ctx) noexcept { return ctx.view.overlay == OverlayId::TextEntry; }

// The line under the name: cleared by anything that changes the text, set when a key is refused.
bool settled(ActionContext& ctx, bool changed) noexcept {
  if (changed) {
    ctx.view.text.message = "";
  }
  return changed;
}

}  // namespace

// A pad: the key under it is typed. A key the name may not have says so.
bool text_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!up(ctx) || arg < 0 || arg >= 48) {
    return false;
  }
  TextEntry& e = ctx.view.text.entry;
  const unsigned pad = static_cast<unsigned>(arg);
  if (e.pad(pad)) {
    return settled(ctx, true);
  }
  if (!e.key_allowed(kTextPadKey[pad])) {
    ctx.view.text.message = "NOT IN A FILE NAME";
  }
  return true;  // the focus moved to it
}

// The arrows: 0 left, 1 right, 2 up, 3 down.
bool text_move(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!up(ctx) || arg < 0 || arg > 3) {
    return false;
  }
  const int dx = arg == 0 ? -1 : (arg == 1 ? 1 : 0);
  const int dy = arg == 2 ? -1 : (arg == 3 ? 1 : 0);
  return ctx.view.text.entry.move(dx, dy);
}

// The encoder: along the row (or along the name, while the focus is in it).
bool text_scroll(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!up(ctx) || arg == 0) {
    return false;
  }
  bool moved = false;
  for (int n = arg < 0 ? -arg : arg, i = 0; i < n && i < 16; ++i) {
    moved = ctx.view.text.entry.move(arg < 0 ? -1 : 1, 0) || moved;
  }
  return moved;
}

// Insert and F1: the key in focus is typed.
bool text_key(ActionContext& ctx, core::ActionArg) noexcept {
  if (!up(ctx)) {
    return false;
  }
  TextEntry& e = ctx.view.text.entry;
  const unsigned key = e.key();
  if (e.confirm_key()) {
    return settled(ctx, true);
  }
  if (!e.frame() && !e.key_allowed(key)) {
    ctx.view.text.message = "NOT IN A FILE NAME";
    return true;
  }
  return false;
}

bool text_caps(ActionContext& ctx, core::ActionArg) noexcept { return up(ctx) && ctx.view.text.entry.type(kTextKeyCaps); }

bool text_backspace(ActionContext& ctx, core::ActionArg) noexcept { return up(ctx) && settled(ctx, ctx.view.text.entry.backspace()); }

// F6: the automatic name again (the next free "Track NN"), selected.
bool text_auto(ActionContext& ctx, core::ActionArg) noexcept {
  if (!up(ctx) || ctx.view.text.target != TextEntryView::Target::SaveAs || ctx.saves == nullptr) {
    return false;
  }
  char name[hal::storage::save::kNameMax + 1] = {};
  if (!ctx.saves->suggest_name(name, sizeof name)) {
    return false;
  }
  ctx.view.text.entry.set(name);
  ctx.view.text.message = "";
  return true;
}

}  // namespace o3os::app
