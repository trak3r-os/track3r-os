#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""ctest `testdata_python` (OS-HILCARD): tools/testdata/testdata.py with no network (standard library only).

usage: testdata_test.py REPO_ROOT

  - the committed manifest passes the validator;
  - the validator refuses a licence outside the allow-list for a bundled file, a bad path, a bad hash, a duplicate target, 'Maintainer-own'
    outside Projects/t-v16/, a file outside the asset with no https url, and a bundled file under ptfx-sdk/;
  - the asset built twice from fake sources is byte-identical, its entries are sorted with one time stamp, and `fetch --zip` unpacks it into a
    temporary directory with every file equal to its source;
  - an asset whose SHA-256 differs from the manifest is refused before anything is written, a source file that differs from the manifest fails
    the pack, a stray, a changed or a missing member of the asset is refused with nothing written, `--third-party` lists a manual entry and counts a failed download, and an output directory inside the repository outside build*/ is refused;
  - a plain `fetch` downloads only the asset (an offline `fetch --zip FILE` touches no network), `--third-party` brings every file outside
    the asset including ptfx-sdk/, and `fetch --only ptfx-sdk/` brings just those without the release asset; `--only` with a prefix that
    matches nothing is refused.
Nothing here uses the network.
"""
import copy
import hashlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import zipfile

ROOT = sys.argv[1]
spec = importlib.util.spec_from_file_location("testdata", os.path.join(ROOT, "tools", "testdata", "testdata.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def expect_error(fn, fragment, what):
    try:
        fn()
    except td.TestdataError as e:
        assert fragment in str(e), "%s: wrong message: %s" % (what, e)
        return
    raise AssertionError("%s: not refused" % what)


def fake_manifest(tv16_files, o3_files, zip_info=None):
    files = []
    for rel, data in sorted(tv16_files.items()):
        files.append(dict(dest="Projects/t-v16/" + rel, sha256=sha(data), size=len(data), licence="Maintainer-own", author="a", source="s", bundled=True))
    for rel, (data, lic) in sorted(o3_files.items()):
        files.append(dict(dest="O3TEST/" + rel, sha256=sha(data), size=len(data), licence=lic, author="a", source="s", bundled=True))
    files.append(dict(dest="O3TEST/MOD/outside.mod", sha256=sha(b"x"), size=1, licence="unknown", author="a", source="s", bundled=False,
                      url="https://example.invalid/outside.mod"))
    return dict(schema=1, release=dict(tag="t", asset="t.zip", url="https://example.invalid/t.zip", sha256="0" * 64, size=1), files=files)


def write_tree(root, files):
    for rel, data in files.items():
        p = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


def main():
    real = td.load_manifest(os.path.join(ROOT, "tools", "testdata", "testdata_manifest.json"))
    td.validate_manifest(real)

    tv = {"patterns/pattern_01.mtp": b"P" * 100, "samples/a.wav": b"RIFF" + bytes(range(64)), "project.mt": b"M" * 10}
    o3 = {"MOD/gen.mod": (b"MOD" * 40, "CC0-1.0"), "XM/pd.xm": (b"XM" * 30, "Public-Domain")}
    m = fake_manifest(tv, o3)
    td.validate_manifest(m)

    # the validator
    def bad(change, fragment, what):
        c = copy.deepcopy(m)
        change(c)
        expect_error(lambda: td.validate_manifest(c), fragment, what)
    bad(lambda c: c["files"][3].update(licence="GPL-3.0"), "may not go into the asset", "GPL in the asset")
    bad(lambda c: c["files"][3].update(licence="unknown"), "may not go into the asset", "unknown in the asset")
    bad(lambda c: c["files"][0].update(dest="Projects/t-v16/../x"), "bad target path", "dot-dot")
    bad(lambda c: c["files"][0].update(dest="/etc/x"), "bad target path", "absolute path")
    bad(lambda c: c["files"][0].update(dest="Other/x"), "outside", "outside the roots")
    bad(lambda c: c["files"][0].update(sha256="ABC"), "sha256", "bad hash")
    bad(lambda c: c["files"][1].update(dest=c["files"][0]["dest"]), "used twice", "duplicate target")
    bad(lambda c: c["files"][3].update(licence="Maintainer-own"), "Maintainer-own", "Maintainer-own outside t-v16")
    bad(lambda c: c["files"][-1].pop("url"), "needs an https url", "no url")
    bad(lambda c: c["files"][-1].update(url="http://example.invalid/x"), "needs an https url", "http url")
    bad(lambda c: c.update(schema=2), "schema", "schema")
    # OS-CEPTFX S0: ptfx-sdk/ is a third root; a file outside the asset may live there, a bundled one may not
    bad(lambda c: c["files"].append(dict(dest="ptfx-sdk/x.h", sha256=sha(b"x"), size=1, licence="MIT", author="a", source="s", bundled=False)),
        "needs an https url", "SDK file without a url")
    bad(lambda c: c["files"].append(dict(dest="ptfx-sdk/x.h", sha256=sha(b"x"), size=1, licence="MIT", author="a", source="s", bundled=True,
                                         url="https://example.invalid/x")), "may not go into the asset", "bundled SDK file")
    bad(lambda c: c["files"][0].update(dest="ptfx-sdkx/y"), "outside", "near-miss root")

    with tempfile.TemporaryDirectory() as tmp:
        src_tv, src_o3 = os.path.join(tmp, "tv"), os.path.join(tmp, "o3")
        write_tree(src_tv, tv)
        write_tree(src_o3, {k: v[0] for k, v in o3.items()})

        # pack twice: the same bytes; sorted entries; one time stamp
        outs = []
        for n in (1, 2):
            out = os.path.join(tmp, "pack%d" % n)
            td.cmd_pack(m, src_tv, src_o3, out)
            with open(os.path.join(out, "t.zip"), "rb") as f:
                outs.append(f.read())
        assert outs[0] == outs[1], "the asset is not reproducible"
        with zipfile.ZipFile(io.BytesIO(outs[0])) as z:
            names = z.namelist()
            assert names[:2] == ["README.txt", "LICENSES/CREDITS.txt"] and names[2:] == sorted(names[2:]), names
            assert {i.date_time for i in z.infolist()} == {td.ZIP_TIME}
            assert "Projects/t-v16/project.mt" in names and "O3TEST/MOD/outside.mod" not in names

        # fetch it: every file equals its source; with the right hash in the manifest
        m["release"].update(sha256=sha(outs[0]), size=len(outs[0]))
        zpath = os.path.join(tmp, "pack1", "t.zip")
        dest = os.path.join(tmp, "dest")
        assert td.cmd_fetch(m, dest, zpath, False) == 0
        for rel, data in tv.items():
            with open(os.path.join(dest, "Projects", "t-v16", *rel.split("/")), "rb") as f:
                assert f.read() == data, rel
        for rel, (data, _) in o3.items():
            with open(os.path.join(dest, "O3TEST", *rel.split("/")), "rb") as f:
                assert f.read() == data, rel
        assert not os.path.exists(os.path.join(dest, "O3TEST", "MOD", "outside.mod"))

        # refusals
        wrong = copy.deepcopy(m)
        wrong["release"]["sha256"] = "1" * 64
        dest2 = os.path.join(tmp, "dest2")
        expect_error(lambda: td.cmd_fetch(wrong, dest2, zpath, False), "refusing to unpack", "asset with another hash")
        assert not os.path.exists(dest2), "something was written before the hash was checked"

        stray = io.BytesIO()
        with zipfile.ZipFile(stray, "w") as z:
            for name in names:
                z.writestr(name, zipfile.ZipFile(io.BytesIO(outs[0])).read(name))
            z.writestr("O3TEST/EXTRA/evil.bin", b"x")
        sp = os.path.join(tmp, "stray.zip")
        with open(sp, "wb") as f:
            f.write(stray.getvalue())
        sm = copy.deepcopy(m)
        sm["release"].update(sha256=sha(stray.getvalue()), size=len(stray.getvalue()))
        d3 = os.path.join(tmp, "dest3")
        expect_error(lambda: td.cmd_fetch(sm, d3, sp, False), "does not list", "stray member")
        assert not os.path.exists(d3), "something was written for a stray member"

        # a member whose bytes differ inside an asset that has the right hash: nothing is written
        def rebuilt(drop=None, change=None):
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w") as z:
                for name in names:
                    if name == drop:
                        continue
                    data = zipfile.ZipFile(io.BytesIO(outs[0])).read(name)
                    z.writestr(name, b"changed" if name == change else data)
            mm = copy.deepcopy(m)
            mm["release"].update(sha256=sha(buf.getvalue()), size=len(buf.getvalue()))
            path = os.path.join(tmp, "r%d.zip" % len(os.listdir(tmp)))
            with open(path, "wb") as f:
                f.write(buf.getvalue())
            return mm, path
        mm, path = rebuilt(change="Projects/t-v16/project.mt")
        d4 = os.path.join(tmp, "dest4")
        expect_error(lambda: td.cmd_fetch(mm, d4, path, False), "differs from the manifest", "changed member")
        assert not os.path.exists(d4), "something was written for a changed member"
        mm, path = rebuilt(drop="O3TEST/MOD/gen.mod")
        expect_error(lambda: td.cmd_fetch(mm, os.path.join(tmp, "dest5"), path, False), "lacks files", "missing member")

        # --third-party: a manual entry is listed and not downloaded; a failing download is counted (http_get replaced, no network)
        calls = []
        orig = td.http_get

        def fake_get(url):
            calls.append(url)
            if url == "https://example.invalid/sdk.h":
                return b"SDK"
            raise td.TestdataError("no network in the test")

        td.http_get = fake_get
        try:
            m3 = copy.deepcopy(m)
            m3["files"].append(dict(dest="O3TEST/WTABLE/by_hand.wav", sha256=sha(b"y"), size=1, licence="terms", author="a", source="s", bundled=False,
                                    url="https://example.invalid/page", manual=True))
            m3["files"].append(dict(dest="ptfx-sdk/tracker_plugin.h", sha256=sha(b"SDK"), size=3, licence="MIT", author="a", source="s", bundled=False,
                                    url="https://example.invalid/sdk.h"))
            td.validate_manifest(m3)
            assert td.cmd_fetch(m3, os.path.join(tmp, "dest6"), zpath, True) == 2
            assert calls == ["https://example.invalid/outside.mod", "https://example.invalid/sdk.h"], calls
            # a plain fetch downloads only the asset: no network at all, exit 0 (an offline `fetch --zip FILE` works)
            del calls[:]
            assert td.cmd_fetch(m3, os.path.join(tmp, "dest7"), zpath, False) == 0
            assert calls == [], calls
            assert not os.path.exists(os.path.join(tmp, "dest7", "ptfx-sdk"))
            # --only brings just that root's files, without the release asset (zip_file is not even read)
            del calls[:]
            assert td.cmd_fetch(m3, os.path.join(tmp, "dest8"), None, False, "ptfx-sdk/") == 0
            assert calls == ["https://example.invalid/sdk.h"], calls
            with open(os.path.join(tmp, "dest8", "ptfx-sdk", "tracker_plugin.h"), "rb") as f:
                assert f.read() == b"SDK"
            expect_error(lambda: td.cmd_fetch(m3, os.path.join(tmp, "dest9"), None, False, "nope/"), "matches no file", "--only without a match")
        finally:
            td.http_get = orig

        with open(os.path.join(src_tv, "project.mt"), "wb") as f:
            f.write(b"changed")
        expect_error(lambda: td.cmd_pack(m, src_tv, src_o3, os.path.join(tmp, "pack3")), "differs from the manifest", "changed source")

        expect_error(lambda: td.check_output_dir(os.path.join(ROOT, "tools", "x")), "build*/", "output inside the repository")
        td.check_output_dir(os.path.join(ROOT, "build-fixtures"))
    print("testdata_python: ok")


main()
