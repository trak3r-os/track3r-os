// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-GRAN: the wavetable voice's windows (audio/voices.h WAVETABLE and POSITION MODULATION): a stack of windows of `wt_size` frames, the window `data` is (wt_pos of wt_windows), the live
// window (inst_param::WtWindow: the render moves the descriptor and the voices that play it, held to the stack), and the rule that the window changes at a block with NO crossfade
// (a hard step at the block, no smoothing while it plays). The same for the live granular position
// (inst_param::GranPos: it takes effect at the grain's next wrap, not at once). The position modulators have their own cases at the end (WT POS, GRAN POS). Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include "support/rng.h"
#include "support/static_slot.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace o3os::audio {

// OS-FLASHDIET T3: drives VoicePool::move_position on a hand-made Voice over a sweep of mod and window counts; no code
struct MovePositionSweepTest {
  using V = VoicePool::Voice;
  static void move(V& v, const SampleDesc& s, float mod) noexcept { VoicePool::move_position(v, s, mod); }
};

}  // namespace o3os::audio


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
namespace ip = audio::inst_param;
using audio::SampleDesc;
using audio::VoicePool;

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr unsigned kWin = 256;      // frames a window
constexpr unsigned kWindows = 8;
std::int16_t g_stack[kWin * kWindows];

VoicePool& pool_a() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

// Window w is the constant (w + 1) x 1000: a window is told by its level.
void fill_stack() {
  for (unsigned w = 0; w < kWindows; ++w) {
    for (unsigned i = 0; i < kWin; ++i) {
      g_stack[w * kWin + i] = static_cast<std::int16_t>((w + 1u) * 1000u);
    }
  }
}

SampleDesc wt_desc(unsigned pos, unsigned windows = kWindows) {
  fill_stack();
  SampleDesc d{g_stack + pos * kWin, kWin, 0, kWin, 65536};
  d.wt_size = kWin;
  d.wt_windows = windows;
  d.wt_pos = pos;
  d.offset = pos * kWin;
  return d;
}

// One block of track 0, its samples appended.
void block(VoicePool& p, std::vector<std::int16_t>& out) {
  p.begin_block();
  p.render(0, kBlk);
  p.end_block();
  const std::int16_t* f = p.feed(0);
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    out.push_back(f != nullptr ? f[i] : std::int16_t{0});
  }
}

}  // namespace

TEST_CASE("wavetable: the descriptor says its window and the voice plays it (a window is told by its level)") {
  for (const unsigned pos : {0u, 3u, 7u}) {
    VoicePool& p = pool_a();
    p.set_sample(0, wt_desc(pos));
    CHECK(p.sample(0).wt_windows == kWindows);
    CHECK(p.sample(0).wt_pos == pos);
    REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
    std::vector<std::int16_t> out;
    for (unsigned b = 0; b < 4; ++b) {
      block(p, out);
    }
    CHECK(std::abs(out[200] - static_cast<int>((pos + 1) * 1000)) <= 1);
  }
}

TEST_CASE("wavetable: set_sample holds the window to the stack and drops the fields of a descriptor that is no wavetable") {
  VoicePool& p = pool_a();
  SampleDesc d = wt_desc(0);
  d.wt_pos = 99;
  p.set_sample(0, d);
  CHECK(p.sample(0).wt_pos == kWindows - 1u);
  d = wt_desc(0);
  d.wt_size = 0;  // a plain sample with the fields left over
  d.wt_pos = 3;
  p.set_sample(0, d);
  CHECK(p.sample(0).wt_windows == 0u);
  CHECK(p.sample(0).wt_pos == 0u);
}

TEST_CASE("wavetable: a live window edit moves the sounding voice to the new window at the next block, with no crossfade, held to the stack") {
  VoicePool& p = pool_a();
  p.set_sample(0, wt_desc(1));
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < 3; ++b) {
    block(p, out);
  }
  CHECK(std::abs(out.back() - 2000) <= 1);
  std::uint32_t pan = 0;
  CHECK(p.apply_param(0, ip::WtWindow, 5.0f, pan));
  CHECK(p.sample(0).wt_pos == 5u);
  CHECK(p.sample(0).data == g_stack + 5 * kWin);
  CHECK(p.sample(0).offset == 5u * kWin);
  const std::size_t from = out.size();
  block(p, out);
  for (std::size_t i = from; i < out.size(); ++i) {
    CHECK(std::abs(out[i] - 6000) <= 1);  // the whole next block is window 5: the step is at the block, nothing between the two levels
  }
  CHECK(p.apply_param(0, ip::WtWindow, 500.0f, pan));  // past the stack: the last window
  CHECK(p.sample(0).wt_pos == kWindows - 1u);
  CHECK(p.apply_param(0, ip::WtWindow, -3.0f, pan));  // a negative one: the first
  CHECK(p.sample(0).wt_pos == 0u);
  CHECK(p.apply_param(0, ip::WtWindow, 2.4f, pan));   // a whole number as the other fields
  CHECK(p.sample(0).wt_pos == 2u);
  block(p, out);
  CHECK(std::abs(out.back() - 3000) <= 1);
}

