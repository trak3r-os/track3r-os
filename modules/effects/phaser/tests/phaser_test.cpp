// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): the PHASER effect module driven through the ABI as the OS drives an insert, against a double-precision reference of the same maths (the block-wise control
// step with the same float LFO, the bilinear coefficient with the same Pade tan, the normalised-lattice all-pass chain with its feedback in double): the sample-by-sample error over a grid of settings; the frequency
// response against the exact analytic one (std::tan, complex arithmetic: the Pade's error is bounded by it); the LFO's period; the swept frequency range against exp2 at the LFO extremes, computed here in double; the level law (at the
// defaults the output rms is never above the input's, DRIVE's lesson, and over the whole parameter grid, the fastest sweep included, never above it either: the sections are contractions), and its one exception, a Feedback that moves
// (a MOD target: the per-sample bound needs a fixed g; the measured bound is stated); stability at the maximum feedback over a long run (finite, bounded, no growth) and
// NaN-freedom (hostile parameters, a NaN or infinite input flushed at the end of its block); Mix 0 as the dry track bit for bit; silence in giving silence out and the tail's end; block-size
// independence and in == out; the Mix glide; the view and the packed file through the loader.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-199).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "phaser_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;

struct Setting {
  int stages = 4;
  double period = 2000.0;  // ms
  double depth = 60.0;     // %
  double centre = 800.0;   // Hz
  double fb = 35.0;        // %
  double mix = 50.0;       // %
};

// ---- the reference ----------------------------------------------------------------------------------------------------------------------------
// The control step is the module's own (a float LFO phase and wave, the exp2 and the clamp, the Pade tan: the oracle checks the maths, not the float rounding); the audio path is in double.
double ref_coef(double f) {
  const double x = f * 3.14159265358979 / kFs;
  const double p = x * (15.0 - x * x), q = 15.0 - 6.0 * x * x;
  return (p - q) / (p + q);
}

std::vector<double> ref_run(const Setting& st, const std::vector<float>& x, std::size_t block = 64) {
  const float inc = 1000.0f / (static_cast<float>(st.period) * kRate);
  float ph = 0.25f;
  const double g = st.fb / 100.0, m = st.mix / 100.0, dep = static_cast<double>(static_cast<float>(st.depth) * 0.01f);
  double a = 0.0, c = 0.0, fb = 0.0, z[6] = {0, 0, 0, 0, 0, 0};
  bool first = true;
  std::vector<double> y(x.size());
  for (std::size_t at = 0; at < x.size(); at += block) {
    const std::size_t nb = std::min(block, x.size() - at);
    ph += inc * static_cast<float>(nb);
    ph -= ph >= 1.0f ? 1.0f : 0.0f;
    const float t = 4.0f * std::fabs(ph - 0.5f) - 1.0f;
    const float lfo = t * (1.5f - 0.5f * t * t);
    double f = st.centre;
    if (dep != 0.0) {
      f *= std::exp2(2.0 * dep * static_cast<double>(lfo));
    }
    f = std::min(std::max(f, 40.0), 0.17 * kFs);
    const double target = ref_coef(f);
    const double ctarget = std::sqrt(1.0 - target * target) * 0.999999;
    if (first) {
      a = target;
      c = ctarget;
      first = false;
    }
    const double da = (target - a) / static_cast<double>(nb), dc = (ctarget - c) / static_cast<double>(nb);
    for (std::size_t i = 0; i < nb; ++i) {
      a += da;
      c += dc;
      const double xi = x[at + i];
      double u = (1.0 - std::fabs(g)) * xi + g * fb;
      for (int k = 0; k < st.stages; ++k) {
        const double yy = a * u + c * z[k];
        z[k] = c * u - a * z[k];
        u = yy;
      }
      fb = std::min(std::max(u, -4.0), 4.0);
      y[at + i] = xi + m * (u - xi);
    }
    a = target;
    c = ctarget;
  }
  return y;
}

// The exact response of the static effect (depth 0) at f Hz: std::tan, complex arithmetic: |(1 - m) + m W|, W = (1 - |g|) A / (1 - g A / z) (the feedback is one sample late), A = H^N,
// H = (a + 1/z) / (1 + a/z).
double exact_gain(const Setting& st, double f) {
  const double w = std::tan(kPi * st.centre / kFs);
  const double a = (w - 1.0) / (w + 1.0);
  const std::complex<double> zi = std::polar(1.0, -2.0 * kPi * f / kFs);
  const std::complex<double> h1 = (a + zi) / (1.0 + a * zi);
  std::complex<double> A = 1.0;
  for (int k = 0; k < st.stages; ++k) {
    A *= h1;
  }
  const double g = st.fb / 100.0, m = st.mix / 100.0;
  const std::complex<double> W = (1.0 - std::fabs(g)) * A / (1.0 - g * A * zi);
  return std::abs((1.0 - m) + m * W);
}

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fx() { ok = lib.open(O3M_PHASER_HOST_SO) && inst.start(*lib.entry); }
  void set(const Setting& s) {
    inst.set(PHASER_PARAM_STAGES, static_cast<float>(s.stages));
    inst.set(PHASER_PARAM_PERIOD, static_cast<float>(s.period));
    inst.set(PHASER_PARAM_DEPTH, static_cast<float>(s.depth));
    inst.set(PHASER_PARAM_CENTRE, static_cast<float>(s.centre));
    inst.set(PHASER_PARAM_FEEDBACK, static_cast<float>(s.fb));
    inst.set(PHASER_PARAM_MIX, static_cast<float>(s.mix));
  }
  // The input through render() in blocks of `block`; `busy` keeps the last return value.
  std::vector<float> run(const std::vector<float>& x, std::uint32_t block = 64) {
    std::vector<float> y(x.size());
    for (std::size_t at = 0; at < x.size();) {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, x.size() - at));
      busy = inst.m->render(inst.state, x.data() + at, y.data() + at, n);
      at += n;
    }
    return y;
  }
  std::uint32_t busy = 0;
};

std::vector<float> noise(std::size_t n, std::uint32_t seed, float amp = 0.5f) {
  std::vector<float> x(n);
  std::uint32_t s = seed;
  for (auto& v : x) {
    s = s * 1664525u + 1013904223u;
    v = amp * (static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.0f);
  }
  return x;
}

std::vector<float> sine(std::size_t n, double amp, double f) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs));
  }
  return x;
}

