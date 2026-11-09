# OS-ODD11: muted tracks keep rendering (root cause, options, measurement plan)

Status: **study and spec only, no firmware code (2026-10-16)** · item ODD-11 (`docs/roadmap/BACKLOG.md`), approved by the maintainer to study · Level C if built (audio render path; the voice pool and the strip pass are touched) · branch `chore/OS-ODD11`.

Evidence markers: **[code]** read in the tree at dev 0c82737 (file:line), **[unit]** measured by the tester on the unit (v0.6.3 / v0.6.4; the numbers are the tester's, quoted in the brief of this item; the two reports are not in the repository), **[est]** estimated from instruction counts or from other specs of this repository (`docs/specs/OS-CPUOPT1.md` section 2, `docs/architecture/cpu-budget.md`), never measured for this item. Behaviour classes: **verified parity**, **new Community OS behaviour**, **open research question**.

## 1. The problem

[unit] With every track muted, 6-8 voices keep rendering and cost 60-85k cycles per block (the block is 261,120 cycles at 180 MHz and 64 frames: 23-33 %). Muting a track therefore buys no CPU, and a song that is close to the budget is not relieved by muting parts of it.

The tester's count says "voices"; it does not say how many are sampler voices and how many are module instances (a TB-303 instance alone is about 56.6k cycles, `cpu-budget.md` line 9). **Which engine the 60-85k belongs to decides how much of it this item can recover** (question Q1).

## 2. Root cause

A mute is a gain, nowhere else. The model flag `kTrackMute` becomes the Track parameter Mute, then the graph strip's `mute`; nothing between the sequencer and the strip looks at it for an audio track.

| Step | What happens | Where |
|---|---|---|
| Sequencer | A muted SAMPLE, module or Amiga track keeps starting notes and running FX; only MIDI tracks are held back at note level. Deliberate (OS-MUTE) | `seq/include/o3os/seq/sequencer.h:280-283`, `seq/src/sequencer.cpp:1323`, `app/src/step_actions.cpp:24-26` |
| Engine | `play_block()` renders **every** playing voice into its track block, then hands each block to the graph, whatever the strip is | `audio/src/engine.cpp:384` (`voices_->render`), `:390-396` (Amiga, modules), `:399-407` (`graph_.feed`) |
| Voice pool | `render()` visits all 12 voices; a playing voice always runs the sample loop, the tremolo, the filter and the shaper | `audio/src/voices.cpp:654-679` (`render`), `:603-652` (`render_voice`) |
| Graph | `recompute_strip` sends the muted strip's level and both send gains to 0 (`graph.cpp:530-544`). `run_tracks` still copies the voice block (`graph.cpp:691`), then finds the level closed and skips the rest (`graph.cpp:715-721`) | `audio/src/graph.cpp` |

So the **root cause is `Engine::play_block` / `VoicePool::render` (`engine.cpp:384`, `voices.cpp:654`): the voice render is unconditional and knows nothing of the strip.** The graph's own saving (a closed fader skips the pan, buses, taps) starts after the expensive part. Muting saves the strip pass and the metering, nothing of the voices [code].

The cost of a sampler voice per block, from `OS-CPUOPT1` section 2 [est]: the interpolation loop about 1.0-1.5k cycles (`voices.cpp:176-289`, 12-20 cycles a sample), the SVF filter about 2.9k (45 cycles a sample), up to 2.3k for the six live LFOs, 0.5k for tremolo, 0.5k for overdrive / bit depth. A plain voice is about 1.3k, a voice with filter and LFOs 6-8k. Eight of the latter are 50-60k, the same order as the unit figure.

What a muted track costs and still must keep, as built [code]:

- Its voice stays allocated (`VoicePool::find` is per track, one voice per track), so voice stealing, `diag voices` and the sample bank hand-over (`slot_in_use`) see it.
- Its sends are closed by the mute (`graph.cpp:543-544`), so no signal reaches the send effects; their tails ring on in their own memory (send effects run every block, `graph.h:47`).
- It is not metered and not staged for the USB stems: the strip is skipped before `finish_track` (`graph.cpp:715-721`).

## 3. Compatibility facts

| Fact | Class | Evidence |
|---|---|---|
| A track's mute is the strip's level at 0; the voices go on; **an unmute is heard at once, mid-note** (a sustained or looped sample comes back where it would be) | new Community OS behaviour (OS-MUTE) | `sequencer.h:280-283`, `step_actions.cpp:26`; pinned by `tests/play_after_load_test.cpp:516-548` (`peak[1] > 500` for the muted track's voice, strip 0 until the unmute) |
| Whether the official firmware's mute is note-level | open research question: the repository holds no evidence; nothing says what the official firmware does with a note that sounds at the moment of the unmute | `docs/formats/og.md` has the mute flags of the file, not the playback behaviour |
| Solo is a mute of every other track, sends included, in the live mixer | Community OS choice (the official firmware's live behaviour is an open research question) | `graph.h` "Solo", ADR 0013 "Solo and the sends", `OS-005` "Solo and the sends" |
| Mute and solo ramp over one block (1.45 ms) and are click free; a quantised mute (MUTE MODE Bar / Pattern) reaches the strip at its boundary sample through `StripMute` | new Community OS behaviour | `engine.cpp:156-157`, `smoothing.h:10`, `graph.cpp:333-339` |
| A strip that carries no signal does not ramp: its smoothers snap to their targets | new Community OS behaviour, tested | `graph.h:37-39`, `OS-005` line 97 |
| Export: a stem pass pushes the strip mutes itself (`push_mixer`, masked-in tracks unmuted whatever the project says, all others muted); the project's muted track keeps its stem. The stem and the selection export render through `Engine::render_offline`, which runs the same `play_block` | new Community OS behaviour (OS-WAVEXP) | `seq/src/offline.cpp:61-100`, `app/src/export_stems.cpp:122-132`, `docs/specs/OS-WAVEXP.md:194`, `engine.cpp:604-616` |
| Meters: a muted strip shows nothing; USB stems: a muted strip stages nothing | new Community OS behaviour | `graph.cpp:715-721` |
| The looper, the metronome click and the recorder's REC bus are not fed by a muted track (REC is post-fader) | new Community OS behaviour | `graph.cpp:583-640`, `finish_track` |
| Voices: 12 in the pool, one per track, the oldest stolen only by a live note over 12 sounding tracks | new Community OS behaviour (documented) | `voices.h:11`, `:257` |
| The ParamChange ring is drained **inside** `Graph::render`, after `play_block`; a live parameter edit is deliberately taken one block after the voices render | new Community OS behaviour (what the official firmware does is an open research question) | `graph.cpp:610`, `voices.h:232` |

What this fixes for the design: a fix must not change what is heard, in particular not the mid-note unmute, not an export, not the sends and not the meters. A change that makes an unmute start silent is a **behaviour change** and the maintainer's call (Q2).

## 4. Options

All savings are [est] until section 5 measures them. "Quiet" below means the strip is inaudible: level closed and both send gains closed (a muted track, a track silenced by another's solo, a fader at 0 with its sends shut). A fader at 0 with open pre-fader sends is not quiet, nor is a master DRY MUTE with open sends.

### Option A: do not start notes on a quiet track (note level)

The engine drops a `NoteOn` for a track whose strip is quiet; a voice that sounds when the mute arrives is released or cut once the strip has closed.

- Saving: about 100 % of the muted sampler voices, 57-84k for the tester's scene if it is all sampler [est]. The gate itself is a mask test per note.
- Behaviour: **an unmute does not resume a held or looped note until the track's next note** (a drone, a pad, a looped sample stays silent). This undoes the OS-MUTE decision. Also, the strip state the engine reads is one block old (the drain follows the voices), so a note that lands in the block of an unmute is lost.
- Risk: **medium-high** (a musical regression and a lost note at the edge, 10-20 lines of code). Tests: `play_after_load_test.cpp:516` changes meaning.

### Option B: keep the voice, advance it without rendering it (recommended)

A voice of a quiet track keeps its state (position, amplitude ramp or envelope stage, the cutoff ADSR's clock, the one-shot's end) but skips what makes sound: the sample loop's interpolation, the tremolo, the filter, the shaper, the block zero-fill. `render_voice` already walks the voice in segments of constant envelope; in a gated segment the `run_*` call is replaced by a closed-form advance:

- one-shot or forward loop, no backward / ping-pong, not granular, not a wavetable window, no residual: `phase += inc * n` (a one-shot that ends inside the span stops at the same block as today), a loop wraps with the same `loop_start + (idx - loop_start) % loop_len` the loop uses (one division, a wrap of many loop lengths equals the lazy per-sample wraps), the amplitude ramp is `clamp(amp + step * n)` (a linear ramp with a sticky clamp, so the closed form equals the sample-by-sample result);
- every other voice (backward, ping-pong, granular, wavetable, a voice that holds a residual tail) renders as today. The fallback is the safety valve, and also keeps the first slice small;
- the block is not touched (`touch()` is not called), so `VoicePool::feed(track)` is null and the graph's `make` is false: `copy_block` and the insert loop are skipped as well (`graph.cpp:662-691`);
- `mod_block` (pan, finetune, tremolo, position targets, once a block, flash) keeps running, so `inc` (finetune LFO, slide, glide) follows the exact trajectory and the position after an ungated stretch is the one an always-rendered voice has (decision Q9).

The gate is a 16-bit mask computed once a block by the graph at the end of `run_tracks` (it already visits all strips and holds each strip's smoothers), published to the pool in `play_block` before the voice render, in `begin_block()`. It reflects the previous block's state: a mute is acted on 2 blocks after the press (the ramp, then the closed state), an unmute opens it one block after the strip does.

- Saving: for a sampler voice, all but `mod_block` and about 150 cycles of bookkeeping: 80-95 % of a heavy voice, 50-70k of the tester's 60-85k if it is all sampler [est]. Plain voices save about 1k each.
- Behaviour: **unchanged**, with one engineering requirement. The ungate block is where it can go wrong: the strip opened in block N (the drain, after the voices), but the gate was still closed in block N, so no feed arrives and `run_tracks` snaps the strip's gains to their targets; in block N+1 the voice arrives at full gain with a non-zero waveform value: a step (click). The voice must therefore **re-enter with its own short attack** (the `kAttackSamples` = 32 ramp of a note on, `voices.h:267`, reusing `ramp_left` / `amp_step`; for an enveloped voice a separate declick multiplier), and its filter state is reset on re-entry. With that the unmute is one block (1.45 ms) later than today and click free. Without it the option must not be built.
- Risk: **medium-high, Level C**. State continuity (each state that advances during a gated block must be listed and tested), the re-entry declick, and the offline path (section 6, H3).

### Option C: render, but skip the post stages while quiet

The sample loop runs and writes the block, but the tremolo, the filter and the shaper are skipped for a quiet voice (their state is reset on re-entry). Stays within `render()` (`voices.cpp:665-678`): three `if`s.

- Saving: the filter (about 2.9k), tremolo and shaper (about 1k), up to 3.9k of a 6-8k voice, 25-35k for the tester's scene [est]; nothing for a plain voice.
- Behaviour: unchanged apart from the re-entry (the filter state is cold; a click is possible on a high-resonance filter, so the same declick as B applies).
- Risk: **low-medium**. It is the cheap first slice of B (the gate mask and its plumbing are the same), not a replacement for it.

### Option D: do nothing, document it

Muting is a mix tool, not a CPU tool. Record the fact in `docs/architecture/cpu-budget.md` and in the manual. Saving 0. Risk none. Reasonable if the maintainer judges all-tracks-muted a corner case: in normal play the muted tracks are a few, and an unmuted project costs the same as today.

### Not covered by A to C

Module instances and Amiga voices render in the same unconditional way (`engine.cpp:390-396`). A module instance is mono and shared by the tracks that play its instrument (`module_voices.h:8`), has its own governor and may hold state the ABI cannot skip. A gate for them is an instance-level decision ("every track that owns it is quiet", skip `render()`, keep the clock the module's ABI allows) and is a separate item with its own ABI check. If the tester's 60-85k contains module instances, A to C recover only the sampler part.

### Comparison

| Option | Saving, tester's scene if all sampler [est] | Unmute mid-note | Risk | Flash / SRAM_L [est] |
|---|---|---|---|---|
| A: drop notes | 57-84k (about 95-100 %) | **resumes at the next note** (behaviour change) | medium-high | +0 / +80 B |
| **B: advance without rendering** | **50-70k (80-95 %)** | resumes as today (declick, 1 block later) | **medium-high** | +0..150 B / +250..400 B |
| C: skip post stages | 25-35k (35-45 %) | resumes as today (cold filter) | low-medium | +0 / +60..120 B |
| D: document only | 0 | as today | none | 0 |

**Recommendation: B, built in two slices.** Slice 1 is C plus the whole plumbing (the graph's quiet mask, the engine hand-over, the offline switch, the counters, the re-entry declick), which already returns a third to a half of the cost at low risk and proves the gate on the unit. Slice 2 adds the closed-form advance of B behind the same gate, once slice 1 has been measured. If after slice 1 the unit figure meets the maintainer's target (Q3), slice 2 is dropped. Modules and Amiga stay a separate item (Q4). Option A is the fallback only if the maintainer decides that the mid-note unmute is not worth its complexity (Q2).

## 5. Unit A/B measurement protocol (tester, after the go; one image per arm)

Per `cpu-budget.md` section 5 and `OS-CPUOPT1.md` section 5: the same build id except for the feature, one project, the `diag cpu perf off` side (the governor's extra counter reads would colour the figures, `cpu-budget.md` line 83).

1. **Project.** t-v16 plus the TB-303 line (the reference project) and, as a second project, the tester's own 60-85k scene. Same file, same card, same song position (start the song from row 1).
2. **Split first (Q1).** Before any build: on the current image, with all tracks muted, record `diag cpu`, `diag voices` (in use / peak / steals) and `diag mod` (instances and cycles per voice). From these, the share of the 60-85k that is sampler voices and the share that is module instances.
3. **Arms, alternated at least 4 times (A B A B A B A B), 60 s each, per image:**
   - U: nothing muted. M: every track muted (Shift + the 12 mute pads, as `platform/mk66/tools/hil/mute_solo.json`). H: half the tracks muted (a partial mute is the common case). S: one track soloed.
   - For each window: `diag cpu reset`, `diag cpu perf off`, play 5 s (settle), `diag cpu reset`, play 60 s, then `diag cpu` (render max, mean, underruns, over-budget), `diag voices`, `diag mod`. With a `-DO3OS_BENCH=ON` image: `diag bench worst` for the stage split (voice render against the strips and the master).
4. **The floor.** One window on an empty project (no pattern notes): the same readings. The all-muted figure after the fix is judged against this floor, not against zero: the send effects, the master chain, the limiter, the USB return and the strip pass run whatever is muted.
5. **Quantities.** Mean and max of the render in cycles and in % of 261,120; M minus floor (the remaining waste); U unchanged between images (the cost on an unmuted project must not rise: the gate mask is expected at about 50-100 cycles a block, under 0.05 %).
6. **Pass thresholds (proposed, Q3).** On slice 1: M mean at least 35 % below the M mean of the baseline image; slice 2: M mean within 5k of the floor; U mean within noise (the tester's ±1.2 pp paired noise, `OS-CPUOPT1` section 1) of the baseline; underruns 0 and over-budget 0 in every window.
7. **Sound.** Hearing and the USB AUDIO check (`o3audio.py`, level / click / silence): (a) mute then unmute a sustained looped sample and a note with a long envelope: it comes back mid-note, no click; (b) a quantised mute (MUTE MODE Bar) at its boundary; (c) solo and unsolo; (d) mute during a note release; (e) mute, play on 30 s, unmute: the track is exactly where it would be (the position of a loop). (f) Stem export of a project-muted track (it still holds its audio) and a selection export, byte-compared to the baseline image's files. (g) The first hit after an unmute, or after a step at volume 0 (slice 1): it loses about one block (1.45 ms) to the 32-sample ramp; listen that it is a soft edge, not a missing note (the strip opens one block late, the voice's re-entry ramp is 32 samples).
8. Report the delta table into this file (section 9 style of `OS-CPUOPT1`), SRAM_L / SRAM_U / flash deltas from the linker usage table of `tools/ci.sh mk66`.
9. **Scripted (`hil/cpu_mute_ab.json`, one pass = U M H S).** The scene is the controlled one of `OS-CPULAYOUT.md` section 8: t-v16 with FILTER On on its 16 instruments (the project saves it Off) and pattern 9 looping; each arm starts from a stopped transport, so the arms run the same material from the same position. Mutes and solo are set explicitly (read from the pad LEDs on the Pattern screen with Shift held, toggled only where wrong, asserted; arm M also checks the master silent). The mean is that of 12 reads of `render last` 5 s apart (`diag cpu` prints last and max only); the report keeps it as the `stat` line of each arm. A scenario of this kind is [new Community OS tooling]; the unit run is still open.

## 6. Hazards (ranked, for the risk review)

| # | Hazard | Applies to | Mitigation |
|---|---|---|---|
| H1 | **Click at the ungate** (the strip snaps while no feed arrives, section 4 B) | B, C | the voice re-enters through an attack ramp and a cold filter; a host test measures the largest step across the ungate |
| H2 | State that does not advance during a gated block: the cutoff ADSR's clock (`ModGen::since`), the amp envelope (`env_left`, stage), a slide / glide ramp, `inc` from a finetune LFO, a one-shot's end, the filter LFO's tick (global, `tick_q_`: unaffected) | B | list every field `render_voice`, `mod_block`, `filter_run`, `trem_span` and `shape_span` move, advance or consciously reset it; the equivalence test below |
| H3 | **Export and offline.** A stem pass mutes tracks by ParamChange and unmutes the masked-in ones; the first export block would run on a gate that still says "quiet", losing up to 64 samples of the stem of a project-muted track | B, C, A | the gate is off while `offline_entered_` (`Engine::render_offline` calls `play_block` too, `engine.cpp:604-616`) and for the first block after any change of the strip set; the exported files are byte-identical to the baseline image's |
| H4 | The state is one block old (the drain follows the voices, `graph.cpp:610`; reordering it would shift every live parameter edit by a block, `voices.h:232`: not proposed) | all | the design accepts a 1-block-late open and a 2-block-late close; no note is dropped in B and C |
| H5 | Quiet is not "muted": a fader at 0 with open pre-fader sends, a master DRY MUTE with open sends, the export's solo half (`set_solo_mutes_sends(false)`) | all | the predicate is "level closed and both sends closed" read from the smoothers, never from the mute flag |
| H6 | SRAM_L is the hot placement (`O3OS_AUDIO_HOT`, `tools/check_ramfunc.py`); `render_voice` is in it | B, C | the delta is measured at once (`tools/ci.sh mk66`); a budget of 512 B SRAM_L is proposed |
| H7 | A gated voice still counts in `voices_in_use`, steals and `slot_in_use`; `end_block` publishes the load | all | unchanged by design (a voice that exists is counted); add the gated count only to the test hooks |
| H8 | Tests that pin today's behaviour | B, C | `play_after_load_test.cpp:516` (the muted track's voice block has a peak) changes to the voice state (note-ons and position), see section 7 |

## 7. Host tests to add (doctest, registered with the `audio` label; qemu run for the target goldens)

Pure logic only; none of these proves a cycle.

1. `tests/audio_graph_test.cpp`: **the quiet mask.** Muted after the ramp (not during it); fader 0 with open pre-fader sends is not quiet; solo elsewhere with `solo_mutes_sends` true and false; a closed route; master DRY MUTE with and without open sends; an inactive strip; the mask follows a quantised `StripMute` event at its sample.
2. `tests/audio_engine_test.cpp` or a new `audio_gate_test.cpp`: **equivalence.** Two pools get the same notes and events; the second one is gated for K blocks. After each block: `phase`, `amp`, `ramp_left`, `env_stage` / `env_left`, `mg[].since`, `playing`, `inc` are equal, and a one-shot stops at the same block. Cases: plain one-shot, forward loop (wrap by many loop lengths), amp ramp, enveloped voice through all stages and its release, finetune LFO, slide / glide, cutoff ADSR; the block of the first ungated render after K gated blocks equals the reference block except in the declick region (section 6 H1).
3. **Fallback.** Backward, ping-pong, granular, wavetable and a voice with a residual are never gated: their output is bit-identical to an ungated run.
4. **Ungate click.** A sustained loop, gate K blocks, ungate: the largest sample-to-sample step across the first block is not above the attack ramp's slope bound (and equals the baseline's within the declick region).
5. **Unmute heard mid-note** (replaces the peak assertion of `play_after_load_test.cpp:516`): the muted track makes its note-ons, its voice block is null while quiet, its strip stays at 0, and after the unmute the strip peak is above 500 within 3 blocks.
6. `tests/audio_offline_test.cpp`: **export is unchanged.** The stem of a project-muted track and a selection export with the gate on are byte-identical to the same pass with the gate off; the first block of a stem is not gated.
7. `tests/audio_pipeline_test.cpp`-style budget check is not needed; the cost is the unit's.
8. Sim: no scenario changes expected. Goldens that could move: none for the sim (the muted track's strip output is 0 before and after); `tools/ci.sh qemu` must run (target goldens run only there, `audio` change). Regenerate nothing by hand.

## 8. Flash and SRAM estimate [est]

| Slice | Flash | SRAM_L (hot) | SRAM_U | SDRAM_STATIC |
|---|---|---|---|---|
| 1 (mask, plumbing, post stages skipped, declick) | +100..200 B | +100..180 B | +8 B (mask, counters) | 0 |
| 2 (closed-form advance) | +0..100 B | +150..250 B | 0 | 0 |
| Total | +100..300 B | +250..430 B | +8 B | 0 |

No table, no buffer, no persistent data. The unit cost added to an unmuted project is the mask: about 50-100 cycles a block [est]. The implementer reports the real deltas from the linker usage table (`build-os-ci-mk66/ci-build.log`); a figure above 512 B SRAM_L is raised before merging.

## 9. Docs to write with the build

- `docs/architecture/cpu-budget.md`: a row for ODD-11 with the measured M / U / floor figures and the arm table of section 5.
- `docs/qa/catalogue/mixer.md` (mute / solo rows): the behaviour is unchanged, so one row per unmute case of section 5 item 7 (mid-note, quantised, solo, release), each with its unit scenario.
- `platform/mk66/tools/hil/`: a scenario `cpu_mute_ab.json` derived from `cpu_play.json` and `mute_solo.json` (generator `gen_hil.py`).
- `app/CLAUDE.md` / `audio/CLAUDE.md`: one line each if the entry points move (`Graph::quiet_tracks`, `VoicePool::set_gate`).

## 10. Decision table (recommendation first)

Decided by: the maintainer for behaviour (Q2, Q3), the spec/risk review for the rest.

| # | Question | Recommendation | Alternatives | Class |
|---|---|---|---|---|
| Q1 | How is the tester's 60-85k split between sampler voices and module instances? | Answer from `diag voices` / `diag mod` on the current image (section 5 step 2) **before slice 1 is queued** | none; without it the saving is a range | measurement |
| Q2 | Must an unmute resume a sounding note (mid-note), as OS-MUTE built it? | **Yes: keep it** (option B / C) | No: option A, simpler, an unmute is silent until the next note and a note at the edge may be lost | new Community OS behaviour (keep) |
| Q3 | What is the target and the success threshold? | M mean at least 35 % below baseline after slice 1; within 5k of the empty-project floor after slice 2; U unchanged within noise | a single number from the maintainer | decision |
| Q4 | Module instances and Amiga voices: in this item? | **No, a separate item** (mono shared instances, governor, ABI) after Q1 shows whether it is needed | include in slice 2 | decision |
| Q5 | The slicing | **Slice 1 = gate + post stages (option C) + declick, measure, then slice 2 = closed-form advance** | build B in one go | decision |
| Q6 | What is "quiet"? | **The strip is inaudible: level closed and both sends closed**, from the smoothers (mute, solo elsewhere, fader 0 with shut sends); not the mute flag | mute flag only (misses solo, simpler) | decision |
| Q7 | Export and offline | **Gate off while the engine is offline and for the first block after a strip-set change**; exports byte-identical | gate on (loses the first block of a muted stem) | decision, tested |
| Q8 | Who computes the gate | **The graph, at the end of `run_tracks` (one pass), read by the engine in `play_block`** | the engine queries each strip (more calls); the sequencer's `silent_` mask (misses the export mutes, wrong for stems) | design |
| Q9 | Keep the per-block modulator evaluation (`mod_block`) in a gated voice | **Yes: exact position and pitch state after an ungated stretch** (costs up to 2.3k for a voice with every LFO) | skip it, accept a small position drift after unmute | design |
| Q10 | Voice kinds in slice 2 | **Forward one-shot and forward loop only; backward, ping-pong, granular, wavetable, residual render as today** | extend later with a proof per kind | design |
| Q11 | Unmute latency | **Accept up to 1 extra block (1.45 ms), click free; the close is 2 blocks late** | reorder the drain before the voices (shifts every live edit by a block: not proposed) | accepted by design |
| Q12 | Filter state at the ungate | **Reset it (svf_reset) with the declick** | keep the stale state (can click on a resonant filter) | design |
| Q13 | Live pad notes on a muted track | **Same gate** (a pad plays into the track's strip, silent today, stays silent) | exclude live notes | new Community OS behaviour (unchanged) |
| Q14 | Counters | **Test hooks only** (`gated_voice_blocks` in `Stats`); `diag voices` output unchanged | add to `diag voices` (format change for the HIL parser) | design |
| Q15 | Rigor level | **Level C** (audio render path): this spec, a risk review, one independent reviewer, qemu run, unit measurement | Level B (not allowed by the classification) | process |
| Q16 | What does the official firmware do with a note that sounds at an unmute? | Leave open; does not block B or C (behaviour unchanged) | research with the tester on an official unit if option A is chosen | open research question |

## 11. State for implementer

- Read first: `audio/src/engine.cpp:340-412` (`play_block`) and `:427-457` (`render_block`), `audio/src/voices.cpp:603-683` (`render_voice`, `render`), `audio/src/graph.cpp:524-546` (`recompute_strip`), `:647-725` (`run_tracks`), `audio/include/o3os/audio/smoothing.h:60-104` (`Smoother`, `closed()`), `seq/src/offline.cpp:61-100` (the export's mutes).
- Nothing in `seq/`, `model/`, `app/` changes; no persistent format, no ParamId, no event layout, no ABI changes. The sequencer keeps emitting notes for muted audio tracks (`sequencer.h:280`).
- Slice 1 plumbing: `Graph` builds `std::uint16_t quiet_` at the end of `run_tracks` (strip active, `level.closed()`, both `send_gain[]` closed, not the input strip, not the audition strip), exposes it as a render-context accessor; `Engine::play_block` passes `quiet_ & gate_enabled` to `VoicePool::begin_block` / `set_gate`; `gate_enabled` is false while `offline_entered_` and for one block after a strip change. In `VoicePool::render`, a gated voice (not `releasing` into a residual, no `resid`) skips `trem_span`, `filter_run`, `shape_span`; on the first block after the gate lifts the voice restarts its attack ramp and `svf_reset`s.
- Slice 2: in `render_voice` a gated voice of the simple kind replaces `run_steady` / `run_ramp` by a closed-form advance and does not call `touch()`; `mod_block` still runs; `ModGen::since` advances by the gated samples.
- Keep: `O3OS_AUDIO_HOT` placement, no calls into flash on the per-block path (`tools/check_ramfunc.py`), no allocation, no division except the one wrap.
- Do not change: the `kTrackMute` semantics, `Graph::recompute_strip`'s gains, the drain order, `StripMute` timing.
- Tests: section 7 (items 1 to 6 are required, the equivalence test is the gate of slice 2). Run scoped: `ctest --test-dir build-os-host -R "audio|play_after_load" --output-on-failure -j2`, then `tools/ci.sh mk66` once under the CI lock and `tools/ci.sh qemu` before merge. Report the FLASH / SRAM_L / SRAM_U / SDRAM_STATIC deltas.
- Hardware verification remaining after the host tests: section 5 (A/B cycles, the six listening checks, byte-compared exports). A host test is not hardware verification.
- Open before slice 1 is queued: Q1 (the split), Q2 and Q3 (the maintainer).

## 12. Slice 1 as built (2026-10-17, branch feat/OS-ODD11)

Differences from the plan above, all inside its decisions:

- The gate is `Graph::quiet_tracks()` (16-bit mask, tracks 0..14: the audition strip is left out; strip active, level and both send gains closed in the smoothers), computed at the end of `run_tracks`, read by `Engine::play_block` into `VoicePool::set_gate()`.
- A gated voice runs its sample loop and `mod_block` and skips the tremolo, the filter and the shaper; the cutoff modulator's clock (`ModGen::since`) goes on by the span. Its block is **not fed** to the track (the strip is closed or just opening, so the graph settles it: this is the plan's B behaviour for the block, kept in C to avoid one unfiltered ramped-in block at the open).
- Re-entry: `Voice::dk` holds the re-entry state (`kOwed` while gated, then the 32-sample ramp left); the ramp is the graph's gain kernel over the first `kAttackSamples` samples of the voice's output, and the filter is reset. A new note clears it. One byte in an existing alignment gap: the Voice and the SRAM_U row do not grow.
- Q7 (export): the gate is empty in `Engine::render_offline` (`Graph::clear_quiet()` before `play_block`) and for the first live block after the export (`offline_leave`); no hold counter. Test hook `Engine::set_voice_gate_mask()` (0 turns the gate off) is what the byte-identity test compares against.
- Q14: the counter is `VoicePool::Stats::gated_blocks` (blocks in which some voice was gated, a cheaper count than per voice); `diag voices` is unchanged (slice 2 adds `advanced` to it, section 13).
- Hot code: SRAM_L +288 B in the release image (the hot budget is 27 KiB and had 332 B left, not the 512 B of H6; the bench image has 4 B left), FLASH +364 B, SRAM_U 0.
- Tests: `tests/audio_gate_test.cpp` (quiet mask, gate and re-entry on a pool, unmute mid-note through the engine, export byte-identity and the first live block after it); `play_after_load_test` asserts the gated blocks instead of the muted voice's peak (H8).
- Open for the unit: section 5 (the arms, `cpu_mute_ab.json`), the listening checks, the byte-compared exports.

Review fold (Level C, 2026-10-17):

- The cutoff ADSR of a gated voice keeps its place: `VoicePool::gate_cut` (flash, called once a gated span) adds the span to `ModGen::since` and, once it has reached a block, folds it into the envelope with `fenv_eval` (the 16-bit `since` no longer wraps after 65,536 gated samples, about 1.5 s); the first span after the gate evaluates the cutoff afresh. An LFO keeps no clock of its own, so it needs nothing. Test: `audio_gate_test` (1300 gated blocks of a 2 s attack).
- A note start sets the voice's `gated_` bit from the new track's gate bit (in `mods_start`, flash, once per note: `note_on` is SRAM_L code and has 4 B to spare in the bench image): a voice reused in the middle of a block (the gated voice ended earlier in it, or the oldest voice of a gated track was stolen) no longer keeps its previous occupant's bit, so the new note's first block is fed and ramped. Test: `audio_gate_test` (a one shot ends in span 1, a note of another track takes the voice at sample 17). The samples of the block before that offset still belong to the previous occupant, as for any stolen voice.
- Known edge, accepted: the first hit after an unmute (or after a step at volume 0) loses about one block (1.45 ms) to the 32-sample re-entry ramp, and the strip opens one block after the gate lifts (Q11). Unit listening check 5.7 (g); QA-MIX-025.
- Sizes after the fold, against the first slice 1 commit (release and bench images alike): SRAM_L -8 B (hot code does not grow), FLASH +128 B, SRAM_U 0.

## 13. Slice 2 as built (2026-10-18, branch feat/OS-ODD11-S2)

The closed-form advance of section 4 B, behind the slice 1 gate. Differences from the plan, all inside its decisions:

- **Which voices.** A gated voice takes the closed form when it is of the plain kind: `mode == 0` (a one shot, a beat slice, or a forward loop), no residual tail (`resid == 0`), not a wavetable (Q10; `run()` has no window code, so a wavetable would be exact too: a follow-up with its proof, not built). Backward, ping-pong, granular, wavetable, and a voice that holds the fade-out of a retrigger (a block or less) render as in slice 1; the residual voice is advanced again from the span after the tail is out.
- **Where.** `VoicePool::gated_span` (flash, `O3OS_AUDIO_COLD`, one call per gated voice and span, in place of `odd_span`'s call) runs `render_seg<true>`, the segment walk of `render_voice` as one template with two instances (`render_seg<false>` is `render_voice`: its machine code is unchanged, 0x118 B). The sample loop is replaced by `advance()`; `env_segment`, `env_advance`, `stop_voice` are the render's own functions. `odd_span` stays for the other kinds; both use `gate_mark` (the voice's `gated_` bit, `dk = kOwed`, the cutoff clock).
- **The arithmetic.** Position: `phase + (n - 1) x inc` wrapped once with the loop's own `loop_start + (idx - loop_start) % loop_len` (fraction kept), then `+ inc`: that is the state the lazy per-sample wraps leave (a wrap subtracts whole loops, which commutes with the steps), so `phase` is equal bit for bit, not only the sound. A one shot is out when the last sample's integer part has reached `end` (stops in the same span). Amplitude: two clamps (the first step, then `n - 1` steps), exact for any start value. No division except the wrap's UDIV, no `double`.
- **Block and `last`.** The voice's block is not zeroed or written; `touched_` bit `16 + i` marks the visit (the "first span of the block" test reads both bits), so `end_block` feeds nothing and `mod_block` still runs once a block. `last` is 0 while advanced (the voice put nothing out), so a stop or a retrigger of a gated voice carries no tail; the re-entry ramp covers the open. **By design** (decided in the review fold, pinned by tests): a gated stop or retrigger carries no residual tail, and a one shot that ends while gated is dropped up to one block earlier than the rendered one (the fade-out its last sample would have had runs for one block after the end and falls partly in the block the gate lifts; there it is silent instead, and `resid` is 0). A retrigger on the very first sample of the block the gate lifts has no tail either; a retrigger later in that block carries the last sample of the span rendered before it (under the 32-sample re-entry ramp), as any rendered voice does.
- **Superseded 2026-11-06 (OS-BUG-LATENT-EXPFADE): `Engine::offline_enter` now drops every live tail, so neither side carries this fade-out. The text below is the old corner.** **Accepted corner: an export that starts over a gated voice.** `Engine::offline_quiet()` cuts every voice (`engine.cpp`, `VoicePool::cut`), and `cut` carries `last` into a residual. A voice that was advanced has `last == 0`, so when an export starts while a muted track's plain voice sounds, the first 64 samples at most of block 0 of that stem lack the fade-out the baseline (no gate) has; every later block is the baseline's. Not changed: engine behaviour stays (QA-MIX-024; `tests/audio_advance_test.cpp` on a pool, `tests/audio_gate_test.cpp` through the engine). The tail is arguably the wrong thing to have in the stem at all (it belongs to a live voice cut before the export): a possible separate item, **"offline_quiet drops residuals"** (clear `resid` in `offline_quiet`, which would make the baseline equal the advanced voice); not part of this slice.
- **Test hooks and the counter.** `Stats::advanced_spans` (spans taken) and `VoicePool::probe()` (inline: phase, inc, amp, gain, gain_prev, envelope stage / level / left, ramp_left, resid, last; no code unless a test calls it). `Stats` grew by 4 B inside the existing alignment: SRAM_U 0. **`diag voices` shows the count** (Q14 revisited for the unit A/B, which needs proof that the closed form was taken): `voices  in use 9/12  peak 9  steals 0  advanced 4021`, the new field last so that the HIL parser, which reads the start of the line, is unchanged. The one increment is in `gated_span` (flash) as a relaxed atomic store of the single writer; `voice_snapshot()` reads it from the main loop, and `end_block` (hot code) is not touched: `.ramfunc_l` does not grow. About 690 a second per muted plain voice (one span a block, more when an event falls inside the block); 0 when nothing is muted.
- **Tests.** `tests/audio_advance_test.cpp` (suite `audio_advance`, also on QEMU): two pools, one never gated, one gated in windows, the same events in the same spans (cut at event offsets and at a pseudo-random point); after every block the probes are equal, a one shot stops in the same block, the gated voice is not fed, and after the gate lifts the output is the reference's (all 64 samples from the second block, from sample 32 of the first). Sweeps: one shots over 11 pitches, 3 lengths, 2 gate starts; 10 loops (down to 2 samples, wraps of 1000+ loops a block) over 10 pitches with two gate windows; the attack ramp gated from the first sample, NoteOff at six offsets, a retrigger while gated, a 300 ms fade, a cut; 8 envelopes (stages from 8 to 5,600 samples) with and without NoteOff, two velocities, two amounts; a finetune LFO (4 rates), a finetune ADSR, a glide; the other kinds (backward, ping-pong, three granular types, a wavetable) are never advanced and equal; the residual case. Added in the review fold: a gated one shot that ends before the gate lifts (silent lift block, `resid` 0, the next note is the reference's), a retrigger in the lift block at five offsets, the export corner on a pool, a live loop edit (`apply_loop`), a live volume edit (`regain`: ramp, enveloped, in release) and a voice taken in the middle of a block (a steal, a note of another kind, a pan / finetune modulator each); the probe also carries `resid` and `last`, and the span counter is compared block by block (fold of the second review): in every gated block in which the voice sounded at its start, `advanced_spans` rose by at least one, but for the one block per plan that renders a residual tail (an aggregate over the run would pass with half the gated blocks rendered, since every block has two spans or more). Mutations of `advance()` (an eager wrap, a late one-shot end, no `mod_block`, **no final amplitude clamp**, a clamp at the new gain instead of the larger of the two), of `gated_span` (`last` kept, a block marked touched) fail it. The final amplitude clamp IS reachable: `start_release` rounds the step up, so for the 300 ms fade (13230 samples from 65536) the step is 5 and 5 x 13230 > 65536: the amplitude is 0 for the last 122 samples, and the fade gated through its end (blocks 3 to 214 of 230) pins it. `audio_gate_test` (engine level) also asserts the muted looped track's spans were advanced, and pins the export corner above.
- **Hot code.** `.ramfunc_l` +36 B (render() 0x19c -> 0x1c0: the call, one more mask test), against the 150..250 B estimated: `render_voice` and the sample loops are untouched. The release image has 60 B left under the 27 KiB budget, the bench image 24 B. The review fold leaves it as it was (27,588 B release, 27,624 B bench, the same as at 401300a): the new `diag voices` counter is stored in `gated_span`, which is in flash.
- **Sizes** (`tools/ci.sh mk66`; first measure against dev b5313e5): FLASH +652 B (release; the segment walk is a second copy in flash, 0x244 B for `gated_span`; the estimate was +0..100 B), SRAM_L +40 B (the hot code +36 B, rounding), SRAM_U 0, SDRAM_STATIC 0. Tone image FLASH +636 B, bench image FLASH +652 B, SRAM_L +32 B. **After the review fold and the merge of dev 744cdc9** (both trees built in one CI hold; dev's own growth since b5313e5 is not in these figures): release 792,676 B against 791,968 B, FLASH +708 B, SRAM_L 49,864 against 49,824 (+40 B), `.ramfunc_l` 27,588 against 27,552 (+36 B), SRAM_U and SDRAM_STATIC 0; bench FLASH +700 B, SRAM_L +32 B, `.ramfunc_l` 27,624 against 27,588 (+36 B); tone FLASH +700 B. The fold costs about 56 B of flash (the `advanced` field, its argument and text, the help line).
- **Expected saving** [est]: a gated plain voice no longer pays the interpolation loop (1.0-1.5k cycles a block) and the block zero-fill (about 70); it pays about 150-250 cycles of flash code (`gated_span`, `mod_block` when it has modulators, one segment). With 6 plain voices, about 6-8k below slice 1.
- **Open for the unit.** Section 5 with the thresholds of Q3 for slice 2: M mean within 5k of the empty-project floor, U within noise of the baseline, underruns 0, over-budget 0; the listening checks 5.7 (a) to (g) again (the unmute is the slice 1 re-entry, now after a closed-form advance); (e) is the check this slice adds: mute a looped sample, play on 30 s, unmute, the loop is where it would be (QA-MIX-026); a one shot that ends while muted (QA-MIX-027); an enveloped note muted through its stages (QA-MIX-028). **Evidence that the closed form ran:** read `diag voices` at the start and the end of each window: `advanced` must rise in the M and H arms (about 690 a second per muted plain voice) and stay equal in the floor and U arms (and in a slice 1 image the field does not exist). An M arm whose `advanced` does not rise measured slice 1 or a project with no plain muted voice, not the closed form.

## 14. Unit A/B result and decision for slice 2 (2026-10-18)

**[unit]** Tester run, bench images of dev 744cdc9 (A, slice 1 only) and of f5d1070 (B, slice 2 with its review fold); eight visits A B A B A B A B, four runs of `hil/cpu_mute_ab.json` per image (the tester's copy adds a `voices` read before each window and an arm F, the floor: an empty project), scene and method of section 5, `diag cpu perf off`. The report is the tester's and is not in the repository; the figures are quoted from it. Means of 12 reads of `render last`, in cycles of the 261,120-cycle block:

| Arm | A (slice 1) | B (slice 2) | B - A | Over-budget blocks per window, A | B |
|---|---|---|---|---|---|
| floor (empty project) | 29,761 | 29,673 | -88 (-0.03 pp) | 0 | 0 |
| U (nothing muted) | 133,171 | 132,121 | -1,050 (-0.40 pp) | about 690 | about 690 |
| M (T01..T12 muted) | 53,145 | 35,645 | -17,500 (-6.70 pp) | 0 | 0 |
| H (T01..T06 muted) | 107,207 | 96,136 | -11,071 (-4.24 pp) | 27, 24, 23, 24 | 0 |
| S (T02 soloed) | 56,057 | 38,932 | -17,125 (-6.56 pp) | 0 | 0 |

Verdict against the thresholds of Q3 / section 5:

- **U within +-1.2 pp of A: pass** (-0.40 pp; U is the cost on an unmuted project, so slice 2 adds nothing measurable there).
- **Underruns 0 in every window, both images: pass** (0 in the 8 visits).
- **Over-budget 0 in every window:** met in M, H, S and the floor on B; **in H the blocks over budget went from about 24 a window to 0**. Arm U holds about 690 over-budget blocks a window on both images (A 699, B 685 on average): the baseline already misses this in U and B is not worse; it is the scene's cost, not the gate's.
- **M within 5k of the floor: miss by 972.** M(B) 35,645 against floor(B) 29,673 gives 5,972 (+2.29 pp) against the 5,000 target. Per visit (M minus the floor of the same visit): 8,522, 5,699, 5,232, 4,438, so one of four visits is within 5k. For reference M(A) minus floor(A) is 23,384: slice 2 removes 17,500 of it, about three quarters.
- **`advanced` showed the closed form ran** (B only, the tester's timed windows): flat at 0 in U and flat in the floor; +4,208 a second in M (12 muted, about 700 a second for each of the 6 voices sounding); +2,788 a second in H (about 4 muted voices). In use 6 to 7 voices a window, steals 0.
- The two looped wavetables of t-v16 (instruments 4 and 6, which slice 2 leaves rendering) are not played by pattern 9, so they do not explain the 5,972 in this scene; what the remainder is made of (the mod blocks of the muted voices, the strips, the master and the sends, which run whatever is muted) was not split by the run.

**Decision (maintainer, 2026-10-18): the merge is accepted despite the M miss.** The 5k target was a proposed threshold (Q3), the remaining figure is 972 cycles (0.37 % of the block) over it, every other criterion passes, and H went from over budget to in budget.

**More testing needed** (not run in this session; the QA rows say `more testing needed`): the listening checks 5.7 (a) to (g) (QA-MIX-020 to QA-MIX-028), the byte compare of a stem and a selection export against the baseline image's files (QA-MIX-024), and a dense-event scene (microtiming or ratchets on muted tracks; the worst block of A against B). The tester needs a project that holds the dense scene to run it on both images.
