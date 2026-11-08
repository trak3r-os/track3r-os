// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The step FX as the sequencer sees them (OS-005c): which of the 256 types does what, in which group, with which range, and the little pure
// functions of the FX values (roll, micro-move, stutter). One table, so that what is executable is decided in one place (PAR-003: "FX bytes are
// untrusted": a pattern may hold types this firmware cannot run, and they are carried verbatim and sanitised only where they are used).
//
// TYPES. 0..42 are the ids of the OG pattern file (ADR 0004: ids and ranges are part of the pattern file), 43..45 the core FX of ADR 0004
// amendment 1 (Speed, Tempo fine, Order jump), 46..62 those of amendment 2 (OS-COREFX: the tracker FX Arpeggio, Volume slide, Fine volume slide, Fine porta up and down, then Pan slide,
// Vibrato waveform, Tremolo waveform, Glissando, Retrigger, Tremor, Channel pan, then CF3's Channel volume, Channel volume slide, Global volume, Global volume slide, Panbrello; Row kind, one TrkFx event each), 63..127 are reserved and inert, 128..255 belong to the instrument's engine (ADR 0004: the Amiga/MED engine's MED commands verbatim in 128..191)
// and pass through to the voice untouched.
//
// KINDS. What the sequencer does with a type:
//   Inert     nothing. A placeholder that no slice has filled yet (none left among the 43 ids of the OG file format: Overdrive 38 and Bit depth 39 became Voice FX with OS-IFX1), an undefined
//             type, the reserved core range. The step keeps it; it is never sent anywhere.
//   Row       consumed by the sequencer at the start of the row: timing and flow (Micro-move, Roll, Chance, Break, Tempo, Swing, Gate, Speed, Tempo fine,
//             Order jump), randomness (Random note / instrument / value), the note itself (Off) and, since OS-PFX, what the note is made of: Chord 14 and
//             Arp 21 (more notes, at once or one after another) and Tune 40 (the note shifted), all three read when a note STARTS (docs/specs/OS-PFX.md); and, since OS-COREFX,
//             the tracker FX 46..62, which act on the sounding voice tick by tick for the row they stand in (a TrkFx event; audio/tracker_fx.h) and, unlike a Voice FX, last that row only.
//   RowVoice  the velocity pair (Velocity 18, Random velocity 7): on a step with a note it sets the velocity of that note (a pre-effect of the row);
//             on a step without one it is a persistent volume override of the track's voice (it is forwarded
//             to the engine, as the voice's volume layer), so it goes the Voice way.
//   Midi      a MIDI message at the row: CC A..E (8..12) and slot 37 (CC F by the strategy-B decision; a Program Change is
//             one constant away: kSlot37IsProgramChange).
//   Voice     forwarded to the track's voice as a persistent modulation or override (ModSet / ModEnd events through the control table, seq/control.h): the
//             the list of sequencer FX: filters, glide, micro-tune, panning, sends, reverse, position, slice, the five LFOs,
//             Slide up / down (PAR-079: 1/16 semitone units from the step note, [MAN] 7.36-7.37) and, since OS-IFX1, Overdrive 38 and Bit depth 39 (PAR-018, PAR-051).
//
// GROUPS. Two FX of one group compete for one voice parameter (a low-pass and a high-pass both set the filter), so they go through the control table's
// arbitration; FX of different groups do not interact. An engine FX (128..255) has a group of its own per FX column, so that the two columns of a
// step can carry two different engine commands (two MED commands in one cell) without one hiding the other.

#include <cstdint>

