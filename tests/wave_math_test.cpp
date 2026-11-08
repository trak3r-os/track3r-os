// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the arithmetic of the Wave / Chop screen (app/wave_math.h): the 0..65535 points (the OG instrument file's range), the four points and their order, the slice table (at most 48),
// the zero-crossing snap, the zoomed window.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/wave_math.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os::app;

namespace {

// A tiny deterministic generator for the property loops.
struct Lcg {
  std::uint32_t s = 12345u;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
  int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1)); }
};

}  // namespace

TEST_CASE("wave math: a point names the frame floor(point * frames / 65535), the last point the end; a frame's point is the nearest") {
  CHECK(point_to_frame(0, 1000) == 0u);
  CHECK(point_to_frame(65535, 1000) == 1000u);
  CHECK(point_to_frame(32768, 65535) == 32768u);
  CHECK(point_to_frame(1, 441000) == 6u);            // 6.7 frames a unit on a 10 s sample: floor
  CHECK(point_to_frame(65534, 5000000) == 4999923u);
  CHECK(frame_to_point(0, 441000) == 0u);
  CHECK(frame_to_point(441000, 441000) == 65535u);
  CHECK(frame_to_point(500000, 441000) == 65535u);   // past the end: the end
  CHECK(frame_to_point(10, 0) == 0u);
  // Back and forth: within one unit's width of frames.
  Lcg g;
  for (int i = 0; i < 2000; ++i) {
    const std::uint32_t frames = static_cast<std::uint32_t>(g.range(2, 5000000));
    const std::uint32_t f = static_cast<std::uint32_t>(g.range(0, static_cast<int>(frames) - 1));
    const std::uint32_t back = point_to_frame(frame_to_point(f, frames), frames);
    const std::uint32_t unit = frames / 65535u + 1u;
    CHECK(((back > f ? back - f : f - back) <= unit));
  }
}

TEST_CASE("wave math: setting a point keeps start < end and start <= loop start < loop end <= end whatever is asked, and moves the others only as far as it must") {
  Points p;  // 0, 0, 65535, 65535
  CHECK(points_valid(p));
  CHECK(set_point(p, PointId::Start, 1000) == 1000u);
  CHECK(p.loop_start == 1000u);  // the loop's start was behind it: pushed ahead of the start
  CHECK(p.loop_end == 65535u);
  CHECK(set_point(p, PointId::LoopStart, 20000) == 20000u);
  CHECK(set_point(p, PointId::LoopEnd, 40000) == 40000u);
  CHECK(set_point(p, PointId::LoopStart, 50000) == 39999u);  // never past the loop's end
  CHECK(set_point(p, PointId::LoopEnd, 100) == 40000u);      // nor the end before the start: loop end > loop start
  CHECK(set_point(p, PointId::LoopStart, 5) == 1000u);       // nor before the start point
  CHECK(set_point(p, PointId::End, 30000) == 30000u);        // pulls the loop in behind it
  CHECK(p.loop_end == 30000u);
  CHECK(points_valid(p));
  CHECK(set_point(p, PointId::End, 10) == 1001u);            // never at or before the start
  CHECK(points_valid(p));
  CHECK(set_point(p, PointId::Start, 70000) == 1000u);       // never at or past the end
  CHECK(p.start == 1000u);
  CHECK(set_point(p, PointId::Start, -5) == 0u);
  CHECK(get_point(p, PointId::End) == p.end);
  CHECK(get_point(p, PointId::LoopEnd) == p.loop_end);
}

TEST_CASE("wave math: a random walk over the four points never breaks the order, and the point asked for is the point returned") {
  Lcg g;
  for (int round = 0; round < 200; ++round) {
    Points p;
    if (round % 4 == 0) {  // junk, as a file may deliver it
      p.start = static_cast<std::uint16_t>(g.range(0, 65535));
      p.loop_start = static_cast<std::uint16_t>(g.range(0, 65535));
      p.loop_end = static_cast<std::uint16_t>(g.range(0, 65535));
      p.end = static_cast<std::uint16_t>(g.range(0, 65535));
    }
    for (int i = 0; i < 100; ++i) {
      const PointId id = static_cast<PointId>(g.range(0, 3));
      const int value = g.range(-100, 65700);
      const std::uint16_t r = set_point(p, id, value);
      CHECK(points_valid(p));
      CHECK(get_point(p, id) == r);
    }
  }
}

