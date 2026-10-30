// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SID slice 1: the SID module (modules/sid), driven through the ABI from its host shared object, as the OS drives it: the pitch of every wave, the pulse width (a duty sweep with no clicks), the
// 23-bit noise (level, rate, the register's period), the combined waves, the modulator oscillator (hard sync against a naive reset, ring), the stepped ADSR against the Commodore 64
// Programmer's Reference Guide's table, release to idle, the parameter blob, the pitch calls, block-size independence, the parameter clamps, and the two views. The module is written from
// scratch: the references here are formulae, the C64 table and counts, not a third-party engine. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../tests/o3m_test_support.h"

#include "../sid_dsp.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace o3m_test;
using namespace o3os::module;

namespace {

constexpr std::uint32_t kSaw = 0, kTri = 1, kPulse = 2, kNoise = 3, kSawTri = 4, kPulseSaw = 5, kPulseTri = 6, kPulseSawTri = 7;

// The Commodore 64 Programmer's Reference Guide's ADSR table, in milliseconds: its own copy, so that a slip in the module's table is caught.
constexpr double kAttackTable[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};
constexpr double kDecayTable[16] = {6, 24, 48, 72, 114, 168, 204, 240, 300, 750, 1500, 2400, 3000, 9000, 15000, 24000};

// An instance with a plain flat voice: full volume, no velocity sensitivity, a 50 % pulse, the fastest attack and release, the sustain at the top.
struct Fixture {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fixture() {
    ok = lib.open(O3M_SID_HOST_SO) && inst.start(*lib.entry);
    if (ok) {
      inst.set(SID_PARAM_VOLUME, 0.0f);
      inst.set(SID_PARAM_VEL_SENS, 0.0f);
      inst.set(SID_PARAM_ATTACK, 0.0f);
      inst.set(SID_PARAM_DECAY, 0.0f);
      inst.set(SID_PARAM_SUSTAIN, 15.0f);
      inst.set(SID_PARAM_RELEASE, 0.0f);
      inst.set(SID_PARAM_SWEEP_TIME, 0.0f);
      inst.set(SID_PARAM_PULSE_WIDTH, 50.0f);
      inst.set(SID_PARAM_WAVE, static_cast<float>(kPulse));
    }
  }
};

// The period of a periodic signal in samples, from the autocorrelation around the expected one (+-10 %), with a parabola through the peak.
double period_of(const std::vector<float>& s, double expect) {
  const std::size_t lo = static_cast<std::size_t>(expect * 0.9), hi = static_cast<std::size_t>(expect * 1.1) + 2;
  const std::size_t n = s.size() - hi - 2;
  std::vector<double> r(hi + 2, 0.0);
  for (std::size_t lag = lo - 1; lag <= hi + 1; ++lag) {
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      acc += static_cast<double>(s[i]) * static_cast<double>(s[i + lag]);
    }
    r[lag] = acc;
  }
  std::size_t best = lo;
  for (std::size_t lag = lo; lag <= hi; ++lag) {
    best = r[lag] > r[best] ? lag : best;
  }
  const double a = r[best - 1], b = r[best], c = r[best + 1];
  const double den = a - 2.0 * b + c;
  return static_cast<double>(best) + (den != 0.0 ? 0.5 * (a - c) / den : 0.0);
}

double rms_of(const std::vector<float>& s) {
  double acc = 0.0;
  for (float x : s) {
    acc += static_cast<double>(x) * static_cast<double>(x);
  }
  return std::sqrt(acc / static_cast<double>(s.size()));
}

double mean_of(const std::vector<float>& s) {
  double acc = 0.0;
  for (float x : s) {
    acc += static_cast<double>(x);
  }
  return acc / static_cast<double>(s.size());
}

// The share of the spectrum (Hann window, a plain DFT) that is not within 2.5 bins of a harmonic of f0: that is what aliasing and timing jitter are.
double inharmonic_share(const std::vector<float>& s, double f0) {
  const std::uint32_t n = static_cast<std::uint32_t>(s.size());
  std::vector<double> power(n / 2, 0.0);
  std::vector<double> w(n), c(n), sn(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    c[i] = std::cos(2.0 * kPi * i / n);
    sn[i] = std::sin(2.0 * kPi * i / n);
  }
  for (std::uint32_t k = 1; k < n / 2; ++k) {
    double re = 0.0, im = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::uint32_t m = static_cast<std::uint32_t>((static_cast<std::uint64_t>(k) * i) % n);
      re += w[i] * static_cast<double>(s[i]) * c[m];
      im -= w[i] * static_cast<double>(s[i]) * sn[m];
    }
    power[k] = re * re + im * im;
  }
  double total = 0.0, bad = 0.0;
  for (std::uint32_t k = 1; k < n / 2; ++k) {
    const double hz = static_cast<double>(k) * kRate / n;
    const double h = hz / f0;
    const double off = std::fabs(h - std::round(h)) * f0 / (static_cast<double>(kRate) / n);
    total += power[k];
    bad += off > 2.5 ? power[k] : 0.0;
  }
  return bad / total;
}

// The envelope as the audio shows it: a 50 % pulse is +-1 on its flat tops, so the largest |sample| of the last `w` samples is the level.
std::vector<float> level_of(const std::vector<float>& s, std::size_t w = 18) {
  std::vector<float> m(s.size(), 0.0f);
  for (std::size_t i = 0; i < s.size(); ++i) {
    float p = 0.0f;
    for (std::size_t k = 0; k < w && k <= i; ++k) {
      p = std::max(p, std::fabs(s[i - k]));
    }
    m[i] = p;
  }
  return m;
}

}  // namespace

TEST_CASE("sid: the descriptor is valid as the loader reads it, and the 31 parameters are in the final order on four pages") {
  Lib lib;
  NEED(lib.open(O3M_SID_HOST_SO));
  const o3m_module& m = *lib.entry;
  ModuleSet set;
  CHECK(set.add_native("sid", m));
  CHECK(std::string(m.name) == "SID");
  CHECK(m.kind == O3M_KIND_INSTRUMENT);
  CHECK(m.abi_major == O3M_ABI_MAJOR);
  CHECK(m.abi_minor == 8u);  // views, pitch, pages and labels, a version in its header
  CHECK(m.param_count == 31u);
  CHECK(m.param_count == SID_PARAM_COUNT);
  CHECK(m.view_count == SID_VIEW_COUNT);
  CHECK(m.pitch != nullptr);
  CHECK(m.state_bytes == sizeof(sid::Voice));
  CHECK(m.state_bytes <= SID_STATE_BYTES);
  // a preset is 31 little-endian float32 values in this order (docs/vision/radar/sid-instrument.md, Fits): the layout is final from this slice on
  const char* const names[31] = {"Wave",      "Pulse Width", "PWM Depth", "PWM Rate",  "Sync",      "Ring",      "Mod Pitch",  "Mod Fine",  "Attack",    "Decay",     "Sustain",
                                 "Release",   "Volume",      "Vel Sens",  "Transpose", "Fine",      "Mode",      "Cutoff",     "Resonance", "Key Track", "Env Amount", "Model",
                                 "Drive",     "Sweep",       "Sweep Time", "Vib Depth", "Vib Rate", "Vib Delay", "Arp 1",      "Arp 2",     "Arp Speed"};
  for (std::uint32_t i = 0; i < m.param_count; ++i) {
    const o3m_param& p = m.params[i];
    CAPTURE(std::string(p.name));
    CHECK(std::string(p.name) == names[i]);
    CHECK(std::strlen(p.name) < O3M_PARAM_NAME_MAX);
    CHECK(p.min < p.max);
    CHECK(p.min <= p.def);
    CHECK(p.def <= p.max);
  }
  CHECK((m.params[SID_PARAM_WAVE].flags & O3M_PARAM_INT) != 0u);
  CHECK((m.params[SID_PARAM_WAVE].flags & O3M_PARAM_NO_LEARN) == 0u);
  CHECK((m.params[SID_PARAM_SYNC].flags & O3M_PARAM_BOOL) != 0u);
  CHECK((m.params[SID_PARAM_RING].flags & O3M_PARAM_BOOL) != 0u);
  for (std::uint32_t id : {1u, 6u, 12u, 17u, 23u, 25u}) {  // Pulse Width, Mod Pitch, Volume, Cutoff, Sweep, Vib Depth
    CAPTURE(id);
    CHECK((m.params[id].flags & O3M_PARAM_MOD) != 0u);
  }
  CHECK((m.params[SID_PARAM_PWM_RATE].flags & O3M_PARAM_LOG) != 0u);
  // pages: OSC 0-7, ENV 8-15, FILTER 16-22 (seven), PITCH 23-30; labels where the value is a name
  NEED(m.pages != nullptr);
  CHECK(m.page_count == 4u);
  const unsigned first[4] = {0, 8, 16, 23};
  unsigned next = 0;
  for (std::uint32_t k = 0; k < m.page_count; ++k) {
    CHECK(m.pages[k].first_param == first[k]);
    CHECK(m.pages[k].first_param == next);
    const unsigned end = k + 1u < m.page_count ? m.pages[k + 1].first_param : m.param_count;
    CHECK((end > next && end - next <= O3M_PAGE_PARAMS_MAX));
    CHECK((std::strlen(m.pages[k].name) >= 1u && std::strlen(m.pages[k].name) < O3M_PAGE_NAME_MAX));
    next = end;
  }
  CHECK(next == m.param_count);
  CHECK(std::string(m.pages[0].name) == "OSC");
  CHECK(std::string(m.pages[2].name) == "FILTER");
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
  CHECK(m.labels[SID_PARAM_WAVE].count == 8u);
  CHECK(std::string(m.labels[SID_PARAM_WAVE].names[kPulseSawTri]) == "P+SAW+TRI");
  CHECK(m.labels[SID_PARAM_ATTACK].count == 16u);
  CHECK(m.labels[SID_PARAM_DECAY].count == 16u);
  CHECK(m.labels[SID_PARAM_RELEASE].count == 16u);
  CHECK(m.labels[SID_PARAM_MODE].count == 8u);
  CHECK(m.labels[SID_PARAM_MODEL].count == 2u);
  CHECK(std::string(m.labels[SID_PARAM_MODEL].names[0]) == "6581");
  CHECK(std::string(m.labels[SID_PARAM_MODEL].names[1]) == "8580");
  CHECK(m.labels[SID_PARAM_VOLUME].count == 0u);
}

TEST_CASE("sid: the parameter blob round trip: 31 floats applied in any order give the same voice, and a stored blob of the defaults is the voice's defaults") {
  Lib lib;
  NEED(lib.open(O3M_SID_HOST_SO));
  const o3m_module& m = *lib.entry;
  // a preset as the project stores it: little-endian float32 in parameter order
  std::vector<unsigned char> blob(4u * m.param_count);
  for (std::uint32_t i = 0; i < m.param_count; ++i) {
    const float v = m.params[i].def;
    std::memcpy(&blob[4u * i], &v, 4);
  }
  CHECK(blob.size() == 124u);
  auto render_blob = [&](const std::vector<unsigned char>& b, bool reverse) {
    Instance in;
    in.m = &m;
    in.host = make_host();
    CHECK(m.init(in.state, &in.host) == 0);
    for (std::uint32_t k = 0; k < m.param_count; ++k) {
      const std::uint32_t i = reverse ? m.param_count - 1u - k : k;
      float v;
      std::memcpy(&v, &b[4u * i], 4);
      in.set(i, v);
    }
    in.on(60);
    return in.render(4000);
  };
  Instance defaults;
  NEED(defaults.start(m));
  defaults.on(60);
  CHECK(render_blob(blob, false) == defaults.render(4000));
  // a preset with everything moved off its default: the order of the set calls does not matter, and clamping is the module's (the values are within range here)
  const float vals[31] = {6, 30, 40, 5, 1, 1, -5, 20, 3, 4, 7, 5, -6, 80, 2, -10, 3, 70, 9, 50, -40, 1, 60, 5, 300, 40, 8, 100, 3, 7, 20};
  for (std::uint32_t i = 0; i < 31; ++i) {
    std::memcpy(&blob[4u * i], &vals[i], 4);
  }
  const std::vector<float> a = render_blob(blob, false), b = render_blob(blob, true);
  CHECK(a == b);
  CHECK(rms_of(a) > 0.001);
}

