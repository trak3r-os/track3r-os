// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <limits>

using namespace o3os::model;

namespace {

Project& fresh() {
  Project& p = o3os::test::arena(0).project;
  p.reset();
  return p;
}

Step step_of(std::uint8_t note) {
  Step s = kEmptyStep;
  s.note = note;
  return s;
}

Instrument sample_instrument() {
  Instrument in;
  init_instrument(in, InstrumentKind::Sample);
  std::strcpy(in.name, "Kick");
  return in;
}

}  // namespace

// The declared layout (ADR 0011): these are the numbers docs/formats/o3-project.md quotes.
static_assert(sizeof(Step) == 7 && alignof(Step) == 1, "step");
static_assert(sizeof(Pattern) == 14384, "pattern");
static_assert(sizeof(Instrument) == 720, "instrument");
static_assert(kModelArenaBytes == 1933760, "arena");
static_assert(alignof(ModelArena) == 8, "arena alignment");
static_assert(kMaxPatterns * sizeof(Pattern) + kMaxInstruments * sizeof(Instrument) + kUnknownPoolBytes < kModelArenaBytes,
              "the pools are most of the arena");
// Worst case in SDRAM: under 12% of 16 MiB, which leaves this OS's sample memory budget (11.25 MiB) and 2.9 MiB.
static_assert(kModelArenaBytes < (16u << 20) / 8, "arena stays under an eighth of the SDRAM");
static_assert(kModelArenaBytes + 11796480u < (16u << 20), "arena plus the sample memory budget fit the SDRAM");

TEST_CASE("model: limits table (one place) matches the spec") {
  CHECK(kMaxTracks == 16);
  CHECK(kDefaultActiveTracks == 12);
  CHECK(kMaxPatternLength == 128);
  CHECK(kMaxSongPositions == 255);
  CHECK(kMaxPatternId == 255);
  CHECK(kMaxPatterns <= kMaxPatternId);
  CHECK(kMaxInstruments == 64);
}

TEST_CASE("model: step is seven packed bytes, fx classes follow ADR 0004") {
  CHECK(fx_class(0) == FxClass::Og);
  CHECK(fx_class(42) == FxClass::Og);
  CHECK(fx_class(43) == FxClass::Reserved);
  CHECK(fx_class(127) == FxClass::Reserved);
  CHECK(fx_class(128) == FxClass::Engine);
  CHECK(fx_class(255) == FxClass::Engine);
  CHECK(is_empty(kEmptyStep));
  Step s = kEmptyStep;
  s.vol = 0;  // volume 0 is a value, not "unset"
  CHECK_FALSE(is_empty(s));
  s = kEmptyStep;
  s.fx[1].value = 1;
  CHECK_FALSE(is_empty(s));
  const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(&kEmptyStep);
  CHECK(raw[2] == kVolumeUnset);  // the in-memory layout is the file layout
}

TEST_CASE("model: reset gives a valid default project") {
  Project& p = fresh();
  CHECK(p.check_invariants());
  CHECK(std::strlen(p.name()) == 0);
  CHECK(p.tempo_x100() == kDefaultTempoX100);
  CHECK(p.swing() == 50);
  CHECK(p.default_pattern_length() == 32);
  CHECK(p.active_tracks() == 12);
  CHECK(p.master().volume == kDefaultVolume);
  CHECK(p.track(0)->type == TrackType::Audio);
  CHECK(p.track(15)->volume == kDefaultVolume);
  CHECK(p.track(16) == nullptr);
  CHECK(o3os::test::is_default_empty(p));
  CHECK(p.pattern(1) == nullptr);
  CHECK(p.instrument(0) == nullptr);
  CHECK(p.order(0) == 0);
}

TEST_CASE("model: reset keeps every generation counter moving") {
  Project& p = fresh();
  const std::uint32_t g = p.generation();
  const std::uint32_t t = p.tracks_generation();
  const std::uint32_t s = p.song_generation();
  const std::uint32_t i = p.instrument_generation(5);
  p.reset();
  CHECK(p.generation() != g);
  CHECK(p.tracks_generation() != t);
  CHECK(p.song_generation() != s);
  CHECK(p.instrument_generation(5) != i);
}

