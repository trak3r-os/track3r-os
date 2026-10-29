#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the manual's key tables from the firmware's own tables (OS-MANUAL2 M2-2).

The key labels of a screen, and of the row it draws while Shift is held, are declared in the firmware: the `k<Screen>SoftKeys` and
`k<Screen>ShiftKeys` arrays of app/include/o3os/app/screens/*.h (and app/include/o3os/app/mixer_view.h). A host test
(`shift_labels_test`) ties every Shift label to the binding it runs, so these arrays are what the unit binds and draws. The screen buttons
are the `nav.screen` rows of Global in app/src/bindings.cpp. This tool writes tables from them between markers in docs/manual/*.md, so a
renamed key cannot leave a stale table behind. Everything else in a chapter (what a key does, the pads, the encoder) stays hand-written,
outside the markers.

A generated block is a pair of HTML comments; the text between them is written by this tool and nothing else:

    <!-- manual-tables: keys screen=<name> -->     the screen's key row (F1 to F8) and its Shift row, in a chapter
    <!-- manual-tables: cheat shift [screens=a,b] -->    the Shift rows of several screens side by side (cheat_sheet.md)
    <!-- manual-tables: cheat screens [screens=a,b] -->  the F1 to F8 labels, one screen a row
    <!-- manual-tables: cheat buttons -->           the screen buttons and the screen each one opens
    <!-- /manual-tables -->

`<!-- manual-tables: hand -->` is a note for a reader: the cell or table next to it is written by hand because the sources lack the fact
(a latent issue to record); the tool ignores it.

  tools/manual_tables.py              write every block (there is no --write: a run without --check writes)
  tools/manual_tables.py --check      exit 1 when a block differs from the sources, a marker is malformed, or a screen's chapter has no block
  tools/manual_tables.py --self-test  plant each fault in a scratch copy of the sources and see it fail

Every `k...Keys[` declaration of app/include/o3os/app/screens/*.h and mixer_view.h is read: `[static] [inline] [constexpr] const char* [const] kXxx[8] = {...};`
with eight string literals. One the tool cannot read fails with its file and line named: fix the firmware table, not this tool. A new key-row table
in the firmware that SCREENS below does not list fails too, so a screen cannot gain a row the manual never shows.

Python 3 standard library only; read-only on the sources.
"""

import argparse
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MANUAL = "docs/manual"
BINDINGS = "app/src/bindings.cpp"
BUTTONS = "app/include/o3os/app/buttons.h"
LABEL_DIR = "app/include/o3os/app/screens"  # every header of it is read for key-row tables
LABEL_EXTRA = ["app/include/o3os/app/mixer_view.h"]  # the Mixer's rows live next to its view, outside that folder

# The screens the manual documents. For each: the screen's name in a marker, its title and chapter, the ScreenId and the printed name of the
# button that opens it (checked against the nav.screen rows), and the firmware arrays its key row can show: `keys` for F1 to F8 and `shift` for
# Shift + F1 to F8, each (column title, array). The first of a list is the screen as it opens; the others are the states that swap the row (a
# Wave view, the page of a loop review, the File page). A screen with several lists of one kind gets a column for each; with one, a single
# column. The titles are the manual's words for a state, not facts from the code.
SCREENS = {
    "pattern": dict(title="Pattern", chapter="pattern.md", sid="Pattern", button="Pattern",
                    keys=[("Pattern", "kPatternSoftKeys")], shift=[("Pattern", "kPatternShiftKeys")]),
    "song": dict(title="Song", chapter="song.md", sid="Song", button="Song",
                 keys=[("Song", "kSongSoftKeys")], shift=[("Song", "kSongShiftKeys")]),
    "instrument": dict(title="Instrument", chapter="instrument.md", sid="Instrument", button="Params",
                       keys=[("Instrument", "kParamsSoftKeys")], shift=[("Instrument", "kParamsShiftKeys")]),
    "wave": dict(title="Wave and Chop", chapter="wave.md", sid="Wave", button="SamplePlay",
                 keys=[("WAVE", "kWaveSoftKeys"), ("CHOP", "kChopSoftKeys"), ("WAVETABLE", "kWtSoftKeys"), ("GRANULAR", "kGranSoftKeys")],
                 shift=[("All views", "kWaveShiftKeys")]),
    "sample_edit": dict(title="Sample edit", chapter="sample_edit.md", sid="SampleEdit", button="SampleEdit",
                        keys=[("Sample edit", "kSampleEditSoftKeys"), ("FX page", "kSampleEditFxSoftKeys")], shift=[("Sample edit", "kSampleEditShiftKeys")]),
    "sample_load": dict(title="Sample load", chapter="sample_load.md", sid="SampleLoad", button="SampleLoad",
                        keys=[("Sample load", "kSampleLoadSoftKeys")], shift=[("Sample load", "kSampleLoadShiftKeys")]),
    "record": dict(title="Record", chapter="record.md", sid="Record", button="SampleRec",
                   keys=[("Record", "kRecordSoftKeys"), ("Loop review", "kRecordLoopKeys"), ("Loop review, muted", "kRecordLoopMutedKeys")],
                   shift=[("SOURCES and RESAMPLE", "kRecordShiftKeys"), ("RADIO", "kRecordRadioShiftKeys")]),
    "mixer": dict(title="Mixer", chapter="mixer.md", sid="Mixer", button="Master",
                  keys=[("Mixer", "kMixerSoftKeys")], shift=[("Mixer", "kMixerShiftKeys")]),
    "perform": dict(title="Perform", chapter="perform.md", sid="Perform", button="Performance",
                    keys=[("Perform", "kPerformSoftKeys")], shift=[("Perform", "kPerformShiftKeys")]),
    "file": dict(title="File", chapter="file.md", sid="File", button="File",
                 keys=[("PROJECTS", "kFileSoftKeys"), ("EXPORT", "kFileExportSoftKeys")],
                 shift=[("PROJECTS", "kFileShiftKeys"), ("IMPORT and EXPORT", "kFileOtherShiftKeys")]),
    "config": dict(title="Config", chapter="config.md", sid="Config", button="Config",
                   keys=[("Config", "kConfigSoftKeys")], shift=[("Config", "kConfigShiftKeys")]),
    # No screen button: the Navigator opens it. It binds no Shift key (the default row, all dashes).
    "modules": dict(title="Modules", chapter="modules.md", sid=None, button=None,
                    keys=[("Modules", "kModulesSoftKeys")], shift=[("Modules", "kNoShiftKeys")]),
}

# Key-row tables of the firmware that the manual does not show: the input test (a diagnostic), a running app's page (its labels are the app's),
# the default rows that screen.h hands to every screen with nothing of its own, and the INFO page's row of Config (OS-FWCONFIG: kConfigSoftKeys with F8 UPDATE, which config.md says in words).
# kPerformDj*: the DJ page of Perform (OS-DJ A1, A2) is a hand-written section of perform.md ("Use the DJ page"); its two key rows are described there.
UNDOCUMENTED = {"kInputTestSoftKeys", "kModuleAppSoftKeys", "kJumpShiftKeys", "kConfigInfoSoftKeys", "kPerformDjSoftKeys", "kPerformDjShiftKeys", "kSampleEditBusySoftKeys", "kSampleEditFxBusySoftKeys", "kMixerStemKeys"}  # (the last three: the Sample edit rows while a job runs, F8 CANCEL, and the Mixer row on the STEMS page, F6 APPLY (OS-USBOUTS); the chapters say so in words)

# What the front panel prints on a screen button, by Button enum name (the drawing docs/manual/img/panel.svg and the manual say it this way).
BUTTON_PRINTED = {"Params": "Params", "Performance": "Perform", "File": "File", "Config": "Config", "Master": "Master",
                  "SampleLoad": "Sample Loader", "SamplePlay": "Sample Playback", "SampleEdit": "Sample Editor",
                  "SampleRec": "Sample Recorder", "Song": "Song", "Pattern": "Pattern"}

# `cheat shift` and `cheat screens` without `screens=`: the screens the cheat sheet has always shown.
CHEAT_DEFAULT = ["pattern", "instrument", "sample_load", "mixer", "file"]

SKIP_FILES = {"index.md", "chapter-template.md"}  # the template describes the markers; it carries none of its own


class TableError(Exception):
    pass


def read(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        raise TableError("cannot read %s: %s" % (rel, e.strerror))


# ---- The firmware's tables ----------------------------------------------------------------------------------------------------------

# A key-row declaration. DECL_RE is the form the tool reads; ANY_DECL_RE finds every declaration of a `k...Keys` array (any `char` pointer type that
# names one), so a spelling DECL_RE does not know is an error and not a table that silently goes missing.
DECL_RE = re.compile(r"^[ \t]*(?:(?:static|inline|constexpr)[ \t]+)*const[ \t]+char[ \t]*\*[ \t]*(?:const[ \t]+)?(k\w*Keys)[ \t]*\[", re.M)
ANY_DECL_RE = re.compile(r"\bchar\b[\s\w*&]*?\b(k\w*Keys)\s*\[")
BODY_RE = re.compile(r"\[\s*(\w*)\s*\]\s*=\s*\{(.*?)\}\s*;", re.S)
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
COMMENT_OR_STRING_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"|//[^\n]*|/\*.*?\*/', re.S)


