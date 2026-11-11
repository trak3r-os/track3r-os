# Project-local GNU Arm toolchain

The project-local toolchain is Arm's official **Arm GNU Toolchain 15.3.Rel1** release for an x86_64 Linux host and the `arm-none-eabi` bare-metal target. No system packages were changed.

- Official package URL: <https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz>
- Arm-published SHA-256 file: <https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz.sha256asc>
- SHA-256: `563bebb2b97d53382b956d6ee1fe61e2cae26699901417234a37df505ef9b5fa`
- Extracted directory: `.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi/`
- Executables: `.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi/bin/`

Verified locally:

| Executable | Version |
| --- | --- |
| `arm-none-eabi-gcc` | 15.3.1 (Arm GNU Toolchain 15.3.Rel1, Build arm-15.149) |
| `arm-none-eabi-g++` | 15.3.1 (Arm GNU Toolchain 15.3.Rel1, Build arm-15.149) |
| `arm-none-eabi-objcopy` | GNU objcopy 2.45.1.20260126 |
| `arm-none-eabi-size` | GNU size 2.45.1.20260126 |

Use it by adding the local `bin` directory to `PATH`, for example:

```sh
export PATH="$PWD/.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
```

## Build lanes, CI lock and cache (maintainer, 2026-10-16)

- A lane build outside the CI lock runs at `-j4`; `tools/ci.sh` takes its count from `O3OS_CI_JOBS` (default 4, the lane count). The 18-job total and cores 0-1 stay free.
- Whoever holds `flock "$(git rev-parse --git-common-dir)/../.claude/worktrees/.ci.lock"` (a full `tools/ci.sh` run, or an `mk66`/`qemu` run) uses `O3OS_CI_JOBS=10`; that knob sets both build and ctest parallelism.
- ccache covers every mode (host, qemu, mk66). The cache limit is 30 GB (`ccache -M 30G`); the hit rate is about 80%. `tools/ci.sh` sets `CCACHE_BASEDIR` to the repository root so worktrees share the cache.
- Verifier: run the host, qemu and mk66 stages as separate calls, so one failure does not hide the others. After a fix, re-run only from the failed stage.

## Remote build machine (maintainer, 2026-10-19 and 2026-10-21)

