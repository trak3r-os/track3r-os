// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX S8 (new Community OS behaviour): the Biquad Filter effect module driven through the ABI as the OS drives an insert, against a double-precision scalar reference written from the
// cookbook's formulas: the frequency response of each mode at cutoff-relative points (with the mix folded in), the time-domain error against a double biquad on seeded noise, the mix law, block
// size independence, in == out, the smoothing of a cutoff step (against the same step done without it), the tail contract, and extremes (no NaN, no denormals, bounded, exact silence at the end of a ring).
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-170).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "biquad_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;

// Resonance in percent for a Q (the module's law: Q = 0.5 * 2^(r / 20)).
double res_of_q(double q) { return 20.0 * std::log2(q / 0.5); }

struct Coef {
  double b0, b1, b2, a1, a2;
};

// The cookbook, in double, a0 normalised, the mix folded in (out = (1 - m) x + m H x).
Coef reference(int mode, double fc, double q, double mix) {
  const double w0 = 2.0 * kPi * fc / kFs, cs = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
  double b0, b1, b2;
  switch (mode) {
    case 1: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; break;
    case 2: b0 = alpha; b1 = 0; b2 = -alpha; break;
    case 3: b0 = 1; b1 = -2 * cs; b2 = 1; break;
    default: b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; break;
  }
  const double a0 = 1 + alpha, a1 = -2 * cs / a0, a2 = (1 - alpha) / a0;
  b0 /= a0; b1 /= a0; b2 /= a0;
  return {(1 - mix) + mix * b0, (1 - mix) * a1 + mix * b1, (1 - mix) * a2 + mix * b2, a1, a2};
}

double ref_gain(const Coef& c, double f) {
  const double w = 2.0 * kPi * f / kFs;
  const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
  return std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
}

// A plain double transposed direct form II.
std::vector<double> ref_run(const Coef& c, const std::vector<float>& x) {
  std::vector<double> y(x.size());
  double z1 = 0, z2 = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double v = x[i];
    y[i] = c.b0 * v + z1;
    z1 = c.b1 * v - c.a1 * y[i] + z2;
    z2 = c.b2 * v - c.a2 * y[i];
  }
  return y;
}

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fx() { ok = lib.open(O3M_BIQUAD_HOST_SO) && inst.start(*lib.entry); }
  void set(int mode, double fc, double q_res_percent, double mix_percent) {
    inst.set(BIQUAD_PARAM_MODE, static_cast<float>(mode));
    inst.set(BIQUAD_PARAM_CUTOFF, static_cast<float>(fc));
    inst.set(BIQUAD_PARAM_RESONANCE, static_cast<float>(q_res_percent));
    inst.set(BIQUAD_PARAM_MIX, static_cast<float>(mix_percent));
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

// Steady-state gain of the module at f (rounded to an integer number of cycles of M samples), by projection on sin and cos.
double measured_gain(Fx& fx, double f, double* f_exact) {
  constexpr std::size_t M = 8192;
  const double k = std::round(f * M / kFs);
  const double fe = k * kFs / M;
  *f_exact = fe;
  std::vector<float> x(2 * M);
  for (std::size_t i = 0; i < x.size(); ++i) {
    x[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * fe * static_cast<double>(i) / kFs));
  }
  const std::vector<float> y = fx.run(x);
  double a = 0, b = 0;
  for (std::size_t i = M; i < 2 * M; ++i) {
    const double ph = 2.0 * kPi * fe * static_cast<double>(i) / kFs;
    a += y[i] * std::sin(ph);
    b += y[i] * std::cos(ph);
  }
  return std::hypot(a, b) * 2.0 / M / 0.25;
}

double db(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }

}  // namespace

TEST_CASE("biquad: the descriptor says an effect with four parameters and a view") {
  Lib lib;
  NEED(lib.open(O3M_BIQUAD_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == 4u);
  CHECK(BIQUAD_PARAM_MODE == 0);
  CHECK(BIQUAD_PARAM_CUTOFF == 1);
  CHECK(BIQUAD_PARAM_RESONANCE == 2);
  CHECK(BIQUAD_PARAM_MIX == 3);
  CHECK((m.params[BIQUAD_PARAM_MODE].flags & O3M_PARAM_INT) != 0u);
  CHECK(m.params[BIQUAD_PARAM_MODE].min == 0.0f);
  CHECK(m.params[BIQUAD_PARAM_MODE].max == 3.0f);
  CHECK((m.params[BIQUAD_PARAM_CUTOFF].flags & O3M_PARAM_LOG) != 0u);
  CHECK(O3M_PARAM_UNIT(m.params[BIQUAD_PARAM_CUTOFF].flags) == O3M_UNIT_HZ);
  for (std::uint32_t i = BIQUAD_PARAM_CUTOFF; i < 4; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_MOD) != 0u);      // a modulation target: set_param stores a target and nothing else
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);  // MIDI Learn may bind each of them
  }
  CHECK(m.view_count == 1u);
  CHECK(sizeof(float) * 32u <= BIQUAD_STATE_BYTES);
}

