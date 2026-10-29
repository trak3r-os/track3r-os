// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011b: the Mixer screen on the App. The MIXER page (columns, cursor, edits of the model's parameters as undoable actions that tell the engine, the board's own levels,
// mute and solo, the meters' ballistics from a MeterBank) and the pure pieces under it (routing bits). The pictures are the gallery's `mixer` states.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/send_fx_rows.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/meters.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::MixerPage;
using core::ParamId;
using core::ParamKind;
namespace tp = model::track_param;
namespace mp = model::master_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];
unsigned g_stem_commits = 0;  // how many times the board handed the map to the stream
// OS-USBOUTS: the board's request_outputs hook (the platform's asks usb0): how many times it was asked, with what, and whether it takes the request
unsigned g_outputs_requests = 0;
usb::Profile g_outputs_last = usb::Profile::P10;
bool g_outputs_accept = true;

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;
  std::unique_ptr<app::MixBoard> board = std::make_unique<app::MixBoard>();
  audio::StemMap stems;

  Rig() {
    model::Project& p = test::arena(1).project;
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    board->stems = &stems;
    board->apply_stems = [](void) noexcept { ++g_stem_commits; };
    board->request_outputs = [](usb::Profile profile) noexcept {
      g_outputs_last = profile;
      ++g_outputs_requests;
      return g_outputs_accept;
    };
    g_outputs_requests = 0;
    g_outputs_accept = true;
    g_stem_commits = 0;
    app->attach_mix(board.get());
    tap(Button::Master);
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
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  void shift_turn(int detents) {
    press(Button::Shift);
    pass(1);
    turn(detents);
    release(Button::Shift);
    pass(1);
  }
  const model::Project& project() const { return test::arena(1).project; }
  MixerPage page() const { return app::mixer::page_of(app->state()); }
  unsigned col() const { return app::mixer::cursor(app->model(), app->state()).col; }
  unsigned row() const { return app::mixer::cursor(app->model(), app->state()).row; }
  void move_to(unsigned column) {
    for (unsigned i = 0; i < 40 && col() > column; ++i) {
      tap(Button::Left);
    }
    for (unsigned i = 0; i < 40 && col() < column; ++i) {
      tap(Button::Right);
    }
  }
  // The ParamChanges in the ring, in order.
  std::vector<core::ParamChange> sent() {
    std::vector<core::ParamChange> out;
    core::ParamChange c;
    while (ring.pop(c)) {
      out.push_back(c);
    }
    return out;
  }
};

ParamId track_id(unsigned slot, unsigned field) { return core::param_id(ParamKind::Track, slot, field); }

}  // namespace

TEST_CASE("mixer: routing bits turn on and off by the sign of the turn, and the input has no USB column") {
  using namespace app;
  CHECK(route_turn(audio::route::kMain, 0, 1, +1) == (audio::route::kMain | audio::route::kCue));
  CHECK(route_turn(audio::route::kMain | audio::route::kCue, 0, 1, -3) == audio::route::kMain);
  CHECK(route_turn(audio::route::kMain, 0, 0, +5) == audio::route::kMain);  // already set: a turn up leaves it
  CHECK(route_turn(audio::route::kMain, 0, 0, 0) == audio::route::kMain);   // no turn, no change
  CHECK(route_turn(audio::route::kMain, 0, 5, +1) == (audio::route::kMain | audio::route::kUsb));
  CHECK(route_turn(audio::route::kMain, audio::kInputStrip, 5, +1) == audio::route::kMain);  // the input has no USB bit
  CHECK(route_turn(audio::route::kMain, 0, 6, +1) == audio::route::kMain);                   // past the sixth column
  CHECK(return_turn(audio::route::kMain, 1, +1) == (audio::route::kMain | audio::route::kCue));
  CHECK(return_turn(audio::route::kMain, 2, +1) == audio::route::kMain);  // a return has MAIN and CUE only
  CHECK(return_turn(audio::route::kMain | audio::route::kCue, 0, -1) == audio::route::kCue);
  for (unsigned strip = 0; strip < kMixStrips; ++strip) {  // whatever the turn, the result is a valid route
    for (unsigned c = 0; c < 8; ++c) {
      for (int d = -2; d <= 2; ++d) {
        CHECK((route_turn(0x3F, strip, c, d) & ~audio::route::kAll) == 0);
      }
    }
  }
}

TEST_CASE("mixer: the Master button opens the screen on the MIXER page with the active tracks and the five buses") {
  Rig rig;
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  CHECK(rig.page() == MixerPage::Mixer);
  CHECK(app::mixer::tracks(rig.app->model()) == rig.project().active_tracks());
  CHECK(app::mixer::columns(rig.app->model()) == rig.project().active_tracks() + app::kMixBuses);
  CHECK(rig.col() == 0);
  CHECK(rig.row() == 0);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer: the arrows move the column and the field; a bus has a level and no pan") {
  Rig rig;
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  CHECK(rig.col() == 2);
  rig.tap(Button::Down);
  CHECK(rig.row() == 1);  // PAN
  rig.tap(Button::Down);
  CHECK(rig.row() == 1);  // clamped
  rig.tap(Button::Up);
  CHECK(rig.row() == 0);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  CHECK(rig.col() == 0);  // clamped at the left
  const unsigned last = app::mixer::columns(rig.app->model()) - 1u;
  rig.move_to(last);
  CHECK(rig.col() == last);
  CHECK(app::mixer::bus_of(rig.app->model(), rig.col()) == static_cast<unsigned>(app::MixBus::Master));
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Down), true));
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Down), false));
  rig.pass();
  CHECK(rig.row() == 0);  // the master has no pan row
}

TEST_CASE("mixer: the encoder edits the level of a track and tells the engine; the Mixer has no undo (OS-UNDOGRP D10)") {
  Rig rig;
  (void)rig.sent();
  rig.move_to(2);
  const float before = rig.project().get_param(track_id(2, tp::Volume));
  rig.turn(-7);
  CHECK(rig.project().get_param(track_id(2, tp::Volume)) == before - 7.0f);
  std::vector<core::ParamChange> s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == track_id(2, tp::Volume));
  CHECK(s.back().value == before - 7.0f);
  rig.shift_turn(-1);  // the coarse step: ten
  CHECK(rig.project().get_param(track_id(2, tp::Volume)) == before - 17.0f);
  rig.tap(Button::Delete);  // the default
  CHECK(rig.project().get_param(track_id(2, tp::Volume)) == model::param_desc(track_id(2, tp::Volume))->def);
  // its edits are final: no unit in any ring, and the undo key (SOLO here) has nothing to take back
  CHECK(rig.journal.depth() == 0u);
  CHECK(rig.app->invoke(core::action_id("edit.undo"), 0) == app::DispatchResult::Changed);  // (the strip says there is nothing to undo here)
  CHECK(rig.project().get_param(track_id(2, tp::Volume)) == model::param_desc(track_id(2, tp::Volume))->def);
}

