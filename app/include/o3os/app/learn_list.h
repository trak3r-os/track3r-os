// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The bindings of both Learn stores as one list (OS-013a): what the Learn overlay shows and what its cursor, delete and clear-all act on. The project's used slots come first
// (when a project is open), then the device's, each in slot order; an empty slot is not in the list. The list is read from the stores on every call (at most 2 x 32 slots), so
// it is never stale and costs no storage.

#include <o3os/app/model.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/learn.h>

namespace o3os::app {

struct ViewState;

struct LearnEntry {
  const core::LearnBinding* binding;
  core::LearnScope scope;
  unsigned slot;  // the slot in its store
};

// How many bindings are in use in the two stores.
unsigned learn_entry_count(const Model& model) noexcept;
// The `index`-th binding of the list; false when there is none.
bool learn_entry_at(const Model& model, unsigned index, LearnEntry& out) noexcept;

// What owns a binding's parameter, for a list row: "INS 03", "INS *" (a follow binding), "TRK 02", "MASTER", "SETTING", "FX 03A" (screens/overlays.cpp).
void owner_text(core::FixedString<48>& line, const core::LearnBinding& b) noexcept;
// LEARN MAP's VALUE column (learn_map_actions.cpp): the current value of what the binding acts on (a follow binding: the selected instrument's), text of at most core::kParamTextChars; "--" when there is none.
void learn_value_text(const Model& model, const ViewState& view, const core::LearnBinding& b, char* out) noexcept;

}  // namespace o3os::app
