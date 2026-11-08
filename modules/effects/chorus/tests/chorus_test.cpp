// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X2 (new Community OS behaviour): the STEREO CHORUS effect module driven through the ABI as the OS drives a WIDENS insert (render_st on L and R planes), against a double-precision
// reference of the same maths: the sample-by-sample error on seeded stereo noise over a grid of settings, the taps' delays adding up to twice the Delay at every sample (a ramp input makes a tap's
// delay readable), the shortest tap at least kMinDelay at every parameter value and rate (the clamped delay included), the LFO's period, the mono sum and the side (what the effect adds to each),
// Mix 0 as the input bit for bit (also after a wet period), the parameter glides, block-size independence and in == out, the tail contract (silence in, 0 out, exact zeros, no one-shot clear), hostile
// values and a NaN in the input (the ABI's input is finite: the OS's stereo adapter builds the planes from Q15, so the module keeps no guard for one), the factory presets, and the packed file
// through the loader. Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-214..217).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "chorus_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;
constexpr double kMs = kFs * 0.001;  // frames in a millisecond (the module's `ms`)

double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

// The reference: the same maths in double, chunk by chunk of at most 64 frames as the module takes them (docs/specs/OS-INSFX-FX2.md 4.2): the mid line, two linearly interpolated taps at
// Delay -+ the LFO's swing (a parabola refined to a sine), the swing limited to Delay - 66 frames, Delay and Depth gliding toward their targets at n / 4096 a chunk, Period and Mix at n / 256,
// the delays and the mix ramping linearly over a chunk from where the last one ended, and L' = L + Mix (tap1 - M), R' = R + Mix (tap2 - M). It also keeps the taps, for the checks of the mono sum and of
// the side.
struct Ref {
  static constexpr int kLine = 2048;
  std::vector<double> line = std::vector<double>(kLine, 0.0);
  std::int64_t pos = 0;
  double t_period = 1667, t_depth = 0.4, t_delay = 12, t_mix = 0.5;
  double inc = 0, depth = 0, delay = 0, mix = 0, ph = 0, d1 = 0, d2 = 0, mx = 0;
  bool fresh = true;
  std::vector<double> tap1, tap2, mid;  // per frame, over everything run so far

  void set(double period, double depth_pct, double delay_ms, double mix_pct) {
    t_period = clampd(period, 200.0, 20000.0);
    t_depth = clampd(depth_pct, 0.0, 100.0) * 0.01;
    t_delay = clampd(delay_ms, 5.0, 25.0);
    t_mix = clampd(mix_pct, 0.0, 100.0) * 0.01;
  }
  static double lfo(double ph) {
    const double x = ph < 0.5 ? ph : ph - 1.0;
    const double p = 8.0 * x * (1.0 - 2.0 * std::fabs(x));
    return p * (0.775 + 0.225 * std::fabs(p));
  }
  double swing(double ph_) const {
    const double a = depth * 8.0 * kMs;
    const double e = std::min(a, delay - 66.0);
    return std::max(e, 0.0) * lfo(ph_);
  }
  double at(std::int64_t w0, double t) const {  // the line at (w0 + t), t < 0, interpolated
    const double fl = std::floor(t);
    const double fr = t - fl;
    const std::int64_t k = w0 + static_cast<std::int64_t>(fl);
    const double a = line[static_cast<std::size_t>(k & (kLine - 1))];
    const double b = line[static_cast<std::size_t>((k + 1) & (kLine - 1))];
    return a + fr * (b - a);
  }
  void chunk(const float* inl, const float* inr, double* ol, double* orr, std::uint32_t n) {
    std::vector<double> m(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      m[i] = 0.5 * (static_cast<double>(inl[i]) + static_cast<double>(inr[i]));
    }
    const double td = t_delay * kMs;
    const double tinc = 1.0 / (t_period * kMs);
    if (fresh) {
      inc = tinc;
      depth = t_depth;
      delay = td;
      mix = t_mix;
      d1 = delay + swing(ph);
      d2 = delay - swing(ph);
      mx = mix;
      fresh = false;
    } else {
      const double kf = n / 256.0, ks = n / 4096.0;
      inc += kf * (tinc - inc);
      depth += ks * (t_depth - depth);
      delay += ks * (td - delay);
      mix += kf * (t_mix - mix);
      if (std::fabs(t_mix - mix) < 1e-4) {
        mix = t_mix;
      }
    }
    double p1 = ph + inc * n;
    if (p1 >= 1.0) {
      p1 -= 1.0;
    }
    const double sw = swing(p1);
    const double d1e = delay + sw, d2e = delay - sw;
    const std::int64_t w0 = pos;
    for (std::uint32_t i = 0; i < n; ++i) {
      const double f = static_cast<double>(i) / n;
      const double dd1 = d1 + (d1e - d1) * f, dd2 = d2 + (d2e - d2) * f;
      const double t1 = at(w0, i - dd1), t2 = at(w0, i - dd2);
      const double mxi = mx + (mix - mx) * f;
      ol[i] = inl[i] + mxi * (t1 - m[i]);
      orr[i] = inr[i] + mxi * (t2 - m[i]);
      tap1.push_back(t1);
      tap2.push_back(t2);
      mid.push_back(m[i]);
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      line[static_cast<std::size_t>((w0 + i) & (kLine - 1))] = m[i];
    }
    pos = w0 + n;
    ph = p1;
    d1 = d1e;
    d2 = d2e;
    mx = mix;
  }
  // frames in blocks of `block` (the module cuts a block into chunks of 64 as this does)
  void run(const std::vector<float>& xl, const std::vector<float>& xr, std::vector<double>& yl, std::vector<double>& yr, std::uint32_t block = 64) {
    const std::size_t base = yl.size();
    yl.resize(base + xl.size());
    yr.resize(base + xl.size());
    for (std::size_t at_ = 0; at_ < xl.size();) {
      const std::size_t nb = std::min<std::size_t>(block, xl.size() - at_);
      for (std::size_t c = 0; c < nb;) {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, nb - c));
        chunk(xl.data() + at_ + c, xr.data() + at_ + c, yl.data() + base + at_ + c, yr.data() + base + at_ + c, n);
        c += n;
      }
      at_ += nb;
    }
  }
};

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  std::uint32_t busy = 0;
  explicit Fx(float rate = kRate) { ok = lib.open(O3M_CHORUS_HOST_SO) && inst.start(*lib.entry, rate); }
  void set(double period, double depth, double delay, double mix) {
    inst.set(CHORUS_PARAM_PERIOD, static_cast<float>(period));
    inst.set(CHORUS_PARAM_DEPTH, static_cast<float>(depth));
    inst.set(CHORUS_PARAM_DELAY, static_cast<float>(delay));
    inst.set(CHORUS_PARAM_MIX, static_cast<float>(mix));
  }
  // The stereo planes through render_st in blocks of `block`, into separate output buffers.
  void run(const std::vector<float>& xl, const std::vector<float>& xr, std::vector<float>& yl, std::vector<float>& yr, std::uint32_t block = 64) {
    const std::size_t base = yl.size();
    yl.resize(base + xl.size());
    yr.resize(base + xl.size());
    for (std::size_t at = 0; at < xl.size();) {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, xl.size() - at));
      busy = inst.m->render_st(inst.state, xl.data() + at, xr.data() + at, yl.data() + base + at, yr.data() + base + at, n);
      at += n;
    }
  }
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

