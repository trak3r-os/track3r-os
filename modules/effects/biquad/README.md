# Biquad Filter (module `biquad`)

A resonant filter with a dry/wet mix for a **track insert** of the Community OS (OS-INSFX S8), loaded from the SD card as `/modules/effects/biquad.tmod` and bound with `diag ins bind <track> <slot> biquad` after `mod load biquad`
(`docs/specs/OS-INSFX.md`; the Mixer's INSERTS page and the parameter page come with S4 and S5). It is **not** built into the image. Status: new Community OS behaviour; tested on the host through the ABI against a
double-precision reference (`tests/biquad_test.cpp`, ctest `o3m_biquad_test`); **its cost and its sound are not verified on the unit** (`platform/mk66/tools/hil/qa_biquad.json`, QA-GLB-168..172).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** the coefficient formulas of the Audio EQ Cookbook (Robert Bristow-Johnson), written here from the
formulas; no code is taken from the note or from any library (`THIRD_PARTY.md`, "RBJ Audio EQ Cookbook").

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Mode | 0..3 | 0 | 0 low-pass, 1 high-pass, 2 band-pass (constant 0 dB peak), 3 notch; shown as a number, the view names it; a change is a crossfade (about 25 ms), not a step |
| 1 | Cutoff | 20..20000 Hz, log | 1000 | modulation target; clamped to 0.45 of the sample rate in the filter |
| 2 | Resonance | 0..100 % | 10 | modulation target; Q = 0.5 * 2^(r / 20): 0.5 at 0, 0.707 (Butterworth) at 10, 16 at 100 (+24 dB at the peak of a low-pass: it can clip the converter, as any resonant filter) |
| 3 | Mix | 0..100 % | 100 | modulation target; out = (1 - m) dry + m filtered; 0 is the input bit for bit |

All four will be learnable with MIDI Learn (S7). `set_param` only stores a target, so a modulation source may call it every block.

## How it works

- One transposed direct form II section (`y = b0 x + z1; z1 = b1 x - a1 y + z2; z2 = b2 x - a2 y`), float only, no division and no library call in the sample loop (14 instructions a sample, 5 multiplies and 4 adds).
- The mix is folded into the numerator, `((1 - m) A + m B) / A`, so the effect is one biquad at any mix.
- Once a block (1..128 frames) the smoothed cutoff and resonance (log domain) and mix move a fraction of the way to their targets (k = frames / 256: about 6 ms at 64 frames), the prototype is derived again only while
  something moves (two host trig calls and two exp2), and the five coefficients ramp linearly in steps of 16 samples to the new ones. The first parameters after `init()` (the stored blob) are applied at once, without a glide.
- Mix 0 is the dry track: under the folded identity the state would keep ringing (`y = x + z1`), so once the mix has settled at 0 the ring is faded out over one block, the state is zeroed and kept at zero, and the
  output is a copy of the input bit for bit (no filter work while the mix is 0).
- Known limits: a second Mode change while a crossfade is under way starts from the unfinished target, not from the blend, so the numerator steps once (change the mode at most once per 25 ms); block-size
  invariance (same bits at any block size) holds for static parameters only, a moving parameter glides once a block.
- Tail (the OS-INSFX S2 contract): `render` returns 1 while the input is live or the state is over 1e-7 (-140 dB), 0 when both are silent; a state under 1e-7 (or NaN, or infinite) is flushed to exact 0 at the end of the
  block (no subnormals, an exact end of the tail).
- One view, **Response**: the magnitude in dB over 20 Hz to 20 kHz with the mode's name, from the values the page hands over (the mix included).

## Cost

Static (packed file): hot part 1,136 B (the render, 176 B of it the mix-0 fade path), cold 2,144 B, file 3,444 B, state 100 B (a fifth of the 512 B the bench copies). Sample loop 14 instructions. Estimates [est, not measured on the unit]: about 1.2k cycles
a block settled and 1.9k while the cutoff moves every block (0.5 % and 0.7 % of a 64-frame block). Measure with `bench insert 64` on a bench image (rows `insert_effect` and `insert_effect_mod`), see `docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_biquad_test o3m_biquad_tmod
    ctest --test-dir build-os-host -R biquad --output-on-failure
