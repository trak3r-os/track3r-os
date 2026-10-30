<!-- manual-sync: 67c2869a -->
<!--
Chapter: File. Rules: .claude/skills/manual-writing/SKILL.md. Scope: projects, saving and loading, the card on a computer. Sources: docs/qa/catalogue/file.md (QA-FIL-001, 002, 012, 013, 032 to 042, 060 to 062, 070 to 073, 080, 084, 087, 088, 091, 093, 094), docs/qa/catalogue/global.md (QA-GLB-008, 009, 011), docs/ui/ui-map.md (File row).
Not in this chapter: the IMPORT page beyond the module task (QA-FIL-005 to 011, the engine, the report), the EXPORT page and WAV export (QA-FIL-020 to 025), stems (QA-FIL-050 to 057), the OG PROJECT row (QA-FIL-082, 083, 085, 086, 089; see export.md).
Dropped rows, not written: QA-FIL-030, 031, 043 (RENAME of a project folder). Open rows, not written: none. QA-FIL-010, 011, 044 and 064 are host PASS with unit open.
Brief tier (skill section 0): no How it works and no Try it. The USB disk tasks moved to usb_disk.md (C4 fold); the console export task keeps the chapter over the 800-word brief cap.
-->
# File

Status: draft

File lists the projects on the card, and one part of it is new in track3r-os: a project can be exported in the original firmware's format. <!-- og: new --> The card can also go to a computer as a disk, which [USB disk](usb_disk.md) describes. The screen itself, its keys and its messages are not checked against the original firmware. <!-- og: open -->

## What it is for

Use the File screen to work with projects on the card. A project holds your patterns, song order, instruments and samples, in its own folder. Open the screen with `File`. It opens the PROJECTS page, where you open, save, start, name and delete projects.

## The screen at a glance

![File, PROJECTS page: RECENT rows on top, the open project named in the header, and the key row from OPEN to EXPORT.](img/file_typical.png)

| Area | What it shows |
|---|---|
| Header | At the right, **PROJECT** and the project's title, then the page name (**OPEN**) at the far right. A project with no title shows its folder name. While the project has changes that no save has taken, **UNSAVED** stands where **PROJECT** was (**UNSAVED** alone for a project with no name); a save, NEW or OPEN clears it. <!-- QA-FIL-117 --> |
| Main area | The project list. The first rows are **RECENT**: the last five projects you opened or saved. |
| Strip | Messages, for example **SAVE ... DONE**. Questions also appear here. |
| Key row | `F1` to `F8`: **OPEN**, **RESCAN**, **NEW**, **SAVE**, **SAVE AS**, **IMPORT**, **EXPORT**, and a blank. |
| Shift row | The labels drawn while `Shift` is held: **0/4**, **1/4**, **2/4**, **3/4**, **END**, **NAME** and **PURGE**. |

## More tasks

### Open a project
<!-- sync: src=app/src/file_actions.cpp qa=QA-FIL-012,QA-FIL-035,QA-FIL-095,QA-FIL-096,QA-FIL-110,QA-FIL-111 -->

Start on the File screen, on the PROJECTS page.

1. Press `Up` or `Down` to the project. For a recent project, press `Left` or `Right` to move between the **RECENT** rows and the list. <!-- QA-FIL-035 -->
2. Press `F1` (**OPEN**). <!-- QA-FIL-012 -->
3. If the project has changes that no save has taken, the strip says **UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM**. Press `F1` again. A move of the cursor, or any other key, takes the question back. <!-- QA-FIL-012 -->

You should see **OPENING** and the project's name, then the Pattern screen with **PROJECT <NAME> OPENED**. If the project cannot be opened, the screen stays on File and the strip says why, for example **PROJECT <NAME> FAILED: NO PROJECT FILE** or **PROJECT <NAME> FAILED: THE FILE IS CUT SHORT**. A refused project leaves the project you had open, its undo history and the playing transport as they were. <!-- QA-FIL-095, QA-FIL-096, QA-FIL-110 -->

At power-on the unit opens the project you had open last (the newest **RECENT** entry), and the strip says **PROJECT <NAME> OPENED**. If it cannot load, the unit starts with an empty project and the strip says **PROJECT <NAME> FAILED** and why, at every start until you open another project. With no card, or on the USB disk boot, nothing is opened. New in track3r-os. Not yet checked on the unit. <!-- QA-FIL-111 -->

