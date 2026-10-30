// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PERF-CPU2 (A1): the backward fast path of run<true, true> (VoicePool::run_dir, the loop of a reversed note and of a backward or ping-pong instrument loop).
// The reference is a scalar copy of the checked loop as it was before the fast path (OS-PERF-REVERSE, dev 67579087). The new run_dir must make the same samples and leave the same voice
// (position, amplitude, direction, last sample, increment) for any position, increment, loop, amplitude and length. The hashes at the end were captured on that code, before the fast path
// was written. Host and Cortex-M4F (`qemu_audio_voices`: the fast path's inline asm only exists there).
#include <doctest/doctest.h>

#include <o3os/audio/voices.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace o3os::audio {

struct VoiceRunTest {
  using V = VoicePool::Voice;
  static constexpr std::uint8_t kRev = VoicePool::kModeReverse;
  static std::uint32_t run_dir(V& v, std::int16_t* out, std::uint32_t n) { return VoicePool::run_dir(v, out, n); }

  static std::int16_t sat(std::int32_t x) { return static_cast<std::int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x)); }

  // The checked loop of run<true, true> before OS-PERF-CPU2, verbatim except for the names.
  static std::uint32_t ref_dir(V& v, std::int16_t* out, std::uint32_t n) {
    const std::int16_t* const data = v.data;
    const std::uint64_t inc = v.inc;
    const std::uint32_t end = v.end;
    const std::uint32_t loop_len = v.loop_len;
    const std::uint32_t loop_start = end - loop_len;
    const std::int32_t step = v.amp_step;
    const std::int32_t top = (v.gain > v.gain_prev ? v.gain : v.gain_prev) * 256;
    std::uint64_t phase = v.phase;
    std::int32_t amp = v.amp;
    const std::int64_t lim_lo = static_cast<std::int64_t>(loop_start) << 32;
    const std::int64_t lim_hi = static_cast<std::int64_t>(end) << 32;
    const std::int64_t turn_hi = (static_cast<std::int64_t>(end) * 2 - 1) << 32;
    const std::int64_t turn_lo = (static_cast<std::int64_t>(loop_start) * 2 + 1) << 32;
    const std::int64_t span = static_cast<std::int64_t>(loop_len) << 32;
    const bool pingpong = v.mode == 2u;
    bool bwd = v.bwd;
    const bool rev = v.mode == kRev;
    std::uint32_t i = 0;
    if (rev && static_cast<std::int64_t>(phase) < lim_lo) {
      return 0;
    }
    for (; i < n; ++i) {
      std::uint32_t idx = static_cast<std::uint32_t>(phase >> 32);
      idx = idx < end ? idx : end - 1u;
      const std::uint32_t next = idx + 1u < end ? idx + 1u : idx;
      const std::int32_t s0 = data[idx];
      const std::int32_t s1 = data[next];
      const std::int32_t frac = static_cast<std::int32_t>(static_cast<std::uint32_t>(phase) >> 17);
      const std::int32_t s = s0 + (((s1 - s0) * frac) >> 15);
      out[i] = sat(static_cast<std::int32_t>((static_cast<std::int64_t>(s) * amp + 0x800000) >> 24));
      std::int64_t q = static_cast<std::int64_t>(phase);
      if (!bwd) {
        q += static_cast<std::int64_t>(inc);
        if (q >= lim_hi) {
          q = turn_hi - q;
          q = q < lim_lo ? lim_lo : q;
          bwd = true;
        }
      } else {
        q -= static_cast<std::int64_t>(inc);
        if (q <= lim_lo) {
          if (rev) {
            if (q < lim_lo) {
              phase = static_cast<std::uint64_t>(q);
              ++i;
              break;
            }
          } else {
            if (pingpong) {
              q = turn_lo - q;
              bwd = false;
            } else {
              q += span;
            }
            q = q > lim_hi ? lim_hi : (q < lim_lo ? lim_lo : q);
          }
        }
      }
      phase = static_cast<std::uint64_t>(q);
      amp += step;
      amp = amp < 0 ? 0 : (amp > top ? top : amp);
    }
    v.phase = phase;
    v.amp = amp;
    v.bwd = bwd;
    if (i != 0u) {
      v.last = out[i - 1u];
    }
    return i;
  }

  static bool same_state(const V& a, const V& b) {
    return a.phase == b.phase && a.inc == b.inc && a.amp == b.amp && a.amp_step == b.amp_step && a.bwd == b.bwd && a.last == b.last && a.end == b.end && a.loop_len == b.loop_len && a.mode == b.mode &&
           a.gain == b.gain && a.gain_prev == b.gain_prev && a.data == b.data;
  }

  static void setup(V& v, const std::int16_t* data, std::uint8_t mode, std::uint32_t end, std::uint32_t loop_len, std::uint64_t phase, std::uint64_t inc, bool bwd, std::int32_t amp, std::int32_t step,
                    std::int32_t gain) {
    v = V{};
    v.data = data;
    v.mode = mode;
    v.end = end;
    v.loop_len = loop_len;
    v.phase = phase;
    v.inc = inc;
    v.bwd = bwd;
    v.amp = amp;
    v.amp_step = step;
    v.gain = gain;
    v.gain_prev = gain;
    v.playing = true;
  }
};

}  // namespace o3os::audio

