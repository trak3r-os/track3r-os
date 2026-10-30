// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011b: the performance mode's slots (seq/perform.h) against the rules (cited there) and against a model of the engine. The commands a pad, the encoder, a track
// button and the edit operations send; the exclusion of the same FX and of the filters that block each other; the latch after a release; leaving the mode; a full ring (the
// refused commands go out at the next apply); and a random walk over every operation whose invariant is that after each one the engine, as the commands built it (the control
// table, the stutters, the play modes, the pattern length), is exactly what the slots say. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/rng.h"
#include "support/zero_bank.h"

#include <o3os/audio/events.h>
#include <o3os/audio/voices.h>
#include <o3os/seq/control.h>
#include <o3os/seq/perform.h>

#include <cstdint>
#include <string>
#include <vector>

namespace seq = o3os::seq;
namespace audio = o3os::audio;
using seq::Command;
using seq::FxGroup;
using seq::PerfFx;
using o3os::test::Rng;

namespace {

struct Log final : seq::CommandSink {
  std::vector<Command> sent;
  unsigned room = 1000;  // commands accepted before the ring "is full"
  bool push(const Command& c) noexcept override {
    if (room == 0u) {
      return false;
    }
    --room;
    sent.push_back(c);
    return true;
  }
};

// The engine as the commands build it: the control table for the groups, and the sequencer's own FX.
struct Engine {
  seq::ControlTable ctl;
  std::uint8_t stutter[16] = {};
  std::uint8_t mode[16] = {};
  std::uint32_t length = 0;
  Engine() { ctl.reset(); }
  void take(const Command& c) {
    seq::ModChange ch[1];
    switch (c.kind) {
      case Command::Kind::PerfControl:
        if ((c.c >> 16) & 1u) {
          (void)ctl.end_perf(c.a, static_cast<FxGroup>(c.b), ch);
        } else {
          (void)ctl.set_perf(c.a, static_cast<FxGroup>(c.b), static_cast<std::int16_t>(c.c & 0xFFFFu), ch);
        }
        break;
      case Command::Kind::PerfStutter: stutter[c.a] = static_cast<std::uint8_t>(c.b); break;
      case Command::Kind::PerfTrack: mode[c.a] = static_cast<std::uint8_t>((c.c >> 8) & 3u); break;
      case Command::Kind::PerfPatternLen: length = c.c; break;
      default: break;
    }
  }
  void take(const Log& log, std::size_t from) {
    for (std::size_t i = from; i < log.sent.size(); ++i) {
      take(log.sent[i]);
    }
  }
};

constexpr unsigned pad(unsigned slot, unsigned row) { return row * seq::kPerfSlots + slot; }

}  // namespace

TEST_CASE("perform: our defaults - slot i holds FX i + 1 with its three default values, nothing selected, no track affected") {
  seq::PerformBank b;
  for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
    CHECK(static_cast<unsigned>(b.fx(s)) == s + 1u);
    CHECK(b.row(s) == 0);
    CHECK(b.live(s) == 0);
    CHECK_FALSE(b.active(s));
    CHECK_FALSE(b.held(s));
    for (unsigned r = 1; r < seq::kPerfRows; ++r) {
      CHECK(b.stored(s, r) == o3os::model::kPerfDefaultValues[s + 1u][r - 1u]);  // OS-PRFDEFAULTS
    }
  }
  // the bank of a new session is the project of a new one
  o3os::model::PerfState fresh;
  o3os::model::init_perf_state(fresh);
  CHECK(b.state() == fresh);
  CHECK(b.affected_mask() == 0);
  CHECK_FALSE(b.edit());
  CHECK(b.fx(0) == PerfFx::Volume);
  CHECK(b.fx(3) == PerfFx::LowPass);
  CHECK(b.fx(11) == PerfFx::VolumeLfoSpeed);
}

TEST_CASE("perform: the table - names fit a slot's column, ranges are ordered, labelled FX have a label for every value, the three filters block each other") {
  for (unsigned f = 1; f < seq::kPerfFxCount; ++f) {
    const seq::PerfFxInfo& i = seq::kPerfFxInfo[f];
    unsigned n = 0;
    while (i.name[n] != '\0') {
      ++n;
    }
    CHECK_MESSAGE((n >= 1 && n <= 8), i.name);
    CHECK(i.min < i.max);
    CHECK(i.min <= 0);  // 0 (no FX) is always a value
    CHECK(i.max >= 0);
    // the labelled FX are the four whose value is a choice (OS-PERFFX P1: BITDEPTH 0..15 is a number, the amount of the crush, and has none)
    const PerfFx fx = static_cast<PerfFx>(f);
    const bool labelled = fx == PerfFx::SamplePlayback || fx == PerfFx::StepRepeater || fx == PerfFx::PatternPlayMode || fx == PerfFx::PatternLength;
    CHECK((i.labels != nullptr) == labelled);
    if (i.labels != nullptr) {
      for (int v = 0; v <= i.max; ++v) {
        CHECK_MESSAGE((i.labels[v] != nullptr && i.labels[v][0] != '\0'), i.name << " " << v);
      }
    }
  }
  for (const PerfFx fx : {PerfFx::LowPass, PerfFx::HighPass, PerfFx::BandPass}) {
    const seq::PerfFxInfo& i = seq::perf_info(fx);
    CHECK(i.group == FxGroup::Filter);
    CHECK(i.live);
    CHECK(i.blocks[0] != fx);
    CHECK(i.blocks[1] != fx);
    CHECK(i.blocks[0] != i.blocks[1]);
    for (const PerfFx other : {PerfFx::LowPass, PerfFx::HighPass, PerfFx::BandPass}) {
      if (other != fx) {
        CHECK(((i.blocks[0] == other) != (i.blocks[1] == other)));
      }
    }
  }
  char text[8];
  seq::perf_value_text(PerfFx::Volume, 0, text);
  CHECK(std::string(text) == "---");
  seq::perf_value_text(PerfFx::Volume, -37, text);
  CHECK(std::string(text) == "-37");
  seq::perf_value_text(PerfFx::Volume, 100, text);
  CHECK(std::string(text) == "100");
  seq::perf_value_text(PerfFx::StepRepeater, 0, text);
  CHECK(std::string(text) == "OFF");
  seq::perf_value_text(PerfFx::StepRepeater, 14, text);
  CHECK(std::string(text) == "1/12");
  seq::perf_value_text(PerfFx::PatternPlayMode, 2, text);
  CHECK(std::string(text) == "PING");  // OS-BUG-PERFMODE: the labels are the engine's modes (seq_sequencer_test.cpp runs each)
  seq::perf_value_text(PerfFx::PatternPlayMode, 3, text);
  CHECK(std::string(text) == "RND");
  seq::perf_value_text(PerfFx::PatternLength, 8, text);
  CHECK(std::string(text) == "128");
  CHECK(seq::perf_length_steps(0) == 0);
  CHECK(seq::perf_length_steps(1) == 1);
  CHECK(seq::perf_length_steps(5) == 16);
  CHECK(seq::perf_length_steps(8) == 128);
}

