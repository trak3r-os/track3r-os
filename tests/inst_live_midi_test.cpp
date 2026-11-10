// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S2b, the app side (O2 for MIDI): the Instrument screen's pads on a MIDI instrument, with a PreviewSink that does what the platform's does (a pad's note-on and its release on the
// live queue) in front of the real render path (Engine, MidiLink, MidiOut, the DIN emitter and the USB ring: support/live_midi_rig.h). Every way the instrument can change under a held pad
// (F6 / F7, Shift + F6 / F7, a pick from the Instr-hold list), a project load, a stop, a lost input and leaving the screen: every note-on on the wire has its note-off on the same
// channel and note, and none is sent twice. The render-level rules are audio_live_midi_test.cpp's.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cstdint>
#include <memory>

#include "support/live_midi_rig.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/seq_fixture.h"

using namespace o3os;
using namespace o3os::test;
using app::Button;
using core::ActionArg;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

// The platform's LivePreview in the part that matters here: a pad's note-on goes on the live queue as a pad note, its release (if the pad holds one) as an Off, an input reset as an all-notes-off.
struct QueueSink final : app::PreviewSink {
  audio::LiveBox* box = nullptr;
  std::uint64_t held = 0;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override {
    if (pad >= 48u) {
      return;
    }
    if (box->queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), static_cast<std::uint8_t>(note > 127u ? 127u : note),
                                         static_cast<std::uint8_t>(instrument), audio::kLiveVelocityDefault, static_cast<std::uint8_t>(track), 0})) {
      held |= std::uint64_t{1} << pad;
    }
  }
  void pad_up(unsigned pad) noexcept override {
    if (pad >= 48u || (held & (std::uint64_t{1} << pad)) == 0u) {
      return;
    }
    held &= ~(std::uint64_t{1} << pad);
    (void)box->queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), 0, 0, 0, 0, 0});
  }
  void notes_off(bool midi_only) noexcept override {
    if (!midi_only) {
      held = 0;
    }
    (void)box->queue.push(audio::LiveEvent{audio::LiveKind::AllOff, midi_only ? audio::kLiveSourceMidi : audio::kLiveAllSources, 0, 0, 0, 0, 0, 0});
  }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  LiveMidiRig live;
  QueueSink sink;

  Rig() {
    model::Project& p = project();
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    make_midi(6, 3);   // instrument 6: channel 3 (wire 2)
    make_midi(7, 16);  // instrument 7: channel 16 (wire 15)
    make_midi(9, 1);   // instrument 9: channel 1 (wire 0)
    live.init(p);
    sink.box = &live.box();
    app->attach_preview(&sink);
    app->run_once();
    tap(Button::Params);
  }

  model::Project& project() { return test::arena(1).project; }
  void make_midi(unsigned n, unsigned channel) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = static_cast<std::uint8_t>(channel);
    CHECK(project().set_instrument(n - 1u, in) == model::Status::Ok);
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass() {
    clock.advance(core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    press(b);
    pass();
    release(b);
    pass();
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass();
    tap(b);
    release(Button::Shift);
    pass();
  }
  void pad_down(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 127));
    pass();
  }
  void pad_up(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass();
  }
  void shift_pad(unsigned index) {
    press(Button::Shift);
    pass();
    pad_down(index);
    pad_up(index);
    release(Button::Shift);
    pass();
  }
  void select(int n) { (void)app->invoke(core::action_id("instrument.select"), static_cast<ActionArg>(n)); }
  unsigned pad_note(unsigned pad) const { return app->state().edit.pads.note(pad); }
};

}  // namespace

TEST_CASE("inst live midi: a pad on a MIDI instrument sends its note on the channel, and its release ends it") {
  Rig rig;
  rig.select(6);
  rig.pad_down(3);
  rig.live.blocks(3);
  NEED(rig.live.notes().size() == 1u);
  CHECK(rig.live.notes()[0].on);
  CHECK(rig.live.notes()[0].ch == 2);
  CHECK(rig.live.notes()[0].note == rig.pad_note(3));
  CHECK(rig.live.notes()[0].vel == 127);
  CHECK(rig.app->state().instrument == 6);
  rig.pad_up(3);
  rig.live.settle();
  NEED(rig.live.notes().size() == 2u);
  CHECK_FALSE(rig.live.notes()[1].on);
  CHECK(rig.live.notes()[1].ch == 2);
  CHECK(rig.live.notes()[1].note == rig.pad_note(3));
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(same_notes_on_both_ports(rig.live));
}

TEST_CASE("inst live midi: Shift + a pad plays a MIDI instrument like a plain pad and does not select (OS-SHIFTPAD)") {
  Rig rig;
  rig.select(7);  // channel 16
  rig.live.settle();
  CHECK(rig.live.notes().empty());
  rig.shift_pad(0);
  rig.live.blocks(3);
  CHECK(rig.app->state().instrument == 7);
  NEED(rig.live.notes().size() >= 1u);
  CHECK(rig.live.notes()[0].ch == 15);
  rig.live.settle();
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
}