### Save a project, save a copy, or name it
<!-- sync: src=app/src/save_actions.cpp qa=QA-FIL-104,QA-FIL-072,QA-FIL-091,QA-FIL-098,QA-FIL-061,QA-FIL-040,QA-FIL-034,QA-FIL-060 -->

Start on the PROJECTS page.

1. Press `F4` (**SAVE**). The strip says **SAVE <NAME> DONE**, and the earlier file is kept as `project.o3pj.bak`. A project that has no folder yet is saved in the first free folder named **Track nn**, with a title or without one. <!-- QA-FIL-072, QA-FIL-091, QA-FIL-098 -->
2. To save a copy, press `F5` (**SAVE AS**). The keyboard opens. Type a folder name with the pads: letters, digits, spaces and `-`, at most 31 characters. Press `F8` (**SAVE**). `F6` (**AUTO**) fills in a name for you, and `F7` leaves without saving. `F8` with no name typed says **TYPE A NAME** and the keyboard stays open. <!-- QA-FIL-061 QA-FIL-113 -->
3. To change the title shown in the header, press `Shift` + `F6` (**NAME**), type up to 32 characters and press `F8`. The strip says **PROJECT NAMED**. <!-- QA-FIL-060 -->

**SAVE** in a project you opened from an official (OG) folder adds `project.o3pj` beside the original files and leaves them alone: a sample whose `samples/instrNN.wav` already holds the same audio is not rewritten, and only a sample you edited gets a new file (the old one kept as `.bak`). <!-- QA-FIL-104 --> **SAVE AS** writes a new folder and leaves the old one alone. <!-- QA-FIL-040, QA-FIL-034 --> The folder keeps its name when you change the title. Not yet checked on the unit.

### Start a new project
<!-- sync: src=app/include/o3os/app/file_view.h qa=QA-FIL-070,QA-FIL-099,QA-FIL-071,QA-FIL-073,QA-FIL-080,QA-FIL-097,QA-FIL-072,QA-FIL-101,QA-FIL-103 -->

Start on the PROJECTS page.

1. Press `F3` (**NEW**). The strip says **NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS**, or **UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS** when a save is due. The key row marks the two answers. <!-- QA-FIL-070, QA-FIL-099 -->
2. Press `F1` (**KEEP INS**) to keep the instruments and their samples, or `F3` (**ALL**) to remove every instrument. <!-- QA-FIL-071, QA-FIL-070 -->

You should see **NEW PROJECT**, then the Pattern screen. With **KEEP INSTRUMENTS** the patterns, song and title are new, and a save gives the new project its own copy of each kept sample. With **ALL** the project has one empty pattern, one song position, no title and no instrument. A move, another key, leaving the screen, or 4 seconds without an answer cancels the question, and the plain key row returns. The question takes the strip at once, even from a message such as **SAVE ... DONE**; a warning or an error already there keeps its place. <!-- QA-FIL-071, QA-FIL-070, QA-FIL-073, QA-FIL-080, QA-FIL-097, QA-FIL-072, QA-FIL-101, QA-FIL-103 -->

> **Careful**: **ALL** removes every instrument and frees the sample memory. Save first if you want to keep them.

### Import a module file
<!-- sync: src=app/src/file_actions.cpp qa=QA-FIL-003,QA-FIL-125,QA-FIL-126,QA-FIL-127,QA-FIL-128,QA-FIL-129 -->

A module is a song file from an Amiga or PC tracker: ProTracker (MOD), Scream Tracker 3 (S3M) or OctaMED (MED, MMD0 to MMD3). Start on the File screen.

1. Press `F6` (**IMPORT**). The list is the card from its root, with every folder and the module files. A file counts as a module when its name ends in `.mod`, `.s3m`, `.med` or `.mmd0` to `.mmd3`, or starts with `mod.`, `s3m.`, `med.` or `mmd0.` to `mmd3.`, as many Amiga files do. The **TYPE** column says which. <!-- QA-FIL-003, QA-FIL-125 -->
2. If the file has no type in its name, press `F2` (**ALL**). The list shows every file, and **TYPE** says **?** for a name that tells nothing. `F2` again (**MODULES**) goes back. The list is read again each time. Leaving the page with `F6` puts it back on **MODULES**; another screen and back keeps the list as it was. <!-- QA-FIL-126 -->
3. Press `Up` or `Down` to the file and press `F1` (**IMPORT**). On a folder `F1` (**ENTER**) opens it, and `F3` (**UP**) goes up. <!-- QA-FIL-004 -->
4. If the project has changes that no save has taken, press `F1` again, as for **OPEN**. <!-- QA-FIL-008 -->

