// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MIDICCMAP (docs/specs/OS-MIDICCMAP.md, sections 3.1 to 3.4): the fixed CC map's pure table and value laws, and the app's side on the App rig — CC 71..82 move the mixer
// when CC MAP is On, final (no undo unit, no FX step in a take, QA-MIX-078), Learn bindings keep priority (Q2 a), the Learn overlay binds, a WAV export blocks it, CC MAP Off is
// byte for byte today's behaviour, CC 51..62 drive a Perform slot on the slot page only (S3, Q5 a), and CC 41..48 queue a pattern on one track while playing on the same page (S4:
// the dedupe of the last value sent, the refused pattern's strip message, the clear when the run stops). Host only (the app, the host fakes; the queue needs a running sequencer).
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/cc_map.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace o3os;
using app::Button;
using app::DispatchResult;
using app::PatternMode;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
namespace ds = model::device_setting;
namespace tp = model::track_param;
namespace mp = model::master_param;
namespace ip = model::instrument_param;
namespace gp = audio::graph_param;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

ParamId track(unsigned t) { return core::param_id(ParamKind::Track, t, tp::Volume); }
ParamId master(unsigned field) { return core::param_id(ParamKind::Master, 0, field); }

ActionArg cc(unsigned channel, unsigned controller, unsigned value) {
  return static_cast<ActionArg>((channel << 16) | (controller << 8) | value);
}

// The app rig of mixer_test.cpp: the demo document, the undo journal, the ParamChange ring and the MixBoard, on the Pattern screen. A Perform board is attached too, for the
// slot CCs of S3 (its sink discards: the bank's commands are seq_perform_test's, here the bank's state is what is checked).
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;
  std::unique_ptr<app::MixBoard> board = std::make_unique<app::MixBoard>();
  std::unique_ptr<app::PerformBoard> perform = std::make_unique<app::PerformBoard>();

  Rig() {
    model::Project& p = test::arena(1).project;
    p.reset();
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->attach_mix(board.get());
    app->attach_perform(perform.get());
    app->run_once();
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  DispatchResult midicc(unsigned channel, unsigned num, unsigned value) { return invoke("midi.cc", cc(channel, num, value)); }
  void cc_map_on() { NEED(app->device_settings().set(ds::CcMap, 1.0f) == model::Status::Ok); }
  void bind_device(unsigned slot, ParamId param, std::uint8_t num, std::uint8_t channel, core::LearnMode mode) {
    NEED(app->device_settings().set_learn(slot, core::LearnBinding{param, num, channel, static_cast<std::uint8_t>(mode), 0}) == model::Status::Ok);
  }
  model::Project& project() { return test::arena(1).project; }
  float value(ParamId id) { return project().get_param(id); }
  unsigned depth() const { return journal.depth(); }
  app::ViewState& view() { return const_cast<app::ViewState&>(app->state()); }  // (a test steers a state that only a running export or recording would)
  // Every ParamChange pushed since the last drain.
  std::vector<core::ParamChange> drained() {
    std::vector<core::ParamChange> out;
    core::ParamChange pc;
    while (ring.pop(pc)) {
      out.push_back(pc);
    }
    return out;
  }
  bool has_binding(unsigned num) const {
    const core::LearnBinding* stores[2] = {app->model().device.learn_slots(),
                                           app->model().project != nullptr ? app->model().project->settings().learn_slots() : nullptr};
    for (const core::LearnBinding* slots : stores) {
      for (unsigned s = 0; slots != nullptr && s < core::kLearnSlots; ++s) {
        if (!core::learn_empty(slots[s]) && slots[s].cc == num) {
          return true;
        }
      }
    }
    return false;
  }
};

// A sink that keeps what the board sends (the sequencer's command ring on the device).
struct RecSink final : seq::CommandSink {
  std::vector<seq::Command> cmds;
  bool push(const seq::Command& c) noexcept override {
    cmds.push_back(c);
    return true;
  }
};