std::vector<float> square(std::size_t n, double f) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs) >= 0.0 ? 1.0f : -1.0f;
  }
  return x;
}

double rms(const std::vector<float>& v, std::size_t from = 0) {
  double s = 0;
  for (std::size_t i = from; i < v.size(); ++i) {
    s += static_cast<double>(v[i]) * v[i];
  }
  return std::sqrt(s / static_cast<double>(v.size() - from));
}

double peak(const std::vector<float>& v, std::size_t from = 0, std::size_t to = 0) {
  double p = 0;
  for (std::size_t i = from; i < (to == 0 ? v.size() : to); ++i) {
    p = std::max(p, static_cast<double>(std::fabs(v[i])));
  }
  return p;
}

bool all_finite(const std::vector<float>& v) {
  for (float x : v) {
    if (!std::isfinite(x)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("phaser: the descriptor says an effect with six parameters and a view") {
  Lib lib;
  NEED(lib.open(O3M_PHASER_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == 6u);
  CHECK(PHASER_PARAM_STAGES == 0);
  CHECK(PHASER_PARAM_PERIOD == 1);  // the first modulation target: the bench's moving-parameter row sweeps it
  CHECK(PHASER_PARAM_DEPTH == 2);
  CHECK(PHASER_PARAM_CENTRE == 3);
  CHECK(PHASER_PARAM_FEEDBACK == 4);
  CHECK(PHASER_PARAM_MIX == 5);
  CHECK((m.params[PHASER_PARAM_STAGES].flags & O3M_PARAM_INT) != 0u);
  CHECK(m.params[PHASER_PARAM_STAGES].min == 4.0f);
  CHECK(m.params[PHASER_PARAM_STAGES].max == 6.0f);
  CHECK(m.params[PHASER_PARAM_STAGES].def == 4.0f);
  CHECK((m.params[PHASER_PARAM_STAGES].flags & O3M_PARAM_MOD) == 0u);  // a change of Stages is a jump: not a modulation target
  CHECK(O3M_PARAM_UNIT(m.params[PHASER_PARAM_PERIOD].flags) == O3M_UNIT_MS);
  CHECK((m.params[PHASER_PARAM_PERIOD].flags & O3M_PARAM_LOG) != 0u);
  CHECK(m.params[PHASER_PARAM_PERIOD].min == 100.0f);
  CHECK(m.params[PHASER_PARAM_PERIOD].max == 20000.0f);
  CHECK(O3M_PARAM_UNIT(m.params[PHASER_PARAM_DEPTH].flags) == O3M_UNIT_PERCENT);
  CHECK(O3M_PARAM_UNIT(m.params[PHASER_PARAM_CENTRE].flags) == O3M_UNIT_HZ);
  CHECK((m.params[PHASER_PARAM_CENTRE].flags & O3M_PARAM_LOG) != 0u);
  CHECK(m.params[PHASER_PARAM_CENTRE].min == 100.0f);
  CHECK(m.params[PHASER_PARAM_CENTRE].max == 4000.0f);
  CHECK(m.params[PHASER_PARAM_FEEDBACK].min == -90.0f);  // the bound of the feedback: |g| <= 0.9
  CHECK(m.params[PHASER_PARAM_FEEDBACK].max == 90.0f);
  CHECK(m.params[PHASER_PARAM_MIX].def == 50.0f);
  for (std::uint32_t i = 1; i < 6u; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);  // MIDI Learn may bind each of them
    CHECK((m.params[i].flags & O3M_PARAM_MOD) != 0u);       // set_param does a clamp, a store and at most one division
  }
  CHECK(m.view_count == 1u);
  CHECK(sizeof(float) * 28u <= PHASER_STATE_BYTES);
}

TEST_CASE("phaser: the output tracks a double-precision reference over a grid of settings") {
  const std::vector<float> x = noise(8192, 7, 1.0f);  // full scale, two cycles of the fastest LFO
  const int stages[] = {4, 5, 6};
  const double periods[] = {100.0, 2000.0};
  const double depths[] = {0.0, 100.0};
  const double centres[] = {100.0, 800.0, 4000.0};
  const double fbs[] = {-90.0, 0.0, 35.0, 90.0};
  const double mixes[] = {50.0, 100.0};
  double worst_err = 0.0, worst_snr = 1e9;
  int settings = 0;
  for (int n : stages) {
    for (double per : periods) {
      for (double dep : depths) {
        for (double cen : centres) {
          for (double fb : fbs) {
            for (double mx : mixes) {
              const Setting st{n, per, dep, cen, fb, mx};
              Fx fx;
              NEED(fx.ok);
              fx.set(st);
              const std::vector<float> y = fx.run(x);
              const std::vector<double> ref = ref_run(st, x);
              double es = 0, ee = 0, err = 0;
              for (std::size_t i = 0; i < x.size(); ++i) {
                const double e = static_cast<double>(y[i]) - ref[i];
                es += ref[i] * ref[i];
                ee += e * e;
                err = std::max(err, std::fabs(e));
              }
              CAPTURE(n);
              CAPTURE(per);
              CAPTURE(dep);
              CAPTURE(cen);
              CAPTURE(fb);
              CAPTURE(mx);
              CHECK(err < 1e-4);
              const double snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
              CHECK_MESSAGE(snr > 80.0, "snr " << snr);
              worst_snr = std::min(worst_snr, snr);
              worst_err = std::max(worst_err, err);
              ++settings;
            }
          }
        }
      }
    }
  }
  MESSAGE("worst max error " << worst_err << " (limit 1e-4), worst SNR " << worst_snr << " dB (limit 80) over " << settings << " settings");
}

TEST_CASE("phaser: the frequency response is the exact bilinear all-pass one (std::tan), notches and feedback included") {
  constexpr std::size_t M = 8192, settle = 24576;
  double worst = 0.0;
  struct Row {
    int stages;
    double fb, mix;
  };
  const Row rows[] = {{4, 0.0, 50.0}, {6, 0.0, 50.0}, {4, 50.0, 50.0}, {4, -50.0, 100.0}, {6, 90.0, 100.0}, {5, -90.0, 50.0}};
  for (const Row& r : rows) {
    Setting st;
    st.stages = r.stages;
    st.depth = 0.0;  // static: the LFO does not move it
    st.fb = r.fb;
    st.mix = r.mix;
    for (int k = 0; k < 24; ++k) {
      const double f0 = 60.0 * std::pow(2.0, 7.0 * k / 23.0);  // 60 Hz .. 7.7 kHz, a log grid
      const double fe = std::round(f0 * M / kFs) * kFs / M;     // a whole number of cycles in the window
      Fx fx;
      NEED(fx.ok);
      fx.set(st);
      const std::vector<float> y = fx.run(sine(settle + M, 0.25, fe));
      double a = 0, b = 0;
      for (std::size_t i = settle; i < settle + M; ++i) {
        const double ph = 2.0 * kPi * fe * static_cast<double>(i) / kFs;
        a += y[i] * std::sin(ph);
        b += y[i] * std::cos(ph);
      }
      const double meas = std::hypot(a, b) * 2.0 / M / 0.25;
      const double want = exact_gain(st, fe);
      CAPTURE(r.stages);
      CAPTURE(r.fb);
      CAPTURE(r.mix);
      CAPTURE(fe);
      CHECK(std::fabs(meas - want) < 3e-3);
      worst = std::max(worst, std::fabs(meas - want));
    }
  }
  MESSAGE("response: the largest gain difference from the exact one " << worst << " (limit 3e-3) over 6 rows x 24 frequencies");
  // the notches are there: 4 stages, Mix 50 %, no feedback: the exact response dips under 0.05 at two frequencies (-90 and -270 degrees of one section)
  Setting st;
  st.depth = 0.0;
  st.fb = 0.0;
  double lo = 1e9;
  int dips = 0;
  bool below = false;
  for (int k = 0; k < 2000; ++k) {
    const double g = exact_gain(st, 20.0 * std::pow(2.0, 10.0 * k / 1999.0));
    lo = std::min(lo, g);
    if (g < 0.05 && !below) {
      ++dips;
    }
    below = g < 0.05;
  }
  CHECK(dips == 2);
  CHECK(lo < 0.01);
}

TEST_CASE("phaser: the LFO period is the Period parameter, and Depth 0 does not modulate") {
  // a sine in the notches' reach: the 64-sample rms envelope follows the sweep and repeats with the period; its autocorrelation peaks at the period
  for (double per : {500.0, 1000.0, 3000.0}) {
    Setting st;
    st.period = per;
    st.depth = 100.0;
    st.fb = 0.0;
    const std::size_t cycles = per >= 3000.0 ? 4 : 8;
    const std::size_t n = static_cast<std::size_t>(per * 0.001 * kFs) * cycles;
    Fx fx;
    NEED(fx.ok);
    fx.set(st);
    const std::vector<float> y = fx.run(sine(n, 0.5, 1000.0));
    std::vector<double> env;
    for (std::size_t at = 0; at + 64 <= n; at += 64) {
      double s = 0;
      for (std::size_t i = 0; i < 64; ++i) {
        s += static_cast<double>(y[at + i]) * y[at + i];
      }
      env.push_back(s / 64.0);
    }
    double mean = 0;
    for (double v : env) {
      mean += v;
    }
    mean /= static_cast<double>(env.size());
    for (double& v : env) {
      v -= mean;
    }
    const std::size_t p = static_cast<std::size_t>(per * 0.001 * kFs / 64.0);  // the period in blocks
    auto corr = [&](std::size_t lag) {
      double a = 0, b = 0, c = 0;
      for (std::size_t i = 0; i + lag < env.size(); ++i) {
        a += env[i] * env[i + lag];
        b += env[i] * env[i];
        c += env[i + lag] * env[i + lag];
      }
      return a / std::sqrt(b * c);
    };
    std::size_t best = 0;
    double bestc = -2.0;
    for (std::size_t lag = p * 4 / 10; lag <= p * 16 / 10; ++lag) {
      const double c = corr(lag);
      if (c > bestc) {
        bestc = c;
        best = lag;
      }
    }
    CAPTURE(per);
    MESSAGE("Period " << per << " ms: the envelope repeats at lag " << best << " blocks (the period is " << p << "), correlation " << bestc);
    CHECK(std::fabs(static_cast<double>(best) - static_cast<double>(p)) <= 0.03 * static_cast<double>(p) + 1.0);
    CHECK(bestc > 0.9);
    double mn = 1e9, mx = -1e9;
    for (double v : env) {
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    CHECK(mx - mn > 0.1 * std::fabs(mean));  // the sweep is heard: the envelope moves by over 10 % of its mean
  }
  // Depth 0: the notches stand still: a settled sine's envelope is flat
  Setting st;
  st.depth = 0.0;
  Fx fx;
  NEED(fx.ok);
  fx.set(st);
  const std::vector<float> y = fx.run(sine(88200, 0.5, 1000.0));
  double lo = 1e9, hi = 0.0;
  for (std::size_t at = 22050; at + 441 <= y.size(); at += 441) {  // a window of 441 samples is 10 whole cycles of 1 kHz
    double s = 0;
    for (std::size_t i = 0; i < 441; ++i) {
      s += static_cast<double>(y[at + i]) * y[at + i];
    }
    lo = std::min(lo, s);
    hi = std::max(hi, s);
  }
  CHECK(hi < lo * 1.0001);
}

TEST_CASE("phaser: the swept range is Centre 2^(+-2 Depth) at the LFO extremes, clamped to 40 Hz .. 0.17 fs") {
  // The expected range is computed here from the parameters alone -- std::exp2 at the LFO's two extremes (lfo = -1 and +1), clamped -- NOT by re-running the module's block-wise control step. The module's own
  // frequency at an extreme is read back from an impulse: with Mix 100 %, no feedback and the sections at rest (silence before it), the impulse's first output sample is a^Stages (every section passes a u through
  // with z = 0), so with 5 stages a = copysign(|y|^(1/5), y) and f = fs/pi atan((1+a)/(1-a)), the inverse of the bilinear coefficient (the Pade tan's 2e-5 error is about 0.1 Hz at the top of the sweep).
  for (double cen : {800.0, 4000.0}) {
    const double dep = 1.0;  // Depth 100 %
    const double f_lo = std::min(std::max(cen * std::exp2(-2.0 * dep), 40.0), 0.17 * kFs);
    const double f_hi = std::min(std::max(cen * std::exp2(2.0 * dep), 40.0), 0.17 * kFs);
    for (int which = 0; which < 2; ++which) {  // 0: the LFO's low extreme (phase 0.5, lfo = -1); 1: the high one (phase 0/1, lfo = +1)
      Setting st;
      st.stages = 5;  // an odd count: the sign of a survives in a^5
      st.depth = 100.0;
      st.period = 20000.0;  // the slowest LFO: the wave p(t) = t(3 - t^2)/2 is flat at its peaks, so the frequency holds over the impulse's block
      st.centre = cen;
      st.fb = 0.0;
      st.mix = 100.0;
      Fx fx;
      NEED(fx.ok);
      fx.set(st);
      // Place the impulse in the block whose end phase is the closest to the extreme (this only says where the LFO is, not what it computes).
      const double inc = 1000.0 / (st.period * kFs);  // the LFO's cycles a sample: 1 / (Period fs)
      const std::size_t blocks = 11000;
      const double target = which == 0 ? 0.5 : 0.0;  // the phase of the extreme, mod 1 (0 and 1 are the same phase)
      std::size_t b_at = 0;
      double best = 1e9;
      for (std::size_t b = 0; b < blocks; ++b) {
        const double ph = 0.25 + (static_cast<double>(b) + 1.0) * 64.0 * inc;  // the phase at the end of block b
        const double phw = ph - std::floor(ph);
        const double d = std::min(std::fabs(phw - target), 1.0 - std::fabs(phw - target));
        if (d < best) {
          best = d;
          b_at = b;
        }
      }
      const std::size_t n = blocks * 64;
      std::vector<float> x(n, 0.0f);
      x[b_at * 64] = 1.0f;  // the impulse at the block's first sample
      const std::vector<float> y = fx.run(x);
      const double a5 = y[b_at * 64];
      NEED(a5 != 0.0);
      const double a = std::copysign(std::pow(std::fabs(a5), 1.0 / 5.0), a5);
      const double f_meas = kFs / kPi * std::atan((1.0 + a) / (1.0 - a));
      const double f_exp = which == 0 ? f_lo : f_hi;
      CAPTURE(cen);
      CAPTURE(which);
      MESSAGE("Centre " << cen << " Hz, Depth 100 %, LFO extreme " << (which == 0 ? "lfo = -1" : "lfo = +1") << ": the swept frequency measured " << f_meas << " Hz, expected " << f_exp
                        << " Hz (Centre 2^(+-2 Depth) by exp2 here, clamped to 40 Hz .. 0.17 fs)");
      CHECK(std::fabs(f_meas - f_exp) <= std::max(1.0, 0.005 * f_exp));
    }
  }
}

TEST_CASE("phaser: the level law: the output rms is never above the input's at the defaults, nor over the whole grid with the sweep at its fastest") {
  // The lesson of DRIVE (+12..16 dB over bypass at its defaults): the defaults (4 stages, 2 s, depth 60 %, 800 Hz, feedback 35 %, mix 50 %) must not raise the level. |W| <= 1 at every frequency
  // (the input is scaled by 1 - |g| before the loop) and |(1 - m) + m W| <= 1, so the output cannot be louder than the input on any signal; the lattice sections keep it so while the coefficient moves.
  double worst_up = -1e9;
  const double freqs[] = {100.0, 440.0, 1234.5, 3000.0};
  for (int db = -60; db <= -6; db += 2) {
    const double amp = std::pow(10.0, db / 20.0);
    for (double f : freqs) {
      const std::vector<float> x = sine(44100, amp, f);
      Fx fx;
      NEED(fx.ok);  // the defaults: start() sets every parameter to its default
      const std::vector<float> y = fx.run(x);
      CAPTURE(db);
      CAPTURE(f);
      CHECK(rms(y) <= rms(x));
      worst_up = std::max(worst_up, 20.0 * std::log10(rms(y) / rms(x)));
    }
    for (std::uint32_t seed : {1u, 2u}) {
      const std::vector<float> x = noise(44100, seed, static_cast<float>(amp * 1.7320508));  // a uniform noise of rms amp
      Fx fx;
      NEED(fx.ok);
      const std::vector<float> y = fx.run(x);
      CAPTURE(db);
      CHECK(rms(y) <= rms(x));
      worst_up = std::max(worst_up, 20.0 * std::log10(rms(y) / rms(x)));
    }
  }
  MESSAGE("defaults: the largest rise over bypass " << worst_up << " dB (must be <= 0)");
  CHECK(worst_up <= 0.0);

  // the whole grid: a static response is a passive filter, and so is a moving one: each section is a reflection [a c; c -a] of (input, state), which never gains, whatever the coefficient does
  double worst_static = -1e9, worst_moving = -1e9;
  for (int n : {4, 5, 6}) {
    for (double fb : {-90.0, 0.0, 90.0}) {
      for (double mx : {25.0, 50.0, 100.0}) {
        for (double cen : {100.0, 800.0, 4000.0}) {
          for (double f : {150.0, 1000.0, 5000.0}) {
            for (double dep : {0.0, 100.0}) {
              Setting st;
              st.stages = n;
              st.fb = fb;
              st.mix = mx;
              st.centre = cen;
              st.depth = dep;
              st.period = 100.0;  // the fastest sweep: the worst for the time-varying case
              Fx fx;
              NEED(fx.ok);
              fx.set(st);
              const std::vector<float> x = sine(44100, 0.5, f);
              const std::vector<float> y = fx.run(x);
              const double d = 20.0 * std::log10(rms(y) / rms(x));
              CAPTURE(n);
              CAPTURE(fb);
              CAPTURE(mx);
              CAPTURE(cen);
              CAPTURE(f);
              CAPTURE(dep);
              (dep == 0.0 ? worst_static : worst_moving) = std::max(dep == 0.0 ? worst_static : worst_moving, d);
              CHECK(d <= 2e-3);
            }
          }
        }
      }
    }
  }
  MESSAGE("grid: the largest rise over bypass: static " << worst_static << " dB, moving at the fastest sweep " << worst_moving << " dB (limit 0.002: none)");
}

TEST_CASE("phaser: Feedback moving (it is a MOD target) is the exception to the fixed-g level law: the measured bound is stated, the clamp still bounds the signal") {
  // |W| <= 1 is a per-sample bound with the g of that sample in the loop equation. Feedback glides (a MOD source may call set_param every block), so while it moves the loop is time-varying: an impulse charged while
  // g = 0 and then the fastest Feedback rise the glide allows recirculates the energy the sections stored. This measures the output peak on a full-scale input and states the bound; the fed-back sample is clamped to
  // +-4, so the signal stays finite and bounded.
  double worst_peak = 0.0;
  int runs = 0;
  for (int stages : {4, 6}) {
    for (double cen : {100.0, 800.0, 4000.0}) {
      for (double fb_hi : {90.0, -90.0}) {
        Setting st;
        st.stages = stages;
        st.depth = 0.0;
        st.period = 20000.0;
        st.centre = cen;
        st.fb = 0.0;  // the impulse is charged while g = 0: nothing recirculates yet
        st.mix = 100.0;
        Fx fx;
        NEED(fx.ok);
        fx.set(st);
        const std::size_t n = 44100;  // 1 s
        std::vector<float> x(n, 0.0f);
        x[64] = 1.0f;  // one full-scale impulse in block 1
        std::vector<float> y(n);
        for (std::size_t p = 0; p < n; p += 64) {
          if (p == 128) {  // the rise: set_param every block is the fastest a MOD source can go
            fx.inst.set(PHASER_PARAM_FEEDBACK, static_cast<float>(fb_hi));
          }
          const std::uint32_t nb = static_cast<std::uint32_t>(std::min<std::size_t>(64, n - p));
          fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, nb);
        }
        CAPTURE(stages);
        CAPTURE(cen);
        CAPTURE(fb_hi);
        NEED(all_finite(y));
        const double pk = peak(y);
        CHECK(pk <= 5.0);  // the measured bound with Feedback moving: up to about 5x the full-scale input peak (the fixed-g law does not hold sample-wise here); the +-4 clamp on the fed-back sample keeps the output finite and bounded
        worst_peak = std::max(worst_peak, pk);
        ++runs;
      }
    }
  }
  MESSAGE(runs << " runs with Feedback swept 0 -> +-90 % right after a full-scale impulse: the largest output peak " << worst_peak << " on input peak 1 (" << worst_peak
               << "x; the fed-back sample is clamped to +-4, the output stays under that bound)");
}

TEST_CASE("phaser: stable at the maximum feedback over a long run: finite, bounded, no growth, whatever the sweep") {
  // |g| <= 0.9, the input scaled by 1 - |g|, the fed-back sample clamped to +-4 and sections that never gain (a reflection of (input, state)): bounded by construction; this is the run that shows it.
  const std::size_t n = 441000;  // 10 s
  const std::vector<float> inputs[] = {noise(n, 1, 1.0f), square(n, 1000.0), square(n, 55.0), sine(n, 1.0, 330.0)};
  struct Sweep {
    double per, dep, cen;
  };
  const Sweep sweeps[] = {{100.0, 100.0, 100.0}, {100.0, 100.0, 4000.0}, {20000.0, 0.0, 800.0}};
  double worst_peak = 0.0, worst_growth = -1e9, worst_rms_up = -1e9;
  int runs = 0;
  for (int stages : {4, 5, 6}) {
    for (double fb : {90.0, -90.0}) {
      for (const Sweep& sw : sweeps) {
        for (const std::vector<float>& x : inputs) {
          Setting st;
          st.stages = stages;
          st.period = sw.per;
          st.depth = sw.dep;
          st.centre = sw.cen;
          st.fb = fb;
          st.mix = 100.0;
          Fx fx;
          NEED(fx.ok);
          fx.set(st);
          const std::vector<float> y = fx.run(x);
          CAPTURE(stages);
          CAPTURE(fb);
          CAPTURE(sw.per);
          CAPTURE(sw.cen);
          NEED(all_finite(y));
          const double pk = peak(y);
          CHECK(pk <= 4.0);  // the clamp's level: the wet path cannot exceed the input in level, so a full-scale input stays near full scale
          // no growth: the last third is no louder than the first third (a |H| <= 1 filter keeps the level), and its rms is not above the input's
          const std::size_t t3 = n / 3;
          const double first = rms(std::vector<float>(y.begin(), y.begin() + t3));
          const double last = rms(y, 2 * t3);
          CHECK(last <= first * 1.05 + 1e-3);
          const double up = 20.0 * std::log10(last / rms(x, 2 * t3));
          CHECK(up <= 5e-3);
          worst_peak = std::max(worst_peak, pk);
          worst_growth = std::max(worst_growth, 20.0 * std::log10(last / std::max(first, 1e-9)));
          worst_rms_up = std::max(worst_rms_up, up);
          ++runs;
        }
      }
    }
  }
  MESSAGE(runs << " runs of 10 s at Feedback +-90 %: largest peak " << worst_peak << " (limit 4), largest last-third over first-third " << worst_growth << " dB, largest last-third over the input " << worst_rms_up << " dB (limit 0.005: none)");
  // one minute at the worst setting (6 stages, +90 %, the fastest full sweep down to 40 Hz, Mix 100 %): the end is as quiet as the middle
  Setting st;
  st.stages = 6;
  st.period = 100.0;
  st.depth = 100.0;
  st.centre = 100.0;
  st.fb = 90.0;
  st.mix = 100.0;
  Fx fx;
  NEED(fx.ok);
  fx.set(st);
  const std::vector<float> x = noise(44100 * 60, 3, 1.0f);
  const std::vector<float> y = fx.run(x);
  NEED(all_finite(y));
  CHECK(peak(y) <= 4.0);
  const std::size_t half = y.size() / 2;
  CHECK(rms(y, half) <= rms(std::vector<float>(y.begin(), y.begin() + half)) * 1.02);
  CHECK(20.0 * std::log10(rms(y, half) / rms(x, half)) <= 5e-3);
  // a hard step, then silence: the ring dies away (no sustained oscillation) within a few seconds at the worst setting (a DC step rings the loop at its slowest)
  Fx ring;
  NEED(ring.ok);
  ring.set(st);
  std::vector<float> burst(4096, 1.0f);
  (void)ring.run(burst);
  const std::vector<float> zero(64, 0.0f);
  std::vector<float> o(64);
  std::uint32_t blocks = 0;
  while (ring.inst.m->render(ring.inst.state, zero.data(), o.data(), 64) != 0u) {
    NEED(++blocks < 8u * 44100u / 64u);  // 8 s
    for (float v : o) {
      NEED(std::isfinite(v));
      NEED(std::fabs(v) <= 4.0f);
    }
  }
  MESSAGE("worst setting: the tail after a full-scale step ended after " << blocks << " silent blocks (" << (blocks * 64.0 / kFs) << " s)");
}

TEST_CASE("phaser: hostile values: NaN and infinite parameters clamp, a NaN or infinite input is flushed at the end of its block and does not poison the effect for good") {
  Fx fx;
  NEED(fx.ok);
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  for (std::uint32_t id = 0; id < 7; ++id) {  // (id 6 does not exist: ignored)
    for (float v : {nan, inf, -inf, 1e30f, -1e30f}) {
      fx.inst.set(id, v);
    }
  }
  const std::vector<float> y = fx.run(noise(8192, 9, 1.0f));
  NEED(all_finite(y));
  CHECK(peak(y) <= 4.0);
  for (int stages : {4, 6}) {
    for (double fb : {0.0, 90.0}) {
      for (float poison : {nan, inf, -inf, 1e30f, -1e30f}) {
        Setting st;
        st.stages = stages;
        st.fb = fb;
        Fx fy;
        NEED(fy.ok);
        fy.set(st);
        (void)fy.run(noise(1024, 3, 0.5f));
        std::vector<float> bad(64, 0.25f);
        bad[10] = poison;  // inside one block: the flush is at its end
        std::vector<float> out(64);
        fy.inst.m->render(fy.inst.state, bad.data(), out.data(), 64);
        CAPTURE(stages);
        CAPTURE(fb);
        CAPTURE(poison);
        const std::vector<float> after = fy.run(noise(4096, 6, 0.5f));  // the next block is clean: the sections were flushed
        NEED(all_finite(after));
        CHECK(peak(after) <= 4.0);
        CHECK(rms(after) <= rms(noise(4096, 6, 0.5f)) * 1.02);
      }
    }
  }
  // every setting of the extremes on full-scale input stays finite, with nothing subnormal left in the output
  const std::vector<float> loud = noise(4096, 5, 1.0f);
  for (int stages : {4, 6}) {
    for (double per : {100.0, 20000.0}) {
      for (double dep : {0.0, 100.0}) {
        for (double cen : {100.0, 4000.0}) {
          for (double fb : {-90.0, 90.0}) {
            for (double mx : {0.0, 100.0}) {
              Setting st{stages, per, dep, cen, fb, mx};
              Fx fz;
              NEED(fz.ok);
              fz.set(st);
              std::vector<float> in = loud;
              in[100] = 1.0f;
              in[101] = -1.0f;
              const std::vector<float> yy = fz.run(in);
              CAPTURE(stages);
              CAPTURE(per);
              CAPTURE(dep);
              CAPTURE(cen);
              CAPTURE(fb);
              CAPTURE(mx);
              for (float v : yy) {
                NEED(std::isfinite(v));
                NEED(std::fabs(v) <= 4.0f);
                NEED(std::fpclassify(v) != FP_SUBNORMAL);
              }
            }
          }
        }
      }
    }
  }
}

TEST_CASE("phaser: Mix 0 is the input bit for bit, also after a wet period") {
  const std::vector<float> x = noise(4096, 11);
  Setting st;
  st.mix = 0.0;
  Fx dry;
  NEED(dry.ok);
  dry.set(st);
  const std::vector<float> yd = dry.run(x);
  CHECK(std::memcmp(yd.data(), x.data(), x.size() * sizeof(float)) == 0);

  // wet first, then Mix 0: the mix glides to 0 (about 22 blocks at 64 frames, then it snaps), and from then on the output is the input bit for bit
  Fx fx;
  NEED(fx.ok);
  st.mix = 100.0;
  fx.set(st);
  (void)fx.run(noise(4096, 3, 0.5f));
  fx.inst.set(PHASER_PARAM_MIX, 0.0f);
  const std::vector<float> in = noise(64 * 400, 9, 0.25f);
  std::vector<float> o(in.size());
  for (std::size_t at = 0; at < in.size(); at += 64) {
    fx.inst.m->render(fx.inst.state, in.data() + at, o.data() + at, 64);
  }
  CHECK(std::memcmp(o.data() + 64 * 300, in.data() + 64 * 300, 64 * 100 * sizeof(float)) == 0);  // the last 100 blocks are the input
  std::size_t exact_from = in.size();
  for (std::size_t at = in.size(); at >= 64; at -= 64) {
    if (std::memcmp(o.data() + at - 64, in.data() + at - 64, 64 * sizeof(float)) != 0) {
      break;
    }
    exact_from = at - 64;
  }
  MESSAGE("bit exact from sample " << exact_from << " (the glide takes about 22 blocks)");
  CHECK(exact_from <= 64u * 30u);
}

TEST_CASE("phaser: silence in gives silence out, and the tail ends: at the defaults within half a second, with exact zeros afterwards") {
  Setting worst;
  worst.stages = 6;
  worst.period = 100.0;
  worst.depth = 100.0;
  worst.centre = 100.0;
  worst.fb = -90.0;
  for (int which = 0; which < 3; ++which) {
    Fx fx;
    NEED(fx.ok);
    if (which == 1) {
      Setting st;
      st.fb = 90.0;
      st.stages = 6;
      fx.set(st);
    } else if (which == 2) {
      fx.set(worst);
    }
    const std::vector<float> zero(64, 0.0f);
    std::vector<float> out(64);
    CAPTURE(which);
    CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);  // never fed: idle at once, exact zeros
    for (float v : out) {
      CHECK(v == 0.0f);
    }
    // a loud burst, then silence: the sections ring, the effect says so, and it ends
    const std::vector<float> loud = noise(4096, 4, 1.0f);
    CHECK(fx.run(loud).size() == loud.size());
    CHECK(fx.busy == 1u);
    std::uint32_t blocks = 0;
    std::uint32_t last = 1;
    bool rang = false;
    while (last != 0u) {
      last = fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64);
      rang = rang || last != 0u;
      NEED(++blocks < 8u * 44100u / 64u);  // 8 s
    }
    const double secs = blocks * 64.0 / kFs;
    MESSAGE("setting " << which << ": the tail ended after " << blocks << " silent blocks (" << secs << " s)");
    CHECK(rang);  // a burst of noise leaves the sections ringing for at least a block
    if (which == 0) {
      CHECK(secs < 0.5);
    }
    for (int again = 0; again < 3; ++again) {
      CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);
      for (float v : out) {
        CHECK(v == 0.0f);  // exact zeros once it says 0
      }
    }
    // live input is reported live, and a quiet input (a tenth of a Q15 step, 3e-6) is still live: the effect never gates what is there
    std::vector<float> sig(64, 0.1f);
    CHECK(fx.inst.m->render(fx.inst.state, sig.data(), out.data(), 64) == 1u);
    std::vector<float> tiny(64, 3e-6f);
    CHECK(fx.inst.m->render(fx.inst.state, tiny.data(), out.data(), 64) == 1u);
  }
}