TEST_CASE("model: scalar mutators check their range and bump generations only when accepted") {
  Project& p = fresh();
  std::uint32_t g = p.generation();
  const std::uint32_t tg = p.tracks_generation();

  CHECK(p.set_tempo_x100(kMinTempoX100 - 1) == Status::BadValue);
  CHECK(p.set_tempo_x100(kMaxTempoX100 + 1) == Status::BadValue);
  CHECK(p.set_swing(24) == Status::BadValue);
  CHECK(p.set_swing(76) == Status::BadValue);
  CHECK(p.set_default_pattern_length(0) == Status::BadValue);
  CHECK(p.set_default_pattern_length(129) == Status::BadValue);
  CHECK(p.set_active_tracks(0) == Status::BadValue);
  CHECK(p.set_active_tracks(17) == Status::BadValue);
  CHECK(p.set_master(MasterSettings{101, 0, 0, 0, 0}) == Status::BadValue);
  CHECK(p.set_master(MasterSettings{0, 0, 0, 0, 16}) == Status::BadValue);
  CHECK(p.set_name("0123456789abcdef0123456789ABCDEFX") == Status::BadValue);  // 33 characters
  CHECK(p.generation() == g);  // nothing accepted, nothing bumped
  CHECK(p.tracks_generation() == tg);

  CHECK(p.set_tempo_x100(14000) == Status::Ok);
  CHECK(p.generation() != g);
  g = p.generation();
  CHECK(p.set_swing(60) == Status::Ok);
  CHECK(p.set_default_pattern_length(64) == Status::Ok);
  CHECK(p.set_active_tracks(16) == Status::Ok);
  CHECK(p.set_master(MasterSettings{100, 90, 80, 70, kMasterLimiter}) == Status::Ok);
  CHECK(p.set_name("Demo") == Status::Ok);
  CHECK(p.generation() == g + 5);
  CHECK(p.tracks_generation() == tg + 6);  // tempo, swing, length, active, master, name
  CHECK(p.tempo_x100() == 14000);
  CHECK(p.swing() == 60);
  CHECK(std::strcmp(p.name(), "Demo") == 0);
  CHECK(p.check_invariants());
}

TEST_CASE("model: text is stored whole or refused, never cut") {
  Project& p = fresh();
  CHECK(p.set_name("abc") == Status::Ok);
  CHECK(p.set_name("a\0b", 3) == Status::BadValue);  // a NUL inside
  CHECK(std::strcmp(p.name(), "abc") == 0);          // unchanged
  CHECK(p.set_name("xy", 2) == Status::Ok);
  CHECK(std::strcmp(p.name(), "xy") == 0);  // nothing of "abc" left behind
  CHECK(p.set_name(nullptr, 0) == Status::Ok);
  CHECK(std::strlen(p.name()) == 0);
  CHECK(p.set_name(nullptr, 1) == Status::BadValue);
  CHECK(p.set_track_name(16, "x", 1) == Status::BadIndex);
  CHECK(p.set_track_name(0, "012345678901234567890", 21) == Status::BadValue);  // 21 > 20
  CHECK(p.set_track_name(0, "012345678901234567890", 20) == Status::Ok);
}

TEST_CASE("model: track slots are validated as a whole") {
  Project& p = fresh();
  TrackSlot t = *p.track(2);
  t.type = TrackType::ExternalAudio;
  t.flags = kTrackMute | kTrackSolo;
  t.bus = kMaxOutputBus;
  CHECK(p.set_track(2, t) == Status::Ok);
  CHECK(p.track(2)->type == TrackType::ExternalAudio);
  CHECK(p.set_track(16, t) == Status::BadIndex);
  TrackSlot bad = t;
  bad.type = static_cast<TrackType>(3);
  CHECK(p.set_track(2, bad) == Status::BadValue);
  bad = t;
  bad.flags = 4;
  CHECK(p.set_track(2, bad) == Status::BadValue);
  bad = t;
  bad.volume = 101;
  CHECK(p.set_track(2, bad) == Status::BadValue);
  bad = t;
  bad.pan = 101;
  CHECK(p.set_track(2, bad) == Status::BadValue);
  bad = t;
  bad.bus = kMaxOutputBus + 1;
  CHECK(p.set_track(2, bad) == Status::BadValue);
  bad = t;
  std::memset(bad.name, 'x', sizeof bad.name);  // not terminated
  CHECK(p.set_track(2, bad) == Status::BadValue);
  CHECK(p.check_invariants());
}

