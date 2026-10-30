<!-- manual-sync: f5e29c02 -->
<!--
Chapter: Config. Rules: .claude/skills/manual-writing/SKILL.md. Source: docs/qa/catalogue/config.md (USB and PATTERN rows), the Config row of docs/ui/ui-map.md, and the cited rules of docs/specs/OS-METRO.md and docs/specs/OS-RECMICRO.md.
Images: snaps qa_cfg_016 and qa_cfg_003 of sim/scenarios/qa/qa_config.txt, and the walk-through sim/scenarios/manual/config_walk.txt (OS-MANUAL2 C5).
Open rows, not written (sim none, unit pending only): QA-CFG-014, QA-CFG-024, QA-CFG-202, QA-CFG-206 (a pointer to the Global rows).
-->
# Config

Status: draft

Config holds the unit's settings on six pages. Three things are new in track3r-os: the USB page can turn the audio device and the audio return off, CARD AS DISK hands the card to a computer while the unit plays on, and the metronome can click on the Main outputs with a count-in of one or two bars. <!-- og: new --> The rest of the PATTERN page differs from the official manual. The metronome has no time-signature rows. Live recording has the rows REC MICRO and REC VELOCITY. PAD OFFSET sets how the pad rows overlap, while the root and scale come from the held Note panel. PAT SWITCH chooses whether a pattern picked with `F1` held lands at the next step or at the end of the pattern. <!-- og: different --> A change to USB AUDIO or USB RETURN waits until you press `F7` (**APPLY**) twice, and `Shift` + `F7` puts a page back to its defaults. Not checked against the original firmware: the pages and keys. <!-- og: open -->

## What it is for

Use the Config screen to change the unit's settings. Press `Config` to open it. It has six pages, one for each of `F1` to `F6`: MIDI, DISPLAY, SD, INFO, USB and PATTERN. This chapter covers the MIDI, DISPLAY, USB and PATTERN pages and the keys that work on every page. The SD and INFO pages have no rows to set. The SD page shows what the card reports (CARD, MOUNTED, FILE SYSTEM, CAPACITY and CLUSTER). Not yet checked on the unit. <!-- QA-CFG-217 --> The USB page also has the **CARD AS DISK** row, which gives the card to a computer while the unit keeps playing, and the **READ ONLY** row beside it: see [USB disk](usb_disk.md). The INFO page has one key, `F8` (**UPDATE**), which installs a new firmware from the card.

## How it works
<!-- sync: src=app/src/screens/config_screen.cpp,model/src/params.cpp qa=QA-CFG-002,QA-CFG-008,QA-CFG-010,QA-CFG-038 -->

A Config row is a setting of the unit, not of the project. Load another project and the rows stay as they are: they are kept in the unit's settings file on the card. The one exception is READ ONLY on the USB page, which is for this session only and is not saved. <!-- QA-CFG-038, QA-CFG-200 -->

Most rows are live. Turn the encoder and the change takes effect at once, with nothing to confirm: RETURN LEVEL moves the level the moment you turn it. <!-- QA-CFG-008 --> The two USB rows that change what the computer sees, USB AUDIO and USB RETURN, are different. The encoder only sets a pending value, marked with a star (\*). Nothing is written and the computer notices nothing until you press `F7` (**APPLY**) twice, because applying can make the USB port reconnect. Leaving the page, or turning to another page, throws a pending value away. <!-- QA-CFG-002, QA-CFG-010 -->

Example: set METRO to Cue and COUNT-IN to 1 bar, and the next live take starts with a click. Both rows are live and are kept until you change them. Set USB AUDIO to Off and press `F1` instead of `F7`, and the unit forgets it.

## Try it
<!-- sync: src=app/src/screens/config_screen.cpp qa=QA-CFG-002,QA-CFG-010,QA-CFG-017,QA-CFG-018 -->

This walk-through changes a live row, then shows a pending one that is dropped.