TEST_CASE("biquad: the frequency response of each mode matches the double reference") {
  struct Case {
    int mode;
    double q;
    double mix;
  };
  const Case cases[] = {{0, 0.7071067811865476, 100}, {1, 0.7071067811865476, 100}, {2, 0.7071067811865476, 100}, {3, 0.7071067811865476, 100},
                        {0, 8.0, 100}, {1, 8.0, 100}, {2, 4.0, 100}, {3, 4.0, 100}, {0, 0.7071067811865476, 50}, {3, 2.0, 30}};
  const double fc = 1000.0;
  const double ratios[] = {0.125, 0.25, 0.5, 0.9, 1.0, 1.1, 2.0, 4.0, 8.0};
  double worst = 0.0;
  for (const Case& c : cases) {
    Fx fx;
    NEED(fx.ok);
    fx.set(c.mode, fc, res_of_q(c.q), c.mix);
    const Coef ref = reference(c.mode, fc, c.q, c.mix / 100.0);
    for (double r : ratios) {
      double fe = 0;
      fx.inst.start(*fx.lib.entry);  // a fresh state for each point: the previous sine's tail is not in it
      fx.set(c.mode, fc, res_of_q(c.q), c.mix);
      const double got = measured_gain(fx, fc * r, &fe);
      const double want = ref_gain(ref, fe);
      const double err = std::fabs(db(got) - db(want));
      CAPTURE(c.mode);
      CAPTURE(c.q);
      CAPTURE(c.mix);
      CAPTURE(r);
      if (want > 1e-3) {  // below -60 dB the stop band is a few LSBs of float noise, tested by the time-domain case
        CHECK(err < 0.02);
        worst = std::max(worst, err);
      }
    }
  }
  MESSAGE("worst response error " << worst << " dB (limit 0.02 dB, in regions above -60 dB)");
}

TEST_CASE("biquad: the time-domain output tracks a double biquad on noise at every mode: 80 dB from 1 kHz up, 65 dB at 100 Hz") {
  const std::vector<float> x = noise(16384, 7);
  const double fcs[] = {100.0, 1000.0, 8000.0, 18000.0};
  const double qs[] = {0.5, 0.7071067811865476, 4.0, 16.0};
  double worst = 1e9;
  for (int mode = 0; mode < 4; ++mode) {
    for (double fc : fcs) {
      for (double q : qs) {
        Fx fx;
        NEED(fx.ok);
        fx.set(mode, fc, res_of_q(q), 100.0);
        const std::vector<float> y = fx.run(x);
        const std::vector<double> r = ref_run(reference(mode, fc, q, 1.0), x);
        double es = 0, ee = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
          const double e = y[i] - r[i];
          es += r[i] * r[i];
          ee += e * e;
        }
        const double snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
        CAPTURE(mode);
        CAPTURE(fc);
        CAPTURE(q);
        // Single-precision coefficients near a1 = -2 move a 100 Hz cutoff by about 1e-4 (0.003 dB, the response test above), which is -70 dB of error against the exact reference on a signal that is
        // mostly stop band; from 1 kHz up the error is the arithmetic's, 80 dB and better.
        CHECK_MESSAGE(snr > (fc < 500.0 ? 65.0 : 80.0), "snr " << snr);
        worst = std::min(worst, snr);
      }
    }
  }
  MESSAGE("worst SNR " << worst << " dB over 64 settings");
}

