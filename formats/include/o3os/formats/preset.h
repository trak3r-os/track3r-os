// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/native.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::preset {

// The module preset file (OS-MODPRESET P1, docs/formats/o3-preset.md): an IFF-style container like the settings file (`FORM` + `O3MP`, one
// `MPST` chunk, `CRC `), holding the named parameter values of one module instance (an instrument or an insert effect), by the parameter's
// name, not its index. It is at most kMaxFileBytes (1,024 bytes; 714 at the largest), so like the settings file it is read and written whole
// from and to a buffer: the card side reads the file into 1,025 bytes (see read() below), calls read(), and calls write() into 1 KiB to save. No file I/O, no heap,
// no state, never a read past the buffer. This layer knows nothing of modules or of ParamSet: matching a file's names to a descriptor is the
// card side's job (OS-MODPRESET P3).
//
// Version rules (docs/formats/o3-project.md section 3, as the settings file): a newer major is refused (NewerMajor), a newer minor is read as
// far as this version knows it (newer_minor), a chunk of another kind is ignored (unknown_chunks, not kept: a preset is never saved over, so
// nothing is dropped). Every entry is kept in file order, repeated names too (a card module may have two parameters of one name; the card side
// matches them in that order); a name seen before is only counted (duplicates). Any error leaves `out` cleared.

inline constexpr std::uint32_t kTypeO3mp = native::fourcc('O', '3', 'M', 'P');
inline constexpr std::uint32_t kChunkMpst = native::fourcc('M', 'P', 'S', 'T');  // the preset: module, kind, version, named values
inline constexpr std::uint16_t kVersion = 0x0100;                                 // MPST 1.0 (major in the high byte)

inline constexpr std::size_t kMaxFileBytes = 1024;
inline constexpr std::size_t kStemMax = 31;   // = module::kStemMax (the loader's file stem)
inline constexpr std::size_t kNameMax = 15;   // = the module SDK's parameter name (o3module.h)
inline constexpr std::size_t kMaxEntries = 32;  // = the module SDK's parameter count

inline constexpr std::uint8_t kKindInstrument = 1;  // = O3M_KIND_INSTRUMENT
inline constexpr std::uint8_t kKindEffect = 2;      // = O3M_KIND_EFFECT

struct Entry {
  char name[kNameMax + 1] = {};  // 1..15 printable ASCII characters, NUL-terminated
  float value = 0.0f;            // finite
};

// The decoded file (about 680 bytes). Unused entries are zero.
struct Preset {
  char module[kStemMax + 1] = {};  // the module's file stem, NUL-terminated (is_stem)
  std::uint8_t kind = 0;           // kKindInstrument or kKindEffect
  std::uint32_t module_version = 0;  // the module's version as its header holds it: informative, never refused on
  std::uint8_t count = 0;          // 0..kMaxEntries
  Entry entries[kMaxEntries] = {};
};

enum class Error : std::uint8_t {
  None,
  Truncated,   // fewer bytes than the FORM says, or than a FORM header (a file of under twelve bytes that starts with FORM, or of under four bytes)
  BadMagic,    // not an O3MP FORM: the magic is checked first, so any file whose first four bytes are not FORM (or, from twelve bytes, whose type is not O3MP) is this, whatever its size
  BadCrc,      // the CRC chunk does not match
  NewerMajor,  // the MPST (or the CRC chunk) of a newer major version
  Malformed,   // a length that does not fit, a bad pad, no MPST or two, a missing or misplaced CRC chunk, a bad stem, kind or name, a count over 32,
               // fewer entries than the count says, a non-finite value (the writer never writes one: a file that holds one is damaged)
  TooBig,      // the file is longer than kMaxFileBytes (and its first bytes are FORM and O3MP: the magic is checked first)
};

const char* error_name(Error e) noexcept;

struct Report {
  Error error = Error::None;
  const char* detail = "";             // static text: what was wrong, "" when nothing was
  std::uint32_t offset = 0;            // where in the file it was found
  std::uint32_t chunk = 0;             // the fourcc of the chunk being read
  std::uint32_t duplicates = 0;        // entries whose name an earlier entry has: information only, every entry is kept
  std::uint32_t unknown_chunks = 0;    // chunks that are neither MPST nor CRC: ignored
  bool newer_minor = false;            // a chunk of a newer minor version: fields it appends were not read
};

// Parses `size` bytes into `out` (cleared first, and again on any error) and fills `report`. Once the magic is right, a `size` over kMaxFileBytes is
// TooBig whatever the FORM says, so the caller must pass the file's real size: reading into a 1,024-byte buffer would cut a longer file to a size
// that may pass (read up to 1,025 bytes, or check the size first). Bytes after the FORM, within that limit, are not looked at. On the target only the
// Error and the offset of the report are filled; `detail` is "" there.
Error read(const std::uint8_t* data, std::size_t size, Preset& out, Report& report) noexcept;

// Writes the preset into `out` and returns the size in bytes; 0 when it is not valid() or does not fit `cap` (kMaxFileBytes always suffices).
// Deterministic: the same preset gives the same bytes. Emits exactly the 1.0 layout.
std::size_t write(const Preset& p, std::uint8_t* out, std::size_t cap) noexcept;

// True when `p` is something write() will write and read() will give back unchanged: a stem, a kind, a count of at most 32, names of 1..15
// printable ASCII characters (a name may repeat), finite values.
bool valid(const Preset& p) noexcept;

// The loader's rule for a file stem (module::is_stem, which this layer may not include): 1..31 characters, none of / \ : * ? " < > | or a control
// character, not "." or "..". tests/preset_format_test.cpp holds the two equal.
bool is_stem(const char* text) noexcept;

// True when the first bytes look like this format (FORM and O3MP).
bool sniff(const std::uint8_t* head, std::size_t n) noexcept;

}  // namespace o3os::formats::preset
