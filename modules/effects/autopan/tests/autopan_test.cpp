// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-FX2 X4 and X5 (new Community OS behaviour): the AUTOPAN effect module driven through the ABI as the OS drives a WIDENS insert (render_st on two float planes, a scripted get_clock), against a
// double-precision reference of the same maths written independently in the test (the LFO by floor and the one-pole by exp, the ramp, the pan law and the HEADPHONE cues in double): the sample-by-sample error
// over a grid of settings (sync, shape, depth, phase, mode, a clock that plays, one that is stopped and one that jumps), the MONO SUM (L + R) / 2 equal to the input's mid at every SPEAKERS setting,
// a stereo input keeping its side, Depth 0 as the input bit for bit, the free rate to 0.1 %, the zero crossings at the start and the middle of a beat at 120 BPM (and shifted by Phase), a WRAPPED clock
// re-aligning the phase in that block, the SQUARE's step under the 5 ms smoothing's bound, SPEAKERS bit for bit after a HEADPHONE period, the HEADPHONE far ear 26.5 frames later and darker by the
// reference's figures, L = R at the centre, no click on a sweep of p, the tail contract of both modes, hostile values and a hostile input, no allocation in render, the presets, and the packed file through
// the loader. Block-size independence of the whole fold: the same stream at block 7, 32, 100 and 200 with a synced, playing clock (the clock phase taken only where a chunk starts) and with HEADPHONE
// switched on mid-chunk (the prime snapping the cues to the chunk's own targets), and the tail flag OR-ing over a block's chunks.
// Host only: a host test does not prove the cost or the sound on the Tracker (diag bench insert, QA-GLB-226; by ear, QA-GLB-224 and 227).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../../tests/o3m_test_support.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <vector>

using namespace o3m_test;
using namespace o3os::module;

#include "autopan_gen.h"  // the generated descriptor header (ids and ranges): build/gen

// A counting allocator: the module has no libc, but the host build of the same source must not allocate either (QA: no allocation in render).
namespace {
std::size_t g_news = 0;
}
void* operator new(std::size_t n) {
  ++g_news;
  void* p = std::malloc(n != 0u ? n : 1u);
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
constexpr double kCpb[16] = {0.0, 4.0 / 64.0, 4.0 / 32.0, 4.0 / 16.0, 4.0 / 12.0, 4.0 / 8.0, 4.0 / 6.0, 4.0 / 4.0, 4.0 / 3.0, 4.0 / 2.0, 4.0 / 1.5, 4.0 / 1.0, 4.0 / 0.75, 4.0 / 0.5, 4.0 / (1.0 / 3.0), 4.0 / 0.25};

// ---- the scripted clock: what get_clock answers (the test sets it before each block) ----
struct ClockScript {
  std::uint32_t flags = 0;
  std::uint32_t tempo = 12000;
  float beat = 0.0f;
  int rc = 1;  // 1: the OS has no sequencer to ask (the stub of A1)
};
ClockScript g_clk;
std::int32_t fake_get_clock(o3m_clock* out) {
  out->size = sizeof(o3m_clock);
  out->flags = g_clk.flags;
  out->tempo_x100 = g_clk.tempo;
  out->beat = g_clk.beat;
  return g_clk.rc;
}
void clock_stub() { g_clk = ClockScript{}; }
// A sequencer that plays from beat 0 at `bpm`: the beat at sample `at`.
void clock_playing(std::size_t at, double bpm, std::size_t restart_at = 0, bool wrapped = false) {
  g_clk.flags = O3M_CLOCK_PLAYING | (wrapped ? O3M_CLOCK_WRAPPED : 0u);
  g_clk.tempo = static_cast<std::uint32_t>(bpm * 100.0 + 0.5);
  g_clk.beat = static_cast<float>(static_cast<double>(at - restart_at) / kFs * bpm / 60.0);
  g_clk.rc = 0;
}
void clock_stopped(double bpm) {
  g_clk.flags = 0;
  g_clk.tempo = static_cast<std::uint32_t>(bpm * 100.0 + 0.5);
  g_clk.beat = 0.0f;
  g_clk.rc = 0;
}

// ---- the reference ----
double frac_d(double x) { return x - std::floor(x); }
double ref_lfo(double ph, int shape) {  // the module's LFO, in double
  const double x = ph < 0.5 ? ph : ph - 1.0;
  if (shape == 2) {
    return x >= 0.0 ? 1.0 : -1.0;
  }
  if (shape == 1) {
    double y = 4.0 * x;
    if (y > 1.0) {
      y = 2.0 - y;
    }
    if (y < -1.0) {
      y = -2.0 - y;
    }
    return y;
  }
  const double y = 8.0 * x * (1.0 - 2.0 * std::fabs(x));
  return y + 0.225 * (y * std::fabs(y) - y);
}

struct Set {
  int sync = 3;
  double period = 1000.0;
  double depth = 70.0;
  int shape = 0;
  int phase = 0;
  int mode = 0;
};

struct Ref {
  Set st;
  double ph = 0.0, p_last = 0.0;
  bool primed = false;
  // HEADPHONE
  bool hp_ready = false;
  double rl[32] = {}, rr[32] = {};
  unsigned pos = 0;
  double dl = 0, dr = 0, cl = 0, cr = 0, lpl = 0, lpr = 0;
  double fc_near() const { return std::min(20000.0, 0.45 * kFs); }
  double c_near() const { return 1.0 - std::exp(-2.0 * kPi * fc_near() / kFs); }
  double itd_max() const { return std::min(0.6e-3 * kFs, 30.0); }

  // One block of n frames. `locked`: the sequencer plays and get_clock answered (phase from the beat); `ph_clock` the phase it gives (without Phase); `bpm` the tempo in force.
  void block(const float* l, const float* r, double* ol, double* orr, std::size_t n, bool locked, double ph_clock, double bpm) {
    const double inc = st.sync == 0 ? 1000.0 / (st.period * kFs) : bpm / 60.0 * kCpb[st.sync] / kFs;
    const double depth = st.depth * 0.01;
    const double off = st.phase / 360.0;
    const double ph0 = locked ? ph_clock : ph;
    const double ph1 = ph0 + static_cast<double>(n) * inc;
    const double raw = depth * ref_lfo(frac_d(ph1 + off), st.shape);
    ph = frac_d(ph1);
    if (!primed) {  // the first block holds the pan
      p_last = raw;
      primed = true;
    }
    const double p_from = p_last;
    const double p1 = st.shape == 2 ? p_from + (1.0 - std::exp(-static_cast<double>(n) / (0.005 * kFs))) * (raw - p_from) : raw;
    p_last = p1;
    const double dp = (p1 - p_from) / static_cast<double>(n);
    std::vector<double> pv(n);
    {
      double p = p_from;
      for (std::size_t j = 0; j < n; ++j) {
        p += dp;
        pv[j] = p;
      }
    }
    if (st.mode == 0) {
      hp_ready = false;
      for (std::size_t j = 0; j < n; ++j) {
        const double a = pv[j] * 0.5 * (static_cast<double>(l[j]) + r[j]);
        ol[j] = l[j] + a;
        orr[j] = r[j] - a;
      }
      return;
    }
    const double ap = std::fabs(p1);
    const double dfar = ap * itd_max();
    const double fc = fc_near() * std::pow(1800.0 / fc_near(), ap);
    const double cfar = 1.0 - std::exp(-2.0 * kPi * fc / kFs);
    const double dl_to = p1 < 0.0 ? dfar : 0.0, dr_to = p1 > 0.0 ? dfar : 0.0;
    const double cl_to = p1 < 0.0 ? cfar : c_near(), cr_to = p1 > 0.0 ? cfar : c_near();
    if (!hp_ready) {
      const double a = pv[0] * 0.5 * (static_cast<double>(l[0]) + r[0]);
      for (unsigned i = 0; i < 32; ++i) {
        rl[i] = l[0] + a;
        rr[i] = r[0] - a;
      }
      lpl = rl[0];
      lpr = rr[0];
      dl = dl_to;
      dr = dr_to;
      cl = cl_to;
      cr = cr_to;
      hp_ready = true;
    }
    const double inv = 1.0 / static_cast<double>(n);
    const double ddl = (dl_to - dl) * inv, ddr = (dr_to - dr) * inv, dcl = (cl_to - cl) * inv, dcr = (cr_to - cr) * inv;
    for (std::size_t j = 0; j < n; ++j) {
      const double a = pv[j] * 0.5 * (static_cast<double>(l[j]) + r[j]);
      pos = (pos + 1u) & 31u;
      rl[pos] = l[j] + a;
      rr[pos] = r[j] - a;
      dl += ddl;
      dr += ddr;
      cl += dcl;
      cr += dcr;
      const double kl = std::floor(std::max(dl, 0.0)), kr = std::floor(std::max(dr, 0.0));
      const double fl = std::max(dl, 0.0) - kl, fr = std::max(dr, 0.0) - kr;
      const unsigned il = static_cast<unsigned>(kl), ir = static_cast<unsigned>(kr);
      const double xl = rl[(pos - il) & 31u], yl = rl[(pos - il - 1u) & 31u];
      const double xr = rr[(pos - ir) & 31u], yr = rr[(pos - ir - 1u) & 31u];
      lpl += cl * ((xl + fl * (yl - xl)) - lpl);
      lpr += cr * ((xr + fr * (yr - xr)) - lpr);
      ol[j] = lpl;
      orr[j] = lpr;
    }
    dl = dl_to;
    dr = dr_to;
    cl = cl_to;
    cr = cr_to;
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
std::vector<float> sine(std::size_t n, double amp, double f) {
  std::vector<float> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs));
  }
  return x;
}
std::vector<float> dc(std::size_t n, float v) { return std::vector<float>(n, v); }

