// SPDX-License-Identifier: GPL-3.0-or-later
// OS-004d: the runtime side of the project-level state (app/project_state.h): the mixer board and the Perform bank copied into the project when they change and back when a project is loaded
// (App::document_replaced), the MIDI conversions, the stem map and the USB apply state machine. The format side is project_state_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/app.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/project_state.h>
#include <o3os/audio/graph_params.h>
#include <o3os/core/input_event.h>
#include <o3os/midi/config.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/midi_link.h>
#include <o3os/seq/perform.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <memory>
#include <vector>

using namespace o3os;
namespace gp = audio::graph_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

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
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->attach_mix(board.get());
    app->attach_perform(perform.get());
    app->run_once();
    (void)sent();
  }
  model::Project& project() { return test::arena(1).project; }
  void pass() {
    clock.advance(16 * core::kTicksPerMs);
    app->run_once();
  }
  std::vector<core::ParamChange> sent() {
    std::vector<core::ParamChange> out;
    core::ParamChange c;
    while (ring.pop(c)) {
      out.push_back(c);
    }
    return out;
  }
};

bool has(const std::vector<core::ParamChange>& v, core::ParamId id, float value) {
  for (const core::ParamChange& c : v) {
    if (c.id == id && c.value == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("project state (app): the board starts as the model's defaults, and mix_apply tells the engine only what changed") {
  app::MixBoard board;
  model::MixState def;
  model::init_mix_state(def);
  CHECK(app::mix_state_of(board) == def);
  core::ParamRing ring;
  CHECK(app::mix_apply(board, def, &ring) == 0u);  // already there: nothing is sent
  core::ParamChange c;
  CHECK_FALSE(ring.pop(c));
  model::MixState s = def;
  s.route[3] = 1 | 2;
  s.route[16] = 1;
  s.ret_route[1] = 3;
  s.out_mode = 1;
  s.cue_level = 40;
  s.in_level = 80;
  CHECK(app::mix_apply(board, s, &ring) == 6u);
  CHECK(app::mix_state_of(board) == s);
  std::vector<core::ParamChange> sent;
  while (ring.pop(c)) {
    sent.push_back(c);
  }
  CHECK(sent.size() == 6u);
  CHECK(has(sent, audio::strip_param(3, gp::strip::Route), 3.0f));
  CHECK(has(sent, audio::strip_param(audio::kInputStrip, gp::strip::Route), 1.0f));
  CHECK(has(sent, audio::master_param(gp::master::RetRouteB), 3.0f));
  CHECK(has(sent, audio::master_param(gp::master::OutMode), 1.0f));
  CHECK(has(sent, audio::master_param(gp::master::CueLevel), 40.0f));
  CHECK(has(sent, audio::strip_param(audio::kInputStrip, gp::strip::Volume), 80.0f));
  CHECK(app::mix_apply(board, def, nullptr) == 6u);  // without a ring the board still follows
  CHECK(app::mix_state_of(board) == def);
}

TEST_CASE("project state (app): a change of the mixer board is the project's at the next pass, keeps the undo history, and a load puts it back") {
  Rig rig;
  model::Project& p = rig.project();
  const model::MixState def = p.mix();
  CHECK(p.state_present() == 0);  // nothing yet: the boot state is not a change
  CHECK(rig.editor->set_name("Song", 4) == model::Status::Ok);
  CHECK(rig.editor->can_undo());
  rig.board->route[2] = 1 | 4;
  rig.board->cue_level = 25;
  rig.pass();
  CHECK((p.state_present() & model::Project::kStateMix) != 0);
  CHECK(p.mix().route[2] == 5);
  CHECK(p.mix().cue_level == 25);
  CHECK(rig.editor->can_undo());  // not an edit behind the editor's back
  const std::uint32_t generation = p.generation();
  rig.pass();
  CHECK(p.generation() == generation);  // nothing changed: nothing written
  CHECK(rig.sent().empty());            // and the engine is told nothing by the copy into the project

  // a project is loaded: its mixer part goes to the board and the engine; the one before does not leak into a project that has none
  model::MixState other = def;
  other.route[0] = 1;
  other.out_mode = 1;
  p.reset();
  CHECK(p.set_mix(other) == model::Status::Ok);
  rig.app->document_replaced();
  CHECK(rig.board->route[0] == 1);
  CHECK(rig.board->out_mode == 1);
  CHECK(rig.board->route[2] == def.route[2]);
  CHECK(rig.board->cue_level == 100);
  std::vector<core::ParamChange> sent = rig.sent();
  CHECK(has(sent, audio::strip_param(0, gp::strip::Route), 1.0f));
  CHECK(has(sent, audio::master_param(gp::master::OutMode), 1.0f));
  CHECK(has(sent, audio::master_param(gp::master::CueLevel), 100.0f));  // back to the default the previous project had changed
  const std::uint32_t after = p.generation();
  rig.pass();
  CHECK(p.generation() == after);  // the board equals the project: no write after a load
  p.reset();  // a project with no mixer part: the defaults
  rig.app->document_replaced();
  CHECK(app::mix_state_of(*rig.board) == def);
}

TEST_CASE("project state (app): the Perform slots are the project's, a load ends every FX, and values are held to their FX's range") {
  Rig rig;
  model::Project& p = rig.project();
  seq::PerformBank& bank = rig.perform->bank;
  CHECK(bank.state() == p.perf());
  struct Sink final : seq::CommandSink {
    std::vector<seq::Command> got;
    bool push(const seq::Command& c) noexcept override {
      got.push_back(c);
      return true;
    }
  } sink;
  CHECK(bank.change_fx(0, 1, sink));  // slot 0: Volume -> Panning
  bank.pad_down(0, sink);             // row 0 of slot 0 is "off"; pad 12 is row 1 of slot 0
  bank.pad_up(0, sink);
  bank.pad_down(12, sink);
  bank.turn(5, sink);
  bank.pad_up(12, sink);
  const model::PerfState now = bank.state();
  CHECK(now.fx[0] == static_cast<std::uint8_t>(seq::PerfFx::Panning));
  CHECK(now.row[0] == 1);
  rig.pass();
  CHECK((p.state_present() & model::Project::kStatePerf) != 0);
  CHECK(p.perf() == now);
  CHECK(rig.editor->in_sync());

  // a load: every FX ends, no track is affected, the values come from the project
  CHECK(bank.toggle_track(0, sink));
  CHECK(bank.affected(0));
  model::PerfState s;
  model::init_perf_state(s);
  s.fx[3] = static_cast<std::uint8_t>(seq::PerfFx::Tune);  // -48..48
  s.row[3] = 2;
  s.value[3][1] = 100;                                      // above Tune's range: held to it
  s.value[3][0] = -7;
  s.fx[11] = static_cast<std::uint8_t>(seq::PerfFx::PatternLength);  // 0..8
  s.value[11][2] = -50;
  p.reset();
  CHECK(p.set_perf(s) == model::Status::Ok);
  rig.app->document_replaced();
  CHECK_FALSE(bank.affected(0));
  CHECK(bank.edit() == false);
  CHECK(bank.fx(3) == seq::PerfFx::Tune);
  CHECK(bank.row(3) == 2);
  CHECK(bank.stored(3, 2) == 48);
  CHECK(bank.stored(3, 1) == -7);
  CHECK(bank.stored(11, 3) == 0);
  CHECK(bank.live(3) == 48);
  CHECK_FALSE(bank.held(3));
  // the bank holds what it clamped, and the load already wrote that into the project (OS-FU5: before the unsaved baseline), as state: no undo unit, and the pass changes nothing
  CHECK(p.perf() == bank.state());
  CHECK(p.perf().value[3][1] == 48);
  const std::uint32_t generation = p.generation();
  rig.pass();
  CHECK(p.generation() == generation);
  CHECK(p.perf() == bank.state());
  CHECK(p.perf().value[3][1] == 48);
  // an FX id the table does not have becomes none
  model::PerfState odd = bank.state();
  odd.fx[5] = 19;
  bank.load_slots(odd);
  CHECK(bank.fx(5) == seq::PerfFx::PatternLength);
  p.reset();
  rig.app->document_replaced();  // a project with no Perform part: our defaults
  model::PerfState def;
  model::init_perf_state(def);
  CHECK(bank.state() == def);
}

TEST_CASE("project state (app): the MIDI part: what a project stores, what the device keeps, and what a load applies") {
  midi::MidiConfig cfg;
  model::MidiState def;
  model::init_midi_state(def);
  CHECK(seq::project_midi_state(cfg) == def);  // the config's defaults are the model's
  cfg.mode = midi::SyncMode::Slave;
  cfg.in_port = midi::Port::Usb;
  cfg.sync_correction = -3;
  cfg.out_ports = 3;
  cfg.note_ports = 2;
  cfg.notes_channel = 9;
  cfg.follow = midi::FollowTightness::Loose;
  cfg.cc_number[1] = 71;
  cfg.in_offset_dms[1] = -250;
  cfg.out_offset_dms[0] = 1000;
  const model::MidiState s = seq::project_midi_state(cfg);
  CHECK(s.out_ports == 3);
  CHECK(s.follow == 2);
  CHECK(s.in_offset_dms[1] == -250);
  CHECK(model::validate_midi_state(s) == nullptr);
  // applying it over a config of the device: the project's fields replace theirs; mode, followed port and the PAR-119 correction stay the device's
  midi::MidiConfig device;
  device.mode = midi::SyncMode::Master;
  device.in_port = midi::Port::Din;
  device.sync_correction = 4;
  const midi::MidiConfig applied = seq::apply_project_midi(device, s);
  CHECK(applied.mode == midi::SyncMode::Master);
  CHECK(applied.in_port == midi::Port::Din);
  CHECK(applied.sync_correction == 4);
  CHECK(applied.out_ports == 3);
  CHECK(applied.note_ports == 2);
  CHECK(applied.notes_channel == 9);
  CHECK(applied.follow == midi::FollowTightness::Loose);
  CHECK(applied.cc_number[1] == 71);
  CHECK(applied.cc_number[0] == midi::kCcUnassigned);
  CHECK(applied.in_offset_dms[1] == -250);
  CHECK(applied.out_offset_dms[0] == 1000);
  CHECK(seq::project_midi_state(applied) == s);
}

TEST_CASE("project state (app): the stem map and the USB apply: mode and profile first, the masks after, once, never forever") {
  audio::StemMap map;
  model::UsbState def;
  model::init_usb_state(def);
  CHECK(app::usb_state_of(map, usb::Mode::Standard) == def);  // the factory map of the default profile is the model's default
  for (usb::Profile p : {usb::Profile::P2, usb::Profile::P8, usb::Profile::P10, usb::Profile::P11, usb::Profile::P4, usb::Profile::P6}) {
    CHECK(map.reset(p));
    const model::UsbState s = app::usb_state_of(map, usb::Mode::Audio);
    CHECK(model::validate_usb_state(s) == nullptr);
    CHECK(s.mode == 1);
    CHECK(s.slots == usb::profile_channels(p));
    audio::StemMap other;
    CHECK(other.reset(p));
    CHECK(other.set_slot(0, 0x4001));
    CHECK(app::stem_map_apply(other, s));  // the factory slots back over a map that had been edited
    CHECK(app::usb_state_of(other, usb::Mode::Audio) == s);
  }
  CHECK(map.reset(usb::Profile::P8));
  model::UsbState custom = app::usb_state_of(map, usb::Mode::Audio);
  custom.mask[2] = 0x3003;
  audio::StemMap p10;  // the default profile (P10): the state is for P8
  CHECK_FALSE(app::stem_map_apply(p10, custom));
  CHECK(app::stem_map_apply(map, custom));
  CHECK(map.slot(2) == 0x3003);
  CHECK(app::usb_state_of(map, usb::Mode::Audio) == custom);

  using Act = app::UsbApply::Act;
  app::UsbApply apply;
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::None);  // nothing pending
  // already in the project's mode and profile: only the masks
  apply.begin(custom);
  CHECK(apply.pending());
  CHECK(apply.step(usb::Mode::Audio, usb::Profile::P8, 0) == Act::Masks);
  CHECK_FALSE(apply.pending());
  CHECK(apply.step(usb::Mode::Audio, usb::Profile::P8, 0) == Act::None);  // once
  // another mode: a request, a wait while the driver re-enumerates, then the masks
  apply.begin(custom);
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::Request);
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 1) == Act::None);  // detaching
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 2) == Act::None);  // detached
  CHECK(apply.pending());
  CHECK(apply.step(usb::Mode::Audio, usb::Profile::P8, 0) == Act::Masks);
  CHECK_FALSE(apply.pending());
  // the driver ends in something else: given up after the one request
  apply.begin(custom);
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::Request);
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::None);
  CHECK_FALSE(apply.pending());
  // abandoned by the platform (the driver refused the request)
  apply.begin(custom);
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::Request);
  apply.abandon();
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::None);
  // a profile change in STANDARD (no re-enumeration): the driver is at the target at the next pass
  model::UsbState standard_p11 = app::usb_state_of(map, usb::Mode::Standard);
  CHECK(map.reset(usb::Profile::P11));
  standard_p11 = app::usb_state_of(map, usb::Mode::Standard);
  apply.begin(standard_p11);
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::Request);
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P11, 0) == Act::Masks);
}

