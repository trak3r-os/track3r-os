// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal to formats/native: the five chunks that carry the project-level state that used to be runtime only (OS-004d, docs/formats/o3-project.md section 3; model/project_state.h):
//
//   MIDI  u16 version, u8 out_ports, u8 note_ports, u8 notes_channel, u8 follow, u8 cc_number[6], i16 in_offset_dms[2], i16 out_offset_dms[2]            (20 bytes)
//   USBS  u16 version, u8 mode, u8 profile, u8 slots, u16 mask[slots]                                                                                   (5 + 2 slots)
//         1.1 (OS-USBOUTS, only for 4 or 6 outputs): profile 1 (P8), slots 8, u16 mask[8], u8 channels (4 or 6)                                                (22 bytes)
//   MIXR  u16 version, u8 strips (17), u8 route[17], u8 ret_route[2], u8 out_mode, u8 cue_level, u8 in_level                                           (25 bytes)
//   PERF  u16 version, u8 slots (12), then per slot u8 fx, u8 row, i16 value[3]                                                                         (99 bytes)
//   APPM  u16 version, u8 count (1..6; 1.1: 1..12), then per app: str name, u8 abi_major, u8 abi_minor, u8 params (1..32), f32 value[params]  (3 .. 981; 1.1: 3 .. 1015 bytes)
//
// OS-PERSIST2 adds four more parts, the session state (docs/formats/o3-project.md section 3, "The session state"), read TOLERANTLY (a value out of range is its default and is counted: the
// model's sanitise_*), not strictly like these five; the shape of a chunk is still the only error:
//
//   NOTE  u16 version, u8 root, u8 scale, u8 octave, u8 lock, u8 jump                                                                                   (7 bytes)
//   MUTE  u16 version, u8 count (1..16), u8 mode[count]                                                                                                 (3 .. 19 bytes)
//   SCEN  u16 version, u8 latch, u8 count (3), then per scene u8 stored and 12 PERF-slot records (u8 fx, u8 row, i16 value[3])                          (295 bytes)
//   SNFX  u16 version, u8 rows (1..32), u16 value[rows]                                                                                                 (3 .. 67 bytes)
//
// OS-BUG-PASTE24 adds the tenth, the instrument volume law (docs/formats/o3-project.md, "The LAW chunk"), written only when the law is the 1.1.14 table (1); a file without it is the 1.2+ law (0):
//
//   LAW   u16 version, u8 law (0 or 1; above 1 is Malformed in every minor)                                                                             (3 bytes)
//
// decode() only validates it: the Reader keeps the byte and applies it with Project::set_volume_law at Done.
//
// PERF reads an FX id above the table as none and a row above 3 as off (a later version may extend the table; INST's engine does the same); every other value out of range is an error.
// The table of version 1.0 ends at id 19; PERF and SCEN 1.1 (OS-PERFFX P1, native.h kStateVersionFx) add 20 and 21 and are written only when a slot holds one of them: in a 1.0 chunk those two read as none.
// Each is written only when the project has the part (Project::state_present(); APPM when an app is remembered) and read with the strict rules of every known chunk: a value outside
// its range, a count the layout does not fit, or unused bytes in a chunk of this minor are Malformed; a newer minor only appends. A file without them reads as the defaults.
#include "native_codec.h"

#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::state_chunks {

enum class Part : std::uint8_t { Midi, Usb, Mix, Perf, Apps, Note, Mute, Scene, SendFx, Law };
inline constexpr unsigned kParts = 10;  // the file order of the chunks

inline constexpr std::uint16_t kVersion = 0x0100;
inline constexpr std::uint8_t kPerfFxMaxV10 = 19;  // the last Perform FX id of PERF / SCEN 1.0 (seq::PerfFx::PatternLength); 1.1 goes to model::kPerfFxMax

