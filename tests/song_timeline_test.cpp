// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG-TIMELINE (docs/specs/OS-SONG-VIEW.md; new Community OS behaviour): the pure logic of the Song screen's TIMELINE page: the window of slots in view, the scaling of the blocks to bars,
// and the cache of the note densities of the thumbnails (a scan once per pattern generation).
#include <doctest/doctest.h>

#include <o3os/app/model.h>
#include <o3os/app/song_map.h>
#include <o3os/app/song_source.h>
#include <o3os/app/song_timeline.h>
#include <o3os/model/project.h>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;

namespace {

// A project whose patterns 1..`n` are `steps` steps long, and a song of `slots` slots over them (slot i holds pattern 1 + i % n).
struct TimelineRig {
  app::Model model;
  TimelineRig(unsigned slots, unsigned steps, unsigned n = 4) {
    model::Project& p = test::arena(1).project;
    p.reset();
    for (unsigned id = 1; id <= n; ++id) {
      NEED(p.create_pattern(id, steps) == model::Status::Ok);
    }
    for (unsigned i = 0; i < slots; ++i) {
      NEED(p.append_order(1 + i % n) == model::Status::Ok);
    }
    model.project = &p;
  }
  model::Project& project() { return test::arena(1).project; }  // (Model holds it const)
};

}  // namespace

TEST_CASE("timeline: a slot takes ceil(steps / 16) bars, at least one") {
  CHECK(app::timeline_bars(0) == 1u);
  CHECK(app::timeline_bars(16) == 1u);
  CHECK(app::timeline_bars(17) == 2u);
  CHECK(app::timeline_bars(64) == 4u);
  CHECK(app::timeline_bars(128) == 8u);
}

TEST_CASE("timeline: the window starts four slots before the cursor and holds the cursor's slot") {
  TimelineRig rig(60, 16);
  const app::SongSource src(rig.model);
  app::TimelineWindow w = app::timeline_window(src, 0, 8);
  CHECK((w.first == 0 && w.count > 0u));
  w = app::timeline_window(src, 3, 8);
  CHECK(w.first == 0);  // fewer than four before it: from the start
  w = app::timeline_window(src, 10, 8);
  CHECK(w.first == 6);
  w = app::timeline_window(src, 59, 8);
  CHECK((w.first == 55 && w.count == 5u));  // the last slots: the window ends with the song
  w = app::timeline_window(src, 400, 8);
  CHECK(w.first == 55);  // a cursor past the end counts as the last slot
  w = app::timeline_window(src, -5, 8);
  CHECK(w.first == 0);
}

TEST_CASE("timeline: at most 40 slots, 48 bars and the slot budget") {
  {
    TimelineRig rig(100, 16);  // one-bar slots
    const app::SongSource src(rig.model);
    CHECK(app::timeline_window(src, 20, 4).count == 40u);   // the slot limit
    CHECK(app::timeline_window(src, 20, 0).count == 40u);   // no tracks: no thumbnails, the slot limit still holds
    CHECK(app::timeline_window(src, 20, 9).count == 40u);   // 40 x (9 + 6) = 600
    CHECK(app::timeline_window(src, 20, 12).count == 34u);  // 34 x 18 = 612
    CHECK(app::timeline_window(src, 20, 16).count == 28u);  // 28 x 22 = 616
    CHECK(app::timeline_window(src, 20, 15).count == 29u);  // 29 x 21 = 609
  }
  {
    TimelineRig rig(20, 128);  // eight-bar slots
    const app::SongSource src(rig.model);
    const app::TimelineWindow w = app::timeline_window(src, 5, 4);
    CHECK(w.count == 6u);  // 6 x 8 = 48 bars, a seventh would be 56
    CHECK(w.start[w.count] == 48u);
    CHECK((w.first == 1 && 5 < w.first + static_cast<int>(w.count)));  // the cursor is inside
    // whatever the cursor, its slot is in the window (4 before it at 8 bars is 32, its own 8: 40 of 48)
    for (int c = 0; c < 20; ++c) {
      const app::TimelineWindow v = app::timeline_window(src, c, 16);
      CHECK((c >= v.first && c < v.first + static_cast<int>(v.count)));
    }
  }
}

