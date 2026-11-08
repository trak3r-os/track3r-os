#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest `starter_python` (OS-STARTER): tools/release/make_starter.py and its generators, with no network (standard library only).

usage: starter_test.py REPO_ROOT [O3PJ_TOOL]

  - the committed manifest passes the validator, and the validator refuses an unknown licence, a bad path, a bad hash, a duplicate target,
    a Mod Archive file with a licence the archive does not use, and a licence whose text is missing;
  - the generators (drum kit, wavetables, MED song, demo projects) give the same bytes twice, and what they give is what it claims to be:
    16-bit mono WAV, wavetables of whole 2,048-sample windows with the 'clm ' marker, a MED module and two projects that o3project.py reads
    (and the C++ reader too, byte for byte, when O3PJ_TOOL is given);
  - the whole archive built twice in offline mode, from fake downloads in a temporary cache, is byte-identical; its entries are sorted, with
    one time stamp, and SHA256SUMS matches them;
  - a download with a wrong SHA-256, a Mod Archive page whose licence changed or that cannot be read, a licence outside the allow-list and an
    output directory inside the repository are all refused;
  - OS-PTFXPACK: the `ptfx` list is every plugin folder of modules/ptfx, each source says MIT, the .ptfx files land in Effects/ with the MIT licence
    text and a CREDITS.txt entry, and a missing file, a file the 3.1 checks refuse, a source that is not MIT, a missing --ptfx-dir and an MIT
    entry in `files` (MIT is for the `ptfx` section only) are refused; so is a .ptfx file older than its source (a stale build) and a
    --ptfx-build directory that cannot be built.
Nothing here uses the network: http_get is replaced where a download is exercised.
"""
import copy
import hashlib
import importlib.util
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
import zipfile


def load(root):
    spec = importlib.util.spec_from_file_location("make_starter", os.path.join(root, "tools", "release", "make_starter.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print("FAILED: " + what)


def refuses(ms, fn, needle, what):
    try:
        fn()
    except ms.StarterError as e:
        check(needle in str(e), "%s: the message should contain %r, got %r" % (what, needle, str(e)))
        return
    check(False, what + ": was not refused")


def sha(data):
    return hashlib.sha256(data).hexdigest()


PD_PAGE = b'<h2>License</h2><ul><li class="stats">Licensed under the <a class="stats" href="https://creativecommons.org/licenses/publicdomain/">Public Domain license</a></li></ul>'
BY_PAGE = b'<li class="stats">Licensed under the <a class="stats" href="https://creativecommons.org/licenses/by/4.0/">Attribution license</a></li>'
FAKE = {
    "one": b"fake cc0 file one",
    "two": b"fake public domain module",
    "three": b"fake surge wavetable",
    "four": b"fake cc-by module",
    "cc0": b"CC0 fake legal text",
    "gpl": b"GPL fake legal text",
}


def fake_manifest():
    def entry(dest, key, licence, **extra):
        e = {"dest": dest, "url": "https://example.invalid/" + key, "sha256": sha(FAKE[key]), "licence": licence,
             "author": "Author " + key, "title": "Title " + key, "source": "https://example.invalid/src/" + key}
        e.update(extra)
        return e
    return {
        "schema": 1,
        "modules": {"apps": ["fakeapp"], "instruments": ["fakeinst"], "effects": ["fakefx"], "importers": ["faketracker"]},
        "presets": ["sid"],
        "ptfx": sorted(FAKE_PTFX),
        "files": [entry("Samples/Fake/one.wav", "one", "CC0-1.0"),
                  entry("Songs/MOD/two.mod", "two", "Public-Domain", modarchive_id=4242),
                  entry("WTABLE/Surge/three.wt", "three", "GPL-3.0"),
                  entry("Songs/MOD/four.mod", "four", "CC-BY-4.0", modarchive_id=4343)],
        "licence_texts": [{"dest": "LICENSES/CC0-1.0.txt", "url": "https://example.invalid/cc0", "sha256": sha(FAKE["cc0"]), "licence": "CC0-1.0"},
                          {"dest": "LICENSES/GPL-3.0.txt", "url": "https://example.invalid/gpl", "sha256": sha(FAKE["gpl"]), "licence": "GPL-3.0"}],
    }


def fake_ptfx(name):
    """A .ptfx file that passes the 3.1 checks of tools/ptfx_inspect.py: the 472-byte header (version 1, no flags, no params, three entry points) and 16 payload bytes."""
    h = bytearray(472)
    struct.pack_into("<IHH", h, 0, 0x58465450, 1, 0)
    h[8:8 + len(name)] = name.encode()
    h[40:44] = b"test"
    struct.pack_into("<IIIIII", h, 64, 472, 16, 0, 0, 4, 8)   # code_offset, code_size, bss_size, init, set_param, process_block
    return bytes(h) + bytes(16)


FAKE_PTFX = {"overdrive": "Overdrive", "delay": "Delay"}     # stems with a real source in modules/ptfx (the MIT check reads it)


def fake_tmod(kind):
    """The 64 bytes of a module header that the packer's kind check reads: the magic, then (header size, target) and the kind byte at 8."""
    return b"O3MD" + bytes(4) + bytes([kind]) + bytes(55)


