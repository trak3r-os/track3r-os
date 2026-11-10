#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The QA catalogue gate (OS-E2E slice E2E-3): every row of docs/qa/catalogue/*.md names what proves it, and the name is real.

    python3 -I tools/qa_cover.py --check        the ctest `qa_cover`: exit 1 with one line per finding
    python3 -I tools/qa_cover.py --write        regenerate the index table of docs/qa/catalogue/README.md
    python3 -I tools/qa_cover.py --self-test    the ctest `qa_cover_selftest`: plants each fault and expects the finding

A row is `| ID | Function | How | Expected | Sim | Unit | Spec | Status |` (docs/specs/OS-FINALQA.md section 1). What the gate reads:

  Sim cell     `qa/qa_x.txt` > `label`, `label`   the file is under sim/scenarios/ (the `.txt`, and `qa/`, may be left out: a bare scenario
                                                   name resolves) and every label is a `snap <label>` of it
               `gallery/name`                     a picture of the gallery: sim/goldens.txt has gallery/name_<theme>
               `label`, `name_test`               with no file: a snap of any scenario, a scenario name or a host test
  Unit cell    `x.json` > `label`                 platform/mk66/tools/hil/x.json exists; with a label, a `say`, `note`, `shot`, `snap` or `name` of
                                                   its steps has the label as a whole token (case and `_` / `-` do not matter; `T1` is not in
                                                   `T16`). A label written like an ID (`qa_pat_256`) is among the IDs a step puts first, ranges
                                                   and lists expanded (`QA-INS-092..095`, `QA-PAT-255 / 257`); a header that merely names the
                                                   rows does not count. A bare `x.json` needs only the file.
  Status cell  host PASS (`name_test`, ...)       the host reference, free text: the names in the host clause
  Any cell     `host: `x_test``, `x_test`, `x.py` a host test named in a Sim or Unit cell, remarks included (`none (`x_test.cpp`)`)
  A host name is a file of tests/, platform/mk66/tests/ or tools/, a ctest name, or a TEST_CASE title (or its start): a word that only
  occurs in the test sources is not one. Any other text in parentheses is a remark and is not read. `-` and `none` in a cell mean no
  reference there, but a backtick reference after them (outside parentheses) is still checked.

The rules:
  1. A reference that does not resolve is a finding, on every row (a renamed snap or step, a missing JSON).
  2. A row that is not dropped needs at least one reference that resolves, or `manual: <reason>` in its Unit cell (the Sim and Status
     cells are read too). The legacy forms (Unit cell `by hand`, `manual (not scripted)`) are accepted only on a row unchanged since
     the base.
  3. A row changed or added since the base is held to rule 2 without the legacy forms. The base is $O3OS_QA_BASE, else
     `git merge-base HEAD dev` (then origin/dev); with none, a warning says so and this rule is skipped.
  4. The rows have eight cells and IDs are unique.
  5. The index table of docs/qa/catalogue/README.md, between <!-- qa-cover --> and <!-- /qa-cover -->, is what --write makes: functions
     are the rows that are not dropped; Sim PASS and Unit PASS count the words `sim PASS` and `unit PASS` of the Status cell; open
     is a row with neither. Never edit it by hand (on a merge conflict, run --write).

Stdlib only; reads the files, never builds anything. About half a second on the real catalogue.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

CAT = "docs/qa/catalogue"
SCEN = "sim/scenarios"
HIL = "platform/mk66/tools/hil"
GOLDENS = "sim/goldens.txt"
HOST_DIRS = ("tests", "platform/mk66/tests", "platform/mk66/tools", "tools")  # where a host test's file may live
TEST_DIRS = ("tests", "platform/mk66/tests")                                   # where TEST_CASE titles are read
THEMES = ("amiga", "black", "white")
BEGIN = "<!-- qa-cover -->"
END = "<!-- /qa-cover -->"
COLS = 8  # ID, Function, How, Expected, Sim, Unit, Spec, Status
ROW_RE = re.compile(r"^\|\s*(QA-[A-Z0-9]+-\d+)\s*\|")
CELL_RE = re.compile(r"(?<!\\)\|")
ARROW = "\u203a"  # between a file and its labels: `qa/qa_x.txt` > `label`
TOKEN_RE = re.compile(r"`([^`]*)`|;")
MANUAL_RE = re.compile(r"\bmanual:\s*\S")
LEGACY_RE = re.compile(r"^(by hand|manual)\b(?!:)", re.I)
STEP_TEXT_KEYS = ("say", "note", "shot", "snap", "name")  # the strings of a HIL step that say what it is: a reference's label or the row's ID is among them
SKIP_DIRS = {".git", ".toolchains", ".claude", "third_party", "node_modules"}


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def norm(s):
    """The comparison form of a label or an ID: `qa_fil_050` and `QA-FIL-050` are one."""
    return s.lower().replace("_", "-")


