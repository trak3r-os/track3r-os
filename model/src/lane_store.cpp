// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/lane_store.h>

#include <atomic>
#include <cstring>

namespace o3os::model {

bool lane_event_valid(const LaneEvent& e) noexcept {
  const std::uint32_t kind = e.kind_channel >> 4;
  if (e.tick >= kLaneTicks || kind >= kLaneEventKinds || e.port > 3 || e.a > 127 || e.b > 127) {
    return false;
  }
  // A note has a length of 1..6144 ticks, every other kind has none.
  return kind == 0 ? (e.len >= 1 && e.len <= kLaneTicks) : e.len == 0;
}

bool lane_events_valid(const LaneEvent* e, std::uint32_t n, std::uint32_t tick_lo, std::uint32_t tick_hi) noexcept {
  std::uint32_t last = tick_lo;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (!lane_event_valid(e[i]) || e[i].tick < last || e[i].tick >= tick_hi) {
      return false;
    }
    last = e[i].tick;
  }
  return true;
}

std::uint32_t LaneStore::lower_bound(const View& lane, std::uint32_t tick) noexcept {
  std::uint32_t lo = 0;
  std::uint32_t hi = lane.count;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2;
    if (lane.events[mid].tick < tick) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void LaneStore::begin_write() noexcept {
  if (depth_++ == 0) {
    seq_ = seq_ + 1;  // odd: the render skips the lanes this block
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
}

void LaneStore::end_write() noexcept {
  if (depth_ != 0 && --depth_ == 0) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    seq_ = seq_ + 1;  // even again: a render that kept a cursor sees the number changed and re-seeks
  }
}

void LaneStore::clear() noexcept {
  // A window a dropped load left open (depth_ above 0, seq_ odd) must not survive a clear or a sync: close it for good first (OS-MIDIREC-P1 S2 fold).
  if (depth_ != 0) {
    depth_ = 1;
    end_write();
  }
  WriteWindow w(*this);
  std::memset(dir_, 0, sizeof dir_);
  lanes_ = 0;
  events_ = 0;
}

void LaneStore::sync(std::uint32_t epoch) noexcept {
  if (epoch_ != epoch) {
    clear();
    epoch_ = epoch;
  }
}

LaneStore::View LaneStore::lane_at(std::uint32_t key) const noexcept {
  if (key >= kKeys || dir_[key].count == 0) {
    return {nullptr, 0};
  }
  return {pool_ + dir_[key].offset, dir_[key].count};
}

LaneEvent* LaneStore::lane_writable(std::uint32_t pattern, std::uint32_t track) noexcept {
  if (!valid_key(pattern, track)) {
    return nullptr;
  }
  const Slot& s = dir_[key_of(pattern, track)];
  return s.count != 0 ? pool_ + s.offset : nullptr;
}

LaneStore::View LaneStore::lane(std::uint32_t pattern, std::uint32_t track) const noexcept {
  return valid_key(pattern, track) ? lane_at(key_of(pattern, track)) : View{nullptr, 0};
}

std::uint32_t LaneStore::next_key(std::uint32_t from) const noexcept {
  for (std::uint32_t k = from; k < kKeys; ++k) {
    if (dir_[k].count != 0) {
      return k;
    }
  }
  return kKeys;
}

std::uint32_t LaneStore::pattern_events(std::uint32_t pattern) const noexcept {
  if (pattern < 1 || pattern > kMaxPatternId) {
    return 0;
  }
  std::uint32_t n = 0;
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    n += dir_[key_of(pattern, t)].count;
  }
  return n;
}

std::uint32_t LaneStore::pool_pos(std::uint32_t key) const noexcept {
  const std::uint32_t k = next_key(key);  // this lane when it has events, else the next one: its start is ours
  return k == kKeys ? events_ : dir_[k].offset;
}

bool LaneStore::can_grow(std::uint32_t pattern, std::uint32_t track, std::uint32_t count) const noexcept {
  if (!valid_key(pattern, track) || count == 0) {
    return false;
  }
  const Slot& s = dir_[key_of(pattern, track)];
  if (s.count + count > kMaxLaneEvents || events_ + count > kMaxLanePoolEvents) {
    return false;
  }
  return s.count != 0 || lanes_ < kMaxLanes;
}

LaneEvent* LaneStore::open_gap(std::uint32_t pattern, std::uint32_t track, std::uint32_t index,
                               std::uint32_t count) noexcept {
  if (!can_grow(pattern, track, count)) {
    return nullptr;
  }
  const std::uint32_t key = key_of(pattern, track);
  Slot& s = dir_[key];
  if (index > s.count) {
    return nullptr;
  }
  WriteWindow w(*this);  // closes at the return: the gap is the caller's to fill (a caller that must fill it inside the window holds its own)
  const std::uint32_t start = pool_pos(key);
  const std::uint32_t at = start + index;
  std::memmove(pool_ + at + count, pool_ + at, (events_ - at) * sizeof(LaneEvent));
  if (s.count == 0) {
    ++lanes_;
  }
  s.offset = static_cast<std::uint16_t>(start);
  s.count = static_cast<std::uint16_t>(s.count + count);
  events_ += count;
  for (std::uint32_t k = key + 1; k < kKeys; ++k) {
    if (dir_[k].count != 0) {
      dir_[k].offset = static_cast<std::uint16_t>(dir_[k].offset + count);
    }
  }
  return pool_ + at;
}

bool LaneStore::erase(std::uint32_t pattern, std::uint32_t track, std::uint32_t index, std::uint32_t count) noexcept {
  if (!valid_key(pattern, track) || count == 0) {
    return false;
  }
  const std::uint32_t key = key_of(pattern, track);
  Slot& s = dir_[key];
  if (index + count > s.count) {
    return false;
  }
  WriteWindow w(*this);
  const std::uint32_t at = s.offset + index;
  std::memmove(pool_ + at, pool_ + at + count, (events_ - at - count) * sizeof(LaneEvent));
  s.count = static_cast<std::uint16_t>(s.count - count);
  if (s.count == 0) {
    s.offset = 0;
    --lanes_;
  }
  events_ -= count;
  for (std::uint32_t k = key + 1; k < kKeys; ++k) {
    if (dir_[k].count != 0) {
      dir_[k].offset = static_cast<std::uint16_t>(dir_[k].offset - count);
    }
  }
  return true;
}

}  // namespace o3os::model