// Noise with the top taken off (a one-pole at about 4 kHz): what music looks like to an interpolator, and the signal the 1e-4 limit is held on. White noise is the harshest input
// (its adjacent samples differ by up to the full amplitude) and has its own, looser, limits below.
std::vector<float> soft_noise(std::size_t n, std::uint32_t seed, float amp = 0.5f) {
  std::vector<float> x = noise(n, seed, amp);
  float z = 0.0f;
  for (auto& v : x) {
    z += 0.45f * (v - z);
    v = z * 1.4f;
  }
  return x;
}

struct Err {
  double max = 0, snr = 1e9;
};
Err compare(const std::vector<float>& y, const std::vector<double>& r) {
  Err e;
  double es = 0, ee = 0;
  for (std::size_t i = 0; i < y.size(); ++i) {
    const double d = static_cast<double>(y[i]) - r[i];
    es += r[i] * r[i];
    ee += d * d;
    e.max = std::max(e.max, std::fabs(d));
  }
  e.snr = 10.0 * std::log10(es / std::max(ee, 1e-30));
  return e;
}

// A ramp x[n] = 1e-4 n on both channels: the line holds a straight line, so a linearly interpolated tap reads it back exactly and a tap's delay is n - tap / 1e-4.
constexpr double kRampC = 1e-4;
std::vector<float> ramp(std::size_t n) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(kRampC * static_cast<double>(i));
  }
  return x;
}

}  // namespace

