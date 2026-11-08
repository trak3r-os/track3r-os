#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the user manual (docs/manual/) and list what the QA catalogue changed under it (OS-MANUALTOOLS).

Default mode reports, for every chapter:
  - broken relative links (images are not links; see below);
  - chapters that index.md does not mention;
  - open `<!-- TODO ... -->` / `<!-- CONFLICT ... -->` comments (information, never a failure);
  - image references that have no file on disk: the pictures are committed (docs/manual/img/), so a chapter that shows
    a picture whose file is missing renders broken on GitHub, whether or not images.txt (PNGs) or diagrams.txt (SVGs)
    lists it (a line marked `# MISSING` in images.txt means no file yet). Without a manifest line the message is
    "image with no manifest line".

--stale adds the update list. Each chapter carries `<!-- manual-sync: <sha> -->`: the commit of the repository
the chapter was last brought in step with. For the catalogue file(s) of the chapter (the CATALOGUE table below), the
QA IDs of rows added or changed since that sha (`git diff <sha> -- <file>`) are listed, and so are the catalogue IDs that
no chapter cites (information: some screens have no chapter yet). A chapter without a sync marker is reported.

The chapter -> catalogue mapping is the CATALOGUE table in this script (not a front-matter line): one place to edit,
and the chapters stay free of metadata other than the sync marker.

Accessibility (OS-MANUAL2 M2-5, rule D11; every page of docs/manual/, the index and the chapter template included), each a problem:
  - an image (`![alt](file)` or `<img>`) with no alt text, only blanks, a file name, "screenshot of ..." or 25 words or more (the rule is under 25);
  - a heading that is not the one `#` title first, a second `#`, or a level that skips one (`##` straight to `####`), or an empty heading;
  - a Markdown table with no header separator row, an empty header cell, or a separator with another number of cells than the header;
  - a link whose text is "here", "click here", "this", "this page", "link" or "this link", is empty, or is a bare URL; a URL left in the text
    (not one inside an HTML tag such as `<img src="https://...">`). The capitalised word `Link` passes: it can be the name of a feature.
Meaning carried by colour alone, and the contrast of the SVG diagrams, cannot be checked from the text: `-v` lists the colour words of each
chapter for a reviewer, and never fails.

Sync v2 (OS-MANUAL2 M2-3): a section carries `<!-- sync: src=.. spec=.. qa=.. -->`, and docs/manual/sync.lock holds what each cited item was when the section was
last checked. The branch gate (--gate, in ci.sh host) fails when a section cites something this branch changed and neither the section nor its lock line did, and prints
the one command that acknowledges them all:
  --ack <chapter>#<section>   one section, whole line        --ack-path <file or QA ID>...   every section that cites it, that item only
  --ack-all                   every line, from the tree as it is (the first migration, or the lock after a merge conflict in it)
A merge conflict in sync.lock: take either side, then run --ack-path <file> on the merged tree for each file both sides acknowledged (or --ack-all).
A manual page cited by another section is hashed without its `manual-tables` blocks, so a renamed key does not reach the cheat sheet's sections.

Exit code: 0 always, except with --strict: 1 when there is a problem (broken link, chapter missing from the
index, image with no file, an accessibility fault, and with --stale a changed QA ID or a missing sync marker). Open TODOs and
uncited catalogue IDs never fail.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

# chapter file -> the catalogue files (docs/qa/catalogue/<name>.md) whose rows the chapter documents; "*" is every file; [] a chapter that has no
# catalogue file and is kept in step by its `sync:` markers only; a string exempts the chapter from the sync-sha check, with the reason.
CATALOGUE = {
    "cheat_sheet.md": "*",  # the keys of every screen on one page
    "computer_tools.md": [],  # the diag console and the host tools (platform/mk66/tools): `sync: src=` markers
    "config.md": ["config"],
    "export.md": ["pattern", "file"],
    "file.md": ["file"],
    "usb_disk.md": ["global", "config"],
    "fx.md": ["pattern"],
    "getting_started.md": ["global"],
    "glossary.md": "exempt: a list of terms; the entries that state behaviour carry `sync:` markers, the rest follow no catalogue file",
    "install.md": [],  # the release files and the update path: `sync: src=` markers
    "input_test.md": ["input_test"],
    "instrument.md": ["instrument"],
    "mixer.md": ["mixer"],
    "module-instrument-fx.md": ["modules"],
    "insert_effects.md": ["global"],
    "module_app.md": ["module_app"],
    "module-sounds.md": ["modules"],
    "modules.md": ["modules"],
    "panel.md": ["global"],
    "pattern.md": ["pattern"],
    "pattern_tools.md": ["pattern"],
    "perform_dj.md": ["perform"],
    "perform.md": ["perform"],
    "quick_start.md": ["global"],
    "record.md": ["record"],
    "sample_edit.md": ["sample_edit"],
    "sample_load.md": ["sample_load"],
    "song.md": ["song"],
    "wave.md": ["wave"],
}
NOT_CHAPTERS = {"index.md", "chapter-template.md"}

