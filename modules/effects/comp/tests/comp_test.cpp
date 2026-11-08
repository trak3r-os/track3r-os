// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): the COMP effect module (a compressor with a TRANSIENT mode) driven through the ABI as the OS drives an insert, against a double-precision reference of the
// same maths (the same control-step schedule and ramp, the exact log2, exp2 and exponential where the module has polynomials and a series): the sample-by-sample error over a grid of settings in both
// modes, the level law at the defaults (the output rms never above the input's, DRIVE's lesson: sines and noise from -60 to -6 dBFS, in both modes), the compression law at steady state against the
// soft-knee curve, the attack and release times, the transient shaper's laws (Punch and Sustain, level independence, a steady tone), Mix 0 as the dry track bit for bit, silence and the tail (an idle
// effect is a fresh one), block-size independence and in == out, the Makeup and Mix ramps, a Mode change without a jump, extremes (finite, bounded, no subnormals), hostile values, the view and the
// packed file through the loader.
// What the reference proves: it is a double-precision model of the same structure (the same control-step schedule, ramp, knee and follower equations, written again in the test), so the grid checks the float
// arithmetic (the log2 and exp2 polynomials, the series for the coefficients, the ramp, the seeding), not the design; the design is checked by the law tests (the steady-state curve, the attack and release
// times, the transient windows, the level law).
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-196).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "comp_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;
constexpr double kDb2L2 = 0.16609640474436812;  // log2(10) / 20

// ---- the reference, in double ------------------------------------------------------------------------------------------------------------------
struct Pr {
  int mode = 0;  // 0 COMP, 1 TRANSIENT
  double thr = -18, ratio = 4, att = 10, rel = 120, makeup = 0, mix = 100, punch = 0, sus = 0;  // the module's defaults
};

double ref_pole(double ms) {
  const double x = std::min(1000.0 / kFs / ms, 0.3);
  return 1.0 - std::exp(-x);
}

// The soft knee (1 log2 unit wide): the gain reduction, in log2 units, of a level `over` log2 units above the threshold.
double ref_red(double over, double s) {
  if (over >= 0.5) {
    return s * over;
  }
  if (over > -0.5) {
    const double k = over + 0.5;
    return s * k * k * 0.5;
  }
  return 0.0;
}

std::vector<double> ref_run(const Pr& p, const std::vector<float>& x) {
  const double s = 1.0 - 1.0 / p.ratio, T = p.thr * kDb2L2, kp = p.punch * 0.01, ks = p.sus * 0.01;
  const double ca = ref_pole(p.att), cr = ref_pole(p.rel);
  const double fa = ref_pole(std::max(p.att * 0.125, 0.1)), fr = ref_pole(p.rel / 3.0);
  const double og = std::pow(10.0, p.makeup / 20.0), m = p.mix / 100.0;
  double e0 = 0, e1 = 0, G = og, Gt = og, dG = 0;
  if (p.mode == 1 && !x.empty()) {  // a Mode change seeds the followers from the first input sample (the instance is set up with Mode 1 before its first block)
    e0 = e1 = std::fabs(static_cast<double>(x[0]));
  }
  int left = 0;
  std::vector<double> y(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (left == 0) {
      double g;
      if (p.mode == 0) {
        g = std::min(std::exp2(std::max(-ref_red(std::log2(e0 + 1e-9) - T, s), -16.0)), 1.0);
      } else {
        const double d = std::log2(e0 + 3e-4) - std::log2(e1 + 3e-4);
        g = std::exp2(std::clamp(d >= 0.0 ? kp * d : -ks * d, -2.0, 2.0));
      }
      Gt = og * ((1.0 - m) + m * g);
      dG = std::fabs(Gt - G) < 3e-5 ? 0.0 : (Gt - G) / 16.0;  // (a ramp of rounding-sized slope does not run)
      left = 16;
    }
    const double a = std::fabs(static_cast<double>(x[i]));
    if (p.mode == 0) {
      e0 += (a > e0 ? ca : cr) * (a - e0);
    } else {
      e0 += (a > e0 ? fa : fr) * (a - e0);
      e1 += (a > e1 ? ca : cr) * (a - e1);
    }
    y[i] = static_cast<double>(x[i]) * G;
    G += dG;
    if (--left == 0) {
      G = Gt;
    }
  }
  return y;
}

// The sidechain key's host (ABI 1.12, OS-INSFX-FX2 X6): a track_level that answers g_key_levels per track index and records which track the module asked for.
float g_key_levels[16] = {};
std::uint32_t g_key_asked = 0xFFFFFFFFu;  // the last track index the module passed to track_level
float fake_track_level(std::uint32_t track) {
  g_key_asked = track;
  return track < 16u ? g_key_levels[track] : 0.0f;
}
void set_key_level(std::uint32_t track_idx, float level) {  // what the fake answers for track index `track_idx` (0..15 = T1..T16)
  if (track_idx < 16u) {
    g_key_levels[track_idx] = level;
  }
}
o3m_host key_host() {
  o3m_host h = make_host();  // size sizeof(o3m_host), so O3M_HOST_HAS(host, track_level) holds
  h.track_level = &fake_track_level;
  return h;
}

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fx() { ok = lib.open(O3M_COMP_HOST_SO) && inst.start_with(*lib.entry, key_host()); }  // a host with a track_level: the KEY tests script g_key_levels (with Key OFF it is never asked)
  void set(const Pr& p) {
    inst.set(COMP_PARAM_THRESHOLD, static_cast<float>(p.thr));
    inst.set(COMP_PARAM_RATIO, static_cast<float>(p.ratio));
    inst.set(COMP_PARAM_ATTACK, static_cast<float>(p.att));
    inst.set(COMP_PARAM_RELEASE, static_cast<float>(p.rel));
    inst.set(COMP_PARAM_MAKEUP, static_cast<float>(p.makeup));
    inst.set(COMP_PARAM_MIX, static_cast<float>(p.mix));
    inst.set(COMP_PARAM_MODE, static_cast<float>(p.mode));
    inst.set(COMP_PARAM_PUNCH, static_cast<float>(p.punch));
    inst.set(COMP_PARAM_SUSTAIN, static_cast<float>(p.sus));
  }
  std::vector<float> run(const std::vector<float>& x, std::uint32_t block = 64) {
    std::vector<float> y(x.size());
    for (std::size_t at = 0; at < x.size();) {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, x.size() - at));
      busy = inst.m->render(inst.state, x.data() + at, y.data() + at, n);
      at += n;
    }
    return y;
  }
  // render_st, the stereo shape (ABI 1.12): the same state, one linked gain on the peak of L and R.
  void run_st(const std::vector<float>& il, const std::vector<float>& ir, std::vector<float>& ol, std::vector<float>& orr, std::uint32_t block = 64) {
    ol.assign(il.size(), 0.0f);
    orr.assign(il.size(), 0.0f);
    for (std::size_t at = 0; at < il.size();) {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, il.size() - at));
      busy = inst.m->render_st(inst.state, il.data() + at, ir.data() + at, ol.data() + at, orr.data() + at, n);
      at += n;
    }
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

void append(std::vector<float>& a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); }

double db_of(double a) { return 20.0 * std::log10(a); }
double lin_of(double db) { return std::pow(10.0, db / 20.0); }

