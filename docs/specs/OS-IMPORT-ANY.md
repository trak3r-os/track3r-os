# OS-IMPORT-ANY: import module files by their header, and 15-sample Soundtracker MODs

Level B (main loop, the File screen and the module importer; no format change, no audio path). Approved by the maintainer on 2026-10-30 ("many files have no extension... read the type from the header", then "vamos con 1,2"). New Community OS behaviour. Follows docs/specs/OS-FILES-IMPORT.md and the importers of OS-MODIMP (`docs/formats/mod.md`) and OS-008b (OctaMED).

## Facts (dev 558838f0)

- The format registry already identifies modules by their bytes: MOD by the signature at offset 1,080 (`M.K.`, `M!K!`, `N.T.`, `FLT4/8`, `xCHN`, `xxCH`), S3M by `SCRM` at 44, OctaMED by `MMD0`-`MMD3` (formats/src/builtin_formats.cpp:90-97; `FormatRegistry::sniff` and `identify`, formats/include/o3os/formats/format.h:223-228; `kSniffBytes` 1,084).
- Two places refuse a file by its name: the IMPORT list's filter `kModuleFilter` (app/include/o3os/app/dir_scan.h:62) and `ModuleImport::begin`, which picks the reader by extension (app/src/module_import.cpp:252-256). The TYPE column also comes from the extension (app/src/screens/file_screen.cpp:107-115).
- Amiga files often carry the type as a prefix, not a suffix: `mod.title`, `MED.title`. Other files have no type in the name at all.
- The MOD reader refuses 15-sample Soundtracker files, which have no signature (formats/mod/src/module.cpp:137).
- The IMPORT page's keys: F1 IMPORT / ENTER, F2 RESCAN, F3 UP, F4 SAVE, F5 SAVE AS, F6 PROJECTS, F7 the engine, F8 REPORT / LIST (file_screen.cpp:358-363).

## Decisions (orchestrator, 2026-10-30; defaults, to confirm by the maintainer)

