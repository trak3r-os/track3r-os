#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The test data of the HIL and import scenarios, kept out of git (OS-HILCARD; standard library only; run it with `python3 -I`).

  testdata.py check-manifest
  testdata.py fetch [--dest build-fixtures] [--zip FILE] [--third-party] [--only PREFIX]
  testdata.py pack --tv16 DIR --o3test DIR [--out build-fixtures-pack]

The data is two things (testdata_manifest.json lists every file with SHA-256, size, licence, author and source):

  Projects/t-v16/   an OG-format Workspace saved by firmware 1.9.0 (structure 16, 12 tracks, 16 instruments, tempo 105): the project that the HIL
                    scenarios under platform/mk66/tools/hil/ name. The maintainer's own work, given to the project as test data.
  O3TEST/           one folder per import format (8SVX, AIFF, IT, MED, MOD, S3M, WTABLE, XM): the files that the format readers and the import
                    scenarios are tried on.
  ptfx-sdk/         the eight editor-fx-sdk files of tracker-ce@7fd6c2c (tracker_plugin.h, ptfx_builder.py, the six examples; MIT, LICENSE part 1,
                    use expressly permitted by part 2 1(b)): the S0 fixtures of OS-CEPTFX, fetched with `fetch --only ptfx-sdk/` (no release
                    asset, no other download) or with `--third-party`, never by a plain `fetch`. Never committed: build-fixtures/ is untracked.

`fetch` brings them into build-fixtures/ (untracked): the release asset named by the manifest ("release": tag, asset, url, SHA-256), checked
against its SHA-256 before anything is unpacked, then every member checked against the manifest. `--zip FILE` uses a local copy of the asset
(the repository may be private, or the machine offline). `--third-party` also downloads the files that are NOT in the asset (O3TEST entries
with a "url" and "bundled": false: unknown, share-alike or non-redistributable terms, and the ptfx-sdk/ entries of OS-CEPTFX S0), each one
checked against its SHA-256; a download that fails or differs is reported and the run exits with 2 (an entry marked "manual" has a page,
not a file: it is listed, to be downloaded by hand). `--only PREFIX` downloads just the non-bundled entries under PREFIX (e.g.
`--only ptfx-sdk/`: the SDK fixtures alone), without the release asset and without any other download. A plain `fetch` downloads only the
asset: an offline `fetch --zip FILE` does not touch the network. Copy build-fixtures/Projects and build-fixtures/O3TEST to the root of an SD
card by hand.

