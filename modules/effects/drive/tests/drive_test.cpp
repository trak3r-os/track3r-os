// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): the DRIVE effect module driven through the ABI as the OS drives an insert, against a double-precision reference of the same maths: the
// sample-by-sample error on seeded noise over a grid of settings (shapes, drives, tones, mixes, output levels), the DC blocker removing an offset, Mix 0 as the dry track bit for bit
// (also after a wet period), silence in giving silence out, block-size independence and in == out, the parameter ramps' steps, the tail contract, extremes (finite, bounded, no
// subnormals, a ring that ends), hostile values, the view and the packed file through the loader.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-176).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "drive_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;
constexpr double kBias = 0.25;   // the module's ASYM bias
constexpr double kDcR = 0.995;   // the module's DC blocker pole

// The reference: the same maths in double, with the coefficients the module derives from the parameters (valid sample by sample from the first one: the parameters are set before
// the first render, so the module snaps to them and its per-sample values are constant).
struct Ref {
  double g, b, a, m, og;
};

Ref ref_of(int shape, double drive_db, double tone_hz, double mix_pct, double out_db) {
  Ref r;
  r.g = (2.0 / 3.0) * std::pow(10.0, drive_db / 20.0);  // the module folds 2/3 into the control-rate gain (small-signal unity)
  r.b = shape != 0 ? kBias : 0.0;
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

double ref_shape(double u, double b) {
  double v = u + b;
  v = v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
  return v * (1.5 - 0.5 * v * v) - b * (1.5 - 0.5 * b * b);  // through 0 at zero input, as the module's
}

std::vector<double> ref_run(const Ref& r, const std::vector<float>& x) {
  std::vector<double> y(x.size());
  double ty = 0, dx = 0, dy = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double sh = ref_shape(r.g * x[i], r.b);
    ty += r.a * (sh - ty);
    const double dcv = ty - dx + kDcR * dy;
    dx = ty;
    dy = dcv;
    y[i] = (1.0 - r.m) * r.og * x[i] + r.m * r.og * dcv;
  }
  return y;
}

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fx() { ok = lib.open(O3M_DRIVE_HOST_SO) && inst.start(*lib.entry); }
  void set(int shape, double drive_db, double tone_hz, double mix_pct, double out_db) {
    inst.set(DRIVE_PARAM_SHAPE, static_cast<float>(shape));
    inst.set(DRIVE_PARAM_DRIVE, static_cast<float>(drive_db));
    inst.set(DRIVE_PARAM_TONE, static_cast<float>(tone_hz));
    inst.set(DRIVE_PARAM_MIX, static_cast<float>(mix_pct));
    inst.set(DRIVE_PARAM_OUTPUT, static_cast<float>(out_db));
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

}  // namespace

