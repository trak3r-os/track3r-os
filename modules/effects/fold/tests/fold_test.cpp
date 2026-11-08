// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): the FOLD effect module driven through the ABI as the OS drives an insert, against a double-precision reference of the same maths, independent only in the fold
// itself (the reference folds by fmod, the module by its add-and-subtract rounding; the tone, the blocker and the mix are the same maths in double): what that proves is the sample-by-sample match over a grid of
// settings (fold, symmetry, tone, mix, output) - max error 1e-5, SNR over 90 dB - not that the maths is right. Beyond it: the level law (at the defaults the
// output rms is never above the input's, and at Output 0 dB at no setting either, DRIVE's lesson), the small-signal unity and linearity under the threshold, the fold's oddness at Symmetry 0 and its
// even harmonics at Symmetry over 0, the DC blocker removing the offset the asymmetric fold makes, the Tone taming the top, Mix 0 as the dry track bit for bit (also after a wet period), silence in
// giving silence out, block-size independence and in == out, the Output and Mix ramps, a Fold moved every block, the tail contract, extremes, hostile values (NaN freedom), the view and the packed file
// through the loader.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-204).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "fold_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;
constexpr double kBiasPerPercent = 0.009;  // the module's Symmetry: b = 0.009 per %
constexpr double kDcR = 0.995;             // the module's DC blocker: y = k (x - x1) + R y1
constexpr double kDcK = 0.9975;

// The reference triangle fold of period 4, by fmod: fold1(u) = u for |u| <= 1, reflected at +-1 beyond (fold1(1 + d) = 1 - d).
double ref_fold1(double u) {
  double w = std::fmod(u + 1.0, 4.0);
  if (w < 0.0) {
    w += 4.0;
  }
  return w <= 2.0 ? w - 1.0 : 3.0 - w;
}

struct Ref {
  double g, b, a, m, og;
};

Ref ref_of(double fold_db, double sym_pct, double tone_hz, double mix_pct, double out_db) {
  Ref r;
  r.g = std::pow(10.0, fold_db / 20.0);
  r.b = sym_pct * kBiasPerPercent;
  double hi = kFs * 0.45;
  if (hi > 20000.0) {
    hi = 20000.0;
  }
  const double fc = tone_hz < 500.0 ? 500.0 : (tone_hz > hi ? hi : tone_hz);
  r.a = 1.0 - std::exp(-2.0 * kPi * fc / kFs);
  r.m = mix_pct / 100.0;
  r.og = std::pow(10.0, out_db / 20.0);
  return r;
}

// The wet signal before the filters and the 1/g: fold1(g x + b) - b.
double ref_wet_raw(const Ref& r, double x) { return ref_fold1(r.g * x + r.b) - r.b; }

std::vector<double> ref_run(const Ref& r, const std::vector<float>& x) {
  std::vector<double> y(x.size());
  double ty = 0, dx = 0, dy = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    ty += r.a * (ref_wet_raw(r, x[i]) - ty);
    const double dcv = kDcK * (ty - dx) + kDcR * dy;
    dx = ty;
    dy = dcv;
    y[i] = (1.0 - r.m) * r.og * x[i] + r.m * r.og * dcv / r.g;
  }
  return y;
}

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fx() { ok = lib.open(O3M_FOLD_HOST_SO) && inst.start(*lib.entry); }
  void set(double fold_db, double sym_pct, double tone_hz, double mix_pct, double out_db) {
    inst.set(FOLD_PARAM_FOLD, static_cast<float>(fold_db));
    inst.set(FOLD_PARAM_SYMMETRY, static_cast<float>(sym_pct));
    inst.set(FOLD_PARAM_TONE, static_cast<float>(tone_hz));
    inst.set(FOLD_PARAM_MIX, static_cast<float>(mix_pct));
    inst.set(FOLD_PARAM_OUTPUT, static_cast<float>(out_db));
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

double rms(const std::vector<float>& v) {
  double s = 0;
  for (float x : v) {
    s += static_cast<double>(x) * x;
  }
  return std::sqrt(s / static_cast<double>(v.size()));
}

// The amplitude of the component at `f` Hz over the samples [from, to), by correlation (f is a whole number of cycles over the span in the tests that use it).
double amp_at(const std::vector<float>& sig, std::size_t from, std::size_t to, double f) {
  double a = 0, b = 0;
  for (std::size_t i = from; i < to; ++i) {
    const double ph = 2.0 * kPi * f * static_cast<double>(i) / kFs;
    a += sig[i] * std::sin(ph);
    b += sig[i] * std::cos(ph);
  }
  return std::hypot(a, b) * 2.0 / static_cast<double>(to - from);
}

}  // namespace

