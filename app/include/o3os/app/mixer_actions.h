// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Mixer screen's actions (mixer_actions.cpp): what is shared beyond that file.

#include <o3os/app/actions.h>

#include <cstdint>

namespace o3os::app {

// OS-MIDICCMAP S2: sets a bus level to an absolute value 0..100 — the board's copy and the tell to the engine in the same write, exactly as a turn of the encoder that lands
// on `value` does (edit_level lands on this). The CC map's CC 82 (the INPUT strip) comes through here; the model's levels (the tracks, the master's) are parameters and go
// through write_param. No history: the board has none.
bool set_level(ActionContext& ctx, std::uint8_t& level, unsigned value, unsigned strip, bool master_field, unsigned field) noexcept;

}  // namespace o3os::app