// ---- OS-PERFFX P1: BITDEPTH (20) and OVERDRV (21) --------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("perform P1: BITDEPTH is id 20 and OVERDRV id 21, appended to the table, with the ranges the spec gives, and the engine acts on them") {
  CHECK(static_cast<unsigned>(PerfFx::PatternLength) == 19u);  // the ids stored in a project keep their meaning
  CHECK(static_cast<unsigned>(PerfFx::BitDepth) == 20u);
  CHECK(static_cast<unsigned>(PerfFx::Overdrive) == 21u);
  CHECK(seq::kPerfFxCount == 22u);
  CHECK(o3os::model::kPerfFxMax == 21u);
  const seq::PerfFxInfo& bits = seq::perf_info(PerfFx::BitDepth);
  CHECK(std::string(bits.name) == "BITDEPTH");  // 8 characters: the widest a slot's column takes
  CHECK(bits.min == 0);
  CHECK(bits.max == 15);  // the value is 16 - bits: 0 is clean (and is the FX's end), 15 leaves one bit
  CHECK(bits.group == FxGroup::BitDepth);
  CHECK(bits.live);
  CHECK(bits.labels == nullptr);
  const seq::PerfFxInfo& drive = seq::perf_info(PerfFx::Overdrive);
  CHECK(std::string(drive.name) == "OVERDRV");
  CHECK(drive.min == 0);
  CHECK(drive.max == 100);
  CHECK(drive.group == FxGroup::Overdrive);
  CHECK(drive.live);
  CHECK(drive.labels == nullptr);
  CHECK(seq::perf_info(static_cast<PerfFx>(22)).group == FxGroup::None);  // an id past the table reads as the empty row
  char text[8];
  seq::perf_value_text(PerfFx::BitDepth, 0, text);
  CHECK(std::string(text) == "---");
  seq::perf_value_text(PerfFx::BitDepth, 12, text);
  CHECK(std::string(text) == "12");
  seq::perf_value_text(PerfFx::Overdrive, 100, text);
  CHECK(std::string(text) == "100");
  // the command: the group's value, held to the FX's range; 0 is the end
  Command c{};
  NEED(seq::perf_command(3, PerfFx::BitDepth, 7, c));
  CHECK(c.kind == Command::Kind::PerfControl);
  CHECK(c.a == 3);
  CHECK(c.b == static_cast<std::uint16_t>(FxGroup::BitDepth));
  CHECK(c.c == 7u);
  NEED(seq::perf_command(3, PerfFx::BitDepth, 99, c));
  CHECK(c.c == 15u);
  NEED(seq::perf_command(3, PerfFx::BitDepth, -4, c));  // (the clamp: a value below the range is held to 0 and sent as a set to 0; it is not the end, which is only a value of 0 itself, next)
  CHECK(c.c == 0u);
  NEED(seq::perf_command(3, PerfFx::BitDepth, 0, c));
  CHECK(c.c == (1u << 16));
  NEED(seq::perf_command(11, PerfFx::Overdrive, 100, c));
  CHECK(c.b == static_cast<std::uint16_t>(FxGroup::Overdrive));
  CHECK(c.c == 100u);
  NEED(seq::perf_command(11, PerfFx::Overdrive, 150, c));
  CHECK(c.c == 100u);
  NEED(seq::perf_command(11, PerfFx::Overdrive, 0, c));
  CHECK(c.c == (1u << 16));
}

TEST_CASE("perform P1: a slot holding BITDEPTH or OVERDRV sets its value on the affected tracks, the encoder moves it inside the range, and the pad's end ends it") {
  seq::PerformBank b;
  Log log;
  o3os::model::PerfState st = b.state();
  st.fx[0] = static_cast<std::uint8_t>(PerfFx::BitDepth);
  st.fx[1] = static_cast<std::uint8_t>(PerfFx::Overdrive);
  st.value[0][0] = 6;
  st.value[1][0] = 70;
  b.load_slots(st);
  CHECK(b.fx(0) == PerfFx::BitDepth);
  CHECK(b.fx(1) == PerfFx::Overdrive);
  b.toggle_track(4, log);
  b.toggle_track(9, log);
  log.sent.clear();
  Engine e;
  b.pad_down(pad(0, 1), log);
  b.pad_down(pad(1, 1), log);
  e.take(log, 0);
  for (const unsigned t : {4u, 9u}) {
    CHECK(e.ctl.resolve(static_cast<std::uint8_t>(t), FxGroup::BitDepth).perf == 6);
    CHECK(e.ctl.resolve(static_cast<std::uint8_t>(t), FxGroup::Overdrive).perf == 70);
  }
  CHECK_FALSE(e.ctl.perf_holds(5, FxGroup::BitDepth));  // a track that is not affected
  // the encoder moves what is held, held to each FX's own range
  log.sent.clear();
  b.turn(+200, log);
  e.take(log, 0);
  CHECK(e.ctl.resolve(4, FxGroup::BitDepth).perf == 15);
  CHECK(e.ctl.resolve(4, FxGroup::Overdrive).perf == 100);
  log.sent.clear();
  b.turn(-30, log);
  e.take(log, 0);
  CHECK(e.ctl.resolve(4, FxGroup::BitDepth).perf == 0 + 0);
  CHECK_FALSE(e.ctl.perf_holds(4, FxGroup::BitDepth));  // 0 is the end: the clean sound is the instrument's own again
  CHECK(e.ctl.resolve(4, FxGroup::Overdrive).perf == 70);
  // row 0 ends it
  b.pad_up(pad(0, 1), log);
  b.pad_up(pad(1, 1), log);
  b.pad_down(pad(1, 0), log);
  b.pad_up(pad(1, 0), log);
  e.take(log, 0);
  CHECK_FALSE(e.ctl.perf_holds(9, FxGroup::Overdrive));
}