TEST_CASE("fold: the descriptor says an effect with five parameters and a view") {
  Lib lib;
  NEED(lib.open(O3M_FOLD_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == 5u);
  CHECK(FOLD_PARAM_FOLD == 0);  // first, so the bench's moving-parameter row (the first MOD target) sweeps the derivation, the worst case
  CHECK(FOLD_PARAM_SYMMETRY == 1);
  CHECK(FOLD_PARAM_TONE == 2);
  CHECK(FOLD_PARAM_MIX == 3);
  CHECK(FOLD_PARAM_OUTPUT == 4);
  CHECK(O3M_PARAM_UNIT(m.params[FOLD_PARAM_FOLD].flags) == O3M_UNIT_DB);
  CHECK(m.params[FOLD_PARAM_FOLD].min == 0.0f);
  CHECK(m.params[FOLD_PARAM_FOLD].max == 24.0f);
  CHECK(m.params[FOLD_PARAM_FOLD].def == 9.0f);
  CHECK(O3M_PARAM_UNIT(m.params[FOLD_PARAM_SYMMETRY].flags) == O3M_UNIT_PERCENT);
  CHECK(m.params[FOLD_PARAM_SYMMETRY].min == -100.0f);
  CHECK(m.params[FOLD_PARAM_SYMMETRY].max == 100.0f);
  CHECK(m.params[FOLD_PARAM_SYMMETRY].def == 0.0f);  // symmetric at the defaults
  CHECK((m.params[FOLD_PARAM_TONE].flags & O3M_PARAM_LOG) != 0u);
  CHECK(O3M_PARAM_UNIT(m.params[FOLD_PARAM_TONE].flags) == O3M_UNIT_HZ);
  CHECK(m.params[FOLD_PARAM_TONE].min == 500.0f);
  CHECK(m.params[FOLD_PARAM_TONE].max == 20000.0f);
  CHECK(m.params[FOLD_PARAM_TONE].def == 12000.0f);
  CHECK(O3M_PARAM_UNIT(m.params[FOLD_PARAM_MIX].flags) == O3M_UNIT_PERCENT);
  CHECK(m.params[FOLD_PARAM_MIX].def == 100.0f);
  CHECK(O3M_PARAM_UNIT(m.params[FOLD_PARAM_OUTPUT].flags) == O3M_UNIT_DB);
  CHECK(m.params[FOLD_PARAM_OUTPUT].min == -24.0f);
  CHECK(m.params[FOLD_PARAM_OUTPUT].max == 6.0f);
  CHECK(m.params[FOLD_PARAM_OUTPUT].def == 0.0f);
  for (std::uint32_t i = 0; i < 5u; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);  // MIDI Learn may bind each of them
    CHECK((m.params[i].flags & O3M_PARAM_MOD) != 0u);       // a modulation target: set_param only stores a target
  }
  CHECK(m.view_count == 1u);
  CHECK(sizeof(float) * 22u <= FOLD_STATE_BYTES);
}

TEST_CASE("fold: the output tracks a double-precision reference over a grid of settings") {
  const std::vector<float> x = noise(4096, 7, 1.0f);  // full scale: every Fold folds it several times over
  const double folds[] = {0.0, 3.0, 9.0, 18.0, 24.0};
  const double syms[] = {-100.0, -40.0, 0.0, 60.0, 100.0};
  const double tones[] = {500.0, 4000.0, 12000.0, 20000.0};
  const double mixes[] = {0.0, 50.0, 100.0};
  const double outs[] = {-24.0, 0.0, 6.0};
  double worst_err = 0.0, worst_snr = 1e9;
  std::size_t runs = 0;
  for (double fo : folds) {
    for (double sy : syms) {
      for (double tn : tones) {
        for (double mx : mixes) {
          for (double ou : outs) {
            Fx fx;
            NEED(fx.ok);
            fx.set(fo, sy, tn, mx, ou);
            const std::vector<float> y = fx.run(x);
            const std::vector<double> ref = ref_run(ref_of(fo, sy, tn, mx, ou), x);
            double es = 0, ee = 0, err = 0;
            for (std::size_t i = 0; i < x.size(); ++i) {
              const double e = static_cast<double>(y[i]) - ref[i];
              es += ref[i] * ref[i];
              ee += e * e;
              err = std::max(err, std::fabs(e));
            }
            CAPTURE(fo);
            CAPTURE(sy);
            CAPTURE(tn);
            CAPTURE(mx);
            CAPTURE(ou);
            CHECK(err < 1e-5);
            if (es > 1.0) {
              const double snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
              CHECK_MESSAGE(snr > 90.0, "snr " << snr);
              worst_snr = std::min(worst_snr, snr);
            }
            worst_err = std::max(worst_err, err);
            ++runs;
          }
        }
      }
    }
  }
  MESSAGE("worst max error " << worst_err << " (limit 1e-5), worst SNR " << worst_snr << " dB (limit 90) over " << runs << " settings");
}

