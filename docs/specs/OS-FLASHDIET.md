# OS-FLASHDIET — recover core flash for the approved specs

Status: decided and queued 2026-11-05 (Q1-Q5 a); specified 2026-11-05, not implemented · Level B per slice, Level C for T3 (the audio path) and for I1 to I3 (a published ABI, a format reader that moves) · Backlog row OS-FLASHDIET, approved by the maintainer 2026-11-05 · Measured on `dev` 788b0738, arm-none-eabi-gcc 15.3.1, MinSizeRel, `O3OS_RATE_44100=ON`, once with `O3OS_BENCH=ON` (bench) and once without (release) · Everything this spec proposes is **new Community OS behaviour** with no visible change, except the message of an import whose importer file is missing (section 3.6).

## Maintainer decisions (taken 2026-11-05)

**Decided by the maintainer 2026-11-05: Q1 a, Q2 a, Q3 a, Q4 a, Q5 a (every recommended default): a version-locked importer kind for first-party modules; MOD, S3M and MED move to the card, the sample readers stay; the four built-in instruments stay in flash, compressed; LTO parked as research (R2); phase 2 right after phase 1. Queued.**

**Taken 2026-11-05 (relayed by the orchestrator):** an importer may depend on a file on the card ("un importador puede depender de un fichero en la tarjeta, no problem"). The MOD / S3M / MED importers may move to an importer `.tmod` on the card; the spec says what a user without the file sees and how the starter zip ships it (3.6, slice I3).

Open questions (each with a recommended default; everything else is decided in section 6):

| # | Question | Options | Recommended | Why |
|---|---|---|---|---|
| Q1 | The importer module's ABI | a) version-locked: kind 3 is first-party only, the module carries the OS model's layout tag and works on the OS's own model structures; a mismatch is refused with a message · b) a stable public POD ABI (steps, instruments, order list as C structs) open to third-party importers | a | The importers write the model directly today (`formats/mod/include/o3os/formats/mod/import.h:110`); a POD ABI is about 400 more lines, a second mapping layer to test, and it freezes the import-facing model fields. The module ships in every release's starter zip, so a lock costs users nothing; b stays possible later as ABI 2 |
| Q2 | Which readers move to the card | a) MOD, S3M and MED (the project importers, about 40.6 KB) · b) also the AIFF / 8SVX and Surge `.wt` sample readers (about 7.5 KB more) | a | Sample readers are used from the sample browser on every load of such a file; moving them needs the importer kind to cover samples too (more ABI). Phase 1 and phase 2 with a leave the release image about 77 KiB free and the bench image about 57 KiB (3.8) |
| Q3 | The built-in instrument modules (TB-303, chip, FM, SID) | a) all four stay in flash, compressed (slice A3) · b) SID and FM leave the image for the card (about 9 KB more after compression; a project using them says the module is missing until the file is copied) | a | Compression alone saves 14.7 KB with nothing visible; b changes what a bare unit can play |
| Q4 | Link-time optimisation (LTO) | a) parked as reserve research (R2) · b) one spike slice now (Level C: build, size, every CPU bench row on the unit) | a | The experiment did not link (fact F9); its saving is unknown and it changes the code of every real-time path, so it needs a full CPU re-measurement. Phase 1 already gives about 50 KB |
| Q5 | When phase 2 (importers on the card) runs | a) right after phase 1 · b) only when the bench image's free flash drops under 16 KB | a | After phase 1 plus the approved specs the bench image keeps about 21 KiB (3.8); phase 2 adds 37 KB of flash and gives back 40 KB of SDRAM_STATIC |

## 1. Goal

Recover at least 30 KB of core flash, without costing render CPU and without any visible change, so that the approved specs (about 65 KiB of core flash in total, listed in the backlog row) fit in both the release and the bench image. **Realistic:** phase 1 (eleven S slices, invisible, Level B except T3) about 50 KB in both images (between 45 and 53 KB with the estimate error); phase 2 (the importers on the card, four S slices, Level C) about 37 KB more; the bench image alone about 11 to 13 KB more (slice B1). That is about 87 KB in the release image.

