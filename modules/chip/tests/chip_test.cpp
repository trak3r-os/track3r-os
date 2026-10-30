// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012c: the Chip module (modules/chip), driven through the ABI from its host shared object, as the OS drives it: the pulse's duty and pitch, the triangle, the LFSR noise (periods of both
// sequences), the envelope's timing, the 4-bit steps, the arpeggio, the sweep and the vibrato, block-size independence, the parameter clamps, and the two views against formulae and the draw
// API's budgets. The module is written from scratch: the references here are formulae and counts, not a third-party engine. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../tests/o3m_test_support.h"

#include "../chip_dsp.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace o3m_test;
using namespace o3os::module;

namespace {

constexpr std::uint32_t kPulseWave = 0, kTriWave = 1, kNoiseLongWave = 2, kNoiseShortWave = 3;

// An instance with a plain flat voice: full volume, no velocity sensitivity, instant attack and release limits, sustain at the top.
struct Fixture {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fixture() {
    ok = lib.open(O3M_CHIP_HOST_SO) && inst.start(*lib.entry);
    if (ok) {
      inst.set(CHIP_PARAM_VOLUME, 0.0f);
      inst.set(CHIP_PARAM_VEL_SENS, 0.0f);
      inst.set(CHIP_PARAM_ATTACK, 0.0f);
      inst.set(CHIP_PARAM_DECAY, 0.0f);
      inst.set(CHIP_PARAM_SUSTAIN, 100.0f);
      inst.set(CHIP_PARAM_RELEASE, 0.0f);
      inst.set(CHIP_PARAM_SWEEP_TIME, 0.0f);
      inst.set(CHIP_PARAM_RAW_EDGES, 1.0f);
      inst.set(CHIP_PARAM_DUTY, 2.0f);
    }
  }
};

}  // namespace

TEST_CASE("chip: the descriptor is valid as the loader reads it and says what module.json says") {
  Lib lib;
  NEED(lib.open(O3M_CHIP_HOST_SO));
  const o3m_module& m = *lib.entry;
  ModuleSet set;
  CHECK(set.add_native("chip", m));
  CHECK(std::string(m.name) == "Chip");
  CHECK(m.kind == O3M_KIND_INSTRUMENT);
  CHECK(m.abi_major == O3M_ABI_MAJOR);
  CHECK(m.abi_minor == 8u);  // it declares views and, as a built-in module, ABI 1.8 (module.json): pitch, pages and labels, and a version in its header
  CHECK(m.param_count == CHIP_PARAM_COUNT);
  CHECK(m.view_count == CHIP_VIEW_COUNT);
  CHECK(m.state_bytes == sizeof(chip::Voice));
  CHECK(m.state_bytes <= CHIP_STATE_BYTES);
  for (std::uint32_t i = 0; i < m.param_count; ++i) {
    const o3m_param& p = m.params[i];
    CAPTURE(std::string(p.name));
    CHECK(std::strlen(p.name) < O3M_PARAM_NAME_MAX);
    CHECK(p.min < p.max);
    CHECK(p.min <= p.def);
    CHECK(p.def <= p.max);
  }
  CHECK((m.params[CHIP_PARAM_WAVE].flags & O3M_PARAM_INT) != 0u);
  CHECK((m.params[CHIP_PARAM_WAVE].flags & O3M_PARAM_NO_LEARN) == 0u);  // OS-SNDMOD S1 (C3): Wave and Duty take a CC
  CHECK((m.params[CHIP_PARAM_DUTY].flags & O3M_PARAM_NO_LEARN) == 0u);
  CHECK((m.params[CHIP_PARAM_RAW_EDGES].flags & O3M_PARAM_BOOL) != 0u);
  CHECK(O3M_PARAM_UNIT(m.params[CHIP_PARAM_ATTACK].flags) == O3M_UNIT_MS);
  CHECK(O3M_PARAM_UNIT(m.params[CHIP_PARAM_FINE].flags) == O3M_UNIT_CENTS);
  CHECK((m.params[CHIP_PARAM_ARP_SPEED].flags & O3M_PARAM_LOG) != 0u);
}

TEST_CASE("chip: the pulse's duty cycles are 12.5, 25, 50 and 75 percent of the period, and the mean is removed") {
  Fixture f;
  NEED(f.ok);
  const double duties[4] = {0.125, 0.25, 0.5, 0.75};
  for (std::uint32_t d = 0; d < 4; ++d) {
    CAPTURE(d);
    f.inst.set(CHIP_PARAM_DUTY, static_cast<float>(d));
    f.inst.on(45);  // 110 Hz: 401 samples a period, so a duty is resolved to a quarter of a percent
    f.inst.render(2000);
    const std::vector<float> s = f.inst.render(40100);  // 100 periods
    // the two levels of the raw pulse are (1 - dc) and (-1 - dc), dc = 2 duty - 1; the threshold is between them (at -dc)
    const float dc = static_cast<float>(2.0 * duties[d] - 1.0);
    std::size_t high = 0;
    double mean = 0.0;
    for (float x : s) {
      high += x > -dc ? 1u : 0u;
      mean += static_cast<double>(x);
      CHECK((std::fabs(x - (1.0f - dc)) < 1e-4f || std::fabs(x - (-1.0f - dc)) < 1e-4f));
    }
    CHECK(static_cast<double>(high) / static_cast<double>(s.size()) == doctest::Approx(duties[d]).epsilon(0.01));
    CHECK(std::fabs(mean / static_cast<double>(s.size())) < 0.01);
    f.inst.off(45);
    f.inst.render(256);
  }
}

