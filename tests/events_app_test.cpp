// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MIDIREC S1b in the app (docs/specs/OS-MIDIREC.md sections 3.1, 3.4, 4 S1): the EVENTS overlay opened from the TOOLS list's
// EVENTS entry — opening with and without a block, the refusals (NO PROJECT, PATTERN IS EMPTY, the follow rule of tools_open),
// the scroll, the pages and the fields, the BACK landing on the grid cursor, no undo unit from any key of the list, and the key
// row and Shift row labels. The projection itself is tests/event_list_test.cpp (S1a).
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/events_view.h>
#include <o3os/app/pad_note.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/tools_list.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/action.h>
#include <o3os/model/editor.h>
#include <o3os/model/event_list.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>
#include <string>

#include "support/need.h"
#include "support/project_fixture.h"
#include "support/key_row.h"

using namespace o3os;
using app::Button;
using app::Column;
using app::DispatchResult;
using app::OverlayId;
using core::ActionArg;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

namespace ev = model::events;

// An App with a document attached: pattern 1 of 64 steps (0: no pattern yet), one song position, instruments 1 (sample) and
// 49 (MIDI, channel 3), and the project's CC numbers A = 74, C = 71, the rest unassigned. Track 0 holds a note on step 1 with an
// OFF on step 5; track 1 a CC A on step 1 and a micro-moved note on step 9.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  audio::PlayPosition position;

  explicit Rig(bool with_pattern = true) {
    model::Project& p = test::arena(0).project;
    p.reset();
    if (with_pattern) {
      (void)p.create_pattern(1, 64);
      model::Step s = model::kEmptyStep;
      s.note = 61;  // C-4
      s.instr = 1;
      s.vol = 100;
      (void)p.set_step(1, 0, 0, s);
      s = model::kEmptyStep;
      s.note = model::kNoteOff;
      (void)p.set_step(1, 0, 4, s);
      s = model::kEmptyStep;
      s.fx[0] = {ev::kCcFirst, 74};  // CC A, value 74
      (void)p.set_step(1, 1, 0, s);
      s = model::kEmptyStep;
      s.note = 64;
      s.instr = 49;  // the MIDI instrument, channel 3
      s.fx[1] = {ev::kFxMicroMove, 50};
      (void)p.set_step(1, 1, 8, s);
    }
    (void)p.append_order(1);
    (void)p.create_pattern(2, 128);  // a row on every other step of track 0: 64 rows, more than a page
    model::Step row = model::kEmptyStep;
    row.note = 61;
    row.instr = 1;
    for (unsigned r = 0; r < 128; r += 2) {
      (void)p.set_step(2, 0, r, row);
    }
    (void)p.create_pattern(3, 16);  // exists, empty
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::memcpy(in.name, "SAMPLE", 6);
    (void)p.set_instrument(0, in);
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = 3;
    std::memcpy(in.name, "MIDI 3", 6);
    (void)p.set_instrument(48, in);  // instrument 49: MIDI on channel 3
    model::MidiState midi{};
    midi.notes_channel = 0;  // channel 1 on the wire
    midi.cc_number[0] = 74;
    midi.cc_number[2] = 71;
    for (unsigned i = 0; i < model::kMidiCcSlots; ++i) {
      if (midi.cc_number[i] == 0u) {
        midi.cc_number[i] = model::kMidiCcUnassigned;
      }
    }
    midi.cc_number[0] = 74;
    midi.cc_number[2] = 71;
    (void)p.set_midi(midi);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_play_position(&position);
    app->run_once();
  }
  model::Project& project() { return test::arena(0).project; }
  const app::ViewState& state() const { return app->state(); }
  const app::PatternView& view() const { return app->view(); }
  std::string message() const { return app->message(); }
  unsigned depth() const { return journal.depth(); }

  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(1);
  }
  void flush() {
    pass(1);
    pass(10000);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  // F6 EVENTS on the Pattern key row (OS-F6EVENTS; it was F7, F6 in the verb menu).
  void open_events() {
    tap(Button::F6);
  }
  // The pattern in view, through the PATTERN chooser (OS-PATSWITCH: a hold of F1 only, so chooser.hold, not toggle).
  void select_pattern(unsigned id) {
    (void)invoke("chooser.hold", 0);
    for (unsigned guard = 0; guard < 300u && state().edit.pattern != id; ++guard) {
      (void)invoke("chooser.delta", state().edit.pattern < id ? 1 : -1);
    }
    (void)invoke("chooser.release", 0);
    pass(1);
  }
  // The cursor to (row, track) in the Note column.
  void go(int row, int track = 0) {
    (void)invoke("cursor.jump_fraction", 0);
    (void)invoke("nav.scroll", row);
    (void)invoke("cursor.home");
    for (int i = 0; i < track * 5; ++i) {
      (void)invoke("nav.right");
    }
  }
};

}  // namespace

// ---- opening ----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("events: Pattern F6 opens the overlay over the cursor's track (OS-F6EVENTS)") {
  Rig rig;
  rig.tap(Button::F6);
  CHECK(rig.state().overlay == OverlayId::Events);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);

  rig.go(9, 1);  // the cursor on track 2, step 10 (1-based): the first row at or after it is the micro-moved note on step 9
  rig.open_events();
  CHECK(rig.state().overlay == OverlayId::Events);
  const ev::Scope& sc = rig.state().events.scope;
  CHECK(sc.track0 == 1u);
  CHECK(sc.track1 == 1u);
  CHECK_FALSE(sc.multi_track());
  CHECK(sc.row0 == 0u);
  CHECK(sc.row1 == 63u);  // every step of the track's length
  // The scope's rows: the CC A on step 1 and the NOTE on step 9, the note first at or after step 10 is none, so the cursor is the last row.
  CHECK(rig.state().events.cursor == 1u);
  CHECK(rig.state().events.top == 0u);
  CHECK(rig.state().events.filter == ev::Filter::All);
  rig.tap(Button::F8);  // BACK lands on the row's step, track and Note column
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.view().row() == 8);
  CHECK(rig.view().track() == 1);
  CHECK(rig.view().column() == Column::Note);
}

TEST_CASE("events: opening over a block scopes the list to the block's tracks and rows") {
  Rig rig;
  rig.go(2, 1);
  (void)rig.invoke("select.extend", 1);  // Shift + Down: rows 3..4 (0-based 2..3) of track 2
  NEED(rig.state().edit.selection.active);
  rig.open_events();
  CHECK(rig.state().overlay == OverlayId::Events);
  const ev::Scope& sc = rig.state().events.scope;
  CHECK(sc.track0 == 1u);
  CHECK(sc.track1 == 1u);  // (one track: a column block is widened to whole steps, the scope has no columns)
  CHECK(sc.row0 == 2u);
  CHECK(sc.row1 == 3u);
  CHECK(sc.multi_track() == false);
  // A block over three tracks: the TRK column's scope.
  rig.tap(Button::F8);
  (void)rig.invoke("select.clear");
  rig.go(0, 0);
  (void)rig.invoke("select.extend", -1);  // Shift + Up at step 0: the whole column of track 1 (0-based 0)
  (void)rig.invoke("select.extend", 10);
  (void)rig.invoke("select.extend", 10);  // two tracks to the right (the column block steps by tracks): a block over tracks 1..3, 0-based 0..2
  NEED(rig.state().edit.selection.active);
  rig.open_events();
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.state().events.scope.track0 == 0u);
  CHECK(rig.state().events.scope.track1 == 2u);
  CHECK(rig.state().events.scope.multi_track());
}

// ---- the refusals -----------------------------------------------------------------------------------------------------------------------