double rms(const std::vector<float>& v, std::size_t a, std::size_t b) {
  double s = 0;
  for (std::size_t i = a; i < b; ++i) {
    s += static_cast<double>(v[i]) * v[i];
  }
  return std::sqrt(s / static_cast<double>(b - a));
}
double rms(const std::vector<float>& v) { return rms(v, 0, v.size()); }

// The gain in dB of a window: out rms over in rms.
double gain_db(const std::vector<float>& y, const std::vector<float>& x, std::size_t a, std::size_t b) { return db_of(rms(y, a, b) / rms(x, a, b)); }

// A program with loud and quiet noise, a tone and a decaying burst (12000 samples): both followers, attacks and releases get something to do.
std::vector<float> program() {
  std::vector<float> x = noise(1500, 7, 0.9f);
  append(x, noise(2000, 8, 0.05f));
  append(x, sine(2500, 0.4, 330.0));
  std::vector<float> burst = noise(3000, 9, 0.8f);
  for (std::size_t i = 0; i < burst.size(); ++i) {
    burst[i] *= static_cast<float>(std::exp(-static_cast<double>(i) / 900.0));
  }
  append(x, burst);
  append(x, sine(3000, 0.02, 1500.0));
  return x;
}

// The peak of a 1 ms window of a signal, at window k from `from`.
double peak_win(const std::vector<float>& v, std::size_t from, std::size_t k) {
  double p = 0;
  for (std::size_t i = from + k * 44; i < from + (k + 1) * 44 && i < v.size(); ++i) {
    p = std::max(p, static_cast<double>(std::fabs(v[i])));
  }
  return p;
}

// The amplitude of a sine of frequency fe in the last M samples of a signal, by correlation.
double amp_of(const std::vector<float>& sig, double fe, std::size_t M) {
  double a = 0, b = 0;
  for (std::size_t i = sig.size() - M; i < sig.size(); ++i) {
    const double ph = 2.0 * kPi * fe * static_cast<double>(i) / kFs;
    a += sig[i] * std::sin(ph);
    b += sig[i] * std::cos(ph);
  }
  return std::hypot(a, b) * 2.0 / static_cast<double>(M);
}

}  // namespace

TEST_CASE("comp: the descriptor says an effect with ten parameters, stereo, presets and a view") {
  Lib lib;
  NEED(lib.open(O3M_COMP_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == COMP_PARAM_COUNT);
  CHECK(COMP_PARAM_COUNT == 10u);
  CHECK(COMP_PARAM_THRESHOLD == 0);  // first: the bench's moving-parameter row sweeps the first MOD target
  CHECK(COMP_PARAM_RATIO == 1);
  CHECK(COMP_PARAM_ATTACK == 2);
  CHECK(COMP_PARAM_RELEASE == 3);
  CHECK(COMP_PARAM_MAKEUP == 4);
  CHECK(COMP_PARAM_MIX == 5);
  CHECK(COMP_PARAM_MODE == 6);
  CHECK(COMP_PARAM_PUNCH == 7);
  CHECK(COMP_PARAM_SUSTAIN == 8);
  CHECK(COMP_PARAM_KEY == 9);
  CHECK(O3M_PARAM_UNIT(m.params[COMP_PARAM_THRESHOLD].flags) == O3M_UNIT_DB);
  CHECK(m.params[COMP_PARAM_THRESHOLD].min == -60.0f);
  CHECK(m.params[COMP_PARAM_THRESHOLD].max == 0.0f);
  CHECK(m.params[COMP_PARAM_THRESHOLD].def == -18.0f);
  CHECK((m.params[COMP_PARAM_RATIO].flags & O3M_PARAM_LOG) != 0u);
  CHECK(m.params[COMP_PARAM_RATIO].min == 1.0f);
  CHECK(m.params[COMP_PARAM_RATIO].max == 20.0f);
  CHECK(m.params[COMP_PARAM_RATIO].def == 4.0f);
  CHECK(O3M_PARAM_UNIT(m.params[COMP_PARAM_ATTACK].flags) == O3M_UNIT_MS);
  CHECK(m.params[COMP_PARAM_ATTACK].def == 10.0f);
  CHECK(O3M_PARAM_UNIT(m.params[COMP_PARAM_RELEASE].flags) == O3M_UNIT_MS);
  CHECK(m.params[COMP_PARAM_RELEASE].def == 120.0f);
  CHECK(O3M_PARAM_UNIT(m.params[COMP_PARAM_MAKEUP].flags) == O3M_UNIT_DB);
  CHECK(m.params[COMP_PARAM_MAKEUP].def == 0.0f);  // no make-up at the defaults: the level law
  CHECK(O3M_PARAM_UNIT(m.params[COMP_PARAM_MIX].flags) == O3M_UNIT_PERCENT);
  CHECK(m.params[COMP_PARAM_MIX].def == 100.0f);
  CHECK((m.params[COMP_PARAM_MODE].flags & O3M_PARAM_INT) != 0u);
  CHECK(m.params[COMP_PARAM_MODE].min == 0.0f);
  CHECK(m.params[COMP_PARAM_MODE].max == 1.0f);
  CHECK(m.params[COMP_PARAM_MODE].def == 0.0f);  // COMP
  CHECK(m.params[COMP_PARAM_PUNCH].min == -100.0f);
  CHECK(m.params[COMP_PARAM_PUNCH].max == 100.0f);
  CHECK(m.params[COMP_PARAM_PUNCH].def == 0.0f);  // neutral
  CHECK(m.params[COMP_PARAM_SUSTAIN].def == 0.0f);
  CHECK((m.params[COMP_PARAM_KEY].flags & O3M_PARAM_INT) != 0u);
  CHECK((m.params[COMP_PARAM_KEY].flags & O3M_PARAM_KEY) != 0u);  // the sidechain key (ABI 1.12): OFF, T1..T16; the OS never lets MIDI Learn bind it
  CHECK(m.params[COMP_PARAM_KEY].min == 0.0f);
  CHECK(m.params[COMP_PARAM_KEY].max == 16.0f);
  CHECK(m.params[COMP_PARAM_KEY].def == 0.0f);  // OFF
  CHECK(m.render_st != nullptr);  // stereo-capable (not WIDENS: one linked gain, render_st on a block with a side signal)
  CHECK(m.fx_flags == 0u);
  NEED(m.presets != nullptr);
  CHECK(m.preset_count == COMP_PRESET_COUNT);
  CHECK(COMP_PRESET_COUNT == 4u);
  for (std::uint32_t i = 0; i < COMP_PARAM_COUNT; ++i) {
    if (i == COMP_PARAM_KEY) {
      continue;  // the sidechain key: O3M_PARAM_KEY, never a Learn target (the OS honours the flag) and not a MOD target
    }
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);  // MIDI Learn may bind each of them
    CHECK(((m.params[i].flags & O3M_PARAM_MOD) != 0u) == (i != COMP_PARAM_MODE));  // a modulation target: set_param is a few multiplies and two divisions at most (the Mode switch is not one)
  }
  CHECK(m.view_count == 1u);
}