TEST_CASE("sid: the pitch follows the note, the transposition and the fine tune, for every wave that has one") {
  Fixture f;
  NEED(f.ok);
  for (std::uint32_t wave : {kSaw, kTri, kPulse}) {
    f.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
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
  // the combined waves, by their period (their zero crossings are not one a period): both models
  for (std::uint32_t model : {0u, 1u}) {
    f.inst.set(SID_PARAM_MODEL, static_cast<float>(model));
    for (std::uint32_t wave : {kSawTri, kPulseSaw, kPulseTri, kPulseSawTri}) {
      f.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
      for (std::uint32_t note : {45u, 57u, 69u}) {
        CAPTURE(model);
        CAPTURE(wave);
        CAPTURE(note);
        f.inst.on(note);
        f.inst.render(512);
        const std::vector<float> s = f.inst.render(30000);
        const double expect = static_cast<double>(kRate) / hz_of_note(note);
        CHECK(period_of(s, expect) == doctest::Approx(expect).epsilon(2e-3));
        f.inst.off(note);
        f.inst.render(64);
      }
    }
  }
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kPulse));
  f.inst.set(SID_PARAM_TRANSPOSE, 7.0f);
  f.inst.set(SID_PARAM_FINE, -50.0f);
  f.inst.on(60);
  f.inst.render(512);
  const std::vector<float> s = f.inst.render(44100);
  CHECK(frequency(s.data(), s.size()) == doctest::Approx(hz_of_note(60.0 + 7.0 - 0.5)).epsilon(5e-4));
  // the highest notes stay below Nyquist (the step is held at 0.45 of the rate): no NaN, bounded, every wave
  f.inst.set(SID_PARAM_TRANSPOSE, 24.0f);
  for (std::uint32_t wave = 0; wave < 8; ++wave) {
    f.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    f.inst.on(127);
    for (float x : f.inst.render(4096)) {
      CHECK(std::isfinite(x));
      CHECK(std::fabs(x) <= 2.6f);
    }
  }
}

TEST_CASE("sid: the saw and the triangle are a saw and a triangle with no DC, and the noise-free waves are band-limited where they should be") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  f.inst.on(45);
  f.inst.render(512);
  std::vector<float> s = f.inst.render(4410);
  float lo = 1e9f, hi = -1e9f;
  for (float x : s) {
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  CHECK(hi == doctest::Approx(1.0f).epsilon(0.03));
  CHECK(lo == doctest::Approx(-1.0f).epsilon(0.03));
  CHECK(std::fabs(mean_of(s)) < 0.01);
  const float sstep = static_cast<float>(2.0 * hz_of_note(45) / kRate);  // a ramp of 2 over the period
  std::size_t straight = 0;
  for (std::size_t i = 1; i < s.size(); ++i) {
    straight += std::fabs(s[i] - s[i - 1] - sstep) < 2e-3f ? 1u : 0u;
  }
  CHECK(straight > s.size() - 11u * 8u);  // 110 Hz over 0.1 s: 11 wraps, each smoothed over a few samples
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kTri));
  f.inst.on(45);
  f.inst.render(512);
  s = f.inst.render(4410);
  lo = 1e9f;
  hi = -1e9f;
  for (float x : s) {
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  CHECK(hi == doctest::Approx(1.0f).epsilon(0.02));
  CHECK(lo == doctest::Approx(-1.0f).epsilon(0.02));
  CHECK(std::fabs(mean_of(s)) < 0.01);
  const float tstep = static_cast<float>(4.0 * hz_of_note(45) / kRate);
  straight = 0;
  for (std::size_t i = 1; i < s.size(); ++i) {
    straight += std::fabs(std::fabs(s[i] - s[i - 1]) - tstep) < 1e-3f ? 1u : 0u;
  }
  CHECK(straight > s.size() - 2u * 12u * 2u);
  // a saw's wrap is band-limited (PolyBLEP): at a high note the saw's inharmonic share is far below a naive saw's
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  f.inst.on(100);
  f.inst.render(777);
  const std::vector<float> hiw = f.inst.render(4096);
  const double f0 = frequency(hiw.data(), hiw.size());
  std::vector<float> naive(4096);
  double ph = 0.1;
  for (float& x : naive) {
    ph += f0 / kRate;
    ph -= ph >= 1.0 ? 1.0 : 0.0;
    x = static_cast<float>(2.0 * ph - 1.0);
  }
  CHECK(inharmonic_share(hiw, f0) < inharmonic_share(naive, f0) * 0.25);
}

TEST_CASE("sid: the pulse's width is the Pulse Width parameter, and the mean is removed") {
  Fixture f;
  NEED(f.ok);
  for (float pw : {10.0f, 25.0f, 50.0f, 75.0f, 90.0f}) {
    CAPTURE(pw);
    f.inst.set(SID_PARAM_PULSE_WIDTH, pw);
    f.inst.on(45);  // 110 Hz: 401 samples a period
    f.inst.render(2000);
    const std::vector<float> s = f.inst.render(40100);  // 100 periods
    const double d = static_cast<double>(pw) / 100.0;
    // the two levels of the pulse are 2 (1 - d) and -2 d; the threshold is half way between
    std::size_t high = 0;
    for (float x : s) {
      high += static_cast<double>(x) > 1.0 - 2.0 * d ? 1u : 0u;
    }
    CHECK(static_cast<double>(high) / static_cast<double>(s.size()) == doctest::Approx(d).epsilon(0.02));
    CHECK(std::fabs(mean_of(s)) < 0.01);
    f.inst.off(45);
    f.inst.render(256);
  }
}

namespace {

// Sign changes of a pulse's output (it is above 0 while high and below while low, whatever the width), over s.
std::size_t sign_changes(const std::vector<float>& s, std::size_t from = 0, std::size_t to = ~std::size_t{0}) {
  std::size_t n = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1u); i < s.size() && i < to; ++i) {
    n += (s[i - 1] > 0.0f) != (s[i] > 0.0f) ? 1u : 0u;
  }
  return n;
}

}  // namespace

TEST_CASE("sid: a pulse width sweep (the PWM LFO, a stepped parameter, a jump) has no clicks: two edges a period, no extra pulse, bounded") {
  Fixture f;
  NEED(f.ok);
  // the LFO: 80 % deep at 8 Hz moves the width between 10 and 90 %, at a low, a mid and a high note
  for (std::uint32_t note : {33u, 45u, 69u}) {
    CAPTURE(note);
    f.inst.set(SID_PARAM_PWM_DEPTH, 80.0f);
    f.inst.set(SID_PARAM_PWM_RATE, 8.0f);
    f.inst.on(note);
    f.inst.render(512);
    const std::vector<float> s = f.inst.render(44100);
    const double expect = 2.0 * hz_of_note(note);  // edges a second
    CHECK(static_cast<double>(sign_changes(s)) == doctest::Approx(expect).epsilon(0.02));
    for (float x : s) {
      CHECK(std::isfinite(x));
      CHECK(std::fabs(x) < 2.1f);
    }
    f.inst.off(note);
    f.inst.render(256);
  }
  // a width stepped every block from 5 to 95 % over a second
  f.inst.set(SID_PARAM_PWM_DEPTH, 0.0f);
  f.inst.set(SID_PARAM_PULSE_WIDTH, 5.0f);
  f.inst.on(45);
  f.inst.render(512);
  std::vector<float> s;
  for (std::uint32_t b = 0; b < 689; ++b) {
    f.inst.set(SID_PARAM_PULSE_WIDTH, 5.0f + 90.0f * static_cast<float>(b) / 688.0f);
    const std::vector<float> blk = f.inst.render(64);
    s.insert(s.end(), blk.begin(), blk.end());
  }
  CHECK(static_cast<double>(sign_changes(s)) == doctest::Approx(2.0 * hz_of_note(45) * 689.0 * 64.0 / kRate).epsilon(0.03));
  for (float x : s) {
    CHECK(std::fabs(x) < 2.1f);
  }
  // one jump from 5 % to 95 % in the middle of a note: the edge glides there (a few periods), it never makes a second pulse
  f.inst.on(45);
  f.inst.set(SID_PARAM_PULSE_WIDTH, 5.0f);
  f.inst.on(45);
  f.inst.render(2000);
  f.inst.set(SID_PARAM_PULSE_WIDTH, 95.0f);
  const std::vector<float> j = f.inst.render(4000);
  const double periods = 4000.0 * hz_of_note(45) / kRate;
  CHECK(static_cast<double>(sign_changes(j)) <= 2.0 * periods + 3.0);
  CHECK(static_cast<double>(sign_changes(j)) >= 2.0 * periods - 3.0);
  // the width never leaves 1..99 % whatever the parameters say
  f.inst.set(SID_PARAM_PULSE_WIDTH, 99.0f);
  f.inst.set(SID_PARAM_PWM_DEPTH, 100.0f);
  f.inst.on(60);
  for (float x : f.inst.render(44100)) {
    CHECK(std::isfinite(x));
    CHECK(std::fabs(x) < 2.1f);
  }
}

TEST_CASE("sid: the register is 23 bits and has the maximal period 2^23 - 1") {
  const std::uint32_t seed = sid::kLfsrSeed;
  CHECK(seed != 0u);
  CHECK(seed <= sid::kLfsrMask);
  std::uint32_t r = seed;
  bool early = false;
  constexpr std::uint32_t kPeriod = (1u << 23) - 1u;
  for (std::uint32_t n = 1; n <= kPeriod; ++n) {
    r = sid::lfsr_step(r);
    early = early || (r == seed && n < kPeriod);
    CHECK(r <= sid::kLfsrMask);
  }
  CHECK_FALSE(early);
  CHECK(r == seed);  // back to the seed after 8,388,607 steps and not before: a primitive polynomial
}

TEST_CASE("sid: the noise is eight-bit uniform-looking, sample-and-held at 16 x the note's frequency, and follows the pitch") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kNoise));
  for (std::uint32_t note : {57u, 69u, 81u}) {
    CAPTURE(note);
    f.inst.on(note);
    f.inst.render(512);
    const std::vector<float> s = f.inst.render(441000);  // ten seconds
    CHECK(std::fabs(mean_of(s)) < 0.02);
    CHECK(rms_of(s) == doctest::Approx(0.577).epsilon(0.05));  // uniform over +-1
    std::set<float> levels;
    std::size_t flips = 0;
    float peak = 0.0f;
    for (std::size_t i = 0; i < s.size(); ++i) {
      levels.insert(s[i]);
      peak = std::max(peak, std::fabs(s[i]));
      flips += i > 0 && s[i] != s[i - 1] ? 1u : 0u;
    }
    CHECK(peak <= 1.0f);
    CHECK(levels.size() >= 200u);  // 8 bits: 256 levels
    const double clock = std::min(16.0 * hz_of_note(note), static_cast<double>(kRate));
    CHECK(static_cast<double>(flips) / 10.0 == doctest::Approx(clock).epsilon(0.03));
    f.inst.off(note);
    f.inst.render(256);
  }
}

