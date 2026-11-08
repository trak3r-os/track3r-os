// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X1 (new Community OS behaviour): the WIDE effect module driven through the ABI as the OS drives a WIDENS insert (render_st on L / R float planes, 64 frames a block), against a double-precision
// reference of the same law written here as a plain history array (no ring, no chunk copy: what is shared with the module is the law and its rounding of the delay to whole frames, not the code):
// L' = L + w h, R' = R - w h, h = HP(delay(M)), M = (L + R) / 2. What the tests prove: the module's float path matches the double maths sample by sample over a grid of settings (max error 1e-4 and SNR over
// 90 dB on the ADDED side), the MONO SUM of the output is the input's mid at every setting and on every input kind, Width 0 is the input bit for bit, a stereo input keeps its own side, the L / R
// correlation of a mono input falls monotonically with Width, the delay lands on the frame the Time says, a Time, Width or Low cut moved every block follows the reference (cross-fade and glide), block size
// independence and in == out, the tail contract, extremes and hostile values (NaN freedom), the mono render, no allocation, the presets, the packed file through the loader.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, the unit rows of the QA catalogue).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <malloc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "wide_gen.h"  // the generated descriptor header (ids, ranges, presets): build/gen

// A counting allocator for the "no allocation in the render path" test: only operator new is counted (the module has no libc: its packed file links with none), the heap's in-use bytes too.
namespace {
std::size_t g_new_calls = 0;
}
void* operator new(std::size_t n) {
  ++g_new_calls;
  void* p = std::malloc(n != 0 ? n : 1);
  if (p == nullptr) {
    std::abort();
  }
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr double kFs = kRate;
using Vec = std::vector<float>;

// ---- the double-precision reference ------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Ref {
  double fs;
  int d_max;
  std::vector<double> hist;  // M of every sample so far
  double lp = 0.0;
  int d = 0;
  double w = 0.0;  // in force, 0..1
  bool primed = false;

  explicit Ref(double rate = kFs) : fs(rate) {
    const float fpm = static_cast<float>(rate) * 0.001f;
    d_max = std::min(std::max(static_cast<int>(30.0f * fpm + 0.5f), 64), 2048 - 64);
  }
  // The delay in whole frames: the module's own rounding (a float product), so that a tie never decides a test; 64 frames at least (a chunk reads only what earlier chunks wrote).
  int frames_of(double ms) const {
    const float fpm = static_cast<float>(fs) * 0.001f;
    const int f = static_cast<int>(static_cast<float>(ms) * fpm + 0.5f);
    return std::min(std::max(f, 64), d_max);
  }
  double h_at(long t) const { return t < 0 ? 0.0 : hist[static_cast<std::size_t>(t)]; }
  // One chunk of n (1..64) frames at the settings (width %, ms, Hz): the added side a[j] = w h of each frame.
  void chunk(const float* l, const float* r, std::size_t n, double width_pct, double ms, double lowcut, double* a) {
    const double w_to = width_pct * 0.01;
    const int d_to = frames_of(ms);
    if (!primed) {
      w = w_to;
      d = d_to;
      primed = true;
    }
    const double c = 1.0 - std::exp(-2.0 * kPi * lowcut / fs);
    const long t0 = static_cast<long>(hist.size());
    for (std::size_t j = 0; j < n; ++j) {
      const double m = 0.5 * (static_cast<double>(l[j]) + static_cast<double>(r[j]));
      hist.push_back(m);
    }
    const double w_from = w;
    for (std::size_t j = 0; j < n; ++j) {
      const long t = t0 + static_cast<long>(j);
      double x = h_at(t - d);
      if (d_to != d) {
        const double f = static_cast<double>(j + 1) / static_cast<double>(n);
        x += f * (h_at(t - d_to) - x);
      }
      lp += c * (x - lp);
      const double wj = w_from + (w_to - w_from) * static_cast<double>(j + 1) / static_cast<double>(n);
      a[j] = wj * (x - lp);
    }
    d = d_to;
    w = w_to;
  }
};

struct Fx {
  Lib lib;
  Instance inst;
  bool ok = false;
  explicit Fx(float rate = kRate) { ok = lib.open(O3M_WIDE_HOST_SO) && inst.start(*lib.entry, rate); }
  void set(double width, double ms, double lowcut) {
    inst.set(WIDE_PARAM_WIDTH, static_cast<float>(width));
    inst.set(WIDE_PARAM_TIME, static_cast<float>(ms));
    inst.set(WIDE_PARAM_LOW_CUT, static_cast<float>(lowcut));
  }
  // The input through render_st in blocks of `block`; `busy` keeps the last return value, `busy_at` every block's.
  void run(const Vec& l, const Vec& r, Vec& ol, Vec& orr, std::uint32_t block = 64) {
    ol.assign(l.size(), 0.0f);
    orr.assign(l.size(), 0.0f);
    busy_at.clear();
    for (std::size_t at = 0; at < l.size();) {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(block, l.size() - at));
      busy = inst.m->render_st(inst.state, l.data() + at, r.data() + at, ol.data() + at, orr.data() + at, n);
      busy_at.push_back(busy);
      at += n;
    }
  }
  std::uint32_t busy = 0;
  std::vector<std::uint32_t> busy_at;
};

