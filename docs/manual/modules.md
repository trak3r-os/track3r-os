<!-- manual-sync: c3ee4c54 -->
<!--
Chapter: Modules (deep). Rules: .claude/skills/manual-writing/SKILL.md. Sources: docs/qa/catalogue/modules.md, instrument.md (QA-INS-029, -031, -043, -046, -054, -208), docs/ui/ui-map.md, docs/formats/o3-project.md (MODR), module/sdk/README.md. Walk-through: sim/scenarios/manual/modules_walk.txt. The benchmark rows QA-MOD-110..116 are for the maintainer and are not written here.
-->
# Modules

Status: draft

Modules are files on the card that add to the OS without a firmware update: sound engines for instrument slots, and apps that edit the pattern or show you what the unit is doing. The TB-303, Chip, FM and SID engines are built in and need no card; every other module is a `.tmod` file. The **MODULES** screen lists the apps. Module instruments and apps are new in track3r-os. <!-- og: new -->

## What it is for

Use a module instrument when a slot should be a synth, not a sample: pick it with `F4` (**MODULE**) on the Instrument screen. Use an app for a job on the pattern, such as a Euclidean rhythm, humanizing, or a MIDI monitor. Open the screen from the Navigator: `Shift` + `Pattern`, then **MODULES**.

## How it works
<!-- sync: src=module/src/loader.cpp,app/src/module_ui.cpp,app/src/module_actions.cpp spec=docs/specs/OS-MODPITCH.md qa=QA-MOD-001,QA-MOD-079,QA-MOD-099,QA-MOD-105,QA-MOD-127,QA-MOD-128 -->

### Three kinds of module

A `.tmod` file is an **instrument** (a sound engine, one voice a track), an **effect** or an **app** (a small program with its own page). The folder says which: `modules/instruments`, `modules/effects`, `modules/apps`. The file name, without `.tmod`, is the module's name everywhere. <!-- QA-MOD-079 -->

An effect module is read by the loader, but this version has no slot to run it, so it does nothing yet. <!-- module/sdk/README.md -->

### A module instrument: live, stored and ended

- **Live:** the slot sounds as the module's engine. Its pages and values show on the Instrument screen, and the encoder changes them while you play. <!-- QA-MOD-105 -->
- **Stored:** the project keeps the module's name and the values of its parameters. The `.tmod` file stays on the card. A module that the card does not have mutes the instruments that name it. <!-- docs/formats/o3-project.md -->
- **Ended:** `Shift` + `F4` (**DELETE**) stops its sound at once, and `F8` twice brings it back. <!-- QA-INS-031 -->

Example: put the TB-303 in slot 4, write a line on a track, then add a Glide to the second note. The pitch slides from the first note, as a legato note, in the Glide time. See [Step FX on module instruments](module-instrument-fx.md). <!-- QA-MOD-101 -->

### An app: live, stored and ended

- **Live:** an app has its own page while it runs. Its key row shows its own labels. `Play` still starts the transport, and the app never sees it. <!-- QA-MOD-120 QA-MOD-121 -->
- **Stored:** the app's own rows are kept with the project (the last 12 apps): a change to them survives leaving the app, a relaunch, and a save and load. `Delete` puts a row's default back, and that is kept too. A value the project has not kept reads the app's default. The target rows (pattern, track, row and instrument) are not kept. What an app writes into a pattern is ordinary pattern data, and the edit is undoable. <!-- QA-MOD-127 QA-MOD-078 -->
- **Ended:** `F8` (**BACK**), a screen button, or `F7` + `F8` together, which leaves even a hung app. <!-- QA-MOD-121 QA-MOD-128 QA-MOD-009 -->

Example: open Rhythm Gen from `F7` (**TOOLS**) on Pattern. It starts at your cursor, builds a rhythm with its rows, and **APPLY** writes it into the pattern. `F8` on Pattern takes it back. <!-- QA-MOD-132 QA-MOD-078 -->

An app asks before its first launch of a boot when it edits the project or writes files: `F1` reads **LAUNCH?** and the strip says what it will do. Press `F1` again to start it. An app that only listens asks nothing. <!-- QA-MOD-009 QA-MOD-090 QA-MOD-117 -->

