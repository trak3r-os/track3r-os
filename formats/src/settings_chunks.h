// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal to formats: the two chunks that carry settings (docs/formats/o3-settings.md section 3), shared by the native project
// format (which holds the project's settings and Learn bindings in them, docs/formats/o3-project.md) and the device settings
// file (formats/settings.h). The same bytes in both: a settings block of either scope is written and read by one codec.
//
//   SETS  u16 version, u16 count, then count x { u32 ParamId, f32 value }: the value of every parameter of the scope
//   LRNB  u16 version, u16 count, then count x { u32 ParamId, u8 cc, u8 channel, u8 mode, u8 flags }: the Learn bindings in use
//
// Reading is tolerant by design (ADR 0009 rev 1: default-fill for old or unknown data): an entry for a parameter this version does
// not have, or with a value outside its range, or a binding that is not valid, is counted and skipped and the setting keeps its
// default; a setting the file does not mention keeps its default. Only the shape of the chunk (its length against its count) is
// an error.

#include "native_codec.h"

#include <o3os/model/settings.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::settings_chunks {

inline constexpr std::uint32_t kChunkSets = native::kChunkSets;
inline constexpr std::uint32_t kChunkLrnb = native::kChunkLrnb;

inline constexpr std::uint16_t kVersion = 0x0100;
inline constexpr std::size_t kFixed = 4;   // version and count
inline constexpr std::size_t kEntry = 8;   // one entry of either chunk
// What a version 1.0 chunk may hold, which is more than this version keeps (room to grow without a format change): a SETS has an entry for
// each setting of a scope (this version: at most Settings::kMaxFields, 32 since OS-MIDICCMAP S1, 24 since OS-006e, 16 before), an LRNB one for each binding of a store (this version: at most
// core::kLearnSlots, 32; the rest of a longer one is skipped and counted).
inline constexpr unsigned kMaxSetsEntries = 64;
inline constexpr unsigned kMaxLrnbEntries = 64;
inline constexpr std::size_t kSetsMax = kFixed + kMaxSetsEntries * kEntry;  // 516
inline constexpr std::size_t kLrnbMax = kFixed + kMaxLrnbEntries * kEntry;  // 516

static_assert(model::Settings::kMaxFields <= kMaxSetsEntries, "a SETS chunk holds every field of a scope");
static_assert(core::kLearnSlots <= kMaxLrnbEntries, "an LRNB chunk holds every binding of a store");

// Payload sizes of what `s` writes: SETS holds every field (the writer decides whether to write it at all), LRNB the bindings in use.
std::size_t sets_payload_size(const model::Settings& s) noexcept;
std::size_t lrnb_payload_size(const model::Settings& s) noexcept;
void encode_sets(native::codec::Enc& e, const model::Settings& s) noexcept;
void encode_lrnb(native::codec::Enc& e, const model::Settings& s) noexcept;

struct Counts {
  std::uint32_t skipped = 0;  // entries not taken: an unknown parameter, a value out of range, an invalid or conflicting binding
};

// Decode into `out` (a block of the scope being read, defaults to begin with). The cursor is at the start of the payload (the
// version is read here). `newer_minor`: the chunk is of a newer minor version, which only appends: the length may exceed what
// the count needs, and a count above the maximum is read as far as the maximum (the rest counts as skipped, whether or not the
// bytes are there to see: the native reader hands over at most kSetsMax / kLrnbMax of them). False: the chunk's shape is wrong
// (a count above the maximum in a chunk of this minor, length not matching, too short for the entries that are read), nothing applied.
bool decode_sets(native::codec::Cursor& c, bool newer_minor, model::Settings& out, Counts& counts) noexcept;
bool decode_lrnb(native::codec::Cursor& c, bool newer_minor, model::Settings& out, Counts& counts) noexcept;

}  // namespace o3os::formats::settings_chunks
