<!-- manual-sync: 571f81a -->
<!--
Chapter: Record (deep). Rules: .claude/skills/manual-writing/SKILL.md. Source: docs/qa/catalogue/record.md (QA-REC-001 to 050), docs/specs/OS-LOOPER.md. Walk-through: sim/scenarios/manual/record_walk.txt (the simulator has no recorder: the pictures are the gallery states).
-->
# Record
<!-- sync: src=app/src/screens/record_screen.cpp,app/src/record_actions.cpp,app/include/o3os/app/record_view.h,app/include/o3os/app/recorder.h,audio/include/o3os/audio/loop_player.h -->
<!-- The start row prints START ON (ALIGN is its internal name). The loop fade is 32 frames (0.7 ms). -->

Status: draft

The Record screen is a looper as well as a sampler. Set the **LOOP** row to 1 to 16 bars and a take records exactly those bars, then repeats in time with the pattern while you add layers on top of it. Save the result and it becomes an instrument. The three pages, **SOURCES**, **RESAMPLE** and **RADIO**, and the take, trim and save flow are the screen's other half. New in track3r-os: the looper. <!-- og: new -->

## What it is for

Use Record to capture sound into a sample, or to build a loop over the pattern. Open the screen with the `Sample Rec` button. Come here when you want to sample a line input, the microphone, the radio or the computer, or to resample your own mix. <!-- og: open -->

![Record, SOURCES page: the LOOP row is the last row, and sets the loop length.](img/record_loop_row.png)

## How it works
<!-- sync: src=app/src/record_actions.cpp,audio/include/o3os/audio/loop_player.h spec=docs/specs/OS-LOOPER.md qa=QA-REC-020,QA-REC-021,QA-REC-023,QA-REC-027,QA-REC-028,QA-REC-029,QA-REC-031 -->

A loop pedal repeats what you played and lets you play over it. The looper does the same, except that its length is a number of pattern bars and it restarts on the bar, so it stays in time with the pattern. <!-- QA-REC-023 -->

### A loop: live, stored and ended

With **LOOP** on, press `F1` and the take waits for the next bar. It records exactly the bars you set, then ends by itself on the bar. <!-- QA-REC-021 -->

- **Live:** the loop sits in the sample memory and plays in time with the sequencer, on whatever screen you are on. It is not written to the project. <!-- QA-REC-027 -->
- **Stored:** only when you press `F3` (**SAVE**). The loop, with its layers, goes into the first free instrument slot, for example **LOOP 4B**, with play mode LOOP FWD over the whole sample. <!-- QA-REC-025 -->
- **Ended:** by `F4` (**DISCARD**), by a project load or NEW, or by saving it. A Stop of the transport only fades it out, and Play brings it back on the first bar. <!-- QA-REC-023 QA-REC-026 -->

Example: set **LOOP** to 4 BARS over a four-bar drum pattern, play a bass line into the line input, and the bass repeats against the drums on every pass.

### A layer: what an overdub adds

A layer is one pass of overdub: the sound you add to a loop while it repeats. Press `F1` (or `Rec`) in the loop review and what the input carries is written onto the loop where it plays. You hear the new layer from the next pass. Press `F1` again to end it. <!-- QA-REC-028 -->

**FEEDBACK** decides what happens to the old sound under the new. At 100 nothing fades and every layer stays. At 0 the new input replaces the old. In between, older layers fade each pass. <!-- QA-REC-031 -->

Example: record a one-bar kick loop, then overdub a hat for one pass. At FEEDBACK 100 you keep both. Press `F5` (**UNDO LYR**) and the hat goes. Layers add up, so a loud layer on a loud loop can clip.

### What the loop listens to

The loop takes its sound from the source it was made from. On **RESAMPLE** that is your mix before the loop. While the loop exists, the **SOURCE**, **TAP**, **FREQUENCY** and **REGION** rows are dim, because its source is locked. <!-- QA-REC-022 QA-REC-028 -->

If you want to know more:

- **UNDO LYR** takes back one layer: the last overdub session. A new session drops the old undo. <!-- QA-REC-029 -->
- **LOOP VOL** sets the loop's level, from 0 to 100. <!-- QA-REC-022 -->
- The loop goes to the line out after the master chain. It is not in the meters, the stems or any take.

![Record, loop review: the header reads REVIEW with the loop length, and the key row offers OVERDUB, SAVE, DISCARD, UNDO LYR and LOOP ON.](img/record_loop_review.png)