You should see **IMPORTING** and the name, then **IMPORTED** and the name. The module replaces the project; **SAVE AS** names it. The OS reads the first bytes of the file, not its name, to know what it is. A file that no reader takes says **NOT A MODULE THIS OS READS**, asks nothing and changes nothing. A real module cut off inside its header or song list says the same, not a more precise reason; the project stays as it was. A name that says one format over bytes of another is refused the same way, except that a MOD and an S3M may swap names. <!-- QA-FIL-127, QA-FIL-128 --> The oldest Soundtracker modules have 15 samples and no tag. They import when the name says MOD or says nothing, as a 4-channel song. Their own effect numbers are not handled. <!-- QA-FIL-129 --> New in track3r-os. Not yet checked on the unit.

### Convert other module formats with OpenMPT

The OS reads MOD, S3M and OctaMED. Formats such as XM, IT, MTM, 669 or Oktalyzer are not read. OpenMPT, a free tracker for a computer, opens many of them and can save a copy as S3M, or as MOD for four channels. <!-- sync: src=app/src/module_import.cpp qa=QA-FIL-128 -->
<!-- OpenMPT manual, "Saving and exporting" and "Module formats": Song Properties holds the type; saving keeps the current type; MOD: 64 rows, no instruments, 8-bit samples -->

1. Open the file in OpenMPT.
2. Open the Song Properties dialog and choose the new module type, S3M or MOD. OpenMPT warns about what the type cannot hold. A MOD has 64 rows to a pattern and plain 8-bit samples, so instruments are turned into samples. Other features may be lost too, and the song may sound different.
3. Save the file from the **File** menu, under a new name, and keep the original.
4. Put the copy on the card and import it as above.

Listen to the result and compare it with the original in OpenMPT. The conversion is OpenMPT's, not the OS's.

### Delete a project

Start on the PROJECTS page. Deleting hides a project. It does not erase it.

1. Move the cursor to the project, and press `Delete`. The strip says **DELETE <NAME>? DELETE AGAIN**. <!-- QA-FIL-032 -->
2. Press `Delete` again. The strip says **DELETED: <NAME>**, and the project leaves the list. <!-- QA-FIL-032 -->

The folder stays on the card, marked with an empty `.deleted` file. To list the project again, or free the space, use a computer. <!-- QA-FIL-034, QA-FIL-042 --> You cannot delete the open project: the strip says **CANNOT DELETE THE OPEN PROJECT**. <!-- QA-FIL-033 -->

### Remove unused instruments

Start on the PROJECTS page. This removes instruments that no step, FX or Learn binding uses.

1. Press `Shift` + `F7` (**PURGE**). The strip asks **PURGE <N> UNUSED INSTRUMENTS? SHIFT+F7 AGAIN**, or says **NO UNUSED INSTRUMENTS**. <!-- QA-FIL-037 -->
2. Press `Shift` + `F7` again. The strip says **PURGED <N> INSTRUMENTS - F8 ON PARAMS UNDOES**. <!-- QA-FIL-037 -->

Module instruments are never removed. <!-- QA-FIL-038 --> The next save leaves the removed instruments out; their sample files stay in the `samples` folder. <!-- QA-FIL-039 --> To bring them all back, press `F8` (**UNDO**) on the Params screen.

### Export a project in the original format (console)

New in track3r-os. <!-- og: new --> This writes the patterns and the project file into a new folder on the card, and changes nothing already there. It is not the EXPORT page (`F7`), described in [Export](export.md). Stop the transport, end any take, and open the diag console (see [Computer tools](computer_tools.md)).

1. Type `diag ogexp` and press Enter. <!-- QA-FIL-084 -->
2. Type `diag ogexp status` until the reply says `DONE`. <!-- QA-FIL-084 -->
3. To check, type `diag load Export/<name> OG`, with the project's name in place of `<name>`. <!-- QA-FIL-088 -->

