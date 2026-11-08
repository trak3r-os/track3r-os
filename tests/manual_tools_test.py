#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""OS-MANUALTOOLS: tools/manual_check.py (links, index, manifest, image files, TODOs, stale; the accessibility checks of OS-MANUAL2 M2-5), tools/manual_llms.py (--check),
tools/manual_images.py (--check --strict, with a fake simulator; OS-MANUALIMG) and tools/manual_diagrams.py (chord diagrams from fake
binding, boot chord and Power hold sources, --check; OS-MANUALDIAG) on a temporary repository.

usage: manual_tools_test.py <repository root>
"""
import contextlib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile


def load(root, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(root, "tools", name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def git(d, *args):
    return subprocess.run(["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
                          check=True, capture_output=True, text=True).stdout.strip()


def run(mod, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = mod.main(argv)
    return rc, out.getvalue() + err.getvalue()


CAT1 = "| ID | Function |\n|---|---|\n| QA-MIX-001 | one |\n| QA-MIX-002 | two |\n"
CAT2 = "| ID | Function |\n|---|---|\n| QA-MIX-001 | one, changed |\n| QA-MIX-002 | two |\n| QA-MIX-003 | three |\n"


# A stand-in for o3sim: `--scenario <file> --out <dir>` writes <dir>/<snap>.png for each `snap` line, `--gallery <dir>` writes
# file_typical_amiga.png; the bytes carry O3_FAKE_TWEAK, so a test can make a "render" differ from the committed picture.
FAKE_SIM = """#!/usr/bin/env python3
import os, sys
a = sys.argv[1:]
tweak = os.environ.get("O3_FAKE_TWEAK", "")
if a[0] == "--scenario":
    for line in open(a[1], encoding="utf-8"):
        w = line.split()
        if len(w) >= 2 and w[0] == "snap":
            open(os.path.join(a[3], w[1] + ".png"), "wb").write(("PNG:" + w[1] + tweak).encode())
elif a[0] == "--gallery":
    open(os.path.join(a[1], "file_typical_amiga.png"), "wb").write(("PNG:gallery" + tweak).encode())
else:
    sys.exit(2)
"""


def images_test(images, d, m):
    """tools/manual_images.py: write, then --check --strict against the committed pictures."""
    img = os.path.join(m, "img")
    write(os.path.join(d, "sim", "scenarios", "qa", "qa_mixer.txt"), "tap F1\nsnap qa_mix_001\n")
    write(os.path.join(d, "sim", "scenarios", "manual", "mixer_walk.txt"), "tap F2\nsnap walk_1\n")  # OS-MANUAL2 M2-4: a walk-through is <folder>/<scenario>
    write(os.path.join(m, "images.txt"),
          "# manifest\nmixer_a.png qa_mixer qa_mix_001\nfile_typical.png gallery file_typical\nlater.png qa_mixer qa_later # MISSING\nmixer_walk_1.png manual/mixer_walk walk_1\n")
    sim = os.path.join(d, "fake_o3sim")
    write(sim, FAKE_SIM)
    os.chmod(sim, 0o755)
    for name in ("mixer_a.png", "file_typical.png", "mixer_walk_1.png"):
        if os.path.exists(os.path.join(img, name)):
            os.remove(os.path.join(img, name))
    base = ["--root", d, "--sim", sim]
    try:
        rc, out = run(images, base + ["--check", "--strict"])
        assert rc == 1 and "3 not generated yet" in out and "not generated yet: mixer_a.png" in out and "not generated yet: mixer_walk_1.png" in out, out
        rc, out = run(images, base)
        assert rc == 0 and "wrote 3 images" in out and "1 marked MISSING" in out, out
        assert open(os.path.join(img, "mixer_a.png"), "rb").read() == b"PNG:qa_mix_001", out
        assert open(os.path.join(img, "mixer_walk_1.png"), "rb").read() == b"PNG:walk_1", out
        rc, out = run(images, base + ["--check", "--strict"])
        assert rc == 0 and "3 unchanged, 0 changed" in out, out
        os.environ["O3_FAKE_TWEAK"] = "+1"  # the simulator now renders other bytes: the committed pictures are stale
        rc, out = run(images, base + ["--check"])
        assert rc == 0 and "3 changed" in out and "changed: mixer_walk_1.png" in out, out
        rc, out = run(images, base + ["--check", "--strict"])
        assert rc == 1 and "commit docs/manual/img/" in out, out
    finally:
        os.environ.pop("O3_FAKE_TWEAK", None)
    write(os.path.join(img, "stray.png"), "x")  # a picture the manifest does not list
    rc, out = run(images, base + ["--check", "--strict"])
    assert rc == 1 and "not in the manifest: stray.png" in out, out
    os.remove(os.path.join(img, "stray.png"))
    rc, out = run(images, base + ["--check", "--strict"])
    assert rc == 0, out
    # a scenario of a folder that is not there, a scenario that is not in its folder, and every way out of sim/scenarios/ (a `..`, an absolute word, a .txt word outside the folder, a
    # symbolic link that leads out) are errors, never a picture
    write(os.path.join(d, "outside.txt"), "snap walk_1\n")
    link = os.path.join(d, "sim", "scenarios", "manual", "link_walk.txt")
    try:
        os.symlink(os.path.join(d, "outside.txt"), link)
    except OSError:
        link = None
    bad_words = ["nofolder/mixer_walk walk_1", "manual/qa_mixer walk_1", "manual/../qa/qa_mixer qa_mix_001", "../qa/qa_mixer qa_mix_001", "/etc/x walk_1", "manual/.. walk_1",
                 "qa/mixer_walk walk_1", "manual/ walk_1", "a/b/c walk_1", "outside.txt walk_1", "sim/scenarios/../../outside.txt walk_1",
                 os.path.join(d, "sim", "scenarios", "manual", "mixer_walk.txt") + " walk_1"]
    if link is not None:
        bad_words.append("manual/link_walk walk_1")
    for bad in bad_words:
        write(os.path.join(m, "images.txt"), "x.png " + bad + "\n")
        rc, out = run(images, base)
        assert rc == 1 and "x.png: no scenario" in out, (bad, rc, out)
        assert not os.path.exists(os.path.join(img, "x.png")), bad
    write(os.path.join(m, "images.txt"), "x.png manual/mixer_walk nosnap\n")  # the scenario is there, the snap is not
    rc, out = run(images, base)
    assert rc == 1 and "x.png: no snap" in out, (rc, out)
    if link is not None:
        os.remove(link)
    write(os.path.join(m, "images.txt"), "ok.png sim/scenarios/manual/mixer_walk.txt walk_1\n")  # a .txt word from the root is still a path
    rc, out = run(images, base)
    assert rc == 0 and open(os.path.join(img, "ok.png"), "rb").read() == b"PNG:walk_1", (rc, out)
    os.remove(os.path.join(img, "ok.png"))
    write(os.path.join(m, "images.txt"), "bad line\n")  # a malformed manifest is a usage error
    rc, out = run(images, base + ["--check", "--strict"])
    assert rc == 2, out


FAKE_BINDINGS = """constexpr core::ContextId kMixerCtx = context_id(Context::Mixer);
constexpr core::ActionId kMixerName = core::action_id("mixer.name");
constexpr core::ActionId kMixerMuteNone = core::action_id("mixer.mute_none");
constexpr core::ActionId kMixerSoloNone = core::action_id("mixer.solo_none");
constexpr core::InputRef kShift = button_ref(Button::Shift);
constexpr core::Binding kBase[] = {
    bind(kMixerCtx, tap(button_ref(Button::F1), kShift), kMixerName),
    bind(kMixerCtx, tap(button_ref(Button::F7), kShift), kMixerMuteNone, ArgFrom::Fixed, 0),
    bind(kMixerCtx, tap(button_ref(Button::F8), kShift), kMixerMuteNone, ArgFrom::Fixed, 1),
    bind(kMixerCtx, tap(button_ref(Button::F2), kShift), kMixerSoloNone, ArgFrom::Fixed, 0),
    bind(kMixerCtx, tap(button_ref(Button::F3), kShift), kMixerSoloNone, ArgFrom::Fixed, 1),
};
"""
FAKE_APP_MAIN = """constexpr unsigned kChordButtons[] = {static_cast<unsigned>(app::Button::Shift), static_cast<unsigned>(app::Button::F1),
                                      static_cast<unsigned>(app::Button::F8)};
