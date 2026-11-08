// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX1 (new Community OS behaviour): the CRUSH effect module driven through the ABI as the OS drives an insert, against a double-precision reference of the same maths (the
// dead-zone quantiser with nearbyint, the sample-and-hold with the same integer phase step): the sample-by-sample error over a grid of settings (bits, rate, mix, output), the level law
// at the defaults (the output rms never above the input's, DRIVE's lesson; Bits 16 is the identity on the Q15 grid), the hold's structure and length, Mix 0 as the dry track bit for bit
// (also after a wet period), silence in giving silence out, block-size independence and in == out, the mix and output ramps, the tail contract, extremes (finite, bounded, no
// subnormals, a hold that ends), hostile values, the view and the packed file through the loader.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-179).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "crush_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;

// The reference quantiser in double: v = x * 2^(bits-1) (exact), m = round-to-nearest-even(|v| - 1/4) = floor(|v| + 1/4) off the ties, y = sign(x) m / 2^(bits-1).
double ref_quant(double x, int bits) {
  const double l = std::ldexp(1.0, bits - 1);
  const double m = std::nearbyint(std::fabs(x * l) - 0.25);
  const double y = m / l;
  return x < 0.0 ? -y : y;
}

// The hold's phase step the module derives for a factor (the same float expression: the oracle checks the maths, not the float division), or 0 for no hold.
std::uint32_t ref_inc(double rate) {
  const float h = static_cast<float>(rate);
  if (!(h > 1.0f)) {
    return 0;
  }
  const float f = 4294967296.0f / h;
  return f < 4.0e9f ? static_cast<std::uint32_t>(f) + 128u : 0xFFFFFFFFu;
}

std::vector<double> ref_run(int bits, double rate, double mix_pct, double out_db, const std::vector<float>& x) {
  const std::uint32_t inc = ref_inc(rate);
  const double m = mix_pct / 100.0;
  const double og = std::pow(10.0, out_db / 20.0);
  std::uint32_t ph = 0u - inc;
  double held = 0.0;
  std::vector<double> y(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double q = ref_quant(x[i], bits);
    double wet = q;
    if (inc != 0u) {
      ph += inc;
      if (ph < inc) {
        held = q;
      }
      wet = held;
    }
    y[i] = (1.0 - m) * og * x[i] + m * og * wet;
  }
  return y;
}

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  Fx() { ok = lib.open(O3M_CRUSH_HOST_SO) && inst.start(*lib.entry); }
  void set(int bits, double rate, double mix_pct, double out_db) {
    inst.set(CRUSH_PARAM_BITS, static_cast<float>(bits));
    inst.set(CRUSH_PARAM_RATE, static_cast<float>(rate));
    inst.set(CRUSH_PARAM_MIX, static_cast<float>(mix_pct));
    inst.set(CRUSH_PARAM_OUTPUT, static_cast<float>(out_db));
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

}  // namespace

TEST_CASE("crush: the descriptor says an effect with four parameters and a view") {
  Lib lib;
  NEED(lib.open(O3M_CRUSH_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == 4u);
  CHECK(CRUSH_PARAM_RATE == 0);  // first, so the bench's moving-parameter row (the first MOD target) sweeps the hold's worst case
  CHECK(CRUSH_PARAM_BITS == 1);
  CHECK(CRUSH_PARAM_MIX == 2);
  CHECK(CRUSH_PARAM_OUTPUT == 3);
  CHECK((m.params[CRUSH_PARAM_BITS].flags & O3M_PARAM_INT) != 0u);
  CHECK(m.params[CRUSH_PARAM_BITS].min == 1.0f);
  CHECK(m.params[CRUSH_PARAM_BITS].max == 16.0f);
  CHECK(m.params[CRUSH_PARAM_BITS].def == 8.0f);
  CHECK((m.params[CRUSH_PARAM_RATE].flags & O3M_PARAM_LOG) != 0u);
  CHECK(m.params[CRUSH_PARAM_RATE].min == 1.0f);
  CHECK(m.params[CRUSH_PARAM_RATE].max == 64.0f);
  CHECK(m.params[CRUSH_PARAM_RATE].def == 1.0f);  // no hold at the defaults
  CHECK(O3M_PARAM_UNIT(m.params[CRUSH_PARAM_MIX].flags) == O3M_UNIT_PERCENT);
  CHECK(m.params[CRUSH_PARAM_MIX].def == 100.0f);
  CHECK(O3M_PARAM_UNIT(m.params[CRUSH_PARAM_OUTPUT].flags) == O3M_UNIT_DB);
  CHECK(m.params[CRUSH_PARAM_OUTPUT].min == -24.0f);
  CHECK(m.params[CRUSH_PARAM_OUTPUT].max == 6.0f);
  CHECK(m.params[CRUSH_PARAM_OUTPUT].def == 0.0f);
  for (std::uint32_t i = 0; i < 4u; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);  // MIDI Learn may bind each of them
    CHECK((m.params[i].flags & O3M_PARAM_MOD) != 0u);       // a modulation target: set_param does a few integer operations at most
  }
  CHECK(m.view_count == 1u);
  CHECK(sizeof(float) * 12u <= CRUSH_STATE_BYTES);
}

