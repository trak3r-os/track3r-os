# Stereo Chorus (module `chorus`)

A stereo chorus for a **track insert** of the Community OS (OS-INSFX-FX2 X2, the second module of the stereo pack; `docs/specs/OS-INSFX-FX2.md` section 4.2), a **WIDENS** effect (ABI 1.12): a mono sound gets a stereo image, and a
stereo voice keeps its own. Loaded from the SD card as `/modules/effects/chorus.tmod` and bound with `diag ins bind <track> <slot> chorus` after `mod load` (`docs/specs/OS-INSFX.md`). It is **not** built into the image. It
needs an image with OS-INSFX-ABI2 A4 (the stereo track adapter): before it the OS never calls `render_st` and the insert is silent. Status: new Community OS behaviour; tested on the host through the ABI against a
double-precision reference of the same maths (`tests/chorus_test.cpp`, ctest `o3m_chorus_test`); **its cost and its sound are not verified on the unit** (`platform/mk66/tools/hil/qa_chorus.json`, QA-GLB-214 and 217).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: the delay line, the interpolation and the LFO are this module's own, written for it; no code of
any chorus or library is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Period | 200..20000 ms, log | 1667 | modulation target; the LFO's period. **A period in ms where the spec says a Rate of 0.05..5 Hz (0.6 Hz default):** the OS shows a parameter with a unit as whole numbers, so a rate in Hz would read 0, 1 or 5 (SDK README, "Choosing a parameter's type"; the PHASER does the same). 1667 ms is 0.6 Hz |
| 1 | Depth | 0..100 % | 40 | modulation target; the swing of each tap, 100 % = 8 ms. Limited to what the Delay leaves: at most Delay - 66 frames (at Delay 5 ms the swing stops at 3.5 ms) |
| 2 | Delay | 5..25 ms | 12 | modulation target; the centre of the two taps. A move of it is a slow pitch glide (about 93 ms) |
| 3 | Mix | 0..100 % | 50 | modulation target; 0 is the input bit for bit, 100 % the wet mid (the VIBRATO preset) |

Presets (the ABI2 factory presets; Period, Depth, Delay, Mix): `Classic` (1667, 40, 12, 50: the spec's 0.6 Hz), `Slow Wide` (6667, 70, 18, 60: 0.15 Hz), `Vibrato` (222, 25, 6, 100: 4.5 Hz), `Ensemble` (455, 30, 20, 45: 2.2 Hz). The
spec's values, the rates turned into periods (A-7: not retuned by ear yet).

All four are learnable with MIDI Learn (S7). `set_param` only clamps and stores a target, so a modulation source may call it every block.

## How it works

- **The line and the taps.** One line of the mid `M = (L + R) / 2` (2,048 floats, 46 ms, in the module's state: 8.4 KB, SDRAM), two taps read with linear interpolation: tap 1 at `Delay + swing`, tap 2 at
  `Delay - swing` (one LFO, the two taps a half cycle apart), so **the two delays always add up to 2 x Delay**. The LFO is a parabola refined to a sine (error 0.1 %), no libm.
- **The output keeps the input's side and crossfades its mid:** `L' = L + Mix (tap1 - M)`, `R' = R + Mix (tap2 - M)`. So Mix 0 is the input bit for bit, a stereo voice keeps its side `(L - R) / 2` (the effect adds
  `Mix (tap1 - tap2) / 2` to it), and the level does not rise with Mix. The mono sum is `M' = (1 - Mix) M + Mix (tap1 + tap2) / 2`: a mild, even chorus, **not exactly M** (WIDE is the mono-exact widener; the mono
  compatibility of the chorus is documented, not exact: FX2 spec R4).
- **The spec's formula and this one.** The spec's section 4.2 writes `L = dry + Mix x tap 1`; with it Mix 100 % is the dry sound plus a full copy (louder, not a vibrato) and the VIBRATO preset (Mix 100) would
  not be one. The crossfade of the mid above keeps everything else the spec says (the taps, the dry side, Mix 0 = the input) and makes the presets mean what their names say. One line of the code and of the
  oracle to change back if the maintainer wants the sum.
- **Only behind the block.** The shortest tap is 66 frames (1.5 ms), so a 64-frame block reads what the blocks before it wrote (the send delay's rule, FX2 spec section 3): the span each tap reads (the block plus the tap's move over it,
  at most 132 frames) is copied once a block into a scratch on the stack, the interpolation runs on it, and the new frames go into the line in one run.
- **Control once a block.** `render_st` is cut into chunks of at most 64 frames. Per chunk the smoothed Period and Mix (k = frames / 256, about 6 ms) and Delay and Depth (k = frames / 4096, about 93 ms) move toward their
  targets; the LFO is evaluated at the chunk's first and last sample and the two delays and the mix ramp linearly between them. The first parameters after `init()` are applied without a glide. At a swing of 8 ms and
  5 Hz the chord of the ramp is 0.09 frame off the true sine at the chunk's middle (the oracle models the ramp, so the test sees none).
- **Tail** (the OS-INSFX S2 contract): `render_st` returns 1 while the input is live (a mid peak of 1e-7, -140 dB, or more) or the line may still hold it (33 ms: Delay 25 + Depth 8), then 0. A chunk whose peak is under that writes exact zeros into the line, so once the last live frame is more than the longest tap behind the write head, every slot a tap can still reach holds a zero: the tail ends at exact zeros with **no one-shot clear of the 2,048 floats** (that clear was a ~10k-cycle spike in a single block; the taps reach at most `tail` frames behind the write head, and all of those slots were written by quiet chunks). A fresh effect with a silent input is idle (0). The input is finite by ABI — the stereo adapter builds the L and R planes from Q15 int16 (`InsertHost::stereo_block`, `audio/src/insert_host.cpp`) and the output side maps a NaN to 0 (`to_q15`, the module rule) or scrubs it (the master insert, S6) — so no NaN or infinity can reach the line from the OS and the sample loop keeps no per-sample guard for one.
- **The mono `render`** (the loader requires one of every effect; no host of this OS calls it for a WIDENS effect) is the left output of the same code on a mono input.
- **No allocation, no libm, no host call** in the render; the packed file links no libc (the packer would refuse it).

## Cost

Static (packed file, from the build): see the `o3m_pack` line of the CI build and the test's message (hot part limit 1.5 KB, state 8,448 B). Estimates [est, not measured on the unit]: per frame two interpolated taps
(an int conversion, two loads and four floating-point operations each) and the output arithmetic, about 30 cycles a frame, plus the two span copies (about 130 words each) and the line write: about 2.5k cycles a block
(1.0 %); the insert with the stereo adapter (about 4.5k to 5.5k [est] a block, ABI2 A4) about 7.5k (2.9 %), against the 4 points (10.4k cycles) a WIDENS insert may take (FX2 spec Q4). Measure on the unit,
in situ (QA-GLB-217): the `ins` line with the insert bound on the playing song, and `bench stages 256` / `bench strips 256` with the insert bound and with none (the delta is the insert's cost, adapter
included), as in QA-GLB-223. **`diag bench insert` cannot time this module:** the bench's state buffer is 512 B (`platform/mk66/src/bench.cpp`) and the chorus's state is 8.4 KB, so the stereo rows time the built-in identity.

## Build and test

    cmake --build build-os-host --target o3m_chorus_test o3m_chorus_tmod
    ctest --test-dir build-os-host -R chorus --output-on-failure
