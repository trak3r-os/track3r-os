// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI Time Code out (midi/mtc.h, OS-MTC A3; docs/specs/OS-MTC.md section 3): the encoders at the four rate codes, the exact Q32 boundaries, the generator over a run (the
// first full frame, the quarter frames on the exact sample and fraction, pieces in order and whole cycles, a mid-song anchor, a relocation, the stop, the 24-hour wrap, a run longer
// than a rebase), and the router's side: the quarter frame on the MTC ports as `F1 0nnndddd`, the full frame as SysEx on DIN and as four USB words, whole or not at all, the OUT
// OFFSET, the export's suppression, the lateness bound with no notes. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/events.h>
#include <o3os/core/audio_clock.h>
#include <o3os/core/qtime.h>
#include <o3os/midi/config.h>
#include <o3os/midi/mtc.h>
#include <o3os/midi/out.h>

#include <cstdint>
#include <vector>

#include "support/need.h"

namespace midi = o3os::midi;
namespace mtc = o3os::midi::mtc;
namespace audio = o3os::audio;
namespace core = o3os::core;
using audio::EventKind;

namespace {

constexpr bool kRate44100 = core::kRefCyclesPerSample.num == 1800000u && core::kRefCyclesPerSample.den == 441u;
constexpr std::uint64_t kSecond = 44100u;  // only used where the test says it needs the default clock

// An event of the generator with the absolute sample of its block.
struct Got {
  std::uint64_t sample;   // block start + offset: the nearest sample
  std::int32_t value;
  EventKind kind;
  std::uint8_t a;
};

// Runs the generator over [from, to) in blocks of `n` samples and collects what it says.
std::vector<Got> run(mtc::Generator& g, std::uint64_t from, std::uint64_t to, std::uint32_t n) {
  std::vector<Got> got;
  for (std::uint64_t bs = from; bs < to; bs += n) {
    audio::SeqEvent out[mtc::Generator::kMaxEvents];
    const std::uint32_t c = g.step(bs, n, out);
    CHECK(c <= mtc::Generator::kMaxEvents);
    for (std::uint32_t i = 0; i < c; ++i) {
      got.push_back(Got{bs + out[i].offset, out[i].value, out[i].kind, out[i].a});
    }
  }
  return got;
}

// A receiver's view of eight pieces: the time of the frame at which piece 0 was sent, and the rate code. Written from the MIDI specification, apart from the encoder.
struct Decoded {
  bool ok = false;
  mtc::Time t;
  std::uint8_t code = 0;
};
Decoded decode(const std::uint8_t* piece /* eight data bytes in order 0..7 */) {
  Decoded d;
  for (unsigned i = 0; i < 8; ++i) {
    if ((piece[i] >> 4) != i || (piece[i] & 0x80u) != 0u) {
      return d;
    }
  }
  const auto lo = [&](unsigned i) { return static_cast<unsigned>(piece[i] & 0x0Fu); };
  d.t.f = static_cast<std::uint8_t>(lo(0) | (lo(1) << 4));
  d.t.s = static_cast<std::uint8_t>(lo(2) | (lo(3) << 4));
  d.t.m = static_cast<std::uint8_t>(lo(4) | (lo(5) << 4));
  d.t.h = static_cast<std::uint8_t>(lo(6) | ((lo(7) & 1u) << 4));
  d.code = static_cast<std::uint8_t>((lo(7) >> 1) & 3u);
  d.ok = true;
  return d;
}

}  // namespace

TEST_CASE("mtc: the rate table and the day") {
  CHECK(mtc::fps_of_setting(0) == 0u);
  CHECK(mtc::fps_of_setting(1) == 24u);
  CHECK(mtc::fps_of_setting(2) == 25u);
  CHECK(mtc::fps_of_setting(3) == 30u);
  CHECK(mtc::fps_of_setting(4) == 0u);
  CHECK(mtc::valid_fps(24));
  CHECK(mtc::valid_fps(25));
  CHECK(mtc::valid_fps(30));
  CHECK_FALSE(mtc::valid_fps(0));
  CHECK_FALSE(mtc::valid_fps(29));  // 29.97 drop-frame is not offered (Q1 a)
  CHECK_FALSE(mtc::valid_fps(60));
  CHECK(mtc::rate_code(24) == 0u);
  CHECK(mtc::rate_code(25) == 1u);
  CHECK(mtc::rate_code(30) == 3u);  // 2 is 29.97 drop-frame
  CHECK(mtc::quarter_frames_per_day(24) == 8294400u);
  CHECK(mtc::quarter_frames_per_day(25) == 8640000u);
  CHECK(mtc::quarter_frames_per_day(30) == 10368000u);
  CHECK(mtc::quarter_frames_per_day(0) == 0u);
  for (const std::uint32_t fps : {24u, 25u, 30u}) {
    CHECK(mtc::quarter_frames_per_day(fps) % 8u == 0u);  // a whole number of 8-piece cycles: the count wraps between cycles
  }
}

