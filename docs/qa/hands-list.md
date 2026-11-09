# Hands list for the v1.0.0 session

The maintainer's checklist for the hands session before v1.0.0. It is built from the catalogue rows whose Unit cell says `manual:` (a bare or code-quoted marker) and whose status is still `more testing needed` or `unit open`. Each row goes into one group. The HANDS items (91) are split four ways:

- **T1, the battery already covers it**: the Unit cell names a `qa_*.json` step.
- **T2, console and shot**: keys, the encoder or pads through `o3console`, then a unit shot read by a person or the orchestrator.
- **T3, measured over USB audio**: the tester records the unit's USB capture and measures level, pan, pitch or clicks with a script.
- **M, the maintainer only**: the unit's own switches, panel, ear and feel, a card pulled by hand, a computer as the USB host.

CONSOLE is the tester's list, as before.

## Counts

| Group | Rows | Added items | Items |
|---|---|---|---|
| T1, the battery covers it | 29 | - | 29 |
| T2, console and shot | 24 | - | 24 |
| T3, measured over USB audio | 17 | - | 17 |
| M, maintainer only | 11 | 10 | 21 |
| HANDS total | 81 | 10 | 91 |
| CONSOLE | 40 | - | 40 |
| Total | 121 | 10 | 131 |

The added items are known hands items that no row covers (the last group, M). Items already covered by a row's manual step are not repeated (see "Coverage notes").

## T1: the battery already covers it

The row's Unit cell names a `qa_*.json` step, so the HIL battery carries it.

### Perform

