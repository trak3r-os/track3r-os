# tools/release: the release kit and the starter-content zips (OS-STARTER, OS-RELGHA)

## The release flow (OS-RELGHA, maintainer 2026-11-18)

Push a tag `v<major>.<minor>.<patch>[-suffix]` and GitHub does the rest (`.github/workflows/release.yml`; there is no other GitHub workflow, CI is
`tools/ci.sh` run locally): two clean MK66 builds, `cmp`, the image checks, then `make_kit.sh`, then the release is PUBLISHED and marked Latest with
four files: `track3r-os-<tag>.ptf`, `track3r-os-<tag>-modules.zip`, `track3r-os-<tag>-examples.zip` and one `SHA256SUMS` over those three. The
body is `docs/release/<tag>.md` (relative links made absolute to the tag by `notes_links.py`), followed by the SHA-256 and CRC-32
table; a tag without that page fails the run at its start. Redo: Actions, release, Run workflow, type the tag: an existing release gets its files replaced (`--clobber`) and its notes
rewritten. Write `docs/release/<tag>.md` before the tag, and make sure the tag is on the commit you mean before pushing it: the release is public at once.

```
ARM_GNU_ROOT=<the Arm GNU toolchain> tools/release/make_kit.sh <tag> <outdir> [<the gated .ptf>]
```

`make_kit.sh` is what the workflow runs and what a person can run: it builds the modules (host build with the Arm compiler: the `o3m_<name>_tmod`
targets of the manifest and `o3_ptfx`), calls `make_starter.py --split`, and writes `<outdir>/SHA256SUMS` over the `.ptf` and the two zips (the
`.ptf` is `<outdir>/track3r-os-<tag>.ptf` or the third argument). It needs the network (pinned downloads, Mod Archive licence pages): a failed
fetch, a hash mismatch or a changed licence fails the run. `<outdir>` inside the repository must be below `build*/`. `O3OS_KIT_JOBS` (default 4) sets
the jobs, `O3OS_KIT_CACHE` the download cache.

`make_starter.py --split` writes the two zips instead of the one: the modules zip (`modules/`, `Effects/`, `LICENSES/`) and the examples zip
(`Samples/`, `WTABLE/`, `Songs/`, `Projects/`, `LICENSES/`), named after the `.ptf` (`track3r-os-<tag>-modules.zip`), each with its own `README.txt`
(`README_MODULES`, `README_EXAMPLES`) and a `SHA256SUMS` over all its other files. The `Firmware/` entries of the one-zip archive are not in either.
From the v1.4.0 inputs it gives the same entry lists as the published v1.4.0 zips, and the same bytes apart from the version string in `README.txt`
and `LICENSES/CREDITS.txt` (and so `SHA256SUMS`) when the version differs. The one-zip mode (below) is unchanged.

## The starter zip

`make_starter.py` builds `track3r-os-<version>.zip`, the archive that goes next to the bare `.ptf` of an alpha release. It is unpacked into the root
of the SD card. It never goes into git, and nothing here uploads or publishes it: a person does that after the maintainer's go.

```
python3 -I tools/release/make_starter.py --version 0.7.0 --ptf <track3r-os-0.7.0.ptf> --modules-dir <build dir>/modules [--out build-release]
```

- The output (default `build-release/`, untracked) also gets `<zip>.sha256` and a record of the Mod Archive licences that were re-read.
  The script refuses an output directory inside the repository unless it is below `build*/`.
- `--modules-dir` is the `modules/` folder of a build that has the `.tmod` files (the host build with the Arm compiler has them; to make only
  those, configure with `ARM_GNU_ROOT` set and build the `o3m_<name>_tmod` targets). `starter_manifest.json` lists what is packed, in
  `modules` (one list per kind, each file taken from `--modules-dir` as `<name>.tmod`) and `presets`. The script checks the kind in each
  file's header against the list it is in, so a module cannot land in a folder the firmware does not look in.
- Where the firmware looks for each kind (the zip puts the files there):

  | Kind | Manifest list | Card path | Firmware code |
  |---|---|---|---|
  | App | `modules.apps` | `modules/apps/<name>.tmod` | `module/src/loader.cpp` (`kPlayableDirs`), `app/src/module_ui.cpp` (`scan_apps`) |
  | Effect | `modules.effects` | `modules/effects/<name>.tmod` | `module/src/loader.cpp`, `app/src/module_ui.cpp` (`scan_effects`) |
  | Instrument | `modules.instruments` | `modules/instruments/<name>.tmod` | `module/src/loader.cpp`, `app/src/module_ui.cpp` (`scan_instruments`) |
  | Importer | `modules.importers` | `modules/importers/<name>.tmod` | `module/src/loader.cpp` (`kImporterDir`), `app/src/module_import.cpp` |
  | Preset pack | `presets` | `modules/presets/<id>/<preset>.o3mp` | `app/src/module_ui.cpp` (`preset_path`) |
  | `.ptfx` plugin | `ptfx` (a list of plugin names, `--ptfx-dir`) | `Effects/<name>.ptfx` | `module/include/o3os/module/ptfx_catalog.h`, `app/src/ptfx_page.cpp` |