TEST_CASE("phaser: any block size gives the same bits (static parameters), and in == out is the same as two buffers") {
  const std::vector<float> x = noise(3000, 5, 1.0f);
  Setting st;
  st.stages = 5;
  st.depth = 0.0;  // the LFO is evaluated once a block: only a static response is block-size independent
  st.fb = 60.0;
  st.mix = 70.0;
  Fx base;
  NEED(base.ok);
  base.set(st);
  const std::vector<float> y = base.run(x, 64);
  for (std::uint32_t block : {1u, 3u, 16u, 17u, 100u, 128u}) {
    Fx fx;
    fx.set(st);
    const std::vector<float> z = fx.run(x, block);
    CAPTURE(block);
    CHECK(std::memcmp(y.data(), z.data(), y.size() * sizeof(float)) == 0);
  }
  // a moving sweep is close at any block size (the LFO is sampled at the block's end): the difference is small against the signal
  Setting mv;
  mv.period = 3000.0;
  mv.depth = 100.0;
  Fx a64;
  a64.set(mv);
  const std::vector<float> xs = noise(44100, 2, 0.5f);
  const std::vector<float> ya = a64.run(xs, 64);
  for (std::uint32_t block : {32u, 128u}) {
    Fx fx;
    fx.set(mv);
    const std::vector<float> yb = fx.run(xs, block);
    double es = 0, ee = 0;
    for (std::size_t i = 0; i < ya.size(); ++i) {
      es += static_cast<double>(ya[i]) * ya[i];
      ee += (static_cast<double>(ya[i]) - yb[i]) * (static_cast<double>(ya[i]) - yb[i]);
    }
    CAPTURE(block);
    CHECK(10.0 * std::log10(es / ee) > 25.0);
  }
  Fx alias;
  alias.set(mv);
  std::vector<float> buf = xs;
  for (std::size_t at = 0; at < buf.size(); at += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, buf.size() - at));
    alias.inst.m->render(alias.inst.state, buf.data() + at, buf.data() + at, n);
  }
  CHECK(std::memcmp(ya.data(), buf.data(), ya.size() * sizeof(float)) == 0);
}