namespace o3os::seq {

namespace fx {
inline constexpr std::uint8_t kNone = 0;
inline constexpr std::uint8_t kOff = 1;
inline constexpr std::uint8_t kMicroMove = 2;
inline constexpr std::uint8_t kRoll = 3;
inline constexpr std::uint8_t kChance = 4;
inline constexpr std::uint8_t kRandomNote = 5;
inline constexpr std::uint8_t kRandomInstrument = 6;
inline constexpr std::uint8_t kRandomVelocity = 7;
inline constexpr std::uint8_t kCcA = 8;
inline constexpr std::uint8_t kCcE = 12;
inline constexpr std::uint8_t kBreakPattern = 13;
inline constexpr std::uint8_t kChord = 14;
inline constexpr std::uint8_t kTempo = 15;
inline constexpr std::uint8_t kRandomValue = 16;
inline constexpr std::uint8_t kSwing = 17;
inline constexpr std::uint8_t kVelocity = 18;
inline constexpr std::uint8_t kGlide = 19;
inline constexpr std::uint8_t kGate = 20;
inline constexpr std::uint8_t kArp = 21;
inline constexpr std::uint8_t kPosition = 22;
inline constexpr std::uint8_t kVolumeLfo = 23;
inline constexpr std::uint8_t kPanningLfo = 24;
inline constexpr std::uint8_t kSlice = 25;
inline constexpr std::uint8_t kReverse = 26;
inline constexpr std::uint8_t kLowPass = 27;
inline constexpr std::uint8_t kHighPass = 28;
inline constexpr std::uint8_t kBandPass = 29;
inline constexpr std::uint8_t kDelaySend = 30;
inline constexpr std::uint8_t kPanning = 31;
inline constexpr std::uint8_t kReverbSend = 32;
inline constexpr std::uint8_t kFinetuneLfo = 33;
inline constexpr std::uint8_t kMicroTune = 34;
inline constexpr std::uint8_t kFilterLfo = 35;
inline constexpr std::uint8_t kPositionLfo = 36;
inline constexpr std::uint8_t kSlot37 = 37;  // CC F (strategy B) or a Program Change
inline constexpr std::uint8_t kOverdrive = 38;
inline constexpr std::uint8_t kBitDepth = 39;
inline constexpr std::uint8_t kTune = 40;
inline constexpr std::uint8_t kSlideUp = 41;
inline constexpr std::uint8_t kSlideDown = 42;
inline constexpr std::uint8_t kSpeed = 43;
inline constexpr std::uint8_t kTempoFine = 44;
inline constexpr std::uint8_t kOrderJump = 45;
inline constexpr std::uint8_t kTrkArp = 46;           // OS-COREFX (ADR 0004 amendment 2): MOD 0xy, S3M Jxy, XM / IT 0xy
inline constexpr std::uint8_t kTrkVolSlide = 47;      // MOD Axy, S3M Dxy
inline constexpr std::uint8_t kTrkFineVol = 48;       // MOD EAx EBx, S3M DFy DxF
inline constexpr std::uint8_t kTrkFinePortaUp = 49;   // MOD E1x, S3M FFx FEx
inline constexpr std::uint8_t kTrkFinePortaDown = 50; // MOD E2x, S3M EFx EEx
inline constexpr std::uint8_t kTrkPanSlide = 51;      // OS-COREFX CF2: S3M Pxy (IT / XM Pxy)
inline constexpr std::uint8_t kTrkVibWave = 52;       // MOD E4x, S3M S3x
inline constexpr std::uint8_t kTrkTremWave = 53;      // MOD E7x, S3M S4x
inline constexpr std::uint8_t kTrkGliss = 54;         // MOD E3x, S3M S1x
inline constexpr std::uint8_t kTrkRetrig = 55;        // S3M Qxy (XM Rxy)
inline constexpr std::uint8_t kTrkTremor = 56;        // S3M Ixy (XM Txy)
inline constexpr std::uint8_t kTrkChanPan = 57;       // OS-COREFX: Channel pan (0..100), MOD 8xx E8x, S3M Xxx S8x: the track's pan until the next one or the transport start (the Panning FX 31 ends with its step)
inline constexpr std::uint8_t kOgLast = 42;
// CF3 (ADR 0004 amendment 2): Channel volume 0..64 and Global volume 0..64 are held like Channel pan (the track's / every track's level scale until changed or the transport starts); the slides and Panbrello last their row.
inline constexpr std::uint8_t kTrkChanVol = 58;       // S3M Mxx (IT Mxx)
inline constexpr std::uint8_t kTrkChanVolSlide = 59;  // S3M Nxy
inline constexpr std::uint8_t kTrkGlobalVol = 60;     // S3M Vxx (XM Gxx)
inline constexpr std::uint8_t kTrkGlobalVolSlide = 61;  // S3M Wxy (XM Hxy)
inline constexpr std::uint8_t kTrkPanbrello = 62;     // S3M Yxy (IT Yxy)
inline constexpr std::uint8_t kCoreLast = 62;
inline constexpr std::uint8_t kEngineFirst = 128;
// OS-CEWTENV W6a: the tracker-ce step FX w (43) and y (44) are stored as the model ids 96 and 97 (ADR 0004 amendment 3). They are voice FX 0..255 of their own
// groups (the two nibbles x, y: docs/specs/OS-CEWTENV.md F9, F10, A7); the table below stays dense 0..62, so the lookups branch on these two (W6b: and on 98, z).
inline constexpr std::uint8_t kAmpEnvFx = 96;      // w: the amp envelope slot's attack and decay or the amp WT slot's duration scale and warp offset
inline constexpr std::uint8_t kFilterEnvFx = 97;   // y: the cutoff envelope slot's attack and decay
inline constexpr std::uint8_t kGranLenFx = 98;     // z (W6b): the grain length of a granular instrument, 0..100 (F11); fx[0] wins (the group resolves column 0 first)

// OS-CECHANCE C1: the Chance conditions beyond 0..100 (docs/specs/OS-CECHANCE.md). Verified parity (tracker-ce@7fd6c2c:core/source/mtSequencer.h:260-269, evaluation
// mtSequencer.cpp:2185-2222): 0..100 is a probability; 101 Group with Last (plays when the last condition of the track fired), 102 Not Last (when it did not), 103..138 Play N / Skip M
// and 139..174 Skip N / Play M (N, M 1..6, counted in passes of the pattern).
inline constexpr std::uint8_t kChanceProbMax = 100;
inline constexpr std::uint8_t kChanceMax = 174;
inline constexpr std::uint8_t kChanceWithLast = 101;
inline constexpr std::uint8_t kChanceNotLast = 102;
inline constexpr std::uint8_t kChanceCycleFirst = 103;  // 103..138 Play N / Skip M, 139..174 Skip N / Play M
// A cycle value v (103..174) decodes to its N, M: i = v - 103 (or v - 139), N = 1 + i / 6, M = 1 + i % 6.
constexpr void chance_cycle(std::uint8_t v, std::uint32_t& n, std::uint32_t& m) noexcept {
  const std::uint32_t i = v >= 139u ? static_cast<std::uint32_t>(v) - 139u : static_cast<std::uint32_t>(v) - 103u;
  n = 1u + i / 6u;
  m = 1u + i % 6u;
}
}  // namespace fx

// Open item (spec 005c, "FX 37"): the triage fixes slot 37 as CC F; what the official firmware sends is an open research question. One constant.
inline constexpr bool kSlot37IsProgramChange = false;

enum class FxKind : std::uint8_t { Inert, Row, RowVoice, Midi, Voice };

enum class FxGroup : std::uint8_t {
  None = 0,
  Volume,        // Velocity 18, Random velocity 7
  Panning,       // 31
  Finetune,      // Micro-tune 34
  Glide,         // 19
  Filter,        // Low-pass 27, High-pass 28, Band-pass 29
  DelaySend,     // 30
  ReverbSend,    // 32
  Reverse,       // 26
  Position,      // 22
  Slice,         // 25
  LfoVolume,     // 23
  LfoPanning,    // 24
  LfoFinetune,   // 33
  LfoFilter,     // 35
  LfoPosition,   // 36
  Slide,         // Slide up 41, Slide down 42
  Engine0,       // an engine FX in the first FX column
  Engine1,       // an engine FX in the second
  Overdrive,     // 38 (OS-IFX1: appended, so that every group number above keeps its value)
  BitDepth,      // 39
  Tune,          // no step FX: the performance layer's TUNE alone (OS-PERFFX P5, appended like the two above)
  SampleEnd,     // no step FX: the performance layer's SMP END alone (OS-PERFFX P7; SMP POS uses Position, whose step FX 22 has no consumer)
  AmpEnvFx,      // 96 (tracker-ce w, OS-CEWTENV W6a); mirrored as audio::kModGroupAmpEnv = 23
  FilterEnvFx,   // 97 (tracker-ce y); audio::kModGroupFilterEnv = 24
  GranLenFx,     // 98 (tracker-ce z, W6b); audio::kModGroupGranLen = 25
  kCount
};
inline constexpr std::uint32_t kFxGroupCount = static_cast<std::uint32_t>(FxGroup::kCount);
static_assert(kFxGroupCount <= 32u, "the control table keeps one bit per group in a word");

struct FxInfo {
  FxKind kind;
  FxGroup group;
  std::uint8_t min;
  std::uint8_t max;
};

// Types 0..62. The ranges of the OG file format's types are the rows (model/fx_table.h kRows); the types 17 Swing 25..75, 20 Gate 0..100 and 41/42 Slide 0..255 were
// reserved before their rows were flipped and keep their ranges; the core FX the ranges of med.md section
// 4 (Speed 1..32, Tempo fine any byte, Order jump 0..254); the tracker FX of OS-COREFX take any byte (the nibbles x y, or a count of quarter period units).
inline constexpr std::uint32_t kFxInfoCount = 63;
inline constexpr FxInfo kFxInfo[kFxInfoCount] = {
    {FxKind::Inert, FxGroup::None, 0, 0},                // 0 None
    {FxKind::Row, FxGroup::None, 0, 0},                  // 1 Off
    {FxKind::Row, FxGroup::None, 0, 100},                // 2 Micro-move
    {FxKind::Row, FxGroup::None, 0, 53},                 // 3 Roll
    {FxKind::Row, FxGroup::None, 0, 174},                // 4 Chance (OS-CECHANCE C1: 0..100 a probability, 101..174 the conditions of ADR 0004 amendment 4)
    {FxKind::Row, FxGroup::None, 0, 100},                // 5 Random note
    {FxKind::Row, FxGroup::None, 0, 47},                 // 6 Random instrument
    {FxKind::RowVoice, FxGroup::Volume, 0, 100},         // 7 Random velocity
    {FxKind::Midi, FxGroup::None, 0, 127},               // 8 CC A
    {FxKind::Midi, FxGroup::None, 0, 127},               // 9 CC B
    {FxKind::Midi, FxGroup::None, 0, 127},               // 10 CC C
    {FxKind::Midi, FxGroup::None, 0, 127},               // 11 CC D
    {FxKind::Midi, FxGroup::None, 0, 127},               // 12 CC E
    {FxKind::Row, FxGroup::None, 1, 1},                  // 13 Break pattern
    {FxKind::Row, FxGroup::None, 0, 29},                  // 14 Chord (PAR-038; 30 masks, docs/specs/OS-PFX.md)
    {FxKind::Row, FxGroup::None, 5, 200},                // 15 Tempo
    {FxKind::Row, FxGroup::None, 0, 255},                // 16 Random FX value
    {FxKind::Row, FxGroup::None, 25, 75},                // 17 Swing (PAR-081)
    {FxKind::RowVoice, FxGroup::Volume, 0, 100},         // 18 Velocity
    {FxKind::Voice, FxGroup::Glide, 0, 100},             // 19 Glide
    {FxKind::Row, FxGroup::None, 0, 100},                // 20 Gate length (PAR-080)
    {FxKind::Row, FxGroup::None, 0, 33},                  // 21 Arp (PAR-078)
    {FxKind::Voice, FxGroup::Position, 0, 100},          // 22 Position
    {FxKind::Voice, FxGroup::LfoVolume, 0, 23},          // 23 Volume LFO
    {FxKind::Voice, FxGroup::LfoPanning, 0, 28},         // 24 Panning LFO
    {FxKind::Voice, FxGroup::Slice, 0, 47},              // 25 Slice
    {FxKind::Voice, FxGroup::Reverse, 0, 1},             // 26 Reverse
    {FxKind::Voice, FxGroup::Filter, 0, 100},            // 27 Low-pass
    {FxKind::Voice, FxGroup::Filter, 0, 100},            // 28 High-pass
    {FxKind::Voice, FxGroup::Filter, 0, 100},            // 29 Band-pass
    {FxKind::Voice, FxGroup::DelaySend, 0, 100},         // 30 Delay send
    {FxKind::Voice, FxGroup::Panning, 0, 100},           // 31 Panning
    {FxKind::Voice, FxGroup::ReverbSend, 0, 100},        // 32 Reverb send
    {FxKind::Voice, FxGroup::LfoFinetune, 0, 28},        // 33 Finetune LFO
    {FxKind::Voice, FxGroup::Finetune, 0, 198},          // 34 Micro-tune
    {FxKind::Voice, FxGroup::LfoFilter, 0, 28},          // 35 Filter LFO
    {FxKind::Voice, FxGroup::LfoPosition, 0, 28},        // 36 Position LFO
    {FxKind::Midi, FxGroup::None, 0, 127},               // 37 CC F / Program change
    {FxKind::Voice, FxGroup::Overdrive, 0, 100},         // 38 Overdrive (PAR-018; OS-IFX1)
    {FxKind::Voice, FxGroup::BitDepth, 1, 16},           // 39 Bit depth (PAR-051; OS-IFX1)
    {FxKind::Row, FxGroup::None, 0, 48},                  // 40 Tune (PAR-046; 24 is no change)
    {FxKind::Voice, FxGroup::Slide, 0, 255},             // 41 Slide up (PAR-079)
    {FxKind::Voice, FxGroup::Slide, 0, 255},             // 42 Slide down
    {FxKind::Row, FxGroup::None, 1, 32},                 // 43 Speed
    {FxKind::Row, FxGroup::None, 0, 255},                // 44 Tempo fine
    {FxKind::Row, FxGroup::None, 0, 254},                // 45 Order jump
    {FxKind::Row, FxGroup::None, 0, 255},                // 46 Arpeggio (OS-COREFX)
    {FxKind::Row, FxGroup::None, 0, 255},                // 47 Volume slide
    {FxKind::Row, FxGroup::None, 0, 255},                // 48 Fine volume slide
    {FxKind::Row, FxGroup::None, 0, 255},                // 49 Fine porta up
    {FxKind::Row, FxGroup::None, 0, 255},                // 50 Fine porta down
    {FxKind::Row, FxGroup::None, 0, 255},                // 51 Pan slide (OS-COREFX CF2)
    {FxKind::Row, FxGroup::None, 0, 255},                // 52 Vibrato waveform
    {FxKind::Row, FxGroup::None, 0, 255},                // 53 Tremolo waveform
    {FxKind::Row, FxGroup::None, 0, 255},                // 54 Glissando
    {FxKind::Row, FxGroup::None, 0, 255},                // 55 Retrigger
    {FxKind::Row, FxGroup::None, 0, 255},                // 56 Tremor
    {FxKind::Row, FxGroup::None, 0, 100},                // 57 Channel pan (0..100 as the Panning FX)
    {FxKind::Row, FxGroup::None, 0, 64},                 // 58 Channel volume (0..64)
    {FxKind::Row, FxGroup::None, 0, 255},                // 59 Channel volume slide
    {FxKind::Row, FxGroup::None, 0, 64},                 // 60 Global volume (0..64)
    {FxKind::Row, FxGroup::None, 0, 255},                // 61 Global volume slide
    {FxKind::Row, FxGroup::None, 0, 255},                // 62 Panbrello
};

constexpr bool fx_is_engine(std::uint8_t type) noexcept { return type >= fx::kEngineFirst; }

// What a type is. Reserved (63..127) types are inert; engine types are Voice.
constexpr FxKind fx_kind(std::uint8_t type) noexcept {
  if (type < kFxInfoCount) {
    return kFxInfo[type].kind;
  }
  if (type == fx::kAmpEnvFx || type == fx::kFilterEnvFx || type == fx::kGranLenFx) {
    return FxKind::Voice;
  }
  return fx_is_engine(type) ? FxKind::Voice : FxKind::Inert;
}

// The group of a type in a column (0 or 1): engine types have one per column.
constexpr FxGroup fx_group(std::uint8_t type, std::uint32_t column) noexcept {
  if (type < kFxInfoCount) {
    return kFxInfo[type].group;
  }
  if (type == fx::kAmpEnvFx) {
    return FxGroup::AmpEnvFx;
  }
  if (type == fx::kFilterEnvFx) {
    return FxGroup::FilterEnvFx;
  }
  if (type == fx::kGranLenFx) {
    return FxGroup::GranLenFx;
  }
  return fx_is_engine(type) ? (column == 0u ? FxGroup::Engine0 : FxGroup::Engine1) : FxGroup::None;
}

// The point-of-use sanitiser (PAR-003 sanitize): an inert or unknown type becomes (0, 0) for this use only, an executable one has its value
// clamped to its range; type 0 always carries value 0; an engine FX is left as it is (its range is the engine's).
constexpr void fx_sanitize(std::uint8_t& type, std::uint8_t& value) noexcept {
  if (type == fx::kNone || fx_kind(type) == FxKind::Inert) {
    type = fx::kNone;
    value = 0;
    return;
  }
  if (type < kFxInfoCount) {
    if (value < kFxInfo[type].min) {
      value = kFxInfo[type].min;
    } else if (value > kFxInfo[type].max) {
      value = kFxInfo[type].max;
    }
  }
}

// ---- the FX values ---------------------------------------------------------------------------------------------------

// Roll, 0..53 = 9 x direction + period: the period 1..8 is 1/1, 1/2, 1/3,
// 1/4, 1/6, 1/8, 1/12, 1/16 of a step, i.e. a repeat every 48, 24, 16, 12, 8, 6, 4, 3 microsteps; period 0 is "none".
enum class RollDir : std::uint8_t { Const = 0, VolDown = 1, VolUp = 2, NoteDown = 3, NoteUp = 4, NoteRandom = 5 };
constexpr std::uint8_t roll_period(std::uint8_t v) noexcept { return static_cast<std::uint8_t>(v % 9u); }
constexpr RollDir roll_dir(std::uint8_t v) noexcept { return static_cast<RollDir>(v / 9u > 5u ? 5u : v / 9u); }
constexpr std::uint8_t roll_microsteps(std::uint8_t period) noexcept {
  constexpr std::uint8_t kTable[9] = {0, 48, 24, 16, 12, 8, 6, 4, 3};
  return period < 9u ? kTable[period] : std::uint8_t{0};
}

// Micro-move, 0..100: the microstep of the row at which the note starts, 1 (the row's first) to 47. The value maps by
// start = (v + 1) * 48 / 101 + 1 (integer, truncating), held to 1..47. A value of 0 is a delay of none; 100 is 46
// microsteps (of 48: the last two are never reached).
constexpr std::uint8_t micromove_start(std::uint8_t value) noexcept {
  const std::uint32_t v = value > 100u ? 100u : value;
  const std::uint32_t start = (v + 1u) * 48u / 101u + 1u;
  return static_cast<std::uint8_t>(start > 47u ? 47u : start);
}

// Performance stutter periods in microsteps, by value 0..15: none, 16, 12, 8, 6, 4, 3, 2, 1 steps, 1/2, 1/3, 1/4, 1/6, 1/8,
// 1/12, 1/16 of a step.
constexpr std::uint16_t stutter_microsteps(std::uint8_t value) noexcept {
  constexpr std::uint16_t kTable[16] = {0, 768, 576, 384, 288, 192, 144, 96, 48, 24, 16, 12, 8, 6, 4, 3};
  return value < 16u ? kTable[value] : std::uint16_t{0};
}

// ---- OS-PFX: Chord, Arp, Tune and Slide values (docs/specs/OS-PFX.md) -------------------------------------------------------

// Chord 14, stored 0..29 (PAR-038; the table is this OS's, tagged Q in docs/specs/OS-PFX.md: whether it matches the official firmware's chord map is an open research question; the HIL scenario platform/mk66/tools/hil/pfx_midi.json checks the MIDI out against it): the byte indexes a table of 30 semitone masks. Bit b of the entry set means "play
// the step's note + b semitones" (b = 0..14); bit 0 is set in every entry, so the root always sounds. Value 0 is no chord, 1..29 are chords of two to four tones (root first, ascending). A
// value past 29 is held at 29 by fx_sanitize where the sequencer reads it; chord_mask() itself reads it as none (mask 1).
inline constexpr std::uint32_t kChordMaxExtra = 3;  // the tones above the root: no mask below holds more
inline constexpr std::uint32_t kChordCount = 30;
inline constexpr std::uint16_t kChordMask[kChordCount] = {
    0x0001, 0x0085, 0x0105, 0x0049, 0x0089, 0x0091, 0x0111, 0x00A1, 0x0421, 0x1021,  //  0 -- 1 27 2 28 3 36 4 37 5 47 6 48 7 57 8 5A 9 5C
    0x00C1, 0x1081, 0x4081, 0x0285, 0x0485, 0x0885, 0x0249, 0x0449, 0x0189, 0x0289,  // 10 67 11 7C 12 7E 13 279 14 27A 15 27B 16 369 17 36A 18 378 19 379
    0x0489, 0x0889, 0x0291, 0x0491, 0x0891, 0x0311, 0x0511, 0x0911, 0x04A1, 0x08A1,  // 20 37A 21 37B 22 479 23 47A 24 47B 25 489 26 48A 27 48B 28 57A 29 57B
};
constexpr std::uint16_t chord_mask(std::uint8_t value) noexcept { return value < kChordCount ? kChordMask[value] : std::uint16_t{1}; }

// Arp 21, 0..33 (PAR-078; this OS's map, Q like the chord table): 0 is off, 1..33 are three directions of eleven rates, value - 1 = 11 x direction + rate;
// direction 0 up "/", 1 down "\", 2 random "R". The rate is the period between two notes in microsteps (48 to a step): 384, 288, 192, 144, 96 and 48 are 8, 6, 4, 3, 2 and 1 steps, 24, 16, 12, 8 and 6
// are 1/2, 1/3, 1/4, 1/6 and 1/8 of a step (the Roll grid, 48 .. 3, is the one beside it).
// The arp plays the chord of the Chord FX in the other column, one tone at a time, over the offsets 0..11 only (a rule of this OS, Q against the official firmware: chord 9 "5C" arps as 0 5, chords 11 and 12 as 0 7);
// a Chord FX of value 0 gives the chromatic arpeggio over all twelve. Without a Chord FX in the other column the arp does nothing.
// The walk (OS-PFXFIX, docs/specs/OS-PFX.md): a position 0..11 where the next search starts. Up: first set offset going up from it (wrapping), then position = offset + 1, start 0. Down: going down,
// position = offset - 1, start 11. Random: the search starts at (position + r) mod 12 with r a random number 0..10, position = offset + 1, start 0 (not uniform over the tones; whether the official firmware does the same is Q). The note is
// min(base + offset, 127): the arp loses no tone near the top, unlike the chord (which skips the tones of 128 and up).
enum class ArpDir : std::uint8_t { Up = 0, Down = 1, Random = 2 };
inline constexpr std::uint32_t kArpRates = 11;
inline constexpr std::uint16_t kArpPeriod[kArpRates] = {384, 288, 192, 144, 96, 48, 24, 16, 12, 8, 6};
constexpr std::uint8_t arp_index(std::uint8_t value) noexcept { return static_cast<std::uint8_t>((value > 33u ? 33u : (value == 0u ? 1u : value)) - 1u); }  // value 1..33 -> 0..32 (0 reads as 1)
constexpr ArpDir arp_dir(std::uint8_t value) noexcept { return static_cast<ArpDir>(arp_index(value) / kArpRates); }
constexpr std::uint16_t arp_microsteps(std::uint8_t value) noexcept { return kArpPeriod[arp_index(value) % kArpRates]; }
// The offsets the arp visits for the Chord value in the other column: the chord's own below 12; none or past the table all twelve.
constexpr std::uint16_t arp_mask(std::uint8_t chord_value) noexcept { return chord_value == 0u || chord_value >= kChordCount ? std::uint16_t{0x0FFF} : static_cast<std::uint16_t>(kChordMask[chord_value] & 0x0FFFu); }

// Tune 40, stored 0..48 = -24..+24 semitones (PAR-046): 24 is no change.
constexpr int tune_semitones(std::uint8_t value) noexcept { return static_cast<int>(value > 48u ? 48u : value) - 24; }

// Slide up 41 / down 42, 0..255 sixteenths of a semitone from the step note (PAR-079: [MAN] 7.36-7.37, 16 is one semitone); the sign is the type's.
constexpr int slide_sixteenths(std::uint8_t type, std::uint8_t value) noexcept { return type == fx::kSlideDown ? -static_cast<int>(value) : static_cast<int>(value); }

// Velocity, the FX values 0..100 as a MIDI velocity 0..127 (velocity * 127 / 100, integer, truncating).
constexpr std::uint8_t velocity_from_fx(std::uint8_t value) noexcept {
  return static_cast<std::uint8_t>((value > 100u ? 100u : value) * 127u / 100u);
}

}  // namespace o3os::seq
