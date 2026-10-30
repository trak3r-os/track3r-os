// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sampler voices (audio/voices.h, OS-005d lite): the pitch increment and the interpolation against a double-precision reference, and the placement of
// a note inside a block, sample for sample, through the engine (a scripted event source, the real graph behind it). The end-to-end test of the demo
// pattern is audio_demo_test.cpp. Host and Cortex-M4F (`qemu_audio`).
#include <doctest/doctest.h>

#include <o3os/audio/engine.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "data/loop_walk_vectors.h"
#include "support/need.h"
#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

namespace haudio = o3os::hal::audio;
using o3os::audio::Engine;
using o3os::audio::EventKind;
using o3os::audio::SampleDesc;
using o3os::audio::SeqEvent;
using o3os::audio::VoicePool;

constexpr std::uint32_t kBlk = haudio::kBlock;

VoicePool& pool() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

// Renders `blocks` whole blocks of one voice pool (no events inside a block: the notes are started between blocks) into out[blocks * 64], the feed of `track` (0 unless said).
void render_blocks(VoicePool& p, std::int16_t* out, std::uint32_t blocks, unsigned track = 0) {
  for (std::uint32_t b = 0; b < blocks; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(track);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out[b * kBlk + i] = f != nullptr ? f[i] : std::int16_t{0};
    }
  }
}

// The attack ramp the voice puts on the first kAttackSamples samples at velocity 127: the amplitude of sample k is k * 2048 / 65536.
double attack(std::uint32_t k) { return k < o3os::audio::kAttackSamples ? static_cast<double>(k) / 32.0 : 1.0; }

// A sample of `n` points of a sine of amplitude 20000 and 37.3 periods over the sample, as int16 (a signal with every kind of slope).
void make_sine(std::int16_t* s, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    s[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * 37.3 * static_cast<double>(i) / static_cast<double>(n))));
  }
}

// The reference: linear interpolation of the sample at position `pos` (in samples), a forward loop [ls, le) when le > ls, in double.
double reference(const std::int16_t* s, std::uint32_t length, std::uint32_t ls, std::uint32_t le, double pos) {
  const bool loop = le > ls;
  const std::uint32_t end = loop ? le : length;
  if (loop && pos >= static_cast<double>(end)) {
    pos = static_cast<double>(ls) + std::fmod(pos - static_cast<double>(ls), static_cast<double>(le - ls));
  }
  const std::uint32_t i = static_cast<std::uint32_t>(pos);
  const double f = pos - static_cast<double>(i);
  std::uint32_t next = i + 1u;
  if (next >= end) {
    next = loop ? ls : i;
  }
  return static_cast<double>(s[i]) + (static_cast<double>(s[next]) - static_cast<double>(s[i])) * f;
}

}  // namespace

TEST_CASE("voices: the pitch increment is 2^((note - 60) / 12) in Q32.32, for all 128 notes") {
  using o3os::audio::pitch_inc_q32;
  CHECK(pitch_inc_q32(60) == (std::uint64_t{1} << 32));  // the root: exactly one sample a sample
  CHECK(pitch_inc_q32(72) == (std::uint64_t{2} << 32));  // octaves are exact shifts
  CHECK(pitch_inc_q32(48) == (std::uint64_t{1} << 31));
  CHECK(pitch_inc_q32(0) == (std::uint64_t{1} << 27));
  double worst = 0.0;
  for (unsigned note = 0; note < 128; ++note) {
    const double ref = std::exp2((static_cast<double>(note) - 60.0) / 12.0);
    const double got = static_cast<double>(pitch_inc_q32(note)) / 4294967296.0;
    const double err = std::fabs(got / ref - 1.0);
    worst = err > worst ? err : worst;
  }
  CHECK_MESSAGE(worst < 1e-8, "worst relative error " << worst);  // 1.7e-5 cent: the Q30 table and the shifted-out bits of the lowest octave
  CHECK(pitch_inc_q32(127) == pitch_inc_q32(200));                 // out of range is clamped, not wrapped
}

TEST_CASE("voices: the interpolation matches a double reference at ratios from 1/4 to 4, one shot and loop") {
  static std::int16_t sine[2000];
  static std::int16_t out[8 * kBlk];
  make_sine(sine, 2000);
  struct Case {
    unsigned note;
    std::uint32_t ls, le;
  };
  const Case cases[] = {{36, 0, 0}, {55, 0, 0}, {60, 0, 0}, {64, 0, 0}, {72, 0, 0}, {84, 0, 0}, {67, 100, 400}, {50, 20, 120}};
  for (const Case& c : cases) {
    VoicePool& p = pool();
    p.set_sample(0, SampleDesc{sine, 2000, c.ls, c.le, 65536});
    p.note_on(0, c.note, 0, 127);
    render_blocks(p, out, 8);
    const double ratio = std::exp2((static_cast<double>(c.note) - 60.0) / 12.0);
    std::int32_t worst = 0;
    for (std::uint32_t k = 0; k < 8 * kBlk; ++k) {
      const double pos = static_cast<double>(k) * ratio;
      if (c.le == 0 && pos >= 1999.0) {
        break;  // a one shot ends here
      }
      const double expect = reference(sine, 2000, c.ls, c.le, pos) * attack(k);
      const std::int32_t err = static_cast<std::int32_t>(std::lround(std::fabs(static_cast<double>(out[k]) - expect)));
      worst = err > worst ? err : worst;
    }
    CHECK_MESSAGE(worst <= 1, "note " << c.note << " loop " << c.ls << ".." << c.le << ": worst error " << worst << " LSB");
  }
}

TEST_CASE("voices: the instrument's tune shifts the note, its fine tune moves toward the neighbour semitone, and the voice plays it") {
  using o3os::audio::pitch_inc_q32;
  using o3os::audio::tuned_inc_q32;
  for (unsigned note = 0; note < 128; ++note) {
    CHECK(tuned_inc_q32(note, 0, 0) == pitch_inc_q32(note));  // an untuned instrument is what it was
  }
  CHECK(tuned_inc_q32(60, 12, 0) == pitch_inc_q32(72));
  CHECK(tuned_inc_q32(60, -24, 0) == pitch_inc_q32(36));
  CHECK(tuned_inc_q32(120, 24, 0) == pitch_inc_q32(127));  // the tune is clamped into the table, not wrapped
  CHECK(tuned_inc_q32(5, -24, 0) == pitch_inc_q32(0));
  // The fine tune against the reference formula, in double: pitch[n] + fine * (pitch[n +- 1] - pitch[n]) / 100, for every note, both signs, a few values.
  for (unsigned note = 1; note < 127; ++note) {
    for (const int fine : {-100, -37, -1, 1, 50, 100}) {
      const double base = static_cast<double>(pitch_inc_q32(note));
      const double other = static_cast<double>(pitch_inc_q32(fine > 0 ? note + 1 : note - 1));
      const double want = base + std::abs(fine) * (other - base) / 100.0;
      const double got = static_cast<double>(tuned_inc_q32(note, 0, fine));
      CHECK_MESSAGE(std::fabs(got - want) <= want * 2e-6, "note " << note << " fine " << fine);
    }
  }
  CHECK(tuned_inc_q32(127, 0, 50) == pitch_inc_q32(127));  // past either end of the table: no fine tune
  CHECK(tuned_inc_q32(0, 0, -50) == pitch_inc_q32(0));
  CHECK(tuned_inc_q32(60, 0, 500) == tuned_inc_q32(60, 0, 100));  // the fine tune is clamped to +-100

  // Through a voice: a sample tuned up an octave at the root plays what an untuned one does an octave above, sample for sample.
  static std::int16_t sine[2000];
  static std::int16_t tuned[4 * kBlk];
  static std::int16_t plain[4 * kBlk];
  make_sine(sine, 2000);
  VoicePool& p = pool();
  SampleDesc d{sine, 2000, 0, 0, 65536};
  d.tune = 12;
  p.set_sample(0, d);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, tuned, 4);
  VoicePool& q = pool();  // a fresh pool over the same storage
  q.set_sample(1, SampleDesc{sine, 2000, 0, 0, 65536});
  q.note_on(0, 72, 1, 127);
  render_blocks(q, plain, 4);
  for (std::uint32_t k = 0; k < 4 * kBlk; ++k) {
    CHECK(tuned[k] == plain[k]);
  }
}

// OS-007h: beat slice and wavetable. A slice table is slice_count + 1 offsets as two u16 halves (low, high).
static void put_bounds(std::uint16_t* table, std::initializer_list<std::uint32_t> bounds) {
  std::uint32_t i = 0;
  for (const std::uint32_t b : bounds) {
    table[2 * i] = static_cast<std::uint16_t>(b & 0xFFFFu);
    table[2 * i + 1] = static_cast<std::uint16_t>(b >> 16);
    ++i;
  }
}

TEST_CASE("voices: a beat slice plays the slice the note names, at the root's pitch whatever the note (OS-007h)") {
  static std::int16_t ramp[1000];
  static std::uint16_t table[2 * 5];
  static std::int16_t out[4 * kBlk];
  for (std::uint32_t i = 0; i < 1000; ++i) {
    ramp[i] = static_cast<std::int16_t>(static_cast<int>(i) * 30 - 15000);  // every sample told apart, no step above 30
  }
  put_bounds(table, {0, 250, 600, 600, 1000});  // slices 0 [0, 250), 1 [250, 600), 2 empty, 3 [600, 1000)
  const auto desc = [&](int tune) {
    SampleDesc d{ramp, 1000, 0, 0, 65536};
    d.tune = static_cast<std::int8_t>(tune);
    d.beat_slice = true;
    d.slice_count = 4;
    d.slices = table;
    return d;
  };
  struct Case {
    unsigned note;
    std::uint32_t first;  // the sample the note's slice starts on
    std::uint32_t len;
  };
  // note - 24 is the slice, clamped; notes 36 and 100 are not an octave and four up, they are the last slice at the root's pitch.
  const Case cases[] = {{24, 0, 250}, {25, 250, 350}, {0, 0, 250}, {23, 0, 250}, {27, 600, 400}, {36, 600, 400}, {100, 600, 400}};
  for (const Case& c : cases) {
    VoicePool& p = pool();
    p.set_sample(0, desc(0));
    p.note_on(0, c.note, 0, 127);
    render_blocks(p, out, 4);
    std::uint32_t wrong = 0;
    const std::uint32_t n = c.len < 4 * kBlk ? c.len : 4 * kBlk;
    for (std::uint32_t k = o3os::audio::kAttackSamples; k < n; ++k) {
      wrong += out[k] == ramp[c.first + k] ? 0u : 1u;
    }
    CHECK_MESSAGE(wrong == 0u, "note " << c.note << ": " << wrong << " samples are not the slice's");
    // the slice is a one shot: the voice is over where the slice is (and the voices read nothing past it)
    CHECK_MESSAGE(p.track_playing(0) == (c.len >= 4 * kBlk), "note " << c.note);
  }
  // The instrument's tune still applies: tuned up an octave, a slice is read twice as fast.
  {
    VoicePool& p = pool();
    p.set_sample(0, desc(12));
    p.note_on(0, 25, 0, 127);
    render_blocks(p, out, 4);
    for (std::uint32_t k = o3os::audio::kAttackSamples; k < 170; ++k) {
      CHECK(out[k] == ramp[250 + 2 * k]);
    }
  }
  // A slice with no length plays nothing, and the note still ends the one that was sounding on the track (the note replaces it).
  {
    VoicePool& p = pool();
    p.set_sample(0, desc(0));
    p.note_on(0, 24, 0, 127);
    render_blocks(p, out, 1);
    CHECK(p.track_playing(0));
    const std::uint32_t ignored = p.stats().ignored;
    p.note_on(0, 26, 0, 127);
    CHECK(p.stats().ignored == ignored + 1u);
    CHECK_FALSE(p.track_playing(0));
  }
  // An end before the start (a table out of order) runs to the end of the sample (this OS's rule); nothing is read past it.
  {
    static std::uint16_t bad[2 * 3];
    put_bounds(bad, {900, 100, 1000});
    VoicePool& p = pool();
    SampleDesc d = desc(0);
    d.slice_count = 2;
    d.slices = bad;
    p.set_sample(0, d);
    p.note_on(0, 24, 0, 127);
    render_blocks(p, out, 4);
    for (std::uint32_t k = o3os::audio::kAttackSamples; k < 100; ++k) {
      CHECK(out[k] == ramp[900 + k]);
    }
    CHECK_FALSE(p.track_playing(0));  // the slice (100 samples) is over and the voice with it
  }
  // A table that is not there, or too long, is dropped: the whole sample is the slice, still at the root's pitch.
  {
    VoicePool& p = pool();
    SampleDesc d = desc(0);
    d.slice_count = 49;
    p.set_sample(0, d);
    CHECK(p.sample(0).slice_count == 0u);
    CHECK(p.sample(0).slices == nullptr);
    CHECK(p.sample(0).beat_slice);
    p.note_on(0, 77, 0, 127);
    render_blocks(p, out, 4);
    for (std::uint32_t k = o3os::audio::kAttackSamples; k < 4 * kBlk; ++k) {
      CHECK(out[k] == ramp[k]);
    }
  }
}

TEST_CASE("voices: the wavetable pitch law is the note table times window x 523.2511306 / 44100, and a window plays as an oscillator (OS-007h)") {
  using o3os::audio::pitch_inc_q32;
  using o3os::audio::wavetable_inc_q32;
  const double k = 523.2511306011972 / 44100.0;
  double worst = 0.0;
  for (const unsigned window : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u}) {
    for (unsigned note = 0; note < 120; ++note) {
      const double want = std::exp2((static_cast<double>(note) - 60.0) / 12.0) * window * k * 4294967296.0;
      const double got = static_cast<double>(wavetable_inc_q32(pitch_inc_q32(note), window));
      worst = std::fmax(worst, std::fabs(got / want - 1.0));
    }
  }
  CHECK_MESSAGE(worst < 5e-8, "worst relative error " << worst);  // two units of 2^-32 at the smallest increment (5e7 units: note 0, window 32), the constant's 1e-9, the Q30 table
  // A window of 256 cycles of a sine: C-5 is 523.25 Hz at any window size, so the voice makes 523.25 / 44100 cycles a sample.
  static std::int16_t win[256];
  for (std::uint32_t i = 0; i < 256; ++i) {
    win[i] = static_cast<std::int16_t>(std::lround(15000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 256.0)));
  }
  static std::int16_t out[8 * kBlk];
  for (const unsigned note : {60u, 48u, 69u}) {
    VoicePool& p = pool();
    SampleDesc d{win, 256, 0, 256, 65536};
    d.wt_size = 256;
    p.set_sample(0, d);
    p.note_on(0, note, 0, 127);
    render_blocks(p, out, 8);
    const double inc = std::exp2((static_cast<double>(note) - 60.0) / 12.0) * 256.0 * k;
    std::int32_t worst_lsb = 0;
    for (std::uint32_t i = o3os::audio::kAttackSamples; i < 8 * kBlk; ++i) {
      const double expect = reference(win, 256, 0, 256, static_cast<double>(i) * inc);
      const std::int32_t err = static_cast<std::int32_t>(std::lround(std::fabs(static_cast<double>(out[i]) - expect)));
      worst_lsb = err > worst_lsb ? err : worst_lsb;
    }
    CHECK_MESSAGE(worst_lsb <= 2, "note " << note << ": worst error " << worst_lsb << " LSB");
    CHECK(p.track_playing(0));  // an oscillator: it sounds until the note ends
  }
  // A window with no loop is an ordinary sample (the loader always gives one; the voices do not trust it).
  {
    VoicePool& p = pool();
    SampleDesc d{win, 256, 0, 0, 65536};
    d.wt_size = 256;
    p.set_sample(0, d);
    CHECK(p.sample(0).wt_size == 0u);
  }
}

TEST_CASE("voices: a one shot that ends fades its last value to zero without a step, and frees the voice") {
  static std::int16_t dc[100];
  for (std::int16_t& s : dc) {
    s = 12000;
  }
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{dc, 100, 0, 0, 65536});
  p.note_on(0, 60, 0, 127);
  static std::int16_t out[4 * kBlk];
  render_blocks(p, out, 4);
  CHECK(out[99] == 12000);  // the last sample of the one shot
  std::int32_t biggest_step = 0;
  for (std::uint32_t k = 100; k < 4 * kBlk; ++k) {
    const std::int32_t d = static_cast<std::int32_t>(out[k - 1]) - static_cast<std::int32_t>(out[k]);
    biggest_step = d > biggest_step ? d : biggest_step;
    CHECK(out[k] >= 0);
  }
  CHECK_MESSAGE(biggest_step <= 12000 / 64 + 1, "step " << biggest_step);  // the residual walks to zero over kDeclickSamples
  CHECK(out[100 + o3os::audio::kDeclickSamples] == 0);
  CHECK(out[4 * kBlk - 1] == 0);
  CHECK(p.voices_in_use() == 0u);
  CHECK(p.feed(0) == nullptr);  // nothing sounds: no feed
}

// ---- OS-007i: the instrument volume law and the headroom above unity ----------------------------------------------------------

TEST_CASE("voices: the instrument volume law: 1.2+ is 0 silent, 1 is -24 dB, 0.48 dB a level, 50 unity, 100 +24 dB; older projects keep the 1.1.14 table") {
  using o3os::audio::level_gain_q16;
  using o3os::audio::LevelLaw;
  CHECK(level_gain_q16(0, LevelLaw::Og12) == 0);
  CHECK(level_gain_q16(50, LevelLaw::Og12) == 65536);  // the stored default of every 1.2+ instrument is 0 dB, exactly
  CHECK(level_gain_q16(1, LevelLaw::Og12) == 4135);    // 10^(-24 / 20) x 65536 = 4135.04
  CHECK(level_gain_q16(100, LevelLaw::Og12) == 1038676);  // +24 dB: 10^(24 / 20) x 65536 = 1038675.6
  CHECK(level_gain_q16(100, LevelLaw::Og12) < o3os::audio::kernel::kMaxGain);
  double worst_lsb = 0.0;
  double worst_db = 0.0;
  for (unsigned l = 1; l <= 100; ++l) {
    const double db = l == 1 ? -24.0 : 0.48 * (static_cast<double>(l) - 50.0);  // crosscheck P2-1: level 1 is -24.00 dB, then 0.48 dB a level from 2
    const double want = std::pow(10.0, db / 20.0) * 65536.0;
    const double got = static_cast<double>(level_gain_q16(l, LevelLaw::Og12));
    worst_lsb = std::fmax(worst_lsb, std::fabs(got - want));
    worst_db = std::fmax(worst_db, std::fabs(20.0 * std::log10(got / want)));
    CHECK(level_gain_q16(l, LevelLaw::Og12) > level_gain_q16(l - 1, LevelLaw::Og12));
  }
  CHECK_MESSAGE(worst_lsb <= 1.0, "worst error " << worst_lsb << " Q16 LSB");
  CHECK_MESSAGE(worst_db <= 0.01, "worst error " << worst_db << " dB");
  // the 0.48 dB step is the law's: 51 over 50 and 100 over 99, and the old piecewise shape (24/49 dB a step below 50) is gone: level 2 is -23.04 dB, not -23.51
  CHECK(std::fabs(20.0 * std::log10(static_cast<double>(level_gain_q16(51, LevelLaw::Og12)) / 65536.0) - 0.48) < 0.001);
  CHECK(std::fabs(20.0 * std::log10(static_cast<double>(level_gain_q16(2, LevelLaw::Og12)) / 65536.0) + 23.04) < 0.001);
  // 1.1.14 (projects older than 1.2): 0 silent, 10^((level - 100) / 50), 100 unity, 50 is -20 dB
  CHECK(level_gain_q16(0, LevelLaw::Og1114) == 0);
  CHECK(level_gain_q16(100, LevelLaw::Og1114) == 65536);
  for (unsigned l = 1; l <= 100; ++l) {
    const double want = std::pow(10.0, (static_cast<double>(l) - 100.0) / 50.0) * 65536.0;
    CHECK(std::fabs(static_cast<double>(level_gain_q16(l, LevelLaw::Og1114)) - want) <= 1.0);
  }
  CHECK(level_gain_q16(250, LevelLaw::Og12) == level_gain_q16(100, LevelLaw::Og12));  // clamped, not wrapped
  CHECK(level_gain_q16(250, LevelLaw::Og1114) == 65536);
}