You should see `ogexp: DONE n/n files Export/<name> OG`. The folder, such as `Export/Mine OG`, holds one file for each pattern, the pattern names, `project.mt` and one file for each sample instrument. The project loads with the same patterns, tempo, song and track names; pattern names are not read back. <!-- QA-FIL-084, QA-FIL-088 -->

- A second export goes to `Mine OG 2`, and so on up to `OG 99`, then the reply says `NO FREE OG FOLDER NAME`. What the format cannot keep is left out, and a Sine LFO is written as Triangle; [Export](export.md) lists those kinds. <!-- QA-FIL-084, QA-FIL-114 -->
- It is refused, and writes nothing, with a reason such as `STOP THE TRANSPORT FIRST`, `A JOB IS RUNNING` or `CARD IS WRITE PROTECTED`. <!-- QA-FIL-093 -->
- If the project changes during the export, it ends with `MODIFIED` and no `project.mt`. A full card, a cancel (`diag ogexp cancel`, or `F8` under the overlay of the OG PROJECT row) or a lost card or power can do the same. Every file with a final name is whole, loading such a folder says **NO PROJECT FILE**, and the unit never removes a folder. <!-- QA-FIL-093, QA-FIL-087 -->
- The files use the original firmware's formats, checked byte for byte by tests. An official unit has not opened them yet. <!-- QA-FIL-084, QA-FIL-094 -->

The EXPORT page's **OG PROJECT** row starts the same export from the screen: see [Export](export.md). <!-- QA-FIL-082, QA-FIL-083, QA-FIL-085, QA-FIL-086, QA-FIL-089 --> Not yet checked on the unit.

## Keys on this screen
<!-- sync: src=app/src/file_actions.cpp qa=QA-FIL-012,QA-FIL-070,QA-FIL-071,QA-FIL-072,QA-FIL-061,QA-FIL-003,QA-FIL-020,QA-FIL-002,QA-FIL-060,QA-FIL-037,QA-FIL-041,QA-FIL-013,QA-FIL-032,QA-FIL-035 -->

<!-- manual-tables: keys screen=file -->
The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.

| Key | PROJECTS | EXPORT | `Shift`, PROJECTS | `Shift`, IMPORT and EXPORT |
|---|---|---|---|---|
| `F1` | **OPEN** | **EXPORT** | **0/4** | **0/4** |
| `F2` | **RESCAN** | (blank) | **1/4** | **1/4** |
| `F3` | **NEW** | (blank) | **2/4** | **2/4** |
| `F4` | **SAVE** | **SAVE** | **3/4** | **3/4** |
| `F5` | **SAVE AS** | **SAVE AS** | **END** | **END** |
| `F6` | **IMPORT** | **IMPORT** | **NAME** | **NAME** |
| `F7` | **EXPORT** | **PROJECTS** | **PURGE** | - |
| `F8` | - | (blank) | - | - |

<!-- /manual-tables -->

<!-- manual-tables: hand -->
The table shows the PROJECTS and EXPORT pages. The IMPORT page (`F6`) has a key row of its own, which changes while you use it, so it is not in the table. `F1` reads **ENTER** while the cursor is on a folder and **IMPORT** on a file. `F2` is **ALL**, or **MODULES** once the list shows every file, `F3` **UP**, `F4` **SAVE**, `F5` **SAVE AS** and `F6` **PROJECTS**. `F7` shows the engine the module's commands go to: **FX AUTO**, **FX AMIGA** or **FX MODRN**. `F8` is **REPORT**, and **LIST** while the report is open. Its Shift row is the last column. <!-- QA-FIL-003, QA-FIL-007, QA-FIL-009 -->