TEST_CASE("mtc: a frame count is a time, wraps at 24 hours, and back") {
  for (const std::uint32_t fps : {24u, 25u, 30u}) {
    const std::uint64_t per_day = std::uint64_t{86400u} * fps;
    CHECK(mtc::time_of_frame(0, fps) == mtc::Time{0, 0, 0, 0});
    CHECK(mtc::time_of_frame(fps - 1u, fps) == mtc::Time{0, 0, 0, static_cast<std::uint8_t>(fps - 1u)});
    CHECK(mtc::time_of_frame(fps, fps) == mtc::Time{0, 0, 1, 0});
    CHECK(mtc::time_of_frame(std::uint64_t{3600u} * fps, fps) == mtc::Time{1, 0, 0, 0});
    CHECK(mtc::time_of_frame(per_day - 1u, fps) == mtc::Time{23, 59, 59, static_cast<std::uint8_t>(fps - 1u)});
    CHECK(mtc::time_of_frame(per_day, fps) == mtc::Time{0, 0, 0, 0});  // the wrap
    CHECK(mtc::time_of_frame(per_day + 5u, fps) == mtc::Time{0, 0, 0, 5});
    for (std::uint64_t f = 0; f < per_day; f += 7919u) {
      CHECK(mtc::frame_of_time(mtc::time_of_frame(f, fps), fps) == f);
    }
  }
  CHECK(mtc::time_of_frame(100, 0) == mtc::Time{0, 0, 0, 0});  // an invalid rate
}

TEST_CASE("mtc: the quarter-frame bytes of known times at 24, 25 and 30 fps") {
  struct Known {
    std::uint32_t fps;
    mtc::Time t;
    std::uint8_t bytes[8];
  };
  // The frame of each time is even (a cycle starts on an even frame count from 00:00:00:00), the pieces are worked by hand from the MIDI specification.
  const Known known[] = {
      {24, {1, 23, 45, 12}, {0x0C, 0x10, 0x2D, 0x32, 0x47, 0x51, 0x61, 0x70}},  // rate code 0: 24
      {25, {1, 23, 45, 13}, {0x0D, 0x10, 0x2D, 0x32, 0x47, 0x51, 0x61, 0x72}},  // rate code 1: 25 (the frame number within the second is odd: 45 s is an odd second)
      {30, {23, 59, 59, 28}, {0x0C, 0x11, 0x2B, 0x33, 0x4B, 0x53, 0x67, 0x77}}, // rate code 3: 30, hours 23, frames 28 (high bit set)
      {25, {23, 0, 0, 0}, {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x67, 0x73}},
      {30, {0, 0, 0, 0}, {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x76}},
  };
  for (const Known& k : known) {
    const std::uint32_t frame = mtc::frame_of_time(k.t, k.fps);
    NEED(frame % 2u == 0u);
    for (unsigned p = 0; p < 8; ++p) {
      CHECK_MESSAGE(mtc::qf_data(p, k.t, mtc::rate_code(k.fps)) == k.bytes[p], k.fps << " fps piece " << p);
      CHECK_MESSAGE(mtc::qf_byte(std::uint64_t{frame} * 4u + p, k.fps) == k.bytes[p], k.fps << " fps q piece " << p);
    }
    // The receiver reads it back, and the pieces of one cycle all spell the time of the frame at which piece 0 was sent.
    const Decoded d = decode(k.bytes);
    CHECK(d.ok);
    CHECK(d.t == k.t);
    CHECK(d.code == mtc::rate_code(k.fps));
  }
  // q wraps with the day: the first quarter frame of the next day is the first of this one.
  CHECK(mtc::qf_byte(mtc::quarter_frames_per_day(25), 25) == mtc::qf_byte(0, 25));
  CHECK(mtc::qf_byte(mtc::quarter_frames_per_day(25) + 5u, 25) == mtc::qf_byte(5, 25));
  CHECK(mtc::qf_byte(3, 0) == 0u);
}

TEST_CASE("mtc: a full frame is F0 7F 7F 01 01 hh mm ss ff F7 and packs into 24 bits") {
  const mtc::Time t{1, 23, 45, 13};
  const std::uint32_t packed = mtc::pack_full(t, 1);
  CHECK(packed < (1u << 24));
  CHECK(mtc::unpack_time(packed) == t);
  CHECK(mtc::unpack_code(packed) == 1u);
  std::uint8_t body[mtc::kFullFrameBodyBytes];
  mtc::full_frame_body(packed, body);
  const std::uint8_t want[8] = {0x7F, 0x7F, 0x01, 0x01, 0x21 /* rate 1 << 5 | hours 1 */, 23, 45, 13};
  for (unsigned i = 0; i < 8; ++i) {
    CHECK_MESSAGE(body[i] == want[i], "byte " << i);
  }
  // The extremes: every field at its largest, at every rate code.
  for (const unsigned code : {0u, 1u, 3u}) {
    const mtc::Time top{23, 59, 59, 29};
    const std::uint32_t pk = mtc::pack_full(top, static_cast<std::uint8_t>(code));
    CHECK(mtc::unpack_time(pk) == top);
    CHECK(mtc::unpack_code(pk) == code);
    mtc::full_frame_body(pk, body);
    CHECK(body[4] == ((code << 5) | 23u));
    CHECK(body[5] == 59);
    CHECK(body[6] == 59);
    CHECK(body[7] == 29);
    for (unsigned i = 0; i < 8; ++i) {
      CHECK((body[i] & 0x80u) == 0u);  // SysEx data bytes
    }
  }
  // As USB event words: F0 7F 7F | 01 01 hh | mm ss ff | F7, code indexes 4, 4, 4, 5.
  std::uint32_t words[4];
  CHECK(midi::usb_sysex_events(0, body, 8, words) == 4u);
  mtc::full_frame_body(packed, body);
  CHECK(midi::usb_sysex_events(2, body, 8, words) == 4u);
  CHECK(words[0] == (0x24u | (0xF0u << 8) | (0x7Fu << 16) | (0x7Fu << 24)));
  CHECK(words[1] == (0x24u | (0x01u << 8) | (0x01u << 16) | (0x21u << 24)));
  CHECK(words[2] == (0x24u | (23u << 8) | (45u << 16) | (13u << 24)));
  CHECK(words[3] == (0x25u | (0xF7u << 8)));
  // Other lengths: the last chunk is told by its length (5, 6, 7), a chunk of three that is not the last is 4.
  const std::uint8_t two[2] = {0x10, 0x20};
  std::uint32_t w[3];
  CHECK(midi::usb_sysex_events(0, two, 2, w) == 2u);  // F0 10 20 | F7
  CHECK(w[0] == (0x04u | (0xF0u << 8) | (0x10u << 16) | (0x20u << 24)));
  CHECK(w[1] == (0x05u | (0xF7u << 8)));
  CHECK(midi::usb_sysex_events(0, two, 1, w) == 1u);  // F0 10 F7: one chunk of three that is the last: 7
  CHECK(w[0] == (0x07u | (0xF0u << 8) | (0x10u << 16) | (0xF7u << 24)));
  CHECK(midi::usb_sysex_events(0, two, 0, w) == 1u);  // F0 F7: 6
  CHECK(w[0] == (0x06u | (0xF0u << 8) | (0xF7u << 16)));
}

