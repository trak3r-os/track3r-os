#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The one CI entry point (OS-CI, docs/specs/OS-CI.md): the same script runs in .github/workflows/ci.yml and on a
# developer's machine, so what CI checks is what a person can read and reproduce.
#
#   tools/ci.sh host           configure with ASan + UBSan, build, ctest (the doctest units, the simulator goldens, the
#                                 layer check, the Python reference tools)
#   tools/ci.sh host-off       the host build with -DO3OS_RATE_44100=OFF (the pre-1.0 audio clock, 4080 cycles a sample), in build-os-ci-host-off/, and
#                                 its ctest (O3OS_CI_TESTS=<regex> to narrow it, e.g. `^unit_(audio|seq)$`); no manual or document checks (OS-RATE)
#   tools/ci.sh qemu           cross-build the unit suites for the Cortex-M4F, run them on QEMU mps2-an386, ctest
#   tools/ci.sh mk66           cross-build the MK66 libraries and the skeleton image, ctest (the image's static checks); then the tone
#                                 image (-DO3OS_AUDIO_TEST=ON, build only: it compiles different code and must not rot)
#                                 and the bench image (-DO3OS_BENCH=ON, OS-BENCHOPT, build only; the default image carries no bench code), plus one bench
#                                 image with the retired A/B benches kept (-DO3OS_BENCH_ARCHIVE=ON, OS-FLASHDIET B1, build only, so that they do not rot)
#   tools/ci.sh goldens        the host build as for `host`, then only the target update_goldens: rewrites sim/goldens.txt and the PNGs
#                                 (O3OS_GOLDEN_ONLY narrows it; no ctest). For the remote machine: tools/remote_ci.sh brings both back
#   tools/ci.sh fingerprints   the host build as for `host`, then only the target update_audio_fingerprints: rewrites tests/audio_fingerprints/*.json and writes
#                                 <build>/sim/fingerprint/fingerprint_diff.md (no ctest). For the remote machine: tools/remote_ci.sh brings both back (OS-RATE)
#   tools/ci.sh e2e            the end-to-end battery (OS-E2E, docs/specs/OS-E2E.md): the host build as for `host`, but ctest runs ONLY the tests labelled `e2e`
#                                 (the legibility sweep of the whole gallery, `legibility_<i>of6`, and the journeys `scenario_e2e_*`), and the result goes to build-e2e/<YYYY-MM-DD>/:
#                                 report.md (the pass/fail table of ctest, the findings of a failing test), screens.png (the contact sheet of every screen
#                                 and theme, `o3sim --legibility --sheet`, for a look once per release), wav/ (every `render` of the journeys as a WAV file,
#                                 `o3sim --wav`: OS-E2E-2, for listening). The reference players of OS-E2E-9 (`e2e_ref_*`: libopenmpt, scipy, Open303) need
#                                 libopenmpt0t64, python3-numpy and python3-scipy (a missing one fails here: O3_CI=1, and so does a libopenmpt that is not 0.7.x, the series the tolerances were measured on). On demand and as a release
#                                 step (a failure blocks the release); every other mode, and the GitHub workflow, leave the label out (ctest -LE '^e2e$').
#                                 The 8 shards of the random user (`qanet_random_user_<i>of8`, label `long`, 5 to 13 minutes each) run only with O3OS_CI_LONG=1: the
#                                 orchestrator's batch run and the image gate set it; a per-item e2e run leaves it out (OS-CISPEED).
#   tools/ci.sh toolchain         unpack the pinned Arm GNU toolchain into .toolchains/ unless it is there (downloads the
#                                 archive and checks its SHA-256 first)
#   tools/ci.sh toolchain-key  print the cache key of its archive (it contains the SHA-256)
#
# It fails (non-zero) on a configure or build error, on ANY warning in the configure or build output (the first-party
# targets are also -Werror), and on a failing ctest or an empty ctest list. Every run starts from a fresh build directory
# build-os-ci-<mode>/ (the logs ci-configure.log and ci-build.log stay in it; set O3OS_CI_KEEP=1 to reuse the directory when
# iterating, which can hide a warning in an up-to-date file). No image is ever copied out of the build directory: CI does not
# produce a flashable candidate (candidates go through fw-gate-check and the maintainer).
#
# Fixtures: fixtures/ (the maintainer's SD card files) is never in the repository. The tests that read it skip when it is absent
# (og_fixture_survey_test.cpp), and a CI run must not depend on whether the checkout happens to sit inside a tree that has one,
# so this script points O3_FIXTURES_DIR at a directory that does not exist. O3OS_CI_FIXTURES=1 leaves the variable alone
# (the tests then look for fixtures/sd the way they do under a plain ctest).
#
# Second opinions: ctest `med_openmpt` compares the OctaMED importer with libopenmpt, loaded from the system (apt: libopenmpt0t64).
# This script exports O3_CI=1, with which that test fails instead of skipping when the library is missing or too old, so a CI run
# cannot go quiet; a developer without the library runs `O3_CI=0 tools/ci.sh host` to let it skip.
#
# Risk scope (OS-CIFAST): the default runs every ctest. O3OS_CI_SCOPE=changed (host, qemu, mk66) builds as usual but runs only the ctests
# whose AREA label (cmake/O3osLabels.cmake) the changed files map to: `git diff --name-only <base>...HEAD`, plus the uncommitted
# and untracked files, where <base> is O3OS_CI_BASE (default `git merge-base HEAD $O3OS_CI_BASE_BRANCH`, that branch being `main` unless set). The map is in scope_resolve below; the chosen
# labels and the reason for each are printed; anything unmapped runs everything; a change of documents only runs nothing. It is for an
# agent's and a developer's intermediate runs; every image (an rc before the tester) and the GitHub workflow run the full CI.
#
# Speed (OS-CISPEED): (1) the tests labelled `slow` (over a minute each: the QANET self-tests, qanet_random_user_short, o3sim_manual_images) are left out
# of a scoped run; the full run (no scope) keeps them. (2) ctest runs the most expensive tests first when it has their cost from an earlier run
# (Testing/Temporary/CTestCostData.txt); a fresh build directory has none, so it is kept in O3OS_CI_COST_DIR (default ~/.cache/o3os-ci-cost) between
# runs and put back before ctest. (3) O3OS_CI_TESTS=<regex> runs only the tests whose name matches (ctest -R), whatever their labels and the scope
# (the scope is not even computed): for iterating on one test, e.g. `O3OS_CI_TESTS=unit_model tools/ci.sh host`. (4) O3OS_CI_LONG=1: see `e2e` above.
#
# Environment: O3OS_CI_JOBS (build and ctest parallelism, default 4), O3OS_CI_NICE, O3OS_CI_KEEP, O3OS_CI_FIXTURES, O3OS_CI_CCACHE, O3OS_CI_ARCHIVE_DIR
# (toolchain mode: a directory that keeps the downloaded archive between runs), O3_CI (default 1, see above), O3OS_CI_SCOPE (all, the
# default, or changed), O3OS_CI_BASE (scope=changed: the commit to compare with), O3OS_CI_BASE_BRANCH (scope=changed: the branch whose merge-base
# is that commit when O3OS_CI_BASE is unset, default main; tools/remote_ci.sh computes the base itself, from dev), O3OS_CI_TESTS, O3OS_CI_LONG, O3OS_CI_COST_DIR (above).
#
# Cross toolchain (qemu, mk66, and the module packer of the host build): the Arm GNU Toolchain 15.3.Rel1 directory is taken from ARM_GNU_ROOT
# (or O3_ARM_TOOLCHAIN) in the environment; without either it is .toolchains/<name> of this checkout, which `tools/ci.sh toolchain` fills.
# The host mode builds the .tmod modules too when that directory has a working compiler, and skips them (the tests that need them say so) when not.
# The pinned release and its checksum are in docs/toolchain.md.
set -euo pipefail

