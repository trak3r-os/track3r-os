#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# remote_ci.sh [--slot <name>] [--scope changed] <mode> [mode...]: runs `tools/ci.sh <mode>` for the working tree on the
# remote build machine, when one is configured (docs/toolchain.md, "Remote build machine"). Every agent may use it.
#
# O3OS_REMOTE_CI=<ssh-host>:<root> names the machine. It is set locally only (the `env` block of
# .claude/settings.local.json), never in git. <root> holds repo.git (a bare repository) and o3ci.sh, the machine's
# wrapper: `o3ci.sh <slot> <ref> <mode...>` checks <ref> out in its worktree wt-<slot>, runs the modes under the
# machine's own CI lock and prints the log path, the ctest summary and `rc=<n> secs=<n>`.
#
# The working tree is sent as it is, committed or not: tracked changes and untracked files that git does not ignore
# go into a snapshot commit on top of HEAD (a temporary index; the branch, the index and the tree stay as they are).
# The snapshot is pushed to refs/ci/<sha> of repo.git, a new ref per commit, so nothing is ever forced.
# --slot   the remote worktree (default: this worktree's folder name). A lane keeps its slot, so its builds stay
#          incremental; `dev` and `gate` start fresh and are the orchestrator's.
# --scope  changed: only the ctests of the areas changed since the merge-base with O3OS_CI_BASE_BRANCH (default dev, main when there is no
#          dev), computed here and passed as O3OS_CI_BASE. It leaves out the tests labelled `slow` (OS-CISPEED); a run without --scope runs them.
# O3OS_CI_TESTS=<regex> (OS-CISPEED): only the ctests whose name matches (ctest -R), whatever their labels and the scope: for iterating on one
#          test. Letters, digits and _ . | ^ $ * + ? ( ) - only.
# O3OS_CI_LONG=1 (OS-CISPEED): `e2e` also runs the tests labelled `long`, the random user's 8 shards of 5 to 13 minutes each. Per-item e2e runs
#          leave it unset; the orchestrator's batch run and the image gate set it.
# O3OS_QANET_SEED, O3OS_QANET_SEEDS, O3OS_QANET_STEPS (numbers) are passed on: `O3OS_QANET_SEEDS=2000 tools/remote_ci.sh e2e` is the long run of the random user (ctest qanet_random_user_*, label e2e).
# The full log is copied to build-remote/<log name> (untracked). Exit: the ci.sh exit code; 2 bad usage; 3 no remote
# configured (run tools/ci.sh locally); 255 ssh failed (the machine is off or unreachable: run locally).
set -u

usage() { echo "usage: tools/remote_ci.sh [--slot <name>] [--scope changed] <host|host-off|e2e|qemu|mk66|goldens|fingerprints> [mode...]" >&2; exit 2; }

slot= scope=
while [ $# -gt 0 ]; do
  case "$1" in
    --slot) [ $# -ge 2 ] || usage; slot=$2; shift 2 ;;
    --scope) [ $# -ge 2 ] || usage; scope=$2; shift 2 ;;
    -h|--help) usage ;;
    -*) usage ;;
    *) break ;;
  esac
done
[ $# -ge 1 ] || usage
for m in "$@"; do
  case "$m" in host|host-off|e2e|qemu|mk66|goldens|fingerprints) ;; *) echo "remote_ci: unknown mode '$m'" >&2; exit 2 ;; esac
done
case "$scope" in ""|changed) ;; *) echo "remote_ci: --scope takes 'changed'" >&2; exit 2 ;; esac

if [ -z "${O3OS_REMOTE_CI:-}" ]; then
  echo "remote_ci: O3OS_REMOTE_CI is not set; run tools/ci.sh locally (docs/toolchain.md)" >&2
  exit 3
