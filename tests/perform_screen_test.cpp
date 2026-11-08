// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011b: the Perform screen on the App - the keys and pads of the performance mode (the rules and the slot bank are seq_perform_test.cpp, the voices audio_perform_test.cpp),
// what reaches the sequencer's ring, what leaving the screen does, the pads' LEDs and the screen's pictures (the gallery's `perform` states).
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/app.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/screens/perform_screen.h>
#include <o3os/audio/events.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::PadRole;
using app::ScreenId;
using seq::Command;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

struct Log final : seq::CommandSink {
  std::vector<Command> sent;
  bool open = true;
  bool push(const Command& c) noexcept override {
    if (!open) {
      return false;
    }
    sent.push_back(c);
    return true;
  }
};

struct TransportLog final : app::TransportSink {
  unsigned jumps = 0;
  unsigned jump_row = 0;
  bool accept_jumps = true;
  bool queue_jump(unsigned row) noexcept override {
    if (!accept_jumps) {
      return false;
    }
    ++jumps;
    jump_row = row;
    return true;
  }
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::PerformBoard> board = std::make_unique<app::PerformBoard>();
  Log log;

  // `zeros`: the slots' row values are 0, as the project of a build before OS-PRFDEFAULTS had them (most tests pick their own values from 0); false keeps the new project's defaults.
  explicit Rig(bool zeros = true) {
    model::Project& p = test::arena(1).project;
    sim::fill_demo_project(p);
    if (zeros) {
      model::PerfState s = p.perf();
      for (auto& row : s.value) {
        for (std::int16_t& v : row) {
          v = 0;
        }
      }
      (void)p.set_perf(s);
    }
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    board->out = &log;
    app->attach_perform(board.get());
    if (zeros) {  // the App's bank holds the defaults of its own (PerformBank::reset): put the zeros in as a load would
      model::PerfState s = bank().state();
      for (auto& row : s.value) {
        for (std::int16_t& v : row) {
          v = 0;
        }
      }
      bank().load_slots(s);
    }
    tap(Button::Performance);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    release(b);
    pass();
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  // A track joins or leaves the layer: a lone Shift hold turns the pads into the TRACKS layer (OS-PADS-A), whose first row is AFFECTED.
  void affect(unsigned track) {
    press(Button::Shift);
    pass(1);
    pad_down(track);
    pad_up(track);
    release(Button::Shift);
    pass(1);
  }
  void pad_down(unsigned n) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 127));
    pass();
  }
  void pad_up(unsigned n) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 0));
    pass();
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  void flush() {  // the notification on screen runs out (the next one is shown)
    pass(1);
    pass(10000);
  }
  seq::PerformBank& bank() { return board->bank; }
  const model::Project& project() const { return test::arena(1).project; }
};

constexpr unsigned pad_of(unsigned slot, unsigned row) { return row * seq::kPerfSlots + slot; }

}  // namespace

TEST_CASE("perform screen: the Performance button opens it, the registry names its context, and the pads are the slots' four rows") {
  Rig rig;
  CHECK(rig.app->state().screen == ScreenId::Perform);
  CHECK(app::kPerformScreen.context() == app::Context::Perform);
  CHECK(rig.app->failed_frames() == 0);
  // the pad roles: the selected row of every slot that has an FX, and that is row 0 until a pad says otherwise
  app::PadRole roles[48];
  CHECK(rig.app->pad_lights(roles));
  for (unsigned p = 0; p < 48; ++p) {
    CHECK(roles[p] == (p < seq::kPerfSlots ? PadRole::Back : PadRole::Off));
  }
  // a pad held is brighter, the slot's row 0 goes dark, and after the release the row stays lit at the back (latched)
  rig.tap(Button::Rec);  // EDIT, so that the encoder stores a value
  rig.pad_down(pad_of(4, 2));
  CHECK(rig.app->pad_lights(roles));
  CHECK(roles[pad_of(4, 2)] == PadRole::Front);
  CHECK(roles[pad_of(4, 0)] == PadRole::Off);
  CHECK(roles[pad_of(5, 0)] == PadRole::Back);
  rig.pad_up(pad_of(4, 2));
  CHECK(rig.app->pad_lights(roles));
  CHECK(roles[pad_of(4, 2)] == PadRole::Back);
  CHECK_FALSE(rig.bank().held(4));
  // a slot with no FX lights nothing
  rig.tap(Button::Right);
  for (unsigned i = 0; i < 4; ++i) {
    rig.tap(Button::Right);
  }
  rig.tap(Button::Down);
  CHECK(rig.bank().edit_slot() == 5);
  while (rig.bank().fx(5) != seq::PerfFx::None) {
    rig.tap(Button::Down);
  }
  CHECK(rig.app->pad_lights(roles));
  CHECK(roles[pad_of(5, 0)] == PadRole::Off);
}

