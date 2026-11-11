// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/core/action.h>
#include <o3os/model/editor.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/lane_take.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "support/need.h"

// OS-MIDIREC-P1 S3: the take buffer (thinning A9, LEN, wrap A11, full buffer) and its merge into the lane (A10, A15).

namespace {

using o3os::model::Editor;
using o3os::model::LaneEvent;
using o3os::model::LaneStore;
using o3os::model::LaneTake;
using o3os::model::Status;
using o3os::model::UndoJournal;
using o3os::model::UndoResult;

o3os::model::ModelArena g_arena;
alignas(8) std::uint8_t g_ring[o3os::model::kUndoRegionBytes];
LaneStore g_store;
LaneTake g_take;

LaneEvent note(std::uint32_t tick, std::uint32_t len, std::uint8_t n, std::uint8_t ch = 0) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = static_cast<std::uint16_t>(len);
  e.kind_channel = ch;
  e.a = n;
  e.b = 100;
  return e;
}

LaneEvent cc(std::uint32_t tick, std::uint8_t ctl, std::uint8_t v, std::uint8_t ch = 0) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.kind_channel = static_cast<std::uint8_t>((1u << 4) | ch);
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

bool take_is(const LaneTake& t, const LaneEvent* e, std::uint32_t n) {
  if (t.count() != n) return false;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (!same_event(t.events()[i], e[i])) return false;
  }
  return true;
}

// Patterns 1 (128 steps) and 2 (16 steps = 768 ticks), an empty store attached to a new Editor.
struct Rig {
  UndoJournal journal;
  Editor* ed = nullptr;
  bool ok = false;
  alignas(Editor) unsigned char ed_mem[sizeof(Editor)];