TEST_CASE("voices: a full-scale sample at level 100 (+24 dB) saturates at the rails and never wraps; the instrument's gain is what a note without a velocity gets") {
  static std::int16_t square[256];
  for (std::uint32_t i = 0; i < 256; ++i) {
    square[i] = static_cast<std::int16_t>(i % 2 == 0 ? 32767 : -32768);
  }
  using o3os::audio::level_gain_q16;
  using o3os::audio::LevelLaw;
  VoicePool& p = pool();
  SampleDesc d{square, 256, 0, 256, level_gain_q16(100, LevelLaw::Og12)};
  p.set_sample(0, d);
  p.note_on(0, 60, 0, o3os::audio::kVelocityDefault);
  static std::int16_t out[4 * kBlk];
  render_blocks(p, out, 4);
  for (std::uint32_t k = 0; k < 4 * kBlk; ++k) {
    // the root's pitch: sample k is square[k % 256] (the loop is the whole sample, 256 samples, an even count)
    if (k != 0) {
      CHECK_MESSAGE(((out[k] > 0) == (k % 2 == 0)), "sample " << k << " is " << out[k]);
    }
    if (k >= o3os::audio::kAttackSamples) {
      CHECK(out[k] == (k % 2 == 0 ? 32767 : -32768));  // 15.85x of full scale: the rails
    }
  }
  // below unity and at unity nothing changes: the 32-bit product and the 64-bit one agree (the demo kit's gain is the default, unity)
  static std::int16_t dc[600];
  for (std::int16_t& s : dc) {
    s = 20000;
  }
  p.set_sample(1, SampleDesc{dc, 600, 0, 600, level_gain_q16(50, LevelLaw::Og12)});
  p.set_sample(2, SampleDesc{dc, 600, 0, 600, level_gain_q16(25, LevelLaw::Og12)});
  p.note_on(0, 60, 1, o3os::audio::kVelocityDefault);
  render_blocks(p, out, 3);
  p.cut(0);
  CHECK(out[2 * kBlk] == 20000);  // level 50: 0 dB
  p.note_on(0, 60, 2, o3os::audio::kVelocityDefault);
  render_blocks(p, out, 3);
  p.cut(0);
  CHECK(std::abs(out[2 * kBlk] - 20000.0 * std::pow(10.0, -12.0 / 20.0)) <= 1.5);  // level 25: -12 dB
  // a velocity a step names is linear and replaces the instrument's gain (open item, voices.h LEVEL): 127 is unity even on a +24 dB instrument
  p.set_sample(3, SampleDesc{dc, 600, 0, 600, level_gain_q16(100, LevelLaw::Og12)});
  p.note_on(0, 60, 3, 127);
  render_blocks(p, out, 3);
  CHECK(out[2 * kBlk] == 20000);
}

// ---- The amp envelope and the loop directions (OS-007k) ------------------------------------------------------------------------

namespace {

using o3os::audio::AmpEnvelope;

struct EnvCfg {
  unsigned delay, attack, hold, decay, release;  // groups of 8 samples
  double sustain;                                // 0..1
};

constexpr std::uint32_t kNever = 0xFFFFFFFFu;

// The envelope level, 0..1, of output sample k of a note released at sample `off`, in double: the stages of voices.h AMP ENVELOPE, each `n x 8` samples. The
// release starts from the level the note has reached at `off`; a release in the delay is the note's end.
double env_level(const EnvCfg& c, std::uint32_t k, std::uint32_t off) {
  const double d = 8.0 * c.delay;
  const double a = 8.0 * c.attack;
  const double h = 8.0 * c.hold;
  const double dc = 8.0 * c.decay;
  const double r = 8.0 * c.release;
  const auto before_release = [&](double t) {
    if (t < d) {
      return 0.0;
    }
    t -= d;
    if (t < a) {
      return t / a;
    }
    t -= a;
    if (t < h) {
      return 1.0;
    }
    t -= h;
    return t < dc ? 1.0 + (c.sustain - 1.0) * t / dc : c.sustain;
  };
  if (k < off) {
    return before_release(k);
  }
  const double from = before_release(off);
  const double t = static_cast<double>(k - off);
  return t < r ? from * (1.0 - t / r) : 0.0;
}

AmpEnvelope make_env(const EnvCfg& c, std::int32_t amount = 65536) {
  AmpEnvelope e;
  e.enable = true;
  e.delay = static_cast<std::uint16_t>(c.delay);
  e.attack = static_cast<std::uint16_t>(c.attack);
  e.hold = static_cast<std::uint16_t>(c.hold);
  e.decay = static_cast<std::uint16_t>(c.decay);
  e.release = static_cast<std::uint16_t>(c.release);
  e.sustain = static_cast<std::uint32_t>(c.sustain * 1073741824.0 > 1073741824.0 ? 1073741824.0 : c.sustain * 1073741824.0);
  e.amount = amount;
  return e;
}

// One note of `slot` on track 0, `blocks` whole blocks, with a NoteOff (or a fade) called before block `off_block` (so at sample 64 x off_block).
void play_env(VoicePool& p, unsigned slot, unsigned velocity, std::int16_t* out, std::uint32_t blocks, std::uint32_t off_block, bool fade = false) {
  p.note_on(0, 60, slot, velocity);
  for (std::uint32_t b = 0; b < blocks; ++b) {
    if (b == off_block) {
      if (fade) {
        p.fade(0);
      } else {
        p.note_off(0);
      }
    }
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out[b * kBlk + i] = f != nullptr ? f[i] : std::int16_t{0};
    }
  }
}

// The track's note ended and its residual gone: two blocks after a cut.
void settle(VoicePool& p) {
  static std::int16_t sink[2 * kBlk];
  p.cut(0);
  render_blocks(p, sink, 2);
}

}  // namespace

TEST_CASE("voices: the amp envelope runs its stages to the sample against a double oracle, holds the sustain, and releases to silence (OS-007k)") {
  static std::int16_t dc[4000];
  for (std::int16_t& v : dc) {
    v = 20000;
  }
  static std::int16_t out[64 * 12];
  const EnvCfg cfg{3, 5, 2, 10, 6, 0.5};  // delay 24, attack 40, hold 16, decay 80 samples; sustain 0.5; release 48 samples
  VoicePool& p = pool();
  SampleDesc d{dc, 4000, 0, 0, 65536};
  d.env = make_env(cfg);
  p.set_sample(0, d);

  play_env(p, 0, o3os::audio::kVelocityDefault, out, 12, 5);  // released at sample 320, in the sustain
  for (std::uint32_t k = 0; k < 64u * 12u; ++k) {
    const double want = 20000.0 * env_level(cfg, k, 320);
    if (k < 368u || k >= 432u) {  // between the end of the release (368) and the end of its 64-sample residual (432) the declick residual is added
      CHECK_MESSAGE(std::fabs(out[k] - want) <= 2.0, "sample " << k << " is " << out[k] << ", the oracle says " << want);
    }
  }
  // The stage edges, in samples: silent through the delay (24) and the first sample of the attack, full level at the hold (64..79), the decay from 80, the
  // sustain (0.5) from 160, the release from 320 down to nothing at 368.
  CHECK(out[23] == 0);
  CHECK(out[24] == 0);
  CHECK(out[25] > 0);
  CHECK(std::abs(out[44] - 10000) <= 2);  // half way up the attack
  CHECK(out[63] < 20000);
  CHECK(std::abs(out[64] - 20000) <= 2);
  CHECK(std::abs(out[79] - 20000) <= 2);
  CHECK(std::abs(out[80] - 20000) <= 2);
  CHECK(out[81] < 19990);
  CHECK(std::fabs(out[159] - 20000.0 * (1.0 - 0.5 * 79.0 / 80.0)) <= 2.0);  // the last sample of the decay
  CHECK(std::abs(out[160] - 10000) <= 2);
  CHECK(std::abs(out[319] - 10000) <= 2);
  CHECK(out[320] == out[319]);
  CHECK(out[321] < out[320]);
  CHECK(std::abs(out[368] - out[367]) <= 8);  // the end of the release is continuous: the last value fades as a residual, no click
  for (std::uint32_t k = 432; k < 64u * 12u; ++k) {
    CHECK(out[k] == 0);
  }
  CHECK(p.voices_in_use() == 0u);
}

TEST_CASE("voices: the envelope's release starts from where the level is (mid attack), a note off in the delay ends the note, a fade releases over 300 ms (OS-007k)") {
  static std::int16_t dc[20000];
  for (std::int16_t& v : dc) {
    v = 20000;
  }
  static std::int16_t out[64 * 220];
  VoicePool& p = pool();

  // mid attack: delay 24, attack 800 samples; released at sample 192, level 0.21; release 160 samples
  const EnvCfg mid{3, 100, 1, 1, 20, 1.0};
  SampleDesc d{dc, 20000, 0, 0, 65536};
  d.env = make_env(mid);
  p.set_sample(0, d);
  play_env(p, 0, o3os::audio::kVelocityDefault, out, 8, 3);
  for (std::uint32_t k = 0; k < 64u * 8u; ++k) {
    if (k < 352u || k >= 416u) {
      CHECK_MESSAGE(std::fabs(out[k] - 20000.0 * env_level(mid, k, 192)) <= 2.0, "sample " << k);
    }
  }
  CHECK(std::abs(out[191] - 20000.0 * (191.0 - 24.0) / 800.0) <= 2.0);
  CHECK(std::fabs(out[192] - 4200.0) <= 2.0);  // the release is from 0.21, not from the full level
  CHECK(out[193] < out[192]);
  CHECK(out[300] > 0);

  // a note off in the delay: the note never sounds, and the voice is free at once
  const EnvCfg late{20, 2, 1, 1, 5, 1.0};  // delay 160 samples
  d.env = make_env(late);
  p.set_sample(1, d);
  p.note_on(0, 60, 1, o3os::audio::kVelocityDefault);
  CHECK(p.voices_in_use() == 1u);
  p.note_off(0);
  CHECK(p.voices_in_use() == 0u);  // nothing had sounded: no residual either
  for (std::uint32_t b = 0; b < 4; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    CHECK(p.feed(0) == nullptr);
  }

  // a fade: 300 ms = 13232 samples (1654 groups of 8) at 44,100 Hz (13240 and 1655 at 44,117.647 Hz), from the sustain at unity, whatever the release time says
  constexpr std::uint32_t kFadeSamples = o3os::audio::kFadeEnvUnits * o3os::audio::kEnvGroup;
  const EnvCfg quick{1, 1, 1, 1, 3, 1.0};
  d.env = make_env(quick);
  p.set_sample(2, d);
  play_env(p, 2, o3os::audio::kVelocityDefault, out, 220, 2, true);  // faded at sample 128
  for (std::uint32_t t = 0; t < kFadeSamples; t += 331u) {
    CHECK_MESSAGE(std::fabs(out[128 + t] - 20000.0 * (1.0 - static_cast<double>(t) / static_cast<double>(kFadeSamples))) <= 2.0, "fade sample " << t);
  }
  CHECK(out[128 + kFadeSamples - 1u] < 4);
  CHECK(p.voices_in_use() == 0u);  // out of the release and its residual, within the 220 blocks

  // the same note released normally runs the instrument's release time (24 samples): out in a fraction of the time
  play_env(p, 2, o3os::audio::kVelocityDefault, out, 4, 2);
  CHECK(out[128 + 23] > 0);
  CHECK(out[128 + 24 + 64] == 0);  // 24 samples of release, then the 64-sample residual
}

TEST_CASE("voices: the envelope's amount multiplies the gain, the velocity's included; a disabled envelope is the plain note, bit for bit (OS-007k)") {
  static std::int16_t dc[600];
  for (std::int16_t& v : dc) {
    v = 20000;
  }
  static std::int16_t a[64 * 6];
  static std::int16_t b[64 * 6];
  VoicePool& p = pool();
  const EnvCfg flat{1, 1, 1, 1, 1, 1.0};  // full level from sample 32
  SampleDesc d{dc, 600, 0, 0, 65536};
  d.env = make_env(flat, 16384);  // amount 0.25
  p.set_sample(0, d);
  play_env(p, 0, o3os::audio::kVelocityDefault, a, 6, kNever);
  CHECK(std::abs(a[200] - 5000) <= 1);
  settle(p);
  play_env(p, 0, 64, a, 6, kNever);  // velocity 64 replaces the instrument's gain (33025), amount still applies
  CHECK(std::fabs(a[200] - 20000.0 * (33025.0 * 0.25) / 65536.0) <= 1.5);

  // disabled: the other fields are not read
  SampleDesc plain{dc, 600, 0, 0, 65536};
  SampleDesc off = plain;
  off.env = make_env(EnvCfg{20, 20, 20, 20, 20, 0.1}, 100);
  off.env.enable = false;
  p.set_sample(1, plain);
  p.set_sample(2, off);
  settle(p);
  play_env(p, 1, o3os::audio::kVelocityDefault, a, 6, 4);
  settle(p);
  play_env(p, 2, o3os::audio::kVelocityDefault, b, 6, 4);
  for (std::uint32_t k = 0; k < 64u * 6u; ++k) {
    CHECK(a[k] == b[k]);
  }
  CHECK(a[10] == static_cast<std::int16_t>(std::lround(20000.0 * 10.0 / 32.0)));  // the plain attack ramp is still there
}

namespace {

// The positions a backward loop (mode 2) and a ping-pong loop (mode 3) visit over [20, 100) at a pitch of `step` whole samples: expected values,
// the output of this project's own reference model (maintainer, 2026-10-18), kept as test vectors (data/loop_walk_vectors.h). One deviation, on purpose: the
// backward loop's jump back to the end can land on loop2 itself (the vector then holds the sample after the loop, once, and goes on from
// loop2 - 1); the voices read the loop's last sample there, loop2 - 1, and go on from loop2 - 1, so the loop keeps its length.
const std::uint8_t* og_walk(bool pingpong, int step) {
  for (const o3os::test::vectors::LoopWalk& w : o3os::test::vectors::kLoopWalks) {
    if (w.pingpong == pingpong && w.step == step) {
      return w.pos;
    }
  }
  return nullptr;
}

}  // namespace

// OS-QAFIX finding 24 (the audio matrix reads a zero crossing 0.9 ms late after the first turn of a backward or ping-pong loop): the turn at loop_end reflects the position about loop_end - 0.5, so the
// sound after it is the sound before it, backward, sample for sample, with the last sample twice and no step. The "slip" a zero-crossing reader sees is that reversal of a periodic tone (the crossings
// that were rising are falling), which depends on where the loop ends; it is not a skipped or repeated stretch and not a click. This holds the equality on a 441 Hz sine for both modes.
TEST_CASE("voices: the first turn of a backward and a ping-pong loop mirrors the sound before it, the last sample twice, with no step (finding 24)") {
  static std::int16_t sine[4000];
  for (int i = 0; i < 4000; ++i) {
    sine[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979323846 * i / 100.0)));
  }
  static std::int16_t out[64 * 80];
  for (const bool pingpong : {false, true}) {
    VoicePool& p = pool();
    SampleDesc d{sine, 4000, 0, 3500, 65536};
    d.loop_mode = pingpong ? 2 : 1;
    p.set_sample(0, d);
    p.note_on(0, 60, 0, o3os::audio::kVelocityDefault);
    render_blocks(p, out, 80);
    std::uint32_t wrong = 0;
    for (std::uint32_t k = 0; k < 1000u; ++k) {
      if (out[3500u + k] != out[3499u - k]) {  // sample 3500 + k is the one at 3499 - k: the position reflects about 3499.5
        ++wrong;
      }
    }
    CHECK_MESSAGE(wrong == 0u, "mode " << (pingpong ? 3 : 2) << ": " << wrong << " samples of the backward run differ from the forward run they mirror");
    int widest = 0;  // the largest step between two neighbours over the turn (a 441 Hz sine of 12000 moves at most 760 a sample)
    for (std::uint32_t n = 3400; n < 3600u; ++n) {
      widest = std::max(widest, std::abs(static_cast<int>(out[n + 1u]) - static_cast<int>(out[n])));
    }
    CHECK(widest <= 770);
  }
}

// OS-QAFIX finding 30 (a settled pad differs from the Pattern step by tens of cents on some play modes): a pad and a step go through the same VoicePool::note_on, so the same note at the same
// modulator phase sounds the same bit for bit; what differs is the phase. A modulator that is an LFO starts at the song's current step (mod_start: l_shift = step % 36 steps, "the LFO is locked to
// the step grid"), a pad starts at the step the transport last stopped on and a step at its own row, so a vibrato or a position LFO is at another point of its cycle. A measurement artifact of the pad
// against step comparison, not a pad path defect; this holds both halves.
TEST_CASE("voices: the same note at the same song step is bit for bit the same, and an LFO modulator starts at the step's phase (finding 30)") {
  static std::int16_t sine[2000];
  for (int i = 0; i < 2000; ++i) {
    sine[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979323846 * i / 100.0)));
  }
  static std::int16_t a[64 * 30];
  static std::int16_t b[64 * 30];
  static std::int16_t c[64 * 30];
  const auto play = [&](unsigned step, std::int16_t* out) {
    VoicePool& p = pool();
    SampleDesc d{sine, 2000, 0, 2000, 65536};  // a forward loop: the note goes on
    d.tune_mod.kind = o3os::audio::kFmodLfo;
    d.tune_mod.lfo = true;
    d.tune_mod.shape = 2;
    d.tune_mod.rate = 12;
    d.tune_mod.amount = 1.0f;
    p.set_sample(0, d);
    p.on_step(step);
    p.note_on(0, 60, 0, o3os::audio::kVelocityDefault);
    render_blocks(p, out, 30);
  };
  play(0, a);
  play(0, b);
  play(5, c);
  CHECK(std::memcmp(a, b, sizeof a) == 0);  // one path, one phase: one sound
  CHECK(std::memcmp(a, c, sizeof a) != 0);  // another step of the song: another point of the vibrato's cycle
}

TEST_CASE("voices: a backward loop and a ping-pong loop walk the reference positions, at pitch 1 and 2, and a huge pitch on a tiny loop never reads outside it (OS-007k)") {
  static std::int16_t ramp[120];
  for (int i = 0; i < 120; ++i) {
    ramp[i] = static_cast<std::int16_t>(i * 100);  // the output says where the voice was
  }
  static std::int16_t out[64 * 10];
  for (const bool pingpong : {false, true}) {
    for (const unsigned note : {60u, 72u}) {
      VoicePool& p = pool();
      SampleDesc d{ramp, 120, 20, 100, 65536};
      d.loop_mode = pingpong ? 2 : 1;
      p.set_sample(0, d);
      p.note_on(0, note, 0, o3os::audio::kVelocityDefault);
      render_blocks(p, out, 10);
      const std::uint8_t* walk = og_walk(pingpong, note == 60u ? 1 : 2);
      NEED(walk != nullptr);
      std::uint32_t wrong = 0;
      for (std::uint32_t k = 0; k < 64u * 10u; ++k) {
        const double want = ramp[walk[k]] * attack(k);
        if (std::fabs(out[k] - want) > 1.01) {
          ++wrong;
          CHECK_MESSAGE(false, "mode " << (pingpong ? 3 : 2) << " note " << note << " sample " << k << ": " << out[k] << ", the recorded walk says " << want);
          if (wrong > 5u) {
            break;
          }
        }
      }
      CHECK(wrong == 0u);
      CHECK(p.track_playing(0));  // a loop never ends
    }
  }
  // note 127 is 48x: the loop [20, 24) is 4 samples long, a step is 12 loops. The position stays inside [0, 24) (the sanitizers would say otherwise) and the voice keeps going.
  for (const bool pingpong : {false, true}) {
    VoicePool& p = pool();
    SampleDesc d{ramp, 120, 20, 24, 65536};
    d.loop_mode = pingpong ? 2 : 1;
    p.set_sample(0, d);
    p.note_on(0, 127, 0, o3os::audio::kVelocityDefault);
    render_blocks(p, out, 10);
    CHECK(p.track_playing(0));
    for (std::uint32_t k = 64; k < 64u * 10u; ++k) {
      CHECK(out[k] >= 0);
      CHECK(out[k] <= 2400);  // only samples of [0, 24) (the first few are the intro's and the attack's)
    }
  }
}

// ---- Placement of a note inside a block, through the engine ------------------------------------------------------------------