Vec noise(std::size_t n, std::uint32_t seed, float amp = 0.5f) {
  Vec x(n);
  std::uint32_t s = seed;
  for (auto& v : x) {
    s = s * 1664525u + 1013904223u;
    v = amp * (static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.0f);
  }
  return x;
}

Vec sine(std::size_t n, double amp, double f) {
  Vec x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs));
  }
  return x;
}

struct Match {
  double max_err = 0.0;    // max |module - reference| over both channels
  double snr_db = 0.0;     // of the ADDED side: (out_l - l) against the reference's w h
  double mono_err = 0.0;   // max |(L' + R') / 2 - M|
  double side_err = 0.0;   // max |(L' - R') / 2 - S_in - w h|
};

// Runs the module and the reference over (l, r) at fixed settings, in blocks of 64.
Match compare(const Vec& l, const Vec& r, double width, double ms, double lowcut, double rate = kFs) {
  Fx fx(static_cast<float>(rate));
  Match mt;
  if (!fx.ok) {
    mt.max_err = 1e9;
    return mt;
  }
  fx.set(width, ms, lowcut);
  Vec ol, orr;
  fx.run(l, r, ol, orr);
  Ref ref(rate);
  std::vector<double> a(64);
  double sig = 0.0, err = 0.0;
  for (std::size_t at = 0; at < l.size(); at += 64) {
    const std::size_t n = std::min<std::size_t>(64, l.size() - at);
    ref.chunk(l.data() + at, r.data() + at, n, static_cast<double>(static_cast<float>(width)), static_cast<double>(static_cast<float>(ms)), static_cast<double>(static_cast<float>(lowcut)), a.data());
    for (std::size_t j = 0; j < n; ++j) {
      const std::size_t i = at + j;
      const double el = static_cast<double>(l[i]) + a[j];
      const double er = static_cast<double>(r[i]) - a[j];
      mt.max_err = std::max(mt.max_err, std::max(std::fabs(ol[i] - el), std::fabs(orr[i] - er)));
      const double got_a = static_cast<double>(ol[i]) - static_cast<double>(l[i]);
      sig += a[j] * a[j];
      err += (got_a - a[j]) * (got_a - a[j]);
      const double m = 0.5 * (static_cast<double>(l[i]) + static_cast<double>(r[i]));
      mt.mono_err = std::max(mt.mono_err, std::fabs(0.5 * (static_cast<double>(ol[i]) + static_cast<double>(orr[i])) - m));
      const double s_in = 0.5 * (static_cast<double>(l[i]) - static_cast<double>(r[i]));
      mt.side_err = std::max(mt.side_err, std::fabs(0.5 * (static_cast<double>(ol[i]) - static_cast<double>(orr[i])) - s_in - a[j]));
    }
  }
  mt.snr_db = err <= 0.0 ? 300.0 : (sig <= 0.0 ? 300.0 : 10.0 * std::log10(sig / err));
  return mt;
}

bool bits_equal(const Vec& a, const Vec& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

bool all_finite(const Vec& v) {
  for (float x : v) {
    if (!std::isfinite(x)) {
      return false;
    }
  }
  return true;
}

double lcg01(std::uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return static_cast<double>(s >> 8) / 16777216.0;
}

}  // namespace

