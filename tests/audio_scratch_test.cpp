// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-DJ C1: the master scratch stage (audio/master_scratch.h) with a SCRIPTED encoder count (the platform pointer): a still platter is silence, one turn is 1.8 s of audio, a reversal plays backwards, the head
// never passes the write head nor reads what was not written, the backspin brakes in 0.5 s, TAPE STOP reaches 0 in one beat and the release is the live mix, no click at the entry or the release, and the
// ring's owner rule (stage / busy). The stage in the graph (the slot, the ParamChange, the export) is audio_perform_test.cpp's.
#include <doctest/doctest.h>

#include <o3os/audio/master_scratch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using o3os::audio::MasterScratch;

namespace {

constexpr std::uint32_t kB = MasterScratch::kBlock;
constexpr std::uint32_t kRing = 110250;  // 2.5 s
std::uint16_t g_count = 0;
std::uint16_t scripted() noexcept { return g_count; }
std::int16_t g_ring[2 * kRing];
constexpr std::uint32_t kBig = MasterScratch::kMaxFrames;  // 4 s: the largest ring
std::int16_t g_big[2 * kBig];
constexpr std::uint32_t kSmall = MasterScratch::kMinFrames;  // 0.1 s: the smallest ring, so that a few blocks wrap it
constexpr std::uint32_t kGuard = 32;                          // frames of sentinel on each side of the small ring
std::int16_t g_guarded[2 * (kSmall + 2 * kGuard)];

struct Rig {
  MasterScratch s;
  bool bypass = true;
  std::uint32_t n = 0;  // frames run
  std::int16_t* ring = g_ring;
  std::uint32_t frames = kRing;
  explicit Rig(std::int16_t* r = g_ring, std::uint32_t f = kRing) : ring(r), frames(f) {
    for (std::uint32_t i = 0; i < 2u * f; ++i) r[i] = 32767;  // anything the stage reads without having written shows as full scale
    g_count = 0;
    s.attach(&bypass);
    s.set_counter(&scripted);
  }
  bool arm(float mode, std::uint32_t ramp = 0) {
    CHECK(s.stage(ring, frames));
    return s.set_mode(mode, ramp);
  }
  // One block of `in(n)`, the hand moving `counts` before it; the output is appended to `out`.
  template <class Sig>
  void block(int counts, Sig in, std::vector<float>& out) {
    g_count = static_cast<std::uint16_t>(g_count + counts);
    float l[kB], r[kB];
    for (std::uint32_t i = 0; i < kB; ++i) l[i] = r[i] = in(n + i);
    s.process(l, r, kB);
    for (std::uint32_t i = 0; i < kB; ++i) out.push_back(l[i]);
    n += kB;
  }
  template <class Sig>
  void run(unsigned blocks, int counts, Sig in, std::vector<float>& out) {
    for (unsigned b = 0; b < blocks; ++b) block(counts, in, out);
  }
  float head() const { return static_cast<float>(n) - s.lag(); }  // the head's absolute position, in frames
};

// A small ring wrapped in sentinel frames: a write or a read outside it shows.
struct SmallRig : Rig {
  SmallRig() : Rig(g_guarded + 2u * kGuard, kSmall) {
    for (std::uint32_t i = 0; i < 2u * kGuard; ++i) {
      g_guarded[i] = 32767;
      g_guarded[2u * (kGuard + kSmall) + i] = 32767;
    }
  }
  bool guards_intact() const {
    for (std::uint32_t i = 0; i < 2u * kGuard; ++i) {
      if (g_guarded[i] != 32767 || g_guarded[2u * (kGuard + kSmall) + i] != 32767) return false;
    }
    return true;
  }
};

float ramp(std::uint32_t n) { return 0.9f * static_cast<float>(n % kRing) / static_cast<float>(kRing); }
float climb(std::uint32_t n) { return 5e-5f * static_cast<float>(n); }  // strictly rising for 40,000 frames: a head that moves forwards reads a non-decreasing value
float sine(std::uint32_t n) { return 0.5f * std::sin(6.2831853f * 1000.0f * static_cast<float>(n) / 44117.65f); }
}  // namespace

TEST_CASE("scratch: a still platter is silence, the entry is the live mix running down") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  CHECK_FALSE(t.bypass);
  t.run(400, 0, sine, out);  // (OS-SCRATCH: the entry rate 1 runs down over the 70 ms inertia: under 0.001 after about 340 blocks)
  for (std::size_t i = 400u * kB - 64u; i < out.size(); ++i) CHECK(out[i] == 0.0f);  // exactly silent
  CHECK(t.s.rate() == 0.0f);
  CHECK(t.s.lag() > 3000.0f);  // the sequencer ran underneath: the head fell behind
  // the first block is the live mix itself (rate 1 at the entry), then it falls away
  CHECK(std::fabs(out[5] - sine(5)) < 0.01f);
}

TEST_CASE("scratch: one turn of the platter is 1.8 s of audio") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(1500, 0, ramp, out);  // 2.2 s of history, the platter held
  const float p0 = t.head();
  t.run(24, 4, ramp, out);  // 96 counts, spread over 24 blocks
  t.run(500, 0, ramp, out);  // let the inertia (70 ms) and the position debt settle
  const float turned = t.head() - p0;
  CHECK(std::fabs(turned - 1.8f * 44117.65f) < 1.5f * MasterScratch::kFramesPerCount);  // OS-SCRATCH: the head follows the hand to within one and a half counts (the velocity is an estimate)
  CHECK(t.s.rate() == 0.0f);
}

TEST_CASE("scratch: a reversal plays backwards") {
  Rig t;
  std::vector<float> fwd, back;
  CHECK(t.arm(1.0f));
  std::vector<float> warm;
  t.run(1000, 0, ramp, warm);
  t.run(40, 1, ramp, fwd);    // one count a block forwards: the ramp rises
  t.run(40, -1, ramp, back);  // then backwards: the ramp falls
  float up = 0.0f, down = 0.0f;
  for (std::size_t i = 20u * kB; i + 1 < fwd.size(); ++i) up += fwd[i + 1] - fwd[i];
  for (std::size_t i = 20u * kB; i + 1 < back.size(); ++i) down += back[i + 1] - back[i];
  CHECK(up > 0.0f);
  CHECK(down < 0.0f);
  CHECK(std::fabs(down) > 1e-3f);
}

TEST_CASE("scratch: the head never passes the write head and never reads what was not written") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.s.set_tuning(70.0f, 1.0f, 32.0f, false);  // OS-BUG-SCRATCH: the head's frames are read plain here (the anti-alias filter has its own tests)
  t.run(400, 0, ramp, out);
  out.clear();
  t.run(100, 16, ramp, out);  // far faster than real time forwards: the head sticks to the write head
  CHECK(t.s.lag() == 0.0f);
  for (std::size_t i = out.size() - 64u; i < out.size(); ++i) CHECK(std::fabs(out[i] - ramp(t.n - static_cast<std::uint32_t>(out.size() - i))) < 2e-4f);
  out.clear();
  t.run(400, -16, ramp, out);  // far faster than the history backwards: the head sticks to the oldest frame written
  CHECK(t.s.lag() <= static_cast<float>(t.n));
  for (float v : out) CHECK(std::fabs(v) <= 0.9f + 1e-3f);  // never the full-scale fill of the unwritten ring
}

TEST_CASE("scratch: a fast backward turn before the release spins back and brakes in 0.5 s; a slow one releases") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(1200, 0, ramp, out);
  t.run(30, -2, ramp, out);  // rate about -26
  const float r = t.s.rate();
  CHECK(r < -MasterScratch::kSpinMin);
  CHECK(t.s.set_mode(0.0f, 0));
  t.run(1, 0, ramp, out);
  CHECK(t.s.phase() == MasterScratch::Phase::Spin);
  CHECK(t.s.rate() < -MasterScratch::kSpinMin);  // it keeps spinning
  t.run(171, 0, ramp, out);                       // about 0.25 s in
  CHECK(t.s.rate() == doctest::Approx(r * 0.5f).epsilon(0.1));
  t.run(180, 0, ramp, out);  // past 0.5 s
  CHECK(t.s.phase() != MasterScratch::Phase::Spin);
  t.run(8, 0, ramp, out);
  CHECK(t.s.idle());
  CHECK(t.bypass);
  CHECK_FALSE(t.s.busy());
  // a slow turn: no spin
  Rig u;
  CHECK(u.arm(1.0f));
  u.run(1200, 0, ramp, out);
  u.run(30, -1, ramp, out);
  u.run(30, 0, ramp, out);  // (rate back to 0, but the window of 30 ms has passed)
  CHECK(u.s.set_mode(0.0f, 0));
  u.run(1, 0, ramp, out);
  CHECK(u.s.phase() == MasterScratch::Phase::Release);
}

TEST_CASE("scratch: TAPE STOP reaches 0 over the beat, holds silent and is live again at the release") {
  Rig t;
  std::vector<float> out;
  const std::uint32_t beat = 22058;  // 120 BPM
  CHECK(t.arm(2.0f, beat));
  t.run(172, 0, sine, out);  // half the beat
  CHECK(t.s.rate() == doctest::Approx(0.5f).epsilon(0.02));
  t.run(180, 0, sine, out);  // past the beat
  CHECK(t.s.rate() == 0.0f);
  CHECK(t.s.lag() == doctest::Approx(static_cast<float>(beat) / 2.0f + static_cast<float>(t.n - beat)).epsilon(0.005));  // a constant brake: half a beat behind at the stop, and the sequencer ran on underneath since (slip)
  out.clear();
  t.run(20, 0, sine, out);
  for (float v : out) CHECK(v == 0.0f);
  CHECK(t.s.set_mode(0.0f, 0));
  out.clear();
  t.run(6, 0, sine, out);
  CHECK(t.s.idle());
  CHECK(t.bypass);
  CHECK(std::fabs(out[5 * kB + 63] - sine(t.n - 1u)) < 1e-6f);  // live again, to the bit
}

