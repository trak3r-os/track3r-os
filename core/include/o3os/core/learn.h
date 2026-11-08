// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI Learn (ADR 0009 rev 1, ADR 0013 D6, docs/design/product-ux.md workflow 3 and decision 8; OS-002d): a control change
// message is bound to the parameter under the cursor, and from then on moves it.
//
// The store is a fixed array of bindings (a slot is empty when `param` is None). There are two stores, as the maintainer decided
// (decision 8, "two stores: device and project"): the DEVICE store holds what is about the controller and the mixer (the
// parameters of kind Track, Master and Device), the PROJECT store what is about the music (Instrument, Project and, OS-INSFX S7, Insert): the store
// of a parameter is given by its kind (learn_scope). A third idea, "follow the selected instrument", is a flag of a binding, not
// a store: a follow binding names an instrument parameter by its field and acts on whichever instrument the UI has selected.
//
// A binding moves a parameter in one of three ways (`mode`, per binding):
//   Pickup    the default (maintainer answer Q4): an absolute CC does nothing until the knob has reached the parameter, so a
//             parameter never jumps. Reached means the knob's position is within one step of the parameter's, or it crossed it
//             since the last message. When the parameter is changed by anything else (the screen, undo, another binding) the knob
//             has to be caught again.
//   Jump      the parameter takes the knob's position at once.
//   Relative  the CC is an increment: values 1..63 count up by that many detents, 65..127 down by 128 - v (two's complement
//             "relative 1"), 0 and 64 nothing. PROVISIONAL: this encoding is the common one, not sourced.
//
// This header is the data and the pure rules; the app applies them (app/param_actions.cpp), the model stores them
// (model/settings.h, and the project keeps its store in the file: LRNB chunk, docs/formats/o3-settings.md). Nothing here
// allocates, and the runtime state (LearnRuntime) is never persisted.

#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::core {

enum class LearnMode : std::uint8_t { Pickup = 0, Jump = 1, Relative = 2 };
inline constexpr unsigned kLearnModeCount = 3;

inline constexpr std::uint8_t kLearnFollow = 1u << 0;  // the target is the selected instrument's parameter of this field

inline constexpr unsigned kLearnSlots = 32;  // per store. PROVISIONAL, like the keymap pools (16 bytes a binding there, 8 here)

struct LearnBinding {
  ParamId param;          // None: an empty slot. A follow binding keeps index 0 here; only its kind and field count
  std::uint8_t cc;        // controller number, 0..127
  std::uint8_t channel;   // 0 = every channel, else 1..16
  std::uint8_t mode;      // LearnMode
  std::uint8_t flags;     // kLearnFollow
};
static_assert(sizeof(LearnBinding) == 8 && alignof(LearnBinding) == 4, "LearnBinding is two words (journal records and LRNB chunks copy it)");

constexpr bool learn_empty(const LearnBinding& b) noexcept { return b.param == ParamId::None; }

enum class LearnScope : std::uint8_t { Project, Device };
// Which store holds a binding of a parameter of this kind (the maintainer's split: device = controller and mixer, project = music).
constexpr LearnScope learn_scope(ParamKind kind) noexcept {
  return (kind == ParamKind::Track || kind == ParamKind::Master || kind == ParamKind::Device) ? LearnScope::Device
                                                                                              : LearnScope::Project;
}

// An empty slot is all zero; a used one has a parameter of a kind that can be bound, a controller 0..127, a channel 0..16, a
// known mode and no unknown flag; a follow binding names an indexed kind (Instrument) with index 0. An Insert binding (OS-INSFX S7)
// names an id this version has (insert_param_valid: index 0..32, field 0..31) and never follows: the project file's LRNB reader of an
// older version does not know the kind, so it skips such an entry and counts it (this rule), and drops it on its next save.
bool learn_valid(const LearnBinding& b) noexcept;

