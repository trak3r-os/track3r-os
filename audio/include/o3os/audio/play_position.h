// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where the sequencer is, for the screen (OS-009b). The render context (Sequencer::step(), once a block) is the ONE writer; the main loop reads it once
// per pass. No lock, no queue, nothing allocated: the whole position is one 32-bit word (playing, pattern, song row, step, and since OS-LIVEREC the microstep the
// playhead is in, 0..47 of the 48 a step has), so the five fields always come together (OS-005e: the song row is what the log says and a Song screen
// will mark), and the tempo is a second word of its own (it is shown, it is never matched against the step). Both accesses are relaxed: no other data is published with them, the screen draws from the project, which the main loop owns.
//
//   writer (render):   position.publish(playing, pattern, step, tempo_x100, order);
//   reader (main):     const PlayPosition::Value v = position.read();
//
// A std::atomic<uint32_t> is a plain load and store on the Cortex-M4 (lock free), and on the host.

#include <atomic>
#include <cstdint>

namespace o3os::audio {

class PlayPosition {
 public:
  static constexpr std::uint16_t kNoOrder = 0xFFFF;

  struct Value {
    bool playing = false;
    std::uint8_t pattern = 0;       // the pattern the song cursor is in (pattern id, 0..255)
    std::uint16_t order = kNoOrder; // its position in the song's order list (0..254), kNoOrder in Pattern mode
    std::uint16_t step = 0;         // the song cursor's step in it (0..127: a pattern has at most 128 steps)
    std::uint8_t ustep = 0;         // the microstep of that step the playhead is in (0..47; swing and tempo are in it: OS-LIVEREC rounds a live note to the nearest step by it)
    std::uint32_t tempo_x100 = 0;   // BPM x 100 in force; 0 before the first publish
    std::uint16_t queued = kNoOrder;  // the song position a live jump waits for (Sequencer::queue_jump), kNoOrder for none (OS-SONG1)
    std::uint8_t count_in = 0;        // OS-METRO: the steps of the count-in still to run, the one running included (0: not counting in); `step` stays on row 1 (0) meanwhile (OS-COUNTIN: the count-in counts on row 1, the pattern is silent)
    std::uint16_t loop_first = kNoOrder;  // OS-SONG3 S7: the slots the song loops (Sequencer::set_loop), both kNoOrder for none; loop_first <= loop_last
    std::uint16_t loop_last = kNoOrder;
    bool looping() const noexcept { return loop_first != kNoOrder; }
  };

  // Render context only (the one writer). `order` >= 255 is "none" (the order list has at most 255 positions, 0..254). `ustep` is held at 47.
  void publish(bool playing, std::uint32_t pattern, std::uint32_t step, std::uint32_t tempo_x100, std::uint32_t order = kNoOrder, std::uint32_t ustep = 0) noexcept {
    const std::uint32_t row = order < 0xFFu ? order : 0xFFu;
    const std::uint32_t st = step < 0xFFu ? step : 0xFFu;
    const std::uint32_t us = ustep < kMicrosteps ? ustep : kMicrosteps - 1u;
    where_.store((playing ? kPlaying : 0u) | (us << kUstepShift) | ((pattern & 0xFFu) << kPatternShift) | (row << kOrderShift) | st, std::memory_order_relaxed);
    tempo_.store(tempo_x100, std::memory_order_relaxed);
  }

  // The live jump that waits, one word of its own (the Song screen blinks the slot; it is never matched against the step). Render context only.
  void publish_queued(std::uint32_t order) noexcept { queued_.store(order < 0xFFFFu ? order : 0xFFFFu, std::memory_order_relaxed); }

  // OS-SONG3 S7: the looped range of slots, one halfword (first | last << 8; 0xFFFF none, which no range is: a position is 0..254). Render context only (the one writer); it fills the padding of the class.
  void publish_loop(std::uint32_t first, std::uint32_t last) noexcept {
    loop_.store(first < 0xFFu && last < 0xFFu ? static_cast<std::uint16_t>(first | (last << 8)) : std::uint16_t{0xFFFFu}, std::memory_order_relaxed);
  }