TEST_CASE("crush: the output tracks a double-precision reference over a grid of settings") {
  const std::vector<float> x = noise(4096, 7, 1.0f);  // full scale: Bits 1 (a 3/4 threshold) and every other depth see both the dead zone and the steps
  const int bits[] = {1, 2, 4, 8, 12, 16};
  const double rates[] = {1.0, 2.0, 2.5, 3.0, 7.0, 10.3, 64.0};
  const double mixes[] = {0.0, 50.0, 100.0};
  const double outs[] = {-24.0, 0.0, 6.0};
  double worst_err = 0.0, worst_snr = 1e9;
  for (int b : bits) {
    for (double r : rates) {
      for (double mx : mixes) {
        for (double ou : outs) {
          Fx fx;
          NEED(fx.ok);
          fx.set(b, r, mx, ou);
          const std::vector<float> y = fx.run(x);
          const std::vector<double> ref = ref_run(b, r, mx, ou, x);
          double es = 0, ee = 0, err = 0;
          for (std::size_t i = 0; i < x.size(); ++i) {
            const double e = static_cast<double>(y[i]) - ref[i];
            es += ref[i] * ref[i];
            ee += e * e;
            err = std::max(err, std::fabs(e));
          }
          CAPTURE(b);
          CAPTURE(r);
          CAPTURE(mx);
          CAPTURE(ou);
          CHECK(err < 1e-5);
          if (es > 1.0) {
            const double snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
            CHECK_MESSAGE(snr > 90.0, "snr " << snr);
            worst_snr = std::min(worst_snr, snr);
          }
          worst_err = std::max(worst_err, err);
        }
      }
    }
  }
  MESSAGE("worst max error " << worst_err << " (limit 1e-5), worst SNR " << worst_snr << " dB (limit 90) over 378 settings");
}

