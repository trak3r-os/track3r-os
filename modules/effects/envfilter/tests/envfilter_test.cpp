// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): the ENV FILTER effect module driven through the ABI as the OS drives an insert, against a double-precision reference of the same maths (the trapezoidal
// state-variable filter and the one-pole follower with exact tan, log2 and exp2, the same 16-sample control grid): the sample-by-sample error over a grid of settings, the frequency response of
// the three modes against the analytic one, the level law at the defaults (the output rms never above the input's, DRIVE's lesson, sines and noise from -60 to -6 dBFS, in every mode), the
// follower's step response read back through the filter (the attack and the release times), the sweep's direction, stability at the largest resonance (the poles of the discrete system, a
// bound from the impulse response for a static filter, a long run with the cutoff slammed from end to end, the tail), NaN freedom, Mix 0 as the dry track bit for bit, silence and the tail
// contract, block-size independence, the smoothing, the view and the packed file through the loader.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-205..208).
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

#include "envfilter_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;

struct Set {  // the eight parameters, in the module's order (the defaults of module.json)
  int mode = 0;
  double cutoff = 400.0, range = 4.0, res = 0.0, attack = 5.0, release = 150.0, sens = 12.0, mix = 100.0;
};

// ---- the reference: the same maths in double with the exact functions -------------------------------------------------------------------------------------------------------------------------
// One call per block of any size whose chunks (16 samples from the start) the test aligns with: the module's control step runs at the start of each chunk, with the follower's value at that point.
std::vector<double> ref_run(const Set& p, const std::vector<float>& x) {
  const double lbase = std::log2(p.cutoff);
  const double k = std::sqrt(2.0) * std::exp2(-p.res * 0.01 * std::log2(10.0 * std::sqrt(2.0)));
  const double ls = p.sens * std::log2(10.0) / 20.0;
  const double wx = p.mode == 2 ? 1.0 : 0.0;
  const double wb = p.mode == 1 ? 1.0 : (p.mode == 2 ? -1.0 : 0.0);
  const double wl = p.mode == 0 ? 1.0 : (p.mode == 2 ? -1.0 : 0.0);
  const double ca = 1.0 - std::exp(-1.0 / (p.attack * 1e-3 * kFs));
  const double cr = 1.0 - std::exp(-1.0 / (p.release * 1e-3 * kFs));
  const double m = p.mix * 0.01;
  const double cA = 1.0 - m + m * wx, cB = m * wb * k, cC = m * wl;
  const double lmin = std::log2(20.0), lmax = std::log2(0.4 * kFs);
  double ic1 = 0.0, ic2 = 0.0, e = 0.0;
  std::vector<double> y(x.size());
  for (std::size_t j = 0; j < x.size(); j += 16) {
    const double u = e > 0.0 ? std::min(1.0, std::max(0.0, 1.0 + (std::log2(e) + ls) / 8.0)) : 0.0;
    const double l = std::min(lmax, std::max(lmin, lbase + p.range * u));
    const double g = std::tan(kPi * std::exp2(l) / kFs);
    const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    for (std::size_t i = j; i < std::min(x.size(), j + 16); ++i) {
      const double xi = x[i];
      const double v3 = xi - ic2;
      const double v1 = a1 * ic1 + a2 * v3;
      const double v2 = ic2 + a2 * ic1 + a3 * v3;
      ic1 = 2.0 * v1 - ic1;
      ic2 = 2.0 * v2 - ic2;
      y[i] = cA * xi + cB * v1 + cC * v2;
      const double d = std::fabs(xi) - e;
      e += (d > 0.0 ? ca : cr) * d;
    }
  }
  return y;
}

// The analytic response of a static filter (cutoff fc, mode, resonance, mix) at f: the bilinear map puts s = j r, r = tan(pi f / fs) / tan(pi fc / fs).
double ref_gain(const Set& p, double fc, double f) {
  using C = std::complex<double>;
  const double k = std::sqrt(2.0) * std::exp2(-p.res * 0.01 * std::log2(10.0 * std::sqrt(2.0)));
  const double r = std::tan(kPi * f / kFs) / std::tan(kPi * fc / kFs);
  const C d(1.0 - r * r, k * r);
  const C h = p.mode == 0 ? C(1.0, 0.0) / d : (p.mode == 1 ? C(0.0, k * r) / d : C(-r * r, 0.0) / d);
  const double m = p.mix * 0.01;
  return std::abs((1.0 - m) + m * h);
}

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fx() { ok = lib.open(O3M_ENVFILTER_HOST_SO) && inst.start(*lib.entry); }
  void set(const Set& p) {
    inst.set(ENVFILTER_PARAM_MODE, static_cast<float>(p.mode));
    inst.set(ENVFILTER_PARAM_CUTOFF, static_cast<float>(p.cutoff));
    inst.set(ENVFILTER_PARAM_RANGE, static_cast<float>(p.range));
    inst.set(ENVFILTER_PARAM_RESONANCE, static_cast<float>(p.res));
    inst.set(ENVFILTER_PARAM_ATTACK, static_cast<float>(p.attack));
    inst.set(ENVFILTER_PARAM_RELEASE, static_cast<float>(p.release));
    inst.set(ENVFILTER_PARAM_SENS, static_cast<float>(p.sens));
    inst.set(ENVFILTER_PARAM_MIX, static_cast<float>(p.mix));
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

// Noise whose level alternates between loud and quiet every 1024 samples, so that the follower and the cutoff move.
std::vector<float> bursty(std::size_t n, std::uint32_t seed) {
  std::vector<float> x = noise(n, seed, 1.0f);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] *= ((i / 1024u) % 2u == 0u) ? 0.9f : 0.03f;
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

double rms(const std::vector<float>& v) {
  double s = 0;
  for (float x : v) {
    s += static_cast<double>(x) * x;
  }
  return std::sqrt(s / static_cast<double>(v.size()));
}

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-30)); }