TEST_CASE("fold: the level law: at the defaults the output rms is never above the input's, and at Output 0 dB at no setting either") {
  // The lesson of DRIVE (+12..16 dB over bypass at its defaults): the defaults (Fold 9 dB, Symmetry 0, Tone 12 kHz, Mix 100, Output 0 dB) must not raise the level. Sines and noise from -60 to -6 dBFS
  // (and on to 0 dBFS, where the fold bites hardest): the folder never makes a sample bigger and the tone and the blocker never have a gain over 1.
  double worst_up = -1e9, worst_down = 1e9;
  const double freqs[] = {100.0, 440.0, 1234.5, 3000.0};
  for (int db = -60; db <= 0; db += 2) {
    const double amp = std::pow(10.0, db / 20.0);
    for (double f : freqs) {
      const std::vector<float> x = sine(22050, amp, f);
      Fx fx;
      NEED(fx.ok);  // the defaults: start() sets every parameter to its default
      const std::vector<float> y = fx.run(x);
      const double ri = rms(x), ro = rms(y);
      CAPTURE(db);
      CAPTURE(f);
      CHECK(ro <= ri);
      worst_up = std::max(worst_up, 20.0 * std::log10(ro / ri));
      if (f >= 400.0 && f <= 2000.0) {
        worst_down = std::min(worst_down, 20.0 * std::log10(ro / ri));
      }
    }
    for (std::uint32_t seed : {1u, 2u}) {
      const std::vector<float> x = noise(22050, seed, static_cast<float>(std::min(1.0, amp * 1.7320508)));  // a uniform noise of rms amp (the peak capped at full scale)
      Fx fx;
      NEED(fx.ok);
      const std::vector<float> y = fx.run(x);
      CAPTURE(db);
      CHECK(rms(y) <= rms(x));
      worst_up = std::max(worst_up, 20.0 * std::log10(rms(y) / rms(x)));
    }
  }
  MESSAGE("defaults: the largest rise over bypass " << worst_up << " dB (must be <= 0); the smallest level of a 400 Hz to 2 kHz sine " << worst_down << " dB");
  CHECK(worst_up <= 0.0);

  // every setting at Output 0 dB: Fold 0..24, Symmetry -100..+100, Tone 500..20000, Mix 50 and 100
  std::size_t runs = 0;
  double worst = -1e9;
  for (double fo : {0.0, 6.0, 9.0, 12.0, 18.0, 24.0}) {
    for (double sy : {-100.0, 0.0, 100.0}) {
      for (double tn : {500.0, 12000.0, 20000.0}) {
        for (double mx : {50.0, 100.0}) {
          for (int db = -60; db <= 0; db += 6) {
            const double amp = std::pow(10.0, db / 20.0);
            const std::vector<float> xs = sine(8192, amp, 440.0);
            const std::vector<float> xn = noise(8192, 3, static_cast<float>(std::min(1.0, amp * 1.7320508)));
            for (const std::vector<float>* x : {&xs, &xn}) {
              Fx fx;
              NEED(fx.ok);
              fx.set(fo, sy, tn, mx, 0.0);
              const std::vector<float> y = fx.run(*x);
              CAPTURE(fo);
              CAPTURE(sy);
              CAPTURE(tn);
              CAPTURE(mx);
              CAPTURE(db);
              CHECK(rms(y) <= rms(*x));
              worst = std::max(worst, 20.0 * std::log10(rms(y) / rms(*x)));
              ++runs;
            }
          }
        }
      }
    }
  }
  MESSAGE("Output 0 dB, " << runs << " runs over every setting: the largest rise over bypass " << worst << " dB (must be <= 0)");
  CHECK(worst <= 0.0);
}

TEST_CASE("fold: under the threshold the module is a linear unity-gain path; the threshold is 1/g of full scale") {
  constexpr std::size_t M = 8192;
  const double fe = std::round(440.0 * M / kFs) * kFs / M;
  auto gain = [&](double fold, double sym, double amp, double tone) {
    Fx fx;
    fx.set(fold, sym, tone, 100.0, 0.0);
    const std::vector<float> y = fx.run(sine(2 * M, amp, fe));
    return amp_at(y, M, 2 * M, fe) / amp;
  };
  // Fold 12 dB: threshold 0.251. Both amplitudes are under it: unity within the tone's and the blocker's gain at 440 Hz (under 0.5 %), and the same gain at both (linear)
  const double g1 = gain(12.0, 0.0, 0.05, 20000.0), g2 = gain(12.0, 0.0, 0.2, 20000.0);
  MESSAGE("Fold 12 dB: gain at 0.05 " << g1 << ", at 0.2 " << g2);
  CHECK(g1 > 0.99);
  CHECK(g1 <= 1.0);
  CHECK(std::fabs(g1 - g2) < 1e-4);
  // Fold 24 dB: threshold 0.063
  CHECK(std::fabs(gain(24.0, 0.0, 0.01, 20000.0) - g1) < 1e-3);
  // Symmetry +100 % (b = 0.9): the positive side reaches the threshold after 0.1/g of full scale (0.025 at 12 dB): 0.005 and 0.01 are both under it and linear; 0.5 is well over it and is not
  const double s1 = gain(12.0, 100.0, 0.005, 20000.0), s2 = gain(12.0, 100.0, 0.01, 20000.0);
  CHECK(std::fabs(s1 - s2) < 1e-4);
  CHECK(s1 > 0.99);
  const double over = gain(12.0, 0.0, 0.5, 20000.0);
  MESSAGE("Fold 12 dB at 0.5 (twice the threshold): fundamental gain " << over);
  CHECK(over < 0.5);  // the fold took the fundamental down
  // Fold 0 dB: the threshold is full scale: a +-1 sine never folds, whatever the Symmetry at 0
  CHECK(std::fabs(gain(0.0, 0.0, 0.9, 20000.0) - gain(0.0, 0.0, 0.1, 20000.0)) < 1e-4);
}

