// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The clock a module reads through host->get_clock (ABI 1.12, OS-INSFX-ABI2 A3): 16 bytes, one object, in SRAM_U (.bss). The engine's render context writes it once per block at the block boundary
// (Engine::block_end after the block, Engine::render_offline before one: values as of the block that comes next, from the event source's `clock`, audio/event_source.h); host_get_clock reads it.
// Every reader that runs in the render context (instruments and effects) sees one block's values whole. An app (the main loop) may read it too: each word is atomic, and a read that lands between the
// writer's four stores mixes two blocks' values, which a display does not mind. Nothing allocates, blocks or loops. `valid` 0 is "no sequencer behind it" (boot, a test without a source): get_clock then
// answers 1 and the stopped project tempo of the stub.

#include <atomic>
#include <cstdint>

namespace o3os::module {

struct ClockState {
  std::atomic<std::uint32_t> flags{0};        // O3M_CLOCK_PLAYING 1, STARTED 2, WRAPPED 4
  std::atomic<std::uint32_t> tempo_x100{0};   // BPM x 100: what plays, half and double time included; the project tempo while stopped
  std::atomic<std::uint32_t> beat_q16{0};     // beats (4 steps) from step 0 of the pattern that plays, Q16.16; 0 while stopped
  std::atomic<std::uint32_t> valid{0};        // 1 once a source has written it

  void publish(std::uint32_t f, std::uint32_t tempo, std::uint32_t beat) noexcept {
    flags.store(f, std::memory_order_relaxed);
    tempo_x100.store(tempo, std::memory_order_relaxed);
    beat_q16.store(beat, std::memory_order_relaxed);
    valid.store(1u, std::memory_order_release);
  }
  void invalidate() noexcept { valid.store(0u, std::memory_order_relaxed); }
  // False: nothing written (the outputs are untouched).
  bool read(std::uint32_t& f, std::uint32_t& tempo, std::uint32_t& beat) const noexcept {
    if (valid.load(std::memory_order_acquire) == 0u) {
      return false;
    }
    f = flags.load(std::memory_order_relaxed);
    tempo = tempo_x100.load(std::memory_order_relaxed);
    beat = beat_q16.load(std::memory_order_relaxed);
    return true;
  }
};
static_assert(sizeof(ClockState) == 16, "the clock's store is 16 bytes of SRAM_U (OS-INSFX-ABI2 A3)");

// The one store (inline: one object across translation units; zero-initialised, so it is .bss).
inline ClockState g_clock;

}  // namespace o3os::module