namespace {

struct Cue {
  std::uint32_t block;
  SeqEvent ev;
};
constexpr std::uint32_t kMaxCues = 12;
Cue g_cues[kMaxCues];
std::uint32_t g_cue_count = 0;
std::uint64_t g_seen[16];
std::uint32_t g_seen_count = 0;

SeqEvent event(std::uint16_t offset, EventKind kind, std::uint8_t note = 0, std::uint8_t slot = 0, std::uint8_t vel = 0) {
  SeqEvent e{};
  e.offset = offset;
  e.kind = kind;
  e.track = 0;
  e.a = note;
  e.b = slot;
  e.c = vel;
  return e;
}

// The scripted event source: the events of g_cues that belong to the block that starts at `block_start`.
std::uint32_t scripted_step(void*, std::uint64_t block_start, std::uint32_t n, SeqEvent* out, std::uint32_t cap) noexcept {
  if (g_seen_count < 16u) {
    g_seen[g_seen_count++] = block_start;
  }
  const std::uint32_t block = static_cast<std::uint32_t>(block_start / n);
  std::uint32_t count = 0;
  for (std::uint32_t i = 0; i < g_cue_count && count < cap; ++i) {
    if (g_cues[i].block == block) {
      out[count++] = g_cues[i].ev;
    }
  }
  return count;
}

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 7u; }

struct Rig {
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  haudio::BlockReport report = {};
  std::int16_t voice[kBlk] = {};  // the voice block of track 0 of the block just rendered (zeros when there is none)
  bool fed = false;
};

void render(Engine& e, VoicePool& p, Rig& r) {
  r.report = haudio::BlockReport{};
  const haudio::BlockIo io{r.in, r.out, 0, &r.report};
  e.render_block(io);
  const std::int16_t* f = p.feed(0);
  r.fed = f != nullptr;
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    r.voice[i] = f != nullptr ? f[i] : std::int16_t{0};
  }
}

}  // namespace

TEST_CASE("voices: a note starts and ends at its exact sample, the attack and release ramps are what they say, and the clock never skips a block") {
  static std::int16_t dc[1000];
  for (std::int16_t& s : dc) {
    s = 16384;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static SeqEvent events[o3os::audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{dc, 1000, 0, 0, 65536});
  e.attach_voices(&p);
  o3os::audio::EventSource src;
  src.step = &scripted_step;
  src.buffer = events;
  src.capacity = o3os::audio::kMaxEventsPerBlock;
  e.attach_events(src);

  g_seen_count = 0;
  g_cue_count = 0;
  g_cues[g_cue_count++] = Cue{2, event(17, EventKind::NoteOn, 60, 0, 127)};  // block 2, sample 17
  g_cues[g_cue_count++] = Cue{3, event(40, EventKind::NoteOff)};             // block 3, sample 40: the release begins there
  Rig r;
  for (std::uint32_t b = 0; b < 2; ++b) {
    render(e, p, r);
    CHECK_FALSE(r.fed);  // no voice yet: the track has no input
  }
  render(e, p, r);  // block 2
  NEED(r.fed);
  for (std::uint32_t i = 0; i <= 17; ++i) {
    CHECK(r.voice[i] == 0);  // silent up to the note's sample (the attack starts from nothing: its first sample is 0)
  }
  for (std::uint32_t k = 0; k < 32; ++k) {
    CHECK_MESSAGE(r.voice[17 + k] == static_cast<std::int16_t>(512 * k), "sample " << 17 + k);  // 16384 * k * 2048 / 65536
  }
  for (std::uint32_t i = 17 + 32; i < kBlk; ++i) {
    CHECK(r.voice[i] == 16384);  // the attack is over
  }
  // The graph made it audible, from the same sample: exactly silent before the note, sound after (a centred track: both channels alike).
  for (std::uint32_t i = 0; i <= 17; ++i) {
    CHECK(r.out[2 * i] == 0);
    CHECK(r.out[2 * i + 1] == 0);
  }
  CHECK(r.out[2 * 63] != 0);
  CHECK(r.out[2 * 63 + 1] != 0);

  render(e, p, r);  // block 3
  for (std::uint32_t i = 0; i < 40; ++i) {
    CHECK(r.voice[i] == 16384);
  }
  for (std::uint32_t k = 0; k < kBlk - 40; ++k) {
    CHECK_MESSAGE(r.voice[40 + k] == static_cast<std::int16_t>(16384 - 64 * static_cast<std::int32_t>(k)), "release sample " << k);  // 256 a sample of 65536
  }
  for (std::uint32_t b = 0; b < 6; ++b) {  // the release is 256 samples long: 24 are in block 3, the rest in 4 to 7, and its 64-sample residual ends in block 8
    render(e, p, r);
  }
  CHECK_FALSE(r.fed);
  CHECK(p.voices_in_use() == 0u);
  CHECK(e.sample_clock() == 64u * 10u);
  NEED(g_seen_count == 10u);
  for (std::uint32_t b = 0; b < g_seen_count; ++b) {
    CHECK(g_seen[b] == 64u * b);  // one step() a block, contiguous, from the first
  }
  CHECK(r.report.stage_cycles[haudio::kStageDrain] != 0u);  // the event source's cycles are in the report
}

namespace {
std::uint32_t g_param_calls = 0;
float g_param_value = 0.0f;
unsigned g_param_field = 0;
bool taking_param(void*, const o3os::core::ParamChange& c) noexcept {
  ++g_param_calls;
  g_param_value = c.value;
  g_param_field = o3os::core::param_field(c.id);
  return o3os::core::param_field(c.id) == 0x200u;  // the sequencer's tempo takes it, anything else is not its own
}
}  // namespace

TEST_CASE("voices: a ParamChange the graph does not own (the sequencer's tempo) reaches the event source; one nobody takes is counted as rejected") {
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  Engine& e = engine_slot.make(&fake_cycles, taps);
  o3os::audio::EventSource src;
  src.self = nullptr;
  src.param = &taking_param;
  e.attach_events(src);  // no step function: the hook alone
  g_param_calls = 0;
  using o3os::core::ParamKind;
  CHECK(e.params().push(o3os::audio::ParamChange{o3os::core::param_id(ParamKind::Project, 0, 0x200), 140.5f, 0u}));  // seq_param::Tempo
  CHECK(e.params().push(o3os::audio::ParamChange{o3os::core::param_id(ParamKind::Project, 0, 0x777), 1.0f, 0u}));   // nobody's
  CHECK(e.params().push(o3os::audio::ParamChange{o3os::audio::strip_param(3, o3os::audio::graph_param::strip::Volume), 80.0f, 0u}));  // the graph's own
  Rig r;
  VoicePool& p = pool();
  render(e, p, r);
  CHECK(g_param_calls == 2u);  // the tempo and the unknown one; the strip's volume never goes to the hook
  CHECK(g_param_field == 0x777u);
  CHECK(e.params_applied() == 3u);
  CHECK(e.graph().params_rejected() == 1u);
}

// ---- The instrument's pan (OS-007k) ---------------------------------------------------------------------------------------------

TEST_CASE("voices: the law's gains at pan 0, 50 and 100, and the instrument's pan reaches the note's track around the strip's own pan (OS-007k)") {
  using o3os::audio::pan_gains_q16;
  CHECK(pan_gains_q16(0.0f).left == 65536);
  CHECK(pan_gains_q16(0.0f).right == 0);
  CHECK(pan_gains_q16(50.0f).left == 65536);
  CHECK(pan_gains_q16(50.0f).right == 65536);
  CHECK(pan_gains_q16(100.0f).left == 0);
  CHECK(pan_gains_q16(100.0f).right == 65536);

  static std::int16_t dc[3000];
  for (std::int16_t& v : dc) {
    v = 16384;
  }
  struct Case {
    std::uint8_t inst;  // the instrument's pan
    float strip;        // the strip's own pan (50: the centre, what an imported OG project gets)
    float effective;    // clamp(strip + inst - 50, 0, 100)
  };
  static constexpr Case kCases[] = {{0, 50.0f, 0.0f}, {100, 50.0f, 100.0f}, {50, 50.0f, 50.0f}, {25, 50.0f, 25.0f}, {75, 25.0f, 50.0f}, {100, 25.0f, 75.0f}, {0, 25.0f, 0.0f}, {50, 80.0f, 80.0f}};
  for (const Case& c : kCases) {
    static o3os::test::StaticSlot<Engine> engine_slot;
    static o3os::audio::TapMem taps;
    static SeqEvent events[o3os::audio::kMaxEventsPerBlock];
    Engine& e = engine_slot.make(&fake_cycles, taps);
    VoicePool& p = pool();
    SampleDesc d{dc, 3000, 0, 0, 65536};
    d.pan = c.inst;
    p.set_sample(0, d);
    e.attach_voices(&p);
    o3os::audio::EventSource src;
    src.step = &scripted_step;
    src.buffer = events;
    src.capacity = o3os::audio::kMaxEventsPerBlock;
    e.attach_events(src);
    g_cue_count = 0;
    g_cues[g_cue_count++] = Cue{1, event(5, EventKind::NoteOn, 60, 0, 127)};
    NEED(e.params().push(o3os::audio::ParamChange{o3os::audio::strip_param(0, o3os::audio::graph_param::strip::Pan), c.strip, 1u}));
    Rig r;
    render(e, p, r);  // block 0: the strip's pan lands
    render(e, p, r);  // block 1: the note starts at sample 5, the pan with it, from the block's first sample
    render(e, p, r);  // block 2: the note at full level
    const o3os::audio::PanGains want = pan_gains_q16(c.effective);
    const std::int32_t l = r.out[2 * 40];
    const std::int32_t rt = r.out[2 * 40 + 1];
    INFO("instrument " << int(c.inst) << " strip " << c.strip << " effective " << c.effective << ": left " << l << " right " << rt);
    CHECK(std::abs(l) + std::abs(rt) > 0);
    // the two channels are in the ratio of the law's gains (cross-multiplied: no division by a zero gain), within the rounding of the two outputs
    const double lhs = static_cast<double>(rt) * want.left;
    const double rhs = static_cast<double>(l) * want.right;
    CHECK(std::fabs(lhs - rhs) <= 2.0 * 65536.0);
    if (want.left == 0) {
      CHECK(l == 0);
    }
    if (want.right == 0) {
      CHECK(rt == 0);
    }
  }
}

// ---- The Slice FX (OS-007k) -----------------------------------------------------------------------------------------------------

TEST_CASE("voices: the Slice FX forces the slice of a beat slice note, value modulo the slice count, until it ends or the FX are reset (OS-007k)") {
  static std::int16_t ramp[1000];
  static std::uint16_t table[2 * 5];
  for (std::uint32_t i = 0; i < 1000; ++i) {
    ramp[i] = static_cast<std::int16_t>(static_cast<int>(i) * 30 - 15000);
  }
  const std::uint16_t bounds[5] = {0, 250, 500, 750, 1000};  // four slices of 250
  for (unsigned i = 0; i < 5; ++i) {
    table[2 * i] = bounds[i];
    table[2 * i + 1] = 0;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static SeqEvent events[o3os::audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool();
  SampleDesc d{ramp, 1000, 0, 0, 65536};
  d.beat_slice = true;
  d.slice_count = 4;
  d.slices = table;
  p.set_sample(0, d);
  e.attach_voices(&p);
  o3os::audio::EventSource src;
  src.step = &scripted_step;
  src.buffer = events;
  src.capacity = o3os::audio::kMaxEventsPerBlock;
  e.attach_events(src);

  using o3os::audio::kModGroupSlice;
  const auto mod = [](std::uint16_t offset, EventKind kind, std::uint8_t value, std::uint8_t layer, std::int32_t perf) {
    SeqEvent ev = event(offset, kind);
    ev.b = value;
    ev.c = layer;
    ev.flags = kModGroupSlice;
    ev.value = perf;
    return ev;
  };
  g_cue_count = 0;
  // block 0: note 24 (slice 0) with no FX; block 2: FX Slice 6 (6 % 4 = slice 2), then the note 24 plays slice 2; block 4: the FX ends and note 24 is slice 0 again;
  // block 6: the performance layer's value 3 forces slice 3; block 8: FxReset, note 25 is slice 1; and a ModSet of another group changes nothing.
  g_cues[g_cue_count++] = Cue{0, event(0, EventKind::NoteOn, 24, 0, 127)};
  g_cues[g_cue_count++] = Cue{2, mod(0, EventKind::ModSet, 6, o3os::audio::kLayerFx1, 0)};
  g_cues[g_cue_count++] = Cue{2, event(1, EventKind::NoteOn, 24, 0, 127)};
  g_cues[g_cue_count++] = Cue{4, mod(0, EventKind::ModEnd, 0, 0, 0)};
  SeqEvent other = mod(0, EventKind::ModSet, 2, o3os::audio::kLayerFx1, 0);
  other.flags = 1;  // the Volume group: not the voices' business here
  g_cues[g_cue_count++] = Cue{4, other};
  g_cues[g_cue_count++] = Cue{4, event(1, EventKind::NoteOn, 24, 0, 127)};
  g_cues[g_cue_count++] = Cue{6, mod(0, EventKind::ModSet, 0, o3os::audio::kLayerPerf, 3)};
  g_cues[g_cue_count++] = Cue{6, event(1, EventKind::NoteOn, 24, 0, 127)};
  g_cues[g_cue_count++] = Cue{8, event(0, EventKind::FxReset)};
  g_cues[g_cue_count++] = Cue{8, event(1, EventKind::NoteOn, 25, 0, 127)};
  Rig r;
  const std::uint32_t first[] = {0, 0, 500, 500, 0, 0, 750, 750, 250};  // the first sample of the slice the note of block b (even) plays
  for (std::uint32_t b = 0; b < 10; ++b) {
    render(e, p, r);
    if (b % 2 == 1) {  // the block after a note started (at sample 0 of block 0, sample 1 of the others): its click-free residual is gone, the slice is audible
      const std::uint32_t at = b == 1 ? 0u : 1u;
      const std::uint32_t j = 10;
      CHECK_MESSAGE(r.voice[j] == ramp[first[b - 1] + (kBlk - at + j)], "block " << b);
    }
  }
}

// ---- The Cutoff FX (OS-007l) --------------------------------------------------------------------------------------------------------

TEST_CASE("voices: the Cutoff FX through the engine: a high-pass at the top cutoff takes the DC out of the sounding note, its end restores the bypass, an FX type that is not 27..29 does nothing (OS-007l)") {
  static std::int16_t dc[1000];
  for (std::int16_t& s : dc) {
    s = 16384;
  }
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static SeqEvent events[o3os::audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{dc, 1000, 0, 0, 65536});
  e.attach_voices(&p);
  o3os::audio::EventSource src;
  src.step = &scripted_step;
  src.buffer = events;
  src.capacity = o3os::audio::kMaxEventsPerBlock;
  e.attach_events(src);

  const auto mod = [](EventKind kind, std::uint8_t fx_type, std::uint8_t value) {
    SeqEvent ev = event(0, kind);
    ev.a = fx_type;
    ev.b = value;
    ev.c = o3os::audio::kLayerFx1;
    ev.flags = o3os::audio::kModGroupFilter;
    return ev;
  };
  g_cue_count = 0;
  g_cues[g_cue_count++] = Cue{0, event(0, EventKind::NoteOn, 60, 0, 127)};
  g_cues[g_cue_count++] = Cue{2, mod(EventKind::ModSet, 28, 100)};  // High-pass 100: the cutoff at the top, 14.9 kHz
  g_cues[g_cue_count++] = Cue{4, mod(EventKind::ModEnd, 0, 0)};
  g_cues[g_cue_count++] = Cue{6, mod(EventKind::ModSet, 30, 100)};  // FX 30 (Delay send) is not a filter FX
  Rig r;
  for (std::uint32_t b = 0; b < 8; ++b) {
    render(e, p, r);
    if (b == 1 || b == 5 || b == 7) {
      CHECK_MESSAGE(r.voice[20] == 16384, "block " << b << ": the plain DC");
      CHECK_MESSAGE(r.voice[kBlk - 1] == 16384, "block " << b << ": the plain DC");
    }
    if (b == 3) {
      CHECK_MESSAGE(std::abs(static_cast<int>(r.voice[kBlk - 1])) <= 8, "block 3: the DC is gone under the high-pass: " << r.voice[kBlk - 1]);
    }
  }
}

TEST_CASE("voices: the live voice's position is one word: sounding, the slot + 1, the samples played; it ends with the note and follows no other") {
  static std::int16_t sine[4000];
  static std::int16_t out[4 * kBlk];
  make_sine(sine, 4000);
  VoicePool& p = pool();
  p.set_sample(3, SampleDesc{sine, 4000, 0, 0, 65536});
  CHECK(p.live_position() == 0u);
  CHECK(p.note_on(0, 60, 3, 127));
  render_blocks(p, out, 1);
  CHECK(p.live_position() == 0u);  // not marked: a sequencer's note is no live note
  p.mark_live();
  render_blocks(p, out, 2);
  const std::uint32_t w = p.live_position();
  CHECK((w & VoicePool::kLivePlaying) != 0u);
  CHECK(((w >> 24) & 0x7Fu) == 4u);  // slot 3 + 1
  CHECK((w & 0xFFFFFFu) == 3u * kBlk);  // three blocks since the note on (one before the mark), at the root's pitch: one sample a sample
  render_blocks(p, out, 1);
  CHECK((p.live_position() & 0xFFFFFFu) == 4u * kBlk);
  // Another note on the same track retriggers the voice: the live one is gone until it is marked again.
  CHECK(p.note_on(0, 60, 3, 127));
  render_blocks(p, out, 1);
  CHECK(p.live_position() == 0u);
  p.mark_live();
  render_blocks(p, out, 1);
  CHECK((p.live_position() & 0xFFFFFFu) == 2u * kBlk);
  // The note ends (a one shot runs out): no live voice.
  p.cut(0);
  for (int i = 0; i < 8; ++i) {
    render_blocks(p, out, 1);
  }
  CHECK(p.live_position() == 0u);
}

TEST_CASE("voices: 12 notes on 12 tracks sound at once, none is stolen, a retrigger keeps the count; a 13th track steals the oldest; a 17th track does not exist") {
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 1000;
  }
  static std::int16_t out[2 * kBlk];
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{dc, 4000, 0, 0, 65536});
  static_assert(o3os::audio::kMaxVoices == 12u && o3os::audio::kMaxVoices < o3os::audio::kVoiceTracks, "the CPU cap: a pattern's tracks, under the graph's");
  for (unsigned t = 0; t < o3os::audio::kMaxVoices; ++t) {
    CHECK(p.note_on(t, 60, 0, 127));
    render_blocks(p, out, 1);  // one block apart: track 0's note is the oldest
  }
  CHECK(p.voices_in_use() == o3os::audio::kMaxVoices);
  CHECK(p.stats().steals == 0u);
  for (unsigned t = 0; t < o3os::audio::kMaxVoices; ++t) {
    CHECK(p.track_playing(t));
    NEED(p.feed(t) != nullptr);
    CHECK(p.feed(t)[kBlk - 1] == 1000);  // every track has its own voice, sounding at the sample's level
  }
  CHECK(p.note_on(5, 64, 0, 127));  // a retrigger takes the track's voice over: no new voice, no steal
  CHECK(p.voices_in_use() == o3os::audio::kMaxVoices);
  CHECK(p.stats().steals == 0u);
  CHECK(p.note_on(o3os::audio::kMaxVoices, 60, 0, 127));  // the 13th track (a live note's spare track): the oldest voice (track 0) is stolen
  render_blocks(p, out, 2);
  CHECK(p.stats().steals == 1u);
  CHECK(p.voices_in_use() == o3os::audio::kMaxVoices);
  CHECK_FALSE(p.track_playing(0));
  CHECK(p.track_playing(o3os::audio::kMaxVoices));
  CHECK(p.track_playing(1));
  const std::uint32_t ignored = p.stats().ignored;
  CHECK_FALSE(p.note_on(o3os::audio::kVoiceTracks, 60, 0, 127));  // the 17th: no such track, ignored (counted), nothing stolen
  CHECK(p.stats().ignored == ignored + 1u);
  CHECK(p.stats().steals == 1u);
}

TEST_CASE("voices: the voices' load is published once a block: in use, the peak since boot, the steals (OS-VOX)") {
  static std::int16_t dc[4000];
  for (std::int16_t& s : dc) {
    s = 1000;
  }
  static std::int16_t out[kBlk];
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{dc, 4000, 0, 0, 65536});
  CHECK(p.voice_snapshot().in_use == 0u);
  CHECK(p.voice_snapshot().peak == 0u);
  for (unsigned t = 0; t < 5; ++t) {
    CHECK(p.note_on(t, 60, 0, 127));
  }
  CHECK(p.voice_snapshot().in_use == 0u);  // published by the block, not by the note
  render_blocks(p, out, 1);
  CHECK(p.voice_snapshot().in_use == 5u);
  CHECK(p.voice_snapshot().peak == 5u);
  p.cut(0);
  p.cut(1);
  p.cut(2);
  for (int i = 0; i < 4; ++i) {  // the cut voices' residuals fade out within a block
    render_blocks(p, out, 1);
  }
  CHECK(p.voice_snapshot().in_use == 2u);
  CHECK(p.voice_snapshot().peak == 5u);  // the peak stays
  CHECK(p.voice_snapshot().steals == 0u);
}

