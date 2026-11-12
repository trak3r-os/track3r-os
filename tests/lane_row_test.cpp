// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/app/song_build.h>
#include <o3os/app/song_edit.h>
#include <o3os/core/action.h>
#include <o3os/model/editor.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>
#include <o3os/model/undo.h>

#include <cstdint>
#include <cstring>
#include <new>

#include "support/need.h"

// OS-MIDIREC-P1 S7a: the row operations of a lane. A pattern copy carries its lanes (A16), the copy is one undo unit with the pattern (A15), a full pool or ring
// changes nothing, a deleted pattern frees its lanes, and CLEAR LANE is one undo unit.

namespace {

using o3os::model::Editor;
using o3os::model::LaneEvent;
using o3os::model::LaneStore;
using o3os::model::Status;
using o3os::model::UndoArea;
using o3os::model::UndoJournal;
using o3os::model::UndoResult;

o3os::model::ModelArena g_arena;
alignas(8) std::uint8_t g_ring[o3os::model::kUndoRegionBytes];
LaneStore g_store;

LaneEvent cc(std::uint32_t tick, std::uint8_t ctl, std::uint8_t v) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.kind_channel = static_cast<std::uint8_t>(1u << 4);
  e.a = ctl;
  e.b = v;
  return e;
}

bool lane_is(const LaneStore& s, std::uint32_t pattern, std::uint32_t track, const LaneEvent* e, std::uint32_t n) {
  const LaneStore::View v = s.lane(pattern, track);
  if (v.count != n) return false;
  return n == 0 || std::memcmp(v.events, e, n * sizeof(LaneEvent)) == 0;
}

// Patterns 1 (16 steps) and 2 (16 steps, tracks 1 and 2 shorter: 8 steps), an empty store attached to a new Editor.
struct Rig {
  UndoJournal journal;
  Editor* ed = nullptr;
  bool ok = false;
  alignas(Editor) unsigned char ed_mem[sizeof(Editor)];

  explicit Rig(std::size_t ring_bytes = 160 * 1024) {
    g_arena.project.reset();
    g_store.clear();
    g_store.adopt(g_arena.project.load_epoch());
    if (!journal.begin(g_ring, ring_bytes)) return;
    ed = new (ed_mem) Editor(g_arena.project, journal);
    ed->attach_lanes(&g_store);
    ok = ed->create_pattern(1, 16) == Status::Ok;
    ed->history_reset();
  }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;
  ~Rig() {
    if (ed != nullptr) ed->~Editor();
  }
};

const LaneEvent kLane0[] = {cc(5, 1, 7), cc(100, 1, 8), cc(700, 1, 9)};
const LaneEvent kLane2[] = {cc(10, 74, 20), cc(300, 74, 30)};

// Lanes on tracks 0 and 2 of pattern 1, one step of tick 48 each.
bool seed(Editor& ed) {
  if (ed.lane_replace(1, 0, 0, 768, kLane0, 3) != Status::Ok) return false;
  if (ed.lane_replace(1, 2, 0, 768, kLane2, 2) != Status::Ok) return false;
  ed.history_reset();
  return true;
}

}  // namespace

TEST_CASE("lane copy: DUP PAT carries the lanes of every track, the source is untouched, undo and redo restore both") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(seed(ed));
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  CHECK(o3os::app::song_copy_pattern(ed, 1, 2) == Status::Ok);
  ed.end_group();
  CHECK(lane_is(g_store, 2, 0, kLane0, 3));
  CHECK(lane_is(g_store, 2, 2, kLane2, 2));
  CHECK(g_store.lane(2, 1).count == 0u);
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));  // the source stays
  CHECK(lane_is(g_store, 1, 2, kLane2, 2));
  CHECK(g_store.lane_count() == 4u);

  CHECK(ed.undo() == UndoResult::Ok);  // one unit: the pattern and its lanes
  CHECK(g_arena.project.pattern(2) == nullptr);
  CHECK(g_store.lane_count() == 2u);
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
  CHECK(lane_is(g_store, 1, 2, kLane2, 2));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 2, 0, kLane0, 3));
  CHECK(lane_is(g_store, 2, 2, kLane2, 2));
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
}

