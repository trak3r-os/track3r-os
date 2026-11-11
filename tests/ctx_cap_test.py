#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-OPS-TOKCAP: .claude/hooks/ctx_cap.py denies a subagent's tools past its context cap."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".claude", "hooks", "ctx_cap.py")


def usage_line(inp, cc=0, cr=0):
    return json.dumps({"type": "assistant", "message": {"usage": {
        "input_tokens": inp, "cache_creation_input_tokens": cc,
        "cache_read_input_tokens": cr, "output_tokens": 5}}})


class CtxCap(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.proj = os.path.join(self.tmp.name, "proj")
        os.makedirs(os.path.join(self.proj, ".claude"))
        self.write_ini("[agents]\nhard_ctx = 250000\nstop_ctx_no_commit = 150000\n")
        self.sub = os.path.join(self.tmp.name, "sess", "subagents")
        os.makedirs(self.sub)
        self.parent = os.path.join(self.tmp.name, "sess.jsonl")

    def write_ini(self, text):
        with open(os.path.join(self.proj, ".claude", "ops-params.ini"), "w") as f:
            f.write(text)

    def transcript(self, ctx, aid="a1", pad=0):
        with open(os.path.join(self.sub, "agent-%s.jsonl" % aid), "w") as f:
            f.write(usage_line(1000) + "\n")
            f.write("x" * pad + "\n")  # garbage line, cut-off tail start
            f.write(usage_line(5, 10, ctx - 15) + "\n")
            f.write('{"type":"user"}\n')

    def run_hook(self, tool="Read", tool_input=None, agent="firmware-reviewer", aid="a1",
                 cwd=None, raw=None, env_proj=True):
        d = {"agent_id": aid, "agent_type": agent, "session_id": "sess",
             "transcript_path": self.parent, "cwd": cwd or self.tmp.name,
             "tool_name": tool, "tool_input": tool_input or {}}
        if aid is None:
            del d["agent_id"]
        env = dict(os.environ)
        if env_proj:
            env["CLAUDE_PROJECT_DIR"] = self.proj
        else:
            env["CLAUDE_PROJECT_DIR"] = os.path.join(self.tmp.name, "none")
        p = subprocess.run([sys.executable, "-I", HOOK], input=raw if raw is not None else json.dumps(d),
                           capture_output=True, text=True, env=env, timeout=20)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout

    def assertDenied(self, out, n=None):
        j = json.loads(out)["hookSpecificOutput"]
        self.assertEqual(j["permissionDecision"], "deny")
        self.assertEqual(j["hookEventName"], "PreToolUse")
        if n:
            self.assertIn("Context %d tokens" % n, j["permissionDecisionReason"])
        return j["permissionDecisionReason"]

    def test_no_agent_id(self):
        self.transcript(300000)
        self.assertEqual(self.run_hook(aid=None), "")

    def test_under_cap(self):
        self.transcript(100000)
        self.assertEqual(self.run_hook(), "")

    def test_over_hard_cap(self):
        self.transcript(260000, pad=400 * 1024)
        r = self.assertDenied(self.run_hook(tool="Read"), 260000)
        self.assertIn("250000", r)
        self.assertIn("STATE.md", r)
        self.assertDenied(self.run_hook(tool="Bash", tool_input={"command": "ls"}))
        self.assertDenied(self.run_hook(tool="Bash", tool_input={"command": "git status && rm -rf x"}))
        self.assertDenied(self.run_hook(tool="Bash", tool_input={"command": "git commit -m $(rm x)"}))
        self.assertDenied(self.run_hook(tool="Write", tool_input={"file_path": "/a/b.cpp"}))
        self.assertEqual(self.run_hook(tool="Bash", tool_input={
            "command": "git add a b && git commit -m x; git push -u origin chore/x | git log"}), "")
        self.assertEqual(self.run_hook(tool="Bash", tool_input={"command": "git status"}), "")

    def test_commit_title_with_parentheses(self):
        self.transcript(260000)
        c = lambda x: self.run_hook(tool="Bash", tool_input={"command": x})
        self.assertEqual(c('git commit -m "feat(hooks): cap [OS-X]" -m "body text"'), "")
        self.assertEqual(c("git add a && git commit -m 'fix(ops): x [OS-Y]' && git push"), "")
        # ; and | inside quotes are text, not separators
        self.assertEqual(c('git commit -m "a; b | c && d"'), "")
        # the same outside quotes separates commands, and rm is not git
        self.assertDenied(c('git commit -m x; rm -rf y'))
        self.assertDenied(c('git commit -m "a" | rm y'))
        # a separator hidden after a quoted one is still seen
        self.assertDenied(c('git commit -m "a;b"; rm y'))
        # open quote, escape, expansion, subshell-looking text in the message
        self.assertDenied(c('git commit -m "abc'))
        self.assertDenied(c('git commit -m "a\\" ; rm y ; \\""'))
        self.assertDenied(c('git commit -m "a $(rm y)"'))
        self.assertDenied(c('git commit -m "a `rm y`"'))
        r = self.assertDenied(self.run_hook(tool="Read"))
        self.assertIn("git commit -m", r)
        self.assertIn("no heredoc", r)

    def test_state_md_allowed(self):
        self.transcript(260000)
        self.assertEqual(self.run_hook(tool="Write", tool_input={
            "file_path": "/w/wt/build-remote/STATE.md"}), "")
        self.assertEqual(self.run_hook(tool="Edit", tool_input={
            "file_path": "/w/wt/build-remote/STATE.md"}), "")

    def git(self, repo, *a):
        subprocess.run(["git", "-C", repo] + list(a), check=True, capture_output=True,
                       env=dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                                GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t"))

    def test_engineer_no_commit(self):
        repo = os.path.join(self.tmp.name, "repo")
        os.makedirs(repo)
        self.git(repo, "init", "-q", "-b", "dev")
        self.git(repo, "commit", "-q", "--allow-empty", "-m", "base")
        self.git(repo, "checkout", "-q", "-b", "chore/x")
        self.transcript(160000)
        # engineer, branch without commits: denied under the hard cap
        r = self.assertDenied(self.run_hook(agent="firmware-engineer", cwd=repo), 160000)
        self.assertIn("150000", r)
        self.assertIn("no commit yet", r)
        # a reviewer is not subject to the engineer rule
        self.assertEqual(self.run_hook(agent="firmware-reviewer", cwd=repo), "")
        # after a commit the engineer may go on
        self.git(repo, "commit", "-q", "--allow-empty", "-m", "work")
        self.assertEqual(self.run_hook(agent="firmware-engineer", cwd=repo), "")
        # on dev the rule is skipped; outside a repo too
        self.git(repo, "checkout", "-q", "dev")
        self.assertEqual(self.run_hook(agent="firmware-engineer", cwd=repo), "")
        self.assertEqual(self.run_hook(agent="firmware-engineer", cwd=self.tmp.name), "")

    def test_missing_transcript(self):
        self.assertEqual(self.run_hook(aid="nope"), "")

    def test_garbage_stdin(self):
        self.assertEqual(self.run_hook(raw="not json{"), "")
        self.assertEqual(self.run_hook(raw=""), "")
        self.assertEqual(self.run_hook(raw="[1,2]"), "")

    def test_missing_ini_defaults(self):
        self.transcript(260000)
        self.assertDenied(self.run_hook(env_proj=False), 260000)
        self.transcript(200000)
        self.assertEqual(self.run_hook(env_proj=False), "")

    def test_subagent_transcript_path(self):
        self.transcript(260000)
        path = os.path.join(self.sub, "agent-a1.jsonl")
        d = {"agent_id": "a1", "agent_type": "x", "session_id": "s", "transcript_path": path,
             "cwd": self.tmp.name, "tool_name": "Read", "tool_input": {}}
        env = dict(os.environ, CLAUDE_PROJECT_DIR=self.proj)
        p = subprocess.run([sys.executable, "-I", HOOK], input=json.dumps(d),
                           capture_output=True, text=True, env=env, timeout=20)
        self.assertDenied(p.stdout, 260000)


if __name__ == "__main__":
    unittest.main()