# ---- The pinned cross toolchain: Arm GNU Toolchain 15.3.Rel1, x86_64 Linux host, arm-none-eabi. The SHA-256 is that of the
# archive, also recorded in docs/toolchain.md; nothing is unpacked before it matches. The URL is Arm's own package registry
# (the one docs/toolchain.md names). cmake/arm-none-eabi-toolchain.cmake is told where the compiler is through ARM_GNU_ROOT (set below).
ARM_NAME=arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi
ARM_SHA256=563bebb2b97d53382b956d6ee1fe61e2cae26699901417234a37df505ef9b5fa
ARM_URL="https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/${ARM_NAME}.tar.xz"
ARM_GCC_VERSION_RE='15\.3\.1 '   # `arm-none-eabi-gcc --version`: "... (Arm GNU Toolchain 15.3.Rel1 (Build arm-15.149)) 15.3.1 20260627"

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
ARM_INSTALL_DIR="$ROOT/.toolchains/$ARM_NAME"
ARM_DIR=${ARM_GNU_ROOT:-${O3_ARM_TOOLCHAIN:-$ARM_INSTALL_DIR}}
ARM_GCC="$ARM_DIR/bin/arm-none-eabi-gcc"
# 4 jobs by default, at a low priority: the development PC is shared by several sessions and its maintainer keeps at least 2-3 cores free
# for their own work (maintainer, 2026-10-14). O3OS_CI_JOBS overrides the count, O3OS_CI_NICE=0 keeps the normal priority.
JOBS=${O3OS_CI_JOBS:-4}
if [ "${O3OS_CI_NICE:-1}" = 1 ]; then renice -n 10 -p $$ >/dev/null 2>&1 || true; fi

# ccache: when it is installed, every C/C++ compile of every mode goes through it, so a fresh build directory (the default here) or a new
# second checkout rebuilds from the cache instead of from scratch. The outputs are the compiler's own, byte for byte. O3OS_CI_CCACHE=0 turns it off.
# CCACHE_BASEDIR: paths inside this checkout are hashed relative to it, so another checkout's build of the same sources hits the cache (the
# images embed no path: no -g on the target, no __FILE__ outside the tests). The host build has -g: HOST_PREFIX_MAP writes its debug paths
# relative to the checkout too, and ccache then hashes the mapped directory instead of the checkout's own.
CCACHE_ARGS=()
HOST_PREFIX_MAP=()
if [ "${O3OS_CI_CCACHE:-1}" = 1 ] && command -v ccache >/dev/null 2>&1; then
  CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
  export CCACHE_BASEDIR="$ROOT"
  HOST_PREFIX_MAP=("-DCMAKE_C_FLAGS=-fdebug-prefix-map=$ROOT=." "-DCMAKE_CXX_FLAGS=-fdebug-prefix-map=$ROOT=.")
fi