## Try it
<!-- sync: src=app/src/screens/record_screen.cpp,app/src/record_actions.cpp qa=QA-REC-020,QA-REC-021,QA-REC-025,QA-REC-028,QA-REC-029 -->

1. Press `Sample Rec`, then `Down` to **LOOP**, and turn the encoder to **1 BAR**. <!-- QA-REC-020 -->
2. Press `Play`, then `F1`. The header says **ARMED - LOOP 1 BAR**. <!-- QA-REC-021 -->
3. Play a sound into the input. The take ends on the bar: the header says **REVIEW - LOOP 1 BAR** and the loop repeats. <!-- QA-REC-021 -->
4. Press `F1` (**OVERDUB**), play a second sound for one bar, then press `F1` again. <!-- QA-REC-028 -->

   ![Record, overdub: the header shows OVERDUB and the key F1 is lit.](img/record_loop_overdub.png)

5. Listen: both sounds play on the same bar. Press `F5` (**UNDO LYR**) and the second one goes. <!-- QA-REC-029 -->
6. Press `F3` (**SAVE**) to keep the loop as an instrument, or `F4` (**DISCARD**) to throw it away. <!-- QA-REC-025 -->

The simulator has no recorder, so its scenario `record_walk.txt` checks only the screen and the page turn (the `Sample Rec` button), not these steps, and this walk-through has no pictures of its own: the three above come from the screen's gallery. Not yet checked on the unit.

## The screen at a glance

| Area | What it shows |
|---|---|
| Header | The state of a take or loop, for example **REVIEW - LOOP 4 BARS**. |
| Rows | The settings of the page. `Up` and `Down` move the cursor; the encoder, or `Left` and `Right`, changes the value. |
| Strip | Short messages, for example **RECORDING IN 4.. 3.. 2.. 1..**. |
| Key row | `F1` to `F8`. Under a loop review the whole row changes. |
| Shift row | The labels drawn while `Shift` is held. See the table under "Keys on this screen". |

## More tasks

### Record a sample
<!-- sync: src=app/src/screens/record_screen.cpp,app/include/o3os/app/recorder.h,app/src/recorder.cpp,app/src/record_actions.cpp qa=QA-REC-010,QA-REC-036,QA-REC-037,QA-REC-039,QA-REC-040,QA-REC-041,QA-REC-042,QA-REC-043,QA-REC-044 -->

Start on the **SOURCES** page.

1. Press `Down` to **SOURCE** and turn the encoder: **LINE**, **LINE L**, **LINE R**, **MIC LG**, **MIC HG**, **RADIO** or **USB**. The **RESAMPLE** page has **TAP** in its place (**MASTER**, **REC BUS** or **TRACK**) and the **RADIO** page has **FREQUENCY** and **REGION**.
2. Press `Down` to **GAIN** and turn the encoder from 0 to 100 %. Each input type keeps its own gain. It reads **--** for **USB**.
3. Press `Down` to the start row, **START ON**, and turn the encoder. Choose **FREE** to start at once, **STEP**, **BAR** or **PATTERN** to wait for that point, or **LEVEL** to start when the sound reaches a level (see "Start a take on a level" below).
4. Press `F1`. With the transport running, the take waits for that point, and the strip counts down: **RECORDING IN 4.. 3.. 2.. 1..**. <!-- QA-REC-010 -->
5. A Stop of the transport before the take starts gives the take up. <!-- QA-REC-010 -->
6. Press `F1` again to end the take. With **STOP AT** on **STEP**, **BAR** or **PATTERN** it ends at that point, and a second `F1` ends it at once. With **STOP AT** on a number of bars, it ends by itself.
7. The screen says **TAKE: TRIM, THEN SAVE**. Set **TRIM START** and **TRIM END**. Press `F3` to save the window into an instrument (the keyboard opens at once over its name, **RECORDING**: type a name and press `F8`, or press `F7` to keep **RECORDING**; the take is saved either way), or `Shift` + `F3` (**CROP**) to cut the take down to the window and keep working on it, or `F4` to throw it away (with nothing recorded, `F4` says **NOTHING RECORDED**). `F4`, a new take with `F1` and **CROP** each ask first (**TAKE NOT SAVED: F4 AGAIN**, **CROP KEEPS THE WINDOW ONLY: SHIFT+F3 AGAIN**): press the same key again within four seconds. A take you have written with `F7` (**SAVE WAV**) is saved, so it needs no question until you move a marker. <!-- QA-REC-040 QA-REC-041 QA-REC-042 QA-REC-044 -->

