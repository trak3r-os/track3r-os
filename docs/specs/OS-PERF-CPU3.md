# OS-PERF-CPU3: the Perform LFO rows' cost, and SRAM_L space back

Level C (the audio path). Approved by the maintainer on 2026-10-30. Follow-up of the corrected LFO figures (`docs/architecture/cpu-budget.md`: the OS-PERF-CPU2 and OS-PERF-LFO-STEP sections and the fqa.15 correction).

## Goal

1. Explain, then cut, what the Perform layer's LFO rows cost beyond their own work: POS +13.0 pp on a plain sample, where it moves nothing; PAN +4.8 pp, all of it in the strips (+4,647 cycles) and buses (+7,555), none in the voices (-109).
2. Give `.ramfunc_l` bytes back (176 B of headroom left after four exceptions: P7, P8, OS-BUG-MODDRIVE, OS-PERF-CPU2).

## Facts (design pass, 2026-10-30, dev fd280986)

- **POS on a plain sample does no work by code.** The layer's row and step FX 36 both go through `VoicePool::perf_index` (audio/src/voices.cpp:2387); :2400 returns -1 for a plain sample, `perf_dest` (:2412-2416) clears the bit and starts nothing: no `mod_start`, no `mmask`, no `mod_block` (`render`, :863, calls it only when `mmask != 0`). The gated advance and the fast path do not test `mmask` (:2226-2228). `audio_perflfo_test.cpp:225` pins that the output is bit-identical. Yet +13.0 pp (cpu2.1) and +13.34 pp (lfostep.1) were measured, repeatably. **The mechanism is an open research question**: the scene's voices may not be plain (a wavetable or granular instrument in Dense8?), a harness effect (the AFFECTED toggling, the pad holding another row), or a cost outside the voices (flash-cache churn).
- **PAN, strips:** a ramping destination gain makes `steady()` false (kernels_impl.h:222), so the fused pass takes `Gen<false>` (:63-75, :227) for the whole pair; a track that leaves the centre moves from `gain_mac` to `gain_pan` (graph.cpp:917; kernels_impl.h:176-181). About 3.8k cycles [est] against 4.6k measured.
- **PAN, buses:** the bus kernels are data-independent (kernels_impl.h:261-310; graph.cpp:1109-1113). Working hypothesis [est]: the flash code each modulated voice-block runs (`mod_block`, `apply_pan_mods`, `retarget_pan`) evicts the send FX loops (delay, reverb, limiter, in flash) from the 8 KiB code cache. Counter-evidence: the host bench shows 1.04x; `bench worst` is one block.
- **SRAM_L:** the `note_on` tail (voices.cpp:1411-1429: envelope / attack ramp, `dk`, `gated_`, `age`, `playing`, stats) can move into `mods_start`, already a flash long call per note: about 100 B back [est], +100 to +200 cycles a note [est] (worst block of 12 note-ons about +2.4k, under 1 %). The comment at :1424 says `gated_` was put in SRAM_L on purpose: read why first.

## Decisions (orchestrator, 2026-10-30; defaults, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| D1 | Fix POS before its mechanism is known | No: measure first (step 0) |
| D2 | An audible PAN fix: F1, the LFO pan sent with a 1-block ramp (a 689 Hz gain staircase, about 2 % per integer pan step), or F2, the pan sent every second block with a 2-block ramp (about 1.5 ms late) | Not without the maintainer's ear check and the step 0 split; recorded as options |
| D3 | Another `.ramfunc_l` exception | No: bytes come back first (slice 1) |
| D4 | VOL, TUNE, FLT rows (real work: `trem_span`, `repitch`, the SVF coefficients) | Known costs for now; TUNE (+7.3 pp, about 3.2k a voice-block) gets a bench row |

## Plan

- **Step 0 (unit, no code; the bench image `v1.0.1-cpu2.1-bench`, already gated):** Dense8 + REC_001 one-shot scene, `cpu reset`, `cpu perf off`, 60 s windows off / ON / off / ON, AFFECTED tracks set once. POS row held at +3 and at 0: `diag bench stages` (platform/mk66/src/bench.cpp:641, the mean per stage) and `diag voices`, plus the type of every sounding instrument. PAN: the LFO at a fast and a slow rate against a static off-centre pan (Panning FX 31): `bench stages` and `diag cache check`. The split says where POS and PAN cost goes, and which of F1 / F2 (or neither) is worth it.
- **Slice 1 (Level C, bit-identical): the `note_on` tail to flash** (appended to `mods_start`, no new call). Read the `gated_` reason at :1424 first; keep anything the render reads before `mods_start` runs in SRAM_L. Report the `.ramfunc_l` delta from the mk66 build and the per-note cost (`diag bench` or a host bench row).
- **Slice 2 (optional, after slice 1): the MODDRIVE gate** (engine.cpp around :410, `shape_mask` voices.h:865, +48 B): a one-byte cached mask written by every writer of `ifx_on_[1]`, `ifx_on_[2]`, `perf_.on[0..1]` (FxReset, `perf_clear`, `perf_restore` included), about -15 B [est]; or fold it into the `pan_mods_pending` test (about -40 B [est], an ordering check: the shaping must stay after the modules' `end_block`).
- **Slice 3 (after step 0 and D2):** the PAN fix, if any, and the POS cause.

## Tests

- Slice 1 and 2, bit-identical: the existing hash scenes (`audio_voices_test`, `audio_voices_dir_test`, `audio_perflfo_test`, `audio_perform_test`, `module_voices_test` with FX 38 / 39 on module tracks); a randomized differential test of the note-on voice state (random notes, FX, retriggers, gated tracks) against a copy of today's `note_on`.
- Slice 3 (if audible): a bound on the pan-gain error against the reference; a QA ear check.
- QA rows `QA-PRF-190..199`.

## Done when

Step 0's split recorded in cpu-budget.md; slice 1 merged with its `.ramfunc_l` delta (bytes back) and per-note cost; slices 2 and 3 as decided after it.
