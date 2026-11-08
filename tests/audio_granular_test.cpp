// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-GRAN: the granular voice (play mode 7; audio/voices.h GRANULAR). The oracles are written here, independently of the voice, from the grain model that audio/voices.h GRANULAR documents:
// the grain's geometry (u16 position units, the float point conversion), the three loop types (forward, backward, ping-pong) as the
// real-valued position of a reflecting or wrapping read, the amplitude shapes by the integer map of the position onto a 1000-entry table, and the table's own values (half sine,
// triangle) against the formulas.
// Also: no read ever leaves the sample (a poisoned guard around it), the output does not depend on how the block is cut into spans, a granular note's life (attack, release, retrigger),
// and the descriptor's sanitising. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/static_slot.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

// doctest here has no exceptions: REQUIRE is not available, so a failed precondition is a failed check and the test case returns.
#define REQ(cond) \
  do {            \
    if (!(cond)) { \
      FAIL_CHECK("precondition failed: " #cond); \
      return;     \
    }             \
  } while (0)

namespace {

namespace audio = o3os::audio;
namespace haudio = o3os::hal::audio;
using audio::SampleDesc;
using audio::VoicePool;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kGuard = 4096;     // poisoned samples on each side of the sample
constexpr std::uint32_t kMaxLen = 65535;
constexpr std::int16_t kPoison = 31000;

std::int16_t g_buf[kGuard + kMaxLen + kGuard];

VoicePool& pool_a() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}
VoicePool& pool_b() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

enum class Fill { Wave, Ramp, Dc };

// The sample of `len` frames at g_buf + kGuard, the rest poison. Wave: a sum of two sines of amplitude 5000 + 3000 (never near the poison); Ramp: i; Dc: 20000.
const std::int16_t* fill(std::uint32_t len, Fill kind) {
  for (std::uint32_t i = 0; i < kGuard + kMaxLen + kGuard; ++i) {
    g_buf[i] = kPoison;
  }
  for (std::uint32_t i = 0; i < len; ++i) {
    double v;
    switch (kind) {
      case Fill::Ramp: v = static_cast<double>(i % 30000u); break;
      case Fill::Dc: v = 20000.0; break;
      default: v = 5000.0 * std::sin(0.071 * static_cast<double>(i)) + 3000.0 * std::sin(0.0113 * static_cast<double>(i) + 1.0); break;
    }
    g_buf[kGuard + i] = static_cast<std::int16_t>(std::lround(v));
  }
  return g_buf + kGuard;
}

struct Cfg {
  std::uint32_t len = kMaxLen;
  unsigned gr_len = 300;
  unsigned pos = 20000;
  unsigned shape = 0;  // 0 square, 1 triangle, 2 gauss
  unsigned type = 0;   // 0 forward, 1 backward, 2 ping-pong
  unsigned note = 60;
  Fill kind = Fill::Wave;
};

// The descriptor of `c` over a sample already in place (fill() was called for c.len).
SampleDesc desc_over(const std::int16_t* pcm, const Cfg& c) {
  SampleDesc d{pcm, c.len, 0, 0, 65536};
  d.granular = true;
  d.gr_len = static_cast<std::uint16_t>(c.gr_len);
  d.gr_pos = static_cast<std::uint16_t>(c.pos);
  d.gr_shape = static_cast<std::uint8_t>(c.shape);
  d.loop_mode = static_cast<std::uint8_t>(c.type);
  return d;
}

SampleDesc make_desc(const Cfg& c) { return desc_over(fill(c.len, c.kind), c); }

// ---- The oracle ---------------------------------------------------------------------------------------------------------------------------------------------------------
struct Geom {
  int start = 0, avail = 0, l1 = 0, l2 = 0;
};

// The granular geometry (audio/voices.h GRANULAR, grain_geom), typed again independently: u16 units, u16 points, float conversion; held so that the grain is at least one sample long and inside the sample.
Geom ref_geom(std::uint32_t len, unsigned gr_len, unsigned pos) {
  const std::uint16_t units = static_cast<std::uint16_t>(gr_len * 65535u / len);
  const int half = units / 2;
  const int down = static_cast<int>(pos) - half;
  const int up = static_cast<int>(pos) + half;
  const std::uint16_t s = down > 0 ? static_cast<std::uint16_t>(down) : 0;
  const std::uint16_t e = up < 65535 ? static_cast<std::uint16_t>(up) : 65535;
  const std::uint16_t p1 = static_cast<std::uint16_t>(s + 1);
  const std::uint16_t p2 = static_cast<std::uint16_t>(e - 1);
  const float r = static_cast<float>(len) / 65535.0f;
  const auto at = [r](std::uint16_t g) { return static_cast<int>(static_cast<float>(g) * r); };
  Geom g;
  g.start = std::min(at(s), static_cast<int>(len) - 1);
  g.avail = static_cast<int>(len) - g.start;
  g.l1 = std::max(at(p1) - g.start, 0);
  g.l2 = std::max(at(p2) - g.start, 0);
  g.l2 = std::max(g.l2, g.l1 + 1);
  return g;
}

// The amplitude table the voice indexes: gain(shape, i) as 16 bits, by the formulas (the half sine and the triangle), 0 at 999.
double table_gain(unsigned shape, int i) {
  if (shape == 0) {
    return 65535.0;
  }
  if (i >= 999) {
    return 0.0;
  }
  if (shape == 1) {
    return std::floor(65535.0 * static_cast<double>(i <= 500 ? i : 1000 - i) / 500.0 + 0.5);
  }
  return std::floor(65535.0 * std::sin(3.14159265358979323846 * static_cast<double>(i) / 1000.0) + 0.5);
}

struct Ref {
  Geom g;
  const std::int16_t* pcm = nullptr;
  unsigned shape = 0, type = 0;
  double inc = 1.0;
  double p = 0.0;
  bool bwd = false;
  std::uint32_t n = 0;  // samples made

  Ref(const Cfg& c, const std::int16_t* data, double step) : g(ref_geom(c.len, c.gr_len, c.pos)), pcm(data), shape(c.shape), type(c.type), inc(step), bwd(c.type == 1) {}

  double at(int i) const { return static_cast<double>(pcm[g.start + (i < 0 ? 0 : (i >= g.avail ? g.avail - 1 : i))]); }

  // The next output sample (the attack ramp of kAttackSamples included: unity gain, no amp envelope).
  double next() {
    const int x = static_cast<int>(std::floor(p));
    const double f = p - static_cast<double>(x);
    const double s = at(x) + (at(x + 1) - at(x)) * f;
    const int xe = (bwd && f > 0.0) ? x + 1 : x;
    const int k = xe - g.l1;
    const int span = g.l2 - g.l1;
    const int idx = k <= 0 ? 0 : (k >= span ? 999 : (k * 999) / span);
    const double amp = n >= audio::kAttackSamples ? 1.0 : static_cast<double>(n) / static_cast<double>(audio::kAttackSamples);
    const double out = s * table_gain(shape, idx) / 65536.0 * amp;
    ++n;
    const double lo = g.l1, hi = g.l2, w = hi - lo;
    if (type == 0) {
      p += inc;
      if (p >= hi) {
        p = lo + std::fmod(p - hi, w);
      }
    } else if (type == 1) {
      p -= inc;
      if (p <= lo) {
        p = hi - std::fmod(lo - p, w);
      }
    } else if (!bwd) {
      p += inc;
      if (p >= hi) {
        p = std::max(2.0 * hi - 1.0 - p, lo);
        bwd = true;
      }
    } else {
      p -= inc;
      if (p <= lo) {
        p = std::min(2.0 * lo + 1.0 - p, hi);
        bwd = false;
      }
    }
    return out;
  }
};

// Renders `samples` samples (rounded up to whole blocks) of track 0 into `out` (the voice's own feed).
void render(VoicePool& p, std::vector<std::int16_t>& out, unsigned samples) {
  for (unsigned b = 0; b < (samples + kBlk - 1u) / kBlk; ++b) {
    p.begin_block();
    p.render(0, kBlk);
    p.end_block();
    const std::int16_t* f = p.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out.push_back(f != nullptr ? f[i] : std::int16_t{0});
    }
  }
}