TEST_CASE("scratch: OS-DJSTOPLEN mode 3 rebases the TAPE STOP onto a new length and keeps the rate") {
  Rig t;
  std::vector<float> out;
  const std::uint32_t beat = 22058;
  CHECK_FALSE(t.s.set_mode(3.0f, beat));  // nothing runs yet
  CHECK(t.arm(2.0f, beat));
  t.run(172, 0, sine, out);  // half the beat
  const float half = t.s.rate();
  CHECK(half == doctest::Approx(0.5f).epsilon(0.02));
  CHECK_FALSE(t.s.set_mode(3.0f, 0u));  // a zero length is refused
  CHECK(t.s.set_mode(3.0f, 4u * beat));
  t.run(1, 0, sine, out);
  CHECK(t.s.rate() == doctest::Approx(half).epsilon(0.02));  // no jump
  t.run(172, 0, sine, out);  // the rest of the brake now runs over 4 beats: the rate falls 0.5 / (0.5 x 4 beats) a sample
  CHECK(t.s.rate() == doctest::Approx(half - 0.5f * 173.0f * 64.0f / (2.0f * static_cast<float>(beat))).epsilon(0.03));
  CHECK(t.s.set_mode(3.0f, beat / 8u));  // shorter again: the rate is still where it was, and the stop comes soon
  t.run(1, 0, sine, out);
  CHECK(t.s.rate() > 0.0f);
  t.run(30, 0, sine, out);
  CHECK(t.s.rate() == 0.0f);
  CHECK(t.s.set_mode(0.0f, 0));
  t.run(6, 0, sine, out);
  CHECK(t.s.idle());
  CHECK_FALSE(t.s.set_mode(3.0f, beat));  // and refused once the stage is off
}

TEST_CASE("scratch: no click at the entry nor at the release") {
  Rig t;
  std::vector<float> out;
  const float own = 0.5f * 6.2831853f * 1000.0f / 44117.65f;  // the sine's own largest step
  CHECK(t.arm(1.0f));
  t.run(40, 0, sine, out);  // the live mix runs down to the still hand
  float worst = 0.0f;
  for (std::size_t i = 1; i < out.size(); ++i) worst = std::fmax(worst, std::fabs(out[i] - out[i - 1]));
  CHECK(worst <= own * 1.05f);
  out.clear();
  t.run(20, 0, sine, out);
  CHECK(t.s.set_mode(0.0f, 0));
  out.clear();
  t.run(8, 0, sine, out);
  worst = 0.0f;
  for (std::size_t i = 1; i < out.size(); ++i) worst = std::fmax(worst, std::fabs(out[i] - out[i - 1]));
  CHECK(worst <= own * 1.05f + 0.5f / 256.0f);
}

TEST_CASE("scratch: the owner rule, the refusals and the abort") {
  Rig t;
  std::vector<float> out;
  CHECK_FALSE(t.s.set_mode(1.0f, 0));  // no ring staged
  CHECK_FALSE(t.s.set_mode(3.0f, 0));
  CHECK_FALSE(t.s.stage(g_ring, 100));  // too small
  CHECK(t.s.stage(g_ring, kRing));
  CHECK(t.s.busy());
  CHECK_FALSE(t.s.stage(g_ring, kRing));  // staged and not let go: the second asker is refused
  t.s.unstage();                          // the push of the mode was refused: the claim is withdrawn
  CHECK_FALSE(t.s.busy());
  CHECK(t.arm(1.0f));
  t.run(10, 0, sine, out);
  CHECK(t.s.busy());
  CHECK_FALSE(t.s.set_mode(1.0f, 0));  // already running
  t.s.abort();                          // the export
  CHECK(t.s.idle());
  CHECK(t.bypass);
  CHECK_FALSE(t.s.busy());
}

// ---- OS-DJ C1 review fold ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("scratch: a full ring stops the lag a block short, so a resume reads old frames and never the block just written") {
  SmallRig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(3u * kSmall / kB, 0, climb, out);  // a still hand for three rings: the ring wraps twice and the lag runs to its stop
  CHECK(t.s.lag_frames() == static_cast<std::int32_t>(kSmall - kB - 2u));  // (0ad289b: kSmall - 2, which is inside the 64 frames the block overwrites)
  CHECK(t.s.lag_fraction() == 0.0f);
  const std::uint32_t n0 = t.n;
  out.clear();
  t.run(30, 1, climb, out);  // the hand turns on: the rate rises from 0 within a block, and the head sweeps up from the oldest frame
  // The first block reads the oldest frames, not the live ones: 0ad289b read frame n + 2 (the block's own, 63 ahead) for the first 18 frames, then fell to the old frames, a step of 0.65.
  const float oldest = climb(n0 - static_cast<std::uint32_t>(kSmall - kB - 2u) + 2u * kB);
  for (std::uint32_t i = 0; i < kB; ++i) CHECK(out[i] <= oldest);
  // Rising ramp, forward head, gate rising: the output never steps back.
  for (std::size_t i = 1; i < out.size(); ++i) CHECK(out[i] >= out[i - 1] - 1e-6f);
  CHECK(out.back() <= climb(t.n));  // and never ahead of the live frame
  CHECK(t.guards_intact());
}

TEST_CASE("scratch: a small ring wraps its write and read indexes; TAPE at rate 1 is the live mix, three rings long") {
  SmallRig t;
  std::vector<float> out;
  CHECK(t.arm(2.0f, 0xF0000000u));  // a ramp of 97 hours: the rate stays 1.0 (float), the lag 0
  t.run(3u * kSmall / kB + 3u, 0, sine, out);  // (the write index passes the ring's end three times; the read index with it, so j == 0 is read with k = the last slot)
  for (std::size_t i = 0; i < out.size(); ++i) CHECK(std::fabs(out[i] - sine(static_cast<std::uint32_t>(i))) < 2e-3f);  // (the ramp's slow fall leaves a lag of 0.03 frame at the end: that much interpolation, and the 16-bit step)
  CHECK(t.s.lag_frames() == 0);
  CHECK(t.guards_intact());
}

TEST_CASE("scratch: a second stage() starts fresh - the epoch, the lag, the ring and the size are the new run's") {
  Rig a;
  std::vector<float> out;
  CHECK(a.arm(1.0f));
  a.run(300, 0, sine, out);
  CHECK(a.s.lag_frames() > 10000);
  CHECK(a.s.set_mode(0.0f, 0));
  a.run(8, 0, sine, out);
  CHECK(a.s.idle());
  CHECK_FALSE(a.s.busy());
  // The same stage again, on a smaller ring of another buffer.
  CHECK(a.s.stage(g_guarded + 2u * kGuard, kSmall));
  CHECK(a.s.busy());
  CHECK(a.s.set_mode(1.0f, 0));
  CHECK(a.s.lag_frames() == 0);
  CHECK(a.s.rate() == 1.0f);
  out.clear();
  a.n = 0;
  a.run(2, 0, sine, out);  // the entry is the live mix again; the first run's frames are nowhere in it
  CHECK(std::fabs(out[5] - sine(5)) < 0.01f);
  a.run(150, 0, sine, out);
  CHECK(a.s.lag_frames() == static_cast<std::int32_t>(kSmall - kB - 2u));  // the new ring's stop, not the old one's
  CHECK(a.s.set_mode(0.0f, 0));
  a.run(8, 0, sine, out);
  CHECK(a.s.idle());
  CHECK_FALSE(a.s.busy());
  CHECK(a.s.stage(g_ring, kRing));  // and a third time
  a.s.unstage();
  CHECK_FALSE(a.s.busy());
}

TEST_CASE("scratch: the ring holds the mix at half scale - a hot sum of 1.5 is not clipped before the limiter") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(2.0f, 0xF0000000u));
  const auto hot = [](std::uint32_t n) { return 3.0f * sine(n); };  // +-1.5
  t.run(40, 0, hot, out);
  float worst = 0.0f;
  for (std::size_t i = 0; i < out.size(); ++i) worst = std::fmax(worst, std::fabs(out[i] - hot(static_cast<std::uint32_t>(i))));
  CHECK(worst < 1e-3f);  // (0ad289b stored +-1.0 and clipped it: an error of 0.5)
  const auto peak = *std::max_element(out.begin(), out.end());
  CHECK(peak > 1.4f);
  // and the ring's ends: +-2.0 is the largest mix it keeps whole, more is held there
  Rig u;
  out.clear();
  CHECK(u.arm(2.0f, 0xF0000000u));
  u.run(2, 0, [](std::uint32_t) { return 3.0f; }, out);
  CHECK(out.back() == doctest::Approx(2.0f).epsilon(1e-4));
}

