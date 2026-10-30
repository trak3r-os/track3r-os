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
| D1 | Bit-identical only, or may the filter's output move | Slice 1 is bit-identical; any change of the output (slice 3) needs the maintainer's yes |
| D2 | Order | Measure each candidate with the `cpuopt` svf rows on the unit before it goes into the voice |
| D3 | `.ramfunc_l` | No growth beyond the headroom (about 320 B after OS-PERF-CPU3); a candidate that needs more stops for a decision |

## Plan

- **Slice 1 (bit-identical): the register copies.** Restructure `svf_run` so the compiler drops the three copy `vmov` (a rotation of the state variables per oversampled step; check `-O2` for that function if `MinSizeRel` keeps them). About 3-5 cycles a sample, 200-300 a voice-block [est]; SRAM_L -8 to -24 B [est]. Proof: the disassembly before and after, `audio_voice_digest_test` "filter" digests unchanged, `audio_svf_test`, `audio_svf_cache_test`, `cpuopt_eq_svf` OK on the unit.
- **Slice 2 (measure first): two voices in one loop.** A bench-only `svf_run2` (two independent chains interleaved to hide the multiply-accumulate latency) as a `cpuopt` row. Only if the unit shows at least 15 % does it go into `render` (pairing filtered voices; SRAM_L +120 to +200 B [est], within D3). Bit-identical.
- **Slice 3 (only with the maintainer's yes): fixed point.** A Q31 SVF (SMMLA) saves about 1k a voice-block [est] but moves the output by 1-2 LSB: new digests (`audio_voice_digest_test` filter, cutoff ADSR / LFO groups; `audio_lfo_equiv_test`; `og_sample_fidelity_test`) and `audio_svf_test`'s oracle bounds. Recorded as an option.
- Rejected: block-rate coefficients (already so); skipping at the row's neutral end (HP and BP never equal a bypass); skipping silent tails (needs exact-zero state, no help for sounding voices).

## Tests

The digests and SVF tests above, unchanged for slices 1 and 2; `cpuopt_eq_svf` on the unit. QA rows `QA-PRF-200..205`.

## Done when

Slice 1 merged with the disassembly and unit `cpuopt` figures before and after; slice 2 measured and decided; cpu-budget.md updated (and the stale 128-sample lines fixed); slice 3 recorded for the maintainer.