- A second machine runs CI for every agent: 16 threads in four turns of 4 (the owner's choice, 2026-10-23; before: two turns of 8 since 2026-10-22, one turn of 16 earlier), at `nice -n 10`: four runs go at once, a fifth waits for the first turn that frees (its wrapper `o3ci.sh` takes the first free of `.ci.lock.0` to `.ci.lock.3`, with `O3OS_CI_JOBS=4`). It is the first choice for every `tools/ci.sh` run of a branch or of `dev`: `host` (full or scoped), `e2e`, `qemu`, `mk66`. The edit-compile loop goes there too (maintainer, 2026-10-22): `tools/remote_ci.sh --scope changed host`, with only the cmake configure kept local for the LSP and the hooks. Image builds and their gate stay local, because the images live here.
- `tools/remote_ci.sh [--slot <name>] [--scope changed] <host|e2e|qemu|mk66> [mode...]` sends the working tree as it is (committed or not: a snapshot commit on top of HEAD, made with a temporary index, so the branch and the index are untouched), runs the modes there and prints the ctest summary and `rc=<n> secs=<n>`; the full log comes back to `build-remote/` (untracked), where the linker usage table gives FLASH, SRAM_U and SDRAM_STATIC for `mk66`. Its exit code is the run's.
- The machine is named by `O3OS_REMOTE_CI=<ssh-host>:<root>`, set locally only (the `env` block of `.claude/settings.local.json`); the host, the user and the path never go into git. Exit 3 (not set) or 255 (ssh failed: the machine is off or unreachable) means run locally under the rules above.
- `tools/remote_ci.sh goldens` (2026-10-22) builds `update_goldens` there (`O3OS_GOLDEN_ONLY=<name>` narrows it, as locally), brings back `sim/goldens.txt` with the PNGs of the entries that changed in `build-remote/goldens-<slot>/` (`png/<scenario>/`, `gallery/`, the list in `changed.txt`), and puts the file in the working tree when the local one is still the one that was sent. Look at the PNGs before committing.
- `tools/remote_ci.sh fingerprints` and `host-off` (OS-RATE, 2026-10-23): `fingerprints` runs `update_audio_fingerprints` there and brings back `tests/audio_fingerprints/*.json`, `fingerprint_diff.md` and the WAVs in `build-remote/fingerprints-<slot>/` (it updates the tree when the local files are the ones that were sent; read the diff before committing). `host-off` is the host build with `-DO3OS_RATE_44100=OFF` (the pre-1.0 clock) in its own build directory; narrow it with `O3OS_CI_TESTS`, e.g. `^unit_(audio|seq)$`.
- The slot is the remote worktree, by default this worktree's folder name: a lane keeps its slot, so its builds stay incremental. The slots `dev` and `gate` start fresh and are the orchestrator's.
- Overlapping runs (OS-OPS-CIISO, 2026-11-11): `tools/remote_ci.sh` holds a local `flock` per slot (`remote-ci-slot-<slot>.lock` in the git common dir) for the whole run, so a second run of the same slot prints `remote_ci: waiting for slot <slot>` and starts when the first ends; runs of different slots go at once. Two runs of one commit that push `refs/ci/<sha>` together no longer fail with 255: a push that loses the race counts as done when the ref already holds that commit.
- CI speed (OS-OPS-CISPEED, 2026-11-11): (1) green cache: after rc=0 of a full `host`, `host-off`, `e2e`, `qemu` or `mk66` run (no `--scope`, no `O3OS_CI_TESTS`) `remote_ci.sh` appends `<key> <mode> <date> <slot>` to `ci-green.log` in the git common dir (key = sha1 of the snapshot tree, the mode and the env sent: `O3OS_CI_LONG`, the QANET variables); a later run of the same tree prints `<mode> already green for this tree (...); skipped` and, when every mode is skipped, exits 0 without ssh. `O3OS_CI_FORCE=1` runs anyway; the key does not cover the machine's own state (wrapper, toolchain), so run with it after a remote toolchain change. docs/ stays in the tree and the key, because tests and gates read docs/manual, docs/qa, docs/specs and docs/formats. (2) Every run ends with `remote_ci: wait=<s> run=<s>` (wait = slot lock plus the machine's queue, run = the remote's `secs=`). (3) With `O3OS_REMOTE_JOBS` unset the job count is automatic: the script counts the machine's busy turns (`flock -n` on `.ci.lock.0` to `.3`, read-only) and takes `nproc/(busy+1)`, at least 4 (16 threads: 16, 8, 5, 4, 4), and prints it as `jobs=<n>`; a number you set wins.

### Which run when (OS-CISPEED, 2026-10-23)

| Need | Command | Runs |
|---|---|---|
| Full run | `tools/remote_ci.sh host mk66 qemu` | the job count is automatic (`O3OS_REMOTE_JOBS=<n>` is a ceiling on it, OS-OPS-JOBSCAP), over the turn's default; `O3OS_CI_JOBS` is not forwarded |
| One test while you iterate | `O3OS_CI_TESTS=<regex> tools/remote_ci.sh host` | only the tests whose name matches (`ctest -R`), any label, no scope; the build is still incremental |
| Your change, before review | `tools/remote_ci.sh --scope changed host` | the area labels your files map to plus `static`; the base is the merge-base with `dev` (`main` when there is no `dev`; `O3OS_CI_BASE_BRANCH` overrides); the tests labelled `slow` and `e2e` are left out |
| What a scoped run leaves out | | host tests only: `e2e`, `slow` and `qanet` (unless the change is QANET's); target tests (`mk66`, `qemu`) always run whole, they take about a minute and hold the budgets; the `mk66` mode skips the tone and bench-archive images |
| A change that is risky, or the end of a branch | `tools/remote_ci.sh host` | every test except `e2e`, the `slow` ones included |
| End-to-end journeys of one item | `tools/remote_ci.sh e2e` | the label `e2e` without the `long` shards (the random user's 8 shards, 5 to 13 minutes each) |
| The orchestrator's batch run and the image gate | `O3OS_CI_LONG=1 tools/remote_ci.sh e2e`, then `host`, `qemu`, `mk66` | everything |

- Labels (`cmake/O3osLabels.cmake`): `slow` is a test of over a minute (the QANET self-tests, `qanet_random_user_short`, `o3sim_manual_images`); `long` is an `e2e` test of 5 minutes or more. A new test that takes that long gets the label in its rule. A scoped run leaves them out even for an `app/`, `audio/` or `seq/` change (the QANET probe and rules self-tests among them), and one that changes only the manual's pictures no longer runs `o3sim_manual_images`: the full `host` at the end of the branch, before review, runs them all.
- `O3OS_CI_LONG=1` and `O3OS_CI_TESTS` are passed through by `tools/remote_ci.sh` (letters, digits and `_ . | ^ $ * + ? ( ) -` only in the regex).
- Cost data: ctest starts the tests that cost most first. A fresh build directory (the `dev` and `gate` slots, or a local run) has no record of them, so `tools/ci.sh` keeps `CTestCostData.txt` per mode in `~/.cache/o3os-ci-cost/` (`O3OS_CI_COST_DIR` moves it) and puts it back before ctest.

## Merging a branch (OS-MERGETOOL)

`python3 -I tools/o3merge.py <ref> -m "<message>"` merges `<ref>` into the current branch (`git merge --no-ff --no-commit`), resolves the generated files itself — `docs/manual/sync.lock` (a three-way merge of the index stages, then `--ack` for the sections whose items both sides changed), `docs/qa/catalogue/README.md` (`tools/qa_cover.py --write`) and `docs/manual/llms*.txt` (`tools/manual_llms.py`) — runs the checks (`manual_check.py --strict`, `qa_cover.py --check`, `manual_llms.py --check`, plus `git diff --cached --check` for leftover conflict markers in any staged file) and commits with your message. A conflict in any other file stops it with exit 2 and the merge left in progress: resolve it by hand, `git add` it, then run `python3 -I tools/o3merge.py --continue -m "<message>"`. `--no-commit` stops with everything resolved and staged; `--allow-detached` permits a detached HEAD, to replay an old merge in a throwaway worktree. A failed check is exit 3, merge left in progress. The end summary lists what was auto-resolved, the re-acked sections, and a "possibly stale after the merge" block (information only): the sections citing something the merged branch changed since the merge base without re-acknowledging it — read those.

## clangd, the LSP tool (OS-CODEMAP)

- The `clangd-lsp` plugin needs the `clangd` binary on the PATH when Claude Code starts. `.clangd` points it at the worktree's `build-os-host/compile_commands.json` (configure it first: `cmake -S . -B build-os-host`); the index goes to `build-os-host/.cache/clangd/`, built once per worktree at idle priority (a minute or two, about 1 GB of memory).
- If every file shows `'atomic' file not found` or `no type named 'uint32_t'`, clang picked a GCC installation without its libstdc++ headers (clangd 18 takes the newest GCC directory). Install that version's `libstdc++-<n>-dev`, or pin the host compiler's version in the user config `~/.config/clangd/config.yaml`: `CompileFlags: {Add: [--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/<n>]}`. `clangd --check=<file>` ends with `0 errors` when it is right.

## The build's compression: `tools/deflate_pack.py` (OS-FLASHDIET A2)

The image stores its flash assets (the built-in modules, the glyph faces, the boot logo, the help text) as raw-deflate streams with a 4 KiB window, decoded on the device by `core::Inflater`; the streams are made at build time by our own encoder, `tools/deflate_pack.py`, not by Python's zlib: zlib's deflate bytes change with the zlib library a machine links (the same input compressed differently by 1.2.11 and 1.3), and the reproducible-image gate must not depend on the build machine's zlib, so the encoder is written from scratch (greedy-lazy LZ77 over a 4 KiB window, dynamic-Huffman blocks with lengths limited to 15, a stored block when smaller, standard library only) and is deterministic across machines, Python versions and runs, at about 100 % of zlib level 9 on those assets (the target is within 4 %). `zlib` appears in the tool only in `--self-test` and `--check-fixture`, which round-trip its streams through `zlib.decompress(data, -12)`, check no distance exceeds 4,096, that two runs are byte-identical and that the length-limited code lengths stay complete, and pin the built-in samples to an absolute byte budget (the zlib level 9 ratio is only reported, so the gate does not depend on the build machine's zlib); ctest `deflate_pack_selftest` runs the self-test with `--check-fixture tests/core_inflate_test.cpp`, which re-makes the fixture that file carries and compares it byte for byte with its committed `kPackZ`, and A1's `tests/core_inflate_test.cpp` carries that fixture, which the Inflater decodes byte for byte.

## The end-to-end battery: `tools/ci.sh e2e` (OS-E2E, `docs/specs/OS-E2E.md`)

- A fourth mode beside `host`, `qemu` and `mk66`: the same host build as `host` (ccache hits it), but ctest runs **only** the tests labelled `e2e` (`cmake/O3osLabels.cmake`). An `e2e` test also carries an area label (`app` for the simulator's), so `ci.sh` accepts it; every other run, `ci.sh host` and the GitHub workflow included, leaves the label out (`ctest -LE '^e2e$'`), and `O3OS_CI_SCOPE=changed` does too.
- It is on demand and a release step: **`tools/ci.sh e2e` passes** is a line of the release checklist, next to the full `tools/ci.sh` and `fw-gate-check`; a failing journey blocks the release. Nothing in it touches the firmware or a unit.
- Today the battery is the legibility sweep of the whole gallery (`legibility_<i>of6`, `o3sim --legibility`, see `sim/README.md`). Later slices add the journeys.
- Output, in `build-e2e/<YYYY-MM-DD>/` (untracked, `build*/` is in `.gitignore`): `report.md` (the verdict, the pass/fail table of ctest with the times, the findings of each failing test), `screens.png` (a contact sheet of every screen and theme, for a look once per release), `legibility.txt` (the full output of the sheet run), later the journeys' WAV files. A second run on the same day overwrites them.
- Run it under the CI lock like any full run: `flock "$(git rev-parse --git-common-dir)/../.claude/worktrees/.ci.lock" env O3OS_CI_JOBS=10 nice -n 10 taskset -c 2-19 tools/ci.sh e2e`.