// The amplitude of the component at f in y[i0, i0 + n), by a Hann-windowed quadrature correlation (no leakage worth speaking of for windows of a few periods).
double amp_at(const std::vector<float>& y, double f, std::size_t i0, std::size_t n) {
  double sa = 0, sb = 0, sw = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n));
    const double ph = 2.0 * kPi * f * static_cast<double>(i0 + i) / kFs;
    sa += w * y[i0 + i] * std::sin(ph);
    sb += w * y[i0 + i] * std::cos(ph);
    sw += w;
  }
  return 2.0 * std::hypot(sa, sb) / sw;
}

// The L1 norm of the impulse response of the static filter (Range 0, so the cutoff cannot move), the exact bound of |y| for |x| <= 1.
double l1_norm(int mode, double cutoff, double res) {
  Fx fx;
  Set p;
  p.mode = mode;
  p.cutoff = cutoff;
  p.range = 0.0;
  p.res = res;
  fx.set(p);
  std::vector<float> x(static_cast<std::size_t>(4.0 * kFs), 0.0f);
  x[0] = 1.0f;
  const std::vector<float> y = fx.run(x);
  double s = 0.0;
  for (float v : y) {
    s += std::fabs(static_cast<double>(v));
  }
  return s;
}

// The largest L1 norm over the modes and a grid of cutoffs at the largest resonance (computed once).
double worst_l1() {
  static double w = 0.0;
  if (w == 0.0) {
    for (int mode = 0; mode < 3; ++mode) {
      for (double fc : {20.0, 60.0, 200.0, 800.0, 3000.0, 10000.0, 17000.0}) {
        w = std::max(w, l1_norm(mode, fc, 100.0));
      }
    }
  }
  return w;
}

}  // namespace

TEST_CASE("envfilter: the descriptor says an effect with eight parameters and a view") {
  Lib lib;
  NEED(lib.open(O3M_ENVFILTER_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == 8u);
  CHECK(ENVFILTER_PARAM_MODE == 0);
  CHECK(ENVFILTER_PARAM_CUTOFF == 1);  // the first modulation target: the unit bench's moving row sweeps it
  CHECK(ENVFILTER_PARAM_RANGE == 2);
  CHECK(ENVFILTER_PARAM_RESONANCE == 3);
  CHECK(ENVFILTER_PARAM_ATTACK == 4);
  CHECK(ENVFILTER_PARAM_RELEASE == 5);
  CHECK(ENVFILTER_PARAM_SENS == 6);
  CHECK(ENVFILTER_PARAM_MIX == 7);
  CHECK((m.params[ENVFILTER_PARAM_MODE].flags & O3M_PARAM_INT) != 0u);
  CHECK(m.params[ENVFILTER_PARAM_MODE].min == 0.0f);
  CHECK(m.params[ENVFILTER_PARAM_MODE].max == 2.0f);
  CHECK(m.params[ENVFILTER_PARAM_MODE].def == 0.0f);  // low-pass
  CHECK((m.params[ENVFILTER_PARAM_CUTOFF].flags & O3M_PARAM_LOG) != 0u);
  CHECK(O3M_PARAM_UNIT(m.params[ENVFILTER_PARAM_CUTOFF].flags) == O3M_UNIT_HZ);
  CHECK(m.params[ENVFILTER_PARAM_CUTOFF].min == 20.0f);
  CHECK(m.params[ENVFILTER_PARAM_CUTOFF].max == 20000.0f);
  CHECK(m.params[ENVFILTER_PARAM_RANGE].min == -6.0f);  // a range under 0 sweeps down
  CHECK(m.params[ENVFILTER_PARAM_RANGE].max == 6.0f);
  CHECK(m.params[ENVFILTER_PARAM_RESONANCE].def == 0.0f);  // the Butterworth: the defaults cannot be hotter than bypass
  CHECK(O3M_PARAM_UNIT(m.params[ENVFILTER_PARAM_ATTACK].flags) == O3M_UNIT_MS);
  CHECK(m.params[ENVFILTER_PARAM_ATTACK].min == 0.5f);
  CHECK(O3M_PARAM_UNIT(m.params[ENVFILTER_PARAM_RELEASE].flags) == O3M_UNIT_MS);
  CHECK(m.params[ENVFILTER_PARAM_RELEASE].max == 2000.0f);
  CHECK(O3M_PARAM_UNIT(m.params[ENVFILTER_PARAM_SENS].flags) == O3M_UNIT_DB);
  CHECK(O3M_PARAM_UNIT(m.params[ENVFILTER_PARAM_MIX].flags) == O3M_UNIT_PERCENT);
  CHECK(m.params[ENVFILTER_PARAM_MIX].def == 100.0f);
  for (std::uint32_t i = 0; i < 8u; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);  // MIDI Learn may bind each of them
  }
  CHECK((m.params[ENVFILTER_PARAM_MODE].flags & O3M_PARAM_MOD) == 0u);
  CHECK((m.params[ENVFILTER_PARAM_ATTACK].flags & O3M_PARAM_MOD) == 0u);
  CHECK((m.params[ENVFILTER_PARAM_RELEASE].flags & O3M_PARAM_MOD) == 0u);
  for (std::uint32_t i : {1u, 2u, 3u, 6u, 7u}) {
    CHECK((m.params[i].flags & O3M_PARAM_MOD) != 0u);  // a modulation target: set_param stores and evaluates a polynomial at most
  }
  CHECK(m.view_count == 1u);
}