`pack` builds the asset from local copies (the maintainer's, after a change of the data): sorted entries, a fixed time stamp, fixed attributes,
the same bytes for the same inputs and the same zlib. It prints the SHA-256 and size to write into the manifest's "release". Only "bundled" files are packed, and
only with a licence of BUNDLE_LICENCES; a file whose hash differs from the manifest fails the run. Uploading the asset to a release is a person's
act, after the maintainer's go. Nothing is written inside the repository except below a build*/ directory.
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
import urllib.error
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
DEFAULT_MANIFEST = os.path.join(HERE, "testdata_manifest.json")
DEFAULT_DEST = os.path.join(ROOT, "build-fixtures")
DEFAULT_PACK_OUT = os.path.join(ROOT, "build-fixtures-pack")

ZIP_TIME = (2026, 1, 1, 0, 0, 0)             # fixed time stamp of every entry (the zip format starts in 1980)
USER_AGENT = "track3r-os-testdata/1.0 (+https://github.com/trak3r-os/track3r-os)"

# The only licences the asset may carry. "Maintainer-own" is the t-v16 Workspace, GPL-3.0-or-later like the repository (see README.md); "CC-BY-4.0" needs its credit in CREDITS.txt.
BUNDLE_LICENCES = ("CC0-1.0", "Public-Domain", "CC-BY-4.0", "Maintainer-own")
DEST_ROOTS = ("Projects/t-v16/", "O3TEST/", "ptfx-sdk/")
# A file outside the asset may live under O3TEST/ (fetched by `--third-party`) or under ptfx-sdk/ (the OS-CEPTFX S0 SDK fixtures,
# fetched by `--only ptfx-sdk/` or `--third-party`, never by a plain `fetch`).
NONBUNDLED_ROOTS = ("O3TEST/", "ptfx-sdk/")
REQUIRED = ("dest", "sha256", "size", "licence", "author", "source")


class TestdataError(Exception):
    pass


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


# ---- the manifest ---------------------------------------------------------------------------------------------------------

def load_manifest(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError) as e:
        raise TestdataError("cannot read the manifest %s: %s" % (path, e))


def _check_dest(dest, where):
    if not isinstance(dest, str) or not dest or dest.startswith("/") or "\\" in dest or ":" in dest:
        raise TestdataError("%s: bad target path %r" % (where, dest))
    if any(part in ("", ".", "..") for part in dest.split("/")):
        raise TestdataError("%s: bad target path %r" % (where, dest))
    if not dest.startswith(DEST_ROOTS):
        raise TestdataError("%s: target %r is outside %s" % (where, dest, ", ".join(DEST_ROOTS)))


def validate_manifest(m):
    """Raises TestdataError for the first problem: a missing field, a bad path or hash, a licence outside the allow-list for a bundled file."""
    if not isinstance(m, dict) or m.get("schema") != 1:
        raise TestdataError("manifest: schema must be 1")
    rel = m.get("release")
    if not isinstance(rel, dict):
        raise TestdataError("manifest: 'release' must be an object")
    for key in ("tag", "asset", "url", "sha256"):
        if not isinstance(rel.get(key), str) or not rel[key].strip():
            raise TestdataError("manifest release: missing or empty %r" % key)
    if not re.fullmatch(r"[0-9a-f]{64}", rel["sha256"]):
        raise TestdataError("manifest release: sha256 must be 64 lowercase hex digits")
    if not isinstance(rel.get("size"), int) or rel["size"] <= 0:
        raise TestdataError("manifest release: size must be a positive integer")
    if not rel["url"].startswith("https://") or "/" in rel["asset"]:
        raise TestdataError("manifest release: the URL must be https and the asset a bare file name")
    files = m.get("files")
    if not isinstance(files, list) or not files:
        raise TestdataError("manifest: 'files' must be a non-empty list")
    seen = set()
    for i, e in enumerate(files):
        where = "manifest files[%d]" % i
        if not isinstance(e, dict):
            raise TestdataError(where + ": not an object")
        for key in REQUIRED:
            if key == "size":
                if not isinstance(e.get(key), int) or e[key] < 0:
                    raise TestdataError("%s: bad size" % where)
            elif not isinstance(e.get(key), str) or not e[key].strip():
                raise TestdataError("%s: missing or empty %r" % (where, key))
        where = "%s (%s)" % (where, e["dest"])
        _check_dest(e["dest"], where)
        if e["dest"] in seen:
            raise TestdataError(where + ": the target path is used twice")
        seen.add(e["dest"])
        if not re.fullmatch(r"[0-9a-f]{64}", e["sha256"]):
            raise TestdataError(where + ": sha256 must be 64 lowercase hex digits")
        if not isinstance(e.get("bundled"), bool):
            raise TestdataError(where + ": 'bundled' must be true or false")
        if e["bundled"]:
            if e["licence"] not in BUNDLE_LICENCES:
                raise TestdataError("%s: licence %r may not go into the asset (allowed: %s)" % (where, e["licence"], ", ".join(BUNDLE_LICENCES)))
            if (e["licence"] == "Maintainer-own") != e["dest"].startswith("Projects/t-v16/"):
                raise TestdataError(where + ": 'Maintainer-own' is for Projects/t-v16/ and only for it")
        else:
            if not e["dest"].startswith(NONBUNDLED_ROOTS):
                raise TestdataError(where + ": a file outside the asset belongs to %s" % " or ".join(NONBUNDLED_ROOTS))
            if not isinstance(e.get("url"), str) or not e["url"].startswith("https://"):
                raise TestdataError(where + ": a file outside the asset needs an https url")
            if "manual" in e and not isinstance(e["manual"], bool):
                raise TestdataError(where + ": 'manual' must be true or false")


def bundled(m):
    return sorted((e for e in m["files"] if e["bundled"]), key=lambda e: e["dest"])


def readme_text(m):
    return (
        "track3r-os test data (%s)\n\n"
        "Projects/t-v16/   an OG-format Workspace saved by firmware 1.9.0 (structure 16, 12 tracks, 16 instruments, tempo 105), the\n"
        "                  maintainer's own work, released by him under the licence of the repository (GPL-3.0-or-later).\n"
        "O3TEST/           import-format test files (8SVX, AIFF, IT, MED, MOD, S3M, WTABLE, XM): generated ones, CC0 ones, Public Domain\n"
        "                  and CC-BY ones. Credits and licences: LICENSES/CREDITS.txt.\n\n"
        "Copy Projects/ and O3TEST/ to the root of an SD card by hand. Files of other terms are not here: see tools/testdata/README.md\n"
        "in the repository (testdata.py fetch --third-party downloads them from their sources).\n" % m["release"]["tag"])


def credits_text(m):
    lines = ["Credits and licences of the files in this archive (every entry: file | licence | author | source)", ""]
    for e in bundled(m):
        if e["licence"] == "Maintainer-own":
            continue
        lines.append("%s | %s | %s | %s" % (e["dest"], e["licence"], e["author"], e["source"]))
    lines += ["", "CC-BY-4.0: https://creativecommons.org/licenses/by/4.0/ (attribution: the author and source above).",
              "Public-Domain: the dedication of the author, as listed by The Mod Archive.",
              "CC0-1.0: https://creativecommons.org/publicdomain/zero/1.0/", ""]
    return "\n".join(lines)


# ---- output location ------------------------------------------------------------------------------------------------------

def check_output_dir(path):
    """Writes below the repository are allowed only under a build*/ directory (they are untracked)."""
    real = os.path.realpath(path)
    root = os.path.realpath(ROOT)
    if real == root or real.startswith(root + os.sep):
        first = os.path.relpath(real, root).split(os.sep)[0]
        if not first.startswith("build"):
            raise TestdataError("refusing to write inside the repository outside a build*/ directory: %s" % path)


# ---- pack -----------------------------------------------------------------------------------------------------------------

def source_path(entry, tv16, o3test):
    if entry["dest"].startswith("Projects/t-v16/"):
        return os.path.join(tv16, *entry["dest"][len("Projects/t-v16/"):].split("/"))
    return os.path.join(o3test, *entry["dest"][len("O3TEST/"):].split("/"))


def zip_add(z, name, data):
    info = zipfile.ZipInfo(name, ZIP_TIME)
    info.external_attr = 0o644 << 16
    info.create_system = 3                    # Unix, whatever the machine that packs
    info.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(info, data, compresslevel=9)


def cmd_pack(m, tv16, o3test, out):
    check_output_dir(out)
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, m["release"]["asset"])
    with zipfile.ZipFile(path, "w") as z:
        zip_add(z, "README.txt", readme_text(m).encode("utf-8"))
        zip_add(z, "LICENSES/CREDITS.txt", credits_text(m).encode("utf-8"))
        for e in bundled(m):
            src = source_path(e, tv16, o3test)
            try:
                with open(src, "rb") as f:
                    data = f.read()
            except OSError as ex:
                raise TestdataError("cannot read %s: %s" % (src, ex))
            if sha256_hex(data) != e["sha256"] or len(data) != e["size"]:
                raise TestdataError("%s differs from the manifest (SHA-256 or size)" % e["dest"])
            zip_add(z, e["dest"], data)
    with open(path, "rb") as f:
        data = f.read()
    digest, size = sha256_hex(data), len(data)
    with open(path + ".sha256", "w", encoding="utf-8") as f:
        f.write("%s  %s\n" % (digest, m["release"]["asset"]))
    print("packed %s: %d files, %d bytes, sha256 %s" % (path, len(bundled(m)), size, digest))
    if digest != m["release"]["sha256"] or size != m["release"]["size"]:
        print("the manifest's release says sha256 %s size %d: write the values above into it before uploading the asset" %
              (m["release"]["sha256"], m["release"]["size"]))
        return 1
    return 0


