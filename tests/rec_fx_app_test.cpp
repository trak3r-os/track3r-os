// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FXREC S2 (docs/specs/OS-FXREC.md, the wiring of section 3.2 to 3.5) on the app rig with a playing sequencer stub: a MIDI Learn binding on a mapped instrument parameter writes its step
// FX at the play line of the cursor's track during a LIVE take, in the take's undo unit, with the strip messages; outside a take nothing differs from today. The pure map and column rule are
// rec_fx_test.cpp. Host only (the app, the host fakes and the play position stub).
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/mix_board.h>
#include <o3os/app/rec_fx.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument.h>
#include <o3os/model/param_auto.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/fx.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
using app::DispatchResult;
using app::PatternMode;
using core::ActionArg;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;
namespace tp = model::track_param;
namespace seq = o3os::seq;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

ActionArg cc(unsigned channel, unsigned controller, unsigned value) {
  return static_cast<ActionArg>((channel << 16) | (controller << 8) | value);
}

core::LearnBinding binding(unsigned slot, unsigned field, std::uint8_t num, core::LearnMode mode, std::uint8_t flags = 0) {
  return core::LearnBinding{inst(slot, field), num, 0, static_cast<std::uint8_t>(mode), flags};
}

// The app rig of rec_test.cpp with a document of our own: pattern 1 (16 steps, one position) and two sample instruments, slot 0 (Low-pass, cutoff 0.30) and slot 1 (High-pass).
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  model::UndoJournal journal;
  model::UndoHistory history;  // used only after three_rings(); declared before the editor so that it outlives it
  std::unique_ptr<model::Editor> editor;
  audio::PlayPosition pos;
  core::ParamRing ring;
  // Declared after the editor, so that it is destroyed before it: ~App closes the take's undo group on the editor (OS-LIVEREC).
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(unsigned length = 16, bool edit = true, std::size_t ring_bytes = sizeof g_ring) {
    model::Project& p = project();
    p.reset();
    (void)p.create_pattern(1, length);
    (void)p.append_order(1);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::memcpy(in.name, "TEST", 4);
    (void)p.set_instrument(0, in);
    (void)p.set_instrument(1, in);
    (void)p.set_param(inst(0, ip::Cutoff), 0.30f);
    (void)p.set_param(inst(0, ip::FilterType), 0.0f);
    (void)p.set_param(inst(1, ip::Cutoff), 0.30f);
    (void)p.set_param(inst(1, ip::FilterType), 1.0f);  // instrument 2 is High-pass
    journal.begin(g_ring, ring_bytes);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&pos);
    app->attach_param_ring(&ring);
    app->run_once();
    if (edit) {
      live();
    }
  }
  // The device's shape: one ring per area (PATTERN, SONG, SOUND) in place of the single ring of the other tests (OS-UNDOGRP).
  void three_rings() {
    NEED(history.begin(g_ring, sizeof g_ring));
    auto three = std::make_unique<model::Editor>(project(), history);
    app->attach_document(three.get());
    editor = std::move(three);
  }
  // Edit following, LIVE (OS-RECMODE2), entered as the tests always did.
  void live() { (void)app->invoke(core::action_id("view.mode"), static_cast<ActionArg>(PatternMode::Edit)); }
  model::Project& project() { return test::arena(2).project; }
  const app::ViewState& state() const { return app->state(); }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  // The sequencer is at `step` of pattern 1, `ustep` of its 48 microsteps played (or stopped).
  void play(unsigned step, unsigned ustep = 0, bool playing = true) {
    pos.publish(playing, 1, step, 12000, audio::PlayPosition::kNoOrder, ustep);
    pos.publish_count_in(0);
    pass();
  }
  void play_pattern(unsigned pattern, unsigned step, unsigned ustep = 0) {
    pos.publish(true, pattern, step, 12000, audio::PlayPosition::kNoOrder, ustep);
    pos.publish_count_in(0);
    pass();
  }
  void count_in(unsigned step, unsigned ustep) {
    pos.publish(true, 1, step, 12000, audio::PlayPosition::kNoOrder, ustep);
    pos.publish_count_in(2);
    pass();
  }
  void stop() { play(0, 0, false); }
  void press(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass();
  }
  void release(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void tap(Button b) {
    press(b);
    release(b);
  }
  void pad_down(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass();
  }
  void pad_up(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass();
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  DispatchResult midicc(unsigned channel, unsigned num, unsigned value) { return invoke("midi.cc", cc(channel, num, value)); }
  void bind(unsigned slot, unsigned field, std::uint8_t num, core::LearnMode mode, std::uint8_t flags = 0, unsigned learn_slot = 0) {
    NEED(editor->set_learn_slot(learn_slot, binding(slot, field, num, mode, flags)) == model::Status::Ok);
  }
  void bind_track(unsigned track, unsigned field, std::uint8_t num) {  // a Mixer parameter (A-7): the device's Learn store
    NEED(app->device_settings().set_learn(0, core::LearnBinding{core::param_id(ParamKind::Track, track, field), num, 0,
                                                                static_cast<std::uint8_t>(core::LearnMode::Jump), 0}) == model::Status::Ok);
  }
  const model::Step& step(int track, int row) { return project().pattern(1)->steps[track][row]; }
  unsigned depth() const { return journal.depth(); }
  int track() const { return app->view().track(); }
  void go_track(int t) { (void)invoke("cursor.track", t - track()); }
  float value(ParamId id) { return project().get_param(id); }
  // The newest notification in the queue.
  std::string last_note() const {
    const core::NotifyQueue& q = app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  }
};

// True when no step of the pattern holds any FX.
bool no_fx_anywhere(Rig& rig) {
  const model::Pattern* pat = rig.project().pattern(1);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    for (unsigned r = 0; r < model::kMaxPatternLength; ++r) {
      if (pat->steps[t][r].fx[0].type != model::kFxNone || pat->steps[t][r].fx[1].type != model::kFxNone) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

TEST_CASE("fxrec app: a bound CUTOFF knob records the filter FX at the play line of the cursor's track during LIVE; a second message on the row changes the value, not the column; Nearest past half the step writes the next row") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  rig.go_track(2);
  rig.play(3);
  CHECK(rig.state().edit.take_open);
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);  // the parameter moved
  CHECK(rig.step(2, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(2, 3).fx[0].value == 100);
  CHECK(rig.step(2, 3).fx[1].type == model::kFxNone);
  for (int t = 0; t < 12; ++t) {
    if (t != 2) {
      CHECK(rig.step(t, 3).fx[0].type == model::kFxNone);  // only the cursor's track
      CHECK(rig.step(t, 3).fx[1].type == model::kFxNone);
    }
  }
  for (int r = 0; r < 16; ++r) {
    if (r != 3) {
      CHECK(rig.step(2, r).fx[0].type == model::kFxNone);  // only the play line's row
    }
  }
  CHECK(rig.last_note().find("REC FX: CUTOFF") != std::string::npos);  // the take's first FX says the parameter
  // a second message on the same row overwrites the value in the same column: one value per step
  CHECK(rig.midicc(1, 74, 64) == DispatchResult::Changed);
  CHECK(rig.step(2, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(2, 3).fx[0].value == 50);  // round(64 / 127 * 100)
  CHECK(rig.step(2, 3).fx[1].type == model::kFxNone);
  // REC QUANTIZE Nearest past half the step writes the next row
  rig.play(3, 30);
  CHECK(rig.midicc(1, 74, 0) == DispatchResult::Changed);
  CHECK(rig.step(2, 4).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(2, 4).fx[0].value == 0);
  CHECK(rig.step(2, 3).fx[0].value == 50);  // the earlier row keeps its value
  // the same value again changes nothing and records nothing (the cell is unchanged)
  const unsigned d = rig.depth();
  rig.play(4, 30);
  CHECK(rig.midicc(1, 74, 0) == DispatchResult::Unchanged);  // the parameter is already 0: nothing moves, nothing records
  CHECK(rig.step(2, 5).fx[0].type == model::kFxNone);
  CHECK(rig.depth() == d);
}

TEST_CASE("fxrec app: the FX type follows the instrument's stored FILTER TYPE (High-pass writes 28, Band-pass 29), and the value is the stored one") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  (void)rig.project().set_param(inst(0, ip::FilterType), 1.0f);  // High-pass
  rig.play(5);
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 5).fx[0].type == seq::fx::kHighPass);
  CHECK(rig.step(0, 5).fx[0].value == 100);
  (void)rig.project().set_param(inst(0, ip::FilterType), 2.0f);  // Band-pass now
  rig.play(6);
  CHECK(rig.midicc(1, 74, 64) == DispatchResult::Changed);
  CHECK(rig.step(0, 6).fx[0].type == seq::fx::kBandPass);
  CHECK(rig.step(0, 6).fx[0].value == 50);
}

