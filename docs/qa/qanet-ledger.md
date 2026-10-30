# OS-QANET ledger

One row per bug that a person found and no test caught. It answers one question for each: which rule, generator, matrix cell, invariant or audit line of the net caught it, or should have. The process rule is section 9 of `docs/specs/OS-QANET.md`: every bug a person finds gets a regression test that fails before the fix and a row here, in the same branch as its fix. If nothing in the net would have caught it, the same branch adds something that would (a rule row, a generator weight, a matrix cell, an invariant, an audit line) or writes `none, because <reason>`. The `firmware-reviewer` checks the row.

Evidence: the rows are `verified` against the code and the baselines on dev at the date below; "found" means a test of the net finds the bug on dev as it is, without being told where.

## The ledger

Classes (OS-QANET section 2): 1 our spec is the oracle, 2 sequences between screens, 3 two paths for one thing, 4 the simulator differs from the unit.

States:

- `found`: a test of the net caught it. The regression cell names the ctest and the baseline file that holds a line tagged with the ID. When the fix lands, the line leaves the baseline.
- `flagged`: an audit of part E flagged it and no test holds it yet; the regression cell names the audit document. The fix adds the test.
- `fixed`: the fix landed; the regression cell names the ctest that failed before it. The fix's own commit moves the row here.
- `tried`: the net tried and did not reproduce it; the note names the slices.
- `pending`: no part has tried yet; the note names the slice that will.

<!-- ledger -->

