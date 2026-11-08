// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal to formats/og: little-endian field access, the file layouts (PAR-006, 007, 005-004, 008-009) as named
// offsets, and the step mapping between the OG's six bytes and the model's seven. docs/formats/og.md is the
// description; tests/support/og_fixture.h writes the same layouts a second time, from the specs, for the tests.

#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace o3os::formats::og::codec {

// ---- little-endian access ---------------------------------------------------------------------------------

inline std::uint16_t get16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t get32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline float get_f32(const std::uint8_t* p) noexcept {
  const std::uint32_t bits = get32(p);
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}
inline void put16(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}
inline void put_f32(std::uint8_t* p, float f) noexcept {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  put32(p, bits);
}

// ---- headers ----------------------------------------------------------------------------------------------

// Every OG file starts with 16 bytes: magic (2), type u16, firmware version (4), structure version (4, all equal),
// size u16, two junk bytes. Versions written by Export (official 1.9.0, fixtures): project fw 1.9.0.255, pattern and
// instrument fw 1.9.0.1.
inline constexpr std::uint32_t kHeaderLen = 16;
inline constexpr std::uint32_t kVersionAt = 8;
inline constexpr std::uint16_t kTypeProject = 0, kTypeInstrument = 1, kTypePattern = 2;

inline void stamp_header(std::uint8_t* h, char m0, char m1, std::uint16_t type, const std::uint8_t fw[4],
                         std::uint8_t version, std::uint16_t size) noexcept {
  std::memset(h, 0, kHeaderLen);
  h[0] = static_cast<std::uint8_t>(m0);
  h[1] = static_cast<std::uint8_t>(m1);
  put16(h + 2, type);
  std::memcpy(h + 4, fw, 4);
  h[8] = h[9] = h[10] = h[11] = version;
  put16(h + 12, size);
}

// ---- project.mt (file offsets) ----------------------------------------------------------------------------

namespace proj {
inline constexpr std::uint32_t kPlaylist = 0x10, kPlaylistLen = 255, kPlaylistPos = 0x10F;
inline constexpr std::uint32_t kActualPattern = 0x114, kPadBoard = 0x115, kOldVolume = 0x119;
inline constexpr std::uint32_t kDelayFeedback = 0x11A, kDelayTime = 0x11C, kDelayParams = 0x11E;
inline constexpr std::uint32_t kLimiterAttack = 0x120, kLimiterRelease = 0x124, kLimiterThreshold = 0x128;
inline constexpr std::uint32_t kBitDepth = 0x12A, kEditStep = 0x12B, kTrackMute = 0x12C;
inline constexpr std::uint32_t kRadioFreq = 0x13C;
inline constexpr std::uint32_t kPerfTracksPatterns = 0x142, kPerfFxPlaces = 0x152;
inline constexpr std::uint32_t kTempo = 0x1C0, kPatternLength = 0x1C4;
inline constexpr std::uint32_t kMidiVelocity = 0x309, kBitmask = 0x319, kBitmaskLen = 255;
inline constexpr std::uint32_t kReverb = 0x418, kTrackNames = 0x428, kTrackName = 21, kTrackVolume = 0x4D0;
inline constexpr std::uint32_t kReverbVolume = 0x4D8, kDelayVolume = 0x4D9, kReverbMute = 0x4DA, kDelayMute = 0x4DB,
                               kDryMute = 0x4DC, kDryVolume = 0x4DD;
inline constexpr std::uint32_t kCommonEnd = 0x4DE;  // [0x10, 0x4DE) has one layout in every version from 6 to 16
inline constexpr std::uint32_t kExt13 = 0x4DE, kExt13Len = 0x600 - 0x4DE;  // v11+: 0x4DE..0x5FF
inline constexpr std::uint32_t kShortNames = 0x603, kShortName = 8;       // v16, v17: names of tracks 9-16
inline constexpr std::uint32_t kName = 32;
inline constexpr std::uint32_t kNameV6 = 0x4E0, kNameV11 = 0x600, kNameV16 = 0x80C, kNameV17 = 0x810;
// Lengths of the versions that have a fixed one, and where the name is.
inline constexpr std::uint32_t kLen2 = 1084, kLen3 = 1100, kLen4 = 1268, kLen5 = 1280, kLen6 = 1284, kLen11 = 1572,
                               kLen16 = 2320, kLen17 = 2324;
// Version 17 (assumed to be official 1.9.2's; no file backs it, docs/formats/og.md section 2: an open research question) inserts four bytes at the end of the values block:
// 0x80C is a new one-byte field (default 0, meaning unknown; 0x80D-0x80F padding) and the name, the second string and the CRC
// move up by four. The second string (221 characters and a NUL, then a spare byte; uninitialised memory in every file seen)
// is not carried: the model has no field for it and Export writes zeros, as for v16.
inline constexpr std::uint32_t kNewByteV17 = 0x80C;
inline constexpr std::uint32_t kSecondV16 = 0x82D, kSecondV17 = 0x831, kSecondLen = 221;
static_assert(kNameV16 + kName + 1 == kSecondV16 && kNameV17 + kName + 1 == kSecondV17, "the second string follows the name");
static_assert(kSecondV16 + kSecondLen + 2 == kLen16 - 4 && kSecondV17 + kSecondLen + 2 == kLen17 - 4,
              "NUL and one spare byte, then the CRC");
static_assert(kLen17 == kLen16 + 4 && kNameV17 == kNameV16 + 4 && kNewByteV17 == kNameV16,
              "the new byte sits where v16 has the name");
}  // namespace proj