TEST_CASE("timeline: blocks are scaled to bars, the page at least 16 bars wide") {
  {
    TimelineRig rig(3, 16);  // three one-bar slots on a page of 16 bars: they do not stretch over the whole width
    const app::SongSource src(rig.model);
    const app::TimelineWindow w = app::timeline_window(src, 0, 8);
    CHECK((w.count == 3u && w.den == 16u));
    CHECK((w.start[0] == 0u && w.start[1] == 1u && w.start[2] == 2u && w.start[3] == 3u));
    CHECK(app::timeline_x(w, w.start[0], 800) == 0);
    CHECK(app::timeline_x(w, w.start[1], 800) == 50);
    CHECK(app::timeline_x(w, w.start[3], 800) == 150);
  }
  {
    TimelineRig rig(10, 64);  // 40 bars of four-bar slots: the whole width
    const app::SongSource src(rig.model);
    const app::TimelineWindow w = app::timeline_window(src, 0, 8);
    CHECK((w.count == 10u && w.den == 40u));
    CHECK(app::timeline_x(w, w.start[w.count], 750) == 750);  // the last block ends at the edge
    int prev = -1;
    for (unsigned i = 0; i <= w.count; ++i) {  // monotone
      const int x = app::timeline_x(w, w.start[i], 750);
      CHECK(x > prev);
      prev = x;
    }
    CHECK(app::timeline_x(w, w.start[1], 750) == 75);  // four bars of forty
  }
  {
    TimelineRig rig(0, 16);
    const app::SongSource src(rig.model);
    CHECK(app::timeline_window(src, 0, 8).count == 0u);  // an empty list: nothing to show
  }
  app::TimelineWindow w;
  CHECK(app::timeline_x(w, 5, 0) == 0);  // no width: no pixel
}

TEST_CASE("timeline: note density levels, 0 for nothing and full at a note on every other step") {
  CHECK(app::SongDensity::level(0, 16) == 0u);
  CHECK(app::SongDensity::level(1, 0) == 0u);
  CHECK(app::SongDensity::level(1, 128) == 1u);  // one note in 128 steps still shows
  CHECK(app::SongDensity::level(1, 16) == 2u);
  CHECK(app::SongDensity::level(4, 16) == 8u);
  CHECK(app::SongDensity::level(8, 16) == 15u);
  CHECK(app::SongDensity::level(16, 16) == 15u);  // capped
  CHECK(app::SongDensity::level(128, 128) == 15u);
}

TEST_CASE("timeline: the densities of a pattern are counted per track over the track's own length") {
  TimelineRig rig(1, 16, 1);
  model::Project& p = rig.project();
  model::Step note = model::kEmptyStep;
  note.note = 61;
  for (unsigned s = 0; s < 16; ++s) {
    NEED(p.set_step(1, 0, s, note) == model::Status::Ok);  // track 0: every step
  }
  for (unsigned s = 0; s < 16; s += 4) {
    NEED(p.set_step(1, 3, s, note) == model::Status::Ok);  // track 3: 4 of 16
  }
  model::Step fx = model::kEmptyStep;
  fx.fx[0] = {12, 5};
  NEED(p.set_step(1, 4, 0, fx) == model::Status::Ok);  // track 4: FX only, no note
  NEED(p.set_track_length(1, 5, 8) == model::Status::Ok);
  NEED(p.set_step(1, 5, 12, note) == model::Status::Ok);  // track 5: a note past its length does not count
  const std::uint64_t d = app::SongDensity::compute(*p.pattern(1));
  CHECK(app::SongDensity::nibble(d, 0) == 15u);
  CHECK(app::SongDensity::nibble(d, 1) == 0u);
  CHECK(app::SongDensity::nibble(d, 3) == 8u);
  CHECK(app::SongDensity::nibble(d, 4) == 0u);
  CHECK(app::SongDensity::nibble(d, 5) == 0u);
  CHECK(app::SongDensity::nibble(d, 99) == 0u);
}