TEST_CASE("fold: the fold is odd at Symmetry 0 and makes even harmonics (and DC) at Symmetry over 0, and the blocker takes the DC out") {
  constexpr std::size_t M = 8192;
  const double fe = std::round(1000.0 * M / kFs) * kFs / M;
  const std::vector<float> xs = sine(2 * M, 0.5, fe);
  // odd: the response to -x is minus the response to x (to the float rounding of the two phases of the rounding trick)
  {
    std::vector<float> nx(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) {
      nx[i] = -xs[i];
    }
    Fx a, b;
    NEED(a.ok);
    NEED(b.ok);
    a.set(12.0, 0.0, 12000.0, 100.0, 0.0);
    b.set(12.0, 0.0, 12000.0, 100.0, 0.0);
    const std::vector<float> ya = a.run(xs), yb = b.run(nx);
    double worst = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
      worst = std::max(worst, std::fabs(static_cast<double>(ya[i]) + yb[i]));
    }
    MESSAGE("Symmetry 0: largest |y(x) + y(-x)| " << worst);
    CHECK(worst < 2e-6);
  }
  Fx sym, asym;
  NEED(sym.ok);
  NEED(asym.ok);
  sym.set(12.0, 0.0, 20000.0, 100.0, 0.0);
  asym.set(12.0, 100.0, 20000.0, 100.0, 0.0);
  const std::vector<float> ys = sym.run(xs), ya = asym.run(xs);
  const double f1s = amp_at(ys, M, 2 * M, fe), f2s = amp_at(ys, M, 2 * M, 2 * fe);
  const double f1a = amp_at(ya, M, 2 * M, fe), f2a = amp_at(ya, M, 2 * M, 2 * fe);
  MESSAGE("2nd harmonic over the fundamental: symmetric " << f2s / f1s << ", asymmetric " << f2a / f1a);
  CHECK(f2s < 1e-4 * f1s);  // an odd function of a sine: odd harmonics only
  CHECK(f2a > 0.02 * f1a);  // the one-sided fold: even harmonics
  // the asymmetric fold really makes DC before the blocker (the reference, no filters), and the module's output has none left
  const Ref r = ref_of(12.0, 100.0, 20000.0, 100.0, 0.0);
  double raw_mean = 0, out_mean = 0, out_peak = 0;
  for (std::size_t i = M; i < 2 * M; ++i) {
    raw_mean += ref_wet_raw(r, xs[i]) / r.g;
    out_mean += static_cast<double>(ya[i]);
    out_peak = std::max(out_peak, std::fabs(static_cast<double>(ya[i])));
  }
  raw_mean /= M;
  out_mean /= M;
  MESSAGE("Symmetry +100 %, Fold 12 dB, sine 0.5: DC of the fold " << raw_mean << ", DC of the output " << out_mean << " (peak " << out_peak << ")");
  CHECK(std::fabs(raw_mean) > 0.01);
  CHECK(std::fabs(out_mean) < 1e-3);
  // a constant at the input: the fold of a constant is a constant, and the blocker removes it
  for (double sy : {-100.0, 0.0, 100.0}) {
    for (double fo : {0.0, 12.0, 24.0}) {
      Fx fx;
      NEED(fx.ok);
      fx.set(fo, sy, 8000.0, 100.0, 0.0);
      const std::vector<float> y = fx.run(std::vector<float>(88200, 0.3f));  // 2 s
      double mean = 0, peak = 0;
      for (std::size_t i = y.size() - 22050; i < y.size(); ++i) {
        mean += y[i];
        peak = std::max(peak, std::fabs(static_cast<double>(y[i])));
      }
      mean /= 22050.0;
      CAPTURE(sy);
      CAPTURE(fo);
      CHECK(std::fabs(mean) < 1e-3);
      CHECK(peak < 1e-3);
    }
  }
}