TEST_CASE("wave math: slices are kept ascending and unique, at most 48; a duplicate or a full table is refused") {
  std::uint16_t at[kMaxSliceCount] = {};
  std::uint8_t n = 0;
  CHECK(slice_add(at, n, 30000) == 0u);
  CHECK(slice_add(at, n, 10000) == 0u);   // goes in front
  CHECK(slice_add(at, n, 20000) == 1u);   // between
  CHECK(slice_add(at, n, 40000) == 3u);
  CHECK(slice_add(at, n, 20000) == kNoSlice);  // already a start
  CHECK(slice_add(at, n, 70000) == kNoSlice);  // not a point
  CHECK(n == 4u);
  CHECK((at[0] == 10000u && at[1] == 20000u && at[2] == 30000u && at[3] == 40000u));
  CHECK(slice_remove(at, n, 1));
  CHECK((n == 3u && at[1] == 30000u && at[3] == 0u));
  CHECK_FALSE(slice_remove(at, n, 3));
  // Full.
  n = 0;
  for (unsigned i = 0; i < kMaxSliceCount; ++i) {
    CHECK(slice_add(at, n, 100 + 100 * i) != kNoSlice);
  }
  CHECK(n == 48u);
  CHECK(slice_add(at, n, 60000) == kNoSlice);
  CHECK(slice_add(at, n, 99, 10) == kNoSlice);  // a smaller cap
  for (unsigned i = 1; i < n; ++i) {
    CHECK(at[i - 1] < at[i]);
  }
}

TEST_CASE("wave math: EVEN makes n equal slices from the start, and the table past them is zero") {
  std::uint16_t at[kMaxSliceCount];
  for (auto& a : at) {
    a = 777;
  }
  std::uint8_t n = 5;
  CHECK(slice_even(at, n, 16) == 16u);
  CHECK(n == 16u);
  CHECK(at[0] == 0u);
  CHECK(at[4] == 16383u);
  CHECK(at[8] == 32767u);
  CHECK(at[15] == 61439u);
  CHECK(at[16] == 0u);
  CHECK(at[47] == 0u);
  CHECK(slice_even(at, n, 0) == 1u);    // at least one
  CHECK(slice_even(at, n, 99) == 48u);  // at most 48
  CHECK(at[47] == 64169u);
  for (unsigned i = 1; i < 48; ++i) {
    CHECK(at[i - 1] < at[i]);
  }
}

TEST_CASE("wave math: a slice moves between its neighbours and no further; slice_of finds the slice a point is in") {
  std::uint16_t at[kMaxSliceCount] = {0, 1000, 2000, 3000};
  CHECK(slice_move(at, 4, 1, 1500) == 1500u);
  CHECK(slice_move(at, 4, 1, 5000) == 1999u);
  CHECK(slice_move(at, 4, 1, -5) == 1u);      // above the first start
  CHECK(slice_move(at, 4, 0, -5) == 0u);      // the first may go to 0
  CHECK(slice_move(at, 4, 0, 20000) == 0u);   // (the next is at 1: nothing between)
  CHECK(slice_move(at, 4, 3, 99999) == 65535u);
  CHECK(slice_move(at, 4, 4, 1) == kNoSlice);
  const std::uint16_t four[4] = {0, 1000, 2000, 3000};
  CHECK(slice_of(four, 4, 0) == 0u);
  CHECK(slice_of(four, 4, 1999) == 1u);
  CHECK(slice_of(four, 4, 2000) == 2u);
  CHECK(slice_of(four, 4, 65535) == 3u);
  std::uint16_t late[2] = {500, 900};
  CHECK(slice_of(late, 2, 100) == kNoSlice);  // before the first
  CHECK(slice_of(late, 0, 100) == kNoSlice);
}

