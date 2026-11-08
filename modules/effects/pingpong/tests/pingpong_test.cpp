// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X3 (new Community OS behaviour): the PING-PONG effect module driven through the ABI as the OS drives a WIDENS insert (render_st on two float planes, in place or not, with a scripted
// o3m_host.get_clock), against a double-precision reference of the same maths (the line of int16 words rounded toward zero, the two feedback one-poles, the cross-fade of 1,024 frames (done on the int16 words, the quotient toward zero), the chunks of
// 64 frames): the echoes of an impulse (first on the left at D, next on the right at 2D, then the left at 3D) to 1 LSB of the int16 line; the sample error of a noise run against the reference at
// several settings; the synced delay at 90, 120 and 174 BPM for every label (round(steps x fs x 15 / bpm)), halved when it is over 1 s, held to 64 frames at least; the free time; the cross-fade of a tempo or a
// time change (two changes in a row too) against the reference; the MONO SUM (L + R) / 2 (the input's mid plus Mix (Lr + Rr) / 2 whatever the Width), Width 0 centring the echoes and Width keeping the
// side of the input; Mix 0 as the dry pair bit for bit; the feedback at 95 % with silence in reaching exactly 0, the tail flag (1 while the line holds a sample, 0 once it is exactly zero), a sound after
// the silence; the clock's absence (no member, a stub's 1, a zero tempo): 120 BPM; hostile parameters; block-size independence, in place equal to not in place, the mono render; the loop gain bound of the
// two poles; the packed file through the loader.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-223).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "pingpong_gen.h"  // the generated descriptor header (ids and ranges): build/gen

namespace {

constexpr double kFs = kRate;
constexpr std::size_t kBlock = 64;
constexpr std::size_t kRingFrames = 44200;  // the module's line (the oracle's quiet rule counts the same frames)
constexpr double kLsb = 1.0 / 32768.0;
constexpr std::uint16_t kRates96[16] = {0, 768, 576, 384, 288, 192, 144, 96, 72, 48, 36, 32, 24, 18, 16, 12};

// ---- the scripted clock -----------------------------------------------------------------------------------------------------------------------
struct Clock {
  std::int32_t rc = 0;
  std::uint32_t tempo = 12000;
  int calls = 0;
};
Clock g_clock;

std::int32_t fake_clock(o3m_clock* out) {
  ++g_clock.calls;
  if (out->size >= 12) {
    out->flags = 1;
    out->tempo_x100 = g_clock.tempo;
  }
  return g_clock.rc;
}

float fake_level(std::uint32_t) { return 0.0f; }

struct Cfg {
  int sync = 4;  // the label "3": a dotted eighth
  double time = 375.0, fb = 45.0, low = 120.0, high = 6000.0, mix = 35.0, width = 100.0;
};

// One module instance, driven through render_st.
struct Rig {
  Lib lib;
  std::vector<unsigned char> mem;
  unsigned char* state = nullptr;
  o3m_host host{};
  const o3m_module* m = nullptr;
  bool ok = false;
  std::uint32_t busy = 0;