  // The count-in (OS-METRO), one word of its own like the live jump: the steps left, the running one included, 0 when the transport is not counting in. Render context only.
  void publish_count_in(std::uint32_t steps) noexcept { count_in_.store(steps < 0xFFu ? steps : 0xFFu, std::memory_order_relaxed); }

  // E-PERF-07: the per-track source pattern (Sequencer::queue_track_pattern), one word a track like the live jump: bits 0..7 the override that plays (0: the song's), 8..15 the pattern
  // that waits for the track's loop end, bit 16 set: one waits. Render context only (the one writer); the main loop reads track_pattern(). Tracks past kTrackWords are nothing.
  // Stored as 12 halfwords (the tracks Perform reaches, seq::kPerfTracks: the Sequencer asserts it) and one mask of the waiting tracks, 26 B: sizeof(Sequencer) is 8824 B on the target (the linker map, E-PERF-07), 8 B under the 8832 B ceiling of app_main.cpp. The
  // pattern and the wait are two stores, so the screen can see one block's change half made for a pass: it only draws it.
  static constexpr unsigned kTrackWords = 12;
  void publish_track_pattern(unsigned track, std::uint32_t word) noexcept {
    if (track < kTrackWords) {
      tracks_[track].store(static_cast<std::uint16_t>(word & 0xFFFFu), std::memory_order_relaxed);
      const std::uint16_t bit = static_cast<std::uint16_t>(1u << track);
      const std::uint16_t mask = track_wait_.load(std::memory_order_relaxed);
      track_wait_.store(static_cast<std::uint16_t>((word & 0x10000u) != 0u ? (mask | bit) : (mask & ~bit)), std::memory_order_relaxed);
    }
  }
  std::uint32_t track_pattern(unsigned track) const noexcept {
    if (track >= kTrackWords) {
      return 0u;
    }
    return tracks_[track].load(std::memory_order_relaxed) | (((track_wait_.load(std::memory_order_relaxed) >> track) & 1u) != 0u ? 0x10000u : 0u);
  }

  // Any context; the main loop's once-a-pass read.
  Value read() const noexcept {
    const std::uint32_t w = where_.load(std::memory_order_relaxed);
    Value v;
    v.playing = (w & kPlaying) != 0u;
    v.pattern = static_cast<std::uint8_t>((w >> kPatternShift) & 0xFFu);
    const std::uint32_t row = (w >> kOrderShift) & 0xFFu;
    v.order = row == 0xFFu ? kNoOrder : static_cast<std::uint16_t>(row);
    v.step = static_cast<std::uint16_t>(w & 0xFFu);
    v.ustep = static_cast<std::uint8_t>((w >> kUstepShift) & 0x3Fu);
    v.tempo_x100 = tempo_.load(std::memory_order_relaxed);
    v.queued = static_cast<std::uint16_t>(queued_.load(std::memory_order_relaxed));
    v.count_in = static_cast<std::uint8_t>(count_in_.load(std::memory_order_relaxed));
    const std::uint16_t lp = loop_.load(std::memory_order_relaxed);
    if (lp != 0xFFFFu) {
      v.loop_first = lp & 0xFFu;
      v.loop_last = lp >> 8;
    }
    return v;
  }

 private:
  static constexpr std::uint32_t kPlaying = 1u << 31;
  static constexpr std::uint32_t kMicrosteps = 48;  // a step (seq::kMicrostepsPerStep; this header cannot include the sequencer's)
  static constexpr unsigned kUstepShift = 24;       // bits 24..29
  static constexpr unsigned kPatternShift = 16;
  static constexpr unsigned kOrderShift = 8;

  std::atomic<std::uint32_t> where_{0};
  std::atomic<std::uint32_t> tempo_{0};
  std::atomic<std::uint32_t> queued_{0xFFFFu};
  std::atomic<std::uint32_t> count_in_{0};
  std::atomic<std::uint16_t> tracks_[kTrackWords] = {};  // E-PERF-07: override | pending << 8 (render context writes, main loop reads)
  std::atomic<std::uint16_t> track_wait_{0};             // bit t: track t waits for its loop end
  std::atomic<std::uint16_t> loop_{0xFFFFu};             // OS-SONG3 S7: first | last << 8, 0xFFFF none
};

}  // namespace o3os::audio
