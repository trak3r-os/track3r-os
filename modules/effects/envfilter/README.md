# Env Filter (module `envfilter`)

An envelope-controlled filter (an auto-wah) with a dry/wet mix for a **track insert** of the Community OS (OS-INSFX-FX1, part of the first effect pack; `docs/roadmap/BACKLOG.md`), loaded from the SD card as
`/modules/effects/envfilter.tmod` and bound with `diag ins bind <track> <slot> envfilter` after `mod load` (`docs/specs/OS-INSFX.md`; the Mixer's INSERTS page and the parameter page come with S4 and S5).
It is **not** built into the image. Status: new Community OS behaviour; tested on the host through the ABI against a double-precision reference (`tests/envfilter_test.cpp`, ctest `o3m_envfilter_test`); **its cost and its
sound are not verified on the unit** (`platform/mk66/tools/hil/qa_envfilter.json`, QA-GLB-205..208).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none taken: the filter is the topology-preserving (trapezoidal) state-variable structure, a
published one, written here from its equations; the follower, the sweep law, the polynomials for log2, 2^x and tan, the smoothing and the mode weights are this module's own; no code of any note or library is used
(`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Mode | 0 LP, 1 BP, 2 HP | 0 | not a modulation target. A switch glides as a cross-fade of the three outputs (about 4 ms), so it does not click. BP has unity gain at its cutoff whatever the resonance |
| 1 | Cutoff | 20..20000 Hz, log | 400 | modulation target; the base the follower sweeps from (the cutoff with the follower at the bottom of its window). First MOD target, so the unit bench's moving row sweeps it |
| 2 | Range | -6..+6 octaves | +4 | modulation target; how far the follower moves the cutoff from the base at the top of its window: up for a positive Range, down for a negative one (the envelope closes the filter) |
| 3 | Resonance | 0..100 % | 0 | modulation target; Q from 1/sqrt(2) (the Butterworth) to 10 on an exponential curve. 0 never raises the level |
| 4 | Attack | 0.5..200 ms, log | 5 | the follower's rise time (time to cover 63 % of a step up) |
| 5 | Release | 5..2000 ms, log | 150 | the follower's fall time (63 % of a step down) |
| 6 | Sens | 0..+36 dB | +12 | modulation target; how far under full scale the follower must be to open the filter fully: +12 dB opens it at -12 dBFS, +24 dB at -24 dBFS; the window below it is 48 dB |
| 7 | Mix | 0..100 % | 100 | modulation target; 0 is the input bit for bit |

All eight will be learnable with MIDI Learn (S7). `set_param` stores the target and evaluates a polynomial or two at most (Resonance, Cutoff, Attack, Release), so a modulation source may call it every block.

## How it works

- **Filter:** the trapezoidal state-variable filter, `g = tan(pi fc / fs)`, `k = 1/Q`, `a1 = 1/(1 + g (g + k))`, `a2 = g a1`, `a3 = g a2`; per sample `v3 = x - ic2`, `v1 = a1 ic1 + a2 v3`, `v2 = ic2 + a2 ic1 + a3 v3`, `ic1 = 2 v1 - ic1`,
  `ic2 = 2 v2 - ic2`. LP is `v2`, BP is `k v1`, HP is `x - k v1 - v2`. The mode and the mix fold into three coefficients, `out = A x + B v1 + C v2` (three multiplies a sample, whatever the mode).
- **Follower:** on the effect's own input (the audio sidechain comes with OS-INSFX-ABI2 A6): `e += c (|x| - e)` with `c` the Attack's one-pole coefficient `1 - exp(-1/(T fs))` while `|x|` is above `e`, the Release's below. A compare and a branch between the attack and the release path a sample.
- **Sweep, every 16 samples:** `u = clamp(1 + (log2 e + Sens) / 8, 0, 1)`, a window of 48 dB; `fc = Cutoff 2^(Range u)`, kept between 20 Hz and 0.4 fs. The log2, 2^x and tan are this module's polynomials (1e-7 relative for 2^x, 2.4e-6 absolute for log2,
  a (5, 6) Pade for tan, 4e-8 relative): no host maths call in `render`, no `blx`. One division for tan and one for `a1` per 16 samples.
- **Level law (the lesson of DRIVE, which came out +12 to +16 dB over bypass at its defaults):** at the defaults the filter is a low-pass of Q = 1/sqrt(2), a Butterworth, whose gain is at most 1 at every frequency, swept by the follower: the output rms is
  **never above the input's**. The test scans sines (100, 440, 1234.5 and 3000 Hz) and noise from -60 to -6 dBFS, in every mode at Resonance 0, whole runs from rest (measured worst: 0.0015 dB under bypass). The cost is level: the defaults take
  the highs off a quiet signal (a -60 dBFS noise comes out about 16 dB down, a -6 dBFS one 7 dB down) and that is what a low-pass does; the sweep opens it as the signal gets louder. A resonance over 0, a Mix under 100 % at a high Range or the
  band-pass at a resonant peak can be louder than bypass (up to +20 dB at the top of Resonance, at the cutoff): there is no make-up or limiter, and the output past +-1 is the converter's to clip.
- **Stability:** the bilinear transform puts the discrete poles inside the unit circle for every `g > 0` and `k > 0`, whatever the cutoff and the Resonance (the test computes the pole radius over a grid: 0.99986 at the worst corner, 20 Hz and Q = 10). A static filter is
  linear and time-invariant, so `|y|` is under `||h||_1 max|x|` exactly: the test measures `||h||_1` from the impulse response of every mode at Q = 10 (13.8 at the worst, the low cutoffs; the static filter's peak reaches 0.9999 of it) and runs 8 s of full-scale noise and of a square wave at the cutoff under it.
  With the cutoff slammed from end to end every 3 control steps (Range +-6, the fastest Attack and Release, bursts of noise) for 20 s the output stays under twice that bound and finite (the measured peak is 4.6, the bound 27.6).
- **Smoothing:** Cutoff, Range, Resonance, Sens, Mode and Mix move by 1/10 of the distance every 16 samples (3.6 ms), landing exactly on the target when 1e-5 away, so a parameter change does not zipper and Mix 0 is the input bit for bit after it has arrived. Before the first block a value
  snaps instead. Attack and Release act at once.
- **Tail (the OS-INSFX S2 contract):** `render` returns 1 while the input is live (its last sample is not 0) or a filter state is over 1e-6 (-120 dBFS), 0 when both are silent; it then flushes the states and the follower to exact zeros (the follower forgets: a note after a
  silence starts from the base cutoff). A high-Q low cutoff rings long (20 Hz at Q = 10: a few seconds, the test allows 5.8 s; 100 Hz at Q = 10 0.34 s) and the strip stays busy for that long.
- **Hostile input:** the filter is recursive, so a NaN, an infinity or a huge sample taints the rest of that block's output; at the end of the block a non-finite or absurd (over 1e18) state is flushed and the next block is clean (the test compares it with a fresh instance, bit for bit).
- Known limits: block-size invariance (same bits at any block size) holds for a static cutoff (Range 0), and for any sweep when the block sizes are multiples of 16; otherwise the control step falls at different samples (the sweep differs by a fraction of a millisecond).
  No oversampling, no saturation inside the filter (a resonant peak is as linear as the float is), no stereo.
- One view, **Response**: the magnitude response at the base cutoff (a filled curve, with the mode and the Mix applied) and, as a line, at the cutoff the follower reaches when it is fully open (`Cutoff 2^Range`); it draws from the parameters the page hands over, not the state.

## Cost

Static (packed file, from the mk66 build, the o3m_pack line of the CI build): envfilter.tmod 4,276 B, hot 1,168 B, cold 2,912 B, state 108 B. The hot part has 322 instructions, 19 % FP loads and stores (the control step's slot arrays; the sample loop has two),
two VDIV (the control step's tan and `a1`) and no blx and no call anywhere. The sample loop is about 35 instructions (a dependent chain through the two integrators) with no VDIV and no call.
Estimates [est, not measured on the unit]: about 40 cycles a sample and about 140 for each of the four control steps of a 64-frame block, so about 3.2k cycles a block settled and 3.5k while a parameter moves (1.2 % and 1.3 % of a block of 261,225 cycles), against the biquad's measured
1,666 and DRIVE's 5,046 with its adapter (about 2.4k of it): about 5.5k to 5.9k with the adapter, well under the 3 points (7.8k cycles) of a track insert. Measure with `bench insert 64` on a bench image (rows `insert_effect` and `insert_effect_mod`), see `docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_envfilter_test o3m_envfilter_tmod
    ctest --test-dir build-os-host -R envfilter --output-on-failure
