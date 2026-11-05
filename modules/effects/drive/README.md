# Drive (module `drive`)

A saturation effect with a dry/wet mix for a **track insert** of the Community OS (OS-INSFX-FX1, module 1 of the first effect pack; `docs/roadmap/BACKLOG.md`), loaded from the SD card as
`/modules/effects/drive.tmod` and bound with `diag ins bind <track> <slot> drive` after `mod load` (`docs/specs/OS-INSFX.md`; the Mixer's INSERTS page and the parameter page come with S4 and S5).
It is **not** built into the image. Status: new Community OS behaviour; tested on the host through the ABI against a double-precision reference of the same maths (`tests/drive_test.cpp`, ctest
`o3m_drive_test`); **its cost and its sound are not verified on the unit** (`platform/mk66/tools/hil/qa_drive.json`, QA-GLB-174..176).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: every formula is this module's own (a polynomial soft clipper, a one-pole
low-pass, a one-pole DC blocker), written for it; no code of any note or library is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Shape | 0..1 | 0 | 0 SOFT (symmetric), 1 ASYM (asymmetric: even harmonics, and a DC offset, which the DC blocker removes); shown as a number, the view names it; a change glides through intermediate biases (about 6 ms per block step, no step) |
| 1 | Drive | 0..36 dB | 12 | modulation target; the input gain 10^(dB/20), 1 at 0 dB and 63 at 36 dB |
| 2 | Tone | 500..20000 Hz, log | 8000 | modulation target; a one-pole low-pass after the shaper, clamped to 0.45 of the sample rate |
| 3 | Mix | 0..100 % | 100 | modulation target; out = (1 - m) Output x + m Output wet; 0 is the input bit for bit (at Output 0 dB) |
| 4 | Output | -24..+6 dB | 0 | modulation target; the output level 10^(dB/20), folded into the mix's two coefficients |

All five will be learnable with MIDI Learn (S7). `set_param` only stores a target, so a modulation source may call it every block.

## How it works

- Shaper: `v = clamp((2/3) g x + b, -1, 1)`, `y = v (1.5 - 0.5 v^2) - b (1.5 - 0.5 b^2)` — a polynomial soft clipper, bounded (by 1.37) whatever the drive, with slope 0 at the knee (|v| = 1), so the curve joins the clamp smoothly, and passing through 0 at zero input. The bias `b` is the Shape: 0 is SOFT (the curve is odd), 0.25 is ASYM (not odd: even harmonics plus a DC offset on any signal, which the DC blocker removes). Float only, no division and no library call in the sample loop.
- **Gain law:** the `2/3` is folded into the control-rate gain, so the small-signal gain is `1.5 * 2/3 = 1` exactly: at Drive 0 dB a quiet signal passes at unity (Mix 100, Output 0 dB: wet equals dry within 0.1 dB, and a Mix sweep 0..100 % moves the level by less than 0.2 dB — tested). Full scale at 0 dB compresses to 0.85 (-1.4 dB); above about 3.5 dB of drive, full scale reaches the clamp and clips at ±1. ASYM's bias offset (`b (1.5 - 0.5 b^2)` at zero input) is subtracted in the shaper, and any DC the asymmetric curve puts on a signal is removed by the DC blocker.
- After the shaper: the tone one-pole (`ty += a (x - ty)`, `a = 1 - e^(-2 pi fc / fs)`), then the DC blocker (`y = x - x1 + R y1`, `R = 0.995`, about 35 Hz at 44.1 kHz, always on), then the mix and the output level.
- Once a block (1..128 frames) the smoothed drive (dB), tone (log2 Hz), output (dB), mix and bias move a fraction of the way to their targets (k = frames / 256: about 6 ms at 64 frames); the derived values (the gain, the tone coefficient, the output level) are recomputed only while the drive, tone or output moves (four host exp2 calls, never per sample), and the five per-sample values ramp linearly in steps of 16 samples from the last block's to the new ones. The first parameters after `init()` (the stored blob) are applied at once, without a glide.
- Known limits: **no oversampling**, so a high drive aliases (the harmonics the shaper adds fold back over the Nyquist frequency); block-size invariance (same bits at any block size) holds for static parameters only, a moving parameter glides once a block.
- Tail (the OS-INSFX S2 contract): `render` returns 1 while the input is live or the state is over 1e-7 (-140 dB), 0 when both are silent; a state under 1e-7 (or NaN, or infinite) is flushed to exact 0 at the end of the block (no subnormals, an exact end of the tail).
- One view, **Curve**: the shaper's transfer curve at the Drive and Shape the page hands over (65 points in a frame, with the zero line and the shape's name).

## Cost

Static (packed file, from the mk66 build): hot 1,328 B, cold 1,648 B, file 3,108 B, state 88 B (the o3m_pack line of the CI build). Sample loop about 25 instructions (8 of them multiplies), no VDIV and no call in it; the dependent chain of the
shaper + tone + DC blocker + mix is about 29 cycles a sample. Estimates [est, not measured on the unit]: about 1.9k cycles a block settled and 2.5k while a parameter moves every block (0.7 % and 1.0 % of a
64-frame block of 261,225 cycles) — about the biquad's 1,666. Measure with `bench insert 64` on a bench image (rows `insert_effect` and `insert_effect_mod`), see `docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_drive_test o3m_drive_tmod
    ctest --test-dir build-os-host -R drive --output-on-failure
