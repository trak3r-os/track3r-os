# Chip (module `chip`)

A chip-tune instrument for the Community OS, loaded from the SD card as `/modules/instruments/chip.tmod` (ADR 0002, OS-012c). **Written from scratch**, plus a port of tracker-ce's CHIP engine (waves 4..11, `chip_ce.h`; MIT with the notice of `LICENSE-tracker-ce.txt`: `THIRD_PARTY.md`), against `module/sdk/o3module.h`, MIT like the SDK. The port came in by slices (OS-CECHIP, `docs/specs/OS-CECHIP.md`): C2 the parameters 20..29 and the pulse family (2A03 PULSE, ZAP/LASER, ARP CHORD), C3 the post chain, C4 SID SAW/TRI, NOISE/LFSR and SUPER SQR, C5 FM BELL and CHIP DRUM (each held to the original by `tests/cechip_oracle_test.cpp` at policy Std). The cost of the new waves on the unit is C6's. Status: new Community OS behaviour; tested on the host through the ABI (`modules/chip/tests/chip_test.cpp`); run on the unit (measured 2.8 % of a block per sounding voice on 0.4.9, `docs/architecture/cpu-budget.md`; the HIL scenarios bind and sound it). By-ear checks of each wave remain open.

## What it is

One mono voice an instance (the ABI runs one instance per instrument slot: play chords on several tracks).

- **Wave**: pulse with a **Duty** of 12.5 / 25 / 50 / 75 %, triangle, noise from a 15-bit LFSR, long (period 32767) or short (period 93, the metallic one). The pulse is band-limited with PolyBLEP; **Raw Edges** turns that
  off for the aliasing of a real chip. The pulse has its mean removed (a narrow pulse peaks at 1.75, hence the -12 dB default Volume); noise is scaled by 0.7. Noise is a sample-and-hold of the LFSR clocked at 16 x the note's
  frequency (never faster than the sample rate), so it follows the pitch.
- **Vol Steps**: the envelope's level is quantised to sixteen 4-bit steps (the staircase of a hardware volume register); off, the level is smooth.
- **Envelope**: Attack, Decay (the time of the fall from full scale to the sustain level), Sustain, Release, linear, per sample (no 60 Hz zipper). Attack and Release never go below 1 ms (a step would click); a new note over a
  sounding one starts from the level the old one is at. A sustain of 0 makes the note end by itself after the decay (a percussive sound), and the voice then says it is idle.
- **Arp 1 / Arp 2 / Arp Speed**: the pitch steps through 0, Arp 1, Arp 2 semitones at Arp Speed steps a second (two steps when Arp 2 is 0, none when both are 0).
- **Sweep / Sweep Time**: the note starts Sweep semitones away and glides to its pitch over Sweep Time (linearly in semitones; 0 ms turns it off): a kick, a laser.
- **Vib Depth / Vib Rate / Vib Delay**: a triangle vibrato of +-Depth cents at Rate Hz, fading in over Delay.
- Pitch (arpeggio, sweep, vibrato) updates every 32 samples; a live legato key retriggers like any other note, but a note the host ramps over the one that sounds (the Glide and Slide FX of a pattern, ABI 1.8)
  keeps the envelope, the arpeggio, the sweep and the vibrato and only moves the pitch. A note off before the first sample of a note ends the voice at once.

## Views

