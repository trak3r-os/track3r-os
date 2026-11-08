# Third-party code in this repository

Vendored code keeps its own licence (ADR 0005). Of these, the Spleen and Terminus glyph data are linked into the mk66
libraries (as `ui`'s four face tables, 12.5 KB) and, since OS-007e, FatFs (`ff.c` and `ffunicode.c`, 6.8 KB of the image); everything
else, the Riedel EVE library included, is host-only.

| Component | Version | Files | Licence | Used by |
|---|---|---|---|---|
| doctest | 2.4.11 | `third_party/doctest/doctest.h`, `third_party/doctest/LICENSE.txt` | MIT, (c) 2016-2023 Viktor Kirilov | unit tests, host builds only |
| stb_image_write | v1.16 | `third_party/stb/stb_image_write.h`, `third_party/stb/LICENSE` | MIT or public domain (Unlicense), your choice; (c) 2017 Sean Barrett | PNG snapshots of the simulator (`platform/host`), host builds only |
| Spleen | 2.2.0 | `third_party/fonts/spleen/spleen-8x16.bdf`, `spleen-6x12.bdf`, `LICENSE`, `AUTHORS` | BSD-2-Clause, (c) 2018-2026 Frederic Cambus | the Label (8x16) and Micro (6x12) faces: `tools/font_convert.py` turns the BDFs into the `ui` glyph tables |
| Terminus | 4.49.1 | `third_party/fonts/terminus/ter-u20b.bdf`, `ter-u32b.bdf`, `OFL.TXT`, `AUTHORS` | SIL Open Font License 1.1, (c) 2020 Dimitar Toshkov Zhekov, Reserved Font Name "Terminus Font" | the Data (10x20) and Display (16x32) faces, bold weight; the 10x20 face is also what the pattern grid's hex-pair and note tiles are composed from at boot |
| FatFs | R0.15a (22 November 2024) | `third_party/fatfs/{ff.c,ff.h,ffunicode.c,diskio.h,LICENSE.txt}` byte-identical; `ffconf.h` is **ours** (six options changed, below); `CMakeLists.txt` ours | ChaN's licence: BSD-style, one condition (keep the copyright notice and the disclaimer in redistributed source), binary form needs nothing; (C) 2024 ChaN; compatible with GPL-3.0-or-later | the SD card's file system (`platform/mk66/storage`), read-only; compiled into the MK66 image and into the host and QEMU test builds |
| Riedel EmbeddedVideoEngine (FT800-FT813) | tag 5.0.9 (commit `26ecdbf58413e1c7ea3ca6eab338ecdb55e68e4c`, released 2025-08-30) | `third_party/eve-riedel/{LICENSE,EVE.h,EVE_commands.h,EVE_commands.c,EVE_config.h}` | MIT, (c) 2016-2024 Rudolph Riedel | the host test oracle of the FT812 display driver (`tests/eve_riedel_test.cpp`, built by `third_party/eve-riedel/CMakeLists.txt` for the host only); **not linked into any image** |
| Open303 | commit `52f8614966cd` (github.com/maddanio/open303, a mirror of the SourceForge project; last commit 2019-09-17) | `third_party/open303/` (38 files byte-identical, the DSP core: `rosic_Open303`, `rosic_TeeBeeFilter`, the filters, envelopes and wavetable classes, their headers and `License.txt`), `modules/tb303/LICENSE-Open303.txt` | MIT, (c) 2009 Robin Schmidt (www.rs-met.com) | (1) the double-precision ORACLE of the TB-303 module's float port: built for the host only (`third_party/open303/CMakeLists.txt`), never linked into an image; (2) the module `modules/tb303` is a reimplementation of its algorithms and constants (credit and licence in the file header of `tb303_dsp.h` and in `LICENSE-Open303.txt`), shipped as a `.tmod` file on the SD card **and, since OS-012d, as bytes inside the firmware image** (`modules/builtin`): the MIT notice (copyright line, permission text, disclaimer) is in the image as the text `kBuiltinNotices` (`modules/builtin/NOTICES.txt` + `LICENSE-Open303.txt`; since OS-FLASHDIET P1 a deflate stream that `diag mod notice` inflates and prints, the text unchanged) and the INFO page lists it among the licences |
| pt2-clone | commit `18954f23c3d0ebd8583e61b65fe4507ae8aff4bd` (github.com/8bitbubsy/pt2-clone, master on 2026-10-12) | `third_party/pt2-clone/{LICENSE,pt2_paula.c,pt2_paula.h,pt2_blep.c,pt2_blep.h,pt2_rcfilters.c,pt2_rcfilters.h}` byte-identical; `pt2_header.h`, `pt2_audio.h`, `CMakeLists.txt` are **ours** (stubs for the original's GUI headers); `pt2_period_table.inc` is lines 101-192 of `src/pt2_tables.c` verbatim | BSD-3-Clause, (c) 2010-2026 Olav Sorensen (LICENSE read in full, 3 clauses; the BLEP code is "by aciddose, written for this project" and so under the same notice) | (1) the ORACLE of the Paula model (`audio/paula.h`): built for the host only (`third_party/pt2-clone/CMakeLists.txt`), never linked into an image; (2) the DATA of `audio/src/paula_tables.cpp` (the 16 x 36 ProTracker period table and the 257-point minBLEP step) is in the firmware image, so its notice is reproduced in that file's header, as clause 2 asks of a binary |
| Polyend Tracker Community Edition, CHIP synth engine (OS-CECHIP C1) | commit `7fd6c2c3002a8f690b5341c486ba69606077b513` (github.com/polyend/tracker-ce, 2026-11-04) | `third_party/tracker-ce-chip/{ChiptuneSynthVoice.cpp,LICENSE}` byte-identical (SHA-256 in `PROVENANCE.md`; ctest `cechip_oracle_sha`); `CMakeLists.txt`, `check_sha256.cmake`, `PROVENANCE.md` and the shim `tests/support/cechip_shim/` and wrapper `tests/support/cechip_ref.*` are **ours** | MIT with a no-commercial-clone clause for the complete application (upstream `LICENSE` Parts 1 and 2), (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors; the maintainer approved this component 2026-11-05 | (1) the ORACLE of the CHIP waves that `modules/chip` gains (OS-CECHIP): the vendored files are built for the host only (`third_party/tracker-ce-chip/CMakeLists.txt`), never linked into an image; (2) since slice C2 the module `modules/chip` carries a PORT of the engine (`modules/chip/chip_ce.h`: waves 4..11: the pulse family and the chord tables from C2, SID, noise, Super Square, FM Bell and Chip Drum from C4 and C5), shipped as bytes inside the firmware image (`modules/builtin`): the MIT notice, with Part 2's conditions, is `modules/chip/LICENSE-tracker-ce.txt` in `kBuiltinNotices` and the header of `chip_ce.h`; no vendored file and no `synth/lib/*.a` is in the image |
| Polyend Tracker Community Edition, `.ptfx` SDK and six example plugins (OS-PTFXPACK P1) | commit `7fd6c2c3002a8f690b5341c486ba69606077b513` (github.com/polyend/tracker-ce, 2026-11-04) | `modules/ptfx/sdk/tracker_plugin.h` and `modules/ptfx/{add_silence,analog_kick_gen,chiptune_gen,multimode_filter,stereo_delay,stereo_to_mono}/<name>.c` (from `editor-fx-sdk/` and `editor-fx-sdk/effects/` at that commit), `modules/ptfx/LICENSE-tracker-ce.txt`; **our** change is one notice block at the top of each file (SPDX line, provenance, the MIT text, the pointer to Part 2), the file after the line `---- end of the notice` is byte-identical (SHA-256 in `tools/testdata/testdata_manifest.json`, checked by ctest `ptfx_pack`); `modules/ptfx/CMakeLists.txt`, `tools/ptfx_pack.py` and `tests/ptfx_pack_test.py` are ours | MIT with a no-commercial-clone clause for the complete application (upstream `LICENSE` Parts 1 and 2; Part 2 1(b) expressly permits `.ptfx` plugins built with the SDK), (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors; the maintainer approved these six plugins 2026-11-08 | built as `.ptfx` files for the SD card's `Effects/` folder by the `o3_ptfx` target (host build, Arm toolchain), copied into the SD content zip by `tools/release/make_starter.py`; no `.ptfx` is committed and none is linked into any firmware image |
| Polyend Tracker Community Edition, six Sample Editor effects rebuilt as `.ptfx` (OS-PTFXPACK P2) | commit `7fd6c2c3002a8f690b5341c486ba69606077b513` (github.com/polyend/tracker-ce, 2026-11-04) | the algorithms of `core/effector/mtEffect{Overdrive,Delay,Bitcrusher,Chorus,Flanger,Limiter}.cpp` (parameter ranges: `mtSampleEditorEngine.cpp`), written anew in C as `modules/ptfx/{overdrive,delay,bitcrusher,chorus,flanger,limiter}/<name>.c` against the SDK header; each file's notice lists what is the original's and what is changed | MIT (upstream `LICENSE` Part 1 covers `core/`; Part 2 conditions apply, see the P1 row), (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors for the algorithms, (c) 2026 the track3r-os contributors for the port; the maintainer approved these components 2026-11-08 | built as `.ptfx` files for the SD card's `Effects/` folder like the P1 plugins; no `.ptfx` is committed and none is linked into any firmware image |
| Polyend Tracker Community Edition, three-band EQ rebuilt as `.ptfx` (OS-PTFXPACK P3) | commit `7fd6c2c3002a8f690b5341c486ba69606077b513` (github.com/polyend/tracker-ce, 2026-11-04) | the algorithm of `core/effector/mtEffectEQ.cpp/.h` (parameter ranges: `mtSampleEditorEngine.cpp`, `eqParams`), written anew in C as `modules/ptfx/eq3/eq3.c` against the SDK header; the file's notice lists what is the original's and what is changed | MIT (upstream `LICENSE` Part 1 covers `core/`; Part 2 conditions apply, see the P1 row), (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors for the algorithm, (c) 2026 the track3r-os contributors for the port; the maintainer approved this component 2026-11-08 | built as a `.ptfx` file for the SD card's `Effects/` folder like the P1 and P2 plugins; no `.ptfx` is committed and none is linked into any firmware image |

Built-in modules (OS-012d): the image also carries the `.tmod` files of `modules/chip` and `modules/fm`, this project's own code (MIT, Copyright (c) 2026 the track3r-os authors, the module SDK's terms), except
that Chip (OS-CECHIP C2) is no longer free of third-party code: its waves 4..11 are a port of tracker-ce's CHIP engine (`modules/chip/chip_ce.h`, MIT plus tracker-ce's notice and Part 2's conditions, below). Their
notice is the first paragraph of `kBuiltinNotices` (deflated in the image, printed by `diag mod notice`), which also carries `modules/chip/LICENSE-tracker-ce.txt`. All three are distributable inside the GPL-3.0-or-later image (MIT is compatible); each stays a separate
`.tmod` that the loader reads, not a part linked into the core.

Project-owned assets (not third-party): `ui/assets/boot_logo.png`, the boot logo (also the logo at the top of the README), was made by the maintainer for track3r-os and is licensed GPL-3.0-or-later like the rest of the repository.

doctest provenance: `https://github.com/doctest/doctest/releases/download/v2.4.11/doctest.h`, byte-identical to
`https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h` (fetched 2026-10-10, unmodified).

- `doctest.h` SHA-256 `44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb`
- `LICENSE.txt` SHA-256 `0fe0b331fa1513dcce8604ff1fa925f32d1cea17d8aeb1c2471fad40d291adc5`

Referenced, not vendored: the Bridgetek *FT81X Series Programmers Guide* v1.2 (BRT_000031), the source of the
display-list encodings in `eve/include/o3os/eve/dl.h`.

stb_image_write provenance: `https://raw.githubusercontent.com/nothings/stb/1ee679ca2ef753a528db5ba6801e1067b40481b8/stb_image_write.h`
(and `.../LICENSE`), the last commit that touched the file in the official repository `https://github.com/nothings/stb`
(2021-07-12, "update version numbers"; fetched 2026-10-10, unmodified).

- `stb_image_write.h` SHA-256 `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05`
- `LICENSE` SHA-256 `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63`

Spleen provenance: the release tarball `https://github.com/fcambus/spleen/releases/download/2.2.0/spleen-2.2.0.tar.gz`
of the official repository `https://github.com/fcambus/spleen`, fetched 2026-10-10 into an empty scratch directory;
its SHA-256 `ec42925c6b56d2138c862b2f97147c872e472f674bf03423417d827a08d69a89` is the digest GitHub publishes for that
release asset. Only the files below were taken from it, byte-identical; the licence text is the project's own and
travels with the data. Redistribution in binary form (a firmware image containing the glyph tables) must reproduce
that licence text in its documentation.

- `spleen-8x16.bdf` SHA-256 `4a3d97ee61a8c86a7525d8c723cb8a14081f395cd2feb4227ba5e3baf0629bae`
- `spleen-6x12.bdf` SHA-256 `fc0743d164690f99b7e2e1b9d503180e4c719a9831ae03fd8f6da18c857dee27`
- `LICENSE` SHA-256 `f33fe8679d5b2abecc4f1313ce6c6bfa58262964de5f7bca146596a7318047af`
- `AUTHORS` SHA-256 `be3bbf507efb0befa8d464289467f081238adf9e25dd4a7e7b959fdea9a006f8`

Terminus provenance (OS-003b): the release tarball `terminus-font-4.49.1.tar.gz`, the "Unix/Linux source" download linked
from the project's own page `http://terminus-font.sourceforge.net/`
(`https://sourceforge.net/projects/terminus-font/files/terminus-font-4.49/terminus-font-4.49.1.tar.gz/download`, which
redirects to a SourceForge mirror), fetched 2026-10-11 into an empty scratch directory. Its SHA-256 is
`d961c1b781627bf417f9b340693d64fc219e0113ad3a3af1a3424c7aa373ef79`; its MD5, `1b6acbd221957e33c8a792ebfaf3a659`, is the
digest SourceForge publishes for that file (the project's RSS listing). Only the files below were taken from it,
byte-identical (checked with `cmp` against the extracted tarball); the licence text is the project's own `OFL.TXT`, read
in full: SIL OFL 1.1 of 26 February 2007 with the Terminus copyright line and the Reserved Font Name.

- `ter-u20b.bdf` SHA-256 `3051598391e5f7f59f09dc5aa26a97700fcbbe57ac315495878c52fc74aba500`
- `ter-u32b.bdf` SHA-256 `f6295e92ebbf95afbb6274f4621608d255c1287ec90d3fefbc50f077bf09dab4`
- `OFL.TXT` SHA-256 `c14f8d795784a547ea35e69c51dee2957bb71a1cdb492ec5321e4b61d3d97630`
- `AUTHORS` SHA-256 `7a5e955baaf28951cf5d75d8f13b48273b02a9569d25210d6486ba8656f8af27`

What the OFL asks, and what is done about it: the Font Software may be bundled and redistributed with software provided
each copy carries the copyright notice and the licence, and may not be sold by itself: `OFL.TXT` travels with the BDFs,
and a firmware image that contains the glyph tables must reproduce it in its documentation (as for Spleen above). The
bitmaps the build compiles are a Modified Version in the licence's sense (a format conversion), so they must not use the
Reserved Font Name: the compiled faces are named by cell size (`f10x20`, `f16x32`) and the generated source says so.
Not taken from the tarball: the build tools and the Windows files, some of which are GPL-2.0-or-later (`configure`,
`bdftopsf.pl`, `ucstoany.pl`, `fcp.c`, `terminus.nsi`), and the sizes and the normal weight the UI does not use (the
normal 10x20 and 16x32 were tried and judged too light for a 135 ppi panel read at arm's length).

ADR 0001 forbids OctaMED/Amiga/Renoise assets in the repo: Spleen and Terminus are independent open fonts, chosen for
their licences and their cell sizes, not for imitating any of them. The icon face is drawn in-house (data in
`tools/icon_convert.py`, GPL-3.0-or-later like the rest of the tree); no third-party icon set is used.

OS-003d (`ui/plot`) vendors nothing. The spectrum's FFT is written in `ui/plot/src/fft.cpp` (radix-2 on n/2 complex points plus the
real-signal split, twiddles generated at init) behind the interface and output layout of CMSIS-DSP's `arm_rfft_fast_f32`
(Apache-2.0, Arm Limited), so that the MK66 build can call that routine instead without touching a caller. CMSIS-DSP was not
vendored because the host tests must run the code that runs on the device, and its tables for 256 and 512 points are about 6 KB
of flash that the generated twiddles are not. The tests check the FFT against a DFT in double precision; no CMSIS source or table
is copied, and the layout is the interface's documented one. The biquad response formula (`ui/plot/plot_math.h`) is the standard
identity of z-transform magnitude in `sin^2(w/2)`; the RBJ audio-EQ-cookbook coefficients that the tests and the gallery build for
their own filters are not part of the library.

## RBJ Audio EQ Cookbook, as formulas (OS-INSFX S8)

The Biquad Filter effect module (`modules/effects/biquad/`) uses the low-pass, high-pass, band-pass (constant 0 dB peak) and notch coefficient formulas of the Audio EQ Cookbook (Robert Bristow-Johnson, a published note of formulas; no licence is stated for the note and it is widely re-implemented). Only the formulas are used, written in the module's own half-angle form (`1 - cos w = 2 sin^2(w/2)`) with its own smoothing, mix folding and tail rule. No code of the note or of any library is copied or vendored, and the module's test checks it against a double-precision biquad written from the same formulas.

## Riedel EmbeddedVideoEngine (OS-007c)

What it is and how it is used: Rudolph Riedel's FT800-FT813 C library for the FT81x / BT81x display controllers
(`https://github.com/RudolphRiedel/FT800-FT813`, MIT). ADR 0011 amendment 1 and the radar card `eve-driver-libraries` chose it as
the model for the MK66 display driver, **adapted, not copied**: the driver in `platform/mk66/display/` is written fresh from the
FT81X Programmers Guide, follows this library's order of operations (`EVE_init()`), its bounded REG_ID / REG_CPURESET polls and its
coprocessor fault recovery, and says so in `chip.h` (with the notice below). The vendored files are the library at a pinned release
and are used **only as an oracle in the host tests**: its register and command definitions are compared with the driver's, its
`EVE_init()`, `EVE_busy()` fault recovery and command-FIFO burst are run against the same FT812 model as the driver's own code, and
every difference is listed and asserted in `tests/eve_riedel_test.cpp`. No file of it is compiled into an image, so no notice has to
travel with a firmware binary; the `LICENSE` file travels with the source.

Provenance: the tagged release `5.0.9` of the official repository (GitHub release of 2025-08-30; the radar card of 2026-10-11 said
the repository had no tags: it has tags `5.0.2` .. `5.0.9`, the newest being the one taken), fetched 2026-10-11 as `https://api.github.com/repos/RudolphRiedel/FT800-FT813/tarball/refs/tags/5.0.9`
into an empty scratch directory, listed with `tar -t` before extraction (no absolute or `..` paths), extracted with
`--no-same-owner`, nothing of it executed. The tarball's SHA-256 is
`5c05851442783005bb6e0432a1db5ed6e59ae24bde74294fc6ef7fc0d73d46f9` (GitHub generates the archive on request, so the **commit** SHA above
is the stable pin, the tarball hash only identifies the download that was read). Only the files below were taken, byte-identical
(checked with `cmp` against the extracted tree); the licence text was read in full: MIT, "Copyright (c) 2016-2024 Rudolph Riedel".

- `LICENSE` SHA-256 `862e06968462faf8ff2066fffaf7c4522aa48ddafd11fe9c578248aa4b013729`
- `EVE.h` SHA-256 `1d3d8d07c5479701b89e1056ab20667551a3258a97593e901ca946ba911d51eb`
- `EVE_commands.h` SHA-256 `7e091701a5e039e548c2872f468836efe254564148734c7b01ecca186a835c8a`
- `EVE_commands.c` SHA-256 `7553c1e56aebbaaee322964c6e58affcbc440ac4317d640257e4e96dc5481ee7`
- `EVE_config.h` SHA-256 `25579425d5531e3f88ac7750ff07a9af1bea19cf0008618f8724d87aff061e8d`

Not taken: the per-MCU target headers and `EVE_target.[ch]` (the oracle's port is `tests/support/eve_riedel_port/EVE_target.h`, first
party), `EVE_supplemental.*`, the C++ wrappers, the examples and pictures. The delay after ACTIVE is 40 ms in this
tag (`EVE_init()`); the radar card reports that a later commit raised it to 60 ms after a user report, and the driver waits 60 ms
regardless.

The MIT notice the driver's `chip.h` / `chip.cpp` follow in spirit (the order of operations is his; the code is not): "Copyright (c)
2016-2024 Rudolph Riedel. Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction ... The above copyright notice and this
permission notice shall be included in all copies or substantial portions of the Software." (full text: `third_party/eve-riedel/LICENSE`).

Not vendored, reference only (ADR 0011 amendment 1; no code, text or headers of it are in this repository): Bridgetek EVE-MCU-Dev (its
source headers point to a proprietary licence with a field-of-use limit and no derivative works). The board facts (pins, panel timing,
clocks and delays) are noted where they are used (`board.h`, `dspi_config.h`, `panel.h`).

## OctaMED references (OS-008a)

OS-008a (`formats/med`, the OctaMED / MED reader, tempo conversion and flow linearisation) vendors nothing and ports nothing.
Its sources are `docs/formats/med.md` section 9: Kinnunen's MMD0-MMD3 layout document (a format reference, no text copied) and
two readers that were **read as references only**: libxmp (MIT, "Copyright (C) 1996-2026 Claudio Matsuoka and Hipolito Carraro
Jr") and OpenMPT (BSD-3-Clause, "Copyright (c) 2004-2026, OpenMPT Project Developers and Contributors"). No file, function or
table of either is copied, translated or compiled in, so neither licence's notice requirement applies and none is claimed to
be honoured here; this paragraph is attribution of where the facts came from. What was read: the ten 8-channel tempos (179, 164,
152, 141, 131, 123, 116, 110, 104, 99), the SoundTracker-compatible tempo constant (293.7 ticks per second per tempo step,
also from the numbers of the Haskell med-module, no code) and the mixing tempo 157.86, all numbers that two or more independent
readers agree on, and the behaviour of the pattern-loop command (a start line and a repeat count per track, the ProTracker
rule), which `med_flow.cpp` implements from that description in its own code. The tests compare the tempo conversion with the
formulas of libxmp and OpenMPT (0.66 / tempo seconds per tick, which the CIA constants of the spec differ from by 1.3 %); the
frame-by-frame libxmp oracle of med.md section 5 was not built: libxmp is not installed here and vendoring it would pull in
about 60 KB of loaders for a test that only pins numbers the formula tests already pin. The generated test files
(`tools/mmd_build.py`, `tests/support/mmd_fixture.h`) are made up and contain no part of any real module.

## MOD and S3M references (OS-MODIMP)

OS-MODIMP (`formats/mod`, the ProTracker MOD and Scream Tracker 3 S3M importers) vendors nothing and ports nothing. Its sources are the public layouts of the two formats
(`docs/formats/mod.md` sections 2 to 4: ProTracker's 31-sample header and period table, Scream Tracker 3's header, parapointers and packed patterns; no text copied) and the behaviour
of libopenmpt 0.7.3 (BSD-3-Clause, "Copyright (c) 2004-2026, OpenMPT Project Developers and Contributors"), which the tests MEASURE and the code does not copy: libopenmpt is loaded
from the system by `tests/modimp_openmpt_oracle.py` (ctypes, nothing vendored, skipped without it, failing under `O3_CI=1`) and what it does with generated modules (note numbers, effect
text, rows visited, durations, rendered pitch) fixes the rules of `mod/flow.h` and `mod/map.h`. No file, function or table of OpenMPT or libxmp is copied, translated or compiled in.
The generated test modules (`tools/mod_build.py`, `tests/support/modimp_fixture.h`) are made up and contain no part of any real module; none is committed.

## USB device driver references (OS-006b)

OS-006b (`usb/` device logic, `platform/mk66/usb` driver, the `diag` console) vendors nothing and ports nothing. The USB0 register sequences are written from the K66 Sub-Family Reference
Manual's USB OTG FS, SIM and SYSMPU chapters, with NXP's MK66F18 device header and the Teensy core's `kinetis.h` as fact tables for addresses and
field positions (numbers, not code). **Our driver's own values; no line copied or translated** (the Teensy core's USB driver is PJRC's, MIT-style
licence with the extra target-device clause; ADR 0012 Decision 5 explains why nothing of it enters this repository): the order of the bring-up, the 180 MHz clock choice (IRC48M with clock recovery),
the endpoint 0 stall and re-arm behaviour, and the ENDPT register values (0x15 / 0x1D) are this driver's own. TinyUSB (MIT) was not consulted for code; its conformance behaviour (a ZLP when a reply is a
multiple of the packet size and shorter than wLength, a full-speed device answering the device-qualifier request with a stall) is USB 2.0 chapter 9 and is cited there. No file of the original firmware is
compiled in or carries weight in the licence of this tree.

## FatFs (OS-007e)

What it is and how it is used: Elm-ChaN's generic FAT file system module, "FatFs", used here from its upstream archive. In this repository it
is the read-only file system of the SD card (`platform/mk66/storage/fat_storage.cpp`, the `hal::storage::Storage` over it). It is configured read-only
(`FF_FS_READONLY 1`), so ChaN's `f_write`, `f_mkdir`, `f_unlink`... are not compiled, and the glue defines no `disk_write` (a reference to one would not
link).

Provenance: `http://elm-chan.org/fsw/ff/arc/ff15a.zip`, fetched 2026-10-11 into an empty scratch directory, listed with `unzip -l` before
extraction (no absolute or `..` paths), nothing of it executed, and the scratch directory deleted. The archive's SHA-256 is
`74737b1cafa1a67a3f722dd0d1a44767c5b54d37b6300ad3825a904cbe88fc3c`; the files taken from its `source/` directory are byte-identical (CRLF line
endings as upstream ships them):

- `ff.c` SHA-256 `2bed98e3f7708e4520cfcf69a855156458543ec9ea592d35630c501c2fd73e5f`
- `ff.h` SHA-256 `432bde036a1570d16de4716f9daf96fdeb04fb45726ca8560108222b625ee7a5`
- `ffunicode.c` SHA-256 `3f1632c1f1e64b0b683b0d7e51341b9870728f398604869dd8211f1819509a8e` (1.9 MB of source, 0.6 MB compressed in git: every code page's
  tables; the build compiles only CP437's, 1.2 KB)
- `diskio.h` SHA-256 `43b63bc5d1c35b2aa4a57c4e1cd7b66ab318748b51e9c6b12aded2574896e73c`
- `LICENSE.txt` SHA-256 `2576183a3e1c2d2f4b3628a7ef93c2f937879698abdd6d554dd718e79062e079`, the licence page of the archive
- `ffconf.h`: the archive's template has SHA-256 `59cac4aed1d07bca3dcffc3f2fd49dc3bc090cb5fcde8ed81ce82a5ffea819c0`; the file in the tree is the OS's
  configuration, that template with these changes (its own header lists them): `FF_FS_READONLY` 0 to 1, `FF_CODE_PAGE` 932 to 437, `FF_USE_LFN` 0 to 1
  (the static working buffer: no heap), `FF_LFN_UNICODE` 0 to 2 (UTF-8 at the API), `FF_FS_NORTC` 0 to 1, `FF_FS_EXFAT` 0 to 1 (+1,480 B of code, +608 B of
  `.bss`, measured; the 64-bit division it needs is already in the image). Not taken: `diskio.c` and `ffsystem.c` (examples; the glue and the
  system functions are not needed: no re-entrancy, no dynamic allocation), `00history.txt`, `00readme.txt`, the documents.

Why R0.15a and not the newer ones: R0.15b (June 2025) is the next release and **R0.16's history lists "Fixed f_readdir cannot detect end of directory
and it leads the application process into infinite loop. (appeared at R0.15b)"**: a bug in the one call the project loader and `ls` live on, so R0.15b is
out. R0.15a (the last of the R0.15 line the maintainer asked for without that bug) is vendored; R0.16 (22 July 2025, which also has that fix) is the upgrade
path and is a drop-in replacement for these five files (the configuration's revision ID `FFCONF_DEF` changes with it). The R0.15a defect that R0.15b
fixed (FatFs failed to load the FAT32 FSInfo sector, so `f_getfree` always scans the whole FAT) is in a call this tree never makes. Independently of
upstream's fixes, `FatStorage::readdir` stops a listing after 65,536 entries (the FAT directory limit), so a file system that never ends one cannot
hold the main loop.

The licence, read in full: ChaN's "FatFs License": the copyright notice and the disclaimer travel with redistributed source (they head every file
above and `LICENSE.txt` is kept); redistributing in binary form (an image) needs nothing; it says FatFs is compatible with the GNU GPL and with most
open source licences, which is what this tree's GPL-3.0-or-later needs.

**Warning relaxation, third-party code only** (the rule of stb_image_write above): the `o3os_fatfs` target (`third_party/fatfs/CMakeLists.txt`) is
built with `-w`, not the first-party `-Wall -Wextra -Wshadow -Wconversion -Werror`, because the code is upstream's and a vendored file stays
byte-identical to its archive. `CMakeLists.txt` builds C for this one library (`project(... LANGUAGES C CXX)`), which is why `-fno-rtti` and
`-fno-threadsafe-statics` became C++-only flags in the top-level file (gcc warns about them on C). What holds the library's behaviour instead of the
warnings are the tests that read FAT32 and exFAT volumes through it (`platform/mk66/tests/mk66_storage_fat_test.cpp`, host and QEMU, and
`tests/og_folder_fat_test.cpp`), under ASan and UBSan on the host.

## SD card references (OS-007e)

`platform/mk66/storage` (the SD protocol, the SDHC driver, `diag sd` and `ls`) vendors nothing else. The SD command set, the response and register
formats (OCR, CID, CSD versions 1 and 2, the CMD6 switch status) are written from the SD Physical Layer Simplified Specification's tables (facts, no text
or code copied); the SDHC register addresses and field positions are the K66 Sub-Family Reference Manual's, cross-checked against NXP's MK66F18 device
header and the Teensy core's `kinetis.h`, as for every other driver here. **Our driver's own values; no line copied or translated**: the pins (PTE0-5 at
ALT4), the card-detect switch (PTD0), the 180 MHz source clock, 400 kHz identification, 4-bit bus, the order of the init commands and the high-speed step
(which `SdhcBus` and `sd::Card` implement, with their own code). No file of the original firmware is compiled in or carries weight in the licence of this tree.

## Teensy Audio library: the filter oracle (OS-007l)

The instrument filter (`audio/include/o3os/audio/svf.h`) is **our own single-precision float code**: it follows the structure of the Teensy Audio library's state variable filter
(`AudioFilterStateVariable`, a Chamberlin filter at twice the sample rate, musicdsp.org archive 92) and copies none of its
lines. What does carry upstream code is the **host and QEMU test oracle**, `tests/audio_svf_test.cpp`: a port of the fixed-point loop of
`AudioFilterStateVariable::update_variable` (the `MULT` and `signed_saturate_rshift` of its `utility/dspinst.h`, written out for the host), used only to check the
float filter against it. It is not linked into any image. The oracle is the library's stock class and takes nothing from any vendor addition to it: it builds the
frequency from the formula 20 Hz x 2^(i / 50), which the test also holds the filter's coefficient to.

Audio Library for Teensy 3.X, Copyright (c) 2014, Paul Stoffregen, paul@pjrc.com. MIT licence:

> Development of this audio library was funded by PJRC.COM, LLC by sales of Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop open source
> software by purchasing Teensy or other PJRC products.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the
> Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
> Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice, development funding notice, and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
> PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
> OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Open303 provenance (OS-012a, the dsp-port skill's licence gate): `https://github.com/maddanio/open303` cloned (depth 1) into an empty scratch directory on
2026-10-12, treated as untrusted data (no build script was run), commit `52f8614966cd` (2019-09-17). `License.txt` (SHA-256
`375931a5a91cb4e96137f0c5c7b95f041576f33838f581019320d1559bfde064`) is the MIT licence, "Copyright (c) 2009 Robin Schmidt (www.rs-met.com)", the text the radar card
(radar card `tb303-engine`) found identical in the SourceForge SVN r1. The source files carry no licence line of their own: the MIT claim rests on
that one file (the card's risk 7), and the notice travels with every use. Not taken: `fft4g.c` (a third-party FFT, no notice in its first lines), the VST wrapper
(`Source/VSTPlugIn`, needs the Steinberg SDK), `rosic_FourierTransformerRadix2.cpp`, which includes it (the two members the wavetable generator calls are
`tests/support/open303_fft_stub.cpp`, ours). Everything under `third_party/open303/` except `CMakeLists.txt` is byte-identical to that commit (CRLF line ends kept).
JC-303 and its GPLv3 additions (diode ladder, overdrive, sequencer) were not looked at.

Written from scratch, no third-party code or data (OS-012c): the SD modules `modules/chip` (a chip-tune voice) and `modules/fm` (a four-operator FM voice). Licence gate of the dsp-port skill: nothing was ported, so nothing is
vendored and no oracle is third-party. The FM engine is **not** derived from Google's MSFA / Dexed (Apache-2.0 core, GPL-3 application): none of that code was read or ported; the engine is the textbook maths of phase modulation
(Chowning, 1973) with this module's own eight algorithm tables, a polynomial sine fitted here, and an envelope model of its own. Its oracle (`modules/fm/tests/fm_test.cpp`) is an independent double-precision model written in the test and
the Bessel functions of the first kind. The Chip's waveforms (pulse duty cycles, a 15-bit LFSR with two taps, triangle) are descriptions of how such chips work, not code. Both modules' sources are MIT like the SDK (SPDX headers); their tests, which use the OS's loader and draw recorder, are GPL-3.0-or-later like the rest of this repository.

Written from scratch, no third-party code, data or formula note (OS-INSFX-FX1): the SD effect module `modules/effects/drive` (saturation: a polynomial soft clipper with a shape bias, a one-pole tone,
a one-pole DC blocker, a dry/wet mix and an output level). Nothing was ported or transcribed from any clipper design, app note or library: the shaper, the blocker and the smoothing are this module's own, and its oracle
(`modules/effects/drive/tests/drive_test.cpp`) is an independent double-precision model written in the test. The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

Written from scratch, no third-party code, data or formula note (OS-INSFX-FX1): the SD effect module `modules/effects/crush` (bit-depth and sample-rate reduction: a dead-zone quantiser, a sample-and-hold driven by a 32-bit phase accumulator,
a dry/wet mix and an output level). Nothing was ported or transcribed from any bit-crusher design, app note or library; the "add and subtract 1.5 * 2^23" rounding is the textbook float trick and is not code of any source. Its oracle
(`modules/effects/crush/tests/crush_test.cpp`) is an independent double-precision model written in the test. The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

Written from scratch, no third-party code, data or formula note (OS-INSFX-FX1): the SD effect module `modules/effects/fold` (a wavefolder: a triangle fold with a level-compensated drive and a symmetry offset, a one-pole tone low-pass,
a normalised one-pole DC blocker, a dry/wet mix and an output level). Nothing was ported or transcribed from any wavefolder design, app note or library: a triangle wave folded by reflection at +-1 is textbook geometry, and the
"add and subtract 1.5 * 2^25" rounding is the float trick (the 2^23 one of the CRUSH module scaled to multiples of 4), not code of any source. Its oracle (`modules/effects/fold/tests/fold_test.cpp`) is an independent double-precision
model written in the test (the fold by fmod). The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

Written from scratch, no third-party code, data or formula note (OS-INSFX-FX2 X4 and X5): the SD effect module `modules/effects/autopan` (a stereo auto-pan: an LFO on the side with the linear law `S' = S + p M`, a sine from two
parabolas and one correction step, a triangle, a square through a one-pole, the phase from the host's beat; and the HEADPHONE mode: the far ear delayed by up to 0.6 ms and darkened by a one-pole low-pass that follows the pan).
Nothing was ported or transcribed from any auto-panner, binaural or HRTF code, data set or library: the mid and side decomposition and the parabolic sine are textbook, and the HEADPHONE mode only takes the **idea** of the
spherical-head model (an interaural time difference and a head-shadow level-and-tone difference between the ears) with its own constants (0.6 ms, 20 kHz to 1.8 kHz), no code or data. Its oracle
(`modules/effects/autopan/tests/autopan_test.cpp`) is an independent double-precision model written in the test. The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

Written from scratch, no third-party code or data (OS-INSFX-FX1): the SD effect module `modules/effects/envfilter` (ENV FILTER: an envelope follower driving the cutoff of a state-variable filter, a dry/wet mix). The filter is the topology-preserving
(trapezoidal) state-variable structure, a published one (Zavalishin, "The Art of VA Filter Design", the TPT SVF), written here from its equations: the structure is the only thing taken, and no code of any library, note or plug-in. The follower,
the sweep law, the polynomials for log2, 2^x and tan, the smoothing and the mode weights are the module's own. Its oracle (`modules/effects/envfilter/tests/envfilter_test.cpp`) is an independent double-precision model written in the test.
The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

Written from scratch, no third-party code, data or formula note (OS-INSFX-FX1): the SD effect module `modules/effects/phaser` (a phaser: first-order all-pass sections in the normalised lattice form, the bilinear coefficient with a Pade [3/2] tan, a
sine-like LFO from a smoothed triangle, a feedback that is scaled and clamped, a dry/wet mix). Nothing was ported or transcribed from any phaser design, app note or library; the all-pass section, the bilinear transform and the Pade approximant of tan are textbook mathematics,
not code of any source. Its oracle (`modules/effects/phaser/tests/phaser_test.cpp`) is an independent double-precision model written in the test, plus an exact response from `std::tan` and complex arithmetic. The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

Written from scratch, no third-party code, data or formula note (OS-INSFX-FX1): the SD effect module `modules/effects/comp` (a compressor with a TRANSIENT mode: a peak-follower compressor with a soft knee, a transient shaper from a fast and a slow
envelope follower, a make-up gain and a dry/wet mix folded into one gain). Nothing was ported or transcribed from any compressor or transient-shaper design, app note or library; the log2 and exp2 polynomials were fitted for this module and the
"add and subtract 1.5 * 2^23" rounding is the textbook float trick, code of no source. Its oracle (`modules/effects/comp/tests/comp_test.cpp`) is a double-precision model of the same structure written again in the test (it checks the float arithmetic, not the design; law tests check the design). The module's source is
GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

Written from scratch, no third-party code, data or formula note (OS-INSFX-FX2): the SD effect module `modules/effects/pingpong` (a tempo-synced stereo echo: a stereo line of int16 words rounded toward zero, a one-pole low-pass and high-pass in the feedback, a
cross-fade of two taps on the words, a mono-sum-preserving width). Nothing was ported or transcribed from any delay design, app note or library; the ping-pong arrangement, the one-pole filters and the cross-fade are textbook mathematics, and the structure of this OS's own
send delay (`audio/include/o3os/audio/send_delay.h`) was followed as a fact (the rules, not its code). Its oracle (`modules/effects/pingpong/tests/pingpong_test.cpp`) is an independent double-precision model written in the test. The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).
Written from scratch, no third-party code, data or formula note (OS-INSFX-FX2): the SD effect module `modules/effects/wide` (WIDE: a stereo widener, a delayed and high-passed copy of the mid added to the side). The widening idea (a short delay of the mid, added to the side so that the mono sum cancels it, Lauridsen's trick) is a textbook one; nothing was ported or transcribed from any widener, plug-in, app note or library, and the one-pole high-pass, the ring and the cross-fade are the module's own. Its oracle (`modules/effects/wide/tests/wide_test.cpp`) is a double-precision model of the same law written again in the test over a plain history array. The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).
Written from scratch, no third-party code, data or formula note (OS-INSFX-FX2): the SD effect module `modules/effects/chorus` (a stereo chorus: one delay line of the mid, two linearly interpolated taps swung in opposite
directions by a sine-like LFO, a dry/wet crossfade of the mid that keeps the input's side). Nothing was ported or transcribed from any chorus design, app note or library: a modulated delay line read with linear interpolation
is textbook DSP, and the parabolic sine is the usual two-term approximation, not code of any source. Its oracle (`modules/effects/chorus/tests/chorus_test.cpp`) is an independent double-precision model written in the test.
The module's source is GPL-3.0-or-later like the rest of this repository (the SDK header it includes is MIT).

## pt2-clone's Paula (OS-005h)

`audio/paula.h` re-implements the model of pt2-clone's Paula emulator (period-based sample-and-hold, minBLEP band limiting, the Amiga 500 / 1200 RC filters and the LED
filter) in single-precision float, block based, with the original's arithmetic in the original's order; `tests/paula_oracle_test.cpp` runs the original (vendored, host
only) beside it and the raw channel is bit-exact (tolerance 0) over a fuzz of period, volume and restart writes, the filters within 1e-6. Provenance: the files were
fetched on 2026-10-12 from `https://raw.githubusercontent.com/8bitbubsy/pt2-clone/18954f23c3d0ebd8583e61b65fe4507ae8aff4bd/` into an empty scratch directory (nothing run);
the LICENSE is "BSD 3-Clause License, Copyright (c) 2010-2026, Olav Sorensen" (the files carry no header of their own).

- `LICENSE` SHA-256 `a17d758d5cf35c30bc53ba93de3e71df6a3e6e76652d31b761e61b4028b518cc`
- `pt2_paula.c` SHA-256 `dab7d7a79247631f61409a34c1578f496feb9ee5d1aa38bae8b1942a9e5ac763`
- `pt2_paula.h` SHA-256 `f650f6d4cdbc7e045eeee5cf1666578c887f475ca7169e390e5a91edc75417c2`
- `pt2_blep.c` SHA-256 `0c32d66ba1a03fb5df10e4d78a1bc426c114543e4e93da3eda8a0ea5effcbb52`
- `pt2_blep.h` SHA-256 `c51a8aebb8290b6c99b8f625532b874211a83898c428a6a19629cf8edf220974`
- `pt2_rcfilters.c` SHA-256 `412bfbfeb1e6b31a9958d567b594f4fc5e35914d798e5bd0fd2c328dadeeffc8`
- `pt2_rcfilters.h` SHA-256 `c54e3a5865f3b200cf3d1550af8488a45d177d56cdcd1acc17b2aaf9a42a3585`

What reaches the firmware image is DATA taken from the original: the period table (a standard ProTracker table: 16 finetunes x 36 notes) and the 257 floats of the minBLEP
step, which the original credits to aciddose inside its BSD-3 repository (its origin beyond that is not stated: provenance risk 7 of the radar card stays open; the
notice travels with the table in `audio/src/paula_tables.cpp`, and the BSD-3 text should also go into the product's licence notices). Not taken: the 2x oversampling
and half-band downsampler (`pt2_downsample2x.c`), the DMA register emulation, the GUI, the replayer.

## libxmp as the MED tick oracle (OS-008c)

`audio/med_tick.h` (the MED tick processor, docs/formats/med.md slice M3) is written from the MED command descriptions and from the order of work of libxmp's player
(`https://github.com/libxmp/libxmp`, MIT, "Copyright (C) 1996-2025 Claudio Matsuoka and Hipolito Carraro Jr", commit `5d4f6aa2c7dcdf8e9cd8129fe579d45b36a302da`, master on
2026-10-12; the licence text is the header of every source file I read: `src/med_extras.c`, `src/player.c`, `src/effects.c`, `src/read_event.c`, `src/lfo.c`,
`src/loaders/mmd_common.c`). No file, function or table of it is copied or compiled into the tree; the arithmetic of each effect was read and re-written in integers
(the 32-step sine is ProTracker's table, a published constant of the format). libxmp is used ONLY to produce `tests/data/med_tick_golden.txt`: `tools/med_tick_golden.py`
writes 27 one-track MMD1 scenarios with `tools/mmd_build.py` (made-up sample), plays them with a small dump program linked against a libxmp that a developer builds by hand
(`gcc -c` on its sources with -DLIBXMP_NO_PROWIZARD -DLIBXMP_NO_DEPACKERS, no build script run; the dump program is five lines of the public `xmp_play_frame` /
`xmp_get_frame_info` API and is not in the repository), and keeps the printed periods, volumes and restart markers. Those numbers are facts about the format as played by
libxmp; the file contains no libxmp code. The tarball read was `https://api.github.com/repos/libxmp/libxmp/tarball/5d4f6aa2c7dcdf8e9cd8129fe579d45b36a302da`, SHA-256
`5aead9abe7f972de8364485f1f40332c1216884c45aa1fc4140e5bc16f708ac9`, listed with `tar -t` (no absolute or `..` paths) and extracted into an empty scratch directory.

## Dattorro's plate reverberator, as a reference (OS-SENDFX)

`audio/send_reverb.h` follows the structure of the plate in J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", J. Audio Eng. Soc. 45(9), 1997 (after Griesinger's work): four input allpasses, a two-branch tank with modulated allpasses, damping, and the output taps. The paper is a reference only: no code of it or of any implementation is taken, the delay lengths and tap offsets are the paper's published numbers as the author remembers them (to be audited against the paper), and the code is our own. Mutable Instruments' Clouds reverb (MIT) was read for the topology only and nothing of it is used.

## Polyend Tracker Community Edition: the wavetable contour bank (OS-CEWTENV W2)

`audio/include/o3os/audio/wt_contour.h` and `audio/src/wt_contour.cpp` are a port of one component of the Polyend Tracker Community
Edition firmware (tracker-ce, github.com/polyend/tracker-ce, commit `7fd6c2c`): `core/source/mtWavetableEnvelopes.cpp` and its
header — the 256 procedural wavetable contours (0..127 factory, 128..255 user fallbacks) and the warp law of `evalWavetableEnvelopeFloat`
(nibble decode, warp X on the phase, warp Y on the level, -7 on Y inverts, 128-point linear interpolation). The port keeps the maths
identical and rewrites the code in this tree's style (C++17, float, no double, no allocation, no exceptions); the component's `.wav`
contour loader is not ported (slice W8) and nothing else of tracker-ce is taken. The maintainer approved this one component for this
item (OS-CEWTENV Q1 a, 2026-11-05); the upstream licence also restricts a wholesale commercial clone of the complete application
(upstream `LICENSE` Part 2), which this port is not. No file of this component is in this repository (the CHIP engine's file is vendored as a test oracle, below), and the
component's provenance is pinned by the golden `tests/data/wt_contour_golden.h` (made once from the upstream code; the procedure is
in `tests/audio_wt_contour_test.cpp`'s header).

The component's copyright line and MIT permission notice, copied exactly from tracker-ce@7fd6c2c `LICENSE`:

> Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice, this permission notice, and the conditions in
> Part 2 shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

The conditions of Part 2 of the same `LICENSE`, which its MIT notice requires in every copy, copied exactly (they restrict the
complete Tracker application, and Part 2 section 1(c) expressly permits reusing an individual component like this one):

> PART 2: WHOLESALE COMMERCIAL CLONE RESTRICTION & EXPRESS PERMISSIONS
> ================================================================================
>
> 1. What Is Expressly Permitted (Commercial & Non-Commercial):
>    (a) Music Production, Live Performance & Content Creation:
>        Using the firmware to compose, record, produce, perform, distribute,
>        broadcast, or monetize music, albums, sample packs, paid live
>        performances ("gigs"), courses, or monetized video/streaming content
>        is EXPRESSLY PERMITTED without restriction.
>    (b) Selling `.ptfx` Sample Editor Plugins:
>        Building, distributing, and selling `.ptfx` Sample Editor plugins built
>        with `editor-fx-sdk/` (commercially or non-commercially) is EXPRESSLY
>        PERMITTED under the pure MIT License in Part 1.
>    (c) Reusing Code, Drivers, DSP, and Open Synth Engines in Other Projects:
>        Reusing, modifying, or incorporating individual components, drivers,
>        DSP algorithms, UI helpers, SDKs, or open-source C++ synth engines
>        (such as `core/synth/engines/`) into your own open-source or commercial
>        software (e.g., VST/AU/CLAP plugins, iOS/desktop apps) or original
>        hardware products (e.g., Eurorack modules, standalone instruments that
>        are not clones of the Polyend Tracker) is EXPRESSLY PERMITTED under the
>        MIT License in Part 1.
>
> 2. What Is Prohibited (No Wholesale Commercial Clones):
>    Notwithstanding the permissions in Part 1, you may NOT:
>    (a) Commercial Hardware Clones:
>        Manufacture, distribute, or sell a commercial hardware product that is
>        preloaded with, designed primarily to run, or marketed to use the
>        complete Polyend Tracker firmware/application (or a substantially
>        unmodified or re-skinned derivative of the complete firmware/application);
>    (b) Commercial Software Clones / Ports:
>        Sell, license for a fee, or commercially distribute the complete Polyend
>        Tracker application/firmware (or a direct port or substantially
>        unmodified derivative of the integrated Tracker sequencer, UI, and
>        workflow as a whole) as a paid standalone software application, paid
>        plugin, or paid mobile/desktop app;
>    (c) Selling Firmware Builds:
>        Sell or charge a fee for downloading or obtaining compiled `.ptf`
>        firmware images or source distributions of the Polyend Tracker firmware;
>    (d) Trademark & Trade Dress:
>        Nothing in this License grants any right to use the "Polyend" or
>        "Polyend Tracker" trade names, trademarks, service marks, logos, boot
>        screens, or hardware trade dress in connection with any third-party
>        commercial product.

## Polyend Tracker Community Edition: the custom play orders (OS-PERFMODES M1)

The custom play orders of `core/source/playModeDefs.h` — the twenty tables C1..C20 of 128 entries (step to play, FX option), 2,560 numbers — are taken
from the Polyend Tracker Community Edition firmware (tracker-ce, github.com/polyend/tracker-ce, commit `7fd6c2c`) as an approved MIT component, data
only (maintainer decision Q1 a, 2026-11-05): `tools/gen_play_orders.py` reads them from a checkout of that commit (given by its root) and writes
`seq/src/play_orders_data.inc`, which is committed (CI never runs the generator; no file of this component is in this repository). The MK66 image carries the
numbers only in a deflate-packed form (OS-FLASHDIET P4: `tools/play_orders_pack.py` writes a blob into the build tree from that same `.inc`, and the firmware
inflates it into SDRAM at boot); it is the same data, under the same notice.
No code or comment of tracker-ce is copied, only the numbers. tracker-ce's copyright notice for this component, from `LICENSE` at that commit:

> Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors

## Polyend Tracker Community Edition: the CHIP synth engine, as a test oracle (OS-CECHIP C1)

`third_party/tracker-ce-chip/ChiptuneSynthVoice.cpp` is `core/synth/engines/ChiptuneSynthVoice.cpp` of the Polyend Tracker Community Edition firmware
(tracker-ce, github.com/polyend/tracker-ce, commit `7fd6c2c3002a8f690b5341c486ba69606077b513`), the CHIP synth engine, an approved MIT component (maintainer
decision, 2026-11-05), vendored **byte for byte** next to a byte-for-byte copy of that commit's whole `LICENSE` (Parts 1 to 3, whose Part 2 conditions its MIT
notice requires in every copy). The file's SHA-256 and the licence's are in `third_party/tracker-ce-chip/PROVENANCE.md` and are checked by ctest
`cechip_oracle_sha`. It is **only a test oracle**: a static library `cechip_oracle` built for the host with the unit tests (C++20 for that target, no warnings and
no UBSan on the original's file, `-ffp-contract=off`), never linked into any firmware target (slice C1 adds 0 bytes to an image). The headers the file includes
(`config.h`, `ChiptuneSynthVoice.h`, `PolySynth/sdk/SynthVoiceSDK.h`) are **ours**, written from the facts of `docs/specs/OS-CECHIP.md` in
`tests/support/cechip_shim/`; no file of tracker-ce's SDK is vendored or copied, and `synth/lib/*.a` (proprietary, Part 3 of the licence) and every other file of
tracker-ce are not used. The port itself (OS-CECHIP C2 onwards: `modules/chip/chip_ce.h`, the pulse family, the parameters 20..29 and Chord of `chip_dsp.h`, the tracker-ce chord tables) is
Chip's code: waves 4..11 of the Chip module, MIT, with the notice in the header of `chip_ce.h`, in `modules/chip/LICENSE-tracker-ce.txt` (Parts 1 and 2 of the licence, verbatim) and in
`kBuiltinNotices` (`diag mod notice`); Chip stays a separate `.tmod`. The upstream licence
also restricts a wholesale commercial clone of the complete Tracker application (Part 2), which neither this oracle nor Chip is.

The copyright line and MIT permission notice, copied exactly from tracker-ce@7fd6c2c `LICENSE` (the same text as in the two sections above; Part 2 is in the vendored
`third_party/tracker-ce-chip/LICENSE`):

> Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice, this permission notice, and the conditions in
> Part 2 shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

## Polyend Tracker Community Edition: the `.ptfx` SDK header and six example plugins (OS-PTFXPACK P1)

`modules/ptfx/sdk/tracker_plugin.h` is `editor-fx-sdk/tracker_plugin.h`, and `modules/ptfx/<name>/<name>.c` are `editor-fx-sdk/effects/<name>.c` for Add Silence
(`add_silence`), Analog Kick Gen, Chiptune Voice Gen, Multi-Mode Filter, Stereo Delay and Stereo to Mono, of the Polyend Tracker Community Edition firmware (tracker-ce,
github.com/polyend/tracker-ce, commit `7fd6c2c3002a8f690b5341c486ba69606077b513`). They are an approved MIT component (maintainer decision, 2026-11-08, `CLAUDE.md`
hard rule 1): upstream `LICENSE` Part 1 covers `editor-fx-sdk/` and Part 2 1(b) expressly allows plugins built with this SDK. What is **taken**: the seven files
as they are. What is **changed**: each file gets a notice block in front (the SPDX line, where the file comes from, the copyright line, the MIT permission text and the
pointer to Part 2), ended by the line `// ---- end of the notice: ...`; everything after that line is byte for byte upstream's (ctest `ptfx_pack` hashes it against
`tools/testdata/testdata_manifest.json`). `modules/ptfx/LICENSE-tracker-ce.txt` is Parts 1 and 2 of the upstream licence, verbatim, as for Chip. **Not used**:
`ptfx_builder.py` (ours is `tools/ptfx_pack.py`, whose payload and parameter bytes are identical to the official builder's for these six files; it also writes the
`PTFX_AUTHOR` into the header, which the official builder leaves blank), every other tracker-ce file and `synth/lib/*.a` (proprietary, Part 3). The built `.ptfx` files
are build products (`build-os-host/modules/ptfx/`), copied into the SD content zip by `tools/release/make_starter.py`; none is in git and none is in a firmware image. The
upstream licence also restricts a wholesale commercial clone of the complete Tracker application (Part 2), which a set of effect plugins is not.

The copyright line and MIT permission notice are the ones quoted in the sections above, from tracker-ce@7fd6c2c `LICENSE`.

## Polyend Tracker Community Edition: six Sample Editor effects rebuilt as `.ptfx` (OS-PTFXPACK P2)

Overdrive, Delay, Bitcrusher, Chorus, Flanger and Limiter of the Sample Editor of tracker-ce (commit `7fd6c2c3002a8f690b5341c486ba69606077b513`, `core/effector/`) exist
here as `modules/ptfx/<name>/<name>.c`, one file each, so that they run through the `.ptfx` plugin path (OS-CEPTFX) on the unit. They are an approved MIT component
(maintainer decision, 2026-11-08, `CLAUDE.md` hard rule 1; upstream `LICENSE` Part 1 covers the `core/` DSP algorithms). What is **taken**: the signal paths, constants and parameter
ranges of the six effects, with the original's quirks. What is **ours**: the C files themselves, written against `tracker_plugin.h` (the original is C++ working on whole selections
in the edit pool; a plugin sees blocks), so none is a copy. Each file starts with the SPDX line, the provenance, the copyright lines of the original and of this port, the MIT text
and a list of what is the original's and every change (for example: Delay as a recursive comb with the original's voice delays and gains and its tail asked from the host; Chorus
and Flanger process every frame instead of whole groups of 128; Flanger wraps the tap index fully and uses the host `sinf`; Bitcrusher and Limiter keep their state across blocks; the
Length knobs are in ms because the SDK has no display multiplier). **Not used**: the other `core/effector/` effects (OS-CEPTFX S3 decides them one by one), `synth/lib/*.a`
(proprietary, Part 3). `modules/ptfx/LICENSE-tracker-ce.txt` is the upstream licence text as for P1. The built `.ptfx` files are build products; none is in git or in a firmware image.

## Polyend Tracker Community Edition: the three-band EQ rebuilt as `.ptfx` (OS-PTFXPACK P3)

The three-band EQ of the Sample Editor of tracker-ce (commit `7fd6c2c3002a8f690b5341c486ba69606077b513`, `core/effector/mtEffectEQ.cpp/.h`) exists here as `modules/ptfx/eq3/eq3.c`, run through the `.ptfx` plugin path (OS-CEPTFX). It is an approved MIT component (maintainer decision, 2026-11-08, `CLAUDE.md` hard rule 1; upstream `LICENSE` Part 1 covers `core/`). What is **taken**: the three-band signal path (the Regalia-Mitra equaliser filter the original cites), its level map, bandwidth rule and parameter ranges. What is **ours**: the C file, written against `tracker_plugin.h`; its notice lists the changes (a band at 0 dB is skipped instead of the original's level-equals-1 test, flat defaults, single precision, state reset per run). **Not used**: the other `core/effector/` effects. Wavetable Smoother, Cut and Time-stretch are not rebuilt: a plugin sees one block at a time and cannot give them what they need (see `tools/release/README.md`). `synth/lib/*.a` is never used. `modules/ptfx/LICENSE-tracker-ce.txt` is the upstream licence text as for P1. The built `.ptfx` file is a build product; none is in git or in a firmware image.
