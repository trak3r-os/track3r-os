# Feature radar

The matrix of researched ideas (the `radar` skill). One card per idea in `docs/vision/radar/`. The first rows are imported from the initial radar of `docs/vision/community-os.md` (§Feature radar); they have no card yet. Every number is an estimate until a hardware session measures it.

| Idea | Inspiration | OSS (licence) | CPU | RAM | Flash | Lives in | UX value | Priority | Card |
|---|---|---|---|---|---|---|---|---|---|
| MIDI CC in / MIDI Learn / mod matrix | OpenMPT, Renoise (concepts) | own code | — | — | — | — | universal CC/NRPN → any parameter, learn mode | very high, ~0 CPU | — |
| Sample chops | Amigo (commercial, product reference), MilkyTracker, Renoise tools | own code | — | — | — | — | equal / transient / zero-cross / manual slicing, 2–64 slices on pads, per-slice start/end/pitch/reverse/loop/vol/pan/filter; analysis offline at load | very high | — |
| USB stems + return | Teensy USB audio | Teensy core (MIT-style) | — | — | — | — | 8 tracks out, stereo in, MIDI | very high, bandwidth-bound | — |
| Realtime resample / bounce / freeze | — | own code | — | — | — | — | any track/submix/master/input → new instrument, auto-slice; frees CPU | very high | — |
| Routing: dual-mono in, FX loop, main+cue | maintainer request history | own code | — | — | — | — | L/R as independent channels with sends; OUT R = send, LINE IN = return, latency compensation; L = main, R = cue | very high | — |
| ProTracker / Amiga playback mode | 8bitbubsy pt2-clone | BSD-3 (verify) | — | — | — | — | Paula-like resampling, 8-bit quantisation, A500 RC + LED filters, hard pan | high | — |
| TB-303 engine | Open303 (MIT core); JC-303 extensions GPLv3 | use Open303 core only | — | — | — | — | mono saw/square, env, resonant filter, accent, slide via tracker FX | high | — |
| Moog/ladder filters | MoogLadders (per-file licenses: Unlicense/ISC/BSD…) | pick permissive files | — | — | — | — | selectable ladder variants, benchmark on MK66 | high | — |
| Filter set | Q DSP, Surge (concepts) | own/permissive | — | — | — | — | SVF LP/HP/BP/notch, LP12/24, comb, drive | high | — |
| MED/OctaMED import | libxmp (MIT since 2025), OpenMPT MED loader (BSD) | MIT/BSD | — | — | — | — | MMD0–3 parser → offline conversion to internal model | high/medium | — |
| MOD/XM/IT import | libxmp, OpenMPT | MIT/BSD | — | — | — | — | offline conversion | medium | — |
| Amiga sampler workflow | Ami-Sampler-VST (GPLv3) | reference only | — | — | — | — | decimation, A500 filter, voice modes | medium | — |
| Vocoder | — | own code | — | — | — | — | mic modulator, 8–12 bands to start, carrier = wavetable/saw/303 | medium (CPU) | — |
| Audio → modulation | — | own code | — | — | — | — | envelope follower (cheap), transient trigger, mono pitch detect (moderate) | high | — |
| Radio harvest | — | own code | — | — | — | — | scan → capture N bars → auto-chop → pads; random-station collage | high, unique | — |
| Chip/FM engines | Furnace (GPLv2+) | study only | — | — | — | — | ideas for 2-op FM, SID-ish, NES/GB/AY, noise drums | medium | — |
| MPE input, MIDI processor | — | own code | — | — | — | — | per-note expression; arp/chord/strum/scale/probability/Euclid before MIDI OUT | high | — |
| SD-loadable modules | RETracker precedent | own ABI | — | — | — | — | `/modules/*.tmod` loaded to SDRAM on demand | high impact, needs research | — |
| Logical tracks | — | own design | — | — | — | — | 12–16 tracks typed Audio / MIDI / External Audio over a voice pool | high | — |
| Convolution reverb, 64-voice synth | — | — | — | — | — | — | — | rejected (CPU/RAM) | — |
| CC automation per track (`cc-automation`) | Deluge community firmware, Seq66, OpenMPT (concepts) | own code (Seq66 GPL-3 and Deluge GPL-3 studied, OpenMPT BSD-3) | about 0.4 % at the DIN limit of about 1,040 msg/s; 1 % only at about 2,700 events/s (USB) *estimate* | SRAM about 192 B; lane events in SDRAM | 4 to 6 KB *estimate* | core flash (sequencer, MIDI out) | movement over time on external synths | ⭐4 | [card](radar/cc-automation.md) |
| Several MIDI notes a track, finer than a step (`midi-poly-record`) | DAW piano rolls, Seq66 | own code | about 300 cycles a lane event *estimate* | SRAM about 544 B (held notes, cursors); lanes in SDRAM | — | core flash (sequencer, model, record sink) | track economy for MIDI users; REC POLY Spill, REC MICRO and FX 14 already cover much of it | ⭐3 | [card](radar/midi-poly-record.md) |
| Event-list editor for a MIDI track (`midi-event-list`) | old hardware sequencers, Seq66 event editor | own code | UI only, main loop *estimate* | small | 6 to 10 KB *estimate* | core flash (UI) | precise editing; phase 0 lists existing steps with no format change | ⭐3 | [card](radar/midi-event-list.md) |
| SID-style instrument (`sid-instrument`) | MOS 6581 / 8580 voice, GoatTracker, LMMS Sid | own code, MIT like Chip (reSID / reSIDfp GPL-2+ as listening references only) | about 198 cycles a sample, 4.9 % of a block a voice (3.9 % with the filter off) *estimate* | instance about 320 B; hot part 5.5 to 6.5 KB of the 14 KiB module slab *estimate* | 0 core; 14 to 18 KB on the card *estimate* | SD native plugin (`sid.tmod`, Chip unchanged) | a new voice: saw, triangle, pulse with PWM, noise, combined waves, sync, ring, stepped ADSR, LP/BP/HP filter | ⭐4 | [card](radar/sid-instrument.md) |
| GoatTracker song import (`goattracker-import`) | GoatTracker 2 `.sng` | own reader from the format description (GoatTracker GPL-2, no code) | import on the main loop; playback = the SID instrument, 3 tracks about 14.7 % *estimate* | intermediate rows in SDRAM, freed | 3 to 5 KB *estimate* | core flash (`formats/`) or an importer module | C64 songs as projects; lossy without per-frame tables (those need a Level C data feature) | ⭐2 | [card](radar/goattracker-import.md) |