TEST_CASE("biquad: the lowest cutoff, 20 Hz, stays within a tenth of a dB where the float section is hardest") {
  Fx fx;
  NEED(fx.ok);
  fx.set(0, 20.0, res_of_q(0.7071067811865476), 100.0);
  const Coef ref = reference(0, 20.0, 0.7071067811865476, 1.0);
  for (double f : {10.0, 20.0, 40.0, 80.0}) {
    double fe = 0;
    fx.inst.start(*fx.lib.entry);
    fx.set(0, 20.0, res_of_q(0.7071067811865476), 100.0);
    const double got = measured_gain(fx, f, &fe);
    const double want = ref_gain(ref, fe);
    CAPTURE(f);
    MESSAGE("20 Hz low-pass at " << fe << " Hz: " << db(got) << " dB, reference " << db(want) << " dB");
    CHECK(std::fabs(db(got) - db(want)) < 0.1);
  }
}

TEST_CASE("biquad: the mix law, 0 is the input bit for bit, 100 the filter, between them a linear blend") {
  const std::vector<float> x = noise(4096, 11);
  Fx dry;
  NEED(dry.ok);
  dry.set(0, 500.0, 60.0, 0.0);
  const std::vector<float> yd = dry.run(x);
  CHECK(std::memcmp(yd.data(), x.data(), x.size() * sizeof(float)) == 0);

  Fx wet;
  wet.set(0, 500.0, 60.0, 100.0);
  const std::vector<float> yw = wet.run(x);
  for (double m : {25.0, 50.0, 80.0}) {
    Fx fx;
    fx.set(0, 500.0, 60.0, m);
    const std::vector<float> y = fx.run(x);
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      worst = std::max(worst, std::fabs(static_cast<double>(y[i]) - ((1.0 - m / 100.0) * x[i] + (m / 100.0) * yw[i])));
    }
    CAPTURE(m);
    CHECK(worst < 2e-5);
  }
}

TEST_CASE("biquad: Mix 0 after a wet period at a high Q is the dry track: the ring fades in one block, then the input bit for bit") {
  // Q 16 at 20 Hz rings for seconds. Mix 100 on noise, then Mix 0: the block where the mix settles at 0 fades the ring out (out = x + g ring), and from the next block the output is the input.
  for (const bool alias : {false, true}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(0, 20.0, 100.0, 100.0);
    const std::vector<float> wet_in = noise(4096, 3, 0.5f);
    const std::vector<float> wet = fx.run(wet_in);
    double ring = 0;  // the ring's level at the switch: what the wet output carries beyond the dry input at the last sample
    for (std::size_t i = wet.size() - 64; i < wet.size(); ++i) {
      ring = std::max(ring, std::fabs(static_cast<double>(wet[i])));
    }
    NEED(ring > 1e-3);
    fx.inst.set(BIQUAD_PARAM_MIX, 0.0f);
    // The mix glides to 0 in about 22 blocks (k = 1/4 a block, snap under 1/512); the blocks after the fade block must be the input.
    const std::vector<float> x = noise(64 * 400, 9, 0.25f);
    std::vector<float> y = x;  // the aliased case renders in place
    std::vector<float> o(x.size());
    for (std::size_t at = 0; at < x.size(); at += 64) {
      if (alias) {
        fx.inst.m->render(fx.inst.state, y.data() + at, y.data() + at, 64);
      } else {
        fx.inst.m->render(fx.inst.state, x.data() + at, o.data() + at, 64);
      }
    }
    if (!alias) {
      y = o;
    }
    // The blocks from the third second on (the old resonance lasts seconds) equal the input bit for bit.
    CHECK(std::memcmp(y.data() + 64 * 300, x.data() + 64 * 300, 64 * 100 * sizeof(float)) == 0);
    // And the ring did not linger: from some block on every block is bit exact, and that block is early (the glide, the fade block and a margin).
    std::size_t exact_from = x.size();
    for (std::size_t at = x.size(); at >= 64; at -= 64) {
      if (std::memcmp(y.data() + at - 64, x.data() + at - 64, 64 * sizeof(float)) != 0) {
        break;
      }
      exact_from = at - 64;
    }
    MESSAGE("alias " << alias << ": ring " << ring << ", bit exact from sample " << exact_from);
    CHECK(exact_from <= 64u * 30u);
    // The fade block (the last one that differs from the input): no step beyond the ring's own level, compared with a hard cut of the ring.
    NEED(exact_from >= 64);
    const std::size_t fb = exact_from - 64;
    double worst = 0;
    for (std::size_t i = 1; i < 64; ++i) {
      const double e0 = static_cast<double>(y[fb + i - 1]) - x[fb + i - 1], e1 = static_cast<double>(y[fb + i]) - x[fb + i];
      worst = std::max(worst, std::fabs(e1 - e0));  // the added ring: its sample-to-sample change
    }
    MESSAGE("fade block: largest step of the ring " << worst);
    CHECK(worst <= ring);
  }
}

