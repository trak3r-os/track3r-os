// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/model/editor.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "support/need.h"

// OS-MIDIREC-P1 S1: the lane store and the Editor's lane calls (undo, A16 delete / clear, A17 track length).

namespace {

using o3os::model::Editor;
using o3os::model::LaneEvent;
using o3os::model::LaneStore;
using o3os::model::Status;
using o3os::model::UndoJournal;
using o3os::model::UndoResult;

o3os::model::ModelArena g_arena;
alignas(8) std::uint8_t g_ring[o3os::model::kUndoRegionBytes];
LaneStore g_store;

LaneEvent note(std::uint32_t tick, std::uint32_t len, std::uint8_t n) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = static_cast<std::uint16_t>(len);
  e.kind_channel = 0;
  e.port = 0;
  e.a = n;
  e.b = 100;
  return e;
}

LaneEvent cc(std::uint32_t tick, std::uint8_t ctl, std::uint8_t v) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = 0;
  e.kind_channel = static_cast<std::uint8_t>(1u << 4);
  e.a = ctl;
  e.b = v;
  return e;
}

bool same_event(const LaneEvent& a, const LaneEvent& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

bool lane_is(const LaneStore& s, std::uint32_t pattern, std::uint32_t track, const LaneEvent* e, std::uint32_t n) {
  const LaneStore::View v = s.lane(pattern, track);
  if (v.count != n) return false;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (!same_event(v.events[i], e[i])) return false;
  }
  return true;
}

// Fresh project with patterns 1 (128 steps = 6144 ticks, the whole lane range) and 2 (16 steps), an empty store attached to a new Editor.
struct Rig {
  UndoJournal journal;
  Editor* ed = nullptr;
  bool ok = false;
  alignas(Editor) unsigned char ed_mem[sizeof(Editor)];

  // ring_bytes: the device's Pattern ring is 160 KiB; the tests of a unit that does not fit the ring use a smaller one, because the 7,680-event pool
  // (OS-MIDIREC-P1, delay spare) holds at most 61,472 bytes of lane records
  explicit Rig(std::size_t ring_bytes = 160 * 1024) {
    g_arena.project.reset();
    g_store.clear();
    g_store.adopt(g_arena.project.load_epoch());
    if (!journal.begin(g_ring, ring_bytes)) return;
    ed = new (ed_mem) Editor(g_arena.project, journal);
    ed->attach_lanes(&g_store);
    ok = ed->create_pattern(1, 128) == Status::Ok && ed->create_pattern(2, 16) == Status::Ok;
    ed->history_reset();
  }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;
  ~Rig() {
    if (ed != nullptr) ed->~Editor();
  }
};

}  // namespace