double inc_of(unsigned note) { return static_cast<double>(audio::tuned_inc_q32(note, 0, 0)) / 4294967296.0; }

}  // namespace

// ---- The shapes ---------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: a DC sample under each shape is the table's gain at the integer map of the position, entry by entry") {
  // len 65535: one sample is one position unit, so with gr_len 1002 the grain is 1002 wide and its loop span 1000: the index is floor(k x 999 / 1000), every table entry is reached.
  for (unsigned shape = 0; shape < 3; ++shape) {
    Cfg c;
    c.shape = shape;
    c.gr_len = 1002;
    c.pos = 30000;
    c.kind = Fill::Dc;
    VoicePool& p = pool_a();
    p.set_sample(0, make_desc(c));
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> out;
    render(p, out, 1100);  // the grain's 1000 and the wrap
    const Geom g = ref_geom(c.len, c.gr_len, c.pos);
    REQ(g.l2 - g.l1 == 1000);
    unsigned bad = 0;
    for (int x = static_cast<int>(audio::kAttackSamples); x < g.l2; ++x) {  // past the attack ramp, before the wrap (x is the sample's index: inc 1.0)
      const int k = x - g.l1;
      const int idx = k <= 0 ? 0 : (k * 999) / 1000;
      const int want = (20000 * static_cast<int>(table_gain(shape, idx))) >> 16;
      bad += std::abs(out[static_cast<std::size_t>(x)] - want) > 1 ? 1u : 0u;  // the formulas round where a reference generator may be a unit off
    }
    CHECK_MESSAGE(bad == 0u, "shape " << shape << ": " << bad << " samples off the table");
  }
}

TEST_CASE("granular: the grain's geometry follows the u16 arithmetic of the grain model, at the edges and for a grain longer than the sample") {
  struct Case {
    std::uint32_t len;
    unsigned gr_len, pos;
  };
  const Case cases[] = {{65535, 300, 20000}, {65535, 300, 0}, {65535, 300, 65535}, {1000, 441, 32768}, {300, 441, 32768}, {200000, 44, 65535}, {200000, 44, 0}, {5, 441, 100}, {1, 44, 0}};
  for (const Case& cs : cases) {
    Cfg c;
    c.len = cs.len;
    c.gr_len = cs.gr_len;
    c.pos = cs.pos;
    c.type = 0;
    // a ramp makes the position visible: the first sample past the attack is the grain's start + x (square shape: gain 65535/65536)
    std::vector<std::int16_t> data(cs.len);
    for (std::uint32_t i = 0; i < cs.len; ++i) {
      data[i] = static_cast<std::int16_t>(i % 20000u);
    }
    SampleDesc d{data.data(), cs.len, 0, 0, 65536};
    d.granular = true;
    d.gr_len = static_cast<std::uint16_t>(cs.gr_len);
    d.gr_pos = static_cast<std::uint16_t>(cs.pos);
    VoicePool& p = pool_a();
    p.set_sample(0, d);
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> out;
    render(p, out, 200);
    const Geom g = ref_geom(cs.len, cs.gr_len, cs.pos);
    const int x = 100;  // a sample inside the first block, past the attack, at position x (before any wrap when the loop is long enough; else the reference's wrap decides)
    Ref r(c, data.data(), 1.0);
    double want = 0.0;
    for (int i = 0; i <= x; ++i) {
      want = r.next();
    }
    CHECK_MESSAGE(std::abs(out[static_cast<std::size_t>(x)] - want) <= 3.0, "len " << cs.len << " gr_len " << cs.gr_len << " pos " << cs.pos << ": got " << out[static_cast<std::size_t>(x)] << " want " << want
                                                                                  << " (grain start " << g.start << ", loop " << g.l1 << ".." << g.l2 << ")");
    CHECK(g.l2 > g.l1);
    CHECK(g.start >= 0);
    CHECK(g.start < static_cast<int>(cs.len));
  }
}