TEST_CASE("mixer: the pan field and the master's levels are parameters too") {
  Rig rig;
  (void)rig.sent();
  rig.move_to(1);
  rig.tap(Button::Down);
  const float pan = rig.project().get_param(track_id(1, tp::Pan));
  rig.turn(5);
  CHECK(rig.project().get_param(track_id(1, tp::Pan)) == pan + 5.0f);
  const unsigned t = app::mixer::tracks(rig.app->model());
  rig.move_to(t + static_cast<unsigned>(app::MixBus::Master));
  const float vol = rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::Volume));
  rig.turn(-4);
  CHECK(rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::Volume)) == vol - 4.0f);
  rig.move_to(t + static_cast<unsigned>(app::MixBus::SndA));  // the delay send bus's level is the master DELAY
  const ParamId delay = core::param_id(ParamKind::Master, 0, mp::Delay);
  const float before = rig.project().get_param(delay);
  rig.turn(-10);
  CHECK(rig.project().get_param(delay) == before - 10.0f);
}

// ---- OS-MASTERVOL: Master held + the encoder is the master volume, on any screen ---------------------------------------------

TEST_CASE("master volume: Master held + the encoder moves it from any screen, tells the engine, has no history and says the value") {
  const ParamId volume_id = core::param_id(ParamKind::Master, 0, mp::Volume);
  for (const Button screen : {Button::Pattern, Button::Song, Button::Params, Button::Master}) {
    Rig rig;
    rig.tap(screen);
    (void)rig.sent();
    const app::ScreenId where = rig.app->state().screen;
    const float vol = rig.project().get_param(volume_id);
    rig.press(Button::Master);
    rig.pass(1);
    rig.turn(-4);
    CHECK(rig.project().get_param(volume_id) == vol - 4.0f);
    NEED(!rig.sent().empty());
    CHECK(rig.app->state().screen == where);  // the turn consumed the Master tap: no navigation
    CHECK(std::string(rig.app->state().notify.head().text.c_str()) == "MASTER VOLUME " + std::to_string(static_cast<int>(vol) - 4) + "%");
    rig.turn(-2);  // and again, the key still down
    CHECK(rig.project().get_param(volume_id) == vol - 6.0f);
    const std::vector<core::ParamChange> s = rig.sent();
    NEED(!s.empty());
    CHECK(s.back().id == volume_id);
    CHECK(s.back().value == vol - 6.0f);
    rig.release(Button::Master);
    rig.pass(1);
    CHECK(rig.app->state().screen == where);  // the release is silent
    CHECK(rig.journal.depth() == 0u);          // the Mixer has no history
  }
}

TEST_CASE("master volume: the ends of the range hold and the strip still says where it is; the plain Master tap is the Mixer at the release") {
  const ParamId volume_id = core::param_id(ParamKind::Master, 0, mp::Volume);
  Rig rig;
  rig.tap(Button::Pattern);
  rig.press(Button::Master);
  rig.pass(1);
  rig.turn(-500);
  CHECK(rig.project().get_param(volume_id) == 0.0f);
  CHECK(std::string(rig.app->state().notify.head().text.c_str()) == "MASTER VOLUME 0%");
  rig.turn(-1);  // already at the bottom
  CHECK(rig.project().get_param(volume_id) == 0.0f);
  CHECK(std::string(rig.app->state().notify.head().text.c_str()) == "MASTER VOLUME 0%");
  rig.turn(+500);
  CHECK(rig.project().get_param(volume_id) == 100.0f);
  rig.release(Button::Master);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);

  rig.press(Button::Master);  // a tap: the press waits for the release
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  rig.release(Button::Master);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Mixer);
  CHECK(rig.project().get_param(volume_id) == 100.0f);  // and it did not touch the volume

  rig.tap(Button::Pattern);  // the encoder alone is not the volume
  rig.turn(-3);
  CHECK(rig.project().get_param(volume_id) == 100.0f);
}

// Accepted limits of the gesture (QA-MIX-030 / 031, the CHANGELOG): the volume gesture is the Master key held on its own. A Shift that joins after the Master press ends it: the tap is lost and a
// turn is the screen's own, not the volume. A Shift held before the Master press is ignored by the gesture (the volume moves a plain step a detent, no coarse step). Pinned so that a change of the
// recogniser shows here.
TEST_CASE("master volume: a Shift joined after the Master press loses the tap and the volume; a Shift held first changes nothing") {
  const ParamId volume_id = core::param_id(ParamKind::Master, 0, mp::Volume);
  Rig rig;
  rig.tap(Button::Pattern);
  const float vol = rig.project().get_param(volume_id);
  rig.press(Button::Master);  // Master down first, then Shift joins, then both let go: no Mixer
  rig.pass(1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.release(Button::Master);
  rig.pass(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.project().get_param(volume_id) == vol);

  rig.press(Button::Master);  // Master down first, Shift joins, then the encoder: the screen's own turn, the volume stays
  rig.pass(1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.turn(-4);
  CHECK(rig.project().get_param(volume_id) == vol);
  rig.release(Button::Master);
  rig.pass(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.project().get_param(volume_id) == vol);

  rig.press(Button::Shift);  // Shift held first, then Master, then the encoder: still the volume, one plain step a detent
  rig.pass(1);
  rig.press(Button::Master);
  rig.pass(1);
  rig.turn(-4);
  CHECK(rig.project().get_param(volume_id) == vol - 4.0f);
  rig.release(Button::Master);
  rig.pass(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.app->state().screen == app::ScreenId::Pattern);  // (the turn consumed the Shift + Master tap too)
  CHECK(rig.project().get_param(volume_id) == vol - 4.0f);
}

TEST_CASE("mixer: F7 mutes and F8 solos the track under the cursor, F7 on a send bus mutes the effect") {
  Rig rig;
  (void)rig.sent();
  rig.move_to(3);
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(track_id(3, tp::Mute)) == 1.0f);
  std::vector<core::ParamChange> s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == track_id(3, tp::Mute));
  CHECK(s.back().value == 1.0f);
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(track_id(3, tp::Mute)) == 0.0f);
  rig.tap(Button::F8);
  CHECK(rig.project().get_param(track_id(3, tp::Solo)) == 1.0f);
  CHECK(rig.project().get_param(track_id(2, tp::Solo)) == 0.0f);  // only this track
  const unsigned t = app::mixer::tracks(rig.app->model());
  rig.move_to(t + static_cast<unsigned>(app::MixBus::SndA));
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::DelayMute)) == 1.0f);
  rig.move_to(t + static_cast<unsigned>(app::MixBus::SndB));
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::ReverbMute)) == 1.0f);
  rig.tap(Button::F8);  // no solo for a bus: nothing changes
  CHECK(rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::DryMute)) == 0.0f);
  rig.move_to(t + static_cast<unsigned>(app::MixBus::In));
  const float before = rig.project().get_param(track_id(0, tp::Mute));
  rig.tap(Button::F7);  // the input strip has no mute
  CHECK(rig.project().get_param(track_id(0, tp::Mute)) == before);
}

TEST_CASE("mixer: the INPUT and CUE levels are the board's: the encoder sets them and the engine is told") {
  Rig rig;
  (void)rig.sent();
  const unsigned t = app::mixer::tracks(rig.app->model());
  rig.move_to(t + static_cast<unsigned>(app::MixBus::In));
  CHECK(rig.board->in_level == 0);
  rig.turn(+30);
  CHECK(rig.board->in_level == 30);
  std::vector<core::ParamChange> s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::strip_param(audio::kInputStrip, audio::graph_param::strip::Volume));
  CHECK(s.back().value == 30.0f);
  rig.turn(-100);
  CHECK(rig.board->in_level == 0);
  rig.turn(-1);  // already at the floor: nothing is sent
  CHECK(rig.sent().size() == 1);  // the change of the turn before
  rig.move_to(t + static_cast<unsigned>(app::MixBus::Cue));
  rig.shift_turn(-3);
  CHECK(rig.board->cue_level == 70);
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(audio::graph_param::master::CueLevel));
  CHECK(s.back().value == 70.0f);
  rig.tap(Button::Delete);
  CHECK(rig.board->cue_level == 100);
  rig.move_to(t + static_cast<unsigned>(app::MixBus::In));
  rig.turn(40);
  rig.tap(Button::Delete);
  CHECK(rig.board->in_level == 0);
}