def fill_cache(cache):
    os.makedirs(cache, exist_ok=True)
    for data in FAKE.values():
        with open(os.path.join(cache, sha(data)), "wb") as f:
            f.write(data)
    for mid, page in ((4242, PD_PAGE), (4343, BY_PAGE)):
        with open(os.path.join(cache, "modarchive-%d.html" % mid), "wb") as f:
            f.write(page)


# ---- the manifest -------------------------------------------------------------------------------------------------------------

def test_manifest(ms, root):
    real = ms.load_manifest(os.path.join(root, "tools", "release", "starter_manifest.json"))
    try:
        ms.validate_manifest(real)
    except ms.StarterError as e:
        check(False, "the committed manifest: %s" % e)
    check(len(real["files"]) > 0, "the committed manifest lists files")
    check({e["licence"] for e in real["files"]} <= set(ms.ALLOWED_LICENCES), "committed licences are allowed")
    # OS-FLASHDIET I3: the zip ships the importer module the image needs on the card to read MOD / S3M / MED.
    check(real["modules"].get("importers") == ["tracker"], "the committed manifest lists modules/importers/tracker.tmod")
    # OS-STARTER 1.2.0: every module the repository builds for the card is in the zip, none that the firmware already holds, no template.
    cmake = open(os.path.join(root, "modules", "CMakeLists.txt")).read()
    builtin = set(re.search(r"set\(O3M_BUILTIN_STEMS ([^)]*)\)", cmake).group(1).split())
    check(builtin == {"tb303", "chip", "fm", "sid"}, "the firmware's built-in modules are tb303, chip, fm and sid (update the starter manifest and README if this changed)")
    check(set(real["presets"]) == builtin, "a preset pack of every built-in module is in the zip")
    for stem in builtin:
        check(os.path.isfile(os.path.join(root, "modules", stem, "presets.json")), "modules/%s/presets.json exists" % stem)
    dirs = lambda sub: sorted(d for d in os.listdir(os.path.join(root, "modules", sub)) if os.path.isdir(os.path.join(root, "modules", sub, d)))
    check(real["modules"]["effects"] == dirs("effects"), "the manifest lists every folder of modules/effects: %s" % dirs("effects"))
    check(real["modules"]["apps"] == [d for d in dirs("apps") if d != "common"], "the manifest lists every app of modules/apps (common is a header library)")
    check(real["modules"]["instruments"] == ["sine"], "the only instrument module outside the firmware is sine")
    check(not builtin & set(real["modules"]["instruments"] + real["modules"]["effects"] + real["modules"]["apps"]), "no built-in module is packed to the card")
    for kind in ("apps", "instruments", "effects", "importers"):
        for stem in real["modules"][kind]:
            check(re.search(r"o3m_add_module\(%s\b" % stem, cmake) is not None, "modules/CMakeLists.txt builds the %s module %s" % (kind, stem))
    for e in real["files"]:
        if e["licence"] == "CC-BY-4.0":
            check(bool(e["author"] and e["title"] and e["source"]), "CC-BY attribution of " + e["dest"])
    # OS-PTFXPACK: every plugin folder of modules/ptfx (but the SDK header's) is in the zip, in the order CMake builds it, and each source is MIT.
    pdirs = sorted(d for d in os.listdir(os.path.join(root, "modules", "ptfx")) if os.path.isdir(os.path.join(root, "modules", "ptfx", d)) and d != "sdk")
    check(sorted(real["ptfx"]) == pdirs, "the manifest lists every plugin folder of modules/ptfx: %s" % pdirs)
    pcmake = open(os.path.join(root, "modules", "ptfx", "CMakeLists.txt")).read()
    built = re.search(r"set\(O3_PTFX_STEMS ([^)]*)\)", pcmake).group(1).split()
    check(sorted(built) == sorted(real["ptfx"]), "modules/ptfx/CMakeLists.txt builds exactly the plugins of the manifest")
    for stem in real["ptfx"]:
        with open(os.path.join(root, "modules", "ptfx", stem, stem + ".c")) as f:
            check(f.readline().strip() == "// SPDX-License-Identifier: MIT", "modules/ptfx/%s is MIT" % stem)
    check(os.path.isfile(os.path.join(root, ms.PTFX_LICENCE_SRC)), "the MIT licence text of the plugins is in the repository")
    check("MIT" not in ms.ALLOWED_LICENCES, "MIT is not allowed for `files`: only the `ptfx` section carries it")
    ms.validate_manifest(fake_manifest())

    def bad(mutate, needle, what):
        m = fake_manifest()
        mutate(m)
        refuses(ms, lambda: ms.validate_manifest(m), needle, what)

    bad(lambda m: m["files"][0].update(licence="CC-BY-SA-4.0"), "not allowed", "a licence outside the allow-list")
    bad(lambda m: m["files"][0].update(licence="All-Rights-Reserved"), "not allowed", "an unknown licence")
    bad(lambda m: m["files"][0].update(dest="../escape.wav"), "bad target path", "a path with ..")
    bad(lambda m: m["files"][0].update(dest="/abs.wav"), "bad target path", "an absolute path")
    bad(lambda m: m["files"][0].update(dest="Other/x.wav"), "outside", "a path outside the card folders")
    bad(lambda m: m["files"][0].update(sha256="abc"), "sha256", "a short hash")
    bad(lambda m: m["files"][0].update(url="http://example.invalid/x"), "https", "a plain http URL")
    bad(lambda m: m["files"][1].update(dest="Samples/Fake/one.wav"), "twice", "a duplicate target")
    bad(lambda m: m["files"][0].pop("author"), "author", "a missing author")
    bad(lambda m: m["files"][0].update(modarchive_id=7), "Mod Archive", "a Mod Archive file that is CC0")
    bad(lambda m: m["licence_texts"].pop(0), "licence_texts", "a licence without its text")
    bad(lambda m: m.update(schema=2), "schema", "another schema")
    bad(lambda m: m["modules"].pop("importers"), "needs the lists", "a modules block without the importers list")
    bad(lambda m: m["files"][0].update(licence="MIT"), "not allowed", "an MIT file in `files` (MIT is for the ptfx section only)")
    bad(lambda m: m.update(ptfx="overdrive"), "'ptfx' must be a list", "a ptfx entry that is not a list")
    bad(lambda m: m["ptfx"].append("../x"), "bad .ptfx plugin name", "a plugin name with a path")
    bad(lambda m: m["ptfx"].append("delay"), "twice", "a plugin listed twice")