TEST_CASE("lane copy: a track shorter in the source copies only the events below its end (A17)") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(ed.set_track_length(1, 0, 8) == Status::Ok);  // 8 steps = 384 ticks
  // set_track_length cut the lane's tail already when a lane existed; seed after it
  NEED(ed.lane_replace(1, 0, 0, 384, kLane0, 2) == Status::Ok);
  ed.history_reset();
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  CHECK(o3os::app::song_copy_pattern(ed, 1, 2) == Status::Ok);
  ed.end_group();
  CHECK(lane_is(g_store, 2, 0, kLane0, 2));
}

TEST_CASE("lane copy: the masked copy skips the cleared tracks and copies the others") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(seed(ed));
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  CHECK(o3os::app::song_copy_pattern_masked(ed, 1, 2, 0x0001u) == Status::Ok);  // track 0 stays empty
  ed.end_group();
  CHECK(g_store.lane(2, 0).count == 0u);
  CHECK(lane_is(g_store, 2, 2, kLane2, 2));
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(g_store.lane_count() == 2u);
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(g_store.lane(2, 0).count == 0u);
  CHECK(lane_is(g_store, 2, 2, kLane2, 2));
}

TEST_CASE("lane copy: a project without lanes copies as before and adds no journal bytes") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  CHECK(o3os::app::song_copy_pattern(ed, 1, 2) == Status::Ok);
  ed.end_group();
  CHECK(g_store.lane_count() == 0u);
  CHECK(g_store.total_events() == 0u);
  CHECK(g_arena.project.pattern(2) != nullptr);
}

TEST_CASE("lane copy: a full lane pool refuses the copy and changes nothing") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  static LaneEvent big[4096];
  for (std::uint32_t i = 0; i < 4096; ++i) big[i] = cc(i % 768, 1, static_cast<std::uint8_t>(i & 0x7F));
  // 16 steps = 768 ticks: sorted events, 3 lanes of 2500 do not fit the 7680 pool twice
  for (std::uint32_t i = 0; i < 2500; ++i) big[i] = cc(i * 768 / 2500, 1, static_cast<std::uint8_t>(i & 0x7F));
  NEED(ed.lane_replace(1, 0, 0, 768, big, 2500) == Status::Ok);
  NEED(ed.lane_replace(1, 1, 0, 768, big, 2500) == Status::Ok);
  ed.history_reset();
  const std::uint32_t gen = g_arena.project.generation();
  const std::uint32_t ev = g_store.total_events();
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  CHECK(o3os::app::song_copy_pattern(ed, 1, 2) == Status::Full);  // 5000 more events: 10000 > 7680
  ed.end_group();
  CHECK(g_arena.project.pattern(2) == nullptr);
  CHECK(g_store.total_events() == ev);
  CHECK(g_arena.project.generation() == gen);
  CHECK_FALSE(ed.can_undo());
  // masked: the lane of track 1 only (2500 events) fits
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  CHECK(o3os::app::song_copy_pattern_masked(ed, 1, 2, 0xFFFDu) == Status::Ok);
  ed.end_group();
  CHECK(g_store.lane(2, 1).count == 2500u);
  CHECK(g_store.lane(2, 0).count == 0u);
}

TEST_CASE("lane copy: a copy that does not fit the undo ring is refused before any change") {
  Rig r(8 * 1024);
  NEED(r.ok);
  Editor& ed = *r.ed;
  static LaneEvent many[1500];
  for (std::uint32_t i = 0; i < 1500; ++i) many[i] = cc(i * 768 / 1500, 1, static_cast<std::uint8_t>(i & 0x7F));
  NEED(ed.lane_replace(1, 0, 0, 768, many, 1500) == Status::Full);  // 1500 x 8 bytes of record > the 8 KiB ring
  // 16 lanes of 125 events: 1016 bytes of record each fit alone, 16 x 1016 = 16256 bytes in one unit do not
  static LaneEvent some[125];
  for (std::uint32_t i = 0; i < 125; ++i) some[i] = cc(i * 768 / 125, 1, static_cast<std::uint8_t>(i & 0x7F));
  for (std::uint32_t t = 0; t < 16; ++t) NEED(ed.lane_replace(1, t, 0, 768, some, 125) == Status::Ok);
  ed.history_reset();
  const std::uint32_t gen = g_arena.project.generation();
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  CHECK(o3os::app::song_copy_pattern(ed, 1, 2) == Status::Full);
  ed.end_group();
  CHECK(g_arena.project.pattern(2) == nullptr);
  CHECK(g_store.total_events() == 2000u);
  CHECK(g_arena.project.generation() == gen);
  CHECK_FALSE(ed.can_undo());
}

