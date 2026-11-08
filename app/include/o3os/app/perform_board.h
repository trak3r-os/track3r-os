// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The Perform screen's runtime side (OS-011b): the bank of slots (seq/perform.h, which holds the rules and what is told to the engine) and where its commands go.
// The slots (the FX, the selected row and the stored values) are the project's since OS-004d (the PERF chunk;
// triage BASE-20): App::sync_project_state copies a change into the project and a load puts them back (PerformBank::load_slots: every FX off, no track affected). The live state is not stored.
//
// About 0.5 KB. The platform places it in SDRAM behind the sample arena's other tail parts (platform/mk66/src/app_main.cpp), not in the main loop's SRAM_U. The main loop is its only
// user; the commands go out through `out`, which on the device is the sequencer's command ring (the main loop is its one producer).

#include <o3os/core/tick.h>
#include <o3os/seq/perform.h>
#include <cstdint>

namespace o3os::app {

// The function keys' runtime state (OS-PERF2: F1..F3 scenes, F4 / F5 half and double time while held). Main loop only; nothing here is stored.
struct PerformKeys {
  static constexpr std::uint32_t kSceneHoldMs = 1000;  // a scene key held this long stores the scene (a tap recalls it)
  std::uint8_t down = 0;                               // bit n: F(n+1) went down on the Perform screen and has not been let go
  std::uint8_t fired = 0;                              // bit n (a scene key): its hold stored the scene already, so the release recalls nothing
  core::Tick at[seq::kPerfScenes] = {};                // when a scene key went down
};

struct PerformBoard {
  seq::PerformBank bank;
  PerformKeys keys;
  seq::CommandSink* out = nullptr;  // nullptr: the screen works and tells no engine (the simulator, a bare test)
  std::uint8_t pick = 0;            // E-PERF-07: the pattern Shift + F4 / F5 picked for Shift + F6 (TRK PAT); 0: none yet (the pattern that plays). Live state, not stored
  std::uint64_t pads = 0;          // OS-QAFIX-E-PERF-08: the pads whose press went to the bank (perform.pad) and are still down (bit n = pad n); main loop only. Their release on Master / Song is forwarded, no other's
  // OS-MIDICCMAP S4 (CC 41..48): the last pattern the fixed CC map queued on each of tracks 1..8 (cc_pattern[t] = pattern id 1..255, 0: none sent yet), so a value the map already sent is not
  // sent again and a fader resending it never takes the queue back (F12). Cleared when the run stops (clear_cc_pattern, called from App::poll_play_position). Runtime only, not stored
  std::uint8_t cc_pattern[8] = {};
  // Q5 a (kept from OS-PERFREMIX R2, OS-NOREMIX 2026-11-08): the tracks that play another pattern (TRK PAT, E-PERF-07, the CC map) when Perform was left, marked to go back to the song at the next
  // pass (App::poll_perform sends one SetTrackPattern 0 for the mask; a ring that refuses is retried; a Stop drops it). Main loop only. Live state, not stored
  std::uint16_t back = 0;

  void clear_cc_pattern() noexcept {
    for (unsigned t = 0; t < 8u; ++t) {
      cc_pattern[t] = 0;
    }
  }

  // The sink the bank is applied to: the engine's, or one that takes everything and keeps nothing.
  seq::CommandSink& sink() noexcept {
    static DiscardSink discard;
    return out != nullptr ? *out : static_cast<seq::CommandSink&>(discard);
  }

 private:
  struct DiscardSink final : seq::CommandSink {
    bool push(const seq::Command&) noexcept override { return true; }
  };
};

}  // namespace o3os::app
