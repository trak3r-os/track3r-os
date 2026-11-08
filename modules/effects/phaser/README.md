# Phaser (module `phaser`)

A phaser with a dry/wet mix for a **track insert** of the Community OS (OS-INSFX-FX1, module 3 of the first effect pack; `docs/roadmap/BACKLOG.md`), loaded from the SD card as
`/modules/effects/phaser.tmod` and bound with `diag ins bind <track> <slot> phaser` after `mod load` (`docs/specs/OS-INSFX.md`; the Mixer's INSERTS page and the parameter page come with S4 and S5).
It is **not** built into the image. Status: new Community OS behaviour; tested on the host through the ABI against a double-precision reference (`tests/phaser_test.cpp`, ctest `o3m_phaser_test`); **its cost and its sound are not
verified on the unit** (`platform/mk66/tools/hil/qa_phaser.json`, QA-GLB-197..200).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: the lattice all-pass chain, the Pade tan, the LFO wave and the bounded feedback are this module's own, written
for it; no code of any phaser or library is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Stages | 4..6 (whole) | 4 | the number of all-pass sections (4 gives two notches, 5 two and one at Nyquist, 6 three). Not a modulation target: a change is a jump in the response; a section that drops out restarts from rest |
| 1 | Period | 100..20000 ms, log | 2000 | modulation target; the LFO's period. A period and not a rate in Hz on purpose: the OS shows a parameter that has a unit as whole numbers, and a slow sweep needs fractions of a Hz. The first modulation target, so the unit bench's moving-parameter row sweeps it |
| 2 | Depth | 0..100 % | 60 | modulation target; the sweep's width: +-2 octaves at 100 % (the frequency is Centre 2^(2 Depth LFO)). 0 holds the notches still at Centre |
| 3 | Centre | 100..4000 Hz, log | 800 | modulation target; the frequency the sweep is centred on (a section's -90 degree point), clamped with the sweep to 40 Hz .. 0.17 of the sample rate (7.5 kHz) |
| 4 | Feedback | -90..+90 % | 35 | modulation target; the chain's output goes back to its input: a positive value sharpens and rings the notches, a negative one gives a hollower sound. Bounded at 90 % (see below) |
| 5 | Mix | 0..100 % | 50 | modulation target; out = (1 - m) x + m wet; 0 is the input bit for bit; 50 % is the classic notch depth (the dry and the phase-shifted wet cancel), 100 % is the pure all-pass (a phase swirl, no notches, unless the feedback is up) |

The five modulation targets (Period, Depth, Centre, Feedback, Mix) will be learnable with MIDI Learn (S7) — and so will Stages: learnable, but **not** a modulation target (a change of it is a jump in the response, see the table above, not a glide).
`set_param` does a clamp and a store (and one division for Period), so a modulation source may call it every block.

## How it works

- **The chain:** `Stages` identical first-order all-pass sections in the **normalised lattice** form, `y = a u + c z`, `z' = c u - a z`, `c = sqrt(1 - a^2)`, `H = (a + 1/z) / (1 + a/z)`: gain exactly 1 at every frequency, a phase that falls
  from 0 at DC to -180 degrees at Nyquist and is -90 degrees at the section's frequency. A notch is where the chain's phase is an odd multiple of -180 degrees, so the sum of the dry and the wet cancels there. The map (input, state) -> (output, new state)
  is the reflection `[a c; c -a]`, orthogonal when `a^2 + c^2 = 1`: **a section never gains, whatever the coefficient does**. The cheaper transposed form (2 multiplies and 2 adds a section instead of 4 and 2) was built first and measured 0.46 dB
  over bypass on a 150 Hz tone at the fastest full sweep at Centre 100 Hz while its coefficient moved: the lattice is what buys the exact level law (`c` is shrunk by 1e-6 so that `a^2 + c^2 <= 1` in float, and the linear ramp of `(a, c)` between two blocks is a chord inside the circle).
- **The coefficient:** the bilinear `a = (w - 1) / (w + 1)`, `w = tan(pi f / fs)`, with tan as the Pade `x (15 - x^2) / (15 - 6 x^2)` (error 2e-5 of the value for f up to 0.17 fs), so `a = (P - Q) / (P + Q)` with `P = x (15 - x^2)`, `Q = 15 - 6 x^2`:
  one division and one square root (`VSQRT`: the packer builds `-fno-math-errno`), once a block, no trigonometry. The same frequency feeds every section (the classic phaser: one coefficient).
- **The sweep:** a sine-like LFO (a triangle through `p(t) = t (3 - t^2) / 2`, flat at its peaks) whose phase advances once a block; `f = Centre 2^(2 Depth LFO)`, one host `exp2` per block (none at Depth 0). Once a block the section
  coefficients are taken for the block's end and **ramp linearly sample by sample** from the last block's: no zipper at any LFO rate. The LFO starts at Centre, going down.
- **The feedback and the level law (the lesson of DRIVE, which came out +12 to +16 dB over bypass at its defaults):** the chain's output goes back to its input one sample late, scaled by `g` (Feedback, `|g| <= 0.9`), and the input is scaled
  by `1 - |g|` first, so the wet path is `W = (1 - |g|) A / (1 - g A / z)` with `|A| = 1`. Since `|1 - g A / z| >= 1 - |g|`, **`|W| <= 1` at every frequency**, and the output `(1 - m) + m W` has a gain of at most `(1 - m) + m = 1`: at any **fixed** setting the effect is a passive
  filter, never louder than bypass; it holds while the coefficient moves too (the chain is a contraction at every sample and the delay keeps the norm). The bound is per sample and takes the `g` of that sample in the loop equation: Feedback is a
  modulation target and glides, so **while it moves the loop is time-varying and the law does not hold sample-wise** — an impulse charged while `g` is near 0 and then a fast Feedback rise recirculates the energy the sections stored,
  measured up to about 5x the input peak in the test (the moving-Feedback case); the hard clamp of the fed-back sample to +-4 (below) still bounds the signal. Measured at fixed settings: at the defaults a sine or noise comes out 2.1 dB or more
  quieter than it went in (the notches take level: 3 dB on noise at Mix 50 %), and over 486 settings with the sweep at its fastest the largest rise is -0.0003 dB: none.
- **It cannot blow up (reasoned, and tested):** (1) `|g| <= 0.9` and the loop is a contraction (an all-pass chain of reflections): a stable filter; (2) independent of that, the fed-back sample is hard-clamped to +-4; (3) every section maps
  `(input, state)` to `(output, new state)` without growing the norm, so for any finite input the states and outputs stay finite and bounded by the input and the clamped feedback, for any LFO or parameter schedule. The test runs the maximum feedback, both signs, 4/5/6
  stages, the fastest full-depth sweep at 100 Hz and 4 kHz centres and a still one, noise, squares and a sine (72 runs of 10 s: the level stays at the input's in rms — the law above is **rms-limited, not a peak limiter**: the measured peak is 1.2 on
  full-scale input — and the end is no louder than the start; on the unit the Q15 adapter (`audio/module_q15.h`, `to_q15`) saturates the conversion to -32768..32767 and counts the clipped samples in `InsertHost::Stats::saturated`) and a one-minute run at the worst setting. A NaN or an infinity on the INPUT
  (the OS cannot send one: its adapter is Q15) poisons the sections for the rest of that block, and the state is flushed to 0 at its end (an absurd state over 1e18 too): the next block is clean.
- **Control:** Centre, Depth, Feedback and Mix move `k = frames / 256` of the way to their targets once a block (about 6 ms at 64 frames); Stages and Period act at once. The first parameters after `init()` (the stored blob) are applied without a glide.
  A change of Stages is a jump (the sections that remain keep their state; the new ones start from rest): it is not a modulation target.
- **Tail (the OS-INSFX S2 contract):** `render` returns 1 while the input is live (its last sample is not 0) or the sections ring (the sum of their `|state|` is at least 1e-5, a third of a Q15 step), and 0 when both are silent: the sections are then
  cleared and the output is exact zeros. The ring after a full-scale noise burst lasts 0.02 s at the defaults, 0.45 s at 6 stages and +90 % feedback and 1.2 s at the worst setting (Centre 100 Hz, Period 100 ms, 6 stages, -90 %: the coefficient is near -1 at 40 Hz); after a
  full-scale DC step there 1.7 s (the test's log). A sound that ends on a held frequency near 40 Hz rings longest.
- Known limits: **mono**, one coefficient for every section (the notches are where the chain's phase says, not spread); the response is not block-size independent while Depth is over 0 (the LFO is sampled at the block's end: 25 dB or better against another
  block size at Period 3 s); 5 stages is allowed besides 4 and 6 (an odd count puts a notch exactly at Nyquist); the bypass click is the host's (a one-block ramp to dry) and un-bypass restarts the module from silence.
- One view, **Notches**: the response over the input at the Centre, Stages, Feedback and Mix the page hands over, 20 Hz to 7.5 kHz on a log axis, power from 1 (top) to 0 (bottom), 65 points and the stage count ("4 STAGES"). It draws from the page's parameters only (no
  host call, no instance state, no trigonometry: `H = (1 - j r) / (1 + j r)`, `r = tan(pi f / fs) / tan(pi f_c / fs)`, the Pade tan again, the feedback's delay as the same form); the axis assumes 44.1 kHz.

## Cost

Static (packed file, from the CI build, the o3m_pack line): phaser.tmod 3,584 B, hot 1,648 B, cold 1,792 B, state 108 B. The hot part has 449 instructions, 14 % FP loads and stores, two VDIV (the control step: `1 / frames` and the coefficient), one VSQRT, one blx (the
host exp2 of the sweep) and no VDIV, call or spill inside a sample loop (two FP loads and stores a sample: the input and the output; the compiler keeps every section's state in a register and saves d8-d15 once a block). The sample loop is 45 to 48
instructions at 4 stages, 51 to 54 at 5 and 57 to 60 at 6 (two code paths: the feedback clamp's branch laid out both ways): the ramp of four values, the input scale and the feedback, four multiplies and two adds a section, the clamp and the mix.
Estimates [est, not measured on the unit]: about 50 cycles a sample at 4 stages and 62 at 6 (an instruction a cycle, the FPU's dependent chain of about 3 cycles a section hides behind the issue), so **about 3.2k cycles a block of 64 at 4 stages and 4.0k at 6**, plus about
400 for the control step (the host exp2, two divisions, a square root, four glides): **about 3.6k (1.4 %) and 4.4k (1.7 %) of a 64-frame block of 261,225 cycles**, with the adapter (about 2.2k on the unit, S1) 5.8k to 6.6k: inside the 3 points (7.8k cycles) of a track
insert, and still inside at +40 % (the biquad's measured 1,666 cycles was 39 % over its estimate): 5.0k and 6.2k without the adapter. The heaviest insert so far (twice the biquad). Measure with `bench insert 64` on a bench image (rows `insert_effect` and `insert_effect_mod`), see
`docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_phaser_test o3m_phaser_tmod
    ctest --test-dir build-os-host -R phaser --output-on-failure
