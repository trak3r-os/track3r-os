# Using, adapting or dropping the agent set-up

The repository ships the set-up the maintainer works with in Claude Code: `CLAUDE.md` files, `.claude/agents/`, `.claude/skills/`, `.claude/hooks/` and `.claude/settings.json`. You do not need any of it to build, test or contribute (`docs/toolchain.md`, `CONTRIBUTING.md`). This page says what each part does, what is tied to the maintainer's machines or habits, and what to change.

## What is the project's, what is the maintainer's

| Part | Kind | Notes |
|---|---|---|
| `CLAUDE.md` (hard rules, layout, standards, branches, decisions) | project | The rules apply to every contributor, with or without an agent. |
| `app/ audio/ formats/ midi/ model/ module/ seq/ sim/ ui/` `CLAUDE.md` | project | Entry points and scoped tests per area. |
| `docs/maintainer-workflow.md` | maintainer | Roles, lanes, the CI lock, core pinning, the remote build machine. Imported by the last line of the root `CLAUDE.md`. |
| `.claude/ops-params.ini` | maintainer | The operating parameters (lane count, CI rules and job counts, agent context caps, backlog limits), printed by the SessionStart brief. Set your own numbers. |
| `.claude/hooks/guard_flash.py` | project safety | Refuses uploaders, debug probes and raw block-device writes. Keep it. |
| `.claude/hooks/syntax_check.py` | tooling | Syntax-checks a C/C++ file after an edit, through `compile_commands.json`. Needs a host build once: `cmake -S . -B build-os-host -DO3OS_SANITIZE=ON`. |
| `.claude/hooks/verify_gate.py` | habit | Asks an agent to run a test before it stops, once. |
| `.claude/hooks/code_nav_guard.py` | habit | Refuses a recursive grep for a C++ symbol (use the LSP) and a shell edit of a C/C++ file (use Edit or Write). Opinionated: remove its entry in `settings.json` if you prefer grep and sed. |
| `.claude/hooks/session_brief.py` | habit | Prints the branch, worktrees and the `Next` section of `docs/STATUS.md` at session start. |
| `.claude/skills/fw-build`, `fw-gate-check`, `dsp-port`, `rt-optimize`, `tmod-perf`, `sd-module`, `manual-writing`, `radar` | project | Technical procedures; they assume this repository, not a machine. |
| `.claude/skills/fw-feature`, `handoff`, `decide`, `hw-session` | maintainer | The orchestrator's playbook, session close, decision records and the tester's hardware session. They describe a multi-session set-up (an orchestrator, a tester with the unit, lanes). Read them as an example. |
| `.claude/agents/*.md` | mixed | The role and the rules are the project's; the `model:` line (sonnet or haiku) and a few budget figures are the maintainer's choice. |
| `.claude/settings.json` | shared | Permission allow and deny lists and the hook wiring. No absolute paths (`$CLAUDE_PROJECT_DIR`). |

## What to change on your machine

1. **Build parallelism.** Edit `.claude/ops-params.ini` (lanes, job counts, limits). `tools/ci.sh` takes `O3OS_CI_JOBS` (default 4). The maintainer's `taskset -c 2-19`, 18 jobs and CI lock are for a 20-thread machine; ignore them, or set your own numbers.
2. **Remote CI.** Optional. `tools/remote_ci.sh` needs `O3OS_REMOTE_CI=<ssh-host>:<root>` in your local environment (`.claude/settings.local.json`, which is git-ignored; start from `.claude/settings.local.json.example`). Without it the script exits 3 and you run `tools/ci.sh` locally, which is what the rules say.
3. **Toolchain.** The Arm GNU toolchain lives outside the repository: `docs/toolchain.md`.
4. **LSP.** The set-up assumes clangd on `build-os-host/compile_commands.json` (`.clangd`). Without it, `code_nav_guard.py` will send you to a tool you do not have: remove its entry from `.claude/settings.json`.
5. **Models.** Change `model:` in an agent's frontmatter. The agents work with any model that follows instructions; the maintainer chose cheaper ones for mechanical work.
6. **Roles.** If you work alone in one session, ignore the orchestrator, tester and lane vocabulary. The rules in `CLAUDE.md` (levels A/B/C, evidence labels, verify before you say done, the git rules) are the part that matters.
7. **Hardware.** HIL scenarios and the tester role need the physical unit and a test SD card. Without them, the simulator (`sim/`) and `tools/ci.sh` cover what can be covered; say in a PR that a hardware check is still pending.

## Dropping it

- Without agents: delete nothing; the files are inert if you do not use Claude Code.
- With Claude Code but without the maintainer's way: delete the `@docs/maintainer-workflow.md` line at the end of `CLAUDE.md`, and remove the hooks you do not want from `.claude/settings.json`. Keep `guard_flash.py`.

## What never leaves the maintainer's machine

`.claude/settings.local.json` (the remote host, local permissions), the agent's memory and the `.claude/worktrees/`, `build*/` and `.toolchains/` directories are git-ignored. If you add your own, keep it that way: no host names, user names, paths or tokens in git.
