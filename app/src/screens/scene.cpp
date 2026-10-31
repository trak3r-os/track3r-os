// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/scene.h>

#include <o3os/app/key_prompt.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/screen.h>

namespace o3os::app {

void render_scene(const Model& model, const ViewState& view, ui::Renderer& r) noexcept {
  view.notemap.begin_frame();  // the Song TIMELINE page's note-map uploads are counted a frame (song_notemap.h)
  const ScreenEntry* entry = find_screen(screen_registry(), view.screen);
  const Screen& screen = entry != nullptr && entry->screen != nullptr ? *entry->screen : kPatternScreen;
  // The base screen first, told what the overlay will cover so it can leave that out; then the overlay over it.
  const Overlay* overlay = overlay_of(view.shown_overlay());
  screen.render(model, view, r, overlay != nullptr ? overlay->rect(r.theme()) : ui::Rect{0, 0, 0, 0});
  if (overlay != nullptr) {
    overlay->render(model, view, r);
  }
  draw_owner_keys(r, model, view);  // the F-key row of the dialog that owns it, once, on the frame band (the screen under it drew none: ViewState::overlay_keys)
  render_power(view, r);  // last: it shows over a modal overlay too (OS-POWER D4)
}

}  // namespace o3os::app
