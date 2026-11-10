// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LEARNMAP (docs/specs/OS-LEARNMAP.md): the LEARN MAP overlay, driven through its actions on the app rig of learn_follow_test.cpp. The edits (CC, CH, PARAMETER, INS, MODE), their refusals,
// F1 NEW, F2 MODE, F3 DELETE (the ENC row too), F4 CLEAR ALL twice, F5 FOLLOW, F7 RELEARN, the activity dot, undo, and that a binding the map wrote saves and loads like any other (no format change).
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/screens/screen.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::OverlayId;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

ActionArg cc(unsigned channel, unsigned controller, unsigned value) { return static_cast<ActionArg>((channel << 16) | (controller << 8) | value); }

constexpr unsigned kKeyNew = 1, kKeyMode = 2, kKeyDelete = 3, kKeyClearAll = 4, kKeyFollow = 5, kKeyRelearn = 7;  // lmap.key args (F1 .. F5, F7)
constexpr unsigned kColCc = 0, kColChannel = 1, kColParam = 2, kColIns = 3, kColMode = 4;

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;

  Rig() {
    sim::fill_demo_project(project());
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(project(), journal);
    app->attach_document(editor.get());
    app->attach_param_ring(&ring);
    app->run_once();
  }

  model::Project& project() { return test::arena(1).project; }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass(1);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass(1);
  }
  void invoke(const char* name, ActionArg arg = 0) { (void)app->invoke(core::action_id(name), arg); }
  std::string last_note() const {
    const core::NotifyQueue& q = app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  }
  const app::LearnMapView& map() const { return app->state().learn_map; }
  const core::LearnBinding& slot(unsigned i) { return project().settings().learn(i); }
  bool store_valid() { return core::learn_store_valid(project().settings().learn_slots(), core::kLearnSlots); }

  // A sample instrument's binding, as the Learn dialog makes it: CUTOFF of instrument 3 on CC 74, channel 1.
  void bind_cutoff() {
    invoke("instrument.select", 3);
    // (the Learn dialog binds the params row under the cursor: the Filter group's third row)
    tap(Button::Params);
    invoke("param.group", static_cast<ActionArg>(ip::GroupFilter));
    tap(Button::Down);
    tap(Button::Down);
    invoke("learn.toggle");
    pass(1);
    invoke("midi.cc", cc(1, 74, 64));
    invoke("learn.toggle");  // close
    pass(1);
    CHECK(project().settings().learn_count() == 1u);
    CHECK(slot(0).param == inst(2, ip::Cutoff));
  }
  // Opens the map with the cursor on binding row `row` (0 is the encoder's row) and the column `col`.
  void open_map(unsigned row, unsigned col) {
    invoke("lmap.open");
    pass(1);
    CHECK(app->state().overlay == OverlayId::LearnMap);
    for (unsigned i = 0; i < row; ++i) {
      invoke("nav.down");
    }
    for (unsigned i = 0; i < col; ++i) {
      invoke("nav.right");
    }
    CHECK(map().cursor == row);
    CHECK(map().column == col);
  }
  void make_midi(unsigned slot_index) {
    model::Instrument mi;
    model::init_instrument(mi, model::InstrumentKind::Midi);
    mi.midi_channel = 3;  // a MIDI instrument needs a channel
    std::memcpy(mi.name, "KEYS", 4);
    CHECK(project().set_instrument(slot_index, mi) == model::Status::Ok);
  }
};

bool midi_family(unsigned field) { return field >= ip::kMidiFirst && field < ip::VelSens; }

}  // namespace

TEST_CASE("learn map: it opens from the Learn dialog (F6 MAP) and from Config > MIDI (F8 MAP), and BACK (F8) closes it") {
  Rig rig;
  rig.invoke("learn.toggle");
  rig.pass(1);
  CHECK(rig.app->state().overlay == OverlayId::Learn);
  rig.tap(Button::F6);
  CHECK(rig.app->state().overlay == OverlayId::LearnMap);
  CHECK(rig.map().cursor == 0u);  // the encoder's row first
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == OverlayId::None);
  rig.tap(Button::Config);  // the MIDI page
  CHECK(rig.app->state().overlay == OverlayId::None);
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == OverlayId::LearnMap);
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == OverlayId::None);
  rig.tap(Button::F4);  // the INFO page: no firmware update path in this rig, so F8 does nothing there
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == OverlayId::None);
}