TEST_CASE("events: the refusals — no project, a pattern the project lacks, the follow rule of tools_open") {
  {
    Rig rig(false);  // no pattern, but a document is attached: F6 EVENTS refuses with PATTERN IS EMPTY
    rig.tap(Button::F6);
    CHECK(rig.state().overlay == OverlayId::None);
    CHECK(rig.message() == "PATTERN IS EMPTY");
  }
  {
    Rig rig;
    rig.app->attach_document(nullptr);  // the demo: no document
    rig.pass(1);
    rig.tap(Button::F7);
    CHECK(rig.state().overlay == OverlayId::None);
    CHECK(rig.message() == "NO PROJECT");
  }
  {
    Rig rig;  // the view follows the play line (Browse while playing): the list opens (OS-PTOOLS2 S1) and EVENTS refuses
    rig.position.publish(true, 1, 0, 12000, audio::PlayPosition::kNoOrder);
    rig.pass(40);
    NEED(rig.state().play.playing);
    rig.open_events();
    CHECK(rig.state().overlay == OverlayId::None);
    CHECK(rig.message() == "STOP PLAYING TO OPEN EVENTS");
    rig.position.publish(false, 1, 0, 12000);
    rig.pass(40);
    rig.tap(Button::F7);
    CHECK(rig.state().overlay == OverlayId::VerbMenu);  // stopped: the menu opens
  }
}

TEST_CASE("events: an empty scope opens an empty list, and no key of the list opens an undo unit") {
  Rig rig;
  rig.select_pattern(3);  // a pattern that exists and holds nothing
  rig.open_events();
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.state().events.cursor == 0u);
  const unsigned depth = rig.depth();
  // Every key of the list: the arrows, the encoder, the pages, the fields, the labelled F-keys. None writes, none undoes.
  for (Button b : {Button::Up, Button::Down, Button::Left, Button::Right, Button::F1, Button::F2, Button::F5, Button::F7}) {
    rig.tap(b);
    CHECK(rig.state().overlay == OverlayId::Events);
    CHECK(rig.depth() == depth);
  }
  rig.turn(3);
  CHECK(rig.depth() == depth);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Up);
  rig.tap(Button::Down);
  rig.tap(Button::F2);
  rig.tap(Button::F7);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.depth() == depth);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.depth() == depth);
}

// ---- move, page, field ------------------------------------------------------------------------------------------------------------------

TEST_CASE("events: Up / Down move the row and the window follows; Shift + Up / Down page; Left / Right the field") {
  Rig rig;
  rig.select_pattern(2);  // 64 NOTE rows on track 1, more than a page
  rig.open_events();
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.state().events.cursor == 0u);
  CHECK(rig.state().events.top == 0u);
  const unsigned rows = ev::count_rows(*rig.project().pattern(2), ev::env_of(rig.project()), rig.state().events.scope, ev::Filter::All);
  CHECK(rows == 64u);
  for (unsigned i = 0; i < app::kEventsRows + 3u; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.state().events.cursor == app::kEventsRows + 3u);
  CHECK(rig.state().events.top == 3u + 1u);  // the window followed: the cursor is the last visible row
  for (unsigned i = 0; i < 5u; ++i) {  // back up: the encoder edits the field since S2b, Up / Down move the row
    rig.tap(Button::Up);
  }
  CHECK(rig.state().events.cursor == app::kEventsRows - 2u);
  CHECK(rig.state().events.top == 4u);  // still inside the window
  // A page down: kEventsRows rows at once (OS-NAV3).
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.state().events.cursor == 2u * app::kEventsRows - 2u);
  CHECK(rig.state().events.top == app::kEventsRows - 1u);
  // The field walks the row's fields: the cursor row is a NOTE row, so Type is skipped: Time -> Ch -> Data -> Val -> Len -> In -> Fx1 -> Fx2 (OS-EVFX).
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Time);
  rig.tap(Button::Right);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Ch);
  rig.tap(Button::Right);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Data);
  rig.tap(Button::Left);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Ch);
  for (unsigned i = 0; i < 5u; ++i) {  // Data, Val, Len, In, Fx1
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Fx1);
  rig.tap(Button::Right);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Fx2);
  rig.tap(Button::Right);  // round to Time
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Time);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Data);
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  rig.tap(Button::Left);  // Ch, Time, round to Fx2
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Fx2);
  rig.tap(Button::Right);
  rig.tap(Button::Right);  // Time, Ch: the state the CC part below expects after the page change
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Ch);
  // On a CC row the field set is Time, Type, Ch, Data, Val, In: no Len.
  rig.tap(Button::F8);
  rig.select_pattern(1);  // back to pattern 1: its track 2 holds the CC A on step 1 (pattern 2's track 2 is empty)
  rig.go(0, 1);  // track 2: the CC A on step 1 is the first row
  rig.open_events();
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.state().events.cursor == 0u);
  rig.tap(Button::Right);  // Time -> Type (the CC row has one)
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Type);
  for (unsigned i = 0; i < 5u; ++i) {  // Ch, Data, Val, In (Len is skipped), round to Time
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Time);
}

// ---- BACK -------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("events: BACK lands the grid cursor on the row's step, track and column — the CC's FX column for a CC row") {
  Rig rig;
  rig.go(0, 1);  // track 2: the CC A on step 1 (FX 1) is the first row, the note on step 9 the second
  rig.open_events();
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.state().events.cursor == 0u);  // the CC row
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.view().row() == 0);
  CHECK(rig.view().track() == 1);
  CHECK(rig.view().column() == Column::Fx1);  // the CC A is in FX 1
  // The second row is the NOTE on step 9, whose Micro-move is in FX 2: BACK on it lands on the Note column.
  rig.open_events();
  rig.tap(Button::Down);
  CHECK(rig.state().events.cursor == 1u);
  rig.tap(Button::F8);
  CHECK(rig.view().row() == 8);
  CHECK(rig.view().track() == 1);
  CHECK(rig.view().column() == Column::Note);
}

// ---- the key row ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("events: the key row and the Shift row of the overlay (spec 3.4)") {
  Rig rig;
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  const test::RowRead row = test::read_owner_row(rig.app->model(), rig.state());
  CHECK(row.marked);
  CHECK(row.label[0] == "ALL 1/3");
  CHECK(row.label[1] == "QUANT");
  CHECK(row.label[2].empty());
  CHECK(row.label[3].empty());
  CHECK(row.label[4] == "PREVIEW");
  CHECK(row.label[5].empty());
  CHECK(row.label[6] == "UNDO");
  CHECK(row.label[7] == "BACK");
  CHECK(row.hot == 0u);
  const test::RowRead shifted = test::read_owner_row(rig.app->model(), rig.state(), true);
  CHECK(shifted.label[0] == "-");
  CHECK(shifted.label[1] == "ALL");  // Shift + F2: QUANT ALL (nine characters, over a key cap's eight: ALL)
  CHECK(shifted.label[2] == "-");
  CHECK(shifted.label[3] == "-");
  CHECK(shifted.label[4] == "-");
  CHECK(shifted.label[5] == "-");
  CHECK(shifted.label[6] == "REDO");
  CHECK(shifted.label[7] == "-");
}

// ---- F1 FILTER (S1c) -------------------------------------------------------------------------------------------------------------------

TEST_CASE("events: F1 FILTER cycles ALL, NOTES, CC and back; the count, the cursor and the F1 label follow; no undo unit") {
  Rig rig;
  rig.go(0, 1);  // track 2: the CC A on step 1 and the micro-moved note on step 9 — one row of each kind under ALL
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  CHECK(rig.state().events.cursor == 0u);
  const unsigned depth = rig.depth();
  CHECK(test::read_owner_row(rig.app->model(), rig.state()).label[0] == "ALL 1/3");
  rig.tap(Button::F1);
  CHECK(rig.state().events.filter == ev::Filter::Notes);
  CHECK(rig.state().events.cursor == 0u);  // the CC row is filtered out, the cursor stays inside the list
  CHECK(test::read_owner_row(rig.app->model(), rig.state()).label[0] == "NOTE 2/3");
  rig.tap(Button::F1);
  CHECK(rig.state().events.filter == ev::Filter::Cc);
  CHECK(rig.state().events.cursor == 0u);
  CHECK(test::read_owner_row(rig.app->model(), rig.state()).label[0] == "CC 3/3");
  rig.tap(Button::F1);
  CHECK(rig.state().events.filter == ev::Filter::All);
  CHECK(test::read_owner_row(rig.app->model(), rig.state()).label[0] == "ALL 1/3");
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.depth() == depth);  // the filter writes nothing

  // A filter that leaves nothing: pattern 2 holds 64 NOTE rows and no CC, so CC empties the list and clamps the cursor to 0.
  rig.tap(Button::F8);
  rig.select_pattern(2);
  rig.go(30, 0);
  rig.open_events();
  CHECK(rig.state().events.cursor == 15u);  // the first row at or after step 31
  rig.tap(Button::F1);                        // NOTES: all 64 rows stay
  CHECK(rig.state().events.filter == ev::Filter::Notes);
  CHECK(rig.state().events.cursor == 15u);
  rig.tap(Button::F1);  // CC: nothing is left
  CHECK(rig.state().events.filter == ev::Filter::Cc);
  CHECK(rig.state().events.cursor == 0u);
  CHECK(rig.depth() == depth);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);
}