ID_LIST_RE = re.compile(r"qa-([a-z0-9]+)-(\d+)((?:\s*(?:\.\.|/|,|\band\b)\s*\d+)*)")
ID_SHAPE_RE = re.compile(r"qa-[a-z0-9]+-\d+")


def expand(scr, first, rest):
    """The IDs of one match of ID_LIST_RE: `QA-INS-092..095`, `QA-PAT-255 / 257 / 256`, `QA-MOD-095, 096`."""
    out = {"qa-%s-%s" % (scr, first)}
    width, prev = len(first), int(first)
    for sep, num in re.findall(r"(\.\.|/|,|\band\b)\s*(\d+)", rest):
        if len(num) != width:
            break  # `QA-INS-092, 2 tries`: a number that is not written like an ID ends the list
        n = int(num)
        for k in range(prev + 1, n + 1) if sep == ".." and 0 < n - prev < 200 else [n]:
            out.add("qa-%s-%0*d" % (scr, width, k))
        prev = n
    return out


def ids_in(text):
    """The row IDs a text names anywhere, ranges and lists expanded."""
    out = set()
    for scr, first, rest in ID_LIST_RE.findall(norm(text)):
        out |= expand(scr, first, rest)
    return out


def heading_ids(text):
    """The row IDs a text puts first: at its start or after a sentence (`QA-PAT-255 / 257 / 256: ...`, `... the lists. QA-FIL-092: File F7`),
    ranges and lists expanded (`QA-FIL-037..040 BY HAND`, `QA-FIL-051, QA-FIL-056 BY HAND`). An ID in the middle of a sentence (`rows
    QA-FIL-032..044 of the catalogue`, the header of a file that names all its rows) is not one: that step is not about it."""
    t, out, prev_end = norm(text), set(), None
    for m in ID_LIST_RE.finditer(t):
        before = t[:m.start()].rstrip()
        listed = prev_end is not None and re.fullmatch(r"\s*,\s*", t[prev_end:m.start()]) is not None
        if not before or before[-1] in ".:;" or listed:
            out |= expand(*m.groups())
            prev_end = m.end()
        else:
            prev_end = None
    return out


class Row:
    __slots__ = ("path", "no", "id", "cells", "text")

    def __init__(self, path, no, text):
        self.path, self.no, self.text = path, no, text.rstrip()
        self.id = ROW_RE.match(text).group(1)
        self.cells = [c.strip() for c in CELL_RE.split(text.strip().strip("|"))]

    @property
    def sim(self):
        return self.cells[4]

    @property
    def unit(self):
        return self.cells[5]

    @property
    def status(self):
        return self.cells[7]

    @property
    def dropped(self):
        return self.status.lower().startswith("dropped")

    def where(self):
        return "%s:%d: %s" % (self.path, self.no, self.id)


def load_rows(root):
    """-> (rows, findings): the rows of every catalogue file (README.md is the index), in file order."""
    rows, findings, seen = [], [], {}
    d = os.path.join(root, CAT)
    for name in sorted(os.listdir(d)):
        if not name.endswith(".md") or name == "README.md":
            continue
        rel = "%s/%s" % (CAT, name)
        for no, line in enumerate(read(os.path.join(root, rel)).splitlines(), 1):
            if not ROW_RE.match(line):
                continue
            row = Row(rel, no, line)
            if len(row.cells) != COLS:
                findings.append("%s: %d cells, not %d (ID, Function, How, Expected, Sim, Unit, Spec, Status; no `|` inside a cell)"
                                % (row.where(), len(row.cells), COLS))
                continue
            if row.id in seen:
                findings.append("%s: ID already used at %s (IDs are never reused)" % (row.where(), seen[row.id]))
                continue
            seen[row.id] = "%s:%d" % (rel, no)
            rows.append(row)
    return rows, findings