TEST_CASE("learn map: CC and CH columns edit the binding; a CC already bound on that channel is refused and says so") {
  Rig rig;
  rig.bind_cutoff();
  CHECK(rig.editor->set_learn_slot(1, core::LearnBinding{inst(2, ip::Volume), 20, 1, static_cast<std::uint8_t>(core::LearnMode::Pickup), 0}) == model::Status::Ok);
  rig.open_map(1, kColCc);
  const unsigned depth = rig.editor->journal().depth();
  rig.invoke("lmap.edit", 1);
  CHECK(rig.slot(0).cc == 75);
  CHECK(rig.slot(0).param == inst(2, ip::Cutoff));  // the same binding otherwise
  CHECK(rig.editor->journal().depth() == depth + 1u);  // one undo unit, as in the Learn dialog
  rig.invoke("lmap.coarse", 1);                      // Shift: coarse, 8 at a time
  CHECK(rig.slot(0).cc == 83);
  rig.invoke("lmap.edit", -9);
  CHECK(rig.slot(0).cc == 74);
  // CC 20 on channel 1 is slot 1's: row 1 may not take it.
  rig.invoke("lmap.edit", -54);
  CHECK(rig.slot(0).cc == 74);  // 74 - 54: a plain -54 lands on the taken number, refused, the row stays
}

TEST_CASE("learn map: a refusal leaves the store as it was") {
  Rig rig;
  rig.bind_cutoff();
  CHECK(rig.editor->set_learn_slot(1, core::LearnBinding{inst(2, ip::Volume), 20, 1, static_cast<std::uint8_t>(core::LearnMode::Pickup), 0}) == model::Status::Ok);
  rig.open_map(1, kColCc);
  const core::LearnBinding before = rig.slot(0);
  rig.invoke("lmap.edit", -54);  // CC 74 -> 20: slot 1 has CC 20 on channel 1
  CHECK(rig.last_note() == "CC 20 CH1 IS TAKEN");
  CHECK(std::memcmp(&rig.slot(0), &before, sizeof before) == 0);
  CHECK(rig.store_valid());
  // The same through CH: channel 0 (every channel) overlaps CC 20 of channel 1 only when the CC is the same, so CH changes freely here.
  rig.invoke("nav.right");
  CHECK(rig.map().column == kColChannel);
  rig.invoke("lmap.edit", 1);
  CHECK(rig.slot(0).channel == 2);
  rig.invoke("lmap.edit", -5);
  CHECK(rig.slot(0).channel == 0);
}

TEST_CASE("learn map: PARAMETER walks only the parameters the instrument's kind has and that are Learn-able") {
  Rig rig;
  rig.bind_cutoff();
  rig.open_map(1, kColParam);
  unsigned moved = 0;
  for (unsigned i = 0; i < 40; ++i) {
    const ParamId before = rig.slot(0).param;
    rig.invoke("lmap.edit", 1);
    const ParamId after = rig.slot(0).param;
    CHECK(core::param_kind(after) == ParamKind::Instrument);
    CHECK(core::param_index(after) == 2u);  // the same instrument: a sample one
    CHECK_FALSE(midi_family(core::param_field(after)));
    CHECK_FALSE(ip::is_module_field(core::param_field(after)));
    if (after != before) {
      ++moved;
      CHECK(core::param_field(after) > core::param_field(before));
    }
  }
  CHECK(moved > 3u);
  CHECK(rig.store_valid());
  CHECK(rig.project().settings().learn_count() == 1u);  // a walk never adds a binding
  for (unsigned i = 0; i < 80; ++i) {                     // and back
    rig.invoke("lmap.edit", -1);
    CHECK_FALSE(midi_family(core::param_field(rig.slot(0).param)));
  }
  CHECK(core::param_field(rig.slot(0).param) < core::param_field(inst(2, ip::Cutoff)));
}