TEST_CASE("chorus: the descriptor says a stereo (widening) effect with four parameters, four presets and no view") {
  Lib lib;
  NEED(lib.open(O3M_CHORUS_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  CHECK(m.abi_minor == 12u);
  CHECK(m.render_st != nullptr);
  CHECK(m.fx_flags == O3M_FX_WIDENS);
  NEED(m.param_count == 4u);
  CHECK(CHORUS_PARAM_PERIOD == 0);
  CHECK(CHORUS_PARAM_DEPTH == 1);
  CHECK(CHORUS_PARAM_DELAY == 2);
  CHECK(CHORUS_PARAM_MIX == 3);
  CHECK(O3M_PARAM_UNIT(m.params[CHORUS_PARAM_PERIOD].flags) == O3M_UNIT_MS);  // a unit parameter shows whole numbers: a period in ms, not a rate in Hz
  CHECK((m.params[CHORUS_PARAM_PERIOD].flags & O3M_PARAM_LOG) != 0u);
  CHECK(m.params[CHORUS_PARAM_PERIOD].min == 200.0f);
  CHECK(m.params[CHORUS_PARAM_PERIOD].max == 20000.0f);
  CHECK(O3M_PARAM_UNIT(m.params[CHORUS_PARAM_DEPTH].flags) == O3M_UNIT_PERCENT);
  CHECK(m.params[CHORUS_PARAM_DEPTH].def == 40.0f);
  CHECK(O3M_PARAM_UNIT(m.params[CHORUS_PARAM_DELAY].flags) == O3M_UNIT_MS);
  CHECK(m.params[CHORUS_PARAM_DELAY].min == 5.0f);
  CHECK(m.params[CHORUS_PARAM_DELAY].max == 25.0f);
  CHECK(m.params[CHORUS_PARAM_DELAY].def == 12.0f);
  CHECK(O3M_PARAM_UNIT(m.params[CHORUS_PARAM_MIX].flags) == O3M_UNIT_PERCENT);
  CHECK(m.params[CHORUS_PARAM_MIX].def == 50.0f);
  for (std::uint32_t i = 0; i < 4u; ++i) {
    CHECK((m.params[i].flags & O3M_PARAM_NO_LEARN) == 0u);
    CHECK((m.params[i].flags & O3M_PARAM_MOD) != 0u);  // set_param stores a target and nothing else
  }
  CHECK(m.view_count == 0u);
  CHECK(m.state_bytes >= sizeof(float) * 2048u);
  NEED(m.preset_count == 4u);
  const char* const names[4] = {"Classic", "Slow Wide", "Vibrato", "Ensemble"};
  for (std::uint32_t i = 0; i < 4u; ++i) {
    CAPTURE(i);
    CHECK(std::string(m.presets[i].name) == names[i]);
    NEED(m.presets[i].count == 4u);
    for (std::uint32_t p = 0; p < 4u; ++p) {
      CHECK(m.presets[i].values[p] >= m.params[p].min);
      CHECK(m.presets[i].values[p] <= m.params[p].max);
    }
  }
  CHECK(m.presets[0].values[CHORUS_PARAM_MIX] == 50.0f);  // CLASSIC is the defaults but for the period (1,667 ms = 0.6 Hz)
}

TEST_CASE("chorus: the output tracks a double-precision reference of the same maths over a grid of settings (stereo noise in)") {
  const std::size_t N = 4096;
  const std::vector<float> xl = soft_noise(N, 11), xr = soft_noise(N, 29);
  const double periods[] = {200.0, 1667.0, 20000.0};
  const double depths[] = {0.0, 40.0, 100.0};
  const double delays[] = {5.0, 12.0, 25.0};
  const double mixes[] = {0.0, 50.0, 100.0};
  double worst_err = 0.0, worst_snr = 1e9;
  int settings = 0;
  for (double pe : periods) {
    for (double de : depths) {
      for (double dl : delays) {
        for (double mx : mixes) {
          Fx fx;
          NEED(fx.ok);
          fx.set(pe, de, dl, mx);
          std::vector<float> yl, yr;
          fx.run(xl, xr, yl, yr);
          Ref ref;
          ref.set(pe, de, dl, mx);
          std::vector<double> rl, rr;
          ref.run(xl, xr, rl, rr);
          const Err el = compare(yl, rl), er = compare(yr, rr);
          CAPTURE(pe);
          CAPTURE(de);
          CAPTURE(dl);
          CAPTURE(mx);
          CHECK(el.max < 1e-4);
          CHECK(er.max < 1e-4);
          if (mx > 0.0 && de > 0.0) {
            CHECK_MESSAGE(el.snr > 80.0, "left snr " << el.snr);
            CHECK_MESSAGE(er.snr > 80.0, "right snr " << er.snr);
            worst_snr = std::min(worst_snr, std::min(el.snr, er.snr));
          }
          worst_err = std::max(worst_err, std::max(el.max, er.max));
          ++settings;
        }
      }
    }
  }
  MESSAGE("worst max error " << worst_err << " (limit 1e-4), worst SNR " << worst_snr << " dB (limit 80) over " << settings << " settings, 4096 frames of band-limited stereo noise");
}

TEST_CASE("chorus: white noise, the harshest input for an interpolator, within 2e-4 and 80 dB") {
  const std::size_t N = 4096;
  const std::vector<float> xl = noise(N, 3), xr = noise(N, 5);
  const double sets[][4] = {{1667, 40, 12, 50}, {200, 100, 25, 100}, {20000, 100, 5, 100}, {455, 30, 20, 45}, {222, 25, 6, 100}, {6667, 70, 18, 60}};
  double worst_err = 0.0, worst_snr = 1e9;
  for (const auto& s : sets) {
    Fx fx;
    NEED(fx.ok);
    fx.set(s[0], s[1], s[2], s[3]);
    std::vector<float> yl, yr;
    fx.run(xl, xr, yl, yr);
    Ref ref;
    ref.set(s[0], s[1], s[2], s[3]);
    std::vector<double> rl, rr;
    ref.run(xl, xr, rl, rr);
    const Err el = compare(yl, rl), er = compare(yr, rr);
    CAPTURE(s[0]);
    CAPTURE(s[1]);
    CAPTURE(s[2]);
    CAPTURE(s[3]);
    CHECK(el.max < 2e-4);
    CHECK(er.max < 2e-4);
    CHECK_MESSAGE(el.snr > 80.0, "left snr " << el.snr);
    CHECK_MESSAGE(er.snr > 80.0, "right snr " << er.snr);
    worst_err = std::max(worst_err, std::max(el.max, er.max));
    worst_snr = std::min(worst_snr, std::min(el.snr, er.snr));
  }
  MESSAGE("white noise: worst max error " << worst_err << " (limit 2e-4), worst SNR " << worst_snr << " dB (limit 80) over 6 settings");
}

TEST_CASE("chorus: the two taps' delays add up to twice the Delay at every sample, and the shortest is 66 frames") {
  // A ramp in: with Mix 100 % and a mono input L' = tap 1 and R' = tap 2, and a tap that reads a ramp gives c (n - delay).
  const std::size_t N = 16000;
  const std::vector<float> x = ramp(N);
  for (double dl : {5.0, 12.0, 25.0}) {
    for (double dp : {20.0, 80.0, 100.0}) {
      Fx fx;
      NEED(fx.ok);
      fx.set(1667.0, dp, dl, 100.0);
      std::vector<float> yl, yr;
      fx.run(x, x, yl, yr);
      const double D = dl * 44.1;
      const double amax = std::min(dp * 0.01 * 8.0 * 44.1, D - 66.0);
      double worst_sum = 0, dmin = 1e9, dmax = -1e9;
      for (std::size_t n = 3000; n < N; ++n) {  // after the line holds the ramp (the longest tap is under 1,500 frames)
        const double d1 = static_cast<double>(n) - static_cast<double>(yl[n]) / kRampC;
        const double d2 = static_cast<double>(n) - static_cast<double>(yr[n]) / kRampC;
        worst_sum = std::max(worst_sum, std::fabs(0.5 * (d1 + d2) - D));
        dmin = std::min(dmin, std::min(d1, d2));
        dmax = std::max(dmax, std::max(d1, d2));
      }
      CAPTURE(dl);
      CAPTURE(dp);
      CHECK_MESSAGE(worst_sum < 0.02, "the mean of the two delays strays " << worst_sum << " frames from the Delay");  // (the readout's own rounding: a float near 1.5 is 1.2e-7, 1e-3 frames)
      CHECK(dmin >= 65.9);
      CHECK(dmin <= D - 0.9 * amax + 0.5);  // the swing reaches near its depth over the 16,000 frames (a quarter cycle of the 73,500: sin 0.22 cycle = 0.98)
      CHECK(dmax <= D + amax + 0.1);
      if (dl == 5.0 && dp == 100.0) {
        MESSAGE("Delay 5 ms, Depth 100 %: shortest tap " << dmin << " frames (the swing is limited to " << amax << " of the 353 asked)");
      }
    }
  }
}

TEST_CASE("chorus: the shortest tap is kMinDelay (66 frames) at every parameter value and every rate, the clamped delay included") {
  // Delay 5 ms is the parameter's minimum: 220 frames at 44.1 kHz, but under kMinDelay at a low rate, where the module clamps the delay's target. kMinDelay is guaranteed at the taps
  // themselves, not only through the swing, so a tap never reads the chunk's own slots. A ramp in with Mix 100 % reads a tap's delay back: d = n - out / kRampC.
  const std::size_t N = 16000;
  const std::vector<float> x = ramp(N);
  for (float rate : {kRate, 8000.0f}) {
    const double ms = static_cast<double>(rate) * 0.001;
    const double D = std::max(5.0 * ms, 66.0);  // the Delay in frames, clamped at kMinDelay
    for (double dp : {0.0, 60.0, 100.0}) {
      Fx fx(rate);
      NEED(fx.ok);
      fx.set(200.0, dp, 5.0, 100.0);  // the fastest LFO, the deepest swing, the shortest delay
      std::vector<float> yl, yr;
      fx.run(x, x, yl, yr);
      const double amax = std::min(dp * 0.01 * 8.0 * ms, D - 66.0);
      double dmin = 1e9, dmax = -1e9, worst_sum = 0;
      for (std::size_t n = 3000; n < N; ++n) {  // the line holds the ramp by then (the longest tap is under 1,500 frames at 44.1 kHz, 264 at 8 kHz)
        const double d1 = static_cast<double>(n) - static_cast<double>(yl[n]) / kRampC;
        const double d2 = static_cast<double>(n) - static_cast<double>(yr[n]) / kRampC;
        dmin = std::min(dmin, std::min(d1, d2));
        dmax = std::max(dmax, std::max(d1, d2));
        worst_sum = std::max(worst_sum, std::fabs(0.5 * (d1 + d2) - D));
      }
      CAPTURE(rate);
      CAPTURE(dp);
      CHECK_MESSAGE(dmin >= 65.9, "shortest tap " << dmin << " frames at " << rate << " Hz, Depth " << dp);
      CHECK_MESSAGE(worst_sum < 0.02, "the mean of the two delays strays " << worst_sum << " frames from the Delay at " << rate << " Hz");
      CHECK(dmax <= D + std::max(amax, 0.0) + 0.1);
      for (std::size_t i = 0; i < yl.size(); ++i) {
        CHECK(std::isfinite(yl[i]));
        CHECK(std::isfinite(yr[i]));
      }
    }
  }
  MESSAGE("the shortest tap stays at or above 65.9 frames at 44,100 and 8,000 Hz, Delay 5 ms, Depth 0 / 60 / 100 %");
}

TEST_CASE("chorus: the LFO's period is Period, and the swing is a sine") {
  const std::size_t N = 270000;  // 6.1 s: three rising zero crossings of the swing at a period of 1.667 s
  const std::vector<float> x = ramp(N);
  Fx fx;
  NEED(fx.ok);
  fx.set(1667.0, 100.0, 25.0, 100.0);
  std::vector<float> yl, yr;
  fx.run(x, x, yl, yr);
  const double D = 25.0 * 44.1;
  std::vector<double> cross;
  double prev = 0;
  double peak = 0;
  for (std::size_t n = 2000; n < N; ++n) {
    const double d1 = static_cast<double>(n) - static_cast<double>(yl[n]) / kRampC - D;
    peak = std::max(peak, std::fabs(d1));
    if (n > 2000 && prev < 0.0 && d1 >= 0.0) {
      cross.push_back(static_cast<double>(n - 1) + (-prev) / (d1 - prev));
    }
    prev = d1;
  }
  NEED(cross.size() >= 3u);
  const double period_ms = (cross.back() - cross.front()) / static_cast<double>(cross.size() - 1) / 44.1;
  CHECK_MESSAGE(std::fabs(period_ms - 1667.0) < 1667.0 * 0.005, "period " << period_ms << " ms");
  CHECK_MESSAGE(std::fabs(peak - 353.0) < 4.0, "peak swing " << peak << " frames (8 ms = 352.8)");
  MESSAGE("LFO period " << period_ms << " ms for Period 1667, peak swing " << peak << " frames");
}

TEST_CASE("chorus: the mono sum and the side: what the effect adds to each") {
  const std::size_t N = 6000;
  const std::vector<float> xl = soft_noise(N, 41), xr = soft_noise(N, 43);
  const double sets[][4] = {{1667, 40, 12, 50}, {200, 100, 25, 100}, {6667, 70, 18, 60}, {222, 25, 6, 100}, {1667, 0, 12, 50}};
  for (const auto& s : sets) {
    Fx fx;
    NEED(fx.ok);
    fx.set(s[0], s[1], s[2], s[3]);
    std::vector<float> yl, yr;
    fx.run(xl, xr, yl, yr);
    Ref ref;
    ref.set(s[0], s[1], s[2], s[3]);
    std::vector<double> rl, rr;
    ref.run(xl, xr, rl, rr);
    const double mix = s[3] * 0.01;
    double worst_m = 0, worst_s = 0;
    for (std::size_t i = 0; i < N; ++i) {
      const double M = 0.5 * (static_cast<double>(xl[i]) + xr[i]);
      const double S = 0.5 * (static_cast<double>(xl[i]) - xr[i]);
      const double Mo = 0.5 * (static_cast<double>(yl[i]) + yr[i]);
      const double So = 0.5 * (static_cast<double>(yl[i]) - yr[i]);
      // the mono sum is the input's mid crossfaded with the mean of the taps, and the side is the input's plus Mix x half the taps' difference: nothing else
      worst_m = std::max(worst_m, std::fabs(Mo - ((1.0 - mix) * M + mix * 0.5 * (ref.tap1[i] + ref.tap2[i]))));
      worst_s = std::max(worst_s, std::fabs((So - S) - mix * 0.5 * (ref.tap1[i] - ref.tap2[i])));
    }
    CAPTURE(s[0]);
    CAPTURE(s[1]);
    CAPTURE(s[2]);
    CAPTURE(s[3]);
    CHECK(worst_m < 1e-4);
    CHECK(worst_s < 1e-4);
    if (s[1] == 0.0) {  // no swing: both taps are the same sample, so the effect adds no side at all, and a mono sound stays centred bit for bit
      double side = 0;
      for (std::size_t i = 0; i < N; ++i) {
        side = std::max(side, std::fabs(0.5 * (static_cast<double>(yl[i]) - yr[i]) - 0.5 * (static_cast<double>(xl[i]) - xr[i])));
      }
      CHECK(side < 1e-6);
      Fx mono;
      NEED(mono.ok);
      mono.set(s[0], s[1], s[2], s[3]);
      std::vector<float> ml, mr;
      mono.run(xl, xl, ml, mr);
      CHECK(std::memcmp(ml.data(), mr.data(), ml.size() * sizeof(float)) == 0);
    }
  }
  // a mono sound with a swing gets a side, and only from the taps: the left and the right differ
  Fx fx;
  NEED(fx.ok);
  fx.set(1667, 60, 12, 80);
  std::vector<float> yl, yr;
  fx.run(xl, xl, yl, yr);
  double side = 0;
  for (std::size_t i = 600; i < N; ++i) {
    side = std::max(side, static_cast<double>(std::fabs(yl[i] - yr[i])));
  }
  CHECK(side > 0.005);
}

TEST_CASE("chorus: Mix 0 is the input bit for bit, also after a wet period") {
  const std::size_t N = 4096;
  const std::vector<float> xl = noise(N, 7), xr = noise(N, 9);
  Fx a;
  NEED(a.ok);
  a.set(1667, 100, 25, 0);
  std::vector<float> yl, yr;
  a.run(xl, xr, yl, yr);
  CHECK(std::memcmp(yl.data(), xl.data(), N * sizeof(float)) == 0);
  CHECK(std::memcmp(yr.data(), xr.data(), N * sizeof(float)) == 0);
  Fx b;
  NEED(b.ok);
  b.set(1667, 100, 25, 100);
  std::vector<float> wl, wr;
  b.run(xl, xr, wl, wr);
  CHECK(std::memcmp(wl.data(), xl.data(), N * sizeof(float)) != 0);
  b.set(1667, 100, 25, 0);
  std::vector<float> zl, zr;
  b.run(noise(8192, 13), noise(8192, 17), zl, zr);  // the mix glides to 0 in a few tens of blocks, then it is the input again
  const std::vector<float> tl = noise(8192, 13), tr = noise(8192, 17);
  CHECK(std::memcmp(zl.data() + 4096, tl.data() + 4096, 4096 * sizeof(float)) == 0);
  CHECK(std::memcmp(zr.data() + 4096, tr.data() + 4096, 4096 * sizeof(float)) == 0);
}

TEST_CASE("chorus: a parameter step glides as the reference's glide does, with no step the reference does not have") {
  const std::size_t N = 8192;
  std::vector<float> xl(2 * N + 2 * N), xr(xl.size());
  for (std::size_t i = 0; i < xl.size(); ++i) {
    xl[i] = static_cast<float>(0.4 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs));
    xr[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 777.0 * static_cast<double>(i) / kFs));
  }
  struct Step {
    double a[4], b[4];
  };
  const Step steps[] = {{{1667, 20, 12, 30}, {250, 100, 25, 100}},   // Period, Depth and Mix at once
                        {{1667, 0, 5, 100}, {1667, 0, 25, 100}},      // a Delay move alone: the pitch glide
                        {{1667, 60, 12, 100}, {1667, 60, 12, 0}}};    // Mix to 0
  for (const Step& st : steps) {
    Fx fx;
    NEED(fx.ok);
    Ref ref;
    fx.set(st.a[0], st.a[1], st.a[2], st.a[3]);
    ref.set(st.a[0], st.a[1], st.a[2], st.a[3]);
    std::vector<float> yl, yr;
    std::vector<double> rl, rr;
    const std::vector<float> l1(xl.begin(), xl.begin() + N), r1(xr.begin(), xr.begin() + N);
    const std::vector<float> l2(xl.begin() + N, xl.begin() + 3 * N), r2(xr.begin() + N, xr.begin() + 3 * N);
    fx.run(l1, r1, yl, yr);
    ref.run(l1, r1, rl, rr);
    fx.set(st.b[0], st.b[1], st.b[2], st.b[3]);
    ref.set(st.b[0], st.b[1], st.b[2], st.b[3]);
    fx.run(l2, r2, yl, yr);
    ref.run(l2, r2, rl, rr);
    const Err el = compare(yl, rl), er = compare(yr, rr);
    CAPTURE(st.b[0]);
    CAPTURE(st.b[2]);
    CHECK(el.max < 1e-4);
    CHECK(er.max < 1e-4);
    double sm = 0, sr = 0;
    for (std::size_t i = 1; i < yl.size(); ++i) {
      sm = std::max(sm, static_cast<double>(std::fabs(yl[i] - yl[i - 1])));
      sr = std::max(sr, std::fabs(rl[i] - rl[i - 1]));
    }
    CHECK_MESSAGE(sm <= sr + 1e-4, "largest step " << sm << " against the reference's " << sr);
  }
  // the Delay's move is a slow glide, not a jump: the tap's delay (read from a ramp) moves at most 0.25 frame a frame and ends on the new Delay
  const std::vector<float> x = ramp(60000);
  Fx fx;
  NEED(fx.ok);
  fx.set(1667, 0, 5, 100);
  std::vector<float> yl, yr;
  fx.run(std::vector<float>(x.begin(), x.begin() + 8192), std::vector<float>(x.begin(), x.begin() + 8192), yl, yr);
  fx.set(1667, 0, 25, 100);
  fx.run(std::vector<float>(x.begin() + 8192, x.end()), std::vector<float>(x.begin() + 8192, x.end()), yl, yr);
  double fastest = 0, prev = 0;
  for (std::size_t n = 4000; n < yl.size(); ++n) {
    const double d = static_cast<double>(n) - static_cast<double>(yl[n]) / kRampC;
    if (n > 4000) {
      fastest = std::max(fastest, std::fabs(d - prev));
    }
    prev = d;
  }
  CHECK_MESSAGE(fastest < 0.25, "the Delay moved " << fastest << " frames in one frame");
  CHECK(fastest > 0.05);  // (it does move)
  CHECK_MESSAGE(std::fabs(prev - 25.0 * 44.1) < 1.0, "the delay ended at " << prev << " frames");
}