# The scratch directory of install_toolchain, removed on every exit (a global: an EXIT trap runs after the function returned).
TMP_DIR=
E2E_SHEET_PID=   # the background contact sheet of `ci.sh e2e` (e2e_start); stopped on an exit that did not wait for it
trap '[ -z "$TMP_DIR" ] || rm -rf "$TMP_DIR"; [ -z "$E2E_SHEET_PID" ] || kill "$E2E_SHEET_PID" 2> /dev/null || true' EXIT

die() { echo "ci.sh: $*" >&2; exit 1; }
need() { command -v "$1" > /dev/null 2>&1 || die "$1 not found${2:+ ($2)}"; }

usage() {
  echo "usage: tools/ci.sh <host|host-off|qemu|mk66|e2e|goldens|fingerprints|toolchain|toolchain-key>   (see the comment at the top of this file)" >&2
  exit 2
}

toolchain_ok() {
  local version
  [ -x "$ARM_GCC" ] && version=$("$ARM_GCC" --version 2> /dev/null) && grep -Eq "$ARM_GCC_VERSION_RE" <<< "${version%%$'\n'*}"
}

# verify_archive <file>: the archive is the pinned one.
verify_archive() {
  echo "$ARM_SHA256  $1" | sha256sum --check --status --strict -
}

install_toolchain() {
  if [ "$ARM_DIR" = "$ARM_INSTALL_DIR" ] && toolchain_ok; then
    echo "toolchain: $ARM_NAME is already in .toolchains/"
    return
  fi
  [ "$ARM_DIR" = "$ARM_INSTALL_DIR" ] || die "ARM_GNU_ROOT/O3_ARM_TOOLCHAIN is set ($ARM_DIR): unset it to install into .toolchains/"
  need curl; need sha256sum; need tar; need xz
  mkdir -p "$ROOT/.toolchains"
  # A scratch directory beside the final place (same file system, so the last step is a rename).
  TMP_DIR=$(mktemp -d "$ROOT/.toolchains/.fetch.XXXXXX")
  local tmp=$TMP_DIR
  # O3OS_CI_ARCHIVE_DIR: where the archive is kept between runs (the workflow caches that directory). It is checked against the
  # SHA-256 on EVERY run, so a cache cannot change what gets unpacked; one that does not match is discarded and downloaded again.
  local archive_dir=${O3OS_CI_ARCHIVE_DIR:-$tmp}
  local archive="$archive_dir/$ARM_NAME.tar.xz"
  mkdir -p "$archive_dir"
  if [ -f "$archive" ] && ! verify_archive "$archive"; then
    echo "toolchain: the kept archive does not match its SHA-256, discarded"
    rm -f "$archive"
  fi
  if [ -f "$archive" ]; then
    echo "toolchain: using the kept $ARM_NAME.tar.xz"
  else
    echo "toolchain: downloading $ARM_NAME.tar.xz"
    curl --proto '=https' --proto-redir '=https' --fail --location --silent --show-error --retry 3 --retry-delay 5 --output "$archive.part" "$ARM_URL"
    mv "$archive.part" "$archive"
  fi
  verify_archive "$archive" || { rm -f "$archive"; die "$ARM_NAME.tar.xz does not match SHA-256 $ARM_SHA256"; }
  echo "toolchain: SHA-256 $ARM_SHA256 verified"
  tar -xJf "$archive" -C "$tmp" --no-same-owner
  [ -x "$tmp/$ARM_NAME/bin/arm-none-eabi-gcc" ] || die "the archive has no $ARM_NAME/bin/arm-none-eabi-gcc"
  rm -rf "$ARM_INSTALL_DIR"
  mv "$tmp/$ARM_NAME" "$ARM_INSTALL_DIR"
  toolchain_ok || die "$ARM_GCC is not GCC 15.3.1 after unpacking"
  echo "toolchain: installed $ARM_INSTALL_DIR"
}

# ctest starts the tests of the highest recorded cost first (Testing/Temporary/CTestCostData.txt), which keeps the long ones off the end of the run.
# A fresh build directory has none, so the data is kept between runs in O3OS_CI_COST_DIR (default ~/.cache/o3os-ci-cost, one file a mode): cost_restore
# merges it under the directory's own (the directory's entries win), cost_save writes the merge back through a rename. Best effort: it never fails a run.
cost_file() { echo "${O3OS_CI_COST_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/o3os-ci-cost}/$1.txt"; }
cost_merge() {   # cost_merge <winner> <other> <out>: the union of two CTestCostData files, <winner>'s line for a test in both
  python3 -I - "$@" << 'PY' 2> /dev/null || true
import sys
def load(p):
    d = {}
    try:
        for line in open(p):
            f = line.split()
            if len(f) == 3 and f[0] != "---":
                d[f[0]] = line.rstrip("\n")
    except OSError:
        pass
    return d
m = load(sys.argv[2]); m.update(load(sys.argv[1]))
with open(sys.argv[3], "w") as o:
    o.write("".join(m[k] + "\n" for k in sorted(m)) + "---\n")
PY
}
cost_restore() {   # cost_restore <build dir> <mode>
  local f; f=$(cost_file "$2")
  [ -s "$f" ] || return 0
  mkdir -p "$1/Testing/Temporary" 2> /dev/null || return 0
  cost_merge "$1/Testing/Temporary/CTestCostData.txt" "$f" "$1/Testing/Temporary/CTestCostData.new" && [ -s "$1/Testing/Temporary/CTestCostData.new" ] &&
    mv -f "$1/Testing/Temporary/CTestCostData.new" "$1/Testing/Temporary/CTestCostData.txt" || true
}
cost_save() {   # cost_save <build dir> <mode>
  local f; f=$(cost_file "$2")
  [ -s "$1/Testing/Temporary/CTestCostData.txt" ] || return 0
  mkdir -p "$(dirname "$f")" 2> /dev/null || return 0
  cost_merge "$1/Testing/Temporary/CTestCostData.txt" "$f" "$f.$$" && mv -f "$f.$$" "$f" || rm -f "$f.$$"
}