TEST_CASE("perform screen OS-PRFDEFAULTS: a new project's pad sends the default value, and an FX changed in EDIT sends its own") {
  Rig rig(false);
  rig.affect(1);  // track 2 joins; nothing is on
  rig.log.sent.clear();
  rig.pad_down(pad_of(3, 1));  // the low-pass, row 1: -30
  rig.pad_up(pad_of(3, 1));
  NEED(!rig.log.sent.empty());
  CHECK(rig.log.sent[0].b == static_cast<std::uint16_t>(seq::FxGroup::Filter));
  CHECK(rig.log.sent[0].c == static_cast<std::uint16_t>(audio::perf_filter_value(audio::kFilterLow, -30)));
  CHECK(rig.bank().live(3) == -30);
  // Rec, the edit place on slot 3, Up: the high-pass takes over the slot with +30, +60, +90 and the slot is off
  rig.tap(Button::Rec);
  for (unsigned i = 0; i < 3; ++i) {
    rig.tap(Button::Right);
  }
  rig.tap(Button::Up);
  CHECK(rig.bank().fx(3) == seq::PerfFx::HighPass);
  CHECK(rig.bank().row(3) == 0);
  CHECK(rig.bank().stored(3, 1) == 30);
  CHECK(rig.bank().stored(3, 2) == 60);
  CHECK(rig.bank().stored(3, 3) == 90);
}

TEST_CASE("perform screen: a latched FX goes to the tracks that join, a track that leaves has it ended, row 0 ends it") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.pad_down(pad_of(3, 2));  // slot 3 is the low-pass; the encoder stores +40 in row 2
  rig.turn(+40);
  rig.pad_up(pad_of(3, 2));
  rig.tap(Button::Rec);
  CHECK(rig.log.sent.empty());  // no track is affected yet
  rig.affect(1);                // track 2 joins
  NEED(rig.log.sent.size() == 1);
  CHECK(rig.log.sent[0].kind == Command::Kind::PerfControl);
  CHECK(rig.log.sent[0].a == 1);
  CHECK(rig.log.sent[0].b == static_cast<std::uint16_t>(seq::FxGroup::Filter));
  CHECK(rig.log.sent[0].c == static_cast<std::uint16_t>(audio::perf_filter_value(audio::kFilterLow, 40)));
  rig.affect(1);  // and leaves
  NEED(rig.log.sent.size() == 2);
  CHECK(rig.log.sent[1].c == (1u << 16));
  rig.affect(1);
  rig.pad_down(pad_of(3, 0));  // row 0: off
  rig.pad_up(pad_of(3, 0));
  CHECK(rig.log.sent.back().c == (1u << 16));
  CHECK_FALSE(rig.bank().active(3));
}

TEST_CASE("perform screen: the encoder changes the held value, and a release brings the stored one back") {
  Rig rig;
  rig.affect(0);
  rig.tap(Button::Rec);
  rig.pad_down(pad_of(0, 1));  // Volume
  rig.turn(+30);
  rig.pad_up(pad_of(0, 1));
  rig.tap(Button::Rec);
  CHECK(rig.bank().stored(0, 1) == 30);
  const std::size_t before = rig.log.sent.size();
  rig.pad_down(pad_of(0, 1));
  rig.turn(-10);
  CHECK(rig.bank().live(0) == 20);
  CHECK(rig.log.sent.size() > before);  // the engine hears the live value
  CHECK(rig.log.sent.back().c == 20u);
  rig.pad_up(pad_of(0, 1));
  CHECK(rig.bank().live(0) == 30);
  CHECK(rig.log.sent.back().c == 30u);
  // the encoder with no pad held changes nothing
  const std::size_t after = rig.log.sent.size();
  rig.turn(+5);
  CHECK(rig.log.sent.size() == after);
}

TEST_CASE("perform screen: the EDIT state's arrows, FX, swap and copy, and only in EDIT") {
  Rig rig;
  rig.tap(Button::Right);  // not in EDIT: nothing
  CHECK(rig.bank().edit_slot() == 0);
  rig.tap(Button::Up);
  CHECK(rig.bank().fx(0) == seq::PerfFx::Volume);
  rig.tap(Button::Rec);
  CHECK(rig.bank().edit());
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  CHECK(rig.bank().edit_slot() == 2);
  rig.tap(Button::Up);  // Tune -> low-pass
  CHECK(rig.bank().fx(2) == seq::PerfFx::LowPass);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // back, and one more: Panning
  CHECK(rig.bank().fx(2) == seq::PerfFx::Panning);
  rig.shift_tap(Button::Left);  // swap with slot 1 (Panning): the place goes with it
  CHECK(rig.bank().edit_slot() == 1);
  CHECK(rig.bank().fx(1) == seq::PerfFx::Panning);
  CHECK(rig.bank().fx(2) == seq::PerfFx::Panning);  // slot 1 had Panning too (the default's 2nd) and slot 2 was changed to it: they swapped like for like
  rig.tap(Button::Copy);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  rig.shift_tap(Button::Copy);  // paste at slot 3
  CHECK(rig.bank().fx(3) == seq::PerfFx::Panning);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  CHECK(rig.bank().edit_slot() == 0);
  rig.shift_tap(Button::Left);  // swapping past the first slot does nothing
  CHECK(rig.bank().edit_slot() == 0);
  rig.tap(Button::Rec);
  CHECK_FALSE(rig.bank().edit());
}