TEST_CASE("comp: the output tracks a double-precision reference over a grid of settings, in both modes") {
  const std::vector<float> x = program();
  double worst_err = 0.0, worst_snr = 1e9;
  std::uint32_t runs = 0;
  auto check = [&](const Pr& p) {
    Fx fx;
    NEED(fx.ok);
    fx.set(p);
    const std::vector<float> y = fx.run(x);
    const std::vector<double> ref = ref_run(p, x);
    double es = 0, ee = 0, err = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double e = static_cast<double>(y[i]) - ref[i];
      es += ref[i] * ref[i];
      ee += e * e;
      err = std::max(err, std::fabs(e));
    }
    CAPTURE(p.mode);
    CAPTURE(p.thr);
    CAPTURE(p.ratio);
    CAPTURE(p.att);
    CAPTURE(p.rel);
    CAPTURE(p.makeup);
    CAPTURE(p.mix);
    CAPTURE(p.punch);
    CAPTURE(p.sus);
    CHECK(err < 1e-4);
    const double snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
    CHECK_MESSAGE(snr > 70.0, "snr " << snr);
    worst_snr = std::min(worst_snr, snr);
    worst_err = std::max(worst_err, err);
    ++runs;
  };
  for (double thr : {-40.0, -18.0, -6.0}) {
    for (double ratio : {1.5, 4.0, 20.0}) {
      for (double att : {0.1, 10.0, 100.0}) {
        for (double rel : {10.0, 120.0, 1000.0}) {
          for (double mk : {0.0, 12.0}) {
            for (double mx : {100.0, 50.0}) {
              Pr p;
              p.thr = thr;
              p.ratio = ratio;
              p.att = att;
              p.rel = rel;
              p.makeup = mk;
              p.mix = mx;
              check(p);
            }
          }
        }
      }
    }
  }
  for (double att : {0.1, 10.0, 100.0}) {
    for (double rel : {10.0, 120.0, 1000.0}) {
      for (double punch : {-100.0, 60.0, 100.0}) {
        for (double sus : {-100.0, 60.0, 100.0}) {
          for (double mk : {0.0, 6.0}) {
            for (double mx : {100.0, 50.0}) {
              Pr p;
              p.mode = 1;
              p.att = att;
              p.rel = rel;
              p.punch = punch;
              p.sus = sus;
              p.makeup = mk;
              p.mix = mx;
              check(p);
            }
          }
        }
      }
    }
  }
  MESSAGE("worst max error " << worst_err << " (limit 1e-4), worst SNR " << worst_snr << " dB (limit 70) over " << runs << " settings");
}

TEST_CASE("comp: the level law: at the defaults the output rms is never above the input's, in COMP and in TRANSIENT; what is under the knee passes bit for bit") {
  // The lesson of DRIVE (+12..16 dB over bypass at its defaults): the defaults (Threshold -18, Ratio 4, Makeup 0, Mix 100) must not raise the level. A compressor only reduces, so the gain is never
  // above 1 and the output is never above the input, sample by sample; sines and noise from -60 to -6 dBFS.
  double worst_up = -1e9, worst_down = 1e9;
  const double freqs[] = {100.0, 440.0, 1234.5, 3000.0};
  for (int mode : {0, 1}) {
    for (int db = -60; db <= -6; db += 2) {
      const double amp = std::pow(10.0, db / 20.0);
      for (double f : freqs) {
        const std::vector<float> x = sine(22050, amp, f);
        Fx fx;
        NEED(fx.ok);  // the defaults: start() sets every parameter to its default
        fx.inst.set(COMP_PARAM_MODE, static_cast<float>(mode));
        const std::vector<float> y = fx.run(x);
        CAPTURE(mode);
        CAPTURE(db);
        CAPTURE(f);
        CHECK(rms(y) <= rms(x));
        for (std::size_t i = 0; i < x.size(); ++i) {
          NEED(std::fabs(y[i]) <= std::fabs(x[i]));
        }
        worst_up = std::max(worst_up, db_of(rms(y) / rms(x)));
        if (mode == 0) {
          worst_down = std::min(worst_down, db_of(rms(y) / rms(x)));
        }
        if (mode == 1 || db <= -24) {  // TRANSIENT at Punch 0 and Sustain 0 is the input; COMP's knee starts 3 dB under the threshold: a peak under -21 dBFS is untouched
          CHECK(std::memcmp(y.data(), x.data(), x.size() * sizeof(float)) == 0);
        }
      }
      for (std::uint32_t seed : {1u, 2u}) {
        const std::vector<float> x = noise(22050, seed, static_cast<float>(amp * 1.7320508));  // a uniform noise of rms amp
        Fx fx;
        NEED(fx.ok);
        fx.inst.set(COMP_PARAM_MODE, static_cast<float>(mode));
        const std::vector<float> y = fx.run(x);
        CAPTURE(mode);
        CAPTURE(db);
        CHECK(rms(y) <= rms(x));
        worst_up = std::max(worst_up, db_of(rms(y) / rms(x)));
        if (mode == 0) {
          worst_down = std::min(worst_down, db_of(rms(y) / rms(x)));
        }
      }
    }
  }
  MESSAGE("defaults: the largest rise over bypass " << worst_up << " dB (must be <= 0), the largest fall in COMP " << worst_down << " dB");
  CHECK(worst_up <= 0.0);
  CHECK(worst_down > -14.0);  // -6 dBFS rms noise is 17 dB over the threshold at its peaks: about 12 dB of reduction (10.1 dB measured in the model); the cost of a compressor, not a boost
}