// ---- The Slide FX (OS-PFX, voices.h SLIDE FX) --------------------------------------------------------------------------------------

namespace {

// The upward zero crossings of x[0..n) as positions in samples (linear interpolation between the two samples), at most `cap` of them. Returns how many.
std::uint32_t up_crossings(const std::int16_t* x, std::uint32_t n, double* t, std::uint32_t cap) {
  std::uint32_t c = 0;
  for (std::uint32_t i = 1; i < n && c < cap; ++i) {
    if (x[i - 1] < 0 && x[i] >= 0) {
      t[c++] = static_cast<double>(i - 1) + static_cast<double>(-x[i - 1]) / static_cast<double>(x[i] - x[i - 1]);
    }
  }
  return c;
}

// The mean period, in samples, of the crossings that fall in [from, to): (last - first) / (count - 1). 0 when fewer than two fall there.
double mean_period(const double* t, std::uint32_t c, double from, double to) {
  std::uint32_t first = 0, count = 0;
  double last = 0.0, start = 0.0;
  for (std::uint32_t i = 0; i < c; ++i) {
    if (t[i] >= from && t[i] < to) {
      if (count == 0u) {
        start = t[i];
        first = i;
      }
      last = t[i];
      ++count;
    }
  }
  (void)first;
  return count >= 2u ? (last - start) / static_cast<double>(count - 1u) : 0.0;
}

// A sine of 40 samples a period (so a note at the root plays it at 40, an octave up at 20), `n` samples long.
void make_sine40(std::int16_t* s, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    s[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 40.0)));
  }
}

}  // namespace

TEST_CASE("voices: slid_inc_q32 is the note's pitch moved by tune, fine tune and slide, linear between the semitones, held at the ends of the table (OS-PFX)") {
  using o3os::audio::pitch_inc_q32;
  using o3os::audio::slid_inc_q32;
  using o3os::audio::tuned_inc_q32;
  for (unsigned note = 0; note < 128; ++note) {
    CHECK(slid_inc_q32(note, 0, 0, 0) == pitch_inc_q32(note));  // no slide, no fine tune: the plain table
    CHECK(slid_inc_q32(note, 7, 0, 0) == tuned_inc_q32(note, 7, 0));
  }
  // whole semitones are exact table entries, both ways
  for (int k = -24; k <= 24; ++k) {
    CHECK_MESSAGE(slid_inc_q32(60, 0, 0, k * 16 * 256) == pitch_inc_q32(static_cast<unsigned>(60 + k)), "k " << k);
    CHECK_MESSAGE(slid_inc_q32(60, 5, 0, k * 16 * 256) == pitch_inc_q32(static_cast<unsigned>(65 + k)) , "k " << k << " with a tune of 5");
  }
  // held at the ends of the table, not wrapped
  CHECK(slid_inc_q32(127, 0, 0, 5 * 16 * 256) == pitch_inc_q32(127));
  CHECK(slid_inc_q32(0, 0, 0, -5 * 16 * 256) == pitch_inc_q32(0));
  CHECK(slid_inc_q32(0, 0, 0, 8 * 256) > pitch_inc_q32(0));  // the first note slides up toward the second
  CHECK(slid_inc_q32(0, 0, 0, 8 * 256) < pitch_inc_q32(1));
  CHECK(slid_inc_q32(0, 0, 0, -8 * 256) == pitch_inc_q32(0));
  CHECK(slid_inc_q32(120, 12, 0, 0) == pitch_inc_q32(127));
  // against 2^((n - 60 + offset) / 12) in double: the law is linear between the semitones, so up to 0.05 % off the exponential
  double worst = 0.0;
  for (unsigned note = 12; note <= 110; note += 7) {
    for (const int tune : {-12, 0, 9}) {
      for (const int fine : {-100, -37, 0, 50, 100}) {
        for (const int sixteenths : {-255, -100, -17, -1, 1, 8, 16, 33, 128, 255}) {
          const double offset = tune + fine / 100.0 + sixteenths / 16.0;
          if (static_cast<double>(note) + offset < 1.0 || static_cast<double>(note) + offset > 126.0) {
            continue;  // past the ends the table holds (checked above)
          }
          const double want = std::exp2((static_cast<double>(note) - 60.0 + offset) / 12.0);
          const double got = static_cast<double>(slid_inc_q32(note, tune, fine, sixteenths * 256)) / 4294967296.0;
          const double err = std::fabs(got / want - 1.0);
          worst = err > worst ? err : worst;
        }
      }
    }
  }
  CHECK_MESSAGE(worst < 6e-4, "worst relative error " << worst);
  // the sign of a fraction: one sixteenth down from the root is just under the root, one up just over
  CHECK(slid_inc_q32(60, 0, 0, -256) < pitch_inc_q32(60));
  CHECK(slid_inc_q32(60, 0, 0, -256) > pitch_inc_q32(59));
  CHECK(slid_inc_q32(60, 0, 0, 256) > pitch_inc_q32(60));
  CHECK(slid_inc_q32(60, 0, 0, 256) < pitch_inc_q32(61));
}

TEST_CASE("voices: the Slide FX moves a sampler voice's pitch in one step of the tempo, holds it, and its end gives back the note's own (OS-PFX)") {
  static std::int16_t sine[16000];
  static std::int16_t out[56 * kBlk];
  static double tc[700];
  make_sine40(sine, 16000);
  // (a) an octave up, the FX held before the note: the note slides at once, 827 samples (one step at 800 BPM) to the top, and stays there
  {
    VoicePool& p = pool();
    p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
    p.set_tempo_x100(80000);
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, out, 56);
    const std::uint32_t c = up_crossings(out, 56 * kBlk, tc, 700);
    NEED(c > 40u);
    const double first = tc[1] - tc[0];
    CHECK_MESSAGE((first > 33.0 && first < 41.0), "the first period " << first << ": the note starts at its own pitch and is already sliding");
    for (std::uint32_t i = 2; i < c; ++i) {
      CHECK_MESSAGE((tc[i] - tc[i - 1]) <= (tc[i - 1] - tc[i - 2]) + 1.2, "period " << i << ": the pitch only rises");
    }
    const double mid = mean_period(tc, c, 330.0, 500.0);  // about the middle of the ramp (413): half an octave up is 28.3 samples; the staircase and the window blur it
    CHECK_MESSAGE((mid > 25.0 && mid < 32.0), "mid-ramp period " << mid);
    const double top = mean_period(tc, c, 1000.0, 3500.0);
    CHECK_MESSAGE(std::fabs(top - 20.0) < 0.15, "landed period " << top);
  }
  // (b) the same FX, down
  {
    VoicePool& p = pool();
    p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
    p.set_tempo_x100(80000);
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideDown, 192);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 56);
    const std::uint32_t c = up_crossings(out, 56 * kBlk, tc, 700);
    NEED(c > 20u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1200.0, 3500.0) - 80.0) < 0.5, "landed period down: " << mean_period(tc, c, 1200.0, 3500.0));
  }
  // (c) the FX arrives while the note sounds: it slides from where it is. Six semitones up, then twelve more from the FX's value (the distance is from the NOTE: 192 sixteenths, so
  // six more), then 0: back to the note's own pitch. The first block after an FX still plays the old pitch (the ramp is evaluated at the start of each block).
  {
    VoicePool& p = pool();
    p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
    p.set_tempo_x100(80000);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 4);
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 96);  // +6 semitones: 40 / 2^0.5 = 28.28
    render_blocks(p, out + 4 * kBlk, 20);
    std::uint32_t c = up_crossings(out, 24 * kBlk, tc, 700);
    NEED(c > 10u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 70.0, 250.0) - 40.0) < 0.3, "before the FX: " << mean_period(tc, c, 70.0, 250.0));
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1200.0, 1530.0) - 28.28) < 0.3, "after the first FX: " << mean_period(tc, c, 1200.0, 1530.0));
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);  // on to +12: 20
    render_blocks(p, out, 24);
    c = up_crossings(out, 24 * kBlk, tc, 700);
    NEED(c > 10u);
    const double again = mean_period(tc, c, 0.0, 2.0 * kBlk);
    CHECK_MESSAGE((again > 27.0 && again < 29.5), "the first block of the second FX plays where the first left it: " << again);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1000.0, 1500.0) - 20.0) < 0.2, "after the second FX: " << mean_period(tc, c, 1000.0, 1500.0));
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 0);  // back to the note
    render_blocks(p, out, 24);
    c = up_crossings(out, 24 * kBlk, tc, 700);
    NEED(c > 10u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1000.0, 1500.0) - 40.0) < 0.3, "slide 0: " << mean_period(tc, c, 1000.0, 1500.0));
    // the FX ends while it is up: the note's own pitch at the next block
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);
    render_blocks(p, out, 24);
    p.fx_event(0, o3os::audio::kModGroupSlide, 0, 0);
    render_blocks(p, out, 8);
    c = up_crossings(out, 8 * kBlk, tc, 700);
    NEED(c > 5u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 100.0, 500.0) - 40.0) < 0.3, "after the FX ends: " << mean_period(tc, c, 100.0, 500.0));
    // and a new note under no FX plays at its pitch from the start
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 8);
    c = up_crossings(out, 8 * kBlk, tc, 700);
    NEED(c > 5u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 70.0, 500.0) - 40.0) < 0.3, "a new note after the FX ended: " << mean_period(tc, c, 70.0, 500.0));
  }
  // (d) a note that starts under the FX begins at its own pitch again, though the voice had slid: the FX stays on the track until it ends, and the transport start ends it
  {
    VoicePool& p = pool();
    p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
    p.set_tempo_x100(80000);
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 30);
    p.note_on(0, 60, 0, 127);  // the retrigger of the same track
    render_blocks(p, out, 12);
    const std::uint32_t c = up_crossings(out, 12 * kBlk, tc, 700);
    NEED(c > 5u);
    const double restart = mean_period(tc, c, 70.0, 200.0);  // one octave's ramp has gone about 2 semitones by then: 34..38 samples a period, not the 20 of a note that kept the pitch
    CHECK_MESSAGE((restart > 33.0 && restart < 40.0), "the retriggered note starts at its own pitch: " << restart);
    p.clear_slide_fxs();
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 12);
    const std::uint32_t c2 = up_crossings(out, 12 * kBlk, tc, 700);
    NEED(c2 > 5u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c2, 70.0, 700.0) - 40.0) < 0.3, "the FX reset: " << mean_period(tc, c2, 70.0, 700.0));
  }
}

TEST_CASE("voices: a voice that does not slide is bit for bit what it was, whatever another track's Slide FX is (OS-PFX)") {
  static std::int16_t sine[4000];
  static std::int16_t plain[16 * kBlk];
  static std::int16_t other[16 * kBlk];
  make_sine40(sine, 4000);
  {
    VoicePool& p = pool();
    p.set_sample(0, SampleDesc{sine, 4000, 0, 0, 65536});
    p.note_on(0, 64, 0, 100);
    render_blocks(p, plain, 16);
  }
  {
    VoicePool& p = pool();
    p.set_sample(0, SampleDesc{sine, 4000, 0, 0, 65536});
    p.fx_event(3, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 200);  // another track's FX
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 5);
    p.fx_event(0, o3os::audio::kModGroupSlide, 0, 0);   // and this track's, ended before the note
    p.note_on(0, 64, 0, 100);
    render_blocks(p, other, 16);
  }
  for (std::uint32_t k = 0; k < 16 * kBlk; ++k) {
    CHECK(plain[k] == other[k]);
  }
}

TEST_CASE("voices: the Slide FX through the engine: a ModSet of the Slide group slides the sounding note, a ModEnd gives the pitch back (OS-PFX)") {
  static std::int16_t sine[20000];
  static std::int16_t heard[112 * kBlk];
  static double tc[2000];
  make_sine40(sine, 20000);
  static o3os::test::StaticSlot<Engine> engine_slot;
  static o3os::audio::TapMem taps;
  static SeqEvent events[o3os::audio::kMaxEventsPerBlock];
  Engine& e = engine_slot.make(&fake_cycles, taps);
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{sine, 20000, 0, 0, 65536});
  e.attach_voices(&p);
  o3os::audio::EventSource src;
  src.step = &scripted_step;
  src.buffer = events;
  src.capacity = o3os::audio::kMaxEventsPerBlock;
  e.attach_events(src);

  const auto slide = [](EventKind kind, std::uint8_t type, std::uint8_t value) {
    SeqEvent ev = event(0, kind);
    ev.a = type;
    ev.b = value;
    ev.c = o3os::audio::kLayerFx1;
    ev.flags = o3os::audio::kModGroupSlide;
    return ev;
  };
  g_cue_count = 0;
  g_cues[g_cue_count++] = Cue{0, event(0, EventKind::NoteOn, 60, 0, 127)};
  g_cues[g_cue_count++] = Cue{2, slide(EventKind::ModSet, o3os::audio::kFxSlideUp, 192)};  // an octave up, in a step at 120 BPM (86 blocks)
  g_cues[g_cue_count++] = Cue{104, slide(EventKind::ModEnd, 0, 0)};
  Rig r;
  for (std::uint32_t b = 0; b < 112; ++b) {
    render(e, p, r);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      heard[b * kBlk + i] = r.voice[i];
    }
  }
  const std::uint32_t c = up_crossings(heard, 112 * kBlk, tc, 2000);
  NEED(c > 100u);
  const double before = mean_period(tc, c, 70.0, 190.0);  // the note plays alone for the first two blocks (the FX comes at block 2)
  CHECK_MESSAGE(std::fabs(before - 40.0) < 0.3, "before the FX: " << before);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 6000.0, 6600.0) - 20.0) < 0.2, "landed: " << mean_period(tc, c, 6000.0, 6600.0));
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 6900.0, 7168.0) - 40.0) < 0.3, "after the ModEnd: " << mean_period(tc, c, 6900.0, 7168.0));
}

// ---- GLIDE, OVERDRIVE, BIT DEPTH (OS-IFX1, voices.h GLIDE / OVERDRIVE and BIT DEPTH) ---------------------------------------------------

TEST_CASE("ifx: the Glide FX's time is 20 ms a unit up to 50 and 1 s plus 280 ms a unit above it, the overdrive's gain is 1 + 15 (v / 100)^2, and both stay inside their ranges (OS-IFX1)") {
  using o3os::audio::glide_fx_ms;
  using o3os::audio::overdrive_gain_q11;
  CHECK(glide_fx_ms(0) == 0u);
  CHECK(glide_fx_ms(1) == 20u);
  CHECK(glide_fx_ms(25) == 500u);
  CHECK(glide_fx_ms(50) == 1000u);  // the seam of the two spans
  CHECK(glide_fx_ms(51) == 1280u);
  CHECK(glide_fx_ms(75) == 8000u);
  CHECK(glide_fx_ms(100) == 15000u);
  CHECK(glide_fx_ms(101) == 15000u);  // held
  CHECK(glide_fx_ms(255) == 15000u);
  for (unsigned v = 1; v <= 100; ++v) {
    CHECK(glide_fx_ms(v) > glide_fx_ms(v - 1));  // strictly increasing
  }
  CHECK(overdrive_gain_q11(0) == 0u);  // none: the voice skips the pass
  CHECK(overdrive_gain_q11(1) == 2051u);
  CHECK(overdrive_gain_q11(50) == 9728u);  // 4.75
  CHECK(overdrive_gain_q11(100) == 32768u);  // 16
  CHECK(overdrive_gain_q11(200) == 32768u);
  for (unsigned v = 1; v <= 100; ++v) {
    CHECK(overdrive_gain_q11(v) >= overdrive_gain_q11(v - 1));
    CHECK(overdrive_gain_q11(v) >= 2048u);
  }
}

TEST_CASE("ifx: the overdrive curve is odd, monotonic, bounded and full scale at the clip; the bit depth clears the low bits as a mask (OS-IFX1)") {
  using o3os::audio::bit_depth_sample;
  using o3os::audio::overdrive_gain_q11;
  using o3os::audio::overdrive_sample;
  for (const unsigned od : {1u, 10u, 33u, 50u, 80u, 100u}) {
    const std::int32_t g = overdrive_gain_q11(od);
    CHECK(overdrive_sample(0, g) == 0);
    std::int32_t prev = overdrive_sample(-32768, g);
    if (od >= 50) {
      CHECK(prev == -32768);  // a drive of 4.75 and more is past the clip at full scale, which is exactly full scale
    }
    CHECK(overdrive_sample(32767, g) <= 32767);
    bool monotonic = true, odd = true, bounded = true;
    for (std::int32_t x = -32767; x <= 32767; ++x) {
      const std::int32_t y = overdrive_sample(x, g);
      monotonic = monotonic && y >= prev;
      const std::int32_t m = overdrive_sample(-x, g);
      odd = odd && (y + m >= -3 && y + m <= 3);  // odd to a few LSB (both shifts floor: the slope is up to 2)
      bounded = bounded && y >= -32768 && y <= 32767;
      prev = y;
    }
    CHECK_MESSAGE(monotonic, "od " << od);
    CHECK_MESSAGE(odd, "od " << od);
    CHECK_MESSAGE(bounded, "od " << od);
  }
  // a soft clipper: below the knee it is the drive times the sample (to the second-order term), and a larger drive reaches full scale sooner
  CHECK(overdrive_sample(1000, overdrive_gain_q11(1)) > 990);
  CHECK(overdrive_sample(1000, overdrive_gain_q11(1)) < 1010);
  CHECK(overdrive_sample(4000, overdrive_gain_q11(100)) > 30000);  // x16 would be 64000: clipped, within 8 % of full scale
  CHECK(overdrive_sample(16384, overdrive_gain_q11(100)) >= 32766);  // 16384 x 16 is far past the clip
  // the bit depth: a mask, i.e. a floor to a multiple
  CHECK(bit_depth_sample(127, 8) == 0);
  CHECK(bit_depth_sample(255, 8) == 0);
  CHECK(bit_depth_sample(256, 8) == 256);
  CHECK(bit_depth_sample(-1, 8) == -256);
  CHECK(bit_depth_sample(-256, 8) == -256);
  CHECK(bit_depth_sample(-257, 8) == -512);
  CHECK(bit_depth_sample(32767, 8) == 32512);
  CHECK(bit_depth_sample(-32768, 8) == -32768);
  CHECK(bit_depth_sample(5, 1) == 4);
  CHECK(bit_depth_sample(1, 15) == 0);
  CHECK(bit_depth_sample(-20000, 15) == -32768);
  CHECK(bit_depth_sample(12345, 0) == 12345);  // 16 bits: nothing (the voice never calls it)
  for (unsigned sh = 1; sh <= 15; ++sh) {
    std::int32_t prev = bit_depth_sample(-32768, sh);
    bool ok = true;
    for (std::int32_t x = -32767; x <= 32767; ++x) {
      const std::int32_t y = bit_depth_sample(x, sh);
      ok = ok && y >= prev && (y & ((1 << sh) - 1)) == 0 && y <= x && x - y < (1 << sh) && y >= -32768;
      prev = y;
    }
    CHECK_MESSAGE(ok, "shift " << sh);
  }
}

namespace {
// One note of the 40-sample sine (note 60 plays it at 40 samples a period) on track 0 through a fresh descriptor, `blocks` blocks of it.
SampleDesc ifx_desc(const std::int16_t* sine) {
  SampleDesc d{sine, 16000, 0, 0, 65536};
  return d;
}
}  // namespace