TEST_CASE("crush: the level law: at the defaults the output rms is never above the input's, Bits 16 is the identity on the Q15 grid, a Mix sweep stays within 0.3 dB") {
  // The lesson of DRIVE (+12..16 dB over bypass at its defaults): the defaults (Bits 8, Rate 1, Mix 100, Output 0 dB) must not raise the level. Sines and noise from -60 to -6 dBFS: the
  // dead-zone quantiser's output power stays under the input's at every level, where a plain rounding makes a signal near one step 3 dB louder.
  double worst_up = -1e9, worst_down = 1e9;
  const double freqs[] = {100.0, 440.0, 1234.5, 3000.0};
  for (int db = -60; db <= -6; db += 2) {
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
      if (ro > 0.0) {
        worst_up = std::max(worst_up, 20.0 * std::log10(ro / ri));
        if (db >= -36) {
          worst_down = std::min(worst_down, 20.0 * std::log10(ro / ri));
        }
      }
    }
    for (std::uint32_t seed : {1u, 2u}) {
      const std::vector<float> x = noise(22050, seed, static_cast<float>(amp * 1.7320508));  // a uniform noise of rms amp
      Fx fx;
      NEED(fx.ok);
      const std::vector<float> y = fx.run(x);
      CAPTURE(db);
      CHECK(rms(y) <= rms(x));
      if (rms(y) > 0.0) {
        worst_up = std::max(worst_up, 20.0 * std::log10(rms(y) / rms(x)));
      }
    }
  }
  MESSAGE("defaults: the largest rise over bypass " << worst_up << " dB (must be <= 0), the largest fall from -36 dBFS peak up " << worst_down << " dB");
  CHECK(worst_up <= 0.0);
  CHECK(worst_down > -1.5);  // the dead zone's cost on a quiet signal at 8 bits (measured: -1.39 dB, at -36 dBFS peak)

  // Bits 16 at Rate 1, Mix 100, Output 0 dB is the identity on a Q15 signal (the converter's own grid)
  std::vector<float> q15(8192);
  std::uint32_t s = 5;
  for (auto& v : q15) {
    s = s * 1664525u + 1013904223u;
    v = static_cast<float>(static_cast<std::int32_t>(s) >> 16) / 32768.0f;
  }
  Fx id;
  NEED(id.ok);
  id.set(16, 1.0, 100.0, 0.0);
  const std::vector<float> yq = id.run(q15);
  CHECK(std::memcmp(yq.data(), q15.data(), q15.size() * sizeof(float)) == 0);

  // a Mix sweep at 8 bits on a -20 dBFS sine moves the level by less than 0.3 dB (the wet is the dry within a step, less the dead zone's 0.23 dB at this level)
  constexpr std::size_t M = 8192;
  const double fe = std::round(1000.0 * M / kFs) * kFs / M;
  const std::vector<float> xs = sine(2 * M, 0.1, fe);
  auto amp = [&](const std::vector<float>& sig) {
    double a = 0, b = 0;
    for (std::size_t i = M; i < 2 * M; ++i) {
      const double ph = 2.0 * kPi * fe * static_cast<double>(i) / kFs;
      a += sig[i] * std::sin(ph);
      b += sig[i] * std::cos(ph);
    }
    return std::hypot(a, b) * 2.0 / M / 0.1;
  };
  double lo = 1e9, hi = 0;
  for (double m : {0.0, 25.0, 50.0, 75.0, 100.0}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(8, 1.0, m, 0.0);
    const double g = amp(fx.run(xs));
    lo = std::min(lo, g);
    hi = std::max(hi, g);
  }
  MESSAGE("Mix sweep 0..100 % at Bits 8: gain " << lo << " to " << hi);
  CHECK(20.0 * std::log10(hi / lo) < 0.3);
}

TEST_CASE("crush: the quantiser is odd, bounded by full scale and has a level at 0") {
  const std::vector<float> x = noise(4096, 13, 1.0f);
  std::vector<float> nx(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    nx[i] = -x[i];
  }
  for (int b : {1, 2, 3, 8, 16}) {
    Fx a, c;
    NEED(a.ok);
    NEED(c.ok);
    a.set(b, 1.0, 100.0, 0.0);
    c.set(b, 1.0, 100.0, 0.0);
    const std::vector<float> ya = a.run(x), yc = c.run(nx);
    CAPTURE(b);
    for (std::size_t i = 0; i < x.size(); ++i) {
      NEED(yc[i] == -ya[i]);
      NEED(std::fabs(ya[i]) <= 1.0f);
    }
    // +-1 is exactly +-2^(Bits-1) steps; 0 is a level; the levels are multiples of the step
    Fx e;
    NEED(e.ok);
    e.set(b, 1.0, 100.0, 0.0);
    const std::vector<float> edge = {1.0f, -1.0f, 0.0f, 1.0f};
    const std::vector<float> ye = e.run(edge);
    CHECK(ye[0] == 1.0f);
    CHECK(ye[1] == -1.0f);
    CHECK(ye[2] == 0.0f);
    const float step = std::ldexp(1.0f, 1 - b);
    for (float v : ya) {
      const float k = v / step;
      NEED(k == std::floor(k));
    }
  }
}