TEST_CASE("perform screen: a lone Shift hold is the TRACKS layer: row 1 AFFECTED, row 2 mute, row 4 solo; F1..F8 no longer join tracks") {
  Rig rig;
  const core::ParamId mute = core::param_id(core::ParamKind::Track, 2, model::track_param::Mute);
  const core::ParamId solo = core::param_id(core::ParamKind::Track, 4, model::track_param::Solo);
  CHECK(rig.project().get_param(mute) == 0.0f);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.pad_down(12 + 2);
  rig.pad_up(12 + 2);
  CHECK(rig.project().get_param(mute) == 1.0f);
  CHECK(rig.bank().affected_mask() == 0);  // it did not make the track join the layer
  rig.pad_down(36 + 4);
  rig.pad_up(36 + 4);
  CHECK(rig.project().get_param(solo) == 1.0f);
  rig.pad_down(5);  // AFFECTED: track 6 joins
  rig.pad_up(5);
  CHECK(rig.bank().affected_mask() == (1u << 5));
  rig.pad_down(11);  // the twelfth track can be affected too (kPerfTracks is 12)
  rig.pad_up(11);
  CHECK(rig.bank().affected(11));
  PadRole roles[48];
  CHECK(rig.app->pad_lights(roles));
  CHECK(roles[5] == PadRole::Front);
  CHECK(roles[11] == PadRole::Front);
  CHECK(roles[6] == PadRole::Off);
  CHECK(roles[12 + 2] == PadRole::Front);
  CHECK(roles[36 + 4] == PadRole::Front);
  CHECK(roles[12 + 3] == PadRole::Back);  // silenced by track 5's solo
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.project().get_param(mute) == 1.0f);
  CHECK(rig.app->pad_lights(roles));  // Perform's own lights again: the selected row of each slot (row 0)
  CHECK(roles[5] == PadRole::Back);
  CHECK(roles[12 + 2] == PadRole::Off);
  // F1..F8 no longer make a track join, and Shift + F1..F8 no longer mute one (they are OS-PERF2's: scenes, rate, latch, next pattern, reset)
  for (const Button k : {Button::F1, Button::F2, Button::F3, Button::F8}) {
    rig.tap(k);
    rig.shift_tap(k);
  }
  CHECK(rig.bank().affected_mask() == ((1u << 5) | (1u << 11)));
  CHECK(rig.project().get_param(core::param_id(core::ParamKind::Track, 0, model::track_param::Mute)) == 0.0f);
  rig.affect(5);
  CHECK(rig.bank().affected_mask() == (1u << 11));  // and leaves
}

TEST_CASE("perform screen: a pad the TRACKS layer took does not start a slot, and its release does not let go of one") {
  Rig rig;
  rig.pad_down(pad_of(3, 2));  // slot 3, row 2: held
  CHECK(rig.bank().held(3));
  rig.press(Button::Shift);
  rig.pass(1);
  rig.pad_down(12 + 3);  // the same column, the MUTE row: the layer's
  CHECK(rig.bank().row(3) == 2);
  rig.pad_up(12 + 3);
  CHECK(rig.bank().held(3));  // not let go
  rig.release(Button::Shift);
  rig.pass(1);
  rig.pad_up(pad_of(3, 2));
  CHECK_FALSE(rig.bank().held(3));
  // a slot pad pressed before the layer and let go under it still lets go (the press was the slot's)
  rig.pad_down(pad_of(4, 1));
  rig.press(Button::Shift);
  rig.pass(1);
  rig.pad_up(pad_of(4, 1));
  CHECK_FALSE(rig.bank().held(4));
  rig.release(Button::Shift);
  rig.pass(1);
}

TEST_CASE("perform screen: leaving ends every FX, except for the Master and Song screens, which keep the mode") {
  Rig rig;
  rig.affect(0);
  rig.tap(Button::Rec);
  rig.pad_down(pad_of(0, 1));
  rig.turn(+30);
  rig.pad_up(pad_of(0, 1));
  rig.tap(Button::Rec);
  NEED(!rig.log.sent.empty());
  CHECK(rig.log.sent.back().c == 30u);
  const std::size_t n = rig.log.sent.size();
  rig.tap(Button::Master);  // the Mixer keeps the FX
  CHECK(rig.app->state().screen == ScreenId::Mixer);
  CHECK(rig.log.sent.size() == n);
  CHECK(rig.bank().affected(0));
  rig.tap(Button::Song);  // so does Song
  CHECK(rig.log.sent.size() == n);
  rig.tap(Button::Pattern);  // anywhere else ends them
  CHECK(rig.app->state().screen == ScreenId::Pattern);
  NEED(rig.log.sent.size() == n + 1);
  CHECK(rig.log.sent.back().c == (1u << 16));
  CHECK_FALSE(rig.bank().affected(0));
  CHECK(rig.bank().row(0) == 1);  // the row stays selected
  // back in: nothing is on until a track joins again
  rig.tap(Button::Performance);
  CHECK(rig.log.sent.size() == n + 1);
  rig.affect(0);
  CHECK(rig.log.sent.size() == n + 2);
  CHECK(rig.log.sent.back().c == 30u);
}