// OS-USBAUDIO: a loaded project's USB part never decides the MODE: the unit presents the setting USB AUDIO's from boot (Off must persist, On must not enumerate twice), and a runtime `usb mode` stays
// too. So the target keeps the mode the driver presents now (the platform passes the pending one while a change runs); the project brings the profile and the stem map.
TEST_CASE("project state (app): the USB part of a project asks for the mode the unit presents, with its own profile and stem map") {
  audio::StemMap map;
  CHECK(map.reset(usb::Profile::P10));
  model::UsbState st = app::usb_state_of(map, usb::Mode::Audio);
  st.mask[3] = 0x00F0;
  const model::UsbState to_standard = app::usb_apply_target(st, usb::Mode::Standard);  // the unit is in STANDARD (USB AUDIO Off): the project saved in AUDIO does not switch it to it
  CHECK(to_standard.mode == static_cast<std::uint8_t>(usb::Mode::Standard));
  CHECK(to_standard.profile == st.profile);
  CHECK(to_standard.slots == st.slots);
  CHECK(to_standard.mask[3] == 0x00F0);
  const model::UsbState to_audio = app::usb_apply_target(app::usb_state_of(map, usb::Mode::Standard), usb::Mode::Audio);  // in AUDIO: one saved in STANDARD does not switch it away
  CHECK(to_audio.mode == static_cast<std::uint8_t>(usb::Mode::Audio));
  CHECK(to_audio.profile == st.profile);
  // Applied to a unit that is already in the mode and profile, the project asks for nothing but its masks: no re-enumeration.
  app::UsbApply apply;
  apply.begin(app::usb_apply_target(st, usb::Mode::Audio));
  CHECK(apply.step(usb::Mode::Audio, usb::Profile::P10, 0) == app::UsbApply::Act::Masks);
  apply.begin(app::usb_apply_target(st, usb::Mode::Standard));
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == app::UsbApply::Act::Masks);
  // A different profile in the unit's mode: a request that does not change the mode (a profile in STANDARD is only remembered; in AUDIO it re-enumerates, as before).
  model::UsbState p8 = st;
  CHECK(map.reset(usb::Profile::P8));
  p8 = app::usb_state_of(map, usb::Mode::Audio);
  apply.begin(app::usb_apply_target(p8, usb::Mode::Standard));
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == app::UsbApply::Act::Request);
}