  explicit Rig(std::size_t ring_bytes = 160 * 1024) {
    g_arena.project.reset();
    g_store.clear();
    g_store.adopt(g_arena.project.load_epoch());
    g_take.reset();
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

TEST_CASE("lane take: CC thinning keeps the first and the last of a move, one point per 12 ticks, drops equal values (A9)") {
  g_take.reset();
  g_take.begin(2, 0, 0, 0, 0, 768);
  // A ramp every 2 ticks from 10 up by 3: the first is kept, then one per 12 ticks, the last of the move is kept
  for (std::uint32_t i = 0; i < 20; ++i) CHECK(g_take.cc(i * 2, 7, static_cast<std::uint8_t>(10 + i * 3)));
  CHECK(g_take.cc(38, 7, 67));  // the same value again: dropped (deadband)
  g_take.finish(40);
  // streaming keeps 0, 12, 24, 36 and the last (38); the ramp is a straight line, so the co-linear pass leaves its two ends
  CHECK(g_take.count() == 2u);
  CHECK(g_take.events()[0].tick == 0);
  CHECK(g_take.events()[0].b == 10);
  CHECK(g_take.events()[g_take.count() - 1].b == 67);  // the last of the move is there
  for (std::uint32_t i = 1; i < g_take.count(); ++i) {
    CHECK(g_take.events()[i].tick >= g_take.events()[i - 1].tick);
  }
  CHECK(g_take.moved(7));
  CHECK_FALSE(g_take.moved(8));
  CHECK(g_take.interval_count() == 1u);
  CHECK(g_take.interval(0).t0 == 0);
  CHECK(g_take.interval(0).t1 == 41);
}

TEST_CASE("lane take: a point equal to the kept value is dropped, a move that goes back needs no pending point") {
  g_take.reset();
  g_take.begin(2, 0, 0, 0, 0, 768);
  CHECK(g_take.cc(0, 1, 50));
  CHECK(g_take.cc(2, 1, 50));  // equal: dropped
  CHECK(g_take.cc(4, 1, 53));  // pending
  CHECK(g_take.cc(6, 1, 50));  // back at the kept value: the pending goes
  g_take.finish(10);
  const LaneEvent want[] = {cc(0, 1, 50)};
  CHECK(take_is(g_take, want, 1));
}

TEST_CASE("lane take: a pending point is kept when nothing follows it within the window (the move ended)") {
  g_take.reset();
  g_take.begin(2, 0, 0, 0, 0, 768);
  CHECK(g_take.cc(0, 1, 10));
  CHECK(g_take.cc(5, 1, 20));    // pending
  CHECK(g_take.cc(300, 1, 90));  // far later: the pending was the last of the first move; this starts the next
  g_take.finish(310);
  const LaneEvent want[] = {cc(0, 1, 10), cc(5, 1, 20), cc(300, 1, 90)};
  CHECK(take_is(g_take, want, 3));
}

TEST_CASE("lane take: the co-linear pass drops an interior point on the line between its neighbours") {
  g_take.reset();
  g_take.begin(2, 0, 0, 0, 0, 768);
  // points 24 ticks apart on a straight line 0, 10, 20, 30; then a corner 30 -> 0
  CHECK(g_take.cc(0, 3, 0));
  CHECK(g_take.cc(24, 3, 10));
  CHECK(g_take.cc(48, 3, 20));
  CHECK(g_take.cc(72, 3, 30));
  CHECK(g_take.cc(96, 3, 0));
  g_take.finish(100);
  const LaneEvent want[] = {cc(0, 3, 0), cc(72, 3, 30), cc(96, 3, 0)};
  CHECK(take_is(g_take, want, 3));
}

TEST_CASE("lane take: notes get their LEN, a held note ends at the take's end, the same note again ends the first") {
  g_take.reset();
  g_take.begin(2, 1, 5, 1, 10, 768);
  CHECK(g_take.note_on(10, 60, 100));
  g_take.note_off(58, 60);
  CHECK(g_take.note_on(60, 62, 90));
  CHECK(g_take.note_on(100, 62, 80));  // while held: the first ends at 100
  CHECK(g_take.note_on(120, 64, 0));   // velocity 0 is a note off of a note that is not held: nothing
  g_take.finish(200);                  // 62 (the second) is held: LEN to 200
  LaneEvent a = note(10, 48, 60, 5);
  a.port = 1;
  LaneEvent b = note(60, 40, 62, 5);
  b.port = 1;
  b.b = 90;
  LaneEvent c = note(100, 100, 62, 5);
  c.port = 1;
  c.b = 80;
  const LaneEvent want[] = {a, b, c};
  CHECK(take_is(g_take, want, 3));
}

TEST_CASE("lane take: note_len stores a closed note (STEP mode): a chord is several at one tick, the length is clamped, velocity 0 and a closed take are refused") {
  g_take.reset();
  CHECK_FALSE(g_take.note_len(0, 60, 100, 48));  // not open
  g_take.begin(2, 1, 5, 1, 96, 768);
  CHECK(g_take.note_len(96, 64, 80, 48));
  CHECK(g_take.note_len(96, 60, 100, 48));
  CHECK(g_take.note_len(96, 67, 70, 0));       // LEN 0 is raised to 1
  CHECK(g_take.note_len(96, 72, 70, 70000));   // above 6144: clamped
  CHECK_FALSE(g_take.note_len(96, 60, 0, 48));  // a velocity of 0 is no note
  CHECK_FALSE(g_take.note_len(96, 128, 80, 48));
  g_take.finish(96);
  CHECK(g_take.count() == 4u);
  LaneEvent a = note(96, 48, 64, 5);
  a.port = 1;
  a.b = 80;
  LaneEvent b = note(96, 48, 60, 5);
  b.port = 1;
  LaneEvent c = note(96, 1, 67, 5);
  c.port = 1;
  c.b = 70;
  LaneEvent d = note(96, 6144, 72, 5);
  d.port = 1;
  d.b = 70;
  const LaneEvent want[] = {a, b, c, d};
  CHECK(take_is(g_take, want, 4));
  CHECK(g_take.interval_count() == 1u);
  CHECK(g_take.interval(0).t0 == 96);
  CHECK(g_take.interval(0).t1 == 97);
}

TEST_CASE("lane take: a take that wraps past the end of the track has two intervals and the LEN of a held note wraps") {
  g_take.reset();
  g_take.begin(2, 0, 0, 0, 700, 768);
  CHECK(g_take.note_on(740, 60, 100));
  CHECK(g_take.cc(750, 1, 5));
  CHECK(g_take.cc(10, 1, 90));  // after the wrap
  g_take.all_notes_off(30);
  g_take.finish(40);
  CHECK(g_take.interval_count() == 2u);
  CHECK(g_take.interval(0).t0 == 700);
  CHECK(g_take.interval(0).t1 == 768);
  CHECK(g_take.interval(1).t0 == 0);
  CHECK(g_take.interval(1).t1 == 41);
  CHECK(g_take.count() == 3u);
  CHECK(g_take.events()[0].tick == 10);  // sorted: the post-wrap CC first
  CHECK(g_take.events()[1].tick == 740);
  CHECK(g_take.events()[1].len == 58);  // 740 -> 768 -> 30
  // a take that runs a full loop or more has one interval over the whole track
  g_take.begin(2, 0, 0, 0, 100, 768);
  CHECK(g_take.cc(120, 2, 1));
  CHECK(g_take.cc(20, 2, 9));
  g_take.finish(300);
  CHECK(g_take.interval_count() == 1u);
  CHECK(g_take.interval(0).t0 == 0);
  CHECK(g_take.interval(0).t1 == 768);
}

TEST_CASE("lane take: a full buffer refuses the next event, keeps what it has and says full") {
  Rig r;  // (its constructor resets g_take: first)
  NEED(r.ok);
  g_take.begin(1, 0, 0, 0, 0, 6144);
  std::uint32_t stored = 0;
  for (std::uint32_t i = 0; i < LaneTake::kCapacity + 5; ++i) {
    if (g_take.note_on(i, static_cast<std::uint8_t>(i % 100), 100)) ++stored;
  }
  CHECK(stored == LaneTake::kCapacity);
  CHECK(g_take.full());
  CHECK(g_take.count() == LaneTake::kCapacity);
  g_take.finish(3000);
  CHECK(g_take.count() == LaneTake::kCapacity);
  CHECK_FALSE(g_take.open());
  // the events are valid: a commit takes them (4096 lane events would hold 2048 of them)
  CHECK(r.ed->lane_merge_take(g_take) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == LaneTake::kCapacity);
}

TEST_CASE("lane merge: notes merge after the lane's events at the same tick, a moved CC replaces the old points inside the interval only") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent old[] = {cc(10, 7, 1),   note(50, 10, 60), cc(100, 7, 2), cc(100, 8, 3), cc(150, 7, 4),
                           cc(160, 7, 5, 1),  // another channel: stays
                           cc(300, 7, 6)};  // past the interval: stays
  CHECK(ed.lane_replace(2, 0, 0, 768, old, 7) == Status::Ok);
  ed.history_reset();

  g_take.reset();
  g_take.begin(2, 0, 0, 0, 90, 768);
  CHECK(g_take.cc(100, 7, 40));
  CHECK(g_take.note_on(100, 61, 100));
  g_take.note_off(120, 61);
  CHECK(g_take.cc(200, 7, 90));
  g_take.finish(210);

  ed.begin_group(o3os::core::ActionId::None, 0, o3os::model::UndoArea::Pattern);
  CHECK(ed.lane_merge_take(g_take) == Status::Ok);
  ed.end_group();
  // controller 7 of channel 0 in [90, 211) is replaced: the points at 100 and 150 went; cc 8 and the channel-1 point stay
  const LaneEvent want[] = {cc(10, 7, 1), note(50, 10, 60), cc(100, 8, 3), cc(100, 7, 40), note(100, 20, 61), cc(160, 7, 5, 1), cc(200, 7, 90), cc(300, 7, 6)};
  CHECK(lane_is(g_store, 2, 0, want, 8));

  // one take, one undo unit
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 2, 0, old, 7));
  CHECK(ed.undo() == UndoResult::Nothing);
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 2, 0, want, 8));
}

