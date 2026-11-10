# OS-CECHIP — tracker-ce's CHIP engine inside our Chip module

Status: decided and queued 2026-11-05 (Q1 a, Q2 a); specified 2026-11-05 (rewritten the same day after the maintainer's decision: an extension of `modules/chip`, not a new module), not implemented · Level B, with one Level C slice if Q1 a is chosen (A1: a module ABI minor) · Backlog row OS-CECHIP, approved by the maintainer 2026-11-05; the component (tracker-ce's CHIP engine, MIT) approved 2026-11-05 · Facts from `dev` at c1dc3125 and `polyend/tracker-ce` at 7fd6c2c (cited `tracker-ce@7fd6c2c:<path>:<line>`; the engine's code is ported under its MIT licence; `synth/lib/*.a` is never used) · Today's four waves keep their exact sound (a bit-exact gate, slice G1); the eight new waves' oscillators and post chain are **verified parity** with tracker-ce by the oracle (3.6); their envelope, arpeggio timing, sweep, vibrato, glide and velocity are Chip's own (**new Community OS behaviour**, 3.4).

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a (every recommended default): ABI 1.11 display order (slice A1, Level C) so the ten new parameters fill the free rows of ENV, PITCH and ARP; Chip 1.1 built into the image with the tracker-ce notice in `kBuiltinNotices` (OS-FLASHDIET phase 1 makes the room). Queued.** **Q2 note (maintainer 2026-11-10, OS-FLASHDIET-ROOM R3): all four built-in instruments left the image; Chip 1.1 is a card module (`modules/instruments/chip.tmod` of the SD content zip) and tracker-ce's notice (parts 1 and 2 of its LICENSE) ships next to it as `modules/instruments/NOTICES.txt` and `LICENSES/CHIP-tracker-ce-MIT.txt`, not in `kBuiltinNotices`, which no longer exists in the image. The MIT notice obligation is met by the zip that carries the file.**

Decided by the maintainer 2026-11-05: inside `modules/chip`; Wave grows from 4 to 12 values; the tracker-ce-only parameters are appended after id 19 with defaults that leave today's patches identical; the new waves use Chip's ADSR, arpeggio, sweep, vibrato, Vel Sens and glide paths; today's waves must not get slower; the MIT notice goes into Chip's headers, README and `THIRD_PARTY.md`; the module's version goes up; the oracle's original stays vendored in `third_party/`, for host tests only; shipping is Chip's (built into the image). Two questions are still open:

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | Where the ten new parameters go on the page. Under ABI 1.10 a page is a run of consecutive ids, so ids 20 and up can only join the last page, ARP (17..19). That leaves room for 5 of the 10 (O3) | a) an additive ABI minor 1.11 (slice A1, Level C): a module may give a display order, a permutation of its ids that the pages count rows from. The ten then fill the free rows of ENV (4), PITCH (3) and ARP (5); no page or key is added; an OS of 1.10 refuses a Chip 1.1 copy from a card (the built-in one always matches its image) · b) no ABI change: only five parameters (Shape, Sub Osc, Cutoff, Resonance, Bitcrush) go on the ARP page; PWM Depth and Rate, Env Amt, Decimate and Chord stay fixed (PWM 20 % at 2.5 Hz on the new waves, no envelope on the cutoff, no decimation, no chords) · c) OS-SNDMOD's D1: `O3M_PAGES_MAX` 4 -> 8 and `O3M_PARAMS_MAX` 32 -> 64, with page keys past F4; the ten go on a fifth and a sixth page | a | The smallest ABI change that fits all ten into today's four pages; every existing row keeps its place; b loses half of the engine; c is the larger UI decision that OS-SNDMOD left open (it would also unblock FM) |
| Q2 | Image flash. Chip is built in, so the image grows by about 5.5 KB of module and 5.1 KB of tracker-ce's notice: parts 1 and 2 of its LICENSE, whose notice clause asks for "the conditions in Part 2" in every copy (F1). Flash goes from 94.1 % to about 95.2 % [est] | a) both in the image: Chip 1.1 built in, the notice joining `kBuiltinNotices` as Open303's licence does · b) the image keeps Chip 1.0 (the four waves); Chip 1.1 ships as a card file that replaces the built-in one (a higher version wins, O8). Image +0; on a unit without the file, a project that uses waves 4..11 plays NOISE SHORT | a | The same project sounds the same on every unit; it is how the built-ins already carry their notices; b saves 10.6 KB only by splitting Chip in two |

## 1. Goal

Give our Chip module tracker-ce's eight CHIP models (2A03 pulse, SID saw/triangle with ring, LFSR noise, two-operator FM bell, Super Square, Zap laser, Arp Chord, Chip Drum) as Wave values 4 to 11. They come with that engine's continuous Shape, PWM, sub oscillator, four-step chords, resonant lowpass with envelope, bitcrush and decimation. The four waves of today sound exactly as they do now and cost no more. A project saved before this work loads and plays identically; one saved after it plays on an older build with the new waves falling back (3.2).

## 2. Facts