TEST_CASE("model: the song is an order list of at most 255 pattern ids") {
  Project& p = fresh();
  CHECK(p.append_order(0) == Status::BadValue);
  CHECK(p.append_order(256) == Status::BadValue);
  CHECK(p.insert_order(1, 5) == Status::BadIndex);
  const std::uint32_t sg = p.song_generation();
  CHECK(p.append_order(3) == Status::Ok);
  CHECK(p.append_order(5) == Status::Ok);
  CHECK(p.insert_order(1, 4) == Status::Ok);
  CHECK(p.song_length() == 3);
  CHECK(p.order(0) == 3);
  CHECK(p.order(1) == 4);
  CHECK(p.order(2) == 5);
  CHECK(p.order(3) == 0);
  CHECK(p.set_order(3, 1) == Status::BadIndex);
  CHECK(p.set_order(0, 9) == Status::Ok);
  CHECK(p.remove_order(1) == Status::Ok);
  CHECK(p.song_length() == 2);
  CHECK(p.order(0) == 9);
  CHECK(p.order(1) == 5);
  CHECK(p.remove_order(2) == Status::BadIndex);
  CHECK(p.song_generation() == sg + 5);
  CHECK(p.clear_order() == Status::Ok);
  CHECK(p.song_length() == 0);
  for (std::uint32_t i = 0; i < kMaxSongPositions; ++i) {
    NEED(p.append_order(1 + i % 255) == Status::Ok);
  }
  const std::uint32_t g = p.generation();
  CHECK(p.append_order(1) == Status::Full);
  CHECK(p.insert_order(0, 1) == Status::Full);
  CHECK(p.generation() == g);
  CHECK(p.check_invariants());
}

TEST_CASE("model: pattern pool, ids and per-pattern generations") {
  Project& p = fresh();
  CHECK(p.create_pattern(0, 16) == Status::BadIndex);
  CHECK(p.create_pattern(256, 16) == Status::BadIndex);
  CHECK(p.create_pattern(1, 0) == Status::BadValue);
  CHECK(p.create_pattern(1, 129) == Status::BadValue);
  CHECK(p.create_pattern(200, 16) == Status::Ok);
  CHECK(p.create_pattern(200, 16) == Status::Exists);
  const Pattern* pat = p.pattern(200);
  NEED(pat != nullptr);
  CHECK(pat->id == 200);
  for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
    CHECK(pat->track_length[t] == 16);
  }
  CHECK(p.pattern_length(200) == 16);
  CHECK(p.pattern_length(1) == 0);
  CHECK(is_empty(pat->steps[15][127]));
  CHECK(p.pattern(1) == nullptr);
  CHECK(p.pattern_generation(1) == 0);

  const std::uint32_t pg = p.pattern_generation(200);
  const std::uint32_t g = p.generation();
  CHECK(p.set_step(200, 3, 7, step_of(60)) == Status::Ok);
  CHECK(p.pattern(200)->steps[3][7].note == 60);
  CHECK(p.pattern_generation(200) == pg + 1);
  CHECK(p.generation() == g + 1);
  CHECK(p.set_step(1, 0, 0, step_of(1)) == Status::NotFound);
  CHECK(p.set_step(200, 16, 0, step_of(1)) == Status::BadIndex);
  CHECK(p.set_step(200, 0, 128, step_of(1)) == Status::BadIndex);
  CHECK(p.pattern_generation(200) == pg + 1);  // rejected: nothing bumped
  CHECK(p.generation() == g + 1);

  // steps past the length are kept, and so is the data when the length changes
  CHECK(p.set_step(200, 0, 100, step_of(72)) == Status::Ok);
  CHECK(p.set_pattern_length(200, 128) == Status::Ok);
  CHECK(p.set_pattern_length(200, 4) == Status::Ok);
  CHECK(p.pattern(200)->steps[0][100].note == 72);
  CHECK(p.pattern(200)->track_length[15] == 4);
  CHECK(p.set_pattern_length(200, 0) == Status::BadValue);
  CHECK(p.set_pattern_length(200, 129) == Status::BadValue);
  CHECK(p.set_pattern_length(9, 4) == Status::NotFound);
  CHECK(p.pattern(200)->track_length[0] == 4);

  CHECK(p.set_pattern_name(200, "Verse", 5) == Status::Ok);
  CHECK(std::strcmp(p.pattern(200)->name, "Verse") == 0);
  CHECK(p.set_pattern_name(200, "0123456789012345678901234567890", 31) == Status::BadValue);  // 31 > 30
  CHECK(p.clear_pattern(200) == Status::Ok);
  CHECK(is_empty(p.pattern(200)->steps[3][7]));
  CHECK(p.pattern(200)->track_length[3] == 4);
  CHECK(std::strcmp(p.pattern(200)->name, "Verse") == 0);

  CHECK(p.delete_pattern(200) == Status::Ok);
  CHECK(p.pattern(200) == nullptr);
  CHECK(p.delete_pattern(200) == Status::NotFound);
  CHECK(p.pattern_count() == 0);
  CHECK(p.check_invariants());
}

