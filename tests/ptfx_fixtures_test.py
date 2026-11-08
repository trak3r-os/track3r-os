#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest `ptfx_fixtures` (OS-CEPTFX S0): the .ptfx fixtures are built and inspected.

usage: ptfx_fixtures_test.py REPO_ROOT [O3OS_TESTS_BINARY]

tools/ptfx_fixtures.py builds the six SDK examples of tracker-ce@7fd6c2c (fetched hash-pinned into
build-fixtures/ptfx-sdk/ by tools/testdata/testdata.py fetch --only ptfx-sdk/, which this test runs when they are
missing) with the fetched official builder, and our tests/ptfx/*.c with tools/ptfx_pack.py, all with the Arm GNU
Toolchain, into build-fixtures/ptfx/. This test then:

  - reads every built file back through the 3.1 checks of tools/ptfx_inspect.py: all 17 must parse as Ok, the six
    SDK examples must carry the display names of spec 2.1, and each file must be exactly 472 + code_size bytes, so
    a payload with orphan sections (.got, .init_array, ...) fails;
  - with the host test binary given, runs its "dump the built fixtures" case (tests/ptfx_format_test.cpp, the C++
    parser of module/src/ptfx_format.cpp) over the same files and compares the two dumps field by field, floats
    as raw bits, so the Python inspector and the C++ parser cannot drift apart.

Skipped (exit 77) without the cross compiler or without the fetched SDK (no network); under O3_CI=1 that absence
is a failure, so a CI run cannot go quiet (tools/ci.sh exports O3_CI=1).
"""
import importlib.util
import os
import struct
import subprocess
import sys

ROOT = sys.argv[1]
TESTS_BIN = sys.argv[2] if len(sys.argv) > 2 else None
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(ROOT, "tools")
OUT = os.path.join(ROOT, "build-fixtures", "ptfx")
SDK = os.path.join(ROOT, "build-fixtures", "ptfx-sdk")
HEADER_SIZE = 472

# PtfxStatus of module/include/o3os/module/ptfx_format.h, by value.
STATUS_INT = {"Ok": 0, "NotPtfx": 1, "VersionUnsupported": 2, "NotForThisTracker": 3, "NeedsNewerOs": 4,
              "FileDamaged": 5, "TooBig": 6}

SDK_STEMS = ["add_silence", "analog_kick_gen", "chiptune_gen", "multimode_filter", "stereo_delay", "stereo_to_mono"]
SDK_NAMES = {"add_silence": "Add Silence", "analog_kick_gen": "Analog Kick Gen", "chiptune_gen": "Chiptune Voice Gen",
             "multimode_filter": "Multi-Mode Filter", "stereo_delay": "Stereo Delay", "stereo_to_mono": "Stereo to Mono"}
OUR_STEMS = ["clobber", "cpsid", "extra", "fault_addr", "fault_scs", "gain", "gen_ramp", "hang", "overcount",
             "recorder", "stackhog"]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ci():
    return os.environ.get("O3_CI", "") == "1"


def skip_or_fail(reason):
    if ci():
        print("ptfx_fixtures: FAILED: %s, and O3_CI=1" % reason)
        return 1
    print("ptfx_fixtures: skipped: %s" % reason)
    return 77


def bits(x):
    return "%08x" % struct.unpack("<I", struct.pack("<f", x))[0]


def expected_dump(name, info):
    """The same tab-separated key=value lines tests/ptfx_format_test.cpp's dump case prints for one file."""
    e = info.get("entries") or {}
    lines = ["PTFXDUMP\tfile=%s\tstatus=%d\tversion=%d\tflags=%d\tname=%s\tauthor=%s\tnum_params=%d\tscratch=%d\t"
             "code_offset=%d\tcode_size=%d\tbss=%d\tfilesize=%d\tinit=%d\tset_param=%d\tprocess_block=%d"
             % (name, STATUS_INT[info["status"]], info["version"], info["flags"], info["name"], info["author"],
                info["num_params"], info["scratch_size"], info["code_offset"], info["code_size"], info["bss_size"],
                info["file_size"], e.get("init", 0), e.get("set_param", 0), e.get("process_block", 0))]
    if info["status"] == "Ok":
        for i, p in enumerate(info["params"]):
            lines.append("PTFXDUMP\tparam=%d\tlabel=%s\ttype=%s\tmin=%s\tmax=%s\tdef=%s\tstep=%s\tunit=%s\teditable=%d\t"
                         "list_offset=%d\tlist_count=%d\titems=%s"
                         % (i, p["label"], p["type"], bits(p["min"]), bits(p["max"]), bits(p["def"]), bits(p["step"]),
                            "" if p["type"] == "l" else p["unit"], 1 if p["editable"] else 0, p["list_offset"],
                            p["list_count"], "|".join(p["items"])))
    lines.append("PTFXDUMP\tend")
    return lines


def main():
    pack = load("ptfx_pack", os.path.join(TOOLS, "ptfx_pack.py"))
    insp = load("ptfx_inspect", os.path.join(TOOLS, "ptfx_inspect.py"))

    # The cross compiler (found like cmake/arm-none-eabi-toolchain.cmake and tools/ptfx_pack.py).
    try:
        pack.find_toolchain(None)
    except pack.PackError as e:
        return skip_or_fail(str(e))

    # Build: tools/ptfx_fixtures.py fetches the SDK itself when missing (hash-pinned, no release asset).
    res = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "ptfx_fixtures.py")])
    if res.returncode != 0:
        if not os.path.isfile(os.path.join(SDK, "ptfx_builder.py")):
            return skip_or_fail("the ptfx-sdk fixtures were not fetched (testdata.py fetch --only ptfx-sdk/) and the build failed")
        print("ptfx_fixtures: FAILED: tools/ptfx_fixtures.py exited %d" % res.returncode)
        return 1

    # Inspect what was built: every file Ok, the expected set, the SDK display names of spec 2.1, and the file
    # exactly 472 + code_size bytes (an orphan section in the payload would break that).
    built = sorted(f for f in os.listdir(OUT) if f.lower().endswith(".ptfx")) if os.path.isdir(OUT) else []
    expected = sorted(s + ".ptfx" for s in SDK_STEMS + OUR_STEMS)
    if built != expected:
        print("ptfx_fixtures: FAILED: %s holds %s, expected %s" % (OUT, built, expected))
        return 1
    infos = {}
    failed = 0
    for name in built:
        with open(os.path.join(OUT, name), "rb") as f:
            blob = f.read()
        info = insp.parse_header(blob)
        infos[name] = info
        stem = name[:-5]
        if info["status"] != "Ok":
            print("ptfx_fixtures: FAILED: %s: %s" % (name, info["message"]))
            failed += 1
        elif stem in SDK_NAMES and info["name"] != SDK_NAMES[stem]:
            print("ptfx_fixtures: FAILED: %s: the display name is %r, expected %r" % (name, info["name"], SDK_NAMES[stem]))
            failed += 1
        elif info["file_size"] != HEADER_SIZE + info["code_size"]:
            print("ptfx_fixtures: FAILED: %s: %d bytes on disk but the header says 472 + %d = %d (an orphan section in the payload?)"
                  % (name, info["file_size"], info["code_size"], HEADER_SIZE + info["code_size"]))
            failed += 1
        else:
            print("ptfx_fixtures: OK %s: %s, flags 0x%04X, %d param(s)" % (name, info["name"], info["flags"], info["num_params"]))
    if failed:
        print("ptfx_fixtures: FAILED: %d of %d built files are refused or malformed" % (failed, len(built)))
        return 1

    # The C++ parser (the host test binary) over the same files, compared field by field with the Python one.
    if TESTS_BIN:
        res = subprocess.run([TESTS_BIN, "--test-case=*dump the built fixtures*"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        cpp_lines = [l.rstrip("\n") for l in res.stdout.splitlines() if l.startswith("PTFXDUMP\t")]
        py_lines = []
        for name in built:
            py_lines += expected_dump(name, infos[name])
        if not cpp_lines:
            print("ptfx_fixtures: FAILED: %s printed no fixture dump (exit %d): %s"
                  % (TESTS_BIN, res.returncode, res.stdout.strip()[-300:]))
            return 1
        if cpp_lines != py_lines:
            for a, b in zip(cpp_lines, py_lines):
                if a != b:
                    print("ptfx_fixtures: FAILED: the C++ parser and ptfx_inspect.py disagree:\n  cpp: %s\n  py : %s" % (a, b))
                    break
            if len(cpp_lines) != len(py_lines):
                print("ptfx_fixtures: FAILED: %d dump lines from the C++ parser, %d expected" % (len(cpp_lines), len(py_lines)))
            return 1
        print("ptfx_fixtures: the C++ parser (ptfx_format.cpp) and ptfx_inspect.py agree on all %d files" % len(built))

    print("ptfx_fixtures: ok: %d .ptfx files built in build-fixtures/ptfx and inspected" % len(built))
    return 0


sys.exit(main())