  explicit Rig(bool with_clock = true) {
    if (!lib.open(O3M_PINGPONG_HOST_SO)) {
      return;
    }
    m = lib.entry;
    mem.assign(m->state_bytes + 32u, 0);
    state = mem.data() + (16u - reinterpret_cast<std::uintptr_t>(mem.data()) % 16u) % 16u;
    host = make_host();
    if (with_clock) {
      host.get_clock = fake_clock;
      host.track_level = fake_level;
    } else {
      host.size = static_cast<std::uint32_t>(offsetof(o3m_host, get_clock));  // a 1.11 host table: no clock, no key
    }
    g_clock = Clock{};
    ok = m->init(state, &host) == 0;
    for (std::uint32_t i = 0; ok && i < m->param_count; ++i) {
      m->set_param(state, i, m->params[i].def);
    }
  }
  void set(std::uint32_t id, double v) { m->set_param(state, id, static_cast<float>(v)); }
  void apply(const Cfg& c) {
    set(PINGPONG_PARAM_SYNC, c.sync);
    set(PINGPONG_PARAM_TIME, c.time);
    set(PINGPONG_PARAM_FEEDBACK, c.fb);
    set(PINGPONG_PARAM_LOW_CUT, c.low);
    set(PINGPONG_PARAM_HIGH_CUT, c.high);
    set(PINGPONG_PARAM_MIX, c.mix);
    set(PINGPONG_PARAM_WIDTH, c.width);
  }
  // Renders in blocks of `block` frames; `before(at)` runs before each block (the test scripts the clock and the knobs there).
  void run(const std::vector<float>& il, const std::vector<float>& ir, std::vector<float>& ol, std::vector<float>& orr, std::size_t block = kBlock, const std::function<void(std::size_t)>& before = nullptr,
           bool in_place = false) {
    const std::size_t n = il.size();
    ol.assign(n, 0.0f);
    orr.assign(n, 0.0f);
    std::vector<float> al = il, ar = ir;  // the in-place planes
    for (std::size_t at = 0; at < n; at += block) {
      const std::size_t b = std::min(block, n - at);
      if (before) {
        before(at / block);
      }
      if (in_place) {
        busy = m->render_st(state, al.data() + at, ar.data() + at, al.data() + at, ar.data() + at, static_cast<std::uint32_t>(b));
      } else {
        busy = m->render_st(state, il.data() + at, ir.data() + at, ol.data() + at, orr.data() + at, static_cast<std::uint32_t>(b));
      }
    }
    if (in_place) {
      ol = al;
      orr = ar;
    }
  }
};

// ---- the reference ----------------------------------------------------------------------------------------------------------------------------
// The control step is the module's own (a delay in frames from the tempo, a fade of 1,024 frames, the chunks of 64, the quiet rule); the audio path is in double. The knobs are constant over a run (the
// module's glide is a one-pole over a few chunks: a run that moves them is compared by its settled part only), except Sync and Time, which act through the cross-fade.
std::uint32_t ref_delay(const Cfg& c, std::uint32_t tempo) {
  double d;
  if (c.sync == 0) {
    d = std::floor(c.time * kFs * 0.001 + 0.5);
  } else {
    d = static_cast<double>(kRates96[c.sync]) / 96.0 * kFs * 15.0 * 100.0 / static_cast<double>(tempo);
    while (d > std::floor(kFs + 0.5)) {
      d *= 0.5;
    }
    d = std::floor(d + 0.5);
  }
  return static_cast<std::uint32_t>(std::min(std::max(d, 64.0), std::floor(kFs + 0.5)));
}

struct Ref {
  std::vector<double> l, r;                // the wet outputs' parts: out = in + wet
  std::vector<double> wl, wr;              // the wet sums
  std::vector<std::uint32_t> delay;        // the delay in force at each frame
  std::vector<int> line_l, line_r;         // the written int16 values, by frame
};

Ref ref_run(const std::function<Cfg(std::size_t)>& cfg_at, const std::function<std::uint32_t(std::size_t)>& tempo_at, const std::vector<float>& il, const std::vector<float>& ir, std::size_t block = kBlock) {
  const std::size_t total = il.size();
  Ref o;
  o.l.assign(total, 0.0);
  o.r.assign(total, 0.0);
  o.wl.assign(total, 0.0);
  o.wr.assign(total, 0.0);
  o.delay.assign(total, 0);
  o.line_l.assign(total, 0);
  o.line_r.assign(total, 0);
  double lp0 = 0, hp0 = 0, lp1 = 0, hp1 = 0;
  std::uint32_t d_cur = 0, d_old = 0, fade_left = 0;
  std::size_t zero_run = kRingFrames;
  bool fresh = true;
  const auto at = [&](const std::vector<int>& v, std::size_t t, std::uint32_t d) { return t >= d ? v[t - d] : 0; };
  for (std::size_t b0 = 0; b0 < total; b0 += block) {
    const Cfg c = cfg_at(b0 / block);
    const std::uint32_t tempo = tempo_at(b0 / block);
    const std::size_t nb = std::min(block, total - b0);
    for (std::size_t c0 = 0; c0 < nb; c0 += kBlock) {
      const std::size_t n = std::min(kBlock, nb - c0);
      const std::size_t t0 = b0 + c0;
      if (zero_run >= kRingFrames) {
        bool nz = false;
        for (std::size_t i = 0; i < n; ++i) {
          nz = nz || il[t0 + i] != 0.0f || ir[t0 + i] != 0.0f;
        }
        if (!nz) {  // the module skips the chunk: the dry pair, the line stays zero
          for (std::size_t i = 0; i < n; ++i) {
            o.delay[t0 + i] = d_cur;
          }
          continue;
        }
        fresh = true;
      }
      const std::uint32_t d_new = ref_delay(c, tempo);
      if (fresh) {
        d_cur = d_new;
        d_old = 0;
        fade_left = 0;
        fresh = false;
      } else if (fade_left == 0 && d_new != d_cur) {
        d_old = d_cur;
        d_cur = d_new;
        fade_left = 1024;
      }
      const double g = c.fb * 0.01, mix = c.mix * 0.01, wd = c.width * 0.01;
      const double cl = 1.0 - std::exp(-2.0 * kPi * c.high / kFs), ch = 1.0 - std::exp(-2.0 * kPi * c.low / kFs);
      const double cs = mix * 0.5 * (1.0 - wd) * kLsb, cd = mix * wd * kLsb;
      std::uint32_t acc = 0;
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t t = t0 + i;
        int li = at(o.line_l, t, d_cur), ri = at(o.line_r, t, d_cur);
        if (fade_left != 0) {  // the cross-fade is done on the words: the old tap plus (new - old) x w / 1024, the quotient rounded toward zero, w = (done + i + 1) / 1024 at most 1
          const int wi = static_cast<int>(std::min<std::size_t>(1024, 1024 - fade_left + i + 1));
          const int lb = at(o.line_l, t, d_old), rb = at(o.line_r, t, d_old);
          li = lb + ((li - lb) * wi) / 1024;
          ri = rb + ((ri - rb) * wi) / 1024;
        }
        const double lr = li, rr = ri;
        lp0 += cl * (rr - lp0);
        hp0 += ch * (lp0 - hp0);
        lp1 += cl * (lr - lp1);
        hp1 += ch * (lp1 - hp1);
        const float x = (il[t] + ir[t]) * 16384.0f;
        double vl = static_cast<double>(x) + g * (lp0 - hp0), vr = g * (lp1 - hp1);
        vl = std::min(std::max(vl, -32768.0), 32767.0);
        vr = std::min(std::max(vr, -32768.0), 32767.0);
        const int wl = static_cast<int>(std::trunc(vl)), wr = static_cast<int>(std::trunc(vr));
        o.line_l[t] = wl;
        o.line_r[t] = wr;
        acc |= static_cast<std::uint32_t>(wl) | static_cast<std::uint32_t>(wr);
        o.wl[t] = cs * (lr + rr) + cd * lr;
        o.wr[t] = cs * (lr + rr) + cd * rr;
        o.l[t] = static_cast<double>(il[t]) + o.wl[t];
        o.r[t] = static_cast<double>(ir[t]) + o.wr[t];
        o.delay[t] = d_cur;
      }
      if (fade_left != 0) {
        fade_left = fade_left > n ? fade_left - static_cast<std::uint32_t>(n) : 0;
        d_old = fade_left != 0 ? d_old : 0;
      }
      if (acc == 0) {
        zero_run = std::min(zero_run + n, kRingFrames);
        if (zero_run >= kRingFrames) {
          lp0 = hp0 = lp1 = hp1 = 0.0;
        }
      } else {
        zero_run = 0;
      }
    }
  }
  return o;
}

// ---- signals ----------------------------------------------------------------------------------------------------------------------------------
std::vector<float> noise(std::size_t n, double amp, std::uint32_t seed) {
  std::vector<float> x(n);
  std::uint32_t s = seed;
  for (float& v : x) {
    s = s * 1664525u + 1013904223u;
    v = static_cast<float>(amp * (static_cast<double>(static_cast<std::int32_t>(s)) / 2147483648.0));
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

std::vector<float> impulse(std::size_t n, double amp) {
  std::vector<float> x(n, 0.0f);
  x[0] = static_cast<float>(amp);
  return x;
}

std::function<Cfg(std::size_t)> fixed(const Cfg& c) {
  return [c](std::size_t) { return c; };
}
std::function<std::uint32_t(std::size_t)> tempo_const(std::uint32_t t) {
  return [t](std::size_t) { return t; };
}

double max_err(const std::vector<float>& a, const std::vector<double>& b, std::size_t from = 0) {
  double e = 0;
  for (std::size_t i = from; i < a.size(); ++i) {
    e = std::max(e, std::fabs(static_cast<double>(a[i]) - b[i]));
  }
  return e;
}

double snr_db(const std::vector<float>& a, const std::vector<double>& b) {
  double sig = 0, err = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    sig += b[i] * b[i];
    const double d = static_cast<double>(a[i]) - b[i];
    err += d * d;
  }
  return err == 0.0 ? 300.0 : 10.0 * std::log10(sig / err);
}

// The first frame after 0 where the output differs from the dry input (the first echo).
std::size_t first_echo(const std::vector<float>& out, const std::vector<float>& dry) {
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i] != dry[i]) {
      return i;
    }
  }
  return 0;
}

}  // namespace