TEST_CASE("chip: the pitch follows the note, the transposition and the fine tune") {
  Fixture f;
  NEED(f.ok);
  for (std::uint32_t wave : {kPulseWave, kTriWave}) {
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(wave));
    for (std::uint32_t note : {33u, 45u, 57u, 69u, 81u, 93u}) {
      CAPTURE(wave);
      CAPTURE(note);
      f.inst.on(note);
      f.inst.render(512);
      const std::vector<float> s = f.inst.render(44100);
      CHECK(frequency(s.data(), s.size()) == doctest::Approx(hz_of_note(note)).epsilon(5e-4));
      f.inst.off(note);
      f.inst.render(64);
    }
  }
  f.inst.set(CHIP_PARAM_WAVE, 0.0f);
  f.inst.set(CHIP_PARAM_TRANSPOSE, 7.0f);
  f.inst.set(CHIP_PARAM_FINE, -50.0f);
  f.inst.on(60);
  f.inst.render(512);
  const std::vector<float> s = f.inst.render(44100);
  CHECK(frequency(s.data(), s.size()) == doctest::Approx(hz_of_note(60.0 + 7.0 - 0.5)).epsilon(5e-4));
  // the highest notes stay below Nyquist (the step is held at 0.45 of the rate): no NaN, bounded
  f.inst.set(CHIP_PARAM_TRANSPOSE, 24.0f);
  f.inst.on(127);
  for (float x : f.inst.render(4096)) {
    CHECK(std::isfinite(x));
    CHECK(std::fabs(x) <= 2.0f);
  }
}

TEST_CASE("chip: PolyBLEP keeps the pulse's aliasing well below the raw edges' (the inharmonic energy of a high note)") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_DUTY, 2.0f);
  const std::uint32_t n = 4096;
  // the share of the spectrum (Hann window, a plain DFT) that is not within 2 bins of a harmonic of the note: that is what aliasing is
  auto inharmonic = [&](float raw) {
    f.inst.set(CHIP_PARAM_RAW_EDGES, raw);
    f.inst.on(100);  // 2637 Hz: the 8th harmonic is past 21 kHz, so there is a lot to fold
    f.inst.render(777);
    const std::vector<float> s = f.inst.render(n);
    f.inst.off(100);
    f.inst.render(64);
    const double f0 = frequency(s.data(), s.size());
    std::vector<double> power(n / 2, 0.0);
    for (std::uint32_t k = 1; k < n / 2; ++k) {
      double re = 0.0, im = 0.0;
      for (std::uint32_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
        const double a = 2.0 * kPi * static_cast<double>(k) * i / n;
        re += w * static_cast<double>(s[i]) * std::cos(a);
        im -= w * static_cast<double>(s[i]) * std::sin(a);
      }
      power[k] = re * re + im * im;
    }
    double total = 0.0, bad = 0.0;
    for (std::uint32_t k = 1; k < n / 2; ++k) {
      const double hz = static_cast<double>(k) * kRate / n;
      const double h = hz / f0;
      const double off = std::fabs(h - std::round(h)) * f0 / (kRate / n);  // bins away from the nearest harmonic
      total += power[k];
      bad += off > 2.5 ? power[k] : 0.0;
    }
    return bad / total;
  };
  const double raw = inharmonic(1.0f);
  const double band = inharmonic(0.0f);
  CAPTURE(raw);
  CAPTURE(band);
  CHECK(band < raw * 0.25);  // at least 6 dB
}

TEST_CASE("chip: the triangle is a triangle, and the noise is a +-0.7 sample-and-hold of the LFSR") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(kTriWave));
  f.inst.on(45);
  f.inst.render(512);
  std::vector<float> s = f.inst.render(4410);
  float lo = 1e9f, hi = -1e9f;
  double mean = 0.0;
  for (float x : s) {
    lo = std::min(lo, x);
    hi = std::max(hi, x);
    mean += static_cast<double>(x);
  }
  CHECK(hi == doctest::Approx(1.0f).epsilon(0.02));
  CHECK(lo == doctest::Approx(-1.0f).epsilon(0.02));
  CHECK(std::fabs(mean / static_cast<double>(s.size())) < 0.01);
  // straight slopes: away from the corners, every step is 4 x the phase step (note 45: 110 Hz)
  const float step = static_cast<float>(4.0 * hz_of_note(45) / kRate);
  std::size_t straight = 0;
  for (std::size_t i = 1; i < s.size(); ++i) {
    straight += std::fabs(std::fabs(s[i] - s[i - 1]) - step) < 1e-3f ? 1u : 0u;
  }
  CHECK(straight > s.size() - 2u * 12u * 2u);  // 110 Hz over 0.1 s: 11 periods, two corners each
  // noise: two levels, either sequence; the long one flips about half the LFSR clock a second (16 x 220 Hz at note 57)
  for (std::uint32_t w : {kNoiseLongWave, kNoiseShortWave}) {
    f.inst.set(CHIP_PARAM_WAVE, static_cast<float>(w));
    f.inst.on(57);
    f.inst.render(256);
    s = f.inst.render(44100);
    std::set<float> levels(s.begin(), s.end());
    CHECK(levels.size() == 2u);
    CHECK(std::fabs(*levels.begin() + 0.7f) < 1e-5f);
    CHECK(std::fabs(*levels.rbegin() - 0.7f) < 1e-5f);
    std::size_t flips = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
      flips += s[i] != s[i - 1] ? 1u : 0u;
    }
    CAPTURE(w);
    CHECK(static_cast<double>(flips) == doctest::Approx(0.5 * 16.0 * 220.0).epsilon(0.1));
  }
}

TEST_CASE("chip: the LFSR's periods are 32767 (long) and 93 (short, from the seed 1)") {
  for (bool short_mode : {false, true}) {
    std::uint32_t r = 1, period = 0;
    do {
      r = chip::lfsr_step(r, short_mode);
      ++period;
      CHECK(r != 0u);
      CHECK(r < 0x8000u);
    } while (r != 1u && period < 40000u);
    CAPTURE(short_mode);
    CHECK(period == (short_mode ? 93u : 32767u));
  }
}

