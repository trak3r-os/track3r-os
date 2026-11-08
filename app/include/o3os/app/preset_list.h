// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-INSFX-ABI2 A9 (docs/specs/OS-INSFX-ABI2.md section 3.6): the list the F5 PRESET overlay of a module's parameter page shows. New Community OS behaviour.
//
// This is the one seam between the overlay (screens/preset_overlay.cpp), its actions (preset_actions.cpp) and where presets come from. The list is the factory presets of the module's
// descriptor (ModuleUi::factory_preset) followed by the CARD section (OS-MODPRESET P4): the names of the last ModuleUi::scan_presets, which preset_open runs when the list opens. The
// overlay asks an entry by its index; ParamSet::source says where it comes from. The render reads no file per frame: it takes a card entry's name from the scan, and preset_list_get
// reads (and resolves) a card entry only when the load asks for it.

#include <o3os/app/model.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/param_set.h>
#include <o3os/app/view_state.h>

namespace o3os::app {

// How many presets the list of the page in view holds: the module's factory presets plus the card's listed for it (0 when the page is no bound module's, or the module has none).
unsigned preset_list_count(const Model& model, const ViewState& view) noexcept;
// Entry `n` of that list as a ParamSet (false: no such entry). A factory entry is the descriptor's; a card entry (n at or past the module's factory count) is read and resolved now,
// so the caller that loads (the list's F1) passes `card` / `skipped` to learn how that went: `*card` is ModuleUi::CardPreset::Ok and `*skipped` 0 for a factory entry, and for a card
// entry the read's answer and how many entry names no parameter had. The overlay's render does not call this for card entries (it would read a file a frame).
bool preset_list_get(const Model& model, const ViewState& view, unsigned n, ParamSet& out, ModuleUi::CardPreset* card = nullptr, unsigned* skipped = nullptr) noexcept;

}  // namespace o3os::app