TEST_CASE("envfilter: the output tracks a double-precision reference over a grid of settings") {
  const std::vector<float> x = bursty(8192, 7);  // loud and quiet halves: the follower sweeps the cutoff through the window
  const double combos[3][3] = {{0.5, 5.0, 0.0}, {5.0, 150.0, 12.0}, {50.0, 800.0, 30.0}};  // attack ms, release ms, sens dB
  double worst_err = 0.0, worst_snr = 1e9;
  int idx = 0, runs = 0;
  for (int mode = 0; mode < 3; ++mode) {
    for (double cutoff : {80.0, 1000.0, 9000.0}) {
      for (double range : {-4.0, 0.0, 4.0}) {
        for (double res : {0.0, 60.0, 100.0}) {
          for (double mix : {100.0, 40.0}) {
            Set p;
            p.mode = mode;
            p.cutoff = cutoff;
            p.range = range;
            p.res = res;
            p.mix = mix;
            const double* c = combos[idx++ % 3];
            p.attack = c[0];
            p.release = c[1];
            p.sens = c[2];
            Fx fx;
            NEED(fx.ok);
            fx.set(p);
            const std::vector<float> y = fx.run(x);
            const std::vector<double> ref = ref_run(p, x);
            double es = 0, ee = 0, err = 0, peak = 1.0;
            for (std::size_t i = 0; i < x.size(); ++i) {
              const double e = static_cast<double>(y[i]) - ref[i];
              es += ref[i] * ref[i];
              ee += e * e;
              err = std::max(err, std::fabs(e));
              peak = std::max(peak, std::fabs(ref[i]));
            }
            CAPTURE(mode);
            CAPTURE(cutoff);
            CAPTURE(range);
            CAPTURE(res);
            CAPTURE(mix);
            CHECK(err < 5e-4 * peak);  // relative to the peak (Q 10 outputs reach several units)
            const double snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
            CHECK_MESSAGE(snr > 70.0, "snr " << snr);
            worst_err = std::max(worst_err, err);
            worst_snr = std::min(worst_snr, snr);
            ++runs;
          }
        }
      }
    }
  }
  MESSAGE("worst max error " << worst_err << " (limit 5e-4 of the peak, at least 1), worst SNR " << worst_snr << " dB (limit 70) over " << runs << " settings");
}

TEST_CASE("envfilter: a static filter has the analytic response of each mode, at a cutoff and a resonance") {
  double worst = 0.0;
  for (int mode = 0; mode < 3; ++mode) {
    for (double res : {0.0, 60.0}) {
      for (double fc : {300.0, 2500.0}) {
        Set p;
        p.mode = mode;
        p.cutoff = fc;
        p.range = 0.0;  // the cutoff cannot move
        p.res = res;
        for (double f : {60.0, 200.0, 700.0, 1400.0, 2600.0, 5000.0, 9000.0, 14000.0}) {
          Fx fx;
          NEED(fx.ok);
          fx.set(p);
          const std::vector<float> y = fx.run(sine(49152, 0.25, f));
          const double want = ref_gain(p, fc, f);
          const double got = amp_at(y, f, 16384, 32768) / 0.25;
          CAPTURE(mode);
          CAPTURE(res);
          CAPTURE(fc);
          CAPTURE(f);
          if (want > 0.003) {  // under -50 dB the float state's noise and the window leak count
            CHECK(std::fabs(db(got) - db(want)) < 0.05);
            worst = std::max(worst, std::fabs(db(got) - db(want)));
          }
        }
      }
    }
  }
  MESSAGE("worst deviation from the analytic response " << worst << " dB (limit 0.05)");
  // the shapes in words: the low-pass passes the low end and stops the high one, the high-pass the reverse, the band-pass peaks at 0 dB at its cutoff whatever the resonance
  Set lp;
  lp.range = 0.0;
  lp.cutoff = 1000.0;
  CHECK(db(ref_gain(lp, 1000.0, 100.0)) > -0.01);
  CHECK(db(ref_gain(lp, 1000.0, 12000.0)) < -35.0);
  Set bp = lp;
  bp.mode = 1;
  bp.res = 100.0;
  CHECK(std::fabs(db(ref_gain(bp, 1000.0, 1000.0))) < 1e-9);
  Set hp = lp;
  hp.mode = 2;
  CHECK(db(ref_gain(hp, 1000.0, 12000.0)) > -0.01);
  CHECK(db(ref_gain(hp, 1000.0, 100.0)) < -35.0);
}

TEST_CASE("envfilter: the level law: at the defaults, and in every mode at resonance 0, the output rms is never above the input's") {
  // The lesson of DRIVE (+12..16 dB over bypass at its defaults). The defaults (low-pass, Cutoff 400 Hz, Range +4 octaves, Resonance 0, Attack 5 ms, Release 150 ms, Sens +12 dB, Mix 100 %) are
  // a Butterworth swept by the follower: |H| <= 1 at every frequency, so a sine or noise from -60 to -6 dBFS cannot come out louder. Whole runs from rest, the sweep's transients included.
  double worst = -1e9;
  const double freqs[] = {100.0, 440.0, 1234.5, 3000.0};
  for (int mode = 0; mode < 3; ++mode) {
    for (int dbfs = -60; dbfs <= -6; dbfs += 2) {
      const double amp = std::pow(10.0, dbfs / 20.0);
      for (double f : freqs) {
        const std::vector<float> x = sine(22050, amp, f);
        Fx fx;
        NEED(fx.ok);
        if (mode != 0) {
          fx.inst.set(ENVFILTER_PARAM_MODE, static_cast<float>(mode));
        }
        const std::vector<float> y = fx.run(x);
        const double d = db(rms(y) / rms(x));
        CAPTURE(mode);
        CAPTURE(dbfs);
        CAPTURE(f);
        CHECK(d <= 0.005);
        worst = std::max(worst, d);
      }
      const std::vector<float> n = noise(22050, 5u + static_cast<std::uint32_t>(dbfs + 100), static_cast<float>(amp));
      Fx fx;
      NEED(fx.ok);
      if (mode != 0) {
        fx.inst.set(ENVFILTER_PARAM_MODE, static_cast<float>(mode));
      }
      const std::vector<float> y = fx.run(n);
      const double d = db(rms(y) / rms(n));
      CAPTURE(mode);
      CAPTURE(dbfs);
      CHECK(d <= 0.005);
      worst = std::max(worst, d);
    }
  }
  MESSAGE("worst level against bypass at the defaults: " << worst << " dB (limit +0.005)");
  // a full-scale square-ish burst train and a step also stay under (the transients of the sweep add nothing)
  Fx fx;
  NEED(fx.ok);
  const std::vector<float> b = bursty(44100, 3);
  CHECK(db(rms(fx.run(b)) / rms(b)) <= 0.005);
}

