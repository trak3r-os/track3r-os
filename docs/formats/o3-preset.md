# The module preset file (`O3MP`), version 1

Status: specified in `docs/specs/OS-MODPRESET.md` (decisions of 2026-11-05), the format implemented with slice P1 and the Python reference with P2; the card side (paths, save, list, load) is P3 and P4 · **Level C**: a persistent format, so a change of layout, size or meaning needs a version bump, a loader that still reads every older version and a round-trip test (`CLAUDE.md`, persistent formats) · Every behaviour here is **new Community OS behaviour** (the official firmware's only presets are the Perform presets).

Code: `formats/include/o3os/formats/preset.h` (`o3os::formats::preset`), `formats/src/preset_file.cpp`; Python reference `tools/o3preset.py`. Tests: `tests/preset_format_test.cpp` (doctest, label `formats`), `tests/o3preset_test.py` (ctest `preset_python`), the reference file `tests/data/preset_basic.hex` that both hold their bytes equal to, and the corpus of good and damaged files that `preset_python` writes with the outcome its independent reader gave and `preset_format_test` holds the C++ reader to (ctest fixture `preset_corpus`). The conventions of the container (big-endian, chunk, pad, version, CRC) are those of `docs/formats/o3-project.md` sections 1 and 2; the file is small like the settings file (`docs/formats/o3-settings.md`) and read and written the same way, whole, through a 1 KiB buffer.

A preset is the named parameter values of one module instance: an instrument (TB-303, FM, Chip, SID, a card module) or an insert effect. The values are stored **by the parameter's name, not its index**, so a module that adds or reorders parameters in a later version still loads an old preset (OS-MODPRESET Q5). The file has no name of its own: its file name is the preset's name (A-1). This layer knows nothing of modules or of `app::ParamSet`; matching names to a module's descriptor, clamping and the defaults for what a file lacks are the card side's job (P3).

## 1. Container

```
"FORM" u32 length "O3MP"
  "MPST" u32 n   payload (n bytes) [pad]
  ...unknown chunks (ignored, not kept)
  "CRC " u32 6   u16 0x0100   u32 CRC-32/ISO-HDLC of every byte before this chunk's header
```

`length` counts the 4 bytes `O3MP` and the chunks with their headers and pads; it is even and at least 4. The file is `length + 8` bytes. The CRC is the CRC-32 of zlib, PNG and Ethernet (`zlib.crc32`; check value of `"123456789"` is `0xCBF43926`). The `CRC ` chunk is the last: nothing sits between it and the end of the FORM. `MPST` appears exactly once, before `CRC `; chunks of another kind may be anywhere before `CRC ` (a newer version's, or someone else's), each a printable four-character id.

## 2. The `MPST` chunk, 1.0

| Field | Type | Rule |
|---|---|---|
| version | u16 | `0x0100`: major in the high byte, minor in the low byte |
| module | str | the module's file stem, 1..31 bytes, none of `/ \ : * ? " < > \|` or a control character, not `.` or `..` (the loader's `module::is_stem`) |
| kind | u8 | `1` instrument (`O3M_KIND_INSTRUMENT`) or `2` effect (`O3M_KIND_EFFECT`) |
| module_version | u32 | the module's version as its header holds it; informative, never refused on |
| count | u8 | 0..32 |
| entries | count x (str name, f32 value) | name 1..15 bytes of printable ASCII (`0x20..0x7E`, a space is allowed); the value is the IEEE-754 binary32 pattern, finite |

`str` is one length byte and that many bytes, no NUL inside. A later minor appends fields after the last entry. Sizes: the smallest file (an empty preset of a one-character stem) is 44 bytes; the largest (a 31-byte stem, 32 entries of 15-byte names) is 714; `kMaxFileBytes` is 1,024, as the settings file's.

The reference preset is `tests/data/preset_basic.hex` (96 bytes: module `tb303`, instrument, module version `0x00010000`, entries `Cutoff` 0.5, `Resonance` 0.25, `Env Mod` 1.0, `Decay` 2.5), written by hand from this section, not by either implementation.

## 3. Reading

`formats::preset::read(const uint8_t*, size_t, Preset&, Report&)` is pure: no heap, no I/O, never a read past the buffer, the whole file in one call. It returns an `Error` (also in `Report::error`) and leaves the `Preset` zeroed on any error: a file is never half-read.