struct Out {
  std::vector<float> l, r;
};

struct Rig {
  Lib lib;
  Instance inst;
  bool ok = false;
  std::uint32_t busy = 0;
  Rig() {
    ok = lib.open(O3M_AUTOPAN_HOST_SO) && inst.start(*lib.entry);
    inst.host.get_clock = &fake_get_clock;  // the module reads the pointer of the host table given at init
  }
  void set(const Set& s) {
    inst.set(AUTOPAN_PARAM_SYNC, static_cast<float>(s.sync));
    inst.set(AUTOPAN_PARAM_PERIOD, static_cast<float>(s.period));
    inst.set(AUTOPAN_PARAM_DEPTH, static_cast<float>(s.depth));
    inst.set(AUTOPAN_PARAM_SHAPE, static_cast<float>(s.shape));
    inst.set(AUTOPAN_PARAM_PHASE, static_cast<float>(s.phase));
    inst.set(AUTOPAN_PARAM_HEADPHONE, static_cast<float>(s.mode));
  }
  // The input through render_st in blocks of `block`. `script(at)` sets the clock before the block that starts at sample `at`; `ref` (optional) is run on the same blocks with the same clock and
  // its output goes to `ref_out`.
  Out run(const std::vector<float>& l, const std::vector<float>& r, std::uint32_t block = 64, const std::function<void(std::size_t)>& script = nullptr, Ref* ref = nullptr,
          std::vector<double>* ref_l = nullptr, std::vector<double>* ref_r = nullptr) {
    Out o;
    o.l.assign(l.size(), 0.0f);
    o.r.assign(l.size(), 0.0f);
    if (ref_l != nullptr) {
      ref_l->assign(l.size(), 0.0);
      ref_r->assign(l.size(), 0.0);
    }
    for (std::size_t at = 0; at < l.size();) {
      const std::size_t n = std::min<std::size_t>(block, l.size() - at);
      if (script) {
        script(at);
      } else {
        clock_stub();
      }
      if (ref != nullptr) {
        const bool locked = ref->st.sync != 0 && g_clk.rc == 0 && (g_clk.flags & O3M_CLOCK_PLAYING) != 0u;
        const double bpm = g_clk.tempo >= 2000u && g_clk.tempo <= 100000u ? g_clk.tempo / 100.0 : 120.0;
        const double phc = locked ? frac_d(static_cast<double>(g_clk.beat) * kCpb[ref->st.sync]) : 0.0;
        ref->block(l.data() + at, r.data() + at, ref_l->data() + at, ref_r->data() + at, n, locked, phc, bpm);
      }
      busy = inst.m->render_st(inst.state, l.data() + at, r.data() + at, o.l.data() + at, o.r.data() + at, static_cast<std::uint32_t>(n));
      at += n;
    }
    return o;
  }
};