TEST_CASE("lane store: insert, order, queries and capacity") {
  LaneStore s;
  CHECK(s.lane_count() == 0u);
  CHECK(s.total_events() == 0u);
  CHECK(s.lane(1, 0).count == 0u);
  CHECK(s.lane(1, 0).events == nullptr);
  CHECK(s.next_key(0) == LaneStore::kKeys);
  CHECK(LaneStore::key_of(1, 0) == 0u);
  CHECK(LaneStore::key_of(255, 15) == LaneStore::kKeys - 1);
  CHECK(LaneStore::pattern_of(LaneStore::key_of(7, 3)) == 7u);
  CHECK(LaneStore::track_of(LaneStore::key_of(7, 3)) == 3u);
  CHECK_FALSE(LaneStore::valid_key(0, 0));
  CHECK_FALSE(LaneStore::valid_key(256, 0));
  CHECK_FALSE(LaneStore::valid_key(1, 16));

  // lanes are kept in key order whatever the order they are made in
  LaneEvent* a = s.open_gap(5, 2, 0, 2);
  NEED(a != nullptr);
  a[0] = note(0, 10, 60);
  a[1] = note(48, 10, 62);
  LaneEvent* b = s.open_gap(1, 15, 0, 1);
  NEED(b != nullptr);
  b[0] = note(5, 6, 40);
  LaneEvent* c = s.open_gap(5, 0, 0, 3);
  NEED(c != nullptr);
  c[0] = cc(1, 7, 1);
  c[1] = cc(2, 7, 2);
  c[2] = cc(3, 7, 3);
  CHECK(s.lane_count() == 3u);
  CHECK(s.total_events() == 6u);
  CHECK(s.next_key(0) == LaneStore::key_of(1, 15));
  CHECK(s.next_key(LaneStore::key_of(1, 15) + 1) == LaneStore::key_of(5, 0));
  CHECK(s.pattern_events(5) == 5u);
  CHECK(s.pattern_events(1) == 1u);
  CHECK(s.lane(1, 15).events[0].a == 40);
  CHECK(s.lane(5, 0).events[2].a == 7);
  CHECK(s.lane(5, 0).events[2].b == 3);
  CHECK(s.lane(5, 2).events[1].a == 62);
  CHECK(LaneStore::lower_bound(s.lane(5, 2), 0) == 0u);
  CHECK(LaneStore::lower_bound(s.lane(5, 2), 1) == 1u);
  CHECK(LaneStore::lower_bound(s.lane(5, 2), 48) == 1u);
  CHECK(LaneStore::lower_bound(s.lane(5, 2), 49) == 2u);

  // a gap inside a lane keeps the neighbours
  LaneEvent* m = s.open_gap(5, 2, 1, 1);
  NEED(m != nullptr);
  m[0] = note(20, 4, 61);
  CHECK(s.lane(5, 2).count == 3u);
  CHECK(s.lane(5, 2).events[0].a == 60);
  CHECK(s.lane(5, 2).events[1].a == 61);
  CHECK(s.lane(5, 2).events[2].a == 62);
  CHECK(s.lane(5, 0).events[0].a == 7);  // later lane intact

  // refusals change nothing
  CHECK(s.open_gap(0, 0, 0, 1) == nullptr);
  CHECK(s.open_gap(1, 16, 0, 1) == nullptr);
  CHECK(s.open_gap(5, 2, 0, 0) == nullptr);
  CHECK(s.open_gap(5, 2, 9, 1) == nullptr);
  CHECK_FALSE(s.erase(5, 2, 2, 2));
  CHECK_FALSE(s.erase(9, 9, 0, 1));
  CHECK(s.total_events() == 7u);

  // erase the middle, then whole lanes
  CHECK(s.erase(5, 2, 1, 1));
  CHECK(s.lane(5, 2).count == 2u);
  CHECK(s.lane(5, 2).events[1].a == 62);
  CHECK(s.erase(1, 15, 0, 1));
  CHECK(s.lane_count() == 2u);
  CHECK(s.lane(1, 15).count == 0u);
  CHECK(s.erase(5, 0, 0, 3));
  CHECK(s.erase(5, 2, 0, 2));
  CHECK(s.lane_count() == 0u);
  CHECK(s.total_events() == 0u);

  // capacity: 4096 a lane, 7,680 in all (the delay-spare pool), 2048 lanes
  CHECK(s.can_grow(1, 0, 4096));
  CHECK_FALSE(s.can_grow(1, 0, 4097));
  for (std::uint32_t l = 0; l < 2; ++l) {
    const std::uint32_t n = l == 0 ? 4096 : o3os::model::kMaxLanePoolEvents - 4096;  // 4096 + 3584
    LaneEvent* g = s.open_gap(1 + l, 0, 0, n);
    NEED(g != nullptr);
    for (std::uint32_t i = 0; i < n; ++i) g[i] = note(i, 1, 1);
  }
  CHECK(s.total_events() == 7680u);
  CHECK_FALSE(s.can_grow(9, 0, 1));
  CHECK(s.open_gap(9, 0, 0, 1) == nullptr);
  CHECK_FALSE(s.can_grow(1, 1, 1));
  CHECK(s.lane_count() == 2u);
  s.clear();
  CHECK(s.total_events() == 0u);

  // the 2049th lane is refused
  for (std::uint32_t k = 0; k < 2048; ++k) {
    LaneEvent* g = s.open_gap(LaneStore::pattern_of(k), LaneStore::track_of(k), 0, 1);
    NEED(g != nullptr);
    g[0] = note(0, 1, 1);
  }
  CHECK(s.lane_count() == 2048u);
  CHECK_FALSE(s.can_grow(LaneStore::pattern_of(2048), LaneStore::track_of(2048), 1));
  CHECK(s.can_grow(1, 0, 1));  // an existing lane may still grow
}

TEST_CASE("lane store: the epoch drops a stale store") {
  LaneStore s;
  s.adopt(3);
  LaneEvent* g = s.open_gap(1, 0, 0, 1);
  NEED(g != nullptr);
  g[0] = note(0, 1, 1);
  CHECK(s.in_sync(3));
  s.sync(3);
  CHECK(s.total_events() == 1u);
  s.sync(4);
  CHECK(s.in_sync(4));
  CHECK(s.total_events() == 0u);
  CHECK(s.lane_count() == 0u);
}