TEST_CASE("fxrec app: a Pickup binding that has not caught the knob writes nothing; once caught it records") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Pickup);  // the knob has to reach 0.30 first
  rig.play(3);
  CHECK(rig.midicc(1, 74, 100) == DispatchResult::Unchanged);  // not caught: nothing moves, nothing records
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.30f);
  CHECK(no_fx_anywhere(rig));
  CHECK(rig.midicc(1, 74, 38) == DispatchResult::Changed);  // 38 / 127 is 0.299: caught
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(0, 3).fx[0].value == 30);  // round(38 / 127 * 100)
}

TEST_CASE("fxrec app: a binding of instrument 2 with instrument 1 selected writes nothing (Q3 a); selected, it records; a follow binding follows") {
  Rig rig;
  rig.bind(1, ip::Cutoff, 75, core::LearnMode::Jump);  // a fixed binding on instrument 2's cutoff
  CHECK(rig.state().instrument == 1);
  rig.play(3);
  CHECK(rig.midicc(1, 75, 127) == DispatchResult::Changed);
  CHECK(rig.value(inst(1, ip::Cutoff)) == 1.0f);  // the parameter of instrument 2 moved...
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kCcA);  // ... recorded as a CC letter, not the cutoff FX (Q3 a is for own FX): instrument 1 is selected
  CHECK(rig.project().instrument(1)->midi_cc[0] == model::auto_code(inst(1, ip::Cutoff)));
  CHECK(rig.project().instrument(0)->midi_cc[0] == 0);
  (void)rig.invoke("instrument.select", 2);
  rig.play(4);
  CHECK(rig.midicc(1, 75, 64) == DispatchResult::Changed);
  CHECK(rig.step(0, 4).fx[0].type == seq::fx::kHighPass);  // instrument 2 is High-pass
  CHECK(rig.step(0, 4).fx[0].value == 50);
  // a follow binding acts on the selected instrument
  Rig f;
  f.bind(0, ip::Cutoff, 76, core::LearnMode::Jump, core::kLearnFollow);
  f.play(2);
  CHECK(f.midicc(1, 76, 127) == DispatchResult::Changed);
  CHECK(f.step(0, 2).fx[0].type == seq::fx::kLowPass);  // instrument 1 selected: its cutoff
  CHECK(f.value(inst(0, ip::Cutoff)) == 1.0f);
  (void)f.invoke("instrument.select", 2);
  f.play(6);
  CHECK(f.midicc(1, 76, 0) == DispatchResult::Changed);
  CHECK(f.step(0, 6).fx[0].type == seq::fx::kHighPass);  // now instrument 2's
  CHECK(f.value(inst(1, ip::Cutoff)) == 0.0f);
  CHECK(f.value(inst(0, ip::Cutoff)) == 1.0f);
}

