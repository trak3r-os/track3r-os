#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-E2E-1b: sim/src/sim_project.cpp (SimProject) copies the platform's load sequence; this keeps the copy from drifting unseen.

The platform's sequence is in platform/mk66/src/app_main.cpp, the composition root (firmware: nothing shared with the simulator can live there, E2E-1 latent gap 1):
`finish_load`, `begin_samples` and `begin_replace` hand a loaded project to the app. SimProject repeats the steps that matter to the app's state. This test extracts the
names of the calls those three functions make and fails on any that is neither called in sim_project.cpp nor on NOT_SIMULATED below, which says WHY the simulator does
without it. So a call added to one of the three on the unit (a new hand-over step) fails here until SimProject does it too, or until someone writes down that the
simulator does not, and a listed call that the unit no longer makes fails as stale (the list stays true).

Usage: sim_project_drift_test.py <repo root>      (a self-test of the extraction runs first)
"""
import os
import re
import sys
import textwrap

FUNCTIONS = ("finish_load", "begin_samples", "begin_replace")

# A call by its last name (`g_app->open_pattern(` and `app::open_pattern(` are `open_pattern`). Each entry: the reason the simulator has no such step.
NOT_SIMULATED = {
    # the unit's console and `diag` summaries: the simulator has none (`describe_state` is its own view of the app)
    "log": "console log lines",
    "log_dec": "console log lines",
    "log_muted_tracks": "console log lines",
    "log_inserts_unbound": "console log line (the simulator has no module binder: no inserts are bound on a load)",
    "describe_native_load": "the load summary goes to the console and `diag sd`",
    "describe_load": "the load summary goes to the console and `diag sd`",
    "module_strip_text": "the module import's summary text goes to the console and `diag sd` (the strip's own words come from the app, which reads the sink: SimProject::import_summary)",
    "summary": "the module import's summary text goes to the console and `diag sd` (SimProject::detail reads the summary's error text, which is a different use)",
    # what is the unit's platform, not the app's hand-over
    "apply_project_platform_state": "the project's MIDI and USB parts (hardware settings; its own slice)",
    "native_load": "SimProject keeps its own native loader (`native_loader_`)",
    "storage": "the unit's card: SimProject is handed its own",
    "audio_voices": "the unit's voice pool: SimProject is handed its own",
    "sample_arena": "the unit's sample memory: SimProject is handed its own",
    "clear_sample_table": "SimProject empties the voices' table inline in begin_samples (a `set_sample` loop)",
    # a module file import (File > IMPORT) is simulated since E2E-9 (SimProject::import_module: ModuleImport, its sample feed, close): `module_load()` is the platform's accessor of the active import,
    # SimProject asks its own ModuleImport (`module_->active()`). The SD modules and their binder are E2E-7
    "module_load": "SimProject asks its own ModuleImport whether the load is an import (`module_->active()`)",
    "begin_modules": "the SD modules' binder (E2E-7); the demo kit of a project with no sample instrument is not simulated either",
    "modsys": "the SD modules' system (E2E-7)",
    "cancel": "the module loader, `diag mod load` (E2E-7)",
    "diag_clear": "the module console's binds (E2E-7)",
    "retire_all": "the module instruments leave the render (E2E-7)",
    # `stop` (the transport's Stop) and `notes_off` (the live notes let go) ARE simulated since E2E-2: SimProject::begin_replace calls them on the sinks `audio on` attaches (attach_sound), and
    # waits for the voices (Settle). The Settle's clock is the simulator's own tick count (SimProject::pump is one a tick), not the platform's system clock:
    "system_clock": "the Settle's clock: SimProject counts the simulator's ticks (pump is called once a tick)",
    "uptime_ms": "the Settle's clock: SimProject counts the simulator's ticks (pump is called once a tick)",
}

# Calls the simulator makes under another name: firmware name -> the name in sim_project.cpp.
RENAMED = {
    "finish_load": "finish",  # SimProject::finish
}

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "alignof", "catch", "else", "do", "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast", "decltype", "defined", "void"}


def strip_noise(text):
    """Comments, string/char literals and preprocessor lines out, line structure kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == '"' or c == "'":
            q = c
            i += 1
            while i < n and text[i] != q:
                i += 2 if text[i] == "\\" else 1
            i += 1
            out.append('""' if q == '"' else "' '")
        else:
            out.append(c)
            i += 1
    return "\n".join(line for line in "".join(out).split("\n") if not line.lstrip().startswith("#"))  # (preprocessor lines: `#if !X` is no call)


