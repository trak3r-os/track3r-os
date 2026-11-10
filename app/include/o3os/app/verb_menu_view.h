// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The F7 verb menu's view state (OS-PTOOLS2 S3, app/verb_menu.h): the verb under the cursor (0..15, pad - 1). The menu remembers its last verb between opens
// (spec 3.1 point 2), unlike the TOOLS list, which opens on its first row every time.

#include <o3os/app/pattern_tools_view.h>

#include <cstdint>

namespace o3os::app {

struct VerbMenuView {
  std::uint8_t cursor = 0;
  // OS-PTOOLS2 S3b: the seed the random verbs drew with (each run of RANDOMISE / HUMANISE takes the next seed, so running one again re-rolls; the amounts
  // stay the Plan's fixed defaults in this slice). Parameterless menu verbs never become what F6 REPEAT runs (see last_verb below).
  std::uint16_t seed = 0;
  // OS-PTOOLS2 S3b: a pad ran while F7 was held (verb.hold_pad), before the Hold fired: the Tap of F7 that follows such a pad is not a menu
  // open (verb.open ignores that one Tap and clears the mark; verb.hold and verb.hold_end clear it too, so a stale mark never eats a real Tap).
  bool hold_pad_ran = false;
  // OS-PTOOLS2 S6b: the menu shows its APPS page (pad 16, tools_open): the list of the OS's own entries and the tool apps (app/tools_list.h), whose row cursor is ModulesView::tools. verb_open clears it, so F7 opens the grid.
  bool apps = false;
  // OS-PTOOLS2 S2: what F6 REPEAT runs: the last parameterised apply, the last FILL dialog (fill.apply records LastVerb::Fill) or the last TOOLS dialog APPLY
  // (ptool.apply records LastVerb::Tool with its amount: F6 repeats it with the same amount on the selection as it is now). Orchestrator decision 2026-11-07
  // (maintainer may revisit). The key row of the verb menu says which (F6: RE FILL, or REPEAT after a TOOLS apply; key_prompt.cpp).
  enum class LastVerb : std::uint8_t { None = 0, Fill = 1, Tool = 2 };
  LastVerb last_verb = LastVerb::None;
  struct ToolLast {
    PTool tool = PTool::Transpose;
    std::int8_t amount = 0;         // TRANSPOSE, the applied amount
    std::uint8_t instrument = 1;    // INSTRUMENT
    SongWhat what = SongWhat::All;  // CLEAR
  } tool_last;
  // A NEW or an OPENED project: what F6 repeats is the session's, not the project's (the caller clears FillView::valid beside it), so F6 never re-runs project A's last apply on project B.
  void forget_last() noexcept {
    last_verb = LastVerb::None;
    tool_last = ToolLast{};
  }
};

}  // namespace o3os::app