TEST_CASE("chip: the envelope's attack, decay, sustain and release are linear and take the time the parameters say") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_DUTY, 2.0f);  // a 50 % raw pulse is +-1: |sample| is the envelope itself
  f.inst.set(CHIP_PARAM_ATTACK, 100.0f);
  f.inst.set(CHIP_PARAM_DECAY, 200.0f);
  f.inst.set(CHIP_PARAM_SUSTAIN, 50.0f);
  f.inst.set(CHIP_PARAM_RELEASE, 300.0f);
  f.inst.on(60);
  const std::vector<float> a = f.inst.render(44100);  // one second
  const auto at_ms = [&](double ms) { return std::fabs(a[static_cast<std::size_t>(ms * 44.1)]); };
  CHECK(at_ms(0.5) == doctest::Approx(0.005).epsilon(0.2));
  CHECK(at_ms(25) == doctest::Approx(0.25).epsilon(0.01));
  CHECK(at_ms(50) == doctest::Approx(0.5).epsilon(0.01));
  CHECK(at_ms(99) == doctest::Approx(0.99).epsilon(0.01));
  CHECK(at_ms(150) == doctest::Approx(0.875).epsilon(0.01));  // a quarter of the way down from 1 to 0.5 (200 ms)
  CHECK(at_ms(200) == doctest::Approx(0.75).epsilon(0.01));   // half way
  CHECK(at_ms(250) == doctest::Approx(0.625).epsilon(0.01));
  CHECK(at_ms(299) == doctest::Approx(0.5).epsilon(0.01));
  CHECK(at_ms(700) == doctest::Approx(0.5).epsilon(1e-4));
  CHECK(f.inst.busy == 1u);  // held: still sounding
  f.inst.off(60);
  const std::vector<float> r = f.inst.render(44100);  // the release is from 0.5 in 300 ms
  const auto r_ms = [&](double ms) { return std::fabs(r[static_cast<std::size_t>(ms * 44.1)]); };
  CHECK(r_ms(0.1) == doctest::Approx(0.5).epsilon(0.01));
  CHECK(r_ms(150) == doctest::Approx(0.25).epsilon(0.01));
  CHECK(r_ms(290) == doctest::Approx(0.0167).epsilon(0.1));
  CHECK(r_ms(320) == 0.0f);
  CHECK(f.inst.busy == 0u);  // the module says it is idle: the OS may skip it
  // a release time of 0 is 1 ms, never a step; an attack of 0 likewise
  f.inst.set(CHIP_PARAM_ATTACK, 0.0f);
  f.inst.set(CHIP_PARAM_RELEASE, 0.0f);
  f.inst.on(60);
  const std::vector<float> q = f.inst.render(200);
  CHECK(std::fabs(q[0]) < 0.05f);
  CHECK(std::fabs(q[43]) == doctest::Approx(1.0f).epsilon(0.05));
  CHECK(std::fabs(q[100]) == doctest::Approx(1.0f).epsilon(0.01));  // (the decay of 200 ms to 50 % has hardly begun)
  f.inst.off(60);
  const std::vector<float> e = f.inst.render(200);
  CHECK(e[60] == 0.0f);
}

TEST_CASE("chip: a note over a sounding one continues from the level it is at, a sustain of 0 ends the note after the decay, and an idle voice is silent") {
  Fixture f;
  NEED(f.ok);
  CHECK(f.inst.render(64)[10] == 0.0f);
  CHECK(f.inst.busy == 0u);
  f.inst.set(CHIP_PARAM_ATTACK, 100.0f);
  f.inst.set(CHIP_PARAM_RELEASE, 100.0f);
  f.inst.on(60);
  f.inst.render(2205);  // 50 ms: the envelope is at 0.5
  f.inst.off(60);
  f.inst.render(441);   // 10 ms into the release: 0.4
  f.inst.on(64);
  const std::vector<float> s = f.inst.render(64);
  CHECK(std::fabs(s[0]) < 0.46f);  // no jump to full, no jump to zero: 0.45 and the first step of the new attack
  CHECK(std::fabs(s[0]) > 0.44f);
  // sustain 0: a percussive note ends by itself while the key is held
  f.inst.set(CHIP_PARAM_ATTACK, 0.0f);
  f.inst.set(CHIP_PARAM_DECAY, 100.0f);
  f.inst.set(CHIP_PARAM_SUSTAIN, 0.0f);
  f.inst.on(60);
  f.inst.render(44);
  f.inst.render(4410 - 44);
  CHECK(f.inst.busy == 1u);  // 100 ms: only just
  const std::vector<float> t = f.inst.render(512);
  CHECK(f.inst.busy == 0u);
  CHECK(t[500] == 0.0f);
}

TEST_CASE("chip: a note off before the first sample of the note leaves the voice idle at once, not busy until the next note (OS-SNDMOD B1)") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_ATTACK, 10.0f);
  f.inst.on(60);
  f.inst.off(60);  // same dispatch pass: no sample has been rendered, the envelope is still 0
  const std::vector<float> a = f.inst.render(64);
  CHECK(f.inst.busy == 0u);
  for (const float x : a) {
    CHECK(x == 0.0f);
  }
  CHECK(f.inst.render(64)[5] == 0.0f);
  CHECK(f.inst.busy == 0u);
  // the voice is not stuck: the next note sounds, and a note off after a rendered sample still releases
  f.inst.set(CHIP_PARAM_RELEASE, 5.0f);
  f.inst.on(60);
  const std::vector<float> b = f.inst.render(441);
  CHECK(f.inst.busy == 1u);
  CHECK(std::fabs(b[440]) > 0.5f);
  f.inst.off(60);
  f.inst.render(441);
  CHECK(f.inst.busy == 0u);
}

TEST_CASE("chip: Vol Steps quantises the level to sixteen steps, and the steps are 1/15 apart") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_DECAY, 400.0f);
  f.inst.set(CHIP_PARAM_SUSTAIN, 0.0f);
  f.inst.set(CHIP_PARAM_VOL_STEPS, 1.0f);
  f.inst.on(60);
  std::set<int> steps;
  for (float x : f.inst.render(22050)) {
    const float v = std::fabs(x) * 15.0f;
    CHECK(std::fabs(v - std::round(v)) < 1e-3f);
    steps.insert(static_cast<int>(std::round(v)));
  }
  CHECK(*steps.rbegin() == 15);
  CHECK(steps.size() >= 15u);  // the decay walks down the whole staircase
  f.inst.set(CHIP_PARAM_VOL_STEPS, 0.0f);
  f.inst.on(60);
  std::set<float> smooth;
  for (float x : f.inst.render(22050)) {
    smooth.insert(std::fabs(x));
  }
  CHECK(smooth.size() > 1000u);
}

