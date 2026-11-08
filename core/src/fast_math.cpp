// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/fast_math.h>

#include <cstdint>
#include <cstring>

// sin_fast / cos_fast moved to fast_sincos.cpp (OS-CEPTFX S2): they must keep the -ffp-contract=off build of their old home,
// audio/src/module_voices.cpp, while exp2_fast / log2_fast below stay as core always built them.

namespace o3os::core {

float exp2_fast(float x) noexcept {
  if (!(x >= -126.0f)) {
    return 0.0f;  // underflow (2^-126 is the smallest normal float), and NaN
  }
  x = x > 127.0f ? 127.0f : x;
  int n = static_cast<int>(x);
  n = static_cast<float>(n) > x ? n - 1 : n;  // floor
  float f = x - static_cast<float>(n);         // [0, 1)
  if (f > 0.5f) {                              // [-1/2, 1/2]: the series converges fastest round zero
    f -= 1.0f;
    ++n;
  }
  // 2^f = sum (f ln 2)^k / k!, to k = 7: the first neglected term is below 6e-9 for |f| <= 1/2
  const float p = 1.0f + f * (0.6931471806f + f * (0.2402265070f + f * (0.0555041087f + f * (0.0096181291f +
                         f * (0.0013333558f + f * (0.0001540353f + f * 0.0000152527f))))));
  const std::uint32_t bits = static_cast<std::uint32_t>(n + 127) << 23;  // 2^n, n in [-126, 127]
  float scale;
  std::memcpy(&scale, &bits, sizeof(scale));
  return p * scale;
}

float log2_fast(float x) noexcept {
  std::uint32_t bits;
  std::memcpy(&bits, &x, sizeof(bits));
  const std::uint32_t biased = (bits >> 23) & 0xFFu;
  if ((bits >> 31) != 0 || biased == 0) {
    return -126.0f;  // zero, negative, denormal
  }
  if (biased == 255) {
    return (bits & 0x7FFFFFu) != 0 ? -126.0f : 128.0f;  // NaN, infinity
  }
  int e = static_cast<int>(biased) - 127;
  const std::uint32_t mantissa_bits = (bits & 0x7FFFFFu) | (127u << 23);  // m in [1, 2)
  float m;
  std::memcpy(&m, &mantissa_bits, sizeof(m));
  if (m > 1.41421356f) {  // [1/sqrt 2, sqrt 2): z = (m - 1) / (m + 1) stays below 0.172
    m *= 0.5f;
    ++e;
  }
  const float z = (m - 1.0f) / (m + 1.0f);
  const float z2 = z * z;
  // log2 m = (2 / ln 2) atanh z = (2 / ln 2) (z + z^3 / 3 + z^5 / 5 + z^7 / 7 + z^9 / 9)
  return static_cast<float>(e) + 2.885390082f * z * (1.0f + z2 * (0.3333333333f + z2 * (0.2f + z2 * (0.1428571429f + z2 * 0.1111111111f))));
}

}  // namespace o3os::core