TEST_CASE("sid: the attack takes the Programmer's Reference Guide's time for every rate value, within one control step") {
  Fixture f;
  NEED(f.ok);
  for (std::uint32_t a = 0; a < 16; ++a) {
    CAPTURE(a);
    f.inst.set(SID_PARAM_ATTACK, static_cast<float>(a));
    const std::uint32_t frames = static_cast<std::uint32_t>(kAttackTable[a] * 44.1 + 3000.0);
    f.inst.on(100);  // 2637 Hz: a period of 17 samples, so the flat top of the pulse is seen every few samples
    const std::vector<float> lv = level_of(f.inst.render(frames));
    std::size_t at = 0;
    while (at < lv.size() && lv[at] < 0.995f) {
      ++at;
    }
    const double expect = kAttackTable[a] * 44.1 * 254.0 / 255.0;  // the counter shows 254 of 255 here
    CHECK(std::fabs(static_cast<double>(at) - expect) <= 32.0);
    CHECK(lv.back() == doctest::Approx(1.0f).epsilon(1e-4));  // held at the top (sustain 15)
    f.inst.off(100);
    f.inst.render(2000);
  }
}

TEST_CASE("sid: the decay takes the table's time for a full-scale fall, the sustain is a 4-bit level, and a fall to a sustain takes its share") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_SUSTAIN, 0.0f);  // fall to 0: the note ends by itself
  for (std::uint32_t d = 0; d < 16; ++d) {
    CAPTURE(d);
    f.inst.set(SID_PARAM_DECAY, static_cast<float>(d));
    const std::uint32_t frames = static_cast<std::uint32_t>((2.0 + kDecayTable[d]) * 44.1 + 3000.0);
    f.inst.on(100);
    const std::vector<float> lv = level_of(f.inst.render(frames));
    std::size_t at = 0;
    while (at < lv.size() && lv[at] > 1.5f / 255.0f) {
      ++at;
    }
    const double expect = (2.0 + kDecayTable[d] * 253.0 / 255.0) * 44.1;  // attack of 2 ms, then the counter is below 2
    // one control step (32 samples), the window of the level (it follows a fall by up to 18 samples) and the slope's rounding to 2^-24 of a counter step a sample (1.3e-4 of the time at 24 s)
    CHECK(std::fabs(static_cast<double>(at) - expect) <= 32.0 + 18.0 + 2e-4 * expect);
    CHECK(f.inst.busy == 0u);  // the voice says it is idle: a sustain of 0 ends the note
  }
  // a sustain of 8 holds the level 8 x 17 = 136 of 255, and the fall to it takes (255 - 136) / 255 of the table's time
  f.inst.set(SID_PARAM_SUSTAIN, 8.0f);
  f.inst.set(SID_PARAM_DECAY, 9.0f);  // 750 ms
  f.inst.on(100);
  const std::vector<float> lv = level_of(f.inst.render(44100));
  const double fall_ms = 750.0 * (255.0 - 136.0) / 255.0;
  CHECK(lv[static_cast<std::size_t>((2.0 + 0.5 * fall_ms) * 44.1)] == doctest::Approx(1.0 - 0.5 * (119.0 / 255.0)).epsilon(0.02));
  CHECK(lv[static_cast<std::size_t>((2.0 + fall_ms + 50.0) * 44.1)] == doctest::Approx(136.0 / 255.0).epsilon(1e-3));
  CHECK(lv.back() == doctest::Approx(136.0 / 255.0).epsilon(1e-3));
  CHECK(f.inst.busy == 1u);  // held: still sounding
}

TEST_CASE("sid: the release takes the table's time from the level the note is at, and the voice is idle at the end of it") {
  Fixture f;
  NEED(f.ok);
  for (std::uint32_t r = 0; r < 16; ++r) {
    CAPTURE(r);
    f.inst.set(SID_PARAM_RELEASE, static_cast<float>(r));
    f.inst.on(100);
    f.inst.render(2000);  // at the top
    f.inst.off(100);
    const std::uint32_t frames = static_cast<std::uint32_t>(kDecayTable[r] * 44.1 + 3000.0);
    const std::vector<float> lv = level_of(f.inst.render(frames));
    std::size_t at = 0;
    while (at < lv.size() && lv[at] > 1.5f / 255.0f) {
      ++at;
    }
    CHECK(std::fabs(static_cast<double>(at) - kDecayTable[r] * 253.0 / 255.0 * 44.1) <= 32.0 + 18.0 + 2e-4 * kDecayTable[r] * 44.1);
    CHECK(f.inst.busy == 0u);
    CHECK(f.inst.render(64)[10] == 0.0f);
  }
  // from a lower level it is proportionally shorter: sustain 8 (136 of 255), release rate 9 (750 ms): 400 ms
  f.inst.set(SID_PARAM_SUSTAIN, 8.0f);
  f.inst.set(SID_PARAM_RELEASE, 9.0f);
  f.inst.on(100);
  f.inst.render(2000);
  f.inst.off(100);
  const std::vector<float> lv = level_of(f.inst.render(44100));
  CHECK(lv[static_cast<std::size_t>(0.5 * 400.0 * 44.1)] == doctest::Approx(0.5 * 136.0 / 255.0).epsilon(0.03));
  CHECK(lv[static_cast<std::size_t>(450.0 * 44.1)] == 0.0f);
  CHECK(f.inst.busy == 0u);
}

TEST_CASE("sid: the envelope is the chip's 8-bit staircase: the level is a whole counter step, and a slow attack walks all of them") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_ATTACK, 13.0f);  // 3 s
  f.inst.on(100);
  const std::vector<float> s = f.inst.render(static_cast<std::uint32_t>(3.1 * 44100));
  std::set<int> steps;
  std::size_t whole_steps = 0;
  for (float x : s) {
    const float v = std::fabs(x) * 255.0f;
    whole_steps += std::fabs(v - std::round(v)) < 2e-3f ? 1u : 0u;
    if (std::fabs(v - std::round(v)) < 2e-3f) {
      steps.insert(static_cast<int>(std::round(v)));
    }
  }
  CHECK(static_cast<double>(whole_steps) > 0.5 * static_cast<double>(s.size()));  // the flat tops of the pulse: whole steps
  CHECK(*steps.rbegin() == 255);
  CHECK(steps.size() >= 240u);
}

TEST_CASE("sid: a note over a sounding one continues from the counter's level, a note off before the first sample leaves the voice idle at once, and an idle voice is silent") {
  Fixture f;
  NEED(f.ok);
  CHECK(f.inst.render(64)[10] == 0.0f);
  CHECK(f.inst.busy == 0u);
  f.inst.set(SID_PARAM_ATTACK, 10.0f);   // 500 ms
  f.inst.set(SID_PARAM_RELEASE, 10.0f);  // 1.5 s
  f.inst.on(100);
  f.inst.render(11025);  // 250 ms: the counter is at 128
  f.inst.off(100);
  f.inst.render(4410);  // 100 ms into a release of 1.5 s: 128 - 17 = 111
  f.inst.on(100);
  const std::vector<float> s = level_of(f.inst.render(64));
  CHECK(s[40] > 108.0f / 255.0f);  // no jump to full, no jump to zero
  CHECK(s[40] < 120.0f / 255.0f);
  // a note off in the dispatch pass of its note on: nothing to release
  Fixture g;
  NEED(g.ok);
  g.inst.set(SID_PARAM_ATTACK, 5.0f);
  g.inst.on(60);
  g.inst.off(60);
  for (float x : g.inst.render(64)) {
    CHECK(x == 0.0f);
  }
  CHECK(g.inst.busy == 0u);
  g.inst.set(SID_PARAM_RELEASE, 0.0f);
  g.inst.on(60);
  g.inst.render(441);
  CHECK(g.inst.busy == 1u);
  g.inst.off(60);
  g.inst.render(441);
  CHECK(g.inst.busy == 0u);
}

TEST_CASE("sid: under sync the pitch heard is the key's whatever Mod Pitch is: the period is the key's, for an oscillator below and above the key") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  f.inst.set(SID_PARAM_SYNC, 1.0f);
  const double expect = static_cast<double>(kRate) / hz_of_note(57);
  for (float semis : {-12.0f, -5.0f, 0.0f, 7.0f, 12.0f, 19.0f}) {
    CAPTURE(semis);
    f.inst.set(SID_PARAM_MOD_PITCH, semis);
    f.inst.on(57);
    f.inst.render(1024);
    const std::vector<float> s = f.inst.render(30000);
    CHECK(period_of(s, expect) == doctest::Approx(expect).epsilon(2e-3));
    f.inst.off(57);
    f.inst.render(64);
  }
  // Mod Fine moves the oscillator, not the key
  f.inst.set(SID_PARAM_MOD_PITCH, 7.0f);
  f.inst.set(SID_PARAM_MOD_FINE, 50.0f);
  f.inst.on(57);
  f.inst.render(1024);
  const std::vector<float> s = f.inst.render(30000);
  CHECK(period_of(s, expect) == doctest::Approx(expect).epsilon(2e-3));
  // the timbre does move with Mod Pitch: the same key, a different wave
  Fixture g;
  NEED(g.ok);
  g.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  g.inst.set(SID_PARAM_SYNC, 1.0f);
  g.inst.set(SID_PARAM_MOD_PITCH, 7.0f);
  g.inst.on(57);
  Fixture h;
  NEED(h.ok);
  h.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  h.inst.set(SID_PARAM_SYNC, 1.0f);
  h.inst.set(SID_PARAM_MOD_PITCH, 12.0f);
  h.inst.on(57);
  CHECK(g.inst.render(4000) != h.inst.render(4000));
  // Sync off: the oscillator is the key and the modulator (ring) is the one moved by Mod Pitch: the pitch is the key's too
  Fixture r;
  NEED(r.ok);
  r.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  r.inst.set(SID_PARAM_MOD_PITCH, 12.0f);
  r.inst.on(57);
  const std::vector<float> plain = r.inst.render(30000);
  CHECK(frequency(plain.data(), plain.size()) == doctest::Approx(hz_of_note(57)).epsilon(2e-3));
}

TEST_CASE("sid: sync resets the oscillator at the sync source's wrap to a fraction of a sample: no alias spike against a naive reset") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  f.inst.set(SID_PARAM_SYNC, 1.0f);
  const double note_hz = hz_of_note(80);
  for (float semis : {3.0f, 7.0f, 14.0f}) {
    CAPTURE(semis);
    f.inst.set(SID_PARAM_MOD_PITCH, semis);
    f.inst.on(80);
    f.inst.render(1024);
    const std::vector<float> mod = f.inst.render(4096);
    f.inst.off(80);
    f.inst.render(64);
    const double master = note_hz, slave_hz = note_hz * std::pow(2.0, static_cast<double>(semis) / 12.0);  // the key is the sync source, the oscillator is the one Mod Pitch moves
    // the naive reference: the same two oscillators in double, the slave reset to 0 at the first whole sample at or after the master's wrap, no band-limiting at all
    std::vector<float> naive(4096);
    double slave = 0.0, mph = 0.0;
    for (float& x : naive) {
      mph += master / kRate;
      slave += slave_hz / kRate;
      slave -= slave >= 1.0 ? 1.0 : 0.0;
      if (mph >= 1.0) {
        mph -= 1.0;
        slave = 0.0;
      }
      x = static_cast<float>(2.0 * slave - 1.0);
    }
    const double a = inharmonic_share(mod, master), b = inharmonic_share(naive, master);
    MESSAGE("sync " << semis << " semitones: inharmonic share " << a << " against the naive reset's " << b);
    CHECK(a < b * 0.25);  // at least 6 dB below the naive reset's aliasing and timing jitter
    float peak = 0.0f;
    for (float x : mod) {
      peak = std::max(peak, std::fabs(x));
    }
    CHECK(peak < 1.4f);  // the BLEP's overshoot is small: no spike
  }
  // other waves and the combined ones under sync: bounded, no NaN, and the same period
  for (std::uint32_t wave : {kTri, kPulse, kSawTri, kPulseSaw, kPulseSawTri}) {
    CAPTURE(wave);
    f.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    f.inst.set(SID_PARAM_MOD_PITCH, 7.0f);
    f.inst.on(69);
    f.inst.render(1024);
    const std::vector<float> s = f.inst.render(20000);
    for (float x : s) {
      CHECK(std::isfinite(x));
      CHECK(std::fabs(x) < 2.6f);
    }
    const double expect = static_cast<double>(kRate) / hz_of_note(69);  // the key's period
    CHECK(period_of(s, expect) == doctest::Approx(expect).epsilon(2e-3));
    f.inst.off(69);
    f.inst.render(64);
  }
}