// The open question of the P0 review (OS-PERFFX section 8, item 14): does a project load clear the layer? It does, by the ordinary road: load_slots lets every pad go and forgets which tracks are
// affected, and the next apply() sends the end of each FX the bank had told the sequencer to hold, so the control table sends a ModEnd that reaches the voices (audio_perform_test: the slot lets go).
TEST_CASE("perform P1: a project load ends BITDEPTH and OVERDRV held on the tracks - the bank sends the ends, the control table lets go") {
  seq::PerformBank b;
  Log log;
  o3os::model::PerfState st = b.state();
  st.fx[2] = static_cast<std::uint8_t>(PerfFx::BitDepth);
  st.fx[5] = static_cast<std::uint8_t>(PerfFx::Overdrive);
  st.value[2][0] = 9;
  st.value[5][0] = 45;
  b.load_slots(st);
  b.toggle_track(0, log);
  b.toggle_track(7, log);
  b.pad_down(pad(2, 1), log);
  b.pad_down(pad(5, 1), log);
  b.pad_up(pad(2, 1), log);  // latched: they stay on after the release
  b.pad_up(pad(5, 1), log);
  Engine e;
  e.take(log, 0);
  for (const unsigned t : {0u, 7u}) {
    CHECK(e.ctl.perf_holds(static_cast<std::uint8_t>(t), FxGroup::BitDepth));
    CHECK(e.ctl.perf_holds(static_cast<std::uint8_t>(t), FxGroup::Overdrive));
  }
  // the next project's slots arrive (the same, or any)
  const std::size_t before = log.sent.size();
  b.load_slots(st);
  CHECK(b.affected_mask() == 0);
  CHECK(log.sent.size() == before);  // load_slots itself sends nothing: poll_perform's apply() does
  CHECK(b.apply(log));
  CHECK(log.sent.size() == before + 4u);  // two FX on two tracks
  for (std::size_t i = before; i < log.sent.size(); ++i) {
    CHECK(log.sent[i].kind == Command::Kind::PerfControl);
    CHECK(log.sent[i].c == (1u << 16));
  }
  e.take(log, before);
  for (const unsigned t : {0u, 7u}) {
    CHECK_FALSE(e.ctl.perf_holds(static_cast<std::uint8_t>(t), FxGroup::BitDepth));
    CHECK_FALSE(e.ctl.perf_holds(static_cast<std::uint8_t>(t), FxGroup::Overdrive));
  }
  CHECK(b.apply(log));
  CHECK(log.sent.size() == before + 4u);  // and once only
}

TEST_CASE("perform: a pad sets the slot's row value on the affected tracks only, and a release leaves it latched") {
  o3os::test::ZeroBank b;
  Log log;
  // slot 3 is the low-pass (our default order); give row 2 the value +40 (the edit state stores what the encoder makes)
  b.set_edit(true);
  b.pad_down(pad(3, 2), log);
  CHECK(log.sent.empty());  // no track is affected: nothing to send
  CHECK(b.row(3) == 2);
  b.turn(+40, log);
  CHECK(b.stored(3, 2) == 40);
  b.pad_up(pad(3, 2), log);
  b.set_edit(false);
  CHECK(log.sent.empty());
  // track 1 and 5 join: the latched FX goes to them
  b.toggle_track(1, log);
  NEED(log.sent.size() == 1);
  CHECK(log.sent[0].kind == Command::Kind::PerfControl);
  CHECK(log.sent[0].a == 1);
  CHECK(log.sent[0].b == static_cast<std::uint16_t>(FxGroup::Filter));
  CHECK(log.sent[0].c == static_cast<std::uint16_t>(audio::perf_filter_value(audio::kFilterLow, 40)));
  b.toggle_track(5, log);
  NEED(log.sent.size() == 2);
  CHECK(log.sent[1].a == 5);
  // a track that leaves has its FX ended
  b.toggle_track(1, log);
  NEED(log.sent.size() == 3);
  CHECK(log.sent[2].kind == Command::Kind::PerfControl);
  CHECK(log.sent[2].a == 1);
  CHECK(log.sent[2].c == (1u << 16));
  // row 0 of the slot is off; pressing row 2 again puts it back
  b.pad_down(pad(3, 0), log);
  b.pad_up(pad(3, 0), log);
  NEED(log.sent.size() == 4);
  CHECK(log.sent[3].a == 5);
  CHECK(log.sent[3].c == (1u << 16));
  CHECK_FALSE(b.active(3));
  b.pad_down(pad(3, 2), log);
  CHECK(b.active(3));
  CHECK(log.sent.size() == 5);
  b.pad_up(pad(3, 2), log);
  CHECK(b.active(3));  // latched after the release
  CHECK(log.sent.size() == 5);
  // a pad past the board is nothing
  CHECK_FALSE(b.pad_down(48, log));
  CHECK_FALSE(b.pad_up(99, log));
}

TEST_CASE("perform: the encoder moves the held slots' live value inside the FX's range; only EDIT keeps it; a release brings the stored value back") {
  o3os::test::ZeroBank b;
  Log log;
  b.toggle_track(0, log);
  b.set_edit(true);
  b.pad_down(pad(0, 1), log);  // slot 0 is Volume, range -100..100
  b.turn(+30, log);
  CHECK(b.live(0) == 30);
  CHECK(b.stored(0, 1) == 30);  // EDIT stored it
  b.turn(+500, log);
  CHECK(b.live(0) == 100);  // held to the range
  b.turn(-1000, log);
  CHECK(b.live(0) == -100);
  b.pad_up(pad(0, 1), log);
  b.set_edit(false);
  CHECK(b.live(0) == -100);  // stored, and the release reloads it
  b.pad_down(pad(0, 1), log);
  b.turn(+25, log);
  CHECK(b.live(0) == -75);
  CHECK(b.stored(0, 1) == -100);  // not EDIT: only live
  b.pad_up(pad(0, 1), log);
  CHECK(b.live(0) == -100);  // the release put the stored value back
  // a slot that is not held, or on row 0, ignores the encoder
  const std::size_t before = log.sent.size();
  b.turn(+5, log);
  CHECK(b.live(0) == -100);
  CHECK(log.sent.size() == before);
  b.pad_down(pad(0, 0), log);
  b.turn(+5, log);
  CHECK(b.live(0) == 0);
  b.pad_up(pad(0, 0), log);
}

TEST_CASE("perform: a slot with the same FX, and the filters, end each other; a value of 0 ends the FX") {
  o3os::test::ZeroBank b;
  Log log;
  b.toggle_track(2, log);
  b.set_edit(true);
  for (unsigned s = 3; s <= 5; ++s) {  // the three filters, slots 3, 4, 5, each with +20 on row 1
    b.pad_down(pad(s, 1), log);
    b.turn(+20, log);
    b.pad_up(pad(s, 1), log);
  }
  b.set_edit(false);
  // the last pad is the only one on: it ends what the FX blocks
  CHECK_FALSE(b.active(3));
  CHECK_FALSE(b.active(4));
  CHECK(b.active(5));
  Engine e;
  e.take(log, 0);
  CHECK(e.ctl.perf_holds(2, FxGroup::Filter));
  CHECK(audio::perf_filter_type(e.ctl.resolve(2, FxGroup::Filter).perf) == audio::kFilterBand);
  CHECK(audio::perf_filter_offset(e.ctl.resolve(2, FxGroup::Filter).perf) == 20);
  // pressing the low-pass slot again takes the group back from the band-pass in one set (no gap: the end of the band-pass is silent)
  log.sent.clear();
  b.pad_down(pad(3, 1), log);
  b.pad_up(pad(3, 1), log);
  NEED(log.sent.size() == 1);
  CHECK(log.sent[0].kind == Command::Kind::PerfControl);
  CHECK(log.sent[0].c != (1u << 16));
  e.take(log, 0);
  CHECK(audio::perf_filter_type(e.ctl.resolve(2, FxGroup::Filter).perf) == audio::kFilterLow);
  // a stored value of 0 is no FX
  b.pad_down(pad(3, 2), log);  // row 2 holds 0
  b.pad_up(pad(3, 2), log);
  e.take(log, 1);
  CHECK_FALSE(e.ctl.perf_holds(2, FxGroup::Filter));
  CHECK_FALSE(b.active(3));  // a value of 0 is not an active FX, though row 2 is selected
  CHECK(b.row(3) == 2);
  // two slots of one FX (a paste): the one that is pressed wins
  b.copy_slot(3);
  b.paste_slot(7, log);
  CHECK(b.fx(7) == PerfFx::LowPass);
  b.pad_down(pad(7, 1), log);
  b.pad_up(pad(7, 1), log);
  CHECK(b.active(7));
  CHECK_FALSE(b.active(3));
}