// ---- the project settings the model has no place for: groups, defaults, the OGPM chunk ----------------------

// OGPM ("OG project settings", docs/formats/og.md and o3-project.md) carries, byte for byte as the OG file holds
// them, the groups of project.mt that are real settings the model has no field for. It is an opaque chunk to the
// core (kept and re-emitted by the native format like any unknown chunk). The groups use the DropGroup bit numbers.
// Session state (last used values, pad board, positions, edit step, save lists), the pattern bitmask and the
// uninitialised bytes are not carried: Export rebuilds or defaults them.
namespace settings {

inline constexpr std::uint32_t kStoredGroups = kDropDelay | kDropLimiter | kDropReverb | kDropInputs |
                                               kDropPerformance | kDropMidiVelocity | kDropExt13 | kDropExt19;
inline constexpr std::uint32_t kHeader = 4;      // u16 version, u16 groups (big-endian, as every native chunk)
inline constexpr std::uint16_t kVersion = 0x0100;
// OGPM 1.1 (OS-004c): group 9 is one byte longer, the byte at 0x80C of a v17 project (kNewByteV17). Group 9 is the highest
// group, so the byte is the last of the chunk; a 1.0 reader takes a newer minor's bytes past its own groups as appended and
// skips them. Written only when the byte is not zero, so a chunk of a v16 project, or of a v17 one with a zero byte, is 1.0.
inline constexpr std::uint16_t kVersionTail = 0x0101;
inline constexpr std::uint32_t kTailLen = 1;
inline constexpr unsigned kTailGroup = 9;  // the 1.7-1.9 block
inline constexpr std::uint32_t kMaxPayload = 970;

// OGPN: the PAMD file as read, trailing zeros removed. u16 version, u32 length of the file, the bytes.
inline constexpr std::uint32_t kNamesHeader = 6;

inline std::uint32_t group_len(unsigned bit) noexcept {
  switch (bit) {
    case 1: return 4;     // delay: feedback (0x11A), time u16 (0x11C), sync and ping-pong (0x11E)
    case 2: return 9;     // limiter: attack u16 (0x120), release f32 (0x124), threshold u16 (0x128), bit depth (0x12A)
    case 3: return 16;    // reverb: size, damping, pre-delay, diffusion, f32 (0x418)
    case 4: return 10;    // inputs: source, gains, monitor (0x134-0x139), radio frequency f32 (0x13C)
    case 5: return 144;   // performance: pages, FX places and values (0x142-0x1BD), selection and state (0x2F5-0x308)
    case 6: return 16;    // MIDI velocity of versions 2-7 (0x309)
    case 8: return 306;   // the 1.3-1.6 block (0x4DE-0x5FF) and the 16 bytes of 0x309 (v13 on: a relocated value)
    case 9: return 460;   // the 1.7-1.9 block (0x600-0x602 and 0x643-0x80B)
    default: return 0;
  }
}

static_assert((kStoredGroups >> (kTailGroup + 1)) == 0 && (kStoredGroups >> kTailGroup & 1u) != 0,
              "group 9 is the highest group: the OGPM 1.1 byte follows it at the end of the chunk");

// Where the bytes of a group sit in an OG project file (the payload is these pieces one after the other).
struct Piece {
  std::uint32_t at, len;
};
inline const Piece* pieces(unsigned bit, unsigned& count) noexcept {
  static const Piece kDelay[] = {{0x11A, 1}, {0x11C, 2}, {0x11E, 1}};
  static const Piece kLimiter[] = {{0x120, 2}, {0x124, 4}, {0x128, 2}, {0x12A, 1}};
  static const Piece kReverb[] = {{0x418, 16}};
  static const Piece kInputs[] = {{0x134, 6}, {0x13C, 4}};
  static const Piece kPerf[] = {{0x142, 124}, {0x2F5, 20}};
  static const Piece kVelocity[] = {{0x309, 16}};
  static const Piece kExt13[] = {{0x4DE, 290}, {0x309, 16}};
  static const Piece kExt19[] = {{0x600, 3}, {0x643, 457}};
  switch (bit) {
    case 1: count = 3; return kDelay;
    case 2: count = 4; return kLimiter;
    case 3: count = 1; return kReverb;
    case 4: count = 2; return kInputs;
    case 5: count = 2; return kPerf;
    case 6: count = 1; return kVelocity;
    case 8: count = 2; return kExt13;
    case 9: count = 2; return kExt19;
    default: count = 0; return nullptr;
  }
}

// The group as the payload holds it, read out of an OG project image. Version 11 and 12 keep the value that later
// versions keep at 0x309 inside the 1.3 block (at 0x4F7): a pair of fixtures shows that later generations move it, so this does (PAR-006 3.4).
inline void extract(unsigned bit, const std::uint8_t* image, unsigned version, std::uint8_t* out) noexcept {
  unsigned count = 0;
  const Piece* p = pieces(bit, count);
  for (unsigned i = 0; i < count; ++i) {
    std::memcpy(out, image + p[i].at, p[i].len);
    out += p[i].len;
  }
  if (bit == 8 && version < 13) {
    out -= 16;                       // the 0x309 bytes: not meaningful in versions 11 and 12
    std::uint8_t* block = out - 290;
    std::memset(out, 0, 16);
    out[0] = block[0x4F7 - 0x4DE];   // 86 in the fixtures
    block[0x4F7 - 0x4DE] = 0;
  }
}

// The OGPM 1.1 byte of an OG project image: 0x80C of a v17 file, a field that does not exist before (0x80C is the first
// character of a v16 name).
inline std::uint8_t tail_byte(const std::uint8_t* image, unsigned version) noexcept {
  return version == kProjectObservedOnly ? image[proj::kNewByteV17] : std::uint8_t{0};
}

// Writes a group payload into a version 16 image (the same bytes of a version 17 one: 0x10-0x80B did not move).
inline void apply(unsigned bit, const std::uint8_t* payload, std::uint8_t* image) noexcept {
  unsigned count = 0;
  const Piece* p = pieces(bit, count);
  for (unsigned i = 0; i < count; ++i) {
    std::memcpy(image + p[i].at, payload, p[i].len);
    payload += p[i].len;
  }
}

// The value of a group in a fresh 1.9.0 project (the fixtures and PAR-006); also what Export
// writes when the project carries no such group, and what an import does not bother to keep.
inline void defaults(unsigned bit, std::uint8_t* out) noexcept {
  const std::uint32_t n = group_len(bit);
  std::memset(out, 0, n);
  switch (bit) {
    case 1:  // feedback 50, time 500 ms, parameters 0xC3 (ping-pong, sync, rate 3)
      out[0] = 50;
      put16(out + 1, 500);
      out[3] = 0xC3;
      break;
    case 2:  // attack 100, release 0.512, threshold 30200, bit depth 16
      put16(out, 100);
      put_f32(out + 2, 0.512f);
      put16(out + 6, 30200);
      out[8] = 16;
      break;
    case 3:  // size, damping, pre-delay 0.5, diffusion 0.68
      put_f32(out, 0.5f);
      put_f32(out + 4, 0.5f);
      put_f32(out + 8, 0.5f);
      put_f32(out + 12, 0.68f);
      break;
    case 4:  // radio 87.5 MHz
      put_f32(out + 6, 87.5f);
      break;
    case 5:  // performance patterns 1 on eight tracks, FX places 1..12
      for (unsigned i = 0; i < 8; ++i) put16(out + 2 * i, 1);
      for (unsigned i = 0; i < 12; ++i) out[16 + i] = static_cast<std::uint8_t>(i + 1);
      break;
    case 6:  // the 1.1 default velocity
      std::memset(out, 127, 16);
      break;
    case 8: {  // three constants, 16 per-channel records (channel, velocity 127, CCs 1 5 7 11 64 128), delay filter 20 kHz
      out[0x4F8 - 0x4DE] = 3;
      out[0x4F9 - 0x4DE] = 1;
      out[0x4FC - 0x4DE] = 1;
      static const std::uint8_t kRecord[7] = {0x7F, 0x01, 0x05, 0x07, 0x0B, 0x40, 0x80};
      for (unsigned k = 0; k < 16; ++k) {
        std::uint8_t* r = out + (0x4FD - 0x4DE) + 16 * k;
        r[0] = static_cast<std::uint8_t>(k);
        std::memcpy(r + 1, kRecord, sizeof kRecord);
      }
      put16(out + (0x5FE - 0x4DE), 20000);
      out[290] = 86;  // the relocated value at 0x309
      break;
    }
    default: break;
  }
}

// Offset of the velocity byte of MIDI channel `ch` (0..15) inside the 1.3 block (PAR-006 3.2, inferred).
inline std::uint32_t record_velocity_at(unsigned ch) noexcept { return (0x4FD - 0x4DE) + 16 * ch + 1; }

// Reads a group payload chunk. True when valid; `at[bit]` then points at each present group's bytes inside `payload`.
struct Parsed {
  std::uint16_t groups = 0;
  const std::uint8_t* at[10] = {};
  const std::uint8_t* tail = nullptr;  // the OGPM 1.1 byte (0x80C of a v17 project), nullptr in a chunk that has none
};
inline bool parse(const std::uint8_t* payload, std::uint32_t len, Parsed& out) noexcept {
  out = Parsed{};
  if (len < kHeader) return false;
  const std::uint32_t version = static_cast<std::uint32_t>(payload[0] << 8 | payload[1]);
  const std::uint32_t groups = static_cast<std::uint32_t>(payload[2] << 8 | payload[3]);
  if ((version >> 8) != 1 || (groups & ~kStoredGroups) != 0) return false;
  std::uint32_t expect = kHeader;
  for (unsigned bit = 0; bit < 10; ++bit) {
    if ((groups >> bit & 1u) != 0) {
      out.at[bit] = payload + expect;
      expect += group_len(bit);
    }
  }
  if ((version & 0xFF) >= (kVersionTail & 0xFF) && (groups >> kTailGroup & 1u) != 0) {  // 1.1 and later: group 9's extra byte
    out.tail = payload + expect;
    expect += kTailLen;
  }
  // version 1.0 has exactly this length; a newer minor only appends
  if (len < expect || ((version & 0xFF) == 0 && len != expect)) {
    out = Parsed{};
    return false;
  }
  out.groups = static_cast<std::uint16_t>(groups);
  return true;
}

}  // namespace settings