TEST_CASE("perform screen: a ring that refuses is retried every pass, and a pad release on Master or Song still lets go") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.pad_down(pad_of(0, 1));
  rig.turn(+30);
  rig.pad_up(pad_of(0, 1));
  rig.tap(Button::Rec);
  rig.log.open = false;
  rig.affect(0);
  CHECK(rig.log.sent.empty());
  rig.pass();
  CHECK(rig.log.sent.empty());
  rig.log.open = true;
  rig.pass();
  NEED(rig.log.sent.size() == 1);  // told at the next pass, once
  CHECK(rig.log.sent[0].c == 30u);
  rig.pass();
  CHECK(rig.log.sent.size() == 1);
  // a pad held while the screen changes: its release still reaches the bank
  rig.pad_down(pad_of(0, 1));
  CHECK(rig.bank().held(0));
  rig.tap(Button::Master);
  rig.pad_up(pad_of(0, 1));  // the release arrives on the Mixer
  CHECK(rig.app->state().screen == ScreenId::Mixer);
  CHECK_FALSE(rig.bank().held(0));  // OS-QAFIX-E-PERF-08: Master and Song keep the mode, so the release is forwarded there: the slot is let go at once
  // the same on Song
  rig.tap(Button::Performance);
  rig.pad_down(pad_of(0, 1));
  CHECK(rig.bank().held(0));
  rig.tap(Button::Song);
  rig.pad_up(pad_of(0, 1));
  CHECK(rig.app->state().screen == ScreenId::Song);
  CHECK_FALSE(rig.bank().held(0));
  rig.tap(Button::Performance);
  CHECK_FALSE(rig.bank().held(0));  // entering the screen puts the holds down
}

TEST_CASE("perform screen: on Master another pad of a held slot is not the bank's, so its release cannot strand the FX of the pad that is (OS-QAFIX-E-PERF-08)") {
  Rig rig;
  rig.affect(0);
  const unsigned a = pad_of(0, 1);
  const unsigned b = pad_of(0, 2);  // the same slot, another row
  rig.pad_down(a);
  rig.turn(+30);
  CHECK(rig.bank().active(0));
  const std::size_t n = rig.log.sent.size();
  rig.tap(Button::Master);
  CHECK(rig.app->state().screen == ScreenId::Mixer);
  rig.pad_down(b);  // not sent to Perform
  CHECK(rig.bank().row(0) == 1);
  rig.pad_up(b);  // so its release is not either: the slot stays held by A
  CHECK(rig.bank().held(0));
  CHECK(rig.log.sent.size() == n);
  rig.pad_up(a);  // A's release lets the slot go, and the FX ends with it: LATCH is on (the default, QA-PRF-025), but the +30 was only live, and the release reloads the row's stored 0
  CHECK_FALSE(rig.bank().held(0));
  CHECK_FALSE(rig.bank().active(0));
  NEED(rig.log.sent.size() == n + 1);
  CHECK(rig.log.sent.back().c == (1u << 16));
  // a pad that was never down on Perform is not forwarded either
  rig.pad_up(a);
  CHECK(rig.log.sent.size() == n + 1);
}

TEST_CASE("perform screen: without a board it shows the screen and does nothing") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  auto a = std::make_unique<app::App>(input, clock, display);
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Performance), true));
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Performance), false));
  clock.advance(16 * core::kTicksPerMs);
  a->run_once();
  CHECK(a->state().screen == ScreenId::Perform);
  app::PadRole roles[48];
  CHECK_FALSE(a->pad_lights(roles));
  input.push(core::make_pad_event(clock.now(), 5, 127));
  input.push(core::make_pad_event(clock.now(), 5, 0));
  clock.advance(16 * core::kTicksPerMs);
  a->run_once();
  CHECK(a->failed_frames() == 0);
}

// ---- OS-PERF2: F1..F8 ---------------------------------------------------------------------------------------------------------------------------------

namespace {

unsigned count_rate(const Log& log, std::uint8_t rate) {
  unsigned n = 0;
  for (const Command& c : log.sent) {
    n += c.kind == Command::Kind::PerfRate && c.a == rate ? 1u : 0u;
  }
  return n;
}

}  // namespace

TEST_CASE("perform keys: F4 is half time and F5 double time while held, both cancel, the release brings the song's rate back, a screen change forgets a held key") {
  Rig rig;
  rig.press(Button::F4);
  rig.pass();
  CHECK(rig.bank().rate() == 1);
  NEED(!rig.log.sent.empty());
  CHECK(rig.log.sent.back().kind == Command::Kind::PerfRate);
  CHECK(rig.log.sent.back().a == 1);
  rig.press(Button::F5);  // both: the two cancel
  rig.pass();
  CHECK(rig.bank().rate() == 0);
  CHECK(rig.log.sent.back().a == 0);
  rig.release(Button::F4);  // F5 alone: double
  rig.pass();
  CHECK(rig.bank().rate() == 2);
  CHECK(rig.log.sent.back().a == 2);
  rig.release(Button::F5);
  rig.pass();
  CHECK(rig.bank().rate() == 0);
  CHECK(rig.log.sent.back().a == 0);
  const std::size_t n = rig.log.sent.size();
  rig.tap(Button::F4);  // a plain tap: half time for the instant it is down, then back
  CHECK(rig.log.sent.size() >= n + 2);
  CHECK(rig.bank().rate() == 0);
  // Shift + F4 is not the key
  rig.press(Button::Shift);
  rig.pass(1);
  rig.press(Button::F4);
  rig.pass();
  CHECK(rig.bank().rate() == 0);
  rig.release(Button::F4);
  rig.release(Button::Shift);
  rig.pass();
  CHECK(rig.bank().rate() == 0);
  // F4 held while the screen changes: the rate is the song's again at once, and the late release finds nothing
  rig.press(Button::F4);
  rig.pass();
  CHECK(rig.bank().rate() == 1);
  rig.tap(Button::Song);
  CHECK(rig.app->state().screen == ScreenId::Song);
  CHECK(rig.bank().rate() == 0);
  CHECK(rig.log.sent.back().kind == Command::Kind::PerfRate);
  CHECK(rig.log.sent.back().a == 0);
  const std::size_t m = rig.log.sent.size();
  rig.release(Button::F4);
  rig.pass();
  CHECK(rig.log.sent.size() == m);
  CHECK(count_rate(rig.log, 1) >= 2);
}

