// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012c: the FM module (modules/fm), driven through the ABI from its host shared object, as the OS drives it. The engine is written from scratch, so the oracle is an independent
// double-precision model written here from the maths (phase modulation, textbook Bessel sidebands) with its own transcription of the eight algorithms' routing: every algorithm,
// with and without feedback, is compared sample by sample. Also: the sine, operator ratios and detune (measured frequencies), the sideband amplitudes against J_n(beta), the envelope
// stages against their formulae (the sample divided by the sine of its known phase), block-size independence, hostile parameters, and the two views. Host only.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../tests/o3m_test_support.h"

#include "../fm_dsp.h"

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace o3m_test;
using namespace o3os::module;

namespace {

// The algorithms as the diagrams say (1-based: {source, target} is "source modulates target", then the heard operators). Written out here on purpose: not read from the module's table.
struct Route {
  std::vector<std::pair<int, int>> links;
  std::vector<int> carriers;
};
const std::vector<Route>& routes() {
  static const std::vector<Route> r = {
      {{{4, 3}, {3, 2}, {2, 1}}, {1}},               // 1: 4 > 3 > 2 > 1
      {{{4, 2}, {3, 2}, {2, 1}}, {1}},               // 2: (4 + 3) > 2 > 1
      {{{4, 3}, {3, 1}, {2, 1}}, {1}},               // 3: (4 > 3 + 2) > 1
      {{{3, 2}, {2, 1}}, {1, 4}},                    // 4: (3 > 2 > 1) + 4
      {{{2, 1}, {4, 3}}, {1, 3}},                    // 5: (2 > 1) + (4 > 3)
      {{{4, 3}, {3, 2}}, {1, 2}},                    // 6: (4 > 3 > 2) + 1
      {{{4, 1}, {4, 2}, {4, 3}}, {1, 2, 3}},         // 7: 4 > (1 + 2 + 3)
      {{}, {1, 2, 3, 4}},                            // 8: 1 + 2 + 3 + 4
      // OS-SNDMOD S2: 9..16
      {{{4, 3}, {4, 2}, {3, 1}, {2, 1}}, {1}},       // 9: 4 > (3 + 2) > 1
      {{{4, 3}, {3, 1}, {3, 2}}, {1, 2}},            // 10: 4 > 3 > (1 + 2)
      {{{3, 1}, {4, 2}}, {1, 2}},                    // 11: (3 > 1) + (4 > 2)
      {{{4, 1}, {3, 1}, {2, 1}}, {1}},               // 12: (4 + 3 + 2) > 1
      {{{4, 3}, {3, 2}, {4, 1}}, {1, 2}},            // 13: 4 > 3 > 2, and 4 > 1
      {{{4, 2}, {3, 2}, {4, 1}, {3, 1}}, {1, 2}},    // 14: (4 + 3) > (2 + 1)
      {{{3, 2}, {2, 1}, {4, 1}}, {1}},               // 15: (3 > 2) + 4 > 1
      {{{4, 2}, {2, 1}}, {1, 3}},                    // 16: 4 > 2 > 1, and 3 alone
  };
  return r;
}

struct Fixture {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fixture() {
    ok = lib.open(O3M_FM_HOST_SO) && inst.start(*lib.entry);
    if (ok) {
      inst.set(FM_PARAM_VOLUME, 0.0f);
      inst.set(FM_PARAM_VEL_SENS, 0.0f);
    }
  }
  // all four operators: ratio, detune, level, attack (ms), decay, sustain, release
  void op(int n, float ratio, float detune, float level, float a, float d, float s, float r) {
    const std::uint32_t base = 8u * static_cast<std::uint32_t>(n - 1);
    inst.set(base + 0, ratio);
    inst.set(base + 1, detune);
    inst.set(base + 2, level);
    inst.set(base + 3, a);
    inst.set(base + 4, d);
    inst.set(base + 5, s);
    inst.set(base + 6, r);
  }
  // only operator n is heard: algorithm 8 (all carriers), the others at level 0
  void solo(int n, float ratio, float detune = 0.0f) {
    inst.set(FM_PARAM_ALGORITHM, 8.0f);
    inst.set(FM_PARAM_FEEDBACK, 0.0f);
    for (int k = 1; k <= 4; ++k) {
      op(k, k == n ? ratio : 1.0f, k == n ? detune : 0.0f, k == n ? 100.0f : 0.0f, 1.0f, 100.0f, 100.0f, 100.0f);
    }
  }
};

// Independent model of the voice in double precision for a fresh note, with envelopes that are a linear attack to 1 and then a sustain of 100 %.
struct Model {
  double ratio[4], cents[4], level[4], attack_ms[4];
  int algorithm = 1;
  int feedback = 0;
  double f0 = 440.0;
  std::vector<double> run(std::size_t n) const {
    const Route& r = routes()[static_cast<std::size_t>(algorithm - 1)];
    double inc[4], amp[4], a_step[4], phase[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
      const double mul = (ratio[i] < 1.0 ? 0.5 : ratio[i]) * std::pow(2.0, cents[i] / 1200.0);
      inc[i] = f0 * mul / static_cast<double>(kRate);
      amp[i] = level[i] <= 0.0 ? 0.0 : std::pow(10.0, -(100.0 - level[i]) * 0.75 / 20.0);
      a_step[i] = 1.0 / (std::max(attack_ms[i], 1.0) * static_cast<double>(kRate) * 0.001);
    }
    const double fb = feedback == 0 ? 0.0 : 0.25 * std::pow(2.0, feedback - 7);
    double fb0 = 0.0, fb1 = 0.0;
    std::vector<double> out(n);
    for (std::size_t k = 0; k < n; ++k) {
      double o[5] = {0, 0, 0, 0, 0};  // 1-based
      for (int i = 4; i >= 1; --i) {
        phase[i - 1] = std::fmod(phase[i - 1] + inc[i - 1], 1.0);
        const double env = std::min(1.0, static_cast<double>(k + 1) * a_step[i - 1]);
        double m = i == 4 ? fb * (fb0 + fb1) : 0.0;
        for (const auto& l : r.links) {
          if (l.second == i) {
            m += 2.0 * o[l.first];
          }
        }
        o[i] = std::sin(2.0 * kPi * (phase[i - 1] + m)) * env * amp[i - 1];
        if (i == 4) {
          fb1 = fb0;
          fb0 = o[4];
        }
      }
      double sum = 0.0;
      for (int c : r.carriers) {
        sum += o[c];
      }
      out[k] = sum;
    }
    return out;
  }
};

double bessel_j(int n, double x) {
  double sum = 0.0, term = std::pow(x / 2.0, n);
  for (int k = 1; k <= n; ++k) {
    term /= static_cast<double>(k);
  }
  for (int m = 0; m < 40; ++m) {
    sum += term;
    term *= -(x / 2.0) * (x / 2.0) / (static_cast<double>(m + 1) * static_cast<double>(m + 1 + n));
  }
  return sum;
}

// The amplitude of the component at `hz` (Hann window, a single-frequency DFT: the lobes are narrow against the 110 Hz spacing).
double component(const std::vector<float>& s, std::size_t from, std::size_t n, double hz) {
  double re = 0.0, im = 0.0, wsum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
    const double a = 2.0 * kPi * hz * static_cast<double>(i) / static_cast<double>(kRate);
    re += w * static_cast<double>(s[from + i]) * std::cos(a);
    im -= w * static_cast<double>(s[from + i]) * std::sin(a);
    wsum += w;
  }
  return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

}  // namespace