// ---- patterns (file offsets) ------------------------------------------------------------------------------

namespace patt {
inline constexpr std::uint32_t kTempoReserve = 16, kSwing = 20;  // the head's f32s, written as 100.0 and 0.0
inline constexpr std::uint32_t kStep = 6;
inline constexpr std::uint32_t kStep6 = 8;  // v6 (OS-CEPAT S1): the six v5 bytes, then volume i8, micromove i8
}  // namespace patt

// ---- instruments (file offsets) ---------------------------------------------------------------------------

namespace instr {
inline constexpr std::uint32_t kActive = 16, kSampleType = 20, kFileName = 21, kFileNameLen = 32, kAddress = 56,
                               kLength = 60, kWindowSize = 64, kWindowNumber = 68, kName4 = 72, kPlayMode = 76,
                               kStart = 78, kLoopStart = 80, kLoopEnd = 82, kEnd = 84, kCurrentWindow = 88,
                               kEnvelope = 92, kEnvelopeStride = 20, kLfo = 212, kLfoStride = 8, kCutoff = 260,
                               kResonance = 264, kFilterType = 268, kFilterEnable = 269, kTune = 270, kFineTune = 271,
                               kVolume = 272, kGlide = 274, kPanning = 276, kDelaySend = 278, kPlayThru = 279, kSlices = 280,
                               kSliceCount = 376, kSelectedSlice = 377, kGrainLength = 378, kGrainPosition = 380,
                               kGrainShape = 382, kGrainType = 383, kReverbSend = 384, kOverdrive = 385,
                               kBitDepth = 386, kReserved = 387, kCrc = 388;
inline constexpr std::uint32_t kCrcLegacy = 384;  // versions 1-3: the CRC sits where reverb send now is
}  // namespace instr