1. Press `Config`.
2. Press `F6` (**PATTERN**). The nine rows of the page show.

   ![Config, PATTERN page: the nine rows, the cursor on the first.](img/config_walk_2.png)
3. Press `Down` four times. The cursor is on METRO. Turn the encoder one click to the right: the row reads **Main**. <!-- QA-CFG-017 -->
4. Turn the encoder two clicks to the left. The row reads **Off**. The Pattern screen's info line loses its METRO chip. <!-- QA-CFG-018 -->

   ![Config, PATTERN page: METRO reads Off.](img/config_walk_4.png)
5. Press `F5` (**USB**). Turn the encoder one click to the left. USB AUDIO reads **Off\*** and the info line says it is not applied yet. <!-- QA-CFG-002 -->

   ![Config, USB page: USB AUDIO reads Off with a star, not applied.](img/config_walk_5.png)
6. Press `F1` (**MIDI**), then `F5` again. USB AUDIO reads **On** with no star: the pending value is gone and the computer never noticed. <!-- QA-CFG-010 -->

   ![Config, USB page: USB AUDIO reads On again, with no star.](img/config_walk_6.png)

METRO stays **Off** until you change it. Press `Delete` on the METRO row to put **Cue** back.

## The screen at a glance

![Config, PATTERN page: the nine rows, from REC POLY to PAT SWITCH, with the metronome rows in the middle.](img/config_qa_cfg_016.png)

| Area | What it shows |
|---|---|
| Main area | The rows of the page. Each row has a name and a value. |
| Info line | Notes about the page or a pending change. On PATTERN it says **live recording, metronome and pads**. |
| Strip | The message line, for example **USB RECONNECTING**. |
| Key row | `F1` to `F6` choose a page. `F7` is **APPLY** (blank on the SD and INFO pages, which have nothing to apply). On the INFO page, `F8` is **UPDATE**: it opens the firmware update dialog (new, not yet tried on a unit). |
| Shift row | `F7` is **DEFAULTS**. The other keys do nothing. |

## More tasks

### Turn USB AUDIO off, then on

USB AUDIO sets whether the unit shows an audio device to the computer. Off gives **STANDARD**: MIDI and serial only. On is the default, and adds the audio device.

Start on the Config screen. Press `F5` (**USB**).

1. Turn the encoder until USB AUDIO reads **Off**. The row shows a star (\*). The info line says the change is not applied yet. <!-- QA-CFG-002 -->
2. Press `F7` (**APPLY**). The strip says **USB WILL RECONNECT: PRESS AGAIN**, and the key reads **APPLY?**, inverted. <!-- QA-CFG-003, QA-CFG-034 -->
3. Press `F7` (**APPLY?**) again. The strip says **USB RECONNECTING**. The star goes. <!-- QA-CFG-004 -->

![Config, USB page: after the first APPLY, the strip says USB WILL RECONNECT: PRESS AGAIN.](img/config_qa_cfg_003.png)

The computer sees the unit again, with MIDI and serial but no audio device. To turn USB AUDIO back on, turn the encoder until the row reads **On**, then press `F7` twice. The computer sees the audio device again, named **Track3r Audio**. <!-- QA-CFG-011 -->

Not yet checked on the unit.

### Change USB RETURN
<!-- sync: src=app/src/screens/config_screen.cpp qa=QA-CFG-005,QA-CFG-006,QA-CFG-012,QA-CFG-007,QA-CFG-013 -->

The USB return is the stereo stream from the computer to the unit. **On** (the default) adds it. **Off** leaves capture only.

Start on the USB page.

