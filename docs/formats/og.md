# The Tracker OG files: import and Export -> OG

Status: implemented with OS-004b (revised after review: OGPM and OGPN, the PAMD gate) and OS-004c (project v17 of official 1.9.2, the Export target), 2026-10-11 · Decisions: ADR 0004 (step format, FX ranges, unknown kept verbatim), ADR 0006 + amendment 1 (open everything up to official 1.9.2; versions newer than known load as "unverified", never misread), ADR 0007 (native format), ADR 0009 rev 1 (formats as Strategy + registry, resumable), ADR 0011 (declared layout) · Orchestrator decision of 2026-10-11 on the volume column and per-track length (section 4) · Project v17 (official 1.9.2): open research question, unverified, no file backs it (sections 2 and 10).

Code: `formats/include/o3os/formats/{og,format,builtin,progress}.h`, `formats/src/og_*.cpp`, `format_registry.cpp`, `builtin_formats.cpp`. Tests: `tests/og_*_test.cpp` (`og_v17_test.cpp` for 1.9.2), `format_registry_test.cpp`, `support/og_fixture.h` (synthetic OG files written from the byte maps, sharing no code with the readers). The byte maps (project.mt, patterns, .mti, .pti, PAMD, folder layout, FX) are the sections below. The native side is `docs/formats/o3-project.md`.

## 1. What an import is

An OG project is a folder. An import is one small read per file into one project, in this order, each a resumable `FormatReader` (`begin(source, arena, params)`, `step(budget)`, `report()`): no file I/O, no heap, nothing that blocks, any input ends in `Done` or a typed error.

| Step | File | Reader | What it does |
|---|---|---|---|
| 1 | `project.mt` | `og::ProjectReader` | resets the arena's project; name, tempo, default pattern length, song, 16 track slots, master section; creates the 16 MIDI instruments (slots 49-64); keeps the settings the model has no field for in an `OGPM` chunk |
| 2 | `pattern_NN.mtp` | `og::PatternReader` | adds pattern `NN` (1..255); `params.index` = NN |
| 3 | `instrument_NN.mti`, `NN name.pti`, `instrument_NN.pti` | `og::InstrumentReader` (mti / pti) | sets sample instrument slot `NN - 1`; `params.index` = slot |
| 4 | `patterns/patternsMetadata` | `og::PamdReader` | names the patterns that exist; keeps the file verbatim in an `OGPN` chunk |

`og::classify_file_name()` recognises the files by name (case-insensitive, any leading path): `project.mt`, `pattern_NN.mtp` (1..255), `instrument_NN.mti` (1..48), `NN name.pti` (1-3 digits, a space, anything; 1..48), `instrument_NN.pti` (the Workspace name of firmware 1.5+), the pattern-names metadata; everything else, macOS `._` files included, is none.

A file that fails leaves the project as it was: the project reader resets it (valid and empty, as the native reader does), the pattern reader deletes what it added, the instrument reader sets the slot only at the end, and the PAMD reader reads and checks the whole file (12,816 bytes at most, staged in the reader) before it applies anything. `.mt` files and the like that start with `._` (macOS AppleDouble) are never recognised, by name or by bytes. A pattern whose id is taken, or a full pool (128), is an error, never a replacement.

The `.pti` PCM is not read into anything. The report says `pcm_frames`; the platform copies the PCM (file offset 392, mono signed 16-bit little-endian, 44.1 kHz) into the WAV `samples/instrNN.wav` that the instrument's `sample_path` names. `params.size_hint` (the file size, when the platform knows it) lets the reader skip the PCM instead of reading it through.

