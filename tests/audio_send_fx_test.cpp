// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SENDFX: the send effects, SND A the delay (audio/send_delay.h) and SND B the reverb (audio/send_reverb.h), and their wiring in the graph. NEW COMMUNITY OS BEHAVIOUR: there is no oracle
// of the OG's sound, so these are PROPERTY tests (a sample-exact time, the channels of a ping-pong, a decay against its gain, bounded and finite at every corner, exact silence after the tail,
// no click when the time moves) plus a plain sample-by-sample reference of the delay's loop. The A/B listening of both is a hardware-session row.
#include <doctest/doctest.h>

#include <o3os/audio/graph.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/hal/audio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using o3os::audio::SendDelay;
using o3os::audio::SendReverb;
namespace sx = o3os::audio::sendfx;
namespace dl = o3os::audio::sendfx::delay;
namespace rv = o3os::audio::sendfx::reverb;

constexpr std::uint32_t kBlk = o3os::hal::audio::kBlock;
constexpr std::int32_t kA = 20000;  // the impulse

struct Lcg {
  std::uint32_t s = 12345u;
  std::uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
  std::int16_t sample() { return static_cast<std::int16_t>(next() & 0xFFFFu); }
};

// A delay on a ring of `frames` frames (the full 4 s one unless a test wants a small one).
struct DelayRig {
  explicit DelayRig(std::uint32_t frames = SendDelay::kRingFrames) : ring(frames) { d.attach(ring.data(), frames); }
  bool set(unsigned field, float v) { return d.set_param(field, v); }
  // Runs `in` through the delay in blocks of kBlk (the last one shorter), appending to l and r.
  void run(const std::vector<std::int16_t>& in) {
    std::int16_t ol[kBlk];
    std::int16_t orr[kBlk];
    for (std::size_t i = 0; i < in.size(); i += kBlk) {
      const std::uint32_t n = static_cast<std::uint32_t>(in.size() - i < kBlk ? in.size() - i : kBlk);
      d.process(in.data() + i, ol, orr, n);
      l.insert(l.end(), ol, ol + n);
      r.insert(r.end(), orr, orr + n);
    }
  }
  std::int64_t sum_after(std::size_t from) const {
    std::int64_t s = 0;
    for (std::size_t i = from; i < l.size(); ++i) s += std::abs(l[i]) + std::abs(r[i]);
    return s;
  }
  std::vector<std::uint32_t> ring;
  SendDelay d;
  std::vector<std::int16_t> l, r;
};

std::vector<std::int16_t> impulse(std::size_t frames, std::size_t at = 0, std::int16_t a = kA) {
  std::vector<std::int16_t> v(frames, 0);
  v[at] = a;
  return v;
}

// The oracle for a time in frames, exact integers: round(num / den).
std::uint32_t round_div(std::uint64_t num, std::uint64_t den) { return static_cast<std::uint32_t>((2u * num + den) / (2u * den)); }
// How far the exact quotient is from a tie (a half): a value near one is skipped, the float arithmetic of the effect may land on either side of it.
bool near_tie(std::uint64_t num, std::uint64_t den) {
  const std::uint64_t rem = (2u * num) % (2u * den);  // remainder of 2 num / 2 den, in 1/(2 den)
  const std::uint64_t half = den;                     // a tie is the remainder `den`
  const std::uint64_t d = rem > half ? rem - half : half - rem;
  return d * 1000u < den * 2u;  // within 0.002 of a frame
}
constexpr std::uint64_t kFsMhz = o3os::hal::audio::sample_rate_mhz();
// Free time: ms x fs / 1000 frames.
std::uint64_t free_num(unsigned ms) { return static_cast<std::uint64_t>(ms) * kFsMhz; }
constexpr std::uint64_t kFreeDen = 1000000u;
// Synced: rate96 / 96 steps, a step is 15 / bpm seconds: rate96 x 15 x 100 x fs / (96 x tempo_x100 x 1000)... with fs in millihertz: x 1500 x fs_mhz / (96000 x tempo_x100).
std::uint64_t sync_num(unsigned rate_idx, unsigned tempo_x100) {
  (void)tempo_x100;
  return static_cast<std::uint64_t>(sx::kDelayRates96[rate_idx]) * 1500u * kFsMhz;
}
std::uint64_t sync_den(unsigned tempo_x100) { return 96000u * static_cast<std::uint64_t>(tempo_x100); }

int at(const std::vector<std::int16_t>& v, std::size_t i) { return i < v.size() ? v[i] : 0; }

}  // namespace

// ---- The delay -----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("send delay: a synced time lands on the exact sample, for every rate and several tempos") {
  const unsigned tempos[] = {12000, 9337, 17441, 6000};  // BPM x 100
  unsigned checked = 0;
  for (const unsigned tempo : tempos) {
    for (unsigned rate = 0; rate < sx::kDelayRateCount; ++rate) {
      const std::uint64_t num = sync_num(rate, tempo);
      const std::uint64_t den = sync_den(tempo);
      if (near_tie(num, den)) {
        continue;
      }
      std::uint32_t want = round_div(num, den);
      want = want < SendDelay::kMinDelay ? SendDelay::kMinDelay : (want > SendDelay::kRingFrames - SendDelay::kMinDelay ? SendDelay::kRingFrames - SendDelay::kMinDelay : want);
      DelayRig r;
      CHECK(r.set(dl::Sync, 1.0f));
      CHECK(r.set(dl::Rate, static_cast<float>(rate)));
      CHECK(r.set(sx::kTempoField, static_cast<float>(tempo)));
      CHECK(r.set(dl::Feedback, 0.0f));
      const std::size_t frames = want + 3u * kBlk;
      r.run(impulse(frames));
      INFO("rate " << rate << " tempo " << tempo << " want " << want);
      CHECK(r.d.delay_left() == want);
      // the echo is exactly one sample, exactly at `want`, on both channels (MONO), and nothing else sounds
      CHECK(at(r.l, want) == kA);
      CHECK(at(r.r, want) == kA);
      std::int64_t other = 0;
      for (std::size_t i = 0; i < r.l.size(); ++i) {
        other += i == want ? 0 : (std::abs(r.l[i]) + std::abs(r.r[i]));
      }
      CHECK(other == 0);
      ++checked;
    }
  }
  CHECK(checked >= 60u);  // the tempos are chosen so that few combinations are skipped as ties
}