double max_err(const std::vector<float>& a, const std::vector<double>& b) {
  double e = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    e = std::max(e, std::fabs(static_cast<double>(a[i]) - b[i]));
  }
  return e;
}
bool finite_all(const std::vector<float>& v) {
  for (float x : v) {
    if (!std::isfinite(x)) {
      return false;
    }
  }
  return true;
}
// The pan of a mono DC input of 0.5: L' - R' = 2 p M = p.
std::vector<float> pan_of(const Out& o) {
  std::vector<float> p(o.l.size());
  for (std::size_t i = 0; i < p.size(); ++i) {
    p[i] = o.l[i] - o.r[i];
  }
  return p;
}
// Crossings of zero, as sample positions with linear interpolation: rising (negative to non-negative) or falling.
std::vector<double> crossings(const std::vector<float>& s, bool rising) {
  std::vector<double> t;
  for (std::size_t i = 1; i < s.size(); ++i) {
    const bool hit = rising ? (s[i - 1] < 0.0f && s[i] >= 0.0f) : (s[i - 1] > 0.0f && s[i] <= 0.0f);
    if (hit) {
      t.push_back(static_cast<double>(i - 1) + static_cast<double>(s[i - 1]) / static_cast<double>(s[i - 1] - s[i]));
    }
  }
  return t;
}
double max_step(const std::vector<float>& s, std::size_t from = 1) {
  double m = 0.0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < s.size(); ++i) {
    m = std::max(m, std::fabs(static_cast<double>(s[i]) - s[i - 1]));
  }
  return m;
}
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

TEST_CASE("autopan: the descriptor says a WIDENS effect with six parameters, four presets and render_st") {
  Lib lib;
  NEED(lib.open(O3M_AUTOPAN_HOST_SO));
  const o3m_module& m = *lib.entry;
  CHECK(m.kind == O3M_KIND_EFFECT);
  NEED(m.param_count == 6u);
  CHECK(AUTOPAN_PARAM_SYNC == 0);
  CHECK(AUTOPAN_PARAM_PERIOD == 1);
  CHECK(AUTOPAN_PARAM_DEPTH == 2);
  CHECK(AUTOPAN_PARAM_SHAPE == 3);
  CHECK(AUTOPAN_PARAM_PHASE == 4);
  CHECK(AUTOPAN_PARAM_HEADPHONE == 5);
  CHECK(m.render_st != nullptr);
  CHECK(m.render != nullptr);  // the loader requires the mono render of every effect
  CHECK((m.fx_flags & O3M_FX_WIDENS) != 0u);
  for (std::uint32_t i : {0u, 3u, 4u}) {
    CHECK((m.params[i].flags & O3M_PARAM_INT) != 0u);
  }
  CHECK((m.params[AUTOPAN_PARAM_HEADPHONE].flags & O3M_PARAM_BOOL) != 0u);  // shown as OFF / ON: effects have no value labels in ABI 1.12 (the spec's MODE SPEAKERS / HEADPHONE)
  CHECK(m.params[AUTOPAN_PARAM_SYNC].max == 15.0f);
  CHECK(m.params[AUTOPAN_PARAM_SYNC].def == 3.0f);  // `16`, one bar
  CHECK((m.params[AUTOPAN_PARAM_PERIOD].flags & O3M_PARAM_LOG) != 0u);
  CHECK(m.params[AUTOPAN_PARAM_PERIOD].min == 100.0f);
  CHECK(m.params[AUTOPAN_PARAM_PERIOD].max == 20000.0f);
  CHECK(m.params[AUTOPAN_PARAM_DEPTH].def == 70.0f);
  CHECK(m.params[AUTOPAN_PARAM_PHASE].max == 360.0f);
  CHECK(m.params[AUTOPAN_PARAM_HEADPHONE].def == 0.0f);  // SPEAKERS: mono-compatible, by default
  CHECK(m.preset_count == 4u);
  CHECK(sizeof(float) * 40u + 256u <= AUTOPAN_STATE_BYTES);
}

TEST_CASE("autopan: the presets are in range, whole where the parameter is whole, and each keeps the mono sum") {
  Lib lib;
  NEED(lib.open(O3M_AUTOPAN_HOST_SO));
  const o3m_module& m = *lib.entry;
  NEED(m.preset_count == 4u);
  for (std::uint32_t k = 0; k < m.preset_count; ++k) {
    const o3m_preset& pr = m.presets[k];
    CAPTURE(pr.name);
    CHECK(pr.count >= 1u);
    CHECK(pr.count <= m.param_count);
    Rig rig;
    NEED(rig.ok);
    for (std::uint32_t i = 0; i < m.param_count; ++i) {
      const float v = i < pr.count ? pr.values[i] : m.params[i].def;
      CHECK(v >= m.params[i].min);
      CHECK(v <= m.params[i].max);
      if ((m.params[i].flags & O3M_PARAM_INT) != 0u) {
        CHECK(v == std::floor(v));
      }
      rig.inst.set(i, v);
    }
    const std::vector<float> x = noise(2048, 100u + k);
    const Out o = rig.run(x, x, 64, [](std::size_t at) { clock_playing(at, 120.0); });
    double worst = 0.0, side = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      worst = std::max(worst, std::fabs(0.5 * (static_cast<double>(o.l[i]) + o.r[i]) - x[i]));
      side = std::max(side, std::fabs(static_cast<double>(o.l[i]) - o.r[i]));
    }
    CHECK(worst < 1e-6);
    CHECK(side > 0.05);  // and it does pan
  }
}

TEST_CASE("autopan: SPEAKERS tracks a double reference over 90 settings, keeps the mono sum and a stereo input's side") {
  const int syncs[] = {0, 1, 7, 11, 15};
  const int shapes[] = {0, 1, 2};
  const double depths[] = {0.0, 50.0, 100.0};
  const int phases[] = {0, 135};
  double worst = 0.0, worst_mid = 0.0;
  int n_set = 0;
  for (int sync : syncs) {
    for (int shape : shapes) {
      for (double depth : depths) {
        for (int phase : phases) {
          Set st;
          st.sync = sync;
          st.period = 130.0 + 70.0 * (n_set % 7);
          st.depth = depth;
          st.shape = shape;
          st.phase = phase;
          const bool stereo = (n_set % 2) == 0;
          const std::vector<float> l = noise(2048, 11u + static_cast<std::uint32_t>(n_set));
          const std::vector<float> r = stereo ? noise(2048, 911u + static_cast<std::uint32_t>(n_set)) : l;
          const double bpm = n_set % 3 == 0 ? 90.0 : (n_set % 3 == 1 ? 120.0 : 174.0);
          const int clock_kind = n_set % 3;  // 0 stub, 1 playing, 2 stopped
          Rig rig;
          NEED(rig.ok);
          rig.set(st);
          Ref ref;
          ref.st = st;
          std::vector<double> rl, rr;
          const Out o = rig.run(l, r, 64,
                                [&](std::size_t at) {
                                  if (clock_kind == 0) {
                                    clock_stub();
                                  } else if (clock_kind == 1) {
                                    clock_playing(at, bpm);
                                  } else {
                                    clock_stopped(bpm);
                                  }
                                },
                                &ref, &rl, &rr);
          CAPTURE(sync);
          CAPTURE(shape);
          CAPTURE(depth);
          CAPTURE(phase);
          CAPTURE(clock_kind);
          const double e = std::max(max_err(o.l, rl), max_err(o.r, rr));
          worst = std::max(worst, e);
          CHECK(e < 1e-4);
          for (std::size_t i = 0; i < l.size(); ++i) {
            worst_mid = std::max(worst_mid, std::fabs(0.5 * (static_cast<double>(o.l[i]) + o.r[i]) - 0.5 * (static_cast<double>(l[i]) + r[i])));
          }
          if (depth == 0.0) {  // Depth 0: the input bit for bit
            CHECK(std::memcmp(o.l.data(), l.data(), l.size() * sizeof(float)) == 0);
            CHECK(std::memcmp(o.r.data(), r.data(), r.size() * sizeof(float)) == 0);
          }
          ++n_set;
        }
      }
    }
  }
  CHECK(n_set == 90);
  CHECK(worst_mid < 1e-6);  // the mono sum is the input's mid to the rounding of two float adds (1 LSB of 16 bits is 3e-5)
  MESSAGE("autopan SPEAKERS: worst error against the reference " << worst << ", worst mono-sum deviation " << worst_mid);
}