TEST_CASE("model: a pattern's generation is never repeated, not even after delete and create in another slot") {
  Project& p = fresh();
  NEED(p.create_pattern(1, 8) == Status::Ok);  // pool slots 0, 1, 2
  NEED(p.create_pattern(2, 8) == Status::Ok);
  NEED(p.create_pattern(3, 8) == Status::Ok);
  for (int i = 0; i < 5; ++i) {  // slot 0 runs ahead of the others
    NEED(p.set_step(1, 0, 0, step_of(static_cast<std::uint8_t>(10 + i))) == Status::Ok);
  }
  CHECK(p.pattern_generation(1) == p.generation());  // stamped with the project counter at its last change
  const std::uint32_t seen = p.pattern_generation(1);
  NEED(p.delete_pattern(1) == Status::Ok);
  CHECK(p.pattern_generation(1) == 0);
  NEED(p.create_pattern(4, 8) == Status::Ok);  // takes the free slot 0
  NEED(p.delete_pattern(2) == Status::Ok);
  NEED(p.create_pattern(1, 8) == Status::Ok);  // id 1 again, now in slot 1
  CHECK(p.pattern_generation(1) != seen);
  CHECK(p.pattern_generation(1) == p.generation());
  std::uint32_t last = p.pattern_generation(1);
  for (int i = 0; i < 4; ++i) {
    NEED(p.set_step(1, 1, 0, step_of(static_cast<std::uint8_t>(20 + i))) == Status::Ok);
    CHECK(p.pattern_generation(1) != last);
    last = p.pattern_generation(1);
    CHECK(p.pattern_generation(3) != last);  // never equal to another pattern's value either
  }
  const std::uint32_t before_reset = p.pattern_generation(1);
  p.reset();
  CHECK(p.generation() != before_reset);
  NEED(p.create_pattern(1, 8) == Status::Ok);
  CHECK(p.pattern_generation(1) != before_reset);
}

TEST_CASE("model: every track of a pattern has its own length, and the pattern's length is derived") {
  Project& p = fresh();
  NEED(p.create_pattern(5, 16) == Status::Ok);
  const std::uint32_t pg = p.pattern_generation(5);
  const std::uint32_t g = p.generation();
  CHECK(p.set_track_length(5, 2, 24) == Status::Ok);
  CHECK(p.pattern(5)->track_length[2] == 24);
  CHECK(p.pattern(5)->track_length[1] == 16);
  CHECK(p.pattern_generation(5) == pg + 1);  // bumps the pattern's and the project's generation
  CHECK(p.generation() == g + 1);
  CHECK(p.set_track_length(5, 15, 128) == Status::Ok);
  CHECK(p.set_track_length(5, 0, 1) == Status::Ok);
  CHECK(p.pattern(5)->track_length[0] == 1);

  // rejected: nothing changes, nothing is bumped
  const std::uint32_t pg2 = p.pattern_generation(5);
  CHECK(p.set_track_length(5, 16, 8) == Status::BadIndex);
  CHECK(p.set_track_length(5, 0, 0) == Status::BadValue);
  CHECK(p.set_track_length(5, 0, 129) == Status::BadValue);
  CHECK(p.set_track_length(6, 0, 8) == Status::NotFound);
  CHECK(p.pattern_generation(5) == pg2);
  CHECK(p.pattern(5)->track_length[0] == 1);

  // the derived length is the longest of the active tracks (12 by default): track 15 (128) is not one of them
  CHECK(p.active_tracks() == 12);
  CHECK(p.pattern_length(5) == 24);
  CHECK(p.set_active_tracks(16) == Status::Ok);
  CHECK(p.pattern_length(5) == 128);
  CHECK(p.set_active_tracks(1) == Status::Ok);
  CHECK(p.pattern_length(5) == 1);
  CHECK(p.pattern_length(99) == 0);
  CHECK(p.check_invariants());
}