TEST_CASE("fold: Tone tames the top: the energy above 12 kHz of a hard-folded bright sine falls with the cutoff") {
  // A 5 kHz sine at 0.9 folded 24 dB: the folds' kinks put harmonics up to Nyquist and alias the rest back. The energy above 12 kHz (a Hann DFT of the last 4096 samples) at Tone 20 kHz and 1 kHz.
  constexpr std::size_t N = 4096;
  auto top_energy = [&](double tone) {
    Fx fx;
    fx.set(24.0, 0.0, tone, 100.0, 0.0);
    const std::vector<float> y = fx.run(sine(2 * N, 0.9, 5000.0));
    double e = 0;
    std::vector<double> c(N), s(N);
    for (std::size_t n = 0; n < N; ++n) {
      c[n] = std::cos(2.0 * kPi * static_cast<double>(n) / N);
      s[n] = std::sin(2.0 * kPi * static_cast<double>(n) / N);
    }
    const std::size_t k0 = static_cast<std::size_t>(12000.0 / kFs * N);
    for (std::size_t k = k0; k <= N / 2; ++k) {
      double re = 0, im = 0;
      for (std::size_t n = 0; n < N; ++n) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(n) / N);
        const std::size_t idx = (n * k) % N;
        re += w * y[N + n] * c[idx];
        im -= w * y[N + n] * s[idx];
      }
      e += re * re + im * im;
    }
    return e;
  };
  const double wide = top_energy(20000.0), narrow = top_energy(1000.0);
  MESSAGE("energy above 12 kHz: Tone 20 kHz " << wide << ", Tone 1 kHz " << narrow << " (" << 10.0 * std::log10(narrow / wide) << " dB)");
  CHECK(wide > 0.0);
  CHECK(narrow < 0.02 * wide);  // 17 dB or more (about 21 dB in the model)
}

TEST_CASE("fold: Mix 0 is the input bit for bit, also after a wet period") {
  const std::vector<float> x = noise(4096, 11);
  Fx dry;
  NEED(dry.ok);
  dry.set(24.0, 100.0, 500.0, 0.0, 0.0);  // Output 0 dB: the output level is exactly 1
  const std::vector<float> yd = dry.run(x);
  CHECK(std::memcmp(yd.data(), x.data(), x.size() * sizeof(float)) == 0);

  // wet first, then Mix 0: the mix glides to 0 (about 22 blocks at 64 frames, then it snaps), and from then on the output is the input bit for bit
  Fx fx;
  NEED(fx.ok);
  fx.set(18.0, 50.0, 8000.0, 100.0, 0.0);
  (void)fx.run(noise(4096, 3, 0.5f));
  fx.inst.set(FOLD_PARAM_MIX, 0.0f);
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

TEST_CASE("fold: silence in gives silence out, in both symmetries") {
  for (double sy : {0.0, 100.0}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(18.0, sy, 4000.0, 100.0, 0.0);
    const std::vector<float> zero(64, 0.0f);
    std::vector<float> out(64);
    CAPTURE(sy);
    CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);  // never fed: idle at once, exact zeros
    for (float v : out) {
      CHECK(v == 0.0f);
    }
    // the wet at x = 0 is exactly 0 even with the symmetry (the fold passes through 0): a long silence after a signal ends at exact zeros
    (void)fx.run(noise(512, 8, 1.0f));
    std::uint32_t blocks = 0;
    while (fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) != 0u) {
      NEED(++blocks < 8000u);
    }
    for (int again = 0; again < 2; ++again) {
      CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);
      for (float v : out) {
        CHECK(v == 0.0f);
      }
    }
  }
}

TEST_CASE("fold: any block size gives the same bits (static parameters), and in == out is the same as two buffers") {
  const std::vector<float> x = noise(3000, 5, 1.0f);
  Fx base;
  NEED(base.ok);
  base.set(15.0, 35.0, 6000.0, 70.0, -6.0);
  const std::vector<float> y = base.run(x, 64);
  for (std::uint32_t block : {1u, 3u, 16u, 17u, 100u, 128u}) {
    Fx fx;
    fx.set(15.0, 35.0, 6000.0, 70.0, -6.0);
    const std::vector<float> z = fx.run(x, block);
    CAPTURE(block);
    CHECK(std::memcmp(y.data(), z.data(), y.size() * sizeof(float)) == 0);
  }
  Fx alias;
  alias.set(15.0, 35.0, 6000.0, 70.0, -6.0);
  std::vector<float> buf = x;
  for (std::size_t at = 0; at < buf.size(); at += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, buf.size() - at));
    alias.inst.m->render(alias.inst.state, buf.data() + at, buf.data() + at, n);
  }
  CHECK(std::memcmp(y.data(), buf.data(), y.size() * sizeof(float)) == 0);
}