TEST_CASE("wide: the descriptor says a WIDENS effect with three parameters, four presets and no view") {
  Lib lib;
  NEED(lib.open(O3M_WIDE_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  CHECK(m.abi_minor == 12u);
  CHECK(m.struct_size == sizeof(o3m_module));
  CHECK(m.param_count == WIDE_PARAM_COUNT);
  CHECK(m.view_count == 0u);
  CHECK(m.render_st != nullptr);
  CHECK(m.render != nullptr);
  CHECK(m.fx_flags == O3M_FX_WIDENS);
  CHECK(m.state_bytes <= WIDE_STATE_BYTES);
  CHECK(m.preset_count == 4u);
  CHECK(std::strcmp(m.params[WIDE_PARAM_WIDTH].name, "Width") == 0);
  CHECK(std::strcmp(m.params[WIDE_PARAM_TIME].name, "Time") == 0);
  CHECK(std::strcmp(m.params[WIDE_PARAM_LOW_CUT].name, "Low cut") == 0);
  CHECK(m.params[WIDE_PARAM_WIDTH].def == 50.0f);
  CHECK(m.params[WIDE_PARAM_TIME].def == 12.0f);
  CHECK(m.params[WIDE_PARAM_LOW_CUT].def == 200.0f);
}

TEST_CASE("wide: every factory preset is in range, sets all three parameters, and the one named Wide is the defaults") {
  Lib lib;
  NEED(lib.open(O3M_WIDE_HOST_SO));
  const o3m_module& m = *lib.entry;
  NEED(m.presets != nullptr);
  for (std::uint32_t k = 0; k < m.preset_count; ++k) {
    const o3m_preset& p = m.presets[k];
    CHECK(p.count == WIDE_PARAM_COUNT);
    for (std::uint32_t i = 0; i < p.count; ++i) {
      CHECK(p.values[i] >= m.params[i].min);
      CHECK(p.values[i] <= m.params[i].max);
    }
    for (std::uint32_t j = k + 1; j < m.preset_count; ++j) {
      CHECK(std::strcmp(p.name, m.presets[j].name) != 0);
    }
  }
  CHECK(std::strcmp(m.presets[1].name, "Wide") == 0);
  for (std::uint32_t i = 0; i < WIDE_PARAM_COUNT; ++i) {
    CHECK(m.presets[1].values[i] == m.params[i].def);
  }
}

TEST_CASE("wide: against the double reference over a grid of settings on stereo noise: max error 1e-4, SNR of the added side over 90 dB, mono sum and side exact") {
  const Vec l = noise(4096, 11, 0.5f);
  const Vec r = noise(4096, 22, 0.5f);
  const double widths[] = {0, 25, 50, 85, 100};
  const double times[] = {2, 8, 12, 18, 26, 30, 7.3};  // (5, 15 and 25 ms land on a half frame at 44.1 kHz: a rounding tie, left out)
  const double lows[] = {20, 120, 200, 300, 1000};
  double worst_err = 0.0, worst_mono = 0.0, worst_side = 0.0, worst_snr = 300.0;
  int runs = 0;
  for (double w : widths) {
    for (double t : times) {
      for (double lc : lows) {
        const Match mt = compare(l, r, w, t, lc);
        worst_err = std::max(worst_err, mt.max_err);
        worst_mono = std::max(worst_mono, mt.mono_err);
        worst_side = std::max(worst_side, mt.side_err);
        if (w > 0.0) {
          worst_snr = std::min(worst_snr, mt.snr_db);
        }
        ++runs;
      }
    }
  }
  CHECK(runs == 175);
  CHECK(worst_err < 1e-4);
  CHECK(worst_snr > 90.0);
  CHECK(worst_mono < 1e-6);  // the mid is kept to the rounding of two float adds (1 LSB of 16 bits is 3e-5)
  CHECK(worst_side < 1e-4);
  MESSAGE("wide: 175 settings: max error " << worst_err << ", worst SNR " << worst_snr << " dB, mono-sum error " << worst_mono << ", side error " << worst_side);
}

TEST_CASE("wide: sine, impulse train, mono and one-sided inputs against the reference, and the mono sum is the input's mid") {
  const std::size_t n = 8192;
  Vec sl = sine(n, 0.7, 440.0), sr = sine(n, 0.4, 1234.5);
  Vec imp(n, 0.0f);
  for (std::size_t i = 100; i < n; i += 1777) {
    imp[i] = 1.0f;
  }
  const Vec nz = noise(n, 5, 0.9f);
  const Vec zero(n, 0.0f);
  struct Case {
    const Vec* l;
    const Vec* r;
    const char* what;
  };
  const Case cases[] = {{&sl, &sl, "mono sine"}, {&sl, &sr, "stereo sines"}, {&imp, &imp, "mono impulses"}, {&nz, &nz, "mono noise"}, {&nz, &zero, "left only"}, {&zero, &nz, "right only"}};
  const double sets[][3] = {{50, 12, 200}, {100, 2, 20}, {100, 30, 1000}, {85, 18, 150}, {60, 26, 120}, {25, 8, 300}, {10, 7.3, 500}, {75, 22, 60}};
  for (const Case& c : cases) {
    for (const auto& s : sets) {
      const Match mt = compare(*c.l, *c.r, s[0], s[1], s[2]);
      CHECK_MESSAGE(mt.max_err < 1e-4, c.what << " at " << s[0] << " " << s[1] << " " << s[2]);
      CHECK_MESSAGE(mt.mono_err < 1e-6, c.what << " at " << s[0] << " " << s[1] << " " << s[2]);
      CHECK_MESSAGE(mt.side_err < 1e-4, c.what);
      CHECK_MESSAGE(mt.snr_db > 90.0, c.what << " at " << s[0] << " " << s[1] << " " << s[2] << ": " << mt.snr_db);
    }
  }
}

TEST_CASE("wide: other sample rates (48 kHz, 22.05 kHz where 2 ms is under the 64-frame floor) against the reference") {
  const Vec l = noise(4096, 3, 0.5f);
  const Vec r = noise(4096, 4, 0.5f);
  const double rates[] = {48000.0, 22050.0, 32000.0};
  for (double rate : rates) {
    const double sets[][3] = {{50, 12, 200}, {100, 2, 20}, {100, 30, 300}};
    for (const auto& s : sets) {
      const Match mt = compare(l, r, s[0], s[1], s[2], rate);
      CHECK_MESSAGE(mt.max_err < 1e-4, rate << " Hz at " << s[0] << " " << s[1] << " " << s[2]);
      CHECK(mt.mono_err < 1e-6);
    }
  }
}

TEST_CASE("wide: Width 0 is the input bit for bit, also after a wet period, and for a stereo input the added side is exactly the oracle's") {
  const Vec l = noise(8192, 31, 0.8f);
  const Vec r = noise(8192, 32, 0.8f);
  for (double t : {2.0, 12.0, 30.0}) {
    for (double lc : {20.0, 200.0, 1000.0}) {
      Fx fx;
      NEED(fx.ok);
      fx.set(0, t, lc);
      Vec ol, orr;
      fx.run(l, r, ol, orr);
      CHECK(bits_equal(ol, l));
      CHECK(bits_equal(orr, r));
    }
  }
  // wet, then Width 0: the glide ends within the chunk the change lands in, then the output is the input again
  Fx fx;
  NEED(fx.ok);
  fx.set(100, 18, 150);
  Vec ol, orr;
  fx.run(l, r, ol, orr);
  CHECK_FALSE(bits_equal(ol, l));
  fx.set(0, 18, 150);
  const Vec l2 = noise(2048, 33, 0.8f);
  const Vec r2 = noise(2048, 34, 0.8f);
  fx.run(l2, r2, ol, orr);
  Vec tail_l(ol.begin() + 64, ol.end()), want_l(l2.begin() + 64, l2.end());
  Vec tail_r(orr.begin() + 64, orr.end()), want_r(r2.begin() + 64, r2.end());
  CHECK(bits_equal(tail_l, want_l));
  CHECK(bits_equal(tail_r, want_r));
}

TEST_CASE("wide: the L / R correlation of a mono input falls monotonically with Width") {
  const Vec x = noise(16384, 77, 0.5f);
  double prev = 2.0;
  for (int w = 0; w <= 100; w += 10) {
    Fx fx;
    NEED(fx.ok);
    fx.set(w, 12, 200);
    Vec ol, orr;
    fx.run(x, x, ol, orr);
    double sll = 0, srr = 0, slr = 0;
    for (std::size_t i = 2048; i < x.size(); ++i) {
      sll += static_cast<double>(ol[i]) * ol[i];
      srr += static_cast<double>(orr[i]) * orr[i];
      slr += static_cast<double>(ol[i]) * orr[i];
    }
    const double corr = slr / std::sqrt(sll * srr);
    if (w == 0) {
      CHECK(corr > 0.999999);
    }
    CHECK_MESSAGE(corr < prev, "Width " << w << ": " << corr << " after " << prev);
    prev = corr;
  }
  CHECK(prev < 0.5);  // Width 100 % is a clearly decorrelated pair
}

TEST_CASE("wide: an impulse comes back on the frame the Time says, and not before") {
  for (double t : {2.0, 8.0, 12.0, 18.0, 26.0, 30.0}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(100, t, 200);
    Vec x(4096, 0.0f);
    x[100] = 1.0f;
    Vec ol, orr;
    fx.run(x, x, ol, orr);
    const std::size_t d = static_cast<std::size_t>(static_cast<float>(t) * (kRate * 0.001f) + 0.5f);
    const double c = 1.0 - std::exp(-2.0 * kPi * 200.0 / kFs);
    for (std::size_t i = 0; i < 100 + d; ++i) {
      if (ol[i] != (i == 100 ? 1.0f : 0.0f) || orr[i] != (i == 100 ? 1.0f : 0.0f)) {
        CHECK_MESSAGE(false, "Time " << t << ": sample " << i << " is " << ol[i] << " / " << orr[i]);
        break;
      }
    }
    CHECK(std::fabs(static_cast<double>(ol[100 + d]) - (1.0 - c)) < 1e-6);   // the high-pass of a step: 1 - c
    CHECK(std::fabs(static_cast<double>(orr[100 + d]) + (1.0 - c)) < 1e-6);  // the other ear gets the opposite
  }
}

TEST_CASE("wide: any block size 1 to 128 and in == out give the same bits for static settings") {
  const Vec l = noise(3000, 41, 0.6f);
  const Vec r = noise(3000, 42, 0.6f);
  Fx base;
  NEED(base.ok);
  base.set(70, 14, 180);
  Vec bl, br;
  base.run(l, r, bl, br, 64);
  for (std::uint32_t block : {1u, 7u, 37u, 64u, 100u, 128u}) {
    Fx fx;
    NEED(fx.ok);
    fx.set(70, 14, 180);
    Vec ol, orr;
    fx.run(l, r, ol, orr, block);
    CHECK_MESSAGE(bits_equal(ol, bl), "block " << block);
    CHECK_MESSAGE(bits_equal(orr, br), "block " << block);
  }
  // in == out: both planes processed in place
  Fx fx;
  NEED(fx.ok);
  fx.set(70, 14, 180);
  Vec il = l, ir = r;
  for (std::size_t at = 0; at < il.size(); at += 64) {
    const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, il.size() - at));
    fx.inst.m->render_st(fx.inst.state, il.data() + at, ir.data() + at, il.data() + at, ir.data() + at, n);
  }
  CHECK(bits_equal(il, bl));
  CHECK(bits_equal(ir, br));
}