// ---- F5 PREVIEW (S1c) ------------------------------------------------------------------------------------------------------------------

namespace {
struct HoldLog final : app::PreviewSink {
  struct Call {
    bool on;
    unsigned track, instrument, note;
  };
  std::vector<Call> calls;
  void preview(unsigned, unsigned) noexcept override {}
  void hold_note(bool on, unsigned track, unsigned instrument, unsigned note) noexcept override {
    calls.push_back({on, track, instrument, note});
  }
};
}  // namespace

TEST_CASE("events: F5 PREVIEW sounds the cursor row's step through the grid's preview path, and its release ends it") {
  Rig rig;
  HoldLog sink;
  rig.app->attach_preview(&sink);
  rig.go(8, 1);  // track 2, step 9: the cursor opens on the micro-moved note (note 64 on instrument 49)
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 1u);
  const unsigned depth = rig.depth();
  rig.press(Button::F5);
  rig.pass(1);
  CHECK(rig.state().step_preview);
  NEED(sink.calls.size() == 1u);
  CHECK(sink.calls[0].on);
  CHECK(sink.calls[0].track == 1u);
  CHECK(sink.calls[0].instrument == 49u);
  CHECK(sink.calls[0].note == 63u);  // note 64, as the OG files number them
  CHECK(rig.depth() == depth);        // no undo unit from the key
  rig.release(Button::F5);            // the raw release ends the held note (App::run_once)
  rig.pass(1);
  CHECK_FALSE(rig.state().step_preview);
  NEED(sink.calls.size() == 2u);
  CHECK_FALSE(sink.calls[1].on);
  // On the CC row's step (no note there) the grid's refusal says so, and nothing sounds.
  rig.tap(Button::Up);
  CHECK(rig.state().events.cursor == 0u);
  rig.press(Button::F5);
  rig.pass(1);
  CHECK(std::string(rig.state().notify.head().text.c_str()) == "PREVIEW: NO NOTE ON THIS STEP");  // the Notify strip waits behind the modal overlay, so app->message() is ""
  CHECK(sink.calls.size() == 2u);
  rig.release(Button::F5);
  rig.pass(1);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.depth() == depth);
}

TEST_CASE("events: F5 PREVIEW without an audio path says so") {
  Rig rig;  // no PreviewSink attached
  rig.go(8, 1);
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  rig.press(Button::F5);
  rig.pass(1);
  CHECK(std::string(rig.state().notify.head().text.c_str()) == "PREVIEW: NO AUDIO OUTPUT");
  CHECK_FALSE(rig.state().step_preview);
  rig.release(Button::F5);
  rig.pass(1);
  rig.tap(Button::F8);
}

// ---- S2b: the field edits (spec 3.5, A13-A14, A21; QA-PAT-412..420, 424) ----------------------------------------------------------------

namespace {

// The strip's head while the modal overlay is up (app->message() is "" behind one).
std::string head_text(const Rig& rig) { return std::string(rig.state().notify.head().text.c_str()); }

}  // namespace

TEST_CASE("events S2b: in STEP the encoder edits the field as one undo unit a detent; in WATCH it says TURN EDIT ON and writes nothing (QA-PAT-412, 424)") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP
  rig.go(0, 0);           // track 1: the NOTE on 001 is the first row
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 0u);
  const unsigned depth = rig.depth();
  rig.turn(1);  // TIME +1 microstep: a Micro-move in FX 2 of step 1 (A6)
  CHECK(rig.depth() == depth + 1u);
  core::ActionId unit = core::ActionId::None;
  core::ActionArg unit_arg = 0;
  CHECK(rig.journal.newest_undo(unit, unit_arg));
  CHECK(unit == core::action_id("events.edit"));  // the unit is the action's
  const model::Step& st = rig.project().pattern(1)->steps[0][0];
  CHECK(st.note == 61);
  CHECK(st.fx[1].type == ev::kFxMicroMove);
  CHECK(st.fx[1].value == ev::micro_value(1));
  CHECK(rig.state().events.cursor == 0u);  // the cursor stays on the row
  // DATA (QA-PAT-412): a semitone, an octave (Shift + encoder), the pad's note.
  rig.tap(Button::Right);  // Time -> Ch
  rig.tap(Button::Right);  // Ch -> Data
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Data);
  rig.turn(1);
  CHECK(rig.project().pattern(1)->steps[0][0].note == 62);
  CHECK(rig.depth() == depth + 2u);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.turn(1);  // +12: an octave
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.project().pattern(1)->steps[0][0].note == 74);
  CHECK(rig.depth() == depth + 3u);
  const unsigned pad = 5;
  const unsigned want = app::pad_note(&rig.project(), rig.state().instrument, rig.state().edit.pads, pad) + 1u;
  (void)rig.invoke("events.pad", static_cast<ActionArg>(pad));
  CHECK(rig.project().pattern(1)->steps[0][0].note == want);
  CHECK(rig.depth() == depth + 4u);
  // WATCH (QA-PAT-424): the same keys say TURN EDIT ON, write nothing, open no unit.
  rig.tap(Button::F8);
  rig.tap(Button::Rec);  // back to WATCH
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  const unsigned depth2 = rig.depth();
  const model::Step before = rig.project().pattern(1)->steps[0][0];
  rig.turn(1);
  CHECK(rig.depth() == depth2);
  CHECK(std::memcmp(&before, &rig.project().pattern(1)->steps[0][0], sizeof(model::Step)) == 0);
  CHECK(head_text(rig) == "TURN EDIT ON");
  rig.tap(Button::Right);  // the field resets to Time on open; walk back to Data
  rig.tap(Button::Right);
  (void)rig.invoke("events.pad", 0);  // a pad on DATA in WATCH too
  CHECK(rig.depth() == depth2);
  CHECK(std::memcmp(&before, &rig.project().pattern(1)->steps[0][0], sizeof(model::Step)) == 0);
  CHECK(head_text(rig) == "TURN EDIT ON");
  rig.tap(Button::F8);
}