TEST_CASE("mtc: the due time of a quarter frame is exact in Q32 at 44,100 Hz, and the inverse finds the first boundary at or after a sample") {
  if (!kRate44100) {
    return;  // the pre-1.0 clock (-DO3OS_RATE_44100=OFF) has no exact periods: the arithmetic is the same, the constants below are not
  }
  // 24 fps: 459.375 samples a quarter frame; 25: 441; 30: 367.5.
  CHECK(mtc::qf_time_q32(1, 24) == ((core::Q32{459} << 32) | 0x60000000u));
  CHECK(mtc::qf_time_q32(8, 24) == (core::Q32{3675} << 32));  // eight quarter frames are two frames: 2 x 1837.5 = 3675
  CHECK(mtc::qf_time_q32(1, 25) == (core::Q32{441} << 32));
  CHECK(mtc::qf_time_q32(1, 30) == ((core::Q32{367} << 32) | 0x80000000u));
  CHECK(mtc::qf_time_q32(0, 30) == 0u);
  CHECK(mtc::qf_time_q32(1, 29) == 0u);  // invalid
  // A day: 24 h x 3600 x 44,100 samples, at every rate, exactly.
  for (const std::uint32_t fps : {24u, 25u, 30u}) {
    CHECK(mtc::qf_time_q32(mtc::quarter_frames_per_day(fps), fps) == (core::Q32{86400u} * 44100u << 32));
  }
  // Inverse: the first quarter frame at or after a sample.
  CHECK(mtc::qf_at_or_after(0, 25) == 0u);
  CHECK(mtc::qf_at_or_after(441, 25) == 1u);
  CHECK(mtc::qf_at_or_after(442, 25) == 2u);
  CHECK(mtc::qf_at_or_after(440, 25) == 1u);
  CHECK(mtc::qf_at_or_after(459, 24) == 1u);  // 459 < 459.375
  CHECK(mtc::qf_at_or_after(460, 24) == 2u);
  CHECK(mtc::qf_at_or_after(367, 30) == 1u);
  CHECK(mtc::qf_at_or_after(368, 30) == 2u);
  for (const std::uint32_t fps : {24u, 25u, 30u}) {
    for (std::uint32_t q = 0; q < 2000u; ++q) {
      const core::Q32 t = mtc::qf_time_q32(q, fps);
      const std::uint64_t whole = t >> 32;
      const bool exact = (t & 0xFFFFFFFFu) == 0u;
      (void)exact;  // a whole boundary finds itself; a fractional one is found from the sample before it
      CHECK(mtc::qf_at_or_after(whole, fps) == q);
      CHECK(mtc::qf_at_or_after(whole + 1u, fps) == q + 1u);
    }
    // A day's worth: the last quarter frame of the day and the wrap.
    const std::uint32_t last = mtc::quarter_frames_per_day(fps) - 1u;
    CHECK(mtc::qf_at_or_after(mtc::qf_time_q32(last, fps) >> 32, fps) <= last + 1u);
  }
  CHECK(mtc::frame_at(44100, 25) == 25u);
  CHECK(mtc::frame_at(44099, 25) == 24u);
  CHECK(mtc::frame_at(44100u * 3600u, 30) == 108000u);
}

TEST_CASE("mtc generator: Off, an invalid rate, and a generator that was never anchored say nothing") {
  mtc::Generator g;
  audio::SeqEvent out[2];
  CHECK_FALSE(g.enabled());
  g.anchor(100, 0);  // Off: nothing
  CHECK_FALSE(g.anchored());
  CHECK(g.step(0, 64, out) == 0u);
  g.configure(29, 1);  // invalid: Off
  CHECK_FALSE(g.enabled());
  g.configure(25, 99);  // the hour is held to 23
  CHECK(g.hour() == 23u);
  CHECK(g.enabled());
  CHECK(g.step(0, 64, out) == 0u);  // not anchored
  audio::SeqEvent stop{};
  CHECK_FALSE(g.stop(10, stop));
  g.configure(25, 1);
  g.anchor(1000, 0);
  CHECK(g.anchored());
  g.configure(0, 1);  // switched off by the settings while running: the owner calls reset(); a step in the meantime says nothing
  CHECK(g.step(960, 64, out) == 0u);
  g.reset();
  CHECK_FALSE(g.anchored());
}

