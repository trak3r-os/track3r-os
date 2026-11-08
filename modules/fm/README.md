# FM (module `fm`)

A four-operator FM instrument for the Community OS, loaded from the SD card as `/modules/instruments/fm.tmod` (ADR 0002, OS-012c). **Written from scratch** against `module/sdk/o3module.h`: the textbook maths of phase modulation
(Chowning, 1973) with this module's own algorithm tables, sine and envelopes. **Not derived from MSFA / Dexed**: none of that code was read or ported (the dsp-port licence gate has nothing to pass; `THIRD_PARTY.md`). MIT like the SDK.
Status: new Community OS behaviour; tested on the host through the ABI (`modules/fm/tests/fm_test.cpp`) against an independent double-precision model; run on the unit (measured 11.7 % of a block per sounding voice on 0.4.9, `docs/architecture/cpu-budget.md`; the HIL scenarios bind and sound it). By-ear checks of each algorithm remain open.

## What it is

One mono voice an instance. Four sine operators; for each a frequency **Ratio** (0 is 0.5, then 1 to 31), a **Detune** (cents), a **Level** (0..100, 0.75 dB a step, 0 silent), and an **ADSR**. Sixteen **algorithms** say which operators modulate
which and which are heard (the diagrams are the Algorithm view); operator 4 has a **Feedback** loop.

    operator:  out = sin(2 pi (phase + modulation)) x envelope x level          phase advances ratio x detune x the note's frequency a sample
    modulation = 2 turns x the sum of the outputs of the operators the algorithm routes into it  (a full-level modulator swings the phase by 4 pi)
    output     = the sum of the carriers x Volume x velocity

| algorithm | routing (4 > 3: 4 modulates 3) | heard |
|---|---|---|
| 1 | 4 > 3 > 2 > 1 | 1 |
| 2 | (4 + 3) > 2 > 1 | 1 |
| 3 | (4 > 3 + 2) > 1 | 1 |
| 4 | (3 > 2 > 1) + 4 alone | 1, 4 |
| 5 | (2 > 1) + (4 > 3) | 1, 3 |
| 6 | (4 > 3 > 2) + 1 alone | 1, 2 |
| 7 | 4 > (1 + 2 + 3) | 1, 2, 3 |
| 8 | 1 + 2 + 3 + 4 | all |
| 9 | 4 > (3 + 2) > 1 (4 modulates 3 and 2, both modulate 1) | 1 |
| 10 | 4 > 3 > (1 + 2) | 1, 2 |
| 11 | (3 > 1) + (4 > 2) | 1, 2 |
| 12 | (4 + 3 + 2) > 1 | 1 |
| 13 | 4 > 3 > 2, and 4 > 1 | 1, 2 |
| 14 | (4 + 3) > (2 + 1) | 1, 2 |
| 15 | (3 > 2) + 4 > 1 (1 is modulated by 2 and 4, 2 by 3) | 1 |
| 16 | 4 > 2 > 1, and 3 alone | 1, 3 |

Algorithms 9 to 16 were added by OS-SNDMOD S2; 1 to 8 keep their numbers, so a stored project sounds as it did. The render always evaluates the operators 4, 3, 2, 1 with the same six links, so an algorithm costs nothing extra.

Envelopes: the attack is linear in amplitude (at least 1 ms), the decay and the release are exponential: **D and R are the time of a fall by 60 dB**, a decay to a sustain level covers 99.9 % of its way in D. A sustain of 0 ends the
operator at -80 dB. A new note over a sounding one starts every envelope from where it is (no click). Feedback F is the mean of operator 4's last two outputs x 0.5 turn x 2^(F-7) (F = 0 off). An operator whose frequency passes
0.4 of the sample rate fades out and is silent at 0.5 (it never folds). The voice says it is idle when no audible carrier's envelope runs. Carriers are summed at unity: four carriers at full level can add up to 4, so lower Volume (default -12 dB) for the stacked algorithms; the engine clips at full scale and counts it.
The sine is a 7th-order odd polynomial on a quarter wave (maximum error 1.2e-6, no table: a table in the cold part would be read from SDRAM by the hot loop).

## Views

**Algorithm**: the selected algorithm as a diagram, one box an operator (its number, its ratio, a bar for its level; carriers in the instrument colour, modulators in the FX colour), a line from each modulator down to the operator it
modulates, a stub from each carrier to the output bus, and operator 4's feedback loop (lit when Feedback is on): about 190 estimated words. **Envelopes**: the four envelopes in one frame, each scaled by Level / 100, the key's release
marked, the window's length in ms: four polylines of 48 points and a frame of rectangles, about 270 words (a curve with axes costs 200 words each, which four would exceed). The pair is about 470 words.