TEST_CASE("perform: a track that joins gets every active slot, the stutter, play mode and length reach the sequencer, and leaving ends it all") {
  o3os::test::ZeroBank b;
  Log log;
  b.set_edit(true);
  // the slots that do something in the sequencer: change slot 8, 9, 10 to the stutter, the play mode and the pattern length
  const auto make = [&](unsigned slot, PerfFx fx, int value) {
    unsigned guard = 0;
    while (b.fx(slot) != fx && guard++ < 40) {
      b.change_fx(slot, +1, log);
    }
    b.pad_down(pad(slot, 1), log);
    b.turn(value - b.stored(slot, 1), log);  // from the stored default to `value`
    b.pad_up(pad(slot, 1), log);
  };
  make(8, PerfFx::StepRepeater, 11);   // 1/4
  make(9, PerfFx::PatternPlayMode, 2); // ping-pong (value 2 is PING)
  make(10, PerfFx::PatternLength, 5);  // 16
  b.set_edit(false);
  CHECK(b.active(8));
  CHECK(b.active(9));
  CHECK(b.active(10));
  CHECK(log.sent.empty());  // no track yet
  b.toggle_track(3, log);
  Engine e;
  e.take(log, 0);
  CHECK(e.stutter[3] == 11);
  CHECK(e.mode[3] == 2);
  CHECK(e.length == 16);
  CHECK(e.stutter[2] == 0);
  b.toggle_track(0, log);
  e.take(log, 0);
  CHECK(e.stutter[0] == 11);
  b.turn_off(log);  // leaving
  e.take(log, 0);
  CHECK(e.stutter[3] == 0);
  CHECK(e.stutter[0] == 0);
  CHECK(e.mode[3] == 0);
  CHECK(e.length == 0);
  CHECK(b.affected_mask() == 0);
  CHECK(b.row(8) == 1);  // the rows stay selected
  // the play mode command leaves the track's source pattern and length to the song
  Command c{};
  CHECK(seq::perf_command(4, PerfFx::PatternPlayMode, 1, c));
  CHECK(c.kind == Command::Kind::PerfTrack);
  CHECK(c.a == 4);
  CHECK(c.b == 0);
  CHECK(c.c == (1u << 8));
  // OS-PERFFX P5: TUNE is a control-table group of its own now, -48..48 held to its range, 0 the end
  NEED(seq::perf_command(6, PerfFx::Tune, -12, c));
  CHECK(c.kind == Command::Kind::PerfControl);
  CHECK(c.a == 6);
  CHECK(c.b == static_cast<std::uint16_t>(FxGroup::Tune));
  CHECK(static_cast<std::int16_t>(c.c) == -12);
  NEED(seq::perf_command(6, PerfFx::Tune, 99, c));
  CHECK(static_cast<std::int16_t>(c.c) == 48);
  NEED(seq::perf_command(6, PerfFx::Tune, -99, c));
  CHECK(static_cast<std::int16_t>(c.c) == -48);
  NEED(seq::perf_command(6, PerfFx::Tune, 0, c));
  CHECK(c.c == (1u << 16));
  const seq::PerfFxInfo& tune = seq::perf_info(PerfFx::Tune);
  CHECK(tune.group == FxGroup::Tune);
  CHECK(tune.live);
  CHECK((tune.min == -48 && tune.max == 48));
  // OS-PERFFX P7: SMP POS and SMP END are control-table groups of their own (Position, SampleEnd), -100..100 held to their range, 0 the end
  for (const PerfFx fx : {PerfFx::SamplePosition, PerfFx::SampleEnd}) {
    const FxGroup g = fx == PerfFx::SamplePosition ? FxGroup::Position : FxGroup::SampleEnd;
    NEED(seq::perf_command(3, fx, -40, c));
    CHECK(c.kind == Command::Kind::PerfControl);
    CHECK(c.a == 3);
    CHECK(c.b == static_cast<std::uint16_t>(g));
    CHECK(static_cast<std::int16_t>(c.c) == -40);
    NEED(seq::perf_command(3, fx, 500, c));
    CHECK(static_cast<std::int16_t>(c.c) == 100);
    NEED(seq::perf_command(3, fx, -500, c));
    CHECK(static_cast<std::int16_t>(c.c) == -100);
    NEED(seq::perf_command(3, fx, 0, c));
    CHECK(c.c == (1u << 16));
    const seq::PerfFxInfo& info = seq::perf_info(fx);
    CHECK(info.group == g);
    CHECK(info.live);
    CHECK((info.min == -100 && info.max == 100));
  }
  // an FX with nothing to send
  CHECK_FALSE(seq::perf_command(0, PerfFx::None, 5, c));
  CHECK_FALSE(seq::perf_command(12, PerfFx::Volume, 5, c));  // a track past the twelve
}

TEST_CASE("perform: a full ring loses nothing - the refused commands go out at the next apply, none twice") {
  seq::PerformBank b;
  Log log;
  b.set_edit(true);
  for (unsigned s = 0; s < 3; ++s) {  // Volume, Panning and the low-pass... Tune has nothing to send, so slots 0, 1, 3
    const unsigned slot = s == 2 ? 3u : s;
    b.pad_down(pad(slot, 1), log);
    b.turn(+10, log);
    b.pad_up(pad(slot, 1), log);
  }
  b.set_edit(false);
  log.room = 2;
  for (unsigned t = 0; t < 4; ++t) {
    b.toggle_track(t, log);
  }
  CHECK(log.sent.size() == 2);          // the ring took two
  CHECK_FALSE(b.apply(log));            // and the bank knows it is behind
  log.room = 5;
  CHECK_FALSE(b.apply(log));
  CHECK(log.sent.size() == 7);
  log.room = 1000;
  CHECK(b.apply(log));
  CHECK(log.sent.size() == 12);         // 4 tracks x 3 FX, each exactly once
  CHECK(b.apply(log));
  CHECK(log.sent.size() == 12);         // and then nothing more
  // every (track, group) once
  unsigned seen[4][3] = {};
  for (const Command& c : log.sent) {
    NEED(c.kind == Command::Kind::PerfControl);
    NEED(c.a < 4);
    const unsigned k = c.b == static_cast<std::uint16_t>(FxGroup::Volume) ? 0u : (c.b == static_cast<std::uint16_t>(FxGroup::Panning) ? 1u : 2u);
    ++seen[c.a][k];
  }
  for (unsigned t = 0; t < 4; ++t) {
    for (unsigned k = 0; k < 3; ++k) {
      CHECK(seen[t][k] == 1);
    }
  }
}