TEST_CASE("wavetable: the live window of a slot that has none, or of an empty slot, is valid and changes nothing") {
  VoicePool& p = pool_a();
  std::uint32_t pan = 0;
  CHECK(p.apply_param(5, ip::WtWindow, 3.0f, pan));  // empty slot
  SampleDesc plain{g_stack, kWin, 0, 0, 65536};
  p.set_sample(1, plain);
  CHECK(p.apply_param(1, ip::WtWindow, 3.0f, pan));
  CHECK(p.sample(1).data == g_stack);
  SampleDesc fixed = wt_desc(2, 0);  // a wavetable that does not say its windows: the window is fixed
  p.set_sample(2, fixed);
  CHECK(p.apply_param(2, ip::WtWindow, 4.0f, pan));
  CHECK(p.sample(2).wt_pos == 2u);  // where it was
  CHECK(p.sample(2).data == g_stack + 2 * kWin);
  CHECK_FALSE(p.apply_param(0, ip::WtWindow, std::nanf(""), pan));  // not a number: refused
}

TEST_CASE("granular: a live position takes effect at the grain's next wrap, not at once") {
  static std::int16_t ramp[20000];
  for (unsigned i = 0; i < 20000; ++i) {
    ramp[i] = static_cast<std::int16_t>(i);
  }
  VoicePool& p = pool_a();
  SampleDesc d{ramp, 20000, 0, 0, 65536};
  d.granular = true;
  d.gr_len = 200;
  d.gr_pos = 10000;  // len 20000: the grain around sample 3052, ~200 long
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  block(p, out);
  block(p, out);  // 128 samples: inside the first pass of the grain (~200 samples)
  std::uint32_t pan = 0;
  CHECK(p.apply_param(0, ip::GranPos, 60000.0f, pan));
  CHECK(p.sample(0).gr_pos == 60000u);
  const std::int16_t before = out.back();
  for (unsigned b = 0; b < 12; ++b) {
    block(p, out);
  }
  // the first pass goes on at its place (the ramp rises), and after the wrap the grain is far up the ramp
  CHECK(out[130] > before - 3);
  CHECK(out[130] < 4000);
  CHECK(out.back() > 15000);
}

// ---- OS-GRAN: WT POS and GRAN POS (voices.h POSITION MODULATION) -------------------------------------------------------------------------------------------------------------
namespace {

audio::FilterMod adsr_mod(float amount, float sustain, std::uint16_t delay, std::uint16_t attack, std::uint16_t hold, std::uint16_t decay, std::uint16_t release) {
  audio::FilterMod m;
  m.kind = audio::kFmodEnv;
  m.amount = amount;
  m.sustain = sustain;
  m.delay = delay;
  m.attack = attack;
  m.hold = hold;
  m.decay = decay;
  m.release = release;
  return m;
}

audio::FilterMod lfo_mod(std::uint8_t shape, std::uint8_t rate, float amount) {
  audio::FilterMod m;
  m.kind = audio::kFmodLfo;
  m.shape = shape;
  m.rate = rate;
  m.amount = amount;
  m.lfo_amount = amount;
  return m;
}

// The window a block plays, from its last sample's level ((w + 1) x 1000).
int window_of(const std::vector<std::int16_t>& out, std::size_t block_index) { return (out[block_index * kBlk + kBlk - 1] + 500) / 1000 - 1; }

constexpr double kSamplesPerMs = static_cast<double>(haudio::sample_rate_mhz()) * 1e-6;

}  // namespace

