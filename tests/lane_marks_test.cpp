// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC-P1 S4: what the Pattern grid says about a lane, step by step (compute_lane_marks).
#include <doctest/doctest.h>

#include <o3os/app/lane_marks.h>
#include <o3os/model/lane_store.h>

#include <cstdint>
#include <initializer_list>
#include <memory>

namespace {

using o3os::app::compute_lane_marks;
using o3os::app::LaneMark;
using o3os::model::LaneEvent;
using o3os::model::LaneStore;

constexpr std::uint16_t kTps = o3os::model::kLaneTicksPerStep;

LaneEvent note(unsigned step, unsigned tick_in_step, unsigned len_ticks) {
  return {static_cast<std::uint16_t>(step * kTps + tick_in_step), static_cast<std::uint16_t>(len_ticks), 0x00, 0, 60, 100};
}
LaneEvent cc(unsigned step, unsigned tick_in_step) {
  return {static_cast<std::uint16_t>(step * kTps + tick_in_step), 0, 0x10, 0, 1, 64};
}

struct Fixture {
  std::unique_ptr<LaneStore> store = std::make_unique<LaneStore>();
  explicit Fixture(std::initializer_list<LaneEvent> events) {
    LaneStore::WriteWindow window(*store);
    LaneEvent* gap = store->open_gap(1, 0, 0, static_cast<std::uint32_t>(events.size()));
    CHECK(gap != nullptr);
    if (gap == nullptr) {
      return;
    }
    for (const LaneEvent& e : events) {
      *gap++ = e;
    }
  }
  LaneStore::View view() const { return store->lane(1, 0); }
};

}  // namespace

TEST_CASE("lane marks: an empty lane marks nothing") {
  LaneMark m[16];
  compute_lane_marks({nullptr, 0}, 0, 16, 64, m);
  for (const LaneMark& x : m) {
    CHECK_FALSE(x.any());
  }
}

TEST_CASE("lane marks: starts, held, CC and none") {
  // A note of 3 steps from step 2, a CC on step 3 (inside the note: the step is "held" and has a CC), a CC alone on step 8.
  Fixture f({note(2, 0, 3 * kTps), cc(3, 5), cc(8, 0)});
  LaneMark m[16];
  compute_lane_marks(f.view(), 0, 16, 64, m);
  CHECK(m[1].any() == false);
  CHECK((m[2].starts == 1 && !m[2].held && m[2].ccs == 0));
  CHECK((m[3].starts == 0 && m[3].held && m[3].ccs == 1));
  CHECK((m[4].starts == 0 && m[4].held && m[4].ccs == 0));
  CHECK_FALSE(m[5].any());  // the note ends with step 4: its last tick is the last of that step
  CHECK((m[8].starts == 0 && !m[8].held && m[8].ccs == 1));
  CHECK_FALSE(m[9].any());  // a CC has no length: the step after it is "none"
}

TEST_CASE("lane marks: a note that ends at the step's edge does not hold the next step") {
  Fixture f({note(0, 0, kTps)});
  LaneMark m[4];
  compute_lane_marks(f.view(), 0, 4, 64, m);
  CHECK(m[0].starts == 1);
  CHECK_FALSE(m[1].any());
}

TEST_CASE("lane marks: starts wins over held, counts add up") {
  Fixture f({note(0, 0, 3 * kTps), note(1, 0, kTps), note(1, 10, 10), cc(1, 1), cc(1, 2)});
  LaneMark m[4];
  compute_lane_marks(f.view(), 0, 4, 64, m);
  CHECK((m[1].starts == 2 && m[1].ccs == 2 && !m[1].held));
  CHECK((m[2].held && m[2].starts == 0));
}

TEST_CASE("lane marks: a note from above the window holds the steps it still sounds in") {
  Fixture f({note(2, 0, 10 * kTps)});  // steps 2..11
  LaneMark m[8];
  compute_lane_marks(f.view(), 6, 8, 64, m);  // window: steps 6..13
  for (int i = 0; i < 6; ++i) {
    CHECK_MESSAGE(m[i].held, "row " << 6 + i);
    CHECK(m[i].starts == 0);
  }
  CHECK_FALSE(m[6].any());  // step 12
  CHECK_FALSE(m[7].any());
}

TEST_CASE("lane marks: nothing at or past the track's length, nothing above the window's first row") {
  Fixture f({note(6, 0, 4 * kTps), cc(7, 0), cc(9, 0)});
  LaneMark m[8];
  compute_lane_marks(f.view(), 4, 8, 8, m);  // rows 4..11 of a track of 8 steps: rows 8.. are blank slots
  CHECK((m[2].starts == 1 && m[3].ccs == 1));
  CHECK(m[3].held);
  CHECK_FALSE(m[4].any());  // row 8: the track ends before it, the note's tail is cut
  CHECK_FALSE(m[5].any());  // row 9: the CC is past the length
  CHECK_FALSE(m[7].any());
  // A negative first row (the window's top is above the pattern): those rows stay empty.
  compute_lane_marks(f.view(), -3, 8, 8, m);
  CHECK_FALSE(m[0].any());
  CHECK_FALSE(m[2].any());
  CHECK_FALSE(m[7].any());  // row 4: the note starts at row 6, past this window
}

TEST_CASE("lane marks: the counts saturate") {
  LaneEvent ev[300];
  unsigned n = 0;
  auto store = std::make_unique<LaneStore>();
  {
    LaneStore::WriteWindow window(*store);
    for (unsigned i = 0; i < 300; ++i) {
      ev[n++] = cc(0, i % 40);
    }
    // the lane's tick order: sort by tick (insertion of a few equal ticks keeps it valid)
    for (unsigned i = 1; i < n; ++i) {
      for (unsigned j = i; j > 0 && ev[j - 1].tick > ev[j].tick; --j) {
        const LaneEvent t = ev[j];
        ev[j] = ev[j - 1];
        ev[j - 1] = t;
      }
    }
    LaneEvent* gap = store->open_gap(1, 0, 0, n);
    CHECK(gap != nullptr);
    for (unsigned i = 0; gap != nullptr && i < n; ++i) {
      gap[i] = ev[i];
    }
  }
  LaneMark m[2];
  compute_lane_marks(store->lane(1, 0), 0, 2, 64, m);
  CHECK(m[0].ccs == 255);
}
