#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The simulator and the unit wire the App the same way (OS-REVIEW), textually (stdlib only).

Bug class: in 0.4.1 the unit's start_app() never called g_app->attach_param_ring(...) while the simulator did, so the Mixer moved on the
screen and not in the sound, and every host and simulator test passed. The two sides cannot share one wiring function (the unit hands the
App the real engine, card and recorder; the simulator and the tests hand it fakes or nothing), so this test compares what they call:

  * every attach_* / set_build_info hook that app/include/o3os/app/app.h declares is called by start_app() of the unit
    (platform/mk66/src/app_main.cpp) in a normal build, unless it is listed in UNIT_NOT_WIRED below with the reason;
  * a hook that the simulator (sim/src/*.cpp, on `app_.`) calls is one that the unit calls too;
  * the hooks that an O3OS_AUDIO_TEST build leaves out are exactly AUDIO_TEST_LEAVES_OUT (that image has no sequencer, voices or card
    layer to hand over; adding a hook to that list is a decision, not an accident);
  * start_app() builds the document and the sound (make_document()) before it constructs the App, and every hook comes after the
    construction (the first boot of 0.4.0 held null boards because the order was the other way round).

Usage: wiring_audit.py <os root>        Exit 0 consistent, 1 drift.     wiring_audit.py --self-test
"""
import re
import sys
from pathlib import Path

# Hooks of app.h that start_app() deliberately does not call (none today): name -> why.
UNIT_NOT_WIRED = {}

# The hooks that an O3OS_AUDIO_TEST image (no voices, no sequencer, no save layer: diagnostics of the audio path) does not give the App.
AUDIO_TEST_LEAVES_OUT = {
    "attach_transport", "attach_preview", "attach_audio_load", "attach_samples", "attach_param_ring", "attach_mix",
    "attach_perform", "attach_recorder", "attach_saves", "attach_play_position", "attach_modules", "attach_scratch",  # (OS-DJ C1: the scratch stage is told through the param ring, which that image does not have)
    "attach_ptfx_runner",  # (OS-CEPTFX S4b: the plugin run needs the module system and the sample bank, which that image does not give the App)
    "attach_metro_sync",  # (OS-RECCOUNT RC2: that image never steps g_metro_sync — sync_metro() is under the same #if — so the override would go nowhere)
}

HOOK = r"(?:attach_\w+|set_build_info)"


def strip_comments(text):
    """The text without `//` and `/* */` comments (a commented-out call is not a call). String literals with // in them are not handled: none holds a hook name."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def app_hooks(app_h):
    # Any return type (`void`, `bool`, `Result`...): a declaration is `<type> attach_x(` or `set_build_info(` at the start of a member declaration.
    return set(re.findall(r"^[ \t]*(?:\[\[\w+\]\]\s*)?(?:[\w:<>&*]+[ \t]+)+(%s)\(" % HOOK, strip_comments(app_h), re.M))


def sim_hooks(sim_sources):
    found = set()
    for text in sim_sources:
        found |= set(re.findall(r"\bapp_\s*\.\s*(%s)\(" % HOOK, strip_comments(text)))
    return found


def start_app_body(main_cpp):
    m = re.search(r"^bool start_app\([^)]*\)[^{]*\{\n", main_cpp, re.M)
    if not m:
        raise ValueError("start_app() not found in app_main.cpp")
    end = main_cpp.index("\n}\n", m.end())
    return strip_comments(main_cpp[m.end():end]).split("\n")


def unit_calls(body):
    """Returns (calls, errors): calls is the set of hooks start_app() makes, guarded the subset made only in a normal build (`#if !O3OS_AUDIO_TEST`)."""
    errors = []
    guards = []  # True for each open `#if !O3OS_AUDIO_TEST`
    calls, guarded = set(), set()
    made_document = constructed = None
    for n, line in enumerate(body):
        s = line.strip()
        if re.match(r"#\s*if\b", s):
            guards.append(bool(re.match(r"#\s*if\s+!\s*O3OS_AUDIO_TEST\b", s)))
        elif re.match(r"#\s*endif\b", s):
            guards.pop()
        elif re.match(r"#\s*(else|elif)\b", s):
            errors.append("start_app(): #else/#elif is not understood by this audit (line %d of the body)" % n)
        if re.search(r"\bmake_document\(\)", s) and made_document is None:
            made_document = n
        if re.search(r"\bg_app\s*=\s*new\b", s):
            constructed = n
        m = re.search(r"\bg_app->(%s)\(" % HOOK, s)
        if m:
            calls.add(m.group(1))
            if any(guards):
                guarded.add(m.group(1))
            if constructed is None:
                errors.append("%s is called before the App is constructed" % m.group(1))
    if made_document is None or constructed is None or made_document > constructed:
        errors.append("make_document() must come before the App is constructed in start_app()")
    return calls, guarded, errors


def audit(app_h, main_cpp, sim_sources):
    problems = []
    declared = app_hooks(app_h)
    if len(declared) < 10:
        problems.append("app.h: only %d hooks found, the parser lost them" % len(declared))
    try:
        body = start_app_body(main_cpp)
    except ValueError as e:
        return [str(e)]
    calls, guarded, errors = unit_calls(body)
    problems += errors
    for name in sorted(declared):
        if name not in calls and name not in UNIT_NOT_WIRED:
            problems.append("app.h declares %s and start_app() never calls it (the unit does not wire it)" % name)
    for name in sorted(sim_hooks(sim_sources)):
        if name not in declared:
            problems.append("the simulator calls %s, which app.h does not declare" % name)
        elif name not in calls:
            problems.append("the simulator calls %s and the unit does not" % name)
    if guarded != AUDIO_TEST_LEAVES_OUT:
        problems.append("hooks under #if !O3OS_AUDIO_TEST are %s, expected %s (update AUDIO_TEST_LEAVES_OUT on purpose)"
                        % (sorted(guarded), sorted(AUDIO_TEST_LEAVES_OUT)))
    return problems


def read_tree(root):
    app_h = (root / "app/include/o3os/app/app.h").read_text()
    main_cpp = (root / "platform/mk66/src/app_main.cpp").read_text()
    sim = [p.read_text() for p in sorted((root / "sim/src").glob("*.cpp"))]
    return app_h, main_cpp, sim


def self_test(root):
    app_h, main_cpp, sim = read_tree(root)
    base = audit(app_h, main_cpp, sim)
    if base:
        print("self-test: the tree itself is not clean:", base)
        return 1
    cases = {
        "param ring dropped": (app_h, main_cpp.replace("g_app->attach_param_ring(", "(void)(", 1), sim),
        "new hook": (app_h.replace("void attach_param_ring(", "void attach_new_thing(core::ParamRing* r) noexcept {}\n  void attach_param_ring(", 1), main_cpp, sim),
        "new hook, bool": (app_h.replace("void attach_param_ring(", "bool attach_extra(int x) noexcept { return x != 0; }\n  void attach_param_ring(", 1), main_cpp, sim),
        "order": (app_h, main_cpp.replace("make_document();", "(void)0;", 1), sim),
        "commented out": (app_h, main_cpp.replace("  g_app->attach_storage(", "  // g_app->attach_storage(", 1), sim),
        "audio test": (app_h, main_cpp.replace("  g_app->attach_storage(&storage::device::storage());", "#if !O3OS_AUDIO_TEST\n  g_app->attach_storage(&storage::device::storage());\n#endif", 1), sim),
    }
    sim_extra = sim + ["app_.attach_storage(nullptr);"]
    cases["sim only"] = (app_h, main_cpp.replace("g_app->attach_storage(", "(void)(", 1), sim_extra)
    bad = 0
    for name, args in cases.items():
        if not audit(*args):
            print("self-test: '%s' was not caught" % name)
            bad += 1
    print("self-test: %d cases, %d not caught" % (len(cases), bad))
    return 1 if bad else 0


def main(argv):
    if len(argv) == 3 and argv[1] == "--self-test":
        return self_test(Path(argv[2]))
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = Path(argv[1])
    problems = audit(*read_tree(root))
    for p in problems:
        print("wiring drift: " + p)
    if not problems:
        app_h, main_cpp, sim = read_tree(root)
        print("wiring ok: %d hooks declared, the unit wires them in a normal build, the simulator's %d are a subset"
              % (len(app_hooks(app_h)), len(sim_hooks(sim))))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
