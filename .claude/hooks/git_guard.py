#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PreToolUse hook (Bash): keep subagents off the main checkout's git state (OS-OPS-GITGUARD).

Only calls that come from a subagent (the hook input has agent_id) are checked.
 1. In the main checkout (not a linked worktree under .claude/worktrees/) a
    subagent may not run git commands that move HEAD, the index or refs
    (checkout, switch, merge, rebase, reset, commit, stash, pull, cherry-pick,
    revert, am, restore, clean, tag, branch -d/-m/-f). Read-only git is fine.
 2. Anywhere, a subagent may not push to dev or main.
The effective directory of a git command is a preceding `cd <dir>` segment,
`git -C <dir>`, or the call's cwd. Fails open: any error means exit 0, no output.
"""
# Imports of re, shlex and subprocess are lazy: most calls contain no git and exit early (startup time).
import json
import os
import sys

DENY_MAIN = {"checkout", "switch", "merge", "rebase", "reset", "commit", "stash", "pull",
             "cherry-pick", "revert", "am", "restore", "clean"}
BRANCH_BAD_LONG = {"--delete", "--move", "--force"}
BRANCH_BAD_SHORT = set("dDmMf")
TAG_READ = {"-l", "--list", "-v", "--verify", "-n", "--contains", "--points-at",
            "--merged", "--no-merged", "--sort", "--format"}
PROTECTED = ("dev", "main")
PUSH_VALUE_OPTS = {"--repo", "-o", "--push-option", "--receive-pack", "--exec", "--signed"}
WRAPPERS = {"env", "nice", "timeout", "command", "time", "nohup"}

MSG_MAIN = ("Subagents may not change the main checkout's git state (git %s). "
            "Read with `git diff dev...origin/<branch>` or `git show origin/<branch>:<path>`; "
            "work in your own worktree.")
MSG_PUSH = "Pushing dev or main is the orchestrator's."


def split_commands(cmd):
    """Split on && ; | newline outside quotes; None when a quote is left open."""
    parts, cur, q, i = [], [], None, 0
    while i < len(cmd):
        c = cmd[i]
        if q:
            if c == q:
                q = None
            cur.append(c)
        elif c in "'\"":
            q = c
            cur.append(c)
        elif c in ";|\n" or cmd.startswith("&&", i):
            parts.append("".join(cur))
            cur = []
            i += 1 if c in ";|\n" else 2
            continue
        else:
            cur.append(c)
        i += 1
    if q:
        return None
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def strip_wrappers(w):
    """Drop leading env VAR=.., nice -n N, timeout N and similar; returns (argv, env dict)."""
    import re
    env = {}

    def var(t):
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", t)
        if m:
            env[m.group(1)] = m.group(2)
        return bool(m)

    while w:
        t = w[0]
        if var(t):
            w = w[1:]
        elif t in WRAPPERS:
            w = w[1:]
            while w:
                if w[0] in ("-n", "-s", "-k", "-u") and len(w) > 1:
                    w = w[2:]
                elif w[0].startswith("-") or var(w[0]) or re.match(r"^\d+[smhd]?$", w[0]):
                    w = w[1:]
                else:
                    break
        else:
            break
    return w, env


def real(p):
    return os.path.realpath(p)


def join(base, t):
    """base/t, or None (unknown dir) when base is unknown or t has $, a backtick or a glob."""
    if base is None or any(c in t for c in "$`*?["):
        return None
    t = os.path.expanduser(t)
    return t if os.path.isabs(t) else os.path.join(base, t)


def is_main(d, project):
    if d is None or project is None:
        return False
    r = real(d)
    if r != project and not r.startswith(project + os.sep):
        return False
    wt = os.path.join(project, ".claude", "worktrees")
    return not (r == wt or r.startswith(wt + os.sep))


def current_branch(d):
    if d is None:
        return ""
    try:
        import subprocess
        r = subprocess.run(["git", "-C", d, "rev-parse", "--abbrev-ref", "HEAD"],
                           capture_output=True, text=True, timeout=2)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def parse_git(w, cwd, env):
    """w starts after 'git'. Returns (effective dir or None when unknown, subcommand, args)."""
    d = cwd
    work_tree, git_dir = env.get("GIT_WORK_TREE"), env.get("GIT_DIR")
    i = 0
    while i < len(w) and w[i].startswith("-"):
        o = w[i]
        val = None
        if "=" in o and o.startswith("--"):
            o, val = o.split("=", 1)
            step = 1
        elif o in ("-C", "-c", "--git-dir", "--work-tree", "--namespace") and i + 1 < len(w):
            val, step = w[i + 1], 2
        else:
            step = 1
        if o == "-C" and val is not None:
            d = join(d, val)
        elif o == "--work-tree" and val is not None:
            work_tree = val
        elif o == "--git-dir" and val is not None:
            git_dir = val
        i += step
    if work_tree is not None:
        d = join(cwd, work_tree)
    elif git_dir is not None:
        d = join(cwd, git_dir)
        if d is not None and "/.git/worktrees/" in real(d) + "/":
            d = None  # the git dir of a linked worktree: not the main checkout
    if i >= len(w):
        return d, "", []
    return d, w[i], w[i + 1:]


def branch_bad(args):
    for a in args:
        if a in BRANCH_BAD_LONG:
            return True
        if a.startswith("-") and not a.startswith("--") and BRANCH_BAD_SHORT & set(a[1:]):
            return True
    return False


def tag_bad(args):
    """Creating or deleting a tag; listing is read-only."""
    if not args:
        return False
    return not any(a in TAG_READ or a.startswith("-n") or a.startswith("--sort=") or a.startswith("--format=")
                   or a.startswith("--contains=") or a.startswith("--points-at=") for a in args)


def push_targets_protected(args, d):
    pos, i = [], 0
    while i < len(args):
        a = args[i]
        if a in ("--all", "--mirror"):
            return True
        if a.startswith("-"):
            i += 2 if a in PUSH_VALUE_OPTS else 1
            continue
        pos.append(a)
        i += 1
    refspecs = pos[1:]
    if not refspecs:
        return current_branch(d) in PROTECTED
    for rs in refspecs:
        rs = rs.lstrip("+")
        dst = rs.rsplit(":", 1)[-1] if ":" in rs else rs
        if dst == "HEAD":
            dst = current_branch(d)
        if dst.startswith("refs/heads/"):
            dst = dst[len("refs/heads/"):]
        if dst in PROTECTED:
            return True
    return False


SHELLS = {"bash", "sh", "zsh", "dash"}
KEYWORDS = {"do", "then", "else", "if", "while", "until", "!"}


def check_words(w, cwd, project):
    """w: argv of one command; returns (deny reason or None, new cwd; None = unknown)."""
    import re
    while w and w[0] in KEYWORDS:
        w = w[1:]
    w, env = strip_wrappers(w)
    if not w:
        return None, cwd
    if w[0] in ("cd", "pushd"):
        t = next((a for a in w[1:] if not a.startswith("-")), None)
        return None, join(cwd, t) if t else os.path.expanduser("~")
    prog = os.path.basename(w[0])
    if prog in SHELLS:
        for k in range(1, min(len(w) - 1, 4)):
            if re.match(r"^-[a-z]*c[a-z]*$", w[k]):
                return check_command(w[k + 1], cwd, project), cwd
        return None, cwd
    if prog != "git":
        return None, cwd
    d, sub, args = parse_git(w[1:], cwd, env)
    if not sub:
        return None, cwd
    if sub == "push" and push_targets_protected(args, d):
        return MSG_PUSH, cwd
    if is_main(d, project):
        bad = (sub in DENY_MAIN and not (sub == "stash" and args[:1] in (["list"], ["show"]))
               or (sub == "tag" and tag_bad(args))
               or (sub == "branch" and branch_bad(args)))
        if bad:
            return MSG_MAIN % sub, cwd
    return None, cwd


def check_command(cmd, cwd, project):
    import shlex
    parts = split_commands(cmd)
    if not parts:
        return None
    for p in parts:
        p = p.lstrip("({ ").rstrip(")} ")
        try:
            w = shlex.split(p)
        except ValueError:
            continue
        reason, cwd = check_words(w, cwd, project)
        if reason:
            return reason
    return None


def decide(d, project):
    if not isinstance(d, dict) or not d.get("agent_id") or d.get("tool_name") not in (None, "Bash"):
        return None
    inp = d.get("tool_input") or {}
    cmd = str(inp.get("command") or "")
    if "git" not in cmd:
        return None
    project = real(project)
    cwd = d.get("cwd") or project
    if not os.path.isdir(os.path.join(project, ".git")):
        project = None  # a linked worktree as project dir: only the push rule applies
    return check_command(cmd, cwd, project)


def main():
    try:
        d = json.load(sys.stdin)
        project = os.environ.get("CLAUDE_PROJECT_DIR")
        if not project:
            return
        reason = decide(d, project)
        if reason:
            json.dump({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                              "permissionDecision": "deny",
                                              "permissionDecisionReason": reason}}, sys.stdout)
    except Exception:
        return


if __name__ == "__main__":
    main()
    sys.exit(0)
