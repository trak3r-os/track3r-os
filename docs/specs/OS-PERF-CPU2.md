# OS-PERF-CPU2: cheaper reversed voices and Perform PAN LFO

Level C (the hot audio path). New Community OS behaviour: no change in sound, only in cost (both fixes are bit-identical). Follow-up of the Perform arms measured on fqa.15 (`docs/architecture/cpu-budget.md`, "Perform arms measured on the final candidate"); queued by the maintainer on 2026-10-29.

## Goal

- (a) A reversed voice (PLAYBACK = `Voice::mode` 4, `kModeReverse`, OS-PERF-REVERSE) costs about 2,300 cycles a block more than a forward one (REV12 +5.34 pp on the one-shot scene, QA-PRF-154). Bring REV12 to +3 pp or less.
- (b) The Perform PAN LFO costs about 1.9k cycles a voice-block (+4.3 pp, QA-PRF-162; 0.15k estimated). Cut what can be cut without a change in sound, and measure where the rest goes.

## Causes (design pass, 2026-10-30, dev 67579087)

- (a) `render_seg` sends every voice with `mode != 0` to `run_dir` = `run<true, true>` (audio/src/voices.cpp:402, :772-774), whose checked loop (:324-388) runs per sample two index clamps, the backward branch, a 64-bit subtract, the window and stop tests and an amp clamp, with about 12 live 64/32-bit values (spills; :235-236 records the same effect on a smaller loop). The OS-PERF3 unchecked fast path exists only for forward voices (`if constexpr (!Dir)`, :299-323). About 36 extra cycles a sample, against the 6 to 9 estimated.
- (b) The LFO is evaluated once a block (`mod_block`, :2227-2234); the pan law is linear (`pan_gains_q16`, smoothing.h:154-166). The cost is the per-block chain: `mod_block` (flash) -> `Engine::apply_pan_mods` (engine.cpp:69) -> `Graph::set_voice_pan` (graph.cpp:312) -> `recompute_pan` / `retarget_pan` (graph.cpp:254-264, :722-730, falling back to `recompute_strip` :681 while a level or a send ramps) -> `gain_pan` instead of `gain_mac` once the strip leaves the centre (graph.cpp:917-925, :958-962). The split is unknown. LOWPASS held (+8.6 pp) is the filter itself (`svf_run`), not the LFO: out of scope.

## Decisions (orchestrator, 2026-10-30; the maintainer is away: defaults, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| D1 | `.ramfunc_l` for the backward fast path | Up to +96 B, a fourth exception to the net-zero rule (after P7, P8 and OS-BUG-MODDRIVE), recorded; more stops for a decision. Headroom is 268 B before it |
| D2 | Backward and ping-pong instrument loops | They take the same fast path (the same code, bit-identical) |
| D3 | A pan LFO evaluated every 4th block with a 4-block ramp (smoother, up to 4.4 ms late; audible) | Not now. Only if PAN LFO still costs more than 1 pp after this item, as a separate decision |
| D4 | A closed-form advance for muted reversed voices (`gated_span` refuses `mode != 0`, voices.cpp:2179) | Not now (it helps only muted voices); recorded as a follow-up |
| D5 | Targets | REV12 at most +3 pp on the QA-PRF-154 scene; PAN LFO: the measured reduction is recorded, no fixed target |

## Design

### A1: a backward fast path (hot, bit-identical)

- In `run<Ramp, Dir>` for `Dir`, before the checked loop: when the voice moves backward and `idx0 + 2 <= end` (the first sample at `end - 1` stays in the checked loop), take `k = min(n, (idx0 - loop_start) / ip)` steps with `ip = (inc >> 32) + 1` (one 32-bit UDIV, the mirror of :304-309). Over those `k` steps `q_j > (idx0 - j·ip) << 32 >= lim_lo`: no stop, no turn, no clamp, and `idx <= end - 2`.
- Reuse `fast_steady` / `fast_ramp` with the negated increment (`q + (2^64 - inc)` modulo 2^64 is the same two-word add): `fast_steady` when `amp_step == 0` and `0 <= amp <= top` (the clamp is a no-op), else `fast_ramp`. No new loop instance. The checked loop takes over for the rest.
- Placement: `.ramfunc_l`, within D1; read the size from the mk66 build and the map before and after.

### P1: send the pan only when it changed (flash, bit-identical)

The send is skipped when the pan the LFO computes equals the pan the strip already has: the comparison is with the strip's own current pan value in the graph (in `Engine::apply_pan_mods`, flash, before `Graph::set_voice_pan`), never with a value cached in the voices. Other writers of the strip's pan (rule 1 below) are then seen for free. A held LFO at a slow rate, or one whose value repeats, skips the apply chain. SRAM_L 0.