TEST_CASE("pingpong: the descriptor says a WIDENS effect with seven parameters, sixteen labels and five presets") {
  Lib lib;
  NEED(lib.open(O3M_PINGPONG_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  CHECK(m.abi_minor == 12u);
  CHECK(m.struct_size == O3M_MODULE_SIZE(12));
  CHECK(m.render_st != nullptr);
  CHECK(m.fx_flags == O3M_FX_WIDENS);
  CHECK(m.state_bytes <= 256u * 1024u);
  CHECK(m.state_bytes >= 4u * kRingFrames);
  NEED(m.param_count == 7u);
  CHECK(PINGPONG_PARAM_SYNC == 0);
  CHECK(PINGPONG_PARAM_TIME == 1);
  CHECK(PINGPONG_PARAM_FEEDBACK == 2);  // the first modulation target: the bench's moving-parameter row sweeps it
  CHECK(PINGPONG_PARAM_LOW_CUT == 3);
  CHECK(PINGPONG_PARAM_HIGH_CUT == 4);
  CHECK(PINGPONG_PARAM_MIX == 5);
  CHECK(PINGPONG_PARAM_WIDTH == 6);
  CHECK((m.params[PINGPONG_PARAM_SYNC].flags & O3M_PARAM_INT) != 0u);
  CHECK((m.params[PINGPONG_PARAM_TIME].flags & O3M_PARAM_LOG) != 0u);
  CHECK((m.params[PINGPONG_PARAM_TIME].flags & O3M_PARAM_MOD) == 0u);  // a time change is a cross-fade: not a modulation target
  CHECK((m.params[PINGPONG_PARAM_FEEDBACK].flags & O3M_PARAM_MOD) != 0u);
  CHECK(m.params[PINGPONG_PARAM_SYNC].min == 0.0f);
  CHECK(m.params[PINGPONG_PARAM_SYNC].max == 15.0f);
  CHECK(m.params[PINGPONG_PARAM_SYNC].def == 4.0f);  // the label "3": a dotted eighth
  CHECK(m.params[PINGPONG_PARAM_TIME].min == 10.0f);
  CHECK(m.params[PINGPONG_PARAM_TIME].max == 1000.0f);
  CHECK(m.params[PINGPONG_PARAM_TIME].def == 375.0f);
  CHECK(m.params[PINGPONG_PARAM_FEEDBACK].max == 95.0f);
  CHECK(m.params[PINGPONG_PARAM_LOW_CUT].min == 20.0f);
  CHECK(m.params[PINGPONG_PARAM_LOW_CUT].max == 2000.0f);
  CHECK(m.params[PINGPONG_PARAM_HIGH_CUT].min == 1000.0f);
  CHECK(m.params[PINGPONG_PARAM_HIGH_CUT].max == 20000.0f);
  NEED(m.labels != nullptr);
  NEED(m.labels[PINGPONG_PARAM_SYNC].names != nullptr);
  NEED(m.labels[PINGPONG_PARAM_SYNC].count == 16u);
  const char* const want[16] = {"FREE", "8", "6", "4", "3", "2", "3/2", "1", "3/4", "1/2", "3/8", "1/3", "1/4", "3/16", "1/6", "1/8"};
  for (unsigned i = 0; i < 16; ++i) {
    CHECK(std::strcmp(m.labels[PINGPONG_PARAM_SYNC].names[i], want[i]) == 0);
  }
  for (unsigned i = 1; i < 7; ++i) {
    CHECK(m.labels[i].names == nullptr);
    CHECK(m.labels[i].count == 0u);
  }
  // the presets: every value in range, Sync a whole label index, the names the spec gives
  NEED(m.presets != nullptr);
  NEED(m.preset_count == 5u);
  const char* const names[5] = {"1/8 PING", "DOTTED 8", "SLAP", "DUB", "WIDE 16TH"};
  const char* const sync_label[5] = {"2", "3", "FREE", "4", "1"};  // the note length each one is named for
  for (unsigned k = 0; k < 5; ++k) {
    const o3m_preset& p = m.presets[k];
    CHECK(std::strcmp(p.name, names[k]) == 0);
    NEED(p.count >= 6u);
    for (unsigned i = 0; i < p.count && i < m.param_count; ++i) {
      CHECK(p.values[i] >= m.params[i].min);
      CHECK(p.values[i] <= m.params[i].max);
    }
    CHECK(p.values[PINGPONG_PARAM_SYNC] == std::floor(p.values[PINGPONG_PARAM_SYNC]));
    CHECK(std::strcmp(m.labels[PINGPONG_PARAM_SYNC].names[static_cast<unsigned>(p.values[PINGPONG_PARAM_SYNC])], sync_label[k]) == 0);
  }
  CHECK(m.presets[2].values[PINGPONG_PARAM_TIME] == 90.0f);  // SLAP: FREE, 90 ms
}

TEST_CASE("pingpong: an impulse echoes on the left at D, on the right at 2D, on the left at 3D, as the reference to 1 LSB") {
  for (const bool wide_open : {true, false}) {
    Cfg c;
    c.sync = 0;
    c.time = 100.0;  // D = 4,410
    c.fb = 45.0;
    c.mix = 100.0;
    c.width = 100.0;
    if (wide_open) {
      c.low = 20.0;
      c.high = 20000.0;
    }
    const std::size_t n = 6 * 4410 + 2000;
    const std::vector<float> in = impulse(n, 0.5);
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    std::vector<float> ol, orr;
    rig.run(in, in, ol, orr);
    const Ref ref = ref_run(fixed(c), tempo_const(12000), in, in);
    std::vector<float> wl(n), wr(n);  // the wet parts: the output less the dry input
    for (std::size_t i = 0; i < n; ++i) {
      wl[i] = ol[i] - in[i];
      wr[i] = orr[i] - in[i];
    }
    const double el = max_err(wl, ref.wl), er = max_err(wr, ref.wr);
    MESSAGE("impulse, " << (wide_open ? "filters open" : "default cuts") << ": max error left " << el << " (" << el / kLsb << " LSB), right " << er << " (" << er / kLsb << " LSB)");
    CHECK(el <= 1.5 * kLsb);  // the echo is mix x line / 32768 with the line rounded toward zero: the module and the reference agree to the line's LSB
    CHECK(er <= 1.5 * kLsb);
    // where the echoes are: the first on the left at D (the right word there is exactly 0), the next on the right at 2D, then the left at 3D
    CHECK(ol[4410] != in[4410]);
    CHECK(orr[4410] == in[4410]);
    CHECK(std::fabs(static_cast<double>(ol[4410]) - 0.5 * 1.0) < 1e-4 + 2 * kLsb);  // the first echo is the impulse itself: x = 0.5, a = 1 (Width 100 %), Mix 100 %
    for (std::size_t i = 1; i < 4410; ++i) {
      CHECK(ol[i] == in[i]);
      CHECK(orr[i] == in[i]);
    }
    std::size_t first_r = 0, first_l_after = 0;
    for (std::size_t i = 1; i < n; ++i) {
      if (first_r == 0 && orr[i] != in[i]) {
        first_r = i;
      }
      if (i > 4410 && first_l_after == 0 && ol[i] != in[i]) {
        first_l_after = i;
      }
    }
    CHECK(first_r == 2 * 4410);
    CHECK(first_l_after == 3 * 4410);  // between D and 3D the left is silent: the echo was a single word and the next one is the loop's third pass
    // the left echo at 3D is smaller than the one at D, and the right one at 2D carries the loop gain
    double peak1 = 0, peak2 = 0, peak3 = 0;
    for (std::size_t i = 4400; i < 4420; ++i) {
      peak1 = std::max(peak1, std::fabs(static_cast<double>(ol[i]) - in[i]));
    }
    for (std::size_t i = 8810; i < 8830; ++i) {
      peak2 = std::max(peak2, std::fabs(static_cast<double>(orr[i]) - in[i]));
    }
    for (std::size_t i = 13220; i < 13240; ++i) {
      peak3 = std::max(peak3, std::fabs(static_cast<double>(ol[i]) - in[i]));
    }
    CHECK(peak2 < 0.45 * peak1 + 2 * kLsb);
    CHECK(peak3 < peak2 + 2 * kLsb);
    CHECK(rig.busy == 1u);  // the tail flag: the line still holds the echoes
  }
}

TEST_CASE("pingpong: the sample error against the reference over a grid of settings stays at the line's rounding") {
  const std::size_t n = 24000;
  const std::vector<float> il = noise(n, 0.5, 1), ir = noise(n, 0.4, 2);
  const Cfg grid[] = {
      {0, 120.0, 45.0, 120.0, 6000.0, 35.0, 100.0},  {0, 10.0, 95.0, 20.0, 20000.0, 100.0, 100.0}, {0, 300.0, 70.0, 2000.0, 1000.0, 100.0, 0.0}, {0, 700.0, 10.0, 300.0, 2500.0, 60.0, 50.0},
      {4, 0.0, 45.0, 120.0, 6000.0, 35.0, 100.0},    {12, 0.0, 80.0, 150.0, 9000.0, 80.0, 100.0},  {15, 0.0, 95.0, 20.0, 20000.0, 100.0, 30.0}, {1, 0.0, 30.0, 500.0, 4000.0, 50.0, 100.0},
  };
  double worst = 0, worst_snr = 1000;
  for (const Cfg& c : grid) {
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    std::vector<float> ol, orr;
    rig.run(il, ir, ol, orr);
    const Ref ref = ref_run(fixed(c), tempo_const(12000), il, ir);
    const double el = max_err(ol, ref.l), er = max_err(orr, ref.r);
    worst = std::max(worst, std::max(el, er));
    // the SNR of the wet part (the dry input is common to both)
    std::vector<float> wl(n);
    std::vector<double> rl(n);
    for (std::size_t i = 0; i < n; ++i) {
      wl[i] = ol[i] - il[i];
      rl[i] = ref.wl[i];
    }
    if (c.mix > 0) {
      worst_snr = std::min(worst_snr, snr_db(wl, rl));
    }
    CHECK(el < 1e-4);
    CHECK(er < 1e-4);
  }
  MESSAGE("grid of " << sizeof(grid) / sizeof(grid[0]) << " settings: worst sample error " << worst << ", worst SNR of the wet part " << worst_snr << " dB");
  CHECK(worst_snr > 60.0);
}

TEST_CASE("pingpong: Width 0 centres the echoes and Width keeps the side of the input") {
  const std::size_t n = 20000;
  const std::vector<float> il = noise(n, 0.5, 3), ir = noise(n, 0.3, 4);
  Cfg c;
  c.sync = 0;
  c.time = 80.0;
  c.mix = 70.0;
  c.fb = 60.0;
  c.width = 0.0;
  Rig rig;
  NEED(rig.ok);
  rig.apply(c);
  std::vector<float> ol, orr;
  rig.run(il, ir, ol, orr);
  double worst = 0;
  for (std::size_t i = 0; i < n; ++i) {
    // the echoes are centred: both ears get the same wet signal, so the side of the output is the side of the input
    worst = std::max(worst, std::fabs(static_cast<double>(ol[i] - orr[i]) - static_cast<double>(il[i] - ir[i])));
  }
  MESSAGE("Width 0: the largest change of the side " << worst);
  CHECK(worst < 2e-6);
  // a stereo input keeps its side at every Width when Mix is 0, and the echo's side is Mix Width (Lr - Rr): the left and right correlation of the wet part falls as Width rises
  double prev = 2.0;
  for (const double w : {0.0, 25.0, 50.0, 75.0, 100.0}) {
    Cfg d = c;
    d.width = w;
    Rig r2;
    NEED(r2.ok);
    r2.apply(d);
    std::vector<float> a, b;
    r2.run(il, ir, a, b);
    double sab = 0, saa = 0, sbb = 0;
    for (std::size_t i = 8000; i < n; ++i) {
      const double x = static_cast<double>(a[i]) - il[i], y = static_cast<double>(b[i]) - ir[i];
      sab += x * y;
      saa += x * x;
      sbb += y * y;
    }
    const double corr = sab / std::sqrt(saa * sbb);
    MESSAGE("Width " << w << ": wet L/R correlation " << corr);
    CHECK(corr < prev + 1e-9);
    prev = corr;
  }
  CHECK(prev < 0.2);  // Width 100: the echoes hard left and right (a ping-pong): almost uncorrelated
}

TEST_CASE("pingpong: the mono sum is the input's mid plus Mix (Lr + Rr) / 2 whatever the Width") {
  const std::size_t n = 30000;
  const std::vector<float> il = noise(n, 0.5, 5), ir = noise(n, 0.35, 6);
  Cfg c;
  c.sync = 5;  // "2": an eighth
  c.mix = 60.0;
  c.fb = 55.0;
  std::vector<std::vector<float>> mono;
  double worst_ref = 0;
  for (const double w : {0.0, 20.0, 50.0, 80.0, 100.0}) {
    c.width = w;
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    std::vector<float> ol, orr;
    rig.run(il, ir, ol, orr);
    std::vector<float> m(n);
    std::vector<double> mref(n);
    const Ref ref = ref_run(fixed(c), tempo_const(12000), il, ir);
    for (std::size_t i = 0; i < n; ++i) {
      m[i] = 0.5f * (ol[i] + orr[i]);
      mref[i] = 0.5 * (ref.l[i] + ref.r[i]);
    }
    worst_ref = std::max(worst_ref, max_err(m, mref));
    mono.push_back(m);
  }
  double spread = 0;
  for (std::size_t k = 1; k < mono.size(); ++k) {
    for (std::size_t i = 0; i < n; ++i) {
      spread = std::max(spread, std::fabs(static_cast<double>(mono[k][i]) - static_cast<double>(mono[0][i])));
    }
  }
  MESSAGE("mono sum: largest change over Width 0..100 % is " << spread << ", largest error against the reference " << worst_ref);
  CHECK(spread < 2e-6);  // the same sum at every Width (float rounding of the sum only)
  CHECK(worst_ref < 1e-4);
  // and with Mix 0 the mono sum is the input's mid exactly
  c.mix = 0.0;
  Rig rig;
  NEED(rig.ok);
  rig.apply(c);
  std::vector<float> ol, orr;
  rig.run(il, ir, ol, orr);
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(0.5f * (ol[i] + orr[i]) == 0.5f * (il[i] + ir[i]));
  }
}

TEST_CASE("pingpong: Mix 0 is the dry pair bit for bit, in render_st and in the mono render") {
  const std::size_t n = 12000;
  const std::vector<float> il = noise(n, 0.9, 7), ir = noise(n, 0.7, 8);
  for (const int sync : {0, 4, 15}) {
    Cfg c;
    c.sync = sync;
    c.mix = 0.0;
    c.fb = 90.0;
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    std::vector<float> ol, orr;
    rig.run(il, ir, ol, orr);
    CHECK(std::memcmp(ol.data(), il.data(), n * sizeof(float)) == 0);
    CHECK(std::memcmp(orr.data(), ir.data(), n * sizeof(float)) == 0);
    // the mono render: the same code with the right input equal to the left one, the left output
    Rig mono;
    NEED(mono.ok);
    mono.apply(c);
    std::vector<float> out(n);
    for (std::size_t at = 0; at < n; at += 100) {
      mono.busy = mono.m->render(mono.state, il.data() + at, out.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(100, n - at)));
    }
    CHECK(std::memcmp(out.data(), il.data(), n * sizeof(float)) == 0);
  }
}

