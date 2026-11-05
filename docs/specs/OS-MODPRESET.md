# OS-MODPRESET — User presets for module instruments and effects (`.o3mp`)

Status: specified 2026-11-05, decisions taken the same day (every recommended default), not implemented · **Level C** (a new persistent file format and a new written path on the card) for P1 and P3, Level B for P2, P4 and P5 · Backlog row OS-MODPRESET, approved by the maintainer 2026-11-05 · Facts from `dev` at c1dc3125 · Order: after OS-INSFX-ABI2 A8 and A9 (the shared `app::ParamSet` model and the F5 PRESET list this item extends) · Every behaviour here is **new Community OS behaviour**: the official firmware's only presets are the Perform presets (F1).

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a, Q4 a, Q5 a, Q6 a, Q7 a (every recommended default).**

Only what the maintainer must decide (a card layout users see, what a file means, scope). The byte layout, the reader's rules and the UI's details are decided in this spec (section 6).

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | Where presets live on the card | a) `modules/presets/<module>/<name>.o3mp` · b) `presets/<module>/` at the root · c) beside the `.pti` library in `instruments/` | a | Beside the modules they belong to; one folder a module, so the list needs no filtering and a user can copy a module's presets as one folder; the `.pti` library is the OG's format and folder |
| Q2 | What a preset holds | a) the module's own parameters only (the values on its parameter pages) · b) also the instrument's common rows (volume, pan, sends, envelopes, filter) | a | One model for instruments and effects (an insert has no common rows); loading a sound does not move the mix |
| Q3 | Saving under a name that is taken | a) the next free `<name> 2` .. `<name> 99`, said on the toast (the `.pti` library's rule) · b) refused, type another · c) replaced after a confirm | a | Nothing is ever overwritten (the save guard's rule for user files), and no extra screen |
| Q4 | Deleting or renaming a preset on the unit | a) not in this item (on a computer, or the card's USB disk) · b) add it | a | The save guard never removes or renames a user's file (`hal/include/o3os/hal/save_guard.h:51-55`); adding it is its own Level C change |
| Q5 | How a file's values match the module's parameters | a) by the parameter's name · b) by index | a | A module that adds or reorders parameters in a later version (TB-303, FM and Chip did, OS-SNDMOD) still loads old presets; an unknown name is skipped and counted |
| Q6 | Factory presets for the built-in TB-303, FM, Chip and SID | a) `.o3mp` packs in the release zip (0 flash) · b) tables in the descriptors in flash (ABI2 presets, about 1 KB each) | a | Flash is at 94 to 97 %; the pack is the same file a user saves |
| Q7 | Naming a preset when saving | a) a text entry with a suggested name (the instrument's name, or the effect's name) · b) automatic `<module> 1`, `2`, ... with no entry | a | The name is the only thing that tells presets apart in the list |

## 1. Goal