TEST_CASE("envfilter: the follower tracks a step in the set attack, and falls in the set release (read back through the filter)") {
  // A DC step plus a small probe tone: the follower sees |DC + probe| = DC + probe, a clean one-pole step toward DC. The filter is a low-pass at Res 0 with Sens 0 dB, so the cutoff
  // is Cutoff 2^(Range u) with u = 1 + log2(e) / 8; DC passes the low-pass at exactly 1 whatever the cutoff, the probe's gain says the cutoff, and the cutoff says e. The probe's period is an
  // integer number of samples and the correlation window two periods: DC and the transients leave no trace in it.
  const double f0 = kFs / 30.0;
  const double base = 300.0, range = 4.0;
  auto invert = [&](double a) {  // the follower value that puts the Butterworth's gain at f0 at a
    a = std::min(std::max(a, 1e-4), 0.9999);
    const double g = std::tan(kPi * f0 / kFs) / std::pow(1.0 / (a * a) - 1.0, 0.25);
    const double fc = std::atan(g) * kFs / kPi;
    const double u = (std::log2(fc) - std::log2(base)) / range;
    return std::exp2(8.0 * (u - 1.0));
  };
  auto probe_e = [&](const std::vector<float>& y, std::size_t centre, double p) {
    const std::size_t i0 = centre - 30;
    double sa = 0, sb = 0;
    for (std::size_t i = 0; i < 60; ++i) {
      const double ph = 2.0 * kPi * f0 * static_cast<double>(i0 + i) / kFs;
      sa += y[i0 + i] * std::sin(ph);
      sb += y[i0 + i] * std::cos(ph);
    }
    return invert(2.0 * std::hypot(sa, sb) / 60.0 / p);
  };
  double worst = 0.0;
  // ---- the attack: a step from silence to 0.4 --------------------------------------------------------------------------------------------------------------------------------------------
  for (double attack : {10.0, 50.0}) {
    const double tau = attack * 1e-3 * kFs;  // samples
    const double p = 0.02, e_end = 0.4;
    std::vector<float> x(static_cast<std::size_t>(0.4 * kFs));
    for (std::size_t i = 0; i < x.size(); ++i) {
      x[i] = static_cast<float>(e_end + p * std::sin(2.0 * kPi * f0 * static_cast<double>(i) / kFs));
    }
    Fx fx;
    NEED(fx.ok);
    Set s;
    s.cutoff = base;
    s.range = range;
    s.attack = attack;
    s.release = 500.0;
    s.sens = 0.0;
    fx.set(s);
    const std::vector<float> y = fx.run(x);
    double t63 = -1.0;
    for (double k = 0.5; k <= 3.5; k += 0.25) {
      const std::size_t centre = static_cast<std::size_t>(k * tau);
      const double got = probe_e(y, centre, p);
      const double want = e_end * (1.0 - std::exp(-static_cast<double>(centre) / tau));
      CAPTURE(attack);
      CAPTURE(k);
      CHECK(std::fabs(got - want) < 0.02);
      worst = std::max(worst, std::fabs(got - want));
      if (t63 < 0.0 && got >= 0.632 * e_end) {
        t63 = static_cast<double>(centre) / tau;
      }
    }
    MESSAGE("attack " << attack << " ms: the follower reached 63.2 % of the step at " << t63 << " attacks");
    CHECK(t63 >= 0.75);  // the follower covers 63 % of a step in one attack (this reads it at quarter attacks, inside the filter's own lag and the control step's 0.4 ms)
    CHECK(t63 <= 1.25);
  }
  // ---- the release: from a DC of 0.4 down to 0.1 -----------------------------------------------------------------------------------------------------------------------------------------
  for (double release : {50.0, 200.0}) {
    const double tau = release * 1e-3 * kFs;
    const double p = 0.01;
    const std::size_t t0 = static_cast<std::size_t>(0.3 * kFs);
    std::vector<float> x(t0 + static_cast<std::size_t>(0.9 * kFs));
    for (std::size_t i = 0; i < x.size(); ++i) {
      x[i] = static_cast<float>((i < t0 ? 0.4 : 0.1) + p * std::sin(2.0 * kPi * f0 * static_cast<double>(i) / kFs));
    }
    Fx fx;
    NEED(fx.ok);
    Set s;
    s.cutoff = base;
    s.range = range;
    s.attack = 5.0;
    s.release = release;
    s.sens = 0.0;
    fx.set(s);
    const std::vector<float> y = fx.run(x);
    for (double k = 0.5; k <= 3.0; k += 0.25) {
      const std::size_t centre = t0 + static_cast<std::size_t>(k * tau);
      const double got = probe_e(y, centre, p);
      const double want = 0.1 + 0.3 * std::exp(-static_cast<double>(centre - t0) / tau);
      CAPTURE(release);
      CAPTURE(k);
      CHECK(std::fabs(got - want) < 0.02);
      worst = std::max(worst, std::fabs(got - want));
    }
  }
  MESSAGE("worst deviation of the follower from the one-pole step, read through the filter: " << worst << " (limit 0.02 of a 0.4 step)");
}