TEST_CASE("model: set_steps is all or nothing") {
  Project& p = fresh();
  NEED(p.create_pattern(1, 16) == Status::Ok);
  Step row[10];
  for (int i = 0; i < 10; ++i) {
    row[i] = step_of(static_cast<std::uint8_t>(10 + i));
  }
  const std::uint32_t g = p.generation();
  CHECK(p.set_steps(1, 0, 120, row, 10) == Status::BadIndex);  // 120 + 10 > 128
  CHECK(p.set_steps(1, 0, 129, row, 0) == Status::BadIndex);
  CHECK(p.set_steps(1, 16, 0, row, 1) == Status::BadIndex);
  CHECK(p.set_steps(1, 0, 0, nullptr, 3) == Status::BadValue);
  CHECK(p.set_steps(2, 0, 0, row, 1) == Status::NotFound);
  CHECK(p.generation() == g);
  CHECK(is_empty(p.pattern(1)->steps[0][120]));
  CHECK(p.set_steps(1, 0, 118, row, 10) == Status::Ok);
  CHECK(p.pattern(1)->steps[0][127].note == 19);
  CHECK(p.set_steps(1, 5, 0, nullptr, 0) == Status::Ok);  // accepted, a no-op
  CHECK(p.generation() == g + 1);
}

TEST_CASE("model: the pattern pool holds 128 patterns and refuses the 129th, a freed slot is reused") {
  Project& p = fresh();
  for (std::uint32_t i = 0; i < kMaxPatterns; ++i) {
    NEED(p.create_pattern(255 - i, 8) == Status::Ok);
  }
  CHECK(p.pattern_count() == kMaxPatterns);
  const std::uint32_t g = p.generation();
  CHECK(p.create_pattern(1, 8) == Status::Full);  // id 1 is free but the pool is not
  CHECK(p.generation() == g);
  CHECK(p.delete_pattern(200) == Status::Ok);
  CHECK(p.create_pattern(1, 8) == Status::Ok);
  CHECK(p.pattern(1) != nullptr);
  CHECK(p.pattern(200) == nullptr);
  CHECK(p.check_invariants());
}

TEST_CASE("model: instrument validation") {
  Project& p = fresh();
  Instrument in = sample_instrument();
  const std::uint32_t ig = p.instrument_generation(3);
  CHECK(p.set_instrument(3, in) == Status::Ok);
  CHECK(p.instrument_generation(3) == ig + 1);
  CHECK(p.instrument_count() == 1);
  NEED(p.instrument(3) != nullptr);
  CHECK(p.instrument(3)->sampler.bit_depth == 16);
  CHECK(p.set_instrument(64, in) == Status::BadIndex);

  Instrument bad = in;
  bad.kind = InstrumentKind::None;
  CHECK(p.set_instrument(4, bad) == Status::BadValue);
  bad = in;
  bad.kind = static_cast<InstrumentKind>(9);
  CHECK(p.set_instrument(4, bad) == Status::BadValue);
  bad = in;
  std::memset(bad.name, 'n', sizeof bad.name);
  CHECK(p.set_instrument(4, bad) == Status::BadValue);
  bad = in;
  bad.midi_channel = 1;  // on a sample instrument
  CHECK(p.set_instrument(4, bad) == Status::BadValue);
  bad = in;
  bad.kind = InstrumentKind::Midi;
  CHECK(p.set_instrument(4, bad) == Status::BadValue);  // channel 0
  bad.midi_channel = 17;
  CHECK(p.set_instrument(4, bad) == Status::BadValue);
  bad.midi_channel = 16;
  CHECK(p.set_instrument(4, bad) == Status::Ok);
  bad = in;
  bad.kind = InstrumentKind::Module;  // no module in the table
  bad.module_index = 0;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.param_len = 1;  // blob on a sample instrument
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.cutoff = std::numeric_limits<float>::quiet_NaN();
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.envelope[5].sustain = std::numeric_limits<float>::infinity();
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.lfo[2].amount = -std::numeric_limits<float>::infinity();
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.play_mode = 8;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.volume = 101;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.bit_depth = 3;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad.sampler.bit_depth = 17;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.slice_count = 49;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.selected_slice = 48;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  bad = in;
  bad.sampler.grain_shape = 3;
  CHECK(p.set_instrument(5, bad) == Status::BadValue);
  CHECK(p.instrument(5) == nullptr);
  CHECK(p.instrument_count() == 2);

  CHECK(p.clear_instrument(3) == Status::Ok);
  CHECK(p.clear_instrument(3) == Status::NotFound);
  CHECK(p.clear_instrument(70) == Status::BadIndex);
  CHECK(p.instrument(3) == nullptr);
  CHECK(p.instrument_count() == 1);
  CHECK(p.check_invariants());
}