Let a user save the parameter set of a module instance (an instrument such as TB-303, FM, Chip or SID, or an insert effect) as a named file on the card, see the card's presets for that module in the F5 PRESET list next to the module's factory presets (OS-INSFX-ABI2), and load one with a key. One parameter-set model (`app::ParamSet`, ABI2 A8) serves factory and card presets; one new small file format (`.o3mp`) with a version, a backward-compatible reader and a round-trip test; the built-in instruments get factory presets as a pack of the same files in the release zip.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | The official firmware's only presets are the Perform presets (stored globally or per project); its instrument library is the `.pti` file | `tracker-ce@7fd6c2c:core/interface/configEditor/configMenuDefs.h:271-272`; this OS writes `.pti` to `instruments/` (`hal/include/o3os/hal/save_guard.h:19-20`) | verified (tracker-ce source); no parity claim |
| F2 | A module instrument's parameters are a float blob in the instrument (`Instrument::params`, 256 B, `param_len`), written through `write_module_param` with undo; an insert's are the `INSX` record's floats (positional, a short blob takes the defaults for the rest), written without undo (OS-INSFX D9) | `model/include/o3os/model/instrument.h:113-128`; `app/src/param_actions.cpp:303-345`; `audio/include/o3os/audio/insert_host.h:103-104` | code fact |
| F3 | A module has at most 32 parameters, names of at most 15 characters, a stem of at most 31 | `module/sdk/o3module.h:141-143`; `module/include/o3os/module/loader.h:45` | code fact |
| F4 | The save guard allowlists every path the OS writes (Temp, Target, Backup), at most four components and a 96-byte path; a Target appears only by the rename of a Temp; a user file is never removed or renamed; the `.pti` library and the exports are NEVER replaced and a taken name gets the next free one | `hal/include/o3os/hal/save_guard.h:8-56`, `:93-96`; `app/include/o3os/app/save_service.h:23` | code fact |
| F5 | The atomic save writes a Temp and renames it; `NewOnly` refuses if the target appeared, so nothing is replaced | `app/include/o3os/app/atomic_save.h:14-35` | code fact |
| F6 | The device settings file is the model for a small file: an IFF container (`FORM` + type, chunks, `CRC `), read and written whole through a 1 KiB buffer, no heap; a newer major refused, a newer minor read as far as known, an unknown entry skipped and counted, unknown chunks ignored | `formats/include/o3os/formats/settings.h:12-40`; `docs/formats/o3-project.md` sections 1, 2 and 10 | code fact |
| F7 | The module picker lists a card folder synchronously on the main loop (`opendir` / `readdir`, at most 256 entries) | `app/src/module_ui.cpp:610-634` | code fact |
| F8 | Shift + F5 on the Instrument screen saves a sample instrument as `.pti` and says `SAMPLE INSTRUMENTS ONLY` for a module instrument; a module instrument's pages are F1..F4 | `app/src/save_actions.cpp:211-225`; `app/src/instrument_actions.cpp:54-63` | code fact |
| F9 | The text entry has a `FileName` filter (letters, digits, space, `-`) and a `Target` per use | `app/include/o3os/app/text_entry.h:40-46`; `app/include/o3os/app/view_state.h:83` | code fact |
| F10 | The `App` object lives in SDRAM: its members cost no SRAM | `platform/mk66/src/app_main.cpp:164` | code fact |
| F11 | The release starter zip is built by `tools/release/make_starter.py` from `starter_manifest.json` (apps, instruments, effects lists); the built-in instruments are in the firmware and not packed | `tools/release/README.md:7-13`; `tools/release/starter_manifest.json:4` | code fact |

## 3. The file format (`O3MP` 1.0, P1)

`docs/formats/o3-preset.md` (new) holds the normative text; the conventions are the project's (`docs/formats/o3-project.md` section 1: big-endian, `str` = a length byte and the bytes, chunk = id + u32 length + payload + a zero pad byte when odd, a chunk's first u16 its version, major in the high byte).

```
"FORM" u32 length "O3MP"
  "MPST" u32 n   payload (n bytes) [pad]
  ...unknown chunks (ignored, not kept)
  "CRC " u32 6   u16 0x0100   u32 CRC-32/ISO-HDLC of every byte before this chunk's header
```

`MPST` 1.0 payload, in order:

| Field | Type | Rule |
|---|---|---|
| version | u16 | `0x0100` |
| module | str | the module's stem, 1..31 bytes, `module::is_stem` |
| kind | u8 | `O3M_KIND_INSTRUMENT` (1) or `O3M_KIND_EFFECT` (2) |
| module_version | u32 | the module's version as its header holds it; informative, never refused on |
| count | u8 | 0..32 |
| entries | count x (str name, f32 value) | name 1..15 bytes, printable ASCII; value finite |

A later minor appends fields after the last entry. Largest file: 714 bytes (`kMaxFileBytes` 1,024, as the settings file).