TEST_CASE("mixer: without a project the pages show the defaults and say so, and nothing is written") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  auto a = std::make_unique<app::App>(input, clock, display);
  core::ParamRing ring;
  a->attach_param_ring(&ring);
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Master), true));
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Master), false));
  clock.advance(16 * core::kTicksPerMs);
  a->run_once();
  CHECK(a->state().screen == app::ScreenId::Mixer);
  CHECK(app::mixer::tracks(a->model()) == app::kMixDefaultTracks);
  input.push(core::make_encoder_event(clock.now(), 0, 5));
  clock.advance(16 * core::kTicksPerMs);
  a->run_once();
  core::ParamChange c;
  CHECK_FALSE(ring.pop(c));  // nothing to write to
  CHECK(a->failed_frames() == 0);
}

TEST_CASE("mixer: the board follows the engine's meters once a window and says when a bar moved") {
  audio::MeterStore store;
  audio::MeterBank bank(store);
  app::MixBoard board;
  board.meters = &bank;
  CHECK_FALSE(board.poll());  // no window yet
  const std::uint64_t frames = audio::kMeterWindowBlocks * hal::audio::kBlock;
  for (unsigned b = 0; b < audio::kMeterWindowBlocks; ++b) {
    if (b + 1u == audio::kMeterWindowBlocks) {
      bank.acc(3) = audio::kernel::MeterAcc{16384u, frames * 8192ull * 8192ull};  // a half-scale peak, a quarter-scale RMS
    }
    bank.end_block();
  }
  CHECK(board.poll());
  CHECK(board.windows == 1);
  CHECK(board.meter[3].peak() > 0.0f);
  CHECK(board.meter[3].rms() > 0.0f);
  CHECK(board.meter[4].peak() == 0.0f);
  CHECK_FALSE(board.poll());  // the same window again: nothing to do
  for (unsigned b = 0; b < audio::kMeterWindowBlocks; ++b) {
    bank.end_block();  // a silent window: the bar falls
  }
  const float before = board.meter[3].peak();
  CHECK(board.poll());
  CHECK(board.meter[3].peak() < before);
}

TEST_CASE("mixer: F1..F4 pick the page, the Master button again goes to the next, and the cursor starts the page at its first row") {
  Rig rig;
  rig.tap(Button::Down);
  CHECK(rig.row() == 1);
  rig.tap(Button::F2);
  CHECK(rig.page() == MixerPage::Routing);
  CHECK(rig.row() == 0);
  rig.tap(Button::F3);
  CHECK(rig.page() == MixerPage::MasterFx);
  rig.tap(Button::F4);
  CHECK(rig.page() == MixerPage::Stems);
  rig.tap(Button::F1);
  CHECK(rig.page() == MixerPage::Mixer);
  rig.tap(Button::Master);  // pressed again: the next page, wrapping after the fourth
  CHECK(rig.page() == MixerPage::Routing);
  rig.tap(Button::Master);
  rig.tap(Button::Master);
  CHECK(rig.page() == MixerPage::Stems);
  rig.tap(Button::Master);
  CHECK(rig.page() == MixerPage::Mixer);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer routing: a turn up sets the route bit and a turn down clears it, and the engine hears the byte") {
  Rig rig;
  rig.tap(Button::F2);
  (void)rig.sent();
  const unsigned t = app::mixer::tracks(rig.app->model());
  CHECK(app::mixer::rows(rig.app->model(), rig.app->state()) == t + 4u);
  rig.tap(Button::Down);  // T2
  rig.tap(Button::Right);  // CUE
  CHECK(rig.col() == 1);
  CHECK(rig.board->route[1] == (audio::route::kMain | audio::route::kUsb));
  rig.turn(+1);
  CHECK(rig.board->route[1] == (audio::route::kMain | audio::route::kUsb | audio::route::kCue));
  std::vector<core::ParamChange> s = rig.sent();
  NEED(s.size() == 1);
  CHECK(s[0].id == audio::strip_param(1, audio::graph_param::strip::Route));
  CHECK(s[0].value == static_cast<float>(audio::route::kMain | audio::route::kUsb | audio::route::kCue));
  rig.turn(+1);  // already on
  CHECK(rig.sent().empty());
  rig.turn(-1);
  CHECK(rig.board->route[1] == (audio::route::kMain | audio::route::kUsb));
  rig.turn(-1);  // already off: nothing sent
  CHECK(rig.sent().size() == 1);
  rig.tap(Button::Delete);  // the default of a track: MAIN and USB
  rig.turn(+1);
  rig.tap(Button::Delete);
  CHECK(rig.board->route[1] == (audio::route::kMain | audio::route::kUsb));
  // the other tracks are untouched
  CHECK(rig.board->route[0] == (audio::route::kMain | audio::route::kUsb));
  CHECK(rig.board->route[2] == (audio::route::kMain | audio::route::kUsb));
}

TEST_CASE("mixer routing: the line input has no USB column, a return has two columns, OUT MODE one row") {
  Rig rig;
  rig.tap(Button::F2);
  const unsigned t = app::mixer::tracks(rig.app->model());
  for (unsigned i = 0; i < t; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == t);  // LINE IN
  for (unsigned i = 0; i < 6; ++i) {
    rig.tap(Button::Right);
  }
  CHECK(rig.col() == 5);
  (void)rig.sent();
  rig.turn(+1);  // the USB column of the input: nothing
  CHECK(rig.board->route[audio::kInputStrip] == (audio::route::kMain | audio::route::kCue));
  CHECK(rig.sent().empty());
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  rig.tap(Button::Left);  // SNDA
  rig.turn(+1);
  CHECK(rig.board->route[audio::kInputStrip] == (audio::route::kMain | audio::route::kCue | audio::route::kSndA));
  std::vector<core::ParamChange> s = rig.sent();
  NEED(s.size() == 1);
  CHECK(s[0].id == audio::strip_param(audio::kInputStrip, audio::graph_param::strip::Route));
  rig.tap(Button::Down);  // RET A: the cursor goes inside its two columns
  CHECK(rig.row() == t + 1);
  CHECK(rig.col() == 1);
  rig.turn(+1);
  CHECK(rig.board->ret_route[0] == (audio::route::kMain | audio::route::kCue));
  s = rig.sent();
  NEED(s.size() == 1);
  CHECK(s[0].id == audio::master_param(audio::graph_param::master::RetRouteA));
  CHECK(s[0].value == 3.0f);
  rig.tap(Button::Down);  // RET B
  rig.tap(Button::Left);
  rig.turn(-1);
  CHECK(rig.board->ret_route[1] == 0);
  rig.tap(Button::Delete);
  CHECK(rig.board->ret_route[1] == audio::route::kMain);
  rig.tap(Button::Down);  // OUT MODE
  CHECK(rig.row() == t + 3);
  (void)rig.sent();
  rig.turn(+1);
  CHECK(rig.board->out_mode == 1);
  s = rig.sent();
  NEED(s.size() == 1);
  CHECK(s[0].id == audio::master_param(audio::graph_param::master::OutMode));
  CHECK(s[0].value == 1.0f);
  rig.turn(-1);
  CHECK(rig.board->out_mode == 0);
  rig.turn(+1);
  rig.tap(Button::Delete);
  CHECK(rig.board->out_mode == 0);
  rig.tap(Button::Down);  // the last row stays the last
  CHECK(rig.row() == t + 3);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer routing: F7 and F8 mute and solo the track of the row") {
  Rig rig;
  rig.tap(Button::F2);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(track_id(2, tp::Mute)) == 1.0f);
  rig.tap(Button::F8);
  CHECK(rig.project().get_param(track_id(2, tp::Solo)) == 1.0f);
  const unsigned t = app::mixer::tracks(rig.app->model());
  for (unsigned i = 0; i < t; ++i) {
    rig.tap(Button::Down);  // LINE IN: no mute
  }
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(track_id(0, tp::Mute)) == 0.0f);
}