TEST_CASE("mtc generator: a run from the song start at 25 fps: the full frame, then quarter frames on the exact sample, pieces in order, whole cycles, the time of every cycle") {
  if (!kRate44100) {
    return;
  }
  mtc::Generator g;
  g.configure(25, 1);  // MTC START 01
  g.anchor(1000, 0);
  const std::vector<Got> got = run(g, 960, 960 + 2u * kSecond, 64);
  NEED(got.size() > 100u);
  // The first event: the full frame at the anchor's sample, 01:00:00:00 at rate code 1.
  CHECK(got[0].kind == EventKind::MidiMtcFull);
  CHECK(got[0].sample == 1000u);
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{1, 0, 0, 0});
  CHECK(mtc::unpack_code(static_cast<std::uint32_t>(got[0].value)) == 1u);
  // The first quarter frame: the first boundary at least 4 ms (176 samples) after the full frame: boundary 1, at 1000 + 441. Every later one a quarter frame (441 samples) after the last.
  std::uint64_t at = 1000;
  std::uint8_t pieces[8];
  unsigned filled = 0;
  std::uint32_t q = 1;
  std::uint32_t cycles_seen = 0;
  for (std::size_t i = 1; i < got.size(); ++i) {
    CHECK(got[i].kind == EventKind::MidiMtcQf);
    at += 441u;
    CHECK_MESSAGE(got[i].sample == at, "quarter frame " << q);
    CHECK(got[i].value == 0);  // exact at 25 fps: no fraction
    // piece number: (q + the hour's quarter frames) mod 8; the hour's 360,000 is a whole number of cycles
    CHECK((got[i].a >> 4) == (q & 7u));
    CHECK((got[i].a & 0x80u) == 0u);
    if ((q & 7u) == 0u) {
      filled = 0;
    }
    if (filled == (q & 7u)) {
      pieces[filled++] = got[i].a;
    }
    if (filled == 8u) {
      const Decoded d = decode(pieces);
      CHECK(d.ok);
      CHECK(d.code == 1u);
      // The frame of piece 0 (sent at quarter frame q - 7): the time code at the sample piece 0 was sent, on an even frame count. Frame count = 90,000 (the hour) + (q - 7) / 4.
      CHECK(mtc::frame_of_time(d.t, 25) == 90000u + (q - 7u) / 4u);
      CHECK(mtc::frame_of_time(d.t, 25) % 2u == 0u);
      ++cycles_seen;
      filled = 0;
    }
    ++q;
  }
  CHECK(q - 1u == got.size() - 1u);
  CHECK(cycles_seen >= 24u);  // two seconds are 200 quarter frames: 24 whole cycles from piece 0 on
  const std::uint64_t covered_end = 960u + 64u * ((2u * kSecond + 63u) / 64u);  // the last block's end
  CHECK(at < covered_end);                // the last quarter frame is inside the run,
  CHECK(at + 441u >= covered_end);        // and the next one would be past it: none was lost
}

TEST_CASE("mtc generator: 24 and 30 fps put the exact fraction of a sample in `value`") {
  if (!kRate44100) {
    return;
  }
  for (const std::uint32_t fps : {24u, 30u}) {
    mtc::Generator g;
    g.configure(fps, 0);
    g.anchor(5000, 0);
    const std::vector<Got> got = run(g, 4992, 4992 + 4u * kSecond, 64);  // 4 s: 384 quarter frames at 24 fps, 480 at 30
    NEED(got.size() > 300u);
    CHECK(got[0].kind == EventKind::MidiMtcFull);
    CHECK(got[0].sample == 5000u);
    CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{0, 0, 0, 0});
    const core::Q32 per_q = mtc::qf_time_q32(1, fps);  // 459.375 or 367.5 samples
    for (std::size_t i = 1; i < got.size(); ++i) {
      const std::uint64_t k = i;  // quarter frame number: the first boundary after 4 ms is number 1 at both rates (459.375 and 367.5 are above 176)
      // exact time = sample + value / 2^32, in Q32 against the anchor + k periods
      const std::int64_t exact = static_cast<std::int64_t>(got[i].sample << 32) + got[i].value;
      CHECK_MESSAGE(exact == static_cast<std::int64_t>((std::uint64_t{5000u} << 32) + per_q * k), fps << " fps, quarter frame " << k);
      CHECK((got[i].a >> 4) == (k & 7u));
    }
  }
}

TEST_CASE("mtc generator: the events do not depend on the block size") {
  if (!kRate44100) {
    return;
  }
  const auto collect = [](std::uint32_t n) {
    mtc::Generator g;
    g.configure(30, 1);
    g.anchor(7001, 123456);
    return run(g, 7001 - 7001u % n, 7001 + kSecond, n);
  };
  const std::vector<Got> a = collect(64);
  for (const std::uint32_t n : {1u, 7u, 17u, 100u, 367u}) {
    const std::vector<Got> b = collect(n);
    NEED(a.size() > 100u);
    NEED(b.size() > 100u);
    for (std::size_t i = 0; i < a.size() && i < b.size() && a[i].sample + 1000u < 7001u + kSecond; ++i) {  // (the end of the run differs with the block size)
      CHECK(a[i].sample == b[i].sample);
      CHECK(a[i].value == b[i].value);
      CHECK(a[i].a == b[i].a);
      CHECK(a[i].kind == b[i].kind);
    }
  }
}