TEST_CASE("perform: the edit operations - the arrows, the FX of a slot, swap, copy and paste") {
  seq::PerformBank b;
  Log log;
  CHECK(b.edit_slot() == 0);
  CHECK(b.move_edit(+3));
  CHECK(b.edit_slot() == 3);
  CHECK(b.move_edit(+100));  // clamped at the last
  CHECK(b.edit_slot() == 11);
  CHECK_FALSE(b.move_edit(+1));
  CHECK(b.move_edit(-100));
  CHECK(b.edit_slot() == 0);
  // the FX of a slot wraps through None; its values go to 0
  b.set_edit(true);
  b.pad_down(pad(0, 1), log);
  b.turn(+9, log);
  b.pad_up(pad(0, 1), log);
  CHECK(b.stored(0, 1) == -25 + 9);  // VOLUME's default -25, +9 detents
  CHECK(b.change_fx(0, -1, log));  // Volume - 1 = None
  CHECK(b.fx(0) == PerfFx::None);
  CHECK(b.stored(0, 1) == 0);
  CHECK(b.row(0) == 0);
  CHECK(b.change_fx(0, -1, log));  // wraps to the last (OVERDRV since OS-PERFFX P1); its defaults are 20, 50, 80 (OS-PRFDEFAULTS)
  CHECK(b.fx(0) == PerfFx::Overdrive);
  CHECK(b.stored(0, 1) == 20);
  CHECK(b.stored(0, 2) == 50);
  CHECK(b.stored(0, 3) == 80);
  CHECK(b.change_fx(0, +1, log));
  CHECK(b.fx(0) == PerfFx::None);
  // a slot with no FX has no row
  b.pad_down(pad(0, 2), log);
  CHECK(b.row(0) == 0);
  b.pad_up(pad(0, 2), log);
  // swap: the FX and the values, both slots off
  b.pad_down(pad(1, 1), log);
  b.turn(+7, log);   // PANNING's default -100, +7
  b.pad_up(pad(1, 1), log);
  b.pad_down(pad(2, 1), log);
  b.turn(+11, log);  // TUNE's default -12, +11
  b.pad_up(pad(2, 1), log);
  CHECK(b.swap_slots(1, 2, log));
  CHECK(b.fx(1) == PerfFx::Tune);
  CHECK(b.fx(2) == PerfFx::Panning);
  CHECK(b.stored(1, 1) == -1);
  CHECK(b.stored(2, 1) == -93);
  CHECK(b.row(1) == 0);
  CHECK(b.row(2) == 0);
  CHECK_FALSE(b.swap_slots(1, 1, log));
  CHECK_FALSE(b.swap_slots(1, 12, log));
  // copy and paste: the FX and the values, the row goes to 0
  b.copy_slot(2);
  CHECK(b.paste_slot(5, log));
  CHECK(b.fx(5) == PerfFx::Panning);
  CHECK(b.stored(5, 1) == -93);
  CHECK(b.row(5) == 0);
  seq::PerformBank fresh;
  CHECK_FALSE(fresh.paste_slot(0, log));  // nothing was copied
  CHECK_FALSE(fresh.paste_slot(12, log));
}

TEST_CASE("perform OS-PRFDEFAULTS: the table - every value is in its FX's range, a live FX has three distinct values that are not 0, an FX with no consumer keeps 0") {
  using o3os::model::kPerfDefaultValues;
  static_assert(sizeof kPerfDefaultValues / sizeof kPerfDefaultValues[0] == seq::kPerfFxCount, "one row of three per FX id");
  for (unsigned id = 0; id < seq::kPerfFxCount; ++id) {
    const seq::PerfFxInfo& info = seq::perf_info(static_cast<PerfFx>(id));
    INFO("FX ", id, " ", info.name);
    for (unsigned r = 0; r < 3; ++r) {
      CHECK(kPerfDefaultValues[id][r] >= info.min);
      CHECK(kPerfDefaultValues[id][r] <= info.max);
      if (info.live) {
        CHECK(kPerfDefaultValues[id][r] != 0);  // 0 would end the FX
        const int nonzero = info.max + (info.min < 0 ? -info.min : 0);  // PLAYBACK (0..1, OS-PERF-REVERSE) has one non-zero value: its three rows are all 1
        for (unsigned q = r + 1u; q < 3 && nonzero >= 3; ++q) {
          CHECK(kPerfDefaultValues[id][r] != kPerfDefaultValues[id][q]);
        }
      } else {
        CHECK(kPerfDefaultValues[id][r] == 0);  // a value that does nothing would light a slot of a dim FX
      }
    }
  }
}

TEST_CASE("perform OS-PRFDEFAULTS: changing a slot's FX in EDIT gives the new FX's three defaults, one operation, the row off and the engine told the end") {
  seq::PerformBank b;
  Log log;
  b.set_edit(true);
  b.toggle_track(0, log);
  b.pad_down(pad(0, 2), log);  // VOLUME on its row 2: -50 on track 0
  b.pad_up(pad(0, 2), log);
  CHECK(b.active(0));
  const std::size_t before = log.sent.size();
  CHECK(b.change_fx(0, +1, log));  // PANNING
  CHECK(b.fx(0) == PerfFx::Panning);
  CHECK(b.row(0) == 0);
  CHECK_FALSE(b.active(0));
  CHECK(b.stored(0, 0) == 0);
  CHECK(b.stored(0, 1) == -100);
  CHECK(b.stored(0, 2) == 100);
  CHECK(b.stored(0, 3) == -50);
  CHECK(log.sent.size() > before);  // the Volume layer on track 0 was ended
  // every FX of the table, up then down (both wrap): each id is reached once and gets its own three table values, the row off
  for (const int dir : {+1, -1}) {
    seq::PerformBank c;
    Log out;
    unsigned id = static_cast<unsigned>(c.fx(4));
    for (unsigned n = 0; n < seq::kPerfFxCount; ++n) {
      CHECK(c.change_fx(4, dir, out) == true);
      id = dir > 0 ? (id + 1u) % seq::kPerfFxCount : (id + seq::kPerfFxCount - 1u) % seq::kPerfFxCount;
      CHECK(static_cast<unsigned>(c.fx(4)) == id);
      CHECK(c.row(4) == 0);
      CHECK(c.stored(4, 0) == 0);
      for (unsigned r = 1; r < seq::kPerfRows; ++r) {
        CHECK(c.stored(4, r) == o3os::model::kPerfDefaultValues[id][r - 1u]);
      }
    }
    CHECK(static_cast<unsigned>(c.fx(4)) == 5u);  // a full turn is back where it began
  }
  // a pad of the new slot loads its default: LOWPASS row 1 is -30 on the track
  seq::PerformBank d;
  Log lg;
  d.toggle_track(1, lg);
  d.pad_down(pad(3, 1), lg);
  CHECK(d.live(3) == -30);
  CHECK(d.active(3));
  d.pad_up(pad(3, 1), lg);
  // a swap and a paste carry the values the slots hold (here an edited one), the defaults are not put over them
  d.set_edit(true);
  d.pad_down(pad(0, 3), lg);
  d.turn(+5, lg);  // VOLUME row 3: -100 + 5
  d.pad_up(pad(0, 3), lg);
  CHECK(d.stored(0, 3) == -95);
  d.copy_slot(0);
  CHECK(d.paste_slot(6, lg));
  CHECK(d.stored(6, 3) == -95);
  CHECK(d.swap_slots(0, 1, lg));
  CHECK(d.stored(1, 3) == -95);
  CHECK(d.stored(0, 3) == -50);  // PANNING's row 3 came over with it
}

