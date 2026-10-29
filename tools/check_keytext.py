#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-KEYROWDLG S4 (docs/specs/OS-KEYROWDLG.md, section 2.1 rule 5): a key is named in one table.

The F-key row, the dialog's key-cap line and the strip's question text are all made from the key tables of app/src/key_prompt.cpp, and tests/key_prompt_test.cpp resolves every key of
a table against the bindings. What would let the three drift apart again is a dialog that writes "F1 SELECT" or a question that writes "F3 AGAIN" in a string of its own. So this
fails when a string literal in app/src, app/include or platform/mk66/src/app_main.cpp names a function key (F1..F8, SHIFT+F1..) outside app/src/key_prompt.cpp, unless the literal is
in ALLOW below: each entry a file, the whole literal and a one-line reason. The entries are the pointers of screens that name a key of their own plain row in an info line or an
empty-state message (spec audit E1), the printed names of the buttons, and the like. A new entry is a decision for a reviewer: a dialog or a question uses the table instead.
An entry that no longer matches any literal fails too (a stale entry would hide the next one).

Comments are not scanned (only string literals). Usage: check_keytext.py [--root DIR] [--list] | --self-test      Exit 0 clean, 1 violations.
"""
import argparse
import contextlib
import io
import re
import sys
import tempfile
from pathlib import Path

TABLE = "app/src/key_prompt.cpp"
# OS-HELP: the help pages (app/src/help_text.cpp) are the documentation of the keys, so they name them in nearly every line by design; they are not a prompt and are skipped like the table. Their keys are checked against
# the bindings by review (a drift test is a later row, see Risks in docs/specs/OS-HELP.md); the width and ASCII of every line by tests/help_test.cpp.
DOCS = ("app/src/help_text.cpp",)
SCAN_DIRS = ("app/src", "app/include")
SCAN_FILES = ("platform/mk66/src/app_main.cpp",)
SUFFIXES = (".cpp", ".h")

# A function key: F1..F8 as a word, or SHIFT+F1 in any case with or without spaces round the plus.
KEY = re.compile(r"\bF[1-8]\b|\b[Ss][Hh][Ii][Ff][Tt]\s*\+\s*[Ff][1-8]\b")

# (file, the literal as written between the quotes, the reason). Grouped by reason; the reason of a group is repeated on each of its entries.
ALLOW = (
    # ---- OS-USBDISK U2b
    ('app/src/param_actions.cpp', 'THE COMPUTER GETS THE CARD: SAVES STOP. F7 AGAIN', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/param_actions.cpp', 'USB AUDIO PAUSES. F7 AGAIN TO SHARE THE CARD', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/param_actions.cpp', 'THE COMPUTER MAY STILL BE WRITING: F7 AGAIN', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/screens/config_screen.cpp', 'OUT: F7 EJECTS', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/screens/config_screen.cpp', 'F7: SHARE', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/screens/config_screen.cpp', 'F7 twice: the computer gets the card', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/screens/config_screen.cpp', 'Eject: F7 twice, or on the computer', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/screens/config_screen.cpp', 'F7 twice: the computer gets the card (saves and loads stop)', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/screens/file_screen.cpp', 'CONFIG USB: F7 EJECT', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/src/screens/sample_load_screen.cpp', 'CONFIG USB: F7 EJECT', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    ('app/include/o3os/app/app.h', 'CARD ON COMPUTER - CONFIG USB: F7 EJECT', "OS-USBDISK U2b: the CARD AS DISK row and the strips of the card that is out name their own key; the texts are spec section 9's"),
    # ---- OS-USBOUTS: the STEMS page's OUTPUTS row names its own key (the APPLY key twice) in the row text and in the info line
    ('app/src/screens/mixer_screen.cpp', '*   F6 APPLY', "OS-USBOUTS: the OUTPUTS row names its own key (F6 APPLY) while a value is pending"),
    ('app/src/screens/mixer_screen.cpp', 'Left/Right: choose  F6 twice: apply  Delete: drop', "OS-USBOUTS: the info line of the OUTPUTS row points at its own keys, as the STEMS line does"),
    # ---- the short names of the 33 buttons the INPUT TEST cells show, by button code (F1..F8 are buttons 0..7); a button's printed name, not a prompt
    ('app/src/screens/input_test_screen.cpp', 'F1', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    ('app/src/screens/input_test_screen.cpp', 'F2', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    ('app/src/screens/input_test_screen.cpp', 'F3', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    ('app/src/screens/input_test_screen.cpp', 'F4', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    ('app/src/screens/input_test_screen.cpp', 'F5', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    ('app/src/screens/input_test_screen.cpp', 'F6', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    ('app/src/screens/input_test_screen.cpp', 'F7', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    ('app/src/screens/input_test_screen.cpp', 'F8', 'the short names of the 33 buttons the INPUT TEST cells show, by button code'),
    # ---- the registry's printed key of a screen that has no screen button of its own: INPUT TEST (hidden, opened by a boot chord) and an app's page (the exit chord the OS binds)
    ('app/src/screens/registry.cpp', 'F1', "the registry's printed key of a screen that has no screen button of its own: INPUT TEST"),
    ('app/src/screens/registry.cpp', 'F7 + F8', "the registry's printed key of a screen that has no screen button of its own: INPUT TEST"),
    # ---- OS-BUG-SLDOVER: the info line of a refused WAV load names the key that deletes the module instrument (the pointer of INS 15 IS A MODULE: DELETE IT FIRST, with its key)
    ('app/src/sample_actions.cpp', 'SLOT HAS A MODULE: INSTRUMENT, SHIFT+F4 DELETE', "OS-BUG-SLDOVER: the refusal of a WAV onto a module slot points at the Instrument page's DELETE key"),
    ('app/src/sample_actions.cpp', 'SLOT IS MIDI: INSTRUMENT, SHIFT+F4 DELETE', "OS-QAFIX-E-LOAD-25: the refusal of a WAV onto a MIDI instrument's slot points at the Instrument page's DELETE key"),
    # ---- the empty Instrument slot's five choices name their keys beside their words (the precedent of the key row itself: spec C1); the keys are F1..F5 of that page's row
    ('app/src/screens/params_screen.cpp', '   empty   F1..F5: choose', "the empty Instrument slot's five choices name their keys beside their words"),
    ('app/src/screens/params_screen.cpp', 'F1', "the empty Instrument slot's five choices name their keys beside their words"),
    ('app/src/screens/params_screen.cpp', 'F2', "the empty Instrument slot's five choices name their keys beside their words"),
    ('app/src/screens/params_screen.cpp', 'F3', "the empty Instrument slot's five choices name their keys beside their words"),
    ('app/src/screens/params_screen.cpp', 'F4', "the empty Instrument slot's five choices name their keys beside their words"),
    ('app/src/screens/params_screen.cpp', 'F5', "the empty Instrument slot's five choices name their keys beside their words"),
    # ---- MODULES and an app's page: the exit chord F7 + F8 is the OS's (a binding in the app page's context), named in the info line and the quarantine note; S5 of OS-KEYROWDLG gives the page labels
    ('app/src/screens/modules_screen.cpp', 'F1 launches the app on its own page', "MODULES and an app's page: the exit chord F7 + F8 is the OS's"),
    ('app/src/screens/modules_screen.cpp', 'exit an app with F8', "MODULES and an app's page: the exit chord F7 + F8 is the OS's"),
    ('app/src/screens/modules_screen.cpp', 'F8: back', "MODULES and an app's page: the exit chord F7 + F8 is the OS's"),
    ('app/src/screens/modules_screen.cpp', 'it is quarantined until reboot: leave with F8', "MODULES and an app's page: the exit chord F7 + F8 is the OS's"),
    # ---- the dialog's body text of the export progress (and the EXPORT page's note) names its one key, the F8 CANCEL of its prompt; the strip and goldens keep these words
    ('app/src/screens/export_overlay.cpp', 'F8 stops it: finished files stay, no project.mt.', "the dialog's body text of the export progress"),
    ('app/src/screens/export_overlay.cpp', 'Nothing else works until it ends. F8 stops it; finished files stay.', "the dialog's body text of the export progress"),
    ('app/src/screens/file_screen.cpp', 'F8 cancel', "the dialog's body text of the export progress"),
    ('app/src/screens/file_screen.cpp', 'While it runs, F8 cancels it; the file being made is not left (stems already made stay).', "the dialog's body text of the export progress"),
    ('app/src/screens/file_screen.cpp', 'While it runs, F8 stops it: finished files stay, and there is no project.mt until the last.', "the dialog's body text of the export progress"),
    # ---- the fallback text of the first-launch confirm of an edit app (ModuleUi::launch_text); the live text, launch_message(), is composed with the Launch question's key table
    ('app/src/module_ui.cpp', 'EDITS THE PROJECT (UNDOABLE): F1 AGAIN', 'the fallback text of the first-launch confirm of an edit app'),
    # ---- the info line of a screen names a key of its own plain row (audit E1: Config, File, Sample load, Mixer, Record, Song, Wave, Perform)
    ('app/src/screens/config_screen.cpp', '* chosen, not applied yet: F7', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/config_screen.cpp', 'F1..F6: page', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/config_screen.cpp', 'F7 twice: USB reconnects', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/config_screen.cpp', 'Play in Edit; F1 on Record', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/file_screen.cpp', 'F1 export   F6 import   F7 projects', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/file_screen.cpp', 'F1 import   F3 up   F7 fx', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/file_screen.cpp', 'last import   F8: back to the list', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/file_screen.cpp', 'left / right: letter   F2 rescan   Delete: delete', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/mixer_screen.cpp', 'left / right: column  up / down: field  encoder: change  F7 mute  F8 solo', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/perform_screen.cpp', 'hold Shift: TRACKS  hold F1-F3: store the scene  Rec: edit', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'listening for the level; F1 cancels', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'F1 records the loop on the bars','the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'F1 records; START ON says where it starts', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'F1: overdub  F5: undo  F3: save  F4: discard', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'F3: save to a free instrument  F4: discard', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'armed: Play starts it on the step; F1 stops', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'recording: F1 stops; gain and monitor move', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'recording: ends by itself; F1 stops now', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/record_screen.cpp', 'waiting for the STOP AT boundary; F1 again stops now', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/sample_edit_screen.cpp', 'AMIGA   Shift+F1: 8287 HZ   Shift+F2: 16574 HZ   Shift+F3: 8-BIT ', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/sample_edit_screen.cpp', 'pad / Shift+F5: change', 'the header hint of a screen names a key of its own Shift row (OS-QAFIX-27)'),
    ('app/src/screens/sample_load_screen.cpp', 'pad: target   left / right: letter   F3 enter   F1 load','the info line of a screen names a key of its own plain row'),
    ('app/src/screens/sample_load_screen.cpp', 'pad: target   left / right: letter   F3 enter   F4 add', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/song_screen.cpp', 'tempo   encoder: 1 BPM, Shift: 0.1   F7 again: tap', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/wave_screen.cpp', '   SHIFT+F1: CHANGE', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/wave_screen.cpp', 'F1 AUTO: one slice per hit', 'the info line of a screen names a key of its own plain row'),
    ('app/src/screens/wave_screen.cpp', 'F2 EVEN: equal slices (shift + up / down: n)', 'the info line of a screen names a key of its own plain row'),
    # ---- an empty-state message names the key that fixes it (audit E1; one wording says SHIFT+F5 saves an instrument on Sample load, where its own Shift+F5 is END: recorded, not changed)
    ('app/src/screens/file_screen.cpp', 'press F2 to read the folder again', 'an empty-state message names the key that fixes it'),
    ('app/src/screens/file_screen.cpp', 'press F3 to go up', 'an empty-state message names the key that fixes it'),
    ('app/src/screens/sample_load_screen.cpp', 'SHIFT+F5 ON AN INSTRUMENT MAKES IT; F2 goes up', 'an empty-state message names the key that fixes it'),
    ('app/src/screens/sample_load_screen.cpp', 'SHIFT+F5 SAVES AN INSTRUMENT HERE; folders are listed too', 'an empty-state message names the key that fixes it'),
    ('app/src/screens/sample_load_screen.cpp', 'press F2 for the folder above', 'an empty-state message names the key that fixes it'),
    ('app/src/screens/sample_load_screen.cpp', 'press F6 to read the folder again', 'an empty-state message names the key that fixes it'),
    # ---- a notice on the strip points at a key of the screen's plain row or of another screen's (audit E1)
    ('app/src/file_ops_actions.cpp', ' INSTRUMENT - F8 ON PARAMS UNDOES', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/file_ops_actions.cpp', ' INSTRUMENTS - F8 ON PARAMS UNDOES', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/fill_actions.cpp', 'FILL: TURN EDIT ON (EXPORT: F7 TOOLS)', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/instrument_actions.cpp', ' - F8 UNDO', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/instrument_actions.cpp', 'NOTHING TO PASTE: SHIFT+F2 COPIES', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/module_import.cpp', 'F8 REPORT', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/param_actions.cpp', 'PAGE RESET: F7 TO APPLY', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/pattern_actions.cpp', 'EXPORT: IN F7 TOOLS', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/record_actions.cpp', 'NO TAKE TO PLAY: F1 RECORDS ONE', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/record_actions.cpp', 'NO TAKE TO SAVE: F1 RECORDS ONE', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/save_actions.cpp', 'NO TAKE TO SAVE: F1 RECORDS ONE', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    ('app/src/screens/params_screen.cpp', 'NOTHING COPIED: SHIFT+F2 ON AN INSTRUMENT', "a notice on the strip points at a key of the screen's plain row or of another screen's"),
    # ---- the image's boot chord (Shift + F1 + F8, and F7 for the disk) in a compile-time message and in the console's log: not a screen
    ('platform/mk66/src/app_main.cpp', 'boot: chord Shift+F1+F7, USB DISK mode (the card is not mounted, nothing plays)', "the image's boot chord"),
    ('platform/mk66/src/app_main.cpp', 'boot: chord keys held (bit 0 Shift, 1 F1, 2 F8)', "the image's boot chord"),
    ('platform/mk66/src/app_main.cpp', 'the boot chord is Shift (29), F1 (0), F8 (7), bits 0, 1, 2 of the held mask', "the image's boot chord"),
    ('platform/mk66/src/app_main.cpp', "the boot disk chord's third key is F7 (6)", "the image's boot chord"),
)


def strings_of(text):
    """The string literals of C++ source as (line, text), comments and character literals skipped. Escapes are left as written."""
    out = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text.count("\n", i, j)
            i = j
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif c == 'R' and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.compile(r'R"([^(\s]{0,16})\(').match(text, i)
            if m is None:
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', m.end())
            end = n if end < 0 else end
            out.append((line, text[m.end():end]))
            line += text.count("\n", i, end)
            i = end + len(m.group(1)) + 2
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append((line, text[i + 1:j]))
            i = j + 1
        else:
            i += 1
    return out


def source_files(root):
    files = []
    for d in SCAN_DIRS:
        base = root / d
        if base.is_dir():
            files += [p for p in sorted(base.rglob("*")) if p.suffix in SUFFIXES]
    files += [root / f for f in SCAN_FILES if (root / f).is_file()]
    return files


def scan(root, allow):
    """(violations, stale): the flagged literals not in `allow`, and the entries of `allow` no literal matched."""
    violations = []
    used = set()
    for path in source_files(root):
        rel = path.relative_to(root).as_posix()
        if rel == TABLE or rel in DOCS:
            continue
        for line, lit in strings_of(path.read_text(encoding="utf-8", errors="replace")):
            if not KEY.search(lit):
                continue
            entry = next((a for a in allow if a[0] == rel and a[1] == lit), None)
            if entry is None:
                violations.append((rel, line, lit))
            else:
                used.add(entry)
    stale = [a for a in allow if a not in used]
    return violations, stale


def check(root, allow):
    violations, stale = scan(root, allow)
    for rel, line, lit in violations:
        print("%s:%d: a function key named in a string outside %s: \"%s\" (make it a key table there, or, for a pointer in an info line, add it to ALLOW with a reason)" % (rel, line, TABLE, lit))
    for rel, lit, _ in stale:
        print("tools/check_keytext.py: the ALLOW entry (%s, \"%s\") matches no literal any more: remove it" % (rel, lit))
    return 1 if violations or stale else 0


def self_test():
    failures = []

    def expect(name, ok):
        if not ok:
            failures.append(name)

    expect("a string is found", strings_of('a("F1 OK", \'F\');') == [(1, "F1 OK")])
    expect("comments are skipped", strings_of('// "F1"\n/* "F2" */ x("F3");') == [(2, "F3")])
    expect("an escaped quote stays inside", strings_of('x("a\\"F1");') == [(1, 'a\\"F1')])
    expect("a raw string is a literal", strings_of('x(R"(F1 text)");') == [(1, "F1 text")])
    expect("a char literal is not a string", strings_of("char c = '\"'; x(\"F2\");") == [(1, "F2")])
    expect("F1 is a key", KEY.search("press F1 now") is not None)
    expect("F9 is not", KEY.search("press F9 now") is None)
    expect("a word that ends in F1 is not", KEY.search("SELF1") is None)
    expect("SHIFT+F7 is a key", KEY.search("SHIFT+F7 AGAIN") is not None)
    expect("shift + F1 is a key", KEY.search("shift + F1: 8287 HZ") is not None)
    expect("a range is a key", KEY.search("F1..F5: choose") is not None)
    expect("lower case f1 is a button name, not a key", KEY.search("f1") is None)

    def quiet(root, allow):
        with contextlib.redirect_stdout(io.StringIO()):
            return check(root, allow)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "app/src").mkdir(parents=True)
        (root / "app/include").mkdir(parents=True)
        (root / "platform/mk66/src").mkdir(parents=True)
        (root / TABLE).write_text('const char* a = "F1 SELECT";\n', encoding="utf-8")
        (root / "app/src/clean.cpp").write_text('// F1 in a comment\nconst char* a = "BACK";\n', encoding="utf-8")
        expect("the table and a clean file pass", quiet(root, ()) == 0)
        (root / DOCS[0]).write_text('const char* a = "F1 PLAY PAT";\n', encoding="utf-8")
        expect("the help text may name keys", quiet(root, ()) == 0)
        (root / DOCS[0]).unlink()
        (root / "app/src/dialog.cpp").write_text('x("F8 BACK");\n', encoding="utf-8")
        expect("a key in a dialog fails", quiet(root, ()) == 1)
        expect("an entry allows it", quiet(root, (("app/src/dialog.cpp", "F8 BACK", "reason"),)) == 0)
        expect("an entry for another literal does not", quiet(root, (("app/src/dialog.cpp", "F7 BACK", "reason"),)) == 1)
        expect("a stale entry fails", quiet(root, (("app/src/dialog.cpp", "F8 BACK", "reason"), ("app/src/clean.cpp", "F2", "gone"))) == 1)
        (root / "app/include/hint.h").write_text('inline const char* h = "shift + F2: copy";\n', encoding="utf-8")
        expect("a header is scanned", quiet(root, (("app/src/dialog.cpp", "F8 BACK", "reason"),)) == 1)
        (root / "app/include/hint.h").unlink()
        (root / "platform/mk66/src/app_main.cpp").write_text('log("press F3");\n', encoding="utf-8")
        expect("the image's main is scanned", quiet(root, (("app/src/dialog.cpp", "F8 BACK", "reason"),)) == 1)
        (root / "platform/mk66/src/other.cpp").write_text('log("press F3");\n', encoding="utf-8")
        expect("another file of the image is not", quiet(root, (("app/src/dialog.cpp", "F8 BACK", "reason"), ("platform/mk66/src/app_main.cpp", "press F3", "r"))) == 0)
    for name in failures:
        print("self-test failed: " + name)
    print("check_keytext self-test: %d failed" % len(failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--list", action="store_true", help="print every literal that names a key outside the table, allowed or not")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    root = Path(args.root)
    if args.list:
        for path in source_files(root):
            rel = path.relative_to(root).as_posix()
            if rel == TABLE or rel in DOCS:
                continue
            for line, lit in strings_of(path.read_text(encoding="utf-8", errors="replace")):
                if KEY.search(lit):
                    print("%s:%d: %s" % (rel, line, lit))
        return 0
    return check(root, ALLOW)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
