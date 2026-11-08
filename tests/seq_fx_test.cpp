// SPDX-License-Identifier: GPL-3.0-or-later
//
// The step FX table and the FX value functions (seq/fx.h, OS-005c): the table (and the model's rows) against the value ranges the UI shows for the
// types 1..42, written out again here, the point-of-use sanitiser, Roll, Micro-move and the stutter periods against their formulas written out again here, the groups of the control
// layers. Host and Cortex-M4F (`qemu_seq`).
#include <doctest/doctest.h>

#include <o3os/model/fx_table.h>
#include <o3os/seq/fx.h>

#include <cstdint>

namespace model = o3os::model;
namespace seq = o3os::seq;

namespace {

// The value range the UI shows for each FX type 1..42 (behaviour facts, written out here independently of the tables under test). The ids are fixed by the
// project file format (docs/formats/og.md); the range is what the editor lets a person enter. Where the repository records a source: Chord 0..29, Arp 0..33,
// Tune 0..48 (24 is no change) and Slide 0..255 sixteenths of a semitone in docs/specs/OS-PFX.md; Glide 0..100, Overdrive 0..100 and Bit depth 1..16 in
// docs/specs/OS-IFX1.md; Swing 25..75 (straight at 50) in docs/specs/OS-005-audio-engine.md. The sequencer's
// table (seq/fx.h) and the model's rows (model/fx_table.h) must both agree with it.
struct UiRange {
  std::uint8_t id;
  std::uint8_t min, max;
};
constexpr UiRange kUiRanges[42] = {
    {1, 0, 0},    // Off
    {2, 0, 100},  // Micro-move
    {3, 0, 53},   // Roll
    {4, 0, 174},  // Chance (OS-CECHANCE C2: 0..100 a probability, 101..174 the conditions of ADR 0004 amendment 4)
    {5, 0, 100},  // Random note
    {6, 0, 47},   // Random instrument
    {7, 0, 100},  // Random velocity
    {8, 0, 127},  // CC A
    {9, 0, 127},  // CC B
    {10, 0, 127},  // CC C
    {11, 0, 127},  // CC D
    {12, 0, 127},  // CC E
    {13, 1, 1},   // Break pattern
    {14, 0, 29},  // Chord
    {15, 5, 200},  // Tempo
    {16, 0, 255},  // Random FX value
    {17, 25, 75},  // Swing
    {18, 0, 100},  // Velocity
    {19, 0, 100},  // Glide
    {20, 0, 100},  // Gate length
    {21, 0, 33},  // Arp
    {22, 0, 100},  // Position
    {23, 0, 23},  // Volume LFO
    {24, 0, 28},  // Panning LFO
    {25, 0, 47},  // Slice
    {26, 0, 1},   // Reverse
    {27, 0, 100},  // Low-pass
    {28, 0, 100},  // High-pass
    {29, 0, 100},  // Band-pass
    {30, 0, 100},  // Delay send
    {31, 0, 100},  // Panning
    {32, 0, 100},  // Reverb send
    {33, 0, 28},  // Finetune LFO
    {34, 0, 198},  // Micro-tune
    {35, 0, 28},  // Filter LFO
    {36, 0, 28},  // Position LFO
    {37, 0, 127},  // CC F / Program change
    {38, 0, 100},  // Overdrive
    {39, 1, 16},  // Bit depth
    {40, 0, 48},  // Tune
    {41, 0, 255},  // Slide up
    {42, 0, 255},  // Slide down
};

// The integer range rescale, written out again independently of model::edit::map_int: long arithmetic, the +1 branch when the input range is the larger one.
long rescale(long x, long in_min, long in_max, long out_min, long out_max) {
  if ((in_max - in_min) > (out_max - out_min)) {
    return (x - in_min) * (out_max - out_min + 1) / (in_max - in_min + 1) + out_min;
  }
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

}  // namespace

TEST_CASE("seq fx: the value range of every type 1..42 is the one the UI shows, in the sequencer's table and in the model's rows") {
  static_assert(sizeof(kUiRanges) / sizeof(kUiRanges[0]) == 42, "types 1..42");
  for (std::uint32_t i = 0; i < 42; ++i) {
    const UiRange& r = kUiRanges[i];
    CAPTURE(static_cast<int>(r.id));
    CHECK(r.id == i + 1);
    CHECK(seq::kFxInfo[r.id].min == r.min);
    // OS-CECHANCE C2: Chance (4) reaches 174 in both tables — the sequencer clamps a stored value to 174 at the point of use and the editor's row accepts it (ADR 0004 amendment 4).
    CHECK(seq::kFxInfo[r.id].max == r.max);
    CHECK(model::fx::kRows[r.id].lo == r.min);
    CHECK(model::fx::kRows[r.id].hi == r.max);
  }
  CHECK(seq::kFxInfo[seq::fx::kSlot37].min == 0);
  CHECK(seq::kFxInfo[seq::fx::kSlot37].max == 127);
  // What is executable: every type 1..42 (Swing 17 and Gate 20 since OS-CFGWIRE; Chord 14, Arp 21, Tune 40, Slide 41 and 42 since OS-PFX; Overdrive 38 and Bit depth 39 since OS-IFX1) and the core FX 43..62.
  for (std::uint32_t t = 1; t < 43; ++t) {
    const bool inert = false;  // since OS-IFX1 every one of the 43 types runs (Overdrive 38 and Bit depth 39 are Voice FX)
    CHECK_MESSAGE((seq::fx_kind(static_cast<std::uint8_t>(t)) == seq::FxKind::Inert) == inert, t);
  }
  CHECK(seq::fx_kind(0) == seq::FxKind::Inert);
  for (std::uint32_t t = 43; t <= seq::fx::kCoreLast; ++t) {  // 43..45 (ADR 0004 amendment 1) and the tracker FX 46..62 (amendment 2, OS-COREFX): Row FX of no group, any byte (Channel pan 0..100, Channel and Global volume 0..64)
    CHECK(seq::fx_kind(static_cast<std::uint8_t>(t)) == seq::FxKind::Row);
    CHECK(seq::fx_group(static_cast<std::uint8_t>(t), 0) == seq::FxGroup::None);
    if (t >= 46) {
      CHECK(seq::kFxInfo[t].min == 0);
      CHECK(seq::kFxInfo[t].max == (t == seq::fx::kTrkChanPan ? 100 : (t == seq::fx::kTrkChanVol || t == seq::fx::kTrkGlobalVol ? 64 : 255)));
    }
  }
  for (std::uint32_t t = seq::fx::kCoreLast + 1u; t < 128; ++t) {
    const bool ce = t == seq::fx::kAmpEnvFx || t == seq::fx::kFilterEnvFx || t == seq::fx::kGranLenFx;  // OS-CEWTENV W6a, W6b: the step FX w, y and z (96, 97, 98) act
    CHECK((seq::fx_kind(static_cast<std::uint8_t>(t)) == seq::FxKind::Inert) == !ce);  // reserved core range: preserved, inert
  }
  for (std::uint32_t t = 128; t < 256; ++t) {
    CHECK(seq::fx_kind(static_cast<std::uint8_t>(t)) == seq::FxKind::Voice);  // the engine's window
  }
  // The kinds of the groups: MIDI, row, voice.
  for (std::uint8_t t = seq::fx::kCcA; t <= seq::fx::kCcE; ++t) {
    CHECK(seq::fx_kind(t) == seq::FxKind::Midi);
  }
  CHECK(seq::fx_kind(seq::fx::kSlot37) == seq::FxKind::Midi);
  CHECK(seq::fx_kind(seq::fx::kVelocity) == seq::FxKind::RowVoice);
  CHECK(seq::fx_kind(seq::fx::kRandomVelocity) == seq::FxKind::RowVoice);
  CHECK(seq::fx_kind(seq::fx::kRoll) == seq::FxKind::Row);
  CHECK(seq::fx_kind(seq::fx::kPanning) == seq::FxKind::Voice);
}

TEST_CASE("seq fx: the sanitiser (a type this firmware cannot run becomes None for this use, a value is clamped, None carries no value)") {
  std::uint8_t t = 63;  // the reserved core range: inert
  std::uint8_t v = 9;
  seq::fx_sanitize(t, v);
  CHECK(t == 0);
  CHECK(v == 0);
  t = 39;  // Bit depth (OS-IFX1): runs, 1..16
  v = 200;
  seq::fx_sanitize(t, v);
  CHECK(t == 39);
  CHECK(v == 16);
  v = 0;
  seq::fx_sanitize(t, v);
  CHECK(v == 1);
  t = 38;  // Overdrive: 0..100
  v = 200;
  seq::fx_sanitize(t, v);
  CHECK(t == 38);
  CHECK(v == 100);
  t = 14;  // Chord (OS-PFX): runs, 0..29
  v = 200;
  seq::fx_sanitize(t, v);
  CHECK(t == 14);
  CHECK(v == 29);
  t = 21;  // Arp, 0..33
  v = 200;
  seq::fx_sanitize(t, v);
  CHECK(t == 21);
  CHECK(v == 33);
  t = 40;  // Tune, 0..48
  v = 200;
  seq::fx_sanitize(t, v);
  CHECK(t == 40);
  CHECK(v == 48);
  t = 3;
  v = 200;  // Roll, range 0..53
  seq::fx_sanitize(t, v);
  CHECK(t == 3);
  CHECK(v == 53);
  t = 15;
  v = 1;  // Tempo, 5..200
  seq::fx_sanitize(t, v);
  CHECK(v == 5);
  t = 15;
  v = 250;
  seq::fx_sanitize(t, v);
  CHECK(v == 200);
  t = 0;
  v = 77;
  seq::fx_sanitize(t, v);
  CHECK(v == 0);
  t = 100;  // reserved core range
  v = 5;
  seq::fx_sanitize(t, v);
  CHECK(t == 0);
  t = 130;  // an engine FX: untouched
  v = 250;
  seq::fx_sanitize(t, v);
  CHECK(t == 130);
  CHECK(v == 250);
  t = 17;
  v = 10;  // Swing, 25..75
  seq::fx_sanitize(t, v);
  CHECK(v == 25);
  t = 43;
  v = 0;  // Speed, 1..32
  seq::fx_sanitize(t, v);
  CHECK(v == 1);
  t = 44;
  v = 255;  // Tempo fine: any byte
  seq::fx_sanitize(t, v);
  CHECK(v == 255);
  t = 45;
  v = 255;  // Order jump 0..254
  seq::fx_sanitize(t, v);
  CHECK(v == 254);
  // Idempotent, for every (type, value)
  for (std::uint32_t ty = 0; ty < 256; ++ty) {
    for (std::uint32_t va = 0; va < 256; va += 5) {
      std::uint8_t a = static_cast<std::uint8_t>(ty);
      std::uint8_t b = static_cast<std::uint8_t>(va);
      seq::fx_sanitize(a, b);
      std::uint8_t a2 = a;
      std::uint8_t b2 = b;
      seq::fx_sanitize(a2, b2);
      CHECK(a == a2);
      CHECK(b == b2);
    }
  }
}

TEST_CASE("seq fx: Roll = 9 x direction + period, the periods in microsteps") {
  const std::uint8_t ms[9] = {0, 48, 24, 16, 12, 8, 6, 4, 3};  // none, 1/1, 1/2, 1/3, 1/4, 1/6, 1/8, 1/12, 1/16 of a step (48 microsteps)
  for (std::uint32_t v = 0; v <= 53; ++v) {
    CHECK(seq::roll_period(static_cast<std::uint8_t>(v)) == v % 9);
    CHECK(static_cast<std::uint32_t>(seq::roll_dir(static_cast<std::uint8_t>(v))) == v / 9);
    CHECK(seq::roll_microsteps(seq::roll_period(static_cast<std::uint8_t>(v))) == ms[v % 9]);
  }
  CHECK(seq::roll_dir(53) == seq::RollDir::NoteRandom);
  CHECK(seq::roll_dir(9) == seq::RollDir::VolDown);
  CHECK(seq::roll_dir(18) == seq::RollDir::VolUp);
  CHECK(seq::roll_dir(27) == seq::RollDir::NoteDown);
  CHECK(seq::roll_dir(36) == seq::RollDir::NoteUp);
  CHECK(seq::roll_microsteps(9) == 0);  // out of range is none
  // a step of 48 microsteps is a whole number of repeats for every period but 1/3 and 1/6... check the arithmetic: 48 / ms is an integer for all of them
  for (std::uint32_t p = 1; p <= 8; ++p) {
    CHECK(48u % seq::roll_microsteps(static_cast<std::uint8_t>(p)) == 0u);
  }
}

TEST_CASE("seq fx: Micro-move is v + 1 rescaled from 0..100 to 1..48 and constrained to 0..47, for every value") {
  for (std::uint32_t v = 0; v <= 100; ++v) {
    long off = rescale(static_cast<long>(v) + 1, 0, 100, 1, 48);
    off = off < 0 ? 0 : (off > 47 ? 47 : off);
    CHECK_MESSAGE(seq::micromove_start(static_cast<std::uint8_t>(v)) == off, v);
  }
  CHECK(seq::micromove_start(0) == 1);    // no delay
  CHECK(seq::micromove_start(100) == 47); // 46 microsteps: the last two of the 48 are never reached
  CHECK(seq::micromove_start(50) == 25);  // 24 microsteps, half a step
  CHECK(seq::micromove_start(250) == 47); // beyond the range is the range's end
  // monotonic, and every start is a legal microstep
  std::uint8_t prev = 0;
  for (std::uint32_t v = 0; v <= 100; ++v) {
    const std::uint8_t s = seq::micromove_start(static_cast<std::uint8_t>(v));
    CHECK(s >= prev);
    CHECK(s >= 1);
    prev = s;
  }
}

TEST_CASE("seq fx: stutter periods, velocity scale, groups") {
  const std::uint16_t st[16] = {0, 768, 576, 384, 288, 192, 144, 96, 48, 24, 16, 12, 8, 6, 4, 3};
  for (std::uint32_t v = 0; v < 16; ++v) {
    CHECK(seq::stutter_microsteps(static_cast<std::uint8_t>(v)) == st[v]);
  }
  CHECK(seq::stutter_microsteps(16) == 0);
  // velocity: the integer rescale of 0..100 onto 0..127, v * 127 / 100, truncating
  for (std::uint32_t v = 0; v <= 100; ++v) {
    CHECK(seq::velocity_from_fx(static_cast<std::uint8_t>(v)) == rescale(static_cast<long>(v), 0, 100, 0, 127));
  }
  CHECK(seq::velocity_from_fx(100) == 127);
  CHECK(seq::velocity_from_fx(255) == 127);
  // the groups: the three filters are one, the velocity pair one, engine FX one per column
  CHECK(seq::fx_group(seq::fx::kLowPass, 0) == seq::FxGroup::Filter);
  CHECK(seq::fx_group(seq::fx::kHighPass, 1) == seq::FxGroup::Filter);
  CHECK(seq::fx_group(seq::fx::kBandPass, 0) == seq::FxGroup::Filter);
  CHECK(seq::fx_group(seq::fx::kVelocity, 0) == seq::fx_group(seq::fx::kRandomVelocity, 1));
  CHECK(seq::fx_group(130, 0) == seq::FxGroup::Engine0);
  CHECK(seq::fx_group(130, 1) == seq::FxGroup::Engine1);
  CHECK(seq::fx_group(seq::fx::kPanning, 0) != seq::fx_group(seq::fx::kPanningLfo, 0));
  CHECK(seq::fx_group(seq::fx::kRoll, 0) == seq::FxGroup::None);
  // every voice type has a group, every other kind has none (except the velocity pair)
  for (std::uint32_t t = 1; t < 43; ++t) {
    const seq::FxKind k = seq::fx_kind(static_cast<std::uint8_t>(t));
    const seq::FxGroup g = seq::fx_group(static_cast<std::uint8_t>(t), 0);
    CHECK_MESSAGE((k == seq::FxKind::Voice || k == seq::FxKind::RowVoice) == (g != seq::FxGroup::None), t);
  }
  CHECK(seq::kFxGroupCount <= 32u);
}
