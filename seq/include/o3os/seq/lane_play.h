// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// OS-MIDIREC-P1 S2: the state of the sequencer's lane playback (docs/specs/OS-MIDIREC-P1.md A4..A7, A17). The playback itself is in
// seq/src/sequencer_lanes.cpp (Sequencer::lane_*); this header holds the constants and the plain data, so a test can name them.
//
// Render context only (the one writer and the one reader of everything here is Sequencer::step()): no atomics, no allocation.
// Size: about 1.6 KB, owned by the caller and attached with Sequencer::attach_lanes (SDRAM on the unit: SRAM_U has no room, its 32 KiB floor) (16 cursors of about 90 bytes, a 16-entry pending table and a few words); the spec said 1.0 KB:
// the chase table (kLaneSlots controllers a track) is the difference, a documented deviation.

#include <cstdint>

namespace o3os::seq {

inline constexpr std::uint32_t kLaneTracks = 16;         // = kMaxTracks (checked in sequencer.h)
inline constexpr std::uint32_t kLaneHeldMax = 8;         // A5: lane notes sounding at once on a track; a ninth ends the oldest
inline constexpr std::uint32_t kLaneBlockEvents = 16;    // A7: lane events (and CC sends) one block handles; a due event over the cap waits
inline constexpr std::uint32_t kLaneCcPerSecond = 300;   // A7: control changes a second on one port (notes, program changes, pressures, bends exempt)
inline constexpr std::uint32_t kLanePending = 16;        // CCs waiting for their port's gap (the latest value of a controller wins)
inline constexpr std::uint32_t kLaneSlots = 8;           // A6: controllers a track keeps a chase record of
inline constexpr std::uint32_t kLaneChaseScan = 128;     // A6: lane events the chase looks at in one block (all tracks together)
inline constexpr std::uint32_t kLaneCatchUp = 192;       // ticks: a block that found the store mid-write is caught up when it closes, up to this far behind
inline constexpr std::uint16_t kLaneNoKey = 0xFFFF;
inline constexpr std::uint8_t kLaneNoValue = 0xFF;

struct LaneHeld {
  std::uint8_t note;
  std::uint8_t route;  // channel << 2 | port
  std::uint16_t left;  // microsteps until the note-off (A5: playback time, so a tempo change moves it with the music)
};
static_assert(sizeof(LaneHeld) == 4);

// The chase record of one controller (route, number) on one track.
struct LaneSlot {
  static constexpr std::uint8_t kUsed = 1;  // allocated
  static constexpr std::uint8_t kSeen = 2;  // playback or the chase has already put a value out: an older one must never follow
  static constexpr std::uint8_t kPend = 4;  // the chase found a value (`want`) that has not gone out yet
  std::uint8_t route = 0;
  std::uint8_t ctl = 0;
  std::uint8_t last = kLaneNoValue;  // the value this track last sent on this route and controller (the chase skips an equal one)
  std::uint8_t want = 0;
  std::uint8_t flags = 0;
};

struct LaneCursor {
  std::uint32_t seq = 0;           // the store's seq the cursor was taken at: a changed one re-seeks
  std::uint16_t key = kLaneNoKey;  // the lane (pattern, track) the cursor is in
  std::uint16_t next = 0;          // the index of the next event not played
  std::uint16_t expect = 0xFFFF;   // the tick the next call should have (another one is a discontinuity: a loop, a jump, a muted gap)
  std::uint16_t scan = 0;          // the chase looks at events [0, scan) backwards
  std::uint8_t held_n = 0;
  std::uint8_t slot_rr = 0;        // the slot taken next when all are used
  LaneHeld held[kLaneHeldMax];
  LaneSlot slot[kLaneSlots];
};

struct LanePending {
  std::uint8_t route;  // channel << 2 | port
  std::uint8_t ctl;
  std::uint8_t value;
  std::uint8_t track;
};

struct LaneRun {
  LaneCursor cur[kLaneTracks];
  LanePending pend[kLanePending];
  std::uint64_t gate[2] = {0, 0};  // per physical port (DIN, USB): the first sample at which a CC may go out again (the 300 / s limit)
  std::uint32_t seq = 0;           // the store's seq of this block (even when `live`)
  std::uint16_t held_mask = 0;     // tracks with a lane note sounding
  std::uint16_t dirty_mask = 0;    // tracks whose position was set (a start, a jump, a locate, a switch): end held notes, chase
  std::uint16_t chase_mask = 0;    // tracks whose chase is not finished
  std::uint16_t owed_mask = 0;     // tracks that missed microsteps because the store was mid-write (an odd seq): their events are owed, played late when the window closes
  std::uint8_t pend_n = 0;
  std::uint8_t budget = 0;         // lane events this block may still handle
  bool live = false;               // this block reads the lanes (the store is not mid-write, belongs to the project, and has events)

  void reset() noexcept {
    for (std::uint32_t t = 0; t < kLaneTracks; ++t) {
      LaneCursor& c = cur[t];
      c.seq = 0;
      c.key = kLaneNoKey;
      c.next = 0;
      c.expect = 0xFFFF;
      c.scan = 0;
      c.held_n = 0;
      c.slot_rr = 0;
      for (std::uint32_t i = 0; i < kLaneSlots; ++i) {
        c.slot[i] = LaneSlot{};
      }
    }
    gate[0] = gate[1] = 0;
    seq = 0;
    held_mask = 0;
    dirty_mask = 0xFFFFu;
    chase_mask = 0;
    owed_mask = 0;
    pend_n = 0;
    budget = 0;
    live = false;
  }
};

}  // namespace o3os::seq