TEST_CASE("sid: with Sync and Ring off the modulator does nothing: Mod Pitch and Mod Fine change no sample") {
  Fixture a, b;
  NEED(a.ok);
  NEED(b.ok);
  for (std::uint32_t wave : {kSaw, kTri, kPulse, kSawTri}) {
    a.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    b.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    b.inst.set(SID_PARAM_MOD_PITCH, -19.0f);
    b.inst.set(SID_PARAM_MOD_FINE, 33.0f);
    a.inst.on(64);
    b.inst.on(64);
    CHECK(a.inst.render(4000) == b.inst.render(4000));
  }
}

TEST_CASE("sid: ring multiplies the triangle by the modulator's sign, and leaves the other waves alone") {
  Fixture a, b;
  NEED(a.ok);
  NEED(b.ok);
  for (auto* f : {&a, &b}) {
    f->inst.set(SID_PARAM_WAVE, static_cast<float>(kTri));
    f->inst.set(SID_PARAM_MOD_PITCH, -12.0f);  // the modulator an octave below the note
  }
  b.inst.set(SID_PARAM_RING, 1.0f);
  a.inst.on(57);
  b.inst.on(57);
  const std::vector<float> plain = a.inst.render(44100), ring = b.inst.render(44100);
  std::size_t flips = 0;
  float last = 0.0f;
  for (std::size_t i = 0; i < plain.size(); ++i) {
    if (std::fabs(plain[i]) < 1e-3f) {
      continue;  // (a sign is not defined at the triangle's zero)
    }
    const float sign = ring[i] / plain[i];
    CHECK(sign == doctest::Approx(sign > 0.0f ? 1.0f : -1.0f).epsilon(1e-4));
    flips += last != 0.0f && (sign > 0.0f) != (last > 0.0f) ? 1u : 0u;
    last = sign;
  }
  CHECK(static_cast<double>(flips) == doctest::Approx(2.0 * hz_of_note(45)).epsilon(0.03));  // the modulator is note 45: two sign changes a period
  // ring is about the triangle: a pulse and a saw are the same with it on
  for (std::uint32_t wave : {kSaw, kPulse}) {
    a.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    b.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    a.inst.on(57);
    b.inst.on(57);
    CHECK(a.inst.render(3000) == b.inst.render(3000));
  }
}

TEST_CASE("sid: the combined waves are bounded, have no DC, are not silent, differ from each other, and the 8580's are louder than the 6581's") {
  // the tables themselves: eight, each peaking where the normalisation says, no two alike
  for (std::uint32_t t = 0; t < 8; ++t) {
    CAPTURE(t);
    std::uint32_t peak = 0;
    for (std::uint32_t i = 0; i < sid::kTableN; ++i) {
      peak = std::max<std::uint32_t>(peak, sid::kTables.t[t][i]);
    }
    CHECK(peak == (t < 4 ? 191u : 255u));
    const double mean = static_cast<double>(sid::kCum.c[t][16]) / (128.0 * 255.0);  // the DC the output subtracts
    CHECK(mean > 0.02);
    CHECK(mean < 0.5);
    for (std::uint32_t k = 0; k < 16; ++k) {
      CHECK(sid::kCum.c[t][k + 1] >= sid::kCum.c[t][k]);
    }
    CHECK(sid::table_dc(t, 1.0f) == doctest::Approx(static_cast<float>(mean)).epsilon(1e-5));
    CHECK(sid::table_dc(t, 0.0f) == 0.0f);
    for (std::uint32_t u = t + 1; u < 8; ++u) {
      CHECK(std::memcmp(sid::kTables.t[t], sid::kTables.t[u], sid::kTableN) != 0);
    }
  }
  Fixture f;
  NEED(f.ok);
  double level[2][4] = {};
  std::vector<float> shape[2][4];
  for (std::uint32_t model = 0; model < 2; ++model) {
    f.inst.set(SID_PARAM_MODEL, static_cast<float>(model));
    for (std::uint32_t k = 0; k < 4; ++k) {
      f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSawTri + k));
      f.inst.set(SID_PARAM_PULSE_WIDTH, 75.0f);
      f.inst.on(45);
      f.inst.render(2000);
      const std::vector<float> s = f.inst.render(40100);  // 100 periods
      CAPTURE(model);
      CAPTURE(k);
      for (float x : s) {
        CHECK(std::isfinite(x));
        CHECK(std::fabs(x) <= 2.01f);
      }
      CHECK(std::fabs(mean_of(s)) < 0.03);
      level[model][k] = rms_of(s);
      CHECK(level[model][k] > 0.03);
      shape[model][k].assign(s.begin(), s.begin() + 401);
      f.inst.off(45);
      f.inst.render(256);
    }
  }
  for (std::uint32_t k = 0; k < 4; ++k) {
    CAPTURE(k);
    CHECK(level[1][k] > level[0][k] * 1.1);  // the 8580's combined waves are louder
    CHECK(shape[0][k] != shape[1][k]);
    for (std::uint32_t j = k + 1; j < 4; ++j) {
      CHECK(shape[1][k] != shape[1][j]);
    }
  }
  // the pulse is a gate in the three that have one: at a width of 1 % PULSE+SAW is nearly silent, at 99 % it sounds; SAW+TRI has no gate
  double quiet = 0.0, loud = 0.0, st1 = 0.0, st99 = 0.0;
  for (float pw : {1.0f, 99.0f}) {
    f.inst.set(SID_PARAM_MODEL, 1.0f);
    f.inst.set(SID_PARAM_PULSE_WIDTH, pw);
    f.inst.set(SID_PARAM_WAVE, static_cast<float>(kPulseSaw));
    f.inst.on(45);
    f.inst.render(2000);
    (pw < 50.0f ? quiet : loud) = rms_of(f.inst.render(40100));
    f.inst.off(45);
    f.inst.render(256);
    f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSawTri));
    f.inst.on(45);
    f.inst.render(2000);
    (pw < 50.0f ? st1 : st99) = rms_of(f.inst.render(40100));
    f.inst.off(45);
    f.inst.render(256);
  }
  CHECK(quiet < loud * 0.5);
  CHECK(st1 == doctest::Approx(st99).epsilon(0.02));
  // the model is the combined tables only: the plain waves are the same on both
  Fixture m0, m1;
  NEED(m0.ok);
  NEED(m1.ok);
  m1.inst.set(SID_PARAM_MODEL, 1.0f);
  for (std::uint32_t wave : {kSaw, kTri, kPulse, kNoise}) {
    m0.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    m1.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    m0.inst.on(64);
    m1.inst.on(64);
    CHECK(m0.inst.render(3000) == m1.inst.render(3000));
  }
}

// ---- the filter -----------------------------------------------------------------------------------------------------------------------------------

namespace {

int g_sin_calls = 0, g_exp2_calls = 0;
float counting_sin(float x) {
  ++g_sin_calls;
  return std::sin(x);
}
float counting_exp2(float x) {
  ++g_exp2_calls;
  return std::exp2(x);
}

// The module's own cutoff curve, written again from the README: the 8580 an exponential from 30 Hz to 12 kHz, the 6581 an S-shaped one (smoothstep) from 150 Hz to 12 kHz.
double curve_hz(std::uint32_t model, double cutoff_percent) {
  const double c = cutoff_percent * 0.01;
  return model == 1u ? 30.0 * std::pow(400.0, c) : 150.0 * std::pow(80.0, c * c * (3.0 - 2.0 * c));
}

// The filter's digital response: the bilinear prewarp w = tan(pi f / fs) / tan(pi fc / fs), damping k, and the mix (lp, bp x k, hp).
double response(double f, double fc, double k, std::uint32_t mode) {
  const std::uint32_t bits[8] = {0, 1, 2, 4, 5, 3, 6, 7};
  const double lp = bits[mode] & 1u, bp = (bits[mode] >> 1) & 1u, hp = (bits[mode] >> 2) & 1u;
  const double w = std::tan(kPi * f / kRate) / std::tan(kPi * fc / kRate), w2 = w * w;
  const double re = lp - hp * w2, im = bp * k * w;
  return std::sqrt((re * re + im * im) / ((1.0 - w2) * (1.0 - w2) + k * k * w2));
}

double damping(std::uint32_t res) { return std::sqrt(2.0) * std::pow(20.0, -static_cast<double>(res) / 15.0); }

// The filter alone on a Voice: set the parameters, make the coefficients for a cutoff position, run a sine through it, and read the gain.
struct FilterRig {
  o3m_host host = make_host();
  sid::Voice v{};
  FilterRig() { sid::voice_init(v, &host); }
  void set(std::uint32_t id, float x) { sid::voice_set_param(v, id, x); }
  void setup(std::uint32_t model, std::uint32_t mode, float cutoff, std::uint32_t res) {
    set(SID_PARAM_MODEL, static_cast<float>(model));
    set(SID_PARAM_MODE, static_cast<float>(mode));
    set(SID_PARAM_CUTOFF, cutoff);
    set(SID_PARAM_RESONANCE, static_cast<float>(res));
    set(SID_PARAM_KEY_TRACK, 0.0f);
    set(SID_PARAM_ENV_AMOUNT, 0.0f);
    set(SID_PARAM_DRIVE, 0.0f);
    v.env = 0u;
    v.fe_last = -100.0f;
    sid::filter_control(v, 60.0f);
  }
  // RMS gain at f for a sine of amplitude `a` (small: the soft clip is a straight line there)
  double gain(double f, double a = 0.004) {
    v.fs1 = 0.0f;
    v.fs2 = 0.0f;
    std::vector<float> x(24000);
    for (std::size_t i = 0; i < x.size(); ++i) {
      x[i] = static_cast<float>(a * std::sin(2.0 * kPi * f * static_cast<double>(i) / kRate));
    }
    std::vector<float> y = x;
    sid::filter_run(v, y.data(), static_cast<std::uint32_t>(y.size()));
    double ex = 0.0, ey = 0.0;
    const double periods = std::floor(12000.0 * f / kRate);  // whole periods of the sine after the first 12000 samples
    const std::size_t end = 12000u + static_cast<std::size_t>(std::lround(periods * kRate / f));
    for (std::size_t i = 12000; i < end && i < x.size(); ++i) {
      ex += static_cast<double>(x[i]) * x[i];
      ey += static_cast<double>(y[i]) * y[i];
    }
    return std::sqrt(ey / ex);
  }
};

}  // namespace

TEST_CASE("sid filter: the response follows the cutoff curve of the model, for every mode: gains at five frequencies against the filter's own formula") {
  FilterRig r;
  for (std::uint32_t model = 0; model < 2; ++model) {
    for (float cut : {40.0f, 60.0f, 80.0f}) {
      const double fc = curve_hz(model, cut);
      for (std::uint32_t res : {0u, 7u, 15u}) {
        for (std::uint32_t mode = 1; mode < 8; ++mode) {
          r.setup(model, mode, cut, res);
          for (double ratio : {0.05, 0.4, 0.9, 1.0, 1.7}) {
            const double f = fc * ratio;
            CAPTURE(model);
            CAPTURE(cut);
            CAPTURE(res);
            CAPTURE(mode);
            CAPTURE(ratio);
            const double want = response(f, fc, damping(res), mode);
            const double got = r.gain(f);
            if (want > 0.01) {
              CHECK(got == doctest::Approx(want).epsilon(0.06));
            } else {
              CHECK(got < 0.03);
            }
          }
        }
      }
    }
  }
}