TEST_CASE("scratch: the lag keeps its fraction - a long hold, then a rate near 1 for 10 s, gains the frames the rate says") {
  // The arithmetic: a float lag of 176k frames resolves 0.016 and swallows an increment of 0.003 (a rate of 0.997); the whole + fraction does not.
  float as_float = 176334.0f;
  std::int32_t whole = 176334;
  float frac = 0.0f;
  for (int i = 0; i < 441000; ++i) {
    as_float += 0.003f;
    MasterScratch::advance_lag(whole, frac, 0.003f);
  }
  CHECK(as_float == 176334.0f);  // (0ad289b: the head would have played at exactly 1.000 for the whole 10 s, 0.3 % off)
  CHECK(std::fabs(static_cast<double>(whole) + static_cast<double>(frac) - (176334.0 + 1323.0)) < 0.1);
  whole = 5;
  frac = 0.5f;
  MasterScratch::advance_lag(whole, frac, -6.0f);  // held at 0 from below
  CHECK(whole == 0);
  CHECK(frac == 0.0f);
  MasterScratch::advance_lag(whole, frac, -0.25f);
  CHECK(whole == 0);
  CHECK(frac == 0.0f);
  MasterScratch::advance_lag(whole, frac, 3.75f);
  CHECK(whole == 3);
  CHECK(frac == 0.75f);
  MasterScratch::advance_lag(whole, frac, -0.875f);
  CHECK(whole == 2);
  CHECK(frac == 0.875f);

  // Through the stage: a still hand for 3.3 s (the lag passes 131,072, where a float resolves 1/64), then one count every 13 blocks (a mean rate of 0.994, a ripple between 0 and 3.6) for 10 s.
  Rig t(g_big, kBig);
  std::vector<float> sink;
  CHECK(t.arm(1.0f));
  for (unsigned b = 0; b < 2300; ++b) {
    float l[kB] = {}, r[kB] = {};
    t.s.process(l, r, kB);
    t.n += kB;
  }
  CHECK(t.s.lag_frames() > 140000);
  double lag0 = static_cast<double>(t.s.lag_frames()) + static_cast<double>(t.s.lag_fraction());
  double expected = 0.0;
  for (unsigned b = 0; b < 6891; ++b) {
    const float r0 = t.s.rate();
    float l[kB] = {}, r[kB] = {};
    g_count = static_cast<std::uint16_t>(g_count + (b % 13u == 0u ? 1u : 0u));
    t.s.process(l, r, kB);
    t.n += kB;
    expected += static_cast<double>(kB) * (1.0 - 0.5 * (static_cast<double>(r0) + static_cast<double>(t.s.rate())));
  }
  const double lag1 = static_cast<double>(t.s.lag_frames()) + static_cast<double>(t.s.lag_fraction());
  CHECK(std::fabs(lag1 - lag0 - expected) < 4.0);  // (frames; 0.1 % of the 441,000 would be 441)
}

// ---- OS-SCRATCH: the velocity from the time between counts, the 70 ms inertia (OS-BUG-SCRATCH), the Hermite read ----

namespace {
// One block with `counts` already added to the scripted counter, nothing played: returns the rate at its end.
float step_rate(Rig& t, int counts) {
  g_count = static_cast<std::uint16_t>(g_count + counts);
  float l[kB] = {}, r[kB] = {};
  t.s.process(l, r, kB);
  t.n += kB;
  return t.s.rate();
}
// A scratch ready for a rate test: armed, the entry (rate 1) run down to the still hand.
void settle_still(Rig& t) {
  CHECK(t.arm(1.0f));
  for (unsigned b = 0; b < 400; ++b) step_rate(t, 0);  // (the 70 ms inertia: the entry rate 1 is under 0.001 after about 340 blocks)
  CHECK(t.s.rate() == 0.0f);
}
float lin(std::uint32_t n) { return 2e-5f * static_cast<float>(n); }  // rising for 83,000 frames up to 1.66 (the ring keeps +-2.0)
}  // namespace

TEST_CASE("scratch: the platter's rate follows a steady hand by a one-pole of 70 ms") {
  Rig t;
  settle_still(t);
  // 2 counts every block from now on: the target is 2 x kCountRate once two events are seen (block 1 is a lone event: two counts in 8 blocks).
  const float target = 2.0f * MasterScratch::kCountRate;
  float peak = 0.0f;
  unsigned b63 = 0;
  float r = 0.0f;
  std::vector<float> trace;
  for (unsigned b = 1; b <= 400; ++b) {
    r = step_rate(t, 2);
    trace.push_back(r);
    if (b63 == 0 && r >= 0.632f * target) b63 = b;
    peak = std::fmax(peak, r);
  }
  // 70 ms is 48.2 blocks; the first block's target is only a quarter count a block, so the 63 % point is a block or two later
  CHECK(b63 >= 48);
  CHECK(b63 <= 54);
  CHECK(r == doctest::Approx(target).epsilon(0.01));  // settled to 1 % after 7 time constants
  CHECK(peak <= target * 1.001f);                      // no overshoot
  for (std::size_t i = 1; i < trace.size(); ++i) CHECK(trace[i] >= trace[i - 1] - 1e-4f);  // monotonic
  // the step of one block is the one-pole's own: (target - r) x kInertia
  CHECK(trace[120] - trace[119] == doctest::Approx((target - trace[119]) * MasterScratch::kInertia).epsilon(0.02));
}

TEST_CASE("scratch: a slow turn (one count every 14 blocks) is a steady rate, not pulses; a stop is a curve") {
  Rig t;
  settle_still(t);
  float lo = 1e9f, hi = -1e9f;
  float r = 0.0f;
  for (unsigned b = 1; b <= 1000; ++b) {
    r = step_rate(t, b % 14u == 0u ? 1 : 0);
    if (b > 600) {
      lo = std::fmin(lo, r);
      hi = std::fmax(hi, r);
    }
  }
  const float mean = MasterScratch::kCountRate / 14.0f;  // 0.92
  CHECK(lo > 0.8f * mean);
  CHECK(hi < 1.2f * mean);  // (C1: 12.9 for one block in 14 and 0 between)
  // the hand stops: the rate falls without a step and ends exactly 0
  float worst = 0.0f;
  float prev = r;
  for (unsigned b = 1; b <= 800; ++b) {
    r = step_rate(t, 0);
    worst = std::fmax(worst, std::fabs(r - prev));
    CHECK(r <= prev + 1e-6f);  // never speeds up again
    prev = r;
  }
  CHECK(worst < 0.1f * mean);
  CHECK(r == 0.0f);
}

TEST_CASE("scratch: a lone count is a nudge, not a carry-on: the head moves by at most a count and a half") {
  Rig t;
  settle_still(t);
  const float n0 = static_cast<float>(t.n);
  const float lag0 = t.s.lag();
  step_rate(t, 1);
  for (unsigned b = 0; b < 800; ++b) step_rate(t, 0);
  const float moved = (static_cast<float>(t.n) - n0) - (t.s.lag() - lag0);  // the head's travel in the source: the frames of time elapsed less the lag gained
  CHECK(moved > 0.3f * MasterScratch::kFramesPerCount);
  CHECK(moved < 1.5f * MasterScratch::kFramesPerCount);
  CHECK(t.s.rate() == 0.0f);
}

TEST_CASE("scratch: a reversal is smooth - no sample step beyond the signal's own at the head's rate") {
  Rig t;
  std::vector<float> warm, out;
  CHECK(t.arm(1.0f));
  t.run(1200, 0, sine, warm);
  // forwards one count every 14 blocks (rate about 0.9), then backwards the same: the head turns round through 0
  float maxrate = 0.0f;
  for (unsigned b = 1; b <= 500; ++b) {
    t.block(b % 14u == 0u ? 1 : 0, sine, out);
    maxrate = std::fmax(maxrate, std::fabs(t.s.rate()));
  }
  for (unsigned b = 1; b <= 500; ++b) {
    t.block(b % 14u == 0u ? -1 : 0, sine, out);
    maxrate = std::fmax(maxrate, std::fabs(t.s.rate()));
  }
  const float own = 0.5f * 6.2831853f * 1000.0f / 44117.65f;
  float worst = 0.0f;
  for (std::size_t i = 64; i < out.size(); ++i) worst = std::fmax(worst, std::fabs(out[i] - out[i - 1]));
  CHECK(maxrate < 1.3f);
  CHECK(worst <= own * (maxrate + 0.05f) + 0.004f);
  CHECK(worst > 0.0f);
}

TEST_CASE("scratch: the Hermite read is exact at the taps and for a quadratic, and beats the linear read") {
  using H = MasterScratch;
  CHECK(H::hermite4(3.0f, 5.0f, 7.0f, 11.0f, 0.0f) == 5.0f);
  CHECK(H::hermite4(3.0f, 5.0f, 7.0f, 11.0f, 1.0f) == 7.0f);
  CHECK(H::hermite4(2.0f, 2.0f, 2.0f, 2.0f, 0.37f) == doctest::Approx(2.0f));
  const auto f = [](float x) { return 0.25f - 0.5f * x + 0.125f * x * x; };  // a quadratic: taps at x = -1, 0, 1, 2
  for (int q = 0; q <= 10; ++q) {
    const float t = static_cast<float>(q) / 10.0f;
    CHECK(H::hermite4(f(-1.0f), f(0.0f), f(1.0f), f(2.0f), t) == doctest::Approx(f(t)).epsilon(1e-5));
  }
  // a 1 kHz sine at 44.1 kHz, half way between two frames
  const float w = 6.2831853f * 1000.0f / 44100.0f;
  const float h = H::hermite4(std::sin(-w), std::sin(0.0f), std::sin(w), std::sin(2.0f * w), 0.5f);
  const float lin_err = std::fabs(0.5f * (std::sin(0.0f) + std::sin(w)) - std::sin(0.5f * w));
  CHECK(std::fabs(h - std::sin(0.5f * w)) < 1e-4f);
  CHECK(lin_err > 10.0f * std::fabs(h - std::sin(0.5f * w)));
}

TEST_CASE("scratch: through the stage, a Platter on a rising ramp rises by the head's travel (the Hermite is exact on a line)") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(1200, 0, lin, out);
  for (unsigned b = 0; b < 100; ++b) t.block(b % 2u == 0u ? 1 : 0, lin, out);  // a count every second block: a rate of about 6.5, the lag shrinks by 5.5 a frame
  const float r0 = t.s.rate();
  out.clear();
  t.block(0, lin, out);
  const float r1 = t.s.rate();
  const float dr = (r1 - r0) / 64.0f;
  float expect = 0.0f;
  float rr = r0 + 0.5f * dr;
  for (std::uint32_t i = 0; i + 1 < kB; ++i, rr += dr) expect += 2e-5f * rr;  // the head steps rr[i] frames between frame i and i + 1
  CHECK(t.s.lag() > 1000.0f);
  CHECK(out[kB - 1] - out[0] == doctest::Approx(expect).epsilon(0.02));
}