TEST_CASE("send delay: a free time in milliseconds is round(ms x fs / 1000) frames") {
  for (const unsigned ms : {1u, 2u, 10u, 333u, 500u, 1234u, 3500u}) {
    if (near_tie(free_num(ms), kFreeDen)) {
      continue;
    }
    const std::uint32_t want = round_div(free_num(ms), kFreeDen) < SendDelay::kMinDelay ? SendDelay::kMinDelay : round_div(free_num(ms), kFreeDen);
    DelayRig rig;
    CHECK(rig.set(dl::Time, static_cast<float>(ms)));
    CHECK(rig.set(dl::Feedback, 0.0f));
    rig.run(impulse(want + 2u * kBlk));
    INFO("ms " << ms << " want " << want);
    CHECK(rig.d.delay_left() == want);
    CHECK(at(rig.l, want) == kA);
    CHECK(at(rig.r, want) == kA);
    CHECK(at(rig.l, want - 1u) == 0);
    CHECK(at(rig.l, want + 1u) == 0);
  }
}

TEST_CASE("send delay: a time beyond the ring is held to the ring, a time below a block to a block") {
  DelayRig rig;
  CHECK(rig.set(dl::Sync, 1.0f));
  CHECK(rig.set(dl::Rate, 0.0f));  // 8 steps
  CHECK(rig.set(sx::kTempoField, 2000.0f));  // 20 BPM: 8 steps are 6 s
  rig.run(impulse(kBlk));
  CHECK(rig.d.delay_left() == SendDelay::kRingFrames - SendDelay::kMinDelay);
  DelayRig fast;
  CHECK(fast.set(dl::Sync, 1.0f));
  CHECK(fast.set(dl::Rate, 18.0f));  // 1/32 step
  CHECK(fast.set(sx::kTempoField, 100000.0f));  // 1000 BPM: 0.1 ms
  fast.run(impulse(kBlk));
  CHECK(fast.d.delay_left() == SendDelay::kMinDelay);
}

// OS-MIDIREC-P1 (maintainer decision 2026-11-10): the 78.7 KiB of the 768 KiB delay region that the 4.0 s ring leaves over (mem_map.h kDelayBase + kDelayRingBytes) holds the lane store. This
// is the proof that the delay never touches it: the ring is attached with exactly kRingFrames words, and with guard words around it no mode, time or wrap writes (or clears) a word outside.
TEST_CASE("send delay: no mode, time or wrap writes a word outside the ring (the delay region's spare belongs to the lane store)") {
  constexpr std::uint32_t kGuard = 4096u;
  constexpr std::uint32_t kPat = 0xA5C3A5C3u;
  static_assert(SendDelay::kRingBytes == SendDelay::kRingFrames * 4u, "the ring is kRingFrames words, nothing more");
  struct Cfg {
    float ping, mode, sync, time_ms, rate, tempo;
  };
  // Every mode (ping-pong, STEREO, MONO) at the longest free time, past the top (sync at 20 BPM, held to the ring) and at the shortest; 44.1 kHz and 44,117 Hz differ only in the frames of a ms.
  const Cfg cfgs[] = {{1, 0, 0, 3500.0f, 0, 0}, {0, 1, 0, 3500.0f, 0, 0}, {0, 0, 0, 3500.0f, 0, 0}, {1, 0, 1, 0, 0, 2000},
                      {0, 1, 1, 0, 0, 2000},    {0, 0, 1, 0, 0, 2000},    {1, 1, 0, 1.0f, 0, 0},    {0, 1, 0, 1.0f, 0, 0}};
  for (const Cfg& c : cfgs) {
    std::vector<std::uint32_t> mem(SendDelay::kRingFrames + 2u * kGuard, kPat);
    SendDelay d;
    d.attach(mem.data() + kGuard, SendDelay::kRingFrames);
    CHECK(d.set_param(dl::PingPong, c.ping));
    CHECK(d.set_param(dl::Mode, c.mode));
    CHECK(d.set_param(dl::Sync, c.sync));
    if (c.sync != 0.0f) {
      CHECK(d.set_param(dl::Rate, c.rate));
      CHECK(d.set_param(sx::kTempoField, c.tempo));
    } else {
      CHECK(d.set_param(dl::Time, c.time_ms));
    }
    CHECK(d.set_param(dl::Feedback, 100.0f));
    Lcg rng;
    std::int16_t in[kBlk];
    std::int16_t ol[kBlk];
    std::int16_t orr[kBlk];
    // Two and a half rings of loud noise (the write position wraps twice), then a clear (a project load).
    for (std::uint32_t done = 0; done < 5u * SendDelay::kRingFrames / 2u; done += kBlk) {
      for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = rng.sample();
      d.process(in, ol, orr, kBlk);
    }
    d.begin_clear();
    for (std::uint32_t done = 0; done < 2u * SendDelay::kRingFrames; done += kBlk) {
      for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = rng.sample();
      d.process(in, ol, orr, kBlk);
    }
    CHECK(d.delay_left() <= SendDelay::kRingFrames - SendDelay::kMinDelay);
    CHECK(d.delay_right() <= SendDelay::kRingFrames - SendDelay::kMinDelay);
    bool guards_intact = true;
    for (std::uint32_t i = 0; i < kGuard; ++i) {
      guards_intact = guards_intact && mem[i] == kPat && mem[kGuard + SendDelay::kRingFrames + i] == kPat;
    }
    CHECK(guards_intact);
  }
}

TEST_CASE("send delay: ping-pong alternates the channels, each repeat a feedback gain down") {
  DelayRig rig;
  CHECK(rig.set(dl::Time, 400.0f));
  CHECK(rig.set(dl::Feedback, 50.0f));
  CHECK(rig.set(dl::PingPong, 1.0f));
  const std::uint32_t d = round_div(free_num(400), kFreeDen);
  rig.run(impulse(7u * d));
  CHECK(rig.d.delay_left() == d);
  CHECK(rig.d.delay_right() == d);
  std::int32_t want = kA;
  for (unsigned k = 1; k <= 6; ++k) {
    const bool left = (k % 2u) == 1u;  // the first repeat is on the left
    const int peak = at(left ? rig.l : rig.r, k * d);
    const int other = at(left ? rig.r : rig.l, k * d);
    INFO("repeat " << k);
    CHECK(std::abs(peak - want) <= 2);
    CHECK(std::abs(other) <= 1);
    want = want / 2;  // the feedback is 0.5
  }
}