class Env:
    """What a reference can point at, read lazily from the checkout."""

    def __init__(self, root):
        self.root = root
        self._snaps = self._gallery = self._hil = self._host = self._ctest = self._sources = None
        self._heads = {}

    def path(self, *p):
        return os.path.join(self.root, *p)

    def walk(self, *dirs):
        for d in dirs:
            for dp, dn, fn in os.walk(self.path(d)):
                dn[:] = sorted(x for x in dn if x not in SKIP_DIRS and not x.startswith("build"))
                for f in sorted(fn):
                    yield os.path.join(dp, f)

    @property
    def snaps(self):
        """scenario path under sim/scenarios without `.txt` ('qa/qa_song') -> set of its `snap` names."""
        if self._snaps is None:
            self._snaps = {}
            base = self.path(SCEN)
            for p in self.walk(SCEN):
                if p.endswith(".txt"):
                    key = os.path.relpath(p, base)[:-4].replace(os.sep, "/")
                    self._snaps[key] = set(re.findall(r"^\s*snap\s+(\S+)", read(p), re.M))
        return self._snaps

    def scenario(self, name):
        """The key of snaps for a file or scenario name as a row writes it (`qa/qa_song.txt`, `qa_song`, `pattern_edit`), or None."""
        name = name.strip()
        if name.startswith("sim/scenarios/"):
            name = name[len("sim/scenarios/"):]
        if name.endswith(".txt"):
            name = name[:-4]
        for key in (name, "qa/" + name):
            if key in self.snaps:
                return key
        return None

    @property
    def gallery(self):
        if self._gallery is None:
            self._gallery = set()
            p = self.path(GOLDENS)
            if os.path.exists(p):
                for line in read(p).splitlines():
                    if line.startswith("gallery/"):
                        self._gallery.add(line.split()[0][len("gallery/"):])
        return self._gallery

    def gallery_has(self, name):
        return name in self.gallery or any(name + "_" + t in self.gallery for t in THEMES)

    @property
    def hil(self):
        """json file name -> the normalised text of every string of its steps that says what the step is (STEP_TEXT_KEYS)."""
        if self._hil is None:
            self._hil = {}
            d = self.path(HIL)
            for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
                if f.endswith(".json"):
                    texts = []
                    try:
                        collect_notes(json.loads(read(os.path.join(d, f))), texts)
                    except ValueError:
                        pass
                    self._hil[f] = [norm(t) for t in texts]
        return self._hil

    def step_names(self, jsonfile, label):
        """A step of the JSON says the label. A label that is an ID or a shot named like one (`qa_pat_256`) is among the IDs a step puts first,
        lists and ranges included (heading_ids: a sentence that only mentions the ID does not count). Any other label is a whole token of one string
        of a step (`T1` is not in `T16`)."""
        lab = norm(label)
        if ID_SHAPE_RE.fullmatch(lab):
            if jsonfile not in self._heads:
                self._heads[jsonfile] = set().union(*map(heading_ids, self.hil[jsonfile]))
            return lab in self._heads[jsonfile]
        return any(re.search(r"(?<![a-z0-9])%s(?![a-z0-9])" % re.escape(lab), t) for t in self.hil[jsonfile])

    @property
    def host_files(self):
        if self._host is None:
            self._host = {}
            for p in self.walk(*HOST_DIRS):
                self._host.setdefault(os.path.basename(p), p)
        return self._host

    @property
    def ctest_names(self):
        """add_test(NAME x), o3m_add_test(x ...) = o3m_x_test, and the scenario_<name> of every scenario."""
        if self._ctest is None:
            names = {"scenario_" + k.rsplit("/", 1)[-1] for k in self.snaps}
            for p in self._cmake_files():
                t = read(p)
                names.update(re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_]+)", t))
                # a shard loop: add_test(NAME x_${k}of6) stands for x_1of6 .. x_6of6
                for pre, n in re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_]+)\$\{\w+\}of(\d+)\b", t):
                    names.update("%s%dof%s" % (pre, i, n) for i in range(1, int(n) + 1))
                names.update("o3m_%s_test" % n for n in re.findall(r"o3m_add_test\(\s*([A-Za-z0-9_]+)", t))
            self._ctest = names
        return self._ctest

    def _cmake_files(self):
        for dp, dn, fn in os.walk(self.root):
            dn[:] = sorted(x for x in dn if x not in SKIP_DIRS and not x.startswith("build"))
            for f in sorted(fn):
                if f == "CMakeLists.txt" or f.endswith(".cmake"):
                    yield os.path.join(dp, f)

    @property
    def sources(self):
        """The text of every host test source (C++ and Python), joined. A TEST_CASE title is looked up in it (str.find: it is 8 MB)
        only when no file or ctest name matched."""
        if self._sources is None:
            self._sources = "\n".join(read(p) for p in self.walk(*TEST_DIRS, "tools") if p.endswith((".cpp", ".py")))
        return self._sources

    def host_has(self, tok):
        """A host reference: `names_test`, `names_test.cpp`, `o3os_tests: audio tap (H2a)`, `o3disk.py --self-test`."""
        tok = re.sub(r"^[a-z0-9_]+:\s+", "", tok.strip())
        first = tok.split()[0] if tok.split() else ""
        stem = first.rsplit(".", 1)[0] if first.endswith((".cpp", ".py", ".json")) else first
        if first in self.host_files or stem + ".cpp" in self.host_files or stem + ".py" in self.host_files:
            return True
        if stem in self.ctest_names or tok in self.ctest_names:
            return True
        if any(self.sources.find('TEST_CASE("%s%s' % (tok, end)) >= 0 for end in ('"', ":", " ")):
            return True
        return False


