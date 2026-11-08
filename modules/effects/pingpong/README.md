# Ping-Pong (module `pingpong`)

A tempo-synced stereo echo for a **track insert** of the Community OS (OS-INSFX-FX2, module X3 of the second effect pack, `docs/specs/OS-INSFX-FX2.md` section 4.3; `docs/roadmap/BACKLOG.md`): the echoes bounce between the left and
the right ear, with a low cut and a high cut in the loop, a feedback that cannot run away, a stereo width for the echoes and a mix. It is a **WIDENS** effect of ABI 1.12 (OS-INSFX-ABI2): the OS calls `render_st` on every block, with the
track's mono block on both inputs, and the track gets a stereo image from it. Loaded from the SD card as `/modules/effects/pingpong.tmod` and bound to a track insert after `mod load` (`docs/specs/OS-INSFX.md`).
It is **not** built into the image. Status: new Community OS behaviour (the official firmware has no insert effects; its send delay has a ping-pong switch and a sync rate, `docs/specs/OS-INSFX-FX2.md` F1); tested on the host through the ABI
against a double-precision reference (`tests/pingpong_test.cpp`, ctest `o3m_pingpong_test`); **its cost and its sound are not verified on the unit** (`platform/mk66/tools/hil/qa_pingpong.json`, QA-GLB-220..223).

