# Official-manual map

Status: 2026-10-18, OS-MANUAL2 slice M2-1 (`docs/specs/OS-MANUAL2.md` section 4). Documentation only; no firmware change.

This page maps the features of the official firmware's public manual onto track3r-os, screen by screen, so that a chapter writer reads one block and knows what an owner of the official unit already expects. It lives in `docs/ui/` and not in `docs/manual/` because `tools/manual_check.py` treats every `docs/manual/*.md` as a chapter.

## 1. Sources and rules

- **The official manual.** Polyend's public Tracker manual, web edition labelled 1.7.0 (`polyend.com/manuals/tracker`). The web page is a shell around the PDF it links, so the section numbers below are the PDF's own: 149 pages, 13 chapters, numbered 1.1 to 13.11. The PDF address named in the spec (`.../2023/11/Tracker_Manual_Rev1.pdf`) answered 404 on 2026-10-18. The PDF read is the one that the 1.7.0 web edition links, `Polyend-Tracker-Manual-1.7.0.pdf` (149 pages, produced September 2023), fetched into a scratch directory outside the repository and not kept.
- **Release notes after 1.7.** The changelog of the official Tracker downloads page (`polyend.com/downloads/tracker-downloads/`) and Polyend's release posts, for 1.7.1, 1.8.0, 1.9.0, 1.9.1 and 1.9.2. The page lists nothing newer than 1.9.2. The same page offers a "Tracker manual" labelled 1.9.2, which this map did **not** read (the spec names the 1.7.0 edition plus the release notes); follow-up F1 in section 7 says why it stays unread.
- **Our side.** `docs/ui/ui-map.md`, `docs/qa/catalogue/*.md`, `docs/manual/*.md`, `docs/specs/`, and the code where a spec or a row left a question open. Nothing from the official firmware's binaries or sources was used.
- **No official material is copied.** Web reads only. Every "What it does there" cell is a one-line paraphrase in our own words; the section number and title are citations. No Polyend text, pictures or layout are reproduced.
- **Evidence rule** (CLAUDE.md, `OS-MANUAL2` section 5). A description in the official manual is **documented behaviour**, not `verified parity`; only an observation on an official unit, with evidence, earns that. A "same" mark with a passing QA row lets a chapter say "Works as the official manual describes". A "same" mark whose evidence is only a spec or a chapter is weaker: say so in the chapter, or use "Not checked against the original firmware". A QA row whose status reads "host PASS · unit open" (for example QA-PAT-219 and QA-PAT-220, QA-SNG-012) is host evidence only: it shows the behaviour on the host build, never on a unit, so a chapter must not word it as a check on the unit.

### Marks

| Mark | Meaning |
|---|---|
| same | The manual's description matches what our evidence shows. |
| different | The feature exists on both sides but behaves or is reached differently. The Notes cell says how. |
| new | The manual does not describe it (track3r-os behaviour). |
| missing | The manual describes it and we do not have it. A gap list for the maintainer, never a backlog row by itself. |
| open | `open research question`: the manual is unclear, or our evidence does not settle it. The Notes cell says why. Not a guess. |

### Columns

| Column | Content |
|---|---|
| Feature | Our words. |
| Official manual | Section number and title, or the release-note version (`RN 1.8.0`). |
| What it does there | A paraphrase in one line. |
| track3r-os | One of the five marks. |
| Our evidence | A QA row ID, or a spec, or a chapter of `docs/manual/` where no row or spec says it. `-` means none. |
| Notes | Detail, doubts, pointers. |

Section 4 has one extra leading column, FX, with our FX type number. A feature that a Shift layer, a key row or a mode changes is still one row. The manual's step has four parts: note, instrument, FX1 and FX2. Ours has five: Note, Instr, Vol (a volume column the manual does not describe), Fx1 and Fx2; the unit's four column keys are Note, Instr, Vol and Fx, and the Fx key moves between the two FX cells (`model/include/o3os/model/step.h`, `app/src/pattern_view.cpp`).

## 2. Unit, transport and global keys

Our home: Global (`docs/qa/catalogue/global.md`, `docs/ui/ui-map.md` section 2).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Power-up restores the last project | §1.3 Start Up | Switching on brings back the project that was loaded when the unit was last used. | open | - | What the OS loads at boot is in `docs/specs/OS-007-platform.md` (the load sequence); not checked here. |
| Hold the power switch to switch off | §1.3 Start Up | The back power button has to stay pressed through a short progress bar before the unit turns off. | different | QA-GLB-100, QA-GLB-102, OS-POWER | Ours: a line across the top fills in 2 s and the strip warns about unsaved work; it refuses during an update or an export. New Community OS behaviour. |
| Autosave | §3 Projects (intro), §3.2 Opening & Saving a Project | The open project is written to the card without being asked, and an explicit save command exists as well. | different | QA-GLB-109, QA-FIL-012 | Ours saves nothing by itself, and the power-off saves nothing (QA-GLB-109). F4 SAVE writes; an unsaved-changes question guards OPEN and IMPORT (QA-FIL-008, QA-FIL-012). |
| How many projects the list shows | §3 Projects (intro), §3.2 | The list is capped at 100 projects even when the card holds more. | open | - | No row or spec states a limit for the File list. |
| Master volume from any screen | §1.4 First Steps with Factory Projects | A held Master key turns the wheel into an output-level control with a ceiling of +12 dB. | missing | - | No held-Master gesture (`docs/ui/ui-map.md` section 2). The master level is the MST bus of the Mixer. The range was not compared. |
| Tempo from any screen | §1.9 Quick Reference Commands, §3.4 Project Tempo | With Song held, the wheel moves the tempo by whole BPM between 40 and 800, or by tenths when Shift is added. | different | `docs/manual/song.md` (F7 TEMPO) | Ours sets the tempo on the Song screen only (F7, then the encoder; Shift+encoder for 0.1 BPM). No held-Song gesture. No QA row for the range. |
| Tap tempo | §3.4 Project Tempo | Four or more taps on the Tempo key work out a tempo from their spacing. | different | QA-MOD-003 | Ours: each F7 press on Song counts as a tap and two taps less than 1.5 s apart set it (`docs/manual/song.md`). The Tap Calc app (QA-MOD-002..008) is new. |
| Delete puts a value back to its default | §1.9 Quick Reference Commands, §3.4 (note) | Delete on a highlighted parameter restores its starting value. | same | QA-INS-021, QA-CFG-019 | Rule 6 of `docs/ui/ui-map.md` section 5. Shift+Delete is a larger reset where it exists (Instrument INIT, Mixer stems). |
| Return to the top while stopped | §1.4 First Steps, §1.9 Quick Reference Commands | With the transport idle, Shift with Insert/Home parks the position on the first row. | different | `docs/ui/ui-map.md` section 1 (Pattern, Song rows), QA-SNG-003 | On Pattern ours moves the cursor to step 1; on Song, Shift+Insert duplicates a pattern. |
| Stop a running pattern with Play | §1.4 First Steps, §1.9 | A further press of Play halts a running pattern. | same | QA-PAT-126, QA-PAT-129, QA-PAT-132 | The QA rows cover only the stop (Play halts the transport in WATCH, STEP and LIVE). Starting from the top and looping have no QA row: not checked. Play also starts from the cursor's row with Shift (next rows). |
| Continue from the paused place | §1.4 First Steps, §1.9 | Shift with Play picks up at the point where playback was halted. | different | QA-PAT-231, QA-PAT-232 | Ours: Shift+Play on Pattern plays from the cursor's row (OS-PLAYCUR). `docs/specs/OS-005-audio-engine.md` O19 records how Continue resumes. |
| Play the song from the Pattern page | §1.9 Quick Reference Commands | Song held together with Play runs the whole song although the Pattern screen stays up. | different | `docs/ui/ui-map.md` Song row | Ours has no chord: F2 PLAY SNG on the Song screen, or Play there. |
| Peek at the Song page | §1.9, §9.3 Song Mode Interface | A held Song key flips to the song view for as long as it stays down. | missing | `docs/ui/ui-map.md` section 2 | Song is a tap. The Navigator opens with the Master+Song or Song+Pattern chord. |
| The page buttons | §1.2 Hardware Overview | One button per page: Pattern, Song, Instrument parameters, the three sample pages, Master, Perform, File, Config. | same | `docs/ui/ui-map.md` section 1 | The same buttons; pressing one again turns its pages (ui-map rule on press-again). Our names: Params, Sample Play, Sample Edit, Sample Load, Sample Rec. |
| Eight screen keys, dimmed when not available | §1.1 How to Use This Notebook, §1.7 Page Layout | Eight context keys line up under the display; a greyed label means the action cannot be used right now. | same | QA-INS-062, QA-INS-074, `docs/ui/ui-map.md` section 5 | F1 to F8. QA-INS-062 and QA-INS-074 show the dim keys on a MIDI instrument. Ours also draws a Shift label row on every screen (new, OS-UIFIX; QA-PRF-001 covers only that row). |
| Jog wheel and arrows | §1.2, §1.9 | The rotary wheel and the four arrow keys move the cursor and edit values. | same | `docs/ui/ui-map.md` section 2 | Ours adds Shift+arrows pages on lists (OS-NAV3, OS-SHIFTENC). |
| On-screen keyboard on the pads | §1.6 QWERTY Keyboard | The pad grid turns into a letter keyboard for naming, while three screen keys confirm a character, back out, or store the name. | same | QA-MIX-010, QA-SNG-010, QA-FIL-060 | F7 cancels and F8 accepts, as the manual's 7th and 8th keys. Ours also has caps and Insert (ui-map overlays). |
| Automatic names | §1.6, §3.2, §8.7 Sampling | A key in the naming editor fills in a generated name. | different | QA-FIL-061 | Ours: F6 AUTO exists on the SAVE AS keyboard only (QA-FIL-061). The instrument and pattern name keyboards have no AUTO key (`docs/ui/ui-map.md` overlays). |
| On-screen hints | §2.4 Internal Help & Tips | Some menus show a brief help line. | open | - | Ours has info lines and notifications; whether they match the official hints was not compared. |
| Built-in NES games | §13.7 Games | Retro console ROMs on the card can be started from the File area. | missing | - | No screen or action for games. |
| Screenshot | §13.6 Screen Capturing | Shift with the rear power button saves the shown page as an 800 x 480 bitmap on the card. | missing | OS-POWER | OS-SCREENSHOT is approved for after the alpha (BACKLOG); Shift+Power is reserved (QA-GLB-110). |
| Card format | §2.3 SD Card Structure, §13.8 | Only FAT32 on an MBR partition table is accepted. | same | QA-GLB-012, `docs/manual/install.md` | Ours reports "card not readable" for other volumes (QA-GLB-012). |
| Card folder layout | §2.3 SD Card Structure | The unit expects folders for firmware, games, instruments, manual, mod, projects, samples and its log. | different | QA-INS-052, QA-FIL-021, QA-FIL-036, OS-WAVEXP | Ours uses Projects/, Samples/, instruments/, Export/<Project>/, Settings/, Firmware/ and module folders. No Games/, Manual/ or trackerLog/. |
| Project folder layout | §2.3, §4.2 Process of Creating a Pattern | A project is a folder with its pattern, instrument and sample files. | different | `docs/formats/o3-project.md`, QA-FIL-082 | Ours saves its own `.o3pj` format. It opens OG project folders and can export one (OG PROJECT row, marked EXPERIMENTAL). |
| Card pulled and put back | not in the manual | - | new | QA-GLB-008..012 | The unit notices the card, rereads it and says so. |
| The unit as a USB disk | not in the manual | - | new | QA-GLB-015..024, OS-USBDISK | Boot chord Shift+F1+F7; the card goes to the computer. |
| USB audio and MIDI to a computer | §11.1 MIDI Definitions (MIDI only) | The unit sends and receives MIDI over USB. | different | QA-CFG-001..014 | Ours adds a USB audio device with a stems return; STANDARD mode is MIDI and serial only. |
| Firmware update from the Config menu | §13.2 Generic Firmware Update | Config lists the `.ptf` files of the Firmware folder; choosing one and confirming installs it. | missing | `docs/manual/install.md`, OS-006 | Ours has no Config firmware page. Updates go through the hidden reset button or the USB console (`o3console`). |
| Emergency reset button | §13.3 Emergency Firmware Update | A recessed button restarts the unit into an install from the card. | different | `docs/manual/install.md` | The manual says a random file from `Firmware/` is chosen; ours was observed taking the first `.ptf` in directory order. |
| Reset the configuration | §13.4 Resetting the Configuration, §3.7 | One Firmware-menu entry returns every Config option to its default. | different | QA-CFG-031, QA-CFG-032 | Ours resets one page at a time: Shift+F7 twice within 2 s. |
| Version and build display | §3.7 Summary of Configuration Options, §13.2 | Config shows the installed version and build. | same | `docs/manual/config.md` INFO page | PRODUCT and BUILD ID rows. No QA row. |
| Licences page | RN 1.8.0 | Config gained an information area holding a page of licence texts. | same | `docs/manual/config.md` INFO page | A LICENSES row on INFO. No QA row. |