`F7` (**SAVE WAV**) writes the trimmed window as a 16-bit mono WAV at 44,100 Hz into the `Samples` folder of the card, as `REC_001.wav`, `REC_002.wav` and so on. The strip says **SAVE REC_001 DONE** and the take stays in review, so `F3` can still make an instrument of it. With no take it says **NO TAKE TO SAVE: F1 RECORDS ONE**, with no card write layer **NO CARD WRITE LAYER**, and on a write-protected card **CARD IS WRITE PROTECTED**. Not yet checked on the unit. <!-- QA-REC-039 -->

Press `F2` (**MONITOR**) on the **SOURCES** page to hear the input through the output. It still works while a take is armed or recording. On **RESAMPLE** it does nothing. New in track3r-os. <!-- QA-REC-037 -->

While the take records, **MEM** in the top bar reads 100 %, because the take holds all the free sample memory. When the take ends, **MEM** drops to the memory that was in use before, plus the size of the take. `F4` gives that memory back at once and **MEM** returns to the level from before the take; `F3` keeps it in the instrument. <!-- QA-REC-036 -->

The card beside the rows shows **TIME LEFT**, the longest take you can still make: before the take it is the free sample memory in seconds, while the take records it counts down, and at zero the take ends (**THE SAMPLE MEMORY FILLED: THE TAKE ENDED**). It is gone under the review, and a loop does not show it, because a loop is as long as its bars. This is new in track3r-os; the official firmware does not show a maximum. <!-- QA-REC-043 -->

### Start a take on a level
<!-- sync: src=app/src/screens/record_screen.cpp,app/src/record_actions.cpp,app/include/o3os/app/record_view.h,app/include/o3os/app/recorder.h,app/src/recorder.cpp,audio/include/o3os/audio/taps.h qa=QA-REC-045,QA-REC-046,QA-REC-047,QA-REC-048,QA-REC-049,QA-REC-050 -->

New in track3r-os: the official firmware has no such start. <!-- og: new --> <!-- QA-REC-045 QA-REC-046 QA-REC-047 QA-REC-048 QA-REC-049 QA-REC-050 -->

Set **START ON** to **LEVEL** and two rows join the page under it. **THRESHOLD** is the level the sound must reach, from **-60 DB** to **-6 DB** in steps of 3 (`Shift` + encoder moves 9 dB). It starts at **-30 DB**. **PRE-ROLL** is how much sound from before the level the take keeps: **0**, **10**, **20**, **50** or **100 MS**. It starts at **50 MS**. `Delete` puts the row under the cursor back to its default. The rows are on all three pages, and only while **START ON** is **LEVEL**.

1. Set **START ON** to **LEVEL**, then **THRESHOLD** and **PRE-ROLL**.
2. Press `F1`. The card says **WAITING FOR LEVEL**, and the clock stays at 0. Nothing is recorded yet, but the last **PRE-ROLL** of sound is kept all the time.
3. When the sound reaches **THRESHOLD**, the take starts and the card says **RECORDING**. The take begins **PRE-ROLL** before that point, so the start of the sound is not cut off.
4. Press `F1` to end the take, as for any other take. Press `F1` while it still waits for the level to cancel: nothing is kept, and no take is made.

**LEVEL** does not need the transport. It works with the transport stopped, and it does not count in, even when **COUNT-IN** is on. It never falls back to **FREE** (the other waiting starts do when the transport is stopped). The two rows are runtime settings only: nothing is stored, and a restart puts them back to **-30 DB** and **50 MS**. They are not shown while a take is under review: save it (`F3`) or throw it away (`F4`) to change them.

With **LOOP** on, a loop must start on a bar, so **START ON** reads **BAR** (as it does for any loop), the two rows are hidden and **F1** waits for the bar. **LEVEL** itself is kept and is back when **LOOP** is off.

Limits:

- The level is read on the sound that the take is made of: the REC bus, the master (**MAIN**) or the track the page taps, the same signal as the two meters beside the rows. It is not the level at the codec input.
- One sample at or above **THRESHOLD** starts the take: there is no hysteresis and no hold, so a click can start it.
- The start comes from the sound, not from the sequencer. A step of the sequencer that falls in the same short block (64 frames, about 1.5 ms) as the level is not counted, so a **STOP AT** of **STEP**, **BAR** or **PATTERN** ends one boundary later when its boundary is in that block.
- **PRE-ROLL** is kept at least: it is rounded up to an even number of frames, and one frame more can be kept. If the level arrives less than **PRE-ROLL** after `F1`, nothing was heard before it: the first frames of the take are silence, and **TRIM START** starts after them.
- A Stop of the transport does not end a take that waits for a level, because it does not need the transport. `F1` ends it, and it ends with no take.
- With the sample memory nearly full, `F1` says **NO ROOM IN THE SAMPLE MEMORY**: **LEVEL** needs room for the pre-roll and about a second of take.
- Not checked on the unit yet. <!-- og: new -->

### Record what the computer plays
<!-- sync: src=app/include/o3os/app/recorder.h qa=QA-REC-034 -->

The **USB** source records the sound that the computer plays into the Tracker. The unit must be connected to the computer, with **USB AUDIO** and **USB RETURN** both **On** on the USB page of [Config](config.md). They are On by default.

Start on the **SOURCES** page.

1. Press `Down` to **SOURCE** and turn the encoder to **USB**. **GAIN** reads **--**, because USB has no gain. The take has the level that the computer plays. <!-- QA-REC-034 -->
2. Play the sound on the computer, to the Tracker as its audio output.
3. Press `F1` to start the take, and `F1` again to end it. The take is mono: the left and right sides are averaged. <!-- QA-REC-034 -->
4. The screen says **TAKE: TRIM, THEN SAVE**. Press the pad at the take's own pitch (**C-4**, pad 12) to hear it. Press `F3` to save it, or `F4` to throw it away (it asks first: press `F4` again).

Not yet checked on the unit. <!-- QA-REC-034 -->

### Record a loop

Start on the **SOURCES**, **RESAMPLE** or **RADIO** page. The loop row is on all three.

1. Press `Down` to **LOOP** and turn the encoder to the number of bars, from **1 BAR** to **16 BARS**. Hold `Shift` and turn it to move four bars at a time. <!-- QA-REC-020 -->
2. Press `Play`, then `F1`. The header says **ARMED - LOOP** and the number of bars. The take starts on the next bar. <!-- QA-REC-021 -->
3. Wait. The take records exactly the bars you set. It ends by itself on the bar, and the screen shows **REVIEW - LOOP** and the number of bars. The loop repeats on the bars. <!-- QA-REC-021 QA-REC-023 -->

![Record, loop review: the header reads REVIEW with the loop length, and the key row offers OVERDUB, SAVE, DISCARD, UNDO LYR and LOOP ON.](img/record_loop_review.png)

### Add a layer (overdub)

Start on the review of a loop.

1. Press `F1` (or `Rec`). The header gains **- OVERDUB**. The screen says **OVERDUB: LAYERING**, and `F1` is lit. <!-- QA-REC-028 -->
2. Make the sound the loop was recorded from. Each pass adds it on top of the loop, and you hear the new layer from the next pass. <!-- QA-REC-028 -->
3. Press `F1` again to end the layer. <!-- QA-REC-028 -->

> **Tip**: With the transport stopped, `F1` starts it. <!-- QA-REC-028 -->

With the transport stopped, `F1` first counts in when **COUNT-IN** is 1 or 2 bars (set on the Config screen). The count-in plays the pattern that `Play` starts. Nothing is recorded during it. The layer begins on the loop's first bar after the count-in, and fades in over a very short ramp. With **COUNT-IN** Off, the transport and the layer start together. Not yet checked on the unit. <!-- QA-REC-032 -->

![Record, overdub: the header shows OVERDUB and the key F1 is lit.](img/record_loop_overdub.png)

### Set how much a layer keeps

Start on the review of a loop. FEEDBACK is used only while overdub is on.

1. Press `Down` to **FEEDBACK**, the last row, and turn the encoder. It goes from 0 to 100. The default is 100. <!-- QA-REC-031 -->
2. Hold `Shift` and turn the encoder to move in steps of 5. Press `Delete` to set 100. <!-- QA-REC-031 -->

The change is heard from the next pass, and only while overdub is on. <!-- QA-REC-031 -->

### Undo the last layer

Start on the review of a loop, after an overdub.

1. Press `F5` (**UNDO LYR**). It is dim while the loop has no layer. <!-- QA-REC-029 -->
2. Wait. The loop fades out. The header says **- UNDO**, and the screen says **UNDOING THE LAYER**. <!-- QA-REC-029 -->
3. When the restore is done, the loop fades in on its next restart. You hear the loop without the last layer. <!-- QA-REC-029 -->