Strategy and registry: `formats::FormatReader` / `FormatWriter` (virtual at format granularity), `FormatRegistry` (16 entries) finds a format by id, by file name (extension or whole name) or by sniffed magic (`identify`: the name decides when it is known and the bytes must agree). `register_builtin_formats()` registers `native` (`.o3pj`, provisional), `og-project` (`.mt`), `og-pattern` (`.mtp`), `og-pti` (`.pti`), `og-mti` (`.mti`, read only), `og-pamd` (`patternsmetadata`); `og-pti` comes before `og-mti` so that bytes of an instrument file of unknown size are taken for a `.pti`, never an `.mti`. Their strategy objects live in a `BuiltinFormats` the caller owns (29.1 KB on the MK66, of which the PAMD reader's staging buffer is 12.8 KB). Through `FormatReport` alone (no downcast) a job reads `sanitised`, `dropped_groups`, `kept_groups` and `pcm_frames`. MOD, MED, XM, IT and MIDI add descriptors to the same registry.

## 2. Versions

"Verified" means the layout is backed by fixture files (real files of the official firmware, kept outside the repository; section 10). **"From the source"** is the class of layouts read on the cited published source of tracker-ce (`tracker-ce@7fd6c2c`, cited as facts, no code taken) with no fixture behind them yet: pattern v5 at 12,336 B (16 tracks) and pattern v6 at 8,232 / 12,332 / 16,432 B (OS-CEPAT S1/S1b). They are read as known on that evidence, and the evidence covers v6 itself, not only v5: tracker-ce's writer stamps all four structure-version bytes with `PATTERN_FILE_VERSION` 6 and the declared size with `sizeof(strPattern)` — the 16-track v6 file of 16,432 B — and writes the CRC over everything before it (`core/source/mtStructs.h:44`, `core/source/mtSequencer_patternEdit.cpp:1367-1385`, `core/FileManager/fileManagerPattern.cpp:276-283`), while its validation accepts exactly those v6 sizes and the three v5 ones (`core/include/patternFileValidation.h:9-15, 24-59`). So `kPatternVerified` is 6 on the source's word; the layouts are never guessed at, and the S3 fixture survey moves them to verified when a real file agrees. The "observed only" class (the `ImportReport::observed_only` flag; the name is historical, nothing in the class was observed on a file) is the one class between, and nothing in it is backed by a file: **open research question (unverified)**. The flag is set for project v17 only, the layout assumed for official 1.9.2 (open research question): its version, its size (2,324 B) and the offsets of section 3 are not backed by any file, and a v17 file is read like a verified one (never `unverified_version`; a CRC that does not match is only warned about) as a deliberate policy, until a real file confirms or corrects it. Project v4, v5 and v6 sit in the same column of the table (no file of them either: open research question, unverified) and are read as known versions, without the flag. Everything else follows ADR 0006 amendment 1: read as far as the layout is known to be unchanged, set `unverified_version` (the UI tells the user; the source is read-only: the OS never writes into OG files anyway), never guess, and stop at the first structure whose size or version is unknown.

| File | Read, verified (fixture) | Read, `observed_only` class (no fixture; open research question, unverified) | Refused | Unverified (loads, flagged) |
|---|---|---|---|---|
| project.mt | v2 1,084 B, v3 1,100 B, v7 1,284 B (1.2), v11-v14 1,572 B (1.3-1.6), v16 2,320 B (1.9.0) | v4 1,268 B, v5 1,280 B and v6 1,284 B (no file of any of them is in the repository: open research question, unverified); v17 2,324 B (1.9.2): v16 with four bytes inserted at 0x80C, name at 0x810, CRC at 0x910 (section 3); open research question (unverified) | v0, v1 (an unreadable layout), a known version at a length other than its own (`Truncated` / `Malformed`: a v17 header on 2,320 B, a v16 header on 2,324 B) | v8-v10, v15 (never seen), v18+: only the part common to every version (below), name and tracks 9-16 left empty; the CRC must match |
| pattern .mtp | v1-v5 at 6,184 B (8 tracks) or 9,260 B (12 tracks, 1.9); fixtures: v1, v3, v4 at 6,184 and v5 at 9,260 (v2 has none); magic `KS` or `PM`; the type field is not judged. **From the source** (no fixture; OS-CEPAT S1b): v5 at 12,336 B (16 tracks, tracker-ce's, `tracker-ce@7fd6c2c:core/include/patternFileValidation.h:12, 26-29`) and v6 at 8,232 / 12,332 / 16,432 B (8, 12 or 16 tracks, the declared size at offset 12 gives the layout; `tracker-ce@7fd6c2c:core/source/mtSequencer.h:282-336`, sizes at `core/include/patternFileValidation.h:13-15`) | none (1.9.2 writes the same pattern as 1.9.0) | v0, other lengths (a v6 version byte over a v5 stream fails on the framing or the CRC) | v7+ at a v6 size: read with the v6 layout, flagged `unverified_version` (ADR 0006 amendment 1) |
| instrument .mti | v3 at 388 B, v4-v9 at 392 B, CRC enforced | v1-v2 at 388 B (play-mode shift, fine-tune envelope; no fixture: open research question, unverified) | v0 | v10+ at 392 B, CRC must match |
| instrument .pti | v4-v9, 392 B + mono PCM, CRC only warned about (`crc_mismatch`) | none | v0, v1-v3 (a `.pti` never had the 388-byte layout), stereo or inconsistent PCM | v10+, CRC must match |
| PAMD | v1 | none | v0 | v2+: no names are read (the record layout is unknown) |

**Strictness is OS policy, not OG parity.** How strictly the official firmware checks what it opens (a newer version, the CRC of a project, instrument or pattern, a pattern's magic and type, the PAMD version) is an open research question: no test on the unit. The OS's "CRC must match" for unverified versions, the `.mti` CRC enforcement and "unverified, flagged" are deliberate extra care by the OS (new Community OS behaviour), and are not wrong.

The part of project.mt common to every version, read for an unverified one, is `[0x10, 0x4DE)` (playlist, mutes, tempo, pattern length, names of tracks 1-8, volumes) plus `0x4D8-0x4DD` (reverb and delay volumes, mutes, dry volume); the fixtures show it unchanged from v7 to v16 (that v17 keeps it is an open research question, unverified). Where the name sits in a layout nobody has seen (v8-v10, v15, v18+) is not known, so it is left to the folder.

## 3. project.mt

### OG -> model

| OG field (file offset) | Versions | Model | Rule |
|---|---|---|---|
| song playlist, 255 entries (0x10) | all | song order | up to the first 0; entries behind it are ignored and counted in `sanitised` |
| tempo, f32 (0x1C0) | all | tempo (BPM x 100) | `round(t x 100)`, clamped to 40.00..800.00 (counted); not finite, below 10 or above 1000: 130.00 (counted) |
| default pattern length, u16 (0x1C4) | all | default pattern length | 1..128, else 32 (counted) |
| track mutes, 8 bytes (0x12C) | all | slots 1-8 mute flag | any non-zero byte |
| track names, 8 x 21 bytes (0x428) | v4+ | slots 1-8 name | to the first NUL, 20 characters at most; v2, v3: "Track n" |
| track volumes, 8 bytes (0x4D0) | v5+ | slots 1-8 volume | above 100: 100 (counted); before v5: 100 |
| reverb volume, delay volume (0x4D8, 0x4D9) | v5+ | master reverb, delay volume | as above; before v5: 100 |
| reverb mute, delay mute, dry mix mute (0x4DA-0x4DC), dry mix volume (0x4DD) | v6+ | master flags, dry volume | non-zero is mute; before v6: unmuted, 100 |
| project name (32 B at 0x418 v2, 0x428 v3, 0x4D0 v4, 0x4DC v5, 0x4E0 v6-7, 0x600 v11-14, 0x80C v16, 0x810 v17) | known | project name | to the first NUL, 31 characters at most; v17's NUL is at 0x830 and its second string (221 characters at 0x831; open research question, unverified: no v17 file exists, so it is assumed to be uninitialised memory like the 224 bytes behind a v16 name, which the v16 fixtures hold as such) is not read: the model has no field for it |
| names of tracks 9-16 (8 x 8 B at 0x603) | v16, v17 | slots 9-16 name | 7 characters at most; the "Midi 9".."Midi 12" names that OG v16 files carry in slots 9-12 become "Track 9".."Track 12" (OS-TRK912, new OS behaviour: the default of the slot only, a typed name is kept); slots 13-16 as they are |
| (none) | | slot type, pan, bus | slots 1-12 Audio (OS-TRK912: the "Midi N" names suggest that slots 9-12 of an OG file are MIDI tracks, an inference and an open research question; here they are ordinary tracks), 13-16 MIDI; pan 50; bus 0. A native `.o3pj` is not touched: a Midi type stored on 9-12 stays |
| (none) | | active tracks | 12 for v16, v17 and unverified v18+, else 8 (OG pattern files hold 8 tracks before the 1.9 layout and 12 in it) |
| (none) | | master volume, limiter flag, swing | 86 (this OS's default; an OG project file has no field for it), limiter on (always on here, BASE-10), 50 |
| (none) | | instruments 49-64 | MIDI instruments for channels 1-16 ("MIDI n"): an OG project has no file for them |

### Settings the model has no field for: the `OGPM` chunk

The model has no field for the audio and configuration settings of an OG project (delay, limiter, reverb, inputs, performance, the 1.3 and 1.7 blocks, MIDI velocity). ADR 0006 asks for OG -> OS -> OG to be lossless for OG-only content, so the project reader keeps them **verbatim in an `OGPM` chunk** of the project (an unknown chunk to the core: the native format keeps and re-emits it, `o3project.py` decodes it for its dump; layout in `docs/formats/o3-project.md`), and Export writes them back. A group is kept only when it differs from the default Export writes anyway (a project of the OS, or an OG project that never touched a group, has no chunk at all). Bits are the `DropGroup` numbers:

| Bit | Group | Bytes carried (file offsets of project.mt) | Present in |
|---|---|---|---|
| 1 | delay | feedback (0x11A), time u16 (0x11C), sync and ping-pong (0x11E): 4 B | v2+ |
| 2 | limiter | attack u16 (0x120), release f32 (0x124), threshold u16 (0x128), bit depth (0x12A): 9 B | v2+ |
| 3 | reverb | size, damping, pre-delay, diffusion, 4 x f32 (0x418): 16 B | v3+ |
| 4 | inputs | source, gains, monitor (0x134-0x139), radio frequency f32 (0x13C): 10 B | v2+ |
| 5 | performance | pages, FX places and values (0x142-0x1BD), selection and state (0x2F5-0x308): 144 B | v2+ |
| 6 | midi velocity | per-channel velocity (0x309, 16 B) | v2-v7 |
| 8 | 1.3-1.6 block | EQ, bass boost and the rest of 0x4DE-0x5FF (290 B: the 16 per-channel MIDI records, the delay filter at 0x5FE...) and the 16 bytes at 0x309 that later versions use (306 B) | v11+ |
| 9 | 1.7-1.9 block | 0x600-0x602 and 0x643-0x80B: 460 B; OGPM 1.1 adds 0x80C of a v17 project: 461 B (below) | v16, v17 |

Not carried, because Export rebuilds or defaults it: **session state** (bit 0: last used note, instrument, volume and FX, current pattern, pad board, old volume, song position, edit step, not-saved flag, the instruments-to-save and patterns-to-save masks), the **pattern bitmask** (bit 7: rebuilt from the patterns), padding between fields, and the 224 uninitialised bytes behind the v16 name.

Details that matter:
- Bytes are carried exactly as the file holds them, floats included; nothing is validated (the engine that uses them sanitises what it reads).
- Versions 11 and 12 kept at 0x4F7 the value that 1.4.0.2 and later keep at 0x309; a pair of fixtures of the same project saved by the two generations shows the value moved, and the reader moves it too: 0x4F7 is cleared in the carried block and the value goes to the first of the 16 relocated bytes.
- The velocities of versions 2-7 (0x309) have no place of their own in v16: Export writes each into byte 1 of its channel's 16-byte record (0x4FD), which is inferred to be the velocity (OQ-5, inferred; a hardware check).
- **The byte at 0x80C of a v17 project** is a field that official 1.9.2 is assumed to have added (open research question, unverified: a one-byte field at the end of the values block, default 0, followed by three padding bytes; what it is for is not known, so the model gets no field for it). It is carried for OG -> OS -> OG like every setting the model has no field for: zero is its default and needs no chunk, and a non-zero value goes into group 9, widened by one byte for v17. The widening is gated by the chunk's version: **OGPM 1.1** (`0x0101`) has group 9 at 461 bytes, the last one being the byte of 0x80C; a chunk that needs no such byte (a v16 project, whose 0x80C is the first character of the name, or a v17 one holding zero there) stays 1.0 with 460, so every chunk of 1.0 is unchanged. Group 9 is the highest group, so the byte is the last of the chunk and an OS-004b reader takes a 1.1 chunk as it takes any newer minor: what it knows is read, the appended byte is skipped. A 1.1 chunk without group 9 is valid (nothing to widen); a 1.1 chunk with group 9 and no byte is not. Export to 1.9.2 writes the byte at 0x80C; to 1.9.0 there is no place for it and it is counted (below).
- An unverified version (v8-v10, v15, v18+) keeps the groups of the common part (delay, limiter, reverb, inputs, performance); the blocks whose layout is not known are dropped.
- `FormatReport::kept_groups` says which groups went into the chunk; `dropped_groups` lists what was not kept: the session state and the bitmask always, the blocks of an unverified layout, and the groups of a chunk the pool had no room for (the model counts the dropped chunk).
- The model's pool of unknown chunks (16 chunks, 16 KiB) holds the chunk: it is at most 970 bytes (969 for 1.0). The OGPN chunk of the PAMD file can take up to 12.8 KB (section 6).

### Model -> OG (version 16, 2,320 bytes, or version 17, 2,324)

**Export target** (`WriteParams::target`, `og::kTargetOg190` = 0, the default, and `og::kTargetOg192` = 1; any other value is `Malformed`, before a byte is written): which generation of official firmware the project file is written for. Only the project file differs; the pattern (v5, 12 tracks), instrument (`.pti` v9) and PAMD (v1) files are the same for both, and their writers ignore the target.

| | Target 1.9.0 (default) | Target 1.9.2 |
|---|---|---|
| Header | `MT`, type 0, firmware 1.9.0.255, version 16 (four bytes), size 2,320 | `MT`, type 0, firmware 1.9.2.255, version 17 (four bytes), size 2,324 |
| Layout | the table below | the same bytes up to 0x80B; 0x80C the new byte (the carried value of OGPM 1.1, else 0), 0x80D-0x80F zero; name (31 characters) at 0x810 with its NUL at 0x830; the second string and its spare byte (0x831-0x90F) zero; CRC-32 of [0, 2320) at 0x910 |
| On an official 1.9.0 | expected to open it (open research question, unverified: no device was tried) | whether it opens a 2,324-byte project, and whether the project name survives, is unknown (open research question, unverified: no device was tried) |
| On an official 1.9.2 | whether the project name survives a 2,320-byte project is unknown (open research question, unverified: no device was tried) | expected to read the name (open research question, unverified) |
| Byte at 0x80C of the project | counted in `ExportDrops::project_settings` (a v16 file has no such field: 0x80C is the name) | written |

"Export writes what official 1.9.x opens" is the intent for either target (open research question, unverified: no device was tried); whether the project name survives on the other device generation is open too. The default is 1.9.0, as before 1.9.2 was known (OS-004c spec); the choice reaches the user as a device-scope setting parameter shown in the Export dialog once OS-002d's parameters have one, and as the API parameter until then. Which generation a given OG device is, and what a v16 file does on 1.9.2 and a v17 on 1.9.0, are hardware checks (ADR 0006 amendment 1). The firmware bytes in the header are the build's own (whether the official firmware reads them is an open research question); Export stamps 1.9.0.1 into patterns and instruments for both targets and 1.9.0.255 / 1.9.2.255 into the project.

The 1.9.0 layout, from the model:

Header: `MT`, type 0, firmware 1.9.0.255, version 16 (four bytes), size 2,320, two zero bytes. From the model: playlist (an empty song is written as pattern 1, this OS's choice), tempo (`x100 / 100.0f`), default pattern length, mutes, names and volumes of slots 1-8, names of slots 9-16 (cut to 7 characters, counted), master reverb, delay and dry volumes and mutes, project name (31 characters), and the pattern bitmask rebuilt from the patterns (so write the project file after the patterns are in the model). The settings groups are written from the project's `OGPM` chunk (the first valid one; an invalid or repeated one is ignored and counted in `settings_ignored`). What the chunk does not carry is written at the value of a fresh 1.9.0 project (the fixtures' values; this OS's choice where no fixture holds one), and `ExportDrops::defaulted_groups` lists every group written that way (the session state always, a settings group the chunk does not hold). Once the audio engine keeps delay, limiter and reverb settings in chunks of its own (OS-005), those take precedence over the `OGPM` values; `write_engine_settings()` in `og_project.cpp` is that hook, and a comment says so. The defaults:

| Offset | Value |
|---|---|
| 0x114 actual pattern | first pattern of the song, or 1 (session state: always this) |
| 0x115 pad board scale | 0 (Chromatic; session state: always this). The byte is the scale in the official order 0..38 (verified parity tracker-ce@7fd6c2c:core/source/scales.h:6-7 and mtStructs.h:846-851): unread by the importer, written 0 by the export (the NOTE chunk holds our scale since OS-PERSIST2; importing this byte is a later item, OS-SCALES28 Q5) |
| 0x116-0x118 pad board | note offset 11, root note 24, max voices 8 (session state: always these) |
| 0x119 old volume | 50 (session state: always this) |
| 0x11A, 0x11C, 0x11E delay | feedback 50, time 500 ms, parameters 0xC3 (ping-pong, sync, rate 3) |
| 0x120, 0x124, 0x128 limiter | attack 100, release 0.512, threshold 30200 |
| 0x12A, 0x12B | bit depth 16 (limiter group); edit step 1 (session state: always this) |
| 0x13C radio | 87.5 (every other input byte 0) |
| 0x142, 0x152 performance | patterns 1 x 8, FX places 1..12 (values 0) |
| 0x309 | 86 (then 15 zeros: the relocated byte of v13+) |
| 0x418 reverb | 0.5, 0.5, 0.5, 0.68 |
| 0x4F8, 0x4F9, 0x4FC | 3, 1, 1 |
| 0x4FD, 16 records of 16 B | `k 7F 01 05 07 0B 40 80` and 8 zeros (80/80 records of the fixtures) |
| 0x5FE | 20000 (delay filter, Hz) |
| every other byte | 0 (including the 224 bytes behind the name, which the fixtures hold as uninitialised memory) |

## 4. Patterns

A pattern file is a 28-byte head (header 16, tempo f32, swing f32, 4 reserved bytes), track rows, and the CRC-32 (zlib, little-endian) of everything before it, enforced. Versions 1 to 5 hold 8, 12 or 16 rows of 769 bytes (a length byte, then 128 steps of 6 bytes): 6,184 / 9,260 / 12,336 B. Which one it is shows only in the stream (for v ≤ 5 the declared size of the head is not judged): at the end of the eighth row, four bytes then the end of the source mean 8 tracks, five more bytes a ninth row; at the end of the twelfth row, four bytes then the end mean 12 tracks, a 13th row means 16 (OS-CEPAT S1b; the 16-track v5 is tracker-ce's, read from the source: `tracker-ce@7fd6c2c:core/include/patternFileValidation.h:12, 26-29` — new Community OS behaviour: the stream detection extended past row 12). Version 6 (tracker-ce, OS-CEPAT S1) holds 8, 12 or 16 rows of 1,025 bytes (a length byte, then 128 steps of 8 bytes) and says its track count in the declared size at offset 12 of the head: 8,232 / 12,332 / 16,432 B (from the source: `tracker-ce@7fd6c2c:core/source/mtSequencer.h:282-336`, the 8-byte step at `:305-316`; the sizes at `core/include/patternFileValidation.h:13-15`). A v7+ file of a v6 size is read with the v6 layout and flagged `unverified_version`. Tracks 13-16 of a 16-track file go into the model's tracks 13-16; tracks a file does not hold take the first track's length (verified parity for the track-count rule: `tracker-ce@7fd6c2c:core/FileManager/fileManagerPattern.cpp:561-591`).

**Leniency where tracker-ce is strict (new Community OS behaviour).** For v ≤ 5 the declared size of the head is ignored and the stream decides, so a v5 file carrying a v6 size in its head (say 16,432 over a 9,260-byte stream) loads; tracker-ce refuses that file, because it validates the declared size against the version's own size list (`tracker-ce@7fd6c2c:core/include/patternFileValidation.h:24-33`, refused at `:41`). The reverse — a v6 version byte over a v5 stream — is refused here (the 1,025-byte rows do not fit the stream; the framing or the CRC fails). tracker-ce accepts a v ≤ 5 file without a CRC when its length + 4 equals the declared size (`patternFileValidation.h:52-56`); this OS keeps refusing one (open question, no such file is known). tracker-ce writes the CRC but never checks it on load (`core/FileManager/fileManagerPattern.cpp:276-283, 302-316`); this OS enforces it for every pattern version (OS policy, section 9).

**Track length.** The OG's length byte is the index of the last step (31 = 32 steps). The model's per-track length (OS-004a) is `byte + 1`, one to one, per track; a byte above 127 refuses the file. Steps past the length are kept (OG files hold them: PAR-003). Tracks the file does not hold take the first track's length.

**Steps** (OG six bytes -> model seven; a bijection except where noted, so nothing is lost):

| OG | Model | Rule |
|---|---|---|
| note i8 | note | -1 empty -> 0; 0..127 -> 1..128; -2 fade -> 253; -3 cut -> 254; -4 off -> 255; -5..-128 -> 252..129 (kept, "reserved") |
| instrument u8 | instr | slot + 1 (0..63 -> 1..64, MIDI channels at 48..63; 64..254 are kept as 65..255); on empty, off, cut and fade the byte 0 means none (0); 255 reads as none |
| (none, v1..v5) | vol | 255, "not set": **an OG v1-v5 step has no volume byte; this OS carries the volume as FX 18** (maintainer decision): the two FX columns stay verbatim, so FX 18 still shows where the author put it |
| volume i8 (v6 byte 6) | vol | negative -> 255 unset (verified parity: -1 is none, `tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:477`); 0..100 -> `floor(v x 127 / 100)` (verified parity for MIDI velocity: tracker-ce sends `mapClamped(volume, 0, 100, 0, 127)`, an integer floor, `mtSequencer.cpp:2445`; sample tracks play the value as an FX 18 of the same value, whose formula is the same, `seq/include/o3os/seq/fx.h:316`); 101..127 -> 127, counted in `values_clamped` (new Community OS behaviour: tracker-ce's editor clamps to 100, so it never writes one, `core/source/mtSequencer_patternEdit.cpp:849, 916`). On a step with no note the volume is kept in `vol` and plays nothing here (open research question: tracker-ce changes the playing voice's velocity, `mtSequencer.cpp:483-486`) |
| FX 1 type, value | fx[0] | the id map of OS-CEFXIDS (ADR 0004 amendment 3), any byte: 0..42 verbatim; 43..74 -> 96..127 (id + 53); 75..127 -> (0, 0), counted in `ce_fx_cleared` (OS-CEPAT S1b, new Community OS behaviour); 128..255 verbatim. Leftover values on type 0 and typed FX past the track length are kept (PAR-003) |
| FX 2 type, value | fx[1] | the same map |
| micromove i8 (v6 byte 7) | fx[0] / fx[1] | negative -> nothing (verified parity); otherwise FX 2 with the same value in the first free FX column (placement: new Community OS behaviour, Q2 (a)); superseded when a column already holds FX 2 (verified parity: the FX columns run after the volume and micromove columns and win, `tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:471-474, 490, 518-527`); dropped and counted in `micromoves_dropped` when no column is free or the other one holds Random value 16 (new Community OS behaviour: this OS's sequencer would randomise the moved value, `seq/src/sequencer.cpp:1820-1834`). Values 101..127 are kept as stored (`fx_sanitize` plays them as 100) |

The reader reports what the v6 columns and the id map cost, in `ImportReport`: `micromoves_dropped`, `values_clamped`, `ce_fx_cleared` (OS-CEPAT A10; the load strip names them in S2).

Export writes the inverse: version 5, 12 tracks (9,260 bytes), header `KS`, type 2, firmware 1.9.0.1, size 9,260, tempo bytes 100.0f, swing 0, reserved 0, for either Export target (1.9.2 writes the same pattern as 1.9.0). An empty step is `FF 00 00 00 00 00`.

| Export rewrites or drops | Counted in `ExportDrops` |
|---|---|
| the volume column (0..127, or 255 not set) goes to the first FX column of type 0 as FX 18 with value `(v x 100 + 63) / 127` (127 -> 100, 64 -> 50); a leftover value there is overwritten | `volume_as_fx18` |
| both columns taken, or FX 18 already on the step: the volume is not written | `volume_dropped` |
| FX types 128..255 (engine FX, ADR 0004): written as none (0, 0), before the volume looks for a free column | `engine_fx` |
| FX types 43..95 (core FX of ADR 0004 amendments 1 and 2: Speed, Tempo fine, Order jump, the tracker FX 46..62): written as none (0, 0), before the volume looks for a free column; FX 0..42 are written as they are | `core_fx` |
| FX types 96..127 (tracker-ce FX, ADR 0004 amendment 3): written as none (0, 0), in the same place | `ce_fx` |
| tracks 13-16 are not written | `tracks_beyond_12` (non-empty steps in them) |
| a regular note with no instrument is written as instrument byte 0 (OG notes always name one) and reads back as instrument 1 | (normalised, section 8) |

Since OS-COREFX (ADR 0004 amendment 1 accepted) and OS-CEFXIDS (amendment 3): a cell of FX 43..95 (core) or 96..127 (tracker-ce) is dropped, not carried, and counted in `core_fx` or `ce_fx`; what the official firmware shows for such a cell is an open research question. Only a native `.o3pj` keeps them. The count is per FX column (a step with both columns holding one counts 2). On the way in the map of amendment 3 applies instead: an OG cell of 43..74 is stored as 96..127 (so it no longer comes back identical through OG -> model -> OG), and 75..127 are cleared to (0, 0); the types the OG itself defines (0..42) and the engine types (128..255) still round-trip byte for byte. Labels: the 0..42 row is verified parity (`tracker-ce@7fd6c2c:core/source/mtSequencer.h:122-171` against `seq/include/o3os/seq/fx.h:38-77`); the 43..74 -> 96..127 and 75..127 -> (0, 0) rules are new Community OS behaviour.

## 5. Instruments (.mti, .pti)

The first 392 bytes are one block: header 16, sampler block 372, CRC-32 of [0, 388) at 388. Versions 1-3 have 368 bytes of sampler block and the CRC at 384 (388 B).

| OG field (file offset) | Model (`SamplerParams` unless noted) | Rule |
|---|---|---|
| active flag (16) | kind | `.mti` with 0: no instrument; a `.pti` is always active; a `.pti` with no PCM is no instrument (the slot stays as it is) |
| sample type (20) | sample_type | above 1: 0 (counted) |
| sample name, 32 bytes (21) | instrument name | to the first NUL, 31 characters, verbatim; **never used as a path** |
| (derived) | sample_path | `samples/instrNN.wav`, NN = slot + 1: safe by construction |
| wavetable window size (64) | wavetable_window_size | verbatim |
| play mode (76) | play_mode | version 1: values above 4 plus one; above 7: 0 (counted) |
| start, loop 1, loop 2, end (78-85) | start, loop_start, loop_end, end | verbatim |
| current wavetable window (88) | wavetable_window | verbatim |
| envelopes 6 x 20 B (92) | envelope[6] | verbatim; non-finite amount or sustain: 1.0 (counted); version 1-2: the fine-tune envelope is the 1.1 default (attack 3000, release 1000, amount 1, sustain 1) |
| LFOs 6 x 8 B (212) | lfo[6] | for a slot with loop 2 or 3 (WT Env / WT LFO, OS-CEWTENV W1) — **tested exactly: only 2 and 3 are WT slots, a garbage loop byte (any other value) is not one** and keeps the loop 0/1 rule below: the shape is the contour 0..255, verbatim, and bytes +2/+3 are `wt_bend` (warp X in the high nibble, warp Y in the low one) and `wt_sust` (0 one-shot, 1..99 freeze %), read (verified parity, tracker-ce@7fd6c2c:core/source/mtStructs.h:629-637, `mtEnvelopeGenerator.h:24-44`); for loop 0 or 1: shape above 4: 2 (Triangle; the OG has the shapes 0..4, and 5 is this OS's Sine, OS-LFOSINE: never an OG value) and bytes +2/+3 dropped; speed and amount verbatim; non-finite amount: 0.5 (counted) |
| cutoff, resonance (260, 264) | cutoff, resonance | non-finite: 1.0, 0.0 (counted) |
| filter type, filter enable (268, 269) | filter_type, filter_enable | type above 2: 0 (counted); enable verbatim |
| tune, fine tune (270, 271) | tune, fine_tune | verbatim (i8) |
| volume (272) | volume | above 100: 50 (counted) |
| glide (274) | glide | verbatim |
| panning i16 (276) | panning | above 100 or negative: 50 (counted) |
| delay send (278), reverb send (384) | delay_send, reverb_send | above 100: 0 (counted); reverb send is 0 before version 4 |
| play thru (279) | slice_thru | any non-zero byte: 1, in every version (verified parity, tracker-ce@7fd6c2c:core/FileManager/fileManagerInstrument.cpp:663-706: the struct is copied raw, with no version rule for the byte); 0: 0. Export writes the model's 0 or 1. The byte is the alignment padding the official layout has between the delay send and the slice table: file byte 279 = header 16 + the struct offset (inference from the two layouts, tracker-ce@7fd6c2c:core/source/mtStructs.h:653-654, :763 — open research question until OS-CECHECK (a) surveys a tracker-ce file; measured: 0 in all 70 fixture instruments, so official 1.9 writes 0 there and what it does with a 1 is unknown, most likely nothing). **Versions before 1.9 are an open research question**: the byte was padding in the older layouts, so a stale non-zero byte in such a file would import as play-through (all measured fixtures are official 1.9, where the byte is 0) |
| slice points, 48 entries (280), slice count (376), selected slice (377) | slices, slice_count, selected_slice | count above 48: 48, selected above 47: 0 (counted) |
| grain length, position (378, 380), shape, type (382, 383) | grain_length, grain_position, grain_shape, grain_type | shape and type above 2: 0 (counted) |
| overdrive (385) | overdrive | 0 before version 6; above 100: 0 (counted) |
| bit depth (386) | bit_depth | 16 before version 8 (the bytes hold junk in fixtures); outside 4..16: 16 (counted) |

Not kept: the header's firmware, version and size bytes and its two padding bytes, the padding at 17-19, 53-55, 66-67, 77, 86-87 and 273, the sample address field (56: not data, not kept), the sample length field (60: re-derived from the PCM), the wavetable window number (68: derived, the OS recomputes it), the 4-byte name field (72), the byte at 387, junk after the name's NUL, and the CRC (recomputed).

A `.pti` whose header states a frame count that is not the PCM's (the length field is non-zero and differs from (size - 392) / 2) is stereo or damaged: `Unsupported`. A header that states 0 is mono. An odd trailing byte is ignored. More than 5,898,240 frames, or a source that goes on past that, is `LimitExceeded`.

Export writes `NN name.pti` as version 9: header `TI`, type 1, firmware 1.9.0.1, size 372; the active flag 1; the sample name = the instrument's name (31 characters, counted when cut) or, when it has none, the file name of its sample path; the sample length field = the frames of the PCM the caller passes (0 without PCM, and then the file is the 392-byte head alone, the shape of an `.mti`); the address field, window number, the 4-byte name field and padding 0. **A Sine LFO (shape 5, OS-LFOSINE: this OS's sixth shape, which the OG does not have) is written as shape 2 (Triangle)**, every other byte as an instrument without it writes; counted in `ExportDrops::lfo_sine` (one for each sample instrument with such an LFO), and the export's loss list and strip say `SINE LFO` (`SINE LFO SHAPES WRITTEN AS TRIANGLE`). New Community OS behaviour (the OG has no sine). **A WT slot (an envelope loop of 2 or 3, OS-CEWTENV W1: what official 1.9 does with those loop values is unknown, it may index past its LFO tables — open research question) is written as an official slot: loop 0 and enable 0 (off), the shape held to 0..4 and bytes +2/+3 zero**, counted in `ExportDrops::wt_mod` (one per WT slot), and the export's loss list and strip say `WT MOD` (`WT MODULATION SLOTS WRITTEN OFF (NO WT IN THE OG)`); the Sine rule above applies only to a slot with loop 0 or 1. New Community OS behaviour (the Export rule and the counter); the bytes' meaning is verified parity (tracker-ce@7fd6c2c:core/source/mtStructs.h:629-637). **Play thru (byte 279, OS-CEPLAYTHRU P1) is written as the instrument's flag, 0 or 1**; what official 1.9 does with a 1 there is an open research question (measured: 0 in all 70 fixture instruments, docs/specs/OS-CEPLAYTHRU.md F8), so an Export counts nothing for it. `og::format_pti_name()` makes the file name: `NN ` + the name with `/ \ : " * ? < > |` and control characters replaced by `_` + `.pti`, cut to the buffer, so a hostile name cannot leave its folder. `og::export_class()` says what Export does with each model instrument: sample slots 1-48 become `.pti`, MIDI channel n in slot 48 + n is implicit (an OG project has them without a file), anything else (module instruments, MIDI elsewhere, samples in slots 49-64) is dropped and counted (`count_instrument_drops`).

## 6. PAMD (`patterns/patternsMetadata`)

A 16-byte header (`PAMD`, u16 version, two reserved bytes, u32 total size, u32 control flags), then 50-byte records: a name of up to 30 characters in 31 bytes, 19 further bytes nobody has seen used. The only fixture holds 256 records (12,816 bytes). **Record i names pattern i + 1** is an inference (PAR-008 OQ-8.2, from the 1-based pattern numbering).

**Import.** The reader takes the whole file in (12,816 bytes at most; `params.size_hint` above that refuses it before any byte is read), checks it (header, version, length: more than 256 records is `LimitExceeded`, bytes short of a whole record are ignored and reported as `partial_record`), and only then names the patterns that exist (`names_applied`; names of patterns the project does not have are counted in `names_skipped`). A bad or failing file changes nothing. The file is kept **verbatim, minus its trailing zeros, in an `OGPN` chunk** (layout in `o3-project.md`): reserved bytes, flags, the 19 bytes of a record and names of patterns the project does not have are nobody's to rewrite. A project that already holds an `OGPN` chunk refuses a second file (a pattern id that is taken is refused the same way). The canonical empty file (what Export writes without being told) needs no chunk. A version above 1 is `unverified_version`: no names, nothing kept. A pool without room for the chunk still gets the names (`names_kept` false, the model counts the dropped chunk).

**Export.** The file the project was read from is written back byte for byte (the chunk's file length, the kept bytes, then the zeros). A project without an `OGPN` chunk gets a header and 256 empty records. **Names given to patterns in the OS are not written** (hardware gate): a wrong mapping would shift every name by one on an official device, so until it is confirmed, every pattern whose name is not the one the written file holds at record id - 1 is counted in `ExportDrops::pattern_names` and the export reports it as lost. An invalid or repeated `OGPN` chunk is ignored and counted in `settings_ignored`.

**Hardware / maintainer items to close this gate** (`hw-session`): (1) the only real file has 256 records for patterns numbered 1..255 and no names, so either record 0 is unused (record = pattern number) or record 255 is spare (record = pattern number - 1): one file with two named patterns settles it; (2) the meaning of the two reserved bytes (0x99 0x99 in that file), the control flags and the 19 trailing bytes. Once confirmed, writing OS names is one function in `PamdWriter::begin` and the counter goes away.

## 7. The Export report

`ExportReport::drops` of each writer; the app adds them (`ExportDrops::add`). `any_lost()` is true for anything but `volume_as_fx18` (a rewrite), `audio_on_midi_tracks` (a note) and `defaulted_groups` (a default, not a loss).

| Counter | Counted by | Meaning |
|---|---|---|
| `tracks_beyond_12` | pattern | non-empty steps in tracks 13-16 |
| `engine_fx` | pattern | FX columns with a type of 128..255, written as none |
| `core_fx` | pattern | FX columns with a type of 43..95, written as none |
| `ce_fx` | pattern | FX columns with a tracker-ce FX type of 96..127 (ADR 0004 amendment 3), written as none; summed into the same FX line of the Export screen |
| `volume_as_fx18` | pattern | volume columns written as FX 18 |
| `volume_dropped` | pattern | volume columns that could not be written |
| `audio_on_midi_tracks` | project | audio tracks 9-12 (what Import makes of them): written into slots 9-12 of the OG project as they are, under the "Midi N" name when theirs is "Track N". A note, not a loss (OS-TRK912) |
| `track_settings` | project | tracks 1-12 with a pan, solo or output bus (OG has none), a type other than the one an OG file implies (tracks 1-8 audio, 9-12 MIDI or audio, see above), and on tracks 9-12 a mute or a volume that is not the default (the file only holds those for tracks 1-8) |
| `track_names_cut` | project | names of tracks 9-16 longer than 7 characters |
| `project_settings` | project | swing other than 50 (OG has no project swing); a project name cut to 31; the byte at 0x80C that a v17 project carried (OGPM 1.1), when the target is 1.9.0 and a v16 file has no such field |
| `module_instruments`, `midi_instruments`, `sample_slots_beyond_48` | project | instruments `export_class` cannot write |
| `unknown_chunks` | project | native chunks kept for a newer version: no OG home (the OGPM and OGPN chunks are not counted: they are carried) |
| `instrument_names_cut` | instrument | names longer than 31 characters |
| `pattern_names` | PAMD | pattern names that are not what the PAMD file written holds (OS names are not written, section 6) |
| `settings_ignored` | project, PAMD | OGPM or OGPN chunks that were invalid or repeated, and so not used |
| `defaulted_groups` | project | `DropGroup` bits of every group written at its default: the session state always, a settings group the project carries no value for. Not a loss |

Not reported because the OG has no place for them and the model's value is the default or derived: master volume, the limiter flag, active track count, pattern names (the model's 30 characters are the OG's).

## 8. What Export normalises, and why

OG -> model -> OG is byte-identical for synthetic project v16 (with a non-default value in every settings group: `og_settings_test.cpp`), project v17 with the 1.9.2 target (with a non-zero byte at 0x80C, and with a non-default value in every group: `og_v17_test.cpp`), 12-track pattern v5, `.pti` v9 and PAMD files (`og_roundtrip_test.cpp`, and the same through the native format). For other input exactly these bytes change, and the test `the bytes Export normalises are exactly the ones nobody reads` proves the list is complete by feeding junk into them and getting the canonical file out:

| File | Normalised | Why |
|---|---|---|
| any | firmware, version and size bytes of the header, its two padding bytes | they describe the writer, not the content; Export is firmware 1.9.0 and version 16, 5, 9 (the project: 1.9.2 and 17 for the 1.9.2 target) |
| project | the session state (song position, last used values, pad board, old volume, edit step, not-saved flag, save lists), padding between fields, the 224 bytes behind the v16 name (v17: the three padding bytes after 0x80C, and the NUL, second string and spare byte behind the name, 0x830-0x90F) | state of the editing session, not of the project; padding and uninitialised memory (the settings groups, and the byte at 0x80C, are carried, section 3) |
| project | the usage bitmask | rebuilt from the patterns by a rule that agrees with all 104 real patterns |
| project | an empty song becomes pattern 1; a non-zero byte in a mute becomes 1; tempo is kept to 0.01 BPM | this OS's choice (pattern 1); a flag; the model's resolution |
| project | versions before 16 become 16 (17 for the 1.9.2 target): names of tracks 9-16 empty, the groups an older version lacks at their defaults, the 0x4F7 value moved to 0x309 (v11, v12), MIDI velocities of v2-v7 moved into the channel records | Export aims to write what official 1.9.x opens (intent, open research question); the moves are an inference (OQ-5; open research question, unverified) |
| project | v16 to v17 (the 1.9.2 target): the name moves to 0x810, the new byte at 0x80C is 0, the CRC moves to 0x910; v17 to 1.9.0: the byte at 0x80C is dropped (counted) | the layout of the target; the byte has no home in v16 |
| project | an invalid or repeated `OGPM` chunk | not used, counted (`settings_ignored`) |
| pattern | head tempo and swing floats, reserved bytes | not read by this OS (what the official firmware does with them is an open research question) |
| pattern | an 8-track file becomes 12 tracks, the new ones empty with the first track's length | Export writes 12 |
| pattern | instrument byte 255 becomes 0; a regular note with no instrument comes back as instrument 1 | 255 is outside 0..63; OG notes always name one |
| pattern | engine FX cleared, volume column moved into FX 18 | section 4 |
| instrument | the sample address field, window number, the 4-byte name field, padding, junk after the name's NUL | not data, derived, reserved, padding |
| instrument | the sample length field becomes the PCM's frames | derived |
| instrument | overdrive before version 6 and bit depth before version 8 | those bytes were padding, not values |
| instrument | any value replaced when read (the `sanitised` count) | the model cannot hold it |
| PAMD | nothing: the file comes back byte for byte (`OGPN`); names given to patterns in the OS are not written, counted in `pattern_names` | section 6 |

A project of 8 tracks reads back from its export with 12 active tracks: Export always writes a 12-track project, which is the one model difference of a second import.

## 9. Hostile input

| Rule | Behaviour | Test |
|---|---|---|
| truncation | `Truncated` at every offset of every file kind (header cuts included); a cut at an 8-track length reads as an 8-track file whose CRC then fails | `og_*_test.cpp`, each "truncated at every offset" |
| oversize lengths | project over 2,400 bytes (a source that never ends too): `LimitExceeded`, within 10 steps; a PAMD file over 12,816 bytes (also from `size_hint`, before any byte is read), refused whole; a known version at the wrong length: `Truncated` / `Malformed`; bytes after a pattern, `.mti` or 256 PAMD records refused; PCM past 5,898,240 frames refused (a source that never ends too); a pattern length byte above 127 refused | same |
| bad versions | 0 refused, older than readable refused, newer than known (v18+) or unobserved loads as unverified (section 2); v17 at a length that is not 2,324 refused | same, `og_v17_test.cpp` |
| bad CRC | refused for projects of unverified version, patterns and `.mti`; only warned about for verified projects (v17 included) and `.pti` | same, `og_v17_test.cpp` |
| 16-track v5 framing (OS-CEPAT S1b) | a 13th row means 16 tracks: a file cut inside the 13th row is `Truncated`; four bytes then the end after row 12 read as the 12-track file, whose CRC those four bytes then fail (`BadCrc`); one byte after the CRC of a 16-track file is `Malformed`; a bad CRC anywhere refuses the file and removes the pattern, as for 8 and 12 tracks | `og_hostile_test.cpp` (`16-track v5 framing`), `og_pattern_test.cpp` (truncated at every offset of 8-, 12- and 16-track files; bytes after the end) |
| leniency: the declared size is ignored for v ≤ 5 | a v5 file at a v6 declared size loads (the stream decides); tracker-ce refuses that file, validating the declared size against the version's own sizes (`tracker-ce@7fd6c2c:core/include/patternFileValidation.h:24-33`, refused at `:41`). New Community OS behaviour | `og_hostile_test.cpp` (`v6 framing`: v5 at a v6 size) |
| names | a sample file name is only a name (never a path); `format_pti_name` removes every character a file name must not carry; the sample path is generated | `og_instrument_test.cpp`, `og_pamd_test.cpp` |
| junk anywhere | 16,000 deterministic mutations (CRC repaired in half) of valid files, random budgets and read sizes, under ASan and UBSan: ends, says why, leaves the project as it found it (a PAMD file that fails applies no name and keeps nothing), and what it accepts is a valid model, with valid OGPM / OGPN chunks, that Export writes and reads back | `og_hostile_test.cpp` (`O3OS_FUZZ_SCALE=<n>` for longer) |
| bad sources and sinks | `Failed`, `Ok` with no bytes, a count above the request: `SourceFailed` / `SinkFailed`; `WouldBlock` is waited out | each file kind |
| the project changes while a writer runs | `Modified` | each writer |

## 10. Evidence, inferences, open questions

The fixture survey (`og_fixture_survey_test.cpp`, skipped when `fixtures/` is absent; counts and byte offsets only, nothing from a file is ever printed): 8 project folders, 8 of 8 projects (v2, v3, v7, v11-v14, v16), 104 of 104 patterns (103 8-track, 1 12-track), 93 of 93 instruments (75 `.mti`, 18 `.pti`) and 1 PAMD import with 0 errors, 0 sanitised values, 0 CRC warnings. Export then import again gives the same model (through the native format) for 8 of 8 projects; the 104 patterns' track blocks come back byte for byte; the usage bitmask rebuilt from the patterns equals the file's own for 104 of 104; instrument files differ from their originals only at the bytes of section 8 (header, padding, pointer, junk after the name, window number, overdrive and bit depth of old versions). Since OGPM / OGPN: every byte of every settings group comes back in 8 of 8 projects (all 8 hold an OGPM chunk), and the PAMD file comes back byte for byte (1 of 1, reserved bytes included).

No fixture holds a project v17 (official 1.9.2); the survey is unchanged by OS-004c and reports the same counts. Everything about v17 is an open research question (unverified, section 2): no file backs its layout, version, size or firmware bytes, and the synthetic v17 files of the tests are written from the table of section 3 (`support/og_fixture.h`), so they prove the reader and the writer against each other, not against a real file.

Inferred, to check on hardware or with a real file (`hw-session`):
- Project v17: the three bytes at 0x80D-0x80F are padding (inferred from the block's size being a multiple of 4; open research question, unverified) and are not carried; what the byte at 0x80C is for is not known (open research question), so it is carried and written back unread; the second string at 0x831 is not carried (the model has no field for it). A real 1.9.2 project settles all three; so does a diff of `project.mt` before and after toggling a 1.9.2 feature (Focus Loop, the theme) on a device.
- What 1.9.0 and 1.9.2 devices do with the other generation's project file (the Export target's table in section 3) is an open research question (unverified): no device was tried.
- PAMD record i names pattern i + 1 (a wrong guess shifts every name): the hardware gate of section 6 (256 records vs 255 patterns, record = pattern number vs number - 1). Until it is closed OS pattern names are not written.
- The velocity of a version 2-7 project goes to byte 1 of its channel's record (OQ-5, inferred).
- Export's defaults for what a project does not carry (delay 50 / 500 ms / 0xC3, limiter 30200, pad board 11 / 24 / 8, the 1.3 block and the 0x5FE value 20000, MIDI velocity byte 86): from the fixtures (this OS's choice where no fixture holds a value); they are written only for a project that did not come from the OG or left a group at the default; official 1.9.x opening an exported project and all its files is the test (ADR 0006 amendment 1's hardware check).
- Tracks 9-12 are MIDI-only and the volume column is FX 18 scaled 0..127 -> 0..100.
- Project versions 4, 5 and 6 (1,268, 1,280 and 1,284 bytes): open research question (unverified); no file of any of them is in the repository (of the versions before 7 only v2 and v3 have fixtures), so their layouts are not backed by a file. Version 1 (an unreadable layout) is refused.
- Instrument versions 1-2 (play-mode shift, fine-tune envelope): open research question (unverified); no fixture.

Follow-ups, not done here:
- OS-005: when the audio engine keeps delay, limiter and reverb settings in chunks of its own, they take precedence over `OGPM` (the hook is `write_engine_settings()`); the same settings should then leave `OGPM` for good.
- The pool of unknown chunks is 16 KiB and 16 chunks: a PAMD file with data in its last record costs 12.8 KB of it. If that squeezes other chunks out, keep only the non-zero records (sparse form) in a new minor version of `OGPN`.
- WAV wrapping of a `.pti`'s PCM and the WAV -> PCM source of an export (the platform's, with OS-002c's WAV job).
- 16-track v5 (12,336 B) is read (OS-CEPAT S1b): tracker-ce reads it (from the source: `tracker-ce@7fd6c2c:core/include/patternFileValidation.h:12` and its acceptance for v ≤ 5 at `:26-29`; its loader gives the tracks a file does not hold track 1's length, `core/FileManager/fileManagerPattern.cpp:561-591`, the same rule this OS already had). No fixture holds one (the survey counts 103 8-track and 1 12-track pattern), so like v6 it is "from the source" until the S3 survey reads a real one.
- Pattern v6 (8,232 / 12,332 / 16,432 B) is read (OS-CEPAT S1): the layout is from the source only — `tracker-ce@7fd6c2c:core/source/mtSequencer.h:282-336` (the 28-byte head, rows of one length byte plus 128 steps of 8 bytes, the step of note i8, instrument u8, fx[2] (type, value), volume i8, micromove i8 at `:305-316`, the CRC-32 over everything before it), the sizes at `core/include/patternFileValidation.h:13-15`, the version bytes 6 at `core/source/mtStructs.h:43-45`; tracker-ce writes the CRC with the standard CRC-32 (`core/FileManager/fileManagerPattern.cpp:276-283`) but checks only sizes on load, never the CRC (`:302-316`); a v ≤ 5 file is upgraded in memory with volume and micromove at -1 (`:526-559`). `kPatternVerified` is 6 on that evidence; until the S3 survey reads a real v6 file, v6 stays "from the source".
- The Export target as a device-scope setting parameter (OS-002d) in the Export dialog; until then `WriteParams::target`.