// ---- The loop types -----------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: forward, backward and ping-pong grains follow the real-valued oracle sample by sample, across wraps and turns, at several pitches and shapes") {
  struct Case {
    unsigned type, shape, note, gr_len, pos;
  };
  const Case cases[] = {
      {0, 0, 60, 300, 20000}, {0, 2, 67, 300, 20000}, {0, 1, 55, 777, 40000}, {1, 0, 60, 300, 20000}, {1, 2, 64, 500, 1000}, {1, 1, 72, 300, 65535},
      {2, 0, 60, 300, 20000}, {2, 2, 67, 300, 20000}, {2, 1, 50, 500, 30000}, {2, 2, 84, 300, 100},
  };
  for (const Case& cs : cases) {
    Cfg c;
    c.type = cs.type;
    c.shape = cs.shape;
    c.note = cs.note;
    c.gr_len = cs.gr_len;
    c.pos = cs.pos;
    VoicePool& p = pool_a();
    p.set_sample(0, make_desc(c));
    REQ(p.note_on(0, cs.note, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> out;
    render(p, out, 1500);
    Ref r(c, g_buf + kGuard, inc_of(cs.note));
    int worst = 0;
    unsigned at_worst = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
      const double want = r.next();
      const int e = static_cast<int>(std::lround(std::abs(out[i] - want)));
      if (e > worst) {
        worst = e;
        at_worst = static_cast<unsigned>(i);
      }
    }
    CHECK_MESSAGE(worst <= 3, "type " << cs.type << " shape " << cs.shape << " note " << cs.note << " gr_len " << cs.gr_len << " pos " << cs.pos << ": worst error " << worst << " at sample " << at_worst);
  }
}

TEST_CASE("granular: a backward grain starts at the loop's end at once, and a ping-pong grain reaches the end then comes back") {
  Cfg c;
  c.kind = Fill::Ramp;
  c.len = 20000;
  c.gr_len = 400;
  c.pos = 30000;  // len 20000: ratio 0.305; the grain ~400 samples
  c.type = 1;
  VoicePool& p = pool_a();
  p.set_sample(0, make_desc(c));
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  render(p, out, 400);
  const Geom g = ref_geom(c.len, c.gr_len, c.pos);
  // a ramp: out ~ start + x. After the first sample the voice runs backward from loop2: strictly falling, 1 a sample
  for (int i = 40; i < 200; ++i) {
    CHECK(out[static_cast<std::size_t>(i)] < out[static_cast<std::size_t>(i - 1)]);
  }
  CHECK(out[100] > g.start + g.l1 - 5);
  c.type = 2;
  VoicePool& q = pool_b();
  q.set_sample(0, make_desc(c));
  REQ(q.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> o2;
  render(q, o2, 800);
  bool rose = false, fell = false;
  for (std::size_t i = 40; i + 1 < o2.size(); ++i) {
    rose = rose || o2[i + 1] > o2[i];
    fell = fell || o2[i + 1] < o2[i];
  }
  CHECK(rose);
  CHECK(fell);
}

// ---- Safety -------------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: no read leaves the sample, whatever the position, length, pitch or type (a poisoned guard around it stays out of the output)") {
  const std::uint32_t lens[] = {1000, 300, 20000, 65535, 2, 7};
  const unsigned positions[] = {0, 1, 32768, 65534, 65535};
  const unsigned gr_lens[] = {44, 441, 44100};
  const unsigned notes[] = {24, 60, 96, 127};
  unsigned bad = 0, runs = 0;
  for (const std::uint32_t len : lens) {
    // The sample depends on the length only, and nothing writes to it: filled and scaled once per length, not for each of its 180 runs (the double
    // sines of fill() made this the slowest QEMU suite). The signal is small so that a poison read (31000) cannot hide: |signal| <= 4000.
    const std::int16_t* pcm = fill(len, Fill::Wave);
    for (std::uint32_t i = 0; i < len; ++i) {
      g_buf[kGuard + i] = static_cast<std::int16_t>(g_buf[kGuard + i] / 3);
    }
    for (const unsigned pos : positions) {
      for (const unsigned gl : gr_lens) {
        for (unsigned type = 0; type < 3; ++type) {
          for (const unsigned note : notes) {
            Cfg c;
            c.len = len;
            c.pos = pos;
            c.gr_len = gl;
            c.type = type;
            c.shape = (len + pos + type) % 3u;
            const SampleDesc d = desc_over(pcm, c);
            VoicePool& p = pool_a();
            p.set_sample(0, d);
            if (!p.note_on(0, note, 0, audio::kVelocityDefault)) {
              continue;
            }
            std::vector<std::int16_t> out;
            render(p, out, 300);
            ++runs;
            for (const std::int16_t v : out) {
              bad += std::abs(v) > 4500 ? 1u : 0u;
            }
          }
        }
      }
    }
  }
  CHECK(runs > 1000u);
  CHECK(bad == 0u);
}

