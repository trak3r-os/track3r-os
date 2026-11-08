#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the manual's LLM guide from its chapters (OS-MANUALTOOLS).

  docs/manual/llms.txt        llmstxt.org index: title, summary, a link list of the chapters
  docs/manual/llms-full.txt   every chapter concatenated, hidden comments stripped

Both are generated, never edited by hand. `--check` regenerates in memory and exits 1 when a committed file differs.
Output is deterministic (no dates, no hashes). Stripped for the machine reader: HTML comments (the QA IDs, the
manual-sync marker, TODOs) and picture links (the PNGs and SVGs are committed, but a machine reader cannot see them), replaced by "(Picture: <alt text>)".
Standard library only; read-only on the sources.
"""

import argparse
import difflib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Reading order. Chapters not named here follow alphabetically; index.md and the template are never included.
ORDER = ["install.md", "getting_started.md", "panel.md", "song.md", "mixer.md", "perform.md", "perform_dj.md", "sample_load.md",
         "sample_edit.md", "wave.md", "record.md", "file.md", "usb_disk.md", "modules.md", "module-sounds.md", "module-instrument-fx.md", "insert_effects.md", "config.md",
         "computer_tools.md", "glossary.md"]
SKIP = {"index.md", "chapter-template.md"}
# One-line notes for chapters whose opening sentence says too little for a link list; the rest are derived from the chapter.
NOTES = {
    "install.md": "installing an update, what is verified, and going back to the official firmware",
    "getting_started.md": "what the OS is, the card, first boot, the keys and the screen layout",
    "mixer.md": "track levels, mute and solo, naming a track, the master page",
    "perform.md": "the three scenes and the live effects",
    "modules.md": "installing .tmod modules from the card and the bundled apps",
    "module-sounds.md": "the new parameters of the TB-303 (MODS page), FM (16 algorithms, Detune) and Chip (learnable wave and duty)",
    "module-instrument-fx.md": "which step FX a module instrument obeys",
    "insert_effects.md": "the Fold, Env Filter, Phaser and Comp track inserts: what each does and its settings",
    "computer_tools.md": "the USB console on a computer: connect, screenshots, firmware path, diagnostics, key injection",
    "glossary.md": "every term of the manual, one entry each",
}

TITLE = "track3r-os"
SUMMARY = ("Community operating system for the Polyend Tracker hardware. Unofficial: not affiliated with or endorsed "
           "by Polyend. This is the user manual, one chapter per screen; read the chapter of the screen you are asked about.")
NOTE = ("Key notation: keys are written in code style, for example `Shift` + `F3` or hold `Instr`; `F1` to `F8` are the "
        "soft keys under the screen. If a thing is not in the manual, it is not known: say so.")

COMMENT_RE = re.compile(r"<!--.*?-->", re.S)
IMAGE_RE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
BLANKS_RE = re.compile(r"\n{3,}")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def clean(text):
    text = COMMENT_RE.sub("", text)
    text = IMAGE_RE.sub(lambda m: "(Picture: %s)" % m.group(1).strip() if m.group(1).strip() else "", text)
    text = "\n".join(line.rstrip() for line in text.split("\n"))
    return BLANKS_RE.sub("\n\n", text).strip() + "\n"


def chapter_files(root):
    d = os.path.join(root, "docs", "manual")
    have = sorted(n for n in os.listdir(d) if n.endswith(".md") and n not in SKIP)
    return [n for n in ORDER if n in have] + [n for n in have if n not in ORDER]


def title_of(text):
    m = re.search(r"^#\s+(.+?)\s*$", text, re.M)
    return m.group(1) if m else "Untitled"


def blurb(text):
    """One line for the link list: first sentence of 'What it is for', else the first paragraph after the title."""
    body = clean(text)
    m = re.search(r"^##\s+What it is for\s*\n+(.+?)(?:\n\n|\Z)", body, re.S | re.M)
    para = m.group(1) if m else None
    if para is None:
        for block in re.split(r"\n\n+", body):
            b = block.strip()
            if b and not b.startswith(("#", "Status:", "|", ">", "-", "(Picture")):
                para = b
                break
    para = " ".join((para or "").split())
    para = re.sub(r"[`*_]|\[([^\]]*)\]\([^)]*\)", lambda mm: mm.group(1) or "", para)
    sent = re.split(r"(?<=[.!?])\s", para, maxsplit=1)[0]
    return sent if len(sent) <= 140 else sent[:137].rstrip() + "..."


def build(root):
    names = chapter_files(root)
    texts = {n: read(os.path.join(root, "docs", "manual", n)) for n in names}
    idx = ["# " + TITLE, "", "> " + SUMMARY, "", NOTE, "", "## Reference", "",
           "- [Full manual](llms-full.txt): every chapter in one file, for loading whole into a context window", "",
           "## Chapters", ""]
    for n in names:
        b = NOTES.get(n) or blurb(texts[n])
        idx.append("- [%s](%s)%s" % (title_of(texts[n]), n, ": " + b if b else ""))
    idx += ["", "## Optional", "",
            "- [Manual index](index.md): the chapter list and how the manual is written", ""]
    full = ["# " + TITLE + " user manual", "", "> " + SUMMARY, "", NOTE, ""]
    for n in names:
        full += ["", "---", ""]
        full.append(clean(texts[n]).rstrip("\n"))
    return {"llms.txt": "\n".join(idx), "llms-full.txt": "\n".join(full).rstrip("\n") + "\n"}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed files differ from what would be generated")
    args = ap.parse_args(argv)
    out = build(args.root)
    d = os.path.join(args.root, "docs", "manual")
    bad = 0
    for name, text in out.items():
        path = os.path.join(d, name)
        if args.check:
            cur = read(path) if os.path.exists(path) else None
            if cur != text:
                bad += 1
                print("manual_llms: docs/manual/%s is %s; run tools/manual_llms.py and commit the result"
                      % (name, "missing" if cur is None else "out of date"), file=sys.stderr)
                if cur is not None:
                    diff = list(difflib.unified_diff(cur.splitlines(), text.splitlines(), "committed", "generated", lineterm="", n=0))
                    for line in diff[:12]:
                        print("  " + line, file=sys.stderr)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print("manual_llms: wrote docs/manual/%s (%d bytes)" % (name, len(text.encode("utf-8"))))
    if args.check and not bad:
        print("manual_llms: llms.txt and llms-full.txt are up to date")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
