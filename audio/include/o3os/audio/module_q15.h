// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The module output rule, shared by the module instruments (module_voices.cpp) and the effect inserts (insert_host.cpp): a module's float block to Q15. Moved unchanged from
// module_voices.cpp (OS-INSFX S1); always_inline, so the instruments' render() is the code it was. NOT kernel::float_to_q15: that one maps NaN to +32767, a DC rail from a faulty module.

#include <o3os/audio/hot.h>

#include <cstdint>
#include <cstring>

#if defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

namespace o3os::audio {

// n floats of a module's output (+-1.0 is full scale) to Q15 in `dst`, and the number of samples that did not fit. THE RULE, the same on every platform: the integer part of x * 32768 (the
// fraction is cut towards zero, as the loop before OS-303PERF did with static_cast<int32_t>), held to -32768..32767; a value that is NaN is 0 (silence, not a click at full scale); +-infinity and
// anything out of the int32 range saturate. A sample is counted when its integer part left the int16 range (x >= 1.0 or x <= -1.0 - 1/32768): one that is cut to the rail without losing its integer
// part (32767.5 -> 32767) is not a clip. On the Cortex-M4F that is VCVT (float to fixed point, 15 fractional bits: it multiplies by 2^15 itself, truncates towards zero, saturates at int32 and
// makes NaN 0) and SSAT #16: no compare, no branch (the loop it replaces had three per sample).
O3OS_AUDIO_INLINE std::uint32_t to_q15(std::int16_t* dst, const float* src, std::uint32_t n) noexcept {  // (n >= 1)
  std::uint32_t clipped = 0;
  do {  // (n >= 1: render() returns before for a segment of no frames; the test at the bottom is one SUBS and one branch)
#if defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
    float t = *src;
    __asm__("vcvt.s32.f32 %0, %0, #15" : "+t"(t));
    std::int32_t i;
    std::memcpy(&i, &t, sizeof i);
    const std::int32_t c = __ssat(i, 16);
#else
    const float y = *src * 32768.0f;  // (exact, a power of two: infinity stays infinity)
    std::int32_t i = 0;
    if (y >= 2147483648.0f) {
      i = 2147483647;
    } else if (y <= -2147483648.0f) {
      i = -2147483647 - 1;
    } else if (y == y) {
      i = static_cast<std::int32_t>(y);  // in range: truncated towards zero
    }
    const std::int32_t c = i > 32767 ? 32767 : (i < -32768 ? -32768 : i);
#endif
    clipped += c != i ? 1u : 0u;
    *dst++ = static_cast<std::int16_t>(c);
    ++src;
  } while (--n != 0u);
  return clipped;
}

}  // namespace o3os::audio