TEST_CASE("fxrec app: no FX when Browse, stopped, STEP, counting in, or the playhead is in another pattern; the parameter still moves") {
  {
    Rig rig(16, false);  // Browse: no Edit
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.play(3);
    CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
    CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig;  // LIVE mode, but the transport is stopped: no take
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.stop();
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 64) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig(16, false);
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.tap(Button::Rec);  // WATCH -> STEP (Edit free): the playhead does not record
    rig.play(5);
    CHECK(rig.state().edit.rec_free);
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 100) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig;
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.count_in(0, 10);  // counting in: nothing is recorded yet
    CHECK_FALSE(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 90) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig;
    rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
    rig.play(7);
    CHECK(rig.state().edit.take_open);
    rig.play_pattern(2, 7);  // the playhead is in another pattern
    CHECK(rig.midicc(1, 74, 80) == DispatchResult::Changed);
    CHECK(no_fx_anywhere(rig));
    rig.play(9);  // back in pattern 1: it records again
    CHECK(rig.state().edit.take_open);
    CHECK(rig.midicc(1, 74, 70) == DispatchResult::Changed);
    CHECK(rig.step(0, 9).fx[0].type == seq::fx::kLowPass);
    CHECK(rig.step(0, 9).fx[0].value == 55);  // round(70 / 127 * 100)
  }
}