TEST_CASE("wide: set_param before the first block snaps; after it the Width glides over one chunk, the Time cross-fades and the Low cut follows, all against the reference") {
  const std::size_t blocks = 700;
  const Vec l = noise(blocks * 64, 51, 0.7f);
  const Vec r = noise(blocks * 64, 52, 0.4f);
  Fx fx;
  NEED(fx.ok);
  Ref ref;
  Vec ol(l.size()), orr(r.size());
  std::vector<double> a(64);
  std::uint32_t seed = 12345;
  double worst = 0.0, worst_mono = 0.0;
  for (std::size_t b = 0; b < blocks; ++b) {
    // a new target every block; the first set lands before the first block (it snaps)
    const double w = std::floor(lcg01(seed) * 101.0);
    const double t = 2.0 + std::floor(lcg01(seed) * 29.0) + 0.37;  // 2.37 .. 30.37 clamps at 30: a few land on the clamp, none on a half frame
    const double lc = 20.0 * std::pow(50.0, lcg01(seed));
    fx.set(w, t, lc);
    const double tc = std::min(static_cast<double>(static_cast<float>(t)), 30.0);
    ref.chunk(l.data() + b * 64, r.data() + b * 64, 64, static_cast<double>(static_cast<float>(w)), tc, static_cast<double>(static_cast<float>(lc)), a.data());
    fx.inst.m->render_st(fx.inst.state, l.data() + b * 64, r.data() + b * 64, ol.data() + b * 64, orr.data() + b * 64, 64);
    for (std::size_t j = 0; j < 64; ++j) {
      const std::size_t i = b * 64 + j;
      worst = std::max(worst, std::max(std::fabs(ol[i] - (static_cast<double>(l[i]) + a[j])), std::fabs(orr[i] - (static_cast<double>(r[i]) - a[j]))));
      worst_mono = std::max(worst_mono, std::fabs(0.5 * (static_cast<double>(ol[i]) + orr[i]) - 0.5 * (static_cast<double>(l[i]) + r[i])));
    }
  }
  CHECK(worst < 1e-4);
  CHECK(worst_mono < 1e-6);  // moving every block, the mono sum is still the mid
  MESSAGE("wide: parameters moved every block for " << blocks << " blocks: max error " << worst << ", mono-sum error " << worst_mono);
}