TEST_CASE("envfilter: the sweep goes the way Range says: up opens the filter on a loud signal, down closes it") {
  // A 3 kHz sine, quiet (-50 dBFS) for 0.3 s and loud (-10 dBFS) for 0.3 s: the low-pass's gain on it in the last 0.1 s of each.
  auto gains = [&](double cutoff, double range, double& quiet, double& loud) {
    std::vector<float> x = sine(static_cast<std::size_t>(0.6 * kFs), 1.0, 3000.0);
    const std::size_t half = x.size() / 2;
    for (std::size_t i = 0; i < x.size(); ++i) {
      x[i] *= i < half ? 0.00316f : 0.316f;
    }
    Fx fx;
    NEED(fx.ok);
    Set s;
    s.cutoff = cutoff;
    s.range = range;
    s.sens = 12.0;
    fx.set(s);
    const std::vector<float> y = fx.run(x);
    const std::size_t w = static_cast<std::size_t>(0.1 * kFs);
    quiet = amp_at(y, 3000.0, half - w, w) / 0.00316;
    loud = amp_at(y, 3000.0, x.size() - w, w) / 0.316;
  };
  double q = 0, l = 0;
  gains(600.0, 4.0, q, l);
  MESSAGE("Range +4: quiet " << db(q) << " dB, loud " << db(l) << " dB");
  CHECK(db(l) > db(q) + 10.0);
  gains(9600.0, -4.0, q, l);
  MESSAGE("Range -4: quiet " << db(q) << " dB, loud " << db(l) << " dB");
  CHECK(db(l) < db(q) - 10.0);
  gains(1500.0, 0.0, q, l);
  MESSAGE("Range 0: quiet " << db(q) << " dB, loud " << db(l) << " dB");
  CHECK(std::fabs(db(l) - db(q)) < 0.05);  // no sweep: the same filter at both levels
}

TEST_CASE("envfilter: Mix 0 is the input bit for bit, also after a wet period") {
  const std::vector<float> x = noise(4096, 11);
  Fx dry;
  NEED(dry.ok);
  Set p;
  p.mix = 0.0;
  dry.set(p);
  const std::vector<float> yd = dry.run(x);
  CHECK(std::memcmp(yd.data(), x.data(), x.size() * sizeof(float)) == 0);

  // wet first, then Mix 0: the mix glides to 0 (about 110 control steps, then it snaps), and from then on the output is the input bit for bit
  Fx fx;
  NEED(fx.ok);
  p.mix = 100.0;
  fx.set(p);
  (void)fx.run(noise(4096, 3, 0.5f));
  fx.inst.set(ENVFILTER_PARAM_MIX, 0.0f);
  const std::vector<float> in = noise(64 * 400, 9, 0.25f);
  std::vector<float> o(in.size());
  for (std::size_t at = 0; at < in.size(); at += 64) {
    fx.inst.m->render(fx.inst.state, in.data() + at, o.data() + at, 64);
  }
  CHECK(std::memcmp(o.data() + 64 * 300, in.data() + 64 * 300, 64 * 100 * sizeof(float)) == 0);  // the last 100 blocks are the input
  CHECK(std::memcmp(o.data(), in.data(), 64 * 4 * sizeof(float)) != 0);                           // the first ones are not: it glides
}

TEST_CASE("envfilter: silence in gives silence out, and the tail ends: 1 while the input is live or the filter rings, then 0 with exact zeros") {
  Fx fx;
  NEED(fx.ok);
  const std::vector<float> zeros(64 * 20, 0.0f);
  for (float v : fx.run(zeros)) {
    CHECK(v == 0.0f);
  }
  CHECK(fx.busy == 0u);
  // a live input is busy; the sound stops, the filter rings and then goes idle
  Set p;
  p.res = 100.0;
  p.cutoff = 100.0;
  p.range = 0.0;
  fx.set(p);
  (void)fx.run(noise(4096, 5, 0.5f));
  CHECK(fx.busy == 1u);
  int idle_after = -1;
  std::vector<float> tail;
  for (int b = 0; b < 4000 && idle_after < 0; ++b) {
    const std::vector<float> y = fx.run(std::vector<float>(64, 0.0f));
    tail.insert(tail.end(), y.begin(), y.end());
    if (fx.busy == 0u) {
      idle_after = b;
    }
  }
  NEED(idle_after >= 0);
  MESSAGE("the resonant filter at 100 Hz, Q 10, went idle " << idle_after << " blocks (" << 64.0 * idle_after / kFs << " s) after the input stopped");
  CHECK(idle_after > 2);  // it does ring
  for (int b = 0; b < 50; ++b) {  // and from then on it is exact zeros, busy 0
    for (float v : fx.run(std::vector<float>(64, 0.0f))) {
      CHECK(v == 0.0f);
    }
    CHECK(fx.busy == 0u);
  }
  // the follower forgot too: the same burst again gives the same bits as on a fresh instance
  Fx fresh;
  NEED(fresh.ok);
  fresh.set(p);
  const std::vector<float> burst = noise(512, 17, 0.6f);
  const std::vector<float> ya = fx.run(burst), yb = fresh.run(burst);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
}

TEST_CASE("envfilter: any block size gives the same bits for a static cutoff, and in == out is the same as two buffers") {
  const std::vector<float> x = noise(1000, 13, 0.8f);
  Set p;
  p.mode = 1;
  p.range = 0.0;  // the cutoff cannot move: the control grid (a block's 16-sample chunks) cannot show
  p.res = 70.0;
  p.cutoff = 900.0;
  Fx ref;
  NEED(ref.ok);
  ref.set(p);
  const std::vector<float> y64 = ref.run(x, 64);
  for (std::uint32_t block : {1u, 7u, 16u, 33u, 100u, 128u}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(p);
    const std::vector<float> y = fx.run(x, block);
    CAPTURE(block);
    CHECK(std::memcmp(y.data(), y64.data(), y.size() * sizeof(float)) == 0);
  }
  // with a sweep, blocks of 64 and 128 share the 16-sample grid and give the same bits
  Set sw;
  sw.range = 3.0;
  Fx a, b;
  NEED(a.ok);
  NEED(b.ok);
  a.set(sw);
  b.set(sw);
  const std::vector<float> bx = bursty(8192, 2);
  const std::vector<float> ya = a.run(bx, 64), yb = b.run(bx, 128);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  // in == out
  Fx c;
  NEED(c.ok);
  c.set(sw);
  std::vector<float> io = bx;
  for (std::size_t at = 0; at < io.size(); at += 64) {
    c.inst.m->render(c.inst.state, io.data() + at, io.data() + at, 64);
  }
  CHECK(std::memcmp(io.data(), ya.data(), ya.size() * sizeof(float)) == 0);
}