def collect_notes(node, out):
    if isinstance(node, dict):
        for k, v in node.items():
            if k in STEP_TEXT_KEYS and isinstance(v, str):
                out.append(v)
            else:
                collect_notes(v, out)
    elif isinstance(node, list):
        for v in node:
            collect_notes(v, out)


def strip_parens(s):
    """s without its (...) groups, which are remarks; a parenthesis inside backticks is text."""
    out, depth, tick = [], 0, False
    for ch in s:
        if ch == "`":
            tick = not tick
        if not tick and ch == "(":
            depth += 1
        elif not tick and ch == ")" and depth:
            depth -= 1
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def is_none(cell):
    """`-` or `none`, with at most a remark: a reference in backticks after it (outside parentheses) is still read."""
    return bool(re.match(r"(-|none)(\s|$)", cell.strip())) and "`" not in strip_parens(cell)


HOST_INLINE_RE = re.compile(r"\bhost(?: tests?)?:?\s+((?:`[^`]*`(?:\s*(?:,|\band\b)\s*)?)+)")


TESTLIKE_RE = re.compile(r"[A-Za-z0-9_]+(_test(\.cpp)?|\.cpp|\.py)")


def inline_host_refs(cell, env):
    """-> [(text, ok, why)]: the host tests a Sim or Unit cell names outside its main form, remarks included: what follows the word
    `host` (`- (host: `x_test`, `y_test`)`) and any name that reads as a test file (`none (`metro_app_test.cpp`)`, `y_test`)."""
    toks = [t for g in HOST_INLINE_RE.findall(cell) for t in re.findall(r"`([^`]*)`", g)]
    toks += re.findall(r"`([^`]*)`", cell)
    refs, seen = [], set()
    for tok in (t.strip() for t in toks):
        after_host = any(tok in re.findall(r"`([^`]*)`", g) for g in HOST_INLINE_RE.findall(cell))
        if tok not in seen and ((after_host and re.fullmatch(r"[A-Za-z0-9_.]+( --[a-z-]+)?", tok)) or TESTLIKE_RE.fullmatch(tok)):
            seen.add(tok)
            refs.append((tok, env.host_has(tok), "no host test, ctest or test word of that name"))
    return refs


def sim_refs(row, env):
    """-> [(text, ok, why)] for the Sim cell."""
    cell = row.sim
    refs = inline_host_refs(cell, env)
    if is_none(cell):
        return refs
    cur = None
    for m in TOKEN_RE.finditer(strip_parens(cell)):
        if m.group(0) == ";":
            cur = None
            continue
        tok = m.group(1).strip()
        head, *labels = [p.strip() for p in tok.split(ARROW)]  # the arrow between a file and its labels
        if head.startswith("gallery/"):
            refs.append((tok, env.gallery_has(head[len("gallery/"):]), "no gallery picture in %s" % GOLDENS))
            cur = None
            continue
        if labels or head.endswith(".txt"):
            key = env.scenario(head)
            refs.append((head, key is not None, "no scenario %s/%s" % (SCEN, head)))
            cur = key
            for lab in labels:
                if key is not None and lab:
                    refs.append((lab, lab in env.snaps[key], "no `snap %s` in %s/%s.txt" % (lab, SCEN, key)))
            continue
        if cur is not None:
            refs.append((head, head in env.snaps[cur], "no `snap %s` in %s/%s.txt" % (head, SCEN, cur)))
            continue
        ok = env.scenario(head) is not None or any(head in s for s in env.snaps.values()) or env.host_has(head)
        refs.append((head, ok, "no scenario, snap or host test of that name"))
    return refs


