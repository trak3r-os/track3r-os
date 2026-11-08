// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S1: the header of a tracker-ce `.ptfx` Sample Editor plugin, parsed from untrusted bytes (docs/specs/OS-CEPTFX.md 2.1 and 3.1). Pure: no
// execution, no heap, explicit little-endian reads (no packed struct). The refusals, the tolerated oddities and the sanitising rules are the spec's.
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3os::module {

constexpr std::uint32_t kPtfxHeaderSize = 472;
constexpr std::uint32_t kPtfxMagic = 0x58465450u;  // "PTFX"
constexpr std::uint16_t kPtfxVersion = 1;
constexpr std::uint16_t kPtfxFlagGenerator = 0x01;
constexpr std::uint16_t kPtfxFlagStereo = 0x02;
constexpr std::uint16_t kPtfxFlagWavetable = 0x04;
constexpr std::uint16_t kPtfxFlagConvertToMono = 0x08;
constexpr std::uint16_t kPtfxFlagImxOnly = 0x10;
constexpr std::uint32_t kPtfxMaxParams = 12;
constexpr std::uint32_t kPtfxMaxListItems = 16;
constexpr std::uint32_t kPtfxMaxCode = 32u * 1024u;      // align4(code_size) + bss_size, the MK66 limit
constexpr std::uint32_t kPtfxMaxScratch = 640u * 1024u;  // scratch_size is clamped to this

enum class PtfxStatus : std::uint8_t {
  Ok,
  NotPtfx,             // NOT A PTFX PLUGIN
  VersionUnsupported,  // PTFX VERSION n NOT SUPPORTED
  NotForThisTracker,   // NOT FOR THIS TRACKER
  NeedsNewerOs,        // NEEDS NEWER OS
  FileDamaged,         // PLUGIN FILE DAMAGED
  TooBig,              // PLUGIN TOO BIG
};

struct PtfxParam {
  char label[9];  // up to 8 characters, printable ASCII, NUL-terminated
  char unit[7];   // up to 6 characters (empty for a list: its offset and count are in list_offset and list_count)
  char type;      // 'f', 'd' or 'l' after sanitising
  bool editable;  // false for a non-finite or reversed range
  float min, max, def, step;
  std::uint16_t list_offset;  // 'l': offset of the first string in the payload
  std::uint8_t list_count;    // 'l': declared item count, capped at 16
};

struct PtfxInfo {
  PtfxStatus status;
  std::uint16_t version;  // as read (for the "VERSION n" message)
  std::uint16_t flags;
  char name[33];  // display name, printable ASCII
  char author[17];
  std::uint8_t num_params;  // clamped to 12
  std::uint32_t scratch_size;
  std::uint32_t code_offset, code_size, bss_size;
  std::uint32_t init_offset, set_param_offset, process_block_offset;  // bit 0 (Thumb) masked
  PtfxParam params[kPtfxMaxParams];
};

// Parses `len` header bytes of a file of `file_size` bytes. `info.status` is the verdict; the other fields are valid only when it is Ok (the version and
// flags also for the refusals that read them). Returns info.status.
PtfxStatus ptfx_parse_header(const std::uint8_t* bytes, std::size_t len, std::uint64_t file_size, PtfxInfo& info) noexcept;

// The number of usable items of a list parameter inside `payload`: the declared count capped at 16, stopping at the first item that has no NUL inside the
// payload or starts outside it. 0 means the parameter is shown as an integer.
std::uint32_t ptfx_list_count(const std::uint8_t* payload, std::size_t payload_len, const PtfxParam& p) noexcept;

// Item `index` (< ptfx_list_count) as printable ASCII, up to 10 characters, into out[11]. Returns false when the item is not usable.
bool ptfx_list_item(const std::uint8_t* payload, std::size_t payload_len, const PtfxParam& p, std::uint32_t index, char (&out)[11]) noexcept;

}  // namespace o3os::module
