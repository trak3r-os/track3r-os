# OS-NAMES: name tracks, patterns and the project

BACKLOG #7, approved by the maintainer on 2026-10-14. Written on 2026-10-15. Line refs come from version 0.4.20.

This is **new Community OS behaviour**: Level B, size S. **No format change**: every name already has a stored field and an undoable setter (§1). The on-screen keyboard and its overlay come from OS-INSTUI S7.

## 1. What exists

| Name | Model field (limit) | Stored | Editor setter (undoable) | Drawn today |
|---|---|---|---|---|
| Track | `TrackSlot::name` (`model/project.h:53`, `kTrackNameMax` 20) | TRAK (`native_writer.cpp:24, :240`; reader `:620`) | `set_track_name` (`editor.h:96`) | Mixer track rows "T1 NAME" (`mixer_screen.cpp:440-446`) |
| Pattern | pattern `name` (`kPatternNameMax` 30, the OG PAMD limit) | PATT (`native_writer.cpp:113, :297`; reader `:276`) | `set_pattern_name` (`editor.h:106`) | nowhere |
| Project | `Project::name_` (`project.h:287`, `kProjectNameMax` 32) | PHDR (`native_writer.cpp:19, :202`; reader `:583`) | `set_name` (`editor.h:86-87`) | nowhere |

**OG and MOD imports already fill these fields where the source has the name** (the song name from a module header, OS-FILES-IMPORT). This slice adds only the keys and the drawing.

## 2. Gestures (UI map rule 4: where an object has a name, its Shift key is `NAME`)

`TextEntryView::Target` (`view_state.h:65`) grows by `Track`, `Pattern` and `Project`. Each opens the S7 keyboard the same way (`instrument_actions.cpp:180-181`):
- `Filter::Any`;
- prefilled with the current name;
- the limit is the field's;
- F1 accepts and F8 cancels (OS-UXF-KEYS item 5; F7 types the key in focus).

On accept, the matching setter runs as **one undo unit**. An empty accept clears the name.