// OS-EVFX: the NOTE row's FX1 and FX2 fields. The encoder walks the slot's value, one undo unit a detent, a coarse turn is a
// nibble; the type does not change; a CC row has no FX fields. The micro-moved note on track 2 step 9 holds {Micro-move, 50} in FX 2.
TEST_CASE("events OS-EVFX: the encoder edits a NOTE row's FX value, one undo unit a detent; CC rows skip FX1 and FX2 (QA-PAT-484)") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP
  rig.go(8, 1);           // track 2, step 9: the micro-moved note (cursor on row 1)
  rig.open_events();  // OS-F6EVENTS: F6
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 1u);
  for (unsigned i = 0; i < 7u; ++i) {  // Time -> Ch, Data, Val, Len, In, Fx1, Fx2
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Fx2);
  const model::Step before = rig.project().pattern(1)->steps[1][8];
  NEED(before.fx[1].type == ev::kFxMicroMove);
  const unsigned depth = rig.depth();
  rig.turn(1);
  CHECK(rig.depth() == depth + 1u);
  const model::Step& st = rig.project().pattern(1)->steps[1][8];
  CHECK(st.fx[1].type == ev::kFxMicroMove);
  CHECK(st.fx[1].value == before.fx[1].value + 1u);
  CHECK(st.fx[0].type == before.fx[0].type);
  CHECK(st.note == before.note);
  CHECK(rig.state().events.cursor == 1u);  // the cursor stays on the row
  core::ActionId unit = core::ActionId::None;
  core::ActionArg unit_arg = 0;
  CHECK(rig.journal.newest_undo(unit, unit_arg));
  CHECK(unit == core::action_id("events.edit"));
  rig.press(Button::Shift);
  rig.pass(1);
  rig.turn(1);  // a nibble
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.project().pattern(1)->steps[1][8].fx[1].value == before.fx[1].value + 17u);
  CHECK(rig.depth() == depth + 2u);
  (void)rig.invoke("events.undo");  // inside the list (F7): the grid's PATTERN-area undo
  (void)rig.invoke("events.undo");
  CHECK(std::memcmp(&before, &rig.project().pattern(1)->steps[1][8], sizeof(model::Step)) == 0);
  CHECK(rig.depth() == depth);
  // FX1 is empty: a turn gives it the last used FX type (Micro-move, the FX 2 edit above) at its start value, one unit.
  rig.tap(Button::Left);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Fx1);
  const unsigned depth2 = rig.depth();
  rig.turn(1);
  CHECK(rig.depth() == depth2 + 1u);
  CHECK(rig.project().pattern(1)->steps[1][8].fx[0].type == ev::kFxMicroMove);
  CHECK(rig.project().pattern(1)->steps[1][8].fx[1].value == before.fx[1].value);
  // The CC row (row 0) has no FX field: moving up with the box on FX1 lands the box on a field the row has.
  rig.tap(Button::Up);
  NEED(rig.state().events.cursor == 0u);
  rig.tap(Button::Left);
  CHECK(app::events_field_ok(ev::Kind::Cc, static_cast<app::EventsView::Field>(rig.state().events.field)));
  CHECK_FALSE(app::events_field_ok(ev::Kind::Cc, app::EventsView::Field::Fx1));
  CHECK_FALSE(app::events_field_ok(ev::Kind::Off, app::EventsView::Field::Fx2));
  CHECK(app::events_field_ok(ev::Kind::Note, app::EventsView::Field::Fx2));
  rig.tap(Button::F8);
}

// A pad that writes sounds as on the grid (QA-PAT-412 "heard as on the grid", OS-PADS-B): pad_down on the row's track and
// instrument with the pad's note on DATA (the row's note on VAL), and the session's last note follows it.
namespace {
struct PadDownLog final : app::PreviewSink {
  struct Down {
    unsigned pad, track, instrument, note;
  };
  std::vector<Down> downs;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override { downs.push_back({pad, track, instrument, note}); }
};
}  // namespace

TEST_CASE("events S2b: a writing pad sounds as on the grid and sets the last note (QA-PAT-412, OS-PADS-B)") {
  Rig rig;
  PadDownLog sink;
  rig.app->attach_preview(&sink);
  rig.tap(Button::Rec);  // STEP
  rig.go(0, 0);           // track 1: the NOTE on 001 (instrument 1) is the first row
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 0u);
  rig.tap(Button::Right);  // Time -> Ch
  rig.tap(Button::Right);  // Ch -> Data
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Data);
  const unsigned pad = 5;
  const unsigned note = app::pad_note(&rig.project(), rig.state().instrument, rig.state().edit.pads, pad);
  CHECK(rig.state().edit.last_note != note);  // nothing played yet
  (void)rig.invoke("events.pad", static_cast<ActionArg>(pad));
  CHECK(rig.project().pattern(1)->steps[0][0].note == note + 1u);  // the write stands
  NEED(sink.downs.size() == 1u);
  CHECK(sink.downs[0].pad == pad);
  CHECK(sink.downs[0].track == 0u);
  CHECK(sink.downs[0].instrument == 1u);
  CHECK(sink.downs[0].note == note);
  CHECK(rig.state().edit.last_note == note);
  // The undo unit says the event on the strip (spec A12): F8 on the grid after BACK takes the pad edit back.
  rig.tap(Button::F8);  // BACK to the grid
  CHECK(rig.state().overlay == OverlayId::None);
  rig.tap(Button::F8);  // UNDO on the grid (QA-PAT-412: one back at a time)
  CHECK(rig.project().pattern(1)->steps[0][0].note == 61);
  CHECK(rig.message() == "UNDO EVENT");
  rig.flush();  // let the notice expire, so the next one is the queue's head (invoke runs no pass)
  (void)rig.invoke("edit.redo");
  CHECK(rig.project().pattern(1)->steps[0][0].note == note + 1u);
  CHECK(rig.message() == "REDO EVENT");
  (void)rig.invoke("edit.undo");
  CHECK(rig.project().pattern(1)->steps[0][0].note == 61);
  // VAL (QA-PAT-413): the pad ramps the volume and sounds the row's note — track 2, the micro-moved NOTE on 009 (instrument 49).
  rig.go(8, 1);
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 1u);
  for (unsigned i = 0; i < 3u; ++i) {  // Time -> Ch -> Data -> Val
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Val);
  (void)rig.invoke("events.pad", 0);
  CHECK(rig.project().pattern(1)->steps[1][8].vol == 0u);  // the ramp's foot
  NEED(sink.downs.size() == 2u);
  CHECK(sink.downs[1].pad == 0u);
  CHECK(sink.downs[1].track == 1u);
  CHECK(sink.downs[1].instrument == 49u);
  CHECK(sink.downs[1].note == 63u);  // note 64, as the OG files number them
  CHECK(rig.state().edit.last_note == 63u);
  rig.tap(Button::F8);
}

// The undo notice names the event unit, not the field in the box: an edit on TIME, then Right to another field and an edit
// on DATA, BACK, undo twice — both notices say UNDO EVENT, whatever field the box holds at undo time.
TEST_CASE("events S2b: the undo notice does not read the field from the view (OS-MIDIREC S2b review)") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP
  rig.go(0, 0);           // track 1: the NOTE on 001 is the first row
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 0u);
  rig.turn(1);             // TIME: one unit (a Micro-move)
  rig.tap(Button::Right);  // Time -> Ch
  rig.tap(Button::Right);  // Ch -> Data
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Data);
  rig.turn(1);  // DATA: one unit (a semitone)
  CHECK(rig.depth() == 2u);
  rig.tap(Button::F8);  // BACK: the field in the box stays Data
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Data);
  rig.tap(Button::F8);  // UNDO on the grid: the DATA unit
  CHECK(rig.project().pattern(1)->steps[0][0].note == 61);
  CHECK(rig.message() == "UNDO EVENT");  // not "UNDO EVENT DATA" read from the box
  rig.flush();                            // let the notice expire, so the next one is the queue's head (invoke runs no pass)
  (void)rig.invoke("edit.undo");          // the TIME unit: the same notice, not the field in the box
  CHECK(rig.project().pattern(1)->steps[0][0].fx[1].type == model::kFxNone);
  CHECK(rig.message() == "UNDO EVENT");
  CHECK(rig.depth() == 0u);
}