TEST_CASE("sid filter: the low pass at Resonance 0 is -3 dB at the cutoff of the curve, the high pass too; resonance raises the peak to 1 / k; the band pass peaks at 1; the notch is deep") {
  FilterRig r;
  for (std::uint32_t model = 0; model < 2; ++model) {
    for (float cut : {40.0f, 60.0f, 80.0f}) {
      const double fc = curve_hz(model, cut);
      CAPTURE(model);
      CAPTURE(cut);
      for (std::uint32_t mode : {1u, 3u}) {  // LP, HP: find the -3 dB frequency by bisection and compare with the curve
        r.setup(model, mode, cut, 0);
        double lo = fc * 0.5, hi = fc * 2.0;
        for (int it = 0; it < 22; ++it) {
          const double mid = std::sqrt(lo * hi);
          const bool above = r.gain(mid) > 0.70710678;
          ((mode == 1u) == above ? lo : hi) = mid;  // the LP falls with frequency, the HP rises
        }
        CHECK(std::sqrt(lo * hi) == doctest::Approx(fc).epsilon(0.02));
      }
      r.setup(model, 1, cut, 15);
      const double peak = r.gain(fc);
      CHECK(peak == doctest::Approx(1.0 / damping(15)).epsilon(0.07));  // about 23 dB
      r.setup(model, 1, cut, 0);
      CHECK(r.gain(fc) < peak * 0.1);
      r.setup(model, 2, cut, 15);
      CHECK(r.gain(fc) == doctest::Approx(1.0).epsilon(0.05));
      r.setup(model, 4, cut, 0);
      CHECK(r.gain(fc) < 0.01);  // the notch's depth at the cutoff: below -40 dB
      CHECK(r.gain(fc * 0.1) == doctest::Approx(1.0).epsilon(0.05));
      CHECK(r.gain(fc * 6.0) == doctest::Approx(1.0).epsilon(0.1));
    }
  }
}

TEST_CASE("sid filter: stable at the highest resonance and the extreme cutoffs, every mode, both models, full-scale input and the most Drive: finite and bounded by the soft clip") {
  Lib lib;
  NEED(lib.open(O3M_SID_HOST_SO));
  for (std::uint32_t model = 0; model < 2; ++model) {
    for (std::uint32_t mode = 1; mode < 8; ++mode) {
      for (float cut : {0.0f, 100.0f}) {
        for (std::uint32_t wave : {kSaw, kNoise, kPulse}) {
          Instance in;
          NEED(in.start(*lib.entry));
          in.set(SID_PARAM_WAVE, static_cast<float>(wave));
          in.set(SID_PARAM_MODEL, static_cast<float>(model));
          in.set(SID_PARAM_MODE, static_cast<float>(mode));
          in.set(SID_PARAM_CUTOFF, cut);
          in.set(SID_PARAM_RESONANCE, 15.0f);
          in.set(SID_PARAM_DRIVE, 100.0f);
          in.set(SID_PARAM_KEY_TRACK, 100.0f);
          in.set(SID_PARAM_ENV_AMOUNT, 100.0f);
          in.set(SID_PARAM_VOLUME, 6.0f);
          in.set(SID_PARAM_VEL_SENS, 0.0f);
          in.set(SID_PARAM_ATTACK, 0.0f);
          in.set(SID_PARAM_SUSTAIN, 15.0f);
          CAPTURE(model);
          CAPTURE(mode);
          CAPTURE(cut);
          CAPTURE(wave);
          in.on(127);
          const float bound = model == 0u ? 0.1251f : 0.5001f;  // the clip's peak a model has (1 and 4) times the compensation of full Drive, (1 + 1) / 16
          for (std::uint32_t block = 0; block < 60; ++block) {
            for (float x : in.render(64)) {
              CHECK(std::isfinite(x));
              CHECK(std::fabs(x) <= bound);
            }
          }
          in.off(127);
          for (float x : in.render(30000)) {
            CHECK(std::isfinite(x));
            CHECK(std::fabs(x) <= bound);
          }
        }
      }
    }
  }
}

TEST_CASE("sid filter: Drive is level-compensated: full Drive is at most +6 dB over Mode OFF at the same note and Volume, on both models, and the saturation is audible") {
  Lib lib;
  NEED(lib.open(O3M_SID_HOST_SO));
  const auto peak_of = [&](std::uint32_t model, std::uint32_t wave, float volume, bool drive) {
    Instance in;
    in.start(*lib.entry);
    in.set(SID_PARAM_WAVE, static_cast<float>(wave));
    in.set(SID_PARAM_MODEL, static_cast<float>(model));
    in.set(SID_PARAM_VOLUME, volume);
    in.set(SID_PARAM_VEL_SENS, 0.0f);
    in.set(SID_PARAM_ATTACK, 0.0f);
    in.set(SID_PARAM_SUSTAIN, 15.0f);
    if (drive) {
      in.set(SID_PARAM_MODE, 1.0f);  // LP wide open, no resonance: the filter alone adds nothing
      in.set(SID_PARAM_CUTOFF, 100.0f);
      in.set(SID_PARAM_RESONANCE, 0.0f);
      in.set(SID_PARAM_DRIVE, 100.0f);
    }
    in.on(60);
    in.render(256);  // past the start of the filter
    double sum = 0.0;  // the RMS level: a wide-open low pass never raises it, where a peak of noise or of a saw moves with the filter's phase
    for (std::uint32_t block = 0; block < 40; ++block) {
      for (float x : in.render(64)) {
        sum += static_cast<double>(x) * static_cast<double>(x);
      }
    }
    return static_cast<float>(std::sqrt(sum / (40.0 * 64.0)));
  };
  for (std::uint32_t model = 0; model < 2; ++model) {
    for (std::uint32_t wave : {kSaw, kTri, kPulse, kNoise}) {
      for (float volume : {-48.0f, -24.0f, -12.0f, 0.0f, 6.0f}) {
        CAPTURE(model);
        CAPTURE(wave);
        CAPTURE(volume);
        const float off = peak_of(model, wave, volume, false);
        const float on = peak_of(model, wave, volume, true);
        CHECK(off > 0.0f);
        CHECK(on <= 2.0f * off * 1.01f);  // +6 dB in level (RMS)
        if (volume == -48.0f && wave == kTri) {
          CHECK(on >= 1.6f * off);  // a small signal sees the compensated gain, 1 + Drive: Drive is not silent
        }
      }
    }
  }
}

TEST_CASE("sid filter: Mode OFF to a filter mid-note remakes the coefficients for the cutoff the knob says now, not the one the filter last ran at") {
  o3m_host host = make_host();
  sid::Voice v{};
  sid::voice_init(v, &host);
  sid::voice_set_param(v, SID_PARAM_ATTACK, 0.0f);
  sid::voice_set_param(v, SID_PARAM_SUSTAIN, 15.0f);
  sid::voice_set_param(v, SID_PARAM_WAVE, static_cast<float>(kSaw));
  sid::voice_set_param(v, SID_PARAM_ENV_AMOUNT, 0.0f);
  sid::voice_set_param(v, SID_PARAM_KEY_TRACK, 0.0f);
  sid::voice_set_param(v, SID_PARAM_MODEL, 1.0f);
  sid::voice_set_param(v, SID_PARAM_MODE, 1.0f);
  sid::voice_set_param(v, SID_PARAM_CUTOFF, 90.0f);
  float out[64];
  sid::voice_note_on(v, 60, 100);
  for (int i = 0; i < 10; ++i) {
    sid::voice_render(v, out, 64);
  }
  CHECK(v.fe_last == doctest::Approx(std::log2(curve_hz(1, 90.0))).epsilon(1e-3));
  sid::voice_set_param(v, SID_PARAM_MODE, 0.0f);  // OFF mid-note: the filter does not follow the knob
  sid::voice_set_param(v, SID_PARAM_CUTOFF, 10.0f);
  for (int i = 0; i < 10; ++i) {
    sid::voice_render(v, out, 64);
  }
  sid::voice_set_param(v, SID_PARAM_MODE, 1.0f);
  sid::voice_render(v, out, 32);  // one control period
  CHECK(v.fe_last == doctest::Approx(std::log2(curve_hz(1, 10.0))).epsilon(1e-3));
  sid::voice_set_param(v, SID_PARAM_RESONANCE, 5.0f);  // and a Resonance change keeps the cutoff it runs at
  CHECK(v.fe_last == doctest::Approx(std::log2(curve_hz(1, 10.0))).epsilon(1e-3));
}

TEST_CASE("sid filter: the soft clip is bounded by 1, odd, monotone to its knee and a straight line of slope 1 at 0") {
  float last = -2.0f;
  for (float x = -1.5f; x <= 1.5f; x += 0.001f) {
    const float y = sid::sclip(x);
    CHECK(y >= last - 1e-6f);
    last = y;
    CHECK(sid::sclip(-x) == doctest::Approx(-y).epsilon(1e-5));
  }
  CHECK(sid::sclip(1.5f) == doctest::Approx(1.0f).epsilon(1e-5));
  CHECK(sid::sclip(0.001f) == doctest::Approx(0.001f).epsilon(1e-4));
  for (float x : {1.5f, 2.0f, 10.0f, 1e6f, 1e30f}) {
    CHECK(sid::sclip(x) <= 1.0001f);
    CHECK(sid::sclip(-x) >= -1.0001f);
  }
  // the 6581 clips earlier than the 8580: the same loud input is flatter through it
  FilterRig a, b;
  a.setup(0, 7, 100.0f, 0);
  b.setup(1, 7, 100.0f, 0);
  a.set(SID_PARAM_DRIVE, 60.0f);
  b.set(SID_PARAM_DRIVE, 60.0f);
  std::vector<float> ya(2000), yb(2000);
  for (std::size_t i = 0; i < ya.size(); ++i) {
    ya[i] = yb[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 200.0 * static_cast<double>(i) / kRate));
  }
  sid::filter_run(a.v, ya.data(), 2000);
  sid::filter_run(b.v, yb.data(), 2000);
  float pa = 0.0f, pb = 0.0f;
  for (std::size_t i = 500; i < 2000; ++i) {
    pa = std::max(pa, std::fabs(ya[i]));
    pb = std::max(pb, std::fabs(yb[i]));
  }
  CHECK(pa <= 1.0001f);
  CHECK(pb > pa);
}

