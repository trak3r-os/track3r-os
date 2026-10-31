// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SONG-VIEW2 V4 (docs/specs/OS-SONG-VIEW2.md; new Community OS behaviour): the note map of the Song screen's TIMELINE page: the bits of a pattern's picture (a step with a note sets its bit, empty steps,
// steps past a track's length and tracks past 12 clear it), the RAM_G address and upload of each picture, the cache (one upload per pattern generation, a new one after every pattern writer of the model,
// another project or display starts again, a refused upload is tried again) and the scaling arithmetic that puts a picture over a block.
#include <doctest/doctest.h>

#include <o3os/app/song_notemap.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <cstring>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::SongNoteMap;
using platform::host::FakeDisplay;

namespace {

model::Step note_step(std::uint8_t note) {
  model::Step s = model::kEmptyStep;
  s.note = note;
  return s;
}

// A fresh project with patterns 1 and 2 of 16 steps.
model::Project& fresh() {
  model::Project& p = test::arena(1).project;
  p.reset();
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  return p;
}

}  // namespace

TEST_CASE("song notemap: a step with a note sets its bit, in the track's even row, MSB leftmost") {
  model::Project& p = fresh();
  NEED(p.set_step(1, 0, 0, note_step(25)) == model::Status::Ok);
  NEED(p.set_step(1, 0, 9, note_step(128)) == model::Status::Ok);
  NEED(p.set_pattern_length(1, 128) == model::Status::Ok);
  NEED(p.set_step(1, 3, 127, note_step(1)) == model::Status::Ok);
  NEED(p.set_step(1, 11, 64, note_step(60)) == model::Status::Ok);
  std::uint8_t bits[SongNoteMap::kBytes];
  SongNoteMap::render(*p.pattern(1), bits);
  CHECK(SongNoteMap::tick(bits, 0, 0));
  CHECK(SongNoteMap::tick(bits, 0, 9));
  CHECK(SongNoteMap::tick(bits, 3, 127));
  CHECK(SongNoteMap::tick(bits, 11, 64));
  CHECK(bits[0] == 0x80);  // track 0, step 0: the leftmost bit of the first byte of row 0
  CHECK(bits[1] == 0x40);  // step 9: the second bit of the second byte
  CHECK(bits[6 * SongNoteMap::kStride + 15] == 0x01);  // track 3 is row 6; step 127 is the last bit of the row
  CHECK(bits[22 * SongNoteMap::kStride + 8] == 0x80);  // track 11 is row 22; step 64 is the first bit of byte 8
  unsigned set = 0;
  for (unsigned b = 0; b < SongNoteMap::kBytes; ++b) {
    for (unsigned k = 0; k < 8; ++k) {
      set += (bits[b] >> k) & 1u;
    }
  }
  CHECK(set == 4u);  // nothing else
  CHECK_FALSE(SongNoteMap::tick(bits, 0, 1));
  CHECK_FALSE(SongNoteMap::tick(bits, 12, 0));   // past the tracks
  CHECK_FALSE(SongNoteMap::tick(bits, 0, 128));  // past the steps
}

TEST_CASE("song notemap: empty steps, other data, fades, cuts, offs and reserved bytes clear the bit; the odd rows stay blank") {
  model::Project& p = fresh();
  model::Step data = model::kEmptyStep;  // only an instrument, a volume and an FX: not a note
  data.vol = 100;
  data.instr = 3;
  data.fx[0] = {4, 9};
  NEED(p.set_step(1, 0, 1, data) == model::Status::Ok);
  const std::uint8_t kNotNotes[] = {model::kNoteEmpty, static_cast<std::uint8_t>(model::kNoteLast + 1u), 252, model::kNoteFade, model::kNoteCut, model::kNoteOff};
  for (unsigned i = 0; i < sizeof(kNotNotes); ++i) {
    NEED(p.set_step(1, 1, i, note_step(kNotNotes[i])) == model::Status::Ok);
  }
  NEED(p.set_step(1, 0, 0, note_step(model::kNoteFirst)) == model::Status::Ok);
  NEED(p.set_step(1, 0, 15, note_step(model::kNoteLast)) == model::Status::Ok);
  std::uint8_t bits[SongNoteMap::kBytes];
  SongNoteMap::render(*p.pattern(1), bits);
  CHECK(SongNoteMap::tick(bits, 0, 0));  // the first and the last note
  CHECK(SongNoteMap::tick(bits, 0, 15));
  CHECK_FALSE(SongNoteMap::tick(bits, 0, 1));  // data without a note
  for (unsigned i = 0; i < sizeof(kNotNotes); ++i) {
    CHECK_FALSE(SongNoteMap::tick(bits, 1, i));
  }
  for (unsigned t = 0; t < SongNoteMap::kTracks; ++t) {  // the odd rows (the gaps between tracks) are never set
    for (unsigned b = 0; b < SongNoteMap::kStride; ++b) {
      CHECK(bits[(2u * t + 1u) * SongNoteMap::kStride + b] == 0u);
    }
  }
  SongNoteMap::render(*p.pattern(2), bits);  // an empty pattern: nothing
  for (unsigned b = 0; b < SongNoteMap::kBytes; ++b) {
    CHECK(bits[b] == 0u);
  }
}