def strip_comments(text):
    """The text with its comments blanked and every line break kept, so a line number of the result is a line of the file; string literals stay."""
    return COMMENT_OR_STRING_RE.sub(lambda m: m.group(0) if m.group(0)[0] == '"' else re.sub(r"[^\n]", " ", m.group(0)), text)


def label_files(root):
    """The headers that can declare a key row: every .h of LABEL_DIR, then LABEL_EXTRA."""
    d = os.path.join(root, LABEL_DIR)
    found = sorted("%s/%s" % (LABEL_DIR, n) for n in os.listdir(d) if n.endswith(".h")) if os.path.isdir(d) else []
    return found + [f for f in LABEL_EXTRA if f not in found]


def label_tables(root):
    """{array name: (labels, file, line)} for every `kXxx[8] = {...}` of the label headers. A declaration that is not eight string literals is an error."""
    out = {}
    for rel in label_files(root):
        text = strip_comments(read(root, rel))
        readable = {m.start(1) for m in DECL_RE.finditer(text)}
        for m in ANY_DECL_RE.finditer(text):
            if m.start(1) not in readable:
                line = text.count("\n", 0, m.start(1)) + 1
                raise TableError("%s:%d: %s is declared in a form the tool cannot read: write it as `inline constexpr const char* %s[8] = {...};` "
                                 "(`static` and `const char* const` also read)" % (rel, line, m.group(1), m.group(1)))
        for m in DECL_RE.finditer(text):
            name, line = m.group(1), text.count("\n", 0, m.start(1)) + 1
            where = "%s:%d: %s" % (rel, line, name)
            b = BODY_RE.match(text, m.end() - 1)
            if not b:
                raise TableError("%s: not a `[8] = {...};` initialiser the tool can read: fix the firmware table" % where)
            if b.group(1) != "8":
                raise TableError("%s: the size is `[%s]`, a key row has eight keys" % (where, b.group(1)))
            body = b.group(2)
            labels = [s for s in STR_RE.findall(body)]
            rest = STR_RE.sub("", body)
            if len(labels) != 8 or re.sub(r"[\s,]", "", rest):
                raise TableError("%s: %d string literals (and other tokens) where eight labels are expected" % (where, len(labels)))
            for s in labels:
                if "|" in s or "\\" in s or "*" in s or "`" in s:
                    raise TableError("%s: the label \"%s\" has a character Markdown would read" % (where, s))
            if name in out:
                raise TableError("%s: declared twice (also %s:%d)" % (where, out[name][1], out[name][2]))
            out[name] = (labels, rel, line)
    return out


