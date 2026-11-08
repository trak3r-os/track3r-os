#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ce_usb_descriptor.py (OS-CECHECK K4): the USB configuration descriptor of the official firmware's source
(`polyend/tracker-ce`, a read-only checkout given by --ce-root) next to ours, as a byte table, with the differing
descriptors and fields named. Host only, standard library plus the host C preprocessor (`cc -E`, no compile); run it
with `python3 -I`. Nothing of the checkout is copied into this repository: the script reads it at run time and prints
numbers and field names.

  ce_usb_descriptor.py --ce-root DIR [--usb-type USB_MIDI] [--ours kStandardFS] [--bytes]

tracker-ce side: the checkout's Teensy core describes the device per USB type (a -DUSB_xxx define); the MK66 makefile
of the checkout names the type the Tracker build uses (found by reading it, USB_MIDI today). The script gathers the
type's #define block and the size macros plus the `config_descriptor[]` initializer, preprocesses that text and
evaluates every item.

Our side: the golden configuration descriptors of tests/ (the builder in usb/src is checked byte for byte against
them by usb_descriptor_test), read by name. `kStandardFS` is the Tracker-compatible STANDARD mode at full speed.

Exit 0 when both sides parse (the differences are the result, not a failure); 2 when a side cannot be read.
"""
import argparse
import os
import re
import subprocess
import sys

TYPE_NAMES = {1: "DEVICE", 2: "CONFIG", 4: "INTERFACE", 5: "ENDPOINT", 0x0B: "IAD", 0x21: "HID", 0x24: "CS_INTERFACE",
              0x25: "CS_ENDPOINT"}

# Field names per descriptor type (byte offset inside the descriptor, width, name). CS_* types are decoded below.
FIELDS = {
    2: [(2, 2, "wTotalLength"), (4, 1, "bNumInterfaces"), (5, 1, "bConfigurationValue"), (6, 1, "iConfiguration"),
        (7, 1, "bmAttributes"), (8, 1, "bMaxPower")],
    0x0B: [(2, 1, "bFirstInterface"), (3, 1, "bInterfaceCount"), (4, 1, "bFunctionClass"), (5, 1, "bFunctionSubClass"),
           (6, 1, "bFunctionProtocol"), (7, 1, "iFunction")],
    4: [(2, 1, "bInterfaceNumber"), (3, 1, "bAlternateSetting"), (4, 1, "bNumEndpoints"), (5, 1, "bInterfaceClass"),
        (6, 1, "bInterfaceSubClass"), (7, 1, "bInterfaceProtocol"), (8, 1, "iInterface")],
    5: [(2, 1, "bEndpointAddress"), (3, 1, "bmAttributes"), (4, 2, "wMaxPacketSize"), (6, 1, "bInterval")],
    0x21: [(2, 2, "bcdHID"), (4, 1, "bCountryCode"), (5, 1, "bNumDescriptors"), (6, 1, "bReportType"), (7, 2, "wReportLength")],
}

# Fields of the class-specific interface descriptors that matter here: by (interface class, subclass, subtype) with the
# subtype at offset 2. Anything not listed is compared byte by byte as "data[n]".
CS_SUBTYPES = {
    (2, 0x00): "CDC Header", (2, 0x01): "CDC Call Management", (2, 0x02): "CDC ACM", (2, 0x06): "CDC Union",
    (1, 0x01): "MIDI/AC Header", (1, 0x02): "MIDI IN Jack", (1, 0x03): "MIDI OUT Jack",
}


def die(msg):
    print("ce_usb_descriptor: " + msg, file=sys.stderr)
    sys.exit(2)


def path(root, *parts):
    return os.path.join(root, *parts)


# ---- tracker-ce side -------------------------------------------------------------------------------------------

def find_usb_type(root):
    """The -DUSB_xxx of the MK66 makefile of the checkout."""
    mk = path(root, "platforms", "mk66", "makefile")
    try:
        text = open(mk, encoding="utf-8", errors="replace").read()
    except OSError as e:
        die("cannot read the MK66 makefile of the checkout: %s" % e)
    found = re.findall(r"-D(USB_[A-Z0-9_]+)", text)
    if not found:
        die("no -DUSB_xxx in the MK66 makefile")
    return found[0]


def teensy_core(root):
    return path(root, "platforms", "mk66", "3rdparty", "teensy_core")


def usb_type_block(lines, usb_type):
    """The text between `#elif defined(USB_xxx)` (or `#if defined`) and the next #elif/#else/#endif at the same level."""
    start = None
    for i, ln in enumerate(lines):
        if re.match(r"\s*#\s*(el)?if\s+defined\(%s\)\s*$" % re.escape(usb_type), ln):
            start = i + 1
            break
    if start is None:
        die("USB type %s not found in the Teensy core's descriptor definitions" % usb_type)
    depth = 0
    out = []
    for ln in lines[start:]:
        m = re.match(r"\s*#\s*(\w+)", ln)
        if m:
            d = m.group(1)
            if d in ("if", "ifdef", "ifndef"):
                depth += 1
            elif d == "endif":
                if depth == 0:
                    break
                depth -= 1
            elif d in ("elif", "else") and depth == 0:
                break
        out.append(ln)
    return out