TEST_CASE("pingpong: the synced delay is round(steps x fs x 15 / bpm) at 90, 120 and 174 BPM for every label, halved when over 1 s") {
  const char* const label[16] = {"FREE", "8", "6", "4", "3", "2", "3/2", "1", "3/4", "1/2", "3/8", "1/3", "1/4", "3/16", "1/6", "1/8"};
  int checked = 0, halved = 0;
  for (const std::uint32_t tempo : {9000u, 12000u, 17400u, 6000u}) {
    for (int sync = 1; sync <= 15; ++sync) {
      Cfg c;
      c.sync = sync;
      c.mix = 100.0;
      c.fb = 0.0;
      c.width = 100.0;
      Rig rig;
      NEED(rig.ok);
      rig.apply(c);
      g_clock.tempo = tempo;
      const std::uint32_t expect = ref_delay(c, tempo);
      const double raw = static_cast<double>(kRates96[sync]) / 96.0 * kFs * 15.0 * 100.0 / static_cast<double>(tempo);
      halved += raw > kFs + 0.5 ? 1 : 0;
      const std::size_t n = expect + 300;
      const std::vector<float> in = impulse(n, 0.5);
      std::vector<float> ol, orr;
      rig.run(in, in, ol, orr);
      const std::size_t got = first_echo(ol, in);
      CHECK_MESSAGE(got == expect, "label " << label[sync] << " at " << tempo / 100 << " BPM: delay " << got << ", expected " << expect);
      ++checked;
    }
  }
  CHECK(checked == 60);
  CHECK(halved > 0);
  // 8 steps at 60 BPM (2 s) is halved to 4 steps' length (1 s = 44,100 frames) and 8 steps at 120 BPM is exactly 1 s
  Cfg c8;
  c8.sync = 1;
  CHECK(ref_delay(c8, 6000u) == 44100u);
  CHECK(ref_delay(c8, 12000u) == 44100u);
  // the shortest synced delay is held to 64 frames (a tempo of 5,000 BPM: the label 1/8 would be 16)
  Cfg fast;
  fast.sync = 15;
  fast.mix = 100.0;
  fast.fb = 0.0;
  Rig rig;
  NEED(rig.ok);
  rig.apply(fast);
  g_clock.tempo = 500000u;
  const std::vector<float> in = impulse(400, 0.5);
  std::vector<float> ol, orr;
  rig.run(in, in, ol, orr);
  CHECK(first_echo(ol, in) == 64u);
}