TEST_CASE("lane copy: lane_copy refuses a bad call and a non-empty target") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(seed(ed));
  NEED(ed.create_pattern(2, 16) == Status::Ok);
  ed.history_reset();
  CHECK(ed.lane_copy(1, 1, 0) == Status::BadValue);
  CHECK(ed.lane_copy(1, 2, 99) == Status::BadIndex);
  CHECK(ed.lane_copy(1, 77, 0) == Status::NotFound);
  CHECK(ed.lane_copy(1, 2, 1) == Status::Ok);  // nothing on track 1: nothing to copy
  CHECK(g_store.lane_count() == 2u);
  CHECK(ed.lane_copy(1, 2, 0) == Status::Ok);
  CHECK(ed.lane_copy(1, 2, 0) == Status::BadValue);  // the target holds a lane now
  CHECK(lane_is(g_store, 2, 0, kLane0, 3));
}

TEST_CASE("lane delete: deleting a pattern frees its lanes, undo brings them back") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(seed(ed));
  CHECK(ed.delete_pattern(1) == Status::Ok);
  CHECK(g_store.lane_count() == 0u);
  CHECK(g_store.total_events() == 0u);
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
  CHECK(lane_is(g_store, 1, 2, kLane2, 2));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(g_store.lane_count() == 0u);
}

TEST_CASE("lane clear: lane_clear empties one lane in one undo unit and undo restores it") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(seed(ed));
  CHECK(ed.lane_clear(1, 0) == Status::Ok);
  CHECK(g_store.lane(1, 0).count == 0u);
  CHECK(lane_is(g_store, 1, 2, kLane2, 2));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
  CHECK_FALSE(ed.can_undo());  // one unit
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(g_store.lane(1, 0).count == 0u);
}

namespace {

// A lane of 3 events on every track of pattern 1 (48 events), then pattern 5 takes the pool down to `spare` free events.
bool crowd(Editor& ed, std::uint32_t spare) {
  for (std::uint32_t t = 0; t < 16; ++t) {
    if (ed.lane_replace(1, t, 0, 768, kLane0, 3) != Status::Ok) return false;
  }
  if (ed.create_pattern(5, 16) != Status::Ok) return false;
  std::uint32_t left = o3os::model::kMaxLanePoolEvents - g_store.total_events() - spare;
  static LaneEvent buf[2544];
  for (std::uint32_t t = 0; left != 0u; ++t) {
    const std::uint32_t n = left < 2544u ? left : 2544u;
    for (std::uint32_t i = 0; i < n; ++i) buf[i] = cc(i * 768 / n, 1, static_cast<std::uint8_t>(i & 0x7F));
    if (ed.lane_replace(5, t, 0, 768, buf, n) != Status::Ok) return false;
    left -= n;
  }
  ed.history_reset();
  return g_store.total_events() + spare == o3os::model::kMaxLanePoolEvents;
}

}  // namespace

