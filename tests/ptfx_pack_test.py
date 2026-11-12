#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest `ptfx_pack` (OS-PTFXPACK): the .ptfx plugins of modules/ptfx/ are intact, built and run.

usage: ptfx_pack_test.py REPO_ROOT BUILD_DIR [TOOLCHAIN_PREFIX]      (BUILD_DIR holds the <stem>.ptfx files of the o3_ptfx target)

  1. Integrity: the six files taken from tracker-ce@7fd6c2c (editor-fx-sdk/effects/*.c and tracker_plugin.h) are, past
     the notice block at their top (up to the line "---- end of the notice"), byte for byte the pinned upstream
     files: sha256 against tools/testdata/testdata_manifest.json (no network needed), so a "taken as is" file cannot
     drift. Every plugin source carries an SPDX line and, for the vendored ones, the MIT text.
  2. Built files: every modules/ptfx/<stem>/<stem>.c has a <stem>.ptfx that parses as Ok through the 3.1 checks of
     tools/ptfx_inspect.py, with the display name of its PTFX_NAME tag (for the six taken ones: the display name of
     spec 2.1), is exactly 472 + code_size bytes and holds the flags of its PTFX_FLAGS tag.
  3. Host smoke: each plugin source is compiled natively against the SDK header and called through ctypes with a
     stand-in host table (libm, libc, a recording set_extra_samples): the defaults of the header go in through
     plugin_set_param, four 512-sample blocks of a sine go through plugin_process_block, and the call must return a
     non-negative count, write sound (generators and effects), be deterministic and, per plugin, do what it says
     (Add Silence copies and asks for 2.5 s in front; Stereo Delay and Delay ask for a tail; Stereo to Mono is a mono sum;
     Bitcrusher holds samples on a 256 grid; Limiter lowers the level; Three-Band EQ is flat at its defaults and gives about x4 and
     x0.2 on its own frequency at +12 and -12 dB).
  4. Reproducible: with the toolchain prefix as third argument, every plugin is built twice by tools/ptfx_pack.py into two other
     directories; both must equal each other and the file of BUILD_DIR byte for byte.
     This is a check of the C, not of the Cortex-M4 machine code: that one runs on QEMU/the unit (OS-CEPTFX S6).

Skipped (exit 77) when no .ptfx was built (no Arm toolchain) or no native C compiler exists; under O3_CI=1 that
absence is a failure, so a CI run cannot go quiet (tools/ci.sh exports O3_CI=1), like ptfx_fixtures.
"""
import ctypes
import hashlib
import importlib.util
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = sys.argv[1]
BUILD = sys.argv[2]
SRC = os.path.join(ROOT, "modules", "ptfx")
HEADER_SIZE = 472
MARKER = "// ---- end of the notice"
FLAGS = {"EFFECT": 0x0000, "GENERATOR": 0x0001, "STEREO": 0x0002, "WAVETABLE": 0x0004, "CONVERT_TO_MONO": 0x0008,
         "IMX_ONLY": 0x0010}

# The six taken as is: display names of spec 2.1 and the testdata manifest dest of each pinned upstream file.
TAKEN = {"add_silence": "Add Silence", "analog_kick_gen": "Analog Kick Gen", "chiptune_gen": "Chiptune Voice Gen",
         "multimode_filter": "Multi-Mode Filter", "stereo_delay": "Stereo Delay", "stereo_to_mono": "Stereo to Mono"}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ci():
    return os.environ.get("O3_CI", "") == "1"


def skip_or_fail(reason):
    if ci():
        print("ptfx_pack: FAILED: %s, and O3_CI=1" % reason)
        return 1
    print("ptfx_pack: skipped: %s" % reason)
    return 77


def stems():
    return sorted(d for d in os.listdir(SRC) if os.path.isfile(os.path.join(SRC, d, d + ".c")))


def tag(text, name):
    m = re.search(r"^// PTFX_%s:[ \t]*(.*?)[ \t]*$" % name, text, re.M)
    return m.group(1) if m else None


def past_notice(data):
    """The bytes after the marker line of a vendored file (the original, byte for byte)."""
    at = data.find(MARKER.encode())
    if at < 0:
        return None
    eol = data.find(b"\n", at)
    return data[eol + 1:]


# ---- 1. integrity ----

def check_integrity():
    failed = 0
    with open(os.path.join(ROOT, "tools", "testdata", "testdata_manifest.json")) as f:
        man = {e["dest"]: e["sha256"] for e in json.load(f)["files"]}
    pinned = {os.path.join(SRC, "sdk", "tracker_plugin.h"): man["ptfx-sdk/tracker_plugin.h"]}
    for stem in TAKEN:
        pinned[os.path.join(SRC, stem, stem + ".c")] = man["ptfx-sdk/effects/%s.c" % stem]
    for path, want in sorted(pinned.items()):
        with open(path, "rb") as f:
            data = f.read()
        rest = past_notice(data)
        rel = os.path.relpath(path, ROOT)
        if rest is None:
            print("ptfx_pack: FAILED: %s has no notice marker line" % rel)
            failed += 1
        elif hashlib.sha256(rest).hexdigest() != want:
            print("ptfx_pack: FAILED: %s past its notice is not the pinned tracker-ce@7fd6c2c file (sha256 differs)" % rel)
            failed += 1
        head = data[:data.find(MARKER.encode())].decode("utf-8", "replace") if rest is not None else ""
        for need in ("SPDX-License-Identifier: MIT", "Copyright (c) 2020-2026 Polyend", "7fd6c2c3002a8f690b5341c486ba69606077b513",
                     "THE SOFTWARE IS PROVIDED \"AS IS\""):
            if need not in head:
                print("ptfx_pack: FAILED: the notice of %s lacks %r" % (rel, need))
                failed += 1
    if not os.path.isfile(os.path.join(SRC, "LICENSE-tracker-ce.txt")):
        print("ptfx_pack: FAILED: modules/ptfx/LICENSE-tracker-ce.txt is missing")
        failed += 1
    for stem in stems():
        with open(os.path.join(SRC, stem, stem + ".c"), "rb") as f:
            first = f.readline().decode()
        if "SPDX-License-Identifier:" not in first:
            print("ptfx_pack: FAILED: %s.c does not start with an SPDX line" % stem)
            failed += 1
    return failed


# ---- 2. built files ----

def check_built(insp):
    failed = 0
    for stem in stems():
        path = os.path.join(BUILD, stem + ".ptfx")
        if not os.path.isfile(path):
            print("ptfx_pack: FAILED: %s was not built" % path)
            failed += 1
            continue
        with open(path, "rb") as f:
            blob = f.read()
        with open(os.path.join(SRC, stem, stem + ".c")) as f:
            text = f.read()
        info = insp.parse_header(blob)
        want_name = TAKEN.get(stem, tag(text, "NAME"))
        want_flags = 0
        for w in re.split(r"[ ,|]+", tag(text, "FLAGS") or ""):
            if w:
                want_flags |= FLAGS[w]
        if info["status"] != "Ok":
            print("ptfx_pack: FAILED: %s.ptfx: %s" % (stem, info["message"]))
            failed += 1
        elif info["name"] != want_name:
            print("ptfx_pack: FAILED: %s.ptfx: display name %r, expected %r" % (stem, info["name"], want_name))
            failed += 1
        elif info["flags"] != want_flags:
            print("ptfx_pack: FAILED: %s.ptfx: flags 0x%04X, expected 0x%04X" % (stem, info["flags"], want_flags))
            failed += 1
        elif info["file_size"] != HEADER_SIZE + info["code_size"]:
            print("ptfx_pack: FAILED: %s.ptfx: %d bytes, the header says 472 + %d" % (stem, info["file_size"], info["code_size"]))
            failed += 1
        else:
            print("ptfx_pack: OK %s.ptfx: %s, flags 0x%04X, %d param(s), %d bytes" % (stem, info["name"], info["flags"],
                                                                                  info["num_params"], info["file_size"]))
    return failed


# ---- 3. host smoke ----

class Host(ctypes.Structure):
    """PtfxHostApi of modules/ptfx/sdk/tracker_plugin.h, field for field."""
    _fields_ = [("sample_rate", ctypes.c_uint32)] + [(n, ctypes.c_void_p) for n in (
        "sinf", "cosf", "tanf", "tanhf", "powf", "expf", "sqrtf", "logf", "log10f", "fminf", "fmaxf", "fabsf",
        "memcpy", "memset", "rand", "set_extra_samples")]


EXTRA_T = ctypes.CFUNCTYPE(None, ctypes.c_uint32, ctypes.c_uint32)
BLOCK = 512
BLOCKS = 4


def sine(freq, amp, n):
    return [int(round(amp * math.sin(2 * math.pi * freq * i / 44100.0))) for i in range(n)]


def run_plugin(so, info, scratch_size, defaults, block=BLOCK):
    """The sound the plugin makes for the defaults: (outputs per channel, returns, extra-samples calls)."""
    lib = ctypes.CDLL(so)
    libm = ctypes.CDLL("libm.so.6")
    libc = ctypes.CDLL("libc.so.6")
    libc.srand(1)  # the host rand() is the C library's: the same seed for every run
    calls = []
    cb = EXTRA_T(lambda pre, app: calls.append((pre, app)))
    host = Host()
    host.sample_rate = 44100
    for n in ("sinf", "cosf", "tanf", "tanhf", "powf", "expf", "sqrtf", "logf", "log10f", "fminf", "fmaxf", "fabsf"):
        setattr(host, n, ctypes.cast(getattr(libm, n), ctypes.c_void_p).value)
    for n in ("memcpy", "memset", "rand"):
        setattr(host, n, ctypes.cast(getattr(libc, n), ctypes.c_void_p).value)
    host.set_extra_samples = ctypes.cast(cb, ctypes.c_void_p).value
    scratch = ctypes.create_string_buffer(max(scratch_size, 16))
    ctypes.memset(scratch, 0, len(scratch))
    lib.plugin_init.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    lib.plugin_set_param.argtypes = [ctypes.c_uint8, ctypes.c_float]
    lib.plugin_process_block.restype = ctypes.c_int32
    lib.plugin_process_block.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint8]
    lib.plugin_init(ctypes.cast(scratch, ctypes.c_void_p), scratch_size, ctypes.byref(host))
    for i, d in enumerate(defaults):
        lib.plugin_set_param(i, d)
    nch = 2 if info["flags"] & 0x2 else 1
    n = block * BLOCKS
    ins = [sine(440.0, 8000, n), sine(660.0, 6000, n)][:nch]
    outs = [[] for _ in range(nch)]
    rets = []
    for b in range(BLOCKS):
        in_bufs = [(ctypes.c_int16 * block)(*ins[c][b * block:(b + 1) * block]) for c in range(nch)]
        out_bufs = [(ctypes.c_int16 * block)() for _ in range(nch)]
        in_arr = (ctypes.c_void_p * nch)(*[ctypes.addressof(x) for x in in_bufs])
        out_arr = (ctypes.c_void_p * nch)(*[ctypes.addressof(x) for x in out_bufs])
        rets.append(lib.plugin_process_block(ctypes.addressof(in_arr), ctypes.addressof(out_arr), block, nch))
        for c in range(nch):
            outs[c].extend(out_bufs[c][:])
    return ins, outs, rets, calls


def check_smoke(insp):
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not cc or not sys.platform.startswith("linux"):
        return None
    failed = 0
    sdk = os.path.join(SRC, "sdk")
    with tempfile.TemporaryDirectory() as tmp:
        for stem in stems():
            src = os.path.join(SRC, stem, stem + ".c")
            so = os.path.join(tmp, stem + ".so")
            res = subprocess.run([cc, "-shared", "-fPIC", "-O1", "-w", "-I", sdk, "-o", so, src, "-lm"],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
            if res.returncode != 0:
                print("ptfx_pack: FAILED: %s did not compile natively:\n%s" % (stem, res.stdout))
                failed += 1
                continue
            with open(os.path.join(BUILD, stem + ".ptfx"), "rb") as f:
                info = insp.parse_header(f.read())
            with open(src) as f:
                scratch = int(tag(f.read(), "SCRATCH") or "0")
            defaults = [p["def"] for p in info["params"]]
            flat = list(defaults)
            if stem == "eq3":  # flat at its defaults: the run is made with the mid band at +12 dB on the sine's own frequency (Mid, M Freq)
                defaults[3], defaults[4] = 12.0, 440.0
            # Two runs, each from a freshly loaded copy (a plugin keeps its state in statics, which dlopen shares per file).
            so2 = os.path.join(tmp, stem + "_again.so")
            shutil.copyfile(so, so2)
            block = 2048 if stem == "wt_resynth" else BLOCK  # WT Resynth takes whole 2048-frame slices: a shorter block ends its run
            ins, outs, rets, calls = run_plugin(so, info, scratch, defaults, block)
            ins2, outs2, rets2, calls2 = run_plugin(so2, info, scratch, defaults, block)
            problems = []
            if min(rets) < 0:
                problems.append("process_block returned %s" % rets)
            if outs != outs2 or calls != calls2:
                problems.append("two runs from the same state differ")
            generator = bool(info["flags"] & 0x1)
            if not any(any(c) for c in outs):
                problems.append("the output is silent")
            if not generator and outs == ins and stem != "add_silence":
                problems.append("the effect left the input untouched")
            if stem == "add_silence":
                if outs != ins:
                    problems.append("Add Silence does not copy its input")
                if calls[-1] != (110250, 0):
                    problems.append("Add Silence asked for %s, expected (110250, 0) for 2500 ms in front" % (calls[-1],))
            if stem == "stereo_delay" and not (calls and calls[-1][0] == 0 and calls[-1][1] > 0):
                problems.append("Stereo Delay asked for no tail: %s" % (calls,))
            if stem == "delay" and (not calls or calls[-1] != (0, 5 * 11025 + 1)):
                problems.append("Delay asked for %s, expected (0, 55126): 50 %% feedback, 250 ms, voices to 1 %%" % (calls[-1:],))
            if stem == "bitcrusher":
                if any(v % 256 for v in outs[0]):
                    problems.append("Bitcrusher output is not a multiple of 256 at 8 bits")
                if any(outs[0][i] != outs[0][i - i % 6] for i in range(len(outs[0]))):
                    problems.append("Bitcrusher does not hold each sample for 6 frames at 8000 Hz")
            if stem == "limiter":
                last = BLOCK
                if max(abs(v) for v in outs[0][-last:]) >= 0.95 * max(abs(v) for v in ins[0][-last:]):
                    problems.append("Limiter does not bring the level down at -18 dB")
            if stem == "eq3":
                peak_in = float(max(abs(v) for v in ins[0][-BLOCK:]))
                gain = max(abs(v) for v in outs[0][-BLOCK:]) / peak_in
                if not 3.0 < gain < 5.0:
                    problems.append("EQ at +12 dB on 440 Hz gives x%.2f, expected about x4 (3.0..5.0)" % gain)
                cut = list(flat)
                cut[3], cut[4] = -12.0, 440.0
                _, outs_cut, _, _ = run_plugin(so2, info, scratch, cut)
                gain = max(abs(v) for v in outs_cut[0][-BLOCK:]) / peak_in
                if not 0.1 < gain < 0.4:
                    problems.append("EQ at -12 dB on 440 Hz gives x%.2f, expected about x0.2" % gain)
                _, outs_flat, _, _ = run_plugin(so2, info, scratch, flat)
                if any(abs(a - b) > 1 for c in range(2) for a, b in zip(outs_flat[c], ins[c])):
                    problems.append("EQ at its flat defaults changes the sound by more than 1 step")
            if stem == "pad_stretch" and calls[-1:] != [(0, 8 * 44100)]:
                problems.append("Pad Stretch asked for %s, expected (0, 352800): Room 8 s behind the sample" % (calls[-1:],))
            if stem in ("wt_forge", "wt_notes", "grain_lock", "pad_forge", "wt_resynth", "pad_stretch") and max(rets) > block:
                problems.append("returned more frames than offered: %s" % rets)
            if stem == "wt_resynth" and rets != [2048] * BLOCKS:
                problems.append("WT Resynth returned %s, expected one 2048-frame slice a call" % rets)
            if stem == "wt_resynth":  # offered more than a slice it takes one and the host calls again (a short positive return)
                _, _, rets_big, _ = run_plugin(so2, info, scratch, defaults, 8192)
                if rets_big != [2048] * BLOCKS:
                    problems.append("WT Resynth offered 8192 frames returned %s, expected 2048 a call" % rets_big)
            if stem == "stereo_to_mono":
                want = [max(-32768, min(32767, (a + b) // 2)) for a, b in zip(ins[0], ins[1])]
                if all(abs(x - y) > 1 for x, y in zip(outs[0], want)):
                    problems.append("Stereo to Mono is not the sum of the channels / 2")
            if problems:
                print("ptfx_pack: FAILED: smoke %s: %s" % (stem, "; ".join(problems)))
                failed += 1
            else:
                print("ptfx_pack: OK smoke %s: peak %d, %d extra-sample call(s)" % (stem, max(abs(v) for c in outs for v in c), len(calls)))
    return failed


def check_reproducible():
    """Each plugin is built twice, into two different directories, and compared byte for byte with each other and with the file the o3_ptfx
    target made (OS-PTFXPACK review minor): the starter zip is reproducible, so the .ptfx files in it must be."""
    prefix = sys.argv[3] if len(sys.argv) > 3 else ""
    if not prefix:
        if ci():
            print("ptfx_pack: FAILED: no toolchain prefix for the double-build check, and O3_CI=1")
            return 1
        print("ptfx_pack: the double-build check is skipped: no toolchain prefix was given")
        return 0
    failed = 0
    packer = os.path.join(ROOT, "tools", "ptfx_pack.py")
    with tempfile.TemporaryDirectory() as tmp:
        for stem in stems():
            if not os.path.isfile(os.path.join(BUILD, stem + ".ptfx")):
                continue
            blobs = []
            for run in ("a", "b"):
                out = os.path.join(tmp, run, stem, stem + ".ptfx")
                os.makedirs(os.path.dirname(out))
                res = subprocess.run([sys.executable, "-I", packer, os.path.join(SRC, stem, stem + ".c"), "-o", out,
                                      "--abi-dir", os.path.join(SRC, "sdk"), "--toolchain", prefix],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
                if res.returncode != 0:
                    print("ptfx_pack: FAILED: %s did not build again:\n%s" % (stem, res.stdout))
                    failed += 1
                    break
                with open(out, "rb") as f:
                    blobs.append(f.read())
            if len(blobs) < 2:
                continue
            with open(os.path.join(BUILD, stem + ".ptfx"), "rb") as f:
                built = f.read()
            if blobs[0] != blobs[1]:
                print("ptfx_pack: FAILED: %s.ptfx is not reproducible: two builds differ" % stem)
                failed += 1
            elif blobs[0] != built:
                print("ptfx_pack: FAILED: %s.ptfx of the build directory differs from a fresh build (stale?)" % stem)
                failed += 1
    if not failed:
        print("ptfx_pack: OK double build: every plugin identical byte for byte")
    return failed


def main():
    insp = load("ptfx_inspect", os.path.join(ROOT, "tools", "ptfx_inspect.py"))
    failed = check_integrity()
    present = [s for s in stems() if os.path.isfile(os.path.join(BUILD, s + ".ptfx"))]
    if not present:
        if failed:
            return 1
        return skip_or_fail("no .ptfx was built under %s (no arm-none-eabi toolchain: set ARM_GNU_ROOT)" % BUILD)
    failed += check_built(insp)
    failed += check_reproducible()
    smoke = check_smoke(insp)
    if smoke is None:
        if ci():
            print("ptfx_pack: FAILED: no native C compiler for the host smoke, and O3_CI=1")
            return 1
        print("ptfx_pack: the host smoke is skipped: no native C compiler")
    else:
        failed += smoke
    if failed:
        print("ptfx_pack: FAILED: %d problem(s)" % failed)
        return 1
    print("ptfx_pack: OK (%d plugin(s))" % len(stems()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