TEST_CASE("scratch: TAPE STOP is still the linear read of C1 - its output matches a straight reference") {
  // A 10 kHz sine: a linear read is off the sine by a quarter of its size at the half frame, a Hermite read is not; the reference below is the C1 maths (lag = the sum of 1 - rate, linear between two frames).
  const auto hf = [](std::uint32_t n) { return 0.5f * std::sin(6.2831853f * 10000.0f * static_cast<float>(n) / 44117.65f); };
  Rig t;
  std::vector<float> out;
  const std::uint32_t beat = 22058;
  CHECK(t.arm(2.0f, beat));
  t.run(120, 0, hf, out);
  double lag = 0.0;
  float rate0 = 1.0f;
  double worst = 0.0;
  for (unsigned b = 0; b < 120; ++b) {
    const std::uint32_t tt = (b + 1u) * kB;
    const float rate1 = tt >= beat ? 0.0f : 1.0f - static_cast<float>(tt) / static_cast<float>(beat);
    const float dr = (rate1 - rate0) / 64.0f;
    float rr = rate0 + 0.5f * dr;
    for (std::uint32_t i = 0; i < kB; ++i, rr += dr) {
      const std::uint32_t pos = b * kB + i;
      const std::uint32_t whole = static_cast<std::uint32_t>(lag);
      const double fr = lag - static_cast<double>(whole);
      const float g = std::fmin(8.0f * std::fabs(rr), 1.0f);
      const double ref = static_cast<double>(g) * ((1.0 - fr) * static_cast<double>(hf(pos - whole)) + fr * static_cast<double>(hf(pos - whole - 1u)));
      if (pos > 3u + whole) worst = std::fmax(worst, std::fabs(ref - static_cast<double>(out[pos])));
      lag += 1.0 - static_cast<double>(rr);
    }
    rate0 = rate1;
  }
  CHECK(worst < 2e-3);  // (the ring is 15 bits: far below the 0.1 or so that a Hermite read would differ by)
}

namespace {
// OS-SCRATCH, the evidence that TAPE STOP (modes 2 and 3) is unchanged: a MIRROR of the C1 stage's Tape / Release path (the code of the base commit abfcf51b, linear read, float for float: the same
// advance_lag, the same lag limit, the same gate and release), run beside the real stage over the scenarios below. It reads the ring the stage wrote, so both see the same 15-bit samples.
struct TapeRef {
  enum class Ph { Tape, Release, Off };
  const std::int16_t* ring;
  std::uint32_t cap;
  Ph ph = Ph::Tape;
  bool exit_req = false;
  std::uint32_t t = 0, len, fade = 0, written = 0, widx = 0;
  std::int32_t lag_i = 0;
  float lag_f = 0.0f, rate = 1.0f;
  // OS-SCRATCH-IDLE: the same mirror for a Platter run (`platter`): the rate at the end of each block is the real stage's (`r1_ext`, set before block()), the read is the 4-point Hermite of the Platter.
  bool platter = false;
  float r1_ext = 0.0f;
  bool c1_gate = false;  // the real stage is in the backspin (Spin): its gate is C1's slope, not the hand's
  TapeRef(const std::int16_t* r, std::uint32_t c, std::uint32_t ramp) : ring(r), cap(c), len(ramp != 0u ? ramp : 1u) {}
  bool retarget(std::uint32_t ramp) {  // set_mode(3)
    if (ph != Ph::Tape || ramp == 0u) return false;
    const float r = rate < 0.0f ? 0.0f : (rate > 1.0f ? 1.0f : rate);
    len = ramp;
    t = static_cast<std::uint32_t>((1.0f - r) * static_cast<float>(len));
    return true;
  }
  void end() {  // set_mode(0)
    if (ph != Ph::Off) exit_req = true;
  }
  void begin_release() {  // OS-SCRATCH-IDLE: the end of the Platter's backspin (the stage's begin_release())
    ph = Ph::Release;
    fade = 0;
  }
  void block(const float* in, float* out) {
    if (ph == Ph::Off) {
      for (std::uint32_t i = 0; i < kB; ++i) out[i] = in[i];
      return;
    }
    if (exit_req) {
      exit_req = false;
      if (ph == Ph::Tape) {
        ph = Ph::Release;
        fade = 0;
      }
    }
    const std::uint32_t idx0 = widx;
    const std::uint32_t before = written;
    widx = (idx0 + kB) % cap;
    written = before + kB < cap ? before + kB : cap;
    const float r0 = rate;
    float r1 = r0;
    if (ph == Ph::Tape) {
      t += kB;
      r1 = platter ? r1_ext : (t >= len ? 0.0f : 1.0f - static_cast<float>(t) / static_cast<float>(len));
    }
    const float dr = (r1 - r0) / static_cast<float>(kB);
    float rr = r0 + 0.5f * dr;
    std::int32_t li = lag_i;
    float lf = lag_f;
    const std::int32_t ring_lim = static_cast<std::int32_t>(cap - kB - 2u);
    const auto limit = [&](std::uint32_t seen) {
      const std::uint32_t avail = seen < cap ? seen : cap;
      const std::int32_t lim = avail >= 2u ? static_cast<std::int32_t>(avail - 2u) : 0;
      return lim < ring_lim ? lim : ring_lim;
    };
    const bool releasing = ph == Ph::Release;
    for (std::uint32_t i = 0; i < kB; ++i, rr += dr) {
      const std::int32_t lag_max = limit(before + i + 1u);
      if (li >= lag_max) {
        li = lag_max;
        lf = 0.0f;
      }
      float w = 1.0f;
      if (releasing) {
        const std::uint32_t f = fade + i;
        w = f >= MasterScratch::kReleaseSamples ? 0.0f : 1.0f - static_cast<float>(f) / static_cast<float>(MasterScratch::kReleaseSamples);
      }
      const float mag = rr < 0.0f ? -rr : rr;
      const float slope = platter && ph == Ph::Tape && !c1_gate ? MasterScratch::kPlatterGate : MasterScratch::kGateSlope;  // OS-BUG-SCRATCH: only the hand itself (the Platter phase) has the steeper gate; the release keeps C1's
      const float g = mag * slope < 1.0f ? mag * slope : 1.0f;
      float hl = 0.0f;
      if (g > 0.0f && w > 0.0f) {
        std::uint32_t j = idx0 + i + cap - static_cast<std::uint32_t>(li);
        j = j >= cap ? j - cap : j;
        j = j >= cap ? j - cap : j;
        const std::uint32_t k = j == 0u ? cap - 1u : j - 1u;
        if (platter) {
          const std::uint32_t m = (li == 0 && i == kB - 1u) ? j : (j + 1u == cap ? 0u : j + 1u);
          const std::uint32_t o = k == 0u ? cap - 1u : k - 1u;
          hl = MasterScratch::hermite4(static_cast<float>(ring[2u * m]), static_cast<float>(ring[2u * j]), static_cast<float>(ring[2u * k]), static_cast<float>(ring[2u * o]), lf) * (g * (2.0f / 32767.0f));
        } else {
          const float a = (1.0f - lf) * g * (2.0f / 32767.0f);
          const float b = lf * g * (2.0f / 32767.0f);
          hl = static_cast<float>(ring[2u * j]) * a + static_cast<float>(ring[2u * k]) * b;
        }
      }
      out[i] = in[i] * (1.0f - w) + hl * w;
      MasterScratch::advance_lag(li, lf, 1.0f - rr);
    }
    const std::int32_t lag_cap = limit(written);
    if (li >= lag_cap) {
      li = lag_cap;
      lf = 0.0f;
    }
    lag_i = li;
    lag_f = lf;
    rate = r1;
    if (releasing) {
      fade += kB;
      if (fade >= MasterScratch::kReleaseSamples) ph = Ph::Off;
    }
  }
};

struct TapeScenario {
  const char* name;
  std::uint32_t ramp;         // the beat in samples (mode 2)
  unsigned blocks;
  int counts;                 // the scripted encoder per block: the TAPE STOP ignores it
  unsigned retarget_at;       // mode 3 before this block (kNever: none) ...
  std::uint32_t retarget_len; // ... with this length
  unsigned exit_at;           // mode 0 before this block (kNever: none)
};
constexpr unsigned kNever = 0xFFFFFFFFu;
}  // namespace