TEST_CASE("wide: a Time step cross-fades: the side follows the reference and makes no sample step beyond its") {
  // a 1 kHz sine, Width 100, Time 8 ms then 26 ms from the middle on
  const std::size_t n = 64 * 60;
  const Vec x = sine(n, 0.8, 1000.0);
  Fx fx;
  NEED(fx.ok);
  fx.set(100, 8, 120);
  Vec ol(n), orr(n);
  Ref ref;
  std::vector<double> a(64);
  double worst_step_mod = 0.0, worst_step_ref = 0.0, worst_diff = 0.0;
  double prev_mod = 0.0, prev_ref = 0.0;
  for (std::size_t b = 0; b < n / 64; ++b) {
    const double t = b < 30 ? 8.0 : 26.0;
    fx.inst.set(WIDE_PARAM_TIME, static_cast<float>(t));
    ref.chunk(x.data() + b * 64, x.data() + b * 64, 64, 100.0, t, 120.0, a.data());
    fx.inst.m->render_st(fx.inst.state, x.data() + b * 64, x.data() + b * 64, ol.data() + b * 64, orr.data() + b * 64, 64);
    for (std::size_t j = 0; j < 64; ++j) {
      const std::size_t i = b * 64 + j;
      const double side = 0.5 * (static_cast<double>(ol[i]) - orr[i]);
      if (i > 0) {
        worst_step_mod = std::max(worst_step_mod, std::fabs(side - prev_mod));
        worst_step_ref = std::max(worst_step_ref, std::fabs(a[j] - prev_ref));
      }
      worst_diff = std::max(worst_diff, std::fabs(side - a[j]));
      prev_mod = side;
      prev_ref = a[j];
    }
  }
  CHECK(worst_diff < 1e-4);
  CHECK(worst_step_mod <= worst_step_ref + 1e-4);
  CHECK(worst_step_mod < 0.4);  // (a 0.8 sine at 1 kHz moves 0.11 a sample: the cross-fade of two phases of it stays far under a jump of its whole swing)
}

