#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-MERGETOOL: tools/o3merge.py on temporary repositories: the three-way merge of docs/manual/sync.lock (unit cases), and end to end
a merge whose only conflicts are in the generated files (resolved and committed, the check tools run), a merge with a conflict in a
non-generated file (exit 2, the merge left in progress, then --continue), and the preflight faults.

usage: o3merge_test.py <repository root>
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile


def load(root, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(root, "tools", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def git(d, *args):
    return subprocess.run(["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
                          check=True, capture_output=True, text=True).stdout.strip()


def git_rc(d, *args):
    """(rc, stdout) without check, for the commands that may fail (MERGE_HEAD present or not)."""
    r = subprocess.run(["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
                       capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def tool(root, d, *args):
    """Run the real CLI: the root's tools/o3merge.py in the temporary repository d (so the helpers come from d's tools/, like on an
    older checkout). -> (rc, output)"""
    r = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "o3merge.py"), *args],
                       cwd=d, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def helper(d, name, *args):
    return subprocess.run([sys.executable, "-I", os.path.join(d, "tools", name), "--root", d, *args],
                          capture_output=True, text=True)


CAT_MIXER = ("| ID | Function | How | Expected | Sim | Unit | Spec | Status |\n|---|---|---|---|---|---|---|---|\n"
             "| QA-MIX-001 | one | F1 | a | - | manual: reviewed | OS-X | open |\n"
             "| QA-MIX-002 | two | F2 | b | - | manual: reviewed | OS-X | open |\n")
CAT_GLOBAL = ("| ID | Function | How | Expected | Sim | Unit | Spec | Status |\n|---|---|---|---|---|---|---|---|\n"
              "| QA-GLB-001 | g one | F3 | c | - | manual: reviewed | OS-X | open |\n")
README_TMPL = ("# QA catalogue\n\nThe rows of the screens.\n\n<!-- qa-cover -->\n\n| Screen | File | Functions | Sim PASS | Unit PASS | Open |\n"
               "|---|---|---|---|---|---|\n\n<!-- /qa-cover -->\n")
INDEX = "# Index\n\n[Mixer](mixer.md)\n\n## Notes\n\nSome text.\n\nMore text.\n"
CHAPTER = ("# Mixer\n\n## What it is for\n\nThe Mixer sets levels.\n\n## How it works\n\n"
           "<!-- sync: src=app/src/x.cpp qa=QA-MIX-001 -->\n\nPress F1.\n\nText.\n")
AAA = "# Aaa\n\n## What it is for\n\nDoes aaa things.\n"
BBB = "# Bbb\n\n## What it is for\n\nDoes bbb things.\n"


def lock_unit_tests(om, mc):
    """The three-way merge of sync.lock, on plain texts: one side changed; both the same; a key added; a key removed; an item changed on
    both sides differently (re-ack); an item removed on one side and changed on the other (re-ack); a key removed on one side and changed
    on the other (re-ack); the header from ours; the entry lines exactly as manual_check.lock_text writes them."""
    H = "# header of ours\n"
    base = H + "k1 a=1 b=2\nk2 c=3\n"

    def merged(ours, theirs, b=base):
        return om.merge_lock_texts(b, ours, theirs, mc.lock_text)

    text, reack = merged(H + "k1 a=9 b=2\nk2 c=3\n", base)  # ours changed k1 only
    assert om.parse_lock_text(text) == {"k1": {"a": "9", "b": "2"}, "k2": {"c": "3"}} and reack == [], (text, reack)
    text, reack = merged(H + "k1 a=9 b=2\nk2 c=3\n", H + "k1 a=9 b=2\nk2 c=3\n")  # both the same
    assert om.parse_lock_text(text) == {"k1": {"a": "9", "b": "2"}, "k2": {"c": "3"}} and reack == [], (text, reack)
    text, reack = merged(base, base + "k3 d=4\n")  # a key added on theirs
    assert om.parse_lock_text(text) == {"k1": {"a": "1", "b": "2"}, "k2": {"c": "3"}, "k3": {"d": "4"}} and reack == [], (text, reack)
    text, reack = merged(H + "k1 a=1 b=2\n", base)  # k2 removed on ours, unchanged on theirs
    assert om.parse_lock_text(text) == {"k1": {"a": "1", "b": "2"}} and reack == [], (text, reack)
    text, reack = merged(H + "k1 a=2 b=2\nk2 c=3\n", H + "k1 a=3 b=2\nk2 c=3\n")  # item a changed on both, differently
    assert om.parse_lock_text(text) == {"k1": {"a": "3", "b": "2"}, "k2": {"c": "3"}} and reack == ["k1"], (text, reack)
    text, reack = merged(H + "k1 b=2\nk2 c=3\n", H + "k1 a=5 b=2\nk2 c=3\n")  # item a removed on ours, changed on theirs
    assert om.parse_lock_text(text) == {"k1": {"a": "5", "b": "2"}, "k2": {"c": "3"}} and reack == ["k1"], (text, reack)
    text, reack = merged(H + "k1 a=1 b=2\n", H + "k1 a=1 b=2\nk2 c=9\n")  # key k2 removed on ours, changed on theirs
    assert om.parse_lock_text(text) == {"k1": {"a": "1", "b": "2"}, "k2": {"c": "9"}} and reack == ["k2"], (text, reack)
    # the header comes from ours, and the entry lines are exactly lock_text's (format and order)
    ours = "# header of ours\n# second header line\n" + "k1 a=9 b=2\nk2 c=3\n"
    text, reack = merged(ours, base)
    lock = om.parse_lock_text(text)
    assert text.startswith("# header of ours\n# second header line\n"), text
    entries = [l for l in mc.lock_text(lock).splitlines() if l and not l.startswith("#")]
    assert text == "# header of ours\n# second header line\n" + "\n".join(entries) + "\n", (text, entries)
    # a missing base (the file added on both sides) is an empty base
    text, reack = om.merge_lock_texts("", H + "k1 a=1\n", H + "k1 a=2\n", mc.lock_text)
    assert om.parse_lock_text(text) == {"k1": {"a": "2"}} and reack == ["k1"], (text, reack)


def build_repo(root, d):
    """A small repository with the manual, the catalogue and the generated files, on a dev branch; the helper tools of the root are
    copied into its tools/."""
    os.makedirs(os.path.join(d, "tools"))
    for t in ("manual_check.py", "qa_cover.py", "manual_llms.py", "o3merge.py"):
        shutil.copy(os.path.join(root, "tools", t), os.path.join(d, "tools", t))
    m, cat = os.path.join(d, "docs", "manual"), os.path.join(d, "docs", "qa", "catalogue")
    write(os.path.join(d, "app", "src", "x.cpp"), "one\n")
    write(os.path.join(d, "docs", "specs", "S.md"), "spec\n")
    write(os.path.join(cat, "mixer.md"), CAT_MIXER)
    write(os.path.join(cat, "global.md"), CAT_GLOBAL)
    write(os.path.join(cat, "README.md"), README_TMPL)
    write(os.path.join(m, "index.md"), INDEX)
    write(os.path.join(m, "images.txt"), "# manifest\n")
    write(os.path.join(m, "mixer.md"), CHAPTER)
    git(d, "init", "-q")
    git(d, "checkout", "-q", "-b", "dev")
    git(d, "config", "user.name", "t")
    git(d, "config", "user.email", "t@example.invalid")
    git(d, "add", "-A")
    git(d, "commit", "-q", "-m", "one")
    assert helper(d, "manual_check.py", "--ack-all").returncode == 0
    assert helper(d, "qa_cover.py", "--write").returncode == 0
    assert helper(d, "manual_llms.py").returncode == 0
    assert helper(d, "manual_check.py", "--strict").returncode == 0
    assert helper(d, "qa_cover.py", "--check").returncode == 0
    assert helper(d, "manual_llms.py", "--check").returncode == 0
    git(d, "add", "-A")
    git(d, "commit", "-q", "-m", "generated")


def generated_conflict_test(root):
    """End to end: both branches change the generated files (and only auto-mergeable things besides), the tool resolves them, runs the
    checks and commits."""
    with tempfile.TemporaryDirectory() as d:
        build_repo(root, d)
        m, cat = os.path.join(d, "docs", "manual"), os.path.join(d, "docs", "qa", "catalogue")

        # feat/a: another source, another row, another chapter
        git(d, "checkout", "-q", "-b", "feat/a")
        write(os.path.join(d, "app", "src", "x.cpp"), "two\n")
        write(os.path.join(m, "mixer.md"), CHAPTER.replace("The Mixer sets levels.", "The Mixer sets the levels."))
        write(os.path.join(cat, "mixer.md"), CAT_MIXER + "| QA-MIX-003 | three | F4 | d | - | manual: reviewed | OS-X | open |\n")
        write(os.path.join(m, "aaa.md"), AAA)
        write(os.path.join(m, "index.md"), INDEX.replace("[Mixer](mixer.md)", "[Mixer](mixer.md), [Aaa](aaa.md)"))
        assert helper(d, "manual_check.py", "--ack", "mixer.md#how-it-works").returncode == 0
        assert helper(d, "qa_cover.py", "--write").returncode == 0
        assert helper(d, "manual_llms.py").returncode == 0
        assert helper(d, "manual_check.py", "--strict").returncode == 0
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "a")

        # feat/b: a changed row (another hash of the same lock line), a row in another file, another chapter
        git(d, "checkout", "-q", "dev")
        git(d, "checkout", "-q", "-b", "feat/b")
        write(os.path.join(cat, "mixer.md"), CAT_MIXER.replace("| F1 | a |", "| F1 | a changed |"))
        write(os.path.join(cat, "global.md"), CAT_GLOBAL + "| QA-GLB-002 | g two | F5 | e | - | manual: reviewed | OS-X | open |\n")
        write(os.path.join(m, "mixer.md"), CHAPTER.replace("Press F1.", "Press F2."))
        write(os.path.join(m, "bbb.md"), BBB)
        write(os.path.join(m, "index.md"), INDEX + "\n[Bbb](bbb.md)\n")
        assert helper(d, "manual_check.py", "--ack", "mixer.md#how-it-works").returncode == 0
        assert helper(d, "qa_cover.py", "--write").returncode == 0
        assert helper(d, "manual_llms.py").returncode == 0
        assert helper(d, "manual_check.py", "--strict").returncode == 0
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "b")

        git(d, "checkout", "-q", "feat/a")
        rc, out = tool(root, d, "feat/b", "-m", "merge feat/b into feat/a")
        assert rc == 0, (rc, out)
        assert "docs/manual/sync.lock" in out and "docs/qa/catalogue/README.md" in out and "llms" in out, out
        assert git_rc(d, "rev-parse", "-q", "--verify", "MERGE_HEAD")[0] != 0  # the merge is done
        assert len(git(d, "rev-parse", "HEAD^2").split()) == 1 and git(d, "log", "-1", "--format=%B") == "merge feat/b into feat/a"
        assert git(d, "diff", "--name-only", "--diff-filter=U") == ""
        # the lock line holds both sides' changes: the source a changed and the QA row b changed
        mc = load(root, "manual_check")
        lock = mc.read_lock(d)
        row = mc.row_hash(mc.qa_rows(mc.catalogue_texts(d)), "QA-MIX-001")
        assert lock["mixer.md#how-it-works"] == {"app/src/x.cpp": mc.path_hash(d, "app/src/x.cpp"), "QA-MIX-001": row}, lock
        # the checks ran and pass on the committed merge
        assert helper(d, "manual_check.py", "--strict").returncode == 0
        assert helper(d, "manual_check.py", "--gate").returncode == 0
        assert helper(d, "qa_cover.py", "--check").returncode == 0
        assert helper(d, "manual_llms.py", "--check").returncode == 0
        llms = open(os.path.join(m, "llms.txt"), encoding="utf-8").read()
        assert "[Aaa](aaa.md)" in llms and "[Bbb](bbb.md)" in llms, llms
        readme = open(os.path.join(cat, "README.md"), encoding="utf-8").read()
        assert "| Mixer | [mixer.md](mixer.md) | 3 |" in readme and "| Global | [global.md](global.md) | 2 |" in readme, readme

        # preflight: a detached HEAD without --allow-detached, and a ref that is not a commit
        git(d, "checkout", "-q", "--detach", "HEAD")
        rc, out = tool(root, d, "dev", "-m", "x")
        assert rc == 1 and "detached" in out, (rc, out)
        git(d, "checkout", "-q", "feat/a")
        rc, out = tool(root, d, "not-a-ref", "-m", "x")
        assert rc == 1 and "not a commit" in out, (rc, out)


def hand_conflict_test(root):
    """End to end: a conflict in a non-generated file -> exit 2, MERGE_HEAD still there; resolve it, git add, --continue -> committed."""
    with tempfile.TemporaryDirectory() as d:
        build_repo(root, d)
        git(d, "checkout", "-q", "-b", "feat/c")
        write(os.path.join(d, "app", "src", "x.cpp"), "c\n")
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "c")
        git(d, "checkout", "-q", "dev")
        git(d, "checkout", "-q", "-b", "feat/d")
        write(os.path.join(d, "app", "src", "x.cpp"), "d\n")
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "d")
        git(d, "checkout", "-q", "feat/c")

        rc, out = tool(root, d, "feat/d", "-m", "merge feat/d")
        assert rc == 2, (rc, out)
        assert "app/src/x.cpp" in out and "--continue" in out, out
        assert git_rc(d, "rev-parse", "-q", "--verify", "MERGE_HEAD")[0] == 0  # the merge is left in progress

        write(os.path.join(d, "app", "src", "x.cpp"), "d\n")  # resolved by hand, theirs
        git(d, "add", "app/src/x.cpp")
        rc, out = tool(root, d, "--continue", "-m", "merge feat/d")
        assert rc == 0, (rc, out)
        assert git_rc(d, "rev-parse", "-q", "--verify", "MERGE_HEAD")[0] != 0
        assert git(d, "log", "-1", "--format=%B") == "merge feat/d"
        assert git(d, "diff", "--name-only", "--diff-filter=U") == ""
        assert helper(d, "manual_check.py", "--strict").returncode == 0
        assert open(os.path.join(d, "app", "src", "x.cpp")).read() == "d\n"


