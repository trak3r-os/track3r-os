# SID (module `sid`)

A SID-style instrument for the Community OS (OS-SID, `docs/specs/OS-SID.md`), built into the image like TB-303, Chip and FM when it fits the flash cap, else loaded from the SD card as `/modules/instruments/sid.tmod`
(ADR 0002). **Written from scratch** against `module/sdk/o3module.h`: no third-party code, no emulator source, table or constant (no licence gate, `THIRD_PARTY.md`), MIT like the SDK. It imitates the voice of the MOS
6581 / 8580; it is not an emulation and makes no parity claim. Status: new Community OS behaviour; tested on the host through the ABI (`modules/sid/tests/sid_test.cpp`); **slice 2 of 2: the voice and the filter**.
Not yet run on a unit: the cost and the sound are open (below).

## What it is

One mono voice an instance (the ABI runs one instance per instrument slot: play chords on several tracks).

- **Wave**: SAW, TRIANGLE, PULSE, NOISE, and four combined waves SAW+TRI, PULSE+SAW, PULSE+TRI, P+SAW+TRI. Saw and pulse are band-limited with PolyBLEP; the triangle, the noise and the combined waves are raw, as on the chip.
- **Pulse Width** (1 to 99 %, a modulation target) and a built-in **PWM LFO** (PWM Depth, PWM Rate): a triangle that moves the width. The width glides to its target over a control period by at most half a phase step a
  sample, so an edge never runs back over the phase and makes a second pulse: a sweep, a stepped parameter or a jump has no click. The pulse has its mean removed (a narrow pulse peaks at 1.75, hence the -12 dB default Volume).
- **Noise**: a 23-bit LFSR, 8-bit output, sample-and-hold at 16 x the note's frequency (never faster than the sample rate), so it follows the pitch.
- **Combined waves**: the module's own 128 x 8-bit tables (interpolated linearly), 4 combinations x 2 models (1 KiB), generated at compile time by a documented formula (`sid_dsp.h`: the AND of the waveform bits, a pull-down by the neighbouring bit
  where a pulse is in the mix, one more for the 6581, a circular [1 2 1] smoothing on a 256-point grid whose even points are kept, normalised to a peak of 255 for the 8580 and 191 for the 6581). The pulse in the three combinations that have one is a gate: high for the
  Pulse Width, silent for the rest of the period. The output has no DC (the table's integral up to the width is subtracted, at control rate).
- **Mod Pitch / Mod Fine** (semitones, a modulation target; cents): the pitch of the second oscillator. **Sync on**: the modulator is the sync source and runs at the key's pitch (the note, Transpose, Fine, arpeggio, sweep
  and vibrato), and the main oscillator runs at the key plus Mod Pitch and Mod Fine, so the pitch you hear is always the key's and sweeping Mod Pitch sweeps the timbre (the classic sync lead). The oscillator restarts each time
  the modulator wraps, to the fraction of a sample at which it wrapped, with a BLEP on both sides of the reset (the jump is known one sample ahead). **Sync off**: the oscillator is at the key and the modulator at the key plus
  Mod Pitch; **Ring** multiplies the triangle by the modulator's sign (only the
  triangle rings, as on the chip). Off, the modulator costs nothing and Mod Pitch changes no sample.
- **Envelope**: Attack, Decay, Release are the 16 rate values of the Commodore 64 table, Sustain 16 levels (the 4-bit value as the byte value x 17). The counter is 8 bits and the output is its whole part (the chip's
  staircase); it is held as 8.24 fixed point, so a 24 s fall is exact to its rounding. A rate value is the time of a full-scale travel of the counter; a fall to a sustain level, or a release from the level the note is at, takes
  its share of it. A new note starts its attack from the counter's level; a Sustain of 0 ends the note after the decay (the voice says it is idle). The labels of the three parameters carry the times.
