#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Merge a branch and resolve the generated files yourself (OS-MERGETOOL).

    python3 -I tools/o3merge.py <ref> -m "<message>"            git merge --no-ff --no-commit <ref>, resolve, check, commit
    python3 -I tools/o3merge.py --continue -m "<message>"       after you resolved and `git add`ed the other conflicts
    python3 -I tools/o3merge.py <ref> -m "..." --no-commit      stop with everything resolved and staged, do not commit
    --allow-detached                                            allow a detached HEAD (to replay an old merge in a worktree)

Merging a branch into dev (or dev into a branch) stops on the same generated files every time, resolved by hand the same way:
docs/manual/sync.lock, docs/qa/catalogue/README.md, docs/manual/llms.txt and docs/manual/llms-full.txt. This tool does it:

  1. Preflight: on a branch (not detached, unless --allow-detached), the tree clean for tracked files, no merge in progress (unless
     --continue). It prints the branch and HEAD. A ref that is not a commit, or a merge that leaves no MERGE_HEAD, is an error: exit 1,
     nothing left half done.
  2. Unmerged paths outside the generated set are printed with the way out ("resolve these by hand, git add them, then run
     tools/o3merge.py --continue -m ..."); the merge is left IN PROGRESS, exit 2. Nothing is regenerated while other conflicts remain
     (the chapters may hold conflict markers).
  3. docs/manual/sync.lock: a three-way merge of the index stages (:1: base, :2: ours, :3: theirs; a missing stage 1 is an empty base).
     Header lines (#) from ours; one entry line per key (<chapter>#<section>), items <item>=<hash>. Per key, a side that equals the base
     loses to the side that changed (absent = removed); equal changes are taken; when both changed differently, the items merge with the
     same rule, and an item that conflicts (two different new hashes, or removed on one side and changed on the other) takes theirs
     provisionally and its key goes to a re-ack list, then `tools/manual_check.py --ack <key>` runs for every key of that list (both sides
     acknowledged their own change, so the merged content is acknowledged). The file is written exactly as tools/manual_check.py writes
     it (its lock_text is imported from the repository's tools/, so this script also works on an older checkout). Why that is correct:
     an item one branch changed was acknowledged by that branch (its gate forced it), so the item's hash from that branch is the merged
     content's hash; only items both branches changed have new content.
  4. docs/qa/catalogue/README.md: take any side, then `tools/qa_cover.py --write`.
  5. docs/manual/llms*.txt: last, `tools/manual_llms.py` (rewrites both).
  6. Also after a merge with no conflict in them: `tools/qa_cover.py --check` and `tools/manual_llms.py --check`, regenerated when stale.
  7. Before the commit: `tools/manual_check.py --strict`, `tools/qa_cover.py --check` and `tools/manual_llms.py --check` must pass
     (`tools/manual_check.py --gate` is printed for information), and `git diff --cached --check` must be clean: no leftover conflict
     marker in ANY staged file, the hand-resolved ones included. Any failure: printed, the merge left in progress, exit 3.
  8. Each generated file is staged (`git add <path>`) as soon as the tool has written it, so a failure later still leaves a clean tree
     that --continue accepts. No unmerged path may remain, then the commit with the given message, verbatim (multi-line allowed).
  9. A summary at the end: what was auto-resolved, the re-acked sections, the "possibly stale after the merge" block (information
     only, never a failure: the sections whose cited file or QA row changed on the merged branch since the merge base while that
     branch's lock entry for it did not), and the commit hash.

The repository is the one of the current directory (git rev-parse --show-toplevel); the helper tools are run from THAT root's tools/.

Exit codes: 0 resolved (and committed, unless --no-commit), 1 preflight or merge error, 2 other conflicts remain (merge left in
progress), 3 a check failed or a staged file has leftover conflict markers (merge left in progress).

Stdlib only.
"""

import argparse
import importlib.util
import os
import shlex
import subprocess
import sys

LOCK = "docs/manual/sync.lock"
README = "docs/qa/catalogue/README.md"
LLMS = ("docs/manual/llms.txt", "docs/manual/llms-full.txt")
GENERATED = (LOCK, README) + LLMS


def git(root, *args):
    """(rc, stdout, stderr) of a git command in `root`."""
    r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def git_out(root, *args):
    rc, out, _ = git(root, *args)
    return out if rc == 0 else None


def die(message, code):
    print("o3merge: " + message, file=sys.stderr)
    sys.exit(code)


def unmerged_paths(root):
    return [p for p in (git_out(root, "diff", "--name-only", "--diff-filter=U") or "").splitlines() if p]


def load_tool(root, name):
    """Import <root>/tools/<name>.py (the repository's own copy, so this script works on an older checkout too)."""
    path = os.path.join(root, "tools", name + ".py")
    spec = importlib.util.spec_from_file_location(name.replace(".", "_"), path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_tool(root, name, *args):
    """Run <root>/tools/<name> with python3 -I, as the manual says; (rc, stdout, stderr)."""
    r = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", name), "--root", root] + list(args),
                       capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def show_tool(root, name, *args):
    rc, out, err = run_tool(root, name, *args)
    if out:
        print(out, end="")
    if err:
        print(err, end="", file=sys.stderr)
    return rc


# ---- The three-way merge of docs/manual/sync.lock -------------------------------------------------------------------------------------

def parse_lock_text(text):
    """{key: {item: hash}} from a sync.lock text (header lines and blanks skipped)."""
    lock = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split()
        items = {}
        for p in parts[1:]:
            k, _, v = p.rpartition("=")
            items[k] = v
        lock[parts[0]] = items
    return lock


def merge_lock(base, ours, theirs):
    """(merged {key: {item: hash}}, re-ack list) of the three lock versions. Per key a side that equals the base loses to the side that
    changed (absent = removed); equal changes are taken; when both changed differently the items merge with the same rule, and an item
    that conflicts (two different new hashes, or removed on one side and changed on the other) takes theirs provisionally and puts its
    key on the re-ack list."""
    merged, reack = {}, []
    for key in sorted(set(base) | set(ours) | set(theirs)):
        b, o, t = base.get(key), ours.get(key), theirs.get(key)
        if o == t:
            if o is not None:
                merged[key] = o
            continue
        if o == b:
            if t is not None:
                merged[key] = t
            continue
        if t == b:
            if o is not None:
                merged[key] = o
            continue
        items, conflict = {}, False
        for item in sorted(set(b or {}) | set(o or {}) | set(t or {})):
            bb, oo, tt = (b or {}).get(item), (o or {}).get(item), (t or {}).get(item)
            if oo == tt:
                v = oo
            elif oo == bb:
                v = tt
            elif tt == bb:
                v = oo
            else:
                v = tt  # theirs, provisionally: --ack rewrites the whole line from the merged tree
                conflict = True
            if v is not None:
                items[item] = v
        if items:
            merged[key] = items
        if conflict:
            reack.append(key)
    return merged, reack


def merge_lock_texts(base_text, ours_text, theirs_text, lock_text):
    """(the merged lock file's text, re-ack list). The header lines come from ours; the entry lines are exactly what lock_text (the
    writer of tools/manual_check.py) writes, in its order."""
    base, ours, theirs = (parse_lock_text(t) for t in (base_text, ours_text, theirs_text))
    merged, reack = merge_lock(base, ours, theirs)
    header = [l for l in ours_text.splitlines() if l.startswith("#")]
    if not header:
        header = [l for l in lock_text({}).splitlines() if l.startswith("#")]
    entries = [l for l in lock_text(merged).splitlines() if l and not l.startswith("#")]
    return "\n".join(header + entries) + "\n", reack


def stage_text(root, path, n):
    """The text of index stage n (1 base, 2 ours, 3 theirs) of an unmerged path, or "" when the stage is missing."""
    rc, out, _ = git(root, "show", ":%d:%s" % (n, path))
    return out if rc == 0 else ""


_ROWS_AT = {}


def rows_at(root, rev, mc):
    """{QA ID: row text} of the catalogue files at a commit (parsed once per commit: the stale summary asks for every QA item)."""
    key = (root, rev)
    if key not in _ROWS_AT:
        texts = []
        for f in (git_out(root, "ls-tree", "-r", "--name-only", rev, "docs/qa/catalogue") or "").splitlines():
            if f.endswith(".md"):
                texts.append(git_out(root, "show", "%s:%s" % (rev, f)) or "")
        _ROWS_AT[key] = mc.qa_rows(texts)
    return _ROWS_AT[key]


def moved_on_other_side(root, base_rev, other_rev, item, mc, changed=None):
    """True when the cited file or QA row differs between the merge base and the other side of the merge (a manual page by its
    digest, so a change inside its generated tables does not count). `changed`, when given, is the set of paths that differ
    between the two commits: a file outside it has not moved."""
    if mc.QA_RE.fullmatch(item):
        return rows_at(root, base_rev, mc).get(item) != rows_at(root, other_rev, mc).get(item)
    if os.path.isdir(os.path.join(root, item)):
        if changed is not None:
            prefix = item.rstrip("/") + "/"
            return any(p.startswith(prefix) for p in changed)
        return bool((git_out(root, "diff", "--name-only", base_rev, other_rev, "--", item) or "").strip())
    if changed is not None and item not in changed:
        return False
    a, b = mc.blob_at(root, base_rev, item), mc.blob_at(root, other_rev, item)
    if a is None or b is None:
        return a != b
    return mc.digest(item, a) != mc.digest(item, b)


def possibly_stale(root, mc):
    """[(key, [items])]: the sections whose lock entry the merged branch did not change although the cited file or QA row moved on
    that branch since the merge base. Information only, never a failure."""
    base_rev = (git_out(root, "merge-base", "HEAD", "MERGE_HEAD") or "").strip()
    if not base_rev:
        return []
    base_lock = parse_lock_text(git_out(root, "show", "%s:%s" % (base_rev, LOCK)) or "")
    theirs_lock = parse_lock_text(git_out(root, "show", "MERGE_HEAD:%s" % LOCK) or "")
    with open(os.path.join(root, LOCK), encoding="utf-8") as f:
        now_lock = parse_lock_text(f.read())
    changed = set((git_out(root, "diff", "--name-only", base_rev, "MERGE_HEAD") or "").splitlines())
    out = []
    for key in sorted(now_lock):
        items = [i for i in sorted(now_lock[key])
                 if base_lock.get(key, {}).get(i) == theirs_lock.get(key, {}).get(i)
                 and moved_on_other_side(root, base_rev, "MERGE_HEAD", i, mc, changed)]
        if items:
            out.append((key, items))
    return out


# ---- The merge --------------------------------------------------------------------------------------------------------------------------

def preflight(args):
    """(root, branch, head). Dies with exit 1 on any preflight fault."""
    out = git_out(os.getcwd(), "rev-parse", "--show-toplevel")
    if out is None:
        die("the current directory is not inside a git repository", 1)
    root = out.strip()
    branch = (git_out(root, "rev-parse", "--abbrev-ref", "HEAD") or "").strip()
    head = (git_out(root, "rev-parse", "HEAD") or "").strip()
    merging = git_out(root, "rev-parse", "-q", "--verify", "MERGE_HEAD") is not None
    if branch == "HEAD" and not args.allow_detached:
        die("HEAD is detached (%s): check out a branch, or pass --allow-detached to replay an old merge" % head[:12], 1)
    if args.cont:
        if not merging:
            die("--continue but no merge is in progress (no MERGE_HEAD)", 1)
        unmerged = set(unmerged_paths(root))
        dirty = [p for p in (git_out(root, "diff", "--name-only") or "").splitlines() if p and p not in unmerged]
    else:
        if merging:
            die("a merge is already in progress: resolve it and commit, or run this tool with --continue", 1)
        dirty = [p for p in (git_out(root, "diff", "--name-only", "HEAD") or "").splitlines() if p]
    if dirty:
        die("the tree has uncommitted changes to tracked files: %s" % ", ".join(dirty[:10]), 1)
    print("o3merge: branch %s, HEAD %s, repository %s" % (branch, head[:12], root))
    return root, branch, head


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ref", nargs="?", help="the ref to merge in (git merge --no-ff --no-commit <ref>)")
    ap.add_argument("--continue", dest="cont", action="store_true", help="a merge is in progress and the other conflicts are resolved and added")
    ap.add_argument("-m", "--message", required=True, help="the commit message, verbatim (multi-line allowed)")
    ap.add_argument("--no-commit", action="store_true", help="stop with everything resolved and staged, do not commit")
    ap.add_argument("--allow-detached", action="store_true", help="allow a detached HEAD (to replay an old merge)")
    args = ap.parse_args(argv)
    if args.cont and args.ref:
        ap.error("--continue takes no ref")
    if not args.cont and not args.ref:
        ap.error("a ref to merge is required (or --continue)")

    root, branch, head = preflight(args)

    if not args.cont:
        if git_out(root, "rev-parse", "-q", "--verify", args.ref + "^{commit}") is None:
            die("%s is not a commit" % args.ref, 1)
        rc, out, err = git(root, "merge", "--no-ff", "--no-commit", args.ref)
        if out:
            print(out, end="")
        if err:
            print(err, end="", file=sys.stderr)
        if git_out(root, "rev-parse", "-q", "--verify", "MERGE_HEAD") is None:
            if rc == 0:
                die("%s is already contained in HEAD, nothing to merge" % args.ref, 1)
            die("git merge failed (exit %d), nothing was changed" % rc, 1)

    # Rule 2: unmerged paths outside the generated set stop here, merge left in progress.
    unmerged = unmerged_paths(root)
    outside = [p for p in unmerged if p not in GENERATED]
    if outside:
        print("o3merge: unmerged paths outside the generated set:")
        for p in outside:
            print("  " + p)
        print("Resolve them by hand, `git add` them, then run:")
        print("  python3 -I tools/o3merge.py --continue -m %s" % shlex.quote(args.message))
        print("The merge is left in progress.")
        return 2

    # Rules 3 to 6: resolve the generated files. Each one is staged as soon as the tool has written it, so a later failure (exit 3)
    # still leaves a clean tree that --continue accepts.
    reacked = []
    resolved = []
    mc = load_tool(root, "manual_check")

    def stage(path):
        rc, _, err = git(root, "add", "--", path)
        if rc != 0:
            print("o3merge: git add %s failed: %s" % (path, err.strip()), file=sys.stderr)
            return False
        return True

    if LOCK in unmerged:
        text, reack = merge_lock_texts(stage_text(root, LOCK, 1), stage_text(root, LOCK, 2), stage_text(root, LOCK, 3), mc.lock_text)
        with open(os.path.join(root, LOCK), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        resolved.append("%s: three-way merge of the index stages (%d section(s))" % (LOCK, len(parse_lock_text(text))))
        if reack:
            print("o3merge: re-acknowledging %d section(s) whose items both sides changed: %s" % (len(reack), ", ".join(reack)))
            rc = show_tool(root, "manual_check.py", "--ack", *reack)
            if rc != 0:
                print("o3merge: tools/manual_check.py --ack failed; the merge is left in progress", file=sys.stderr)
                return 3
            reacked = reack
        if not stage(LOCK):
            return 3

    if README in unmerged:
        side = stage_text(root, README, 2) or stage_text(root, README, 3)
        with open(os.path.join(root, README), "w", encoding="utf-8", newline="\n") as f:
            f.write(side)
        rc = show_tool(root, "qa_cover.py", "--write")
        if rc != 0:
            print("o3merge: tools/qa_cover.py --write failed; the merge is left in progress", file=sys.stderr)
            return 3
        if not stage(README):
            return 3
        resolved.append("%s: one side, then tools/qa_cover.py --write" % README)

    llms_unmerged = [p for p in LLMS if p in unmerged]
    if llms_unmerged:
        rc = show_tool(root, "manual_llms.py")
        if rc != 0:
            print("o3merge: tools/manual_llms.py failed; the merge is left in progress", file=sys.stderr)
            return 3
        for p in LLMS:
            if not stage(p):
                return 3
        resolved.append("%s and %s: tools/manual_llms.py" % LLMS)

    # Rule 6: no conflict in them -> check, regenerate when stale.
    if README not in unmerged:
        rc = show_tool(root, "qa_cover.py", "--check")
        if rc != 0:
            if show_tool(root, "qa_cover.py", "--write") != 0:
                print("o3merge: tools/qa_cover.py --write failed; the merge is left in progress", file=sys.stderr)
                return 3
            if not stage(README):
                return 3
            resolved.append("%s: regenerated (tools/qa_cover.py --check said stale)" % README)
    if not llms_unmerged:
        rc = show_tool(root, "manual_llms.py", "--check")
        if rc != 0:
            if show_tool(root, "manual_llms.py") != 0:
                print("o3merge: tools/manual_llms.py failed; the merge is left in progress", file=sys.stderr)
                return 3
            for p in LLMS:
                if not stage(p):
                    return 3
            resolved.append("%s and %s: regenerated (tools/manual_llms.py --check said stale)" % LLMS)

    stale = possibly_stale(root, mc)

    def print_stale():
        if stale:
            print("o3merge: possibly stale after the merge (information only, never a failure): %d section(s) cite something the "
                  "merged branch changed without re-acknowledging it:" % len(stale))
            for key, items in stale:
                print("  %s: %s" % (key, ", ".join(items)))

    # Rule 7: the checks before the commit.
    print("o3merge: checks before the commit:")
    failed = []
    for name, extra in (("manual_check.py", ["--strict"]), ("qa_cover.py", ["--check"]), ("manual_llms.py", ["--check"])):
        print("== tools/%s %s" % (name, " ".join(extra)))
        if show_tool(root, name, *extra) != 0:
            failed.append("tools/%s %s" % (name, " ".join(extra)))
    print("== tools/manual_check.py --gate (information)")
    show_tool(root, "manual_check.py", "--gate")
    if failed:
        print_stale()
        print("o3merge: FAILED: %s; the merge is left in progress: fix it, then run tools/o3merge.py --continue -m %s"
              % ("; ".join(failed), shlex.quote(args.message)), file=sys.stderr)
        return 3

    # Rule 8: no unmerged path left, no leftover conflict marker in any staged file (hand-resolved ones included), then commit.
    left = unmerged_paths(root)
    if left:
        print("o3merge: unmerged paths left: %s; the merge is left in progress" % ", ".join(left), file=sys.stderr)
        return 3
    rc, out, _ = git(root, "diff", "--cached", "--check")
    if rc != 0:
        print("o3merge: leftover conflict markers in staged files (git diff --cached --check):", file=sys.stderr)
        print(out, end="", file=sys.stderr)
        print("o3merge: the merge is left in progress: fix them, git add, then run tools/o3merge.py --continue -m %s"
              % shlex.quote(args.message), file=sys.stderr)
        return 3

    print("o3merge: auto-resolved:")
    for line in resolved:
        print("  " + line)
    if not resolved:
        print("  (nothing: the merge had no conflict in the generated files)")
    if reacked:
        print("o3merge: re-acknowledged sections (an item both sides changed): %s" % ", ".join(reacked))
    print_stale()

    if args.no_commit:
        print("o3merge: --no-commit: everything is resolved and staged, the merge is still in progress (HEAD %s)" % head[:12])
        return 0
    rc, out, err = git(root, "commit", "-m", args.message)
    if out:
        print(out, end="")
    if err:
        print(err, end="", file=sys.stderr)
    if rc != 0:
        print("o3merge: git commit failed (exit %d); the merge is left in progress, everything resolved and staged" % rc, file=sys.stderr)
        return 1
    commit = (git_out(root, "rev-parse", "HEAD") or "").strip()
    print("o3merge: committed %s on %s" % (commit[:12], branch))
    return 0


if __name__ == "__main__":
    sys.exit(main())
