#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PreToolUse hook: enforce a subagent's context budget (OS-OPS-TOKCAP).

Only calls that come from a subagent (the hook input has agent_id) are checked.
The subagent's context is the sum of the three input-token counts of the last
usage entry in its transcript. Caps come from .claude/ops-params.ini [agents]:
hard_ctx for every subagent, stop_ctx_no_commit for a firmware-engineer whose
branch has no commit yet. Over a cap every tool call is denied except the ones
that wrap up (git add/commit/push/status/log/diff/rev-parse/show, and a write of
build-remote/STATE.md). Fails open: any error means exit 0 with no output.
"""
import configparser
import json
import os
import re
import shlex
import subprocess
import sys

TAIL_BYTES = 256 * 1024
DEFAULT_HARD = 250000
DEFAULT_NO_COMMIT = 150000
GIT_OK = {"add", "commit", "push", "status", "log", "diff", "rev-parse", "show"}


def transcript_path(d):
    tp = d.get("transcript_path") or ""
    if "/subagents/" in tp:
        return tp
    sid = d.get("session_id") or ""
    aid = d.get("agent_id") or ""
    if not (tp and sid and aid):
        return ""
    return os.path.join(os.path.dirname(tp), sid, "subagents", "agent-%s.jsonl" % aid)


def last_ctx(path):
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - TAIL_BYTES))
        data = f.read()
    for line in reversed(data.split(b"\n")):
        if b'"usage"' not in line:
            continue
        try:
            e = json.loads(line)
        except ValueError:
            continue  # first line of the tail may be cut
        u = None
        if isinstance(e, dict):
            m = e.get("message")
            u = m.get("usage") if isinstance(m, dict) else e.get("usage")
        if isinstance(u, dict):
            return sum(int(u.get(k) or 0) for k in (
                "input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"))
    return None


def caps(project):
    hard, nocommit = DEFAULT_HARD, DEFAULT_NO_COMMIT
    try:
        cp = configparser.ConfigParser(interpolation=None)
        cp.read(os.path.join(project, ".claude", "ops-params.ini"))
        hard = cp.getint("agents", "hard_ctx", fallback=hard)
        nocommit = cp.getint("agents", "stop_ctx_no_commit", fallback=nocommit)
    except Exception:
        pass
    return hard, nocommit


def no_commit_yet(cwd):
    """True only when git says the branch has 0 commits beyond dev."""
    try:
        br = subprocess.run(["git", "-C", cwd, "rev-parse", "--abbrev-ref", "HEAD"],
                            capture_output=True, text=True, timeout=2)
        if br.returncode != 0 or br.stdout.strip() in ("dev", "main", "HEAD", ""):
            return False
        r = subprocess.run(["git", "-C", cwd, "rev-list", "--count", "dev..HEAD"],
                           capture_output=True, text=True, timeout=2)
        return r.returncode == 0 and r.stdout.strip() == "0"
    except Exception:
        return False


def split_commands(cmd):
    """Split on && ; | outside quotes; None when a quote is left open."""
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
        elif c in ";|" or cmd.startswith("&&", i):
            parts.append("".join(cur))
            cur = []
            i += 1 if c in ";|" else 2
            continue
        else:
            cur.append(c)
        i += 1
    if q:
        return None
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def allowed(d):
    tool = d.get("tool_name")
    inp = d.get("tool_input") or {}
    if tool in ("Write", "Edit"):
        return str(inp.get("file_path") or "").endswith("build-remote/STATE.md")
    if tool == "Bash":
        cmd = str(inp.get("command") or "")
        # No expansion, redirect, brace or escape anywhere, quoted or not.
        if not cmd.strip() or re.search(r"[`$<>{}\\\n]", cmd):
            return False
        parts = split_commands(cmd)
        if not parts:
            return False
        for p in parts:
            try:
                w = shlex.split(p)
            except ValueError:
                return False
            if len(w) < 2 or w[0] != "git":
                return False
            i = 1
            if w[i] == "-C" and len(w) > 3:
                i = 3
            if w[i] not in GIT_OK:
                return False
        return True
    return False


def main():
    try:
        d = json.load(sys.stdin)
        if not isinstance(d, dict) or not d.get("agent_id"):
            return
        path = transcript_path(d)
        if not path:
            return
        ctx = last_ctx(path)
        if ctx is None:
            return
        hard, nocommit = caps(os.environ.get("CLAUDE_PROJECT_DIR") or ".")
        if os.environ.get("CTX_CAP_DEBUG"):
            sys.stderr.write("ctx=%d\n" % ctx)
        cap, why = None, ""
        if ctx >= hard:
            cap = hard
        elif ctx >= nocommit and d.get("agent_type") == "firmware-engineer" \
                and no_commit_yet(d.get("cwd") or "."):
            cap, why = nocommit, ", no commit yet"
        if cap is None or allowed(d):
            return
        reason = ("Context %d tokens >= %d (.claude/ops-params.ini [agents]%s). "
                  "Commit what is green (git add <paths>, git commit, git push; commit with "
                  "git commit -m \"<title>\" -m \"<body>\", no heredoc, no $, no backticks, "
                  "no ; or | outside quotes), write build-remote/STATE.md (10 lines: done, left, next step), "
                  "then stop and report." % (ctx, cap, why))
        json.dump({"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                          "permissionDecision": "deny",
                                          "permissionDecisionReason": reason}}, sys.stdout)
    except Exception:
        return


if __name__ == "__main__":
    main()
    sys.exit(0)