## 2. Facts (measured)

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | Bench image: 1,022,028 B of content (`.text` 993,636 + `.ARM.exidx` 8 + `.init_array` 24 + `.ramfunc_l` load image 27,608 + `.data` 752), padded to 1,024,000 B (1000 KiB): **24 KiB free**. Release image: 988,692 B, padded to 991,232 B (968 KiB): **56 KiB free**. The bench costs 33,336 B (`bench.cpp` 25,010, its strings, the console's bench hooks). The pad is to the 4 KiB sector (OS-BUG-COSMBOOT) | `arm-none-eabi-size -A -d o3os.elf`; `platform/mk66/mk66.ld.in:223-235`; `platform/mk66/CMakeLists.txt:113-121` | measured |
| F2 | `-ffunction-sections -fdata-sections` on every cross target, `--gc-sections` at the link, `-Os` (MinSizeRel) everywhere on the MK66; no LTO anywhere. String literals sit in per-function `.rodata.<fn>.str1.1` sections, so the strings of a dropped function are dropped too; the linker merges equal strings across objects (117,157 B before the merge, 98,225 B after) | `CMakeLists.txt:83`, `platform/mk66/CMakeLists.txt:188-191`; the map's `.str1.1` entries and "size before relaxing" | measured |
| F3 | Seven audio files are forced `-O3` for CPU (send delay and reverb, limiter, DJ filter, kill EQ, scratch, loop player). At `-O3` they take 31,034 B, at `-O2` 22,312 B, at `-Os` 18,286 B; the growth is in the hot functions themselves (`MasterScratch::process` 3,596 B at -O3 against 888 at -Os, `SendReverb::run` 2,214 against 944, `LoopPlayer::serve` 2,276 against 728). Not undone here (reserve R1) | `audio/CMakeLists.txt:46-51` | measured (object sizes) |
| F4 | The built-in modules are four `.tmod` files embedded as bytes: SID 14,644, TB-303 10,104, FM 9,404, chip 6,588 (40,740 B) plus the notices 1,582 B. The loader reads a built-in sequentially through `read_some` (a `memcpy` from flash at `flash_pos_`) into the module arenas; only the 64-byte header is read in place (`builtin_header`) | `modules/CMakeLists.txt:196-220`, `tools/o3m_embed.py:23-52`, `module/include/o3os/module/builtin.h:18-46`, `module/src/loader.cpp:191-205`, `:234-245` | measured; code fact |
| F5 | UI assets: four L1 glyph faces (16x32 6,080 B, 10x20 3,800, 8x16 1,520, 6x12 1,140), the icon face 352 B, the boot logo (124x124 L4) 7,688 B. They are uploaded once at boot (`upload_assets`); tiles are composed at boot from the Data, Label and Micro faces (`compose_tile`); nothing else on the target reads them afterwards (the simulator's gallery and host tests do) | `ui/src/assets.cpp:26-57`, `:136-206`; `ui/include/o3os/ui/font.h:64-79`; `sim/src/gallery.cpp:88` | measured; code fact |
| F6 | Help text: 19,354 B of strings plus 3,492 B of line tables in `help_text.cpp`, 15 pages, the largest 2,961 B of text and 57 lines; served by `help_page(index)` | `app/src/help_text.cpp`, `app/include/o3os/app/help_text.h:23-37`, `tests/help_test.cpp` | measured |
| F7 | Raw deflate (RFC 1951) with a 4 KiB window, as Python's zlib makes it at level 9: help 19,354 -> 7,986 B; faces and icons 12,892 -> 2,816; logo 7,688 -> 4,138; the four modules 40,740 -> 26,063 (per file). Fixed-Huffman only would be 46,528 B for all of them against 41,003 (dynamic); LZ4- and LZSS-style formats came out clearly larger on the same bytes. Python's zlib output depends on the zlib library (this machine: built against 1.2.11, runs 1.3) | measurement with the image's own bytes | measured |
| F8 | The MOD / S3M importer (`formats/mod`, 14,662 B of code, 1,781 B of strings) and the MED importer (`formats/med`, 20,674 + 3,520 B) take 40,637 B; nothing outside `formats` and `app/include/o3os/app/module_import.h` includes them. Their objects (`ModuleImport`'s two `ImportReader` members) live in SDRAM_STATIC: `g_module`, 40,704 B. The image does not link the format registry (`register_builtin_formats`: 0 symbols in the map); the File screen asks `ModuleImport::probe` (the first bytes and the name, through `mod::sniff_kind` and the MMD check) whether a file is a module. The importer is a resumable job: `begin(RandomSource&, model::ModelArena&, ImportOptions)`, `step(budget)`, `read_pcm(slot, first, n, dst)`, a report. `ModuleImport::begin` only opens the file; the first `step()`, after Settle, resets the document; a failure before it leaves the document as it was, one after it leaves it empty | `formats/CMakeLists.txt:17-40`, `formats/mod/src/module.cpp:91`, `formats/mod/include/o3os/formats/mod/import.h:184-200`, `app/include/o3os/app/module_import.h:6-20`, `:80-100`, `:153-154`, `app/src/module_import.cpp:278-300`, `platform/mk66/src/app_main.cpp:1105`, `:1169` | measured; code fact |
| F9 | LTO experiment (`-flto`, `gcc-ar`, then one partition, then grouped archives): the link fails with "Unknown destination type (ARM/Thumb)" on calls into `audio::diag::Text` and `usb::diag::Output`; no size figure | local experiment, not committed | open research question |
| F10 | The module ABI reserves kind 3 (importer) and the OS refuses it today; a module has no libc, no writable global, no constructor and no undefined symbol (the packer refuses each); image and state up to 256 KiB; the module region is 768 KiB of SDRAM (64 KiB of it the module system) | `module/sdk/o3module.h:103-107`, `module/include/o3os/module/image.h:52-57`, `:71`, `tools/o3m_pack.py:19`, `module/sdk/README.md:153`, `platform/mk66/include/o3os/platform/mk66/mem_map.h:102-103` | code fact |
| F11 | Template and clone duplication: `pe::edit_selection<Fn>` 17 instances (7,032 B); `append_uint<N>` 40 (2,002 B) and `append_dec<N>` 27 (1,254 B), plus their per-object `.isra` clones and `say()` / `put_dec()` copies (5,415 B of duplicated local clones in all) | `app/src/pattern_edit_util.h:67`, `app/include/o3os/app/text_format.h:44-60` | measured |
| F12 | libgcc's soft double (1,548 B: add/sub, mul, d2uiz) is pulled by one float to int64 conversion in `VoicePool::move_position`; one uint64 to float in `AppTap::linear_rms` pulls the soft single add (540 B) | `audio/src/voices.cpp:2466`, `audio/src/app_tap.cpp:43-48`; the map's archive member list | measured |
| F13 | The starter zip packs the modules listed in `starter_manifest.json` (`modules.apps`, `instruments`, `effects`) from a build's `modules/` folder; its generated content includes an OctaMED song | `tools/release/starter_manifest.json:4`, `tools/release/README.md:12-17` | code fact |

### 2.1 Flash by area (release image, content bytes)

| Area | Bytes | % | Notes |
|---|---|---|---|
| app (screens, actions, bindings) | 378,197 | 38.3 | code 341,696; tables 36,493 (bindings 9,632, actions 5,700) |
| audio | 117,828 | 11.9 | 24,816 of it is SRAM_L hot code (copied from flash at reset) |
| platform (image objects, mk66 pure) | 110,555 | 11.2 | composition root `app_main` 31,447, `usb0` 9,430, `fw_service` 6,928, `diag_text` 6,315 |
| string literal pool (merged) | 98,225 | 9.9 | before the merge: app 71,798 (help 19,354), image 17,892 (console 10,906), formats 11,866, platform 9,827 |
| formats | 74,242 | 7.5 | MOD / S3M / MED importers 35,336 (plus 5,301 B of their strings in the pool); native 14,172; OG 11,240; samples 11,506 |
| built-in modules (`.tmod` bytes) | 42,374 | 4.3 | SID, TB-303, FM, chip, notices |
| model | 30,135 | 3.0 | |
| seq | 29,900 | 3.0 | |
| ui assets | 20,708 | 2.1 | faces 12,636, logo 7,704, icons 368 |
| ui code | 16,919 | 1.7 | |
| console (diag) | 13,710 | 1.4 | plus its strings in the pool |
| usb | 13,483 | 1.4 | |
| third_party (FatFs) | 12,417 | 1.3 | |
| core | 8,118 | 0.8 | |
| midi | 8,076 | 0.8 | 2,014 of it SRAM_L hot code |
| module host | 7,626 | 0.8 | |
| toolchain libraries | 4,465 | 0.5 | libgcc 3,280 (soft double 1,548), newlib-nano 1,079 |
| alignment fill, eve | 1,714 | 0.2 | |
| **total** | **988,692** | | bench image: + 33,336 |

### 2.2 Flash by library (release image; strings in their own row)

| Library | Bytes | Code | Constants | SRAM_L code |
|---|---|---|---|---|
| o3os_app | 378,197 | 341,696 | 36,493 | 0 |
| o3os_audio | 117,828 | 87,658 | 5,354 | 24,816 |
| (merged strings) | 98,225 | | 98,225 | |
| image objects (`platform/mk66/src`, console, usb device, storage, display, input) | 73,611 | 68,130 | 3,612 | 736 |
| o3os_formats + formats_wav | 74,242 | 72,756 | 1,486 | 0 |
| o3os_platform_mk66_pure | 50,654 | 48,064 | 2,578 | 0 |
| o3m_builtin | 42,374 | 0 | 42,374 | 0 |
| o3os_ui | 37,627 | 16,324 | 21,303 | 0 |
| o3os_model | 30,135 | 25,702 | 4,433 | 0 |
| o3os_seq | 29,900 | 29,166 | 734 | 0 |
| o3os_usb | 13,483 | 13,208 | 275 | 0 |
| o3os_fatfs | 12,417 | 11,334 | 1,083 | 0 |
| o3os_core, o3os_midi, o3os_module, o3os_eve | 23,974 | 21,744 | 216 | 2,014 |
| toolchain, fill | 6,025 | | | |

### 2.3 The 40 largest symbols (release image)

| # | Bytes | Symbol | # | Bytes | Symbol |
|---|---|---|---|---|---|
| 1 | 14,644 | `module::k_sid` (built-in .tmod) | 21 | 3,768 | `app::App::run_once` |
| 2 | 10,748 | `app::ParamsScreen::render` | 22 | 3,708 | `app::PatternView::draw` |
| 3 | 10,104 | `module::k_tb303` | 23 | 3,620 | `app::SampleEditScreen::render` |
| 4 | 9,632 | `app::kBuiltinTable` (602 bindings) | 24 | 3,596 | `audio::MasterScratch::process` (-O3) |
| 5 | 9,404 | `module::k_fm` | 25 | 2,276 | `audio::LoopPlayer::serve` (-O3) |
| 6 | 8,444 | `app::MixerScreen::render` | 26 | 2,214 | `audio::SendReverb::run` (-O3) |
| 7 | 7,688 | `ui::k_boot_logo` | 27 | 2,170 | `audio::SendDelay::process` (-O3) |
| 8 | 6,896 | `app::PerformScreen::render` | 28 | 1,968 | `formats::state_chunks::decode` |
| 9 | 6,588 | `module::k_chip` | 29 | 1,964 | `app::ModuleUi::launch_app` |
| 10 | 6,080 | `ui::k_f16x32_bitmap` | 30 | 1,948 | `USB0_IRQHandler` |
| 11 | 6,072 | `app::FileScreen::render` | 31 | 1,900 | `app::draw_song_timeline` |
| 12 | 5,700 | `app::kActions` (285 actions) | 32 | 1,796 | `formats::med::Reader::open_impl` |
| 13 | 5,052 | `appimg::pass` | 33 | 1,752 | `formats::native::Writer::build_piece` |
| 14 | 4,852 | `app::ConfigScreen::render` | 34 | 1,744 | `app::SaveService::SaveService` |
| 15 | 4,816 | `app::SongScreen::render` | 35 | 1,740 | `app::ViewState::ViewState` |
| 16 | 4,632 | `app::WaveScreen::render` | 36 | 1,732 | `audio::Graph::recompute_strip` |
| 17 | 4,608 | `app::RecordScreen::render` | 37 | 1,656 | `app::InputTestScreen::render` |
| 18 | 4,432 | `app::SampleLoadScreen::render` | 38 | 1,640 | `audio::apply_instrument_param` |
| 19 | 4,296 | `appimg::start_app` | 39 | 1,624 | `app::StepPopupOverlay::render` |
| 20 | 3,800 | `ui::k_f10x20_bitmap` | 40 | 1,600 | `audio::VoicePool::run_gran` |

Next below: `main` 1,584, `kBuiltinNotices` 1,582, `ModulesScreen::render` 1,576, `k_f8x16_bitmap` 1,520. The merged string pool (98,225 B) is not a symbol but is the largest single item. Every other flash object of 900 B or more: `paula::kPeriodTable` 1,152, `k_f6x12_bitmap` 1,140, `model::kDevice` 1,040, `paula::kMinBlep` 1,028, `kHalfSine` 1,002.

### 2.4 Compiled twice, dead, kept

- Nothing is compiled twice: the map's 328 objects are 328 distinct sources (five basenames repeat in different folders). The duplication is template instances and per-object clones (F11, about 12 KB in all).
- No dead code found: `--gc-sections` drops unreferenced functions and their strings; the linker script keeps only the vector table, the flash configuration field and the init arrays. The hidden INPUT TEST screen (1,656 B of render and more) is a feature (the boot chord), kept.
- Large constructors (14 KB over 200 B each: `SaveService` 1,744, `ViewState` 1,740, `SampleBankMem` 1,344, `Graph` 1,310, `App` 1,252) are member initialisers and memsets; a rewrite is uncertain and touches many structs (reserve R4).

## 3. Design

### 3.1 Candidates evaluated

| Candidate | Saves (est.) | Risk | Verdict |
|---|---|---|---|
| Compress the built-in `.tmod` bytes (inflate at load) | 12.7 KB net (14.7 minus the decoder) | low: load path only, the `.tmod` files and the loader's checks unchanged | phase 1 (A3) |
| Compress the glyph faces (the 352-byte icon face stays raw: 161 B to gain) | 9.7 KB | low: boot only; goldens prove it | phase 1 (A4) |
| Compress the boot logo | 3.4 KB | low | phase 1 (A5) |
| Compress the help text | 14.4 KB | low: page change only | phase 1 (A6, A7) |
| One `edit_selection` instead of 17 | 5.8 KB | low: main loop, host-tested edits | phase 1 (T1) |
| Non-template text helpers | 4.5 KB | low | phase 1 (T2) |
| Drop libgcc's soft double (two conversions) | 2.0 KB | low, but `move_position` is in the render (Level C) | phase 1 (T3), last |
| Retired A/B benches out of the default bench image | 11 to 13 KB, bench image only | none for users | phase 1 (B1) |
| MOD / S3M / MED importers to an importer `.tmod` | 37 KB net (40.6 minus 3 to 4 of host); SDRAM_STATIC -40 KB | medium: new ABI kind, a reader moves (Level C) | phase 2 (I0 to I3) |
| `-O3` files at `-O2` | up to 8.7 KB | render CPU unknown: each file needs its bench row on the unit | reserve R1 |
| LTO | unknown (literature on Cortex-M -Os builds: 5 to 10 % of code) | high: links fail today, every hot path changes | reserve R2 (Q4) |
| Console usage text shortened or compressed | 3 to 5 KB | the console is the documented update path and the HIL tools' channel | reserve R3 |
| Constructor rewrite | 3 to 5 KB | uncertain | reserve R4 |
| Bindings and action tables packed | about 3 KB | the binding struct is held at 16 B for its pools | reserve R5 |
| SID and FM to the card | 9 KB after A3 | visible (Q3) | reserve R6 |
| AIFF / 8SVX / Surge `.wt` readers to the importer module | 7.5 KB | visible (Q2) | reserve R7 |
| OG project / pattern / instrument readers to the card | 13 KB | OG files are the daily compatibility path | not proposed |

### 3.2 One decoder: `core::Inflater`

A raw-deflate decoder (RFC 1951: stored, fixed and dynamic Huffman blocks), written from scratch, in `core` (every consumer can include it: `tools/check_layers.py`). Pull interface over an input held entirely in memory: `begin(const uint8_t* z, size_t z_size, size_t raw_size)`, `size_t read(uint8_t* dst, size_t n)` (produces up to `n` bytes; it suspends only when `dst` is full, in the middle of a match if need be), `skip(size_t n)`, `done()` (the stream ended exactly at `raw_size`), `failed()`. A 4 KiB window ring and the Huffman tables live in the object (about 5.3 KiB). Bounded: each `read` does at most `n` output bytes plus one block header. No allocation, no recursion. Main loop only; never in the render or an ISR. Every stream it reads is made by the build with a window of at most 4 KiB (A2 enforces it); a stream that asks for a longer distance is `failed()`.

Size: about 1.4 KB of code, paid once by the first slice that links it (A3).

### 3.3 One encoder: `tools/deflate_pack.py`

Deterministic and in the repository (Python standard library only): greedy-lazy LZ77 over a 4 KiB window, dynamic Huffman blocks with lengths limited to 15 (and 7 for the code-length code), a stored block when it is smaller. It does not call Python's zlib to make bytes (F7: its output changes with the zlib library, which would break the reproducible-image gate between machines); it uses `zlib.decompress` only in its self-test. Target: within 4 % of zlib level 9 on the assets above. A small `emit_cpp` helper writes a word-aligned `const` array plus `raw_size` and `z_size`.

### 3.4 Consumers (phase 1)

- **Built-in modules (A3):** `o3m_embed.py` keeps each file's 64-byte header raw (so `builtin_header` and `prefers_card` still read it in place) and deflates the rest. `BuiltinModule` gains `z` and `z_size`; `size` stays the file's size. The loader's `read_some` for a built-in reads the header from `data` and then from its own `Inflater` (held in the loader: a load is stepped across passes, so it cannot share a decoder). The `.tmod` CRC and every loader check run as before on the inflated bytes.
- **Faces (A4):** the font generator emits each face compressed (the icon face stays raw); `FontFace::bitmap` becomes `z` / `z_size` / `bitmap_bytes`, and `face_bitmap(Font, uint8_t* dst, size_t cap)` inflates a face for the few readers (the tile composer, the simulator's gallery, tests). `upload_assets` inflates each face through the staging chunk (4 KB at most per upload, word-aligned as now); the tiles of a face are composed from a 4 KiB face scratch (the 10x20 face is 3,800 B, the largest the tiles use).
- **Boot logo (A5):** `logo_convert.py` emits the compressed logo (the committed generated file stays committed, and `logo_convert_fresh` still regenerates and compares it); `boot_logo()` returns `z`, `z_size`, `size`; the upload inflates it like a face.
- **Help (A6, A7):** the pages move from the C++ table to a text file `app/help/pages.txt` (format in A6) that `tools/help_pack.py` packs into one stream plus a page index (offset of each page in the inflated text). `help_page(index)` keeps its signature: it inflates the page into a 4 KiB page buffer (text plus a 64-entry `HelpLine` table) only when the index differs from the last call (a one-entry cache); the decode skips to the page's offset. Worst case (the last page) inflates the whole 19 KB.
- **RAM:** one `Inflater` shared by the UI assets and the help (both decode synchronously in one call, never across passes): 5.3 KiB; the face scratch 4 KiB; the help page buffer 4 KiB; all three in SDRAM_STATIC (`O3OS_SDRAM_NOINIT`), +13.3 KiB (160,384 -> about 174,000 of 196,608 B). The loader's `Inflater` lives in the module system object (64 KiB block in the module region) if `sizeof(ModuleSystem)` leaves room (its static_assert says), else in SDRAM_STATIC (+5.3 KiB more).
- **Time:** inflating is about 20 to 40 cycles a byte: boot +2 to 5 ms (21 KB of assets), a built-in module load +1 to 3 ms, a help page change at most about 4 ms. Render CPU: 0.

### 3.5 Template and helper duplication (T1, T2)

`pe::edit_selection` becomes one out-of-line function taking `void (*)(model::Step&, const Rule&, Last&, void*)` and a context pointer; each call site keeps a captureless lambda as the thunk. `append_uint`, `append_dec`, `put_dec` and the anonymous `say()` copies become one out-of-line function each over `(char* buf, size_t cap, size_t& len, ...)`; the `FixedString<N>` wrappers stay one-line inline calls (as `FixedString::append` already is).

### 3.6 Importers on the card (phase 2)

- **The module:** one importer module `tracker.tmod` (kind 3) that serves `mod`, `s3m` and `med`. It is built from the same sources as today (`formats/mod`, `formats/med` and the model code they call), so the import's result is the same code's result. A small module libc shim (`memcpy`, `memset`, `memmove`, `memcmp`, `strlen` in the module itself; the packer refuses undefined symbols) and the host's read function are all it needs beyond today's code; I0 proves it builds and lists what had to change.
- **The ABI (Q1 a):** kind 3 is accepted from the next free ABI minor (1.10 is the latest today; minors are claimed in merge order). The descriptor gains an importer table (read only for kind 3): the formats it serves (`"mod s3m med"`), `begin(state, host, format, model_arena, options, options_size)`, `step(state, budget)`, `read_pcm(state, slot, first, n, dst)`, `report(state, out, size)`, `close(state)`. The host table: `read(ctx, offset, dst, n)` and `size(ctx)` over the opened file (the random-access window the app already has). The **layout tag** is a 32-bit hash of the model layout the importer writes (`sizeof` and `offsetof` of `ModelArena`, `Project`, `Instrument`, `Pattern`, `Step` and the import report, plus a manual version number), computed by one header both sides compile (`model/include/o3os/model/import_layout.h`). For kind 3 it sits in the file header's free word at offset 44 (`reserved2[1]`, 0 for every other kind, as now), so the OS can refuse a mismatch from the 64-byte header alone, before anything is placed or reset. The SDK documents kind 3 as first-party and version-locked.
- **The OS side:** `ModuleImport::probe` and `decide` stay in the image (with `mod::sniff_kind` and the MMD check), so the File screen's IMPORT page offers the same files. `ModuleImport::begin` (which today only opens the file, before Settle, with the document live) also opens `modules/importers/tracker.tmod` and checks its header: present, kind 3, ABI within the OS's, layout tag equal. Any refusal there is a failure at `begin()`, so the document stays as it was. The first `step()`, after Settle, resets the document as today, unloads the old project's SD modules if they are still loaded (the document is empty from here; the SD-module phase after the samples loads the new project's set), loads the importer into the module region (image about 45 to 50 KB, state about 40 KB of the 704 KiB) and runs it exactly as today's members. The PCM phase asks the module's `read_pcm`; `close()` unloads it. The report page and the strip are unchanged.
- **Without the file (new Community OS behaviour):** the user sees a clear failure and nothing worse. Opening a MOD, S3M or MED with no `modules/importers/tracker.tmod` on the card fails at `begin()`; nothing is reset and nothing is lost. The strip says `IMPORT X.MOD FAILED: NO IMPORTER`. The REPORT page says `FAILED    NO IMPORTER`, then `NEEDS MODULES/IMPORTERS/TRACKER.TMOD` and `FROM THE STARTER ZIP OF THIS RELEASE` (each line within the page's 46 characters). A file of another firmware (wrong kind, ABI or layout tag) fails at `begin()` the same way with `IMPORTER FOR OTHER FIRMWARE` and the same two lines. The two failures that can only come after the reset (no room in the module region, a CRC or relocation error while loading) leave the document empty, as any failure after the first step does today, and say `NO MODULE MEMORY` or `IMPORTER DAMAGED`. Projects already imported and saved (`.o3pj`) need nothing, because the MED tick processor and the Amiga voices stay in the core.
- **Shipping:**
  - `starter_manifest.json` gains `modules.importers: ["tracker"]`.
  - `make_starter.py` copies the file to `modules/importers/tracker.tmod`, next to `modules/apps/`.
  - The manual's install and File pages say so.
  - The release notes of the first release without the linked importers tell users to copy the file.
- **Memory:** `g_module` (40,704 B of SDRAM_STATIC) shrinks to the proxy (about 1 KB).

### 3.7 Bench image (B1)

`-DO3OS_BENCH_ARCHIVE=ON` (default OFF) keeps the retired one-off A/B benches: `cpuopt` (OS-CPUOPT1), `looper` (OS-LOOPER L2b), `voice_st` (OS-STEREO step 1) and `disk` (OS-USBDISK U1d). Without it the console still lists them and answers `not in this image (O3OS_BENCH_ARCHIVE)`, as a release image answers for `bench`. The kernels every CPU measurement uses (`kernels`, `voice*`, `insert`, `tb303`, `reverb`, `delay`, `limiter`, `dj`, `stages`, `burst`, `worst`, `on`) and `diag cache check` (a general tool of the `rt-optimize` skill) stay.

### 3.8 The budget after each phase (content bytes, estimates)

| Image | Now free | After phase 1 | After the approved specs (65 KiB) | After phase 2 |
|---|---|---|---|---|
| Release | 56 KiB | about 107 KiB | about 42 KiB | about 77 KiB |
| Bench | 24 KiB | about 86 KiB (B1 included) | about 21 KiB | about 57 KiB |

## 4. Slices

| # | Slice | Size | Level | Flash (est.) | Depends on |
|---|---|---|---|---|---|
| A1 | `core::Inflater` and its host tests | S | B | 0 (not linked yet; +1.4 KB with A3) | — |
| A2 | `tools/deflate_pack.py`, deterministic encoder, self-test | S | B | 0 | — |
| A3 | Built-in `.tmod` bytes compressed | S | B | -12.7 KB | A1, A2 |
| A4 | Glyph faces compressed | S | B | -9.7 KB | A1, A2 |
| A5 | Boot logo compressed | S | B | -3.4 KB | A4 |
| A6 | Help pages as a packed text file, parity test (old table still live) | S | B | 0 | A1, A2 |
| A7 | `help_page()` served from the packed stream; old table removed | S | B | -14.4 KB | A6 |
| T1 | One `edit_selection` | S | B | -5.8 KB | — |
| T2 | Non-template text helpers | S | B | -4.5 KB | — |
| T3 | No soft double: two conversions | S | C | -2.0 KB | — |
| B1 | `O3OS_BENCH_ARCHIVE` | S | B | bench image -11 to -13 KB | — |
| I0 | Spike: the importer builds as `tracker.tmod` | S | B | 0 | — |
| I1 | ABI: importer kind 3, layout tag, loader, SDK | S | C | +1.5 KB | I0 |
| I2 | `ModuleImport` through the module; missing-file messages; equality tests | S | C | +1.5 KB | I1 |
| I3 | The image stops linking the importers; starter zip; manual | S | C | -40.6 KB | I2 |

Proof for every slice: the content bytes of both images before and after (`arm-none-eabi-size -A -d o3os.elf`: `.text` + `.ARM.exidx` + `.init_array` + `.ramfunc_l` + `.data`; release and `O3OS_BENCH=ON`), the SRAM_L / SRAM_U / SDRAM_STATIC lines of `--print-memory-usage`, `tools/ci.sh host` green with no test changed except the ones the slice names, and `mk66` / `mk66-bench` builds warning-free. No slice touches render code except T3. QA rows and manual pages: none (nothing visible changes), except I2 (QA-FIL-139 to 141) and I3 (`docs/manual/file.md`, `docs/manual/install.md`).

**A1 — `core::Inflater`** (S, Level B). Files: `core/include/o3os/core/inflate.h`, `core/src/inflate.cpp` (new), `core/CMakeLists.txt`, `tests/core_inflate_test.cpp` (new), test fixtures generated at configure time by a Python script with `zlib.compress(..., wbits=-12)` (stored, fixed and dynamic blocks; empty input; 1-byte input; 64 KiB of random bytes; a run of 10 KB of one byte; the image's own font bytes if present). Acceptance: 3.2. Host tests: every fixture inflates to its source, byte for byte, through `read` in chunks of 1, 7, 64 and 4096 bytes and through `skip`; truncated, over-long, bad block type, bad Huffman table and distance-over-4-KiB streams end `failed()` with no write past `dst + n` (ASan); `done()` only at exactly `raw_size`. QA rows, manual: none. Budget: flash 0 until a consumer links it, then about 1.4 KB; SRAM 0 (the object lives where its owner puts it).

**A2 — `tools/deflate_pack.py`** (S, Level B, tooling). Files: `tools/deflate_pack.py` (new), a `ctest` that runs `--self-test`, `docs/toolchain.md` (one paragraph: why our own encoder). Acceptance: 3.3; the self-test round-trips random and fixed inputs through `zlib.decompress(data, -12)`, checks that no distance exceeds 4,096 and that two runs give identical bytes, and that the total over a built-in sample (a text, a bitmap, a code blob, all in the test) is within 4 % of zlib level 9. A1's test gains a fixture made by this encoder. Budget: no firmware.

**A3 — Built-in modules compressed** (S, Level B). Files: `tools/o3m_embed.py` (raw header, deflated rest, `raw_size`), `module/include/o3os/module/builtin.h` (`z`, `z_size`; `data` is the header), `module/include/o3os/module/loader.h` and `module/src/loader.cpp` (`read_some` from the `Inflater` after the header; `use_builtin` begins it), `platform/mk66/src/app_main.cpp` (only if the loader's decoder needs a home outside `ModuleSystem`), `tests/module_builtin_test.cpp` (+ a test: each built-in inflates to the byte-exact `.tmod` the build made), the other tests that read `kBuiltinModules[i].data` as a whole file (use a helper). Acceptance: the four modules load and run as before (their host tests, `module_builtin_run_test`, the QEMU goldens unchanged); a card copy still wins only when newer. `tools/ci.sh qemu` before merge (a module path). Unit check: load TB-303, chip, FM and SID on the unit (`mod load`), play the demo, `mod` lists them, no `boot: previous run crashed`. Budget: flash -12.7 KB (bench and release); SDRAM +5.3 KiB in the module system or SDRAM_STATIC; render CPU 0; a load +1 to 3 ms.

**A4 — Glyph faces compressed** (S, Level B). Files: `tools/font_convert.py` (emit each face compressed through A2's helper), `ui/include/o3os/ui/font.h` (`FontFace`: `z`, `z_size`, `bitmap_bytes`; `face_bitmap()`), `ui/src/assets.cpp` (upload through the staging chunk; tiles from the face scratch), `sim/src/gallery.cpp`, `tests/assets_test.cpp`, `tests/upload_test.cpp`, `tests/ui_test.cpp` (read faces through `face_bitmap`). Acceptance: every sim golden and screenshot test byte-identical (no golden regenerated); the RAM_G bytes after `upload_assets` equal the old ones (a host test compares the fake display's RAM_G with the raw faces). Unit check: boot, every screen in each theme looks as on the previous image (spot screenshots with Shift, then Power, compared by the tester), boot time line within +5 ms. The icon face stays raw. Budget: flash -9.7 KB; SDRAM_STATIC +9.3 KiB (the shared decoder and the face scratch); boot +1 to 3 ms.

**A5 — Boot logo compressed** (S, Level B). Files: `tools/logo_convert.py`, `ui/src/boot_logo_data.cpp` (regenerated), `ui/include/o3os/ui/boot_logo.h`, `ui/src/assets.cpp`, `tests/boot_screen_test.cpp`, `tests/boot_screen_pixels_test.cpp`, `tests/display_boot_test.cpp`. Acceptance: `logo_convert_fresh` passes on the regenerated file; the boot screen goldens are byte-identical. Unit check: the logo shows at boot as before. Budget: flash -3.4 KB.

**A6 — Help pages as a packed text** (S, Level B). Files: `app/help/pages.txt` (new; made once from `help_text.cpp` by a throw-away script; format: `== <title>` starts a page; `| <text>` a paragraph line (key nullptr); an empty line a blank line; `<key>\t<text>` a key line, `<key>\t` a section label, `\t<text>` a continuation), `tools/help_pack.py` (new: parse, validate ASCII and widths as `help_test` does, pack with A2, emit the stream and the page index), `app/CMakeLists.txt` (generate `help_blob.cpp` into the build tree), `app/include/o3os/app/help_blob.h` (`help_blob_page(index, HelpPageBuf&)`), `app/src/help_blob.cpp`, `tests/help_blob_test.cpp`. Acceptance: for every page, `help_blob_page` gives the same title, line count and, line by line, the same key (nullptr against `""` kept apart) and text as `help_page()` from the old table. Nothing live changes. Budget: flash 0 in the image (the blob is not referenced yet; host only).

**A7 — Help served from the packed stream** (S, Level B). Files: `app/src/help_text.cpp` (the tables go; `help_page` and `help_page_of` stay, `help_page` decodes with the one-entry cache), `app/include/o3os/app/help_text.h` (comment: where the text lives, the page buffer's lifetime: valid until a call with another index), `tests/help_blob_test.cpp` (the parity part becomes a golden of the old text, kept as test data), docs: `docs/specs/OS-HELP.md` (one line: the authoring file is `app/help/pages.txt`). Acceptance: `help_test` unchanged and green; the help overlay goldens byte-identical. Unit check: open the help on Pattern, Song and Mixer and turn three pages. Budget: flash -14.4 KB; SDRAM_STATIC +4 KiB (page buffer).

**T1 — One `edit_selection`** (S, Level B). Files: `app/src/pattern_edit_util.h`, a new `app/src/pattern_edit_util.cpp` (or the existing step-actions file), the call sites in `app/src/step_actions.cpp` and `app/src/step_popup_actions.cpp`. Acceptance: no behaviour change: the pattern edit and step popup tests and their sim goldens unchanged. Budget: flash -5.8 KB; CPU: one indirect call a step of a selection (main loop).

**T2 — Non-template text helpers** (S, Level B). Files: `app/include/o3os/app/text_format.h`, a new `app/src/text_format.cpp`, the anonymous `say()` copies that are identical (one shared helper in an existing `app` file; a copy that does something else, such as `song_map_actions.cpp`'s, stays), `put_dec`. Acceptance: `text_format` tests (or new ones: every width and edge value, a full buffer refused as before) and every sim golden unchanged. Budget: flash -4.5 KB.

**T3 — No soft double** (S, Level C: `move_position` runs in the render). Files: `audio/src/voices.cpp:2466` (clamp the float product `mod * wt_windows` to ±2 × `wt_windows` in float, then convert through `int32_t`: the clamp to `[0, last]` that follows makes the window identical to today's for every finite `mod`), `audio/src/app_tap.cpp:47` (the uint64 sum as two 32-bit halves), host tests (`tests/audio_*`: the window index equals the old expression over a sweep of `mod` and window counts; the RMS within 1 ulp of the old value). Acceptance: the map no longer lists `_arm_muldf3`, `_arm_addsubdf3`, `_arm_addsubsf3`; render code otherwise unchanged; the audio property tests green. Risk review plus one review (Level C), `tools/ci.sh qemu`. Unit check: `diag bench voice` and a wavetable position-modulation run on the unit, cycles equal or lower, 0 underruns. Budget: flash -2.0 KB; render CPU 0 or lower.

**B1 — `O3OS_BENCH_ARCHIVE`** (S, Level B, tester tooling). Files: `platform/mk66/CMakeLists.txt` (option, definition), `platform/mk66/src/bench.cpp` (the four groups under `#if O3OS_BENCH_ARCHIVE`), `platform/mk66/include/o3os/platform/mk66/bench_report.h` (the kernel list says which need the option), the console's answer, `tools/ci.sh` (the `mk66-bench` build keeps building one image with the archive ON so it does not rot), `docs/architecture/cpu-budget.md` (one line). Acceptance: the bench image without the option links and answers the four with the message; with it, as today; `diag cache check` and every other kernel unchanged. Budget: bench image -11 to -13 KB; release 0.

**I0 — Spike: the importer as a module** (S, Level B; no image change). Files: `modules/importers/tracker/` (new: `tracker.cpp` entry, `module.json`, the libc shim), `modules/CMakeLists.txt` (`o3m_add_module(tracker ...)` compiling the `formats/mod` and `formats/med` sources and the model sources they need), whatever the packer refuses inside those sources, fixed without behaviour change (for example a writable static turned const, a global constructor removed), each fix covered by the existing format tests. Acceptance: `tracker.tmod` packs; record in this spec's state section its image size, state size, relocations and any symbol the shim had to provide; host `.so` builds. Kind 3 is not accepted yet, so nothing loads it. Budget: no image change.

**I1 — Importer kind** (S, Level C: published ABI). Files: `module/sdk/o3module.h` (the importer table, kind 3 from the next free minor, the layout tag in header word 44 for kind 3 only, the version-lock comment), `tools/o3m_pack.py` (writes the tag for kind 3; still 0 for every other kind), `module/sdk/README.md` (a section: first-party, version-locked, what the host lends), `model/include/o3os/model/import_layout.h` (new: the tag), `module/include/o3os/module/image.h` and `module/src/image.cpp` or `loader.cpp` (accept kind 3 with a valid table; refuse `LayoutMismatch`; a header-only check function the app can call before anything is placed; an importer never binds to a track and does not count against `kMaxModules`), `tests/support/module_blob.h` (a kind 3 test blob and a host fake of the read table), `tests/module_importer_test.cpp` (new). Acceptance: a 1.x module of any other kind loads unchanged, and a non-zero word 44 in one is still refused; a kind 3 file with a wrong tag, a missing function, a format list out of bounds, or a minor above the OS's is refused with its status; the tag changes when a field of `Step` moves (a test with a copy of the struct). Risk review plus one review; `tools/ci.sh qemu`. Budget: flash +1.5 KB.

**I2 — Import through the module** (S, Level C). Files: `app/include/o3os/app/module_import.h`, `app/src/module_import.cpp` (the proxy: the header check at `begin`, the old SD modules unloaded and the importer loaded at the first step after Settle, run, `read_pcm`, unload at `close`; the messages of 3.6), `platform/mk66/src/app_main.cpp` and the simulator's composition root (hand the module set and the path), `tests/module_import_test.cpp` (equality: for each MOD, S3M and MED fixture (the generated ones of `tests/support/modimp_fixture.h` and `mmd_fixture.h`, plus `build-fixtures/` when present), the project written by the native writer after an import through the module equals the one through the linked importer, byte for byte, and so does the PCM of every slot; the missing-file and wrong-tag cases leave the document untouched, the no-memory case leaves it empty, and each gives the strip and report text of 3.6), a QEMU test that imports one fixture through the module and checks the project's hash against the linked importer's. Docs: `docs/qa/catalogue/file.md` rows QA-FIL-139 (no importer: strip and report, document unchanged, sim), QA-FIL-140 (importer for other firmware, sim), QA-FIL-141 (import with the module, the same result as before: unit). Unit check: import `kc_dancinonamiga.mod`, an S3M and the starter's MED with the file; remove it and import again (message, document unchanged); time a large MED import before and after (report; no stall in the strip). Budget: flash +1.5 KB (the linked importers stay until I3).

**I3 — Importers leave the image** (S, Level C). Files: `formats/CMakeLists.txt` (`formats/mod` and `formats/med` into a library `formats_tracker` that the module, the tests and the host registry link, not the image; `mod::sniff_kind` and the two `drop_group_name` tables that `ModuleSummary::group_name` reads move to a small file that stays in `formats`), `formats/src/builtin_formats.cpp` (host only: links `formats_tracker`; the image never referenced the registry), `app/src/module_import.cpp` (the linked path goes), `platform/mk66/src/app_main.cpp` (`g_module` becomes the proxy), `tools/release/starter_manifest.json` and `tools/release/make_starter.py` (`modules.importers`), `tools/release/README.md`, `tests/starter_test.py`, `docs/manual/file.md` (the import needs `modules/importers/tracker.tmod`, from the starter zip), `docs/manual/install.md` (what the zip puts in `modules/importers/`), `docs/formats/mod.md` and `docs/formats/med.md` (one line each: where the reader runs). Acceptance: the image does not link any `formats::mod` or `formats::med` symbol except `mod::sniff_kind` and the two `drop_group_name` (a ctest over the map); `ModuleImport::decide`'s tests unchanged; I2's equality tests still pass (the linked side now comes from `formats_tracker` in the test build); `tests/starter_test.py` (ctest `starter_python`) checks that the zip holds `modules/importers/tracker.tmod`. Unit check: as I2 with the starter zip's file. Budget: flash -40.6 KB; SDRAM_STATIC -39.7 KB; render CPU 0.

## 5. Order

A1 and A2 first (parallel), then A3, A4, A6 (parallel; A5 after A4, A7 after A6), T1 and T2 at any time, B1 at any time, T3 last in phase 1. Phase 2 (Q5): I0 can run during phase 1; I1, I2, I3 in order.

## 6. Assumptions (decided here; local, testable, reversible)

- A-1 One decoder format, raw deflate with a 4 KiB window, for every compressed asset: deflate came out clearly smaller than LZ4- or LZSS-style formats over these assets (F7), for about 1 KB more decoder.
- A-2 Our own deterministic encoder (A2) instead of Python's zlib: the reproducible-image gate must not depend on the build machine's zlib.
- A-3 No CRC on the compressed blobs: they are in flash, the decoder checks the exact length and the end of the stream, the `.tmod` keeps its own CRC, and host plus QEMU tests run the same decoder over every blob.
- A-4 The MCU inflates, not the FT812's CMD_INFLATE (which ADR 0010 point 8 foresaw for faces, tiles and icons): the tile composer, the help and the module loader need the raw bytes on the MCU anyway, so one decoder serves all of them, and the simulator and the host tests run the same path. CMD_INFLATE would save only the decoder's 1.4 KB.
- A-5 The help's authoring source becomes `app/help/pages.txt`; `help_test` keeps checking the served pages.
- A-6 One importer module for MOD, S3M and MED (`tracker.tmod`): one file for users to copy, one load path, one layout tag.
- A-7 The importer file's header is checked at `begin()` (the document still live), and the importer is loaded only at the first step, after Settle and the reset, when the old project's SD modules can be unloaded; so a missing or mismatched file never costs the document, and the module region is as free as it gets.
- A-8 The retired benches leave the default bench image (B1); the image that needs one is built with the option.
- A-9 Not touched: the `-O3` files (R1), the console's text (R3), the OG readers, the render code (except T3).

## 7. Risks

- R1 (reserve) The `-O3` files at `-O2` would save up to 8.7 KB; each needs its bench row (`reverb`, `delay`, `limiter`, `dj`, `stages`, `looper`) on the unit within 1 % of today's cycles before it is kept.
- R2 (reserve, Q4) LTO: the link must be made to work first (F9), then every CPU bench row and the ramfunc check re-run.
- R3 (reserve) Console usage text: shorten or compress; check the HIL tools do not parse it.
- R4, R5 (reserve) Constructors, packed tables.
- R6 A decoder bug would break the boot screen or a built-in module: covered by A1's fuzz-style tests, the byte-equality tests of A3 to A7 and the QEMU goldens.
- R7 The importer module's code runs from SDRAM (cached through the ICode alias): a large import may be slower; I2 measures it on the unit.
- R8 A user who updates the firmware but not the card after a model layout change gets `IMPORTER FOR OTHER FIRMWARE`: the release notes say to copy the file; the tag only changes when the import-facing layout does.
- R9 SDRAM_STATIC goes from 160 KB to about 174 KB (of 192 KB) in phase 1 (180 KB if the loader's decoder cannot live in the module system); phase 2 gives 40 KB back.
- R10 The module build flags (`-fsingle-precision-constant`, `-ffreestanding`, `-ffp-contract=off`; `tools/o3m_pack.py:438-441`) may change a floating-point result of the importers (tempo and volume maths): I0 lists every `double` and floating constant in the importer sources, and I2's byte-equality test (host and QEMU) catches any difference before the linked importers go.

## 8. Evidence labels

- Every slice: **new Community OS behaviour**, invisible (the same pixels, sounds, imports and help text, proved byte for byte by host and QEMU tests).
- The messages of a missing or mismatched importer (3.6): **new Community OS behaviour** (the MOD / S3M / MED import is a Community OS feature; no parity is claimed).
- F9 (LTO), the inflate time on the unit (3.4) and the importer's speed from SDRAM (R7): **open research question** until measured.

## 9. State for implementer

- Measure (every slice): `cmake -S . -B build-x -DO3OS_PLATFORM=mk66 -DCMAKE_TOOLCHAIN_FILE=cmake/mk66-toolchain.cmake -DARM_GNU_ROOT=<the toolchain of docs/toolchain.md> -DCMAKE_BUILD_TYPE=MinSizeRel -DO3OS_RATE_44100=ON [-DO3OS_BENCH=ON]`, build `o3os`, then `arm-none-eabi-size -A -d platform/mk66/o3os.elf` and add `.text`, `.ARM.exidx`, `.init_array`, `.ramfunc_l`, `.data`. Baseline (788b0738): release 988,692 B, bench 1,022,028 B.
- Rules that hold: no change to the layout of any existing `.tmod` kind, `.o3pj`, `.o3st` or OG file (I1's header word is defined for kind 3 only, under its new ABI minor); no generated golden regenerated (A4, A5, A7 must be byte-identical); decoders run in the main loop only, synchronously, except the module loader's own instance; nothing new in the render, an ISR or SRAM_L; no Python zlib bytes in the image.
- A1. Start with: `core/include/o3os/core/fixed_string.h` (the style of a core header), `core/CMakeLists.txt` (sources list), `tests/CMakeLists.txt` (how a test and a configure-time fixture are added), RFC 1951 sections 3.2.3 to 3.2.7.
- A2. Start with: `tools/icon_convert.py:300-345` (a generator's `--self-test` and argparse pattern), `tools/logo_convert.py:439` (`emit_cpp`, a word-aligned array), `docs/toolchain.md`.
- A3. Start with: `tools/o3m_embed.py:23-52` (`generate`), `module/include/o3os/module/builtin.h:18-46`, `module/src/loader.cpp:191-205` (`use_builtin`) and `:234-245` (`read_some`), `platform/mk66/src/app_main.cpp:750` (`ModuleSystem`, its 64 KiB static_assert).
  - Unit check: the four built-ins load and play.
- A4. Start with: `ui/src/assets.cpp:26-57` (`upload_flash`, `upload_small_set`, `compose_tile`) and `:136-206` (`upload_assets`), `ui/include/o3os/ui/font.h:64-79`, `tools/font_convert.py:145-218` (`layout`, `emit_cpp`), `sim/src/gallery.cpp:80-95`.
  - Unit check: screens and boot time.
- A5. Start with: `tools/logo_convert.py:439` (`emit_cpp`), `ui/src/boot_logo_data.cpp:24` and `:515`, `ui/CMakeLists.txt` (the `logo_convert_fresh` note).
  - Unit check: the boot logo.
- A6, A7. Start with: `app/include/o3os/app/help_text.h:1-37`, `app/src/help_text.cpp:1-60` and `:480-500` (`kPages`, `help_page`), `tests/help_test.cpp:90-100` and `:520-570`, `app/src/screens/help_overlay.cpp:39-75`.
  - Unit check: the help on Pattern, Song and Mixer, three pages each (A7).
- T1. Start with: `app/src/pattern_edit_util.h:60-130` (`edit_selection` at :67), `app/src/step_actions.cpp:50-70`, `app/src/step_popup_actions.cpp:110-130`.
  - Unit check: none beyond the screens (host tests and goldens prove it).
- T2. Start with: `app/include/o3os/app/text_format.h:18-60` (`put_dec`, `append_dec`, `append_uint`), `app/src/pattern_edit_util.h:23` (the shared `say`) and the file-local copies (`grep -n "bool say(ActionContext" app/src`).
- T3. Start with: `audio/src/voices.cpp:2458-2475` (`move_position`), `audio/src/app_tap.cpp:43-48`, the map's "Archive member included" lines for `_fixsfdi.o` and `_arm_addsubsf3.o`.
  - Unit check: bench voice and a wavetable run.
- B1. Start with: `platform/mk66/CMakeLists.txt:113-121`, `platform/mk66/src/bench.cpp:488-565` (`voice_st`), `:919-1256` (`cpuopt`, `looper`), `:1537-1725` (`diskbench` and its USB counters), `:1735-1750` (the dispatch), `platform/mk66/include/o3os/platform/mk66/bench_report.h:24-55`.
  - Unit check: the bench image answers the four archived kernels with the message; `diag bench all` otherwise as before.
- I0. Start with: `modules/CMakeLists.txt:56-145` (`o3m_add_module`), `tools/o3m_pack.py:19` and `:438-443` (what is refused, the flags), `formats/CMakeLists.txt:17-40`, `formats/mod/include/o3os/formats/mod/import.h:110` (`Importer`) and `formats/med/include/o3os/formats/med/import.h:36`. Record here: image, state, relocations, shim symbols.
- I1. Start with: `module/sdk/o3module.h:95-110` (kinds) and `:750-840` (descriptor; the file header, `reserved2[1]` at offset 44), `module/include/o3os/module/image.h:52-100`, `module/src/image.cpp:174` (where `reserved2[1]` is refused today), `module/sdk/README.md` section 10.
- I2, I3. Start with: `app/include/o3os/app/module_import.h:6-20` (the phases) and `:80-160`, `app/src/module_import.cpp:278-320` (`probe`, `begin`) and `:128-145` (the strip), `platform/mk66/src/app_main.cpp:1105` and `:1169`, `formats/mod/src/module.cpp:91` (`sniff_kind`), `tools/release/starter_manifest.json:4`, `tools/release/make_starter.py:140-142` and `:267-283` (`collect_modules`).
  - Unit check: imports with and without the file.
- Report per slice: the two images' content bytes before and after, SRAM_L, SRAM_U, SDRAM_STATIC, the tests run, and the unit check left.
