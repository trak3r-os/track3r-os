#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest `modpreset_packs` (OS-MODPRESET P5): the factory preset packs of TB-303, FM, Chip and SID (modules/<id>/presets.json).

usage: modpreset_packs_test.py REPO_ROOT

Each pack holds four to six sounds; every parameter name resolves against the module's own module.json (ASCII case-insensitive, A-3), every value
lies inside that parameter's range (whole for int and bool); every file the pack builds reads back with the reference reader (tools/o3preset.py);
the starter's `presets` list names the four modules and tools/release/make_starter.py puts the files at modules/presets/<id>/<name>.o3mp.
"""
import importlib.util
import json
import os
import sys

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print("FAIL: " + msg)


def load(root, rel, name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(root, rel))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def main(root):
    o3 = load(root, "tools/o3preset.py", "o3preset")
    stems = ["tb303", "fm", "chip", "sid"]
    for stem in stems:
        with open(os.path.join(root, "modules", stem, "module.json")) as f:
            params = json.load(f)["params"]
        by_name = {}
        for p in params:
            by_name.setdefault(p["name"].lower(), p)
        path = os.path.join(root, "modules", stem, "presets.json")
        packed = o3.load_pack(path)
        check(4 <= len(packed) <= 6, "%s: %d sounds, wanted four to six" % (stem, len(packed)))
        for name, preset in packed:
            check(preset["module"] == stem and preset["kind"] == o3.KIND_INSTRUMENT, "%s/%s: module or kind" % (stem, name))
            check(len(preset["entries"]) >= 3, "%s/%s: fewer than three values" % (stem, name))
            seen = set()
            for pname, value in preset["entries"]:
                p = by_name.get(pname.lower())
                check(p is not None, "%s/%s: %r is not a parameter of module.json" % (stem, name, pname))
                check(pname.lower() not in seen, "%s/%s: %r twice" % (stem, name, pname))
                seen.add(pname.lower())
                if p is None:
                    continue
                check(p["min"] <= value <= p["max"], "%s/%s: %s=%r outside %r..%r" % (stem, name, pname, value, p["min"], p["max"]))
                if p.get("type") in ("int", "bool"):
                    check(float(value).is_integer(), "%s/%s: %s=%r is not whole" % (stem, name, pname, value))
            data = o3.build(preset)
            back = o3.parse(data)
            check(back["entries"] == preset["entries"] and back["module"] == stem, "%s/%s: does not read back" % (stem, name))
    ms = load(root, "tools/release/make_starter.py", "make_starter")
    with open(os.path.join(root, "tools", "release", "starter_manifest.json")) as f:
        manifest = json.load(f)
    check(manifest.get("presets") == stems, "starter_manifest.json: presets is %r" % (manifest.get("presets"),))
    ms.validate_manifest(manifest)
    files = ms.collect_presets(manifest, root)
    for stem in stems:
        n = sum(1 for k in files if k.startswith("modules/presets/%s/" % stem) and k.endswith(".o3mp"))
        check(4 <= n <= 6, "starter: %d files for %s" % (n, stem))
    check(all(o3.parse(v)["module"] in stems for v in files.values()), "starter: a file does not parse")
    bad = dict(manifest, presets=["tb303", "../x"])
    try:
        ms.validate_manifest(bad)
        check(False, "validate_manifest took a bad preset pack id")
    except ms.StarterError:
        pass
    print("modpreset_packs: %d checks failed" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1])))