TEST_CASE("ifx: OFF is bit for bit what the voice was: a held FX of overdrive 0 and 16 bits over an instrument that shapes, and the instrument's own defaults (OS-IFX1)") {
  static std::int16_t sine[16000];
  static std::int16_t base[24 * kBlk];
  static std::int16_t other[24 * kBlk];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  p.set_sample(0, ifx_desc(sine));
  p.cut(0);
  render_blocks(p, other, 6);  // the declick residual of the earlier cut dies
  p.note_on(0, 60, 0, 127);
  render_blocks(p, base, 24);
  // the instrument shapes hard; the track's FX say OFF: the same samples as the defaults
  SampleDesc d = ifx_desc(sine);
  d.overdrive = 90;
  d.bit_depth = 5;
  p.set_sample(0, d);
  p.cut(0);
  render_blocks(p, other, 6);
  p.fx_event(0, o3os::audio::kModGroupOverdrive, o3os::audio::kFxOverdrive, 0);
  p.fx_event(0, o3os::audio::kModGroupBitDepth, o3os::audio::kFxBitDepth, 16);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, other, 24);
  CHECK(std::memcmp(base, other, sizeof base) == 0);
  // without the FX the instrument's values are heard (so the test above is not the same render twice)
  p.clear_slide_fxs();
  p.cut(0);
  render_blocks(p, other, 6);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, other, 24);
  CHECK(std::memcmp(base, other, sizeof base) != 0);
  // a ModEnd gives the instrument's values back to the sounding voice
  p.fx_event(0, o3os::audio::kModGroupOverdrive, o3os::audio::kFxOverdrive, 0);
  p.fx_event(0, o3os::audio::kModGroupBitDepth, o3os::audio::kFxBitDepth, 16);
  p.cut(0);
  render_blocks(p, other, 6);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, other, 12);
  p.fx_event(0, o3os::audio::kModGroupOverdrive, 0, 0);
  p.fx_event(0, o3os::audio::kModGroupBitDepth, 0, 0);
  render_blocks(p, other + 12 * kBlk, 12);
  CHECK(std::memcmp(base, other, 12 * kBlk * sizeof base[0]) == 0);
  CHECK(std::memcmp(base + 12 * kBlk, other + 12 * kBlk, 12 * kBlk * sizeof base[0]) != 0);
  p.clear_slide_fxs();
  p.set_sample(0, ifx_desc(sine));
}

TEST_CASE("ifx: the voice's block is the overdrive curve and the bit depth applied to the block it would have had (OS-IFX1)") {
  static std::int16_t sine[16000];
  static std::int16_t base[24 * kBlk];
  static std::int16_t got[24 * kBlk];
  static std::int16_t flush[6 * kBlk];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  const auto play = [&](const SampleDesc& d, std::int16_t* out) {
    p.set_sample(0, d);
    p.cut(0);
    render_blocks(p, flush, 6);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 24);
  };
  play(ifx_desc(sine), base);
  // overdrive alone
  SampleDesc d = ifx_desc(sine);
  d.overdrive = 60;
  play(d, got);
  const std::int32_t g = o3os::audio::overdrive_gain_q11(60);
  bool same = true;
  std::int32_t peak = 0;
  for (std::uint32_t i = 0; i < 24 * kBlk; ++i) {
    same = same && got[i] == o3os::audio::overdrive_sample(base[i], g);
    peak = std::abs(got[i]) > peak ? std::abs(got[i]) : peak;
  }
  CHECK(same);
  CHECK_MESSAGE(peak > 28000, "the sine of 20000 is driven toward full scale: peak " << peak);
  // bit depth alone: 6 bits (a multiple of 1024)
  d = ifx_desc(sine);
  d.bit_depth = 6;
  play(d, got);
  same = true;
  bool multiples = true;
  for (std::uint32_t i = 0; i < 24 * kBlk; ++i) {
    same = same && got[i] == o3os::audio::bit_depth_sample(base[i], 10);
    multiples = multiples && (got[i] & 1023) == 0;
  }
  CHECK(same);
  CHECK(multiples);
  // both: the overdrive first, then the depth
  d.overdrive = 60;
  play(d, got);
  same = true;
  for (std::uint32_t i = 0; i < 24 * kBlk; ++i) {
    same = same && got[i] == o3os::audio::bit_depth_sample(o3os::audio::overdrive_sample(base[i], g), 10);
  }
  CHECK(same);
  // the engines of OS-005h are a cap on the depth: Bits8 with the instrument at 16 is 8 bits, with 12 still 8 (both rounding, clamped at 32767), Bits12 with the instrument at 8 is the
  // mask of 8 bits (the Bit Depth parameter is the lower: it truncates)
  d = ifx_desc(sine);
  d.engine = o3os::audio::kEngineBits8;
  play(d, got);
  same = true;
  for (std::uint32_t i = 0; i < 24 * kBlk; ++i) {
    const std::int32_t r = (base[i] + 128) & ~255;
    same = same && got[i] == (r > 32767 ? 32767 : r);
  }
  CHECK(same);
  static std::int16_t cap[24 * kBlk];
  d.bit_depth = 12;
  play(d, cap);
  CHECK(std::memcmp(got, cap, sizeof got) == 0);
  d.engine = o3os::audio::kEngineBits12;
  d.bit_depth = 8;
  play(d, cap);
  same = true;
  for (std::uint32_t i = 0; i < 24 * kBlk; ++i) {
    same = same && cap[i] == o3os::audio::bit_depth_sample(base[i], 8);
  }
  CHECK(same);
  // the FX replaces the instrument's value, and acts on the voice sounding now
  d = ifx_desc(sine);
  p.set_sample(0, d);
  p.cut(0);
  render_blocks(p, got, 6);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, got, 12);
  p.fx_event(0, o3os::audio::kModGroupBitDepth, o3os::audio::kFxBitDepth, 4);
  render_blocks(p, got + 12 * kBlk, 12);
  same = true;
  for (std::uint32_t i = 12 * kBlk; i < 24 * kBlk; ++i) {
    same = same && got[i] == o3os::audio::bit_depth_sample(base[i], 12);
  }
  CHECK(same);
  p.clear_slide_fxs();
  p.set_sample(0, ifx_desc(sine));
}

// OS-BUG-MODDRIVE: shape_pcm is the one curve the sampler's voices (shape_span) and the module tracks (Engine::shape_modules) run. Over a sweep of the whole range, every kind (drive alone, depth by
// truncation, depth by the 12 / 8 bit engines' rounding, both) equals the pure functions of voices.h sample for sample; a drive and a depth of none leave the block alone.
TEST_CASE("ifx: shape_pcm equals the pure curves for every kind, and a drive and depth of none leaves the block (OS-BUG-MODDRIVE)") {
  using o3os::audio::overdrive_gain_q11;
  using o3os::audio::overdrive_sample;
  constexpr std::uint32_t kN = 256;
  std::int16_t base[kN];
  for (std::uint32_t i = 0; i < kN; ++i) {
    base[i] = static_cast<std::int16_t>(-32768 + static_cast<std::int32_t>(i) * 257);  // -32768 .. 32767
  }
  for (unsigned od : {0u, 1u, 50u, 100u}) {
    for (unsigned sh : {0u, 1u, 4u, 8u, 15u}) {
      for (bool round : {false, true}) {
        if (round && sh == 0u) {
          continue;  // (the rounding is the 12 / 8 bit engines': a shift of 4 or 8)
        }
        const std::int32_t g = overdrive_gain_q11(od);
        std::int16_t got[kN];
        std::memcpy(got, base, sizeof base);
        VoicePool::shape_pcm(got, kN, static_cast<unsigned>(g), sh, round);
        bool same = true;
        for (std::uint32_t i = 0; i < kN; ++i) {
          std::int32_t x = base[i];
          if (g == 0 && sh == 0u) {
            // untouched
          } else {
            x = g != 0 ? overdrive_sample(x, g) : x;
            if (sh != 0u) {
              const std::int32_t mask = ~((std::int32_t{1} << sh) - 1);
              x = round ? (((x + (std::int32_t{1} << (sh - 1u))) & mask) > 32767 ? 32767 : ((x + (std::int32_t{1} << (sh - 1u))) & mask)) : (x & mask);
            }
          }
          same = same && got[i] == static_cast<std::int16_t>(x);
        }
        CHECK_MESSAGE(same, "od " << od << " shift " << sh << (round ? " rounding" : ""));
      }
    }
  }
}

// ---- OS-PERFFX P1: the Perform layer's OVERDRV and BITDEPTH (voices.h PERFORMANCE LAYER, THE REST OF THE GROUPS). The layer's value wins over the step FX's and the instrument's, for the voice
// sounding now and for the track's next notes; its end brings the step FX back, and the step FX's end the instrument's. The curves are OS-IFX1's, so the checks are against its pure functions.

namespace {
// A ModSet of `group` on `track` whose winner is the Perform layer: `value` is the layer's, `step_type` / `step_value` the step FX underneath (0 / 0: none).
SeqEvent layer_set(unsigned group, unsigned track, unsigned step_type, unsigned step_value, int value) {
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.track = static_cast<std::uint8_t>(track);
  e.a = static_cast<std::uint8_t>(step_type);
  e.b = static_cast<std::uint8_t>(step_value);
  e.c = o3os::audio::kLayerPerf;
  e.flags = static_cast<std::uint8_t>(group);
  e.value = value;
  return e;
}

// What the control table sends when the layer ends over a step FX (the step FX as the winner), or when nothing is left (a ModEnd).
SeqEvent step_again(unsigned group, unsigned track, unsigned step_type, unsigned step_value) {
  SeqEvent e = layer_set(group, track, step_type, step_value, 0);
  e.c = o3os::audio::kLayerFx1;
  return e;
}

SeqEvent group_end(unsigned group, unsigned track) {
  SeqEvent e{};
  e.kind = EventKind::ModEnd;
  e.track = static_cast<std::uint8_t>(track);
  e.flags = static_cast<std::uint8_t>(group);
  return e;
}
}  // namespace

TEST_CASE("perform layer P1: OVERDRV and BITDEPTH replace the step FX's and the instrument's value; the layer's end brings the step FX back, and the step FX's end the instrument's") {
  using o3os::audio::kFxBitDepth;
  using o3os::audio::kFxOverdrive;
  using o3os::audio::kModGroupBitDepth;
  using o3os::audio::kModGroupOverdrive;
  using o3os::audio::bit_depth_sample;
  using o3os::audio::overdrive_gain_q11;
  using o3os::audio::overdrive_sample;
  constexpr std::uint32_t kN = 24 * kBlk;
  static std::int16_t sine[16000];
  static std::int16_t base[kN];
  static std::int16_t got[kN];
  static std::int16_t flush[6 * kBlk];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  p.perf_clear();
  const auto play = [&](const SampleDesc& d, std::int16_t* out) {
    p.set_sample(0, d);
    p.cut(0);
    render_blocks(p, flush, 6);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 24);
  };
  const auto is_od = [&](std::int32_t amount) {  // got == the overdrive of `amount` over the plain note
    const std::int32_t g = overdrive_gain_q11(static_cast<unsigned>(amount));
    bool same = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
      same = same && got[i] == (amount == 0 ? base[i] : overdrive_sample(base[i], g));
    }
    return same;
  };
  const auto is_depth = [&](unsigned shift) {  // got == the crush of `shift` low bits over the plain note
    bool same = true;
    for (std::uint32_t i = 0; i < kN; ++i) {
      same = same && got[i] == (shift == 0u ? base[i] : bit_depth_sample(base[i], shift));
    }
    return same;
  };
  play(ifx_desc(sine), base);

  // OVERDRV: the layer alone, over an instrument that is clean
  p.fx_event(0, layer_set(kModGroupOverdrive, 0, 0, 0, 60));
  CHECK(p.perf_held(0, kModGroupOverdrive));
  play(ifx_desc(sine), got);
  CHECK(is_od(60));
  CHECK_FALSE(is_od(0));  // (the check is not vacuous)
  // ... over an instrument that drives (20) and a step FX that drives (30): the layer's 60 wins over both
  SampleDesc d = ifx_desc(sine);
  d.overdrive = 20;
  p.fx_event(0, o3os::audio::kModGroupOverdrive, kFxOverdrive, 30);
  p.fx_event(0, layer_set(kModGroupOverdrive, 0, kFxOverdrive, 30, 60));
  play(d, got);
  CHECK(is_od(60));
  // the layer ends: the control table sends the step FX again, and it is what sounds
  p.fx_event(0, step_again(kModGroupOverdrive, 0, kFxOverdrive, 30));
  CHECK_FALSE(p.perf_held(0, kModGroupOverdrive));
  play(d, got);
  CHECK(is_od(30));
  // the step FX ends: the instrument's own
  p.fx_event(0, group_end(kModGroupOverdrive, 0));
  play(d, got);
  CHECK(is_od(20));
  // the layer's value past the range is held to it (the byte is the layer's; the voice caps at 100)
  p.fx_event(0, layer_set(kModGroupOverdrive, 0, 0, 0, 127));
  play(ifx_desc(sine), got);
  CHECK(is_od(100));
  p.fx_event(0, group_end(kModGroupOverdrive, 0));

  // BITDEPTH: the layer's value IS the shift (16 - bits): 10 is 6 bits
  p.fx_event(0, layer_set(kModGroupBitDepth, 0, 0, 0, 10));
  play(ifx_desc(sine), got);
  CHECK(is_depth(10));
  CHECK_FALSE(is_depth(0));
  // over an instrument at 12 bits and a step FX at 8 bits (shift 8): the layer's shift 10 wins over both, the lighter 15 too
  d = ifx_desc(sine);
  d.bit_depth = 12;
  p.fx_event(0, o3os::audio::kModGroupBitDepth, kFxBitDepth, 8);
  p.fx_event(0, layer_set(kModGroupBitDepth, 0, kFxBitDepth, 8, 15));
  play(d, got);
  CHECK(is_depth(15));
  p.fx_event(0, layer_set(kModGroupBitDepth, 0, kFxBitDepth, 8, 1));
  play(d, got);
  CHECK(is_depth(1));
  p.fx_event(0, step_again(kModGroupBitDepth, 0, kFxBitDepth, 8));
  CHECK_FALSE(p.perf_held(0, kModGroupBitDepth));
  play(d, got);
  CHECK(is_depth(8));
  p.fx_event(0, group_end(kModGroupBitDepth, 0));
  play(d, got);
  CHECK(is_depth(4));

  // both at once: the overdrive first, then the crush (OS-IFX1's order)
  p.fx_event(0, layer_set(kModGroupOverdrive, 0, 0, 0, 60));
  p.fx_event(0, layer_set(kModGroupBitDepth, 0, 0, 0, 10));
  play(ifx_desc(sine), got);
  const std::int32_t g60 = overdrive_gain_q11(60);
  bool both = true;
  for (std::uint32_t i = 0; i < kN; ++i) {
    both = both && got[i] == bit_depth_sample(overdrive_sample(base[i], g60), 10);
  }
  CHECK(both);

  // the 8-bit engine caps the depth: the larger shift applies, and the engine's own (rounding) reduction stays when it is the one in force
  d = ifx_desc(sine);
  d.engine = o3os::audio::kEngineBits8;
  p.fx_event(0, group_end(kModGroupOverdrive, 0));
  p.fx_event(0, layer_set(kModGroupBitDepth, 0, 0, 0, 2));  // 14 bits: the engine's 8 is the larger
  play(d, got);
  bool engine = true;
  for (std::uint32_t i = 0; i < kN; ++i) {
    const std::int32_t r = (base[i] + 128) & ~255;
    engine = engine && got[i] == (r > 32767 ? 32767 : r);
  }
  CHECK(engine);
  p.fx_event(0, layer_set(kModGroupBitDepth, 0, 0, 0, 12));  // 4 bits: the layer's shift 12 is the larger, and it truncates
  play(d, got);
  CHECK(is_depth(12));
  p.fx_event(0, group_end(kModGroupBitDepth, 0));

  // the layer arrives while the voice sounds, and goes: the next block takes it, as a live edit does
  p.set_sample(0, ifx_desc(sine));
  p.cut(0);
  render_blocks(p, flush, 6);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, got, 8);
  p.fx_event(0, layer_set(kModGroupOverdrive, 0, 0, 0, 60));
  render_blocks(p, got + 8 * kBlk, 8);
  p.fx_event(0, group_end(kModGroupOverdrive, 0));
  render_blocks(p, got + 16 * kBlk, 8);
  bool live = true;
  for (std::uint32_t i = 0; i < kN; ++i) {
    live = live && got[i] == (i >= 8 * kBlk && i < 16 * kBlk ? overdrive_sample(base[i], g60) : base[i]);
  }
  CHECK(live);

  // the layer is one track's: a note on another track is what the instrument makes of it
  p.fx_event(0, layer_set(kModGroupOverdrive, 0, 0, 0, 60));
  CHECK_FALSE(p.perf_held(1, kModGroupOverdrive));
  p.cut(0);
  render_blocks(p, flush, 6);
  p.note_on(1, 60, 0, 127);
  bool other = true;
  for (std::uint32_t b = 0; b < 24; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* const f = p.feed(1);
    NEED(f != nullptr);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      other = other && f[i] == base[b * kBlk + i];
    }
  }
  CHECK(other);
  p.cut(1);
  render_blocks(p, flush, 6);
  p.perf_clear();
  p.clear_slide_fxs();
  p.set_sample(0, ifx_desc(sine));
}

TEST_CASE("ifx: a live edit of the overdrive or the bit depth reaches the sounding voice at the next block (OS-IFX1)") {
  std::uint32_t pan_tracks = 0;
  static std::int16_t sine[16000];
  static std::int16_t base[16 * kBlk];
  static std::int16_t got[16 * kBlk];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  p.set_sample(0, ifx_desc(sine));
  p.cut(0);
  render_blocks(p, got, 6);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, base, 16);
  p.cut(0);
  render_blocks(p, got, 6);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, got, 8);
  CHECK(p.apply_param(0, o3os::audio::inst_param::BitDepth, 7.0f, pan_tracks));
  render_blocks(p, got + 8 * kBlk, 8);
  bool same = true;
  for (std::uint32_t i = 8 * kBlk; i < 16 * kBlk; ++i) {
    same = same && got[i] == o3os::audio::bit_depth_sample(base[i], 9);
  }
  CHECK(same);
  CHECK(p.sample(0).bit_depth == 7u);
  CHECK(p.apply_param(0, o3os::audio::inst_param::BitDepth, 99.0f, pan_tracks));
  CHECK(p.sample(0).bit_depth == 16u);  // held to the range
  CHECK(p.apply_param(0, o3os::audio::inst_param::Overdrive, 250.0f, pan_tracks));
  CHECK(p.sample(0).overdrive == 100u);
  CHECK(p.apply_param(0, o3os::audio::inst_param::Glide, 99999.0f, pan_tracks));
  CHECK(p.sample(0).glide == 15000u);
  p.set_sample(0, ifx_desc(sine));
}

// OS-MODPITCH: the module instruments walk the same ramp as the sampler voices (audio/pitch_ramp.h, the one law of the Slide and Glide FX). The sampler's audible pitch, measured in windows of a
// 200 ms glide an octave up, against the periods the shared ramp predicts block by block (the voice takes the offset at the start of each block). The module side's own exact trajectory is
// tests/module_pitch_test.cpp; the helper's arithmetic is held to the old formula there too.
TEST_CASE("ifx: the sampler's Glide is the shared ramp of audio/pitch_ramp.h: its audible periods are the ones the ramp predicts (OS-MODPITCH)") {
  static std::int16_t sine[16000];
  static std::int16_t out[160 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  SampleDesc d = ifx_desc(sine);
  d.glide = 200;
  p.set_sample(0, d);
  p.cut(0);
  render_blocks(p, out, 6);
  p.note_on(0, 48, 0, 127);
  render_blocks(p, out, 4);
  p.cut(0);
  render_blocks(p, out, 6);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, out, 160);
  const std::uint32_t c = up_crossings(out, 160 * kBlk, tc, 900);
  NEED(c > 150u);
  o3os::audio::PitchRamp ramp;
  ramp.q16 = (48 - 60) * 65536;
  ramp.tgt = 0;
  ramp.left = o3os::audio::glide_samples(200);
  double predicted[160];
  for (std::uint32_t b = 0; b < 160u; ++b) {
    predicted[b] = 40.0 * std::exp2(-static_cast<double>(ramp.q16) / 65536.0 / 12.0);  // the period at the pitch this block has (the offset at its start)
    (void)o3os::audio::ramp_advance(ramp, kBlk);
  }
  const std::uint32_t windows[3][2] = {{20, 30}, {60, 70}, {100, 110}};
  for (const auto& w : windows) {
    double mean = 0.0;
    for (std::uint32_t b = w[0]; b < w[1]; ++b) {
      mean += predicted[b];
    }
    mean /= static_cast<double>(w[1] - w[0]);
    const double got = mean_period(tc, c, static_cast<double>(w[0] * kBlk), static_cast<double>(w[1] * kBlk));
    CHECK_MESSAGE(std::fabs(got / mean - 1.0) < 0.03, "blocks " << w[0] << ".." << w[1] << ": heard " << got << ", the ramp says " << mean);
  }
  CHECK(ramp.left == 0u);  // landed within the 160 blocks
}