TEST_CASE("scratch: TAPE STOP, modes 2 and 3, is the C1 linear path in 8 scenarios (a mirror of the base stage)") {
  const auto hf = [](std::uint32_t n) { return 0.5f * std::sin(6.2831853f * 10000.0f * static_cast<float>(n) / 44117.65f); };
  const TapeScenario scenarios[] = {
      {"half second", 22058, 500, 0, kNever, 0, kNever},
      {"one frame", 1, 40, 0, kNever, 0, kNever},
      {"two seconds", 88200, 1300, 0, kNever, 0, kNever},
      {"encoder moving", 22058, 500, 3, kNever, 0, kNever},
      {"mode 3 mid-brake", 44117, 900, 0, 100, 11029, kNever},
      {"mode 3 after the stop", 11029, 420, 0, 200, 22058, kNever},
      {"end mid-brake, mode 3 refused in the release", 44117, 400, 0, 151, 5000, 150},
      {"end after the stop", 11029, 400, 0, kNever, 0, 300},
  };
  for (const TapeScenario& sc : scenarios) {
    CAPTURE(sc.name);
    Rig t;
    std::vector<float> out;
    CHECK(t.arm(2.0f, sc.ramp));
    TapeRef ref(t.ring, t.frames, sc.ramp);
    double worst = 0.0;
    double worst_rate = 0.0;
    unsigned lag_mismatch = 0;
    for (unsigned b = 0; b < sc.blocks; ++b) {
      if (b == sc.retarget_at) CHECK(t.s.set_mode(3.0f, sc.retarget_len) == ref.retarget(sc.retarget_len));
      if (b == sc.exit_at) {
        CHECK(t.s.set_mode(0.0f, 0));
        ref.end();
      }
      const std::uint32_t n0 = t.n;
      t.block(sc.counts, hf, out);  // (the stage has written the block into the ring when it returns)
      float in[kB], want[kB];
      for (std::uint32_t i = 0; i < kB; ++i) in[i] = hf(n0 + i);
      ref.block(in, want);
      for (std::uint32_t i = 0; i < kB; ++i) worst = std::fmax(worst, std::fabs(static_cast<double>(want[i]) - static_cast<double>(out[n0 + i])));
      if (!t.s.idle()) {
        worst_rate = std::fmax(worst_rate, std::fabs(static_cast<double>(t.s.rate()) - static_cast<double>(ref.rate)));
        // OS-SCRATCH-IDLE: the lag, whole frames exact and the fraction, block by block (the held TAPE STOP takes the standstill shortcut of process())
        if (t.s.lag_frames() != ref.lag_i || std::fabs(t.s.lag_fraction() - ref.lag_f) > 1e-6f) ++lag_mismatch;
      }
    }
    CHECK(lag_mismatch == 0u);
    CHECK(worst < 1e-4);  // (a Hermite read of the 10 kHz sine would be off by 0.1 or so; the margin is for a contracted multiply-add on the target)
    CHECK(worst_rate < 1e-6);
    CHECK((sc.exit_at == kNever) == !t.s.idle());
  }
}

TEST_CASE("scratch: SCR off after a hand that stopped dead releases (no backspin); right after a flick it spins; neither clicks") {
  // The latch needs the hand's own target too: the 70 ms inertia keeps the head's rate beyond kSpinMin for about 100 blocks after a hand that stopped dead.
  struct Case { unsigned still; MasterScratch::Phase want; };
  const Case cases[] = {{10, MasterScratch::Phase::Spin}, {40, MasterScratch::Phase::Release}};
  for (const Case& c : cases) {
    CAPTURE(c.still);
    Rig t;
    std::vector<float> out;
    CHECK(t.arm(1.0f));
    for (unsigned b = 1; b <= 1000; ++b) t.block(b % 14u == 0u ? 1 : 0, sine, out);  // forwards, so the head has room to go back
    t.run(30, -2, sine, out);                                                         // a backward flick, rate about -26
    CHECK(t.s.rate() < -MasterScratch::kSpinMin);
    t.run(c.still, 0, sine, out);  // the hand stops dead
    CHECK(t.s.rate() < -1.0f);     // (the head is still coasting on the inertia)
    const std::size_t from = out.size() - 1u;
    CHECK(t.s.set_mode(0.0f, 0));
    t.run(1, 0, sine, out);
    CHECK(t.s.phase() == c.want);
    if (c.want == MasterScratch::Phase::Release) {
      float maxrate = std::fabs(t.s.rate());
      for (unsigned b = 0; b < 8; ++b) t.run(1, 0, sine, out);
      CHECK(t.s.idle());
      // no click: the release is a 256-sample crossfade from the head (at about this rate) to the live mix
      const float own = 0.5f * 6.2831853f * 1000.0f / 44117.65f;
      float worst = 0.0f;
      for (std::size_t i = from + 1u; i < out.size(); ++i) worst = std::fmax(worst, std::fabs(out[i] - out[i - 1u]));
      CHECK(worst <= own * (std::fmax(maxrate, 1.0f) + 0.05f) + 1.0f / 256.0f + 0.004f);
    }
  }
}

TEST_CASE("scratch: a reversal after a flick follows the new direction at once - the old direction's debt is dropped") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  for (unsigned b = 1; b <= 800; ++b) t.block(b % 14u == 0u ? 1 : 0, sine, out);  // forwards at about 0.9, so the head has room to go back
  t.run(600, 0, sine, out);                                                      // the hand rests: rate 0 (the 70 ms inertia takes about 465 blocks to come to rest from a rate of 1)
  CHECK(t.s.rate() == 0.0f);
  t.run(1, 16, sine, out);  // one fast block forwards: a clamped flick, about 8 counts of the hand not yet played
  float peak = t.head();
  float prev = t.s.rate();
  unsigned rising = 0;
  float rate_end = 0.0f;
  for (unsigned b = 1; b <= 120; ++b) {  // straight back, one count in 3 blocks (a target of about -4)
    t.block(b % 3u == 0u ? -1 : 0, sine, out);
    peak = std::fmax(peak, t.head());
    const float r = t.s.rate();
    if (b >= 3u && b <= 40u && r >= prev) ++rising;  // from the first count back the target is negative: the rate only falls (the inertia carries it through 0 in about 40 blocks)
    prev = r;
    rate_end = r;
  }
  CHECK(rising == 0u);  // (with the debt kept the target stayed forwards until the hand had come back about 8 counts: the rate rose for 24 blocks)
  CHECK(rate_end < -0.3f);
  CHECK(t.head() < peak - 0.5f * MasterScratch::kFramesPerCount);
}

// OS-SCRATCH-IDLE: with the hand still (rate exactly 0 before and after the block, no release fade) process() skips the per-sample read and mix: the output is exact silence and only the lag moves. The proof is the
// mirror of the per-sample loop (TapeRef with `platter`: the Hermite read, the rate fed from the real stage) run beside the real stage over a whole session - a scratch that moves, stops, flicks, crawls, stops and is
// released - on the 2.5 s ring and on the smallest ring (the lag limit and the ring wrap bite): the output, and the lag (whole frames exact, the fraction to 1e-6), block by block.
TEST_CASE("scratch: a still hand takes the shortcut - the output and the lag are those of the per-sample loop, moving, still and moving again") {
  const auto hf = [](std::uint32_t n) { return 0.5f * std::sin(6.2831853f * 3000.0f * static_cast<float>(n) / 44117.65f); };
  const auto hand = [](unsigned b) -> int {
    if (b < 500u) return 0;                                                            // the entry runs down to the still hand (70 ms inertia: about 465 blocks)
    if (b < 800u) return b % 4u == 0u ? (((b - 500u) / 40u) % 2u != 0u ? -1 : 1) : 0;  // baby scratch, forth and back
    if (b == 900u) return 16;                                                          // a clamped flick, then the rest
    if (b >= 1500u && b < 1700u) return b % 14u == 0u ? 1 : 0;                         // a slow crawl
    return 0;
  };
  constexpr unsigned kExit = 2400;
  for (const bool small : {false, true}) {
    CAPTURE(small);
    Rig big;
    SmallRig sm;
    Rig& t = small ? static_cast<Rig&>(sm) : big;
    std::vector<float> out;
    CHECK(t.arm(1.0f));
    TapeRef ref(t.ring, t.frames, 1u);
    ref.platter = true;
    t.s.set_tuning(70.0f, 1.0f, 32.0f, false);  // OS-BUG-SCRATCH: the mirror is the plain Hermite read (the anti-alias filter has its own tests)
    double worst = 0.0;
    unsigned lag_mismatch = 0, shortcuts = 0;
    float prev_rate = 1.0f;
    for (unsigned b = 0; b < kExit + 20u; ++b) {
      if (b == kExit) {
        CHECK(t.s.set_mode(0.0f, 0));
        ref.end();
      }
      const std::uint32_t n0 = t.n;
      t.block(hand(b), hf, out);
      float in[kB], want[kB];
      for (std::uint32_t i = 0; i < kB; ++i) in[i] = hf(n0 + i);
      ref.r1_ext = t.s.rate();
      ref.block(in, want);
      for (std::uint32_t i = 0; i < kB; ++i) worst = std::fmax(worst, std::fabs(static_cast<double>(want[i]) - static_cast<double>(out[n0 + i])));
      if (!t.s.idle()) {
        if (t.s.lag_frames() != ref.lag_i || std::fabs(t.s.lag_fraction() - ref.lag_f) > 1e-6f) ++lag_mismatch;
        if (b < kExit && prev_rate == 0.0f && t.s.rate() == 0.0f) {
          ++shortcuts;
          for (std::uint32_t i = 0; i < kB; ++i) CHECK(out[n0 + i] == 0.0f);  // exact silence
        }
      }
      if (b == kExit) CHECK(t.s.phase() == MasterScratch::Phase::Release);  // (a hand that has rested for 300 blocks releases, it does not spin)
      prev_rate = t.s.rate();
    }
    CHECK(t.s.idle());
    CHECK(lag_mismatch == 0u);
    CHECK(worst < 1e-4);
    CHECK(shortcuts > 150u);  // the rests after the flick and the crawl: the shortcut was really taken
    if (small) CHECK(sm.guards_intact());
  }
}

