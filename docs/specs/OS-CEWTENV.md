# OS-CEWTENV — Wavetable envelopes and LFOs (WT Env, WT LFO) and the step FX w, y, z

Status: specified 2026-11-05, decisions taken the same day (Q1 a, Q2 a, Q3 a, Q4 a, Q5 a), not implemented · Level C (an INST minor and OG instrument bytes, the voices' modulators in the render, three step FX in the sequencer) · Backlog row OS-CEWTENV, approved by the maintainer 2026-11-05 ("Spec first; CPU measured on the unit. Depends on OS-CEPAT and OS-CEFXIDS") · Facts from `dev` at c1dc3125 and from `tracker-ce@7fd6c2c` (Polyend's published firmware source, cited as facts; no code taken until Q1 is answered) · Size M-L: 11 S slices, about +13 KiB flash [est] in total.

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a, Q4 a, Q5 a (every recommended default). Q1 a approves tracker-ce's `mtWavetableEnvelopes` (the 256 procedural contours, the warp law, the `.wav` contour loader) as an MIT component; its notice goes into `THIRD_PARTY.md` with slice W2.**

Only what the maintainer must decide. Every technical detail is decided in this spec and listed under Assumptions (section 6).

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | The contour bank and its maths | a) port tracker-ce's `mtWavetableEnvelopes` (one MIT component: the 256 procedural contours, the warp law, the `.wav` contour loader; its notice in `THIRD_PARTY.md`) · b) clean-room from the documented behaviour (our own 256 contours) · c) as a, but the bank is made on the host into a 64 KiB data file on the card (about 2.5 KiB less flash; a card without the file plays WT slots flat) | a | The point of the item is that a tracker-ce instrument sounds the same: only the same contours and warp law give that. The component is self-contained (382 lines, no other tracker-ce code). b breaks parity; c makes sound depend on a card file |
| Q2 | Where user contours live | a) `/Envelopes/` at the card root with tracker-ce's names (`001.wav`..`128.wav`, 0-based when `000.wav` exists) · b) a folder of our own | a | One card works with both firmwares; the user guide of tracker-ce applies as is |
| Q3 | CPU policy | a) ship with warps evaluated in the render, measure (section 3.10); if the worst arm costs more than 3 points of the block above the same voices on ADSR/LFO, a follow-up slice bakes the warps of a slot into its contour at bind time and moves non-amp WT destinations to once a block · b) cap the voices that may run a WT modulator · c) bake warps at bind from the start (the step FX w/y warp offset then re-bakes in the main loop, one step late) | a | Typical use (one or two WT slots on a few voices) is about 1 % [est]; the governor (OS-GOV) already protects the worst case; b is a rule users cannot see |
| Q4 | Export (the official 1.9 target) of a WT slot | a) written as an official slot: loop 0 and enable 0 (off), shape held to 0..4, bytes +2/+3 zero; counted in a new Export line · b) verbatim | a | Official 1.9 has no WT modes; what it does with loop 2/3 and a shape above 4 is unknown (it may index past its LFO tables). The same rule as OS-CEPAT Q4. The source folder is never written, so a tracker-ce user keeps the original files |
| Q5 | Order | a) the instrument side first (W1 to W5, W7a), the step FX (W6a, W6b) only after OS-CEPAT S0 (OS-CEFXIDS) and S1 merge, W7b and W8 last (they can wait if flash is short) · b) everything after OS-CEPAT S1 | a | v9 instruments with WT slots already load today (F16), so W1 to W5 fix real files now; the step FX exist only in v6 patterns |

## 1. Goal