TEST_CASE("lane merge: into an empty lane, only notes, and a take of no events changes nothing") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  g_take.reset();
  g_take.begin(2, 3, 0, 0, 0, 768);
  g_take.finish(50);
  CHECK(ed.lane_merge_take(g_take) == Status::Ok);
  CHECK(g_store.lane_count() == 0u);
  CHECK_FALSE(ed.can_undo());

  g_take.begin(2, 3, 2, 0, 5, 768);
  CHECK(g_take.note_on(5, 60, 100));
  g_take.note_off(20, 60);
  CHECK(g_take.note_on(30, 64, 100));
  g_take.finish(40);
  CHECK(ed.lane_merge_take(g_take) == Status::Ok);
  const LaneEvent want[] = {note(5, 15, 60, 2), note(30, 10, 64, 2)};
  CHECK(lane_is(g_store, 2, 3, want, 2));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_store.lane_count() == 0u);
}

TEST_CASE("lane merge: a wrapped take is two runs of one unit, undo and redo restore both") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent old[] = {cc(5, 1, 7), cc(20, 1, 8), note(300, 5, 50), cc(710, 1, 9), cc(760, 1, 10)};
  CHECK(ed.lane_replace(2, 0, 0, 768, old, 5) == Status::Ok);
  ed.history_reset();

  g_take.reset();
  g_take.begin(2, 0, 0, 0, 700, 768);
  CHECK(g_take.cc(720, 1, 60));
  CHECK(g_take.cc(10, 1, 70));  // after the wrap
  g_take.finish(30);
  CHECK(g_take.interval_count() == 2u);
  ed.begin_group(o3os::core::ActionId::None, 0, o3os::model::UndoArea::Pattern);
  CHECK(ed.lane_merge_take(g_take) == Status::Ok);
  ed.end_group();
  // Controller 1 moved from tick 720 to tick 10 across the wrap (A10): its old points
  // in [720, 768) and [0, 10] go (760 and 5), those before its first message in each
  // run stay (710 and 20); 720 and 10 come.
  const LaneEvent want[] = {cc(10, 1, 70), cc(20, 1, 8), note(300, 5, 50), cc(710, 1, 9), cc(720, 1, 60)};
  CHECK(lane_is(g_store, 2, 0, want, 5));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 2, 0, old, 5));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 2, 0, want, 5));
}