TEST_CASE("autopan: a stereo input keeps its side: S' - S_in is the pan times the mid") {
  Rig rig;
  NEED(rig.ok);
  Set st;
  st.sync = 0;
  st.period = 400.0;
  st.depth = 100.0;
  rig.set(st);
  const std::vector<float> l = noise(4096, 5), r = noise(4096, 6);
  const Out o = rig.run(l, r);
  // the pan from a mono run of the same instance's settings is not available mid-stream; the identity itself: (L' - R') / 2 - (L - R) / 2 = p M and (L' + R') / 2 = M, so p M is the same
  // number on both sides: a = L' - L = R - R'
  double worst = 0.0;
  for (std::size_t i = 0; i < l.size(); ++i) {
    worst = std::max(worst, std::fabs((static_cast<double>(o.l[i]) - l[i]) - (static_cast<double>(r[i]) - o.r[i])));
  }
  CHECK(worst < 1e-6);
}

TEST_CASE("autopan: the free rate is the period to 0.1 %") {
  const double periods[] = {100.0, 400.0, 2000.0};
  for (double period : periods) {
    Rig rig;
    NEED(rig.ok);
    Set st;
    st.sync = 0;
    st.period = period;
    st.depth = 100.0;
    rig.set(st);
    const std::size_t n = static_cast<std::size_t>(kFs * std::max(20.0, period * 0.001 * 12.0));
    const Out o = rig.run(dc(n, 0.5f), dc(n, 0.5f));
    const std::vector<float> p = pan_of(o);
    const double f = frequency(p.data(), p.size());
    CAPTURE(period);
    CHECK(std::fabs(f * period * 0.001 - 1.0) < 1e-3);
  }
}

TEST_CASE("autopan: synced, the pan crosses zero at the start and the middle of every beat; Phase shifts it; a stopped clock free-runs at the tempo in force") {
  const double beat = kFs * 60.0 / 120.0;  // 22050 samples
  const std::size_t n = static_cast<std::size_t>(beat * 8.0);
  {
    Rig rig;
    NEED(rig.ok);
    Set st;
    st.sync = 7;  // `4`: a cycle of 4 steps, one beat
    st.depth = 100.0;
    rig.set(st);
    const Out o = rig.run(dc(n, 0.5f), dc(n, 0.5f), 64, [](std::size_t at) { clock_playing(at, 120.0); });
    const std::vector<float> p = pan_of(o);
    const std::vector<double> up = crossings(p, true), down = crossings(p, false);
    NEED(up.size() >= 6u);
    NEED(down.size() >= 6u);
    for (std::size_t k = 0; k < up.size(); ++k) {
      CHECK(std::fabs(up[k] - beat * static_cast<double>(k + 1)) <= 64.0);  // the start of a beat, to one block
    }
    for (std::size_t k = 0; k < down.size(); ++k) {
      CHECK(std::fabs(down[k] - beat * (static_cast<double>(k) + 0.5)) <= 64.0);  // the middle
    }
  }
  {
    Rig rig;
    NEED(rig.ok);
    Set st;
    st.sync = 7;
    st.depth = 100.0;
    st.phase = 90;  // a quarter cycle early: the pan is a cosine
    rig.set(st);
    const Out o = rig.run(dc(n, 0.5f), dc(n, 0.5f), 64, [](std::size_t at) { clock_playing(at, 120.0); });
    const std::vector<float> p = pan_of(o);
    const std::vector<double> up = crossings(p, true), down = crossings(p, false);
    NEED(up.size() >= 6u);
    NEED(down.size() >= 6u);
    for (std::size_t k = 0; k < down.size(); ++k) {
      CHECK(std::fabs(down[k] - beat * (static_cast<double>(k) + 0.25)) <= 64.0);
    }
    for (std::size_t k = 0; k < up.size(); ++k) {
      CHECK(std::fabs(up[k] - beat * (static_cast<double>(k) + 0.75)) <= 64.0);
    }
  }
  // the clock does not play: the phase free-runs at the tempo in force (a stub answers the project's tempo with rc 1; a stopped sequencer answers it with rc 0), 1 cycle a beat at Sync `4`
  const double tempos[] = {90.0, 120.0, 174.0};
  for (double bpm : tempos) {
    for (int kind = 0; kind < 2; ++kind) {
      Rig rig;
      NEED(rig.ok);
      Set st;
      st.sync = 7;
      st.depth = 100.0;
      rig.set(st);
      const std::size_t len = static_cast<std::size_t>(kFs * 20.0);
      const Out o = rig.run(dc(len, 0.5f), dc(len, 0.5f), 64, [&](std::size_t) {
        if (kind == 0) {
          clock_stub();
          g_clk.tempo = static_cast<std::uint32_t>(bpm * 100.0);  // rc 1, the tempo is the project's
        } else {
          clock_stopped(bpm);
        }
      });
      const std::vector<float> p = pan_of(o);
      CAPTURE(bpm);
      CAPTURE(kind);
      CHECK(std::fabs(frequency(p.data(), p.size()) / (bpm / 60.0) - 1.0) < 1e-3);
    }
  }
}