namespace {

using o3os::audio::VoiceRunTest;
using VRV = VoiceRunTest::V;

struct Rng {  // xorshift64*: the same numbers on the host and on the target
  std::uint64_t s;
  std::uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  std::uint32_t below(std::uint32_t m) { return static_cast<std::uint32_t>((next() >> 33) % m); }
};

std::int16_t g_data[640];
VRV g_va;  // the reference
VRV g_vb;  // run_dir

void fill_data() {
  Rng r{0x9E3779B97F4A7C15ull};
  for (std::int16_t& d : g_data) {
    d = static_cast<std::int16_t>(static_cast<std::int32_t>(r.below(65536)) - 32768);
  }
}

std::uint64_t fnv(std::uint64_t h, std::uint64_t x) {
  for (unsigned b = 0; b < 8; ++b) {
    h ^= (x >> (8u * b)) & 0xFFu;
    h *= 0x100000001B3ull;
  }
  return h;
}

#if defined(__arm__)
constexpr unsigned kTrials = 3000;
#else
constexpr unsigned kTrials = 20000;
#endif

}  // namespace

TEST_CASE("voices: run_dir with the backward fast path equals the checked loop it replaces, in output and in voice state, over random positions, steps, loops, amplitudes and lengths (OS-PERF-CPU2 A1)") {
  fill_data();
  Rng r{0x1234567887654321ull};
  std::uint32_t fast_eligible = 0;
  std::uint32_t at_n = 0;
  std::uint32_t at_n1 = 0;
  std::uint32_t bad = 0;
  static std::int16_t oa[64];
  static std::int16_t ob[64];
  for (unsigned trial = 0; trial < kTrials && bad < 5u; ++trial) {
    const std::uint32_t mode_pick = r.below(3);
    const std::uint8_t mode = mode_pick == 0 ? VoiceRunTest::kRev : (mode_pick == 1 ? std::uint8_t{1} : std::uint8_t{2});
    // the step: 0, tiny, 0.5, 1.0, 2.7, any up to 6, a huge one, and the saturated high word
    std::uint64_t inc;
    switch (r.below(10)) {
      case 0: inc = 0; break;
      case 1: inc = r.below(1000); break;
      case 2: inc = 1ull << 31; break;
      case 3: inc = 1ull << 32; break;
      case 4: inc = static_cast<std::uint64_t>(2.7 * 4294967296.0); break;
      case 5: inc = r.next() >> (r.below(2) == 0 ? 2 : 33); break;
      case 6: inc = 0xFFFFFFFF00000000ull | static_cast<std::uint32_t>(r.next()); break;
      default: inc = (static_cast<std::uint64_t>(r.below(6)) << 32) | static_cast<std::uint32_t>(r.next()); break;
    }
    std::uint32_t n = 1u + r.below(64);
    if (r.below(8) == 0) {
      n = r.below(2) == 0 ? 64u : 48u;
    }
    const std::uint64_t ip64 = (inc >> 32) + 1u;
    std::uint32_t loop_start, end, loop_len, idx0;
    if (r.below(2) == 0 && ip64 <= 7u) {  // targeted: the fast path covers n - 1, n or n + 1 steps (or fewer: the position is random in the step)
      const std::uint32_t ip = static_cast<std::uint32_t>(ip64);
      const std::uint32_t want = n + r.below(3) - 1u;  // n - 1 .. n + 1
      loop_start = r.below(8);
      idx0 = loop_start + want * ip + r.below(ip);
      end = idx0 + 2u + r.below(3);
      loop_len = end - loop_start;
    } else {
      end = 2u + r.below(500);
      loop_len = 1u + r.below(end);
      loop_start = end - loop_len;
      idx0 = loop_start + r.below(loop_len + 1u);
      if (r.below(8) == 0) {
        idx0 = r.below(end + 1u);  // maybe below the loop (a live loop edit raised loop_start), maybe at loop_end
      }
    }
    if (end > 600u) {
      continue;
    }
    std::uint64_t phase = (static_cast<std::uint64_t>(idx0) << 32) | static_cast<std::uint32_t>(r.next());
    if (r.below(10) == 0) {
      phase = static_cast<std::uint64_t>(loop_start) << 32;  // the window's first frame exactly
    }
    const std::int32_t gain = static_cast<std::int32_t>(r.below(1u << 20)) | 1;
    const std::int32_t top = gain * 256;
    std::int32_t amp;
    switch (r.below(6)) {
      case 0: amp = 0; break;
      case 1: amp = top; break;
      case 2: amp = top + static_cast<std::int32_t>(r.below(1u << 20)); break;  // above the cap
      case 3: amp = -static_cast<std::int32_t>(r.below(1u << 20)); break;       // below zero
      default: amp = static_cast<std::int32_t>(r.below(static_cast<std::uint32_t>(top) + 1u)); break;
    }
    std::int32_t step = 0;
    switch (r.below(4)) {
      case 0: step = 1 + static_cast<std::int32_t>(r.below(4000)); break;
      case 1: step = -1 - static_cast<std::int32_t>(r.below(4000)); break;
      default: break;
    }
    const bool bwd = mode == VoiceRunTest::kRev ? true : (r.below(4) != 0);
    VoiceRunTest::setup(g_va, g_data, mode, end, loop_len, phase, inc, bwd, amp, step, gain);
    VoiceRunTest::setup(g_vb, g_data, mode, end, loop_len, phase, inc, bwd, amp, step, gain);
    // the entry guard as run() states it, to count the cases that really take the fast path
    const bool eligible = bwd && static_cast<std::int64_t>(phase) >= (static_cast<std::int64_t>(loop_start) << 32) && idx0 + 2u <= end && (inc >> 32) != 0xFFFFFFFFull;
    if (eligible) {
      const std::uint32_t k = (idx0 - loop_start) / static_cast<std::uint32_t>((inc >> 32) + 1u);
      fast_eligible += k != 0u ? 1u : 0u;
      at_n += k == n ? 1u : 0u;
      at_n1 += k + 1u == n ? 1u : 0u;
    }
    for (unsigned call = 0; call < 3u; ++call) {
      const std::uint32_t nn = call == 0 ? n : 1u + r.below(64);
      std::memset(oa, 0x5A, sizeof oa);
      std::memset(ob, 0x5A, sizeof ob);
      const std::uint32_t ma = VoiceRunTest::ref_dir(g_va, oa, nn);
      const std::uint32_t mb = VoiceRunTest::run_dir(g_vb, ob, nn);
      const bool ok = ma == mb && std::memcmp(oa, ob, sizeof oa) == 0 && VoiceRunTest::same_state(g_va, g_vb);
      if (!ok) {
        ++bad;
        CHECK_MESSAGE(false, "trial " << trial << " call " << call << " mode " << unsigned(mode) << " n " << nn << " inc " << inc << " phase " << phase << " end " << end << " loop_len " << loop_len << " amp "
                                      << amp << " step " << step << " gain " << gain << ": made " << ma << " vs " << mb);
        break;
      }
      if (ma < nn) {
        break;  // a one shot ended
      }
    }
  }
  CHECK(bad == 0u);
  CHECK(fast_eligible > 500u);  // the test reaches the fast path
  CHECK(at_n > 20u);            // k == n
  CHECK(at_n1 > 20u);           // k == n - 1
}

