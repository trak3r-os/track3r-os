# OS-FINALQA final report, 2026-10-20

Written from the two tester reports (TESTER-1 on v1.0.0-fqa.1, TESTER-2 on v1.0.0-fqa.2), the candidate notes, the usability review and the OS-FINALQA commits on dev. The evidence words are the repo's: `verified parity` (none is claimed here), `new Community OS behaviour`, `open research question`. Measurements are marked `[unit]` (the tester's unit), `[sim]` (simulator) or `[host]` (host test).

## 1. Scope and images

| Image | Source commit (version string) | File | CRC32 | Used for |
|---|---|---|---|---|
| v1.0.0-fqa.1 | d10ded4 (`v0.7.0-alpha.1-571-gd10ded4`) | `o3os-v1.0.0-fqa.1-d10ded4.ptf` | 9E65CE2F | TESTER-1: battery, shots, CPU, console rows, T2, T3 |
| v1.0.0-fqa.1 bench | d10ded4 with `O3OS_BENCH=ON` | `o3os-v1.0.0-fqa.1-bench-d10ded4.ptf` | 3E0B5C7C | bench commands (`cpuopt`, `bench stages`, `mod`) |
| v1.0.0-fqa.2 | 1c90d41 (`v0.7.0-alpha.1-603-g1c90d41`) | `o3os-v1.0.0-fqa.2-1c90d41.ptf` | 833B54BE | TESTER-2: changed screens, re-runs |

- Both release images are MinSizeRel with BENCH=OFF; the bench image adds the bench commands only.
- Gate: `fw-gate-check` (check_image 8 of 8, flashconfig exact, ctest 19 of 19); the release build matches the second build machine's image. fqa.2 has no bench image.
- Size, fqa.1: FLASH 881,512 B (84.07 %), SRAM_L 49,880 B (97.42 %), SRAM_U 138,728 B, SDRAM_STATIC 146,720 B. fqa.2: FLASH 84.08 %, the rest unchanged.
- Tests on the candidate code: host 229, e2e 56, qemu 58, mk66 19, all passing.
- The unit ran fqa.1 and fqa.2 with the card's `Projects/t-v16` untouched; saves went to `t-v16-copy` and the tester's own projects.

## 2. What ran

| Run | Scope | Result |
|---|---|---|
| Install and smoke (TESTER-1 §0) | `--put`, one `--update`, STANDARD/AUDIO and profile changes | PASS; disk share 3 of 3; underruns 0 at idle |
| New features (TESTER-1 §1) | OGSAVE (QA-FIL-104), PATTEMPO (QA-GLB-153..155) | PASS; OGSAVE F4 takes about 14 s (open, see §5) |
| HIL battery, fqa.1 (§2 of its report) | 43 `qa_*.json` scenarios | 26 PASS first run; 3 PASS on rerun (qa_power_last, qa_record, qa_usbrec); 2 firmware FAIL (qa_h4, qa_sample_edit); 1 scenario defect (qa_perform, QA-PRF-023); 11 NOT RUN |
| HIL re-run, fqa.2 (TESTER-2 §1) | fixed scenarios | qa_sample_edit, qa_record, qa_wavexp, qa_power_last (alone) PASS; qa_usbrec PASS on rerun; qa_perform FAIL on the Pattern-ends-Perform-FX step (fixed in this batch, §4); qa_cpu_heavy FAIL (§4 CPU); qa_h4 NOT RUN (SMF EXPORT is not tested on the unit, §5) |
| Unit shots, fqa.1 | 39 states x 3 themes = 117 PNG (amiga, black, white) | captured; reviewed in the simulator, §6 |
| Soft golden compares, fqa.1 | 52 soft diffs in 9 logs | 22 RECTS alpha on the FT812 (known, OS-SIMKEY), 30 from the unit's state; no hard step depends on them |
| CPU, heavy song, fqa.1 and fqa.2 | 60 s and 75 s windows, `render last` reads | FAIL against the 83 % budget (see §3 and cpu-budget.md section 4) |
| CONSOLE rows (hands-list "Tester can run these", 40 rows), fqa.1 | console, shots, exports | 30 PASS; 2 FAIL (QA-PRF-122, since reworded: the DJ fine tempo is not an undo unit); 8 NOT RUN |
| T2 (24 items), fqa.1 | console and shots | 20 PASS (6 partial); 1 partial; 3 NOT RUN |
| T3 (17 items), fqa.1 | measured over the USB capture | 12 PASS; 2 inconclusive (QA-PRF-026, 027); 3 NOT RUN |
| Changed screens, fqa.2 (TESTER-2 §2) | Params source visual, Wave after File OPEN, console tempo | PASS; Amiga shots of changed screens done |

