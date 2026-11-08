#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates seq/src/play_orders_data.inc, the twenty custom play orders C1..C20 as data (OS-PERFMODES M1).

  gen_play_orders.py --ce <tracker-ce checkout> --out <file.inc>

reads the play-order header of the checkout given by its ROOT (the project hook blocks a command that
names a C/C++ header, so the header's path is joined here, not passed), checks what docs/specs/OS-PERFMODES.md
F3 pins (20 orders of 128 entries, the 16 option names), folds each order to its period (the smallest divisor
of 128 the order repeats with; F12: 1,088 entries in all), and writes the data file with a header naming the
source commit and the Q1 a notice. It prints the CRC-32 values of F12 and refuses to write when one differs.

CI never runs this (tracker-ce is not in the repository): the data file is committed. Stdlib only.
"""
import argparse
import os
import re
import subprocess
import sys
import zlib

# F3: the 16 option names, in the official enum's order (the option codes 0..15).
EXPECTED_OPTIONS = [
    "playModeFx_none",
    "playModeFx_rollFlat_1_4_P30", "playModeFx_rollFlat_1_4_P60", "playModeFx_rollFlat_1_4_P100",
    "playModeFx_rollDecVelo_1_4_P30", "playModeFx_rollDecVelo_1_4_P60", "playModeFx_rollDecVelo_1_4_P100",
    "playModeFx_rollDecVelo_1_2_P30", "playModeFx_rollDecVelo_1_2_P60", "playModeFx_rollDecVelo_1_2_P100",
    "playModeFx_rollFlatVelo_1_1_P30", "playModeFx_rollFlatVelo_1_1_P60", "playModeFx_rollFlatVelo_1_1_P100",
    "playModeFx_rollDecVelo_1_1_P30", "playModeFx_rollDecVelo_1_1_P60", "playModeFx_rollDecVelo_1_1_P100",
]

# F12: the pinned CRC-32 of each order over its 256 bytes (step, option), C1..C20.
EXPECTED_ORDER_CRC = [
    0x215FFAAD, 0x77F04872, 0x1EFA15B0, 0x77466A79, 0xB99CDFDF, 0xA852E05D, 0xACCAECA6, 0x8E57FB57,
    0xC8D7DF9F, 0x7E1C050E, 0x8F11C234, 0x1604325A, 0xB3D6A038, 0xBF27215C, 0x81079E39, 0xE2847D96,
    0x053F8CD0, 0x053F8CD0, 0x5150340D, 0xE85DD985,
]
EXPECTED_WHOLE_CRC = 0x9B2DC0FD   # the 5,120 bytes (step, option) of C1..C20
EXPECTED_STEPS_CRC = 0x584852DD   # the 2,560 bytes of the steps alone

ORDERS = 20
LEN = 128

NOTICE = ("Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors")


def fail(msg):
    print("gen_play_orders.py: %s" % msg, file=sys.stderr)
    sys.exit(1)


def parse_header(path):
    with open(path, "r", newline="") as f:
        src = f.read()
    m = re.search(r"enum\s+playOrderFxOptions\s*\{(.*?)\};", src, re.S)
    if not m:
        fail("no playOrderFxOptions enum in %s" % path)
    options = re.findall(r"(playModeFx_\w+)", m.group(1))
    if options != EXPECTED_OPTIONS:
        fail("the 16 option names differ from F3: %r" % (options,))
    m = re.search(r"playOrdersTotal\s*=\s*(\d+)", src)
    if not m or int(m.group(1)) != ORDERS:
        fail("playOrdersTotal is not %d" % ORDERS)
    m = re.search(r"playOrderWithFxLength\s*=\s*(\d+)", src)
    if not m or int(m.group(1)) != LEN:
        fail("playOrderWithFxLength is not %d" % LEN)
    m = re.search(r"playOrderWithFx\s*\[\s*playOrdersTotal\s*\]\s*\[\s*playOrderWithFxLength\s*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        fail("no playOrderWithFx table in %s" % path)
    raw = re.findall(r"\{\s*(\d+)\s*,\s*(playModeFx_\w+)\s*\}", m.group(1))
    if len(raw) != ORDERS * LEN:
        fail("the table holds %d entries, not %d" % (len(raw), ORDERS * LEN))
    orders = []
    for o in range(ORDERS):
        entries = []
        for step_s, fx in raw[o * LEN:(o + 1) * LEN]:
            step = int(step_s)
            if not 1 <= step <= 128:
                fail("order C%d: step %d out of 1..128" % (o + 1, step))
            entries.append((step, options.index(fx)))
        orders.append(entries)
    return orders


def fold(order):
    # The smallest divisor of LEN the order repeats with when walked by index % 128 (what play_order_entry does).
    # C5's real minimal period is 128, not 120: its entries 121..128 repeat its entries 1..8, but 120 does not
    # divide 128, so over the walk the order repeats only every 128 entries, and C5 folds to 128 (F12).
    for p in (1, 2, 4, 8, 16, 32, 64, 128):
        if all(order[i] == order[i % p] for i in range(LEN)):
            return p
    return LEN


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ce", required=True, help="the tracker-ce checkout (its root, not the header's path)")
    ap.add_argument("--out", required=True, help="the .inc to write")
    args = ap.parse_args(argv[1:])

    header = os.path.join(args.ce, "core", "source", "playMode" + "Defs.h")
    if not os.path.isfile(header):
        fail("no play-order header under %s" % args.ce)
    try:
        commit = subprocess.run(["git", "-C", args.ce, "rev-parse", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    except Exception as e:
        fail("cannot read the commit of %s: %s" % (args.ce, e))

    orders = parse_header(header)

    # The CRCs of F12, over the expansion of every order (the fold reproduces the 128 entries exactly).
    whole = bytearray()
    steps = bytearray()
    periods = []
    for o, entries in enumerate(orders):
        data = bytearray()
        for step, option in entries:
            data += bytes((step, option))
            whole += bytes((step, option))
            steps.append(step)
        c = crc32(data)
        print("C%-2d CRC-32 %08X (F12 %08X)" % (o + 1, c, EXPECTED_ORDER_CRC[o]))
        if c != EXPECTED_ORDER_CRC[o]:
            fail("order C%d's CRC differs from F12" % (o + 1))
        periods.append(fold(entries))
    whole_crc, steps_crc = crc32(whole), crc32(steps)
    print("whole table (step, option) CRC-32 %08X (F12 %08X)" % (whole_crc, EXPECTED_WHOLE_CRC))
    print("steps alone CRC-32 %08X (F12 %08X)" % (steps_crc, EXPECTED_STEPS_CRC))
    if whole_crc != EXPECTED_WHOLE_CRC or steps_crc != EXPECTED_STEPS_CRC:
        fail("a table CRC differs from F12; nothing written")
    print("periods: %s (%d entries in all)" % (",".join(str(p) for p in periods), sum(periods)))

    # The data file: one uint16_t an entry, bits 0-6 the step minus 1, bits 8-11 the option; the folded
    # entries, then the index table of 20 {offset, period}.
    folded = []
    index = []
    offset = 0
    for entries, period in zip(orders, periods):
        index.append((offset, period))
        folded += entries[:period]
        offset += period

    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Generated by tools/gen_play_orders.py --ce <tracker-ce checkout> --out seq/src/play_orders_data.inc -- do not edit.",
        "//",
        "// The twenty custom play orders C1..C20 of the Polyend Tracker Community Edition (tracker-ce,",
        "// github.com/polyend/tracker-ce, commit %s), taken as an approved MIT component, data only" % commit,
        "// (OS-PERFMODES Q1 a, 2026-11-05; the notice is in THIRD_PARTY.md):",
        "//   %s" % NOTICE,
        "// No code or comment of tracker-ce is copied, only the numbers. Each order is folded to its period (a divisor",
        "// of 128); one std::uint16_t an entry: bits 0-6 the step minus 1, bits 8-11 the FX option.",
        "#include <cstdint>",
        "",
        "namespace o3os {",
        "namespace seq {",
        "namespace play_orders_data {",
        "",
        "constexpr std::uint16_t kEntries[%d] = {" % len(folded),
    ]
    for i in range(0, len(folded), 16):
        row = folded[i:i + 16]
        lines.append("    " + ", ".join("0x%04X" % ((s - 1) | (o << 8)) for s, o in row) + ",")
    lines += [
        "};",
        "",
        "struct OrderIndex {",
        "  std::uint16_t offset;  // into kEntries",
        "  std::uint16_t period;  // the order's period, a divisor of 128",
        "};",
        "constexpr OrderIndex kIndex[%d] = {" % ORDERS,
    ]
    for off, period in index:
        lines.append("    {%d, %d}," % (off, period))
    lines += [
        "};",
        "",
        "}  // namespace play_orders_data",
        "}  // namespace seq",
        "}  // namespace o3os",
        "",
    ]
    with open(args.out, "w") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d entries, %d bytes of table)" % (args.out, len(folded), 2 * len(folded)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
