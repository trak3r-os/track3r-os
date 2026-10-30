#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The HIL scenarios that platform/mk66/tools/hil/gen_hil.py generates are the committed JSON files, byte for byte.

A scenario of that directory is edited in one place only: the generator. A hand edit of a generated JSON (after a unit run, say) is overwritten by the next run of the generator, and a generator change with no
regenerated JSON leaves the files stale; both are caught here. Runs the generator into a temporary folder and compares every file it writes with the one in the directory. The JSON files the generator does not
write (the hand-written ones) are not looked at.

Usage: hil_scenarios_sync_test.py <repo root>
"""
import os
import subprocess
import sys
import tempfile


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    hil = os.path.join(argv[1], "platform", "mk66", "tools", "hil")
    problems = []
    with tempfile.TemporaryDirectory() as out:
        subprocess.run([sys.executable, "-I", os.path.join(hil, "gen_hil.py"), out], check=True, stdout=subprocess.DEVNULL)
        names = sorted(n for n in os.listdir(out) if n.endswith(".json"))
        for name in names:
            committed = os.path.join(hil, name)
            if not os.path.isfile(committed):
                problems.append("%s is generated but not committed" % name)
                continue
            with open(os.path.join(out, name), "rb") as f1, open(committed, "rb") as f2:
                if f1.read() != f2.read():
                    problems.append("%s differs from what gen_hil.py writes (edit the generator, then run `python3 -I platform/mk66/tools/hil/gen_hil.py`)" % name)
    for p in problems:
        print("hil_scenarios_sync: " + p, file=sys.stderr)
    if problems:
        return 1
    print("hil_scenarios_sync: ok (%d generated scenarios equal the committed files)" % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
