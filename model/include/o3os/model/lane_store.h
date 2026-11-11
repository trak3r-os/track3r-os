// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MIDIREC-P1 S1 (docs/specs/OS-MIDIREC-P1.md, A1-A3, "Model"): the MIDI take lanes of a project. A lane is the
// recorded events of one track of one pattern (A2), sorted by tick (1/48 of a step, A1), ties in insertion order.
// The store is one directory plus one pool of events; it is NOT part of `Project` / `ModelArena` (the arena table
// and every pinned layout size stay): the app owns one store beside the project, and `Editor::attach_lanes` and
// the native Reader / Writer take a pointer to it. A project that never uses a lane costs nothing (A19).
//
// Key and directory: the key of a lane is `(pattern - 1) * 16 + track` with the pattern ID 1..255 (the ID the
// file's SONG and PATT chunks use) and the track 0..15, so there are 4080 keys; the directory holds `{offset,
// count}` per key (4 bytes, 16,320 bytes). Spec deviation, recorded in docs/formats/o3-project.md: the spec's
// "pattern 0..127" is the pool slot; the lane follows the ID, which is what the file and the Song carry.
//
// Layout: the lanes with events lie in the pool packed in key order with no gaps, so a lane is found in O(1)
// and the whole store is always compact (no compaction pass). An insert or an erase moves the tail of the pool
// (at most 60 KiB, one memmove, once per take commit or edit, in the main loop). Spec deviation: the spec
// describes a lane moved to the pool's end plus a compaction pass; packing is simpler and needs no pass.
//
// Capacity (limits.h): 4096 events a lane, 7,680 in all (the 60 KiB pool), 2048 lanes with events.
// Size in memory: 77,780 bytes (7,680 x 8 + 16,320 of directory, plus the bookkeeping words; kLaneStoreBytes mirrors it). S2 places the one
// instance in the delay region's spare (decision 6a, amended 2026-11-10: the 4.0 s ring leaves 80,544 B; mem_map.h proves it fits).
//
// Threading (S2): the writer is the main loop (Editor, native Reader, the app's load); the reader is the sequencer's
// render, which preempts the main loop on the single core, so it can see a mutation half done. Protocol (a seqlock
// with one writer): `seq()` is odd while a mutation is in progress (every mutator and `clear` bump it around the
// work, and `begin_write` / `end_write` extend the window over the caller's own fill of an open gap); the render
// reads it once at the start of a block, and a block that finds it odd reads no lane this block: its events stay
// due and the cursor catches up when the window has closed (a late event, never a lost one), and the notes the
// render already holds end on time, because their lengths are the sequencer's own. The window is one memmove of
// the pool's tail (at most 60 KiB, twice for a replace) in SDRAM: its time is NOT measured yet (a few ms is
// plausible; spec gate 4, docs/architecture/cpu-budget.md), the sequencer counts the blocks it met it in
// (Stats::lane_odd). A render never runs inside a window of its own block, because the writer cannot run during
// it. A cursor the render keeps into a lane is valid only while `seq()` is the value it was taken at: it
// re-seeks when it changed.
//
// Epoch: a project load or reset (`Project::load_epoch()`) invalidates every lane. `sync(epoch)` clears a store
// that belongs to another epoch and adopts the new one; the Editor calls it before every lane edit, the Reader
// adopts the loaded project's epoch, and the Writer treats a store of another epoch as empty. The app should
// call `sync` after a load or a new project so that a stale store is dropped at once.

#include <o3os/model/limits.h>

#include <cstddef>
#include <cstdint>

namespace o3os::model {

// A3: 8 bytes in memory and in the file (big-endian there).
struct LaneEvent {
  std::uint16_t tick;         // 0..6143 (kLaneTicks - 1), in the pattern's own track timeline
  std::uint16_t len;          // ticks; notes only (1..6144), else 0
  std::uint8_t kind_channel;  // kind << 4 | channel; kind 0 note, 1 CC, 2 program, 3 channel AT, 4 poly AT, 5 pitch bend
  std::uint8_t port;          // low 2 bits (the value midi_route.h uses for DIN / USB); the rest 0
  std::uint8_t a;             // note, controller, program, pressure, note for poly AT, pitch bend LSB (0..127)
  std::uint8_t b;             // velocity, value, pitch bend MSB (0..127)
};
static_assert(sizeof(LaneEvent) == 8, "A3: the lane event is 8 bytes in memory and in the file");
static_assert(alignof(LaneEvent) == 2);

// True when the event holds only values the file and the sequencer accept (A3).
bool lane_event_valid(const LaneEvent& e) noexcept;

// True when `n` events are valid, sorted by tick and all inside [tick_lo, tick_hi).
bool lane_events_valid(const LaneEvent* e, std::uint32_t n, std::uint32_t tick_lo, std::uint32_t tick_hi) noexcept;

class LaneStore {
 public:
  static constexpr std::uint32_t kKeys = kMaxPatternId * kMaxTracks;  // 4080