## 3. Pattern

Our home: Pattern (`docs/qa/catalogue/pattern.md`).

### 3.1 Pattern, length and the grid

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Choose the pattern number, 1 to 255 | §4.2 Process of Creating a Pattern, §4.3 Configuring a Pattern | Holding Pattern while turning the wheel selects the slot; an unused slot starts empty. | same | QA-PAT-253 | Ours: hold F1 (the PATTERN chooser). A tap of F1 opens nothing (QA-PAT-260). |
| Pattern length 1 to 128, default 32 | §4.2, §4.3 | Holding Length and turning the wheel resizes the pattern; Left and Right jump between round sizes. | same | QA-PAT-253, `model/include/o3os/model/limits.h` | Ours: F2 LENGTH; the default is 32. |
| How many patterns a project holds | §2.2 Tracker Audio Structure, §3 Projects | Up to 255 patterns per project. | different | `model/include/o3os/model/limits.h` | Pattern ids run 1 to 255, but at most 128 patterns exist at once (kMaxPatterns, a memory choice); a larger OG project is refused with "limit exceeded". |
| How many tracks | §1.5, §2.2, RN 1.9.0 | Eight tracks in the 1.7.0 manual, each with one voice; release 1.9.0 adds four MIDI tracks, twelve in all. | open | `model/include/o3os/model/limits.h`, RN 1.9.0 | Ours: 12 active in a new project (16 in the model). Whether tracks 9 to 12 are MIDI-only, on either side, was not checked; the same question stands in 6.10. |
| Tempo range and default | §3.4 Project Tempo | Allowed tempos run from 40 up to 800 BPM, and a fresh project starts at 130. | same | `model/include/o3os/model/limits.h` | 4000 to 80000 hundredths of a BPM, default 130. |
| A volume column on every step | not in the manual | - | new | `model/include/o3os/model/step.h`, QA-CFG-030 | Vol is a 0 to 127 field of its own; the manual describes setting a step's volume only with the Volume FX (V). |
| Different length per track | not in the manual | - | new | QA-MOD-075, `docs/specs/OS-005-audio-engine.md` O16 | The pattern is as long as its longest track; shorter ones wrap. |
| Step jump | §4.2, §5.5 Step Jump Recording Multiple Steps | After a pad writes a step, the cursor moves on by the configured distance. | same | QA-PAT-006, QA-PAT-253 | Ours: F3 JUMP (0, 1, 2, 4, 8, 16, 32). |
| Undo and redo of pattern edits | §4.2 (table) | Up to twenty recent edits can be reverted and re-applied. | different | QA-PAT-246, QA-PAT-247, OS-UNDOGRP | Ours has one history per area (Pattern, Song, Params...) and merges encoder clicks into one unit. F8 undoes, Shift+F8 redoes. Depth not compared. |
| Preview the step | §4.2 (table), §5.4 | Sounds the step's instrument so it can be heard without running the pattern. | same | `docs/ui/ui-map.md` (Pattern row, F5 PREVIEW) | In WATCH the pads preview too (QA-PAT-223). No QA row for F5. |
| Duplicate, expand and shrink a pattern | §4.3, second options page | Doubles a pattern by repeating it or by spreading its rows apart with blank ones, or halves it by thinning out alternate rows. | missing | `docs/specs/OS-009-pattern-editing.md` P1b | Specified (`pattern.duplicate`, `.expand`, `.shrink`) but no action or key exists. |
| Copy and paste a whole pattern | §4.4 Copying and Pasting a Pattern, §5.4 | A pattern's rows and length travel together to another slot through a clipboard. | missing | `docs/specs/OS-009-pattern-editing.md` P1b | Not bound. Song Shift+Insert makes a copy in a new pattern slot (QA-SNG-003), which covers part of the use. |
| Invert a range of steps | §4.5 Inverting a Selection of Steps | Flips the marked rows upside down while every step stays in its own track. | same | `docs/specs/OS-009-pattern-editing.md` P1b | F6 INVERT in edit mode with a block. No QA row. The held-element variants (invert only notes, only FX) are not bound. |
| Render a selection to a sample | §4.3, §5.8 Note Steps, §8.8 Rendering and Exporting Internal Audio | Bounces the marked rows to audio as a new sample, which can go straight into the instrument list. | different | QA-PAT-185, QA-PAT-186, QA-PAT-187 | Ours: Pattern F7 TOOLS, EXPORT SELECTION writes a WAV to `Export/<Project>/`. It does not load the sample into an instrument. |
| Visual step dividers, 1 to 16 | §3.7, §4.6 Pattern Mode Page Layout Options | Every Nth row is marked on the grid, N from 1 to 16. | missing | - | No Config row. |
| Pattern top bar content | §4.6 | The strip above the grid can be hidden, show only track labels, or carry the full status line. | missing | - | No Config row. |
| Horizontal arrangement | §4.6 | Lays the pattern out sideways instead of downward. | missing | `docs/specs/OS-009-pattern-editing.md` P1e.1 | Specified as a page 2 and later left out: the Pattern button has one page (`docs/ui/ui-map.md`). |
| Vertical revolver arrangement | RN 1.7.0, §3.7 | A third pattern layout, a rotating vertical list. | missing | `docs/specs/OS-009-pattern-editing.md` P1e.1 | Reserved value, refused. |
| Step numbering from 0, 1 or hex | RN 1.7.0, §3.7, §13.9 Hexadecimal Option | Rows can be numbered in decimal from 0 or 1, or in hex. | same | `docs/manual/config.md` NUMBERING | Config DISPLAY page. No QA row. |
| Track names | §4.6 (Editing Track Names) | Up to 7 characters, set on the track mixer page with Rec. | different | QA-MIX-010, QA-MIX-011, QA-PAT-200 | Ours: up to 20 characters, Mixer Shift+F1, drawn in the Pattern header. |
| Pattern names | RN 1.8.0 | The Pattern editor gained a further parameter page where patterns and tracks get their labels. | different | QA-SNG-010, QA-SNG-011 | Ours names a pattern on Song, Shift+F6, up to 30 characters. No third Pattern page. |

### 3.2 Moving, selecting and viewing

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Four tracks shown by default | §1.5 Introduction to Tracks, §4.6 | The grid shows 4 tracks and scrolls across 8. | different | QA-PAT-236, QA-PAT-243 | Ours starts at a zoom of 5 tracks, with 12 tracks in a project. |
| Eight-track views of one or two columns | §1.5, §4.6 | Pattern plus one or two column buttons shows all tracks with only those columns. | different | QA-PAT-236, QA-PAT-237, QA-PAT-238 | Ours: Note+Instr cycles 5, 8 and 12 tracks (RAM only), with fixed columns per zoom. Note+Left/Right moves a whole track. |
| Cursor wraps vertically | §4.6 | Up and Down wrap at the ends of the pattern. | same | QA-PAT-219, QA-PAT-220 | A held arrow stops at the edge (QA-PAT-221). |
| Select a range with Shift+arrows | §4.7 Quick Start Step Creation, §5.4 | Shift+arrows select steps across rows and tracks. | same | QA-PAT-007 | Not available while the cursor rides the playhead (QA-PAT-009, QA-PAT-010). |
| Select a whole track or every track | §5.4 Manual Recording Steps | From the top row, Shift+Up grabs the whole track and a second press grabs every track. | same | `docs/architecture/os-tree.md` (selection) | Same gesture. No QA row. |
| Lock a column by tapping its key | §5.4 | A quick tap latches the column so the key need not stay held. | same | QA-PAT-240 | A hold shows the step popup instead (QA-PAT-224). |
| Hold a column key with a pad or wheel | §5.2 Process of Creating a Step, §5.8 to §5.10 | Holding the column key plus a pad or the wheel sets that part of the step. | same | QA-PAT-224, QA-PAT-240 | Ours shows a popup with the pads while the key is held. |
| Quarter jumps | not in the manual | - | new | QA-PAT-244 | Shift+Note/Instr/Vol/Fx jump to 0, 1/4, 2/4, 3/4 of the pattern. |
| Large steps with Shift and the wheel | RN 1.8.0 | Holding Shift while turning the encoder widens each increment. | open | QA-PAT-251, QA-PAT-252, QA-PAT-253 | Ours (OS-SHIFTENC) is marked new Community OS behaviour; `docs/ui/ui-map.md` says OG parity of these keys is open. |
| Mute a track from the Pattern page | §1.5, §1.9 | Shift plus a screen key mutes at once. | different | QA-MIX-020, QA-MIX-021 | Ours: Shift+pad in the TRACKS pad layer, with a mute mode (at once, bar, pattern); the Mixer has F7 MUTE. |
| Hold Pattern to see which patterns exist | §1.9, §12.9 Advanced Pattern Playback | The pads light for the existing patterns. | same | QA-PAT-254 | Hold F1. |
| Change pattern at once while playing | §1.9, §12.9 | Pattern plus Up/Down/wheel/pad switches immediately. | different | QA-PAT-254, QA-PAT-255, QA-PAT-256 | Ours switches at the next step boundary on the same row, with no stop or gap (OS-PATSWITCH, new). |
| Queue the next pattern, shown blinking | §1.9, §12.9 | Pattern with Left/Right cues a pattern for the end of the current one, and its label flashes red. | different | QA-PAT-262 | Ours queues from the PATS pad mode; the blink ends when a live jump wins. |