TEST_CASE("lane store: event validity") {
  using o3os::model::lane_event_valid;
  using o3os::model::lane_events_valid;
  CHECK(lane_event_valid(note(0, 1, 127)));
  CHECK(lane_event_valid(note(6143, 6144, 0)));
  CHECK_FALSE(lane_event_valid(note(6144, 1, 0)));
  CHECK_FALSE(lane_event_valid(note(0, 0, 0)));
  CHECK_FALSE(lane_event_valid(note(0, 6145, 0)));
  CHECK_FALSE(lane_event_valid(note(0, 1, 128)));
  CHECK(lane_event_valid(cc(0, 1, 127)));
  LaneEvent bad = cc(0, 1, 1);
  bad.len = 1;
  CHECK_FALSE(lane_event_valid(bad));
  bad = cc(0, 1, 1);
  bad.kind_channel = static_cast<std::uint8_t>(6u << 4);
  CHECK_FALSE(lane_event_valid(bad));
  bad = cc(0, 1, 1);
  bad.port = 4;
  CHECK_FALSE(lane_event_valid(bad));
  bad = cc(0, 1, 128);
  CHECK_FALSE(lane_event_valid(bad));

  const LaneEvent two[] = {note(1, 1, 1), note(1, 1, 2)};  // ties are in order
  CHECK(lane_events_valid(two, 2, 0, 10));
  const LaneEvent back[] = {note(2, 1, 1), note(1, 1, 2)};
  CHECK_FALSE(lane_events_valid(back, 2, 0, 10));
  CHECK_FALSE(lane_events_valid(two, 2, 2, 10));  // below the range
  CHECK_FALSE(lane_events_valid(two, 2, 0, 1));   // at the end of the range
  CHECK(lane_events_valid(nullptr, 0, 0, 1));
}

TEST_CASE("editor lanes: replace, undo, redo") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent first[] = {note(0, 10, 60), note(100, 10, 62), note(300, 5, 64)};
  CHECK(ed.lane_replace(1, 3, 0, 6144, first, 3) == Status::Ok);
  CHECK(lane_is(g_store, 1, 3, first, 3));
  CHECK(ed.can_undo());

  // replace the middle range only: the events outside [t0, t1) stay
  const LaneEvent mid[] = {cc(90, 7, 1), cc(120, 7, 2)};
  CHECK(ed.lane_replace(1, 3, 50, 200, mid, 2) == Status::Ok);
  const LaneEvent want[] = {first[0], mid[0], mid[1], first[2]};
  CHECK(lane_is(g_store, 1, 3, want, 4));

  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 3, first, 3));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_store.lane(1, 3).count == 0u);
  CHECK(g_store.lane_count() == 0u);
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 3, first, 3));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 3, want, 4));
  CHECK(ed.redo() == UndoResult::Nothing);

  // the same events as there are: applied, not recorded
  const std::uint32_t depth = r.journal.depth();
  CHECK(ed.lane_replace(1, 3, 50, 200, mid, 2) == Status::Ok);
  CHECK(r.journal.depth() == depth);

  // clear
  CHECK(ed.lane_clear(1, 3) == Status::Ok);
  CHECK(g_store.lane(1, 3).count == 0u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 3, want, 4));

  // a lane edit moves the pattern's generation (the native Writer's Modified check sees it)
  const std::uint32_t gen = g_arena.project.generation();
  CHECK(ed.lane_replace(2, 0, 0, 100, first, 1) == Status::Ok);
  CHECK(g_arena.project.generation() != gen);
  CHECK(ed.in_sync());
}

TEST_CASE("editor lanes: refusals change nothing") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent one[] = {note(10, 5, 60)};
  const std::uint32_t gen = g_arena.project.generation();
  CHECK(ed.lane_replace(9, 0, 0, 100, one, 1) == Status::NotFound);   // no such pattern
  CHECK(ed.lane_replace(1, 16, 0, 100, one, 1) == Status::BadIndex);  // track
  CHECK(ed.lane_replace(1, 0, 100, 100, one, 1) == Status::BadValue);
  CHECK(ed.lane_replace(1, 0, 0, 6145, one, 1) == Status::BadValue);
  CHECK(ed.lane_replace(1, 0, 20, 100, one, 1) == Status::BadValue);  // event below t0
  CHECK(ed.lane_replace(1, 0, 0, 10, one, 1) == Status::BadValue);   // event at t1
  const LaneEvent unsorted[] = {note(20, 1, 1), note(10, 1, 1)};
  CHECK(ed.lane_replace(1, 0, 0, 100, unsorted, 2) == Status::BadValue);
  LaneEvent bad = note(10, 1, 200);
  CHECK(ed.lane_replace(1, 0, 0, 100, &bad, 1) == Status::BadValue);
  CHECK(ed.lane_replace(1, 0, 0, 100, nullptr, 1) == Status::BadValue);
  CHECK(g_store.total_events() == 0u);
  CHECK(g_arena.project.generation() == gen);
  CHECK_FALSE(ed.can_undo());

  // no store attached: NotFound
  ed.attach_lanes(nullptr);
  CHECK(ed.lane_replace(1, 0, 0, 100, one, 1) == Status::NotFound);
  CHECK(ed.lane_clear(1, 0) == Status::NotFound);
}