- What is packed, and what is not:
  - Apps: every folder of `modules/apps` except `common` (a header library): chords, humanize, loudness, midimon, morph, mutate, pads (the SDK's
    example app), rhythmgen, scope, smfexport, tapcalc, tuner.
  - Effects: every folder of `modules/effects` (autopan, biquad, chorus, comp, crush, drive, envfilter, fold, phaser, pingpong, wide).
  - Instruments: `tb303`, `chip`, `fm`, `sid` and `sine` (the SDK's example voice, MIT). The four synths are not inside the firmware (OS-FLASHDIET-ROOM R3):
    the card is where a unit gets them, so a project that uses one without its file says the module is missing. Their licence texts are packed
    next to them (`modules/instruments/NOTICES.txt`: the notice, Open303's MIT text and tracker-ce's, as the image carried them before) and in
    `LICENSES/` (`Open303-MIT.txt`, `CHIP-tracker-ce-MIT.txt`); `CREDITS.txt` names them. Their factory presets are packed too (`presets`).
  - Importers: `tracker` (OS-FLASHDIET I3), the MOD / S3M / MED reader the firmware loads from the card on IMPORT, so the zip must carry it.
  - `.ptfx` plugins (OS-PTFXPACK): the nineteen folders of `modules/ptfx`, built by the CMake target `o3_ptfx` (`tools/ptfx_pack.py`) and copied to
    `Effects/`: the six `editor-fx-sdk` examples of polyend/tracker-ce (add_silence, analog_kick_gen, chiptune_gen, multimode_filter, stereo_delay,
    stereo_to_mono) and seven Sample Editor effects rebuilt from its `core/effector` (overdrive, delay, bitcrusher, chorus, flanger, limiter, eq3). Wavetable Smoother, Cut and Time-stretch are not
    rebuilt (OS-PTFXPACK P3): one block at a time, a plugin cannot read the whole selection (Smoother, Time-stretch), cannot ask for the project tempo, or produce an output length
    unrelated to the selection, and our host refuses a run that produces nothing (Cut, which deletes the range). The
    maintainer approved these components (CLAUDE.md, hard rule 1; `THIRD_PARTY.md`). They are MIT, which is allowed for this section only: each
    file is read back through the checks of `tools/ptfx_inspect.py`, its source must say `SPDX-License-Identifier: MIT`, the licence text goes to
    `LICENSES/MIT-tracker-ce.txt` and `CREDITS.txt` names the files. No `.ptfx` is committed.
    The other six folders are plugins of our own (maintainer 2026-11-12): the generators wt_forge, wt_notes, grain_lock and pad_forge and the effects wt_resynth and
    pad_stretch, written against the SDK header with no tracker-ce code or algorithm. They are MIT too, (c) the track3r-os contributors (`PTFX_OWN` of
    `make_starter.py`): `CREDITS.txt` lists them apart, under their own MIT text.
  - Not packed: the `modules/templates` examples (SDK scaffolding) and our own `.ptfx` test fixtures (`tests/ptfx`: fault, hang and ABI probes, an
    integer gain, a test ramp), which are not tools for a user. A plugin from another tracker-ce component is never packed without the
    maintainer's approval of that component.
  - `tests/starter_test.py` (ctest `starter_python`) holds the committed lists to `modules/`: a new effect or app that is not in the manifest, or
    a synth of `O3M_BUILTIN_STEMS` that is not packed (or whose licence text is missing), fails it.
- Third-party files are listed in `starter_manifest.json` and fetched into a cache (`OUT/cache`, named by SHA-256). A download whose SHA-256 differs
  fails the run. Allowed licences: `CC0-1.0`, `Public-Domain`, `CC-BY-4.0`, `GPL-3.0`; anything else is refused. A file with `modarchive_id` has its
  licence re-read on the module's Mod Archive page; a changed licence or an unreadable page fails the run.
- Our own content is generated by `gen/` (a drum kit, five wavetables, an OctaMED module, the two demo projects) and is CC0. It is deterministic:
  no libm call, no random module, no clock.
- The zip is reproducible: sorted entries, one time stamp, fixed attributes, deflate level 9 (same bytes for the same inputs and the same zlib).
- `--check-manifest` validates the manifest without a network. `--offline` uses the cache only (the ctest `starter_python` does, with fake files).

## The 1.2.0 archive

Build the modules once, make a `.ptf` through the image gate (`fw-gate-check`), then (the `.ptf` is the gated image of the release):

```
export ARM_GNU_ROOT=<the Arm GNU toolchain>            # docs/toolchain.md
cmake -S . -B build-release-mods && cmake --build build-release-mods -j4 --target <every o3m_<name>_tmod of the manifest> o3_ptfx
python3 -I tools/release/make_starter.py --version 1.2.0 --ptf <build-images>/track3r-os-1.2.0.ptf --modules-dir build-release-mods/modules \
    --ptfx-dir build-release-mods/modules/ptfx
```

`--ptfx-build build-release-mods` builds `o3_ptfx` itself before packing (and takes `--ptfx-dir` from it), and a `.ptfx` older than its source, the SDK header or `tools/ptfx_pack.py`
is refused as stale, so an old `--ptfx-dir` cannot ship. The ctest `ptfx_pack` builds every plugin twice and compares the bytes (the build is reproducible).

Add `--offline` when `build-release/cache` is already filled. The zip is `build-release/track3r-os-1.2.0.zip`.

## Adding a third-party file

1. Check its licence on the source page and use only a licence from the list above. No Freesound packs, nothing from `build-sdtest/`, no
   share-alike or unknown terms.
2. Pin the URL (a commit, not a branch), download it once, compute the SHA-256, and add one line to `files` with `dest`, `url`, `sha256`, `licence`,
   `author`, `title` and `source`. For CC-BY the author, title and source are the attribution that goes to `LICENSES/CREDITS.txt`.
3. `python3 -I tools/release/make_starter.py --check-manifest`, then a real run by hand.

The target folders are `Samples/`, `WTABLE/` and `Songs/` (`LICENSES/` is for licence texts only).