## Parameters (a preset is thirty-two little-endian float32 values in this order: four pages of eight, OP1 to OP4, an operator's seven and one global each)

| ids | name | range | default (op 1 / 2 / 3 / 4) |
|---|---|---|---|
| 8n + 0 | Op(n+1) Ratio | 0..31 | 1 / 14 / 1 / 1 |
| 8n + 1 | Op(n+1) Detune | -1200..1200 cents (was -100..100) | 0 / 0 / 4 / 0 |
| 8n + 2 | Op(n+1) Level | 0..100 (0.75 dB a step, 0 silent; no unit, not a percentage) | 100 / 55 / 90 / 50 |
| 8n + 3 | Op(n+1) Attack | 1..4000 ms (log) | 1 |
| 8n + 4 | Op(n+1) Decay | 5..8000 ms (log) | 1500 / 250 / 1800 / 800 |
| 8n + 5 | Op(n+1) Sustain | 0..100 % | 25 / 0 / 25 / 0 |
| 8n + 6 | Op(n+1) Release | 5..8000 ms (log) | 300 / 100 / 300 / 200 |
| 7 | Algorithm | 1..16 | 5 (labelled `4>3>2>1`, `(4+3)>2>1`, `(4>3)+2>1`, `3>2>1 + 4`, `2>1 + 4>3`, `4>3>2 + 1`, `4>1,2,3`, `1+2+3+4`, `4>(3+2)>1`, `4>3>1,2`, `3>1 + 4>2`, `4+3+2>1`, `4>3>2, 4>1`, `(4+3)>(2+1)`, `(3>2)+4>1`, `4>2>1 + 3`; the Algorithm view's title reads the same labels; not learnable) |
| 15 | Feedback | 0..7 | 0 |
| 23 | Volume | -48..+6 dB | -12 |
| 31 | Vel Sens | 0..100 % | 50 (velocity scales the carriers only) |

The default patch is an electric piano (algorithm 5: a tine at ratio 14 on the first stack, a soft body on the second, 4 cents of chorus).

## Sizes and cost (mk66, `arm-none-eabi-g++ -O2` through `tools/o3m_pack.py`)

Image 9,008 B: hot part 1,952 B (`render` with the four operators inlined), the rest cold (descriptor, the 32-entry parameter table, set-up, the two views); instance 432 B (the manifest reserves 448), as `o3m_pack.py`
builds it at OS-SNDMOD S2. **Cost, measured on the unit: 11.7 % of a 64-frame block (about 30.5k cycles) per sounding voice on 0.4.9** (`docs/architecture/cpu-budget.md`; the earlier estimate of 6 % from the instruction count was
half of it); an operator at level 0 skips its sine. A silent voice costs nothing. The heavy-song budget row for FM is 8 %: over it today (Perf M3, parked).

## Tests (`ctest -R o3m_fm`)

The algorithm table (all sixteen) against an independent transcription of the diagrams; the sine against `std::sin` (1.2e-6 over +-5.5 turns); **every algorithm, with and without feedback, against a double-precision model, sample by sample** (and Detune up to +-1200 cents in four algorithms)
(ratios, detunes and levels different on each operator); operator frequencies (ratios 0 to 31, detunes, notes 24 to 96); **sideband amplitudes against the Bessel functions J0 to J3 of the modulation index** (four levels, upper and
lower sidebands); envelope stages against their formulae (the sample divided by the sine of its known phase: attack, decay to the sustain, release, 60 dB in R, the end at -80 dB); a percussive operator ending the note and retrigger
continuity; velocity, Volume and the feedback's harmonics; block-size independence; hostile parameters (NaN, 1e30, ids, note and velocity out of range) and the Nyquist fade; the packed `.tmod` through the loader's checks; the views'
geometry (a line per modulation, a stub per carrier, modulators above their targets, the ratio texts, the envelope polylines against the formulae to a pixel) and budgets for every algorithm and corner of the operators.

## Hardware check (by ear still open)

The cost is measured (above). Copy `fm.tmod` to `/modules/instruments/` on a test card, Instrument page F1 ENGINE, pick FM, play: the default electric piano, then each algorithm; sweep an operator's Level while holding a note (the modulation index follows with no
zipper noise); read the `diag mod` timing of a held note (the budget row is 8 % of the audio CPU).

RATE (OS-MODRATE, ABI 1.13): this module declares `O3M_FX_RATE` (`"rate": true` in `module.json`), so the MIX page offers FULL, 1/2 and 1/4: it is called for half or a quarter of the frames at 22050 or 11025 Hz and the OS holds each sample. Every rate is derived from `host->sample_rate` at `init`.
