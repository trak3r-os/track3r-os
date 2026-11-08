// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Base-2 logarithm and exponential in single precision without libm (powf, exp2f, log2f and log10f are about 7 KB of flash on the
// MK66 that nothing needs them for; sinf and cosf, which the FFT and the engine use anyway, are kept). They began in ui/plot
// (plot_math.h, OS-003d) and moved here with OS-002d, because the parameter curves (param.h: log and exp) need them below `ui`;
// the plot library's exp2_fast / log2_fast are these two. No state, allocation-free, host-tested (tests/plot_math_test.cpp).
//
// exp2_fast(x) is 2^x: the exponent by construction and 2^f of the fraction by a degree-7 Taylor series on [-1/2, 1/2], relative
// error under 1e-7 for x in [-126, 127] (0 below, 2^127 above, NaN gives 0). log2_fast(x) is the exponent plus 2 atanh((m - 1) / (m + 1))
// / ln 2 of the mantissa m in [1/sqrt 2, sqrt 2) by its series to z^9, error under 1e-6 plus the rounding of a float result (-126 for
// zero, a negative number, NaN or a denormal; 128 for infinity).

namespace o3os::core {

float exp2_fast(float x) noexcept;
float log2_fast(float x) noexcept;
// sin and cos (moved from audio/src/module_voices.cpp, OS-CEPTFX S2: the module host and the .ptfx host table share one copy). Argument reduced into [-pi, pi],
// then a Taylor series: absolute error under 1e-6 for |x| < 1000 rad; NaN, infinity and |x| >= 1e4 give 0.
float sin_fast(float x) noexcept;
float cos_fast(float x) noexcept;

}  // namespace o3os::core