TEST_CASE("ifx: GLIDE starts a note at the previous note's pitch and reaches its own in the glide time, from the instrument or the FX (OS-IFX1)") {
  static std::int16_t sine[16000];
  static std::int16_t out[160 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  const auto note_pair = [&](unsigned first, unsigned second) {
    p.cut(0);
    render_blocks(p, out, 6);
    p.note_on(0, first, 0, 127);
    render_blocks(p, out, 4);
    p.cut(0);  // the glide survives the note's end (this OS's rule: OS-IFX1)
    render_blocks(p, out, 6);
    p.note_on(0, second, 0, 127);
    render_blocks(p, out, 160);
    return up_crossings(out, 160 * kBlk, tc, 900);
  };
  // (a) the instrument's glide, 200 ms = 8820 samples, an octave up (48 -> 60): the first period is the old note's (80), the middle one a half octave up (56.6), then 40
  {
    SampleDesc d = ifx_desc(sine);
    d.glide = 200;
    p.set_sample(0, d);
    const std::uint32_t c = note_pair(48, 60);
    NEED(c > 150u);
    const double first = tc[1] - tc[0];
    CHECK_MESSAGE((first > 74.0 && first < 82.0), "the first period is the old note's: " << first);
    const double mid = mean_period(tc, c, 3800.0, 5000.0);
    CHECK_MESSAGE((mid > 52.0 && mid < 61.0), "mid-glide period " << mid << " (a half octave up is 56.6)");
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 9600.0, 10200.0) - 40.0) < 0.3, "landed: " << mean_period(tc, c, 9600.0, 10200.0));
    // no later start than the time says: the pitch has arrived by 8820 + a block
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 8900.0, 9500.0) - 40.0) < 0.5, "just after the time: " << mean_period(tc, c, 8900.0, 9500.0));
  }
  // (b) down, a fifth (67 -> 60): starts at the fifth up (26.7 samples), ends at 40
  {
    const std::uint32_t c = note_pair(67, 60);
    NEED(c > 150u);
    const double first = tc[1] - tc[0];
    CHECK_MESSAGE((first > 25.0 && first < 28.5), "the first period " << first);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 9600.0, 10200.0) - 40.0) < 0.3, "landed: " << mean_period(tc, c, 9600.0, 10200.0));
  }
  // (c) the same note again does not glide, and a first note after the FX reset... (a repeat is a repeat)
  {
    const std::uint32_t c = note_pair(60, 60);
    NEED(c > 150u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 70.0, 700.0) - 40.0) < 0.3, "a repeated note: " << mean_period(tc, c, 70.0, 700.0));
  }
  // (d) glide 0 in the instrument: none
  {
    p.set_sample(0, ifx_desc(sine));
    const std::uint32_t c = note_pair(48, 60);
    NEED(c > 150u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 70.0, 700.0) - 40.0) < 0.3, "no glide: " << mean_period(tc, c, 70.0, 700.0));
  }
  // (e) the Glide FX over an instrument with none: value 10 = 200 ms, the same ramp; value 0 over an instrument with 200 ms: none
  {
    p.fx_event(0, o3os::audio::kModGroupGlide, o3os::audio::kFxGlide, 10);
    const std::uint32_t c = note_pair(48, 60);
    NEED(c > 150u);
    const double first = tc[1] - tc[0];
    CHECK_MESSAGE((first > 74.0 && first < 82.0), "FX glide, first period " << first);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 9600.0, 10200.0) - 40.0) < 0.3, "FX glide landed: " << mean_period(tc, c, 9600.0, 10200.0));
    SampleDesc d = ifx_desc(sine);
    d.glide = 200;
    p.set_sample(0, d);
    p.fx_event(0, o3os::audio::kModGroupGlide, o3os::audio::kFxGlide, 0);
    const std::uint32_t c2 = note_pair(48, 60);
    NEED(c2 > 150u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c2, 70.0, 700.0) - 40.0) < 0.3, "FX glide 0: " << mean_period(tc, c2, 70.0, 700.0));
    p.fx_event(0, o3os::audio::kModGroupGlide, 0, 0);  // ended: the instrument's 200 ms again
    const std::uint32_t c3 = note_pair(48, 60);
    NEED(c3 > 150u);
    CHECK_MESSAGE((tc[1] - tc[0]) > 74.0, "the FX ended: " << (tc[1] - tc[0]));
  }
  // (f) a Slide FX held by the track has the ramp: the note takes the slide, not the glide
  {
    SampleDesc d = ifx_desc(sine);
    d.glide = 200;
    p.set_sample(0, d);
    p.clear_slide_fxs();
    p.set_tempo_x100(80000);
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 0);
    const std::uint32_t c = note_pair(48, 60);
    NEED(c > 150u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 70.0, 700.0) - 40.0) < 0.5, "Slide wins: " << mean_period(tc, c, 70.0, 700.0));
  }
  p.clear_slide_fxs();
  p.set_sample(0, ifx_desc(sine));
}

// ---- OS-RATEPITCH: the sample's own rate (voices.h, rate_factor_q28) -------------------------------------------------------------------

TEST_CASE("ratepitch: the factor is 0 (unity) for 0 and 44,100 Hz, R / 44100 in Q28 otherwise, and scales the increment exactly at 22,050 Hz") {
  using o3os::audio::pitch_inc_q32;
  using o3os::audio::rate_factor_q28;
  using o3os::audio::rate_scale_q32;
  CHECK(rate_factor_q28(0) == 0u);
  CHECK(rate_factor_q28(44100) == 0u);
  CHECK(rate_factor_q28(22050) == (1u << 27));  // exactly one half
  CHECK(rate_factor_q28(88200) == (1u << 29));
  CHECK(rate_factor_q28(1) == rate_factor_q28(o3os::audio::kRateMinHz));  // held to the range, never a stopped voice
  CHECK(rate_factor_q28(4000000000u) == rate_factor_q28(o3os::audio::kRateMaxHz));
  for (unsigned note = 0; note < 128; ++note) {
    const std::uint64_t inc = pitch_inc_q32(note);
    CHECK(rate_scale_q32(inc, 0) == inc);                         // unity: untouched
    CHECK(rate_scale_q32(inc, rate_factor_q28(44100)) == inc);
    CHECK(rate_scale_q32(inc, 1u << 28) == inc);                  // a factor of exactly 1.0 is exact too
    CHECK(rate_scale_q32(inc, rate_factor_q28(22050)) == (inc >> 1));
    for (const std::uint32_t hz : {8287u, 16726u, 22095u, 48000u, 96000u}) {
      const double want = static_cast<double>(inc) * static_cast<double>(hz) / 44100.0;
      const double got = static_cast<double>(rate_scale_q32(inc, rate_factor_q28(hz)));
      CHECK_MESSAGE(std::fabs(got - want) <= want * 1e-8 + 2.0, "note " << note << " rate " << hz);
    }
  }
}

namespace {

SampleDesc rate_desc(const std::int16_t* s, std::uint32_t n, std::uint32_t hz) {
  SampleDesc d{s, n, 0, 0, 65536};
  d.rate_q28 = o3os::audio::rate_factor_q28(hz);
  return d;
}

// The mean period of the sine40 voice's first 40 blocks, in output samples.
double rate_period(const std::int16_t* out, double* tc, std::uint32_t from) {
  const std::uint32_t c = up_crossings(out, 40 * kBlk, tc, 900);
  return c > 3u ? mean_period(tc, c, static_cast<double>(from), 40.0 * kBlk) : 0.0;
}

}  // namespace

TEST_CASE("ratepitch: a sample plays at its own pitch through the voice: 22,050 Hz advances 0.5 frames, 48,000 Hz 48000 / 44100, 44,100 Hz and rate 0 are bit for bit the plain voice") {
  static std::int16_t sine[16000];
  static std::int16_t out[40 * kBlk];
  static std::int16_t ref[40 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});  // the plain voice
  p.note_on(0, 60, 0, 127);
  render_blocks(p, ref, 40);
  CHECK(std::fabs(rate_period(ref, tc, 100) - 40.0) < 0.01);
  for (const std::uint32_t hz : {0u, 44100u}) {
    p.cut(0);
    render_blocks(p, out, 6);
    p.set_sample(0, rate_desc(sine, 16000, hz));
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 40);
    CHECK(std::memcmp(out, ref, sizeof out) == 0);  // no rounding change at all
  }
  struct Case {
    std::uint32_t hz;
    double period;
  };
  for (const Case c : {Case{22050, 80.0}, Case{48000, 40.0 * 44100.0 / 48000.0}, Case{16726, 40.0 * 44100.0 / 16726.0}, Case{22095, 40.0 * 44100.0 / 22095.0}}) {
    p.cut(0);
    render_blocks(p, out, 6);
    p.set_sample(0, rate_desc(sine, 16000, c.hz));
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 40);
    const double got = rate_period(out, tc, 100);
    CHECK_MESSAGE(std::fabs(got - c.period) < c.period * 0.002, "rate " << c.hz << ": period " << got << " want " << c.period);
  }
  p.cut(0);
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
}

TEST_CASE("ratepitch: the factor survives the tune, the fine tune, the Slide FX, the Glide and the tracker FX of a 22,050 Hz sample") {
  static std::int16_t sine[16000];
  static std::int16_t out[160 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.clear_slide_fxs();
  p.set_tempo_x100(80000);
  const auto settle = [&] {
    p.cut(0);
    render_blocks(p, out, 6);
  };
  // (a) tune +12: half the rate, an octave up: the plain root period 40
  {
    SampleDesc d = rate_desc(sine, 16000, 22050);
    d.tune = 12;
    p.set_sample(0, d);
    settle();
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 40);
    CHECK(std::fabs(rate_period(out, tc, 100) - 40.0) < 0.1);
  }
  // (b) fine tune 100: toward the next semitone, linear: increment 0.5 x 1.059463 -> period 80 / 1.059463
  {
    SampleDesc d = rate_desc(sine, 16000, 22050);
    d.fine_tune = 100;
    p.set_sample(0, d);
    settle();
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 40);
    CHECK(std::fabs(rate_period(out, tc, 100) - 80.0 / 1.0594631) < 0.15);
  }
  // (c) the Slide FX an octave up (repitch, every block of the ramp): the landed period is the octave's, 40
  {
    p.set_sample(0, rate_desc(sine, 16000, 22050));
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);
    settle();
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 56);
    const std::uint32_t c = up_crossings(out, 56 * kBlk, tc, 900);
    NEED(c > 20u);
    CHECK_MESSAGE((tc[1] - tc[0]) > 70.0, "the note starts at its own pitch: " << (tc[1] - tc[0]));
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1200.0, 3500.0) - 40.0) < 0.3, "landed " << mean_period(tc, c, 1200.0, 3500.0));
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 0);  // back to the note, the FX still held: cleared below
    p.clear_slide_fxs();
  }
  // (d) the tracker FX's offset (tracker_set): +12 semitones at once, mod_block remakes the increment: 40
  {
    p.set_sample(0, rate_desc(sine, 16000, 22050));
    settle();
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 4);
    p.tracker_set(0, 12 * 65536, -1);
    render_blocks(p, out + 4 * kBlk, 36);
    const std::uint32_t c = up_crossings(out, 40 * kBlk, tc, 900);
    NEED(c > 10u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 100.0, 4.0 * kBlk) - 80.0) < 0.3, "before: " << mean_period(tc, c, 100.0, 4.0 * kBlk));
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 6.0 * kBlk, 40.0 * kBlk) - 40.0) < 0.3, "after: " << mean_period(tc, c, 6.0 * kBlk, 40.0 * kBlk));
  }
  // (e) the instrument's glide, 200 ms, 48 -> 60: starts at the old note's pitch (160 at 22,050 Hz) and lands on 80
  {
    SampleDesc d = rate_desc(sine, 16000, 22050);
    d.glide = 200;
    p.set_sample(0, d);
    settle();
    p.note_on(0, 48, 0, 127);
    render_blocks(p, out, 4);
    p.cut(0);
    render_blocks(p, out, 6);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 160);
    const std::uint32_t c = up_crossings(out, 160 * kBlk, tc, 900);
    NEED(c > 60u);
    CHECK_MESSAGE(((tc[1] - tc[0]) > 150.0 && (tc[1] - tc[0]) < 170.0), "glide starts at the old note's pitch: " << (tc[1] - tc[0]));
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 9600.0, 10200.0) - 80.0) < 0.6, "glide landed: " << mean_period(tc, c, 9600.0, 10200.0));
  }
  p.cut(0);
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
}

TEST_CASE("ratepitch: note 127 with tune +24 at 192 kHz and 384 kHz does not overflow and plays at the pitch computed (the table holds at its last note)") {
  using o3os::audio::pitch_inc_q32;
  using o3os::audio::rate_factor_q28;
  using o3os::audio::rate_scale_q32;
  using o3os::audio::tuned_inc_q32;
  static std::int16_t sine[16000];
  static std::int16_t out[2 * kBlk];
  make_sine(sine, 16000);
  const double top = std::exp2(67.0 / 12.0);  // note 127 against the root 60
  for (const std::uint32_t hz : {192000u, 384000u}) {
    const std::uint64_t inc = rate_scale_q32(tuned_inc_q32(127, 24, 0), rate_factor_q28(hz));
    const double ratio = top * static_cast<double>(hz) / 44100.0;  // frames a sample: 209 and 418
    CHECK_MESSAGE(std::fabs(static_cast<double>(inc) / 4294967296.0 - ratio) < ratio * 1e-6, "rate " << hz);
    CHECK(tuned_inc_q32(127, 24, 0) == pitch_inc_q32(127));
    CHECK((inc >> 41) == 0u);  // under 2^41: a 64-bit increment with room to spare, no wrap
    VoicePool& p = pool();
    p.clear_slide_fxs();
    SampleDesc d = rate_desc(sine, 16000, hz);
    d.tune = 24;
    p.set_sample(0, d);
    CHECK(p.note_on(0, 127, 0, 127));
    render_blocks(p, out, 2);
    std::int32_t worst = 0;
    std::uint32_t checked = 0;
    for (std::uint32_t k = 0; k < 2 * kBlk; ++k) {
      const double pos = static_cast<double>(k) * ratio;
      if (pos >= 15999.0) {
        break;  // a one shot ends here
      }
      const double expect = reference(sine, 16000, 0, 0, pos) * attack(k);
      const std::int32_t err = static_cast<std::int32_t>(std::lround(std::fabs(static_cast<double>(out[k]) - expect)));
      worst = err > worst ? err : worst;
      ++checked;
    }
    CHECK(checked > 30u);
    CHECK_MESSAGE(worst <= 2, "rate " << hz << ": worst error " << worst << " LSB over " << checked << " samples");
    CHECK_FALSE(p.track_playing(0));  // 16000 frames at 209 / 418 a sample: over within the second block
    p.cut(0);
  }
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
}

TEST_CASE("ratepitch: the factor holds on a backward and a ping-pong loop: 88,200 Hz at the root walks the reference positions at a step of 2, 22,050 Hz at a step of 1 from note 72") {
  static std::int16_t ramp[120];
  for (int i = 0; i < 120; ++i) {
    ramp[i] = static_cast<std::int16_t>(i * 100);
  }
  static std::int16_t out[64 * 10];
  struct Case {
    std::uint32_t hz;
    unsigned note;
    int step;
  };
  for (const bool pingpong : {false, true}) {
    for (const Case c : {Case{88200, 60, 2}, Case{22050, 72, 1}}) {
      VoicePool& p = pool();
      SampleDesc d = rate_desc(ramp, 120, c.hz);
      d.loop_start = 20;
      d.loop_end = 100;
      d.loop_mode = pingpong ? 2 : 1;
      p.set_sample(0, d);
      p.note_on(0, c.note, 0, o3os::audio::kVelocityDefault);
      render_blocks(p, out, 10);
      const std::uint8_t* walk = og_walk(pingpong, c.step);
      NEED(walk != nullptr);
      std::uint32_t wrong = 0;
      for (std::uint32_t k = 0; k < 64u * 10u; ++k) {
        const double want = ramp[walk[k]] * attack(k);
        if (std::fabs(out[k] - want) > 1.01) {
          if (++wrong <= 5u) {
            CHECK_MESSAGE(false, (pingpong ? "ping-pong" : "backward") << " rate " << c.hz << " sample " << k << ": " << out[k] << ", the recorded walk says " << want);
          }
        }
      }
      CHECK(wrong == 0u);
      CHECK(p.track_playing(0));
      p.cut(0);
    }
  }
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{ramp, 120, 0, 0, 65536});
}

// ---- OS-PERFFX P5: the performance layer's TUNE (voices.h PERFORMANCE LAYER, TUNE). The layer's value is the base of the pitch offset the Slide FX moves, in whole semitones, -48..48.

namespace {

SeqEvent tune_layer(int value, std::uint8_t track = 0) {
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.track = track;
  e.flags = o3os::audio::kModGroupTune;
  e.c = o3os::audio::kLayerPerf;
  e.value = value;
  return e;
}

SeqEvent tune_end(std::uint8_t track = 0) {
  SeqEvent e{};
  e.kind = EventKind::ModEnd;
  e.track = track;
  e.flags = o3os::audio::kModGroupTune;
  return e;
}

}  // namespace

TEST_CASE("perform layer (P5): TUNE at a note's start is the note moved by whole semitones, bit for bit, -48..48, held at the ends of the table and the value held to its range") {
  static std::int16_t sine[16000];
  static std::int16_t want[10 * kBlk];
  static std::int16_t got[10 * kBlk];
  static std::int16_t flush[8 * kBlk];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
  struct Row {
    unsigned note;
    int value;  // what the layer is given
    unsigned ref;  // the plain note that sounds the same
  };
  const Row rows[] = {{60, 12, 72}, {60, -12, 48}, {60, 1, 61}, {60, 48, 108}, {60, -48, 12}, {100, 48, 127}, {10, -48, 0}, {60, 1000, 108}, {60, -1000, 12}, {60, 0, 60}};
  for (const Row& r : rows) {
    CAPTURE(r.note);
    CAPTURE(r.value);
    p.perf_clear();
    p.clear_slide_fxs();
    p.cut(0);
    render_blocks(p, flush, 8);
    p.note_on(0, r.ref, 0, 127);
    render_blocks(p, want, 10);
    p.cut(0);
    render_blocks(p, flush, 8);
    p.fx_event(0, tune_layer(r.value));
    CHECK(p.perf_held(0, o3os::audio::kModGroupTune));
    p.note_on(0, r.note, 0, 127);
    render_blocks(p, got, 10);
    CHECK(std::memcmp(want, got, sizeof want) == 0);
  }
  // the layer's value is held to 48 in the slot, a ModEnd (or a step ModSet of the group) lets it go, and another track or group is not touched
  p.perf_clear();
  p.fx_event(0, tune_layer(1000));
  CHECK(p.perf_value(0, o3os::audio::kModGroupTune) == 48);
  p.fx_event(0, tune_layer(-1000));
  CHECK(p.perf_value(0, o3os::audio::kModGroupTune) == -48);
  CHECK_FALSE(p.perf_held(1, o3os::audio::kModGroupTune));
  p.fx_event(0, tune_end());
  CHECK_FALSE(p.perf_held(0, o3os::audio::kModGroupTune));
  p.fx_event(0, tune_layer(7));
  SeqEvent step = tune_layer(7);
  step.c = o3os::audio::kLayerFx1;  // a ModSet that is not the layer's: the layer lets go
  p.fx_event(0, step);
  CHECK_FALSE(p.perf_held(0, o3os::audio::kModGroupTune));
  p.perf_clear();
}

TEST_CASE("perform layer (P5): TUNE moves the sounding note at the next block, back and forth, and its end gives the note's own pitch back; the transport's FxReset and Stopped leave it") {
  static std::int16_t sine[16000];
  static std::int16_t out[64 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
  p.perf_clear();
  p.clear_slide_fxs();
  p.note_on(0, 60, 0, 127);
  render_blocks(p, out, 4);
  p.fx_event(0, tune_layer(12));
  render_blocks(p, out + 4 * kBlk, 20);
  std::uint32_t c = up_crossings(out, 24 * kBlk, tc, 900);
  NEED(c > 20u);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 70.0, 250.0) - 40.0) < 0.3, "before: " << mean_period(tc, c, 70.0, 250.0));
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 4.0 * kBlk, 5.0 * kBlk + 16.0) - 20.0) < 1.0, "the block after the event is already an octave up: " << mean_period(tc, c, 4.0 * kBlk, 5.0 * kBlk + 16.0));
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 600.0, 1500.0) - 20.0) < 0.2, "+12: " << mean_period(tc, c, 600.0, 1500.0));
  p.fx_event(0, tune_layer(-12));
  render_blocks(p, out, 40);
  c = up_crossings(out, 40 * kBlk, tc, 900);
  NEED(c > 20u);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 2500.0) - 80.0) < 0.5, "-12: " << mean_period(tc, c, 200.0, 2500.0));
  // the transport starts and stops: the layer stays (and the sounding note with it)
  p.clear_slide_fxs();
  p.transport_stopped();
  CHECK(p.perf_value(0, o3os::audio::kModGroupTune) == -12);
  render_blocks(p, out, 40);
  c = up_crossings(out, 40 * kBlk, tc, 900);
  NEED(c > 20u);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 2500.0) - 80.0) < 0.5, "after FxReset and Stopped: " << mean_period(tc, c, 200.0, 2500.0));
  // a ModEnd: the note's own pitch
  p.fx_event(0, tune_end());
  render_blocks(p, out, 24);
  c = up_crossings(out, 24 * kBlk, tc, 900);
  NEED(c > 20u);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 1500.0) - 40.0) < 0.3, "after the end: " << mean_period(tc, c, 200.0, 1500.0));
  // the layer on another track does not move this one
  p.fx_event(3, tune_layer(24));
  render_blocks(p, out, 24);
  c = up_crossings(out, 24 * kBlk, tc, 900);
  NEED(c > 20u);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 1500.0) - 40.0) < 0.3, "another track's layer: " << mean_period(tc, c, 200.0, 1500.0));
  p.perf_clear();
}

