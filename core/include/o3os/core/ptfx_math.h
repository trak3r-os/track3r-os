// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The math members of the host table a .ptfx plugin gets (OS-CEPTFX S2, docs/specs/OS-CEPTFX.md 3.4, A8), without libm: the table's
// sinf and cosf are core::sin_fast / cos_fast, sqrtf is VSQRT, memcpy and memset are the OS's; these are the rest. Built on exp2_fast and
// log2_fast. No allocation, no peripheral, no shared state but the rand() seed. They run in the plugin's unprivileged context.
//
// Accuracy (tests/ptfx_host_math_test.cpp, against the host's libm): relative error under 2e-6 plus an absolute 2e-6 (logf near 1 has a
// tiny result with an absolute error of the series) over the ranges audio code uses. libm's special cases are kept for powf, fminf and fmaxf.

#include <cstdint>

namespace o3os::core::ptfx_math {

float tanf_(float x) noexcept;    // sin / cos; NaN, infinity and |x| >= 1e4 give 0
float expf_(float x) noexcept;    // exp2_fast(x log2 e): 0 below -87.3, +infinity above 88.7, NaN gives NaN
float logf_(float x) noexcept;    // log2_fast(x) ln 2: -infinity for 0, NaN for a negative or NaN, +infinity for infinity
float log10f_(float x) noexcept;
float powf_(float x, float y) noexcept;
float tanhf_(float x) noexcept;   // odd, saturating (+-1 beyond |x| > 9), a series for small |x|
float fminf_(float a, float b) noexcept;  // libm's NaN rule: a NaN argument gives the other one
float fmaxf_(float a, float b) noexcept;
float fabsf_(float x) noexcept;

// newlib's rand(): a 64-bit LCG, range 0..0x7FFFFFFF, seed 1 at start. The seed is one static; reseed at the start of every run (A8) so a run repeats.
void rand_seed(std::uint32_t seed = 1u) noexcept;
std::int32_t rand_() noexcept;

}  // namespace o3os::core::ptfx_math