TEST_CASE("editor lanes: a full lane or pool answers Full and keeps the lane") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  static LaneEvent big[4097];
  for (std::uint32_t i = 0; i < 4097; ++i) big[i] = note(i, 1, 1);
  CHECK(ed.lane_replace(1, 0, 0, 6144, big, 4097) == Status::BadValue);  // more than a lane holds
  CHECK(ed.lane_replace(1, 0, 0, 6144, big, 4096) == Status::Ok);
  const LaneEvent extra[] = {note(5000, 1, 1)};
  CHECK(ed.lane_replace(1, 0, 4096, 6144, extra, 1) == Status::Full);
  CHECK(g_store.lane(1, 0).count == 4096u);
  // fill the pool: one more lane of 3584 = 7,680
  CHECK(ed.lane_replace(1, 1, 0, 6144, big, 3584) == Status::Ok);
  CHECK(g_store.total_events() == 7680u);
  CHECK(ed.lane_replace(1, 6, 0, 6144, extra, 1) == Status::Full);
  CHECK(g_store.total_events() == 7680u);
  // the same lane replaced by a smaller one works, and room is freed
  CHECK(ed.lane_replace(1, 1, 0, 6144, extra, 1) == Status::Ok);
  CHECK(g_store.total_events() == 4097u);
  CHECK(ed.lane_replace(1, 6, 0, 6144, extra, 1) == Status::Ok);
}

TEST_CASE("editor lanes: A17 shortening a track deletes the events past the end, in one unit, and undo restores them") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  // events at 0, 1000, 1536 (step 32), 2000, 3000 (inside 64 steps = 3072 ticks)
  const LaneEvent ev[] = {note(0, 10, 60), note(1000, 10, 61), note(1536, 10, 62), note(2000, 10, 63), note(3000, 10, 64)};
  NEED(ed.lane_replace(1, 4, 0, 3072, ev, 5) == Status::Ok);
  NEED(ed.lane_replace(1, 5, 0, 3072, ev, 5) == Status::Ok);  // another track: not touched by a track length
  ed.history_reset();

  CHECK(ed.set_track_length(1, 4, 32) == Status::Ok);  // 32 steps = 1536 ticks: events at tick >= 1536 go
  CHECK(lane_is(g_store, 1, 4, ev, 2));
  CHECK(lane_is(g_store, 1, 5, ev, 5));
  CHECK(r.journal.depth() == 1u);  // the length and the cut are one unit
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 4, ev, 5));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 4, ev, 2));
  CHECK(ed.undo() == UndoResult::Ok);

  // lengthening does not delete or add anything
  CHECK(ed.set_track_length(1, 4, 64) == Status::Ok);
  CHECK(lane_is(g_store, 1, 4, ev, 5));
  CHECK(ed.set_track_length(1, 4, 128) == Status::Ok);
  CHECK(lane_is(g_store, 1, 4, ev, 5));
  ed.history_reset();

  // set_pattern_length cuts every track of the pattern
  CHECK(ed.set_pattern_length(1, 16) == Status::Ok);  // 768 ticks
  CHECK(lane_is(g_store, 1, 4, ev, 1));
  CHECK(lane_is(g_store, 1, 5, ev, 1));
  CHECK(r.journal.depth() == 1u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 4, ev, 5));
  CHECK(lane_is(g_store, 1, 5, ev, 5));
  CHECK(g_arena.project.check_invariants());
}

TEST_CASE("editor lanes: deleting or clearing a pattern frees its lanes, and undo brings both back") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent ev[] = {note(0, 10, 60), note(100, 10, 61)};
  const LaneEvent other[] = {cc(5, 1, 1)};
  NEED(ed.lane_replace(2, 0, 0, 768, ev, 2) == Status::Ok);
  NEED(ed.lane_replace(2, 7, 0, 768, ev, 1) == Status::Ok);
  NEED(ed.lane_replace(1, 0, 0, 768, other, 1) == Status::Ok);
  ed.history_reset();

  CHECK(ed.clear_pattern(2) == Status::Ok);
  CHECK(g_store.pattern_events(2) == 0u);
  CHECK(lane_is(g_store, 1, 0, other, 1));
  CHECK(r.journal.depth() == 1u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 2, 0, ev, 2));
  CHECK(lane_is(g_store, 2, 7, ev, 1));

  CHECK(ed.delete_pattern(2) == Status::Ok);
  CHECK(g_arena.project.pattern(2) == nullptr);
  CHECK(g_store.pattern_events(2) == 0u);
  CHECK(lane_is(g_store, 1, 0, other, 1));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_arena.project.pattern(2) != nullptr);
  CHECK(lane_is(g_store, 2, 0, ev, 2));
  CHECK(lane_is(g_store, 2, 7, ev, 1));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(g_store.pattern_events(2) == 0u);
  CHECK(g_arena.project.check_invariants());
}