TEST_CASE("envfilter: stability: the poles are inside the unit circle at every cutoff and damping the parameters reach") {
  // The state update with x = 0 is ic' = M ic with M = [[2 a1 - 1, -2 a2], [2 a2, 1 - 2 a3]] (the module's own formulas in double). Both eigenvalues are inside the unit circle iff
  // |det| < 1 and |trace| < 1 + det.
  double worst_radius = 0.0;
  for (double k : {std::sqrt(2.0), 1.0, 0.5, 0.2, 0.1}) {  // Resonance 0 .. 100 % (k = 1/Q, Q 0.707 .. 10)
    for (int i = 0; i <= 120; ++i) {
      const double fc = 20.0 * std::pow(0.4 * kFs / 20.0, i / 120.0);
      const double g = std::tan(kPi * fc / kFs);
      const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
      const double m00 = 2.0 * a1 - 1.0, m01 = -2.0 * a2, m10 = 2.0 * a2, m11 = 1.0 - 2.0 * a3;
      const double tr = m00 + m11, det = m00 * m11 - m01 * m10;
      CAPTURE(k);
      CAPTURE(fc);
      CHECK(std::fabs(det) < 1.0);
      CHECK(std::fabs(tr) < 1.0 + det);
      const double disc = tr * tr - 4.0 * det;
      const double radius = disc < 0.0 ? std::sqrt(det) : std::max(std::fabs(tr + std::sqrt(disc)), std::fabs(tr - std::sqrt(disc))) / 2.0;
      worst_radius = std::max(worst_radius, radius);
    }
  }
  MESSAGE("largest pole radius over the grid: " << worst_radius << " (below 1)");
  CHECK(worst_radius < 1.0);
}

TEST_CASE("envfilter: a static filter at the largest resonance stays under the L1 bound of its impulse response, over a long run, and rings out") {
  // For Range 0 the filter is linear and time-invariant: |y| <= ||h||_1 max|x|, exactly. Full-scale noise and a full-scale square at the resonance for 8 s per setting, every mode.
  double worst_ratio = 0.0;
  for (int mode = 0; mode < 3; ++mode) {
    for (double fc : {20.0, 200.0, 3000.0, 17000.0}) {
      const double bound = l1_norm(mode, fc, 100.0);
      Set p;
      p.mode = mode;
      p.cutoff = fc;
      p.range = 0.0;
      p.res = 100.0;
      Fx fx;
      NEED(fx.ok);
      fx.set(p);
      const std::size_t n = static_cast<std::size_t>(8.0 * kFs);
      std::vector<float> x = noise(n, 31, 1.0f);
      const std::size_t half = std::max<std::size_t>(2, static_cast<std::size_t>(kFs / std::min(fc, 0.4 * kFs) / 2.0));  // a square at the cutoff, half a period each
      for (std::size_t i = n / 2; i < n; ++i) {
        x[i] = ((i / half) % 2u == 0u) ? 1.0f : -1.0f;
      }
      const std::vector<float> y = fx.run(x);
      double peak = 0.0;
      for (float v : y) {
        NEED(std::isfinite(v));
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
      }
      CAPTURE(mode);
      CAPTURE(fc);
      CHECK(peak <= bound * 1.001);
      worst_ratio = std::max(worst_ratio, peak / bound);
      // then the input stops: the filter rings out and the effect goes idle (a 20 Hz Q 10 filter takes about 14 time constants of 0.16 s)
      int idle_after = -1;
      for (int b = 0; b < 4000 && idle_after < 0; ++b) {
        (void)fx.run(std::vector<float>(64, 0.0f));
        if (fx.busy == 0u) {
          idle_after = b;
        }
      }
      CHECK(idle_after >= 0);
    }
  }
  MESSAGE("largest peak over the L1 bound: " << worst_ratio << "; the worst L1 norm at Q 10: " << worst_l1());
}

TEST_CASE("envfilter: the cutoff slammed from end to end at the largest resonance for 20 s stays finite and bounded, and rings out") {
  // Range +-6 octaves, the fastest attack and release, bursts of noise of 48 samples (about 3 chunks): the cutoff changes every control step. Time-varying, so the L1 bound of the static filter
  // is not exact; a factor of 2 over the worst static one is the check, and the run is finite for ever.
  const double bound = 2.0 * worst_l1();
  for (int mode = 0; mode < 3; ++mode) {
    for (double range : {-6.0, 6.0}) {
      Set p;
      p.mode = mode;
      p.cutoff = 800.0;
      p.range = range;
      p.res = 100.0;
      p.attack = 0.5;
      p.release = 5.0;
      p.sens = 12.0;
      Fx fx;
      NEED(fx.ok);
      fx.set(p);
      std::uint32_t s = 99;
      double peak = 0.0;
      const std::size_t blocks = static_cast<std::size_t>(20.0 * kFs / 64.0);
      std::vector<float> x(64), y(64);
      for (std::size_t b = 0; b < blocks; ++b) {
        for (std::size_t i = 0; i < 64; ++i) {
          s = s * 1664525u + 1013904223u;
          const std::size_t pos = b * 64 + i;
          const float amp = ((pos / 48u) % 2u == 0u) ? 1.0f : 0.02f;
          x[i] = amp * (static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.0f);
        }
        fx.inst.m->render(fx.inst.state, x.data(), y.data(), 64);
        for (float v : y) {
          if (!std::isfinite(v)) {
            peak = std::numeric_limits<double>::infinity();
          } else {
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
          }
        }
      }
      CAPTURE(mode);
      CAPTURE(range);
      MESSAGE("mode " << mode << " range " << range << ": peak " << peak << " (bound " << bound << ")");
      CHECK(peak <= bound);
      int idle_after = -1;
      for (int b = 0; b < 4000 && idle_after < 0; ++b) {
        (void)fx.run(std::vector<float>(64, 0.0f));
        if (fx.busy == 0u) {
          idle_after = b;
        }
      }
      CHECK(idle_after >= 0);
    }
  }
}