TEST_CASE("fold: the Output and the Mix glide: no step where the same change done at once clicks") {
  const std::size_t n = 8192, at = 2048;
  // Output 0 to -24 dB at Fold 0 (the sine stays under the threshold: the path is linear, so the gain ramp is the only thing that moves)
  {
    const std::vector<float> x = sine(n, 0.5, 440.0);
    Fx fx;
    NEED(fx.ok);
    fx.set(0.0, 0.0, 20000.0, 100.0, 0.0);
    std::vector<float> y(n);
    for (std::size_t p = 0; p < n; p += 64) {
      if (p == at) {
        fx.inst.set(FOLD_PARAM_OUTPUT, -24.0f);
      }
      fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
    }
    double jump = 0, step = 0;
    for (std::size_t i = at - 8; i < at + 800; ++i) {
      jump = std::max(jump, std::fabs(static_cast<double>(y[i + 1]) - 2.0 * y[i] + y[i - 1]));
      step = std::max(step, std::fabs(static_cast<double>(y[i + 1]) - y[i]));
    }
    double jump_ref = 0;
    const double g1 = std::pow(10.0, -24.0 / 20.0);
    for (std::size_t i = at - 8; i < at + 800; ++i) {  // the same step at once in the reference
      auto v = [&](std::size_t k) { return static_cast<double>(x[k]) * (k < at ? 1.0 : g1); };
      jump_ref = std::max(jump_ref, std::fabs(v(i + 1) - 2.0 * v(i) + v(i - 1)));
    }
    MESSAGE("Output step: largest second difference " << jump << " (at once: " << jump_ref << "), largest sample step " << step);
    CHECK(jump < 0.08);
    CHECK(jump_ref > 0.2);
    CHECK(jump_ref > 3.0 * jump);
    CHECK(step < 0.1);
  }
  // Mix 0 to 100 % at Fold 24 on a loud sine: the wet signal does not depend on the mix (the filters sit before it), so every sample is a blend of the dry and the same run's Mix 100 output: the
  // blend only rises, in small steps
  {
    const std::vector<float> x = sine(n, 0.9, 440.0);
    Fx wet;
    NEED(wet.ok);
    wet.set(24.0, 0.0, 12000.0, 100.0, 0.0);
    const std::vector<float> w = wet.run(x);
    Fx fx;
    NEED(fx.ok);
    fx.set(24.0, 0.0, 12000.0, 0.0, 0.0);
    std::vector<float> y(n);
    for (std::size_t p = 0; p < n; p += 64) {
      if (p == at) {
        fx.inst.set(FOLD_PARAM_MIX, 100.0f);
      }
      fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
    }
    double prev = -1.0, worst_back = 0.0, worst_step = 0.0, top = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (std::fabs(w[i] - x[i]) < 0.05) {
        continue;  // the wet and the dry are too close here to read the blend
      }
      const double m = (static_cast<double>(y[i]) - x[i]) / (static_cast<double>(w[i]) - x[i]);
      CHECK(m > -1e-3);
      CHECK(m < 1.0 + 1e-3);
      if (prev >= 0.0) {
        worst_back = std::max(worst_back, prev - m);
        worst_step = std::max(worst_step, std::fabs(m - prev));
      }
      top = std::max(top, m);
      prev = m;
    }
    MESSAGE("Mix glide: largest fall " << worst_back << ", largest step " << worst_step << ", reaches " << top);
    CHECK(worst_back < 2e-3);  // it only rises
    CHECK(worst_step < 0.1);   // in small steps (an at-once change is a step of 1)
    CHECK(top > 0.999);        // and gets there
  }
}

TEST_CASE("fold: a Fold and a Symmetry moved every block (a modulation source) stay finite, bounded and under the input's level") {
  const std::vector<float> x = noise(64 * 600, 17, 1.0f);
  Fx fx;
  NEED(fx.ok);
  fx.set(9.0, 0.0, 12000.0, 100.0, 0.0);
  std::vector<float> y(x.size());
  double ei = 0, eo = 0;
  for (std::size_t b = 0; b * 64 < x.size(); ++b) {
    const double ph = 2.0 * kPi * static_cast<double>(b) / 40.0;
    fx.inst.set(FOLD_PARAM_FOLD, static_cast<float>(12.0 + 12.0 * std::sin(ph)));
    fx.inst.set(FOLD_PARAM_SYMMETRY, static_cast<float>(100.0 * std::cos(ph * 0.7)));
    fx.inst.m->render(fx.inst.state, x.data() + b * 64, y.data() + b * 64, 64);
  }
  double peak = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    NEED(std::isfinite(y[i]));
    peak = std::max(peak, std::fabs(static_cast<double>(y[i])));
    ei += static_cast<double>(x[i]) * x[i];
    eo += static_cast<double>(y[i]) * y[i];
  }
  MESSAGE("moving Fold and Symmetry: peak " << peak << ", level " << 10.0 * std::log10(eo / ei) << " dB against the input");
  CHECK(peak <= 2.0);
  CHECK(eo <= ei);
}