Catalogue status on this branch (the `Status` column of `docs/qa/catalogue/*.md`, 880 rows): 44 `unit PASS`, 478 `more testing needed`. The tester's battery and console results are not yet written into the rows; that is the next catalogue step, not part of this report.

## 3. Firmware defects found and fixed

Fixes merged to dev during OS-FINALQA. "Unit" says where the fix was checked on hardware.

| Defect | Found by | Fix commits | Unit check |
|---|---|---|---|
| SMF EXPORT: F1 did its work inside one call and overran the app view (APP STOPPED, quarantined for the boot) | TESTER-1, qa_h4 (QA-MOD-092) | a9f3621 (bounded units over the frames), 8309572 | host only; maintainer: do not test SMF EXPORT on the unit |
| NORMALISE on an already normalised sample said DONE instead of nothing to do | TESTER-1, qa_sample_edit (QA-SED-002/004) | 23ed769, 36d8f05 (the notice replaces the progress at once) | fqa.2 PASS: NOTHING TO NORMALISE |
| Wave page on File OPEN came back on CHOP with an instrument in LOOP FWD | FQA-1a finding, TESTER-1 | 075887c (Wave on the page the play mode picks) | fqa.2 PASS: WAVE page, MODE LOOP FWD |
| Console `state` tempo lagged the project while stopped | TESTER-1 §1 note | 950cc18 | fqa.2 PASS: 12700 to 13000 at once |
| OGSAVE progress could fall back between compare and write; held-check gaps | rev-ogsave, TESTER-1 §1 | f492d28 (tests and the progress fix) | TESTER-1 §1 PASS; time open (§5) |
| PEAKS strip stuck on BUILDING on Params (peak cache built on Wave only) | FQA-1a | 2db6475, 09ae426 (quiet Params peak build) | fqa.2 PASS: the source visual draws |
| OG export note did not fit the screen; export note wording | usability review, TESTER-1 | 0b4fda6, f8c8e45 (keytext allow list) | sim shot; unit not re-read |
| Usability texts: File import info line named F7 wrongly, Sample edit no-selection note clipped, labels and Shift+F notation | usability review, shot review | 13b19f0, a6766cc, 15fe30d | sim shots; unit panel judgement open (§5) |
| Underrun mute stayed on for 256 on-time blocks before it lifted | CPU run, maintainer decision B (2026-10-20) | 23dba4d (lift after 32 on-time blocks), e453c8c, 178d08b, 883937b (tests and HIL F-1 follow the constant) | fqa.2 [unit]: short overloads recover; the heavy song does not (never 32 on-time blocks in a row) |

Evidence words for the fixes: the NORMALISE, Wave, tempo and PEAKS rows are `new Community OS behaviour`, checked on the unit as the table says. The mute lift is a `new Community OS behaviour` whose unit result is `[unit]`: it helps short overloads and cannot help a sustained one.

## 4. Scenario and catalogue fixes (not firmware)

| Item | Where | State |
|---|---|---|
| QA-PRF-023 steps: `_play_song` ends on Pattern, and leaving Perform for Pattern ends the Perform FX (only Master and Song keep them) | `platform/mk66/tools/hil/gen_hil.py` (`play_song_stay`), `qa_perform.json` | fixed in f005bc4; the scenario stays on Song |
| `usb profile 8 duplex` accepted only `profile` | `qa_usbrec.json` step 2 | fixed in f005bc4: both `already presenting` and `re-enumerating` pass |
| qa_cpu_heavy step 14 expected `OPENING_HEAVY` only | `gen_hil.py` | fixed in the repo (accepts `PROJECT_HEAVY_OPENED`); the hard `mix` step is still open (§5) |
| qa_record save wait too short (the 1.2 MB take is DONE within 40 s); `ls Samples` placeholder; `mon` expects 0 for unity | `gen_hil.py` | fixed; qa_record PASS on fqa.2 |
| qa_power_last fails when chained straight after qa_power (the unit is rebooting) | scenario order | PASS alone; run it alone or after a pause |
| QA-PRF-122 says undo; the DJ fine tempo has no undo by decision | `docs/qa/catalogue/perform.md` | reworded to "not an undo unit" (8716a31) |
| QA-PRF-023, 024, 029 rows did not say that the value is stored before it is latched (Rec on) | `docs/qa/catalogue/perform.md` | wording fixed (29ba126, 230dfe2) |
| Battery scenarios and `finally` blocks load `Projects/t-v16` into RAM (read only) | scenario design | noted; the card copy was hashed after the run and unchanged |