TEST_CASE("learn map: F1 NEW binds the next CC moved to the selected instrument's first free parameter, a MIDI instrument's from its own rows") {
  Rig rig;
  rig.make_midi(4);
  rig.invoke("instrument.select", 5);
  rig.open_map(0, 0);
  rig.invoke("lmap.key", kKeyNew);
  CHECK(rig.map().wait == app::LearnMapView::kWaitNew);
  rig.invoke("midi.cc", cc(2, 33, 10));
  CHECK(rig.map().wait == 0u);
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.slot(0).cc == 33);
  CHECK(rig.slot(0).channel == 2);
  CHECK(core::param_index(rig.slot(0).param) == 4u);
  CHECK(midi_family(core::param_field(rig.slot(0).param)));
  CHECK(rig.map().cursor == 1u);  // the cursor is on the new row, on PARAMETER
  CHECK(rig.map().column == kColParam);
  // On a sample instrument the first parameter is a sample one.
  rig.invoke("instrument.select", 3);
  rig.invoke("lmap.key", kKeyNew);
  rig.invoke("midi.cc", cc(2, 34, 10));
  CHECK(rig.project().settings().learn_count() == 2u);
  CHECK(core::param_index(rig.slot(1).param) == 2u);
  CHECK_FALSE(midi_family(core::param_field(rig.slot(1).param)));
  // A CC that is taken is refused, and NEW waits no more.
  rig.invoke("lmap.key", kKeyNew);
  rig.invoke("midi.cc", cc(2, 34, 10));
  CHECK(rig.last_note() == "CC 34 CH2 IS TAKEN");
  CHECK(rig.project().settings().learn_count() == 2u);
  CHECK(rig.map().wait == 0u);
  CHECK(rig.store_valid());
}

TEST_CASE("learn map: INS pins a binding to an instrument; one that has no such parameter is refused; * follows the selected one") {
  Rig rig;
  rig.bind_cutoff();
  rig.make_midi(4);  // instrument 5 is a MIDI one now: no CUTOFF
  rig.open_map(1, kColIns);
  const core::LearnBinding before = rig.slot(0);
  rig.invoke("lmap.edit", 2);  // INS 3 -> 5
  CHECK(rig.last_note() == "INS 05 HAS NO CUTOFF");
  CHECK(std::memcmp(&rig.slot(0), &before, sizeof before) == 0);
  rig.invoke("lmap.edit", 1);  // INS 4: a sample instrument
  CHECK(rig.slot(0).param == inst(3, ip::Cutoff));
  rig.invoke("lmap.coarse", 1);  // Shift skips the MIDI instrument: INS 6
  CHECK(rig.slot(0).param == inst(5, ip::Cutoff));
  rig.invoke("lmap.edit", -6);  // down to * (follow)
  CHECK((rig.slot(0).flags & core::kLearnFollow) != 0);
  CHECK(rig.slot(0).param == inst(0, ip::Cutoff));  // a follow binding keeps index 0
  CHECK(rig.store_valid());
  CHECK(rig.project().settings().learn_count() == 1u);
}

TEST_CASE("learn map: F2 MODE cycles the mode, F5 FOLLOW toggles follow and pin like the Learn dialog, both one undo unit") {
  Rig rig;
  rig.bind_cutoff();
  rig.open_map(1, kColCc);
  const unsigned depth = rig.editor->journal().depth();
  rig.invoke("lmap.key", kKeyMode);
  CHECK(rig.slot(0).mode == static_cast<std::uint8_t>(core::LearnMode::Jump));
  rig.invoke("lmap.key", kKeyMode);
  CHECK(rig.slot(0).mode == static_cast<std::uint8_t>(core::LearnMode::Relative));
  rig.invoke("lmap.key", kKeyMode);
  CHECK(rig.slot(0).mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));
  CHECK(rig.editor->journal().depth() == depth + 3u);
  rig.invoke("lmap.key", kKeyFollow);
  CHECK((rig.slot(0).flags & core::kLearnFollow) != 0);
  CHECK(rig.slot(0).param == inst(0, ip::Cutoff));
  CHECK(rig.slot(0).cc == 74);
  rig.invoke("lmap.key", kKeyFollow);  // pinned back to the selected instrument (3)
  CHECK(rig.slot(0).flags == 0);
  CHECK(rig.slot(0).param == inst(2, ip::Cutoff));
  CHECK(rig.store_valid());
  // The mode column steps the mode too, clamped at both ends.
  rig.invoke("nav.right");
  rig.invoke("nav.right");
  rig.invoke("nav.right");
  rig.invoke("nav.right");
  CHECK(rig.map().column == kColMode);
  rig.invoke("lmap.edit", 5);
  CHECK(rig.slot(0).mode == static_cast<std::uint8_t>(core::LearnMode::Relative));
  rig.invoke("lmap.edit", -5);
  CHECK(rig.slot(0).mode == static_cast<std::uint8_t>(core::LearnMode::Pickup));
}

