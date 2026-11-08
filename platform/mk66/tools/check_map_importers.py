#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The image must not link the MOD / S3M / MED importers (stdlib only).

    check_map_importers.py check MAP
    check_map_importers.py --self-test

OS-FLASHDIET I3: from that slice the image reads MOD / S3M / MED only through the importer module of the card
(modules/importers/tracker.tmod), so nothing of the formats_tracker library (formats/mod, formats/med) may be linked into
o3os.elf. The link map (-Wl,-Map=...) is the proof: this check fails when any line of it names formats_tracker (its archive
libo3os_formats_tracker.a or its object directory), and when any symbol of the o3os::formats::mod / o3os::formats::med
namespace is linked other than the probe side that stays in formats (src/tracker_sniff.cpp): mod::sniff_kind with its two
helpers tagged_mod_min_bytes and is_soundtracker15, which ModuleImport::decide calls, the two drop_group_name tables
which ModuleSummary::group_name reads, and mod_tag, the one copy of the MOD tag check, which the reader side
(formats_tracker) calls from the probe side. Those five probe symbols must be present (the image references them),
so the check cannot pass vacuously.
"""
import argparse
import sys

# The namespaces of the importers, mangled (7formats3mod / 7formats3med) and demangled (the linker prints demangled names
# by default). REQUIRED: the five probe symbols the image must link from formats/src/tracker_sniff.cpp, each as
# (name, mangled fragment, demangled fragment); every one of them must be in the map, so the check cannot pass
# vacuously. ALLOWED: every fragment of REQUIRED plus the shared mod_tag helper (the one copy sits in the probe side;
# the reader of formats_tracker calls it from there, so the image carries it too).
NS_FRAGMENTS = ("7formats3mod", "7formats3med", "o3os::formats::mod::", "o3os::formats::med::")
REQUIRED = (
    ("sniff_kind", "10sniff_kind", "o3os::formats::mod::sniff_kind("),
    ("tagged_mod_min_bytes", "20tagged_mod_min_bytes", "o3os::formats::mod::tagged_mod_min_bytes("),
    ("is_soundtracker15", "17is_soundtracker15", "o3os::formats::mod::is_soundtracker15("),
    ("mod::drop_group_name", "3mod15drop_group_name", "o3os::formats::mod::drop_group_name("),
    ("med::drop_group_name", "3med15drop_group_name", "o3os::formats::med::drop_group_name("),
)
ALLOWED = tuple(frag for _, mangled, demangled in REQUIRED for frag in (mangled, demangled)) + \
          ("7mod_tag", "o3os::formats::mod::mod_tag(")


def symbol_of(line):
    """The symbol a map line defines, or None. A symbol line is '<address> <name...>' (one hex token); an input-section
    line is '<address> <size> <file>' (two hex tokens) and defines none."""
    parts = line.split()
    if len(parts) < 2 or not parts[0].startswith("0x"):
        return None
    if len(parts) >= 3 and parts[1].startswith("0x"):
        return None  # an input section: address, size, file
    return " ".join(parts[1:])


def check(map_text):
    """(problems, probes): the problems of the map (empty when it is clean), and per required probe symbol how many map
    lines define it (mangled or demangled, as the linker prints them)."""
    problems = []
    probes = {name: 0 for name, _, _ in REQUIRED}
    for lineno, line in enumerate(map_text.splitlines(), 1):
        if "formats_tracker" in line:
            problems.append("line %d: the map names formats_tracker (the importers' library is linked): %s" % (lineno, line.strip()))
        sym = symbol_of(line)
        if sym is not None and any(frag in sym for frag in NS_FRAGMENTS):
            if not any(ok in sym for ok in ALLOWED):
                problems.append("line %d: a formats::mod / formats::med symbol is linked: %s" % (lineno, sym))
            for name, mangled, demangled in REQUIRED:
                if mangled in sym or demangled in sym:
                    probes[name] += 1
    for name, _, _ in REQUIRED:
        if probes[name] == 0:
            problems.append("the probe symbol %s is not in the map at all (the image should link it from formats)" % name)
    return problems, probes


GOOD_MAP = """Memory Configuration

Linker script and memory map