TEST_CASE("biquad: any block size gives the same bits (static parameters), and in == out is the same as two buffers") {
  const std::vector<float> x = noise(3000, 5);
  Fx base;
  NEED(base.ok);
  base.set(2, 2500.0, 70.0, 80.0);
  const std::vector<float> y = base.run(x, 64);
  for (std::uint32_t block : {1u, 3u, 16u, 17u, 100u, 128u}) {
    Fx fx;
    fx.set(2, 2500.0, 70.0, 80.0);
    const std::vector<float> z = fx.run(x, block);
    CAPTURE(block);
    CHECK(std::memcmp(y.data(), z.data(), y.size() * sizeof(float)) == 0);
  }
  Fx alias;
  alias.set(2, 2500.0, 70.0, 80.0);
  std::vector<float> buf = x;
  for (std::size_t at = 0; at < buf.size(); at += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, buf.size() - at));
    alias.inst.m->render(alias.inst.state, buf.data() + at, buf.data() + at, n);
  }
  CHECK(std::memcmp(y.data(), buf.data(), y.size() * sizeof(float)) == 0);
}

TEST_CASE("biquad: a cutoff step glides: no click where the same step done at once clicks") {
  // 440 Hz sine through a low-pass that jumps from 200 Hz to 8000 Hz at a block boundary. The jump of the output between two samples: the module's against a double biquad whose coefficients switch at once.
  const std::size_t n = 4096, at = 2048;
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs));
  }
  Fx fx;
  NEED(fx.ok);
  fx.set(0, 200.0, res_of_q(0.7071067811865476), 100.0);
  std::vector<float> y(n);
  for (std::size_t p = 0; p < n; p += 64) {
    if (p == at) {
      fx.inst.set(BIQUAD_PARAM_CUTOFF, 8000.0f);
    }
    fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
  }
  // reference: switch at once
  std::vector<double> r(n);
  {
    const Coef a = reference(0, 200.0, 0.7071067811865476, 1.0), b = reference(0, 8000.0, 0.7071067811865476, 1.0);
    double z1 = 0, z2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const Coef& c = i < at ? a : b;
      r[i] = c.b0 * x[i] + z1;
      z1 = c.b1 * x[i] - c.a1 * r[i] + z2;
      z2 = c.b2 * x[i] - c.a2 * r[i];
    }
  }
  double jump_mod = 0, jump_ref = 0;
  for (std::size_t i = at - 8; i < at + 400; ++i) {  // the second difference, which a sine of this pitch keeps small, and a click does not
    jump_mod = std::max(jump_mod, std::fabs(static_cast<double>(y[i + 1]) - 2.0 * y[i] + y[i - 1]));
    jump_ref = std::max(jump_ref, std::fabs(r[i + 1] - 2.0 * r[i] + r[i - 1]));
  }
  MESSAGE("largest second difference across the step: module " << jump_mod << ", unsmoothed " << jump_ref);
  CHECK(jump_mod < 0.03);                  // a clean 440 Hz sine of 0.5 has 0.5 * w^2 = 0.002; the rest is the filter's own transient (the sine's level moves from 0.1 to 0.5 as the cutoff opens)
  CHECK(jump_mod * 2.5 < jump_ref);        // and the test sees a click when there is one
  for (std::size_t i = 0; i < n; ++i) {
    NEED(std::isfinite(y[i]));
  }
}

TEST_CASE("biquad: a mode change is a crossfade, not a step") {
  const std::size_t n = 4096, at = 2048;
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 330.0 * static_cast<double>(i) / kFs));
  }
  Fx fx;
  NEED(fx.ok);
  fx.set(0, 5000.0, res_of_q(0.7071067811865476), 100.0);  // low-pass, passing 330 Hz
  std::vector<float> y(n);
  for (std::size_t p = 0; p < n; p += 64) {
    if (p == at) {
      fx.inst.set(BIQUAD_PARAM_MODE, 1.0f);  // high-pass at 5 kHz: the sine is gone
    }
    fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
  }
  double jump = 0;
  for (std::size_t i = at - 8; i < at + 400; ++i) {
    jump = std::max(jump, std::fabs(static_cast<double>(y[i + 1]) - y[i]));
  }
  MESSAGE("largest sample step across the mode change: " << jump << " (the sine's own: " << 0.5 * 2 * kPi * 330.0 / kFs << ")");
  CHECK(jump < 0.1);
  double tail = 0;
  for (std::size_t i = n - 256; i < n; ++i) {
    tail = std::max(tail, std::fabs(static_cast<double>(y[i])));
  }
  CHECK(tail < 0.01);  // the high-pass has taken over
}