TEST_CASE("envfilter: parameters thrown about every block stay finite and bounded, and hostile values clamp") {
  Fx fx;
  NEED(fx.ok);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (std::uint32_t id = 0; id < 9; ++id) {  // (id 8 does not exist: ignored)
    for (float v : {nan, inf, -inf, 1e30f, -1e30f}) {
      fx.inst.set(id, v);
    }
  }
  const std::vector<float> n1 = noise(4096, 9, 1.0f);
  for (float v : fx.run(n1)) {
    NEED(std::isfinite(v));
  }
  // random settings every block over 10 s, at every corner of the ranges too
  const double bound = 2.0 * worst_l1();
  std::uint32_t s = 4242;
  auto rnd = [&]() {
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / 16777216.0;
  };
  Fx fy;
  NEED(fy.ok);
  const o3m_module& m = *fy.lib.entry;
  std::vector<float> x(64), y(64);
  double peak = 0.0;
  for (std::size_t b = 0; b < static_cast<std::size_t>(10.0 * kFs / 64.0); ++b) {
    for (std::uint32_t id = 0; id < 8u; ++id) {
      const double u = rnd();
      const double pick = rnd();
      double v = m.params[id].min + u * (m.params[id].max - m.params[id].min);
      if (pick < 0.15) {
        v = m.params[id].min;
      } else if (pick < 0.3) {
        v = m.params[id].max;
      }
      fy.inst.set(id, static_cast<float>(v));
    }
    const float amp = rnd() < 0.5 ? 1.0f : 0.05f;
    for (auto& v : x) {
      v = amp * static_cast<float>(2.0 * rnd() - 1.0);
    }
    m.render(fy.inst.state, x.data(), y.data(), 64);
    for (float v : y) {
      if (!std::isfinite(v)) {
        peak = std::numeric_limits<double>::infinity();
      } else {
        peak = std::max(peak, static_cast<double>(std::fabs(v)));
      }
    }
  }
  MESSAGE("random parameters every block: peak " << peak << " (bound " << bound << ")");
  CHECK(peak <= bound);
}

TEST_CASE("envfilter: a NaN, an infinity or an absurd input sample does not poison the effect for good") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  for (int mode = 0; mode < 3; ++mode) {
    for (float bad : {nan, inf, -inf, 1e30f, -1e30f}) {
      for (double res : {0.0, 100.0}) {
        Fx fx;
        NEED(fx.ok);
        Set p;
        p.mode = mode;
        p.res = res;
        fx.set(p);
        (void)fx.run(noise(256, 4));
        std::vector<float> in = noise(64, 6);
        in[10] = bad;
        std::vector<float> out(64);
        fx.inst.m->render(fx.inst.state, in.data(), out.data(), 64);  // this block may carry it (the filter is recursive)
        const std::vector<float> clean = noise(64 * 8, 8, 0.5f);
        const std::vector<float> y = fx.run(clean);  // the next blocks may not
        CAPTURE(mode);
        CAPTURE(bad);
        CAPTURE(res);
        double peak = 0.0;
        for (float v : y) {
          NEED(std::isfinite(v));
          peak = std::max(peak, static_cast<double>(std::fabs(v)));
        }
        CHECK(peak <= 4.0 * worst_l1() * 0.5);
        // and it is the clean run's bits from rest: the flush put everything back at rest
        Fx fresh;
        NEED(fresh.ok);
        fresh.set(p);
        const std::vector<float> yr = fresh.run(clean);
        CHECK(std::memcmp(y.data(), yr.data(), y.size() * sizeof(float)) == 0);
      }
    }
  }
}

