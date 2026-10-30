# QA feature catalogue

One file per screen of `docs/ui/ui-map.md` §1, plus `global.md` for global keys, transport, MIDI, USB and storage. The table format, the ID rule and the incremental rule are defined in `docs/specs/OS-FINALQA.md` §1–2.

- Simulator scenarios: `sim/scenarios/qa/qa_<screen>.txt`. The `qa_` prefix keeps their test names and golden prefixes apart from the other scenarios.
- Unit scenarios: `platform/mk66/tools/hil/qa_<screen>.json`.

## The gate

`tools/qa_cover.py --check` (ctest `qa_cover`, label `static`, runs in every scope and on every push, under a second) reads every row and fails on:

- **A reference that does not resolve.**
  - Sim cell: `` `qa/qa_x.txt` › `label` `` (a `snap label` of that scenario; the `.txt` and the `qa/` may be left out, and a bare scenario name resolves too), `` `gallery/name` `` (a picture of `sim/goldens.txt`), or a bare snap or host test.
  - Unit cell: `` `x.json` › `label` `` (the JSON is in `platform/mk66/tools/hil/` and a `say`, `note`, `shot`, `snap` or `name` of its steps has the label as a whole token, so `T1` is not in `T16`; a label written like an ID, `qa_pat_256`, is among the IDs a step puts first, with ranges such as `QA-INS-092..095` and lists such as `QA-PAT-255 / 257` expanded, and a sentence that only mentions the ID, like a header that names the whole file, does not count; a bare `` `x.json` `` needs only the file).
  - Host: in the Status cell after `host PASS (...)`, and in a Sim or Unit cell as `host: `x_test`` or any `x_test` / `x.cpp` / `x.py`: a file of `tests/`, `platform/mk66/tests/` or `tools/`, a ctest name, or a `TEST_CASE` title (or its start). A word that only occurs in the test sources, such as a function name, is not a test name: write the test.
  - Text in parentheses is a remark and is not read; `-` and `none` mean no reference, but a backtick reference after them (outside parentheses) is still checked.
- **A row with no reference.** A row that is not dropped names at least one reference that resolves, or writes `manual: <reason>` in its Unit cell. The older forms (`by hand`, `manual (not scripted)`) stay valid only on a row unchanged since the base.
- **A row changed or added since the base** (`$O3OS_QA_BASE`, else `git merge-base HEAD dev`; with none the rule is skipped with a warning) is held to the line above without the older forms.
- A row that does not have eight cells, or an ID used twice.
- The index table below out of date.

`python3 -I tools/qa_cover.py --self-test` (ctest `qa_cover_selftest`) plants each fault in a scratch catalogue.

## A bug a person finds

Every bug a person finds, on the unit or in the simulator, gets a row in `docs/qa/qanet-ledger.md` in the same branch as its fix (OS-QANET section 9): the class, the regression test that failed before the fix, and the rule, generator, matrix cell, invariant or audit line that caught it or should have (`none, because <reason>` when none would). `tools/qanet_ledger.py --check` (ctest `qanet_ledger`, label `static`) fails on a row with no catcher entry or with a test or baseline line that does not exist; the `firmware-reviewer` checks the row. A catalogue row for the fixed behaviour is still added here as usual.

## Index

Open = rows with neither a sim nor a unit PASS. The table is generated: `python3 -I tools/qa_cover.py --write` rewrites it from the rows (never edit it by hand; after a merge conflict in it, run the command again). Functions are the rows that are not dropped; Sim PASS and Unit PASS count the words `sim PASS` and `unit PASS` in the Status cell.

<!-- qa-cover -->

| Screen | File | Functions | Sim PASS | Unit PASS | Open |
|---|---|---|---|---|---|
| Config | [config.md](config.md) | 102 | 66 | 0 | 36 |
| File | [file.md](file.md) | 89 | 30 | 16 | 51 |
| Global | [global.md](global.md) | 76 | 26 | 7 | 43 |
| Input test | [input_test.md](input_test.md) | 9 | 0 | 0 | 9 |
| Instrument | [instrument.md](instrument.md) | 126 | 50 | 11 | 67 |
| Mixer | [mixer.md](mixer.md) | 37 | 17 | 0 | 20 |
| Module app | [module_app.md](module_app.md) | 9 | 5 | 0 | 4 |
| Modules | [modules.md](modules.md) | 151 | 21 | 1 | 129 |
| Pattern | [pattern.md](pattern.md) | 220 | 107 | 9 | 105 |
| Perform | [perform.md](perform.md) | 112 | 35 | 7 | 70 |
| Record | [record.md](record.md) | 38 | 6 | 1 | 31 |
| Sample edit | [sample_edit.md](sample_edit.md) | 23 | 13 | 0 | 10 |
| Sample load | [sample_load.md](sample_load.md) | 46 | 8 | 8 | 31 |
| Song | [song.md](song.md) | 67 | 50 | 7 | 17 |
| Wave | [wave.md](wave.md) | 26 | 20 | 0 | 6 |
| **Total** | | **1131** | **454** | **67** | **629** |

<!-- /qa-cover -->

Functions merged before the incremental rule (2026-10-14) get their rows during the final batch (OS-FINALQA §3).

Usability review of the simulator gallery (OS-FINALQA §1 item 5): [usability.md](../usability.md).
