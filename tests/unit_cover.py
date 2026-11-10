# SPDX-License-Identifier: GPL-3.0-or-later
"""Every doctest case of the host unit binary runs in exactly one `unit_*` ctest (OS-CI).

usage: unit_cover.py <command...> -- <doctest filter option>...

<command...> is the unit binary with its runner prefix, if any; each filter option (`--source-file=...` or
`--source-file-exclude=...`, or several joined by `|` when the ctest passes more than one) is one group's filter, as its ctest passes it. The binary is asked for `--count`, which lists
nothing and runs nothing: first without a filter (the total), then once per group. The check passes when no group is empty
and the counts add up to the total. A case in two groups makes the sum too large, a case in none too small, a pattern that
matches nothing leaves its group at 0.
"""
import re
import subprocess
import sys

COUNT = re.compile(r"unskipped test cases passing the current filters: (\d+)")


def count(command, option):
    argv = command + ["--count"] + (option.split("|") if option else [])
    done = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False)
    found = COUNT.search(done.stdout)
    if done.returncode != 0 or not found:
        sys.exit("unit_cover: `%s` exited %d without a count:\n%s" % (" ".join(argv), done.returncode, done.stdout))
    return int(found.group(1))


def main(argv):
    if "--" not in argv:
        sys.exit(__doc__)
    split = argv.index("--")
    command, groups = argv[:split], argv[split + 1:]
    if not command or not groups:
        sys.exit(__doc__)

    total = count(command, None)
    sizes = [count(command, option) for option in groups]
    for option, size in zip(groups, sizes):
        print("%5d  %s" % (size, option if len(option) <= 100 else option[:97] + "..."))
    print("%5d  the whole binary" % total)

    failed = False
    for option, size in zip(groups, sizes):
        if size == 0:
            print("unit_cover: a group matches no test case: %s" % option, file=sys.stderr)
            failed = True
    if sum(sizes) != total:
        print("unit_cover: the groups hold %d cases, the binary has %d (%s)" %
              (sum(sizes), total, "some run twice" if sum(sizes) > total else "some run in no group"), file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
