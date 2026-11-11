// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC-P1 S2 (docs/specs/OS-MIDIREC-P1.md A4..A7): the sequencer plays the MIDI take lanes of the project (model/lane_store.h) beside the steps.
//
// One cursor a track walks its lane (pattern, track) at the track's own position: tick = pos x 48 + (microstep - 1), so a lane follows the track's play mode, its length, loops and jumps like the step column does.
// Everything here runs in the render (Sequencer::step): no allocation, no wait, bounded loops (16 tracks, 8 held notes, 16 waiting CCs, 16 events and 128 chase looks a block).
//
// The store's writer is the main loop (a seqlock, model/lane_store.h): `seq()` is constant inside one step() because the render preempts the main loop on the one core. An odd seq means a mutation is in progress:
// the block reads no lane; its microsteps owe their events, which the cursor plays late when the window has closed (a track that missed microsteps is `owed`, up to kLaneCatchUp ticks back); a note that is sounding
// ends on time, because its length is counted here (LaneHeld::left), not read from the store. A cursor re-seeks when the seq has changed (an edit, a take committed).

#include <o3os/core/audio_clock.h>
#include <o3os/seq/sequencer.h>

namespace o3os::seq {

using audio::EventKind;
using model::LaneEvent;
using model::LaneStore;

namespace {

// A7: the gap between two control changes on one port, in samples (300 a second).
constexpr std::uint64_t kCcGap = static_cast<std::uint64_t>(core::kRefRateHz / static_cast<float>(kLaneCcPerSecond));

constexpr std::uint8_t route_of(const LaneEvent& e) noexcept { return static_cast<std::uint8_t>(((e.kind_channel & 0x0Fu) << 2) | (e.port & 3u)); }

// The physical ports a route label addresses, as gate bits: 0 (the device's MIDI OUT) counts with DIN, 1 DIN, 2 USB, 3 both.
constexpr std::uint32_t gate_bits(std::uint8_t ports) noexcept { return ports == 2u ? 2u : (ports == 3u ? 3u : 1u); }

LaneSlot* find_slot(LaneCursor& c, std::uint8_t route, std::uint8_t ctl) noexcept {
  for (LaneSlot& s : c.slot) {
    if ((s.flags & LaneSlot::kUsed) != 0u && s.route == route && s.ctl == ctl) {
      return &s;
    }
  }
  return nullptr;
}

// The record of a controller: the existing one, else a free slot, else (recycle: the playback's newest value always has a place) the next in turn; nullptr (the chase: it has no place for a ninth controller).
LaneSlot* take_slot(LaneCursor& c, std::uint8_t route, std::uint8_t ctl, bool recycle) noexcept {
  if (LaneSlot* s = find_slot(c, route, ctl)) {
    return s;
  }
  LaneSlot* fresh = nullptr;
  for (LaneSlot& s : c.slot) {
    if ((s.flags & LaneSlot::kUsed) == 0u) {
      fresh = &s;
      break;
    }
  }
  if (fresh == nullptr && !recycle) {
    // The chase: a slot a seek left behind (allocated, but neither seen nor waiting in this chase) belongs to a controller of an earlier pattern or position, so it is taken over;
    // only the controllers this chase has already met keep their place (OS-MIDIREC-P1 S2 fold: eight CCs of pattern A must not block pattern B's chase).
    for (LaneSlot& s : c.slot) {
      if ((s.flags & (LaneSlot::kSeen | LaneSlot::kPend)) == 0u) {
        fresh = &s;
        break;
      }
    }
  }
  if (fresh == nullptr) {
    if (!recycle) {
      return nullptr;
    }
    fresh = &c.slot[c.slot_rr];
    c.slot_rr = static_cast<std::uint8_t>((c.slot_rr + 1u) % kLaneSlots);
  }
  *fresh = LaneSlot{};
  fresh->route = route;
  fresh->ctl = ctl;
  fresh->flags = LaneSlot::kUsed;
  return fresh;
}

}  // namespace

// ---- the run -----------------------------------------------------------------------------------------------------------------

void Sequencer::lane_begin_run() noexcept {
  if (lane_ != nullptr) {
    lane_->reset();
  }
}

// A stop: every lane note ends, what waits and what chases is dropped (a note-off that found no room in the queue is as lost as the steps' own: counted in queue_overflow).
void Sequencer::lane_end_run(std::uint64_t at) noexcept {
  if (lane_ == nullptr) {
    return;
  }
  LaneRun& r = *lane_;
  for (std::uint32_t t = 0; t < kLaneTracks; ++t) {
    if ((r.held_mask & (1u << t)) != 0u) {
      lane_end_held(t, at);
    }
    r.cur[t].held_n = 0;
  }
  r.held_mask = 0;
  r.pend_n = 0;
  r.chase_mask = 0;
  r.owed_mask = 0;
}

// Once a block, before its microsteps: the budget, the CCs that waited for their port, the store's state and the chase.
void Sequencer::lane_block() noexcept {
  if (lane_ == nullptr) {
    return;
  }
  LaneRun& r = *lane_;
  r.budget = kLaneBlockEvents;
  r.live = false;
  lane_flush(now_);
  if (lanes_ == nullptr) {
    return;
  }
  const std::uint32_t s = lanes_->seq();
  if ((s & 1u) != 0u) {
    ++stats_.lane_odd;
    r.owed_mask = 0xFFFFu;  // the tracks that play now owe their events; a track with no cursor in a lane owes none (lane_track)
    return;
  }
  if (!lanes_->in_sync(project_.load_epoch()) || lanes_->total_events() == 0u) {
    r.owed_mask = 0;
    return;
  }
  r.seq = s;
  r.live = true;
  if (r.chase_mask != 0u) {
    lane_chase();
  }
}

// ---- one microstep --------------------------------------------------------------------------------------------------------------

void Sequencer::lane_microstep(std::uint32_t u, std::uint64_t at, std::uint32_t active) noexcept {
  if (lane_ == nullptr) {
    return;
  }
  LaneRun& r = *lane_;
  const std::uint16_t cut = static_cast<std::uint16_t>(r.held_mask & r.dirty_mask);
  for (std::uint32_t t = 0; cut != 0u && t < kLaneTracks; ++t) {
    if ((cut & (1u << t)) != 0u) {
      lane_end_held(t, at);  // A5: a jump, a locate or a pattern switch ends the lane's notes
    }
  }
  if (r.held_mask != 0u) {
    lane_count_down(at);  // from the lengths counted here, whether the store can be read or not, and for muted and inactive tracks
  }
  for (std::uint32_t t = 0; t < active; ++t) {
    const std::uint16_t bit = static_cast<std::uint16_t>(1u << t);
    if (midi_silenced(t)) {
      if ((r.held_mask & bit) != 0u) {
        lane_end_held(t, at);
      }
      r.chase_mask = static_cast<std::uint16_t>(r.chase_mask & ~bit);
      r.dirty_mask = static_cast<std::uint16_t>(r.dirty_mask & ~bit);
      r.cur[t].expect = 0xFFFFu;  // the unmuted track re-seeks at its position: what passed while muted is not played, and no chase
      continue;
    }
    if (r.live) {
      lane_track(t, u, at);
    }
  }
}

void Sequencer::lane_track(std::uint32_t t, std::uint32_t u, std::uint64_t at) noexcept {
  LaneRun& r = *lane_;
  LaneCursor& c = r.cur[t];
  const TrackState& k = tr_[t];
  const std::uint16_t bit = static_cast<std::uint16_t>(1u << t);
  const std::uint16_t key = LaneStore::valid_key(k.pattern_id, t) ? static_cast<std::uint16_t>(LaneStore::key_of(k.pattern_id, t)) : kLaneNoKey;
  const std::uint32_t tick = static_cast<std::uint32_t>(k.pos) * kMicrostepsPerStep + (u - 1u);
  bool seek = false;
  bool chase = false;
  std::uint32_t from = tick;
  if (key != c.key) {  // another lane (a pattern change): its notes end, its controllers are chased
    lane_end_held(t, at);
    c.key = key;
    seek = chase = true;
  }
  if ((r.dirty_mask & bit) != 0u) {
    r.dirty_mask = static_cast<std::uint16_t>(r.dirty_mask & ~bit);
    seek = chase = true;
  }
  if (c.seq != r.seq) {  // the store changed since the cursor was taken: an edit, a commit, a window that met an odd block
    // Owed events of a window (the catch-up, A17): the microsteps from `expect` on were skipped, not jumped over.
    if (!seek && (r.owed_mask & bit) != 0u && c.expect != 0xFFFFu && c.expect < tick && tick - c.expect <= kLaneCatchUp) {
      from = c.expect;
    }
    seek = true;
    r.chase_mask = static_cast<std::uint16_t>(r.chase_mask & ~bit);  // its scan position belonged to the old data
  }
  r.owed_mask = static_cast<std::uint16_t>(r.owed_mask & ~bit);
  if (c.expect != tick) {
    seek = true;  // a loop, a jump of the step, a play mode's order: the cursor goes to where the track is
  }
  const LaneStore::View v = key != kLaneNoKey ? lanes_->lane_at(key) : LaneStore::View{nullptr, 0};
  c.seq = r.seq;
  if (v.count == 0u) {
    c.next = 0;
    c.expect = 0xFFFFu;
    r.chase_mask = static_cast<std::uint16_t>(r.chase_mask & ~bit);
    return;
  }
  if (seek) {
    c.next = static_cast<std::uint16_t>(LaneStore::lower_bound(v, from));
    if (chase) {
      c.scan = static_cast<std::uint16_t>(LaneStore::lower_bound(v, tick));
      for (LaneSlot& s : c.slot) {
        s.flags = static_cast<std::uint8_t>(s.flags & LaneSlot::kUsed);  // what was sent before the jump does not make a value "newer": the chase starts afresh
      }
      if (c.scan != 0u) {
        r.chase_mask = static_cast<std::uint16_t>(r.chase_mask | bit);
      }
    }
  } else if (c.next > v.count) {
    c.next = static_cast<std::uint16_t>(v.count);
  }
  c.expect = static_cast<std::uint16_t>(tick + 1u);
  while (c.next < v.count && v.events[c.next].tick <= tick) {
    if (r.budget == 0u) {
      ++stats_.lane_deferred;  // A7: it stays due; the next microstep takes it first
      break;
    }
    --r.budget;
    lane_event(t, v.events[c.next], at);
    ++c.next;
  }
}

// ---- events -----------------------------------------------------------------------------------------------------------------------

void Sequencer::lane_event(std::uint32_t t, const LaneEvent& e, std::uint64_t at) noexcept {
  const std::uint8_t track = static_cast<std::uint8_t>(t);
  const std::uint8_t ch = static_cast<std::uint8_t>(e.kind_channel & 0x0Fu);
  const std::uint8_t ports = static_cast<std::uint8_t>(e.port & audio::kRoutePorts);
  const std::uint8_t a = static_cast<std::uint8_t>(e.a & 0x7Fu);
  const std::uint8_t b = static_cast<std::uint8_t>(e.b & 0x7Fu);
  ++stats_.lane_events;
  switch (e.kind_channel >> 4) {
    case 0:
      lane_note_on(t, e, at);
      break;
    case 1:
      lane_cc(t, route_of(e), a, b, at);
      break;
    case 2:  // a program change is not noted in chan_prog_ (as the steps' PROG: the instrument's own PROGRAM is put back only when it changes)
      emit(at, EventKind::MidiProgram, track, a, 0, ch, ports, 0);
      break;
    case 3:
      emit(at, EventKind::MidiChanAt, track, a, 0, ch, ports, 0);
      break;
    case 4:
      emit(at, EventKind::MidiPolyAt, track, a, b, ch, ports, 0);
      break;
    case 5: {
      const std::uint64_t route_bit = std::uint64_t{1} << (4u * ch + ports);
      if (emit(at, EventKind::MidiPitchBend, track, a, b, ch, ports, 0)) {
        // the stop centres every route a bend went to (centre_bends); a bend back to the centre needs no more
        bend_routes_ = (a == (kBendCentre & 0x7Fu) && b == (kBendCentre >> 7)) ? (bend_routes_ & ~route_bit) : (bend_routes_ | route_bit);
      }
      break;
    }
    default:
      break;
  }
}

// A5: a note sounds for its length counted in microsteps; at most 8 a track (a ninth ends the oldest), and a repeat of a sounding pitch on the same route ends the first.
void Sequencer::lane_note_on(std::uint32_t t, const LaneEvent& e, std::uint64_t at) noexcept {
  LaneCursor& c = lane_->cur[t];
  const std::uint8_t route = route_of(e);
  const std::uint8_t note = static_cast<std::uint8_t>(e.a & 0x7Fu);
  for (std::uint32_t i = 0; i < c.held_n; ++i) {
    if (c.held[i].note == note && c.held[i].route == route) {
      if (!lane_off(t, i, at)) {
        return;  // no room for the note-off: the note is not started over the sounding one
      }
      break;
    }
  }
  if (c.held_n >= kLaneHeldMax && !lane_off(t, 0, at)) {
    return;
  }
  const std::uint8_t vel = e.b == 0u ? std::uint8_t{1} : static_cast<std::uint8_t>(e.b & 0x7Fu);
  if (!emit(at, EventKind::MidiNoteOn, static_cast<std::uint8_t>(t), note, vel, static_cast<std::uint8_t>(e.kind_channel & 0x0Fu), static_cast<std::uint8_t>(e.port & audio::kRoutePorts), 0)) {
    return;
  }
  c.held[c.held_n++] = LaneHeld{note, route, e.len == 0u ? std::uint16_t{1} : e.len};
  lane_->held_mask = static_cast<std::uint16_t>(lane_->held_mask | (1u << t));
}

// The note-off of held note `index` of track t. false: no room in the queue, the note stays held (its countdown retries it at the next microstep).
bool Sequencer::lane_off(std::uint32_t t, std::uint32_t index, std::uint64_t at) noexcept {
  LaneCursor& c = lane_->cur[t];
  const LaneHeld h = c.held[index];
  if (!emit(at, EventKind::MidiNoteOff, static_cast<std::uint8_t>(t), h.note, 0, static_cast<std::uint8_t>(h.route >> 2), static_cast<std::uint8_t>(h.route & 3u), 0)) {
    c.held[index].left = 0;
    return false;
  }
  for (std::uint32_t i = index + 1u; i < c.held_n; ++i) {
    c.held[i - 1u] = c.held[i];
  }
  --c.held_n;
  if (c.held_n == 0u) {
    lane_->held_mask = static_cast<std::uint16_t>(lane_->held_mask & ~(1u << t));
  }
  return true;
}

void Sequencer::lane_end_held(std::uint32_t t, std::uint64_t at) noexcept {
  LaneCursor& c = lane_->cur[t];
  for (std::uint32_t i = 0; i < c.held_n;) {
    if (!lane_off(t, i, at)) {
      ++i;
    }
  }
}

void Sequencer::lane_count_down(std::uint64_t at) noexcept {
  const std::uint16_t mask = lane_->held_mask;
  for (std::uint32_t t = 0; t < kLaneTracks; ++t) {
    if ((mask & (1u << t)) == 0u) {
      continue;
    }
    LaneCursor& c = lane_->cur[t];
    for (std::uint32_t i = 0; i < c.held_n;) {
      LaneHeld& h = c.held[i];
      if (h.left > 0u) {
        --h.left;
      }
      if (h.left == 0u && lane_off(t, i, at)) {
        continue;  // the next note slid into `i`
      }
      ++i;
    }
  }
}

// ---- control changes: the 300 / s limit, the latest value wins (A7) ------------------------------------------------------------------------

// Out at `at` when every port the route addresses is past its gap; false: it waits (or the queue was full).
bool Sequencer::lane_cc_send(std::uint8_t route, std::uint8_t ctl, std::uint8_t value, std::uint8_t track, std::uint64_t at) noexcept {
  const std::uint8_t ports = static_cast<std::uint8_t>(route & 3u);
  const std::uint32_t bits = gate_bits(ports);
  for (std::uint32_t i = 0; i < 2u; ++i) {
    if ((bits & (1u << i)) != 0u && at < lane_->gate[i]) {
      return false;
    }
  }
  if (!emit(at, EventKind::MidiCc, track, static_cast<std::uint8_t>(audio::kMidiCcExplicit | ctl), value, static_cast<std::uint8_t>(route >> 2), ports, 0)) {
    return false;
  }
  for (std::uint32_t i = 0; i < 2u; ++i) {
    if ((bits & (1u << i)) != 0u) {
      lane_->gate[i] = at + kCcGap;
    }
  }
  return true;
}

void Sequencer::lane_cc(std::uint32_t t, std::uint8_t route, std::uint8_t ctl, std::uint8_t value, std::uint64_t at) noexcept {
  LaneRun& r = *lane_;
  LaneSlot* s = take_slot(r.cur[t], route, ctl, true);
  s->flags = static_cast<std::uint8_t>((s->flags | LaneSlot::kSeen) & ~LaneSlot::kPend);  // the playback's value is the newest the chase may know
  for (std::uint32_t i = 0; i < r.pend_n; ++i) {
    if (r.pend[i].route == route && r.pend[i].ctl == ctl) {
      r.pend[i].value = value;  // the controller is already waiting: only its latest value goes out
      r.pend[i].track = static_cast<std::uint8_t>(t);
      ++stats_.lane_cc_merged;
      return;
    }
  }
  if (lane_cc_send(route, ctl, value, static_cast<std::uint8_t>(t), at)) {
    s->last = value;
    return;
  }
  if (r.pend_n == kLanePending) {
    for (std::uint32_t i = 1; i < kLanePending; ++i) {
      r.pend[i - 1u] = r.pend[i];
    }
    --r.pend_n;
    ++stats_.lane_cc_dropped;
  }
  r.pend[r.pend_n++] = LanePending{route, ctl, value, static_cast<std::uint8_t>(t)};
}

// The CCs that waited: out when their port's gap has passed (at most one a port a block).
void Sequencer::lane_flush(std::uint64_t at) noexcept {
  LaneRun& r = *lane_;
  for (std::uint32_t i = 0; i < r.pend_n;) {
    const LanePending p = r.pend[i];
    if (!lane_cc_send(p.route, p.ctl, p.value, p.track, at)) {
      ++i;
      continue;
    }
    if (LaneSlot* s = find_slot(r.cur[p.track & 15u], p.route, p.ctl)) {
      s->last = p.value;
    }
    for (std::uint32_t j = i + 1u; j < r.pend_n; ++j) {
      r.pend[j - 1u] = r.pend[j];
    }
    --r.pend_n;
    if (r.budget != 0u) {
      --r.budget;
    }
  }
}

// ---- the chase (A6) -------------------------------------------------------------------------------------------------------------------------

// After a jump, a locate, a start or a pattern change the track's controllers are put where the lane says they stand at the new position: the latest control change before it, per route and controller, skipped when it
// equals the value this track sent last. Control changes only (no note, no bend, no program). Backwards from the position, at most kLaneChaseScan events a block; a controller already played since (kSeen) is left alone; at
// most kLaneSlots controllers a track.
void Sequencer::lane_chase() noexcept {
  LaneRun& r = *lane_;
  std::uint32_t looks = kLaneChaseScan;
  for (std::uint32_t t = 0; t < kLaneTracks; ++t) {
    const std::uint16_t bit = static_cast<std::uint16_t>(1u << t);
    if ((r.chase_mask & bit) == 0u) {
      continue;
    }
    LaneCursor& c = r.cur[t];
    const LaneStore::View v = c.key != kLaneNoKey ? lanes_->lane_at(c.key) : LaneStore::View{nullptr, 0};
    if (v.count == 0u || c.seq != r.seq || c.scan > v.count || midi_silenced(t)) {
      r.chase_mask = static_cast<std::uint16_t>(r.chase_mask & ~bit);
      continue;
    }
    bool waiting = false;
    for (LaneSlot& s : c.slot) {  // values found earlier that the port's gap held back
      if ((s.flags & LaneSlot::kPend) == 0u) {
        continue;
      }
      if (r.budget != 0u && lane_cc_send(s.route, s.ctl, s.want, static_cast<std::uint8_t>(t), now_)) {
        s.flags = static_cast<std::uint8_t>(s.flags & ~LaneSlot::kPend);
        s.last = s.want;
        --r.budget;
        ++stats_.lane_chase_sent;
      } else {
        waiting = true;
      }
    }
    while (c.scan > 0u && looks > 0u) {
      --looks;
      const LaneEvent& e = v.events[--c.scan];
      if ((e.kind_channel >> 4) != 1u) {
        continue;
      }
      const std::uint8_t route = route_of(e);
      LaneSlot* s = take_slot(c, route, e.a, false);
      if (s == nullptr || (s->flags & LaneSlot::kSeen) != 0u) {
        continue;
      }
      s->flags = static_cast<std::uint8_t>(s->flags | LaneSlot::kSeen);
      if (e.b == s->last) {
        continue;
      }
      if (r.budget != 0u && lane_cc_send(route, e.a, e.b, static_cast<std::uint8_t>(t), now_)) {
        s->last = e.b;
        --r.budget;
        ++stats_.lane_chase_sent;
      } else {
        s->want = e.b;
        s->flags = static_cast<std::uint8_t>(s->flags | LaneSlot::kPend);
        waiting = true;
      }
    }
    if (c.scan == 0u && !waiting) {
      r.chase_mask = static_cast<std::uint16_t>(r.chase_mask & ~bit);
    }
  }
}

}  // namespace o3os::seq
