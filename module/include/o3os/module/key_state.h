// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sidechain key a module reads through host->track_level (ABI 1.12, OS-INSFX-ABI2 A6): one word a track, 64 bytes, and the mask of the tracks some running insert names as its key, in SRAM_U (.bss).
// The render context writes both: a strip that is a key measures the peak of its mono block after its inserts and before the fader (the insert host's key tap, or the module in insert slot B, audio/insert_host.h)
// and stores it with the RAN flag; the block boundary (InsertHost::begin_block) ages the words, so a word that still has the flag was measured in THIS block and one without it in the block before:
// track_level(t) answers the peak either way, which is exactly "this block's when the track's strip already ran, else the previous block's" (a key on a lower track is at most this block's, on a higher one a
// block late, 1.45 ms). A track that did not run for a whole block reads 0 from the next boundary on, and a track no insert names reads 0 whatever its word holds. Nothing allocates, blocks or loops
// but the boundary's 16-word walk (in flash).

#include <atomic>
#include <cstdint>

namespace o3os::module {

struct KeyLevels {
  static constexpr unsigned kTracks = 16;
  static constexpr std::uint32_t kRan = 0x80000000u;  // the strip ran in this block
  static constexpr std::uint32_t kPeak = 0xFFFFu;     // the peak, 0..32768 (the Q15 magnitude of -32768 is 32768)
  std::atomic<std::uint32_t> word[kTracks];           // peak | kRan
  std::atomic<std::uint16_t> mask{0};                 // bit t: some running insert names track t as its key

  void store(unsigned track, std::uint32_t peak) noexcept { word[track].store((peak & kPeak) | kRan, std::memory_order_relaxed); }
  // The linear peak 0..1 of `track` (0: a bad index, a track nobody names, a silent one).
  float level(std::uint32_t track) const noexcept {
    if (track >= kTracks || ((mask.load(std::memory_order_relaxed) >> track) & 1u) == 0u) {
      return 0.0f;
    }
    return static_cast<float>(word[track].load(std::memory_order_relaxed) & kPeak) * (1.0f / 32768.0f);
  }
  // The block boundary, for the tracks in `m`: a word measured in the block that ended becomes the previous block's (the flag goes), one that was not measured is 0.
  void age(std::uint16_t m) noexcept {
    for (; m != 0u; m = static_cast<std::uint16_t>(m & (m - 1u))) {
      const unsigned t = static_cast<unsigned>(__builtin_ctz(m));
      const std::uint32_t w = word[t].load(std::memory_order_relaxed);
      word[t].store((w & kRan) != 0u ? (w & kPeak) : 0u, std::memory_order_relaxed);
    }
  }
  // The tracks in `m` are no longer keys: their words are forgotten (a track that is named again starts from 0).
  void forget(std::uint16_t m) noexcept {
    for (; m != 0u; m = static_cast<std::uint16_t>(m & (m - 1u))) {
      word[__builtin_ctz(m)].store(0u, std::memory_order_relaxed);
    }
  }
  void clear_words() noexcept {
    for (unsigned t = 0; t < kTracks; ++t) {
      word[t].store(0u, std::memory_order_relaxed);
    }
  }
  void clear() noexcept {
    clear_words();
    mask.store(0, std::memory_order_relaxed);
  }
};
static_assert(sizeof(KeyLevels) == 68, "the key store is 68 bytes of SRAM_U (OS-INSFX-ABI2 A6)");

// The one store (inline: one object across translation units; zero-initialised, so it is .bss).
inline KeyLevels g_keys;

}  // namespace o3os::module