// A pad whose write does not happen (the note it enters is the row's own: nothing written, no unit) still sounds, but the
// session's last note is set only when the write happens, as on the grid. And the pad's note comes from the pad board of
// the instrument that sounds — the row's resolved one — so on a beat slice instrument the slice note (24 + pad) is both
// written and heard.
TEST_CASE("events S2b: a refused pad write leaves the last note alone; the note and the sound agree on the row's instrument") {
  Rig rig;
  PadDownLog sink;
  rig.app->attach_preview(&sink);
  rig.tap(Button::Rec);  // STEP
  rig.go(0, 0);           // track 1: the NOTE on 001 (note 61, instrument 1) is the first row
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 0u);
  rig.tap(Button::Right);  // Time -> Ch
  rig.tap(Button::Right);  // Ch -> Data
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Data);
  // The pad whose note is the row's own (60): entering it is a no-op — nothing written, no unit.
  unsigned same = model::edit::kPadCount;
  for (unsigned p = 0; p < model::edit::kPadCount; ++p) {
    if (app::pad_note(&rig.project(), 1, rig.state().edit.pads, p) == 60u) {
      same = p;
      break;
    }
  }
  NEED(same < model::edit::kPadCount);
  CHECK(rig.state().edit.last_note == app::kNoNote);  // nothing played yet
  const unsigned depth = rig.depth();
  (void)rig.invoke("events.pad", static_cast<ActionArg>(same));
  CHECK(rig.depth() == depth);                               // no unit: the write did not happen
  CHECK(rig.project().pattern(1)->steps[0][0].note == 61u);   // the step is unchanged
  CHECK(rig.state().edit.last_note == app::kNoNote);          // a refused write does not change the last note
  NEED(sink.downs.size() == 1u);                             // the pad still sounded
  CHECK(sink.downs[0].pad == same);
  CHECK(sink.downs[0].instrument == 1u);
  CHECK(sink.downs[0].note == 60u);
  rig.tap(Button::F8);
  // The row's instrument is a beat slice one: the pad's note is the slice note (24 + pad) of THAT instrument, and the
  // sound goes to the same instrument — not to the strip's current one with another instrument's board note.
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  in.sampler.play_mode = app::kPlayModeBeatSlice;
  (void)rig.project().set_instrument(48, in);  // instrument 49: the row's instrument on track 2
  rig.go(8, 1);                                 // track 2: the NOTE on 009 (instrument 49) is row 1
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 1u);
  rig.tap(Button::Right);  // Time -> Ch
  rig.tap(Button::Right);  // Ch -> Data
  const unsigned pad = 5;
  const unsigned slice_note = audio::kSliceRootNote + pad;  // 29
  CHECK(app::pad_note(&rig.project(), rig.state().instrument, rig.state().edit.pads, pad) != slice_note);  // the other board's note would differ
  const unsigned depth2 = rig.depth();
  (void)rig.invoke("events.pad", static_cast<ActionArg>(pad));
  CHECK(rig.depth() == depth2 + 1u);                            // the write happened
  CHECK(rig.project().pattern(1)->steps[1][8].note == slice_note + 1u);  // the slice note was written
  CHECK(rig.project().pattern(1)->steps[1][8].instr == 49u);    // the step keeps its instrument
  NEED(sink.downs.size() == 2u);
  CHECK(sink.downs[1].instrument == 49u);  // the sound went to the row's instrument
  CHECK(sink.downs[1].note == slice_note);  // with the note that was written
  CHECK(rig.state().edit.last_note == slice_note);
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: VAL — from --- the first detent is 127, the encoder steps down, a pad ramps 0..127 (QA-PAT-413)") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.go(8, 1);  // track 2: the micro-moved NOTE on 009 has no volume (---)
  rig.open_events();
  NEED(rig.state().events.cursor == 1u);
  for (unsigned i = 0; i < 3u; ++i) {  // Time -> Ch -> Data -> Val
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Val);
  const unsigned depth = rig.depth();
  rig.turn(1);  // kFirstVolume: 127
  CHECK(rig.project().pattern(1)->steps[1][8].vol == model::kVolumeMax);
  CHECK(rig.depth() == depth + 1u);
  rig.turn(-1);
  CHECK(rig.project().pattern(1)->steps[1][8].vol == 126u);
  CHECK(rig.depth() == depth + 2u);
  (void)rig.invoke("events.pad", 47);  // the ramp's top
  CHECK(rig.project().pattern(1)->steps[1][8].vol == model::kVolumeMax);
  (void)rig.invoke("events.pad", 0);  // the ramp's foot
  CHECK(rig.project().pattern(1)->steps[1][8].vol == 0u);
  CHECK(rig.depth() == depth + 4u);
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: TIME moves the note by microsteps and past .96 by a whole step, the cursor follows, the end moves with it (QA-PAT-414, 415)") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.go(0, 0);
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  const unsigned depth = rig.depth();
  rig.turn(24);  // 001.00 -> 001.50: a Micro-move, no step move
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.project().pattern(1)->steps[0][0].note == 61);
  CHECK(rig.project().pattern(1)->steps[0][0].fx[1].type == ev::kFxMicroMove);
  CHECK(rig.project().pattern(1)->steps[0][0].fx[1].value == ev::micro_value(24));
  CHECK(rig.state().events.cursor == 0u);
  rig.turn(24);  // past .96: the note goes to step 2 at .00, its end (the OFF on 005) moves to 006
  CHECK(rig.depth() == depth + 2u);
  const model::Pattern* pat = rig.project().pattern(1);
  CHECK(pat->steps[0][0].note == model::kNoteEmpty);
  CHECK(pat->steps[0][1].note == 61);
  CHECK(pat->steps[0][1].instr == 1);
  CHECK(pat->steps[0][1].vol == 100);
  CHECK(pat->steps[0][1].fx[1].type == model::kFxNone);  // offset 0 removes the Micro-move (A6)
  CHECK(pat->steps[0][4].note == model::kNoteEmpty);   // the old end cleared
  CHECK(pat->steps[0][5].note == model::kNoteOff);     // the end moved by the same whole step (A21)
  ev::Row row;
  NEED(app::events_cursor_row(rig.project(), 1, rig.state().events, row));
  CHECK(row.key.step == 1u);  // the cursor followed the moved row (3.2)
  // A refused move (QA-PAT-414): the target step holds a note — nothing written, no unit, the strip says so.
  model::Step blocker = model::kEmptyStep;
  blocker.note = 40;
  (void)rig.editor->set_step(1, 0, 2, blocker);  // step 003 (0-based 2) holds a note (the test's own setup; through the Editor, so the journal stays in sync)
  const unsigned depth2 = rig.depth();
  rig.turn(48);  // from 002 (0-based 1) +48 microsteps -> 003 (0-based 2), which holds a note
  CHECK(rig.depth() == depth2);
  CHECK(head_text(rig) == "STEP 003 HOLDS A NOTE");
  CHECK(rig.project().pattern(1)->steps[0][1].note == 61);  // the note did not move
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: Shift + encoder moves a note one step with its LEN; its CC rows stay on their step (QA-PAT-415, 416)") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.go(0, 1);  // track 2: the CC A on 001 (row 0), the micro-moved NOTE on 009.50 (row 1)
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  rig.tap(Button::Down);
  NEED(rig.state().events.cursor == 1u);
  const unsigned depth = rig.depth();
  rig.press(Button::Shift);
  rig.pass(1);
  rig.turn(1);  // coarse: a whole step, the microstep keeps
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.depth() == depth + 1u);
  const model::Pattern* pat = rig.project().pattern(1);
  CHECK(pat->steps[1][8].note == model::kNoteEmpty);  // the source cleared
  CHECK(pat->steps[1][9].note == 64);                 // the note on 010, still micro-moved at .50
  CHECK(pat->steps[1][9].fx[1].type == ev::kFxMicroMove);
  CHECK(pat->steps[1][0].fx[0].type == ev::kCcFirst);  // the CC A stays on 001
  ev::Row row;
  NEED(app::events_cursor_row(rig.project(), 1, rig.state().events, row));
  CHECK(row.key.step == 9u);  // the cursor followed the moved row
  CHECK(row.key.kind == ev::Kind::Note);
  // A CC row moves by whole steps only (A21): the encoder on its TIME goes one step.
  rig.tap(Button::Up);  // back to the CC A row (now still on 001)
  NEED(rig.state().events.cursor == 0u);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Time);
  rig.turn(1);
  CHECK(rig.depth() == depth + 2u);
  CHECK(pat->steps[1][0].fx[0].type == model::kFxNone);  // cleared at the source
  CHECK(pat->steps[1][1].fx[0].type == ev::kCcFirst);  // CC A on 002, same column
  CHECK(pat->steps[1][1].fx[0].value == 74);
  NEED(app::events_cursor_row(rig.project(), 1, rig.state().events, row));
  CHECK(row.key.step == 1u);
  CHECK(row.key.kind == ev::Kind::Cc);
  CHECK(row.key.column == 0u);
  // The refusal: both FX columns of the target are taken (A21).
  model::Step full = model::kEmptyStep;
  full.fx[0] = {14, 47};
  full.fx[1] = {ev::kFxGate, 50};
  (void)rig.editor->set_step(1, 1, 2, full);  // through the Editor: a direct project write would clear the journal on the next group
  const unsigned depth3 = rig.depth();
  rig.turn(1);  // CC A from 002 to 003, whose both columns are taken
  CHECK(rig.depth() == depth3);
  CHECK(head_text(rig) == "STEP 003: NO FREE FX COLUMN");
  CHECK(pat->steps[1][1].fx[0].type == ev::kCcFirst);  // nothing moved
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: LEN moves the end a step; a gated note refuses with THE GATE FX SETS THIS LENGTH (QA-PAT-417)") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.go(0, 0);
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  for (unsigned i = 0; i < 4u; ++i) {  // Time -> Ch -> Data -> Val -> Len
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Len);
  const unsigned depth = rig.depth();
  rig.turn(1);  // the OFF moves from 005 to 006
  CHECK(rig.depth() == depth + 1u);
  const model::Pattern* pat = rig.project().pattern(1);
  CHECK(pat->steps[0][4].note == model::kNoteEmpty);
  CHECK(pat->steps[0][5].note == model::kNoteOff);
  CHECK(rig.state().events.cursor == 0u);  // the cursor stays on the NOTE row
  rig.tap(Button::F8);
  // A Gate FX on the note's step: the LEN edit is refused (A19).
  model::Step gated = model::kEmptyStep;
  gated.note = 61;
  gated.instr = 1;
  gated.fx[0] = {ev::kFxGate, 50};
  (void)rig.editor->set_step(1, 0, 0, gated);  // through the Editor, as above
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  for (unsigned i = 0; i < 4u; ++i) {
    rig.tap(Button::Right);
  }
  const unsigned depth2 = rig.depth();
  rig.turn(1);
  CHECK(rig.depth() == depth2);
  CHECK(head_text(rig) == "THE GATE FX SETS THIS LENGTH");
  CHECK(rig.project().pattern(1)->steps[0][5].note == model::kNoteOff);  // the end (moved to 006 above) did not move
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: TYPE cycles the CC letter (the value stays), VAL of a CC row is the FX value with the pad ramp (QA-PAT-418)") {
  Rig rig;
  rig.tap(Button::Rec);
  rig.go(0, 1);  // track 2, row 0: the CC A 074 on 001
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  rig.tap(Button::Right);  // Time -> Type
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Type);
  const unsigned depth = rig.depth();
  rig.turn(5);  // CC A -> CC F (type 37), the value stays
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.project().pattern(1)->steps[1][0].fx[0].type == ev::kCcF);
  CHECK(rig.project().pattern(1)->steps[1][0].fx[0].value == 74);
  for (unsigned i = 0; i < 3u; ++i) {  // Type -> Ch -> Data -> Val
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Val);
  rig.turn(1);
  CHECK(rig.project().pattern(1)->steps[1][0].fx[0].value == 75);
  CHECK(rig.depth() == depth + 2u);
  (void)rig.invoke("events.pad", 0);  // the ramp's foot
  CHECK(rig.project().pattern(1)->steps[1][0].fx[0].value == 0);
  CHECK(rig.depth() == depth + 3u);
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: CH picks the MIDI instrument on that channel, else refuses (QA-PAT-419); IN walks the instruments (QA-PAT-420)") {
  Rig rig;
  // A second MIDI instrument, slot 47 = instrument 48 on channel 5.
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 5;
  std::memcpy(in.name, "MIDI 5", 6);
  (void)rig.project().set_instrument(47, in);
  rig.tap(Button::Rec);
  rig.go(8, 1);  // the NOTE on 009.50 of track 2, instrument 49 (channel 3)
  rig.open_events();
  NEED(rig.state().events.cursor == 1u);
  rig.tap(Button::Right);  // Time -> Ch
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Ch);
  const unsigned depth = rig.depth();
  rig.turn(2);  // channel 3 -> 5: instrument 48
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.project().pattern(1)->steps[1][8].instr == 48);
  rig.turn(1);  // channel 6: none
  CHECK(rig.depth() == depth + 1u);
  CHECK(head_text(rig) == "NO MIDI INSTRUMENT ON CH 06");
  CHECK(rig.project().pattern(1)->steps[1][8].instr == 48);
  // IN (QA-PAT-420): the grid's Instr walk.
  for (unsigned i = 0; i < 4u; ++i) {  // Ch -> Data -> Val -> Len -> In
    rig.tap(Button::Right);
  }
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::In);
  rig.turn(-1);  // the Instr walk from 48 (the order's last) down is 47; up from 48 stops: no-op, no unit
  CHECK(rig.depth() == depth + 2u);
  CHECK(rig.project().pattern(1)->steps[1][8].instr == 47u);
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: an empty list — the edit keys say NO EVENTS: INSERT ADDS ONE in STEP, TURN EDIT ON in WATCH; no unit (QA-PAT-424)") {
  Rig rig;
  rig.select_pattern(3);  // exists, empty
  rig.tap(Button::Rec);   // STEP
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  const unsigned depth = rig.depth();
  rig.turn(1);
  CHECK(head_text(rig) == "NO EVENTS: INSERT ADDS ONE");
  CHECK(rig.depth() == depth);
  (void)rig.invoke("events.pad", 0);
  CHECK(head_text(rig) == "NO EVENTS: INSERT ADDS ONE");
  CHECK(rig.depth() == depth);
  rig.tap(Button::F8);
  rig.flush();  // with the overlay down the strip's head runs out (head() is the oldest live entry; the WATCH check below posts a new one)
  rig.tap(Button::Rec);  // WATCH
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  rig.turn(1);
  CHECK(head_text(rig) == "TURN EDIT ON");
  CHECK(rig.depth() == depth);
  rig.tap(Button::F8);
}