TEST_CASE("send delay: stereo puts the right repeat at 3/4 of the left, mono centres both; the repeats fall by the feedback") {
  const std::uint32_t d = round_div(free_num(500), kFreeDen);
  {
    DelayRig rig;
    CHECK(rig.set(dl::Time, 500.0f));
    CHECK(rig.set(dl::Feedback, 0.0f));
    CHECK(rig.set(dl::Mode, 1.0f));  // STEREO
    rig.run(impulse(d + 2u * kBlk));
    const std::uint32_t dr = (3u * d + 2u) >> 2;
    CHECK(rig.d.delay_left() == d);
    CHECK(rig.d.delay_right() == dr);
    CHECK(at(rig.l, d) == kA);
    CHECK(at(rig.r, dr) == kA);
    CHECK(at(rig.l, dr) == 0);
    CHECK(at(rig.r, d) == 0);
  }
  {
    DelayRig rig;  // MONO, feedback 50 %: the repeats are A, A/2, A/4 ... on both channels
    CHECK(rig.set(dl::Time, 500.0f));
    CHECK(rig.set(dl::Feedback, 50.0f));
    rig.run(impulse(5u * d));
    std::int32_t want = kA;
    for (unsigned k = 1; k <= 4; ++k) {
      INFO("repeat " << k);
      CHECK(std::abs(at(rig.l, k * d) - want) <= 2);
      CHECK(rig.l[k * d] == rig.r[k * d]);
      want = want / 2;
    }
  }
}

TEST_CASE("send delay: the tone darkens the repeats and leaves the first one's timing alone") {
  DelayRig open;
  DelayRig dark;
  for (DelayRig* r : {&open, &dark}) {
    CHECK(r->set(dl::Time, 100.0f));
    CHECK(r->set(dl::Feedback, 60.0f));
  }
  CHECK(dark.set(dl::Tone, 0.0f));
  const std::uint32_t d = round_div(free_num(100), kFreeDen);
  open.run(impulse(4u * d));
  dark.run(impulse(4u * d));
  // an impulse through the low-pass keeps its sum and loses its peak: the second repeat of the dark one is lower and wider
  CHECK(std::abs(at(open.l, 2u * d)) > std::abs(at(dark.l, 2u * d)));
  CHECK(at(open.l, d) == kA);  // the first repeat is not in the loop yet
  CHECK(at(dark.l, d) == kA);
}

TEST_CASE("send delay: a time that moves cross-fades, no sample-to-sample jump and the new time exactly afterwards") {
  DelayRig rig;
  CHECK(rig.set(dl::Time, 100.0f));
  CHECK(rig.set(dl::Feedback, 0.0f));
  const std::uint32_t d1 = round_div(free_num(100), kFreeDen);
  const std::uint32_t d2 = round_div(free_num(150), kFreeDen);
  const std::size_t total = 40000;
  std::vector<std::int16_t> in(total);
  for (std::size_t i = 0; i < total; ++i) {
    in[i] = static_cast<std::int16_t>(std::lround(10000.0 * std::sin(2.0 * 3.14159265358979 * 441.0 * static_cast<double>(i) / 44117.647)));
  }
  const std::size_t change = 125u * kBlk;  // 8000 frames: block-aligned, past the first delay (4412), the sine is flowing
  std::vector<std::int16_t> head(in.begin(), in.begin() + change);
  std::vector<std::int16_t> tail(in.begin() + change, in.end());
  rig.run(head);
  CHECK_FALSE(rig.d.fading());
  CHECK(rig.set(dl::Time, 150.0f));
  rig.run(tail);
  CHECK(rig.d.delay_left() == d2);
  // the slope of a 10000-amplitude 441 Hz sine is at most 628 per frame; the cross-fade is a linear pair of gains that sum to 1, so it adds only the slope of the ramp (about 20), where a hard
  // switch would jump by thousands
  int worst = 0;
  for (std::size_t i = d1 + 1; i < rig.l.size(); ++i) {
    worst = std::max(worst, std::abs(rig.l[i] - rig.l[i - 1]));
  }
  CHECK(worst < 700);
  // after the fade (1024 frames from the first block that saw the change) the output is the input, d2 frames late, bit for bit
  std::size_t wrong = 0;
  for (std::size_t i = change + SendDelay::kXfade + 2u * kBlk; i < total; ++i) {
    wrong += rig.l[i] == in[i - d2] ? 0u : 1u;
  }
  CHECK(wrong == 0u);
}

TEST_CASE("send delay: a cross-fade never swells a correlated signal: the gains sum to 1, so a constant stays a constant (and a loop through it keeps its gain below unity)") {
  DelayRig rig(8192);
  CHECK(rig.set(dl::Time, 20.0f));
  CHECK(rig.set(dl::Feedback, 0.0f));
  rig.run(std::vector<std::int16_t>(8u * kBlk * 8u, 8000));  // the constant has filled both times' taps
  const std::size_t from = rig.l.size();
  CHECK(rig.set(dl::Time, 30.0f));
  rig.run(std::vector<std::int16_t>(6000, 8000));
  int peak = 0;
  for (std::size_t i = from; i < rig.l.size(); ++i) peak = std::max(peak, std::max(std::abs(rig.l[i]), std::abs(rig.r[i])));
  CHECK(rig.d.delay_left() == round_div(free_num(30), kFreeDen));
  CHECK(peak == 8000);  // the old equal-power pair swelled it to 11,000 in the middle of the fade
  CHECK(rig.d.finite());
}

TEST_CASE("send delay: a clear forgets the ring a slice a block, the return silent meanwhile, then the delay is as new (a project load)") {
  {
    DelayRig rig(4096);
    CHECK(rig.set(dl::Time, 11.0f));
    CHECK(rig.set(dl::Feedback, 90.0f));
    rig.run(impulse(3000));
    CHECK_FALSE(rig.d.idle());
    CHECK_FALSE(rig.set(sx::kClearField, 0.0f));  // only 1 asks for it
    CHECK(rig.set(sx::kClearField, 1.0f));
    CHECK(rig.d.clearing());
    const std::size_t from = rig.l.size();
    rig.run(std::vector<std::int16_t>(2u * kBlk * 16u, 0));  // two slices of 2,048 frames are the 4,096
    CHECK_FALSE(rig.d.clearing());
    CHECK(rig.d.idle());
    CHECK(rig.sum_after(from) == 0);
    // nothing of the old tail remains, and the delay works as new
    rig.run(std::vector<std::int16_t>(20000, 0));
    CHECK(rig.sum_after(from) == 0);
    const std::size_t at = rig.l.size();
    std::vector<std::int16_t> again(3u * 485u, 0);
    again[7] = kA;
    rig.run(again);
    CHECK(rig.l[at + 7 + round_div(free_num(11), kFreeDen)] == kA);
  }
  {
    // a block clears 2,048 frames of the 176,468 and no more: a marker far away is still there after one block, gone after the last
    DelayRig rig;
    std::int16_t in[kBlk] = {};
    std::int16_t l[kBlk], r[kBlk];
    in[0] = kA;
    rig.d.process(in, l, r, kBlk);  // awake: a sleeping delay has nothing to clear
    in[0] = 0;
    CHECK_FALSE(rig.d.idle());
    rig.ring[100000] = 0x00010002u;
    CHECK(rig.set(sx::kClearField, 1.0f));
    rig.d.process(in, l, r, kBlk);
    CHECK(rig.ring[100000] == 0x00010002u);
    CHECK(rig.d.clearing());
    unsigned blocks = 1;
    while (rig.d.clearing() && blocks < 200u) {
      rig.d.process(in, l, r, kBlk);
      ++blocks;
    }
    CHECK(blocks == (SendDelay::kRingFrames + SendDelay::kClearFrames - 1u) / SendDelay::kClearFrames);
    CHECK(rig.ring[100000] == 0u);
  }
}