TEST_CASE("perform keys: a scene key taps to recall and holds a second to store; an empty scene says so; the release of a store recalls nothing") {
  Rig rig;
  rig.tap(Button::F1);
  CHECK(std::string(rig.app->message()) == "SCENE IS EMPTY: HOLD THE KEY TO STORE");
  rig.flush();
  rig.affect(0);
  rig.pad_down(pad_of(0, 2));  // slot 0 (volume), row 2, latched; the value is 0 so it is "on" only once it has one: store it with the encoder in EDIT
  rig.pad_up(pad_of(0, 2));
  rig.tap(Button::Rec);
  rig.pad_down(pad_of(0, 2));
  rig.turn(+30);
  rig.pad_up(pad_of(0, 2));
  rig.tap(Button::Rec);
  CHECK(rig.bank().stored(0, 2) == 30);
  CHECK(rig.bank().row(0) == 2);
  // a hold of less than a second is a tap (and the scene is empty)
  rig.press(Button::F1);
  rig.pass(1);
  rig.pass(900);
  CHECK_FALSE(rig.bank().scene_stored(0));
  rig.release(Button::F1);
  rig.pass();
  CHECK_FALSE(rig.bank().scene_stored(0));
  rig.flush();  // (the empty scene's message)
  // a second: stored, announced; the key goes on being held
  rig.press(Button::F1);
  rig.pass(1);
  rig.pass(500);
  CHECK_FALSE(rig.bank().scene_stored(0));
  rig.pass(600);
  CHECK(rig.bank().scene_stored(0));
  CHECK(std::string(rig.app->message()) == "SCENE 1 STORED");
  rig.flush();
  CHECK(rig.bank().scene_matches(0));
  // the slots change under the held key, and its release does not recall (the hold stored)
  rig.pad_down(pad_of(0, 0));
  rig.pad_up(pad_of(0, 0));
  CHECK(rig.bank().row(0) == 0);
  rig.release(Button::F1);
  rig.pass();
  CHECK(rig.bank().row(0) == 0);
  CHECK_FALSE(rig.bank().scene_matches(0));
  // a tap now recalls it: the row, the value, and the engine hears it on the affected track
  const std::size_t n = rig.log.sent.size();
  rig.tap(Button::F1);
  CHECK(rig.bank().row(0) == 2);
  CHECK(rig.bank().live(0) == 30);
  CHECK(rig.bank().scene_matches(0));
  CHECK(rig.log.sent.size() > n);
  // the other scenes are separate and empty; a second hold overwrites
  CHECK_FALSE(rig.bank().scene_stored(1));
  rig.press(Button::F2);
  rig.pass(1);
  rig.pass(1100);
  CHECK(rig.bank().scene_stored(1));
  CHECK(std::string(rig.app->message()) == "SCENE 2 STORED");
  rig.flush();
  rig.release(Button::F2);
  rig.pass();
}

TEST_CASE("perform keys: F6 toggles LATCH, and with it off a pad acts only while it is held; F8 puts every FX off at once and keeps the tracks") {
  Rig rig;
  rig.affect(0);
  rig.tap(Button::Rec);
  rig.pad_down(pad_of(0, 1));
  rig.turn(+40);
  rig.pad_up(pad_of(0, 1));
  rig.tap(Button::Rec);
  CHECK(rig.bank().latch());
  rig.pad_down(pad_of(0, 1));
  rig.pad_up(pad_of(0, 1));
  CHECK(rig.bank().active(0));  // latched (the default)
  rig.tap(Button::F6);
  CHECK_FALSE(rig.bank().latch());
  CHECK(rig.bank().active(0));  // the switch leaves it
  rig.pad_down(pad_of(0, 1));
  CHECK(rig.bank().active(0));
  app::PadRole roles[48];
  CHECK(rig.app->pad_lights(roles));
  CHECK(roles[pad_of(0, 1)] == PadRole::Front);
  rig.pad_up(pad_of(0, 1));
  CHECK_FALSE(rig.bank().active(0));
  CHECK(rig.app->pad_lights(roles));
  CHECK(roles[pad_of(0, 0)] == PadRole::Back);
  rig.tap(Button::F6);
  CHECK(rig.bank().latch());
  rig.pad_down(pad_of(0, 1));
  rig.pad_up(pad_of(0, 1));
  rig.pad_down(pad_of(3, 1));
  rig.pad_up(pad_of(3, 1));
  CHECK(rig.bank().row(0) == 1);
  CHECK(rig.bank().row(3) == 1);
  const std::size_t n = rig.log.sent.size();
  rig.tap(Button::F8);
  for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
    CHECK(rig.bank().row(s) == 0);
  }
  CHECK(rig.bank().affected(0));
  CHECK(rig.bank().stored(0, 1) == 40);
  CHECK(rig.log.sent.size() > n);
  CHECK(rig.log.sent.back().kind == Command::Kind::PerfControl);
  CHECK(rig.log.sent.back().c == (1u << 16));  // the end
}

