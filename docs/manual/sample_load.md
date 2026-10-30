<!-- manual-sync: 571f81a -->
<!--
Chapter: Sample load. Rules: .claude/skills/manual-writing/SKILL.md. Source: docs/qa/catalogue/sample_load.md, docs/ui/ui-map.md (Sample load row).
Images to add to docs/manual/images.txt (the manifest does not exist yet): sample_load_formats (shot, gallery state formats), sample_load_stereo (shot, gallery state stereo).
-->
# Sample load
<!-- sync: src=app/src/bindings.cpp,app/src/sample_actions.cpp,app/include/o3os/app/sample_load_view.h,app/src/screens/sample_load_screen.cpp -->
<!-- The catalogue has no rows for F2, F3, F4, F6 and F7. -->

Status: draft

Sample load takes a sound file from the card into an instrument slot. It reads WAV, AIFF and 8SVX files by what is inside them, not by their name, and it can load one channel of a stereo file. The official manual accepts plain WAV only and brings every file to 44.1 kHz, 16-bit, one channel. Here a file keeps its own rate. <!-- og: different --> Not checked against the original firmware: the keys. <!-- og: open -->
<!-- TODO: ask: the printed label of the screen button that opens this screen. (code: no answer) -->

## What it is for

Use Sample load to put a sound file from the card into an instrument. Browse the folders, move the cursor to a file, and press `F1` (**LOAD**). You reach the screen from a free slot on the Instrument screen: press `F1` (**SAMPLE**). When the load is done, the screen returns you to the instrument.

## The screen at a glance

![Sample load: two folders first, then AIFF, 8SVX, WAV and WT files together in one list.](img/sample_load_formats.png)

| Area | What it shows |
|---|---|
| Header | **NAME** at the left, then **TARGET INSTRUMENT** and the number of the instrument that gets the file. At the right, **WAV**. |
| Main area | The folders and files of the card, folders first. The cursor marks one row. |
| Info line | How many entries the folder has, and the keys that work in the list, starting with **pad: target**. |
| Strip | Messages, for example **IMPORT <NAME> FAILED**. |
| Key row | `F1` to `F8`, with the labels in the table under "Keys on this screen". |
| Shift row | The jumps through the list and **INST -** / **INST +**, drawn while `Shift` is held. |

> **Try this**:
> 1. Start on Sample load. Press `F3` (**ENTER**) on the `/AIFF` folder, then put the cursor on `afsp_16bit_stereo.aif`.
> 2. Press `F1` (**LOAD**).
> 3. The instrument takes its name from the file.


## More tasks

### Choose the instrument that gets the file
<!-- sync: src=app/src/sample_actions.cpp,app/src/screens/sample_load_screen.cpp qa=QA-SLD-021,QA-SLD-024,QA-SLD-028,QA-SLD-030,QA-SLD-035 -->

The header always says which instrument `F1` loads into: **TARGET INSTRUMENT 07**, for example. The card on the right shows what that slot holds now: its name, **MODULE** and a name for a module instrument, **MIDI** and a name for a MIDI instrument, or **-- empty --** for a free slot. A WAV cannot go into a module slot or a MIDI slot: the strip says **SLOT IS MIDI: INSTRUMENT, SHIFT+F4 DELETE**, and the instrument stays. Any other slot, 49 to 64 too, can take the file. <!-- QA-SLD-024 -->

The card also shows the sample memory: a bar of how much of it is used, and under it **ARENA** with the used and free figures in KB, for example **3284 KB USED  4908 KB FREE** (rounded down; Config shows the same free figure). The line turns the accent colour when less than 4 KB is free. This is new in the Community OS. <!-- QA-SLD-028 -->

Loading over an instrument that already has a sample can give its memory back: when the new sample is not longer than the room the old one had, a project's own sample included, then once playback has stopped and the pads are let go the new sample moves into the old one's room and **USED** goes back. The room the old sample does not need stays with that instrument; it is not given to the others until you load a project again. This does not happen when the new sample is longer than about 3 seconds (131,072 frames), when the instrument is a beat-sliced one, or while something still holds the old sample: you copied the instrument, pasted it, or loaded it from an instrument file a moment ago, so that Undo can bring the old sample back. In those cases the old sample stays and the figure stays up. Saving the project and opening it again frees everything that is no longer used. This is new in the Community OS. <!-- QA-SLD-035 -->