1. Press `Down`. The cursor is on USB RETURN. <!-- QA-CFG-005 -->
2. Turn the encoder until the row reads **Off**. The row shows a star (\*). <!-- QA-CFG-005 -->
3. Press `F7` (**APPLY**). With USB AUDIO Off, the strip says **PRESS AGAIN TO APPLY**. With USB AUDIO On, it says **USB WILL RECONNECT: PRESS AGAIN**. <!-- QA-CFG-006 --> <!-- QA-CFG-012 -->
4. Press `F7` (**APPLY**) again. With USB AUDIO Off, the strip says **APPLIED**, and the computer sees no change. With USB AUDIO On, the strip says **USB RECONNECTING**, and the computer reconnects once. <!-- QA-CFG-007 --> <!-- QA-CFG-013 -->

The return needs room on the USB bus, so it is off when the unit presents 10 outputs (the Mixer's STEMS page chooses the number). The row then reads **On (10 OUT: off)**, dim. The setting stays On: with 8 outputs or fewer the return is back. <!-- QA-CFG-250 --> <!-- QA-CFG-251 -->

Not yet checked on the unit.

### Set RETURN LEVEL
<!-- sync: src=app/src/screens/config_screen.cpp,model/src/params.cpp qa=QA-CFG-008 -->

RETURN LEVEL sets how loud the USB return reaches the outputs. **100 %** is the default.

Start on the USB page.

1. Press `Down` twice. The cursor is on RETURN LEVEL. <!-- QA-CFG-008 -->
2. Turn the encoder until the row reads **90 %**. <!-- QA-CFG-008 -->

You should see **90 %** at once, with no star. `F7` is not needed. Not yet checked on the unit: the level the computer receives. <!-- QA-CFG-008 -->

### Share the card with a computer
<!-- sync: src=app/src/screens/config_screen.cpp qa=QA-CFG-200,QA-CFG-203,QA-CFG-211,QA-CFG-212 -->

CARD AS DISK on the USB page shows the card to a computer while the unit keeps playing. It is new in track3r-os. The steps, the refusals and the safe way to eject are in [File](file.md), in the part about the USB disk.

1. On the USB page, press `Down` until the cursor is on CARD AS DISK, then once more to READ ONLY. It starts at **On**: the computer can read the card but cannot write to it. Turn the encoder to **Off** only if the computer needs to write. The choice is not saved, so READ ONLY is **On** again the next time. <!-- QA-CFG-200 -->
2. Press `Up` to CARD AS DISK, then press `F7` twice. The strip says **CARD ON COMPUTER - CONFIG USB: F7 EJECT**. <!-- QA-CFG-200 -->
3. When you are done, press `F7` on the same row twice more. The strip says **CARD READY**. <!-- QA-CFG-203 -->

`F7` on CARD AS DISK can refuse. The strip says **NO CARD**, **CARD BUSY: WAIT FOR THE SAVE** or **STOP THE EXPORT FIRST**. <!-- QA-CFG-212 --> While the card is on the computer, SAVE, SAVE AS, OPEN, an export and the sample loads refuse too, and nothing is written. The strip says **CARD ON COMPUTER - EJECT FIRST** (the sample and instrument lists say **CARD ON COMPUTER**). <!-- QA-CFG-211 --> After the eject, **CARD NOT EJECTED PROPERLY: CHECK THE COMPUTER** shows only when the computer left the card unclean during this share. <!-- QA-CFG-212 -->

Not yet checked on the unit.

### Turn the metronome on, off or to Main
<!-- sync: qa=QA-CFG-016,QA-CFG-017,QA-CFG-018,QA-CFG-022,QA-CFG-023 -->

The metronome, METRO, plays a click with the beat. It is set on the PATTERN page.

Start on the Config screen. Press `F6` (**PATTERN**).

1. Press `Down` four times. The cursor is on METRO. <!-- QA-CFG-017 -->
2. Turn the encoder until the row reads **Off**, **Cue** or **Main**. <!-- QA-CFG-016 -->

The default is **Cue**. While OUT MODE on the [Mixer](mixer.md) is Stereo, Cue plays on the Main side, and the row reads **Cue (-> Main)**. **Main** reads **Main** whatever OUT MODE is. <!-- QA-CFG-017 --> When METRO is **Off**, the Pattern screen's info line loses its METRO chip. <!-- QA-CFG-018 -->

With OUT MODE on **MAIN L / CUE R**, Main plays on the left jack only and Cue on the right jack only. <!-- QA-CFG-022 --> The click is not in a RESAMPLE take, in the USB stream or in a WAV export. <!-- QA-CFG-023 -->

Not yet checked on the unit.

### Set the metronome volume

Start on the PATTERN page, with the cursor on METRO.

1. Press `Down`. The cursor is on METRO VOL. <!-- QA-CFG-019 -->
2. Turn the encoder until the row reads **80 %**. <!-- QA-CFG-019 -->
3. Press `Delete`. The row reads **50 %**, the default. <!-- QA-CFG-019 -->

Not yet checked on the unit.

### Set the count-in
<!-- sync: qa=QA-CFG-020,QA-CFG-025 -->

Start on the PATTERN page. The count-in is the click before a recording starts.

1. Press `Down` six times. The cursor is on COUNT-IN. <!-- QA-CFG-020 -->
2. Turn the encoder until the row reads **2 bars**. One step of the encoder reads **1 bar**. <!-- QA-CFG-020 -->

You should see **2 bars** on the row. Now press `Shift` + `Rec` on the Pattern screen with the transport stopped. The strip counts and the pads flash each beat, brighter on the first of a bar. The play line waits on row 1 and only the click sounds. Then row 1 plays and recording starts. A pad you hit during the count-in sounds and writes nothing. `Play` never counts in. <!-- QA-CFG-025 --> Not yet checked on the unit.

### Keep the timing and velocity of live notes
<!-- sync: src=app/include/o3os/app/screens/pattern_screen.h qa=QA-CFG-026,QA-CFG-027,QA-CFG-028,QA-CFG-029,QA-CFG-030 -->

Start on the PATTERN page. These rows apply to LIVE recording (`Shift` + `Rec` on Pattern), not to STEP.

1. Press `Down` twice. The cursor is on REC MICRO. Turn the encoder until the row reads **On**. The hint reads: late notes get m02 in FX2 and early ones snap to the step. <!-- QA-CFG-026 -->
2. Press `Down`. Turn the encoder until REC VELOCITY reads **External**. The hint reads: keyboard velocity goes to the volume column. <!-- QA-CFG-027 -->
3. Press `Delete` on REC VELOCITY. It reads **Off**. REC MICRO stays **On**. <!-- QA-CFG-028 -->

You should see this on the Pattern screen. With REC MICRO **On** and REC QUANTIZE on **Playing step**, a late note gets a Micro-move in FX 2 of its step. The later the note, the larger the value. A note on the beat gets none. With REC VELOCITY **External**, a note from a MIDI keyboard sets the volume of its step. Pads do not. Not yet checked on the unit. <!-- QA-CFG-029 --> <!-- QA-CFG-030 -->

### Move the pad rows against each other
<!-- sync: src=model/include/o3os/model/params.h,app/src/pad_board.cpp qa=QA-CFG-035,QA-CFG-036,QA-CFG-037,QA-CFG-038 -->

PAD OFFSET sets where each row of the note pads starts on the row below it. It is on the PATTERN page, the eighth row. It reads from **0** to **12**, and **12** is the default. At **12** each row carries on right after the row below it, so the four rows are one long keyboard. At a smaller number, a row starts on that note of the row below it, so the rows overlap. At **0** every row has the same notes.

Start on the PATTERN page.

1. Press `Down` seven times. The cursor is on PAD OFFSET. The hint reads: 12: rows follow on, no overlap. <!-- QA-CFG-035 -->
2. Turn the encoder until the row reads **5**. The hint reads: Less: rows overlap by 12 - N. <!-- QA-CFG-035 -->
3. Press `Delete`. The row reads **12**, the default. <!-- QA-CFG-036 -->

You should see the new number on the row. The pads follow at the next press, with no restart: on the Pattern screen, in the pad mode **NOTES** (`Shift` + `F2`), the pads of the upper rows play the notes the new number gives. The root, the scale and the octave are not here. They are in the held Note panel and belong to the project. PAD OFFSET belongs to the unit: it is kept with the other settings on the card, and a settings file from an older build reads **12**. Not yet checked on the unit. <!-- QA-CFG-037 --> <!-- QA-CFG-038 -->

### Choose when a picked pattern starts
<!-- sync: src=model/include/o3os/model/params.h,app/src/chooser_actions.cpp qa=QA-CFG-229,QA-PAT-318 -->

PAT SWITCH decides what happens when the pattern plays and you pick another one with `F1` held on the Pattern screen. It is on the PATTERN page, the last row. It reads **Step** or **Pattern end**, and **Step** is the default. On **Step** the new pattern takes over at the next step. On **Pattern end** the pick waits and takes over when the pattern that plays has ended, as a pad does in PATS.

Start on the PATTERN page.

1. Press `Down` eight times. The cursor is on PAT SWITCH. <!-- QA-CFG-229 -->
2. Turn the encoder until the row reads **Pattern end**. The card on the right says what each value does. <!-- QA-CFG-229 -->
3. Press `Delete`. The row reads **Step**, the default. <!-- QA-CFG-229 -->

You should see the setting act at once, with no restart. With **Pattern end**, hold `F1` on the Pattern screen while a pattern plays and press a pad: the pick is queued. While `F1` is held the view stays on your pick. Let go and the view goes on with the pattern that plays, until the queue lands. Press the same pad again, or the pad of the pattern that plays, to take the queue back. Not yet checked on the unit. <!-- QA-PAT-318 -->

### Choose where the unit listens and sends MIDI
<!-- sync: src=model/src/params.cpp,seq/src/midi_link.cpp,app/include/o3os/app/midi_input.h qa=QA-CFG-021,QA-CFG-220,QA-CFG-221,QA-CFG-222,QA-CFG-223,QA-CFG-225,QA-CFG-227,QA-CFG-228 -->

Start on the Config screen. Press `F1` (**MIDI**). The page has nine rows. Move to a row and turn the encoder. The defaults are DIN + USB for the ports, **All** for IN CHANNEL, 1 for OUT CHANNEL and **On** for both TRANSPORT rows.

| Row | Values | What it does |
|---|---|---|
| CLOCK IN | Internal, USB, MIDI In jack | Where the clock comes from. USB or MIDI In jack makes the unit follow that clock. |
| CLOCK SYNC | -6 to +6 pulses | A timing correction when the unit follows a clock |
| CLOCK MODE | Off, Master, Slave | With CLOCK IN on Internal: Master sends the clock, Slave follows it |
| TRANSP IN | Off, On | Off: the unit ignores Start, Continue, Stop and Song Position from the clock it follows. The default is **On**. |
| TRANSP OUT | Off, On | Off: the unit sends no Start, Continue, Stop or Song Position. The clock still goes out. The default is **On**. |
| MIDI IN | DIN, USB, DIN + USB | The ports the unit listens to for notes and MIDI Learn |
| MIDI OUT | DIN, USB, DIN + USB | The ports for clock, transport and notes of MIDI tracks |
| IN CHANNEL | All, 1 to 16 | The channel it listens to |
| OUT CHANNEL | 1 to 16 | The channel for notes of MIDI tracks |

**TRANSPORT IN and TRANSPORT OUT** (the rows read TRANSP IN and TRANSP OUT, which is as long as a caption can be) are new on this firmware. They matter when the unit sends or follows a MIDI clock (CLOCK MODE Master or Slave, or CLOCK IN on USB or MIDI In jack). With CLOCK MODE **Off** they do nothing. A row set to **On** is how every unit worked before: the clock and the transport go together.

- **TRANSPORT OUT Off**: the unit sends only the clock. A synth or sequencer that follows your clock keeps time but does not start or stop with `Play` and `Stop`. As a Slave the unit relays the clock only.
- **TRANSPORT IN Off**: the unit still follows the tempo of the clock, but a Start, Continue, Stop or Song Position from the master does nothing. Press `Play` and the unit starts on the master's next clock. A Stop from the master does not stop the unit. It stops by itself only if the master's clock goes quiet.

A change takes effect at the next `Play`. If you turn TRANSPORT OUT to Off while the unit plays, the Stop of that run is still sent, so a device that got the Start is also stopped. The choice is kept on the card. `Delete` on a TRANSP row puts **On** back. Not yet checked on the unit. <!-- QA-CFG-220 --> <!-- QA-CFG-221 --> <!-- QA-CFG-222 --> <!-- QA-CFG-223 --> <!-- QA-CFG-225 --> <!-- QA-CFG-227 --> <!-- QA-CFG-228 --> <!-- QA-CFG-224 --> <!-- QA-CFG-226 -->

### Change the display, the pad lights and the notation
<!-- sync: src=model/src/params.cpp,app/include/o3os/app/brightness.h,app/src/screens/pattern_screen.cpp,app/src/bindings.cpp qa=QA-CFG-213,QA-CFG-214,QA-CFG-215,QA-CFG-216 -->

Press `F2` (**DISPLAY**). Move to a row and turn the encoder.

| Row | Values | What it does |
|---|---|---|
| DISPLAY | Low, Medium, High | The brightness of the screen. The default is **High**. |
| PAD LIGHTS | Low, Medium, High | The brightness of the pads. The default is **High**. |
| NUMBERING | From 0, From 1, Hex | How the step rows of the Pattern screen are numbered. The default is **From 1**. |
| MIDDLE C | C-3, C-4, C-5, C-6 | The name of MIDI note 60 on the Pattern screen. The default is **C-5**. |
| THEME | Amiga, Black, White | The colours of the screens. `Shift` + `Master` also steps to the next theme. |

All five rows act at once, with nothing to apply. <!-- QA-CFG-213 --> <!-- QA-CFG-215 --> NUMBERING and MIDDLE C show on the Pattern screen: with **From 0** the step rows count from 00, and with MIDDLE C on **C-4** a note that reads C-5 by default reads C-4. <!-- QA-CFG-216 --> <!-- QA-CFG-214 -->

Not yet checked on the unit: the MIDI and DISPLAY rows.

### Reset a page to its defaults

Start on the page to reset, after you change a row.

1. Hold `Shift`. Press `F7` (**DEFAULTS**). The strip says **SHIFT+F7 AGAIN: RESET PAGE** and the key reads **RESET?**. <!-- QA-CFG-031 --> <!-- QA-CFG-034 -->
2. Press `Shift` + `F7` again within two seconds. <!-- QA-CFG-032 -->

You should see **PAGE RESET TO DEFAULTS** in the strip. Every row of the page is at its default, and the cursor stays on its row. Other pages are not changed. After more than two seconds, a press only asks again. A USB row becomes a pending value: press `F7` (**APPLY**) to apply it. Not yet checked on the unit. <!-- QA-CFG-032 -->

### Go to another page

Press `F1` to `F6` to go straight to a page, or press `Config` again for the next page. The pages wrap after PATTERN. With `F1` to `F6` the cursor starts on the first row of the new page; with `Config` it keeps its row (the last row if the new page is shorter). `Left` and `Right` do not turn the page: they change the value under the cursor. <!-- QA-CFG-033 -->

### Update from the unit
<!-- sync: src=app/src/firmware_actions.cpp,app/src/screens/firmware_overlay.cpp,app/include/o3os/app/firmware_view.h qa=QA-CFG-040,QA-CFG-042,QA-CFG-044,QA-CFG-046,QA-CFG-048,QA-CFG-051,QA-CFG-054,QA-CFG-056,QA-CFG-057,QA-CFG-041,QA-CFG-043,QA-CFG-045,QA-CFG-047,QA-CFG-049,QA-CFG-050,QA-CFG-052,QA-CFG-053,QA-CFG-055,QA-CFG-058,QA-CFG-059 -->

New in track3r-os. You can install a new firmware from the unit, with no computer. The image is a file that ends in **.ptf**. It must already be in the `Firmware` folder of the card. The unit checks the file, asks you twice, and then restarts into the update. The unit has checked a file up to the image check; the INSTALL step (step 5) is not yet checked on the unit.

Start with the **.ptf** file on the card and your project saved.

1. Press `Config`, then `F4` (**INFO**). The key row now has `F8` (**UPDATE**). <!-- QA-CFG-040 -->
2. Press `F8`. A list opens with every **.ptf** file in the folder and its size. The first file is marked **RESET TARGET**: it is the one the reset button installs. A name too long for that row is shortened with three dots; the question in step 3 shows it whole. <!-- QA-CFG-042 -->
3. Choose a file with `Up` and `Down`. The cursor stops at both ends. <!-- QA-CFG-043 --> Press `F1` (**UPDATE**). The unit asks **UPDATE TO** and the name of the file. <!-- QA-CFG-044 -->
4. Press `F1` (**YES**). A bar shows the unit reading the file. Nothing is written yet. <!-- QA-CFG-046 -->
5. When the file passes, the unit says **IMAGE OK**, with the length of the image and a CRC. The CRC is a code worked out from every byte of the file, so two copies of the same file give the same code. Press `F1` (**INSTALL**) within 30 seconds. <!-- QA-CFG-048 -->
6. The screen says **UPDATING: DO NOT POWER OFF**. The unit restarts by itself with the new firmware. Config, then **INFO**, shows the new build. <!-- QA-CFG-054 -->

> **Careful**: when the screen says **UPDATING: DO NOT POWER OFF**, no key does anything and nothing can stop the update. Do not switch the unit off.

Each question is answered by `F1` alone. Any other key takes the question back, and the list returns. During the check, `F8` (**CANCEL**) stops it. If you wait 30 seconds at the second question, the list says **THE CHECK EXPIRED: CHECK AGAIN**. Nothing is written in any of these cases. <!-- QA-CFG-045 --> <!-- QA-CFG-047 --> <!-- QA-CFG-049 --> <!-- QA-CFG-050 --> `F8` (**BACK**) on the list closes it. <!-- QA-CFG-053 --> Pulling the card out during the check or at the second question, or holding the Power switch during the check, cancels the check and closes the dialog. <!-- QA-CFG-058 -->

If the file is not a good image, the list says **NOT A VALID IMAGE**, with the reason and the place in the file. <!-- QA-CFG-051 --> If the list does not open, the strip says why, for example **SAVE THE PROJECT FIRST** or **THE CARD IS ON THE COMPUTER**. <!-- QA-CFG-056 --> <!-- QA-CFG-057 --> If the unit says **UPDATE DID NOT START**, the update did not begin and the unit goes on with the firmware it has. <!-- QA-CFG-052 --> <!-- QA-CFG-055 --> <!-- QA-CFG-059 --> The list holds 12 files at a time, and every **.ptf** file of `Firmware` can be reached: `Down` past the twelfth file loads the next 12, and `Up` past the first file of a window loads the 12 before it. <!-- QA-CFG-260 --> <!-- QA-CFG-261 --> With more than 12 files the bottom right of the dialog says where the cursor is, for example **FILE 18 / 19**. <!-- QA-CFG-262 --> The folder is read again each time the list moves to another 12, so a file you add or remove while the list is open shows up then. <!-- QA-CFG-263 --> If the card is busy at that moment, the list stays as it was and the line says **THE CARD IS BUSY: TRY AGAIN**. <!-- QA-CFG-264 --> *New Community OS behaviour (OS-BUG-FWLIST).*

## Keys on this screen

<!-- manual-tables: keys screen=config -->
The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.

| Key | Label | With `Shift` |
|---|---|---|
| `F1` | **MIDI** | - |
| `F2` | **DISPLAY** | - |
| `F3` | **SD** | - |
| `F4` | **INFO** | - |
| `F5` | **USB** | - |
| `F6` | **PATTERN** | - |
| `F7` | **APPLY** | **DEFAULTS** |
| `F8` | - | - |

<!-- /manual-tables -->

| Key | What it does | Ref |
|---|---|---|
| `F1` to `F6` | goes to a page: MIDI, DISPLAY, SD, INFO, USB, PATTERN | QA-CFG-033 |
| `F7` (**APPLY**) | on a USB row: press once to say what will happen, again to write. No label and no effect on the SD and INFO pages. | QA-CFG-003, QA-CFG-004, QA-CFG-034, QA-CFG-219 |
| `F8` (**UPDATE**) | on the INFO page only: opens the firmware update list. On the other pages it does nothing. | QA-CFG-040, QA-CFG-041, QA-CFG-218 |
| `Shift` + `F7` (**DEFAULTS**) | puts the page's rows back to their defaults. Press it twice within two seconds. | QA-CFG-031, QA-CFG-032 |
| `Shift` + `F1` to `F6`, `Shift` + `F8` | do nothing, and do not reach the page keys or APPLY | QA-CFG-015 |
| `Config` (again) | goes to the next page. It wraps after PATTERN. | QA-CFG-033 |
| `Left`, `Right` | change the value under the cursor, one click of the encoder down or up. On USB AUDIO and USB RETURN they change a pending value. New in track3r-os. | QA-CFG-240, QA-CFG-242, QA-CFG-243 |
| `Shift` + `Left`, `Shift` + `Right` | the same in the coarse step, as `Shift` + the encoder | QA-CFG-241 |
| `Up`, `Down` | moves the cursor between rows | QA-CFG-005, QA-CFG-017 |
| the encoder | changes the value under the cursor. On USB AUDIO and USB RETURN it changes a pending value. | QA-CFG-002, QA-CFG-008 |
| `Delete` | puts the row's default back. On USB AUDIO and USB RETURN, the default becomes a pending value. | QA-CFG-009, QA-CFG-019, QA-CFG-028, QA-CFG-036 |

## Good to know

- The pads play the current instrument while you hold them, on every page of Config. No setting changes. <!-- QA-CFG-230 -->
- A **pending value** is a USB row you changed but did not apply. It shows a star (\*). Changing the page or leaving it drops the pending value. Nothing is applied. <!-- QA-CFG-010 --> <!-- QA-CFG-033 -->
- `Delete` on USB AUDIO when it reads Off gives **On\***. The On waits for `F7`. <!-- QA-CFG-009 -->
- Pressing `Shift` + `F7` on the SD or INFO page says **NO ROWS ON THIS PAGE**. <!-- QA-CFG-032 -->
- REC MICRO and REC VELOCITY are new in track3r-os. <!-- QA-CFG-026 --> <!-- QA-CFG-027 -->
- PAT SWITCH is new in track3r-os. <!-- QA-CFG-229 --> <!-- QA-PAT-318 -->
- PAD OFFSET is new in track3r-os. The official manual names a row offset among the pad options of Config, and this chapter does not claim the same range. <!-- QA-CFG-035 -->
- Not checked against the original firmware: the keys and pages in this chapter.

## See also

- [Mixer](mixer.md): OUT MODE, which decides where Cue plays.
- [Pattern](pattern.md): live recording, the count-in, and the notes that REC MICRO and REC VELOCITY write.
- [USB disk](usb_disk.md): the CARD AS DISK and READ ONLY rows, and the card at power-on.

<!-- Open rows, not written: QA-CFG-014, QA-CFG-024, QA-CFG-202, QA-CFG-206 -->
