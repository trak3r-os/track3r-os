#!/usr/bin/env python3
"""SessionStart hook: inject a short state brief so no session starts cold.

Prints (stdout becomes session context) the branch, uncommitted files, live
worktrees and the "## Next" section of docs/STATUS.md. Kept under ~40 lines
on purpose: token spend is kept low. Fails open (prints what it can).
"""
import configparser
import json
import os
import subprocess
import sys

MAX_NEXT_LINES = 12
MAX_LINE_CHARS = 200
STATUS = os.path.join("docs", "STATUS.md")
OPS = os.path.join(".claude", "ops-params.ini")
MAX_VALUE_CHARS = 110


def git(root, *args):
    try:
        return subprocess.run(["git", "-C", root, *args], capture_output=True,
                              text=True, timeout=10).stdout.rstrip("\n")
    except (OSError, subprocess.SubprocessError):
        return ""


def status_next(root):
    path = os.path.join(root, STATUS)
    try:
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return []
    out, inside = [], False
    for line in lines:
        if line.startswith("## "):
            if inside:
                break
            inside = line.strip() == "## Next"
            continue
        if inside and line.strip():
            if len(line) > MAX_LINE_CHARS:
                line = line[:MAX_LINE_CHARS - 1] + "..."
            out.append(line)
    if len(out) > MAX_NEXT_LINES:
        out = out[:MAX_NEXT_LINES] + ["  ... (rest in docs/STATUS.md)"]
    return out


def ops_params(root):
    """Compact lines of .claude/ops-params.ini; one line on a missing or broken file."""
    head = "- Ops params (.claude/ops-params.ini): "
    try:
        cp = configparser.ConfigParser(interpolation=None)
        with open(os.path.join(root, OPS), encoding="utf-8") as fh:
            cp.read_file(fh)
        lines = []
        for sec in cp.sections():
            pairs = []
            for key, val in cp.items(sec):
                if len(val) > MAX_VALUE_CHARS:
                    val = val[:MAX_VALUE_CHARS - 3] + "..."
                pairs.append(f"{key}={val}")
            lines.append(f"  [{sec}] " + "; ".join(pairs))
        if not lines:
            return [head + "file empty"]
        return [head] + lines
    except (OSError, configparser.Error, UnicodeDecodeError):
        return [head + "missing or unreadable, see docs/maintainer-workflow.md"]


def _proc_ppid(proc_root, pid):
    with open(os.path.join(proc_root, str(pid), "stat"), "rb") as fh:
        data = fh.read().decode("utf-8", "replace")
    return int(data[data.rindex(")") + 2:].split()[1])


def orphans(session_id, proc_root="/proc", ppid=None):
    """Pids of other claude processes started with --resume for this session.

    The hook's own ancestor chain (its claude, the shell between) is excluded.
    Linux only; any problem yields an empty list.
    """
    found = []
    try:
        if not session_id or not os.path.isdir(proc_root):
            return found
        skip, pid = set(), os.getppid() if ppid is None else ppid
        while pid > 1 and pid not in skip:
            skip.add(pid)
            try:
                pid = _proc_ppid(proc_root, pid)
            except (OSError, ValueError):
                break
        skip.add(os.getpid())
        for name in os.listdir(proc_root):
            if not name.isdigit() or int(name) in skip:
                continue
            try:
                with open(os.path.join(proc_root, name, "cmdline"), "rb") as fh:
                    args = fh.read().decode("utf-8", "replace").split("\0")
            except OSError:
                continue
            if not any("claude" in a for a in args[:3]):
                continue
            if ("--resume=" + session_id) in args or any(
                    a == "--resume" and b == session_id for a, b in zip(args, args[1:])):
                found.append(int(name))
    except Exception:
        return []
    return sorted(found)


def main():
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        payload = {}
    if not isinstance(payload, dict):
        payload = {}
    root = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or "."
    branch = git(root, "branch", "--show-current") or "(detached)"
    head = git(root, "log", "-1", "--format=%h %s")[:90]
    dirty = [l for l in git(root, "status", "--porcelain").splitlines() if l]
    here = os.path.realpath(root)
    trees = [l.split()[0] for l in git(root, "worktree", "list").splitlines()
             if l and os.path.realpath(l.split()[0]) != here]

    print("## Session brief (auto, SessionStart hook)")
    print(f"- Branch `{branch}` at {head}; {len(dirty)} uncommitted path(s)"
          + (": " + ", ".join(l[3:] for l in dirty[:5]) if dirty else "."))
    if trees:
        print(f"- {len(trees)} extra worktree(s): "
              + ", ".join(os.path.basename(t) for t in trees[:6])
              + ". Another session or agent may be using them; run ListAgents "
              "before touching shared files.")
    for pid in orphans(str(payload.get("session_id") or "")):
        print(f"- WARNING: another claude process (pid {pid}) runs this same session "
              "(orphan after a reload?); its background agents may still be editing. "
              "Stop them from that session before relaunching agents.")
    nxt = status_next(root)
    if nxt:
        print("- docs/STATUS.md -> Next:")
        print("\n".join(nxt))
    else:
        print("- No \"## Next\" section found in docs/STATUS.md; read it and "
              "docs/roadmap/BACKLOG.md.")
    print("\n".join(ops_params(root)))
    print("- Rules: CLAUDE.md. Never flash; any SD card may be written, "
          "never a fixed or system disk; verify before claiming done; builds under nice -n 10, "
          "at most -j4; keep token spend low.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