// The transport the Play key talks to (the engine's on the device): it counts what the real keys asked.
struct FakeTransport final : app::TransportSink {
  unsigned plays = 0;
  unsigned stops = 0;
  bool play(unsigned) noexcept override {
    ++plays;
    return true;
  }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override {
    ++stops;
    return true;
  }
};

}  // namespace

TEST_CASE("cc map: the table over 0..127 (every target, None elsewhere; 120..127 never reach it)") {
  for (unsigned c = 0; c < 128u; ++c) {
    const app::CcTarget t = app::cc_map_target(c);
    if (c >= 41u && c <= 48u) {
      CHECK(t.kind == app::CcMapKind::PerfTrack);
      CHECK(t.index == c - 41u);
    } else if (c >= 51u && c <= 62u) {
      CHECK(t.kind == app::CcMapKind::PerfSlot);
      CHECK(t.index == c - 51u);
    } else if (c >= 71u && c <= 78u) {
      CHECK(t.kind == app::CcMapKind::Track);
      CHECK(t.index == c - 71u);
    } else if (c == 79u) {
      CHECK(t.kind == app::CcMapKind::MasterDelay);
    } else if (c == 80u) {
      CHECK(t.kind == app::CcMapKind::MasterReverb);
    } else if (c == 81u) {
      CHECK(t.kind == app::CcMapKind::MasterDry);
    } else if (c == 82u) {
      CHECK(t.kind == app::CcMapKind::Input);
    } else {
      CHECK(t.kind == app::CcMapKind::None);  // 40, 49, 50, 63..70, 83..127
    }
  }
}

TEST_CASE("cc map: the value laws (0 -> 0, 64 -> 50, 127 -> 100; the perf law at both ends of a signed range; the pattern law)") {
  CHECK(app::cc_map_level(0u) == 0u);
  CHECK(app::cc_map_level(64u) == 50u);
  CHECK(app::cc_map_level(127u) == 100u);
  CHECK(app::cc_map_perf(0u, -64, 63) == -64);
  CHECK(app::cc_map_perf(127u, -64, 63) == 63);
  CHECK(app::cc_map_perf(64u, -64, 63) == 0);
  CHECK(app::cc_map_perf(0u, 0, 100) == 0);
  CHECK(app::cc_map_perf(127u, 0, 100) == 100);
  CHECK(app::cc_map_pattern(0u) == 1u);
  CHECK(app::cc_map_pattern(127u) == 128u);
}

TEST_CASE("cc map app: CC MAP On, CC 71..82 move the mixer on the Pattern screen — the tracks, the master levels, the input strip — final, no undo unit") {
  Rig rig;
  rig.cc_map_on();
  NEED(rig.app->state().screen == app::ScreenId::Pattern);
  (void)rig.project().set_param(track(0), 0.0f);
  (void)rig.project().set_param(track(7), 90.0f);
  (void)rig.project().set_param(master(mp::Delay), 10.0f);
  (void)rig.project().set_param(master(mp::Reverb), 20.0f);
  (void)rig.project().set_param(master(mp::Dry), 30.0f);
  rig.pass();
  const unsigned depth = rig.depth();
  CHECK(rig.midicc(1, 71, 127) == DispatchResult::Changed);  // CC 71 = 127: track 1 at 100
  CHECK(rig.value(track(0)) == 100.0f);
  CHECK(rig.midicc(1, 78, 64) == DispatchResult::Changed);  // CC 78 = 64: track 8 at 50
  CHECK(rig.value(track(7)) == 50.0f);
  CHECK(rig.midicc(1, 79, 127) == DispatchResult::Changed);  // CC 79 / 80 / 81: the master's DELAY / REVERB / DRY
  CHECK(rig.value(master(mp::Delay)) == 100.0f);
  CHECK(rig.midicc(1, 80, 64) == DispatchResult::Changed);
  CHECK(rig.value(master(mp::Reverb)) == 50.0f);
  CHECK(rig.midicc(1, 81, 0) == DispatchResult::Changed);
  CHECK(rig.value(master(mp::Dry)) == 0.0f);
  CHECK(rig.midicc(1, 82, 127) == DispatchResult::Changed);  // CC 82: the INPUT strip, the board and the ParamChange
  CHECK(rig.board->in_level == 100u);
  const std::vector<core::ParamChange> sent = rig.drained();
  bool told = false;
  for (const core::ParamChange& pc : sent) {
    if (pc.id == audio::strip_param(audio::kInputStrip, gp::strip::Volume)) {
      told = true;
      CHECK(pc.value == 100.0f);
    }
  }
  CHECK(told);
  CHECK(rig.depth() == depth);  // final: no undo unit, no journal entry (D10)
  // a message outside the map does nothing
  CHECK(rig.midicc(1, 70, 127) != DispatchResult::Changed);
  CHECK(rig.midicc(1, 83, 127) != DispatchResult::Changed);
  CHECK(rig.value(track(0)) == 100.0f);
}