constexpr unsigned kDiskChordKey = static_cast<unsigned>(app::Button::F7);
"""
FAKE_BOOT = "inline constexpr unsigned kBootChordKeys = 3;\ninline constexpr unsigned kBootDiskMask = 0x3u;\n"
FAKE_POWER = "inline constexpr std::uint32_t kHoldMs = 2000;\n"
ALT_NAME = "Panel: Shift is ringed, and F1, F7 and F8 are marked. Hold Shift, then press one of them."
DIAGRAMS = """# manifest
[chord_mixer.svg]
kind = chord
chapter = mixer.md
region = soft+edit
why = The fixture shows a Shift row on purpose.
alt = %s
caption = Mixer: hold Shift, then press F1 (NAME), F7 or F8.
bind = Mixer mixer.name | NAME
bind = Mixer mixer.mute_none 0 | UNMUTE
bind = Mixer mixer.mute_none 1 | UNSOLO

[chord_disk.svg]
kind = chord
chapter = mixer.md
region = edit+soft
extra = Held at power-up, before any key label exists.
alt = Panel: Shift, F1, F7 (DISK) and F8 (TEST) are ringed.
caption = At power-up: hold Shift and F1 with F7 (DISK) or F8 (TEST).
source = boot-chords

[chord_power.svg]
kind = chord
chapter = mixer.md
region = power
extra = The rear-panel button is on no part of the front.
alt = Panel: Power is ringed. Hold it for {hold_s} seconds.
caption = Hold Power for {hold_s} s.
source = power-hold
""" % ALT_NAME


def diagrams_test(root):
    """tools/manual_diagrams.py on a fake tree: keys from the bindings and the boot constants, then every way --check fails."""
    dg = load(root, "manual_diagrams")
    with tempfile.TemporaryDirectory() as d:
        m = os.path.join(d, "docs", "manual")
        for rel in ("docs/manual/img/panel.svg", "app/include/o3os/app/buttons.h"):
            write(os.path.join(d, rel), open(os.path.join(root, rel), encoding="utf-8").read())
        write(os.path.join(d, "app/src/bindings.cpp"), FAKE_BINDINGS)
        write(os.path.join(d, "platform/mk66/src/app_main.cpp"), FAKE_APP_MAIN)
        write(os.path.join(d, "platform/mk66/include/o3os/platform/mk66/boot_chord.h"), FAKE_BOOT)
        write(os.path.join(d, "core/include/o3os/core/power_hold.h"), FAKE_POWER)
        write(os.path.join(m, "diagrams.txt"), DIAGRAMS)
        chapter = ("# Mixer\n\n![%s](img/chord_mixer.svg)\n\n![Panel: Shift, F1, F7 (DISK) and F8 (TEST) are ringed.](img/chord_disk.svg)\n\n"
                   "![Panel: Power is ringed. Hold it for 2 seconds.](img/chord_power.svg)\n\nThe NAME, UNMUTE and UNSOLO keys. The DISK and TEST keys.\n") % ALT_NAME
        write(os.path.join(m, "mixer.md"), chapter)
        base = ["--root", d]
        img = os.path.join(m, "img")

        rc, out = run(dg, base + ["--check"])
        assert rc == 1 and "missing" in out, out
        rc, out = run(dg, base)
        assert rc == 0 and "wrote 3 diagrams" in out, out
        rc, out = run(dg, base + ["--check"])
        assert rc == 0 and "0 problem(s)" in out, out
        # The keys come from the bindings: Shift is held (ring), F1, F7, F8 are pressed (filled), under F1 the label; the boot chords are rings only,
        # F7 labelled DISK (the disk key) and F8 TEST (the last key of kChordButtons).
        mixer = open(os.path.join(img, "chord_mixer.svg"), encoding="utf-8").read()
        assert mixer.count('class="soft p"') == 3 and mixer.count('class="key h"') == 1 and ">NAME</text>" in mixer and ">UNSOLO</text>" in mixer, mixer
        disk = open(os.path.join(img, "chord_disk.svg"), encoding="utf-8").read()
        assert disk.count('class="soft h"') == 3 and disk.count('class="key h"') == 1 and ' p"' not in disk, disk
        assert ">DISK</text>" in disk and ">TEST</text>" in disk, disk
        power = open(os.path.join(img, "chord_power.svg"), encoding="utf-8").read()
        assert "Hold Power for 2 s." in power and "rear panel" in power and "{hold_s}" not in power, power
        assert len(mixer) < dg.MAX_BYTES < 12000, len(mixer)
        # A figure is a crop: its viewBox is not the whole panel, the parts outside it are not drawn (no pad grid, no encoder, no screen label),
        # and the legend is in a band under it.
        for svg in (mixer, disk, power):
            assert 'viewBox="0 0 1360 960"' not in svg and 'id="pd"' not in svg and "ENCODER" not in svg and "SCREEN 800" not in svg, svg
        assert "F1" not in power and 'class="soft' not in power and "hold down" in power, power
        # The keys of the boot chord (Shift and F1..F8) are further apart than MAX_FIG_WIDTH: one pane for each region, in the order listed (edit,
        # then soft), the figure no wider than the limit; the Power figure is one pane, and an encoder is never drawn cut.
        assert disk.count("<svg ") == 3 and mixer.count("<svg ") == 3 and power.count("<svg ") == 2, (disk, power)
        assert 'width="%d" height=' % dg.MAX_FIG_WIDTH in disk and disk.index('class="key h"') < disk.index('class="soft h"'), disk

        # --list pastes the chapter's line.
        rc, out = run(dg, base + ["--list"])
        assert rc == 0 and "![Panel: Power is ringed. Hold it for 2 seconds.](img/chord_power.svg)" in out, out

        def fails(text, *what):
            rc, out = run(dg, base + ["--check"])
            assert rc != 0 and all(w in out for w in what), (what, out)

        orig = open(os.path.join(img, "chord_mixer.svg"), encoding="utf-8").read()
        write(os.path.join(img, "chord_mixer.svg"), orig + " ")
        fails("stale", "chord_mixer.svg: stale")
        write(os.path.join(img, "chord_mixer.svg"), orig)
        os.remove(os.path.join(img, "chord_disk.svg"))
        fails("missing", "chord_disk.svg: missing")
        run(dg, base)
        write(os.path.join(img, "other.svg"), "<svg/>")
        fails("unlisted", "other.svg: in docs/manual/img/ but not in")
        os.remove(os.path.join(img, "other.svg"))
        write(os.path.join(m, "mixer.md"), chapter.replace("img/chord_power.svg", "img/chord_pow.svg"))
        fails("not in the chapter", "chord_power.svg: mixer.md does not contain the line")
        write(os.path.join(m, "mixer.md"), chapter.replace("The NAME, UNMUTE and UNSOLO keys.", "Nothing."))
        fails("label not in the chapter", "the label `UNMUTE` does not appear in mixer.md")
        write(os.path.join(m, "mixer.md"), chapter)
        rc, out = run(dg, base + ["--check"])
        assert rc == 0, out

        # A binding that moves or goes: the diagram cannot outlive it.
        write(os.path.join(d, "app/src/bindings.cpp"), FAKE_BINDINGS.replace("kMixerName),", "kMixerMuteNone, ArgFrom::Fixed, 5),"))
        fails("moved", "chord_mixer.svg", "no row of app/src/bindings.cpp binds mixer.name")
        write(os.path.join(d, "app/src/bindings.cpp"), FAKE_BINDINGS.replace("ArgFrom::Fixed, 1", "ArgFrom::Fixed, 0"))
        fails("ambiguous", "2 rows of app/src/bindings.cpp bind mixer.mute_none", "add the argument or `via <Button>`")
        write(os.path.join(d, "app/src/bindings.cpp"), FAKE_BINDINGS)
        # A boot chord that is not what the script reads, a long alt text, an unknown source.
        write(os.path.join(d, "platform/mk66/include/o3os/platform/mk66/boot_chord.h"), FAKE_BOOT.replace("0x3u", "0x5u"))
        fails("boot", "chord_disk.svg", "boot chord constants")
        write(os.path.join(d, "platform/mk66/include/o3os/platform/mk66/boot_chord.h"), FAKE_BOOT)
        write(os.path.join(m, "diagrams.txt"), DIAGRAMS.replace("alt = Panel: Shift, F1, F7 (DISK) and F8 (TEST) are ringed.", "alt = " + "word " * 26))
        fails("long alt", "chord_disk.svg", "26 words, at most 25")
        write(os.path.join(m, "diagrams.txt"), DIAGRAMS.replace("source = power-hold", "source = flying"))
        fails("source", "unknown source `flying`")
        write(os.path.join(m, "diagrams.txt"), DIAGRAMS.replace("[chord_power.svg]", "[chord_disk.svg]"))
        rc, out = run(dg, base + ["--check"])
        assert rc == 2 and "twice" in out, out

        # The rules of OS-MANUALFIX: a Shift row needs `why`, a chord figure after the first of a chapter needs `extra`, `region` is needed and true,
        # at most four keys are marked (keys that share a label count once).
        def manifest_fails(text, *what):
            write(os.path.join(m, "diagrams.txt"), text)
            rc, out = run(dg, base + ["--check"])
            assert rc != 0 and all(w in out for w in what), (what, out)

        manifest_fails(DIAGRAMS.replace("why = The fixture shows a Shift row on purpose.\n", ""), "chord_mixer.svg", "shows only Shift + F1..F8", "`why =")
        manifest_fails(DIAGRAMS.replace("extra = Held at power-up, before any key label exists.\n", ""), "chord_disk.svg", "add `extra =")
        manifest_fails(DIAGRAMS.replace("region = power\n", ""), "chord_power.svg", "has no `region`")
        manifest_fails(DIAGRAMS.replace("region = power", "region = edit"), "chord_power.svg", "the marked keys are in power")
        manifest_fails(DIAGRAMS.replace("region = edit+soft\nextra", "region = soft\nextra"), "chord_disk.svg", "the marked keys are in edit+soft")
        manifest_fails(DIAGRAMS.replace("region = power", "region = rear"), "chord_power.svg", "expected soft, nav, edit or power")
        many = """