TEST_CASE("drive: the descriptor says an effect with five parameters and a view") {
  Lib lib;
  NEED(lib.open(O3M_DRIVE_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == 5u);
  CHECK(DRIVE_PARAM_SHAPE == 0);
  CHECK(DRIVE_PARAM_DRIVE == 1);
  CHECK(DRIVE_PARAM_TONE == 2);
  CHECK(DRIVE_PARAM_MIX == 3);
  CHECK(DRIVE_PARAM_OUTPUT == 4);
  CHECK((m.params[DRIVE_PARAM_SHAPE].flags & O3M_PARAM_INT) != 0u);
  CHECK(m.params[DRIVE_PARAM_SHAPE].min == 0.0f);
  CHECK(m.params[DRIVE_PARAM_SHAPE].max == 1.0f);
  CHECK(O3M_PARAM_UNIT(m.params[DRIVE_PARAM_DRIVE].flags) == O3M_UNIT_DB);
  CHECK(m.params[DRIVE_PARAM_DRIVE].min == 0.0f);
  CHECK(m.params[DRIVE_PARAM_DRIVE].max == 36.0f);
  CHECK(m.params[DRIVE_PARAM_DRIVE].def == 12.0f);
  CHECK((m.params[DRIVE_PARAM_TONE].flags & O3M_PARAM_LOG) != 0u);
  CHECK(O3M_PARAM_UNIT(m.params[DRIVE_PARAM_TONE].flags) == O3M_UNIT_HZ);
  CHECK(m.params[DRIVE_PARAM_TONE].min == 500.0f);
  CHECK(m.params[DRIVE_PARAM_TONE].max == 20000.0f);
  CHECK(O3M_PARAM_UNIT(m.params[DRIVE_PARAM_MIX].flags) == O3M_UNIT_PERCENT);
  CHECK(O3M_PARAM_UNIT(m.params[DRIVE_PARAM_OUTPUT].flags) == O3M_UNIT_DB);
  CHECK(m.params[DRIVE_PARAM_OUTPUT].min == -24.0f);
  CHECK(m.params[DRIVE_PARAM_OUTPUT].max == 6.0f);
  for (std::uint32_t i = 0; i < 5u; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);  // MIDI Learn may bind each of them
  }
  for (std::uint32_t i = DRIVE_PARAM_DRIVE; i < 5u; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_MOD) != 0u);  // a modulation target: set_param stores a target and nothing else
  }
  CHECK(m.view_count == 1u);
  CHECK(sizeof(float) * 24u <= DRIVE_STATE_BYTES);
}

TEST_CASE("drive: the output tracks a double-precision reference of the same maths over a grid of settings") {
  const std::vector<float> x = noise(4096, 7);
  const int shapes[] = {0, 1};
  const double drives[] = {0.0, 12.0, 36.0};
  const double tones[] = {500.0, 8000.0, 20000.0};
  const double mixes[] = {0.0, 50.0, 100.0};
  const double outs[] = {-24.0, 0.0, 6.0};
  double worst_err = 0.0, worst_snr = 1e9;
  for (int shape : shapes) {
    for (double dr : drives) {
      for (double tn : tones) {
        for (double mx : mixes) {
          for (double ou : outs) {
            Fx fx;
            NEED(fx.ok);
            fx.set(shape, dr, tn, mx, ou);
            const std::vector<float> y = fx.run(x);
            const std::vector<double> r = ref_run(ref_of(shape, dr, tn, mx, ou), x);
            double es = 0, ee = 0, err = 0;
            for (std::size_t i = 0; i < x.size(); ++i) {
              const double e = static_cast<double>(y[i]) - r[i];
              es += r[i] * r[i];
              ee += e * e;
              err = std::max(err, std::fabs(e));
            }
            const double snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
            CAPTURE(shape);
            CAPTURE(dr);
            CAPTURE(tn);
            CAPTURE(mx);
            CAPTURE(ou);
            CHECK(err < 1e-4);
            CHECK_MESSAGE(snr > 70.0, "snr " << snr);
            worst_err = std::max(worst_err, err);
            worst_snr = std::min(worst_snr, snr);
          }
        }
      }
    }
  }
  MESSAGE("worst max error " << worst_err << " (limit 1e-4), worst SNR " << worst_snr << " dB (limit 70) over 108 settings");
}