TEST_CASE("send delay: a tempo change while synced moves the time by a cross-fade; a second change waits for the first") {
  DelayRig rig;
  CHECK(rig.set(dl::Sync, 1.0f));
  CHECK(rig.set(dl::Rate, 11.0f));  // 1/4 step... see kDelayRates96: 24/96
  CHECK(rig.set(sx::kTempoField, 12000.0f));
  CHECK(rig.set(dl::Feedback, 0.0f));
  Lcg rng;
  std::vector<std::int16_t> noise(8u * kBlk);
  for (auto& v : noise) v = rng.sample();
  rig.run(noise);
  const std::uint32_t d120 = rig.d.delay_left();
  CHECK(rig.set(sx::kTempoField, 14000.0f));
  rig.run(std::vector<std::int16_t>(kBlk, 0));
  CHECK(rig.d.fading());
  CHECK(rig.set(sx::kTempoField, 9000.0f));  // while the fade runs
  rig.run(std::vector<std::int16_t>(kBlk, 0));
  CHECK(rig.d.fading());
  CHECK(rig.d.delay_left() != d120);
  // the fade of the first change ends, the second starts: after both, the time is the last one asked for
  rig.run(std::vector<std::int16_t>(4u * SendDelay::kXfade, 0));
  const std::uint64_t num = sync_num(11, 9000);
  CHECK(rig.d.delay_left() == round_div(num, sync_den(9000)));
  CHECK_FALSE(rig.d.fading());
}

TEST_CASE("send delay: out-of-range and non-numeric parameters are refused and change nothing") {
  DelayRig rig;
  CHECK_FALSE(rig.set(dl::Time, 0.0f));
  CHECK_FALSE(rig.set(dl::Time, 3501.0f));
  CHECK_FALSE(rig.set(dl::Feedback, -1.0f));
  CHECK_FALSE(rig.set(dl::Feedback, 100.5f));
  CHECK_FALSE(rig.set(dl::Sync, 0.5f));
  CHECK_FALSE(rig.set(dl::Rate, 19.0f));
  CHECK_FALSE(rig.set(dl::PingPong, 2.0f));
  CHECK_FALSE(rig.set(dl::Tone, 101.0f));
  CHECK_FALSE(rig.set(dl::Mode, -1.0f));
  CHECK_FALSE(rig.set(sx::kTempoField, 999.0f));
  CHECK_FALSE(rig.set(0x7u, 1.0f));  // not a field
  CHECK_FALSE(rig.set(dl::Time, std::nanf("")));
  CHECK_FALSE(rig.set(dl::Feedback, std::nanf("")));
  CHECK(rig.set(dl::Time, 3500.0f));
  CHECK(rig.set(dl::Feedback, 100.0f));
  CHECK(rig.set(dl::Rate, 18.0f));
  // the default time still stands where a refusal left it
  DelayRig fresh;
  fresh.run(impulse(30000));
  CHECK(fresh.d.delay_left() == round_div(free_num(500), kFreeDen));
}

TEST_CASE("send delay: the loop is bounded and finite at the top feedback, in every mode, with loud noise; the tail then ends in exact silence") {
  const std::uint32_t frames = 4096;
  for (unsigned mode = 0; mode < 3; ++mode) {
    for (const float tone : {0.0f, 100.0f}) {
      DelayRig rig(frames);
      CHECK(rig.set(dl::Time, 4.0f));  // 176 frames: many passes a second
      CHECK(rig.set(dl::Feedback, 100.0f));
      CHECK(rig.set(dl::Tone, tone));
      CHECK(rig.set(dl::PingPong, mode == 2u ? 1.0f : 0.0f));
      CHECK(rig.set(dl::Mode, mode == 1u ? 1.0f : 0.0f));
      Lcg rng;
      std::vector<std::int16_t> loud(3u * 44100u);
      for (auto& v : loud) v = rng.sample();
      rig.run(loud);
      CHECK(rig.d.finite());
      // the tail: input stops, the loop's gain is 0.98 a pass, 176 frames a pass: well under 6 s to zero
      const std::size_t before = rig.l.size();
      rig.run(std::vector<std::int16_t>(8u * 44100u, 0));
      CHECK(rig.d.finite());
      std::int64_t last_second = 0;
      for (std::size_t i = rig.l.size() - 44100u; i < rig.l.size(); ++i) {
        last_second += std::abs(rig.l[i]) + std::abs(rig.r[i]);
      }
      INFO("mode " << mode << " tone " << tone << " (" << before << " frames of input)");
      CHECK(last_second == 0);
      CHECK(rig.d.idle());
    }
  }
}

TEST_CASE("send delay: the DC blocker keeps a constant from piling up in the loop") {
  // Without it a DC input of 8000 at a feedback of 0.9 would settle at 8000 / (1 - 0.9) = 80000 and clip; with it the repeats of a constant die and the output is the input, delayed.
  DelayRig rig(4096);
  CHECK(rig.set(dl::Time, 10.0f));  // 441 frames
  CHECK(rig.set(dl::Feedback, 90.0f));
  rig.run(std::vector<std::int16_t>(2u * 44100u, 8000));
  CHECK(rig.d.finite());
  CHECK(std::abs(rig.l.back() - 8000) <= 80);
  CHECK(std::abs(rig.r.back() - 8000) <= 80);
}

