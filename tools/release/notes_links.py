#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a release page (docs/release/<tag>.md) to stdout with its relative links made absolute (OS-RELGHA).

usage: notes_links.py PAGE REPOSITORY TAG     for example: notes_links.py docs/release/v1.4.0.md owner/repo v1.4.0

`[text](path)` and `[ref]: path` become links to https://github.com/REPOSITORY/blob/TAG/<path relative to the page's folder, normalised>.
Links with a scheme, a bare #anchor and absolute paths stay as they are. Run it from the repository root, with the page path from there.
"""
import posixpath
import re
import sys


def absolute(page, base, target):
    if re.match(r"^([A-Za-z][A-Za-z0-9+.-]*:|#|/)", target):
        return target
    path, hash_, frag = target.partition("#")
    full = posixpath.normpath(posixpath.join(posixpath.dirname(page), path))
    return base + full + hash_ + frag


def rewrite(text, page, repo, tag):
    base = "https://github.com/%s/blob/%s/" % (repo, tag)
    text = re.sub(r"(\]\()([^)\s]+)(\s*(?:\"[^\"]*\")?\))", lambda m: m.group(1) + absolute(page, base, m.group(2)) + m.group(3), text)
    return re.sub(r"(?m)^(\[[^\]]+\]:\s*)(\S+)", lambda m: m.group(1) + absolute(page, base, m.group(2)), text)


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        sys.stdout.write(rewrite(f.read(), argv[1], argv[2], argv[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