TEST_CASE("wt pos: an ADSR moves the window by output x windows, cut toward zero, once a block, from the instrument's window, held to the stack") {
  VoicePool& p = pool_a();
  SampleDesc d = wt_desc(1);
  d.pos_mod = adsr_mod(0.5f, 1.0f, 0, 40, 0, 0, 100);  // 0 -> +0.5 over 40 ms, then held: window 1 + 4 = 5
  p.set_sample(0, d);
  CHECK(p.sample(0).pos_mod.kind == audio::kFmodEnv);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  const unsigned blocks = 60;  // 60 x 64 samples = 87 ms
  for (unsigned b = 0; b < blocks; ++b) {
    block(p, out);
  }
  int worst = 0;
  for (unsigned b = 1; b < blocks; ++b) {
    const double t = static_cast<double>(b * kBlk);  // the generator is evaluated at the block's first sample
    const double level = std::min(t / (40.0 * kSamplesPerMs), 1.0) * 0.5;
    const int want = std::min(1 + static_cast<int>(level * 8.0), 7);
    worst = std::max(worst, std::abs(window_of(out, b) - want));
  }
  CHECK_MESSAGE(worst <= 1, "worst window difference " << worst);
  CHECK(window_of(out, blocks - 1) == 5);
  CHECK(window_of(out, 3) < window_of(out, 30));  // it moves
}

TEST_CASE("wt pos: a negative amount and a big one are held to the first and the last window; no modulator, no movement") {
  VoicePool& p = pool_a();
  SampleDesc d = wt_desc(3);
  d.pos_mod = adsr_mod(-1.0f, 1.0f, 0, 1, 0, 0, 10);
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < 12; ++b) {
    block(p, out);
  }
  CHECK(window_of(out, 11) == 0);  // 3 - 8, held
  d.pos_mod = adsr_mod(1.0f, 1.0f, 0, 1, 0, 0, 10);
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  out.clear();
  for (unsigned b = 0; b < 12; ++b) {
    block(p, out);
  }
  CHECK(window_of(out, 11) == 7);  // 3 + 8, held
  d.pos_mod = audio::FilterMod{};
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  out.clear();
  for (unsigned b = 0; b < 12; ++b) {
    block(p, out);
  }
  CHECK(window_of(out, 11) == 3);
}

TEST_CASE("wt pos: an LFO swings the window around the instrument's, and the descriptor drops a modulator that has no window to move") {
  VoicePool& p = pool_a();
  SampleDesc d = wt_desc(4);
  d.pos_mod = lfo_mod(3, 17, 0.5f);  // square, +-0.5 x 8 windows: 8 or 0, held to 7 and 0
  p.set_sample(0, d);
  p.restart_clock();
  p.on_step(0);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < 80; ++b) {
    block(p, out);
  }
  bool low = false, high = false;
  unsigned other = 0;
  for (unsigned b = 1; b < 80; ++b) {
    const int w = window_of(out, b);
    low = low || w == 0;
    high = high || w == 7;
    other += (w != 0 && w != 7) ? 1u : 0u;
  }
  CHECK(low);
  CHECK(high);
  CHECK(other == 0u);
  SampleDesc one = wt_desc(0, 1);  // a stack of one window: nothing to move
  one.pos_mod = adsr_mod(1.0f, 1.0f, 0, 1, 0, 0, 10);
  p.set_sample(1, one);
  CHECK(p.sample(1).pos_mod.kind == audio::kFmodNone);
  SampleDesc plain{g_stack, kWin, 0, kWin, 65536};
  plain.pos_mod = adsr_mod(1.0f, 1.0f, 0, 1, 0, 0, 10);
  p.set_sample(2, plain);
  CHECK(p.sample(2).pos_mod.kind == audio::kFmodNone);
}

TEST_CASE("wt pos: switching the modulator off live puts the voice back on the instrument's window") {
  VoicePool& p = pool_a();
  SampleDesc d = wt_desc(2);
  d.pos_mod = adsr_mod(1.0f, 1.0f, 0, 1, 0, 0, 10);
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < 6; ++b) {
    block(p, out);
  }
  CHECK(window_of(out, 5) == 7);
  std::uint32_t pan = 0;
  CHECK(p.apply_param(0, ip::envelope(3, ip::EnvEnable), 0.0f, pan));
  CHECK(p.sample(0).pos_mod.kind == audio::kFmodNone);
  block(p, out);
  CHECK(window_of(out, 6) == 2);
}