TEST_CASE("mixer stems: the encoder adds and removes a source of a slot, the map goes to the stream, Shift + Delete is the factory map") {
  Rig rig;
  rig.tap(Button::F4);
  CHECK(app::mixer::stem_rows(rig.app->model()) == 10);  // P10
  CHECK(rig.row() == 0);  // OS-USBOUTS: row 0 is OUTPUTS, S1 is row 1
  rig.tap(Button::Down);
  CHECK(rig.row() == 1);
  const std::uint16_t s1 = rig.stems.slot(0);
  CHECK(s1 == 1u);  // T1
  rig.tap(Button::Right);  // T2
  rig.turn(+1);
  CHECK(rig.stems.slot(0) == 3u);
  CHECK(g_stem_commits == 1);
  rig.turn(+1);  // already in: nothing handed on
  CHECK(g_stem_commits == 1);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // S3 (T3)
  CHECK(rig.row() == 3);
  rig.turn(-1);  // T2 is not in S3: nothing
  CHECK(rig.stems.slot(2) == (1u << 2));
  CHECK(g_stem_commits == 1);
  rig.tap(Button::Left);
  rig.turn(-1);  // T1 is not in S3 either
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  CHECK(rig.col() == 2);
  rig.turn(-1);  // T3 leaves S3
  CHECK(rig.stems.slot(2) == 0u);
  CHECK(g_stem_commits == 2);
  rig.shift_tap(Button::Delete);  // asks first (final, no undo: OS-QAFIX-E-GLOB-18)
  CHECK(rig.stems.slot(2) == 0u);
  CHECK(g_stem_commits == 2);
  rig.shift_tap(Button::Delete);  // the factory map again
  CHECK(rig.stems.slot(2) == (1u << 2));
  CHECK(rig.stems.slot(0) == 1u);
  CHECK(g_stem_commits == 3);
  for (unsigned i = 0; i < 12; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == 10);  // OUTPUTS and ten slots
  for (unsigned i = 0; i < 20; ++i) {
    rig.tap(Button::Right);
  }
  CHECK(rig.col() == 15);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer stems: Insert sets the cursor's cell, Delete clears only that bit, Shift + Delete is the factory map, a no-op hands nothing on") {
  Rig rig;
  rig.tap(Button::F4);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // S2 (T2 by default; row 0 is OUTPUTS)
  CHECK(rig.row() == 2);
  const std::uint16_t s1 = rig.stems.slot(0);
  const std::uint16_t s3 = rig.stems.slot(2);
  CHECK(rig.stems.slot(1) == (1u << 1));
  rig.move_to(12);  // ML
  rig.tap(Button::Insert);
  CHECK(rig.stems.slot(1) == ((1u << 1) | (1u << 12)));
  CHECK(g_stem_commits == 1);
  rig.tap(Button::Insert);  // already set: nothing
  CHECK(rig.stems.slot(1) == ((1u << 1) | (1u << 12)));
  CHECK(g_stem_commits == 1);
  rig.move_to(15);  // SB
  rig.tap(Button::Delete);  // not in the slot: nothing, and the map is not reset
  CHECK(rig.stems.slot(1) == ((1u << 1) | (1u << 12)));
  CHECK(g_stem_commits == 1);
  rig.tap(Button::Insert);
  CHECK(rig.stems.slot(1) == ((1u << 1) | (1u << 12) | (1u << 15)));
  CHECK(g_stem_commits == 2);
  rig.move_to(12);
  rig.tap(Button::Delete);  // asks first
  CHECK(rig.stems.slot(1) == ((1u << 1) | (1u << 12) | (1u << 15)));
  CHECK(g_stem_commits == 2);
  rig.tap(Button::Delete);  // only ML leaves
  CHECK(rig.stems.slot(1) == ((1u << 1) | (1u << 15)));
  CHECK(g_stem_commits == 3);
  CHECK(rig.stems.slot(0) == s1);  // the other rows are as they were
  CHECK(rig.stems.slot(2) == s3);
  rig.shift_tap(Button::Delete);  // asks first
  CHECK(rig.stems.slot(1) == ((1u << 1) | (1u << 15)));
  CHECK(g_stem_commits == 3);
  rig.shift_tap(Button::Delete);  // the whole map, the factory's again
  CHECK(rig.stems.slot(1) == (1u << 1));
  CHECK(g_stem_commits == 4);
  rig.shift_tap(Button::Delete);  // the factory already: asks again, then the same map again (the reset always hands it on, as it did)
  rig.shift_tap(Button::Delete);
  CHECK(rig.stems.slot(1) == (1u << 1));
  CHECK(rig.stems.slot(0) == s1);
  CHECK(rig.app->failed_frames() == 0);
}