TEST_CASE("send delay: a loop that is driven past full scale saturates instead of wrapping") {
  // A Nyquist square wave (+32767, -32767, ...) through a delay of an even number of frames comes back in phase with itself every pass: at the top feedback the loop builds up until it
  // clips. The words it writes are clamped, so the output ends as the clipped wave (every sample at the rails), not as a wrapped one (small and wild values).
  DelayRig rig(4096);
  CHECK(rig.set(dl::Time, 4.0f));  // 176 frames, even
  CHECK(rig.set(dl::Feedback, 100.0f));
  std::vector<std::int16_t> in(44100);
  for (std::size_t i = 0; i < in.size(); ++i) {
    in[i] = (i & 1u) == 0u ? 32767 : -32767;
  }
  rig.run(in);
  CHECK(rig.d.finite());
  std::size_t not_clipped = 0;
  for (std::size_t i = in.size() - 2000u; i < in.size(); ++i) {
    not_clipped += std::abs(rig.l[i]) >= 32767 && ((rig.l[i] > 0) == ((i & 1u) == 0u)) ? 0u : 1u;
  }
  CHECK(not_clipped == 0u);
}

TEST_CASE("send delay: after its tail it sleeps, and wakes on the next sound with the time still exact") {
  const std::uint32_t frames = 4096;
  DelayRig rig(frames);
  CHECK(rig.set(dl::Time, 11.0f));  // 485 frames
  CHECK(rig.set(dl::Feedback, 50.0f));
  const std::uint32_t d = round_div(free_num(11), kFreeDen);
  rig.run(impulse(2000));
  CHECK_FALSE(rig.d.idle());
  rig.run(std::vector<std::int16_t>(20000, 0));  // the repeats halve each pass: 15 passes to nothing, then a ring's worth of zeros
  CHECK(rig.d.idle());
  const std::size_t at_wake = rig.l.size();
  std::vector<std::int16_t> second(3u * d, 0);
  second[7] = kA;
  rig.run(second);
  CHECK_FALSE(rig.d.idle());
  CHECK(rig.l[at_wake + 7 + d] == kA);
  CHECK(rig.r[at_wake + 7 + d] == kA);
}

TEST_CASE("send delay: against a sample-by-sample reference of the same loop, over ring wraps, in each mode") {
  const std::uint32_t frames = 1500;  // not a multiple of 64: the staging splits at the wrap
  for (unsigned mode = 0; mode < 3; ++mode) {
    DelayRig rig(frames);
    const std::uint32_t d = 701;  // frames; set through the time: 701 / 44.117647 ms
    const float ms = 701.0f / (static_cast<float>(kFsMhz) * 1.0e-6f);
    CHECK(rig.set(dl::Time, ms));
    CHECK(rig.set(dl::Feedback, 55.0f));
    CHECK(rig.set(dl::Tone, 60.0f));
    CHECK(rig.set(dl::PingPong, mode == 2u ? 1.0f : 0.0f));
    CHECK(rig.set(dl::Mode, mode == 1u ? 1.0f : 0.0f));
    Lcg rng;
    std::vector<std::int16_t> in(40u * kBlk + 13u);  // a short last block too
    for (std::size_t i = 0; i < in.size(); ++i) {
      in[i] = i < 20u * kBlk ? static_cast<std::int16_t>(rng.sample() / 4) : static_cast<std::int16_t>(0);
    }
    rig.run(in);
    const std::uint32_t dl_frames = rig.d.delay_left();
    const std::uint32_t dr_frames = rig.d.delay_right();
    CHECK(dl_frames == d);
    // the reference: a ring of frames (l, r), the same arithmetic one sample at a time
    std::vector<std::int32_t> rl(frames, 0), rr(frames, 0);
    const float u = 60.0f * 0.01f;
    const float a = 0.04f + 0.96f * u * u;
    const float g = 55.0f * 0.01f;
    float lpl = 0, lpr = 0, xl1 = 0, xr1 = 0, yl1 = 0, yr1 = 0;
    std::int64_t worst = 0;
    for (std::size_t t = 0; t < in.size(); ++t) {
      const std::uint32_t pos = static_cast<std::uint32_t>(t % frames);
      const std::uint32_t pl = (pos + frames - dl_frames) % frames;
      const std::uint32_t pr = (pos + frames - dr_frames) % frames;
      const float lo = static_cast<float>(rl[pl]);
      const float ro = static_cast<float>(rr[pr]);
      worst = std::max<std::int64_t>(worst, std::abs(rig.l[t] - static_cast<std::int32_t>(lo)));
      worst = std::max<std::int64_t>(worst, std::abs(rig.r[t] - static_cast<std::int32_t>(ro)));
      lpl += a * (lo - lpl);
      lpr += a * (ro - lpr);
      const float yl = lpl - xl1 + 0.995f * yl1;
      const float yr = lpr - xr1 + 0.995f * yr1;
      xl1 = lpl;
      xr1 = lpr;
      yl1 = yl;
      yr1 = yr;
      const float fl = g * yl;
      const float fr = g * yr;
      const float x = static_cast<float>(in[t]);
      const float wl = mode == 2u ? x + fr : x + fl;
      const float wr = mode == 2u ? fl : x + fr;
      rl[pos] = static_cast<std::int32_t>(std::fmax(-32768.0f, std::fmin(32767.0f, wl)));
      rr[pos] = static_cast<std::int32_t>(std::fmax(-32768.0f, std::fmin(32767.0f, wr)));
    }
    INFO("mode " << mode);
    CHECK(worst <= 3);  // identical arithmetic: the allowance is for a fused multiply-add on a target that has one
  }
}

TEST_CASE("send delay: no ring, no sound; a ring too small is no ring") {
  SendDelay d;
  std::int16_t in[kBlk] = {1, 2, 3};
  std::int16_t l[kBlk], r[kBlk];
  d.process(in, l, r, kBlk);
  for (std::uint32_t i = 0; i < kBlk; ++i) {
    CHECK(l[i] == 0);
    CHECK(r[i] == 0);
  }
  std::vector<std::uint32_t> tiny(100);
  d.attach(tiny.data(), static_cast<std::uint32_t>(tiny.size()));
  d.process(in, l, r, kBlk);
  CHECK(l[1] == 0);
}

TEST_CASE("send delay: a fuzz of parameters and loud noise stays finite") {
  DelayRig rig(8192);
  Lcg rng;
  std::int16_t in[kBlk];
  std::int16_t l[kBlk], r[kBlk];
  for (unsigned blk = 0; blk < 3000; ++blk) {
    if (rng.next() % 7u == 0u) {
      const unsigned which = rng.next() % 8u;
      const float t = static_cast<float>(rng.next() % 1001u) * 0.001f;
      switch (which) {
        case 0: rig.set(dl::Time, 1.0f + 60.0f * t); break;
        case 1: rig.set(dl::Feedback, 100.0f * t); break;
        case 2: rig.set(dl::Sync, t > 0.5f ? 1.0f : 0.0f); break;
        case 3: rig.set(dl::Rate, std::floor(18.99f * t)); break;
        case 4: rig.set(dl::PingPong, t > 0.5f ? 1.0f : 0.0f); break;
        case 5: rig.set(dl::Tone, 100.0f * t); break;
        case 6: rig.set(dl::Mode, t > 0.5f ? 1.0f : 0.0f); break;
        default: rig.set(sx::kTempoField, 2000.0f + 30000.0f * t); break;
      }
    }
    const bool silent = (blk / 200u) % 3u == 2u;
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      in[i] = silent ? 0 : rng.sample();
    }
    rig.d.process(in, l, r, kBlk);
    if (!rig.d.finite()) {
      CHECK(rig.d.finite());  // (one report, at the block that broke it)
      break;
    }
  }
}