# run_mode <mode> <cmake configure argument>...   (BUILD_ONLY_TARGET=<target> in front: build that target and skip ctest)
run_mode() {
  local mode=$1; shift
  local dir="$ROOT/build-os-ci-$mode"

  need cmake; need ctest; need python3
  echo "== ci.sh $mode: $(cmake --version | sed -n 1p), python $(python3 -c 'import platform; print(platform.python_version())'), ${JOBS} jobs${CCACHE_ARGS[0]+, ccache}"

  if [ "${O3OS_CI_KEEP:-0}" != 1 ]; then rm -rf "$dir"; fi
  mkdir -p "$dir"

  if [ "${O3OS_CI_FIXTURES:-0}" != 1 ]; then
    export O3_FIXTURES_DIR="$dir/no-fixtures"   # never created: the survey tests find no directory and skip
  fi
  export O3_CI="${O3_CI:-1}"                    # med_openmpt may not skip (see the header)

  echo "== ci.sh $mode: configure"
  # The clock is pinned on every configure (a later -D in "$@" wins, host-off passes OFF): a kept build directory (O3OS_CI_KEEP=1, the
  # remote slots) holds the O3OS_RATE_44100 of its first configure in its cache, and the default turning ON in the CMakeLists (OS-RATE)
  # does not change a cached OFF: such a directory rendered the pre-OS-RATE gallery/record_* pictures.
  cmake -S "$ROOT" -B "$dir" ${CCACHE_ARGS[@]+"${CCACHE_ARGS[@]}"} -DO3OS_RATE_44100=ON "$@" 2>&1 | tee "$dir/ci-configure.log"
  echo "== ci.sh $mode: build"
  cmake --build "$dir" ${BUILD_ONLY_TARGET:+--target "$BUILD_ONLY_TARGET"} -j "$JOBS" 2>&1 | tee "$dir/ci-build.log"

  # The first-party targets are -Werror; this also catches a warning from CMake, the linker or a target that opts out.
  if grep -nEi 'warning[:(]|CMake (Deprecation )?Warning' "$dir/ci-configure.log" "$dir/ci-build.log" >&2; then
    die "$mode: warnings in the configure or build output (above)"
  fi

  if [ -z "${BUILD_ONLY_TARGET:-}" ]; then
    # Every ctest has an area label (O3osLabels.cmake stops the configure for one that has none; this is the second guard, for a directory
    # that forgot the call): a test without one would be left out of every scope.
    local unlabelled
    unlabelled=$(ctest --test-dir "$dir" -N -LE "$ALL_LABELS_RE" | sed -n 's/^Total Tests: //p')
    [ "$unlabelled" = 0 ] || { ctest --test-dir "$dir" -N -LE "$ALL_LABELS_RE" >&2; die "$mode: $unlabelled ctest(s) without an area label (cmake/O3osLabels.cmake)"; }
    # The label `e2e` (OS-E2E) marks the on-demand battery: every other run leaves it out, the e2e mode runs only it. The label `slow`
    # (OS-CISPEED) marks the tests of more than a minute: a scoped run leaves them out, the full run keeps them. `long` (e2e tests of
    # 5 minutes or more) runs in the e2e mode only with O3OS_CI_LONG=1. O3OS_CI_TESTS=<regex> runs just the tests whose name matches
    # (ctest -R), whatever their labels and the scope: for iterating on one test.
    local label_args=() selected
    local total_all
    total_all=$(ctest --test-dir "$dir" -N | sed -n 's/^Total Tests: //p')
    if [ -n "${O3OS_CI_TESTS:-}" ]; then
      if [ "$mode" = e2e ]; then label_args=(-L '^e2e$' -R "$O3OS_CI_TESTS"); else label_args=(-R "$O3OS_CI_TESTS"); fi
      selected=$(ctest --test-dir "$dir" -N "${label_args[@]}" | sed -n 's/^Total Tests: //p')
      [ "$selected" = 0 ] || TESTS_MATCHED=1
      echo "== ci.sh $mode: ctest, O3OS_CI_TESTS='$O3OS_CI_TESTS': $selected of $total_all tests"
    elif [ "$mode" = e2e ]; then
      label_args=(-L '^e2e$')
      [ "${O3OS_CI_LONG:-0}" = 1 ] || label_args+=(-LE '^long$')
      selected=$(ctest --test-dir "$dir" -N "${label_args[@]}" | sed -n 's/^Total Tests: //p')
      echo "== ci.sh $mode: ctest, the e2e label$([ "${O3OS_CI_LONG:-0}" = 1 ] || echo ', without the long shards (O3OS_CI_LONG=1 adds them)'): $selected of $total_all tests"
    elif [ -n "$SCOPE_REGEX" ] && [ "${mode#host}" != "$mode" ]; then   # the host tests only: the target ones are about a minute and hold the budgets
      label_args=(-L "$SCOPE_REGEX" -LE "$SCOPE_EXCLUDE")
      selected=$(ctest --test-dir "$dir" -N "${label_args[@]}" | sed -n 's/^Total Tests: //p')
      echo "== ci.sh $mode: ctest, scope changed (without the labels $SCOPE_EXCLUDE): $selected of $(ctest --test-dir "$dir" -N -LE '^e2e$' | sed -n 's/^Total Tests: //p') tests"
    else
      label_args=(-LE '^e2e$')
      selected=
      echo "== ci.sh $mode: ctest"
    fi
    local rc=0 e2e_out=
    CTEST_RAN=1
    if [ "$selected" = 0 ] && [ "$mode" = e2e ] && [ -z "${O3OS_CI_TESTS:-}" ]; then
      die "e2e: no ctest has the label e2e (cmake/O3osLabels.cmake)"
    elif [ "$selected" = 0 ]; then
      echo "== ci.sh $mode: no test of this mode has these labels, nothing to run"
    else
      if [ "$mode" = e2e ]; then
        e2e_out="$ROOT/build-e2e/$(date +%F)"
        e2e_start "$dir" "$e2e_out"
        # (ctest --output-junit needs CMake 3.21; the project minimum is 3.20, so only `ci.sh e2e` asks for more)
        label_args+=(--output-junit "$dir/e2e-junit.xml")
      fi
      cost_restore "$dir" "$mode"
      ctest --test-dir "$dir" --output-on-failure --no-tests=error -j "$JOBS" "${label_args[@]}" || rc=$?
      cost_save "$dir" "$mode"
      if [ "$mode" = e2e ]; then e2e_finish "$dir" "$e2e_out" "$rc"; fi
      [ "$rc" = 0 ] || exit "$rc"
    fi
    # The sum of the tests' own times (what a serial run would take) beside the wall time that ctest prints.
    awk '/^Test time = / {sum += $4} END {printf "== ci.sh %s: ctest cpu sum %.0f s\n", "'"$mode"'", sum}' "$dir/Testing/Temporary/LastTest.log" 2> /dev/null || true
  fi
  echo "== ci.sh $mode: OK"
}