TEST_CASE("drive: the level law at Drive 0 dB: small-signal unity, and a Mix sweep changes the level by less than 0.2 dB") {
  constexpr std::size_t M = 8192;
  const double f = 1000.0;
  const double k = std::round(f * M / kFs);
  const double fe = k * kFs / M;
  std::vector<float> x(2 * M);
  for (std::size_t i = 0; i < x.size(); ++i) {
    x[i] = static_cast<float>(0.1 * std::sin(2.0 * kPi * fe * static_cast<double>(i) / kFs));  // a -20 dBFS sine
  }
  // the steady-state gain at fe, by projection on sin and cos over the second half
  auto amp = [&](const std::vector<float>& sig) {
    double a = 0, b = 0;
    for (std::size_t i = M; i < 2 * M; ++i) {
      const double ph = 2.0 * kPi * fe * static_cast<double>(i) / kFs;
      a += sig[i] * std::sin(ph);
      b += sig[i] * std::cos(ph);
    }
    return std::hypot(a, b) * 2.0 / M / 0.1;
  };
  Fx dry;
  NEED(dry.ok);
  dry.set(0, 0.0, 8000.0, 0.0, 0.0);  // Mix 0: the dry track
  const double gd = amp(dry.run(x));
  Fx wet;
  NEED(wet.ok);
  wet.set(0, 0.0, 8000.0, 100.0, 0.0);  // Mix 100, Output 0 dB, Drive 0 dB, SOFT
  const double gw = amp(wet.run(x));
  MESSAGE("Drive 0 dB, -20 dBFS sine at " << fe << " Hz: dry gain " << gd << ", wet gain " << gw);
  CHECK(std::fabs(20.0 * std::log10(gw / gd)) < 0.1);  // wet and dry within 0.1 dB
  double lo = 1e9, hi = 0;
  for (double m : {0.0, 25.0, 50.0, 75.0, 100.0}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(0, 0.0, 8000.0, m, 0.0);
    const double g = amp(fx.run(x));
    CAPTURE(m);
    lo = std::min(lo, g);
    hi = std::max(hi, g);
  }
  MESSAGE("Mix sweep 0..100 % at Drive 0 dB: gain " << lo << " to " << hi);
  CHECK(20.0 * std::log10(hi / lo) < 0.2);
}

TEST_CASE("drive: the DC blocker removes an offset, in both shapes") {
  for (int shape : {0, 1}) {
    for (double dr : {0.0, 36.0}) {
      Fx fx;
      NEED(fx.ok);
      fx.set(shape, dr, 8000.0, 100.0, 0.0);
      const std::vector<float> dc(88200, 0.5f);  // 2 s of a constant: the shaper's output is a constant, the blocker must take it out
      const std::vector<float> y = fx.run(dc);
      double mean = 0, peak = 0;
      for (std::size_t i = y.size() - 22050; i < y.size(); ++i) {  // the last half second
        mean += y[i];
        peak = std::max(peak, std::fabs(static_cast<double>(y[i])));
      }
      mean /= 22050.0;
      CAPTURE(shape);
      CAPTURE(dr);
      MESSAGE("shape " << shape << " drive " << dr << ": mean of the last 0.5 s " << mean << ", peak " << peak);
      CHECK(std::fabs(mean) < 1e-3);
      CHECK(peak < 1e-3);
    }
  }
}

TEST_CASE("drive: Mix 0 is the input bit for bit, also after a wet period") {
  const std::vector<float> x = noise(4096, 11);
  Fx dry;
  NEED(dry.ok);
  dry.set(0, 12.0, 8000.0, 0.0, 0.0);  // Output 0 dB: the output level is exactly 1
  const std::vector<float> yd = dry.run(x);
  CHECK(std::memcmp(yd.data(), x.data(), x.size() * sizeof(float)) == 0);

  // wet first, then Mix 0: the mix glides to 0 (about 22 blocks at 64 frames, then it snaps), and from then on the output is the input bit for bit
  Fx fx;
  NEED(fx.ok);
  fx.set(1, 36.0, 500.0, 100.0, 0.0);
  (void)fx.run(noise(4096, 3, 0.5f));
  fx.inst.set(DRIVE_PARAM_MIX, 0.0f);
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

TEST_CASE("drive: silence in gives silence out, in both shapes") {
  for (int shape : {0, 1}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(shape, 12.0, 8000.0, 100.0, 0.0);
    const std::vector<float> zero(64, 0.0f);
    std::vector<float> out(64);
    if (shape == 0) {
      CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);  // never fed, SOFT: idle at once, exact zeros
      for (float v : out) {
        CHECK(v == 0.0f);
      }
    } else {
      // ASYM puts a DC offset on a silent input; the blocker takes it out, then the state is flushed and the output is exact zero
      std::uint32_t blocks = 0;
      bool idle = false;
      while (blocks < 200u) {
        ++blocks;
        if (fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u) {
          idle = true;
          break;
        }
      }
      CAPTURE(shape);
      MESSAGE("ASYM idle after " << blocks << " blocks");
      CHECK(idle);
      for (float v : out) {
        CHECK(v == 0.0f);  // the block that said 0 was computed from the flushed state
      }
      CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);
      for (float v : out) {
        CHECK(v == 0.0f);
      }
    }
  }
}

