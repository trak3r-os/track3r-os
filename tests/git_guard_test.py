#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-OPS-GITGUARD: .claude/hooks/git_guard.py keeps subagents off the main checkout's git state
and off dev/main pushes; session_brief.py warns about an orphaned copy of the same session."""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HOOKS = os.path.join(HERE, "..", ".claude", "hooks")
HOOK = os.path.join(HOOKS, "git_guard.py")


def git(cwd, *a):
    subprocess.run(["git", "-C", cwd, "-c", "user.name=t", "-c", "user.email=t@example.invalid", *a],
                   check=True, capture_output=True)


class GitGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.main = os.path.realpath(os.path.join(cls.tmp.name, "main"))
        os.makedirs(cls.main)
        subprocess.run(["git", "init", "-q", "-b", "dev", cls.main], check=True)
        git(cls.main, "commit", "-q", "--allow-empty", "-m", "init")
        cls.wt = os.path.join(cls.main, ".claude", "worktrees", "wt")
        git(cls.main, "worktree", "add", "-q", "-b", "feat/x", cls.wt)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_hook(self, cmd, cwd, agent=True, raw=None):
        d = {"tool_name": "Bash", "cwd": cwd, "tool_input": {"command": cmd}}
        if agent:
            d.update(agent_id="a1", agent_type="firmware-reviewer")
        env = dict(os.environ, CLAUDE_PROJECT_DIR=self.main)
        r = subprocess.run([sys.executable, "-I", HOOK], input=raw if raw is not None else json.dumps(d),
                           capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        if not r.stdout:
            return None
        o = json.loads(r.stdout)["hookSpecificOutput"]
        self.assertEqual(o["permissionDecision"], "deny")
        return o["permissionDecisionReason"]

    def test_main_session_allowed(self):
        self.assertIsNone(self.run_hook("git checkout --detach", self.main, agent=False))
        self.assertIsNone(self.run_hook("git push origin dev", self.wt, agent=False))

    def test_checkout_in_main_denied(self):
        why = self.run_hook("git checkout --detach", self.main)
        self.assertIn("git checkout", why)
        self.assertIn("git show origin/<branch>:<path>", why)

    def test_cd_then_switch_denied(self):
        self.assertIn("git switch", self.run_hook("cd %s && git switch dev" % self.main, self.wt))

    def test_dash_c_merge_denied(self):
        self.assertIn("git merge", self.run_hook("git -C %s merge x" % self.main, self.wt))

    def test_wrappers_and_shell_c_denied(self):
        self.assertIsNotNone(self.run_hook("nice -n 10 git reset --hard", self.main))
        self.assertIsNotNone(self.run_hook("GIT_X=1 timeout 5 git commit -m x", self.main))
        self.assertIsNotNone(self.run_hook("bash -c 'git stash'", self.main))
        self.assertIsNotNone(self.run_hook("git branch -D feat/x", self.main))
        self.assertIsNotNone(self.run_hook("git tag v1", self.main))

    def test_worktree_work_allowed(self):
        self.assertIsNone(self.run_hook("git commit -q -m 'git checkout x'", self.wt))
        self.assertIsNone(self.run_hook("git push -u origin feat/x", self.wt))
        self.assertIsNone(self.run_hook("git -C %s checkout -b feat/y" % self.wt, self.main))

    def test_push_dev_main_denied(self):
        for c in ("git push origin dev", "git push origin HEAD:main", "git push origin dev:main",
                  "git push origin +feat/x:refs/heads/dev", "git push --force origin main"):
            self.assertIn("orchestrator", self.run_hook(c, self.wt) or "", c)
        # HEAD is feat/x in the worktree (allowed) and dev in main (denied)
        self.assertIsNone(self.run_hook("git push origin HEAD", self.wt))
        self.assertIsNotNone(self.run_hook("git push origin HEAD", self.main))

    def test_read_only_in_main_allowed(self):
        for c in ("git status", "git diff dev...origin/x", "git show origin/x:f", "git log -3",
                  "git branch -a", "git tag -l", "git fetch origin", "git stash list",
                  "git worktree add ../w2 -b feat/z", "git branch --show-current"):
            self.assertIsNone(self.run_hook(c, self.main), c)

    def test_effective_dir_forms(self):
        main, wt = self.main, self.wt
        self.assertIsNotNone(self.run_hook("git --git-dir=%s/.git checkout x" % main, wt))
        self.assertIsNotNone(self.run_hook("git --work-tree %s switch dev" % main, wt))
        self.assertIsNotNone(self.run_hook("GIT_DIR=%s/.git git reset --hard" % main, wt))
        self.assertIsNotNone(self.run_hook("GIT_WORK_TREE=%s git merge x" % main, wt))
        self.assertIsNotNone(self.run_hook("/usr/bin/git checkout x", main))
        self.assertIsNotNone(self.run_hook("bash -lc 'git checkout x'", main))
        self.assertIsNotNone(self.run_hook("for i in 1; do git commit -m x; done", main))
        self.assertIsNotNone(self.run_hook("if true; then git merge x; fi", main))
        # the git dir of a linked worktree is not the main checkout
        gd = subprocess.run(["git", "-C", wt, "rev-parse", "--absolute-git-dir"],
                            capture_output=True, text=True).stdout.strip()
        self.assertIsNone(self.run_hook("git --git-dir=%s commit -q --allow-empty -m x" % gd, main))

    def test_unknown_dir_fails_open(self):
        self.assertIsNone(self.run_hook('cd "$WT" && git commit -m x', self.main))
        self.assertIsNone(self.run_hook("cd `pwd`/x && git commit -m x", self.main))
        self.assertIsNone(self.run_hook("git -C ../w* checkout x", self.main))
        self.assertIsNone(self.run_hook("git --work-tree=$W checkout x", self.main))
        # pushes are still checked with an unknown dir
        self.assertIsNotNone(self.run_hook('cd "$WT" && git push origin dev', self.main))

    def test_project_dir_is_worktree(self):
        d = {"tool_name": "Bash", "cwd": self.wt, "agent_id": "a1"}
        env = dict(os.environ, CLAUDE_PROJECT_DIR=self.wt)  # .git is a file there

        def run(cmd):
            d["tool_input"] = {"command": cmd}
            r = subprocess.run([sys.executable, "-I", HOOK], input=json.dumps(d),
                               capture_output=True, text=True, env=env)
            self.assertEqual(r.returncode, 0)
            return r.stdout
        self.assertEqual(run("git commit -q --allow-empty -m x"), "")
        self.assertIn("deny", run("git push origin dev"))

    def test_tag_n_allowed(self):
        self.assertIsNone(self.run_hook("git tag -n5", self.main))
        self.assertIsNone(self.run_hook("git tag -n", self.main))
        self.assertIsNotNone(self.run_hook("git tag -d v1", self.main))

    def test_quoted_text_allowed(self):
        self.assertIsNone(self.run_hook('echo "run git checkout dev; then git merge"', self.main))
        self.assertIsNone(self.run_hook("grep 'git switch' notes.md", self.main))

    def test_garbage_input(self):
        for raw in ("", "not json", "[]", "null", "{}"):
            self.assertIsNone(self.run_hook("", self.main, raw=raw))


class Orphans(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("session_brief", os.path.join(HOOKS, "session_brief.py"))
        cls.sb = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.sb)

    def proc(self, root, pid, ppid, args):
        d = os.path.join(root, str(pid))
        os.makedirs(d)
        with open(os.path.join(d, "stat"), "w") as f:
            f.write("%d (claude code) S %d 1 1 0 -1\n" % (pid, ppid))
        with open(os.path.join(d, "cmdline"), "wb") as f:
            f.write(b"\0".join(a.encode() for a in args) + b"\0")

    def test_scan(self):
        with tempfile.TemporaryDirectory() as root:
            sid = "11111111-2222"
            self.proc(root, 100, 1, ["claude", "--resume=" + sid])        # this hook's own claude
            self.proc(root, 101, 100, ["sh", "-c", "hook"])               # shell between
            self.proc(root, 200, 1, ["/usr/bin/claude", "--resume=" + sid])      # orphan, = form
            self.proc(root, 201, 1, ["node", "/x/claude/cli.js", "--resume", sid])  # orphan, split form
            self.proc(root, 300, 1, ["claude", "--resume=other"])         # another session
            self.proc(root, 301, 1, ["grep", "--resume=" + sid])          # not claude
            os.makedirs(os.path.join(root, "self"))                        # non-pid entry
            self.assertEqual(self.sb.orphans(sid, root, ppid=101), [200, 201])
            self.assertEqual(self.sb.orphans("", root, ppid=101), [])
            self.assertEqual(self.sb.orphans(sid, os.path.join(root, "nope")), [])

    def test_brief_exits_zero(self):
        for raw in ("", "{}", "[]", '{"session_id": "x"}'):
            r = subprocess.run([sys.executable, "-I", os.path.join(HOOKS, "session_brief.py")],
                               input=raw, capture_output=True, text=True,
                               env=dict(os.environ, CLAUDE_PROJECT_DIR=HERE))
            self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