fi
host=${O3OS_REMOTE_CI%%:*}
root=${O3OS_REMOTE_CI#*:}
if [ "$host" = "$O3OS_REMOTE_CI" ] || [ -z "$host" ] || [ -z "$root" ]; then
  echo "remote_ci: O3OS_REMOTE_CI must be <ssh-host>:<root>" >&2
  exit 2
fi

top=$(git rev-parse --show-toplevel) || exit 2
cd "$top" || exit 2
[ -n "$slot" ] || slot=$(basename "$top")
slot=$(printf '%s' "$slot" | tr -c 'A-Za-z0-9._-' '-')

# The snapshot: HEAD's index plus every tracked change and untracked, not ignored file, in a temporary index.
tmp_index=$(mktemp)
# One EXIT trap for the whole script: the modes below that rewrite files in the slot's worktree add their undo to $undo_remote, so a run with
# both goldens and fingerprints puts both back (a trap set per mode would replace the one before it).
undo_remote=""
cleanup() {
  rm -f "$tmp_index"
  if [ -n "$undo_remote" ]; then ssh -o BatchMode=yes "$host" "$undo_remote" || true; fi
}
trap cleanup EXIT
cp "$(git rev-parse --git-path index)" "$tmp_index" || exit 2
GIT_INDEX_FILE=$tmp_index git add -u -- . || exit 2
git ls-files -z --others --exclude-standard | GIT_INDEX_FILE=$tmp_index xargs -0 -r git add -- || exit 2
tree=$(GIT_INDEX_FILE=$tmp_index git write-tree) || exit 2
head=$(git rev-parse HEAD) || exit 2
if [ "$tree" = "$(git rev-parse "HEAD^{tree}")" ]; then
  sha=$head
  echo "remote_ci: HEAD $(git rev-parse --short HEAD), clean tree"
else
  sha=$(git commit-tree "$tree" -p "$head" -m "remote_ci snapshot of $(git rev-parse --short HEAD)") || exit 2
  echo "remote_ci: snapshot $(git rev-parse --short "$sha") of HEAD $(git rev-parse --short HEAD) plus the working tree"
fi

envs=
if [ "$scope" = changed ]; then
  bb=${O3OS_CI_BASE_BRANCH:-}
  if [ -z "$bb" ]; then
    if git rev-parse --verify --quiet "refs/heads/dev^{commit}" > /dev/null; then bb=dev; else bb=main; fi
  fi
  base=$(git merge-base "$head" "$bb") || { echo "remote_ci: no merge-base with $bb" >&2; exit 2; }
  envs="O3OS_CI_SCOPE=changed O3OS_CI_BASE=$base "
fi

# OS-QANET B1: the random user's long run takes its seed range and step count from the environment (tests/qanet/CMakeLists.txt); numbers only, passed on to the machine's ci.sh.
for v in O3OS_QANET_SEED O3OS_QANET_SEEDS O3OS_QANET_STEPS; do
  val=${!v:-}
  case "$val" in
    "") ;;
    *[!0-9]*) echo "remote_ci: $v must be a number" >&2; exit 2 ;;
    *) envs="$envs$v=$val " ;;
  esac
done
# O3OS_REMOTE_JOBS: the machine's job count for this run, over its turn's default (a full dev run on a free machine). A name of
# its own, so an O3OS_CI_JOBS set for a local run never reaches the machine.
case "${O3OS_REMOTE_JOBS:-}" in
  "") ;;
  *[!0-9]*) echo "remote_ci: O3OS_REMOTE_JOBS must be a number" >&2; exit 2 ;;
  *) envs="${envs}O3OS_CI_JOBS=$O3OS_REMOTE_JOBS " ;;
esac
case "${O3OS_GOLDEN_ONLY:-}" in
  "") ;;
  *[!A-Za-z0-9_./-]*) echo "remote_ci: O3OS_GOLDEN_ONLY must be a scenario or screen name" >&2; exit 2 ;;
  *) envs="${envs}O3OS_GOLDEN_ONLY=$O3OS_GOLDEN_ONLY " ;;
esac

case "${O3OS_CI_LONG:-}" in
  ""|0) ;;
  1) envs="${envs}O3OS_CI_LONG=1 " ;;
  *) echo "remote_ci: O3OS_CI_LONG must be 1" >&2; exit 2 ;;
esac
case "${O3OS_CI_TESTS:-}" in
  "") ;;
  *[!A-Za-z0-9_.\|^\$*+?\(\)-]*) echo "remote_ci: O3OS_CI_TESTS may hold only letters, digits and _ . | ^ \$ * + ? ( ) -" >&2; exit 2 ;;
  *) envs="${envs}O3OS_CI_TESTS='$O3OS_CI_TESTS' " ;;
esac