TEST_CASE("drive: any block size gives the same bits (static parameters), and in == out is the same as two buffers") {
  const std::vector<float> x = noise(3000, 5);
  Fx base;
  NEED(base.ok);
  base.set(1, 24.0, 3000.0, 70.0, -6.0);
  const std::vector<float> y = base.run(x, 64);
  for (std::uint32_t block : {1u, 3u, 16u, 17u, 100u, 128u}) {
    Fx fx;
    fx.set(1, 24.0, 3000.0, 70.0, -6.0);
    const std::vector<float> z = fx.run(x, block);
    CAPTURE(block);
    CHECK(std::memcmp(y.data(), z.data(), y.size() * sizeof(float)) == 0);
  }
  Fx alias;
  alias.set(1, 24.0, 3000.0, 70.0, -6.0);
  std::vector<float> buf = x;
  for (std::size_t at = 0; at < buf.size(); at += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, buf.size() - at));
    alias.inst.m->render(alias.inst.state, buf.data() + at, buf.data() + at, n);
  }
  CHECK(std::memcmp(y.data(), buf.data(), y.size() * sizeof(float)) == 0);
}

TEST_CASE("drive: a parameter step glides: no step beyond a bound where the same step done at once clicks") {
  const std::size_t n = 4096, at = 2048;
  struct Step {
    const char* name;
    std::uint32_t id;
    float to;
    int shape;
    double drive;
    double tone;
    float amp;  // a full drive saturates the sine into a square wave, whose own zero-crossing edges are ~1 a sample: the step bound is measured on a signal that stays in the shaper's smooth region
  };
  const Step steps[] = {{"Drive 0 to 36 dB", DRIVE_PARAM_DRIVE, 36.0f, 0, 0.0, 8000.0, 0.005f},
                        {"Shape SOFT to ASYM", DRIVE_PARAM_SHAPE, 1.0f, 0, 12.0, 8000.0, 0.5f},
                        {"Tone 500 Hz to 20 kHz", DRIVE_PARAM_TONE, 20000.0f, 0, 12.0, 500.0, 0.5f}};
  double drive_jump = 0.0;
  for (const Step& st : steps) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
      x[i] = static_cast<float>(st.amp * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs));
    }
    Fx fx;
    NEED(fx.ok);
    fx.set(st.shape, st.drive, st.tone, 100.0, 0.0);
    std::vector<float> y(n);
    for (std::size_t p = 0; p < n; p += 64) {
      if (p == at) {
        fx.inst.set(st.id, st.to);
      }
      fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
    }
    double jump = 0, step = 0;
    for (std::size_t i = at - 8; i < at + 400 && i + 1 < n; ++i) {
      jump = std::max(jump, std::fabs(static_cast<double>(y[i + 1]) - 2.0 * y[i] + y[i - 1]));  // the second difference: a click, not the sine
      step = std::max(step, std::fabs(static_cast<double>(y[i + 1]) - y[i]));
    }
    CAPTURE(st.name);
    MESSAGE(st.name << ": largest second difference " << jump << ", largest sample step " << step);
    CHECK(jump < 0.15);
    CHECK(step < 0.4);
    if (st.id == DRIVE_PARAM_DRIVE) {
      drive_jump = jump;
    }
    for (std::size_t i = 0; i < n; ++i) {
      NEED(std::isfinite(y[i]));
    }
  }
  // the same Drive step done at once in the reference clicks much more: the test sees a click when there is one
  {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
      x[i] = static_cast<float>(0.005 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kFs));
    }
    // the reference with the gain switched at once at `at`: recompute with drive 36 from there
    const Ref ra = ref_of(0, 0.0, 8000.0, 100.0, 0.0);
    const Ref rb = ref_of(0, 36.0, 8000.0, 100.0, 0.0);
    std::vector<double> r2(n);
    double ty = 0, dx = 0, dy = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const Ref& rr = i < at ? ra : rb;
      const double sh = ref_shape(rr.g * x[i], rr.b);
      ty += rr.a * (sh - ty);
      const double dcv = ty - dx + kDcR * dy;
      dx = ty;
      dy = dcv;
      r2[i] = rr.m * rr.og * dcv + (1.0 - rr.m) * rr.og * x[i];
    }
    double jump_ref = 0;
    for (std::size_t i = at - 8; i < at + 400 && i + 1 < n; ++i) {
      jump_ref = std::max(jump_ref, std::fabs(r2[i + 1] - 2.0 * r2[i] + r2[i - 1]));
    }
    MESSAGE("the same step unsmoothed: largest second difference " << jump_ref << ", the module's " << drive_jump);
    CHECK(jump_ref > 0.05);              // the unsmoothed step really is a click, so the bound above means something
    CHECK(jump_ref > 4.0 * drive_jump);  // and the glide is clearly smoother than it
  }
}