### 3.3 Entering and editing steps

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Rec toggles editing | §5.3 Recording Mode, §5.4 | Rec flips the cursor between a green playback frame and a red editing frame. | different | QA-PAT-001, QA-PAT-002, QA-PAT-133, QA-PAT-136 | Ours has three states, WATCH, STEP and LIVE (OS-RECMODE2). Plain Rec is STEP on and off. |
| Enter a note with a pad | §5.8 | A pad writes its note at the cursor and moves on by the jump. | same | QA-PAT-006, QA-PAT-225 | The pad also sounds. |
| Note pads: default layout | §5.8 | Chromatic from C3 at the bottom left, 12 semitones per row. | same | `docs/specs/OS-009-pattern-editing.md` P1a.1 | The defaults of `app/pad_board.h` match; no QA row. |
| Pads: scale, root note, row offset | §3.7, §5.8 | Config options change the scale, the root (C2 to C4) and the row offset. | different | QA-CFG-035, QA-CFG-036, `docs/specs/OS-009-pattern-editing.md` P1a.1 | The row offset is a Config PATTERN row since OS-PADOFFSET (PAD OFFSET, 0..12, default 12, a device setting; the official range is not claimed). Root, scale and octave are set from the held Note panel, per project, not on Config. |
| Note by wheel, octave by Shift+wheel | §5.8 | The wheel moves the note, Shift moves an octave. | same | `docs/ui/ui-map.md` Pattern row | OS-SHIFTENC keeps the octave step on Note. No QA row. |
| Special notes CUT, FAD and OFF | §5.8 | Wheel fully down reaches three end-of-note commands. | same | `docs/specs/OS-009-pattern-editing.md` P1a.1 | Note ranking Off, Cut, Fade, then notes. Tiles `OFF`, `CUT`, `FAD`. No QA row. |
| Empty-step defaults on release | §5.4 | Releasing a column key held over an empty step writes a default: C0, the previous instrument, or the previous FX type with a random value. | open | - | Ours opens a popup on a hold (QA-PAT-224); what a plain release writes was not checked. |
| Choose the instrument by pad, 1 to 48 | §5.9 Instrument Step Parameters | In the Instrument column a pad picks the instrument. | different | QA-PAT-222, QA-PAT-224 | Ours: a pad in the Instr column writes the note (OS-PADHOLD); the hold popup picks the instrument. 64 slots: 1 to 48 and 49 to 64 for MIDI. |
| Delete a step or a range | §4.7, §5.4 | Delete wipes the whole step, except when an FX column is the active one, where only that effect goes. | same | QA-PAT-216, QA-PAT-217 | A held Delete repeats (QA-PAT-216). Shift+Delete pulls the rows below up (new). |
| Insert a step row | §4.7 | Insert pushes the steps below down. | same | QA-PAT-229 | Shift+Delete is its inverse. |
| Copy and paste steps | §5.4 | Copy then Shift+Copy pastes the range at the cursor. | same | QA-PAT-226, QA-PAT-227, QA-PAT-228 | Pastes are clipped at the end; one undo unit. Pasting only the notes, only the instruments or only an FX is not bound. |
| Live recording | §5.7 Live Recording Steps | Pads are captured as they are played against the running loop. | different | QA-PAT-003, QA-PAT-004, QA-PAT-131 | Ours: Shift+Rec switches LIVE on. A whole take is one undo unit (QA-PAT-151). |
| Leave recording and keep playing | §1.9 | A Rec press mid-take ends capture while the music carries on. | same | QA-PAT-130 | Ours: Rec in LIVE goes to WATCH. |
| Arm or disarm tracks for recording | §1.9, §5.7 | A per-track record enable, toggled from the screen keys with Shift+Rec. | missing | `docs/specs/OS-009-pattern-editing.md` P1d (superseded by OS-LIVEREC) | No track arm. Live notes go to the cursor's track (`docs/architecture/os-tree.md`). |
| A coinciding note goes to the next track | §5.7 | If the target step is taken, the new note lands one track further right. | different | `docs/architecture/os-tree.md` (Live recording) | Ours writes over the step on the cursor's track. |
| Note-off recorded with a pad note | §5.7 | A live note is written with its note-off. | same | `docs/architecture/os-tree.md` (Release) | An OFF is written when the pad is released. No QA row. |
| Recording options: quantise, micro-timing, velocity | §3.7, §5.7 (Setting Quantize Recording Options) | One list: notes only, micro-timing, velocity, or both. | different | QA-CFG-026..030, QA-PAT-059..061 | Ours is REC QUANTIZE, REC MICRO, REC VELOCITY; OS-RECMICRO says the rules are new, not recovered from the unit. Velocity goes to the Vol column; where the official firmware writes it is reported there, not verified (open research question). |
| Metronome | §3.5 Metronome | A click with state, pre-roll, time signature 1 to 12 / 1 to 12, volume. | different | QA-CFG-016..025, OS-METRO | Ours: METRO Off, Cue or Main, METRO VOL, COUNT-IN Off, 1 or 2 bars. No time-signature rows (a fixed 16-step bar). |
| When the metronome clicks | §3.5 | Per the manual the click sounds only during a live take. | open | OS-METRO | Ours clicks whenever the transport runs (OS-METRO D3, our choice). Which is right is an open research question. |
| Count-in before recording | §3.5, §5.7 | A four-beat lead-in with an on-screen countdown precedes the take. | different | QA-PAT-052..057, QA-CFG-020, QA-CFG-025 | Ours: 1 or 2 bars; the strip counts 4.. 3.. 2.. 1.. from one beat on. Row 1 is the first step played after it (QA-PAT-230). |
| Fill utility | §5.6 Fill Recording Multiple Steps | A dialog fills a selected range by rule. | different | QA-PAT-086..095 | Ours: F4, eight rows SCOPE, WHAT, TYPE, N / EVENTS / DENSITY, MODE, KEY (root and scale) or FX TYPE, FROM, TO; note, instrument, volume and FX 1 / FX 2, in CONST, RAMP or RANDOM (OS-FILL2). |
| Fill: where to write (Note, No Note, FX, No FX, Random, Each, Euclidean) | §5.6 | Picks which steps of the range are filled. | different | QA-PAT-086, QA-PAT-094 | Ours has every N, random, Euclid and scale as TYPE. No "only where a note exists" choices. |
| Fill: constant, from-to, random values | §5.6 | Fills with one value, a ramp, or random values. | missing | `docs/specs/OS-PTOOLS.md` section 4 (Q2) | "Not in this slice". Ours uses the current note, instrument or volume. |
| Fill an FX column | §5.6 | The same dialog fills FX1 or FX2, for example a pan ramp. | missing | `docs/specs/OS-PTOOLS.md` section 4 (Q2) | Fill writes note, instrument and volume only. |
| Euclidean fill | §5.6 | Distributes N hits across the selection with the most regular spacing it can. | open | QA-PAT-087, `docs/specs/OS-PTOOLS.md` (Q1) | The rounding rule is ours; the official build's rule is not known. |
| Fill scales | §5.6 | About forty scales are offered. | open | QA-PAT-095 | Four scale definitions were corrected (Melo Min, Blues Maj, Todi, Gipsy); `docs/architecture/os-tree.md` lists four scales as unverified. |
| Step popup list and pad picker | §5.9, §5.10, §7.2 | Holding a column key lists the instruments or FX to pick from. | same | QA-PAT-121, QA-PAT-224, QA-PAT-253 | These rows cover the instrument list, its pad picker and the Shift+encoder paging. QA-PAT-015 checks only the order of the FX list, so the held FX list itself is not counted as evidence. Ours also pages the list with Shift+Up/Down. |
| Show an FX description | §5.10 FX Step Parameters, §7.2 Step FX Reference, RN 1.9.0 | The last two screen keys show or hide a help text for the highlighted FX. | open | QA-PAT-011..014 | Ours reads the value in words on the info line; no description pane. |
| Focus Loop | RN 1.9.2 | A chosen block of the pattern repeats on its own until playback is stopped. | missing | `docs/ui/ui-map.md` section 4 | Planned in the ui-map. Shift+Play plays from the cursor's row today, also with a block selected, until the loop exists (QA-PAT-233, `docs/ui/ui-map.md` Pattern row). |
| Polyphony by using several tracks | §5.8 | A track sounds one note at a time, so stacking a chord takes several tracks. | same | `docs/design/product-ux.md` section 2 | Ours is monophonic per track too; Chord and Arp FX play several notes from one step (QA-PAT-016..018). |
| A pad plays the whole step in edit mode | §5.8 (note under the scale picture) | Pads sound the full step including FX while editing, but only note and instrument otherwise. | open | QA-PAT-225 | Ours: a pad in STEP sounds the note; whether the FX of the step play with it was not checked. |
| Pattern tools: apps | not in the manual | - | new | QA-PAT-160..167, QA-PAT-210..213 | F7 TOOLS lists module apps (Rhythm Gen, Humanize, Chords, Mutate, Morph, SMF export). |
| Zoom and quarter keys | not in the manual | - | new | QA-PAT-236..245 | See the rows in 3.2. |

## 4. Step FX