TEST_CASE("model: sample paths stay inside the project folder") {
  struct Case {
    const char* path;
    bool ok;
  };
  const Case cases[] = {
      {"", true},
      {"kick.wav", true},
      {"samples/kick.wav", true},
      {"a/b/c.wav", true},
      {"..", false},
      {"../x.wav", false},
      {"a/../b.wav", false},
      {"a/..", false},
      {".", false},
      {"./a.wav", false},
      {"/abs/kick.wav", false},
      {"a//b.wav", false},
      {"a/", false},
      {"a\\b.wav", false},
      {"C:kick.wav", false},
      {"a\tb.wav", false},
      {"a\x7f.wav", false},
      {"..a/b", true},   // a name that merely starts with dots
      {"a..b", true},
      {".hidden", true},
  };
  for (const Case& c : cases) {
    CAPTURE(c.path);
    CHECK(is_safe_relative_path(c.path, std::strlen(c.path)) == c.ok);
  }
  Project& p = fresh();
  Instrument in = sample_instrument();
  std::strcpy(in.sample_path, "../../etc/passwd");
  CHECK(p.set_instrument(0, in) == Status::BadValue);
  std::strcpy(in.sample_path, "samples/ok.wav");
  CHECK(p.set_instrument(0, in) == Status::Ok);
}

TEST_CASE("model: modules and module instruments") {
  Project& p = fresh();
  CHECK(p.add_module("tb303", 5, 1, 0) == Status::Ok);
  CHECK(p.add_module("tb303", 5, 1, 0) == Status::Exists);
  CHECK(p.add_module("", 0, 1, 0) == Status::BadValue);
  CHECK(p.add_module("../evil", 7, 1, 0) == Status::BadValue);
  CHECK(p.add_module("a/b", 3, 1, 0) == Status::BadValue);
  CHECK(p.add_module("..", 2, 1, 0) == Status::BadValue);
  CHECK(p.add_module("ok", 2, 256, 0) == Status::BadValue);
  CHECK(p.add_module("0123456789012345678901234567890X", 32, 1, 0) == Status::BadValue);  // 32 > 31
  CHECK(p.module_count() == 1);
  CHECK(p.module(0)->abi_major == 1);
  CHECK(p.module(1) == nullptr);

  Instrument in;
  init_instrument(in, InstrumentKind::Module);
  std::strcpy(in.name, "Acid");
  in.module_index = 0;
  in.param_len = kModuleParamBytes;
  std::memset(in.params, 0x5A, sizeof in.params);
  CHECK(p.set_instrument(10, in) == Status::Ok);
  in.param_len = kModuleParamBytes + 1;
  CHECK(p.set_instrument(10, in) == Status::BadValue);
  in.param_len = 4;
  in.module_index = 1;  // not in the table
  CHECK(p.set_instrument(11, in) == Status::BadValue);

  CHECK(p.clear_modules() == Status::BadValue);  // instrument 10 still names it
  CHECK(p.module_count() == 1);
  CHECK(p.clear_instrument(10) == Status::Ok);
  CHECK(p.clear_modules() == Status::Ok);
  CHECK(p.module_count() == 0);

  for (std::uint32_t i = 0; i < kMaxModules; ++i) {
    char name[8];
    name[0] = 'm';
    name[1] = static_cast<char>('a' + i);
    CHECK(p.add_module(name, 2, 1, 0) == Status::Ok);
  }
  CHECK(p.add_module("zz", 2, 1, 0) == Status::Full);
  CHECK(p.check_invariants());
}