TEST_CASE("phaser: Stages 4, 5 and 6 differ, a fractional Stages rounds, and a hostile one clamps") {
  const std::vector<float> x = noise(2048, 17, 0.5f);
  std::vector<float> y[3];
  for (int n = 4; n <= 6; ++n) {
    Setting st;
    st.stages = n;
    Fx fx;
    NEED(fx.ok);
    fx.set(st);
    y[n - 4] = fx.run(x);
  }
  CHECK(std::memcmp(y[0].data(), y[1].data(), y[0].size() * sizeof(float)) != 0);
  CHECK(std::memcmp(y[1].data(), y[2].data(), y[0].size() * sizeof(float)) != 0);
  auto with = [&](float v) {
    Fx fx;
    fx.inst.set(PHASER_PARAM_STAGES, v);
    return fx.run(x);
  };
  CHECK(with(4.4f) == y[0]);
  CHECK(with(5.6f) == y[2]);
  CHECK(with(5.0f) == y[1]);
  CHECK(with(-1e30f) == y[0]);
  CHECK(with(1e30f) == y[2]);
  CHECK(with(std::numeric_limits<float>::quiet_NaN()) == y[0]);
}

TEST_CASE("phaser: the Mix and the Centre glide: no step where the same change done at once clicks") {
  const std::size_t n = 8192, at = 2176;  // (at 2176 the wet and dry tracks differ by 0.7 at the switch: a clear step for the at-once reference)
  // Mix 0 to 100 % at the defaults on a sine: the wet-minus-dry difference steps in at once, or ramps in
  {
    Setting st;
    st.mix = 100.0;
    Fx wet;
    NEED(wet.ok);
    wet.set(st);
    const std::vector<float> x = sine(n, 0.5, 440.0);
    const std::vector<float> yw = wet.run(x);
    std::vector<float> once(n);  // the same change at once: the dry track until `at`, then the wet one
    for (std::size_t i = 0; i < n; ++i) {
      once[i] = i < at ? x[i] : yw[i];
    }
    st.mix = 0.0;
    Fx fx;
    NEED(fx.ok);
    fx.set(st);
    std::vector<float> y(n);
    for (std::size_t p = 0; p < n; p += 64) {
      if (p == at) {
        fx.inst.set(PHASER_PARAM_MIX, 100.0f);
      }
      fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
    }
    double jump = 0, jump_ref = 0;
    for (std::size_t i = at - 8; i < at + 800; ++i) {
      jump = std::max(jump, std::fabs(static_cast<double>(y[i + 1]) - 2.0 * y[i] + y[i - 1]));
      jump_ref = std::max(jump_ref, std::fabs(static_cast<double>(once[i + 1]) - 2.0 * once[i] + once[i - 1]));
    }
    MESSAGE("Mix step: largest second difference " << jump << " (at once: " << jump_ref << ")");
    CHECK(jump < 0.02);
    CHECK(jump_ref > 0.3);
    CHECK(jump_ref > 5.0 * jump);
    // and it gets there: the end is the wet track
    double e = 0;
    for (std::size_t i = n - 512; i < n; ++i) {
      e = std::max(e, std::fabs(static_cast<double>(y[i]) - yw[i]));
    }
    CHECK(e < 1e-6);
  }
  // Centre 200 to 3000 Hz at once on a 440 Hz sine, Mix 100 %: the coefficients ramp through the block, so there is no step in the output (the sine's own second difference is 0.002; measured 0.014)
  {
    Setting st;
    st.depth = 0.0;
    st.fb = 0.0;
    st.mix = 100.0;
    st.centre = 200.0;
    const std::vector<float> x = sine(n, 0.5, 440.0);
    Fx fx;
    NEED(fx.ok);
    fx.set(st);
    std::vector<float> y(n);
    for (std::size_t p = 0; p < n; p += 64) {
      if (p == at) {
        fx.inst.set(PHASER_PARAM_CENTRE, 3000.0f);
      }
      fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
    }
    double jump = 0;
    for (std::size_t i = at - 8; i < at + 1500; ++i) {
      jump = std::max(jump, std::fabs(static_cast<double>(y[i + 1]) - 2.0 * y[i] + y[i - 1]));
    }
    MESSAGE("Centre step: largest second difference " << jump);
    CHECK(jump < 0.03);
    CHECK(all_finite(y));
  }
}