// ---- The reverb ----------------------------------------------------------------------------------------------------------------------------------

namespace {

constexpr double kFsHz = 44117.647;

struct ReverbRig {
  ReverbRig() : mem(SendReverb::kMemSamples) { r.attach(mem.data(), static_cast<std::uint32_t>(mem.size())); }
  bool set(unsigned field, float v) { return r.set_param(field, v); }
  void run(const std::vector<std::int16_t>& in) {
    std::int16_t ol[kBlk];
    std::int16_t orr[kBlk];
    for (std::size_t i = 0; i < in.size(); i += kBlk) {
      const std::uint32_t n = static_cast<std::uint32_t>(in.size() - i < kBlk ? in.size() - i : kBlk);
      r.process(in.data() + i, ol, orr, n);
      l.insert(l.end(), ol, ol + n);
      rr.insert(rr.end(), orr, orr + n);
    }
  }
  // `seconds` of input made by `fill(i)` (the frame index), in whole blocks
  template <typename Fn>
  void run_seconds(double seconds, Fn fill) {
    const std::size_t frames = (static_cast<std::size_t>(seconds * kFsHz) / kBlk) * kBlk;
    std::vector<std::int16_t> in(frames);
    const std::size_t base = l.size();
    for (std::size_t i = 0; i < frames; ++i) in[i] = fill(base + i);
    run(in);
  }
  std::vector<std::int16_t> mem;
  SendReverb r;
  std::vector<std::int16_t> l, rr;
};

double energy(const std::vector<std::int16_t>& a, const std::vector<std::int16_t>& b, std::size_t from, std::size_t to) {
  double e = 0;
  for (std::size_t i = from; i < to && i < a.size(); ++i) {
    e += static_cast<double>(a[i]) * a[i] + static_cast<double>(b[i]) * b[i];
  }
  return e;
}
std::size_t at_s(double seconds) { return static_cast<std::size_t>(seconds * kFsHz); }

std::int16_t noise_at(Lcg& rng, int divisor) { return static_cast<std::int16_t>(rng.sample() / divisor); }

}  // namespace

TEST_CASE("send reverb: the memory is what the header says and the lines fit the tank") {
  CHECK(SendReverb::kMemSamples == 26632u);
  CHECK(SendReverb::kMemBytes == 53264u);
  std::uint32_t sum = 0;
  for (const std::uint16_t len : SendReverb::kRingFull) sum += (len + 1u) & ~1u;  // each line starts on a word boundary
  CHECK(sum == SendReverb::kMemSamples);
}

TEST_CASE("send reverb: an impulse gives a finite, bounded, decaying response at every corner, in both qualities") {
  for (unsigned q = 0; q < 2; ++q) {
    for (const float size : {0.0f, 50.0f, 100.0f}) {
      for (const float damp : {0.0f, 100.0f}) {
        ReverbRig rig;
        CHECK(rig.set(rv::Size, size));
        CHECK(rig.set(rv::Damp, damp));
        CHECK(rig.set(rv::Quality, static_cast<float>(q)));
        std::vector<std::int16_t> in(at_s(8.0) / kBlk * kBlk, 0);
        in[0] = 30000;
        rig.run(in);
        INFO("quality " << q << " size " << size << " damp " << damp);
        CHECK(rig.r.finite());
        CHECK(rig.r.half_rate() == (q == 0u));
        const double e_early = energy(rig.l, rig.rr, at_s(0.1), at_s(0.6));
        const double e_late = energy(rig.l, rig.rr, at_s(2.0), at_s(2.5));
        CHECK(e_early > 1.0e6);        // it sounds
        CHECK(e_late < e_early);       // and decays
        // every second of the tail carries no more than the one before it (after the build-up of the first 0.6 s)
        double prev = energy(rig.l, rig.rr, at_s(0.6), at_s(1.6));
        for (unsigned sec = 2; sec < 8; ++sec) {
          const double now = energy(rig.l, rig.rr, at_s(static_cast<double>(sec) - 0.4), at_s(static_cast<double>(sec) + 0.6));
          CHECK(now <= prev * 1.001);
          prev = now;
        }
      }
    }
  }
}

TEST_CASE("send reverb: silence in gives silence out after the tail, and the effect then sleeps, at every corner") {
  for (unsigned q = 0; q < 2; ++q) {
    for (const float size : {0.0f, 100.0f}) {
      for (const float damp : {0.0f, 100.0f}) {
        ReverbRig rig;
        CHECK(rig.set(rv::Size, size));
        CHECK(rig.set(rv::Damp, damp));
        CHECK(rig.set(rv::Diffusion, 100.0f));
        CHECK(rig.set(rv::Quality, static_cast<float>(q)));
        Lcg rng;
        rig.run_seconds(0.5, [&](std::size_t) { return noise_at(rng, 4); });
        CHECK_FALSE(rig.r.idle());
        std::size_t slept_at = 0;
        for (unsigned step = 0; step < 80 && !rig.r.idle(); ++step) {  // up to 80 s of silence, a second at a time
          rig.run_seconds(1.0, [](std::size_t) { return std::int16_t{0}; });
          slept_at = rig.l.size();
        }
        INFO("quality " << q << " size " << size << " damp " << damp);
        CHECK(rig.r.idle());
        CHECK(rig.r.finite());
        // from the moment it sleeps the outputs are zero; and a second of more silence stays zero
        const std::size_t before = rig.l.size();
        rig.run_seconds(1.0, [](std::size_t) { return std::int16_t{0}; });
        CHECK(energy(rig.l, rig.rr, before, rig.l.size()) == 0.0);
        CHECK(slept_at > 0u);
      }
    }
  }
}