TEST_CASE("comp: Ratio 1 is the input bit for bit, and Mix 0 too, also after a wet period") {
  const std::vector<float> x = noise(4096, 11, 1.0f);
  {
    Fx fx;
    NEED(fx.ok);
    Pr p;
    p.ratio = 1.0;
    p.thr = -60.0;
    fx.set(p);
    const std::vector<float> y = fx.run(x);
    CHECK(std::memcmp(y.data(), x.data(), x.size() * sizeof(float)) == 0);
  }
  {
    Fx fx;
    NEED(fx.ok);
    Pr p;
    p.ratio = 20.0;
    p.thr = -60.0;
    p.mix = 0.0;
    fx.set(p);
    const std::vector<float> y = fx.run(x);
    CHECK(std::memcmp(y.data(), x.data(), x.size() * sizeof(float)) == 0);
  }
  // wet first, then Mix 0: the mix glides to 0 (about 22 blocks at 64 frames, then it snaps), and from then on the output is the input bit for bit
  Fx fx;
  NEED(fx.ok);
  Pr p;
  p.ratio = 20.0;
  p.thr = -60.0;
  fx.set(p);
  (void)fx.run(noise(4096, 3, 0.5f));
  fx.inst.set(COMP_PARAM_MIX, 0.0f);
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

TEST_CASE("comp: at steady state the gain follows the soft-knee curve: out = in - reduction(in - Threshold)") {
  constexpr std::size_t M = 4096;
  const double fe = std::round(1000.0 * M / kFs) * kFs / M;  // a whole number of periods in the window
  double worst = 0;
  for (double ratio : {2.0, 4.0, 8.0, 20.0}) {
    for (double thr : {-36.0, -24.0, -12.0}) {
      for (double level : {-30.0, -24.0, -20.0, -12.0, -6.0}) {
        Pr p;
        p.thr = thr;
        p.ratio = ratio;
        p.att = 1.0;
        p.rel = 1000.0;  // a slow release: the follower holds the peak, with no ripple at 1 kHz
        Fx fx;
        NEED(fx.ok);
        fx.set(p);
        const double a = lin_of(level);
        const std::vector<float> x = sine(22050, a, fe);
        const std::vector<float> y = fx.run(x);
        const double got = db_of(amp_of(y, fe, M) / amp_of(x, fe, M));
        const double want = -ref_red(std::log2(a) - thr * kDb2L2, 1.0 - 1.0 / ratio) / kDb2L2;
        CAPTURE(ratio);
        CAPTURE(thr);
        CAPTURE(level);
        CHECK(std::fabs(got - want) < 0.25);
        worst = std::max(worst, std::fabs(got - want));
      }
    }
  }
  MESSAGE("steady-state gain against the curve: worst difference " << worst << " dB");
}

TEST_CASE("comp: Attack and Release set how fast the gain moves") {
  const double hi = lin_of(-6.0), lo = lin_of(-40.0);
  std::size_t t_att[3] = {};
  {
    const std::size_t pre = 8820;
    std::vector<float> x = sine(pre, lo, 1000.0);
    append(x, sine(52920, hi, 1000.0));
    const double atts[3] = {1.0, 10.0, 50.0};
    for (int k = 0; k < 3; ++k) {
      Pr p;
      p.thr = -24.0;
      p.ratio = 8.0;
      p.att = atts[k];
      p.rel = 1000.0;
      Fx fx;
      NEED(fx.ok);
      fx.set(p);
      const std::vector<float> y = fx.run(x);
      const double fin = db_of(peak_win(y, x.size() - 1000, 0) / peak_win(x, x.size() - 1000, 0));
      std::size_t t = 0;
      for (; t < 1200; ++t) {
        if (db_of(peak_win(y, pre, t) / peak_win(x, pre, t)) <= fin + 3.0) {
          break;
        }
      }
      t_att[k] = t;  // ms until the gain is within 3 dB of its final value
      MESSAGE("Attack " << atts[k] << " ms: within 3 dB of the final gain " << fin << " dB after " << t << " ms");
    }
    CHECK(t_att[0] < 10u);
    CHECK(t_att[1] >= 8u);
    CHECK(t_att[1] < 60u);
    CHECK(t_att[2] >= 40u);
    CHECK(t_att[2] < 250u);
    CHECK(t_att[0] < t_att[1]);
    CHECK(t_att[1] < t_att[2]);
  }
  std::size_t t_rel[3] = {};
  {
    std::vector<float> x = sine(22050, hi, 1000.0);
    append(x, sine(110250, lo, 1000.0));
    const double rels[3] = {10.0, 120.0, 1000.0};
    for (int k = 0; k < 3; ++k) {
      Pr p;
      p.thr = -24.0;
      p.ratio = 8.0;
      p.att = 1.0;
      p.rel = rels[k];
      Fx fx;
      NEED(fx.ok);
      fx.set(p);
      const std::vector<float> y = fx.run(x);
      std::size_t t = 0;
      for (; t < 2500; ++t) {
        if (db_of(peak_win(y, 22050, t) / peak_win(x, 22050, t)) >= -1.0) {
          break;
        }
      }
      t_rel[k] = t;  // ms until the gain is back within 1 dB of unity
      MESSAGE("Release " << rels[k] << " ms: back within 1 dB of unity after " << t << " ms");
    }
    CHECK(t_rel[0] < 60u);
    CHECK(t_rel[1] >= 100u);
    CHECK(t_rel[1] < 500u);
    CHECK(t_rel[2] >= 1000u);
    CHECK(t_rel[0] < t_rel[1]);
    CHECK(t_rel[1] < t_rel[2]);
  }
}

TEST_CASE("comp: TRANSIENT: Punch lifts an onset, Sustain lifts a decay, each leaves the other part alone, and a steady tone barely moves") {
  // a burst of noise with a 80 ms exponential decay after 100 ms of silence; window A is the first 8 ms, B is 150 to 300 ms after the onset
  auto burst = [](double level_db) {
    std::vector<float> x(4410, 0.0f);
    const std::vector<float> n = noise(22050, 5, 1.0f);
    for (std::size_t i = 0; i < 17640; ++i) {
      x.push_back(static_cast<float>(lin_of(level_db) * n[i] * std::exp(-static_cast<double>(i) / (0.08 * kFs))));
    }
    return x;
  };
  const std::size_t on = 4410;
  const std::size_t a0 = on, a1 = on + 352, b0 = on + 6615, b1 = on + 13230;
  struct Row {
    double punch, sus;
  };
  double a_punch = 0, a_sus = 0;
  for (Row r : {Row{100, 0}, Row{-100, 0}, Row{0, 100}, Row{0, -100}, Row{50, 0}}) {
    const std::vector<float> x = burst(-6.0);
    Pr p;
    p.mode = 1;
    p.punch = r.punch;
    p.sus = r.sus;
    Fx fx;
    NEED(fx.ok);
    fx.set(p);
    const std::vector<float> y = fx.run(x);
    const double ga = gain_db(y, x, a0, a1), gb = gain_db(y, x, b0, b1);
    MESSAGE("Punch " << r.punch << " Sustain " << r.sus << ": onset " << ga << " dB, decay " << gb << " dB");
    if (r.punch > 0.0) {
      CHECK(ga > (r.punch >= 100.0 ? 6.0 : 3.0));
      CHECK(std::fabs(gb) < 1.0);
    } else if (r.punch < 0.0) {
      CHECK(ga < -5.0);
      CHECK(std::fabs(gb) < 1.0);
    } else if (r.sus > 0.0) {
      CHECK(gb > 4.0);
      CHECK(std::fabs(ga) < 1.0);
    } else {
      CHECK(gb < -4.0);
      CHECK(std::fabs(ga) < 1.0);
    }
    if (r.punch == 100.0) {
      a_punch = ga;
    }
    if (r.sus == 100.0) {
      a_sus = gb;
    }
  }
  CHECK(a_punch > 0.0);
  CHECK(a_sus > 0.0);
  // level independence: the same burst 20 dB lower gives the same gains (a ratio of two followers), within 1 dB
  for (Row r : {Row{100, 0}, Row{0, 100}}) {
    Pr p;
    p.mode = 1;
    p.punch = r.punch;
    p.sus = r.sus;
    double g[2][2];
    int k = 0;
    for (double lvl : {-6.0, -26.0}) {
      const std::vector<float> x = burst(lvl);
      Fx fx;
      NEED(fx.ok);
      fx.set(p);
      const std::vector<float> y = fx.run(x);
      g[k][0] = gain_db(y, x, a0, a1);
      g[k][1] = gain_db(y, x, b0, b1);
      ++k;
    }
    CHECK(std::fabs(g[0][0] - g[1][0]) < 1.0);
    CHECK(std::fabs(g[0][1] - g[1][1]) < 1.0);
  }
  // a steady tone at full Punch and Sustain: within 1 dB of unity at any frequency (the followers' ripple counts on both sides)
  for (double f : {60.0, 100.0, 440.0, 3000.0}) {
    const std::vector<float> x = sine(66150, 0.5, f);
    Pr p;
    p.mode = 1;
    p.punch = 100.0;
    p.sus = 100.0;
    Fx fx;
    NEED(fx.ok);
    fx.set(p);
    const std::vector<float> y = fx.run(x);
    const double g = gain_db(y, x, 33075, 66150);
    CAPTURE(f);
    MESSAGE("steady tone " << f << " Hz at Punch 100, Sustain 100: " << g << " dB");
    CHECK(std::fabs(g) < 1.0);
  }
}

TEST_CASE("comp: silence in gives silence out and a tail that ends; an idle effect is a fresh one") {
  for (int mode : {0, 1}) {
    Pr p;
    p.mode = mode;
    p.rel = 10.0;
    p.punch = 100.0;
    p.sus = 100.0;
    Fx fx;
    NEED(fx.ok);
    fx.set(p);
    const std::vector<float> zero(64, 0.0f);
    std::vector<float> out(64);
    CAPTURE(mode);
    CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);  // never fed: idle at once, exact zeros
    for (float v : out) {
      CHECK(v == 0.0f);
    }
    // a loud burst, then silence: the followers ring (it says so), and in a bounded time it goes idle
    const std::vector<float> loud = noise(2000, 3, 0.9f);
    (void)fx.run(loud);
    CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 1u);  // the first silent block: a follower is above -80 dBFS
    std::uint32_t blocks = 1;
    while (fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) != 0u) {
      NEED(++blocks < 600u);
    }
    MESSAGE("mode " << mode << ": idle after " << blocks << " silent block(s) at Release 10 ms");
    for (int again = 0; again < 2; ++again) {
      CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);
      for (float v : out) {
        CHECK(v == 0.0f);
      }
    }
    // live input is reported live, and what it does is what a fresh instance does
    const std::vector<float> sig = noise(1000, 4, 0.8f);
    Fx fresh;
    NEED(fresh.ok);
    fresh.set(p);
    (void)fresh.run(zero);  // (a Mode change seeds the followers from the first input sample: one silent block consumes the seed)
    const std::vector<float> ya = fx.run(sig), yb = fresh.run(sig);
    CHECK(fx.busy == 1u);
    CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  }
  // the tail is bounded at the slowest release too (1 s): the followers fall from full scale to -80 dBFS within about 9 s
  for (int mode : {0, 1}) {
    Pr p;
    p.mode = mode;
    p.rel = 1000.0;
    Fx fx;
    NEED(fx.ok);
    fx.set(p);
    (void)fx.run(noise(2000, 3, 1.0f));
    const std::vector<float> zero(64, 0.0f);
    std::vector<float> out(64);
    std::uint32_t blocks = 0;
    while (fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) != 0u) {
      NEED(++blocks < 12000u);
    }
    MESSAGE("mode " << mode << ": idle after " << blocks << " silent block(s) at Release 1000 ms (" << blocks * 64.0 / kFs << " s)");
  }
}

