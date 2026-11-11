# OS-MIDIREC-P1: MIDI lanes, CC automation and poly recording

Status: design, 2026-11-10. Level C (a persistent format: the `MEVT` chunk of `.o3pj`; the sequencer's render path). The maintainer unparked the row on 2026-11-10. Read-only study of `dev` (f1599080). No code in this document. Radar cards: `docs/vision/radar/cc-automation.md`, `midi-poly-record.md`, `midi-event-list.md`. Phase 0 (the EVENTS list over steps): `docs/specs/OS-MIDIREC.md`. Neighbour: `docs/specs/OS-PARAMAUTO.md`. All numbers marked [est] are estimates to be measured on the unit before the slice that spends them merges. Every behaviour here is "new Community OS behaviour" unless it says otherwise; no parity with the official firmware is claimed (the official firmware's handling of this was not studied).

## Decisions for the maintainer

Every other choice is made below as an assumption (A-) and is reversible. Plain table, copy and answer by number.

| # | Question | Options | Recommendation |
|---|---|---|---|
**Answered by the maintainer 2026-11-10:** 1c (REC STORE Grid/Lane, default Grid), 2a, 3a (the maintainer: older firmware is no concern, no marker), 4a, 5b stored / a recorded in v1, 6a, 7a, 8b. Also answered: a length cut deletes the events past it (A17), no MIDI thru while recording, the pads record notes only (A13), the grid mark of "What the pattern grid shows". Lanes on non-MIDI tracks (the maintainer took the recommendation, 2026-11-10): not in v1. On a sample or module track a Learn-mapped CC records into the grid as a step FX (the param's own FX, or CC A..F with OS-PARAMAUTO), one value a step; an unmapped CC is not recorded. A later slice S8 may add sub-step parameter lanes there (storing the parameter, not the CC number; audio-path cost measured on the unit). MIDI thru: as today (live notes re-sent on the track instrument's channel and port), no raw MIDI IN to OUT echo.

| 1 | Chords and sub-step notes: keep Spill + Micro-move in the grid, or lanes with a mark in the grid? | a) grid only: Spill + Micro-move stay the only way, lanes carry CC only · b) lanes replace Spill and Micro-move on MIDI tracks · c) both: a new device setting REC STORE (Grid or Lane), the grid shows a mark for lane events | c, default Grid. Nothing changes until you pick Lane; OG export and old firmware keep the chord as ordinary steps; Lane gives a chord on ONE track and 1/48-step timing with true note lengths. Revisit the default after unit use. |
| 2 | What the OG `.mtp` export does with lane events | a) drop them and count them in the export report (as core FX are today) · b) bake them to steps and step FX where they fit (lossy: one note a step, CC on FX columns only) | a (your 2026-11-05 note: no preference; this stays the rule). b can be a later verb. |
| 3 | A project with lanes opened and saved by an OLD firmware (no `MEVT` support) | a) accept: old builds keep a chunk of 16 KiB or less as an unknown chunk and write it back, a bigger one is dropped and lost on that save (counted, no error) · b) also write a small marker so the old build can warn | a. The `LAW` chunk has the same property; a marker needs a code change in builds that no longer ship. The format docs and the project-load notice (new build) say it. |
| 4 | Step events and lane events on one track | a) both play: step row events first, then lane events at the same tick; a lane note never cuts a step note · b) a track is either step-based or lane-based (a per-track mode) | a. No mode to learn; a lane is just more events. Cost: a track can sound two things at once, shown by the grid mark. |
| 5 | Event types stored in v1 | a) notes and CC only · b) also pitch bend, channel and poly aftertouch, program change | b for storage (the 8-byte event has room; the reader accepts them), a for recording UI in v1: S4/S5 record notes and CC, pitch bend and aftertouch recording and playback follow as one extra slice when you ask. 14-bit CC pairs and RPN/NRPN stay out. |
| 6 | Lane memory | a) 192 KiB pool in SDRAM, 24,576 events in all, 4,096 per lane · b) 128 KiB · c) 256 KiB. Decision 2026-11-10, delay spare: 60 KiB pool, 7,680 events in all, 4,096 per lane, in the delay region's unused spare after the 4.0 s ring (not in the sample arena); the notes below that name 192 KiB / 24,576 are the original sizing | a. About 1.2 % of SDRAM, from the 2.9 MiB beyond the sample budget (which stays 11.25 MiB). A full pool refuses the take with a message, never overwrites. |
| 7 | Recording thinning and the DIN rate cap | a) fixed: CC deadband 1, at most one point per controller per 1/4 step (12 ticks), 300 CC messages a second out on DIN, notes exempt · b) a device setting for each | a. The user has no use for the knobs; they can come later. |
| 8 | Which incoming CCs a take records | a) only CCs the encoder, the pads and learned knobs send (resolved to the track instrument's controller) · b) a), plus every CC an external controller sends on the track's channel | b. A hardware keyboard's mod wheel and knobs are the main source; the channel filter keeps other tracks' CCs out. |

## Goal and what the user does

A MIDI instrument track gets a LANE: a time-ordered list of events at 1/48 step (192 PPQN at four steps a beat) next to its steps. A take records into it: CC moves from the encoder, the pads and an external controller (part a), chords and sub-step notes with true lengths (part b). The EVENTS list of phase 0 shows and edits the lane rows (part c).

1. Set the device setting REC STORE to Lane (Settings; default Grid). Choose a MIDI instrument for the cursor's track (the one the track's rows use).
2. LIVE (Shift+Rec) with the song playing: play a chord on the connected keyboard. The notes land on the cursor's track at the tick they were played, with the held time as LEN. Turn the encoder with a Learn target (OS-ENCLEARN) or move an external knob: the CC points land at their tick. Stop or the Rec key ends the take.
3. The grid shows a lane mark in the steps the lane starts events in (next section). Play: the lane plays with the steps.
4. Undo (one key) removes the whole take; Redo brings it back.
5. STEP mode (Rec, stopped): notes held together are one chord at the cursor step's first tick, with a LEN of one step (editable in EVENTS); a Learn knob turn writes one CC point at the step's first tick.
6. F7, EVENTS: the list shows the lane rows beside the step rows; the user edits TIME, CH, DATA, VAL, LEN there.

## What the pattern grid shows (the maintainer's question, 2026-11-10)

**Today (facts).** REC POLY Spill sends the notes held together on to the next tracks, one a voice (`app/include/o3os/app/rec_place.h:9`, `:48`, `:87`); REC MICRO writes the offset into FX 2 as a Micro-move (FX type 2, a delay of 0..100 of a step, 48 microsteps). Both put ordinary steps in the grid: a chord uses as many tracks as it has notes, the timing is a delay only and takes the FX 2 column, a length is a whole step (an OFF row). Everything is visible and exports to OG `.mtp` unchanged.

**With a lane (REC STORE = Lane, MIDI instrument only; any other instrument records as Grid).** The grid stays a step grid; lane events never occupy a step column and never overwrite one.