TEST_CASE("send reverb: loud noise at the loosest settings stays finite and the tail still ends in silence") {
  for (unsigned q = 0; q < 2; ++q) {
    ReverbRig rig;
    CHECK(rig.set(rv::Size, 100.0f));
    CHECK(rig.set(rv::Damp, 0.0f));
    CHECK(rig.set(rv::Diffusion, 100.0f));
    CHECK(rig.set(rv::Quality, static_cast<float>(q)));
    Lcg rng;
    rig.run_seconds(6.0, [&](std::size_t) { return noise_at(rng, 1); });  // full scale
    CHECK(rig.r.finite());
    for (unsigned step = 0; step < 90 && !rig.r.idle(); ++step) {
      rig.run_seconds(1.0, [](std::size_t) { return std::int16_t{0}; });
    }
    INFO("quality " << q);
    CHECK(rig.r.idle());
  }
}

TEST_CASE("send reverb: SIZE sets the decay, DAMP the colour of the tail, DIFFUSION the density, PREDELAY the wait") {
  // SIZE: the level of the tail 1.5 s after a noise burst, against the level at the end of the burst, rises with the size
  double prev_ratio = -1.0;
  for (const float size : {0.0f, 25.0f, 50.0f, 75.0f, 100.0f}) {
    ReverbRig rig;
    CHECK(rig.set(rv::Size, size));
    Lcg rng;
    rig.run_seconds(1.0, [&](std::size_t) { return noise_at(rng, 4); });
    rig.run_seconds(2.0, [](std::size_t) { return std::int16_t{0}; });
    const double during = energy(rig.l, rig.rr, at_s(0.7), at_s(1.0));
    const double after = energy(rig.l, rig.rr, at_s(2.2), at_s(2.5));
    const double ratio = after / during;
    INFO("size " << size << " ratio " << ratio);
    CHECK(ratio > prev_ratio);
    prev_ratio = ratio;
  }
  // DAMP: the share of high frequencies (the energy of the first difference over the energy) of the tail falls as DAMP rises
  double hf_open = 0, hf_dark = 0;
  for (const float damp : {0.0f, 100.0f}) {
    ReverbRig rig;
    CHECK(rig.set(rv::Damp, damp));
    CHECK(rig.set(rv::Quality, 1.0f));
    Lcg rng;
    rig.run_seconds(1.0, [&](std::size_t) { return noise_at(rng, 4); });
    rig.run_seconds(1.5, [](std::size_t) { return std::int16_t{0}; });
    double hf = 0, tot = 0;
    for (std::size_t i = at_s(1.3) + 1u; i < at_s(1.8); ++i) {
      const double d = static_cast<double>(rig.l[i]) - rig.l[i - 1];
      hf += d * d;
      tot += static_cast<double>(rig.l[i]) * rig.l[i];
    }
    (damp == 0.0f ? hf_open : hf_dark) = hf / tot;
  }
  CHECK(hf_dark < 0.5 * hf_open);
  // DIFFUSION: more of the first 150 ms is filled in
  std::size_t filled[2] = {0, 0};
  for (unsigned k = 0; k < 2; ++k) {
    ReverbRig rig;
    CHECK(rig.set(rv::Diffusion, k == 0u ? 0.0f : 100.0f));
    CHECK(rig.set(rv::Predelay, 0.0f));
    CHECK(rig.set(rv::Quality, 1.0f));
    std::vector<std::int16_t> in(at_s(0.4) / kBlk * kBlk, 0);
    in[0] = 30000;
    rig.run(in);
    for (std::size_t i = 0; i < at_s(0.15); ++i) filled[k] += (rig.l[i] != 0 || rig.rr[i] != 0) ? 1u : 0u;
  }
  CHECK(filled[1] > 3u * filled[0]);
  // PREDELAY: the first sound comes later by the difference, 4,032 frames, whatever the rate
  for (unsigned q = 0; q < 2; ++q) {
    std::size_t first[2] = {0, 0};
    for (unsigned k = 0; k < 2; ++k) {
      ReverbRig rig;
      CHECK(rig.set(rv::Predelay, k == 0u ? 0.0f : 100.0f));
      CHECK(rig.set(rv::Quality, static_cast<float>(q)));
      std::vector<std::int16_t> in(at_s(0.4) / kBlk * kBlk, 0);
      in[5] = 30000;
      rig.run(in);
      while (first[k] < rig.l.size() && rig.l[first[k]] == 0 && rig.rr[first[k]] == 0) ++first[k];
    }
    INFO("quality " << q << " first " << first[0] << " " << first[1]);
    CHECK(first[1] > first[0]);
    CHECK(std::abs(static_cast<long>(first[1] - first[0]) - 4032L) <= 80);
  }
}

TEST_CASE("send reverb: the two returns are decorrelated, and HALF and FULL agree in level on a tone") {
  ReverbRig rig;
  CHECK(rig.set(rv::Quality, 1.0f));
  Lcg rng;
  rig.run_seconds(3.0, [&](std::size_t) { return noise_at(rng, 4); });
  double ll = 0, rrr = 0, lr = 0;
  for (std::size_t i = at_s(1.0); i < rig.l.size(); ++i) {
    ll += static_cast<double>(rig.l[i]) * rig.l[i];
    rrr += static_cast<double>(rig.rr[i]) * rig.rr[i];
    lr += static_cast<double>(rig.l[i]) * rig.rr[i];
  }
  CHECK(std::abs(lr) / std::sqrt(ll * rrr) < 0.3);
  double level[2] = {0, 0};
  for (unsigned q = 0; q < 2; ++q) {
    ReverbRig t;
    CHECK(t.set(rv::Quality, static_cast<float>(q)));
    t.run_seconds(4.0, [](std::size_t i) { return static_cast<std::int16_t>(std::lround(8000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(i) / kFsHz))); });
    level[q] = energy(t.l, t.rr, at_s(2.0), at_s(4.0));
  }
  const double db = 10.0 * std::log10(level[0] / level[1]);
  CHECK(std::abs(db) < 1.5);
}

