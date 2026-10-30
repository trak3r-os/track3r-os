# GoatTracker / C64 tracker song import — radar card
Maintainer's words: "si, quiza convierte el instrumento chip en SID, tiene sentido?" (about a SID-style instrument; this card is the follow-up idea of opening C64 music: "podemos abrir SID, es facil?") · 2026-10-30 · Status: idea (later; waits for `sid-instrument`)

Related card: `sid-instrument` (this card depends on it). Backlog row: OS-GTIMPORT (IDEA; the maintainer moves rows).

## What it is
Open a C64 tracker song as a Community OS project that plays with the SID instrument: GoatTracker 2 `.sng` first, maybe SID-Wizard later. The three C64 channels become three tracks, the patterns and order lists become the song, each C64 instrument becomes a SID instrument preset. Playing raw `.sid` files (6502 code) is a different job and is not proposed.

Honest summary of what survives: the notes, patterns, order, tempo and the instruments' envelopes, waveform and pulse width survive; the **per-frame instrument programs** (wave, pulse and filter tables, the soul of many C64 sounds: drum hits, fast arpeggios, filter sweeps) do **not** survive in the first version. The result is a faithful score with simpler sounds, unless a later table feature exists.

## Prior art
| Project | URL | Licence | Language | Last activity |
|---|---|---|---|---|
| GoatTracker 2 (C64 tracker by Lasse Oorni; the `.sng` format; the SourceForge project is the home, release 2.77 is the newest found, packaged by Debian as 2.77+ds-1, uploaded 2025-02-28 per a package-tracker search result) | GitHub mirror with the readme: https://github.com/dwhinham/goattracker2 ; Debian: https://tracker.debian.org/goattracker | GPL: the GitHub field says GPL-2.0 for the mirror; a Gentoo ebuild lists GPL-2; the readme of v2.75 says "GNU General Public License" and points to `COPYING` without a version | C | mirror pushed 2020-03-13 (a personal fork, GitHub API read 2026-10-30); upstream releases 2.75 and 2.77 |
| GoatTracker's own tools in the package (per the Gentoo ebuild: `gt2reloc`, `ins2snd2`, `mod2sng` (MOD to SNG), `sngspli2`) | https://gpo.zugaina.org/AJAX/Ebuild/55561395/View | GPL-2 | C | n/a |
| SID-Wizard (C64 tracker by Hermit; SourceForge says Public Domain, the GitHub fork of 1.8.7 says "WTF" licensed; a separate source repository link was not confirmed) | https://sourceforge.net/projects/sid-wizard , https://github.com/anarkiwi/sid-wizard | Public Domain (SourceForge label) / "WTF" (fork README); neither verified in a licence file | 6502 assembly (not checked) | fork not checked; SourceForge page not dated here |
| libsidplayfp / reSIDfp (reference for how the sound should be) | see `sid-instrument` | GPL-2.0-or-later | C++ | see that card |
| Furnace (multi-system tracker with SID channels; a modern tracker's treatment of SID instruments with macros, i.e. per-tick programs) | https://github.com/tildearrow/furnace | GPL-2.0-or-later for most of the project (README wording) | C++ | pushed 2026-10-30 |

The OS already reads MOD, S3M and MED (`formats/`); GoatTracker's own `mod2sng` goes the other way (MOD to SNG), so a `.sng` reader is new work.

## Approach
**adapt the idea, write the reader from scratch** from the format description (a few hundred lines). The format is a documented binary layout (the readme section), not code; no GoatTracker source is needed. Three tiers, only the first is a candidate:

- **Tier 0 (score and static instruments):** parse the song, unroll the order lists, map instruments to static SID presets (below). Lossy for table programs; report what was dropped.
- **Tier 1 (per-frame programs):** needs the SID module (or another feature) to run a small table program at the C64's frame rate (50 Hz, PAL). The SID module as proposed has 31 of the 32 allowed parameters and its presets are float parameter blobs, so a table cannot be carried by parameters: it needs new persisted instrument data (a project-format chunk: Level C) or a data pack the module can read (no SD reads in the render path). Open research question; not in this card's scope.
- **Tier 2:** SID-Wizard (`.swm`, format not researched here).

### Format facts (GoatTracker v2 `.sng`, from the readme of v2.75, https://raw.githubusercontent.com/dwhinham/goattracker2/master/readme.txt; v2.77 not diffed)
- Identifier `GTS5`; then song name, author, copyright (three 32-byte zero-padded fields); then the number of subtunes.
- Order lists, per subtune and per channel (three channels): a length byte `n` (not counting the restart position), then `n + 1` bytes: `$00-$CF` pattern numbers, `$D0-$DF` REPEAT (repeat the next pattern 1 to 16 times), `$E0-$FE` TRANSPOSE (halftones, up 0 to 14, down 1 to 15; resets only when the song starts, not at a loop), `$FF` end mark followed by the restart position. Up to 254 entries.
- Instruments: a count byte (instrument 0 is not stored), then 25 bytes each: attack / decay, sustain / release, wave pointer, pulse pointer, filter pointer, vibrato parameter (a speed table pointer), vibrato delay, gate-off timer, hard restart / first-frame waveform, then a 16-byte name. Up to 63 instruments.
- Four tables in the order wave, pulse, filter, speed; each: a row count `n`, `n` left bytes, `n` right bytes. Wave left: `00` leave, `01-0F` delay, `10-DF` waveform, `E0-EF` inaudible waveforms, `F0-FE` pattern command, `FF` jump; wave right: relative notes, absolute notes (`81-DF`), `80` keep frequency. Pulse left: `01-7F` modulation step (time; signed speed on the right), `8X-FX` set pulse width, `FF` jump. Filter left: `00` set cutoff, `01-7F` modulation step, `80-F0` set filter parameters, `FF` jump. Speed table: two bytes per entry, used for vibrato, portamento, funktempo.
- Patterns: a count byte, then each pattern is a row count `m` (up to 128) and `m` rows of four bytes: note (`$60-$BC` = C-0 to G#7, `$BD` rest, `$BE` key off, `$BF` key on, `$FF` pattern end), instrument (`$00-$3F`), command (`$00-$0F`), data byte. Commands: 1XY / 2XY portamento up / down, 3XY tone portamento, 4XY vibrato, 5XY / 6XY set attack-decay / sustain-release, 7XY set waveform, 8XY / 9XY / AXY set wave / pulse / filter table pointer, BXY filter control, CXY filter cutoff, DXY master volume, EXY funktempo, FXY tempo.
- Instrument files `.ins` (`GTI5`) carry the same instrument bytes plus the four tables.
- Sizes (arithmetic from the above): the instrument block is at most 63 x 25 = 1,575 B; each table at most 1 + 2 x 255 = 511 B (2,044 B for four); the pattern block is at most 208 x (1 + 128 x 4) = 106,704 B; typical songs are a few KB to tens of KB *estimate*.

### What maps (static tier)
| GoatTracker | Community OS |
|---|---|
| Three channels | Three tracks, each with a SID instance (the chip's one shared filter becomes one filter an instance; for a linear filter the same cutoff and resonance on every routed channel is equivalent, see `sid-instrument`) |
| Pattern rows (note, instrument, command, data) | Steps (note, instrument, FX) in 128-row patterns (the OS `Pattern` is 16 tracks x 128 steps, as in the `cc-automation` card); GoatTracker's 128-row maximum matches |
| Note `$60-$BC` (C-0 to G#7), rest, key off | Note range, empty step, note-off step; range to be checked against the OS note range |
| Order list per channel with REPEAT and TRANSPOSE | Unrolled: repeats expanded, transposes added to the notes; channels have independent pattern lengths and independent restarts, so the channels are laid on one common timeline and cut into fixed patterns; different loop lengths force unrolling up to a cap (the least common multiple can be huge): cap and report |
| Instrument AD, SR | Attack, Decay, Sustain, Release of the SID module: the 4-bit values are the SID's own, a direct, exact map |
| First wave-table waveform byte (`10-DF`) | Wave: `$10` triangle, `$20` saw, `$40` pulse, `$80` noise, sums for the combined waves; noise combined with another bit (not offered in the module) becomes noise |
| Pulse table: first "set pulse width", simple modulation step | Pulse Width, and PWM Depth and Rate as an approximation of a constant-speed modulation |
| Vibrato parameter through the speed table, vibrato delay | Vib Depth, Vib Rate, Vib Delay (approximate: units differ) |
| Wave table that only steps relative notes in a short loop | Arp 1, Arp 2, Arp Speed (the module's three-step arpeggio), only when the table is two or three notes; otherwise dropped |
| Tempo (ticks a row at 50 Hz): a row lasts tempo / 50 s | Song tempo: with four rows to a beat, BPM = 60 x 50 / (tempo x 4) = 750 / tempo, so tempo 6 gives 125 BPM; funktempo (alternating tempo) to swing, if the OS can express it |
| 1XY / 2XY / 3XY portamento and tone portamento | Slide and Glide FX where they exist (the module reads host slide and glide) |
| DXY master volume, FXY tempo | Volume and tempo FX where they exist |
| Subtunes | One project per subtune, or only subtune 0 with a note |

### What does not map (v1 reports it and drops it)
- General wave, pulse and filter table programs (waveform changes every frame, long relative-note sequences, `FF` jumps), the speed-table detail of vibrato and portamento, funktempo precision, the gate-off timer and hard restart (these exist to work around the chip's ADSR delay; the SID module does not have it unless that option is added), filter control commands (BXY, CXY) beyond a static cutoff, the chip's shared filter timing when channels interact, and any version difference of `GTS5` not checked (stereo or multi-SID versions: the readme read describes three channels).
- 50 Hz frame timing against the OS clock: rows are preserved; sub-row effects are not.
- Pattern commands that have no OS FX: dropped, one line each in an import report shown to the user.

## Cost on MK66 (estimate until measured)
- **Import:** a one-off parse on the main loop, not in the render path: reading at most about 107 KB from the SD card, unrolling to a project of 3 tracks. Estimated well under a second; RAM for the intermediate rows in SDRAM, freed afterwards (no allocation in the render path; an import is main loop only). Code: the reader and the mapper about 3 to 5 KB of flash *estimate* if in the core `formats/`, or 3 to 5 KB on the card if an importer module (the ABI has `O3M_KIND_IMPORTER`; whether it can read arbitrary SD files and write steps and instrument parameters is not verified here: an open check on `module/sdk/o3module.h` and `docs/guides/module-sdk.md`).
- **Playback:** only the SID instrument: about 4.9 % of a block per sounding voice *estimate* (see `sid-instrument`: 198 cycles x 64 = 12,672 of 261,120), so three channels = 3 x 4.9 = 14.7 % of a block *estimate*, plus the song's other content.
- Persistent format: the import writes ordinary native projects (`.o3pj`), no format change in tier 0. Tier 1 would add data (Level C).

## Fits
Layers: `formats/` (reader, if core) or an importer module (`modules/`), `model/` (project, patterns, undo), `app/` (an import entry in the file browser), `seq/` (tempo). No audio-path change. Depends on: `sid-instrument` (hard), the OS FX list (slide, glide, volume, tempo, arpeggio), the module ABI for importers (if done as a module).

## UX sketch
In the file browser a `.sng` shows up; choose Import: the OS shows "3 channels, 24 patterns after unrolling, 9 instruments; dropped: 14 table programs, 31 commands" and creates a new project with three SID tracks. The user keeps editing as any project. The report is the honest part: it names what was simplified.

## Licence
- The format is a binary layout documented in a GPL-2 project's readme; describing and implementing a file format is not a copy of its code. Write the reader from the format description; do not copy GoatTracker source (GPL-2, version not stated in the readme read: GPL-2-only would not mix with the OS's GPL-3.0-or-later).
- GoatTracker's documentation text is not copied here beyond the layout facts.
- SID-Wizard: licence unclear (Public Domain label on SourceForge against "WTF" in a fork's README); read the licence files before using anything from it.
- User songs imported from outside carry their own licences; the OS does not ship any (no test songs in the repository: `build-fixtures/` only, never committed).

## Risks / open questions
- **Lossy tier 0:** many C64 tunes depend on frame-rate tables; the import may sound much thinner than the original. Decide whether tier 0 alone is worth building or whether to wait for a table feature (Level C).
- **Timeline unrolling:** independent channel loops can blow up the song; pattern count, song length and total size limits of the OS project are not checked here.
- **FX coverage:** which GoatTracker commands have an OS equivalent is unchecked (list the OS FX first).
- **Tempo model:** funktempo and the PAL / NTSC difference (50 or 60 Hz) change the BPM mapping; the file does not say which chip clock the author used.
- **Instrument count:** GoatTracker has 63; the OS play API shows instruments 1 to 64 (`stop_note` in `o3module.h`); verify the project's instrument slots.
- **Format versions:** `GTS5` read from the v2.75 readme; 2.77 and any multi-SID format are not diffed.
- Evidence rule: everything is `new Community OS behaviour` or `open research question`; no parity claim with any original firmware.

## Priority
⭐2: a nice bridge to C64 music, but it depends on `sid-instrument` existing, the first version is lossy without per-frame tables, and the full version needs a new persisted-data feature (Level C); do it only after the SID instrument is on the unit and the maintainer wants songs, not just sounds.