TEST_CASE("pingpong: the free time is round(ms x fs / 1000) and the clock is not asked while FREE") {
  for (const double ms : {10.0, 45.5, 90.0, 375.0, 733.0, 1000.0}) {
    Cfg c;
    c.sync = 0;
    c.time = ms;
    c.mix = 100.0;
    c.fb = 0.0;
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    const std::uint32_t expect = static_cast<std::uint32_t>(std::floor(static_cast<double>(static_cast<float>(ms)) * kFs * 0.001 + 0.5));
    const std::size_t n = expect + 300;
    const std::vector<float> in = impulse(n, 0.5);
    std::vector<float> ol, orr;
    rig.run(in, in, ol, orr);
    CHECK_MESSAGE(first_echo(ol, in) == expect, "Time " << ms << " ms: delay " << first_echo(ol, in) << ", expected " << expect);
    CHECK(g_clock.calls == 0);
  }
}

TEST_CASE("pingpong: without a clock the tempo is 120 BPM (no member, a stub's answer 1, a zero tempo)") {
  Cfg c;
  c.sync = 4;  // "3": 3 steps
  c.mix = 100.0;
  c.fb = 0.0;
  const std::uint32_t expect = ref_delay(c, 12000u);  // 16,538
  const std::size_t n = expect + 300;
  const std::vector<float> in = impulse(n, 0.5);
  std::vector<float> ol, orr;
  {
    Rig rig(false);  // the host table has no get_clock (a 52-byte table)
    NEED(rig.ok);
    CHECK(!O3M_HOST_HAS(&rig.host, get_clock));
    rig.apply(c);
    rig.run(in, in, ol, orr);
    CHECK(first_echo(ol, in) == expect);
  }
  {
    Rig rig;  // the OS's stub before the clock: 1 (no sequencer), the tempo field 12000
    NEED(rig.ok);
    rig.apply(c);
    g_clock.rc = 1;
    rig.run(in, in, ol, orr);
    CHECK(first_echo(ol, in) == expect);
  }
  {
    Rig rig;  // an answer 1 with a tempo that is not 120: ignored (no clock means 120)
    NEED(rig.ok);
    rig.apply(c);
    g_clock.rc = 1;
    g_clock.tempo = 9000u;
    rig.run(in, in, ol, orr);
    CHECK(first_echo(ol, in) == expect);
  }
  {
    Rig rig;  // a tempo of 0 is no tempo
    NEED(rig.ok);
    rig.apply(c);
    g_clock.tempo = 0u;
    rig.run(in, in, ol, orr);
    CHECK(first_echo(ol, in) == expect);
    CHECK(g_clock.calls > 0);
  }
}