def check_coverage(tables):
    """Every key row of the firmware is listed in SCREENS (or UNDOCUMENTED), and every name in SCREENS exists."""
    used = set(UNDOCUMENTED)
    for key, s in SCREENS.items():
        for title, arr in s["keys"] + s["shift"]:
            if arr not in tables:
                raise TableError("SCREENS[%s] lists %s, which no label header declares (renamed? edit SCREENS in tools/manual_tables.py)" % (key, arr))
            used.add(arr)
    for name, (_, rel, line) in sorted(tables.items()):
        if name not in used:
            raise TableError("%s:%d: %s is a key row the manual's tables do not cover: add it to SCREENS in tools/manual_tables.py "
                             "(or to UNDOCUMENTED with the reason)" % (rel, line, name))
    for name in UNDOCUMENTED:
        if name not in tables:
            raise TableError("UNDOCUMENTED lists %s, which the firmware no longer declares" % name)


def button_order(root):
    """Button enum names in the enum's order, from app/buttons.h."""
    text = re.sub(r"//[^\n]*", "", read(root, BUTTONS))
    m = re.search(r"enum class Button[^{]*\{(.*?)\};", text, re.S)
    if not m:
        raise TableError("%s: no `enum class Button`" % BUTTONS)
    codes, nxt = {}, 0
    for item in m.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        name, _, val = item.partition("=")
        nxt = int(val.strip(), 0) if val.strip() else nxt
        codes[name.strip()] = nxt
        nxt += 1
    return sorted(codes, key=lambda n: codes[n])


NAV_RE = re.compile(r"bind\(kGlobal, tap\(button_ref\(Button::(\w+)\)\), kNavScreen, ArgFrom::Fixed, screen_arg\(ScreenId::(\w+)\)\),")
NAV_LOOSE_RE = re.compile(r"^[^\n/]*\bbind\(kGlobal,[^\n]*\bkNavScreen\b", re.M)


def nav_screens(root):
    """{Button name: ScreenId name} from the `nav.screen` rows of Global in app/src/bindings.cpp: exactly one for each screen button of SCREENS."""
    text = read(root, BINDINGS)
    if 'kNavScreen = core::action_id("nav.screen")' not in text:
        raise TableError("%s: no `kNavScreen = core::action_id(\"nav.screen\")`" % BINDINGS)
    rows = NAV_RE.findall(text)
    loose = len(NAV_LOOSE_RE.findall(text))
    want = sum(1 for s in SCREENS.values() if s["button"])
    nav = {}
    for button, sid in rows:
        if button in nav:
            raise TableError("%s: Button::%s has two nav.screen rows of Global (ScreenId::%s and ScreenId::%s)" % (BINDINGS, button, nav[button], sid))
        nav[button] = sid
    if loose != len(rows):
        raise TableError("%s: %d nav.screen rows of Global, the tool reads %d: a row has another shape than "
                         "`bind(kGlobal, tap(button_ref(Button::X)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Y)),`" % (BINDINGS, loose, len(rows)))
    if len(rows) != want:
        raise TableError("%s: %d nav.screen rows of Global, expected %d (one for each screen button of SCREENS): a screen button added or removed? "
                         "edit SCREENS in tools/manual_tables.py" % (BINDINGS, len(rows), want))
    return nav