TEST_CASE("inst live midi: every change of instrument under a held pad ends its MIDI note on the channel that started it (O2)") {
  struct Path {
    const char* name;
    void (*go)(Rig&);
  };
  const Path paths[] = {
      {"F7", [](Rig& r) { r.tap(Button::F7); }},
      {"F6", [](Rig& r) { r.tap(Button::F6); }},
      {"Shift + F7", [](Rig& r) { r.shift_tap(Button::F7); }},
      {"Shift + F6", [](Rig& r) { r.shift_tap(Button::F6); }},
      {"a pick from the list", [](Rig& r) { r.select(20); }},
  };
  for (const Path& path : paths) {
    CAPTURE(path.name);
    Rig rig;
    rig.select(7);  // channel 16 (wire 15); its neighbours 6 and 8: MIDI and a sample instrument
    rig.pad_down(2);
    rig.pad_down(9);
    rig.live.blocks(3);
    NEED(rig.live.notes().size() == 2u);
    path.go(rig);
    rig.live.settle();
    CHECK(rig.app->state().instrument != 7);
    CHECK(LiveMidiRig::held(rig.live.notes()) == 0);  // the notes ended with the change, before any release
    CHECK(LiveMidiRig::balanced(rig.live.notes()));
    const std::size_t after_change = rig.live.notes().size();
    CHECK(after_change == 4u);
    for (const WireNote& n : rig.live.notes()) {
      CHECK(n.ch == 15);  // the channel of the instrument that was in view when the pad went down
    }
    rig.pad_up(2);  // the fingers come off later: nothing more
    rig.pad_up(9);
    rig.live.settle();
    CHECK(rig.live.notes().size() == after_change);
    CHECK(same_notes_on_both_ports(rig.live));
  }
}

TEST_CASE("inst live midi: leaving the screen with a pad held, the release still ends the note") {
  Rig rig;
  rig.select(6);
  rig.pad_down(5);
  rig.live.blocks(3);
  CHECK(LiveMidiRig::held(rig.live.notes()) == 1);
  rig.tap(Button::Pattern);
  rig.live.blocks(3);
  rig.pad_up(5);
  rig.live.settle();
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(rig.live.notes().size() == 2u);
  CHECK(rig.live.notes()[1].ch == 2);
}

TEST_CASE("inst live midi: a project load under a held pad (the slot is another kind, another channel) still ends the note where it started") {
  Rig rig;
  rig.select(6);
  rig.pad_down(1);
  rig.live.blocks(3);
  CHECK(LiveMidiRig::held(rig.live.notes()) == 1);
  sim::fill_demo_project(rig.project());  // the load: slot 6 is not the MIDI instrument any more
  rig.app->run_once();
  rig.pad_up(1);
  rig.live.settle();
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(rig.live.notes().size() == 2u);
  CHECK(rig.live.notes()[1].ch == 2);
}

TEST_CASE("inst live midi: a lost input (App::reset_input) ends every pad note; the later releases add nothing") {
  Rig rig;
  rig.select(6);
  rig.pad_down(1);
  rig.pad_down(2);
  rig.live.blocks(3);
  CHECK(LiveMidiRig::held(rig.live.notes()) == 2);
  rig.app->reset_input();
  rig.live.settle();
  CHECK(LiveMidiRig::held(rig.live.notes()) == 0);
  rig.pad_up(1);
  rig.pad_up(2);
  rig.live.settle();
  CHECK(rig.live.notes().size() == 4u);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
}

TEST_CASE("inst live midi: Stop under a held pad is the router's panic: the note ends once") {
  Rig rig;
  CHECK(rig.project().create_pattern(200, 16) == model::Status::Ok);
  for (std::uint32_t k = 0; k < 16; ++k) {
    CHECK(rig.project().set_step(200, 0, k, make_step(static_cast<std::uint8_t>(40 + k), 9)) == model::Status::Ok);  // the sequencer plays the MIDI instrument 9 (wire channel 0)
  }
  CHECK(rig.project().append_order(200) == model::Status::Ok);
  rig.live.sequencer().play_pattern(200);
  rig.live.blocks(2);
  rig.select(6);
  rig.pad_down(4);
  rig.live.blocks(4);
  rig.live.sequencer().stop();
  rig.live.settle();
  CHECK(LiveMidiRig::held(rig.live.notes()) == 0);
  const std::size_t after_stop = rig.live.notes().size();
  rig.pad_up(4);
  rig.live.settle();
  CHECK(rig.live.notes().size() == after_stop);
  CHECK(LiveMidiRig::balanced(rig.live.notes()));
  CHECK(same_notes_on_both_ports(rig.live));
}