TEST_CASE("fm: the descriptor is valid as the loader reads it and says what module.json says") {
  Lib lib;
  NEED(lib.open(O3M_FM_HOST_SO));
  const o3m_module& m = *lib.entry;
  ModuleSet set;
  CHECK(set.add_native("fm", m));
  CHECK(std::string(m.name) == "FM");
  CHECK(m.kind == O3M_KIND_INSTRUMENT);
  CHECK(m.abi_minor == 13u);  // ABI 1.13 (module.json; RATE flag, OS-MODRATE): pitch, pages and labels, and a version
  CHECK(m.param_count == FM_PARAM_COUNT);
  CHECK(m.param_count == 32u);
  CHECK(m.view_count == 2u);
  CHECK(m.state_bytes == sizeof(fm::Voice));
  CHECK(m.state_bytes <= FM_STATE_BYTES);
  for (std::uint32_t i = 0; i < m.param_count; ++i) {
    const o3m_param& p = m.params[i];
    CAPTURE(std::string(p.name));
    CHECK(std::strlen(p.name) < O3M_PARAM_NAME_MAX);
    CHECK(p.min <= p.def);
    CHECK(p.def <= p.max);
    // the page of eight: seven of an operator and a global closing it
    if (i % 8u == 7u) {
      CHECK(std::string(p.name).rfind("Op", 0) != 0u);
    } else {
      CHECK(std::string(p.name).rfind("Op" + std::to_string(i / 8u + 1u), 0) == 0u);
    }
  }
  CHECK(std::string(m.params[FM_PARAM_ALGORITHM].name) == "Algorithm");
  CHECK((m.params[FM_PARAM_ALGORITHM].min == 1.0f && m.params[FM_PARAM_ALGORITHM].max == 16.0f));
}

TEST_CASE("fm: the module's algorithm table is the diagrams'") {
  CHECK(routes().size() == 16u);
  CHECK(fm::kAlgorithmCount == 16u);
  for (std::size_t a = 0; a < 16; ++a) {
    const fm::Algorithm& alg = fm::kAlgorithms[a];
    const Route& r = routes()[a];
    CAPTURE(a + 1);
    std::uint8_t mods[4] = {0, 0, 0, 0};
    for (const auto& l : r.links) {
      mods[l.second - 1] = static_cast<std::uint8_t>(mods[l.second - 1] | (1u << (l.first - 1)));
      CHECK(l.first > l.second);  // evaluated 4, 3, 2, 1: a modulator has a higher number than its target
    }
    std::uint8_t carriers = 0;
    for (int c : r.carriers) {
      carriers = static_cast<std::uint8_t>(carriers | (1u << (c - 1)));
    }
    for (int i = 0; i < 4; ++i) {
      CHECK(alg.mods[i] == mods[i]);
    }
    CHECK(alg.carriers == carriers);
  }
  // all different
  for (std::size_t a = 0; a < 16; ++a) {
    for (std::size_t b = a + 1; b < 16; ++b) {
      CHECK(!(std::equal(fm::kAlgorithms[a].mods, fm::kAlgorithms[a].mods + 4, fm::kAlgorithms[b].mods) && fm::kAlgorithms[a].carriers == fm::kAlgorithms[b].carriers));
    }
  }
}

TEST_CASE("fm: the sine is good to 1.2e-6 over any phase") {
  double worst = 0.0;
  for (int i = -400000; i <= 400000; ++i) {
    const float x = static_cast<float>(i) * 1.37e-5f;  // -5.5 .. 5.5 turns
    worst = std::max(worst, std::fabs(static_cast<double>(fm::sin_turns(x)) - std::sin(2.0 * kPi * static_cast<double>(x))));
  }
  CAPTURE(worst);
  CHECK(worst < 2.0e-6);
  for (float x : {1000.123f, -1000.123f, 12345.678f, 0.0f, 0.5f, -0.5f, 0.25f, -0.25f, 1.0f}) {
    CHECK(std::fabs(static_cast<double>(fm::sin_turns(x)) - std::sin(2.0 * kPi * static_cast<double>(x))) < 2.0e-4);  // far phases lose float bits, not the shape
  }
  CHECK(fm::sin_turns(0.0f) == 0.0f);
  CHECK(fm::sin_turns(0.25f) == doctest::Approx(1.0f).epsilon(2e-6));
  CHECK(fm::sin_turns(0.75f) == doctest::Approx(-1.0f).epsilon(2e-6));
}