| Name | Where | Key | Acts on |
|---|---|---|---|
| Track | **Mixer** | **Shift+F1 `NAME`** (the Mixer Shift row's F1 is blank today: OS-SHIFTROW) | the track under the Mixer cursor; on a row or strip that is not a track (bus, master), the strip says `TRACKS ONLY` |
| Pattern | **Song** | **Shift+F6 `NAME`** (blank today) | the pattern in the order slot under the cursor |
| Project | **File, PROJECTS page** | **Shift+F6 `NAME`** (blank today; Shift+F7 is PURGE from OS-FILES-B) | the open project's title, not its folder |

- **Pattern has no NAME key.** Its Shift row is the pad modes, and a pattern is named in Song, where the patterns are listed.
- **Project title vs folder.** The folder is what SAVE AS and RENAME make on the card. The title is the name stored in the project.
  - When the project has no folder yet (new or imported), **SAVE AS is prefilled with the title**, reduced to the FileName characters (`text_entry.cpp:48`).
  - The title never renames a folder, and a folder rename never changes the title.
- The text-entry overlay's heading (`text_entry_overlay.cpp:29`) reads `TRACK NAME`, `PATTERN NAME` or `PROJECT NAME`.

## 3. Display

All drawing follows the theme tokens. A name that does not fit is clipped, never wrapped. With no name set, today's text stays.

| Where | What |
|---|---|
| Pattern track headers | the track name in place of the bare number, clipped to the column (e.g. `03 BASS`); the number stays first; MUTE/SOLO tags unchanged |
| Pattern info line | the pattern number, then its name (`P05 VERSE`) |
| Song order rows | each slot's pattern number, then its name, clipped |
| Mixer | unchanged: it already draws track names |
| File header | the open project's title, or the folder name when the title is empty |

## 4. Acceptance

**Host tests:**
- For each target:
  - the key opens the entry with the current name;
  - F8 sets the name, and it is one undo unit;
  - F7 changes nothing;
  - an empty accept clears the name;
  - the length limit holds.
- Mixer on a bus row: `TRACKS ONLY`.
- SAVE AS is prefilled from the title when there is no folder.
- `shift_labels_test`: the new labels equal their bindings.

**Round trip:** a project with all three kinds of names survives save and reopen. This is already covered by `native_format_test`; add the case if it is missing.

**Gallery and goldens:**
- Pattern with named tracks and a named pattern;
- Song order with names;
- the File header;
- each of the three text-entry headings;
- in the three themes.

**QA rows:**
- QA-MIX: track NAME;
- QA-SNG: pattern NAME;
- QA-FIL: project NAME and the SAVE AS prefill;
- QA-PAT: the names drawn.

**HIL `qa_names.json`:**
1. Name track 1 in Mixer, then check the Pattern header shot.
2. Name a pattern in Song, then check the order row shot.
3. Undo restores both.

**Docs:**
- UI map §1, the Shift columns of Mixer, Song and File.
- The OS-SHIFTROW table gets a line saying these three blanks are now NAME.
- BACKLOG #7 goes to DONE.

## Built

Built from version 0.4.20. No format change: the three names are the stored fields of §1 and their `Editor` setters; one `text.accept` is one undo unit.

**Keys.** `mixer.name` = Mixer Shift+F1; `song.name` = Song Shift+F6; `file.name` = File Shift+F6 (`bindings.cpp`, labels `kMixerShiftKeys`, `kSongShiftKeys`, new `kFileShiftKeys`). `TextEntryView::Target` grew `Track`, `Pattern`, `Project` and `TextEntryView::subject` (the track slot or the pattern id, fixed when the keyboard opens). `name_open` / `name_accept` (`save_actions.cpp`, beside `text_accept`) open the keyboard (`Filter::Any`, the field's limit: 20, 30, 32) and run `set_track_name` / `set_pattern_name` / `set_name` on F8; the strip says `TRACK nn NAMED`, `PATTERN nn NAMED`, `PROJECT NAMED` (`... NAME CLEARED` for an empty name). Headings `TRACK NAME`, `PATTERN NAME`, `PROJECT NAME`.

**What the code showed that the spec did not say (nothing needed a stop).**
- Pattern track headers already drew `NN name` (`PatternView::draw_headers`), and the Song order rows (and the strip's PAT field) already drew the pattern's name cut to 12 characters. Neither changed. New drawing: the Pattern **info line** (`P05 verse` after `track n`, cut to 14, nothing for an unnamed pattern: `PatternView::Status::pattern_name`) and the **File header** (`PROJECT <title>` at the right, before the page's name, cut to the room; the title, or the last component of the loaded folder when the title is empty; `Model::project_folder`, set by `App::poll_files` from the load sink).
- Mixer NAME names the track of the MIXER page's column, the ROUTING page's row and the STEMS page's column; a bus, the master, LINE IN, the returns, OUT MODE and the MASTER FX page say `TRACKS ONLY`.
- File Shift+F6 works on all three File pages (PROJECTS, IMPORT, EXPORT): the title belongs to the open project, not to the listing, and one label row serves them all. OS-FILES part B may restrict it when it adds Shift+F7 / F8.
- A slot that names a pattern the project does not have says `NO SUCH PATTERN YET`; no document says `NO PROJECT TO NAME`.
- SAVE AS (`file_save_as`): with no folder (`ProjectSink::loaded()` false or `folder()` empty) the entry opens with the title through `text_file_name` (letters, digits, space and `-`, the first a letter or digit, no space at the end, at most 31); a title that leaves nothing or `valid_name` refuses (`CON`) falls back to the automatic `Track nn`. The `save_app_test` rig clears its fixture title so its automatic-name cases stand.

**Tests.** `names_test.cpp` (13 cases): each target (current name, F8 one undo unit, undo and redo, F7 nothing, same name records nothing, empty clears, the field's limit), any key accepted, ROUTING and STEMS, bus rows refused, no document, `text_file_name`, SAVE AS prefill and its fallbacks, the Pattern info line and the File header cut (display-list equality). `builtin_bindings_test` lists the three new Shift bindings; `shift_labels_test` resolves the new labels unchanged. The round trip of the three names is `native_format_test` ("a small project round-trips ... every field").

**Goldens.** `update_goldens`: 9 changed, all the Shift row now reading NAME (`qa_file/qa_fil_001`, `qa_mixer/qa_mix_001`, `qa_song/qa_sng_001`, `screens/shift_file`, `shift_mixer`, `shift_mixer_black`, `shift_mixer_white`, `shift_song`, `shift_song_quarter`), 24 new (the three keyboards, the names drawn, TRACKS ONLY, the Pattern screen, each in the three themes where it matters). No Pattern snap changed: the demo's patterns have no name.

**Size** (`ci.sh mk66`, against the previous build): image 707,880 -> 709,852 B (+1,972 B flash; tone image +2,020 B), `.data` and `.bss` unchanged.

**CI:** host 131/131, qemu and mk66 (with the tone image) exit 0, no warnings.

**For the unit** (`qa_names.json`): the keyboard on the real pads, the names on the real panel, SAVE AS on a card with a project that has a title and no folder.
