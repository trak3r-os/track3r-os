# The native project format (`O3PJ`), version 1

Status: specified with OS-004a, 2026-10-11; `SETS` and `LRNB` added with OS-002d, same day (section 3 and `docs/formats/o3-settings.md`); the project-level state (`MIDI`, `USBS`, `MIXR`, `PERF`, `APPM`) and `INST` 1.2 (the playback engine) added with OS-004d, 2026-10-12 (section 3, "The project-level state"); the session state (`NOTE`, `MUTE`, `SCEN`, `SNFX`) added with OS-PERSIST2, 2026-10-13 (section 3, "The session state"); `INST` 1.3 (the MIDI instrument's fields and VEL>) added with OS-INSTUI S8b + S9, 2026-10-15 (section 3, "INST", and `docs/formats/INST-1.3-diff.md`); the instrument volume law (`LAW`) added with OS-BUG-PASTE24, 2026-10-16 (section 3, "The `LAW` chunk"; no major bump, with a downgrade hearing risk stated there); `APPM` 1.1 (twelve apps, not six) added with OS-APPUX S3, 2026-10-19 (section 3, "The project-level state"; a minor bump with a loader for 1.0); `USBS` 1.1 (4 or 6 USB outputs: P8 plus a count byte) added with OS-USBOUTS, 2026-10-29 (section 3, "The project-level state"; a minor bump with a loader for 1.0, 1.0 kept byte for byte for 2, 8, 10, 11 outputs) · Decisions: ADR 0003 (16 track slots), ADR 0004 (step format), ADR 0007 (IFF-style chunks, versioned, unknown chunks kept, CRC, Python reference), ADR 0009 rev 1 (resumable `begin / step / report`), ADR 0011 (declared alignment).

Code: model `model` (`o3os::model`), format `formats/native` (`o3os::formats::native`), Python reference `tools/o3project.py`. Tests: `tests/model_test.cpp`, `native_format_test.cpp`, `native_fuzz_test.cpp`, ctest `project_python`.

A project is one file. Samples stay as WAV files next to it; the file stores relative paths (see "Samples").

## 1. Conventions

- **Big-endian.** Every multi-byte integer is big-endian, as in IFF. Floats are IEEE-754 binary32 stored as their 32-bit pattern (also big-endian); the model never holds NaN or infinity (a file that does is rejected), so the pattern is the value.
- **Text.** `str` is one length byte followed by that many bytes. No terminator, no NUL inside (a NUL is an error), no encoding is enforced (UTF-8 is expected; the core does not look). Maximum lengths are in section 8.
- **Chunk id** is four ASCII characters in `0x20..0x7E`.
- **Chunk** = id (4) + payload length `n` (u32) + payload (`n` bytes) + one zero pad byte when `n` is odd, so every chunk starts at an even offset. The length does not include the pad.
- **Chunk version** is the first u16 of every payload of a known chunk: major in the high byte, minor in the low byte. All chunks are `0x0100` (1.0) in this version except `INST`, which is 1.1 (the sample rate) or, for an instrument that chose its playback engine, 1.2, or, for an instrument one of whose MIDI fields or VEL> is not its default, 1.3, or, for an instrument with an LFO of the Sine shape, 1.4 (OS-LFOSINE: the 1.3 layout, no new byte), or, for a Sample instrument with the stereo flag, 1.5 (OS-STEREO step 2: the 1.3 layout and one byte); and `PERF` and `SCEN`, which are 1.1 when a slot holds the Perform FX 20 or 21 (OS-PERFFX P1) and 1.0 otherwise; and `APPM`, which is 1.1 when more than six apps are stored (OS-APPUX S3) and 1.0 otherwise. The four gates are independent: each chunk is bumped by its own condition only. A reader reads it **before** any length rule (rules for versions in section 3, "Versions").

## 2. Container

```
"FORM"  u32 length   "O3PJ"   chunk chunk ... chunk
```

`length` counts everything after the length field itself: the 4 bytes `O3PJ` plus all chunks with their headers and pads. It must be even and at least 4. The file is `length + 8` bytes; bytes after that are ignored (the reader never looks at them).

Order of chunks, as written:

| Chunk | Count | Content |
|---|---|---|
| `PHDR` | 1, first | project header, master section |
| `TRAK` | 1 | the 16 track slots |
| `MODR` | 1 | module references |
| `SONG` | 1 | order list |
| `PATT` | 0..128 | one per pattern (with its per-track lengths), ascending id |
| `INST` | 0..64 | one per instrument, ascending slot |
| `SETS` | 0..1 | the project's settings (OS-002d), only when one is not its default |
| `LRNB` | 0..1 | the project's MIDI Learn bindings (OS-002d), only when one is in use |
| `MIDI` | 0..1 | the project's MIDI routing, offsets and CC numbers (OS-004d), only when the project has them |
| `USBS` | 0..1 | the USB mode, profile and stem map (OS-004d), only when the project has them; 1.1 for 4 or 6 outputs (OS-USBOUTS) |
| `MIXR` | 0..1 | the mixer's routing matrix, OUT mode and levels (OS-004d), only when the project has them |
| `PERF` | 0..1 | the twelve Perform slots (OS-004d), only when the project has them |
| `APPM` | 0..1 | the parameters of the SD apps the project remembers (OS-004d), only when there are some |
| `NOTE` | 0..1 | the note-entry settings: root, scale, octave, SCALE LOCK and JUMP (OS-PERSIST2), only when the project has them |
| `MUTE` | 0..1 | the MUTE MODE of each track (OS-PERSIST2), only when the project has them |
| `SCEN` | 0..1 | the Perform scenes 1..3 and LATCH (OS-PERSIST2), only when the project has them |
| `SNFX` | 0..1 | the send effects' controls of the MASTER FX page (OS-PERSIST2), only when the project has them |
| `LAW ` | 0..1 | the instrument volume law (OS-BUG-PASTE24), only when it is the 1.1.14 table (law 1); the last known chunk |
| unknown | 0..16 | chunks this version does not know (the OG formats' `OGPM` and `OGPN` among them), in the order they were read |
| `CRC ` | 1, last | CRC-32 of every byte before this chunk's header |

Reading is stricter than writing only where it protects the model: `PHDR` must be the first chunk of all; known chunks must appear in the order above (only `PATT` and `INST` repeat; `SETS` comes after the last `INST`, `LRNB` after `SETS`, then `MIDI`, `USBS`, `MIXR`, `PERF`, `APPM`, `NOTE`, `MUTE`, `SCEN`, `SNFX`, `LAW ` in that order, each once); `CRC ` must be the last chunk and end exactly at the end of `FORM`. `TRAK`, `MODR` and `SONG` may be absent (defaults: default tracks, no modules, empty song; a module count in `PHDR` must still match). Unknown chunks may appear anywhere after `PHDR`; they are written back after the known chunks (their position among the known ones is not kept).

The pad byte must be zero. Everything else a reader finds wrong is an error (section 7); it never guesses.

**CRC.** CRC-32/ISO-HDLC (the one of zlib, PNG and Ethernet: reflected polynomial `0xEDB88320`, initial value and final xor `0xFFFFFFFF`; the check value of `"123456789"` is `0xCBF43926`) over the bytes from offset 0 up to, not including, the header of the `CRC ` chunk. It therefore covers the `FORM` length field. The `CRC ` payload is `u16 version`, `u32 crc`.

## 3. Chunks

Offsets are payload offsets. `[...]` is a run of the preceding item.

### PHDR (project header)

| Offset | Size | Field | Valid |
|---|---|---|---|
| 0 | 2 | version | `0x01xx` |
| 2 | 1 | active tracks | 1..16 (new project: 12) |
| 3 | 1 | default pattern length | 1..128 (32) |
| 4 | 4 | tempo, BPM x 100 | 4000..80000 (13000 = 130.00) |
| 8 | 1 | swing, percent | 25..75 (50) |
| 9 | 1 | master volume | 0..100 (86) |
| 10 | 1 | dry volume | 0..100 (86) |
| 11 | 1 | reverb volume | 0..100 (86) |
| 12 | 1 | delay volume | 0..100 (86) |
| 13 | 1 | master flags | bit 0 dry mute, 1 reverb mute, 2 delay mute, 3 limiter; bits 4..7 must be 0 |
| 14 | 2 | number of `PATT` chunks | <= 128, must equal the chunks that follow |
| 16 | 1 | number of `INST` chunks | <= 64, must equal |
| 17 | 1 | number of modules | <= 16, must equal the `MODR` entries |
| 18 | 1+n | project name | `str`, <= 32 |

Length 19 + name, so 19..51. Engine FX settings (reverb, delay and limiter parameters) are not stored yet; they arrive with the audio engine as new chunks (older readers keep them as unknown chunks).

### TRAK (track slots)

`u16 version`, `u8 slot count` (1..16; writers always write 16, missing slots keep their defaults), then per slot:

| Size | Field | Valid |
|---|---|---|
| 1 | type | 0 Audio, 1 MIDI, 2 External audio (ADR 0003) |
| 1 | flags | bit 0 mute, bit 1 solo; others 0 |
| 1 | volume | 0..100 (86) |
| 1 | pan | 0..100, 50 centre |
| 1 | output bus | 0 master mix, 1..16 stem buses |
| 1+n | name | `str`, <= 20 |

### MODR (module references, ADR 0002)

`u16 version`, `u8 count` (<= 16), then per module: `u8 ABI major`, `u8 ABI minor`, `str name` (1..31 characters: a file stem in `/modules`; no `/`, `\`, `:`, control characters, and not `.` or `..`; names are unique). Opening a project loads these; a missing one mutes the instruments that name it.

### SONG (order list)

`u16 version`, `u8 length` (0..255), then `length` bytes, each a pattern id 1..255. An id may name a pattern that does not exist (it plays as empty).

### PATT (one pattern)

| Offset | Size | Field | Valid |
|---|---|---|---|
| 0 | 2 | version | |
| 2 | 1 | id | 1..255, unique |
| 3 | 1 | track count | 1..16 (writers: 16) |
| 4 | 1+n | name | `str`, <= 30 |
| | | per stored track, in order: | |
| +0 | 1 | track length (steps that play) | 1..128 |
| +1 | 1 | stored steps `s` | 0..128 |
| +2 | 7 s | steps 0..s-1 | see "Step" |

A pattern always has 16 tracks of 128 steps in memory, and **every track has its own length**, as in the OG file (a length byte per track; nothing requires them equal). Steps past a track's length are kept (lengthening it brings them back). Steps from `s` on are empty: writers store the minimum `s` (up to the last non-empty step, whatever the length), readers accept any `s` up to 128. Tracks beyond the track count are empty and take the project's default pattern length (`PHDR`). There is no pattern-level length in the file or in `Pattern`; `Project::pattern_length(id)` derives one, the longest of the first `active tracks` tracks. Payload at most 5 + 30 + 16 x (2 + 896) = 14,403 bytes.

**Step** (7 bytes, alignment 1, declared in `model/step.h`; the in-memory layout is this layout):

| Byte | Field | Meaning |
|---|---|---|
| 0 | note | 0 empty; 1..128 = note 0..127 plus one (1 = C-0); 253 fade, 254 cut, 255 off; 129..252 reserved, kept |
| 1 | instrument | 0 none; 1..64 = instrument slot + 1 |
| 2 | volume | 0..127; 255 not set |
| 3, 4 | FX 1 type, value | |
| 5, 6 | FX 2 type, value | |

FX types follow ADR 0004: 0 none, 1..42 the OG types, 43..127 reserved for core FX, 128..255 declared by the instrument's engine module. Steps are untrusted data (PAR-003): the model and the format keep every byte exactly as written, whatever it means; it is sanitised where it is used. The empty step is `00 00 FF 00 00 00 00`.

### INST (one instrument)

| Offset | Size | Field | Valid |
|---|---|---|---|
| 0 | 2 | version | |
| 2 | 1 | slot | 0..63, unique |
| 3 | 1 | kind | 1 Sample, 2 MIDI, 3 Module (a free slot has no chunk) |
| 4 | 1 | MIDI channel | 1..16 for MIDI, else 0 |
| 5 | 1 | module index | into `MODR` for Module, else 255 |
| 6 | 1+n | name | `str`, <= 32 |
| | 1+n | sample path | `str`, <= 96, relative (see "Samples"), may be empty |
| | 296 | sampler block | below, always present (also for MIDI and Module kinds) |
| | 2 | blob length `b` | 0..256, 0 unless Module |
| | b | module parameter blob | opaque, never interpreted by the core, kept verbatim |
| | 4 | sample rate (version 1.1) | u32, Hz; 0 = not recorded (played at 44,100 Hz, the rate of the PCM in an OG `.pti`); any value is carried, sanitised where it is used |
| | 1 | playback engine (version 1.2, OS-004d) | u8: 0 automatic, 1 Modern, 2 Amiga, 3 12-bit, 4 8-bit; a larger value reads as 0 |
| | 12 | MIDI fields and VEL> (version 1.3, OS-INSTUI S8b + S9) | twelve u8, in this order: velocity 1..127 (default 127), program 0..128 (0 none, else program + 1; 0), out 0..3 (0 the device's MIDI OUT, 1 DIN, 2 USB, 3 both; 0), PB range 0..24 semitones (2: the bend range the Micro-tune FX is sent for as pitch bend, 0 sends none), offset 0..100 ms (0), CC A..F 0..131 (0 the Config page's number, 1..128 controller 0..127, 129 PROG, 130 channel aftertouch, 131 polyphonic aftertouch; 0 each), VEL> 0..100 % (100: the keyboard's velocity as it is; 0 ignores it; between is linear, played = 127 - VEL> / 100 x (127 - velocity)). A value out of its range reads as the field's default |
| | 1 | stereo flag (version 1.5, OS-STEREO step 2) | u8: 0 mono, 1 stereo; a Sample instrument's only (a 1.5 chunk of a MIDI or Module instrument with 1 reads 0 and loads); a value above 1 reads as 0. **New Community OS behaviour** |

Length 306 + name + path + b, at most 690, in version 1.0; version 1.1 adds the 4 bytes of the sample rate (310 + name + path + b, at most 694). **INST is written as version 1.1** (OS-002c: a WAV import keeps the rate of the sample, `Instrument::sample_rate`); every other chunk is written as 1.0. A reader that knows 1.1 reads a 1.0 chunk as rate 0 (and rejects a 1.0 chunk with the four bytes: unused bytes). A reader that only knows 1.0 skips the four bytes and says `newer_minor` (the rate is lost on its next save), which is the minor-version rule below working as designed: this is the first chunk to use it.

**INST 1.2 (OS-004d)** appends one byte after the rate: the instrument's playback engine (`Instrument::engine`, which was the struct's padding byte, so the in-memory layout did not change). The stored value is the engine plus one, so that 0 is "automatic", what every instrument was before: the engine follows the instrument (a sample instrument at a Paula rate, 8,287 or 8,363 Hz, that plays as a one shot or a forward loop is Amiga, the rest Modern), 1 Modern, 2 Amiga (Paula: integer period, sample-and-hold, BLEP), 3 Modern with the output rounded to 12 bits, 4 with 8 bits. **The writer writes 1.2 only for an instrument whose engine is not 0 and 1.1 for every other**, so a project that never chose an engine is byte for byte the file it was (and an older reader meets no newer minor in it; a file that does have an engine is read by an older build as a newer minor of INST: the engine is lost on its next save, the instrument is otherwise intact). A reader that knows 1.2 reads a 1.1 chunk as engine 0; a 1.2 chunk is exactly one byte longer than 1.1 (a cut or a spare byte is `Malformed`); a value above 4 (an engine a later version added) reads as 0. The Amiga filter model (A500 / A1200 / LED) is an engine-wide setting of the voices, not an instrument's, and is not stored.

**INST 1.3 (OS-INSTUI S8b + S9)** appends twelve bytes after the engine byte (the table above; `Instrument::midi_velocity`, `midi_program`, `midi_out`, `midi_pb_range`, `midi_offset`, `midi_cc[6]`, `vel_sens`; the struct grew from 704 to 716 bytes). **The engine byte is always present in a 1.3 chunk** (0 for automatic), since the twelve follow it by position. **The writer writes 1.3 only for an instrument one of whose twelve bytes is not its default, 1.2 for an instrument whose engine alone is not automatic, and 1.1 for every other**, so a project that never used the fields is byte for byte the file it was (checked against the build before INST 1.3: the small, state, persist2 and maximal fixtures have the same size and CRC-32; `tests/inst_midi_format_test.cpp`). A reader that knows 1.3 reads a 1.2 or 1.1 chunk as the defaults; a 1.3 chunk is exactly 13 bytes longer than 1.1 (a cut or a spare byte is `Malformed`); a value out of a field's range reads as that field's default (as the engine byte does, so a later version may add values). **An older reader** (INST 1.2 or 1.1, such as the build before OS-INSTUI S8b) meets a newer minor of `INST`: it reads the instrument as far as it knows (the rate, the engine), skips the twelve bytes, reports `newer_minor`, and writes the instrument back as 1.2 / 1.1: the fields are lost on its next save and nothing else of the instrument changes (checked with a reader of the build before INST 1.3 on a 1.3 file). The fields apply to a MIDI instrument (VEL>: a sample instrument); the bytes of the other kind stay at their defaults. **Learn bindings** to the twelve parameters (ids `0x80..0x8B`) are ordinary `LRNB` entries (no change of that chunk): an older reader keeps and writes them back, but has no such parameter, so a control change bound to one does nothing there.

**INST 1.5 (OS-STEREO step 2, `docs/specs/OS-STEREO.md` decision S1; new Community OS behaviour)** appends one byte after the twelve of 1.3: the instrument's stereo flag (`Instrument::stereo`, 0 mono, 1 stereo; the struct grows from 716 to 720 bytes: the stereo byte and three explicit zero `reserved` bytes, so there is no tail padding and a whole-instrument `memcmp` is defined). The layout before it is 1.3's, whole (the engine byte and the twelve MIDI bytes always present, as in 1.4); the Sine LFO gate stays "minor >= 4", so a 1.5 chunk reads shape 5 as Sine. **The writer writes 1.5 only for a Sample instrument with the flag set** (it covers a Sine LFO too), so every project without a stereo instrument is byte for byte the file it was (the existing fixtures' sizes and CRC-32 are pinned: `tests/inst_midi_format_test.cpp`, ctest `project_python`). A reader that knows 1.5 reads an older chunk as 0 and a value above 1 as 0 (as the engine byte does); a 1.5 chunk of a MIDI or Module instrument with the flag at 1 reads 0 and the project loads (the flag is a Sample instrument's, so the reader drops it as it drops a value above 1; `validate_instrument` still refuses it on an in-memory instrument); a 1.5 chunk is exactly 1 byte longer than the same instrument's 1.4 chunk (a cut or a spare byte is `Malformed`). An older reader meets 1.5 as a newer minor, reads the instrument as of 1.4, skips the byte and loses the flag on its next save: the instrument then plays mono, by design. The flag is the instrument's wish: it plays stereo only while the sample bank also holds the side signal (slice (b) of the step), which nothing provides yet, so the flag is 0 in every project this build saves. The Python mirror `tools/o3project.py` does the same (`stereo` fixture of `project_python`).

**INST 1.4 (OS-LFOSINE, `docs/specs/OS-LFOSINE.md`)** adds no field and no byte: its layout is 1.3's whole (the engine byte and the twelve always present), and the version only says that an LFO shape of 5 is **Sine** (the sixth shape; 0 Rev Saw, 1 Saw, 2 Triangle, 3 Square, 4 Random). The writer writes 1.4 only for an instrument one of whose six LFOs has shape 5, so a project that never used Sine is byte for byte the file it was. A reader that knows 1.4 reads a shape of 5 in an older chunk as 2 (Triangle: what it played, the older build holding a larger shape to Triangle), and a shape above 5 in any chunk as 2 (a later shape plays as Triangle); an older reader meets 1.4 as a newer minor with nothing appended, reads it whole, keeps the byte and plays it as Triangle (`tests/lfosine_format_test.cpp`; the Python mirror `tools/o3project.py` does the same).

**Sampler block** (296 bytes; the OG sampler parameters, see `docs/formats/og.md`; positions are the OG's 0..65535 fractions of the sample length):

| Offset | Size | Field | Valid |
|---|---|---|---|
| 0 | 1 | sample type | 0 wave, 1 wavetable |
| 1 | 1 | play mode | 0..7 |
| 2 | 1 | filter type | 0 low-pass, 1 high-pass, 2 band-pass |
| 3 | 1 | filter enable | |
| 4 | 1 | tune (i8) | |
| 5 | 1 | fine tune (i8) | |
| 6 | 1 | volume | 0..100 |
| 7 | 1 | panning | 0..100, 50 centre |
| 8 | 1 | delay send | 0..100 |
| 9 | 1 | reverb send | 0..100 |
| 10 | 1 | overdrive | 0..100 |
| 11 | 1 | bit depth | 4..16 (16 = off) |
| 12 | 1 | slice count | 0..48 |
| 13 | 1 | selected slice | 0..47 |
| 14 | 1 | grain shape | 0..2 |
| 15 | 1 | grain type | 0..2 |
| 16 | 2 x 8 | glide, wavetable window size, grain length, grain position, start, loop start, loop end, end | u16 |
| 32 | 4 | wavetable current window | u32 |
| 36 | 4 | cutoff | f32, finite |
| 40 | 4 | resonance | f32, finite |
| 44 | 6 x 20 | envelopes (amp, pan, cutoff, wavetable position, granular position, finetune) | amount f32, delay u16, attack u16, hold u16, decay u16, sustain f32, release u16, loop u8, enable u8; floats finite |
| 164 | 6 x 6 | LFOs | shape u8, speed u8, amount f32 (finite). Edited since OS-INSTUI S6 by the Params MOD page (shape 0..4, and 5 Sine from INST 1.4: OS-LFOSINE, speed 0..28 and 0..23 for the amp slot, amount 0..1); neither the layout nor the meaning changed; `init_instrument` now writes this OS's defaults (triangle, speed 0, amount 0.5) for a fresh instrument (it wrote zeros); a file already written loads as stored |
| 200 | 96 | slice positions | 48 x u16 |

A Module instrument needs its module in the table; a non-Module instrument has module index 255 and an empty blob; a MIDI instrument names a channel, any other kind has channel 0.

### SETS and LRNB (OS-002d)

`SETS` is the project's settings (kind `Project`: anti-aliasing, limiter mode, sends mode; snap to zero waits for a sourced default), `LRNB` its MIDI Learn bindings. Layout, rules and the defaults: `docs/formats/o3-settings.md` sections 2, 3 and 6; the same two chunks, read by the same code, are the device settings file. `SETS` is `u16 version`, `u16 count`, `count` x (`u32 ParamId`, `f32 value`), written (all three entries, 28 bytes) only when one setting is not its default; `LRNB` is `u16 version`, `u16 count`, `count` x (`u32 ParamId`, `u8 cc`, `u8 channel`, `u8 mode`, `u8 flags`), written only when a binding is in use. A project with neither is the file it was before they existed (the 186 bytes of section 4). **Reading is tolerant**: an entry for a setting this version does not have, with a value outside its range, or an invalid or conflicting binding is skipped and counted (`Report::settings_skipped`), the setting keeps its default; only a chunk whose length does not fit its count, or a count above 64 in a chunk of this minor version, is `Malformed` (a newer minor with a count above 64 is read as far as 64 and the rest counted as skipped).

### The project-level state (OS-004d): `MIDI`, `USBS`, `MIXR`, `PERF`, `APPM`

State that was runtime only until OS-004d (maintainer decision 2026-10-12: make it persistent at project level). Five new chunks, each version 1.0 with the layouts below (`PERF` is 1.1 when a slot uses the Perform FX 20 or 21; `APPM` is 1.1 when more than six apps are stored). **A chunk is written only when the project has the part** (the model keeps a flag per part, `Project::state_present()`, set by the first `set_midi / set_usb / set_mix / set_perf` and by a file that has the chunk; `APPM` is written when an app is remembered), so the default project is still the 186 bytes of section 4 and every project that never said anything is the file it was. Reading is strict, like `PHDR` and `TRAK` (the one tolerance: in `PERF` an FX id above the table reads as none, with its row and values reset, and a row above 3 as off, so that a later version that adds an FX does not make this one refuse the project; the same rule as `INST`'s engine): a value outside its range, a count the layout does not fit, a part twice or out of order and unused bytes in a chunk of this minor are `Malformed`; a newer minor only appends (read as far as 1.0, the rest skipped at any length, `newer_minor` set, lost on the next save). A file without a chunk reads as the part's defaults (and "absent"); for `PERF` those are the values of the table in `docs/specs/OS-PERFFX.md` section 9, not zeros (below).

`MIDI` (20 bytes): `u16 version`, `u8 out_ports` (bit 0 DIN, bit 1 USB; the ports clock, Start, Continue, Stop and SPP go out on: CLOCK OUT per port), `u8 note_ports` (the ports the notes, CC and program changes of MIDI tracks go out on), `u8 notes_channel` (0..15: where a MIDI event without a channel goes), `u8 follow` (the clock-follow bandwidth: 0 tight, 1 normal, 2 loose), `u8 cc_number[6]` (CC A..F, 0..127 or 255 = unassigned), `i16 in_offset_dms[2]`, `i16 out_offset_dms[2]` (per port DIN, USB, 0.1 ms, -1000..1000). These are the `midi::MidiConfig` fields (`midi/config.h`) that have no row in the settings table or that the project may override; `mode`, `in_port` and `sync_correction` stay the device's.

`USBS` (5 + 2 slots, 7..27 bytes): `u16 version`, `u8 mode` (0 STANDARD, 1 AUDIO), `u8 profile` (0 P2, 1 P8, 2 P10, 3 P11; the engine does not feed P4S, which is not stored), `u8 slots` (must be the profile's channel count: 2, 8, 10, 11), then `u16 mask[slots]`: the sources each stem slot sums, bit i = source i of `audio/stem_map.h` (T1..T12, Master L, Master R, Send A, Send B).

**`USBS` 1.1 (OS-USBOUTS, 2026-10-29: the number of USB outputs is a per-project choice, 4 or 6 outputs added to 2, 8, 10 and 11).** A project with 2, 8, 10 or 11 outputs writes `USBS` **1.0 byte for byte as above** (a build before this one reads and writes it the same). A project with **4 or 6 outputs** writes 1.1, 22 bytes: `u16 version` (0x0101), `u8 mode`, `u8 profile` = 1 (P8), `u8 slots` = 8, `u16 mask[8]`, then **`u8 channels`** (4 or 6). The masks are the real ones in slots 0..channels-1 (the user's map of the 4 or 6 stems, Master L/R last) followed by P8's factory masks for the rest (slots 4..7 for 4 outputs: T5, T6, Master L, Master R; slots 6..7 for 6 outputs: Master L, Master R), so that **a v1.0.x reader, which reads a 1.1 chunk through the newer-minor rule (known part parsed, the rest skipped, `newer_minor` reported), loads the project as P8 (8 outputs) with the real masks first**, and never refuses it (a profile byte 4 or more would refuse the whole project there). A v1.0.x build that saves such a project again writes it as P8: the 4 or 6 count is lost, the project comes back with 8 outputs and the stem map's first slots kept (accepted). A 1.1 reader takes the count: it must be 4 or 6 with profile 1 and 8 slots (anything else, a missing count byte or an unused byte after it is `Malformed`), and the masks past the count are zero. The profile byte of the chunk stays 0..3 in every version (the in-memory `usb::Profile` has P4 = 5 and P6 = 6; P4S = 4 is never stored). The volume of USB RETURN is still not in the chunk (a device setting; the return is presented only below 10 outputs, ADR 0012 amendment 8).

`MIXR` (25 bytes): `u16 version`, `u8 strips` (17), `u8 route[17]` (the routing matrix: bits MAIN 1, CUE 2, SNDA 4, SNDB 8, REC 16, USB 32 of strips T1..T16 and the input strip; the input strip has no USB bit), `u8 ret_route[2]` (where the SNDA / SNDB return goes: bit 0 MAIN, bit 1 CUE), `u8 out_mode` (0 STEREO, 1 MAIN L / CUE R), `u8 cue_level` (0..100), `u8 in_level` (0..100). Track volume, pan, mute, solo and the master levels are the model's own parameters (`TRAK`, `PHDR`) and are not repeated here. Defaults: tracks MAIN + USB, input MAIN + CUE, returns MAIN, STEREO, CUE 100, input 0 (what `MixBoard` and the graph start with).

`PERF` (99 bytes in both versions): `u16 version`, `u8 slots` (12), then per slot `u8 fx` (0..19 in version 1.0: `seq::PerfFx`, 0 none; **0..21 in version 1.1**, which adds 20 BITDEPTH 0..15 and 21 OVERDRV 0..100, OS-PERFFX P1; a larger id reads as none), `u8 row` (0..3, the selected row; 0 off; a larger one reads as off) and `i16 value[3]` (rows 1..3, -100..100). Defaults: slot i holds FX i + 1, row 0, and the three values the table of `docs/specs/OS-PERFFX.md` section 9 gives that FX (OS-PRFDEFAULTS: LOWPASS -30, -60, -90; STUTTER 11, 13, 15; 0 for the FX with no consumer yet; this OS's choice). **A project with no `PERF` chunk reads as these defaults** (maintainer decision 2026-10-23): every project saved before Perform was touched, and the OG, MOD, S3M and MED imports, which used to open with zeros. A chunk that is present keeps exactly the values it stores, 0 included. No byte of the format changes. Which tracks the layer affects, the held slots and the edit place are live state and are not stored; a loaded bank has every FX off (the values are clamped to each FX's own range when the bank takes them). A BITDEPTH value is how many bits are taken away (16 minus the bits that stay), so 0 is the clean sound and ends the FX like every other 0.

**`PERF` and `SCEN` 1.1 (OS-PERFFX P1, decision D5, maintainer 2026-10-19, option A).** The layout and the length are the same in 1.0 and 1.1; the version says which ids are valid. **The writer writes 1.1 only when a slot (for `SCEN`, a slot of a stored scene) holds the id 20 or 21, and 1.0 for every other bank, byte for byte as it always did** (the same pattern as `INST` 1.2: a project that never used the two FX is the file it was, and an older reader meets no newer minor in it). The reader accepts both: in 1.1 an id above 21 reads as none, in 1.0 an id above 19 reads as none (so 20 and 21 in a 1.0 chunk are none, as they always were), and in both the slot's row and values are reset to 0 with it. A 1.1 chunk that holds no 20 or 21 loads, and is written back as 1.0. **An older build** (every build before OS-PERFFX P1: it knows `PERF` and `SCEN` at 1.0 and its last id is 19) meets a 1.1 chunk as a newer minor (section 3, "Versions"): it does not refuse the project; the chunk has the length of 1.0 so it is read whole, every id above 19 reads as none with its row and values reset, `newer_minor` is set (the load line says "newer fields skipped"), and the next save writes the chunk as 1.0 with those slots empty. The other slots are as they were. Readers are safe, but a downgrade through a save is not lossless: the BITDEPTH and OVERDRV slots (and the same ids in the scenes) are lost.

`APPM` (3..1,015 bytes; **version 1.1** since OS-APPUX S3, 1.0 before): `u16 version`, `u8 count` (1..6 in 1.0, 1..12 in 1.1), then per app `str name` (the module's file stem, 1..31, a safe module name), `u8 abi_major`, `u8 abi_minor` (the ABI the module was built with), `u8 params` (1..32), `f32 value[params]` (finite). Keyed by the module name and its ABI: when an app is launched its entry is applied if the name matches and the ABI major is the same (values past the module's own parameter count are ignored, a missing one keeps its default, every value is clamped by the module's descriptor); an app that is not installed leaves its entry untouched (it is written back as it was). The project remembers the last twelve apps used (OS-APPUX S3: every shipped app stores its rows with `set_own_param`; it was six): a thirteenth drops the oldest, and so does any entry that would take the chunk past 1,015 bytes (one writer piece: six apps of 32 values and 31-byte names are 981 bytes, so a project of such apps keeps the six newest). **Version rule:** the layout of 1.1 is that of 1.0, only the count may be larger. The writer writes **1.0, byte for byte as before, while the project holds six apps or fewer** (an older OS opens such a project), and 1.1 from the seventh; the reader takes 1.0 with at most 6 apps and a length of at most 981 bytes, and 1.1 with at most 12 apps and at most 1,015 bytes (a 1.0 chunk that says seven apps is `Malformed`). An OS that knows only 1.0 reads a 1.1 chunk as a newer minor and refuses it with `APPM: bad app count`, and so refuses the whole project: such a project needs this OS or a newer one (maintainer decision 2026-10-19, option B: the format stays as it is, with no appended tail that would let an older OS skip it; with six apps or fewer stored nothing changes for an older OS). A newer minor (1.2) appends fields: the 1.1 part is read, the rest skipped. Tests: `tests/appm_format_test.cpp` (round trips, the exact bytes, 1.0 as an older OS wrote it, the counts and bounds of each version, the model's byte budget) and `tests/o3project_roundtrip.py` (the Python reference agrees on every case).

**Precedence and when it applies.** Opening a project applies its parts to the runtime from the main loop once the load has ended (`app_main.cpp`, never from an interrupt): MIXR and PERF always (a project without them sets the defaults, so the previous project's routing does not leak; for PERF the defaults are the section 9 values of `docs/specs/OS-PERFFX.md`), MIDI and USBS only when the project has them (without, the device's values stand). The device settings rows 7..11 (CLOCK MODE, MIDI IN, MIDI OUT, IN CHANNEL, OUT CHANNEL, `docs/formats/o3-settings.md`) stay device-level; the MIDI part is applied after them, so **a project's values win when it loads, and the last writer wins afterwards**: a later change of MIDI OUT or OUT CHANNEL in the Config screen overrides the project's out ports and channel until the next load, and every change made to the runtime while a project is open is stored in that project (it becomes unsaved, undo is not affected). **USB**: a project that has `USBS` asks the driver (`usb0::request_mode`, then the stem map once the new descriptor set is attached) for its profile and stem map, and for the **mode of the device setting USB AUDIO** (OS-USBAUDIO, `docs/formats/o3-settings.md` field 17: the unit enumerated in that mode at boot, so the stored `mode` byte no longer decides it: a project saved in AUDIO does not switch a unit whose USB AUDIO is Off, and one saved in STANDARD does not switch it away when it is On; the byte keeps recording the mode the unit was in when the project was saved, and the file is unchanged). A profile different from the boot's (8) re-enumerates the unit in AUDIO (not in STANDARD, which only remembers it), which is the cost of the maintainer's decision, so it is done from the main loop after the load and never twice. Whether the AUDIO descriptors carry the stereo return stream (USB RETURN, OS-USBRET) is **not** part of `USBS` (no byte of the chunk changed): it is the device setting of that name (`docs/formats/o3-settings.md`, field 13), because it depends on what the host and its hubs can carry, not on the song; a project's mode and profile are applied with the setting as it is. **The mode is the device setting's** (USB AUDIO, default On: the unit enumerated in AUDIO at boot; before OS-USBAUDIO every boot was STANDARD and a project's stored mode overrode it); a project without `USBS` leaves the profile and the map as they are. Reading the new chunks never touches the device.

**Older builds.** An older build reads a file with these chunks, keeps the five as unknown chunks (they fit the pool next to a full OG import: 1,152 bytes in at most five chunks against `OGPM` 970 + `OGPN` 12,822 of 16,384; the total with every later chunk is under "Older builds" of the session state) and writes them back byte for byte, in the order it read them; it meets `INST` 1.2 as a newer minor (the engines are lost on its next save). Checked against the build before OS-004d.

### The session state (OS-PERSIST2): `NOTE`, `MUTE`, `SCEN`, `SNFX`

The settings that were runtime only after OS-004d and that a musician sets up and expects to find again (maintainer decision 2026-10-13: persistence for all of them, one design, one format change, additive and backward compatible). **Four new chunks**, by the rule of section 10 ("a new persistent part is a new chunk"): no version bump of any existing chunk, no change to the meaning of any existing field, and an older build keeps them as unknown chunks and writes them back byte for byte. Each is version 1.0 (`0x0100`) and is **written only when the project has the part** (`Project::state_present()` bits 16, 32, 64 and 128, set by the first change of the runtime value or by a file that has the chunk), so a project that never changed any of them is the file it was (the default project is still the 186 bytes of section 4).

**Where each setting lives (the inventory, and why).** "Project" follows the OG file layout where it has an equivalent (`project.mt` holds that value); "device" would be a row of the settings file (`docs/formats/o3-settings.md`).

| Setting | Lived in | Closest field of an OG project file | Now | Where |
|---|---|---|---|---|
| per-track MUTE MODE (Immediate, Bar, Pattern) | `MixBoard::mute_mode[16]`, reset by nothing | none (new, OS-MUTE); the track's mute flag is `TRAK` | project: a property of how the song is played, next to the track's mute | `MUTE` |
| NOTE root, scale, octave, SCALE LOCK | `PatternEdit::entry` (`NoteEntry`, 4 bytes) | the OG project's pad board scale, root note and note offset | project: the key of the song | `NOTE` |
| JUMP (the edit step) | `PatternEdit::edit_step`, **not stored** | the OG project's pattern edit step | project (the OG importer still drops it as session state, `og.md`: unchanged) | `NOTE` |
| PERFORM SCENES 1..3 and LATCH | `PerformBank::scenes_`, `latch_`, cleared by a load | none (new, OS-PERF2); the slots a scene snapshots are `PERF` (OS-004d) | project, next to the slots | `SCEN` |
| send effects: DLY PINGPONG, SYNC, RATE, TIME, FEEDBACK; RVB SIZE, DAMPING, PREDELAY, DIFFUSE | `MixBoard::fx[12]` | the OG project's delay feedback, time and parameters and the reverb block (`OGPM` group 3) | project | `SNFX` |
| send effects: DLY TONE, DLY MODE, RVB QUALITY (new in OS-SENDFX) | `MixBoard::fx[12]` | none (new) | project | `SNFX` |
| MIDI CLOCK OUT, notes' ports and channel, FOLLOW, CC numbers, offsets | the link's `MidiConfig` | the config | **already persisted** by OS-004d (the old "not persisted" notes were stale) | `MIDI` |
| USB mode, profile, stem map | USB driver, `StemMap` | none | **already persisted** by OS-004d | `USBS` |
| mixer routing, returns, OUT MODE, CUE, IN level; Perform slots; SD app parameters | board, bank, `ModuleUi` | config and project | **already persisted** by OS-004d | `MIXR`, `PERF`, `APPM` |

Inventoried and **not** persisted by this slice: the pads' mode (NOTES, PIANO, TRACKS, ISO, STEP, PATS, VU: a performer's layout, not the song's; its home is a device row, but the device scope has three free rows of 16 and no page for it until the Config slice, so it waits for that); the recorder's configuration (source, gains, monitor, START ON, STOP AT, tap, region, FM frequency: an OG project file holds the source and gains, but they describe the hardware input and the radio region the device, so the project-or-device split is an open maintainer decision); the Sample load screen's wavetable preference (a screen preference); every cursor, page, selection, last-used value and play position (session state: the fields of an OG project file that the importer drops). The Amiga filter model has no control yet.

**`NOTE`** (7 bytes): `u16 version`, `u8 root` (0..11, the pitch class C..B; default 0), `u8 scale` (0..11, an index of the note panel's twelve scales in `app/note_entry.h`: Chromatic, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Pent. maj, Pent. min, Blues, Harm. minor, Whole tone; default 0), `u8 octave` (0..9; default 3), `u8 lock` (0 off, 1 on; default 0), `u8 jump` (0..32: the edit step, the distance the cursor moves after an entry; default 1).

**`MUTE`** (3 + n bytes, 19 written): `u16 version`, `u8 count` (1..16; writers write 16), `u8 mode[count]` (0 Immediate, 1 Bar, 2 Pattern; default 0). Tracks past `count` are Immediate.

**`SCEN`** (295 bytes): `u16 version`, `u8 latch` (0 off, 1 on; default 1, our choice), `u8 count` (3), then 3 scenes of 97 bytes: `u8 stored` (0 or 1), then 12 slots of `u8 fx`, `u8 row`, `i16 value[3]` (the layout of a `PERF` slot: `seq::PerfFx` ids, rows 0..3, values -100..100). **`SCEN` is version 1.1 when a slot of a stored scene holds the id 20 or 21** (ids 0..21), else 1.0 (ids 0..19); the rules, the writer's condition and what an older build does are those of `PERF` 1.1 above. A scene that is not stored holds 96 zero bytes. A scene is the slots as they were when it was stored: the FX, the selected row and the stored values (not the live values the encoder made, not the affected tracks, not the held pads); recalling it holds each value to its FX's own range, as a load does.

**`SNFX`** (3 + 2 rows bytes, 27 written): `u16 version`, `u8 rows` (1..32; writers write 12), then `u16 value[rows]`, one per row of the MASTER FX page's table (`app/send_fx_rows.h`, in this order; **a row is never renumbered, a new row is appended**):

| Row | Name | Range | Default |
|---|---|---|---|
| 0 | DLY PINGPONG | 0..1 | 0 |
| 1 | DLY SYNC | 0..1 | 0 |
| 2 | DLY RATE | 0..18 (this OS's 19 sync rates) | 0 |
| 3 | DLY TIME | 1..3500 ms | 500 |
| 4 | DLY FEEDBACK | 0..70 percent | 50 |
| 5 | DLY TONE | 0..100 percent | 100 |
| 6 | DLY MODE | 0 MONO, 1 STEREO | 0 |
| 7 | RVB SIZE | 0..100 percent | 50 |
| 8 | RVB DAMPING | 0..100 percent | 50 |
| 9 | RVB PREDELAY | 0..100 percent | 50 |
| 10 | RVB DIFFUSE | 0..100 percent | 80 |
| 11 | RVB QUALITY | 0 HALF, 1 FULL | 0 |

**Reading is tolerant and says so** (the rule of `SETS`, section "SETS and LRNB"; the strictness of the OS-004d chunks is for state that has no safe default). The shape of a chunk is the only error: a chunk version 0 or a newer major as always, a length outside the bounds below, a `count` or `rows` of 0 or above its maximum, a `MUTE` or `SNFX` whose length does not fit its count, a `SCEN` whose count is not 3 (`Malformed`). **A value outside its range reads as its default and is counted** (`Report::settings_skipped`, together with the entries `SETS` and `LRNB` skipped; `FormatReport::sanitised`; the app's load line says "N values reset"): `NOTE` field by field; a `MUTE` mode above 2; in `SCEN` a `latch` above 1, a `stored` above 1 (not stored) and, in a stored scene, an FX the chunk's version does not have (above 19 in 1.0, above 21 in 1.1: the slot is none, its row and values 0), a row above 3 (off) and a value outside -100..100 (0); in `SNFX` a value outside its row's range, and a row past the ones this version has (skipped; a row the chunk does not have is its default). A part that was read is present (the file said it), with the values that were taken. The 96 bytes of a scene that is not stored are ignored. A newer minor only appends: it is read as far as 1.0 goes, the rest is skipped at any length, `newer_minor` is set, and the appended fields are lost on the next save. Lengths of version 1.0: `NOTE` exactly 7, `MUTE` 3..19, `SCEN` exactly 295, `SNFX` 3..67 (a newer minor: at least that many).

**When it applies.** Opening a project (and making a new one) applies the four parts from the main loop once the load has ended (`App::apply_project_state`, never from an interrupt): **a project without the part sets the defaults**, so the last project's key, mute modes, scenes and effect settings do not leak into the next (a change from the runtime-only days, when the MUTE MODEs and the send controls survived a load). `NOTE` fills the note-entry settings and the edit step and lays the pad board out again; `MUTE` the tracks' mute modes; `SCEN` the bank's scenes and LATCH (after the `PERF` slots, whose load clears the scenes); `SNFX` the board's values, and the engine is told of each that differs from what the board held, as the parameter change of kind Master that the page itself sends (the effects' own smoothing, no click). Afterwards **every change made to the runtime is stored in the project** (it becomes unsaved, undo is not affected), as for `MIXR` and `PERF` (`App::sync_project_state`); a value that is already what the project holds is not written, so a load does not make the project unsaved.

**Older builds.** An older build (the one before OS-PERSIST2) keeps the four chunks as unknown chunks and writes them back byte for byte, in the order it read them: 348 payload bytes in four chunks, which with the five of OS-004d (1,152 bytes), a full OG import (`OGPM` 970, `OGPN` 12,822) is 15,292 of 16,384 bytes of the pool and 11 of its 16 chunks; with the `LAW` chunk of OS-BUG-PASTE24 (3 bytes, one more chunk, only in the copy of an OG project below version 7) it is **15,295 of 16,384 bytes and 12 of 16 chunks** (`project_state_test.cpp`, `persist2_format_test.cpp` and `law_format_test.cpp` hold the arithmetic). A file from before this slice (none of the four) reads here as the defaults of the tables above and is written back as it was. **Checked against the build before OS-PERSIST2** (built from its sources): its C++ reader and writer (`o3pj_tool resave`) and its Python reference (`o3project.py rewrite`) read the file of the `persist2` fixture, keep the four chunks as its unknown chunks and write the file back byte for byte; its writer and this one give the same bytes for every fixture that has none of the four (`empty` 186 bytes, `small` 2,648, `settings` 2,720, `state` 3,372, `maximal` 1,888,426: `persist2_format_test.cpp` holds the size and CRC-32 of `empty`, `small` and `state` as regression constants). `tests/o3project_roundtrip.py` also reads the new file with the Python reference as that build has it (the four ids unknown to it).

### The `LAW` chunk (OS-BUG-PASTE24): the instrument volume law

An instrument stores a bare level 0..100 (`INST`, the sampler's `volume`). Which gain that level is depends on the **law**, and the law is data of the project: **0** the 1.2+ instrument law (level 50 is 0 dB, 100 is +24 dB, the law every native project plays at) and **1** the 1.1.14 table (level 100 is 0 dB), which this OS applies to the copy of an OG project of a structure version below 7 (`audio::LevelLaw`, `Project::volume_law()`; both laws are this OS's model, and whether the official firmware plays the same curves is an open research question: PAR-023, maintainer answer Q4 of OS-CLEANROOM). Without this chunk the copy of such a project, saved natively, would play 24 dB louder at VOLUME 100 when it is loaded again.

`LAW ` (the fourth character is a space, as in `CRC `): `u16 version` (1.0), `u8 law`. Exactly 3 bytes, 12 in the file with its header and pad byte. It is the **last known chunk**: after `SNFX`, before the unknown chunks and `CRC `.

- **Written only when the law is 1.** A file without the chunk is law 0, the law every file written until now plays at, so no file written before changes meaning. Every project that is not the copy of an OG project below version 7 is the same bytes as before (`empty` 186, `small` 2,648, `settings` 2,720, `state` 3,372, `midi13` 3,336, `persist2` 3,032, `maximal` 1,888,426: `tests/o3project_roundtrip.py` holds the size and CRC-32 of each, from the build before this chunk). A chunk that holds law 0 is accepted and written back without the chunk. A project that holds law 1 is 12 bytes longer.
- **Reading.** The shape rules of every chunk: version 0 or a newer major is refused (`NewerMajor` for the latter), the payload is exactly 3 bytes for 1.0 (a newer minor: at least 3, the rest is skipped and `newer_minor` is set), a second `LAW` or a `LAW` before a chunk of a lower rank is `Malformed`. **A `law` byte above 1 is `Malformed`, in a newer minor as well** (`LAW: unknown law`): it is not read as a default, because a law this version does not know would play its levels up to 24 dB wrong, and the tolerant rule of `NOTE` and `SNFX` is for state that has a safe default. (Default of the OS-BUG-PASTE24 spec, to confirm by the maintainer.) The reader holds the value and applies it with `Project::set_volume_law` **at Done**, after the CRC matched: a load that fails after the chunk commits nothing, and the held value is cleared at the start of every load, so a `Reader` used for two files in a row cannot carry the first file's law into the second.
- **Runtime.** The law is the document's (`Project::volume_law()`); `Project::reset()` puts it back to 0, except for a KEEP INSTRUMENTS reset. A new sample instrument in a project of law 1 is born at 100, the table's 0 dB (`app::unity_level`); in a project of law 0 at 50.

**Older builds, and a downgrade hearing risk (decision of the maintainer, 2026-10-16: no major bump).** An older build (any build before this chunk) opens a file with `LAW`, keeps it as one unknown chunk, and writes it back byte for byte; the chunk is the first of the unknown ones, and the older writer puts the unknown chunks after the known ones in the order it read them, so the order `LAW`, `OGPM`, `OGPN` is kept. **It does not act on it: an older build plays the levels of that file at the 1.2+ law, so the native copy of an OG project below version 7 sounds about 24 dB louder at VOLUME 100 than this OS's law for that copy (+24 dB instead of 0 dB).** Do not open a project saved with the `LAW` chunk in a build older than the one that wrote it, with the master up. This is the very defect of OS-BUG-PASTE24, met by a downgrade; it does not make an existing file worse (such a copy played +24 dB before this chunk existed). The alternatives (a `PHDR` 2.0 only in the files that carry `LAW`, so that an older build refuses them, or in every file) were weighed and not chosen: they strand older builds from files that have no problem or from a file that is otherwise fine. The pool arithmetic of an older build with every chunk is under "Older builds" of the session state (15,295 of 16,384 bytes, 12 of 16 chunks).

### CRC

`u16 version`, `u32 crc` (section 2). Payload 6 bytes.

### Unknown chunks

Any other id (printable ASCII) is kept: up to 16 chunks and 16,384 payload bytes in all. Each is written back byte for byte (id, length, payload, pad) after the known chunks. A chunk that does not fit is skipped and counted: `Report::unknown_dropped` and `Project::unknown_dropped_chunks()` / `unknown_dropped_bytes()` tell the UI to say so; the load still succeeds.

### `OGPM` and `OGPN`: what an OG import carries (OS-004b)

Two chunks that the OG readers (`formats/og`) put in the pool of unknown chunks, so that a native project holds what an OG project held and the model has no field for, and Export -> OG can write it back (ADR 0006: OG -> OS -> OG is lossless for OG-only content). The core neither reads nor checks them: they are kept, re-emitted and counted like any unknown chunk, `o3project.py` decodes them in its dump, and `formats/og` validates them when it uses them (an invalid one is ignored and counted at Export, never a load error). Both begin with a chunk version (`0x0100`, 1.0; OGPM is 1.1, `0x0101`, when it carries the byte of a v17 project, below) and use the chunk conventions of this format (big-endian integers); the bytes of a group are OG file bytes, copied as they are (their integers and floats are little-endian). Full description: `docs/formats/og.md` sections 3 and 6.

**`OGPM`, the project settings.** `u16 version`, `u16 groups` (a bit set), then the groups whose bit is set, in bit order, each of a fixed length. A group is present only when it differs from the default Export writes. Valid: major 1, no bit outside the table, and a length of exactly the sum for version 1.0 (a newer minor may only append). At most 969 bytes for 1.0, 970 for 1.1.

| Bit | Group | Length | Bytes (project.mt file offsets) |
|---|---|---|---|
| 1 | delay | 4 | 0x11A, 0x11C-0x11D, 0x11E |
| 2 | limiter | 9 | 0x120-0x121, 0x124-0x127, 0x128-0x129, 0x12A |
| 3 | reverb | 16 | 0x418-0x427 |
| 4 | inputs | 10 | 0x134-0x139, 0x13C-0x13F |
| 5 | performance | 144 | 0x142-0x1BD, 0x2F5-0x308 |
| 6 | MIDI velocity | 16 | 0x309-0x318 of a version 2-7 project |
| 8 | 1.3-1.6 block | 306 | 0x4DE-0x5FF, then 0x309-0x318 of a version 13+ project |
| 9 | 1.7-1.9 block | 460 (1.1: 461) | 0x600-0x602, 0x643-0x80B (1.1: and 0x80C of a version 17 project, the last byte of the chunk) |

**OGPM 1.1** (OS-004c). Project.mt version 17 (official 1.9.2; open research question, unverified: no file backs it) is assumed to have a new one-byte field at 0x80C whose meaning is not known (`docs/formats/og.md` section 3). Zero is its default and is not carried; a non-zero value makes the chunk version 1.1, in which group 9 is one byte longer and that byte is the value. Group 9 is the highest group, so the byte is the last of the chunk, and a 1.0 reader skips it as it skips anything a newer minor appends. In 1.1 a chunk that has group 9 must have its byte (length at least the sum for 1.1, more is appended); a chunk without group 9 has no extra byte and is the same as in 1.0. A chunk that does not need the byte stays 1.0, so a project that was never a v17 one writes the chunks it always did.

**`OGPN`, the pattern names file.** `u16 version`, `u32 length` of the PAMD file (16..12,816), then the file's bytes without its trailing zeros (the first four are `PAMD`; the kept bytes are no longer than the file). Written back as `length` bytes: the kept bytes, then zeros. At most 12,822 bytes: with it in the pool little room is left for other unknown chunks.

**Versions.** A known chunk's version is read first, before its length is judged:

- **Newer major** (a larger high byte): the file is refused with `NewerMajor`, whatever the chunk's length or layout (the reader cannot know either). Version 0 is `Malformed`.
- **Newer minor** (same major, larger low byte than the reader knows for that chunk: 5 for `INST`, 1 for `PERF`, `SCEN`, `APPM` and `USBS`, 0 for the others): the new version only appends fields, so the chunk must still be at least as long as version 1.0 requires (`PHDR` 19, `TRAK` 3, `MODR` 3, `SONG` 3, `PATT` 7, `INST` 310 (1.1), `SETS` 4, `LRNB` 4, `MIDI` 20, `USBS` 5, `MIXR` 25, `PERF` 99, `APPM` 3, `NOTE` 7, `MUTE` 3, `SCEN` 295, `SNFX` 3, `LAW ` 3, `CRC ` 6 bytes, else `Malformed`); there is **no upper bound**. The reader parses the part of the version it knows (INST: 1.5, so including the rate, the engine, the twelve MIDI bytes and the stereo flag), skips whatever follows (it may be any size: a chunk read whole buffers at most the version 1.0 maximum and the rest is skipped in pieces; `PATT` is parsed as it streams and its tail skipped) and sets `Report::newer_minor`. Those appended fields are **lost on the next save**: the file is written back as the smallest version that holds the data this reader has (1.0; INST 1.1, 1.2, 1.3, 1.4 or 1.5 by the write condition in the INST section; PERF and SCEN 1.1 only when a slot holds the id 20 or 21; APPM 1.1 only when more than six apps are stored; USBS 1.1 only for 4 or 6 outputs), which is why the report says so. This holds for the `CRC ` chunk too: version 1.x with any number of bytes after the CRC value is a valid CRC chunk (the CRC value itself is still checked).
- **Same version (1.0; INST 1.0, 1.1, 1.2, 1.3, 1.4 or 1.5; PERF, SCEN and APPM 1.1; USBS 1.1)**: the exact bounds of that version apply (for example `CRC ` is exactly 6 bytes, `PHDR` 19..51) and unused bytes after the fields are `Malformed`. USBS 1.1 is exactly `5 + 2 * slots + 1` bytes (22): profile byte 1 (P8), 8 slots, then the count, 4 or 6; anything else is `Malformed`. A USBS newer than 1.1 is read leniently: the count is used only when profile byte 1, 8 slots and a count of 4 or 6 are all present, else the chunk is read as 1.0 (never refused for another profile or extra bytes).

## 4. Example: the default project (186 bytes)

```
46 4f 52 4d  00 00 00 b2  4f 33 50 4a            FORM, length 178, O3PJ
50 48 44 52  00 00 00 13                         PHDR, 19 bytes
  01 00 0c 20 00 00 32 c8 32 56 56 56 56 00      version 1.0, 12 active, length 32, tempo 0x32c8 = 13000,
  00 00 00 00 00 00                              swing 50, volumes 86, flags 0, 0 patterns, 0 instruments,
                                                 0 modules, name length 0, pad
54 52 41 4b  00 00 00 63  01 00 10 ...           TRAK, 99 bytes, version, 16 slots: 00 00 56 32 00 00 each,
                                                 pad
4d 4f 44 52  00 00 00 03  01 00 00  00           MODR, 3 bytes: no modules, pad
53 4f 4e 47  00 00 00 03  01 00 00  00           SONG, 3 bytes: length 0, pad
43 52 43 20  00 00 00 06  01 00  3b 09 f0 ae     CRC, version, crc32 of the first 172 bytes
```

(`native_format_test.cpp` holds the whole file as a literal and compares it; `o3project.py` writes the same bytes.)

## 5. The model and its arena

`o3os::model::Project` holds everything above with no knowledge of a screen, file or device: name, tempo, swing, default pattern length, active tracks, master, 16 track slots, a module table, the song, a pool of patterns, 64 instrument slots, and the unknown-chunk pool. Reads are free (`const` accessors). Writes go through the mutators (`set_step`, `set_track_length`, `set_order`, `create_pattern`, `set_instrument`...), which check the value, change it and bump the generation counters; a rejected mutation changes nothing and says why (`Status`). Undoable edits go through `model::Editor` (OS-002c), which calls the same mutators and journals what they changed in an in-RAM ring (not part of any file); a change made without it (a load, an import) clears that history.

**Generations**: `generation()` (any change), `tracks_generation()` (header, tracks, master), `song_generation()`, `pattern_generation(id)`, `instrument_generation(slot)`. Free-running u32, compare for equality.

**Storage.** No heap. The project lives in one `ModelArena`, a typed object the platform provides (a static in the host tests; on the MK66 the same object placed in the SDRAM section by the linker script). All limits are constants in `model/limits.h`; the layout is declared (alignment and size `static_assert`ed, identical on the 64-bit host and on the MK66 because the build proves it).

| Part | Bytes |
|---|---|
| pattern pool: 128 x 14,384 (1 + 16 + 31 + 16 x 128 x 7) | 1,841,152 |
| instrument pool: 64 x 720 (OS-STEREO step 2: the stereo byte, +4 each) | 46,080 |
| unknown-chunk pool | 16,384 |
| header, tracks, song (255), id map (256), modules, tables, generation counters | 2,520 |
| per-parameter generation counters (3,224 x 4), their epoch, the settings block (OS-002d); +2,304 for OS-INSTUI S3's nine source-page slots and +6,144 for S6's 24 MOD-page slots, +3,072 for S8b's twelve MIDI / VEL> slots (all included in the total below) | 13,248 |
| project-level state (OS-004d): MIDI 18, USB 26, mixer 22, Perform 96, six apps x 164, flags | 1,144 |
| app parameter sets 7..12 (OS-APPUX S3): six more x 164 | 984 |
| session state (OS-PERSIST2): note entry 5, mute modes 16, scenes 296, send effects 24, padding 3 | 344 |
| **`kModelArenaBytes`, worst case, everything full** | **1,933,760** (1.84 MiB, 11.5 % of the 16 MiB SDRAM; OS-STEREO step 2: the stereo byte of the 64 instruments, +256; E-CFG-03: the settings block holds 26 fields instead of 24, +8; OS-APPUX S3: twelve app sets instead of six, +984; E-SONG-07: the order list edit journal, runtime only and not saved, +80) |

That leaves 14.2 MiB of SDRAM, 2.9 MiB beyond this OS's sample memory budget of 11.25 MiB. The largest possible file is about 1.82 MiB (the maximal test fixture is 1,888,426 bytes); the reader refuses a `FORM` over 4 MiB.

**Why 128 patterns, not 255.** Pattern ids are 1..255 (the OG numbering, and what the song refers to) but at most 128 patterns exist at once. A full pool of 255 would be 3.5 MiB (21 % of the SDRAM) and squeeze the sample memory (the pool is 1.76 MiB at 128); the 8 OG fixture projects hold 104 patterns in all (`docs/formats/og.md` section 10). A file or an import that needs more than 128 is refused with "limit exceeded" (never silently trimmed). Raising `kMaxPatterns` to 255 costs another 1.76 MiB and changes nothing else (the `PHDR` count is a u16). The limit lives in `model/limits.h` and, for the check, in `o3project.py`.

## 6. Reading and writing

Both work over `core::ByteSource` / `core::ByteSink` (`core/byte_io.h`) and do no file I/O: the platform wraps an SD file or a test buffer. A source or sink may move fewer bytes than asked or answer `WouldBlock`; `End` (source) means the stream is over; a count above what was asked or `Ok` with no bytes is treated as a failure.

```cpp
formats::native::Reader r;                 // 2.7 KB: put it in a job slot or a static
r.begin(source, arena);                    // resets the arena's project
while (r.step(4096) == Progress::Running) {}   // at most 4096 bytes read per call; resumable
r.report();                                // error, detail, offset, chunk, counts, unknown kept/dropped
```

`Writer` is the same shape (`begin(project, sink)`, `step(budget)`). `begin()` touches nothing: the first steps run a budgeted sizing pre-pass (the `FORM` length is written first, so the whole project is measured before the first byte; it writes nothing, costs a pattern track the steps it scans against the budget, and does at least one item per call, so even `step(1)` returns promptly). `sized()` says when it is over and `total_bytes()` is valid. The project must not change while the writer runs, pre-pass included: it checks `generation()` at each step of the pre-pass and at each piece boundary and fails with `Modified`. A file written is a function of the model alone (canonical chunk order, minimal stored steps, zero pads), so saving twice, with any step budget, gives the same bytes, and `load` then `save` of a file this version wrote gives the file back byte for byte.

**A failed load leaves a valid empty project** (the arena's project is reset), never a half-filled one. The reader holds at most one 1 KiB piece of a chunk and one 7 x 128 byte pattern row at a time.

## 7. Errors

| Error | When |
|---|---|
| `Truncated` | the source ended before the `FORM` did (at any offset) |
| `BadMagic` | not `FORM` + `O3PJ` |
| `BadCrc` | the `CRC ` chunk does not match |
| `NewerMajor` | a chunk version with a larger major |
| `LimitExceeded` | more patterns, instruments or modules than the model holds (announced in `PHDR` or found while loading), or a `FORM` over 4 MiB |
| `Malformed` | anything else wrong: a length that does not fit, a value out of range, an unsafe path, a duplicate, a chunk out of order, a bad pad byte, a missing or misplaced `CRC `, counts that differ from `PHDR`... `Report::detail` says which |
| `SourceFailed`, `SinkFailed` | the byte stream failed or broke its contract |
| `Modified` | writer only: the project changed under it |
| `Internal` | writer only: its size formulas disagree with its output (a bug, never input) |

Chunks are parsed as they arrive, before the CRC is known (it is last), so a file with a wrong CRC is also validated; the CRC verdict wins only for files that parse cleanly. `Report` carries the byte offset and chunk id of the first problem.

## 8. Limits (one table, `model/limits.h`; `o3project.py` carries a copy that ctest compares)

| Item | Limit |
|---|---|
| track slots / default active | 16 / 12 |
| track length (per track of a pattern) | 1..128 (default 32) |
| pattern ids / patterns at once | 1..255 / 128 |
| song positions | 255 |
| instrument slots | 64 |
| modules, module blob | 16, 256 bytes |
| name lengths (characters) | project 32, pattern 30, track 20, instrument 32, sample path 96, module 31 |
| unknown chunks | 16 chunks, 16,384 bytes |
| settings (`SETS`) entries, Learn (`LRNB`) bindings | 64, 64 (this version keeps 16 settings per scope and 32 bindings) |
| project-level state (OS-004d): MIDI ports / CC slots / offset | 2 / 6 / +-1000 (0.1 ms) |
| stem slots, mixer strips, Perform slots / FX ids | 11, 17, 12 / 0..19 (`PERF` and `SCEN` 1.0), 0..21 (1.1) |
| apps remembered / values per app | 12 (`APPM` 1.0: 6) / 32 |
| session state (OS-PERSIST2): Perform scenes / note scales, octaves, jump / mute modes / send effect rows | 3 / 12, 10, 32 / 3 / 12 (a chunk holds up to 32 rows) |
| tempo, swing, levels | 40.00..800.00 BPM, 25..75, 0..100 |
| file | 4 MiB |

## 9. Samples

Samples are standard WAV files next to the project file; an instrument stores the relative path. A path is `""` or components separated by single `/`, none empty, `.` or `..`, no leading or trailing `/`, no `\`, no `:`, no control characters. The reader and the model refuse anything else, so a project can never point outside its folder. The PCM itself is not in the project file; the instrument records the sample's rate (`sample_rate`, INST 1.1) and the OG's positions (start, loop, end, slices) in the sampler block. An imported WAV makes such an instrument (`samples/instrNN.wav`, the rate of the file, its `smpl` loop and `cue` points as positions): `docs/architecture/os-tree.md`, "WAV as a registered format".

## 10. Evolution

- A field appended to a chunk is a **minor** bump: old readers skip it and say so.
- A new chunk kind needs no bump: old readers keep it as unknown (within the pool) and write it back. (`SETS` and `LRNB` of OS-002d are the first: they sit where the unknown chunks are written, so a reader that does not know them preserves them.)
- A change of meaning, or a removed or reordered field, is a **major** bump: old readers refuse the file, and the new reader gets a loader for the old major.
- OS-004d, the first use of these rules after the format was written: five new chunks (no bump: an older build keeps them as unknown chunks, see "Older builds" in section 3) and one new field (`INST` 1.1 to 1.2, a minor bump with a loader for 1.1: engine 0). Compatibility, both ways: an older file in a new build reads as the defaults, and the new build writes the file it always wrote for a project that has none of the new state (the default project is still 186 bytes, a small fixture is byte-identical); a new file in an older build loads, keeps the chunks, loses only the engines (a newer minor of `INST`). Round trips: `tests/project_state_test.cpp` (the exact bytes written from the description, the older file, the strict reader), ctest `project_python` (C++ against `o3project.py`, both ways, the same verdict on damaged state chunks).
- OS-INSTUI S8b + S9: `INST` 1.2 to 1.3, twelve appended bytes (the MIDI instrument's VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET, CC A..F and a sample instrument's VEL>), the engine byte always present in a 1.3 chunk. Compatibility, both ways: an older file (1.1, 1.2) in the new build reads as the defaults and the new build writes the file it always wrote for a project that never used the fields (byte for byte, size and CRC-32 pinned against the build before INST 1.3); a new file in an older build loads, keeps the instrument whole and loses only the twelve bytes (a newer minor of `INST`). Round trips: `tests/inst_midi_format_test.cpp` (exact bytes, the write condition, the older files, damaged values, the length), ctest `project_python` (the `midi13` fixture: C++ against `o3project.py`, both ways).
- OS-LFOSINE: `INST` 1.3 to 1.4, no appended byte: the version says that an LFO shape of 5 (Sine) is valid; written only for an instrument with a Sine LFO; an older chunk's shape of 5 or more reads as Triangle (`docs/specs/OS-LFOSINE.md`).
- OS-STEREO step 2: `INST` 1.4 to 1.5, one appended byte (the instrument's stereo flag); written only for a Sample instrument with the flag set; an older chunk and a value above 1 read as 0; a flag on a MIDI or Module instrument reads 0. Compatibility, both ways: an older file in the new build reads mono and the new build writes the file it always wrote for a project without a stereo instrument (size and CRC-32 of every fixture pinned); a new file in an older build loads, keeps the instrument whole and loses only the flag (a newer minor of `INST`). Round trips: `tests/inst_midi_format_test.cpp` (the 1.5 cases), ctest `project_python` (the `stereo` fixture: C++ against `o3project.py`, both ways).
- A new persistent part is a new chunk written only when the project has it, and a new field a minor bump written only when it is not its default: that keeps every file that never used it as it was.
- OS-PERSIST2: four new chunks (`NOTE`, `MUTE`, `SCEN`, `SNFX`; no bump of any chunk, no field changed) for the settings that were still runtime only. Compatibility, both ways: an older file in a new build reads as the defaults and is written back as it was; a new file in an older build loads, keeps the four as unknown chunks and writes them back byte for byte (the pool arithmetic is in section 3). New is the tolerant reading of a value out of range (its default, counted), which `SETS` already had. Round trips: `tests/persist2_format_test.cpp` and `persist2_app_test.cpp`, ctest `project_python` (the `persist2` fixture, both ways, the same reading of damaged chunks).

- OS-PERFFX P1: two new Perform FX ids, 20 BITDEPTH and 21 OVERDRV, in the byte of `PERF` (and `SCEN`) that was always there; **`PERF` and `SCEN` 1.0 to 1.1, a minor bump with a loader for 1.0** (decision D5, maintainer 2026-10-19, option A: no field, no length and no layout changes, the version only says that the ids 20 and 21 are valid). The writer writes 1.1 only when a slot holds one of the two ids, so a project that never used them is the bytes it always was. Compatibility, both ways: an older file in a new build reads as it always did (ids 20 and 21 in a 1.0 chunk are none); a new file in an older build loads, reads the chunk as a newer minor ("newer fields skipped" on the load line), reads the two ids as none (the slot's row and values reset) and writes the chunk back as 1.0 with those slots empty, so a downgrade through a save is not lossless (see `PERF`). Tests: `tests/project_state_test.cpp` (PERF: 1.0 byte for byte without the ids, 1.1 round trip, a 1.1 and a 1.0 chunk read), `tests/persist2_format_test.cpp` (the same for SCEN), ctest `project_python` (C++ against `o3project.py`, both ways, the same reading).
- OS-BUG-PASTE24: one new chunk, `LAW ` (the instrument volume law; no bump of any chunk, no field changed, no major bump: maintainer decision 2026-10-16), written only when the law is the 1.1.14 table, so every other file is the bytes it always was. Compatibility, both ways: an older file in a new build is law 0 and is written back as it was; a new file in an older build loads, keeps the chunk as unknown and writes it back byte for byte, **but plays the levels at the 1.2+ law (a downgrade hearing risk, section 3, "The `LAW` chunk")**. Round trips: `tests/law_format_test.cpp`, ctest `project_python` (the `law` fixture: C++ against `o3project.py`, both ways, the same reading of damaged chunks). Checked against the build before this chunk (built from its sources, see the commit of the slice): its C++ reader and writer and its Python reference keep the `LAW` chunk of the `law` fixture and write the file back byte for byte.

- OS-APPUX S3: `APPM` 1.0 to 1.1, the same layout with a count of up to 12 apps (it was 6) and a payload of up to 1,015 bytes; written as 1.0 while six apps or fewer are stored (every file that never used more is the bytes it always was), as 1.1 beyond; a loader for 1.0 (at most 6) stays; the model arena grows by 984 B (six more 164-byte sets: 1,933,416 B). An older OS refuses a 1.1 `APPM` (`APPM: bad app count`) and with it the whole project (maintainer, 2026-10-19, option B: no appended tail); no other chunk changed.
- OS-PRFDEFAULTS: no bytes change and no version moves. The defaults of the `PERF` values (what a project with no `PERF` chunk reads as, and what a new project holds) are the table of `docs/specs/OS-PERFFX.md` section 9 instead of 0 (maintainer decision 2026-10-23); a chunk that is present loads exactly as stored, and a project that never touched Perform is still written without the chunk.
- OS-PERFFX P1 + OS-APPUX S3 together: the two version gates are independent. `PERF` and `SCEN` are 1.1 only with the Perform FX 20 or 21 in a slot (of a stored scene for `SCEN`); `APPM` is 1.1 only with more than six apps stored; every other chunk stays 1.0. A project that uses neither is the file it always was; one that uses one feature bumps only that feature's chunks; one that uses both writes all three as 1.1. Every combination is read (a 1.1 `PERF` with six apps, a 1.0 `PERF` with seven apps, and so on) and written back as the versions its content needs. An older OS opens the file when no `APPM` is 1.1 (it reads the two FX ids as none) and refuses it when `APPM` is 1.1. Tests: `tests/project_state_test.cpp` ("the version gates are independent"), ctest `project_python` ("PERF/SCEN 1.1 ... and APPM 1.1 in one project", C++ against `o3project.py`).

## 11. Known gaps (for the slices that need them)

- Per-pattern tempo and swing of the OG pattern header are not stored (the project has them).
- An OG step has six bytes and no volume byte; the model's step has a volume byte of its own, and Export writes it as FX type 18 (`og.md` section 4). The mapping is OG import's.
- Engine FX parameters of the master section (the limiter's), automation lanes (ADR 0009: open) and module state beyond the blob arrive as new chunks. The send effects' controls are `SNFX` since OS-PERSIST2; the OG's own delay and reverb values (`OGPM` groups 1 and 3, kept verbatim by the importer) are not mapped into `SNFX` yet (open: an import would fill it, an Export would read it back), and until they are `SNFX` takes precedence over `OGPM` when both exist.