TEST_CASE("perform OS-PRFDEFAULTS: a loaded project keeps its values, 0s included; a scene recall keeps what the scene holds") {
  seq::PerformBank b;
  Log log;
  o3os::model::PerfState zeros;
  o3os::model::init_perf_state(zeros);
  for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
    for (unsigned r = 0; r < o3os::model::kPerfStoredRows; ++r) {
      zeros.value[s][r] = 0;  // a project saved before the table
    }
  }
  zeros.value[3][1] = -77;  // and one value the user chose
  b.load_slots(zeros);
  CHECK(b.state() == zeros);
  CHECK(b.stored(3, 2) == -77);  // (the state's value[s][r] is the bank's row r + 1)
  CHECK(b.stored(3, 1) == 0);
  CHECK(b.stored(0, 1) == 0);
  // a bank that was loaded and then reset is the new project again
  b.reset();
  o3os::model::PerfState fresh;
  o3os::model::init_perf_state(fresh);
  CHECK(b.state() == fresh);
  // a scene comes back with the values it holds: a slot changed to another FX since gets its old FX and values back, the defaults are not put over them
  b.set_edit(true);
  b.pad_down(pad(0, 1), log);
  b.turn(+3, log);
  b.pad_up(pad(0, 1), log);
  CHECK(b.stored(0, 1) == -25 + 3);
  CHECK(b.store_scene(0));
  CHECK(b.change_fx(0, +1, log));
  CHECK(b.stored(0, 1) == -100);
  CHECK(b.recall_scene(0, log));
  CHECK(b.fx(0) == PerfFx::Volume);
  CHECK(b.stored(0, 1) == -25 + 3);
}

TEST_CASE("perform: a random walk over every operation keeps the engine exactly what the slots say (also through a ring that refuses at random)") {
  for (std::uint32_t seed = 1; seed <= 12; ++seed) {
    Rng rng(seed * 7919u);
    seq::PerformBank b;
    Log log;
    Engine e;
    std::size_t taken = 0;
    for (unsigned step = 0; step < 600; ++step) {
      log.room = rng.below(4) == 0u ? rng.below(4) : 1000u;  // sometimes the ring is nearly full
      switch (rng.below(10)) {
        case 0: case 1: b.pad_down(rng.below(seq::kPerfPads), log); break;
        case 2: case 3: b.pad_up(rng.below(seq::kPerfPads), log); break;
        case 4: b.turn(static_cast<int>(rng.below(21)) - 10, log); break;
        case 5: b.toggle_track(rng.below(seq::kPerfTracks), log); break;
        case 6: b.change_fx(rng.below(seq::kPerfSlots), rng.below(2) == 0u ? 1 : -1, log); break;
        case 7: b.swap_slots(rng.below(seq::kPerfSlots), rng.below(seq::kPerfSlots), log); break;
        case 8: b.set_edit(rng.below(2) == 0u); b.copy_slot(rng.below(seq::kPerfSlots)); b.paste_slot(rng.below(seq::kPerfSlots), log); break;
        default: if (rng.below(5) == 0u) { b.turn_off(log); } break;
      }
      e.take(log, taken);
      taken = log.sent.size();
      log.room = 1000;
      for (unsigned pass = 0; pass < 32u && !b.apply(log); ++pass) {  // the next passes of the App (16 commands each: with 12 tracks one pass may not hold it all)
      }
      e.take(log, taken);
      taken = log.sent.size();
      // the invariant: for every track and FX the engine holds what the slots want
      for (unsigned t = 0; t < seq::kPerfTracks; ++t) {
        int want_stutter = 0, want_mode = 0, want_length = 0;
        for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
          if (!b.affected(t) || !b.active(s)) {
            continue;
          }
          const PerfFx fx = b.fx(s);
          const int v = b.live(s);
          if (fx == PerfFx::StepRepeater) { want_stutter = v; }
          if (fx == PerfFx::PatternPlayMode) { want_mode = v; }
          if (fx == PerfFx::PatternLength && want_length == 0) { want_length = v; }
          const FxGroup g = seq::perf_info(fx).group;
          if (g != FxGroup::None) {
            NEED(e.ctl.perf_holds(static_cast<std::uint8_t>(t), g));
            const std::int16_t got = e.ctl.resolve(static_cast<std::uint8_t>(t), g).perf;
            const bool filter = fx == PerfFx::LowPass || fx == PerfFx::HighPass || fx == PerfFx::BandPass;
            const unsigned type = fx == PerfFx::LowPass ? 0u : (fx == PerfFx::HighPass ? 1u : 2u);
            CHECK(got == (filter ? audio::perf_filter_value(type, v) : static_cast<std::int16_t>(v)));
          }
        }
        CHECK(e.stutter[t] == want_stutter);
        CHECK(e.mode[t] == want_mode);
        // and no group is held that no slot wants
        for (unsigned g = 1; g < seq::kFxGroupCount; ++g) {
          bool wanted = false;
          for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
            wanted = wanted || (b.affected(t) && b.active(s) && static_cast<unsigned>(seq::perf_info(b.fx(s)).group) == g);
          }
          CHECK(e.ctl.perf_holds(static_cast<std::uint8_t>(t), static_cast<FxGroup>(g)) == wanted);
        }
      }
      // the pattern length: the first affected track's wish
      std::uint32_t want = 0;
      for (unsigned t = 0; t < seq::kPerfTracks && want == 0; ++t) {
        for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
          if (b.affected(t) && b.active(s) && b.fx(s) == PerfFx::PatternLength) {
            want = seq::perf_length_steps(b.live(s));
            break;
          }
        }
      }
      CHECK(e.length == want);
      // bounds
      for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
        CHECK(b.row(s) < seq::kPerfRows);
        if (b.row(s) != 0) {
          CHECK(b.fx(s) != PerfFx::None);
        }
        const seq::PerfFxInfo& i = seq::perf_info(b.fx(s));
        CHECK((b.live(s) >= i.min && b.live(s) <= i.max));
      }
    }
  }
}

