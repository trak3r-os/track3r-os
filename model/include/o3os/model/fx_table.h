// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pattern-step FX table (OS-009 P1a.1; PAR-003). The type ids 0..42 are fixed by the OG project file format: the id is stored in the
// first byte of each FX column of every step (ADR 0004, docs/formats/og.md), so these ids cannot be renumbered or reused. Ids 43..127 are this
// tree's core FX (ADR 0004 amendments 1 and 2; since OS-COREFX the tracker FX 46..62 have a table of their own below), 128..255 the
// instrument engine's. The rows (what a type may hold, where it starts, whether the editor may write it) are this tree's table; the
// sequencer's own ranges (seq/fx.h kFxInfo) are held equal to them by tests/pattern_edit_rules_test.cpp.
//
// The two FX bytes of a step are carried verbatim (ADR 0004, step.h); this table says which types the UI may write (the *executable*
// ones, D9 of the spec), their ranges and starting values, and how a leftover value is sanitised where it is used. A type that is not in the
// table (43..255 outside the core range: reserved for core FX and engine-declared types) is inert: kept, never written and never edited.
//
// Reserved rows (`Support::Reserved`) keep their data and stay out of the picker until the row that implements them lands; flipping
// one is one row of kRows and one entry of kPickerOrder (Swing 17 and Gate Length 20 were flipped by OS-CFGWIRE, the first two: the sequencer
// runs both, seq/sequencer.cpp kSwing / kGate; Chord 14, Arp 21, Tune 40 and Slide up / down 41, 42 by OS-PFX, which appended them after those; Overdrive 38 and Bit Depth 39 by OS-IFX1, last;
// tests/pattern_edit_rules_test.cpp holds the table and the sequencer's kind in step). Pure data, no heap.

#include <cstdint>

