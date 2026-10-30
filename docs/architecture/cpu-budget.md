# CPU budget and optimisation plan (Community OS, MK66)

Status: plan, 2026-10-14. Source: a study of the whole OS tree (code and the `tb303.elf` disassembly), asked by the maintainer after noting that the CPU is what is really tight. Items marked [est] are estimates from reading code, not unit measurements. Nothing here is verified on hardware.

Units: points = percent of one render block (64 frames at 689 Hz, about 261k cycles at 180 MHz). The figure is 261,120 cycles at the pre-1.0 clock (4,080 cycles per sample); since the audio clock became exactly 44,100 Hz (D-OS-8, OS-RATE) a sample is 1,800,000/441 = 4,081.63 cycles and the block 261,224.5 cycles, 0.04 % more per block (the same work is 0.04 % less of the block). Measured percentages taken before that change keep their meaning to that precision; the cycle counts do not change.

## 1. Where we stand

- A 0.4.8 song plus a TB-303 line: 69.8 % render load on the unit. Per voice, sustained note, 0.4.9: tb303 21.6 % (56.6k cycles), fm 11.7 %, chip 2.8 %.
- Adding FM, OS-MODMX, OS-IFX1 and the looper to that song lands near 92 % [est], past the OS-GOV threshold (90 %), unless E1 and M1–M3 below land first.

## 2. Findings

**TB-303 and modules**
- The ladder coefficients are already updated every kCtl = 4 samples (`modules/tb303/tb303_dsp.h:37`, `:472-478`). The OS-303PERF hypothesis "coefficients every N samples" is done; it is not the lever.
- The per-sample cost (43.1k / 64 = about 670 cycles on the bench) is memory traffic, not arithmetic [est]: `.o3m_hot` is 616 instructions of which 196 (32 %) are VLDR/VSTR, because about 70 live floats (10 filter structs read through `v.` in `voice_render`, `:480-495`) do not fit 32 S registers; only `lad` and `env` are copied to locals (`:458-459`). `filter_coefficients` divides by `v.fs2` although `inv_fs2` exists (`:259`).
- Live module state already sits in the 4 KiB SRAM_U arena (`platform/mk66/src/app_main.cpp:594-600`), SDRAM only on overflow; the render's helpers are all in `.o3m_hot`, none in `.o3m_cold`. The OS-303PERF hypothesis "state or code through the SDRAM alias" is not supported.
- Arena room after OS-SNDMOD S3: the TB-303 state block is 432 B (was 400 B). Eight instances (`kMaxModuleInstances`) take 3,456 B of the 4,096 B arena (3,200 B before), so nothing spills; the room left for other module states falls from 896 B to 640 B.
- The live figure (56.6–59k) and the bench figure (43.1k) measure different things: the bench reports the minimum of the render alone (`platform/mk66/src/bench.cpp:5`); live adds the float→int16 loop with a NaN test per sample (`audio/src/module_voices.cpp:283-295`), a halfword zero-fill (`:274-279`), enter/leave through `boot::load_context`, preemption by AudioDma, MIDI UART, USB and the MIDI out timer (all above kRender 128, `hal/include/o3os/hal/irq.h:30-36`) and a cold 8 KiB code cache at block start. The split needs the measurement in section 5.
- Modules are built `-O2 -ffp-contract=off` (`tools/o3m_pack.py:348-351`): no fused multiply-add in the ladder or the biquads.
- A modulated module parameter costs a full `derive()` per block (`tb303_dsp.h:405`: 4 exp2/log2 and divisions, in cold code). OS-MODMX pays this for every moved module value.