def function_body(text, name):
    """The text between the braces of the definition of `name` (a definition at the start of a line: `void name() noexcept {`); None when there is none."""
    m = re.search(r"^[A-Za-z_][\w:<>\*& ]*\b" + re.escape(name) + r"\s*\([^)]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{", text, re.M)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1] if depth == 0 else None


def calls(body):
    """The last names of the calls in `body`."""
    return {m.group(1) for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", body)} - KEYWORDS


def read(path):
    with open(path, encoding="utf-8") as f:
        return strip_noise(f.read())


def check(app_main_text, sim_text, functions=FUNCTIONS, not_simulated=NOT_SIMULATED, renamed=RENAMED):
    """-> list of problems (empty: no drift)."""
    problems = []
    in_sim = calls(sim_text)
    in_sim |= {old for old, new in renamed.items() if new in in_sim}
    seen = set()
    for fn in functions:
        body = function_body(app_main_text, fn)
        if body is None:
            problems.append("app_main.cpp: no definition of %s() found (renamed or moved? then the extraction and this test change with it)" % fn)
            continue
        names = calls(body)
        if not names:
            problems.append("app_main.cpp: %s() has no calls: the extraction is broken" % fn)
        seen |= names
        for name in sorted(names - in_sim - set(not_simulated)):
            problems.append("%s() calls %s(), which sim/src/sim_project.cpp does not: simulate it there, or add it to NOT_SIMULATED in tests/sim_project_drift_test.py with the reason" % (fn, name))
    for name in sorted(set(not_simulated) - seen):
        problems.append("NOT_SIMULATED lists %s, which finish_load, begin_samples and begin_replace no longer call: remove the entry" % name)
    return problems


def self_test():
    firmware = """
    // g_app->nothing() in a comment is no call
    void finish_load() noexcept {
      log("a call(in a string)");
      g_app->open_pattern(1);
      if (x) { new_step(); }
    }
    void begin_samples() noexcept { app::project_law(p); }
    void begin_replace() noexcept {
    #if !X
      stop();
    #endif
    }
    """
    sim = "void f() { app_.open_pattern(1); project_law(p); }"
    text, simtext = strip_noise(textwrap.dedent(firmware)), strip_noise(sim)
    assert function_body(text, "finish_load") is not None and function_body(text, "nope") is None
    assert calls(function_body(text, "finish_load")) == {"log", "open_pattern", "new_step"}, calls(function_body(text, "finish_load"))
    assert calls(function_body(text, "begin_replace")) == {"stop"}
    problems = check(text, simtext, not_simulated={"log": "x", "stop": "x"}, renamed={})
    assert len(problems) == 1 and "new_step" in problems[0], problems  # a call neither simulated nor listed
    assert check(text, simtext, not_simulated={"log": "x", "stop": "x"}, renamed={"new_step": "open_pattern"}) == []  # (a renamed one counts as simulated)
    problems = check(text, simtext, not_simulated={"log": "x", "stop": "x", "new_step": "x", "gone": "x"}, renamed={})
    assert len(problems) == 1 and "gone" in problems[0], problems  # a stale entry
    assert check(text, simtext, not_simulated={"log": "x", "stop": "x", "new_step": "x"}, renamed={}) == []
    assert any("no definition" in p for p in check("int x;", simtext, renamed={}))


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    self_test()
    root = argv[1]
    problems = check(read(os.path.join(root, "platform", "mk66", "src", "app_main.cpp")), read(os.path.join(root, "sim", "src", "sim_project.cpp")))
    for p in problems:
        print("sim_project_drift: " + p, file=sys.stderr)
    if not problems:
        print("sim_project_drift: ok (%s all simulated or listed)" % ", ".join(FUNCTIONS))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