TEST_CASE("granular: the output does not depend on how the block is cut into spans") {
  for (unsigned type = 0; type < 3; ++type) {
    Cfg c;
    c.type = type;
    c.shape = 2;
    c.gr_len = 260;
    VoicePool& a = pool_a();
    VoicePool& b = pool_b();
    a.set_sample(0, make_desc(c));
    b.set_sample(0, make_desc(c));
    REQ(a.note_on(0, 63, 0, audio::kVelocityDefault));
    REQ(b.note_on(0, 63, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> oa, ob;
    render(a, oa, 20 * kBlk);
    for (unsigned blk = 0; blk < 20; ++blk) {
      b.begin_block();
      b.render(0, 13);
      b.render(13, 14);
      b.render(14, 37);
      b.render(37, kBlk);
      b.end_block();
      const std::int16_t* f = b.feed(0);
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        ob.push_back(f[i]);
      }
    }
    CHECK(oa == ob);
  }
}

// ---- A note's life ------------------------------------------------------------------------------------------------------------------------------------------------------
TEST_CASE("granular: the note attacks from silence, releases to silence, a retrigger restarts the grain, and the amp envelope and gain apply") {
  Cfg c;
  c.kind = Fill::Dc;
  c.shape = 0;
  c.gr_len = 600;
  VoicePool& p = pool_a();
  SampleDesc d = make_desc(c);
  d.gain = 32768;  // half
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  render(p, out, 128);
  CHECK(out[0] == 0);  // the attack starts from silence
  CHECK(std::abs(out[100] - 10000) <= 2);  // half of 20000 x 65535 / 65536
  p.note_off(0);
  render(p, out, 600);
  CHECK(out[128 + 256 + 70] == 0);  // 256 samples of release, then the residual's 64
  CHECK(p.voices_in_use() == 0u);
  // retrigger: a new note on the track while it sounds takes the voice over; the grain restarts (position 0: the first sample past the attack is the grain's start)
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  render(p, out, 128);
  REQ(p.note_on(0, 64, 0, audio::kVelocityDefault));
  CHECK(p.voices_in_use() == 1u);
  std::vector<std::int16_t> again;
  render(p, again, 128);
  CHECK(std::abs(again[100] - 10000) <= 2);
}

TEST_CASE("granular: set_sample drops the loop, the slices and the wavetable of a granular descriptor, and holds the shape and the type") {
  const std::int16_t* pcm = fill(4000, Fill::Wave);
  SampleDesc d{pcm, 4000, 10, 3000, 65536};
  d.granular = true;
  d.wt_size = 256;
  d.beat_slice = true;
  d.gr_shape = 9;
  d.loop_mode = 7;
  VoicePool& p = pool_a();
  p.set_sample(0, d);
  const SampleDesc& s = p.sample(0);
  CHECK(s.granular);
  CHECK(s.loop_start == 0u);
  CHECK(s.loop_end == 0u);
  CHECK(s.wt_size == 0u);
  CHECK_FALSE(s.beat_slice);
  CHECK(s.gr_shape == 0u);
  CHECK(s.loop_mode == 0u);
}

TEST_CASE("granular: a non-granular descriptor is untouched by all of it (the plain forward loop plays as before)") {
  const std::int16_t* pcm = fill(4000, Fill::Ramp);
  SampleDesc d{pcm, 4000, 0, 4000, 65536};
  VoicePool& p = pool_a();
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  render(p, out, 128);
  CHECK_FALSE(p.sample(0).granular);
  CHECK(out[100] == 100);  // a ramp read at 1.0: the sample's own value
}

// OS-RATEPITCH: the rate factor scales the increment of a granular voice like any other, so a grain of a 88,200 Hz sample at the root plays as one of a 44,100 Hz sample an octave up, and one of a
// 22,050 Hz sample as the one an octave down (the factors 2 and 1/2 are exact in the Q28; the octave's increment is the root's shifted): the same samples, all three grain types.
TEST_CASE("granular: a grain takes the sample's rate factor: 88,200 Hz at the root is 44,100 Hz an octave up, 22,050 Hz is an octave down (OS-RATEPITCH)") {
  for (unsigned type = 0; type < 3; ++type) {
    for (const std::uint32_t hz : {88200u, 22050u}) {
      Cfg c;
      c.type = type;
      c.shape = 2;
      c.gr_len = 300;
      c.pos = 20000;
      SampleDesc plain = make_desc(c);
      SampleDesc scaled = plain;
      scaled.rate_q28 = audio::rate_factor_q28(hz);
      std::vector<std::int16_t> want, got;
      VoicePool& p = pool_a();
      p.set_sample(0, plain);
      REQ(p.note_on(0, hz == 88200u ? 72u : 48u, 0, audio::kVelocityDefault));
      render(p, want, 1100);
      VoicePool& q = pool_b();
      q.set_sample(0, scaled);
      REQ(q.note_on(0, 60, 0, audio::kVelocityDefault));
      render(q, got, 1100);
      REQ(want.size() == got.size());
      CHECK_MESSAGE(want == got, "type " << type << " rate " << hz);
      p.cut(0);
      q.cut(0);
    }
  }
}

// ---- OS-CEWTENV W5: the granular length destination (slot 3's enable bit 0x02 on a granular instrument, spec F12) -----------------------------------------------
namespace {

// This test binary's grlen storage, bound once (on the unit it is O3OS_SDRAM_NOINIT in the SDRAM slack window, app_main.cpp; voices.h grlen_bind).
audio::FilterMod g_w5_slot_mods[audio::kDescSlots];
audio::ModGen g_w5_voice_gens[audio::kMaxVoices];
struct W5Bind {
  W5Bind() { audio::grlen_bind(g_w5_slot_mods, g_w5_voice_gens); }
};
const W5Bind g_w5_bind{};

// An ADSR length source at `amount` from its first evaluation (every stage 0, sustain 1: the sustain holds sustain x amount).
audio::FilterMod len_adsr(float amount) {
  audio::FilterMod m{};
  m.kind = audio::kFmodEnv;
  m.amount = amount;
  m.sustain = 1.0f;
  return m;
}

}  // namespace

namespace o3os::audio {
// The test's view of the pool's private state (voices.h friend): the voice's grain fields and mmask, the bound grlen arrays.
struct GranLenW5Test {
  static const VoicePool::Voice& voice(VoicePool& p, unsigned i) { return p.v_[i]; }
  static bool len_on(VoicePool& p, unsigned i) { return (p.v_[i].mmask & VoicePool::kMmLen) != 0u; }
  static float gen_e_out(unsigned i) { return g_w5_voice_gens[i].e_out; }
  static const FilterMod* slot_mod(const VoicePool& p, unsigned slot) { return p.grlen_slot(slot); }
  static unsigned voice_of(VoicePool& p, unsigned track) { return p.find(track); }
};
}  // namespace o3os::audio

// F12: length = base + mod x (max - 44), max = min(sample length, 44,100), held to 44..max; the new length applies at the grain's next wrap, never mid-grain.
TEST_CASE("grlen W5: slot 3 bit 0x02 drives the grain length by F12, held within the grain and applied at the next wrap") {
  const std::uint32_t L = 20000;  // max = min(20000, 44100) = 20000
  const std::int16_t* pcm = fill(L, Fill::Ramp);
  VoicePool& p = pool_a();
  Cfg c;
  c.len = L;
  c.gr_len = 441;
  c.pos = 10000;
  c.shape = 0;
  c.type = 0;
  c.note = 60;  // the root: inc 1
  c.kind = Fill::Ramp;
  SampleDesc d = desc_over(pcm, c);
  const audio::FilterMod half = len_adsr(0.5f);
  p.set_sample(0, d, &half);  // length = 441 + 0.5 x (20000 - 44) = 10419
  const audio::FilterMod* sm = o3os::audio::GranLenW5Test::slot_mod(p, 0);
  REQ(sm != nullptr);
  CHECK(sm->kind == audio::kFmodEnv);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vi = o3os::audio::GranLenW5Test::voice_of(p, 0);
  CHECK(o3os::audio::GranLenW5Test::len_on(p, vi));
  const auto& v = o3os::audio::GranLenW5Test::voice(p, vi);
  const Geom base = ref_geom(L, 441, 10000);
  CHECK(v.gstart == static_cast<std::uint32_t>(base.start));  // the note starts on the instrument's own grain length
  CHECK(v.gl1 == static_cast<std::uint32_t>(base.l1));
  CHECK(v.gl2 == static_cast<std::uint32_t>(base.l2));
  std::vector<std::int16_t> out;
  render(p, out, kBlk);  // one block: the modulator has been evaluated once (mneed), the grain has not wrapped (441 samples at inc 1)
  CHECK(std::abs(o3os::audio::GranLenW5Test::gen_e_out(vi) - 10419.0f) < 0.5f);  // base + mod x (max - 44)
  CHECK(v.gl1 == static_cast<std::uint32_t>(base.l1));  // held within the grain: the block's length has not moved it yet
  CHECK(v.gl2 == static_cast<std::uint32_t>(base.l2));
  render(p, out, 30 * kBlk);  // many wraps now
  const Geom want = ref_geom(L, 10419, 10000);
  CHECK(v.gstart == static_cast<std::uint32_t>(want.start));  // applied at a wrap, not before
  CHECK(v.gl1 == static_cast<std::uint32_t>(want.l1));
  CHECK(v.gl2 == static_cast<std::uint32_t>(want.l2));
  p.cut(0);
}

TEST_CASE("grlen W5: the length is held to 44..max (an amount of 1 clamps to the sample, -1 to 44)") {
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Ramp);
  for (const float amount : {1.0f, -1.0f}) {
    VoicePool& p = pool_a();
    Cfg c;
    c.len = L;
    c.gr_len = 441;
    c.pos = 10000;
    c.note = 60;
    c.kind = Fill::Ramp;
    SampleDesc d = desc_over(pcm, c);
    const audio::FilterMod lm = len_adsr(amount);
    p.set_sample(0, d, &lm);
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    const unsigned vi = o3os::audio::GranLenW5Test::voice_of(p, 0);
    std::vector<std::int16_t> out;
    render(p, out, kBlk);
    const float want = amount >= 0.0f ? 20000.0f : 44.0f;  // 441 + 1 x (20000 - 44) = 20397 > max: max; 441 - 19956 < 44: 44
    CHECK(std::abs(o3os::audio::GranLenW5Test::gen_e_out(vi) - want) < 0.5f);
    p.cut(0);
  }
}

