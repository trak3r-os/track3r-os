---
name: fw-gate-check
description: Static release gate for a Community OS image (.ptf) before it goes to a test card or through the OS update path - reproducible build, Intel HEX validity, address range, vectors, flash configuration field, link sanity, manifest. Never flashes. Use before handing any image to the tester or the maintainer for hardware testing.
---

# Gate C - static validation of a candidate image

This produces evidence for the decision to update a unit. It never flashes, never writes to anything but an SD card (CLAUDE.md, SD cards and firmware delivery), and never names a candidate so that it could be mistaken for an official file. A file copied to the SD card is not proof of an update.

1. **Clean tree and identity.** `git status --short` must be clean (or list exactly what is uncommitted). Record `git rev-parse HEAD`, the version, and the build options of the image (for example `O3OS_AUDIO_TEST`, `O3OS_RATE_44100`).
2. **Reproducible build twice.** Build the MK66 image into two fresh directories with identical options and compare:
   ```sh
   for d in a b; do
     nice -n 10 cmake -S . -B build-gate-$d -DO3OS_PLATFORM=mk66 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mk66-toolchain.cmake -DARM_GNU_ROOT=<toolchain dir>
     flock "$(git rev-parse --git-common-dir)/../.claude/worktrees/.ci.lock" nice -n 10 cmake --build build-gate-$d -j10
   done
   cmp build-gate-a/platform/mk66/o3os.hex build-gate-b/platform/mk66/o3os.hex
   ```
   Both must exit 0 without warnings; the HEX files must be identical. The toolchain directory is documented in `docs/toolchain.md`.
3. **Image checks.** The build already runs them as ctests (`mk66_image`, `mk66_flashconfig`, `mk66_ld`, `mk66_ramfunc`, `mk66_noinit_guard`); run them again and read the verdicts:
   ```sh
   ctest --test-dir build-gate-a --output-on-failure
   python3 -I platform/mk66/tools/check_image.py check build-gate-a/platform/mk66/o3os.hex --expect-sp 0x20030000 --ramfunc-l 0x1FFF0000:0xC000 --json build-gate-a/image-check.json
   python3 -I platform/mk66/tools/check_image.py flashconfig build-gate-a/platform/mk66/o3os.hex --expect ffffffffffffffffffffffffdef9ffff
   ```
   Required: every record checksum valid; every address at or below `0x000FFFFF`; one contiguous run from 0; initial SP `0x20030000`; reset vector a Thumb address inside the populated range; flash configuration bytes `0x400..0x40F` equal to `ffffffffffffffffffffffffdef9ffff` (a wrong value can permanently lock the MCU; any difference is an automatic FAIL). A failing HEX is deleted by `--delete-on-fail`, never keep it.
4. **Link sanity.** In the build log: no "region overflowed", no undefined references, no warnings. Record the linker usage table (FLASH, SRAM_U, SDRAM_STATIC) and `size -A`.
5. **Tests.** `tools/ci.sh host`, `qemu` and `mk66` (full runs, not scoped; separate calls, so one failure does not hide the others; after a fix, re-run only from the failed stage) must have exited 0 on this firmware code. Don't re-run what already passed: `git diff --stat <passed> HEAD -- . ':!docs' ':!.github'` empty means the code is identical, so reuse that commit's result and name it in the manifest; run only the targets that never ran on this code (usually `qemu`, once per image). A bench image (not a release) needs only steps 1-4 and 7 plus the build's ctest. Every run goes through the CI lock with `O3OS_CI_JOBS=10` (`flock "$(git rev-parse --git-common-dir)/../.claude/worktrees/.ci.lock" env O3OS_CI_JOBS=10 nice -n 10 taskset -c 2-19`).
6. **Update-path facts.** Convert the HEX into the deliverable only by renaming/copying the verified `o3os.hex` to a `.ptf` name that matches the OS update path's guard (`[0-9A-Za-z][0-9A-Za-z._-]{0,25}.ptf`: **at most 30 characters in all on the card, `.ptf` included**, the longest name proven on the unit; with 33 characters the SD bootloader flashed the FIRST `.ptf` of `Firmware/` with no error, OS-BUG-FWNAMELEN, and the OS refuses to arm an update for a longer name; avoid names starting with `Polyend`, which the writer treats as official files): for example `o3os-<version>-<shortsha>.ptf` when it fits (count it: `o3os-v1.0.1-abcdef0.ptf` is 23), else a shorter tag such as `o3os_1.0.1-stbase.1.ptf` (23); check with `printf %s NAME | wc -c`. The supported ways to get it onto a unit are the `diag` console update path (`platform/mk66/tools/o3console.py --put FILE.ptf`, then `--update NAME --crc X --yes-update`, `--expect-volume XXXX-XXXX` is optional) or a copy to `Firmware/` of the SD card (any card may be written, CLAUDE.md, SD cards and firmware delivery). The hidden reset button flashes the FIRST `.ptf` of `Firmware/` in directory order: say in the manifest which `.ptf` files are already on the card.
7. **Manifest.** Write `build-gate-a/CANDIDATE.md` (untracked): commit, version, options, toolchain version (`arm-none-eabi-gcc --version | head -1`), `.ptf` SHA-256 (`sha256sum`), flash/SRAM usage, test results, every check above with PASS/FAIL, and the explicit line "Not flashed. Hardware testing requires the maintainer's go and a verified target (card volume, unit)." Copy the verified image and manifest to `build-images/<version>/` (untracked).
8. **Second pass.** Ask the `firmware-reviewer` agent (or do a deliberate re-read) to check the manifest against the raw outputs. Report with the candidate SHA-256 and stop. The maintainer or the tester decides whether to install.

Never: SWD or debug-probe flashing, improvised uploaders, bypassing a confirmation the device enforces, copying to storage that is not an SD card, committing an image to git.