git push -q "ssh://$host$root/repo.git" "$sha:refs/ci/$sha" || exit 255
echo "remote_ci: slot $slot, modes $*${scope:+, scope $scope}; waiting for the remote CI lock if another run holds it"
out=$(ssh -o BatchMode=yes "$host" "$envs$root/o3ci.sh $slot $sha $*")
rc=$?
printf '%s\n' "$out"
log=$(printf '%s\n' "$out" | head -1)
case "$log" in
  "$root"/logs/*.log)
    mkdir -p build-remote
    ssh -o BatchMode=yes "$host" "cat $log" > "build-remote/$(basename "$log")" &&
      echo "remote_ci: full log in build-remote/$(basename "$log")" ;;
esac

# The goldens mode rewrote sim/goldens.txt in the slot's worktree on the machine. Bring it back with the PNGs of the entries
# that changed (build-remote/goldens-<slot>/, to be looked at before committing), and put it in the working tree when the
# local file is still the one that was sent. The slot's own copy is then put back, or its next checkout would be refused.
case " $* " in
  *" goldens "*)
    wt=$root/wt-$slot
    undo_remote="$undo_remote git -C $wt checkout -q -- sim/goldens.txt;"
    [ $rc -eq 0 ] || exit $rc
    gdir=build-remote/goldens-$slot
    rm -rf "$gdir" && mkdir -p "$gdir" || exit 2
    ssh -o BatchMode=yes "$host" "cat $wt/sim/goldens.txt" > "$gdir/goldens.txt" || exit 255
    git show "$sha:sim/goldens.txt" > "$gdir/goldens.sent.txt" || exit 2
    python3 -I - "$gdir/goldens.sent.txt" "$gdir/goldens.txt" > "$gdir/changed.txt" <<'PY'
import sys
def load(p):
    return dict(l.split() for l in open(p) if l.strip() and not l.startswith("#"))
old, new = load(sys.argv[1]), load(sys.argv[2])
for key in sorted(new):
    if old.get(key) != new[key]:
        print(key)
for key in sorted(set(old) - set(new)):
    print("removed", key, file=sys.stderr)
PY
    n=$(wc -l < "$gdir/changed.txt")
    sed -e 's|^gallery/\(.*\)$|gallery/\1.png|' -e '/^gallery\//!s|^\(.*\)$|png/\1.png|' "$gdir/changed.txt" |
      ssh -o BatchMode=yes "$host" "cd $wt/build-os-ci-host/sim && tar cf - --ignore-failed-read -T - 2>/dev/null" | tar xf - -C "$gdir"
    echo "remote_ci: $n golden(s) changed; their PNGs are in $gdir/ (png/<scenario>/, gallery/), the list in $gdir/changed.txt"
    if cmp -s "$gdir/goldens.sent.txt" sim/goldens.txt; then
      cp "$gdir/goldens.txt" sim/goldens.txt && echo "remote_ci: sim/goldens.txt updated; look at the PNGs before committing it"
    else
      echo "remote_ci: sim/goldens.txt changed here since the run; the new one is $gdir/goldens.txt" >&2
    fi
    ;;
esac

# The fingerprints mode (OS-RATE) rewrote tests/audio_fingerprints/*.json in the slot's worktree: bring them back with fingerprint_diff.md
# (build-remote/fingerprints-<slot>/, to be read before committing) and put them in the working tree when the local files are still the ones
# that were sent. The slot's own copies are then put back, or its next checkout would be refused.
case " $* " in
  *" fingerprints "*)
    wt=$root/wt-$slot
    undo_remote="$undo_remote git -C $wt checkout -q -- tests/audio_fingerprints;"
    [ $rc -eq 0 ] || exit $rc
    fdir=build-remote/fingerprints-$slot
    rm -rf "$fdir" && mkdir -p "$fdir" || exit 2
    ssh -o BatchMode=yes "$host" "cd $wt && tar cf - tests/audio_fingerprints build-os-ci-host/sim/fingerprint/fingerprint_diff.md build-os-ci-host/sim/fingerprint/wav" | tar xf - -C "$fdir" || exit 255
    mv "$fdir/build-os-ci-host/sim/fingerprint/fingerprint_diff.md" "$fdir/fingerprint_diff.md" && mv "$fdir/build-os-ci-host/sim/fingerprint/wav" "$fdir/wav" && rm -rf "$fdir/build-os-ci-host"
    mkdir -p "$fdir/sent" && git archive "$sha" tests/audio_fingerprints | tar xf - -C "$fdir/sent" || exit 2
    echo "remote_ci: the rewritten files are in $fdir/tests/audio_fingerprints/, what moved in $fdir/fingerprint_diff.md, the renders in $fdir/wav/"
    if diff -rq "$fdir/sent/tests/audio_fingerprints" tests/audio_fingerprints > /dev/null; then
      cp "$fdir"/tests/audio_fingerprints/*.json tests/audio_fingerprints/ && echo "remote_ci: tests/audio_fingerprints/ updated; read fingerprint_diff.md before committing it"
    else
      echo "remote_ci: tests/audio_fingerprints/ changed here since the run; the new files are in $fdir/tests/audio_fingerprints/" >&2
    fi
    ;;
esac
exit $rc