// ---- OS-PERF2: scenes, latch, reset, step rate ---------------------------------------------------------------------------------------------------------

namespace {

// Slot 0 (Volume) row 1 = +40 and slot 3 (low-pass) row 2 = -25, both latched, on track 0.
void dress(seq::PerformBank& b, Log& log) {
  b.toggle_track(0, log);
  b.set_edit(true);
  b.pad_down(pad(0, 1), log);
  b.turn(+40, log);
  b.pad_up(pad(0, 1), log);
  b.pad_down(pad(3, 2), log);
  b.turn(-25, log);
  b.pad_up(pad(3, 2), log);
  b.set_edit(false);
}

}  // namespace

TEST_CASE("perform scenes: a stored scene recalls the slots' FX, rows and values, the engine is told the difference, and the affected tracks stay") {
  o3os::test::ZeroBank b;
  Log log;
  CHECK_FALSE(b.scene_stored(0));
  CHECK_FALSE(b.recall_scene(0, log));  // nothing stored: nothing happens
  CHECK(log.sent.empty());
  dress(b, log);
  Engine eng;
  std::size_t taken = 0;
  const auto sync = [&] {
    eng.take(log, taken);
    taken = log.sent.size();
  };
  sync();
  CHECK(eng.ctl.perf_holds(0, FxGroup::Volume));
  CHECK(eng.ctl.perf_holds(0, FxGroup::Filter));
  CHECK(b.store_scene(1));
  CHECK(b.scene_stored(1));
  CHECK(b.scene_matches(1));
  CHECK_FALSE(b.scene_matches(0));
  CHECK(b.scene(1).slots.fx[0] == static_cast<std::uint8_t>(PerfFx::Volume));
  CHECK(b.scene(1).slots.row[3] == 2);
  CHECK(b.scene(1).slots.value[3][1] == -25);
  // change the slots: everything off, a slot's FX another one, a value; the scene no longer matches
  b.all_off(log);
  b.change_fx(5, +1, log);
  b.set_edit(true);
  b.pad_down(pad(5, 3), log);
  b.turn(+7, log);
  b.pad_up(pad(5, 3), log);
  b.set_edit(false);
  sync();
  CHECK_FALSE(b.scene_matches(1));
  CHECK_FALSE(eng.ctl.perf_holds(0, FxGroup::Volume));
  const PerfFx fx5 = b.fx(5);
  // recall: the slots are the scene's again, the engine has the scene's FX on track 0 (the volume and the filter), what the scene did not have is off
  CHECK(b.recall_scene(1, log));
  sync();
  CHECK(b.scene_matches(1));
  CHECK(b.fx(5) != fx5);
  CHECK(b.row(0) == 1);
  CHECK(b.live(0) == 40);
  CHECK(b.row(3) == 2);
  CHECK(b.live(3) == -25);
  CHECK(b.row(5) == 0);
  CHECK(b.affected_mask() == 1u);  // the tracks are not the scene's
  CHECK(eng.ctl.perf_holds(0, FxGroup::Volume));
  CHECK(eng.ctl.perf_holds(0, FxGroup::Filter));
  CHECK(eng.ctl.resolve(0, FxGroup::Volume).perf == 40);
  // recalling what is already there tells the engine nothing
  const std::size_t n = log.sent.size();
  CHECK(b.recall_scene(1, log));
  CHECK(log.sent.size() == n);
}

TEST_CASE("perform scenes: three independent scenes; a stored scene survives changes and recalls twice; a project load clears them; the out-of-range ones are nothing") {
  o3os::test::ZeroBank b;
  Log log;
  dress(b, log);
  for (unsigned i = 0; i < seq::kPerfScenes; ++i) {
    CHECK(b.store_scene(i));
    b.turn_off(log);
    b.all_off(log);
    b.set_edit(true);
    b.pad_down(pad(1, 1), log);
    b.turn(static_cast<int>(10 * (i + 1)), log);  // slot 1 (Panning) row 1 = 10, 20, 30
    b.pad_up(pad(1, 1), log);
    b.set_edit(false);
  }
  // scene 0 was stored before the panning values: recalling it brings slot 1 back to 0
  b.toggle_track(0, log);
  CHECK(b.recall_scene(0, log));
  CHECK(b.stored(1, 1) == 0);
  CHECK(b.row(1) == 0);
  CHECK(b.recall_scene(0, log));  // again: nothing new to tell
  const std::size_t n = log.sent.size();
  CHECK(b.recall_scene(0, log));
  CHECK(log.sent.size() == n);
  CHECK(b.recall_scene(2, log));  // stored third: the panning value after the second round (the EDIT turns add up: 10, 30)
  CHECK(b.stored(1, 1) == 30);
  CHECK_FALSE(b.store_scene(seq::kPerfScenes));
  CHECK_FALSE(b.recall_scene(seq::kPerfScenes, log));
  CHECK_FALSE(b.scene_stored(7));
  const o3os::model::PerfState state = b.state();
  b.load_slots(state);
  for (unsigned i = 0; i < seq::kPerfScenes; ++i) {
    CHECK_FALSE(b.scene_stored(i));
  }
}

TEST_CASE("perform scenes: a scene is clamped like a loaded project (a value past its FX's range, an unknown FX, a row past 3), held pads stay held") {
  o3os::test::ZeroBank b;
  Log log;
  b.toggle_track(2, log);
  CHECK(b.store_scene(0));
  const_cast<seq::PerfScene&>(b.scene(0)).slots.fx[0] = 200;  // the test reaches into the plain data a later slice will load
  const_cast<seq::PerfScene&>(b.scene(0)).slots.fx[1] = static_cast<std::uint8_t>(PerfFx::Panning);
  const_cast<seq::PerfScene&>(b.scene(0)).slots.row[1] = 9;
  const_cast<seq::PerfScene&>(b.scene(0)).slots.value[2][0] = 30000;  // slot 2 is Tune (+-48)
  b.pad_down(pad(4, 1), log);
  CHECK(b.recall_scene(0, log));
  CHECK(b.fx(0) == PerfFx::None);
  CHECK(b.row(1) == 0);
  CHECK(b.stored(2, 1) == 48);
  CHECK(b.held(4));
  b.pad_up(pad(4, 1), log);
  CHECK_FALSE(b.held(4));
}