TEST_CASE("cc map app: CC MAP Off changes nothing that midi.cc did before") {
  Rig rig;
  NEED(rig.app->device_settings().get(ds::CcMap) == 0.0f);  // the default
  (void)rig.project().set_param(track(0), 33.0f);
  rig.pass();
  (void)rig.drained();  // whatever the pass itself told the engine
  const unsigned in_level = rig.board->in_level;
  CHECK(rig.midicc(1, 71, 127) != DispatchResult::Changed);
  CHECK(rig.midicc(1, 82, 127) != DispatchResult::Changed);
  CHECK(rig.value(track(0)) == 33.0f);
  CHECK(rig.board->in_level == in_level);
  CHECK(rig.drained().empty());
}

TEST_CASE("cc map app: a Learn binding wins over the map (Q2 a) — a Pickup binding that has not caught the knob still owns the message; another channel does not") {
  Rig rig;
  rig.cc_map_on();
  (void)rig.project().set_param(track(0), 10.0f);
  rig.pass();
  rig.bind_device(0, track(0), 71, 0, core::LearnMode::Pickup);  // CC 71, every channel, pickup
  CHECK(rig.midicc(1, 71, 127) != DispatchResult::Changed);     // the knob has not reached 10: the binding owns the message, the map does nothing
  CHECK(rig.value(track(0)) == 10.0f);
  rig.bind_device(0, track(0), 71, 5, core::LearnMode::Pickup);  // the same CC on channel 5 only
  CHECK(rig.midicc(6, 71, 127) == DispatchResult::Changed);      // channel 6 is not listened to: the map acts
  CHECK(rig.value(track(0)) == 100.0f);
}

TEST_CASE("cc map app: with the Learn overlay up the CC binds, the map does not act") {
  Rig rig;
  rig.cc_map_on();
  (void)rig.project().set_param(track(0), 10.0f);
  rig.pass();
  rig.tap(Button::Params);  // the instrument params page: the cursor is on a row a controller can bind to
  CHECK(rig.invoke("learn.toggle", 0) != DispatchResult::Disabled);
  CHECK(rig.view().overlay == app::OverlayId::Learn);
  CHECK(rig.midicc(1, 71, 64) != DispatchResult::Disabled);  // binds CC 71 to the row under the cursor
  CHECK(rig.has_binding(71));
  CHECK(rig.value(track(0)) == 10.0f);  // the map did not act
  CHECK(rig.invoke("learn.toggle", 0) != DispatchResult::Disabled);
  CHECK(rig.midicc(1, 71, 127) != DispatchResult::Changed);  // the new binding listens (pickup, not caught): the map still does nothing
  CHECK(rig.value(track(0)) == 10.0f);
}