TEST_CASE("fold: the tail contract, 1 while the input lives or the effect rings, then 0, with exact zeros") {
  Fx fx;
  NEED(fx.ok);
  fx.set(24.0, 100.0, 500.0, 100.0, 0.0);  // full fold, asymmetric, tone at 500 Hz: the filters ring for a few thousand samples
  std::vector<float> silent(64, 0.0f), out(64);
  CHECK(fx.inst.m->render(fx.inst.state, silent.data(), out.data(), 64) == 0u);  // never fed: idle
  std::vector<float> imp(64, 0.0f);
  imp[0] = 1.0f;
  CHECK(fx.inst.m->render(fx.inst.state, imp.data(), out.data(), 64) == 1u);
  std::uint32_t blocks = 0;
  bool rang = false;
  while (fx.inst.m->render(fx.inst.state, silent.data(), out.data(), 64) != 0u) {
    rang = true;
    NEED(++blocks < 8000u);
  }
  CHECK(rang);
  MESSAGE("ring of the fold at 24 dB, tone 500 Hz: " << blocks << " blocks (" << blocks * 64.0 / kFs << " s)");
  for (float v : out) {
    CHECK(std::fabs(v) < 1.5e-5f);  // the block that said 0 holds the last of the ring: under half a Q15 step
  }
  CHECK(fx.inst.m->render(fx.inst.state, silent.data(), out.data(), 64) == 0u);
  for (float v : out) {
    CHECK(v == 0.0f);  // after it the state is flushed: exact zero, no denormal
  }
  // live input is reported live, even from a silent state
  std::vector<float> sig(64, 0.1f);
  CHECK(fx.inst.m->render(fx.inst.state, sig.data(), out.data(), 64) == 1u);
}

TEST_CASE("fold: extremes: Fold 0 and 24 dB, both symmetries, on full-scale input stay finite and bounded, with no subnormals, and ring out") {
  const std::vector<float> loud = noise(4096, 3, 1.0f);
  for (double fo : {0.0, 24.0}) {
    for (double sy : {-100.0, 100.0}) {
      for (double tn : {500.0, 20000.0}) {
        for (double mx : {0.0, 100.0}) {
          for (double ou : {-24.0, 6.0}) {
            Fx fx;
            NEED(fx.ok);
            fx.set(fo, sy, tn, mx, ou);
            CAPTURE(fo);
            CAPTURE(sy);
            CAPTURE(tn);
            CAPTURE(mx);
            CAPTURE(ou);
            std::vector<float> in = loud;
            in[100] = 1.0f;
            in[101] = -1.0f;
            const std::vector<float> y = fx.run(in);
            for (float v : y) {
              NEED(std::isfinite(v));
              NEED(std::fabs(v) < 4.0f);  // |x| <= 1, the wet is under 1 (the filters keep |fold1 - b| <= 1 and 1/g <= 1) and the output level is 2: under 2 * (1 + 1.01)
              NEED(std::fpclassify(v) != FP_SUBNORMAL);
            }
            // silence after the noise: the ring is bounded and ends at an exact zero
            std::uint32_t blocks = 0;
            const std::vector<float> zero(64, 0.0f);
            std::vector<float> o(64);
            bool sub = false;
            while (fx.inst.m->render(fx.inst.state, zero.data(), o.data(), 64) != 0u) {
              for (float v : o) {
                sub = sub || std::fpclassify(v) == FP_SUBNORMAL || !std::isfinite(v);
              }
              NEED(++blocks < 8000u);
            }
            CHECK(!sub);
            CHECK(blocks < 5000u);  // the DC blocker's pole 0.995: under 7.3 s
          }
        }
      }
    }
  }
}

TEST_CASE("fold: hostile values: NaN and infinite parameters clamp, a NaN or huge input does not poison the effect for good, and a random walk stays finite") {
  Fx fx;
  NEED(fx.ok);
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  for (std::uint32_t id = 0; id < 6; ++id) {  // (id 5 does not exist: ignored)
    for (float v : {nan, inf, -inf, 1e30f, -1e30f}) {
      fx.inst.set(id, v);
    }
  }
  for (float v : fx.run(noise(2048, 9, 1.0f))) {
    NEED(std::isfinite(v));
  }
  for (double sy : {0.0, 100.0}) {
    Fx fy;
    NEED(fy.ok);
    fy.set(12.0, sy, 8000.0, 100.0, 0.0);
    std::vector<float> bad = noise(256, 4);
    bad[10] = nan;
    bad[20] = inf;
    bad[30] = -inf;
    bad[40] = 1e30f;
    (void)fy.run(bad);
    for (float v : fy.run(noise(256, 6))) {  // the state was reset at the end of the poisoned block
      CHECK(std::isfinite(v));
      CHECK(std::fabs(v) <= 1.0f);
    }
  }
  // a huge finite input (beyond the rounding trick's exact range) comes out finite and bounded
  {
    Fx fy;
    NEED(fy.ok);
    fy.set(24.0, 0.0, 20000.0, 100.0, 0.0);
    std::vector<float> big(256, 0.0f);
    big[5] = 1e6f;
    big[6] = -1e8f;
    big[7] = 1e20f;
    big[8] = -3e38f;
    const std::vector<float> y = fy.run(big);
    (void)y;
    for (float v : fy.run(noise(256, 2))) {
      CHECK(std::isfinite(v));
    }
  }
  // a random walk of every parameter, a block at a time, over noise: finite, always
  Fx walk;
  NEED(walk.ok);
  std::uint32_t s = 12345;
  auto rnd = [&]() {
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) / 16777216.0;
  };
  const std::vector<float> in = noise(64 * 2000, 31, 1.0f);
  std::vector<float> out(64);
  bool all_finite = true;
  for (std::size_t b = 0; b < 2000; ++b) {
    walk.set(24.0 * rnd(), -100.0 + 200.0 * rnd(), 500.0 * std::pow(40.0, rnd()), 100.0 * rnd(), -24.0 + 30.0 * rnd());
    walk.inst.m->render(walk.inst.state, in.data() + b * 64, out.data(), 64);
    for (float v : out) {
      all_finite = all_finite && std::isfinite(v);
    }
  }
  CHECK(all_finite);
}