[chord_many.svg]
kind = chord
chapter = mixer.md
region = soft+edit
why = Too many keys.
alt = Panel: many keys.
caption = Many keys.
bind = Mixer mixer.name | A
bind = Mixer mixer.mute_none 0 | B
bind = Mixer mixer.mute_none 1 | C
bind = Mixer mixer.solo_none 0 | D
"""
        manifest_fails(DIAGRAMS + many, "chord_many.svg", "marks 5 keys, at most 4")
        # (the same keys under one label count once: the four arrows of a block)
        write(os.path.join(m, "diagrams.txt"), DIAGRAMS + many.replace("| A", "| ROW").replace("| B", "| ROW").replace("| C", "| ROW").replace("| D", "| ROW"))
        rc, out = run(dg, base)
        assert rc == 0 and "wrote 4 diagrams" in out, out
        os.remove(os.path.join(img, "chord_many.svg"))

        manifest_fails(DIAGRAMS.replace("region = edit+soft\nextra", "region = soft+soft\nextra"), "chord_disk.svg", "expected soft, nav, edit or power, each once")
        # An oversize SVG: --check says so (the limit is lowered for the call).
        write(os.path.join(m, "diagrams.txt"), DIAGRAMS)
        run(dg, base)
        limit, dg.MAX_BYTES = dg.MAX_BYTES, 1000
        try:
            rc, out = run(dg, base + ["--check"])
        finally:
            dg.MAX_BYTES = limit
        assert rc != 0 and "chord_disk.svg: " in out and "bytes, more than 1000" in out, out

        # manual_check knows the stanzas: a chapter's SVG is in the manifest, and a missing file is a problem.
        write(os.path.join(m, "diagrams.txt"), DIAGRAMS)
        write(os.path.join(m, "index.md"), "[Mixer](mixer.md)\n")
        write(os.path.join(m, "images.txt"), "# none\n")
        mc = load(root, "manual_check")
        os.remove(os.path.join(img, "chord_disk.svg"))
        rc, out = run(mc, ["--root", d, "--strict"])
        assert rc == 1 and "image file missing" in out and "chord_disk.svg" in out, out
        run(dg, base)
        rc, out = run(mc, ["--root", d, "--strict"])
        assert rc == 0, out


CLEAN_PAGE = """# Page

## Section

![Mixer, MASTER FX page: the limiter card says ALWAYS ON.](img/mixer_a.png)

### Detail

| Key | What it does |
|---|---|
| `F1` | opens [the Mixer](index.md) |
| a \\| b | escaped pipe |