namespace {

// One voice driven as render_seg drives it: `calls` spans of 64 samples (or 16 + 48), the hash over every output sample and the voice's state after each.
std::uint64_t dir_hash(std::uint8_t mode, std::uint64_t inc, std::uint32_t end, std::uint32_t loop_len, double phase_idx, std::int32_t amp, std::int32_t step, std::int32_t gain, bool split, unsigned calls, bool bwd0 = false) {
  VRV& v = g_vb;
  VoiceRunTest::setup(v, g_data, mode, end, loop_len, static_cast<std::uint64_t>(phase_idx * 4294967296.0), inc, mode == VoiceRunTest::kRev || bwd0, amp, step, gain);
  static std::int16_t out[64];
  std::uint64_t h = 0xCBF29CE484222325ull;
  bool stopped = false;
  for (unsigned c = 0; c < calls && !stopped; ++c) {
    const std::uint32_t segs[2] = {split ? 16u : 64u, split ? 48u : 0u};
    for (const std::uint32_t seg : segs) {
      if (seg == 0u || stopped) {
        continue;
      }
      const std::uint32_t made = VoiceRunTest::run_dir(v, out, seg);
      h = fnv(h, made);
      for (std::uint32_t i = 0; i < made; ++i) {
        h = fnv(h, static_cast<std::uint16_t>(out[i]));
      }
      h = fnv(fnv(fnv(fnv(h, v.phase), static_cast<std::uint32_t>(v.amp)), v.bwd ? 1u : 0u), static_cast<std::uint32_t>(v.last));
      stopped = made < seg;
    }
  }
  return h;
}

}  // namespace