TEST_CASE("lane copy: UNIQUE plans the lane pool of its copies, a pool that cannot take them refuses before any copy") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(ed.insert_order(0, 1) == Status::Ok);
  NEED(ed.insert_order(1, 1) == Status::Ok);  // pattern 1 sits in two slots: UNIQUE on slot 0 copies it
  NEED(crowd(ed, 47));                        // the copy needs 48
  ed.history_reset();
  o3os::app::SongUnique u;
  o3os::app::song_unique_plan(g_arena.project, 0, 0, nullptr, u, &g_store);
  CHECK(u.no_room);
  CHECK(g_arena.project.order(0) == 1u);  // the plan wrote nothing
  CHECK_FALSE(ed.can_undo(UndoArea::Song));
  o3os::app::song_unique_plan(g_arena.project, 0, 0, nullptr, u, nullptr);  // without a store: the plan of before
  CHECK_FALSE(u.no_room);
  CHECK(u.copies == 1u);
}

TEST_CASE("lane copy: UNIQUE with room for the lanes copies them") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(ed.insert_order(0, 1) == Status::Ok);
  NEED(ed.insert_order(1, 1) == Status::Ok);
  NEED(crowd(ed, 48));
  ed.history_reset();
  o3os::app::SongUnique u;
  o3os::app::song_unique_plan(g_arena.project, 0, 0, nullptr, u, &g_store);
  CHECK_FALSE(u.no_room);
  CHECK(u.copies == 1u);
  ed.begin_group(o3os::core::ActionId::None, 0, UndoArea::Song);
  o3os::app::song_unique_apply(ed, 0, 0, nullptr, u);
  ed.end_group();
  CHECK(g_arena.project.order(0) != 1u);
  CHECK(g_arena.project.order(1) == 1u);
  CHECK(g_store.total_events() == o3os::model::kMaxLanePoolEvents);
}

TEST_CASE("lane copy: FORM plans the lane pool of its copies and refuses before any copy") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(ed.insert_order(0, 1) == Status::Ok);
  o3os::model::Step s = o3os::model::kEmptyStep;
  s.note = 60;
  s.instr = 1;
  for (std::uint32_t t = 0; t < 16; ++t) NEED(ed.set_step(1, t, 0, s) == Status::Ok);  // data on every group: every form slot with cleared groups is a copy
  NEED(crowd(ed, 1));
  const o3os::app::SongRect rect{0, 0, 0, 15};
  o3os::app::SongBuildPlan plan;
  o3os::app::song_form_plan(g_arena.project, rect, o3os::app::SongForm::Pop, 4, plan, &g_store);
  CHECK(plan.info.refusal == o3os::app::SongBuildRefusal::NoLanes);
  CHECK(plan.bytes == 0u);
  o3os::core::FixedString<48> text;
  o3os::app::song_build_text(o3os::app::SongBuildKind::Form, plan.info, text);
  CHECK(text.size() != 0u);
  o3os::app::song_form_plan(g_arena.project, rect, o3os::app::SongForm::Pop, 4, plan, nullptr);  // without a store: the plan of before
  CHECK(plan.info.refusal == o3os::app::SongBuildRefusal::None);
  CHECK(plan.info.copies != 0u);
}

// ---- OS-MIDIREC-P1 S7c: row insert and remove, and the dry runs (A16, A17) ----------------------------------------------------------------------------------------------------------

TEST_CASE("lane rows S7c: a row insert moves the lane ticks one step down, what passes the track length is deleted, undo and redo restore the lane") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  // 16 steps: tick 768 is the end. Events at steps 0, 2 and 15 (the last one is pushed out by an insert).
  const LaneEvent lane[] = {cc(5, 1, 1), cc(100, 1, 2), cc(15 * 48 + 7, 1, 3)};
  NEED(ed.lane_replace(1, 0, 0, 768, lane, 3) == Status::Ok);
  ed.history_reset();
  CHECK(ed.lane_rows(1, 0, 2, true) == Status::Ok);  // insert at step 2: ticks from 96 on move 48
  const LaneEvent after[] = {cc(5, 1, 1), cc(148, 1, 2)};
  CHECK(lane_is(g_store, 1, 0, after, 2));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, lane, 3));
  CHECK_FALSE(ed.can_undo());  // one unit
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, after, 2));
}

