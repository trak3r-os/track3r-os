# OS-SHIFTROW: Shift+F rows that mean something, and arrows that turn pages

Maintainer question, 2026-10-14: why do many views show 0/4, 1/4... when Shift is pressed; study how the Shift+Fx keys of the views can be improved. A study followed, then the maintainer's answers: keep the quarter jumps where they help, give the other screens functions of their own, or else nothing; Shift+encoder in Pattern stays the pattern change. Then the maintainer approved the table below and to Left/Right changing pages. **New Community OS behaviour**. Level B: bindings, labels, three small actions. No format change. Goes before OS-INSTUI S1.

## 1. Rules

1. **Shift+F belongs to the screen, never to Global.** The global quarter jumps (`app/src/bindings.cpp:286-290`) go. Global binds Shift+F1..F8 to a null action (`ui.none`). The existing resolver order (innermost context first, then the no-modifier retry) plus Global's `ui.none` bindings give that: a Shift+F press the screen does not bind ends at Global's `ui.none`, so it never falls through to the plain key. No core change; the retry that serves Shift+Play stays as it is (`core/src/binding.cpp:60-70`).
2. **A Shift+F key with nothing worth a key is blank and does nothing.** No dim plain label, no fall-through.
3. **Shift+F8 = REDO wherever F8 = UNDO.** Where F8 is something else (PREVIEW, RESET, SOLO), Shift+F8 is not REDO.
4. **Where an object has a name**, Shift+F1..F5 = NAME COPY PASTE DELETE SAVE, in that order (Instrument, through OS-INSTUI).
5. **(Withdrawn for Config and Record by OS-CFG-ARROWS, maintainer 2026-10-29, new Community OS behaviour; for Instrument by OS-INSTUI-ARROWS: Left/Right edit the row, Shift + Left/Right in the coarse step, and the pages are the F-keys or the screen button again.)** On a paged screen with a single column, Left/Right = previous/next page, wrapping. The F-keys still go straight to a page. This applies to Config and Record here, and to Instrument in OS-INSTUI S1 (except a module with kits, where Left/Right select the kit).

## 2. The rows (· = blank, no effect)

| Screen | Shift+F1 … F8 | What changes |
|---|---|---|
| Pattern | NOTES PIANO TRACKS ISO · PATS VU REDO | OS-STEPPADOFF (2026-10-16): F5 was STEP, now blank and unbound (rule 2); the mode's code stays, see OS-STEPPAD |
| Song | 0/4 1/4 2/4 3/4 END · · REDO | The quarter jumps are bound in `kSong` (`cursor.jump_fraction 0..3`, `cursor.jump_end`), and the series is whole again. **DUP PAT moves to Shift+Insert** (`song.add_slot 1`; UI map rule: Insert adds here, Shift+Insert is the bigger add), labelled in the info line "SHIFT+INS DUP". |
| Instrument | NAME COPY PASTE DELETE SAVE <USED USED> REDO | OS-INSTUI S1 (not this slice) |
| Wave | MODE · · · · PRV SEL · REDO | MODE comes with OS-INSTUI S4; the other keys stop falling through |
| Chop | · · · · · PRV SEL · REDO | |
| Sample edit | AMG 8K AMG 16K Q8 DC REM · SAVE WAV PRV EDGE REDO | F4 and F5 blank: the duplicate 3/4 and END edge picks go (UI map §3.16); F4 became DC REM with OS-SEDFX |
| Sample load | 0/4 1/4 2/4 3/4 END · · · | Quarter jumps bound in its own context |
| File | 0/4 1/4 2/4 3/4 END · · · | Same. OS-FILES part B places them on the PROJECTS page. |
| Record | · · · · CH-10 CH+10 · · | RADIO page only: Shift+F5/F6 = tune −/+10 channels (`record.seek` ×10). The two keys are bound in the RADIO page's own context (`record.radio`, the screen's field level); on the SOURCES and RESAMPLE pages the row is blank (rule 2). No in-take seek. Plus Left/Right = page (SOURCES, RESAMPLE, RADIO). |
| Mixer | · · · · · · UNMUTE UNSOLO | **New** small actions `mixer.mute_none` / `mixer.solo_none`, a loop over the 16 track strips' mute and solo (a strip the project has no track for is skipped), one undo unit. They sit over F7 MUTE / F8 SOLO. **Deviations from the table as first written:** the labels are `UNMUTE` / `UNSOLO` (a key cap holds 8 characters), and UNMUTE means all tracks only: the SND A/B master mutes are not touched. |
| Perform | STORE 1 STORE 2 STORE 3 · · · · · | **New:** store a scene with a visible key. The existing ~1 s hold keeps working. With the global jumps gone, SCENE 1–3 are no longer shadowed. If `perform.key` already tells tap from hold, this is a binding with an argument; otherwise it is a small action. |
| Config | · · · · · · DEFAULTS · | **New** `config.page_default`: every row of the page back to its default. Device settings have no undo, so it takes **two presses within 2 s** (the strip says "SHIFT+F7 AGAIN: RESET PAGE"), like APPLY. Plus Left/Right = page (MIDI, DISPLAY, SD, INFO, USB, PATTERN). |
| Modules | · · · · · · · · | |
| Navigator | no jumps | |

**OS-NAMES:** three blanks of this table are now `NAME` (docs/specs/OS-NAMES.md): Mixer Shift+F1 (the track under the cursor; a bus says `TRACKS ONLY`), Song Shift+F6 (the pattern of the slot) and File Shift+F6 (the open project's title). The other keys of those rows are as above.

## 3. Tests and acceptance

- **`tests/shift_labels_test.cpp`:**
  - `ui.none` counts as nothing; it replaces the special `has_no_cursor` case.
  - A new check that no Shift+F press resolves to its plain key.
  - Every screen's Shift row label is equal to its binding.
- **Unit tests** for `mixer.mute_none`, `mixer.solo_none`, `config.page_default` (including the double press), the Perform store, and the Record seek argument.
- **Left/Right page tests** on Config and Record: wrap at both ends, and the page is kept per screen.
- **Goldens:**
  - Regenerate `screens/shift_song_quarter` and `screens/navigator_open_shift`.
  - New `screens/shift_<screen>` snaps for Song, Wave, Chop, Sample edit, Sample load, File, Record, Mixer, Perform, Config and Modules, in the three themes.
- **QA rows:**
  - QA-SNG: the jumps, Shift+Insert DUP.
  - QA-SLD and QA-FIL: the jumps.
  - QA-REC: CH-10 / CH+10 on RADIO (blank on the other pages), page with the arrows.
  - QA-MIX: UNMUTE ALL, UNSOLO ALL.
  - QA-PRF: STORE n; SCENE 1 not shadowed.
  - QA-CFG: DEFAULTS double press, page with the arrows.
  - QA-GLB: a blank Shift+F does nothing.
- **Docs:**
  - UI map: the Shift column in §1, §2, rule 5 above in §5, and close §3.1 and §3.16.
  - A note in OS-UIFIX and OS-PERF2.
  - The generated key reference.