TEST_CASE("chorus: any block size gives the output of the reference, 128 frames are two chunks of 64, and in == out is the same as two buffers") {
  const std::size_t N = 8192;
  const std::vector<float> xl = soft_noise(N, 61), xr = soft_noise(N, 67);
  Fx a;
  NEED(a.ok);
  a.set(1667, 40, 12, 50);
  std::vector<float> al, ar;
  a.run(xl, xr, al, ar, 64);
  for (std::uint32_t block : {1u, 7u, 32u, 100u, 128u}) {
    Fx b;
    NEED(b.ok);
    b.set(1667, 40, 12, 50);
    std::vector<float> bl, br;
    b.run(xl, xr, bl, br, block);
    Ref ref;
    ref.set(1667, 40, 12, 50);
    std::vector<double> rl, rr;
    ref.run(xl, xr, rl, rr, block);
    CAPTURE(block);
    CHECK(compare(bl, rl).max < 1e-4);  // each block size is its own reference (the LFO is sampled at the chunk's ends)
    CHECK(compare(br, rr).max < 1e-4);
    double d = 0;
    for (std::size_t i = 0; i < N; ++i) {
      d = std::max(d, static_cast<double>(std::fabs(bl[i] - al[i])));
    }
    CHECK_MESSAGE(d < 1e-3, "block " << block << " against block 64: " << d);  // (the LFO's chord over a chunk: under 0.001 frame at this setting)
    if (block == 128u) {
      CHECK(std::memcmp(al.data(), bl.data(), N * sizeof(float)) == 0);
    }
  }
  Fx c;
  NEED(c.ok);
  c.set(1667, 40, 12, 50);
  std::vector<float> cl = xl, cr = xr;  // in place
  for (std::size_t at = 0; at < N; at += 64) {
    c.busy = c.inst.m->render_st(c.inst.state, cl.data() + at, cr.data() + at, cl.data() + at, cr.data() + at, 64);
  }
  CHECK(std::memcmp(al.data(), cl.data(), N * sizeof(float)) == 0);
  CHECK(std::memcmp(ar.data(), cr.data(), N * sizeof(float)) == 0);
  // the mono render (the loader requires one; no host of this OS calls it for a WIDENS effect) is the left output on a mono input
  Fx m1, m2;
  NEED(m1.ok && m2.ok);
  m1.set(1667, 40, 12, 50);
  m2.set(1667, 40, 12, 50);
  std::vector<float> sl, sr;
  m1.run(xl, xl, sl, sr);
  std::vector<float> ml(N);
  for (std::size_t at = 0; at < N; at += 64) {
    m2.inst.m->render(m2.inst.state, xl.data() + at, ml.data() + at, 64);
  }
  CHECK(std::memcmp(sl.data(), ml.data(), N * sizeof(float)) == 0);
}