TEST_CASE("song notemap: steps past a track's own length and tracks past 12 are not in the picture") {
  model::Project& p = fresh();
  NEED(p.set_pattern_length(1, 64) == model::Status::Ok);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned s = 0; s < 64; ++s) {
      NEED(p.set_step(1, t, s, note_step(30)) == model::Status::Ok);
    }
  }
  NEED(p.set_track_length(1, 2, 8) == model::Status::Ok);  // track 2 plays 8 steps (the steps past it are kept in the pattern)
  std::uint8_t bits[SongNoteMap::kBytes];
  SongNoteMap::render(*p.pattern(1), bits);
  for (unsigned s = 0; s < 64; ++s) {
    CHECK(SongNoteMap::tick(bits, 0, s));
    CHECK(SongNoteMap::tick(bits, 2, s) == (s < 8u));
  }
  CHECK_FALSE(SongNoteMap::tick(bits, 0, 64));
  for (unsigned t = SongNoteMap::kTracks; t < model::kMaxTracks; ++t) {  // tracks 13..16 hold notes in the pattern and have no row
    CHECK_FALSE(SongNoteMap::tick(bits, t, 0));
  }
  unsigned set = 0;
  for (unsigned b = 0; b < SongNoteMap::kBytes; ++b) {
    for (unsigned k = 0; k < 8; ++k) {
      set += (bits[b] >> k) & 1u;
    }
  }
  CHECK(set == 11u * 64u + 8u);  // 12 tracks of 64 steps less track 2's 56
}

TEST_CASE("song notemap: a picture goes to its pattern's address in RAM_G, once per generation") {
  model::Project& p = fresh();
  NEED(p.set_step(2, 1, 4, note_step(50)) == model::Status::Ok);
  FakeDisplay display;
  display.record_uploads(1u << 20);
  SongNoteMap map;
  CHECK(map.ensure(display, p, 2));
  NEED(display.uploads().size() == 1u);
  const FakeDisplay::Upload& u = display.uploads()[0];
  CHECK(u.addr == ui::plot::kRamGNoteMap + 1u * SongNoteMap::kBytes);
  CHECK(u.addr == SongNoteMap::addr(2));
  CHECK(u.bytes.size() == SongNoteMap::kBytes);
  std::uint8_t want[SongNoteMap::kBytes];
  SongNoteMap::render(*p.pattern(2), want);
  CHECK(std::memcmp(u.bytes.data(), want, SongNoteMap::kBytes) == 0);
  CHECK(SongNoteMap::addr(1) == ui::plot::kRamGNoteMap);
  CHECK(SongNoteMap::addr(255) + SongNoteMap::kBytes <= ui::plot::kRamGNoteMapEnd);
  for (unsigned frame = 0; frame < 10; ++frame) {  // the page's frames: a compare, no scan and no upload
    CHECK(map.ensure(display, p, 2));
  }
  CHECK(map.built() == 1u);
  CHECK(display.accepted_uploads() == 1u);
  CHECK(map.ensure(display, p, 1));  // another pattern: its own picture
  CHECK(map.built() == 2u);
  CHECK(display.uploads()[1].addr == ui::plot::kRamGNoteMap);
}