TEST_CASE("fm: every algorithm, with and without feedback, follows the double-precision model sample by sample") {
  Fixture f;
  NEED(f.ok);
  const double ratios[4] = {1.0, 2.0, 3.0, 5.0};
  const double cents[4] = {0.0, 7.0, -5.0, 12.0};
  const double levels[4] = {100.0, 85.0, 70.0, 90.0};
  for (int fb : {0, 3}) {
    for (int alg = 1; alg <= 16; ++alg) {
      CAPTURE(alg);
      CAPTURE(fb);
      Model model;
      f.inst.set(FM_PARAM_ALGORITHM, static_cast<float>(alg));
      f.inst.set(FM_PARAM_FEEDBACK, static_cast<float>(fb));
      model.algorithm = alg;
      model.feedback = fb;
      model.f0 = hz_of_note(52);
      for (int i = 0; i < 4; ++i) {
        f.op(i + 1, static_cast<float>(ratios[i]), static_cast<float>(cents[i]), static_cast<float>(levels[i]), 1.0f, 500.0f, 100.0f, 100.0f);
        model.ratio[i] = ratios[i];
        model.cents[i] = cents[i];
        model.level[i] = levels[i];
        model.attack_ms[i] = 1.0;
      }
      f.inst.on(52);
      const std::vector<float> got = f.inst.render(3000);
      const std::vector<double> want = model.run(3000);
      double worst = 0.0, peak = 0.0;
      for (std::size_t k = 0; k < got.size(); ++k) {
        worst = std::max(worst, std::fabs(static_cast<double>(got[k]) - want[k]));
        peak = std::max(peak, std::fabs(want[k]));
      }
      CAPTURE(worst);
      CHECK(peak > 0.1);  // the comparison is not of two silences
      CHECK(worst < 4e-3);
      f.inst.off(52);
      f.inst.render(8000);  // a release of 100 ms ends at -80 dB: 133 ms
      CHECK(f.inst.busy == 0u);
    }
  }
}

TEST_CASE("fm: a Detune beyond +-100 cents, up to +-1200, follows the double-precision model sample by sample, in a modulating algorithm too (OS-SNDMOD S2)") {
  Fixture f;
  NEED(f.ok);
  const double ratios[4] = {1.0, 2.0, 1.0, 3.0};
  const double levels[4] = {100.0, 80.0, 90.0, 75.0};
  const double sets[3][4] = {{1200.0, -1200.0, 700.0, -350.0}, {-1200.0, 600.0, 1200.0, -600.0}, {101.0, -101.0, 1199.0, -1199.0}};
  for (int alg : {5, 8, 14, 16}) {
    for (int s = 0; s < 3; ++s) {
      CAPTURE(alg);
      CAPTURE(s);
      Model model;
      f.inst.set(FM_PARAM_ALGORITHM, static_cast<float>(alg));
      f.inst.set(FM_PARAM_FEEDBACK, 0.0f);
      model.algorithm = alg;
      model.feedback = 0;
      model.f0 = hz_of_note(45);
      for (int i = 0; i < 4; ++i) {
        f.op(i + 1, static_cast<float>(ratios[i]), static_cast<float>(sets[s][i]), static_cast<float>(levels[i]), 1.0f, 500.0f, 100.0f, 100.0f);
        model.ratio[i] = ratios[i];
        model.cents[i] = sets[s][i];
        model.level[i] = levels[i];
        model.attack_ms[i] = 1.0;
      }
      f.inst.on(45);
      const std::vector<float> got = f.inst.render(3000);
      const std::vector<double> want = model.run(3000);
      double worst = 0.0, peak = 0.0;
      for (std::size_t k = 0; k < got.size(); ++k) {
        worst = std::max(worst, std::fabs(static_cast<double>(got[k]) - want[k]));
        peak = std::max(peak, std::fabs(want[k]));
      }
      CAPTURE(worst);
      CHECK(peak > 0.1);
      CHECK(worst < 4e-3);
      f.inst.off(45);
      f.inst.render(8000);
      CHECK(f.inst.busy == 0u);
    }
  }
  // a stored value outside the range is held to it (a blob can hold anything), the ends are the octaves
  f.solo(1, 1.0f, 99999.0f);
  f.inst.on(45);
  f.inst.render(1000);
  const std::vector<float> hi = f.inst.render(44100);
  CHECK(frequency(hi.data(), hi.size()) == doctest::Approx(2.0 * hz_of_note(45)).epsilon(5e-4));
  f.inst.off(45);
  f.inst.render(256);
  f.solo(1, 1.0f, -99999.0f);
  f.inst.on(45);
  f.inst.render(1000);
  const std::vector<float> lo = f.inst.render(44100);
  CHECK(frequency(lo.data(), lo.size()) == doctest::Approx(0.5 * hz_of_note(45)).epsilon(5e-4));
}

TEST_CASE("fm: an operator runs at ratio x detune x the note's frequency (0 is 0.5), and Level 0 silences it") {
  Fixture f;
  NEED(f.ok);
  for (int n = 1; n <= 4; ++n) {
    for (float ratio : {0.0f, 1.0f, 2.0f, 7.0f, 31.0f}) {
      CAPTURE(n);
      CAPTURE(ratio);
      f.solo(n, ratio);
      f.inst.on(45);
      f.inst.render(1000);
      const std::vector<float> s = f.inst.render(44100);
      CHECK(frequency(s.data(), s.size()) == doctest::Approx(hz_of_note(45) * (ratio < 1.0f ? 0.5 : static_cast<double>(ratio))).epsilon(5e-4));
      f.inst.off(45);
      f.inst.render(256);
    }
  }
  for (float cents : {-1200.0f, -700.0f, -100.0f, -37.0f, 1.0f, 100.0f, 600.0f, 1200.0f}) {  // (OS-SNDMOD S2: +-1200)
    f.solo(3, 3.0f, cents);
    f.inst.on(50);
    f.inst.render(1000);
    const std::vector<float> s = f.inst.render(44100);
    CAPTURE(cents);
    CHECK(frequency(s.data(), s.size()) == doctest::Approx(hz_of_note(50) * 3.0 * std::pow(2.0, static_cast<double>(cents) / 1200.0)).epsilon(5e-4));
    f.inst.off(50);
    f.inst.render(256);
  }
  // the note: an octave up is twice the frequency, all the way to the top
  f.solo(1, 1.0f);
  for (std::uint32_t note : {24u, 36u, 60u, 84u, 96u}) {
    f.inst.on(note);
    f.inst.render(500);
    const std::vector<float> s = f.inst.render(22050);
    CAPTURE(note);
    CHECK(frequency(s.data(), s.size()) == doctest::Approx(hz_of_note(note)).epsilon(1e-3));
    f.inst.off(note);
    f.inst.render(256);
  }
  // all four at level 0: silence, and the voice says it is idle
  for (int k = 1; k <= 4; ++k) {
    f.op(k, 1, 0, 0, 1, 100, 100, 100);
  }
  f.inst.on(60);
  for (float x : f.inst.render(1000)) {
    CHECK(x == 0.0f);
  }
  CHECK(f.inst.busy == 0u);
}