# ---- The tables ---------------------------------------------------------------------------------------------------------------------

def cell(label):
    if label in ("-", "--"):
        return "-"
    if label == "":
        return "(blank)"
    return "**%s**" % label


def row_cells(arr, tables):
    return [cell(l) for l in tables[arr][0]]


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return out


def keys_block(name, tables):
    s = SCREENS[name]
    plain, shift = s["keys"], s["shift"]
    header = ["Key"] + [t if len(plain) > 1 else "Label" for t, _ in plain] + ["`Shift`, " + t if len(shift) > 1 else "With `Shift`" for t, _ in shift]
    cols = [row_cells(a, tables) for _, a in plain] + [row_cells(a, tables) for _, a in shift]
    rows = [["`F%d`" % (i + 1)] + [c[i] for c in cols] for i in range(8)]
    return ["The key row, and the row drawn while `Shift` is held. `-` means the key does nothing.", ""] + md_table(header, rows)


def cheat_screens(args):
    names = args.get("screens")
    if names is None:
        return list(CHEAT_DEFAULT)
    names = [n for n in names.split(",") if n]
    for n in names:
        if n not in SCREENS:
            raise TableError("unknown screen `%s` (known: %s)" % (n, ", ".join(sorted(SCREENS))))
    return names


def cheat_block(section, args, tables, root):
    if section == "shift":
        names = cheat_screens(args)
        header = ["Key"] + [SCREENS[n]["title"] for n in names]
        cols = [row_cells(SCREENS[n]["shift"][0][1], tables) for n in names]
        rows = [["`Shift` + `F%d`" % (i + 1)] + [c[i] for c in cols] for i in range(8)]
        return md_table(header, rows)
    if section == "screens":
        names = cheat_screens(args)
        rows = []
        for n in names:
            s = SCREENS[n]
            if s["button"] is None:
                raise TableError("`cheat screens` lists %s, which has no screen button" % n)
            rows.append(["[%s](%s)" % (s["title"], s["chapter"]), "`%s`" % BUTTON_PRINTED[s["button"]],
                         ", ".join(row_cells(s["keys"][0][1], tables))])
        return md_table(["Screen", "Button", "F1 to F8"], rows)
    if section == "buttons":
        nav = nav_screens(root)
        by_sid = {s["sid"]: s for s in SCREENS.values() if s["sid"]}
        rows = []
        for b in button_order(root):
            if b not in nav:
                continue
            s = by_sid.get(nav[b])
            if s is None:
                raise TableError("%s: Button::%s opens ScreenId::%s, which SCREENS in tools/manual_tables.py does not list" % (BINDINGS, b, nav[b]))
            if b not in BUTTON_PRINTED or s["button"] != b:
                raise TableError("Button::%s opens %s, but SCREENS says its button is %s: edit SCREENS / BUTTON_PRINTED" % (b, nav[b], s["button"]))
            rows.append(["`%s`" % BUTTON_PRINTED[b], s["title"], "[%s](%s)" % (s["title"], s["chapter"])])
        if len(rows) != len(nav):
            raise TableError("%d nav.screen rows, %d table rows" % (len(nav), len(rows)))
        return md_table(["Screen button", "Opens", "Chapter"], rows)
    raise TableError("`cheat %s` is not a table (expected shift, screens or buttons)" % section)


def generate(spec, tables, root):
    """The lines of a block for the marker text `spec` (the words after `manual-tables:`)."""
    words = spec.split()
    args = {}
    plain = []
    for w in words:
        if "=" in w:
            k, _, v = w.partition("=")
            args[k] = v
        else:
            plain.append(w)
    if plain[:1] == ["keys"] and len(plain) == 1:
        name = args.get("screen")
        if name not in SCREENS:
            raise TableError("`keys screen=%s`: unknown screen (known: %s)" % (name, ", ".join(sorted(SCREENS))))
        extra = set(args) - {"screen"}
        if extra:
            raise TableError("`keys`: unknown argument %s" % ", ".join(sorted(extra)))
        return keys_block(name, tables)
    if plain[:1] == ["cheat"] and len(plain) == 2:
        extra = set(args) - {"screens"}
        if extra:
            raise TableError("`cheat`: unknown argument %s" % ", ".join(sorted(extra)))
        return cheat_block(plain[1], args, tables, root)
    raise TableError("unknown marker `manual-tables: %s` (expected `keys screen=<name>`, `cheat shift|screens|buttons` or `hand`)" % spec)


# ---- The chapters -------------------------------------------------------------------------------------------------------------------

OPEN_RE = re.compile(r"^<!--\s*manual-tables:\s*(.*?)\s*-->[ \t]*$", re.M)
CLOSE_RE = re.compile(r"^<!--\s*/manual-tables\s*-->[ \t]*$", re.M)


def manual_files(root):
    d = os.path.join(root, MANUAL)
    return sorted(n for n in os.listdir(d) if n.endswith(".md") and n not in SKIP_FILES)