COMMENT_RE = re.compile(r"<!--(.*?)-->", re.S)
FENCE_RE = re.compile(r"^```.*?^```", re.S | re.M)
LINK_RE = re.compile(r"(!?)\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
QA_RE = re.compile(r"QA-[A-Z0-9]+-\d+")
SYNC_RE = re.compile(r"<!--\s*manual-sync:\s*([0-9a-fA-F]{4,40})\s*-->")
ROW_RE = re.compile(r"^\|\s*(QA-[A-Z0-9]+-\d+)\s*\|")
DIFF_ROW_RE = re.compile(r"^([+-])\|\s*(QA-[A-Z0-9]+-\d+)\s*\|(.*)$")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def strip_hidden(text):
    """Text without HTML comments and fenced code blocks (links and images there do not count)."""
    return FENCE_RE.sub("", COMMENT_RE.sub("", text))


def blank_hidden(text):
    """strip_hidden that keeps every line break, so a line number of the result is a line of the file."""
    keep = lambda m: "\n" * m.group(0).count("\n")
    return FENCE_RE.sub(keep, COMMENT_RE.sub(keep, text))


def mask_fences(text):
    """The text with fenced code blocks emptied (their line breaks stay); comments stay, since the sync markers are comments."""
    return FENCE_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)


# ---- Accessibility (OS-MANUAL2 M2-5) ------------------------------------------------------------------------------------------------