TEST_CASE("perform layer (P5): the base survives the Slide FX, the Glide and the tracker FX - each moves the pitch around it - and a TUNE that changes mid-ramp moves the ramp's ends together") {
  static std::int16_t sine[16000];
  static std::int16_t out[160 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.set_tempo_x100(80000);
  // (a) TUNE -12 held, Slide up one octave (192 sixteenths) before the note: the note starts at the base (80 samples a period, not the note's 40), slides to 0 (40), and the FX's end is the base
  {
    p.perf_clear();
    p.clear_slide_fxs();
    p.cut(0);
    render_blocks(p, out, 8);
    p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
    p.fx_event(0, tune_layer(-12));
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 56);
    std::uint32_t c = up_crossings(out, 56 * kBlk, tc, 900);
    NEED(c > 40u);
    const double first = tc[1] - tc[0];
    CHECK_MESSAGE((first > 72.0 && first < 82.0), "the first period is the base's: " << first);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1200.0, 3500.0) - 40.0) < 0.3, "landed at the base + 12: " << mean_period(tc, c, 1200.0, 3500.0));
    p.fx_event(0, o3os::audio::kModGroupSlide, 0, 0);  // the FX ends while it is up: the base, not 0
    render_blocks(p, out, 40);
    c = up_crossings(out, 40 * kBlk, tc, 900);
    NEED(c > 20u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 2500.0) - 80.0) < 0.5, "after the FX ends: " << mean_period(tc, c, 200.0, 2500.0));
    // a Slide of 0 held on the track and a note after it: the base again
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 0);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 40);
    c = up_crossings(out, 40 * kBlk, tc, 900);
    NEED(c > 20u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 2500.0) - 80.0) < 0.5, "a Slide of 0: " << mean_period(tc, c, 200.0, 2500.0));
    p.clear_slide_fxs();
  }
  // (b) the TUNE changes while the ramp runs (-12 to 0 at block 4 of 56): the ramp goes on to its own target around the new base, an octave above the note
  {
    p.perf_clear();
    p.cut(0);
    render_blocks(p, out, 8);
    p.fx_event(0, tune_layer(-12));
    p.fx_event(0, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 4);
    p.fx_event(0, tune_layer(0));
    render_blocks(p, out + 4 * kBlk, 52);
    const std::uint32_t c = up_crossings(out, 56 * kBlk, tc, 900);
    NEED(c > 40u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1200.0, 3500.0) - 20.0) < 0.2, "landed at the new base + 12: " << mean_period(tc, c, 1200.0, 3500.0));
    p.clear_slide_fxs();
    p.perf_clear();
  }
  // (c) the Glide: the previous note one octave above, TUNE -12: the glide starts at that pitch around the base (period 40), reaches the base (80)
  {
    SampleDesc d = SampleDesc{sine, 16000, 0, 0, 65536};
    d.glide = 200;
    p.set_sample(0, d);
    p.perf_clear();
    p.clear_slide_fxs();
    p.cut(0);
    render_blocks(p, out, 8);
    p.fx_event(0, tune_layer(-12));
    p.note_on(0, 72, 0, 127);
    render_blocks(p, out, 4);
    p.cut(0);
    render_blocks(p, out, 6);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 160);
    const std::uint32_t c = up_crossings(out, 160 * kBlk, tc, 900);
    NEED(c > 100u);
    const double first = tc[1] - tc[0];
    CHECK_MESSAGE((first > 36.0 && first < 44.0), "the glide starts at the previous note's pitch: " << first);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 9600.0, 10200.0) - 80.0) < 0.8, "landed at the base: " << mean_period(tc, c, 9600.0, 10200.0));
    p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
  }
  // (d) the tracker FX's offset (an arpeggio step, a vibrato phase) lands on top of the base and its release goes back to it
  {
    p.perf_clear();
    p.cut(0);
    render_blocks(p, out, 8);
    p.fx_event(0, tune_layer(-12));
    p.note_on(0, 60, 0, 127);
    render_blocks(p, out, 8);
    p.tracker_set(0, 12 * 65536, -1);
    render_blocks(p, out, 24);
    std::uint32_t c = up_crossings(out, 24 * kBlk, tc, 900);
    NEED(c > 20u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 1500.0) - 40.0) < 0.3, "tracker +12 over the base: " << mean_period(tc, c, 200.0, 1500.0));
    p.tracker_set(0, 0, -1);
    render_blocks(p, out, 40);
    c = up_crossings(out, 40 * kBlk, tc, 900);
    NEED(c > 20u);
    CHECK_MESSAGE(std::fabs(mean_period(tc, c, 200.0, 2500.0) - 80.0) < 0.5, "tracker 0: " << mean_period(tc, c, 200.0, 2500.0));
    p.perf_clear();
  }
}

TEST_CASE("perform layer (P5): TUNE and a Slide FX on a track other than 0, on a voice reused from another track - the ramp runs around the new track's base, not the old one's") {
  static std::int16_t sine[16000];
  static std::int16_t out[56 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.set_tempo_x100(80000);
  p.perf_clear();
  p.clear_slide_fxs();
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
  // the one free voice a note takes was last on track 0 and held a note there, with no TUNE on track 0
  p.note_on(0, 60, 0, 127);
  render_blocks(p, out, 4);
  p.cut(0);
  render_blocks(p, out, 8);
  CHECK_FALSE(p.track_playing(0));
  // track 1 holds TUNE -12 and a Slide up one octave (192 sixteenths): the note starts at the base (80 samples a period), lands at its own pitch (40), the target being the slide plus ITS base
  p.fx_event(1, tune_layer(-12, 1));
  p.fx_event(1, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideUp, 192);
  p.note_on(1, 60, 0, 127);
  render_blocks(p, out, 56, 1);
  std::uint32_t c = up_crossings(out, 56 * kBlk, tc, 900);
  NEED(c > 40u);
  const double first = tc[1] - tc[0];
  CHECK_MESSAGE((first > 72.0 && first < 82.0), "the first period is the base's: " << first);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1200.0, 3500.0) - 40.0) < 0.3, "landed at the base + 12: " << mean_period(tc, c, 1200.0, 3500.0));
  p.clear_slide_fxs();
  p.perf_clear();
  p.cut(1);
  render_blocks(p, out, 8, 1);
}

TEST_CASE("perform layer (P5): TUNE, a Slide FX and a Glide time on a voice reused from another track - the Slide has the ramp and the base is the track's own") {
  static std::int16_t sine[16000];
  static std::int16_t out[56 * kBlk];
  static double tc[900];
  make_sine40(sine, 16000);
  VoicePool& p = pool();
  p.set_tempo_x100(80000);
  p.perf_clear();
  p.clear_slide_fxs();
  SampleDesc d = SampleDesc{sine, 16000, 0, 0, 65536};
  d.glide = 200;
  p.set_sample(0, d);
  // track 2 has a previous note (the glide would start from it: an octave up), then the one voice goes to track 1 and is free again, so the next note on track 2 reuses a voice last on track 1
  p.note_on(2, 72, 0, 127);
  render_blocks(p, out, 4, 2);
  p.cut(2);
  render_blocks(p, out, 8, 2);
  p.note_on(1, 60, 0, 127);
  render_blocks(p, out, 4, 1);
  p.cut(1);
  render_blocks(p, out, 8, 1);
  CHECK_FALSE(p.track_playing(1));
  // track 2: TUNE +12, a Slide down one octave: the note starts at the base (20), lands at its own pitch (40); the Glide time stays out of it (the Slide has the ramp)
  p.fx_event(2, tune_layer(12, 2));
  p.fx_event(2, o3os::audio::kModGroupSlide, o3os::audio::kFxSlideDown, 192);
  p.note_on(2, 60, 0, 127);
  render_blocks(p, out, 56, 2);
  const std::uint32_t c = up_crossings(out, 56 * kBlk, tc, 900);
  NEED(c > 40u);
  const double first = tc[1] - tc[0];
  CHECK_MESSAGE((first > 18.0 && first < 22.0), "the first period is the base's, not the Glide's: " << first);
  CHECK_MESSAGE(std::fabs(mean_period(tc, c, 1200.0, 3500.0) - 40.0) < 0.3, "landed at the base - 12: " << mean_period(tc, c, 1200.0, 3500.0));
  p.clear_slide_fxs();
  p.perf_clear();
  p.cut(2);
  render_blocks(p, out, 8, 2);
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
}

// ---- OS-PERFFX P7: the performance layer's SMP POS and SMP END (voices.h PERFORMANCE LAYER, SMP POS AND SMP END). A note starts at the layer's frame and stops at its last frame; the part is the
// slice the note names or the whole sample, the percent is floor(len x f / 100) with f = v for v > 0 and 100 + v for v < 0.

namespace {

SeqEvent smp_layer(unsigned group, int value, std::uint8_t track = 0) {
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.track = track;
  e.flags = static_cast<std::uint8_t>(group);
  e.c = o3os::audio::kLayerPerf;
  e.value = value;
  return e;
}

SeqEvent smp_end(unsigned group, std::uint8_t track = 0) {
  SeqEvent e{};
  e.kind = EventKind::ModEnd;
  e.track = track;
  e.flags = static_cast<std::uint8_t>(group);
  return e;
}

// The frame the spec says (OS-PERFFX section 10.1), in 64 bits, so that the test does not share the arithmetic of the voices.
std::uint32_t smp_want(std::uint32_t len, int v) {
  const std::uint64_t f = v >= 0 ? static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(100 + v);
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(len) * f / 100u);
}

constexpr std::uint32_t kSmpLen = 16000;

}  // namespace

TEST_CASE("perform layer (P7): SMP POS and SMP END at a note's start play the sub-sample, bit for bit, at the limits and between, and end before start plays nothing") {
  static std::int16_t sine[kSmpLen];
  static std::int16_t want[10 * kBlk];
  static std::int16_t got[10 * kBlk];
  static std::int16_t flush[8 * kBlk];
  make_sine40(sine, kSmpLen);
  using o3os::audio::kModGroupPosition;
  using o3os::audio::kModGroupSampleEnd;
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{sine, kSmpLen, 0, 0, 65536});
  struct Row {
    int pos;  // 0: the layer holds none
    int end;
    unsigned note;
  };
  const Row rows[] = {{1, 0, 60},  {25, 0, 60},   {50, 0, 60},  {99, 0, 60},  {-25, 0, 60}, {-50, 0, 72}, {-100, 0, 60}, {0, 100, 60}, {0, 75, 60}, {0, 50, 48},
                      {0, 1, 60},  {0, -25, 60},  {0, -99, 60}, {20, 80, 60}, {-80, -20, 60}, {30, 31, 60}, {10, 100, 84}, {100, 0, 60}, {0, -100, 60}, {70, 30, 60},
                      {40, 40, 60}, {-1, 0, 60},  {0, 0, 60}};
  for (const Row& r : rows) {
    CAPTURE(r.pos);
    CAPTURE(r.end);
    CAPTURE(r.note);
    const std::uint32_t first = r.pos != 0 ? smp_want(kSmpLen, r.pos) : 0u;
    const std::uint32_t last = r.end != 0 ? smp_want(kSmpLen, r.end) : kSmpLen;
    p.perf_clear();
    p.cut(0);
    render_blocks(p, flush, 8);
    if (r.pos != 0) {
      p.fx_event(0, smp_layer(kModGroupPosition, r.pos));
    }
    if (r.end != 0) {
      p.fx_event(0, smp_layer(kModGroupSampleEnd, r.end));
    }
    const std::uint32_t ignored = p.stats().ignored;
    if (first >= last) {  // nothing to play: the note is refused, as a beat slice with no length is, and counted
      CHECK_FALSE(p.note_on(0, r.note, 0, 127));
      CHECK_FALSE(p.track_playing(0));
      CHECK(p.stats().ignored == ignored + 1u);
      continue;
    }
    // the reference: the same bytes as a sample of their own, played with the layer off
    p.perf_clear();
    p.set_sample(1, SampleDesc{sine + first, last - first, 0, 0, 65536});
    p.note_on(0, r.note, 1, 127);
    render_blocks(p, want, 10);
    p.cut(0);
    render_blocks(p, flush, 8);
    if (r.pos != 0) {
      p.fx_event(0, smp_layer(kModGroupPosition, r.pos));
    }
    if (r.end != 0) {
      p.fx_event(0, smp_layer(kModGroupSampleEnd, r.end));
    }
    CHECK(p.note_on(0, r.note, 0, 127));
    render_blocks(p, got, 10);
    CHECK(std::memcmp(want, got, sizeof want) == 0);
    p.cut(0);
  }
  p.perf_clear();
  p.cut(0);
  // the layer off, or let go by a ModEnd, is the plain note, and a note that sounds is not moved by a change of the value
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, want, 10);
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, smp_layer(kModGroupPosition, 50));
  p.fx_event(0, smp_layer(kModGroupSampleEnd, 50));
  p.fx_event(0, smp_end(kModGroupPosition));
  p.fx_event(0, smp_end(kModGroupSampleEnd));
  CHECK_FALSE(p.perf_held(0, kModGroupPosition));
  p.note_on(0, 60, 0, 127);
  render_blocks(p, got, 5);
  p.fx_event(0, smp_layer(kModGroupPosition, 80));  // the sounding note stays where it is
  p.fx_event(0, smp_layer(kModGroupSampleEnd, 1));
  render_blocks(p, got + 5 * kBlk, 5);
  CHECK(std::memcmp(want, got, sizeof want) == 0);
  p.perf_clear();
  p.cut(0);
  // another track is left alone
  render_blocks(p, flush, 8);
  p.fx_event(0, smp_layer(kModGroupPosition, 50, 1));
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 10);
  CHECK(std::memcmp(want, got, sizeof want) == 0);
  p.perf_clear();
  p.cut(0);
  p.set_sample(0, SampleDesc{sine, 16000, 0, 0, 65536});
}

TEST_CASE("perform layer (P7): a tracker Retrigger restarts the voice at the window's start (SMP POS), not at frame 0; without SMP POS at frame 0") {
  static std::int16_t sine[kSmpLen];
  static std::int16_t fresh[12 * kBlk];
  static std::int16_t again[12 * kBlk];
  static std::int16_t flush[8 * kBlk];
  static std::int16_t lead[3 * kBlk];
  static std::int16_t plain[12 * kBlk];  // the note with no layer, from frame 0
  for (std::uint32_t i = 0; i < kSmpLen; ++i) {  // a saw of 2000 frames: 40 % (6400) is not a whole number of periods, so frame 0 and frame 6400 sound different
    sine[i] = static_cast<std::int16_t>(static_cast<int>(i % 2000u) * 12 - 12000);
  }
  using o3os::audio::kModGroupPosition;
  using o3os::audio::kModGroupSampleEnd;
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{sine, kSmpLen, 0, 0, 65536});
  // pos = 0: no layer; 40: the layer holds SMP POS 40 (with SMP END 90 in the second row, which the restart keeps)
  const int rows[][2] = {{0, 0}, {40, 0}, {40, 90}};
  for (const auto& r : rows) {
    CAPTURE(r[0]);
    CAPTURE(r[1]);
    p.perf_clear();
    p.cut(0);
    render_blocks(p, flush, 8);
    if (r[0] != 0) {
      p.fx_event(0, smp_layer(kModGroupPosition, r[0]));
    }
    if (r[1] != 0) {
      p.fx_event(0, smp_layer(kModGroupSampleEnd, r[1]));
    }
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, fresh, 12);  // the note from its start (12 blocks are far inside the window)
    p.cut(0);
    render_blocks(p, flush, 8);
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, lead, 3);  // moves the phase away from the start
    CHECK(p.tracker_retrigger(0));
    render_blocks(p, again, 12);
    // past the restart's short fade of the old output and the note's own attack, the second pass is the first one, sample for sample
    CHECK(std::memcmp(fresh + 8 * kBlk, again + 8 * kBlk, 4 * kBlk * sizeof(std::int16_t)) == 0);
    if (r[0] == 0) {
      std::memcpy(plain, fresh, sizeof plain);
    } else {
      CHECK(std::memcmp(plain + 8 * kBlk, again + 8 * kBlk, 4 * kBlk * sizeof(std::int16_t)) != 0);  // and it is not the frame-0 note
    }
    p.cut(0);
  }
  p.perf_clear();
  p.cut(0);
}

TEST_CASE("perform layer (P7): in a beat slice the percent is of the slice the note names") {
  static std::int16_t ramp[1000];
  static std::uint16_t table[2 * 5];
  static std::int16_t want[6 * kBlk];
  static std::int16_t got[6 * kBlk];
  static std::int16_t flush[8 * kBlk];
  for (std::uint32_t i = 0; i < 1000; ++i) {
    ramp[i] = static_cast<std::int16_t>(static_cast<int>(i) * 30 - 15000);
  }
  const std::uint16_t bounds[5] = {0, 250, 500, 750, 1000};
  for (unsigned i = 0; i < 5; ++i) {
    table[2 * i] = bounds[i];
    table[2 * i + 1] = 0;
  }
  using o3os::audio::kModGroupPosition;
  using o3os::audio::kModGroupSampleEnd;
  VoicePool& p = pool();
  SampleDesc d{ramp, 1000, 0, 0, 65536};
  d.beat_slice = true;
  d.slice_count = 4;
  d.slices = table;
  p.set_sample(0, d);
  for (unsigned slice = 0; slice < 4; ++slice) {
    CAPTURE(slice);
    p.perf_clear();
    p.cut(0);
    render_blocks(p, flush, 8);
    // slice `slice` is the 250 frames from bounds[slice]: POS +40 (frame 100) and END -20 (frame 200) leave the 100 frames [100, 200) of it
    p.set_sample(1, SampleDesc{ramp + bounds[slice] + 100, 100, 0, 0, 65536});
    p.note_on(0, 60, 1, 127);  // the root note: a beat slice plays at the root's pitch whatever the note (slice 0..3 is the note 24..27)
    render_blocks(p, want, 6);
    p.cut(0);
    render_blocks(p, flush, 8);
    p.fx_event(0, smp_layer(kModGroupPosition, 40));
    p.fx_event(0, smp_layer(kModGroupSampleEnd, -20));
    CHECK(p.note_on(0, 24 + slice, 0, 127));
    render_blocks(p, got, 6);
    CHECK(std::memcmp(want, got, sizeof want) == 0);
  }
  p.perf_clear();
  p.cut(0);
  p.set_sample(0, SampleDesc{ramp, 1000, 0, 0, 65536});
}