TEST_CASE("timeline: the density cache scans a pattern once per generation, never per frame") {
  TimelineRig rig(4, 16, 3);
  model::Project& p = rig.project();
  const app::SongDensity cache;
  CHECK(cache.computed() == 0u);
  for (int frame = 0; frame < 20; ++frame) {  // twenty frames over the three patterns of the song
    for (unsigned id = 1; id <= 3; ++id) {
      (void)cache.get(p, id);
    }
  }
  CHECK(cache.computed() == 3u);
  model::Step note = model::kEmptyStep;
  note.note = 61;
  NEED(p.set_step(2, 7, 1, note) == model::Status::Ok);
  CHECK(app::SongDensity::nibble(cache.get(p, 2), 7) > 0u);  // pattern 2 changed: scanned again, and the new note shows
  CHECK(cache.computed() == 4u);
  CHECK(app::SongDensity::nibble(cache.get(p, 1), 7) == 0u);
  CHECK(cache.computed() == 4u);  // the others were not touched
  (void)cache.get(p, 2);
  CHECK(cache.computed() == 4u);
  CHECK(cache.get(p, 9) == 0u);  // a pattern that does not exist: nothing, and no scan
  CHECK(cache.get(p, 0) == 0u);
  CHECK(cache.get(p, 300) == 0u);
  CHECK(cache.computed() == 4u);
  // another project is never answered from this one's cache
  model::Project& other = test::arena(2).project;
  other.reset();
  NEED(other.create_pattern(2, 16) == model::Status::Ok);
  CHECK(cache.get(other, 2) == 0u);
  CHECK(cache.computed() == 5u);
}

// ---- the energy curve ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("timeline: loudness is the mean step volume scaled to 0..15, an unset volume the loudest, and only notes count") {
  CHECK(app::SongDensity::loud(0, 0) == 0u);  // no notes
  CHECK(app::SongDensity::loud(127 * 4, 4) == 15u);
  CHECK(app::SongDensity::loud(0, 4) == 0u);  // notes at volume 0
  CHECK(app::SongDensity::loud(64 * 2, 2) == 8u);  // about half
  TimelineRig rig(1, 16, 1);
  model::Project& p = rig.project();
  model::Step note = model::kEmptyStep;
  note.note = 61;  // volume unset (255): the loudest
  NEED(p.set_step(1, 0, 0, note) == model::Status::Ok);
  note.vol = 64;
  NEED(p.set_step(1, 1, 0, note) == model::Status::Ok);
  note.vol = 0;
  NEED(p.set_step(1, 2, 0, note) == model::Status::Ok);
  model::Step cut = model::kEmptyStep;
  cut.note = model::kNoteCut;
  cut.vol = 127;
  NEED(p.set_step(1, 3, 0, cut) == model::Status::Ok);  // a cut is not a note: no density, no loudness
  std::uint64_t dens = 0;
  std::uint64_t loud = 0;
  app::SongDensity::scan(*p.pattern(1), dens, loud);
  CHECK(app::SongDensity::nibble(loud, 0) == 15u);
  CHECK(app::SongDensity::nibble(loud, 1) == 8u);
  CHECK(app::SongDensity::nibble(loud, 2) == 0u);
  CHECK(app::SongDensity::nibble(dens, 2) > 0u);  // (a note at volume 0 is still a note)
  CHECK(app::SongDensity::nibble(dens, 3) == 0u);
  CHECK(app::SongDensity::nibble(loud, 3) == 0u);
}

TEST_CASE("timeline: the loudness comes from the same cached scan as the density") {
  TimelineRig rig(2, 16, 2);
  model::Project& p = rig.project();
  const app::SongDensity cache;
  model::Step note = model::kEmptyStep;
  note.note = 61;
  note.vol = 100;
  NEED(p.set_step(1, 0, 0, note) == model::Status::Ok);
  for (int frame = 0; frame < 5; ++frame) {
    for (unsigned id = 1; id <= 2; ++id) {
      (void)cache.get(p, id);
      (void)cache.loudness(p, id);
    }
  }
  CHECK(cache.computed() == 2u);  // one scan a pattern for both
  CHECK(app::SongDensity::nibble(cache.loudness(p, 1), 0) == 12u);
  CHECK(cache.loudness(p, 9) == 0u);
  model::Step louder = note;
  louder.vol = 127;
  NEED(p.set_step(1, 0, 0, louder) == model::Status::Ok);
  CHECK(app::SongDensity::nibble(cache.loudness(p, 1), 0) == 15u);  // an edit shows
  CHECK(cache.computed() == 3u);
}