The grid design is the maintainer's (2026-11-10). A step is in one of four states:
"starts" (a lane note starts in it), "held" (a lane note that started earlier is still sounding in it; it ends in the step that holds the last note off), "CC" (it holds lane CC events), or "none".
A CC has no length: a step between two CC events is "none", because the controller keeps its last value.

- **Text, only when the step's own columns are empty.**
  - A "starts" or "CC" step reads `MID nn cc`. `MID` is in the dim lane colour, in the note column. `nn` is in the instrument column, in the lane-note colour: the number of lane notes that start in the step. `cc` is in the volume column, in the lane-CC colour: the number of lane CC events in the step. Both are two digits, `00`, and capped at `99`.
  - A "held" step without CCs reads `MID` only.
  - A step with its own note, instrument or volume shows no text: what the step holds wins, and only the bar is drawn.
  - In a zoom that shows only the note column, the text is just `MID`.
  - The text is for display only: the cursor on it edits the step, never the lane.
- **Bar, on every step that is not "none"**: 2 px at the cell's left edge. The top half is for notes and the bottom half for CCs.

  | Step | Top half (notes) | Bottom half (CC) |
  |---|---|---|
  | starts | lane-note colour, full width | lane-CC colour if the step has CCs, else empty |
  | held, no note starts | lane-note colour, thin (1 px) | lane-CC colour if the step has CCs, else empty |
  | CC only (no note starts or is held) | the whole bar in the lane-CC colour | |
  | none | no bar | no bar |

- The two colours are theme tokens and must be legible in every theme (Amiga, Black, White).
- There is no badge in the track header. Events at or past the track's length get no mark: they are deleted by a length cut (A17).
- **Cost.** The draw computes the held notes by walking only the lane events of the visible steps, plus the notes still held at the first visible step: a bounded walk in the main loop, never in the audio path.
- Marks cost display-list words: worst case 16 tracks x 16 visible rows x ~4 words [est] if every cell had one; typical a few dozen. S4 measures it against the grid's own budget; if it does not fit, the counts of the held steps go first, then the counts of the CC steps (`MID` and the bar stay).
- The count of events in the lane per pattern is on the F7 EVENTS list header (`LANE n`), not in the grid.

**What EVENTS shows.** Step rows and lane rows interleaved by time (A3 of phase 0: at the same time, step rows first, then lane rows in tick order). A one-character SRC column left of TYPE tells them apart (blank for a step row, `L` for a lane row). A chord is N NOTE rows with the same TIME, each with its LEN and CH. A CC move is one CC row with TIME at tick precision. For a lane row CH is stored and editable, LEN is the true length, and several NOTE rows may share a time (phase 0 forbade them; its rules A5 to A8 apply to step rows only).

**Relation to Spill and Micro-move.** They are not removed or changed (decision 1 c): REC STORE = Grid is today's behaviour, byte for byte. A user who wants the OG-compatible, grid-visible result keeps Grid; a user who wants a chord on one track and finer timing than one FX column picks Lane. EVENTS never converts between the two in v1 (a possible later verb).

## Facts studied (file:line on `dev` f1599080)

- F1 `seq/include/o3os/seq/sequencer.h:113-114`: `kMicrostepsPerStep` 48, `kPpqn` 192. A step is 48 ticks, a 128-step track 6,144 ticks (fits a u16). `step()` runs once a block (64 samples, 1.45 ms, 261,120 cycles) inside the render and returns events sorted by offset into a capped array.
- F2 `seq/src/sequencer.cpp` (`emit_cc`, `play_microstep`, `start_step`, `send_voice_off`): a CC FX goes out on the instrument's channel and port through `midi_route.h` and `Instrument::midi_cc[6]`; a new note ends the sounding one on the track (PAR-063).
- F3 `midi/include/o3os/midi/cc_in.h`: the render is the only producer of two SPSC rings of 32 words (`CcMsg`, `NoteMsg`); messages carry no time stamp. The main loop drains them (`app/src/app.cpp:978` `poll_midi_cc`, `:996` `poll_midi_notes`, 16 CCs a pass, about every 16 ms). Poly pressure and pitch bend are dropped by the parser.
- F4 `midi/out.h` orders output by due time with a hang book (`midi/hang.h`). DIN carries about 1,041 three-byte messages a second (1,562 with running status); USB out 16 events per 1 ms frame; the staging queue is 256 items.
- F5 `docs/formats/o3-project.md` (chunk rules at 15-55, unknown chunks at 324, evolution at 454-478, `INSX` template at 290-318): FORM/O3PJ, big-endian, chunk id + u32 length + payload + pad; the payload starts with a u16 version (major high byte). An unknown chunk is kept in a pool of 16 chunks and 16,384 payload bytes shared with `OGPM` and `OGPN` (970 + up to 12,822 bytes), counted in `Report::unknown_dropped` when it does not fit. A newer major is refused (`NewerMajor`); a newer minor is read as far as known. Writers emit a chunk only when needed, so projects without it stay byte-identical (size and CRC-32 fixtures in `tests/o3project_roundtrip.py`). `INSX` is the last known chunk.
- F6 Model: a `Step` is 7 bytes, one note a step; 128 patterns x 16 tracks x 128 steps, each track with its own length (`Project::set_track_length`); the arena is `kModelArenaBytes` 1,934,344 bytes. `Project::generation()` and `pattern_generation(id)` exist.
- F7 Undo (`model/include/o3os/model/undo.h`): areas Auto, Pattern, Song, Sound, None; the PATTERN ring is 160 KiB; a whole live take is one unit (`rec_take_sync`, `app/src/rec_actions.cpp`).
- F8 Phase 0 (`docs/specs/OS-MIDIREC.md`, `model/src/event_list.cpp`): the list is an overlay from F7 TOOLS; its row key is (track, step, kind, column) "with a lane index added later"; A23 lists lanes as out of phase 0. S1a is merged, S2b is open (backlog row OS-MIDIREC).
- F9 OS-PARAMAUTO (route A, approved 2026-11-10): CC FX A..F on NON-MIDI instruments automate instrument parameters through `midi_cc[L]` parameter codes; hook points `emit_cc` and a main-loop apply; MIDI instruments' CC FX and `midi_cc` maps are untouched.
- F10 OS-ENCLEARN (merged, unit-pending): Learn knobs record into the pattern through `rec_fx_write` (`app/src/rec_actions.cpp`) with `enc_take`/`enc_said`, reached from `apply_cc` and `enc_turn` (`app/src/param_actions.cpp`).

## Assumptions (decided here; reversible; each covered by a test)