// OS-USBOUTS: row 0 of the STEMS page is OUTPUTS, the number of USB outputs of the project: a pending value (starred) edited by the encoder and by Left / Right, applied by F6 pressed twice.
TEST_CASE("mixer stems OUTPUTS: the encoder and Left / Right choose a pending value, F6 asks then applies, the driver is asked once with the profile") {
  Rig rig;
  rig.tap(Button::F4);
  CHECK(rig.row() == 0);
  CHECK(rig.app->state().mixer.outputs_pending == 0);
  CHECK(app::mixer::rows(rig.app->model(), rig.app->state()) == 11u);  // OUTPUTS and ten slots
  rig.turn(-1);  // 10 -> 8
  CHECK(rig.app->state().mixer.outputs_pending == 8);
  rig.turn(-1);
  rig.tap(Button::Left);  // 8 -> 6 -> 4 (Left is the encoder's -1 on this row)
  CHECK(rig.app->state().mixer.outputs_pending == 4);
  rig.tap(Button::Left);
  CHECK(rig.app->state().mixer.outputs_pending == 2);
  rig.tap(Button::Left);  // the lowest: nothing more
  CHECK(rig.app->state().mixer.outputs_pending == 2);
  rig.tap(Button::Right);
  rig.turn(+1);
  CHECK(rig.app->state().mixer.outputs_pending == 6);
  CHECK(g_outputs_requests == 0);  // nothing is asked of the driver until F6
  CHECK(rig.stems.channels() == 10u);  // and the map is as it was
  rig.tap(Button::F6);  // the first press only asks
  CHECK(rig.app->state().notify.last_text() == std::string("USB WILL RECONNECT: PRESS AGAIN"));
  CHECK(g_outputs_requests == 0);
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 1);
  CHECK(g_outputs_last == usb::Profile::P6);
  CHECK(rig.app->state().mixer.outputs_pending == 0);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer stems OUTPUTS: a value back at the present one is not pending, Delete and leaving the page drop it, a late second F6 asks again") {
  Rig rig;
  rig.tap(Button::F4);
  rig.turn(-1);
  CHECK(rig.app->state().mixer.outputs_pending == 8);
  rig.turn(+1);  // back at 10: nothing pending, nothing starred
  CHECK(rig.app->state().mixer.outputs_pending == 0);
  rig.turn(+1);  // the top: nothing
  CHECK(rig.app->state().mixer.outputs_pending == 0);
  rig.tap(Button::F6);  // nothing pending: nothing asked, and the strip says why
  CHECK(g_outputs_requests == 0);
  CHECK(rig.app->state().notify.last_text() == std::string("NOTHING TO APPLY"));
  rig.turn(-1);
  rig.tap(Button::Delete);  // drops it, without a question
  CHECK(rig.app->state().mixer.outputs_pending == 0);
  rig.turn(-2);
  CHECK(rig.app->state().mixer.outputs_pending == 6);
  rig.tap(Button::F6);
  rig.pass(2500);  // too late: asks again
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 0);
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 1);
  rig.turn(-3);  // (the map is still the 10-output one in this rig: 10 -> 8 -> 6 -> 4)
  CHECK(rig.app->state().mixer.outputs_pending == 4);
  rig.tap(Button::F1);  // leaving the page drops it
  CHECK(rig.app->state().mixer.outputs_pending == 0);
  rig.tap(Button::F4);
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 1);
  rig.turn(-1);
  rig.tap(Button::Song);  // so does leaving the screen
  rig.tap(Button::Master);
  CHECK(rig.app->state().mixer.outputs_pending == 0);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer stems OUTPUTS: a refused request keeps the pending value and says so; a change of the value asks again; F6 elsewhere does nothing; the slot rows are one down") {
  Rig rig;
  rig.tap(Button::F4);
  rig.turn(-2);  // 6
  g_outputs_accept = false;  // the driver is busy (a change in flight, DISK, a project's USB part)
  rig.tap(Button::F6);
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 1);
  CHECK(rig.app->state().notify.last_text() == std::string("USB BUSY: TRY AGAIN"));
  CHECK(rig.app->state().mixer.outputs_pending == 6);  // kept
  g_outputs_accept = true;
  rig.turn(-1);  // another value: asked again
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 1);
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 2);
  CHECK(g_outputs_last == usb::Profile::P4);
  // F6 on another page does nothing, and the OUTPUTS row has no slot to edit: Insert and the map are untouched
  rig.turn(-1);
  rig.tap(Button::F1);
  rig.tap(Button::F6);
  rig.tap(Button::F6);
  CHECK(g_outputs_requests == 2);
  rig.tap(Button::F4);
  rig.tap(Button::Insert);
  CHECK(g_stem_commits == 0);
  CHECK(rig.stems.slot(0) == 1u);
  CHECK(app::mixer::outputs_next(11, -1) == 10);  // profile 11 (console only) is left by a turn down, never reached by one up
  CHECK(app::mixer::outputs_next(11, +1) == 11);
  CHECK(app::mixer::outputs_next(2, -1) == 2);
  CHECK(app::mixer::outputs_next(10, +1) == 10);
  CHECK(app::mixer::outputs_next(8, +1) == 10);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer: Delete is the default on the other pages, Shift + Delete the same, Insert does nothing there") {
  Rig rig;
  rig.tap(Button::F2);  // ROUTING
  CHECK(rig.page() == MixerPage::Routing);
  const std::uint8_t def = rig.board->route[0];
  rig.move_to(0);         // MAIN
  rig.turn(-1);           // a route bit leaves
  CHECK(rig.board->route[0] != def);
  rig.tap(Button::Insert);  // not bound here
  CHECK(rig.board->route[0] != def);
  rig.shift_tap(Button::Delete);
  CHECK(rig.board->route[0] == def);
  rig.turn(-1);
  CHECK(rig.board->route[0] != def);
  rig.tap(Button::Delete);
  CHECK(rig.board->route[0] == def);
  CHECK(g_stem_commits == 0);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer stems: without a map the page says so and an edit does nothing") {
  Rig rig;
  rig.board->stems = nullptr;
  rig.tap(Button::F4);
  CHECK(app::mixer::stem_rows(rig.app->model()) == 0);
  rig.turn(+1);
  CHECK(g_stem_commits == 0);
  rig.tap(Button::Delete);
  rig.tap(Button::Insert);
  rig.shift_tap(Button::Delete);
  CHECK(g_stem_commits == 0);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer master fx: the master section's parameters and the CUE level, F5 jumps to it") {
  Rig rig;
  rig.tap(Button::F3);
  CHECK(app::mixer::rows(rig.app->model(), rig.app->state()) == app::kMasterFxRows);
  (void)rig.sent();
  rig.tap(Button::Down);  // DRY
  const ParamId dry = core::param_id(ParamKind::Master, 0, mp::Dry);
  const float before = rig.project().get_param(dry);
  rig.turn(-6);
  CHECK(rig.project().get_param(dry) == before - 6.0f);
  std::vector<core::ParamChange> s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == dry);
  for (unsigned i = 0; i < 3; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == 4);  // DRY MUTE: a switch, the encoder toggles it
  rig.turn(+1);
  CHECK(rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::DryMute)) == 1.0f);
  rig.tap(Button::F1);
  rig.tap(Button::F5);  // CUE LVL
  CHECK(rig.page() == MixerPage::MasterFx);
  CHECK(rig.row() == app::kMasterCueRow);
  rig.turn(-20);
  CHECK(rig.board->cue_level == 80);
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(audio::graph_param::master::CueLevel));
  CHECK(s.back().value == 80.0f);
  rig.tap(Button::Down);  // the first of the send effects' rows (OS-SENDFX)
  CHECK(rig.row() == app::kMasterFirstFxRow);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer master fx: the send effects' controls are rows of the page, edited as the levels are and told to the engine, runtime only (OS-SENDFX)") {
  namespace gpm = audio::graph_param::master;
  using namespace app;
  Rig rig;
  const model::MasterSettings master0 = rig.project().master();
  rig.tap(Button::F3);
  (void)rig.sent();
  // the table: five delay controls and four reverb controls, then TONE, MODE and QUALITY; names fit the page; the defaults are the effects'
  CHECK(kSendFxRows == 12u);
  for (unsigned i = 0; i < kSendFxRows; ++i) {
    CHECK(std::string(kSendFxTable[i].name).size() <= core::kParamNameChars);
    CHECK(kSendFxTable[i].min <= kSendFxTable[i].def);
    CHECK(kSendFxTable[i].def <= kSendFxTable[i].max);
    CHECK(rig.board->fx[i] == kSendFxTable[i].def);
  }
  // every value the page can reach is one the effects accept (the screen never makes a ParamChange the engine would refuse)
  {
    static audio::SendDelay delay;
    static audio::SendReverb reverb;
    for (unsigned i = 0; i < kSendFxRows; ++i) {
      const SendFxRow& row = kSendFxTable[i];
      const unsigned local = row.field - audio::sendfx::kFieldBase[i < kSendFxDelayRows ? 0 : 1];
      for (const unsigned v : {static_cast<unsigned>(row.min), static_cast<unsigned>(row.def), static_cast<unsigned>(row.max)}) {
        INFO(row.name << " " << v);
        CHECK((i < kSendFxDelayRows ? delay.set_param(local, static_cast<float>(v)) : reverb.set_param(local, static_cast<float>(v))));
      }
    }
  }
  // the cursor walks down past the pane: the page holds 24 rows, 16 show
  for (unsigned i = 0; i < kMasterFirstFxRow; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == kMasterFirstFxRow);
  // DLY PINGPONG: a switch, the turn points to the end
  rig.turn(+1);
  CHECK(rig.board->fx[0] == 1);
  std::vector<core::ParamChange> s = rig.sent();
  NEED(s.size() == 1);
  CHECK(s[0].id == audio::master_param(gpm::DelayPingPong));
  CHECK(s[0].value == 1.0f);
  rig.turn(+1);  // already on: nothing changes, nothing is sent
  CHECK(rig.sent().empty());
  rig.turn(-1);
  CHECK(rig.board->fx[0] == 0);
  (void)rig.sent();
  rig.tap(Button::Down);  // SYNC
  rig.tap(Button::Down);  // RATE
  rig.turn(+2);
  CHECK(rig.board->fx[2] == 2);
  rig.turn(+100);
  CHECK(rig.board->fx[2] == 18);  // the last of the 19 rates
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(gpm::DelayRate));
  CHECK(s.back().value == 18.0f);
  rig.tap(Button::Down);  // TIME: 10 ms below 500 and 30 from there
  CHECK(rig.board->fx[3] == 500);
  rig.turn(+1);
  CHECK(rig.board->fx[3] == 530);
  rig.turn(-2);  // one call, so one rule: 530 is not below 500, 30 a detent
  CHECK(rig.board->fx[3] == 470);
  rig.turn(-1);
  CHECK(rig.board->fx[3] == 460);
  rig.turn(-100);
  CHECK(rig.board->fx[3] == 1);
  rig.turn(+1000);
  CHECK(rig.board->fx[3] == 3500);
  (void)rig.sent();
  rig.tap(Button::Down);  // FEEDBACK: stops at 70
  rig.turn(+100);
  CHECK(rig.board->fx[4] == 70);
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(gpm::DelayFeedback));
  CHECK(s.back().value == 70.0f);
  rig.shift_turn(-2);  // a coarse turn: ten a detent
  CHECK(rig.board->fx[4] == 50);
  // Delete puts the default back and tells the engine
  rig.tap(Button::Delete);
  CHECK(rig.board->fx[4] == 50);
  rig.turn(-10);
  CHECK(rig.board->fx[4] == 40);
  (void)rig.sent();
  rig.tap(Button::Delete);
  CHECK(rig.board->fx[4] == 50);
  s = rig.sent();
  NEED(s.size() == 1);
  CHECK(s[0].id == audio::master_param(gpm::DelayFeedback));
  CHECK(s[0].value == 50.0f);
  // the reverb's rows and QUALITY
  for (unsigned i = 0; i < 3 + 3; ++i) {
    rig.tap(Button::Down);  // TONE, MODE, SIZE ... (the cursor moves down one a tap)
  }
  CHECK(rig.row() == kMasterFirstFxRow + 10u);  // RVB DIFFUSE
  rig.turn(-80);
  CHECK(rig.board->fx[10] == 0);
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(gpm::ReverbDiffusion));
  CHECK(s.back().value == 0.0f);
  rig.tap(Button::Down);  // QUALITY, the last of the send effects' rows (the limiter's three follow: OS-LIMIT, the case below)
  CHECK(rig.row() == kMasterFirstLimRow - 1u);
  rig.turn(+1);
  CHECK(rig.board->fx[11] == 1);
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(gpm::ReverbQuality));
  // nothing of it is in the project: the master section is as it was
  CHECK(rig.project().master().reverb_volume == master0.reverb_volume);
  CHECK(rig.project().master().delay_volume == master0.delay_volume);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer master fx: the pure pieces, the stepping and the text of a send effect's row (OS-SENDFX)") {
  using namespace app;
  const SendFxRow& time = kSendFxTable[3];
  CHECK(send_fx_step(time, 500, 1, false) == 530);
  CHECK(send_fx_step(time, 499, 1, false) == 509);  // the detent is 10 below 500, by where it starts
  CHECK(send_fx_step(time, 500, -1, false) == 470);  // 500 is not below 500: 30 a detent: the rule looks at the value the turn starts from
  CHECK(send_fx_step(time, 3490, 1, false) == 3500);
  CHECK(send_fx_step(time, 1, -1, false) == 1);
  const SendFxRow& rate = kSendFxTable[2];
  CHECK(send_fx_step(rate, 17, 1, true) == 18);
  CHECK(send_fx_step(rate, 3, -1, true) == 0);
  CHECK(send_fx_step(kSendFxTable[0], 0, 5, false) == 1);
  CHECK(send_fx_step(kSendFxTable[6], 1, -5, false) == 0);
  CHECK(send_fx_step(kSendFxTable[6], 1, 0, false) == 1);
  char buf[16];
  CHECK(send_fx_text(kSendFxTable[0], 1, buf, sizeof buf) == 2);
  CHECK(std::string(buf) == "ON");
  CHECK(std::string(buf, send_fx_text(kSendFxTable[1], 0, buf, sizeof buf)) == "OFF");
  CHECK(std::string(buf, send_fx_text(rate, 0, buf, sizeof buf)) == "8");
  CHECK(std::string(buf, send_fx_text(rate, 11, buf, sizeof buf)) == "1/4");
  CHECK(std::string(buf, send_fx_text(rate, 18, buf, sizeof buf)) == "1/32");
  CHECK(std::string(buf, send_fx_text(rate, 99, buf, sizeof buf)) == "1/32");  // held to the list
  CHECK(std::string(buf, send_fx_text(time, 3500, buf, sizeof buf)) == "3500");
  CHECK(std::string(buf, send_fx_text(kSendFxTable[6], 1, buf, sizeof buf)) == "STEREO");
  CHECK(std::string(buf, send_fx_text(kSendFxTable[11], 0, buf, sizeof buf)) == "HALF");
  CHECK(std::string(buf, send_fx_text(kSendFxTable[4], 0, buf, sizeof buf)) == "0");
}