TEST_CASE("model: the instrument parameter blob is stored canonically") {
  Project& p = fresh();
  NEED(p.add_module("m", 1, 1, 0) == Status::Ok);
  Instrument in;
  init_instrument(in, InstrumentKind::Module);
  in.module_index = 0;
  std::strcpy(in.name, "abcdef");
  in.name[3] = '\0';  // "abc", garbage "ef" after the terminator
  in.param_len = 2;
  in.params[0] = 1;
  in.params[1] = 2;
  in.params[2] = 3;  // past param_len
  NEED(p.set_instrument(0, in) == Status::Ok);
  const Instrument* out = p.instrument(0);
  CHECK(std::strcmp(out->name, "abc") == 0);
  CHECK(out->name[4] == '\0');
  CHECK(out->params[2] == 0);
}

TEST_CASE("model: unknown chunks live in a bounded pool and overflow is counted") {
  Project& p = fresh();
  const std::uint32_t id = 0x58545241;  // "XTRA"
  CHECK(p.unknown_begin(0x01020304, 4) == Status::BadValue);  // not printable
  CHECK(p.unknown_append(nullptr, 0) == Status::BadValue);    // nothing is being filled
  NEED(p.unknown_begin(id, 6) == Status::Ok);
  const std::uint8_t a[4] = {1, 2, 3, 4};
  const std::uint8_t b[3] = {5, 6, 7};
  CHECK(p.unknown_append(a, 4) == Status::Ok);
  CHECK(p.unknown_append(b, 3) == Status::BadValue);  // 4 + 3 > 6
  CHECK(p.unknown_append(b, 2) == Status::Ok);
  CHECK(p.unknown_count() == 1);
  CHECK(p.unknown(0)->length == 6);
  CHECK(p.unknown_data(0)[5] == 6);
  CHECK(p.unknown_data(1) == nullptr);

  // fill the pool with the rest of its bytes, then one more chunk does not fit
  NEED(p.unknown_begin(id, kUnknownPoolBytes - 6) == Status::Ok);
  CHECK(p.unknown_dropped_chunks() == 0);
  CHECK(p.unknown_begin(id, 1) == Status::Full);
  CHECK(p.unknown_dropped_chunks() == 1);
  CHECK(p.unknown_dropped_bytes() == 1);
  CHECK(p.unknown_begin(id, 0xFFFFFFFFu) == Status::Full);  // a hostile length cannot wrap the counter
  CHECK(p.unknown_dropped_bytes() == 0xFFFFFFFFu);
  CHECK(p.unknown_count() == 2);
  CHECK(p.check_invariants());

  p.reset();
  for (std::uint32_t i = 0; i < kMaxUnknownChunks; ++i) {
    NEED(p.unknown_begin(id, 1) == Status::Ok);
  }
  CHECK(p.unknown_begin(id, 1) == Status::Full);  // 17th chunk, pool far from full
  CHECK(p.unknown_count() == kMaxUnknownChunks);
  p.clear_unknown();
  CHECK(p.unknown_count() == 0);
  CHECK(p.unknown_dropped_chunks() == 0);
}

TEST_CASE("model: a maximal project holds every limit and passes the invariants") {
  Project& p = o3os::test::arena(0).project;
  o3os::test::fill_maximal(p);
  CHECK(p.pattern_count() == kMaxPatterns);
  CHECK(p.instrument_count() == kMaxInstruments);
  CHECK(p.module_count() == kMaxModules);
  CHECK(p.song_length() == kMaxSongPositions);
  CHECK(p.unknown_count() == kMaxUnknownChunks);
  CHECK(p.active_tracks() == kMaxTracks);
  CHECK(p.pattern(255) != nullptr);
  CHECK(p.pattern(128) == nullptr);
  CHECK(p.check_invariants());
}
