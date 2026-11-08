# Fold (module `fold`)

A wavefolder with a dry/wet mix for a **track insert** of the Community OS (OS-INSFX-FX1, module 3 of the first effect pack; `docs/roadmap/BACKLOG.md`), loaded from the SD card as `/modules/effects/fold.tmod` and bound
with `diag ins bind <track> <slot> fold` after `mod load` (`docs/specs/OS-INSFX.md`; the Mixer's INSERTS page and the parameter page come with S4 and S5). It is **not** built into the image. Status: new Community OS
behaviour; tested on the host through the ABI against a double-precision reference of the same maths (`tests/fold_test.cpp`, ctest `o3m_fold_test`; the reference is independent only in the fold itself, by fmod
against the module's add-and-subtract rounding, so what it proves is that the module's float path matches the double maths sample by sample over the whole grid of settings, max error 1e-5 and SNR over 90 dB, not that the maths is right); **its cost and its sound are not verified on the unit**
(`platform/mk66/tools/hil/qa_fold.json`, QA-GLB-201..204).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: the triangle fold, its level compensation, the tone and the DC blocker are this module's own,
written for it; no code of any note or library is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Fold | 0..24 dB | 9 | modulation target; the drive into the folder: the signal folds back once it passes `1/g` of full scale, `g = 10^(Fold/20)` (9 dB: above 0.35, 24 dB: above 0.063). Level-compensated: see "Level law". Listed first so that the unit bench's moving-parameter row (the first MOD target) sweeps the derivation, the worst case |
| 1 | Symmetry | -100..+100 % | 0 | modulation target; an offset `b = 0.009 per %` before the fold: one side's peaks reach the threshold first and fold (even harmonics, and a DC offset that the blocker removes). A signal that stays under the threshold on both sides never sees it. 0 is a symmetric fold (odd harmonics only) |
| 2 | Tone | 500..20000 Hz, log | 12000 | modulation target; a one-pole low-pass after the fold (the aliasing, and the buzz of the kinks) |
| 3 | Mix | 0..100 % | 100 | modulation target; out = (1 - m) Output x + m Output wet; 0 is the input bit for bit (at Output 0 dB) |
| 4 | Output | -24..+6 dB | 0 | modulation target; the output level 10^(dB/20), folded into the mix's two coefficients. A hard fold loses level (it bounds the wet at `1/g`): this is the make-up |

All five will be learnable with MIDI Learn (S7). `set_param` only stores a target, so a modulation source may call it every block.

## How it works

- **Folder:** a triangle fold of period 4: `fold1(u) = u` for `|u| <= 1`, reflected at +-1 beyond (`fold1(1 + d) = 1 - d`). With `u = g x + b` the wet signal is `(fold1(u) - b) / g`. In float: `t = g x - (1 - b)`,
  `e = t - 4 round(t / 4)` (the round is the add and subtract of 1.5 * 2^25, which leaves a multiple of 4: exact, round to nearest even, no int conversion and no call; `e` is in [-2, 2]), `fold1(u) - b = (1 - b) - |e|`:
  seven operations a sample. The 1/g is folded into the mix coefficient and applied after the filters, so the filters' memory is always on one scale (`|fold1(u) - b| <= 1`) and a moving Fold cannot mix two.
- **Level law (the lesson of DRIVE, which came out +12 to +16 dB over bypass at its defaults):** the drive into the folder is divided by its own gain. Below the threshold `1/g` the wet is the input (the small-signal gain is
  exactly 1 whatever the Fold, a linear path), and above it the signal folds back. `fold1` has slope +-1 and `fold1(b) = b`, so `|wet| <= |x|` at every sample **at a static setting**. While the Fold moves (a modulation source calling `set_param` every block) the per-sample bound does not hold: a falling Fold
applies a larger `1/g` to the filters' memory, which was filled at the old, larger g, so a sample can exceed `|x|` (the host test with the Fold and the Symmetry moved every block, `tests/fold_test.cpp` 574-597, bounds the peak at 2
against an input capped at 1 and still keeps the output rms under the input's). The tone (one pole, gain at most 1) and the
  DC blocker (`y = k (x - x1) + R y1`, `R = 0.995`, `k = (1 + R) / 2`: gain 0 at DC and exactly 1 at its peak, at Nyquist) cannot raise it either, so **at Output 0 dB the output rms is never above the input's at ANY
  setting**: the host test scans sines and noise from -60 to 0 dBFS at the defaults, then Fold 0..24 dB, Symmetry -100..+100 %, Tone 500..20000 Hz and Mix 50 and 100 % (the largest rise over bypass is under 0 dB; the
  model of the module gives -0.03 dB at worst; with the Fold and the Symmetry moved every block the rms also stays under the input's, `tests/fold_test.cpp` 574-597). The price: a hard fold is quieter than the source (a sine at 0.5 folded at 12 dB has a fundamental of 22 % of the input's), and Output +6 dB is the only make-up. A classic
  `fold(g x)` would be g times louder on every quiet signal; that is the choice not made here.
- **Defaults:** Fold 9 dB, Symmetry 0, Tone 12 kHz, Mix 100 %, Output 0 dB: a track whose peaks are under -9 dBFS passes unchanged (less the tone's small loss, -0.26 dB at 3 kHz, and the DC blocker's, about 1.7 dB at 50 Hz: the price of the 35 Hz corner, `R = 0.995` in the code), and one above it folds the peaks back.
- **Anti-aliasing (the choice):** no oversampling. A triangle fold has slope kinks, so its harmonics fall at 12 dB an octave (like a triangle wave); the ones that fold back are the high ones. A 2x oversampled fold would double
  the sample loop and add two half-band filters (about +70 % of the block's cost [est], about 3.2k cycles instead of 1.9k) to help only a hot, bright source at a high Fold, and the cost is paid on every insert of up to
  four. TONE, a one-pole low-pass after the fold, takes the top of the aliased and the kinked content for three operations a sample (the host test: a 5 kHz sine at 0.9 folded 24 dB has more than 17 dB less energy above
  12 kHz at Tone 1 kHz than at 20 kHz). The pole sits in the wet path before the mix, so at the Tone's bottom, 500 Hz, it dulls all the wet, not only its top: -3 dB at 500 Hz, -7 dB at 1 kHz, -29 dB at Nyquist (the code: `zty += a (y - zty)` before the mix). **Known limit:** it does not remove an alias that landed below the cutoff, and at Fold 24 dB on a bright source some remains. A 2x mode is a later option if the ear asks for it.
- **DC blocker (always on):** the one-sided fold (Symmetry over 0) has a DC offset; the blocker's corner is the same as DRIVE's (about 35 Hz at 44.1 kHz). It rings out in a few thousand samples after a signal; the tail rule below ends it.
- Once a block (1..128 frames) the smoothed fold (dB), symmetry, tone (log2 Hz), output (dB) and mix move k = frames / 256 of the way to their targets (about 6 ms at 64 frames); g, the tone coefficient and the output level
  are derived again only while the fold, tone or output moves (four host exp2 at most, never per sample), and the five per-sample values ramp linearly in steps of 16 samples; the 1/g is one division a step, outside the
  sample loop.
- Tail (the OS-INSFX S2 contract): `render` returns 1 while the input is live or the state rings, 0 when both are silent; a state under -140 dB is flushed to exact zero, and so is a NaN or infinite one (a NaN in the input
  spoils the rest of its block, as in DRIVE, and is gone at the block's end). The wet at x = 0 is exactly 0 for every Symmetry.
- One view, **Curve**: the wet transfer curve at the Fold and Symmetry the page hands over (65 points in a frame, the zero line, "FOLD" or "ASYM"): a line of slope 1 up to the threshold, then reflected.

## Cost

Static (packed file, from the build): see the o3m_pack line of the CI build; the sample loop is about 22 instructions with no division and no call (7 for the fold, 3 for the tone, 4 for the blocker, 3 for the mix, the
load and the store), a dependent chain of about 15 floating-point operations. Estimates [est, not measured on the unit]: about 26 cycles a sample, so about 1.9k cycles a block settled with the control step (0.7 % of a
64-frame block of 261,225 cycles), about 2.5k while the Fold moves every block (the derivation's four exp2, about 600; 1.0 %), the biquad's measured 1,666 for comparison (which came out 39 % over its own estimate), and well
under the 3 points (7.8k cycles) of a track insert. Measure with `bench insert 64` on a bench image (rows `insert_effect` and `insert_effect_mod`), see `docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_fold_test o3m_fold_tmod
    ctest --test-dir build-os-host -R fold --output-on-failure