def reack_conflict_test(root):
    """End to end: both sides changed the same cited item differently (the file itself auto-merges), so the lock item conflicts and
    the key is re-acked to the merged content's hash."""
    with tempfile.TemporaryDirectory() as d:
        build_repo(root, d)
        git(d, "checkout", "-q", "-b", "feat/a")
        write(os.path.join(d, "app", "src", "x.cpp"), "one\naaa\n")  # appended at the end
        assert helper(d, "manual_check.py", "--ack", "mixer.md#how-it-works").returncode == 0
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "a")
        git(d, "checkout", "-q", "dev")
        git(d, "checkout", "-q", "-b", "feat/b")
        write(os.path.join(d, "app", "src", "x.cpp"), "bbb\none\n")  # prepended at the start: the file auto-merges
        assert helper(d, "manual_check.py", "--ack", "mixer.md#how-it-works").returncode == 0
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "b")
        git(d, "checkout", "-q", "feat/a")
        rc, out = tool(root, d, "feat/b", "-m", "merge feat/b")
        assert rc == 0, (rc, out)
        assert "re-acknowledged" in out and "mixer.md#how-it-works" in out, out
        assert open(os.path.join(d, "app", "src", "x.cpp")).read() == "bbb\none\naaa\n"
        mc = load(root, "manual_check")
        lock = mc.read_lock(d)
        assert lock["mixer.md#how-it-works"]["app/src/x.cpp"] == mc.path_hash(d, "app/src/x.cpp"), lock  # the merged content's hash
        assert git_rc(d, "rev-parse", "-q", "--verify", "MERGE_HEAD")[0] != 0
        assert git(d, "log", "-1", "--format=%B") == "merge feat/b"
        assert helper(d, "manual_check.py", "--strict").returncode == 0