TEST_CASE("chorus: the tail contract: 1 while the input lives or the line holds it, then 0, with exact zeros, and a new burst wakes it") {
  Fx fx;
  NEED(fx.ok);
  fx.set(200, 100, 25, 100);  // the longest tap: 25 + 8 ms
  Ref ref;
  ref.set(200, 100, 25, 100);
  // a fresh effect with a silent input is idle
  const std::vector<float> z(64, 0.0f);
  std::vector<float> yl, yr;
  fx.run(z, z, yl, yr);
  CHECK(fx.busy == 0u);
  std::vector<double> rl, rr;
  ref.run(z, z, rl, rr);
  const std::vector<float> bl = soft_noise(2000, 21), br = soft_noise(2000, 23);
  fx.run(bl, br, yl, yr);
  ref.run(bl, br, rl, rr);
  CHECK(fx.busy == 1u);
  std::vector<std::uint32_t> busy;
  std::size_t first_zero = 0;
  for (std::size_t at = 0; at < 4000; at += 64) {
    const std::size_t before = yl.size();
    fx.run(std::vector<float>(64, 0.0f), std::vector<float>(64, 0.0f), yl, yr);
    ref.run(std::vector<float>(64, 0.0f), std::vector<float>(64, 0.0f), rl, rr);
    busy.push_back(fx.busy);
    if (fx.busy == 0u && first_zero == 0u) {
      first_zero = at;
    }
    if (at >= 1700) {  // 38.5 ms after the burst's end (the longest tap is 33 ms, the exact zeros by then): the whole block is exactly zero
      for (std::size_t i = before; i < yl.size(); ++i) {
        CHECK(yl[i] == 0.0f);
        CHECK(yr[i] == 0.0f);
      }
    }
  }
  for (std::size_t k = 0; k < 20; ++k) {
    CHECK(busy[k] == 1u);  // the first 1,280 frames (29 ms) after the input: still sounding
  }
  CHECK(first_zero >= 1400);  // not before the longest tap's 1,456 frames, less one chunk
  CHECK(first_zero <= 1764);  // and by 40 ms
  CHECK(busy.back() == 0u);
  MESSAGE("render returned 0 from " << first_zero << " frames (" << static_cast<double>(first_zero) / 44.1 << " ms) after the end of the input");
  const std::vector<float> cl = soft_noise(1000, 31), cr = soft_noise(1000, 33);
  fx.run(cl, cr, yl, yr);
  ref.run(cl, cr, rl, rr);
  CHECK(fx.busy == 1u);  // woken at once
  const Err el = compare(yl, rl), er = compare(yr, rr);
  CHECK(el.max < 1e-4);  // the whole life of the run, the zeroed quiet line included, is the reference's
  CHECK(er.max < 1e-4);
}