// A load while the unit's mode differs from the setting's (the setting USB AUDIO is Off, so the unit booted in STANDARD; `usb mode audio` then put it in AUDIO): the load leaves it where it is.
TEST_CASE("project state (app): a load while the mode differs from the setting (a runtime `usb mode`) does not undo it") {
  audio::StemMap map;
  CHECK(map.reset(usb::Profile::P10));
  using Act = app::UsbApply::Act;
  const model::UsbState saved_standard = app::usb_state_of(map, usb::Mode::Standard);
  const model::UsbState saved_audio = app::usb_state_of(map, usb::Mode::Audio);
  app::UsbApply apply;
  // The unit was put in AUDIO at run time: a project saved in STANDARD does not switch it back (no Request, only the masks) ...
  apply.begin(app::usb_apply_target(saved_standard, usb::Mode::Audio));
  CHECK(apply.step(usb::Mode::Audio, usb::Profile::P10, 0) == Act::Masks);
  // ... and, with the unit put in STANDARD at run time, one saved in AUDIO does not switch it to it.
  apply.begin(app::usb_apply_target(saved_audio, usb::Mode::Standard));
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 0) == Act::Masks);
  // While a change to AUDIO runs (the platform passes the pending mode), the apply waits for it and then finds the mode it asked for.
  apply.begin(app::usb_apply_target(saved_standard, usb::Mode::Audio));
  CHECK(apply.step(usb::Mode::Standard, usb::Profile::P10, 1) == Act::None);
  CHECK(apply.step(usb::Mode::Audio, usb::Profile::P10, 0) == Act::Masks);
}