### Mute the loop

Start on the review of a loop.

1. Press `F8`. The key reads **MUTED**, and the header gains **- MUTED**. It keeps its place on the bars. <!-- QA-REC-024 -->
2. Press `F8` again. The key reads **LOOP ON**, and the loop is heard where the bars have it. <!-- QA-REC-024 -->

### Save the loop as an instrument
<!-- sync: src=app/src/record_actions.cpp qa=QA-REC-025,QA-REC-030 -->

Start on the review of a loop.

1. Press `F3` (**SAVE**). <!-- QA-REC-025 -->
2. The loop, with its layers, goes into the first free instrument slot, named for example **LOOP 4B**, in LOOP FWD over the whole sample. The keyboard opens over that name: type a new one and press `F8`, or press `F7` to keep it. The looper is then free. <!-- QA-REC-025 QA-REC-030 -->

If no instrument slot is free, nothing is saved and the strip says **NO FREE INSTRUMENT SLOT**.

### Discard the loop

> **Careful**: Discard throws away the loop, its layers and its undo. Nothing brings them back.

Start on the review of a loop.

1. Press `F4` (**DISCARD**). The strip asks **TAKE NOT SAVED: F4 AGAIN**; press `F4` again. The strip says **TAKE DISCARDED**. The loop fades out within one block. <!-- QA-REC-026 QA-REC-040 -->

## Keys on this screen

<!-- manual-tables: keys screen=record -->
The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.

| Key | Record | Loop review | Loop review, muted | `Shift`, SOURCES and RESAMPLE | `Shift`, RADIO |
|---|---|---|---|---|---|
| `F1` | **REC/STOP** | **OVERDUB** | **OVERDUB** | - | - |
| `F2` | **MONITOR** | **MONITOR** | **MONITOR** | - | - |
| `F3` | **SAVE** | **SAVE** | **SAVE** | **CROP** | **CROP** |
| `F4` | **DISCARD** | **DISCARD** | **DISCARD** | - | - |
| `F5` | **SEEK-** | **UNDO LYR** | **UNDO LYR** | - | **CH-10** |
| `F6` | **SEEK+** | - | - | - | **CH+10** |
| `F7` | **SAVE WAV** | **SAVE WAV** | **SAVE WAV** | - | - |
| `F8` | **PREVIEW** | **LOOP ON** | **MUTED** | - | - |

<!-- /manual-tables -->

| Key | What it does | Ref |
|---|---|---|
| `Up`, `Down` | Move the cursor between the rows. | QA-REC-012 |
| `Left`, `Right` | Change the value of the row under the cursor, one click of the encoder down or up. New in track3r-os. | QA-REC-060 |
| `Shift` + `Left`, `Shift` + `Right` | The same in the coarse step, as `Shift` + the encoder. | QA-REC-061 |
| `Sample Rec` (again) | Turns the page through SOURCES, RESAMPLE and RADIO, and wraps after RADIO. Each page keeps its row. `Left` and `Right` never turn the page. | QA-REC-012, QA-REC-062 |
| the encoder | Change the value of the row under the cursor. | QA-REC-020, QA-REC-031 |
| `Shift` + the encoder | LOOP: moves four bars. FEEDBACK: moves in steps of 5. | QA-REC-020, QA-REC-031 |
| `Delete` | LOOP: OFF. FEEDBACK and LOOP VOL: 100. | QA-REC-020, QA-REC-022, QA-REC-031 |
| `F1` | Arms a take. Under a loop review: **OVERDUB**, on and off, with the count-in if it is on. | QA-REC-010, QA-REC-021, QA-REC-028, QA-REC-032 |
| `Rec` | Under a loop review: the same as `F1`. | QA-REC-022, QA-REC-028 |
| `F2` (**MONITOR**) | Hear the input through the output, on and off. Nothing on RESAMPLE. | QA-REC-037 |
| `F3` (**SAVE**) | Save the loop as an instrument. | QA-REC-025, QA-REC-030 |
| `F4` (**DISCARD**) | Throw the loop away. It asks first: press `F4` again. | QA-REC-026, QA-REC-040 |
| `F5` (**SEEK-**), `F6` (**SEEK+**) | RADIO page: the radio's own seek, down and up. Nothing on the other pages. | QA-REC-038 |
| `F7` (**SAVE WAV**) | Write the take's window as a WAV on the card. | QA-REC-039 |
| `Shift` + `F3` (**CROP**) | Cut a take under review down to its window. It asks first. Not for a loop. | QA-REC-041 |
| `F5` (**UNDO LYR**) | Undo the last layer. Dim with no layer. | QA-REC-022, QA-REC-029 |
| `F8` (**LOOP ON** / **MUTED**) | Mute or unmute the loop. | QA-REC-024 |
| `Play` | Start and stop the transport. The loop follows it. | QA-REC-023 |