TEST_CASE("learn map: F5 FOLLOW is refused when the selected instrument has no such parameter (the binding would be dead)") {
  Rig rig;
  rig.bind_cutoff();
  rig.make_midi(4);
  rig.invoke("instrument.select", 5);  // a MIDI instrument: no CUTOFF
  rig.open_map(1, kColCc);
  const core::LearnBinding before = rig.slot(0);
  rig.invoke("lmap.key", kKeyFollow);
  CHECK(rig.last_note() == "INS 05 HAS NO CUTOFF");
  CHECK(std::memcmp(&rig.slot(0), &before, sizeof before) == 0);
  rig.invoke("instrument.select", 4);  // a sample instrument again: the toggle works
  rig.invoke("lmap.key", kKeyFollow);
  CHECK((rig.slot(0).flags & core::kLearnFollow) != 0);
}

TEST_CASE("learn map: a cursor left past the end by a shrunk list is pulled back before the key or the edit acts") {
  {
    Rig rig;
    rig.bind_cutoff();
    rig.open_map(1, kColCc);
    rig.invoke("learn.clear_all");  // the list shrinks under the open map: first press arms,
    rig.invoke("learn.clear_all");  // the second clears
    CHECK(rig.project().settings().learn_count() == 0u);
    rig.invoke("lmap.key", kKeyDelete);
    CHECK(rig.map().cursor == 0u);
    CHECK(rig.last_note() == "NO ENCODER TARGET");
  }
  {
    Rig rig;
    rig.bind_cutoff();
    rig.open_map(1, kColCc);
    rig.invoke("learn.clear_all");
    rig.invoke("learn.clear_all");
    rig.invoke("lmap.edit", 1);
    CHECK(rig.map().cursor == 0u);
    CHECK(rig.last_note() == "ENCODER HAS NO CC");
  }
}

TEST_CASE("learn map: F3 DELETE leaves the Learn dialog's own cursor where it was") {
  Rig rig;
  rig.bind_cutoff();
  rig.open_map(1, kColCc);
  rig.invoke("lmap.key", kKeyNew);
  rig.invoke("midi.cc", cc(1, 75, 64));
  CHECK(rig.project().settings().learn_count() == 2u);
  CHECK(rig.map().cursor == 2u);  // the new row, whose index differs from the dialog's cursor
  const unsigned dialog_cursor = rig.app->state().learn.cursor;
  CHECK(dialog_cursor != rig.map().cursor - 1u);
  rig.invoke("lmap.key", kKeyDelete);
  CHECK(rig.project().settings().learn_count() == 1u);
  CHECK(rig.app->state().learn.cursor == dialog_cursor);
}

TEST_CASE("learn map: F7 RELEARN makes the next CC moved the row's source; a taken one is refused") {
  Rig rig;
  rig.bind_cutoff();
  CHECK(rig.editor->set_learn_slot(1, core::LearnBinding{inst(2, ip::Volume), 20, 1, static_cast<std::uint8_t>(core::LearnMode::Pickup), 0}) == model::Status::Ok);
  rig.open_map(1, kColCc);
  rig.invoke("lmap.key", kKeyRelearn);
  CHECK(rig.map().wait == app::LearnMapView::kWaitRelearn);
  rig.invoke("midi.cc", cc(3, 41, 5));
  CHECK(rig.map().wait == 0u);
  CHECK(rig.slot(0).cc == 41);
  CHECK(rig.slot(0).channel == 3);
  CHECK(rig.slot(0).param == inst(2, ip::Cutoff));  // the parameter stays
  rig.invoke("lmap.key", kKeyRelearn);
  rig.invoke("midi.cc", cc(1, 20, 5));  // slot 1's
  CHECK(rig.last_note() == "CC 20 CH1 IS TAKEN");
  CHECK(rig.slot(0).cc == 41);
  CHECK(rig.map().wait == 0u);
  // Any other key takes the wait back.
  rig.invoke("lmap.key", kKeyRelearn);
  rig.invoke("lmap.key", kKeyMode);
  CHECK(rig.map().wait == 0u);
}

TEST_CASE("learn map: F4 CLEAR ALL asks first and clears on the second press; any other key takes the question back") {
  Rig rig;
  rig.bind_cutoff();
  CHECK(rig.editor->set_learn_slot(1, core::LearnBinding{inst(2, ip::Volume), 20, 1, static_cast<std::uint8_t>(core::LearnMode::Pickup), 0}) == model::Status::Ok);
  rig.open_map(2, kColCc);
  rig.invoke("lmap.key", kKeyClearAll);
  CHECK(rig.app->state().learn.confirm);
  CHECK(rig.project().settings().learn_count() == 2u);
  rig.invoke("lmap.key", kKeyMode);  // another key: the question goes
  CHECK_FALSE(rig.app->state().learn.confirm);
  rig.invoke("lmap.key", kKeyClearAll);
  rig.invoke("lmap.key", kKeyClearAll);
  CHECK(rig.project().settings().learn_count() == 0u);
  CHECK(rig.map().cursor == 0u);  // only the encoder's row is left
  CHECK(rig.store_valid());
}