| Error | When |
|---|---|
| `Truncated` | an empty file or one of under 4 bytes; under 12 bytes that start with `FORM`; fewer bytes than the FORM's own length says |
| `BadMagic` | the first four bytes are not `FORM`, or (from 12 bytes) the type is not `O3MP`. The magic is checked first: a file of any size that is not an `O3MP` FORM is this, never `TooBig`, and so is a file of 4 to 11 bytes whose first four bytes are not `FORM` |
| `BadCrc` | the CRC chunk does not match |
| `NewerMajor` | the `MPST` (or the `CRC `) chunk has a major version above 1 |
| `Malformed` | a FORM length that is odd or under 4; a chunk that overruns the FORM or whose header does not fit; a pad byte that is not zero; a chunk version of major 0; a chunk id that is not four printable characters; no `MPST` or two of them; a missing or misplaced `CRC ` chunk (not last, or of a length other than 6 at version 1.0); a stem, kind or name that is not valid; a count over 32; fewer entries than the count says; bytes after the last entry at version 1.0; a value that is NaN or infinite (the writer never writes one, so a file that holds one is damaged, the project file's rule) |
| `TooBig` | an `O3MP` FORM (the magic is right) in a file longer than 1,024 bytes, whatever its FORM says; or a FORM length over 1,016 |

Bytes after the FORM, within that limit, are not looked at. **The caller passes the file's real size:** a longer file read into a 1,024-byte buffer would be cut to a size that may pass, so the card side reads up to 1,025 bytes (or checks the size first) and `read` says `TooBig` for a 1,025. The errors are found in file order: a chunk's header, length, pad and version before the next chunk, the CRC when its chunk is reached, then the fields of `MPST`.

**Versions** (`docs/formats/o3-project.md` section 3): a newer major is refused (`NewerMajor`); a newer minor of `MPST` or `CRC ` is read as far as this version knows it and `Report::newer_minor` is set (for `CRC `, a longer payload is read as 1.0); a chunk of another kind is ignored and counted in `unknown_chunks`, not kept (a preset is never saved over, so nothing is dropped, A-7). **Every entry is kept, in file order, repeated names too:** a card module may legally have two parameters of one name, and the card side matches the entries to the module's parameters in that order (the first parameter of that name not yet matched, ASCII case-insensitive). `Report::duplicates` counts the entries whose name (compared exactly) an earlier entry has: information only, nothing is dropped; `Preset::count` is the count the file says.

On the target only the `Error` and the `offset` of the report are filled; `detail` (a text for people) is built in on the host only.

## 4. Writing

`formats::preset::write(const Preset&, uint8_t*, size_t cap)` emits exactly the 1.0 layout and returns its size, or 0 when the preset is not `valid()` or does not fit `cap`. Deterministic: the same preset gives the same bytes; writing what `read` gave gives the file back (without any unknown chunk). `valid()` is what the reader accepts: a stem, a kind, a count of at most 32, names of 1..15 printable ASCII characters (a name may repeat, see section 3), finite values. -0.0 and denormals are written as they are.

## 5. The Python tool (`tools/o3preset.py`)

An independent implementation of the same bytes, standard library only, run with `python3 -I`:

- `dump FILE` prints every field (module, kind, module version, count, each entry with its value and its bit pattern), and what a reader reports (duplicates, unknown chunks, a newer minor); exit 1 and the error kind when the file is not valid.
- `make --module STEM --kind instrument|effect [--module-version N] --set Name=value ... -o FILE` writes one file, the values in the order given.
- `pack PRESETS.json OUTDIR` writes `OUTDIR/<name>.o3mp` for every preset of a pack and prints the paths; `OUTDIR` is the module's folder (on the card `modules/presets/<stem>/`). A pack is `{"module": "tb303", "kind": "instrument", "module_version": 65536, "presets": [{"name": "Acid Bass", "values": {"Cutoff": 0.4, ...}}]}`; the values keep their order. `values` may also be a list of `[name, value]` pairs, in the module's order, which is how a module with two parameters of one name (a card `.tmod` may have them) is written: an object cannot hold a key twice, and a JSON key twice is refused. Nothing is written when any preset is refused: a non-finite value (a JSON `NaN` or `Infinity`, or a number past the 32-bit floats), a parameter name over 15 bytes or not printable, more than 32 values, a bad stem or kind, a preset name that is not a file name, two preset names that FAT takes for one (case-insensitive). **A file name** is the save guard's `valid_name` (`hal/include/o3os/hal/save_guard.h`) at most 30 bytes long (the longest card path `modules/presets/<31>/<30>.o3mp` is 84 bytes, inside the guard's 96): letters, digits, space, `_`, `-` or `.`, the first a letter or digit, the last neither a space nor a dot, **no `..` anywhere**, and **not a name Windows reserves for a device**: `CON`, `PRN`, `AUX`, `NUL`, `COM1`..`COM9`, `LPT1`..`LPT9`, in any case, with or without an extension (the part before the first dot is what is compared).

`tests/o3preset_test.py` holds `make` of the reference preset equal to the hex fixture, the CRC equal to `zlib.crc32`, and every good and damaged file it tries (a cut at every byte, every bit flipped, the refusals of section 3, repeated names, an unknown chunk, a newer minor, a file over 1,024 bytes) read with the error the spec says. It writes them all to `corpus.txt` (one line a file: the hex, the outcome and, for a file that reads, its fields), and `tests/preset_format_test.cpp` reads that file and holds the C++ reader to the same outcome and the same fields: the two readers are checked against one list, not against two copies of it.

## 6. Evolution

Any change of layout, size or meaning of `MPST` is a new version: a minor appends fields after the last entry (old readers read 1.0 and say `newer_minor`), a major is a new reader that still reads 1.0. The hex fixture stays: it is the bytes of 1.0 for ever.