def config_region(lines):
    """From the size macros to the end of the config_descriptor[] initializer."""
    a = next((i for i, ln in enumerate(lines) if re.match(r"\s*#define\s+CONFIG_HEADER_DESCRIPTOR_SIZE\b", ln)), None)
    b = next((i for i, ln in enumerate(lines) if re.match(r"\s*static\s+uint8_t\s+config_descriptor\[", ln)), None)
    if a is None or b is None or b < a:
        die("the config descriptor definition was not found in the Teensy core")
    end = next((i for i in range(b, len(lines)) if re.match(r"\s*};", lines[i])), None)
    if end is None:
        die("the end of the config descriptor initializer was not found")
    # the HID report descriptor arrays come first: the config descriptor takes their sizeof()
    r = next((i for i, ln in enumerate(lines) if re.match(r"\s*static\s+uint8_t\s+\w+_report_desc\[\]", ln)), None)
    pre = []
    if r is not None and r < a:
        while r > 0 and not re.match(r"\s*#\s*if", lines[r]):
            r -= 1
        pre = lines[r:a]
    return pre + lines[a:end + 1], b - a


def preprocess(text):
    try:
        p = subprocess.run(["cc", "-E", "-P", "-x", "c", "-"], input=text, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as e:
        die("the host C preprocessor (cc -E) is needed: %s" % e)
    if p.returncode != 0:
        die("cc -E failed: %s" % p.stderr.strip()[:300])
    return p.stdout


SAFE = re.compile(r"^[0-9xXa-fA-F\s|&+*()<>-]*$")


def eval_item(expr):
    expr = expr.strip()
    if not expr or not SAFE.match(expr):
        die("unexpected item in the preprocessed initializer: %r" % expr[:60])
    return eval(expr, {"__builtins__": {}}, {}) & 0xFFFF  # noqa: S307 (digits and operators only, checked above)


def ce_bytes(root, usb_type):
    core = teensy_core(root)
    hdr = path(core, "usb_desc." + "h")
    src = path(core, "usb_desc." + "c")
    try:
        hlines = open(hdr, encoding="utf-8", errors="replace").read().splitlines()
        slines = open(src, encoding="utf-8", errors="replace").read().splitlines()
    except OSError as e:
        die("cannot read the Teensy core descriptor files: %s" % e)
    block = usb_type_block(hlines, usb_type)
    region, _ = config_region(slines)
    text = "\n".join(["#define LSB(n) ((n) & 255)", "#define MSB(n) (((n) >> 8) & 255)", "#define " + usb_type + " 1"]
                     + block + region) + "\n"
    out = preprocess(text)
    m = re.search(r"config_descriptor\s*\[[^\]]*\]\s*=\s*\{(.*)\};", out, re.S)
    if not m:
        die("the preprocessed config descriptor could not be matched")
    sizes = {}
    for am in re.finditer(r"(\w+_report_desc)\s*\[\s*\]\s*=\s*\{(.*?)\};", out, re.S):
        sizes[am.group(1)] = len([x for x in am.group(2).split(",") if x.strip()])
    body = re.sub(r"sizeof\s*\(\s*(\w+)\s*\)", lambda mm: str(sizes.get(mm.group(1), 0)), m.group(1))
    items = [s for s in body.replace("\n", " ").split(",") if s.strip()]
    declared = re.search(r"config_descriptor\s*\[([^\]]*)\]", out).group(1)
    data = [eval_item(s) & 0xFF for s in items]
    return data, eval_item(declared)


# ---- our side --------------------------------------------------------------------------------------------------

def our_bytes(root, name):
    f = path(root, "tests", "usb_golden." + "h")
    try:
        text = open(f, encoding="utf-8", errors="replace").read()
    except OSError as e:
        die("cannot read our golden descriptors: %s" % e)
    m = re.search(r"\b%s\s*\[\s*\d+\s*\]\s*=\s*\{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        die("golden descriptor %s not found" % name)
    return [int(x, 16) for x in re.findall(r"0[xX][0-9A-Fa-f]{1,2}", m.group(1))]


# ---- decoding and comparison -----------------------------------------------------------------------------------

def split_descriptors(data):
    """[(offset, bytes, context)]; the context is the (class, subclass) of the last interface descriptor."""
    out = []
    pos = 0
    cur = None
    while pos < len(data):
        n = data[pos]
        if n < 2 or pos + n > len(data):
            die("malformed descriptor at offset %d (bLength %d)" % (pos, n))
        d = data[pos:pos + n]
        if d[1] == 4:
            cur = (d[5], d[6])
        out.append((pos, d, cur))
        pos += n
    return out


def label(desc):
    off, d, cur = desc
    t = d[1]
    name = TYPE_NAMES.get(t, "0x%02X" % t)
    if t == 0x24 and cur is not None and len(d) > 2:
        sub = CS_SUBTYPES.get((cur[0], d[2]) if cur[0] == 2 else (1, d[2]))
        if cur[0] == 1 and cur[1] == 1 and d[2] != 1:
            sub = None
        name = "%s %s" % (name, sub or "subtype 0x%02X" % d[2])
    elif t == 4:
        name = "INTERFACE %d alt %d class %02X/%02X/%02X" % (d[2], d[3], d[5], d[6], d[7])
    elif t == 5:
        name = "ENDPOINT 0x%02X %s" % (d[2], {0: "control", 1: "iso", 2: "bulk", 3: "interrupt"}.get(d[3] & 3, "?"))
    return name


def key(desc):
    """What identifies a descriptor for alignment: type and, for CS descriptors, the subtype."""
    off, d, cur = desc
    return (d[1], d[2] if d[1] in (0x24, 0x25) else 0, d[3] & 3 if d[1] == 5 else 0, len(d))


def field_diffs(a, b):
    """Named fields (or data[n]) of two descriptors of the same key that differ."""
    names = FIELDS.get(a[1], [])
    covered = set()
    out = []
    for off, width, name in names:
        covered.update(range(off, off + width))
        va = int.from_bytes(bytes(a[off:off + width]), "little")
        vb = int.from_bytes(bytes(b[off:off + width]), "little")
        if va != vb:
            out.append((off, name, va, vb, width))
    for i in range(2, min(len(a), len(b))):
        if i not in covered and a[i] != b[i]:
            out.append((i, "data[%d]" % (i - 2), a[i], b[i], 1))
    return out


def align(da, db):
    import difflib
    sm = difflib.SequenceMatcher(None, [key(x) for x in da], [key(x) for x in db], autojunk=False)
    return sm.get_opcodes()


def hexrow(d, width=16):
    return " ".join("%02X" % x for x in d[:width]) + (" .." if len(d) > width else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--ce-root", required=True, help="root of a tracker-ce checkout (read-only)")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    help="root of this repository (default: the script's)")
    ap.add_argument("--usb-type", help="the USB type define (default: read from the checkout's MK66 makefile)")
    ap.add_argument("--ours", default="kStandardFS", help="golden descriptor of this OS to compare (default kStandardFS)")
    ap.add_argument("--bytes", action="store_true", help="also print every descriptor's bytes in full")
    a = ap.parse_args()

    usb_type = a.usb_type or find_usb_type(a.ce_root)
    ce, declared = ce_bytes(a.ce_root, usb_type)
    ours = our_bytes(a.root, a.ours)
    dce, dours = split_descriptors(ce), split_descriptors(ours)

    print("tracker-ce: type %s, %d bytes (wTotalLength %d, declared size %d); this OS: %s, %d bytes (wTotalLength %d)" % (
        usb_type, len(ce), ce[2] | ce[3] << 8, declared, a.ours, len(ours), ours[2] | ours[3] << 8))
    print()
    print("%-5s %-44s | %-5s %-44s | %s" % ("ce@", "tracker-ce descriptor", "our@", "this OS descriptor", "result"))
    diffs = []
    for tag, i1, i2, j1, j2 in align(dce, dours):
        if tag == "equal":
            for x, y in zip(dce[i1:i2], dours[j1:j2]):
                fd = field_diffs(x[1], y[1])
                res = "same" if not fd else "differs: " + ", ".join(
                    "%s %s -> %s" % (n, hex(va), hex(vb)) for _, n, va, vb, _ in fd)
                print("%-5d %-44s | %-5d %-44s | %s" % (x[0], label(x), y[0], label(y), res))
                for off, n, va, vb, w in fd:
                    diffs.append((x[0] + off, y[0] + off, label(y), n, va, vb))
        else:
            for k in range(max(i2 - i1, j2 - j1)):
                x = dce[i1 + k] if i1 + k < i2 else None
                y = dours[j1 + k] if j1 + k < j2 else None
                res = "only in tracker-ce" if y is None else ("only in this OS" if x is None else "different kind")
                print("%-5s %-44s | %-5s %-44s | %s" % (x[0] if x else "-", label(x) if x else "", y[0] if y else "-",
                                                         label(y) if y else "", res))
    print()
    print("Differing fields of matched descriptors (ce offset, our offset, descriptor, field, ce value, our value):")
    for co, oo, lab, n, va, vb in diffs:
        print("  ce@%d our@%d  %-40s %-18s ce=0x%X ours=0x%X" % (co, oo, lab, n, va, vb))
    if a.bytes:
        print()
        for nm, ds in (("tracker-ce", dce), ("this OS", dours)):
            print(nm + ":")
            for off, d, _ in ds:
                print("  %4d: %s" % (off, " ".join("%02X" % x for x in d)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