TEST_CASE("lane rows S7c: a row pull deletes the events of the step and moves the later ones one step up, undo restores them") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent lane[] = {cc(5, 1, 1), cc(2 * 48 + 3, 1, 2), cc(2 * 48 + 40, 1, 3), cc(5 * 48, 1, 4)};
  NEED(ed.lane_replace(1, 0, 0, 768, lane, 4) == Status::Ok);
  ed.history_reset();
  CHECK(ed.lane_rows(1, 0, 2, false) == Status::Ok);
  const LaneEvent after[] = {cc(5, 1, 1), cc(4 * 48, 1, 4)};
  CHECK(lane_is(g_store, 1, 0, after, 2));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, lane, 4));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, after, 2));
}

TEST_CASE("lane rows S7c: a step at or past the track length is refused and a lane before the step stays") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(seed(ed));
  CHECK(ed.lane_rows(1, 0, 16, true) == Status::BadValue);
  CHECK(ed.lane_rows(1, 0, 40, false) == Status::BadValue);
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
  CHECK(ed.lane_rows(1, 0, 15, true) == Status::Ok);  // nothing at or after step 15 (tick 720) on track 0
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
}

TEST_CASE("lane rows S7c: lanes_replace_fit and lanes_rows_fit change nothing and say no when the ring cannot take the unit") {
  Rig r(8 * 1024);
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent lane[] = {cc(5, 1, 1), cc(100, 1, 2)};
  NEED(ed.lane_replace(1, 0, 0, 768, lane, 2) == Status::Ok);
  ed.history_reset();
  const Editor::LaneSpan span{0, 0, 768, 0};
  CHECK(ed.lanes_replace_fit(1, &span, 1, 0));
  CHECK_FALSE(ed.lanes_replace_fit(1, &span, 1, 1024 * 1024));  // other records that would not fit
  CHECK(ed.lanes_rows_fit(1, 1, 0, true, 0));
  CHECK_FALSE(ed.lanes_rows_fit(1, 1, 0, true, 1024 * 1024));
  CHECK(lane_is(g_store, 1, 0, lane, 2));
  CHECK_FALSE(ed.can_undo());
}

// ---- OS-MIDIREC-P1 S7d: EXPAND and SHRINK scale the lane ticks, TRANSPOSE moves the lane notes (A16, A17) ----------------------------------------------------------------------------------

namespace {
LaneEvent nt(std::uint32_t tick, std::uint32_t len, std::uint8_t note, std::uint8_t vel) {
  LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.len = static_cast<std::uint16_t>(len);
  e.kind_channel = 0;
  e.a = note;
  e.b = vel;
  return e;
}
}  // namespace

TEST_CASE("lane scale S7d: EXPAND doubles ticks and note lengths, SHRINK halves them, each one undo unit that restores the lane exactly") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent lane[] = {nt(5, 20, 60, 100), cc(100, 1, 2), cc(700, 1, 3)};
  NEED(ed.lane_replace(1, 0, 0, 768, lane, 3) == Status::Ok);
  ed.history_reset();
  CHECK(ed.lane_scale(1, 0, 32, true) == Status::Ok);  // EXPAND 16 -> 32 steps (the caller then sets the length)
  const LaneEvent big[] = {nt(10, 40, 60, 100), cc(200, 1, 2), cc(1400, 1, 3)};
  CHECK(lane_is(g_store, 1, 0, big, 3));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, lane, 3));
  CHECK_FALSE(ed.can_undo());
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, big, 3));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(ed.lane_scale(1, 0, 8, false) == Status::Ok);  // SHRINK 16 -> 8 steps: (L+1)/2 for a length, never zero
  const LaneEvent small[] = {nt(2, 10, 60, 100), cc(50, 1, 2), cc(350, 1, 3)};
  CHECK(lane_is(g_store, 1, 0, small, 3));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, lane, 3));
}