TEST_CASE("comp: any block size gives the same bits (static parameters), and in == out is the same as two buffers") {
  const std::vector<float> x = program();
  for (int mode : {0, 1}) {
    Pr p;
    p.mode = mode;
    p.thr = -30.0;
    p.ratio = 6.0;
    p.makeup = 3.0;
    p.mix = 70.0;
    p.punch = 80.0;
    p.sus = -50.0;
    Fx base;
    NEED(base.ok);
    base.set(p);
    const std::vector<float> y = base.run(x, 64);
    for (std::uint32_t block : {1u, 3u, 16u, 17u, 100u, 128u}) {
      Fx fx;
      NEED(fx.ok);
      fx.set(p);
      const std::vector<float> z = fx.run(x, block);
      CAPTURE(mode);
      CAPTURE(block);
      CHECK(std::memcmp(y.data(), z.data(), y.size() * sizeof(float)) == 0);  // the control step is counted across blocks
    }
    Fx alias;
    NEED(alias.ok);
    alias.set(p);
    std::vector<float> buf = x;
    for (std::size_t at = 0; at < buf.size(); at += 64) {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, buf.size() - at));
      alias.inst.m->render(alias.inst.state, buf.data() + at, buf.data() + at, n);
    }
    CHECK(std::memcmp(y.data(), buf.data(), y.size() * sizeof(float)) == 0);
  }
}

TEST_CASE("comp: Makeup and Mix glide: no step where the same change done at once clicks") {
  const std::size_t n = 8192, at = 2048;
  // Makeup 0 to -24 dB at Ratio 1 (the compressor is transparent): the gain ramp is the only thing that moves
  {
    const std::vector<float> x = sine(n, 0.5, 440.0);
    Fx fx;
    NEED(fx.ok);
    Pr p;
    p.ratio = 1.0;
    fx.set(p);
    std::vector<float> y(n);
    for (std::size_t q = 0; q < n; q += 64) {
      if (q == at) {
        fx.inst.set(COMP_PARAM_MAKEUP, -24.0f);
      }
      fx.inst.m->render(fx.inst.state, x.data() + q, y.data() + q, 64);
    }
    double jump = 0, step = 0;
    for (std::size_t i = at - 8; i < at + 800; ++i) {
      jump = std::max(jump, std::fabs(static_cast<double>(y[i + 1]) - 2.0 * y[i] + y[i - 1]));
      step = std::max(step, std::fabs(static_cast<double>(y[i + 1]) - y[i]));
    }
    double jump_ref = 0;
    const double g1 = lin_of(-24.0);
    for (std::size_t i = at - 8; i < at + 800; ++i) {  // the same step at once in the reference
      auto v = [&](std::size_t k) { return static_cast<double>(x[k]) * (k < at ? 1.0 : g1); };
      jump_ref = std::max(jump_ref, std::fabs(v(i + 1) - 2.0 * v(i) + v(i - 1)));
    }
    MESSAGE("Makeup step: largest second difference " << jump << " (at once: " << jump_ref << "), largest sample step " << step);
    CHECK(jump < 0.08);
    CHECK(jump_ref > 0.2);
    CHECK(jump_ref > 3.0 * jump);
    CHECK(step < 0.1);
    // and it gets there
    CHECK(std::fabs(amp_of(y, 440.0, 2048) - 0.5 * g1) < 0.005 * 0.5 + 1e-4);
  }
  // Mix 0 to 100 % on a heavily compressed sine: the gain G = y / x only falls, in small steps, and ends far below 1
  {
    const std::vector<float> x = sine(n, 0.5, 440.0);
    Fx fx;
    NEED(fx.ok);
    Pr p;
    p.ratio = 20.0;
    p.thr = -60.0;
    p.att = 0.1;
    p.mix = 0.0;
    fx.set(p);
    std::vector<float> y(n);
    for (std::size_t q = 0; q < n; q += 64) {
      if (q == at) {
        fx.inst.set(COMP_PARAM_MIX, 100.0f);
      }
      fx.inst.m->render(fx.inst.state, x.data() + q, y.data() + q, 64);
    }
    double prev = -1.0, worst_up = 0.0, worst_step = 0.0, last = 1.0;
    bool run_on = false;  // the previous sample was read too (a step is read only inside a run of samples: between two runs the gain has time to move)
    for (std::size_t i = 0; i < n; ++i) {
      if (std::fabs(x[i]) < 0.3f) {
        run_on = false;
        continue;
      }
      const double g = static_cast<double>(y[i]) / x[i];
      if (prev >= 0.0) {
        worst_up = std::max(worst_up, g - prev);
        if (run_on) {
          worst_step = std::max(worst_step, std::fabs(g - prev));
        }
      }
      last = g;
      prev = g;
      run_on = true;
    }
    MESSAGE("Mix glide: largest rise of the gain " << worst_up << ", largest step " << worst_step << ", ends at " << last);
    CHECK(worst_up < 2e-3);  // it only falls
    CHECK(worst_step < 0.1);
    CHECK(last < 0.01);
  }
}