def markers_left_test(root):
    """End to end: a hand-resolved file is `git add`ed with conflict markers left, then --continue refuses (exit 3): nothing
    committed, MERGE_HEAD still there."""
    with tempfile.TemporaryDirectory() as d:
        build_repo(root, d)
        git(d, "checkout", "-q", "-b", "feat/c")
        write(os.path.join(d, "app", "src", "x.cpp"), "c\n")
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "c")
        git(d, "checkout", "-q", "dev")
        git(d, "checkout", "-q", "-b", "feat/d")
        write(os.path.join(d, "app", "src", "x.cpp"), "d\n")
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "d")
        git(d, "checkout", "-q", "feat/c")
        rc, out = tool(root, d, "feat/d", "-m", "merge feat/d")
        assert rc == 2 and "app/src/x.cpp" in out, (rc, out)
        write(os.path.join(d, "app", "src", "x.cpp"), "<<<<<<< HEAD\nc\n=======\nd\n>>>>>>> feat/d\n")  # resolved by hand, markers left
        git(d, "add", "app/src/x.cpp")
        rc, out = tool(root, d, "--continue", "-m", "merge feat/d")
        assert rc == 3, (rc, out)
        assert "conflict marker" in out and "app/src/x.cpp" in out, out
        assert git_rc(d, "rev-parse", "-q", "--verify", "MERGE_HEAD")[0] == 0  # the merge is still in progress
        assert git(d, "log", "-1", "--format=%s") == "c"  # nothing was committed


def main():
    root = os.path.abspath(sys.argv[1])
    om, mc = load(root, "o3merge"), load(root, "manual_check")
    lock_unit_tests(om, mc)
    generated_conflict_test(root)
    hand_conflict_test(root)
    reack_conflict_test(root)
    markers_left_test(root)
    print("o3merge_test: ok")


if __name__ == "__main__":
    main()