| ID | Class | Regression test | Catcher | State |
|---|---|---|---|---|
| OS-BUG-NEWLIST | 2 (and 4) | - | none, because the simulator does not reproduce it: B1 ran 1000 seeds on a large card (about 80 NEW then Sample load listings, none empty) and a scripted File, NEW, Sample load run lists correctly; the scan or directory-handle state that FatFs may leave is not in a host folder | tried (B1, D1 retries it on FatFs; the maintainer and the orchestrator debug it with the unit) |
| OS-BUG-PREVLAT | 4 | `qanet_budgets`, `unit_rest` (audition_test) | none, because no latency target exists yet; the matrix cell file.preview measures the onset but has no budget to fail on | fixed (OS-QAFIX D3, 2026-10-23: the preview streams and follows the cursor; the two baseline_budgets lines are gone; the unit time to first sound is still to measure) |
| OS-BUG-SLDOVER | 1 | `qanet_rules_*of4`, `unit_rest` (sample_load_test) | rule 1 (A1): a document change needs an undo unit or a question; two lines, WAV onto a used sample slot and onto a module slot; E also flagged it as E-LOAD-12 | fixed (OS-QAFIX D1b, 2026-10-23: a used slot asks REPLACE first, a module slot refuses; the baseline lines are gone). Rule 1's blind spot behind it (a full sample-edit history credited any change) is narrowed by B8a and held by `qanet_random_exhaust_selftest` |
| OS-SLDTARGET | 3 | `docs/qa/og-user-audit/triage.md`, `keytext`, `qanet_keys_*of6` (the R2 lines of baseline_keys.txt are gone), `unit_rest` (sample_load_test) | part E audit lines E-LOAD-03, E-LOAD-10 and E-LOAD-05 (triage rows 7, 39, 35): a pad picks the target but nothing says so; no rule can see a missing hint, it is a UX difference against the official firmware; also finding 11 of `baseline_keys.txt` (F8 unlabelled, Shift+F7 unbound) | fixed (OS-QAFIX D1c, 2026-10-23: the header reads TARGET INSTRUMENT nn, Shift+F7 / F8 INST - / INST +, F8 CH MONO; `unit_rest` sample_load_test, QA-SLD-021/022) |
| OS-BUG-ADDSLICE | 1 | `qanet_select_*of3`, `tests/qanet/baseline_select.txt` | rule 3 (A3): an operation on a selection acts on the selection, with the maintainer's decision of 2026-10-21 as the oracle; five lines, wave.add in five states | fixed (OS-QAFIX G2, 2026-10-22; the baseline lines are gone) |
| OS-BUG-SLICEPITCH | 3 (and 2) | `qanet_audio_matrix.slice.beat_*`, `qanet_audio_matrix.ring.*` | audio matrix (C1 to C3): pad against Pattern step on a freshly chopped slice (slice.beat_even, beat_add, beat_auto) and after a play-mode change, an undo or a load; the ring cells (C3 more) showed that the old descriptor stayed for as long as a voice sounded, a pad was held, pads were played in a row or the transport played (0.9 s to 4 s); C4 found the same variants in the random walk | fixed (OS-BUG-SLICEPITCH, 2026-10-22; `baseline_audio.txt` is empty, the ring cells are its regression test) |
| OS-AUDIT-A7-SUMMARY | 4 | `unit_rest` (sample_load_test: the summary of a multi-load is the last word on the strip) | none, because no rule or matrix cell lists the order of the strip's news; the host rig shows the summary after the wait of the PEAKS line of the instrument in view, the unit's longer build held it back for good | fixed (OS-AUDIT-41, 2026-10-24: the waveform build that follows a multi-load is quiet; unit fqa.11 to confirm) |
| OS-BUG-WATCHSEL | 1 | `qanet_keys_*of6`, `tests/qanet/baseline_keys.txt` | rule 2 (A2): a bound key that changes nothing and says nothing; Shift + the arrows in Pattern WATCH and REC follow while playing | fixed (OS-QAFIX, 2026-10-23) |
| OS-BUG-SIDWAVELABEL | 4 | `o3m_sid_test` (the case "sid views: OS-BUG-SIDWAVELABEL ...") | none, because no rule or matrix cell measures two draw calls of a module view against each other; the host view test drew the name with the stub's 12-pixel face, the theme's Data face is 20 | fixed (OS-BUG-COSMETIC1, 2026-10-30; unit re-check open) |
| OS-BUG-ONCARD | 4 | `unit_rest` (module_builtin_test: the case "built-in: OS-BUG-ONCARD ...") | none, because no rule reads the words of a list's state column; the label was a plain mapping of Ready to ON CARD that nothing in the simulator contradicts | fixed (OS-BUG-COSMETIC1, 2026-10-30; unit re-check open) |
| OS-BUG-CFGARM | 4 | `unit_rest` (key_prompt_test: the case "key prompt: Config marks F7 APPLY? ...") | none, because the simulator snaps right after the first F7, where the arm and the strip agree; no rule waits out the strip with a question armed (QA-CFG-034's 2 s was only tested for RESET?) | fixed (OS-BUG-COSMETIC1, 2026-10-30; unit re-check open) |
<!-- /ledger -->

The gate is `python3 -I tools/qanet_ledger.py --check --ctest-dir <build>` (ctest `qanet_ledger`, label `static`, under a second; its self-test is `qanet_ledger_selftest`). It fails on a row with no catcher entry, a `found` row whose ctest does not exist or whose baseline has no line tagged with the ID, a `flagged` row whose document does not name the ID, a `pending` or `tried` row that names no slice, a class outside 1 to 4, an unknown state and a repeated ID.

## Measuring the net

Four numbers (OS-QANET section 10). The runs print them; this section keeps the latest. A placeholder says which slice fills it.

| Number | Where it comes from | Today |
|---|---|---|
| Mutant kill rate per part and per class (the headline) | `tools/qanet_mutants.py`, F1: each planted bug of `tests/qanet/mutants/`, the parts that caught it | F1, 2026-10-23 (one full run at cc3dabe1 = dev 0292f250 plus the F1 files, 18 counted planted bugs plus 3 reverts of fixes that are shown and not counted, `docs/qa/qanet-kill-table.md`): **11 of 18 killed (61%)** by what `ci.sh host` runs; the 25-seed shard of the random user adds none; one kill (M18) is a Timeout beside a plain failure, none is only a Timeout or a crash. The earlier table (12 of 19, 63%) mixed two partial runs and counted three reverts of fixes; first table 2026-10-21: 11 of 19 (57%), 13 of 19 (68%) with the shard. Per part, mutants killed (of 18): A1 2, A2 1, A3 1, B 4, C 3, D1 3, D3 0; per class: 1 spec is the oracle 4 of 7, 2 sequences 2 of 3, 3 two paths 3 of 6, 4 simulator vs unit 2 of 2. Survivors name the gaps: no dirty-document state for the firmware update (A1, M20; the reverts M01 M02 survive for the same reason on OPEN and IMPORT), no card-state rule (M05), no unshared-pattern Song state (A2, M06), a wrong but non-empty listing (B, M09), slices that sound alike so the Wave pad's slice is not told (C, M14), no pad pressed in the block of the edit (C, M21), no save and reload (B5, M19: closed after that run, 2026-10-24: the save+OPEN relation of the meta pass kills M19 in `qanet_random_user_short`, so 12 of 18 once the next full run counts it) |
| Tuple coverage per seed-minute | `o3_qanet_random_user` prints, per seed, the distinct (screen from, action, screen to) tuples and, per run, the total and the time; B6 steers the walk to the unseen tuples and divides by the minutes | B1 prints the counts (a line per seed, then `tuples: N distinct ... over the run`, with the tuples listed under `--verbose`); the per-minute figure is B6's: not computed yet |
| Ledger rate | the share of the rows above whose catcher is not `none`: printed by `python3 -I tools/qanet_ledger.py --stats` and by the gate. Target: rising | 5 of 8 rows have a catcher (62%); the three without are NEWLIST (does not reproduce on the host), PREVLAT (no budget yet, D3) and A7-SUMMARY (no rule lists the order of the strip's news) |
| Steps to the first finding on a planted mutant | the random user's efficiency, B6 and F1: the step count at which a seed first reports a mutant | F1 and B6: not measured yet |

The ledger's own states today: found 1 (SLDOVER), flagged 1 (SLDTARGET), tried 1 (NEWLIST), pending 1 (PREVLAT), fixed 3 (ADDSLICE, SLICEPITCH, WATCHSEL).
