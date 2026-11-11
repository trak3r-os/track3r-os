#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The only path from a reviewed branch to dev (OS-OPS-LAND).

    python3 -I tools/land.py <branch>... [--title "<summary>"] [--modes "host"] [--no-ci "<reason>"]
                                         [--no-push] [--dry-run] [--allow-local]

Run it from any checkout or worktree of the repository, after the branch is pushed (and has merged dev, with the gates green).
It never merges in the main checkout, so a stopped merge or a detached HEAD cannot land there:

  1. Lock: holds flock on <git common dir>/land.lock for the whole run, waiting if another run holds it (one line is printed).
  2. Main checkout (the first worktree of `git worktree list`): must be on branch dev, with no MERGE_HEAD and a clean tracked tree.
     `git fetch origin`; dev behind origin/dev is fast-forwarded; `origin/dev..dev` not empty aborts and lists those commits
     (--allow-local pushes them with the merge).
  3. Per branch the ref is origin/<branch>; a local branch ahead of it aborts ("push it first"). The ID is the leading run of
     [A-Z0-9]+ segments joined by `-` after `<type>/` (chore/OS-OPS-CIISO gives OS-OPS-CIISO, fix/OS-REL130-spdx gives OS-REL130).
     Summary: --title (one branch only), else the subject of the branch's first commit after the merge base with dev, without its
     `type(area): ` prefix and its trailing ` [ID]`. Commit title: `merge: <summary> [<ID>]`, body `Landed-by: tools/land.py`.
  4. Merges happen in the integration worktree <main>/.claude/worktrees/land on branch land/work (reset to dev), one
     tools/o3merge.py run per branch (it resolves the generated files). A failure aborts, leaves the worktree as it is for
     inspection and dev untouched. O3OS_LAND_MERGE=git uses plain `git merge --no-ff -m` (tests).
  5. CI on the merge result, in the land worktree: `tools/remote_ci.sh --slot land <modes>` (default host; the remote green cache
     makes an already tested tree instant). Non-zero aborts, dev untouched. No remote (exit 3 or 255) aborts with the local way out:
     run tools/ci.sh <modes> in the land worktree, then rerun with --no-ci "local ci.sh rc=0". --no-ci needs a reason, printed in
     the summary. O3OS_LAND_CI_CMD (a shell command) replaces the CI command (tests).
  6. Only then (a failure after the fast-forward says "dev advanced locally to <sha>; run: git push origin dev"): `git merge --ff-only land/work` in the main checkout (HEAD and the last title are verified), and unless --no-push
     `git push origin dev` (origin/dev is verified). One line per landed merge, then the pushed range. If origin/dev moved during
     the CI the run aborts before touching dev: rerun it.
  --dry-run does the merges in the land worktree, prints the titles and the plan, and stops before CI.

Exit codes: 0 landed (or dry run done), 1 aborted (dev and origin untouched unless the message says otherwise), 2 bad usage,
3 no remote CI available.