- **Arp 1 / Arp 2 / Arp Speed, Sweep / Sweep Time, Vib Depth / Vib Rate / Vib Delay**: Chip's control-rate pitch block (every 32 samples), and a note the host ramps over the one that sounds (the Glide and Slide FX of a pattern,
  `O3M_NOTE_RAMP`, ABI 1.8) keeps the envelope, the arpeggio, the sweep and the vibrato and only moves the pitch.
- **Model** (6581 / 8580, on the FILTER page) selects the combined tables, the filter's cutoff curve and the clip's headroom.

## The filter (slice 2, OS-SID S5; new Community OS behaviour, an imitation of the character of the chip's filter, no parity claim)

One filter per instance, that is per track (the chip shares one filter between its three voices). A **zero-delay-feedback state-variable filter** (the trapezoidal-integrator form: g = tan(pi fc / fs), damping k, a1 = 1 / (1 + g (g + k)),
a2 = g a1, a3 = g a2; low pass v2, band pass v1, high pass x - k v1 - v2), unconditionally stable for k > 0, one pass per sample after the envelope, in the hot part, on each chunk of at most 32 samples with the coefficients of that chunk's control tick.

- **Mode** mixes the three: OFF (skipped: no pass, no host call, the state at rest), LP, BP, HP, NOTCH (LP + HP), LP+BP, BP+HP, ALL. The band pass is weighted by k so that its peak is 1 at any resonance.
- **Resonance** 0..15: k = sqrt(2) / 20^(r / 15): 0 is a Butterworth low pass (-3 dB at the cutoff), 15 a peak of about 23 dB (1 / k).
- **Cutoff** 0..100 % through the model's curve, the module's own (not a measurement of the chip): 8580 an exponential from 30 Hz to 12 kHz, 6581 an S-shaped one (smoothstep) from 150 Hz to 12 kHz (lingers at both ends,
  quick in the middle). **Key Track** (0..100 %) multiplies the frequency by 2^(kt (key - 60) / 12), the key being the note with Transpose, Fine, arpeggio, sweep and vibrato; **Env Amount** (-100..100 %) adds that fraction of the
  envelope's level to the cutoff position, clamped to 0..100 %. The frequency is held between 10 Hz and 0.45 of the sample rate.
