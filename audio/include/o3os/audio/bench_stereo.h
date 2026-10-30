// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-STEREO step 1 (bench only): the sample loop of a stereo voice, beside the plain voice's, so that the unit can say what a stereo voice costs. Nothing of the engine includes this header: it
// is read by platform/mk66/src/bench.cpp (an O3OS_BENCH image only, `diag bench voice_st`) and tests/bench/audio_bench.cpp, so a release image carries none of it.
//
// The mono loop is VoicePool::fast_body<false>'s (voices.cpp: the forward sample loop that runs while the position is clear of the end of the data): two halfword loads, a Q15 linear
// interpolation from the top 15 bits of the fraction, one SMULL by the Q24 amplitude, a saturation to 16 bits, and the 64-bit position step as an `adds` / `adc` pair. The stereo loop reads two
// planar blocks, the mid M = (L + R) / 2 and the side S = (L - R) / 2, with ONE position (one index, one fraction), interpolates both, and writes L = M + S and R = M - S, each through the
// amplitude: two SMULL and two saturations to the mono loop's one. The difference between the two rows is the cost of the second channel, kernel against kernel, with no pool in it.
// The placement is the caller's: the bodies are always_inline, so a wrapper in flash and one in `.ramfunc_l` would compile to the same instructions (the bench puts them in flash: the bench image
// has no SRAM_L room for a copy, +312 B).

#include <o3os/audio/hot.h>
#include <o3os/audio/simd.h>

#include <cstdint>

namespace o3os::audio::bench_st {

// The position as the voice keeps it: the integer part (the index of the first sample) and the Q32 fraction.
struct Pos {
  std::uint32_t idx;
  std::uint32_t lo;
};

// `Stereo` false: `m` to `ol`, the plain voice's loop. `Stereo` true: `m` and `s` to `ol` and `orr`. `inc` is the Q32.32 step, `amp` the Q24 amplitude (at most 2^28). The caller keeps
// idx + n x ceil(inc) + 2 inside the blocks: this loop checks nothing, as the voice's fast path (which the voice enters only when that holds).
template <bool Stereo>
O3OS_AUDIO_INLINE void resample(const std::int16_t* m, const std::int16_t* s, std::int16_t* ol, std::int16_t* orr, std::uint32_t n, Pos& p, std::uint64_t inc, std::int32_t amp) noexcept {
  const std::uint32_t inc_lo = static_cast<std::uint32_t>(inc);
  const std::uint32_t inc_hi = static_cast<std::uint32_t>(inc >> 32);
  std::uint32_t lo = p.lo;
  std::uint32_t idx = p.idx;
  std::int16_t* const stop = ol + n;
  for (; ol != stop; ++ol) {
    const std::int32_t m0 = m[idx];
    const std::int32_t m1 = m[idx + 1u];
    const std::int32_t frac = static_cast<std::int32_t>(lo >> 17);
    const std::int32_t sm = m0 + (((m1 - m0) * frac) >> 15);
    if constexpr (Stereo) {
      const std::int32_t s0 = s[idx];
      const std::int32_t s1 = s[idx + 1u];
      const std::int32_t ss = s0 + (((s1 - s0) * frac) >> 15);
      *ol = static_cast<std::int16_t>(simd::NativeOps::ssat16(static_cast<std::int32_t>((static_cast<std::int64_t>(sm + ss) * amp + 0x800000) >> 24)));
      *orr = static_cast<std::int16_t>(simd::NativeOps::ssat16(static_cast<std::int32_t>((static_cast<std::int64_t>(sm - ss) * amp + 0x800000) >> 24)));
      ++orr;
    } else {
      *ol = static_cast<std::int16_t>(simd::NativeOps::ssat16(static_cast<std::int32_t>((static_cast<std::int64_t>(sm) * amp + 0x800000) >> 24)));
    }
#if defined(__arm__)
    __asm__("adds %0, %0, %2\n\tadc %1, %1, %3" : "+r"(lo), "+r"(idx) : "r"(inc_lo), "r"(inc_hi) : "cc");  // the 64-bit add as the compiler makes it, in place (voices.cpp fast_body)
#else
    lo += inc_lo;
    idx += inc_hi + (lo < inc_lo ? 1u : 0u);
#endif
  }
  p.lo = lo;
  p.idx = idx;
}

// The same blocks made the plain way (one 64-bit position, no tricks), for the equality line: what the loops above must write, sample for sample. Not a bench kernel.
inline void reference(const std::int16_t* m, const std::int16_t* s, std::int16_t* ol, std::int16_t* orr, std::uint32_t n, Pos p, std::uint64_t inc, std::int32_t amp) noexcept {
  std::uint64_t phase = (static_cast<std::uint64_t>(p.idx) << 32) | p.lo;
  for (std::uint32_t k = 0; k < n; ++k, phase += inc) {
    const std::uint32_t idx = static_cast<std::uint32_t>(phase >> 32);
    const std::int32_t frac = static_cast<std::int32_t>(static_cast<std::uint32_t>(phase) >> 17);
    const std::int32_t sm = m[idx] + (((m[idx + 1u] - m[idx]) * frac) >> 15);
    const std::int32_t ss = s != nullptr ? s[idx] + (((s[idx + 1u] - s[idx]) * frac) >> 15) : 0;
    const auto make = [amp](std::int32_t v) {
      const std::int64_t x = (static_cast<std::int64_t>(v) * amp + 0x800000) >> 24;
      return static_cast<std::int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
    };
    ol[k] = make(s != nullptr ? sm + ss : sm);
    if (orr != nullptr) {
      orr[k] = make(sm - ss);
    }
  }
}

}  // namespace o3os::audio::bench_st