TEST_CASE("crush: the hold's structure and length: a whole Rate holds exactly Rate samples, a fractional one Rate on average") {
  const std::vector<float> x = noise(1000, 3, 1.0f);
  for (int n : {2, 3, 4, 5, 16, 64}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(16, n, 100.0, 0.0);
    const std::vector<float> y = fx.run(x);
    CAPTURE(n);
    for (std::size_t i = 0; i < x.size(); ++i) {
      const double want = ref_quant(x[(i / static_cast<std::size_t>(n)) * static_cast<std::size_t>(n)], 16);  // the first sample of each group of n
      NEED(static_cast<double>(y[i]) == want);
    }
  }
  const std::vector<float> long_x = noise(200000, 4, 1.0f);
  for (double r : {1.5, 2.5, 10.3, 33.3, 64.0}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(16, r, 100.0, 0.0);
    const std::vector<float> y = fx.run(long_x);
    std::size_t changes = 0;
    for (std::size_t i = 1; i < y.size(); ++i) {
      changes += y[i] != y[i - 1] ? 1u : 0u;
    }
    const double mean = static_cast<double>(y.size()) / static_cast<double>(changes + 1);
    CAPTURE(r);
    MESSAGE("Rate " << r << ": mean hold " << mean << " samples");
    CHECK(std::fabs(mean - r) < 0.005 * r);
  }
  // a hold takes the quantised value of the sample, and a held value never changes by itself: the output under Rate 64 has at most one value per 64 samples
  Fx fx;
  NEED(fx.ok);
  fx.set(4, 64.0, 100.0, 0.0);
  const std::vector<float> y = fx.run(sine(4096, 0.8, 440.0));
  std::size_t changes = 0;
  for (std::size_t i = 1; i < y.size(); ++i) {
    changes += y[i] != y[i - 1] ? 1u : 0u;
  }
  CHECK(changes <= 4096u / 64u);
}

TEST_CASE("crush: Mix 0 is the input bit for bit, also after a wet period") {
  const std::vector<float> x = noise(4096, 11);
  Fx dry;
  NEED(dry.ok);
  dry.set(2, 64.0, 0.0, 0.0);  // Output 0 dB: the output level is exactly 1
  const std::vector<float> yd = dry.run(x);
  CHECK(std::memcmp(yd.data(), x.data(), x.size() * sizeof(float)) == 0);

  // wet first, then Mix 0: the mix glides to 0 (about 22 blocks at 64 frames, then it snaps), and from then on the output is the input bit for bit
  Fx fx;
  NEED(fx.ok);
  fx.set(2, 64.0, 100.0, 0.0);
  (void)fx.run(noise(4096, 3, 0.5f));
  fx.inst.set(CRUSH_PARAM_MIX, 0.0f);
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

TEST_CASE("crush: silence in gives silence out, with and without the hold, and the hold's tail ends") {
  for (double rate : {1.0, 3.7, 64.0}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(4, rate, 100.0, 0.0);
    const std::vector<float> zero(64, 0.0f);
    std::vector<float> out(64);
    CAPTURE(rate);
    CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);  // never fed: idle at once, exact zeros
    for (float v : out) {
      CHECK(v == 0.0f);
    }
    // a loud block, then silence: the held value is still sounding at the end of the block that held it, the effect says so, and within two blocks it is 0
    std::vector<float> loud(64, 0.0f);
    loud[0] = 1.0f;
    const std::uint32_t first = fx.inst.m->render(fx.inst.state, loud.data(), out.data(), 64);
    std::uint32_t blocks = 0;
    std::uint32_t last = first;
    while (last != 0u) {
      last = fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64);
      NEED(++blocks < 4u);
    }
    MESSAGE("rate " << rate << ": first block returned " << first << ", the hold ended after " << blocks << " silent block(s)");
    for (int again = 0; again < 2; ++again) {
      CHECK(fx.inst.m->render(fx.inst.state, zero.data(), out.data(), 64) == 0u);
      for (float v : out) {
        CHECK(v == 0.0f);  // exact zeros once it says 0
      }
    }
    // live input is reported live
    std::vector<float> sig(64, 0.1f);
    CHECK(fx.inst.m->render(fx.inst.state, sig.data(), out.data(), 64) == 1u);
  }
}

TEST_CASE("crush: any block size gives the same bits (static parameters), and in == out is the same as two buffers") {
  const std::vector<float> x = noise(3000, 5, 1.0f);
  Fx base;
  NEED(base.ok);
  base.set(5, 3.7, 70.0, -6.0);
  const std::vector<float> y = base.run(x, 64);
  for (std::uint32_t block : {1u, 3u, 16u, 17u, 100u, 128u}) {
    Fx fx;
    fx.set(5, 3.7, 70.0, -6.0);
    const std::vector<float> z = fx.run(x, block);
    CAPTURE(block);
    CHECK(std::memcmp(y.data(), z.data(), y.size() * sizeof(float)) == 0);
  }
  Fx alias;
  alias.set(5, 3.7, 70.0, -6.0);
  std::vector<float> buf = x;
  for (std::size_t at = 0; at < buf.size(); at += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, buf.size() - at));
    alias.inst.m->render(alias.inst.state, buf.data() + at, buf.data() + at, n);
  }
  CHECK(std::memcmp(y.data(), buf.data(), y.size() * sizeof(float)) == 0);
}