TEST_CASE("events S2b: a pad on another field sounds the row's step (a pad preview, A14) — in WATCH too") {
  struct PadLog final : app::PreviewSink {
    struct Call {
      unsigned pad, track, instrument, note;
    };
    std::vector<Call> calls;
    void preview(unsigned, unsigned) noexcept override {}
    void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override { calls.push_back({pad, track, instrument, note}); }
  };
  Rig rig;
  PadLog sink;
  rig.app->attach_preview(&sink);
  rig.go(8, 1);  // track 2: the NOTE on 009.50 (row 1)
  rig.open_events();
  NEED(rig.state().events.cursor == 1u);
  CHECK(static_cast<app::EventsView::Field>(rig.state().events.field) == app::EventsView::Field::Time);  // a non-writing field
  const unsigned depth = rig.depth();
  (void)rig.invoke("events.pad", 3);  // TIME field: the row's step sounds
  NEED(sink.calls.size() == 1u);
  CHECK(sink.calls[0].pad == 3u);
  CHECK(sink.calls[0].track == 1u);
  CHECK(sink.calls[0].instrument == 49u);
  CHECK(sink.calls[0].note == 63u);  // note 64, as the OG files number them
  CHECK(rig.depth() == depth);       // no unit from a sounding pad
  rig.tap(Button::F8);
}

// ---- S2c: Insert, Delete and the history inside the list (spec A9, A10, A12, A13; QA-PAT-421, 422, 423, 424) --------------------------------

namespace {

void shifted_tap(Rig& rig, Button b) {
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(b);
  rig.release(Button::Shift);
  rig.pass(1);
}

ev::Key cursor_key(Rig& rig) {
  ev::Row row{};
  CHECK(app::events_cursor_row(rig.project(), 1, rig.state().events, row));
  return row.key;
}

}  // namespace