TEST_CASE("lane merge: a full lane or pool answers Full, changes nothing and the take can try again") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  static LaneEvent big[4096];
  for (std::uint32_t i = 0; i < 4096; ++i) big[i] = note(i, 1, static_cast<std::uint8_t>(i & 0x7F));
  CHECK(ed.lane_replace(1, 0, 0, 6144, big, 4096) == Status::Ok);
  ed.history_reset();

  g_take.reset();
  g_take.begin(1, 0, 0, 0, 5000, 6144);
  CHECK(g_take.note_on(5000, 60, 100));
  g_take.note_off(5010, 60);
  g_take.finish(5020);
  const std::uint32_t gen = g_arena.project.generation();
  CHECK(ed.lane_merge_take(g_take) == Status::Full);  // the lane would pass 4096
  CHECK(lane_is(g_store, 1, 0, big, 4096));
  CHECK(g_arena.project.generation() == gen);
  CHECK_FALSE(ed.can_undo());
  CHECK(g_take.count() == 1u);  // still there for one more try

  // pool full: another lane of 3584, then a take into a third lane
  CHECK(ed.lane_replace(1, 1, 0, 6144, big, 3584) == Status::Ok);
  ed.history_reset();
  g_take.begin(1, 2, 0, 0, 0, 6144);
  CHECK(g_take.note_on(1, 60, 100));
  g_take.finish(5);
  CHECK(ed.lane_merge_take(g_take) == Status::Full);
  CHECK(g_store.total_events() == 7680u);
  // room made: the same take commits
  CHECK(ed.lane_replace(1, 1, 0, 6144, big, 10) == Status::Ok);
  CHECK(ed.lane_merge_take(g_take) == Status::Ok);
  CHECK(g_store.lane(1, 2).count == 1u);
}

TEST_CASE("lane merge: a take that does not fit the undo ring is refused before any change") {
  Rig r(8 * 1024);  // a small ring: 8 KiB
  NEED(r.ok);
  Editor& ed = *r.ed;
  g_take.reset();
  g_take.begin(1, 0, 0, 0, 0, 6144);
  for (std::uint32_t i = 0; i < 1100; ++i) CHECK(g_take.note_on(i * 2, static_cast<std::uint8_t>(i % 100), 100));
  g_take.finish(2300);
  CHECK(ed.lane_merge_take(g_take) == Status::Full);  // 1100 x 8 bytes of record > 8 KiB
  CHECK(g_store.lane_count() == 0u);
  CHECK(g_take.count() == 1100u);
}