def unit_refs(row, env):
    """-> [(text, ok, why)] for the Unit cell: each JSON exists, and each label after it is a step of it. A JSON with no label is the file
    alone. A host test named in the cell (after `host`, or a name that reads as a test file) is not a label: inline_host_refs reads it."""
    cell = row.unit
    refs = inline_host_refs(cell, env)
    if is_none(cell):
        return refs
    groups, cur = [], None  # [json, labels] per JSON of the cell; `;` ends a group
    for m in TOKEN_RE.finditer(HOST_INLINE_RE.sub("", strip_parens(cell))):
        if m.group(0) == ";":
            cur = None
            continue
        head, *labels = [p.strip() for p in m.group(1).strip().split(ARROW)]
        if head.endswith(".json"):
            cur = [head, [lab for lab in labels if lab]]
            groups.append(cur)
        elif cur is not None and not TESTLIKE_RE.fullmatch(head):
            cur[1].append(head)
    for head, labels in groups:
        have = head in env.hil
        refs.append((head, have, "no %s/%s" % (HIL, head)))
        for lab in labels if have else []:
            refs.append((lab, env.step_names(head, lab), "no step of %s says `%s`" % (head, lab)))
    return refs


def host_refs(row, env):
    """-> [(text, ok, why)]: the names in the host clause of the Status cell (from `host` to the next ` . `, `unit` or `sim`)."""
    m = re.search(r"\bhost\b.*", row.status)
    if not m:
        return []
    clause = re.split(r" · |, unit|, sim|; unit|; sim", m.group(0))[0]
    refs = []
    for tok in re.findall(r"`([^`]*)`", clause):
        tok = tok.strip()
        # only what reads as a test name: one word, or a name with an option (`o3disk.py --self-test`), or `o3os_tests: <title>`
        if re.fullmatch(r"[A-Za-z0-9_.]+( --[a-z-]+)?|(o3os_tests|unit_rest): .+", tok) and not re.fullmatch(r"[A-Z]\d+", tok):
            refs.append((tok, env.host_has(tok), "no host test, ctest or test word of that name"))
    return refs


def has_manual(row):
    return any(MANUAL_RE.search(c) for c in (row.sim, row.unit, row.status))


def check(root, base=None):
    """-> the findings. base: None (no changed-row rule) or {id: text of the row at the base}."""
    rows, findings = load_rows(root)
    env = Env(root)
    for row in rows:
        if row.dropped:
            continue
        refs = []
        for kind, fn in (("Sim", sim_refs), ("Unit", unit_refs), ("Status", host_refs)):
            for text, ok, why in fn(row, env):
                refs.append(ok)
                if not ok:
                    findings.append("%s: %s `%s`: %s" % (row.where(), kind, text, why))
        # a reference that dangles is already a finding; the coverage rule asks whether any resolves
        covered = any(refs)
        changed = base is not None and base.get(row.id) != row.text
        if covered or has_manual(row):
            continue
        if refs:
            continue  # only dangling references: reported above
        if LEGACY_RE.match(row.unit) or LEGACY_RE.match(row.sim):
            if changed:
                findings.append("%s: changed since the base and still the legacy manual form: write `manual: <reason>` in the Unit cell"
                                % row.where())
            continue
        findings.append("%s: no reference: name a Sim snap, a HIL step or a host test, or write `manual: <reason>` in the Unit cell%s"
                        % (row.where(), " (changed since the base)" if changed else ""))
    findings.extend(index_findings(root, rows))
    return list(dict.fromkeys(findings))