Needs on the unit: OS-INSFX-ABI2 **A4** (the stereo track adapter: before it the OS never calls `render_st`, and the module is silent) and **A3** (`get_clock` live: before it the clock is a stub that says 120 BPM, so a synced echo does not
follow the project's tempo; FREE and the 120 BPM case work). The module is complete for both: it asks the clock every block and falls back to 120 BPM when there is no `get_clock` in the host table, when it answers non-zero (no sequencer) or
when the tempo is 0.

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: the ping-pong loop, the two one-poles, the cross-fade and the tail rule are this module's own, written for it
(the send delay's own rules in `audio/include/o3os/audio/send_delay.h` are the model: facts only, no code); no third-party code is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Sync | 0..15 (whole), labels `FREE`, `8`, `6`, `4`, `3`, `2`, `3/2`, `1`, `3/4`, `1/2`, `3/8`, `1/3`, `1/4`, `3/16`, `1/6`, `1/8` | 4 (`3`: a dotted eighth) | the echo time as a note length in **sequencer steps** (a step is a 16th note: `4` is a quarter, `1` a step), the send delay's 15 longest rates; `FREE` uses Time. The values are label indexes, the label of a value is on the parameter page. The first effect with labels (the generator takes `"labels"` for an effect since this slice, which makes it ABI 1.12) |
| 1 | Time | 10..1000 ms, log | 375 | the echo time when Sync is `FREE` (a unit parameter shows whole numbers). Not a modulation target: a change is a cross-fade of 1,024 frames |
| 2 | Feedback | 0..95 % | 45 | modulation target (the first one: the bench's moving-parameter row sweeps it); the loop gain. At most 0.95, so the loop gain is below 1 at every frequency |
| 3 | Low cut | 20..2000 Hz, log | 120 | modulation target; a one-pole high-pass in the loop: the echoes thin out as they repeat |
| 4 | High cut | 1000..20000 Hz, log | 6000 | modulation target; a one-pole low-pass in the loop: the echoes darken as they repeat |
| 5 | Mix | 0..100 % | 35 | modulation target; the wet level. 0 is the dry pair bit for bit |
| 6 | Width | 0..100 % | 100 | modulation target; 0 puts every echo in the centre (both ears the same), 100 is the full ping-pong (the first echo hard left, the next hard right) |

Presets (the generator checks every value against its range; values by the spec, section 4.3): `1/8 PING` (Sync `2`, Feedback 45, Low cut 120, High cut 6000, Mix 35, Width 100), `DOTTED 8` (`3`, 40, 150, 5000, 35, 100), `SLAP` (FREE, Time 90 ms,
10, 200, 8000, 30, 60), `DUB` (`4`, 75, 300, 2500, 40, 100), `WIDE 16TH` (`1`, 30, 200, 9000, 25, 100). The sync of a preset is the **label** the spec names, stored as its index (`2` is index 5).
No preset was retuned by ear (spec A-7): that is a check for the tester.

## How it works

- **The loop**, as the send delay's ping-pong. One line of stereo frames (a frame is one 32-bit word: the left int16 in the low half, the right in the high half). With x the mid of the input `(L + R) / 2`, `(Lr, Rr)` the line read D frames back,
  `f` the feedback filter and `g` the feedback: `Lw = x + g f(Rr)`, `Rw = g f(Lr)`. The first echo is on the left, the next on the right, the third on the left again, D apart.
  The output is `out_L = in_L + Mix (a Lr + b Rr)`, `out_R = in_R + Mix (b Lr + a Rr)` with `a = (1 + Width) / 2` and `b = (1 - Width) / 2`: the dry pair keeps the side signal a stereo voice or an earlier stereo insert brought.
- **Mono sum.** Since `a + b = 1`, `(out_L + out_R) / 2 = (in_L + in_R) / 2 + Mix (Lr + Rr) / 2` at every Width: the Width only moves the echoes between the ears and a mono listener (or a mono sum of the USB recording) hears the same echo level at any Width.
  The host test holds the sum to the reference and to itself over the Width (6e-8 apart, float rounding).
- **The filters.** `f` is a one-pole low-pass (High cut) followed by a one-pole high-pass (Low cut: the input less its own one-pole low-pass). Both have a gain of at most 1 at every frequency (the grid in the test: 0.998 at most), `g` is at most 0.95,
  so the loop gain is below unity at every frequency, a gliding coefficient included (a one-pole is a convex combination of its state and its input).
- **int16 lines, rounded toward zero.** The written value is saturated to -32768..32767 and truncated toward zero (a C cast), so a recirculating tail reaches **exactly 0**: silence in gives silence out, with no limit cycle from rounding.
  A loud input at a high feedback clips in the line (bounded, never wrapped): the wet part is never above `Mix` times full scale. The line is 44,200 frames (176,800 bytes; with the fields the state is 176,936 bytes, `state_bytes` 177,000),
  held in the module's state: the OS puts it in SDRAM (the 256 KiB a module's state may take, `kMaxStateBytes`). Four instances take 708 KB of the 768 KiB module region, so a fourth may be refused when other modules are loaded (spec R2).
- **Time.** Synced: `D = round(rate x fs x 15 / bpm)`, the rate in steps, the tempo from `o3m_host.get_clock` (120 BPM when there is none), a D over 1 s (`fs` frames) **halved until it fits** (8 steps at 120 BPM is exactly 1 s; at 60 BPM it
  becomes 4 steps' length), held to 64 frames at least. `FREE`: `D = round(Time x fs / 1000)`. A change of D (Time, the tempo, the Sync) is a **cross-fade of 1,024 frames (23 ms)** between the old tap and the new one with a linear pair
  of weights that sum to 1; a change that arrives during a fade waits for it (the newest target wins). **The fade is done on the int16 words** (`old + (new - old) x w / 1024`, the quotient rounded toward zero, per channel): that keeps the
  hot part small and the main loop a single one, and costs at most one LSB of rounding; a pair of zeros stays zero. (The send delay blends floats.) The clock is read once a block (not at all while FREE).
- **Blocks and SDRAM.** Every delay is at least 64 frames, so a chunk of 64 frames reads only words written by earlier chunks: the read span of a tap is copied once a chunk into a 256-byte scratch (one word loop, two runs at the end of the ring), all
  the arithmetic runs on the scratch and the registers, the 64 new words are written back in one loop. A block of more than 64 frames is processed in chunks of 64 (the rule needs a chunk no longer than the shortest delay), a shorter one as it is.
- **Control.** `set_param` is a clamp and a store. The two exp2 calls of the poles (High cut, Low cut) and the output coefficients are computed once, when a parameter changed. Once a chunk the five values (g, the two poles, the two output
  coefficients) move 0.2 of the way to their targets (about 7 ms at 64 frames) and the three that act per sample ramp linearly inside the chunk; the first parameters after `init()` and the first chunk after a long silence take their values at once.
- **Tail.** The module counts the consecutive zero words it has written. After a whole line (1 s) of them nothing a tail could say is left and no old audio is left to surface when Time grows: `render` returns 0 (unless the input's last
  sample is not 0), the filters are cleared, and the OS then skips the module while its input is silent. Until then it returns 1 (the host's own rule also ends a tail after 2 s of exactly zero output). A sound after a long silence starts as it is set.
- **The mono `render`** the loader requires is `render_st` on one input twice with the left output kept (no host of this OS calls it for a WIDENS effect).

## Cost

Static (the CI build's packed file, the o3m_pack line): see the commit body; the hot part is **1,472 B** (473 instructions: one sample loop of 56, the cross-fade blend, the ring copy and the control step; `-j .o3m_hot` of the build's ELF:
`instr 473  fp ld/st 43 (9%)  vdiv 2  blx 2`: the two VDIV are the synced delay's quotient and `1 / n` once a chunk, the two blx are `get_clock` and the (cold, only on a parameter change) pole derivation; the two int16 saturations are one SSAT each;
no division, call or spill in the sample loop). Estimates [est, not measured on the unit]: about 60 cycles a sample (56 instructions, four dependent one-pole updates per sample; a spec figure of 20 cycles a frame was for the
loop without the filters) so **about 3.8k cycles a block of 64 for the loop**, plus about 0.4k for the ring copy (64 words in, 64 out) and the control step: **about 4.2k cycles (1.6 % of a 261,225-cycle block)**; a block that cross-fades adds about 1.8k
(the word blend). With the stereo adapter (about 4.5k to 5.5k [est], OS-INSFX-ABI2 F8) the insert is about 9k to 10k, **at the edge of the 4 points (10.4k) a WIDENS insert is allowed** (FX2 Q4 b): **the unit decides (`bench insert 64`)**;
above the budget the slice stops for the maintainer (spec R1). The spec's own estimate (about 2.0k for the effect) did not count the two filter chains. Measure with `bench insert 64` on a bench image, see `docs/architecture/cpu-budget.md`.
**The bench cannot time this module yet**: `run_insert` and `run_insert_st` in `platform/mk66/src/bench.cpp` copy the state to a 512-byte stack buffer (a bigger state: `too big to copy`, or the stereo row times a built-in identity); the 177 KB line needs a bench change first (QA-GLB-223), an OS-code slice outside this one.

## Known limits

- The echoes are signal added to the mid: the mono sum is the input's mid plus the echoes, at any Width (checked), not the input's mid alone (WIDE and AUTOPAN keep the mid exactly; an echo cannot).
- A synced time follows the tempo only with OS-INSFX-ABI2 A3 on the unit (120 BPM before it). A tempo ramp makes a cross-fade each time the rounded delay changes by a frame.
- The parameters' glide is a one-pole over a few chunks, not block-size independent when a knob moves (the settled output is, bit for bit: the test runs blocks of 1, 7, 64, 100, 128 and 4,096).
- The tail is a whole line (1 s) of silence long in the module's own count, which only decides when it returns 0: the host's rule decides when the strip stops.

## Build and test

    cmake --build build-os-host --target o3m_pingpong_test o3m_pingpong_tmod
    ctest --test-dir build-os-host -R pingpong --output-on-failure