An instrument slot (amp, pan, cutoff, wavetable position, granular position, finetune, and the new granular length) can be a **WT Env** (a 128-point contour played over a duration, one-shot or with a sustain point, with time and level warps) or a **WT LFO** (the same contour looped at the LFO's tempo-synced rate), as in tracker-ce; the step FX w (WT Amp Env), y (WT Filter Env) and z (Granular Length) play as in tracker-ce; files keep every byte tracker-ce stores.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | A slot's loop byte: 0 ADSR, 1 LFO, 2 WT Env, 3 WT LFO. The LFO entry (8 B): shape, speed, `wtBend` (warp X high nibble, warp Y low nibble), `wtSust` (0 one-shot, 1..99 freeze %), amount f32. In a WT slot, shape is the contour 0..255 (F001..F128, then U001..U128). Instrument version stays 9 | `tracker-ce@7fd6c2c:core/source/mtEnvelopeGenerator.h:24-44`; `core/source/mtStructs.h:629-637`, `:43-45` | verified (tracker-ce source) |
| F2 | Seven destinations; granular length shares slot 3 with wavetable position; slot 3's enable byte is a bit mask, 0x01 wavetable position, 0x02 granular length, chosen by the play mode | `core/source/mtStructs.h:415-458`, `:673-702` | verified (tracker-ce source) |
| F3 | The bank: 256 contours of 128 unipolar int16 in external RAM (64 KiB); 0..127 factory and 128..255 user fallbacks, made procedurally at boot; `/Envelopes/NNN.wav` (or `N.wav`) replaces user slots, 1-based, 0-based when `000.wav` or `0.wav` exists; PCM 8/16/24-bit, mono or the left channel, any length resampled to 128 points, a file with negative samples mapped from -1..1 to 0..1 | `core/source/mtWavetableEnvelopes.cpp:46-210`, `:211-309`; `core/source/Tracker.cpp:100`; `core/interface/instrumentEditor/gInstrEdit.cpp:1229`; `docs/wavetable_envelopes.md:100-175` | verified (tracker-ce source) |
| F4 | Evaluation: a warp nibble 0..7 is +0..+7, 9..15 is -7..-1, 8 is 0; warp X bends the phase by p(1 + k) / (2pk + 1 - k) with k = wx / 9 (one division); 128-point linear interpolation; warp Y the same law on the level (a second division); -7 on Y inverts; a Q30 fixed-point twin serves the amp | `core/source/mtWavetableEnvelopes.cpp:21-44`, `:311-382` | verified (tracker-ce source) |
| F5 | WT Env on a non-amp slot: duration = the slot's decay in ms (1 or less: 1,000 ms), release = its release in ms (below 50: 50 ms); output y x amount. One-shot: plays to its end whatever the key does, and starts again while the key is still held at the end. Sustained: runs to the sustain %, holds while the key is held; the release runs from where it is: phase = start + r (1 - start), output x (1 - r) | `core/source/mtEnvelopeGenerator.cpp:87-176` | verified (tracker-ce source) |
| F6 | WT LFO on a non-amp slot: tempo-synced, period 576 x rate ticks, phase shifted by the note's start step x 576 (the LFO's clock); output (2y - 1) x amount; amount and speed are the LFO entry's | `core/source/mtEnvelopeGenerator.cpp:177-207`; `core/AudioEngine/mtAudioEngine.cpp:2539-2570`; `docs/wavetable_envelopes.md:48-55` | verified (tracker-ce source) |
| F7 | Amp slot: WT Env multiplies the voice by y (no amount), one-shot at its end stops the voice, sustain and release as F5; WT LFO multiplies by 1 - (1 - y) x depth (the LFO amount) | `core/libs/Audio/effect_envelope.cpp:472-495`, `:520-560`, `:560-602`; `core/AudioEngine/mtAudioNoteOnHandle.cpp:55-66` | verified (tracker-ce source) |
| F8 | Step FX: 43 Amp Env (w), 44 Filter Env (y), 45 Granular Length (z); w and y take 00..FF (nibbles x, y), z 0..100 | `core/source/mtSequencer.h:122-186`; `core/source/mtSequencer_patternEdit.cpp:1786-1787`, `:1974-1976` | verified (tracker-ce source) |
| F9 | w: ADSR amp slot: attack T[x], decay = release T[y], sustain 0 when y <= 6, T = {0, 5, 12, 25, 45, 80, 140, 240, 400, 650, 1000, 1500, 2200, 3300, 5000, 8000} ms; LFO slot: no effect; WT slot (loop 2 or 3): duration x (0.05 + 0.95 x / 8) for x < 8, x (1 + 3 (x - 8) / 7) for x > 8, at least 10 ms, and warp X + (y - 8) held to -7..7; when the FX ends the instrument's values return. The tracker-ce guide describes the ADSR case differently (`docs/wavetable_envelopes.md:92-94`): the code is the fact | `core/AudioEngine/mtAudioSequencer.cpp:641-720` | verified (tracker-ce source) |
| F10 | y: ADSR cutoff slot: attack 8x ms (at least 1 ms), decay = release 8y ms, sustain 0 when y <= 6 (through the envelope's MIDI parameters); it also stores T[x] and T[y] as the step's values: which of the two the following notes hear is an open research question; WT slot as w; the end restores | `core/AudioEngine/mtAudioSequencer.cpp:723-790`; `core/source/mtEnvelopeGenerator.cpp:668-698` | verified (tracker-ce source), one open point |
| F11 | z: granular instruments only; 0..50 maps to 44..4,410 samples and 50..100 to 4,410..44,100, held to the sample (at least 44); the FX in the step's first slot (fx[0]) wins | `core/AudioEngine/mtAudioSequencer.cpp:548-610`; `core/AudioEngine/mtAudioEngine.h:25-26` | verified (tracker-ce source) |
| F12 | Granular length modulation: length = base (or z's) + mod x (max - 44), max = min(sample length, 44,100), held to 44..max, while the slot's envelope runs; any source kind (ADSR, LFO, WT) | `core/AudioEngine/mtAudioEngine.cpp:2520-2537`; `core/AudioEngine/mtAudioUpdateHandle.cpp:650-675` | verified (tracker-ce source) |
| F13 | tracker-ce's `core/` classes are MIT, with a clause against a wholesale commercial clone of the application; taking one needs the maintainer's approval (approved so far: CHIP) | `tracker-ce@7fd6c2c:LICENSE:1-30`; `CLAUDE.md` (hard rules) | verified |
| F14 | Our modulators: `FilterMod` (kind None/Env/Lfo, shape, rate, amount, the ADSR fields; 2 tail padding bytes); five `ModGen` a voice (cut, pan, tune, trem, pos; 44 B, static_assert); `Voice` 448 B in SRAM_U, 12 voices; cutoff and amp tremolo evaluated every 16 samples (`kFilterCtl`), pan, finetune and position once a block (`mod_block`); the amp ADSR in groups of 8 samples | `audio/include/o3os/audio/voices.h:120-215`, `:294`, `:309-311`, `:414-448`, `:658-681`, `:1116`, `:1172`; `audio/src/voices.cpp:1208`, `:1318`, `:1325-1354`, `:1356`, `:2546` | code fact |
| F15 | Our LFO is tempo-synced with the same clock as F6 (`l_period` ticks, `l_shift` = 576 x (step % 36)); rates: 29 for the other slots, 24 for the amp | `voices.h:675-679`; `model/include/o3os/model/params.h:103-110` | code fact |
| F16 | Today a WT slot of a tracker-ce v9 instrument: the OG reader keeps loop and enable verbatim, reads a shape above 4 as Triangle and drops bytes +2/+3; `make_sample_desc` treats any loop other than 0 as an LFO; so it plays as a Triangle LFO at the slot's speed and amount. A wavetable instrument whose slot 3 enable is 0x02 alone runs position modulation (enable is read as non-zero) | `formats/src/og_instrument.cpp:55-76`; `app/src/og_samples.cpp:256-295` | code fact |
| F17 | Native INST is at minor 1.5 (OS-CEPLAYTHRU plans 1.6); the sampler block stores 6-byte LFO entries; a native shape above 5 reads Triangle; the model `Lfo` has 2 padding bytes | `formats/src/native_reader.cpp:50-56`, `:753-755`; `formats/src/native_codec.h:36-50`; `model/include/o3os/model/instrument.h:75-79` | code fact |
| F18 | Step FX: `kFxInfo` covers 0..62, 63..127 are inert; 23 of 32 `FxGroup`s used; mod group ids equal the groups. OS-CEFXIDS (OS-CEPAT S0, not merged) stores OG 43..74 as model 96..127; until then OG 43, 44, 45 read as our Speed, Tempo fine and Order jump | `seq/include/o3os/seq/fx.h:109-136`, `:148`, `:218`; `audio/include/o3os/audio/events.h:185-198`; `docs/specs/OS-CEPAT.md` (Q1, section 2.4) | code fact |
| F19 | MOD page: per slot SOURCE Off/Envelope/LFO (ids 0x60 + 4e), then the envelope's AMOUNT..RELEASE or the LFO's SHAPE, SPEED, AMOUNT; at most 8 rows a page; virtual fields (no table row, not learnable) exist at 0x1E and 0x1F | `model/include/o3os/model/params.h:79-110`; `app/src/screens/params_screen.cpp:234-248`; `app/include/o3os/app/param_view.h:27` | code fact |

## 3. Design

### 3.1 What must land first

- **OS-CEFXIDS (OS-CEPAT S0) before W6a and W6b.** Without it OG 43, 44, 45 are our core FX 43 Speed, 44 Tempo fine, 45 Order jump (F18); W6 implements model ids 96, 97, 98.
- **OS-CEPAT S1 (the v6 pattern reader) before W6 has anything to play from a real file:** tracker-ce writes w, y and z only into v6 patterns, which fail to load today. W6's tests use synthetic model patterns, so S1 gates the fixture test only.
- **Nothing gates W1 to W5 and W7a:** v9 instruments with WT slots load today (F16).
- **INST minor:** OS-CEPLAYTHRU P1 and W1 each append to INST; the first to merge takes 1.6, the other 1.7 (append-only rule).
- **Q1 answered before W2** (the component's approval). OS-CECHECK (a) (a tracker-ce-saved instrument) confirms the WT bytes on a real file; OS-CECHECK (c) (whether a new wavetable instrument enables the position LFO, ex OS-WTLFO-DEFAULT) is related and not in scope.

### 3.2 Data (W1)

- Model `Lfo` gets `wt_bend` and `wt_sust` in its two padding bytes (struct sizes unchanged). For a slot with loop 2 or 3, `shape` is the contour index 0..255.
- OG read: for loop >= 2, shape verbatim and bytes +2/+3 read; for loop <= 1, today's rule (above 4: Triangle). OG write (Export): Q4 a, a new `ExportDrops::wt_mod` counter; the Sine rule (`lfo_sine`) applies only to loop <= 1.
- Native: INST minor 1.7 (or 1.6, 3.1) appends 12 B, `wt_bend` and `wt_sust` of the six slots, written only when a slot has loop >= 2 or a non-zero byte. Reader: shape verbatim when loop >= 2 in a chunk of that minor; in an older chunk loop >= 2 reads as 1 (an LFO: what those files played, A3). `tools/o3project.py` and `docs/formats/o3-project.md` follow; `docs/formats/og.md` section 5 gets the bytes.

### 3.3 Contour bank and evaluator (W2)

`audio/include/o3os/audio/wt_contour.h`, `audio/src/wt_contour.cpp` (pure; with Q1 a, the port of the component, notice in `THIRD_PARTY.md`): a bank of 256 x 128 int16 in the SDRAM arena (64 KiB) and a pointer table (1 KiB); `wt_make(shape)` makes one contour in the main loop (about 40k cycles [est]; all 256 about 55 ms [est]) and marks it; `wt_eval(shape_ptr, phase, bend)` (float, F4) for every destination. The main loop makes the contour before it stages a descriptor that names it (`make_sample_desc`), so the render never reads an unmade one. The evaluator stays in flash (SRAM_L has 864 B left).

### 3.4 Non-amp modulators (W3a, W3b)

- `FilterMod` gets kinds `kFmodWtEnv` (3) and `kFmodWtLfo` (4), and `wt_bend`, `wt_sust` in its tail padding (`sizeof` unchanged); `make_sample_desc` maps loop 2 and 3 to them, with `shape` the contour.
- `ModGen` runs them without growing (static_assert 44 B): WT Env uses `stage`, `key`, `e_t` (samples into the stage), `l_inv` (1 / duration), `e_max` (the release's start phase), `e_out`; WT LFO uses the LFO's `l_period`, `l_shift`, `l_inv`. Laws F5 and F6 (verified parity).
- W3a: the cutoff (every 16 samples, `filter_run`); W3b: pan, finetune and wavetable position (once a block, `mod_block`), and slot 3 honours bit 0x01 for the position (F2: fixes F16's second case).

### 3.5 Amp (W4)

Loop 2 replaces the ADSR (`AmpEnvelope` is not run) and loop 3 the tremolo: both drive the tremolo path's multiplier, evaluated at every 16th sample and walked linearly (`trem_span`), with `mg[kMgTrem]` as the state; laws F7. A one-shot WT Env at its end releases the voice with the usual declick. Control rate: ours every 16 samples walked linearly, tracker-ce stepwise per envelope region: new Community OS behaviour (expected inaudible; listened for in the unit check).

### 3.6 Granular length (W5)

On a granular instrument, slot 3 with bit 0x02 drives the grain length by F12, from any source kind. The state is a sixth `ModGen` per voice kept in an SDRAM array (`O3OS_SDRAM_NOINIT`, 12 x 44 B): it is read once a block, so SRAM_U does not grow (A5). The new length applies at the next grain. Export keeps bit 0x01 only and counts a dropped 0x02 in `wt_mod`.

### 3.7 Step FX (W6a, W6b)

- A second FX info table for model 96..98 (`fx_kind` answers it; 99..127 stay inert); groups `AmpEnvFx`, `FilterEnvFx`, `GranLenFx` appended (26 of 32); voice events through the mod group ids.
- w (96) and y (97): F9 and F10. For y's ADSR case we take the envelope's values (8x, 8y ms; A7). The override lasts while the FX is on the track's rows and is undone at the first row without it (the FX group's existing end rule).
- z (98): F11; fx[0] wins.
- W6b adds the picker entries, the grid glyphs `w`, `y`, `z`, and `fx_text` words (`AMP ENV x8 WARP +0`, `FLT ENV ...`, `GRAIN 100 MS`).

### 3.8 UI (W7a, W7b)

- SOURCE gains WT ENV (3, loop 2) and WT LFO (4, loop 3) (`model/include/o3os/model/params.h:105-107`).
- New virtual fields `0x90 + 5e + f` (`f`: WAVE, DURATION, WARP X, WARP Y, SUSTAIN; 0x90..0xAD), read from and written to the instrument through an action (one undo record, the slot rebinds), not learnable, not in the app SDK.
- Rows: WT ENV: SOURCE, WAVE (F001..U128), DURATION (0..20.000 s, stored in the slot's decay), WARP X, WARP Y (-7..+7), SUSTAIN (OFF, 1..99 %), RELEASE (the existing row), AMOUNT (the existing row): 8 rows. WT LFO: SOURCE, WAVE, SPEED, WARP X, WARP Y, AMOUNT (the LFO's rows): 6 rows.
- W7b: on a granular instrument slot 3 is the GRAN LENGTH destination (bit 0x02), and the MOD page draws the selected contour with its warps (64 points, one FT812 line strip; new Community OS behaviour, the first thing to drop if flash is short).

### 3.9 User contours (W8)

The main loop loads `/Envelopes/` after the card mounts and on a project load (F3 rules; through our WAV reader), resamples each file to 128 points, writes it to a spare buffer and swaps the slot's pointer with one aligned 32-bit store (the render reads the pointer once per evaluation). A load report goes to the strip. Never in the render.

### 3.10 CPU estimate [est] and what the unit measures

- **One evaluation:** about 40 cycles without warps (two int16 reads from SDRAM through the cache, a lerp), about 80-95 with both warps (two `VDIV.F32` of 14 cycles each, a few multiplies). An uncached SDRAM line costs more: the bench says.
- **Worst voice** (every destination WT, both warps):
  - amp: 4 evaluations a block plus 4 segment divisions;
  - cutoff: 4 evaluations;
  - pan, finetune, position or grain length: 1 each;
  - about 1,240 cycles a block. 12 voices: about 15k cycles, 5.7 % of a block, about +3 points over the ADSR/LFO it replaces.
- **Typical** (amp and cutoff WT Env, no warps, 6 voices): about 2.3k cycles, under 1 %.
- **No WT slot anywhere: 0.** The kinds are a branch the ADSR and LFO paths already take.
- **Unit measurements** (W4 and W5 are not done before 1 to 4):
  1. `diag cpu` mean and worst on Dense8 in three arms: no WT (must equal the baseline image within noise); 6 voices with amp and cutoff WT Env, no warps; 12 voices with every destination WT and both warps;
  2. a DWT bench line for one `wt_eval` (no warp, both warps; warm and cold);
  3. `wt_make` time per contour and for all 256 (console);
  4. the SDRAM arena's free bytes before and after (-65 KiB);
  5. 0 underruns over 10 minutes in the worst arm, with OS-GOV on.

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| W1 | Data: model bytes, OG read/write (Q4), INST minor, Python mirror | S | C | +0.4 KiB | — |
| W2 | Contour bank and evaluator (pure) | S | B | +3 KiB | Q1 |
| W3a | WT Env/LFO kinds, `ModGen` machines, cutoff | S | C | +1.2 KiB | W1, W2 |
| W3b | Pan, finetune, wavetable position; slot 3 bit 0x01 | S | C | +0.6 KiB | W3a |
| W4 | Amp WT Env and WT LFO | S | C | +1 KiB | W3a |
| W5 | Granular length destination (slot 3 bit 0x02) | S | C | +0.6 KiB | W3b |
| W6a | Step FX w and y (96, 97) | S | C | +1.2 KiB | W4, OS-CEPAT S0 |
| W6b | Step FX z (98), picker, glyphs, texts | S | B | +0.8 KiB | W5, W6a |
| W7a | MOD page: WT ENV, WT LFO rows | S | B | +1.5 KiB | W3a |
| W7b | GRAN LENGTH on the MOD page, contour drawing | S | B | +1 KiB | W5, W7a |
| W8 | `/Envelopes/*.wav` user contours | S | B | +1.5 KiB | W2 |

Totals [est]: flash about +12.8 KiB, SRAM about +64 B (the bank's pointer and marks; voices unchanged), SDRAM +65 KiB plus 528 B (W5), render CPU per 3.10.

**W1** (S, Level C). Files: `model/include/o3os/model/instrument.h`, `formats/src/og_instrument.cpp`, `formats/include/o3os/formats/og.h` (`wt_mod`), `app/src/og_export.cpp` (the Export line), `formats/src/native_codec.h`, `formats/src/native_writer.cpp`, `formats/src/native_reader.cpp`, `tools/o3project.py`, `docs/formats/og.md`, `docs/formats/o3-project.md`. Tests: `og_instrument_test` (a synthetic v9 file with loop 2 shape 37 bend 0x9C sust 40 reads back exactly; loop 3; loop 0 shape 7 still Triangle; Export writes the slot off and counts it; an official fixture is byte-identical); `native_format_test` (a WT instrument writes the new minor and round-trips; one without WT stays at its older minor byte for byte; an older chunk with loop 2 reads loop 1; an older reader skips the 12 B); `tests/o3project_roundtrip.py`.

**W2** (S, Level B). Files: `audio/include/o3os/audio/wt_contour.h`, `audio/src/wt_contour.cpp`, the audio library's CMake list, `platform/mk66/include/o3os/platform/mk66/mem_map.h` (the arena), `THIRD_PARTY.md`, `tests/audio_wt_contour_test.cpp`. Tests: a CRC-32 of each of the 256 contours and 64 spot evaluations (warps -7, -3, 0, +4, +7 on X and Y) against a golden header made once on the host from tracker-ce's file (the procedure in the test's header); nibble pack/unpack; phase 0 and 1 exact; no NaN for any input.

**W3a** (S, Level C). Files: `audio/include/o3os/audio/voices.h`, `audio/src/voices.cpp` (`fenv_eval`/`flfo_eval` dispatch, `filter_run`), `app/src/og_samples.cpp` (`put_mod`). Tests: `audio_voices_test` (a cutoff WT Env one-shot traces the contour x amount at the 16-sample points within 1e-4; repeats while held; sustain freezes; release law; WT LFO phase equals the LFO's for the same rate and start step; output bipolar).

**W3b** (S, Level C). Files: `audio/src/voices.cpp` (`mod_block`), `app/src/og_samples.cpp`. Tests: pan and finetune WT once a block; slot 3 enable 0x02 alone on a wavetable instrument runs no position modulation; 0x01 does.

**W4** (S, Level C). Files: `audio/include/o3os/audio/voices.h`, `audio/src/voices.cpp` (`mods_start`, `trem_span` targets, the envelope gate), `app/src/og_samples.cpp`. Tests: amp WT Env one-shot ends the voice at the duration (plus the declick); sustained holds and releases by F5; WT LFO multiplier 1 - (1 - y) x depth; an ADSR instrument's output is bit-identical to today (`audio_voices_test`, the render goldens unchanged).

**W5** (S, Level C). Files: `audio/src/voices.cpp` (the granular voice's length), `audio/include/o3os/audio/voices.h`, `app/src/og_samples.cpp`, `formats/src/og_instrument.cpp` (Export bit 0x02). Tests: `audio_granular_test` (length = base + mod x (max - 44), held; applied at the next grain; a wavetable instrument ignores 0x02).

**W6a** (S, Level C). Files: `seq/include/o3os/seq/fx.h`, `seq/src/sequencer.cpp`, `audio/include/o3os/audio/events.h`, `audio/src/voices.cpp`, `model/include/o3os/model/fx_table.h` (rows 96, 97: 0..255). Tests: `seq_pfx_test` (w with x 0..F on an ADSR slot sets T; y <= 6 zero sustain; WT duration scale at x 0, 7, 8, 9, F and the 10 ms floor; warp offset clamp; LFO slot unchanged; the next row without it restores); `seq_determinism_test` unchanged for projects without 96..98.

**W6b** (S, Level B). Files: `seq/src/sequencer.cpp` (z), `model/include/o3os/model/fx_table.h` (picker), the grid glyph table, `app/include/o3os/app/fx_text.h`, `docs/qa/catalogue/pattern.md`, `docs/manual/fx.md`, `sim/scenarios/qa/qa_pattern.txt`. Tests: z mapping at 0, 50, 100 and a short sample; fx[0] wins; non-granular ignores; `pattern_edit_rules_test` (picker order, ranges). QA rows QA-PAT-340..344.

**W7a** (S, Level B). Files: `model/include/o3os/model/params.h`, `model/src/params.cpp` (descriptors), `app/src/module_ui.cpp`, `app/src/param_actions.cpp`, `app/src/screens/params_screen.cpp`, `docs/qa/catalogue/instrument.md`, `docs/manual/instrument.md`, `sim/scenarios/qa/qa_instrument.txt`. Tests: `instrument_mod_test` (SOURCE cycles Off, ENV, LFO, WT ENV, WT LFO; the rows of each; DURATION writes decay; warps pack into the byte; one undo record; rebind). QA rows QA-INS-290..299.

**W7b** (S, Level B). Files: `app/src/screens/params_screen.cpp`, `app/src/og_samples.cpp`, `docs/qa/catalogue/instrument.md`, `docs/manual/instrument.md`. Tests: `instrument_mod_test` (slot 3 on a granular instrument is GRAN LENGTH and writes bit 0x02; on a wavetable 0x01); a sim golden of the contour drawing. QA rows QA-INS-300..303.

**W8** (S, Level B). Files: `app/src/wt_user.cpp` (new), `app/include/o3os/app/wt_user.h`, the main loop's card-mount hook, `docs/manual/instrument.md` (the folder and file rules). Tests: `wt_user_test` (8/16/24-bit, stereo takes left, 128 and 3,000 frames resampled, a bipolar file mapped to 0..1, 0-based naming, a missing number keeps the fallback, a float WAV refused and reported). QA rows QA-INS-304..306.

## 5. Unit checks

- After W4: the measurements of 3.10; a listen of a tracker-ce-style instrument (amp WT Env F001 one-shot, cutoff WT LFO) against the same instrument on ADSR/LFO.
- After W6a: a pattern with w and y on an ADSR and on a WT instrument, by ear.
- After W8: a card with `/Envelopes/001.wav`: U001 plays it after a reboot.

## 6. Assumptions (decided here; local, testable, reversible)

- A1. Contour indexes 0..127 are F001..F128 and 128..255 are U001..U128 (F1).
- A2. WT LFO uses our tempo-synced rate tables (29 rates, the amp's 24), which match tracker-ce's speeds (F6, F15).
- A3. An INST chunk older than the WT minor with loop 2 or 3 reads as loop 1: those bytes came from a tracker-ce file through an older build, which had already lost the contour and warps, and played them as an LFO.
- A4. The slot's RELEASE row is shown for WT ENV (tracker-ce uses the stored release but shows no row): new Community OS behaviour.
- A5. The granular length state lives in SDRAM (read once a block); the five SRAM `ModGen`s stay.
- A6. Live edits of the WT rows rebind the slot at the next quiet moment (as STEREO); a live ParamChange for them (OS-LFOLIVE style) is a follow-up.
- A7. y on an ADSR cutoff slot uses 8x / 8y ms (what the envelope receives in tracker-ce); the table values it also stores are not used (F10's open point).
- A8. The slot 3 enable byte's bits change meaning only for bytes other than 0 and 1, which only tracker-ce files carry; files of official 1.9 and of this OS read as before.

## 7. Risks

- R1 Flash: about +12.8 KiB of the room left (FLASH 94.14 % at v1.1.0); W7b's drawing and W8 can wait (Q5).
- R2 CPU above the estimate if the SDRAM bank is uncached in the render: the bench (3.10 item 2) decides; the fallback copies the contours in use into a small SRAM_U cache (follow-up).
- R3 A contour replaced while a voice reads it: the pointer swap of 3.9 and a test.
- R4 The amp WT Env replaces the ADSR path: the render goldens must be bit-identical for every instrument without WT.
- R5 Golden data from tracker-ce's generator: allowed only under Q1 a, with the notice.

## 8. Evidence labels

- WT Env and WT LFO laws, the warp law, the contours (with Q1 a), the slot 3 bits, the step FX w, y, z, granular length modulation: **verified parity** (F1 to F12).
- y's ADSR values (A7), what official 1.9 does with WT bytes (Q4): **open research question**.
- The control rates, the RELEASE row, the contour drawing, the Export rule, the INST minor: **new Community OS behaviour**.
- CPU, flash and timing numbers marked [est]: estimates; the unit measurements replace them.

## 9. State for implementer

**W1.** Start with: `Lfo` `model/include/o3os/model/instrument.h:75`; the LFO read `formats/src/og_instrument.cpp:70-75` and write `:364-371`; the shape clamp `formats/src/native_reader.cpp:753`; `inst_version` `formats/src/native_writer.cpp:44`. Rules: every file without WT bytes is byte-identical on write; struct sizes unchanged; Level C review; `docs/formats` updated in the same commit.

**W2.** Start with: the SDRAM regions in `platform/mk66/include/o3os/platform/mk66/mem_map.h:122` and `app/include/o3os/app/sample_bank.h` (`SampleBankMem`); `audio/CLAUDE.md` (scoped tests); the tracker-ce file (read only, under Q1 a). Rules: no double; no allocation; the bank is made in the main loop only; flash placement (no SRAM_L).

**W3a.** Start with: `FilterMod` `audio/include/o3os/audio/voices.h:432`; `ModGen` `:664`; `VoicePool::filter_run` `audio/src/voices.cpp:1325`; `put_mod` `app/src/og_samples.cpp:284`. Rules: `sizeof(ModGen) == 44`, `sizeof(Voice) == 448`; ADSR and LFO outputs bit-identical; no division in the sample loop.

**W3b.** Start with: `VoicePool::mod_block` `audio/src/voices.cpp:2546`; `pos_mod` `audio/include/o3os/audio/voices.h:533`. Rules: as W3a; a non-wavetable instrument never runs slot 3.

**W4.** Start with: `trem_span` `audio/src/voices.cpp:1356`; `mods_start` `:1961`; `AmpEnvelope` `voices.h:414`. Rules: one division per 16-sample segment at most; the declick at a one-shot's end; render goldens unchanged without WT. Unit check: 3.10 items 1 to 5.

**W5.** Start with: the grain geometry `audio/src/voices.cpp:688`; `O3OS_SDRAM_NOINIT` users (`grep`). Rules: the length changes only at a grain wrap; 44..max.

**W6a.** Start with: `FxGroup` `seq/include/o3os/seq/fx.h:109`; `fx_kind` `:218`; the Filter LFO FX 35 path (`kFilterLfo`, `fx.h:70`) as the model of a group with an end; `kModGroupLfoFilter` `audio/include/o3os/audio/events.h:191`. Rules: `kFxGroupCount <= 32`; types 99..127 stay inert; determinism hashes unchanged without 96..98.

**W6b.** Start with: `kRows` `model/include/o3os/model/fx_table.h:48`; `append_fx_value` `app/include/o3os/app/fx_text.h:56`. Rules: picker order after the core FX; texts fit the info line.

**W7a.** Start with: `ModSource` `model/include/o3os/model/params.h:98`; the MOD rows `app/src/screens/params_screen.cpp:234`; `write_play_stereo` `app/src/param_actions.cpp:493` (the virtual-field action to copy). Rules: at most 8 rows; virtual fields not learnable and not in the app SDK.

**W7b.** Start with: the MOD page's dim rule `app/src/screens/params_screen.cpp:1076`. Rules: the drawing uses one display list command block; drop it before anything else if the flash budget says so.

**W8.** Start with: `formats/src/wav_sample.cpp` (the WAV reader); the card-mount hook in `platform/mk66/src/app_main.cpp`. Rules: main loop only; a bad file never stops the others; the pointer swap of 3.9.