- **A1 Time unit.** A lane event time is a tick: `step x 48 + microstep`, a u16 below 6,144, in the pattern's own track timeline. The lane follows the sequencer's microstep clock, so internal tempo, external MIDI clock and MTC follow with no lane code (`metro_sync.h`, `mtc_sync.h`, `midi/clock_dll.h` feed that clock); a lane is not tempo-scaled by itself. Swing moves a step's microsteps as it moves step FX today (the lane time stays; playback maps it through the same function).
- **A2 One lane per (pattern, track)**, only for a track index below 16. A lane belongs to a pattern, so a song that switches pattern switches lanes, and Perform's per-track pattern source (PerfTrack) selects the lane of the pattern it plays. A lane holds no instrument; each event carries its channel and port (the 2026-11-05 decision: the channel is stored per event). At record time the channel and port are those of the cursor track's MIDI instrument; a later instrument change does not move recorded events.
- **A3 Event record, 8 bytes, the same in memory and in the file** (big-endian in the file): `tick u16`, `len u16` (ticks; notes only, else 0; 1..6,144), `kind<<4 | channel u8` (kind 0 note, 1 CC, 2 program, 3 channel aftertouch, 4 poly aftertouch, 5 pitch bend; channel 0..15), `port u8` (low 2 bits, the value `midi_route.h` uses for DIN/USB; the rest 0), `a u8` (note, controller, program, pressure, note for poly AT; pitch bend LSB), `b u8` (velocity, value, pitch bend MSB). Lane events are sorted by tick, ties in insertion order (a chord keeps its played order).
- **A4 Playback order at a tick.** Step events of the row first (its FX, then the note), then the lane events of that tick in order. For one controller the later one wins, so the lane wins over a step CC FX of the same tick. A lane note never ends a step note and a step note never ends a lane note (A5); Stop, a jump, a pattern change and all-notes-off end all of them through the hang book (F4).
- **A5 Note length** is `len` ticks of playback time. The OFF is scheduled when the note-on is sent, len ticks later in the microstep clock, so it follows tempo and clock changes, may cross a step, a loop end or a pattern change, and in Backward, PingPong and Random play modes a note's length is still real time (the lane is indexed by (step, microstep) of the step being played, so a chord in a step plays in order inside that step whatever the step order is). Up to 8 lane notes a track sound at once; a ninth ends the oldest (a note-off first). A repeat of a sounding pitch ends the first one.
- **A6 Chase.** At transport start, a jump and a pattern change a lane sends the last value of each controller it uses (the newest CC event at or before the target tick, per controller), at most 16 messages a block spread over the following blocks, skipping a value equal to the last one sent on that controller; never at a plain pattern loop. Nothing is chased for notes.
- **A7 Playback bounds.** At most 16 lane events a block; a due event over the cap waits (the cursor does not move) and goes out the next block, so nothing is dropped except by the DIN rate cap. CC messages are limited on the way out to 300 a second a port with a latest-value-wins merge per controller (a controller that moves faster than that sends its newest value); notes, program changes and note-offs are exempt.
- **A8 Capture stamp.** While a take is open the render stamps each incoming message with the recording track's (step, microstep) at the block it drains, so the error is at most one block (1.45 ms, below a tick at 120 BPM, 2.6 ms). With no take open the existing rings are unchanged. In STEP mode (stopped) no stamp is used: a chord goes to the cursor step's first tick, LEN 48.
- **A9 Thinning.** The main loop thins a CC stream per (channel, controller) while draining: drop a point whose value differs by less than the deadband (1) from the last kept one, keep at most one per 12 ticks, always keep the first and the last of a move, and drop an interior point that lies on the line between its kept neighbours when the take ends (the co-linear pass). No interpolation is stored: a CC point holds its value until the next point (steps, not ramps; ramps are a later `flags` value).
- **A10 Take commit.** A take collects in a take buffer (2,048 events = 16 KiB, SDRAM; maintainer-level memory decision 2026-11-10 because SDRAM_STATIC had about 31 KB free; a take that fills it commits what it has and goes on in a new one, see "S3 as built"). On the take's end the main loop merges it into the lane in one pass: Overdub keeps the old events and merges; a CC controller the take moved REPLACES the lane's old points of that controller in ITS OWN range of the take, from its first to its last message of the take (a re-recorded move overwrites, as an automation lane does; the rest of a long take keeps its old points; a controller whose messages passed the end of the track covers the loop's end and start, or all of it); notes are always merged. The commit is one undo unit holding the lane's records of the affected tick range (A15).
- **A11 Take over a loop.** A take longer than the track wraps: events after the wrap get ticks from 0 and are merged by the same pass (two sorted runs). Notes still held at the end of the take get the LEN up to the end tick.
- **A12 REC STORE** is a Settings entry next to REC POLY and REC MICRO; REC POLY, REC MICRO and REC QUANTIZE apply only to Grid. Lane takes record raw ticks; QUANT (phase 0 S3) later quantises lane rows as a verb.
- **A13 Encoder and pads.** With REC STORE = Lane and the Learn target a CC letter A..F of a MIDI instrument, a move is a lane CC event whose controller is `midi_cc[letter]` resolved at record time (a Config number 0 or program/aftertouch targets follow the playback rules of `emit_cc`), whose channel and port are the instrument's, instead of a step CC FX through `rec_fx_write`. The pads send no CC today (pad modes NOTES, PIANO, TRACKS, ISO, STEPS, PATTERNS, VU: `app/include/o3os/app/pad_layer.h:28`), so a take records the pads as notes only; pads as CC sources are out of scope. A target with no letter records as today. External CCs follow decision 8.
- **A14 Coexistence with OS-PARAMAUTO.** The lanes drive MIDI output only, never an audio voice or an instrument parameter, so PARAMAUTO's CC FX A..F on non-MIDI instruments (the `midi_cc[L]` parameter codes) cannot meet a lane: the two use different namespaces (lane CC events in the `MEVT` chunk, on MIDI tracks; PARAMAUTO letters in the instrument's `midi_cc` bytes). A MIDI instrument's CC FX steps stay what they are. A lane may sit on a track whose steps use a non-MIDI instrument (the lane still sends MIDI). PARAMAUTO's render mailbox and P1's lane player share no state and can ship in either order; both add a call in `emit_cc`'s neighbourhood and S2 must rebase on whichever merges first.
- **A15 Undo.** One take, one UndoArea::Pattern unit, never one record per event. Its record is the lane's events in [t0, t1) before the commit (at most 4,096 x 8 = 32 KiB, 20 % of the 160 KiB ring) plus the new count; undo swaps the range back. A unit that does not fit the ring is refused before the commit with a message (the take stays in the buffer for one more try), never half committed. Edits in EVENTS are ordinary Pattern units of the changed rows.
- **A16 Pattern copy, paste and lengths.** A whole-pattern copy and a block copy carry the lane events of the block's tracks and tick range (a block widens to whole steps); the clip gets a 2,048-event lane buffer (16 KiB, SDRAM) and a paste that would exceed the pool is refused whole. Block cut, delete, row insert and remove, SHRINK and EXPAND move or scale lane ticks with the rows; FILL, RAND and other generators touch steps only; TRANSPOSE moves lane notes of the block too (a chord must stay a chord). `delete_pattern` and CLEAR free the pattern's lanes. A Song-row copy carries pattern references only, so lanes follow the pattern.
- **A17 Track length.** Shortening a track DELETES the lane events at or past the new length (maintainer, 2026-11-10: "NO" to keeping them), in the same undo unit as the length change, so Undo brings them back; lengthening does not. Pinned by a test at S1.
- **A18 Phase 0 relation.** The EVENTS screen, its keys and its row key stay (decided 2026-11-05); the row key gains the lane index, `OverlayId::Events` and `Context::Events` are reused. Step rows keep a derived CH; lane rows store CH. S6 needs phase 0 S2b (edit-through) merged.
- **A19 No lane on a project that never uses one.** No chunk, no pool use, no per-block cost beyond one `lane == nullptr` test a step start.