TEST_CASE("events S2c: Insert copies a note onto its end step; a note with nothing below refuses; each write is one unit and the cursor goes to the new row (QA-PAT-421)") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP
  rig.go(0, 0);          // track 1: the NOTE on 001 (its OFF on 005 is folded)
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 0u);
  const unsigned d0 = rig.depth();
  rig.tap(Button::Insert);  // A9: a copy of the note (note, instrument, volume) on the row's end step; it replaces the OFF
  const model::Step ended = rig.project().pattern(1)->steps[0][4];
  CHECK(ended.note == 61);
  CHECK(ended.instr == 1);
  CHECK(ended.vol == 100);
  CHECK(rig.depth() == d0 + 1u);
  CHECK(rig.state().events.cursor == 1u);  // the cursor is on the new row, the NOTE on 005
  CHECK(cursor_key(rig) == ev::Key{0, 4, ev::Kind::Note, 0});
  // The new note has nothing below it: no end to copy onto, so Insert refuses and writes nothing.
  const unsigned d1 = rig.depth();
  rig.tap(Button::Insert);
  CHECK(rig.depth() == d1);
  CHECK(head_text(rig) == "NO FREE STEP AFTER THIS NOTE");
  rig.tap(Button::F8);
  rig.flush();
  // Shift + Insert on a NOTE row adds a CC A 000 in the first free FX column of its step (the first one: FX 1).
  rig.go(0, 0);
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  shifted_tap(rig, Button::Insert);
  CHECK(rig.project().pattern(1)->steps[0][0].fx[0].type == ev::kCcFirst);
  CHECK(rig.project().pattern(1)->steps[0][0].fx[0].value == 0);
  CHECK(rig.depth() == d1 + 1u);
  CHECK(cursor_key(rig) == ev::Key{0, 0, ev::Kind::Cc, 0});  // a CC row sorts before the NOTE row at the same time
  CHECK(rig.state().events.cursor == 0u);
  // Insert on that CC row: the same letter and value on the next step of the scope that has a free FX column (step 2).
  rig.tap(Button::Insert);
  CHECK(rig.project().pattern(1)->steps[0][1].fx[0].type == ev::kCcFirst);
  CHECK(rig.depth() == d1 + 2u);
  CHECK(cursor_key(rig) == ev::Key{0, 1, ev::Kind::Cc, 0});
  rig.tap(Button::F8);
  rig.flush();
  rig.tap(Button::F8);  // UNDO on the grid: the Insert's unit is named for the event too
  CHECK(rig.message() == "UNDO EVENT");
  CHECK(rig.project().pattern(1)->steps[0][1].fx[0].type == model::kFxNone);
}

TEST_CASE("events S2c: Insert and Shift + Insert on an empty list add a NOTE or a CC A at the scope's first step (QA-PAT-421)") {
  Rig rig;
  rig.select_pattern(3);  // exists, empty
  rig.tap(Button::Rec);   // STEP
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  const unsigned d0 = rig.depth();
  rig.tap(Button::Insert);
  CHECK(ev::note_is_note(rig.project().pattern(3)->steps[0][0].note));
  CHECK(rig.depth() == d0 + 1u);
  CHECK(cursor_key(rig) == ev::Key{0, 0, ev::Kind::Note, 0});
  rig.tap(Button::F7);  // undo inside the list: the list is empty again
  CHECK(rig.depth() == d0);
  CHECK(rig.project().pattern(3)->steps[0][0].note == model::kNoteEmpty);
  NEED(rig.state().overlay == OverlayId::Events);
  shifted_tap(rig, Button::Insert);
  CHECK(rig.project().pattern(3)->steps[0][0].fx[0].type == ev::kCcFirst);
  CHECK(rig.depth() == d0 + 1u);
  rig.tap(Button::F8);
}

TEST_CASE("events S2c: Delete clears a row's owned bytes — the end with a NOTE, the column of a CC, an end row alone; the note above keeps its LEN (QA-PAT-422)") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP
  // A second note at 009 on track 1 ends the first note's run where its OFF was: the OFF on 005 is the first note's end.
  rig.go(0, 0);
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  const unsigned d0 = rig.depth();
  rig.tap(Button::Delete);  // the NOTE row: its bytes and its end (the OFF on 005) go
  CHECK(rig.project().pattern(1)->steps[0][0].note == model::kNoteEmpty);
  CHECK(rig.project().pattern(1)->steps[0][4].note == model::kNoteEmpty);
  CHECK(rig.depth() == d0 + 1u);
  ev::Row gone;
  CHECK_FALSE(app::events_cursor_row(rig.project(), 1, rig.state().events, gone));  // the track is empty now
  rig.tap(Button::F7);  // undo inside the list: both come back
  CHECK(rig.project().pattern(1)->steps[0][0].note == 61);
  CHECK(rig.project().pattern(1)->steps[0][4].note == model::kNoteOff);
  CHECK(rig.depth() == d0);
  rig.tap(Button::F8);
  // A CC row: its FX column only. Track 2: the CC A on 001 is the first row.
  rig.go(0, 1);
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  CHECK(cursor_key(rig) == ev::Key{1, 0, ev::Kind::Cc, 0});
  rig.tap(Button::Delete);
  CHECK(rig.project().pattern(1)->steps[1][0].fx[0].type == model::kFxNone);
  CHECK(rig.depth() == d0 + 1u);
  CHECK(cursor_key(rig).kind == ev::Kind::Note);  // the nearest row in time is the micro-moved note on 009
  rig.tap(Button::F8);
}

TEST_CASE("events S2c: Delete of a note that ends the note above writes an OFF there, so the note above keeps its LEN (A10, I2)") {
  Rig rig;
  // Track 1 gets a second note on 003, so the first note (001) ends there: its OFF on 005 is the second note's end.
  model::Step n = model::kEmptyStep;
  n.note = 63;
  n.instr = 1;
  NEED(rig.editor->set_step(1, 0, 2, n) == model::Status::Ok);
  rig.tap(Button::Rec);  // STEP
  rig.go(0, 0);
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  ev::Row first;
  NEED(app::events_cursor_row(rig.project(), 1, rig.state().events, first));
  CHECK(first.len_steps == 2u);  // the NOTE on 001 runs to the one on 003
  rig.tap(Button::Down);         // the NOTE on 003
  CHECK(cursor_key(rig) == ev::Key{0, 2, ev::Kind::Note, 0});
  rig.tap(Button::Delete);
  CHECK(rig.project().pattern(1)->steps[0][2].note == model::kNoteOff);  // the OFF that keeps the first note's LEN
  CHECK(rig.project().pattern(1)->steps[0][4].note == model::kNoteEmpty);  // its own end went with it
  NEED(app::events_cursor_row(rig.project(), 1, rig.state().events, first));
  CHECK(first.key == ev::Key{0, 0, ev::Kind::Note, 0});
  CHECK(first.len_steps == 2u);
  rig.tap(Button::F8);
}

TEST_CASE("events S2c: Insert and Delete in WATCH say TURN EDIT ON and write nothing (QA-PAT-424)") {
  Rig rig;
  rig.go(0, 0);
  rig.open_events();  // WATCH: no Rec
  NEED(rig.state().overlay == OverlayId::Events);
  const unsigned d0 = rig.depth();
  const model::Step s0 = rig.project().pattern(1)->steps[0][0];
  const model::Step s4 = rig.project().pattern(1)->steps[0][4];
  rig.tap(Button::Insert);
  CHECK(head_text(rig) == "TURN EDIT ON");
  rig.tap(Button::Delete);
  shifted_tap(rig, Button::Insert);
  CHECK(rig.depth() == d0);
  CHECK(std::memcmp(&s0, &rig.project().pattern(1)->steps[0][0], sizeof s0) == 0);
  CHECK(std::memcmp(&s4, &rig.project().pattern(1)->steps[0][4], sizeof s4) == 0);
  rig.tap(Button::F8);
}