TEST_CASE("fxrec app: a take of notes and FX is one undo unit; Undo takes the notes and the FX back and leaves the CUTOFF where the knob put it; no other unit was journalled in the take") {
  Rig rig;
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  rig.go_track(1);
  rig.play(3);
  const unsigned d0 = rig.depth();
  rig.pad_down(12);  // a note at the play line
  CHECK(rig.step(1, 3).note != model::kNoteEmpty);
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);  // the FX on the same row
  CHECK(rig.step(1, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(1, 3).fx[0].value == 100);
  CHECK(rig.depth() == d0);  // the take's unit is still open: nothing was committed, no SOUND unit from the CC
  rig.play(5);
  rig.pad_up(12);  // the OFF: the playhead has left the note's step, so it goes to the play line's row
  CHECK(rig.step(1, 5).note == model::kNoteOff);
  rig.stop();  // the take ends: one unit
  CHECK(rig.depth() == d0 + 1u);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);
  (void)rig.invoke("edit.undo");  // one Undo removes the take: the note, the OFF and the FX
  CHECK(rig.depth() == d0);
  CHECK(rig.step(1, 3).note == model::kNoteEmpty);
  CHECK(rig.step(1, 3).fx[0].type == model::kFxNone);
  CHECK(rig.step(1, 5).note == model::kNoteEmpty);
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);  // the parameter stays where the knob left it (Q1 a)
  (void)rig.invoke("edit.redo");
  CHECK(rig.step(1, 3).fx[0].type == seq::fx::kLowPass);
  CHECK(rig.step(1, 3).fx[0].value == 100);
}

TEST_CASE("fxrec app: outside a take the CC is its own undo unit as before, and records no FX") {
  Rig rig;  // LIVE mode but the transport is stopped: no take
  rig.bind(0, ip::Cutoff, 74, core::LearnMode::Jump);
  CHECK_FALSE(rig.state().edit.take_open);
  const unsigned d0 = rig.depth();
  CHECK(rig.midicc(1, 74, 127) == DispatchResult::Changed);
  CHECK(rig.depth() == d0 + 1u);  // the parameter write is its own unit, as today
  CHECK(rig.value(inst(0, ip::Cutoff)) == 1.0f);
  CHECK(no_fx_anywhere(rig));
  (void)rig.invoke("edit.undo");
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.30f);  // and it undoes on its own
}

TEST_CASE("fxrec app: TUNE has no FX of its own (Q2 a) and records as a CC letter (OS-PARAMAUTO); a Mixer parameter records nothing and does not close the take (A-7)") {
  Rig rig;
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  rig.bind_track(2, tp::Volume, 20);
  rig.play(3);
  CHECK(rig.state().edit.take_open);
  CHECK(rig.midicc(1, 77, 60) == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Tune)) != 0.0f);  // the parameter moved
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kCcA);  // no FX of its own: letter A
  CHECK(rig.project().instrument(0)->midi_cc[0] == model::auto_code(inst(0, ip::Tune)));
  const unsigned d1 = rig.depth();
  const model::Step before = rig.step(0, 3);
  CHECK(rig.midicc(1, 20, 100) == DispatchResult::Changed);  // the track's volume moved
  CHECK(rig.state().edit.take_open);                         // the take stayed open (kActionRecords on midi.cc)
  CHECK(rig.depth() == d1);                                  // and nothing was journalled for it
  CHECK(std::memcmp(&rig.step(0, 3), &before, sizeof before) == 0);
}

