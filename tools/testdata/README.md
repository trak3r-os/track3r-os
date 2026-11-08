# tools/testdata: the test data kept out of git (OS-HILCARD)

The HIL scenarios under `platform/mk66/tools/hil/` name an OG project, `Projects/t-v16`, and the import tests use sample folders (`O3TEST/`). None of it is in git
(binaries, and some files have terms that do not allow redistribution). `testdata.py` brings it back, with every file pinned by SHA-256 in
`testdata_manifest.json` (standard library only, run it with `python3 -I`).

| Part | What it is | Where it lives |
| --- | --- | --- |
| `Projects/t-v16/` | An OG-format Workspace saved by firmware 1.9.0 (structure 16, 12 tracks, 16 instruments with samples, tempo 105), 46 files. The maintainer's own work, released by him under the licence of the repository, GPL-3.0-or-later (manifest label `Maintainer-own`; maintainer, 2026-10-23). | the release asset |
| `O3TEST/` generated, CC0, Public Domain, CC-BY 4.0 | 46 import-format files (8SVX, AIFF, IT, MED, MOD, S3M, WTABLE, XM). CC-BY credits are in the asset's `LICENSES/CREDITS.txt`. | the release asset |
| `O3TEST/` other terms | 32 files of unknown, share-alike, GPL or non-redistributable terms (FFmpeg, McGill, libxmp, OpenMPT, Surge XT, Echo Sound Works, one CC-BY-SA module). | fetched from the source by hash; never in the asset |
| `ptfx-sdk/` | The eight `editor-fx-sdk` files of `tracker-ce@7fd6c2c` (`tracker_plugin.h`, `ptfx_builder.py`, the six examples; MIT, LICENSE part 1, use expressly permitted by part 2 1(b)) — the S0 fixtures of OS-CEPTFX, built into `.ptfx` by `tools/ptfx_fixtures.py`. Never committed. | fetched from raw.githubusercontent.com by hash, with `fetch --only ptfx-sdk/` or `--third-party` — never by a plain `fetch` |

## Use

```
python3 -I tools/testdata/testdata.py fetch [--zip FILE] [--third-party]     # into build-fixtures/ (untracked)
python3 -I tools/testdata/testdata.py fetch --only ptfx-sdk/               # just the OS-CEPTFX SDK files, no release asset
python3 -I tools/testdata/testdata.py check-manifest                         # no network
```

`fetch` downloads the asset named by the manifest's `release` (tag `fixtures-1`), refuses it unless its SHA-256 and size match, checks every member
against the manifest and only then writes; a plain `fetch` touches nothing else, so an offline `fetch --zip FILE` needs no network. `--zip FILE` uses a local copy (the repository may be private, or the machine offline). `--third-party` also
downloads the files outside the asset, each checked by SHA-256; a failure is listed and the run exits with 2. The three Echo Sound Works wavetables have
a page, not a file: they are listed to be downloaded by hand. Copy `build-fixtures/Projects` and `build-fixtures/O3TEST` to the root of an SD card by hand
(any card, never a fixed disk).

## Changing the data (the maintainer's side)

1. Edit the data, list it in `testdata_manifest.json` (`dest`, `sha256`, `size`, `licence`, `author`, `source`, `bundled`; a file outside the asset also needs an `https` `url`).
   A bundled file may carry only `CC0-1.0`, `Public-Domain`, `CC-BY-4.0` or `Maintainer-own` (t-v16 only). Check the licence on the source page first. No Freesound packs.
2. `python3 -I tools/testdata/testdata.py pack --tv16 DIR --o3test DIR` builds `build-fixtures-pack/<asset>` (reproducible: sorted entries, one time stamp, Unix attributes; the same bytes need the same zlib, as for the starter zip) and prints the
   SHA-256 and size to write into `release`. A source file that differs from the manifest fails the run.
3. Upload the asset to a release with the tag of `release.tag` (a new tag, `fixtures-2`, for a new set; never replace an uploaded asset). That is a person's act, after the
   maintainer's go. The hash in the repository makes a replaced asset fail to unpack.

The ctest `testdata_python` runs the validator, the pack twice (byte-identical) and the unpack, with fake data and no network.

Checked on 2026-10-23, before the first upload: the licence line of the 24 Mod Archive pages of the 26 Public-Domain and CC-BY files is the one in the manifest, and the
Toisto AIFF suite states CC0 in its `LICENSE` and README. Not done: the generator of the 12 `gen-*` files (the scripts are lost; the files are CC0 and stay as bytes).