TEST_CASE("crush: the Output and the Mix glide: no step where the same change done at once clicks") {
  const std::size_t n = 8192, at = 2048;
  // Output 0 to -24 dB on a transparent crusher (Bits 16): the gain ramp is the only thing that moves
  {
    const std::vector<float> x = sine(n, 0.5, 440.0);
    Fx fx;
    NEED(fx.ok);
    fx.set(16, 1.0, 100.0, 0.0);
    std::vector<float> y(n);
    for (std::size_t p = 0; p < n; p += 64) {
      if (p == at) {
        fx.inst.set(CRUSH_PARAM_OUTPUT, -24.0f);
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
  // Mix 0 to 100 % at Bits 2: every sample is a blend of the dry and the wet sample, the blend only rises, in small steps
  {
    const std::vector<float> x = sine(n, 0.9, 440.0);
    Fx fx;
    NEED(fx.ok);
    fx.set(2, 1.0, 0.0, 0.0);
    std::vector<float> y(n);
    for (std::size_t p = 0; p < n; p += 64) {
      if (p == at) {
        fx.inst.set(CRUSH_PARAM_MIX, 100.0f);
      }
      fx.inst.m->render(fx.inst.state, x.data() + p, y.data() + p, 64);
    }
    double prev = -1.0, worst_back = 0.0, worst_step = 0.0, top = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double q = ref_quant(x[i], 2);
      if (std::fabs(q - x[i]) < 0.05) {
        continue;  // the wet and the dry are too close here to read the blend
      }
      const double m = (static_cast<double>(y[i]) - x[i]) / (q - x[i]);
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

TEST_CASE("crush: extremes: every depth and rate on full-scale input stay finite and bounded, with no subnormals, and the hold ends") {
  const std::vector<float> loud = noise(4096, 3, 1.0f);
  for (int b : {1, 2, 16}) {
    for (double r : {1.0, 64.0}) {
      for (double mx : {0.0, 100.0}) {
        for (double ou : {-24.0, 6.0}) {
          Fx fx;
          NEED(fx.ok);
          fx.set(b, r, mx, ou);
          CAPTURE(b);
          CAPTURE(r);
          CAPTURE(mx);
          CAPTURE(ou);
          std::vector<float> in = loud;
          in[100] = 1.0f;
          in[101] = -1.0f;
          const std::vector<float> y = fx.run(in);
          for (float v : y) {
            NEED(std::isfinite(v));
            NEED(std::fabs(v) <= 2.0f * 1.0001f);  // |x|, |q| <= 1 and the output level <= 2
            NEED(std::fpclassify(v) != FP_SUBNORMAL);
          }
          std::uint32_t blocks = 0;
          const std::vector<float> zero(64, 0.0f);
          std::vector<float> o(64);
          while (fx.inst.m->render(fx.inst.state, zero.data(), o.data(), 64) != 0u) {
            NEED(++blocks < 4u);
          }
        }
      }
    }
  }
}

TEST_CASE("crush: hostile values: NaN and infinite parameters clamp, an out-of-range or NaN input does not poison the effect for good") {
  Fx fx;
  NEED(fx.ok);
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  for (std::uint32_t id = 0; id < 5; ++id) {  // (id 4 does not exist: ignored)
    for (float v : {nan, inf, -inf, 1e30f, -1e30f}) {
      fx.inst.set(id, v);
    }
  }
  for (float v : fx.run(noise(2048, 9, 1.0f))) {
    NEED(std::isfinite(v));
  }
  for (int b : {1, 8}) {
    for (double r : {1.0, 64.0}) {
      Fx fy;
      NEED(fy.ok);
      fy.set(b, r, 100.0, 0.0);
      std::vector<float> bad = noise(256, 4);
      bad[10] = nan;
      bad[20] = inf;
      bad[30] = -inf;
      bad[40] = 1e30f;
      (void)fy.run(bad);
      for (float v : fy.run(noise(256, 6))) {  // the quantiser and the hold hold nothing poisoned
        CHECK(std::isfinite(v));
        CHECK(std::fabs(v) <= 1.0f);
      }
    }
  }
}

TEST_CASE("crush: set_param before the first block snaps the mix and the output, after it glides; Bits and Rate act at once") {
  const std::vector<float> x = noise(512, 21, 1.0f);
  Fx a;
  a.set(5, 4.0, 60.0, -12.0);
  Fx b;
  b.set(8, 1.0, 100.0, 6.0);
  b.set(5, 4.0, 60.0, -12.0);
  const std::vector<float> ya = a.run(x), yb = b.run(x);
  CHECK(std::memcmp(ya.data(), yb.data(), ya.size() * sizeof(float)) == 0);
  Fx c;
  c.set(5, 4.0, 60.0, -12.0);
  (void)c.run(std::vector<float>(64, 0.0f));
  c.inst.set(CRUSH_PARAM_MIX, 100.0f);
  c.inst.set(CRUSH_PARAM_OUTPUT, 6.0f);
  const std::vector<float> yc = c.run(x);
  CHECK(std::memcmp(ya.data(), yc.data(), ya.size() * sizeof(float)) != 0);
  // Bits at once: the next sample is quantised at the new depth
  Fx d;
  NEED(d.ok);
  d.set(16, 1.0, 100.0, 0.0);
  (void)d.run(std::vector<float>(64, 0.0f));
  d.inst.set(CRUSH_PARAM_BITS, 2.0f);
  const std::vector<float> yd = d.run(std::vector<float>(64, 0.3f));  // 0.3 is under the 3/8 threshold at 2 bits (step 1/2, dead zone to 3/4 of it): 0
  for (float v : yd) {
    CHECK(v == 0.0f);
  }
}

TEST_CASE("crush: the view draws the quantiser's staircase and the depth's name, under the budgets") {
  Lib lib;
  NEED(lib.open(O3M_CRUSH_HOST_SO));
  Instance inst;
  NEED(inst.start(*lib.entry));
  ViewHost views;
  for (int bits = 1; bits <= 16; ++bits) {
    float p[CRUSH_PARAM_COUNT];
    for (std::uint32_t i = 0; i < CRUSH_PARAM_COUNT; ++i) {
      p[i] = crush_params[i].def;
    }
    p[CRUSH_PARAM_BITS] = static_cast<float>(bits);
    CAPTURE(bits);
    NEED(views.draw(*lib.entry, inst.state, 1, 0, 380, 140, p, CRUSH_PARAM_COUNT) == ViewStatus::Ok);
    Drawing d;
    views.replay(d);
    NEED(d.polys.size() == 1u);
    NEED(d.polys[0].xy.size() == 2u * 65u);
    NEED(d.texts.size() == 1u);
    CHECK(d.texts[0].s == std::to_string(bits) + " BIT");
    const std::int16_t* xy = d.polys[0].xy.data();
    for (std::uint32_t i = 0; i < 65u; ++i) {
      CHECK(xy[2 * i] >= 0);
      CHECK(xy[2 * i] < 380);
      CHECK(xy[2 * i + 1] >= 0);
      CHECK(xy[2 * i + 1] < 140);
    }
    const int ymid = (140 - 1) / 2;
    const float ys = static_cast<float>(140 - 3) / 2.0f;
    auto py = [&](float y) { return static_cast<std::int16_t>(ymid - static_cast<int>(y * ys)); };
    CHECK(xy[2 * 32 + 1] == py(0.0f));   // x = 0: a level
    CHECK(xy[1] == py(-1.0f));           // the ends are full scale at every depth
    CHECK(xy[2 * 64 + 1] == py(1.0f));
  }
  // the minimum size the manifest asks for also draws
  float p[CRUSH_PARAM_COUNT];
  for (std::uint32_t i = 0; i < CRUSH_PARAM_COUNT; ++i) {
    p[i] = crush_params[i].def;
  }
  CHECK(views.draw(*lib.entry, inst.state, 1, 0, 120, 80, p, CRUSH_PARAM_COUNT) == ViewStatus::Ok);
}

TEST_CASE("crush: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_CRUSH_TMOD);
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
  CHECK(t.h.param_count == CRUSH_PARAM_COUNT);
  CHECK(t.h.view_count == CRUSH_VIEW_COUNT);
  CHECK(t.h.state_bytes <= CRUSH_STATE_BYTES);
  CHECK(t.h.hot_size <= 1536u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: keep the hot part under about 1.5 KiB (DRIVE's is 1,328 B; the skill's ceiling is 2.5 KB)
  MESSAGE("crush.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