TEST_CASE("project state (app): instrument.engine sets the engine of the instrument in view as one undo unit, and -1 steps through the table") {
  Rig rig;
  model::Project& p = rig.project();
  int slot = -1;
  for (unsigned i = 0; i < model::kMaxInstruments && slot < 0; ++i) {
    if (p.instrument(i) != nullptr && p.instrument(i)->kind == model::InstrumentKind::Sample) {
      slot = static_cast<int>(i);
    }
  }
  NEED(slot >= 0);
  (void)rig.app->invoke(core::action_id("instrument.select"), slot + 1);
  {
    model::Instrument at_paula = *p.instrument(static_cast<unsigned>(slot));
    at_paula.sample_rate = 8287;  // a rate Paula can play at the unity note (OS-FU5: an Amiga the rate cannot play is refused)
    NEED(rig.editor->set_instrument(static_cast<unsigned>(slot), at_paula) == model::Status::Ok);
  }
  const auto engine = [&]() { return p.instrument(static_cast<unsigned>(slot))->engine; };
  CHECK(engine() == model::kInstEngineAuto);
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), model::kInstEngineAmiga) == app::DispatchResult::Changed);
  CHECK(engine() == model::kInstEngineAmiga);
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), model::kInstEngineAmiga) == app::DispatchResult::Unchanged);
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), -1) == app::DispatchResult::Changed);
  CHECK(engine() == model::kInstEngineBits12);
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), -1) == app::DispatchResult::Changed);
  CHECK(engine() == model::kInstEngineBits8);
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), -1) == app::DispatchResult::Changed);  // wraps to automatic
  CHECK(engine() == model::kInstEngineAuto);
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), 5) == app::DispatchResult::Unchanged);  // not an engine
  CHECK(engine() == model::kInstEngineAuto);
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), model::kInstEngineBits8) == app::DispatchResult::Changed);
  CHECK(rig.editor->undo() == model::UndoResult::Ok);  // one unit
  CHECK(engine() == model::kInstEngineAuto);
  // a MIDI instrument has no engine to choose
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    if (p.instrument(i) != nullptr && p.instrument(i)->kind == model::InstrumentKind::Midi) {
      (void)rig.app->invoke(core::action_id("instrument.select"), static_cast<int>(i) + 1);
      (void)rig.app->invoke(core::action_id("instrument.engine"), model::kInstEngineAmiga);
      CHECK(p.instrument(i)->engine == model::kInstEngineAuto);
      break;
    }
  }
}