// Reversed notes (a window of 600 frames: three calls of 64 reach 192 frames at most), the window's edge and the stop, a backward and a ping-pong instrument loop (the turn at the loop's start, the turn
// at its end, a wrap, a step longer than the loop). Captured on the code before the fast path.
TEST_CASE("voices: run_dir bit-identity hashes of reversed notes and backward / ping-pong loops, captured before the backward fast path (OS-PERF-CPU2 A1)") {
  fill_data();
  const std::uint64_t half = 1ull << 31;
  const std::uint64_t one = 1ull << 32;
  const std::uint64_t t27 = static_cast<std::uint64_t>(2.7 * 4294967296.0);
  const std::uint8_t R = VoiceRunTest::kRev;
  const std::int32_t unity = 65536;      // the gain, Q16
  const std::int32_t top = unity * 256;  // its cap, Q24
  std::uint64_t h[21];
  unsigned n = 0;
  for (const std::uint64_t inc : {half, one, t27}) {
    h[n++] = dir_hash(R, inc, 600, 600, 599.0, top, 0, unity, false, 3);       // steady, whole blocks
    h[n++] = dir_hash(R, inc, 600, 600, 599.0, top / 4, 700, unity, true, 3);  // ramping, 16 + 48
  }
  h[n++] = dir_hash(R, one, 600, 600, 63.0, top, 0, unity, false, 2);   // the stop inside the second call
  h[n++] = dir_hash(R, one, 600, 600, 0.0, top, 0, unity, false, 2);    // the window's first frame exactly
  h[n++] = dir_hash(R, half, 120, 40, 100.0, top, 0, unity, false, 2);  // a short window, from the middle
  h[n++] = dir_hash(R, one, 64, 64, 63.0, top, 0, unity, false, 2);     // the stop at a block's end
  h[n++] = dir_hash(R, t27, 200, 150, 199.5, top, -300, unity, true, 3);
  for (const std::uint8_t m : {std::uint8_t{1}, std::uint8_t{2}}) {
    h[n++] = dir_hash(m, one, 300, 200, 299.25, top, 0, unity, false, 6, false);
    h[n++] = dir_hash(m, half, 300, 200, 150.0, top, 0, unity, true, 6, true);
    h[n++] = dir_hash(m, t27, 300, 200, 250.5, top / 2, 900, unity, false, 6, true);
    h[n++] = dir_hash(m, t27, 24, 4, 23.5, top, 0, unity, false, 3, false);
    h[n++] = dir_hash(m, one, 300, 200, 299.25, top, -400, unity, true, 6, true);
  }
  CHECK(n == 21u);
  static const std::uint64_t kWant[21] = {8638674187086943013ull,  18064514353105606015ull, 2869952906377957352ull,  1723090688637586540ull,  15831244612177557560ull, 8700312957337079154ull,
                                          11775831403016900244ull, 16984003542763479854ull, 1002524996593515436ull,  11775831403016900244ull, 13300127228671401016ull, 12071676137309792299ull,
                                          7364047395332775276ull,  16291067773062288065ull, 1945036587346609269ull,  17117467517241447163ull, 1998948200838638632ull,  3392145087335120906ull,
                                          12358305153361925064ull, 1150664088724236590ull,  5786393602765507287ull};
  for (unsigned i = 0; i < n; ++i) {
    CHECK_MESSAGE(h[i] == kWant[i], "HASH " << i << " " << h[i] << "ull");
  }
}