TEST_CASE("wide: the tail contract: 1 while the input lives or the line and the filter ring, then 0 with exact zeros") {
  struct Set {
    double w, t, lc;
    std::size_t bound;  // the frame by which render_st must have returned 0: the line dry after 30 ms + a chunk (1,387 frames, two chunks of slack), or the high-pass ring-out to -140 dB after the echo, whichever is later
  };
  const Set sets[] = {{50, 12, 200, 1515}, {100, 30, 200, 2000}, {100, 2, 1000, 1515}, {100, 12, 20, 6000}};
  for (const Set& s : sets) {
    Fx fx;
    NEED(fx.ok);
    fx.set(s.w, s.t, s.lc);
    Vec x(16384, 0.0f);
    x[0] = 1.0f;
    x[1] = -0.7f;
    Vec ol, orr;
    fx.run(x, x, ol, orr);
    std::size_t first_zero = fx.busy_at.size();
    for (std::size_t b = 0; b < fx.busy_at.size(); ++b) {
      if (fx.busy_at[b] == 0u) {
        first_zero = b;
        break;
      }
    }
    CHECK_MESSAGE(first_zero < fx.busy_at.size(), "Width " << s.w << " Time " << s.t << " Low cut " << s.lc << ": never returned 0");
    CHECK_MESSAGE((first_zero + 1) * 64 <= s.bound, "Width " << s.w << " Time " << s.t << " Low cut " << s.lc << ": 0 only at frame " << (first_zero + 1) * 64);
    const std::size_t d = static_cast<std::size_t>(static_cast<float>(s.t) * (kRate * 0.001f) + 0.5f);
    for (std::size_t b = 0; b * 64 <= d + 64 && b < fx.busy_at.size(); ++b) {
      CHECK(fx.busy_at[b] == 1u);  // the echo has not come out yet
    }
    for (std::size_t b = first_zero; b < fx.busy_at.size(); ++b) {
      CHECK(fx.busy_at[b] == 0u);  // and it stays 0
    }
    bool zeros = true;
    for (std::size_t i = (first_zero + 1) * 64; i < x.size(); ++i) {  // (the block that returns 0 still plays the last 1e-8 of the ring-out)
      zeros = zeros && ol[i] == 0.0f && orr[i] == 0.0f;
    }
    CHECK(zeros);
  }
  // live input reads live, even a very quiet one; silence from the start reads idle
  Fx fx;
  NEED(fx.ok);
  Vec dc(2048, 1e-3f), ol, orr;
  fx.run(dc, dc, ol, orr);
  for (std::uint32_t b : fx.busy_at) {
    CHECK(b == 1u);
  }
  Fx idle;
  NEED(idle.ok);
  Vec z(512, 0.0f);
  idle.run(z, z, ol, orr);
  for (std::uint32_t b : idle.busy_at) {
    CHECK(b == 0u);
  }
}

