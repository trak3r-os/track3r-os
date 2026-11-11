// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/lane_take.h>

#include <cstring>

namespace o3os::model {

namespace {
constexpr std::uint8_t kKindNote = 0;
constexpr std::uint8_t kKindCc = 1;
constexpr std::uint8_t kTombstone = 0xFF;  // kind_channel of an event that the co-linear pass dropped (kinds above 5 are invalid)
}  // namespace

void LaneTake::reset() noexcept {
  std::memset(held_, 0, sizeof held_);
  std::memset(cc_, 0, sizeof cc_);
  intervals_[0] = {0, 0};
  intervals_[1] = {0, 0};
  count_ = 0;
  pend_n_ = 0;
  start_ = 0;
  length_ = kLaneTicks;
  last_tick_ = 0;
  intervals_n_ = 0;
  pattern_ = 0;
  track_ = 0;
  channel_ = 0;
  port_ = 0;
  open_ = false;
  finished_ = false;
  full_ = false;
  wrapped_ = false;
}

void LaneTake::begin(std::uint8_t pattern, std::uint8_t track, std::uint8_t channel, std::uint8_t port, std::uint32_t start_tick,
                     std::uint32_t length_ticks) noexcept {
  reset();
  pattern_ = pattern;
  track_ = track;
  channel_ = static_cast<std::uint8_t>(channel & 15u);
  port_ = static_cast<std::uint8_t>(port & 3u);
  length_ = length_ticks == 0 || length_ticks > kLaneTicks ? kLaneTicks : length_ticks;
  start_ = start_tick < length_ ? start_tick : length_ - 1;
  last_tick_ = start_;
  open_ = true;
}

bool LaneTake::append(const LaneEvent& e) noexcept {
  if (!room()) {
    full_ = true;
    return false;
  }
  events_[count_++] = e;
  return true;
}

void LaneTake::note_wrap(std::uint32_t tick) noexcept {
  if (tick < last_tick_) {
    wrapped_ = true;  // the stamps only go back when the playhead passed the end of the track
  }
  last_tick_ = tick;
}

void LaneTake::close_note(std::uint8_t note, std::uint32_t tick) noexcept {
  const std::uint16_t h = held_[note];
  if (h == 0) {
    return;
  }
  held_[note] = 0;
  LaneEvent& e = events_[h - 1u];
  std::uint32_t len = tick >= e.tick ? tick - e.tick : tick + length_ - e.tick;
  len = len < 1 ? 1 : (len > kLaneTicks ? kLaneTicks : len);
  e.len = static_cast<std::uint16_t>(len);
}

bool LaneTake::note_on(std::uint32_t tick, std::uint8_t note, std::uint8_t velocity) noexcept {
  if (!open_ || note > 127) {
    return false;
  }
  if (velocity == 0) {
    note_off(tick, note);
    return true;
  }
  if (tick >= length_) {
    tick = length_ - 1;
  }
  note_wrap(tick);
  close_note(note, tick);  // the same note again while held: the first ends here
  if (!room()) {
    full_ = true;
    return false;
  }
  LaneEvent e;
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = 1;
  e.kind_channel = static_cast<std::uint8_t>((kKindNote << 4) | channel_);
  e.port = port_;
  e.a = note;
  e.b = velocity > 127 ? 127 : velocity;
  held_[note] = static_cast<std::uint16_t>(count_ + 1u);
  events_[count_++] = e;
  return true;
}

bool LaneTake::note_len(std::uint32_t tick, std::uint8_t note, std::uint8_t velocity, std::uint32_t len_ticks) noexcept {
  if (!open_ || note > 127 || velocity == 0) {
    return false;
  }
  if (tick >= length_) {
    tick = length_ - 1;
  }
  note_wrap(tick);
  LaneEvent e;
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = static_cast<std::uint16_t>(len_ticks < 1 ? 1 : (len_ticks > kLaneTicks ? kLaneTicks : len_ticks));
  e.kind_channel = static_cast<std::uint8_t>((kKindNote << 4) | channel_);
  e.port = port_;
  e.a = note;
  e.b = velocity > 127 ? 127 : velocity;
  return append(e);
}

void LaneTake::note_off(std::uint32_t tick, std::uint8_t note) noexcept {
  if (!open_ || note > 127) {
    return;
  }
  if (tick >= length_) {
    tick = length_ - 1;
  }
  note_wrap(tick);
  close_note(note, tick);
}

void LaneTake::all_notes_off(std::uint32_t tick) noexcept {
  if (!open_) {
    return;
  }
  if (tick >= length_) {
    tick = length_ - 1;
  }
  note_wrap(tick);
  for (std::uint32_t n = 0; n < 128; ++n) {
    close_note(static_cast<std::uint8_t>(n), tick);
  }
}

void LaneTake::flush_pending(std::uint8_t controller) noexcept {
  CcState& s = cc_[controller];
  if (s.has_pend == 0) {
    return;
  }
  s.has_pend = 0;
  --pend_n_;  // its slot was kept (room()): the write below cannot fail, the guard is for a corrupt state only
  if (count_ >= kCapacity) {
    return;
  }
  LaneEvent e;
  e.tick = s.pend_tick;
  e.len = 0;
  e.kind_channel = static_cast<std::uint8_t>((kKindCc << 4) | channel_);
  e.port = port_;
  e.a = controller;
  e.b = s.pend_value;
  events_[count_++] = e;
  s.kept_tick = s.pend_tick;
  s.kept_value = s.pend_value;
}

// A9, streaming: the first point of a controller is kept; a point equal to the last kept one is dropped (deadband 1); one that is at least
// kThinTicks after the last kept one is kept; a nearer one waits as the pending point (the newest wins), and the pending point is kept
// when the move ends (no point of the controller within kThinTicks after it, or the take ends).
bool LaneTake::cc(std::uint32_t tick, std::uint8_t controller, std::uint8_t value) noexcept {
  if (!open_ || controller > 127 || value > 127) {
    return false;
  }
  if (tick >= length_) {
    tick = length_ - 1;
  }
  note_wrap(tick);
  CcState& s = cc_[controller];
  const auto dist = [this](std::uint32_t from, std::uint32_t to) { return to >= from ? to - from : to + length_ - from; };
  // The controller's replace range (A10): every message that is accepted counts, kept or not (a value held is a value held).
  const auto seen = [&s, tick]() {
    if (tick < s.last_tick) {
      s.wrapped = 1;
    }
    s.last_tick = static_cast<std::uint16_t>(tick);
  };
  if (s.has_pend != 0 && dist(s.pend_tick, tick) >= kThinTicks) {
    flush_pending(controller);  // nothing followed it within the window: it was the last of its move
  }
  if (s.has_kept == 0) {
    LaneEvent e;
    e.tick = static_cast<std::uint16_t>(tick);
    e.len = 0;
    e.kind_channel = static_cast<std::uint8_t>((kKindCc << 4) | channel_);
    e.port = port_;
    e.a = controller;
    e.b = value;
    if (!append(e)) {
      return false;
    }
    s.has_kept = 1;
    s.kept_tick = e.tick;
    s.kept_value = value;
    s.first_tick = e.tick;
    s.last_tick = e.tick;
    s.wrapped = 0;
    return true;
  }
  const std::uint32_t diff = value > s.kept_value ? value - s.kept_value : s.kept_value - value;
  if (diff < kDeadband) {
    if (s.has_pend != 0) {  // back at the kept value: no point is needed
      s.has_pend = 0;
      --pend_n_;
    }
    seen();
    return true;
  }
  if (dist(s.kept_tick, tick) >= kThinTicks) {
    const std::uint32_t own = s.has_pend;  // this point replaces the pending one: its slot is free for it
    if (count_ + pend_n_ - own >= kCapacity) {
      full_ = true;
      return false;
    }
    s.has_pend = 0;
    pend_n_ -= own;
    LaneEvent e;
    e.tick = static_cast<std::uint16_t>(tick);
    e.len = 0;
    e.kind_channel = static_cast<std::uint8_t>((kKindCc << 4) | channel_);
    e.port = port_;
    e.a = controller;
    e.b = value;
    events_[count_++] = e;  // room was checked above
    s.kept_tick = e.tick;
    s.kept_value = value;
    seen();
    return true;
  }
  if (s.has_pend == 0) {
    if (!room()) {  // no slot to keep for it: the take is full here, this message starts the next
      full_ = true;
      return false;
    }
    s.has_pend = 1;
    ++pend_n_;
  }
  s.pend_tick = static_cast<std::uint16_t>(tick);
  s.pend_value = value;
  seen();
  return true;
}

bool LaneTake::replaces(std::uint8_t controller, std::uint32_t tick) const noexcept {
  if (controller >= 128 || cc_[controller].has_kept == 0) {
    return false;
  }
  const CcState& s = cc_[controller];
  if (s.wrapped == 0) {
    return tick >= s.first_tick && tick <= s.last_tick;
  }
  return s.last_tick >= s.first_tick || tick >= s.first_tick || tick <= s.last_tick;  // passed the end of the track: the end and the start of it, or all of it
}

// The co-linear pass (A9) on the sorted events: an interior point of a controller that lies on the line (rounded to the nearest value)
// between its kept neighbours goes.
void LaneTake::drop_colinear() noexcept {
  std::uint16_t prev[128] = {};  // index + 1 of the last kept point of the controller
  std::uint16_t cand[128] = {};  // index + 1 of the point after it that may be interior
  bool any = false;
  for (std::uint32_t i = 0; i < count_; ++i) {
    const LaneEvent& e = events_[i];
    if ((e.kind_channel >> 4) != kKindCc) {
      continue;
    }
    const std::uint32_t c = e.a;
    if (prev[c] == 0) {
      prev[c] = static_cast<std::uint16_t>(i + 1u);
      continue;
    }
    if (cand[c] == 0) {
      cand[c] = static_cast<std::uint16_t>(i + 1u);
      continue;
    }
    const LaneEvent& p = events_[prev[c] - 1u];
    LaneEvent& q = events_[cand[c] - 1u];
    const std::int32_t dt = static_cast<std::int32_t>(e.tick) - p.tick;
    bool on_line = false;
    if (dt > 0) {
      const std::int32_t num = (static_cast<std::int32_t>(e.b) - p.b) * (static_cast<std::int32_t>(q.tick) - p.tick);
      const std::int32_t r = (2 * num + (num >= 0 ? dt : -dt)) / (2 * dt);
      on_line = p.b + r == q.b;
    }
    if (on_line) {
      q.kind_channel = kTombstone;
      any = true;
    } else {
      prev[c] = cand[c];
    }
    cand[c] = static_cast<std::uint16_t>(i + 1u);
  }
  if (!any) {
    return;
  }
  std::uint32_t w = 0;
  for (std::uint32_t i = 0; i < count_; ++i) {
    if (events_[i].kind_channel != kTombstone) {
      events_[w++] = events_[i];
    }
  }
  count_ = w;
}

// Stable, in place, no allocation, O(n log^2 n) worst case: insertion-sorted blocks of 20, then SymMerge (the merge of the Go library's
// sort.Stable) of neighbouring runs, skipping a pair that is in order already. The events arrive almost sorted (the playhead's passes over
// the loop are runs, a pending CC point is flushed late), so the common take is one pass of compares. The recursion is log2(n) deep.
void LaneTake::rotate(std::uint32_t a, std::uint32_t m, std::uint32_t b) noexcept {
  const auto reverse = [this](std::uint32_t i, std::uint32_t j) {  // [i, j)
    while (i + 1 < j) {
      --j;
      const LaneEvent t = events_[i];
      events_[i] = events_[j];
      events_[j] = t;
      ++i;
    }
  };
  reverse(a, m);
  reverse(m, b);
  reverse(a, b);
}

void LaneTake::sym_merge(std::uint32_t a, std::uint32_t m, std::uint32_t b) noexcept {
  if (m - a == 1) {  // one element on the left: it goes in front of the first right-hand event that is not lower
    std::uint32_t i = m;
    std::uint32_t j = b;
    while (i < j) {
      const std::uint32_t h = (i + j) / 2;
      if (events_[h].tick < events_[a].tick) {
        i = h + 1;
      } else {
        j = h;
      }
    }
    const LaneEvent t = events_[a];
    for (std::uint32_t k = a; k + 1 < i; ++k) {
      events_[k] = events_[k + 1];
    }
    events_[i - 1] = t;
    return;
  }
  if (b - m == 1) {
    std::uint32_t i = a;
    std::uint32_t j = m;
    while (i < j) {
      const std::uint32_t h = (i + j) / 2;
      if (events_[m].tick >= events_[h].tick) {
        i = h + 1;
      } else {
        j = h;
      }
    }
    const LaneEvent t = events_[m];
    for (std::uint32_t k = m; k > i; --k) {
      events_[k] = events_[k - 1];
    }
    events_[i] = t;
    return;
  }
  const std::uint32_t mid = (a + b) / 2;
  const std::uint32_t n = mid + m;
  std::uint32_t start;
  std::uint32_t r;
  if (m > mid) {
    start = n - b;
    r = mid;
  } else {
    start = a;
    r = m;
  }
  const std::uint32_t p = n - 1;
  while (start < r) {
    const std::uint32_t c = (start + r) / 2;
    if (events_[p - c].tick >= events_[c].tick) {
      start = c + 1;
    } else {
      r = c;
    }
  }
  const std::uint32_t end = n - start;
  if (start < m && m < end) {
    rotate(start, m, end);
  }
  if (a < start && start < mid) {
    sym_merge(a, start, mid);
  }
  if (mid < end && end < b) {
    sym_merge(mid, end, b);
  }
}

void LaneTake::sort_events() noexcept {
  constexpr std::uint32_t kBlock = 20;
  for (std::uint32_t lo = 0; lo < count_; lo += kBlock) {  // insertion sort of each block
    const std::uint32_t hi = lo + kBlock < count_ ? lo + kBlock : count_;
    for (std::uint32_t i = lo + 1; i < hi; ++i) {
      if (events_[i - 1].tick <= events_[i].tick) {
        continue;
      }
      const LaneEvent tmp = events_[i];
      std::uint32_t j = i;
      while (j > lo && events_[j - 1].tick > tmp.tick) {
        events_[j] = events_[j - 1];
        --j;
      }
      events_[j] = tmp;
    }
  }
  for (std::uint32_t w = kBlock; w < count_; w *= 2) {  // merge neighbouring runs of w
    for (std::uint32_t a = 0; a + w < count_; a += 2 * w) {
      const std::uint32_t b = a + 2 * w < count_ ? a + 2 * w : count_;
      if (events_[a + w - 1].tick > events_[a + w].tick) {
        sym_merge(a, a + w, b);
      }
    }
  }
}

void LaneTake::finish(std::uint32_t end_tick) noexcept {
  if (!open_) {
    return;
  }
  if (end_tick >= length_) {
    end_tick = length_ - 1;
  }
  if (end_tick < last_tick_) {
    wrapped_ = true;
  }
  if (wrapped_ && end_tick < start_) {
    // The low run is [0, end_tick]: an event above it (a message in the block that stopped, after the playhead's last read) would fall outside both intervals and lose the take. The end moves up to it (OS-MIDIREC-P1 S5 fold).
    for (std::uint32_t i = 0; i < count_; ++i) {
      if (events_[i].tick < start_ && events_[i].tick > end_tick) {
        end_tick = events_[i].tick;
      }
    }
    for (std::uint32_t c = 0; c < 128; ++c) {
      if (cc_[c].has_pend != 0 && cc_[c].pend_tick < start_ && cc_[c].pend_tick > end_tick) {
        end_tick = cc_[c].pend_tick;
      }
    }
  }
  for (std::uint32_t n = 0; n < 128; ++n) {
    close_note(static_cast<std::uint8_t>(n), end_tick);
  }
  for (std::uint32_t c = 0; c < 128; ++c) {
    flush_pending(static_cast<std::uint8_t>(c));
  }
  sort_events();
  drop_colinear();
  if (wrapped_) {
    if (end_tick < start_) {
      intervals_[0] = {static_cast<std::uint16_t>(start_), static_cast<std::uint16_t>(length_)};  // the higher run first
      intervals_[1] = {0, static_cast<std::uint16_t>(end_tick + 1u)};
      intervals_n_ = 2;
    } else {
      intervals_[0] = {0, static_cast<std::uint16_t>(length_)};  // a full loop or more
      intervals_n_ = 1;
    }
  } else {
    std::uint32_t last = end_tick;
    if (count_ != 0 && events_[count_ - 1].tick > last) {
      last = events_[count_ - 1].tick;
    }
    intervals_[0] = {static_cast<std::uint16_t>(start_), static_cast<std::uint16_t>(last + 1u)};
    intervals_n_ = 1;
  }
  open_ = false;
  finished_ = true;
}

}  // namespace o3os::model
