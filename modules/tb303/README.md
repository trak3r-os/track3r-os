# TB-303 (module `tb303`)

An acid bass voice for the Community OS, loaded from the SD card as `/modules/instruments/tb303.tmod` (ADR 0002, OS-012a). A float, block-based
port of Robin Schmidt's Open303 (MIT, credits below), built from `tb303_dsp.h` (the voice) and `tb303.cpp` (the descriptor), against `module/sdk/o3module.h`.
Status: new Community OS behaviour; the DSP is tested on the host against the original (`tests/tb303_oracle_test.cpp`); run on the unit (cost below); a by-ear check of the sound against the original is still open.

## What it is

One mono voice per instance, on a track. Saw / square oscillator (a continuous blend), the TB-303 filter (four coupled one-pole stages, soft clip and a
high-pass in the feedback path), a decay envelope for the cutoff, accent and slide, and the 303's fixed shaping filters (pre / feedback / post high-pass,
all-pass, notch, de-clicker). **Accent**: velocity of *Acc Thresh* or more (default 100). **Slide**: a note that starts while another is held (`O3M_NOTE_LEGATO`) glides to the new
pitch without a new envelope, when the *Legato Slide* parameter is on (a live key played over a held live key). The Slide and Glide FX of a pattern (OS-MODPITCH, ABI 1.8) reach the module as
`O3M_NOTE_LEGATO | O3M_NOTE_RAMP` and `pitch()` calls: the envelope is kept whatever *Legato Slide* says, and the host moves the pitch (the module's own slew stays out of it). A pattern note that names no
velocity reaches the module as velocity 96, under the accent threshold of 100: a plain step is not accented, a volume FX of 100 or more accents it.

## Header fields (the `.tmod` produced by `tools/o3m_pack.py`)

kind instrument (1), ABI 1.8 (two views, `pitch`, the page table), target Cortex-M4F; hot part 3,440 B (`render` and what it inlines: the SRAM_L slab when it fits, else SDRAM), image 9,584 B
(descriptor, parameters, set-up code, the two views and the hot part), instance state 408 B (the manifest reserves 432: the host build's voice is 16 B larger), as `o3m_pack.py` builds it at OS-SNDMOD S3. The parameters, views and fixed descriptor fields
are `module.json`'s, turned into `tb303_gen.h` by `tools/o3m_gen.py` (the SDK guide's worked example, `module/sdk/README.md` section 7).

## Views (OS-012b)

Two views on the Instrument page, drawn from the parameter values the page hands over (not from the voice): **Filter**, the magnitude response of a four-pole ladder at the cutoff the knob
says and at the cutoff the envelope opens it to, with the resonance; **Envelope**, the cutoff against the time after a note (`Env Mod` with Open303's measured mapping, `Decay`). They illustrate the
voice's mapping with a linear ladder's shape; the voice itself adds a soft clip and the TB_303 fit. Tested against formulae in `tests/tb303_view_test.cpp`.

## Parameters (a preset is fourteen little-endian float32 values in this order; two pages: SOUND, ids 0 to 7, and MODS from id 8)

Ids 9 to 13 were appended by OS-SNDMOD S3. A preset of nine values (older projects) is valid: the loader keeps the module's defaults for the rest, and the defaults are the old sound bit for bit.

| id | name | range | default | note |
|---|---|---|---|---|
| 0 | Waveform | 0..1 | 0 | saw to square |
| 1 | Cutoff | 314..2394 Hz (log) | 1000 | the range Open303's cutoff mapping was measured over |
| 2 | Resonance | 0..100 % | 50 | |
| 3 | Env Mod | 0..100 % | 25 | |
| 4 | Decay | 200..2000 ms | 1000 | filter envelope of a normal note (an accent has *Acc Decay*) |
| 5 | Accent | 0..100 % | 50 | |
| 6 | Slide | 0..300 ms | 60 | |
| 7 | Volume | -48..+6 dB | -12 | |
| 8 | Legato Slide | 0 / 1 | 1 | |
| 9 | Tune | -100..100 cents | 0 | added to the note at note start |
| 10 | Transpose | -24..24 semitones | 0 | the sum with Tune is held to notes 0..127 |
| 11 | Acc Thresh | 1..127 | 100 | the velocity from which a note is an accent; a pattern note that names no velocity is 96 |
| 12 | Filter Track | 0..100 % | 0 | the cutoff follows the note: a factor 2^((note - 48) / 12 x Filter Track) on the knob cutoff, fixed at note start (new Community OS behaviour; the reference note 48 is a choice, not measured on a Devil Fish) |
| 13 | Acc Decay | 30..2000 ms (log) | 200 | the filter decay of an accented note (the original's fixed 200 ms) |

Tune, Transpose, Acc Thresh and Filter Track take effect at the next note; Acc Decay at the next accented note. None is a modulation target. Not in this package: Drive (a per-sample stage) and Soft Attack (the
amplitude envelope has no attack stage to time: it steps to full and the 200 Hz de-clicker rounds it).

## How it differs from Open303 (the radar card's variant B)

PolyBLEP saw / pulse at twice the sample rate instead of two mip-mapped wavetables (197 KB) at four times; a 4th-order elliptic decimator instead of
the 12th-order one; the filter's coefficients every 4 samples; float with an anti-denormal bias; the note list and sequencer are the host's. The filter, the
one-pole and the cutoff-mapping constants, the envelope model and the accent and slide rules are Open303's. Measured against the original (the tests print
the numbers): the ladder and every filter of the chain at 100-140 dB SNR, the envelopes' cutoff to 2e-6 relative, the oscillator in the band below 8 kHz at
36-49 dB, the whole voice within 0.3-0.6 dB mean level and 0.5-0.8 dB mean spectral distance with identical pitch. The cutoff is held to wc = pi/4 at the
2x rate (11 kHz at 44.1 kHz), the top of the fit.

## Cost (measured on the unit)

One sounding voice: 21.6 % of a 64-frame block on 0.4.9 (about 56.6k cycles live), and 41.5-43.5k cycles (about 16 %) on v0.6.4 after OS-303PERF (the tester's sampler/module split; `docs/architecture/cpu-budget.md`,
the ODD-11 row of the backlog). The heavy-song budget row for the TB-303 is 12 %, so it is over it today; the next lever is the decimator (Perf M2, parked). A silent voice costs nothing (the module reports
itself idle).

## Credits and licence

Open303, Copyright (c) 2009 Robin Schmidt (www.rs-met.com), MIT licence: `LICENSE-Open303.txt`, THIRD_PARTY.md. This port: MIT (SPDX in the sources), the same
terms. Upstream: github.com/maddanio/open303 commit 52f8614966cd.

RATE (OS-MODRATE, ABI 1.13): this module declares `O3M_FX_RATE` (`"rate": true` in `module.json`), so the MIX page offers FULL, 1/2 and 1/4: it is called for half or a quarter of the frames at 22050 or 11025 Hz and the OS holds each sample. Every rate is derived from `host->sample_rate` at `init`.