TEST_CASE("song notemap: a pattern that does not exist, or an id out of range, has no picture and uploads nothing") {
  model::Project& p = fresh();
  FakeDisplay display;
  SongNoteMap map;
  CHECK_FALSE(map.ensure(display, p, 0));
  CHECK_FALSE(map.ensure(display, p, 3));
  CHECK_FALSE(map.ensure(display, p, 255));
  CHECK_FALSE(map.ensure(display, p, 256));
  CHECK_FALSE(map.ensure(display, p, 4000));
  CHECK(display.accepted_uploads() == 0u);
  CHECK(map.built() == 0u);
}

TEST_CASE("song notemap: every pattern writer of the model makes the next frame render the picture again") {
  model::Project& p = fresh();
  FakeDisplay display;
  display.record_uploads(1u << 20);
  SongNoteMap map;
  NEED(map.ensure(display, p, 1));
  std::uint32_t built = map.built();
  const auto check = [&](const char* what, model::Status st) {
    CAPTURE(what);
    CHECK(st == model::Status::Ok);
    map.begin_frame();  // a frame of its own: the per-frame cap is not what this test is about
    CHECK(map.ensure(display, p, 1));
    CHECK_MESSAGE(map.built() == built + 1u, what << ": the picture was not rendered again");
    built = map.built();
    CHECK(map.ensure(display, p, 1));  // and it stays cached after that
    CHECK(map.built() == built);
    std::uint8_t want[SongNoteMap::kBytes];
    SongNoteMap::render(*p.pattern(1), want);  // what RAM_G holds is the pattern as it is now
    CHECK(std::memcmp(display.uploads().back().bytes.data(), want, SongNoteMap::kBytes) == 0);
  };
  check("set_step", p.set_step(1, 0, 2, note_step(33)));
  const model::Step two[2] = {note_step(34), note_step(35)};
  check("set_steps", p.set_steps(1, 1, 4, two, 2));
  check("set_pattern_name", p.set_pattern_name(1, "abc", 3));
  check("set_track_length", p.set_track_length(1, 0, 8));
  check("set_pattern_length", p.set_pattern_length(1, 32));
  check("clear_pattern", p.clear_pattern(1));
  check("set_step after the clear", p.set_step(1, 5, 5, note_step(70)));
  // a pattern created again in the same id (deleted first) is a new generation: its picture is made again
  NEED(p.delete_pattern(1) == model::Status::Ok);
  CHECK_FALSE(map.ensure(display, p, 1));  // gone: no picture
  const std::size_t uploads = display.accepted_uploads();
  check("create_pattern", p.create_pattern(1, 16));
  CHECK(display.accepted_uploads() == uploads + 1u);
  // a project load (reset) starts every pattern over
  map.begin_frame();
  NEED(map.ensure(display, p, 2));
  built = map.built();
  p.reset();
  NEED(p.create_pattern(2, 16) == model::Status::Ok);
  map.begin_frame();
  CHECK(map.ensure(display, p, 2));
  CHECK(map.built() == built + 1u);
}

TEST_CASE("song notemap: another project or another display starts over, and a refused upload is tried again") {
  model::Project& p = fresh();
  FakeDisplay a;
  FakeDisplay b;
  SongNoteMap map;
  CHECK(map.ensure(a, p, 1));
  CHECK(map.ensure(a, p, 1));
  CHECK(a.accepted_uploads() == 1u);
  CHECK(map.ensure(b, p, 1));  // the picture is in a's RAM_G, not b's
  CHECK(b.accepted_uploads() == 1u);
  CHECK(map.ensure(b, p, 1));
  CHECK(b.accepted_uploads() == 1u);
  b.set_busy(true);
  NEED(p.set_step(1, 0, 0, note_step(61)) == model::Status::Ok);
  CHECK_FALSE(map.ensure(b, p, 1));  // the display refused: no picture this frame
  CHECK(b.accepted_uploads() == 1u);
  b.set_busy(false);
  map.begin_frame();
  CHECK(map.ensure(b, p, 1));  // and the next frame uploads it
  CHECK(b.accepted_uploads() == 2u);
  // another Project object: nothing recorded is its
  model::Project& q = test::arena(2).project;
  q.reset();
  NEED(q.create_pattern(1, 16) == model::Status::Ok);
  CHECK(map.ensure(b, q, 1));
  CHECK(b.accepted_uploads() == 3u);
}