TEST_CASE("lane merge: refusals of a bad take") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  g_take.reset();
  g_take.begin(9, 0, 0, 0, 0, 768);  // no pattern 9
  CHECK(g_take.cc(1, 1, 1));
  g_take.finish(5);
  CHECK(ed.lane_merge_take(g_take) == Status::NotFound);
  g_take.begin(2, 0, 0, 0, 700, 768);  // pattern 2 is 768 ticks: fine; then a take made for a longer track is refused
  CHECK(g_take.cc(701, 1, 1));
  g_take.finish(710);
  CHECK(ed.set_track_length(2, 0, 8) == Status::Ok);  // 384 ticks now
  CHECK(ed.lane_merge_take(g_take) == Status::BadValue);
  CHECK(g_store.lane_count() == 0u);
  // a wrapped take (two intervals) whose events lie outside both after the track got shorter: refused the same way, nothing written
  g_take.begin(2, 0, 0, 0, 700, 768);
  CHECK(g_take.cc(701, 1, 1));
  g_take.finish(10);
  CHECK(g_take.interval_count() == 2u);
  CHECK(ed.lane_merge_take(g_take) == Status::BadValue);
  CHECK(g_store.lane_count() == 0u);
}

TEST_CASE("lane merge: a controller replaces the lane's old points only from its first to its last message of the take (A10)") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  // an 8-bar lane of controller 7 (a point every 700 ticks) and controller 8
  const LaneEvent old[] = {cc(100, 7, 1), cc(150, 8, 9), cc(800, 7, 2), cc(1500, 7, 3), cc(2200, 7, 4), cc(3000, 8, 8),
                           cc(3000, 7, 5), cc(4000, 7, 6), cc(5000, 7, 7), cc(5900, 7, 8), cc(6000, 7, 9)};
  CHECK(ed.lane_replace(1, 0, 0, 6144, old, 11) == Status::Ok);
  ed.history_reset();

  g_take.reset();
  g_take.begin(1, 0, 0, 0, 0, 6144);  // the take covers the whole loop ...
  CHECK(g_take.cc(5800, 7, 100));     // ... but controller 7 moved only at its end
  CHECK(g_take.cc(5950, 7, 120));
  CHECK(g_take.cc(100, 8, 50));  // controller 8 only at the start
  CHECK(g_take.cc(120, 8, 60));
  g_take.finish(6100);
  CHECK(g_take.replaces(7, 5800));
  CHECK(g_take.replaces(7, 5950));
  CHECK_FALSE(g_take.replaces(7, 5799));
  CHECK_FALSE(g_take.replaces(7, 5951));
  CHECK_FALSE(g_take.replaces(9, 5900));  // a controller that did not move

  CHECK(ed.lane_merge_take(g_take) == Status::Ok);
  // controller 7: only the point at 5900 was inside 5800..5950; controller 8: the point at 150 is outside 100..120, nothing goes
  const LaneEvent want[] = {cc(100, 7, 1),  cc(100, 8, 50), cc(120, 8, 60), cc(150, 8, 9),  cc(800, 7, 2),  cc(1500, 7, 3), cc(2200, 7, 4),
                            cc(3000, 8, 8), cc(3000, 7, 5), cc(4000, 7, 6), cc(5000, 7, 7), cc(5800, 7, 100), cc(5950, 7, 120), cc(6000, 7, 9)};
  CHECK(lane_is(g_store, 1, 0, want, 14));
}

TEST_CASE("lane take: a controller's replace range follows the loop when its messages wrap past the end of the track") {
  g_take.reset();
  g_take.begin(2, 0, 0, 0, 700, 768);
  CHECK(g_take.cc(750, 3, 10));
  CHECK(g_take.cc(10, 3, 20));  // passed the end of the track
  CHECK(g_take.cc(100, 4, 5));
  CHECK(g_take.cc(300, 5, 5));  // went round: its first and last are the same tick range reversed
  CHECK(g_take.cc(200, 5, 6));
  g_take.finish(400);
  CHECK(g_take.replaces(3, 760));
  CHECK(g_take.replaces(3, 5));
  CHECK(g_take.replaces(3, 10));
  CHECK_FALSE(g_take.replaces(3, 300));
  CHECK(g_take.replaces(4, 100));
  CHECK_FALSE(g_take.replaces(4, 101));
  CHECK_FALSE(g_take.replaces(4, 99));
  CHECK_FALSE(g_take.replaces(200, 1));
  CHECK(g_take.replaces(5, 300));  // 300 .. end of track, start of track .. 200
  CHECK(g_take.replaces(5, 200));
  CHECK_FALSE(g_take.replaces(5, 250));
}