TEST_CASE("chorus: extremes and hostile values stay finite and bounded; a NaN or an infinity in the input passes to the output (the ABI's input is finite, so no guard)") {
  Fx fx;
  NEED(fx.ok);
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 1e30f, -1e30f, 0.0f};
  for (std::uint32_t id = 0; id < 4u; ++id) {
    for (float v : bad) {
      fx.inst.set(id, v);
    }
    fx.inst.set(id, fx.lib.entry->params[id].def);
  }
  fx.set(200, 100, 5, 100);  // the hardest: the shortest tap with the deepest swing, the fastest LFO
  std::vector<float> yl, yr;
  fx.run(noise(8192, 51, 0.9f), noise(8192, 53, 0.9f), yl, yr);
  for (std::size_t i = 0; i < yl.size(); ++i) {
    CHECK(std::isfinite(yl[i]));
    CHECK(std::isfinite(yr[i]));
    CHECK(std::fabs(yl[i]) <= 2.0f);  // the dry plus Mix times (tap - mid): |L| + |tap| + |M| <= 2.7 at full scale, 1.8 at 0.9
    CHECK(std::fabs(yr[i]) <= 2.0f);
  }
  // a NaN or an infinity in the input comes out non-finite where it went in (the dry). The OS never sends one: the stereo adapter (InsertHost::stereo_block) builds the L and R planes from Q15 int16,
  // so the input is finite and |x| <= 1, and the output side maps a NaN to 0 (to_q15, the module rule) or scrubs it; the module therefore keeps no guard for a non-finite input in its sample loop
  std::vector<float> xl = noise(256, 71), xr = noise(256, 73);
  xl[10] = std::numeric_limits<float>::quiet_NaN();
  xr[70] = std::numeric_limits<float>::infinity();
  yl.clear();
  yr.clear();
  fx.run(xl, xr, yl, yr);
  CHECK(std::isnan(yl[10]));
  CHECK(!std::isfinite(yr[70]));
}

