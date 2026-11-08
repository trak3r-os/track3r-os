// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEWTENV W8 (docs/specs/OS-CEWTENV.md F3, 3.9): the user contours of /Envelopes (app/wt_user.h). Files are built in memory (a WAV writer here),
// served by the in-memory card (tests/support/mem_card.h), loaded through the real WAV importer and read back through the audio layer's published
// pointer (audio::wt_eval_shape of the user rows 128..255), which is what the render sees.
#include <doctest/doctest.h>

#include <o3os/app/wt_user.h>
#include <o3os/audio/wt_contour.h>

#include "support/mem_card.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using test::MemCard;

namespace {

audio::WtBank g_bank;  // statics: 64 KB and 83 KB never go on a stack
app::WtUserMem g_mem;

void rig() {
  audio::wt_bind(g_bank);
  audio::wt_user_bind(g_mem.bank);
}

// One WAV file: `tag` 1 PCM or 3 float, `bits` 8/16/24/32, `values` interleaved (8 bit: the unsigned byte; 16, 24: the signed value; 32: the raw bits).
std::vector<std::uint8_t> wav(unsigned tag, unsigned channels, unsigned bits, const std::vector<std::int32_t>& values) {
  std::vector<std::uint8_t> out;
  const auto u16 = [&](unsigned v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  const auto u32 = [&](std::uint32_t v) {
    u16(v & 0xFFFFu);
    u16(v >> 16);
  };
  const auto tag4 = [&](const char* t) { out.insert(out.end(), t, t + 4); };
  const unsigned bytes = bits / 8u;
  const std::uint32_t data = static_cast<std::uint32_t>(values.size() * bytes);
  tag4("RIFF");
  u32(36u + data);
  tag4("WAVE");
  tag4("fmt ");
  u32(16);
  u16(tag);
  u16(channels);
  u32(44100);
  u32(44100u * channels * bytes);
  u16(channels * bytes);
  u16(bits);
  tag4("data");
  u32(data);
  for (const std::int32_t v : values) {
    const std::uint32_t raw = static_cast<std::uint32_t>(v);
    for (unsigned b = 0; b < bytes; ++b) {
      out.push_back(static_cast<std::uint8_t>(raw >> (8u * b)));
    }
  }
  return out;
}

std::vector<std::int32_t> ramp16(unsigned frames, double from, double to) {
  std::vector<std::int32_t> v(frames);
  for (unsigned k = 0; k < frames; ++k) {
    v[k] = static_cast<std::int32_t>(std::lround(from + (to - from) * k / (frames > 1 ? frames - 1 : 1)));
  }
  return v;
}

void put_file(MemCard& card, const char* name, const std::vector<std::uint8_t>& bytes) {
  card.make_dir_now("Envelopes");
  card.put((std::string("Envelopes/") + name).c_str(), bytes);
}

// The 128 points the render sees for user slot `slot`, sampled at the phases of a point (phase i/127 evaluates to the point itself).
std::vector<float> seen(unsigned slot) {
  audio::wt_make_bound(audio::kWtContourFactoryCount + slot);
  std::vector<float> v(audio::kWtContourLen);
  for (unsigned i = 0; i < audio::kWtContourLen; ++i) {
    v[i] = audio::wt_eval_shape(audio::kWtContourFactoryCount + slot, static_cast<float>(i) / 127.0f, 0);
  }
  return v;
}

// The level a point list has at the same phases.
std::vector<float> eval_of(const std::vector<std::int16_t>& points) {
  std::vector<float> v(audio::kWtContourLen);
  for (unsigned i = 0; i < audio::kWtContourLen; ++i) {
    v[i] = audio::wt_eval(points.data(), static_cast<float>(i) / 127.0f, 0);
  }
  return v;
}

void check_points(unsigned slot, const std::vector<std::int16_t>& expect) {
  const std::vector<float> got = seen(slot);
  const std::vector<float> want = eval_of(expect);
  for (unsigned i = 0; i < audio::kWtContourLen; ++i) {
    CHECK(static_cast<double>(got[i]) == doctest::Approx(static_cast<double>(want[i])).epsilon(1e-6));
  }
}

// The points of a ramp file, direct from the samples: 16-bit mono with 128 frames resamples to itself.
std::vector<std::int16_t> points_of(const std::vector<std::int32_t>& v) {
  std::vector<std::int16_t> p(v.size());
  for (std::size_t i = 0; i < v.size(); ++i) {
    p[i] = static_cast<std::int16_t>(v[i]);
  }
  return p;
}

}  // namespace

TEST_CASE("wt user: the file name rules") {
  unsigned n = 99;
  CHECK(app::wt_user_parse_name("001.wav", n));
  CHECK(n == 1u);
  CHECK(app::wt_user_parse_name("128.WAV", n));
  CHECK(n == 128u);
  CHECK(app::wt_user_parse_name("7.Wav", n));
  CHECK(n == 7u);
  CHECK(app::wt_user_parse_name("000.wav", n));
  CHECK(n == 0u);
  CHECK_FALSE(app::wt_user_parse_name("0001.wav", n));  // four digits
  CHECK_FALSE(app::wt_user_parse_name("a01.wav", n));
  CHECK_FALSE(app::wt_user_parse_name("01.wave", n));
  CHECK_FALSE(app::wt_user_parse_name("01.wa", n));
  CHECK_FALSE(app::wt_user_parse_name(".wav", n));
  CHECK_FALSE(app::wt_user_parse_name("01", n));
  CHECK_FALSE(app::wt_user_parse_name(nullptr, n));
}

TEST_CASE("wt user: resampling to 128 points, unipolar and bipolar") {
  std::int16_t out[128];
  // 128 frames: the points are the frames.
  std::vector<std::int16_t> f(128);
  for (unsigned i = 0; i < 128; ++i) {
    f[i] = static_cast<std::int16_t>(i * 250);
  }
  app::wt_user_make_points(f.data(), 128, out);
  for (unsigned i = 0; i < 128; ++i) {
    CHECK(out[i] == f[i]);
  }
  // 3000 frames of a ramp 0..29990: point i is the ramp at i * 2999 / 127.
  std::vector<std::int16_t> g(3000);
  for (unsigned k = 0; k < 3000; ++k) {
    g[k] = static_cast<std::int16_t>(k * 10);
  }
  app::wt_user_make_points(g.data(), 3000, out);
  for (unsigned i = 0; i < 128; ++i) {
    CHECK(std::abs(out[i] - 10.0 * i * 2999.0 / 127.0) <= 2.0);
  }
  CHECK(out[0] == 0);
  CHECK(out[127] == 29990);
  // Bipolar: -1..1 maps to 0..1.
  std::vector<std::int16_t> b(3000);
  for (unsigned k = 0; k < 3000; ++k) {
    b[k] = static_cast<std::int16_t>(-20000 + static_cast<int>(40000.0 * k / 2999.0));
  }
  app::wt_user_make_points(b.data(), 3000, out);
  for (unsigned i = 0; i < 128; ++i) {
    const double src = -20000.0 + 40000.0 * (i * 2999.0 / 127.0) / 2999.0;
    CHECK(std::abs(out[i] - (src + 32768.0) / 2.0) <= 3.0);
  }
  // A unipolar file that only touches 0, or dips to -2 (dither), stays unipolar.
  std::vector<std::int16_t> d(128, 0);
  d[5] = -2;
  d[6] = 20000;
  app::wt_user_make_points(d.data(), 128, out);
  CHECK(out[5] == 0);
  CHECK(out[6] == 20000);
  // One frame: a constant contour. Silence stays 0.
  const std::int16_t one = 12345;
  app::wt_user_make_points(&one, 1, out);
  CHECK(out[0] == 12345);
  CHECK(out[127] == 12345);
}

TEST_CASE("wt user: 8, 16 and 24 bit files, a stereo file takes its left channel") {
  rig();
  MemCard card;
  const std::vector<std::int32_t> r16 = ramp16(128, 0, 32000);
  put_file(card, "001.wav", wav(1, 1, 16, r16));
  // 8 bit: unsigned bytes 128..255 are 0..32512.
  std::vector<std::int32_t> b8(128);
  std::vector<std::int16_t> e8(128);
  for (unsigned i = 0; i < 128; ++i) {
    b8[i] = 128 + static_cast<std::int32_t>(i);
    e8[i] = static_cast<std::int16_t>(i * 256);
  }
  put_file(card, "002.wav", wav(1, 1, 8, b8));
  // 24 bit with a zero low byte: exact.
  std::vector<std::int32_t> v24(128);
  for (unsigned i = 0; i < 128; ++i) {
    v24[i] = r16[i] * 256;
  }
  put_file(card, "003.wav", wav(1, 1, 24, v24));
  // 24 bit with a real low byte: the import's dither moves a point by at most 2.
  std::vector<std::int32_t> w24(128);
  for (unsigned i = 0; i < 128; ++i) {
    w24[i] = r16[i] * 256 + 77;
  }
  put_file(card, "004.wav", wav(1, 1, 24, w24));
  // Stereo: left is the ramp, right is the reverse ramp (and bipolar), the contour is the left.
  std::vector<std::int32_t> st(256);
  for (unsigned i = 0; i < 128; ++i) {
    st[2 * i] = r16[i];
    st[2 * i + 1] = -r16[i] - 5000;
  }
  put_file(card, "005.wav", wav(1, 2, 16, st));

  const app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK(rep.folder);
  CHECK_FALSE(rep.zero_based);
  CHECK(rep.loaded == 5);
  CHECK(rep.skipped() == 0);
  CHECK(rep.refused_float == 0);
  check_points(0, points_of(r16));
  check_points(1, e8);
  check_points(2, points_of(r16));
  const std::vector<float> got = seen(3);
  const std::vector<float> want = eval_of(points_of(r16));
  for (unsigned i = 0; i < 128; ++i) {
    CHECK(std::abs(got[i] - want[i]) <= 3.0f / 32768.0f);
  }
  check_points(4, points_of(r16));
  CHECK(audio::wt_user_is_file(0));
  CHECK_FALSE(audio::wt_user_is_file(5));
}

TEST_CASE("wt user: a long file is resampled, a bipolar file is mapped to 0..1") {
  rig();
  MemCard card;
  put_file(card, "010.wav", wav(1, 1, 16, ramp16(3000, 0, 29990)));
  put_file(card, "011.wav", wav(1, 1, 16, ramp16(3000, -20000, 20000)));
  const app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK(rep.loaded == 2);
  const std::vector<float> a = seen(9);
  const std::vector<float> b = seen(10);
  for (unsigned i = 0; i < 128; ++i) {
    CHECK(std::abs(static_cast<double>(a[i]) - 10.0 * i * 2999.0 / 127.0 / 32767.0) <= 3.0 / 32767.0);
    const double src = -20000.0 + 40000.0 * i / 127.0;
    CHECK(std::abs(static_cast<double>(b[i]) - (src + 32768.0) / 2.0 / 32767.0) <= 4.0 / 32767.0);
  }
}

TEST_CASE("wt user: 1-based numbering, a missing number keeps the fallback") {
  rig();
  const std::vector<float> fallback = seen(1);  // U002 before any load
  MemCard card;
  put_file(card, "1.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));
  put_file(card, "003.wav", wav(1, 1, 16, ramp16(128, 1000, 20000)));
  put_file(card, "128.wav", wav(1, 1, 16, ramp16(128, 0, 10000)));
  put_file(card, "notes.txt", std::vector<std::uint8_t>{1, 2, 3});
  const app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK_FALSE(rep.zero_based);
  CHECK(rep.loaded == 3);
  CHECK(audio::wt_user_is_file(0));
  CHECK(audio::wt_user_is_file(2));
  CHECK(audio::wt_user_is_file(127));
  CHECK_FALSE(audio::wt_user_is_file(1));
  CHECK(seen(1) == fallback);  // U002: no 002.wav, still the procedural contour
  check_points(2, points_of(ramp16(128, 1000, 20000)));
}

TEST_CASE("wt user: 0-based numbering when 000.wav or 0.wav exists") {
  for (const char* zero : {"000.wav", "0.wav"}) {
    rig();
    MemCard card;
    put_file(card, zero, wav(1, 1, 16, ramp16(128, 0, 30000)));
    put_file(card, "127.wav", wav(1, 1, 16, ramp16(128, 0, 12000)));
    put_file(card, "128.wav", wav(1, 1, 16, ramp16(128, 0, 12000)));  // past U128 in a 0-based folder
    const app::WtUserReport rep = app::wt_user_load(card, g_mem);
    CHECK(rep.zero_based);
    CHECK(rep.loaded == 2);
    CHECK(rep.out_of_range == 1);
    CHECK(audio::wt_user_is_file(0));    // 000.wav is U001
    CHECK(audio::wt_user_is_file(127));  // 127.wav is U128
    check_points(0, points_of(ramp16(128, 0, 30000)));
  }
}

TEST_CASE("wt user: a float WAV is refused and reported, the other files still load") {
  rig();
  const std::vector<float> fallback = seen(1);
  MemCard card;
  float f = 0.5f;
  std::int32_t bits = 0;
  std::memcpy(&bits, &f, 4);
  put_file(card, "001.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));
  put_file(card, "002.wav", wav(3, 1, 32, std::vector<std::int32_t>(128, bits)));
  put_file(card, "003.wav", wav(1, 1, 16, ramp16(128, 0, 20000)));
  const app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK(rep.loaded == 2);
  CHECK(rep.refused_float == 1);
  CHECK(rep.skipped() == 0);
  CHECK_FALSE(audio::wt_user_is_file(1));
  CHECK(seen(1) == fallback);
  char text[64];
  CHECK(app::wt_user_notice(rep, text, sizeof text) > 0);
  CHECK(std::string(text) == "ENVELOPES: 2 LOADED, 1 FLOAT");
}

TEST_CASE("wt user: a bad file never stops the others") {
  rig();
  MemCard card;
  put_file(card, "001.wav", std::vector<std::uint8_t>{'n', 'o', 'p', 'e'});               // not a WAV
  put_file(card, "002.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));
  put_file(card, "003.wav", wav(1, 1, 16, ramp16(24577, 0, 30000)));                      // one frame too long
  put_file(card, "004.wav", wav(1, 1, 16, ramp16(24576, 0, 30000)));                      // the longest taken
  put_file(card, "500.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));                        // past U128
  put_file(card, "005.wav", wav(1, 1, 16, std::vector<std::int32_t>{}));                  // no frames
  const app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK(rep.loaded == 2);  // 002 and 004
  CHECK(rep.too_long == 1);
  CHECK(rep.out_of_range == 1);
  CHECK(rep.bad == 2);     // 001 (not a WAV) and 005 (no frames)
  CHECK(audio::wt_user_is_file(1));
  CHECK(audio::wt_user_is_file(3));
  CHECK_FALSE(audio::wt_user_is_file(0));
  CHECK_FALSE(audio::wt_user_is_file(2));
  CHECK(card.open_handles == 0);
}

TEST_CASE("wt user: a second file for one number is counted, and a slot that lost its file goes back to the fallback") {
  rig();
  const std::vector<float> fallback = seen(0);
  MemCard card;
  put_file(card, "001.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));
  put_file(card, "1.wav", wav(1, 1, 16, ramp16(128, 0, 10000)));
  app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK(rep.loaded == 1);
  CHECK(rep.duplicate == 1);
  CHECK(seen(0) != fallback);
  // The card changes: no file now.
  MemCard empty;
  empty.make_dir_now("Envelopes");
  rep = app::wt_user_load(empty, g_mem);
  CHECK(rep.folder);
  CHECK(rep.loaded == 0);
  CHECK(rep.restored == 1);
  CHECK_FALSE(audio::wt_user_is_file(0));
  CHECK(seen(0) == fallback);
  // No folder at all: nothing says anything, a slot is still restored when it held a file.
  rep = app::wt_user_load(card, g_mem);
  CHECK(rep.loaded == 1);
  MemCard none;
  rep = app::wt_user_load(none, g_mem);
  CHECK_FALSE(rep.folder);
  CHECK(rep.restored == 1);
  CHECK(seen(0) == fallback);
  char text[64];
  CHECK(app::wt_user_notice(rep, text, sizeof text) == 0);
  CHECK(text[0] == '\0');
}

TEST_CASE("wt user: the strip's text") {
  char text[64];
  app::WtUserReport r;
  CHECK(app::wt_user_notice(r, text, sizeof text) == 0);
  r.folder = true;
  CHECK(app::wt_user_notice(r, text, sizeof text) == 0);  // an empty folder says nothing
  r.loaded = 12;
  r.refused_float = 1;
  r.bad = 2;
  r.duplicate = 1;
  r.restored = 4;
  CHECK(app::wt_user_notice(r, text, sizeof text) > 0);
  CHECK(std::string(text) == "ENVELOPES: 12 LOADED, 1 FLOAT, 3 SKIPPED, 4 RESET");
  char small[12];
  const std::size_t n = app::wt_user_notice(r, small, sizeof small);
  CHECK(n == std::strlen(small));
  CHECK(n < sizeof small);
}

TEST_CASE("wt user: the pointer swap, a reload replaces the contour the render reads") {
  rig();
  MemCard a;
  put_file(a, "001.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));
  app::wt_user_load(a, g_mem);
  const std::vector<float> first = seen(0);
  MemCard b;
  put_file(b, "001.wav", wav(1, 1, 16, ramp16(128, 5000, 9000)));
  app::wt_user_load(b, g_mem);
  const std::vector<float> second = seen(0);
  CHECK(first != second);
  check_points(0, points_of(ramp16(128, 5000, 9000)));
}

TEST_CASE("wt user: a truncated file and a file just over the size limit are skipped, the others load") {
  rig();
  MemCard card;
  std::vector<std::uint8_t> cut = wav(1, 1, 16, ramp16(1000, 0, 30000));
  cut.resize(44u + 300u);  // the header says 2000 data bytes, the file ends after 300
  put_file(card, "001.wav", cut);
  // 24-bit stereo: 24,920 frames are 149,520 data bytes, over kWtUserMaxFileBytes (149,504) and over the frame limit.
  CHECK(24920u * 6u > app::kWtUserMaxFileBytes);
  put_file(card, "002.wav", wav(1, 2, 24, ramp16(24920u * 2u, 0, 30000)));
  put_file(card, "003.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));
  const app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK(rep.too_long == 1);          // 002
  CHECK(rep.loaded + rep.bad == 2);  // 001 (however the importer takes a cut file) and 003
  CHECK(audio::wt_user_is_file(2));  // 003 loads whatever happened to 001
  CHECK_FALSE(audio::wt_user_is_file(1));
  CHECK(card.open_handles == 0);
}

TEST_CASE("wt user: a card error while listing keeps the loaded contours") {
  rig();
  MemCard card;
  put_file(card, "001.wav", wav(1, 1, 16, ramp16(128, 0, 30000)));
  app::WtUserReport rep = app::wt_user_load(card, g_mem);
  CHECK(rep.loaded == 1);
  const std::vector<float> loaded = seen(0);
  card.files.clear();                              // the folder stays, the file is gone
  card.readdir_fail_at = card.readdir_calls + 1u;  // pass 1 ends at its first readdir, pass 2's first readdir fails
  rep = app::wt_user_load(card, g_mem);
  CHECK(rep.restored == 0);
  CHECK(audio::wt_user_is_file(0));
  CHECK(seen(0) == loaded);
  rep = app::wt_user_load(card, g_mem);  // the card answers again: the slot goes back
  CHECK(rep.restored == 1);
  CHECK_FALSE(audio::wt_user_is_file(0));
}