TEST_CASE("timeline: a slot's energy is density x loudness x level summed over the tracks; muted and unsoloed tracks count nothing") {
  std::uint8_t level[model::kMaxTracks] = {};
  level[0] = 100;
  level[1] = 50;
  const std::uint64_t dens = 0x0A0Full;   // track 0: 15, track 1: 0, track 2: 10
  const std::uint64_t loud = 0x08F8ull;   // track 0: 8, track 1: 15, track 2: 8
  CHECK(app::timeline_energy(dens, loud, level) == 12000u);  // track 0: 15 x 8 x 100; track 1: 0; track 2: level 0
  level[2] = 10;
  CHECK(app::timeline_energy(dens, loud, level) == 12000u + 10u * 8u * 10u);
  const std::uint8_t full[model::kMaxTracks] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100};
  CHECK(app::timeline_energy(~0ull, ~0ull, full) == 360000u);  // the biggest there is
  CHECK(app::timeline_energy(0, 0, full) == 0u);

  TimelineRig rig(1, 16, 1);
  model::Project& p = rig.project();
  NEED(p.set_active_tracks(4) == model::Status::Ok);
  std::uint8_t got[model::kMaxTracks];
  app::timeline_levels(p, 4, got);
  const unsigned volume0 = p.track(0)->volume;
  CHECK((got[0] == volume0 && got[3] == p.track(3)->volume && got[4] == 0u));  // tracks past the count stay 0
  model::TrackSlot t1 = *p.track(1);
  t1.flags = model::kTrackMute;
  NEED(p.set_track(1, t1) == model::Status::Ok);
  app::timeline_levels(p, 4, got);
  CHECK((got[1] == 0u && got[0] == volume0));  // muted: nothing
  model::TrackSlot t2 = *p.track(2);
  t2.flags = model::kTrackSolo;
  NEED(p.set_track(2, t2) == model::Status::Ok);
  app::timeline_levels(p, 4, got);
  CHECK((got[2] == p.track(2)->volume && got[0] == 0u && got[1] == 0u && got[3] == 0u));  // a solo: only the soloed
}

TEST_CASE("timeline: the curve's heights are in proportion to the biggest slot in view") {
  CHECK(app::timeline_energy_height(0, 100, 48) == 0);
  CHECK(app::timeline_energy_height(100, 100, 48) == 48);
  CHECK(app::timeline_energy_height(50, 100, 48) == 24);
  CHECK(app::timeline_energy_height(1, 360000, 48) == 1);  // anything shows
  CHECK(app::timeline_energy_height(10, 0, 48) == 0);      // a window of nothing
  CHECK(app::timeline_energy_height(10, 5, 48) == 48);     // never taller than the strip
  CHECK(app::timeline_energy_height(360000, 360000, 48) == 48);
  CHECK(app::timeline_energy_height(5, 10, 0) == 0);
}

// The structural display-list bound (`song/timeline_worst` measures it, 1 to 16 tracks): for every track count the slots in view cost at most kTimelineSlotBudget rectangle-equivalents.
TEST_CASE("timeline: slots x (tracks + overhead) stays within the budget for every track count") {
  static_assert(app::timeline_slot_cap(0) == app::kTimelineMaxSlots, "no tracks: the slot limit");
  unsigned worst_cost = 0;
  for (unsigned tracks = 0; tracks <= model::kMaxTracks; ++tracks) {
    const unsigned slots = app::timeline_slot_cap(tracks);
    const unsigned cost = slots * (tracks + app::kTimelineSlotOverhead);
    CHECK(slots <= app::kTimelineMaxSlots);
    CHECK(cost <= app::kTimelineSlotBudget);
    CHECK(slots >= 5u);  // the cursor's slot and the four before it always fit
    // no slot is wasted: one more would break the budget, unless the slot limit holds
    CHECK((slots == app::kTimelineMaxSlots || (slots + 1u) * (tracks + app::kTimelineSlotOverhead) > app::kTimelineSlotBudget));
    worst_cost = cost > worst_cost ? cost : worst_cost;
    TimelineRig rig(100, 16);
    const app::SongSource src(rig.model);
    CHECK(app::timeline_window(src, 50, tracks).count == slots);
  }
  // the words of the dearest page: the rest of the page (about 400, measured) + 2 for each track's lane label + 2 for each rectangle and 12 for each slot's overhead (2 x the budget): the chip's 1792
  // with 100 spare (the measured worst is 1650)
  CHECK(400u + 2u * model::kMaxTracks + 2u * worst_cost <= 1792u - 100u);
}