**Engine**
- The send delay, the reverb and the master limiter run from flash at `-O3` (`audio/include/o3os/audio/send_reverb.h:39`, `send_delay.h:30`, `audio/CMakeLists.txt:39`), sharing the 8 KiB code cache with the sampler's SDRAM reads and the sequencer's project reads. Last measurement (before OS-PERF3): buses 24 %, master 9 % (`the project status notes:228`).
- The rest of the audio library is `-Os` with `-ffp-contract=off` (`audio/CMakeLists.txt:36,48`).
- Already done: idle skips for delay and reverb, sends at 0 skipped per strip, silent strips settle (`audio/src/graph.cpp:588-640`); a check-free sampler loop; float SVF with coefficients every 16 samples; no double or libm on the render path (`-Wdouble-promotion`, `-fsingle-precision-constant`).
- Each live strip copies its feed block before sends and pan (`graph.cpp:622`); mixer SIMD uses ssub16/sel/pkhbt/ssat, no SMLAD.
- SRAM_L hot code has 864 B left (27 KiB budget); the module slab is 14 KiB with about 8 KiB used by the three built-in hot parts and scratch [the study's count].

## 3. Ranked optimisations

| # | Item | Gain [est] | Risk | Effort | Proof |
|---|---|---|---|---|---|
| E1 | Delay, reverb and limiter inner loops into SRAM_L, paid with 2 KiB from the module slab or by moving cold kernels out of `.ramfunc_l` | 3–8 | low | M | `bench stages`, `diag bench worst` buses + master before / after |
| M1 | TB-303 register pressure: locals for hp1 / fbhp / dec / declick like `lad`; `quiet` and `ctl` tests on the kCtl grid; use `inv_fs2` | 2.5–4 | low (bit-exact, oracle test) | S–M | `bench tb303` min |
| E2 | Block 64 → 128 frames: halves the fixed per-block cost | 4–6 | +1.45 ms latency; **maintainer: allowed where needed, for intensive modules such as the TB-303** | M | `bench stages` on an empty project |
| M3 | FM: per-block envelope segments (as chip does), skip silent operators | 2–4 | low | M | a `bench fm` line |
| M2 | TB-303 decimator as a polyphase half-band IIR (2 allpass branches instead of 4 biquads a sample) | 1.5 | M (sound) | M | bench, oracle SNR, A/B by ear |
| E3 | `-ffp-contract=fast` and `-O2` for the voice kernels, the SVF and the module packer | 1–3 | M (oracle tolerances, SRAM_L bytes) | S | kernel bench, map delta |
| E5 | SMLAD packed pan / gain for settled strips | 1–2 | M | M | `bench k_gain_pan` |
| M5 | OS-MODMX: cheap `set_param` path for module cutoff / env mod (skip the full `derive`), or an ABI `set_mod` read at kCtl (the next free ABI minor after 1.8, OS-MODPITCH's) | 0.5–1 per modulated instance | low | S | `diag cpu` with 12 routes |
| E4 | Use the voice feed in place, no `copy_block` per live strip | 0.6 | low | S | `bench strips` |
| M4 | Module wrapper: VCVTR + SSAT conversion (the `svf.h:88-99` recipe), word zero-fill | 0.5 per sounding module | low | XS | DWT around the instance loop |
| M6 | kCtl 4 → 8 with interpolated coefficients; drop `kTiny` on the target | 0.5 | M (accent attack) | S | A/B by ear |
| E6 | Sequencer row cache in SRAM at `begin_step`, only if stage_drain max > 5k | ≤ 1 | low | S | stage_drain max |

Proposed grouping: **OS-303PERF** = M1 + M4 (M2 and M6 after an A/B); **OS-PERF4** = E1 + E4, decided by the first measurement below; **OS-FMPERF** = M3; E3 and E5 after them; M5 inside OS-MODMX MX2; E2 waits for the measurement and a maintainer decision.

Instrumentation to add first (small, Level B): `diag mod` prints per-instance min / mean / max cycles and whether the instance is in the slab or SDRAM; the bench prints `BENCH <kernel> min mean max`.

## 4. Budget for a heavy song

A heavy song: 12 tracks (the product limit, ADR 0003): 10 sampler voices, one TB-303, one FM, the modulation matrix, per-instrument FX, master FX, looper.

| Subsystem | Target | Measured (unit) |
|---|---|---|
| Sequencer and drain | 2 % | worst block drain 36,725 cycles (14.1 %) [unit, bench image, fqa.1]; last block 5,709 cycles |
| Sampler voices (filters, envelopes, OS-MOD) | 18 % | not separable on the unit (the render mean of the whole song is in the Total row) |
| Modules: TB-303 12 % (after M1 / M2), FM 8 %, chip 2 % | 22 % | TB-303 15.5 %, FM 10.7 % per voice [unit, fqa.1, `mod`]; fqa.14 (2026-10-25, release, `mod`, one held note, about 4,400 blocks): TB-303 14.8 % mean (worst 16.9-17.7 %), Chip 1.3-2.3 % mean (worst 4.0-4.9 %), FM 10.3 % mean in every algorithm (worst 12.5-13.2 %) [unit, fqa.14, tester]; TB-303 and FM stay over their rows (kept, maintainer 2026-10-20) |
| Strips and buses | 10 % | worst block: strips 1,254 and buses 6,295 cycles (about 3 %) [unit, bench image, fqa.1] |
| Send FX (delay, plate HALF) | 8 % | not separable on the unit |
| Master (limiter, master FX) | 5 % | worst block 9,863 cycles (3.8 %) [unit, bench image, fqa.1] |
| OS-MODMX 3, OS-IFX1 4, OS-LOOPER 0.5 (L1 read: 0.35 % kernel [unit]; L2 overdub and restore: gate in docs/specs/OS-LOOPER.md 3c.7; the restore step is measured, `restore_step` avg 2.81k / max 4.34k cycles [unit, 2026-10-17], so `kRestoreCycles` = 6,000 stands with about a 38 % margin; the overdub kernels' worst block is still to be read), OS-METRO 0.2 (OS-COREFX within the voices' share) | 8 % | |
| ISRs above the render (USB, MIDI), UI stamps | 6 % | not measured in the song run |
| **Total** | **79 %** | render mean 95.2 % (fqa.1, release, 2 runs), 95.5 % (fqa.1 bench image), 96.0 % (fqa.2); worst block 130.0 % (fqa.1) and 144.2 % to 171.0 % (fqa.2); underruns 5 % to 26 % of blocks; the mute stays on for the whole song [unit] |
| Headroom for OS-GOV (acts at 90 %) | 21 % | |

**12-track mix (OS-HEAVY12, maintainer 2026-10-23).** The reference song has 12 tracks, the product limit, not 14: it lost two of its Modern sampler voices (tracks 7 and 8 of the old song); the four Paula voices, the TB-303 and the FM voice are unchanged, and so are the tempo, the pattern, the sends and the master. The sampler row scales from 12 to 10 voices (22 % x 10/12, rounded: 18 %), so the Total target is 79 % and the headroom 21 %; the other rows are unchanged. **Every Measured figure in this section and in the sections below up to fqa.6 was taken on the old 14-track Heavy** (12 sampler voices, 8 Modern and 4 Paula); they are not comparable with a run of the 12-track song, which has to be measured again on the unit (QA-GLB-156).

A feature that measures above its row needs the maintainer's agreement before it merges.

**Release 1.0.0 limit (maintainer, 2026-10-20).** The heavy song does not fit this budget on the unit [unit]: the render mean is 95 % to 96 % of the block against a Total of 83 % (the target of the old 14-track song), the worst block reaches 130 % to 171 %, and about 5 % to 26 % of the blocks underrun. The underrun mute mutes the output while the song plays and does not lift on its own. The heavy fixture is beyond the unit. The maintainer decided that release 1.0.0 ships with this limit documented and not with cut features. The underrun mute now lifts after 32 on-time blocks (it was 256): this helps short overloads, but it cannot help a sustained one, so the song stays silent until the load drops [unit].

**Heavy12 is the stress test, Dense is the 1.0 reference (OS-DENSE, maintainer 2026-10-23).** Heavy12 (10 sampler voices, 6 Modern and 4 Amiga, TB-303 and FM) stays as it is, with no sound change and no feature cut, but it is a STRESS test only: on the unit it runs at 113 % mean and 147 % worst (v1.0.0-fqa.8, 477d5d9, unit run of 2026-10-23), over the budget, and the main-loop guard (OS-MAINGUARD) keeps the unit controllable there. `qa_cpu_heavy.json` asserts that (the console answers within 1 s at every read, the guard line is read, Stop works, the UI responds) and only reports shed blocks, underruns and the mean. The 1.0 CPU target (zero underruns at about 85 % mean) moves to a dense but realistic reference project: **Dense10** (6 Modern + 2 Amiga sampler voices + TB-303 + FM, 10 tracks) or **Dense8** (4 Modern + 2 Amiga + TB-303 + FM, 8 tracks), both with the send delay and the reverb on and the same per-voice FX as Heavy12 (`o3pj_tool gen dense10|dense8`, the same routine and the same voice recipe, so a Dense voice is a Heavy12 voice). `qa_cpu_dense.json` (QA-GLB-158) asserts underrun 0 and guard shed 0 at every read and reports the mean against 85 % of the block (221,952 of 261,120 cycles). **Reference: Dense8, the 1.0 CPU reference (maintainer's orchestrator, 2026-10-24):** zero underruns at about 85 % mean, met at 86.5 % mean (worst block 106 to 107 %) with the guard window N >= 32 (verified on the unit: v1.0.0-fqa.9, 740c79a, unit run of 2026-10-24). The default N is **64** (`kStarveBlocks`; about 93 ms without a main pass before a shed, the main-loop floor about 1.5 % of the CPU). N sweep on the unit, per project (mean / worst %, shed blocks / runs, underruns, console answer s, Stop s):

| Project | N | Mean / worst % | Shed blocks / runs | Underruns | Console s | Stop s |
|---|---|---|---|---|---|---|
| Dense8 | 8 | 86.3 / 106.9 | 3926 / 1774 | 0 | 0.29 | 0.31 |
| Dense8 | 16 | 86.6 / 106.6 | 1578 / 1423 | 0 | | |
| Dense8 | 32 | 86.3 / 106.9 | 0 / 0 | 0 | 0.31 | 0.31 |
| Dense8 | 64 | 86.6 / 106.3 | 0 / 0 | 0 | 0.31 | 0.33 |
| Dense10 | 32 | 94.0 / 117.4 | 1627 / 1028 | 0 | | |
| Dense10 | 64 | 94.0 / 117.8 | 222 / 210 | 0 | 0.37 | 0.36 |
| Heavy12 | 32 | 113.8 / 139.0 | 2930 / 1086 | 8381 | 0.33 | 0.34 |
| Heavy12 | 64 | 113.3 / 139.1 | 1544 / 578 | 9600 | 0.42 | 0.43 |

Dense8 sheds nothing from N 32 up, so N 64 costs it nothing; Dense10 (a stress between the two) sheds 222 blocks in 60 s at N 64; Heavy12 stays over budget and controllable (console 0.42 s, Stop 0.43 s) with the larger window. Dense10 is no longer a reference. The Total row below is the old 14-track target and stays as history.

**The fixture (OS-FINALQA FQA-7).** `o3pj_tool gen heavy <dir>/Heavy/project.o3pj` (the folder `<dir>/Heavy/samples/` made first; `tests/tools/o3pj_tool.cpp`, test `heavy_song_python`) writes this song as a native project, deterministic, with its WAVs (generated sine and saw, nothing from any firmware): 10 sampler voices (6 Modern on a looped sine, 4 Paula on a looped saw; each with filter, cutoff on an envelope or an LFO, delay and reverb sends, a third with drive, a quarter with bit depth 12, a third with a pan LFO; all sounding the whole pattern), one TB-303 line (a note on each of 16 steps), one FM voice, the send effects set (ping-pong delay 375 ms, plate quality HALF) and the master limiter on. The Dense projects are the same song with 8 or 6 sampler voices (`gen dense10`: 6 Modern + 2 Paula, `gen dense8`: 4 Modern + 2 Paula; folders `Dense10` and `Dense8`, generated into `build-hil/dense/<name>/`, never committed; test `heavy_song_python` asserts the split for all three). **Not in the file:** OS-MODMX (parked, no matrix exists: its 3 % row stays empty), the looper (a loop is not file state; its 0.5 % idle row is in the unit's figure) and the chip voice. The "Measured" column is filled by the tester from `qa_cpu_heavy.json` (QA-GLB-156): the whole-song render mean and worst block go in the Total row; the rows below it are not separable on the unit (`diag bench worst` gives the stage split of the worst block, `diag mod` the instrument lines), so write what the tools print and leave a row empty when no tool prints it.

## 5. Measurement protocol (maintainer rule: every feature that adds per-block work)

On the unit, the same build id, the feature off then on:
1. Reference project: t-v16 plus the TB-303 line, 60 s. The heavy song of section 4 is the stress test `qa_cpu_heavy.json`, and the 1.0 reference is `qa_cpu_dense.json` (Dense10 or Dense8; the fixtures above): copy the folder `Heavy`, `Dense10` or `Dense8` into `Projects/` on the card (a card reader or the unit's USB Disk mode; `o3console --put` is for firmware only), then run the scenario.
2. `diag cpu reset`, play, then report `diag cpu` (render max, mean, underruns) and `diag bench worst` (stage split of the worst block). `diag bench` exists only in an image built with `-DO3OS_BENCH=ON` (a release image answers "not in this image").
   **The bench-image recipe.** Two images of the same commit: the release image (`O3OS_PLATFORM=mk66`, the default; `tools/ci.sh mk66` builds it) and the bench image, the same configure with `-DO3OS_BENCH=ON` and the target `o3os_image` (`tools/ci.sh mk66` builds it as `mk66-bench`, build only; `-DO3OS_BENCH=ON` compiles in `diag bench` and its kernels, so its flash and SRAM are the bench's cost alone). Run this protocol on the release image first (the figures that count: `diag cpu`, `diag mod`), then on the bench image for `diag bench worst` and the kernels. The pair is one A/B of the same build id apart from the bench code; the bench image is never the one that is released and goes to the unit only through the documented update path after `fw-gate-check`.
3. The feature's own `bench <kernel> N`: min / mean / max.
4. SRAM_L, SRAM_U and flash deltas from the map.
5. A change that alters sound adds the oracle SNR and an A/B by ear.

The deltas go in the commit body and in the perf notes. Host tests prove correctness only, never cycles.

**The module instruments' own measure (OS-303PERF) and the governor's figure.** `diag cpu reset` (step 2) also turns ON the per-instance split that `diag mod` prints as a `perf` line (loop, render alone, output side, hooks; `platform/mk66/README.md`), and nothing but `diag cpu perf off` turns it off: every
scenario that starts with `cpu reset` leaves it on. While it is on, the governor's window (the first stamp of a segment to its last, `t4 - t0`) holds three more counter reads (after `enter()`, after `render()`, after `leave()`), so the governor's figure (`cycles/voice` in `diag mod`, the
cost it admits notes by, and the render time of `diag cpu`) reads higher than with it off by about three counter reads a segment; the whole measure, with its adds and its fold, is about 0.1 % of a block for each sounding instance [est: QEMU icount, not measured on the unit]. It is
therefore not a free reading, and the "feature off" side of the protocol above, and any baseline that is compared across builds, is taken with it OFF: `diag cpu reset`, then `diag cpu perf off`, then play, then `diag cpu` and the instrument line of `diag mod`. The run with the measure ON gives the perf lines; the difference
of the two runs is the measure's own cost. A held note (`diag mod note 61 36 110`) sounds about 17 s and an idle instance is not rendered, so it is measured for 8 s; a song for 60 s.

## 6. Hardware checks that decide the order

- `bench stages` on 0.4.9 with the cache on: if buses + master are under 10 %, E1 drops; above 15 %, E1 goes before any DSP change.
- The TB-303 gap: DWT around `in.mod->render` alone versus the instance loop, USB off and MIDI idle, min / mean / max. If the gap is mostly preemption, M4 is cosmetic and the ISR row of the budget grows.
- If a module's min and max differ by more than 20 % in a song, cache contention is real and placement (E1, slab size) beats micro-optimisation.
- E3 and M1 need unit cycle counts.
- E2, maintainer 2026-10-14: if a switch to 128 is needed, make it, for intensive modules such as the 303. Open design points before any code:
  - A module rendering 128 frames every other block halves its per-call fixed cost, but doubles its cost in the blocks where it renders. The worst block only improves if instances are staggered (odd and even blocks), or if the module renders one block ahead at a lower priority with a two-block deadline (Level C, concurrency).
  - The TB-303's cost is per sample (about 670 cycles a sample), so 128 frames saves it little [est]; M1 is its lever. The 4–6 points estimated for E2 are the engine's fixed per-block cost under a global block of 128.
  - Measure first: DWT around `in.mod->render` with n = 64 and n = 128, and `bench stages` on an empty project.

## OS-MODPITCH (ABI 1.8, 2026-10-16): the Slide / Glide path of module instruments

Estimates [est], to be replaced by the unit measure (`diag mod`, `diag cpu`; steps in `qa_modules.json`). Host side: one flag test per note, one byte test per instance segment, and for each ramping instance one block-start call (about 50 cycles) that makes one `pitch()` call; module side about 300 cycles for TB-303 and FM (one `exp2` per call, a staircase of one block), near 0 for Chip (the offset joins the semitone sum it already computes at control rate). Eight ramping instances at once are about 2.4k cycles, 0.9 % of a block; a typical song under 0.2 %. Accept under 1 % of the block on the unit. Memory: `ModuleVoices` (SRAM_U) grows by the per-track FX state (72 bytes) and the per-instance ramp (20 bytes, 8 instances); **Measured by `tools/ci.sh mk66` against dev b1676a6** (MOD-ABI, 2026-10-16): FLASH 746,628 -> 750,384 B (+3,756), SRAM_L 49,896 -> 49,608 B (-288), SRAM_U 136,528 -> 136,760 B (+232), SDRAM_STATIC 113,728 B (0). `.ramfunc_l` was 27,624 B of its 27 KiB (27,648 B) hot budget on dev, i.e. **24 B left, not the 864 B the spec assumed**: with the pitch hooks inline in `start_note`, `render` and `Engine::dispatch` it went over by 88 B and the link failed (+112 B). `ModuleVoices::start_note` (per note, not per sample) is therefore in flash now (`O3OS_AUDIO_COLD`; about 420 B out of SRAM_L): `.ramfunc_l` 27,624 -> 27,336 B (module_voices 1,420 -> 1,100 B, engine 3,416 -> 3,448 B), 312 B of the budget free. The note start runs from flash (about a hundred instructions; to be seen in `diag cpu` with a dense module track). Slab (module hot parts): TB-303 3,424 B, Chip 2,464 -> 2,096 B, FM 1,952 B (7,840 -> 7,472 B of 13,056); cold (SDRAM) +192 / +432 / +848 B; Chip `state_bytes` 192 -> 208 B. The ramp, the FX state and the events are in flash.

## OS-CPUOPT1 (2026-10-16, plan; `docs/specs/OS-CPUOPT1.md`): the live LFOs and the loop player

Unit v0.6.0: the loop player costs +0.9..2.0 % of the block, the live LFOs +0.8 % mean (worst block 136k -> 154..158k cycles, the filter alone +12..17k). Static finding [dis, est]: `LoopPlayer::run`'s lambda was not inlined at -Os, so `pos`, `g` and the output pointer go through stack references every frame (about 26 instructions, 45..50 cycles a frame, 3.0k a block) against the 12..16 cycles a frame the L1 estimate assumed; a voice with six LFOs costs about 2.3k cycles a block (`flfo_level` 75..90 cycles an evaluation, 7 evaluations in `mod_block`, 4 in the cutoff path, 16-sample filter chunks that reload the filter state, `trem_span` 7 cycles a sample), and the SVF loop is about 45 cycles a sample because ten `vmla` (3 cycles, serial) sit on the dependent chain. As built (2026-10-16, branch `feat/OS-CPUOPT1`, section 8 of the spec): L1, C1, C3, C4; C2, C5 (unit: 0.916x against a 0.8x gate; the SVF's chain is not the limit; deleted), C1 (reverted), L2, L3, C6 dropped. Unit: `loop_new` 893 against `loop_ref` 3,173 cycles per block (0.28x). Size at the bench image 79260ba, against dev aafdcb8 (before C5 was switched off, which frees about 64 B of SRAM_L): FLASH +5,700 B (about 3.9 KB of it is the `diag bench cpuopt` references), SRAM_L +192 B (limit +200), SRAM_U row use about +192 B (Svf::idx +4, Voice +8, x16; the map is unchanged), SDRAM_STATIC 0. Unit baselines: LFOLIVE scene +19.9k cycles (+7.6 %), looper +1.1..1.5 % while running. Plan (all bit-identical): L1 loop rewrite with word reads and one `qadd16` (-2.2k), C1..C4 and C6 for the LFO paths (-0.6..0.8k per six-LFO voice), C5 `vmul` + `vadd` in the SVF (-1.2k..1.6k per filtered voice). Targets LOOPER < 0.5 %, LFOLIVE < 1 % in the worst block. SRAM_L budget: 312 B free, the item is held to +200 B. Confirmed on the unit by `diag bench` kernels first, then the paired windows.

## OS-ODD11 slice 1 (2026-10-17; `docs/specs/OS-ODD11.md`): muted tracks keep rendering

Unit v0.6.4 (tester): idle floor 27.5k cycles; all tracks muted with 6 sampler voices 67-74k (about 7k a voice); unmuted 94-120k. Slice 1 (the voice gate, option C): a voice on an inaudible strip (level and both sends closed in the strip smoothers, so mute, solo elsewhere, fader 0) keeps its sample loop and `mod_block` and skips the tremolo, the filter (its cutoff clock goes on) and the shaper; its block is not fed to the track. Expected [est]: the filter, tremolo and shaper share of a voice, 2.9k-3.9k of the 6-8k of a filtered voice, nothing for a plain voice. Cost on an unmuted project [est]: the mask loop at the end of `run_tracks` (one compare a strip, about 60 cycles) and about 8 cycles a voice in `render`.

Build deltas (`tools/ci.sh mk66`, release image, against dev 8fd9ce3): FLASH +364 B, SRAM_L +288 B (hot `.ramfunc_l`), SRAM_U 0, SDRAM_STATIC 0. The bench image has 4 B of SRAM_L headroom left under the 27 KiB hot budget (the release image 44 B): the next hot-code change must pay for itself.

Measure on the unit (arms and thresholds: spec section 5; scenario `platform/mk66/tools/hil/cpu_mute_ab.json`, run it four times per image, alternating the images): M mean at least 35 % below the baseline's M mean; U within the paired noise (+-1.2 pp) of the baseline; underruns 0, over-budget 0 in every window.

| Arm | Baseline mean | Slice 1 mean | Delta |
|---|---|---|---|
| floor (empty project) | open | open | |
| U (nothing muted) | open | open | |
| M (every track muted) | open | open | |
| H (half muted) | open | open | |
| S (one soloed) | open | open | |

## OS-ODD11 slice 2 (2026-10-18; `docs/specs/OS-ODD11.md` section 13): the closed-form advance

A gated voice of the plain kind (a one shot or a forward loop, no residual tail, not a wavetable) no longer runs its sample loop: `VoicePool::gated_span` (flash) advances its position, amplitude, envelope and end in closed form, leaves its block untouched and runs `mod_block` as before. Expected [est]: the interpolation loop (1.0-1.5k cycles a block) and the block zero-fill (about 70) are gone; about 150-250 cycles of flash code remain a gated voice and span. With 6 plain voices about 6-8k below slice 1; the other kinds (backward, ping-pong, granular, wavetable, a voice with a residual tail) cost what they cost in slice 1. Cost on an unmuted project [est]: none beyond slice 1 (`render()` +36 B of hot code: one call and one mask test, taken only for a gated voice).

Build deltas (`tools/ci.sh mk66`, release image, against dev 744cdc9, after the review fold which adds the `diag voices` `advanced` count): FLASH +708 B, SRAM_L +40 B (hot `.ramfunc_l` +36 B, 27,588 B of the 27 KiB budget: 60 B left; the bench image 27,624 B, 24 B left), SRAM_U 0, SDRAM_STATIC 0. Bench image: FLASH +700 B, SRAM_L +32 B.

Measure on the unit (spec section 5, scenario `platform/mk66/tools/hil/cpu_mute_ab.json`, same procedure as slice 1, alternating the slice 1 image and the slice 2 image; read `diag voices` at the start and the end of every window: the `advanced` count must rise in the M and H arms, about 690 a second per muted plain voice, and not in the floor and U arms, which shows the closed form was taken): M mean within 5k of the empty-project floor; U within the paired noise (+-1.2 pp) of the baseline; underruns 0, over-budget 0 in every window.

Unit result (tester, 2026-10-18; bench images of dev 744cdc9 as A = slice 1 and f5d1070 as B = slice 2 with its fold; eight visits A B A B A B A B, four runs of `cpu_mute_ab.json` per image; cycles, mean of 12 reads of `render last`, `diag cpu perf off`; the report is the tester's and is not in the repository):

| Arm | Slice 1 mean (A) | Slice 2 mean (B) | Delta |
|---|---|---|---|
| floor (empty project) | 29,761 | 29,673 | -88 (-0.03 pp) |
| U (nothing muted) | 133,171 | 132,121 | -1,050 (-0.40 pp) |
| M (every track muted) | 53,145 | 35,645 | -17,500 (-6.70 pp) |
| H (half muted) | 107,207 | 96,136 | -11,071 (-4.24 pp) |
| S (one soloed) | 56,057 | 38,932 | -17,125 (-6.56 pp) |

U passes (within +-1.2 pp). Underruns 0 in 8 visits. Blocks over budget in H: about 24 a window on A (27, 24, 23, 24), 0 on B; arm U holds about 690 on both images (the baseline's own, B not worse). M(B) minus the floor is 5,972 against the 5,000 target: a miss of 972 (per visit 8,522, 5,699, 5,232, 4,438). `diag voices` `advanced` rose in M (+4,208 a second) and H (+2,788), flat in U and the floor. **The maintainer accepted the merge on 2026-10-18 despite the M miss** (`docs/specs/OS-ODD11.md` section 14). Still unit pending: listening (QA-MIX-020 to 028), the export byte compare (QA-MIX-024), a dense-event scene.

## OS-PERFFX P0 (2026-10-18; `docs/specs/OS-PERFFX.md` section 7): the Perform layer reaches the voices whole

P0 changes no sound and adds no per-block work: `Engine::dispatch` passes `fx_event(track, const SeqEvent&)` (a reference) in place of four scalars, and the layer's value is kept by one flash function (`VoicePool::layer_event`) once per ModSet or ModEnd of a group the voices do not handle in SRAM_L. The export puts the layer away and gives it back once per export (`offline_enter` / `offline_leave`, flash). Expected [est]: 0 cycles a block with Perform off or on; about 200 cycles per Perform event (the same event cost as before plus a store). Memory: `Strip::perf_gain` and `perf_pan` sit in the padding before `volume` (`sizeof(Strip)` 216 B, unchanged); `VoicePool` +36 B (the layer's slots) and `Engine` +100 B (the copy an export keeps) inside the SRAM_U rows they were in.

Build deltas (host `ci.sh mk66` run, release image, against dev 6409283): see the commit body of the P0 commit; the dispatch function in `.ramfunc_l` is the same size as before (1,120 B) and `.ramfunc_l` is 27,588 B in both (net 0 B).

Measure on the unit (protocol of section 5; the tester, bench image not needed): images A = dev 6409283, B = the P0 commit; visits A B A B A B A B, four windows per image and arm, `diag cpu reset` then `diag cpu perf off`, 5 s settle, 60 s window, 12 reads of `diag cpu` 5 s apart (the mean of `render last`, as `cpu_mute_ab.json` does), `diag voices` at the start and the end of the window.

| Arm | Scene | Baseline mean (A) | P0 mean (B) | Delta |
|---|---|---|---|---|
| F | empty project, stopped (the idle floor) | open | open | |
| U | `t-v16`, FILTER On on the 16 instruments, pattern 9 looping, Perform not entered (the U arm of `cpu_mute_ab.json`: its steps up to the first `cpu` window load and prepare it) | open | open | |
| P12 | the U scene, then on `Performance`: `F8` (RESET), `Shift` held and pads 0 to 11 tapped (pads counted from 0, as the HIL steps do: the AFFECTED row of the TRACKS layer, all twelve lit, then `Shift` released), pad 15 (row 1 of the fourth slot, which holds LOWPASS by default) held while the encoder turns +50, released (LATCH on keeps it); the screen reads `LOWPASS 50`, then `Pattern` and `Play` | open | open | |

The pad numbers are from `app/pad_lights.h` (pad = 12 x row + column); the tester confirms on the unit that the twelve AFFECTED LEDs and the slot are lit before the window starts. P12 minus U is the cost of the existing performance filter on 12 tracks and is the same on both images; it is also the reference the OS-PERFFX P1 A/B (Overdrive and Bit depth on 12 tracks) is read against.

Accept for P0: F, U and P12 each within the paired noise (+-1.2 pp of a block) of the baseline's; underruns 0 and over-budget not above the baseline's in every window. Then the export check (QA-PRF-010): with P12 held, export the pattern to a WAV, `RESET` on `Performance`, export again, and `cmp` the two files on the PC: they are equal.

## OS-PERFFX P1 (2026-10-19; `docs/specs/OS-PERFFX.md` section 7): BITDEPTH and OVERDRV from Perform

P1 adds no per-block work with the FX off: `VoicePool::shape_set` (flash, once a note and once a Perform event, never per sample) reads two extra bits of `perf_` and the voice's `drive` and `crush` are the fields OS-IFX1 already had, so the shaper's two passes (`shape_span`, unchanged) are the only cost and they run only on a voice whose track holds the FX. Expected [est], from OS-IFX1's kernels (about 17 cycles a sample for OVERDRV, 8 for BITDEPTH, 20 for both; 44,118 samples a second on a 180 MHz core): 0.42 % of the core per sounding voice for OVERDRV, 0.20 % for BITDEPTH, 0.49 % for both, so **about 5.0 %, 2.4 % and 5.9 % of a block with the FX on all 12 tracks** (12 voices sounding). The spec says 0.2 to 0.5 % per voice. Memory: `VoicePool` +0 B (the slots are P0's), `PerformBank::sent_` +48 B (12 x 22 x 2 B = 528 B), `.ramfunc_l` net 0 B (nothing in SRAM_L changes; the commit body has the figures).

Measure on the unit (protocol of section 5 and of P0 above, the tester): images A = the P0 commit, B = the P1 commit; the same visits (A B A B A B A B), windows and reads. The arms F and U are P0's and must be within the paired noise (P1 changes nothing with the FX off). New arms, all on the U scene with the twelve AFFECTED tracks (as P12 above), and the slot held and latched (LATCH on) by the encoder; the screen reads the value, then `Pattern` and `Play`:

| Arm | Held slot | Value | Expected cost over U on image B [est] |
|---|---|---|---|
| OD12 | `OVERDRV` (Up/Down in EDIT to the 21st FX) | 100 | +5.0 pp of a block (17 cycles x 12 voices) |
| BIT12 | `BITDEPTH` | 8 | +2.4 pp |
| BOTH12 | both slots | 100 and 8 | +5.9 pp |

Image A has no such slots, so the A/B for these arms is B against the U figure of the same image: the cost of the FX is OD12 (BIT12, BOTH12) minus U, read against the estimate and against the kernels `diag bench voice_od`, `voice_bit` and `voice_odbit` (transport stopped; OS-IFX1's figures, `docs/specs/OS-IFX1.md`) times the voices that sound. Accept: F and U within +-1.2 pp of A's; each new arm's cost not above its kernel figure times the voices by more than the noise; underruns 0 and over-budget windows not above the baseline's. Then by ear (QA-PRF-012 to 014) and the export check with OVERDRV held (QA-PRF-017: two exports, `cmp`).
## OS-PERFFX P2 (2026-10-19; `docs/specs/OS-PERFFX.md` section 7): VOLUME and PANNING from Perform

P2 adds no per-sample and no per-block work: the layer's value is kept by one flash function (`Graph::set_perf_gain` / `set_perf_pan`, called from `TrackerFx::event`, which `Engine::tracker_event` already calls for every ModSet and ModEnd) once per Perform event, and `Graph::recompute_strip` (flash; it already runs once for every strip a gain or pan change dirties) adds one byte compare, and below 0 two float multiplies, to a recompute that happens only when the value, the tracker's gain or a pan changed. A held value costs nothing per block. Expected [est]: about 20 cycles more per recompute, a few hundred cycles per Perform event, 0 per block: noise. With VOLUME at -100 the strip's level and sends close in its smoothers, so the voices on the track are gated (OS-ODD11): the arm is cheaper than the same scene without the slot, not dearer. Memory: `Strip::perf_gain` and `perf_pan` are P0's (padding, 0 B), `.ramfunc_l` net 0 B (nothing in SRAM_L changes), flash only. Build deltas (`tools/ci.sh mk66`, release image, against dev f05dd57 built the same way): FLASH +464 B (812,924 to 813,388 B), SRAM_L +0 B (49,880 B), `.ramfunc_l` 27,608 B in both, SRAM_U +0 B (137,992 B), SDRAM_STATIC +0 B (144,320 B).

Measure on the unit (protocol of section 5 and of P0 above, the tester): images A = dev f05dd57 (P0 merged), B = the P2 commit; the same visits (A B A B A B A B), windows and reads. The arms F, U and P12 are P0's and must be within the paired noise (+-1.2 pp of a block) of the baseline's. New arms on the U scene with the twelve AFFECTED tracks (as P12), the slot held and latched (LATCH on) with the encoder; the screen reads the value, then `Pattern` and `Play`:

| Arm | Held slot | Value | Expected over U on image B [est] |
|---|---|---|---|
| V12 | `VOLUME` (`Up` / `Down` in EDIT to the 2nd FX) | -50 | noise (+-1.2 pp) |
| V12Z | `VOLUME` | -100 | below U: 12 gated voices skip their post stages (OS-ODD11's figures) |
| PAN12 | `PANNING` | +50 | noise (+-1.2 pp) |

Accept: each new arm not above U by more than the noise, V12Z not above U; underruns 0 and over-budget windows not above the baseline's. Then by ear (QA-PRF-020 to 023) and the export check with both slots held (QA-PRF-024: two exports, `cmp`).

## OS-PERFFX P3 (2026-10-19; `docs/specs/OS-PERFFX.md` section 7): DELAY and REVERB from Perform, and the step FX 30 and 32

P3 adds no per-sample and no per-block work of its own: the layer's offset and the step FX's send are kept by two flash functions (`Graph::set_perf_send` / `set_step_send`, called from `TrackerFx::event` once per Perform or step FX event of the Delay send and Reverb send groups), and `Graph::recompute_strip` (flash; it already runs once for every strip a send, a gain or a pan change dirties) adds, for each of the two sends, a byte compare and a float add and clamp. Expected [est]: about 30 cycles more per recompute, a few hundred cycles per Perform event, 0 per block with the slots off, and nothing in SRAM_L. What a held slot changes is audible work that already existed: with a send open, the strip adds into the send bus and the delay or the reverb runs (both idle-skip while nothing feeds them, and a send at 0 is skipped per strip). That cost is OS-SENDFX's, the same as an instrument whose DELAY SEND or REVERB SEND is raised, and it is what the new arms read; with a slot at -100 on tracks whose instruments send, the send closes and the arm is cheaper than the scene without the slot. Memory: the four new strip fields and the instrument's two sends are bytes in the space the two float sends had (`sizeof(Strip)` 216 B, unchanged); `Engine` +32 B of statics (the export's copy of the layer's sends: `Graph::PerfSave` is 4 x 16 B now, 2 x 16 B before), which the linker's SRAM_U figure does not show because it falls in alignment slack: `g_engine` (15,224 B at 0x20007e70 in the P3 map) is followed by 252 B of fill before the USB buffer descriptor table (`g_bdt`, `alignas(512)`, at 0x2000f000), and it is that fill that is 32 B smaller than on dev (284 B there). The slack has 220 B left; once the statics before `g_bdt` grow past it, SRAM_U moves by a 512 B step. Build deltas (`tools/ci.sh mk66`, release image, against dev ba919e8 built the same way): FLASH +520 B (814,092 to 814,612 B), SRAM_L +0 B (49,880 B), `.ramfunc_l` 27,608 B in both, SRAM_U +0 B (137,992 B), SDRAM_STATIC +0 B (144,320 B).

Measure on the unit (protocol of section 5 and of P0 above, the tester): images A = dev ba919e8 (P0 and P2 merged), B = the P3 commit; the same visits (A B A B A B A B), windows and reads. The arms F, U and P12 are P0's and must be within the paired noise (+-1.2 pp of a block) of the baseline's. The U scene's instruments must send nothing (their DELAY SEND and REVERB SEND are 0 on the SENDS page; the tester checks before the first window), with the master DELAY and REVERB levels at their defaults. New arms on the U scene with the twelve AFFECTED tracks (as P12), the slot's row 1 held for the whole window with the pad (the live value ends at the release, QA-PRF-025; the slots of the default scene: DELAY is slot 7 and REVERB slot 8, so pads 18 and 19 of row 1, counted from 0), the encoder at +100, then `Pattern` and `Play`:

| Arm | Held slot (pad held) | Value | Expected over U on image B [est] |
|---|---|---|---|
| DLY12 | `DELAY` (pad 18) | +100 | the delay's cost with twelve full sends: within the Send FX row of section 4 (8 % for the delay and the plate together), the delay about a third of it |
| REV12 | `REVERB` (pad 19) | +100 | the reverb's cost with twelve full sends: within the Send FX row, the plate about two thirds of it |

Accept: each new arm not above U by more than the Send FX row (8 % of a block), P3's own code showing as noise: F, U and P12 within +-1.2 pp of A's; underruns 0 and over-budget windows not above the baseline's in every window. The same two arms on image A are not possible (the slots do nothing there), so DLY12 and REV12 are read against U on the same image and against the instrument's own send on image A: U with every instrument's DELAY SEND (then REVERB SEND) at 100 on image A is the reference the slot should match within the noise. Then by ear (QA-PRF-026 to 029) and the export check with both slots held (QA-PRF-030: two exports, `cmp`).

## OS-PERFFX P5 (2026-10-19; `docs/specs/OS-PERFFX.md` section 7): TUNE from Perform

P5 adds no per-sample work and no per-block work for a held value that does not change. The layer's TUNE is the zero of the pitch offset the Slide FX already moves (`Voice::slide_q16` / `slide_tgt`): a note starts at the base, the first span makes its pitch (`mod_block`, flash, the `kMmSlide` path of the Slide FX) and lets the flag go, and the voice then renders as any voice does. Expected [est]: one more `repitch` (the `slid_inc_q32` path and the rate factor, about 150 to 250 cycles, flash) at each note start of a tuned track, one per sounding voice of the track for each encoder detent, and nothing a block. A track that holds no TUNE costs one compare and a load in `mods_start` (flash, once a note). No SRAM_L code: `.ramfunc_l` is unchanged (build deltas in the commit body).

Measure on the unit (protocol of section 5 and of P0 above, the tester): images A = dev 04ca029 (P0 to P3 and the P1 merge), B = the P5 commit (it holds P1 as well); the same visits (A B A B A B A B), windows and reads. The arms F, U and P12 are P0's, within the paired noise (+-1.2 pp of a block) of A's. The new arm, on the U scene with the twelve AFFECTED tracks (as P12), the third slot (TUNE by default: pad 14 of row 1, counted from 0) held while the encoder turns +12, released (LATCH on keeps it); the screen reads `TUNE 12`, then `Pattern` and `Play`:

| Arm | Held slot (pad held) | Value | Expected over U on image B [est] |
|---|---|---|---|
| TUNE12 | `TUNE` (pad 14) | +12 | noise (+-1.2 pp): the same twelve voices an octave up: the cost of a sounding voice does not depend on its pitch, but a one-shot sample ends sooner at +12, so read it with `diag voices` at the start and the end of the window |

For the Perform effects of P1 the arms are OD12 (OVERDRV at 100), BIT12 (BITDEPTH at 8) and BOTH12 (both): they are in the P1 section above and run on the same image B, so one visit list covers OD12, BIT12, BOTH12 and TUNE12.

Accept: TUNE12 not above U by more than the noise; underruns 0 and over-budget windows not above the baseline's. Then by ear (QA-PRF-031) and the Play / Stop and export check with TUNE held (QA-PRF-032: two exports, `cmp`).

## OS-PERF-SMP (2026-10-24; `docs/specs/OS-PERFFX.md` section 10): SMP POS and SMP END from Perform

P7 changes no render loop: the layer's two percentages are read once at a note's start (`note_on` tests the two masks, the flash helpers `smp_window` and `smp_frame` do the arithmetic: a few dozen cycles with the slots held, two byte tests without) and become the voice's first position and last frame, which the sample loops, the closed-form advance and `mod_block` already take. `.ramfunc_l` +48 B measured (27,284 to 27,332 B, headroom 364 to 316 B): `note_on` itself runs from SRAM_L. A note with a late start or an early end does less work than the plain one. Expected [est]: noise (+-1.2 pp of a block) against the same scene with the slots off.

Build deltas against fqa.14 (orchestrator's report, 2026-10-25): FLASH +240 B (925,460 to 925,700), SRAM_U +32 B, `.ramfunc_l` +48 B. The unit has not measured the branch yet (`feat/OS-PERF-SMP`, not on dev).

Measure on the unit (the tester; the Dense8 song of section 5 is the reference scene, `heavy_song_python` `gen dense8`): `diag cpu reset`, `diag cpu perf off`, a 60 s window with SMP POS and SMP END off, one with SMP POS +30 and SMP END -20 held on the twelve AFFECTED tracks (arm SMP12: the ninth and tenth slots of row 1 held while the encoder turns, LATCH on), alternated twice (off, SMP12, off, SMP12). Accept: SMP12 within the noise of off; underruns 0 and over-budget windows not above off's. Then by ear (QA-PRF-151: where each note starts and stops, and no click at a short stop beyond a slice's own) and the Play / Stop and export check with both held (QA-PRF-152: two exports, `cmp`).

**Measured on fqa.15 (2026-10-25, the final candidate):** arm SMP12 +0.39 pp (one-shot scene) and +0.48 pp (stock Dense8), both within the noise, underruns 0 and mute 0: PASS. Details in the fqa.15 section below.

## OS-PERF-REVERSE (2026-10-25; `docs/specs/OS-PERFFX.md` section 10.5): PLAYBACK from Perform

P8 adds no render loop and no template instance. A one-shot or beat-slice note of a track that holds PLAYBACK (or the step FX 26) is made a `Voice::mode` 4 voice at its start (`mods_start`, flash): the window `[gstart, end)` is the loop, the position starts at its last frame and runs backward, and `run<true, true>` (`run_dir`) stops below the first frame (one `rev` flag and one test at the turn; one entry test for a stop at the last sample of a call). A forward voice pays nothing; a reversed voice is `mode != 0`, so it renders in the checked Dir loop (no fast path, not advanced in closed form when gated): 6 to 9 more cycles a sample than the forward fast path, so about 0.4 to 0.6 k cycles per voice-block [est], 0.15 to 0.22 % of the 261,120-cycle block each and 1.8 to 2.6 pp for twelve reversed tracks. `.ramfunc_l` +52 B measured (27,332 to 27,384 B, headroom 316 to 264 B), FLASH +260 B, SRAM_U 0 (spec section 10.5, "as built").

Expected [est, not measured]: arm REV12 (Dense8 of section 5 with PLAYBACK held on the twelve AFFECTED tracks, every sample one shot) 1.8 to 2.6 pp of a block over the same scene with it off, so under the 3 pp gate; a reversed note also ends at its window like a forward one, so the long tail of a note is not extra work. Measure on the unit (the tester): `diag cpu reset`, `diag cpu perf off`, 60 s with PLAYBACK off, 60 s with REV12 (LATCH on), alternated twice. Accept: REV12 within 3 pp of off, underruns 0, over-budget windows not above off's (QA-PRF-154). QA-PRF-154 and REV12 were measured on fqa.15 (section below): the gate failed (+5.34 pp). The row is kept by the decision of 2026-10-25, not replaced.

## OS-PERF-LFO (2026-10-25; `docs/specs/OS-PERFFX.md` section 11): the five Perform LFO rows

The five rows start, re-rate and end the voices' existing LFO generators (`mod_start`, the transport-phase generator of OS-007m) at note and pad events, in flash (`VoicePool::perf_dest`); no per-sample code is added. With the rows off nothing runs and nothing is evaluated (cost 0). With them held, each destination adds the work its modulator has anyway: **[est]** tremolo about 1.0 k cycles per voice-block, cutoff about 1.1 k (eight `flfo_eval` + `svf_set_cutoff`), fine tune about 0.4 k (`repitch`), pan about 0.15 k, position small: about 2.7 k cycles with all five, **about 6 to 7 % of a 128-sample block on twelve voices**. Built: `.ramfunc_l` 27,332 to 27,280 B (-52 B: two group-14 branches of `Engine::dispatch` left, one byte test for the voice's modulator choice (`dmod`) came to `post_stages`), FLASH +1,180 B, SRAM_U +512 B (184 B of real growth, `PerfSlots` +90 B in `g_voices` and its export copy `PerfSave` +90 B in `g_engine`, which overran by 68 B the 116 B of slack before `g_bdt`'s 512-byte alignment: it moved one step, 444 B of padding now lie in front of it; spec OS-PERFFX 11), SRAM_L -56 B.

Measure on the unit (the tester; the Dense8 song of section 5): `diag cpu reset`, `diag cpu perf off`, a 60 s window with the five rows off and one with VOL LFO, PAN LFO, TUNE LFO, FLT LFO (with a held LOWPASS) and POS LFO (wavetable instrument) held at +3 on the twelve AFFECTED tracks (arm LFO12, LATCH on), alternated twice. Accept: the rise within the estimate (7 % of a block) with no underrun, off within the noise (+-1.2 pp) of the previous image. Then by ear (QA-PRF-160) and the export check (QA-PRF-161, `cmp`). Measured on fqa.15, section below: LFO12 +18.11 pp, over the accept line; accepted on 2026-10-25.

## OS-BUG-MODDRIVE (2026-10-30; `docs/specs/OS-BUG-MODDRIVE.md`): Overdrive and Bit depth on module instruments

`Engine::play_block` tests `VoicePool::shape_mask()` once a block (four loads and three ORs, in `.ramfunc_l`) and, only while a track holds FX 38 / 39 or the layer's OVERDRV / BITDEPTH, calls `Engine::shape_modules` (flash), which runs the sampler's own `shape_pcm` over every shaped module track's 64-sample block. With nothing held the cost is the test. The cost falls on the governor's `base` (engine cycles less instance cycles), so admission narrows for every module while it runs, and a shaped track is not shed for it.

Size (mk66, scoped build vs dev before the change): `.ramfunc_l` 27,332 -> 27,380 B (+48 B, at the 48 B ceiling the orchestrator accepted; headroom under the 27 KiB budget 316 -> 268 B), a third exception to the net-zero rule after P7 and P8; SRAM_L +48 B (49,608 -> 49,656); FLASH +256 B (932,728 -> 932,984); SRAM_U and SDRAM_STATIC unchanged. Follow-up: moving the gate's call into an existing flash function would give the bytes back.

Expected [est, not measured]: about 1.1k / 0.5k / 1.3k cycles a block per shaped module track (drive / depth / both: 0.42 / 0.20 / 0.49 % of 261,120), about 3.9 % at eight shaped tracks. Measure on the unit (the tester): `qa_cpu_moddrive.json` on `ModDrive1`, `ModDrive4`, `ModDrive8` (`o3pj_tool gen moddrive<n>`) against their `off` controls, the mean `render last` of each pair; `bench voice_od|voice_bit|voice_odbit` for the per-sample cost; `diag mod gov`.

Measured [unit, tester, 2026-10-30, image `v1.0.1-moddrive.1` = 3091e535; tables in `build-images/v1.0.1-moddrive.1/REPORT.md`, not in git], mean `render last`, shaped minus off:

| Pair | Off | Shaped | Delta | Underrun / shed | Note |
|---|---|---|---|---|---|
| ModDrive1 | 34,031 | 34,160 | +128 cycles (+0.05 pp) | 0 / 0 | below the estimate |
| ModDrive4 | 45,642 | 48,476 | +2,835 cycles (+1.09 pp) | 0 / 0 | below the 4 x 1.3k estimate |
| ModDrive8 | 174,779 | 176,980 | +2,201 cycles | 0 / 0 | not a like-for-like pair: eight TB-303 instances sit at the module limit and the governor drops notes in both songs (212 off, 424 shaped); over-budget 13,877 / 13,876 blocks; window max 256,748 cycles (98 %) |

Underrun 0 and guard shed 0 in all six runs. Ordinary projects with no FX 38 / 39: t-v16-copy mean 64,994 cycles, no underrun; Dense8 mean 225,692, +746 against `v1.0.1-arrows.1` (224,946), an image without OS-USBOUTS and OS-BUG-FWLIST; the A/B against `v1.0.1-usbouts.1` (the "cost when off" check) is open. Run it per project with `--var PROJ=ModDrive1|1off|4|4off|8|8off` (the folder name under `Projects/`; the default is `ModDrive1`).

## OS-PERF-CPU2 (2026-10-30; `docs/specs/OS-PERF-CPU2.md`): the backward fast path (A1) and the pan sent only when it changed (P1)

Both are bit-identical (hashes captured before the change, a randomized differential test for A1; `audio_voices_dir_test.cpp`, `audio_perflfo_test.cpp`). Sizes are from `tools/remote_ci.sh mk66` against dev 599c4b7c (release image; the bench image `-DO3OS_BENCH=ON` in the last column):

| Part | `.ramfunc_l` release | `.ramfunc_l` bench | FLASH release | SRAM_L | Note |
|---|---|---|---|---|---|
| A1 (b5b0b46b) | 27,380 -> 27,472 B (+92) | 27,416 -> 27,508 B (+92) | 932,984 -> 933,076 B | 49,656 -> 49,744 B | D1: up to +96 B, the fourth exception to the net-zero rule; headroom 268 -> 176 B; `fast_steady` 98 B and `fast_ramp` 120 B unchanged |
| P1 (tried, dropped) | 0 | 0 | 0 (it was +64 B) | 0 | the code is reverted (orchestrator, 2026-10-30: no gain, see below); its tests and the bench rows stay; the bench image holds `voice_pan` |

Host bench (`tests/bench/audio_bench.cpp`, Release, ns a block of 12 voices; relative figures, the M4F differs):

- A1, 12 reversed one-shot voices against forward ones: 1.25x before, 1.00x after (the same fast loop).
- P1, per LFO destination through the whole engine (voices, `apply_pan_mods`, the graph's pan retarget), before / after in two runs each: no modulator 12,629 / 12,607 ns; VOL LFO 1.05x / 1.05x; **PAN LFO 1.04x / 1.04x (13,155 and 13,162 ns before, 13,139 and 13,193 ns after: no measurable difference)**; TUNE 1.01x; FLT 1.01x; POS 1.01x of the wavetable row.

Why P1 saves next to nothing: `Graph::set_voice_pan` already compared the pan with `Strip::pan_offset` and returned when equal (graph.cpp:312), so a repeated value never reached `retarget_pan`; P1 only saves the call into that function (a dozen cycles a voice-block). The cost of the PAN LFO is then in the voice-side work every LFO destination pays (`mod_block` in flash: `flfo_eval`, `dmod`) and in the retarget of the blocks where the whole pan unit does change. P1's code was therefore reverted (no gain for 64 B of flash; the smallest change that does the job). Its nine hash scenes in `audio_perflfo_test.cpp` (`pan sent only when it changed`: a Panning FX over a held LFO, its end, a live pan edit under a clamped LFO) stay as regression tests of today's behaviour, and the bench rows stay for the unit split.

Unit figures: **open** (QA-PRF-171 REV12 against the 3 pp gate, QA-PRF-172 `voice_rev`, QA-PRF-173..175 `voice_pan`, the PAN LFO row and the P0 stage split on the bench image: `diag bench worst` on the PAN-only window). `diag bench voice_pan` renders one voice with an instrument pan LFO on the pool alone: it measures the voice side, not `Engine::apply_pan_mods`; the strips' share is in `diag bench worst` / `diag cpu` (the "strips" stage).

## Perform arms measured on the final candidate (fqa.15, 2026-10-25)

Source: `build-images/v1.0.0-fqa.15/REPORT.md`, sections C1 to C5 (tester, on the unit; the scripts and captures are in `build-hil/v1.0.0-fqa.15/`, not in git). The release image is `o3os-v1.0.0-fqa.15-854e58d.ptf`, the code of `merge/batch-0125g` 854e58dd. Per window: `cpu reset`, `cpu perf off`, 5 s settle, then 12 reads of `cpu` 5 s apart (about 60 s). Arms alternated off, ON, off, ON. Notes for these arms are played from `Master` or `Song`: leaving Performance for any other screen ends every Perform effect (QA-PRF-016).

Scenes. **One-shot scene:** stock Dense8 with `Samples/REC_001.wav` (one-shot, 597,376 frames) loaded in RAM into instruments 1 to 6, so six voices sound (base load about 49 %). **Stock Dense8:** the reference of section 5 (about 86 %). Its samples are looped, and a loop ignores PLAYBACK and SMP END, so REV12 measures nothing on it.

| Arm (QA-PRF) | Scene | Off, mean % | Held, mean % | Delta | Gate | Result |
|---|---|---|---|---|---|---|
| SMP12 (152) | one-shot | 49.3 / 49.5 | 49.7 / 50.0 | +0.39 pp | within the 1.2 pp noise, 0 underruns | PASS |
| SMP12 | stock Dense8 | 86.8 / 86.0 | 87.0 / 86.7 | +0.48 pp | same; every block is over budget at 86 %, so the split does not separate here | PASS |
| REV12 (154) | one-shot | 49.7 / 50.1 | 55.2 / 55.2 | +5.34 pp | within 3 pp; over-budget blocks not above off | **FAIL** (accepted) |
| REV12 | stock Dense8 (loops) | 86.0 / 87.7 | 86.3 / 86.6 | -0.37 pp | not a test: the effect is ignored on loops | not run |
| LFO12 (162) | one-shot; five rows and a held LOWPASS at +3 / 50 | 49.1 / 49.1 | 67.2 / 67.1 | +18.11 pp (worst +22 pp) | the rise within the 6 to 7 % estimate | over (accepted) |

- REV12: about 0.89 pp of a block per sounding reversed voice (about 2,300 cycles a block), linear in the number of reversed voices. Over-budget blocks 499 / 501 against 114 / 119 with it off (about four times).
- LFO12, one window per row (off 48.9 and 49.3 % around them): LOWPASS alone +8.6 pp; VOL LFO +0.5; PAN LFO +4.3 (about 1.9 k cycles a voice-block, against the 0.15 k estimate); TUNE LFO +0.4; FLT LFO +1.1 over LOWPASS alone; POS LFO on a plain sample +0.3. The five rows alone add up to 6.6 pp in the split; together with LOWPASS held they cost +9.5 pp over LOWPASS alone and +18.11 pp in the arm, against the 6 to 7 % estimate.
- SNDMOD, one voice per module (`mod note`, `perf` lines): TB-303 38,678 cycles a block (14.8 %, against 38,653 on fqa.14), Chip 5,938 (2.3 %, 5,861), FM 26,834 (10.3 %, 26,842). Unchanged in substance. Not run: eight TB-303 voices and a chord.
- Dense8 on the release image (section D): mean 86.6 %, worst block 107.9 %, 0 underruns, 0 shed. Bench image (C5, default guard N = 64, 48 reads over 2 min): mean 86.8 % (range 83.1 to 92.3), worst 107.4 %, 0 underruns.

**Decision (decided by the orchestrator, 2026-10-25; the maintainer is away).** QA-PRF-154 (REV12) failing its 3 pp gate, and QA-PRF-162 (LFO12) above its estimate, are accepted and are not blockers. The maintainer's standing rule (2026-10-20) is that features are not cut for CPU: the cost appears only while the rows are held, and the release notes list it as a known issue. Follow-up after 1.0 (OS-PERF-CPU2, IDEA): an rt-optimize pass on the reversed voice path (it has no fast path) and on the PAN LFO (about 12 times its estimate).

## OS-DJ C1 (2026-10-20; `docs/specs/OS-DJ.md` section 2.6): the master scratch stage

The stage sits in the first master chain slot (flash, `-Os`, through the graph's `MasterFn`), bypassed (cost 0) unless SCR or TAPE STOP is on. **Unmeasured: every figure here is an estimate [est], the unit check measures it.** While armed it copies each block to the ring (one 64-frame loop of two 16-bit stores a frame at half scale, plus the float to Q15 conversion) and, while a head is audible, reads the ring: two interpolation taps, so 4 uncached SDRAM halfword loads a frame, plus the 2 stores: **6 uncached SDRAM halfword accesses a frame**, about 1.5 to 2 % of the block's cycle budget while scratching (the same loop with the head silent skips the loads: about 0.3 % armed), and 0 bypassed. The integer lag (review fold) adds one float to int conversion and a compare a frame, under 0.2 %. A figure over 3 % of a block on the unit, or an underrun while scratching, returns this stage to the optimisation list (the ring in SDRAM is its cost; SRAM_L has no room for a ring).

Measure on the unit (section 5): arms F (idle floor), S0 (the DJ page, SCR armed, hand still: the head is silent), S1 (SCR on, the encoder turned steadily so the head is audible), T (TAPE STOP held), against the same song with the stage bypassed; `diag cpu` windows as in the OS-PERFFX arms.

## Links

`docs/specs/OS-MODMX.md`, `OS-LOOPER.md`, `OS-COREFX.md`, `OS-METRO.md`; the `rt-optimize` guidelines; the project status notes (OS-PERF3, OS-GOV); ADR 0013 (render rules, D7 placement).

## OS-DJ CPU pass (2026-10-20): the DJ page at rest, the kills held

Unit, int.14 (884ad9e plus B1), four visits A B A B, underruns 0, `diag cpu` means [unit, tester]:

| Arm | Delta | Reading |
|---|---|---|
| DJ page at rest against Perform page 1 | +1.5 pp (+1.1, +1.8, +2.2, +0.9) | nothing is in force: expected 0 |
| KILL LOW / KILL MID / all three, against DJ rest | +3.5 / +4.1 / +3.0 pp | the five-section kill, [est] 3.7 % held |
| LP50 / HP50, against DJ rest | +0.4 / -0.9 pp | noise level |
| kills and filter together | +3.6 pp | max block 64 % (167k cycles), 15-17 over-budget events a window (0-1 with the filter only) |

**The rest cost is not in the render path** [verified by reading and by test, not yet by a unit stage split]. At rest the page tells the engine nothing: `dj_publish`, `dj_fx_publish` (change-gated `set_dj_layer`, `apply` only on a change), `dj_tempo_publish` and `dj_scratch_poll` push no command; the only values it ever sends are the 12 `XfGain` cuts (180 at the centre, 255 for THRU), once, with a 20 ms glide, and `recompute_strip` runs per event, not per sample. The stages Headroom, Eq and Bass are bypassed (their slots' flags) and are not called; the limiter is always on in Perform too. `tests/audio_master_limiter_graph_test.cpp` now asserts both (the graph with the stages at rest is the graph without them, bit for bit; with the 12 cuts at 180 the only active master stage is the limiter). So the +1.5 pp is what the DJ page does around the render (redraws and SPI/DMA traffic that preempt or stall it, the flash-resident master stages after a cold cache), or the cuts' -3 dB on the limiter's input. To localize it on the unit: `diag cpu reset`, 60 s on each page, then `diag bench worst` (stage split) and `diag bench stages`, DJ rest against Perform page 1; a delta in `stage_master` or `stage_strips` would reopen the render side, a delta spread over the stages is the UI.

**The kills held, cheaper.** A held kill (weights arrived) now runs only the sections its live terms need: the weights are (g_low, g_mid - g_high, g_high), so LOW alone needs sections 1 to 3 (the second split's high half), LOW + MID and LOW + HIGH sections 1 to 4, MID + HIGH sections 1, 2 and 5 (the all-passed LOW), MID, HIGH and all three (silence) the full loop (all three keeps its states alive for the release). A term of weight exactly 0 was a multiply by 0, so the output is bit for bit the old held loop's. The left-out sections are marked stale and primed when the next ramp runs (the second integrator from the section that feeds it, the first 0): the band they feed ramps up from weight 0, so the settling (a few samples at 2.8 kHz) is under 2 % of a band's level; the exit test (every mask, 200 held blocks) holds the old click bound. Estimates from the section counts [est, 75 cycles a channel-sample for five sections, 69 instructions]: KILL LOW 3.7 % -> about 2.1 % (sections 1-3), KILL MID unchanged 3.7 %, KILL HIGH unchanged, all three unchanged. The bench image has `diag bench dj` (`dj_kill_low`, `dj_kill_mid`, `dj_kill_high`, `dj_kill_all`, `dj_filter_lp50`, `dj_filter_kill_low`: cycles a block, ramps run down, loud noise): the before (any image before this change, same arms) and the after are the unit's to read; the numbers above are estimates until then.

## OS-CPU10: Heavy without underruns (slice 1: the Paula path; slice 3: the TX ring)

Goal: Projects/Heavy at zero underruns on the unit (unmuted mean about 266k cycles of 261,120, spikes to 430k). All figures below marked [est] are arithmetic from the disassembly and the fqa.3 split, not measurements; the unit measurement comes with the candidate image (HW check pending: `cpu reset`, `cpu perf off`, Heavy 3 x 60 s, unmuted, mute counter flat; the I1/I2 `cpu` split is not in this slice).

| Item | What changed | [est] gain a block | Bit-exact proof |
|---|---|---|---|
| A1 | `Channel::blep_add`: the 16 taps are two straight runs (no ring mask per tap), four taps a group, the table reads taken before the stores | -14..-17k | `paula_oracle_test` (tolerance 0 against pt2-clone, fuzz), the pinned digest |
| A3 | `Channel::generate`: phase, step, held value, BLEP phase, ring index and count in locals, the fetch inline | -1.5..-3k | same |
| A2 | `Channel::run_q15`: channel, model filter and Q15 conversion in one loop, one code instance (the model's flags are read once); `scratch_` (256 B) is gone; `Filter::process`, `Channel::run` and the three-pass `AmigaVoices::render_ref` are flash (the specification) | -2k | `amiga_voices_test` Twin: fused against `render_ref`, int16 equal per block, Off/A500/A1200 x LED, an inactive channel on a held voice; `paula_oracle_test` run_q15 against the three passes and the oracle |
| play_live | `Engine::play_live` (1,468 B) is flash; `play_block` keeps the empty-queue test | about +10 cycles | digest, e2e |

Digest: `amiga_voices_test` pins an FNV of every fed int16 for 11 scenarios (c1878120, from the code before the change); host and qemu (Cortex-M4F float) agree and still do after.

Bytes (remote mk66, dev 5ff06aed against this branch): `.ramfunc_l` 27,608 -> 26,488 B of 27,648 (bench image 27,644 -> 26,524), so 1,160 B of SRAM_L are free for slice 2. Flash: the bench image +208 B. SRAM_U: 138,728 B (bench 139,240 -> 138,728 B: the two Amiga pools lose `scratch_`).

Slice 3 (in; `hal::audio::kTxSlots` 2 -> 3, approved by the maintainer 2026-10-23; ADR 0013 amendment 6): the block rendered for boundary `s` plays at `s + 2`, so the render's deadline is two blocks and a single late block (the 430k spikes of Heavy against 261,120 cycles) no longer underruns; two or more blocks late still do. It changes no cycle count of the render: it moves the deadline. Costs: output latency +1.45 ms (one block; line-in to out about 5.8 ms, `midi_cfg::kAudioLeadCycles` 261,120 -> 522,240 cycles follows the constant) and the TX ring is 4 physical slots, 1 KiB, +512 B of SRAM_U (`audio_io_rings` row stays 2 KiB; ATTR[SMOD] 10, the ring 1 KiB aligned). Remote mk66: SRAM_U 138,728 -> 139,240 B in the normal and the bench image, which leaves 24 B of the 32 KiB free floor in both (stack limit at 168 KiB minus 139,240 minus 32,768; the linker ASSERT passes); FLASH 900,132 B (bench 928,236 B), `.ramfunc_l` 26,496 B (bench 26,532 B). Tests: `audio_slot_ring_test` (N = 3 properties in the 4-slot ring), `audio_pipeline_test` and `audio_fake_test` scripts written in `kTxSlots`, `mk66_audio_config_test` (SMOD 10). **Still to measure on the unit:** Projects/Heavy underruns in `diag cpu` (the goal of OS-CPU10); the 24 B of SRAM_U margin means the next SRAM_U byte needs a decision. `AmigaVoices::render_ref` is test-only but compiled into the image (part of the bench image's +208 B of flash, a 256 B stack array, never called on the target).

Main-loop guard (OS-MAINGUARD, 2026-10-23; ADR 0013 amendment 7): a render at or above the 60 % governor budget that never lets the main loop finish a pass is cut short by the render itself. After N such renders in a row with no main pass between them (N = 8 when first written; the default is 64 since 2026-10-24, see section 4), `Pipeline::render()` sheds (a faded silent block, the callback not called) until the loop has completed a pass, then renders N more. The cost to the audio is a share of blocks, not cycles: with a main pass of P blocks of CPU the shed share is about ceil(P) / (N + ceil(P)), at N = 8 11 % for a half-block pass and 27 % for 2.5 blocks, at N = 64 about 1.5 % and 4.5 % (QEMU model at N = 8, `audio_fake_test`, a render that takes the whole block, callbacks of 75 to 85 % of a block plus the fake's 26 %: 7 to 11 % and 26 to 28 % of the blocks measured, no underrun, no mute; guard off, the loop passes 2 times in 800 blocks). `diag cpu` prints the shed blocks, runs, late and the longest run since boot: under overload the longest run is the main pass's cost in blocks, the first real measure of what a pass costs while Heavy plays. A load under 60 % never sheds (the count decays on any under-budget render). Per-block cost of the guard in the render: one relaxed load of the beat, one compare and one flag, no cycle reads of its own (about a dozen instructions). Hole in the stems tap and the recorder: 64 frames per shed block. **Unit check:** Heavy on a guard image: the console answers, Stop works, the heartbeat LED blinks, `diag cpu` shed > 0; a 50 % project sheds 0.

Bytes of the guard (remote mk66, against the slice 3 figures): FLASH +1,416 B (900,132 -> 901,548), `.ramfunc_l` +424 B (26,496 -> 26,920 of 27,648: 728 B free), SRAM_U +0 (139,240 B, 24 B of margin kept).