TEST_CASE("perform keys: F7 queues the next position of the song with the live jump, again cancels, only while the sequencer says it plays, the song loops, the blink is the published one") {
  Rig rig;
  TransportLog t;
  audio::PlayPosition position;
  rig.app->attach_transport(&t);
  rig.app->attach_play_position(&position);
  CHECK(test::arena(1).project.append_order(2) == model::Status::Ok);  // the demo's song has one position: 1, 2, 3
  CHECK(test::arena(1).project.append_order(3) == model::Status::Ok);
  const unsigned count = rig.project().song_length();
  NEED(count == 3u);
  rig.tap(Button::F7);
  CHECK(std::string(rig.app->message()) == "NOT PLAYING");
  rig.flush();
  CHECK(t.jumps == 0u);
  // playing the song at position 1: position 2 is queued; the screen does not claim it before the sequencer publishes it
  position.publish(true, rig.project().order(1), 0, 12000, 1);
  rig.pass(1);
  rig.tap(Button::F7);
  CHECK(t.jumps == 1u);
  CHECK(t.jump_row == 2u);
  CHECK(std::string(rig.app->message()) == "NEXT PATTERN QUEUED");
  rig.flush();
  CHECK(rig.app->state().play.queued == audio::PlayPosition::kNoOrder);
  position.publish_queued(2);
  rig.pass(1);
  CHECK(rig.app->state().play.queued == 2u);
  const bool phase = rig.app->state().blink;
  const std::uint32_t frames = rig.app->frames();
  rig.pass(260);
  CHECK(rig.app->state().blink != phase);  // it blinks (and redraws) on the Perform screen too
  CHECK(rig.app->frames() > frames);
  rig.tap(Button::F7);  // while it waits: cancel
  CHECK(t.jumps == 2u);
  CHECK(t.jump_row == 0xFFFFu);
  CHECK(std::string(rig.app->message()) == "NEXT PATTERN CANCELLED");
  rig.flush();
  position.publish_queued(audio::PlayPosition::kNoOrder);
  rig.pass(1);
  // the last position wraps to the first (the song loops)
  position.publish(true, rig.project().order(count - 1), 0, 12000, count - 1);
  rig.pass(1);
  rig.tap(Button::F7);
  CHECK(t.jump_row == 0u);
  position.publish_queued(audio::PlayPosition::kNoOrder);
  rig.pass(1);
  // Pattern mode: the position is where the pattern that plays sits in the order list
  position.publish(true, rig.project().order(1), 0, 12000);
  rig.pass(1);
  rig.tap(Button::F7);
  CHECK(t.jump_row == 2u);
  rig.flush();
  position.publish_queued(audio::PlayPosition::kNoOrder);
  rig.pass(1);
  // a pattern the song does not have, a refusal (no sequencer behind the sink): nothing is said to have been queued
  position.publish(true, 200, 0, 12000);
  rig.pass(1);
  rig.tap(Button::F7);
  CHECK(std::string(rig.app->message()) == "PATTERN IS NOT IN THE SONG");
  rig.flush();
  position.publish(true, rig.project().order(1), 0, 12000, 1);
  rig.pass(1);
  t.accept_jumps = false;
  const unsigned jumps = t.jumps;
  rig.tap(Button::F7);
  CHECK(t.jumps == jumps);
  CHECK(std::string(rig.app->message()) != "NEXT PATTERN QUEUED");
}

TEST_CASE("perform keys: the soft keys name the jobs") {
  Rig rig;
  CHECK(std::string(app::kPerformScreen.soft_keys()[0]) == "SCENE 1");
  CHECK(std::string(app::kPerformScreen.soft_keys()[3]) == "HALF");
  CHECK(std::string(app::kPerformScreen.soft_keys()[4]) == "DOUBLE");
  CHECK(std::string(app::kPerformScreen.soft_keys()[5]) == "LATCH");
  CHECK(std::string(app::kPerformScreen.soft_keys()[6]) == "NEXT PAT");
  CHECK(std::string(app::kPerformScreen.soft_keys()[7]) == "RESET");
  CHECK(rig.app->failed_frames() == 0);
}

// OS-SHIFTROW: Shift + F1..F3 STORE 1..3 store the scene at once, with a visible key; the plain keys (and their 1 s hold) are not shadowed by Global's jumps any more.
TEST_CASE("perform keys: Shift + F1..F3 store scene 1..3 at once; the release recalls nothing; the plain scene keys still recall") {
  Rig rig;
  for (unsigned n = 0; n < 3; ++n) {
    CHECK_FALSE(rig.bank().scene_stored(n));
  }
  rig.shift_tap(Button::F2);
  CHECK(rig.bank().scene_stored(1));
  CHECK(std::string(rig.app->message()) == "SCENE 2 STORED");
  CHECK_FALSE(rig.bank().scene_stored(0));
  CHECK_FALSE(rig.bank().scene_stored(2));
  rig.flush();
  rig.shift_tap(Button::F3);
  CHECK(rig.bank().scene_stored(2));
  CHECK(std::string(rig.app->message()) == "SCENE 3 STORED");
  rig.flush();
  rig.shift_tap(Button::F1);
  CHECK(rig.bank().scene_stored(0));
  CHECK(std::string(rig.app->message()) == "SCENE 1 STORED");
  rig.flush();
  // a plain tap recalls (SCENE 1 is no longer "empty"); the store key held the plain one down for no time
  rig.tap(Button::F1);
  CHECK(std::string(rig.app->message()).find("EMPTY") == std::string::npos);
  CHECK(rig.bank().scene_matches(0));
  // Shift + F4..F8 are the tracks' queued pattern (E-PERF-07), not the plain HALF, DOUBLE, LATCH, NEXT, RESET
  const bool latch = rig.bank().latch();
  rig.shift_tap(Button::F6);
  CHECK(rig.bank().latch() == latch);
}