TEST_CASE("fxrec app: a write with no free column says so once per take and leaves the step alone") {
  Rig rig;
  rig.bind(0, ip::Panning, 78, core::LearnMode::Jump);
  model::Step full = model::kEmptyStep;
  full.fx[0] = {seq::fx::kChance, 80};
  full.fx[1] = {seq::fx::kMicroMove, 20};
  NEED(rig.project().set_step(1, 0, 3, full) == model::Status::Ok);
  NEED(rig.project().set_step(1, 0, 5, full) == model::Status::Ok);
  rig.play(3);
  CHECK(rig.midicc(1, 78, 127) == DispatchResult::Changed);  // the parameter moved, the FX is dropped
  CHECK(std::memcmp(&rig.step(0, 3), &full, sizeof full) == 0);
  CHECK(rig.last_note().find("REC FX: NO FREE FX COLUMN") != std::string::npos);
  rig.play(5);
  CHECK(rig.midicc(1, 78, 0) == DispatchResult::Changed);
  CHECK(std::memcmp(&rig.step(0, 5), &full, sizeof full) == 0);
  CHECK(rig.last_note().find("REC FX: NO FREE FX COLUMN") != std::string::npos);  // still the one message (once per take)
  CHECK_FALSE(rig.state().edit.rec_fx_said);  // no FX was recorded, so no "REC FX: <PARAMETER>"
  // a free column on another row records, and the strip says the parameter then
  rig.play(7);
  CHECK(rig.midicc(1, 78, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 7).fx[0].type == seq::fx::kPanning);
  CHECK(rig.step(0, 7).fx[0].value == 100);  // the stored pan of the knob at 127
  CHECK(rig.last_note().find("REC FX: PANNING") != std::string::npos);
  rig.stop();  // the take ends: the flags go with it
  CHECK_FALSE(rig.state().edit.rec_fx_said);
  CHECK_FALSE(rig.state().edit.rec_fx_full);
}

TEST_CASE("fxrec app: the recorded FX leaves the step's note, instrument and volume alone") {
  Rig rig;
  rig.bind(0, ip::Panning, 78, core::LearnMode::Jump);
  model::Step with_note = model::kEmptyStep;
  with_note.note = 60;
  with_note.instr = 1;
  with_note.vol = 100;
  NEED(rig.project().set_step(1, 0, 3, with_note) == model::Status::Ok);
  rig.play(3);
  CHECK(rig.midicc(1, 78, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 3).note == 60);
  CHECK(rig.step(0, 3).instr == 1);
  CHECK(rig.step(0, 3).vol == 100);
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kPanning);
  CHECK(rig.step(0, 3).fx[0].value == 100);
  CHECK(rig.step(0, 3).fx[1].type == model::kFxNone);
}

// OS-PARAMAUTO S3: a learnable parameter with no FX of its own is recorded as a CC letter of the project.
TEST_CASE("paramauto rec: the first recorded move takes letter A and writes CC A; the same parameter reuses it, another takes B; the strip says which") {
  Rig rig;
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  rig.bind(0, ip::FineTune, 79, core::LearnMode::Jump, 0, 1);
  const model::Instrument* in = rig.project().instrument(0);
  CHECK(in->midi_cc[0] == 0);  // no letter at assign time
  rig.play(3);
  CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
  CHECK(in->midi_cc[0] == model::auto_code(inst(0, ip::Tune)));
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kCcA);
  CHECK(rig.step(0, 3).fx[0].value == 127);  // Tune at its maximum
  CHECK(rig.last_note() == "REC FX: TUNE -> CC A");
  rig.play(4);
  CHECK(rig.midicc(1, 77, 0) == DispatchResult::Changed);  // the same parameter: the same letter
  CHECK(rig.step(0, 4).fx[0].type == seq::fx::kCcA);
  CHECK(rig.step(0, 4).fx[0].value == 0);
  CHECK(in->midi_cc[1] == 0);
  rig.play(5);
  CHECK(rig.midicc(1, 79, 127) == DispatchResult::Changed);  // another parameter: the next letter
  CHECK(in->midi_cc[1] == model::auto_code(inst(0, ip::FineTune)));
  CHECK(rig.step(0, 5).fx[0].type == static_cast<std::uint8_t>(seq::fx::kCcA + 1));
  CHECK(rig.step(0, 5).fx[0].value == 127);
}

TEST_CASE("paramauto rec: one undo takes back the step and the letter; WATCH assigns nothing") {
  {
    Rig rig;
    rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
    rig.play(3);
    CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
    CHECK(rig.project().instrument(0)->midi_cc[0] != 0);
    rig.stop();  // the take ends: one undo group
    (void)rig.invoke("edit.undo");
    CHECK(rig.project().instrument(0)->midi_cc[0] == 0);
    CHECK(no_fx_anywhere(rig));
  }
  {
    Rig rig(16, false);  // WATCH: the parameter moves, nothing records, no letter
    rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
    rig.play(3);
    CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
    CHECK(rig.value(inst(0, ip::Tune)) != 0.0f);
    CHECK(rig.project().instrument(0)->midi_cc[0] == 0);
    CHECK(no_fx_anywhere(rig));
  }
}