// ---- steps ------------------------------------------------------------------------------------------------

// Note byte (i8): -1 empty, -2 fade, -3 cut, -4 off, 0..127 notes; the other negatives are kept (model 129..252).
// The map is a bijection of the 256 byte values onto the 256 model values.
inline std::uint8_t note_from_og(std::uint8_t b) noexcept {
  if (b <= 251) return static_cast<std::uint8_t>(b + 1);   // 0..127 -> 1..128, 128..251 (-128..-5) -> 129..252
  if (b == 255) return model::kNoteEmpty;
  return static_cast<std::uint8_t>(507 - b);               // 254 fade -> 253, 253 cut -> 254, 252 off -> 255
}
inline std::uint8_t note_to_og(std::uint8_t m) noexcept {
  if (m <= 252) return static_cast<std::uint8_t>(m - 1);   // 0 -> 255 (empty)
  return static_cast<std::uint8_t>(507 - m);               // 253 fade -> 254, 254 cut -> 253, 255 off -> 252
}
// "Empty, fade, cut, off" carry an instrument byte in OG files but name no instrument of their own (the byte is 0
// on all but a few); a regular note always names one.
inline bool is_special_note(std::uint8_t m) noexcept {
  return m == model::kNoteEmpty || m >= model::kNoteFade;
}
// Instrument byte: OG 0..47 sample slots and 48..63 MIDI channels, model 1..64 = slot + 1, 0 = none. A special
// note with byte 0 names none; byte 255 (outside OG's 0..63) reads as none.
inline std::uint8_t instr_from_og(std::uint8_t model_note, std::uint8_t b) noexcept {
  if (b == 0 && is_special_note(model_note)) return 0;
  return static_cast<std::uint8_t>(b + 1);  // 255 + 1 wraps to 0 on purpose
}
inline std::uint8_t instr_to_og(std::uint8_t x) noexcept {
  return x == 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(x - 1);
}