1. Press a pad to make its instrument the target. The pad also plays that instrument for as long as you hold it, so you hear what the slot holds now; a file you loaded a moment ago is heard after the hand-over (the card says **SOUNDS WHEN STOPPED**). Or hold `Shift` and press `F8` (**INST +**) for the next instrument, `F7` (**INST -**) for the one before. After 64 it goes round to 1. <!-- QA-SLD-021, QA-SLD-030 -->
2. Press `F1` (**LOAD**).

A question about the old target (**REPLACE 03 KICK: F1 AGAIN**) goes away when you change the target.

### Hear a file before you load it
<!-- sync: src=app/src/sample_actions.cpp,app/src/audition.cpp -->

Start on Sample load, with the cursor on a `.wav` file.

1. Hold `F5` (**PLAY**). The file starts at once and plays for as long as you hold the key. <!-- QA-SLD-023 -->
2. With `F5` still held, press `Up` or `Down`. The sound of the first file ends and the file now under the cursor starts. On a folder it stays quiet until the cursor reaches a `.wav` again.
3. Let go of `F5` to stop.

A long file starts before it is all read from the card. If the card fails in the middle of the file, the strip says **PREVIEW FAILED** and the part that was read goes on playing.

New Community OS behaviour. Not yet checked on the unit.

### Load a sample file

Start on Sample load. The cursor must be on the file, not on a folder.

1. Move the cursor onto the file with `Up` and `Down`. <!-- QA-SLD-016 -->
2. Press `F1` (**LOAD**). <!-- QA-SLD-003 -->

If the instrument in view already holds something, nothing is replaced yet. The strip asks, for example **REPLACE 03 KICK: F1 AGAIN**, and `F1` reads **REPLACE?**. Press `F1` again within four seconds to load. A free instrument loads at once, with no question. A module instrument (the TB-303, for example) is never replaced: the strip says **SLOT HAS A MODULE: INSTRUMENT, SHIFT+F4 DELETE**. Delete it on the Instrument screen first. A load that was asked for cannot be undone, but it leaves the undo history of your pattern and song as it was. Only the undo of that instrument's own parameter edits ends, and the strip says so. <!-- QA-SLD-019, QA-SLD-020 -->

You should see the instrument named after the file. A one-shot file plays once. A file that says it loops plays in a loop, forward or ping-pong. The file plays at the pitch of the rate it was recorded at.

Not yet checked on the unit.

### Open a folder
<!-- sync: src=app/src/sample_actions.cpp qa=QA-SLD-017,QA-SLD-025,QA-SLD-026,QA-SLD-027 -->

Start on Sample load, with the cursor on a folder.

1. Press `F3` (**ENTER**). The list shows what is in the folder. <!-- QA-SLD-017 -->
2. Press `F2` (**UP**) to go back to the folder above. When the list is read, the cursor stands on the folder you just left (new Community OS behaviour). At the top of the card, the strip says **ALREADY AT THE TOP**. <!-- QA-SLD-026 -->
3. Press `F6` (**RESCAN**) to read the folder again, for example after you changed the card on a computer. When the list is read, the strip says how many items it holds, for example **READ AGAIN: 12 ITEMS**.

On a file, `F3` says **NOT A FOLDER**. `F1` on a folder says **A FOLDER: PRESS ENTER**.

If you visit the File screen and come back, Sample load is in the same folder and the cursor is on the same file again once the list is read (new Community OS behaviour). <!-- QA-SLD-027 -->

### Add a file to a free instrument
<!-- sync: src=app/src/sample_actions.cpp -->

Start on Sample load, with the cursor on a file.

1. Press `F4` (**ADD**). The file goes to the first free instrument slot of the 64, not to the one in view.

If every slot is used, the strip says **NO FREE INSTRUMENT**. With files marked, `F4` loads the marked files instead (next section).