TEST_CASE("pingpong: a tempo or a time change cross-fades over 1,024 frames, as the reference, with no step over its") {
  const std::size_t n = 700 * kBlock;  // the line holds sound by the first change (frame 28,800), so both taps of the fade read it
  const std::vector<float> in = sine(n, 0.4, 440.0);
  const std::vector<float> in2 = noise(n, 0.3, 9);
  struct Case {
    const char* what;
    Cfg c;
    std::function<std::uint32_t(std::size_t)> tempo;
    std::function<Cfg(std::size_t)> cfg;
  };
  Cfg synced;
  synced.sync = 4;
  synced.mix = 100.0;
  synced.fb = 60.0;
  Cfg free_;
  free_.sync = 0;
  free_.time = 120.0;
  free_.mix = 100.0;
  free_.fb = 60.0;
  Cfg free2 = free_;
  free2.time = 333.0;
  Cfg free3 = free_;
  free3.time = 61.0;
  const Case cases[] = {
      {"tempo 120 to 90 at block 450", synced, [](std::size_t b) { return b < 450 ? 12000u : 9000u; }, nullptr},
      {"tempo 120 to 90 at block 450, to 174 at block 455 (inside the fade: waits)", synced, [](std::size_t b) { return b < 450 ? 12000u : (b < 455 ? 9000u : 17400u); }, nullptr},
      {"tempo 120 to 174 at block 450, to 60 at block 520", synced, [](std::size_t b) { return b < 450 ? 12000u : (b < 520 ? 17400u : 6000u); }, nullptr},
      {"Time 120 ms to 333 ms at block 450, to 61 ms at block 520", free_, tempo_const(12000), [=](std::size_t b) { return b < 450 ? free_ : (b < 520 ? free2 : free3); }},
  };
  for (const Case& cs : cases) {
    for (const std::vector<float>* src : {&in, &in2}) {
      Rig rig;
      NEED(rig.ok);
      rig.apply(cs.c);
      std::vector<float> ol, orr;
      const auto cfg = cs.cfg ? cs.cfg : fixed(cs.c);
      rig.run(*src, *src, ol, orr, kBlock, [&](std::size_t b) {
        g_clock.tempo = cs.tempo(b);
        const Cfg now = cfg(b);
        rig.set(PINGPONG_PARAM_TIME, now.time);
      });
      const Ref ref = ref_run(cfg, cs.tempo, *src, *src);
      const double el = max_err(ol, ref.l), er = max_err(orr, ref.r);
      double step_m = 0, step_r = 0;
      for (std::size_t i = 1; i < n; ++i) {
        step_m = std::max(step_m, std::fabs(static_cast<double>(ol[i]) - ol[i - 1]));
        step_r = std::max(step_r, std::fabs(ref.l[i] - ref.l[i - 1]));
      }
      MESSAGE(std::string(cs.what) << ": max error " << std::max(el, er) << ", largest sample step " << step_m << " (reference " << step_r << ")");
      CHECK_MESSAGE(el < 2e-4, std::string(cs.what));
      CHECK_MESSAGE(er < 2e-4, std::string(cs.what));
      CHECK_MESSAGE(step_m <= step_r + 1e-4, std::string(cs.what));
      // the delay did change (the reference's last delay is not its first), and the fade did something: the output is not the one of a run that never changes
      CHECK(ref.delay.front() != ref.delay.back());
      Rig still;
      NEED(still.ok);
      still.apply(cs.c);
      std::vector<float> sl, sr;
      still.run(*src, *src, sl, sr);
      double moved = 0;
      for (std::size_t i = 0; i < n; ++i) {
        moved = std::max(moved, std::fabs(static_cast<double>(ol[i]) - sl[i]));
      }
      CHECK(moved > 0.01);
    }
  }
}