  struct View {
    const LaneEvent* events;
    std::uint32_t count;
  };

  // An empty store (the member initialisers run: where the instance lives, and how it is constructed there, is the app's to decide in S2).
  LaneStore() noexcept = default;

  // The key of a lane; `valid_key` says whether (pattern, track) is one (pattern ID 1..255, track 0..15).
  static constexpr bool valid_key(std::uint32_t pattern, std::uint32_t track) noexcept {
    return pattern >= 1 && pattern <= kMaxPatternId && track < kMaxTracks;
  }
  static constexpr std::uint32_t key_of(std::uint32_t pattern, std::uint32_t track) noexcept {
    return (pattern - 1) * kMaxTracks + track;
  }
  static constexpr std::uint32_t pattern_of(std::uint32_t key) noexcept { return key / kMaxTracks + 1; }
  static constexpr std::uint32_t track_of(std::uint32_t key) noexcept { return key % kMaxTracks; }

  // The index of the first event of the lane whose tick is at or past `tick` (the count when there is none); binary search.
  static std::uint32_t lower_bound(const View& lane, std::uint32_t tick) noexcept;

  void clear() noexcept;

  // The write window (see Threading). Mutators open and close it themselves; a caller that fills the gap
  // `open_gap` returns brackets the call and the fill with begin_write / end_write (nesting is counted).
  void begin_write() noexcept;
  void end_write() noexcept;
  std::uint32_t seq() const noexcept { return seq_; }  // odd: a mutation is in progress
  struct WriteWindow {
    explicit WriteWindow(LaneStore& s) noexcept : store(s) { store.begin_write(); }
    ~WriteWindow() { store.end_write(); }
    WriteWindow(const WriteWindow&) = delete;
    WriteWindow& operator=(const WriteWindow&) = delete;
    LaneStore& store;
  };

  // Epoch protocol (see the header comment).
  void sync(std::uint32_t epoch) noexcept;
  bool in_sync(std::uint32_t epoch) const noexcept { return epoch_ == epoch; }
  void adopt(std::uint32_t epoch) noexcept { epoch_ = epoch; }

  // Queries. An absent lane is {nullptr, 0}; the pointer stays valid until the next mutation.
  View lane(std::uint32_t pattern, std::uint32_t track) const noexcept;
  View lane_at(std::uint32_t key) const noexcept;
  // The lane's events for the Editor's in-place merge (OS-MIDIREC-P1 S3): writable, valid only inside a write window and until the next mutator; nullptr for an absent lane.
  LaneEvent* lane_writable(std::uint32_t pattern, std::uint32_t track) noexcept;
  std::uint32_t lane_count() const noexcept { return lanes_; }     // lanes with events
  std::uint32_t total_events() const noexcept { return events_; }  // in all lanes
  // The first key at or after `from` that holds events, or kKeys when there is none (ordered iteration).
  std::uint32_t next_key(std::uint32_t from) const noexcept;
  // How many events the pattern's lanes hold in all (delete_pattern, CLEAR).
  std::uint32_t pattern_events(std::uint32_t pattern) const noexcept;

  // Capacity check of a growth by `count` events: false when the lane would pass 4096, the pool 7,680, or a new
  // lane would be the 2049th.
  bool can_grow(std::uint32_t pattern, std::uint32_t track, std::uint32_t count) const noexcept;

  // Mutators. They keep the store packed and sorted by key; they never look at the events (the caller validates).
  // open_gap makes room for `count` events at `index` of the lane (0..lane count) and returns the first, which
  // the caller must fill at once; nullptr when the key is invalid, count is 0, the index is past the lane, or
  // can_grow is false (then nothing changes). erase removes [index, index + count); false (no change) when the
  // range is not inside the lane.
  LaneEvent* open_gap(std::uint32_t pattern, std::uint32_t track, std::uint32_t index, std::uint32_t count) noexcept;
  bool erase(std::uint32_t pattern, std::uint32_t track, std::uint32_t index, std::uint32_t count) noexcept;

 private:
  struct Slot {
    std::uint16_t offset;  // first event in pool_; meaningful only while count != 0
    std::uint16_t count;
  };
  std::uint32_t pool_pos(std::uint32_t key) const noexcept;  // where the lane of `key` starts (or would start)

  Slot dir_[kKeys] = {};
  volatile std::uint32_t seq_ = 0;  // single writer (main loop), read by the render: odd while a mutation is in progress; the stores to dir_ and pool_ stay between the two bumps (compiler fences)
  std::uint32_t depth_ = 0;         // nesting of begin_write (main loop only)
  std::uint32_t lanes_ = 0;
  std::uint32_t events_ = 0;
  std::uint32_t epoch_ = 0;
  alignas(4) LaneEvent pool_[kMaxLanePoolEvents];  // packed in key order; only [0, events_) is meaningful
};
static_assert(kMaxLanePoolEvents <= 0xFFFF && kMaxLaneEvents <= 0xFFFF, "directory slots are u16");

}  // namespace o3os::model