| # | Question | Default |
|---|---|---|
| I1 | The toggle | F2 on the IMPORT page switches the list between MODULES (today's list plus the prefix names of I2) and ALL FILES (every file and folder); the label shows the mode a press goes to; each press rescans the folder, so the rescan stays one press away. The mode is not saved (MODULES at every visit) |
| I2 | Prefix names | `mod.*`, `s3m.*`, `med.*` and `mmd0.*`..`mmd3.*` (any case) count as module names in MODULES mode |
| I3 | Which reader | `ModuleImport::begin` reads the first 1,084 bytes (or the whole file when shorter) and asks the registry: a known extension or prefix must agree with the bytes (as `identify` does); a name that says nothing is decided by `sniff`. No match: the import refuses with `NOT A MODULE THIS OS READS` and nothing changes |
| I4 | When the header is read | Before the NEW question (save / discard the current project): a file that is not a module never asks it, and the document is untouched until the reader's first step, as today |
| I5 | TYPE column | By the name as today, plus the prefix names; `?` for a file whose name says nothing (no header read per row: a listing never reads file contents) |
| I6 | 15-sample Soundtracker | The MOD reader accepts a file with no signature when its structure holds: a 600-byte header (title, 15 sample records, length, restart, 128 orders); song length 1..128; every order below 64; sample volumes at most 64; sample names and the title of printable ASCII or NUL; the file at least as long as the header, the patterns (highest order + 1, 1,024 bytes each) and the samples, with a tolerance for a short last sample. It is tried only when the name says MOD (extension or prefix) or the user imports a file of no known type; `sniff` never claims it, so no listing guesses it |
| I7 | ST15 playback | The 15-sample file reads as a 4-channel M.K. module with samples 16-31 empty; effects as ProTracker. Known difference: Ultimate Soundtracker's own effect numbering (1 = arpeggio, 2 = pitch bend) is not detected; recorded as an open research question if a test file shows it |

## Plan (one branch `feat/OS-IMPORT-ANY`, two commits)

1. **ALL FILES and the header** (I1-I5): the filter, the toggle, the reader choice, the refusal; host tests (a module under a wrong or empty name imports; a WAV renamed `.mod` refuses; a `mod.title` lists and imports; nothing changes on a refusal); a sim scenario for the toggle; QA rows; the manual section in `docs/manual/file.md` (or the chapter that holds IMPORT), with a short section on converting other formats (XM, IT, Oktalyzer, MTM, 669...) with OpenMPT on a computer: open, change the module type to S3M (or MOD for 4 channels), save, import (the maintainer parked native XM on 2026-10-30; check the steps against OpenMPT's own documentation).
2. **15-sample Soundtracker** (I6, I7): the reader path, host tests with a synthetic 15-sample file built in the test (no third-party file in git) and the edges (length 0, an order of 64, a truncated sample, a 31-sample M.K. file unchanged), the `docs/formats/mod.md` note.

## Tests and Done when

Scoped host CI green, `remote_ci mk66` for the size, one firmware-reviewer; both commits merged with scenarios, goldens, QA rows and the manual section; the flash delta recorded (expected under 2 KB). Unit: import a module without an extension and a 15-sample file from the card (any card).

## Defaults taken in the implementation (to confirm by the maintainer)

- **I3, the registry:** `ModuleImport::decide` applies the registry's own sniff rules (`formats::mod::sniff_kind` for MOD and S3M, the `MMD0`-`MMD3` test and size floor of `sniff_med`) directly, not through a `FormatRegistry` instance: a registry with its readers is `BuiltinFormats`, over 100 KB, and `ModuleImport` already owns the two readers that matter. One difference from `identify`: a name that says MOD over S3M bytes, or S3M over MOD bytes, goes on (the two descriptors share one reader, and such files imported before); a name that says OctaMED over MOD or S3M bytes, or the other way round, is refused.
- **I3, a file the card will not give** (not found, read error) is not "not a module": the import starts and fails with its own words (`FILE NOT FOUND`, `READ ERROR`), as before. `ModuleImport::begin` answers `Start::NotModule` for a file the header refuses; the platform and the simulator map it, like `BadPath`, to a refused start.
- **I3, behaviour change:** a `.mod` of noise used to start the import and fail after the reader reset the project (an empty document); it is now refused at once with the document untouched. QA-FIL-010 says so.
- **I4:** the header is read by a new `ProjectSink::module_file(path)` (the platform's and the simulator's sinks run `ModuleImport::probe` over the card; a sink that cannot tell answers true), called by the IMPORT action before the unsaved-changes question. It reads at most 1,084 bytes (one open, a read, a close) in the action handler, like `import_module`'s own open.
- **I1:** the label shows the mode a press goes to (`ALL` in MODULES mode, `MODULES` in the other; `ALL FILES` is one character too wide for a key cell); the info line adds `, all files` in ALL FILES mode; a folder entered keeps the mode, F6 (leaving the page) resets it. F2 on the report page (F8) is still a rescan of the same mode.
- **I2:** `module_name_kind`: the extension wins over a prefix (`mod.song.s3m` is S3M); a prefix needs at least one character after the dot.
- **OpenMPT section of the manual:** the steps follow OpenMPT's own wiki ("Saving and exporting": saving keeps the current type, Song Properties holds the type; "Module formats": MOD has 64 rows, no instruments, 8-bit samples).
- **I6 (commit 2):** "every order below 64" is applied to the orders the song plays (the song length's worth); the entries behind it may be junk, as in a 31-sample file, and only count toward the pattern total when the whole table fits the file. The printable rule is bytes 0x20..0x7E or NUL for the title and the 15 sample names. The structural test is `formats::mod::is_soundtracker15`, used by `Module::open` (any caller of the reader) and by `ModuleImport::decide` (name says MOD or nothing); a name that says S3M or OctaMED never tries it. The model gets 15 instruments (`sample_count` 15), not 31 with 16 empty; the restart byte is ignored (a Soundtracker file keeps another value there, usually 120).
- **I7, open research questions, not handled:** Ultimate Soundtracker's effect numbering (1 arpeggio, 2 pitch bend) and the unit of the sample loop start (bytes in the oldest files, words in ProTracker's): both need a real file to decide, and none is in git.

## Unit check (2026-10-30, `build-images/v1.0.1-importany.1/REPORT.md`, not in git)

PASS: a module with no extension imports from ALL FILES (TYPE `?`); `mod.*`, `MOD.*`, `s3m.*` and `med.*` names list and import; three real 15-sample files import and play as 4 channels with 0 underruns; a WAV or text file under a module name or no extension is refused with no question and the Song and Pattern screens unchanged; cuts at 700, 1,000 and 1,083 bytes get the same refusal.

Found:
- **Loop start in bytes:** in two of the three real 15-sample files the loop only fits the sample when its start is read in bytes. Fix queued as OS-BUG-ST15LOOP.
- **Effect numbering:** one file uses effects 1 and 2 about 18 times; still an open research question (it needs listening).
- **A tagged MOD of exactly 1,084 bytes** (no patterns) passes the header, asks the NEW question, then fails `IMPORT FAILED: TRUNCATED` and leaves an empty project. Fix queued as OS-BUG-IMPTRUNC: refuse before the question when the file cannot hold one pattern.
- **I1, amended (default, to confirm by the maintainer):** another screen and back keeps ALL FILES and the cursor, because the File screen keeps its state; leaving the IMPORT page with F6 resets it to MODULES; the mode is never saved. The unit showed this and it matches the rest of the screen, so the spec follows the build.