TEST_CASE("mixer master fx: the master limiter's three controls are the last rows of the page, edited like the effects' and told to the engine, not stored (OS-LIMIT)") {
  namespace gpm = audio::graph_param::master;
  using namespace app;
  Rig rig;
  const model::MasterSettings master0 = rig.project().master();
  rig.tap(Button::F3);
  (void)rig.sent();
  // the table: threshold, attack and release, in the units and ranges of audio::limiter; names fit the page; the board starts at the defaults
  CHECK(kLimiterRows == 3u);
  CHECK(kMasterFirstLimRow == kMasterFirstFxRow + kSendFxRows);
  CHECK(kMasterFxRows == kMasterFirstLimRow + kLimiterRows);
  for (unsigned i = 0; i < kLimiterRows; ++i) {
    CHECK(std::string(kLimiterTable[i].name).size() <= core::kParamNameChars);
    CHECK(kLimiterTable[i].min <= kLimiterTable[i].def);
    CHECK(kLimiterTable[i].def <= kLimiterTable[i].max);
    CHECK(rig.board->lim[i] == kLimiterTable[i].def);
    CHECK(&master_fx_row(kSendFxRows + i) == &kLimiterTable[i]);
    CHECK(&rig.board->control(kSendFxRows + i) == &rig.board->lim[i]);
  }
  CHECK(&master_fx_row(0) == &kSendFxTable[0]);
  CHECK(&rig.board->control(11) == &rig.board->fx[11]);
  // every value the page can reach is one the limiter accepts (the screen never makes a ParamChange the engine would refuse)
  {
    static audio::MasterLimiter limiter;
    limiter.reset();
    const unsigned local[kLimiterRows] = {audio::limiter::Threshold, audio::limiter::Attack, audio::limiter::Release};
    const unsigned ids[kLimiterRows] = {gpm::LimiterThreshold, gpm::LimiterAttack, gpm::LimiterRelease};
    for (unsigned i = 0; i < kLimiterRows; ++i) {
      CHECK(kLimiterTable[i].field == ids[i]);
      for (const unsigned v : {static_cast<unsigned>(kLimiterTable[i].min), static_cast<unsigned>(kLimiterTable[i].def), static_cast<unsigned>(kLimiterTable[i].max)}) {
        INFO(kLimiterTable[i].name << " " << v);
        CHECK(limiter.set_param(local[i], static_cast<float>(v)));
      }
      CHECK_FALSE(limiter.set_param(local[i], static_cast<float>(kLimiterTable[i].max) + 1.0f));
      CHECK_FALSE(limiter.set_param(local[i], static_cast<float>(kLimiterTable[i].min) - 1.0f));
    }
  }
  // walk to the first limiter row (the page scrolls past the pane)
  for (unsigned i = 0; i < kMasterFirstLimRow; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == kMasterFirstLimRow);
  // THRESHOLD: a detent is one per cent, held to 1..100
  rig.turn(+5);
  CHECK(rig.board->lim[0] == 97);
  rig.turn(+100);
  CHECK(rig.board->lim[0] == 100);
  std::vector<core::ParamChange> s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(gpm::LimiterThreshold));
  CHECK(s.back().value == 100.0f);
  rig.turn(-200);
  CHECK(rig.board->lim[0] == 1);
  rig.shift_turn(+3);  // a coarse turn: ten a detent
  CHECK(rig.board->lim[0] == 31);
  (void)rig.sent();
  rig.tap(Button::Delete);  // the default, told to the engine
  CHECK(rig.board->lim[0] == 92);
  s = rig.sent();
  NEED(s.size() == 1);
  CHECK(s[0].id == audio::master_param(gpm::LimiterThreshold));
  CHECK(s[0].value == 92.0f);
  // ATTACK: milliseconds, the detent grows with the value
  rig.tap(Button::Down);
  CHECK(rig.board->lim[1] == 10);
  rig.turn(+1);
  CHECK(rig.board->lim[1] == 15);  // five a detent from 10 (below 50)
  rig.turn(-100);
  CHECK(rig.board->lim[1] == 1);
  rig.turn(+1000);
  CHECK(rig.board->lim[1] == 1000);
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(gpm::LimiterAttack));
  CHECK(s.back().value == 1000.0f);
  // RELEASE, the last row
  rig.tap(Button::Down);
  CHECK(rig.row() == kMasterFxRows - 1u);
  rig.turn(+3);
  CHECK(rig.board->lim[2] == 130);
  s = rig.sent();
  NEED(!s.empty());
  CHECK(s.back().id == audio::master_param(gpm::LimiterRelease));
  CHECK(s.back().value == 130.0f);
  rig.tap(Button::Down);
  CHECK(rig.row() == kMasterFxRows - 1u);  // the last row stays the last
  // the send effects' rows are untouched, and nothing of it is in the project: the master section is as it was
  CHECK(rig.board->fx[0] == kSendFxTable[0].def);
  CHECK(rig.project().master().flags == master0.flags);
  CHECK(rig.project().master().volume == master0.volume);
  CHECK(rig.app->failed_frames() == 0);
}