TEST_CASE("phaser: set_param before the first block snaps every value, after it glides; Stages and Period act at once") {
  const std::vector<float> x = noise(512, 21, 1.0f);
  Setting st;
  st.centre = 1500.0;
  st.fb = -40.0;
  st.mix = 60.0;
  st.depth = 30.0;
  Fx a;
  a.set(st);
  Fx b;  // the same values reached through another first value: the last one set before the first block is where it starts
  b.inst.set(PHASER_PARAM_MIX, 100.0f);
  b.inst.set(PHASER_PARAM_CENTRE, 300.0f);
  b.set(st);
  const std::vector<float> ya = a.run(x), yb = b.run(x);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  Fx c;
  c.set(st);
  (void)c.run(std::vector<float>(64, 0.0f));
  c.inst.set(PHASER_PARAM_MIX, 100.0f);
  c.inst.set(PHASER_PARAM_CENTRE, 300.0f);
  const std::vector<float> yc = c.run(x);
  CHECK(std::memcmp(ya.data(), yc.data(), ya.size() * sizeof(float)) != 0);  // after a block they glide: different from the snapped start
  // Period at once: a different period gives a different sweep from the first block that follows
  Fx d;
  d.set(st);
  (void)d.run(std::vector<float>(64, 0.0f));
  Fx e;
  e.set(st);
  (void)e.run(std::vector<float>(64, 0.0f));
  e.inst.set(PHASER_PARAM_PERIOD, 100.0f);
  const std::vector<float> xl = noise(4096, 22, 0.5f);
  CHECK(d.run(xl) != e.run(xl));
}