TEST_CASE("biquad: the tail contract, 1 while the input lives or the filter rings, then 0, with exact zeros") {
  Fx fx;
  NEED(fx.ok);
  fx.set(0, 100.0, 90.0, 100.0);  // a resonant low-pass at 100 Hz: a ring of a few hundred ms
  std::vector<float> silent(64, 0.0f), out(64);
  CHECK(fx.inst.m->render(fx.inst.state, silent.data(), out.data(), 64) == 0u);  // never fed: idle
  std::vector<float> imp(64, 0.0f);
  imp[0] = 1.0f;
  CHECK(fx.inst.m->render(fx.inst.state, imp.data(), out.data(), 64) == 1u);
  std::uint32_t blocks = 0;
  bool rang = false;
  while (fx.inst.m->render(fx.inst.state, silent.data(), out.data(), 64) != 0u) {
    rang = true;
    NEED(++blocks < 20000u);
  }
  CHECK(rang);
  MESSAGE("ring of a 100 Hz, Q " << std::pow(2.0, 90.0 / 20.0) * 0.5 << " low-pass: " << blocks << " blocks (" << blocks * 64.0 / kFs << " s)");
  for (float v : out) {
    CHECK(std::fabs(v) < 1.5e-5f);  // the block that said 0 holds the last of the ring: under half a Q15 step (1.5e-5), which the insert's output rounds to exact 0
  }
  CHECK(fx.inst.m->render(fx.inst.state, silent.data(), out.data(), 64) == 0u);
  for (float v : out) {
    CHECK(v == 0.0f);  // after it the state is flushed: exact zero, the host sees silence and not a denormal
  }
  // live input is reported live, even from a silent state
  std::vector<float> dc(64, 0.1f);
  CHECK(fx.inst.m->render(fx.inst.state, dc.data(), out.data(), 64) == 1u);
}

TEST_CASE("biquad: extremes: every mode at the ends of every range stays finite and bounded, with no subnormals, and rings out") {
  const std::vector<float> loud = noise(4096, 3, 1.0f);
  for (int mode = 0; mode < 4; ++mode) {
    for (double fc : {20.0, 20000.0}) {
      for (double res : {0.0, 100.0}) {
        for (double mix : {0.0, 50.0, 100.0}) {
          Fx fx;
          NEED(fx.ok);
          fx.set(mode, fc, res, mix);
          CAPTURE(mode);
          CAPTURE(fc);
          CAPTURE(res);
          CAPTURE(mix);
          std::vector<float> in = loud;
          in[100] = 1.0f;
          std::vector<float> y = fx.run(in);
          for (float v : y) {
            NEED(std::isfinite(v));
            NEED(std::fabs(v) < 200.0f);  // a Q of 16 peaks at +24 dB (x16) over a loud noise: bounded, whatever the settings
            NEED(std::fpclassify(v) != FP_SUBNORMAL);
          }
          // silence after the noise: the ring is bounded and ends at an exact zero (20 Hz at Q 16: a time constant of 0.25 s)
          std::uint32_t blocks = 0;
          const std::vector<float> zero(64, 0.0f);
          std::vector<float> o(64);
          bool sub = false;
          while (fx.inst.m->render(fx.inst.state, zero.data(), o.data(), 64) != 0u) {
            for (float v : o) {
              sub = sub || std::fpclassify(v) == FP_SUBNORMAL || !std::isfinite(v);
            }
            NEED(++blocks < 8000u);  // 11.6 s
          }
          CHECK(!sub);
          CHECK(blocks < 5000u);  // under 7.3 s
        }
      }
    }
  }
}

TEST_CASE("biquad: hostile values: NaN and infinite parameters clamp, a NaN input does not poison the filter for good") {
  Fx fx;
  NEED(fx.ok);
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  for (std::uint32_t id = 0; id < 4; ++id) {
    for (float v : {nan, inf, -inf, 1e30f, -1e30f}) {
      fx.inst.set(id, v);
    }
  }
  const std::vector<float> x = noise(2048, 9);
  for (float v : fx.run(x)) {
    NEED(std::isfinite(v));
  }
  Fx fy;
  fy.set(0, 1000.0, 10.0, 100.0);
  std::vector<float> bad = noise(256, 4);
  bad[10] = nan;
  (void)fy.run(bad);
  const std::vector<float> after = fy.run(noise(256, 6));  // the state was reset at the end of the poisoned block
  for (float v : after) {
    CHECK(std::isfinite(v));
  }
}