def index_table(rows):
    """The index table of the README: one line per catalogue file, from the rows."""
    per = {}
    for row in rows:
        per.setdefault(os.path.basename(row.path)[:-3], []).append(row)
    lines = ["| Screen | File | Functions | Sim PASS | Unit PASS | Open |", "|---|---|---|---|---|---|"]
    tot = [0, 0, 0, 0]
    for name in sorted(per):
        live = [r for r in per[name] if not r.dropped]
        sim = sum(1 for r in live if "sim PASS" in r.status)
        unit = sum(1 for r in live if "unit PASS" in r.status)
        opened = sum(1 for r in live if "sim PASS" not in r.status and "unit PASS" not in r.status)
        for i, v in enumerate((len(live), sim, unit, opened)):
            tot[i] += v
        lines.append("| %s | [%s.md](%s.md) | %d | %d | %d | %d |" % (name.replace("_", " ").capitalize(), name, name, len(live), sim, unit, opened))
    lines.append("| **Total** | | **%d** | **%d** | **%d** | **%d** |" % tuple(tot))
    return "\n".join(lines)


def readme_split(text):
    """-> (before, table, after) around the markers, or None."""
    i, j = text.find(BEGIN), text.find(END)
    if i < 0 or j < i:
        return None
    return text[:i + len(BEGIN)], text[i + len(BEGIN):j], text[j:]


def index_findings(root, rows):
    path = os.path.join(root, CAT, "README.md")
    parts = readme_split(read(path)) if os.path.exists(path) else None
    if parts is None:
        return ["%s/README.md: no %s ... %s markers around the index table" % (CAT, BEGIN, END)]
    if parts[1].strip() != index_table(rows):
        return ["%s/README.md: the index table is stale: run `python3 -I tools/qa_cover.py --write`" % CAT]
    return []


def write_index(root):
    rows, _ = load_rows(root)
    path = os.path.join(root, CAT, "README.md")
    text = read(path)
    parts = readme_split(text)
    if parts is None:
        raise SystemExit("qa_cover: %s has no %s ... %s markers" % (path, BEGIN, END))
    new = parts[0] + "\n\n" + index_table(rows) + "\n\n" + parts[2]
    if new != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)
    return new != text


def git(root, *args):
    try:
        out = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout


def base_rows(root):
    """-> (base sha or None, {id: row text at the base}); the base is $O3OS_QA_BASE, else the merge base with dev."""
    sha = os.environ.get("O3OS_QA_BASE")
    if sha is None:
        for branch in ("dev", "origin/dev"):
            sha = (git(root, "merge-base", "HEAD", branch) or "").strip()
            if sha:
                break
    if not sha or git(root, "rev-parse", "--verify", "--quiet", sha + "^{commit}") is None:
        return None, None
    rows = {}
    for name in (git(root, "ls-tree", "--name-only", sha, CAT + "/") or "").split():
        if name.endswith(".md") and not name.endswith("README.md"):
            for line in (git(root, "show", "%s:%s" % (sha, name)) or "").splitlines():
                if ROW_RE.match(line):
                    rows[ROW_RE.match(line).group(1)] = line.rstrip()
    return sha, rows


def run_check(root):
    sha, base = base_rows(root)
    if base is None:
        print("qa_cover: warning: no base (O3OS_QA_BASE, or `git merge-base HEAD dev`): the changed-row rule is skipped", file=sys.stderr)
    findings = check(root, base)
    for f in findings:
        print(f)
    print("qa_cover: %d finding(s)%s" % (len(findings), "" if sha is None else ", base " + sha[:10]))
    return 1 if findings else 0


# ---- the self-test: a tiny checkout, one planted fault at a time -------------------------------------------------------------------

ROW = "| QA-ABC-001 | f | h | e | %s | %s | OS-X | %s |"
GOOD = ROW % ("`qa/qa_abc.txt` › `qa_abc_001`", "`qa_abc.json` › `qa_abc_001`", "sim PASS · host PASS (`abc_test`) · unit pending")


def fixture(root, rows, snap="qa_abc_001", step="QA-ABC-001: press F1", steps=None):
    def put(rel, text):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
    put(CAT + "/abc.md", "# Abc\n\n| ID | Function | How | Expected | Sim | Unit | Spec | Status |\n|---|---|---|---|---|---|---|---|\n"
        + "\n".join(rows) + "\n")
    put(SCEN + "/qa/qa_abc.txt", "tap song\nsnap %s\n" % snap)
    put(HIL + "/qa_abc.json", json.dumps({"steps": steps if steps is not None else [{"say": step}]}))
    put("tests/abc_test.cpp", 'TEST_CASE("abc") { /* a reference snapshot */ }\n')  # words that are in the sources but are not test names
    put(GOLDENS, "gallery/abc_amiga 00\n")
    rs, _ = load_rows(root)
    put(CAT + "/README.md", "# Index\n\n" + BEGIN + "\n\n" + index_table(rs) + "\n" + END + "\n")