TEST_CASE("comp: a Mode change does not jump: the followers start again from the input") {
  const std::vector<float> x = sine(6400, 0.5, 220.0);
  const double sine_step = 0.5 * 2.0 * kPi * 220.0 / kFs;
  for (int from : {1, 0}) {
    Fx fx;
    NEED(fx.ok);
    Pr p;
    p.mode = from;
    p.punch = 100.0;
    p.sus = 100.0;
    fx.set(p);
    std::vector<float> y(x.size());
    for (std::size_t q = 0; q < x.size(); q += 64) {
      if (q == 3200) {
        fx.inst.set(COMP_PARAM_MODE, static_cast<float>(1 - from));
      }
      fx.inst.m->render(fx.inst.state, x.data() + q, y.data() + q, 64);
    }
    double step = 0;
    for (std::size_t i = 3000; i < 4200; ++i) {
      step = std::max(step, std::fabs(static_cast<double>(y[i + 1]) - y[i]));
    }
    MESSAGE("mode " << from << " -> " << 1 - from << ": largest sample step " << step << " (the tone's own: " << sine_step << ")");
    CHECK(step < 0.06);
    for (float v : y) {
      NEED(std::isfinite(v));
    }
  }
}

TEST_CASE("comp: extremes: every setting on full-scale input stays finite and bounded, with no subnormals") {
  std::vector<float> loud = noise(4096, 3, 1.0f);
  loud[100] = 1.0f;
  loud[101] = -1.0f;
  for (int mode : {0, 1}) {
    for (double thr : {-60.0, 0.0}) {
      for (double ratio : {1.0, 20.0}) {
        for (double mk : {-24.0, 24.0}) {
          for (double mx : {0.0, 100.0}) {
            for (int k = 0; k < 2; ++k) {
              Pr p;
              p.mode = mode;
              p.thr = thr;
              p.ratio = ratio;
              p.makeup = mk;
              p.mix = mx;
              p.att = k == 0 ? 0.1 : 100.0;
              p.rel = k == 0 ? 10.0 : 1000.0;
              p.punch = k == 0 ? 100.0 : -100.0;
              p.sus = k == 0 ? -100.0 : 100.0;
              Fx fx;
              NEED(fx.ok);
              fx.set(p);
              CAPTURE(mode);
              CAPTURE(thr);
              CAPTURE(ratio);
              CAPTURE(mk);
              CAPTURE(mx);
              CAPTURE(k);
              const std::vector<float> y = fx.run(loud);
              for (float v : y) {
                NEED(std::isfinite(v));
                NEED(std::fabs(v) <= 64.1f);  // |x| <= 1, the make-up at most 15.85 and TRANSIENT's gain at most 4
                NEED(std::fpclassify(v) != FP_SUBNORMAL);
              }
            }
          }
        }
      }
    }
  }
}

TEST_CASE("comp: hostile values: NaN and infinite parameters clamp, an out-of-range or NaN input does not poison the effect for good") {
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  {
    Fx fx;
    NEED(fx.ok);
    for (std::uint32_t id = 0; id < COMP_PARAM_COUNT; ++id) {  // every parameter, Key included: it clamps like the rest (a NaN key falls to 0 = OFF)
      for (float v : {nan, inf, -inf, 1e30f, -1e30f}) {
        fx.inst.set(id, v);
      }
    }
    for (float v : fx.run(noise(2048, 9, 1.0f))) {
      NEED(std::isfinite(v));
    }
  }
  for (int mode : {0, 1}) {
    Fx fy;
    NEED(fy.ok);
    fy.inst.set(COMP_PARAM_MODE, static_cast<float>(mode));
    fy.inst.set(COMP_PARAM_PUNCH, 100.0f);
    fy.inst.set(COMP_PARAM_SUSTAIN, 100.0f);
    std::vector<float> bad = noise(256, 4);
    bad[10] = nan;
    bad[20] = inf;
    bad[30] = -inf;
    bad[40] = 1e30f;
    (void)fy.run(bad);
    const std::vector<float> after = noise(2048, 6);
    const std::vector<float> ya = fy.run(after);
    for (float v : ya) {  // the followers hold nothing poisoned: finite and bounded by the gain's ceiling
      CHECK(std::isfinite(v));
      CHECK(std::fabs(v) <= 4.0f * 0.5f + 1e-3f);
    }
  }
}

TEST_CASE("comp: set_param before the first block snaps the mix and the make-up, after it glides") {
  const std::vector<float> x = noise(512, 21, 1.0f);
  Pr q;
  q.makeup = -12.0;
  q.mix = 60.0;
  q.ratio = 1.0;
  Fx a;
  NEED(a.ok);
  a.set(q);
  Fx b;
  NEED(b.ok);
  b.inst.set(COMP_PARAM_MAKEUP, 6.0f);
  b.inst.set(COMP_PARAM_MIX, 100.0f);
  b.set(q);
  const std::vector<float> ya = a.run(x), yb = b.run(x);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  Fx c;
  NEED(c.ok);
  c.set(q);
  (void)c.run(std::vector<float>(64, 0.0f));
  c.inst.set(COMP_PARAM_MIX, 100.0f);
  c.inst.set(COMP_PARAM_MAKEUP, 6.0f);
  const std::vector<float> yc = c.run(x);
  CHECK(std::memcmp(ya.data(), yc.data(), ya.size() * sizeof(float)) != 0);
}