**Reader** (`formats::preset::read(const uint8_t*, size_t, Preset&, Report&)`, pure, no heap): errors `Truncated`, `BadMagic`, `BadCrc`, `NewerMajor` (the `MPST` major above 1), `Malformed` (a length that does not fit, a bad pad, no `MPST`, two of them, a missing or misplaced `CRC `, a bad stem, kind or name, a count over 32, a non-finite value: the writer never writes one, so a file that holds one is damaged, the project file's rule), `TooBig`. A newer minor: read as far as known, `newer_minor` set. Entries with a name already seen: the first wins, the rest counted in `duplicates`. **Writer** (`write(const Preset&, uint8_t*, size_t cap)`) emits exactly the 1.0 layout. `Preset` is the decoded file: module, kind, module_version, count, entries (about 680 bytes).

## 4. Design (card and UI)

- **Path (Q1 a).** `modules/presets/<module>/<name>.o3mp`, `<module>` the stem, `<name>` a `valid_name()`; the Temp `_o3save.tmp` in that folder. New save guard rows: `modules` (folder, make_dir), `modules/presets` (folder, make_dir), `modules/presets/<stem>` (folder, make_dir), `.../<name>.o3mp` (Target, NEVER replaced, no Backup form), the Temp. Longest path: 84 bytes, 85 with the NUL (within `kPathCap` 96); four components.
- **Save** (Shift + F5 on a module instrument's page on the Instrument screen, or on an insert's parameter page): a text entry (`Target::Preset`, the `FileName` filter) with the suggested name (the instrument's name, or the effect's display name); on confirm the values in force are read (the instrument blob, or the insert's `INSX` floats: every parameter of the descriptor, the blob's missing tail as the defaults), named from the descriptor, and written by a save job (`SaveService::save_preset`, the atomic save `NewOnly`); a taken name gets `<name> 2` .. `<name> 99` (Q3 a, the `.pti` rule), the toast says `PRESET SAVED: <NAME>`; the card's gate (no card, write protected, exFAT) and the other refusals say why, as every save.
- **List** (`ModuleUi::scan_presets(stem)`, main loop, F7): the `.o3mp` files of `modules/presets/<stem>/`, names sorted case-insensitively, at most 128 (the rest not listed; the list says `+ MORE ON CARD`), rescanned each time the overlay opens.
- **Load** (`ModuleUi::read_preset(stem, name, ParamSet&)`, main loop): the file read whole into a 1 KiB buffer (synchronous: one cluster, as the settings file), `formats::preset::read`, then **resolved by name (Q5 a)** against the target module's descriptor: each entry matches the first parameter of that name (ASCII case-insensitive) not yet matched; the value is clamped to the range and rounded for an INT; an unknown name is counted; the result is a `ParamSet` (source Card, the mask of the matched parameters) that ABI2's `apply_preset` writes (unmatched parameters take their defaults; an instrument in one undo unit, an insert with none). A file of another module or kind is refused (`PRESET OF <STEM>`), a damaged one says `PRESET DAMAGED`, a newer major `NEWER PRESET FORMAT`; a load with skipped names says `PRESET LOADED, <n> SKIPPED`.
- **The list overlay** (ABI2 A9's F5 PRESET): the factory section (from the descriptor) then a `CARD` section; F1 LOAD on either; Shift + F5 (SAVE) also from inside the overlay. A module with neither says `NO PRESETS, SHIFT+F5 SAVES`.
- **Memory.** The `Preset`, the 1 KiB buffer and the list's 128 names (32 bytes each) are members of the App (SDRAM, F10): SRAM 0. CPU: main loop only, render 0.
- **Built-in packs (Q6 a, P5).** `modules/<id>/presets.json` (TB-303, FM, Chip, SID; four to six sounds each, parameter names as in each `module.json`) built by `tools/o3preset.py pack` into `modules/presets/<id>/*.o3mp` of the starter zip (`make_starter.py`, a `presets` list in the manifest).

## 5. Slices

| # | Slice | Size | Level | Depends on | Flash [est] | SRAM |
|---|---|---|---|---|---|---|
| P1 | The `O3MP` format: reader, writer, the format document, round-trip tests | S | C | none | +1.2 KB | 0 |
| P2 | `tools/o3preset.py`: the Python reference (dump, make, pack) and its cross-test | S | B | P1 | 0 | 0 |
| P3 | The card side: save guard rows, `SaveService::save_preset`, `scan_presets`, `read_preset` and the resolve by name, console verbs | S | C | P1, ABI2 A8 | +1.6 KB | 0 (SDRAM) |
| P4 | The UI: Shift + F5 SAVE and its text entry, the overlay's CARD section, messages, sim goldens, QA, manual | S | B | P3, ABI2 A9 | +1.2 KB | 0 |
| P5 | Factory packs for TB-303, FM, Chip and SID in the starter zip | S | B | P2 | 0 | 0 |

Total flash about +4 KB [est]; SRAM 0; render CPU 0.

**P1 — the format** (S, Level C). Files: `formats/include/o3os/formats/preset.h`, `formats/src/preset_file.cpp`, `formats/CMakeLists.txt`, `docs/formats/o3-preset.md`, `tests/preset_format_test.cpp`, `tests/data/preset_basic.hex` (a reference file as hex text, shared with P2: no binary in git). Acceptance: write then read gives the same `Preset` for 0, 1 and 32 entries and every float bit pattern used (including -0.0 and denormals); the bytes of the reference preset equal the hex fixture; each error case (truncated at every byte offset, bad magic, a CRC off by one bit, a newer major, a newer minor with extra bytes read as 1.0, a NaN, a 16-byte name, a count of 33, two `MPST`, an unknown chunk before `CRC ` ignored, over 1,024 bytes) gives its `Error` and never reads past the buffer; duplicates counted. Host tests: `preset_format_test` (doctest, label `formats`). QA rows: `docs/qa/catalogue/global.md` one host-only row (the format's round trip). Unit check: none (P3).

**P2 — the Python reference** (S, Level B). Files: `tools/o3preset.py` (`dump <file>`, `make --module <stem> --kind instrument|effect --set Name=value ... -o <file>`, `pack <presets.json> <outdir>`), `tests/o3preset_test.py`, the ctest `preset_python` in `tests/CMakeLists.txt`, `docs/formats/o3-preset.md` (the tool's section). Acceptance: `make` of the reference preset gives the hex fixture's bytes; `dump` of it prints every field; `pack` refuses a non-finite value, a name over 15 bytes, more than 32 entries; the CRC is `zlib.crc32`. QA rows: none.

**P3 — the card side** (S, Level C: a new written path). Files: `hal/include/o3os/hal/save_guard.h`, `hal/src/save_guard.cpp` (the rows of section 4), `tests/save_guard_test.cpp` (the new rows and their limits: a fifth component, `..`, a `.o3mp.bak`, a rename or remove of a Target refused), `app/include/o3os/app/save_service.h`, `app/src/save_service.cpp` (`save_preset`: the folder chain made, NewOnly, the next free name), `app/include/o3os/app/module_ui.h`, `app/src/module_ui.cpp` (`scan_presets`, `read_preset`, the resolve), console `mod presets <stem>`, `mod psave <slot> <name>`, `mod pload <slot> <name>`, `ins psave` / `ins pload <track> <slot> <name>` (`platform/mk66/src/app_main.cpp`). Tests: `tests/preset_card_test.cpp` (the fake card of `tests/save_fatfs_test.cpp`: save then list then load gives the values; a taken name becomes `<name> 2`; a cut at every step of the save leaves the old state or the new file, never a damaged Target; a file of another module refused; a damaged file refused with its message; a renamed parameter is skipped and counted, the rest load; a module version with a parameter added: the new one takes its default; 129 files list 128; an insert's preset applies to the blob with no undo, an instrument's in one undo unit). QA rows: `docs/qa/catalogue/modules.md` console rows. Unit check: `mod psave` on a TB-303 instrument writes `modules/presets/tb303/<name>.o3mp` on the card (read back on a computer with `o3preset.py dump`), `mod pload` restores it after the knobs moved.

**P4 — the UI** (S, Level B). Files: `app/include/o3os/app/view_state.h` (`TextEntryView::Target::Preset`, the overlay's CARD section), `app/src/save_actions.cpp` (Shift + F5 on a module instrument: the preset save instead of `SAMPLE INSTRUMENTS ONLY`), `app/src/bindings.cpp` (Shift + F5 on the insert parameter page and in the overlay), the overlay's actions (ABI2 A9's file), `app/src/screens/params_screen.cpp` and the Instrument screen's key row (`SAVE` on Shift), sim `sim/scenarios/qa/qa_presets.txt` (extended: save, list, load, a taken name, a damaged file) and its goldens (regenerated on the remote and looked at). Tests: `tests/module_ui_test.cpp`, `tests/mixer_inserts_test.cpp`, `tests/text_entry_test.cpp` (the Preset target). QA rows: `docs/qa/catalogue/instrument.md`, `modules.md`, `mixer.md`. Manual: `docs/manual/modules.md` (a "Presets" section: save, load, where the files are, copying them on a computer), `docs/manual/mixer.md` (insert presets); ui-map lines. Unit check: save a TB-303 sound with Shift + F5, change it, load it from F5; the same on a WIDE insert.

**P5 — factory packs** (S, Level B). Files: `modules/tb303/presets.json`, `modules/fm/presets.json`, `modules/chip/presets.json`, `modules/sid/presets.json` (four to six named sounds each, own values), `tools/release/make_starter.py` and `starter_manifest.json` (a `presets` list: the packs built into `modules/presets/<id>/`), `tools/release/README.md`, the starter's test (`tests/release_starter_test.py` or the existing one). Acceptance: the starter zip holds the packs; every file reads back with the P1 reader (a ctest that runs the C++ reader over the pack's files, or `o3preset.py dump`); each preset's names all resolve against its module's `module.json`. Unit check: the starter copied to a card: the TB-303's F5 list shows the pack's presets under CARD.

## 6. Assumptions (decided here)

- A-1 The file's name (its stem) is the preset's name: no name field inside the file (renaming the file on a computer renames the preset).
- A-2 Values are stored for every parameter of the descriptor at save time, in descriptor order, named; a KEY parameter (ABI2) is saved like any other (the key track is part of an effect's sound).
- A-3 Name matching is ASCII case-insensitive, first unmatched parameter of that name; clamped and INT-rounded; unknown names skipped and counted.
- A-4 A file of another module or kind is refused, not resolved by name.
- A-5 The list is read synchronously on the main loop (F7's pattern), at most 128 names; the file is read whole (at most 1,024 bytes).
- A-6 The suggested save name is the instrument's name (sanitised as `instrument_file_stem`) or the effect's display name.
- A-7 Unknown chunks are ignored and not kept (the settings file's rule); a preset is never saved over, so nothing is dropped.
- A-8 The packs' sounds are this project's own values, written for each module.

## 7. Risks

- R1 **Format mistakes are permanent** once a release writes them: Level C risk review of P1 and P3; the hex fixture fixes the bytes; the format document is normative.
- R2 **Card writes** (a new allowlisted path): the guard tests and the cut tests of P3; NEVER replaced, no Backup form, nothing removed.
- R3 **Main-loop stalls**: a folder of 128 files scanned when the overlay opens (a few ms [est]); a busy card with many files: the 256-entry readdir bound.
- R4 **Parameter renames in a module** break name matching for that parameter: counted and said (`<n> SKIPPED`); module authors are told in the SDK README to keep names stable.
- R5 **Flash** (+4 KB [est] at 94 to 97 %): measured per slice; P4's UI shares ABI2 A9's overlay.

## 8. Evidence labels

- The `.o3mp` file, the card folder, save, list and load, the built-in packs: **new Community OS behaviour** (F1: the official firmware has Perform presets only, verified from tracker-ce).
- F2 to F11: code facts at c1dc3125. Flash figures are estimates until each slice's `size -A`.

## 9. State for implementer

P1 and P3 Level C (a risk review and one independent review); every slice: scoped CI on the remote (`tools/remote_ci.sh`, `O3OS_CI_SCOPE=changed`), one `tools/ci.sh host` per branch; report flash and SRAM deltas (SRAM must stay 0).

- **P1.** Start with: `formats::settings` (`formats/include/o3os/formats/settings.h:12-60`) and `formats/src/settings_file.cpp` (the read and write of a whole small file), `native::codec::Cursor` (`formats/src/native_codec.h`), `crc32` (`formats/include/o3os/formats/crc32.h:22`). Rules: no heap, no I/O, never read past the buffer; big-endian; the layout of section 3 exactly; a newer minor read, a newer major refused.
- **P2.** Start with: `tools/o3m_pack.py` (a Python tool with its ctest), `tests/o3m_gen_scaffold_test.py`. Rules: `python3 -I`, no third-party packages; bytes equal the hex fixture.
- **P3.** Start with: the guard's table (`hal/include/o3os/hal/save_guard.h:8-56`) and `tests/save_guard_test.cpp`, `SaveService::save_instrument` (`app/include/o3os/app/save_service.h:322`, `app/src/save_service.cpp:849-935`), `ModuleUi::scan` (`app/src/module_ui.cpp:610-634`), ABI2's `ModuleUi::apply_preset`. Rules: NewOnly, never replaced; four components; the resolve by name of section 4; main loop only.
- **P4.** Start with: `instrument_save` (`app/src/save_actions.cpp:211-225`), `TextEntryView::Target` (`app/include/o3os/app/view_state.h:83`), ABI2 A9's overlay bindings, `sim/scenarios/qa/qa_presets.txt`. Rules: Shift + F5 on a sample instrument stays the `.pti` save; goldens regenerated on the remote and looked at.
- **P5.** Start with: `tools/release/make_starter.py:94-150` (`validate_manifest`, the manifest's lists), `modules/tb303/module.json` (the parameter names). Rules: own sound values; every name resolves.
- Unit checks (tester, a gated image): P3 console save and load on a TB-303; P4 Shift + F5 and F5 on an instrument and an insert; P5 the starter's packs listed.