TEST_CASE("envfilter: set_param before the first block snaps, after it glides (Cutoff, Range, Mode, Mix)") {
  const std::vector<float> x = bursty(2048, 21);
  Set a_set;
  a_set.mode = 1;
  a_set.cutoff = 900.0;
  a_set.range = 2.0;
  a_set.res = 40.0;
  a_set.mix = 60.0;
  Fx a;
  NEED(a.ok);
  a.set(a_set);
  Fx b;
  NEED(b.ok);
  Set other;  // set something else first: before any block the last value is the one that counts
  other.mode = 2;
  other.cutoff = 5000.0;
  other.range = -3.0;
  other.res = 90.0;
  other.mix = 10.0;
  b.set(other);
  b.set(a_set);
  const std::vector<float> ya = a.run(x), yb = b.run(x);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  // after a block the same changes are not instantaneous
  Fx c;
  NEED(c.ok);
  c.set(other);
  (void)c.run(std::vector<float>(64, 0.0f));
  c.set(a_set);
  const std::vector<float> yc = c.run(x);
  CHECK(std::memcmp(ya.data(), yc.data(), ya.size() * sizeof(float)) != 0);
  // ... and they arrive: a long while later the glided instance is the snapped one
  Fx d, e;
  NEED(d.ok);
  NEED(e.ok);
  d.set(other);
  (void)d.run(std::vector<float>(64, 0.0f));
  d.set(a_set);
  e.set(a_set);
  const std::vector<float> settle = noise(64 * 2000, 2, 0.3f);  // 3 s: twenty release times, the follower's memory of the glide is gone
  (void)d.run(settle);
  (void)e.run(settle);
  // the filters' memories differ (the states), but the follower and the smoothing are on the same targets: the same quiet input gives the same output once the transient has died
  const std::vector<float> probe = noise(2048, 5, 0.3f);
  const std::vector<float> yd = d.run(probe), ye = e.run(probe);
  double es = 0, ee = 0;
  for (std::size_t i = 512; i < probe.size(); ++i) {
    es += static_cast<double>(ye[i]) * ye[i];
    const double df = static_cast<double>(yd[i]) - ye[i];
    ee += df * df;
  }
  CHECK(10.0 * std::log10(es / std::max(ee, 1e-30)) > 60.0);
  // Mode glides as a cross-fade: the first block after a switch from LP to HP is neither pure
  Fx lp, hp, sw;
  NEED(lp.ok);
  NEED(hp.ok);
  NEED(sw.ok);
  Set sl;
  sl.range = 0.0;
  sl.cutoff = 1000.0;
  lp.set(sl);
  Set sh = sl;
  sh.mode = 2;
  hp.set(sh);
  sw.set(sl);
  const std::vector<float> warm = noise(64 * 20, 1, 0.5f);
  (void)lp.run(warm);
  (void)hp.run(warm);
  (void)sw.run(warm);
  sw.inst.set(ENVFILTER_PARAM_MODE, 2.0f);
  const std::vector<float> nx = noise(16, 3, 0.5f);
  std::vector<float> ylp(16), yhp(16), ysw(16);
  lp.inst.m->render(lp.inst.state, nx.data(), ylp.data(), 16);
  hp.inst.m->render(hp.inst.state, nx.data(), yhp.data(), 16);
  sw.inst.m->render(sw.inst.state, nx.data(), ysw.data(), 16);
  CHECK(std::memcmp(ysw.data(), ylp.data(), 16 * sizeof(float)) != 0);
  CHECK(std::memcmp(ysw.data(), yhp.data(), 16 * sizeof(float)) != 0);
}

TEST_CASE("envfilter: the view draws the response at both ends of the sweep and the mode's name, under the budgets") {
  Lib lib;
  NEED(lib.open(O3M_ENVFILTER_HOST_SO));
  Instance inst;
  NEED(inst.start(*lib.entry));
  ViewHost views;
  const char* const names[3] = {"LOW-PASS", "BAND-PASS", "HIGH-PASS"};
  for (int mode = 0; mode < 3; ++mode) {
    float p[ENVFILTER_PARAM_COUNT];
    for (std::uint32_t i = 0; i < ENVFILTER_PARAM_COUNT; ++i) {
      p[i] = envfilter_params[i].def;
    }
    p[ENVFILTER_PARAM_MODE] = static_cast<float>(mode);
    CAPTURE(mode);
    NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, ENVFILTER_PARAM_COUNT) == ViewStatus::Ok);
    Drawing d;
    views.replay(d);
    NEED(d.curves.size() == 2u);
    NEED(d.curves[0].n == 40u);
    NEED(d.curves[1].n == 40u);
    NEED(d.texts.size() == 1u);
    CHECK(d.texts[0].s == names[mode]);
    for (int c = 0; c < 2; ++c) {
      for (float v : d.curve_y[static_cast<std::size_t>(c)]) {
        CHECK(std::isfinite(v));
        CHECK(v >= -48.0f);
        CHECK(v <= 24.0f);
      }
    }
    // the shape of the base curve (Cutoff 400 Hz): a low-pass is up on the left and down on the right, a high-pass the reverse, a band-pass is up in the middle
    const std::vector<float>& y = d.curve_y[0];
    if (mode == 0) {
      CHECK(y.front() > y.back() + 20.0f);
    } else if (mode == 2) {
      CHECK(y.back() > y.front() + 20.0f);
    } else {
      const float top = *std::max_element(y.begin(), y.end());
      CHECK(top > -1.0f);
      CHECK(top > y.front() + 20.0f);
      CHECK(top > y.back() + 20.0f);
    }
  }
  // both curves agree with the analytic response at the points they have: the base at Cutoff, the second at Cutoff 2^Range
  float p[ENVFILTER_PARAM_COUNT] = {2.0f, 1500.0f, 2.0f, 50.0f, 5.0f, 150.0f, 12.0f, 80.0f};
  NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, ENVFILTER_PARAM_COUNT) == ViewStatus::Ok);
  Drawing d;
  views.replay(d);
  NEED(d.curves.size() == 2u);
  Set s;
  s.mode = 2;
  s.res = 50.0;
  s.mix = 80.0;
  const double step = std::log2(1000.0) / 39.0;
  for (int c = 0; c < 2; ++c) {
    const double fc = c == 0 ? 1500.0 : 1500.0 * 4.0;
    for (int i = 0; i < 40; ++i) {
      const double f = 20.0 * std::exp2(step * i);
      const double want = std::max(-48.0, std::min(24.0, db(ref_gain(s, fc, f))));
      CAPTURE(c);
      CAPTURE(i);
      CHECK(std::fabs(d.curve_y[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] - want) < 0.05);
    }
  }
  // the minimum size the manifest asks for also draws
  float q[ENVFILTER_PARAM_COUNT];
  for (std::uint32_t i = 0; i < ENVFILTER_PARAM_COUNT; ++i) {
    q[i] = envfilter_params[i].def;
  }
  CHECK(views.draw(*lib.entry, inst.state, 1, 0, 120, 80, q, ENVFILTER_PARAM_COUNT) == ViewStatus::Ok);
}

TEST_CASE("envfilter: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_ENVFILTER_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_EFFECT);
  CHECK(t.h.abi_minor == 2u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == ENVFILTER_PARAM_COUNT);
  CHECK(t.h.view_count == ENVFILTER_VIEW_COUNT);
  CHECK(t.h.state_bytes <= ENVFILTER_STATE_BYTES);
  CHECK(t.h.hot_size <= 2048u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: the skill's ceiling is 2.5 KB (DRIVE's is 1,328 B, CRUSH's 720 B)
  MESSAGE("envfilter.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