TEST_CASE("comp: the sidechain Key in COMP mode: a silent key is unity bit for bit, a loud key ducks and releases back, OFF is the 1.0.0 arithmetic") {
  const std::vector<float> x = noise(22050, 17, 0.9f);  // loud: the unkeyed defaults would compress this by about 8 dB
  {
    // KEY = T3 reads track index 2 (the key - 1 mapping): only index 2 is loud, the module asks for index 2, and the track ducks
    Fx fx;
    NEED(fx.ok);
    fx.inst.set(COMP_PARAM_KEY, 3.0f);  // T3
    set_key_level(2, 1.0f);
    g_key_asked = 0xFFFFFFFFu;
    const std::vector<float> y = fx.run(x);
    CHECK(g_key_asked == 2u);
    const double g = gain_db(y, x, x.size() - 4096, x.size());
    MESSAGE("Key T3, only track index 2 loud: asked index " << g_key_asked << ", steady-state gain " << g << " dB");
    CHECK(g < -6.0);
    set_key_level(2, 0.0f);
  }
  {
    // Key T1 with the key track silent (level 0): the detector follows 0, the reduction is 0, the gain is 1: the input bit for bit
    Fx fx;
    NEED(fx.ok);
    fx.inst.set(COMP_PARAM_KEY, 1.0f);
    set_key_level(0, 0.0f);
    const std::vector<float> y = fx.run(x);
    CHECK(std::memcmp(y.data(), x.data(), x.size() * sizeof(float)) == 0);
  }
  {
    // Key T1 with the key at full scale: the detector follows 1 and the gain falls to the knee's reduction of a full-scale key
    Fx fx;
    NEED(fx.ok);
    fx.inst.set(COMP_PARAM_KEY, 1.0f);
    set_key_level(0, 1.0f);
    const std::vector<float> y = fx.run(x);
    const double g = gain_db(y, x, x.size() - 4096, x.size());
    MESSAGE("key at full scale: steady-state gain " << g << " dB (the knee's reduction of a full-scale key, about -13.5 dB at the defaults)");
    CHECK(g < -6.0);
    // the key goes silent while the track keeps playing: the gain comes back through the Release (120 ms at the defaults)
    const std::vector<float> x2 = noise(44100, 18, 0.9f);
    set_key_level(0, 0.0f);
    const std::vector<float> y2 = fx.run(x2);
    const double g2 = gain_db(y2, x2, x2.size() - 4096, x2.size());
    MESSAGE("key silent again: gain " << g2 << " dB in the last 4096 samples after 1 s of release");
    CHECK(g2 > -1.0);
  }
  {
    // Key OFF, and a 1.0.0 blob (nine floats, no Key): the 1.0.0 arithmetic, against the reference that has no key — even with a loud key host
    Pr p;
    p.thr = -30.0;
    p.ratio = 8.0;
    p.att = 5.0;
    p.rel = 250.0;
    p.makeup = 3.0;
    p.mix = 80.0;
    const std::vector<float> prog = program();
    const float blob100[9] = {-30.0f, 8.0f, 5.0f, 250.0f, 3.0f, 80.0f, 0.0f, 0.0f, 0.0f};  // what a 1.0.0 project's INSX record holds
    Fx off, blob;
    NEED(off.ok && blob.ok);
    off.set(p);
    off.inst.set(COMP_PARAM_KEY, 0.0f);  // OFF, set explicitly
    for (int i = 0; i < 9; ++i) {
      blob.inst.set(static_cast<std::uint32_t>(i), blob100[i]);  // the nine floats only: the Key keeps its default 0
    }
    set_key_level(0, 1.0f);  // a loud key host: with the Key OFF it must not matter
    const std::vector<float> yo = off.run(prog), yb = blob.run(prog);
    CHECK(std::memcmp(yo.data(), yb.data(), yo.size() * sizeof(float)) == 0);  // OFF and a 1.0.0 blob are the same instance
    const std::vector<double> ref = ref_run(p, prog);  // the 1.0.0 arithmetic (the reference model has no key)
    double err = 0.0;
    for (std::size_t i = 0; i < prog.size(); ++i) {
      err = std::max(err, std::fabs(static_cast<double>(yo[i]) - ref[i]));
    }
    MESSAGE("Key OFF against the 1.0.0 reference: worst error " << err);
    CHECK(err < 1e-4);
    set_key_level(0, 0.0f);
  }
}

TEST_CASE("comp: a Key on a host without track_level (an ABI 1.11 table) reads 0: the gain stays unity") {
  Lib lib;
  NEED(lib.open(O3M_COMP_HOST_SO));
  Instance inst;
  o3m_host h = make_host();
  h.size = static_cast<std::uint32_t>(__builtin_offsetof(o3m_host, track_level));  // the table ends before track_level: O3M_HOST_HAS(host, track_level) is false
  h.track_level = nullptr;
  NEED(inst.start_with(*lib.entry, h));
  inst.set(COMP_PARAM_KEY, 5.0f);  // T5: the module cannot ask for it, reads 0
  const std::vector<float> x = noise(8192, 23, 0.9f);  // loud: the unkeyed defaults would compress this
  std::vector<float> y(x.size());
  for (std::size_t at = 0; at < x.size(); at += 64) {
    inst.m->render(inst.state, x.data() + at, y.data() + at, 64);
  }
  CHECK(std::memcmp(y.data(), x.data(), x.size() * sizeof(float)) == 0);  // unity: the input bit for bit
}

TEST_CASE("comp: TRANSIENT ignores the Key, and render_st is the mono render bit for bit on L = R, keyed or not") {
  const std::vector<float> x = program();
  {
    // TRANSIENT reads no key (the keyed detector is COMP's), so a loud key and a silent one give the same bits
    Fx a, b;
    NEED(a.ok && b.ok);
    for (Fx* f : {&a, &b}) {
      f->inst.set(COMP_PARAM_MODE, 1.0f);
      f->inst.set(COMP_PARAM_KEY, 3.0f);
      f->inst.set(COMP_PARAM_PUNCH, 60.0f);
      f->inst.set(COMP_PARAM_SUSTAIN, -40.0f);
    }
    set_key_level(2, 1.0f);  // Key T3: track index 2
    const std::vector<float> ya = a.run(x);
    set_key_level(2, 0.0f);
    const std::vector<float> yb = b.run(x);
    CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  }
  {
    // stereo: on L = R the linked gain is the mono gain, sample for sample, in COMP (keyed and not) and in TRANSIENT
    for (int mode : {0, 1}) {
      for (int keyed : {0, 1}) {
        Pr p;
        p.mode = mode;
        p.thr = -24.0;
        p.ratio = 6.0;
        p.makeup = 2.0;
        p.mix = 70.0;
        p.punch = 80.0;
        p.sus = -50.0;
        Fx mono, st;
        NEED(mono.ok && st.ok);
        mono.set(p);
        st.set(p);
        if (keyed != 0) {
          mono.inst.set(COMP_PARAM_KEY, 2.0f);
          st.inst.set(COMP_PARAM_KEY, 2.0f);
          set_key_level(1, 0.8f);  // Key T2: track index 1
        }
        const std::vector<float> ym = mono.run(x);
        std::vector<float> ol, orr;
        st.run_st(x, x, ol, orr);
        CAPTURE(mode);
        CAPTURE(keyed);
        CHECK(std::memcmp(ym.data(), ol.data(), ym.size() * sizeof(float)) == 0);
        CHECK(std::memcmp(ol.data(), orr.data(), ol.size() * sizeof(float)) == 0);
        set_key_level(1, 0.0f);
      }
    }
  }
}