TEST_CASE("pingpong: feedback at 95 % with silence in reaches exactly 0, the tail flag follows, and a sound after it starts as it is set") {
  Cfg c;
  c.sync = 0;
  c.time = 20.0;
  c.fb = 95.0;
  c.mix = 100.0;
  c.width = 100.0;
  c.low = 20.0;
  c.high = 20000.0;
  const std::size_t burst = 20000, silence = 44100 * 9;
  std::vector<float> il = noise(burst, 0.9, 10), ir = il;
  il.resize(burst + silence, 0.0f);
  ir.resize(burst + silence, 0.0f);
  Rig rig;
  NEED(rig.ok);
  rig.apply(c);
  std::vector<float> ol(il.size()), orr(il.size());
  std::vector<std::uint32_t> flag;
  double peak = 0;
  bool finite = true;
  std::size_t last_wet = 0;
  for (std::size_t at = 0; at < il.size(); at += kBlock) {
    const std::uint32_t b = static_cast<std::uint32_t>(std::min(kBlock, il.size() - at));
    const std::uint32_t r = rig.m->render_st(rig.state, il.data() + at, ir.data() + at, ol.data() + at, orr.data() + at, b);
    flag.push_back(r);
    for (std::size_t i = at; i < at + b; ++i) {
      finite = finite && std::isfinite(ol[i]) && std::isfinite(orr[i]);
      peak = std::max(peak, std::max(std::fabs(static_cast<double>(ol[i]) - il[i]), std::fabs(static_cast<double>(orr[i]) - ir[i])));
      if (ol[i] != il[i] || orr[i] != ir[i]) {
        last_wet = i;
      }
    }
  }
  MESSAGE("feedback 95 %: the last echo sample at " << last_wet << " (" << static_cast<double>(last_wet) / kFs << " s), peak wet " << peak);
  CHECK(finite);
  CHECK(peak <= 1.0 + 1e-6);  // the line is int16: the wet part is at most Mix x (|Lr| + |Rr|) / 32768 x 1
  CHECK(last_wet > burst);
  CHECK(last_wet < burst + silence - 2 * kRingFrames);  // it dies well before the end: no limit cycle
  // the tail flag: 1 in every block that has wet sound, and 0 at the end (the whole line is exactly 0)
  bool flag_ok = true;
  for (std::size_t b = 0; b * kBlock < il.size(); ++b) {
    bool wet = false;
    for (std::size_t i = b * kBlock; i < std::min(il.size(), (b + 1) * kBlock); ++i) {
      wet = wet || ol[i] != il[i] || orr[i] != ir[i];
    }
    flag_ok = flag_ok && (!wet || flag[b] == 1u);
  }
  CHECK(flag_ok);
  CHECK(flag.back() == 0u);
  // the last non-zero word was written one delay (882 frames) before the last echo; the flag stays 1 until the whole line (kRingFrames words) after it is zero: no old audio is left behind when Time grows
  const std::size_t w_last = last_wet - 882;
  CHECK(flag[(w_last + kRingFrames) / kBlock - 1] == 1u);
  CHECK(flag[(w_last + kRingFrames) / kBlock + 1] == 0u);
  // after the silence: Time grows (the quiet line takes it at once, no fade from old audio) and a click is echoed at the new delay, exactly
  rig.set(PINGPONG_PARAM_TIME, 500.0);
  const std::size_t d2 = 22050;
  const std::vector<float> in = impulse(d2 + 400, 0.5);
  std::vector<float> a, b;
  rig.run(in, in, a, b);
  CHECK(first_echo(a, in) == d2);
  CHECK(rig.busy == 1u);
}

TEST_CASE("pingpong: the tail flag is 1 for a whole block that has input, even when its last frame is 0") {
  Cfg c;
  c.sync = 0;
  c.time = 10.0;
  const std::size_t n = 4 * kBlock;
  std::vector<float> il(n, 0.0f), ir(n, 0.0f);
  // a sub-LSB input: the written word truncates to 0, so the line stays exactly silent (the flag would rest on the input alone), and every frame but the last of each block carries it
  for (std::size_t at = 0; at < n; at += kBlock) {
    for (std::size_t i = 0; i + 1 < kBlock; ++i) {
      il[at + i] = 1.0e-6f;
      ir[at + i] = 1.0e-6f;
    }
  }
  for (const bool in_place : {false, true}) {
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    std::vector<float> ol(n, 0.0f), orr(n, 0.0f);
    std::vector<float> al = il, ar = ir;
    std::uint32_t busy = 0;
    for (std::size_t at = 0; at < n; at += kBlock) {
      if (in_place) {
        busy = rig.m->render_st(rig.state, al.data() + at, ar.data() + at, al.data() + at, ar.data() + at, kBlock);
      } else {
        busy = rig.m->render_st(rig.state, il.data() + at, ir.data() + at, ol.data() + at, orr.data() + at, kBlock);
      }
    }
    CHECK_MESSAGE(busy == 1u, "in_place " << in_place);
  }
  // a silent block on the silent line still says idle
  Rig quiet;
  NEED(quiet.ok);
  quiet.apply(c);
  std::vector<float> z(kBlock, 0.0f), oz(kBlock, 0.0f);
  CHECK(quiet.m->render_st(quiet.state, z.data(), z.data(), oz.data(), oz.data(), kBlock) == 0u);
}

TEST_CASE("pingpong: hostile parameters clamp, and the output stays finite and the delay in range") {
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 1e30f, -1e30f, 0.0f};
  const std::size_t n = 6000;
  const std::vector<float> in = noise(n, 0.8, 11);
  for (const float v : bad) {
    for (std::uint32_t id = 0; id < 9; ++id) {  // two ids that do not exist
      Rig rig;
      NEED(rig.ok);
      rig.m->set_param(rig.state, id, v);
      std::vector<float> ol, orr;
      rig.run(in, in, ol, orr);
      bool finite = true;
      for (std::size_t i = 0; i < n; ++i) {
        finite = finite && std::isfinite(ol[i]) && std::isfinite(orr[i]);
      }
      CHECK_MESSAGE(finite, "param " << id << " = " << v);
    }
  }
}