TEST_CASE("sid filter: Mode OFF skips the filter: no sample changes whatever the other filter values are, no host maths runs, and the state rests") {
  Fixture a, b;
  NEED(a.ok);
  NEED(b.ok);
  b.inst.set(SID_PARAM_CUTOFF, 5.0f);
  b.inst.set(SID_PARAM_RESONANCE, 15.0f);
  b.inst.set(SID_PARAM_KEY_TRACK, 100.0f);
  b.inst.set(SID_PARAM_ENV_AMOUNT, -100.0f);
  b.inst.set(SID_PARAM_DRIVE, 100.0f);
  for (std::uint32_t wave : {kSaw, kPulse, kNoise, kPulseSawTri}) {
    a.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    b.inst.set(SID_PARAM_WAVE, static_cast<float>(wave));
    a.inst.on(60);
    b.inst.on(60);
    CHECK(a.inst.render(3000) == b.inst.render(3000));
  }
  // and a filter that is on changes the sound
  b.inst.set(SID_PARAM_MODE, 1.0f);
  b.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  a.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  a.inst.on(60);
  b.inst.on(60);
  CHECK(a.inst.render(3000) != b.inst.render(3000));
  // the host's sin runs for the filter's coefficients only: none with Mode OFF, and with a steady note none after the first period
  o3m_host host = make_host();
  host.sin_f = counting_sin;
  host.exp2_f = counting_exp2;
  sid::Voice v{};
  sid::voice_init(v, &host);
  sid::voice_set_param(v, SID_PARAM_ATTACK, 0.0f);
  sid::voice_set_param(v, SID_PARAM_SUSTAIN, 15.0f);
  sid::voice_set_param(v, SID_PARAM_WAVE, static_cast<float>(kSaw));
  float out[64];
  g_sin_calls = 0;
  sid::voice_note_on(v, 60, 100);
  for (int i = 0; i < 50; ++i) {
    sid::voice_render(v, out, 64);
  }
  CHECK(g_sin_calls == 0);
  sid::voice_set_param(v, SID_PARAM_MODE, 1.0f);
  sid::voice_set_param(v, SID_PARAM_ENV_AMOUNT, 0.0f);
  sid::voice_render(v, out, 64);
  sid::voice_render(v, out, 64);
  const int after_first = g_sin_calls;
  CHECK(after_first >= 1);
  const int exp2_before = g_exp2_calls;
  for (int i = 0; i < 100; ++i) {
    sid::voice_render(v, out, 64);  // a steady note: no vibrato, no sweep, no envelope movement, no Key Track: the cutoff does not move
  }
  CHECK(g_sin_calls == after_first);
  CHECK(g_exp2_calls - exp2_before == 100 * 64 / 32);  // only the pitch's exp2, once a control period (two periods a block)
  // switching it off puts the state to rest
  sid::voice_set_param(v, SID_PARAM_MODE, 0.0f);
  CHECK((v.fs1 == 0.0f && v.fs2 == 0.0f));
}

TEST_CASE("sid filter: Key Track moves the cutoff an octave an octave of the key, Env Amount by the envelope, Cutoff by its curve; a modulated Cutoff only stores a value") {
  FilterRig r;
  r.setup(1, 1, 50.0f, 0);
  const float e60 = r.v.fe_last;
  r.set(SID_PARAM_KEY_TRACK, 100.0f);
  sid::filter_control(r.v, 72.0f);
  CHECK(r.v.fe_last - e60 == doctest::Approx(1.0f).epsilon(1e-4));
  sid::filter_control(r.v, 48.0f);
  CHECK(r.v.fe_last - e60 == doctest::Approx(-1.0f).epsilon(1e-4));
  r.set(SID_PARAM_KEY_TRACK, 50.0f);
  sid::filter_control(r.v, 72.0f);
  CHECK(r.v.fe_last - e60 == doctest::Approx(0.5f).epsilon(1e-3));
  r.set(SID_PARAM_KEY_TRACK, 0.0f);
  r.set(SID_PARAM_ENV_AMOUNT, 50.0f);
  r.v.env = 255u << sid::kFrac;
  sid::filter_control(r.v, 60.0f);
  CHECK(std::exp2(r.v.fe_last) == doctest::Approx(curve_hz(1, 100.0)).epsilon(1e-3));  // 50 % + 50 % x full level = 100 %
  r.set(SID_PARAM_ENV_AMOUNT, -100.0f);
  sid::filter_control(r.v, 60.0f);
  CHECK(std::exp2(r.v.fe_last) == doctest::Approx(curve_hz(1, 0.0)).epsilon(1e-3));  // below 0 % holds at the lowest
  // the cutoff is held to the allowed range at both ends
  r.set(SID_PARAM_ENV_AMOUNT, 0.0f);
  r.set(SID_PARAM_KEY_TRACK, 100.0f);
  sid::filter_control(r.v, 127.0f);
  CHECK(std::exp2(r.v.fe_last) <= 0.45f * kRate * 1.001f);
  sid::filter_control(r.v, -200.0f);
  CHECK(std::exp2(r.v.fe_last) >= 9.99f);
  // the cutoff's own curve: 8580 exponential, 6581 S-shaped, the 6581's lower and higher at the ends
  for (std::uint32_t model = 0; model < 2; ++model) {
    for (float cut : {0.0f, 25.0f, 50.0f, 75.0f, 100.0f}) {
      r.setup(model, 1, cut, 0);
      CHECK(std::exp2(r.v.fe_last) == doctest::Approx(curve_hz(model, cut)).epsilon(1e-3));
    }
  }
}

TEST_CASE("sid filter: a resonant filter rings on after the note: the voice stays busy for the tail and is idle at its end") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  f.inst.set(SID_PARAM_MODEL, 1.0f);
  f.inst.set(SID_PARAM_MODE, 1.0f);
  f.inst.set(SID_PARAM_CUTOFF, 35.0f);
  f.inst.set(SID_PARAM_RESONANCE, 15.0f);
  f.inst.on(36);
  f.inst.render(4000);
  f.inst.off(36);
  f.inst.render(512);  // the release is over (6 ms): the note is, the filter rings
  const std::vector<float> tail = f.inst.render(512);
  CHECK(f.inst.busy == 1u);
  CHECK(rms_of(tail) > 1e-5);
  bool idle = false;
  for (int i = 0; i < 400 && !idle; ++i) {
    for (float x : f.inst.render(512)) {
      CHECK(std::isfinite(x));
    }
    idle = f.inst.busy == 0u;
  }
  CHECK(idle);
  CHECK(rms_of(f.inst.render(512)) == 0.0);
  // with the filter off a finished note is idle at once
  Fixture g;
  NEED(g.ok);
  g.inst.on(36);
  g.inst.render(2000);
  g.inst.off(36);
  g.inst.render(1024);
  CHECK(g.inst.busy == 0u);
}

TEST_CASE("sid: velocity scales the level by Vel Sens, and Volume is in dB") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSaw));
  const auto peak = [&](std::uint32_t vel) {
    f.inst.on(45, vel);
    float p = 0.0f;
    for (float x : f.inst.render(2000)) {
      p = std::max(p, std::fabs(x));
    }
    f.inst.off(45);
    f.inst.render(512);
    return p;
  };
  const float full = peak(127);
  CHECK(full == doctest::Approx(1.0f).epsilon(0.05));
  f.inst.set(SID_PARAM_VEL_SENS, 100.0f);
  CHECK(peak(64) == doctest::Approx(full * 64.0f / 127.0f).epsilon(0.02));
  f.inst.set(SID_PARAM_VEL_SENS, 50.0f);
  CHECK(peak(64) == doctest::Approx(full * (1.0f - 0.5f * (1.0f - 64.0f / 127.0f))).epsilon(0.02));
  f.inst.set(SID_PARAM_VEL_SENS, 0.0f);
  f.inst.set(SID_PARAM_VOLUME, -6.0206f);
  CHECK(peak(127) == doctest::Approx(full * 0.5f).epsilon(0.01));
}

TEST_CASE("sid: the arpeggio steps through 0, Arp 1, Arp 2, the sweep glides, and the vibrato swings the pitch") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_WAVE, static_cast<float>(kPulse));
  f.inst.set(SID_PARAM_ARP_1, 4.0f);
  f.inst.set(SID_PARAM_ARP_2, 7.0f);
  f.inst.set(SID_PARAM_ARP_SPEED, 10.0f);  // 100 ms a step
  f.inst.on(57);
  f.inst.render(256);
  const double want[3] = {hz_of_note(57), hz_of_note(61), hz_of_note(64)};
  for (std::uint32_t step = 0; step < 6; ++step) {
    const std::vector<float> s = f.inst.render(2205);  // 50 ms in the middle of each 100 ms step
    f.inst.render(2205);
    CAPTURE(step);
    CHECK(frequency(s.data(), s.size()) == doctest::Approx(want[(step + 0u) % 3u]).epsilon(0.03));
  }
  // the sweep: a note that starts 12 semitones up glides down over 500 ms and repeats at every note
  Fixture g;
  NEED(g.ok);
  g.inst.set(SID_PARAM_SWEEP, 12.0f);
  g.inst.set(SID_PARAM_SWEEP_TIME, 500.0f);
  g.inst.on(57);
  const std::vector<float> early = g.inst.render(2205);
  g.inst.render(22050);
  const std::vector<float> late = g.inst.render(4410);
  CHECK(frequency(early.data(), early.size()) == doctest::Approx(hz_of_note(57 + 11)).epsilon(0.06));
  CHECK(frequency(late.data(), late.size()) == doctest::Approx(hz_of_note(57)).epsilon(5e-3));
  // the vibrato: +-100 cents at 5 Hz moves the pitch both ways
  Fixture h;
  NEED(h.ok);
  h.inst.set(SID_PARAM_VIB_DEPTH, 100.0f);
  h.inst.set(SID_PARAM_VIB_RATE, 5.0f);
  h.inst.on(57);
  h.inst.render(512);
  double fmin = 1e9, fmax = 0.0;
  for (std::uint32_t k = 0; k < 40; ++k) {
    const std::vector<float> s = h.inst.render(1102);  // 25 ms windows over a second
    const double fr = frequency(s.data(), s.size());
    fmin = std::min(fmin, fr);
    fmax = std::max(fmax, fr);
  }
  CHECK(fmax > hz_of_note(57.0 + 0.7));
  CHECK(fmin < hz_of_note(57.0 - 0.7));
  CHECK(fmax < hz_of_note(57.0 + 1.2));
}

TEST_CASE("sid: the voice is the same whatever the block sizes it is rendered in, sync, ring, PWM, arpeggio, sweep, vibrato and the filter included") {
  Lib lib;
  NEED(lib.open(O3M_SID_HOST_SO));
  for (std::uint32_t wave : {kSaw, kTri, kPulse, kNoise, kPulseSaw}) {
    std::vector<float> ref;
    for (std::uint32_t block : {64u, 1u, 7u, 128u, 33u}) {
      Instance in;
      NEED(in.start(*lib.entry));
      in.set(SID_PARAM_WAVE, static_cast<float>(wave));
      in.set(SID_PARAM_SYNC, 1.0f);
      in.set(SID_PARAM_RING, 1.0f);
      in.set(SID_PARAM_PWM_DEPTH, 60.0f);
      in.set(SID_PARAM_PWM_RATE, 7.0f);
      in.set(SID_PARAM_MOD_PITCH, -5.0f);
      in.set(SID_PARAM_ARP_1, 5.0f);
      in.set(SID_PARAM_MODE, 5.0f);  // the filter on, its cutoff following the key, the envelope and the vibrato
      in.set(SID_PARAM_CUTOFF, 40.0f);
      in.set(SID_PARAM_RESONANCE, 9.0f);
      in.set(SID_PARAM_KEY_TRACK, 60.0f);
      in.set(SID_PARAM_ENV_AMOUNT, 70.0f);
      in.set(SID_PARAM_DRIVE, 30.0f);
      in.set(SID_PARAM_VIB_DEPTH, 40.0f);
      in.set(SID_PARAM_SWEEP, 5.0f);
      in.set(SID_PARAM_ATTACK, 7.0f);
      in.on(55);
      std::vector<float> out = in.render(6000, block);
      in.off(55);
      const std::vector<float> tail = in.render(2000, block);
      out.insert(out.end(), tail.begin(), tail.end());
      if (ref.empty()) {
        ref = out;
      }
      CAPTURE(wave);
      CAPTURE(block);
      CHECK(out == ref);  // bit for bit: the control tick counts samples, not calls
    }
  }
}