TEST_CASE("perform layer (P7): a looped sample starts inside its first pass and keeps looping, SMP END is ignored, and a start at the loop's end plays nothing; granular and wavetable ignore both") {
  static std::int16_t data[1000];
  static std::int16_t base[10 * kBlk];
  static std::int16_t got[10 * kBlk];
  static std::int16_t flush[8 * kBlk];
  for (std::uint32_t i = 0; i < 1000; ++i) {
    data[i] = i < 200 ? std::int16_t{1000} : std::int16_t{8000};  // the intro is quiet, the loop [200, 1000) loud
  }
  using o3os::audio::kModGroupPosition;
  using o3os::audio::kModGroupSampleEnd;
  VoicePool& p = pool();
  p.set_sample(0, SampleDesc{data, 1000, 200, 1000, 65536});
  p.perf_clear();
  p.cut(0);
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, base, 10);
  std::int32_t quiet = 0;
  for (std::uint32_t i = o3os::audio::kAttackSamples; i < 64; ++i) {  // the plain note plays the intro first (1000 x the attack ramp's end)
    quiet = std::max<std::int32_t>(quiet, std::abs(base[i]));
  }
  CHECK(quiet < 1500);
  // SMP POS +50 starts at frame 500 of the loop: loud from the first sample past the attack, and still sounding ten blocks on (it loops)
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, smp_layer(kModGroupPosition, 50));
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 10);
  std::int32_t loud = 0;
  for (std::uint32_t i = o3os::audio::kAttackSamples; i < 64; ++i) {
    loud = std::max<std::int32_t>(loud, std::abs(got[i]));
  }
  CHECK(loud > 7000);
  CHECK(std::abs(got[9 * kBlk + 5]) > 7000);
  CHECK(p.track_playing(0));
  // SMP END alone changes nothing for a loop; with SMP POS it leaves the start where it was
  p.cut(0);
  p.perf_clear();
  render_blocks(p, flush, 8);
  p.fx_event(0, smp_layer(kModGroupSampleEnd, 10));
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 10);
  CHECK(std::memcmp(base, got, sizeof base) == 0);
  // a start at loop_end (+100 of the part up to loop_end) plays nothing
  p.cut(0);
  p.fx_event(0, smp_layer(kModGroupPosition, 100));
  const std::uint32_t ignored = p.stats().ignored;
  CHECK_FALSE(p.note_on(0, 60, 0, 127));
  CHECK_FALSE(p.track_playing(0));
  CHECK(p.stats().ignored == ignored + 1u);
  // the full part (-100) is the plain note
  p.perf_clear();
  render_blocks(p, flush, 8);  // (the tail of the cut note out)
  p.fx_event(0, smp_layer(kModGroupPosition, -100));
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 10);
  CHECK(std::memcmp(base, got, sizeof base) == 0);
  // a wavetable window and a granular voice: both ignored, the note is the plain one bit for bit
  static std::int16_t win[256];
  make_sine40(win, 256);
  for (const int kind : {0, 1}) {
    SampleDesc w{kind == 0 ? win : data, kind == 0 ? 256u : 1000u, 0, kind == 0 ? 256u : 0u, 65536};
    if (kind == 0) {
      w.wt_size = 256;
    } else {
      w.granular = true;
      w.gr_len = 441;
      w.gr_pos = 20000;
    }
    p.perf_clear();
    p.set_sample(0, w);
    p.cut(0);
    render_blocks(p, flush, 8);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, base, 10);
    p.cut(0);
    render_blocks(p, flush, 8);
    p.fx_event(0, smp_layer(kModGroupPosition, 50));
    p.fx_event(0, smp_layer(kModGroupSampleEnd, 30));
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, got, 10);
    CHECK_MESSAGE(std::memcmp(base, got, sizeof base) == 0, (kind == 0 ? "wavetable" : "granular"));
  }
  p.perf_clear();
  p.cut(0);
  p.set_sample(0, SampleDesc{data, 1000, 0, 0, 65536});
}

// ---- OS-PERF-REVERSE (P8): the performance layer's PLAYBACK and the step FX 26 (voices.h PERFORMANCE LAYER, PLAYBACK). A one shot or beat slice note of a track that holds the reverse plays its window
// [first, end) from the last frame down to the first and stops. Bit for bit at a whole-frame step (notes 60 and 72) the reference is the window's frames reversed in a sample of their own, played forward.

namespace {

SeqEvent rev_layer(int value, std::uint8_t track = 0) { return smp_layer(o3os::audio::kModGroupReverse, value, track); }

SeqEvent rev_step(unsigned b, std::uint8_t track = 0) {  // the step FX 26 in the first FX column
  SeqEvent e{};
  e.kind = EventKind::ModSet;
  e.track = track;
  e.flags = o3os::audio::kModGroupReverse;
  e.c = o3os::audio::kLayerFx1;
  e.a = o3os::audio::kFxReverse;
  e.b = static_cast<std::uint8_t>(b);
  return e;
}

void fill_ramp(std::int16_t* d, std::uint32_t n) {  // a different value in nearly every frame, and loud enough to see
  for (std::uint32_t i = 0; i < n; ++i) {
    d[i] = static_cast<std::int16_t>(static_cast<int>((i * 37u) % 20011u) - 10000);
  }
}

void reversed_copy(std::int16_t* dst, const std::int16_t* src, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    dst[i] = src[n - 1u - i];
  }
}

}  // namespace

TEST_CASE("perform layer (P8): PLAYBACK plays a one shot backward, bit for bit the reversed frames played forward, and stops after the first frame") {
  static std::int16_t ramp[3 * kBlk + 37];
  static std::int16_t back[3 * kBlk + 37];
  static std::int16_t want[8 * kBlk];
  static std::int16_t got[8 * kBlk];
  static std::int16_t flush[8 * kBlk];
  VoicePool& p = pool();
  // a length that ends inside a block, one that ends exactly at a block's end (the stop comes at the call's last sample), one block, and five frames
  for (const std::uint32_t len : {3 * kBlk + 37u, 3 * kBlk, kBlk, std::uint32_t{5}}) {  // (all std::uint32_t: on the target that is unsigned long, and 3u * kBlk is not)
    for (const unsigned note : {60u, 72u}) {
      CAPTURE(len);
      CAPTURE(note);
      fill_ramp(ramp, len);
      reversed_copy(back, ramp, len);
      p.perf_clear();
      p.set_sample(0, SampleDesc{ramp, len, 0, 0, 65536});
      p.set_sample(1, SampleDesc{back, len, 0, 0, 65536});
      p.cut(0);
      render_blocks(p, flush, 8);
      p.note_on(0, note, 1, 127);
      render_blocks(p, want, 8);
      CHECK_FALSE(p.track_playing(0));
      p.cut(0);
      render_blocks(p, flush, 8);
      p.fx_event(0, rev_layer(1));
      CHECK(p.note_on(0, note, 0, 127));
      render_blocks(p, got, 8);
      CHECK(std::memcmp(want, got, sizeof want) == 0);
      CHECK_FALSE(p.track_playing(0));  // it stopped below the first frame, and nothing sounds after it
      std::int32_t peak = 0;
      for (std::uint32_t i = 0; i < 8 * kBlk; ++i) {
        peak = std::max<std::int32_t>(peak, std::abs(got[i]));
      }
      CHECK(peak > 100);
    }
  }
  // the half-frame step (note 48): the same sound within a count (the interpolation rounds the other way round), and it ends
  fill_ramp(ramp, 3 * kBlk);
  reversed_copy(back, ramp, 3 * kBlk);
  p.perf_clear();
  p.set_sample(0, SampleDesc{ramp, 3 * kBlk, 0, 0, 65536});
  p.set_sample(1, SampleDesc{back, 3 * kBlk, 0, 0, 65536});
  p.cut(0);
  render_blocks(p, flush, 8);
  p.note_on(0, 48, 1, 127);
  render_blocks(p, want, 8);
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, rev_layer(1));
  CHECK(p.note_on(0, 48, 0, 127));
  render_blocks(p, got, 8);
  std::int32_t worst = 0;
  for (std::uint32_t i = 0; i < 6 * kBlk - 2; ++i) {
    worst = std::max<std::int32_t>(worst, std::abs(want[i] - got[i]));
  }
  CHECK(worst <= 2);
  CHECK_FALSE(p.track_playing(0));
  p.perf_clear();
  p.cut(0);
  p.set_sample(0, SampleDesc{ramp, 16000, 0, 0, 65536});
}

TEST_CASE("perform layer (P8): PLAYBACK 0 or its ModEnd plays forward again, a sounding note is not moved, and another track's layer is not this track's") {
  static std::int16_t ramp[4000];
  static std::int16_t fwd[8 * kBlk];
  static std::int16_t got[8 * kBlk];
  static std::int16_t flush[8 * kBlk];
  fill_ramp(ramp, 4000);
  VoicePool& p = pool();
  p.perf_clear();
  p.set_sample(0, SampleDesc{ramp, 4000, 0, 0, 65536});
  p.cut(0);
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, fwd, 8);
  // value 0 and a ModEnd of the track: the plain forward note; the layer of another track: this track's note is still backward
  const SeqEvent ends[] = {rev_layer(0), smp_end(o3os::audio::kModGroupReverse), rev_layer(1, 1)};
  for (const SeqEvent& e : ends) {
    p.cut(0);
    p.perf_clear();
    render_blocks(p, flush, 8);
    p.fx_event(0, rev_layer(1));
    p.fx_event(e.track, e);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, got, 8);
    if (e.track == 0u) {
      CHECK(std::memcmp(fwd, got, sizeof fwd) == 0);
    } else {
      CHECK(std::memcmp(fwd, got, sizeof fwd) != 0);
    }
  }
  // a note that sounds is not moved by the layer, in either direction
  p.cut(0);
  p.perf_clear();
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 0, 127);
  render_blocks(p, got, 2);
  p.fx_event(0, rev_layer(1));
  render_blocks(p, got + 2 * kBlk, 6);
  CHECK(std::memcmp(fwd, got, sizeof fwd) == 0);
  p.cut(0);
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 0, 127);  // the layer is held: this one starts backward
  render_blocks(p, got, 2);
  p.fx_event(0, smp_end(o3os::audio::kModGroupReverse));
  render_blocks(p, got + 2 * kBlk, 6);
  CHECK(std::memcmp(fwd, got, sizeof fwd) != 0);
  CHECK(p.track_playing(0));  // 4000 frames backward: still sounding after eight blocks, and still going backward
  p.perf_clear();
  p.cut(0);
}

TEST_CASE("perform layer (P8): the step FX 26 reverses the same way, with 0 it is forward, and tracker_reset ends it and not the layer") {
  static std::int16_t ramp[3 * kBlk + 5];
  static std::int16_t back[3 * kBlk + 5];
  static std::int16_t want[6 * kBlk];
  static std::int16_t got[6 * kBlk];
  static std::int16_t flush[8 * kBlk];
  const std::uint32_t len = 3 * kBlk + 5;
  fill_ramp(ramp, len);
  reversed_copy(back, ramp, len);
  VoicePool& p = pool();
  p.perf_clear();
  p.set_sample(0, SampleDesc{ramp, len, 0, 0, 65536});
  p.set_sample(1, SampleDesc{back, len, 0, 0, 65536});
  p.cut(0);
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 1, 127);
  render_blocks(p, want, 6);
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, rev_step(1));
  CHECK(p.step_reverse(0));
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 6);
  CHECK(std::memcmp(want, got, sizeof want) == 0);
  // 0 is forward
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, rev_step(0));
  CHECK_FALSE(p.step_reverse(0));
  p.note_on(0, 60, 0, 127);
  render_blocks(p, got, 6);
  CHECK(std::memcmp(want, got, sizeof want) != 0);
  // the layer's PLAYBACK 1 over a step FX 26 = 0 is backward (either source reverses)
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, rev_layer(1));
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 6);
  CHECK(std::memcmp(want, got, sizeof want) == 0);
  p.fx_event(0, rev_layer(0));
  // the transport start (FxReset reaches tracker_reset) ends the step FX, and leaves the layer
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, rev_step(1));
  p.fx_event(1, rev_layer(1, 1));
  p.tracker_reset();
  CHECK_FALSE(p.step_reverse(0));
  CHECK(p.perf_held(1, o3os::audio::kModGroupReverse));
  p.note_on(0, 60, 0, 127);
  render_blocks(p, got, 6);
  CHECK(std::memcmp(want, got, sizeof want) != 0);
  p.perf_clear();
  p.cut(0);
  p.cut(1);
  p.set_sample(0, SampleDesc{ramp, 16000, 0, 0, 65536});
}

TEST_CASE("perform layer (P8): with SMP POS and SMP END the window plays backward from its last frame to its first, and in a beat slice the slice is the part") {
  static std::int16_t ramp[1000];
  static std::int16_t win[1000];
  static std::uint16_t table[2 * 5];
  static std::int16_t want[6 * kBlk];
  static std::int16_t got[6 * kBlk];
  static std::int16_t flush[8 * kBlk];
  fill_ramp(ramp, 1000);
  using o3os::audio::kModGroupPosition;
  using o3os::audio::kModGroupSampleEnd;
  VoicePool& p = pool();
  // the whole sample, window [200, 700) (POS +20, END +70)
  p.perf_clear();
  p.set_sample(0, SampleDesc{ramp, 1000, 0, 0, 65536});
  reversed_copy(win, ramp + 200, 500);
  p.set_sample(1, SampleDesc{win, 500, 0, 0, 65536});
  p.cut(0);
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 1, 127);
  render_blocks(p, want, 6);
  p.cut(0);
  render_blocks(p, flush, 8);
  p.fx_event(0, smp_layer(kModGroupPosition, 20));
  p.fx_event(0, smp_layer(kModGroupSampleEnd, 70));
  p.fx_event(0, rev_layer(1));
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 6);
  CHECK(std::memcmp(want, got, sizeof want) == 0);
  // the window is 500 frames, 7.8 blocks of kBlk = 64: the note still sounds after 6 and after 7 blocks and ends inside the 8th (a forward note of the same frames ends there too)
  CHECK(p.track_playing(0));
  render_blocks(p, flush, 1);
  CHECK(p.track_playing(0));
  render_blocks(p, flush, 1);
  CHECK_FALSE(p.track_playing(0));
  // SMP POS alone: [400, 1000); SMP END alone: [0, 300)
  const int cases[][2] = {{40, 0}, {0, 30}};
  for (const auto& c : cases) {
    const std::uint32_t first = c[0] != 0 ? smp_want(1000, c[0]) : 0u;
    const std::uint32_t last = c[1] != 0 ? smp_want(1000, c[1]) : 1000u;
    p.perf_clear();
    reversed_copy(win, ramp + first, last - first);
    p.set_sample(1, SampleDesc{win, last - first, 0, 0, 65536});
    p.cut(0);
    render_blocks(p, flush, 8);
    p.note_on(0, 60, 1, 127);
    render_blocks(p, want, 6);
    p.cut(0);
    render_blocks(p, flush, 8);
    if (c[0] != 0) {
      p.fx_event(0, smp_layer(kModGroupPosition, c[0]));
    }
    if (c[1] != 0) {
      p.fx_event(0, smp_layer(kModGroupSampleEnd, c[1]));
    }
    p.fx_event(0, rev_layer(1));
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, got, 6);
    CHECK(std::memcmp(want, got, sizeof want) == 0);
  }
  // an empty window plays nothing, reversed or not
  p.perf_clear();
  p.fx_event(0, smp_layer(kModGroupPosition, 80));
  p.fx_event(0, smp_layer(kModGroupSampleEnd, 50));
  p.fx_event(0, rev_layer(1));
  CHECK_FALSE(p.note_on(0, 60, 0, 127));
  // a beat slice: slice 2 is [500, 750); POS +40 and END -20 leave [600, 700) of the file, played backward at the root's pitch
  const std::uint16_t bounds[5] = {0, 250, 500, 750, 1000};
  for (unsigned i = 0; i < 5; ++i) {
    table[2 * i] = bounds[i];
    table[2 * i + 1] = 0;
  }
  SampleDesc d{ramp, 1000, 0, 0, 65536};
  d.beat_slice = true;
  d.slice_count = 4;
  d.slices = table;
  for (const bool window : {false, true}) {
    p.perf_clear();
    p.set_sample(0, d);
    const std::uint32_t first = window ? 500u + 100u : 500u;
    const std::uint32_t last = window ? 500u + 200u : 750u;
    reversed_copy(win, ramp + first, last - first);
    p.set_sample(1, SampleDesc{win, last - first, 0, 0, 65536});
    p.cut(0);
    render_blocks(p, flush, 8);
    p.note_on(0, 60, 1, 127);
    render_blocks(p, want, 6);
    p.cut(0);
    render_blocks(p, flush, 8);
    if (window) {
      p.fx_event(0, smp_layer(kModGroupPosition, 40));
      p.fx_event(0, smp_layer(kModGroupSampleEnd, -20));
    }
    p.fx_event(0, rev_layer(1));
    CHECK(p.note_on(0, 26, 0, 127));  // the note 26 names slice 2
    render_blocks(p, got, 6);
    CHECK_MESSAGE(std::memcmp(want, got, sizeof want) == 0, (window ? "slice window" : "slice"));
  }
  p.perf_clear();
  p.cut(0);
  p.set_sample(0, SampleDesc{ramp, 1000, 0, 0, 65536});
}

TEST_CASE("perform layer (P8): a tracker Retrigger restarts a backward note from its window's last frame") {
  static std::int16_t ramp[12 * kBlk];
  static std::int16_t fresh[12 * kBlk];
  static std::int16_t again[12 * kBlk];
  static std::int16_t lead[3 * kBlk];
  static std::int16_t flush[8 * kBlk];
  const std::uint32_t len = 12 * kBlk;
  fill_ramp(ramp, len);
  VoicePool& p = pool();
  for (const int pos : {0, 25}) {  // the whole sample, and the window from 25 %
    CAPTURE(pos);
    p.perf_clear();
    p.set_sample(0, SampleDesc{ramp, len, 0, 0, 65536});
    p.cut(0);
    render_blocks(p, flush, 8);
    p.fx_event(0, rev_layer(1));
    if (pos != 0) {
      p.fx_event(0, smp_layer(o3os::audio::kModGroupPosition, pos));
    }
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, fresh, 12);
    p.cut(0);
    render_blocks(p, flush, 8);
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, lead, 3);
    CHECK(p.tracker_retrigger(0));
    render_blocks(p, again, 12);
    // past the restart's fade and the attack, the second pass is the first one, sample for sample, down to the window's first frame
    const std::uint32_t frames = pos != 0 ? len - len / 4u : len;
    const std::uint32_t from = 8 * kBlk;
    const std::uint32_t upto = std::min<std::uint32_t>(12 * kBlk, frames);
    NEED(upto > from);
    CHECK(std::memcmp(fresh + from, again + from, (upto - from) * sizeof(std::int16_t)) == 0);
    render_blocks(p, flush, 1);  // (a stop at the last sample of a call ends the voice at the next call)
    CHECK_FALSE(p.track_playing(0));
  }
  p.perf_clear();
  p.cut(0);
  p.set_sample(0, SampleDesc{ramp, 16000, 0, 0, 65536});
}

TEST_CASE("perform layer (P8): a looped sample, a granular voice and a wavetable window ignore PLAYBACK; a gated backward voice renders and stops on time") {
  static std::int16_t data[1000];
  static std::int16_t base[10 * kBlk];
  static std::int16_t got[10 * kBlk];
  static std::int16_t flush[8 * kBlk];
  static std::int16_t win[256];
  fill_ramp(data, 1000);
  make_sine40(win, 256);
  VoicePool& p = pool();
  p.perf_clear();
  for (const int kind : {0, 1, 2}) {
    SampleDesc w{kind == 2 ? win : data, kind == 2 ? 256u : 1000u, kind == 0 ? 200u : 0u, kind == 0 ? 1000u : (kind == 2 ? 256u : 0u), 65536};
    if (kind == 1) {
      w.granular = true;
      w.gr_len = 441;
      w.gr_pos = 20000;
    } else if (kind == 2) {
      w.wt_size = 256;
    }
    p.perf_clear();
    p.set_sample(0, w);
    p.cut(0);
    render_blocks(p, flush, 8);
    p.note_on(0, 60, 0, 127);
    render_blocks(p, base, 10);
    p.cut(0);
    render_blocks(p, flush, 8);
    p.fx_event(0, rev_layer(1));
    p.fx_event(0, rev_step(1));
    CHECK(p.note_on(0, 60, 0, 127));
    render_blocks(p, got, 10);
    CHECK_MESSAGE(std::memcmp(base, got, sizeof base) == 0, (kind == 0 ? "looped" : (kind == 1 ? "granular" : "wavetable")));
  }
  // the gate: a backward voice of an inaudible track is rendered (the closed form knows forward voices only), counts no advanced span and ends when the window does
  const std::uint32_t len = 3 * kBlk + 10;
  p.perf_clear();
  p.set_sample(0, SampleDesc{data, len, 0, 0, 65536});
  p.cut(0);
  render_blocks(p, flush, 8);
  p.set_gate(1u);
  p.fx_event(0, rev_layer(1));
  const std::uint32_t before = p.stats().advanced_spans;
  CHECK(p.note_on(0, 60, 0, 127));
  render_blocks(p, got, 3);
  CHECK(p.track_playing(0));
  render_blocks(p, got, 1);
  CHECK_FALSE(p.track_playing(0));
  CHECK(p.stats().advanced_spans == before);
  p.perf_clear();
  p.cut(0);
  render_blocks(p, flush, 8);
  p.note_on(0, 60, 0, 127);  // the forward note of the same gated track is advanced in closed form
  render_blocks(p, got, 2);
  CHECK(p.stats().advanced_spans > before);
  p.set_gate(0);
  p.cut(0);
  p.set_sample(0, SampleDesc{data, 1000, 0, 0, 65536});
}