// The OG id map of OS-CEFXIDS (ADR 0004 amendment 3), applied to every OG pattern version: OG 0..42 are the shared
// OG ids and stay; 43..74 are tracker-ce FX and are stored as 96..127 (id + kFxCeOffset), where they stay inert
// until a definition lands and cannot collide with the core FX 43..95; 75..127 hold nothing this OS can name and
// are cleared to (none, 0), counted in `ce_fx_cleared` (OS-CEPAT S1b, A10); 128..255 are engine FX and stay verbatim.
inline model::StepFx fx_from_og(std::uint8_t t, std::uint8_t v, std::uint32_t* ce_fx_cleared = nullptr) noexcept {
  if (t > model::kFxOgLast && t <= model::kFxOgCeLast) {
    return {static_cast<std::uint8_t>(t + model::kFxCeOffset), v};
  }
  if (t > model::kFxOgCeLast && t < model::kFxEngineFirst) {
    if (ce_fx_cleared != nullptr) ++*ce_fx_cleared;
    return {model::kFxNone, 0};
  }
  return {t, v};
}

inline model::Step step_from_og(const std::uint8_t* b, std::uint32_t* ce_fx_cleared = nullptr) noexcept {
  model::Step s;
  s.note = note_from_og(b[0]);
  s.instr = instr_from_og(s.note, b[1]);
  s.vol = model::kVolumeUnset;
  s.fx[0] = fx_from_og(b[2], b[3], ce_fx_cleared);
  s.fx[1] = fx_from_og(b[4], b[5], ce_fx_cleared);
  return s;
}