// ---- OS-CEWTENV W6b: the step FX z (model id 98, group kModGroupGranLen), the grain length by F11 -------------------------------------------------------------------
namespace {

// This test binary's FX storage for the FX z (voices.h fxe_bind; on the unit it is SDRAM, app_main.cpp).
audio::FxEnvMem g_w6b_mem;
struct W6bBind {
  W6bBind() { audio::fxe_bind(&g_w6b_mem); }
};
const W6bBind g_w6b_bind{};

void z_fx(VoicePool& p, unsigned value) { p.fx_event(0, audio::kModGroupGranLen, audio::kFxGranLen, value); }
void z_end(VoicePool& p) { p.fx_event(0, audio::kModGroupGranLen, 0u, 0u); }

// A note of a granular instrument (length L, descriptor grain length 441, position 10000) with z = `value` set before it (value > 100: none): the grain's geometry at the note start.
void z_start_geom(std::uint32_t L, unsigned value, unsigned want_len) {
  const std::int16_t* pcm = fill(L, Fill::Ramp);
  VoicePool& p = pool_a();
  p.cut(0);
  Cfg c;
  c.len = L;
  c.gr_len = 441;
  c.pos = 10000 < L ? 10000 : L / 2;
  c.note = 60;
  c.kind = Fill::Ramp;
  p.set_sample(0, desc_over(pcm, c));
  if (value <= 100u) {
    z_fx(p, value);
  }
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vi = o3os::audio::GranLenW5Test::voice_of(p, 0);
  const auto& v = o3os::audio::GranLenW5Test::voice(p, vi);
  const Geom g = ref_geom(L, want_len, c.pos);
  CHECK(v.gstart == static_cast<std::uint32_t>(g.start));
  CHECK(v.gl1 == static_cast<std::uint32_t>(g.l1));
  CHECK(v.gl2 == static_cast<std::uint32_t>(g.l2));
  p.cut(0);
  z_end(p);
}

}  // namespace

