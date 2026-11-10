// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The items of the Pattern TOOLS menu's APPS page (OS-H3A, ADR 0014 slice H3; docs/specs/OS-H3A.md section 2; the F7 verb menu's APPS > row since OS-PTOOLS2 S3). One place that the page's overlay
// (screens/verb_menu_overlay.cpp), the action that runs an item (tools_actions.cpp) and the tests ask.
//
// OS-TOOLSDEDUP: the page lists only the tool apps, the installed apps whose file asks for edit_project (ModuleUi::scan_tools), sorted by name. They launch with the Pattern's block as their context
// (o3m_launch_ctx). The OS's own operations (DUPLICATE, EXPAND, SHRINK, the exports) are verbs of the menu's grid and its F6 is RE FILL / REPEAT (verb_repeat; EVENTS is Pattern F6, OS-F6EVENTS), so the page does not repeat them.
//
// Main loop only. No allocation.

#include <o3os/app/model.h>
#include <o3os/app/module_ui.h>

#include <cstdint>

namespace o3os::app {

struct ToolItem {
  unsigned app_entry = 0;  // the index launch_app takes
  const char* label = "";  // the text shown (the app's file stem)
  bool bad = false;        // shown dim: the app is quarantined (it cannot run this boot)
};

// Items in the page: the tool apps (the last scan_tools(); none without a module system).
inline unsigned tools_count(const Model& model) noexcept { return model.modules != nullptr ? model.modules->tool_count() : 0u; }

// Item `i` (false: out of range).
inline bool tools_item(const Model& model, unsigned i, ToolItem& out) noexcept {
  if (i >= tools_count(model)) {
    return false;
  }
  const unsigned entry = model.modules->tool_entry(i);
  const CatalogEntry* e = model.modules->app_entry_at(entry);
  if (e == nullptr) {
    return false;
  }
  out.app_entry = entry;
  out.label = e->stem;
  out.bad = e->state == CatalogEntry::State::Quarantined;
  return true;
}

// OS-APPSNAV: the apps that are not tools (the complement of the list above, in catalogue order): what Shift + Pattern lists. `tool_entry` ascends, so a walk finds item `k` without a table.
inline unsigned apps_count(const Model& model) noexcept {
  return model.modules != nullptr ? model.modules->app_entry_count() - tools_count(model) : 0u;
}

// The catalogue index of item `k` of that list (apps_count() when out of range).
inline unsigned apps_entry(const Model& model, unsigned k) noexcept {
  if (model.modules == nullptr) {
    return 0u;
  }
  const unsigned total = model.modules->app_entry_count();
  const unsigned tools = tools_count(model);
  unsigned t = 0;
  for (unsigned e = 0; e < total; ++e) {
    if (t < tools && model.modules->tool_entry(t) == e) {
      ++t;
    } else if (k-- == 0u) {
      return e;
    }
  }
  return total;
}

}  // namespace o3os::app