TEST_CASE("paramauto rec: six letters taken say NO FREE CC LETTER once per take and record nothing; an uncodable row still has no FX") {
  Rig rig;
  model::Instrument other = *rig.project().instrument(1);
  for (unsigned l = 0; l < 6; ++l) {
    other.midi_cc[l] = static_cast<std::uint8_t>(100 + l);  // six other parameters of instrument 2 hold every letter
  }
  NEED(rig.project().set_instrument(1, other) == model::Status::Ok);
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  rig.bind(0, ip::VelSens, 80, core::LearnMode::Jump, 0, 1);
  rig.play(3);
  CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
  CHECK(rig.value(inst(0, ip::Tune)) != 0.0f);  // the move still sounds
  CHECK(no_fx_anywhere(rig));
  CHECK(rig.project().instrument(0)->midi_cc[0] == 0);
  CHECK(rig.last_note().find("REC FX: NO FREE CC LETTER") != std::string::npos);
  CHECK(rig.state().edit.rec_cc_full);
  rig.play(4);
  CHECK(rig.midicc(1, 77, 0) == DispatchResult::Changed);  // said once
  CHECK(no_fx_anywhere(rig));
  rig.stop();
  CHECK_FALSE(rig.state().edit.rec_cc_full);
  rig.play(5);  // VelSens is not coverable: it keeps its own message
  CHECK(rig.midicc(1, 80, 64) == DispatchResult::Changed);
  CHECK(no_fx_anywhere(rig));
  CHECK(rig.project().instrument(0)->midi_cc[0] == 0);
}

TEST_CASE("paramauto rec: a write with no free FX column leaves no stray letter") {
  Rig rig;
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  model::Step full = model::kEmptyStep;
  full.fx[0] = {seq::fx::kChance, 80};
  full.fx[1] = {seq::fx::kMicroMove, 20};
  NEED(rig.project().set_step(1, 0, 3, full) == model::Status::Ok);
  rig.play(3);
  CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
  CHECK(std::memcmp(&rig.step(0, 3), &full, sizeof full) == 0);
  CHECK(rig.project().instrument(0)->midi_cc[0] == 0);
}

TEST_CASE("paramauto rec: a CC letter takes the free column and never overwrites another FX of no group; the same letter overwrites itself") {
  Rig rig;
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  model::Step held = model::kEmptyStep;
  held.fx[0] = {seq::fx::kChance, 80};
  NEED(rig.project().set_step(1, 0, 3, held) == model::Status::Ok);
  rig.play(3);
  CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kChance);
  CHECK(rig.step(0, 3).fx[0].value == 80);
  CHECK(rig.step(0, 3).fx[1].type == seq::fx::kCcA);
  CHECK(rig.step(0, 3).fx[1].value == 127);
  CHECK(rig.midicc(1, 77, 0) == DispatchResult::Changed);  // the same letter on the same step: its own column
  CHECK(rig.step(0, 3).fx[0].type == seq::fx::kChance);
  CHECK(rig.step(0, 3).fx[1].type == seq::fx::kCcA);
  CHECK(rig.step(0, 3).fx[1].value == 0);
}

// Review fold: the letter is its own 4-byte record in the take's unit, not the whole instrument.
TEST_CASE("paramauto rec: undoing the take takes only the letter back; another parameter and the name changed since keep their values") {
  Rig rig;
  rig.three_rings();
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  rig.play(3);
  CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
  CHECK(rig.project().instrument(0)->midi_cc[0] != 0);
  rig.stop();  // the take is one undo unit of the PATTERN ring
  NEED(rig.editor->set_param(inst(0, ip::Cutoff), 0.80f) == model::Status::Ok);  // after the take: the SOUND ring
  NEED(rig.editor->set_instrument_name(0, "NEWNAME", 7) == model::Status::Ok);
  const float tune = rig.value(inst(0, ip::Tune));
  // The take's unit lives in the PATTERN ring (edit.undo picks the ring of the screen in view; the editor is asked directly).
  CHECK(rig.editor->undo(model::UndoArea::Pattern) == model::UndoResult::Ok);
  const model::Instrument* in = rig.project().instrument(0);
  CHECK(in->midi_cc[0] == 0);  // the letter is gone with the FX
  CHECK(no_fx_anywhere(rig));
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.80f);  // the other parameter keeps its new value
  CHECK(std::strcmp(in->name, "NEWNAME") == 0);
  CHECK(rig.value(inst(0, ip::Tune)) == tune);
  CHECK(rig.editor->redo(model::UndoArea::Pattern) == model::UndoResult::Ok);  // and redo gives the letter back
  CHECK(rig.project().instrument(0)->midi_cc[0] == model::auto_code(inst(0, ip::Tune)));
  CHECK(rig.value(inst(0, ip::Cutoff)) == 0.80f);
}