TEST_CASE("wave math: the snap finds a zero crossing near the point, the one nearest it, and reads nothing outside the sample") {
  const std::uint32_t frames = 65535;  // a unit is a frame
  std::vector<std::int16_t> pcm(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    pcm[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 44100.0 + 0.7)));
  }
  Lcg g;
  for (int i = 0; i < 300; ++i) {
    const std::uint32_t p = static_cast<std::uint32_t>(g.range(10, 65500));
    const std::uint32_t s = snap_point(pcm.data(), frames, p, 60);
    CHECK(((s > p ? s - p : p - s) <= 60u));
    // A crossing within 60 frames always exists (a period is 100 frames: two crossings), so the result is one.
    const bool crossing = pcm[s] == 0 || (s + 1 < frames && ((pcm[s] < 0) != (pcm[s + 1] < 0)));
    CHECK(crossing);
    // Nothing nearer is a crossing.
    for (std::uint32_t d = 0; d < (s > p ? s - p : p - s); ++d) {
      for (const std::uint32_t u : {p - d, p + d}) {
        CHECK_FALSE((pcm[u] == 0 || ((pcm[u] < 0) != (pcm[u + 1] < 0))));
      }
    }
  }
  // Without a crossing (a constant) the point stays; at the ends it stays inside the sample.
  std::vector<std::int16_t> flat(1000, 500);
  CHECK(snap_point(flat.data(), 1000, 5000, 30) == 5000u);
  CHECK(snap_point(pcm.data(), frames, 0, 50) <= 50u);
  CHECK(snap_point(pcm.data(), frames, 65535, 50) >= 65485u);
  CHECK(snap_point(pcm.data(), frames, 65535, 50) <= 65535u);
  CHECK(snap_point(nullptr, 100, 7, 5) == 7u);
  CHECK(snap_point(pcm.data(), 1, 7, 5) == 7u);
  // On a long sample a unit is several frames: the nearest unit with the smallest sample.
  std::vector<std::int16_t> ramp(262140);
  for (std::uint32_t i = 0; i < ramp.size(); ++i) {
    ramp[i] = static_cast<std::int16_t>(static_cast<int>(i % 2000) - 1000);  // crossing every 2000 frames, near frame 1000 + 2000k
  }
  const std::uint32_t s = snap_point(ramp.data(), 262140, 250, 10);  // frame 1000 is unit 250: a crossing exactly
  CHECK(s == 250u);
  CHECK(snap_radius(65535, 6553, 800) >= 1u);
  CHECK(snap_radius(0, 100, 800) == 1u);
  CHECK(snap_radius(441000, 441000, 800) >= 1u);
}

TEST_CASE("wave math: zooming keeps the anchor where it was on the lane, stops at the whole sample and at 32 frames, and stays inside the sample") {
  const std::uint32_t frames = 100000;
  Window w = window_fit(frames);
  CHECK(w.first == 0u);
  CHECK(w.span == frames);
  Window z = window_zoom(w, frames, 1, 60000);
  CHECK(z.span == 50000u);
  // The anchor (60000, at 60 % of the lane) stays at 60 %.
  CHECK(z.first == 30000u);
  CHECK((60000 - z.first) * 100 / z.span == 60u);
  Window out = window_zoom(z, frames, -1, 60000);
  CHECK(out.span == frames);
  CHECK(out.first == 0u);
  Window out2 = window_zoom(out, frames, -1, 0);
  CHECK(out2.span == frames);  // already the whole sample
  // To the closest zoom.
  Window c = w;
  for (int i = 0; i < 40; ++i) {
    c = window_zoom(c, frames, 1, 12345);
  }
  CHECK(c.span == kMinSpan);
  CHECK(c.first <= 12345u);
  CHECK(c.first + c.span > 12345u - 1u);
  CHECK(c.first + c.span <= frames);
  // A sample shorter than the closest zoom.
  CHECK(window_zoom(window_fit(10), 10, 1, 5).span == 10u);
  CHECK(window_zoom(Window{0, 0}, 0, 1, 0).span == 0u);
  // Invalid windows are made valid.
  const Window bad = window_clamp(Window{99990, 500}, frames);
  CHECK(bad.span == 500u);
  CHECK(bad.first + bad.span <= frames);
  CHECK(window_clamp(Window{5, 0}, frames).span == 1u);
  CHECK(window_clamp(Window{5, 999999}, frames).span == frames);
}