TEST_CASE("pingpong: a huge input saturates the line word like the unit (a huge positive writes +32767, not the x86 INT_MIN), and its echo keeps the sign") {
  for (const float huge : {1.0e30f, -1.0e30f, 3.0e38f, -3.0e38f}) {
    Cfg c;
    c.sync = 0;
    c.time = 10.0;  // 441 frames at 44.1 kHz
    c.fb = 0.0;     // the echo is the input's word alone
    c.mix = 100.0;
    c.width = 0.0;  // the side sum puts the echo on both outputs
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    const std::size_t n = 4096;
    std::vector<float> il(n, 0.0f), ir(n, 0.0f);
    for (std::size_t i = 0; i < kBlock; ++i) {  // one block of huge input, then silence
      il[i] = huge;
      ir[i] = huge;
    }
    std::vector<float> ol, orr;
    rig.run(il, ir, ol, orr);
    bool finite = true;
    for (std::size_t i = 0; i < n; ++i) {
      finite = finite && std::isfinite(ol[i]) && std::isfinite(orr[i]);
    }
    CHECK_MESSAGE(finite, "huge " << huge);
    // the first echo at the delay: Mix 100 %, Width 0 puts Mix (Lr + Rr) / 2 / 32768 on each output, with Rr = 0 and Lr = +32767 for a huge positive (the unit's conversion saturates), -32768 for a huge negative
    const std::size_t d = ref_delay(c, 12000u);
    const double want = huge > 0.0f ? 0.5 * 32767.0 / 32768.0 : -0.5;
    const double wet_l = static_cast<double>(ol[d]);  // il[d] == 0: the wet part alone
    const double wet_r = static_cast<double>(orr[d]);
    MESSAGE("huge " << huge << ": the echo at frame " << d << " is " << wet_l << " / " << wet_r);
    CHECK_MESSAGE(std::fabs(wet_l - want) < 1e-6, "huge " << huge);
    CHECK_MESSAGE(std::fabs(wet_r - want) < 1e-6, "huge " << huge);
  }
}

TEST_CASE("pingpong: block-size independence, in place equal to not in place, and the mono render is the left output") {
  const std::size_t n = 30000;
  const std::vector<float> il = noise(n, 0.5, 12), ir = noise(n, 0.4, 13);
  Cfg c;
  c.sync = 5;
  c.mix = 80.0;
  c.fb = 70.0;
  c.width = 65.0;
  Rig base;
  NEED(base.ok);
  base.apply(c);
  std::vector<float> bl, br;
  base.run(il, ir, bl, br);
  for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{100}, std::size_t{128}, std::size_t{4096}}) {
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    std::vector<float> ol, orr;
    rig.run(il, ir, ol, orr, block, nullptr, false);
    const bool same = std::memcmp(ol.data(), bl.data(), n * sizeof(float)) == 0 && std::memcmp(orr.data(), br.data(), n * sizeof(float)) == 0;
    CHECK_MESSAGE(same, "block " << block);
  }
  {
    Rig rig;
    NEED(rig.ok);
    rig.apply(c);
    std::vector<float> ol, orr;
    rig.run(il, ir, ol, orr, kBlock, nullptr, true);
    CHECK(std::memcmp(ol.data(), bl.data(), n * sizeof(float)) == 0);
    CHECK(std::memcmp(orr.data(), br.data(), n * sizeof(float)) == 0);
  }
  {
    // the mono render: render_st(in, in) and the left output
    Rig st, mono;
    NEED(st.ok);
    NEED(mono.ok);
    st.apply(c);
    mono.apply(c);
    std::vector<float> sl, sr;
    st.run(il, il, sl, sr);
    std::vector<float> out(n);
    for (std::size_t at = 0; at < n; at += 128) {
      mono.m->render(mono.state, il.data() + at, out.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(128, n - at)));
    }
    CHECK(std::memcmp(out.data(), sl.data(), n * sizeof(float)) == 0);
  }
}

TEST_CASE("pingpong: at the worst setting the loop is bounded and the filters never gain") {
  Cfg c;
  c.sync = 0;
  c.time = 10.0;
  c.fb = 95.0;
  c.mix = 100.0;
  c.low = 20.0;
  c.high = 20000.0;
  std::vector<float> sq(44100 * 6);
  for (std::size_t i = 0; i < sq.size(); ++i) {
    sq[i] = ((i / 100) & 1u) != 0u ? 0.99f : -0.99f;
  }
  Rig rig;
  NEED(rig.ok);
  rig.apply(c);
  std::vector<float> ol, orr;
  rig.run(sq, sq, ol, orr);
  double peak = 0;
  bool finite = true;
  for (std::size_t i = 0; i < sq.size(); ++i) {
    finite = finite && std::isfinite(ol[i]) && std::isfinite(orr[i]);
    peak = std::max(peak, std::max(std::fabs(static_cast<double>(ol[i]) - sq[i]), std::fabs(static_cast<double>(orr[i]) - sq[i])));
  }
  MESSAGE("a full-scale square at 95 % feedback, 10 ms: the wet peak is " << peak);
  CHECK(finite);
  CHECK(peak <= 1.0 + 1e-6);
  // the two poles' gain: the low-pass |c / (1 - (1 - c) z^-1)| and the high-pass 1 - the low-pass, at every frequency of a grid and at the extreme cuts
  double worst = 0;
  for (const double fc_lp : {1000.0, 6000.0, 20000.0}) {
    for (const double fc_hp : {20.0, 120.0, 2000.0}) {
      const double cl = 1.0 - std::exp(-2.0 * kPi * fc_lp / kFs), ch = 1.0 - std::exp(-2.0 * kPi * fc_hp / kFs);
      for (int k = 0; k <= 2000; ++k) {
        const double w = kPi * k / 2000.0;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const std::complex<double> lp = cl / (1.0 - (1.0 - cl) * z1);
        const std::complex<double> hp = 1.0 - ch / (1.0 - (1.0 - ch) * z1);
        worst = std::max(worst, std::abs(lp * hp));
      }
    }
  }
  MESSAGE("the largest gain of the feedback filter over the grid: " << worst);
  CHECK(worst <= 1.0 + 1e-12);
}

TEST_CASE("pingpong: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_PINGPONG_TMOD);
  if (!t.present) {
    MESSAGE("pingpong.tmod is not present in this build: the packed-file checks are skipped");
    return;
  }
  CHECK(t.header == Status::Ok);
  CHECK(t.crc_ok);
  CHECK(t.relocation == Status::Ok);
  CHECK(t.descriptor == Status::Ok);
  CHECK(t.h.kind == O3M_KIND_EFFECT);
  CHECK(t.h.abi_minor == 12u);
  CHECK(t.h.module_version == O3M_VERSION(1, 0, 0));
  CHECK(t.h.param_count == PINGPONG_PARAM_COUNT);
  CHECK(t.h.view_count == 0u);
  CHECK(t.h.state_bytes <= PINGPONG_STATE_BYTES);
  CHECK(t.h.state_bytes <= kMaxStateBytes);
  CHECK(t.h.hot_size <= 1536u);  // the spec's ceiling for an FX2 module's hot part (the shared SRAM_L slab has about 6 KiB free)
  CHECK(t.file_bytes <= 4096u);
  MESSAGE("pingpong.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