TEST_CASE("paramauto editor: set_instrument_cc is one record; the same code, a bad letter and an empty slot record nothing") {
  Rig rig;
  model::Editor& e = *rig.editor;
  const unsigned depth = rig.depth();
  CHECK(e.set_instrument_cc(0, 6, 5) == model::Status::BadIndex);
  CHECK(e.set_instrument_cc(9, 0, 5) == model::Status::NotFound);
  CHECK(e.set_instrument_cc(0, 0, 0) == model::Status::Ok);  // already 0
  CHECK(rig.depth() == depth);
  CHECK(e.set_instrument_cc(0, 2, 5) == model::Status::Ok);
  CHECK(rig.project().instrument(0)->midi_cc[2] == 5);
  CHECK(rig.depth() == depth + 1);
  (void)e.undo();
  CHECK(rig.project().instrument(0)->midi_cc[2] == 0);
}

namespace {
// A sequencer stand-in for App::poll_param_auto: it keeps the mask it was told and hands out the value of a letter once.
struct AutoSink final : app::TrackSink {
  std::uint8_t mask = 0xFF;  // 0xFF: never told
  int masks = 0;
  std::uint8_t pending = 0;
  std::uint8_t val[6] = {};
  bool playing() const noexcept override { return true; }
  bool mute_defer(unsigned, app::MuteMode, bool) noexcept override { return false; }
  bool mute_cancel(unsigned) noexcept override { return false; }
  std::uint16_t mute_pending() const noexcept override { return 0; }
  std::uint8_t note_count(unsigned) const noexcept override { return 0; }
  void auto_mask(std::uint8_t m) noexcept override {
    mask = m;
    ++masks;
  }
  std::uint8_t auto_take(std::uint8_t* out) noexcept override {
    std::memcpy(out, val, sizeof val);
    const std::uint8_t p = pending;
    pending = 0;
    return p;
  }
};
}  // namespace