TEST_CASE("wave math: window_show moves the window by the least that brings a frame into view") {
  const std::uint32_t frames = 100000;
  const Window w{20000, 10000};
  CHECK(window_show(w, frames, 25000).first == 20000u);   // inside: not moved
  CHECK(window_show(w, frames, 10000).first == 10000u);   // before: the frame is the first
  CHECK(window_show(w, frames, 40000).first == 30001u);   // after: the frame is the last
  CHECK(window_show(w, frames, 99999).first == 90000u);   // held inside the sample
  CHECK(window_show(window_fit(frames), frames, 5).first == 0u);
}

TEST_CASE("wave math: the loop's levels are the samples at its two frames and their distance, clamped to the sample, nothing without one") {
  std::vector<std::int16_t> pcm(1000, 0);
  pcm[100] = 13763;   // +0.42
  pcm[899] = -12780;  // -0.39 (the last frame of the loop [100, 900))
  pcm[999] = -32768;
  LoopLevels l = loop_levels(pcm.data(), 1000, 100, 899);
  CHECK(l.valid);
  CHECK(l.at_start == 13763);
  CHECK(l.at_end == -12780);
  CHECK(l.jump == 26543u);
  l = loop_levels(pcm.data(), 1000, 100, 1000);  // LOOP END at 65535 names frame `frames`: the last one is read
  CHECK(l.at_end == -32768);
  CHECK(l.jump == 46531u);
  l = loop_levels(pcm.data(), 1000, 5000, 7000);  // both far past the end
  CHECK(l.valid);
  CHECK(l.at_start == -32768);
  CHECK(l.at_end == -32768);
  CHECK(l.jump == 0u);
  l = loop_levels(pcm.data(), 1, 0, 0);  // one frame
  CHECK(l.valid);
  CHECK(l.jump == 0u);
  pcm[0] = 32767;
  pcm[1] = -32768;
  l = loop_levels(pcm.data(), 2, 0, 1);  // the widest jump there is
  CHECK(l.jump == 65535u);
  for (const LoopLevels& none : {loop_levels(nullptr, 1000, 1, 2), loop_levels(pcm.data(), 0, 1, 2)}) {
    CHECK_FALSE(none.valid);
    CHECK(none.at_start == 0);
    CHECK(none.at_end == 0);
    CHECK(none.jump == 0u);
  }
}

TEST_CASE("wave math: level_text writes a sample as a fraction of full scale, two decimals, rounded, and a jump up to 2.00") {
  const auto text = [](std::int32_t v, bool sign) {
    char out[7];
    const unsigned n = level_text(out, v, sign);
    CHECK(n == std::strlen(out));
    return std::string(out);
  };
  CHECK(text(0, true) == "+0.00");
  CHECK(text(0, false) == "0.00");
  CHECK(text(13763, true) == "+0.42");
  CHECK(text(-12780, true) == "-0.39");
  CHECK(text(32767, true) == "+1.00");
  CHECK(text(-32768, true) == "-1.00");
  CHECK(text(-100, true) == "+0.00");  // rounds to nothing: no minus sign on a zero
  CHECK(text(164, true) == "+0.01");   // 0.005 rounds up
  CHECK(text(163, true) == "+0.00");
  CHECK(text(-164, true) == "-0.01");
  CHECK(text(983, false) == "0.03");
  CHECK(text(26543, false) == "0.81");
  CHECK(text(65535, false) == "2.00");
  CHECK(text(-32768, false) == "1.00");  // an unsigned text of a negative value is its magnitude
}

// ---- OS-INSTUI S3 / S4: the wavetable's window (play mode change, window size, window position) --------------------------------------------------

TEST_CASE("wavetable window: the valid sizes are the powers of two from 32 to 2048, anything else plays 2048, the counter is its log2") {
  for (unsigned size = 0; size <= 4096; ++size) {
    const bool pow2 = size >= 32 && size <= 2048 && (size & (size - 1)) == 0;
    CHECK_MESSAGE(wt_size_valid(size) == pow2, size);
    CHECK_MESSAGE(wt_size_eff(size) == (pow2 ? size : 2048u), size);
  }
  CHECK(wt_counter(32) == 5);
  CHECK(wt_counter(2048) == 11);
  CHECK(wt_counter(1) == 0);
  CHECK(wt_counter(0) == 0);
}