def self_test():
    ok = True

    def expect(name, rows, want, base=None, **kw):
        nonlocal ok
        with tempfile.TemporaryDirectory() as d:
            fixture(d, rows, **kw)
            got = check(d, base)
        hit = [g for g in got if want in g] if want else got
        good = bool(hit) if want else not got
        print("%-34s %s%s" % (name, "ok" if good else "FAIL", "" if good else "  (wanted %r, got %r)" % (want, got)))
        ok = ok and good

    expect("a good row passes", [GOOD], None)
    expect("a dangling sim label", [GOOD], "no `snap", snap="qa_abc_002")
    expect("a missing sim file", [GOOD.replace("qa/qa_abc.txt", "qa/qa_none.txt")], "no scenario")
    expect("a missing HIL JSON", [GOOD.replace("qa_abc.json", "qa_none.json")], "qa_none.json")
    expect("a HIL step that lost the label", [GOOD], "no step of", step="something else")
    expect("a dangling host test", [GOOD.replace("`abc_test`", "`gone_test`")], "gone_test")
    expect("a gallery picture", [ROW % ("`gallery/abc`", "-", "sim PASS")], None)
    expect("a missing gallery picture", [ROW % ("`gallery/nope`", "-", "sim PASS")], "no gallery picture")
    expect("a row with no reference", [ROW % ("-", "-", "unit pending")], "no reference")
    expect("manual: with a reason", [ROW % ("-", "manual: a person holds the keys", "unit pending")], None)
    expect("a bare manual: is not a reason", [ROW % ("-", "manual:", "unit pending")], "no reference")
    expect("a legacy form, row unchanged", [ROW % ("-", "by hand", "unit pending")], None, base={"QA-ABC-001": ROW % ("-", "by hand", "unit pending")})
    expect("a legacy form, row changed", [ROW % ("-", "by hand", "unit PASS")], "legacy manual form",
           base={"QA-ABC-001": ROW % ("-", "by hand", "unit pending")})
    expect("a new row with no reference", [GOOD, (ROW % ("-", "-", "unit pending")).replace("001", "002")], "QA-ABC-002: no reference: name a Sim snap, a HIL step or a host test, or write `manual: <reason>` in the Unit cell (changed",
           base={"QA-ABC-001": GOOD})
    expect("a changed row with a reference", [GOOD + " "], None, base={"QA-ABC-001": ROW % ("-", "-", "unit pending")})
    expect("a dropped row needs nothing", [ROW % ("-", "-", "dropped")], None)
    expect("a duplicate ID", [GOOD, GOOD], "already used")
    expect("a row with seven cells", [GOOD.replace(" OS-X |", "")], "7 cells")
    # a label is required when it is given: the row's ID in some other step is no proof, and `T1` is not `T16`
    unit = lambda cell: ROW % ("-", cell, "unit pending")
    expect("a label that no step says", [unit("`qa_abc.json` \u203a `other_label`")], "no step of qa_abc.json says `other_label`")
    expect("a label is a whole token", [unit("`qa_abc.json` \u203a `T1`")], "says `T1`", steps=[{"say": "QA-ABC-001: T16 pressed"}])
    expect("a label that is a whole token", [unit("`qa_abc.json` \u203a `T16`")], None, steps=[{"say": "T16 pressed"}])
    expect("a label that is a shot", [unit("`qa_abc.json` \u203a `qa_abc_001_end`")], None, steps=[{"shot": "qa_abc_001_end"}])
    # ranges and lists: an ID label is among the IDs a step puts first; a header that names the rows is not a step about each
    two = [GOOD.replace("001", "002").replace("`qa_abc_001`", "`qa_abc_002`")]
    expect("an ID label in a range", two, None, snap="qa_abc_002", steps=[{"say": "QA-ABC-001..003: the fields"}])
    expect("an ID label in a list", two, None, snap="qa_abc_002", steps=[{"say": "QA-ABC-003 / 001 / 002: the fields"}])
    expect("an ID label in a later sentence", two, None, snap="qa_abc_002", steps=[{"say": "Both of them. QA-ABC-002: the second"}])
    expect("an ID label in a header", two, "no step of qa_abc.json says `qa_abc_002`", snap="qa_abc_002", steps=[{"say": "Rows QA-ABC-001..003 of the catalogue"}])
    expect("an ID label past the range", two, "no step of", snap="qa_abc_002", steps=[{"say": "QA-ABC-001..001: one"}])
    expect("a host test after the JSON is no label", [unit("`qa_abc.json` \u203a `qa_abc_001`, `abc_test.cpp`")], None)
    expect("a bare JSON needs only the file", [unit("`qa_abc.json`")], None, steps=[])
    # the Sim and Unit cells: a reference after `-` or `none`, a host test after `host:`, a name that only occurs in the sources
    expect("none, then a dangling snap", [ROW % ("none `qa/qa_abc.txt` \u203a `qa_abc_999`", "-", "unit pending")], "no `snap qa_abc_999`")
    expect("- then a good snap", [ROW % ("- `qa/qa_abc.txt` \u203a `qa_abc_001`", "-", "unit pending")], None)
    expect("none (a remark)", [ROW % ("none (`qa_abc_999` is not run here)", "manual: a person", "unit pending")], None)
    expect("an inline host test", [ROW % ("- (host: `abc_test`)", "-", "unit pending")], None)
    expect("a dangling inline host test", [ROW % ("- (host: `gone_test`)", "-", "unit pending")], "gone_test")
    expect("a host test in a remark", [ROW % ("none (`gone_test.cpp`)", "-", "unit pending")], "gone_test.cpp")
    expect("a word of the sources is no test", [GOOD.replace("`abc_test`", "`reference`")], "`reference`")
    expect("a ctest name is a test", [GOOD.replace("`abc_test`", "`scenario_qa_abc`")], None)
    expect("a TEST_CASE title is a test", [GOOD.replace("`abc_test`", "`o3os_tests: abc`")], None)
    # the cells: an escaped pipe is text, not a cell border
    expect("an escaped pipe", [GOOD.replace("| f |", "| a \\| b |")], None)
    ok = ok and ids_in("QA-INS-092..095") == {"qa-ins-09%d" % n for n in range(2, 6)} \
        and ids_in("QA-INS-092, 2 tries") == {"qa-ins-092"} and ids_in("QA-PAT-255 / 257 / 256 and 260") == {"qa-pat-%d" % n for n in (255, 256, 257, 260)} \
        and heading_ids("The rows QA-X-001..003 follow. QA-X-007, QA-X-009: both") == {"qa-x-007", "qa-x-009"}
    print("%-34s %s" % ("ranges, lists and 2 tries", "ok" if ok else "FAIL"))
    # the README table
    with tempfile.TemporaryDirectory() as d:
        fixture(d, [GOOD])
        p = os.path.join(d, CAT, "README.md")
        good = check(d) == []
        text = read(p).replace("| 1 | 1 | 0 | 0 |", "| 9 | 1 | 0 | 0 |")
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)
        stale = [g for g in check(d) if "stale" in g]
        changed = write_index(d)
        fixed = check(d) == []
        again = write_index(d)
        print("%-34s %s" % ("a stale README table", "ok" if good and stale and changed and fixed and not again else "FAIL"))
        ok = ok and good and bool(stale) and changed and fixed and not again
    # the base from git, in a scratch repository: no GIT_* variable of the caller (a hook, a worktree) may steer these commands
    with tempfile.TemporaryDirectory() as d:
        fixture(d, [GOOD])
        saved = {k: os.environ.pop(k) for k in [k for k in os.environ if k.startswith("GIT_") or k == "O3OS_QA_BASE"]}
        os.environ.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        steps = (["init", "-q", "-b", "dev"], ["add", "."], ["-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", "commit", "-qm", "x"])
        try:
            for a in steps:
                subprocess.run(["git", "-C", d] + a, check=True, capture_output=True)
            sha, rows = base_rows(d)
            good = sha is not None and rows == {"QA-ABC-001": GOOD}
        except (OSError, subprocess.CalledProcessError):
            good = True  # no git here: nothing to prove
        finally:
            for k in [k for k in os.environ if k.startswith("GIT_")]:
                del os.environ[k]
            os.environ.update(saved)
        print("%-34s %s" % ("the base rows from git", "ok" if good else "FAIL"))
        ok = ok and good
    print("qa_cover self-test: %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.write:
        print("qa_cover: README index %s" % ("rewritten" if write_index(args.root) else "already current"))
        return 0
    return run_check(args.root)


if __name__ == "__main__":
    sys.exit(main())