namespace {
// OS-SCRATCH-IDLE, the bit-exact guard of the standstill shortcut. Each scenario is a scripted session (signals without libm, so that the numbers are those of the unit, too) run beside the mirror of the per-sample
// loop (TapeRef, the code the shortcut replaced): every output bit of both channels, the whole lag and its fraction, block by block, must be the mirror's; and the session's FNV-1a over those bits (the output, the
// lag, the rate) is pinned to a golden value taken from the stage BEFORE the shortcut existed. A ULP of drift in the lag or a sample that is not +0.0 fails one of the two.
float tri(std::uint32_t n) {
  const float p = static_cast<float>(n % 89u) / 89.0f;
  return 0.8f * (p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p);
}
float noise(std::uint32_t n) {
  std::uint32_t x = n * 2654435761u;
  x ^= x >> 15;
  x *= 2246822519u;
  x ^= x >> 13;
  return 0.7f * static_cast<float>(static_cast<std::int32_t>(x >> 8) - 8388608) / 8388608.0f;
}
float mixed(std::uint32_t n) { return 0.6f * tri(n) + 0.3f * noise(n); }
float quiet(std::uint32_t) { return 0.0f; }
float step(std::uint32_t) { return 1.5f; }  // a source that jumps to a high DC level (the ring holds +-2.0)

std::uint32_t bits_of(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}

struct Ab {
  using Ph = TapeRef::Ph;
  Rig& rig;
  TapeRef ref;
  bool pending_exit = false;
  std::uint32_t hash = 2166136261u;
  unsigned bad_out = 0, bad_lr = 0, bad_lag = 0, still_blocks = 0;
  explicit Ab(Rig& r) : rig(r), ref(r.ring, r.frames, 1u) {}
  void add(std::uint32_t v) {
    for (unsigned k = 0; k < 4u; ++k) {
      hash ^= (v >> (8u * k)) & 0xFFu;
      hash *= 16777619u;
    }
  }
  void arm(float mode, std::uint32_t ramp = 0) {
    CHECK(rig.arm(mode, ramp));
    ref = TapeRef(rig.ring, rig.frames, mode == 1.0f ? 1u : ramp);
    ref.platter = mode == 1.0f;
    if (ref.platter) rig.s.set_tuning(70.0f, 1.0f, 32.0f, false);  // OS-BUG-SCRATCH: the mirror is the plain Hermite read: the anti-alias filter is off here (its own tests are below)
    pending_exit = false;
  }
  void end() {  // SCR off
    CHECK(rig.s.set_mode(0.0f, 0));
    pending_exit = true;
  }
  void retarget(std::uint32_t len) {  // mode 3
    const bool real = rig.s.set_mode(3.0f, len);
    CHECK(real == ref.retarget(len));
    CHECK(real);
  }
  void abort() {
    rig.s.abort();
    ref.ph = Ph::Off;
    pending_exit = false;
  }
  template <class Sig>
  void block(int counts, Sig in) {
    g_count = static_cast<std::uint16_t>(g_count + counts);
    const std::uint32_t n0 = rig.n;
    float l[kB], r[kB], src[kB], want[kB];
    for (std::uint32_t i = 0; i < kB; ++i) src[i] = l[i] = r[i] = in(n0 + i);
    const bool was_idle = rig.s.idle();
    const float rate0 = rig.s.rate();
    rig.s.process(l, r, kB);
    rig.n += kB;
    if (pending_exit) {  // the real stage chose between the backspin and the release: the mirror follows (the backspin runs on the stage's rate)
      pending_exit = false;
      if (rig.s.phase() != MasterScratch::Phase::Spin) ref.end();
    }
    ref.r1_ext = rig.s.rate();
    ref.c1_gate = ref.platter && rig.s.phase() != MasterScratch::Phase::Platter;  // the mirror's Tape is the Platter run or the backspin; the stage's phase tells which
    ref.block(src, want);
    if (ref.platter && ref.ph == Ph::Tape && rig.s.phase() == MasterScratch::Phase::Release) ref.begin_release();  // the brake ran out at the end of this block
    bool zero = true;
    for (std::uint32_t i = 0; i < kB; ++i) {
      if (bits_of(l[i]) != bits_of(want[i])) ++bad_out;
      if (bits_of(r[i]) != bits_of(l[i])) ++bad_lr;
      if (bits_of(l[i]) != 0u) zero = false;
      add(bits_of(l[i]));
    }
    const bool live = !rig.s.idle();
    if (live) {
      if (rig.s.lag_frames() != ref.lag_i || bits_of(rig.s.lag_fraction()) != bits_of(ref.lag_f)) ++bad_lag;
      add(static_cast<std::uint32_t>(rig.s.lag_frames()));
      add(bits_of(rig.s.lag_fraction()));
      add(bits_of(rig.s.rate()));
      const auto ph = rig.s.phase();
      if (!was_idle && rate0 == 0.0f && rig.s.rate() == 0.0f && zero && (ph == MasterScratch::Phase::Platter || ph == MasterScratch::Phase::Tape)) ++still_blocks;
    }
  }
  template <class Sig>
  void run(unsigned blocks, int counts, Sig in) {
    for (unsigned b = 0; b < blocks; ++b) block(counts, in);
  }
  template <class Sig>
  void crawl(unsigned blocks, int counts, Sig in) {  // a count every 14th block: a slow turn
    for (unsigned b = 1; b <= blocks; ++b) block(b % 14u == 0u ? counts : 0, in);
  }
};

constexpr unsigned kScenarios = 8;
const char* const kScenarioName[kScenarios] = {"a session: moves, rests, flick, crawl, release", "SCR off through the backspin", "SCR off in the release, head coasting", "SCR off in the release, head still",
                                               "a second SCR on after still blocks, and an abort", "mode 3 on a still TAPE STOP", "hand during the entry run-down", "the source changes while still"};

// The golden hashes (FNV-1a of the output bits, the lag and the rate per block), [scenario][0: 2.5 s ring, 1: 0.1 s ring]. The count-to-frames constant depends on the engine rate, so there is one table per rate
// (the TAPE STOP scenario, 5, does not use the hand and is the same in both). Re-pinned by OS-BUG-SCRATCH (70 ms inertia, ahead 1.0, gate 32, hold, history 8; the mirror above proves the output and lag equal to
// the per-sample loop's block by block, the AA off); scenario 5 (TAPE STOP) is unchanged since OS-SCRATCH.
const std::uint32_t kGolden44100[kScenarios][2] = {
    {1488614164u, 2173584279u}, {1597031248u, 519270040u}, {1523201487u, 3671769802u}, {1837440325u, 3672386063u},
    {556121543u, 3811330824u},  {2869605565u, 2419989626u}, {4044944161u, 2305099546u}, {688836319u, 2242029564u},
};
const std::uint32_t kGolden44117[kScenarios][2] = {
    {4145799046u, 2457319228u}, {998214598u, 4080138003u}, {357598975u, 3609520682u}, {1172007437u, 2667607913u},
    {773679203u, 2383339796u}, {2869605565u, 2419989626u}, {1224983638u, 2434263861u}, {3951884472u, 2024838510u},
};

// A flick backwards after a forward crawl, then the hand rests for `rest` blocks and SCR goes off.
void flick_and_end(Ab& ab, unsigned rest, bool want_spin) {
  ab.arm(1.0f);
  ab.run(400, 0, mixed);  // the entry runs down to the still hand
  ab.crawl(300, 1, mixed);
  ab.run(30, -2, mixed);
  ab.run(rest, 0, mixed);
  ab.end();
  ab.block(0, mixed);
  CHECK((ab.rig.s.phase() == MasterScratch::Phase::Spin) == want_spin);
  ab.run(450, 0, mixed);
  CHECK(ab.rig.s.idle());
}

void play(unsigned which, Ab& ab) {
  Rig& rig = ab.rig;
  switch (which) {
    case 0: {
      const auto hand = [](unsigned b) -> int {
        if (b < 150u) return 0;
        if (b < 450u) return b % 4u == 0u ? (((b - 150u) / 40u) % 2u != 0u ? -1 : 1) : 0;
        if (b == 550u) return 16;
        if (b >= 960u && b < 1160u) return b % 14u == 0u ? 1 : 0;
        return 0;
      };
      ab.arm(1.0f);
      for (unsigned b = 0; b < 1520u; ++b) {
        if (b == 1500u) ab.end();
        ab.block(hand(b), mixed);
      }
      CHECK(rig.s.idle());
      break;
    }
    case 1:  // a head that was turning back fast at the exit spins down; its rate is not 0 there (the latch needs a backward rate of 4 within 20 blocks, the head takes 140 to come to rest)
      flick_and_end(ab, 10, true);
      break;
    case 2:
      flick_and_end(ab, 40, false);
      break;
    case 3:
      flick_and_end(ab, 600, false);  // the head has come to rest: the exit is taken from the shortcut
      CHECK(ab.still_blocks > 60u);
      break;
    case 4:
      ab.arm(1.0f);
      ab.run(400, 0, mixed);
      ab.end();
      ab.run(8, 0, mixed);
      CHECK(rig.s.idle());
      ab.arm(1.0f);  // a second SCR on, the ring holds the first run's frames
      ab.run(3, 0, mixed);
      ab.run(400, 0, mixed);
      ab.block(1, mixed);
      ab.block(1, mixed);
      ab.run(100, 0, mixed);
      ab.abort();
      CHECK(rig.s.idle());
      ab.run(5, 0, mixed);
      ab.arm(1.0f);  // and a third one straight after the abort
      ab.crawl(120, 1, mixed);
      ab.end();
      ab.run(20, 0, mixed);
      CHECK(rig.s.idle());
      break;
    case 5:
      ab.arm(2.0f, 2000);  // the beat of 2000 frames: still after 32 blocks
      ab.run(80, 0, mixed);
      CHECK(rig.s.rate() == 0.0f);
      ab.retarget(7000);  // (t = len: the rate stays 0)
      ab.run(50, 0, mixed);
      CHECK(rig.s.rate() == 0.0f);
      ab.retarget(100);
      ab.run(30, 0, mixed);
      ab.retarget(5000);
      ab.run(5, 0, mixed);
      ab.end();
      ab.run(12, 0, mixed);
      CHECK(rig.s.idle());
      CHECK(ab.still_blocks > 100u);
      break;
    case 6: {
      ab.arm(1.0f);
      unsigned zero_run = 0;
      int sign = 1;
      for (unsigned b = 0; b < 1400u; ++b) {
        int c = 0;
        if ((b >= 4u && b < 8u) || (b >= 52u && b < 53u)) c = 1;  // hand movement while the entry runs down (rate 1 -> 0 takes about 340 blocks)
        if (b >= 30u && b < 34u) c = -1;
        if (b >= 100u && zero_run == 5u) {  // a count 5 blocks after the head has come to rest: the first block after a still one
          c = sign;
          sign = -sign;
        }
        ab.block(c, mixed);
        zero_run = rig.s.rate() == 0.0f ? zero_run + 1u : 0u;
      }
      CHECK(ab.still_blocks > 10u);  // (20: a count wakes the head after 5 still blocks, and it takes ~100 blocks to come to rest again)
      break;
    }
    case 7:
      ab.arm(1.0f);
      ab.run(400, 0, tri);
      ab.run(60, 0, noise);  // the mix changes while the head is held ...
      ab.run(40, 0, quiet);
      ab.run(40, 0, step);
      ab.run(30, 0, mixed);
      for (unsigned b = 0; b < 80u; ++b) ab.block(b % 3u == 0u ? -1 : 0, noise);  // ... and the hand walks back through the changes
      ab.run(600, 0, tri);
      for (unsigned b = 0; b < 60u; ++b) ab.block(b % 2u == 0u ? 1 : 0, quiet);
      ab.run(600, 0, mixed);
      ab.end();
      ab.run(12, 0, mixed);
      CHECK(rig.s.idle());
      CHECK(ab.still_blocks > 100u);
      break;
    default:
      break;
  }
}
}  // namespace