TEST_CASE("chip: the arpeggio steps through 0, Arp 1, Arp 2 at Arp Speed, and is off when both are 0") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_ARP_SPEED, 25.0f);  // 40 ms a step: 1764 samples
  f.inst.set(CHIP_PARAM_ARP_1, 12.0f);
  f.inst.set(CHIP_PARAM_ARP_2, 7.0f);
  f.inst.on(45);
  const std::vector<float> s = f.inst.render(44100);
  const double want[3] = {hz_of_note(45), hz_of_note(57), hz_of_note(52)};
  for (int step = 0; step < 9; ++step) {  // nine steps: three rounds of the three notes
    const std::size_t from = static_cast<std::size_t>(step) * 1764 + 300, len = 1764 - 600;
    CAPTURE(step);
    CHECK(frequency(s.data() + from, len) == doctest::Approx(want[step % 3]).epsilon(0.01));
  }
  // Arp 2 = 0 leaves two steps (0 and Arp 1), both 0 leaves one: the note
  f.inst.set(CHIP_PARAM_ARP_2, 0.0f);
  f.inst.on(45);
  const std::vector<float> t = f.inst.render(44100);
  for (int step = 0; step < 6; ++step) {
    const std::size_t from = static_cast<std::size_t>(step) * 1764 + 300, len = 1764 - 600;
    CAPTURE(step);
    CHECK(frequency(t.data() + from, len) == doctest::Approx(want[step % 2]).epsilon(0.01));
  }
  f.inst.set(CHIP_PARAM_ARP_1, 0.0f);
  f.inst.on(45);
  const std::vector<float> u = f.inst.render(44100);
  CHECK(frequency(u.data(), u.size()) == doctest::Approx(want[0]).epsilon(5e-4));
  // a negative step goes down
  f.inst.set(CHIP_PARAM_ARP_1, -12.0f);
  f.inst.on(57);
  const std::vector<float> d = f.inst.render(4410);
  CHECK(frequency(d.data() + 2200, 1764 - 600) == doctest::Approx(hz_of_note(45)).epsilon(0.01));
}

TEST_CASE("chip: the sweep glides from its offset to the note over Sweep Time, linearly in semitones, and runs again at every note") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_SWEEP, 12.0f);
  f.inst.set(CHIP_PARAM_SWEEP_TIME, 400.0f);
  for (int round = 0; round < 2; ++round) {
    f.inst.on(48);
    const std::vector<float> s = f.inst.render(44100);
    // at 100 ms the offset is 12 * (1 - 100/400) = 9 semitones, at 200 ms 6, at 300 ms 3, from 400 ms on 0
    for (int i = 1; i <= 3; ++i) {
      const std::size_t centre = static_cast<std::size_t>(i * 4410);
      CAPTURE(i);
      CHECK(frequency(s.data() + centre - 441, 882) == doctest::Approx(hz_of_note(48.0 + 12.0 * (1.0 - 0.25 * i))).epsilon(0.02));
    }
    CHECK(frequency(s.data() + 22050, 20000) == doctest::Approx(hz_of_note(48)).epsilon(5e-4));
    f.inst.off(48);
    f.inst.render(512);
  }
  f.inst.set(CHIP_PARAM_SWEEP, -12.0f);  // from an octave below: up
  f.inst.on(60);
  const std::vector<float> d = f.inst.render(4410);
  CHECK(frequency(d.data(), 882) < hz_of_note(60) * 0.6);
  CHECK(frequency(d.data(), 882) > hz_of_note(48) * 0.95);
  // Sweep Time 0 turns the sweep off
  f.inst.set(CHIP_PARAM_SWEEP_TIME, 0.0f);
  f.inst.on(60);
  const std::vector<float> n = f.inst.render(4410);
  CHECK(frequency(n.data(), n.size()) == doctest::Approx(hz_of_note(60)).epsilon(2e-3));
}

TEST_CASE("chip: the vibrato swings the pitch by Vib Depth at Vib Rate, after Vib Delay") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_VIB_DEPTH, 100.0f);  // +-1 semitone
  f.inst.set(CHIP_PARAM_VIB_RATE, 5.0f);
  f.inst.on(60);
  const std::vector<float> s = f.inst.render(44100);
  double fmin = 1e9, fmax = 0.0;
  for (std::size_t at = 0; at + 441 <= s.size(); at += 220) {  // 10 ms windows
    const double hz = frequency(s.data() + at, 441);
    if (hz > 0.0) {
      fmin = std::min(fmin, hz);
      fmax = std::max(fmax, hz);
    }
  }
  CHECK(fmax == doctest::Approx(hz_of_note(61)).epsilon(0.02));
  CHECK(fmin == doctest::Approx(hz_of_note(59)).epsilon(0.02));
  // the swing repeats five times a second: the pitch two periods apart (400 ms) is the same
  CHECK(frequency(s.data() + 1000, 441) == doctest::Approx(frequency(s.data() + 1000 + 17640, 441)).epsilon(0.01));
  // with a delay the first 100 ms are still, then it grows to full by the delay
  f.inst.set(CHIP_PARAM_VIB_DELAY, 800.0f);
  f.inst.on(60);
  const std::vector<float> d = f.inst.render(44100);
  double early = 0.0, late = 0.0;
  for (std::size_t at = 0; at + 441 <= 8820; at += 220) {
    early = std::max(early, std::fabs(std::log2(frequency(d.data() + at, 441) / hz_of_note(60))) * 12.0);
  }
  for (std::size_t at = 35280; at + 441 <= d.size(); at += 220) {
    late = std::max(late, std::fabs(std::log2(frequency(d.data() + at, 441) / hz_of_note(60))) * 12.0);
  }
  CHECK(early < 0.3);  // 200 ms in: a quarter of the depth at most
  CHECK(late > 0.9);
}