TEST_CASE("events S2c: F7 and Shift + F7 undo and redo inside the list, the list projects again and the cursor keeps its event; F8 on the grid undoes the same units (QA-PAT-423)") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP
  rig.go(8, 1);          // track 2: the micro-moved NOTE on 009.50 is the second row
  rig.open_events();
  NEED(rig.state().events.cursor == 1u);
  const unsigned d0 = rig.depth();
  rig.turn(1);  // TIME: one microstep, one unit
  rig.turn(1);  // ... another
  CHECK(rig.depth() == d0 + 2u);
  const model::Step edited = rig.project().pattern(1)->steps[1][8];
  rig.tap(Button::F7);  // UNDO inside the list: the overlay stays, the strip says UNDO EVENT
  CHECK(rig.state().overlay == OverlayId::Events);
  CHECK(rig.depth() == d0 + 1u);
  CHECK(head_text(rig) == "UNDO EVENT");
  CHECK(rig.state().events.cursor == 1u);
  CHECK(cursor_key(rig) == ev::Key{1, 8, ev::Kind::Note, 0});
  shifted_tap(rig, Button::F7);  // REDO
  CHECK(rig.depth() == d0 + 2u);
  CHECK(std::memcmp(&edited, &rig.project().pattern(1)->steps[1][8], sizeof edited) == 0);
  CHECK(rig.state().overlay == OverlayId::Events);
  rig.tap(Button::F7);
  rig.tap(Button::F7);
  CHECK(rig.depth() == d0);
  rig.tap(Button::F7);  // nothing left of the list's own: the history of the grid says so, writes nothing
  CHECK(rig.depth() == d0);
  CHECK(rig.state().overlay == OverlayId::Events);
  // The grid's F8 undoes the same units after BACK.
  rig.turn(1);
  CHECK(rig.depth() == d0 + 1u);
  rig.tap(Button::F8);  // BACK
  rig.tap(Button::F8);  // UNDO on the grid
  CHECK(rig.depth() == d0);
}

// ---- S3: QUANT (spec S3; QA-PAT-426, 427, 428) ---------------------------------------------------------------------------------------------

TEST_CASE("events S3: F2 quantises the row — 009.50 goes to step 10 at .00 as one unit; a CC row says CC IS ON THE STEP; WATCH says TURN EDIT ON (QA-PAT-426, 428, 424)") {
  Rig rig;
  rig.tap(Button::Rec);  // STEP
  rig.go(8, 1);          // track 2: the CC A on 001 is the first row, the NOTE on 009.50 the second
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  NEED(rig.state().events.cursor == 1u);
  const model::Step moved = rig.project().pattern(1)->steps[1][8];
  const unsigned d0 = rig.depth();
  rig.tap(Button::F2);
  CHECK(rig.project().pattern(1)->steps[1][8].note == model::kNoteEmpty);
  const model::Step& landed = rig.project().pattern(1)->steps[1][9];
  CHECK(landed.note == moved.note);
  CHECK(landed.instr == moved.instr);
  CHECK(landed.fx[0].type == model::kFxNone);
  CHECK(landed.fx[1].type == model::kFxNone);  // the Micro-move goes: the note is on the step
  CHECK(rig.depth() == d0 + 1u);
  CHECK(rig.state().events.cursor == 1u);  // the cursor follows the moved row
  CHECK(cursor_key(rig) == ev::Key{1, 9, ev::Kind::Note, 0});
  rig.tap(Button::F2);  // already on the step: nothing to do, and it says so
  CHECK(head_text(rig) == "ALREADY ON THE STEP");
  CHECK(rig.depth() == d0 + 1u);
  rig.tap(Button::F7);  // one undo unit takes it back
  CHECK(std::memcmp(&moved, &rig.project().pattern(1)->steps[1][8], sizeof moved) == 0);
  CHECK(rig.depth() == d0);
  rig.tap(Button::F8);
  rig.flush();
  // A CC row: QUANT leaves it (a CC has no sub-step time).
  rig.go(0, 1);
  rig.open_events();
  NEED(rig.state().events.cursor == 0u);
  CHECK(cursor_key(rig).kind == ev::Kind::Cc);
  rig.tap(Button::F2);
  CHECK(head_text(rig) == "CC IS ON THE STEP");
  CHECK(rig.depth() == d0);
  rig.tap(Button::F8);
  rig.flush();
  // WATCH: the key says TURN EDIT ON and writes nothing.
  rig.tap(Button::Rec);
  rig.go(8, 1);
  rig.open_events();
  rig.tap(Button::F2);
  CHECK(head_text(rig) == "TURN EDIT ON");
  CHECK(rig.depth() == d0);
  shifted_tap(rig, Button::F2);
  CHECK(rig.depth() == d0);
  rig.tap(Button::F8);
}

TEST_CASE("events S3: Shift + F2 quantises every NOTE and end row the filter shows as one unit and counts what the rule left (QA-PAT-427)") {
  Rig rig;
  const auto note = [](std::uint8_t n, unsigned offset) {
    model::Step s = model::kEmptyStep;
    s.note = n;
    s.instr = 1;
    if (offset > 0) {
      s.fx[1] = {ev::kFxMicroMove, static_cast<std::uint8_t>(ev::micro_value(static_cast<int>(offset)))};
    }
    return s;
  };
  // Track 1 (the Rig's: a note on 001, an OFF on 005): 011 is 10 microsteps late (snaps back), 013 and 014 are 30 late (014
  // moves to 015, and 013 is refused: 014's move leaves the OFF of 013's own end on 014, and the target is never an empty note
  // column (A21) — STEP TAKEN: 013 stays), 021 is 30 late with a note on grid on 022 (STEP TAKEN: it stays).
  NEED(rig.editor->set_step(1, 0, 10, note(62, 10)) == model::Status::Ok);
  NEED(rig.editor->set_step(1, 0, 12, note(63, 30)) == model::Status::Ok);
  NEED(rig.editor->set_step(1, 0, 13, note(64, 30)) == model::Status::Ok);
  NEED(rig.editor->set_step(1, 0, 20, note(65, 30)) == model::Status::Ok);
  NEED(rig.editor->set_step(1, 0, 21, note(66, 0)) == model::Status::Ok);
  rig.tap(Button::Rec);  // STEP
  rig.go(0, 0);
  rig.open_events();
  NEED(rig.state().overlay == OverlayId::Events);
  const unsigned d0 = rig.depth();
  static model::Pattern before;
  before = *rig.project().pattern(1);
  shifted_tap(rig, Button::F2);
  const model::Pattern& after = *rig.project().pattern(1);
  CHECK(after.steps[0][10].note == 62);
  CHECK(after.steps[0][10].fx[1].type == model::kFxNone);
  CHECK(after.steps[0][12].note == 63);  // STEP TAKEN: left as it was
  CHECK(after.steps[0][12].fx[1].type == ev::kFxMicroMove);
  CHECK(after.steps[0][13].note == model::kNoteOff);  // 014's note moved on; the OFF of 013's own end stays (I2)
  CHECK(after.steps[0][13].fx[1].type == model::kFxNone);
  CHECK(after.steps[0][14].note == 64);
  CHECK(after.steps[0][14].fx[1].type == model::kFxNone);
  CHECK(after.steps[0][20].note == 65);                       // STEP TAKEN: left as it was
  CHECK(after.steps[0][20].fx[1].type == ev::kFxMicroMove);
  CHECK(after.steps[0][21].note == 66);
  CHECK(rig.depth() == d0 + 1u);  // one unit for the whole operation
  CHECK(head_text(rig) == "QUANTISED 2, 2 LEFT (STEP TAKEN)");
  rig.tap(Button::F7);  // one undo takes the whole operation back, byte for byte
  CHECK(rig.depth() == d0);
  CHECK(std::memcmp(&before, rig.project().pattern(1), sizeof before) == 0);
  // The CC filter shows no NOTE or end row: there is nothing to quantise, and nothing is written.
  rig.tap(Button::F8);
  rig.flush();
  rig.go(0, 0);
  rig.open_events();
  rig.tap(Button::F1);
  rig.tap(Button::F1);
  CHECK(rig.state().events.filter == ev::Filter::Cc);
  shifted_tap(rig, Button::F2);
  CHECK(head_text(rig) == "NOTHING TO QUANTISE");
  CHECK(rig.depth() == d0);
  rig.tap(Button::F8);
}
