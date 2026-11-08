# track3r-os: product and UX design (draft)

Status: draft for maintainer review, 2026-10-10. Inputs: `docs/vision/community-os.md`, `docs/architecture/os-tree.md`, the radar and cards, ADR 0001 (look), ADR 0002 (SD modules), the parity rows (PAR-xxx).
Conventions: **[A]** assumption, not verified. **[P]** proposed control, not in today's firmware. Facts come from this repository's documents (the OG file formats and fixture survey, the hardware notes); release-note items (PAR-xxx) and the official manual (`docs/ui/official-manual-map.md`) are documented, not verified. "Est." numbers come from the radar cards and are unmeasured.

## 1. Product principles

1. **Hands only.** Every function works from pads, buttons and encoder on 800×480. The OS does not use touch [A].
2. **Pattern is home.** One press of Pattern returns to the grid. Transport, BPM, instrument, CPU and status stay in a top band on every screen.
3. **Keep the muscle memory.** The 11 screen buttons keep their places. New features are pages inside those screens (press again, or F-tabs), not new top-level modes.
4. **A note plays an instrument; a sample is only one source.** Engine, slices and modules belong to the instrument.
5. **Two layers deep.** Defaults first; depth is one tab or one Shift away; one overlay at a time.
6. **Never lose work.** Autosave and undo. A missing or crashed module mutes its instrument and names the file (ADR 0002); the project still loads.
7. **Cost is visible.** Engines, filters and FX show a CPU badge; the OS warns before audio drops out.
8. **Flash holds the core; the SD holds the character.** Engines, effects, importers, themes are SD modules or packs.

## 2. Core model

```
PROJECT   settings · sample pool · 48 instruments · routing · MIDI maps
 └ SONG     playlist of pattern slots (OG: 255)
    └ PATTERN   up to 255; each track has its own length
       └ TRACK    typed Audio | MIDI | External
          └ STEP    note · instrument · FX1 · FX2   (FX = type byte + value byte)
INSTRUMENT = engine > source (sample | slices | wavetable | none) > filter > modulation > FX/sends > output
```

Facts from the OG files: patterns of 8 tracks (12 in the 1.9 layout), 48 instrument files, at most 48 slice points per instrument. The release notes document four MIDI-only tracks added in 1.9 (PAR-143: documented, not verified).

- **Instruments.** Keep 48 slots; pad n selects instrument n as today [A]. Instruments reference a shared **sample pool** (many instruments, one sample). Engines: the OG sampler with its eight play modes, Amiga mode, and module engines (303, FM, drums).
- **Voice pool vs fixed tracks. Recommended: logical tracks over a voice pool.** Launch with 12 tracks (8 Audio + 4 MIDI at first, the 1.9 pattern layout, so OG projects map 1:1; OS-TRK912 later made tracks 9-12 ordinary tracks); ceiling 16 after measurement [A]. A track is monophonic by default (a new note cuts the old one, as in a tracker); optional "tail overlap" lets the old release finish on a spare voice. USB stems and sends use **stem buses**, not tracks, so the 10-channel full-speed USB limit does not cap track count (radar card `usb-multichannel-audio`).
  - Alternative A: 8 fixed tracks. Simplest, no migration, but no polyphony or typed tracks.
  - Alternative B: fully polyphonic pool, no tracks. Flexible, but not a tracker, and CPU is unpredictable.
- **FX placement.** Voice: filter, drive, bit depth. Track insert: up to 2 module slots [A]. Sends: delay and reverb as today. Master: the chain of the release-note items PAR-055 to 058, 109 (documented, not verified).

## 3. Physical controls map