### Load several files in one go
<!-- sync: src=app/src/sample_actions.cpp,app/src/app.cpp,app/src/screens/sample_load_screen.cpp qa=QA-SLD-032,QA-SLD-033,QA-SLD-034 -->

Start on Sample load, in a folder with the files you want. New: the official firmware loads one file at a time here, or marks files with `Shift` and the arrows; this firmware marks with `Shift` + `Right`, because `Shift` + `Up` and `Down` are the page keys.

1. Put the cursor on the first file and press `Shift` + `Right`. A solid square marks the file and the cursor steps down, so pressing it again marks the next file. The info line counts them (**3 marked**) and `F1` and `F4` read **LOAD 3** and **ADD 3**. <!-- QA-SLD-032 -->
2. To take a mark back, put the cursor on the file and press `Shift` + `Right` again. `Shift` + `Left` clears every mark.
3. Choose the target instrument if you want to start somewhere else (a pad, or `Shift` + `F7` and `F8`).
4. Press `F1` to load from the target on, or `F4` to load from the first free instrument.

The marked files load one after the other, in the order of the list, each into the next free instrument. An instrument that is already there is skipped, never replaced, and the search stops at instrument 64: it does not go round to 1. When it is done the strip says **LOADED 3 OF 3**. A second `F1` while it runs says **A MULTI-LOAD IS RUNNING**. <!-- QA-SLD-033 -->

If the free instruments run out, the strip says how many came in and why the rest did not, for example **LOADED 3 OF 5: NO FREE INSTRUMENT**. The two files left over stay marked. A file that cannot be read is skipped and counted (**LOADED 2 OF 3: 1 FAILED**), and the others still load. <!-- QA-SLD-034 -->

The marks go when the folder changes, when you press `F6` (**RESCAN**) and when you leave the screen. While the folder is still being read, the strip says **LIST STILL READING** and nothing is marked: wait for the list to finish first.

### Find a file in a long folder

Start on Sample load, in a folder with more files than fit on one screen. A page is 16 rows.

1. Press `Shift` + `Down` to move down one page. Press `Shift` + `Up` to move up one page. <!-- QA-SLD-016 -->
2. Press `Shift` + `F3` (**2/4**) to go to the middle of the list. `Shift` + `F1` goes to the start (**0/4**), `Shift` + `F2` to **1/4**, and `Shift` + `F4` to **3/4**. <!-- QA-SLD-015 -->
3. Press `Shift` + `F5` (**END**) to go to the last entry. <!-- QA-SLD-015 -->

You should see the cursor move. These keys do not load a sample.

Not yet checked on the unit.

### Load one channel of a stereo file
<!-- sync: src=app/include/o3os/app/sample_load_view.h qa=QA-SLD-011,QA-SLD-012,QA-SLD-013 -->

Start on Sample load. Put the cursor on a stereo file. Its name shows **ST** at the right. A mono file does not.

1. Press `F8` (**CH MIX**). Each press moves on: **CH LEFT**, then **CH RIGHT**, then **CH MIX**. <!-- QA-SLD-011 -->
2. Press `F1` (**LOAD**). The instrument gets only the channel the key shows. <!-- QA-SLD-012 --> <!-- QA-SLD-013 -->
3. Press `F1` on another stereo file. The key still shows the same choice. <!-- QA-SLD-013 -->

You should see the strip say what the file will load: **STEREO FILE LOADS: LEFT**, **STEREO FILE LOADS: RIGHT** or **STEREO FILE LOADS: MIX (L+R)/2**. **CH MIX** loads the mix of both channels, (left + right) / 2. **CH LEFT** and **CH RIGHT** load one channel alone, with no mixing.

The choice is not saved with the project. It lasts until the unit is switched off.

![Sample load, stereo file: the cursor is on a file tagged ST, and F8 reads CH MIX. Press it to change the channel.](img/sample_load_stereo.png)

### Play a stereo file in stereo
<!-- sync: src=app/include/o3os/app/sample_load_view.h qa=QA-FIL-120,QA-FIL-121,QA-FIL-122,QA-FIL-123,QA-FIL-124 -->

