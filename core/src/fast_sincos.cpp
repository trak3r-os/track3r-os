// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/fast_math.h>

#include <cstdint>

namespace o3os::core {

namespace {
constexpr float kPi = 3.14159265358979f;
float sin_core(float x) {  // |x| <= pi/2
  const float x2 = x * x;
  return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f + x2 * (-1.0f / 39916800.0f))))));
}
bool reduce(float& x) {  // into [-pi, pi]; false for a value out of range
  if (!(x > -1.0e4f && x < 1.0e4f)) {
    return false;
  }
  const float turns = x * (1.0f / 6.28318530717959f);
  const float k = static_cast<float>(static_cast<std::int32_t>(turns + (turns < 0.0f ? -0.5f : 0.5f)));
  x = (x - k * 6.28125f) - k * 0.0019353071795864769f;
  return true;
}
}  // namespace

float sin_fast(float x) noexcept {
  if (!reduce(x)) {
    return 0.0f;
  }
  if (x > kPi * 0.5f) {
    x = kPi - x;
  } else if (x < -kPi * 0.5f) {
    x = -kPi - x;
  }
  return sin_core(x);
}
float cos_fast(float x) noexcept {
  if (!reduce(x)) {
    return 0.0f;
  }
  const float ax = x < 0.0f ? -x : x;
  return ax <= kPi * 0.5f ? sin_core(kPi * 0.5f - ax) : -sin_core(ax - kPi * 0.5f);  // cos x = sin(pi/2 - |x|); past pi/2 it is -sin(|x| - pi/2)
}

}  // namespace o3os::core