TEST_CASE("sid: no parameter value, however hostile, makes the voice produce NaN or runaway samples") {
  Lib lib;
  NEED(lib.open(O3M_SID_HOST_SO));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float hostile[] = {nan, 1e30f, -1e30f, 0.0f, -0.0f, 1e-30f, 7.5f, -7.5f};
  for (float h : hostile) {
    Instance in;
    NEED(in.start(*lib.entry));
    CAPTURE(h);
    for (std::uint32_t id = 0; id < in.m->param_count; ++id) {
      in.set(id, h);
    }
    in.set(SID_PARAM_VOLUME, 6.0f);
    in.on(200, 1000);  // note and velocity out of range
    for (std::uint32_t block = 0; block < 40; ++block) {
      for (float x : in.render(64)) {
        CHECK(std::isfinite(x));
        CHECK(std::fabs(x) < 12.0f);
      }
    }
    in.set(1000, 5.0f);  // an id out of range is ignored
    in.off(0);
    for (float x : in.render(5000)) {
      CHECK(std::isfinite(x));
    }
  }
  // every wave with sync and ring at the extremes of the modulator
  for (std::uint32_t wave = 0; wave < 8; ++wave) {
    for (float mod : {-24.0f, 48.0f}) {
      Instance in;
      NEED(in.start(*lib.entry));
      in.set(SID_PARAM_WAVE, static_cast<float>(wave));
      in.set(SID_PARAM_SYNC, 1.0f);
      in.set(SID_PARAM_RING, 1.0f);
      in.set(SID_PARAM_MOD_PITCH, mod);
      in.set(SID_PARAM_PWM_DEPTH, 100.0f);
      in.set(SID_PARAM_PWM_RATE, 20.0f);
      in.set(SID_PARAM_VOLUME, 6.0f);
      in.on(127);
      for (float x : in.render(8192)) {
        CHECK(std::isfinite(x));
        CHECK(std::fabs(x) < 12.0f);
      }
    }
  }
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

TEST_CASE("sid: mixed extremes, a different hostile value for each parameter, and a hostile host pitch, stay finite and bounded") {
  Lib lib;
  NEED(lib.open(O3M_SID_HOST_SO));
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const float extremes[] = {nan, inf, -inf, 1e30f, -1e30f, 0.0f, 1e-30f, -7.5f};
  std::uint32_t seed = 12345u;
  const auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 16;
  };
  for (std::uint32_t trial = 0; trial < 48; ++trial) {
    Instance in;
    NEED(in.start(*lib.entry));
    CAPTURE(trial);
    for (std::uint32_t id = 0; id < in.m->param_count; ++id) {
      const std::uint32_t pick = next() % 12u;
      const o3m_param& p = in.m->params[id];
      in.set(id, pick < 8u ? extremes[pick] : (pick < 10u ? p.min : p.max));  // a hostile value, or one end of its range
    }
    in.set(SID_PARAM_VOLUME, 6.0f);
    in.on(next() % 200u, next() % 300u);
    for (std::uint32_t block = 0; block < 30; ++block) {
      if (block == 5u) {
        in.m->pitch(in.state, next() % 3u == 0u ? nan : (next() % 2u == 0u ? 1e30f : -1e30f), next() % 2u == 0u ? inf : 1e30f);
      }
      if (block == 15u) {
        in.set(next() % in.m->param_count, extremes[next() % 8u]);
        in.on(next() % 128u);
      }
      for (float x : in.render(64)) {
        CHECK(std::isfinite(x));
        CHECK(std::fabs(x) < 12.0f);
      }
    }
    in.off(0);
    for (float x : in.render(4000)) {
      CHECK(std::isfinite(x));
    }
  }
}

TEST_CASE("sid: the combined waves have no DC through a pulse width sweep: whole cycles of the PWM LFO and a width that moves under the note") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_PULSE_WIDTH, 50.0f);
  for (std::uint32_t model = 0; model < 2; ++model) {
    for (std::uint32_t k = 0; k < 4; ++k) {
      f.inst.set(SID_PARAM_MODEL, static_cast<float>(model));
      f.inst.set(SID_PARAM_WAVE, static_cast<float>(kSawTri + k));
      f.inst.set(SID_PARAM_PWM_DEPTH, 90.0f);
      f.inst.set(SID_PARAM_PWM_RATE, 4.0f);  // 11025 samples a cycle
      CAPTURE(model);
      CAPTURE(k);
      f.inst.on(45);
      f.inst.render(2048);
      const std::vector<float> s = f.inst.render(4u * 11025u);
      // a mean over windows of 4009 samples (ten periods of A2): the DC of each stays small whatever the width is at that moment
      double worst = 0.0;
      for (std::size_t at = 0; at + 4009u <= s.size(); at += 4009u) {
        const std::vector<float> w(s.begin() + static_cast<std::ptrdiff_t>(at), s.begin() + static_cast<std::ptrdiff_t>(at + 4009u));
        worst = std::max(worst, std::fabs(mean_of(w)));
      }
      CHECK(worst < 0.08);
      CHECK(std::fabs(mean_of(s)) < 0.02);
      f.inst.off(45);
      f.inst.render(256);
    }
  }
}