TEST_CASE("send reverb: a change of quality repartitions the lines and clears the tail; a short block is a padded one; two runs agree") {
  ReverbRig rig;
  Lcg rng;
  rig.run_seconds(0.5, [&](std::size_t) { return noise_at(rng, 4); });
  CHECK(rig.r.half_rate());
  CHECK(rig.set(rv::Quality, 1.0f));
  const std::size_t before = rig.l.size();
  rig.run_seconds(0.1, [](std::size_t) { return std::int16_t{0}; });
  CHECK_FALSE(rig.r.half_rate());
  CHECK(energy(rig.l, rig.rr, before, rig.l.size()) == 0.0);  // the old tail is gone
  CHECK(rig.r.finite());
  std::vector<std::int16_t> in(at_s(0.3) / kBlk * kBlk, 0);
  in[0] = 20000;
  rig.run(in);
  CHECK(energy(rig.l, rig.rr, before + at_s(0.1), rig.l.size()) > 1.0e6);  // and it works again
  // determinism: the same input through two fresh instances
  ReverbRig a, b;
  Lcg r1, r2;
  a.run_seconds(1.0, [&](std::size_t) { return noise_at(r1, 3); });
  b.run_seconds(1.0, [&](std::size_t) { return noise_at(r2, 3); });
  CHECK(a.l == b.l);
  CHECK(a.rr == b.rr);
  // a block shorter than a block: the same as the start of a block padded with zeros
  ReverbRig full, part;
  std::vector<std::int16_t> blk(kBlk, 0);
  for (std::uint32_t i = 0; i < 17; ++i) blk[i] = static_cast<std::int16_t>(1000 + 37 * i);
  full.run(blk);
  part.run(std::vector<std::int16_t>(blk.begin(), blk.begin() + 17));
  CHECK(part.l.size() == 17u);
  CHECK(std::equal(part.l.begin(), part.l.end(), full.l.begin()));
  CHECK(std::equal(part.rr.begin(), part.rr.end(), full.rr.begin()));
}

TEST_CASE("send reverb: a change of quality or a clear forgets the lines a slice a block, the return silent meanwhile (no glitch from one block's work)") {
  for (unsigned how = 0; how < 2; ++how) {  // 0: a change of quality, 1: the clear of a project load
    ReverbRig rig;
    Lcg rng;
    rig.run_seconds(0.5, [&](std::size_t) { return noise_at(rng, 4); });
    CHECK_FALSE(rig.r.idle());
    rig.mem[20000] = 77;  // far into the memory (past the first slice)
    if (how == 0u) {
      CHECK(rig.set(rv::Quality, 1.0f));
    } else {
      CHECK_FALSE(rig.set(sx::kClearField, 0.0f));
      CHECK(rig.set(sx::kClearField, 1.0f));
    }
    std::int16_t in[kBlk];
    std::int16_t l[kBlk], r[kBlk];
    for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = noise_at(rng, 4);  // the send keeps coming: the return stays silent while the lines are cleared
    rig.r.process(in, l, r, kBlk);
    INFO("how " << how);
    CHECK(rig.r.clearing());
    CHECK(rig.mem[20000] == 77);  // one block cleared one slice only
    for (std::uint32_t i = 0; i < kBlk; ++i) CHECK((l[i] == 0 && r[i] == 0));
    unsigned blocks = 1;
    while (rig.r.clearing() && blocks < 100u) {
      rig.r.process(in, l, r, kBlk);
      ++blocks;
    }
    CHECK(blocks == (SendReverb::kMemSamples + SendReverb::kClearSamples - 1u) / SendReverb::kClearSamples);
    CHECK(rig.mem[20000] == 0);
    CHECK(rig.r.idle());
    CHECK(rig.r.half_rate() == (how == 1u));
    // and it sounds again: noise in, a return out
    double e = 0;
    for (unsigned b = 0; b < 100; ++b) {
      for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = noise_at(rng, 4);
      rig.r.process(in, l, r, kBlk);
      for (std::uint32_t i = 0; i < kBlk; ++i) e += static_cast<double>(l[i]) * l[i];
    }
    CHECK(e > 1.0e6);
  }
}

TEST_CASE("send reverb: a sleeping plate has nothing to clear: a change of quality or a clear is at once, no muted blocks") {
  ReverbRig rig;
  CHECK(rig.r.idle());
  CHECK(rig.set(sx::kClearField, 1.0f));
  CHECK_FALSE(rig.r.clearing());
  CHECK(rig.set(rv::Quality, 1.0f));
  std::vector<std::int16_t> in(kBlk, 0);
  in[0] = 20000;
  rig.run(in);  // the very first block already sounds (an impulse: nothing yet at the outputs, but the plate runs)
  CHECK_FALSE(rig.r.clearing());
  CHECK_FALSE(rig.r.half_rate());
  CHECK_FALSE(rig.r.idle());
  DelayRig dly;
  CHECK(dly.d.idle());
  CHECK(dly.set(sx::kClearField, 1.0f));
  CHECK_FALSE(dly.d.clearing());
}

TEST_CASE("send reverb: out-of-range parameters are refused, no memory means no sound") {
  ReverbRig rig;
  CHECK_FALSE(rig.set(rv::Size, -1.0f));
  CHECK_FALSE(rig.set(rv::Size, 101.0f));
  CHECK_FALSE(rig.set(rv::Damp, std::nanf("")));
  CHECK_FALSE(rig.set(rv::Predelay, 100.5f));
  CHECK_FALSE(rig.set(rv::Diffusion, -0.5f));
  CHECK_FALSE(rig.set(rv::Quality, 2.0f));
  CHECK_FALSE(rig.set(sx::kTempoField, 12000.0f));
  CHECK_FALSE(rig.set(9u, 1.0f));
  CHECK(rig.set(rv::Size, 100.0f));
  SendReverb none;
  std::int16_t in[kBlk] = {1, 2, 3};
  std::int16_t l[kBlk], r[kBlk];
  none.process(in, l, r, kBlk);
  CHECK(l[1] == 0);
  std::vector<std::int16_t> small(100);
  none.attach(small.data(), static_cast<std::uint32_t>(small.size()));
  none.process(in, l, r, kBlk);
  CHECK(r[2] == 0);
}

TEST_CASE("send reverb: a fuzz of parameters and loud noise stays finite") {
  ReverbRig rig;
  Lcg rng;
  std::int16_t in[kBlk];
  std::int16_t l[kBlk], r[kBlk];
  for (unsigned blk = 0; blk < 4000; ++blk) {
    if (rng.next() % 5u == 0u) {
      const float t = static_cast<float>(rng.next() % 1001u) * 0.1f;
      switch (rng.next() % 5u) {
        case 0: rig.set(rv::Size, t); break;
        case 1: rig.set(rv::Damp, t); break;
        case 2: rig.set(rv::Predelay, t); break;
        case 3: rig.set(rv::Diffusion, t); break;
        default: if (rng.next() % 40u == 0u) rig.set(rv::Quality, t > 50.0f ? 1.0f : 0.0f); break;
      }
    }
    const bool silent = (blk / 300u) % 3u == 2u;
    for (std::uint32_t i = 0; i < kBlk; ++i) in[i] = silent ? std::int16_t{0} : rng.sample();
    rig.r.process(in, l, r, kBlk);
    if (!rig.r.finite()) {
      CHECK(rig.r.finite());
      break;
    }
  }
}