namespace o3os::model::fx {

// Executes: the editor may write the type and the sequencer runs it. Reserved: a slot a later release fills, inert here. Remapped: slot 37
// (Program Change here; the release notes document the type as MIDI CC F from 1.3.0, not verified). The numeric values are part of the table's bytes: keep them.
enum class Support : std::uint8_t { Executes, Reserved, Remapped };

// One type: whether it runs, the range a value may hold (lo..hi) and the value a freshly picked type starts at.
struct Row {
  Support support;
  std::uint8_t lo;
  std::uint8_t hi;
  std::uint8_t start;
};

constexpr Row executes(std::uint8_t lo, std::uint8_t hi, std::uint8_t start) noexcept { return Row{Support::Executes, lo, hi, start}; }
constexpr Row reserved(std::uint8_t lo, std::uint8_t hi, std::uint8_t start) noexcept { return Row{Support::Reserved, lo, hi, start}; }
constexpr Row remapped(std::uint8_t lo, std::uint8_t hi, std::uint8_t start) noexcept { return Row{Support::Remapped, lo, hi, start}; }

inline constexpr unsigned kTypeCount = 43;    // table size (ids 0-42); not the picker length
inline constexpr unsigned kPickerCount = 63;  // entries of the FX picker: the first 34, then Swing and Gate Length (OS-CFGWIRE), then Chord, Arp, Tune, Slide up, Slide down (OS-PFX), then Overdrive and Bit Depth (OS-IFX1), then the five tracker FX of CF1 and the six of CF2 and Channel pan, and the five of CF3 (OS-COREFX), then w, y and z (OS-CEWTENV W6b)
// OS-COREFX: the tracker FX, ADR 0004 amendment 2 (new Community OS behaviour; the OG ids end at 42 and 43..45 are the importer's core FX that the editor does not offer yet). Ids 46..50 in the order of
// seq/fx.h: Arpeggio, Volume slide, Fine volume slide, Fine porta up, Fine porta down. Their rows are a table of their own so that kRows holds exactly the ids of the OG file format, row for row.
inline constexpr unsigned kCoreFirst = 46;
inline constexpr unsigned kCoreCount = 17;
// Q9 of PAR-003: slot 37 is a MIDI Program Change until M4 turns it into MIDI CC F.
inline constexpr bool kType37Executes = true;

inline constexpr Row kRows[kTypeCount] = {
    executes(0, 100, 0),     //  0 None
    executes(0, 0, 0),       //  1 Off
    executes(0, 100, 0),     //  2 Micro-move
    executes(0, 53, 1),      //  3 Roll
    executes(0, 174, 0),     //  4 Chance (OS-CECHANCE C2: 0..100 a probability, 101..174 the conditions of ADR 0004 amendment 4, kChanceMax below)
    executes(0, 100, 0),     //  5 Random Note
    executes(0, 47, 0),      //  6 Random Instrument
    executes(0, 100, 0),     //  7 Random Volume
    executes(0, 127, 0),     //  8 MIDI CC A
    executes(0, 127, 0),     //  9 MIDI CC B
    executes(0, 127, 0),     // 10 MIDI CC C
    executes(0, 127, 0),     // 11 MIDI CC D
    executes(0, 127, 0),     // 12 MIDI CC E
    executes(1, 1, 1),       // 13 Break Pattern
    executes(0, 29, 0),      // 14 MIDI Chord      (PAR-038; OS-PFX: the sequencer runs it, 30 values, seq/fx.h kChordMask)
    executes(5, 200, 60),    // 15 Tempo
    executes(0, 255, 0),     // 16 Random FX Value
    executes(25, 75, 50),    // 17 Swing           (PAR-081; the sequencer runs it: OS-CFGWIRE turned the reserved row into an executable one)
    executes(0, 100, 0),     // 18 Volume/Velocity
    executes(0, 100, 0),     // 19 Glide
    executes(0, 100, 0),     // 20 Gate Length     (PAR-080; likewise: the length mapping is the sequencer's open item)
    executes(0, 33, 0),      // 21 Arp             (PAR-078; OS-PFX)
    executes(0, 100, 0),     // 22 Position
    executes(0, 23, 0),      // 23 Volume LFO
    executes(0, 28, 0),      // 24 Panning LFO
    executes(0, 47, 0),      // 25 Slice
    executes(0, 1, 0),       // 26 Reverse
    executes(0, 100, 0),     // 27 Low-pass
    executes(0, 100, 0),     // 28 High-pass
    executes(0, 100, 0),     // 29 Band-pass
    executes(0, 100, 0),     // 30 Delay Send
    executes(0, 100, 50),    // 31 Panning
    executes(0, 100, 0),     // 32 Reverb Send
    executes(0, 28, 0),      // 33 Finetune LFO
    executes(0, 198, 99),    // 34 Micro-tune
    executes(0, 28, 0),      // 35 Filter LFO
    executes(0, 28, 0),      // 36 Position LFO
    remapped(0, 127, 0),  // 37 Program Change / MIDI CC F (release notes, 1.3.0+; not verified)
    executes(0, 100, 0),     // 38 Overdrive       (PAR-018; OS-IFX1)
    executes(1, 16, 16),     // 39 Bit Depth       (PAR-051; OS-IFX1)
    executes(0, 48, 24),     // 40 Tune            (PAR-046; OS-PFX)
    executes(0, 255, 0),     // 41 Slide Up        (PAR-079; OS-PFX)
    executes(0, 255, 0),     // 42 Slide Down      (PAR-079; OS-PFX)
};

// The picker: picker index -> FX type. Executable slots only, which keeps placeholders out of every UI entry path.
// Index 0 is None.
inline constexpr std::uint8_t kPickerOrder[kPickerCount] = {
    0, 1, 18, 31, 34, 19, 15, 2, 4, 3, 5, 6, 16, 7, 26, 22, 25, 23, 24, 35, 36, 33, 27, 29, 28, 30, 32, 8, 9, 10, 11, 12, 37, 13,
    17, 20,  // Swing, Gate Length: appended (OS-CFGWIRE), so every earlier picker index keeps its place
    14, 21, 40, 41, 42,  // Chord, Arp, Tune, Slide Up, Slide Down: appended (OS-PFX), likewise
    38, 39,              // Overdrive, Bit Depth: appended (OS-IFX1)
    46, 47, 48, 49, 50,  // the tracker FX: appended (OS-COREFX CF1)
    51, 52, 53, 54, 55, 56,  // Pan slide, Vibrato waveform, Tremolo waveform, Glissando, Retrigger, Tremor: appended (OS-COREFX CF2)
    57,                      // Channel pan: appended (OS-COREFX)
    58, 59, 60, 61, 62,      // Channel volume, Channel volume slide, Global volume, Global volume slide, Panbrello: appended (OS-COREFX CF3)
    96, 97, 98,              // w (amp envelope), y (cutoff envelope), z (granular length): appended (OS-CEWTENV W6b)
};

inline constexpr Row kCoreRows[kCoreCount] = {
    executes(0, 255, 0),     // 46 Arpeggio         (xy: the semitones of the second and third tick)
    executes(0, 255, 0),     // 47 Volume slide     (xy: x up or else y down, a tick)
    executes(0, 255, 0),     // 48 Fine volume slide (xy: once)
    executes(0, 255, 0),     // 49 Fine porta up    (quarter Paula period units, once)
    executes(0, 255, 0),     // 50 Fine porta down
    executes(0, 255, 0),     // 51 Pan slide        (xy: y right else x left, 4/256 of the range a tick; Fy / xF fine, once)
    executes(0, 255, 0),     // 52 Vibrato waveform (0..7: the tracker's E4x / S3x)
    executes(0, 255, 0),     // 53 Tremolo waveform (0..7)
    executes(0, 255, 0),     // 54 Glissando        (0 off, else on)
    executes(0, 255, 0),     // 55 Retrigger        (xy: x the volume change, y the interval in ticks)
    executes(0, 255, 0),     // 56 Tremor           (xy: x + 1 ticks on, y + 1 off)
    executes(0, 100, 50),    // 57 Channel pan      (0..100, 50 the centre: the track's pan until the next one or the transport start)
    executes(0, 64, 64),     // 58 Channel volume   (0..64, 64 full: the track's level scale until the next one or the transport start)
    executes(0, 255, 0),     // 59 Channel volume slide (xy: y down else x up a tick, Fy / xF fine, once)
    executes(0, 64, 64),     // 60 Global volume    (0..64, 64 full: every track's level scale until the next one or the transport start)
    executes(0, 255, 0),     // 61 Global volume slide (xy: x up else y down a tick, Fy / xF fine, once)
    executes(0, 255, 0),     // 62 Panbrello        (xy: x speed, y depth; a zero nibble keeps the track's last)
};

// OS-CEWTENV W6a: the tracker-ce step FX w (43) and y (44), stored as the ids 96 and 97 (ADR 0004 amendment 3; seq/fx.h kAmpEnvFx, kFilterEnvFx). Executable, two nibbles x y
// (0..255), start 0; in the picker since W6b. OS-CEWTENV W6b adds z (98, the grain length, 0..100). A table of their own like the core rows.
inline constexpr unsigned kCeFirst = 96;
inline constexpr unsigned kCeCount = 3;
inline constexpr Row kCeRows[kCeCount] = {
    executes(0, 255, 0),     // 96 w: amp envelope attack x and decay y, or the amp WT slot's duration scale x and warp offset y
    executes(0, 255, 0),     // 97 y: cutoff envelope attack x and decay y
    executes(0, 100, 50),    // 98 z: the granular grain length (F11: 0..50 is 44..4,410 samples, 50..100 is 4,410..44,100; 50 is 100 ms at 44.1 kHz)
};

// Random FX fill (P1c) excludes these two deliberately.
inline constexpr std::uint8_t kTempoType = 15;
inline constexpr std::uint8_t kBreakPatternType = 13;

// OS-CECHANCE C2: Chance (type 4) holds 0..100 a probability and 101..174 the conditions (101 with last, 102 not last, 103..138 Play N / Skip M,
// 139..174 Skip N / Play M; the same constants as seq/fx.h, which the model cannot include). The pads and Fill RANDOM stay in 0..100; the encoder
// and Fill CONSTANT / FROM-TO reach 174.
inline constexpr std::uint8_t kChanceType = 4;
inline constexpr std::uint8_t kChanceProbMax = 100;
inline constexpr std::uint8_t kChanceWithLast = 101;
inline constexpr std::uint8_t kChanceNotLast = 102;
inline constexpr std::uint8_t kChanceCycleFirst = 103;
inline constexpr std::uint8_t kChanceCycleStep = 6;
inline constexpr std::uint8_t kChanceMax = 174;

constexpr bool is_core(std::uint8_t type) noexcept { return type >= kCoreFirst && type < kCoreFirst + kCoreCount; }
constexpr bool is_ce(std::uint8_t type) noexcept { return type >= kCeFirst && type < kCeFirst + kCeCount; }
constexpr bool is_known(std::uint8_t type) noexcept { return type < kTypeCount || is_core(type) || is_ce(type); }

// Type 0 (None) counts as executable: a valid, do-nothing slot.
constexpr bool is_executable(std::uint8_t type) noexcept {
  if (is_core(type) || is_ce(type)) {
    return true;
  }
  if (type >= kTypeCount) {
    return false;
  }
  const Support s = kRows[type].support;
  return s == Support::Executes || (kType37Executes && s == Support::Remapped);
}

// The table row of a type; an unknown type is row 0 (None: 0-100, default 0).
constexpr const Row& row(std::uint8_t type) noexcept {
  return is_core(type) ? kCoreRows[type - kCoreFirst] : (is_ce(type) ? kCeRows[type - kCeFirst] : kRows[type < kTypeCount ? type : 0u]);
}

// The upper bound of a random draw for a type (OS-CECHANCE C2): Chance draws in 0..100 only (a condition is entered by the encoder or a Fill
// CONSTANT / FROM-TO, never drawn), every other type in its whole range.
constexpr std::uint8_t random_hi(std::uint8_t type) noexcept {
  const Row& r = row(type);
  return type == kChanceType && r.hi > kChanceProbMax ? kChanceProbMax : r.hi;
}

// Point-of-use sanitiser: a non-executable type becomes (0, 0) for this use only; an executable type gets its value clamped to
// [min, max]; type 0 always carries value 0. Idempotent.
constexpr void sanitize(std::uint8_t& type, std::uint8_t& value) noexcept {
  if (type == 0 || !is_executable(type)) {
    type = 0;
    value = 0;
    return;
  }
  const Row& r = row(type);
  value = value < r.lo ? r.lo : (value > r.hi ? r.hi : value);
}

constexpr bool is_random_eligible(std::uint8_t type) noexcept {
  return type >= 2 && type < kTypeCount && is_executable(type) && type != kTempoType && type != kBreakPatternType;  // (the tracker FX are an importer's, not a Fill's)
}

// The picker index of a type, 0 when it is not in the picker.
constexpr std::uint8_t picker_index(std::uint8_t type) noexcept {
  for (unsigned i = 0; i < kPickerCount; ++i) {
    if (kPickerOrder[i] == type) {
      return static_cast<std::uint8_t>(i);
    }
  }
  return 0;
}

// The type at a picker index; an index past the picker is 0 (None); a negative one is itself (the Fill
// dialogs' -1 "random FX", P1c).
constexpr int type_of_picker(int index) noexcept {
  if (index >= static_cast<int>(kPickerCount)) {
    return 0;
  }
  return index >= 0 ? kPickerOrder[index] : index;
}

}  // namespace o3os::model::fx