TEST_CASE("lane take: the last point of a move is kept at an exactly full buffer (a slot is reserved for the pending point)") {
  g_take.reset();
  g_take.begin(1, 0, 0, 0, 0, 6144);
  for (std::uint32_t i = 0; i < LaneTake::kCapacity - 2; ++i) CHECK(g_take.note_on(i, static_cast<std::uint8_t>(i % 100), 100));
  CHECK(g_take.cc(3000, 5, 10));  // count is now capacity - 1
  CHECK(g_take.cc(3005, 5, 50));  // pending: takes the last slot
  CHECK_FALSE(g_take.full());
  CHECK_FALSE(g_take.note_on(3010, 60, 100));  // no slot left for a note
  CHECK(g_take.full());
  CHECK(g_take.cc(3006, 5, 60));  // the pending point moves on: no slot needed
  g_take.finish(3100);
  CHECK(g_take.count() == LaneTake::kCapacity);
  bool found = false;
  for (std::uint32_t i = 0; i < g_take.count(); ++i) {
    const LaneEvent& e = g_take.events()[i];
    if ((e.kind_channel >> 4) == 1 && e.a == 5 && e.tick == 3006) found = (e.b == 60);
  }
  CHECK(found);
}

TEST_CASE("lane take: finish sorts stably, also for the worst orders of a full buffer") {
  for (std::uint32_t mode = 0; mode < 3; ++mode) {
    g_take.reset();
    g_take.begin(1, 0, 0, 0, 0, 6144);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> want;  // (tick, a * 256 + b) in arrival order
    std::uint32_t lcg = 12345u;
    for (std::uint32_t i = 0; i < LaneTake::kCapacity; ++i) {
      std::uint32_t tick;
      if (mode == 0) {
        lcg = lcg * 1664525u + 1013904223u;
        tick = (lcg >> 8) % 6144u;  // random
      } else if (mode == 1) {
        tick = 6143u - (i * 3u);  // descending
      } else {
        tick = ((i % 2u) != 0u ? 3000u : 10u) + (i / 64u);  // two interleaved runs with many equal ticks
      }
      const std::uint8_t a = static_cast<std::uint8_t>(i % 128u);
      const std::uint8_t b = static_cast<std::uint8_t>(1u + (i / 128u) % 127u);
      CHECK(g_take.note_len(tick, a, b, 1));
      want.emplace_back(tick, static_cast<std::uint32_t>(a) * 256u + b);
    }
    std::stable_sort(want.begin(), want.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    g_take.finish(6143);
    CHECK(g_take.count() == LaneTake::kCapacity);
    bool same = g_take.count() == want.size();
    for (std::uint32_t i = 0; same && i < g_take.count(); ++i) {
      const LaneEvent& e = g_take.events()[i];
      same = e.tick == want[i].first && static_cast<std::uint32_t>(e.a) * 256u + e.b == want[i].second;
    }
    CHECK(same);
  }
}

TEST_CASE("lane take: a wrapped take whose last event is above the end tick (a message after the playhead's last read) raises the end to it, so no event falls outside the intervals and the take merges") {
  Rig r;
  NEED(r.ok);
  g_take.begin(1, 0, 0, 0, 4000, 6144);
  CHECK(g_take.note_on(4100, 60, 90));
  g_take.note_off(4200, 60);
  CHECK(g_take.note_on(100, 62, 90));  // went round
  g_take.note_off(150, 62);
  CHECK(g_take.cc(500, 7, 20));  // after the last position read (300), below the take's start
  g_take.finish(300);
  CHECK(g_take.interval_count() == 2u);
  CHECK(g_take.interval(1).t1 == 501u);  // the low run reaches the event
  CHECK(r.ed->lane_merge_take(g_take) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == 3u);
}