The Shift row of the Record screen:

| Key | SOURCES and RESAMPLE | RADIO | Ref |
|---|---|---|---|
| `Shift` + `F5` | nothing | **CH-10**: tunes ten channels down, with no seek | QA-REC-001, QA-REC-011 |
| `Shift` + `F6` | nothing | **CH+10**: tunes ten channels up, with no seek | QA-REC-001, QA-REC-011 |
| `Shift` + `F3` (**CROP**) | cuts the take down to its window | the same | QA-REC-001, QA-REC-041 |
| any other `Shift` + `F1` to `F8` | nothing | nothing | QA-REC-001 |

On the RADIO page, the plain `F5` (**SEEK-**) and `F6` (**SEEK+**) ask the radio chip for the next station down or up, wrapping at the end of the band. The page reads **SEEKING** while the chip looks, then **LISTENING**, and the frequency is what it found. Under a loop review `F5` is **UNDO LYR** and `F6` does nothing. Not yet checked on the unit: a real station needs an antenna. <!-- QA-REC-011 QA-REC-038 -->

## Good to know
<!-- sync: src=app/src/record_actions.cpp spec=docs/specs/OS-LOOPER.md qa=QA-REC-010,QA-REC-031,QA-REC-027,QA-REC-020,QA-REC-021,QA-REC-023,QA-REC-029,QA-REC-032,QA-REC-033 -->

- New in track3r-os: the loop, the overdub and the undo layer. Not yet checked on the unit: the looper. <!-- og: new --> <!-- QA-REC-020 to QA-REC-031 -->
- Not checked against the original firmware: the sources, the take and trim flow and the radio page. <!-- og: open -->
- The loop keeps playing on other screens, like a loop pedal. `F8`, `F3` and `F4` work only on this screen. <!-- QA-REC-027 -->
- The LOOP row is frozen while a take runs. With LOOP on, **START ON** reads **BAR** and **STOP AT** reads **LOOP**, both dim. Editing either says why. <!-- QA-REC-020 -->
- A loop is refused at `F1` if it cannot fit the free sample memory: **NO ROOM IN THE SAMPLE MEMORY FOR THE LOOP**. A take that ends early, or fills the sample memory, is given up with **THE LOOP TAKE ENDED BEFORE ITS LAST BAR: GIVEN UP** or **THE SAMPLE MEMORY FILLED: THE TAKE ENDED**. A project load, NEW or a kit gives it up with **TAKE LOST: A PROJECT WAS LOADED UNDER IT**. <!-- QA-REC-021 -->
- A Stop fades the loop out in under a millisecond. After Play, a loop starts again from its first bar, not from where it stopped. A tempo change keeps the loop's length: a faster tempo cuts it short, a slower one leaves a gap. The header then says **- TEMPO CHANGED**. <!-- QA-REC-023 -->
- During an undo restore, `F1` and `F3` say **UNDO IN PROGRESS**. `F4` and `F8` still work. <!-- QA-REC-029 -->
- If the transport does not start after the count-in, the strip says **THE COUNT-IN DID NOT START** or **THE TRANSPORT DID NOT START**. The overdub ends again, with no layer. <!-- QA-REC-032 --> Not yet checked on the unit.
- If the sound does not answer when you press `F3` (**SAVE**) during an overdub, the keys ignore presses for up to half a second. Then the strip says **SAVE TIMED OUT: TRY AGAIN**. Nothing is saved, and the loop stays on the screen. Once the sound runs again, the overdub is over, and `F3` saves the loop as usual. It cannot be made to happen by hand on the unit, so it is checked by the host tests only. <!-- QA-REC-033 -->
- The loop goes to both channels of the line out after the master chain. The master volume scales it. It is not in the meters, the stems or any take.

## See also

- [Sample edit](sample_edit.md): change a sample after you record or save it.
- [Sample load](sample_load.md): bring a sample from the card into an instrument.
- [Config](config.md): the count-in, which plays before a take when it is on.

<!-- Open rows, not written: none. -->