TEST_CASE("mtc generator: an anchor in the middle of a song starts at that song time, the first boundary is the exact one after 4 ms") {
  if (!kRate44100) {
    return;
  }
  mtc::Generator g;
  g.configure(30, 0);
  const std::uint32_t song = 3u * 44100u + 123u;  // 3.0028 s: frame 90 = 00:00:03:00
  g.anchor(5000, song);
  const std::vector<Got> got = run(g, 4992, 4992 + 4096, 64);
  NEED(got.size() >= 3u);
  CHECK(got[0].kind == EventKind::MidiMtcFull);
  CHECK(got[0].sample == 5000u);
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{0, 0, 3, 0});
  // song time 132,423 + 176 = 132,599 samples: the first boundary at or after it is quarter frame 361 (361 x 367.5 = 132,667.5), 244.5 samples after the anchor: sample 5244.5, nearest 5245, value -0.5
  CHECK(got[1].kind == EventKind::MidiMtcQf);
  CHECK(got[1].sample == 5245u);
  CHECK(got[1].value == INT32_MIN);
  CHECK((got[1].a >> 4) == (361u & 7u));
  CHECK(got[2].sample == 5245u + 367u);  // 5612, the second 5612.0
  CHECK(got[2].value == 0);
  CHECK((got[2].a >> 4) == (362u & 7u));
  // The decoded time of the cycle that holds quarter frame 361: its piece 0 is quarter frame 360 (frame 90, 00:00:03:00)
  CHECK(mtc::qf_byte(360, 30) == 0x00);
  CHECK(mtc::qf_byte(361, 30) == 0x10);
  CHECK(mtc::qf_byte(363, 30) == 0x30);
  CHECK(got[1].a == mtc::qf_byte(361, 30));
}

TEST_CASE("mtc generator: a relocation replaces the anchor: a new full frame and a new time, nothing of the old run") {
  if (!kRate44100) {
    return;
  }
  mtc::Generator g;
  g.configure(25, 1);
  g.anchor(1000, 0);
  std::vector<Got> got = run(g, 960, 960 + 4096, 64);
  CHECK(got.size() > 5u);
  // Relocated at sample 10,000 to song time 30 s.
  g.anchor(10000, 30u * 44100u);
  got = run(g, 9984, 9984 + 2048, 64);
  NEED(got.size() >= 3u);
  CHECK(got[0].kind == EventKind::MidiMtcFull);
  CHECK(got[0].sample == 10000u);
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{1, 0, 30, 0});
  CHECK(got[1].kind == EventKind::MidiMtcQf);
  CHECK(got[1].sample == 10441u);  // boundary at or after 30 s + 176 samples: 30 s is a boundary (441 x 3000), the next is 441 later
  const std::uint32_t q = 30u * 100u + 1u;  // quarter frame 3001 of the song
  CHECK(got[1].a == mtc::qf_byte(q + 360000u, 25));
  // The old run's next quarter frame is gone.
  for (const Got& e : got) {
    CHECK(e.sample >= 10000u);
  }
}

TEST_CASE("mtc generator: a stop gives one full frame with the stop time, then silence until the next anchor") {
  if (!kRate44100) {
    return;
  }
  mtc::Generator g;
  g.configure(25, 1);
  g.anchor(1000, 0);
  run(g, 960, 960 + 3u * 44100u, 64);
  audio::SeqEvent e{};
  const std::uint64_t at = 1000u + 44100u + 22050u;  // 1.5 s after the start
  CHECK(g.stop(at, e));
  CHECK(e.kind == EventKind::MidiMtcFull);
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(e.value)) == mtc::Time{1, 0, 1, 12});  // frame 37 of 25 fps: 1 s 12 frames
  CHECK(mtc::unpack_code(static_cast<std::uint32_t>(e.value)) == 1u);
  CHECK_FALSE(g.anchored());
  CHECK_FALSE(g.stop(at + 1u, e));  // said once
  audio::SeqEvent out[2];
  CHECK(g.step(at, 64, out) == 0u);
  // A stop before the full frame went out says nothing (nothing was said).
  g.anchor(50000, 0);
  CHECK_FALSE(g.stop(50000, e));
  // And the next run starts clean.
  g.anchor(60000, 0);
  const std::vector<Got> got = run(g, 59968, 59968 + 1024, 64);
  NEED(got.size() >= 2u);
  CHECK(got[0].kind == EventKind::MidiMtcFull);
  CHECK(got[0].sample == 60000u);
}

TEST_CASE("mtc generator: a count-in carries the time before the song start; no time below 00:00:00:00 is sent") {
  if (!kRate44100) {
    return;
  }
  // MTC START 01, a count-in of 2 s: the code at the anchor is 00:59:58:00 and runs up to 01:00:00:00 at the first real step.
  mtc::Generator g;
  g.configure(25, 1);
  g.anchor(1000, -2 * static_cast<std::int64_t>(kSecond));
  std::vector<Got> got = run(g, 960, 960 + 3u * kSecond, 64);
  NEED(got.size() > 200u);
  CHECK(got[0].kind == EventKind::MidiMtcFull);
  CHECK(got[0].sample == 1000u);
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{0, 59, 58, 0});
  // The first quarter frame of 01:00:00:00 is a whole 2 s after the anchor, at sample 1000 + 88200.
  bool at_zero = false;
  for (const Got& e : got) {
    at_zero = at_zero || (e.kind == EventKind::MidiMtcQf && e.sample == 1000u + 2u * kSecond && (e.a >> 4) == (2u * 100u & 7u));
  }
  CHECK(at_zero);
  // MTC START 00: the same count-in sends nothing before 00:00:00:00: the anchor waits 2 s, the code then starts at the first real step.
  g.configure(25, 0);
  g.anchor(1000, -2 * static_cast<std::int64_t>(kSecond));
  got = run(g, 960, 960 + 3u * kSecond, 64);
  NEED(got.size() > 50u);
  CHECK(got[0].kind == EventKind::MidiMtcFull);
  CHECK(got[0].sample == 1000u + 2u * kSecond);
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{0, 0, 0, 0});
  for (const Got& e : got) {
    CHECK(e.sample >= 1000u + 2u * kSecond);
  }
  // A count-in longer than MTC START's hour partly: 01 and 0.5 s in front of 00:00:00:00 gives 00:59:59:12 (25 fps: 12 frames) and later.
  g.configure(25, 1);
  g.anchor(500, -static_cast<std::int64_t>(kSecond / 2));
  got = run(g, 448, 448 + 256, 64);
  NEED(!got.empty());
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{0, 59, 59, 12});
}