TEST_CASE("chip: velocity scales the level by Vel Sens, and Volume is in dB") {
  Fixture f;
  NEED(f.ok);
  const auto peak = [&](std::uint32_t velocity) {
    f.inst.on(60, velocity);
    float p = 0.0f;
    for (float x : f.inst.render(2000)) {
      p = std::max(p, std::fabs(x));
    }
    f.inst.off(60);
    f.inst.render(128);
    return p;
  };
  f.inst.set(CHIP_PARAM_VEL_SENS, 0.0f);
  CHECK(peak(20) == doctest::Approx(1.0f).epsilon(1e-3));
  f.inst.set(CHIP_PARAM_VEL_SENS, 100.0f);
  CHECK(peak(127) == doctest::Approx(1.0f).epsilon(1e-3));
  CHECK(peak(64) == doctest::Approx(64.0f / 127.0f).epsilon(1e-3));
  f.inst.set(CHIP_PARAM_VEL_SENS, 50.0f);
  CHECK(peak(1) == doctest::Approx(1.0f - 0.5f * (1.0f - 1.0f / 127.0f)).epsilon(1e-3));
  f.inst.set(CHIP_PARAM_VEL_SENS, 0.0f);
  f.inst.set(CHIP_PARAM_VOLUME, -6.0f);
  CHECK(peak(100) == doctest::Approx(std::pow(10.0f, -6.0f / 20.0f)).epsilon(1e-3));
}

TEST_CASE("chip: the voice is the same whatever the block sizes it is rendered in") {
  Lib lib;
  NEED(lib.open(O3M_CHIP_HOST_SO));
  std::vector<float> ref;
  for (std::uint32_t block : {64u, 1u, 7u, 128u, 33u}) {
    Instance in;
    NEED(in.start(*lib.entry));
    in.set(CHIP_PARAM_ARP_1, 5.0f);
    in.set(CHIP_PARAM_VIB_DEPTH, 40.0f);
    in.set(CHIP_PARAM_SWEEP, 5.0f);
    in.set(CHIP_PARAM_RAW_EDGES, 0.0f);
    in.on(55);
    std::vector<float> out = in.render(6000, block);
    in.off(55);
    const std::vector<float> tail = in.render(2000, block);
    out.insert(out.end(), tail.begin(), tail.end());
    if (ref.empty()) {
      ref = out;
    }
    CAPTURE(block);
    CHECK(out == ref);  // bit for bit: the control tick counts samples, not calls
  }
}

TEST_CASE("chip: no parameter value, however hostile, makes the voice produce NaN or runaway samples") {
  Lib lib;
  NEED(lib.open(O3M_CHIP_HOST_SO));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float hostile[] = {nan, 1e30f, -1e30f, 0.0f, -0.0f, 1e-30f, 7.5f, -7.5f};
  for (float h : hostile) {
    Instance in;
    NEED(in.start(*lib.entry));
    CAPTURE(h);
    for (std::uint32_t id = 0; id < in.m->param_count; ++id) {
      in.set(id, h);
    }
    in.set(CHIP_PARAM_VOLUME, 6.0f);
    in.on(200, 1000);  // note and velocity out of range
    for (std::uint32_t block = 0; block < 40; ++block) {
      for (float x : in.render(64)) {
        CHECK(std::isfinite(x));
        CHECK(std::fabs(x) < 6.0f);
      }
    }
    in.set(1000, 5.0f);  // an id out of range is ignored
    in.off(0);
    for (float x : in.render(5000)) {
      CHECK(std::isfinite(x));
    }
  }
  // a second note at once, and a note-off with nothing sounding, are harmless
  Instance in;
  NEED(in.start(*lib.entry));
  in.off(60);
  in.on(60);
  in.on(60);
  in.on(72);
  for (float x : in.render(3000)) {
    CHECK(std::isfinite(x));
  }
}