- [ ] QA-PRF-100: The DJ page — Perform opens on its second page: the strip reads PAGE DJ and the curve chip SMOOTH, twelve track boxes all THRU, GROUP A tracks none and GROUP B ...
- [ ] QA-PRF-101: The encoder is the crossfader — one turn of the encoder is the whole travel (24 detents, 4 positions of 96 each): 6 detents to the left leave the fader a quarter of the way from ...
- [ ] QA-PRF-102: Pad row 4 jumps the crossfader — pad 1 is all A, pad 12 all B, 6 and 7 sit either side of the centre
- [ ] QA-PRF-103: CURVE: SMOOTH and CUT — the strip's chip and the mark under CURVE say CUT
- [ ] QA-PRF-104: The groups, Shift + row 1 — each tap sets the group of that pad's track in turn THRU, A, B, THRU
- [ ] QA-PRF-105: RESET centres the crossfader — the fader is back at the centre (the knob, the gains A 70%  B 70%) with the glide
- [ ] QA-PRF-106: Leaving the page ends every DJ effect — the groups come back to full level at once (a 20 ms glide) and nothing of the DJ page is heard or drawn on any other screen
- [ ] QA-PRF-109: The filter: F4 held + the encoder — the filter row reads FILTER HP 30 after three detents to the right (LP on the left of the trough, HP on the right, the knob moves, orange away ...
- [ ] QA-PRF-110: The filter springs back at the release — the filter is back at 0 at once on the screen (FILTER OFF, the knob in the middle) and the engine glides there in 120 ms
- [ ] QA-PRF-111: LATCH keeps the filter — the key under LATCH is marked and the filter row says LATCH after the value
- [ ] QA-PRF-112: RESET clears the filter and keeps LATCH — the filter is back at 0 (FILTER OFF, LATCH still on, the fader centred) and the engine glides to 0
- [ ] QA-PRF-113: The FILT pad is the filter too — the pad is lit bright while the filter is away from 0
- [ ] QA-PRF-114: Leaving the page ends the filter — the engine glides to 0 in 120 ms
- [ ] QA-PRF-115: The KILL pads kill a band while they are down — the KILL row names LOW and HI in the accent colour while they are down and MID stays dim
- [ ] QA-PRF-116: LATCH keeps a kill, a second tap ends it — MID stays in the accent colour after the pad is let go and the row reads LATCHED
- [ ] QA-PRF-117: RESET and leaving the page end the kills — the kills are off at once (the engine crossfades back to the plain mix in 5 ms and bypasses the stage)
- [ ] QA-PRF-118: The pattern row — under the track boxes: NOW  PAT n, NEXT  - (or the queued pattern in the accent colour) and BANK n on one line, and the twelve pattern numbers of ...
- [ ] QA-PRF-119: A pad of the row queues the pattern, a second tap cancels — the pad blinks and NEXT PAT 2 appears
- [ ] QA-PRF-121: The tempo nudge — while Right is down the tempo is 4 % faster (the strip shows NUDGE +4% and the new BPM), Left is 4 % slower
- [ ] QA-PRF-123: REP: a stutter on a group's tracks — the line under the gains reads A  REP 1/16 and B  REP 1/4 in the accent colour and the two pads are bright while they are down
- [ ] QA-PRF-124: ECHO: the send opens whole, one beat later the group is cut — the group's delay send opens at once (whatever the instruments' own sends) and the line reads A  ECHO
- [ ] QA-PRF-125: CUT: the group is silent while the pad is down — the line reads A  CUT and the pad is bright
- [ ] QA-PRF-126: LATCH keeps a row 2 pad, RESET ends it — the two pads stay lit and the line keeps A  REP 1/8 and B  ECHO after the release
- [ ] QA-PRF-127: Leaving the page ends the pad effects; the slots come back — every REP, ECHO and CUT ends at once (the engine is told the slot's own value, or the end): the pads are dim and the line is gone when the page is ...
- [ ] QA-PRF-140: The LATCH line names what it keeps — the strip reads LATCH ON: FILTER AND PAD EFFECTS STAY (the filter, and the CUT, ECHO, REP and KILL pads latch with it)

### Record

- [ ] QA-REC-036: MEM during a take and after it — before the take MEM reads the share of the sample memory in use (observed 75 %)
- [ ] QA-REC-037: F2 MONITOR: hear the input through the engine — MONITOR reads ON / OFF and flips on each press
- [ ] QA-REC-038: F5 SEEK- / F6 SEEK+: the radio chip's own seek — each press asks the FM chip for the next station up (F6 SEEK+) or down (F5 SEEK-), wrapping at the band's end
- [ ] QA-REC-039: F7 SAVE WAV: the take's window as a WAV — the take's window (the trim) is written as a 16-bit mono 44,100 Hz WAV into Samples/ on the card as the next free REC_NNN.wav (REC_001, then ...

## T2: console and shot

Keys, the encoder or pads through `o3console`, then a unit shot read by a person or the orchestrator.

### Config

- [ ] QA-CFG-014: The unit comes up as the setting says — Off: STANDARD at boot (diag log: usb: pull-up on, STANDARD)
- [ ] QA-CFG-202: Playing through a copy — the pattern plays on, 0 underruns
- [ ] QA-CFG-212: The platform says the same words as the row, and a card cleaned on the computer does not warn — NO CARD, CARD BUSY: WAIT FOR THE SAVE, STOP THE EXPORT FIRST on the strip in both the row and the platform

### Global

- [ ] QA-GLB-141: HELP: Shift + Config opens the help page of the screen in view — a full-height dialog titled HELP: SONG with 3/15 at the right of its bar opens over the screen, at the top of the page, on a panel that covers the ...
- [ ] QA-GLB-143: HELP: Up, Down, the encoder and Shift scroll the page — Up and Down move one line, the encoder one line a detent
- [ ] QA-GLB-145: HELP: Shift + Config comes back to the page's last scroll — the page of the screen in view opens at the scroll it was left at
- [ ] QA-GLB-146: HELP: a pad closes it and plays nothing; a pad held across the open ends its note — the first pad press closes help and starts no note, and its release (on Perform, with LATCH on and a slot held) does not end the latched effect
- [ ] QA-GLB-149: HELP: the keys listed are the keys of the screen — each key does what its line says (text condensed from the manual and bindings.cpp

### Mixer

- [ ] QA-MIX-029: The info line does not run into its hint — the line reads the left text, a gap, then the hint, no glyph on another: with no project T1 level   no project and left / right: column  up / ...

### Module app

- [ ] QA-APP-001: The frame of an app page — the strip reads APP, the transport, NAME midimon, STATE RUNNING and the CPU / MEM readout
- [ ] QA-APP-003: A screen button leaves the app and opens its screen — the screen of the button opens (SONG, PATTERN) and the app is ended
- [ ] QA-APP-006: Shift + encoder: edit the parameter under the row — the value of the selected row moves by the encoder's detents inside its range, and the app's own set_param hears the change (the value on the page ...

### Modules

- [ ] QA-MOD-117: The launch confirm marks F1 — F1 reads LAUNCH?, inverted, while the confirm is on the strip and the cursor is on that app
- [ ] QA-MOD-119: The strip's STOPPED word reads in every theme — the strip says STATE STOPPED with STOPPED as dark letters (AccentText) on an orange chip (Accent): in Amiga the bare orange would be 1.2:1 on the ...
- [ ] QA-MOD-120: S5: an app page shows the app's labels — F1-F7 read what the app declares (Tap Calc TAP SET, Rhythm Gen APPLY VARY, Morph APPLY - RESET), a key it does not label reads -, F8 reads BACK
- [ ] QA-MOD-123: An app page speaks one grammar: row, value, coarse step, default, page — Down moves the selection to the next row (ROTATE)
- [ ] QA-MOD-124: The six self-drawn apps (Rhythm Gen, Humanize, Chords, Mutate, Morph, SMF Export) all answer the same keys — Up / Down the row (stops at the ends)
- [ ] QA-MOD-125: The Shift layer of an app page is the OS's parameter column only while the app declares a parameter — Pads: the OS column moves its row and edits it as before and the app does not hear the keys
- [ ] QA-MOD-126: The five apps that declared rows draw them in the same grammar (Tap Calc, MIDI Monitor, Loudness VU, Tuner, Scope Spectrum) — the rows are at the bottom of the page (Tuner: top right): Loudness SOURCE and HOLD, Scope VIEW, SOURCE and TIMEBASE (dim in the spectrum), Tap ...
- [ ] QA-MOD-128: A screen button leaves an app page for its screen; the app never sees it — the app is ended (exit, shutdown) and the button's screen opens
- [ ] QA-MOD-129: Rhythm Gen plays a lane's hits when the encoder turns — the lane's hits sound once through on the instrument INSTR names, eight steps a second, the first 16 steps, one note at a time, then silence

### Perform

- [ ] QA-PRF-005: The strip's EDIT word reads in every theme — the strip says TRACKS n and then EDIT as dark letters (AccentText) on an orange chip (Accent), with a gap each side: in Amiga the bare orange ...
- [ ] QA-PRF-006: The info line with no track on reads apart from the hint — the line on the left names what to do (no track is on: ...) and the hint on the right (hold Shift: TRACKS ...) is a separate text with a gap ...

### Record

- [ ] QA-REC-035: The strip's REC state reads in every theme — the strip says STATE REC with REC as dark letters (AccentText) on an orange chip (Accent): in Amiga the bare orange would be 1.2:1 on the chrome

## T3: measured over USB audio

The tester records the unit's USB capture on the computer (`platform/mk66/tools/o3audio.py`) and compares the figures with the pass threshold in each `measure:` line.

### Mixer

- [ ] QA-MIX-023: Mute during a note release — no click at either edge
  - measure: o3audio clipping and step check across the mute edges of a held release (a click shows as a sample step); pass: no click at either edge, against a clean reference run.

### Perform

- [ ] QA-PRF-011: The held Perform filter stays on across Play and Stop on Master and Song — the held filter is heard from the first note, with no pad touched, and again after the Stop and the second Play (the performance layer is its own: ...
  - measure: o3audio level of the first 100 ms after Play, filter latched, against a run with the slot off; pass: the high band stays down from the first note and after the second Play, as in the first run (Expected: no return to the unfiltered level).
- [ ] QA-PRF-012: BITDEPTH crushes the notes of the tracks that are on — the value reads as the number (--- at 0) and is how many bits are taken away (16 minus the bits that stay): 1 is a trace of grain, 15 is one bit
  - measure: o3audio RMS of one fixed sample at BITDEPTH 1, 8 and 15 and at row 0; pass: 1 is a trace of grain, 15 is two levels only, and row 0 matches the clean run.
- [ ] QA-PRF-013: OVERDRV drives the notes of the tracks that are on — the notes distort more as the value rises (the curve of the instrument's OVERDRIVE parameter and of the step FX 38: 1 is almost clean, 100 drives ...
  - measure: o3audio peak and RMS of one sample at OVERDRV 10, 50 and 100 and at row 0; pass: the distortion grows with the value, 1 almost clean, 100 hard, and row 0 matches the clean run.
- [ ] QA-PRF-014: A held BITDEPTH or OVERDRV wins over the step FX and the instrument, and the step FX comes back when the pad ends — while the pad holds the slot the track sounds at the pad's value
  - measure: o3audio level of one note while the pad is held and after release, against the step FX value; pass: held sounds at the pad's value, after release at the step FX's value, and at the instrument's when the step FX ends too.
- [ ] QA-PRF-016: A held BITDEPTH or OVERDRV stays on through Play, Stop, Master and Song, and is gone when a project is opened — Play, Stop, Master and Song leave the FX on (the performance layer is its own, like the filters)
  - measure: o3audio distortion of the same note after Play, Stop, Master, Song, and after a project open; pass: the first two driven, the last clean, as the Expected says.
- [ ] QA-PRF-020: A VOLUME slot cuts the affected tracks — the tracks drop to a quarter of their level (-12 dB) at -50, are silent at -100 (their delay and reverb sends too), and are as before at 0 and ...
  - measure: o3audio RMS of a steady track at VOLUME -50, -100, 0 and +100 against the slot-off level; pass: -50 about -12 dB, -100 silent (below o3audio's -60 dBFS floor), 0 and +100 no louder than the slot-off level.
- [ ] QA-PRF-021: A PANNING slot moves the affected tracks — the tracks move to the right (positive) or the left (negative) of the pan they have, by the value in pan units: +25 halves the left channel, +50 ...
  - measure: o3audio L and R RMS of a centred sample at PANNING +25, +50, -50 and 0; pass: +25 is about -6 dB on the left, +50 is silent on the left, -50 is silent on the right, 0 is centred again.
- [ ] QA-PRF-022: PANNING adds to a pattern's Panning FX and ends with it — the track plays at 50 (30 + 20) while the slot is held and at 30 when it is released
  - measure: o3audio L/R balance of one steady note with the slot held and after release; pass: held at pan 50 and released at pan 30, as the Expected says (a 20-unit shift).
- [ ] QA-PRF-023: VOLUME and PANNING survive Play and Stop, and a track at VOLUME -100 comes back at once — the tracks stay silent through Play and Stop (the tracker's own volume and pan reset at a transport start, the Perform layer's do not)
  - measure: o3audio silence of the track at VOLUME -100 through Play and Stop, and the time from release to sound; pass: silent through both, sound back within a few ms of release, with no click.
- [ ] QA-PRF-025: A filter slot stays latched only at a stored value — without EDIT the turned value is the live one: the filter bites while the pad is down and ends at the release (the row stays selected, but its ...
  - measure: o3audio level envelope of a sounding track with a pad held and the encoder turned, with and without EDIT; pass: without EDIT the filter ends at release, with EDIT it stays until row 0 or another row.
- [ ] QA-PRF-026: A DELAY slot adds to the affected tracks' delay send — the delay's repeats grow with the value: +30 sends as if the instrument's DELAY SEND were 70, +100 as a full send (100) whatever the instrument's, ...
  - measure: o3audio echo tail after a note at DELAY +30, +100, -30, -100 and 0 against the instrument's send 40; pass: the tail grows with the value, +100 is full, -100 none, and 0 gives the 40 back.
- [ ] QA-PRF-027: A REVERB slot adds to the affected tracks' reverb send — the reverb tail grows with the value in the same steps (+30 is as a send of 70, +100 a full send, -100 closes it, 0 gives the instrument's back)
  - measure: o3audio reverb tail as QA-PRF-026 on the REVERB slot, with both slots held; pass: the same steps, and each slot changes only its own return.
- [ ] QA-PRF-028: The step FX 30 (Delay send) and 32 (Reverb send) set the track's send, and a slot adds to them — from the step with the FX the track sends 60 to the delay in place of the instrument's 20, for that note and the notes after it until the FX ends ...
  - measure: o3audio delay send at the FX step against the note before it; pass: 80 (60 + 20, held to 100) with the slot held, 60 after the slot's release, 20 when the FX ends, as the Expected says.
- [ ] QA-PRF-029: Held DELAY and REVERB survive Play and Stop — the send stays closed (or open) through Play and Stop (the Perform layer is its own
  - measure: o3audio delay repeats of a note with DELAY -100 (or +100) held through Play and Stop; pass: the send holds through both, and it is back within a few ms of release with no click.
- [ ] QA-PRF-031: A TUNE slot moves the affected tracks' pitch by whole semitones — the sounding note follows the value at the next block with no glide (+12 an octave up, -12 an octave down, +48 and -48 four octaves, held at the ...
  - measure: o3audio pitch of a sustained note at TUNE +12, +48, -12, -48 and 0 (strongest line); pass: the ratio is 2, 16, 1/2 and 1/16 of the note's own pitch, and the change takes no glide.
- [ ] QA-PRF-107: The crossfader only cuts, on the dry level and the sends — the cut multiplies the tracker's channel gain, so the track's sends are cut with its dry level
  - measure: o3audio level of a group at the far end, the mix and the send returns, against the dry level; pass: the sends fall by the same amount as the dry level, and nothing is louder than before the cut.

## M: the maintainer only

Needs a person with the unit's own switch, panel, ear, feel or a hand-pulled card, or a computer as the USB host.

### Global

- [ ] QA-GLB-111: POWER-DISK: off in the boot disk mode, a computer attached — the first hold: the line empties and the strip says COMPUTER MAY BE WRITING - HOLD AGAIN
- [ ] QA-GLB-130: SHOT: Shift then Power saves a screenshot — after about 1-2 s the strip says SCREENSHOT SAVED: SHOT_0001.BMP
- [ ] QA-GLB-133: SHOT: refused with no card — the strip says NO SCREENSHOT: NO CARD
- [ ] QA-GLB-134: SHOT: refused with the card on the computer — the strip says NO SCREENSHOT: CARD ON THE COMPUTER
- [ ] QA-GLB-137: SHOT: a plain hold still powers off, Shift during a hold still cancels it — the first: POWERING OFF as before (QA-GLB-102)
- [ ] QA-GLB-138: SHOT: the song plays on during a capture — the sound does not stop, click or stutter during the 1-2 s the screen is frozen
- [ ] QA-GLB-139: SHOT: a card pulled during a capture leaves no SHOT_nnnn.bmp — the strip says SCREENSHOT FAILED
- [ ] QA-GLB-140: SHOT: keys during the capture do nothing, a held key ends cleanly — nothing pressed or turned during the freeze does anything afterwards (no page change, no note, no value moved)

### Input test

- [ ] QA-INP-002: The 33 buttons: down, seen and counted — the 33 cells (11 x 3, by button code, named as the keycaps: F1, PAT, SHIFT, LEFT) show the selection blue while the key is down, the press count ...
- [ ] QA-INP-003: The 48 pads: down, seen and counted — the 48 cells (12 x 4, the number of the pad in the corner, 0 the top left, 11 the top right) light while the pad is down and show its count after it
- [ ] QA-INP-004: The encoder line — the bottom line reads ENCODER  TOTAL +n  LAST +n  EVENTS n: TOTAL is the sum of the detents with its sign, LAST the last turn (-2 for two clicks ...

### Known items added

No catalogue row covers these; each is a hands check in its own right.

- [ ] Help legibility in three themes: open the help page (Shift + Config) on a screen in each of the three themes and read the text and the bar at arm's length; they must read apart. QA-GLB-141 and QA-GLB-149 do not name the theme.
- [ ] FWCONFIG real install: only after `fw-gate-check` passes and the maintainer names the candidate, install it from `Firmware/` through the Config firmware update (the update path in `docs/specs/OS-FWCONFIG.md`): the list shows the name, size and CRC, the two confirmations, the progress, then the unit comes back. Check the version after the real power cycle.
- [ ] OG export with the card shared: with the card on the computer (`disk share`), the OG export is refused with its reason (QA-FIL-093, QA-CFG-201); after the eject it runs, and the OG files open on the computer as QA-FIL-088 expects. The hands part is the computer's view of the card.
- [ ] Pattern tap-tempo feel: tap the tempo on the Pattern screen against a steady beat and judge by ear and by the BPM on screen whether it settles without jumps. No row covers it (QA-MOD-003 is the Tap Calc app's tap). If the Pattern has no tap tempo, say so here and record an open research question.
- [ ] SNDMOD by ear, QA-MOD-151: TB-303 the filter by ear (`qa_modules.json`; the listening is the hands part).
- [ ] SNDMOD by ear, QA-MOD-152: TB-303 accent and slide by ear.
- [ ] SNDMOD by ear, QA-MOD-153: Chip the waves by ear.
- [ ] SNDMOD by ear, QA-MOD-154: Chip envelope, arpeggio, sweep and vibrato by ear.
- [ ] SNDMOD by ear, QA-MOD-155: FM the default patch and the feedback by ear.
- [ ] SNDMOD by ear, QA-MOD-156: FM algorithms 1 to 8 by ear.

## Tester can run these

The CONSOLE rows, by screen. A later slice turns them into HIL steps.

- File: QA-FIL-041, QA-FIL-055, QA-FIL-104
- Global: QA-GLB-131, QA-GLB-132, QA-GLB-135, QA-GLB-136, QA-GLB-142, QA-GLB-144, QA-GLB-147
- Input test: QA-INP-005, QA-INP-008
- Mixer: QA-MIX-024
- Module app: QA-APP-002, QA-APP-004, QA-APP-005, QA-APP-007, QA-APP-008, QA-APP-009
- Modules: QA-MOD-110, QA-MOD-111, QA-MOD-112, QA-MOD-113, QA-MOD-114, QA-MOD-127, QA-MOD-130, QA-MOD-131, QA-MOD-132, QA-MOD-133
- Pattern: QA-PAT-310
- Perform: QA-PRF-010, QA-PRF-017, QA-PRF-018, QA-PRF-024, QA-PRF-030, QA-PRF-032, QA-PRF-108, QA-PRF-120, QA-PRF-122, QA-PRF-128

## Coverage notes

- **Source filter.** The 121 rows are the catalogue rows with a `manual:` Unit cell, `more testing needed` or `unit open`. Rows whose Unit cell is the legacy `by hand` form are not in the source (for example QA-FIL-088 and QA-CFG-200, 201 and 203 to 211); the maintainer may want them added.
- **Covered, not repeated.** Real power off and on: QA-GLB-102, QA-GLB-103 and QA-GLB-117 carry the manual press steps. DJ detents: QA-PRF-101. DJ platter and backspin: the manual steps of QA-PRF-133, QA-PRF-134 and QA-PRF-135 (their Unit cell starts with a test, so the source filter does not take them).
- **Not run here.** No firmware was built or flashed for this list; no unit was touched.