TEST_CASE("mixer master fx: the pure pieces, the stepping and the text of a millisecond row (OS-LIMIT)") {
  using namespace app;
  const SendFxRow& attack = kLimiterTable[1];
  CHECK(send_fx_step(attack, 1, 1, false) == 2);          // 1 a detent below 10
  CHECK(send_fx_step(attack, 9, 1, false) == 10);
  CHECK(send_fx_step(attack, 10, 1, false) == 15);        // 5 from 10, below 50
  CHECK(send_fx_step(attack, 49, 1, false) == 54);
  CHECK(send_fx_step(attack, 50, 1, false) == 60);        // 10 from 50, below 200
  CHECK(send_fx_step(attack, 200, 1, false) == 225);      // 25 from 200, below 500
  CHECK(send_fx_step(attack, 500, 1, false) == 550);      // 50 from 500
  CHECK(send_fx_step(attack, 990, 1, false) == 1000);     // held to the range
  CHECK(send_fx_step(attack, 1000, 1, false) == 1000);
  CHECK(send_fx_step(attack, 100, -1, false) == 90);
  CHECK(send_fx_step(attack, 100, 2, true) == 200);       // a coarse turn: five a detent
  CHECK(send_fx_step(attack, 2, -1, false) == 1);
  CHECK(send_fx_step(attack, 1, -1, false) == 1);
  char buf[16];
  CHECK(std::string(buf, send_fx_text(attack, 250, buf, sizeof buf)) == "250");
  CHECK(std::string(attack.unit) == "ms");
  CHECK(std::string(kLimiterTable[0].unit) == "%");
}

// OS-SHIFTROW: Shift + F7 UNMUTE and Shift + F8 UNSOLO clear every strip's mute / solo (final, like every Mixer edit: OS-UNDOGRP D10).
TEST_CASE("mixer: Shift + F7 unmutes and Shift + F8 unsolos every strip, with no undo unit; with nothing to clear they change nothing") {
  Rig rig;
  const unsigned t = app::mixer::tracks(rig.app->model());
  NEED(t >= 5);
  for (const unsigned strip : {0u, 2u, 4u}) {
    rig.move_to(strip);
    rig.tap(Button::F7);
    CHECK(rig.project().get_param(track_id(strip, tp::Mute)) == 1.0f);
  }
  for (const unsigned strip : {1u, 3u}) {
    rig.move_to(strip);
    rig.tap(Button::F8);
  }
  CHECK(rig.project().get_param(track_id(3, tp::Solo)) == 1.0f);
  (void)rig.sent();

  rig.shift_tap(Button::F7);
  for (unsigned strip = 0; strip < t; ++strip) {
    CHECK_MESSAGE(rig.project().get_param(track_id(strip, tp::Mute)) == 0.0f, "strip " << strip);
  }
  CHECK(rig.project().get_param(track_id(3, tp::Solo)) == 1.0f);  // solo is not mute
  std::vector<core::ParamChange> s = rig.sent();
  CHECK(s.size() == 3);  // only the three that were on were told to the engine
  CHECK(rig.journal.depth() == 0u);  // OS-UNDOGRP D10: the Mixer's edits are final, no unit
  CHECK(rig.app->invoke(core::action_id("edit.undo"), 0) == app::DispatchResult::Changed);  // nothing to undo here: the mutes stay off
  for (const unsigned strip : {0u, 2u, 4u}) {
    CHECK(rig.project().get_param(track_id(strip, tp::Mute)) == 0.0f);
  }

  rig.shift_tap(Button::F8);
  for (unsigned strip = 0; strip < t; ++strip) {
    CHECK_MESSAGE(rig.project().get_param(track_id(strip, tp::Solo)) == 0.0f, "strip " << strip);
  }
  CHECK(rig.journal.depth() == 0u);

  rig.shift_tap(Button::F8);
  (void)rig.sent();
  CHECK(rig.app->invoke(core::action_id("mixer.solo_none"), 0) == app::DispatchResult::Unchanged);  // nothing left to clear
  CHECK(rig.app->invoke(core::action_id("mixer.mute_none"), 0) == app::DispatchResult::Unchanged);
  CHECK(rig.sent().empty());
}