TEST_CASE("autopan: a WRAPPED clock re-aligns the phase in that block, with a ramp and not a step") {
  Rig rig;
  NEED(rig.ok);
  Set st;
  st.sync = 11;  // `1`: four cycles a beat, a fast pan
  st.depth = 100.0;
  rig.set(st);
  const std::size_t n = 64u * 300u, jump = 64u * 150u + 64u * 0u;
  const std::size_t restart = jump - static_cast<std::size_t>(0.3 * kFs * 0.5);  // the beat goes back to 0.3-ish of where it was: a new pattern at an unrelated phase
  Ref ref;
  ref.st = st;
  std::vector<double> rl, rr;
  const Out o = rig.run(dc(n, 0.5f), dc(n, 0.5f), 64,
                        [&](std::size_t at) {
                          if (at < jump) {
                            clock_playing(at, 120.0);
                          } else {
                            clock_playing(at, 120.0, restart, at == jump);
                          }
                        },
                        &ref, &rl, &rr);
  CHECK(std::max(max_err(o.l, rl), max_err(o.r, rr)) < 1e-4);
  const std::vector<float> p = pan_of(o);
  // the value at the last sample of the jump block is the new beat's LFO (the parabola is within 0.0011 of the sine)
  const double beat_new = static_cast<double>(jump + 63u + 1u - restart) / kFs * 2.0;
  const double want = ref_lfo(frac_d(beat_new * 4.0), 0);
  CHECK(std::fabs(static_cast<double>(p[jump + 63u]) - want) < 2e-3);
  // and a jump that big is a ramp over the block: the largest step in the jump block is under the block's ramp (2 / 64) plus the LFO's own slope
  double step = 0.0;
  for (std::size_t i = jump; i < jump + 64u; ++i) {
    step = std::max(step, std::fabs(static_cast<double>(p[i]) - p[i - 1]));
  }
  CHECK(step < 2.0 / 64.0 + 0.01);
}

TEST_CASE("autopan: SQUARE goes through the 5 ms one-pole (once a block): no step in a sample over its bound, and it reaches the full depth") {
  Rig rig;
  NEED(rig.ok);
  Set st;
  st.sync = 0;
  st.period = 500.0;
  st.depth = 60.0;
  st.shape = 2;
  rig.set(st);
  const std::size_t n = static_cast<std::size_t>(kFs * 4.0);
  const Out o = rig.run(dc(n, 0.5f), dc(n, 0.5f));
  const std::vector<float> p = pan_of(o);
  const double kb = 1.0 - std::exp(-64.0 / (0.005 * kFs));  // the one-pole's step response over a block of 64 frames
  const double bound = 2.0 * 0.6 * kb / 64.0 * 1.01;      // |raw - p_last| <= 2 Depth: the largest step of the block's ramp
  double hi = 0.0;
  for (float v : p) {
    hi = std::max(hi, std::fabs(static_cast<double>(v)));
  }
  CHECK(max_step(p) <= bound);
  CHECK(max_step(p) > 0.5 * bound);  // it does step (smoothed), it is not a slow ramp
  CHECK(hi > 0.6 * 0.999);
  CHECK(hi < 0.6 * 1.001);
}