TEST_CASE("wavetable window: the largest counter a sample takes follows its length (11 halved while 2^c > frames, never under 5) and a sample under 32 frames has no window") {
  CHECK(wt_max_counter(0) == 11);        // no sample known
  CHECK(wt_max_counter(1000000) == 11);
  CHECK(wt_max_counter(2048) == 11);
  CHECK(wt_max_counter(2047) == 10);
  CHECK(wt_max_counter(1024) == 10);
  CHECK(wt_max_counter(33) == 5);
  CHECK(wt_max_counter(32) == 5);
  CHECK(wt_max_counter(5) == 5);
  CHECK(wt_fit_size(0, 100000) == 2048);   // unset: the default
  CHECK(wt_fit_size(2048, 3000) == 2048);
  CHECK(wt_fit_size(2048, 2047) == 1024);  // held to the length, rounded down to a power of two
  CHECK(wt_fit_size(512, 100000) == 512);
  CHECK(wt_fit_size(512, 100) == 64);
  CHECK(wt_fit_size(0, 32) == 32);
  CHECK(wt_fit_size(0, 31) == 0);          // not one window: refused by the play mode action
  CHECK(wt_fit_size(0, 0) == 2048);        // no sample known: as the engine plays it
}

TEST_CASE("wavetable window: the position is held to the windows the sample holds") {
  CHECK(wt_windows(96000, 2048) == 46);
  CHECK(wt_windows(2047, 2048) == 0);
  CHECK(wt_windows(96000, 0) == 0);
  CHECK(wt_clamp_pos(96000, 2048, 0) == 0);
  CHECK(wt_clamp_pos(96000, 2048, 45) == 45);
  CHECK(wt_clamp_pos(96000, 2048, 46) == 45);     // the last window
  CHECK(wt_clamp_pos(96000, 2048, 60000) == 45);
  CHECK(wt_clamp_pos(1000, 2048, 5) == 0);         // none: 0
  CHECK(wt_clamp_pos(0, 2048, 5) == 5);            // no sample known: untouched
}

TEST_CASE("wavetable window: a step of the size moves the counter and the position with it, and stops at the ends the sample allows") {
  // 262144 frames (a 256-frame Serum table of 1024): 11 -> 10 doubles the position, 10 -> 11 halves it
  WtWindow w = wt_step_size(262144, 2048, 100, -1);
  CHECK(w.size == 1024);
  CHECK(w.pos == 200);
  w = wt_step_size(262144, w.size, w.pos, +1);
  CHECK(w.size == 2048);
  CHECK(w.pos == 100);
  // down to 32 and no further
  w = WtWindow{2048, 5};
  for (int i = 0; i < 10; ++i) {
    w = wt_step_size(262144, w.size, w.pos, -1);
  }
  CHECK(w.size == 32);
  CHECK(w.pos == 5u << 6);
  // up to the largest the sample holds: 3000 frames take 2048 at most, 1500 take 1024
  CHECK(wt_step_size(1500, 512, 1, +1).size == 1024);
  CHECK(wt_step_size(1500, 1024, 0, +1).size == 1024);
  // the position stays inside the windows of the new size (1500 / 1024 = 1 window: only position 0)
  CHECK(wt_step_size(1500, 512, 2, +1).pos == 0);
  // an unset size steps from the default
  CHECK(wt_step_size(262144, 0, 0, -1).size == 1024);
  CHECK(wt_step_size(262144, 0, 0, +1).size == 2048);
}

TEST_CASE("wavetable and grain frames: the window is whole and inside the sample, the grain is centred on its position and held inside the sample") {
  FrameRange w = wt_window_frames(96000, 2048, 3);
  CHECK(w.valid);
  CHECK(w.begin == 3u * 2048u);
  CHECK(w.end == 4u * 2048u);
  w = wt_window_frames(96000, 2048, 5000);  // past the windows: the last whole one
  CHECK(w.begin == 45u * 2048u);
  CHECK(w.end == 46u * 2048u);
  w = wt_window_frames(96000, 0, 0);  // unset: 2048
  CHECK(w.end - w.begin == 2048u);
  w = wt_window_frames(3000, 2048, 9);  // 3000 frames hold one window of 2048
  CHECK(w.begin == 0u);
  CHECK(w.end == 2048u);
  CHECK_FALSE(wt_window_frames(20, 2048, 0).valid);  // under 32 frames: none
  CHECK_FALSE(wt_window_frames(0, 2048, 0).valid);
  FrameRange g = grain_frames(100000, 32768, 2000);
  CHECK(g.valid);
  CHECK(g.end - g.begin == 2000u);
  CHECK(g.begin == point_to_frame(32768, 100000) - 1000u);
  g = grain_frames(100000, 0, 2000);  // at the start: held at 0
  CHECK(g.begin == 0u);
  CHECK(g.end == 1000u);
  g = grain_frames(100000, 65535, 2000);  // at the end: held at the sample's end
  CHECK(g.end == 100000u);
  g = grain_frames(100000, 100, 0);  // unset: our default, 441
  CHECK(g.valid);
  CHECK(g.end - g.begin <= 441u);
  CHECK_FALSE(grain_frames(0, 0, 441).valid);
}