TEST_CASE("gran pos: an ADSR moves the grain's position by output x 65535 (the grain follows at its next wrap), held to 0..65535") {
  static std::int16_t ramp[20000];
  for (unsigned i = 0; i < 20000; ++i) {
    ramp[i] = static_cast<std::int16_t>(i);
  }
  VoicePool& p = pool_a();
  SampleDesc d{ramp, 20000, 0, 0, 65536};
  d.granular = true;
  d.gr_len = 200;
  d.gr_pos = 10000;
  d.pos_mod = adsr_mod(0.5f, 1.0f, 0, 5, 0, 0, 10);  // +0.5 x 65535 within 5 ms: position 10000 + 32767 = 42767 -> sample 13052
  p.set_sample(0, d);
  CHECK(p.sample(0).pos_mod.kind == audio::kFmodEnv);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  std::vector<std::int16_t> out;
  for (unsigned b = 0; b < 40; ++b) {
    block(p, out);
  }
  // the grain is ~200 samples long: the ramp's value is the position; after the first wraps it sits around 13052 (a grain wide)
  CHECK(out[kBlk * 2 + 20] < 4500);  // still the first grain, around sample 3052
  std::int32_t lo = 32767, hi = 0;
  for (std::size_t i = out.size() - 2 * kBlk; i < out.size(); ++i) {
    lo = std::min<std::int32_t>(lo, out[i]);
    hi = std::max<std::int32_t>(hi, out[i]);
  }
  CHECK(lo > 12900);
  CHECK(hi < 13250);
  // a negative amount is held at 0: the grain sits at the start of the sample
  d.pos_mod = adsr_mod(-1.0f, 1.0f, 0, 1, 0, 0, 10);
  p.set_sample(0, d);
  REQ(p.note_on(0, 60, 0, audio::kVelocityDefault));
  out.clear();
  for (unsigned b = 0; b < 40; ++b) {
    block(p, out);
  }
  hi = 0;
  for (std::size_t i = out.size() - 2 * kBlk; i < out.size(); ++i) {
    hi = std::max<std::int32_t>(hi, out[i]);
  }
  CHECK(hi < 120);
}

// ---- OS-FLASHDIET T3: move_position clamps the float product to +-2 x wt_windows and converts through int32 (no float -> int64, so no __aeabi_f2lz and its
// double helpers in the image). The window it picks must equal the old expression's for every finite mod and every window count.
namespace {

// The window the expression before T3 picked: wt_pos + (int64)(mod * wt_windows), cut toward zero and held to [0, wt_windows - 1]. The float -> int64 conversion is
// modelled with the saturation of the unit's helper (__aeabi_f2lz) and the sum without overflow, so the [0, last] clamp is what decides the out-of-range products too.
std::int64_t old_window(std::uint32_t wt_pos, std::uint32_t windows, float mod) {
  const float prod = mod * static_cast<float>(windows);
  std::int64_t t;
  if (prod >= 9223372036854775808.0f) {  // >= 2^63, +inf included
    t = INT64_MAX;
  } else if (prod <= -9223372036854775808.0f) {  // <= -2^63, -inf included
    t = INT64_MIN;
  } else {
    t = static_cast<std::int64_t>(prod);  // cut toward zero, defined in this range
  }
  const std::int64_t last = static_cast<std::int64_t>(windows) - 1;
  const std::int64_t pos = static_cast<std::int64_t>(wt_pos);  // <= last in the sweep
  if (t >= 0) {
    return t > last - pos ? last : pos + t;
  }
  return t < -pos ? 0 : pos + t;
}

}  // namespace