TEST_CASE("project state: the project's SENDS setting and the master limiter's two fields are told to the engine with the mutes and solos, so a loaded project's sends are pre- or post-fader and its limiter is as saved (OS-SENDFX, OS-LIMIT)") {
  model::Project& p = test::arena(1).project;
  sim::fill_demo_project(p);
  const core::ParamId id = core::param_id(core::ParamKind::Project, 0, model::project_setting::SendsMode);
  const core::ParamId mode_id = core::param_id(core::ParamKind::Project, 0, model::project_setting::LimiterMode);
  const core::ParamId switch_id = core::param_id(core::ParamKind::Master, 0, model::master_param::Limiter);
  CHECK(mode_id == core::param_id(core::ParamKind::Project, 0, gp::project::LimiterMode));
  CHECK(switch_id == audio::master_param(gp::master::Limiter));
  for (const float mode : {1.0f, 0.0f}) {
    CHECK(p.set_param(id, mode) == model::Status::Ok);
    CHECK(p.set_param(mode_id, mode) == model::Status::Ok);
    CHECK(p.set_param(switch_id, 1.0f - mode) == model::Status::Ok);
    core::ParamRing ring;
    CHECK(app::tracks_apply(p, &ring) == 2u * audio::kTracks + 6u);  // the mutes and solos, the SENDS setting, the LIMITER MODE and switch, and the three forgettings (the effects' memory, the instrument sends)
    unsigned found = 0, forgot = 0, mode_seen = 0, switch_seen = 0;
    core::ParamChange c;
    while (ring.pop(c)) {
      if (c.id == id) {
        ++found;
        CHECK(c.value == mode);
      }
      if (c.id == mode_id) {
        ++mode_seen;
        CHECK(c.value == mode);
      }
      if (c.id == switch_id) {
        ++switch_seen;
        CHECK(c.value == 1.0f - mode);
      }
      forgot += (c.id == audio::master_param(gp::master::DelayClear) || c.id == audio::master_param(gp::master::ReverbClear) || c.id == audio::master_param(gp::master::VoiceSendsReset)) ? 1u : 0u;
    }
    CHECK(found == 1u);
    CHECK(mode_seen == 1u);
    CHECK(switch_seen == 1u);
    CHECK(forgot == 3u);
  }
  CHECK(app::tracks_apply(p, nullptr) == 0u);  // no ring: nothing is sent
}