TEST_CASE("editor lanes: a project load invalidates the store at the next edit") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent ev[] = {note(0, 10, 60)};
  NEED(ed.lane_replace(1, 0, 0, 768, ev, 1) == Status::Ok);
  g_arena.project.reset();  // a load or a new project: the epoch moves
  CHECK_FALSE(g_store.in_sync(g_arena.project.load_epoch()));
  NEED(g_arena.project.create_pattern(1, 16) == Status::Ok);
  ed.history_reset();
  CHECK(ed.lane_replace(1, 1, 0, 768, ev, 1) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == 0u);  // the old lane is gone
  CHECK(g_store.lane(1, 1).count == 1u);
}

namespace {

constexpr std::uint32_t kFullLane = 4096;
constexpr std::uint32_t kHalfLane = o3os::model::kMaxLanePoolEvents - kFullLane;  // 3584: with one full lane, the whole pool
constexpr std::size_t kSmallRing = 56u * 1024u;  // takes one lane record, not the two (61,472 bytes)
constexpr std::size_t kTinyRing = 48u * 1024u;   // the shortening and group cases below
LaneEvent g_full[kFullLane];  // 4096 events at ticks 0, 1, 3, 4, 6, ... 6142 (sorted, inside the whole 128-step range)

void make_full() {
  for (std::uint32_t i = 0; i < kFullLane; ++i) g_full[i] = note(i * 3 / 2, 1, static_cast<std::uint8_t>(i & 0x7F));
}

// The ring state an edit that is refused must leave as it was.
struct RingState {
  std::size_t used;
  unsigned depth, redo;
  std::uint32_t dropped, lost;
  explicit RingState(const UndoJournal& j)
      : used(j.used_bytes()), depth(j.depth()), redo(j.redo_depth()), dropped(j.dropped_units()), lost(j.lost_units()) {}
  bool operator==(const RingState& o) const {
    return used == o.used && depth == o.depth && redo == o.redo && dropped == o.dropped && lost == o.lost;
  }
};

}  // namespace

TEST_CASE("editor lanes: a delete whose lane records would not fit the undo ring is refused, nothing changes") {
  Rig r(kSmallRing);
  NEED(r.ok);
  Editor& ed = *r.ed;
  make_full();
  // The pool (7,680 events) is one lane of 4096 and one of 3584: records of 32,784 and 28,688 bytes. A 56 KiB ring takes either alone, but
  // the two lanes (61,472 bytes) do not fit it (the device's 160 KiB ring takes the whole pool: the refusal is a safety net there).
  NEED(ed.lane_replace(1, 0, 0, 6144, g_full, kFullLane) == Status::Ok);
  NEED(ed.lane_replace(1, 1, 0, 6144, g_full, kHalfLane) == Status::Ok);
  NEED(ed.can_undo());
  const RingState before(r.journal);

  CHECK(ed.delete_pattern(1) == Status::Full);
  CHECK(g_arena.project.pattern(1) != nullptr);
  CHECK(lane_is(g_store, 1, 0, g_full, kFullLane));
  CHECK(lane_is(g_store, 1, 1, g_full, kHalfLane));
  CHECK(RingState(r.journal) == before);
  CHECK(r.journal.lost_units() == 0u);
  // CLEAR needs the same room (the lanes plus the steps' image when there are steps): the lanes alone are 61,472 bytes
  CHECK(ed.clear_pattern(1) == Status::Full);
  CHECK(lane_is(g_store, 1, 0, g_full, kFullLane));
  CHECK(lane_is(g_store, 1, 1, g_full, kHalfLane));
  CHECK(RingState(r.journal) == before);
  // the history still works: the last take comes off
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_store.lane(1, 1).count == 0u);
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 1, g_full, kHalfLane));

  // with one lane less it fits: deleted in one unit, and undo brings the pattern and the lane back
  CHECK(ed.lane_clear(1, 1) == Status::Ok);
  CHECK(ed.delete_pattern(1) == Status::Ok);
  CHECK(g_arena.project.pattern(1) == nullptr);
  CHECK(g_store.pattern_events(1) == 0u);
  CHECK(r.journal.lost_units() == 0u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_arena.project.pattern(1) != nullptr);
  CHECK(lane_is(g_store, 1, 0, g_full, kFullLane));
  CHECK(g_store.lane(1, 1).count == 0u);
  CHECK(g_arena.project.check_invariants());
}

