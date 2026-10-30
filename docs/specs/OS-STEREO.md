# OS-STEREO: stereo samples, opt-in per instrument

Level C overall (the audio path, a new instrument field). Approved by the maintainer on 2026-10-30 ("ok D": approach D with every recommended default). Delivered in steps; this spec settles step 1 in full and records the decisions the later steps build on. New Community OS behaviour.

## Facts (2026-10-30, dev 0a9b5ed3)

- Today a stereo file becomes one mono sample at load: the mean (L + R) / 2, or the left or right channel alone with `F8` (**CH MIX / CH LEFT / CH RIGHT**, OS-LOADCH; docs/manual/sample_load.md:135-139, QA-SLD-011).
- The readers can already keep a stereo file stereo (`ReadParams::keep_stereo`, formats/include/o3os/formats/format.h:97-104; the WAV and IFF readers honour it), but nothing downstream uses it: the arena slot, the voice kernels and the mixer strip are mono.
- The official firmware's mono import is documented, not verified on a unit: open research question.
- CPU: Dense8 runs at about 86 % mean on the unit, so a stereo voice has no headroom there; its cost is unknown until measured (estimate +20 to +30 % of a voice).

## Decisions (the maintainer, 2026-10-30)

| # | Question | Answer |
|---|---|---|
| 1 | How it is turned on | Opt-in per instrument; MIX (today's behaviour) stays the default |
| 2 | Voices | At most 4 stereo voices at once; beyond the cap a stereo instrument plays its mid (mono) |
| 3 | Play modes | ONE SHOT, LOOP FWD, SLICE and BEAT SLICE (the chops: the maintainer added them the same day), played forward, Modern engine only; the other modes (LOOP BWD, PING-PONG, WAVETABLE, GRANULAR) and a note played backward play the mid |
| 4 | Outputs | The USB stems and the sends stay mono (the mid); the offline stems (export) stay stereo |
| 5 | Pan | Pan becomes balance, with the same law |
| 6 | Format | A new instrument field (version bump, backward-compatible loader, round-trip test); the `.pti` export downmixes |
| 7 | Order | Document the two-track workaround first |

## Design direction (later steps; re-planned after step 1's figures)

The sample is stored as its mid M = (L + R) / 2, the same buffer a mono load makes today, plus a side-car S = (L - R) / 2 in the arena; L = M + S, R = M - S. Every mono path (the cap, other play modes, USB stems, sends, `.pti` export) keeps reading M alone. A flash stereo kernel (`.ramfunc_l` about 0 B) resamples M and S with one phase; the strip takes a second feed with balance. Editor operations act on both blocks.

**Recording in stereo (proposed; the maintainer asked on 2026-10-30, defaults to confirm).** Today a take is mono: the Record screen averages left and right and `F7` saves a 16-bit mono WAV (docs/manual/record.md:94, :137; QA-REC-034, QA-REC-039). A stereo take fits the same layout: the take is the mid, as today, and the side goes to a second block of the sample memory. Defaults: a MONO / STEREO choice on the Record screen, MONO by default (a stereo take halves **TIME LEFT**); `F7` saves a stereo WAV; `F3` makes a stereo instrument once the instrument field exists, and the mid until then; the looper stays mono. It comes after the stereo voice and the instrument field. Open: the L / R round trip through M and S can lose one LSB when L + R is odd; the step that adds the side-car settles how S is stored so that the saved WAV is bit-exact.

## Step 1 (Level B: manual plus a bench-only spike)

1. **The manual:** a short "Stereo samples" section in docs/manual/sample_load.md, after the CH MIX / LEFT / RIGHT steps: load the file twice into two instruments, one with **CH LEFT**, one with **CH RIGHT**; put them on two tracks with the same notes; pan 0 and 100. Say what it costs (two voices) and that stereo instruments are planned. QA rows `QA-FIL-120..124` in docs/qa/catalogue/sample_load.md.
2. **`voice_st` (bench only):** a new `VoiceMode::Stereo` row in platform/mk66/src/bench.cpp (`run_voice`, :322-324, next to `voice` and `voice_pan`) and a matching host row in tests/bench/audio_bench.cpp. It renders one voice reading M and S (two int16 blocks in the bench's scratch, planar) with the plain voice's interpolation and one phase, writes L and R, and is compiled only when `O3OS_BENCH` is on: the release image's flash and `.ramfunc_l` do not change. Rows at 44.1 kHz: `voice` and `voice_st` with the kernel in flash; one row with it in `.ramfunc_l`, if the bench can place it there without touching the release map, to show what the placement is worth.
3. **Output:** the per-voice cycles of `voice_st` against `voice` on the unit (tester, bench image through `fw-gate-check`), recorded in docs/architecture/cpu-budget.md. They decide whether the cap of 4 holds and how step 2 is cut.

## Done when (step 1)

The manual section and its QA rows merged; `voice_st` in the bench image only (the release build's sizes unchanged, from the mk66 log); `tools/remote_ci.sh host` and `mk66` green; the unit's `voice` / `voice_st` figures in cpu-budget.md.

## Out of scope (step 1)

The instrument field, the loader, the stereo voice in the engine, the mixer, the editor. They follow the measurement.
