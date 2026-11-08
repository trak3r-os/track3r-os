#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The SDK's scaffolding and its round trip (OS-012b): `o3m_gen.py new` makes a module of each kind from the template, the generated header is what `gen` writes,
a manifest that is edited and regenerated still builds, and the packed file reads back as the manifest says (the generator's own reader) -- with the cross compiler when
there is one (the environment's O3M_CXX, which ctest sets from the build's compiler search, or an arm-none-eabi-g++ on PATH; O3_CI=1 makes its absence a failure).

    o3m_gen_scaffold_test.py <o3m_gen.py> <templates dir>
"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def load_gen(path):
    spec = importlib.util.spec_from_file_location("o3m_gen", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def find_cxx(gen_path):
    env = os.environ.get("O3M_CXX")
    if env:
        return env
    return shutil.which("arm-none-eabi-g++")


def main(argv):
    gen_path, templates = argv[1], Path(argv[2])
    gen = load_gen(gen_path)
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    cxx = find_cxx(gen_path)
    pack = Path(gen_path).parent / "o3m_pack.py"
    if cxx is None:
        if os.environ.get("O3_CI") == "1":
            failures.append("no arm-none-eabi-g++ and O3_CI=1")
        print("o3m_gen scaffold: no cross compiler: the build half of the round trip is skipped")
    with tempfile.TemporaryDirectory(prefix="o3m_gen_test_") as tmp:
        tmp = Path(tmp)
        for kind in ("instrument", "effect", "effect_stereo", "effect_key", "app"):  # effect_stereo (OS-INSFX-ABI2 A2) and effect_key (A6): their files are example_stereo.* and example_key.*, the module's own id is what is replaced
            out = tmp / kind
            made = gen.scaffold(kind, "my_" + kind, "My %s" % kind.title(), out)
            check(made == sorted(["module.json", "my_%s.cpp" % kind, "my_%s_gen.h" % kind]), "%s: files %r" % (kind, made))
            for f in out.iterdir():
                text = f.read_text()
                check("example_" not in text.lower(), "%s: %s still says example_" % (kind, f.name))
            manifest = gen.load_manifest(out / "module.json")
            check(manifest["name"] == "My %s" % kind.title() and manifest["id"] == "my_" + kind, "%s: name and id" % kind)
            check((out / ("my_%s_gen.h" % kind)).read_text() == gen.generate(manifest), "%s: the scaffolded header is what gen writes" % kind)
            check("MY_%s_PARAM_COUNT" % kind.upper() in (out / ("my_%s_gen.h" % kind)).read_text(), "%s: the prefix is the id" % kind)
            # nothing is overwritten
            try:
                gen.scaffold(kind, "my_" + kind, None, out)
                failures.append("%s: scaffold overwrote a module" % kind)
            except gen.ManifestError:
                pass
        for bad in ("My", "1abc", "a" * 40, "a-b", ""):
            try:
                gen.scaffold("app", bad, None, tmp / ("bad_" + str(len(bad))))
                failures.append("scaffold accepted the id %r" % bad)
            except gen.ManifestError:
                pass
        try:
            gen.scaffold("importer", "x", None, tmp / "imp")
            failures.append("scaffold made an importer")
        except (gen.ManifestError, SystemExit, KeyError):
            pass
        # the command line
        cli = subprocess.run([sys.executable, "-I", gen_path, "new", "--kind", "effect", "--id", "cli_fx", "--out", str(tmp / "cli")], capture_output=True, text=True)
        check(cli.returncode == 0 and "module.json" in cli.stdout, "the new command: %r" % cli.stderr)
        stale = subprocess.run([sys.executable, "-I", gen_path, "check", str(tmp / "cli" / "module.json"), str(tmp / "cli" / "cli_fx_gen.h")], capture_output=True, text=True)
        check(stale.returncode == 0, "check of a fresh header")
        (tmp / "cli" / "cli_fx_gen.h").write_text((tmp / "cli" / "cli_fx_gen.h").read_text() + "/* edited */\n")
        stale = subprocess.run([sys.executable, "-I", gen_path, "check", str(tmp / "cli" / "module.json"), str(tmp / "cli" / "cli_fx_gen.h")], capture_output=True, text=True)
        check(stale.returncode == 1 and "run `o3m_gen.py gen" in stale.stderr, "check finds an edited header")
        broken = tmp / "broken.json"
        broken.write_text('{"name": "X", "id": "x", "kind": "instrument", "state_bytes": 8, "params": [{"name": "A", "min": 2, "max": 1, "default": 1}]}')
        bad = subprocess.run([sys.executable, "-I", gen_path, "gen", str(broken)], capture_output=True, text=True)
        check(bad.returncode == 1 and "min < max" in bad.stderr, "gen names what is wrong: %r" % bad.stderr)

        # OS-INSFX-ABI2 A2: a manifest preset that names an unknown parameter, or a value out of range, fails generation (the command line says why)
        stereo_spec = json.loads((tmp / "effect_stereo" / "module.json").read_text())
        check(stereo_spec.get("stereo") == "widens" and len(stereo_spec.get("presets", [])) == 2, "the stereo template is a WIDENS effect with two presets")
        check(gen.load_manifest(tmp / "effect_stereo" / "module.json")["abi_minor"] == 12, "the stereo template needs ABI 1.12")
        for what, mutate, fragment in (("an unknown parameter", lambda s: s["presets"][0]["values"].update({"Nope": 1}), "names no parameter"),
                                       ("a value out of range", lambda s: s["presets"][0]["values"].update({"Depth": 101}), "outside its range")):
            bad_spec = json.loads(json.dumps(stereo_spec))
            mutate(bad_spec)
            (tmp / "bad_preset.json").write_text(json.dumps(bad_spec))
            bad = subprocess.run([sys.executable, "-I", gen_path, "gen", str(tmp / "bad_preset.json")], capture_output=True, text=True)
            check(bad.returncode == 1 and fragment in bad.stderr, "%s in a preset: %r" % (what, bad.stderr))
        cli_new = subprocess.run([sys.executable, "-I", gen_path, "new", "--kind", "effect_stereo", "--id", "cli_pan", "--out", str(tmp / "cli_pan")], capture_output=True, text=True)
        check(cli_new.returncode == 0 and "cli_pan.cpp" in cli_new.stdout, "the new command takes effect_stereo: %r" % cli_new.stderr)

        # OS-INSFX-ABI2 A6: the sidechain template is a stereo-capable effect with one KEY parameter (INT 0..16, default 0), ABI 1.12
        key_spec = json.loads((tmp / "effect_key" / "module.json").read_text())
        key_params = [p for p in key_spec["params"] if p.get("type") == "key"]
        check(key_spec.get("stereo") == "capable" and len(key_params) == 1, "the key template is a stereo-capable effect with one key parameter")
        key_manifest = gen.load_manifest(tmp / "effect_key" / "module.json")
        check(key_manifest["abi_minor"] == 12, "the key template needs ABI 1.12")
        check(any(gen.param_flags(p) & gen.F_KEY for p in key_manifest["params"]), "the key template's parameter carries O3M_PARAM_KEY")

        if cxx is not None:
            for kind in ("instrument", "effect", "effect_stereo", "effect_key", "app"):
                out = tmp / kind
                manifest_path = out / "module.json"
                spec = json.loads(manifest_path.read_text())
                if kind != "app":  # edit the manifest as an author would: one more parameter, a change of range, a second view
                    spec["params"].append({"name": "Extra Gain", "min": -12, "max": 12, "default": 0, "unit": "db", "learnable": False, "mod": True})
                    spec["params"][0]["default"] = spec["params"][0]["min"]
                    if kind not in ("effect_stereo", "effect_key"):  # (the stereo and key templates have no views, and no view_draw for a second one)
                        spec["views"].append({"name": "Second", "min_size": [100, 60]})
                else:
                    spec["params"].append({"name": "Speed", "min": 1, "max": 5, "default": 3, "type": "int", "unit": "none"})
                manifest_path.write_text(json.dumps(spec, indent=2))
                m = gen.load_manifest(manifest_path)
                (out / ("my_%s_gen.h" % kind)).write_text(gen.generate(m))
                tmod = out / ("my_%s.tmod" % kind)
                src = out / ("my_%s.cpp" % kind)
                # the second view of an instrument or effect: the template's view_draw ignores an index it does not know, so it builds as it is
                run = subprocess.run([sys.executable, "-I", str(pack), "build", "--cxx", cxx, "--src", str(src), "--out", str(tmod), "-I", str(out)], capture_output=True, text=True)
                check(run.returncode == 0, "%s: the build failed: %s" % (kind, run.stderr[-600:]))
                if run.returncode != 0:
                    continue
                diffs = gen.verify(m, tmod.read_bytes())
                check(not diffs, "%s: the built file differs from the manifest: %r" % (kind, diffs))
                got = gen.read_tmod(tmod.read_bytes())
                check(len(got["params"]) == len(m["params"]) and got["views"] == len(m["views"]), "%s: counts" % kind)
                check(got["abi"] == [1, m["abi_minor"]], "%s: abi %r" % (kind, got["abi"]))
                if kind == "effect_stereo":  # ABI 1.12: render_st and WIDENS in the file, and both presets with the values the manifest gives
                    check(got["render_st"] and got["fx_flags"] == 1, "effect_stereo: render_st and WIDENS")
                    check([pr["name"] for pr in got["presets"]] == ["Quarter Pulse", "Slow Sweep"], "effect_stereo: the presets %r" % got["presets"])
                if kind == "effect_key":  # ABI 1.12: render_st (stereo-capable: fx_flags 0) and the KEY flag on exactly one parameter
                    check(got["render_st"] and got["fx_flags"] == 0, "effect_key: render_st, not WIDENS")
                    check(sum(1 for pr in got["params"] if pr["flags"] & gen.F_KEY) == 1, "effect_key: one KEY parameter in the file")
                if kind == "app":
                    check(got["kind"] == 4 and not got["entries"]["render"], "app: kind and no render")
                    check(got["hot_size"] == 0, "app: no hot code")
                else:
                    check(got["hot_size"] > 0, "%s: hot code" % kind)
                # a manifest that is out of step with the file is found
                for preset in spec.get("presets", []):  # (the presets name parameters: they follow the rename)
                    if spec["params"][0]["name"] in preset["values"]:
                        preset["values"]["Renamed"] = preset["values"].pop(spec["params"][0]["name"])
                spec["params"][0]["name"] = "Renamed"
                other = gen.validate(spec)
                check(gen.verify(other, tmod.read_bytes()) != [], "%s: verify missed a renamed parameter" % kind)
    for f in failures:
        print("FAIL", f)
    print("o3m_gen scaffold: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