def blocks(text, rel):
    """[(spec, start of the text after the opening line, start of the closing line, line of the opening marker)] of a file; a malformed pair is an error."""
    found = []
    pos = 0
    while True:
        m = OPEN_RE.search(text, pos)
        stray = CLOSE_RE.search(text, pos)
        if m is None:
            if stray:
                raise TableError("%s:%d: `<!-- /manual-tables -->` with no opening marker" % (rel, text.count("\n", 0, stray.start()) + 1))
            return found
        line = text.count("\n", 0, m.start()) + 1
        if stray and stray.start() < m.start():
            raise TableError("%s:%d: `<!-- /manual-tables -->` with no opening marker" % (rel, text.count("\n", 0, stray.start()) + 1))
        spec = m.group(1)
        if spec == "hand":
            pos = m.end()
            continue
        c = CLOSE_RE.search(text, m.end())
        nxt = OPEN_RE.search(text, m.end())
        if c is None or (nxt is not None and nxt.start() < c.start() and nxt.group(1) != "hand"):
            raise TableError("%s:%d: `<!-- manual-tables: %s -->` is never closed by `<!-- /manual-tables -->`" % (rel, line, spec))
        found.append((spec, m.end() + 1, c.start(), line))
        pos = c.end()


def render(lines):
    return "\n".join(lines) + "\n\n"


def process(root, write):
    """Check (or write) every block. Returns (problems, blocks seen, files changed)."""
    tables = label_tables(root)
    check_coverage(tables)
    problems, seen, changed = [], 0, 0
    has_keys = set()
    for name in manual_files(root):
        rel = "%s/%s" % (MANUAL, name)
        path = os.path.join(root, rel)
        text = read(root, rel)
        out, last = [], 0
        for spec, a, b, line in blocks(text, rel):
            seen += 1
            try:
                want = render(generate(spec, tables, root))
            except TableError as e:
                raise TableError("%s:%d: %s" % (rel, line, e))
            m = re.fullmatch(r"keys screen=(\w+)", spec)
            if m:
                has_keys.add((name, m.group(1)))
            if text[a:b] != want:
                problems.append("%s:%d: `manual-tables: %s` is out of date" % (rel, line, spec))
            out.append(text[last:a])
            out.append(want)
            last = b
        out.append(text[last:])
        new = "".join(out)
        if write and new != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
            changed += 1
    for key, s in sorted(SCREENS.items()):
        if (s["chapter"], key) not in has_keys:
            problems.append("%s/%s: no `<!-- manual-tables: keys screen=%s -->` block" % (MANUAL, s["chapter"], key))
    return problems, seen, changed


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--check", action="store_true", help="exit 1 when a block is out of date or malformed; write nothing")
    ap.add_argument("--self-test", action="store_true", help="plant each fault in a scratch copy and see it fail")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test(args.root)
    try:
        problems, seen, changed = process(args.root, write=not args.check)
    except TableError as e:
        print("manual_tables: %s" % e)
        return 2
    if args.check:
        for p in problems:
            print("manual_tables: " + p)
        print("manual_tables --check: %d block(s), %d problem(s)%s" % (seen, len(problems), "; run tools/manual_tables.py and commit" if problems else ""))
        return 1 if problems else 0
    for p in problems:
        if "out of date" not in p:
            print("manual_tables: " + p)
    print("manual_tables: %d block(s), %d file(s) written" % (seen, changed))
    return 1 if any("out of date" not in p for p in problems) else 0


# ---- The self-test ------------------------------------------------------------------------------------------------------------------

# The self-test runs on small stand-ins for the firmware sources, written into a scratch directory, not on copies of the real headers: a label
# renamed in the firmware must not turn it red. Labels the test asserts on are fixed here; every other row gets `<Array><n>` (kWaveSoftKeys -> WaveSoft1 ..).
FIXTURE_LABELS = {
    "kPerformSoftKeys": ["SCENE 1", "SCENE 2", "SCENE 3", "HALF", "DOUBLE", "LATCH", "NEXT PAT", "RESET"],
    "kPerformShiftKeys": ["STORE 1", "STORE 2", "STORE 3", "-", "-", "-", "-", "-"],
    "kMixerShiftKeys": ["NAME", "-", "-", "-", "-", "-", "UNMUTE", "UNSOLO"],
    "kRecordLoopMutedKeys": ["OVERDUB", "MONITOR", "SAVE", "DISCARD", "UNDO LYR", "SEEK+", "SAVE WAV", "--"],
    "kFileExportSoftKeys": ["EXPORT", "", "", "SAVE", "SAVE AS", "IMPORT", "PROJECTS", ""],
    "kNoShiftKeys": ["-"] * 8,
    "kJumpShiftKeys": ["0/4", "1/4", "2/4", "3/4", "END", "-", "-", "-"],
}
# Which fixture header declares an array (by name prefix); the rest go to screens/other_screen.h. Three spellings of a declaration are used, to
# prove they all read: plain, `static constexpr ... const`, and `static const char* const` with the body over lines and a comment in it.
FIXTURE_FILES = (("kMixer", "app/include/o3os/app/mixer_view.h"), ("kJump", LABEL_DIR + "/screen.h"), ("kNo", LABEL_DIR + "/screen.h"),
                 ("kPerform", LABEL_DIR + "/perform_screen.h"), ("kSong", LABEL_DIR + "/song_screen.h"), ("kRecord", LABEL_DIR + "/record_screen.h"),
                 ("kFile", LABEL_DIR + "/file_screen.h"))