.text           0x00008000     0x1000
 .text._ZN5o3os7formats3mod10sniff_kindEPKhm
                0x00008abc       0x2c build/formats/CMakeFiles/formats.dir/src/tracker_sniff.cpp.o
                0x00008abc                o3os::formats::mod::sniff_kind(unsigned char const*, unsigned long)
 .text._ZN5o3os7formats3mod7mod_tagEPKhRN5o3os7formats3mod7VariantERh
                0x00008ae0       0x50 build/formats/CMakeFiles/formats.dir/src/tracker_sniff.cpp.o
                0x00008ae0                o3os::formats::mod::mod_tag(unsigned char const*, o3os::formats::mod::Variant&, unsigned char&)
 .text._ZN5o3os7formats3mod15drop_group_nameEj
                0x00008b30       0x18 build/formats/CMakeFiles/formats.dir/src/tracker_sniff.cpp.o
                0x00008b30                o3os::formats::mod::drop_group_name(unsigned int)
 .text._ZN5o3os7formats3med15drop_group_nameEj
                0x00008b48       0x18 build/formats/CMakeFiles/formats.dir/src/tracker_sniff.cpp.o
                0x00008b48                o3os::formats::med::drop_group_name(unsigned int)
 .text._ZN5o3os7formats3mod20tagged_mod_min_bytesEPKhm
                0x00008b60       0x24 build/formats/CMakeFiles/formats.dir/src/tracker_sniff.cpp.o
                0x00008b60                o3os::formats::mod::tagged_mod_min_bytes(unsigned char const*, unsigned long)
 .text._ZN5o3os7formats3mod17is_soundtracker15EPKhmj
                0x00008b84       0x90 build/formats/CMakeFiles/formats.dir/src/tracker_sniff.cpp.o
                0x00008b84                o3os::formats::mod::is_soundtracker15(unsigned char const*, unsigned long, unsigned int)
 .text._ZN5o3os3app13ModuleImport6decideEPKcPKhmy
                0x00008c14      0x180 build/app/CMakeFiles/app.dir/src/module_import.cpp.o
                0x00008c14                o3os::app::ModuleImport::decide(char const*, unsigned char const*, unsigned long, unsigned int)

Discarded input sections

 .text.someother 0x00000000       0x10 build/formats/libo3os_formats.a(volume_level.cpp.o)
"""

BAD_MAP_ARCHIVE = GOOD_MAP + """ .text._ZN5o3os7formats3mod6Importer4stepEm
                0x00009000      0x400 build/formats/libo3os_formats_tracker.a(import.cpp.o)
                0x00009000                o3os::formats::mod::Importer::step(unsigned long)
"""

# A real reader symbol of the importers (Module::read_line, formats/mod/src/module.cpp), demangled as the linker prints
# it, from an object whose path does not say formats_tracker (the tracker module's own build): the symbol rule must
# catch what the path rule misses.
BAD_MAP_SYMBOL = GOOD_MAP + """ .text._ZN5o3os7formats3mod6Module9read_lineEjjPNS1_4CellE
                0x00009400       0x40 build/modules/CMakeFiles/tracker.dir/formats/mod/src/module.cpp.o
                0x00009400                o3os::formats::mod::Module::read_line(unsigned int, unsigned int, o3os::formats::mod::Cell*)
"""

BAD_MAP_MISSING = """ .text.foo
                0x00008000       0x10 build/app/CMakeFiles/app.dir/src/module_import.cpp.o
                0x00008000                o3os::app::f()
"""

# No symbol line at all: the probe side is gone, so the map must be refused (the check cannot pass vacuously).
BAD_MAP_EMPTY = ""

# Four of the five probe symbols only: is_soundtracker15 is gone, so the map must be refused (every one of the five must be there).
BAD_MAP_MISSING_ONE = GOOD_MAP.replace(""" .text._ZN5o3os7formats3mod17is_soundtracker15EPKhmj
                0x00008b84       0x90 build/formats/CMakeFiles/formats.dir/src/tracker_sniff.cpp.o
                0x00008b84                o3os::formats::mod::is_soundtracker15(unsigned char const*, unsigned long, unsigned int)
""", "")


def self_test():
    fails = []
    problems, probes = check(GOOD_MAP)
    if problems:
        fails.append("the good map was refused: %s" % problems)
    if any(n == 0 for n in probes.values()):
        fails.append("the good map does not define every probe symbol: %s" % probes)
    for name, text in (("an archive pull", BAD_MAP_ARCHIVE), ("a reader symbol", BAD_MAP_SYMBOL),
                       ("a map without the probe symbols", BAD_MAP_MISSING), ("an empty map", BAD_MAP_EMPTY),
                       ("a map missing one probe symbol", BAD_MAP_MISSING_ONE)):
        problems, _ = check(text)
        if not problems:
            fails.append("the map with %s was accepted" % name)
    if fails:
        for f in fails:
            print("FAILED: " + f)
        return 1
    print("self-test ok: the good map (demangled lines, only the probe side and the shared mod_tag) passes with all five probe "
          "symbols present; the importer archive, a reader symbol, a probe-less map, an empty map and a map missing one of the "
          "five are refused")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true", help="run the built-in fixtures")
    ap.add_argument("mode", nargs="?", choices=["check"], default="check")
    ap.add_argument("map", nargs="?")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.map:
        ap.error("check needs the map file")
    with open(args.map, "r", encoding="utf-8", errors="replace") as f:
        problems, probes = check(f.read())
    if problems:
        for p in problems:
            print("FAILED: " + p)
        return 1
    print("ok: no formats_tracker section and no formats::mod / formats::med symbol beyond the probe side in %s; "
          "probe symbols found: %s" % (args.map, ", ".join("%s x%d" % (name, n) for name, n in probes.items() if n)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