TEST_CASE("chip: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_CHIP_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_INSTRUMENT);
  CHECK(t.h.abi_minor == 8u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));  // o3m_add_module(... VERSION 1.0.0)
  CHECK(t.h.param_count == CHIP_PARAM_COUNT);
  CHECK(t.h.view_count == CHIP_VIEW_COUNT);
  CHECK(t.h.state_bytes <= CHIP_STATE_BYTES);
  CHECK(t.h.hot_size <= 3072u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: the oscillator loops are the whole of it
  CHECK(t.h.image_size - t.h.hot_size <= 6144u);
  MESSAGE("chip.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}

// ---- views --------------------------------------------------------------------------------------------------------------------------------

namespace {

struct Views {
  Lib lib;
  ViewHost host;
  bool ok = false;
  float p[CHIP_PARAM_COUNT];
  Views() {
    ok = lib.open(O3M_CHIP_HOST_SO);
    if (ok) {
      for (std::uint32_t i = 0; i < CHIP_PARAM_COUNT; ++i) {
        p[i] = chip_params[i].def;
      }
    }
  }
  ViewStatus draw(std::uint32_t view, int w = 380, int h = 100, std::uint32_t max_words = kViewMaxWords) {
    return host.draw(*lib.entry, nullptr, 1, view, w, h, p, CHIP_PARAM_COUNT, max_words);
  }
};

}  // namespace

TEST_CASE("chip views: both draw inside the draw API's budgets for every wave, duty and corner of the envelope, and the two together fit one page") {
  Views v;
  NEED(v.ok);
  CHECK(v.lib.entry->view_count == 2u);
  NEED(v.lib.entry->ui != nullptr);
  const float times[] = {0.0f, 1.0f, 500.0f};
  for (float wave = 0.0f; wave <= 3.0f; wave += 1.0f) {
    for (float duty = 0.0f; duty <= 3.0f; duty += 1.0f) {
      for (float steps : {0.0f, 1.0f}) {
        for (float a : times) {
          for (float d : {0.0f, 2000.0f}) {
            for (float sus : {0.0f, 100.0f}) {
              for (float r : {0.0f, 2000.0f}) {
                v.p[CHIP_PARAM_WAVE] = wave;
                v.p[CHIP_PARAM_DUTY] = duty;
                v.p[CHIP_PARAM_VOL_STEPS] = steps;
                v.p[CHIP_PARAM_ATTACK] = a;
                v.p[CHIP_PARAM_DECAY] = d;
                v.p[CHIP_PARAM_SUSTAIN] = sus;
                v.p[CHIP_PARAM_RELEASE] = r;
                CAPTURE(wave);
                CAPTURE(duty);
                CAPTURE(a);
                CAPTURE(d);
                CAPTURE(sus);
                CAPTURE(r);
                NEED(v.draw(CHIP_VIEW_WAVE) == ViewStatus::Ok);
                const std::uint32_t w0 = v.host.words();
                NEED(v.draw(CHIP_VIEW_ENVELOPE) == ViewStatus::Ok);
                CHECK(w0 + v.host.words() <= 500u);  // the page's own words come out of the 700 too
                Drawing dr;
                v.host.replay(dr);
                CHECK(dr.curves.size() == 1u);
                for (float y : dr.curve_y.at(0)) {
                  CHECK(std::isfinite(y));
                  CHECK(y >= -1e-4f);
                  CHECK(y <= 1.0001f);
                }
              }
            }
          }
        }
      }
    }
  }
  // out-of-range values (a stored blob can hold anything finite) are held to the module's ranges; NaN too; too few values: it draws nothing; the smallest canvas
  v.p[CHIP_PARAM_ATTACK] = -5.0f;
  v.p[CHIP_PARAM_SUSTAIN] = 1e9f;
  v.p[CHIP_PARAM_RELEASE] = std::numeric_limits<float>::quiet_NaN();
  v.p[CHIP_PARAM_WAVE] = 99.0f;
  CHECK(v.draw(CHIP_VIEW_WAVE, 16, 16) == ViewStatus::Ok);
  CHECK(v.draw(CHIP_VIEW_ENVELOPE, 16, 16) == ViewStatus::Ok);
  CHECK(v.draw(CHIP_VIEW_ENVELOPE, 800, 480) == ViewStatus::Ok);
  CHECK(v.host.draw(*v.lib.entry, nullptr, 1, CHIP_VIEW_WAVE, 380, 100, v.p, 3) == ViewStatus::Ok);
  Drawing none;
  v.host.replay(none);
  CHECK((none.polys.empty() && none.curves.empty() && none.texts.empty()));
}

TEST_CASE("chip views: the wave view draws the duty, the triangle and the noise as they are, and names them") {
  Views v;
  NEED(v.ok);
  const int w = 380, h = 100;
  const int left = 6, right = w - 6, top = 26, bottom = h - 6;
  const char* names[4] = {"PULSE 12.5%", "PULSE 25%", "PULSE 50%", "PULSE 75%"};
  const double duty[4] = {0.125, 0.25, 0.5, 0.75};
  v.p[CHIP_PARAM_WAVE] = 0.0f;
  for (std::uint32_t d = 0; d < 4; ++d) {
    v.p[CHIP_PARAM_DUTY] = static_cast<float>(d);
    CAPTURE(d);
    NEED(v.draw(CHIP_VIEW_WAVE, w, h) == ViewStatus::Ok);
    Drawing dr;
    v.host.replay(dr);
    NEED(dr.texts.size() == 1u);
    CHECK(dr.texts[0].s == names[d]);
    NEED(dr.polys.size() == 1u);
    const std::vector<std::int16_t>& xy = dr.polys[0].xy;
    NEED(xy.size() == 18u);  // nine points: two periods
    // the first high stretch is duty x one period wide (a period is half the width), at the top; then the low one to the next rise
    const double period = (right - left) / 2;
    CHECK(xy[3] == top);
    CHECK(xy[5] == top);
    CHECK(xy[7] == bottom);
    CHECK(static_cast<double>(xy[4] - xy[2]) == doctest::Approx(duty[d] * period).epsilon(0.02));
    CHECK(xy[2] == left);
    CHECK(xy[10] == left + period);
    CHECK(xy[16] == right);
    CHECK(dr.polys[0].color == O3M_COLOR_INSTR);
    // the zero line is one rectangle across the middle
    NEED(dr.rects.size() == 1u);
    CHECK(dr.rects[0].y == (top + bottom) / 2);
    CHECK(dr.rects[0].h == 1);
  }
  v.p[CHIP_PARAM_WAVE] = 1.0f;
  NEED(v.draw(CHIP_VIEW_WAVE, w, h) == ViewStatus::Ok);
  Drawing tri;
  v.host.replay(tri);
  NEED(tri.polys.size() == 1u);
  CHECK(tri.texts.at(0).s == "TRIANGLE");
  CHECK(tri.polys[0].xy.size() == 10u);  // five corners
  // the noise draws the LFSR's own first steps (chip::lfsr_step from the seed 1), long and short differ
  std::vector<std::int16_t> shapes[2];
  for (std::uint32_t k = 0; k < 2; ++k) {
    v.p[CHIP_PARAM_WAVE] = static_cast<float>(2 + k);
    NEED(v.draw(CHIP_VIEW_WAVE, w, h) == ViewStatus::Ok);
    Drawing n;
    v.host.replay(n);
    NEED(n.polys.size() == 1u);
    CHECK(n.texts.at(0).s == (k == 0 ? "NOISE LONG" : "NOISE SHORT"));
    shapes[k] = n.polys[0].xy;
    NEED(shapes[k].size() == 2u * 96u);
    std::uint32_t r = 1;
    for (std::uint32_t pre = 0; pre < chip::kNoisePreroll; ++pre) {
      r = chip::lfsr_step(r, k == 1);  // OS-SNDMOD S1 (H4): the view runs the register on before it draws
    }
    for (std::uint32_t step = 0; step < 48; ++step) {
      r = chip::lfsr_step(r, k == 1);
      CHECK(shapes[k][4 * step + 1] == ((r & 1u) != 0u ? bottom : top));
    }
  }
  CHECK(shapes[0] != shapes[1]);
  // the seed's flat run is gone: the long picture changes level within its first 14 cells (from the seed 1 it did not)
  for (std::uint32_t k = 0; k < 2; ++k) {
    bool flat = true;
    for (std::uint32_t step = 1; step < 14; ++step) {
      flat = flat && shapes[k][4 * step + 1] == shapes[k][1];
    }
    CAPTURE(k);
    CHECK_FALSE(flat);
  }
}

TEST_CASE("chip views: OS-BUG-CHIPWAVELABEL the wave's name sits above the trace, in the real Data face (10 x 20), for every wave and duty, at the view's smallest and preferred sizes") {
  Views v;
  NEED(v.ok);
  ViewHost::Metrics m;  // the theme's faces: Display 16 x 32, Data 10 x 20, Label 8 x 16, Micro 6 x 12
  const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
  for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
    m.cell_w[f] = cw[f];
    m.cell_h[f] = ch[f];
  }
  v.host.set_metrics(m);
  const int sizes[3][2] = {{160, 64}, {380, 90}, {380, 100}};  // module.json: the Wave view's min_size, preferred_size, and the tests' usual canvas
  for (const auto& sz : sizes) {
    for (std::uint32_t wave = 0; wave < 4; ++wave) {
      for (std::uint32_t duty = 0; duty < 4; ++duty) {
        v.p[CHIP_PARAM_WAVE] = static_cast<float>(wave);
        v.p[CHIP_PARAM_DUTY] = static_cast<float>(duty);
        CAPTURE(sz[0]);
        CAPTURE(sz[1]);
        CAPTURE(wave);
        CAPTURE(duty);
        NEED(v.draw(CHIP_VIEW_WAVE, sz[0], sz[1]) == ViewStatus::Ok);
        Drawing dr;
        v.host.replay(dr);
        NEED(dr.texts.size() == 1u);
        NEED(dr.polys.size() == 1u);
        NEED(dr.rects.size() == 1u);
        const int text_bottom = dr.texts[0].y + ch[dr.texts[0].face];
        int trace_top = 1 << 20, trace_bottom = -(1 << 20);
        for (std::size_t k = 1; k < dr.polys[0].xy.size(); k += 2) {
          trace_top = std::min<int>(trace_top, dr.polys[0].xy[k]);
          trace_bottom = std::max<int>(trace_bottom, dr.polys[0].xy[k]);
        }
        CHECK(text_bottom < trace_top);  // no row of the name lies on the trace's rows
        CHECK(text_bottom < dr.rects[0].y);
        CHECK(trace_bottom < sz[1]);  // and the trace, moved down, still fits the view
        CHECK(trace_top < trace_bottom);
      }
    }
  }
}

TEST_CASE("chip views: the envelope curve is the voice's envelope in time, in 4-bit steps when they are on") {
  Views v;
  NEED(v.ok);
  v.p[CHIP_PARAM_ATTACK] = 100.0f;
  v.p[CHIP_PARAM_DECAY] = 200.0f;
  v.p[CHIP_PARAM_SUSTAIN] = 50.0f;
  v.p[CHIP_PARAM_RELEASE] = 300.0f;
  NEED(v.draw(CHIP_VIEW_ENVELOPE) == ViewStatus::Ok);
  Drawing dr;
  v.host.replay(dr);
  NEED(dr.curves.size() == 1u);
  const CurveCall& c = dr.curves[0];
  const std::vector<float>& y = dr.curve_y[0];
  NEED(y.size() == 64u);
  // the sustain is held for a quarter of the rest (150 ms of 600): the whole is 100 + 200 + 150 + 300 = 750 ms
  const double total = 750.0;
  CHECK(c.x_hi == doctest::Approx(0.75f).epsilon(1e-4));
  CHECK((c.x_lo == 0.0f && c.y_lo == 0.0f && c.y_hi == 1.0f));
  CHECK((c.flags & O3M_CURVE_AXES) != 0u);
  CHECK(y.front() == 0.0f);
  CHECK(y.back() == doctest::Approx(0.0f).epsilon(1e-4));
  for (std::size_t i = 0; i < y.size(); ++i) {
    const double t = total * static_cast<double>(i) / 63.0;
    double want;
    if (t < 100.0) {
      want = t / 100.0;
    } else if (t < 300.0) {
      want = 1.0 - 0.5 * (t - 100.0) / 200.0;
    } else if (t < 450.0) {
      want = 0.5;
    } else {
      want = 0.5 * (1.0 - (t - 450.0) / 300.0);
    }
    CAPTURE(i);
    CHECK(static_cast<double>(y[i]) == doctest::Approx(want).epsilon(1e-4).scale(1.0));
  }
  v.p[CHIP_PARAM_VOL_STEPS] = 1.0f;
  NEED(v.draw(CHIP_VIEW_ENVELOPE) == ViewStatus::Ok);
  Drawing st;
  v.host.replay(st);
  NEED(st.curve_y.size() == 1u);
  for (float s : st.curve_y[0]) {
    CHECK(std::fabs(s * 15.0f - std::round(s * 15.0f)) < 1e-4f);
  }
}

// ---- OS-MODPITCH (ABI 1.8): the host's Slide / Glide offset ---------------------------------------------------------------------------------------------------------------

TEST_CASE("chip: the descriptor exports pitch, four named pages and labels for Wave and Duty, all within the ABI's limits (ABI 1.8)") {
  Lib lib;
  NEED(lib.open(O3M_CHIP_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.pitch != nullptr);
  NEED(m.pages != nullptr);
  CHECK(m.page_count == 4u);
  unsigned next = 0;
  for (std::uint32_t k = 0; k < m.page_count; ++k) {
    CHECK(m.pages[k].first_param == next);
    const unsigned end = k + 1u < m.page_count ? m.pages[k + 1].first_param : m.param_count;
    CHECK((end > next && end - next <= O3M_PAGE_PARAMS_MAX));
    CHECK((std::strlen(m.pages[k].name) >= 1u && std::strlen(m.pages[k].name) < O3M_PAGE_NAME_MAX));
    next = end;
  }
  CHECK(next == m.param_count);
  NEED(m.labels != nullptr);
  for (std::uint32_t i = 0; i < m.param_count; ++i) {
    if (m.labels[i].count == 0u) {
      CHECK(m.labels[i].names == nullptr);
      continue;
    }
    CAPTURE(std::string(m.params[i].name));
    CHECK((m.params[i].flags & O3M_PARAM_INT) != 0u);
    CHECK(m.labels[i].count == static_cast<std::uint32_t>(m.params[i].max - m.params[i].min) + 1u);
    for (std::uint32_t v = 0; v < m.labels[i].count; ++v) {
      CHECK((std::strlen(m.labels[i].names[v]) >= 1u && std::strlen(m.labels[i].names[v]) < O3M_LABEL_NAME_MAX));
    }
  }
  CHECK(m.labels[CHIP_PARAM_WAVE].count == 4u);
  CHECK(std::string(m.labels[CHIP_PARAM_WAVE].names[1]) == "TRIANGLE");
  CHECK(m.labels[CHIP_PARAM_DUTY].count == 4u);
  CHECK(m.labels[CHIP_PARAM_VOLUME].count == 0u);
}

TEST_CASE("chip: pitch(0, 0) is bit for bit not calling it, pitch(12, 0) is an octave up, and a ramp's slope is followed (ABI 1.8)") {
  Fixture a, b;
  NEED(a.ok);
  NEED(b.ok);
  a.inst.set(CHIP_PARAM_VIB_DEPTH, 40.0f);  // something that moves, so that the comparison is of a busy voice
  b.inst.set(CHIP_PARAM_VIB_DEPTH, 40.0f);
  a.inst.on(57);
  b.inst.on(57);
  b.lib.entry->pitch(b.inst.state, 0.0f, 0.0f);
  const std::vector<float> sa = a.inst.render(8192);
  const std::vector<float> sb = b.inst.render(8192);
  CHECK(sa == sb);
  b.lib.entry->pitch(b.inst.state, 12.0f, 0.0f);
  b.inst.render(512);
  const std::vector<float> up = b.inst.render(44100);
  b.inst.set(CHIP_PARAM_VIB_DEPTH, 0.0f);
  b.inst.on(57);  // (a note puts the offset back to 0)
  b.inst.render(512);
  const std::vector<float> flat = b.inst.render(44100);
  CHECK(frequency(up.data(), up.size()) == doctest::Approx(2.0 * frequency(flat.data(), flat.size())).epsilon(2e-3));
  // a slope: +12 semitones over 44100 frames, from the note: the pitch rises, and is an octave up at the end of it
  Fixture c;
  NEED(c.ok);
  c.inst.on(57);
  c.inst.render(256);
  c.lib.entry->pitch(c.inst.state, 0.0f, 12.0f / 44100.0f);
  const std::vector<float> ramp = c.inst.render(44100);
  const double early = frequency(ramp.data(), 4096);
  const double late = frequency(ramp.data() + 40000, 4000);
  CHECK(early < late);
  CHECK(late == doctest::Approx(hz_of_note(57.0 + 12.0 * 42000.0 / 44100.0)).epsilon(0.02));
  for (float x : ramp) {
    CHECK(std::isfinite(x));
  }
}

TEST_CASE("chip: a note with LEGATO | RAMP over a sounding one keeps the envelope and moves only the pitch; a plain note restarts it (ABI 1.8)") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(CHIP_PARAM_ATTACK, 0.0f);
  f.inst.set(CHIP_PARAM_DECAY, 400.0f);
  f.inst.set(CHIP_PARAM_SUSTAIN, 0.0f);
  const auto peak = [](const std::vector<float>& s, std::size_t from, std::size_t to) {
    float p = 0.0f;
    for (std::size_t i = from; i < to && i < s.size(); ++i) {
      p = std::fabs(s[i]) > p ? std::fabs(s[i]) : p;
    }
    return p;
  };
  f.inst.on(57);
  f.inst.render(8820);  // 200 ms into a 400 ms decay: about half
  f.inst.on(64, 127, O3M_NOTE_LEGATO | O3M_NOTE_RAMP);
  const std::vector<float> kept = f.inst.render(512);
  const float level_kept = peak(kept, 64, 512);
  f.inst.on(57);
  f.inst.render(8820);
  f.inst.on(64, 127, 0);  // a plain note over the sounding one: the envelope starts again from the level it is at
  const std::vector<float> again = f.inst.render(512);
  CHECK(level_kept < peak(again, 256, 512));  // a plain note over a sounding one rises towards the attack again; the kept one goes on decaying
  // the pitch moved to the new note at once
  Fixture g;
  NEED(g.ok);
  g.inst.on(57);
  g.inst.render(1024);
  g.inst.on(69, 127, O3M_NOTE_LEGATO | O3M_NOTE_RAMP);
  g.inst.render(512);
  const std::vector<float> s = g.inst.render(22050);
  CHECK(frequency(s.data(), s.size()) == doctest::Approx(hz_of_note(69)).epsilon(5e-4));
  // with nothing sounding, the same flags are an ordinary start
  Fixture h;
  NEED(h.ok);
  h.inst.on(60, 127, O3M_NOTE_LEGATO | O3M_NOTE_RAMP);
  h.inst.render(512);
  const std::vector<float> t = h.inst.render(22050);
  CHECK(frequency(t.data(), t.size()) == doctest::Approx(hz_of_note(60)).epsilon(5e-4));
  CHECK(peak(t, 0, t.size()) > 0.1f);
}