An instrument holds one channel, so a stereo file cannot play in stereo from one instrument. You can get the stereo image with two instruments and two tracks. Stereo instruments are planned, not built yet.

Start on Sample load. Put the cursor on a stereo file.

1. Press `F8` until it reads **CH LEFT**. Press `F1` (**LOAD**) to load the file into one instrument. <!-- QA-FIL-120 -->
2. Pick a second free instrument as the target (see above). Press `F8` until it reads **CH RIGHT**, then press `F1` to load the same file. <!-- QA-FIL-121 -->
3. Press `Pattern`. Write the notes on one track with the first instrument. Select the steps and press `Copy`, then move to another track and press `Shift` + `Copy` to paste them. Set the **Instr** column of the second track to the second instrument. <!-- QA-FIL-122 -->
4. Press `Master`, then `F1` (**MIXER**). Set the **PAN** of the first track to 0 and the **PAN** of the second to 100 (see [Mixer](mixer.md)). Leave the pan of both instruments at the centre: the instrument's pan moves the track's, so an instrument off the centre is not hard left or right. <!-- QA-FIL-123 -->
5. Press `Play`. <!-- QA-FIL-124 -->

You should hear the left channel from the left output and the right channel from the right output. Each note uses two voices, because each track plays one instrument. A voice is one playing sound; the unit has a fixed number of them (see the [glossary](glossary.md)). Not yet checked on the unit.

## Keys on this screen

<!-- manual-tables: keys screen=sample_load -->
The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.

| Key | Label | With `Shift` |
|---|---|---|
| `F1` | **LOAD** | **0/4** |
| `F2` | **UP** | **1/4** |
| `F3` | **ENTER** | **2/4** |
| `F4` | **ADD** | **3/4** |
| `F5` | **PLAY** | **END** |
| `F6` | **RESCAN** | - |
| `F7` | **WT AUTO** | **INST -** |
| `F8` | **CH MIX** | **INST +** |

<!-- /manual-tables -->

<!-- manual-tables: hand -->
`F7` and `F8` show a setting, so their labels in the table are the first of several. `F7` shows the wavetable mode: **WT AUTO**, **WT LEN**, **WT 256**, **WT 512**, **WT 1024**, **WT 2048** or **WT OFF**. `F8` shows **CH MIX**, **CH LEFT** or **CH RIGHT** while the cursor is on a stereo file, **CH MONO** on a mono file (the key then says **ONLY FOR A STEREO FILE**), and `--` when there is no file. When the screen lists instrument files (`.pti`), `F4`, `F5`, `F7` and `F8` show `--`. <!-- QA-SLD-011, QA-SLD-012, QA-SLD-022 -->

| Key | What it does | Ref |
|---|---|---|
| `Up`, `Down` | Move the cursor one row. | QA-SLD-016 |
| `Shift` + `Up`, `Shift` + `Down` | Move the cursor one page (16 rows). It stops at the first and last entry. | QA-SLD-016 |
| `F1` (**LOAD**) | Load the file under the cursor into the instrument. | QA-SLD-003, QA-SLD-012, QA-SLD-013 |
| `F2` (**UP**), `F3` (**ENTER**) | Go up one folder, or open the folder under the cursor. | QA-SLD-026 |
| `F4` (**ADD**) | Load the file into the first free instrument. | none yet |
| `F6` (**RESCAN**) | Read the folder again. The strip says **READ AGAIN: n ITEMS** when it is done. | QA-SLD-025 |
| `F7` (**WT AUTO**) | Choose how the next WAV is read as a wavetable: **AUTO** (a Serum-style table by its windows), by length, 256, 512, 1024 or 2048 frames, or **OFF**. The strip names the choice. | QA-SLD-018 |
| `F5` (**PLAY**) | Audition the file under the cursor, without loading it: it starts at once and plays while you hold the key. Move the cursor with the key held to hear the next file. | QA-SLD-006, QA-SLD-023 |
| `F8` (**CH MIX**) | Choose the channel of a stereo file: **CH MIX**, **CH LEFT**, **CH RIGHT**. On a mono file the key shows `--`. | QA-SLD-011, QA-SLD-013 |
| `Shift` + `F1` (**0/4**) | Jump the cursor to the start of the list. | QA-SLD-015 |
| `Shift` + `F2` (**1/4**), `F3` (**2/4**), `F4` (**3/4**) | Jump the cursor to that quarter of the list. | QA-SLD-015 |
| `Shift` + `F5` (**END**) | Jump the cursor to the last entry. | QA-SLD-001, QA-SLD-015 |
| `Shift` + `F7` (**INST -**), `Shift` + `F8` (**INST +**) | Step the target instrument down or up over 1 to 64. After 64 it goes round to 1, and before 1 to 64. | QA-SLD-021 |
| `Shift` + `F6` | Does nothing. The key shows `-`. | QA-SLD-001 |
| `Shift` + `Right` | Mark the file under the cursor (or take its mark back) and step down one row. `F1` and `F4` then load the marked files. | QA-SLD-032, QA-SLD-033, QA-SLD-034 |
| `Shift` + `Left` | Clear every mark. | QA-SLD-032 |