TEST_CASE("phaser: the view draws the notch curve and the stage count, under the budgets") {
  Lib lib;
  NEED(lib.open(O3M_PHASER_HOST_SO));
  Instance inst;
  NEED(inst.start(*lib.entry));
  ViewHost views;
  auto draw = [&](int stages, double centre, double fb, double mix, int w, int h, Drawing& d) {
    float p[PHASER_PARAM_COUNT];
    for (std::uint32_t i = 0; i < PHASER_PARAM_COUNT; ++i) {
      p[i] = phaser_params[i].def;
    }
    p[PHASER_PARAM_STAGES] = static_cast<float>(stages);
    p[PHASER_PARAM_CENTRE] = static_cast<float>(centre);
    p[PHASER_PARAM_FEEDBACK] = static_cast<float>(fb);
    p[PHASER_PARAM_MIX] = static_cast<float>(mix);
    const ViewStatus s = views.draw(*lib.entry, inst.state, 1, 0, w, h, p, PHASER_PARAM_COUNT);
    views.replay(d);
    return s;
  };
  for (int stages = 4; stages <= 6; ++stages) {
    Drawing d;
    CAPTURE(stages);
    NEED(draw(stages, 800.0, 35.0, 50.0, 380, 140, d) == ViewStatus::Ok);
    NEED(d.polys.size() == 1u);
    NEED(d.polys[0].xy.size() == 2u * 65u);
    NEED(d.texts.size() == 1u);
    CHECK(d.texts[0].s == std::to_string(stages) + " STAGES");
    const std::int16_t* xy = d.polys[0].xy.data();
    for (std::uint32_t i = 0; i < 65u; ++i) {
      CHECK(xy[2 * i] >= 0);
      CHECK(xy[2 * i] < 380);
      CHECK(xy[2 * i + 1] >= 2);  // power 1 is at y = 2: never above it
      CHECK(xy[2 * i + 1] < 140);
    }
  }
  // no mix, or all wet with no feedback: an all-pass response is flat: the curve sits on the top line
  for (double mix : {0.0, 100.0}) {
    Drawing d;
    NEED(draw(4, 800.0, 0.0, mix, 380, 140, d) == ViewStatus::Ok);
    NEED(d.polys.size() == 1u);
    for (std::uint32_t i = 0; i < 65u; ++i) {
      CHECK(d.polys[0].xy[2 * i + 1] <= 3);
    }
  }
  // 4 stages at Mix 50 %, no feedback: the notches. The exact response has its notches at the axis frequencies the curve is sampled on (20 Hz * 1.097^i): the curve's points nearest to them sit in the bottom fifth
  {
    Drawing d;
    NEED(draw(4, 800.0, 0.0, 50.0, 380, 140, d) == ViewStatus::Ok);
    NEED(d.polys.size() == 1u);
    Setting st;
    st.depth = 0.0;
    st.fb = 0.0;
    int found = 0;
    for (std::uint32_t i = 0; i < 65u; ++i) {
      const double f = 20.0 * std::pow(1.0970, static_cast<double>(i));
      if (f > 7400.0) {
        break;
      }
      const double g = exact_gain(st, f);
      if (g < 0.2) {  // the response is under 0.2 here (power under 0.04): the curve must show it low
        ++found;
        CHECK(d.polys[0].xy[2 * i + 1] >= 2 + (140 - 5) * 90 / 100);
      } else if (g > 0.95) {
        CHECK(d.polys[0].xy[2 * i + 1] <= 2 + (140 - 5) * 10 / 100);
      }
    }
    CHECK(found >= 4);  // two notches, a few points each
  }
  // the minimum size the manifest asks for also draws
  Drawing d;
  CHECK(draw(4, 800.0, 35.0, 50.0, 120, 80, d) == ViewStatus::Ok);
}

TEST_CASE("phaser: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_PHASER_TMOD);
  if (!t.present) {
    MESSAGE("phaser.tmod is not present in this build: the packed-file checks are skipped");
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_EFFECT);
  CHECK(t.h.abi_minor == 2u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == PHASER_PARAM_COUNT);
  CHECK(t.h.view_count == PHASER_VIEW_COUNT);
  CHECK(t.h.state_bytes <= PHASER_STATE_BYTES);
  CHECK(t.h.hot_size <= 2048u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: the skill's ceiling is 2.5 KB (DRIVE's hot part is 1,328 B, CRUSH's 720 B)
  MESSAGE("phaser.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