GENERIC_LINK_TEXT = {"here", "click here", "this", "this page", "link", "this link"}
HEADING_RE = re.compile(r"^(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
SEPARATOR_RE = re.compile(r"^\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)*\|?[ \t]*$")
IMG_TAG_RE = re.compile(r"<img\b[^>]*>", re.I)
ALT_ATTR_RE = re.compile(r"""\balt\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.I)
URL_RE = re.compile(r"(?:https?://|www\.)[^\s)>\]]+", re.I)
CODE_SPAN_RE = re.compile(r"`[^`\n]*`")
HTML_TAG_RE = re.compile(r"<[A-Za-z][A-Za-z0-9]*(?=[\s/>])[^>]*>")  # not an autolink: `<https://...>` has a colon right after its first word
CAPITAL_OK = {"Link"}  # link texts that are not generic when written with a capital
MAX_ALT_WORDS = 25  # the skill's rule (section 8): under 25 words. (tools/manual_diagrams.py holds the diagrams' alt text to at most 25.)
ALT_BAD_START = ("screenshot of", "image of", "picture of", "photo of")
ALT_FILE_RE = re.compile(r"\.(?:png|svg|jpe?g|gif|webp)$", re.I)
COLOUR_RE = re.compile(r"\b(red|green|blue|amber|yellow|orange|purple|pink|violet|white|black|grey|gray)\b", re.I)


def alt_problem(alt):
    """What is wrong with an image's alt text (rule D11: one sentence that says what the picture shows), or None."""
    alt = " ".join(alt.split())
    if not alt:
        return "no alt text"
    if ALT_FILE_RE.search(alt):
        return "alt text is a file name: %s" % alt
    if alt.lower().startswith(ALT_BAD_START):
        return "alt text starts with \"%s\": say what the picture shows" % " ".join(alt.split()[:2])
    if len(alt.split()) >= MAX_ALT_WORDS:
        return "alt text is %d words, keep it under %d" % (len(alt.split()), MAX_ALT_WORDS)
    return None


def table_cells(line):
    """The cells of a table row: split on every pipe that is not escaped, as GitHub does (a pipe in a code span splits too)."""
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]
    return [c.strip() for c in re.split(r"(?<!\\)\|", s)]


def a11y_problems(text):
    """[(line, message)] for the accessibility faults of one page (rule D11): see the module docstring."""
    out = []
    lines = blank_hidden(text).split("\n")
    level = 0
    for no, line in enumerate(lines, 1):
        m = HEADING_RE.match(line)
        if m:
            n, title = len(m.group(1)), (m.group(2) or "").strip()
            if not title:
                out.append((no, "empty heading"))
            if level == 0 and n != 1:
                out.append((no, "the first heading is `%s`, a page starts with one `#` title" % ("#" * n)))
            elif n == 1 and level != 0:
                out.append((no, "a second `#` heading: only the title is level 1"))
            elif n > level + 1 and level != 0:
                out.append((no, "heading level skips from `%s` to `%s`: %s" % ("#" * level, "#" * n, title)))
            level = n
    i = 0
    while i < len(lines):
        if not lines[i].lstrip().startswith("|"):
            i += 1
            continue
        j = i
        while j < len(lines) and lines[j].lstrip().startswith("|"):
            j += 1
        block = lines[i:j]
        head = table_cells(block[0])
        if len(block) < 2 or not SEPARATOR_RE.match(block[1].strip()):
            out.append((i + 1, "table with no header separator row (`|---|---|`) under its first row"))
        else:
            if "" in head:
                out.append((i + 1, "table with an empty header cell"))
            if len(table_cells(block[1])) != len(head):
                out.append((i + 2, "the header separator has %d cells, the header %d" % (len(table_cells(block[1])), len(head))))
        i = j
    for no, line in enumerate(lines, 1):
        for bang, label, target in LINK_RE.findall(line):
            plain = label.strip()
            if bang:
                bad = alt_problem(label)
                if bad:
                    out.append((no, "image %s: %s" % (target, bad) if bad != "no alt text" else "image with no alt text: %s" % target))
            elif not plain:
                out.append((no, "link with no text: %s" % target))
            elif plain.strip(" .,:;!") not in CAPITAL_OK and plain.lower().strip(" .,:;!") in GENERIC_LINK_TEXT:
                out.append((no, "link text \"%s\" does not name its target (%s)" % (plain, target)))
            elif URL_RE.fullmatch(plain):
                out.append((no, "link text is a bare URL: %s" % plain))
        for tag in IMG_TAG_RE.findall(line):
            m = ALT_ATTR_RE.search(tag)
            bad = alt_problem((m.group(1) or m.group(2) or "") if m else "")
            if bad:
                out.append((no, "<img> %s: %s" % (bad, tag[:60])))
        rest = LINK_RE.sub("", HTML_TAG_RE.sub("", CODE_SPAN_RE.sub("", line)))
        for url in URL_RE.findall(rest):
            out.append((no, "a URL left in the text (make it a link whose text names the target): %s" % url))
    return sorted(set(out))


def colour_notes(text):
    """[(line, text)] of the prose lines that name a colour: for a reviewer to see the meaning is also in words (never a failure)."""
    out = []
    for no, line in enumerate(blank_hidden(text).split("\n"), 1):
        if not line.lstrip().startswith("|") and COLOUR_RE.search(CODE_SPAN_RE.sub("", line)):
            out.append((no, " ".join(line.split())[:140]))
    return out


def chapters(root):
    d = os.path.join(root, "docs", "manual")
    return sorted(n for n in os.listdir(d) if n.endswith(".md") and n not in NOT_CHAPTERS)


def manifest_names(root):
    """Image names listed in docs/manual/images.txt (first field of each non-comment line) and the [<name>.svg] stanzas of
    docs/manual/diagrams.txt (OS-MANUALDIAG)."""
    names = set()
    path = os.path.join(root, "docs", "manual", "images.txt")
    if os.path.exists(path):
        for line in read(path).splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                names.add(line.split()[0])
    path = os.path.join(root, "docs", "manual", "diagrams.txt")
    if os.path.exists(path):
        for line in read(path).splitlines():
            m = re.fullmatch(r"\[([a-z0-9_]+\.svg)\]", line.strip())
            if m:
                names.add(m.group(1))
    return names


def catalogue_ids(root, name):
    path = os.path.join(root, "docs", "qa", "catalogue", name + ".md")
    if not os.path.exists(path):
        return []
    ids = []
    for line in read(path).splitlines():
        m = ROW_RE.match(line)
        if m:
            ids.append(m.group(1))
    return ids


def qa_changes(root, sha, name):
    """[(QA ID, "added" | "changed" | "removed")] for the rows of the catalogue file that differ since sha, in the order the diff has them; None
    when sha is unknown. A row that only moved is not in it."""
    rel = "docs/qa/catalogue/%s.md" % name
    out = git_out(root, "diff", "-U0", sha, "--", rel)
    if out is None:
        return None
    plus, minus, order = {}, {}, []
    for line in out.splitlines():
        m = DIFF_ROW_RE.match(line)
        if m:
            (plus if m.group(1) == "+" else minus)[m.group(2)] = " ".join(m.group(3).split())
            if m.group(2) not in order:
                order.append(m.group(2))
    kinds = []
    for i in order:
        if i in plus and i not in minus:
            kinds.append((i, "added"))
        elif i in plus and plus[i] != minus[i]:
            kinds.append((i, "changed"))
        elif i not in plus:
            kinds.append((i, "removed"))
    return kinds


def check_chapter(root, name, manifest, index_text):
    path = os.path.join(root, "docs", "manual", name)
    raw = read(path)
    visible = strip_hidden(raw)
    res = {"broken": [], "no_manifest": [], "no_file": [], "todos": [], "in_index": name in index_text}
    base = os.path.dirname(path)
    for bang, _label, target in LINK_RE.findall(visible):
        if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("#"):
            continue  # http:, mailto:, same-page anchor
        file_part = target.split("#", 1)[0]
        exists = os.path.exists(os.path.normpath(os.path.join(base, file_part)))
        if bang:
            if not exists:
                res["no_manifest" if os.path.basename(file_part) not in manifest else "no_file"].append(target)
        elif not exists:
            res["broken"].append(target)
    for body in COMMENT_RE.findall(raw):
        body = body.strip()
        if body.startswith("TODO") or body.startswith("CONFLICT"):
            res["todos"].append(" ".join(body.split()))
    m = SYNC_RE.search(raw)
    res["sync"] = m.group(1) if m else None
    res["cites"] = set(QA_RE.findall(raw))
    res["a11y"] = a11y_problems(raw)
    res["colour"] = colour_notes(raw)
    return res


# ---- Sync v2 (OS-MANUAL2 M2-3) --------------------------------------------------------------------------------------------------------

LOCK = "docs/manual/sync.lock"
SYNC2_RE = re.compile(r"<!--\s*sync:(.*?)-->", re.S)
OLD_SRC_RE = re.compile(r"<!--\s*src:")
MARKER_FIELDS = ("src", "spec", "qa")
NEEDS_MARKER = ("how it works", "try it")  # sections that state behaviour whatever else is true of the chapter
HASH_LEN = 12
ALL_CATALOGUE = "*"


def catalogue_files(root, name):
    """The catalogue files (names without .md) chapter `name` follows: [] for a chapter synced by `src=` only, None for an exempt or unmapped one."""
    v = CATALOGUE.get(name)
    if v == ALL_CATALOGUE:
        d = os.path.join(root, "docs", "qa", "catalogue")
        return sorted(f[:-3] for f in os.listdir(d) if f.endswith(".md") and f != "README.md")
    return v if isinstance(v, list) else None


def slug(title):
    """The anchor GitHub gives a heading: lower case, only letters, digits, spaces, hyphens and underscores, spaces to hyphens."""
    t = re.sub(r"[*`]", "", title).strip().lower()
    return re.sub(r"[^\w\- ]", "", t).replace(" ", "-")


def split_sections(raw):
    """[{id, title, level, line, text}]: the part of a page from each heading to the next one (a heading inside a comment or a fence is no heading).
    Ids are the heading anchors, with -1, -2 for repeats, as GitHub makes them."""
    lines = raw.split("\n")
    out, seen = [], {}
    heads = []
    for i, l in enumerate(blank_hidden(raw).split("\n")):
        m = HEADING_RE.match(l)
        if m:
            heads.append((i, len(m.group(1)), (m.group(2) or "").strip()))
    for k, (i, level, title) in enumerate(heads):
        end = heads[k + 1][0] if k + 1 < len(heads) else len(lines)
        base = slug(title) or "section"
        n = seen.get(base, 0)
        seen[base] = n + 1
        out.append({"id": base if n == 0 else "%s-%d" % (base, n), "title": title, "level": level, "line": i + 1, "text": "\n".join(lines[i:end])})
    return out


def parse_marker(body):
    """({src: [..], spec: [..], qa: [..]}, None) or (None, why) for the text after `sync:` in a marker."""
    fields = {}
    for tok in body.split():
        k, eq, v = tok.partition("=")
        if not eq or k not in MARKER_FIELDS or not v:
            return None, "`%s` is not src=, spec= or qa= with a value" % tok
        if k in fields:
            return None, "`%s=` twice" % k
        items = v.split(",")
        if "" in items:
            return None, "`%s=` has an empty item" % k
        fields[k] = items
    if not fields:
        return None, "no src=, spec= or qa="
    return fields, None


def qa_rows(texts):
    """{QA ID: text of its Function, How and Expected cells} from catalogue file texts. The Sim, Unit, Spec and Status cells are left out: they
    change whenever a test passes, not when the behaviour does."""
    rows = {}
    for text in texts:
        for line in text.splitlines():
            m = ROW_RE.match(line)
            if m:
                cells = [c.strip() for c in line.strip().strip("|").split("|")]
                rows[m.group(1)] = " | ".join(" ".join(c.split()) for c in cells[1:4])
    return rows


def catalogue_texts(root):
    d = os.path.join(root, "docs", "qa", "catalogue")
    return [read(os.path.join(d, f)) for f in sorted(os.listdir(d)) if f.endswith(".md")] if os.path.isdir(d) else []


def git_out(root, *args):
    """Standard output of a git command in `root`, or None when it fails or there is no git."""
    try:
        return subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


# The lookahead sits right after the colon and takes the blanks into its own pattern: with the blanks outside it, the engine backtracks them to nothing and the lookahead then sees " hand -->",
# so a `hand` note would open a generated block that runs to the next closing marker (tools/manual_tables.py: OPEN_RE / CLOSE_RE, where `hand` has no closing marker).
GENERATED_RE = re.compile(rb"^(<!--[ \t]*manual-tables:(?![ \t]*hand[ \t]*-->)[^\n]*?-->[ \t]*\n).*?^(<!--[ \t]*/manual-tables[ \t]*-->)", re.S | re.M)
MANUAL_DIR = "docs/manual/"


def is_manual_page(rel):
    return rel.startswith(MANUAL_DIR) and rel.endswith(".md") and "/" not in rel[len(MANUAL_DIR):]


def digest(rel, data):
    """The shortened git blob hash of a file's bytes. A page of the manual is hashed without the text between its `manual-tables` markers: that text
    is written by tools/manual_tables.py from the firmware's key labels, so a renamed key must not make every section that cites the page look changed."""
    if is_manual_page(rel):
        data = GENERATED_RE.sub(rb"\1\2", data)
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()[:HASH_LEN]


def path_hash(root, rel):
    """The git blob hash of a file (a manual page: see digest; a folder: a hash of its tracked files' names and hashes), shortened; `missing` when there is none."""
    full = os.path.join(root, rel)
    if os.path.isdir(full):
        listed = git_out(root, "ls-files", "-z", "--", rel)
        files = sorted(f for f in (listed or "").split("\0") if f)
        if not files:
            files = sorted(os.path.relpath(os.path.join(dp, f), root) for dp, dn, fn in os.walk(full) for f in fn if "__pycache__" not in dp)
        h = hashlib.sha1()
        for f in files:
            h.update(("%s\0%s\n" % (f, path_hash(root, f))).encode())
        return h.hexdigest()[:HASH_LEN]
    try:
        with open(full, "rb") as f:
            data = f.read()
    except OSError:
        return "missing"
    return digest(rel, data)


def row_hash(rows, qa_id):
    return hashlib.sha1(rows[qa_id].encode()).hexdigest()[:HASH_LEN] if qa_id in rows else "missing"


def sync_state(root):
    """({chapter#section: entry}, [problems]) for every section that carries a `sync:` marker. entry = {chapter, id, line, items: {item: hash}}."""
    entries, problems = {}, []
    rows = qa_rows(catalogue_texts(root))
    for name in chapters(root):
        raw = read(os.path.join(root, "docs", "manual", name))
        for m in OLD_SRC_RE.finditer(mask_fences(raw)):
            problems.append("%s:%d: old `<!-- src: ... -->` comment: say it in a `<!-- sync: src=<path>,<path> spec=<path> qa=<ID> -->` marker of its section" % (name, raw.count("\n", 0, m.start()) + 1))
        first = split_sections(raw)
        head = "\n".join(raw.split("\n")[:first[0]["line"] - 1]) if first else raw
        for m in SYNC2_RE.finditer(FENCE_RE.sub("", head)):
            problems.append("%s: a `sync:` marker before the first heading belongs to no section" % name)
        for sec in first:
            where = "%s:%d (%s)" % (name, sec["line"], sec["title"])
            marks = list(SYNC2_RE.finditer(FENCE_RE.sub("", sec["text"])))
            if not marks:
                if sec["title"].lower() in NEEDS_MARKER:
                    problems.append("%s: a \"%s\" section states behaviour and needs a `<!-- sync: src=... -->` marker" % (where, sec["title"]))
                continue
            if len(marks) > 1:
                problems.append("%s: %d `sync:` markers, one per section" % (where, len(marks)))
                continue
            fields, why = parse_marker(marks[0].group(1))
            if fields is None:
                problems.append("%s: `sync:` marker: %s" % (where, why))
                continue
            items = {}
            for kind in ("src", "spec"):
                for p in fields.get(kind, []):
                    if not re.fullmatch(r"[\w./+-]+", p) or p.startswith("/") or ".." in p.split("/"):
                        problems.append("%s: `sync:` %s=%s is not a path relative to the repository" % (where, kind, p))
                    elif not os.path.exists(os.path.join(root, p)):
                        problems.append("%s: `sync:` %s=%s does not exist (moved or deleted? point the marker at its new place)" % (where, kind, p))
                    else:
                        items[p] = path_hash(root, p)
            for q in fields.get("qa", []):
                if q not in rows:
                    problems.append("%s: `sync:` qa=%s is in no catalogue file (removed? the section must follow)" % (where, q))
                else:
                    items[q] = row_hash(rows, q)
            entries["%s#%s" % (name, sec["id"])] = {"chapter": name, "id": sec["id"], "line": sec["line"], "title": sec["title"], "items": items}
    return entries, problems


class LockConflict(Exception):
    pass


CONFLICT_RE = re.compile(r"^(?:<<<<<<<|=======|>>>>>>>)", re.M)
CONFLICT_FIX = ("take either side (`git checkout --ours %s` or `--theirs`), then, on the merged tree, run `python3 tools/manual_check.py --ack-path <file>` for each file "
                "both sides acknowledged (or `--ack-all` to write every line again), and commit it" % LOCK)


def read_lock(root):
    """{key: {item: hash}} from docs/manual/sync.lock, or None when the file is missing. Raises LockConflict when a merge left conflict markers in it."""
    path = os.path.join(root, LOCK)
    if not os.path.exists(path):
        return None
    text = read(path)
    if CONFLICT_RE.search(text):
        raise LockConflict("%s has merge conflict markers: %s" % (LOCK, CONFLICT_FIX))
    lock = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split()
        items = {}
        for p in parts[1:]:
            k, _, v = p.rpartition("=")
            items[k] = v
        lock[parts[0]] = items
    return lock


def lock_text(lock):
    head = ["# docs/manual/sync.lock: written by tools/manual_check.py --ack, --ack-path and --ack-all, never by hand (OS-MANUAL2 M2-3).",
            "# One line per manual section that has a `<!-- sync: ... -->` marker: <chapter>#<section> then <item>=<hash> for every item the marker cites,",
            "# as it was when the section was last acknowledged: a path (git blob hash; a manual page without its generated tables; a folder: a hash of",
            "# its tracked files) or a QA ID (a hash of the row's Function, How and Expected cells).",
            "# After a merge conflict here: take either side, then re-run --ack-path <file> (or --ack-all) on the merged tree."]
    return "\n".join(head + ["%s %s" % (k, " ".join("%s=%s" % (i, lock[k][i]) for i in sorted(lock[k]))) for k in sorted(lock)]) + "\n"


def write_lock(root, lock):
    with open(os.path.join(root, LOCK), "w", encoding="utf-8", newline="\n") as f:
        f.write(lock_text(lock))


def lock_problems(entries, lock):
    out = []
    if lock is None:
        return ["%s is missing: run tools/manual_check.py --ack-all (first migration), or --ack <chapter>#<section> for each marked section" % LOCK] if entries else []
    for key in sorted(entries):
        if key not in lock:
            out.append("%s has no line in %s: check the section against what it cites, then run tools/manual_check.py --ack %s" % (key, LOCK, key))
    for key in sorted(lock):
        if key not in entries:
            out.append("%s has a line in %s but no marked section: run tools/manual_check.py --ack %s to drop it" % (key, LOCK, key))
    return out


def moved_items(entry, locked):
    """[(item, what)] that differ between a section's marker as it is now and its line of the lock: changed, added to the marker or gone from it."""
    out = []
    for item, h in sorted(entry["items"].items()):
        if item not in locked:
            out.append((item, "new in the marker"))
        elif locked[item] != h:
            out.append((item, "changed"))
    for item in sorted(set(locked) - set(entry["items"])):
        out.append((item, "dropped from the marker"))
    return out


def cites(cited, item):
    """True when the marker item `cited` is `item`, or a path inside the folder `item`, or the folder that holds the path `item`."""
    if cited == item:
        return True
    if QA_RE.fullmatch(cited) or QA_RE.fullmatch(item):
        return False
    a, b = cited.rstrip("/"), item.rstrip("/")
    return a.startswith(b + "/") or b.startswith(a + "/")


def do_ack(root, keys, everything):
    entries, problems = sync_state(root)
    if everything:
        if problems:
            print("manual_check: --ack-all needs every marker right first:")
            for p in problems:
                print("  " + p)
            return 1
        before = {}
        if os.path.exists(os.path.join(root, LOCK)):
            try:
                before = read_lock(root) or {}
            except LockConflict:
                before = {}  # a lock with conflict markers is the case this is for: it is written again whole
        lock = {k: dict(e["items"]) for k, e in entries.items()}
        write_lock(root, lock)
        print("manual_check: wrote %s: %d section(s) acknowledged, %d line(s) new or different (nothing was looked at: after a merge of the lock, "
              "prefer --ack-path <file> for the files you know changed)" % (LOCK, len(lock), sum(1 for k in lock if before.get(k) != lock[k])))
        return 0
    try:
        lock = read_lock(root) or {}
    except LockConflict as e:
        print("manual_check: " + str(e))
        return 1
    for key in keys:
        if key in entries:
            lock[key] = dict(entries[key]["items"])
            print("manual_check: acknowledged %s (%d item(s))" % (key, len(lock[key])))
        elif key in lock:
            del lock[key]
            print("manual_check: dropped %s from %s (the section or its marker is gone)" % (key, LOCK))
        else:
            chapter = key.split("#")[0]
            known = [k.split("#")[1] for k in sorted(entries) if k.startswith(chapter + "#")]
            print("manual_check: %s is no section with a valid `sync:` marker%s" % (key, (" (this chapter's: %s)" % ", ".join(known)) if known else ""))
            for p in problems:
                if p.startswith(chapter):
                    print("  " + p)
            return 1
    write_lock(root, lock)
    return 0


def do_ack_path(root, items):
    """Acknowledge, in every marked section that cites each of `items` (a path, a folder or a QA ID), that item only: its hash in the section's lock
    line becomes the current one. The section's other items keep what was acknowledged, so they still show as moved. A section with no line yet is
    left for --ack, which takes the whole section."""
    entries, _ = sync_state(root)
    try:
        lock = read_lock(root)
    except LockConflict as e:
        print("manual_check: " + str(e))
        return 1
    if lock is None:
        print("manual_check: %s is missing: run tools/manual_check.py --ack-all (first migration)" % LOCK)
        return 1
    bad = 0
    for item in items:
        hits = [k for k in sorted(entries) if any(cites(i, item) for i in entries[k]["items"])]
        if not hits:
            print("manual_check: no marked section cites %s (a path of the repository or a QA ID, as a marker writes it)" % item)
            bad += 1
            continue
        done, skipped = [], []
        for k in hits:
            if k not in lock:
                skipped.append(k)
                continue
            for i, h in entries[k]["items"].items():
                if cites(i, item):
                    lock[k][i] = h
            done.append(k)
        print("manual_check: acknowledged %s in %d section(s)%s" % (item, len(done), ": " + ", ".join(done) if 0 < len(done) <= 4 else ""))
        for k in skipped:
            print("  %s has no line in %s yet: check the whole section, then run tools/manual_check.py --ack %s" % (k, LOCK, k))
            bad += 1
    if bad:
        print("manual_check: nothing written (fix the above, then run it again)")
        return 1
    write_lock(root, lock)
    return 0


# ---- The branch gate ----------------------------------------------------------------------------------------------------------------

def gate_base(root, override):
    """(base commit, why not, fatal) for the gate: the merge base of HEAD with dev, or with the branch or commit named by --base / O3OS_MANUAL_BASE.
    No base gives the reason, and `fatal` is true when the caller named one that is not a commit (a wrong name must fail the gate, not skip it);
    otherwise the gate only warns."""
    branch = (git_out(root, "rev-parse", "--abbrev-ref", "HEAD") or "").strip()
    if branch == "dev":
        return None, "HEAD is dev", False
    named = override or os.environ.get("O3OS_MANUAL_BASE")
    if named:
        full = (git_out(root, "rev-parse", "--verify", "--quiet", named + "^{commit}") or "").strip()
        if not full:
            return None, "O3OS_MANUAL_BASE / --base %s is not a commit (a pull request needs the full history: checkout with fetch-depth: 0)" % named, True
        return ((git_out(root, "merge-base", "HEAD", full) or "").strip() or full), None, False
    for ref in ("dev", "origin/dev"):
        mb = (git_out(root, "merge-base", "HEAD", ref) or "").strip()
        if mb:
            return mb, None, False
    return None, "no merge base with dev", False


def show_at(root, rev, rel):
    return git_out(root, "show", "%s:%s" % (rev, rel))


def blob_at(root, rev, rel):
    """The bytes of a file at a commit, or None."""
    try:
        return subprocess.run(["git", "-C", root, "show", "%s:%s" % (rev, rel)], capture_output=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def norm_section(text):
    return "\n".join(l.rstrip() for l in SYNC2_RE.sub("", text).strip().split("\n"))


def changed_on_branch(root, base, item, changed):
    """True when the cited path differs from its state at `base`. A manual page is compared without its generated tables (a renamed key label changes
    the page's file, not what the cheat sheet cites of it); any other path by the list of files the branch changed."""
    if is_manual_page(item) and item in changed:
        old = blob_at(root, base, item)
        return old is None or digest(item, old) != path_hash(root, item)
    return item in changed or any(c.startswith(item.rstrip("/") + "/") for c in changed)


def gate_command(items):
    return "python3 tools/manual_check.py --ack-path " + " ".join(items)


def gate(root, override):
    """0 when every section that cites something changed on this branch has changed too (text or lock line); 1 on a branch with one that has not, or
    with a base that was named and is wrong. On dev, or with no merge base, it only warns."""
    base, why, fatal = gate_base(root, override)
    entries, _ = sync_state(root)
    try:
        lock = read_lock(root) or {}
    except LockConflict as e:
        print("manual gate: FAIL: " + str(e))
        return 1
    stale = [(k, moved_items(e, lock[k])) for k, e in sorted(entries.items()) if k in lock and moved_items(e, lock[k])]
    if fatal:
        print("manual gate: FAIL: " + why)
        return 1
    if base is None:
        print("manual gate: warning only (%s)" % why)
        for k, moved in stale:
            print("  WARN %s: %s have moved since the section was acknowledged" % (k, ", ".join("%s (%s)" % m for m in moved[:4]) + (" ..." if len(moved) > 4 else "")))
        print("manual gate: %d section(s) with a marker, %d with sources that moved since their acknowledgement" % (len(entries), len(stale)))
        return 0
    changed = set((git_out(root, "diff", "--name-only", base) or "").split("\n"))
    changed |= set((git_out(root, "ls-files", "--others", "--exclude-standard") or "").split("\n"))
    changed.discard("")
    rows_now = qa_rows(catalogue_texts(root))
    base_texts = []
    for f in (git_out(root, "ls-tree", "-r", "--name-only", base, "docs/qa/catalogue") or "").split("\n"):
        if f.endswith(".md"):
            base_texts.append(show_at(root, base, f) or "")
    rows_base = qa_rows(base_texts)
    base_lock = None
    text = show_at(root, base, LOCK)
    if text is not None:
        base_lock = {}
        for line in text.splitlines():
            if line.strip() and not line.startswith("#"):
                base_lock[line.split()[0]] = line.strip()
    fails = []  # [(section key, [items it cites that changed])]
    chapter_at_base = {}
    for key, e in sorted(entries.items()):
        hit = []
        for item in sorted(e["items"]):
            if QA_RE.fullmatch(item):
                if rows_now.get(item) != rows_base.get(item):
                    hit.append(item)
            elif changed_on_branch(root, base, item, changed):
                hit.append(item)
        if not hit:
            continue
        name = e["chapter"]
        if name not in chapter_at_base:
            old = show_at(root, base, "docs/manual/" + name)
            chapter_at_base[name] = {s["id"]: norm_section(s["text"]) for s in split_sections(old)} if old is not None else {}
        now = next(s for s in split_sections(read(os.path.join(root, "docs", "manual", name))) if s["id"] == e["id"])
        text_changed = chapter_at_base[name].get(e["id"]) != norm_section(now["text"])
        line_now = ("%s %s" % (key, " ".join("%s=%s" % (i, lock[key][i]) for i in sorted(lock[key])))) if key in lock else None
        lock_changed = base_lock is None or base_lock.get(key) != line_now
        if not text_changed and not lock_changed:
            fails.append((key, hit))
    if fails:
        print("manual gate: %d section(s) cite something this branch changed (since %s) and neither the section nor its line of %s changed:" % (len(fails), base[:10], LOCK))
        for key, hit in fails:
            print("  FAIL %s cites %s" % (key, ", ".join(hit[:3]) + (" and %d more" % (len(hit) - 3) if len(hit) > 3 else "")))
        items = sorted({i for _, hit in fails for i in hit})
        print("Read each section against the change and fix the text that is now wrong. If a section still holds, acknowledge what it cites. One command does "
              "every section above at once:")
        print("  " + gate_command(items))
        print("(`--ack <chapter>#<section>` acknowledges a single section, whatever it cites.) Then commit %s." % LOCK)
    print("manual gate: %d section(s) with a marker, %d not followed (base %s)" % (len(entries), len(fails), base[:10]))
    return 1 if fails else 0


def ids_text(ids, verbose, room=12):
    return " ".join(ids) if verbose or len(ids) <= room else " ".join(ids[:room]) + " ... (+%d more)" % (len(ids) - room)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--stale", action="store_true", help="list QA rows added, changed or removed since each chapter's manual-sync sha, and the sections whose sources moved")
    ap.add_argument("--strict", action="store_true", help="exit 1 when there is a problem")
    ap.add_argument("--ack", nargs="+", metavar="CHAPTER#SECTION", help="write the section's line of docs/manual/sync.lock from what its marker cites now (drops the line of a section that is gone)")
    ap.add_argument("--ack-path", nargs="+", metavar="PATH_OR_QA_ID", help="acknowledge a cited file, folder or QA row in every section that cites it (one command after "
                    "a change of a file that many sections cite, such as app/src/bindings.cpp); the section's other items keep what was acknowledged")
    ap.add_argument("--ack-all", action="store_true", help="write every line of sync.lock from the tree as it is, without looking at what moved: the first migration, or "
                    "the lock after a merge conflict in it (otherwise use --ack-path)")
    ap.add_argument("--gate", action="store_true", help="the branch gate: exit 1 when a section cites a file that changed on this branch and neither the section nor its lock line did; on dev it only warns")
    ap.add_argument("--base", help="the branch or commit the gate compares with, by its merge base with HEAD (default: dev; also $O3OS_MANUAL_BASE, which a pull request sets to "
                    "origin/<target branch>); a name that is not a commit fails the gate")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every TODO text, every uncited ID and every colour word")
    args = ap.parse_args(argv)
    root = args.root

    if args.ack_path:
        return do_ack_path(root, args.ack_path)
    if args.ack or args.ack_all:
        return do_ack(root, args.ack or [], args.ack_all)
    if args.gate:
        return gate(root, args.base)

    index_text = read(os.path.join(root, "docs", "manual", "index.md"))
    manifest = manifest_names(root)
    names = chapters(root)
    problems = 0
    todo_total = 0
    results = {n: check_chapter(root, n, manifest, index_text) for n in names}

    print("manual_check: %d chapters, %d images in the manifest" % (len(names), len(manifest)))
    for n in names:
        r = results[n]
        line = []
        if r["broken"]:
            line.append("broken links: " + ", ".join(r["broken"]))
        if not r["in_index"]:
            line.append("not mentioned in index.md")
        if r["no_manifest"]:
            line.append("image with no manifest line: " + ", ".join(r["no_manifest"]))
        if r["no_file"]:
            line.append("image file missing (tools/manual_images.py, then commit docs/manual/img/): " + ", ".join(r["no_file"]))
        problems += len(r["broken"]) + (0 if r["in_index"] else 1) + len(r["no_manifest"]) + len(r["no_file"])
        todo_total += len(r["todos"])
        if r["todos"]:
            line.append("%d open TODO/CONFLICT" % len(r["todos"]))
        if r["a11y"]:
            line.append("%d accessibility problem(s)" % len(r["a11y"]))
            problems += len(r["a11y"])
        if line:
            print("  %s: %s" % (n, "; ".join(line)))
            if args.verbose:
                for t in r["todos"]:
                    print("      " + t[:160])
        for no, msg in r["a11y"]:
            print("      %s:%d: %s" % (n, no, msg))
        if args.verbose and r["colour"]:
            print("  %s: %d line(s) name a colour (review that the meaning is also in words):" % (n, len(r["colour"])))
            for no, t in r["colour"]:
                print("      %d: %s" % (no, t))
    for n in sorted(NOT_CHAPTERS):  # the index and the template are pages of the manual too
        path = os.path.join(root, "docs", "manual", n)
        faults = a11y_problems(read(path)) if os.path.exists(path) else []
        if faults:
            print("  %s: %d accessibility problem(s)" % (n, len(faults)))
            problems += len(faults)
            for no, msg in faults:
                print("      %s:%d: %s" % (n, no, msg))

    entries, sync_problems = sync_state(root)
    try:
        lock = read_lock(root)
    except LockConflict as e:
        lock = None
        sync_problems.append(str(e))
    else:
        sync_problems += lock_problems(entries, lock)
    if sync_problems:
        print("sync markers and %s:" % LOCK)
        for p in sync_problems:
            print("  " + p)
        problems += len(sync_problems)

    if args.stale:
        print("stale (QA rows added, changed or removed since each chapter's manual-sync):")
        stale_total = 0
        for n in names:
            r = results[n]
            cats = catalogue_files(root, n)
            if cats is None:
                continue
            if not r["sync"]:
                print("  %s: no `<!-- manual-sync: <sha> -->` marker" % n)
                problems += 1
                continue
            for cat in cats:
                ch = qa_changes(root, r["sync"], cat)
                if ch is None:
                    print("  %s: sync sha %s is not a known commit" % (n, r["sync"]))
                    problems += 1
                elif ch:
                    every = [i for i, _ in ch]
                    added, changed, removed = ([i for i, k in ch if k == kind] for kind in ("added", "changed", "removed"))
                    stale_total += len(every)
                    new = [i for i in added + changed if i not in r["cites"]]
                    print("  %s (since %s, catalogue %s): %d: %s" % (n, r["sync"][:10], cat, len(every), ids_text(every, args.verbose)))
                    print("      %d added, %d changed, %d removed" % (len(added), len(changed), len(removed)))
                    if new:
                        print("      not cited by the chapter yet: " + ids_text(new, args.verbose))
                    gone = [i for i in removed if i in r["cites"]]
                    if gone:
                        print("      cited by the chapter, removed from the catalogue: " + ids_text(gone, args.verbose))
        problems += stale_total
        if stale_total == 0:
            print("  none")
        outside = [n for n in names if n not in CATALOGUE]
        if outside:
            print("  chapters outside the CATALOGUE table (information; map them or exempt them with a reason): " + ", ".join(outside))
        exempt = [n for n in names if isinstance(CATALOGUE.get(n), str) and CATALOGUE[n] != ALL_CATALOGUE]
        if exempt:
            print("  exempt (information): " + "; ".join("%s: %s" % (n, CATALOGUE[n]) for n in exempt))
        print("sections whose sources moved since their acknowledgement (information; `tools/manual_check.py --gate` fails only for a change made on the branch):")
        moved_total = 0
        for key in sorted(entries):
            if lock is not None and key in lock:
                moved = moved_items(entries[key], lock[key])
                if moved:
                    moved_total += 1
                    print("  %s: %s" % (key, ", ".join("%s (%s)" % m for m in moved[:5]) + (" ... (+%d more)" % (len(moved) - 5) if len(moved) > 5 and not args.verbose else "")))
        if moved_total == 0:
            print("  none")
        cited = set()
        for r in results.values():
            cited |= r["cites"]
        cat_dir = os.path.join(root, "docs", "qa", "catalogue")
        mapped = set()
        for n in names:
            mapped |= set(catalogue_files(root, n) or [])
        print("catalogue IDs no chapter cites (information):")
        for f in sorted(os.listdir(cat_dir)):
            if not f.endswith(".md"):
                continue
            cat = f[:-3]
            unc = [i for i in catalogue_ids(root, cat) if i not in cited]
            if unc:
                tail = " ".join(unc) if args.verbose or len(unc) <= 12 else " ".join(unc[:12]) + " ..."
                print("  %s (%s): %d: %s" % (cat, "chapter exists" if cat in mapped else "no chapter yet", len(unc), tail))

    print("summary: %d problem(s), %d open TODO/CONFLICT comment(s)" % (problems, todo_total))
    return 1 if (args.strict and problems) else 0


if __name__ == "__main__":
    sys.exit(main())