Two views on the Instrument page, drawn from the parameter values the page hands over: **Wave** (waves 0..3: two periods of the pulse with its duty, the triangle, or 48 steps of the noise LFSR, named; waves 4..11, OS-CECHIP C7: 96 samples of the wave's own kernel at a fixed pitch (two periods of the pulse, SID, noise, bell and super square), at the page's Shape, Sub Osc and PWM Depth, with the wave's name above it: ZAP/LASER shows its pitch drop, ARP CHORD its four chord steps, CHIP DRUM a hit with its time stretched 30 times; the kernels run at policy Fast with a local `semis_ratio` polynomial, because the view's canvas has no host maths) and **Envelope** (the level against
time for the key held and released, as the parameters say, in 4-bit steps when Vol Steps is on; with Env Amt not 0 and a canvas of 160 pixels or more, FILTER OPENS or FILTER CLOSES says which way the envelope moves the cutoff). About 35 to 120 (the pulse to the noise) and 340 estimated display-list words (the pair fits the page's 700 with room for the page: the test holds 500).

## Parameters (a preset is thirty little-endian float32 values in this order, ids 20..29 appended by OS-CECHIP (a preset of the first twenty loads with them at their defaults); four pages under ABI 1.8, which keeps ids in order and 8 rows a page, so they are interim: TONE 0-7, ENV 8-13 (Attack .. Release, Sweep, Sweep Time), PITCH 14-21 (Vib Depth, Vib Rate, Vib Delay, Arp 1, Arp 2, Arp Speed, Shape, Sub Osc), OSC/FX 22-29; the spec's pages need the display order of A1, slice C7)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Wave | 0..11 | 0 | PULSE, TRIANGLE, NOISE LONG, NOISE SHORT, then tracker-ce's 2A03 PULSE, SID SAW/TRI, NOISE/LFSR, FM BELL, SUPER SQR, ZAP/LASER, ARP CHORD, CHIP DRUM (labelled; learnable) |
| 1 | Duty | 0..3 | 1 | 12.5%, 25%, 50%, 75% (labelled; learnable) |
| 2 | Raw Edges | 0 / 1 | 0 | |
| 3 | Vol Steps | 0 / 1 | 0 | |
| 4 | Volume | -48..+6 dB | -12 | modulation target |
| 5 | Vel Sens | 0..100 % | 50 | 1 - sens x (1 - velocity / 127) |
| 6 | Transpose | -24..24 semitones | 0 | |
| 7 | Fine | -100..100 cents | 0 | |
| 8 | Attack | 0..500 ms | 0 | at least 1 ms |
| 9 | Decay | 0..2000 ms | 250 | |
| 10 | Sustain | 0..100 % | 60 | |
| 11 | Release | 0..2000 ms | 80 | at least 1 ms |
| 12 | Sweep | -48..48 semitones | 0 | modulation target |
| 13 | Sweep Time | 0..2000 ms | 120 | |
| 14 | Vib Depth | 0..200 cents | 0 | modulation target |
| 15 | Vib Rate | 1..16 Hz (log) | 6 | |
| 16 | Vib Delay | 0..1000 ms | 0 | |
| 17 | Arp 1 | -24..24 semitones | 0 | |
| 18 | Arp 2 | -24..24 semitones | 0 | |
| 19 | Arp Speed | 1..120 Hz (log) | 50 | |
| 20 | Shape | 0..100 % | 35 | waves 4..11: the pulse's duty base (0.125 + 0.75 x shape), Zap's drop depth; SID's and SUPER SQR's duty (the blend, the pulses' width), SUPER SQR's detune (0.2 % to 2 %), NOISE/LFSR's sequence (50 % and up is the short one), FM BELL's modulator ratio (1, 2, 3, 3.5, 4, 5, 7, 9 at Shape x 7.99), CHIP DRUM's pitch drop and noise (kick, snare, hat; 75 % and up the short sequence); tracker-ce's value is /100 |
| 21 | Sub Osc | 0..100 % | 0 | every wave: tracker-ce's 4-bit stepped triangle an octave down, mixed in above 0.1 % as `x (1 - 0.35 s) + 0.75 s tri` |
| 22 | Cutoff | 80..18000 Hz (log) | 18000 | every wave: the Chamberlin lowpass, skipped while the cutoff (moved by Env Amt and the envelope) is at least 17,500 Hz and Resonance at most 2 % |
| 23 | Resonance | 0..100 % | 0 | every wave: the lowpass's damping `clamp(1 - 0.85 r, 0.12, 1)`; its states are held to +-2, so it is finite at 100 % |
| 24 | Bitcrush | 2..16 bits | 16 | every wave: rounds to 2^(bits - 1) levels below 15.5 bits (16 is off) |
| 25 | PWM Depth | 0..100 % | 20 | waves 4..11: 0.38 x depth on the PWM triangle |
| 26 | PWM Rate | 0.1..40 Hz (log) | 2.5 | waves 4..11 |
| 27 | Env Amt | -100..100 % | 0 | every wave: the cutoff is multiplied by 2^(4 x Env Amt x envelope), clamped to 40..18000 Hz; the coefficient is taken from the envelope every 32 samples (tracker-ce: every sample) |
| 28 | Decimate | 1..32 | 1 | every wave: each sample is held for this many samples |
| 29 | Chord | 0..7 | 0 | OFF, OCTAVE, FIFTH, MAJOR, MINOR, SUS4, MAJ7, MIN7 (labelled): on every wave the four-step chord replaces Arp 1 / Arp 2 at Arp Speed; ARP CHORD plays MAJOR while it is OFF |

## Sizes and cost (mk66, `arm-none-eabi-g++ -O2` through `tools/o3m_pack.py`)

At OS-CECHIP C7 (1.1.0): file 17,720 B, hot part 2,112 B (`render` with the three oscillator loops inlined, as before: 2,096 B at 1.0.0; the SRAM_L slab is 14 KiB shared by all modules), cold section 15,056 B
(descriptor, parameter table, labels, set-up, the two views, and the engine's cold path; 8,768 B at C3, 12,160 B at C6 before the new waves' views, which cost 2,896 B with `kernel_trace` built -Os: 3.9 KB at -O2), instance 332 B (the manifest reserves 336, so that twelve instances still fit the 4 KiB arena of 16-byte blocks; it was 312 B at C3 and 192 B at 1.0.0: the models' phases, register and drum decay, less two floats that nothing read). Image minus hot is 15,056 B; `chip_test.cpp` bounds it at 15,360 B.

**Static check of the hot part (`tmod-perf`, `objdump -d -j .o3m_hot`, G1 against now):** 605 against 604 instructions, 90 against 90 FP loads and stores, 4 against 4 `vdiv`, and `blx` 1 against 2 (the extra one is the call from `render` into the cold `ce::` path, once per render call, not per sample); the hot part grew by 16 B (limit 64 B), 2,096 to 2,112 B. No division was added to the per-sample loops.

**Cost.** Waves 0..3 at the defaults run today's loops, unchanged: **measured on the unit, 2.8 % of a 64-frame block (about 7.3k cycles) per sounding voice on 0.4.9** (`docs/architecture/cpu-budget.md`; the earlier
estimate of 1.8 % from the instruction count was low), and on QEMU (`tests/module_builtin_run_test.cpp`) held within 1 % of G1's 3,273 / 1,765 / 1,402 / 1,464 instructions per 64-frame block for waves 0..3. A silent voice costs nothing. Waves 4..11, and waves 0..3 once a post-chain parameter is off its default (Sub Osc above 0, Bitcrush below 15.5, Decimate above 1, Resonance above 2 %, a Cutoff below 17,500 Hz
or an Env Amt other than 0), leave those loops for the cold path (it runs from SDRAM, whose speed for code is not validated): for waves 0..3 `raw<W>` writes the oscillator into the output buffer, then a second pass applies per
sample the sub phase and mix, the hold and crush, the lowpass, and the envelope and gain, with one `exp2_f` and one `sin_f` every 32 samples for the lowpass coefficient. **Measured on QEMU (instructions, not cycles; the
same 64-frame block, 8000 blocks, `[chip post icount]` in `tests/module_builtin_run_test.cpp`, with Sub Osc 40 %, Cutoff 1800 Hz, Resonance 45 %, Bitcrush 6, Env Amt 30 %, Decimate 3):** wave 0 (pulse) 8,460, wave 1 (triangle)
6,941, wave 2 6,753, wave 3 6,742; wave 0 with Sub Osc alone 6,218; that is about 2.6 times the default pulse (3,285 now) and 3.9 to 4.9 times the other three. **Waves 4..11 at their defaults (no post-chain pass; one note, the same block):** 2A03 PULSE 5,859, SID SAW/TRI 6,113, NOISE/LFSR 3,250, FM BELL 9,691, SUPER SQR 10,121, ZAP/LASER 5,910, ARP CHORD 5,886, CHIP DRUM 6,300. Waves 0..3 at the defaults, as of this build: 3,285 / 1,769 / 1,410 / 1,468 against G1's 3,273 / 1,765 / 1,402 / 1,464 (+0.4 %, +0.2 %, +0.6 %, +0.3 %: inside the 1 % that `qemu_module_builtin` checks). They are instruction counts of the whole `render` call on the
Cortex-M4F emulator, not the unit's cycles (`diag cpu` with a held note on each wave, and on waves 0..3 with the post chain on, stays owed: **unit: pending**).

**Size, C3 to C5.** The file grew from 11,328 B (C3) to 14,684 B (C5), +3,356 B (about +3.4 KB), all of it cold: the cold section from 8,768 to 12,096 B (+3,328 B) for SID, NOISE/LFSR, SUPER SQR, FM BELL and CHIP DRUM, the hot part unchanged at 2,112 B. C4 and C5 are both in `chip_ce.h`.

**Fast against Std (OS-CECHIP C6, host, `tests/cechip_oracle_test.cpp`).** Policy `Fast` (polynomial sine, the drum's recurrence, the host's maths at control points) is held against `Std` and against the original engine per wave by SNR. Bound of the spec: at least 60 dB. Measured (dB, higher is closer):

| Case | Measured | Check at |
|---|---|---|
| Pulse voice against the original (waves 0..3 family, every note and shape) | 64 to 122 (worst: note 108, Shape 0.5, 63.9; the others 88 to 122) | 55 |
| Post chain voice, one term at a time: Sub Osc / Decimate / Bitcrush / Cutoff / Env Amt / all | 88.1 / 89.3 / 71.3 / 96.9 / 88.3 / 133 | 55 |
| SID `Fast` against `Std` (Shape 0 / 0.35 / 1) | 173.2 / 169.4 / 163.6 | 60 |
| SUPER SQR `Fast` against `Std` (three shapes) | 168.6 / 165.8 / 166.8 | 60 |
| SID and SUPER SQR voice against the original | wave 5: 93.9 and 102.1; wave 8: 91.2 and 85.7 | 60 |
| NOISE/LFSR voice against the original | 32.6 (note 60, Shape 0.35) and 200 (bit-exact) | 30 |
| FM BELL `Fast` against `Std` (Shape 0 / 0.5 / 1) | 149.9 / 150.2 / 150.2 | 60 |
| FM BELL voice against the original (Shape 0 / 0.5 / 1) | 62.0 / 51.3 / 47.2 | 40 |
| CHIP DRUM `Fast` against `Std` (Shape 0 / 0.5 / 1) | **44.5** / 55.1 / bit-exact (200) | 40 |
| CHIP DRUM voice against the original (Shape 0 / 0.5 / 1; from sample 1500) | 48.3 / 56.9 / 200 | 20 |
| CHIP DRUM recurrence, worst relative error over 30,000 samples | 1.69e-6 | |
| Sine polynomial, worst error | 7.6e-8 | |

The noise bound is the loosest (30 dB) because a one-sample shift of the register's clock changes the sequence, not its sound. **CHIP DRUM misses the 60 dB bound at Shape 0 (44.5 dB) and at 50 % (55.1 dB).** The spec allowed the drum to keep `Std`'s call for that term; it was tried and does not help: with the per-sample host `exp2` of `Std` in the decay the `Fast` drum still reaches only 50.1 dB and 64.6 dB, so the gap is not the recurrence (whose own error is 1.7e-6) but the rounding edges of the stepped body, where a one-ulp phase difference moves a dozen samples a second. `Fast` stays (the per-sample `exp2` is the cost the recurrence exists to avoid, and it buys no 60 dB), the check is held at 40 dB so that it cannot get worse, and the deviation is recorded here (the voice against the original: 48.3 dB from sample 1500). Unit measures (`diag mod`, `diag cpu`) stay owed.

## Tests (`ctest -R o3m_chip`)

Duty cycles and the removed mean; pitch (six notes, two waves, transpose and fine); PolyBLEP against raw edges (inharmonic share of the spectrum, at least 6 dB lower); triangle shape; noise levels and flip rate; the LFSR's
periods (32767 and 93); envelope timing (attack, decay, sustain, release, the 1 ms floor, retrigger continuity, sustain 0 ending the note); 4-bit steps (every sample a multiple of 1/15, sixteen levels); arpeggio (three, two and one
step, downwards); sweep (linear in semitones, repeats at every note); vibrato (depth, rate, delay); velocity and Volume; **block-size independence** (bit for bit over five block sizes); hostile parameters (NaN, 1e30, out-of-range
ids, note and velocity): no NaN, bounded; the packed `.tmod` through the loader's checks (header, CRC, relocation, descriptor); both views against formulae and the draw API's budgets for every wave, duty and corner of the envelope; the new waves' view (names, 96 points, Shape and Sub Osc move the trace, the zap and drum cross zero more often early than late, Chord changes ARP CHORD, a wave out of range clamps) and the envelope view's filter text.

## Hardware check (by ear still open)

The cost is measured (above). Copy `chip.tmod` to `/modules/instruments/` on a test card, Instrument page F1 ENGINE, pick Chip, play: each wave and duty, the envelope, the arpeggio and the sweep by ear; read the `diag mod` timing for a held note.

RATE (OS-MODRATE S5, ABI 1.13): this module declares `O3M_FX_RATE`, so the MIX page offers FULL, 1/2 and 1/4 (it is called at 22050 or 11025 Hz and the OS holds each sample). Every rate is derived from `host->sample_rate` at `init`. The new-path frequency clamp (CE's 16..18000 Hz) is also held at 0.45 of the rate (`ce::kMaxStep`, as waves 0..3 already were), the noise LFSR bound is 8 steps a sample (7.2 at most), and the post chain's lowpass ceiling is 0.45 of the rate (a coefficient past a quarter of the rate would turn negative at 11025 Hz). At 44100 Hz all three are exactly as before (`chip_golden_test`, the oracle tests). By ear and CPU at 1/2 and 1/4 are open on the unit.
