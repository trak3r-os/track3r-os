---
name: fw-feature
description: Orchestrator playbook to build one feature, fix or refactor of the Community OS end-to-end with subagents - brainstorm, maintainer questions, spec, implement, verify, review, fold, commit, document. Use for every work item of docs/roadmap/BACKLOG.md and any change to the OS tree. Argument - the item ID(s) or a short description.
---

> **Maintainer's workflow.** This skill is the orchestrator's playbook of the maintainer's multi-session set-up (lanes, subagents, a tester with the unit). It is published as an example: `docs/agent-setup.md` says what to adapt.

# Feature pipeline (orchestrator)

The orchestrator owns decisions, sequencing and commits; subagents do the work. Open every progress message with `phase k/8 <ID>` and keep it to one or two lines (token economy). Never skip verify or review for code changes of Level B or C.

**Orchestrator economy.** The orchestrator decides, briefs, reads capped reports, commits and merges. It does not review: it never reads diffs, code, build logs or PNGs itself (at most two golden PNGs when the engineer flags a visual change) and does not explore code it can delegate; it reads verdicts (15 lines at most), exit codes and ctest summary lines. When a verdict is not enough to decide, it asks the reviewer for another pass instead of looking itself.

**Orchestrator routine (maintainer, 2026-10-16): move the queue, nothing else.**
- **Keep-alive.** At the start of a session the orchestrator creates one recurring `CronCreate` ping every 30 min on off minutes (e.g. `17,47 * * * *`): a recurring job can fire up to half its period late, so 30 min keeps every gap under the 1 h prompt cache. The ping only dispatches: if a lane is free and the queue has work, brief the next item; otherwise answer one line. It is session-only: recreate it after a restart.
- **Dispatch only.** The engineer merges `dev` into its branch before the merge (no rebase; on a QA id clash dev wins and the branch renumbers), runs the gates and pushes. The orchestrator then runs `python3 -I tools/land.py <branch>` (merge in the `land` worktree, CI on the merge result, ff-only dev, push; `--dry-run` shows the titles) with no reading. `doc-keeper` updates BACKLOG and STATUS rows from the verdict lines, in batches.
- **Manual loop (OS-MANUALTOOLS).** At each doc-keeper batch the orchestrator runs `python3 tools/manual_check.py --stale`. If a chapter lists changed QA IDs (or a missing sync marker), it sends one Haiku manual-update agent with that list and the `manual-writing` skill ("Update mode"); the agent edits only those sections, bumps the chapter's `manual-sync` sha and reruns `tools/manual_llms.py`. Empty list: nothing to send. `tools/ci.sh host` fails when `llms*.txt` are stale.
- **Defaults.** A choice an agent makes that the spec does not settle goes into the spec as "default, to confirm by the maintainer"; `doc-keeper` lists them in STATUS "To confirm" as one table for the maintainer.
- **Unit-gated items.** Render or audio-path items whose CPU must be measured on the unit stop at the gate on their branch; a bench image (branch merged with `dev`, through `fw-gate-check`) goes to the tester.
- **Recovery.** If the orchestrator session ends, any session resumes from the STATUS lanes table: each lane's branch is pushed and its worktree holds the rest.
- **Sessions.** Two standing sessions: the orchestrator (this routine, lanes per `ops-params.ini`) and the tester, who installs gated images on the unit and runs the HIL scenarios when the orchestrator asks (a standing GO: no per-image go; publishing stays the maintainer's). A spec or backlog session runs only when the maintainer opens one.
- **Remote build machine.** Every agent sends its `tools/ci.sh` runs there with `tools/remote_ci.sh` (`docs/toolchain.md`, "Remote build machine"): engineers their scoped `host` and the `mk66` sizes, the verifier the full runs. The local CI lock is for image builds and gates, and for when the remote is unreachable (exit 3 or 255).
- **Lane builds.** In a plain `build-os-host` without the Arm toolchain on PATH, `unit_rest` fails on the `module_builtin*` tests; `tools/ci.sh` is the reference.
- **Lanes (count: `[lanes] engineers` in `.claude/ops-params.ini`; CI on the remote build machine; the maintainer's latest count wins).** The lanes split M/L, S and bug work (the fast lane, below), with files that do not overlap; never two Level C items at once. Each lane is a fixed warm worktree with one owner agent at a time, named in the STATUS lanes table; the verify worktree is never a lane. Reviewers read with `git show` / `git diff` and never touch the main checkout or a lane worktree.
- **Bug fast lane.** A Level A/B bug row (OS-BUG-*) is its own spec: the row's cause, file:line leads and expected behaviour are the brief; phases 1-3 are skipped. The engineer writes the failing test first (a host test or a sim scenario: it is the row's regression test), then the fix; bugs of one area go as a batch, one commit per ID; one reviewer; each ID merges when green. A bug that turns out to need a product decision or touches Level C ground leaves the fast lane for the normal phases.
- **QA id blocks.** The brief reserves a block of QA catalogue ids per lane (e.g. lane A `QA-MOD-110..120`, lane B `QA-PAT-250..260`), so parallel lanes never collide.
- **Level C risk review at spec time.** The risk review is briefed after the spec's questions table has gone to the maintainer (see Spec questions), so the engineer starts with the hazard list; the independent review still follows the implementation.
- **CI by SHA.** For a merge, scoped tests + `mk66` + APPROVE are enough; one full `tools/ci.sh` per batch (by the verifier, in the verify worktree) and before every image. Record `ci-green: <sha>` in STATUS and never re-run CI on code identical to a green SHA (`fw-gate-check` step 5).
- **Pipeline.** When an engineer reports "green, pushed", brief its reviewer and the verifier in parallel; for Level A/B brief the engineer's next item in the same message. For Level C the engineer may start the next slice on the same branch while the review runs, and folds the minors afterwards. A free lane takes the next S item instead of waiting on the CI lock.
- **Verify stages.** `build-verifier` runs the host, qemu and mk66 stages as separate calls; after a fix it re-runs only from the failed stage.
- **CI break.** A trivial, isolated CI break (one obvious edit, one test proves it) is fixed by the orchestrator on a `fix/` branch, landed with `tools/land.py`; no lane, no agent.
- **Spec questions.** As soon as a spec exists, every maintainer question goes to the maintainer in one table, before the risk review is briefed.
- **Agent reuse.** Reuse an engineer only for related items and below the context caps in the `[agents]` section of `.claude/ops-params.ini`; otherwise brief a fresh agent with a short written state (the spec's "state for implementer" section).
- **No time estimates.** Report the phase (`k/8`) and the blocker instead.

## Risk level first (CLAUDE.md)

- **Level A** (typo, docs, isolated tooling, trivial local fix): scope note, the fix, the relevant check, self-review. Phases 1-3, 6 and 7 are not needed.
- **Level B** (feature, refactor, ordinary bug fix): short written intent and acceptance criteria, implementation, host tests plus the target build when relevant, one independent review.
- **Level C** (persistent formats, firmware update, boot or storage formatting, hardware safety, ISRs and the audio path, concurrency, published ABIs, irreversible compatibility): every phase below, with maintainer decisions where required and hardware verification.

| Phase | Who | Model | Output |
|---|---|---|---|
| 1 Brainstorm | orchestrator (+ `Explore`, `radar-researcher` only if needed) | opus / haiku / sonnet | what exists, what is unknown, candidate approaches |
| 2 Questions | orchestrator to maintainer | - | maintainer decisions recorded in the spec (or item parked) |
| 3 Spec | orchestrator; `Plan` for M/L | opus / sonnet | `docs/specs/<ID>.md` |
| 4 Implement | `firmware-engineer` (own worktree) | sonnet | change + scoped build/test numbers, docs rows, goldens |
| 5 Verify | `build-verifier` (scoped host tests, `mk66` when the change reaches the target; one full CI per batch) | haiku | exit code, ctest summary, sizes |
| 6 Review | `firmware-reviewer`: one for Level A/B, a risk review plus one for Level C; in parallel with 5 | sonnet | ranked findings + verdict |
| 7 Fold | same engineer via SendMessage | sonnet | every finding fixed or refuted; 2 rounds at most |
| 8 Integrate | orchestrator, then `doc-keeper` | opus / haiku | commit, `tools/land.py`, STATUS/BACKLOG |

The project agents take their model from their frontmatter. Pass `model` explicitly only to built-in agents (`Explore`, `Plan`, `general-purpose`), which otherwise inherit the orchestrator's model.

## 0. Preconditions
- `git status --short` is clean on the integration branch (`dev`). Run `ListAgents`: another session may share the checkout; agree file ownership before touching shared files.
- Read the backlog row or work item: behaviour, evidence, format impact, risk, dependencies. Dependencies must be merged first.
- One item per branch: `feat/<ID>-<slug>` (or `fix/`, `chore/`, `docs/`). Exception: small (S) items of the same area go as a batch on one branch, one commit per ID, with one brief. Each ID merges into `dev` as soon as its review is APPROVE and its verification is green, without waiting for the rest of the batch; the branch then takes `dev` again for the next IDs. One full CI per batch. Independent items run in parallel, each in its lane's worktree (below); `.toolchains` is symlinked in by the project settings. Items touching the same files run sequentially.
- **Lane worktrees are permanent:** `.claude/worktrees/lane-a` (M/L), `lane-b` (S batch) and `lane-bugs` (bug fast lane), made once from `origin/dev` (lane count per `.claude/ops-params.ini`; unused lanes stay idle). An item is a branch switched inside its lane (`git switch -c <branch> origin/dev` once the previous one is merged); the brief names the lane path; engineers are never started with `isolation: "worktree"`. The lane keeps `build-os-host/` and its clangd index (`build-os-host/.cache/clangd/`): builds and the LSP index stay incremental, where a new worktree costs a fresh configure, a cold build and a full index (about 6 CPU-minutes). Reviewers read with `git show` / `git diff` and use the LSP tool on the main checkout's files, so they need no worktree.
- Any other agent that builds or measures runs in its own worktree. Never merge into a checkout while another agent builds or measures from it. After merging: delete the temporary branch; remove a temporary worktree (`git worktree remove <path>`), but keep a lane worktree, and remove only its other `build*/` trees.
- Builds and tests always run under `nice -n 10 taskset -c 2-19` (cores 0-1 stay free for the system), 18 jobs in total at most. Lane build and ctest jobs outside the lock: `ops-params.ini`. The lock holder (full `tools/ci.sh`, `mk66` or `qemu`, under `flock "$(git rev-parse --git-common-dir)/../.claude/worktrees/.ci.lock"`) uses `O3OS_CI_JOBS=10`, which sets build and ctest parallelism. CI runs go to the remote build machine first (`tools/remote_ci.sh`, all its threads, its own lock). Details: `docs/toolchain.md`.

## 1. Brainstorm
Short and cheap: what the item needs, what already exists (one `Explore` call with a word cap, not file dumps), what is unknown, 1-3 approaches with cost (flash, SRAM, CPU) and risk. Unknown outside library or prior art: skill `radar`. Mark every behaviour as `verified parity` (with its evidence), `new Community OS behaviour` or `open research question`; never claim parity without evidence.

## 2. Questions (batched, once)
Anything only the maintainer can decide (behaviour without evidence, UX choices, licences, format changes) goes to the maintainer in one batch: as many questions as the item needs, in one table, recommended option first, plain tables for big triages. If the maintainer is away, do not invent: park the item in `docs/STATUS.md` under "Blocked on maintainer" with the exact questions and take the next item. Cross-cutting architecture questions go through skill `decide` (ADR) instead. Local, documented, testable, reversible details are the orchestrator's to decide; record the assumption.

## 3. Spec - `docs/specs/<ID>.md`
Goal, decisions (with source or maintainer answer), behaviour, UI entry points and labels, parameters with ranges and defaults, persistence (file, fields, version bump, old-version loader, round-trip test), MIDI/clock and real-time interactions, edge cases, out of scope, **Done when** (checkable by build, tests, simulator or reading code), hardware-only checks, byte budget (flash and SRAM delta, justified if large), CPU cost per block when the feature adds per-block work (`docs/architecture/cpu-budget.md`). S items: 10-15 lines (for a batch, one scope note per ID in the brief); `Plan` only for M/L items, and paste its file list into the spec.

## 4. Implement
Brief the `firmware-engineer` with a packet, not a spec dump: ID, worktree path, risk level, Done-when list, file scope with coordinates (the `build-map/` lines of the keys, screen and IDs involved, from `python3 -I tools/code_map.py`, and the file:line leads of the row: the engineer starts editing, not searching), byte and CPU budget, the 2-4 spec sections that apply (path and line ranges) and, on a fold, the findings. Do not paste CLAUDE.md or the agent's own rules: it already has both. Keep every per-task word in the prompt, never in the agent definitions, so their fixed part stays prompt-cached. Hot paths follow skill `rt-optimize`; modules follow `sd-module` and `tmod-perf` (name the skill in the brief when it applies).

## 5. Verify
While iterating, the engineer runs scoped tests only (`ctest -R` or `O3OS_CI_SCOPE=changed`). When it reports green, `build-verifier` checks the ID in its worktree (scoped host build and tests, plus `mk66` when the change reaches the target, on the remote build machine with `tools/remote_ci.sh`) while the review runs; if a fold changes code, it checks once more. One full `tools/ci.sh` per batch and before every image, not per ID. The orchestrator reads only exit codes and ctest summary lines.

## 6. Review (angles)
`firmware-reviewer` on the diff, read-only, no builds. Level A/B, whatever the size: one reviewer covering correctness, real-time safety and cuts (over-engineering, flash waste, `rt-optimize` rules on hot paths). Level C: a risk review plus one independent reviewer, each with its angle - (a) correctness and real-time/ISR safety, (b) formats, compatibility and failure modes. No three-angle reviews. Findings: 10 at most, `path:line - defect - failure scenario - fix`, ranked, plus a verdict, 250 words at most. Reviewers never check out, switch or merge in the main checkout; the `git_guard.py` hook enforces it for every subagent (read with `git diff dev...origin/<branch>` or `git show origin/<branch>:<path>`).

## 7. Fold
Every BLOCKER or MAJOR goes back to the same engineer by SendMessage, which keeps its context. Never spawn a new engineer for a fold: it pays the cold start again. Each finding ends **fixed** (with evidence) or **refuted** (with a concrete reason the orchestrator accepts). Two rounds at most; then stop and report to the maintainer with the open findings.

## 8. Integrate
The orchestrator reads the exit code and ctest summary line of the final full run (the `tools/land.py` run on the merge result, or the verifier's) before claiming anything; the Stop hook, when installed, enforces this. Commit:
```
feat(<area>): <imperative summary> [<ID>]

<what and why, 2-6 lines; format or version changes called out explicitly>
Flash +<n> B, SRAM +<n> B. Needs HW verification: <items or none>.

Co-Authored-By: <attribution line from the session's system reminder>
```
`python3 -I tools/land.py <branch>...` lands on `dev` once verify and review pass (never `git merge` in the main checkout; `tools/o3merge.py` only for merging `dev` into a branch). Stage explicit paths only (never `git add -A` or `git add .`). Pushing your own verified feature branch is allowed; `main` moves only by fast-forward to a green, reviewed `dev` (`git push origin dev:main`), never by a direct commit; never force-push. The engineer has already written the QA catalogue rows (`docs/qa/catalogue/<screen>.md`), ui-map lines and HIL steps on the branch; `doc-keeper` updates only `docs/STATUS.md` (ID, done / done-needs-HW / blocked, commit, flash delta) and the backlog row.

## Definition of done
Spec with Done-when (Level B/C) - build exit 0 with no warnings - ctest passes (target layers when relevant) - reviewer APPROVE or every finding folded - formats versioned and backward-loadable - STATUS and backlog updated - QA catalogue rows added - hardware-only checks listed for the next `hw-session`.