TEST_CASE("wide: extremes on full-scale input stay finite, bounded by 3, and leave no subnormal") {
  const double sets[][3] = {{100, 2, 20}, {100, 30, 20}, {100, 2, 1000}, {100, 30, 1000}, {100, 12, 200}};
  for (const auto& s : sets) {
    Vec l(8192), r(8192);
    std::uint32_t seed = 9;
    for (std::size_t i = 0; i < l.size(); ++i) {
      l[i] = lcg01(seed) < 0.5 ? -1.0f : 1.0f;                // full-scale noise
      r[i] = (i / 100) % 2 == 0 ? 1.0f : -1.0f;               // and a full-scale square
    }
    Fx fx;
    NEED(fx.ok);
    fx.set(s[0], s[1], s[2]);
    Vec ol, orr;
    fx.run(l, r, ol, orr);
    CHECK(all_finite(ol));
    CHECK(all_finite(orr));
    float peak = 0.0f;
    bool sub = false;
    for (std::size_t i = 0; i < ol.size(); ++i) {
      peak = std::max(peak, std::max(std::fabs(ol[i]), std::fabs(orr[i])));
      sub = sub || std::fpclassify(ol[i]) == FP_SUBNORMAL || std::fpclassify(orr[i]) == FP_SUBNORMAL;
    }
    CHECK(peak <= 3.0001f);  // |L'| <= |L| + w |x - lp| <= 1 + 2 |M|
    CHECK_FALSE(sub);
    // and a ring that ends: silence after it gives exact zeros
    Vec z(16384, 0.0f);
    fx.run(z, z, ol, orr);
    CHECK(fx.busy == 0u);
    CHECK(ol.back() == 0.0f);
    CHECK(orr.back() == 0.0f);
  }
}