TEST_CASE("comp: the factory presets: names, parameter counts and ranges, and PUNCH is the TRANSIENT it names") {
  Lib lib;
  NEED(lib.open(O3M_COMP_HOST_SO));
  const o3m_module& m = *lib.entry;
  NEED(m.preset_count == 4u);
  NEED(m.presets != nullptr);
  const char* const names[4] = {"DUCK T1", "PUMP T1", "GLUE", "PUNCH"};
  const std::uint32_t counts[4] = {10u, 10u, 4u, 8u};  // values in parameter order; past a preset's count the parameters keep their defaults
  for (std::uint32_t i = 0; i < m.preset_count; ++i) {
    CAPTURE(i);
    CHECK(std::strcmp(m.presets[i].name, names[i]) == 0);
    CHECK(m.presets[i].count == counts[i]);
    CHECK(m.presets[i].count >= 1u);
    CHECK(m.presets[i].count <= m.param_count);
    for (std::uint32_t j = 0; j < m.presets[i].count; ++j) {
      CHECK(m.presets[i].values[j] >= m.params[j].min);
      CHECK(m.presets[i].values[j] <= m.params[j].max);
    }
  }
  CHECK(m.presets[0].values[COMP_PARAM_KEY] == 1.0f);  // DUCK T1 keys T1
  CHECK(m.presets[1].values[COMP_PARAM_KEY] == 1.0f);  // PUMP T1 too
  const o3m_preset& punch = m.presets[3];
  CHECK(punch.values[COMP_PARAM_MODE] == 1.0f);  // the TRANSIENT its name implies, not COMP
  CHECK(punch.values[COMP_PARAM_PUNCH] == 100.0f);
  // a preset loads through set_param like a blob: PUNCH gives the TRANSIENT law with its Attack, Release and Punch
  {
    const std::vector<float> x = program();
    Pr p;
    p.mode = 1;
    p.att = punch.values[COMP_PARAM_ATTACK];
    p.rel = punch.values[COMP_PARAM_RELEASE];
    p.punch = punch.values[COMP_PARAM_PUNCH];
    Fx fx;
    NEED(fx.ok);
    for (std::uint32_t j = 0; j < punch.count; ++j) {
      fx.inst.set(j, punch.values[j]);
    }
    const std::vector<float> y = fx.run(x);
    const std::vector<double> ref = ref_run(p, x);
    double err = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      err = std::max(err, std::fabs(static_cast<double>(y[i]) - ref[i]));
    }
    MESSAGE("the PUNCH preset against the TRANSIENT reference: worst error " << err);
    CHECK(err < 1e-4);
  }
}

TEST_CASE("comp: the view draws the transfer law (COMP) or the gain law (TRANSIENT), under the budgets") {
  Lib lib;
  NEED(lib.open(O3M_COMP_HOST_SO));
  Instance inst;
  NEED(inst.start(*lib.entry));
  ViewHost views;
  auto params = [&](double mode, double thr, double ratio, double punch, double sus, float* p) {
    for (std::uint32_t i = 0; i < COMP_PARAM_COUNT; ++i) {
      p[i] = comp_params[i].def;
    }
    p[COMP_PARAM_MODE] = static_cast<float>(mode);
    p[COMP_PARAM_THRESHOLD] = static_cast<float>(thr);
    p[COMP_PARAM_RATIO] = static_cast<float>(ratio);
    p[COMP_PARAM_PUNCH] = static_cast<float>(punch);
    p[COMP_PARAM_SUSTAIN] = static_cast<float>(sus);
  };
  const int ybase = 140 - 3;
  for (double thr : {-60.0, -30.0, -18.0, 0.0}) {
    for (double ratio : {1.0, 4.0, 20.0}) {
      float p[COMP_PARAM_COUNT];
      params(0, thr, ratio, 0, 0, p);
      CAPTURE(thr);
      CAPTURE(ratio);
      NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, COMP_PARAM_COUNT) == ViewStatus::Ok);
      Drawing d;
      views.replay(d);
      NEED(d.polys.size() == 2u);  // the 1:1 line, then the curve
      NEED(d.polys[0].xy.size() == 4u);
      NEED(d.polys[1].xy.size() == 2u * 65u);
      NEED(d.texts.size() == 1u);
      CHECK(d.texts[0].s == "COMP");
      const std::int16_t* xy = d.polys[1].xy.data();
      for (std::uint32_t i = 0; i < 65u; ++i) {
        CHECK(xy[2 * i] >= 0);
        CHECK(xy[2 * i] < 380);
        CHECK(xy[2 * i + 1] >= 0);
        CHECK(xy[2 * i + 1] < 140);
        if (i > 0u) {
          CHECK(xy[2 * i + 1] <= xy[2 * i - 1]);  // the output never falls as the input rises
        }
      }
      if (ratio == 1.0) {
        CHECK(xy[1] == ybase);  // 1:1: the curve is the line, corner to corner
        CHECK(std::abs(xy[2 * 64 + 1] - d.polys[0].xy[3]) <= 1);
      }
      if (thr == 0.0 && ratio == 20.0) {
        CHECK(std::abs(xy[2 * 32 + 1] - (ybase - 68)) <= 1);  // -30 dB in is -30 dB out under a 0 dB threshold: the middle of the frame
      }
    }
  }
  for (double punch : {-100.0, 0.0, 100.0}) {
    for (double sus : {-100.0, 0.0, 100.0}) {
      float p[COMP_PARAM_COUNT];
      params(1, -18, 4, punch, sus, p);
      CAPTURE(punch);
      CAPTURE(sus);
      NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, COMP_PARAM_COUNT) == ViewStatus::Ok);
      Drawing d;
      views.replay(d);
      NEED(d.polys.size() == 1u);
      NEED(d.polys[0].xy.size() == 6u);
      NEED(d.texts.size() == 1u);
      CHECK(d.texts[0].s == "TRANSIENT");
      const std::int16_t* xy = d.polys[0].xy.data();
      const int ymid = xy[3];
      CHECK(ymid == ybase - static_cast<int>(0.5 * 136.0));  // d = 0: unity gain, the middle line
      CHECK((xy[1] < ymid) == (sus > 0.0));                  // Sustain on the left: positive lifts
      CHECK((xy[5] < ymid) == (punch > 0.0));                // Punch on the right
      for (std::uint32_t i = 0; i < 3u; ++i) {
        CHECK(xy[2 * i + 1] >= 0);
        CHECK(xy[2 * i + 1] < 140);
      }
    }
  }
  // the minimum size the manifest asks for also draws
  float p[COMP_PARAM_COUNT];
  params(0, -18, 4, 0, 0, p);
  CHECK(views.draw(*lib.entry, inst.state, 1, 0, 120, 80, p, COMP_PARAM_COUNT) == ViewStatus::Ok);
  params(1, -18, 4, 50, 50, p);
  CHECK(views.draw(*lib.entry, inst.state, 1, 0, 120, 80, p, COMP_PARAM_COUNT) == ViewStatus::Ok);
}

TEST_CASE("comp: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  // modules/CMakeLists.txt (o3m_add_test) makes this test depend on o3m_comp_tmod whenever the cross compiler is there, so the file exists; without the compiler load_tmod fails the test
  // when O3_CI=1 (tools/ci.sh exports it) and says so otherwise, so a CI run cannot pass on a missing file.
  const Tmod t = load_tmod(O3M_COMP_TMOD);
  if (!t.present) {
    MESSAGE("comp.tmod absent (no cross compiler): the loader checks did not run");
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_EFFECT);
  CHECK(t.h.abi_minor == 12u);  // ABI 1.12 (OS-INSFX-ABI2): stereo, the sidechain key, the factory presets
  CHECK(t.h.module_version == O3M_VERSION(1, 1, 0));
  CHECK(t.h.param_count == COMP_PARAM_COUNT);
  CHECK(t.h.view_count == COMP_VIEW_COUNT);
  CHECK(t.h.state_bytes <= COMP_STATE_BYTES);
  CHECK(t.h.hot_size <= 2048u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: the skill's ceiling is about 2.5 KB (DRIVE's hot part is 1,328 B, CRUSH's 720 B)
  MESSAGE("comp.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