TEST_CASE("scratch: the standstill shortcut is bit-exact - output, lag and rate of the per-sample loop in 8 transitions, pinned by a golden hash") {
  for (unsigned which = 0; which < kScenarios; ++which) {
    for (const bool small : {false, true}) {
      CAPTURE(kScenarioName[which]);
      CAPTURE(which);
      CAPTURE(small);
      Rig big;
      SmallRig sm;
      Rig& t = small ? static_cast<Rig&>(sm) : big;
      Ab ab(t);
      play(which, ab);
      CHECK(ab.bad_out == 0u);
      CHECK(ab.bad_lr == 0u);
      CHECK(ab.bad_lag == 0u);
      CHECK(ab.still_blocks > 0u);  // the shortcut ran
      CHECK(ab.hash == (MasterScratch::kRate < 44110.0f ? kGolden44100 : kGolden44117)[which][small ? 1 : 0]);
      if (small) CHECK(sm.guards_intact());
    }
  }
}

// OS-SCRATCH-IDLE2 ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

// The per-sample loop of the lag at rate 0 (the code still_lag replaced), one block.
void still_lag_loop(std::uint32_t cap, std::uint32_t before, std::int32_t ring_lim, std::int32_t& li, float& lf) {
  for (std::uint32_t i = 0; i < kB; ++i) {
    const std::int32_t lag_max = MasterScratch::read_limit_for(cap, before + i + 1u, ring_lim);
    if (li >= lag_max) {
      li = lag_max;
      lf = 0.0f;
    }
    MasterScratch::advance_lag(li, lf, 1.0f - 0.0f);
  }
}

std::uint32_t lcg(std::uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

// The ring's conversion as it was before the fast path (float clamp, then round half away from zero).
std::int16_t q15_half_ref(float x) {
  float v = x * (0.5f * 32767.0f);
  v = v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v);
  return static_cast<std::int16_t>(static_cast<int>(v + (v >= 0.0f ? 0.5f : -0.5f)));
}

// The target's heap and emulated clock are small (a 4 MB sweep vector and 900,000 loop runs aborted qemu_audio_limiter): the host runs the full counts, the target a spread of them.
#if defined(__arm__)
constexpr unsigned kLagStates = 6000u;
constexpr std::uint32_t kBitStride = 65537u;  // 0x10001: walks sign, exponent and mantissa together
constexpr std::size_t kSweepReserve = 65536u + 8000u + 40000u;
#else
constexpr unsigned kLagStates = 150000u;
constexpr std::uint32_t kBitStride = 4099u;
constexpr std::size_t kSweepReserve = 1100000u;
#endif

float from_bits(std::uint32_t u) {
  float f;
  std::memcpy(&f, &u, sizeof f);
  return f;
}

}  // namespace

TEST_CASE("scratch: the closed-form lag of a block at rate 0 equals the per-sample loop, on every ring state") {
  std::uint32_t seed = 12345u;
  std::uint32_t checked = 0;
  for (const std::uint32_t cap : {4410u, 4411u, 4474u, 4475u, 110250u, 176400u}) {
    const std::int32_t ring_lim = static_cast<std::int32_t>(cap - kB - 2u);
    for (unsigned n = 0; n < kLagStates; ++n) {
      // before: the run's start (0..cap, saturated), with the neighbourhoods of the small values and of cap
      std::uint32_t before = lcg(seed) % (cap + 1u);
      if (n % 5u == 0u) before = lcg(seed) % 200u;
      if (n % 5u == 1u) before = cap - lcg(seed) % 200u;
      // the lag: around the limit it will be held at, or anywhere
      std::int32_t li = static_cast<std::int32_t>(lcg(seed) % (cap + 100u));
      if (n % 3u == 0u) li = MasterScratch::read_limit_for(cap, before + 1u, ring_lim) + static_cast<std::int32_t>(lcg(seed) % 140u) - 70;
      if (li < 0) li = 0;
      float lf = 0.0f;
      switch (n % 4u) {
        case 1: lf = static_cast<float>(lcg(seed) & 0x7FFFFFu) * (1.0f / 8388608.0f); break;
        case 2: lf = from_bits(0x3F800000u - 1u - (lcg(seed) & 3u)); break;  // just below 1
        case 3: lf = static_cast<float>(lcg(seed) % 1000u) * 0.001f; break;
        default: break;
      }
      std::int32_t a_i = li, b_i = li;
      float a_f = lf, b_f = lf;
      still_lag_loop(cap, before, ring_lim, a_i, a_f);
      MasterScratch::still_lag(cap, before, ring_lim, b_i, b_f);
      CAPTURE(cap);
      CAPTURE(before);
      CAPTURE(li);
      CAPTURE(lf);
      std::uint32_t fa, fb;
      std::memcpy(&fa, &a_f, sizeof fa);
      std::memcpy(&fb, &b_f, sizeof fb);
      if (a_i != b_i || fa != fb) {
        CHECK(b_i == a_i);
        CHECK(fb == fa);
        return;
      }
      ++checked;
    }
  }
  CHECK(checked == 6u * kLagStates);
}

TEST_CASE("scratch: the ring write is the float clamp and round of before, for every float class, at either alignment and across the ring's end") {
  alignas(4) static std::int16_t buf[2 * kSmall + 4];
  struct Where {
    std::int16_t* ring;
    const char* name;
  };
  const Where where[] = {{buf, "aligned"}, {buf + 1, "halfword offset"}};
  for (const Where& w : where) {
    CAPTURE(w.name);
    MasterScratch s;
    bool bypass = true;
    s.attach(&bypass);
    s.set_counter(&scripted);
    g_count = 0;
    for (std::uint32_t i = 0; i < 2u * kSmall + 4u; ++i) buf[i] = 0x5A5A;
    CHECK(s.stage(w.ring, kSmall));
    CHECK(s.set_mode(1.0f, 0));
    // the values: a sweep of the whole bit space, the rounding ties (k + 0.5 after the scale), the clamp edges, the specials
    std::vector<float> vals;
    vals.reserve(kSweepReserve);
    for (std::uint32_t u = 0; u < 0xFFFFFFFFu - kBitStride; u += kBitStride) vals.push_back(from_bits(u));
    for (std::uint32_t k = 0; k < 2000u; ++k) {
      const float t = static_cast<float>(2u * k + 1u) * (0.5f / (0.5f * 32767.0f));  // v = k + 0.5 up to the float's rounding
      vals.push_back(t);
      vals.push_back(-t);
      vals.push_back(std::nextafter(t, 4.0f));
      vals.push_back(std::nextafter(-t, -4.0f));
    }
    for (const float e : {1.0f, -1.0f, 2.0f, -2.0f, 0.0f, -0.0f, 1e-30f, -1e-30f, 1.00001f}) {
      vals.push_back(e);
      vals.push_back(std::nextafter(e, 8.0f));
      vals.push_back(std::nextafter(e, -8.0f));
    }
    for (const float e : {1e9f, -1e9f, 1.0e38f, -1.0e38f, INFINITY, -INFINITY, NAN, -NAN}) vals.push_back(e);
    std::uint32_t seed = 99u;
    for (unsigned k = 0; k < 20000u; ++k) vals.push_back((static_cast<float>(lcg(seed) & 0xFFFFu) - 32768.0f) * (2.4f / 32768.0f));
    while (vals.size() % kB != 0u) vals.push_back(0.25f);
    std::uint32_t bad = 0;
    std::uint32_t pos = 0;  // frames written
    for (std::size_t b = 0; b + kB <= vals.size(); b += kB) {
      float l[kB], r[kB];
      for (std::uint32_t i = 0; i < kB; ++i) {
        l[i] = vals[b + i];
        r[i] = vals[(b + i + 7u) % vals.size()];
      }
      s.process(l, r, kB);
      for (std::uint32_t i = 0; i < kB; ++i) {
        const std::uint32_t f = (pos + i) % kSmall;
        if (w.ring[2u * f] != q15_half_ref(vals[b + i]) || w.ring[2u * f + 1u] != q15_half_ref(vals[(b + i + 7u) % vals.size()])) ++bad;
      }
      pos += kB;
    }
    CHECK(bad == 0u);
    CHECK(pos > kSmall);  // the ring wrapped, mid-block (4410 is not a multiple of 64)
    // nothing outside the ring was touched
    if (w.ring == buf) {
      CHECK(buf[2u * kSmall] == 0x5A5A);
      CHECK(buf[2u * kSmall + 3u] == 0x5A5A);
    } else {
      CHECK(buf[0] == 0x5A5A);
      CHECK(buf[2u * kSmall + 1u] == 0x5A5A);
    }
  }
}