TEST_CASE("mtc generator: the time code wraps at 24 hours") {
  if (!kRate44100) {
    return;
  }
  mtc::Generator g;
  g.configure(30, 23);  // MTC START 23: the song's first hour reaches 24:00:00:00 after one hour
  const std::uint32_t song = 3600u * 44100u - 12000u;  // 12,000 samples (0.27 s) before the song's first hour ends
  g.anchor(500, song);
  const std::vector<Got> got = run(g, 448, 448 + 16384, 64);
  NEED(got.size() > 10u);
  CHECK(got[0].kind == EventKind::MidiMtcFull);
  CHECK(mtc::unpack_time(static_cast<std::uint32_t>(got[0].value)) == mtc::Time{23, 59, 59, 21});  // 12,000 samples are 8.16 frames before 24:00:00:00: frame 107,991 of the hour
  std::uint8_t pieces[8];
  unsigned filled = 0;
  bool saw_midnight = false;
  bool saw_before = false;
  for (std::size_t i = 1; i < got.size(); ++i) {
    const unsigned piece = got[i].a >> 4;
    if (piece == 0u) {
      filled = 0;
    }
    if (filled == piece) {
      pieces[filled++] = got[i].a;
    }
    if (filled == 8u) {
      const Decoded d = decode(pieces);
      CHECK(d.ok);
      saw_midnight = saw_midnight || (d.t.h == 0u && d.t.m == 0u && d.t.s == 0u);
      saw_before = saw_before || (d.t.h == 23u && d.t.m == 59u && d.t.s == 59u);
      CHECK(d.t.h <= 23u);
      filled = 0;
    }
  }
  CHECK(saw_before);
  CHECK(saw_midnight);
}

TEST_CASE("mtc generator: a run longer than the rebase stays exact, no quarter frame lost or moved") {
  if (!kRate44100) {
    return;
  }
  // 25 fps: a quarter frame every 441 samples exactly; the base moves forward every 2^20 of them (about 2.9 hours).
#if defined(__arm__)
  constexpr std::uint32_t kCount = 100000u;  // the target thins the loop; the host crosses the rebase
#else
  constexpr std::uint32_t kCount = (1u << 20) + 3000u;
#endif
  mtc::Generator g;
  g.configure(25, 1);
  g.anchor(100, 0);
  std::uint64_t expect = 100u + 441u;
  std::uint32_t q = 1;
  std::uint32_t wrong = 0;
  for (std::uint64_t bs = 64; q <= kCount; bs += 441u) {
    audio::SeqEvent out[2];
    const std::uint32_t c = g.step(bs, 441, out);
    for (std::uint32_t i = 0; i < c; ++i) {
      if (out[i].kind == EventKind::MidiMtcFull) {
        continue;
      }
      if (bs + out[i].offset != expect || out[i].value != 0 || (out[i].a >> 4) != (q & 7u)) {
        ++wrong;
      }
      expect += 441u;
      ++q;
    }
  }
  CHECK(wrong == 0u);
  CHECK(q > kCount);
}

// ---- the router --------------------------------------------------------------------------------------------------------------

namespace {

audio::SeqEvent mev(EventKind k, std::uint16_t offset, std::uint8_t a = 0, std::int32_t value = 0) {
  audio::SeqEvent e{};
  e.offset = offset;
  e.kind = k;
  e.track = audio::kNoTrack;
  e.a = a;
  e.value = value;
  return e;
}

struct Wire {
  std::uint8_t bytes[64];
  std::uint64_t start[64];
  std::uint32_t n = 0;
};

Wire drain(midi::DinEmitter& em, std::uint64_t now = 0) {
  Wire w;
  for (int i = 0; i < 300; ++i) {
    const std::uint64_t t = em.next_wake(now);
    if (t == midi::kNever) {
      break;
    }
    now = t;
    const midi::DinEmitter::Tx tx = em.poll(now);
    if (tx.valid && w.n < 64u) {
      w.bytes[w.n] = tx.byte;
      w.start[w.n] = tx.start;
      ++w.n;
    }
  }
  return w;
}

constexpr std::uint64_t block_end(std::uint64_t block_start) { return core::cycles_from_samples(block_start + 64u); }

midi::MidiOut::Config mtc_config(std::uint8_t ports) {
  midi::MidiOut::Config c;
  c.note_ports = midi::kPortDin;
  c.clock_ports = 0;
  c.mtc_ports = ports;
  return c;
}

}  // namespace

