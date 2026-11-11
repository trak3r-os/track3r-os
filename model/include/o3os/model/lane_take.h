// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC-P1 S3: the take buffer of a Lane recording (spec A8..A11). The main loop appends the stamped MIDI of one take here
// (notes with their LEN, CC points thinned per A9), and at the take's end `finish` closes the held notes, drops the co-linear CC
// points, sorts the events by tick and works out the tick intervals the take covers; `Editor::lane_merge_take` then merges it into
// the lane (a CC controller the take moved replaces the lane's old points of that controller inside ITS OWN range: from its first to its last
// message of the take, not the take's whole span).
//
// Pure data and rules: no Project, no store, no allocation. The object is large (kCapacity x 8 bytes) and lives in SDRAM; it has no
// default member initialisers (it is placed in SDRAM_NOINIT) and `reset()` makes it a closed, empty take.
//
// Size decision (S3, orchestrator, 2026-11-10): the spec's 4,096-event buffer does not fit SDRAM_STATIC (about 31 KB free), so the take
// holds kCapacity = 2,048 events (16 KiB). A take that fills it is committed as it is, with a TAKE FULL notice (the App), never lost.
//
// Threading: main loop only (the render's side is the stamped ring of CcInput).
#pragma once

#include <o3os/model/lane_store.h>

#include <cstdint>

namespace o3os::model {

class LaneTake {
 public:
  static constexpr std::uint32_t kCapacity = 2048;  // events (16 KiB)
  static constexpr std::uint32_t kThinTicks = 12;   // A9: at most one point per controller per 12 ticks (a quarter of a step)
  static constexpr std::uint32_t kDeadband = 1;     // A9: a point that differs from the last kept one by less than this is dropped

  struct Interval {
    std::uint16_t t0;  // first tick
    std::uint16_t t1;  // one past the last tick
  };

  void reset() noexcept;  // closed and empty

  // Opens a take of (pattern, track) whose events carry `channel` / `port` and start at `start_tick` of a track `length_ticks` long.
  void begin(std::uint8_t pattern, std::uint8_t track, std::uint8_t channel, std::uint8_t port, std::uint32_t start_tick,
             std::uint32_t length_ticks) noexcept;
  bool open() const noexcept { return open_; }
  bool finished() const noexcept { return finished_; }

  // Feed, in the order the render stamped them. They are ignored on a take that is not open; each returns false when it could not store
  // (the buffer is full: `full()` is then true and the caller commits the take).
  bool note_on(std::uint32_t tick, std::uint8_t note, std::uint8_t velocity) noexcept;
  void note_off(std::uint32_t tick, std::uint8_t note) noexcept;
  // S5 (STEP mode): a note whose length is known now (a step of 48 ticks): one closed event, no hold to follow; a chord is several calls at one tick.
  bool note_len(std::uint32_t tick, std::uint8_t note, std::uint8_t velocity, std::uint32_t len_ticks) noexcept;
  void all_notes_off(std::uint32_t tick) noexcept;
  bool cc(std::uint32_t tick, std::uint8_t controller, std::uint8_t value) noexcept;

  // Closes the take at `end_tick`: held notes get their LEN up to it, the pending last point of every moved CC is kept, the co-linear
  // points go (A9), the events are sorted by tick (stable) and the intervals are set. Closes the take (open() false), keeps the events.
  void finish(std::uint32_t end_tick) noexcept;

  bool full() const noexcept { return full_; }  // a feed did not fit: finish and commit what is here
  std::uint32_t count() const noexcept { return count_; }
  const LaneEvent* events() const noexcept { return events_; }  // sorted by tick after finish
  std::uint8_t pattern() const noexcept { return pattern_; }
  std::uint8_t track() const noexcept { return track_; }
  std::uint8_t channel() const noexcept { return channel_; }
  std::uint8_t port() const noexcept { return port_; }
  std::uint32_t length_ticks() const noexcept { return length_; }
  std::uint32_t start_tick() const noexcept { return start_; }

  // After finish: the tick intervals the take covered (one, or two when it wrapped past the end of the track, the higher first). Which events
  // of the lane they replace is per controller (spec A10): `replaces(c, tick)` is true for a tick from the controller's first to its last
  // message of the take (a controller whose messages passed the end of the track covers the loop's end and start, or all of it).
  std::uint32_t interval_count() const noexcept { return intervals_n_; }
  Interval interval(std::uint32_t i) const noexcept { return intervals_[i]; }
  bool moved(std::uint8_t controller) const noexcept { return controller < 128 && cc_[controller].has_kept != 0; }
  bool replaces(std::uint8_t controller, std::uint32_t tick) const noexcept;

 private:
  struct CcState {
    std::uint16_t kept_tick;
    std::uint16_t pend_tick;
    std::uint16_t first_tick;  // the first and the last message of the controller in the take (kept or not): its replace range (A10)
    std::uint16_t last_tick;
    std::uint8_t kept_value;
    std::uint8_t pend_value;
    std::uint8_t has_kept;  // the controller has had a message in this take (the first one is always kept)
    std::uint8_t has_pend;
    std::uint8_t wrapped;   // a message came at a lower tick than the one before: the playhead passed the end of the track
  };
  bool append(const LaneEvent& e) noexcept;
  void flush_pending(std::uint8_t controller) noexcept;
  void close_note(std::uint8_t note, std::uint32_t tick) noexcept;
  void note_wrap(std::uint32_t tick) noexcept;
  void drop_colinear() noexcept;
  void sort_events() noexcept;
  void sym_merge(std::uint32_t a, std::uint32_t m, std::uint32_t b) noexcept;
  void rotate(std::uint32_t a, std::uint32_t m, std::uint32_t b) noexcept;
  // A slot is kept for every pending point, so that the last point of a move is never lost to a full buffer (finish() flushes them into those slots).
  bool room() const noexcept { return count_ + pend_n_ < kCapacity; }

  LaneEvent events_[kCapacity];
  std::uint16_t held_[128];  // per note: index + 1 of its open note event, 0 when not held
  CcState cc_[128];
  Interval intervals_[2];
  std::uint32_t count_;
  std::uint32_t pend_n_;  // controllers with a pending point (a slot is kept for each)
  std::uint32_t start_;
  std::uint32_t length_;
  std::uint32_t last_tick_;
  std::uint32_t intervals_n_;
  std::uint8_t pattern_;
  std::uint8_t track_;
  std::uint8_t channel_;
  std::uint8_t port_;
  bool open_;
  bool finished_;
  bool full_;
  bool wrapped_;
};

}  // namespace o3os::model