| Key | What it does | Ref |
|---|---|---|
| `F1` (**OPEN**) | opens the project under the cursor. It asks first when there are unsaved changes | QA-FIL-012 |
| `F2` (**RESCAN**) | reads the card's folder again and lists the projects anew. When it is done the strip says how many items it found: **READ AGAIN: 5 ITEMS** | QA-FIL-112 |
| `F3` (**NEW**) | asks which new project. `F3` again is **ALL**. `F1` is **KEEP INSTRUMENTS** | QA-FIL-070, QA-FIL-071 |
| `F4` (**SAVE**) | saves the project | QA-FIL-072 |
| `F5` (**SAVE AS**) | saves under a new name | QA-FIL-061 |
| `F6` (**IMPORT**) | opens the IMPORT page. Its keys are in the note above the table; [Import a module file](#import-a-module-file) describes the page | QA-FIL-003 |
| `F7` (**EXPORT**) | opens the EXPORT page. Its keys are in the EXPORT column above; the page is described in [Export](export.md) | QA-FIL-020 |
| `Shift` + `F1` to `F4` (**0/4** to **3/4**) | move the cursor to a quarter of the list | QA-FIL-002 |
| `Shift` + `F5` (**END**) | moves the cursor to the last project | QA-FIL-002 |
| `Shift` + `F6` (**NAME**) | names the project | QA-FIL-060 |
| `Shift` + `F7` (**PURGE**) | removes unused instruments. On PROJECTS only | QA-FIL-037, QA-FIL-041 |
| `Shift` + `Up`, `Shift` + `Down` | move one page of 16 rows | QA-FIL-013 |
| `Delete` | marks the project deleted. It asks first | QA-FIL-032 |
| `Left`, `Right` | move between the **RECENT** rows and the list | QA-FIL-035 |
| `F8` | blank on the PROJECTS page | docs/ui/ui-map.md |

## Good to know

<!-- sync: src=hal/include/o3os/hal/save_guard.h qa=QA-GLB-008,QA-GLB-009,QA-GLB-011,QA-FIL-030,QA-FIL-043,QA-FIL-035,QA-FIL-036,QA-FIL-091,QA-FIL-081,QA-FIL-118,QA-FIL-119 -->

- The pads play the current instrument while you hold them, on every page of File except the on-screen keyboard and the dialogs. <!-- QA-FIL-118 -->
- In a long list a quick spin of the encoder moves 4 rows a detent, then 8; a slow turn moves one, and the cursor stops at the ends. `Shift` + turn the encoder still moves a page. New in track3r-os. <!-- QA-FIL-119 -->
- The EXPORT page stays open when you leave File, and a second export set starts at once; `F7` there goes back to PROJECTS. <!-- QA-FIL-081 -->
- If the name is taken, the keyboard stays open and the strip says **A PROJECT OF THAT NAME EXISTS: TYPE ANOTHER**. <!-- audit row 20: app/src/save_actions.cpp -->
- If you pull the card, the strip says **CARD REMOVED** and the project keeps playing. A running save ends with **SAVE ... FAILED: NO CARD**. Putting the card back saves nothing. <!-- QA-GLB-008, QA-GLB-009, QA-GLB-011 --> Not yet checked on the unit.
- You cannot rename a project folder on the unit: use **SAVE AS**, then delete the old folder. <!-- QA-FIL-030, QA-FIL-043 (dropped rows); docs/specs/OS-FILES-B.md -->
- The open project has a small square bullet left of its folder icon, in the list and in its **RECENT** row. A new project with no folder has none. New in the Community OS. <!-- QA-FIL-115 --> Not yet checked on the unit.
- A name longer than 32 characters is cut in the middle: the first 25 characters, a **~**, then the last 6, so names that differ at the end stay apart. The card keeps the full name. New in the Community OS. <!-- QA-FIL-116 --> Not yet checked on the unit.
- **RECENT** lists the last five projects you opened or saved, newest first. It is kept on the card, and a deleted project is not shown. <!-- QA-FIL-035, QA-FIL-036 --> Not yet checked on the unit.
- If you change a sample file on a computer, the next save writes the project's sample again and keeps the replaced file as, for example, `instr01.wav.bak`. <!-- QA-FIL-091 --> Not yet checked on the unit.
- Not yet checked on the unit: saving, saving as, naming, a new project and deleting. <!-- QA-FIL-040, QA-FIL-060, QA-FIL-070, QA-FIL-032 -->

## See also

- [Export](export.md): the EXPORT page: WAV files, stems and the OG PROJECT folder.
- [USB disk](usb_disk.md): the card on a computer, at power-on or while the unit plays.
- [Getting started](getting_started.md): the first start, and turning the unit off.
- [Sample load](sample_load.md): bring a sample into an instrument.
- [Computer tools](computer_tools.md): the diag console.