// E-PERF-07: Shift + F4..F8 pick and queue a pattern on the tracks that are ON (new Community OS behaviour; the engine side is tests/seq_sequencer_test.cpp).
TEST_CASE("perform keys E-PERF-07: Shift+F7 queues the next song pattern, F6 the picked one, F8 the song's, on the ON tracks only; hints for stopped, no track, a full ring") {
  Rig rig;
  audio::PlayPosition position;
  rig.app->attach_play_position(&position);
  CHECK(std::string(app::kPerformScreen.soft_keys_shifted()[5]) == "TRK PAT");
  CHECK(std::string(app::kPerformScreen.soft_keys_shifted()[6]) == "TRK NEXT");
  CHECK(std::string(app::kPerformScreen.soft_keys_shifted()[7]) == "TRK SONG");
  CHECK(test::arena(1).project.append_order(2) == model::Status::Ok);
  CHECK(test::arena(1).project.append_order(3) == model::Status::Ok);
  (void)test::arena(1).project.create_pattern(2, 16);  // the queue checks that the pattern exists
  (void)test::arena(1).project.create_pattern(3, 16);
  const auto last_queue = [&]() -> const Command* {
    for (std::size_t i = rig.log.sent.size(); i > 0; --i) {
      if (rig.log.sent[i - 1].kind == Command::Kind::QueueTrackPattern) {
        return &rig.log.sent[i - 1];
      }
    }
    return nullptr;
  };
  rig.affect(0);
  rig.affect(2);
  rig.shift_tap(Button::F7);
  CHECK(std::string(rig.app->message()) == "NOT PLAYING");
  CHECK(last_queue() == nullptr);
  rig.flush();
  position.publish(true, rig.project().order(0), 0, 12000, 0);
  rig.pass(1);
  rig.shift_tap(Button::F7);  // the next song slot's pattern, on tracks 1 and 3 only
  const Command* c = last_queue();
  NEED(c != nullptr);
  CHECK(c->c == 0x5u);
  CHECK(c->b == rig.project().order(1));
  CHECK(std::string(rig.app->message()).find("TRACKS QUEUED: PAT ") == 0u);
  rig.flush();
  rig.shift_tap(Button::F8);  // back to the song's
  c = last_queue();
  NEED(c != nullptr);
  CHECK(c->b == 0u);
  CHECK(c->c == 0x5u);
  CHECK(std::string(rig.app->message()) == "TRACKS GO BACK TO THE SONG");
  rig.flush();
  rig.shift_tap(Button::F6);  // nothing picked yet: a hint, no command
  const std::size_t sent = rig.log.sent.size();
  CHECK(std::string(rig.app->message()).find("PICK") == 0u);
  rig.flush();
  rig.shift_tap(Button::F5);  // the next pattern the project has after the one that plays
  CHECK(std::string(rig.app->message()).find("PICK: PAT ") == 0u);
  rig.flush();
  rig.shift_tap(Button::F6);
  c = last_queue();
  NEED(c != nullptr);
  CHECK(c->b == rig.board->pick);
  CHECK(c->b != 0u);
  CHECK(rig.log.sent.size() == sent + 1u);
  rig.flush();
  // a pick that is no pattern any more (deleted since): the hint, nothing queued
  const std::uint8_t picked = rig.board->pick;
  CHECK(rig.project().pattern(200) == nullptr);
  rig.board->pick = 200;
  const std::size_t gone = rig.log.sent.size();
  rig.shift_tap(Button::F6);
  CHECK(std::string(rig.app->message()) == "PATTERN IS GONE: PICK ANOTHER");
  CHECK(rig.log.sent.size() == gone);
  rig.board->pick = picked;
  rig.flush();
  // a full ring: the hint, nothing claimed
  rig.log.open = false;
  rig.shift_tap(Button::F8);
  CHECK(std::string(rig.app->message()) == "COMMANDS BUSY: TRY AGAIN");
  rig.flush();
  rig.log.open = true;
  // no track ON
  rig.affect(0);
  rig.affect(2);
  const std::size_t before = rig.log.sent.size();
  rig.shift_tap(Button::F8);
  CHECK(std::string(rig.app->message()).find("NO TRACK IS ON") == 0u);
  CHECK(rig.log.sent.size() == before);
  CHECK(rig.app->failed_frames() == 0);
}

// ---- OS-NOREMIX (2026-11-08): the REMIX page of OS-PERFREMIX R2/R3 is gone. What stays: Q5 a (leaving Perform sends the tracks that are not on the song back, E-PERF-07's overrides too) and
// H9 (a pad that the DJ page took never reaches the bank). The sequencer is not in these tests: the published words are set by hand. ----