TEST_CASE("biquad: set_param before the first block snaps, set_param after it glides") {
  const std::vector<float> x = noise(512, 21);
  // two ways of reaching the same setting before any block: the output is the same bits as an instance that was set once
  Fx a;
  a.set(0, 300.0, 40.0, 100.0);
  Fx b;
  b.set(0, 4000.0, 10.0, 50.0);
  b.set(0, 300.0, 40.0, 100.0);
  const std::vector<float> ya = a.run(x), yb = b.run(x);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  // after a block the same change is not instantaneous
  Fx c;
  c.set(0, 300.0, 40.0, 100.0);
  (void)c.run(std::vector<float>(64, 0.0f));
  c.set(0, 4000.0, 10.0, 50.0);
  const std::vector<float> yc = c.run(x);
  CHECK(std::memcmp(ya.data(), yc.data(), ya.size() * sizeof(float)) != 0);
}

TEST_CASE("biquad: the view draws one response curve and the mode's name, under the budgets") {
  Lib lib;
  NEED(lib.open(O3M_BIQUAD_HOST_SO));
  Instance inst;
  NEED(inst.start(*lib.entry));
  ViewHost views;
  const char* const names[4] = {"LOW-PASS", "HIGH-PASS", "BAND-PASS", "NOTCH"};
  for (int mode = 0; mode < 4; ++mode) {
    float p[BIQUAD_PARAM_COUNT];
    for (std::uint32_t i = 0; i < BIQUAD_PARAM_COUNT; ++i) {
      p[i] = biquad_params[i].def;
    }
    p[BIQUAD_PARAM_MODE] = static_cast<float>(mode);
    CAPTURE(mode);
    NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, BIQUAD_PARAM_COUNT) == ViewStatus::Ok);
    Drawing d;
    views.replay(d);
    NEED(d.curves.size() == 1u);
    NEED(d.curves[0].n == 40u);
    NEED(d.texts.size() == 1u);
    CHECK(d.texts[0].s == names[mode]);
    const std::vector<float>& y = d.curve_y[0];
    for (float v : y) {
      CHECK(std::isfinite(v));
      CHECK(v >= -48.0f);
      CHECK(v <= 24.0f);
    }
    // the shape: a low-pass is up on the left and down on the right, a high-pass the reverse, a notch is dipped in the middle somewhere
    if (mode == 0) {
      CHECK(y.front() > y.back() + 20.0f);
    } else if (mode == 1) {
      CHECK(y.back() > y.front() + 20.0f);
    } else if (mode == 3) {
      float lo = y[0];
      for (float v : y) {
        lo = std::min(lo, v);
      }
      CHECK(lo < y.front() - 10.0f);
    }
  }
  // the drawn curve agrees with the reference at the points it has
  float p[BIQUAD_PARAM_COUNT] = {1.0f, 2000.0f, static_cast<float>(res_of_q(4.0)), 100.0f};
  NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, BIQUAD_PARAM_COUNT) == ViewStatus::Ok);
  Drawing d;
  views.replay(d);
  const Coef ref = reference(1, 2000.0, 4.0, 1.0);
  const double step = std::log2(1000.0) / 39.0;
  for (int i = 0; i < 40; ++i) {
    const double f = 20.0 * std::exp2(step * i);
    const double want = std::max(-48.0, std::min(24.0, db(ref_gain(ref, f))));
    CAPTURE(i);
    CHECK(std::fabs(d.curve_y[0][static_cast<std::size_t>(i)] - want) < 0.05);
  }
}

TEST_CASE("biquad: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_BIQUAD_TMOD);
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
  CHECK(t.h.param_count == BIQUAD_PARAM_COUNT);
  CHECK(t.h.view_count == BIQUAD_VIEW_COUNT);
  CHECK(t.h.state_bytes <= BIQUAD_STATE_BYTES);
  CHECK(t.h.hot_size <= 1280u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: one effect takes a kilobyte and a quarter at most (1,136 B with the mix-0 fade path)
  MESSAGE("biquad.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