[Index](index.md), `https://example.invalid/in-code`, [the site](https://example.invalid/x).

[Link](index.md) is a name, not a generic link text. <img src="https://example.invalid/pic.png" alt="Mixer, the MASTER FX page."> has a URL in its tag.

![word word word word word word word word word word word word word word word word word word word word word word word word](img/mixer_a.png)

<!-- ![](hidden.png) and [here](x.md) in a comment do not count -->

```
![](in a fence)
```
"""

# (what the page gets, the message that must appear) for OS-MANUAL2 M2-5: each fault of rule D11 that the tool can see.
A11Y_FAULTS = (
    ("![](img/mixer_a.png)", "image with no alt text: img/mixer_a.png"),
    ("![   ](img/mixer_a.png)", "image with no alt text"),
    ("![mixer_a.png](img/mixer_a.png)", "alt text is a file name"),
    ("![Screenshot of the mixer.](img/mixer_a.png)", 'alt text starts with "Screenshot of"'),
    ("![%s](img/mixer_a.png)" % " ".join(["word"] * 25), "alt text is 25 words, keep it under 25"),
    ("![%s](img/mixer_a.png)" % " ".join(["word"] * 26), "alt text is 26 words, keep it under 25"),
    ('<img src="img/mixer_a.png" alt="%s">' % " ".join(["word"] * 25), "<img> alt text is 25 words"),
    ("See [link](index.md).", 'link text "link" does not name its target (index.md)'),
    ('<img src="img/mixer_a.png">', "<img> no alt text"),
    ("##### Skipped\n", "heading level skips from `###` to `#####`"),
    ("# A second title\n", "a second `#` heading"),
    ("##\n", "empty heading"),
    ("| a | b |\n| 1 | 2 |\n", "table with no header separator row"),
    ("| a | |\n|---|---|\n| 1 | 2 |\n", "table with an empty header cell"),
    ("| a | b |\n|---|\n| 1 | 2 |\n", "the header separator has 1 cells, the header 2"),
    ("See [here](index.md).", 'link text "here" does not name its target (index.md)'),
    ("See [Click here](index.md).", 'link text "Click here"'),
    ("See [this page](index.md).", 'link text "this page"'),
    ("See [link](index.md) and [this](index.md).", 'link text "this"'),
    ("See [https://example.invalid/x](https://example.invalid/x).", "link text is a bare URL"),
    ("Visit https://example.invalid/x now.", "a URL left in the text"),
    ("See <https://example.invalid/x>.", "a URL left in the text"),
    ("See [](index.md).", "link with no text: index.md"),
)


def a11y_test(check):
    """tools/manual_check.py accessibility checks (OS-MANUAL2 M2-5) on a small manual of its own: a clean page passes --strict; each planted fault is named
    with its line and fails --strict (and only reports without it)."""
    with tempfile.TemporaryDirectory() as d:
        m = os.path.join(d, "docs", "manual")
        write(os.path.join(m, "index.md"), "# Index\n\n[Page](page.md)\n")
        write(os.path.join(m, "images.txt"), "# manifest\nmixer_a.png qa_mixer qa_mix_001\n")
        write(os.path.join(m, "img", "mixer_a.png"), "png")
        write(os.path.join(m, "page.md"), CLEAN_PAGE)
        rc, out = run(check, ["--root", d, "--strict"])
        assert rc == 0 and "accessibility" not in out, out
        assert check.a11y_problems(CLEAN_PAGE) == [], check.a11y_problems(CLEAN_PAGE)
        for fault, message in A11Y_FAULTS:
            page = CLEAN_PAGE + "\n" + fault + "\n"
            write(os.path.join(m, "page.md"), page)
            rc, out = run(check, ["--root", d, "--strict"])
            assert rc == 1 and "page.md: " in out and "accessibility problem(s)" in out and message in out, (fault, rc, out)
            rc, out = run(check, ["--root", d])
            assert rc == 0 and message in out, (fault, rc, out)
        # The line is the line of the file: a comment above does not move it.
        write(os.path.join(m, "page.md"), "<!-- one\ntwo -->\n# Page\n\n### Skipped\n")
        rc, out = run(check, ["--root", d, "--strict"])
        assert rc == 1 and "page.md:5: heading level skips from `#` to `###`: Skipped" in out, out
        # The index and the template are pages of the manual too.
        write(os.path.join(m, "page.md"), CLEAN_PAGE)
        write(os.path.join(m, "chapter-template.md"), "## Starts with level two\n")
        rc, out = run(check, ["--root", d, "--strict"])
        assert rc == 1 and "chapter-template.md: 1 accessibility problem(s)" in out and "the first heading is `##`" in out, out
        os.remove(os.path.join(m, "chapter-template.md"))
        write(os.path.join(m, "index.md"), "# Index\n\n[here](page.md)\n")
        rc, out = run(check, ["--root", d, "--strict"])
        assert rc == 1 and "index.md: 1 accessibility problem(s)" in out and 'link text "here"' in out, out
        write(os.path.join(m, "index.md"), "# Index\n\n[Page](page.md)\n")
        # Colour words are for a reviewer: listed with -v, never a failure.
        write(os.path.join(m, "page.md"), CLEAN_PAGE + "\nThe red badge shows. | red | in a table |\n")
        rc, out = run(check, ["--root", d, "--strict", "-v"])
        assert rc == 0 and "page.md: 1 line(s) name a colour" in out and "The red badge shows." in out, out


SYNC_CHAPTER = """<!-- manual-sync: SHA -->
# Mixer

## How it works
<!-- sync: src=app/src/x.cpp spec=docs/specs/S.md qa=QA-MIX-001 -->

The Mixer sets levels. The limiter is always on. <!-- QA-MIX-002 -->

## Try it
<!-- sync: src=app/lib qa=QA-MIX-002 -->

1. Press `F1`.

## See also

Nothing.
"""


def sync_test(check):
    """tools/manual_check.py sync v2 (OS-MANUAL2 M2-3) on a repository with a dev branch: markers and the lock, --ack, the branch gate, --stale with removed
    rows, and the chapters that have no catalogue file."""
    with tempfile.TemporaryDirectory() as d:
        m = os.path.join(d, "docs", "manual")
        git(d, "init", "-q")
        git(d, "checkout", "-q", "-b", "dev")
        write(os.path.join(d, "app/src/x.cpp"), "one\n")
        write(os.path.join(d, "app/lib/a.c"), "a1\n")
        write(os.path.join(d, "docs/specs/S.md"), "spec\n")
        write(os.path.join(d, "docs/qa/catalogue/mixer.md"), "| ID | Function | How | Expected | Status |\n|---|---|---|---|---|\n| QA-MIX-001 | one | F1 | a | sim PASS |\n| QA-MIX-002 | two | F2 | b | sim PASS |\n")
        write(os.path.join(m, "index.md"), "# Index\n\n[Mixer](mixer.md), [Glossary](glossary.md)\n")
        write(os.path.join(m, "images.txt"), "# manifest\n")
        write(os.path.join(m, "glossary.md"), "# Glossary\n\nNo marker needed.\n")
        write(os.path.join(m, "mixer.md"), SYNC_CHAPTER.replace("SHA", "0000000"))
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "one")
        sha = git(d, "rev-parse", "--short", "HEAD")
        write(os.path.join(m, "mixer.md"), SYNC_CHAPTER.replace("SHA", sha))
        chapter = SYNC_CHAPTER.replace("SHA", sha)
        base = ["--root", d]

        # No lock yet: the marked sections are not acknowledged. --ack-all writes it once; the second time it refuses.
        rc, out = run(check, base + ["--strict"])
        assert rc == 1 and "sync.lock is missing" in out, out
        rc, out = run(check, base + ["--ack-all"])
        assert rc == 0 and "2 section(s) acknowledged" in out, out
        lock = open(os.path.join(m, "sync.lock"), encoding="utf-8").read()
        assert "mixer.md#how-it-works QA-MIX-001=" in lock and " app/src/x.cpp=" in lock and " docs/specs/S.md=" in lock and "mixer.md#try-it QA-MIX-002=" in lock and " app/lib=" in lock, lock
        assert "glossary" not in lock, lock
        rc, out = run(check, base + ["--strict"])
        assert rc == 0 and "0 problem(s)" in out, out
        rc, out = run(check, base + ["--ack-all"])  # it writes the lock again from the tree: here nothing differs
        assert rc == 0 and "2 section(s) acknowledged, 0 line(s) new or different" in out and open(os.path.join(m, "sync.lock"), encoding="utf-8").read() == lock, out
        rc, out = run(check, base + ["--ack", "mixer.md#how-it-works"])
        assert rc == 0 and open(os.path.join(m, "sync.lock"), encoding="utf-8").read() == lock, out  # an ack of what is acknowledged changes nothing
        rc, out = run(check, base + ["--ack", "mixer.md#nowhere"])
        assert rc == 1 and "no section with a valid `sync:` marker (this chapter's: how-it-works, try-it)" in out, out

        # The old form is refused; every fault of a marker is named with its section.
        for what, text, message in (
                ("old src comment", chapter.replace("The Mixer sets", "<!-- src: app/src/x.cpp (note) -->\nThe Mixer sets"), "mixer.md:%d: old `<!-- src: ... -->` comment"),
                ("missing file", chapter.replace("src=app/src/x.cpp", "src=app/src/y.cpp"), "src=app/src/y.cpp does not exist"),
                ("missing row", chapter.replace("qa=QA-MIX-001", "qa=QA-MIX-009"), "qa=QA-MIX-009 is in no catalogue file"),
                ("bad field", chapter.replace("spec=docs/specs/S.md", "dir=docs"), "`dir=docs` is not src=, spec= or qa="),
                ("outside the repository", chapter.replace("src=app/src/x.cpp", "src=../x.cpp"), "is not a path relative to the repository"),
                ("two markers", chapter.replace("The Mixer sets", "<!-- sync: src=app/src/x.cpp -->\nThe Mixer sets"), "2 `sync:` markers, one per section"),
                ("Try it without a marker", chapter.replace("<!-- sync: src=app/lib qa=QA-MIX-002 -->\n", ""), 'a "Try it" section states behaviour and needs a'),
                ("marker before a heading", "<!-- sync: src=app/src/x.cpp -->\n" + chapter, "belongs to no section")):
            write(os.path.join(m, "mixer.md"), text)
            rc, out = run(check, base + ["--strict"])
            assert rc == 1 and (message % 7 if "%d" in message else message) in out, (what, out)
        write(os.path.join(m, "mixer.md"), chapter)
        # A marker's QA row that was removed, and a section whose marker went, are named too.
        write(os.path.join(m, "mixer.md"), chapter.replace("## See also", "## Extra\n<!-- sync: src=app/src/x.cpp -->\n\n## See also"))
        rc, out = run(check, base + ["--strict"])
        assert rc == 1 and "mixer.md#extra has no line in" in out and "--ack mixer.md#extra" in out, out
        rc, out = run(check, base + ["--ack", "mixer.md#extra"])
        assert rc == 0, out
        write(os.path.join(m, "mixer.md"), chapter)
        rc, out = run(check, base + ["--strict"])
        assert rc == 1 and "mixer.md#extra has a line in" in out, out
        rc, out = run(check, base + ["--ack", "mixer.md#extra"])
        assert rc == 0 and "dropped mixer.md#extra" in out, out
        rc, out = run(check, base + ["--strict"])
        assert rc == 0, out
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "two")

        # On dev the gate only warns, whatever moved.
        write(os.path.join(d, "app/src/x.cpp"), "two\n")
        rc, out = run(check, base + ["--gate"])
        assert rc == 0 and "warning only (HEAD is dev)" in out and "WARN mixer.md#how-it-works" in out and "app/src/x.cpp (changed)" in out, out
        rc, out = run(check, base + ["--stale"])
        assert "mixer.md#how-it-works: app/src/x.cpp (changed)" in out, out
        git(d, "checkout", "-q", "--", "app/src/x.cpp")

        # On a branch it fails for a cited file that changed there, until the section or its lock line changes.
        git(d, "checkout", "-q", "-b", "feat/x")
        rc, out = run(check, base + ["--gate"])
        assert rc == 0 and "2 section(s) with a marker, 0 not followed" in out, out
        write(os.path.join(d, "app/src/x.cpp"), "two\n")
        git(d, "commit", "-q", "-am", "x changes")
        rc, out = run(check, base + ["--gate"])
        assert rc == 1 and "FAIL mixer.md#how-it-works cites app/src/x.cpp" in out and "\n  python3 tools/manual_check.py --ack-path app/src/x.cpp\n" in out, out
        assert "try-it" not in out, out
        rc, out = run(check, base + ["--ack", "mixer.md#how-it-works"])
        rc, out = run(check, base + ["--gate"])
        assert rc == 0 and "0 not followed" in out, out  # the lock line changed on the branch
        git(d, "checkout", "-q", "--", "docs/manual/sync.lock")
        write(os.path.join(m, "mixer.md"), chapter.replace("The limiter is always on.", "The limiter is always on, at the end of the master chain."))
        rc, out = run(check, base + ["--gate"])
        assert rc == 0 and "0 not followed" in out, out  # the text changed on the branch
        write(os.path.join(m, "mixer.md"), chapter)
        # The base can be given: against the branch's own tip nothing changed.
        os.environ["O3OS_MANUAL_BASE"] = "HEAD"
        try:
            rc, out = run(check, base + ["--gate"])
            assert rc == 0 and "0 not followed" in out, out
        finally:
            os.environ.pop("O3OS_MANUAL_BASE", None)
        rc, out = run(check, base + ["--gate", "--base", "nonsense"])  # a base that was named and is wrong fails: it must not skip the gate
        assert rc == 1 and "manual gate: FAIL: O3OS_MANUAL_BASE / --base nonsense is not a commit" in out, out
        os.environ["O3OS_MANUAL_BASE"] = "origin/nowhere"
        try:
            rc, out = run(check, base + ["--gate"])
            assert rc == 1 and "FAIL: O3OS_MANUAL_BASE / --base origin/nowhere is not a commit" in out, out
        finally:
            os.environ.pop("O3OS_MANUAL_BASE", None)
        # A cited folder changes with a file in it; a cited QA row with its Expected cell, not with its Status.
        write(os.path.join(d, "app/lib/a.c"), "a2\n")
        git(d, "commit", "-q", "-am", "a changes")
        rc, out = run(check, base + ["--gate"])
        assert rc == 1 and "FAIL mixer.md#try-it cites app/lib" in out, out
        git(d, "checkout", "-q", "dev")
        git(d, "checkout", "-q", "-b", "feat/y")
        cat = os.path.join(d, "docs/qa/catalogue/mixer.md")
        text = open(cat, encoding="utf-8").read()
        write(cat, text.replace("sim PASS", "sim PASS · unit PASS"))
        git(d, "commit", "-q", "-am", "status only")
        rc, out = run(check, base + ["--gate"])
        assert rc == 0 and "0 not followed" in out, out
        write(cat, text.replace("| a |", "| a, changed |"))
        git(d, "commit", "-q", "-am", "expected changes")
        rc, out = run(check, base + ["--gate"])
        assert rc == 1 and "FAIL mixer.md#how-it-works cites QA-MIX-001" in out, out
        git(d, "checkout", "-q", "dev")

        # --stale: a removed row is found, and a chapter that cites it is told.
        write(cat, text.replace("| QA-MIX-002 | two | F2 | b | sim PASS |\n", ""))
        git(d, "commit", "-q", "-am", "row removed")
        rc, out = run(check, base + ["--stale"])
        assert "mixer.md (since %s, catalogue mixer): 1: QA-MIX-002" % sha in out and "0 added, 0 changed, 1 removed" in out, out
        assert "cited by the chapter, removed from the catalogue: QA-MIX-002" in out, out
        rc, out = run(check, base + ["--stale", "--strict"])
        assert rc == 1 and "mixer.md#how-it-works" not in out.split("sections whose")[0], out
        assert "qa=QA-MIX-002 is in no catalogue file" in out, out

        # The chapters that follow no single catalogue file: cheat sheet = all, quick start = global, the others by src only or exempt.
        assert check.catalogue_files(d, "cheat_sheet.md") == ["mixer"] and check.catalogue_files(d, "quick_start.md") == ["global"], check.catalogue_files(d, "cheat_sheet.md")
        assert check.catalogue_files(d, "install.md") == [] and check.catalogue_files(d, "computer_tools.md") == [] and check.catalogue_files(d, "glossary.md") is None
        for name in ("cheat_sheet.md", "quick_start.md", "install.md", "computer_tools.md"):
            write(os.path.join(m, name), ("<!-- manual-sync: %s -->\n" % sha if name in ("install.md", "computer_tools.md") else "") + "# %s\n\nText.\n" % name)
        write(os.path.join(m, "index.md"), "# Index\n\n[Mixer](mixer.md), [Glossary](glossary.md), cheat_sheet.md, quick_start.md, install.md, computer_tools.md\n")
        rc, out = run(check, base + ["--stale"])
        assert "cheat_sheet.md: no `<!-- manual-sync" in out and "quick_start.md: no `<!-- manual-sync" in out, out
        assert "install.md: no" not in out and "computer_tools.md: no" not in out and "glossary.md: no" not in out, out
        assert "exempt (information): glossary.md: exempt: a list of terms" in out, out


LOCK_CHAPTER = """# Mixer

## First
<!-- sync: src=app/src/x.cpp,docs/specs/S.md -->

Text.

## Second
<!-- sync: src=app/src/x.cpp -->

Text.

## Third
<!-- sync: src=app/src/y.cpp,app/lib qa=QA-MIX-001 -->

Text.
"""
PAGE_WITH_TABLE = "# Panel\n\nThe keys.\n\n<!-- manual-tables: keys screen=mixer -->\nThe key row.\n\n| Key | Label |\n|---|---|\n| `F1` | **MIXER** |\n\n<!-- /manual-tables -->\n\n<!-- manual-tables: hand -->\nA hand cell.\n"
CITING_CHAPTER = "# Cheat\n\n## Screens\n<!-- sync: src=docs/manual/panel.md -->\n\nText.\n"


def ack_path_test(check):
    """tools/manual_check.py --ack-path (one command for every section that cites a file), the command the gate prints, a conflict in sync.lock, and the
    hash of a manual page without its generated tables."""
    import shlex
    with tempfile.TemporaryDirectory() as d:
        m = os.path.join(d, "docs", "manual")
        lockfile = os.path.join(m, "sync.lock")
        git(d, "init", "-q")
        git(d, "checkout", "-q", "-b", "dev")
        for rel, text in (("app/src/x.cpp", "x1\n"), ("app/src/y.cpp", "y1\n"), ("app/lib/a.c", "a1\n"), ("docs/specs/S.md", "spec\n"),
                          ("docs/qa/catalogue/mixer.md", "| ID | Function | How | Expected | Status |\n|---|---|---|---|---|\n| QA-MIX-001 | one | F1 | a | sim PASS |\n"),
                          ("docs/manual/index.md", "# Index\n\n[Mixer](mixer.md), [Panel](panel.md), [Cheat](cheat.md)\n"), ("docs/manual/images.txt", "# manifest\n"),
                          ("docs/manual/mixer.md", LOCK_CHAPTER), ("docs/manual/panel.md", PAGE_WITH_TABLE), ("docs/manual/cheat.md", CITING_CHAPTER)):
            write(os.path.join(d, rel), text)
        base = ["--root", d]
        rc, out = run(check, base + ["--ack-all"])
        assert rc == 0 and "4 section(s) acknowledged" in out, out
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "one")
        git(d, "checkout", "-q", "-b", "feat/many")

        # Two sections cite x.cpp, one of them also the spec: the gate lists both and prints ONE command for everything the branch changed.
        write(os.path.join(d, "app/src/x.cpp"), "x2\n")
        write(os.path.join(d, "docs/specs/S.md"), "spec 2\n")
        git(d, "commit", "-q", "-am", "x and the spec change")
        rc, out = run(check, base + ["--gate"])
        assert rc == 1 and "2 section(s) cite something this branch changed" in out, out
        assert "FAIL mixer.md#first cites app/src/x.cpp, docs/specs/S.md" in out and "FAIL mixer.md#second cites app/src/x.cpp" in out and "third" not in out, out
        cmd = [l.strip() for l in out.splitlines() if l.strip().startswith("python3 tools/manual_check.py --ack-path")]
        assert cmd == ["python3 tools/manual_check.py --ack-path app/src/x.cpp docs/specs/S.md"], out
        before = open(lockfile, encoding="utf-8").read()
        # The printed command is ready to run as it stands, and it clears the gate.
        rc, out = run(check, base + shlex.split(cmd[0])[2:])
        assert rc == 0 and "acknowledged app/src/x.cpp in 2 section(s)" in out and "acknowledged docs/specs/S.md in 1 section(s)" in out, out
        after = open(lockfile, encoding="utf-8").read()
        assert [l for l in before.splitlines() if "mixer.md#third" in l] == [l for l in after.splitlines() if "mixer.md#third" in l], after
        assert len([1 for a, b in zip(before.splitlines(), after.splitlines()) if a != b]) == 2, after
        rc, out = run(check, base + ["--gate"])
        assert rc == 0 and "0 not followed" in out, out
        rc, out = run(check, base + ["--strict"])
        assert rc == 0, out
        # Only the named item moves in a line: acknowledge x.cpp alone after the spec changed again, and the spec still shows as moved.
        write(os.path.join(d, "docs/specs/S.md"), "spec 3\n")
        write(os.path.join(d, "app/src/x.cpp"), "x3\n")
        rc, out = run(check, base + ["--ack-path", "app/src/x.cpp"])
        assert rc == 0, out
        rc, out = run(check, base + ["--stale"])
        assert "mixer.md#first: docs/specs/S.md (changed)" in out and "mixer.md#second" not in out.split("sections whose")[1].split("catalogue IDs")[0], out
        rc, out = run(check, base + ["--ack", "mixer.md#first"])
        assert rc == 0, out
        git(d, "commit", "-q", "-am", "more")
        # A folder, a file inside it and a QA row: the section that cites the folder is acknowledged by a path in it.
        write(os.path.join(d, "app/lib/a.c"), "a2\n")
        rc, out = run(check, base + ["--ack-path", "app/lib/a.c"])
        assert rc == 0 and "acknowledged app/lib/a.c in 1 section(s): mixer.md#third" in out, out
        rc, out = run(check, base + ["--ack-path", "QA-MIX-001"])
        assert rc == 0 and "acknowledged QA-MIX-001 in 1 section(s)" in out, out
        # Wrong requests change nothing: an item nobody cites, and a section with no line yet.
        saved = open(lockfile, encoding="utf-8").read()
        rc, out = run(check, base + ["--ack-path", "app/src/z.cpp"])
        assert rc == 1 and "no marked section cites app/src/z.cpp" in out and open(lockfile, encoding="utf-8").read() == saved, out
        write(os.path.join(m, "mixer.md"), LOCK_CHAPTER + "\n## Fourth\n<!-- sync: src=app/src/x.cpp -->\n\nText.\n")
        rc, out = run(check, base + ["--ack-path", "app/src/x.cpp"])
        assert rc == 1 and "mixer.md#fourth has no line in docs/manual/sync.lock yet" in out and "nothing written" in out and open(lockfile, encoding="utf-8").read() == saved, out
        write(os.path.join(m, "mixer.md"), LOCK_CHAPTER)
        git(d, "commit", "-q", "-am", "state")

        # A merge conflict in the lock: named by --strict, the gate and --ack-path with the way out; --ack-all writes the lock again from the tree.
        good = open(lockfile, encoding="utf-8").read()
        lines = good.splitlines()
        first = next(i for i, l in enumerate(lines) if l.startswith("mixer.md#first"))
        with open(lockfile, "w", encoding="utf-8") as f:
            f.write("\n".join(lines[:first] + ["<<<<<<< HEAD", lines[first], "=======", lines[first].replace("=", "=0"), ">>>>>>> other"] + lines[first + 1:]) + "\n")
        rc, out = run(check, base + ["--strict"])
        assert rc == 1 and "sync.lock has merge conflict markers: take either side" in out and "--ack-path <file>" in out, out
        rc, out = run(check, base + ["--gate"])
        assert rc == 1 and "manual gate: FAIL: docs/manual/sync.lock has merge conflict markers" in out, out
        rc, out = run(check, base + ["--ack-path", "app/src/x.cpp"])
        assert rc == 1 and "merge conflict markers" in out, out
        rc, out = run(check, base + ["--ack-all"])
        assert rc == 0 and "4 section(s) acknowledged" in out, out
        assert open(lockfile, encoding="utf-8").read() == good
        rc, out = run(check, base + ["--strict"])
        assert rc == 0, out

        # A manual page is hashed without its generated tables: a renamed key changes the page, not what a section that cites the page follows.
        h0 = check.path_hash(d, "docs/manual/panel.md")
        panel = open(os.path.join(m, "panel.md"), encoding="utf-8").read()
        write(os.path.join(m, "panel.md"), panel.replace("**MIXER**", "**MIXER 2**"))
        assert check.path_hash(d, "docs/manual/panel.md") == h0
        rc, out = run(check, base + ["--ack-all"])
        assert rc == 0 and "0 line(s) new or different" in out, out
        git(d, "commit", "-q", "-am", "a generated table changes")
        rc, out = run(check, base + ["--gate"])
        assert rc == 0 and "0 not followed" in out, out
        write(os.path.join(m, "panel.md"), panel.replace("The keys.", "The keys, all of them."))
        assert check.path_hash(d, "docs/manual/panel.md") != h0
        git(d, "commit", "-q", "-am", "the text changes")
        rc, out = run(check, base + ["--gate"])
        assert rc == 1 and "FAIL cheat.md#screens cites docs/manual/panel.md" in out and "--ack-path docs/manual/panel.md" in out, out
        # (the hand note is outside the generated text, so it counts)
        assert check.digest("docs/manual/p.md", b"a\n<!-- manual-tables: hand -->\nb\n") != check.digest("docs/manual/p.md", b"a\n<!-- manual-tables: hand -->\nc\n")
        assert check.digest("docs/other/p.md", panel.encode()) != h0
        # (the generator's markers, tools/manual_tables.py: a generated block runs from its opening line to `<!-- /manual-tables -->` and its text is not hashed; a `hand` note has no closing marker, so the
        # text after it counts however many blanks the note has, even when a generated block follows it)
        gen = ("<!-- manual-tables: keys screen=mixer -->", "<!-- manual-tables: cheat shift screens=perform,mixer -->", "<!-- manual-tables: cheat screens -->", "<!-- manual-tables: cheat buttons -->",
               "<!--manual-tables:keys screen=song-->")
        for note in ("<!-- manual-tables: hand -->", "<!--  manual-tables:   hand  -->", "<!-- manual-tables:hand-->"):
            for opening in gen:
                def page(hand_text, table_text):
                    return ("a\n%s\n%s\n\n%s\n%s\n<!-- /manual-tables -->\nz\n" % (note, hand_text, opening, table_text)).encode()
                assert check.digest("docs/manual/p.md", page("one", "t")) == check.digest("docs/manual/p.md", page("one", "other table")), (note, opening)
                assert check.digest("docs/manual/p.md", page("one", "t")) != check.digest("docs/manual/p.md", page("two", "t")), (note, opening)


MAP_FILES = ("docs/manual/img/panel.svg", "app/include/o3os/app/buttons.h", "app/src/bindings.cpp", "app/src/screens/registry.cpp",
             "platform/mk66/src/app_main.cpp", "platform/mk66/include/o3os/platform/mk66/boot_chord.h")
MAP_ALT = "Screen map: the screen buttons and the screens they open."


def map_test(root):
    """The screen map on copies of the real bindings, registry and boot chord: eleven cards, three ways in, and what fails when they disagree."""
    dg = load(root, "manual_diagrams")
    with tempfile.TemporaryDirectory() as d:
        for rel in MAP_FILES:
            write(os.path.join(d, rel), open(os.path.join(root, rel), encoding="utf-8").read())
        write(os.path.join(d, "docs/manual/diagrams.txt"), "[map.svg]\nkind = map\nchapter = p.md\nalt = %s\n" % MAP_ALT)
        write(os.path.join(d, "docs/manual/p.md"), "![%s](img/map.svg) Sample load, Sample edit, Modules, Input test, App, Mixer, Wave, Record, Song, Pattern, Perform, File, Config, Instrument.\n" % MAP_ALT)
        base = ["--root", d]
        rc, out = run(dg, base)
        assert rc == 0, out
        svg = open(os.path.join(d, "docs/manual/img/map.svg"), encoding="utf-8").read()
        assert svg.count('class="k"') == 11 and ">Sample edit</text>" in svg and ">Mixer</text>" in svg and "Hold Shift + F1 + F8" in svg, svg
        assert "Navigator: Song + Pattern," in svg and "press F8" in svg and "leave it with F8" in svg, svg
        rc, out = run(dg, base + ["--check"])
        assert rc == 0, out
        reg = os.path.join(d, "app/src/screens/registry.cpp")
        text = open(reg, encoding="utf-8").read()
        write(reg, text.replace("Button::Master", "Button::Config"))
        rc, out = run(dg, base + ["--check"])
        assert rc == 1 and "opens Master by the binding and Config by the registry" in out, out
        write(reg, text)
        bnd = os.path.join(d, "app/src/bindings.cpp")
        text = open(bnd, encoding="utf-8").read()
        write(bnd, "\n".join(l for l in text.split("\n") if "screen_arg(ScreenId::Song)" not in l or "constexpr" in l))
        rc, out = run(dg, base + ["--check"])
        assert rc == 1 and "the screen Song has a registry row but no Global nav.screen binding" in out, out


FLOW_FILES = ("audio/include/o3os/audio/engine.h", "audio/include/o3os/audio/graph.h", "audio/include/o3os/audio/graph_params.h",
              "audio/include/o3os/audio/taps.h", "audio/src/graph.cpp", "platform/mk66/src/app_main.cpp")
FLOW_ALT = "Signal flow: a test copy."


def flow_test(root):
    """The signal flow on copies of the audio files: it draws, and it fails when a followed symbol goes, `follows` is wrong, or a master stage is installed."""
    dg = load(root, "manual_diagrams")
    with tempfile.TemporaryDirectory() as d:
        for rel in FLOW_FILES:
            write(os.path.join(d, rel), open(os.path.join(root, rel), encoding="utf-8").read())

        def manifest(files):
            return "[flow.svg]\nkind = flow\nchapter = p.md\nalt = %s\nfollows = %s\n" % (FLOW_ALT, " ".join(files))

        write(os.path.join(d, "docs/manual/diagrams.txt"), manifest(FLOW_FILES))
        write(os.path.join(d, "docs/manual/p.md"), "![%s](img/flow.svg)\n" % FLOW_ALT)
        # the map and the chord kinds read the panel and the Button enum: the flow does not
        base = ["--root", d]
        rc, out = run(dg, base)
        assert rc == 0, out
        svg = open(os.path.join(d, "docs/manual/img/flow.svg"), encoding="utf-8").read()
        assert "MASTER LIMITER, always on" in svg and "USB return" in svg and "USB capture" in svg and len(svg) < 12000, svg
        rc, out = run(dg, base + ["--check"])
        assert rc == 0, out
        taps = os.path.join(d, "audio/include/o3os/audio/taps.h")
        text = open(taps, encoding="utf-8").read()
        write(taps, text.replace("class StemTap", "class Stems"))
        rc, out = run(dg, base + ["--check"])
        assert rc == 1 and 'the box "USB capture" follows `class StemTap`' in out and "which is gone" in out, out
        write(taps, text)
        write(os.path.join(d, "docs/manual/diagrams.txt"), manifest(FLOW_FILES[:-1]))
        rc, out = run(dg, base + ["--check"])
        assert rc == 1 and "missing ['platform/mk66/src/app_main.cpp']" in out, out
        write(os.path.join(d, "docs/manual/diagrams.txt"), manifest(FLOW_FILES))
        write(os.path.join(d, "platform/mk66/src/extra.cpp"), "void f() { graph.install_master_stage(MasterStage::Eq, &fn, nullptr); }\n")
        rc, out = run(dg, base + ["--check"])
        assert rc == 1 and "platform/mk66/src/extra.cpp installs a master stage" in out, out


def main():
    root = os.path.abspath(sys.argv[1])
    check, llms, images = load(root, "manual_check"), load(root, "manual_llms"), load(root, "manual_images")
    with tempfile.TemporaryDirectory() as d:
        m = os.path.join(d, "docs", "manual")
        git(d, "init", "-q")
        write(os.path.join(m, "index.md"), "# Index\n\n[Mixer](mixer.md)\n")
        write(os.path.join(m, "images.txt"), "# manifest\nmixer_a.png qa_mixer qa_mix_001\n")
        write(os.path.join(d, "docs", "qa", "catalogue", "mixer.md"), CAT1)
        body = ("<!-- manual-sync: SHA -->\n# Mixer\n\n## What it is for\n\nUse the Mixer to set levels. See [good](index.md), "
                "[bad](nowhere.md), [web](https://example.invalid/x).\n\n"
                "![ok](img/mixer_a.png)\n\n![orphan](img/mixer_b.png)\n\n"
                "1. Press `F1`. <!-- QA-MIX-001 -->\n<!-- TODO: ask: one\ntwo lines -->\n<!-- CONFLICT: x -->\n"
                "<!-- a [hidden](gone.md) link in a comment is ignored -->\n")
        write(os.path.join(m, "mixer.md"), body.replace("SHA", "0000000"))
        write(os.path.join(m, "other.md"), "# Other\n\nNot in the index.\n")
        write(os.path.join(m, "img", "mixer_a.png"), "png")
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "one")
        sha = git(d, "rev-parse", "--short", "HEAD")
        write(os.path.join(m, "mixer.md"), body.replace("SHA", sha))

        rc, out = run(check, ["--root", d])
        assert rc == 0, out
        assert "broken links: nowhere.md" in out and "gone.md" not in out and "good" not in out, out
        assert "image with no manifest line: img/mixer_b.png" in out and "mixer_a.png" not in out, out
        assert "other.md: not mentioned in index.md" in out, out
        assert "mixer.md: broken links: nowhere.md; image with no manifest line: img/mixer_b.png; 2 open TODO/CONFLICT" in out, out
        rc, out = run(check, ["--root", d, "--strict"])
        assert rc == 1, out

        # OS-MANUALIMG: the pictures are committed, so a listed picture whose file is missing is a problem too.
        os.remove(os.path.join(m, "img", "mixer_a.png"))
        rc, out = run(check, ["--root", d])
        assert "image file missing" in out and "img/mixer_a.png" in out, out
        write(os.path.join(m, "images.txt"), "# manifest\nmixer_a.png qa_mixer qa_mix_001 # MISSING\n")
        rc, out = run(check, ["--root", d, "--strict"])
        assert rc == 1 and "image file missing" in out, out
        write(os.path.join(m, "img", "mixer_a.png"), "png")
        write(os.path.join(m, "images.txt"), "# manifest\nmixer_a.png qa_mixer qa_mix_001\n")
        rc, out = run(check, ["--root", d])
        assert "image file missing" not in out and "mixer_a.png" not in out, out

        # Stale: nothing changed since the sync sha.
        rc, out = run(check, ["--root", d, "--stale"])
        assert "stale" in out and "none" in out, out

        # Row 001 changed, row 003 added: both are stale; 003 is not cited by the chapter.
        write(os.path.join(d, "docs", "qa", "catalogue", "mixer.md"), CAT2)
        git(d, "add", "-A")
        git(d, "commit", "-q", "-m", "two")
        rc, out = run(check, ["--root", d, "--stale"])
        assert "QA-MIX-001 QA-MIX-003" in out and "QA-MIX-002" not in out.split("catalogue IDs")[0].split("stale")[1], out
        assert "not cited by the chapter yet: QA-MIX-003" in out, out
        assert "mixer (chapter exists): 2: QA-MIX-002 QA-MIX-003" in out, out
        # Bumping the sync sha clears it.
        new = git(d, "rev-parse", "--short", "HEAD")
        write(os.path.join(m, "mixer.md"), body.replace("SHA", new))
        rc, out = run(check, ["--root", d, "--stale"])
        assert out.split("stale")[1].split("catalogue IDs")[0].strip().endswith("none"), out
        # A chapter without a marker and one with an unknown sha are reported.
        write(os.path.join(m, "mixer.md"), body.replace("SHA", "deadbeef"))
        rc, out = run(check, ["--root", d, "--stale"])
        assert "is not a known commit" in out, out
        write(os.path.join(m, "mixer.md"), body.replace("<!-- manual-sync: SHA -->\n", ""))
        rc, out = run(check, ["--root", d, "--stale", "--strict"])
        assert "no `<!-- manual-sync" in out and rc == 1, out

        # llms: generate, then --check passes; a change in a chapter makes it fail; comments and pictures are stripped.
        rc, out = run(llms, ["--root", d, "--check"])
        assert rc == 1 and "missing" in out, out
        rc, out = run(llms, ["--root", d])
        assert rc == 0, out
        full = open(os.path.join(m, "llms-full.txt"), encoding="utf-8").read()
        assert "TODO" not in full and "QA-MIX-001" not in full and "manual-sync" not in full, full
        assert "(Picture: ok)" in full and "img/mixer_a.png" not in full and "Not in the index." in full, full
        idx = open(os.path.join(m, "llms.txt"), encoding="utf-8").read()
        assert idx.startswith("# track3r-os\n") and "- [Other](other.md): Not in the index." in idx, idx
        rc, out = run(llms, ["--root", d, "--check"])
        assert rc == 0, out
        write(os.path.join(m, "other.md"), "# Other\n\nChanged.\n")
        rc, out = run(llms, ["--root", d, "--check"])
        assert rc == 1 and "llms-full.txt is out of date" in out, out
        images_test(images, d, m)
    a11y_test(check)
    sync_test(check)
    ack_path_test(check)
    diagrams_test(root)
    map_test(root)
    flow_test(root)
    print("manual_tools_test: ok")


if __name__ == "__main__":
    main()