// Payload bounds of version 1.0: the shortest chunk of any minor, and the longest of this one (a newer minor may be longer: only this much is read).
inline constexpr std::size_t kMidiBytes = 2 + 4 + model::kMidiCcSlots + 4 * model::kMidiPorts;  // 20
inline constexpr std::size_t kUsbFixed = 5;
inline constexpr std::size_t kUsbMax = kUsbFixed + 2 * model::kStemSlots;                         // 27: the longest USBS 1.0
// OS-USBOUTS: USBS 1.1 appends one byte after the masks, the real channel count (4 or 6); the 1.0 part then says P8 with 8 slots (real slots first, then the P8 factory masks), which a 1.0 reader takes as P8.
inline constexpr std::uint16_t kUsbVersionCount = 0x0101;
inline constexpr std::size_t kUsbCountP8Bytes = kUsbFixed + 2 * 8 + 1;                            // 22: the 1.1 chunk
inline constexpr std::size_t kMixBytes = 2 + 1 + model::kMixStrips + 2 + 3;                       // 25
inline constexpr std::size_t kPerfBytes = 3 + model::kPerfSlots * (2 + 2 * model::kPerfStoredRows);  // 99
inline constexpr std::size_t kAppsFixed = 3;
inline constexpr std::size_t kAppEntryFixed = 4;  // the name's length byte, the ABI, the count of values
inline constexpr std::size_t kAppsMax10 = kAppsFixed + model::kAppParamSetsV10 * (kAppEntryFixed + model::kAppNameMax + 4 * model::kMaxAppParams);  // 981: the longest APPM 1.0
inline constexpr std::size_t kAppsMax = model::kAppParamsBytesMax;                                                                                   // 1015: the longest APPM 1.1 (OS-APPUX S3: up to 12 apps, as many as fit one writer piece)
inline constexpr std::uint16_t kAppsVersionV11 = 0x0101;                                                                                             // APPM 1.1: the same layout, more than 6 apps

inline constexpr std::size_t kNoteBytes = 2 + 5;                                                  // 7
inline constexpr std::size_t kMuteFixed = 3;
inline constexpr std::size_t kMuteMax = kMuteFixed + model::kMaxTracks;                           // 19
inline constexpr std::size_t kSceneRecord = 1 + model::kPerfSlots * (2 + 2 * model::kPerfStoredRows);  // 97: stored, 12 slots
inline constexpr std::size_t kSceneBytes = 4 + model::kSceneCount * kSceneRecord;                 // 295
inline constexpr std::size_t kSendFxFixed = 3;
inline constexpr std::size_t kSendFxMax = kSendFxFixed + 2 * model::kSendFxCountMax;              // 67
inline constexpr std::size_t kLawBytes = 3;                                                       // u16 version, u8 law

static_assert(kMidiBytes == 20 && kMixBytes == 25 && kPerfBytes == 99 && kAppsMax10 == 981 && kAppsMax == 1015, "the chunk sizes of docs/formats/o3-project.md");
static_assert(kLawBytes == 3, "the chunk size of docs/formats/o3-project.md (OS-BUG-PASTE24)");
static_assert(kNoteBytes == 7 && kMuteMax == 19 && kSceneBytes == 295 && kSendFxMax == 67, "the chunk sizes of docs/formats/o3-project.md (OS-PERSIST2)");
static_assert(native::codec::kChunkHeader + kAppsMax + 1 <= native::kScratchBytes, "every state chunk is one writer piece and is read whole");
static_assert(kAppsMax10 <= kAppsMax && model::kMaxAppParamSets > model::kAppParamSetsV10, "APPM 1.1 is the longer one");

constexpr std::uint32_t chunk_id(Part p) noexcept {
  return p == Part::Midi   ? native::kChunkMidi
         : p == Part::Usb  ? native::kChunkUsbs
         : p == Part::Mix  ? native::kChunkMixr
         : p == Part::Perf ? native::kChunkPerf
         : p == Part::Apps ? native::kChunkAppm
         : p == Part::Note ? native::kChunkNote
         : p == Part::Mute ? native::kChunkMute
         : p == Part::Scene ? native::kChunkScen
         : p == Part::SendFx ? native::kChunkSnfx
                             : native::kChunkLaw;
}

// Whether the writer writes the part, and its payload size (0 when it is not written).
bool present(const model::Project& p, Part part) noexcept;
std::size_t payload_size(const model::Project& p, Part part) noexcept;
// The version the writer puts at the start of the payload: kVersion (1.0), except PERF and SCEN, which are native::kStateVersionFx (1.1) when a slot holds the Perform FX 20 or 21, and APPM, which is kAppsVersionV11 (1.1) when it holds more than 6 apps.
std::uint16_t version_of(const model::Project& p, Part part) noexcept;
// The whole payload, version included.
void encode(native::codec::Enc& e, const model::Project& p, Part part) noexcept;
// Reads a payload (the cursor is at its start, the version is skipped: the caller checked it) into `p`. nullptr when taken, else why not (Malformed). `newer_minor`: only appends follow.
// `reset` is added the number of values a tolerant part (NOTE, MUTE, SCEN, SNFX) replaced by a default or skipped; the strict parts leave it alone.
const char* decode(native::codec::Cursor& c, bool newer_minor, model::Project& p, Part part, std::uint32_t& reset) noexcept;

}  // namespace o3os::formats::state_chunks