TEST_CASE("learn map: F3 DELETE removes the binding; on the ENC row it clears the encoder target; the ENC row refuses the rest") {
  Rig rig;
  rig.bind_cutoff();
  rig.tap(Button::Params);
  rig.invoke("param.group", static_cast<ActionArg>(ip::GroupFilter));
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.invoke("enc.assign");
  CHECK(rig.app->state().enc_target == inst(2, ip::Cutoff));
  rig.open_map(0, kColCc);
  rig.invoke("lmap.edit", 1);
  CHECK(rig.last_note() == "ENCODER HAS NO CC");
  rig.invoke("lmap.key", kKeyMode);
  CHECK(rig.last_note() == "ENCODER IS ALWAYS RELATIVE");
  rig.invoke("lmap.key", kKeyFollow);
  CHECK(rig.last_note() == "FOLLOW: NOT FOR THE ENCODER");
  rig.invoke("lmap.key", kKeyDelete);
  CHECK(rig.app->state().enc_target == ParamId::None);
  CHECK(rig.project().settings().learn_count() == 1u);  // the binding stays
  rig.invoke("lmap.key", kKeyDelete);
  CHECK(rig.last_note() == "NO ENCODER TARGET");
  // A binding row: delete removes it and the cursor stays inside the list.
  rig.invoke("nav.down");
  rig.invoke("lmap.key", kKeyDelete);
  CHECK(rig.project().settings().learn_count() == 0u);
  CHECK(rig.map().cursor == 0u);
}

TEST_CASE("learn map: an edit is one undo record and undo gives the binding back; nothing else is journalled") {
  Rig rig;
  rig.bind_cutoff();
  rig.open_map(1, kColCc);
  const unsigned depth = rig.editor->journal().depth();
  rig.invoke("lmap.edit", 3);
  CHECK(rig.slot(0).cc == 77);
  CHECK(rig.editor->journal().depth() == depth + 1u);
  rig.invoke("lmap.edit", 0);  // a null turn is no edit
  CHECK(rig.editor->journal().depth() == depth + 1u);
  rig.tap(Button::F8);  // BACK
  CHECK(rig.app->state().overlay == OverlayId::None);
  rig.invoke("edit.undo");
  CHECK(rig.slot(0).cc == 74);
  CHECK(rig.slot(0).param == inst(2, ip::Cutoff));
  CHECK(rig.store_valid());
}

TEST_CASE("learn map: a CC that moves a bound parameter lights its dot, and the main loop puts it out 250 ms later") {
  Rig rig;
  rig.bind_cutoff();
  rig.open_map(1, kColCc);
  CHECK(rig.map().hit[0] == 0u);
  rig.invoke("midi.cc", cc(1, 74, 100));
  CHECK((rig.map().hit[0] & 1u) != 0u);  // project store, slot 0
  rig.pass(100);
  CHECK((rig.map().hit[0] & 1u) != 0u);
  rig.pass(200);
  CHECK(rig.map().hit[0] == 0u);
  // The dot is lit only while the map is open.
  rig.tap(Button::F8);
  rig.invoke("midi.cc", cc(1, 74, 10));
  CHECK(rig.map().hit[0] == 0u);
}

TEST_CASE("learn map: a binding the map wrote saves and loads like any other (LRNB, no format change)") {
  static_assert(sizeof(core::LearnBinding) == 8, "the binding's layout is the file's");
  Rig rig;
  rig.bind_cutoff();
  rig.open_map(1, kColCc);
  rig.invoke("lmap.edit", 5);
  rig.invoke("lmap.key", kKeyMode);
  rig.invoke("lmap.key", kKeyFollow);
  const core::LearnBinding saved = rig.slot(0);
  CHECK(saved.cc == 79);
  std::vector<std::uint8_t> bytes;
  CHECK(test::save(rig.project(), bytes) == formats::native::Progress::Done);
  model::ModelArena& other = test::arena(2);
  CHECK(test::load(other, bytes) == formats::native::Progress::Done);
  CHECK(other.project.settings().learn_count() == 1u);
  CHECK(std::memcmp(&other.project.settings().learn(0), &saved, sizeof saved) == 0);
  // The encoder's target is a session value: it is not in the file.
  CHECK(core::learn_store_valid(other.project.settings().learn_slots(), core::kLearnSlots));
}