# What the fixture headers also hold that the tool must leave alone: declarations in comments, and a use of an array.
FIXTURE_NOISE = ('// inline constexpr const char* kOldKeys[8] = {"gone"};\n/* static const char* kBlockKeys[8] = {"gone"}; */\n'
                 'inline const char* key_label(int i) { return kPerformSoftKeys[i]; }  // a use is no declaration\n')
FIXTURE_PERFORM_BUTTONS = ("F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "Shift", "Params", "Performance", "File", "Config", "Master", "SampleLoad",
                           "SamplePlay", "SampleEdit", "SampleRec", "Song", "Pattern", "Play")


def fixture_labels(arr):
    return FIXTURE_LABELS.get(arr) or ["%s%d" % (arr[1:-4], i + 1) for i in range(8)]


def fixture_decl(arr, style):
    q = ", ".join('"%s"' % l for l in fixture_labels(arr))
    if style == 1:
        return "static constexpr const char* const %s[8] = {%s};" % (arr, q)
    if style == 2:
        first, last = ", ".join('"%s"' % l for l in fixture_labels(arr)[:4]), ", ".join('"%s"' % l for l in fixture_labels(arr)[4:])
        return "static const char* const %s[8] = {\n    %s,  // the first four\n    %s};" % (arr, first, last)
    return "inline constexpr const char* %s[8] = {%s};" % (arr, q)


def write_fixture(put):
    """Write the stand-in sources through put(rel, text): the label headers, the Button enum and the Global nav.screen rows of the bindings."""
    files = {}
    arrays = [a for s in SCREENS.values() for _, a in s["keys"] + s["shift"]] + sorted(UNDOCUMENTED)
    for n, arr in enumerate(arrays):
        rel = next((f for pre, f in FIXTURE_FILES if arr.startswith(pre)), LABEL_DIR + "/other_screen.h")
        files.setdefault(rel, []).append(fixture_decl(arr, 0 if arr == "kPerformSoftKeys" else n % 3))
    for rel, decls in files.items():
        put(rel, "// fixture: stand-in for a firmware header\n#pragma once\n" + FIXTURE_NOISE + "\n".join(decls) + "\n")
    put("app/include/o3os/app/buttons.h", "// fixture\nenum class Button : unsigned char {\n  %s,\n};\n" % ",\n  ".join(FIXTURE_PERFORM_BUTTONS))
    rows = "".join("    bind(kGlobal, tap(button_ref(Button::%s)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::%s)),\n" % (s["button"], s["sid"])
                   for s in SCREENS.values() if s["button"])
    put(BINDINGS, 'constexpr core::ActionId kNavScreen = core::action_id("nav.screen");\nconstexpr core::Binding kBase[] = {\n%s'
        '    bind(kInputTestCtx, chord(button_ref(Button::F4), button_ref(Button::F5)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Pattern)),\n};\n' % rows)