TEST_CASE("song notemap: a frame uploads at most kMaxPerFrame pictures, the rest follow on later frames, never a stale picture") {
  model::Project& p = test::arena(1).project;
  p.reset();
  constexpr unsigned kPatterns = 40;
  for (unsigned id = 1; id <= kPatterns; ++id) {
    NEED(p.create_pattern(id, 16) == model::Status::Ok);
  }
  static_assert(SongNoteMap::kMaxPerFrame * SongNoteMap::kBytes <= 4096u, "ADR 0010: 4 KB a pass");
  FakeDisplay display;
  SongNoteMap map;
  bool done[kPatterns + 1] = {};
  unsigned frames = 0;
  std::size_t last = 0;
  while (frames < 20) {
    map.begin_frame();
    ++frames;
    unsigned ready = 0;
    for (unsigned id = 1; id <= kPatterns; ++id) {  // what the page draws: a picture only where ensure says so
      done[id] = map.ensure(display, p, id) || done[id];
      ready += done[id] ? 1u : 0u;
    }
    CHECK(display.accepted_uploads() - last <= SongNoteMap::kMaxPerFrame);
    last = display.accepted_uploads();
    if (ready == kPatterns) {
      CHECK(!map.pending());
      break;
    }
    CHECK(map.pending());  // the app draws another frame
  }
  CHECK(frames == 5u);  // 40 pictures, 8 a frame
  CHECK(display.accepted_uploads() == kPatterns);
  CHECK(map.built() == kPatterns);
  // a picture that waits is not drawn, even when its old one is still in RAM_G: edit all 40 and draw one frame
  for (unsigned id = 1; id <= kPatterns; ++id) {
    NEED(p.set_step(id, 0, 0, note_step(40)) == model::Status::Ok);
  }
  map.begin_frame();
  unsigned drawn = 0;
  for (unsigned id = 1; id <= kPatterns; ++id) {
    drawn += map.ensure(display, p, id) ? 1u : 0u;
  }
  CHECK(drawn == SongNoteMap::kMaxPerFrame);
}

TEST_CASE("song notemap: after the frame's first Busy nothing more uploads that frame, and the next frame carries on") {
  model::Project& p = test::arena(1).project;
  p.reset();
  for (unsigned id = 1; id <= 6; ++id) {
    NEED(p.create_pattern(id, 16) == model::Status::Ok);
  }
  FakeDisplay display;
  SongNoteMap map;
  map.begin_frame();
  CHECK(map.ensure(display, p, 1));
  CHECK(map.ensure(display, p, 2));
  display.set_busy(true);
  CHECK_FALSE(map.ensure(display, p, 3));  // refused
  display.set_busy(false);                 // the display is free again, but this frame has stopped
  CHECK_FALSE(map.ensure(display, p, 4));
  CHECK_FALSE(map.ensure(display, p, 5));
  CHECK(map.ensure(display, p, 1));  // a picture already in RAM_G still draws
  CHECK(display.accepted_uploads() == 2u);
  CHECK(display.rejected_uploads() == 1u);  // the refused call was the only one that reached the display
  CHECK(map.pending());
  map.begin_frame();
  CHECK_FALSE(map.pending());
  for (unsigned id = 1; id <= 6; ++id) {
    CHECK(map.ensure(display, p, id));
  }
  CHECK(display.accepted_uploads() == 6u);
  CHECK_FALSE(map.pending());
}