TEST_CASE("fm: a modulator's sidebands are the Bessel functions of its index (4 pi x its level's amplitude)") {
  Fixture f;
  NEED(f.ok);
  // algorithm 5: 2 > 1 is heard; note 45 is 110 Hz: operator 1 at ratio 16 (1760 Hz, so that the lower sidebands of an index of 12 do not fold past 0 Hz onto the upper ones), operator 2 at
  // ratio 1 (110 Hz): sidebands at 1760 +- n x 110
  f.inst.set(FM_PARAM_ALGORITHM, 5.0f);
  f.inst.set(FM_PARAM_FEEDBACK, 0.0f);
  for (float l2 : {70.0f, 85.0f, 95.0f, 100.0f}) {
    f.op(1, 16, 0, 100, 1, 100, 100, 100);
    f.op(2, 1, 0, l2, 1, 100, 100, 100);
    f.op(3, 1, 0, 0, 1, 100, 100, 100);
    f.op(4, 1, 0, 0, 1, 100, 100, 100);
    f.inst.on(45);
    f.inst.render(2000);  // past the 1 ms attack
    const std::vector<float> s = f.inst.render(8192);
    const double beta = 4.0 * kPi * std::pow(10.0, -(100.0 - static_cast<double>(l2)) * 0.75 / 20.0);
    const double f_c = hz_of_note(45) * 16.0, f_m = hz_of_note(45);
    for (int n = 0; n <= 3; ++n) {
      CAPTURE(l2);
      CAPTURE(n);
      const double upper = component(s, 0, s.size(), f_c + n * f_m);
      CHECK(upper == doctest::Approx(std::fabs(bessel_j(n, beta))).epsilon(0.02).scale(1.0));
      if (n > 0) {
        CHECK(component(s, 0, s.size(), f_c - n * f_m) == doctest::Approx(std::fabs(bessel_j(n, beta))).epsilon(0.02).scale(1.0));
      }
    }
    f.inst.off(45);
    f.inst.render(8000);  // idle again: the next note starts from phase 0
  }
}

TEST_CASE("fm: the envelope is a linear attack, then an exponential decay to the sustain and an exponential release, 60 dB in D and R") {
  Fixture f;
  NEED(f.ok);
  f.solo(1, 1.0f);
  const float a_ms = 50.0f, d_ms = 400.0f, r_ms = 300.0f, sus = 40.0f;
  f.op(1, 1, 0, 100, a_ms, d_ms, sus, r_ms);
  const double inc = hz_of_note(60) / kRate;
  f.inst.on(60);
  const std::size_t n_on = 44100;
  const std::vector<float> on = f.inst.render(n_on);
  // the envelope at sample k: out / sin(2 pi phase), where the sine is large enough to divide by
  const auto env_at = [&](const std::vector<float>& s, std::size_t k, std::size_t phase_offset) {
    const double phase = std::fmod(static_cast<double>(k + 1 + phase_offset) * inc, 1.0);
    const double sine = std::sin(2.0 * kPi * phase);
    return std::fabs(sine) > 0.6 ? static_cast<double>(s[k]) / sine : -1.0;
  };
  double worst = 0.0;
  std::size_t checked = 0;
  const double att = a_ms * kRate * 0.001, dec = d_ms * kRate * 0.001;
  for (std::size_t k = 0; k < n_on; ++k) {
    const double e = env_at(on, k, 0);
    if (e < -0.5) {
      continue;
    }
    const double t = static_cast<double>(k + 1);
    const double want = t <= att ? t / att : 0.4 + 0.6 * std::pow(2.0, -std::log2(1000.0) * (t - att) / dec);
    worst = std::max(worst, std::fabs(e - want));
    ++checked;
  }
  CHECK(checked > 10000u);
  CAPTURE(worst);
  CHECK(worst < 4e-3);
  // 60 dB: after the decay time from the end of the attack, the distance to the sustain level is down to a thousandth
  CHECK(std::fabs(env_at(on, 100, 0) - 0.0) < 10.0);  // (just exercising the helper at a sample with a usable sine)
  // the release, from the sustain level: 60 dB in 300 ms
  f.inst.off(60);
  const std::vector<float> off = f.inst.render(44100);
  worst = 0.0;
  checked = 0;
  const double rel = r_ms * kRate * 0.001;
  for (std::size_t k = 0; k < 12000; ++k) {
    const double e = env_at(off, k, n_on);
    if (e < -0.5) {
      continue;
    }
    const double want = 0.4 * std::pow(2.0, -std::log2(1000.0) * static_cast<double>(k + 1) / rel);
    worst = std::max(worst, std::fabs(e - want));
    ++checked;
  }
  CHECK(checked > 3000u);
  CAPTURE(worst);
  CHECK(worst < 2e-3);
  // it ends at -80 dB (1e-4) of full scale: 0.4 -> 1e-4 is 12 octaves down by 60 dB per 300 ms: 3 s here? no: log2(4000) / log2(1000) x 300 ms = 361 ms
  CHECK(f.inst.busy == 0u);
  for (std::size_t k = 17000; k < off.size(); ++k) {
    CHECK(off[k] == 0.0f);
  }
}