TEST_CASE("mtc out: a quarter frame is F1 0nnndddd on the MTC ports, placed to a fraction of a sample, as one USB word with code index 2") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  out.configure(mtc_config(midi::kPortDin | midi::kPortUsb));  // the clock ports are empty: CLOCK MODE Off still sends time code (A-1)
  out.attach(&din, &usb);
  // sample 5001 - 0.5 (the nearest sample is 5001, the exact time half a sample before it)
  CHECK(out.send(mev(EventKind::MidiMtcQf, 0, 0x5B, static_cast<std::int32_t>(0x80000000u)), 5001));
  CHECK(out.staged() == 2u);
  out.flush(block_end(5000));
  const Wire w = drain(din);
  NEED(w.n == 2u);
  CHECK(w.bytes[0] == 0xF1);
  CHECK(w.bytes[1] == 0x5B);
  CHECK(w.start[0] == core::cycles_from_samples(5000) + core::cycles_from_samples(1) / 2u);  // 5000.5 samples
  std::uint32_t words[16];
  CHECK(usb.frame(core::cycles_from_samples(5001) + 180000u, words, 16) == 1u);
  CHECK(words[0] == (0x02u | (0xF1u << 8) | (0x5Bu << 16)));
  CHECK(words[0] == midi::usb_event(0, midi::msg2(midi::kQuarterFrame, 0x5B)));
  // A data byte is 7 bits whatever the event holds.
  CHECK(out.send(mev(EventKind::MidiMtcQf, 0, 0xFF), 6000));
  out.flush(block_end(6000));
  const Wire w2 = drain(din, w.start[1] + 1);
  NEED(w2.n == 2u);
  CHECK(w2.bytes[1] == 0x7F);
}

TEST_CASE("mtc out: no MTC port sends nothing and consumes the event; the export's suppression too; the clock's ports are not the MTC's") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config c = mtc_config(0);
  c.clock_ports = midi::kPortDin | midi::kPortUsb;  // the clock goes out, the time code does not
  out.configure(c);
  out.attach(&din, &usb);
  CHECK(out.send(mev(EventKind::MidiMtcQf, 0, 0x12), 100));
  CHECK(out.send(mev(EventKind::MidiMtcFull, 0, 0, static_cast<std::int32_t>(mtc::pack_full({1, 0, 0, 0}, 1))), 100));
  CHECK(out.staged() == 0u);
  out.flush(block_end(100));
  CHECK(drain(din).n == 0u);
  std::uint32_t words[16];
  CHECK(usb.frame(~std::uint64_t{0} >> 2, words, 16) == 0u);
  // The other way: MTC on, clock ports empty (CLOCK MODE Off), a clock event goes nowhere and the time code goes out.
  out.configure(mtc_config(midi::kPortDin));
  CHECK(out.send(mev(EventKind::MidiClock, 0), 200));
  CHECK(out.staged() == 0u);
  CHECK(out.send(mev(EventKind::MidiMtcQf, 0, 0x12), 200));
  CHECK(out.staged() == 1u);
  out.flush(block_end(200));
  CHECK(drain(din).n == 2u);
  // The export: nothing is queued, both kinds are counted as suppressed.
  out.set_suppress(true);
  CHECK(out.send(mev(EventKind::MidiMtcQf, 0, 0x12), 300));
  CHECK(out.send(mev(EventKind::MidiMtcFull, 0, 0, 0), 300));
  CHECK(out.staged() == 0u);
  CHECK(out.stats().suppressed == 2u);
}

TEST_CASE("mtc out: a full frame is SysEx on DIN (F0 7F 7F 01 01 hh mm ss ff F7) and four USB words in order, due at its sample") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  out.configure(mtc_config(midi::kPortDin | midi::kPortUsb));
  out.attach(&din, &usb);
  const std::int32_t packed = static_cast<std::int32_t>(mtc::pack_full({1, 23, 45, 13}, 1));
  CHECK(out.send(mev(EventKind::MidiMtcFull, 3, 0, packed), 4000));
  CHECK(out.staged() == 2u);
  out.flush(block_end(4000));
  const Wire w = drain(din);
  const std::uint8_t want[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, 0x21, 23, 45, 13, 0xF7};
  NEED(w.n == 10u);
  for (unsigned i = 0; i < 10; ++i) {
    CHECK_MESSAGE(w.bytes[i] == want[i], "DIN byte " << i);
  }
  CHECK(w.start[0] == core::cycles_from_samples(4003));
  std::uint32_t words[16];
  CHECK(usb.frame(core::cycles_from_samples(4003) - 1u, words, 16) == 0u);  // never early
  CHECK(usb.frame(core::cycles_from_samples(4003) + 180000u, words, 16) == 4u);
  CHECK(words[0] == (0x04u | (0xF0u << 8) | (0x7Fu << 16) | (0x7Fu << 24)));
  CHECK(words[1] == (0x04u | (0x01u << 8) | (0x01u << 16) | (0x21u << 24)));
  CHECK(words[2] == (0x04u | (23u << 8) | (45u << 16) | (13u << 24)));
  CHECK(words[3] == (0x05u | (0xF7u << 8)));
  CHECK(out.staged() == 0u);
}