The control layout of the Tracker hardware as this design assumes it (a role marked [A] is an assumption; the official firmware's roles are as the official manual describes them, `docs/ui/official-manual-map.md`: documented, not verified), and the module button tables.

| Control | Global role | Context roles | Shift combos |
|---|---|---|---|
| **Pads ×48**: 4×12 key matrix, per-pad PWM LED. No velocity or pressure: pad presses carry velocity 0 [A, inferred] | Play the current instrument on the scale layout (the pad board) | Pattern: enter note; pick instrument (INSTR column); FX value (PAR-117). Pattern held: pad = pattern (PAR-015). Chop: pad n = slice n. Song: pick pattern. Name entry: letters | Shift+pad: select without sounding [P] |
| **F1–F8** | Soft keys; labels in the bottom strip | Pattern: Pattern, Length, Step jump, Fill, Preview, Invert, Export, Undo. Instrument: parameter select, tabs in the OS. File, Recorder, Song: actions | Pattern: mute track 1–8 [A]. Mixer: solo [P] |
| **11 screen buttons**: Params, Performance, File, Config, Master, SampleLoad, SamplePlay, SampleEdit, SampleRec, Song, Pattern | Jump to the screen | Press again flips the page; Params, Song and Master already do. The OS extends this to a page cycle | Shift+button: quick overlay; Shift+Config: Learn [P] |
| **Column buttons** Note, Instr, "Vol" (FX1), "Fx" (FX2) | Choose the step column | Note+Pattern: 8-track view. Hold FX + encoder: FX type. Other screens: hold Note for the note popup | none |
| **Arrows** | Move cursor or list selection | Pattern: cursor. Lists: scroll | Select range; Focus Loop (PAR-156) |
| **Encoder** (resolution 24, acceleration 3) | Change the value under the cursor | Pattern, not editing: scroll steps. Lists: scroll | Coarse step, octaves (PAR-052, 137) |
| **Encoder push** | Not a control in this design [A] | Not used [A] | none |
| **Play, Rec** | Play/stop; Rec = edit mode, then record arm | Rec+Play: record. Song: F1/F2 play pattern/song | Shift+Play: from cursor, loop range |
| **Copy / Insert / Delete** | Copy / insert step / clear | Instrument, Recorder: Delete removes | Paste / Home / Backspace |
| **Power** | Sleep/power | none | Shift+Power: screenshot |

Inputs, not controls: SD slot, MIDI DIN in/out, USB, line in/out, and mic and FM radio as recorder sources.

## 4. Screen map

ADR 0001 look: flat panels, 1 px borders, monospace data, one strong row highlight, fixed palette. Mock-ups are scaled, about 10 px per character [A].

| Screen | Purpose | Reach |
|---|---|---|
| **Pattern** (home) | Step grid, record, 8-track view; page 2 horizontal, page 3 names (PAR-074, 135) | Pattern |
| **Song** | Playlist slots, play pattern/song, undo | Song; again: timeline |
| **Instrument** | Tabs F1 ENGINE, F2 FILTER, F3 ENV, F4 LFO, F5 MOD, F6 NOTE FX, F7 FX/SENDS, F8 OUT | Params |
| **Wave / Chop** | Start, loop, end, zoom; CHOP page for slices | SamplePlay; again: CHOP |
| **Sample edit** | Offline crop, EQ, overdrive, Amiga prep, undo | SampleEdit |
| **Sample load** | SD browser, preview, import, `.pti` library | SampleLoad |
| **Record** | Sources (line L/R, mic, radio, USB); pages RESAMPLE, RADIO | SampleRec; again: pages |
| **Mixer** | Level, pan, mute, solo, meters for tracks and buses | Master |
| **Routing** | Source × destination matrix, OUT mode, USB stems | Master, 2nd press |
| **Master FX** | Saturation, limiter, EQ, bass, space | Master, 3rd press |
| **Perform** | Live pattern triggers and FX | Performance |
| **File** | Project new/open/save/export; pages IMPORT, MODULES | File |
| **Config** | MIDI (clock, input routing), USB mode, display, SD, firmware | Config |
| **Learn overlay** | Binding list, current target | Shift+Config [P] |

**Pattern**

```
+------------------------------------------------------------------------------+
| PAT 03/12  LEN 064  JUMP 04  128.0 BPM  [>] SONG 05   INS 01 KICK    CPU 31% |
+--+--------------+--------------+--------------+--------------+---------------+
|  | T1 A KICK    | T2 A SNARE   | T3 A ACID    | T4 M SYNTH   | T5 E LINE L   |
+--+--------------+--------------+--------------+--------------+---------------+
|00| C-2 01 --- --| --- -- --- --| C-1 03 L40 A0| C-3 09 --- --| --- -- --- -- |
|01| --- -- --- --| --- -- --- --| --- -- --- --| --- -- --- --| --- -- --- -- |
|02| --- -- --- --| D-2 02 V90 --| D#1 03 L55 S1| --- -- --- --| --- -- --- -- |
|03| --- -- --- --| --- -- --- --| --- -- --- --| E-3 09 --- --| --- -- --- -- |
|04|>C-2 01 V80 --| --- -- --- --| C-1 03 L30 --| --- -- --- --| --- -- --- -- |
+--+--------------+--------------+--------------+--------------+---------------+
| scale A min   edit ON   track 3 mono   tail overlap off           undo 12/64 |
|F1 PATTERN|F2 LENGTH|F3 JUMP|F4 FILL|F5 PREVIEW|F6 INVERT|F7 EXPORT|F8 UNDO   |
+------------------------------------------------------------------------------+
```

**Instrument (engine 303 from an SD module)**

```
+------------------------------------------------------------------------------+
| INSTR 03 ACID   ENGINE 303 [tb303.tmod 1.0]   TRACK 3 mono            CPU 6% |
+--------------------+---------------------------------------------------------+
| 01 KICK            | CUTOFF [########------------] 1.2 kHz  mod ENV1 CC74    |
| 02 SNARE           | RESO   [##############------] 72 %                      |
| 03 ACID        <   | ENVMOD [#########-----------] 45 %                      |
| 04 LEAD  sampler   | DECAY  [######--------------] 210 ms                    |
| 05 --              | ACCENT [############--------] 60 %                      |
| ... 48 slots       | SLIDE  [####----------------] 60 ms                     |
+--------------------+-----------------------+---------------------------------+
| filter response       ____                 | MOD ENV1 > CUTOFF   +40         |
|                 ____/    \___              | MOD CC74 > CUTOFF   +25 LEARNED |
+--------------------------------------------+---------------------------------+
|F1 ENGINE|F2 FILTER|F3 ENV|F4 LFO|F5 MOD|F6 NOTE FX|F7 FX/SENDS|F8 OUT        |
+------------------------------------------------------------------------------+
```

**Mixer and routing (Master, page 2)**

```
+------------------------------------------------------------------------------+
| MASTER > ROUTING   OUT MODE [MAIN L / CUE R]   USB [AUDIO+MIDI]      CPU 38% |
+------------+-----+------+------+------+------+------+------+-----------------+
| SOURCE     | lvl | MAIN | CUE  | SNDA | SNDB | REC  | USB  | meter           |
+------------+-----+------+------+------+------+------+------+-----------------+
| T1 KICK    | 80  |  x   |      |  .   |      |      |  S1  | |||||||         |
| T3 ACID    | 64  |  x   |  x   |      |  x   |      |  S3  | ||||||||        |
| LINE L     | 90  |  x   |  x   |      |      |  x   |      | ||||            |
| LINE R     | 90  |      |      |      |      |  x   |      | |||             |
| RADIO      | 50  |      |  x   |      |      |  x   |      | ||              |
| USB RETURN | 70  |  x   |      |      |      |      |      | |||||           |
+------------+-----+------+------+------+------+------+------+-----------------+
| FX loop: OUT R = send, LINE IN = return, latency compensated [A]             |
|F1 MIXER|F2 ROUTING|F3 MASTER FX|F4 STEMS|F5 CUE LVL|F6 PRESET|F7 MUTE|F8 SOLO|
+------------------------------------------------------------------------------+
```

**Chop (SamplePlay, page 2)**

```
+------------------------------------------------------------------------------+
| SAMPLE 12 BREAK_A   CHOP 16 slices   AUTO transient   ZC SNAP ON      CPU 8% |
+------------------------------------------------------------------------------+
| 1    |2   |3  |4     |5   |6  |7    |8   |9   |10 |11   |12  |13|14|15|16    |
| #=#-.|-.#=|=#.|-#=-#.|-#.#|#=#|-.#=#|=#.-|-#=-|-#.|#=#-.|-.#=|=#|-#|-#|#=#-.#|
| selected slice 05 highlighted; markers snap to zero crossings                |
+------------------------------------------------------------------------------+
| SLICE 05  START 021450  END 026012  PITCH +0  REV off  LOOP off  VOL 100     |
+----------------------------+-------------------------------------------------+
| pads (LED mirror)          | Encoder nudges START. Pad n plays slice n.      |
| 01 02 03 04 05 06 07 08 .. | Shift+pad selects without sounding.             |
+----------------------------+-------------------------------------------------+
|F1 AUTO|F2 EVEN|F3 ZC|F4 ADD|F5 DEL|F6 REV|F7 LOOP|F8 BACK                    |
+------------------------------------------------------------------------------+
```

## 5. Key workflows

F-key labels are proposed [P].

**1. A beat from a break, with chops**
1. SampleLoad: encoder picks `break.wav`, F5 previews, F4 adds it as the next free instrument.
2. SamplePlay twice for CHOP. F1 AUTO runs transient detection (a slice detector); F2 EVEN sets n slices.
3. Pads now play slices. Encoder nudges the slice start (ZC snap on); F6 reverse, F7 loop per slice.
4. Pattern: Rec, Play, hit pads in time. Slices are ordinary notes (slice k = key k [A]), so editing is normal.
5. Fill (F4) and Euclid (PAR-053) thicken it; Shift + a top-row pad mutes that track (1–12), and Shift + a second-row pad solos it.

**2. An acid line with the 303 module**
1. File, MODULES: confirm `tb303.tmod` is listed (from `/modules/instruments/`, ADR 0002).
2. Params: pick an empty slot with a pad, F1 ENGINE, encoder to "303". Absent modules show greyed.
3. Pattern: assign it to a track; play on pads or step-enter.
4. Accent and slide are engine-declared step FX (decision 2): hold FX1, turn the encoder to "A Accent" or "S Slide". The OG cutoff FX still works.
5. Params, F5 MOD: route ENV1 and a learned CC74 to cutoff.

**3. MIDI Learn a knob**
1. On any parameter, Shift+Config opens the Learn overlay; the cursor parameter is highlighted.
2. Move the knob. The CC binds (default pickup, no jumps).
3. Encoder sets the range; F-key CLEAR unbinds; Shift+Config exits.
4. The map saves in the project or the device (decision 8).

**4. Record stems over USB**
1. Config, USB: AUDIO+MIDI (applied on re-attach by a confirmed press, F7 twice; the default is AUDIO since OS-USBAUDIO, maintainer decision 2026-10-13). 10 channels: stems 1–8 pre-pan plus master L/R, 16-bit (est.).
2. Routing, F4 STEMS: assign tracks to stem buses (default T1–T8 to S1–S8).
3. Arm the inputs in the DAW; start there or press Play. The stereo return is source "USB RETURN".
4. Fallback: File, export stems to SD.

**5. Resample a track**
1. Cursor on the track; SampleRec, page RESAMPLE.
2. Pick the tap (track post-FX, bus, master, input) and mode LIVE or BOUNCE (offline render, faster than real time; PAR-054).
3. The result becomes a new instrument; AUTO-CHOP optional. FREEZE also mutes the source and keeps it, so UNFREEZE restores.

**6. Import an OctaMED song**
1. Put `song.med` in `/Import/` on the SD.
2. File, IMPORT: encoder picks, F-key confirms, then a report of dropped effects, folded channels and remapped instruments.
3. Samples keep their native rate with notes transposed (radar import card); instruments can switch to the Amiga engine.
4. Release 1 uses a desktop converter; the on-device importer is an SD module later.

**7. Load a module from the SD**
1. Copy `*.tmod` into `/modules/instruments|effects|importers/` with a card reader (the release notes document MTP as disabled in the official releases, PAR-086: documented, not verified).
2. File, MODULES: kind, version, ABI check, est. RAM/CPU; F-key LOAD/UNLOAD. A crashed module is red and quarantined; a button held at boot starts without modules (ADR 0002).
3. It appears in the ENGINE list, FX picker or IMPORT.
4. If it later goes missing, the instrument mutes and the file is named.

## 6. How each maintainer feature lands

| Feature | Screens | Controls | Depends on |
|---|---|---|---|
| Instruments apart from samples; engine per instrument | Instrument, Sample load | Pad = instrument, F1 | Voice pool, engine API, format v2 |
| Sample chops on pads | Chop, Pattern | Pads = slices, F1/F2, encoder | Slice definition (vision 5); existing detector |
| Resample / bounce / freeze | Record RESAMPLE | F-keys, Rec | Routing taps; render (PAR-054); D-OS-10 |
| Routing: dual-mono in, FX loop, main + cue | Routing | Arrows, encoder | Codec dual-input and latency check [A]; line-in channels exist (PAR-076); cue shares the stereo line out [A] |
| MIDI CC in, MIDI Learn | Any, Learn overlay | Shift+Config | Parameter registry; vision 8 |
| Mod matrix | Instrument F3–F5 | Arrows, encoder | Parameter registry; vision 4 |
| MIDI processor (arp, chord, scale, Euclid); MPE dropped (maintainer, 2026-10-11) | Config MIDI; Instrument MOD, F6 NOTE FX | encoder | MIDI input path; mod matrix; the FX of release-note items PAR-038, 078, 053 (documented, not verified) |
| Radio harvest | Record RADIO | Encoder seeks, F-key CAPTURE n bars | Si4703 driver; radio source; chop |
| Audio to modulation | Instrument MOD; Routing | Arrows, encoder | Line-in; follower cheap, pitch moderate (est.) |
| USB stems + return | Config USB, Routing | F4 | USB audio; 10-channel ceiling |
| Amiga mode; filter set, ladders | Instrument ENGINE, F2; Sample edit | encoder | Engine v2; format field; ladder module |
| MED/MOD/XM/IT import | File IMPORT | encoder, F-key | Converter, then importer module |
| Logical tracks | Pattern headers, Mixer | F-key retypes | Voice pool; format v2 |
| SD modules | File MODULES | F-keys | ADR 0002 bench |

## 7. Proposed answers to the open decisions

| Decision | Recommendation | Reason | Process |
|---|---|---|---|
| D-OS-8 Engine rate | **Decided (maintainer, 2026-10-23): the engine runs at exactly 44,100 Hz by default in release 1** (MCLK = 180 MHz x 196/3125 = 11.2896 MHz, `1800000/441` CPU cycles per sample; OS-RATE). The pre-1.0 44,117.65 Hz clock stays buildable with `-DO3OS_RATE_44100=OFF`. This reverses the earlier "keep 44,117.65 Hz in release 1" | **verified on the unit** (image fqa.4r, dev 5ff06aed, 2026-10-23): the true clock measured 44,100.00 Hz against 44,117.58 Hz on fqa.3 (the old clock); USB audio ran 60 s with no xrun or click; the CPU is unchanged; the MIDI clock is its own timer (48.0006 ticks/s on both). The official firmware's own rate is still an open research question (`docs/hw/tracker-hardware-notes.md`) | Decided |
| D-OS-9 Retire v1 | Freeze v1 when the new OS opens all fixture OG/1.9 projects, passes a 30-minute soak and the seven workflows, and has been default for one hardware cycle | Measurable exit | Maintainer yes/no |
| D-OS-10 SDRAM map | No overlap of sample pool and record/edit bank (how the official firmware lays out its sample memory is an open research question). Reserve a module slab | Live resample and freeze need both at once | Panel ADR |
| D-OS-11 UI swap | Superseded by D-OS-1: new tree, whole-UI switch | No v1 adapter needed | Maintainer yes/no |
| 1 Tracks | 12 typed tracks over a voice pool, ceiling 16 | Maps OG 1:1; stems decoupled by buses | Panel ADR |
| 2 FX commands | Keep two columns of type byte + value byte. Types 0–42 keep the OG type numbers and bytes; 43–127 core; 128–255 declared by the instrument's engine [A: needs format check] | Keeps OG data; solves 303 accent/slide IDs | Panel ADR |
| 3 Engine, FX scope | Engine switchable per instrument; shared params persist, engine-only params are stashed. FX placement per section 2 | Experiments keep sample, mods, sends | Panel ADR with 1 |
| 4 Modulators | Per instrument 4 LFO + 4 envelopes (the six envelope slots of an OG instrument file map in); sources MIDI velocity, key, macro, CC, bend, MIDI aftertouch, input follower (the pads sense neither); 16 routes. Every non-structural numeric parameter is a target and learnable | One registry serves mod, learn, FX | Panel ADR |
| 5 Slice | The sample owns slice markers; the instrument owns per-slice overrides (pitch, reverse, loop, vol, pan, filter) by index. Max 48 in release 1 | One pad each; markers travel with the sample | Maintainer yes/no |
| 6 Undo | Pattern/song undo of at least 10 levels (PAR-068 release note; the manual says up to twenty pattern edits: not verified) plus a project command journal in about 1 MiB SDRAM, 64 steps [A]. Sample edits keep snapshots | Bounded memory; covers editing, not performance | Panel ADR |
| 7 1.9 projects | *Superseded by ADR 0006: Export → OG exists.* Load natively; convert once on first save; keep the original | Avoids a round-trip burden | Maintainer yes/no |
| 8 MIDI Learn | Two stores: device (transport, mixer) and project (parameters). "Follow selected instrument" is a target mode, not a third store. Default pickup; jump or relative per binding | No level jumps; simple model | Maintainer yes/no |
| 9 Licence | Core GPL-3.0-or-later, module SDK MIT (decided; see `THIRD_PARTY.md`). No code from the official firmware is carried over | Radar needs only permissive code (Open303, pt2-clone, libxmp) | Panel ADR |
| 10 Module ABI | Accept ADR 0002. Add parameter descriptors (name, range, unit, learnable, mod target) and step-FX declarations. Modules do not draw UI; the OS draws their page | Consistent look (ADR 0001); safety | Panel ADR after the ADR 0002 bench |

Panel ADR = record an ADR. Maintainer yes/no = one answer, no panel. ADR 0002's own three questions stay maintainer yes/no.

## 8. Questions for the maintainer

1. **Track model.** (a) 8 fixed. (b) 12 typed over a pool, ceiling 16 after measurement, **recommended**. (c) 16 from day one.
2. **Step format.** (a) 2 FX columns, type + value, in the OG layout, **recommended**. (b) 3 FX columns. (c) 2 FX plus a parameter-lock lane.
3. **OG projects.** (a) Load natively, convert on first save, never write OG back, **recommended**. (b) Load only. (c) Also keep saving OG format.
4. **MIDI Learn default.** (a) Pickup, device + project stores, **recommended**. (b) Jump. (c) Project store only.
5. **Licence.** (a) MIT core + SDK, **recommended**. (b) Apache-2.0. (c) GPLv3 core, MIT SDK.
6. **First release headline.** (a) Chops, resample, routing, MIDI Learn (core only), **recommended**; the 303 follows once the ABI bench passes. (b) 303 and modules first. (c) USB stems first.
7. **Encoder push.** Does your unit's encoder have a push switch? (a) Yes. (b) No. (c) Unsure: design without it, **recommended**.
8. **Learn gesture.** (a) Shift+Config, **recommended**. (b) A dedicated gesture on Rec. (c) Learn only inside Config.

### Orchestrator decisions (2026-10-11)
- Shift+F1..F4 jump to 0/4..3/4 and Shift+F5 jumps to the end (maintainer: OctaMED-style jumps, OS-002b). Track mute moves to Shift + top-row pad (tracks 1–12) and solo to Shift + second-row pad. The P1 slice implements both.
- F8 closes overlays (Back): the hardware has no Back button.
- Identical notifications (same text and source) within their TTL coalesce into one with a count.

### Maintainer answers (2026-10-10)
- Q1 tracks → 12 typed over a voice pool (ADR 0003). Q2 step → 2 FX in the OG layout + engine FX (ADR 0004). Q5 licence → GPLv3 core, MIT SDK (ADR 0005).
- Q3 OG projects → "compatible with the OG": open natively, save in the OS format, explicit Export → OG (ADR 0006).
- Q7 encoder push → **no push switch**. No workflow may depend on it.
- Q8 Learn gesture → **press FX1 + FX2 together** (the two FX column buttons), not Shift+Config.
- Q4 and Q6 delegated to the maintainer; the recommended options apply. MIDI Learn uses pickup mode and is stored per device and per project. The first release headline is chops, resample, routing and MIDI Learn (core); the 303 module follows once the ABI bench passes.