TEST_CASE("perform latch: off, a slot acts only while its pad is held; on is our default; switching leaves the slots as they are; a second pad of the slot keeps it") {
  o3os::test::ZeroBank b;
  Log log;
  CHECK(b.latch());
  b.toggle_track(0, log);
  b.set_edit(true);
  b.pad_down(pad(0, 1), log);
  b.turn(+50, log);
  b.pad_up(pad(0, 1), log);
  b.pad_down(pad(0, 2), log);
  b.turn(-50, log);
  b.pad_up(pad(0, 2), log);
  b.set_edit(false);
  b.pad_down(pad(0, 1), log);
  b.pad_up(pad(0, 1), log);
  CHECK(b.active(0));  // latched
  b.set_latch(false);
  CHECK(b.active(0));  // switching does not touch it
  Engine eng;
  eng.take(log, 0);
  CHECK(eng.ctl.perf_holds(0, FxGroup::Volume));
  const std::size_t n = log.sent.size();
  b.pad_down(pad(0, 2), log);
  CHECK(b.active(0));
  CHECK(b.live(0) == -50);
  b.pad_up(pad(0, 2), log);
  CHECK_FALSE(b.active(0));  // let go: it ends
  CHECK(b.row(0) == 0);
  NEED(log.sent.size() > n);
  CHECK(log.sent.back().c == (1u << 16));  // the end
  // the encoder works on the held pad, and ends with it
  b.pad_down(pad(0, 1), log);
  b.turn(+10, log);
  CHECK(b.live(0) == 60);
  b.pad_up(pad(0, 1), log);
  CHECK_FALSE(b.active(0));
  // two pads of one slot: the second takes the row; the release of the FIRST (its row is no longer the slot's) does not end the second's FX
  b.pad_down(pad(0, 1), log);
  b.pad_down(pad(0, 2), log);
  b.pad_up(pad(0, 1), log);
  CHECK(b.active(0));
  CHECK(b.row(0) == 2);
  b.pad_up(pad(0, 2), log);
  CHECK_FALSE(b.active(0));
  // row 0 pressed with the latch off is the same as ever
  b.pad_down(pad(0, 0), log);
  b.pad_up(pad(0, 0), log);
  CHECK(b.row(0) == 0);
  // switching it on again restores the default behaviour
  b.set_latch(true);
  b.pad_down(pad(0, 1), log);
  b.pad_up(pad(0, 1), log);
  CHECK(b.active(0));
}

TEST_CASE("perform latch: a second tap of the same pad keeps a latched row (by design, QA-PRF-190); only another row, row 0 or RESET ends it; with LATCH off the release ended it already") {
  o3os::test::ZeroBank b;
  Log log;
  b.toggle_track(0, log);
  b.set_edit(true);
  b.pad_down(pad(0, 1), log);
  b.turn(+50, log);
  b.pad_up(pad(0, 1), log);
  b.set_edit(false);
  CHECK(b.active(0));  // LATCH on (the default): the stored value stays on after the release
  b.pad_down(pad(0, 1), log);  // the second tap of the same pad: the row is the slot's already, nothing is released
  CHECK(b.active(0));
  CHECK(b.row(0) == 1);
  b.pad_up(pad(0, 1), log);
  CHECK(b.active(0));
  CHECK(b.row(0) == 1);
  CHECK(b.live(0) == 50);
  b.pad_down(pad(0, 0), log);  // row 0 ends it
  b.pad_up(pad(0, 0), log);
  CHECK_FALSE(b.active(0));
  b.pad_down(pad(0, 1), log);  // latched again, RESET ends it
  b.pad_up(pad(0, 1), log);
  CHECK(b.active(0));
  CHECK(b.all_off(log));
  CHECK_FALSE(b.active(0));
  b.set_latch(false);  // LATCH off: the release of the first tap ended it, there is nothing for a second tap to release
  b.pad_down(pad(0, 1), log);
  b.pad_up(pad(0, 1), log);
  CHECK_FALSE(b.active(0));
  b.pad_down(pad(0, 1), log);
  b.pad_up(pad(0, 1), log);
  CHECK_FALSE(b.active(0));
}

TEST_CASE("perform reset: every slot off at once, the affected tracks, the slots' FX and values and the held rate stay; the engine ends the FX") {
  o3os::test::ZeroBank b;
  Log log;
  dress(b, log);
  b.toggle_track(4, log);
  Engine eng;
  eng.take(log, 0);
  CHECK(eng.ctl.perf_holds(0, FxGroup::Volume));
  CHECK(eng.ctl.perf_holds(4, FxGroup::Filter));
  b.pad_down(pad(7, 1), log);  // a held pad too
  const unsigned fx_before = static_cast<unsigned>(b.fx(3));
  CHECK(b.all_off(log));
  eng.take(log, 0);
  for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
    CHECK(b.row(s) == 0);
    CHECK_FALSE(b.active(s));
    CHECK_FALSE(b.held(s));
  }
  CHECK(b.affected(0));
  CHECK(b.affected(4));
  CHECK(static_cast<unsigned>(b.fx(3)) == fx_before);
  CHECK(b.stored(3, 2) == -25);
  CHECK_FALSE(eng.ctl.perf_holds(0, FxGroup::Volume));
  CHECK_FALSE(eng.ctl.perf_holds(4, FxGroup::Filter));
  CHECK_FALSE(b.all_off(log));  // nothing was on: no change to show
  b.pad_up(pad(7, 1), log);     // the late release of the pad that was held is harmless
  CHECK(b.row(7) == 0);
  b.set_rate(2);
  CHECK_FALSE(b.all_off(log));  // a rate is not a slot change (apply sent it all the same)
}

TEST_CASE("perform rate: the bank sends PerfRate once when the wanted rate changes, retries a refused one, and says nothing when it is unchanged") {
  seq::PerformBank b;
  Log log;
  CHECK(b.rate() == 0);
  CHECK(b.apply(log));
  CHECK(log.sent.empty());
  b.set_rate(1);
  CHECK(b.rate() == 1);
  log.room = 0;
  CHECK_FALSE(b.apply(log));  // the ring is full
  log.room = 10;
  CHECK(b.apply(log));
  NEED(log.sent.size() == 1);
  CHECK(log.sent[0].kind == Command::Kind::PerfRate);
  CHECK(log.sent[0].a == 1);
  CHECK(b.apply(log));
  CHECK(log.sent.size() == 1);  // once
  b.set_rate(2);
  CHECK(b.apply(log));
  b.set_rate(0);
  CHECK(b.apply(log));
  b.set_rate(9);  // not a rate: the normal one, already sent
  CHECK(b.apply(log));
  NEED(log.sent.size() == 3);
  CHECK(log.sent[1].a == 2);
  CHECK(log.sent[2].a == 0);
  // the rate goes first, before the FX commands of the same apply
  seq::PerformBank c;
  Log l2;
  c.toggle_track(0, l2);
  c.pad_down(pad(0, 1), l2);
  c.set_edit(true);
  c.turn(+5, l2);
  c.set_edit(false);
  c.set_rate(1);
  c.pad_up(pad(0, 1), l2);
  const std::size_t n = l2.sent.size();
  c.set_rate(2);
  c.toggle_track(0, l2);
  c.toggle_track(0, l2);
  NEED(l2.sent.size() > n);
  CHECK(l2.sent[n].kind == Command::Kind::PerfRate);
}
