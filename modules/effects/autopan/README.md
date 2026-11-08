# Autopan (module `autopan`)

A stereo auto-pan for a **track insert** of the Community OS (OS-INSFX-FX2, slices X4 and X5; `docs/specs/OS-INSFX-FX2.md` section 4.4, `docs/roadmap/BACKLOG.md`), loaded from the SD card as `/modules/effects/autopan.tmod` and
bound with `diag ins bind <track> <slot> autopan` after `mod load`. It is **not** built into the image. A WIDENS effect (ABI 1.12): it makes a stereo image from a mono track, so it needs OS-INSFX-ABI2 A4 (the stereo
adapter) on the unit, and A3 for the lock to the pattern's beat. Status: new Community OS behaviour; tested on the host through the ABI against a double-precision reference (`tests/autopan_test.cpp`, ctest
`o3m_autopan_test`); **its cost and its sound are not verified on the unit** (`platform/mk66/tools/hil/qa_autopan.json`, QA-GLB-224..227).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: the pan law, the LFO and the filters are this module's own; the HEADPHONE mode uses the textbook
spherical-head idea (an interaural time difference and a head-shadow level-and-tone difference), with no code or data taken (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

Effects have no value labels in ABI 1.12 (labels are an instrument's, ABI 1.8): the page shows the number, and this table says what it means. The spec's `Rate` (Hz) and `Mode` (SPEAKERS / HEADPHONE) are a `Period` and a
`Headphone` here, for that reason and for the whole-number display of a parameter with a unit (see the file's header).

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Sync | 0..15 (int) | 3 | the cycle's length in sequencer steps (a beat is 4 steps): 0 FREE (the Period rules), 1 = 64 steps, 2 = 32, **3 = 16 (a bar)**, 4 = 12, 5 = 8, 6 = 6, 7 = 4 (a beat), 8 = 3, 9 = 2, 10 = 3/2, 11 = 1, 12 = 3/4, 13 = 1/2, 14 = 1/3, 15 = 1/4 |
| 1 | Period | 100..20000 ms, log | 1000 | modulation target; the cycle's length while Sync is 0 (the spec's Rate 0.05..10 Hz; 1000 ms is 1 Hz) |
| 2 | Depth | 0..100 % | 70 | modulation target; the pan's extent: p = Depth x LFO, from -1 (all right) to +1 (all left) |
| 3 | Shape | 0..2 (int) | 0 | 0 SINE (a parabolic approximation, within 0.11 % of sin, no libm), 1 TRIANGLE, 2 SQUARE (through a one-pole of about 5 ms, so it does not click). All start at 0 and rise |
| 4 | Phase | 0..360 degrees (int) | 0 | modulation target; the LFO's offset |
| 5 | Headphone | 0..1 (bool) | 0 | OFF: SPEAKERS, the mono-compatible law. ON: HEADPHONE, the binaural cue (X5): **NOT mono-compatible**, see below |

`set_param` only stores a target, so a modulation source may call it every block.

## Presets (the generator checks every value)

`Slow Sine` (Sync 3 = 16 steps, Depth 70, SINE), `Tri 8th` (Sync 9 = 2 steps, Depth 80, TRIANGLE), `Square 16th` (Sync 11 = 1 step, Depth 60, SQUARE), `Gentle` (Sync 2 = 32 steps, Depth 35, SINE, Phase 90). The
spec's values (A-7: the slice may retune them by ear): unchanged, written as the Sync's index (the preset's Period is the default and is unused with a Sync). Headphone is OFF in all four.

## How it works

- **SPEAKERS (Q5 a, the linear law):** with `M = (L + R) / 2`, `S' = S + p M`: `L' = L + p M`, `R' = R - p M`. The same number is added to one channel and taken from the other, so **the mono sum is the input's mid at every
  setting** (the host test: under 1e-6 at 90 settings, where 1 LSB of 16 bits is 3e-5), Depth 0 is the input bit for bit, and a stereo input keeps its own side. The sides get 3 dB more power than the centre (the price of the
  law; constant power would dip the mono sum).
- **The LFO** is computed once a block and the pan is ramped linearly from where the last block ended to the block's end value: a depth or phase move, a clock jump or a shape change is a one-block ramp, never a step.
  SQUARE's value goes through a one-pole of 5 ms run once a block (exact at the block's end).
- **Sync:** while the sequencer plays, the phase at the first sample of a block that starts a chunk is `frac(beat x 4 / steps)` from `get_clock` (the beat counts from step 0 of the pattern that plays), plus Phase: a bar-aligned pattern swings
  on the beat and a jump re-aligns in the block (the clock's WRAPPED flag needs no handling: the phase is the beat's at every block that starts a chunk, and a chunk starting mid-block keeps the phase the previous chunks advanced). While the sequencer does not play, the phase **free-runs at the tempo in force** (the
  project's while stopped; 120 BPM when the OS answers no tempo). **Before OS-INSFX-ABI2 A3 the clock is a stub** (stopped, the project's tempo, beat 0): the synced pan runs at the right speed but is not locked to step 0.
  The module only reads the ABI, so nothing changes in it when A3 lands.
- **HEADPHONE (X5):** the pan p drives two cues, derived every chunk (two host exp2) and stored in the state, then ramped. The far ear (R for p > 0) is delayed by `0.6 ms x |p|` (0 to 26.5 frames at 44.1 kHz; linear interpolation on a 32-frame ring an ear, 256 B in
  the state) and darkened by a one-pole low-pass whose corner falls from 20 kHz at the centre to 1.8 kHz at |p| = 1 (`fc = 20000 (1800 / 20000)^|p|`). The near ear keeps SPEAKERS' level law and
  the centre's 20 kHz corner, so at p = 0 both ears are the same. **Not mono-compatible**: the delayed ear combs in a mono sum (a hollow sound on a mono speaker, a mono headphone or a mono fold-down), so it is off by
  default and meant for headphones. The parameter page has no help line: this is where it is said (and in the manual's effects page once it exists). Switching on (also mid-chunk) primes the ring and the filters with the first frame and snaps the cues to the chunk's stored targets (no
  dropout); switching off is a hard switch back to the direct pan.
- **Tail (the OS-INSFX S2 contract):** SPEAKERS holds no memory: `render_st` returns 1 for a block whose input is not all zero, else 0. HEADPHONE returns 1 while the input lives, the 32-frame ring may still hold signal
  or a low-pass rings (|lp| >= 1e-7), then 0, and the low-pass is flushed to exact zero, so the tail ends at exact zeros. A NaN, infinite or huge input is not allowed to stay in the ring (it is primed again from the next
  block); SPEAKERS has no state the input can reach.
- No view, no `views` (the parameter page is enough). The mono `render` the loader requires is the input unchanged (the added side cancels); no host of this OS calls it for a WIDENS effect.

## Cost

Static (the packed file, from the cross build): **hot part 1,488 B** (the shared SRAM_L slab's 1.5 KB rule; 444 instructions, 16 % FP loads and stores, 2 vdiv a block outside the sample loops, 4 calls (get_clock and three host exp2)
and none in a sample loop). Estimates [est, not measured on the unit]: SPEAKERS about 12 instructions and about 14 cycles a sample, so about 0.9k cycles a block (0.35 %) plus the control step (about 0.4k, two host exp2 a chunk for the cue
targets; the spec said about 0.4k); HEADPHONE adds a second pass of about 45 instructions a sample, about 55 cycles, so about +3.5k cycles (1.3 %) while on (the spec said +1.0k [est]: the cost of two float ears, two interpolated reads and two
low-passes; the ring is in the SRAM_U arena if it has room, else in SDRAM, where each access costs more: measure both). With the stereo adapter (about 4.5k to 5.5k [est]) the insert is about 6.5k (2.5 %) in SPEAKERS and
about 10k (3.9 %) in HEADPHONE, **inside the 4 points (10.4k) of a WIDENS insert** (Q4 b) but with little room in HEADPHONE. State 404 B (408 B in the 64-bit host build). Measure with `bench insert 64` on a bench image, SPEAKERS and then HEADPHONE (the HIL file
`qa_autopan.json`, QA-GLB-226), see `docs/architecture/cpu-budget.md`.

## Build and test

    cmake --build build-os-host --target o3m_autopan_test o3m_autopan_tmod
    ctest --test-dir build-os-host -R autopan --output-on-failure