TEST_CASE("sid: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part fits the slab") {
  const Tmod t = load_tmod(O3M_SID_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_INSTRUMENT);
  CHECK(t.h.abi_minor == 8u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == SID_PARAM_COUNT);
  CHECK(t.h.view_count == SID_VIEW_COUNT);
  CHECK(t.h.state_bytes <= SID_STATE_BYTES);
  CHECK(t.h.hot_size <= 4500u);  // the hot-part budget: the SRAM_L slab (14 KiB) is shared by every loaded module; the loops and the 1 KiB of combined-wave tables are the whole of it
  CHECK(t.h.image_size - t.h.hot_size <= 12288u);
  MESSAGE("sid.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}

// ---- views --------------------------------------------------------------------------------------------------------------------------------

namespace {

struct Views {
  Lib lib;
  ViewHost host;
  bool ok = false;
  float p[SID_PARAM_COUNT];
  Views() {
    ok = lib.open(O3M_SID_HOST_SO);
    if (ok) {
      for (std::uint32_t i = 0; i < SID_PARAM_COUNT; ++i) {
        p[i] = sid_params[i].def;
      }
    }
  }
  ViewStatus draw(std::uint32_t view, int w = 380, int h = 100, std::uint32_t max_words = kViewMaxWords) {
    return host.draw(*lib.entry, nullptr, 1, view, w, h, p, SID_PARAM_COUNT, max_words);
  }
};

}  // namespace

TEST_CASE("sid views: both draw inside the draw API's budgets for every wave, model, width and corner of the envelope, and the two together fit one page") {
  Views v;
  NEED(v.ok);
  CHECK(v.lib.entry->view_count == 3u);
  NEED(v.lib.entry->ui != nullptr);
  std::uint32_t worst_wave = 0, worst_both = 0;
  for (float wave = 0.0f; wave <= 7.0f; wave += 1.0f) {
    for (float model : {0.0f, 1.0f}) {
      for (float pw : {1.0f, 50.0f, 99.0f}) {
        v.p[SID_PARAM_WAVE] = wave;
        v.p[SID_PARAM_MODEL] = model;
        v.p[SID_PARAM_PULSE_WIDTH] = pw;
        CAPTURE(wave);
        CAPTURE(model);
        CAPTURE(pw);
        NEED(v.draw(SID_VIEW_WAVE) == ViewStatus::Ok);
        worst_wave = std::max(worst_wave, v.host.words());
      }
    }
  }
  for (float a : {0.0f, 8.0f, 15.0f}) {
    for (float d : {0.0f, 8.0f, 15.0f}) {
      for (float sus : {0.0f, 8.0f, 15.0f}) {
        for (float r : {0.0f, 8.0f, 15.0f}) {
          v.p[SID_PARAM_ATTACK] = a;
          v.p[SID_PARAM_DECAY] = d;
          v.p[SID_PARAM_SUSTAIN] = sus;
          v.p[SID_PARAM_RELEASE] = r;
          CAPTURE(a);
          CAPTURE(d);
          CAPTURE(sus);
          CAPTURE(r);
          v.p[SID_PARAM_WAVE] = static_cast<float>(kNoise);
          NEED(v.draw(SID_VIEW_WAVE) == ViewStatus::Ok);
          const std::uint32_t w0 = v.host.words();
          NEED(v.draw(SID_VIEW_ENVELOPE) == ViewStatus::Ok);
          worst_both = std::max(worst_both, w0 + v.host.words());
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
  MESSAGE("sid views: the wave view at most " << worst_wave << " estimated words, wave + envelope at most " << worst_both);
  CHECK(worst_both <= 480u);  // (with the filter view's 215 that is under the 700)
  // out-of-range values (a stored blob can hold anything finite) are held to the module's ranges; NaN too; too few values: it draws nothing; the smallest canvas
  v.p[SID_PARAM_ATTACK] = -5.0f;
  v.p[SID_PARAM_SUSTAIN] = 1e9f;
  v.p[SID_PARAM_RELEASE] = std::numeric_limits<float>::quiet_NaN();
  v.p[SID_PARAM_WAVE] = 99.0f;
  v.p[SID_PARAM_MODEL] = -3.0f;
  CHECK(v.draw(SID_VIEW_WAVE, 16, 16) == ViewStatus::Ok);
  CHECK(v.draw(SID_VIEW_ENVELOPE, 16, 16) == ViewStatus::Ok);
  CHECK(v.draw(SID_VIEW_ENVELOPE, 800, 480) == ViewStatus::Ok);
  CHECK(v.host.draw(*v.lib.entry, nullptr, 1, SID_VIEW_WAVE, 380, 100, v.p, 3) == ViewStatus::Ok);
  Drawing none;
  v.host.replay(none);
  CHECK((none.polys.empty() && none.curves.empty() && none.texts.empty()));
}

TEST_CASE("sid views: the wave view draws the saw, the triangle, the pulse at its width, the noise and the combined tables as they are, and names them") {
  Views v;
  NEED(v.ok);
  const int w = 380, h = 100;
  const int left = 6, right = w - 6, top = 16, bottom = h - 6;
  const double period = (right - left) / 2;
  const char* names[8] = {"SAW", "TRIANGLE", "PULSE", "NOISE", "SAW+TRI", "PULSE+SAW", "PULSE+TRI", "P+SAW+TRI"};
  for (std::uint32_t wave = 0; wave < 8; ++wave) {
    v.p[SID_PARAM_WAVE] = static_cast<float>(wave);
    CAPTURE(wave);
    NEED(v.draw(SID_VIEW_WAVE, w, h) == ViewStatus::Ok);
    Drawing dr;
    v.host.replay(dr);
    NEED(dr.texts.size() == 1u);
    CHECK(dr.texts[0].s == names[wave]);
    NEED(dr.polys.size() == 1u);
    CHECK(dr.polys[0].color == O3M_COLOR_INSTR);
    NEED(dr.rects.size() == 1u);  // the zero line across the middle
    CHECK(dr.rects[0].y == (top + bottom) / 2);
    CHECK(dr.polys[0].xy.size() <= 2u * 97u);
  }
  // the pulse at 10, 50 and 90 %: the first high stretch is the width x one period wide, at the top
  v.p[SID_PARAM_WAVE] = static_cast<float>(kPulse);
  for (float pw : {10.0f, 50.0f, 90.0f}) {
    v.p[SID_PARAM_PULSE_WIDTH] = pw;
    CAPTURE(pw);
    NEED(v.draw(SID_VIEW_WAVE, w, h) == ViewStatus::Ok);
    Drawing dr;
    v.host.replay(dr);
    NEED(dr.polys.size() == 1u);
    const std::vector<std::int16_t>& xy = dr.polys[0].xy;
    NEED(xy.size() == 18u);
    CHECK(xy[3] == top);
    CHECK(xy[5] == top);
    CHECK(xy[7] == bottom);
    CHECK(std::fabs(static_cast<double>(xy[4] - xy[2]) - static_cast<double>(pw) / 100.0 * period) <= 1.0);  // (whole pixels)
    CHECK(xy[16] == right);
  }
  // the saw: up over a period, down, up; the triangle: five corners
  v.p[SID_PARAM_WAVE] = static_cast<float>(kSaw);
  NEED(v.draw(SID_VIEW_WAVE, w, h) == ViewStatus::Ok);
  Drawing saw;
  v.host.replay(saw);
  NEED(saw.polys.size() == 1u);
  CHECK(saw.polys[0].xy.size() == 8u);
  CHECK(saw.polys[0].xy[3] == top);
  CHECK(saw.polys[0].xy[5] == bottom);
  v.p[SID_PARAM_WAVE] = static_cast<float>(kTri);
  NEED(v.draw(SID_VIEW_WAVE, w, h) == ViewStatus::Ok);
  Drawing tri;
  v.host.replay(tri);
  NEED(tri.polys.size() == 1u);
  CHECK(tri.polys[0].xy.size() == 10u);
  // the noise: the register's own steps from the seed, the 8-bit output as the height
  v.p[SID_PARAM_WAVE] = static_cast<float>(kNoise);
  NEED(v.draw(SID_VIEW_WAVE, w, h) == ViewStatus::Ok);
  Drawing nz;
  v.host.replay(nz);
  NEED(nz.polys.size() == 1u);
  NEED(nz.polys[0].xy.size() == 2u * 96u);
  std::uint32_t r = sid::kLfsrSeed;
  for (std::uint32_t step = 0; step < 48; ++step) {
    r = sid::lfsr_step(r);
    CHECK(nz.polys[0].xy[4 * step + 1] == bottom - static_cast<int>(sid::noise_byte(r)) * (bottom - top) / 255);
  }
  // the combined waves: the table's own values, the two models differ, the gate is shown
  std::vector<std::int16_t> shapes[2];
  for (std::uint32_t model = 0; model < 2; ++model) {
    v.p[SID_PARAM_MODEL] = static_cast<float>(model);
    v.p[SID_PARAM_WAVE] = static_cast<float>(kSawTri);
    NEED(v.draw(SID_VIEW_WAVE, w, h) == ViewStatus::Ok);
    Drawing c;
    v.host.replay(c);
    NEED(c.polys.size() == 1u);
    shapes[model] = c.polys[0].xy;
    NEED(shapes[model].size() == 2u * 97u);
    for (std::uint32_t k = 0; k < 97; ++k) {
      const float ph2 = 2.0f * static_cast<float>(k) / 96.0f;
      const float ph = ph2 >= 1.0f ? ph2 - 1.0f : ph2;
      const std::uint32_t idx = static_cast<std::uint32_t>(ph * 127.0f + 0.5f);
      CHECK(shapes[model][2u * k + 1u] == bottom - static_cast<int>(sid::kTables.t[model * 4u][idx]) * (bottom - top) / 255);
    }
  }
  CHECK(shapes[0] != shapes[1]);
  v.p[SID_PARAM_WAVE] = static_cast<float>(kPulseSaw);
  v.p[SID_PARAM_PULSE_WIDTH] = 25.0f;
  NEED(v.draw(SID_VIEW_WAVE, w, h) == ViewStatus::Ok);
  Drawing gated;
  v.host.replay(gated);
  NEED(gated.polys.size() == 1u);
  std::size_t at_bottom = 0;
  for (std::size_t k = 0; k < 97; ++k) {
    at_bottom += gated.polys[0].xy[2 * k + 1] == bottom ? 1u : 0u;
  }
  CHECK(at_bottom > 97u / 2u);  // three quarters of each period is the pulse's low
}

TEST_CASE("sid views: the envelope curve is the C64 table's times in order, the fall to the sustain and the release from it") {
  Views v;
  NEED(v.ok);
  v.p[SID_PARAM_ATTACK] = 9.0f;   // 250 ms
  v.p[SID_PARAM_DECAY] = 9.0f;    // 750 ms for a full fall
  v.p[SID_PARAM_SUSTAIN] = 8.0f;  // 8 / 15 of the top
  v.p[SID_PARAM_RELEASE] = 10.0f; // 1.5 s for a full fall
  NEED(v.draw(SID_VIEW_ENVELOPE) == ViewStatus::Ok);
  Drawing dr;
  v.host.replay(dr);
  NEED(dr.curves.size() == 1u);
  const CurveCall& c = dr.curves[0];
  const std::vector<float>& y = dr.curve_y[0];
  NEED(y.size() == 64u);
  const double sus = 8.0 / 15.0;
  const double ta = 250.0, td = 750.0 * (1.0 - sus), tr = 1500.0 * sus;
  const double hold = std::min(1000.0, std::max(50.0, 0.25 * (ta + td + tr)));
  const double total = ta + td + hold + tr;
  CHECK(c.x_hi == doctest::Approx(static_cast<float>(total * 0.001)).epsilon(1e-4));
  CHECK((c.x_lo == 0.0f && c.y_lo == 0.0f && c.y_hi == 1.0f));
  CHECK((c.flags & O3M_CURVE_AXES) != 0u);
  CHECK(y.front() == 0.0f);
  CHECK(y.back() == doctest::Approx(0.0f).epsilon(1e-4).scale(1.0));
  for (std::size_t i = 0; i < y.size(); ++i) {
    const double t = total * static_cast<double>(i) / 63.0;
    double want;
    if (t < ta) {
      want = t / ta;
    } else if (t < ta + td) {
      want = 1.0 - (1.0 - sus) * (t - ta) / td;
    } else if (t < ta + td + hold) {
      want = sus;
    } else {
      want = sus * (1.0 - (t - ta - td - hold) / tr);
    }
    CAPTURE(i);
    CHECK(static_cast<double>(y[i]) == doctest::Approx(want).epsilon(1e-3).scale(1.0));
  }
}

// ---- OS-MODPITCH (ABI 1.8): the host's Slide / Glide offset ---------------------------------------------------------------------------------------------------------------

TEST_CASE("sid views: the filter view draws the response of the mix at the cutoff, 97 points inside the budgets, flat for Mode OFF, for every mode, model, cutoff and resonance") {
  Views v;
  NEED(v.ok);
  std::uint32_t worst = 0, worst_wave_env = 0;
  {  // the other two views, at their most expensive
    Views w;
    NEED(w.ok);
    for (float wave : {0.0f, 2.0f, 3.0f, 5.0f}) {
      w.p[SID_PARAM_WAVE] = wave;
      NEED(w.draw(SID_VIEW_WAVE) == ViewStatus::Ok);
      const std::uint32_t a = w.host.words();
      NEED(w.draw(SID_VIEW_ENVELOPE) == ViewStatus::Ok);
      worst_wave_env = std::max(worst_wave_env, a + w.host.words());
    }
  }
  for (float mode = 0.0f; mode <= 7.0f; mode += 1.0f) {
    for (float model : {0.0f, 1.0f}) {
      for (float cut : {0.0f, 50.0f, 100.0f}) {
        for (float res : {0.0f, 15.0f}) {
          v.p[SID_PARAM_MODE] = mode;
          v.p[SID_PARAM_MODEL] = model;
          v.p[SID_PARAM_CUTOFF] = cut;
          v.p[SID_PARAM_RESONANCE] = res;
          CAPTURE(mode);
          CAPTURE(model);
          CAPTURE(cut);
          CAPTURE(res);
          NEED(v.draw(SID_VIEW_FILTER) == ViewStatus::Ok);
          worst = std::max(worst, v.host.words());
          Drawing dr;
          v.host.replay(dr);
          NEED(dr.curves.size() == 1u);
          const CurveCall& c = dr.curves[0];
          const std::vector<float>& y = dr.curve_y[0];
          NEED(y.size() == 97u);
          CHECK((c.x_lo == 20.0f && c.x_hi == 20000.0f && c.y_lo == -48.0f && c.y_hi == 24.0f));
          for (float d : y) {
            CHECK(std::isfinite(d));
            CHECK(d >= -48.0001f);
            CHECK(d <= 24.0001f);
            if (mode == 0.0f) {
              CHECK(d == 0.0f);
            }
          }
          // the middle point is at the cutoff: the low pass at Resonance 0 is -3 dB there, resonance 15 peaks (clipped at the plot's top), the notch is at the floor
          if (mode == 1.0f && res == 0.0f) {
            CHECK(y[48] == doctest::Approx(-3.01f).epsilon(0.03));
          }
          if (mode == 1.0f && res == 15.0f) {
            CHECK(y[48] > 20.0f);
          }
          if (mode == 4.0f) {
            CHECK(y[48] < -40.0f);
          }
          CHECK(dr.texts.size() == 1u);
        }
      }
    }
  }
  MESSAGE("sid views: the filter view at most " << worst << " estimated words");
  CHECK(worst <= 230u);
  CHECK(worst_wave_env + worst <= 690u);  // the three views of the page share 700 words (the TB-303's two use 688)
  // at the smallest canvas, and with hostile values
  v.p[SID_PARAM_MODE] = 7.0f;
  v.p[SID_PARAM_CUTOFF] = std::numeric_limits<float>::quiet_NaN();
  v.p[SID_PARAM_RESONANCE] = 1e30f;
  CHECK(v.draw(SID_VIEW_FILTER, 160, 80) == ViewStatus::Ok);
}

TEST_CASE("sid: pitch(0, 0) is bit for bit not calling it, pitch(12, 0) is an octave up, and a ramp's slope is followed (ABI 1.8)") {
  Fixture a, b;
  NEED(a.ok);
  NEED(b.ok);
  a.inst.set(SID_PARAM_VIB_DEPTH, 40.0f);  // something that moves, so that the comparison is of a busy voice
  b.inst.set(SID_PARAM_VIB_DEPTH, 40.0f);
  a.inst.on(57);
  b.inst.on(57);
  b.lib.entry->pitch(b.inst.state, 0.0f, 0.0f);
  const std::vector<float> sa = a.inst.render(8192);
  const std::vector<float> sb = b.inst.render(8192);
  CHECK(sa == sb);
  b.lib.entry->pitch(b.inst.state, 12.0f, 0.0f);
  b.inst.render(512);
  const std::vector<float> up = b.inst.render(44100);
  b.inst.set(SID_PARAM_VIB_DEPTH, 0.0f);
  b.inst.on(57);  // (a note puts the offset back to 0)
  b.inst.render(512);
  const std::vector<float> flat = b.inst.render(44100);
  CHECK(frequency(up.data(), up.size()) == doctest::Approx(2.0 * frequency(flat.data(), flat.size())).epsilon(2e-3));
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

TEST_CASE("sid: a note with LEGATO | RAMP over a sounding one keeps the envelope and moves only the pitch; a plain note restarts it (O3M_NOTE_RAMP, ABI 1.8)") {
  Fixture f;
  NEED(f.ok);
  f.inst.set(SID_PARAM_ATTACK, 0.0f);
  f.inst.set(SID_PARAM_DECAY, 9.0f);  // 750 ms
  f.inst.set(SID_PARAM_SUSTAIN, 0.0f);
  const auto peak = [](const std::vector<float>& s, std::size_t from, std::size_t to) {
    float p = 0.0f;
    for (std::size_t i = from; i < to && i < s.size(); ++i) {
      p = std::fabs(s[i]) > p ? std::fabs(s[i]) : p;
    }
    return p;
  };
  f.inst.on(57);
  f.inst.render(16537);  // 375 ms into a 750 ms decay: about half
  f.inst.on(64, 127, O3M_NOTE_LEGATO | O3M_NOTE_RAMP);
  const std::vector<float> kept = f.inst.render(512);
  const float level_kept = peak(kept, 64, 512);
  CHECK(level_kept < 0.55f);  // the envelope went on decaying from about half
  CHECK(level_kept > 0.35f);
  f.inst.on(57);
  f.inst.render(16537);
  f.inst.on(64, 127, 0);  // a plain note over the sounding one: the attack starts again from the level it is at
  const std::vector<float> again = f.inst.render(512);
  CHECK(level_kept < peak(again, 256, 512));
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