TEST_CASE("cc map app: a WAV export running: nothing") {
  Rig rig;
  rig.cc_map_on();
  (void)rig.project().set_param(track(0), 10.0f);
  rig.pass();
  rig.view().xport.running = true;
  CHECK(rig.midicc(1, 71, 127) != DispatchResult::Changed);
  CHECK(rig.midicc(1, 82, 127) != DispatchResult::Changed);
  CHECK(rig.value(track(0)) == 10.0f);
  CHECK(rig.board->in_level == 0u);
  rig.view().xport.running = false;
  CHECK(rig.midicc(1, 71, 127) == DispatchResult::Changed);  // the control: it works again after
  CHECK(rig.value(track(0)) == 100.0f);
}

TEST_CASE("cc map app: CC 51..62 act only on the Perform screen's slot page (Q5 a) — the slot enters CC mode at the official law; the DJ page and the other screens do not") {
  Rig rig;
  rig.cc_map_on();
  NEED(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.midicc(1, 51, 127) != DispatchResult::Changed);  // the Pattern screen: no
  CHECK_FALSE(rig.perform->bank.cc_mode(0));
  rig.tap(Button::Performance);
  NEED(rig.app->state().screen == app::ScreenId::Perform);
  NEED(!rig.view().dj_page());
  const model::PerfState slots_before = rig.perform->bank.state();
  CHECK(rig.midicc(1, 51, 127) == DispatchResult::Changed);  // CC 51: slot 1 (VOLUME -100..100), 127 -> +100
  CHECK(rig.perform->bank.cc_mode(0));
  CHECK(rig.perform->bank.live(0) == 100);
  CHECK(rig.perform->bank.active(0));
  CHECK(rig.perform->bank.state() == slots_before);  // CC mode never reaches state() (the PERF chunk, the scenes)
  CHECK(rig.midicc(1, 62, 0) == DispatchResult::Changed);    // CC 62: slot 12 (VOL LFO -23..23), 0 -> -23
  CHECK(rig.perform->bank.cc_mode(11));
  CHECK(rig.perform->bank.live(11) == -23);
  CHECK(rig.midicc(1, 51, 127) != DispatchResult::Changed);  // the same value again: nothing changed
  CHECK(rig.midicc(1, 41, 4) != DispatchResult::Changed);    // CC 41..48 (S4) need the sequencer playing: stopped here, nothing
  rig.tap(Button::Performance);  // the DJ page of Perform
  NEED(rig.view().dj_page());
  CHECK(rig.midicc(1, 51, 127) != DispatchResult::Changed);  // no: only the slot page takes the slot CCs
  CHECK(rig.perform->bank.cc_mode(0));                       // the mode stays while the page is kept, no new CC lands
  rig.tap(Button::Pattern);  // leaving the screen ends CC mode (the bank's turn_off)
  NEED(rig.app->state().screen == app::ScreenId::Pattern);
  CHECK(rig.midicc(1, 51, 127) != DispatchResult::Changed);
  CHECK_FALSE(rig.perform->bank.cc_mode(0));
}

