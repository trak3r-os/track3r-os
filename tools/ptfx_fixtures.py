#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ptfx_fixtures.py: build the OS-CEPTFX S0 fixtures into build-fixtures/ptfx/ (docs/specs/OS-CEPTFX.md 7, S0;
standard library only; run it with `python3 -I`).

  ptfx_fixtures.py [--out build-fixtures/ptfx] [--toolchain PREFIX] [--no-fetch]

  - the six SDK examples (build-fixtures/ptfx-sdk/effects/*.c, fetched by `tools/testdata/testdata.py fetch --only ptfx-sdk/`
    from tracker-ce@7fd6c2c, MIT, untracked — this script asks for them itself when missing) are built with the fetched official builder
    build-fixtures/ptfx-sdk/ptfx_builder.py, run with `python3 -I`, with our Arm GNU Toolchain (spec 2.1);
  - our own test plugins (tests/ptfx/*.c with their JSON sidecars) are built by our packer tools/ptfx_pack.py;
  - every built file is then read back through the 3.1 checks of tools/ptfx_inspect.py and must parse as Ok.

If the SDK files are missing, they are fetched first with `testdata.py fetch --only ptfx-sdk/` (hash-pinned; pass
--no-fetch to refuse that). Nothing built is committed: build-fixtures/ is untracked (`.gitignore` has `build*/`),
and no .ptfx or ELF is ever in git. The toolchain is found like tools/ptfx_pack.py (and
cmake/arm-none-eabi-toolchain.cmake): --toolchain PREFIX, else ARM_GNU_ROOT / O3_ARM_TOOLCHAIN, else PATH.
"""
import argparse
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DEFAULT_OUT = os.path.join(ROOT, "build-fixtures", "ptfx")
SDK_DIR = os.path.join(ROOT, "build-fixtures", "ptfx-sdk")
TESTS_DIR = os.path.join(ROOT, "tests", "ptfx")

# The six examples of tracker-ce@7fd6c2c:editor-fx-sdk/effects/, with the display names their sources carry.
SDK_EXAMPLES = ["add_silence", "analog_kick_gen", "chiptune_gen", "multimode_filter", "stereo_delay", "stereo_to_mono"]
SDK_NAMES = {
    "add_silence": "Add Silence",
    "analog_kick_gen": "Analog Kick Gen",
    "chiptune_gen": "Chiptune Voice Gen",
    "multimode_filter": "Multi-Mode Filter",
    "stereo_delay": "Stereo Delay",
    "stereo_to_mono": "Stereo to Mono",
}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class FixturesError(Exception):
    pass


def sdk_present():
    builder = os.path.join(SDK_DIR, "ptfx_builder.py")
    header = os.path.join(SDK_DIR, "tracker_plugin.h")
    effects = os.path.join(SDK_DIR, "effects")
    if not (os.path.isfile(builder) and os.path.isfile(header) and os.path.isdir(effects)):
        return False
    return sorted(f[:-2] for f in os.listdir(effects) if f.endswith(".c")) == sorted(SDK_EXAMPLES)


def fetch_sdk():
    print("the ptfx-sdk fixtures are missing: tools/testdata/testdata.py fetch --only ptfx-sdk/")
    res = subprocess.run([sys.executable, "-I", os.path.join(HERE, "testdata", "testdata.py"),
                          "fetch", "--only", "ptfx-sdk/"])
    if res.returncode != 0 or not sdk_present():
        raise FixturesError("the fetch of build-fixtures/ptfx-sdk/ failed (no network, or a hash differs)")


def run(cmd, what):
    print("+ %s" % " ".join(cmd))
    res = subprocess.run(cmd)
    if res.returncode != 0:
        raise FixturesError("%s failed (exit %d)" % (what, res.returncode))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=DEFAULT_OUT, help="where the .ptfx files go (default: build-fixtures/ptfx)")
    ap.add_argument("--toolchain", help="arm-none-eabi- prefix (else ARM_GNU_ROOT / O3_ARM_TOOLCHAIN / PATH)")
    ap.add_argument("--no-fetch", action="store_true", help="refuse to fetch a missing ptfx-sdk/")
    a = ap.parse_args(argv)

    pack = load_module("ptfx_pack", os.path.join(HERE, "ptfx_pack.py"))
    insp = load_module("ptfx_inspect", os.path.join(HERE, "ptfx_inspect.py"))
    try:
        prefix = pack.find_toolchain(a.toolchain)
    except pack.PackError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    if not sdk_present():
        if a.no_fetch:
            print("error: %s is missing (tools/testdata/testdata.py fetch --only ptfx-sdk/)" % SDK_DIR, file=sys.stderr)
            return 1
        try:
            fetch_sdk()
        except FixturesError as e:
            print("error: %s" % e, file=sys.stderr)
            return 1
    os.makedirs(a.out, exist_ok=True)

    # The six SDK examples, with the fetched official builder and our toolchain.
    builder = os.path.join(SDK_DIR, "ptfx_builder.py")
    for stem in SDK_EXAMPLES:
        src = os.path.join(SDK_DIR, "effects", stem + ".c")
        out = os.path.join(a.out, stem + ".ptfx")
        try:
            run([sys.executable, "-I", builder, src, "-o", out, "--toolchain", prefix],
                "the builder on %s" % stem)
        except FixturesError as e:
            print("error: %s" % e, file=sys.stderr)
            return 1

    # Our own test plugins, with our packer.
    ours = sorted(f[:-2] for f in os.listdir(TESTS_DIR) if f.endswith(".c"))
    for stem in ours:
        src = os.path.join(TESTS_DIR, stem + ".c")
        side = os.path.join(TESTS_DIR, stem + ".json")
        if not os.path.isfile(side):
            print("error: %s has no sidecar %s" % (src, side), file=sys.stderr)
            return 1
        out = os.path.join(a.out, stem + ".ptfx")
        try:
            run([sys.executable, "-I", os.path.join(HERE, "ptfx_pack.py"), src, side, "-o", out,
                 "--toolchain", prefix, "--abi-dir", TESTS_DIR], "the packer on %s" % stem)
        except FixturesError as e:
            print("error: %s" % e, file=sys.stderr)
            return 1

    # Read every built file back through the 3.1 checks.
    built = sorted(f for f in os.listdir(a.out) if f.lower().endswith(".ptfx"))
    expected = sorted(SDK_EXAMPLES + ours)
    if built != [e + ".ptfx" for e in expected]:
        print("error: %s holds %s, expected %s" % (a.out, built, [e + ".ptfx" for e in expected]), file=sys.stderr)
        return 1
    failed = 0
    for name in built:
        path = os.path.join(a.out, name)
        with open(path, "rb") as f:
            info = insp.parse_header(f.read())
        stem = name[:-5]
        if info["status"] != "Ok":
            print("REFUSED %s: %s" % (path, info["message"]))
            failed += 1
            continue
        want = SDK_NAMES.get(stem)
        if want is not None and info["name"] != want:
            print("REFUSED %s: the display name is %r, expected %r" % (path, info["name"], want))
            failed += 1
            continue
        print("OK %s: %s, flags 0x%04X, %d param(s), %d bytes" % (path, info["name"], info["flags"],
                                                                 info["num_params"], info["file_size"]))
    if failed:
        print("error: %d of %d built files are refused" % (failed, len(built)), file=sys.stderr)
        return 1
    print("built and inspected %d .ptfx files in %s" % (len(built), a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