## Try it
<!-- sync: src=app/src/screens/modules_screen.cpp,sim/scenarios/manual/modules_walk.txt qa=QA-MOD-001,QA-MOD-009,QA-MOD-117,QA-MOD-120,QA-MOD-121,QA-MOD-128 -->

1. Open the Modules screen from the Navigator, and press `F2` (**RESCAN**). The apps on the card are listed, each with its state, and the strip says how many it found, for example **READ AGAIN: 11 APPS**. <!-- QA-MOD-060 (the list), QA-MOD-157 (the count); no QA row covers the state column yet -->

   ![Modules: the apps on the card, each LOADED, with LAUNCH and RESCAN on the keys.](img/modules_walk_1.png)

2. Put the cursor on **midimon** and press `F1` (**LAUNCH**). It only listens, so it starts at once.

   ![MIDI Monitor: the app's page, with its own key labels and BACK on F8.](img/modules_walk_2.png)

3. Press `F8` (**BACK**). You are on the list again. <!-- QA-MOD-121 -->
4. Put the cursor on **rhythmgen** and press `F1`. It edits the project, so `F1` reads **LAUNCH?** and the strip asks. <!-- QA-MOD-009 QA-MOD-117 -->

   ![Modules: the cursor on rhythmgen, F1 reads LAUNCH? and the strip says EDITS THE PROJECT.](img/modules_walk_4.png)

5. Press `F1` again. Rhythm Gen starts.

   ![Rhythm Gen: the app's rows and its key labels.](img/modules_walk_5.png)

6. Press `Song`. The app ends and the Song screen opens. <!-- QA-MOD-128 -->

Not yet checked on the unit.

## The screen at a glance
<!-- sync: src=app/src/screens/modules_screen.cpp qa=QA-MOD-001,QA-MOD-002 -->

![Modules: the apps on the card with their state, the apps refused this boot at the right, and LAUNCH and RESCAN on the keys.](img/modules_qa_mod_001.png)

| Area | What it shows |
|---|---|
| Main area | The apps on the card, 16 rows at a time, each with its state: **ON CARD**, **LOADED**, **QUARANTINED** (refused this boot) or **MISSING** (the project names the app and the card does not have it). At the right, **QUARANTINED THIS BOOT** lists the apps the unit refused, each with the reason. |
| Strip | The message line, for example **NEEDS NEWER OS** for an app too new for your track3r-os. <!-- QA-MOD-079 --> |
| Key row | `F1` (**LAUNCH**) and `F2` (**RESCAN**). |
| Shift row | Blank. No key is bound to `Shift` + `F1` to `F8` here. |

## More tasks

### Open the Modules screen

Start on any screen.

1. Hold `Shift` and press `Pattern`. The Navigator opens. <!-- QA-MOD-002 -->
2. Turn the encoder until **MODULES** is shown. <!-- QA-MOD-002 -->
3. Press `F8` (**GO**). <!-- QA-MOD-002 -->

You should see the list of apps on the card. <!-- QA-MOD-001 -->

### Put a module in an instrument slot

Start on the Instrument screen, on a free slot.

1. Press `F4` (**MODULE**). The picker opens and lists the instrument modules on the card. <!-- QA-INS-029 -->
2. Turn the encoder to a module. `Shift` + `Down` and `Shift` + `Up` move ten rows at a time. <!-- QA-MOD-061 -->
3. Press `F1` to choose it. <!-- QA-INS-029 -->

You should see **ENGINE SET** in the strip. The slot now holds the module as its instrument. The choice is one undo step: press `F8` (**UNDO**) to take it back. <!-- QA-INS-029 -->

Not yet checked on the unit.

### Find the controls of a module instrument

Start on the Instrument screen of a module instrument, for example the TB-303.

1. Press `F1` to `F4` to choose a page. <!-- QA-MOD-105 -->

The key row and the page heading show the module's page names. The TB-303 has **SOUND** and **MODS**. The Chip has **TONE**, **ENV**, **PITCH** and **ARP**. The FM has **OP1** to **OP4**. Keys past the last page, and `F5`, are blank, and a blank key does nothing. The **PAGE** field in the header counts the module's pages: `P1` and `P2` on the TB-303, `P1` to `P4` on the Chip and the FM. A module without named pages shows `1-8`, `9-16` and so on. <!-- QA-MOD-105 QA-MOD-107 -->

2. Turn the encoder to change a value. <!-- QA-MOD-106 -->

Some values show a name instead of a number. The Chip's wave shows **PULSE**, **TRIANGLE**, **NOISE LONG** or **NOISE SHORT**. Its duty shows 12.5%, 25%, 50% or 75%. The FM's algorithm shows a routing such as `4>3>2>1`. The side pane gives the range in names too. <!-- QA-MOD-106 -->

Not yet checked on the unit.

### Launch an app on the pattern you are editing
<!-- sync: src=app/src/module_ui.cpp qa=QA-MOD-091,QA-MOD-132,QA-MOD-133 -->

Start on the Pattern screen, with the cursor on the track and row you want to work on.

1. Press `F7` (**TOOLS**). The list holds **EXPORT PATTERN** and the tool apps, sorted by name. <!-- QA-MOD-091 -->
2. Choose an app and press `F1`. The app opens with its rows set to your cursor: pattern, track and row. <!-- QA-MOD-132 -->

The rows PATTERN, TRACK, ROW and ROWS (TRACKS on Humanize and Chords) are the target. In Rhythm Gen they are on the last page, reached with `Right`; in Chords, on the second page. Press `Down` to a row and turn the encoder. `Shift` + the encoder takes the coarse step. The encoder moves one step and stops at the ends of the pattern. `Delete` puts the value you opened on back. A pattern that does not exist reads **(EMPTY)**, and **APPLY** says so. **APPLY** writes at the track and rows the page shows, and nowhere else. After `F8` the rows read the cursor again. Opened from the Modules screen there is no cursor, and the rows are dim dashes. <!-- QA-MOD-132 -->

Mutate and Morph work on a whole instrument instead. Their last row, **INSTRUMENT** (**INSTRUMENT A** on Morph), starts as the Pattern's current instrument. Turn the encoder to pick another one, 1 to 64, and the page shows that instrument's parameters. A slot that is not a sample or module instrument says **SAMPLE OR MODULE INSTRUMENTS ONLY**. `Delete` puts back the instrument you opened on, and **APPLY** changes the instrument the row shows. <!-- QA-MOD-133 -->

Not yet checked on the unit.

### See what each shipped app does
<!-- sync: src=modules/apps qa=QA-MOD-003,QA-MOD-004,QA-MOD-005,QA-MOD-006,QA-MOD-007,QA-MOD-008,QA-MOD-010,QA-MOD-011,QA-MOD-012,QA-MOD-013,QA-MOD-014,QA-MOD-015,QA-MOD-016,QA-MOD-017,QA-MOD-018,QA-MOD-019,QA-MOD-020,QA-MOD-021,QA-MOD-024,QA-MOD-025,QA-MOD-026,QA-MOD-027,QA-MOD-028,QA-MOD-029,QA-MOD-030,QA-MOD-031,QA-MOD-032,QA-MOD-092,QA-MOD-093,QA-MOD-094,QA-MOD-096,QA-MOD-123,QA-MOD-124,QA-MOD-126,QA-MOD-129,QA-MOD-131,QA-MOD-132 -->

Every app page answers the same keys. `Up` and `Down` choose a row, the encoder changes its value, `Shift` + the encoder takes the coarse step, `Delete` puts the row's default back, and where an app has pages, `Left` and `Right` change the page: Rhythm Gen, Chords (a second page) and Tap Calc (the Hz page). <!-- QA-MOD-123 QA-MOD-124 QA-MOD-126 -->

| App | What it does | Ref |
|---|---|---|
| Tap Calc | Tap any pad or `F1` to measure a tempo. The BPM is the mean of the last 4 to 8 intervals between taps, so 5 to 9 taps (the AVERAGE row, 8 by default), from 20.0 to 300.0. A pause over 2 seconds starts a new series. The encoder sets the BPM by hand. `Right` shows the same notes as LFO rates in Hz. `Delete` on the BPM row goes back to 120.0 and forgets the taps. `F2` sets the project's tempo, which one undo takes back. Under 40.0 BPM it refuses. | QA-MOD-003, QA-MOD-004, QA-MOD-005, QA-MOD-006, QA-MOD-007, QA-MOD-008 |
| MIDI Monitor | Lists the MIDI that arrives on DIN and USB, newest line first, eight lines at most. A strip of 16 channel cells lights for a moment on each message. The CHANNEL and TYPE rows filter the list. `Insert` or `F1` pauses it, and `F2` (**CLEAR**) empties it. SysEx is counted, not shown. It makes no sound. | QA-MOD-010, QA-MOD-011, QA-MOD-012, QA-MOD-013, QA-MOD-014, QA-MOD-015, QA-MOD-016 |
| Loudness VU | PEAK and RMS bars in dBFS, a clip lamp with a CLIPS count, and a 24 second RMS history. The SOURCE row picks `2` MASTER (what you hear) or `1` INPUT (the line input). The HOLD row keeps the highest peak for 1 to 10 seconds. `F1` (**CLEAR**) resets the clip count, HOLD and MAX. | QA-MOD-017, QA-MOD-018, QA-MOD-019, QA-MOD-020, QA-MOD-021 |
| Tuner | Reads a held note between 30 Hz and 2.1 kHz: the note name, **FREQ**, **CENTS** and a needle, which is in tune within 5 cents. The A4 row sets the reference from 415 to 466 Hz (440 by default). SOURCE picks INPUT or MASTER. With no clear pitch it says **NO SIGNAL**. | QA-MOD-024, QA-MOD-025, QA-MOD-026, QA-MOD-027 |
| Scope Spectrum | VIEW `1` draws the wave, steady on a trigger. TIMEBASE sets the span from 2.9 to 34.8 ms. VIEW `2` draws 48 spectrum bars from 40 Hz to 20 kHz with the **PEAK** frequency under them. SOURCE picks INPUT or MASTER. | QA-MOD-028, QA-MOD-029, QA-MOD-030, QA-MOD-031, QA-MOD-032 |
| SMF Export | Open it from `F7` (**TOOLS**) on Pattern. `F1` writes the pattern, or the block, as a MIDI file in `AppExport/smfexport/` on the card. A file is never replaced: the next ones are `PAT001_001.mid` and so on. It lists the FX it cannot write. | QA-MOD-092, QA-MOD-093, QA-MOD-094, QA-MOD-096 |
| Rhythm Gen, Humanize, Chords, Mutate, Morph | The pattern tools. See [Pattern tools](pattern_tools.md). Rhythm Gen plays a lane's hits while you turn the encoder. After **APPLY** and `F8` in Rhythm Gen, Humanize or Chords, the Pattern has the cursor on the first step written and the written block selected. Mutate and Morph change instrument parameters, not steps. | QA-MOD-129, QA-MOD-131 |

Not yet checked on the unit.

### Copy a module to the card
<!-- sync: src=module/src/loader.cpp,app/src/module_ui.cpp,app/src/module_actions.cpp,app/include/o3os/app/module_ui.h qa=QA-MOD-079,QA-MOD-001 -->

Back up the card first. See [Installing track3r-os](install.md).

1. Get the `.tmod` file. The apps of a release come in `track3r-os-<version>-apps.zip`, in the release `apps-v<version>` on the Releases page: unpack it into the root of the card and the files land in `modules/apps`. Otherwise copy the `.tmod` file to the card. An app goes in the `modules/apps` folder. An instrument module goes in `modules/instruments`. An effect module goes in `modules/effects`. <!-- QA-MOD-079 -->
2. Put the card in the unit.
3. For an app, press `F2` (**RESCAN**) on the Modules screen. <!-- QA-MOD-001 -->

**RESCAN** reads the apps again, and the strip says how many it found (**READ AGAIN: 3 APPS**). It does not touch the engine picker. The picker reads the card each time you open it, so a new instrument module shows there without a rescan. The TB-303, Chip, FM and SID are built into the firmware and are listed with no card.

## Keys on this screen

<!-- manual-tables: keys screen=modules -->
The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.

| Key | Label | With `Shift` |
|---|---|---|
| `F1` | **LAUNCH** | - |
| `F2` | **RESCAN** | - |
| `F3` | - | - |
| `F4` | - | - |
| `F5` | - | - |
| `F6` | - | - |
| `F7` | - | - |
| `F8` | - | - |

<!-- /manual-tables -->

| Key | What it does | Ref |
|---|---|---|
| `Shift` + `Pattern` (Navigator), then the encoder to **MODULES**, then `F8` (**GO**) | opens the Modules screen | QA-MOD-002 |
| `F1` (**LAUNCH**) | starts the app under the cursor | QA-MOD-002 |
| `F2` (**RESCAN**) | looks at the card again. The strip says how many apps it found: **READ AGAIN: 3 APPS** | QA-MOD-001, QA-MOD-157 |
| `Up`, `Down` | move one row | QA-MOD-060 |
| `Shift` + `Up`, `Shift` + `Down` | move one page of 16 rows. They stop at the first and last row and do not wrap. | QA-MOD-060 |
| `Shift` + `F1` to `F8` | nothing | QA-MOD-001 |
| `F8` on an app's page | leaves the app and returns to the Modules screen | QA-MOD-121 |
| A screen button (`Song`, `Params`, `File` ...) on an app's page | leaves the app and opens that screen; the settings of the app stay with the project | QA-MOD-128 |
| `F7` + `F8` together | leaves an app, even a hung one | QA-MOD-009 |

The engine picker, on the Instrument screen:

| Key | What it does | Ref |
|---|---|---|
| `F4` (**MODULE**) on a free slot | opens the picker | QA-INS-029 |
| `Shift` + `Down`, `Shift` + `Up` | move ten rows at a time. They stop at the ends. | QA-MOD-061 |
| `F1` | chooses the module the cursor is on (**ENGINE SET**) | QA-INS-029 |
| `F8` | closes the picker with no choice | QA-INS-208 |

## Good to know

- While the picker is open, the other keys do nothing. `Pattern` does not leave the Instrument screen. <!-- QA-INS-208 -->
- A module instrument cannot be copied. `Shift` + `F2` (**COPY**) says **A MODULE INSTRUMENT CANNOT BE COPIED**. <!-- QA-INS-043 -->
- A paste onto a slot that holds a module does nothing. The strip says **INS nn IS A MODULE: DELETE IT FIRST**, where `nn` is the slot number. Press `Shift` + `F4` (**DELETE**) first. <!-- QA-INS-046 -->
- `SAVE` works for sample instruments only. On a module instrument the strip says **SAMPLE INSTRUMENTS ONLY**, and the card is not changed. <!-- QA-INS-054 -->
- Deleting a module instrument with `Shift` + `F4` stops its sound. `F4` (**MODULE**) then says **RELOAD TO PUT A MODULE HERE**. Press `F8` twice to bring the module back. <!-- QA-INS-031 -->
- A module built for an older track3r-os ignores Glide and Slide. Its notes play as ordinary notes. <!-- QA-MOD-104 -->
- Glide, Slide up and Slide down change the pitch of a module instrument. Arp plays only with a Chord in the other FX column. Chord alone plays the root. See [Step FX on module instruments](module-instrument-fx.md). <!-- QA-MOD-099 QA-MOD-100 QA-MOD-101 QA-MOD-103 -->

Not yet checked on the unit: the picker, the pages, the value names, the apps' kept rows, the target rows and the count after **RESCAN**.

## See also

- [Installing track3r-os](install.md): backing up the card and installing an image.
- [TB-303, FM and Chip: new parameters](module-sounds.md): what the new values of the three older built-in sounds do, and the new SID sound.
- [Step FX on module instruments](module-instrument-fx.md): which FX a module instrument obeys.
- [The panel](panel.md): where the keys are on the unit.