TEST_CASE("song notemap: a 16-track pattern has no row past the 12th, and the drawn size never samples past the picture") {
  model::Project& p = fresh();
  NEED(p.set_active_tracks(16) == model::Status::Ok);
  NEED(p.set_pattern_length(1, 128) == model::Status::Ok);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned s = 0; s < 128; ++s) {
      NEED(p.set_step(1, t, s, note_step(30)) == model::Status::Ok);
    }
  }
  std::uint8_t bits[SongNoteMap::kBytes];
  SongNoteMap::render(*p.pattern(1), bits);
  unsigned set = 0;
  for (unsigned b = 0; b < SongNoteMap::kBytes; ++b) {
    for (unsigned k = 0; k < 8; ++k) {
      set += (bits[b] >> k) & 1u;
    }
  }
  CHECK(set == SongNoteMap::kTracks * 128u);  // 12 tracks of 128 steps, nothing from tracks 13..16
  FakeDisplay display;
  SongNoteMap map;
  map.begin_frame();
  CHECK(map.ensure(display, p, 1));
  CHECK(display.uploaded_bytes() == SongNoteMap::kBytes);  // one picture of 24 rows: nothing of the 4 tracks past the 12th
  // the drawn size: rows, for every lane the page can give 12 tracks (and fewer): the last pixel row samples a row of the tracks drawn
  for (unsigned tracks = 1; tracks <= SongNoteMap::kTracks; ++tracks) {
    for (int lane = 4; lane <= 40; ++lane) {
      const std::int32_t e = app::notemap_scale_y(lane);
      const int h = app::notemap_row_top(2u * tracks, e);
      CAPTURE(tracks);
      CAPTURE(lane);
      CHECK(h > 0);
      CHECK(static_cast<unsigned>((static_cast<std::uint32_t>(2 * (h - 1) + 1) * static_cast<std::uint32_t>(e)) / 512u) < 2u * tracks);
    }
  }
  // columns: for every block width and bar count the page can draw, the last pixel of the drawn width is a column of the picture, and the fit takes the widest such width
  for (unsigned bars = 1; bars <= 8; ++bars) {
    for (int span = 8; span <= 520; ++span) {
      const std::int32_t a = app::notemap_scale_x(bars, span);
      const int w = app::notemap_fit_width(a, span < 511 ? span : 511);
      CAPTURE(bars);
      CAPTURE(span);
      CHECK(w > 0);
      CHECK(w <= span);
      CHECK(static_cast<unsigned>((static_cast<std::uint32_t>(2 * (w - 1) + 1) * static_cast<std::uint32_t>(a)) / 512u) < SongNoteMap::kSteps);
      if (w < span && w < 511) {
        CHECK(static_cast<unsigned>((static_cast<std::uint32_t>(2 * w + 1) * static_cast<std::uint32_t>(a)) / 512u) >= SongNoteMap::kSteps);
      }
    }
  }
  CHECK(app::notemap_fit_width(256, 0) == 0);
}

TEST_CASE("song notemap: the scaling puts a step in the same width in every block and the rows on whole pixels") {
  // 16 steps a bar: a block of n bars `w` pixels wide shows 16 n columns, A = 256 * 16 n / w
  CHECK(app::notemap_scale_x(1, 16) == 256);   // 16 px a bar: one pixel a step
  CHECK(app::notemap_scale_x(2, 32) == 256);
  CHECK(app::notemap_scale_x(4, 64) == 256);
  CHECK(app::notemap_scale_x(1, 32) == 128);   // 32 px a bar: two pixels a step
  CHECK(app::notemap_scale_x(1, 49) == 84);    // rounds to nearest
  CHECK(app::notemap_scale_x(8, 392) == 84);
  CHECK(app::notemap_scale_x(1, 0) == 256);    // (no width: the identity, nothing is drawn)
  CHECK(app::notemap_scale_x(1, 100000) == 1); // never 0
  CHECK(app::notemap_scale_y(24) == 22);       // 512 / 24 = 21.33, rounded up
  CHECK(app::notemap_scale_y(16) == 32);
  CHECK(app::notemap_scale_y(22) == 24);
  CHECK(app::notemap_scale_y(1) == 256);
  // every row of the picture owns a run of pixels, the runs follow each other from 0, and the chip's sampling (the pixel's centre, floored) lands in the row: brute force over every E a lane 4..40 asks for
  for (int lane = 4; lane <= 40; ++lane) {
    const std::int32_t e = app::notemap_scale_y(lane);
    CAPTURE(lane);
    CHECK(app::notemap_row_top(0, e) == 0);
    for (unsigned row = 0; row < SongNoteMap::kRows; ++row) {
      const int y0 = app::notemap_row_top(row, e);
      const int y1 = app::notemap_row_top(row + 1u, e);
      CHECK(y1 > y0);  // a row is at least a pixel (E <= 128 for lane >= 4)
      for (int y = y0; y < y1; ++y) {
        CHECK(static_cast<unsigned>((static_cast<std::uint32_t>(2 * y + 1) * static_cast<std::uint32_t>(e)) / 512u) == row);
      }
    }
    // a track's lane (two rows) is no taller than the lane the page asked for, plus the rounding of the last pixel
    const int lane_px = app::notemap_row_top(2, e) - app::notemap_row_top(0, e);
    CHECK(lane_px <= lane);
    CHECK(lane_px >= lane - 1 - lane / 12);
  }
}