Stdlib only.
"""

import argparse
import fcntl
import os
import re
import subprocess
import sys

TRAILER = "Landed-by: tools/land.py"


class Abort(Exception):
    def __init__(self, msg, code=1):
        super().__init__(msg)
        self.code = code


def say(msg):
    print(msg, flush=True)


def git(cwd, *args, check=True):
    """Run git in cwd; return stdout without the trailing newline."""
    p = subprocess.run(["git", "-C", cwd, *args], capture_output=True, text=True)
    if check and p.returncode != 0:
        raise Abort("git %s failed in %s: %s" % (" ".join(args), cwd, (p.stderr or p.stdout).strip()))
    return p.stdout.rstrip("\n") if check else p


def acquire_lock(path, say_fn=say):
    """Take an exclusive flock on path, waiting; say one line when it is busy. Returns the open file (keep it)."""
    f = open(path, "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        say_fn("land: waiting for the lock %s (another land.py run is active)" % path)
        fcntl.flock(f, fcntl.LOCK_EX)
    return f


def branch_id(branch):
    """The leading run of [A-Z0-9]+ segments after '<type>/', joined by '-'; None when there is none."""
    name = branch.split("/", 1)[1] if "/" in branch else branch
    segs = []
    for s in name.split("-"):
        if not re.fullmatch(r"[A-Z0-9]+", s):
            break
        segs.append(s)
    return "-".join(segs) or None


def clean_subject(subject):
    s = re.sub(r"^[a-z]+(\([^)]*\))?!?: ", "", subject)
    return re.sub(r" \[[^\]]+\]$", "", s)


def merge_title(summary, bid):
    return "merge: %s [%s]" % (summary, bid)


def main_checkout():
    out = git(os.getcwd(), "worktree", "list", "--porcelain")
    first = out.split("\n\n", 1)[0]
    for line in first.splitlines():
        if line.startswith("worktree "):
            return os.path.realpath(line[len("worktree "):])
    raise Abort("cannot find the main checkout in `git worktree list`")


def check_main(main):
    br = git(main, "symbolic-ref", "--short", "-q", "HEAD", check=False)
    if br.returncode != 0 or br.stdout.strip() != "dev":
        raise Abort("the main checkout %s is not on branch dev (HEAD: %s)" % (
            main, br.stdout.strip() or "detached"))
    if git(main, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).returncode == 0:
        raise Abort("the main checkout %s has a merge in progress (MERGE_HEAD)" % main)
    if git(main, "status", "--porcelain", "--untracked-files=no"):
        raise Abort("the main checkout %s has uncommitted changes to tracked files" % main)


def sync_dev(main, allow_local):
    git(main, "fetch", "origin")
    if git(main, "merge-base", "--is-ancestor", "dev", "origin/dev", check=False).returncode == 0:
        if git(main, "rev-parse", "dev") != git(main, "rev-parse", "origin/dev"):
            say("land: dev is behind origin/dev, fast-forwarding")
            git(main, "merge", "--ff-only", "-q", "origin/dev")
        return
    local = git(main, "log", "--format=%h %s", "origin/dev..dev")
    if not allow_local or git(main, "merge-base", "--is-ancestor", "origin/dev", "dev", check=False).returncode != 0:
        raise Abort("dev has local commits that origin/dev lacks (--allow-local pushes them with the merge; "
                    "a diverged dev needs the maintainer):\n" + local)
    say("land: --allow-local, these local commits go with the push:\n" + local)


def check_branch(main, branch):
    ref = "origin/" + branch
    if git(main, "rev-parse", "-q", "--verify", "refs/remotes/" + ref, check=False).returncode != 0:
        raise Abort("%s does not exist: push the branch first" % ref)
    if git(main, "rev-parse", "-q", "--verify", "refs/heads/" + branch, check=False).returncode == 0:
        if git(main, "rev-list", ref + "..refs/heads/" + branch):
            raise Abort("local branch %s is ahead of %s: push it first" % (branch, ref))
    return ref


def first_subject(main, ref):
    base = git(main, "merge-base", "dev", ref)
    subjects = git(main, "log", "--no-merges", "--reverse", "--format=%s", base + ".." + ref).splitlines()
    if not subjects:
        raise Abort("%s has no commit beyond dev" % ref)
    return subjects[0]


def land_worktree(main):
    path = os.path.join(main, ".claude", "worktrees", "land")
    git(main, "worktree", "prune")
    if not os.path.isdir(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        git(main, "worktree", "add", "--detach", path, "dev")
    else:
        # A plain directory inside the main checkout also answers `rev-parse --git-dir`: verify it is a worktree of its own.
        top = git(path, "rev-parse", "--show-toplevel", check=False)
        listed = [os.path.realpath(l[len("worktree "):]) for l in
                  git(main, "worktree", "list", "--porcelain").splitlines() if l.startswith("worktree ")]
        real = os.path.realpath(path)
        if (top.returncode != 0 or os.path.realpath(top.stdout.strip()) != real or real == main
                or real not in listed[1:]):
            raise Abort("%s exists but is not a worktree of its own (nothing was reset); remove it or fix it" % path)
    if git(path, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).returncode == 0:
        git(path, "merge", "--abort")
    git(path, "switch", "-q", "-f", "-C", "land/work", "dev")
    git(path, "clean", "-fdq")
    return path


def do_merge(land, ref, title):
    msg = title + "\n\n" + TRAILER
    if os.environ.get("O3OS_LAND_MERGE") == "git":
        cmd = ["git", "-C", land, "merge", "--no-ff", "-m", msg, ref]
    else:
        cmd = [sys.executable, "-I", os.path.join(land, "tools", "o3merge.py"), ref, "-m", msg]
    before = git(land, "rev-parse", "HEAD")
    p = subprocess.run(cmd, cwd=land)
    if p.returncode != 0:
        raise Abort("merging %s failed (exit %d); the worktree %s is left as it is, dev is untouched" % (
            ref, p.returncode, land))
    after = git(land, "rev-parse", "HEAD")
    if after == before or git(land, "rev-parse", "-q", "--verify", "HEAD^2", check=False).returncode != 0:
        raise Abort("%s produced no merge commit (already in dev?); dev is untouched" % ref)
    return after


def run_ci(land, modes):
    cmd = os.environ.get("O3OS_LAND_CI_CMD")
    if cmd:
        rc = subprocess.run(cmd, shell=True, cwd=land).returncode
    else:
        rc = subprocess.run([os.path.join(land, "tools", "remote_ci.sh"), "--slot", "land", *modes.split()],
                            cwd=land).returncode
    if rc in (3, 255):
        raise Abort("no remote CI (exit %d). Run tools/ci.sh %s in %s, then rerun with "
                    "--no-ci 'local ci.sh rc=0'" % (rc, modes, land), 3)
    if rc != 0:
        raise Abort("CI failed (exit %d) on the merge result; dev and origin are untouched; inspect %s" % (rc, land))


def land(args):
    main = main_checkout()
    common = git(main, "rev-parse", "--path-format=absolute", "--git-common-dir")
    lock = acquire_lock(os.path.join(common, "land.lock"))  # held until the process exits
    check_main(main)
    sync_dev(main, args.allow_local)
    start = git(main, "rev-parse", "dev")

    plan = []
    for b in args.branches:
        bid = branch_id(b)
        if not bid:
            raise Abort("cannot read an ID from the branch name %s" % b, 2)
        ref = check_branch(main, b)
        summary = args.title if args.title else clean_subject(first_subject(main, ref))
        plan.append((ref, merge_title(summary, bid)))

    land_wt = land_worktree(main)
    merged = []
    for ref, title in plan:
        merged.append((do_merge(land_wt, ref, title), title))
    work = git(land_wt, "rev-parse", "HEAD")

    if args.dry_run:
        say("land: dry run, nothing touched but the worktree %s (land/work at %s)" % (land_wt, work[:9]))
        for sha, title in merged:
            say("  would land %s %s" % (sha[:9], title))
        say("  next: CI (%s), ff-only dev, %s" % ("skipped: " + args.no_ci if args.no_ci else "tools/remote_ci.sh --slot land " + args.modes,
                                                   "no push" if args.no_push else "push origin dev"))
        return 0

    if args.no_ci:
        say("land: CI skipped, reason: %s" % args.no_ci)
    else:
        run_ci(land_wt, args.modes)

    git(main, "fetch", "origin")
    check_main(main)
    if git(main, "rev-parse", "dev") != start or (not args.allow_local and git(main, "rev-parse", "origin/dev") != start):
        raise Abort("dev or origin/dev moved during the run; dev is untouched, rerun land.py")
    git(main, "merge", "--ff-only", "-q", "land/work")
    head = git(main, "rev-parse", "HEAD")
    try:
        if head != work:
            raise Abort("dev is at %s, expected %s after the fast-forward" % (head[:9], work[:9]))
        if git(main, "log", "-1", "--format=%s") != merged[-1][1]:
            raise Abort("the subject of dev's HEAD is not the expected title %r" % merged[-1][1])
        for sha, title in merged:
            say("land: landed %s %s%s" % (sha[:9], title, " (no CI: %s)" % args.no_ci if args.no_ci else ""))

        if args.no_push:
            say("land: not pushed (--no-push): dev %s..%s is local" % (start[:9], head[:9]))
            return 0
        git(main, "push", "origin", "dev")
        remote = git(main, "ls-remote", "origin", "refs/heads/dev").split()[0]
        if remote != head:
            raise Abort("origin/dev is %s after the push, expected %s" % (remote[:9], head[:9]))
        say("land: pushed origin/dev %s..%s" % (start[:9], head[:9]))
    except Abort as e:
        raise Abort("%s\ndev advanced locally to %s; run: git push origin dev (after checking it)" % (e, head[:9]), e.code)
    return 0


def main():
    ap = argparse.ArgumentParser(prog="land.py", description="Land reviewed branches on dev: merge in the land worktree, CI, ff-only, push.")
    ap.add_argument("branches", nargs="+", metavar="branch", help="branch names as pushed to origin (feat/OS-X-slug)")
    ap.add_argument("--title", help="summary for the merge title (one branch only)")
    ap.add_argument("--modes", default="host", help="tools/remote_ci.sh modes (default: host)")
    ap.add_argument("--no-ci", metavar="REASON", help="skip CI; the reason is printed in the summary")
    ap.add_argument("--no-push", action="store_true", help="fast-forward dev but do not push")
    ap.add_argument("--dry-run", action="store_true", help="merge in the land worktree, print the plan, stop before CI")
    ap.add_argument("--allow-local", action="store_true", help="accept local dev commits that origin/dev lacks (they are pushed)")
    args = ap.parse_args()
    if args.title and len(args.branches) != 1:
        ap.error("--title takes exactly one branch")
    if args.no_ci is not None and not args.no_ci.strip():
        ap.error("--no-ci needs a reason")
    try:
        return land(args)
    except Abort as e:
        print("land: ABORT: %s" % e, file=sys.stderr, flush=True)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