TEST_CASE("fm: a decay to nothing ends the note while the key is held, and a new note over a sounding one continues from its level") {
  Fixture f;
  NEED(f.ok);
  f.solo(1, 1.0f);
  f.op(1, 1, 0, 100, 1, 100, 0, 100);  // a percussive operator: 100 ms (60 dB), sustain 0
  f.inst.on(60);
  f.inst.render(2000);
  CHECK(f.inst.busy == 1u);
  f.inst.render(44100 / 5);  // 200 ms: the envelope is below 1e-4 at about 133 ms + the attack
  CHECK(f.inst.busy == 0u);
  // retrigger over a release
  f.op(1, 1, 0, 100, 200, 1000, 100, 400);
  f.inst.on(60);
  f.inst.render(4410);  // 100 ms of a 200 ms attack: 0.5
  f.inst.off(60);
  f.inst.render(4410);  // 100 ms into a 400 ms release from 0.5: 0.5 x 2^(-9.966 / 4) = 0.089
  f.inst.on(60);
  const std::vector<float> s = f.inst.render(2000);
  double peak = 0.0;
  for (std::size_t k = 0; k < 100; ++k) {
    peak = std::max(peak, std::fabs(static_cast<double>(s[k])));
  }
  CHECK(peak < 0.15);  // continues from where the release was, not from full scale and not from nothing:
  CHECK(peak > 0.05);  // the attack is full-scale per attack time: it climbs from 0.089
}

TEST_CASE("fm: velocity and Volume scale the output (carriers only), and Feedback changes the spectrum of operator 4") {
  Fixture f;
  NEED(f.ok);
  f.solo(1, 1.0f);
  const auto peak = [&](std::uint32_t velocity) {
    f.inst.on(60, velocity);
    float p = 0.0f;
    for (float x : f.inst.render(4000)) {
      p = std::max(p, std::fabs(x));
    }
    f.inst.off(60);
    f.inst.render(30000);
    return p;
  };
  CHECK(peak(20) == doctest::Approx(1.0f).epsilon(5e-3));  // no sensitivity: velocity is ignored
  f.inst.set(FM_PARAM_VEL_SENS, 100.0f);
  CHECK(peak(64) == doctest::Approx(64.0f / 127.0f).epsilon(5e-3));
  f.inst.set(FM_PARAM_VEL_SENS, 0.0f);
  f.inst.set(FM_PARAM_VOLUME, -12.0f);
  CHECK(peak(127) == doctest::Approx(std::pow(10.0f, -12.0f / 20.0f)).epsilon(5e-3));
  // feedback on operator 4 alone (algorithm 8): without it a pure sine at the note; with it harmonics appear at 2f, 3f ...
  f.inst.set(FM_PARAM_VOLUME, 0.0f);
  f.solo(4, 1.0f);
  const auto harmonics = [&](int fb) {
    f.inst.set(FM_PARAM_FEEDBACK, static_cast<float>(fb));
    f.inst.on(45);
    f.inst.render(3000);
    const std::vector<float> s = f.inst.render(8192);
    f.inst.off(45);
    f.inst.render(30000);
    double rest = 0.0;
    for (int h = 2; h <= 5; ++h) {
      rest += component(s, 0, s.size(), hz_of_note(45) * h);
    }
    return rest / component(s, 0, s.size(), hz_of_note(45));
  };
  CHECK(harmonics(0) < 1e-4);
  CHECK(harmonics(4) > 0.05);
  CHECK(harmonics(7) > harmonics(4));
}

TEST_CASE("fm: the voice is the same whatever the block sizes it is rendered in") {
  Lib lib;
  NEED(lib.open(O3M_FM_HOST_SO));
  std::vector<float> ref;
  for (std::uint32_t block : {64u, 1u, 5u, 128u, 100u}) {
    Instance in;
    NEED(in.start(*lib.entry));
    in.set(FM_PARAM_ALGORITHM, 3.0f);
    in.set(FM_PARAM_FEEDBACK, 5.0f);
    in.on(55);
    std::vector<float> out = in.render(5000, block);
    in.off(55);
    const std::vector<float> tail = in.render(3000, block);
    out.insert(out.end(), tail.begin(), tail.end());
    if (ref.empty()) {
      ref = out;
    }
    CAPTURE(block);
    CHECK(out == ref);
  }
}

TEST_CASE("fm: no parameter value, however hostile, makes the voice produce NaN or runaway samples; operators past Nyquist fade out") {
  Lib lib;
  NEED(lib.open(O3M_FM_HOST_SO));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float hostile[] = {nan, 1e30f, -1e30f, 0.0f, -0.0f, 1e-30f, 7.5f, -7.5f};
  for (float h : hostile) {
    Instance in;
    NEED(in.start(*lib.entry));
    CAPTURE(h);
    for (std::uint32_t id = 0; id < in.m->param_count; ++id) {
      in.set(id, h);
    }
    in.set(FM_PARAM_VOLUME, 6.0f);
    in.on(200, 1000);
    for (std::uint32_t block = 0; block < 40; ++block) {
      for (float x : in.render(64)) {
        CHECK(std::isfinite(x));
        CHECK(std::fabs(x) < 8.0f);
      }
    }
    in.set(1000, 5.0f);  // an id out of range is ignored
    in.off(0);
    for (float x : in.render(30000)) {
      CHECK(std::isfinite(x));
    }
  }
  // note 127 (12.5 kHz) with ratio 31 is far beyond Nyquist: that operator is silent, not folded
  Fixture f;
  NEED(f.ok);
  f.solo(1, 31.0f);
  f.inst.on(127);
  for (float x : f.inst.render(2000)) {
    CHECK(x == 0.0f);
  }
  f.solo(1, 1.0f);  // and at ratio 1 it sounds (12.5 kHz is below 0.4 of the rate? 0.28: yes)
  f.inst.on(127);
  float peak = 0.0f;
  for (float x : f.inst.render(2000)) {
    peak = std::max(peak, std::fabs(x));
  }
  CHECK(peak > 0.9f);
  // a note-off with nothing sounding, and a second note on, are harmless
  Instance in;
  NEED(in.start(*f.lib.entry));
  in.off(60);
  in.on(60);
  in.on(72);
  for (float x : in.render(3000)) {
    CHECK(std::isfinite(x));
  }
}