TEST_CASE("autopan: any block size and in == out give the same pan, to the ramp's own error") {
  Set st;
  st.sync = 0;
  st.period = 100.0;  // the fastest swing: the linear ramp's error is largest
  st.depth = 100.0;
  const std::vector<float> l = noise(8192, 3), r = noise(8192, 4);
  Rig a;
  NEED(a.ok);
  a.set(st);
  const Out ref = a.run(l, r, 64);
  const std::uint32_t blocks[] = {1u, 7u, 32u, 64u, 100u, 128u, 200u};
  for (std::uint32_t b : blocks) {
    Rig rig;
    NEED(rig.ok);
    rig.set(st);
    const Out o = rig.run(l, r, b);
    double worst = 0.0;
    for (std::size_t i = 0; i < l.size(); ++i) {
      worst = std::max(worst, std::fabs(static_cast<double>(o.l[i]) - ref.l[i]));
      worst = std::max(worst, std::fabs(static_cast<double>(o.r[i]) - ref.r[i]));
    }
    CAPTURE(b);
    CHECK(worst < 4e-3);
    CHECK(finite_all(o.l));
  }
  {  // in == out: the same bits as two buffers
    Rig rig;
    NEED(rig.ok);
    rig.set(st);
    std::vector<float> xl = l, xr = r;
    for (std::size_t at = 0; at < xl.size(); at += 64) {
      clock_stub();
      rig.inst.m->render_st(rig.inst.state, xl.data() + at, xr.data() + at, xl.data() + at, xr.data() + at, 64u);
    }
    CHECK(std::memcmp(xl.data(), ref.l.data(), xl.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(xr.data(), ref.r.data(), xr.size() * sizeof(float)) == 0);
  }
}

TEST_CASE("autopan: a synced, playing clock gives the same stream at block 7, 32, 100 and 200: the clock phase is taken only where a chunk starts") {
  Set st;
  st.sync = 11;  // `1`: four cycles a beat, a fast pan, so a chunk that runs late is visible
  st.depth = 100.0;
  const std::vector<float> l = noise(8192, 5), r = noise(8192, 6);
  Rig a;
  NEED(a.ok);
  a.set(st);
  const Out ref = a.run(l, r, 64, [](std::size_t at) { clock_playing(at, 120.0); });
  const std::uint32_t blocks[] = {7u, 32u, 100u, 200u};
  for (std::uint32_t b : blocks) {
    Rig rig;
    NEED(rig.ok);
    rig.set(st);
    const Out o = rig.run(l, r, b, [](std::size_t at) { clock_playing(at, 120.0); });
    double worst = 0.0;
    for (std::size_t i = 0; i < l.size(); ++i) {
      worst = std::max(worst, std::fabs(static_cast<double>(o.l[i]) - ref.l[i]));
      worst = std::max(worst, std::fabs(static_cast<double>(o.r[i]) - ref.r[i]));
    }
    CAPTURE(b);
    CHECK(worst < 4e-3);  // with the clock taken at every block, a chunk starting mid-block runs up to 63 samples of phase late (about 0.09 of the pan here)
    CHECK(finite_all(o.l));
  }
}

TEST_CASE("autopan: HEADPHONE switched on mid-chunk primes from the chunk's own cues: any block size (7, 32, 100, 200) gives the same stream") {
  Set st;
  st.sync = 0;
  st.period = 300.0;  // a full swing, so the far ear's cue is far from the centre's at the switch
  st.depth = 100.0;
  const std::vector<float> l = noise(8192, 13), r = noise(8192, 14);
  const std::uint32_t blocks[] = {7u, 32u, 100u, 200u};
  for (std::uint32_t b : blocks) {
    // where the toggle lands in this run: the first block start at or after 2048 (a chunk boundary only for some sizes, mid-chunk for the rest)
    const std::size_t flip = ((2048u + b - 1u) / b) * b;
    // the reference: one call up to the flip under SPEAKERS, then HEADPHONE on — the same mode timeline and the same chunk boundaries as the run under test
    Rig ref;
    NEED(ref.ok);
    ref.set(st);
    std::vector<float> rl(8192), rr(8192);
    clock_stub();
    ref.inst.m->render_st(ref.inst.state, l.data(), r.data(), rl.data(), rr.data(), static_cast<std::uint32_t>(flip));
    ref.inst.set(AUTOPAN_PARAM_HEADPHONE, 1.0f);
    for (std::size_t at = flip; at < 8192u; at += 64u) {
      clock_stub();
      const std::size_t n = std::min<std::size_t>(64u, 8192u - at);
      ref.inst.m->render_st(ref.inst.state, l.data() + at, r.data() + at, rl.data() + at, rr.data() + at, static_cast<std::uint32_t>(n));
    }
    Rig rig;
    NEED(rig.ok);
    rig.set(st);
    std::vector<float> ol(8192), orr(8192);
    for (std::size_t at = 0; at < 8192u;) {
      const std::size_t n = std::min<std::size_t>(b, 8192u - at);
      rig.inst.set(AUTOPAN_PARAM_HEADPHONE, at >= 2048u ? 1.0f : 0.0f);
      clock_stub();
      rig.inst.m->render_st(rig.inst.state, l.data() + at, r.data() + at, ol.data() + at, orr.data() + at, static_cast<std::uint32_t>(n));
      at += n;
    }
    CAPTURE(b);
    double worst = 0.0;
    for (std::size_t i = 0; i < 8192u; ++i) {
      worst = std::max(worst, std::fabs(static_cast<double>(ol[i]) - rl[i]));
      worst = std::max(worst, std::fabs(static_cast<double>(orr[i]) - rr[i]));
    }
    CHECK(worst == 0.0);  // same chunks, same flip sample: bit for bit (without the snap the prime chunk runs on the stale cues)
    CHECK(finite_all(ol));
  }
}

TEST_CASE("autopan: HEADPHONE switched on mid-chunk snaps the cues to the chunk's targets: the far ear is delayed from the first primed frame") {
  for (std::uint32_t b : {7u, 32u, 100u, 200u}) {  // 32 flips at a chunk boundary, the others mid-chunk
    Rig rig;
    NEED(rig.ok);
    Set st;
    st.sync = 0;
    st.period = 300.0;  // at the flip the pan is well off centre, so the far-ear cue is clear
    st.depth = 100.0;
    st.shape = 1;
    rig.set(st);
    const std::size_t n = 4096;
    std::vector<float> l(n, 0.0f), r(n, 0.0f);
    const std::size_t flip = ((2048u + b - 1u) / b) * b;  // where the toggle lands in this run
    const std::size_t c0 = (flip / 64u) * 64u;            // the chunk the prime happens in
    const std::size_t imp = flip + 8u;                    // an impulse inside that chunk, after the flip
    l[imp] = 1.0f;
    r[imp] = 1.0f;
    std::vector<float> ol(n), orr(n);
    for (std::size_t at = 0; at < n;) {
      const std::size_t nn = std::min<std::size_t>(b, n - at);
      rig.inst.set(AUTOPAN_PARAM_HEADPHONE, at >= 2048u ? 1.0f : 0.0f);
      clock_stub();
      rig.inst.m->render_st(rig.inst.state, l.data() + at, r.data() + at, ol.data() + at, orr.data() + at, static_cast<std::uint32_t>(nn));
      at += nn;
    }
    // the pan the prime chunk ends on: p1 = Depth x lfo(frac((c0 + 64) inc)); > 0 here, so R is the far ear
    const double inc = 1000.0 / (st.period * kFs);
    const double p1 = ref_lfo(frac_d(static_cast<double>(c0 + 64u) * inc), 1);
    const double dfar = std::fabs(p1) * std::min(0.6e-3 * kFs, 30.0);
    auto peak = [&](const std::vector<float>& v) {
      std::size_t k = imp;
      for (std::size_t j = imp; j < std::min(n, imp + 40u); ++j) {
        if (std::fabs(static_cast<double>(v[j])) > std::fabs(static_cast<double>(v[k]))) {
          k = j;
        }
      }
      return k;
    };
    const std::size_t pl = peak(ol), pr = peak(orr);
    CAPTURE(b);
    CAPTURE(flip);
    CAPTURE(p1);
    CHECK(p1 > 0.3);                                    // the setup: a clear far-ear cue at the prime chunk
    CHECK(static_cast<double>(pr - imp) > 0.6 * dfar);  // the far ear is delayed by the chunk's target from the first primed frame (without the snap the stale cue delays it by 0)
    CHECK(static_cast<double>(pl - imp) < 3.0);        // the near ear is not delayed
  }
}

TEST_CASE("autopan: the tail flag ORs over the chunks of a block: a live first chunk keeps it at block 100 and 200") {
  for (std::uint32_t b : {100u, 200u}) {
    Rig rig;
    NEED(rig.ok);
    Set st;  // SPEAKERS: no memory, a chunk is live exactly when its input is not all zero
    st.sync = 0;
    st.period = 1000.0;
    rig.set(st);
    const std::size_t n = static_cast<std::size_t>(b) * 3u;
    std::vector<float> l(n, 0.0f), r(n, 0.0f);
    for (std::size_t i = 0; i < 64u; ++i) {  // live only in the first chunk of the first block
      l[i] = 0.25f;
      r[i] = -0.25f;
    }
    std::vector<float> ol(n), orr(n);
    std::uint32_t busy_first = 0;
    for (std::size_t at = 0; at < n; at += b) {
      clock_stub();
      const std::uint32_t busy = rig.inst.m->render_st(rig.inst.state, l.data() + at, r.data() + at, ol.data() + at, orr.data() + at, b);
      if (at == 0u) {
        busy_first = busy;
      }
    }
    CAPTURE(b);
    CHECK(busy_first == 1u);  // the block's last chunks are silent; only the OR keeps the flag
  }
}

TEST_CASE("autopan: the tail contract, SPEAKERS and HEADPHONE: 1 while the input lives, 0 once silent, exact zeros") {
  for (int mode = 0; mode < 2; ++mode) {
    Rig rig;
    NEED(rig.ok);
    Set st;
    st.sync = 0;
    st.period = 300.0;
    st.depth = 100.0;
    st.shape = 2;
    st.mode = mode;
    rig.set(st);
    const std::vector<float> x = noise(640, 8);
    const std::vector<float> z(2560, 0.0f);
    std::vector<float> in(x);
    in.insert(in.end(), z.begin(), z.end());
    std::uint32_t first_zero = 0xFFFFFFFFu;
    std::uint32_t busy_in = 1;
    std::vector<float> ol(64), orr(64);
    bool tail_ok = true;
    for (std::size_t at = 0, blk = 0; at < in.size(); at += 64, ++blk) {
      clock_stub();
      const std::uint32_t b = rig.inst.m->render_st(rig.inst.state, in.data() + at, in.data() + at, ol.data(), orr.data(), 64u);
      if (at < x.size()) {
        busy_in &= b;
      } else if (b == 0u) {
        if (first_zero == 0xFFFFFFFFu) {
          first_zero = static_cast<std::uint32_t>(blk);
        }
        const bool first = static_cast<std::uint32_t>(blk) == first_zero;
        for (std::size_t j = 0; j < 64; ++j) {  // the block that ends the tail may hold the last ring of the low-pass (under -120 dBFS); every block after it is exact zeros
          tail_ok = tail_ok && (first ? (std::fabs(ol[j]) < 1e-6f && std::fabs(orr[j]) < 1e-6f) : (ol[j] == 0.0f && orr[j] == 0.0f));
        }
      } else if (first_zero != 0xFFFFFFFFu) {
        tail_ok = false;  // it came back to 1 without input
      }
    }
    CAPTURE(mode);
    CHECK(busy_in == 1u);
    CHECK(first_zero != 0xFFFFFFFFu);
    CHECK(first_zero <= 10u + 8u);  // within eight blocks of the end of the burst (the ring's 32 frames and the low-pass's ring)
    CHECK(tail_ok);
  }
}

TEST_CASE("autopan: hostile parameters clamp and hostile input does not poison HEADPHONE for good") {
  const float bad[] = {std::nanf(""), INFINITY, -INFINITY, 1e30f, -1e30f};
  for (float v : bad) {
    Rig rig;
    NEED(rig.ok);
    for (std::uint32_t id = 0; id < 6u; ++id) {
      rig.inst.set(id, v);
    }
    const std::vector<float> x = noise(1024, 9);
    const Out o = rig.run(x, x, 64, [](std::size_t) { clock_stub(); });
    CAPTURE(v);
    CHECK(finite_all(o.l));
    CHECK(finite_all(o.r));
  }
  Rig rig;
  NEED(rig.ok);
  Set st;
  st.sync = 0;
  st.depth = 100.0;
  st.mode = 1;
  rig.set(st);
  std::vector<float> x = noise(64u * 40u, 10);
  for (std::size_t i = 64u * 10u; i < 64u * 11u; ++i) {
    x[i] = i % 3u == 0u ? std::nanf("") : (i % 3u == 1u ? INFINITY : 1e30f);
  }
  const Out o = rig.run(x, x);
  bool clean = true;
  for (std::size_t i = 64u * 12u; i < x.size(); ++i) {  // the block after the hostile one is clean
    clean = clean && std::isfinite(o.l[i]) && std::isfinite(o.r[i]) && std::fabs(o.l[i]) < 4.0f && std::fabs(o.r[i]) < 4.0f;
  }
  CHECK(clean);
}

TEST_CASE("autopan: no allocation in render_st, in either mode") {
  for (int mode = 0; mode < 2; ++mode) {
    Rig rig;
    NEED(rig.ok);
    Set st;
    st.sync = 7;
    st.depth = 80.0;
    st.shape = 2;
    st.mode = mode;
    rig.set(st);
    const std::vector<float> x = noise(64, 12);
    std::vector<float> ol(64), orr(64);
    clock_playing(0, 120.0);
    const std::size_t before = g_news;
    for (int i = 0; i < 2000; ++i) {
      rig.inst.m->render_st(rig.inst.state, x.data(), x.data(), ol.data(), orr.data(), 64u);
    }
    const std::size_t after = g_news;
    CHECK(after == before);
  }
}

TEST_CASE("autopan: HEADPHONE tracks a double reference over 36 settings") {
  const int shapes[] = {0, 1, 2};
  const double depths[] = {40.0, 100.0};
  const int syncs[] = {0, 7};
  const int phases[] = {0, 90, 200};
  double worst = 0.0;
  int n_set = 0;
  for (int shape : shapes) {
    for (double depth : depths) {
      for (int sync : syncs) {
        for (int phase : phases) {
          Set st;
          st.sync = sync;
          st.period = 160.0 + 90.0 * (n_set % 5);
          st.depth = depth;
          st.shape = shape;
          st.phase = phase;
          st.mode = 1;
          const bool stereo = (n_set % 2) == 1;
          const std::vector<float> l = noise(4096, 41u + static_cast<std::uint32_t>(n_set));
          const std::vector<float> r = stereo ? noise(4096, 77u + static_cast<std::uint32_t>(n_set)) : l;
          Rig rig;
          NEED(rig.ok);
          rig.set(st);
          Ref ref;
          ref.st = st;
          std::vector<double> rl, rr;
          const Out o = rig.run(l, r, 64, [&](std::size_t at) { clock_playing(at, 120.0 + 7.0 * (n_set % 4)); }, &ref, &rl, &rr);
          const double e = std::max(max_err(o.l, rl), max_err(o.r, rr));
          worst = std::max(worst, e);
          CAPTURE(shape);
          CAPTURE(depth);
          CAPTURE(sync);
          CAPTURE(phase);
          CHECK(e < 1e-4);
          ++n_set;
        }
      }
    }
  }
  CHECK(n_set == 36);
  MESSAGE("autopan HEADPHONE: worst error against the reference " << worst);
}

TEST_CASE("autopan: SPEAKERS is bit for bit what it was after a HEADPHONE period") {
  Set st;
  st.sync = 0;
  st.period = 700.0;
  st.depth = 90.0;
  const std::vector<float> l = noise(64u * 60u, 21), r = noise(64u * 60u, 22);
  Rig a, b;
  NEED(a.ok);
  NEED(b.ok);
  a.set(st);
  b.set(st);
  const Out ref = a.run(l, r);
  std::vector<float> ol(l.size()), orr(l.size());
  for (std::size_t blk = 0; blk < 60u; ++blk) {
    b.inst.set(AUTOPAN_PARAM_HEADPHONE, (blk >= 10u && blk < 25u) ? 1.0f : 0.0f);
    clock_stub();
    b.inst.m->render_st(b.inst.state, l.data() + blk * 64u, r.data() + blk * 64u, ol.data() + blk * 64u, orr.data() + blk * 64u, 64u);
  }
  CHECK(std::memcmp(ol.data(), ref.l.data(), 10u * 64u * sizeof(float)) == 0);
  CHECK(std::memcmp(ol.data() + 25u * 64u, ref.l.data() + 25u * 64u, 35u * 64u * sizeof(float)) == 0);
  CHECK(std::memcmp(orr.data() + 25u * 64u, ref.r.data() + 25u * 64u, 35u * 64u * sizeof(float)) == 0);
  // and the HEADPHONE period differs (it is not the same sound)
  CHECK(std::memcmp(ol.data() + 12u * 64u, ref.l.data() + 12u * 64u, 8u * 64u * sizeof(float)) != 0);
}

TEST_CASE("autopan: HEADPHONE at p = +1: the far ear 26.5 frames later and darker by the reference's figures; at p = 0 L = R; no click on a sweep") {
  // p = +1 held: SQUARE, Depth 100, a 20 s period (the first half cycle is +1 from the first sample). A stereo input with its energy in R only gives both ears 0.5 x at p = 1
  // (the far ear R keeps the side: R - a = x - 0.5 x), so the two ears can be compared; the delay of an impulse is read from the centroid of each ear, the low-pass's own group delay removed.
  Set st;
  st.sync = 0;
  st.period = 20000.0;
  st.depth = 100.0;
  st.shape = 2;
  st.mode = 1;
  const double fc_near = std::min(20000.0, 0.45 * kFs);
  const double c_near = 1.0 - std::exp(-2.0 * kPi * fc_near / kFs);
  const double c_far = 1.0 - std::exp(-2.0 * kPi * 1800.0 / kFs);
  const double d = 0.6e-3 * kFs;
  {
    Rig rig;
    NEED(rig.ok);
    rig.set(st);
    const std::size_t n = 44100u + 256u;
    std::vector<float> l(n, 0.0f), r(n, 0.0f);
    r[44100] = 1.0f;
    const Out o = rig.run(l, r);
    double sn = 0, cn = 0, sf = 0, cf = 0;
    for (std::size_t k = 0; k < 200u; ++k) {
      sn += o.l[44100 + k];
      cn += static_cast<double>(k) * o.l[44100 + k];
      sf += o.r[44100 + k];
      cf += static_cast<double>(k) * o.r[44100 + k];
    }
    CHECK(sn == doctest::Approx(0.5).epsilon(0.02));  // both ears carry half the impulse (the one-pole's DC gain is 1)
    CHECK(sf == doctest::Approx(0.5).epsilon(0.02));
    const double delay = cf / sf - cn / sn - ((1.0 - c_far) / c_far - (1.0 - c_near) / c_near);  // the far ear's centroid less the near ear's, the low-passes' group delays removed
    MESSAGE("HEADPHONE far-ear delay " << delay << " frames (0.6 ms is " << d << ")");
    CHECK(std::fabs(delay - d) <= 0.5);
  }
  {
    Rig rig;
    NEED(rig.ok);
    rig.set(st);
    const std::size_t n = 44100u;
    std::vector<float> l(n, 0.0f);
    const std::vector<float> r = sine(n, 0.8, 5000.0);
    const Out o = rig.run(l, r);
    const double near_a = amp_at(o.l, 22050, 44100, 5000.0);
    const double far_a = amp_at(o.r, 22050, 44100, 5000.0);
    const double w = 2.0 * kPi * 5000.0 / kFs;
    const auto hmag = [&](double c) { return std::abs(std::complex<double>(c, 0.0) / (std::complex<double>(1.0, 0.0) - (1.0 - c) * std::polar(1.0, -w))); };
    const double fdel = d - std::floor(d);
    const double interp = std::abs((1.0 - fdel) + fdel * std::polar(1.0, -w));
    const double want_db = 20.0 * std::log10(hmag(c_far) * interp / hmag(c_near));
    const double got_db = 20.0 * std::log10(far_a / near_a);
    MESSAGE("HEADPHONE far ear at 5 kHz: " << got_db << " dB against the near ear (reference " << want_db << " dB)");
    CHECK(want_db < -3.0);  // it is darker, by a clear margin
    CHECK(std::fabs(got_db - want_db) <= 0.5);
  }
  {  // p = 0 (Depth 0): L = R, bit for bit, for a mono input
    Rig rig;
    NEED(rig.ok);
    Set z = st;
    z.depth = 0.0;
    z.shape = 0;
    rig.set(z);
    const std::vector<float> x = noise(4096, 31);
    const Out o = rig.run(x, x);
    CHECK(std::memcmp(o.l.data(), o.r.data(), x.size() * sizeof(float)) == 0);
  }
  {  // a sweep of p (a 2 Hz sine pan, full depth, on a 300 Hz tone): no click, the largest sample step stays where the tone's slope with the pan's gain puts it
    Rig rig;
    NEED(rig.ok);
    Set sw;
    sw.sync = 0;
    sw.period = 500.0;
    sw.depth = 100.0;
    sw.mode = 1;
    rig.set(sw);
    const std::size_t n = static_cast<std::size_t>(kFs * 4.0);
    const std::vector<float> x = sine(n, 0.5, 300.0);
    const Out o = rig.run(x, x);
    const double bound = 2.0 * kPi * 300.0 / kFs * 0.5 * 2.0 * 1.05;  // the near ear's gain is (1 + p) <= 2
    CHECK(max_step(o.l, 8) < bound);
    CHECK(max_step(o.r, 8) < bound);
  }
}

TEST_CASE("autopan: the packed .tmod passes the loader's checks, relocates, reads back as module.json says, and its hot part is small") {
  const Tmod t = load_tmod(O3M_AUTOPAN_TMOD);
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
  CHECK(t.h.param_count == AUTOPAN_PARAM_COUNT);
  CHECK(t.h.view_count == 0u);
  CHECK(t.h.state_bytes <= AUTOPAN_STATE_BYTES);
  CHECK(t.h.hot_size <= 1536u);  // the SRAM_L slab (14 KiB) is shared by every loaded module: keep the hot part under about 1.5 KiB
  MESSAGE("autopan.tmod: " << t.file_bytes << " B file, hot " << t.h.hot_size << " B, cold " << (t.h.image_size - t.h.hot_size) << " B, state " << t.h.state_bytes << " B");
}