# ---- tools/ci.sh e2e: what goes to build-e2e/<date>/. e2e_start begins the contact sheet in the background (one unsharded legibility run; it takes as long as
# the sharded tests together, so it runs beside them), e2e_finish waits for it and writes report.md from ctest's JUnit file. The sheet run's own verdict is
# the tests' (the same sweep); only a failure to write the picture is reported here.
e2e_start() {
  local dir=$1 out=$2
  mkdir -p "$out"
  rm -f "$out/screens.png" "$out/legibility.txt" "$out/report.md"
  echo "== ci.sh e2e: contact sheet (o3sim --legibility --sheet) in the background -> ${out#"$ROOT"/}/screens.png"
  "$dir/sim/o3sim" --legibility --sheet "$out/screens.png" > "$out/legibility.txt" 2>&1 &
  E2E_SHEET_PID=$!
}

e2e_finish() {
  local dir=$1 out=$2 rc=$3 sheet_rc=0
  wait "$E2E_SHEET_PID" || sheet_rc=$?
  E2E_SHEET_PID=
  # exit 1 of the sheet run is a legibility finding, which the failing test reports; 2 is a run that could not draw or write.
  [ "$sheet_rc" -lt 2 ] || echo "ci.sh e2e: the contact sheet run failed (exit $sheet_rc), see ${out#"$ROOT"/}/legibility.txt" >&2
  [ -s "$out/screens.png" ] || echo "ci.sh e2e: no contact sheet was written" >&2
  # OS-E2E-2: what the journeys rendered (every ctest scenario writes its renders to <build>/sim/wav/): a stale folder of an earlier run is replaced.
  rm -rf "$out/wav"
  if ls "$dir"/sim/wav/*.wav > /dev/null 2>&1; then
    mkdir -p "$out/wav" && cp "$dir"/sim/wav/*.wav "$out/wav/"
    echo "== ci.sh e2e: $(find "$out/wav" -name '*.wav' | wc -l) renders (WAV) in ${out#"$ROOT"/}/wav"
  fi
  python3 -I "$ROOT/tools/e2e_report.py" --junit "$dir/e2e-junit.xml" --out "$out/report.md" --exit-code "$rc" \
    --commit "$(git -C "$ROOT" rev-parse --short=10 HEAD 2> /dev/null || echo unknown)" --sheet "$(basename "$out")/screens.png" \
    --sheet-ok "$([ -s "$out/screens.png" ] && echo yes || echo no)" || echo "ci.sh e2e: report.md could not be written" >&2
  echo "== ci.sh e2e: report ${out#"$ROOT"/}/report.md"
}

# ---- The risk scope. SCOPE_REGEX is empty (run every ctest) or an anchored regex of area labels for `ctest -L`.
# The areas (a test's labels are set in cmake/O3osLabels.cmake): core model formats seq audio app platform python fuzz manual, and `static`
# (the repo-wide structure checks: layers, wiring audit, unit coverage), which every non-empty scope includes. `manual` is the user manual's
# generated pictures (docs/manual/img/, OS-MANUALIMG): a change of one of them, or of its manifest, is not "documents only".
# QANET's rule tests are two thirds of the `app` time: a scoped run leaves them out unless the change is theirs; the full run keeps them.
SCOPE_REGEX=
SCOPE_EXCLUDE='^(e2e|slow|qanet)$'
ALL_LABELS_RE='^(core|model|formats|seq|audio|app|platform|python|fuzz|manual|static)$'

# scope_resolve: sets SCOPE_REGEX from O3OS_CI_SCOPE; exits 0 (nothing to run) for a change of documents only.
scope_resolve() {
  case "${O3OS_CI_SCOPE:-all}" in
    all) return ;;
    changed) ;;
    *) die "O3OS_CI_SCOPE must be 'all' or 'changed', not '${O3OS_CI_SCOPE}'" ;;
  esac
  need git
  local base=${O3OS_CI_BASE:-}
  if [ -z "$base" ]; then
    base=$(git -C "$ROOT" merge-base HEAD "${O3OS_CI_BASE_BRANCH:-main}" 2> /dev/null) || base=
  fi
  if [ -z "$base" ] || ! git -C "$ROOT" rev-parse --verify --quiet "$base^{commit}" > /dev/null; then
    echo "== ci.sh scope: no base commit (git merge-base HEAD ${O3OS_CI_BASE_BRANCH:-main} failed; set O3OS_CI_BASE or O3OS_CI_BASE_BRANCH): running everything"
    return
  fi
  local files
  files=$({ git -C "$ROOT" diff --name-only "$base...HEAD"; git -C "$ROOT" diff --name-only HEAD
            git -C "$ROOT" ls-files --others --exclude-standard; } | sort -u)
  if [ -z "$files" ]; then
    echo "== ci.sh scope: changed against ${base:0:10}: no changes, no tests"
    exit 0
  fi

  local -A want=()
  local f labels all=0 shown=0 total=0
  echo "== ci.sh scope: changed against ${base:0:10} (committed, staged, unstaged and untracked files)"
  while IFS= read -r f; do
    total=$((total + 1))
    case "$f" in
      modules/*) labels="audio python core" ;;   # before the CMakeLists rule: modules/CMakeLists.txt only adds modules. core: unit_rest holds the module_* tests
      core/*|model/*|hal/*|CMakeLists.txt|*/CMakeLists.txt|cmake/*|tools/ci.sh) labels=all ;;
      formats/*) labels="formats fuzz app" ;;
      module/*) labels="audio app python" ;;
      midi/*) labels="seq app platform" ;;
      usb/*) labels=platform ;;
      tests/qanet/*|tools/qanet_ledger.py|docs/qa/qanet-ledger.md) labels="static app qanet" ;;
      # A test file: the unit group of tests/CMakeLists.txt it is in, same globs; any other file is in unit_rest or a group of these labels.
      tests/native_fuzz_test.cpp) labels=fuzz ;;
      tests/usb_*_test.cpp) labels=platform ;;
      tests/audio_*_test.cpp|tests/tb303_*_test.cpp|tests/paula_oracle_test.cpp|tests/amiga_*_test.cpp|tests/cechip_*_test.cpp) labels=audio ;;
      tests/seq_*_test.cpp|tests/midi_*_test.cpp|tests/metro_*_test.cpp|tests/play_*_test.cpp) labels=seq ;;
      tests/*_test.cpp) labels="core app model formats" ;;
      seq/*) labels="seq app" ;;
      audio/*) labels="audio seq" ;;
      sim/src/audio_check.cpp|sim/include/o3os/sim/audio_check.h) labels="audio app" ;;   # OS-E2E-2: the measurements of `assert audio` are unit_audio's (tests/audio_check_test.cpp), the journeys and the rest of sim/ are app's
      sim/src/audio_fingerprint.cpp|sim/include/o3os/sim/audio_fingerprint.h) labels="audio app" ;;   # OS-E2E-4: the analysis is audio_fingerprint_test.cpp (unit_audio); the journeys that compare the prints are label e2e, which no scope runs
      tests/audio_fingerprints/*|tools/update_audio_fingerprints.py|tools/check_fingerprint_guard.py) labels="python static" ;;   # OS-E2E-4: the guard (ctest fingerprint_guard) and the tools' self-tests
      app/*|ui/*|sim/*|eve/*) labels=app ;;
      platform/host/*) labels="platform app" ;;   # OS-E2E-1b: the host card and storage (dir_card, dir_storage) are used by the simulator (dir_card_test, sim_card_test, the journeys)
      platform/mk66/tools/hil/*.py) labels="platform python" ;;   # OS-MIDISYNC M1: the HIL Python tools (midi_clock_tool.py) are pure-Python tools like tools/*.py: their self-tests (ctest midi_clock_tool_selftest) run on their change
      platform/*) labels=platform ;;
      docs/manual/img/*|docs/manual/images.txt|docs/manual/diagrams.txt) labels=manual ;;
      tools/manual_images.py|tools/manual_diagrams.py) labels="python manual" ;;
      tests/e2e_card_fixture_test.py|tests/e2e_og_fixture_test.py) labels="app formats python" ;;   # OS-E2E-1b: the check of the generated card of the journeys (the generator is sim/CMakeLists.txt, which maps to everything)
      docs/qa/*|tools/qa_cover.py) labels=static ;;   # OS-E2E-3: the catalogue gate (ctest qa_cover) runs on a catalogue edit; it is not "docs only". OS-QANET A4: the ledger (docs/qa/qanet-ledger.md) and its gate (ctest qanet_ledger) are the same
      tools/*.py|tests/*.py|tools/release/*) labels=python ;;
      docs/*|*.md) labels=docs ;;
      *) labels=all ;;   # unmapped: fail safe
    esac
    if [ "$shown" -lt 25 ]; then
      echo "   $f -> ${labels/all/ALL (everything)}"
      shown=$((shown + 1))
    fi
    if [ "$labels" = all ]; then all=1; else
      local l; for l in $labels; do want[$l]=1; done
    fi
  done <<< "$files"
  [ "$total" -le "$shown" ] || echo "   ... and $((total - shown)) more files"

  if [ "$all" = 1 ]; then
    echo "== ci.sh scope: a file maps to everything (core, model, hal, a CMakeLists.txt, cmake/, ci.sh, or no rule): all tests"
    return
  fi
  unset 'want[docs]'
  if [ "${#want[@]}" = 0 ]; then
    echo "== ci.sh scope: docs only, no tests"
    exit 0
  fi
  [ -z "${want[qanet]:-}" ] || SCOPE_EXCLUDE='^(e2e|slow)$'
  local chosen
  chosen=$(printf '%s\n' "${!want[@]}" static | sort -u | paste -sd'|')
  SCOPE_REGEX="^(${chosen})\$"
  echo "== ci.sh scope: labels ${chosen//|/ } (ctest -L '$SCOPE_REGEX'); the full run (O3OS_CI_SCOPE unset) is for every image"
}

# The cross modes: the pinned compiler must be in ARM_DIR, which is handed to cmake/mk66-toolchain.cmake as ARM_GNU_ROOT (a different
# arm-none-eabi-gcc on PATH is not used, and a missing one would fail late and obscurely).
need_cross() {
  toolchain_ok || die "$ARM_NAME is not in $ARM_DIR: set ARM_GNU_ROOT to it, or run tools/ci.sh toolchain to unpack it into .toolchains/"
  echo "== ci.sh $1: $("$ARM_GCC" --version | sed -n 1p)"
}

# OS-MANUALTOOLS: the LLM guide must be the build of the chapters. Cheap (no build), and before scope_resolve, which exits early for a
# change of documents only.
manual_llms_check() {
  echo "== ci.sh host: manual LLM guide (tools/manual_llms.py --check)"
  python3 "$ROOT/tools/manual_llms.py" --check || die "host: docs/manual/llms*.txt are out of date: run tools/manual_llms.py and commit them"
}

# OS-MANUALIMG: the manual's pictures are committed, so a chapter that shows a picture with no file (or has a broken link, or is missing from
# the index) renders broken on GitHub. Cheap, no build. The other direction, a committed PNG that the simulator no longer draws, is the ctest
# o3sim_manual_images.
manual_check_run() {
  echo "== ci.sh host: manual links, index, picture files, accessibility and sync markers (tools/manual_check.py --strict)"
  local out
  out=$(python3 "$ROOT/tools/manual_check.py" --strict) || { echo "$out"; die "host: docs/manual has a broken link, a chapter missing from index.md, a picture with no file, an accessibility problem or a wrong sync marker or docs/manual/sync.lock line: run tools/manual_check.py (pictures: tools/manual_images.py, then commit docs/manual/img/; sync: tools/manual_check.py --ack <chapter>#<section>, or --ack-path <file> for a file many sections cite)"; }
  # OS-MANUAL2 M2-3: the branch gate. A section that cites a file this branch changed must have changed too (its text, or its line of sync.lock);
  # on dev, or with no merge base, it only warns. O3OS_MANUAL_BASE overrides the base.
  echo "== ci.sh host: manual sync gate (tools/manual_check.py --gate)"
  python3 "$ROOT/tools/manual_check.py" --gate || die "host: a manual section cites a file this branch changed and neither the section nor its docs/manual/sync.lock line did: read the sections the gate listed, fix the text that is wrong, then run the one command it printed (tools/manual_check.py --ack-path <file> ...) and commit docs/manual/sync.lock. After a merge, a conflict in sync.lock: take either side and run --ack-path <file> again on the merged tree"
  echo "== ci.sh host: manual staleness report (information only, tools/manual_check.py --stale)"
  python3 "$ROOT/tools/manual_check.py" --stale || true
}

# OS-MANUALDIAG: the diagrams (docs/manual/img/*.svg) take their keys and boxes from the firmware's tables and code, so a change of code can make one
# stale although no manual file changed; a scoped run (a change to docs/manual/*.md maps to `docs`) must still check them. Cheap, no build.
manual_diagrams_check() {
  echo "== ci.sh host: manual diagrams (tools/manual_diagrams.py --check)"
  python3 "$ROOT/tools/manual_diagrams.py" --check || die "host: a docs/manual diagram is stale or not in its chapter: run tools/manual_diagrams.py and commit docs/manual/img/"
}

# OS-MANUAL2 M2-2: the key tables of the chapters and the cheat sheet are generated from the firmware's key-row tables (tools/manual_tables.py), so a
# renamed key makes one stale although no manual file changed. Cheap, no build. It runs before the LLM guide's check, which would fail for the same reason.
manual_tables_check() {
  echo "== ci.sh host: manual key tables (tools/manual_tables.py --check)"
  python3 "$ROOT/tools/manual_tables.py" --check || die "host: a docs/manual key table is stale or malformed: run tools/manual_tables.py, then tools/manual_llms.py (the LLM guide copies the tables), and commit docs/manual/"
}

case "${1:-}" in
  host) need python3; manual_tables_check; manual_llms_check; manual_check_run; manual_diagrams_check ;;
esac

case "${1:-}" in
  host|host-off|qemu|mk66) if [ -z "${O3OS_CI_TESTS:-}" ]; then scope_resolve; fi ;;   # O3OS_CI_TESTS names the tests itself: no scope
esac

case "${1:-}" in
  host)
    need g++
    echo "== ci.sh host: $(g++ --version | sed -n 1p)"
    # The cross compiler, when there is one, lets the host build also pack the .tmod modules (modules/CMakeLists.txt).
    HOST_ARM=()
    if toolchain_ok; then HOST_ARM=("-DARM_GNU_ROOT=$ARM_DIR"); fi
    run_mode host -DO3OS_SANITIZE=ON ${HOST_PREFIX_MAP[@]+"${HOST_PREFIX_MAP[@]}"} ${HOST_ARM[@]+"${HOST_ARM[@]}"}
    ;;
  host-off)
    # The pre-1.0 clock (core/audio_clock.h): a build of its own, so the default's build directory is not reconfigured back and forth.
    need g++
    echo "== ci.sh host-off: $(g++ --version | sed -n 1p), -DO3OS_RATE_44100=OFF"
    run_mode host-off -DO3OS_SANITIZE=ON -DO3OS_RATE_44100=OFF ${HOST_PREFIX_MAP[@]+"${HOST_PREFIX_MAP[@]}"}
    ;;
  qemu)
    need_cross qemu
    need qemu-system-arm "apt install qemu-system-arm"
    echo "== ci.sh qemu: $(qemu-system-arm --version | sed -n 1p)"
    run_mode qemu -DO3OS_PLATFORM=qemu "-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/mk66-toolchain.cmake" "-DARM_GNU_ROOT=$ARM_DIR"
    ;;
  e2e)
    need g++
    echo "== ci.sh e2e: $(g++ --version | sed -n 1p)"
    # The same host build as `host` (ccache hits it), but ctest runs the e2e label only (run_mode).
    HOST_ARM=()
    if toolchain_ok; then HOST_ARM=("-DARM_GNU_ROOT=$ARM_DIR"); fi
    run_mode e2e -DO3OS_SANITIZE=ON ${HOST_PREFIX_MAP[@]+"${HOST_PREFIX_MAP[@]}"} ${HOST_ARM[@]+"${HOST_ARM[@]}"}
    ;;
  goldens)
    need g++
    # The build directory of `host` (a kept slot builds incrementally), only update_goldens; its processes follow the job count.
    HOST_ARM=()
    if toolchain_ok; then HOST_ARM=("-DARM_GNU_ROOT=$ARM_DIR"); fi
    export O3OS_GOLDEN_JOBS="${O3OS_GOLDEN_JOBS:-$JOBS}"
    BUILD_ONLY_TARGET=update_goldens run_mode host -DO3OS_SANITIZE=ON ${HOST_PREFIX_MAP[@]+"${HOST_PREFIX_MAP[@]}"} ${HOST_ARM[@]+"${HOST_ARM[@]}"}
    ;;
  fingerprints)
    need g++
    # The build directory of `host`, only update_audio_fingerprints (the same journeys `ci.sh e2e` fingerprints, at this tree's code).
    HOST_ARM=()
    if toolchain_ok; then HOST_ARM=("-DARM_GNU_ROOT=$ARM_DIR"); fi
    BUILD_ONLY_TARGET=update_audio_fingerprints run_mode host -DO3OS_SANITIZE=ON ${HOST_PREFIX_MAP[@]+"${HOST_PREFIX_MAP[@]}"} ${HOST_ARM[@]+"${HOST_ARM[@]}"}
    ;;
  mk66)
    need_cross mk66
    run_mode mk66 -DO3OS_PLATFORM=mk66 "-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/mk66-toolchain.cmake" "-DARM_GNU_ROOT=$ARM_DIR"
    [ "${O3OS_CI_SCOPE:-all}" = changed ] || BUILD_ONLY_TARGET=o3os_image run_mode mk66-tone -DO3OS_PLATFORM=mk66 -DO3OS_AUDIO_TEST=ON "-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/mk66-toolchain.cmake" "-DARM_GNU_ROOT=$ARM_DIR"
    BUILD_ONLY_TARGET=o3os_image run_mode mk66-bench -DO3OS_PLATFORM=mk66 -DO3OS_BENCH=ON "-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/mk66-toolchain.cmake" "-DARM_GNU_ROOT=$ARM_DIR"
    # OS-FLASHDIET B1: the archived A/B benches (cpuopt, looper, voice_st, disk, burst, kernels, mem, delay, reverb, limiter) are compiled (bench.cpp, console.cpp with O3OS_BENCH_ARCHIVE=ON, no link: that image no longer fits the 1 MiB flash), so that the code does not rot.
    [ "${O3OS_CI_SCOPE:-all}" = changed ] || BUILD_ONLY_TARGET=o3os_bench_archive_objs run_mode mk66-bench-archive -DO3OS_PLATFORM=mk66 -DO3OS_BENCH=ON -DO3OS_BENCH_ARCHIVE=ON "-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/mk66-toolchain.cmake" "-DARM_GNU_ROOT=$ARM_DIR"
    ;;
  toolchain)
    install_toolchain
    ;;
  toolchain-key)
    echo "arm-gnu-15.3.rel1-x86_64-$ARM_SHA256"
    ;;
  *)
    usage
    ;;
esac
# O3OS_CI_TESTS that names no test of any mode that ran ctest is a typo, not a pass.
if [ -n "${O3OS_CI_TESTS:-}" ] && [ "${CTEST_RAN:-0}" = 1 ] && [ "${TESTS_MATCHED:-0}" = 0 ]; then
  die "no test matches O3OS_CI_TESTS='$O3OS_CI_TESTS'"
fi