TEST_CASE("editor lanes: shortening a lane-heavy pattern that would not fit the undo ring is refused, nothing changes") {
  Rig r(kTinyRing);
  NEED(r.ok);
  Editor& ed = *r.ed;
  make_full();
  NEED(ed.lane_replace(1, 0, 0, 6144, g_full, kFullLane) == Status::Ok);  // the pool is full: 4096 + 3584
  NEED(ed.lane_replace(1, 1, 0, 6144, g_full, kHalfLane) == Status::Ok);
  const RingState before(r.journal);

  // 16 steps = 768 ticks keeps 512 events of each lane and cuts 3584 and 3072: 28,688 + 24,592 bytes do not fit 48 KiB
  CHECK(ed.set_pattern_length(1, 16) == Status::Full);
  for (std::uint32_t t = 0; t < 16; ++t) CHECK(g_arena.project.pattern(1)->track_length[t] == 128u);
  CHECK(lane_is(g_store, 1, 0, g_full, kFullLane));
  CHECK(lane_is(g_store, 1, 1, g_full, kHalfLane));
  CHECK(RingState(r.journal) == before);
  CHECK(r.journal.lost_units() == 0u);
  // an invalid length is still the Project's own answer
  CHECK(ed.set_pattern_length(1, 0) == Status::BadValue);
  CHECK(ed.set_track_length(1, 0, 129) == Status::BadValue);

  // 100 steps = 4800 ticks cuts 896 and 384 events (7184 + 3088 bytes): fits, one unit, undo restores
  CHECK(ed.set_pattern_length(1, 100) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == 3200u);
  CHECK(g_store.lane(1, 1).count == 3200u);
  CHECK(r.journal.lost_units() == 0u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, g_full, kFullLane));
  CHECK(lane_is(g_store, 1, 1, g_full, kHalfLane));
  for (std::uint32_t t = 0; t < 16; ++t) CHECK(g_arena.project.pattern(1)->track_length[t] == 128u);

  // a single track fits whatever it holds (one lane record is at most 32,784 bytes)
  CHECK(ed.set_track_length(1, 0, 16) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == 512u);
  CHECK(g_arena.project.check_invariants());
}

TEST_CASE("editor lanes: a long group refuses a take that would outgrow the undo ring before it changes anything") {
  Rig r(kTinyRing);
  NEED(r.ok);
  Editor& ed = *r.ed;
  make_full();
  ed.begin_group(o3os::core::ActionId::None, 0, o3os::model::UndoArea::Pattern);
  CHECK(ed.lane_replace(1, 0, 0, 6144, g_full, kFullLane) == Status::Ok);
  // 32,784 + the unit's 20 bytes are held; a second lane record of 28,688 would pass 48 KiB
  CHECK(ed.lane_replace(1, 1, 0, 6144, g_full, kHalfLane) == Status::Full);
  CHECK(g_store.lane(1, 1).count == 0u);
  CHECK(ed.lane_replace(1, 1, 0, 6144, g_full, 100) == Status::Ok);  // a smaller one fits
  ed.end_group();
  CHECK(r.journal.depth() == 1u);
  CHECK(r.journal.lost_units() == 0u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_store.total_events() == 0u);
}

TEST_CASE("editor lanes: lane_replace stays inside the track (A17); lane_clear does not care") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent ev[] = {note(0, 10, 60), note(700, 10, 61)};
  CHECK(ed.lane_replace(2, 0, 0, 769, ev, 2) == Status::BadValue);  // pattern 2: 16 steps = 768 ticks
  CHECK(ed.lane_replace(2, 0, 0, 6144, ev, 2) == Status::BadValue);
  CHECK(g_store.lane(2, 0).count == 0u);
  CHECK(ed.lane_replace(2, 0, 0, 768, ev, 2) == Status::Ok);  // a take over the whole track ends exactly at its length
  CHECK(lane_is(g_store, 2, 0, ev, 2));
  // a longer track takes more, a shorter one less
  CHECK(ed.set_track_length(2, 1, 32) == Status::Ok);
  CHECK(ed.lane_replace(2, 1, 0, 1537, ev, 2) == Status::BadValue);
  CHECK(ed.lane_replace(2, 1, 0, 1536, ev, 2) == Status::Ok);
  CHECK(ed.set_track_length(2, 1, 8) == Status::Ok);  // 384 ticks: the event at 700 goes
  CHECK(g_store.lane(2, 1).count == 1u);
  CHECK(ed.lane_clear(2, 1) == Status::Ok);  // t1 = 6144 > 384 and still allowed
  CHECK(g_store.lane(2, 1).count == 0u);
  CHECK(g_arena.project.check_invariants());
}