TEST_CASE("lane scale S7d: a scaled event that passes the new end is deleted (A17) and undo brings it back; the other lanes and an empty lane are left alone") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  NEED(seed(ed));
  CHECK(ed.lane_scale(1, 0, 4, false) == Status::Ok);  // new end 192: ticks 2, 50 stay, 350 goes
  const LaneEvent kept[] = {cc(2, 1, 7), cc(50, 1, 8)};
  CHECK(lane_is(g_store, 1, 0, kept, 2));
  CHECK(lane_is(g_store, 1, 2, kLane2, 2));
  CHECK(ed.lane_scale(1, 1, 8, false) == Status::Ok);  // no lane on track 1
  CHECK(ed.undo() == UndoResult::Ok);                   // (the empty lane recorded nothing: this undoes the first shrink)
  CHECK(lane_is(g_store, 1, 0, kLane0, 3));
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, kept, 2));
  CHECK(ed.lane_scale(1, 16, 8, false) == Status::BadIndex);
  CHECK(ed.lane_scale(9, 0, 8, false) == Status::NotFound);
}

TEST_CASE("lane scale S7d: a ring that cannot take the lane's records refuses whole (Full), the dry run says the same and nothing changes") {
  Rig r(8 * 1024);
  NEED(r.ok);
  Editor& ed = *r.ed;
  LaneEvent lane[600];
  for (std::uint32_t i = 0; i < 600; ++i) lane[i] = cc(i * 767u / 600u, 1, static_cast<std::uint8_t>(i & 127u));
  NEED(ed.lane_replace(1, 0, 0, 768, lane, 600) == Status::Ok);  // one 4.8 KiB record fits, the before and after of a scale (9.6 KiB) do not
  ed.history_reset();
  CHECK_FALSE(ed.lanes_scale_fit(1, 1, 0));
  CHECK(ed.lanes_scale_fit(1, 2, 0));  // track 1 has no lane: nothing to record
  CHECK(ed.lane_scale(1, 0, 32, true) == Status::Full);
  CHECK(lane_is(g_store, 1, 0, lane, 600));
  CHECK_FALSE(ed.can_undo());
}

TEST_CASE("lane transpose S7d: the notes of the range move together (a chord stays a chord) and clamp to 0..127, CC and the notes outside the range stay, undo restores") {
  Rig r;
  NEED(r.ok);
  Editor& ed = *r.ed;
  const LaneEvent lane[] = {nt(10, 30, 60, 90), nt(10, 30, 64, 90), nt(10, 30, 67, 90), cc(20, 1, 55), nt(300, 40, 126, 80), nt(500, 40, 2, 80)};
  NEED(ed.lane_replace(1, 0, 0, 768, lane, 6) == Status::Ok);
  ed.history_reset();
  CHECK(ed.lanes_transpose_fit(1, 1, 0, 192, 3, 0));
  CHECK_FALSE(ed.lanes_transpose_fit(1, 1, 0, 192, 3, 1024 * 1024));
  CHECK(ed.lane_transpose(1, 0, 0, 192, 3) == Status::Ok);
  const LaneEvent chord[] = {nt(10, 30, 63, 90), nt(10, 30, 67, 90), nt(10, 30, 70, 90), cc(20, 1, 55), nt(300, 40, 126, 80), nt(500, 40, 2, 80)};
  CHECK(lane_is(g_store, 1, 0, chord, 6));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(lane_is(g_store, 1, 0, lane, 6));
  CHECK_FALSE(ed.can_undo());
  CHECK(ed.lane_transpose(1, 0, 0, 768, 12) == Status::Ok);  // 126 clamps to 127; the CC does not move
  const LaneEvent up[] = {nt(10, 30, 72, 90), nt(10, 30, 76, 90), nt(10, 30, 79, 90), cc(20, 1, 55), nt(300, 40, 127, 80), nt(500, 40, 14, 80)};
  CHECK(lane_is(g_store, 1, 0, up, 6));
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(ed.lane_transpose(1, 0, 0, 768, -4) == Status::Ok);
  CHECK(g_store.lane(1, 0).events[5].a == 0);  // 2 - 4 clamps to 0
  CHECK(ed.lane_transpose(1, 0, 0, 768, 0) == Status::Ok);  // nothing to move records nothing
  CHECK(ed.lane_transpose(1, 0, 5, 5, 1) == Status::BadValue);
  CHECK(ed.lane_transpose(1, 16, 0, 768, 1) == Status::BadIndex);
}