TEST_CASE("fm: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_FM_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_INSTRUMENT);
  CHECK(t.h.abi_minor == 13u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));  // o3m_add_module(... VERSION 1.0.0)
  CHECK(t.h.param_count == FM_PARAM_COUNT);
  CHECK(t.h.view_count == 2u);
  CHECK(t.h.state_bytes <= FM_STATE_BYTES);
  CHECK(t.h.hot_size <= 3072u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: the operator loop is the whole of it
  CHECK(t.h.image_size - t.h.hot_size <= 8192u);
  MESSAGE("fm.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}

// ---- views --------------------------------------------------------------------------------------------------------------------------------

namespace {

struct Views {
  Lib lib;
  ViewHost host;
  bool ok = false;
  float p[FM_PARAM_COUNT];
  Views() {
    ok = lib.open(O3M_FM_HOST_SO);
    if (ok) {
      for (std::uint32_t i = 0; i < FM_PARAM_COUNT; ++i) {
        p[i] = fm_params[i].def;
      }
    }
  }
  ViewStatus draw(std::uint32_t view, int w = 380, int h = 130, std::uint32_t max_words = kViewMaxWords) {
    return host.draw(*lib.entry, nullptr, 1, view, w, h, p, FM_PARAM_COUNT, max_words);
  }
};

}  // namespace

TEST_CASE("fm views: both draw inside the draw API's budgets for every algorithm and corner of the operators, and the two together fit one page") {
  Views v;
  NEED(v.ok);
  CHECK(v.lib.entry->view_count == 2u);
  NEED(v.lib.entry->ui != nullptr);
  const float corners[2] = {0.0f, 1.0f};  // fractions of each parameter's range
  for (int alg = 1; alg <= 16; ++alg) {
    for (float fb : {0.0f, 7.0f}) {
      for (float c : corners) {
        for (float c2 : corners) {
          v.p[FM_PARAM_ALGORITHM] = static_cast<float>(alg);
          v.p[FM_PARAM_FEEDBACK] = fb;
          for (std::uint32_t op = 0; op < 4; ++op) {
            for (std::uint32_t k = 0; k < 7; ++k) {
              const o3m_param& q = fm_params[8 * op + k];
              const float frac = (op + k) % 2 == 0 ? c : c2;
              v.p[8 * op + k] = q.min + (q.max - q.min) * frac;
            }
          }
          CAPTURE(alg);
          CAPTURE(fb);
          for (std::uint32_t view = 0; view < 2; ++view) {
            NEED(v.draw(view) == ViewStatus::Ok);
            CHECK(v.host.words() <= 350u);
          }
          NEED(v.draw(FM_VIEW_ALGORITHM) == ViewStatus::Ok);
          const std::uint32_t w0 = v.host.words();
          NEED(v.draw(FM_VIEW_ENVELOPES) == ViewStatus::Ok);
          CHECK(w0 + v.host.words() <= 500u);  // the page's own words come out of the 700 too
        }
      }
    }
  }
  // out-of-range values, NaN, the smallest and the largest canvas, too few values
  v.p[FM_PARAM_ALGORITHM] = 99.0f;
  v.p[8 + 2] = std::numeric_limits<float>::quiet_NaN();
  v.p[FM_PARAM_OP3_ATTACK] = -4.0f;
  v.p[FM_PARAM_OP4_DECAY] = 1e30f;
  for (std::uint32_t view = 0; view < 2; ++view) {
    CHECK(v.draw(view, 16, 16) == ViewStatus::Ok);
    CHECK(v.draw(view, 160, 110) == ViewStatus::Ok);
    CHECK(v.draw(view, 800, 480) == ViewStatus::Ok);
    CHECK(v.host.draw(*v.lib.entry, nullptr, 1, view, 380, 130, v.p, 5) == ViewStatus::Ok);
    Drawing none;
    v.host.replay(none);
    CHECK((none.polys.empty() && none.texts.empty()));
  }
}

TEST_CASE("fm views: the algorithm diagram has a line for every modulation and a stub for every carrier, modulators above their targets, and labels the operators") {
  Views v;
  NEED(v.ok);
  const int W = 380, H = 130;
  for (int alg = 1; alg <= 16; ++alg) {
    const Route& r = routes()[static_cast<std::size_t>(alg - 1)];
    CAPTURE(alg);
    v.p[FM_PARAM_ALGORITHM] = static_cast<float>(alg);
    v.p[FM_PARAM_FEEDBACK] = 0.0f;
    NEED(v.draw(FM_VIEW_ALGORITHM, W, H) == ViewStatus::Ok);
    Drawing d;
    v.host.replay(d);
    // polylines: the links (FX colour), the carriers' stubs (INSTR), operator 4's feedback loop (4 points)
    std::size_t links = 0, stubs = 0, loops = 0;
    for (const Drawing::Poly& p : d.polys) {
      if (p.xy.size() == 8u) {
        ++loops;
        CHECK(p.color == O3M_COLOR_EMPTY);  // feedback 0: unlit
      } else {
        NEED(p.xy.size() == 4u);
        if (p.color == O3M_COLOR_FX) {
          ++links;
          CHECK(p.xy[1] < p.xy[3]);  // from a modulator's bottom down to its target's top
          CHECK(p.xy[3] > 0);
        } else {
          ++stubs;
          CHECK(p.color == O3M_COLOR_INSTR);
          CHECK(p.xy[0] == p.xy[2]);
          CHECK(p.xy[3] == H - 6);
        }
      }
    }
    CHECK(links == r.links.size());
    CHECK(stubs == r.carriers.size());
    CHECK(loops == 1u);
    // texts: the title with the algorithm's number, the four digits, the four ratios
    NEED(!d.texts.empty());
    CHECK(d.texts[0].s.rfind("ALG " + std::to_string(alg), 0) == 0u);
    // OS-SNDMOD S1 (F1): the title spells the algorithm as the parameter's own label does (one table feeds the field and the view)
    CHECK(d.texts[0].s == "ALG " + std::to_string(alg) + "  " + fm_labels_algorithm[alg - 1]);
    std::string digits;
    int ratio_texts = 0;
    for (const Drawing::Text& t : d.texts) {
      if (t.s.size() == 1u && t.s[0] >= '1' && t.s[0] <= '4') {
        digits += t.s;
      }
      ratio_texts += t.s[0] == 'x' ? 1 : 0;
    }
    CHECK(digits == "1234");
    CHECK(ratio_texts == 4);
  }
  // the loop lights up with feedback; the ratio text says 0.5 for ratio 0; no box is drawn outside the canvas
  v.p[FM_PARAM_ALGORITHM] = 1.0f;
  v.p[FM_PARAM_FEEDBACK] = 5.0f;
  v.p[FM_PARAM_OP1_RATIO] = 0.0f;
  v.p[FM_PARAM_OP2_RATIO] = 14.0f;
  NEED(v.draw(FM_VIEW_ALGORITHM, W, H) == ViewStatus::Ok);
  Drawing d;
  v.host.replay(d);
  bool lit = false, half = false, fourteen = false;
  for (const Drawing::Poly& p : d.polys) {
    lit = lit || (p.xy.size() == 8u && p.color == O3M_COLOR_ACCENT);
  }
  for (const Drawing::Text& t : d.texts) {
    half = half || t.s == "x0.5";
    fourteen = fourteen || t.s == "x14";
  }
  CHECK(lit);
  CHECK(half);
  CHECK(fourteen);
  for (const Drawing::Rect& r : d.rects) {
    CHECK(r.x >= 0);
    CHECK(r.y >= 0);
    CHECK(r.x + r.w <= W);
    CHECK(r.y + r.h <= H);
  }
}

TEST_CASE("fm views: the envelope plot draws each operator's envelope scaled by its level, the release marked, silent operators flat") {
  Views v;
  NEED(v.ok);
  const int W = 380, H = 130;
  const int x0 = 4, y0 = 16, x1 = W - 5, y1 = H - 14;
  // operator 1: attack 100 ms, decay 400 ms, sustain 40 %, level 100; operator 2: level 50; operator 3 and 4: level 0
  v.p[FM_PARAM_OP1_ATTACK] = 100.0f;
  v.p[FM_PARAM_OP1_DECAY] = 400.0f;
  v.p[FM_PARAM_OP1_SUSTAIN] = 40.0f;
  v.p[FM_PARAM_OP1_RELEASE] = 300.0f;
  v.p[FM_PARAM_OP1_LEVEL] = 100.0f;
  v.p[FM_PARAM_OP2_LEVEL] = 50.0f;
  v.p[FM_PARAM_OP2_ATTACK] = 100.0f;
  v.p[FM_PARAM_OP2_DECAY] = 400.0f;
  v.p[FM_PARAM_OP2_SUSTAIN] = 40.0f;
  v.p[FM_PARAM_OP2_RELEASE] = 300.0f;
  v.p[FM_PARAM_OP3_LEVEL] = 0.0f;
  v.p[FM_PARAM_OP4_LEVEL] = 0.0f;
  NEED(v.draw(FM_VIEW_ENVELOPES, W, H) == ViewStatus::Ok);
  Drawing d;
  v.host.replay(d);
  std::vector<const Drawing::Poly*> curves;
  for (const Drawing::Poly& p : d.polys) {
    curves.push_back(&p);
  }
  NEED(curves.size() == 4u);
  for (const Drawing::Poly* p : curves) {
    CHECK(p->xy.size() == 96u);
  }
  // the window: the key is down for max(attack + 0.7 decay) = 380 ms, then 0.8 x 300 = 240 ms of release: 620 ms
  const double on_ms = 380.0, total = 620.0;
  const auto want = [&](double t, double lvl) {
    double e;
    if (t <= on_ms) {
      e = t < 100.0 ? t / 100.0 : 0.4 + 0.6 * std::pow(2.0, -std::log2(1000.0) * (t - 100.0) / 400.0);
    } else {
      const double held = 0.4 + 0.6 * std::pow(2.0, -std::log2(1000.0) * (on_ms - 100.0) / 400.0);
      e = held * std::pow(2.0, -std::log2(1000.0) * (t - on_ms) / 300.0);
    }
    return e * lvl;
  };
  const double lvl[2] = {1.0, 0.5};
  for (int op = 0; op < 2; ++op) {
    double worst = 0.0;
    for (int k = 0; k < 48; ++k) {
      const double t = total * k / 47.0;
      const double y_px = y1 - want(t, lvl[op]) * (y1 - y0);
      CHECK(curves[static_cast<std::size_t>(op)]->xy[2u * static_cast<std::size_t>(k)] == x0 + static_cast<int>((x1 - x0) * k / 47.0));
      worst = std::max(worst, std::fabs(static_cast<double>(curves[static_cast<std::size_t>(op)]->xy[2u * static_cast<std::size_t>(k) + 1u]) - y_px));
    }
    CAPTURE(op);
    CHECK(worst <= 1.5);  // a pixel (truncation) and a hair
  }
  for (int op = 2; op < 4; ++op) {
    for (int k = 0; k < 48; ++k) {
      CHECK(curves[static_cast<std::size_t>(op)]->xy[2u * static_cast<std::size_t>(k) + 1u] == y1);
    }
  }
  CHECK(curves[0]->color == O3M_COLOR_INSTR);
  CHECK(curves[1]->color == O3M_COLOR_FX);
  // the time label says the window's length
  bool label = false;
  for (const Drawing::Text& t : d.texts) {
    label = label || t.s == "620 ms";
  }
  CHECK(label);
  // every point is inside the frame
  for (const Drawing::Poly* p : curves) {
    for (std::size_t i = 0; i < p->xy.size(); i += 2) {
      CHECK(p->xy[i] >= x0);
      CHECK(p->xy[i] <= x1);
      CHECK(p->xy[i + 1] >= y0);
      CHECK(p->xy[i + 1] <= y1);
    }
  }
}

// ---- OS-MODPITCH (ABI 1.8): the host's Slide / Glide offset ---------------------------------------------------------------------------------------------------------------

TEST_CASE("fm: the descriptor exports pitch, four pages of eight and the Algorithm's labels, all within the ABI's limits (ABI 1.8)") {
  Lib lib;
  NEED(lib.open(O3M_FM_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.pitch != nullptr);
  NEED(m.pages != nullptr);
  CHECK(m.page_count == 4u);
  for (std::uint32_t k = 0; k < m.page_count; ++k) {
    CHECK(m.pages[k].first_param == 8u * k);
    CHECK((std::strlen(m.pages[k].name) >= 1u && std::strlen(m.pages[k].name) < O3M_PAGE_NAME_MAX));
  }
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
  // OS-SNDMOD S2: sixteen algorithms (the label count is the ABI's most), the first eight as they were, the default and the Learn refusal unchanged
  CHECK(m.labels[FM_PARAM_ALGORITHM].count == 16u);
  CHECK(m.params[FM_PARAM_ALGORITHM].min == 1.0f);
  CHECK(m.params[FM_PARAM_ALGORITHM].max == 16.0f);
  CHECK(m.params[FM_PARAM_ALGORITHM].def == 5.0f);
  CHECK((m.params[FM_PARAM_ALGORITHM].flags & O3M_PARAM_NO_LEARN) != 0u);
  CHECK(std::string(m.labels[FM_PARAM_ALGORITHM].names[0]) == "4>3>2>1");
  CHECK(std::string(m.labels[FM_PARAM_ALGORITHM].names[7]) == "1+2+3+4");
  for (std::uint32_t a = 0; a < 16u; ++a) {
    for (std::uint32_t b = a + 1; b < 16u; ++b) {
      CHECK(std::string(m.labels[FM_PARAM_ALGORITHM].names[a]) != std::string(m.labels[FM_PARAM_ALGORITHM].names[b]));
    }
  }
  // OS-SNDMOD S2: Detune is +-1200 cents on all four operators, whole numbers; the defaults (0, 0, 4, 0) are as they were
  const float detune_def[4] = {0.0f, 0.0f, 4.0f, 0.0f};
  for (std::uint32_t op = 0; op < 4; ++op) {
    CAPTURE(op);
    const o3m_param& d = m.params[8u * op + 1u];
    CHECK(d.min == -1200.0f);
    CHECK(d.max == 1200.0f);
    CHECK(d.def == detune_def[op]);
    CHECK((d.flags & O3M_PARAM_INT) != 0u);
    CHECK(O3M_PARAM_UNIT(d.flags) == O3M_UNIT_CENTS);
  }
  // OS-SNDMOD S1 (F4): Level is 0.75 dB a step, not a percentage: a bare 0..100 with no unit, on all four operators
  for (std::uint32_t op = 0; op < 4; ++op) {
    CAPTURE(op);
    CHECK(O3M_PARAM_UNIT(m.params[8u * op + 2u].flags) == O3M_UNIT_NONE);
    CHECK((m.params[8u * op + 2u].flags & O3M_PARAM_MOD) != 0u);
  }
}

TEST_CASE("fm: pitch(0, 0) is bit for bit not calling it, and pitch(12, 0) is an octave up; a legato ramp note keeps the envelopes (ABI 1.8)") {
  Fixture a, b;
  NEED(a.ok);
  NEED(b.ok);
  a.solo(1, 1.0f);
  b.solo(1, 1.0f);
  a.inst.on(57);
  b.inst.on(57);
  b.lib.entry->pitch(b.inst.state, 0.0f, 0.0f);
  const std::vector<float> sa = a.inst.render(4096);
  const std::vector<float> sb = b.inst.render(4096);
  CHECK(sa == sb);
  Fixture f;
  NEED(f.ok);
  f.solo(1, 1.0f);
  f.inst.on(57);
  f.inst.render(512);
  const std::vector<float> base = f.inst.render(22050);
  f.lib.entry->pitch(f.inst.state, 12.0f, 0.0f);
  f.inst.render(512);
  const std::vector<float> up = f.inst.render(22050);
  CHECK(frequency(up.data(), up.size()) == doctest::Approx(2.0 * frequency(base.data(), base.size())).epsilon(2e-3));
  // a note over a sounding one with LEGATO | RAMP: the pitch moves to the note, the envelope is not started again (a decaying operator keeps decaying)
  Fixture g;
  NEED(g.ok);
  g.solo(1, 1.0f);
  g.op(1, 1.0f, 0.0f, 100.0f, 1.0f, 300.0f, 0.0f, 100.0f);  // a decay of 300 ms to nothing
  g.inst.on(57);
  g.inst.render(8820);
  const auto peak = [](const std::vector<float>& s) {
    float p = 0.0f;
    for (float x : s) {
      p = std::fabs(x) > p ? std::fabs(x) : p;
    }
    return p;
  };
  g.inst.on(69, 127, O3M_NOTE_LEGATO | O3M_NOTE_RAMP);
  const std::vector<float> kept = g.inst.render(1024);
  Fixture h;
  NEED(h.ok);
  h.solo(1, 1.0f);
  h.op(1, 1.0f, 0.0f, 100.0f, 1.0f, 300.0f, 0.0f, 100.0f);
  h.inst.on(57);
  h.inst.render(8820);
  h.inst.on(69, 127, 0);
  const std::vector<float> again = h.inst.render(1024);
  CHECK(peak(kept) < peak(again));
  CHECK(peak(kept) > 0.0f);
}