# ---- fetch ----------------------------------------------------------------------------------------------------------------

def http_get(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise TestdataError("cannot download %s: %s" % (url, e))


def write_file(dest_root, rel, data):
    target = os.path.join(dest_root, *rel.split("/"))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target + ".part", "wb") as f:
        f.write(data)
    os.replace(target + ".part", target)


def download_entries(m, dest, entries):
    """Downloads the given non-bundled entries into dest, each checked against its SHA-256; returns the number of failures.
    An entry marked "manual" has a page, not a file: it is listed, not downloaded."""
    failed = 0
    for e in sorted(entries, key=lambda e: e["dest"]):
        if e.get("manual"):
            print("by hand: %s from %s" % (e["dest"], e["url"]))
            continue
        target = os.path.join(dest, *e["dest"].split("/"))
        if os.path.isfile(target):
            with open(target, "rb") as f:
                if sha256_hex(f.read()) == e["sha256"]:
                    continue
        try:
            data = http_get(e["url"])
            if sha256_hex(data) != e["sha256"] or len(data) != e["size"]:
                raise TestdataError("the download differs from the manifest (SHA-256 or size)")
            write_file(dest, e["dest"], data)
        except TestdataError as ex:
            failed += 1
            print("FAILED %s: %s" % (e["dest"], ex))
    return failed