TEST_CASE("fold: set_param before the first block snaps, set_param after it glides") {
  const std::vector<float> x = noise(512, 21, 1.0f);
  Fx a;
  a.set(18.0, 40.0, 4000.0, 60.0, -12.0);
  Fx b;
  b.set(0.0, 0.0, 500.0, 100.0, 6.0);
  b.set(18.0, 40.0, 4000.0, 60.0, -12.0);
  const std::vector<float> ya = a.run(x), yb = b.run(x);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  Fx c;
  c.set(18.0, 40.0, 4000.0, 60.0, -12.0);
  (void)c.run(std::vector<float>(64, 0.0f));
  c.inst.set(FOLD_PARAM_MIX, 100.0f);
  c.inst.set(FOLD_PARAM_OUTPUT, 6.0f);
  c.inst.set(FOLD_PARAM_FOLD, 3.0f);
  const std::vector<float> yc = c.run(x);
  CHECK(std::memcmp(ya.data(), yc.data(), ya.size() * sizeof(float)) != 0);
}

TEST_CASE("fold: the view draws the wet transfer curve and the symmetry's name, under the budgets") {
  Lib lib;
  NEED(lib.open(O3M_FOLD_HOST_SO));
  Instance inst;
  NEED(inst.start(*lib.entry));
  ViewHost views;
  for (double fo : {0.0, 6.0, 12.0, 24.0}) {
    for (double sy : {0.0, 70.0}) {
      float p[FOLD_PARAM_COUNT];
      for (std::uint32_t i = 0; i < FOLD_PARAM_COUNT; ++i) {
        p[i] = fold_params[i].def;
      }
      p[FOLD_PARAM_FOLD] = static_cast<float>(fo);
      p[FOLD_PARAM_SYMMETRY] = static_cast<float>(sy);
      CAPTURE(fo);
      CAPTURE(sy);
      NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, FOLD_PARAM_COUNT) == ViewStatus::Ok);
      Drawing d;
      views.replay(d);
      NEED(d.polys.size() == 1u);
      NEED(d.polys[0].xy.size() == 2u * 65u);
      NEED(d.texts.size() == 1u);
      CHECK(d.texts[0].s == (sy != 0.0 ? "ASYM" : "FOLD"));
      const std::int16_t* xy = d.polys[0].xy.data();
      const Ref r = ref_of(fo, sy, 12000.0, 100.0, 0.0);
      const int ymid = (140 - 1) / 2;
      const double ys = static_cast<double>(140 - 3) / 2.0;
      for (std::uint32_t i = 0; i < 65u; ++i) {
        CHECK(xy[2 * i] >= 0);
        CHECK(xy[2 * i] < 380);
        CHECK(xy[2 * i + 1] >= 0);
        CHECK(xy[2 * i + 1] < 140);
        const double x = -1.0 + 2.0 * static_cast<double>(i) / 64.0;
        const double want = ymid - ref_wet_raw(r, x) / r.g * ys;  // the curve against the independent fold, to a pixel
        CHECK(std::fabs(static_cast<double>(xy[2 * i + 1]) - want) <= 1.0);
      }
      CHECK(xy[2 * 32 + 1] == ymid);  // x = 0: the wet is 0
    }
  }
  // the minimum size the manifest asks for also draws
  float p[FOLD_PARAM_COUNT];
  for (std::uint32_t i = 0; i < FOLD_PARAM_COUNT; ++i) {
    p[i] = fold_params[i].def;
  }
  CHECK(views.draw(*lib.entry, inst.state, 1, 0, 120, 80, p, FOLD_PARAM_COUNT) == ViewStatus::Ok);
}

TEST_CASE("fold: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_FOLD_TMOD);
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
  CHECK(t.h.param_count == FOLD_PARAM_COUNT);
  CHECK(t.h.view_count == FOLD_VIEW_COUNT);
  CHECK(t.h.state_bytes <= FOLD_STATE_BYTES);
  CHECK(t.h.hot_size <= 1536u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: keep the hot part under about 1.5 KiB (DRIVE's is 1,328 B; the skill's ceiling is 2.5 KB)
  MESSAGE("fold.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