- **Coefficients at control rate and only when they change**: the exponent log2(fc) is formed every control tick (adds and multiplies, no host call) and the coefficients (the host's exp2, sin and cos, one division) are remade only
  when it moved by more than 1.5e-4 octave (a tenth of a cent), and when Mode or Resonance is set. A steady note with no movement makes no host call for the filter at all (the test counts them).
- **Drive** (0..100 %) multiplies the input by m = 1 to 16 before a **soft clip** x (1 - x^2 / 6.75), held at +-1 beyond +-1.5 (bounded by 1, odd, slope 1 at 0): the 6581 has headroom 1, the 8580 four times that: the clip sees
  the input divided by the headroom and the output is multiplied by it, so the small-signal gain is the same. A second clip bounds the mixed output. **Drive is level-compensated**: the output is also multiplied by (1 + Drive) / m, so
  the small-signal gain is 1 + Drive and, as the clip never gives more than its input, full Drive is at most +6 dB over Mode OFF at the same note and Volume on both models (saturation, not level; the test holds it, with the
  filter wide open and no resonance). A loud note under full Drive is therefore quieter than Mode OFF, not louder (the clip's peak, headroom x 2 / 16: 0.125 on the 6581, 0.5 on the 8580). New Community OS behaviour: **a default, to confirm by
  the maintainer** (review fold of slice 2: without it an 8580 at full Drive came out about +22 dB over Mode OFF into the master bus).
- After the note ends the filter keeps running over silence until its state has died away (sum of squares below 1e-9), so a resonant filter rings on and the voice reports busy until then.
- Not in: the chip's filter nonlinearity per stage, the 6581's cutoff spread between chips, the external-input path.
- Open (not claimed as parity): the cutoff curves' shape and ranges, the resonance law and the clip's character are the module's own choices; check them on a unit against a recording of a chip (QA-MOD-226..235).

## Views

**Wave** (two periods of the wave: the pulse at its width, the noise register's 48 steps, the combined waves from the table gated by the pulse; named), **Envelope** (the level against time for the key held and released, with
the table's times) and **Filter** (the magnitude of the Mode's mix at the cutoff the knob says, 20 Hz to 20 kHz on a log axis, -48 to +24 dB, 97 points crowded round the cutoff, the middle one exactly at it; the key's and the
envelope's shifts are not drawn; the mode's name in the corner). All are drawn from the parameter values the page hands over. The page's three views share 700 estimated display-list words: Wave 122 and Envelope 339 (461 together) and
Filter 217 (no axes, which cost 200 words) make 678 (the TB-303's two use 688); the test holds the three at 690 at most.

## Parameters (a preset is 31 little-endian float32 values in this order; pages OSC 0-7, ENV 8-15, FILTER 16-22, PITCH 23-30; the 32nd slot is free)

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Wave | 0..7 | 2 | SAW, TRIANGLE, PULSE, NOISE, SAW+TRI, PULSE+SAW, PULSE+TRI, P+SAW+TRI (labelled) |
| 1 | Pulse Width | 1..99 % | 50 | modulation target |
| 2 | PWM Depth | 0..100 % | 0 | the width moves +- depth / 2 of the period |
| 3 | PWM Rate | 0.1..20 Hz (log) | 2 | |
| 4 | Sync | 0 / 1 | 0 | |
| 5 | Ring | 0 / 1 | 0 | |
| 6 | Mod Pitch | -24..48 semitones | 7 | modulation target |
| 7 | Mod Fine | -100..100 cents | 0 | |
| 8 | Attack | 0..15 | 0 | 2 ms .. 8 s (labelled) |
| 9 | Decay | 0..15 | 8 | 6 ms .. 24 s (labelled) |
| 10 | Sustain | 0..15 | 10 | 16 levels |
| 11 | Release | 0..15 | 6 | 6 ms .. 24 s (labelled) |
| 12 | Volume | -48..+6 dB | -12 | modulation target |
| 13 | Vel Sens | 0..100 % | 50 | 1 - sens x (1 - velocity / 127) |
| 14 | Transpose | -24..24 semitones | 0 | |
| 15 | Fine | -100..100 cents | 0 | |
| 16 | Mode | 0..7 | 0 | OFF, LP, BP, HP, NOTCH, LP+BP, BP+HP, ALL (labelled) |
| 17 | Cutoff | 0..100 % | 50 | modulation target |
| 18 | Resonance | 0..15 | 0 | 16 steps of k |
| 19 | Key Track | 0..100 % | 0 | octaves of cutoff an octave of key |
| 20 | Env Amount | -100..100 % | 0 | the envelope level moves the cutoff position |
| 21 | Model | 0..1 | 0 | 6581, 8580 (labelled): the combined tables, the filter's curve and clip |
| 22 | Drive | 0..100 % | 0 | input gain 1..16 into the soft clip |
| 23 | Sweep | -48..48 semitones | 0 | modulation target |
| 24 | Sweep Time | 0..2000 ms | 120 | |
| 25 | Vib Depth | 0..200 cents | 0 | modulation target |
| 26 | Vib Rate | 1..16 Hz (log) | 6 | |
| 27 | Vib Delay | 0..1000 ms | 0 | |
| 28 | Arp 1 | -24..24 semitones | 0 | |
| 29 | Arp 2 | -24..24 semitones | 0 | |
| 30 | Arp Speed | 1..120 Hz (log) | 50 | |

## Sources of the numbers (no GPL source was read for any of them)

- **Envelope times**: the Commodore 64 Programmer's Reference Guide (Commodore Business Machines, 1982), the SID chapter's rate table, as quoted on C64-Wiki's ADSR page (<https://www.c64-wiki.com/wiki/ADSR>): attack 2 ms, 8 ms,
  16 ms, 24 ms, 38 ms, 56 ms, 68 ms, 80 ms, 100 ms, 250 ms, 500 ms, 800 ms, 1 s, 3 s, 5 s, 8 s; decay and release 6 ms, 24 ms, 48 ms, 72 ms, 114 ms, 168 ms, 204 ms, 240 ms, 300 ms, 750 ms, 1.5 s, 2.4 s, 3 s,
  9 s, 15 s, 24 s. The step period of a rate value is time x clock / 255 clock cycles (a 255-step counter) and a sample is clock / sample rate cycles. The counter's 8 bits and the sustain nibble compared as the byte value x 17
  are the chip's documented register layout (C64-Wiki, SID).
- **Clock**: the PAL clock, 985,248 Hz.
- **Noise**: a maximal-length 23-bit LFSR, the polynomial x^23 + x^18 + 1 (taps 23 and 18) from the standard table of maximal-length LFSR taps (Xilinx application note XAPP052 "Efficient Shift Registers, LFSR Counters, and
  Long Pseudo-Random Sequence Generators"); the period 2^23 - 1 = 8,388,607 is counted by the test. The eight output bits (register bits 20, 18, 15, 12, 10, 7, 4, 1) are this module's own choice.
- **Combined waves**: the formula above; the module's own approximation.

## Open research questions (not claimed as parity)

- The noise's output taps against the chip's, hence the colour of the noise (the period and the level do not depend on them).
- The combined tables: not measurements; tuned by ear is still to do, on a unit, with the by-ear rows of the QA catalogue (QA-MOD-210..).
- The chip's piecewise-exponential decay and release (a counter that slows at fixed levels) is not in: the fall here is linear in the counter, as the Programmer's Reference Guide's times describe. The ADSR delay bug,
  the analogue 6581 modelling (DAC non-linearity, drift, oscillator leak) and `.sid` playback are left out (OS-SID S7).
- Sync and Ring together: the modulator is at the key (Sync wins), so Ring then multiplies by the sign of a square at the key's pitch.

## Sizes and cost (mk66, `arm-none-eabi-g++ -O2` through `tools/o3m_pack.py`)

Image 14,528 B: hot part 4,448 B (`render` with the five oscillator loops and the filter pass inlined, 3,412 B of code, plus the 1 KiB of combined-wave tables, which sit in the hot part so the loop never reads SDRAM), the rest cold
(descriptor, parameter table and labels, set-up, the three views); instance 344 B on the target (the manifest reserves 368: the host's 64-bit pointer makes the host's 360). The usable SRAM_L slab is 14,336 B minus the voices' 1,280 B of
output blocks (`ModuleVoices::kScratchBytes`, `platform/mk66/src/app_main.cpp`) = **13,056 B**, shared by all modules: the hot parts are TB-303 3,520 B, Chip 2,096 B, FM 1,952 B and SID 4,448 B (34.1 % of the arena), 12,016 B together
(92.0 %), **1,040 B left** before padding. (Slice 1's 4,960 B left only about 500 B; review fold: tables halved to 128 entries, budget 4,500 B for the whole module, filter included.) **Flash, embedded** (`tools/remote_ci.sh mk66`):
FLASH 939,548 B (dev) to 954,100 B, +14,552 B (cap 18 KB, OS-SID S1; the .tmod file is 14,528 B); SRAM_U and SDRAM_STATIC unchanged. Slice 2 review fold (level-compensated Drive, the Mode fix): the hot part stays 4,448 B
(budget 4,500 B, which the test now holds), the instance 344 B, the .tmod file 14,644 B, FLASH 985,852 B (+120 B over the unfolded slice 2); SRAM_L, SRAM_U and SDRAM_STATIC unchanged.

`tmod-perf` static check of the hot code (`objdump -d -j .o3m_hot --stop-address` at the table, 3,412 B): 983 instructions, FP loads and stores 9 % (90; target under 15 %), 0 VDIV, 6 `blx` (long calls out of the hot part, the same six
sites as in slice 1: `advance()` when the envelope reaches a stage's target and the control tick; none inside a sample loop), for comparison TB-303 932 / 31 % / 12 / 1, Chip 605 / 15 % / 4 / 1, FM 530 / 16 % / 0 / 0 (slice 1's
measurement of the same command). The filter pass keeps its state and coefficients in registers and has no branch but the loop. The envelope is integer (8.24), the per-sample curves are adds and compares, no host maths in a loop; the
per-note-and-tick work (the pitch's exp2, two reciprocals, the duty ramp, the table's DC and, when the cutoff moved, the filter's exp2, sin, cos and one division) is one call a control period.

**Cost: not measured.** Estimate [est] from the radar card's cost table (`docs/vision/radar/sid-instrument.md`) (the voice alone: about 130 cycles a sample with Sync and Ring off and about 160 with them on, 3.2 to 3.9 % of a
64-frame block; Chip measured 2.3 to 2.8 %) plus about 40 cycles a sample for the filter pass [est] (about 1 %): 4.2 to 4.9 % with the filter on in the worst case. The target with the filter on is at most 5 % (S8); it is measured on the unit with `diag mod` and `diag cpu` (OS-SID bench items B1 and B3) before merge. A silent voice costs nothing.

## Tests (`ctest -R o3m_sid`)

The descriptor and the final order of the 31 parameters, pages and labels; the parameter blob round trip (any order of the set calls, the defaults as a blob); the pitch of every wave (both models for the combined ones),
transposition, fine tune and the Nyquist clamp; the saw and triangle shapes and the saw's band-limiting; the pulse's width and mean; a width sweep (the LFO, a stepped parameter, a jump) with two edges a period and no extra pulse;
the LFSR's period (counted over 8,388,607 steps), the noise's level, levels and clock rate; the attack, decay and release times of all 16 rate values against the Programmer's Reference Guide's table within one control step, the
sustain level, the 8-bit staircase; a note over a sounding one, a note off before the first sample; sync (the period is the key's whatever Mod Pitch is, below and above the key; the inharmonic share 23 to 30 times below a naive reset's, other waves bounded); the modulator
off costs nothing; ring (sign, rate, only the triangle); the combined tables (peaks, no DC, not silent, all different, the 8580 louder, the pulse gate); the filter: the response against its own formula for every mode, both models, three cutoffs and three resonances at five frequencies, the -3 dB point of LP and HP against the model's curve by bisection, the resonance peak 1 / k, the band pass's peak 1, the notch's depth, stability at the highest resonance and the extreme cutoffs for every mode with the most Drive (finite and under the clip's bound), the soft clip's bounds, Mode OFF skipping the filter (no sample changes, no host call, the state at rest), the coefficients remade only when the cutoff moved (host calls counted), Key Track, Env Amount and the cutoff curves, the ring of the tail and the end of it; velocity and Volume;
arpeggio, sweep and vibrato; block-size independence (bit for bit over five block sizes, with sync, ring, PWM, arpeggio, sweep, vibrato and the filter on); hostile parameters and mixed extremes with a hostile host pitch; the combined waves' DC through a PWM sweep; hostile parameters; the packed `.tmod` through the loader's checks; the three views
against formulae and the draw API's budgets; `pitch()` and `O3M_NOTE_RAMP`.

## Hardware check (open)

Copy `sid.tmod` to `/modules/instruments/` on a test card (or pick SID from the ENGINE list when it is built in), Instrument page F1 ENGINE, pick SID, play: each wave, the PWM, sync and ring, the envelope times by ear, and read
`diag mod` and `diag cpu` for a held note, one voice and three (OS-SID bench items B1, B3 and B4). The cost target is at most 5 % of a block a sounding voice (S8).
