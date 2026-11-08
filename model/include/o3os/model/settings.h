// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/status.h>

#include <cstdint>

namespace o3os::model {

// A block of settings of one scope (ADR 0009 rev 1: "settings are parameters with scope device or project", OS-002d): the values
// of the parameters of kind Project or Device (model/params.h lists them), a generation counter for each, and the MIDI Learn
// store of that scope (core/learn.h). The device's block is persisted in the settings file (docs/formats/o3-settings.md), the
// project's inside the project's own file (the SETS and LRNB chunks of docs/formats/o3-project.md): the same two chunks, so
// one codec reads both (formats/settings_chunks.h) and a missing or unknown entry means the default.
//
// The values are bytes of `raw_`, the descriptors give each field's place and range, so a setting is one table row. The block is a
// plain aggregate (no constructor), so the Project, which lives in zeroed SDRAM, can hold one; init() makes it a valid block.
//
// Generations are free-running counters: generation(field) advances whenever THAT value changes, learn_generation() whenever the
// store does; init() and assign() advance what they change (init() all of them), so a screen that polls sees every change and no
// value is ever repeated. Main loop only.
class Settings {
 public:
  // Room reserved for growth: the device scope has 30 fields today (OS-MTC B3: MTC IN, id 29; OS-MTC A3: MTC OUT and MTC START, ids 27 and 28, after OS-MIDICCMAP S1: CC MAP, id 26, after E-CFG-03's TRANSPORT IN / OUT, ids 23 and 24, and E-PAT-35's PAT SWITCH, id 25) and the project 3, each one byte of the values.
  // These are the block's capacity, not the scopes' field counts, so adding a setting is a table row (model/params.cpp) until a scope reaches the capacity. OS-MIDICCMAP S1 landed before OS-MTC and made the raise itself: the capacity went 26 -> 32
  // (room for OS-MTC's ids 27..29 and three more; the device scope was full at 26: E-PAT-35's id 25 was the last that fit). In memory only: the files hold entries (id, value), not this block (it was 16 until OS-006e, 24 until E-CFG-03, 26 until
  // OS-MIDICCMAP S1, 32 since: the arena grew by 40 bytes for the 24, by 8 for the 26, by 32 for the 32: 6 value bytes plus the block's pad, and the 6 new generation bytes).
  static constexpr unsigned kValueBytes = 32;
  static constexpr unsigned kMaxFields = 32;

  // Defaults for every field, an empty Learn store; `kind` is core::ParamKind::Project or Device.
  void init(core::ParamKind kind) noexcept;
  core::ParamKind kind() const noexcept { return kind_; }
  unsigned field_count() const noexcept;

  // The value of a field (0 for a field that does not exist).
  float get(unsigned field) const noexcept;
  // BadIndex for no such field, BadValue for a value outside the range (NaN too); setting what is there is Ok and changes nothing.
  Status set(unsigned field, float value) noexcept;
  std::uint32_t generation(unsigned field) const noexcept;

  // The Learn store. A binding must be valid (core::learn_valid) and its parameter must be of this scope (core::learn_scope): BadValue
  // otherwise; BadIndex for a slot past kLearnSlots. An empty binding empties the slot.
  const core::LearnBinding& learn(unsigned slot) const noexcept;
  Status set_learn(unsigned slot, const core::LearnBinding& b) noexcept;
  std::uint32_t learn_generation() const noexcept { return epoch_ + learn_gen_; }
  // The slots as an array of kLearnSlots, for core::learn_find_param / learn_find_cc / learn_plan.
  const core::LearnBinding* learn_slots() const noexcept { return learn_; }
  // How many slots are in use.
  unsigned learn_count() const noexcept;

  // True when every value is its default / when, besides, the store is empty: the project writer leaves SETS out for the first and LRNB
  // out for an empty store.
  bool values_default() const noexcept;
  bool is_default() const noexcept { return values_default() && learn_count() == 0; }

  // Becomes `other` (same kind), all or nothing: BadValue when it is of another kind or breaks an invariant. The generations of what
  // differs advance.
  Status assign(const Settings& other) noexcept;

  // Every value in range, the unused bytes zero, the store valid and of this scope.
  bool check_invariants() const noexcept;

  // The raw bytes of the values (the owner struct of the descriptors' offsets), for param_load / param_store.
  const std::uint8_t* values() const noexcept { return raw_; }

 private:
  core::ParamKind kind_;
  std::uint8_t raw_[kValueBytes];
  std::uint32_t gen_[kMaxFields];
  std::uint32_t epoch_;      // added to every generation; init() advances it, which moves all of them
  std::uint32_t learn_gen_;
  core::LearnBinding learn_[core::kLearnSlots];
};

// (kind_ and the value bytes fill whole words: 1 + 32 = 33 bytes, 36 with the pad)
static_assert(sizeof(Settings) == ((1 + Settings::kValueBytes + 3) & ~3u) + 4 * Settings::kMaxFields + 4 + 4 + core::kLearnSlots * 8 && alignof(Settings) == 4,
              "Settings layout (it is part of the project arena)");

}  // namespace o3os::model