// open_gap / erase against a plain std::vector per lane: fixed seed, a few thousand random operations.
TEST_CASE("lane store: random open_gap and erase match a vector model") {
  static LaneStore s;
  s.clear();
  constexpr std::uint32_t kKeysUsed = 8;
  static const std::uint32_t pats[kKeysUsed] = {1, 1, 2, 7, 100, 254, 255, 255};
  static const std::uint32_t trks[kKeysUsed] = {0, 15, 3, 0, 8, 1, 0, 15};
  std::vector<LaneEvent> model[kKeysUsed];
  std::uint32_t rng = 0x2545F491u;
  auto next = [&rng](std::uint32_t n) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % n;
  };
  std::uint32_t serial = 0;
  std::uint32_t refused = 0;
  std::uint32_t grown = 0;
  std::uint32_t erased = 0;

  auto total = [&]() {
    std::uint32_t t = 0;
    for (const auto& m : model) t += static_cast<std::uint32_t>(m.size());
    return t;
  };
  auto check_all = [&]() {
    std::uint32_t lanes = 0;
    for (std::uint32_t k = 0; k < kKeysUsed; ++k) {
      const LaneStore::View v = s.lane(pats[k], trks[k]);
      NEED(v.count == model[k].size());
      if (!model[k].empty()) ++lanes;
      NEED(model[k].empty() || std::memcmp(v.events, model[k].data(), model[k].size() * sizeof(LaneEvent)) == 0);
    }
    NEED(s.lane_count() == lanes);
    NEED(s.total_events() == total());
  };

  for (std::uint32_t op = 0; op < 4000; ++op) {
    const std::uint32_t k = next(kKeysUsed);
    std::vector<LaneEvent>& m = model[k];
    const std::uint32_t n = static_cast<std::uint32_t>(m.size());
    if (next(100) < 55) {
      const std::uint32_t at = next(n + 1);
      const std::uint32_t count = 1 + next(op % 7 == 0 ? 2500 : 300);
      const bool fits = n + count <= 4096 && total() + count <= o3os::model::kMaxLanePoolEvents;
      LaneEvent* gap = s.open_gap(pats[k], trks[k], at, count);
      if (!fits) {
        NEED(gap == nullptr);
        ++refused;
      } else {
        NEED(gap != nullptr);
        std::vector<LaneEvent> add(count);
        for (std::uint32_t i = 0; i < count; ++i) {
          ++serial;
          add[i] = note(serial & 0x1FFF, 1, 1);
          add[i].a = static_cast<std::uint8_t>(serial & 0x7F);
          add[i].b = static_cast<std::uint8_t>((serial >> 7) & 0x7F);
          gap[i] = add[i];
        }
        m.insert(m.begin() + at, add.begin(), add.end());
        ++grown;
      }
    } else if (n != 0) {
      const std::uint32_t at = next(n);
      const std::uint32_t count = 1 + next(n - at);
      NEED(s.erase(pats[k], trks[k], at, count));
      m.erase(m.begin() + at, m.begin() + at + count);
      ++erased;
    } else {
      NEED(!s.erase(pats[k], trks[k], 0, 1));  // nothing to erase
    }
    // an out-of-range erase changes nothing
    if (op % 97 == 0) {
      NEED(!s.erase(pats[k], trks[k], static_cast<std::uint32_t>(m.size()), 1));
    }
    const LaneStore::View v = s.lane(pats[k], trks[k]);
    NEED(v.count == m.size());
    NEED(m.empty() || std::memcmp(v.events, m.data(), m.size() * sizeof(LaneEvent)) == 0);
    NEED(s.total_events() == total());
    if (op % 100 == 0) check_all();
  }
  check_all();
  CHECK(grown > 100);
  CHECK(erased > 100);
  CHECK(refused > 0);  // the capacity limits were reached
}

// The Editor's lane record of n events (8 n + 16: Editor::lane_record_bytes, private).
std::size_t lane_rec(std::uint32_t n) { return 8u * n + 16u; }