// Does the binding act on `id`: the same parameter, or (follow) the same field of the same kind whatever the index.
bool learn_targets(const LearnBinding& b, ParamId id) noexcept;
// Do two bindings listen to the same message (same controller, channels that overlap: 0 overlaps everything).
bool learn_overlap(const LearnBinding& a, const LearnBinding& b) noexcept;

// The slot that acts on `id`, or -1.
int learn_find_param(const LearnBinding* slots, unsigned count, ParamId id) noexcept;
// The first slot at or after `start` that listens to controller `cc` on `channel` (1..16; 0 matches every binding), or -1.
int learn_find_cc(const LearnBinding* slots, unsigned count, unsigned channel, unsigned cc, unsigned start = 0) noexcept;

// Binding `b` into a store. One controller moves one parameter, so a binding takes over the message: `write` is the slot to write (the
// one already bound to the same parameter; else the first of the bindings it displaces; else the first empty one) and `clear` the slots
// to empty (bit n = slot n: every other binding that listens to the same message, which a binding on every channel can be several of).
// False when the store is full and the parameter is new.
struct LearnPlan {
  int write;
  std::uint32_t clear;
};
static_assert(kLearnSlots <= 32, "LearnPlan::clear is a bit set of the slots");
bool learn_plan(const LearnBinding* slots, unsigned count, const LearnBinding& b, LearnPlan& out) noexcept;

// Every slot valid, no parameter twice, no two bindings listening to the same message.
bool learn_store_valid(const LearnBinding* slots, unsigned count) noexcept;

// The state a binding needs between messages, for the parameter it last acted on (a follow binding acts on whichever instrument is
// selected). Not persisted; a zeroed one is a binding that has not caught anything yet. Its state is valid for one parameter at one
// generation: any other and the next message starts afresh (picked and last forgotten), so a knob never "crosses" a value it was
// never compared with, and an edit by anyone else (a screen, undo, a load) is always something to catch up with.
inline constexpr std::uint8_t kLearnHasLast = 1u << 0;  // `last` holds a controller value
inline constexpr std::uint8_t kLearnPicked = 1u << 1;   // Pickup: the knob has reached the parameter
struct LearnRuntime {
  std::uint32_t gen;    // the generation of the parameter the state was last valid at
  std::uint16_t key;    // learn_key of that parameter
  std::uint8_t last;    // the last controller value seen
  std::uint8_t flags;   // kLearn* above
};
static_assert(sizeof(LearnRuntime) == 8, "one LearnRuntime a slot, two stores of kLearnSlots: eight bytes each");

// A 16-bit key of a parameter for LearnRuntime: two parameters of the same kind that differ in the index or in the field differ in it (a
// follow binding changes only the index; a slot that is rewritten has its state reset anyway), and a zeroed state is valid for any key.
constexpr std::uint16_t learn_key(ParamId id) noexcept {
  return static_cast<std::uint16_t>(((static_cast<std::uint32_t>(id) >> 16) & 0xFFFFu) ^ (static_cast<std::uint32_t>(id) & 0xFFFFu));
}

// An absolute controller value (0..127) arrives for parameter `id`. `current_norm` is where the parameter is (param_norm) and
// `current_gen` its generation now. True: write `norm_out` (0..1) to the parameter and then call learn_applied with its new
// generation. False: ignore the message (Pickup, the knob has not reached the parameter yet).
bool learn_absolute(LearnRuntime& rt, ParamId id, LearnMode mode, unsigned cc_value, float current_norm, std::uint32_t current_gen,
                    float& norm_out) noexcept;
// A relative controller value as a number of detents (see above): 0 for none.
int learn_relative_delta(unsigned cc_value) noexcept;
// Remember the generation the parameter has after this binding wrote it, so that a change by anyone else is noticed.
inline void learn_applied(LearnRuntime& rt, ParamId id, std::uint32_t new_gen) noexcept {
  rt.gen = new_gen;
  rt.key = learn_key(id);
}

}  // namespace o3os::core