## Data model and the `MEVT` chunk

**Model.** `model/` gets a `LanePool`: a bounded SDRAM region (60 KiB, 7,680 events, decision 6a note 2026-11-10: in the delay region's spare, `kLaneStoreBase`), outside `kModelArenaBytes` (the arena table and every pinned size stay), owned by `Project`. A lane directory maps (pattern, track) to an offset and a count (2,048 slots, 4 bytes each, 8 KiB, in the pool's budget). A lane is a contiguous run of A3 events; an insert in the middle moves the tail (1,000 events, 4 KB, about 22 us [est]); a lane that outgrows its run is moved to the pool's end (compaction is a main-loop pass over the whole pool, 60 KiB, under 1 ms [est]). Limits (`model/limits.h`, compared by ctest against `tools/o3project.py`): 4,096 events a lane, 7,680 in all, 2,048 lanes. `Project::lane(pattern, track)` returns a read-only view; every writer goes through one `Editor` call that journals (A15) and bumps `pattern_generation(id)`.

**File.** A new chunk `MEVT`, written after `INSX` (the last known chunk), before unknown chunks and `CRC `; known-chunk order is fixed, so this is the rule: `...INSX, MEVT, <unknown>, CRC `.

```
MEVT payload (big-endian, padded to even length like every chunk)
  u16 version         0x0100 (1.0); 0 is Malformed
  u16 lane_count      0..2048
  lane x lane_count, sorted by (pattern, track):
    u8  pattern       0..127
    u8  track         0..15
    u16 event_count   1..4096
    u8  event_size    8 (a later minor may grow the event; read the first 8 bytes of each)
    u8  reserved      0
    event x event_count, A3 layout, event_size bytes each
```

Rules: written only when at least one lane has events; an empty lane is never written, so a project without lanes is byte-identical to today's (the size and CRC-32 fixtures of `tests/o3project_roundtrip.py` stay pinned). The chunk is additive, so the major stays 1 and no existing chunk changes layout: an old reader keeps it as an unknown chunk when it is at most 16 KiB and the pool has room, otherwise it skips it and counts it (F5, decision 3). A newer major of `MEVT` is refused (`NewerMajor`); a newer minor is read with the known fields and `newer_minor` set. The reader is tolerant on content that has a safe default (the LRNB/INSX style): a lane with a pattern or track out of range, a duplicate (pattern, track), unsorted ticks, a tick at or past 6,144, an unknown kind, a channel or `a`/`b` byte out of range is skipped and counted (`Report::mevt_skipped`), and the load continues; a lane over the pool's remainder is skipped and counted; a short payload or version 0 is `Malformed`. The writer never writes what the reader would skip.

**OG exports.** The OG `.mtp` writer never sees lanes: the lane events of a pattern are dropped and counted in the export report ("N lane events not exported"), as core FX are (decision 2). OG files that the OS reads or writes keep their format exactly; nothing in an OG import creates a lane. A native `.o3pj` round trip keeps every lane.

## Real-time design

