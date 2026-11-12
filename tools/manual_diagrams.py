#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the manual's SVG diagrams from the firmware's own tables (OS-MANUALDIAG).

Reads the manifest docs/manual/diagrams.txt and writes one SVG per stanza to docs/manual/img/. The SVGs are generated text:
never draw or edit one by hand, change the manifest or this script and run it again. They are committed (the manual renders on
GitHub), and `--check` (ctest `manual_diagrams`, label static + manual) fails when one is stale, missing or not listed.

A stanza is `[<name>.svg]` followed by `key = value` lines (`#` starts a comment):

    kind     chord | map | flow
    chapter  the chapter of docs/manual/ that shows the diagram; --check fails unless it contains the line
             `![<alt>](img/<name>.svg)` and, for a chord, every label of three or more characters
    region   chord: the part(s) of the panel the figure is about, `soft` (F1 to F8), `nav` (the screen buttons), `edit` (the edit and
             transport block) or `power` (the rear-panel button), joined by `+` when the keys are in two of them. The figure is cropped to
             the marked keys plus one key of room, so it is small (about 4 KB). Every marked key must be in a listed part and every listed
             part must hold a marked key, or it is an error. A figure that would be wider than MAX_FIG_WIDTH (840 px: the boot chord, Shift
             and F1..F8) is drawn as one pane per part, stacked in the order listed, each trimmed to that width.
    why      chord: needed when every key of the stanza is Shift + F1..F8 (a Shift row): a Shift row is labelled by the unit itself, so a
             figure of it must say why this one is worth its place. Without it, an error.
    extra    chord: needed on every chord diagram of a chapter after the first, said as the reason it earns its place; `--check` fails a
             chapter that shows two or more chord diagrams without it.
    alt      the alt text, word for word the chapter's; it is also the SVG's <title>
    caption  chord: the figure's long description (its <desc>; the cropped figure has no screen to draw it in); `{hold_s}` (here and in the alt text) is the Power hold in seconds;
             `{nothing}` (alt text) is derived from the bindings: "F5 does nothing." / "F5 and F7 do nothing." for the soft keys F1 to F8
             that have no Shift binding in the context of the stanza's `bind` lines (Global's ui.none does not count), and empty when
             all eight have one. A new Shift binding of that screen changes the text, so --check fails until the chapter says it.

kind chord: the panel of img/panel.svg, cropped to `region`, with the keys of one combination marked (a ring is a key held down, a filled key is
pressed; numbers 1 and 2 say the order when a key is held while another is pressed). At most MAX_KEYS (4) keys are marked: keys that share one label
(the four arrows of a block) count once, so a figure shows one held key and a few targets, or the boot chord; more is an error (split it or write it
in words). The keys come from the code, not from this manifest:

    bind    <Context> <action> [<arg>] [via <Button>] [tap|chord] [shift|note|plain] [| <label>]
            the one `bind(...)` row of app/src/bindings.cpp with that context, action and argument (`via` picks the key, and
            `shift`, `note` or `plain` the modifier, when an action has several); its buttons and its Shift / Note modifier are the keys.
            No row, or more than one: an error, so a diagram cannot outlive the binding it shows. The label is drawn under an F key,
            or inside the key.
    source  boot-chords       the power-up chords, held while the unit powers up: kChordButtons (platform/mk66/src/app_main.cpp, the input
                              test, labelled TEST) and the boot disk, its Shift and F1 plus kDiskChordKey (labelled DISK), in one figure
            power-hold        the rear-panel Power button, held for core::power::kHoldMs