TEST_CASE("wt pos (OS-FLASHDIET T3): the window move_position picks equals the old expression's over a sweep of mod and window counts") {
  using audio::MovePositionSweepTest;
  constexpr std::uint32_t kMaxW = 1u << 19;  // wt_windows is frames / wt_size <= 2^27 on the unit; the sweep's stack holds one frame a window
  static std::int16_t stack[kMaxW + 1];
  const float mods[] = {
      0.0f, -0.0f, 0.25f, -0.25f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f, 2.0f, -2.0f, 2.5f, -2.5f, 3.0f, -3.0f, 100.0f, -100.0f,
      1e6f, -1e6f, 1e10f, -1e10f, 1e18f, -1e18f, 1e30f, -1e30f, FLT_MAX, -FLT_MAX, std::nextafter(FLT_MAX, 0.0f), -std::nextafter(FLT_MAX, 0.0f),
      std::nextafter(2.0f, 0.0f), std::nextafter(2.0f, 3.0f), std::nextafter(-2.0f, 0.0f), std::nextafter(-2.0f, -3.0f),
  };
  o3os::test::Rng rng(0x5144u);
  std::uint64_t bad = 0;
  std::uint64_t cases = 0;
  const auto check = [&](std::uint32_t w, std::uint32_t wt_pos, float mod) {
    SampleDesc s{};
    s.data = stack + wt_pos;  // window wt_pos of the stack, wt_size 1
    s.wt_size = 1;
    s.wt_windows = w;
    s.wt_pos = wt_pos;
    MovePositionSweepTest::V v{};  // mode 0: not granular
    v.data = s.data;
    MovePositionSweepTest::move(v, s, mod);
    const std::int64_t want = w > 1u ? old_window(wt_pos, w, mod) : static_cast<std::int64_t>(wt_pos);  // one window: nothing moves
    const std::int64_t got = (v.data - s.data) + static_cast<std::int64_t>(wt_pos);
    ++cases;
    if (got != want) {
      if (bad == 0u) {
        MESSAGE("mismatch: w=" << w << " wt_pos=" << wt_pos << " mod=" << mod << " got=" << got << " want=" << want);
      }
      ++bad;
    }
  };
  const std::uint32_t window_counts[] = {2u, 3u, 4u, 5u, 8u, 16u, 17u, 63u, 64u, 65u, 100u, 255u, 256u, 257u, 1000u, 4096u, 32768u, 65536u, kMaxW};
  for (unsigned wi = 0; wi < 512u; ++wi) {  // every window count 1..512
    const std::uint32_t w = wi + 1u;
    const std::uint32_t wt_positions[] = {0u, w / 2u, w - 1u};
    for (const std::uint32_t wt_pos : wt_positions) {
      for (const float mod : mods) {
        check(w, wt_pos, mod);
      }
      // boundary products: mod x windows landing on the integers around 0, +-2 x windows and the clamp's ends
      const std::int64_t ks[] = {-2 * (std::int64_t)w - 1, -2 * (std::int64_t)w, -2 * (std::int64_t)w + 1, -(std::int64_t)w, -(std::int64_t)w + 1, -1, 0, 1,
                                 (std::int64_t)w - 1, (std::int64_t)w, (std::int64_t)w + 1, 2 * (std::int64_t)w - 1, 2 * (std::int64_t)w, 2 * (std::int64_t)w + 1,
                                 (std::int64_t)w - 1 - (std::int64_t)wt_pos, (std::int64_t)w - (std::int64_t)wt_pos, -(std::int64_t)wt_pos - 1, -(std::int64_t)wt_pos, -(std::int64_t)wt_pos + 1};
      for (const std::int64_t k : ks) {
        check(w, wt_pos, static_cast<float>(k) / static_cast<float>(w));
      }
      for (unsigned r = 0; r < 64u; ++r) {  // random finite floats of every magnitude
        std::uint32_t bits = rng.next();
        float f;
        std::memcpy(&f, &bits, 4);
        if (std::isnan(f) || std::isinf(f)) {
          continue;
        }
        check(w, wt_pos, f);
      }
    }
  }
  for (const std::uint32_t w : window_counts) {  // the larger window counts, same sweep
    if (w <= 512u) {
      continue;
    }
    const std::uint32_t wt_positions[] = {0u, w / 2u, w - 1u};
    for (const std::uint32_t wt_pos : wt_positions) {
      for (const float mod : mods) {
        check(w, wt_pos, mod);
      }
      const std::int64_t ks[] = {-2 * (std::int64_t)w, -(std::int64_t)w, -1, 0, 1, (std::int64_t)w, 2 * (std::int64_t)w, (std::int64_t)w - 1 - (std::int64_t)wt_pos, -(std::int64_t)wt_pos};
      for (const std::int64_t k : ks) {
        check(w, wt_pos, static_cast<float>(k) / static_cast<float>(w));
      }
      for (unsigned r = 0; r < 256u; ++r) {
        std::uint32_t bits = rng.next();
        float f;
        std::memcpy(&f, &bits, 4);
        if (std::isnan(f) || std::isinf(f)) {
          continue;
        }
        check(w, wt_pos, f);
      }
    }
  }
  MESSAGE("cases=" << cases << " mismatches=" << bad);
  CHECK(bad == 0u);
}