// OS-BUG-SCRATCH ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// "The scratch sounds bad": the head stopped between two counts of a slow hand, the gate turned the rate dips into amplitude buzz, 25 ms of inertia let the detent rate be heard, and a rate above 1 aliased.
namespace {
// The count of block `b` (1-based) of a hand at a steady speed of one count in `blocks_per_count` blocks (a fractional period: the difference of the running floor).
int steady_hand(unsigned b, float blocks_per_count) {
  const auto pos = [&](unsigned k) { return static_cast<unsigned>(static_cast<float>(k) / blocks_per_count); };
  return static_cast<int>(pos(b) - pos(b - 1u));
}
float peak_of(const std::vector<float>& v, std::size_t from, std::size_t to) {
  float p = 0.0f;
  for (std::size_t i = from; i < to; ++i) p = std::fmax(p, std::fabs(v[i]));
  return p;
}
float rms_of(const std::vector<float>& v, std::size_t from) {
  double acc = 0.0;
  for (std::size_t i = from; i < v.size(); ++i) acc += static_cast<double>(v[i]) * static_cast<double>(v[i]);
  return static_cast<float>(std::sqrt(acc / static_cast<double>(v.size() - from)));
}
std::uint32_t bits32(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  return u;
}
}  // namespace

TEST_CASE("scratch: a steady slow hand (one count every 14 blocks) never stops the head between two counts") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(400, 0, sine, out);  // the entry runs down to the still hand
  out.clear();
  unsigned silent = 0, stopped = 0;
  for (unsigned b = 1; b <= 400; ++b) {
    t.block(b % 14u == 0u ? 1 : 0, sine, out);
    if (b > 28u) {  // the first count is a nudge (a lone event), from the second on the hand is steady
      if (peak_of(out, out.size() - kB, out.size()) == 0.0f) ++silent;
      if (t.s.rate() <= 0.0f) ++stopped;
    }
  }
  CHECK(silent == 0u);
  CHECK(stopped == 0u);
}

TEST_CASE("scratch: a hand at 1x (one count every 12.9 blocks) plays at a rate within 15 % of 1; a held hand is silent") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(400, 0, sine, out);
  float lo = 1e9f, hi = -1e9f;
  for (unsigned b = 1; b <= 800; ++b) {
    t.block(steady_hand(b, MasterScratch::kCountRate), sine, out);
    if (b > 500u) {
      lo = std::fmin(lo, t.s.rate());
      hi = std::fmax(hi, t.s.rate());
    }
  }
  CHECK(lo > 0.85f);
  CHECK(hi < 1.15f);
  // the hand is held: the rate runs to exactly 0 and the output is exact silence
  out.clear();
  t.run(900, 0, sine, out);
  CHECK(t.s.rate() == 0.0f);
  for (std::size_t i = out.size() - 4u * kB; i < out.size(); ++i) CHECK(out[i] == 0.0f);
}

TEST_CASE("scratch: a bursty detent hand (4 counts over 3 blocks, every 52 blocks: 1x on average) plays within 15 % of 1") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(400, 0, sine, out);
  float lo = 1e9f, hi = -1e9f;
  for (unsigned b = 0; b < 1200; ++b) {
    const unsigned k = b % 52u;
    t.block(k == 0u ? 2 : (k < 3u ? 1 : 0), sine, out);
    if (b > 500u) {
      lo = std::fmin(lo, t.s.rate());
      hi = std::fmax(hi, t.s.rate());
    }
  }
  CAPTURE(lo);
  CAPTURE(hi);
  CHECK(lo > 0.85f);
  CHECK(hi < 1.15f);
}

TEST_CASE("scratch: the anti-alias engages without a step - a hand speeding up from 1x to 3x has no jump in the output") {
  const auto tone = [](std::uint32_t n) { return 0.5f * std::sin(6.2831853f * 1000.0f * static_cast<float>(n) / 44117.65f); };
  float worst[2] = {0.0f, 0.0f};
  for (unsigned on = 0; on < 2u; ++on) {
    Rig t;
    std::vector<float> out;
    CHECK(t.arm(1.0f));
    t.s.set_tuning(70.0f, 1.0f, 32.0f, on != 0u);
    t.run(1000, 0, tone, out);
    out.clear();
    float acc = 0.0f;  // counts owed, in counts: the speed climbs 1x -> 3x over 600 blocks
    unsigned given = 0;
    float max_rate = 0.0f;
    for (unsigned b = 0; b < 800; ++b) {
      const float x = b < 600u ? static_cast<float>(b) / 600.0f : 1.0f;
      acc += (1.0f + 2.0f * x) / MasterScratch::kCountRate;
      const unsigned due = static_cast<unsigned>(acc);
      t.block(static_cast<int>(due - given), tone, out);
      given = due;
      max_rate = std::fmax(max_rate, t.s.rate());
    }
    CHECK(max_rate > 1.5f);
    for (std::size_t i = 4u * kB; i < out.size(); ++i) worst[on] = std::fmax(worst[on], std::fabs(out[i] - out[i - 1u]));
  }
  // a 1 kHz tone of 0.5 read at 3.3x steps by 2 pi 1000 3.3 / 44100 * 0.5 = 0.24 at most; a switch at 1.2 would add a step on top
  CAPTURE(worst[0]);
  CAPTURE(worst[1]);
  CHECK(worst[1] < 0.3f);
  CHECK(worst[1] <= worst[0] + 0.01f);
}

TEST_CASE("scratch: a reversal of a steady hand turns the head round at once - the rate falls from the first count back") {
  Rig t;
  std::vector<float> out;
  CHECK(t.arm(1.0f));
  t.run(1000, 0, sine, out);
  for (unsigned b = 1; b <= 500; ++b) t.block(steady_hand(b, MasterScratch::kCountRate), sine, out);
  const float before = t.s.rate();
  CHECK(before > 0.85f);
  // the first count back (the rate has not had time to follow: it must fall, not stay)
  t.block(-1, sine, out);
  CHECK(t.s.rate() < before - 0.02f);
  for (unsigned k = 0; k < 150u; ++k) t.block(-steady_hand(k + 1u, MasterScratch::kCountRate), sine, out);
  CHECK(t.s.rate() < -0.7f);
  CHECK(t.s.rate() > -1.15f);
}

TEST_CASE("scratch: the anti-alias low-pass is exactly off at a rate of 1 or less, and takes the top off above it") {
  // at |rate| <= 1: aa on and aa off give the same bits (a crawl of 0.92 after the entry's run-down)
  std::vector<float> o[2];
  for (unsigned on = 0; on < 2u; ++on) {
    Rig t;
    CHECK(t.arm(1.0f));
    t.s.set_tuning(70.0f, 1.0f, 32.0f, on != 0u);
    CHECK(t.s.aa() == (on != 0u));
    for (unsigned k = 1; k <= 1400u; ++k) t.block(k > 400u && k % 14u == 0u ? 1 : 0, sine, o[on]);
  }
  CHECK(o[0].size() == o[1].size());
  unsigned diff = 0;
  for (std::size_t i = 0; i < o[0].size() && i < o[1].size(); ++i) {
    if (bits32(o[0][i]) != bits32(o[1][i])) ++diff;
  }
  CHECK(diff == 0u);
  // above 1: a 15 kHz tone at a rate of about 4.3 aliases down; the filter removes most of it
  const auto hf = [](std::uint32_t n) { return 0.5f * std::sin(6.2831853f * 15000.0f * static_cast<float>(n) / 44117.65f); };
  float rms[2] = {0.0f, 0.0f};
  for (unsigned on = 0; on < 2u; ++on) {
    Rig t;
    std::vector<float> out;
    CHECK(t.arm(1.0f));
    t.s.set_tuning(70.0f, 1.0f, 32.0f, on != 0u);
    t.run(1000, 0, hf, out);
    out.clear();
    for (unsigned b = 1; b <= 500; ++b) t.block(b % 3u == 0u ? 1 : 0, hf, out);
    CHECK(t.s.rate() > 3.0f);
    rms[on] = rms_of(out, out.size() - 100u * kB);
    for (float v : out) CHECK(std::fabs(v) < 1.0f);
  }
  CHECK(rms[0] > 0.1f);           // (off: the aliased tone is at full size)
  CHECK(rms[1] < 0.5f * rms[0]);  // (on: the top is gone)
}

TEST_CASE("scratch: set_tuning has the documented defaults, clamps its inputs and survives NaN") {
  MasterScratch s;
  CHECK(s.inertia_ms() == doctest::Approx(70.0f).epsilon(0.01));
  CHECK(s.ahead() == 1.0f);
  CHECK(s.gate_slope() == 32.0f);
  CHECK(s.aa());
  s.set_tuning(1000.0f, 9.0f, 100.0f, false);
  CHECK(s.inertia_ms() == doctest::Approx(500.0f).epsilon(0.01));
  CHECK(s.ahead() == 4.0f);
  CHECK(s.gate_slope() == 64.0f);
  CHECK_FALSE(s.aa());
  s.set_tuning(0.0f, -1.0f, 0.0f, true);
  CHECK(s.inertia_ms() == doctest::Approx(5.0f).epsilon(0.01));
  CHECK(s.ahead() == 0.0f);
  CHECK(s.gate_slope() == 1.0f);
  CHECK(s.aa());
  const float nan = std::nanf("");
  s.set_tuning(nan, nan, nan, true);
  CHECK(s.inertia_ms() == doctest::Approx(5.0f).epsilon(0.01));
  CHECK(s.ahead() == 0.0f);
  CHECK(s.gate_slope() == 1.0f);
}