// Review fold: the main loop's half of the playback (App::poll_param_auto).
TEST_CASE("paramauto poll: the mask is told on the first pass and again only when the project's generation moves; a played letter is applied to its parameter") {
  Rig rig;
  AutoSink sink;
  auto board = std::make_unique<app::MixBoard>();
  board->tracks = &sink;
  rig.app->attach_mix(board.get());
  rig.pass();
  CHECK(sink.masks == 1);
  CHECK(sink.mask == 0);  // a project with no letter assigned: every CC A..F step plays as before (the sequencer sends the CC)
  rig.pass();
  CHECK(sink.masks == 1);  // nothing changed: not rebuilt
  // a letter is assigned (a take, here by hand): the generation moves and the mask follows
  NEED(rig.editor->set_instrument_cc(0, 0, model::auto_code(inst(0, ip::Tune))) == model::Status::Ok);
  rig.pass();
  CHECK(sink.masks == 2);
  CHECK(sink.mask == 0x01);
  // the sequencer played CC A = 127 (OS-BUG-PARAMAUTO-PLAY): the AUDIO SIDE takes the maximum, the project keeps the base value and its generation does not move
  const auto drain = [&rig]() {
    std::vector<core::ParamChange> out;
    core::ParamChange c;
    while (rig.ring.pop(c)) {
      out.push_back(c);
    }
    return out;
  };
  (void)drain();
  rig.play(1);  // the sequencer runs
  const float before = rig.value(inst(0, ip::Tune));
  const std::uint32_t generation = rig.project().generation();
  sink.val[0] = 127;
  sink.pending = 0x01;
  rig.pass();
  CHECK(rig.value(inst(0, ip::Tune)) == before);
  CHECK(rig.project().generation() == generation);
  std::vector<core::ParamChange> heard = drain();
  NEED(heard.size() == 1u);
  CHECK(heard[0].id == inst(0, ip::Tune));
  CHECK(heard[0].value == doctest::Approx(24.0f));  // TUNE is -24..24 st: 127 is its maximum
  CHECK(sink.pending == 0);
  // a letter of another instrument's slot with the same code is not a second owner: the lowest slot wins
  const float other = rig.value(inst(1, ip::Tune));
  NEED(rig.editor->set_instrument_cc(1, 0, model::auto_code(inst(1, ip::Tune))) == model::Status::Ok);
  (void)drain();
  sink.val[0] = 0;
  sink.pending = 0x01;
  rig.pass();
  CHECK(rig.value(inst(1, ip::Tune)) == other);
  heard = drain();
  NEED(heard.size() == 1u);
  CHECK(heard[0].id == inst(0, ip::Tune));  // the lowest slot's
  CHECK(heard[0].value == doctest::Approx(-24.0f));
  // the sequencer stops: the base value goes to the audio side again, and only once
  rig.play(1, 0, false);
  heard = drain();
  NEED(heard.size() == 1u);
  CHECK(heard[0].id == inst(0, ip::Tune));
  CHECK(heard[0].value == before);
  rig.pass();
  CHECK(drain().empty());
  CHECK(rig.value(inst(0, ip::Tune)) == before);
  // the song ends on a step with a mapped letter: the last value is published and the transport stops in ONE pass; the take comes first, the restore after it, so the base value is what stays
  rig.play(2);
  (void)drain();
  sink.val[0] = 127;
  sink.pending = 0x01;
  rig.play(2, 0, false);
  heard = drain();
  NEED(heard.size() == 2u);
  CHECK(heard[0].value == doctest::Approx(24.0f));
  CHECK(heard[1].id == inst(0, ip::Tune));
  CHECK(heard[1].value == before);
  rig.pass();
  CHECK(drain().empty());
  // a restore the audio side drops (the ring is full) keeps its bit: the next pass sends it
  rig.play(3);
  (void)drain();
  sink.val[0] = 127;
  sink.pending = 0x01;
  rig.pass();
  NEED(drain().size() == 1u);
  unsigned filled = 0;
  while (rig.ring.push(core::ParamChange{inst(1, ip::Tune), 0.0f, 0u})) {
    ++filled;
  }
  CHECK(filled > 0u);
  const std::uint32_t drops = rig.app->session().param_drops;
  rig.play(3, 0, false);
  CHECK(rig.app->session().param_drops == drops + 1u);  // the restore was dropped
  CHECK(drain().size() == filled);                        // only what the test pushed
  rig.pass();                                             // room again: the retry
  heard = drain();
  NEED(heard.size() == 1u);
  CHECK(heard[0].id == inst(0, ip::Tune));
  CHECK(heard[0].value == before);
  rig.pass();
  CHECK(drain().empty());
  rig.app->attach_mix(nullptr);
}

// Review fold: a letter on a MIDI track would send a CC, not move the parameter: refused, nothing written.
TEST_CASE("paramauto rec: a track whose sounding instrument is MIDI refuses the letter once per take and writes nothing") {
  Rig rig;
  rig.bind(0, ip::Tune, 77, core::LearnMode::Jump);
  model::Instrument mi;
  model::init_instrument(mi, model::InstrumentKind::Midi);
  mi.midi_channel = 1;
  NEED(rig.project().set_instrument(2, mi) == model::Status::Ok);
  model::Step n = model::kEmptyStep;
  n.note = 40;
  n.instr = 3;  // instrument slot 2, the MIDI one: the track plays it until a step names another
  NEED(rig.project().set_step(1, 0, 1, n) == model::Status::Ok);
  rig.play(3);
  CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);  // the parameter still moves
  CHECK(rig.last_note() == "REC FX: MIDI TRACK");
  CHECK(rig.step(0, 3).fx[0].type == model::kFxNone);
  CHECK(rig.project().instrument(0)->midi_cc[0] == 0);
  rig.play(4);
  CHECK(rig.midicc(1, 77, 0) == DispatchResult::Changed);
  CHECK(rig.step(0, 4).fx[0].type == model::kFxNone);
  // A step that names the Sample instrument sounds it: the letter records there.
  model::Step s = model::kEmptyStep;
  s.note = 40;
  s.instr = 1;
  NEED(rig.project().set_step(1, 0, 5, s) == model::Status::Ok);
  rig.play(6);
  CHECK(rig.midicc(1, 77, 127) == DispatchResult::Changed);
  CHECK(rig.step(0, 6).fx[0].type == seq::fx::kCcA);
}
