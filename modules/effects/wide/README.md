# Wide (module `wide`)

A stereo widener for a **track insert** of the Community OS (OS-INSFX-FX2 slice X1, module 1 of the stereo pack; `docs/specs/OS-INSFX-FX2.md` section 4.1, `docs/roadmap/BACKLOG.md`), loaded from the SD card as
`/modules/effects/wide.tmod` and bound with `diag ins bind <track> <slot> wide` after `mod load` (`docs/specs/OS-INSFX.md`). It is **not** built into the image. It is a WIDENS effect of ABI 1.12: the OS runs it
through the stereo track adapter on every block (`render_st` on L / R float planes), so it needs OS-INSFX-ABI2 A4 on the unit; before that the OS never calls `render_st`. Status: new Community OS behaviour;
tested on the host through the ABI against a double-precision reference of the same law (`tests/wide_test.cpp`, ctest `o3m_wide_test`: the module's float path matches the double maths sample by sample over a
grid of settings, max error 1e-4 and SNR over 90 dB on the added side, and the mono sum is the input's mid), **its cost and its sound are not verified on the unit**
(`platform/mk66/tools/hil/qa_wide.json`, QA-GLB-210 and 213).

Licence: GPL-3.0-or-later like the OS (the source includes the MIT SDK header `o3module.h`). **Source of the maths:** none: a delayed, high-passed copy of the mid added to the side is a textbook widening
(Lauridsen's trick) and the code is this module's own; no code of any note or library is used (`THIRD_PARTY.md`).

## Parameters (in this order for ever: a stored parameter blob is one little-endian float32 per parameter)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Width | 0..100 % | 50 | modulation target; `w = Width / 100`, the amount of the delayed mid added to the side. 0 is the input bit for bit |
| 1 | Time | 2..30 ms | 12 | modulation target; the delay of the mid, in whole frames (22 us a step at 44.1 kHz), 64 frames at least. A move cross-fades (below) |
| 2 | Low cut | 20..1000 Hz, log | 200 | modulation target; the corner of a one-pole high-pass on the delayed mid: the bass stays centred |

`set_param` only stores a target, so a modulation source may call it every block.

## Factory presets (ABI 1.12; Width, Time, Low cut)

| name | Width | Time | Low cut |
|---|---|---|---|
| Subtle | 25 | 8 | 300 |
| Wide | 50 | 12 | 200 |
| Huge | 85 | 18 | 150 |
| Doubler | 60 | 26 | 120 |

The values are the spec's (A-7: a slice may retune them by ear; none has been, the by-ear check is owed on the unit). `Wide` is the defaults. The names are title case, as the other modules' names.

## How it works

- **The law.** With `M = (L + R) / 2` the mid and `S = (L - R) / 2` the side of the input, and `h = HP(delay(M))` the mid delayed by Time and high-passed at Low cut: `L' = L + w h`, `R' = R - w h`. That is
  `S' = S + w h` and `M' = M`: the effect **only adds to the side**. The same `a = w h` is added to one channel and taken from the other, so the mono sum `(L' + R') / 2` is the input's mid to the rounding of
  two float adds (the host test bounds it at 1e-6 over 175 settings and six input kinds; one LSB of 16 bits is 3e-5), and the two ears get complementary combs that cancel in a mono sum. **Width 0 is the input
  bit for bit** (also after a wet period, once the glide of the chunk the change lands in has ended). A stereo input (a stereo voice, or an earlier stereo insert) keeps its own side: `S' - S = w h`.
  A mono input is the case `S = 0`. A WIDENS effect gets the track's mono block on both inputs when nothing brings a side.
- **The line.** A ring of 2,048 floats of M (8 KB, in the instance block: over the 4 KiB arena, so in SDRAM; the spec's estimate was 5.4 KB for 1,324 floats, the ring is a power of two for the mask and covers
  48 kHz). The shortest delay is 64 frames (Time 2 ms is 88 frames at 44.1 kHz; at 22.05 kHz the floor of 64 applies), so a chunk of up to 64 frames reads only what earlier chunks wrote. `render_st` walks a
  block in chunks of 64: the read span is copied once from the ring to a stack scratch (one burst), the arithmetic runs on SRAM, the chunk of M is written back in one burst (the send delay's rule). A block
  of up to 128 frames is two chunks; any size from 1 gives the same bits for static settings.
- **High-pass.** One pole: `h = x - lp`, `lp += c (x - lp)`, `c = 1 - e^(-2 pi f / fs)`. The coefficient is derived again, with one host exp2, only when Low cut changed (at chunk rate); the memory is continuous,
  so a move of the corner is smooth.
- **Time moves.** A modulation source may move Time every block. The delay in force changes at a chunk boundary and the chunk cross-fades the old read span into the new one, so a move makes no click, only the
  comb moving; it lands on the side, so the mono sum stays the mid (the host test moves all three parameters every block for 700 blocks against the reference). No interpolation: the delay is a whole number of
  frames, so there is no pitch glide either. Width glides linearly over the chunk to its target. **set_param before the first block snaps** (the blob a bind applies), after it glides.
- **Mono `render`.** The loader requires one of every effect; no host of this OS calls it for a WIDENS effect. It is the mid of `render_st`: the input itself, after running the same chunk so that the line and
  the filter stay coherent. (The spec words it as "L written back"; that would put the comb into a mono output, against the mono-compatible promise. Recorded as an assumption.)
- **Tail (the OS-INSFX S2 contract).** `render_st` returns 1 while the input lives (the sum of |M| over the chunk is not 0), or while the last 30 ms (the longest Time) plus one chunk of the line may still hold
  signal, or while the high-pass rings (`|lp| >= 1e-7`, -140 dB), and then 0. `lp` is flushed to exact zero once the line is dry, so the tail ends at exact zeros. The line is dry 1,387 frames (31 ms) after the
  input stopped; the high-pass ring-out of the echo comes after the echo: about 13 ms at the default Low cut, about 130 ms at 20 Hz. So "0 once 30 ms of silence has passed" holds at the defaults and for every
  Time up to the ring-out; the host test pins 1,515 frames for Time 12 ms and Low cut 200 Hz, 2,000 for Time 30 ms, 6,000 for Low cut 20 Hz.
- **Hostile values.** Parameters clamp (a NaN falls to the lower bound; the same bits as the bound set directly). A NaN, infinite or huge input (the sum of |M| over a chunk not below 1e20) is written to the
  line as zero, so it never enters the delay or the filter: its own chunk comes out as the input does, the next is clean, and the output from then on is that of an instance that got zeros in that chunk.
- No allocation, no libm call and no division in the sample loop (one division a chunk for `1 / n`); `render_st` is the only `O3M_HOT` function and holds the three loops, the state is copied into locals around them.

## Cost

Estimates [est, not measured on the unit]. Static check of the CI build's ELF (`tmod-perf` section 5, `.o3m_hot`): `instr 214  fp ld/st 33 (15%)  vdiv 1  blx 1` (the one VDIV is `1 / n` of the glide, once a
chunk outside the loops; the one blx is the host exp2 of a Low cut move; neither is in a sample loop). The sample loop is **27 instructions** a frame (three `vldm` and three `vstm`: the two inputs, the delayed
sample, the two outputs and the store of M, then the mid, the |M| sum for the tail and the hostile-input test, the high-pass, the width ramp and the added side; state in registers, no spill); the read burst is
9 instructions a frame (the index mask, the load, the store), the write burst 8, the control step about 0.15k cycles (`1 / n`, the delay rounding, the coefficient test). With the FOLD's ratio (22
instructions came to 26 cycles) the loop is about 30 cycles a frame, so: 64 x 30 = 1.9k, plus the two bursts 64 x 17 = 1.1k (one SDRAM access of about 28 cycles opens each), plus the control: about **3.2k
cycles a block (1.2 % of the 261,225-cycle block)**; 0.4k more in a block where Low cut moves (one exp2) and 0.7k more in a block where Time moves (the cross-fade's second read burst and its multiply-add). The
spec's figure was 1.2k: it left out the burst loops and the sum for the tail. With the stereo adapter (about 4.5k to 5.5k [est], ABI2 Q7, the OS side) the insert is about 8.2k (3.1 %), inside the 4 points the
maintainer allowed a WIDENS insert (Q4: 10.4k cycles, adapter included). The FP load/store share of the whole hot part is 15 % (the target is under 15): it is the stream of the planes and the line, which has
to be read and written. **Unit measures owed:** `diag bench insert 64` (rows `insert_st_effect`, `insert_st_ramp`, `insert_st`) and `ins` on the Dense8 song with the insert on track 1 (QA-GLB-213), the by-ear
check on headphones and the mono-sum check of a USB recording of MAIN (QA-GLB-210).

Card file and memory (the CI build's `o3m_pack` line): `wide.tmod` 2,196 B, hot 688 B (the ceiling the test pins is 1.5 KB), cold 1,360 B, state 8,252 B in SDRAM; four instances take 33 KB of the 768 KiB
module region.

## Build and test

    cmake --build build-os-host --target o3m_wide_test o3m_wide_tmod
    ctest --test-dir build-os-host -R wide --output-on-failure