kind map: the screen buttons and the screens they reach (the nav.screen rows of Global, the screen registry's names), and the screens
that no button reaches (Navigator, boot chord, app launch).

kind flow: the signal flow of the audio engine. `follows` lists the files it is drawn from; every box names a symbol of one of them and
--check fails when the symbol is gone (or when `follows` lists a file that no box uses).

  tools/manual_diagrams.py            write every diagram of the manifest
  tools/manual_diagrams.py --check    exit 1 when a diagram is stale, missing, unlisted, too big, or not in its chapter
  tools/manual_diagrams.py --list     the manifest with the Markdown line to paste into the chapter

Python 3 standard library only; read-only on the sources.
"""

import argparse
import os
import re
import sys
import textwrap
from xml.sax.saxutils import escape

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MANIFEST = "docs/manual/diagrams.txt"
IMG = "docs/manual/img"
PANEL = "docs/manual/img/panel.svg"
BINDINGS = "app/src/bindings.cpp"
BUTTONS = "app/include/o3os/app/buttons.h"
REGISTRY = "app/src/screens/registry.cpp"
APP_MAIN = "platform/mk66/src/app_main.cpp"
BOOT_CHORD = "platform/mk66/include/o3os/platform/mk66/boot_chord.h"
POWER_HOLD = "core/include/o3os/core/power_hold.h"

MAX_BYTES = 6000  # "keep each SVG small": a cropped chord figure is about 4 KB (an uncropped one is over 6 KB), the signal flow is the biggest at 5.8 KB
MAX_KEYS = 4  # marked keys of a chord figure (keys that share a label count once): at most one held key and three targets, or the boot chord
KEY_ROOM = 80  # the crop of a chord figure: the marked keys and one key (72 px and the 8 px gap) on every side
MIN_WIDTH = 440  # a narrower crop is drawn larger, so that its labels stay legible
MAX_FIG_WIDTH = 840  # a wider figure is shown smaller by the page (GitHub's column is about 830 px) and its 12 px labels with it: one pane per region
PANE_GAP = 16
LEGEND_ROW = 44
MAX_ALT_WORDS = 25  # the manual's rule for alt text (.claude/skills/manual-writing/SKILL.md section 8)
MAX_CAPTION = 130  # characters; the caption is the figure's <desc> (a cropped figure has no screen to draw it in), so it is read, not seen

ACCENT = "#ffb000"
INK = "#141414"


class DiagramError(Exception):
    pass


def read(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        raise DiagramError("cannot read %s: %s" % (rel, e.strerror))


# ---- The firmware's tables --------------------------------------------------------------------------------------------------------

def button_codes(root):
    """{name: code} from the Button enum of app/buttons.h."""
    text = re.sub(r"//[^\n]*", "", read(root, BUTTONS))
    m = re.search(r"enum class Button[^{]*\{(.*?)\};", text, re.S)
    if not m:
        raise DiagramError("%s: no `enum class Button`" % BUTTONS)
    codes, nxt = {}, 0
    for item in m.group(1).split(","):
        item = item.strip()
        if not item:
            continue
        name, _, val = item.partition("=")
        nxt = int(val.strip(), 0) if val.strip() else nxt
        codes[name.strip()] = nxt
        nxt += 1
    return codes


def split_top(s):
    """Split on the commas outside brackets."""
    out, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def parse_call(s):
    """`name(a, b(c), d)` -> (name, [a, b(c), d]); None when s is not one whole call."""
    s = s.strip()
    i = s.find("(")
    if i < 1 or not s.endswith(")"):
        return None
    depth = 0
    for j in range(i, len(s)):
        depth += (s[j] == "(") - (s[j] == ")")
        if depth == 0 and j != len(s) - 1:
            return None
    return s[:i].strip(), split_top(s[i + 1:-1])


class Bind:
    def __init__(self, ctx, kind, buttons, mod, action, arg, line):
        self.ctx, self.kind, self.buttons, self.mod, self.action, self.arg, self.line = ctx, kind, buttons, mod, action, arg, line

    def __str__(self):
        keys = "+".join(([self.mod] if self.mod else []) + self.buttons)
        return "%s %s %s%s (%s:%d)" % (self.ctx, self.kind, keys, " -> " + self.action + ("" if self.arg is None else " " + str(self.arg)), BINDINGS, self.line)


def arg_value(raw):
    try:
        return int(raw, 0)
    except ValueError:
        return raw


def bindings(root):
    """Every `bind(Context, tap|hold|hold_end|chord(buttons), action[, ArgFrom::X, arg])` row of the built-in table that is made of buttons."""
    text = read(root, BINDINGS)
    ctx_names = dict(re.findall(r"constexpr core::ContextId (k\w+) = context_id\(Context::(\w+)\);", text))
    actions = dict(re.findall(r'constexpr core::ActionId (k\w+) = core::action_id\("([^"]+)"\);', text))
    mods = dict((k, v) for k, v in re.findall(r"constexpr core::InputRef (k\w+) = button_ref\(Button::(\w+)\);", text))
    if not ctx_names or not actions or "kShift" not in mods:
        raise DiagramError("%s: the context, action and modifier constants were not found" % BINDINGS)

    def btn(x):
        m = re.fullmatch(r"button_ref\(Button::(\w+)\)", x.strip())
        return m.group(1) if m else None

    out = []
    for no, raw in enumerate(text.splitlines(), 1):
        line = raw.split("//")[0].strip()
        if not line.startswith("bind(") or not line.endswith("),"):
            continue
        call = parse_call(line[:-1])
        if call is None or call[0] != "bind" or len(call[1]) < 3:
            continue
        a = call[1]
        g = parse_call(a[1])
        if g is None or a[0] not in ctx_names or a[2] not in actions:
            continue
        gname, gargs = g
        if gname in ("tap", "hold", "hold_end"):
            b = btn(gargs[0])
            if b is None:
                continue
            mod = mods.get(gargs[1]) if len(gargs) > 1 else None
            if len(gargs) > 1 and mod is None:
                continue
            bs = [b]
        elif gname == "chord":
            bs = [btn(x) for x in gargs]
            if None in bs:
                continue
            mod = None
        else:
            continue
        arg = arg_value(a[4]) if len(a) >= 5 and a[3] == "ArgFrom::Fixed" else None
        out.append(Bind(ctx_names[a[0]], gname, bs, mod, actions[a[2]], arg, no))
    if not out:
        raise DiagramError("%s: no bind(...) rows found" % BINDINGS)
    return out


def boot_chords(root):
    """The power-up chords: {'input-test': [buttons], 'disk': [buttons]} from app_main.cpp and boot_chord.h."""
    app = read(root, APP_MAIN)
    hdr = read(root, BOOT_CHORD)
    m = re.search(r"kChordButtons\[\]\s*=\s*\{(.*?)\};", app, re.S)
    chord = re.findall(r"app::Button::(\w+)", m.group(1)) if m else []
    m = re.search(r"kDiskChordKey\s*=\s*static_cast<unsigned>\(app::Button::(\w+)\)", app)
    disk_key = m.group(1) if m else None
    m = re.search(r"kBootChordKeys\s*=\s*(\d+)", hdr)
    n = int(m.group(1)) if m else -1
    m = re.search(r"kBootDiskMask\s*=\s*(0x[0-9a-fA-F]+|\d+)u", hdr)
    mask = int(m.group(1), 0) if m else -1
    if not chord or disk_key is None or n != len(chord) or mask != 0x3 or chord[:2] != ["Shift", "F1"]:
        raise DiagramError("%s / %s: the boot chord constants are not what this script reads (kChordButtons, kDiskChordKey, "
                           "kBootChordKeys, kBootDiskMask 0x3 = Shift + F1)" % (APP_MAIN, BOOT_CHORD))
    return {"input-test": chord, "disk": chord[:2] + [disk_key]}


def power_hold_seconds(root):
    m = re.search(r"kHoldMs\s*=\s*(\d+)", read(root, POWER_HOLD))
    if not m:
        raise DiagramError("%s: no kHoldMs" % POWER_HOLD)
    return int(m.group(1)) / 1000.0


# ---- The panel ----------------------------------------------------------------------------------------------------------------------

ATTR_RE = re.compile(r'([\w:-]+)="([^"]*)"')

# What the panel drawing says on each key, by Button code (docs/manual/img/panel.svg is hand-made: this table is how a change of it,
# or of the Button enum's order, is noticed).
PANEL_LABELS = {0: "F1", 1: "F2", 2: "F3", 3: "F4", 4: "F5", 5: "F6", 6: "F7", 7: "F8",
                8: "Instrument Parameters", 9: "Perform", 10: "File", 11: "Config", 12: "Master",
                13: "Sample Loader", 14: "Sample Playback", 15: "Sample Editor", 16: "Sample Recorder", 17: "Song",
                18: "Note", 19: "Instr", 20: "Vol", 21: "Fx", 22: "Pattern",
                23: "Play", 24: "Paste Copy", 25: "Home Insert", 26: "Up", 27: "Backspace Delete",
                28: "Rec", 29: "Shift", 30: "Left", 31: "Down", 32: "Right"}


def key_code(x, y, w, h):
    """The Button code of the key whose rectangle this is, or None: the panel's rows are in the order of the Button enum."""
    if (w, h) == (88, 48) and y == 556 and (x - 66) % 100 == 0 and 0 <= (x - 66) // 100 < 8:
        return (x - 66) // 100
    if (w, h) == (72, 72) and (x - 900) % 80 == 0 and 0 <= (x - 900) // 80 < 5:
        col = (x - 900) // 80
        if y in (60, 140, 220):
            return 8 + 5 * ((y - 60) // 80) + col
        if y in (330, 410):
            return 23 + 5 * ((y - 330) // 80) + col
    return None


class Panel:
    """docs/manual/img/panel.svg, read line by line: the key rectangles by Button code, and everything else as it is."""

    def __init__(self, text):
        self.lines = []  # (kind, payload): ("raw", str) | ("rect", attrs, code) | ("text", attrs, content) | ("pad",) | ("style", [str])
        self.geom = {}
        self.fill = {}  # Button code -> the rectangle's own fill attribute (the four coloured column keys), else None
        in_style, style = False, []
        for raw in text.splitlines():
            line = raw.strip()
            if in_style:
                if line == "</style>":
                    in_style = False
                    self.lines.append(("style", style))
                else:
                    style.append(line)
                continue
            if line == "<style>":
                in_style, style = True, []
                continue
            if not line or line.startswith("<!--") or line.startswith("<?xml") or line.startswith("<svg") or line.startswith("<title") \
                    or line.startswith("<desc") or line == "</svg>":
                continue
            m = re.fullmatch(r"<rect ([^>]*)/>", line)
            if m:
                attrs = ATTR_RE.findall(m.group(1))
                d = dict(attrs)
                if d.get("class") == "pad":
                    if not self.lines or self.lines[-1][0] != "pad":
                        self.lines.append(("pad",))
                    continue
                code = key_code(int(d.get("x", -1)), int(d.get("y", -1)), int(d.get("width", -1)), int(d.get("height", -1))) if "x" in d else None
                if code is not None:
                    self.geom[code] = tuple(int(d[k]) for k in ("x", "y", "width", "height"))
                    self.fill[code] = d.get("fill")
                self.lines.append(("rect", attrs, code))
                continue
            m = re.fullmatch(r"<text ([^>]*)>(.*)</text>", line)
            if m:
                attrs = ATTR_RE.findall(m.group(1))
                cls = dict(attrs).get("class")
                if cls == "scr":  # "SCREEN 800 x 480": a diagram says its caption there
                    self.lines.append(("caption",))
                elif cls != "cap":  # the panel's own footnote is dropped
                    self.lines.append(("text", attrs, m.group(2)))
                continue
            self.lines.append(("raw", line))
        missing = [c for c in PANEL_LABELS if c not in self.geom]
        if missing:
            raise DiagramError("%s: no rectangle for the Button code(s) %s" % (PANEL, missing))
        # the pad grid is one pattern: 12 x 4 pads, pitch 67
        self.check_labels()

    def key_of(self, x, y):
        for code, (kx, ky, kw, kh) in self.geom.items():
            if kx <= x <= kx + kw and ky <= y <= ky + kh:
                return code
        return None

    def geom_box(self, code):
        x, y, w, h = self.geom[code]
        return (x, y, x + w, y + h)

    def check_labels(self):
        found = {c: [] for c in PANEL_LABELS}
        for ln in self.lines:
            if ln[0] != "text":
                continue
            d = dict(ln[1])
            if d.get("class", "").split()[0:1] not in (["t"], ["tk"]):
                continue
            code = self.key_of(int(d["x"]), int(d["y"]))
            if code is not None:
                found[code].append((int(d["y"]), ln[2]))
        for code, want in PANEL_LABELS.items():
            got = " ".join(t for _, t in sorted(found[code]))
            if got != want:
                raise DiagramError("%s: the key of Button code %d reads %r, the Button enum order says %r" % (PANEL, code, got, want))


def attr_str(attrs):
    return " ".join('%s="%s"' % kv for kv in attrs)


def add_class(attrs, cls):
    out, done = [], False
    for k, v in attrs:
        if k == "class":
            out.append((k, v + " " + cls))
            done = True
        else:
            out.append((k, v))
    return out if done else [("class", cls)] + out


STYLE_EXTRA = [
    ".p{fill:%s}" % ACCENT,
    ".h{stroke:%s;stroke-width:7}" % ACCENT,
    ".lb{font-family:sans-serif;font-size:20px;font-weight:bold;fill:%s;text-anchor:middle}" % ACCENT,
    ".lk{font-family:sans-serif;font-size:14px;font-weight:bold;fill:%s;text-anchor:middle}" % INK,
    ".bn{font-family:sans-serif;font-size:16px;font-weight:bold;fill:#f4f4f4;text-anchor:middle}",
    ".cp{font-family:sans-serif;font-size:30px;fill:#f4f4f4;text-anchor:middle}",
    ".lg{font-family:sans-serif;font-size:20px;fill:#f4f4f4}",
]


def svg_open(alt, caption, box=(0, 0, 1360, 960), scale=1.0):
    x, y, w, h = box
    return ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="%d %d %d %d" role="img" aria-labelledby="t d">'
            % (round(w * scale), round(h * scale), x, y, w, h),
            '<title id="t">%s</title>' % escape(alt),
            '<desc id="d">%s</desc>' % escape(caption)]


# The parts of the panel a chord figure can be about, by Button code ("Power" is the rear-panel button, drawn at POWER_BOX in the free corner).
REGIONS = {"soft": range(0, 8), "nav": range(8, 23), "edit": range(23, 33), "power": ("Power",)}
BODY = (20, 20, 1340, 940)  # the unit's body in panel.svg: a crop stays inside it
POWER_BOX = (1215, 835, 1325, 905)


def region_of(code):
    for name, codes in REGIONS.items():
        if code in codes:
            return name
    return None


def parse_region(spec, roles):
    """The `region` of a chord stanza as a list of names; every marked key must be in one of them and each must hold a marked key."""
    text = spec.get("region")
    if not text:
        raise DiagramError("%s:%d: [%s] has no `region` (soft, nav, edit or power, joined by +): the figure is cropped to it" % (MANIFEST, spec.lineno, spec.name))
    names = [t.strip() for t in text.split("+")]
    bad = [n for n in names if n not in REGIONS]
    if bad or len(set(names)) != len(names):
        raise DiagramError("%s:%d: [%s] `region = %s`: expected soft, nav, edit or power, each once, joined by +" % (MANIFEST, spec.lineno, spec.name, text))
    used = {region_of(c) for c in roles}
    if used != set(names):
        raise DiagramError("%s:%d: [%s] `region = %s` but the marked keys are in %s" % (MANIFEST, spec.lineno, spec.name, text, "+".join(sorted(used))))
    return names


def crop_box(panel, keys, max_width=None):
    """(x0, y0, x1, y1): the keys and one key of room on every side, inside the unit's body; the room at the sides shrinks to keep it at most
    `max_width` wide (the keys themselves must fit)."""
    boxes = []
    for code in keys:
        if code == "Power":
            boxes.append(POWER_BOX)
        else:
            x, y, w, h = panel.geom[code]
            boxes.append((x, y, x + w, y + h))
    kx0, kx1 = min(b[0] for b in boxes), max(b[2] for b in boxes)
    room = KEY_ROOM
    if max_width is not None:
        if kx1 - kx0 > max_width:
            raise DiagramError("the keys of the figure are %d px apart, more than %d: list them in two regions" % (kx1 - kx0, max_width))
        room = min(room, (max_width - (kx1 - kx0)) // 2)
    x0, x1 = max(BODY[0], kx0 - room), min(BODY[2], kx1 + room)
    return (x0, max(BODY[1], min(b[1] for b in boxes) - KEY_ROOM), x1, min(BODY[3], max(b[3] for b in boxes) + KEY_ROOM))


def touches(box, crop):
    return box is not None and box[0] < crop[2] and box[2] > crop[0] and box[1] < crop[3] and box[3] > crop[1]


def inside(box, crop):
    return box is not None and box[0] >= crop[0] and box[2] <= crop[2] and box[1] >= crop[1] and box[3] <= crop[3]


def shown(box, crop):
    """A big part of the body (the screen, the encoder) is drawn when the crop meets it and the viewBox cuts it; a key, a pad or a label is drawn
    whole or not at all, so that no key is cut in half at the edge of the figure."""
    small = box is not None and box[2] - box[0] <= 120 and box[3] - box[1] <= 120
    return inside(box, crop) if small else touches(box, crop)


def rect_box(attrs):
    d = dict(attrs)
    try:
        x, y, w, h = (int(d[k]) for k in ("x", "y", "width", "height"))
    except (KeyError, ValueError):
        return None
    return (x, y, x + w, y + h)


def count_keys(roles, labels):
    """The keys a figure marks: keys that share a label (the four arrows of a block) count once."""
    shared = {labels[c] for c in roles if c in labels}
    return len(shared) + sum(1 for c in roles if c not in labels)


def pane_lines(panel, crop, roles, labels, badge, keys):
    """One pane in the panel's own coordinates: the parts of the panel inside `crop` (not the body, not the caption) and the marks of `keys`."""
    out = []
    for ln in panel.lines:
        k = ln[0]
        if k in ("style", "caption"):
            continue  # (the description is in <desc>: there is no screen in a cropped figure)
        if k == "pad":
            if inside((60, 650, 855, 909), crop):
                out.append('<pattern id="pd" x="60" y="650" width="67" height="67" patternUnits="userSpaceOnUse"><rect width="58" height="58" rx="6" fill="#6a6a6a"/></pattern>')
                out.append('<rect x="60" y="650" width="795" height="259" fill="url(#pd)"/>')
        elif k == "rect":
            attrs, code = ln[1], ln[2]
            if dict(attrs).get("class") == "body" or not shown(rect_box(attrs), crop):  # (the figure has its own body, under the panes)
                continue
            role = roles.get(code) if code is not None else None
            if role == "press":
                attrs = add_class(attrs, "p")
            elif role == "hold":
                attrs = add_class(attrs, "h")
            out.append("<rect %s/>" % attr_str(attrs))
        elif k == "text":
            attrs, content = ln[1], ln[2]
            d = dict(attrs)
            tx, ty = int(d["x"]), int(d["y"])
            code = panel.key_of(tx, ty)
            if not shown(panel.geom_box(code) if code is not None else (tx - 50, ty - 16, tx + 50, ty + 4), crop):
                continue
            if code is not None and roles.get(code) == "press":
                attrs = [(a, v) for a, v in attrs if a != "style"] + [("style", "fill:%s" % INK)]
            out.append("<text %s>%s</text>" % (attr_str(attrs), content))
        else:
            m = re.search(r'cx="(\d+)" cy="(\d+)" r="(\d+)"', ln[1])
            if m:
                cx, cy, r = (int(m.group(i)) for i in (1, 2, 3))
                if not inside((cx - r, cy - r, cx + r, cy + r), crop):
                    continue  # (the encoder: whole or not at all, like a key)
            out.append(ln[1])
    for code in sorted(keys, key=str):
        if code == "Power":
            continue
        x, y, kw, kh = panel.geom[code]
        text = labels.get(code)
        if text:
            if code < 8:  # an F key: the label goes under it, smaller when it is long (the keys are 100 apart)
                if len(text) > 8:
                    raise DiagramError("the label %r of an F key is longer than 8 characters" % text)
                out.append('<text class="lb" x="%d" y="%d"%s>%s</text>' % (x + kw // 2, y + kh + 28, ' style="font-size:17px"' if len(text) > 6 else "", escape(text)))
            else:  # inside the key, at the bottom
                out.append('<text class="lk" x="%d" y="%d">%s</text>' % (x + kw // 2, y + kh - 9, escape(text)))
        if code in badge:
            r = 13 if code < 8 else 14
            cx, cy = x + r + 1, y + r + 1
            out.append('<circle cx="%d" cy="%d" r="%d" fill="%s"/><text class="bn" x="%d" y="%d">%s</text>' % (cx, cy, r, INK, cx, cy + 6, badge[code]))
    if "Power" in keys:
        out.append('<rect x="1215" y="835" width="110" height="70" rx="8" fill="#2a2a2a" stroke="%s" stroke-width="7"/>' % ACCENT)
        out.append('<text class="t" x="1270" y="866">Power</text><text class="tk" x="1270" y="884">rear panel</text>')
        if labels.get("Power"):
            out.append('<text class="lb" x="1270" y="928">%s</text>' % escape(labels["Power"]))
    return out


def render_chord(panel, alt, caption, roles, labels, press_text, names):
    """roles: {code: 'press'|'hold'} (and 'Power': 'hold' for the rear-panel button); labels: {code or 'Power': text}; names: the figure's regions.
    The numbers 1 and 2 are drawn when a key is held while another is pressed. Only the parts of the panel near the marked keys are drawn, in
    panes (nested <svg>s, which clip them) on a body of their own, with the legend under them: one pane, or one for each region when the keys
    are further apart than MAX_FIG_WIDTH."""
    keys = list(roles)
    groups = [keys]
    whole = crop_box(panel, keys)
    if len(names) > 1 and whole[2] - whole[0] > MAX_FIG_WIDTH:
        groups = [[c for c in keys if region_of(c) == n] for n in names]
    panes = [(g, crop_box(panel, g, MAX_FIG_WIDTH)) for g in groups]
    kinds = set(roles.values())
    badge = {c: ("1" if r == "hold" else "2") for c, r in roles.items()} if kinds == {"hold", "press"} else {}
    legend = []
    if "hold" in kinds:
        legend.append(("h", "1  hold down" if badge else "hold down"))
    if "press" in kinds:
        legend.append(("p", ("2  " if badge else "") + press_text))
    w = max(c[2] - c[0] for _, c in panes)
    top = sum(c[3] - c[1] for _, c in panes) + PANE_GAP * (len(panes) - 1)
    h = top + LEGEND_ROW * (len(legend) - 1) + 54
    out = svg_open(alt, caption, (0, 0, w, h), max(1.0, MIN_WIDTH / w))
    style = [ln for ln in panel.lines if ln[0] == "style"]
    if not style:
        raise DiagramError("%s: no <style> block" % PANEL)
    out.append("<style>")
    out.extend(style[0][1] + STYLE_EXTRA)
    out.append("</style>")
    out.append('<rect class="body" width="%d" height="%d" rx="14"/>' % (w, h))
    y = 0
    for g, crop in panes:
        pw, ph = crop[2] - crop[0], crop[3] - crop[1]
        out.append('<svg x="0" y="%d" width="%d" height="%d" viewBox="%d %d %d %d">' % (y, pw, ph, crop[0], crop[1], pw, ph))
        out.extend(pane_lines(panel, crop, roles, labels, badge, g))
        out.append("</svg>")
        y += ph + PANE_GAP
    for i, (cls, text) in enumerate(legend):
        ly = top + 12 + LEGEND_ROW * i
        if cls == "h":
            out.append('<rect x="16" y="%d" width="30" height="30" rx="5" fill="#7a7a7a" stroke="%s" stroke-width="5"/>' % (ly, ACCENT))
        else:
            out.append('<rect x="16" y="%d" width="30" height="30" rx="5" fill="%s"/>' % (ly, ACCENT))
        out.append('<text class="lg" x="60" y="%d">%s</text>' % (ly + 22, escape(text)))
    out.append("</svg>")
    return "\n".join(out) + "\n"


# ---- The screen map -----------------------------------------------------------------------------------------------------------------

def registry_rows(root):
    """[(ScreenId name, NAME, Button name, hidden, buttonless)] from the rows of app/src/screens/registry.cpp."""
    text = read(root, REGISTRY)
    rows = []
    for m in re.finditer(r'\{ScreenId::(\w+), "([^"]+)", &\w+, Button::(\w+), "[^"]*", Icon::\w+(?:, (true|false))?(?:, (true|false))?\}', text):
        rows.append((m.group(1), m.group(2), m.group(3), m.group(4) == "true", m.group(5) == "true"))
    if len(rows) < 12:
        raise DiagramError("%s: %d screen rows read, the registry table is not what this script reads" % (REGISTRY, len(rows)))
    return rows


def keys_text(b, codes=None):
    return " + ".join(([b.mod] if b.mod else []) + b.buttons)


def render_map(root, spec, ctx):
    """The screen buttons and the screens they open (Global nav.screen rows, registry names), and the screens no button opens."""
    alt = spec.get("alt")
    binds = ctx["binds"][0] if ctx["binds"][0] is not None else bindings(root)
    ctx["binds"][0] = binds
    rows = registry_rows(root)
    nav = {}
    for b in binds:
        m = re.fullmatch(r"screen_arg\(ScreenId::(\w+)\)", b.arg) if isinstance(b.arg, str) else None
        if b.ctx == "Global" and b.action == "nav.screen" and b.kind == "tap" and not b.mod and m:
            nav[m.group(1)] = b.buttons[0]
    nice = lambda n: n.capitalize()
    cards = {}  # Button code -> (key label, screen name)
    names = {}
    for sid, name, reg_button, hidden, buttonless in rows:
        names[sid] = nice(name)
        if hidden or buttonless:
            continue
        if sid not in nav:
            raise DiagramError("%s: the screen %s has a registry row but no Global nav.screen binding in %s" % (spec.name, sid, BINDINGS))
        if nav[sid] != reg_button:
            raise DiagramError("%s: %s opens %s by the binding and %s by the registry" % (spec.name, sid, nav[sid], reg_button))
        cards[ctx["codes"][nav[sid]]] = (PANEL_LABELS[ctx["codes"][nav[sid]]], names[sid])
    for sid in nav:
        if sid not in names:
            raise DiagramError("%s: nav.screen opens %s, which the registry does not list" % (spec.name, sid))
    if len(cards) != 11:
        raise DiagramError("%s: %d screens have a button, the map is drawn for 11" % (spec.name, len(cards)))
    hidden = {sid: nice(name) for sid, name, _, h, bl in rows if h or bl}
    for want in ("Modules", "InputTest", "ModuleApp"):
        if want not in hidden:
            raise DiagramError("%s: the registry has no hidden or buttonless row %s" % (spec.name, want))
    if len(hidden) != 3:
        raise DiagramError("%s: the registry has %d screens with no button, the map draws 3: add a card for %s" % (spec.name, len(hidden), sorted(set(hidden) - {"Modules", "InputTest", "ModuleApp"})))
    open_nav, _ = resolve_bind(binds, "Global navigator.open via Pattern", 0)
    accept, _ = resolve_bind(binds, "Navigator navigator.accept via F1 plain", 0)
    launch, _ = resolve_bind(binds, "Modules module.launch", 0)
    leave, _ = resolve_bind(binds, "ModuleApp module.exit via F8 tap plain", 0)
    boot = boot_chords(root)["input-test"]
    reach = [
        (hidden["Modules"], ["Navigator: %s," % keys_text(open_nav), "choose it, then press %s" % keys_text(accept)]),
        (hidden["InputTest"], ["Hold %s" % " + ".join(boot), "while the unit powers up"]),
        (hidden["ModuleApp"], ["Launch an app on Modules (%s)," % keys_text(launch), "leave it with %s" % keys_text(leave)]),
    ]
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="866" height="528" viewBox="0 0 866 528" role="img" aria-labelledby="t">',
           '<title id="t">%s</title>' % escape(alt),
           "<style>",
           ".b{fill:#3c3c3c}.k{fill:#5e5e5e}.n{fill:#2a2a2a;stroke:#7a7a7a;stroke-width:2;stroke-dasharray:6 4}",
           ".t{font-family:sans-serif;fill:#f4f4f4;text-anchor:middle}.s{font-size:13px;fill:#cfcfcf}.m{font-size:22px;font-weight:bold}",
           ".h{font-family:sans-serif;font-size:18px;fill:#b0b0b0}.r{font-size:15px}",
           "</style>",
           '<rect class="b" width="866" height="528" rx="20"/>']
    for code in range(8, 23):
        i = code - 8
        x, y = 30 + 164 * (i % 5), 30 + 106 * (i // 5)
        if code in cards:
            label, name = cards[code]
            out.append('<rect class="k" x="%d" y="%d" width="150" height="92" rx="8"/>' % (x, y))
            out.append('<text class="t s" x="%d" y="%d">%s</text>' % (x + 75, y + 28, escape(label)))
            out.append('<text class="t m" x="%d" y="%d">%s</text>' % (x + 75, y + 62, escape(name)))
        else:  # a column key of the Pattern screen: drawn dim, in its own colour
            out.append('<rect x="%d" y="%d" width="150" height="92" rx="8" fill="%s" fill-opacity=".3"/>' % (x, y, ctx["panel"].fill.get(code) or "#5e5e5e"))
            out.append('<text class="t s" x="%d" y="%d">%s</text>' % (x + 75, y + 50, escape(PANEL_LABELS[code] + " (a column key)")))
    out.append('<text class="h" x="30" y="378">No screen button:</text>')
    for i, (name, lines) in enumerate(reach):
        x = 30 + 274 * i
        out.append('<rect class="n" x="%d" y="394" width="260" height="104" rx="8"/>' % x)
        out.append('<text class="t m" x="%d" y="428">%s</text>' % (x + 130, escape(name)))
        for j, line in enumerate(lines):
            out.append('<text class="t r" x="%d" y="%d">%s</text>' % (x + 130, 458 + 22 * j, escape(line)))
    out.append("</svg>")
    labels = dict((str(k), v[1]) for k, v in cards.items())
    labels.update(hidden)
    return "\n".join(out) + "\n", labels


# ---- The signal flow -----------------------------------------------------------------------------------------------------------------

AUDIO = "audio/include/o3os/audio/"
# Every box of the flow names the code it follows: (box, file, a symbol or phrase that file must still contain). The files are the manifest's
# `follows` list (the check is both ways), so a box cannot name a thing the engine no longer has.
FLOW_CODE = [
    ("Instruments", AUDIO + "engine.h", "attach_voices"),
    ("Instruments", AUDIO + "engine.h", "attach_amiga"),
    ("Instruments", AUDIO + "engine.h", "attach_modules"),
    ("Track strip", AUDIO + "graph.h", "inline constexpr unsigned kTracks"),
    ("Track strip", AUDIO + "graph.h", "using InsertFn"),
    ("Track strip", "audio/src/graph.cpp", "void Graph::finish_track"),
    ("Send effects", AUDIO + "graph.h", "using SendFxFn"),
    ("Send effects", AUDIO + "graph_params.h", "RetRouteA"),
    ("Buses", AUDIO + "graph_params.h", "kMain = 1"),
    ("Buses", AUDIO + "graph_params.h", "kCue = 2"),
    ("Buses", AUDIO + "graph_params.h", "kRec = 16"),
    ("Buses", AUDIO + "graph_params.h", "kUsb = 32"),
    ("Master chain", AUDIO + "graph.h", "MasterStage::Limiter"),
    ("Master chain", AUDIO + "graph.h", "ALWAYS ON"),
    ("Master chain", "platform/mk66/src/app_main.cpp", "install_master_limiter("),
    ("Line in", AUDIO + "graph.h", "INPUT STRIP"),
    ("USB return", AUDIO + "graph.h", "USB return (OS-006e"),
    ("USB return", "audio/src/graph.cpp", "ret_heard_ = "),
    ("USB return", "audio/src/graph.cpp", "mul_q16(ur.start, mg.start)"),
    ("USB capture", "audio/src/graph.cpp", "stem_have_ |= 1u << track"),
    ("Outputs", AUDIO + "graph_params.h", "MainLCueR"),
    ("USB capture", AUDIO + "taps.h", "class StemTap"),
    ("USB capture", AUDIO + "taps.h", "namespace stem_src"),
    ("Metronome and loop", "audio/src/graph.cpp", "mix_extras("),
]


def flow_check(root, spec):
    """The symbols the boxes follow are still in the files, `follows` lists exactly those files, and the limiter is the only master stage installed."""
    used = set()
    texts = {}
    for box, rel, token in FLOW_CODE:
        texts.setdefault(rel, read(root, rel))
        used.add(rel)
        if token not in texts[rel]:
            raise DiagramError("%s: the box \"%s\" follows `%s` of %s, which is gone: re-read the engine and update the flow (FLOW_CODE in this script)" % (spec.name, box, token, rel))
    listed = set(spec.follows)
    if listed != used:
        raise DiagramError("%s: `follows` must list exactly the files the boxes follow: missing %s, not used %s" % (spec.name, sorted(used - listed), sorted(listed - used)))
    for top in ("platform", "app"):
        for dirpath, _, files in os.walk(os.path.join(root, top)):
            for f in files:
                if f.endswith((".cpp", ".h")):
                    rel = os.path.relpath(os.path.join(dirpath, f), root)
                    with open(os.path.join(dirpath, f), encoding="utf-8", errors="replace") as fh:
                        if "install_master_stage(" in fh.read():
                            raise DiagramError("%s: %s installs a master stage: the flow draws the limiter as the only stage of the master chain" % (spec.name, rel))


def render_flow(root, spec, ctx):
    flow_check(root, spec)
    alt = spec.get("alt")
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="1220" height="640" viewBox="0 0 1220 640" role="img" aria-labelledby="t">',
         '<title id="t">%s</title>' % escape(alt),
         "<style>",
         ".b{fill:#3c3c3c}.x{fill:#4a4a4a;stroke:#262626;stroke-width:2}.g{fill:none;stroke:#7a7a7a;stroke-width:2;stroke-dasharray:6 4}",
         ".t{font-family:sans-serif;fill:#f4f4f4;text-anchor:middle;font-size:14px}.m{font-size:19px;font-weight:bold}",
         ".l{fill:#b0b0b0;font-size:13px}.a{fill:none;stroke:#cfcfcf;stroke-width:2.5}.d{stroke-dasharray:7 5;stroke:#b0b0b0}",
         ".i{fill:#6a4c00;stroke:%s;stroke-width:3}" % ACCENT,
         "</style>",
         '<defs><marker id="h" markerWidth="9" markerHeight="9" refX="8" refY="4.5" orient="auto"><path d="M0,0L9,4.5L0,9z" fill="#cfcfcf"/></marker></defs>',
         '<rect class="b" width="1220" height="640" rx="20"/>']

    def box(x, y, w, h, title, lines, cls="x"):
        o.append('<rect class="%s" x="%d" y="%d" width="%d" height="%d" rx="8"/>' % (cls, x, y, w, h))
        o.append('<text class="t m" x="%d" y="%d">%s</text>' % (x + w // 2, y + 26, escape(title)))
        for j, line in enumerate(lines):
            o.append('<text class="t" x="%d" y="%d">%s</text>' % (x + w // 2, y + 50 + 19 * j, escape(line)))

    def small(x, y, w, h, title):
        o.append('<rect class="x" x="%d" y="%d" width="%d" height="%d" rx="6"/>' % (x, y, w, h))
        o.append('<text class="t" x="%d" y="%d" style="font-weight:bold">%s</text>' % (x + w // 2, y + h // 2 + 5, escape(title)))

    def edge(pts, dashed=False):
        d = "M" + " L".join("%d,%d" % p for p in pts)
        o.append('<path class="a%s" d="%s" marker-end="url(#h)"/>' % (" d" if dashed else "", d))

    def label(x, y, text, anchor="start"):
        o.append('<text class="t l" x="%d" y="%d" style="text-anchor:%s">%s</text>' % (x, y, anchor, escape(text)))

    box(30, 60, 170, 110, "Instruments", ["sampler, Amiga,", "SD modules", "one voice a track"])
    box(250, 40, 240, 150, "Track strip", ["one for each track", "inserts (reserved)", "sends A and B", "level, pan, meter"])
    o.append('<rect class="g" x="550" y="30" width="190" height="196" rx="10"/>')
    label(645, 22, "Buses", "middle")
    small(570, 60, 150, 34, "MAIN")
    small(570, 113, 150, 34, "CUE")
    small(570, 166, 150, 34, "REC")
    box(250, 250, 220, 112, "Send effects", ["SND A: delay", "SND B: reverb", "returns to MAIN and CUE"])
    box(800, 37, 210, 80, "Master chain", ["MASTER LIMITER, always on", "then the master level"])
    box(1070, 30, 130, 200, "Outputs", ["line out and", "headphones", "OUT MODE:", "stereo MAIN, or", "MAIN left and", "CUE right"])
    box(550, 270, 190, 84, "Sample recorder", ["the REC bus, or", "MAIN as heard"])
    box(800, 270, 210, 150, "USB capture", ["to the computer (stems)", "tracks after their level", "(USB route bit),", "master L R,", "SND A, SND B"], "i")
    box(30, 400, 170, 70, "Line in", ["the input strip"])
    box(30, 500, 170, 70, "USB return", ["from the computer"], "i")
    edge([(200, 115), (250, 115)])
    edge([(490, 110), (550, 110)])
    label(520, 100, "route bits", "middle")
    edge([(370, 190), (370, 250)])
    label(378, 226, "sends")
    edge([(470, 300), (510, 300), (510, 170), (550, 170)])
    label(478, 322, "returns")
    edge([(200, 435), (530, 435), (530, 200), (550, 200)])
    label(380, 428, "route bits", "middle")
    edge([(720, 77), (800, 77)])
    edge([(1010, 77), (1070, 77)])
    edge([(720, 130), (1070, 130)])
    label(1040, 147, "cue level", "end")
    edge([(645, 226), (645, 270)])
    label(655, 264, "REC")
    edge([(450, 190), (450, 238), (775, 238), (775, 345), (800, 345)], True)  # the stems are per track, taken after the fader (finish_track), not from a bus
    label(540, 256, "post-fader")
    edge([(905, 117), (905, 270)], True)
    label(913, 215, "master")
    edge([(200, 535), (1135, 535), (1135, 230)])
    label(668, 526, "to the outputs after the limiter, at its own level times the master level", "middle")
    label(30, 612, "The metronome click and a looped take are added last: they are in no meter, take or stem.")
    o.append('<rect class="i" x="1040" y="596" width="24" height="24" rx="5"/>')
    label(1072, 614, "USB audio")
    o.append("</svg>")
    return "\n".join(o) + "\n", {}


# ---- The manifest -----------------------------------------------------------------------------------------------------------------------

class Spec:
    def __init__(self, name, lineno):
        self.name, self.lineno = name, lineno
        self.fields = {}
        self.binds = []  # (text, lineno)
        self.follows = []

    def get(self, key, default=None):
        return self.fields.get(key, default)


def parse_manifest(root):
    path = os.path.join(root, MANIFEST)
    specs, cur = [], None
    text = read(root, MANIFEST)
    for no, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.fullmatch(r"\[([a-z0-9_]+\.svg)\]", line)
        if m:
            cur = Spec(m.group(1), no)
            specs.append(cur)
            continue
        m = re.fullmatch(r"([a-z]+)\s*=\s*(.*)", line)
        if not m or cur is None:
            raise DiagramError("%s:%d: expected `[<name>.svg]` or `key = value`" % (path, no))
        key, val = m.group(1), m.group(2).strip()
        if key == "bind":
            cur.binds.append((val, no))
        elif key == "follows":
            cur.follows.extend(val.split())
        elif key in cur.fields:
            raise DiagramError("%s:%d: `%s` twice in [%s]" % (path, no, key, cur.name))
        else:
            cur.fields[key] = val
    names = [s.name for s in specs]
    for n in sorted(set(names)):
        if names.count(n) > 1:
            raise DiagramError("%s: [%s] twice" % (path, n))
    return specs


def resolve_bind(binds, text, no):
    """One manifest `bind` line -> (Bind, label)."""
    left, _, label = text.partition("|")
    toks = left.split()
    if len(toks) < 2:
        raise DiagramError("%s:%d: `bind = <Context> <action> [<arg>] [via <Button>] [tap|chord] [shift|note|plain] [| <label>]`" % (MANIFEST, no))
    ctx, action = toks[0], toks[1]
    arg, via, mod, kind, i = None, None, "any", None, 2
    while i < len(toks):
        if toks[i] == "via" and i + 1 < len(toks):
            via, i = toks[i + 1], i + 2
        elif toks[i] in ("tap", "chord"):
            kind, i = toks[i], i + 1
        elif toks[i] in ("shift", "note", "plain"):
            mod, i = {"shift": "Shift", "note": "Note", "plain": None}[toks[i]], i + 1
        else:
            try:
                arg = int(toks[i], 0)
            except ValueError:
                raise DiagramError("%s:%d: cannot read `%s` in the bind line" % (MANIFEST, no, toks[i]))
            i += 1
    hits = [b for b in binds if b.ctx == ctx and b.action == action and (arg is None or b.arg == arg) and (via is None or via in b.buttons)
            and (kind is None or b.kind == kind) and (mod == "any" or b.mod == mod)]
    if not hits:
        near = [str(b) for b in binds if b.action == action or (b.ctx == ctx and arg is not None and b.arg == arg)][:4]
        raise DiagramError("%s:%d: no row of %s binds %s %s%s in %s%s" % (MANIFEST, no, BINDINGS, action, "" if arg is None else arg, "" if via is None else " via " + via,
                                                                   ctx, ("; near: " + "; ".join(near)) if near else ""))
    if len(hits) > 1:
        raise DiagramError("%s:%d: %d rows of %s bind %s in %s (add the argument or `via <Button>`): %s" % (MANIFEST, no, len(hits), BINDINGS, action, ctx, "; ".join(str(h) for h in hits)))
    return hits[0], label.strip()


def merge_role(roles, code, role, what):
    if roles.get(code, role) != role:
        raise DiagramError("%s: %s is held in one binding and pressed in another" % (what, code))
    roles[code] = role


def chord_keys(root, spec, codes, binds_cache):
    """(roles {code|'Power': role}, labels, press_text, hold_seconds, resolved binds) of a chord stanza."""
    roles, labels, together, resolved = {}, {}, False, []
    src = spec.get("source")
    seconds = None
    if src and spec.binds:
        raise DiagramError("%s:%d: [%s] has both `source` and `bind`" % (MANIFEST, spec.lineno, spec.name))
    if src == "boot-chords":
        chords = boot_chords(root)
        for k in chords["input-test"] + chords["disk"]:
            roles[codes[k]] = "hold"
        for k in chords["input-test"][2:]:  # past Shift and F1: the key(s) that make the input test
            labels[codes[k]] = "TEST"
        for k in chords["disk"][2:]:
            labels[codes[k]] = "DISK"
    elif src == "power-hold":
        seconds = power_hold_seconds(root)
        roles["Power"] = "hold"
        labels["Power"] = "%g s" % seconds
    elif src:
        raise DiagramError("%s:%d: unknown source `%s`" % (MANIFEST, spec.lineno, src))
    else:
        if not spec.binds:
            raise DiagramError("%s:%d: [%s] has neither `source` nor `bind`" % (MANIFEST, spec.lineno, spec.name))
        if binds_cache[0] is None:
            binds_cache[0] = bindings(root)
        for text, no in spec.binds:
            b, label = resolve_bind(binds_cache[0], text, no)
            resolved.append(b)
            what = "[%s] %s" % (spec.name, text.partition("|")[0].strip())
            if b.kind == "chord":
                together = True
                for k in b.buttons:
                    merge_role(roles, codes[k], "press", what)
                main = [codes[k] for k in b.buttons]
            else:
                if b.mod:
                    merge_role(roles, codes[b.mod], "hold", what)
                merge_role(roles, codes[b.buttons[0]], "hold" if b.kind == "hold" else "press", what)
                main = [codes[b.buttons[0]]]
            if label:
                for code in main:
                    if labels.setdefault(code, label) != label:
                        raise DiagramError("%s: two different labels for the same key" % what)
    npress = sum(1 for r in roles.values() if r == "press")
    press_text = "press together" if together else ("press one of them" if npress > 1 else "press")
    return roles, labels, press_text, seconds, resolved


def shift_row_nothing(binds, spec):
    """The "F5 does nothing." sentence of a Shift row: the soft keys F1..F8 that no row of the stanza's context(s) binds with Shift."""
    if not spec.binds:
        raise DiagramError("%s:%d: [%s] uses `{nothing}` but has no `bind`" % (MANIFEST, spec.lineno, spec.name))
    ctxs = {text.split()[0] for text, _ in spec.binds}  # (resolve_bind checks each row; the context is its first word)
    bound = {b.buttons[0] for b in binds if b.ctx in ctxs and b.mod == "Shift" and b.kind != "chord" and b.action != "ui.none"}
    free = ["F%d" % i for i in range(1, 9) if "F%d" % i not in bound]
    if not free:
        return ""
    return "%s %s nothing." % (" and ".join(free) if len(free) < 3 else ", ".join(free[:-1]) + " and " + free[-1], "does" if len(free) == 1 else "do")


def render_spec(root, spec, ctx):
    kind = spec.get("kind")
    alt, caption = spec.get("alt"), spec.get("caption")
    for key in ("kind", "chapter", "alt"):
        if not spec.get(key):
            raise DiagramError("%s:%d: [%s] has no `%s`" % (MANIFEST, spec.lineno, spec.name, key))
    ctx.setdefault("binds", [None])
    if kind in ("chord", "map") and "panel" not in ctx:
        ctx["panel"] = Panel(read(root, PANEL))
        ctx["codes"] = button_codes(root)
    if len(alt.split()) > MAX_ALT_WORDS:
        raise DiagramError("%s:%d: the alt text of [%s] is %d words, at most %d" % (MANIFEST, spec.lineno, spec.name, len(alt.split()), MAX_ALT_WORDS))
    if kind == "chord":
        if not caption:
            raise DiagramError("%s:%d: [%s] has no `caption`" % (MANIFEST, spec.lineno, spec.name))
        roles, labels, press_text, seconds, resolved = chord_keys(root, spec, ctx["codes"], ctx["binds"])
        names = parse_region(spec, roles)
        if count_keys(roles, labels) > MAX_KEYS:
            raise DiagramError("%s:%d: [%s] marks %d keys, at most %d (keys that share a label count once): split the figure or say it in words"
                               % (MANIFEST, spec.lineno, spec.name, count_keys(roles, labels), MAX_KEYS))
        shift_row = ["F%d" % i for i in range(1, 9)]
        if resolved and all(b.mod == "Shift" and b.kind != "chord" and b.buttons[0] in shift_row for b in resolved) and not spec.get("why"):
            raise DiagramError("%s:%d: [%s] shows only Shift + F1..F8, a Shift row: the unit labels it, so say once in the chapter that Shift relabels F1 to F8. "
                               "Add `why = <what this figure shows that the key row does not>` if it earns its place" % (MANIFEST, spec.lineno, spec.name))
        if seconds is not None:
            caption = caption.replace("{hold_s}", "%g" % seconds)
            alt = alt.replace("{hold_s}", "%g" % seconds)
        if "{nothing}" in alt:
            alt = alt.replace("{nothing}", shift_row_nothing(ctx["binds"][0], spec)).strip()  # (chord_keys has read the bindings: `{nothing}` needs `bind` lines)
        if alt != spec.get("alt"):
            spec.fields["alt"] = alt  # the chapter shows the alt text as written out
            if len(alt.split()) > MAX_ALT_WORDS:
                raise DiagramError("%s:%d: the alt text of [%s] is %d words, at most %d" % (MANIFEST, spec.lineno, spec.name, len(alt.split()), MAX_ALT_WORDS))
        if "{" in caption or "{" in alt:
            raise DiagramError("%s:%d: `{...}` left in the caption or the alt text of [%s]" % (MANIFEST, spec.lineno, spec.name))
        if len(caption) > MAX_CAPTION:
            raise DiagramError("%s:%d: the caption of [%s] is %d characters, at most %d" % (MANIFEST, spec.lineno, spec.name, len(caption), MAX_CAPTION))
        return render_chord(ctx["panel"], alt, caption, roles, labels, press_text, names), labels
    if kind == "map":
        return render_map(root, spec, ctx)
    if kind == "flow":
        return render_flow(root, spec, ctx)
    raise DiagramError("%s:%d: [%s]: unknown kind `%s`" % (MANIFEST, spec.lineno, spec.name, kind))


def render_all(root, specs):
    """{name: (text, labels)} and {name: error} for every stanza."""
    ctx, out, errors = {}, {}, {}
    for spec in specs:
        try:
            out[spec.name] = render_spec(root, spec, ctx)
        except DiagramError as e:
            errors[spec.name] = str(e)
    return out, errors


def md_line(spec):
    return "![%s](img/%s)" % (spec.get("alt", ""), spec.name)


def check(root, specs):
    problems = []
    out, errors = render_all(root, specs)
    for spec in specs:
        name = spec.name
        if name in errors:
            problems.append("%s: %s" % (name, errors[name]))
            continue
        text, labels = out[name]
        path = os.path.join(root, IMG, name)
        if not os.path.exists(path):
            problems.append("%s: missing (run tools/manual_diagrams.py and commit docs/manual/img/)" % name)
        else:
            with open(path, encoding="utf-8", newline="") as f:
                if f.read() != text:
                    problems.append("%s: stale (the manifest, the bindings or the generator changed: run tools/manual_diagrams.py and commit)" % name)
        if len(text.encode("utf-8")) > MAX_BYTES:
            problems.append("%s: %d bytes, more than %d" % (name, len(text.encode("utf-8")), MAX_BYTES))
        chapter = spec.get("chapter", "")
        cpath = os.path.join(root, "docs", "manual", chapter)
        if not chapter or not os.path.isfile(cpath):
            problems.append("%s: chapter `%s` is not in docs/manual/" % (name, chapter))
            continue
        ctext = read(root, "docs/manual/" + chapter)
        if md_line(spec) not in ctext:
            problems.append("%s: %s does not contain the line %s" % (name, chapter, md_line(spec)))
        low = ctext.lower()
        for label in sorted(set(labels.values())):
            if sum(c.isalpha() for c in label) >= 3 and label.lower().strip("<>") not in low:
                problems.append("%s: the label `%s` does not appear in %s" % (name, label, chapter))
    by_chapter = {}
    for spec in specs:
        if spec.get("kind") == "chord":
            by_chapter.setdefault(spec.get("chapter", ""), []).append(spec)
    for chapter, group in sorted(by_chapter.items()):
        for spec in group[1:]:
            if not spec.get("extra"):
                problems.append("%s: %s shows %d chord diagrams; this is not the first: add `extra = <why it earns its place>` to its stanza, or drop it"
                                % (spec.name, chapter, len(group)))
    listed = {s.name for s in specs}
    imgdir = os.path.join(root, IMG)
    for n in sorted(os.listdir(imgdir)) if os.path.isdir(imgdir) else []:
        if n.endswith(".svg") and n != "panel.svg" and n not in listed:
            problems.append("%s: in docs/manual/img/ but not in %s (a generated SVG is listed there or removed)" % (n, MANIFEST))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--check", action="store_true", help="write nothing; exit 1 when a diagram is stale, missing, unlisted, too big or not in its chapter")
    ap.add_argument("--list", action="store_true", help="print the manifest with the Markdown line of each diagram")
    args = ap.parse_args(argv)
    root = args.root
    try:
        specs = parse_manifest(root)
        if args.list:
            render_all(root, specs)  # resolves the {hold_s} of the alt texts
            for s in specs:
                print("%-34s %-22s %s" % (s.name, s.get("chapter", "?"), md_line(s)))
            print("%d diagrams" % len(specs))
            return 0
        if args.check:
            problems = check(root, specs)
            for p in problems:
                print("manual_diagrams: " + p, file=sys.stderr)
            total = 0
            for s in specs:
                path = os.path.join(root, IMG, s.name)
                total += os.path.getsize(path) if os.path.exists(path) else 0
            print("manual_diagrams --check: %d diagrams, %d bytes, %d problem(s)" % (len(specs), total, len(problems)))
            return 1 if problems else 0
        out, errors = render_all(root, specs)
        for name, err in errors.items():
            print("manual_diagrams: %s: %s" % (name, err), file=sys.stderr)
        if errors:
            return 1
        os.makedirs(os.path.join(root, IMG), exist_ok=True)
        total = 0
        for name, (text, _) in out.items():
            with open(os.path.join(root, IMG, name), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            total += len(text.encode("utf-8"))
        print("manual_diagrams: wrote %d diagrams, %d bytes, to %s" % (len(out), total, IMG))
        return 0
    except DiagramError as e:
        print("manual_diagrams: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
