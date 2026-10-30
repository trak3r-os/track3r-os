# OS-PERF-HPBP: a cheaper per-voice filter (the HIPASS / BANDPASS Perform rows)

Level C (the audio path). Approved by the maintainer on 2026-10-30. Found by OS-PERF-CPU3 step 0 (`docs/architecture/cpu-budget.md`, the OS-PERF-CPU3 step 0 table).

## Facts (design pass, 2026-10-30, dev 603791a3)

- On the unit the HIPASS 90 and BANDPASS 70 rows cost +18.8k and +18.4k cycles a block, all in the voices: the per-voice filter on six sounding voices, about 3.1k cycles a voice-block. The block is 64 frames (hal/audio.h:25; about 261k cycles at 180 MHz), so that is about 48 cycles a sample. The "128-sample block" in cpu-budget.md:254 and docs/specs/OS-PERFFX.md:406 is stale.
- The row's path: seq/src/perform.cpp:22, :87 -> a Filter-group ModSet on `kLayerPerf` (voices.h:98-105) -> `filter_set_impl` (voices.cpp:1133) -> `filter_apply` (voices.cpp:897-916, flash, once per event or note: cutoff offset, type, `svf_set`). Per block: `post_stages` (voices.cpp:834-842) -> `filter_run` (voices.cpp:1061, HOT, `.ramfunc_l`) -> `svf_run<Type>` (svf.h:124).
- The kernel is a single-precision Chamberlin SVF at 2x oversampling, int16 in and out. Coefficients change per note or event only (`svf_set_cutoff`, svf.h:81, keeps `f` while the table index is unchanged): no per-sample tan or exp, no wait states (SRAM_L), no denormal branches. The loop is 24 instructions; 7 dependent multiply-accumulates (3 cycles each) set the floor, and 3 of 5 `vmov` are pure register copies (disassembly of a 2026-10-17 build; re-read it from the current build).
- The instrument's own filter is the same code (`filter_run`), so every filtered voice costs the same; the rows are real work, not waste.
- Bench: `diag bench cpuopt` has `svf_low`, `svf_high`, `svf_band` (platform/mk66/src/bench.cpp:941+), `ref_svf` and the bit check `cpuopt_eq_svf`; `svf_low` reads 2,598 cycles per 64 samples (docs/specs/OS-CPUOPT1.md:162). `voice_f` against `voice` is not a clean A/B (different conditions).

## Decisions (orchestrator, 2026-10-30; defaults, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| D1 | Bit-identical only, or may the filter's output move | Bit-identical only: slice 3 is not pursued (the maintainer, 2026-10-30) |
| D2 | Order | Measure each candidate with the `cpuopt` svf rows on the unit before it goes into the voice |
| D3 | `.ramfunc_l` | No growth beyond the headroom (about 320 B after OS-PERF-CPU3); a candidate that needs more stops for a decision |

## Plan

- **Slice 1 (bit-identical): the register copies.** Restructure `svf_run` so the compiler drops the three copy `vmov` (a rotation of the state variables per oversampled step; check `-O2` for that function if `MinSizeRel` keeps them). About 3-5 cycles a sample, 200-300 a voice-block [est]; SRAM_L -8 to -24 B [est]. Proof: the disassembly before and after, `audio_voice_digest_test` "filter" digests unchanged, `audio_svf_test`, `audio_svf_cache_test`, `cpuopt_eq_svf` OK on the unit.
- **Slice 2 (measure first): two voices in one loop.** A bench-only `svf_run2` (two independent chains interleaved to hide the multiply-accumulate latency) as a `cpuopt` row. Only if the unit shows at least 15 % does it go into `render` (pairing filtered voices; SRAM_L +120 to +200 B [est], within D3). Bit-identical.
- **Slice 3: fixed point, not pursued (the maintainer, 2026-10-30).** A Q31 SVF (SMMLA) would save about 1k a voice-block [est] but moves the output by 1-2 LSB: new digests (`audio_voice_digest_test` filter, cutoff ADSR / LFO groups; `audio_lfo_equiv_test`; `og_sample_fidelity_test`) and `audio_svf_test`'s oracle bounds, plus the low-cutoff hazards of a fixed-point Chamberlin SVF. It comes back only on an underrun with filters, or a filtered scene above 80 % mean.
- Rejected: block-rate coefficients (already so); skipping at the row's neutral end (HP and BP never equal a bypass); skipping silent tails (needs exact-zero state, no help for sounding voices).

## Design rules from the risk review (2026-10-30, folded)

1. Keep the arithmetic text and order of svf.h:130-144 exactly (`0.5f*(x+prev) - lp - d*bp` left-associated, separate float ops, `0.5f*y`, `svf_out`); never `fmaf`. The audio library and bench.cpp build with `-ffp-contract=off` (audio/CMakeLists.txt:58, platform/mk66/CMakeLists.txt:121): keep both.
2. Slice 1 renames temporaries only (e.g. `lp1 = lp0 + f*bp` instead of an in-place `+=`); the three types' outputs, the `prev = x` placement and the state write-back stay; one template (`hp0` is live only for High, `lp0` only for Low).
3. Disassemble the `.ramfunc_l` `filter_run` (voices.cpp:1061, `-Os`), not the bench's inlined `svf_run`: different codegen (`cpuopt_eq_svf` and `svf_low` measure the bench copy). Record the `.ramfunc_l` bytes before and after.
4. **Slice 1b, measured first:** separate `vmul` and `vadd` (an empty asm barrier on the product) instead of the 3-cycle `vmla`: bit-identical (the M4's `vmla` is not fused), 1 cycle each; it may beat slice 2.
5. No per-function `optimize("O2")` (no precedent; it may refuse to inline the `always_inline` `svf_run`); a per-file flag on voices.cpp would grow all of `.ramfunc_l`. If `-O2` is wanted, `filter_run` moves to its own translation unit and the size delta is checked.
6. Slice 2 pairs only voices with the same span, the same type and the same `m` null-ness (chunking follows `m`); the rest takes the single path (an odd leftover, the odd_span, gated and `dk` paths). One pair loop is about 150 B: pair one type only (three would be about 450 B, over the 320 B headroom) or stop for a decision. The pairing state is stack-local.
7. Slice 2 needs a new multi-voice differential test (paired against unpaired: mixed types, different `f` and `d`, `fmod`, an odd count, split spans): the existing digests play one voice.
8. `cpuopt_eq_svf` runs on the unit only, and the host digests use the magic-number `svf_out`, not `vcvtr`: run the unit check and the qemu digests.

## Tests

Slice 1 and 1b: the digests and SVF tests above unchanged, the qemu digests, `cpuopt_eq_svf` on the unit. Slice 2: also the multi-voice differential test (rule 7). QA rows `QA-PRF-200..205`.

## Done when

Slice 1 merged with the disassembly and unit `cpuopt` figures before and after; slice 2 measured and decided; cpu-budget.md updated (and the stale 128-sample lines fixed); slice 3 recorded for the maintainer.