// F11: 0 is 44 samples, 50 is 4410 (100 ms), 100 is 44100 (1 s), held to the sample's length.
TEST_CASE("grlen W6b: the FX z maps 0 / 50 / 100 to 44 / 4410 / 44100 samples and a short sample holds it") {
  CHECK(audio::gran_len_fx_samples(0) == 44u);
  CHECK(audio::gran_len_fx_samples(50) == 4410u);
  CHECK(audio::gran_len_fx_samples(100) == 44100u);
  CHECK(audio::gran_len_fx_samples(255) == 44100u);
  z_start_geom(20000, 101, 441);    // no z: the descriptor's own length
  z_start_geom(20000, 0, 44);
  z_start_geom(20000, 50, 4410);
  z_start_geom(20000, 100, 20000);  // 44100 held to the 20000-sample sample
  z_start_geom(1000, 50, 1000);     // 4410 held to 1000
  z_start_geom(1000, 0, 44);
}

// A live z reaches the sounding grain at its next wrap; its end puts the descriptor's length back.
TEST_CASE("grlen W6b: a z set while the note sounds acts at the next grain wrap, and the ModEnd restores the descriptor's length") {
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Ramp);
  VoicePool& p = pool_a();
  p.cut(0);
  Cfg c;
  c.len = L;
  c.gr_len = 441;
  c.pos = 10000;
  c.note = 60;
  c.kind = Fill::Ramp;
  p.set_sample(0, desc_over(pcm, c));
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vi = o3os::audio::GranLenW5Test::voice_of(p, 0);
  const auto& v = o3os::audio::GranLenW5Test::voice(p, vi);
  z_fx(p, 50);
  CHECK(v.gl1 == static_cast<std::uint32_t>(ref_geom(L, 441, 10000).l1));  // not mid-grain
  std::vector<std::int16_t> out;
  render(p, out, 40 * kBlk);
  const Geom z50 = ref_geom(L, 4410, 10000);
  CHECK(v.gstart == static_cast<std::uint32_t>(z50.start));
  CHECK(v.gl2 == static_cast<std::uint32_t>(z50.l2));
  z_end(p);
  render(p, out, 40 * kBlk);
  const Geom back = ref_geom(L, 441, 10000);
  CHECK(v.gstart == static_cast<std::uint32_t>(back.start));
  CHECK(v.gl2 == static_cast<std::uint32_t>(back.l2));
  p.cut(0);
}