// The OG ids the v6 columns interact with (ADR 0004: the ids are part of the pattern file; the same constants as
// seq::fx::kMicroMove and seq::fx::kRandomValue, which formats/ cannot include).
inline constexpr std::uint8_t kFxMicroMove = 2;    // Micro-move
inline constexpr std::uint8_t kFxRandomValue = 16;  // Random FX value; with the value 16 it randomises the other column's value

// The v6 step (OS-CEPAT S1, the byte map of docs/specs/OS-CEPAT.md 2.1): eight bytes, bytes 0-5 as v5, then the
// volume column (byte 6, i8) and the micromove column (byte 7, i8), mapped as section 3.3 says:
//   volume < 0        -> vol unset (verified parity: -1 is none, tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:477)
//   volume 0..100     -> floor(v * 127 / 100), the MIDI velocity tracker-ce sends (verified parity,
//                        tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:2445)
//   volume 101..127   -> full volume, counted in `values_clamped` (new Community OS behaviour: tracker-ce's editor
//                        clamps to 100, so it never writes one)
//   micromove < 0     -> nothing (verified parity)
//   micromove >= 0    -> FX 2 with the same value in the first free FX column (the placement is new Community OS
//                        behaviour, Q2 (a)); superseded when a column already holds FX 2 (verified parity:
//                        tracker-ce@7fd6c2c:core/source/mtSequencer.cpp:471-474, 518-527); dropped and counted in
//                        `micromoves_dropped` when no column is free or the other one holds Random value 16 (new
//                        Community OS behaviour: our sequencer would randomise the moved value,
//                        seq/src/sequencer.cpp:1820-1834). Values 101..127 are kept as stored.
inline model::Step step_from_og_v6(const std::uint8_t* b, std::uint32_t* micromoves_dropped = nullptr,
                                   std::uint32_t* values_clamped = nullptr,
                                   std::uint32_t* ce_fx_cleared = nullptr) noexcept {
  model::Step s = step_from_og(b, ce_fx_cleared);  // bytes 0-5 as v5, FX through the id map
  const int volume = static_cast<std::int8_t>(b[6]);
  if (volume < 0) {
    s.vol = model::kVolumeUnset;
  } else if (volume <= 100) {
    s.vol = static_cast<std::uint8_t>(static_cast<std::uint32_t>(volume) * 127u / 100u);
  } else {
    s.vol = model::kVolumeMax;
    if (values_clamped != nullptr) ++*values_clamped;
  }
  const int m = static_cast<std::int8_t>(b[7]);
  if (m >= 0 && s.fx[0].type != kFxMicroMove && s.fx[1].type != kFxMicroMove) {
    const int free_slot = s.fx[0].type == model::kFxNone ? 0 : (s.fx[1].type == model::kFxNone ? 1 : -1);
    if (free_slot < 0) {
      if (micromoves_dropped != nullptr) ++*micromoves_dropped;
    } else if (s.fx[1 - free_slot].type == kFxRandomValue && s.fx[1 - free_slot].value == kFxRandomValue) {
      if (micromoves_dropped != nullptr) ++*micromoves_dropped;  // Random value 16 would randomise the moved value
    } else {
      s.fx[free_slot] = {kFxMicroMove, static_cast<std::uint8_t>(m)};
    }
  }
  return s;
}