**Capture (part a and b).** The render stays the only producer of the MIDI-in rings (F3). Two changes, both gated by "a take is open" (a flag the main loop sets with release ordering): the ring grows to 256 words of 8 bytes (2 KiB, SDRAM) and each message carries the stamp of A8 (the render reads the sequencer's per-track (step, microstep), which it already owns, no shared write). With no take open the code path and the 32-word ring behaviour are as today. The main loop drains up to 64 messages a pass (it is 16 CC today), thins (A9), and appends to the take buffer; a full take buffer ends the take with a message. Cost [est]: about 80 cycles a message in the render, about 150 in the main loop.

**Playback.** The sequencer gets per track a lane cursor `{lane*, first, next, step_end}` (8 bytes, 128 B for 16 tracks, SRAM). At a track's step start it finds the step's first event by binary search over the lane (12 probes at 4,096 events, about 100 cycles [est]), then at each microstep compares the next event's tick with `step x 48 + microstep`; due events go to the existing sorted output array through the same output path as step events (no new queue, no allocation, no I/O). The cap of A7 and the per-block bound keep the loop bounded; `microstep_overrun` keeps counting. Note-offs of lane notes use a held table of 8 entries a track (A5, 16 x 8 x 4 B = 512 B SRAM) with the due time in microsteps. The last-sent table of the chase (A6) is 16 tracks x 8 controllers x 3 bytes = 384 B SRAM [est]. External clock: the lane runs off the microstep, which the DLL timeline places (F1), so there is nothing to add; a start under an external clock begins on the master's next clock, as today.

**Rates on the wire.** DIN is about 1,041 three-byte messages a second, 0.96 ms each; A7's 300 CC a second leaves about 70 % of the wire to notes and the step FX. USB is not the limit (16 events a ms).

**Estimates [est] (per slice they are measured on the unit with the radar's benches B1 to B6 before the slice merges; `docs/architecture/cpu-budget.md`).**

| Item | Cost |
|---|---|
| Lane event emitted in the render | about 300 cycles; cap 16 a block = 4,800 cycles = 1.8 % of a block, typical 0 to 2 events |
| Idle lanes (cursor compare, 16 tracks, once a microstep) | about 240 cycles = under 0.1 % of a block |
| Chase burst (128 controllers, 38,400 cycles = 15 % of a block) | spread at 16 a block: 4,800 cycles a block for 8 blocks |
| 64-note burst | 7.4 % of a block (why the cap exists) |
| DIN output ISR at saturation | about 120 cycles a byte, 0.38 % |
| Headroom on the heavy song | about 13,000 cycles a block (about 43 events); the cap stays well inside it |
| Take commit (2,048 events) | one pass over the lane and the take, main loop |
| SDRAM | pool 60 KiB (in the delay spare, not SDRAM: 0 B) + take buffer 16 KiB + capture ring 2 KiB + clip lane buffer 16 KiB = about 242 KiB (1.5 % of SDRAM) |
| SRAM | cursors 128 B + held table 512 B + last-sent 384 B = about 1.0 KB |
| Flash | model + format 3-4 KB, player 4-6 KB, notes 1.5-2.5 KB, capture and take 2 KB, grid marks 1 KB, EVENTS lane rows 6-10 KB: about 18-26 KB in all. Flash is at about 94 %; images must end at or below 0xFF000 (about 59 KB of margin), so this is over a third of it: every slice records its own delta and S6 is last. |

## Interaction with other features

- **Phase 0.** See A18 and "What the pattern grid shows". EVENTS rows of kind NOTE/CC from a lane carry the lane index in the row key; the planners in `model/src/event_list.cpp` get lane variants (insert, move, set length, delete) that call the S1 `Editor` calls. The phase 0 rules A5 to A8 (one note a step) stay for step rows.
- **MIDI clock out and sync.** Lane output joins the sequencer's own output, so it leaves with the same timing relation to MIDI clock out as step notes (F4). Lanes are stopped by Stop and the transport's hang-book flush.
- **Undo.** A15. A take that was undone and then a new take: the redo stack is dropped as for any edit.
- **Song and Perform.** A2 (lanes belong to patterns), A16 (copy), A17 (length).
- **MIDI thru while recording:** as today (maintainer, 2026-11-10): a live note on a MIDI track is re-sent on the track instrument's channel and port, so the keyboard sounds while recording; no raw echo of MIDI IN to MIDI OUT is added (loops, doubled notes, doubled clock).
- **Module ABI.** A `.tmod` ABI for lanes (reading or driving a lane from a module) is Level C and out of this spec.

## Slices (each mergeable alone; host tests; 4-7 allowed, 7 used)

| Slice | Content | Level | Tests | Flash / SRAM / CPU [est] |
|---|---|---|---|---|
| S1 | `LanePool`, lane directory, `Editor` lane calls, `MEVT` writer and reader, `tools/o3project.py` reference and limits row, `Report::mevt_skipped`, `docs/formats/o3-project.md` section | C | model tests; native round trip (empty, one lane, 2,048 lanes, a full pool); Python and C++ byte equality; fuzz (truncated, bad counts, unsorted, duplicates); fixture bytes of a lane-free project unchanged; an OLD-reader test (a build without `MEVT` keeps a 16 KiB chunk and writes it back, drops a bigger one and counts it); A17 pinned | 3-4 KB / 0 / none (SDRAM 200 KiB) |
| S2 | Sequencer lane playback: cursor, held table, cap, rate limit, chase, A4 order, play modes | C (audio path) | host: fake lanes, every play mode, jumps, loop, external clock scenario, a note across a loop; `ci.sh qemu` golden before merge; unit measure of B1 to B3 | 5-8 KB / 0.6 KB / about 1.8 % worst |
| S3 | Stamped capture ring, take buffer, drain, thinning, commit, one undo unit, REC STORE setting | B plus a risk read of the render-side producer | host: thinning cases, merge/replace (A10), wrap (A11), undo unit, full buffer, pool full | 2-3 KB / 0 / 80 + 150 cycles a message |
| S4 | CC recording (part a): external CCs (decision 8), encoder (A13), the grid mark (bar, `MID nn cc`, held-note tail) | B | scenarios: take of a CC move, Learn knob in LIVE and STEP, grid screenshot; display-list budget | 2-3 KB / 0 / small |
| S5 | Poly and sub-step note recording (part b): chords, true LEN, STEP-mode chord, note-offs at take end | B | scenarios: chord LIVE and STEP, sub-step timing, REC STORE = Grid unchanged (Spill and Micro byte-identical) | 1.5-2.5 KB / 0 / small |
| S6 | EVENTS lane rows (part c): SRC column, stored CH, true LEN, several notes a time, lane planners; needs phase 0 S2b | B | `event_list` planner tests, screen scenarios, QA rows | 6-10 KB / 0 / main loop only |
| S7 | Copy, paste, cut, row ops, SHRINK/EXPAND, TRANSPOSE (A16), CLEAR and `delete_pattern`, the OG export count line | B | clip round trip, pool-full paste, export report test | 2-3 KB / 0 / none |

**Level C gates for S1 (and S2 for the audio path)** (`fw-feature`): this spec accepted and decisions 2, 3 and 6 answered; a risk review of the format (downgrade, pool pressure, fuzz) and one independent `firmware-reviewer`; round-trip, fuzz and old-reader tests green with the full `tools/ci.sh host`; the format doc and the Python reference updated in the same commit; `ci.sh qemu` and `mk66` for S2; hardware verification of S2 on the unit (DIN saturation, chase burst, jitter against the step grid, CPU on the heavy song) before the feature is called done; a flash and SRAM delta in each commit body.

**S2 gates from the S1 reviews (2026-11-10; not implemented in S1, S2 must close each before it merges).**
1. `LaneStore::sync(Project::load_epoch())` is called in the app's project load and new-project paths, with a test (a loaded project never plays a stale lane).
2. The one `LaneStore` instance (about 208 KiB) is placed explicitly in SDRAM after SDRAM init (a section attribute or a placement pointer; its zero initialisation is part of the placement, not a static constructor in SRAM), with the SDRAM_STATIC delta in the commit body.
3. `Report::newer_minor` and `Report::mevt_skipped` are shown to the user after a load (a notice naming the skipped lanes, and that a newer file was read), not only counted.
4. The cost of `lane_replace` (one memmove of the pool tail, up to 60 KiB, plus the undo record copy) is measured on the unit at a take commit and recorded in `docs/architecture/cpu-budget.md`.
5. Lane undo (maintainer decision 2026-11-10): (a) a lane undo record holds one copy of the removed events (1x the lane data), not two as in S1 (8n + 16 bytes today); (b) when a destructive lane operation (`delete_pattern`, CLEAR, a shorter track or pattern, a take over a long lane) still does not fit the undo ring, the app shows `TOO BIG TO UNDO`; a second press confirms, the operation then runs without undo and the history is cleared explicitly, never silently. The model keeps S1's behaviour underneath: `Status::Full` with nothing changed, history intact, so the app's confirmation is an explicit second call that clears the history first.

## S2 as built (2026-11-10; `seq/src/sequencer_lanes.cpp`, `seq/include/o3os/seq/lane_play.h`, `tests/seq_lane_test.cpp`)

Gates 1, 2 (placement; the arena carve is on hold, see the BACKLOG row), 3 and 5 are closed in code and tests; gate 4 (the `lane_replace` memmove window and the playback CPU) is a unit measurement and stays open (`docs/architecture/cpu-budget.md`, "OS-MIDIREC-P1 S2"). Where the build differs from the text above:

- **Chase table**: 8 controllers a track (not a wider table); the chase has no place for a ninth controller of one track and does not chase it (playback itself recycles slots, so its newest value is always remembered). A CC the chase or the merge sends costs rate budget like any other.
- **Cap-waiting event**: a due event over the cap of 16 a block waits for the next microstep; it is dropped only at a loop wrap or a discontinuity (jump, pattern change), where it would be late anyway.
- **A5 / A4 reconciliation**: held lane notes end on a jump, a pattern change and a Stop; they survive a step and a plain loop (a note may run over the end of the pattern). Step events of a tick go out first, then lane events; a lane CC of a tick wins over a step CC of the same tick.
- **Port 0** (the device's own output) shares the DIN gate and its rate limit.
- **Odd store**: a block that finds `LaneStore::seq()` odd (a main-loop write is in progress) plays nothing from that store, marks the track owed and plays the owed events late (catch-up bounded to 192 ticks, owed tracks only); it never drops a note-off. An edit of the store never replays what already passed.
- **State**: about 1.5-1.6 KB (the spec said 1.0 KB), in a `LaneRun` the caller owns and passes to `attach_lanes(store, run)`: SDRAM on the unit (`g_lane_run`, `app_main.cpp`), because inside `Sequencer` it did not fit SRAM_U's 32 KiB floor; the `Sequencer` keeps its row. The render reads it from SDRAM: measure on the unit.
- **Undo (gate 5)**: the app's LENGTH chooser asks `TOO BIG TO UNDO` and a second identical pick confirms (no undo, history cleared); the pattern operations (duplicate, expand, shrink, paste) only warn and point to F2. The Editor itself still answers `Full` and changes nothing.

## S4 as built, part a: the grid mark (2026-11-10; `app/include/o3os/app/lane_marks.h`, `app/src/pattern_view.cpp`, `tests/lane_marks_test.cpp`)

The grid mark of "What the pattern grid shows" is drawn as written (four step states, `MID nn cc`, bar of 2 px with the top half for notes and the bottom half for CCs, thin when held, events at or past the track length unmarked). `PatternSource::lane(track)` hands the visible tracks' lane views; `compute_lane_marks` walks only the events of the visible steps plus the notes still held at the first visible step (the walk is bounded by the lane's 4096 events, main loop only). Where the build differs from the text:

- **Colours**: no new theme tokens (a new one breaks the pinned token count and the module SDK). The lane-note colour is `Instr`, the lane-CC colour is `Vol` and the dim `MID` is `TextDim`; every theme already keeps them legible on the cell's background.
- **Degrade order**: the budget is `kPatternBudgetWords` 1792 (RAM_DL 2048 less a margin). A draw that would not fit drops the held steps' `MID` text first, estimated before the passes (`kPassOverhead` 150 and `kBarReserve` 64, not calibrated: they only have to be an upper bound), and then the bars, one run at a time, while `words() + 8 <= budget`. The spec's second stage, "the counts of the CC steps go next", was dropped: the fields of a `MID nn cc` step replace the empty-field glyphs of the same cells, so the count text costs no words more than the dim glyphs it replaces and dropping it saves nothing. `MID` and the bar stay.
- **Measured**: the state `lane_marks` (a lane on all 16 tracks, three zooms, three contents, the marks, a block, three cursor tracks: 108 variants) peaks at 1788 words in the translate and in the vertex mode, so the degrade does not trigger inside the sweep; `lane_typical` is the sparse picture of the manual and of QA-PAT-500. The draw CPU is a unit measurement (a few dozen mark runs against the 16-track worst case, no audio-path code).
- **Tests**: `lane_marks_test` (an empty lane, the four states, a note that ends at a step's edge, starts over held, a note held from above the window, the track length and the window's first row, the cap at 99), the `budget` pins and the gallery goldens `pattern_lane_{marks,typical}_{amiga,black,white}`.

## S4 as built, part b: the CC sources (2026-11-11; `app/src/lane_record.cpp` `lane_cc_letter` / `lane_cc_move`, `app/src/param_actions.cpp` `lane_cc_take`, `tests/rec_test.cpp` "rec lane cc")

NEW COMMUNITY OS BEHAVIOUR, host-tested; what a person feels (a knob under the hand, a real controller) is a unit check.

- **External controllers (decision 8b)**: every CC a controller sends reaches the take through S3's stamped ring and `LaneTake::cc` (thinning, replace-in-range), filtered by the keyboard's own MIDI IN and IN CHANNEL as the notes are (with IN CHANNEL 0 a controller on any channel is recorded, on the instrument's channel and port); S4 added nothing there, the test "an external controller ... IN CHANNEL filters it" pins it. PROG, CH AT and POLY AT that a keyboard sends are not stamped (decision (c)).
- **Knob and encoder (A13)**: while a take is armed (`LaneRec::capturing()`), a move of a CC A..F row of the cursor's MIDI instrument (`instrument_param::midi_cc`) is taken before `write_moved` in `apply_cc` (a Learn binding) and `enc_turn` (Instr + FX1): the move is the controller's VALUE and is fed through `lane_cc_move` into the take with the playhead tick of the last poll, through the same `feed` (so thinning, the full-take split and the notices are S3's). Nothing is written to the row, there is no `rec_fx_write` and no undo record of its own: the letter keeps naming its controller, the take is the one unit.
- **Decisions of the coordinator**: (a) the same 0..127 mapping as `rec_fx_write` (a MIDI instrument previously recorded nothing): `round(clamp(param_norm)*127)` of the row's own 0..131 scale (option 75 of 131 is 73), and the controller is `cc_resolve(env, instrument, letter)` read before the move (`midi_cc[letter]`, or the Config page's number for slot 0); (b) the selector write is suppressed for the whole take; (c) PROG, CH AT, POLY AT (and an unassigned letter) record nothing: `LANE REC: CC <L> (PROG/AT) NOT RECORDED` or `... IS UNASSIGNED`, once per take (`LaneRec::said_cc`).
- **Own design choices** (reversible): an absolute Learn binding (Jump or Pickup) bypasses its pickup in the lane (the knob position IS the value; norm = value/127 through the row's curve); a relative binding and the encoder step a per-letter running value (`LaneRec::cc_now`: the first move of a letter in a take starts at CC 64, never at the selector (which is a controller number); later moves go on from the last value of that letter in the take; reset when a take is armed. The selector cannot change under it, nothing writes it during the take); the count-in records nothing (no playhead on the track yet): the move is still swallowed, with one notice per recording (the count-in and the take after it: `LaneRec::run` keeps `said_ci` across the arm), `LANE REC: NOT RECORDED: COUNT-IN` (`lane_cc_move` returns false), because decision (b) holds from the arm, which is the count-in (`lane_cc_letter` takes `capturing()` or `lane_counting_in`; b13 unit defect: the selector was written in the count-in); the encoder's strip reads `CC <L> <value>` (the value the take got); a Config-number controller records on the instrument's channel (the sequencer's Config path would play it on the notes channel: an edge case left as is).
- **Double record, left as is**: a raw external CC that has a Learn binding on a CC letter is also in the stamped ring, so one move is recorded twice (once as the controller itself, once as the letter's controller); it is the same controller number only if the letter names it. Rare; the take thins the second away when the values match.
- **STEP mode**: the lane capture in STEP is S5's; unchanged here.

## S3 as built (2026-11-10; `model/src/lane_take.cpp`, `app/src/lane_record.cpp`, `Editor::lane_merge_take`, `tests/lane_take_test.cpp`, `tests/rec_test.cpp` "rec lane", `tests/midi_cc_in_test.cpp`, `tests/seq_midi_link_test.cpp`)

NEW COMMUNITY OS BEHAVIOUR; all of it is host-tested, the render side and the pool-full refusal included; the cost of the commit on the unit is still a measurement (gate 4).

- **Take buffer: 2,048 events, 16 KiB** (`model::LaneTake::kCapacity`, one constant), `O3OS_SDRAM_NOINIT g_lane_take` in `platform/mk66/src/app_main.cpp`; 4,096 (32 KiB) would not leave margin in SDRAM_STATIC. The sim does NOT attach a take: with a null take REC STORE = Lane falls back to the grid there (the sim has no render stamp).
- **Full take**: the buffer filling (or one note or control change not fitting) commits what it has through the normal merge, says `TAKE FULL: COMMITTED` once and goes on in a new take; a note held across the cut ends there. Tested at the Editor (pool full, 2,048 events) and the App.
- **Stamped capture (A8)**: `CcInput::open_take(track)` / `close_take()` (main loop, atomic, release/acquire), `set_stamp` / `clear_stamp` once a block from `MidiLink::drain_inputs` (render), `Sequencer::capture_stamp(track, pattern, tick)` (false when stopped, in the count-in or the track inactive). The stamped ring is 256 messages (2 KiB, SPSC, render the producer); a drop is counted (`stamp_dropped`) and the App says `MIDI IN OVERRUN: NOTES MISSED` once a take. With no take open `feed` is what it was.
- **Thinning (A9)**: deadband 1, at most one point per 12 ticks, the first and the last of a move are kept, a co-linear pass at the take's end. The per-controller first and last tick of the take (kept or not; a wrap flag) says which lane points of that controller REPLACE (A10, `LaneTake::replaces`; fold 2026-11-11, it was the take's whole span); notes are always merged. A wrap makes two intervals (A11); notes held at the end get LEN to the end tick.
- **Commit (A10/A15)**: `Editor::lane_merge_take`, one `WriteWindow`, one undo unit (joins the open undo group of the take). It refuses before it writes (NotFound, BadIndex, BadValue, Full including `undo_fits`) so nothing is partial. A refusal for room keeps the take (`LANE TAKE: NO ROOM, KEPT`) and tries once more when recording next begins; then it is dropped (`NO ROOM, DROPPED`).
- **When a take is armed**: REC STORE = Lane, the Pattern screen recording in LIVE (`pattern_recording`), the cursor's instrument a MIDI one. The take ends when recording stops, or the track, the instrument or its channel/port changes, or the song moves to another pattern. During the count-in nothing is stamped. MIDI IN and IN CHANNEL filter as the grid's note entry does. STEP mode and chords are S5 ("S5 as built"); pads still write the grid.
- **REC STORE (A12)**: device setting id 31, Grid (default, byte-identical to today) / Lane, PATTERN group; `Settings::kMaxFields` (32) is now full. The chunk version stays 0x0100 (an older file reads the new row as its default); SETS payload 260 bytes, default file 294 bytes.
- **Hooks for S4/S5**: `app/include/o3os/app/lane_record.h` (`lane_poll`, `LaneRec`) is the one place a take is armed and drained; S4's encoder CC feeds `LaneTake::cc` from the same drain (a `StampKind::Cc`), S5's chord grouping and pads-as-lane-notes call `LaneTake::note_on/off` with the pad's tick; the merge needs no change. A CC that does not come from the keyboard (S4: the encoder on a MIDI FX row) is one append: `if (rec.capturing()) rec.take->cc(rec.last_pos, controller, value)` from the main loop (`last_pos` is the playhead tick of the last pass; `LaneTake::cc` thins, tracks "moved" for the replace, and returns false when the buffer is full, which the caller treats as a split as `feed` does).
- **Known, left as is**: a controller moved while REC STORE = Lane is also seen by the CC MAP learn path as ever (a double record is S4's to decide); no marks in the grid yet (S4).

## S3 review folds (2026-11-11)

- **Pattern change**: the take ends at the end of the track of ITS pattern (not at the playhead of the next one), in `feed` and in `lane_poll`; the held notes get their LEN to there.
- **Replace range per controller** (A10, above): `LaneTake::replaces(controller, tick)`; `merge_removes` uses it with the run intervals.
- **Count-in**: `Sequencer::capture_stamp` stamps nothing in a count-in except its last half-step, as the start point's first tick (a note played just before the downbeat lands on it).
- **Pending point at a full buffer**: a slot is kept for every pending CC point (`pend_n_`, `room()`); a message that has no slot is the one refused (the take is `full()`), so the last point of a move is never lost.
- **Sort**: `finish` sorts with an in-place stable SymMerge (blocks of 20 insertion-sorted, neighbouring runs merged only when out of order), O(n log^2 n) in the worst case, no allocation (replaces the insertion sort).

## S5 as built (2026-11-11; `app/src/lane_record.cpp` `lane_step_*`, `LaneTake::note_len`, `tests/rec_test.cpp` "rec lane", `tests/lane_take_test.cpp`)

NEW COMMUNITY OS BEHAVIOUR; host-tested; the feel on the unit is a hardware check.

- **LIVE chords, true LEN, sub-step timing, note-offs at the take's end** need no new code: S3's stamped ticks (A8) already carry them. S5 adds their tests (a chord is N note events at their own ticks, each with its own LEN; a key held at the stop ends at the stop; a note at tick 5 or 133 is stored at that tick, with no Micro-move).
- **STEP chords**: REC STORE = Lane, Pattern screen in Edit with the cursor free (`rec_free`; stopped or playing), the cursor's instrument a MIDI one and a single cell selected. A key down enters its note at the FIRST tick of the cursor step (row x 48) with LEN 48 (one step) and the keyboard's velocity; the keys of one chord share that tick even if the cursor moves while they are held. When the last key of the chord is up the chord is merged (one undo unit, the same `lane_merge_take`) and the cursor advances by the edit step (JUMP; the selection is cleared). Stamps are not used (a take armed only exists while recording). A cursor past the track's end says `PAST TRACK END` and enters nothing. Moving to another track, pattern or instrument while keys are held commits the chord as it is. All-notes-off commits it.
- **`LaneTake::note_len`**: a note whose length is known at once (closed event, LEN clamped to 1..6144, velocity 0 refused); the take buffer is the same 2,048 events.
- **REC STORE = Grid**: none of this runs; the grid, Spill and Micro-move are byte-identical (test).
- **Not done (S5 deviation)**: pads still write the grid (pads as lane notes would need a pad-down tick; not asked for by a test and left to the maintainer). Hooks for S4: `lane_poll`'s drain; the STEP path never arms the ring.

## S7a as built (2026-11-11; `Editor::lane_copy` / `lanes_copy_fit` in `model/src/editor.cpp`, `app/src/song_edit.cpp`, `app/src/pattern_tools_actions.cpp`, `tests/lane_row_test.cpp`)

NEW COMMUNITY OS BEHAVIOUR; host-tested; no on-disk change (a lane-free project is unchanged: no lane, no record, no byte).

- **Copy (A16)**: `song_copy_pattern` and `song_copy_pattern_masked` call `Editor::lane_copy(from, to, track)` for each copied track after its steps (the masked copy skips the cleared tracks). One `kLaneInsert` record per lane, inside the caller's unit; only the events below the source track's end are copied (A17). The source view is re-read after the gap opens (the pool is packed).
- **Full with nothing changed**: before `create_pattern` the copy asks `Editor::lanes_copy_fit` (pool events, lane count, and the open unit's ring bytes plus the copy's own), so Full leaves the project, the lanes and the history as they were. DUP PAT says `NO FREE PATTERN` for that too.
- **Planners**: `copy_pattern_body*` add the lane records to the journal bytes when given the lane store (`song_unique_plan`, `song_form_plan`; the optional parameter defaults to none). Known limit: the UNIQUE and FORM plans do not model the pool across several copies of one unit; a later copy can answer Full at apply time, leaving the earlier ones (the unit stays consistent and undoable).
- **Delete**: `Editor::delete_pattern` already freed the pattern's lanes (records before the pattern's); S7a adds only the test.
- **CLEAR LANE**: in the Pattern TOOLS dialog, CLEAR with WHAT = ALL over every step of the cursor's track also calls `Editor::lane_clear` for that track (same undo unit, done first so a refusal changes nothing; strip `CLEARED: n STEPS + LANE`). A partial block leaves the lane alone. Clearing a time range of a lane is left to S7b or the EVENTS list.

## S7b as built (2026-11-11; `app/src/lane_record.cpp`, `seq::PerformBank::play_mode`, `ViewState::track_mode`, `tests/rec_test.cpp`)

NEW COMMUNITY OS BEHAVIOUR (maintainer-delegated decision, simplest and reversible); host-tested; no on-disk change. A Lane take is refused while the recording track plays in any order but Forward (Backward, PingPong, Random, C1..C20), the alternative of deriving the take's intervals from min/max being left out.

- **Source of the order**: the only writer of a track's play mode is the Perform layer (`Command::Kind::PerfTrack` from the PLAYMODE slot), so the App mirrors `PerformBank::play_mode(track)` (the value the bank wants: the PLAYMODE slot's live value when the track is affected, else 0) into `ViewState::track_mode[12]` once per pass, before `lane_poll`. Tracks above 11 are Forward. Cost: 12 short loops per main-loop pass, 12 bytes.
- **Refusal**: `lane_poll` treats the take as not wanted when `order_ok` fails: no `open_take`, `lane_counting_in` is false (a knob or the encoder in the count-in edits its parameter as for a non-lane instrument), the keyboard goes to the step grid as with REC STORE = Grid. The strip says `LANE REC: FORWARD ONLY` once per arming (`LaneRec::said_order`, reset when the order is Forward again or the recording is not wanted).
- **Order change under a take**: the take is dropped (`close_take`, `LaneTake::reset`; nothing more is committed: the parts a TAKE FULL split already committed stay, the rest of the lane is unchanged), the same notice is said, and the grid takes the keys from then on. The next recording in Forward arms normally.
- **Unchanged**: STEP into a lane (its place is the cursor's row, not the playhead, so the order does not matter); playback of lanes in every order (S2).
- Tests: `rec_test.cpp` "rec lane order" (five orders under a take, arming refused then recording after Forward, a mode on another track leaves the take alone).

## QA rows (reserved QA-PAT-490..519 and the matching screens' ranges; added per slice)

| ID | Function | Gesture | Expected |
|---|---|---|---|
| QA-PAT-490 | REC STORE setting | Settings, REC STORE | Grid or Lane; default Grid; saved with the settings |
| QA-PAT-491 | CC take into a lane | LIVE, play, move the encoder | One lane CC series, one undo unit, mark in the grid |
| QA-PAT-492 | Chord on one track | LIVE, play three keys together | Three NOTE rows at the same TIME in EVENTS, one grid mark |
| QA-PAT-493 | Grid unchanged with Grid | Same gestures with REC STORE = Grid | Spill and Micro-move exactly as today |
| QA-PAT-494 | Lane plays with steps | Play | Lane CC and notes go out on the stored channel and port |
| QA-PAT-495 | Undo a take | Undo | The whole take goes; Redo returns it |
| QA-PAT-496 | OG export | Export `.mtp` | Lane events dropped; the report counts them |

## Risks

- Downgrade (decision 3): a lane over 16 KiB is lost by an old build's save. Documented, not prevented.
- Flash margin (59 KB [est]): S6 and the player are the big spends; measure first, cut the editor rows before the player.
- The render-side stamp is the one new shared-state change; kept to one SPSC producer with a flag the main loop owns.
- A lane note and a step note on one track can overlap in a hardware synth with a mono voice; decision 4 accepts it, and the grid mark shows the lane.
- The pool is a new fixed region: a pool-full refusal path must be tested in S1 and S3, because an unhandled full pool would lose a take.

## Evidence

Everything above is "new Community OS behaviour" except the facts F1 to F10 (read in the code at the paths given) and this "open research question": the display-list cost of grid marks. No parity with the official firmware is claimed; `polyend/tracker-ce` was not consulted and is not needed for this spec.

## State for implementer

Start with S1 only. Entry points: `model/include/o3os/model/` (`project.h`, `limits.h`, `editor.h`, `undo.h`), `formats/src/native_writer.cpp` and `native_reader.cpp` (copy the `INSX` handling), `tools/o3project.py`, `tests/o3project_roundtrip.py`, `tests/native_format_test.cpp`, `tests/native_fuzz_test.cpp`, `tests/model_test.cpp`. For S2: `seq/src/sequencer.cpp` (`start_step`, `play_microstep`, `emit_cc`, `send_voice_off`), `seq/include/o3os/seq/rows.h`, `seq/include/o3os/seq/midi_route.h`, `midi/include/o3os/midi/out.h`, `midi/include/o3os/midi/hang.h`. For S3 to S5: `midi/include/o3os/midi/cc_in.h`, `app/src/app.cpp` (`poll_midi_cc`, `poll_midi_notes`), `app/src/rec_actions.cpp` (`rec_fx_write`, `rec_take_sync`), `app/include/o3os/app/rec_place.h`, `app/src/param_actions.cpp`. For S6: `model/src/event_list.cpp`, `app/src/events_actions.cpp`. For S7: `app/include/o3os/app/pattern_clip.h`, `song_clip.h`, the OG export writer. Order: S1, S2 (host and qemu), then S3 to S5, then S6 (after phase 0 S2b) and S7. Run `python3 -I tools/code_map.py` first. A lane-free project must stay byte-identical at every slice.