<!-- Open rows, not written: QA-SLD-005 (Amiga pitch of an 8SVX, open). -->

## Good to know

- `Up` and `Down` move the cursor one row, and the encoder does too; turn it quickly and it moves 4 rows a detent, then 8 (new Community OS behaviour, the same in the `File` lists; the numbers are being tuned). `Shift` + the encoder pages a detent at a time. `Left` and `Right` jump by the first letter of the name. <!-- QA-SLD-001, QA-SLD-015, QA-SLD-016, QA-SLD-031 -->
- A file name longer than 32 characters is cut in the middle: the first 25 characters, a **~**, then the last 6, so the end of the name (for example `take2.wav`) stays readable. A stereo file keeps three cells for **ST**, so its name is cut to 29. The card keeps the full name, and Sample load finds the file by it. This is new in the Community OS. <!-- QA-SLD-029 --> Not yet checked on the unit.
- Sample load does not delete. To clear a slot, go to the Instrument screen and press `Shift` + `F4` (**DELETE**). It asks nothing, and one `F8` (**UNDO**) brings the instrument back. The pattern steps stay. <!-- QA-INS-030 -->
- To copy an instrument, press `Shift` + `F2` (**COPY**) on the Instrument screen, then `Shift` + `F3` (**PASTE**) on a free slot. The copy shares its audio with the source and keeps playing when the source is deleted. <!-- QA-INS-044, QA-INS-048 -->
- The file's header decides its format, not its name. An AIFF named `.wav` loads as an AIFF. <!-- QA-SLD-006 -->
- Audition (`F5`) on a file that is not `.wav` says **NO PREVIEW OF THIS TYPE: LOAD IT**. <!-- QA-SLD-006 -->
- A file that is neither format says **IMPORT <NAME> FAILED** and changes nothing. <!-- QA-SLD-006 -->
- A damaged file says **IMPORT <NAME> FAILED: DAMAGED FILE**. <!-- QA-SLD-014 -->
- An AIFF with its sound data before its format header says **AIFF: SSND BEFORE COMM NOT SUPPORTED**. This is a known limit. It is not a damaged file. <!-- QA-SLD-014 -->
- AIFF files with NONE or sowt compression load. Another compression says **IMPORT <NAME> FAILED**. <!-- QA-SLD-003 -->
- An 8SVX file takes the name stored in the file. A repeat part loops forward. <!-- QA-SLD-004 -->
- AIFF and 8SVX loading is **Not yet checked on the unit**. <!-- QA-SLD-002 QA-SLD-003 QA-SLD-004 -->
- A sample at a rate other than 44,100 Hz plays at the pitch of its own rate. Earlier builds played it too high. A sample loaded before this change may sound lower now. Retune its **TUNE** once. <!-- QA-SLD-007 QA-SLD-008 -->
- A sample saved at another rate reloads at that rate and keeps its pitch. <!-- QA-SLD-010 -->

## See also

- [Sample edit](sample_edit.md): change a loaded sample with normalise, DC removal and the Amiga conversion.
- [Instrument](instrument.md): the instrument you load into, its play modes and its name.