TEST_CASE("wide: hostile values: NaN and infinite parameters clamp, a NaN, infinite or huge input does not poison the effect, and a random walk of every parameter stays finite") {
  // parameters: a NaN falls to the lower bound, infinity and huge values to the bounds; the same bits as the bounds set directly
  {
    const Vec l = noise(2048, 61, 0.5f), r = noise(2048, 62, 0.5f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    struct P {
      float w, t, lc;
      double ew, et, elc;
    };
    const P ps[] = {{nan, nan, nan, 0, 2, 20}, {inf, inf, inf, 100, 30, 1000}, {-inf, -inf, -inf, 0, 2, 20}, {1e30f, -1e30f, 1e30f, 100, 2, 1000}, {-5.0f, 99.0f, 5.0f, 0, 30, 20}};
    for (const P& p : ps) {
      Fx hostile;
      Fx clean;
      NEED(hostile.ok && clean.ok);
      hostile.inst.set(WIDE_PARAM_WIDTH, p.w);
      hostile.inst.set(WIDE_PARAM_TIME, p.t);
      hostile.inst.set(WIDE_PARAM_LOW_CUT, p.lc);
      clean.set(p.ew, p.et, p.elc);
      Vec a, b, c, d;
      hostile.run(l, r, a, b);
      clean.run(l, r, c, d);
      CHECK(bits_equal(a, c));
      CHECK(bits_equal(b, d));
    }
  }
  // input: one bad sample in chunk 10 (a NaN, an infinity, a huge value); the chunk after it is clean, and from then on the output is that of an instance that got zeros in that chunk
  const float bad_values[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 3.0e30f};
  for (float bad : bad_values) {
    Vec l = noise(64 * 60, 71, 0.5f), r = noise(64 * 60, 72, 0.5f);
    Vec l0 = l, r0 = r;
    l[64 * 10 + 5] = bad;
    for (std::size_t i = 64 * 10; i < 64 * 11; ++i) {  // the clean instance: zeros in the whole chunk, which is what the module writes to its line
      l0[i] = 0.0f;
      r0[i] = 0.0f;
    }
    Fx a, b;
    NEED(a.ok && b.ok);
    a.set(100, 30, 100);
    b.set(100, 30, 100);
    Vec al, ar, bl, br;
    a.run(l, r, al, ar);
    b.run(l0, r0, bl, br);
    bool finite_after = true;
    for (std::size_t i = 64 * 11; i < al.size(); ++i) {
      finite_after = finite_after && std::isfinite(al[i]) && std::isfinite(ar[i]);
    }
    CHECK_MESSAGE(finite_after, "bad input " << bad);
    Vec a_tail(al.begin() + 64 * 11, al.end()), b_tail(bl.begin() + 64 * 11, bl.end());
    Vec a_tailr(ar.begin() + 64 * 11, ar.end()), b_tailr(br.begin() + 64 * 11, br.end());
    CHECK_MESSAGE(bits_equal(a_tail, b_tail), "bad input " << bad);
    CHECK_MESSAGE(bits_equal(a_tailr, b_tailr), "bad input " << bad);
  }
  // a random walk of every parameter over 2000 blocks of full-scale noise stays finite and under 3
  {
    Fx fx;
    NEED(fx.ok);
    std::uint32_t seed = 4242;
    Vec l(64), r(64), ol(64), orr(64);
    std::uint32_t ls = 1, rs = 2;
    bool fine = true;
    float peak = 0.0f;
    for (int b = 0; b < 2000; ++b) {
      for (std::size_t i = 0; i < 64; ++i) {
        ls = ls * 1664525u + 1013904223u;
        rs = rs * 1664525u + 1013904223u;
        l[i] = static_cast<float>(static_cast<std::int32_t>(ls)) / 2147483648.0f;
        r[i] = static_cast<float>(static_cast<std::int32_t>(rs)) / 2147483648.0f;
      }
      fx.set(lcg01(seed) * 100.0, 2.0 + lcg01(seed) * 28.0, 20.0 * std::pow(50.0, lcg01(seed)));
      fx.inst.m->render_st(fx.inst.state, l.data(), r.data(), ol.data(), orr.data(), 64);
      for (std::size_t i = 0; i < 64; ++i) {
        fine = fine && std::isfinite(ol[i]) && std::isfinite(orr[i]);
        peak = std::max(peak, std::max(std::fabs(ol[i]), std::fabs(orr[i])));
      }
    }
    CHECK(fine);
    CHECK(peak <= 3.0001f);
  }
}

TEST_CASE("wide: the render path allocates nothing") {
  Fx fx;
  NEED(fx.ok);
  fx.set(80, 20, 150);
  const Vec l = noise(64, 81, 0.5f), r = noise(64, 82, 0.5f);
  Vec ol(64), orr(64);
  fx.inst.m->render_st(fx.inst.state, l.data(), r.data(), ol.data(), orr.data(), 64);  // (the first block primes it)
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
  const std::size_t heap_before = mallinfo2().uordblks;
#endif
#endif
  const std::size_t before = g_new_calls;
  for (int i = 0; i < 5000; ++i) {
    fx.inst.set(WIDE_PARAM_TIME, 2.0f + static_cast<float>(i % 29));
    fx.inst.set(WIDE_PARAM_LOW_CUT, 20.0f + static_cast<float>(i % 900));
    fx.inst.m->render_st(fx.inst.state, l.data(), r.data(), ol.data(), orr.data(), 64);
    fx.inst.m->render_st(fx.inst.state, l.data(), r.data(), ol.data(), orr.data(), 17);
    fx.inst.m->render(fx.inst.state, l.data(), ol.data(), 64);
  }
  const std::size_t after = g_new_calls;
  CHECK(after == before);
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
  CHECK(mallinfo2().uordblks == heap_before);
#endif
#endif
}

TEST_CASE("wide: the mono render is the input (the mid of render_st), keeps the line coherent, and a re-init gives a clean line") {
  const Vec x = noise(4096, 91, 0.6f);
  Fx mono;
  Fx st;
  NEED(mono.ok && st.ok);
  mono.set(100, 20, 150);
  st.set(100, 20, 150);
  // the mono render over the first half: out == in bit for bit (and in place too)
  Vec out(2048);
  for (std::size_t at = 0; at < 2048; at += 64) {
    mono.inst.m->render(mono.inst.state, x.data() + at, out.data() + at, 64);
  }
  CHECK(bits_equal(out, Vec(x.begin(), x.begin() + 2048)));
  Vec inplace(x.begin(), x.begin() + 2048);
  Fx m2;
  NEED(m2.ok);
  for (std::size_t at = 0; at < 2048; at += 64) {
    m2.inst.m->render(m2.inst.state, inplace.data() + at, inplace.data() + at, 64);
  }
  CHECK(bits_equal(inplace, out));
  // then render_st over the second half equals an instance that ran render_st all along
  Vec ol, orr;
  st.run(x, x, ol, orr);
  Vec h2(x.begin() + 2048, x.end()), a(2048), b(2048);
  for (std::size_t at = 0; at < 2048; at += 64) {
    mono.inst.m->render_st(mono.inst.state, h2.data() + at, h2.data() + at, a.data() + at, b.data() + at, 64);
  }
  CHECK(bits_equal(a, Vec(ol.begin() + 2048, ol.end())));
  CHECK(bits_equal(b, Vec(orr.begin() + 2048, orr.end())));
  // re-init on the same block (what bypass off does): the old line is gone, silence in gives silence out
  CHECK(st.inst.m->init(st.inst.state, &st.inst.host) == 0);
  Vec z(4096, 0.0f);
  st.run(z, z, ol, orr);
  CHECK(st.busy == 0u);
  bool zeros = true;
  for (std::size_t i = 0; i < z.size(); ++i) {
    zeros = zeros && ol[i] == 0.0f && orr[i] == 0.0f;
  }
  CHECK(zeros);
}

TEST_CASE("wide: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_WIDE_TMOD);
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
  CHECK(t.h.param_count == WIDE_PARAM_COUNT);
  CHECK(t.h.view_count == 0u);
  CHECK(t.h.state_bytes <= WIDE_STATE_BYTES);
  CHECK(t.h.hot_size <= 1536u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: the spec's ceiling for an FX2 hot part is 1.5 KB
  MESSAGE("wide.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