// The OG's FX 18 (volume / velocity) value for a model volume 0..127: 127 is full, 100 in the file's range.
inline std::uint8_t volume_to_fx18(std::uint8_t vol) noexcept {
  const std::uint32_t v = vol > model::kVolumeMax ? model::kVolumeMax : vol;
  return static_cast<std::uint8_t>((v * 100u + 63u) / 127u);
}
inline constexpr std::uint8_t kFxVolume = 18;

// Writes the six OG bytes of a model step into `out`, counting what Export has to change.
inline void step_to_og(const model::Step& s, std::uint8_t* out, ExportDrops& drops) noexcept {
  out[0] = note_to_og(s.note);
  out[1] = instr_to_og(s.instr);
  std::uint8_t type[2] = {s.fx[0].type, s.fx[1].type};
  std::uint8_t value[2] = {s.fx[0].value, s.fx[1].value};
  for (int k = 0; k < 2; ++k) {
    if (type[k] >= model::kFxEngineFirst) {  // engine FX: no OG meaning
      type[k] = value[k] = 0;
      ++drops.engine_fx;
    } else if (type[k] >= model::kFxCeFirst) {  // tracker-ce FX 96..127 (ADR 0004 amendment 3): OG has none, written as none
      type[k] = value[k] = 0;
      ++drops.ce_fx;
    } else if (type[k] > model::kFxOgLast) {  // core FX 43..95 (ADR 0004 amendments 1 and 2): OG has none, written as none
      type[k] = value[k] = 0;
      ++drops.core_fx;
    }
  }
  if (s.vol != model::kVolumeUnset) {
    const int free_slot = type[0] == model::kFxNone ? 0 : (type[1] == model::kFxNone ? 1 : -1);
    if (type[0] == kFxVolume || type[1] == kFxVolume || free_slot < 0) {
      ++drops.volume_dropped;
    } else {
      type[free_slot] = kFxVolume;
      value[free_slot] = volume_to_fx18(s.vol);
      ++drops.volume_as_fx18;
    }
  }
  out[2] = type[0];
  out[3] = value[0];
  out[4] = type[1];
  out[5] = value[1];
}

// ---- text -------------------------------------------------------------------------------------------------

// Length of the text in a fixed field of `cap` bytes: up to the first NUL, at most `limit`.
inline std::size_t field_text_len(const std::uint8_t* field, std::size_t cap, std::size_t limit) noexcept {
  std::size_t n = 0;
  while (n < cap && n < limit && field[n] != 0) {
    ++n;
  }
  return n;
}

// Length of the text in a model field of `cap` + 1 bytes (a valid field is always terminated).
inline std::size_t text_len(const char* field, std::size_t cap) noexcept {
  const std::size_t n = model::field_length(field, cap);
  return n > cap ? cap : n;
}

inline bool finite_float(float f) noexcept { return model::is_finite_float(f); }

}  // namespace o3os::formats::og::codec