TEST_CASE("editor lanes: a replace records only what changes, 1x the lane data (maintainer 2026-11-10)") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  make_full();
  // a take into an empty lane records the new events once
  CHECK(ed.lane_replace(1, 0, 0, 6144, g_full, 1000) == Status::Ok);
  CHECK(r.journal.depth() == 1u);
  CHECK(r.journal.used_bytes() < lane_rec(1000) + 64u);  // 8 n + 16 and the unit's own header: not 2x

  // the same events plus 10 more at the end: only the 10 are recorded (the head stays in place)
  const std::size_t before = r.journal.used_bytes();
  CHECK(ed.lane_replace(1, 0, 0, 6144, g_full, 1010) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == 1010u);
  CHECK(r.journal.used_bytes() - before < lane_rec(10) + 64u);
  CHECK(lane_is(g_store, 1, 0, g_full, 1010));

  // a change in the middle records the old and the new event only
  LaneEvent mod[1010];
  std::memcpy(mod, g_full, sizeof mod);
  mod[500] = cc(g_full[500].tick, 7, 9);
  const std::size_t b2 = r.journal.used_bytes();
  CHECK(ed.lane_replace(1, 0, 0, 6144, mod, 1010) == Status::Ok);
  CHECK(lane_is(g_store, 1, 0, mod, 1010));
  CHECK(r.journal.used_bytes() - b2 < 2 * lane_rec(1) + 64u);

  // OS-MIDIREC-P1 S2 (fold a): one replace is ONE write window for the render side: seq() moves by exactly one even step (the gap, the writes and the record never show an even seq in between)
  const std::uint32_t seq0 = g_store.seq();
  CHECK((seq0 & 1u) == 0u);
  mod[501] = cc(g_full[501].tick, 8, 10);
  CHECK(ed.lane_replace(1, 0, 0, 6144, mod, 1010) == Status::Ok);
  CHECK(g_store.seq() == seq0 + 2u);
  CHECK(ed.undo() == UndoResult::Ok);  // back to `mod` before the three undos below
  CHECK((g_store.seq() & 1u) == 0u);

  // undo three times: every state comes back exactly
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, g_full, 1010));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, g_full, 1000));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_store.lane(1, 0).count == 0u);

  // full lanes held in the history, then one event off the end of one: a record of one event
  CHECK(ed.lane_replace(1, 0, 0, 6144, g_full, kFullLane) == Status::Ok);
  CHECK(ed.lane_replace(1, 1, 0, 6144, g_full, kHalfLane) == Status::Ok);
  CHECK(ed.lane_replace(1, 0, 0, 6144, g_full, kFullLane - 1) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == kFullLane - 1);
}

TEST_CASE("lane store: seq() is odd inside a write window, even and changed after it, and windows nest") {
  LaneStore s;
  s.adopt(1);
  const std::uint32_t s0 = s.seq();
  CHECK((s0 & 1u) == 0u);
  {
    LaneStore::WriteWindow outer(s);
    CHECK((s.seq() & 1u) == 1u);
    {
      LaneStore::WriteWindow inner(s);
      CHECK((s.seq() & 1u) == 1u);
    }
    CHECK((s.seq() & 1u) == 1u);  // the inner window ending does not close the outer
  }
  CHECK((s.seq() & 1u) == 0u);
  CHECK(s.seq() != s0);

  // every mutator moves it, an unchanged store does not
  const std::uint32_t s1 = s.seq();
  CHECK(s.seq() == s1);
  LaneEvent* g = s.open_gap(1, 0, 0, 1);
  NEED(g != nullptr);
  g[0] = note(0, 1, 1);
  CHECK((s.seq() & 1u) == 0u);
  CHECK(s.seq() != s1);
  const std::uint32_t s2 = s.seq();
  CHECK(s.erase(1, 0, 0, 1));
  CHECK((s.seq() & 1u) == 0u);
  CHECK(s.seq() != s2);
  const std::uint32_t s3 = s.seq();
  s.clear();
  CHECK((s.seq() & 1u) == 0u);
  CHECK(s.seq() != s3);
}

TEST_CASE("lane store: sync after a project reset drops the lanes of the replaced project (the app's load and NEW paths)") {
  static o3os::model::Project p;  // large: static, like the app's; reset() is what initialises it (the app's first call too)
  p.reset();
  LaneStore s;
  s.sync(p.load_epoch());
  LaneEvent* g = s.open_gap(1, 0, 0, 1);
  NEED(g != nullptr);
  g[0] = note(0, 1, 1);
  CHECK(s.total_events() == 1u);
  s.sync(p.load_epoch());  // same project: untouched
  CHECK(s.total_events() == 1u);
  p.reset();
  CHECK_FALSE(s.in_sync(p.load_epoch()));
  s.sync(p.load_epoch());
  CHECK(s.total_events() == 0u);
  CHECK(s.in_sync(p.load_epoch()));
}
