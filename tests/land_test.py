#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-OPS-LAND: tools/land.py lands reviewed branches on dev through the integration worktree.
Temp bare origin, a clone on dev (the main checkout), feature branches pushed to origin; the merge is plain git
(O3OS_LAND_MERGE=git) and CI is a stub (O3OS_LAND_CI_CMD=true|false)."""
import fcntl
import importlib.util
import os
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LAND = os.path.join(HERE, "..", "tools", "land.py")

ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
           GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid", O3OS_LAND_MERGE="git",
           GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")


def git(cwd, *a):
    p = subprocess.run(["git", "-C", cwd, *a], check=True, capture_output=True, text=True, env=ENV)
    return p.stdout.strip()


def load_land():
    spec = importlib.util.spec_from_file_location("land", LAND)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class LandCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = os.path.realpath(self.tmp.name)
        self.origin = os.path.join(t, "origin.git")
        self.main = os.path.join(t, "main")
        subprocess.run(["git", "init", "-q", "--bare", "-b", "dev", self.origin], check=True, env=ENV)
        subprocess.run(["git", "clone", "-q", self.origin, self.main], check=True, capture_output=True, env=ENV)
        git(self.main, "switch", "-q", "-C", "dev")
        self.write("base.txt", "base\n")
        self.commit("chore: base")
        git(self.main, "push", "-q", "origin", "dev")
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, text, cwd=None):
        with open(os.path.join(cwd or self.main, name), "w") as f:
            f.write(text)

    def commit(self, msg, cwd=None):
        git(cwd or self.main, "add", "-A")
        git(cwd or self.main, "commit", "-q", "-m", msg)

    def branch(self, name, files, subject):
        """A branch off dev with one commit, pushed, the main checkout back on dev."""
        git(self.main, "switch", "-q", "-c", name, "dev")
        for fn, text in files.items():
            self.write(fn, text)
        self.commit(subject)
        git(self.main, "push", "-q", "origin", name)
        git(self.main, "switch", "-q", "dev")
        git(self.main, "branch", "-q", "-D", name)

    def land(self, *args, ci="true", extra_env=None):
        env = dict(ENV, O3OS_LAND_CI_CMD=ci)
        env.update(extra_env or {})
        return subprocess.run([sys.executable, "-I", LAND, *args], cwd=self.main, capture_output=True, text=True, env=env)

    def heads(self):
        return git(self.main, "rev-parse", "dev"), git(self.origin, "rev-parse", "dev")


class Land(LandCase):
    def test_two_branches_land_with_titles_trailer_and_push(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): the first thing [OS-AAA]")
        self.branch("fix/OS-REL130-spdx", {"b.txt": "b\n"}, "fix(tools): second thing")
        r = self.land("feat/OS-AAA-one", "fix/OS-REL130-spdx")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        dev, origin = self.heads()
        self.assertEqual(dev, origin)
        self.assertEqual(git(self.main, "log", "-1", "--format=%s"), "merge: second thing [OS-REL130]")
        self.assertEqual(git(self.main, "log", "-1", "--format=%b"), "Landed-by: tools/land.py")
        self.assertEqual(git(self.main, "log", "-1", "--format=%s", "HEAD^"), "merge: the first thing [OS-AAA]")
        self.assertTrue(os.path.exists(os.path.join(self.main, "a.txt")))
        self.assertTrue(os.path.exists(os.path.join(self.main, "b.txt")))
        self.assertEqual(git(self.main, "symbolic-ref", "--short", "HEAD"), "dev")
        self.assertIn("pushed origin/dev", r.stdout)

    def test_title_option_and_no_push(self):
        self.branch("chore/OS-OPS-X", {"a.txt": "a\n"}, "chore(x): ignored subject [OS-OPS-X]")
        r = self.land("chore/OS-OPS-X", "--title", "my summary", "--no-push", "--no-ci", "local ci.sh rc=0")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(git(self.main, "log", "-1", "--format=%s"), "merge: my summary [OS-OPS-X]")
        self.assertIn("no CI: local ci.sh rc=0", r.stdout)
        dev, origin = self.heads()
        self.assertNotEqual(dev, origin)

    def test_ci_fail_leaves_dev_and_origin(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        before = self.heads()
        r = self.land("feat/OS-AAA-one", ci="false")
        self.assertEqual(r.returncode, 1)
        self.assertIn("CI failed", r.stderr)
        self.assertEqual(self.heads(), before)
        self.assertFalse(os.path.exists(os.path.join(self.main, "a.txt")))

    def test_no_remote_ci_hint(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        before = self.heads()
        r = self.land("feat/OS-AAA-one", ci="exit 3")
        self.assertEqual(r.returncode, 3)
        self.assertIn("--no-ci 'local ci.sh rc=0'", r.stderr)
        self.assertEqual(self.heads(), before)

    def test_conflict_leaves_dev_untouched(self):
        self.branch("feat/OS-AAA-one", {"base.txt": "one\n"}, "feat(ui): one [OS-AAA]")
        self.branch("feat/OS-BBB-two", {"base.txt": "two\n"}, "feat(ui): two [OS-BBB]")
        before = self.heads()
        r = self.land("feat/OS-AAA-one", "feat/OS-BBB-two")
        self.assertEqual(r.returncode, 1)
        self.assertIn("merging origin/feat/OS-BBB-two failed", r.stderr)
        self.assertEqual(self.heads(), before)
        self.assertEqual(git(self.main, "status", "--porcelain", "--untracked-files=no"), "")

    def test_unpushed_local_dev_commits_abort(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        self.write("local.txt", "x\n")
        self.commit("chore: local only")
        before = self.heads()
        r = self.land("feat/OS-AAA-one")
        self.assertEqual(r.returncode, 1)
        self.assertIn("local commits", r.stderr)
        self.assertIn("chore: local only", r.stderr)
        self.assertEqual(self.heads(), before)
        r = self.land("feat/OS-AAA-one", "--allow-local")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        dev, origin = self.heads()
        self.assertEqual(dev, origin)
        self.assertIn("chore: local only", git(self.origin, "log", "--format=%s", "dev").splitlines())

    def test_dev_behind_origin_is_fast_forwarded(self):
        other = os.path.join(os.path.dirname(self.main), "other")
        subprocess.run(["git", "clone", "-q", self.origin, other], check=True, capture_output=True, env=ENV)
        self.write("o.txt", "o\n", cwd=other)
        self.commit("chore: elsewhere", cwd=other)
        git(other, "push", "-q", "origin", "dev")
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        r = self.land("feat/OS-AAA-one")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(os.path.exists(os.path.join(self.main, "o.txt")))

    def test_local_branch_ahead_of_origin_aborts(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        git(self.main, "switch", "-q", "-c", "feat/OS-AAA-one", "origin/feat/OS-AAA-one")
        self.write("more.txt", "m\n")
        self.commit("feat(ui): unpushed")
        git(self.main, "switch", "-q", "dev")
        before = self.heads()
        r = self.land("feat/OS-AAA-one")
        self.assertEqual(r.returncode, 1)
        self.assertIn("push it first", r.stderr)
        self.assertEqual(self.heads(), before)

    def test_unpushed_branch_aborts(self):
        r = self.land("feat/OS-NOPE-x")
        self.assertEqual(r.returncode, 1)
        self.assertIn("push the branch first", r.stderr)

    def test_main_checkout_off_dev_or_dirty_aborts(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        self.write("base.txt", "dirty\n")
        r = self.land("feat/OS-AAA-one")
        self.assertEqual(r.returncode, 1)
        self.assertIn("uncommitted", r.stderr)
        git(self.main, "checkout", "-q", "--", "base.txt")
        git(self.main, "switch", "-q", "--detach")
        r = self.land("feat/OS-AAA-one")
        self.assertEqual(r.returncode, 1)
        self.assertIn("not on branch dev", r.stderr)

    def test_plain_directory_as_land_worktree_aborts_and_main_is_intact(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        plain = os.path.join(self.main, ".claude", "worktrees", "land")
        os.makedirs(plain)
        self.write("keep.txt", "in the plain dir\n", cwd=plain)
        self.write("mine.txt", "untracked edit in main\n")
        before = self.heads()
        for ci in ("true", "false"):
            r = self.land("feat/OS-AAA-one", ci=ci)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("not a worktree of its own", r.stderr)
        self.assertEqual(self.heads(), before)
        self.assertEqual(git(self.main, "symbolic-ref", "--short", "HEAD"), "dev")
        self.assertEqual(git(self.main, "branch", "--list", "land/work"), "")
        for fn in (os.path.join(plain, "keep.txt"), os.path.join(self.main, "mine.txt")):
            self.assertTrue(os.path.exists(fn), fn)

    def test_failure_after_ff_says_dev_advanced(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        hook = os.path.join(self.origin, "hooks", "pre-receive")
        with open(hook, "w") as f:
            f.write("#!/bin/sh\nexit 1\n")
        os.chmod(hook, 0o755)
        r = self.land("feat/OS-AAA-one")
        self.assertEqual(r.returncode, 1)
        self.assertIn("dev advanced locally to", r.stderr)
        self.assertIn("git push origin dev", r.stderr)

    def test_dry_run_changes_nothing(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        before = self.heads()
        r = self.land("feat/OS-AAA-one", "--dry-run", ci="false")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("would land", r.stdout)
        self.assertIn("merge: thing [OS-AAA]", r.stdout)
        self.assertEqual(self.heads(), before)
        self.assertFalse(os.path.exists(os.path.join(self.main, "a.txt")))

    def test_rerun_after_failure_resets_land_worktree(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        self.assertEqual(self.land("feat/OS-AAA-one", ci="false").returncode, 1)
        r = self.land("feat/OS-AAA-one")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        dev, origin = self.heads()
        self.assertEqual(dev, origin)

    def test_title_with_two_branches_is_usage_error(self):
        r = self.land("a/B", "c/D", "--title", "x")
        self.assertEqual(r.returncode, 2)

    def test_second_run_waits_for_the_lock(self):
        self.branch("feat/OS-AAA-one", {"a.txt": "a\n"}, "feat(ui): thing [OS-AAA]")
        lockfile = os.path.join(self.main, ".git", "land.lock")
        f = open(lockfile, "a+")
        fcntl.flock(f, fcntl.LOCK_EX)
        env = dict(ENV, O3OS_LAND_CI_CMD="true")
        p = subprocess.Popen([sys.executable, "-I", LAND, "feat/OS-AAA-one", "--dry-run"], cwd=self.main,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        line = p.stdout.readline()
        self.assertIn("waiting for the lock", line)
        time.sleep(0.2)
        self.assertIsNone(p.poll())
        fcntl.flock(f, fcntl.LOCK_UN)
        f.close()
        out, err = p.communicate(timeout=30)
        self.assertEqual(p.returncode, 0, out + err)


class Helpers(unittest.TestCase):
    def test_branch_id_table(self):
        land = load_land()
        table = {
            "chore/OS-OPS-CIISO": "OS-OPS-CIISO",
            "fix/OS-REL130-spdx": "OS-REL130",
            "feat/OS-MIDIREC-ui-polish": "OS-MIDIREC",
            "feat/OS-BUG-12-crash": "OS-BUG-12",
            "docs/OS-A": "OS-A",
            "feat/lowercase-only": None,
            "OS-NOSLASH-x": "OS-NOSLASH",
            "feat/x/OS-NESTED": None,
        }
        for branch, want in table.items():
            self.assertEqual(land.branch_id(branch), want, branch)

    def test_clean_subject(self):
        land = load_land()
        self.assertEqual(land.clean_subject("chore(tools): land.py, the merge path to dev [OS-OPS-LAND]"),
                         "land.py, the merge path to dev")
        self.assertEqual(land.clean_subject("fix: plain"), "plain")
        self.assertEqual(land.clean_subject("no prefix [X-1]"), "no prefix")
        self.assertEqual(land.merge_title("s", "OS-X"), "merge: s [OS-X]")

    def test_lock_helper_blocks_and_reports(self):
        land = load_land()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "land.lock")
            msgs = []
            first = land.acquire_lock(path, msgs.append)
            self.assertEqual(msgs, [])
            second = open(path, "a+")
            with self.assertRaises(OSError):
                fcntl.flock(second, fcntl.LOCK_EX | fcntl.LOCK_NB)
            first.close()
            fcntl.flock(second, fcntl.LOCK_EX | fcntl.LOCK_NB)
            second.close()


if __name__ == "__main__":
    unittest.main()