TEST_CASE("chorus: the factory presets sound: finite, bounded, and not louder than a mixed-in copy can be") {
  Lib lib;
  NEED(lib.open(O3M_CHORUS_HOST_SO));
  const o3m_module& m = *lib.entry;
  NEED(m.preset_count == 4u);
  const std::vector<float> xl = soft_noise(6000, 81), xr = soft_noise(6000, 83);
  for (std::uint32_t i = 0; i < m.preset_count; ++i) {
    Fx fx;
    NEED(fx.ok);
    for (std::uint32_t p = 0; p < 4u; ++p) {
      fx.inst.set(p, m.presets[i].values[p]);
    }
    std::vector<float> yl, yr;
    fx.run(xl, xr, yl, yr);
    Ref ref;
    ref.set(m.presets[i].values[0], m.presets[i].values[1], m.presets[i].values[2], m.presets[i].values[3]);
    std::vector<double> rl, rr;
    ref.run(xl, xr, rl, rr);
    CAPTURE(m.presets[i].name);
    CHECK(compare(yl, rl).max < 1e-4);
    CHECK(compare(yr, rr).max < 1e-4);
    double peak_in = 0, peak = 0;
    for (std::size_t k = 0; k < yl.size(); ++k) {
      peak_in = std::max(peak_in, static_cast<double>(std::max(std::fabs(xl[k]), std::fabs(xr[k]))));
      peak = std::max(peak, static_cast<double>(std::max(std::fabs(yl[k]), std::fabs(yr[k]))));
    }
    CHECK(peak <= 2.0 * peak_in + 1e-6);  // (L + Mix (tap - M): at most the dry plus twice the input's peak)
  }
}

TEST_CASE("chorus: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_CHORUS_TMOD);
  if (!t.present) {
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_EFFECT);
  CHECK(t.h.abi_minor == 12u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == CHORUS_PARAM_COUNT);
  CHECK(t.h.view_count == CHORUS_VIEW_COUNT);
  CHECK(t.h.state_bytes <= CHORUS_STATE_BYTES);
  CHECK(t.h.hot_size <= 1536u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: the hot part at most 1.5 KiB (FX2 spec section 5)
  MESSAGE("chorus.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