tracker-ce's engine (`core/synth/engines/ChiptuneSynthVoice.cpp`, called `CE` below):

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | MIT licence (Part 1) naming `core/synth/engines/` "including CHIP". The notice clause asks for the copyright line, the permission notice **and Part 2's conditions** in all copies or substantial portions. Part 2 1(c) expressly permits reusing the open synth engines in other projects; its restrictions concern clones of the complete Tracker application. Parts 1 and 2 are 5,104 bytes; Part 3 is about `synth/lib/` | `tracker-ce@7fd6c2c:LICENSE:4`, `:8-22`, `:30-31`, `:55-62`, `:64-85`, `:87-101` | documented |
| F2 | 25 parameters (Model, Tune, Fine, Shape, PWM Rate, PWM Depth, Sub Osc, Chord Arp, Arp Speed, Pitch Sweep, Sweep Time, Vibrato Rate, Vibrato Depth, Attack, Decay, Sustain, Release, Cutoff, Resonance, Env Amt, Bitcrush, Decimate, Glide Mode, Glide Time, Volume). Ranges and defaults: Shape 0..1 (0.35), PWM Rate 0.1..40 Hz (2.5), PWM Depth 0..1 (0.2), Sub Osc 0..1 (0.25), Chord 0..7 (Off), Cutoff 80..18000 Hz (12000), Resonance 0..1 (0.15), Env Amt -1..1 (0.2), Bitcrush 2..16 bits (8: the default patch is crushed), Decimate 1..32 (1) | `ChiptuneSynthVoice.cpp:18-44`, `:494-835` | verified (source) |
| F3 | The models and their names. Eight four-step chords (Off, Octave, Fifth, Major, Minor, Sus4, Maj7, Min7); the Arp Chord model plays Major when Chord is Off | `:47-83`, `:186-190`, `:258-268` | verified (source) |
| F4 | Helpers: PolyBLEP; `fastSine` = `std::sin(2 pi x)`; the stepped triangle `std::round(tri x 7.5) / 7.5`; a 15-bit LFSR tapping bit 6 (short) or bit 1, reseeded to 0x7FFF at a note | `:85-122`, `:130-147` | verified (source) |
| F5 | Per sample: PWM triangle LFO, `duty = clamp(0.125 + 0.75 shape + 0.38 depth x lfo, 0.06, 0.94)`, `freq = clamp(440 x 2^((totalNote - 69) / 12), 16, 18000)`; Zap adds `(12 + 24 shape)((1 - p)^3 - 1)` semitones, with p the note's age over `max(64, Sweep Time x sr)`; Pitch Sweep moves from the note **towards** note + sweep over Sweep Time and stays there | `:270-308`; `synth/include/PolySynth/sdk/SynthVoiceSDK.h:64-70` | verified (source) |
| F6 | The models: the pulse family (2A03, Arp Chord, Zap) a +-1 pulse with two BLEPs x 0.75. SID: a BLEP saw, a triangle and a saw ring-modulated by the sub phase's square, blended by the duty, x 0.8. Noise: an LFSR clocked `dt (2 + 14 (1 - PWM Depth))` times a sample (Shape >= 0.5 short), +-0.8. FM Bell: a sine modulator at ratio `{1, 2, 3, 3.5, 4, 5, 7, 9}[shape x 7.99]`, index `(0.4 + 3.5 (0.35 + 0.65 depth)) x env`, a sine carrier quantised to 6 bits x 0.85. Super Square: three pulses detuned by `+-(0.002 + 0.018 shape)`, `(p1 + 0.8 p2 + 0.8 p3) x 0.34`. Chip Drum: frequency `freq (1 + 3.5 (1 - 0.7 shape) d^2)` with `d = exp(-age / (0.08 sr))`, a stepped-triangle body, LFSR noise stepped on a wrap or an even age, mixed 0.7 for the first 180 samples then `clamp(1.25 shape)` | `:309-433` | verified (source) |
| F7 | The post chain: the sub oscillator (phase `+= dt / 2` always; mixed `sig (1 - 0.35 sub) + tri_step x 0.75 sub` above 0.001); decimation (hold `clamp(lround, 1, 32)` samples) and crush (below 15.5 bits, `round(x 2^(bits-1)) / 2^(bits-1)`); a Chamberlin SVF lowpass at `clamp(cutoff x 2^(4 envAmt env), 40, 18000)`, `f = min(0.85, 2 sin(pi fc / sr))`, damping `clamp(1 - 0.85 res, 0.12, 1)`, states clamped to +-2, skipped while the cutoff is at least 17,500 Hz and resonance at most 0.02; `out = filtered x env x volume` | `:197-205`, `:434-478` | verified (source) |
| F8 | The voice ignores velocity; CE's default patch sends velocity to Volume through the framework's macros (curve not in the open source); MIDI pitch bend +-2 semitones; Glide Mode (Always, Overlap, Legato) and a held-note stack in the SDK base | `:130-147`, `:898-951`; `SynthVoiceSDK.h:202-365` | verified (source); the macro curve: open research question |
| F9 | Mono: channel 0 written | `:168`, `:478` | verified (source) |

Our side:

| # | Fact | Where |
|---|---|---|
| O1 | Chip today: 20 parameters (Wave 0..3 PULSE, TRIANGLE, NOISE LONG, NOISE SHORT; Duty 0..3; Raw Edges; Vol Steps; Volume dB, default -12; Vel Sens, default 50 %; Transpose; Fine; Attack, Decay, Sustain, Release in ms; Sweep, Sweep Time; Vib Depth, Vib Rate, Vib Delay; Arp 1, Arp 2, Arp Speed). Pages TONE 0-7, ENV 8-11, PITCH 12-16, ARP 17-19. ABI 1.8, `state_bytes` 208, version 1.0.0, built into the image | `modules/chip/module.json`; `modules/CMakeLists.txt:172-174`, `:202-203` |
| O2 | Chip's voice: pitch (arpeggio, sweep, vibrato, the host's ramp) every `kControl` = 32 samples with one `exp2_f`, clamped to 0.45 of the rate. The envelope is linear per sample with its stage changes inside the loop; Attack and Release are at least 1 ms; a note over a sounding one starts from the old level. `run<W>` is one loop per wave, inlined into the one `O3M_HOT` function; the dispatch sends every wave >= 3 to NOISE SHORT, and `derive_gain` applies the noise gain to every wave >= 2 | `modules/chip/chip_dsp.h:23-27`, `:88-175`, `:180-238`, `:259-300`, `:302-393` |
| O3 | ABI 1.10: at most 32 parameters; at most 4 pages of at most 8; a page starts at `first_param`, the pages ascend and cover the ids in order, so appended ids join the last page | `module/sdk/o3module.h:142-152`, `:717-726` |
| O4 | A project keeps a module instrument's values as an opaque blob. `bind()` applies `min(param_count, blob / 4)` values and the rest keep `init()`'s defaults; extra values are ignored, kept and written back. Appending is format-safe without a version bump; reordering or changing an id's meaning is not; a range may only widen | `audio/src/module_voices.cpp:161`; `docs/specs/OS-SNDMOD.md:28`, `:46`; `docs/formats/o3-project.md:143` |
| O5 | Cost: Chip measured 2.8 % of a block per sounding voice on 0.4.9 (the baseline); 1.3-2.3 % mean, 4.0-4.9 % worst on fqa.14. Hot part 2,096 B; `chip.tmod` 6,540 B. The slab (13,056 B) holds the four built-ins' hot parts with about 1,040 B to spare. Cold code runs from SDRAM, whose speed for code is not validated | `docs/architecture/cpu-budget.md:59`; `docs/roadmap/BACKLOG.md` (OS-SNDMOD row); `modules/chip/README.md:54`; `modules/sid/README.md:116-123`; `module/src/loader.cpp:333-355`; `module/sdk/README.md:40` |
| O6 | Module state sits in the 4 KiB SRAM_U arena, in SDRAM on overflow | `docs/architecture/cpu-budget.md:17` |
| O7 | (Historical: since OS-FLASHDIET-ROOM R3 no image carries them; the tests do, through `o3m_builtin`.) Built-in modules: the `.tmod` bytes and the notices (`kBuiltinNotices`: `builtin/NOTICES.txt` and Open303's licence, 1,601 B) are in the image's flash; `diag mod notice` prints them; a QEMU suite plays each built-in module | `modules/CMakeLists.txt:197-203`; `tools/o3m_embed.py:5-8`; `tests/module_builtin_run_test.cpp:59-152` |
| O8 | A card copy replaces a built-in module only with a higher version | `module/include/o3os/module/builtin.h:9-10`, `:34-36` |
| O9 | The oracle precedent: Open303 vendored byte for byte, a static library built `-w`, host tests only, with a wrapper in `tests/support/` | `third_party/open303/CMakeLists.txt:11-13`; `tests/CMakeLists.txt:203-230` |
| O10 | Image flash 94.14 % (v1.1.0) | `docs/STATUS.md:7` |

## 3. Design

### 3.1 Waves and parameters

**Wave** (id 0) widens from 0..3 to 0..11; 0..3 keep their meaning. The labels are PULSE, TRIANGLE, NOISE LONG, NOISE SHORT, 2A03 PULSE, SID SAW/TRI, NOISE/LFSR, FM BELL, SUPER SQR, ZAP/LASER, ARP CHORD, CHIP DRUM (CE's model order).

**Appended parameters**, ids 20..29, in priority order: under Q1 b only 20..24 exist. A parameter that only the new waves read takes CE's default. A parameter that every wave reads takes the value that changes nothing.

| Id | Name | Range (stored) | Default | Flags, unit | Read by | CE value |
|---|---|---|---|---|---|---|
| 20 | Shape | 0..100 | 35 | MOD, percent | waves 4..11 | /100 |
| 21 | Sub Osc | 0..100 | 0 | MOD, percent | all waves | /100 |
| 22 | Cutoff | 80..18000 | 18000 | LOG, MOD, hz | all waves | same |
| 23 | Resonance | 0..100 | 0 | MOD, percent | all waves | /100 |
| 24 | Bitcrush | 2..16 | 16 | MOD (bits) | all waves | same |
| 25 | PWM Depth | 0..100 | 20 | MOD, percent | waves 4..11 | /100 |
| 26 | PWM Rate | 0.1..40 | 2.5 | LOG, hz | waves 4..11 | same |
| 27 | Env Amt | -100..100 | 0 | MOD, percent | all waves | /100 |
| 28 | Decimate | 1..32 | 1 | INT | all waves | same |
| 29 | Chord | 0..7 | 0 | INT; OFF, OCTAVE, FIFTH, MAJOR, MINOR, SUS4, MAJ7, MIN7 | all waves | same |

At the defaults the post chain is off (Sub 0, Cutoff 18000 with Resonance 0 bypasses the filter as in F7, Bitcrush 16 is off, Decimate 1, Chord OFF), so waves 0..3 take today's path. Duty and Raw Edges stay with waves 0..3. Shape and PWM shape the new waves, as in CE. CE's Tune, Fine, Arp Speed, Sweep, Sweep Time, vibrato, ADSR and Volume map onto Chip's own parameters (3.4). Glide Mode and Glide Time are not taken: the pattern's Glide and Slide FX drive the glide. 30 parameters in all (32 allowed).

**Pages** (Q1 a; existing rows keep their places, new rows go at the end of a page; page names are not stored):

| Page | Rows |
|---|---|
| TONE | Wave, Duty, Raw Edges, Vol Steps, Volume, Vel Sens, Transpose, Fine (unchanged, full) |
| ENV/FLT (was ENV) | Attack, Decay, Sustain, Release, **Cutoff, Resonance, Env Amt** |
| MOD (was PITCH) | Sweep, Sweep Time, Vib Depth, Vib Rate, Vib Delay, **Shape, PWM Depth, PWM Rate** |
| ARP/OSC (was ARP) | Arp 1, Arp 2, Arp Speed, **Chord, Sub Osc, Bitcrush, Decimate** |

Under Q1 b the ARP page holds Arp 1, Arp 2, Arp Speed, Shape, Sub Osc, Cutoff, Resonance, Bitcrush. Under Q1 c a page OSC holds Shape, PWM Depth, PWM Rate, Sub Osc and Chord, and a page FILTER holds Cutoff, Resonance, Env Amt, Bitcrush and Decimate.

### 3.2 Compatibility

- A blob of 20 values (every project and preset of today) loads with ids 20..29 at their defaults (O4), so it renders bit for bit as today. Slice G1's gate holds this.
- A project saved with Chip 1.1 and read by a build with Chip 1.0: the extra values are kept and written back (O4). A Wave of 4..11 is clamped to 3 (NOISE SHORT) by Chip 1.0's `set_param`. That is documented in the manual; nothing crashes.
- Version 1.0.0 -> 1.1.0 (`modules/CMakeLists.txt`): an older `chip.tmod` left on a card no longer shadows the built-in module (O8). Manifest ABI 1.8 (1.11 under Q1 a).
- `state_bytes` 208 -> 320 [est] (CE phases, LFSR, age, decimation, SVF, derived constants): about 12 Chip instances fit the 4 KiB SRAM_U arena instead of 19 (O6).

### 3.3 Render: the old path stays, the new one is cold

- `set_param` derives one `path` value: 0..3 when the wave is 0..3 and the post chain and Chord are off (today's path), otherwise 4 (the new path). `voice_render`'s chunk loop tests it once per chunk (at most 32 samples): below 4 it runs today's `run<W>` exactly; at 4 it calls `ce::chunk(v, out, n)`, a `noinline` function **outside** `O3M_HOT` (cold, in SDRAM). The hot part grows by the test alone (at most 64 B); waves 0..3 keep their loops instruction for instruction.
- Two traps in today's code that the port must close without changing waves 0..3: `derive_gain` tests `wave >= kNoiseLong` (it must name the two noise waves), and the dispatch's last `else` plays NOISE SHORT for every wave >= 3 (O2).
- `ce::chunk` (new header `modules/chip/chip_ce.h`, the port) runs passes over a stack scratch of 2 x 32 floats:
  1. Chip's envelope (the same per-sample code and stage changes as `run<W>`, Vol Steps included) into `e[]`.
  2. The source. On waves 4..11: CE's PWM LFO and duty per sample, then the model's kernel (FM reads `e[]`; the Drum reads its age). On waves 0..3 with the post chain on: today's oscillator without the envelope (a `raw<W>` variant of `run<W>`, cold).
  3. The sub oscillator (its phase always advances, as SID's ring reads it), decimation and crush, then the SVF with its coefficient from the control point: these are the variants chosen per chunk, each skipped when off.
  4. `out = x · e · gain` (Chip's gain: Volume, Vel Sens, no noise gain for 4..11).
- **Control rate** is Chip's (`control()` every 32 samples). For waves 4..11 a cold `ce::control` adds: CE's frequency clamp 16..18000 Hz (so the noise LFSR takes at most 7 steps a sample), Zap's drop (F5) from the note's age over `max(64 samples, Sweep Time)`, and the SVF's `2^(4 envAmt env)` and `sin` from the envelope at that point (one `exp2_f`, one `sin_f`).
- **Chord** goes into `control()`'s arpeggio. When Chord is not OFF, or the wave is ARP CHORD (which plays MAJOR while Chord is OFF, F3), the four-step chord table replaces Arp 1 and Arp 2 at Arp Speed. With Chord OFF on waves other than ARP CHORD the code computes exactly what it does today.
- **Maths policy**: the kernels and the post chain are templates on a policy. `Std` evaluates CE's expressions with `std::sin`, `std::exp`, `std::pow` in CE's order; the oracle tests use it. `Fast` uses an inline odd polynomial sine for FM Bell (error at most 2e-6), the recurrence `d *= k` for the Drum's decay, and the host's `exp2_f` and `sin_f` at control points; the module uses it. Rounding is half away from zero, inline; `fmod` of a phase in [0, 2) becomes a conditional subtraction.

### 3.4 Where the new waves differ from tracker-ce (Chip's paths; new Community OS behaviour)

| Path | Chip (used by waves 4..11) | tracker-ce |
|---|---|---|
| Envelope | Attack 0..500 ms, Decay and Release 0..2000 ms, at least 1 ms ramps; release from the current level; Vol Steps | 0..5 s; release from full scale |
| Pitch update | every 32 samples (dt held for the chunk) | every sample |
| Sweep | starts Sweep semitones away and returns to the note (linear) | moves from the note to note + sweep and stays (F5) |
| Vibrato | triangle, +-cents (0..200), with a delay | sine, up to +-1.5 semitones |
| Arpeggio timing | Arp Speed 1..120 Hz at control points | 5..75 Hz, whole samples, at least 64 |
| Glide | the pattern's Glide / Slide FX (`pitch()`, RAMP); a live legato key retriggers | Glide Mode, Glide Time, note stack (F8) |
| Velocity | Vel Sens (default 50 %) | ignored by the voice; a macro (F8) |
| Level | Volume in dB, default -12; at 0 dB the level is CE's voice at Volume 100 % | Volume x0..2, default 1 |
| Filter envelope | cutoff from the envelope every 32 samples | every sample |
| Pitch bend | none (no ABI path) | +-2 semitones |

### 3.5 CPU and memory [est]

- **Waves 0..3**: unchanged. Gate: G1's bit-exact renders, the QEMU instructions a block per wave within 1 % of G1's recording, and the unit at or below the baseline (2.8 % a voice on 0.4.9; fqa.14 1.3-2.3 % mean).
- **Waves 4..11**, cycles a sample if the code ran from the slab: envelope ~10, PWM LFO and duty ~8, the kernel (pulse ~30, SID ~40, Noise ~20 with at most 7 LFSR steps, FM ~45, Super Square ~75, Drum ~40), the sub phase ~3, gain ~4, control ~8; the post chain when on adds the sub mix ~12, decimation and crush ~8, SVF ~12; all of it x1.3 for loads and stores. That is about 2.0 points a voice (pulse, dry) to 4.5 (Super Square with the whole post chain). The code runs from SDRAM (cold), whose speed for code is not validated (O5): the unit's figure decides. If a new wave costs more than 5 points (tmod-perf), the follow-up moves its kernel into the hot part only when the slab check of the four built-ins still passes; it is not done in this item.
- **Waves 0..3 with the post chain on**: the cold path, about +1 point plus the SDRAM factor.
- **Memory**: hot +<= 64 B; `chip.tmod` 6,540 -> about 12,000 B; image flash about +10.6 KB with the notice (Q2); state 320 B.

### 3.6 The oracle (host only)

- `third_party/tracker-ce-chip/`: `ChiptuneSynthVoice.cpp` and `LICENSE` byte for byte at 7fd6c2c; `CMakeLists.txt` (ours: a static library `cechip_oracle`, C++20 for that target only, `-w -ffp-contract=off`, host only); `PROVENANCE.md` (URL, commit, SHA-256 of both files).
- `tests/support/cechip_shim/` is ours, written from the facts; no SDK code is copied. It holds the headers the original includes: `config.h`, `ChiptuneSynthVoice.h`, and a minimal `PolySynth/sdk/SynthVoiceSDK.h` with `noteToFreq` as F5 states it, `kSampleRate`, a `CustomSynthVoice` with `playMode`, `pitchBendSemitones` and the parameter-handler table, the `Patch` aggregates, `fmt::format`, `enumutil::EnumMax` and the audio buffer refs.
- `tests/support/cechip_ref.{h,cpp}`: a C++17 interface (`create`, `set(id, CE value)`, `note_on`, `note_off`, `render`). It calls the voice's public `onNoteOn`, `onNoteOff` and `renderVoiceAudio` and the captured setters.
- **What compares 1:1.** CE is driven static: Attack 0 (instant), Sustain 1 (the envelope holds 1), no vibrato, sweep, arpeggio or glide, Volume 1. Its output is then the kernel and the post chain alone, at a constant `dt`. Our kernels and post chain are driven at policy `Std` with the same `dt` (computed by CE's expression), the same Shape, PWM, sub, crush, decimation, cutoff, resonance and Env Amt, an envelope of 1 and the age from 0. Acceptance: max abs error at most 1e-4 over 0.5 s per case; only where a quantiser is in the path, at most 0.1 % of samples may be one step off, with SNR at least 80 dB. Compared by formula: the chord tables, Zap's curve and the LFSR's reseed and taps.
- **What cannot compare 1:1** is the table of 3.4: that is Chip's behaviour, held by Chip's own tests.
- **Fast against Std** (C6): SNR per new wave measured and written in the README. Bounds: at least 60 dB (FM's polynomial sine, the drum's recurrence, the SVF's coefficient through the host's maths); a wave that misses keeps `Std`'s call for that term.

### 3.7 The licence notice

- `modules/chip/chip_ce.h` (the port) carries in its header: "Contains a port of the CHIP synth engine of Polyend Tracker Community Edition (tracker-ce@7fd6c2c, `core/synth/engines/ChiptuneSynthVoice.cpp`), Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors, MIT licence with the conditions of its Part 2: see LICENSE-tracker-ce.txt". `chip.cpp` and `chip_dsp.h` get one line pointing to it.
- `modules/chip/LICENSE-tracker-ce.txt` holds parts 1 and 2 of the LICENSE, verbatim.
- The README's first paragraph changes from "written from scratch" to "written from scratch, plus a port of tracker-ce's CHIP engine (waves 4..11)".
- `THIRD_PARTY.md`: a table row and a section (what is ported, what is only a test oracle, that `synth/lib/*.a` and the SDK headers are not used). Its "Built-in modules" paragraph is updated: Chip is no longer free of third-party code, stays MIT plus tracker-ce's notice, and remains a separate `.tmod`.
- `modules/builtin/NOTICES.txt` names it, and `O3M_BUILTIN_NOTICES` adds the licence file (Q2 a), so `diag mod notice` prints it.

## 4. Slices

| # | Slice | Size | Level | Depends on | Image flash [est] |
|---|---|---|---|---|---|
| G1 | Golden renders of today's Chip (host hashes, QEMU hash and instruction counts), recorded before any change | S | A | — | 0 |
| A1 | ABI 1.11: a module's display order for its pages (only under Q1 a) | S | C | — | +0.3 KB |
| C1 | Oracle harness: vendored CE engine, shim, wrapper, THIRD_PARTY | S | B | — | 0 (host) |
| C2 | Parameters 20..29, Wave 0..11, version 1.1.0, cold dispatch, Chord, pulse family, the notice | S | B | G1, C1 | +7.3 KB (2.2 module + 5.1 notice) |
| C3 | The post chain on all twelve waves | S | B | C2 | +1.2 KB |
| C4 | SID, Noise/LFSR, Super Square | S | B | C2 | +1.0 KB |
| C5 | FM Bell, Chip Drum | S | B | C2 | +0.8 KB |
| C6 | `Fast` policy measured, perf checks, README, unit measurement | S | B | C3, C4, C5 | ~0 |
| C7 | Pages, the Wave view for the new waves, QA, manual | S | A (B under Q1 a) | C6 (A1) | +0.3 KB |

Total about +10.6 KB of image flash, plus 0.3 KB for A1.

**G1 — the gate** (S, Level A, tests only; merged before any other slice). Files: `modules/chip/tests/chip_golden_test.cpp` (new: FNV-1a of the float output bytes of today's voice for a matrix: each wave, each duty, Raw Edges, Vol Steps, three envelopes, sustain 0, arpeggio of one/two/three steps, sweep up and down, vibrato with delay, velocity 1/96/127, Volume -48/0/+6, RAMP with `pitch()`, a note off before the first sample, block sizes 1/7/64/128; about 60 cases, each 0.5 s; the constants recorded from `dev` before C2), `modules/CMakeLists.txt` (`o3m_add_test`), `tests/module_builtin_run_test.cpp` (Chip's audio hash on the Cortex-M4F and its instructions a 64-frame block per wave under icount, as the TB-303's). Acceptance: the tests pass on `dev` unchanged; `tools/ci.sh qemu`. Rule: from G1 on, no slice may change these constants.

**A1 — display order** (S, Level C: a published ABI; only under Q1 a). Files: `module/sdk/o3module.h` (minor 11; the descriptor grows one member at its end, `const uint8_t* param_order`, sized per minor as 1.8 did), `module/src/image.cpp:90-110` (the page checks: a permutation of `0..param_count-1` or NULL; refused otherwise), `app/src/module_ui.cpp` and the params screen (row k of a page is `param_order[first + k]`), `tools/o3m_gen.py` (manifest `"pages": [{"name", "params": [ids]}]` builds the order), `module/sdk/README.md`, tests (`tests/module_loader_test.cpp`: refusals for a duplicate, a gap, an out-of-range id, NULL with 1.10 behaviour; `tests/module_ui_test.cpp`: rows follow the order; `tests/module_gen_test.cpp`). Acceptance: every built-in module and SD example loads and shows as today; a 1.11 example with an order shows its rows in that order. Risk review plus one independent review; `tools/ci.sh qemu`. If it passes 400 lines: A1a (SDK, loader, gen) and A1b (pages).

**C1 — oracle harness** (S, Level B). Files: `third_party/tracker-ce-chip/{ChiptuneSynthVoice.cpp,LICENSE,CMakeLists.txt,PROVENANCE.md}`, `tests/support/cechip_shim/`, `tests/support/cechip_ref.{h,cpp}`, `tests/CMakeLists.txt`, the root `CMakeLists.txt`, `tests/cechip_oracle_smoke_test.cpp` (new), `THIRD_PARTY.md` (row: oracle only). Acceptance: the original compiles unmodified (a ctest checks its SHA-256); each model renders a finite, non-silent note; two runs are identical; a note off ends in silence after Release.

**C2 — parameters, dispatch, pulse family** (S, Level B). Files: `modules/chip/module.json` (Wave labels, ids 20..29 or 20..24 under Q1 b, version, `state_bytes`), `modules/chip/chip_gen.h` (regenerated by `o3m_gen.py`, never by hand), `modules/chip/chip_dsp.h` (the `path` test, the two traps of 3.3, Chord in `control()`, new state), `modules/chip/chip_ce.h` (new: the passes of 3.3 with the pulse kernel for 2A03, Zap and Arp Chord; `ce::control`; the `Std` and `Fast` policies), `modules/chip/chip.cpp` (notice line), `modules/chip/LICENSE-tracker-ce.txt`, `modules/builtin/NOTICES.txt`, `modules/CMakeLists.txt` (VERSION 1.1.0, `O3M_BUILTIN_NOTICES`), `modules/chip/tests/chip_test.cpp`, `tests/cechip_oracle_test.cpp` (new), `tests/module_builtin_run_test.cpp` (the notice check, a note on each new wave audible). Acceptance: G1 unchanged; 1:1 for the pulse kernel at Shape 0/35/100, PWM Depth 0/20/100, PWM Rate 0.1/40, notes 24/60/108 (3.6); Zap's curve by formula; the chord tables equal CE's; a 20-value blob gives today's output; hostile values for ids 20..29 (NaN, +-inf, out of range) are clamped; Chord OFF leaves waves 0..3 bit-exact; `tools/ci.sh qemu`.

**C3 — the post chain** (S, Level B). Files: `chip_ce.h` (sub, decimation, crush, SVF; `raw<W>` for waves 0..3), `chip_dsp.h` (`path` covers the post chain), the two tests. Acceptance: 1:1 on 2A03 for Sub 0/25/100, Decimate 1/2/7/32, Bitcrush 2/8/15.4/15.6/16, Cutoff 80/1000/17499/17500 with Resonance 0/2/100 %, Env Amt -100/0/100 with the envelope at 1; waves 0..3 with only Sub on equal today's wave plus the CE sub mix (a formula test); G1 unchanged at the defaults; the filter stays finite at Resonance 100 % for 10 s.

**C4 — SID, Noise/LFSR, Super Square** (S, Level B). Files: `chip_ce.h`, the two tests. Acceptance: 1:1 per model at Shape 0/49/50/100, PWM Depth 0/100, notes 0/36/60/96/127; an assertion in the host build that the LFSR loop never takes more than 7 steps a sample.

**C5 — FM Bell, Chip Drum** (S, Level B). Files: `chip_ce.h`, the two tests. Acceptance: FM 1:1 at every ratio (Shape in steps of 1/8) and PWM Depth 0/100 with the envelope at 1, plus FM's index following Chip's envelope (a formula test); the Drum 1:1 at Shape 0/50/75/76/100, over the first 180 samples and a 2 s tail; the inline sine's error against `std::sin` at most 2e-6 over [-1000, 1000].

**C6 — cost and deviation** (S, Level B). Files: `chip_ce.h` (`Fast` finished), `modules/chip/README.md` (waves, the new parameters with CE's factors, the table of 3.4, measured SNR of `Fast` against `Std`, hot, `.tmod` and state sizes, the tracker-ce notice), `docs/architecture/cpu-budget.md` (Chip's row), the two tests. Acceptance: 3.6's `Fast` bounds; block-size independence bit for bit (1, 7, 64, 128 frames and random splits) on every wave; the tmod-perf objdump line. In the hot section: no new VDIV or `blx`, today's loops unchanged, growth at most 64 B. The QEMU instruction counts of waves 0..3 within 1 % of G1's. Unit check: `diag mod` (Chip still in the slab with the four built-ins loaded) and `diag cpu`, with a held note on each of the 12 waves and on waves 0..3 with the post chain on, then 8 instances of Super Square. Waves 0..3 at or below the baseline; the figures go into the README and `cpu-budget.md`.

**C7 — pages, views, documents** (S, Level A; B under Q1 a). Files: `modules/chip/module.json` (pages of 3.1), `modules/chip/chip.cpp` (the Wave view: one period of each new wave's kernel at Shape and Sub Osc, named; the Envelope view adds the filter envelope's direction; the pair under 700 words, `module/include/o3os/module/view.h:44`), `docs/manual/module-sounds.md` (a "Chip: tracker-ce's waves" section: what is new, the pages, 3.4 in short, the CPU note, older builds), `docs/manual/modules.md`, `docs/qa/catalogue/modules.md` (QA-MOD-238 onward, the next free at merge: each new wave by ear, the post chain on PULSE, Chord, a 20-value project unchanged, the pages, the views), `platform/mk66/tools/hil/qa_modules.json` (a unit step per new wave), the simulator's module-page golden if it shows Chip (regenerated by the target). Unit check: each new wave by ear, against tracker-ce's CHIP when a CE unit is at hand.

## 5. Assumptions (decided here; each local, testable and reversible)

- A-1 Ids 20..29 in the order of 3.1 (priority order, so that Q1 b keeps a prefix); frozen once released (O4).
- A-2 Defaults: neutral for parameters every wave reads, CE's for those only waves 4..11 read (Shape 35 %, PWM Depth 20 %, PWM Rate 2.5 Hz).
- A-3 Duty and Raw Edges act on waves 0..3 only; Shape and PWM on waves 4..11 only; Sub, the filter, Bitcrush, Decimate and Chord on all twelve.
- A-4 ARP CHORD plays MAJOR while Chord is OFF; any chord replaces Arp 1 and Arp 2.
- A-5 Zap's drop lasts Sweep Time (at least 64 samples, as CE), computed at control points; Chip's own Sweep applies on top.
- A-6 The new waves' level equals CE's voice at Volume 100 % when Volume is 0 dB; no make-up gain.
- A-7 CE's LFSR (reseeded at each note) is a field of its own; Chip's noise LFSR is untouched. Phases restart when the voice was idle, as Chip's do.
- A-8 The new path and all CE code are cold (SDRAM); a kernel moves into the hot part only after C6's unit figure and a passing slab check.
- A-9 The scratch is 2 x 32 floats on the stack; chunks follow Chip's 32-sample control grid.
- A-10 The shim and the wrapper are our code (GPL-3.0-or-later, host tests); only the engine file and its LICENSE are vendored.

## 6. Risks

- R1 **SDRAM code speed** (O5): the new waves may cost two or three times the slab estimate. C6 measures it; A-8 is the way out, and it is not taken without the slab check.
- R2 **Flash** at 94.1 %: +10.6 KB [est] (Q2). If the image cannot take it, Q2 b is the fallback.
- R3 **Bit-exactness of waves 0..3**: any edit of `run<W>`, `control()` or `derive_gain` can move a rounding. G1's constants and the QEMU hash catch it; the two traps of 3.3 are the likely places.
- R4 **State 208 -> 320 B**: fewer Chip instances in the SRAM_U arena; state in SDRAM is slower. `diag mod` in C6 shows where the state sits.
- R5 **Oracle**: 1:1 needs CE's evaluation order at `Std`, and the host must not contract (`-ffp-contract=off` on the oracle library and test files).
- R6 **Licence**: Part 2's conditions travel with every copy (the headers, the LICENSE copy, `kBuiltinNotices`). Chip stays a separate MIT `.tmod` inside the GPL image, as `THIRD_PARTY.md` says of the built-ins.
- R7 **A1 is a published ABI change** (Q1 a): additive and refused cleanly by older OS builds; it needs a risk review.

## 7. Evidence labels

- Waves 0..3 unchanged: **verified** by G1 (host and QEMU).
- The new waves' kernels, PWM and duty, sub oscillator, decimation, crush, SVF, chord tables, Zap curve and LFSR: **verified parity** with tracker-ce@7fd6c2c through the oracle at `Std` (C2 to C5).
- `Fast` (polynomial sine, drum recurrence, host maths at control points): **intended deviation**, measured (C6).
- The paths of 3.4, the post chain on waves 0..3, Chord on every wave, the pages, the views: **new Community OS behaviour**.
- CE's velocity macro and output stage: **open research question**.
- Cycles, sizes and points marked [est]: estimates until C6's unit check.

## 8. State for implementer

- **G1.** Start with: `modules/chip/tests/chip_test.cpp:1-30` (the `Instance` harness); `tests/o3m_test_support.h` (render, blob); `tests/module_builtin_run_test.cpp:104-152` (a note on QEMU, icount) and `:252` (the TB-303's audio golden). Rules: record on `dev` before C2; hash the output bytes, not the parameters; never update a constant afterwards.
- **A1.** Start with: `module/sdk/o3module.h:712-726` (pages), `:756-782` (descriptor and its size per minor); `module/src/image.cpp:90-110` (page checks); `app/src/module_ui.cpp:744`; `tools/o3m_gen.py` (pages). Rules: Level C; additive only; NULL means today's behaviour; risk review plus one independent review.
- **C1.** Start with: `third_party/open303/CMakeLists.txt:11-13`; `tests/CMakeLists.txt:203-230`; tracker-ce's `ChiptuneSynthVoice.cpp:1-16`, `:123-170`, `:478-495`, and `SynthVoiceSDK.h:202-320`. Rules: the vendored file stays byte for byte; the shim copies no SDK code; host only.
- **C2.** Start with: `modules/chip/chip_dsp.h:175-178` (`derive_gain`), `:180-238` (`voice_set_param`), `:138-173` (`control`), `:364-393` (`voice_render`); `modules/chip/module.json:9`; `modules/CMakeLists.txt:172-174`, `:202-203`; tracker-ce `ChiptuneSynthVoice.cpp:270-331`. Rules: G1 stays green at every commit; the new path is `noinline` and outside `O3M_HOT`; `set_param` clamps (NaN included) and never does heavy work; every file with ported code carries the notice of 3.7.
- **C3, C4, C5.** Start with: tracker-ce `ChiptuneSynthVoice.cpp:332-476`; `chip_ce.h` from C2; `.claude/skills/dsp-port/SKILL.md` (oracle tolerances). Rules: `Std` evaluates CE's expressions in CE's order; bounded loops (the LFSR's 7); no allocation, no host maths per sample.
- **C6.** Start with: `.claude/skills/tmod-perf/SKILL.md:36-60`; `modules/sid/README.md:116-123`; `docs/architecture/cpu-budget.md:59`. Rules: the README's numbers are measured; today's loops unchanged in the disassembly.
- **C7.** Start with: `modules/chip/chip.cpp:48-170` (views); `docs/manual/module-sounds.md` (the Chip and SID sections); `docs/qa/catalogue/modules.md` (last row). Rules: goldens regenerated by the target, never by hand; the manual leads with what is new for an owner of the official firmware.
- Report per slice: image flash, the `chip.tmod`, hot and state sizes; the scoped tests (`ctest --test-dir build-os-host -R 'o3m_chip|cechip|module_builtin'`); `tools/ci.sh qemu` from G1 on (the module's bytes change); one full `tools/ci.sh host` per branch; the unit checks of C6 and C7.
- Follow-ups to record, not build: a hot kernel for a new wave if C6 asks for it (A-8); PWM around Duty on PULSE (OS-SNDMOD H-a); a pitch-bend path in a later module ABI; presets when OS-INSFX-ABI2 and OS-MODPRESET land.