## 5. Open items

| # | Item | State | Next step |
|---|---|---|---|
| 1 | CPU: the heavy song does not fit the budget (decision A) | render mean 95 % to 96 % of the block against 83 % Total; worst block 130 % to 171 %; underruns 5 % to 26 %; the mute stays on for the song `[unit]`; documented in cpu-budget.md section 4 | maintainer: release 1.0.0 with the limit documented |
| 2 | qa_cpu_heavy hard `mix` step (step 28) cannot pass on the overloaded engine; the console is slow under load | open; the run used copies with the step soft | scenario change, after 1.0.0 |
| 3 | DJ fine tempo has no undo | decided by the maintainer; QA-PRF-122 says so | none |
| 4 | SMF EXPORT not unit-tested | the fix a9f3621 is host-tested only; maintainer: do not test it on the unit | maintainer, if ever |
| 5 | QA-PRF-026 and 027 (DELAY and REVERB slots) inconclusive twice | `open research question`: a held value starts from the stored row, not from 0; the stored value survives Delete and RESET and shows on the next hold (fqa.2: held DELAY read 70 after Delete) | clean re-measure in a fresh project with no stored rows; tester on request |
| 6 | "Delete did not zero a held row" | observation (fqa.2), `open research question`: the stored slot value is kept on Delete and RESET | maintainer decides whether the page or the code is the intent |
| 7 | QA-MOD-114 FAIL: looper present costs +0.96 % of the block against a limit of 0.5 % (`bench stages`, fqa.1) | looper stays as it is (maintainer decision); the row keeps its FAIL | maintainer |
| 8 | USB-001 (shot review): chrome-dim labels read about 1.7 : 1 on the unit's fills; the simulator reads 5.3 to 5.5 : 1 | judge on the panel | hands session |
| 9 | USA-001 to USA-004 (usability): PARAMS header, F1 label, F7 APPLY on INFO, the F8 hint during the OG export count (no label, no binding) | confusing, not blockers; labels for the implementer | label batch; the panel check is in the hands list |
| 10 | QA-PRF-014, 022, 028 (step FX, panning FX, step FX 30/32 in the pattern) | not built | future scenario build |
| 11 | QA-MOD-113 (LFOLIVE protocol on the copy), QA-MOD-117 (`LAUNCH?` inverted shot), QA-MOD-130 (no app calls get_transport) | NOT RUN | protocol for the copy, or hands |
| 12 | Latent items recorded, not fixed: Amiga offline-leave state; full param ring; ROUTING SND A/B; QA-GLB-016 chord text (catalogue says Shift+F1+F7+F8, code is Shift+F1+F8); the module-app help line omits the ABI < 1.10 rule; `mon` reads 65536 with MONITOR on and off (firmware question) | recorded | FQA-10 follow-ups |
| 13 | QA-CFG-212 (CARD BUSY wording not seen during a save or an export); NO CARD and eject cases | partial | hands or a later run |
| 14 | Unit NOT RUN from fqa.1: QA-GLB-131/132/135/136 (Shift+Power), QA-INP-005/008 (boot chord), the USB disk and power-off scenarios, the card-pull scenarios | need a person | hands list, §6 |

## 6. The maintainer's remaining steps

1. Hands session: `docs/qa/hands-list.md` holds 91 hands items (81 rows in the groups T1 to T3 and M, plus 10 added items); the M group is maintainer-only (21 items).
2. The panel checks in §5 (items 8 and 9) and the NOT RUN rows of §5 item 14 go into that session.
3. Decide release 1.0.0 with the heavy-song limit (§5 item 1). Then cut `rc/1.0.0` from a green, reviewed `dev` and publish: the publication step is the maintainer's.
4. Remote branch clean-up decided on 2026-10-20 (`feat/OS-PATTEMPO`): the maintainer runs the delete; not confirmed here.
5. Decide the open research questions in §5 items 5 and 6 if they matter for 1.0.0.

No firmware image has been flashed by this report. The standing firmware GO covers the gated candidates; no further install is asked for here.

## 7. Release decision

The maintainer decided on 2026-10-20 to release **v0.9.0b**, a beta, from dev at this date, instead of 1.0.0. The release notes are `docs/release/v0.9.0b.md`; the heavy-song limit of section 5, item 1, is a known issue there.

Two items are parked for later:

- the clean re-measure of Perform DELAY and REVERB (QA-PRF-026 and 027, section 5, item 5);
- the maintainer's hands session (`docs/qa/hands-list.md`: the M group and the rows that need a person, section 6, item 1).

Release 1.0.0 comes later.