# ---- the generators -----------------------------------------------------------------------------------------------------------

def pcm_of(data):
    w = wave.open(io.BytesIO(data))
    return w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()


def test_generators(ms, root, o3pj_tool):
    first = ms.generate()
    second = ms.generate()
    check(first == second, "generate() twice gives the same files")
    check(len(first) == len(second) and sorted(first) == sorted(second), "generate() gives the same names")
    check(any(p.startswith("Samples/Drums/") for p in first), "a drum kit")
    for path, data in sorted(first.items()):
        if path.startswith("Samples/Drums/") or path.startswith("WTABLE/Generated/") or path.endswith(".wav"):
            ch, width, rate, frames = pcm_of(data)
            check((ch, width, rate) == (1, 2, 44100) and frames > 0, "%s is 16-bit mono 44.1 kHz WAV" % path)
        if path.startswith("WTABLE/Generated/"):
            ch, width, rate, frames = pcm_of(data)
            check(frames % 2048 == 0 and frames // 2048 == 32, "%s is 32 windows of 2048 samples" % path)
            i = data.find(b"clm ")
            check(0 < i < data.find(b"data") and data[i + 8:i + 14] == b"<!>204", "%s carries the 'clm ' marker before the data" % path)
    check(first["Songs/MED/starter_groove.med"][:4] == b"MMD1", "the MED song is an MMD1 module")
    sys.path.insert(0, os.path.join(root, "tools"))
    try:
        import o3project
    finally:
        sys.path.remove(os.path.join(root, "tools"))
    for name in ("Starter-Samples", "Starter-Modules"):
        data = first["Projects/%s/project.o3pj" % name]
        p = o3project.parse(data)
        check(o3project.build(p) == data, name + " rewrites to the same bytes")
        check(len(p["patterns"]) >= 2 and len(p["song"]) > 0 and len(p["instruments"]) > 0, name + " has patterns, a song and instruments")
        if name == "Starter-Samples":
            for inst in p["instruments"].values():
                path = inst["path"].decode()
                check("Projects/%s/%s" % (name, path) in first, "%s: the sample %s is in the archive" % (name, path))
        else:
            check(len(p["modules"]) == 3 and all(i["kind"] == 3 for i in p["instruments"].values()), name + " uses three modules")
    if o3pj_tool:
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("Starter-Samples", "Starter-Modules"):
                src = os.path.join(tmp, name + ".o3pj")
                dst = os.path.join(tmp, name + ".resaved")
                with open(src, "wb") as f:
                    f.write(first["Projects/%s/project.o3pj" % name])
                done = subprocess.run([o3pj_tool, "resave", src, dst], capture_output=True)
                check(done.returncode == 0, "the C++ reader reads " + name)
                if done.returncode == 0:
                    with open(dst, "rb") as f:
                        check(f.read() == first["Projects/%s/project.o3pj" % name], "the C++ writer rewrites %s byte for byte" % name)
    return first


# ---- the archive --------------------------------------------------------------------------------------------------------------

def run_build(ms, tmp, tag, manifest_path, ptf, modules, cache, extra=()):
    out = os.path.join(tmp, "out-" + tag)
    code = ms.main(["--version", "9.9.9", "--ptf", ptf, "--modules-dir", modules, "--ptfx-dir", os.path.join(tmp, "ptfx"), "--out", out, "--cache", cache,
                    "--manifest", manifest_path, "--offline", *extra])
    return code, out


def test_archive(ms, root):
    import json
    with tempfile.TemporaryDirectory() as tmp:
        cache = os.path.join(tmp, "cache")
        fill_cache(cache)
        mpath = os.path.join(tmp, "manifest.json")
        with open(mpath, "w") as f:
            json.dump(fake_manifest(), f)
        ptf = os.path.join(tmp, "track3r-os-9.9.9.ptf")
        with open(ptf, "wb") as f:
            f.write(b":020000040000FA\n:00000001FF\n")
        modules = os.path.join(tmp, "modules")
        os.makedirs(modules)
        with open(os.path.join(modules, "fakeapp.tmod"), "wb") as f:
            f.write(fake_tmod(4))
        with open(os.path.join(modules, "faketracker.tmod"), "wb") as f:
            f.write(fake_tmod(3))  # OS-FLASHDIET I3: the importer module, as modules/importers/tracker.tmod in the real zip
        with open(os.path.join(modules, "fakeinst.tmod"), "wb") as f:
            f.write(fake_tmod(1))  # OS-STARTER 1.2.0: an instrument module, as modules/instruments/sine.tmod
        with open(os.path.join(modules, "fakefx.tmod"), "wb") as f:
            f.write(fake_tmod(2))  # an effect module, as modules/effects/<name>.tmod
        ptfx_dir = os.path.join(tmp, "ptfx")
        os.makedirs(ptfx_dir)
        for stem, name in FAKE_PTFX.items():
            with open(os.path.join(ptfx_dir, stem + ".ptfx"), "wb") as f:
                f.write(fake_ptfx(name))   # a plugin as modules/ptfx builds it, as Effects/<name>.ptfx in the real zip

        code1, out1 = run_build(ms, tmp, "a", mpath, ptf, modules, cache)
        code2, out2 = run_build(ms, tmp, "b", mpath, ptf, modules, cache)
        check(code1 == 0 and code2 == 0, "both offline builds succeed")
        z1 = os.path.join(out1, "track3r-os-9.9.9.zip")
        z2 = os.path.join(out2, "track3r-os-9.9.9.zip")
        with open(z1, "rb") as a, open(z2, "rb") as b:
            d1, d2 = a.read(), b.read()
        check(d1 == d2 and len(d1) > 0, "two offline builds give byte-identical zips")
        z = zipfile.ZipFile(io.BytesIO(d1))
        names = z.namelist()
        check(names == sorted(names) and len(names) == len(set(names)), "entries are sorted and unique")
        check({i.date_time for i in z.infolist()} == {ms.ZIP_TIME}, "one fixed time stamp")
        for want in ("Firmware/track3r-os-9.9.9.ptf", "Firmware/track3r-os-9.9.9.ptf.sha256", "modules/apps/fakeapp.tmod", "modules/importers/faketracker.tmod",
                     "modules/instruments/fakeinst.tmod", "modules/effects/fakefx.tmod", "README.txt", "SHA256SUMS",
                     "Effects/delay.ptfx", "Effects/overdrive.ptfx", "LICENSES/MIT-tracker-ce.txt",
                     "LICENSES/CREDITS.txt", "LICENSES/CC0-1.0.txt", "LICENSES/GPL-3.0.txt", "LICENSES/CC-BY-4.0.txt", "LICENSES/Public-Domain.txt",
                     "Samples/Fake/one.wav", "Songs/MOD/two.mod", "WTABLE/Surge/three.wt", "Songs/MOD/four.mod",
                     "Samples/Drums/kick.wav", "Songs/MED/starter_groove.med", "Projects/Starter-Samples/project.o3pj",
                     "Projects/Starter-Modules/project.o3pj", "WTABLE/Generated/wt_sine_to_saw.wav"):
            check(want in names, "the zip has " + want)
        check(any(n.startswith("modules/presets/sid/") and n.endswith(".o3mp") for n in names), "the zip has the preset pack of sid under modules/presets/sid/")
        sums = z.read("SHA256SUMS").decode().splitlines()
        check(len(sums) == len(names) - 1, "SHA256SUMS lists every file but itself")
        for line in sums:
            digest, path = line.split("  ", 1)
            check(sha(z.read(path)) == digest, "SHA256SUMS: " + path)
        check(z.read("Firmware/track3r-os-9.9.9.ptf.sha256").decode().split()[0] == sha(open(ptf, "rb").read()), "the .ptf checksum file")
        credits = z.read("LICENSES/CREDITS.txt").decode()
        check("Author four" in credits and "Title four" in credits and "CC BY 4.0" in credits, "the CC-BY attribution is in CREDITS.txt")
        check("Surge Synth Team" in credits, "the GPL notice is in CREDITS.txt")
        check(z.read("Effects/overdrive.ptfx") == fake_ptfx("Overdrive"), "a plugin is packed byte for byte")
        check("MIT: MIT License of Polyend Tracker Community Edition" in credits and "Effects/overdrive.ptfx" in credits and "Overdrive" in credits
              and "Polyend Sp. z o.o." in credits, "the MIT notice and the plugin list are in CREDITS.txt")
        check(z.read("LICENSES/MIT-tracker-ce.txt") == open(os.path.join(root, ms.PTFX_LICENCE_SRC), "rb").read(), "the MIT licence text is the repository's")
        check("Effects/" in z.read("README.txt").decode(), "the README describes Effects/")

        # a changed cache file, a licence change on a Mod Archive page, a page that cannot be read
        def build_with(mutate_cache=None, manifest=None, ptf_path=ptf):
            c = os.path.join(tmp, "cache-x")
            if os.path.isdir(c):
                import shutil
                shutil.rmtree(c)
            fill_cache(c)
            if mutate_cache:
                mutate_cache(c)
            mp = mpath
            if manifest is not None:
                mp = os.path.join(tmp, "manifest-x.json")
                with open(mp, "w") as f:
                    json.dump(manifest, f)
            return lambda: ms.run(["--version", "9.9.9", "--ptf", ptf_path, "--modules-dir", modules, "--ptfx-dir", ptfx_dir, "--out", os.path.join(tmp, "out-x"),
                                   "--cache", c, "--manifest", mp, "--offline"])

        def corrupt(c):
            with open(os.path.join(c, sha(FAKE["one"])), "wb") as f:
                f.write(b"tampered")
        refuses(ms, build_with(corrupt), "does not match", "a tampered cache file")

        # OS-STARTER 1.2.0: a module listed under the wrong kind is refused (the firmware looks in the folder of the module's kind only)
        for kind, stem in (("apps", "fakefx"), ("effects", "fakeapp"), ("instruments", "faketracker"), ("importers", "fakeinst")):
            wrong = fake_manifest()
            wrong["modules"] = {"apps": [], "instruments": [], "effects": [], "importers": []}
            wrong["modules"][kind] = [stem]
            refuses(ms, build_with(manifest=wrong), "module kind", "%s listed under modules.%s" % (stem, kind))
        short = fake_manifest()
        short["modules"]["apps"] = ["fakeshort"]
        with open(os.path.join(modules, "fakeshort.tmod"), "wb") as f:
            f.write(b"O3MD")
        refuses(ms, build_with(manifest=short), "not a .tmod", "a truncated module file")

        # OS-PTFXPACK: a plugin that is not built, one the 3.1 checks refuse, a source that is not MIT, and no --ptfx-dir
        os.remove(os.path.join(ptfx_dir, "delay.ptfx"))
        refuses(ms, build_with(), "not in", "a plugin file that is not built")
        with open(os.path.join(ptfx_dir, "delay.ptfx"), "wb") as f:
            f.write(b"PTFX" + bytes(600))
        refuses(ms, build_with(), "refused by the .ptfx checks", "a plugin file with a bad header")
        with open(os.path.join(ptfx_dir, "delay.ptfx"), "wb") as f:
            f.write(fake_ptfx("Delay")[:480])
        refuses(ms, build_with(), "refused by the .ptfx checks", "a plugin file cut short")
        with open(os.path.join(ptfx_dir, "delay.ptfx"), "wb") as f:
            f.write(fake_ptfx("Delay"))
        refuses(ms, lambda: ms.run(["--version", "9.9.9", "--ptf", ptf, "--modules-dir", modules, "--out", os.path.join(tmp, "out-y"), "--cache", cache,
                                    "--manifest", mpath, "--offline"]), "--ptfx-dir", "a manifest with plugins and no --ptfx-dir")
        gpl_repo = os.path.join(tmp, "repo")      # a stand-in tree: the plugin's source says GPL
        os.makedirs(os.path.join(gpl_repo, "modules", "ptfx", "delay"))
        os.makedirs(os.path.join(gpl_repo, "tools"))
        for name in ("ptfx_inspect.py",):
            with open(os.path.join(root, "tools", name), "rb") as src, open(os.path.join(gpl_repo, "tools", name), "wb") as dst:
                dst.write(src.read())
        with open(os.path.join(gpl_repo, "modules", "ptfx", "delay", "delay.c"), "w") as f:
            f.write("// SPDX-License-Identifier: GPL-3.0-or-later\n")
        one = {"ptfx": ["delay"]}
        refuses(ms, lambda: ms.collect_ptfx(one, ptfx_dir, gpl_repo), "SPDX-License-Identifier: MIT", "a plugin whose source is not MIT")
        refuses(ms, lambda: ms.collect_ptfx({"ptfx": ["nosuch"]}, ptfx_dir, gpl_repo), "not in", "a plugin that has no file")
        os.remove(os.path.join(gpl_repo, "modules", "ptfx", "delay", "delay.c"))
        refuses(ms, lambda: ms.collect_ptfx(one, ptfx_dir, gpl_repo), "source", "a plugin without its source")
        check(ms.collect_ptfx({}, None)[0] == {}, "no plugins, no --ptfx-dir needed")
        # A stale build must not ship (review minor): the .ptfx older than its source is refused, a newer one is packed; --ptfx-build fails on a directory that is no build.
        psrc = os.path.join(gpl_repo, "modules", "ptfx", "delay", "delay.c")
        with open(psrc, "w") as f:
            f.write("// SPDX-License-Identifier: MIT\n")
        pfile = os.path.join(ptfx_dir, "delay.ptfx")
        os.utime(psrc, (2000000000, 2000000000))
        os.utime(pfile, (1900000000, 1900000000))
        refuses(ms, lambda: ms.collect_ptfx(one, ptfx_dir, gpl_repo), "stale", "a plugin file older than its source")
        os.utime(pfile, (2100000000, 2100000000))
        check(sorted(ms.collect_ptfx(one, ptfx_dir, gpl_repo)[0]) == ["Effects/delay.ptfx"], "a plugin file newer than its source is packed")
        refuses(ms, lambda: ms.build_ptfx(os.path.join(tmp, "no-such-build")), "--ptfx-build", "--ptfx-build on a directory that is no build")

        def missing(c):
            os.remove(os.path.join(c, sha(FAKE["three"])))
        refuses(ms, build_with(missing), "not in the cache", "a file missing offline")

        def changed_licence(c):
            with open(os.path.join(c, "modarchive-4242.html"), "wb") as f:
                f.write(BY_PAGE)
        refuses(ms, build_with(changed_licence), "changed", "a Mod Archive licence that changed")

        def unreadable(c):
            with open(os.path.join(c, "modarchive-4343.html"), "wb") as f:
                f.write(b"<html>The Mod Archive is down for maintenance</html>")
        refuses(ms, build_with(unreadable), "cannot read the licence", "a Mod Archive page without a licence line")

        def no_page(c):
            os.remove(os.path.join(c, "modarchive-4343.html"))
        refuses(ms, build_with(no_page), "not in the cache", "a Mod Archive page that is not there")

        bad_licence = fake_manifest()
        bad_licence["files"][0]["licence"] = "CC-BY-SA-4.0"
        refuses(ms, build_with(manifest=bad_licence), "not allowed", "the build with a licence outside the allow-list")
        refuses(ms, build_with(ptf_path=os.path.join(tmp, "nothing.ptf")), ".ptf", "a missing firmware file")

        # downloads with the network replaced: a wrong hash fails and stores nothing, a right one is stored; the page is re-read every time
        real_get = ms.http_get
        try:
            cache2 = os.path.join(tmp, "cache-online")
            entry = fake_manifest()["files"][0]
            ms.http_get = lambda url, retries=3: b"not what the manifest says"
            refuses(ms, lambda: ms.fetch(entry, cache2, False), "SHA-256 mismatch", "a download with a wrong hash")
            check(not os.path.exists(os.path.join(cache2, entry["sha256"])), "a bad download is not cached")
            ms.http_get = lambda url, retries=3: FAKE["one"]
            check(ms.fetch(entry, cache2, False) == FAKE["one"], "a good download is returned")
            check(os.path.exists(os.path.join(cache2, entry["sha256"])), "a good download is cached")
            md = fake_manifest()["files"][1]
            ms.http_get = lambda url, retries=3: BY_PAGE
            refuses(ms, lambda: ms.check_modarchive(md, cache2, False), "changed", "a licence that changed online")

            def down(url, retries=3):
                raise ms.StarterError("cannot download " + url)
            ms.http_get = down
            refuses(ms, lambda: ms.check_modarchive(md, cache2, False), "cannot read the licence", "a Mod Archive page that cannot be fetched")
            ms.http_get = lambda url, retries=3: PD_PAGE
            check("Public Domain" in ms.check_modarchive(md, cache2, False), "an unchanged licence passes")
        finally:
            ms.http_get = real_get

        # the repository is never written to
        refuses(ms, lambda: ms.check_out_dir(os.path.join(root, "tools")), "inside the repository", "an output directory in the tree")
        refuses(ms, lambda: ms.check_out_dir(root), "inside the repository", "the repository root as output")
        ms.check_out_dir(os.path.join(root, "build-release"))      # allowed: untracked
        ms.check_out_dir(tmp)
        refuses(ms, lambda: ms.check_version("1/../2"), "bad --version", "a version with a slash")


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    root = os.path.abspath(argv[1])
    o3pj_tool = argv[2] if len(argv) == 3 else None
    ms = load(root)
    test_manifest(ms, root)
    test_generators(ms, root, o3pj_tool)
    test_archive(ms, root)
    if FAILS:
        print("%d check(s) failed" % len(FAILS))
        return 1
    print("starter_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