def self_test(root):
    import contextlib
    import io

    def run(d, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = main(["--root", d] + list(argv))
        return rc, out.getvalue()

    def put(d, rel, text):
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)

    def get(d, rel):
        with open(os.path.join(d, rel), encoding="utf-8") as f:
            return f.read()

    def expect(rc, out, want_rc, *words):
        assert rc == want_rc and all(w in out for w in words), (want_rc, words, rc, out)

    def row(arr, i):
        return "**%s**" % fixture_labels(arr)[i] if fixture_labels(arr)[i] not in ("-", "--", "") else ("-" if fixture_labels(arr)[i] else "(blank)")

    perform_h = LABEL_DIR + "/perform_screen.h"
    with tempfile.TemporaryDirectory() as d:
        write_fixture(lambda rel, text: put(d, rel, text))
        # The fixture reads: every array, in each spelling, none from a comment or a use.
        tables = label_tables(d)
        assert set(tables) == {a for s in SCREENS.values() for _, a in s["keys"] + s["shift"]} | UNDOCUMENTED, sorted(tables)
        assert tables["kPerformSoftKeys"][0] == FIXTURE_LABELS["kPerformSoftKeys"] and tables["kWaveSoftKeys"][0] == fixture_labels("kWaveSoftKeys"), tables
        assert all(a not in tables for a in ("kOldKeys", "kBlockKeys")), sorted(tables)
        # A chapter for every screen, each with its empty block, and the cheat sheet with its three grids.
        for key, s in SCREENS.items():
            put(d, "%s/%s" % (MANUAL, s["chapter"]), "# %s\n\n## Keys on this screen\n\n<!-- manual-tables: keys screen=%s -->\n<!-- /manual-tables -->\n\nBy hand.\n" % (s["title"], key))
        cheat = ("# Cheat sheet\n\n<!-- manual-tables: cheat buttons -->\n<!-- /manual-tables -->\n\n<!-- manual-tables: cheat shift -->\n<!-- /manual-tables -->\n\n"
                 "<!-- manual-tables: cheat screens screens=perform,mixer -->\nold\n<!-- /manual-tables -->\n\n<!-- manual-tables: hand -->\nA hand cell.\n")
        put(d, MANUAL + "/cheat_sheet.md", cheat)
        put(d, MANUAL + "/index.md", "# Index\n")

        expect(*run(d, "--check"), 1, "`manual-tables: keys screen=perform` is out of date")
        rc, out = run(d)
        expect(rc, out, 0, "written")
        expect(*run(d, "--check"), 0, "0 problem(s)")
        perform = get(d, MANUAL + "/perform.md")
        assert "| `F1` | **SCENE 1** | **STORE 1** |" in perform and "| `F8` | **RESET** | - |" in perform and "By hand." in perform, perform
        wave = get(d, MANUAL + "/wave.md")
        assert "| Key | WAVE | CHOP | WAVETABLE | GRANULAR | With `Shift` |" in wave, wave
        assert "| `F1` | **WaveSoft1** | **ChopSoft1** | **WtSoft1** | **GranSoft1** | **WaveShift1** |" in wave, wave
        record = get(d, MANUAL + "/record.md")
        assert "`Shift`, RADIO |" in record and "| `F5` | **RecordSoft5** | **RecordLoop5** | **UNDO LYR** | **RecordShift5** | **RecordRadioShift5** |" in record, record
        assert "| `F8` | **RecordSoft8** | **RecordLoop8** | - |" in record, record  # "--" is a dash
        file_md = get(d, MANUAL + "/file.md")
        assert "| `F2` | **FileSoft2** | (blank) |" in file_md, file_md  # an empty label is a blank
        sheet = get(d, MANUAL + "/cheat_sheet.md")
        assert "| `Params` | Instrument | [Instrument](instrument.md) |" in sheet and "| `Pattern` | Pattern | [Pattern](pattern.md) |" in sheet, sheet
        assert "| Key | Pattern | Instrument | Sample load | Mixer | File |" in sheet, sheet
        assert "| `Shift` + `F6` | **PatternShift6** | **ParamsShift6** | **SampleLoadShift6** | - | **FileShift6** |" in sheet, sheet
        assert "| [Perform](perform.md) | `Perform` | **SCENE 1**, **SCENE 2**" in sheet and "[Mixer](mixer.md)" in sheet and "A hand cell." in sheet and "old" not in sheet, sheet
        expect(*run(d), 0, "0 file(s) written")  # idempotent

        # A hand-edited block, and a label that changed in the firmware: --check fails and names the block; a plain run brings it back.
        text = get(d, MANUAL + "/perform.md")
        put(d, MANUAL + "/perform.md", text.replace("**SCENE 1**", "**SCENE ONE**"))
        expect(*run(d, "--check"), 1, "perform.md", "keys screen=perform", "out of date")
        run(d)
        expect(*run(d, "--check"), 0)
        hdr = get(d, perform_h)
        put(d, perform_h, hdr.replace('"NEXT PAT"', '"NEXT"'))
        expect(*run(d, "--check"), 1, "perform.md", "cheat_sheet.md", "out of date")
        run(d)
        assert "| `F7` | **NEXT** |" in get(d, MANUAL + "/perform.md")
        # A Shift label changed in the firmware reaches the cheat sheet's grid and the chapter.
        mix = get(d, "app/include/o3os/app/mixer_view.h")
        put(d, "app/include/o3os/app/mixer_view.h", mix.replace('"UNMUTE"', '"UNMUTE ALL"'))
        expect(*run(d, "--check"), 1, "mixer.md", "cheat_sheet.md")
        run(d)
        assert "**UNMUTE ALL**" in get(d, MANUAL + "/mixer.md") and "**UNMUTE ALL**" in get(d, MANUAL + "/cheat_sheet.md")
        # A screen button that opens another screen changes the cheat sheet's table.
        b = get(d, BINDINGS)
        put(d, BINDINGS, b.replace("Button::Master)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Mixer)", "Button::Master)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Config)"))
        expect(*run(d, "--check"), 2, "Button::Master opens Config, but SCREENS says its button is Config")
        # The nav.screen rows are exactly the screen buttons: a duplicate, an extra, a missing and an unreadable one fail.
        master = "    bind(kGlobal, tap(button_ref(Button::Master)), kNavScreen, ArgFrom::Fixed, screen_arg(ScreenId::Mixer)),\n"
        assert master in b
        for what, text, words in (
                ("duplicate row", b.replace(master, master + master), ("Button::Master has two nav.screen rows of Global",)),
                ("extra row", b.replace(master, master + master.replace("Master", "Play").replace("Mixer", "Pattern")), ("12 nav.screen rows of Global, expected 11",)),
                ("missing row", b.replace(master, ""), ("10 nav.screen rows of Global", "expected 11")),
                ("unreadable row", b.replace(master, master.replace("kNavScreen, ArgFrom::Fixed,", "kNavScreen,")), ("11 nav.screen rows of Global, the tool reads 10",))):
            put(d, BINDINGS, text)
            expect(*run(d, "--check"), 2, *words)
        put(d, BINDINGS, b)
        expect(*run(d, "--check"), 0)

        # A declaration the tool cannot read is an error that names the file, the line and the array.
        for what, old, new, words in (
                ("seven labels", '"SCENE 3", "HALF"', '"SCENE 3"', ("perform_screen.h:", "kPerformSoftKeys", "7 string literals")),
                ("a pipe in a label", '"HALF"', '"HA|F"', ("kPerformSoftKeys", "a character Markdown would read")),
                ("a stray token", '"RESET"}', '"RESET", x}', ("kPerformSoftKeys", "8 string literals (and other tokens)")),
                ("a size that is not eight", "kPerformSoftKeys[8]", "kPerformSoftKeys[9]", ("kPerformSoftKeys", "the size is `[9]`")),
                ("an unknown spelling", "inline constexpr const char* kPerformSoftKeys", "constexpr char const* kPerformSoftKeys",
                 ("perform_screen.h:", "kPerformSoftKeys is declared in a form the tool cannot read")),
                ("a declaration after a statement", "inline constexpr const char* kPerformSoftKeys", "int x = 1; inline constexpr const char* kPerformSoftKeys",
                 ("kPerformSoftKeys is declared in a form the tool cannot read",)),
                ("a pointer to a table", "inline constexpr const char* kPerformSoftKeys[8]", "inline constexpr const char* const* kPerformSoftKeys[8]",
                 ("kPerformSoftKeys is declared in a form the tool cannot read",)),
                ("an extern without a body", "inline constexpr const char* kPerformSoftKeys", "extern const char* const kPerformExtraKeys[8];\ninline constexpr const char* kPerformSoftKeys",
                 ("kPerformExtraKeys is declared in a form the tool cannot read",))):
            hdr = get(d, perform_h)
            assert old in hdr, what
            put(d, perform_h, hdr.replace(old, new, 1))
            expect(*run(d, "--check"), 2, *words)
            put(d, perform_h, hdr)
        # A key row the manual does not cover (in a header the glob finds), and one that was renamed under SCREENS.
        put(d, LABEL_DIR + "/extra_screen.h", 'static const char* const kExtraKeys[8] = {"A", "B", "C", "D", "E", "F", "G", "H"};\n')
        expect(*run(d, "--check"), 2, "extra_screen.h:1", "kExtraKeys is a key row the manual's tables do not cover")
        os.remove(os.path.join(d, LABEL_DIR, "extra_screen.h"))
        song_h = LABEL_DIR + "/song_screen.h"
        hdr = get(d, song_h)
        put(d, song_h, hdr.replace("kSongShiftKeys", "kSongShift2Keys"))
        expect(*run(d, "--check"), 2, "SCREENS[song] lists kSongShiftKeys")
        put(d, song_h, hdr)
        os.remove(os.path.join(d, LABEL_DIR, "other_screen.h"))
        expect(*run(d, "--check"), 2, "SCREENS[", "which no label header declares")
        write_fixture(lambda rel, text: put(d, rel, text))
        run(d)  # the labels are the fixture's again: write the chapters back
        expect(*run(d, "--check"), 0)

        # Markers: unknown, unclosed, stray close, unknown screen, a chapter that lost its block.
        good = get(d, MANUAL + "/song.md")
        for what, text, words in (
                ("unknown marker", good.replace("keys screen=song", "table screen=song"), ("song.md:", "unknown marker `manual-tables: table screen=song`")),
                ("unknown screen", good.replace("screen=song", "screen=sonq"), ("song.md:", "`keys screen=sonq`: unknown screen")),
                ("unclosed", good.replace("<!-- /manual-tables -->", ""), ("song.md:5", "never closed")),
                ("stray close", good + "<!-- /manual-tables -->\n", ("song.md:", "with no opening marker")),
                ("no block", re.sub(r"<!-- manual-tables: keys screen=song -->.*?<!-- /manual-tables -->\n", "", good, flags=re.S), ("song.md: no `<!-- manual-tables: keys screen=song -->` block",))):
            put(d, MANUAL + "/song.md", text)
            rc, out = run(d, "--check")
            assert rc in (1, 2) and all(w in out for w in words), (what, rc, out)
        put(d, MANUAL + "/song.md", good)
        expect(*run(d, "--check"), 0)
        # --check never writes.
        put(d, MANUAL + "/song.md", good.replace("**SongSoft1**", "**X**"))
        run(d, "--check")
        assert "**X**" in get(d, MANUAL + "/song.md")
    print("manual_tables --self-test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
