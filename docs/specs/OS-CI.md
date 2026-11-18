# OS-CI — GitHub Actions for `os/` (maintainer decision, 2026-10-11)

Status: specified 2026-10-11. **Superseded 2026-11-18 (maintainer, OS-RELGHA):** there is no GitHub CI; `.github/workflows/ci.yml` is deleted and the gate is `tools/ci.sh` run locally. Only `release.yml` remains (a pushed tag builds and publishes the release). The text below is the history of the old workflow.

- **One script, two callers.** `tools/ci.sh <host|qemu|mk66>` holds the configure, build and test commands, and returns non-zero on any warning or test failure. The workflow only calls it, so the same script is run locally and reviewed. `docs/architecture/os-tree.md` documents it.
- **Workflow `.github/workflows/ci.yml`:**
  - **Triggers:** a pushed release tag `v*` (it runs beside `release.yml`), a pull request into `dev` or `main` (OS-PUBLIC, 2026-10-17: the repository is public, so a change from outside gets the host gate before review) and manual runs (`workflow_dispatch`). Amended by the maintainer, 2026-10-16, because GitHub Actions minutes ran out; earlier it ran on pushes to `main` and `dev` as well. Day to day, the gate is `tools/ci.sh` run locally.
  - **Runner:** `ubuntu-latest`, host job only (host build and tests). QEMU and MK66 run locally with `tools/ci.sh qemu|mk66`.
  - **Concurrency:** `cancel-in-progress` per ref.
  - **Toolchains:**
    - host uses the runner's g++ and CMake ≥ 3.25;
    - qemu uses apt `qemu-system-arm` (8.2 on 24.04, the same as the local machine);
    - the Arm GNU toolchain is **15.3.Rel1** x86_64 arm-none-eabi, downloaded from Arm (the gitlab.arm.com URL in `docs/toolchain.md`; the developer.arm.com one returns 404) and checked against SHA-256 `563bebb2b97d53382b956d6ee1fe61e2cae26699901417234a37df505ef9b5fa`. It is unpacked to `.toolchains/` at the path `cmake/mk66-toolchain.cmake` expects, and cached with `actions/cache` keyed on that hash.
- **Security and hygiene:**
  - `permissions: contents: read`, no secrets, and third-party actions pinned by full commit SHA with the version in a comment.
  - No `.hex`, `.bin` or `.elf` is uploaded as an artifact; only ctest logs, on failure. CI never produces a flashable candidate for anyone: candidates go through the firmware gate check and the maintainer.
- **Fixtures.** `fixtures/` is never in the repo. Every test that reads it must skip cleanly (CTest `SKIP_RETURN_CODE` or a doctest guard) and still pass when it is absent. Prove this locally by running with the directory hidden or renamed in a scratch copy; never modify it.
- **Second opinions from a system library (OS-008b).** ctest `med_openmpt` compares the OctaMED importer with libopenmpt, loaded from the system with ctypes (nothing vendored). It skips without the library, except under `O3_CI=1`, which `ci.sh` exports (`O3_CI=0` lets a developer skip): there a missing library, one older than 0.7, or fewer than 2,400 comparisons fails. ctest `modimp_openmpt` (OS-MODIMP) does the same for the MOD and S3M importers (fewer than 10,000 comparisons fails). The workflow installs `libopenmpt0t64` in the host job.
- **Split the host `unit` ctest** (hygiene item, 157 s, one timeout at 300 s): register several ctests from the same binary by doctest test-suite or source group, each under about 60 s with ASan. Running them all must cover every case exactly once, which a ctest asserts by comparing doctest's `--count` totals.
- **The original firmware** (any 1.x image) is out of scope; this tree does not build or link it.
- **Done when:**
  - `tools/ci.sh` passes all three modes locally, with 0 warnings and the fixtures absent.
  - The workflow is valid. Check with `actionlint` if it is available as a pinned download; otherwise do a careful manual review against the GitHub schema.
  - The reviewer approves, with a supply-chain angle (pins, hash, permissions) and an artifact angle (no images).
  - The first run on GitHub is checked by the maintainer, or by an authenticated maintainer.
