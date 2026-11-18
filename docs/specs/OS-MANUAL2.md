# OS-MANUAL2: a manual for owners who know the official firmware, kept in step with the OS

Status: **approved** (maintainer, 2026-10-18, relayed by the orchestrator). Same-day amendments are folded in: how chapters open (D2), "explain things" with depth by complexity (D3), the official manual as inspiration only (D10), accessibility (D11) and newcomers (D1). M2-0 (template and skill) is done in the commit that folded them. Docs and documentation tooling only: no firmware change, no change to the image. Classification: `new Community OS behaviour` (documentation).

## 1. Why (read-only audit, 2026-10-18)

- **Only 4 of the 18 screen chapters explain their screen:** pattern, fx, instrument and record. The weakest are Perform, Song, Mixer, Wave and Sample edit.
- **Perform never says what performing is.** Missing:
  - the effects are live and not written;
  - pad n is slot n % 12 on row n / 12;
  - hold versus LATCH;
  - Shift picks the tracks;
  - which effects act;
  - leaving the screen ends them.
- **All 23 `manual-sync` markers are 54 to 140 commits behind.**
- **CI does not check staleness.** CI runs `tools/manual_check.py --strict` without `--stale` (`tools/ci.sh`, `manual_check_run`). `--stale` sees only added QA rows (`DIFF_ROW_RE`, `tools/manual_check.py:63`).
- **5 chapters are outside the `CATALOGUE` map** (`tools/manual_check.py:34`): cheat_sheet, computer_tools, glossary, install and quick_start.
- **The 78 `<!-- src: ... -->` comments in the chapters are read by no tool.**

## 2. Decisions (maintainer, 2026-10-18)

| # | Decision |
|---|---|
| D1 | **Audience.** Most readers are current owners who know the official firmware, so each chapter leads with what is new or different. It stays friendly to outsiders: a term is defined the first time a chapter uses it, or linked to the glossary; nothing assumes knowledge without a pointer to where it is explained; Quick start has a short path for someone new to trackers. |
| D2 | **How a chapter opens (correction, 2026-10-18).** No heading or fixed phrase such as "Coming from the official firmware". The opening is natural prose that assumes the reader knows the official firmware, and leads with what this screen does differently or adds. It does not name a comparison section. |
| D3 | **Explain things: the skill's first rule.** Every chapter teaches how its screen works and why you would use it, with examples, not just which key does what. See the depth tiers below. |
| D4 | **Same/different claims follow the evidence rule** (CLAUDE.md, and §5 below). |
| D5 | **Quick start becomes "what's different, in five minutes"**, plus a short path for newcomers (D1). |
| D6 | **Chapter template, in a fixed order** (below). The `manual-writing` skill and `docs/manual/chapter-template.md` follow it. |
| D7 | **Step 1 is the official-manual map.** A feature-by-feature map of Polyend's public manual and of the release notes after 1.7, with section references. Each feature is marked same, different or new in track3r-os. Paraphrase only: never copy its text, pictures or layout. |
| D8 | **Keeping the manual current:** generated key tables, per-section sync markers with a lock file, a branch gate, `--stale` in CI, the 5 unmapped chapters mapped, and walk-throughs as sim scenarios (§4, M2-2 to M2-4). |
| D9 | **Rewrite order**, one Sonnet doc agent per chapter, then a Sonnet fact review: Perform first; then the new features (Record and looper, Modules, step FX); then Song, Mixer, Wave and Sample edit; then Quick start, File and the Pattern intro. |
| D10 | **The official manual is inspiration, not a source to copy.** Never copy its layout, text or pictures, and no Polyend artwork. Take its approach: generous pictures and diagrams next to the steps. Ours are always our own simulator screenshots and generated SVGs. |
| D11 | **Accessible.** The rules are listed below. Where cheap, `tools/manual_check.py` checks them (M2-5). |

**D3, depth tiers.** Depth follows how complex the screen is.

| Tier | Chapters | What they get |
|---|---|---|
| Deep | Perform, Song (arrangement), Mixer (routing and sends), FX (step FX), Record (the looper), Modules, Pattern tools | the fullest explanation |
| Medium | everything else | a real explanation, shorter |
| Brief | Sample edit, Sample load, File | a short "what it's for", plus the tasks |