def cmd_fetch(m, dest, zip_file, third_party, only=None):
    check_output_dir(dest)
    if only is not None:
        # Just the files outside the asset under PREFIX (e.g. --only ptfx-sdk/): no release asset, no other download.
        entries = [e for e in m["files"] if not e["bundled"] and e["dest"].startswith(only)]
        if not entries:
            raise TestdataError("--only %r matches no file outside the asset" % only)
        failed = download_entries(m, dest, entries)
        print("fetched %d file(s) under %s into %s: %d failed" % (len(entries), only, dest, failed))
        return 2 if failed else 0
    rel = m["release"]
    if zip_file:
        try:
            with open(zip_file, "rb") as f:
                blob = f.read()
        except OSError as e:
            raise TestdataError("cannot read %s: %s" % (zip_file, e))
    else:
        blob = http_get(rel["url"])
    if sha256_hex(blob) != rel["sha256"] or len(blob) != rel["size"]:
        raise TestdataError("the asset differs from the manifest's release (sha256 %s, size %d): refusing to unpack it" % (sha256_hex(blob), len(blob)))
    expected = {e["dest"]: e for e in bundled(m)}
    extra = {"README.txt", "LICENSES/CREDITS.txt"}
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        names = set(z.namelist())
        stray = names - set(expected) - extra
        if stray:
            raise TestdataError("the asset holds files that the manifest does not list: %s" % ", ".join(sorted(stray)[:5]))
        missing = set(expected) - names
        if missing:
            raise TestdataError("the asset lacks files that the manifest lists: %s" % ", ".join(sorted(missing)[:5]))
        members = {name: z.read(name) for name in sorted(names)}
    for name, data in members.items():
        if name in expected and (sha256_hex(data) != expected[name]["sha256"] or len(data) != expected[name]["size"]):
            raise TestdataError("%s in the asset differs from the manifest" % name)
    for name, data in members.items():
        write_file(dest, name, data)
    print("unpacked %d files into %s" % (len(names), dest))
    # A plain fetch downloads only the asset; --third-party brings every file outside the asset.
    entries = [e for e in m["files"] if not e["bundled"]] if third_party else []
    failed = download_entries(m, dest, entries) if entries else 0
    if entries:
        print("third-party files: %d failed" % failed)
    return 2 if failed else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check-manifest")
    f = sub.add_parser("fetch")
    f.add_argument("--dest", default=DEFAULT_DEST)
    f.add_argument("--zip")
    f.add_argument("--third-party", action="store_true")
    f.add_argument("--only", help="download only the files outside the asset under this prefix (e.g. ptfx-sdk/), without the release asset")
    p = sub.add_parser("pack")
    p.add_argument("--tv16", required=True)
    p.add_argument("--o3test", required=True)
    p.add_argument("--out", default=DEFAULT_PACK_OUT)
    a = ap.parse_args(argv)
    try:
        m = load_manifest(a.manifest)
        validate_manifest(m)
        if a.cmd == "check-manifest":
            n = sum(1 for e in m["files"] if e["bundled"])
            print("manifest ok: %d files in the asset, %d fetched from their sources (--third-party, or --only PREFIX for one root)"
                  % (n, len(m["files"]) - n))
            return 0
        if a.cmd == "fetch":
            return cmd_fetch(m, a.dest, a.zip, a.third_party, a.only)
        return cmd_pack(m, a.tv16, a.o3test, a.out)
    except TestdataError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