// OS-QAFIX-9: F7 MUTE and F8 SOLO are labelled on every page, so they answer on MSTFX and STEMS too.
TEST_CASE("mixer master fx: F7 mutes the bus of the DRY, REVERB and DELAY rows and their switches; anywhere else it says so; F8 says TRACKS ONLY") {
  Rig rig;
  rig.tap(Button::F3);
  const ParamId dry_mute = core::param_id(ParamKind::Master, 0, mp::DryMute);
  const ParamId rev_mute = core::param_id(ParamKind::Master, 0, mp::ReverbMute);
  const ParamId del_mute = core::param_id(ParamKind::Master, 0, mp::DelayMute);
  rig.tap(Button::F7);  // VOLUME: no bus
  CHECK(rig.app->state().notify.last_text() == std::string("DRY, REVERB OR DELAY ROW"));
  rig.tap(Button::Down);  // DRY
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(dry_mute) == 1.0f);
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(dry_mute) == 0.0f);
  rig.tap(Button::Down);  // REVERB
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(rev_mute) == 1.0f);
  rig.tap(Button::Down);  // DELAY
  rig.tap(Button::Down);  // DRY MUTE: the switch's own row mutes the same bus
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(dry_mute) == 1.0f);
  rig.tap(Button::Down);  // REVERB MUTE
  rig.tap(Button::Down);  // DELAY MUTE
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(del_mute) == 1.0f);
  rig.tap(Button::F8);
  CHECK(rig.app->state().notify.last_text() == std::string("TRACKS ONLY"));
}

TEST_CASE("mixer routing: F7 and F8 on the IN, SND A and SND B rows say TRACKS ONLY, not NO TRACK") {
  Rig rig;
  rig.tap(Button::F2);
  const unsigned tracks = app::mixer::tracks(rig.app->model());
  for (unsigned i = 0; i < tracks; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.row() == tracks);  // the first row past the tracks
  rig.tap(Button::F7);
  CHECK(rig.app->state().notify.last_text() == std::string("TRACKS ONLY"));
  rig.tap(Button::F8);
  CHECK(rig.app->state().notify.last_text() == std::string("TRACKS ONLY"));
}

TEST_CASE("mixer stems: F7 and F8 mute and solo the track of the column, SA and SB mute their effect, the rest says why") {
  Rig rig;
  rig.tap(Button::F4);
  rig.tap(Button::Down);  // S1 (row 0 is OUTPUTS: Left / Right edit its value there)
  rig.move_to(2);  // T3
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(track_id(2, tp::Mute)) == 1.0f);
  rig.tap(Button::F8);
  CHECK(rig.project().get_param(track_id(2, tp::Solo)) == 1.0f);
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(track_id(2, tp::Mute)) == 0.0f);
  rig.move_to(14);  // SA
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::DelayMute)) == 1.0f);
  rig.tap(Button::F8);
  CHECK(rig.app->state().notify.last_text() == std::string("TRACKS ONLY"));
  rig.move_to(15);  // SB
  rig.tap(Button::F7);
  CHECK(rig.project().get_param(core::param_id(ParamKind::Master, 0, mp::ReverbMute)) == 1.0f);
  rig.move_to(12);  // ML
  rig.tap(Button::F7);
  CHECK(rig.app->state().notify.last_text() == std::string("TRACKS ONLY"));
  CHECK(rig.app->failed_frames() == 0);
}

// OS-QAFIX-E-GLOB-18 (maintainer, 2026-10-22): the Mixer has no undo, only RESET asks: STEMS Delete and Shift + Delete are final, so the first press asks and the same key again does it.
TEST_CASE("mixer stems: Delete and Shift + Delete ask first; the question lapses with time, with the other key and with the cursor") {
  Rig rig;
  rig.tap(Button::F4);
  rig.tap(Button::Down);  // S1
  const std::uint16_t s1 = rig.stems.slot(0);  // T1
  rig.tap(Button::Delete);
  CHECK(rig.app->state().notify.last_text() == std::string("DELETE AGAIN: THE SOURCE LEAVES THE SLOT"));
  CHECK(rig.stems.slot(0) == s1);
  CHECK(g_stem_commits == 0);
  rig.pass(2500);  // too late: asks again
  rig.tap(Button::Delete);
  CHECK(rig.stems.slot(0) == s1);
  rig.tap(Button::Down);  // S2 (T2): the cursor moved to a cell that is in its slot, so it asks for that one
  rig.tap(Button::Right);
  rig.tap(Button::Delete);
  CHECK(rig.stems.slot(1) == (1u << 1));
  rig.tap(Button::Left);  // and back on the first cell: asks again
  rig.tap(Button::Up);
  rig.tap(Button::Delete);
  CHECK(rig.stems.slot(0) == s1);
  rig.shift_tap(Button::Delete);  // the other key: its own question
  CHECK(rig.app->state().notify.last_text() == std::string("SHIFT+DELETE AGAIN: STEMS BACK TO FACTORY"));
  CHECK(rig.stems.slot(0) == s1);
  CHECK(g_stem_commits == 0);
  rig.tap(Button::Delete);  // and the plain Delete asks again
  CHECK(rig.stems.slot(0) == s1);
  rig.tap(Button::Delete);
  CHECK(rig.stems.slot(0) == 0u);
  CHECK(g_stem_commits == 1);
  rig.tap(Button::Delete);  // nothing left in the cell: nothing to ask, nothing to do
  rig.tap(Button::Delete);
  CHECK(g_stem_commits == 1);
  CHECK(rig.app->state().mixer.reset_asked == 0);  // (last_text keeps the old question; the state says none is open)
}

// OS-QAFIX-9 (review): the question dies with the page. Ask, go to another page and back inside the window: the Delete asks again, it does not confirm.
TEST_CASE("mixer stems: a page change or leaving the Mixer takes the Delete question back") {
  Rig rig;
  rig.tap(Button::F4);
  rig.tap(Button::Down);  // S1
  const std::uint16_t s1 = rig.stems.slot(0);
  rig.tap(Button::Delete);
  CHECK(rig.app->state().notify.last_text() == std::string("DELETE AGAIN: THE SOURCE LEAVES THE SLOT"));
  rig.tap(Button::F1);
  rig.tap(Button::F4);
  rig.tap(Button::Down);  // (back on the page the cursor is on the first row again)
  rig.tap(Button::Delete);
  CHECK(rig.stems.slot(0) == s1);
  CHECK(g_stem_commits == 0);
  rig.tap(Button::Song);  // leaving the screen does the same
  rig.tap(Button::Master);
  rig.tap(Button::Delete);
  CHECK(rig.stems.slot(0) == s1);
  CHECK(g_stem_commits == 0);
  rig.tap(Button::Delete);  // and the asked key still confirms on the page it was asked on
  CHECK(rig.stems.slot(0) != s1);
  CHECK(g_stem_commits == 1);
}