TEST_CASE("mtc out: a full frame goes to a port whole or not at all: a USB ring with room for three keeps it staged until there is room for four") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  out.configure(mtc_config(midi::kPortUsb));
  out.attach(&din, &usb);
  // Fill the USB message ring to 253 of 256: room for three.
  for (std::uint32_t i = 0; i < midi::usb_out::kRingSize - 3u; ++i) {
    NEED(usb.push(0, midi::usb_event(0, midi::note_on(0, 60, 1))));
  }
  CHECK(usb.room() == 3u);
  CHECK(out.send(mev(EventKind::MidiMtcFull, 0, 0, static_cast<std::int32_t>(mtc::pack_full({1, 0, 0, 0}, 1))), 100));
  out.flush(block_end(100));
  CHECK(out.staged() == 1u);        // kept
  CHECK(usb.room() == 3u);          // none of its four words went in
  CHECK(out.stats().port_full == 1u);
  // The wire drains, then the next flush hands it over whole and behind what was there.
  std::uint32_t words[16];
  std::uint32_t drained = 0;
  while (usb.room() < 4u) {
    drained += usb.frame(~std::uint64_t{0} >> 2, words, 16);
  }
  CHECK(drained >= 1u);
  out.flush(block_end(100));
  CHECK(out.staged() == 0u);
  std::uint32_t total = 0;
  std::uint32_t last4[4] = {0, 0, 0, 0};
  for (;;) {
    const std::uint32_t n = usb.frame(~std::uint64_t{0} >> 2, words, 16);
    if (n == 0u) {
      break;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      last4[0] = last4[1];
      last4[1] = last4[2];
      last4[2] = last4[3];
      last4[3] = words[i];
      ++total;
    }
  }
  CHECK(total >= 4u);
  CHECK(last4[0] == (0x04u | (0xF0u << 8) | (0x7Fu << 16) | (0x7Fu << 24)));
  CHECK(last4[3] == (0x05u | (0xF7u << 8)));
}

TEST_CASE("mtc out: the OUT OFFSET of each port moves the time code, a full frame included") {
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  midi::MidiOut::Config c = mtc_config(midi::kPortDin | midi::kPortUsb);
  c.offset_cycles[0] = midi::offset_cycles(25);   // DIN +2.5 ms
  c.offset_cycles[1] = midi::offset_cycles(-10);  // USB -1 ms
  out.configure(c);
  out.attach(&din, &usb);
  CHECK(out.send(mev(EventKind::MidiMtcQf, 0, 0x33), 8000));
  CHECK(out.send(mev(EventKind::MidiMtcFull, 0, 0, static_cast<std::int32_t>(mtc::pack_full({0, 0, 0, 0}, 3))), 8000));
  out.flush(core::cycles_from_samples(8000) + 4u * 180000u);
  const Wire w = drain(din);
  NEED(w.n == 12u);  // the quarter frame (2 bytes) and the full frame (10)
  CHECK(w.start[0] == core::cycles_from_samples(8000) + 25u * 18000u);
  std::uint32_t words[16];
  CHECK(usb.frame(core::cycles_from_samples(8000) - 180000u - 1u, words, 16) == 0u);
  CHECK(usb.frame(core::cycles_from_samples(8000) - 180000u + 1u, words, 16) == 5u);  // advanced by 1 ms: the quarter frame and the four words
}

TEST_CASE("mtc out: over a run with no notes the USB words are at most a frame late, never deferred, and DIN starts every quarter frame on its time") {
  if (!kRate44100) {
    return;
  }
  midi::DinEmitter din;
  midi::UsbMidiOut usb;
  midi::MidiOut out;
  out.configure(mtc_config(midi::kPortDin | midi::kPortUsb));
  out.attach(&din, &usb);
  mtc::Generator g;
  g.configure(30, 1);  // the densest: 120 quarter frames a second
  g.anchor(256, 0);
  std::uint64_t next_frame = 0;  // CPU cycles of the next USB frame
  std::uint32_t usb_events = 0;
  std::vector<std::uint64_t> din_due;
  std::uint32_t din_qf = 0;
  const std::uint64_t blocks = 3u * 44100u / 64u;
  for (std::uint64_t b = 0; b < blocks; ++b) {
    const std::uint64_t bs = 192u + b * 64u;
    // the wire stays at least a block behind the render
    for (; next_frame <= core::cycles_from_samples(bs); next_frame += 180000u) {
      std::uint32_t words[16];
      usb_events += usb.frame(next_frame, words, 16);
    }
    audio::SeqEvent evs[2];
    const std::uint32_t n = g.step(bs, 64, evs);
    for (std::uint32_t i = 0; i < n; ++i) {
      CHECK(out.send(evs[i], bs));
      if (evs[i].kind == EventKind::MidiMtcQf) {
        din_due.push_back(core::cycles_from_samples_sfrac(bs + evs[i].offset, evs[i].value));
      }
    }
    out.flush(core::cycles_from_samples(bs + 64u));
    // the DIN emitter runs when it is due, in step with the blocks
    for (;;) {
      const std::uint64_t t = din.next_wake(core::cycles_from_samples(bs));
      if (t == midi::kNever || t > core::cycles_from_samples(bs + 64u)) {
        break;
      }
      const midi::DinEmitter::Tx tx = din.poll(t);
      if (tx.valid && tx.byte == 0xF1) {
        NEED(din_qf < din_due.size());
        CHECK(tx.start >= din_due[din_qf]);
        CHECK(tx.start - din_due[din_qf] <= 180000u);  // at most 1 ms late; with no notes and no clock it is on its time
        ++din_qf;
      }
    }
  }
  CHECK(din_qf >= 300u);
  CHECK(usb_events >= 300u);
  CHECK(usb.stats().lateness_max <= 180000u);  // at most the 1 ms frame
  CHECK(usb.stats().deferred == 0u);
  CHECK(out.stats().staging_overflow == 0u);
  CHECK(out.stats().port_full == 0u);
}