namespace {

constexpr std::uint32_t kTrackWait = 0x10000u;

std::vector<Command> sent_of(const Rig& rig, Command::Kind kind) {
  std::vector<Command> out;
  for (const Command& c : rig.log.sent) {
    if (c.kind == kind) {
      out.push_back(c);
    }
  }
  return out;
}

// Patterns 1..3 exist and the song plays its first; returns that pattern.
unsigned play_song(Rig& rig, audio::PlayPosition& position) {
  (void)test::arena(1).project.create_pattern(2, 16);
  (void)test::arena(1).project.create_pattern(3, 16);
  const unsigned song = rig.project().order(0);
  position.publish(true, song, 0, 12000, 0);
  rig.pass(1);
  return song;
}

}  // namespace

TEST_CASE("perform: leaving Perform for a screen but Master or Song sends the tracks that are not on the song back at once (Q5 a)") {
  Rig rig;
  audio::PlayPosition position;
  rig.app->attach_play_position(&position);
  const unsigned song = play_song(rig, position);
  NEED(song == 1u);
  position.publish_track_pattern(2, 3);                        // track 3 plays 3: an E-PERF-07 override
  position.publish_track_pattern(5, kTrackWait | (2u << 8));  // track 6 waits for 2
  rig.pass(1);
  rig.log.sent.clear();
  rig.tap(Button::Master);  // the Mixer and Song keep the mode
  CHECK(rig.app->state().screen == ScreenId::Mixer);
  rig.tap(Button::Song);
  CHECK(rig.app->state().screen == ScreenId::Song);
  CHECK(sent_of(rig, Command::Kind::SetTrackPattern).empty());
  rig.tap(Button::Pattern);  // anywhere else ends it, in the pass of the change
  CHECK(rig.app->state().screen == ScreenId::Pattern);
  const std::vector<Command> c = sent_of(rig, Command::Kind::SetTrackPattern);
  NEED(c.size() == 1u);
  CHECK(c[0].b == 0u);
  CHECK(c[0].c == ((1u << 2) | (1u << 5)));
  // nothing off the song, or not playing: nothing is sent
  position.publish_track_pattern(2, 0);
  position.publish_track_pattern(5, 0);
  rig.pass(1);
  rig.log.sent.clear();
  rig.tap(Button::Performance);
  rig.tap(Button::Pattern);
  CHECK(sent_of(rig, Command::Kind::SetTrackPattern).empty());
  position.publish_track_pattern(2, 3);
  position.publish(false, song, 0, 12000, 0);
  rig.pass(1);
  rig.tap(Button::Performance);
  rig.tap(Button::Pattern);
  CHECK(sent_of(rig, Command::Kind::SetTrackPattern).empty());
  // a ring that refuses the command: retried the next pass, once
  position.publish(true, song, 0, 12000, 0);
  rig.pass(1);
  rig.tap(Button::Performance);
  rig.log.open = false;
  rig.tap(Button::Pattern);
  CHECK(rig.log.sent.empty());
  rig.log.open = true;
  rig.pass(1);
  NEED(sent_of(rig, Command::Kind::SetTrackPattern).size() == 1u);
  CHECK(sent_of(rig, Command::Kind::SetTrackPattern)[0].c == (1u << 2));
  rig.pass(1);
  CHECK(sent_of(rig, Command::Kind::SetTrackPattern).size() == 1u);
}

TEST_CASE("perform (H9): a pad that the DJ page took no longer reaches the bank at its release, so a slot stays when LATCH is off") {
  Rig rig;
  rig.affect(0);
  rig.tap(Button::Rec);  // EDIT: the encoder stores a value
  rig.pad_down(pad_of(0, 1));
  rig.turn(+30);
  rig.pad_up(pad_of(0, 1));
  rig.tap(Button::Rec);
  CHECK(rig.bank().active(0));  // LATCH is on (the default): the FX goes on at the stored 30
  rig.tap(Button::F6);          // LATCH off
  CHECK_FALSE(rig.bank().latch());
  rig.tap(Button::Performance);  // the DJ page
  CHECK(rig.app->state().dj_page());
  CHECK(rig.board->pads == 0u);
  rig.pad_down(pad_of(0, 1));  // the DJ page's pad (dj.pad), not the bank's
  CHECK(rig.board->pads == 0u);
  rig.pad_up(pad_of(0, 1));
  CHECK(rig.bank().active(0));  // the release must not reach PerformBank::pad_up, which would end the slot (LATCH off, the pad's row)
  CHECK_FALSE(rig.bank().held(0));
  // on the slots page the same pad still ends it: the press is the bank's
  rig.tap(Button::Performance);
  CHECK_FALSE(rig.app->state().dj_page());
  rig.pad_down(pad_of(0, 1));
  CHECK(rig.board->pads != 0u);
  rig.pad_up(pad_of(0, 1));
  CHECK_FALSE(rig.bank().active(0));
  CHECK(rig.board->pads == 0u);
}

TEST_CASE("perform: the Performance button cycles SLOTS, DJ, SLOTS (two pages)") {
  Rig rig;
  CHECK(app::kPerformScreen.pages() == 2u);
  CHECK_FALSE(rig.app->state().dj_page());
  rig.tap(Button::Performance);
  CHECK(rig.app->state().dj_page());
  rig.tap(Button::Performance);
  CHECK_FALSE(rig.app->state().dj_page());
  const bool latch = rig.bank().latch();
  rig.tap(Button::F6);
  CHECK(rig.bank().latch() == !latch);  // the slots' key acts again
  CHECK(rig.app->failed_frames() == 0);
}
