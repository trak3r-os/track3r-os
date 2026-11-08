// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PTOOLS2 S6 (docs/specs/OS-PTOOLS2.md, Q8b / Q9b): the allowlist of tool apps that run in the TOOLS panel (ModuleUi::app_panel_fits: Humanize, three declared rows) when launched from the F7 verb menu's
// APPS list: the host draws the declared rows beside the narrowed grid (ViewState::modules.panel), the row cursor stays inside them, F1 acts without a confirm for an app that only edits the project,
// F7 + F8 returns to the Pattern, and an F1 on a block is one undo unit. Every other app (Mutate and Morph with their INSTRUMENT and count rows, Chords, Rhythm Gen, TAP CALC, SMF Export) keeps the APP screen.
// The shipped apps of the simulator's card (`apps on`), driven through the whole stack.
#include <doctest/doctest.h>

#include <cstring>

#include "support/app_grammar.h"
#include "support/need.h"

using namespace o3os;
using namespace o3os::test::grammar;

namespace {

// F7 + F8, the chord that leaves an app opened from TOOLS.
void chord_exit(AppRig& rig) {
  rig.sim.press(app::Button::F7);
  rig.sim.step();
  rig.sim.press(app::Button::F8);
  rig.sim.step();
  rig.sim.release(app::Button::F8);
  rig.sim.step();
  rig.sim.release(app::Button::F7);
  rig.sim.wait_ms(40);
}

void check_panel_app(const char* stem, unsigned declared) {
  CAPTURE(stem);
  AppRig rig;
  NEED(rig.open_tool(stem, O3_E2E_CARD_DIR));
  CHECK(rig.ui->app_param_count() == declared);
  CHECK(rig.sim.app().state().modules.from_tools);
  CHECK(rig.sim.app().state().modules.panel);  // the panel, not the APP screen's page
  // Up / Down stay inside the declared rows: Down at the last one does not reach the target rows or INSTRUMENT, which the panel does not show
  for (unsigned i = 0; i < 12; ++i) {
    rig.key(app::Button::Up);
  }
  CHECK(rig.sim.app().state().modules.app_row == 0u);
  for (unsigned i = 0; i < declared + 4; ++i) {
    rig.key(app::Button::Down);
  }
  CHECK(rig.sim.app().state().modules.app_row == declared - 1u);
  CHECK(rig.page().selected == static_cast<int>(declared) - 1);
  rig.key(app::Button::Up);
  CHECK(rig.sim.app().state().modules.app_row == declared - 2u);
  CHECK(rig.page().selected == static_cast<int>(declared) - 2);
  // F1 acts: no confirm, the app is still running on the panel
  rig.key(app::Button::F1);
  CHECK(rig.sim.app().state().screen == app::ScreenId::ModuleApp);
  CHECK(rig.sim.app().state().overlay == app::OverlayId::None);
  CHECK(rig.ui->app_running());
  CHECK(rig.sim.app().state().modules.panel);
  // F7 + F8: back on the Pattern, the panel gone
  chord_exit(rig);
  CHECK(rig.sim.app().state().screen == app::ScreenId::Pattern);
  CHECK(!rig.ui->app_running());
  CHECK(!rig.sim.app().state().modules.panel);
  CHECK(rig.sim.healthy());
}

}  // namespace

TEST_CASE("tools panel: Humanize runs in the panel: declared rows only, F1 without a confirm, F7 + F8 back to the Pattern") {
  check_panel_app("humanize", 3);
}

TEST_CASE("tools panel: every other app keeps the APP screen (Mutate and Morph reach their INSTRUMENT row there)") {
  for (const char* stem : {"mutate", "morph", "chords", "rhythmgen", "tapcalc", "smfexport"}) {
    CAPTURE(stem);
    AppRig rig;
    NEED(rig.open_tool(stem, O3_E2E_CARD_DIR));
    CHECK(rig.sim.app().state().modules.from_tools);
    CHECK(!rig.sim.app().state().modules.panel);
  }
}

TEST_CASE("tools panel: F1 on a block writes it as one undo unit, and UNDO gives the block back") {
  AppRig rig;
  NEED(rig.setup(O3_E2E_CARD_DIR));
  rig.sim.action(core::action_id("select.clear"), 0);
  rig.sim.action(core::action_id("select.column_all"), 0);  // the whole track 1 of the demo document
  NEED(rig.launch_from_tools("humanize"));
  CHECK(rig.sim.app().state().modules.panel);
  const unsigned pattern = rig.sim.app().state().edit.pattern;
  const model::Pattern before = *rig.sim.project()->pattern(pattern);
  const unsigned depth = rig.sim.app().model().undo_depth;
  rig.key(app::Button::F1);
  rig.sim.wait_ms(100);
  CHECK(rig.sim.app().state().modules.panel);
  CHECK(std::memcmp(&before, rig.sim.project()->pattern(pattern), sizeof before) != 0);  // the demo's notes got a VOL
  CHECK(rig.sim.app().model().undo_depth == depth + 1u);  // one unit, not one per step
  chord_exit(rig);
  rig.sim.action(core::action_id("edit.undo"), 0);
  rig.sim.wait_ms(100);
  CHECK(std::memcmp(&before, rig.sim.project()->pattern(pattern), sizeof before) == 0);
  CHECK(rig.sim.app().model().undo_depth == depth);
}

TEST_CASE("tools panel: an app opened from the MODULES list is never in the panel") {
  AppRig rig;
  NEED(rig.open("humanize", O3_E2E_CARD_DIR));
  CHECK(!rig.sim.app().state().modules.from_tools);
  CHECK(!rig.sim.app().state().modules.panel);
}