- Owners still meet what is new or different first.
- The explanation is deepest for features that are new in this OS (the map's "new" mark).
- Key tables are reference at the end, never the body.
- This replaces an earlier rule, "keep the concept part short for familiar screens".
- The maintainer's "more complex than the sample editor" was a comparison that sets the reference point: the sample editor is the simple end, and anything clearly more complex gets a deeper explanation. It was not a statement about any one screen. Putting Pattern tools in the deep tier, and so in C4, is the orchestrator's application of that rule (2026-10-18).

**D6, the template order:**
1. an opening, with no heading: what's new or different here, written for an owner;
2. What it is for;
3. How it works: the model (what is live, what is stored, what ends it), with examples and pictures, at the tier's depth;
4. Try it: a walk-through of 5 to 7 steps, ending with what you hear or see;
5. The screen at a glance;
6. More tasks;
7. Keys on this screen (generated, reference);
8. Good to know;
9. See also.

Brief chapters leave out How it works and Try it.

**D11, the accessibility rules:**
- plain language and short sentences;
- alt text on every image that says what it shows;
- no information carried by colour alone;
- good contrast in our SVGs;
- a clean heading hierarchy;
- tables with header rows;
- link text that names its target.

Running separately, not in this package: lane E's **OS-MANUALFIX** (`docs/OS-MANUALFIX`). It fixes the broken Perform table and 12 wrong screenshots, and trims the diagrams: 7 kept, 10 dropped, and the generator capped. OS-MANUAL2's slices start after it merges, and the Perform rewrite builds on its fixed table.

## 3. Rules for every slice

- **No firmware change.** Only `docs/manual/`, `docs/ui/`, `docs/specs/`, `.claude/skills/manual-writing/`, `tools/manual_*.py`, `tools/ci.sh` (host checks) and `sim/scenarios/manual/` + `sim/goldens.txt` + the scenario glob. The proof: `git diff --stat dev -- app audio core model seq formats ui eve midi usb module hal platform/mk66` is empty.
- **No official material in the repository:** no text, pictures, page layout or chapter structure from Polyend's manual. Only paraphrased facts, each with a section reference. No Polyend logos or artwork. "Polyend" and "Tracker" appear only to describe compatibility.
- **Every tool** is stdlib Python, run with `python3 -I`, with a self-test ctest. Each one's `--check` is cheap and runs in `tools/ci.sh host` before `scope_resolve`, as `manual_llms_check` does.
- **After any chapter change**, regenerate `docs/manual/llms*.txt` (`tools/manual_llms.py`). `manual_check --strict`, `manual_diagrams --check` and `manual_tables --check` must pass.

## 4. Slices

| Slice | What | Size | Level | Depends |
|---|---|---|---|---|
| M2-0 | Template and skill: the new chapter template and the `manual-writing` skill | S | A | **done** (with this spec's amendment) |
| M2-1 | The official-manual map | S/M | A | – |
| M2-2 | `tools/manual_tables.py`: generated Keys, Shift and cheat-sheet tables | S/M | A | – |
| M2-3 | Sync v2: section markers, `sync.lock`, `--ack`, branch gate, `--stale` in CI, the 5 chapters mapped | M | B | – |
| M2-4 | Walk-throughs as sim scenarios that feed the images | S | A | – |
| M2-5 | Accessibility checks in `tools/manual_check.py` | S | A | – |
| M2-C1 | Perform | S | A | 0, 1, 2, 4, MANUALFIX |
| M2-C2 | Record and looper, Modules (with module-instrument-fx), FX (step FX) | M | A | C1 |
| M2-C3 | Song and Mixer (deep), Wave (medium), Sample edit (brief) | M | A | C2 |
| M2-C4 | Quick start ("what's different, in five minutes" plus the newcomer path), File (brief), the Pattern intro, Pattern tools (deep) | M | A | C3 |
| M2-C5 | Every remaining chapter gets the template at its tier: the opening, the headings and the generated Keys table, plus a How it works where a medium chapter lacks one | S/M | A | C4 |

M2-1 to M2-5 can run in any order (M2-0 is done), and M2-1 comes before the first chapter. M2-3 can land after C1. A chapter written before it carries its markers in the new form, and M2-3 adds the lock. The chapter slices run one at a time in lane E.

### M2-0: template and skill (done)

Done in the commit that folded the amendments of 2026-10-18.
- **`docs/manual/chapter-template.md`** follows the D6 order: an opening with no heading, What it is for, How it works, Try it, The screen at a glance, More tasks, Keys on this screen, Good to know, See also. It carries tier, accessibility and walk-through notes.
- **`.claude/skills/manual-writing/SKILL.md`:**
  - New §0, the first rule: explain things, with the depth tiers (D3).
  - The audience (D1).
  - §1: the official manual as inspiration only (D10).
  - §2: define or link terms in every chapter.
  - §3: the new structure, and the opening rule (D2).
  - §7: the concept recipe for How it works (live, stored, ends; example; picture).
  - §8: generous pictures next to the steps, with caps per tier, and the accessibility rules (D11).
  - §9: "Works as the official manual describes" for the map's "same" plus a passing QA row.
  - §10: word budgets per tier.
  - §11: the Quick start shape (D5).
  - §13: the checklist.
  - §14: the never-list (no copying of the official manual; no fixed comparison phrase).
  - §15: a pointer to sync v2.
- Existing chapters are not changed here. The chapter slices bring each one to the template.

### M2-1: the official-manual map

- **Sources** (public, read-only, cited, never copied):
  - Polyend's Tracker manual, web edition labelled 1.7.0 (`polyend.com/manuals/tracker`), and its PDF `polyend.com/wp-content/uploads/2023/11/Tracker_Manual_Rev1.pdf`;
  - the official release notes for every version after 1.7.
- **Output:** `docs/ui/official-manual-map.md`. It goes in `docs/ui/`, not `docs/manual/`, because `manual_check` treats every `docs/manual/*.md` as a chapter.

  | Column | Content |
  |---|---|
  | Feature | our words |
  | Official manual | section number and title, or release-note version |
  | What it does there | a paraphrase in one line |
  | track3r-os | same / different / new / missing |
  | Our evidence | QA row ID or spec |
  | Notes | |

  The table is grouped by our screens, so a chapter writer reads one block.
- **Marks:**
  - **same:** the manual's description matches what our evidence shows.
  - **different:** the behaviour differs. Say how in one line.
  - **new:** the official manual and release notes do not describe it.
  - **missing:** the official manual or release notes describe it and we do not have it. This is a gap list for the maintainer, never a backlog row by itself.
  - An unclear manual gives `open research question`.
- **Acceptance:**
  - every chapter of the official manual is covered or listed as out of scope, with a reason;
  - no sentence is copied (the fact reviewer compares at random against the manual's text);
  - every "same" has our evidence;
  - the "missing" list is handed to the orchestrator for the maintainer.
- **Agent:** Sonnet with web access (a `radar-researcher`-style brief). Fetch the PDF into a fresh scratch directory and never commit it.

### M2-2: `tools/manual_tables.py`

- **Sources:**
  - `docs/ui/ui-map.md` §1, one row per screen: F1–F8, Shift+F1–F8, other keys;
  - §2, the global keys;
  - the catalogue rows for each key's QA reference.
- **Generated blocks** go between markers:
  - `<!-- manual-tables: keys screen=<name> -->` … `<!-- /manual-tables -->`: the Keys and Shift tables in each chapter;
  - `<!-- manual-tables: cheat <section> -->`: the grids in `cheat_sheet.md`.
- Flags:
  - no flag regenerates (the tool has no `--write`; `--check` writes nothing);
  - `--check` fails when a block differs from what the sources give;
  - `--self-test`.
- A ui-map row the tool cannot parse fails with the row named. The fix goes in ui-map, not in the tool. If ui-map lacks a fact the chapter tables need, record it as a latent issue and leave that cell for hand-writing with a `<!-- manual-tables: hand -->` note.
- **Acceptance:**
  - the self-test covers a changed ui-map label failing `--check`, and an unparseable row failing;
  - a plain run on today's chapters gives tables that match the hand-written ones, or each difference is a real mismatch, listed in the commit body and fixed in ui-map or the chapter.

**Built (2026-10-18), as it differs from the text above.** `ui-map.md` §1 is prose (its cells hold sentences, dynamic labels and `[planned]` notes), so the tool does not parse it. The ui-map's own rule 5 says its tables come from the code, and the code declares every key row: the `k<Screen>SoftKeys` and `k<Screen>ShiftKeys` arrays of `app/include/o3os/app/screens/*.h` and `mixer_view.h`, which `shift_labels_test` ties to the bindings; the screen buttons are Global's `nav.screen` rows of `bindings.cpp`. The tool reads those. A generated block holds the labels only (F1 to F8 and Shift + F1 to F8, one column per state a screen swaps between); what each key does, the pads, the encoder and the Ref column stay hand-written next to it. The tool reads every header of `app/include/o3os/app/screens/` and `mixer_view.h`; a `k...Keys[` declaration it cannot read (it takes `static`, `inline`, `constexpr`, `const char* const`) fails with its file and line, and the Global `nav.screen` rows must be exactly the screen buttons of `SCREENS` (a duplicate, an extra or an unreadable row fails). The self-test plants an unreadable declaration instead of a ui-map row and runs on small fixture headers it writes into a scratch directory, so a renamed firmware label cannot turn it red. `--check` also fails on a key-row array that `SCREENS` does not list and on a screen chapter with no block. Labels that change at run time (File IMPORT, Instrument `F1`/`F6`/`F7`, Sample load `F7`/`F8`) are written by hand under a `<!-- manual-tables: hand -->` note next to the block.

### M2-3: sync v2

- **Section markers.** Each "How it works" and "Try it" section, and any section that states behaviour, carries `<!-- sync: src=<path>[,<path>] spec=<docs/specs/..> qa=<QA-ID>[,..] -->`.
  - The 78 `<!-- src: -->` comments migrate into these markers, and the old form is then refused.
  - The chapter-level `<!-- manual-sync: <sha> -->` stays until every section has a marker, then goes.
- **`docs/manual/sync.lock`** is generated, one line per `chapter#section`: each cited path with its git blob hash at the last acknowledgement.
  - `tools/manual_check.py --ack <chapter>#<section>` rewrites that section's line.
  - `--ack-all` writes every line from the tree: for the first migration, and for the lock after a merge conflict in it.
  - `--ack-path <file | QA-ID>...` acknowledges that item in every section that cites it (review fold: one file such as `bindings.cpp` is cited by many sections).
- **The branch gate**, in `ci.sh host`:
  - On a branch, take the files changed since `git merge-base HEAD dev`. Each section that cites a changed file must have changed on the branch too, either the section text or its lock line. Otherwise the gate fails and names the section and the file.
  - On `dev` (no merge base, or HEAD is `dev`) it only warns.
  - `O3OS_MANUAL_BASE` overrides the base, as `O3OS_QA_BASE` does for E2E-3.
- **`--stale`:**
  - also counts removed and changed QA rows, not only added ones;
  - runs in `ci.sh host` as a warning report, never a failure;
  - the gate above is the failing check.
- **Map the 5 chapters:**
  - `cheat_sheet.md` → every screen file;
  - `quick_start.md` → global;
  - `computer_tools.md` → no catalogue file, sync by `src=` (platform/mk66/tools);
  - `install.md` → no catalogue file, `src=` docs/manual/install sources;
  - `glossary.md` → none, exempt with a reason in `CATALOGUE`.
- **Acceptance:**
  - self-tests for a cited file changed without an ack (fail on a branch, warn on dev), an ack that clears it, a migrated `src:` comment, and a removed QA row found by `--stale`;
  - the lock is generated from the current tree, and all of today's sections are acknowledged once with `--ack-all` in this slice's commit.

**Built (2026-10-18), as it differs from the text above.**
- A section is the text under one heading, named by its GitHub anchor (`perform.md#store-a-scene`). The marker sits right below the heading; one per section; each of `src=`, `spec=`, `qa=` is optional but one is needed. A path may be a file or a folder (a folder hashes its tracked files). A "How it works" or "Try it" section without a marker fails `manual_check --strict`, and so does any path or QA ID that does not exist.
- The lock line holds `item=hash` for every cited path (git blob hash, 12 hex digits) and every cited QA row (a hash of its Function, How and Expected cells; the Status, Sim and Unit cells change whenever a test passes, so they are left out).
- `manual_check --strict` also fails on a marked section with no lock line and on a lock line with no marked section; `--ack` of a section that is gone drops its line. Sections whose sources moved since their line are information (`--stale`), never a failure.
- The gate is `manual_check --gate` (it needs git; `--base <branch or commit>` and `O3OS_MANUAL_BASE` override the base, by its merge base with HEAD, and a name that is not a commit fails the gate). It counts the working tree against the base, so it also works before the commit. `ci.sh host` runs `--strict`, `--gate` and the `--stale` report in `manual_check_run`. (GitHub CI, which set the base for a pull request, was dropped 2026-11-18.)
- **Review fold (2026-10-18).** A failing gate groups the sections and prints one `--ack-path` command; `--ack-path` and a repeatable `--ack-all` exist (above); a conflict in `sync.lock` is named with its way out (take either side, re-run `--ack-path` on the merged tree). A manual page is hashed without its `manual-tables` blocks, so a renamed key does not make the cheat sheet's sections look changed. `app/src/bindings.cpp` stays cited as a whole: a section that describes what a key does depends on the binding, not only on its label, so a narrower source (the screen's `k*Keys` header) would hide a changed action.
- The migration turned the 78 `src:` comments into 68 markers, one per section (prose after the paths stayed as a plain comment), added 9 markers to `computer_tools.md` (5) and `install.md` (4), which have no catalogue file, and acknowledged all 77 sections with `--ack-all`. A `qa=` field was filled from the QA IDs written in the section.
- `--stale` counts added, changed and removed rows (a row that only moved is none of them) and tells a chapter that cites a removed one. Chapters outside the CATALOGUE table are listed as information.
- Because the cheat sheet cites the chapters it repeats, every edit to one of those chapters needs an `--ack` of the cheat sheet section too (or an edit of it): that is the gate working as designed.

### M2-4: walk-throughs as sim scenarios

- Each chapter's "Try it" section is a scenario, `sim/scenarios/manual/<screen>_walk.txt`. Its snaps are the walk-through's pictures.
- `docs/manual/images.txt` gets the form `<name>.png manual/<scenario> <snap>`, and `tools/manual_images.py` renders it. The ctest `o3sim_manual_images` already fails on a stale picture.
- **Scenario glob (coordinate with OS-E2E).** `sim/CMakeLists.txt` globs `scenarios/*.txt` and `scenarios/qa/*.txt`, and `tools/update_goldens.py` knows the same two folders. OS-E2E adds `scenarios/e2e/` and this slice adds `scenarios/manual/`. Whichever lands first widens both places to every subfolder of `sim/scenarios/` (`scenarios/*/*.txt`), test name `scenario_<folder>_<name>`. The other then needs no change.
- Goldens come only through `update_goldens`.
- **Acceptance:** a sample `perform_walk.txt` (handed to C1) renders, and `o3sim_manual_images` passes.

**Built (2026-10-18), as it differs from the text above.**
- **Scenario glob:** nothing to widen. OS-E2E-1a had done it on `dev` (`scenarios/*/*.txt` in `sim/CMakeLists.txt` and `tools/update_goldens.py`), so `scenarios/manual/` was picked up as it was. The ctest name stays `scenario_<file stem>`, not `scenario_<folder>_<name>`, and the file name carries its folder's mark so that stems stay unique (`qa_`, `e2e_`): for this folder it is the suffix, `<screen>_walk.txt`, as the spec and the template always wrote it (`scenario_perform_walk`). Only the comments and docs (`sim/CMakeLists.txt`, `tools/update_goldens.py`, `sim/README.md`, `sim/CLAUDE.md`) name `manual/` now.
- **Manifest and tool:** the form is `<name>.png <folder>/<scenario> <snap>` (`perform_walk_2.png manual/perform_walk walk_2`): a word with a slash is `sim/scenarios/<folder>/<scenario>.txt`, one level; a bare word still means `qa/` and then `sim/scenarios/`; a word that ends in `.txt` is a path from the repository root. Whatever the form, the scenario is a file under `sim/scenarios/` by its real path: an absolute word, a `.`, `..` or empty part, a path of more than one folder, a missing folder or scenario and a symbolic link that leads out are errors, never a picture. A picture is `<screen>_walk_<step>.png` of the snap `walk_<step>`, as the template's example shows, and only a step that changes the screen has one. The self-test of the tool (`tests/manual_tools_test.py`, `manual_tools_selftest`) writes and checks a `manual/` picture and plants 13 bad words (a missing folder, a scenario not in its folder, `..` in four places, an absolute word, a path of three parts, a `.txt` word outside the folder, a link that leads out) and a missing snap.
- **The sample, `perform_walk.txt`:** seven steps from the Perform screen (open it, choose T1 and T2 with `Shift` and the top pads, `Rec` for EDIT then hold pad 16 (LOWPASS, a slot that acts today) and turn the encoder so the value is stored in its row, let go and leave EDIT so that the row stays on and lit, `Shift` + `F1` store, `F8` reset, `F1` recall), a comment above each step that says what the chapter's sentence is, and `assert state screen=PERFORM` / `msg=SCENE_1_STORED` to hold two of its sentences (an assertion ticks no clock, so it moves no golden). It needs `document sound` and `audio on`: the Perform slots are wired to the engine's board, and without one the screen says "no engine: the slots are not wired" (the QA snap `qa_prf_001` shows that state). Nothing is played. The manual's pads count 1 to 48 and `pad` takes 0 to 47: the manual's pad N is `pad N-1` (the header says so). Step 1 has no picture: with no track on, the Perform info line (`perform_screen.cpp`, the `left.assign("no track is on: ...")`) runs into the hint on its right (a latent defect of the screen, not fixed here: no firmware file).
- **C1 is last** (after OS-PERFFX, the backlog's order), so `perform.md` is not touched and the six pictures are not shown yet. An agent that writes a chapter replaces this sample's steps with its own, keeps the shape, and regenerates with `tools/manual_images.py`.

### M2-5: accessibility checks

- Add to `tools/manual_check.py` (D11), failing under `--strict`:
  - an image with missing or empty alt text;
  - a skipped heading level (`#` straight to `###`, or a second `#`);
  - a Markdown table with no header separator row;
  - a link whose text is "here", "click here", "this", "this page" or "link", or a bare URL as its own text.
- Colour-only meaning and SVG contrast stay a review item, not a check. One exception: `tools/manual_diagrams.py` may assert the text/background contrast of its own palette (4.5:1) if that is cheap.
- Today's chapters are fixed in the same slice so that `--strict` stays green.
- **Acceptance:** the self-test plants each fault and sees it fail; `manual_check --strict` passes on the tree.

**Built (2026-10-18), as it differs from the text above.**
- The checks run on every page of `docs/manual/`, the index and the template included, and `manual_check --strict` (already in `ci.sh host`) fails on them.
- Two checks beyond the list, both from the skill's section 8: alt text that is a file name or starts "screenshot of", and alt text of 25 words or more (the skill says under 25; `manual_diagrams` still allows 25 for its own); a table also fails on an empty header cell or a separator with another cell count than the header.
- `-v` lists the prose lines that name a colour, for the fact reviewer; it never fails.
- Today's chapters needed two alt texts shortened (panel.md, record.md).
- SVG contrast is not asserted, and it is not cheap: measured against the 4.5:1 target, the panel's F1 to F8 labels (#f4f4f4 on the #8a8a8a soft key: 3.1), its edit-block labels (the same text on #7a7a7a: 3.9) and the screen map's small labels (#cfcfcf on #5e5e5e: 4.2) are below it. The panel is hand-made and the generators are being trimmed by OS-MANUALFIX, so the palette change belongs after it (latent issue, for the maintainer).

### M2-C1 to M2-C5: chapters

Each chapter is one Sonnet doc agent following the `manual-writing` skill, then one Sonnet fact review (a `firmware-reviewer` brief limited to facts). The review checks:
- every claim against the cited code, spec and QA row;
- every same/different claim against the M2-1 map;
- the walk-through scenario steps against the text;
- that no sentence of the official manual is copied.

Order (D9):
1. **C1 Perform.** The model must cover what performing is:
   - live and not written;
   - pad n = slot n % 12 on row n / 12;
   - hold versus LATCH;
   - Shift picks the tracks;
   - which effects act;
   - leaving the screen ends them.
2. **C2:** Record with the looper, Modules plus `module-instrument-fx.md`, and FX (step FX). These are the long concept sections (D3).
3. **C3:** Song and Mixer (deep: the arrangement; routing and sends), Wave (medium) and Sample edit (brief).
4. **C4:** Quick start (the skill's §11 shape: the differences, then the newcomer path), File (brief), the Pattern intro, and Pattern tools (deep).
5. **C5:** every chapter not rewritten above is brought to the template at its tier.

Acceptance for each chapter:
- the D6 order;
- the opening follows D2, with no comparison heading or fixed phrase;
- it explains at its tier (D3): How it works answers live, stored and ends, with a musical example; the key table is not the body;
- terms are defined in the chapter or linked to the glossary (D1), and the D11 rules hold (the M2-5 checks pass once M2-5 exists);
- the word budgets are met;
- its `<screen>_walk.txt` exists and renders;
- the sync markers resolve and are acknowledged;
- `manual_check --strict`, `manual_tables --check` and `manual_diagrams --check` pass;
- `llms*.txt` is regenerated;
- the fact review is APPROVE.

## 5. Evidence wording for the official firmware (D4)

| Our evidence | In the chapter (plain words, skill §9) | Hidden comment |
|---|---|---|
| The map says "same" and our QA row passes | "Works as the official manual describes." | `<!-- og: <map section> + QA-… -->` |
| `verified parity` (observed on an official unit, with evidence) | "Works the same as on the original firmware." | the evidence reference |
| The map says "different" | say what differs, in the opening or the step | `<!-- og: <map section> -->` |
| new | "New in track3r-os." | `<!-- og: new -->` |
| unclear | "Not checked against the original firmware." | `<!-- og: open -->` |

A description in the official manual is evidence of documented behaviour, not `verified parity`. Only an observation on an official unit, with evidence, earns `verified parity`.

## 6. State for implementer

- **All slices:**
  - Read §2, §3, your slice in §4, and the `manual-writing` skill (after M2-0).
  - Branch `docs/OS-MANUAL2-<slice>`, worktree under `.claude/worktrees/`, and merge into `dev` when green.
  - Docs-only slices skip the build. Run the four manual checks, plus `tools/ci.sh host` once for the tool slices (M2-2, M2-3, M2-4).
- **M2-0:** done (template and skill; see §4).
- **M2-1 files:** new `docs/ui/official-manual-map.md`. Web reads only, nothing downloaded into the repo. Done when the M2-1 acceptance holds. Report the "missing" list to the orchestrator in at most 15 lines.
- **M2-2 files:** new `tools/manual_tables.py`; markers in `docs/manual/*.md` and `cheat_sheet.md`; a ctest `manual_tables_selftest` (label `python`) registered next to the other manual tool tests (next to `manual_tools_selftest`, `tests/CMakeLists.txt:395`); `manual_tables_check` called in `tools/ci.sh` beside `manual_llms_check`.
- **M2-3 files:** `tools/manual_check.py` (`--ack`, `--ack-all`, gate, `--stale` fix, `CATALOGUE` additions), new `docs/manual/sync.lock`, `tools/ci.sh` (`manual_check_run` gains the gate and the `--stale` report), and all chapters (marker migration).
- **M2-5 files:** `tools/manual_check.py` (the checks and their self-test cases in `tests/manual_tools_test.py`), plus whatever chapters fail them.
- **M2-4 files:** `sim/scenarios/manual/perform_walk.txt`, `docs/manual/images.txt`, `tools/manual_images.py`, the scenario glob (`sim/CMakeLists.txt`, `tools/update_goldens.py`, §4 M2-4 note), `sim/goldens.txt` via `update_goldens`.
- **Chapter slices:**
  - the chapter file, its `sim/scenarios/manual/<screen>_walk.txt`, its pictures, `docs/manual/images.txt`, `sync.lock` (`--ack`) and `llms*.txt`;
  - the brief gives the agent its block of the M2-1 map, its catalogue file, the ui-map row and the audit facts above. It gets neither the whole spec nor CLAUDE.md.