TEST_CASE("grlen W6b: a non-granular instrument ignores the FX z (the render is bit-identical)") {
  const std::uint32_t L = 4000;
  const std::int16_t* pcm = fill(L, Fill::Wave);
  const SampleDesc plain{pcm, L, 0, L, 65536};
  VoicePool& a = pool_a();
  a.cut(0);
  a.set_sample(0, plain);
  REQ(a.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> oa;
  render(a, oa, 12 * kBlk);
  a.cut(0);
  VoicePool& b = pool_b();
  b.cut(0);
  b.set_sample(0, plain);
  z_fx(b, 20);
  REQ(b.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> ob;
  render(b, ob, 12 * kBlk);
  CHECK(oa == ob);
  b.cut(0);
  z_end(b);
}

TEST_CASE("grlen W5: a wavetable instrument ignores bit 0x02, and a granular one without the bit renders bit-identically") {
  // a wavetable of four 256-frame windows (each a ramp of its own level, so a window change is audible)
  static std::int16_t wtpcm[4 * 256];
  for (unsigned w = 0; w < 4; ++w) {
    for (unsigned i = 0; i < 256; ++i) {
      wtpcm[w * 256 + i] = static_cast<std::int16_t>((w + 1) * 1000 + i);
    }
  }
  SampleDesc wd{wtpcm, 256, 0, 256, 65536};
  wd.wt_size = 256;
  wd.wt_windows = 4;
  wd.wt_pos = 0;
  VoicePool& a = pool_a();
  a.set_sample(0, wd);  // no grlen: as before W5
  REQ(a.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> oa;
  render(a, oa, 8 * kBlk);
  VoicePool& b = pool_b();
  const audio::FilterMod full = len_adsr(1.0f);
  b.set_sample(0, wd, &full);  // bit 0x02 on a wavetable: ignored (the slot's source is cleared for a non-granular descriptor)
  const audio::FilterMod* sm = o3os::audio::GranLenW5Test::slot_mod(b, 0);
  REQ(sm != nullptr);
  CHECK(sm->kind == audio::kFmodNone);
  REQ(b.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vi = o3os::audio::GranLenW5Test::voice_of(b, 0);
  CHECK_FALSE(o3os::audio::GranLenW5Test::len_on(b, vi));
  std::vector<std::int16_t> ob;
  render(b, ob, 8 * kBlk);
  CHECK(oa == ob);
  a.cut(0);
  b.cut(0);
  // a granular instrument without the bit: a grlen source of kind none changes nothing either
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Wave);
  Cfg c;
  c.len = L;
  c.gr_len = 300;
  c.pos = 20000;
  c.note = 60;
  SampleDesc gd = desc_over(pcm, c);
  audio::FilterMod none_mod{};
  a.set_sample(0, gd);
  REQ(a.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> ga;
  render(a, ga, 40 * kBlk);
  b.set_sample(0, gd, &none_mod);
  REQ(b.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> gb;
  render(b, gb, 40 * kBlk);
  CHECK(ga == gb);
  const unsigned gi = o3os::audio::GranLenW5Test::voice_of(b, 0);
  CHECK_FALSE(o3os::audio::GranLenW5Test::len_on(b, gi));
  a.cut(0);
  b.cut(0);
}

// Review fold: a pool whose grlen storage was never bound (every other test binary, the qemu and bench mains) plays a granular note without touching the arrays.
TEST_CASE("grlen W5: a granular note on a pool with no grlen_bind plays and ignores a length source") {
  struct Unbind {  // the file's binding comes back at the end of the test
    Unbind() { audio::grlen_bind(nullptr, nullptr); }
    ~Unbind() { audio::grlen_bind(g_w5_slot_mods, g_w5_voice_gens); }
  } unbound;
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Wave);
  Cfg c;
  c.len = L;
  c.gr_len = 300;
  c.pos = 20000;
  c.note = 60;
  SampleDesc gd = desc_over(pcm, c);
  const audio::FilterMod full = len_adsr(1.0f);
  VoicePool& a = pool_a();
  a.set_sample(0, gd);
  REQ(a.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> oa;
  render(a, oa, 40 * kBlk);
  VoicePool& b = pool_b();
  b.set_sample(0, gd, &full);  // a source with no storage to keep it in: dropped
  CHECK(o3os::audio::GranLenW5Test::slot_mod(b, 0) == nullptr);
  REQ(b.note_on(0, 60, 0, audio::kVelocityDefault));
  CHECK_FALSE(o3os::audio::GranLenW5Test::len_on(b, o3os::audio::GranLenW5Test::voice_of(b, 0)));
  std::vector<std::int16_t> ob;
  render(b, ob, 40 * kBlk);
  CHECK(oa == ob);
  a.cut(0);
  b.cut(0);
}

// Review fold: the staged descriptor carries the length source (StageCell::grlen) and the render takes it with the descriptor at the next begin_block.
TEST_CASE("grlen W5: stage_sample carries the length source and take_stage keeps it with the descriptor") {
  static audio::StageCell cell;
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Ramp);
  Cfg c;
  c.len = L;
  c.gr_len = 441;
  c.pos = 10000;
  c.note = 60;
  c.kind = Fill::Ramp;
  SampleDesc d = desc_over(pcm, c);
  VoicePool& p = pool_a();
  p.set_sample(0, d);  // no source yet
  REQ(o3os::audio::GranLenW5Test::slot_mod(p, 0) != nullptr);
  CHECK(o3os::audio::GranLenW5Test::slot_mod(p, 0)->kind == audio::kFmodNone);
  p.bind_stage(&cell);
  const audio::FilterMod half = len_adsr(0.5f);
  REQ(p.stage_sample(0, d, nullptr, 0, nullptr, &half));
  CHECK(cell.grlen.kind == audio::kFmodEnv);
  CHECK(o3os::audio::GranLenW5Test::slot_mod(p, 0)->kind == audio::kFmodNone);  // not taken before the render
  std::vector<std::int16_t> out;
  render(p, out, kBlk);  // begin_block takes it
  CHECK(o3os::audio::GranLenW5Test::slot_mod(p, 0)->kind == audio::kFmodEnv);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  CHECK(o3os::audio::GranLenW5Test::len_on(p, o3os::audio::GranLenW5Test::voice_of(p, 0)));
  p.cut(0);
  REQ(p.stage_sample(0, d, nullptr, 0, nullptr, nullptr));  // a descriptor staged without a source clears it
  render(p, out, kBlk);
  CHECK(o3os::audio::GranLenW5Test::slot_mod(p, 0)->kind == audio::kFmodNone);
  p.bind_stage(nullptr);
}

// Review fold: any source kind, here an LFO (bipolar), reaches the length; and clearing the source mid-note sends the grain back to gr_len at its next wrap.
TEST_CASE("grlen W5: an LFO source drives the length, and clearing the source returns the grain to gr_len") {
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Ramp);
  Cfg c;
  c.len = L;
  c.gr_len = 441;
  c.pos = 10000;
  c.shape = 0;
  c.note = 60;
  c.kind = Fill::Ramp;
  SampleDesc d = desc_over(pcm, c);
  audio::FilterMod lm{};
  lm.kind = audio::kFmodLfo;
  lm.shape = 0;  // reverse saw: not at its midpoint at the start
  lm.rate = 0;
  lm.amount = 0.5f;
  lm.lfo_amount = 0.5f;
  VoicePool& p = pool_a();
  p.set_sample(0, d, &lm);
  REQ(o3os::audio::GranLenW5Test::slot_mod(p, 0)->kind == audio::kFmodLfo);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vi = o3os::audio::GranLenW5Test::voice_of(p, 0);
  CHECK(o3os::audio::GranLenW5Test::len_on(p, vi));
  std::vector<std::int16_t> out;
  render(p, out, 40 * kBlk);
  const float e = o3os::audio::GranLenW5Test::gen_e_out(vi);
  CHECK(e >= 44.0f);
  CHECK(e <= 20000.0f);
  CHECK(std::abs(e - 441.0f) > 1.0f);  // the LFO moved it off the base
  const auto& v = o3os::audio::GranLenW5Test::voice(p, vi);
  const Geom want = ref_geom(L, static_cast<unsigned>(e), 10000);
  // the grain was built at a wrap a few blocks back, so the LFO may have moved on a little since: near, not equal, and far from the base
  const Geom base0 = ref_geom(L, 441, 10000);
  CHECK(std::abs(static_cast<int>(v.gl2) - static_cast<int>(want.l2)) <= 200);
  CHECK(v.gl2 != static_cast<std::uint32_t>(base0.l2));
  // the source goes off (a live edit: the slot's descriptor re-set with no 0x02): the grain is rebuilt with gr_len at its next wrap
  p.set_sample(0, d);
  render(p, out, 400 * kBlk);  // far more than a grain (about 8200 frames): at least one wrap
  CHECK_FALSE(o3os::audio::GranLenW5Test::len_on(p, vi));
  const Geom base = ref_geom(L, 441, 10000);
  CHECK(v.gl1 == static_cast<std::uint32_t>(base.l1));
  CHECK(v.gl2 == static_cast<std::uint32_t>(base.l2));
  p.cut(0);
}

// Review fold: the length ADSR's key follows the NoteOff, so its release runs (the length falls back toward the base) while the voice's own release sounds.
TEST_CASE("grlen W5: a NoteOff releases the length ADSR") {
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Ramp);
  Cfg c;
  c.len = L;
  c.gr_len = 441;
  c.pos = 10000;
  c.note = 60;
  c.kind = Fill::Ramp;
  SampleDesc d = desc_over(pcm, c);
  audio::FilterMod lm = len_adsr(0.5f);
  lm.release = 1000;  // ms: a slow release, so each block moves it a little
  VoicePool& p = pool_a();
  p.set_sample(0, d, &lm);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vi = o3os::audio::GranLenW5Test::voice_of(p, 0);
  std::vector<std::int16_t> out;
  render(p, out, 4 * kBlk);
  const float held = o3os::audio::GranLenW5Test::gen_e_out(vi);
  CHECK(std::abs(held - 10419.0f) < 0.5f);  // the sustain
  p.note_off(0);
  render(p, out, 2 * kBlk);  // the voice's own release is 256 samples
  CHECK(p.voices_in_use() == 1u);
  const float after = o3os::audio::GranLenW5Test::gen_e_out(vi);
  CHECK(after < held - 0.5f);  // released: falling
  CHECK(after >= 441.0f);
  p.cut(0);
}

// Review fold: a voice that had the length modulator and is reused by a note of a non-granular instrument carries nothing over.
TEST_CASE("grlen W5: a voice reused by a non-granular note renders as a fresh one") {
  const std::uint32_t L = 20000;
  const std::int16_t* pcm = fill(L, Fill::Wave);
  Cfg c;
  c.len = L;
  c.gr_len = 441;
  c.pos = 10000;
  c.note = 60;
  SampleDesc gd = desc_over(pcm, c);
  const audio::FilterMod full = len_adsr(1.0f);
  SampleDesc pd{pcm, L, 0, 0, 65536};  // plain
  VoicePool& a = pool_a();
  a.set_sample(0, gd, &full);
  REQ(a.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vi = o3os::audio::GranLenW5Test::voice_of(a, 0);
  REQ(o3os::audio::GranLenW5Test::len_on(a, vi));
  std::vector<std::int16_t> out;
  render(a, out, 10 * kBlk);
  a.cut(0);
  render(a, out, 8 * kBlk);  // the cut's residual fades out before the comparison
  a.set_sample(0, pd);
  REQ(a.note_on(0, 60, 0, audio::kVelocityDefault));
  const unsigned vj = o3os::audio::GranLenW5Test::voice_of(a, 0);
  CHECK_FALSE(o3os::audio::GranLenW5Test::len_on(a, vj));
  std::vector<std::int16_t> oa;
  render(a, oa, 20 * kBlk);
  VoicePool& b = pool_b();
  b.set_sample(0, pd);
  REQ(b.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> ob;
  render(b, ob, 20 * kBlk);
  CHECK(oa == ob);
  a.cut(0);
  b.cut(0);
}