// OS-CEPTFX S4c: insert_points (Add Silence, a plugin's set_extra_samples): a point at or before the insertion's start keeps its frame, one inside moves by the prepend, one at or after the end by both.
// (A point is a position in 65536 steps: the frame it names rounds down, so the expected points are compared within 2 units.)
namespace {
bool near_point(unsigned got, unsigned want) { return got + 2u >= want && got <= want + 2u; }
}  // namespace

TEST_CASE("insert_points: points and slices follow the frames they name; nothing inserted leaves the record") {
  PointsRec in;
  in.points.start = 0;
  in.points.loop_start = static_cast<std::uint16_t>(frame_to_point(2000, 10000));
  in.points.loop_end = static_cast<std::uint16_t>(frame_to_point(8000, 10000));
  in.points.end = kPointMax;
  in.slices[0] = 0;
  in.slices[1] = static_cast<std::uint16_t>(frame_to_point(5000, 10000));
  in.slice_count = 2;
  const PointsRec same = insert_points(in, 10000, 0, 10000, 0, 0);
  CHECK(same.points.loop_start == in.points.loop_start);
  CHECK(same.slice_count == 2u);
  // 1000 frames of silence in front of the whole sample: every point keeps its audio, now 1000 frames later.
  const PointsRec pre = insert_points(in, 10000, 0, 10000, 1000, 0);
  CHECK(pre.points.start == 0u);  // a point at the insertion's start stays
  CHECK(near_point(pre.points.loop_start, frame_to_point(3000, 11000)));
  CHECK(near_point(pre.points.loop_end, frame_to_point(9000, 11000)));
  CHECK(pre.points.end == kPointMax);
  CHECK(pre.slice_count == 2u);
  CHECK(near_point(pre.slices[1], frame_to_point(6000, 11000)));
  // 500 frames appended: the points inside keep their frames of the longer sample; END (at the end) moves with the tail.
  const PointsRec app = insert_points(in, 10000, 0, 10000, 0, 500);
  CHECK(near_point(app.points.loop_start, frame_to_point(2000, 10500)));
  CHECK(near_point(app.points.loop_end, frame_to_point(8000, 10500)));
  CHECK(app.points.end == kPointMax);
  CHECK(points_valid(app.points));
  // Insertions inside a selection [3000, 6000): before it nothing moves, a point inside moves by the prepend, after it by prepend + append.
  const PointsRec mid = insert_points(in, 10000, 3000, 6000, 100, 50);
  CHECK(near_point(mid.points.loop_start, frame_to_point(2000, 10150)));
  CHECK(near_point(mid.points.loop_end, frame_to_point(8150, 10150)));
  CHECK(near_point(mid.slices[1], frame_to_point(5100, 10150)));
  CHECK(points_valid(mid.points));
}

TEST_CASE("insert_points: bad ranges and overflowing lengths return the record as it was") {
  PointsRec in;
  in.points.loop_start = 100;
  CHECK(insert_points(in, 0, 0, 0, 5, 5).points.loop_start == 100u);
  CHECK(insert_points(in, 1000, 600, 500, 5, 5).points.loop_start == 100u);  // b <= a
  CHECK(insert_points(in, 1000, 0, 1001, 5, 5).points.loop_start == 100u);   // b past the end
  CHECK(insert_points(in, 1000, 0, 1000, 0x40000000u, 0).points.loop_start == 100u);
}
