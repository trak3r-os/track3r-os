# Crush (module `crush`)

A bit-depth and sample-rate reduction effect with a dry/wet mix for a **track insert** of the Community OS (OS-INSFX-FX1, module 2 of the first effect pack; `docs/roadmap/BACKLOG.md`), loaded from the SD card as
`/modules/effects/crush.tmod` and bound with `diag ins bind <track> <slot> crush` after `mod load` (`docs/specs/OS-INSFX.md`; the Mixer's INSERTS page and the parameter page come with S4 and S5).
It is **not** built into the image. Status: new Community OS behaviour; tested on the host through the ABI against a double-precision reference (`tests/crush_test.cpp`, ctest `o3m_crush_test`); **its cost and its sound are not
verified on the unit** (`platform/mk66/tools/hil/qa_crush.json`, QA-GLB-190..192).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: the dead-zone quantiser, the phase-accumulator sample-and-hold and the mix are this module's own, written
for it; no code of any note or library is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Rate | 1..64, log | 1 | modulation target; the sample-and-hold factor: a value is taken every Rate samples (fractional: Rate on average, exactly Rate for a whole number). 1 is no hold. Listed first so that the unit bench's moving-parameter row (the first MOD target) sweeps the hold, the worst case |
| 1 | Bits | 1..16 | 8 | modulation target; 2^(Bits-1) steps per unit of full scale: 16 is the converter's own grid (the identity), 8 is 256 levels, 1 is the levels -1, 0, +1 (nothing under 3/4 of full scale passes). Acts at once (no glide: nothing to click) |
| 2 | Mix | 0..100 % | 100 | modulation target; out = (1 - m) Output x + m Output wet; 0 is the input bit for bit (at Output 0 dB) |
| 3 | Output | -24..+6 dB | 0 | modulation target; the output level 10^(dB/20), folded into the mix's two coefficients. Lower Bits lose level (the dead zone): this is the make-up |

All four will be learnable with MIDI Learn (S7). `set_param` does a few integer operations (and one division for Rate) and nothing else, so a modulation source may call it every block.

## How it works

- **Quantiser:** `v = x 2^(Bits-1)`, `m = floor(|v| + 1/4)`, `y = sign(x) m 2^(1-Bits)`. A dead-zone rounding: the step's threshold is at 3/4 instead of 1/2. Full scale (+-1) is exactly +-2^(Bits-1) steps, 0 is a level (silence in is silence
  out), the curve is odd, and Bits 16 is the identity on a Q15 signal. In float, `floor(t + 1/4)` is `round(t - 1/4)` done with the add-and-subtract of 1.5 * 2^23 (round to nearest even, exact, no int conversion, no call), so the host test's
  double-precision model matches it bit for bit.
- **Level law (the lesson of DRIVE, which came out +12 to +16 dB over bypass at its defaults):** at the defaults (Rate 1, Bits 8, Mix 100 %, Output 0 dB) the output rms is **never above the input's**: the test scans sines (100, 440, 1234.5 and 3000 Hz)
  and noise from -60 to -6 dBFS. A plain mid-tread rounding would fail it: a signal near one step comes out louder (+3 dB for a sine peaking at -46 dBFS at 8 bits), because values just over the half-step round up to a whole step; with the
  threshold at 3/4 the mean of `m^2 - |v|^2` over a step is negative at every level. The cost is level on a quiet signal: at 8 bits 1.4 dB at most (at -36 dBFS peak), 0.2 dB at -20 dBFS and almost nothing above -18 dBFS; below the dead zone (about -46 dBFS peak at 8 bits) the
  signal is gated, which is what a crusher does. Lower depths lose more (a step is bigger): Output +6 dB is the make-up. Other settings are not level-bound: Bits 1 to 3 or a long hold can be louder or quieter than the input (the hold of noise keeps its power, a
  hold of music aliases it). Defaults are not an unmanaged "wow": 8 bits with no hold is a mild grain.
- **Rate:** a sample-and-hold, with no filter before it, so it aliases (the effect). A 32-bit phase accumulator advances by `2^32 / Rate` a sample (the float quotient is nudged up by 128 so a whole Rate wraps on its exact sample); a carry takes the
  quantised current sample, so a hold lasts Rate samples on average and Rate for a whole number, one period short about every 12 s at Rate 64 (the increment's slack). Rate 1 is a separate loop with no hold, no accumulator and no select.
- After the quantiser or the hold: the mix and the output level, `out = cm0 x + cm1 wet`, with `cm0 = (1 - m) og`, `cm1 = m og`.
- Once a block (1..128 frames) the smoothed mix and output (dB) move k = frames / 256 of the way to their targets (about 6 ms at 64 frames); the output level is the only host exp2 (only while the output moves, never per sample), and the two per-sample
  values ramp linearly from the last block's to the new ones in steps of 16 samples (no zipper). Bits and Rate are applied by `set_param` at once.
- Known limits: **no anti-alias filter and no dither**, by design for a crusher; block-size invariance (same bits at any block size) holds for static parameters only (a moving Mix or Output glides once a block); at Bits 1 the levels are -1, 0, +1.
- Tail (the OS-INSFX S2 contract): `render` returns 1 while the input is live or the held value is not 0, 0 when both are silent. A held value is exactly 0 or at least one step (no subnormal, nothing to flush); a non-finite or absurd one (a NaN or infinity input
  held) is flushed at the end of the block.
- One view, **Curve**: the quantiser's transfer staircase at the Bits the page hands over (65 points in a frame, with the zero line and the depth's name, "8 BIT").

## Cost

Static (packed file, from the mk66 build, the o3m_pack line of the CI build): crush.tmod 2,220 B, hot 720 B, cold 1,312 B, state 64 B. The hot part has 220 instructions, 14 % FP loads and stores, one VDIV (the control step) and one blx (the host exp2 of the output level), none inside a
sample loop. The no-hold sample loop is 20 instructions (4 multiplies; the sign picks the step with a predicated move, no data-dependent branch); the hold loop is 13 instructions on a sample that takes no new value (the quantiser sits behind the phase carry) and 20 on one that does.
Estimates [est, not measured on the unit]: about 1.6k cycles a block with no hold, 1.7k at most with the hold and 1.9k while Rate moves every block (0.6 %, 0.65 % and 0.7 % of a 64-frame block of 261,225 cycles), the biquad's measured 1,666 for comparison, and well under the 3 points (7.8k cycles) of a track insert. Measure with `bench insert 64` on a bench image (rows `insert_effect` and `insert_effect_mod`), see `docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_crush_test o3m_crush_tmod
    ctest --test-dir build-os-host -R crush --output-on-failure