TEST_CASE("cc map app: with a take open, a mapped CC moves its parameter and records no FX step (QA-MIX-078); a Learn binding on the instrument does record one") {
  Rig rig;
  rig.cc_map_on();
  (void)rig.project().set_param(track(0), 10.0f);
  rig.pass();
  (void)rig.invoke("view.mode", static_cast<ActionArg>(PatternMode::Edit));  // LIVE: recording is armed
  rig.pass();
  // a take is open and the playhead is in the edited pattern (a test steers the state a running transport and rec_take_sync would hold)
  rig.view().edit.take_open = true;
  rig.view().play.playing = true;
  rig.view().play.pattern = rig.view().edit.pattern;
  rig.view().instrument = 1;
  // an instrument parameter is project-scoped: the binding goes to the project's store (Editor::set_learn_slot routes by scope)
  NEED(rig.editor->set_learn_slot(0, core::LearnBinding{core::param_id(ParamKind::Instrument, 0, ip::Cutoff), 74, 0,
                                                        static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  const unsigned depth = rig.depth();
  // the control: a Learn binding on the selected instrument's CUTOFF records its step FX in the open take
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
  CHECK(rig.view().edit.take_wrote);
  // the mapped CC: the parameter moves at once, and nothing joins the take
  const std::size_t open_before = rig.editor->journal().open_bytes();
  CHECK(rig.midicc(1, 71, 127) == DispatchResult::Changed);
  CHECK(rig.value(track(0)) == 100.0f);
  CHECK(rig.editor->journal().open_bytes() == open_before);  // no FX step was written into the take
  CHECK(rig.depth() == depth + 1);                           // the control's unit is the only one: the mapped CC was final
  CHECK(rig.view().edit.take_open);                          // midi.cc records: the take stayed open
  rig.view().edit.take_open = false;  // closed here: ~App ends a take on the editor, and the Rig destroys the editor before the app
}

TEST_CASE("cc map app: CC 41..48 queue a pattern on one track while playing, on the Perform slot page (S4) — the dedupe, the refused pattern, the stop") {
  Rig rig;
  rig.cc_map_on();
  RecSink sink;
  rig.perform->out = &sink;  // the board's commands are kept here
  audio::PlayPosition pos;   // what the render would publish
  rig.app->attach_play_position(&pos);
  NEED(rig.project().create_pattern(5, 16) == model::Status::Ok);  // patterns 5 and 6 exist; 200 does not
  NEED(rig.project().create_pattern(6, 16) == model::Status::Ok);
  NEED(rig.app->state().screen == app::ScreenId::Pattern);
  // stopped: nothing is sent
  CHECK(rig.midicc(1, 41, 4) != DispatchResult::Changed);
  CHECK(sink.cmds.empty());
  // playing, but on the Pattern screen: the Perform CCs act on the slot page only (Q5 a)
  pos.publish(true, 1, 0, 12000u);
  rig.pass();
  NEED(rig.view().play.playing);
  CHECK(rig.midicc(1, 44, 4) != DispatchResult::Changed);
  CHECK(sink.cmds.empty());
  rig.tap(Button::Performance);
  NEED(rig.app->state().screen == app::ScreenId::Perform);
  NEED(!rig.view().dj_page());
  // CC 41 = 4: pattern 5 queued on track 1 only, one QueueTrackPattern with mask 1
  CHECK(rig.midicc(1, 41, 4) == DispatchResult::Changed);
  NEED(sink.cmds.size() == 1u);
  CHECK(sink.cmds[0].kind == seq::Command::Kind::QueueTrackPattern);
  CHECK(sink.cmds[0].b == 5u);
  CHECK(sink.cmds[0].c == 1u);
  // the same value again sends nothing: the queue is not taken back (F12)
  CHECK(rig.midicc(1, 41, 4) != DispatchResult::Changed);
  CHECK(sink.cmds.size() == 1u);
  // a changed value sends again: 5 then 4 sends twice
  CHECK(rig.midicc(1, 41, 5) == DispatchResult::Changed);
  CHECK(rig.midicc(1, 41, 4) == DispatchResult::Changed);
  NEED(sink.cmds.size() == 3u);
  CHECK(sink.cmds[1].b == 6u);
  CHECK(sink.cmds[2].b == 5u);
  // another track: CC 42 is track 2's bit alone (the affected mask is ignored, A-5)
  CHECK(rig.midicc(1, 42, 4) == DispatchResult::Changed);
  CHECK(sink.cmds.back().c == 2u);
  // a pattern the project does not have: nothing sent, the strip says so (A-8)
  const std::size_t n = sink.cmds.size();
  CHECK(rig.midicc(1, 41, 9) != DispatchResult::Changed);  // CC 41 = 9: pattern 10 is not in the project
  CHECK(sink.cmds.size() == n);
  CHECK(std::strstr(rig.view().notify.last_text(), "CC 41: NO PAT 010") != nullptr);
  // the DJ page: no
  rig.tap(Button::Performance);
  NEED(rig.view().dj_page());
  CHECK(rig.midicc(1, 43, 4) != DispatchResult::Changed);
  CHECK(sink.cmds.size() == n);
  rig.tap(Button::Performance);  // back to the slot page
  NEED(!rig.view().dj_page());
  CHECK(rig.midicc(1, 43, 4) == DispatchResult::Changed);  // CC 43: track 3
  CHECK(sink.cmds.back().c == 4u);
  // stop: nothing is sent, and the dedupe is cleared when the run stops
  pos.publish(false, 0, 0, 0);
  rig.pass();
  NEED(!rig.view().play.playing);
  CHECK(rig.midicc(1, 41, 4) != DispatchResult::Changed);
  CHECK(sink.cmds.size() == n + 1u);
  // play again: the same value is sent again
  pos.publish(true, 1, 0, 12000u);
  rig.pass();
  NEED(rig.view().play.playing);
  CHECK(rig.midicc(1, 41, 4) == DispatchResult::Changed);
  NEED(sink.cmds.size() == n + 2u);
  CHECK(sink.cmds.back().kind == seq::Command::Kind::QueueTrackPattern);
  CHECK(sink.cmds.back().b == 5u);
  CHECK(sink.cmds.back().c == 1u);
}

TEST_CASE("cc map app: after the real Stop and Play keys, CC 41 sends the same value again (S4) — the clear follows the sequencer's stop, not the badge") {
  Rig rig;
  rig.cc_map_on();
  RecSink sink;
  rig.perform->out = &sink;  // the board's commands are kept here
  audio::PlayPosition pos;   // what the render would publish
  rig.app->attach_play_position(&pos);
  FakeTransport transport;  // what the real Play key asks
  rig.app->attach_transport(&transport);
  NEED(rig.project().create_pattern(5, 16) == model::Status::Ok);
  NEED(rig.app->state().screen == app::ScreenId::Pattern);
  rig.tap(Button::Performance);
  NEED(rig.app->state().screen == app::ScreenId::Perform);
  NEED(!rig.view().dj_page());
  // the real Play key: the badge goes up at once, the sequencer follows a block later
  rig.tap(Button::Play);
  CHECK(rig.view().playing);
  CHECK(transport.plays == 1u);
  pos.publish(true, 1, 0, 12000u);
  rig.pass();
  NEED(rig.view().play.playing);
  // CC 41 = 4: pattern 5 queued on track 1 only
  CHECK(rig.midicc(1, 41, 4) == DispatchResult::Changed);
  NEED(sink.cmds.size() == 1u);
  CHECK(sink.cmds[0].kind == seq::Command::Kind::QueueTrackPattern);
  CHECK(sink.cmds[0].b == 5u);
  CHECK(sink.cmds[0].c == 1u);
  // the real Stop key: the badge goes down at once (transport_play), the sequencer's stop arrives a block later
  rig.tap(Button::Play);
  CHECK_FALSE(rig.view().playing);
  CHECK(transport.stops == 1u);
  pos.publish(false, 0, 0, 0);
  rig.pass();
  NEED(!rig.view().play.playing);
  // stopped: nothing is sent
  CHECK(rig.midicc(1, 41, 4) != DispatchResult::Changed);
  CHECK(sink.cmds.size() == 1u);
  // the real Play key again, and the sequencer follows
  rig.tap(Button::Play);
  CHECK(rig.view().playing);
  CHECK(transport.plays == 2u);
  pos.publish(true, 1, 0, 12000u);
  rig.pass();
  NEED(rig.view().play.playing);
  // the same value is sent again: the dedupe was cleared when the run stopped, not when the badge fell
  CHECK(rig.midicc(1, 41, 4) == DispatchResult::Changed);
  NEED(sink.cmds.size() == 2u);
  CHECK(sink.cmds[1].kind == seq::Command::Kind::QueueTrackPattern);
  CHECK(sink.cmds[1].b == 5u);
  CHECK(sink.cmds[1].c == 1u);
}