**Outcome (2026-10-30): tried and dropped.** `Graph::set_voice_pan` already returned when the strip's `pan_offset` was equal (graph.cpp:312), so P1 saved only the call: no measurable difference on the host bench, for +64 B of flash. The code was reverted by the orchestrator; the nine hash scenes (rule 1) stay as regression tests, and the unit split (P0) says where the PAN LFO's cost is.

### P0: where the rest goes (unit only)

A bench image (`-DO3OS_BENCH=ON`) of the same commit: `diag bench worst` on the PAN-only window, for the stage split (voices / strips); the figures go to cpu-budget.md and decide D3.

## Design rules from the risk review (2026-10-30, folded)

1. **P1 and the other pan writers.** Today a held LFO re-sends the pan every block and so overrides every other writer of `Strip::pan_offset`: the Panning FX, the pan slide and its end (`set_track_pan`, `panning_fx_end` -> `set_voice_pan(inst_pan)`, tracker_fx.cpp:91, :116, :135, :203) and a live instrument-pan edit (engine.cpp:102), also while the LFO value is clamped at 0 or 100. A cached "last sent" value would leave the other writer's pan in place: not bit-identical. Hence the comparison with the strip's actual value. Tests: a Panning FX over a held LFO, the FX's end, a live pan edit under a clamped LFO, each hash-equal to dev.
2. **A1 entry guard.** Take the fast path only when the voice moves backward (`bwd`), `phase >= lim_lo` and `idx0 + 2 <= end`. A live loop edit (`apply_loop`, voices.cpp:2432) can raise `loop_start` above the phase of a mode 1 / 2 voice: without the guard `idx0 - loop_start` underflows, `k` becomes `n` and the voice skips its wrap.
3. **`bwd` flips.** A ping-pong or mode 1 voice starts forward and turns: the tests drive the turn and check `bwd` on both sides.
4. **The negated increment needs a mechanism.** `fast_body` reads `v.inc`. Either store the negated value in `v.inc` for the call and restore it at once (`repitch` and `rp_ok` must never see it), or pass it as a parameter. The forward path (OS-PERF3) must keep its code: compare the size of `fast_steady` / `fast_ramp` and the forward bench before and after; if the forward code changes, re-measure the forward cost.
5. **Saturated step.** Guard (or assert) `inc >> 32 == 0xFFFFFFFF`, where `ip` would wrap and `k` be wrong.
6. **A randomized differential test.** Beside the chosen hashes, a test runs random phase, inc, loop bounds, n and amp (amp above `top` and below 0 included; `k == n - 1` and `k == n`) through the new `run` and through a scalar copy of today's checked loop, and requires equal output and equal voice state.
7. `.ramfunc_l`: read the headroom of the bench image too, not only the release image.
8. Facts checked: the A1 proof holds with the strict `>` (`q_k > lim_lo`, `idx <= end - 2`, no clamp, the neighbour read order irrelevant); the amp rule for `fast_steady` is right; the negation is exact modulo 2^64, inc with a zero integer part and inc 0 included; the export's `perf_save` / `perf_clear` / `perf_restore` touch `perf_pan`, not `pan_offset`, so P1 is safe there.

## Tests

- Bit-identity for A1: hashes captured on dev 67579087 for reversed notes, `inc` 0.5, 1.0 and 2.7, a ramp and a steady amp, segment splits 64 and 16 + 48, the window edge, the stop; and a backward and a ping-pong instrument loop (D2). tests/audio_voices_test.cpp, tests/audio_perform_test.cpp.
- Bit-identity for P1: the engine output hash of a held PAN LFO (and of one with all five rows) equal to the pre-change hash; tests/audio_perflfo_test.cpp.
- Host bench rows (tests/bench/audio_bench.cpp): 12 reversed one-shot voices; one row per LFO destination. Relative figures only.
- Device bench: `voice_rev` and `voice_pan` beside `run_voice` (platform/mk66/src/bench.cpp:322).
- Unit (unit-gated, before the merge): the QA-PRF-154 and QA-PRF-162 scenes as on fqa.15 (`cpu reset`, `cpu perf off`, 60 s windows, off / on / off / on), release image first, then the bench image for P0; the export `cmp` still passes.

## Docs

docs/architecture/cpu-budget.md (the figures), audio/CLAUDE.md (the fast path and the `.ramfunc_l` exception), docs/specs/OS-PERFFX.md (a pointer), QA rows in `QA-PRF-170..175`.

## Done when

- Host tests and the bit-identity hashes green; `tools/ci.sh` host, qemu and mk66 green; flash, SRAM_L and `.ramfunc_l` deltas in the commit.
- The unit figures recorded: REV12 at most +3 pp (D5), PAN LFO before and after, the P0 split.

## Hardware-only checks

The unit measurement above; by ear, a reversed one-shot and a PAN LFO sound the same as on v1.0.1 (bit-identical by test; a short listen to be sure).