Our home: the Fx columns of Pattern (`docs/manual/fx.md`; rows in `docs/qa/catalogue/pattern.md`). The manual's FX chapter (§7) lists 37 types; the picker of the OS has 60 entries (`model/include/o3os/model/fx_table.h`). Ranges below come from that table. The rows follow our FX type number (`seq/include/o3os/seq/fx.h`, the first column), not the manual's order 7.3 to 7.39; the official section number stays in its own column. A "same" with no QA row rests on the table and `docs/specs/OS-009-pattern-editing.md` D9 (the table's type numbers are those of the OG pattern file), and the sound or timing was not compared with a unit.

| FX | Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|---|
| 0 | None | §7.3 None | An empty FX slot. | same | QA-PAT-104 | Picking None clears the cell. |
| 1 | Off | §7.4 Off | Ends an FX that is still running from an earlier row on that track. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 2 | Micro-move | §7.11 Micro Move | Delays the note slightly off the grid; the scale is 0 to 100. | same | QA-PAT-059, QA-PAT-060 | A delay only. |
| 3 | Roll | §7.14 Roll | Retriggers the note inside the row in six ways: steady, softer, louder, lower, higher or random pitch. | same | `docs/specs/OS-005-audio-engine.md` O13, O14 | A few details differ: period 0 is inert (O13) and the volume arithmetic is an integer floor, at most one apart (O14). |
| 4 | Chance | §7.13 Chance | On each pass the row sounds only some of the time, by a percentage. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 5 | Random note | §7.16 Random Note | Picks a different pitch near the written one on every pass; the value sets the spread. | same | `docs/specs/OS-009-pattern-editing.md` D9 | 0 to 100. No QA row. |
| 6 | Random instrument | §7.17 Random Instrument | Swaps in a neighbouring instrument of the list at random on every pass. | different | `docs/specs/OS-005-audio-engine.md` O9 | Ours stays inside slots 0 to 63 whatever the kind. OS-005 O9 records a narrower official range (the sample or the MIDI partition) without evidence: open research question. |
| 7 | Random volume | §7.19 Random Volume | Wobbles the level around the default by up to the value on each pass. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 8-12, 37 | MIDI CC messages A to F | §7.38 MIDI Messages | Six FX send a controller value; the numbers are set on the MIDI instrument. | same | QA-INS-072, QA-INS-079 | CC F is FX 37 (`model/include/o3os/model/fx_table.h` notes that the slot held a Program Change before; not checked here). |
| 13 | Break pattern | not in the §7 list | - | open | `docs/specs/OS-005-audio-engine.md` | FX 13 is in our table, but the 1.7.0 manual lists no such FX. Not checked. |
| 14 | MIDI chords | §7.39 MIDI Chords | A hex code names the notes above the root; about 30 chords. | same | QA-PAT-011, QA-PAT-016 | 30 values (`model/include/o3os/model/fx_table.h`). |
| 15 | Tempo | §7.9 Tempo | Scales the pattern tempo by a percentage from that row on; the lowest setting stops the sequencer. | different | `docs/specs/OS-005-audio-engine.md` O4, O21 | The stop value is not implemented (O4); the FX tempo is ignored while following an external clock (O21). |
| 16 | Random FX value | §7.18 Random FX | Makes the setting of the sibling FX cell vary at random each time round. | same | `docs/specs/OS-009-pattern-editing.md` D9 | 0 to 255. No QA row. |
| 17 | Swing | §7.10 Swing | Pushes off-beat rows late across the whole pattern, 25 to 75 percent; 50 is straight. | same | `docs/specs/OS-005-audio-engine.md` (Timing) | The spec calls the arithmetic new Community OS behaviour (whether the official firmware's swing is the same is an open research question); its scope (O5) is an open item. |
| 18 | Volume / velocity | §7.5 Volume | A per-row level control; on a MIDI instrument it is the note-on velocity. | same | `docs/specs/OS-RECMICRO.md` section 0 | 0 to 100. No QA row for the sound. |
| 19 | Glide | §7.8 Glide | The pitch slides in from the earlier note; the value sets how long the slide takes. | same | QA-PAT-025, QA-PAT-028 | `docs/specs/OS-IFX1.md` marks the time law Q: parity is not recorded. |
| 20 | Gate length | §7.12 Gate Length | Sets how long the note is held open before its release, 0 to 100. | open | `docs/specs/OS-005-audio-engine.md` O2 | The mapping of 0 to 100 and its interplay with Roll are an open item. |
| 21 | Arpeggiator | §7.15 Arpeggiator | Plays the notes of the chord set in the neighbouring FX slot one after another, in a chosen direction and at a chosen division. | same | QA-PAT-012, QA-PAT-017, QA-PAT-018, QA-PAT-021 | An Arp with no Chord changes nothing. |
| 22 | Position | §7.21 Position | Chooses where in the sample playback starts, or the wavetable or grain position. | same | `docs/specs/OS-009-pattern-editing.md` D9 | 0 to 100. No QA row. |
| 23 | Volume LFO rate | §7.23 Volume LFO | Sets how fast the instrument's volume LFO runs from this step. | same | `docs/specs/OS-009-pattern-editing.md` D9 | The LFO is the Instrument MOD page's (QA-INS-092, QA-INS-093). No QA row for the FX itself. |
| 24 | Panning LFO rate | §7.24 Panning LFO | Sets how fast the panning LFO runs from this step. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 25 | Slice | §7.22 Slice | Triggers one numbered slice; numbers beyond the slice count fold back around. | same | `docs/specs/OS-009-pattern-editing.md` D9 | 0 to 47. No QA row. |
| 26 | Reverse sample | §7.20 Reverse Sample | Chooses backward or forward playback of the sample for that row. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 27 | Low-pass cutoff | §7.29 Low Pass Cutoff | Sets the low-pass cutoff of the instrument; 100 is open. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 28 | High-pass cutoff | §7.31 High Pass Cutoff | Sets the high-pass cutoff; 0 is open. | same | `docs/specs/OS-009-pattern-editing.md` D9 | - |
| 29 | Band-pass cutoff | §7.30 Band Pass Cutoff | Sets the band-pass cutoff. | same | `docs/specs/OS-009-pattern-editing.md` D9 | - |
| 30 | Delay send | §7.32 Delay | Sets how much of the step goes to the master delay. | open | QA-PRF-028 | Acts since OS-PERFFX P3: the value 0 to 100 is the track's delay send in place of the instrument's while the FX is held (new Community OS behaviour; the official firmware's exact rule is an open research question). |
| 31 | Panning | §7.6 Panning | Places that one row in the stereo image, hard left to hard right. | same | QA-PAT-117 | 0 to 100 centred on 50. It ends with its step. |
| 32 | Reverb send | §7.33 Reverb | Sets how much of the step goes to the master reverb. | open | QA-PRF-028 | Acts since OS-PERFFX P3, as FX 30 for the reverb (the official rule is an open research question here too). |
| 33 | Finetune LFO rate | §7.27 Finetune LFO | Sets how fast the finetune LFO runs from this step. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 34 | Micro-tune | §7.7 Micro-Tune | Detunes the row in cents, staying just under a semitone. | same | `docs/specs/OS-009-pattern-editing.md` D9 | -99 to +99 cents stored as 0 to 198. No QA row. |
| 35 | Filter LFO rate | §7.25 Filter LFO | Sets how fast the cutoff LFO runs from this step. | same | `docs/specs/OS-009-pattern-editing.md` D9 | No QA row. |
| 36 | Position LFO rate | §7.26 Position LFO | Sets how fast the wavetable or grain position LFO runs from this step. | same | `docs/specs/OS-009-pattern-editing.md` D9 | The position LFO moves while stopped (QA-INS-087). |
| 38 | Overdrive | §7.28 Overdrive | Adds drive to the step, 0 to 100. | same | QA-PAT-022, QA-PAT-026 | `docs/specs/OS-IFX1.md`: the control matches; the curve is new, no parity claimed. |
| 39 | Bit depth | §7.34 Bit Depth | Lowers the bit depth; the text says 4 to 16. | different | QA-PAT-023, QA-PAT-027 | `docs/specs/OS-IFX1.md` marks the range Q: ours is 1 to 16 for the FX and 4 to 16 for the instrument, and how the official firmware limits the two is an open research question. |
| 40 | Tune | §7.35 Tune | Transposes the step by -24 to +24 semitones. | same | QA-PAT-013, QA-PAT-019 | Default 24 (no change). |
| 41, 42 | Slide up and slide down | §7.36 Slide Up, §7.37 Slide Down | Bends the pitch away from the written note in 1/16-semitone units, 0 to 255. | same | QA-PAT-014, QA-PAT-020 | - |
| 46-62 | Tracker FX (arpeggio, volume slides, pan slide, waveforms, retrigger, tremor, channel and global volume, panbrello) | not in the manual | - | new | QA-PAT-096..117, QA-PAT-140..149 | Types 46 to 62, the last 17 entries of the picker (OS-COREFX), for MOD, S3M, XM and IT import fidelity. |
| 128-255 | FX that module instruments obey | not in the manual | - | new | QA-MOD-099..104 | See `docs/manual/module-instrument-fx.md`. |

## 5. Song

Our home: Song (`docs/qa/catalogue/song.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Song as a list of pattern slots, up to 255 | §9.2 Arrangement Overview, §9.3 Song Mode Interface | A project carries a single song, a column of rows that each point at a pattern number. | same | `docs/manual/song.md`, `docs/specs/OS-004-project-model.md` | Slot count limit not compared. |
| Add a slot | §9.5 Creating and Editing a Song, RN 1.7.0 | A new row appears under the cursor, starting as a duplicate of it. | same | QA-SNG-003 | F3 ADD SLOT or Insert, with the same pattern number. |
| Add a slot with a copy of the pattern | RN 1.7.0 (Copy Pattern) | Song mode gained a way to duplicate patterns; the release note says no more. | open | QA-SNG-003 | Ours has Shift+Insert (DUP PAT): a new slot holding a copy of the pattern at the first free number. Whether the official function does the same is not known. |
| Delete a slot | §9.5 | The chosen row disappears and later rows close the gap. | different | QA-SNG-012, QA-SNG-013 | F4 or Delete. Ours undoes it (QA-SNG-013); the manual says undo does not restore a deleted slot. QA-SNG-012 is host evidence only (unit open). |
| Change the pattern of a slot | §9.5 | The side keys, or Shift with the wheel, re-point the row at another pattern, any of 255. | same | `docs/manual/song.md` | F5 PAT-, F6 PAT+, or the encoder. |
| Undo on Song | §9.5, §9.3 | An Undo key takes back song edits. | different | QA-SNG-013, QA-PAT-247 | One history per area: Song undo only touches Song. Shift+F8 redoes. |
| Play song or play pattern | §9.6 Playing Songs and Patterns | Screen keys play the song or loop the current pattern; Shift+Play toggles. | different | `docs/manual/song.md` | Ours: F1 PLAY PAT, F2 PLAY SNG. Shift+Play is play-from-cursor. |
| The song loops at its end | §9.6 | Reaching the final slot wraps playback round to the first. | same | `docs/specs/OS-005-audio-engine.md` O18 | `Config::song_loop`. No QA row. |
| Tracks shown as clips; dark, dimmed, white | §9.3 | Each slot shows its 8 tracks as clips by content and mute state. | missing | - | Ours lists pattern numbers and names with each slot's start bar and length in bars; TIMELINE draws the blocks. |
| Copy and paste track clips | §9.5 | Moves track clips between slots; because a clip is the pattern's own track, the pattern changes too. | missing | - | Ours: Copy and Shift+Copy copy slots (a range of pattern numbers), not tracks inside patterns. |
| Preview one track clip | §9.7 Link between Song and Performance Mode | Shift+Play or Shift+Preview plays a single clip once. | missing | - | No clip view. |
| Green and red modes on Song | §9.3, §9.5 | Rec switches Song between playing and editing. | different | `docs/ui/ui-map.md` Song row | Ours edits without a mode; Rec queues the next slot while the song plays. |
| Queue a jump in a playing song | not in the manual | - | new | `docs/ui/ui-map.md` Song row | Rec on a slot makes it the next one. |
| Quarter jumps and names on Song | not in the manual | - | new | QA-SNG-001, QA-SNG-002, QA-SNG-010 | Shift+F1 to F5 jump; Shift+F6 names the pattern. |
| List page with BAR and BARS | not in the manual | - | new | `docs/manual/song.md` | The start bar and the length in bars of each slot on the list (OS-SONG3 S1 folded the ARRANGEMENT page into it). |
| Empty slot | RN 1.7.0 | A command to insert a blank row was added to Song mode. | open | - | RN 1.7.0 names an Empty Slot function; the 1.7.0 manual text shows Add Slot copying the row. The difference was not resolved. |
| Song tempo field | §3.4, §9.1 Tempo | Tempo lives on the Song page. | same | `docs/manual/song.md` | F7 TEMPO. See the global tempo row. |

## 6. Remaining screens

### 6.1 Instrument (Params)

Our home: Instrument (`docs/qa/catalogue/instrument.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Two parameter pages on the Params button | §6.2 Instrument Parameter Pages | The Params button flips between a general page and a modulation page. | different | QA-INS-010, QA-INS-011 | Ours: five pages on F1 to F5 (source, voice, filter, mod, sends); Left/Right turn them. |
| Volume, panning, tune, finetune | §6.3 General Instrument Parameters | Level up to +24 dB, pan -50 to +50, tune +-24 semitones, finetune +-100 cents. | same | `docs/manual/instrument.md` (volume law) | VOICE page. The volume law is explained in the chapter (OS-BUG-PASTE24). No QA row for the ranges. |
| Filter type, cutoff, resonance | §6.3 | Disabled, low-pass, high-pass or band-pass, with cutoff and resonance. | same | `docs/manual/instrument.md` (FILTER page) | FILTER page. No QA row for the filter types or the sound; QA-INS-085 checks only that a cutoff sweep is one undo unit. |
| Instrument overdrive and bit depth | §6.4 Instrument Effects | Overdrive 0 to 100 and bit depth 4 to 16 on the instrument. | same | QA-INS-002, QA-INS-003 | SENDS page. Overdrive's curve is new (OS-IFX1). |
| Instrument delay and reverb sends | §6.4 | Per-instrument sends to the master effects. | same | `docs/manual/instrument.md` | SENDS page. No QA row. |
| Instrument glide | not in §6.3 | - | open | QA-INS-004 | OS-IFX1 marks the glide time law Q (parity not recorded); the manual lists glide only as an FX. |
| Automation: destinations | §6.5 Instrument Automation Parameters | Volume, panning, cutoff, wavetable position, granular position, finetune. | same | QA-INS-035, QA-INS-037 | F4 MOD, six destinations. |
| Automation: envelope or LFO or off | §6.5 | A destination can be driven by a looping LFO, a one-shot envelope, or left static. | same | QA-INS-035, QA-INS-036, QA-INS-095 | SOURCE row. |
| LFO shapes and tempo-synced speeds | §6.5 | Five shapes; speeds from 128 steps to 1/64 of a step. | different: six shapes, the sixth is Sine (OS-LFOSINE, new Community OS behaviour) | QA-INS-038, QA-INS-092, QA-INS-093 | Labels follow the manual's speed list. |
| Envelope: attack, decay, sustain, release | §6.5 | A 0 to 10 s envelope; amount 0 to 100. | open | QA-INS-035 | The stages match (QA-INS-035 lists AMOUNT, ATTACK, DECAY, SUSTAIN and RELEASE); the ranges (0 to 10 s, amount 0 to 100) were not compared. |
| Instrument slots | §3.6 Project Pool & Instrument List | Up to 48 instruments per project. | different | QA-PAT-120, QA-PAT-121 | Ours has 64 slots: 1 to 48 and 49 to 64 for MIDI instruments. |
| Instrument file type `.pti` | §6.12 Sharing Instruments | An instrument is saved as a `.pti` file. | same | QA-INS-052, QA-INS-059, QA-FIL-090 | Shift+F5 SAVE writes `instruments/<name>.pti`; LOAD reads it. |
| Rename an instrument | §6.12, §3.6 | Renamed in the sample loader. | different | QA-INS-050, QA-INS-051 | Ours: Shift+F1 NAME on the Instrument screen. |
| Copy and paste an instrument | §3.6 | Duplicates an instrument in the loader. | same | QA-INS-043, QA-INS-044, QA-INS-045 | Shift+F2 and Shift+F3 on the Instrument screen. |
| Delete an instrument | §3.6 | Drops the slot from the project while the source files stay on the card. | same | QA-INS-030 | Shift+F4; one undo unit. |
| Delete unused instruments | §3.6 | A single command clears every slot that no step refers to. | same | QA-FIL-037, QA-FIL-038 | File Shift+F7 PURGE. |
| MIDI instruments | §11.4 MIDI Instrument Overview | Slots that send on a chosen MIDI channel and can share a track with sampled instruments. | open | QA-PAT-120..123, QA-INS-061..081 | The numbering is open: ours shows them as 49 to 64, the 1.7.0 manual's pictures number them 51 to 66 in places and its text gives a shorter range. |
| MIDI instrument: channel, velocity, offset | §11.4 | Default velocity and a latency offset per MIDI instrument. | same | QA-INS-070, QA-INS-071, QA-INS-080 | F1 MIDI page: CHANNEL, VELOCITY, OFFSET. |
| MIDI instrument: six CC slots | §11.6 MIDI CC Out | Six slots, A to F, each hold a controller number. | same | QA-INS-072, QA-INS-079 | F2 CC page. |
| Program change, channel aftertouch, poly aftertouch | §11.6, §11.8 Program Change | CC slots can be program change or aftertouch. | same | QA-INS-073, QA-INS-075, QA-INS-079 | Each CC slot: CONFIG, CC, PROG, CH AT, POLY AT. |
| Pitch-bend range, output port per MIDI instrument | not in the manual | - | new | QA-INS-077, QA-INS-078 | OUT and PB RANGE rows. |
| MIDI synthesizer mode (Instrument MIDI Mapping) | §11.9 MIDI Synthesizer, RN 1.7.0 (page moved under More), RN 1.9.0 (renamed) | An external controller drives the instrument's parameters through a fixed CC map; the sequencer must be stopped. | different | QA-GLB-178 | No fixed CC map or page. Learn (FX1+FX2) with `F5` FOLLOW stands in: a binding follows the selected instrument (QA-GLB-178), with no stop-the-sequencer gate. The preset mapping for the maker's other instrument (Play) is part of the same feature. |
| Instrument modulation matrix | not in the manual | - | new | `docs/specs/OS-MODMX.md` | Planned (M4). |
| Module instruments (303, drum, others) | not in the manual | - | new | QA-INS-029, QA-GLB-002 | F4 MODULE on a free slot. |
| Velocity scaling on the VOICE page | not in the manual | - | new | QA-INS-082 | VEL> row. |
| Instrument type chooser on a free slot | not in the manual | - | new | QA-INS-025..029 | SAMPLE, MIDI, PASTE, MODULE, LOAD. |
| Instrument INIT | not in the manual | - | new | QA-INS-032 | Shift+Delete. |

### 6.2 Wave and Chop (Sample playback)

Our home: Wave (`docs/qa/catalogue/wave.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Eight play modes | §6.6 Sample Playback Overview | One-shot, three loops, slice, beat slice, wavetable, granular. | same | QA-WAV-003, QA-INS-015..018 | Chosen on Instrument F1 PLAY MODE or Wave Shift+F1. |
| One-shot: start, end, zoom, preview | §6.7 1-Shot Play Mode | Start and end markers, zoom, hold to preview. | same | `docs/manual/wave.md` (key table) | START, END, ZOOM-, PREVIEW. No QA row (QA-WAV-001 lists only the Shift labels): not checked. |
| Loop modes: start, loop start, loop end, end | §6.8 Loop Play Modes | Forward, backward and ping-pong loops over loop markers. | same | `docs/manual/wave.md` (key table) | START, LOOP S, LOOP E, END. No QA row for the loop playback or the markers: not checked. |
| Snap to zero crossings | §3.7, RN 1.7.0, RN 1.8.0 | A project option snaps markers to zero crossings; release 1.8.0 refined the detection. | different | `docs/manual/wave.md` (key table) | Ours: a ZC key on the WAVE page (F5) and on the CHOP page (F3). There is no project setting, and the detection was not compared. No QA row for the key (QA-WAV-001 lists only the Shift labels). |
| Slice: select, adjust, add, remove | §6.9 Slice Play Modes | Edit up to 48 slice points one at a time. | same | `docs/manual/wave.md` (key table), QA-WAV-006 | CHOP page: ADD, DEL, nudge. QA-WAV-006 only opens the CHOP page; no QA row for the slice edits. Maximum count not compared. |
| Auto slice by transients | §6.9 | Clears the markers and places new ones on detected hits, asking first. | same | `docs/manual/wave.md` (key table), QA-WAV-006 | CHOP AUTO. No QA row beyond the CHOP page. The confirmation prompt was not compared. |
| Equal slices | §6.9 | Cuts the sample into N even parts. | same | `docs/manual/wave.md` (key table), QA-WAV-006 | CHOP EVEN; Shift+Up/Down sets the count. No QA row beyond the CHOP page. |
| Slice mode: pads play the slice in scale; beat slice: a pad per slice | §6.9 | Two behaviours for the same slices. | open | `docs/ui/ui-map.md` Wave row | Ours: a pad plays a slice and Shift+pad selects, with beat slice = slice n on pad n; the scale behaviour of plain SLICE was not checked. |
| Wavetable: position and window | §6.10 Wavetable Play Modes | A window size from the file and a position inside the table. | same | QA-WAV-004, QA-WAV-007, QA-WAV-008 | POSITION and WINDOW. |
| WT Smoother | §6.10, §8.10 Sample Editor Effects | Reprocesses an ordinary sample so that scanning its position sounds less stepped. | different | `docs/manual/sample_load.md` | Ours: WT AUTO on Sample load picks how a WAV becomes a wavetable when loaded. No smoothing effect. |
| Granular: position, length, shape, loop | §6.11 Granular Play Modes | One grain; length 1 to 1000 ms; square, triangle or gauss; forward, reverse, ping-pong. | same | QA-WAV-005, QA-WAV-007 | POSITION, LENGTH, SHAPE, LOOP. |
| LFO or envelope on grain or wavetable position | §6.10, §6.11 | Instrument automation moves the position. | same | QA-INS-087, QA-INS-088, QA-INS-091 | MOD page destinations. |
| Zoom | §6.7 to §6.11 | Magnifies the waveform for fine marker work; the sound is unaffected. | same | `docs/manual/wave.md` (key table) | ZOOM- on F7 and Up. No QA row (QA-WAV-001 lists only the Shift labels): not checked. |
| Play-mode list on two keys | §6.6 | The 7th and 8th screen keys step through the modes. | different | QA-WAV-003 | Ours: a list on Shift+F1. |
| Preview from the selected point | not in the manual | - | new | QA-WAV-001 | Shift+F6 PRV SEL. |

### 6.3 Sample edit

Our home: Sample edit (`docs/qa/catalogue/sample_edit.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Edit range start and end | §8.9 Advanced Audio Editing | Two markers set the span that an effect touches. | same | `docs/manual/sample_edit.md` | Edge picks with Left/Right, the encoder moves them. No QA row. |
| Crop | §8.10 Sample Editor Effects | Keeps only the selection. | same | `docs/manual/sample_edit.md` | F1. No QA row. |
| Reverse | §8.10 | Reverses the selection. | same | `docs/manual/sample_edit.md` | F3. No QA row. |
| Normalizer | §8.10 | Gain to a chosen peak level. | different | QA-SED-002, OS-SEDFX | Ours normalises to full scale with no level setting. |
| Amplifier | §8.10 | Scales the level by a factor between 0.01 and 5. | different | `docs/manual/sample_edit.md` | F6 AMP by a percentage with Shift+Up/Down. |
| Fade in and fade out | §8.10 | Quick ramps up or down over the selected span, with no settings. | same | `docs/manual/sample_edit.md` | F4 and F5. No QA row. |
| Overdrive (offline) | §8.10 | Distortion with a choice of three flavours plus a drive control. | missing | OS-SEDFX | Out by the maintainer's decision of 2026-10-16: the editor keeps normalise and DC remove, and printing live DSP (filter, bitcrush, delay, plate) is not built (BACKLOG, `docs/specs/OS-SEDFX.md`). |
| Delay (offline) | §8.10 | Echo with time and feedback. | missing | OS-SEDFX | As above. |
| Bitcrusher (offline) | §8.10 | Bit depth and sample-rate reduction. | missing | OS-SEDFX | As above. |
| Chorus and flanger (offline) | §8.10 | Voices and length; length, offset, depth, rate. | missing | OS-SEDFX | As above. |
| Equalizer (offline) | §8.10 | Three bands, +-12 dB. | missing | OS-SEDFX | RN 1.7.0 refined the editor's EQ, compressor and limiter. |
| Limiter and compressor (offline) | §8.10, RN 1.7.0 | Threshold, attack, release; ratio for the compressor. | missing | OS-SEDFX | As above. |
| Time stretch, beat and note | §8.10 | Lengthens or shortens the sample while keeping its pitch, in a rhythm-minded and a melody-minded variant. | missing | OS-SEDFX | As above. |
| Undo | §8.10 | Rolls the sample back by one effect. | same | QA-SED-004 | F8; Shift+F8 redoes. |
| Preview with the pads | §8.9 | A pad plays the sample. | different | `docs/ui/ui-map.md` Sample edit row | Ours: a pad selects the instrument; F7 PREVIEW plays. |
| Pick the instrument by holding Instrument | §8.9 | Holding the Instrument key opens a pick list for the sample to work on. | same | `docs/ui/ui-map.md` section 2 | Instr hold lists the sample instruments (OS-INSTPICK). |
| DC removal, Amiga conversion, 8-bit | not in the manual | - | new | QA-SED-001, QA-SED-003, QA-SLD-009 | Shift+F1 to F4. |
| Save the edited sample as a WAV | not in the manual | - | new | `docs/manual/sample_edit.md` | Shift+F6 SAVE WAV, to Samples/. |

### 6.4 Sample load and the instrument pool

Our home: Sample load (`docs/qa/catalogue/sample_load.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Browse the card and load a file into a slot | §3.6 Project Pool & Instrument List | A browser on the left, the instrument list in the middle. | different | QA-SLD-003 | Ours: one list; a pad picks the target instrument; F1 LOAD, F4 ADD into the first free slot. |
| Add and add-next | §3.6 | Add drops the file in the highlighted slot; Add Next does that and then steps the cursor on. | different | `docs/manual/sample_load.md` | Ours: F1 LOAD and F4 ADD (first free slot). No "next slot" key. |
| Batch select several files | §1.9, §3.6 | Holding Shift with the arrows marks a group of files for one import. | missing | - | One file at a time. |
| Import with start and end trim | §3.6 (Importing) | An import page trims the sample before it enters the list. | missing | - | Trim after loading in Sample edit. |
| Low-quality import | §3.6 | A smaller, lower-quality copy to save memory. | missing | - | No such key. |
| Memory bar | §3.6 | A gauge of project memory, with a coloured tip showing what the highlighted file would add or free. | open | - | Config INFO shows SAMPLE MEMORY (`docs/manual/config.md`); no memory bar on Sample load was found. |
| Preview a file; preview volume | §3.6, RN 1.7.0 | Hear a file; adjust the preview level with the arrows. | different | QA-SLD-006 | Ours: F5 PLAY auditions. No preview-level control. |
| Wave formats | §8.5 Sample Formats | Plain WAV is the accepted type, and everything is brought to 44.1 kHz, 16-bit, one channel on import. | different | QA-SLD-002..008 | Ours also loads AIFF and IFF 8SVX (QA-SLD-002..004) and keeps each file's own rate (OS-RATEPITCH). |
| Stereo file channel | §8.5 Sample Formats | A stereo file is turned into a mono sample on import. | different | QA-SLD-011..013 | Ours: F8 picks mix, left or right. |
| Mod files | RN 1.8.0 (beta import, `.it` export) | Tracker module files can be read in beta, and a project can go out as an IT file. | different | QA-FIL-005, QA-FIL-006, `docs/specs/OS-FILES-IMPORT.md` | Ours imports MOD, S3M and MED from File; no `.it` export. |
| Samples folders of the card | §2.3 | Sample packs under Samples/. | same | QA-SLD-002 | Folders first, files listed. |
| Maximum sample time: about 45 s each, 133 s per project | §8.5, §8.7 Sampling, RN 1.8.0 | Memory limits. | open | `docs/specs/OS-004-project-model.md` | Ours reads its limit from the sample arena (QA-FIL-011); the numbers were not compared. |

### 6.5 Record (Sample recorder)

Our home: Record (`docs/qa/catalogue/record.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Sources: line in, mic, radio | §8.6 Recording Audio Workflow, §8.7 Sampling | Choose the input to record. | same | `docs/manual/record.md` | SOURCE row: LINE, LINE L, LINE R, MIC LG, MIC HG, RADIO, USB. USB is new. |
| Gain and level meter | §8.7 | Gain control and a level display. | same | `docs/manual/record.md` | GAIN row, one gain per input type. |
| Monitor | §8.7 | Hear the input at the output. | same | `docs/ui/ui-map.md` Record row | F2 MONITOR. |
| Record, stop, trim, save | §8.7 | Capture up to the memory limit, trim the ends, then store on the card with or without adding to the instrument list. | different | QA-REC-010, `docs/manual/record.md` | Ours: F1 REC/STOP, TRIM START and END rows, F3 SAVE (into the first free instrument slot), F4 DISCARD, F7 SAVE WAV (`docs/ui/ui-map.md`). No separate "save only" key was found. |
| Rec button starts and stops recording | RN 1.7.0 | The physical Rec button toggles sample recording. | same | `docs/ui/ui-map.md` Record row | Rec = F1. No QA row. |
| FM radio and region | §8.4 FM Radio | Tune with the wheel or scan keys; the region sets the grid. | same | QA-REC-011, QA-REC-012 | RADIO page: FREQUENCY, REGION; F5 and F6 seek. |
| Resample (record the unit's own output) | §8.8 (render) | The unit's own sound is captured by bouncing, not through the recorder inputs. | new | QA-REC-001 | RESAMPLE page taps the master, the REC bus or a track. |
| Start and stop on a boundary | not in the manual | - | new | QA-REC-010, QA-PAT-080..085 | START ON and STOP AT. |
| Looper, overdub, loop save | not in the manual | - | new | QA-REC-020..033 | OS-LOOPER. |

### 6.6 Mixer (Master)

Our home: Mixer (`docs/qa/catalogue/mixer.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Master pages | §8.1 Master Pages - Audio Control, §10.2 Master Pages | The Master button steps through three views: the master strip, a per-track mixer and a global mixer. | different | `docs/manual/mixer.md` | Ours: four pages on F1 to F4 (MIXER, ROUTING, MASTER FX, STEMS). |
| Track levels with meters | §8.1 | One level per track with a live bar. | same | `docs/manual/mixer.md` | MIXER page; ours adds a pan per track. No QA row. |
| Mute applied on release | §1.5, §8.1 | A mute on the mixer page only bites once the key comes back up. | open | QA-MIX-020 | Ours mutes with F7; the timing relative to key release was not compared. |
| Solo | §8.1 | Shift plus the track key. | different | QA-MIX-004 | Ours: F8 SOLO; Shift+F8 clears all solos. |
| Rename tracks on the mixer page | §8.1, §4.6 | Rec then a track key. | different | QA-MIX-010 | Shift+F1 NAME. |
| Global mixer: line in, sends, delay, reverb, dry | §8.2 Audio Sources, §10.2 | Page 3 holds the line-in pass-through, the master delay and reverb mix, and the dry level. | different | `docs/manual/mixer.md` | Ours: buses IN, SNDA, SNDB, CUE, MST; ROUTING page with LINE IN and returns. |
| Master saturation | §10.6 Saturation, Space & Bass Boost | Five distortion presets. | missing | QA-MIX-002 | Reserved stage, not listed. |
| Master bass boost and space | §10.6 | Bass boost 0 to 100; stereo widener 0 to 8. | missing | QA-MIX-002 | Reserved. |
| Master EQ, five bands | §10.7 EQ | Five fixed bands, +-12 dB. | missing | QA-MIX-002 | Reserved. |
| Master delay | §10.3 Master Delay | Ping-pong, sync, rate, time, feedback, filter. | different | `app/include/o3os/app/send_fx_rows.h` | Ours: DLY PINGPONG, SYNC, RATE, TIME, FEEDBACK (max 70), TONE, MODE. The manual's feedback goes to 99 and its filter is a cutoff in Hz; ours is a tone percentage. |
| Master reverb | §10.4 Master Reverb | Size, damping, pre-delay, diffusion, 0 to 100. | same | `app/include/o3os/app/send_fx_rows.h` | RVB SIZE, DAMPING, PREDELAY, DIFFUSE (and QUALITY, new). No QA row. |
| Master limiter | §10.5 Limiter | Attack, release, threshold, gain-reduction meter, sidechain. | different | QA-MIX-002, `app/include/o3os/app/limiter_rows.h` | Ours: THRESH in per cent, ATTACK and RELEASE 1 to 1000 ms; always on. No sidechain, no meter. The limiter mode and the sends mode are mapped under Config (6.9). |
| Limiter sidechain from a track or line in | §10.5 | A chosen track or one side of the line input can drive the gain reduction instead of the main mix. | missing | - | Not present. |
| Dry/wet level of effects | §10.2 | A dry mix level on the global page. | open | - | Ours has returns on ROUTING; no dry level row was found. |
| Stems routing to USB | not in the manual | - | new | `docs/manual/mixer.md` | STEMS page. |
| Cue bus, routing matrix, out modes | not in the manual | - | new | `docs/manual/mixer.md` | ROUTING page. |

### 6.7 Perform

Our home: Perform (`docs/qa/catalogue/perform.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Twelve effect slots on the pads | §12.1 Performance Mode Overview, §12.4 Perform Effect Values | Each pad column is a slot; row 1 is off, rows 2 to 4 hold values. | same | `seq/include/o3os/seq/perform.h` | Pad n is slot n % 12 on row n / 12. No QA row. |
| Choose which tracks are affected | §12.3 Selecting Tracks | Screen keys toggle the tracks the effects touch. | different | `docs/manual/perform.md` | Ours: hold Shift; the top row of pads chooses (12 tracks). |
| Punch in an effect with a pad | §12.2 Performing - Quick Start | Tapping a pad switches the effect value on and it stays on until cleared. | same | `docs/manual/perform.md` | LATCH is on by default (the manual's behaviour); LATCH off is new. |
| Temporary value while a pad is held | §12.2 | Turning the wheel with a pad held bends the value only for as long as the pad is down. | same | `docs/manual/perform.md` | The encoder changes the held slots. |
| Edit mode for slots | §12.5 Configuring Effects | Rec edits the template: pick the effect, set the values. | same | `docs/manual/perform.md` | Rec = EDIT; Up/Down change the effect. |
| Swap slots | §12.5 | Exchanges the effects of two neighbouring slots; the pad settings stay with the slot. | same | `docs/ui/ui-map.md` Perform row | Shift+Left/Right. |
| Effect library: 21 effects | §12.6 Perform Effects List | 21 effects including bit depth and overdrive. | same | `seq/include/o3os/seq/perform.h` | Ours has 21 as well (`kPerfFxInfo`; OS-PERFFX P1 added BITDEPTH and OVERDRV, `docs/specs/OS-PERFFX.md`). 15 of them act on the engine (SMP POS and SMP END since OS-PERF-SMP); PLAYBACK and the five LFO speeds are stored and shown dimmed (see the next row). |
| Effects that act | §12.6 | Every listed effect changes its parameter. | different | `seq/include/o3os/seq/perform.h` | The header says the three filters, VOLUME and PANNING (OS-PERFFX P2: VOLUME only cuts, PANNING adds to the pan in force), DELAY and REVERB (P3: an offset of the track's send, -100 closes it and +100 opens it whole), TUNE (P5: the sampler voices' pitch, -48..48 semitones), stutter, play mode and pattern length, SMP POS and SMP END (OS-PERF-SMP: the sampler voices' start and last frame, in percent of the part, at a note's start), PLAYBACK (OS-PERF-REVERSE: the next one-shot and beat-slice notes backward), and BITDEPTH and OVERDRV (OS-PERFFX P1, `docs/specs/OS-PERFFX.md`) act on the engine, 16 in all, as in the row above; the others (the five LFO speeds) are stored and dimmed ("no parity is claimed"). Needs a re-check on the current build. |
| Step repeater and pattern length | §12.6 | Stutter rates and a forced pattern length. | same | `seq/include/o3os/seq/perform.h` | STUTTER and PAT LEN labels follow the manual. |
| Pattern play mode with C1 to C20 | §12.7 Remixing | Twenty built-in shuffles of the pattern order. | same (verified parity of the orders, F1-F11) | `seq/include/o3os/seq/play_orders.h`, `seq/include/o3os/seq/perform.h` | PLAYMODE has FWD, BACK, PING, RND and the twenty custom orders C1..C20 as the values 4..23 with the labels C1..C20 (OS-PERFMODES M2-M4; the official firmware numbers them 3..22, F17: no file maps between the two; the labels used to read FWD, BACK, RND and CSM over the first four modes, OS-BUG-PERFMODE). |
| Remix: per-track pattern selection | §12.7, §12.9 | Per-track pattern swaps from a held track key, either queued for the end of the pattern or at once. | different | `docs/manual/perform.md`, `docs/specs/OS-PERFREMIX.md` | The official firmware holds a track and turns for a change at once, or an arrow and the release for the track's loop end (tracker-ce@7fd6c2c); here the gesture is different; the tracks to switch are the ones that are ON in the TRACKS layer (Shift + the top pads), not a key F1..F8. The REMIX page of OS-PERFREMIX was removed on 2026-11-08 (OS-NOREMIX): the per-track pattern switch is on the SLOTS page as TRK PAT / TRK NEXT / TRK SONG (E-PERF-07, new Community OS behaviour; QA-PRF-147, 225, 227). Not yet checked on the unit. |
| Mute tracks in Perform | §12.3 | Shift plus a track key. | same | `docs/manual/perform.md` | Shift layer of the pads: AFFECTED, MUTE, MUTE MODE, SOLO. |
| Play pattern, Shift+Play song | §12.2 | Play starts the pattern; with Shift the song. | different | `docs/manual/perform.md` | Play starts the transport; Shift+Play is not a song key here. |
| Template saved globally or per project | §3.7, §12.5 | Where the slot setup lives is a choice: shared by all projects or stored with each one. | different | `docs/formats/o3-project.md` (PERF, SCEN chunks) | Ours are the project's, in the PERF and SCEN chunks. |
| Perform ends on leaving | not in the manual | - | new | `docs/ui/ui-map.md` section 3 (12) | Leaving for most screens ends every effect. |
| Scenes, half and double speed, next pattern, reset | not in the manual | - | new | QA-PRF-001..003 | F1 to F3 scenes, F4 HALF, F5 DOUBLE, F6 LATCH, F7 NEXT PAT, F8 RESET. |
| Volume effect controls MIDI velocity | RN 1.8.0 | Perform's volume control reaches MIDI notes through their velocity. | different | `seq/include/o3os/seq/perform.h`, `docs/specs/OS-PERFFX.md` P2 | In ours VOLUME acts since OS-PERFFX P2, on the track's strip level (`perf_gain`: -100 is silent, 0 is unchanged, values above 0 are 0 dB), so the audio tracks follow it. Nothing reads it for MIDI velocity: a MIDI track's notes are not changed by it (new Community OS behaviour; no unit evidence either way for the official firmware). |

### 6.8 File

Our home: File (`docs/qa/catalogue/file.md`).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| New project | §3.2 | New Project starts a blank project. | different | QA-FIL-070, QA-FIL-071 | F3 asks: ALL, or KEEP INSTRUMENTS (new). |
| Open a project | §3.2 | A list; Open loads it, with a save question for edits. | same | QA-FIL-012 | F1 OPEN, F2 RESCAN. |
| Save | §3.2 | Overwrites the project. | same | QA-FIL-072 | F4. |
| Save as | §3.2 | Names a new project; Auto Name offered. | same | QA-FIL-061 | F5, prefilled from the title. |
| Delete a project | §3.3 Deleting a Project | Removes the project from the card after a question. | different | QA-FIL-032..034, QA-FIL-042 | Ours marks the folder deleted (a `.deleted` file) and hides it; nothing is erased. |
| Rename a project | not in §3 | - | new | QA-FIL-060 | Shift+F6 names the project's title only. Folder RENAME was dropped (OS-FILES-B). |
| Recent projects | not in the manual | - | new | QA-FIL-035, QA-FIL-036 | A RECENT group above the list. |
| Export song, pattern, stems | §8.8 | Export Song, Song Stems, Pattern, Patt Stems to `Export/`. | same | QA-FIL-021, QA-FIL-051, QA-FIL-055 | F7 EXPORT page. Stems: tracks, delay, reverb, mix (ours: T01.. DELAY REVERB MIX). |
| Export a stem set takes longer than playing | §8.8 | Stem export plays the song out track after track, so it takes longer than the music itself. | open | QA-FIL-052 | Progress overlay with FILE n OF m; timing not compared. |
| Export folders and names | §8.8 | New numbered folders per render. | different | QA-FIL-021, OS-WAVEXP | Ours: `Export/<Project>/<Project>_Song0001.wav`, `_P<n>_0001.wav`, stems with `_S0001_`. |
| Export instruments as WAV files | §8.8 (Export Samples) | Exports the project's instruments to the card as WAV files. | different | `docs/manual/sample_edit.md` | Ours saves one sample at a time (Sample edit Shift+F6 SAVE WAV); there is no whole-project export of instruments. |
| Import MOD / IT (beta) | RN 1.8.0 | Beta import of MOD and IT, and export as IT. | different | QA-FIL-005..010 | Ours: MOD, S3M, MED import with a report; no `.it` export. |
| Export a project in the OG format | not in the manual | - | new | QA-FIL-082..094 | EXPERIMENTAL row. |
| Export a block as SMF | not in the manual | - | new | QA-MOD-092, QA-MOD-093 | SMF Export app. |

### 6.9 Config

Our home: Config (`docs/qa/catalogue/config.md`). The manual's Config (§3.7) has six areas: Project Settings, General, MIDI, Metronome, Firmware, plus Manual and Credits (removed in 1.8.0). Ours has six pages: MIDI, DISPLAY, SD, INFO, USB, PATTERN. Options of the manual's Config that belong to another screen are mapped there, once: pattern divider, pads scale, pattern layout and top info, recording options, metronome and numbering (Pattern, 3.1 and 3.3); the place where the Perform template is stored (Perform, 6.7); radio region and line-in channel (Record, 6.5); the licences page, the version display, the firmware menu and the reset of the configuration (Unit, 2).

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| Anti-aliasing | §3.7 | Smoother resampling when on, a rougher vintage grain when off. | missing | `model/include/o3os/model/params.h` | Stored, no row. |
| Limiter mode | §3.7 | Subtle to extreme. | missing | `model/include/o3os/model/params.h` | Stored, no row. |
| Sends mode | §3.7 | Pre-fader or post-fader sends. | missing | `model/include/o3os/model/params.h` | Stored, no row. |
| Extra headroom | §3.7, RN 1.7.0 | Reserves output headroom. | missing | - | Not found in the model. |
| Display brightness | §3.7, §13.5 User Interface Style | Three levels. | same | `docs/manual/config.md` | DISPLAY page. |
| Pad brightness | §3.7, §13.5 | Three levels. | same | `docs/manual/config.md` | PAD LIGHTS. |
| Display theme | §3.7, §13.5, RN 1.9.2 | Original or monochrome; 1.9.2 adds an anniversary theme. | different | `docs/manual/config.md` | Ours: Amiga, Black, White; Shift+Master cycles. |
| Display font | §3.7, §13.5 | Original or a new font. | missing | - | No row. |
| Pads preview | §3.7 | Pads highlight notes of the active track. | open | - | The meaning is unclear in the manual. |
| MIDI: clock in | §3.7, §11.1 | Internal, USB or MIDI jack. | same | `docs/manual/config.md` | CLOCK IN. |
| MIDI: clock out | §3.7, §11.1 | Off, USB, jack, or both. | different | `docs/manual/config.md` | CLOCK MODE Off, Master, Slave. |
| MIDI: transport in and out | §3.7, §11.1 | Where start and stop come from and go to. | missing | `docs/ui/ui-map.md` Config row | Planned (OS-MIDI2). |
| MIDI: notes in, channel; MIDI out | §3.7, §11.1 | Ports and channel for note input; the output port. | same | `docs/manual/config.md` | MIDI IN, IN CHANNEL, MIDI OUT, OUT CHANNEL. Our port lists have no Off value (DIN, USB, DIN + USB). |
| MIDI: CC in, CC in channel | §3.7, §11.7 | The port and channel for control changes. | different | `docs/manual/config.md` | Ours: MIDI IN covers notes and Learn; there is no separate CC port or channel row. |
| MIDI: middle C | §3.7, §11.1 | Names note 60 as C-3 to C-6. | same | `docs/manual/config.md` | MIDDLE C. |
| MIDI: clock sync correction | §3.7, §11.1, RN 1.7.0 | A latency correction for incoming clock. | open | `docs/specs/OS-005-audio-engine.md` O23 | The manual says -6 to +6 in one place and -50 to +50 in another. Ours: CLOCK SYNC -6 to +6 pulses, a pulse offset only. |
| SD card and card details | not in §3.7 | - | new | `docs/manual/config.md` | SD page. |
| USB audio and return | not in the manual | - | new | QA-CFG-001..014 | USB page. |
| Metronome output side and count-in bars | not in the manual | - | new | QA-CFG-016..025 | PATTERN page. |

### 6.10 MIDI features reached from several screens

| Feature | Official manual | What it does there | track3r-os | Our evidence | Notes |
|---|---|---|---|---|---|
| MIDI clock follow | §11.2 MIDI Configuration, §11.3 | Follow an external clock over USB or the jack. | same | `docs/specs/OS-005-audio-engine.md` | A DLL clock input; lock time and jitter are in the spec. Unit check pending. |
| Fixed CC map for control | §11.7 MIDI CC In, §8.1 (track levels), §12.8 Controlling Performances with MIDI | Predefined CC numbers reach the track levels, the global mixer, the performance effect values and the per-track pattern choice. | same | QA-MIX-078, QA-MIX-079, QA-MIX-080, QA-PRF-206, QA-PRF-207, QA-PRF-208, unit pending | With CC MAP On (Config > MIDI, default Off) ours maps the same 32 numbers (CC41 to CC48, CC51 to CC62, CC71 to CC82) with the same value laws; a Learn binding wins over the map, and the Perform CCs act on the Perform slot page only. |
| Learn | not in the manual | - | new | `docs/manual/panel.md` | New. |
| Record from a MIDI keyboard | §5.7, §11.5 Handling MIDI Velocity | Notes (and velocity) from an external controller are recorded. | same | QA-CFG-030 | Velocity needs REC VELOCITY External. |
| MIDI latency offset on output | §11.4 | Offset on the MIDI instrument. | same | QA-INS-080 | OFFSET row. |
| Twelve tracks, four MIDI | RN 1.9.0 | The pattern grows from 8 to 12 tracks. | open | `docs/design/product-ux.md` section 2 | Ours has 12 tracks; whether tracks 9 to 12 are MIDI-only was not checked. Same question as "How many tracks" in 3.1 (8 tracks in the 1.7.0 manual, 12 since 1.9.0). |

## 7. Out of scope, follow-ups and the gap list

### 7.1 Official-manual chapters and sections not mapped

| Section | Reason |
|---|---|
| Cover, contents, §1.1 How to Use This Notebook | About the book itself. |
| §1.2 Hardware Overview (photographs, connectors, dimensions) | The hardware is the same unit; see `docs/manual/panel.md`. |
| §1.8 Example Workflow, §2 Architecture overview | A narrative, not features; its facts (8 tracks, 48 instruments, 255 patterns, 128 steps) are in the rows above. |
| §2.1 Glossary of Terms | Terminology; ours is `docs/manual/glossary.md`. |
| §2.5 Page Map | A diagram of pages; mapped through the screen rows. |
| §2.4 Quick Start slideshow on the card, Credits | Removed by the vendor in 1.8.0 (Manual and Credits menus). |
| §13.1 Device Registration | A vendor account step; not part of a firmware. |
| §13.8 SD card advice and backups | Advice, not a feature. |
| §13.10 Official Specifications, §13.11 Supplied Accessories | Product data. |
| Index | Navigation aid. |
| Tracker Mini and Tracker+ notes inside the text | Other products. |
| RN 1.7.1 | Five fixes (freezes, skipped envelopes, import playhead, header timer, tap tempo). No features. |
| RN 1.9.1 | Two fixes (line-in crackle, misplaced OFF messages). No features. |
| Other fixes in RN 1.8.0, 1.9.0 and 1.9.2 | Fixes do not map to rows; they matter only as evidence of the older bugs. |

### 7.2 Release notes after 1.7: where each item is mapped

| Version | Feature items | Rows |
|---|---|---|
| 1.8.0 | Better snap to zero; recording to about 133 s; Shift+wheel large steps; a third Pattern page with names; beta Mod import and IT export; a Licenses page; removal of the Manual and Credits menus | Wave 6.2; Sample load 6.4; Pattern 3.2; Pattern 3.1; File 6.8; Global 2; Config 6.9 |
| 1.9.0 | 12 MIDI tracks; the destination in the FX description; Instrument Synthesizer renamed Instrument MIDI Mapping | Pattern 3.1; MIDI 6.10; Pattern 3.3; Instrument 6.1 |
| 1.9.2 | Focus Loop; a 10th-anniversary theme | Pattern 3.3; Config 6.9 |

### 7.3 Follow-ups for the orchestrator

- **F1.** The official downloads page offers a "Tracker manual 1.9.2". This map follows the spec (the 1.7.0 edition plus release notes after it) and did not read it, and it is not to be read: the maintainer has ruled that the official manual is not consulted again (relayed by the orchestrator, 2026-10-18, OS-CLEANROOM). The rows marked `open` that depend on later changes (Perform effects, pattern names, the track count, Instrument MIDI Mapping, Focus Loop) stay open research questions until a unit observation or a decision settles them.
- **F2.** Rows marked `open` need either a unit observation (`verified parity`) or a decision. The ones with the most effect on a chapter: the metronome rule, the Perform effects that act, the MIDI instrument numbering (49 to 64 against 51 to 66), the track count (8 in the 1.7.0 manual, 12 since 1.9.0), the FX Bit depth range and the fill scales.
- **F3.** No QA row exists for most of the "same" FX in section 4; the FX chapter should say "Not checked against the original firmware" for them (OS-MANUAL2 section 5), or QA rows should be added.

### 7.4 Counts

Mapped rows (sections 2 to 6): **308**. Counts per section:

| Section | same | different | new | missing | open | Rows |
|---|---|---|---|---|---|---|
| 2. Unit, transport and global keys | 9 | 13 | 2 | 5 | 3 | 32 |
| 3.1 Pattern, length and the grid | 7 | 5 | 2 | 6 | 1 | 21 |
| 3.2 Moving, selecting and viewing | 6 | 5 | 1 | 0 | 1 | 13 |
| 3.3 Entering and editing steps | 11 | 9 | 2 | 5 | 6 | 33 |
| 4. Step FX | 32 | 3 | 2 | 0 | 2 | 39 |
| 5. Song | 5 | 4 | 3 | 3 | 2 | 17 |
| 6.1 Instrument (Params) | 14 | 3 | 6 | 1 | 3 | 27 |
| 6.2 Wave and Chop (Sample playback) | 10 | 3 | 1 | 0 | 1 | 15 |
| 6.3 Sample edit | 6 | 3 | 2 | 7 | 0 | 18 |
| 6.4 Sample load and the instrument pool | 1 | 6 | 0 | 3 | 2 | 12 |
| 6.5 Record (Sample recorder) | 5 | 1 | 3 | 0 | 0 | 9 |
| 6.6 Mixer (Master) | 2 | 6 | 2 | 4 | 2 | 16 |
| 6.7 Perform | 7 | 5 | 2 | 2 | 1 | 17 |
| 6.8 File | 4 | 5 | 4 | 0 | 1 | 14 |
| 6.9 Config | 5 | 3 | 3 | 6 | 2 | 19 |
| 6.10 MIDI features reached from several screens | 3 | 0 | 1 | 1 | 1 | 6 |
| **Total** | 127 | 74 | 36 | 43 | 28 | 308 |

Of the 127 "same" rows, 57 cite at least one QA row. The others rest on a spec or a chapter and must not be worded "Works as the official manual describes".

### 7.5 The "missing" list for the maintainer

Each row is something the official firmware has and track3r-os does not. This is a gap list for the maintainer; none of it is a backlog row by itself. Some gaps are decisions already taken (the offline sample effects, OS-SEDFX; the track arm, OS-LIVEREC); the Where it stands cell says so.

| Area | Feature | Official manual | Where it stands |
|---|---|---|---|
| Unit, transport and global keys | Master volume from any screen | §1.4 First Steps with Factory Projects | No held-Master gesture (`docs/ui/ui-map.md` section 2). The master level is the MST bus of the Mixer. The range was not compared. |
| Unit, transport and global keys | Peek at the Song page | §1.9, §9.3 Song Mode Interface | Song is a tap. The Navigator opens with the Master+Song or Song+Pattern chord. |
| Unit, transport and global keys | Built-in NES games | §13.7 Games | No screen or action for games. |
| Unit, transport and global keys | Screenshot | §13.6 Screen Capturing | OS-SCREENSHOT is approved for after the alpha (BACKLOG); Shift+Power is reserved (QA-GLB-110). |
| Unit, transport and global keys | Firmware update from the Config menu | §13.2 Generic Firmware Update | Ours has no Config firmware page. Updates go through the hidden reset button or the USB console (`o3console`). |
| Pattern, length and the grid | Duplicate, expand and shrink a pattern | §4.3, second options page | Specified (`pattern.duplicate`, `.expand`, `.shrink`) but no action or key exists. |
| Pattern, length and the grid | Copy and paste a whole pattern | §4.4 Copying and Pasting a Pattern, §5.4 | Not bound. Song Shift+Insert makes a copy in a new pattern slot (QA-SNG-003), which covers part of the use. |
| Pattern, length and the grid | Visual step dividers, 1 to 16 | §3.7, §4.6 Pattern Mode Page Layout Options | No Config row. |
| Pattern, length and the grid | Pattern top bar content | §4.6 | No Config row. |
| Pattern, length and the grid | Horizontal arrangement | §4.6 | Specified as a page 2 and later left out: the Pattern button has one page (`docs/ui/ui-map.md`). |
| Pattern, length and the grid | Vertical revolver arrangement | RN 1.7.0, §3.7 | Reserved value, refused. |
| Entering and editing steps | Pads: scale, root note | §3.7, §5.8 | Set from the held Note panel, per project, not on Config. The row offset is a Config row since OS-PADOFFSET (QA-CFG-035). |
| Entering and editing steps | Arm or disarm tracks for recording | §1.9, §5.7 | No track arm. Live notes go to the cursor's track (`docs/architecture/os-tree.md`). |
| Entering and editing steps | Fill: constant, from-to, random values | §5.6 | "Not in this slice". Ours uses the current note, instrument or volume. |
| Entering and editing steps | Fill an FX column | §5.6 | Fill writes note, instrument and volume only. |
| Entering and editing steps | Focus Loop | RN 1.9.2 | Planned in the ui-map. Shift+Play plays from the cursor's row today, also with a block selected, until the loop exists (QA-PAT-233, `docs/ui/ui-map.md` Pattern row). |
| Song | Tracks shown as clips; dark, dimmed, white | §9.3 | Ours lists pattern numbers and names with each slot's start bar and length in bars; TIMELINE draws the blocks. |
| Song | Copy and paste track clips | §9.5 | Ours: Copy and Shift+Copy copy slots (a range of pattern numbers), not tracks inside patterns. |
| Song | Preview one track clip | §9.7 Link between Song and Performance Mode | No clip view. |
| Instrument | MIDI synthesizer mode (Instrument MIDI Mapping) | §11.9 MIDI Synthesizer, RN 1.7.0 (page moved under More), RN 1.9.0 (renamed) | No fixed map or page. MIDI Learn (FX1+FX2) maps controls instead. The preset mapping for the maker's other instrument (Play) is part of the same feature. |
| Sample edit | Overdrive (offline) | §8.10 | Out by the maintainer's decision of 2026-10-16: the editor keeps normalise and DC remove, and printing live DSP (filter, bitcrush, delay, plate) is not built (BACKLOG, `docs/specs/OS-SEDFX.md`). |
| Sample edit | Delay (offline) | §8.10 | As above. |
| Sample edit | Bitcrusher (offline) | §8.10 | As above. |
| Sample edit | Chorus and flanger (offline) | §8.10 | As above. |
| Sample edit | Equalizer (offline) | §8.10 | RN 1.7.0 refined the editor's EQ, compressor and limiter. |
| Sample edit | Limiter and compressor (offline) | §8.10, RN 1.7.0 | As above. |
| Sample edit | Time stretch, beat and note | §8.10 | As above. |
| Sample load and the instrument pool | Batch select several files | §1.9, §3.6 | One file at a time. |
| Sample load and the instrument pool | Import with start and end trim | §3.6 (Importing) | Trim after loading in Sample edit. |
| Sample load and the instrument pool | Low-quality import | §3.6 | No such key. |
| Mixer | Master saturation | §10.6 Saturation, Space & Bass Boost | Reserved stage, not listed. |
| Mixer | Master bass boost and space | §10.6 | Reserved. |
| Mixer | Master EQ, five bands | §10.7 EQ | Reserved. |
| Mixer | Limiter sidechain from a track or line in | §10.5 | Not present. |
| Perform | Pattern play mode with C1 to C20 | §12.7 Remixing | Play mode has FWD, BACK, PING, RND and the custom orders C1 to C20 (OS-PERFMODES M4). |
| Config | Anti-aliasing | §3.7 | Stored, no row. |
| Config | Limiter mode | §3.7 | Stored, no row. |
| Config | Sends mode | §3.7 | Stored, no row. |
| Config | Extra headroom | §3.7, RN 1.7.0 | Not found in the model. |
| Config | Display font | §3.7, §13.5 | No row. |
| Config | MIDI: transport in and out | §3.7, §11.1 | Planned (OS-MIDI2). |
| MIDI features reached from several screens | Fixed CC map for control | §11.7 MIDI CC In, §8.1 (track levels), §12.8 Controlling Performances with MIDI | Ours maps controls with Learn (FX1+FX2) instead; the official numbers (CC41 to CC48, CC51 to CC62, CC71 to CC82) are not mapped. |