TEST_CASE("drive: the tail contract, 1 while the input lives or the effect rings, then 0, with exact zeros") {
  Fx fx;
  NEED(fx.ok);
  fx.set(1, 36.0, 500.0, 100.0, 0.0);  // ASYM, full drive, tone at 500 Hz: the DC blocker rings for a few thousand samples
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
  MESSAGE("ring of the drive at 36 dB, tone 500 Hz: " << blocks << " blocks (" << blocks * 64.0 / kFs << " s)");
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

TEST_CASE("drive: extremes: both shapes at 0 and 36 dB on full-scale input stay finite and bounded, with no subnormals, and ring out") {
  const std::vector<float> loud = noise(4096, 3, 1.0f);
  for (int shape : {0, 1}) {
    for (double dr : {0.0, 36.0}) {
      for (double tn : {500.0, 20000.0}) {
        for (double mx : {0.0, 100.0}) {
          for (double ou : {-24.0, 6.0}) {
            Fx fx;
            NEED(fx.ok);
            fx.set(shape, dr, tn, mx, ou);
            CAPTURE(shape);
            CAPTURE(dr);
            CAPTURE(tn);
            CAPTURE(mx);
            CAPTURE(ou);
            std::vector<float> in = loud;
            in[100] = 1.0f;
            std::vector<float> y = fx.run(in);
            for (float v : y) {
              NEED(std::isfinite(v));
              NEED(std::fabs(v) < 8.0f);  // the shaper is bounded by 1, the output level by 2: the output stays under 2 * (1 + 1.01)
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

TEST_CASE("drive: hostile values: NaN and infinite parameters clamp, a NaN input does not poison the effect for good") {
  Fx fx;
  NEED(fx.ok);
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  for (std::uint32_t id = 0; id < 5; ++id) {
    for (float v : {nan, inf, -inf, 1e30f, -1e30f}) {
      fx.inst.set(id, v);
    }
  }
  const std::vector<float> x = noise(2048, 9);
  for (float v : fx.run(x)) {
    NEED(std::isfinite(v));
  }
  Fx fy;
  fy.set(0, 12.0, 8000.0, 100.0, 0.0);
  std::vector<float> bad = noise(256, 4);
  bad[10] = nan;
  (void)fy.run(bad);
  const std::vector<float> after = fy.run(noise(256, 6));  // the state was reset at the end of the poisoned block
  for (float v : after) {
    CHECK(std::isfinite(v));
  }
}

TEST_CASE("drive: set_param before the first block snaps, set_param after it glides") {
  const std::vector<float> x = noise(512, 21);
  Fx a;
  a.set(1, 30.0, 4000.0, 60.0, -12.0);
  Fx b;
  b.set(0, 0.0, 500.0, 100.0, 6.0);
  b.set(1, 30.0, 4000.0, 60.0, -12.0);
  const std::vector<float> ya = a.run(x), yb = b.run(x);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  Fx c;
  c.set(1, 30.0, 4000.0, 60.0, -12.0);
  (void)c.run(std::vector<float>(64, 0.0f));
  c.set(0, 0.0, 500.0, 100.0, 6.0);
  const std::vector<float> yc = c.run(x);
  CHECK(std::memcmp(ya.data(), yc.data(), ya.size() * sizeof(float)) != 0);
}

TEST_CASE("drive: the view draws the transfer curve and the shape's name, under the budgets") {
  Lib lib;
  NEED(lib.open(O3M_DRIVE_HOST_SO));
  Instance inst;
  NEED(inst.start(*lib.entry));
  ViewHost views;
  const char* const names[2] = {"SOFT", "ASYM"};
  for (int shape = 0; shape < 2; ++shape) {
    for (double dr : {0.0, 12.0, 36.0}) {
      float p[DRIVE_PARAM_COUNT];
      for (std::uint32_t i = 0; i < DRIVE_PARAM_COUNT; ++i) {
        p[i] = drive_params[i].def;
      }
      p[DRIVE_PARAM_SHAPE] = static_cast<float>(shape);
      p[DRIVE_PARAM_DRIVE] = static_cast<float>(dr);
      CAPTURE(shape);
      CAPTURE(dr);
      NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, DRIVE_PARAM_COUNT) == ViewStatus::Ok);
      Drawing d;
      views.replay(d);
      NEED(d.polys.size() == 1u);
      NEED(d.polys[0].xy.size() == 2u * 65u);
      NEED(d.texts.size() == 1u);
      CHECK(d.texts[0].s == names[shape]);
      const std::int16_t* xy = d.polys[0].xy.data();
      for (std::uint32_t i = 0; i < 65u; ++i) {
        CHECK(xy[2 * i] >= 0);
        CHECK(xy[2 * i] < 380);
        CHECK(xy[2 * i + 1] >= 0);
        CHECK(xy[2 * i + 1] < 140);
      }
      // the mapping of the module: y in [-1.5, 1.5] over the canvas, the curve through 0 at x = 0
      const int ymid = (140 - 1) / 2;
      const float ys = static_cast<float>(140 - 3) / 3.0f;
      auto py = [&](float y) { return static_cast<std::int16_t>(ymid - static_cast<int>(y * ys)); };
      CHECK(xy[2 * 32 + 1] == py(0.0f));  // x = 0: both shapes pass through 0
      if (dr == 36.0) {  // full drive: the curve is clamped at the ends (the ASYM bias shifts it down)
        const float c0b = shape == 0 ? 0.0f : 0.25f * (1.5f - 0.5f * 0.25f * 0.25f);
        CHECK(xy[1] == py(-1.0f - c0b));
        CHECK(xy[2 * 64 + 1] == py(1.0f - c0b));
      }
    }
  }
  // the minimum size the manifest asks for also draws
  float p[DRIVE_PARAM_COUNT];
  for (std::uint32_t i = 0; i < DRIVE_PARAM_COUNT; ++i) {
    p[i] = drive_params[i].def;
  }
  CHECK(views.draw(*lib.entry, inst.state, 1, 0, 120, 80, p, DRIVE_PARAM_COUNT) == ViewStatus::Ok);
}

TEST_CASE("drive: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_DRIVE_TMOD);
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
  CHECK(t.h.param_count == DRIVE_PARAM_COUNT);
  CHECK(t.h.view_count == DRIVE_VIEW_COUNT);
  CHECK(t.h.state_bytes <= DRIVE_STATE_BYTES);
  CHECK(t.h.hot_size <= 1536u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: keep the hot part under about 1.5 KiB (the biquad's is 1,136 B; the skill's ceiling is 2.5 KB)
  MESSAGE("drive.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
