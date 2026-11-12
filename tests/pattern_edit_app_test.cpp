// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-009 P1a.1 in the app: the Pattern screen on a document, the modes, the field level of the binding chain, the selection, the edit actions
// through their bindings, the choosers, mute and solo, and the undo of all of it. The rules of one step are pattern_edit_rules_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/frame.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/pattern_tools_view.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/song_source.h>
#include <o3os/app/view_state.h>
#include <o3os/core/action.h>
#include <o3os/app/screens/scene.h>
#include <o3os/core/input_event.h>
#include <o3os/core/scales.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/midi/cc_in.h>
#include <o3os/model/editor.h>
#include <o3os/model/fx_table.h>
#include <o3os/model/pattern_verbs.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/components.h>
#include <o3os/ui/font.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/key_row.h"
#include "support/need.h"
#include "data/pattern_edit_vectors.h"
#include "support/step_vectors.h"
#include "support/project_fixture.h"
#include "support/rng.h"

using namespace o3os;
using app::Button;
using app::Column;
using app::DispatchResult;
using app::PatternMode;
using core::ActionArg;
namespace vectors = o3os::test::vectors;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

constexpr unsigned kPattern = static_cast<unsigned>(app::ScreenId::Pattern);

// An App with a document attached: pattern 1 of `length` steps on every track (0: no pattern yet), one song position.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  model::UndoHistory history;  // `rings`: the device's three rings (a structural unit then fences the others: OS-UNDOGRP D4)
  std::unique_ptr<model::Editor> editor;

  // `ring_bytes`: the undo region's size (the device's is the whole g_ring); a smaller one makes a unit too big for the Pattern ring (5/12 of it) reachable.
  explicit Rig(bool with_document = true, unsigned length = 64, bool rings = false, std::size_t ring_bytes = sizeof g_ring) {
    model::Project& p = test::arena(0).project;
    p.reset();
    if (with_document) {
      if (length != 0) {
        (void)p.create_pattern(1, length);
      }
      (void)p.append_order(1);
      if (rings) {
        history.begin(g_ring, ring_bytes);
        editor = std::make_unique<model::Editor>(p, history);
      } else {
        journal.begin(g_ring, ring_bytes);
        editor = std::make_unique<model::Editor>(p, journal);
      }
      app->attach_document(editor.get());
    }
    app->run_once();
  }
  model::Project& project() { return test::arena(0).project; }
  const app::ViewState& state() const { return app->state(); }
  const app::PatternView& view() const { return app->view(); }
  const app::PatternEdit& edit() const { return app->state().edit; }

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
  // OS-PATSWITCH: the PATTERN chooser is the hold of F1: down until the Hold fires, and the release.
  void hold_key(Button b) {
    press(b);
    pass(core::kGestureTimings.hold_ms + 1);
  }
  void release_key(Button b) {
    release(b);
    pass(1);
  }
  // Lets every notification run out (the first pass starts the clock of the one just posted).
  void flush() {
    pass(1);
    pass(10000);
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  // OS-UXF-PAT 7: the pads' modes are the PADS picker's (Shift + F2, then F1..F6 = NOTES PIANO ISO TRACKS PATS VU); `old` is the former Shift key of the mode (F2 NOTES .. F7 VU).
  void pad_mode(Button old) {
    shift_tap(Button::F2);
    tap(static_cast<Button>(static_cast<unsigned>(old) - 1u));
  }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass(1);
  }
  // OS-SHIFTENC: the encoder turned with Shift held.
  void shift_turn(int detents) {
    press(Button::Shift);
    pass(1);
    turn(detents);
    release(Button::Shift);
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  void mode(PatternMode m) { (void)invoke("view.mode", static_cast<ActionArg>(m)); }
  void edit_mode() { mode(PatternMode::Edit); }
  void column(Column c) { (void)invoke("pattern.column", static_cast<ActionArg>(c)); }
  // The cursor to (row, track) in the Note column.
  void go(int row, int track = 0) {
    (void)invoke("cursor.jump_fraction", 0);
    (void)invoke("nav.scroll", row);
    (void)invoke("cursor.home");
    for (int i = 0; i < track * 5; ++i) {
      (void)invoke("nav.right");
    }
  }
  // OS-AUDIT-56: Shift + Right goes a column at a time inside the first track, then takes whole tracks. These press it (by the action, or as a tap with Shift
  // already held) until the block has `n` more tracks.
  void extend_tracks(int n) {
    const int target = view().track() + n;
    for (int i = 0; i < 40 && view().track() < target; ++i) {
      (void)invoke("select.extend", 10);
    }
  }
  void tap_tracks(int n) {
    const int target = view().track() + n;
    for (int i = 0; i < 40 && view().track() < target; ++i) {
      tap(Button::Right);
    }
  }
  const model::Step& step(int track, int row) {
    static const model::Step kNone = model::kEmptyStep;
    const model::Pattern* p = project().pattern(1);
    return p != nullptr ? p->steps[track][row] : kNone;
  }
  std::string message() const { return app->message(); }
  unsigned depth() const { return editor != nullptr ? editor->journal().depth() : journal.depth(); }  // (the Pattern ring)
};

// The pad of the default board (scale 0, offset 12, root 36) with the lowest note: C-3, model byte 37.
constexpr unsigned kPadC3 = 36;

}  // namespace

// ---- the screen on a document ---------------------------------------------------------------------------------------------------------

TEST_CASE("pattern edit: without a document the Pattern screen is the demo, read-only: every edit is Disabled and says NO PROJECT") {
  Rig rig(false);
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Browse));
  const app::PatternSource src(rig.app->model(), rig.edit().pattern);
  CHECK(src.is_demo());
  CHECK(src.length() == 64);
  CHECK(src.tracks() == 12);
  // In Browse the edit actions are Disabled and say nothing (those keys are unbound outside edit mode).
  CHECK(rig.invoke("step.note", 61) == DispatchResult::Disabled);
  CHECK(rig.message() == "");
  CHECK(rig.invoke("step.clear") == DispatchResult::Disabled);
  // Rec turns the edit on (it is view state), and then an edit says why it does nothing.
  rig.tap(Button::Rec);
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Edit));
  CHECK(rig.invoke("step.note", 61) == DispatchResult::Disabled);
  CHECK(rig.message() == "NO PROJECT");
  rig.flush();
  CHECK(rig.message() == "");
  rig.tap(Button::Delete);
  CHECK(rig.message() == "NO PROJECT");
  // The F-keys of the choosers are still labels without a document: there is nothing to choose among.
  rig.flush();
  rig.tap(Button::F1);
  CHECK(rig.message() == "F1 PATTERN");
  CHECK(rig.state().overlay == app::OverlayId::None);
  // Mute and solo are edits of the project too: Disabled with the same words.
  rig.flush();
  rig.press(Button::Shift);
  rig.pass(1);
  rig.pad(12);  // the TRACKS layer's MUTE row (OS-PADS-A)
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.message() == "NO PROJECT");
}

TEST_CASE("pattern edit: the screen reads the document: its length, its tracks, the strip, and a pattern that is not there yet") {
  Rig rig(true, 48);
  const app::PatternSource src(rig.app->model(), 1);
  CHECK_FALSE(src.is_demo());
  CHECK(src.exists());
  CHECK(src.length() == 48);
  CHECK(src.tracks() == 12);  // a new project has 12 active tracks
  CHECK(src.strip_pattern() == 1);
  CHECK(src.strip_patterns() == 1);
  // A track's own length counts: the pattern is as long as the longest of the active tracks.
  (void)rig.project().set_track_length(1, 3, 20);
  CHECK(src.length() == 48);
  CHECK(src.track_length(3) == 20);
  // Names, types, flags come from the track slots.
  model::TrackSlot slot = *rig.project().track(2);
  slot.type = model::TrackType::Midi;
  slot.flags = model::kTrackMute | model::kTrackSolo;
  std::memcpy(slot.name, "BASS", 5);
  (void)rig.project().set_track(2, slot);
  CHECK(src.track_type(2) == 'M');
  CHECK(std::string(src.track_name(2)) == "BASS");
  CHECK((src.muted(2) && src.soloed(2)));
  CHECK((!src.muted(1) && !src.soloed(1)));
  CHECK(src.track_type(0) == 'A');
  // A pattern that does not exist: empty, with the project's default length, and a cursor that can move over it.
  const app::PatternSource missing(rig.app->model(), 7);
  CHECK_FALSE(missing.exists());
  CHECK(missing.length() == static_cast<int>(model::kDefaultPatternLength));
  CHECK(model::is_empty(missing.step(0, 0)));
  CHECK(missing.strip_patterns() == 1);

  // The first edit of a pattern that does not exist creates it, in the same undo unit.
  Rig fresh(true, 0);
  fresh.edit_mode();
  CHECK(fresh.project().pattern(1) == nullptr);
  CHECK(fresh.invoke("step.note", 50) == DispatchResult::Changed);
  NEED(fresh.project().pattern(1) != nullptr);
  CHECK(fresh.project().pattern_length(1) == model::kDefaultPatternLength);
  CHECK(fresh.step(0, 0).note == 50);
  CHECK(fresh.depth() == 1u);  // one unit: the pattern and the note
  CHECK(fresh.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(fresh.project().pattern(1) == nullptr);
  CHECK(fresh.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(fresh.step(0, 0).note == 50);
  // A no-op edit on a missing pattern creates nothing (a Delete on nothing).
  Rig nothing(true, 0);
  nothing.edit_mode();
  CHECK(nothing.invoke("step.clear") == DispatchResult::Changed);  // the cursor still moves
  CHECK(nothing.project().pattern(1) == nullptr);
  CHECK(nothing.depth() == 0u);
}

TEST_CASE("pattern edit: quarter jumps on the model-backed grid, lengths 1, 3, 16, 33, 64, 100, 128, and the end") {
  for (const unsigned len : {1u, 3u, 16u, 33u, 64u, 100u, 128u}) {
    Rig rig(true, len);
    CHECK(rig.app->model().project != nullptr);
    for (int n = 0; n < 4; ++n) {
      (void)rig.invoke("cursor.jump_fraction", n);
      CHECK_MESSAGE(rig.view().row() == static_cast<int>(static_cast<unsigned>(n) * len / 4u), "length " << len << " quarter " << n);
    }
    (void)rig.invoke("cursor.jump_end");
    CHECK_MESSAGE(rig.view().row() == static_cast<int>(len) - 1, "length " << len);
    // (The keys Shift + F1..F5 are the pads' mode keys on Pattern since OS-PADS-A: they do not jump, pad_modes_test.cpp.)
    (void)rig.invoke("cursor.jump_fraction", 2);
    CHECK_MESSAGE(rig.view().row() == static_cast<int>(2u * len / 4u), "length " << len);
    (void)rig.invoke("cursor.jump_end");
    CHECK(rig.view().row() == static_cast<int>(len) - 1);
    // The window follows: the cursor row is always inside it.
    CHECK((rig.view().row() >= rig.view().top_row() && rig.view().row() < rig.view().top_row() + 16));
  }
}

// ---- modes and the field level -----------------------------------------------------------------------------------------------------------

TEST_CASE("pattern edit: Browse is the default, Rec toggles Browse and Edit, and the edits are enabled in Edit only") {
  Rig rig;
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Browse));
  const char* const edits[] = {"step.note", "step.note_delta", "step.instrument", "step.vol", "step.fx_value", "step.clear", "step.insert", "step.pull"};
  for (const char* name : edits) {
    CHECK_MESSAGE(rig.invoke(name, 1) == DispatchResult::Disabled, name);
  }
  CHECK(rig.invoke("select.extend", 1) != DispatchResult::Disabled);  // selecting works in every mode (but Browse while it plays: play_position_test)
  (void)rig.invoke("select.clear");
  CHECK(rig.depth() == 0u);
  rig.tap(Button::Rec);
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Edit));
  CHECK(rig.invoke("step.note", 61) == DispatchResult::Changed);
  rig.tap(Button::Rec);
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Browse));
  CHECK(rig.invoke("step.note", 62) == DispatchResult::Disabled);
  // Select is retired (OS-LIVEREC, as Follow below): view.mode refuses it, so no view is ever in it.
  rig.mode(PatternMode::Select);
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Browse));
  CHECK(rig.invoke("step.note", 62) == DispatchResult::Disabled);
  // Follow is retired (OS-004d): view.mode refuses it, so no view is ever in it (the step edits and the selection keys of Browse are what they were).
  rig.mode(PatternMode::Follow);
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Browse));
  CHECK(rig.invoke("step.note", 62) == DispatchResult::Disabled);
  rig.tap(Button::Rec);
  CHECK(rig.state().mode[kPattern] == static_cast<unsigned>(PatternMode::Edit));
}

TEST_CASE("pattern edit: the encoder scrolls in Browse and edits the field in Edit") {
  Rig rig;
  // Browse: the encoder is the cursor; under Shift a page of rows a detent (OS-SHIFTENC; it was one row, as the plain turn).
  rig.turn(3);
  CHECK(rig.view().row() == 3);
  rig.shift_turn(1);
  CHECK(rig.view().row() == 3 + app::PatternView::kVisibleRows);
  rig.turn(-14);
  CHECK(rig.view().row() == 5);
  CHECK(rig.depth() == 0u);
  // Edit, Note column: the encoder changes the note under the cursor (an empty step takes note 60 and the last used instrument).
  rig.edit_mode();
  (void)rig.invoke("instrument.select", 4);
  rig.turn(1);
  CHECK((rig.step(0, 5).note == 61 && rig.step(0, 5).instr == 4));
  CHECK(rig.view().row() == 5);  // the encoder does not move the cursor
  rig.turn(2);
  CHECK(rig.step(0, 5).note == 63);
  // Shift + encoder is an octave (PAR-052).
  rig.press(Button::Shift);
  rig.pass(1);
  rig.turn(1);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.step(0, 5).note == 75);
  // Instr column: the encoder walks the instrument; Vol column: the volume; FX column: the value.
  rig.tap(Button::Instr);
  rig.turn(1);
  CHECK(rig.step(0, 5).instr == 5);
  rig.tap(Button::Vol);
  rig.turn(1);
  CHECK(rig.step(0, 5).vol == model::edit::kFirstVolume);
  rig.turn(-7);
  CHECK(rig.step(0, 5).vol == 120);
  // An FX needs a type first: the value of an empty FX slot with no last used FX does nothing (our rule: the last used FX starts at None).
  rig.tap(Button::Fx);
  CHECK(rig.view().column() == Column::Fx1);
  rig.turn(3);
  CHECK(rig.step(0, 5).fx[0].type == 0);
  (void)rig.invoke("step.fx_type", 27);  // Low-pass: set by an action (the key combination for it is P1a.2)
  CHECK(rig.step(0, 5).fx[0].type == 27);
  rig.turn(5);
  CHECK(rig.step(0, 5).fx[0].value == 5);
  // The FX type: Fx held and the encoder (a hold-encoder binding of the field).
  const unsigned before = rig.depth();
  rig.press(Button::Fx);
  rig.pass(100);
  rig.turn(1);
  rig.release(Button::Fx);
  rig.pass(1);
  CHECK(rig.depth() > before);
  CHECK(rig.step(0, 5).fx[0].type != 27);  // one picker place up from Low-pass: the next type
}

namespace {
struct PreviewLog final : app::PreviewSink {
  unsigned instrument = 0, note = 0, count = 0;
  void preview(unsigned i, unsigned n) noexcept override {
    instrument = i;
    note = n;
    ++count;
  }
};
}  // namespace

TEST_CASE("pattern edit: the pads in each field, and in Browse they preview") {
  Rig rig;
  PreviewLog sink;
  rig.app->attach_preview(&sink);
  // Browse: a pad previews (the engine hook), nothing is edited.
  rig.pad(kPadC3);
  CHECK((sink.count == 1u && sink.note == 36u && sink.instrument == 1u));
  CHECK(rig.depth() == 0u);
  CHECK(rig.step(0, 0).note == 0);
  // OS-PADHOLD: the Instr column in Browse previews like any other: the pad's note on the last used instrument; it never chooses the instrument.
  rig.tap(Button::Instr);
  rig.pad(3);
  CHECK(rig.app->instrument() == 1);
  CHECK((sink.instrument == 1u && sink.count == 2u));

  rig.edit_mode();
  rig.tap(Button::Note);
  rig.pad(kPadC3);  // note entry: the pad board's note, the last used instrument, and the cursor moves down by the edit step
  CHECK((rig.step(0, 0).note == 37 && rig.step(0, 0).instr == 1));
  CHECK(rig.view().row() == 1);
  rig.pad(36 + 4);  // E-3
  CHECK(rig.step(0, 1).note == 41);
  CHECK(rig.view().row() == 2);
  // Instr column (OS-AUDIT E-PAT-12): a pad picks instrument pad + 1 and writes no note of its own; the cursor moves on.
  rig.go(0);
  rig.tap(Button::Instr);
  rig.pad(kPadC3 + 7);  // pad 43: instrument 44
  CHECK((rig.step(0, 0).note == 37 && rig.step(0, 0).instr == 44 && rig.app->instrument() == 44));
  CHECK(rig.view().row() == 1);
  // Vol column: the pad maps 0..47 over 0..127; the entry makes a volume on an empty step.
  rig.tap(Button::Vol);
  rig.pad(47);
  CHECK(rig.step(0, 1).vol == 127);
  rig.go(2);
  rig.tap(Button::Vol);
  rig.pad(0);
  CHECK(rig.step(0, 2).vol == 0);
  // FX column: the pad is a value over the type's range; an empty slot with no last used FX stays empty.
  rig.go(3);
  rig.tap(Button::Fx);
  rig.pad(10);
  CHECK(rig.step(0, 3).fx[0].type == 0);
  rig.go(3);
  rig.column(Column::Fx1);
  (void)rig.invoke("step.fx_type", 4);  // Chance (the row reaches 174, the pads stay in 0..100, OS-CECHANCE C2)
  rig.go(3);
  rig.tap(Button::Fx);
  rig.pad(47);
  CHECK(rig.step(0, 3).fx[0].value == 100);
  CHECK(rig.step(0, 3).fx[0].type == 4);
  // A pad id above 47 and a non-pad do nothing.
  const unsigned depth = rig.depth();
  CHECK(rig.invoke("step.note_pad", 48) != DispatchResult::Changed);
  CHECK(rig.invoke("step.note_pad", -1) != DispatchResult::Changed);
  CHECK(rig.invoke("step.instrument", 65) != DispatchResult::Changed);
  CHECK(rig.invoke("step.vol_pad", 99) != DispatchResult::Changed);
  CHECK(rig.invoke("step.fx_pad", 48) != DispatchResult::Changed);
  CHECK(rig.invoke("pad.preview", 48) != DispatchResult::Changed);
  CHECK(rig.depth() == depth);
}

namespace {
namespace midi = o3os::midi;
// What the live notes (OS-005g) ask of the voices, in order.
struct LiveLog final : app::PreviewSink {
  struct Entry {
    char what;  // 'D' pad down, 'U' pad up, 'N' note on, 'F' note off, 'A' notes off (all), 'M' notes off (midi only), 'P' preview
    unsigned a, b, c, d;
  };
  std::vector<Entry> log;
  void preview(unsigned i, unsigned n) noexcept override { log.push_back({'P', i, n, 0, 0}); }
  void pad_down(unsigned pad, unsigned track, unsigned i, unsigned n) noexcept override { log.push_back({'D', pad, track, i, n}); }
  void pad_up(unsigned pad) noexcept override { log.push_back({'U', pad, 0, 0, 0}); }
  void midi_note(bool on, unsigned track, unsigned i, unsigned n, unsigned v, unsigned channel) noexcept override {
    (void)channel;
    log.push_back({on ? 'N' : 'F', track, i, n, v});
  }
  void notes_off(bool midi_only) noexcept override { log.push_back({midi_only ? 'M' : 'A', 0, 0, 0, 0}); }
};

void send_midi(midi::CcInput& in, unsigned port, std::initializer_list<std::uint8_t> bytes) {
  for (const std::uint8_t b : bytes) {
    in.feed(midi::RtItem{0, b, static_cast<std::uint8_t>(port), 0});
  }
}
}  // namespace

TEST_CASE("live notes: a pad goes down with its track and instrument and its release comes through, the keyboard follows MIDI IN and IN CHANNEL") {
  namespace ds = model::device_setting;
  Rig rig;
  LiveLog sink;
  rig.app->attach_preview(&sink);
  rig.pad(kPadC3);  // press and release
  NEED(sink.log.size() == 2u);
  CHECK((sink.log[0].what == 'D' && sink.log[0].a == kPadC3 && sink.log[0].b == 0u && sink.log[0].c == 1u && sink.log[0].d == 36u));  // pad, track 0, instrument 1, note
  CHECK((sink.log[1].what == 'U' && sink.log[1].a == kPadC3));
  // A pad of another track's view starts the search from that track.
  sink.log.clear();
  rig.go(0, 3);
  CHECK(rig.view().track() == 3);
  rig.pad(5);
  NEED(sink.log.size() == 2u);
  CHECK((sink.log[0].what == 'D' && sink.log[0].b == 3u));

  // Lost input silences every note.
  sink.log.clear();
  rig.app->reset_input();
  NEED(sink.log.size() == 1u);
  CHECK(sink.log[0].what == 'A');

  // The keyboard: notes on the selected track and the last used instrument, filtered by the settings.
  sink.log.clear();
  midi::CcInput in;
  send_midi(in, 0, {0x90, 60, 100, 0x80, 60, 0, 0x90, 62, 0});  // note on, note off, a note on with velocity 0 is a note off
  CHECK(rig.app->poll_midi_notes(in) == 1u);
  NEED(sink.log.size() == 3u);
  CHECK((sink.log[0].what == 'N' && sink.log[0].c == 60u && sink.log[0].d == 100u && sink.log[0].b == static_cast<unsigned>(rig.app->instrument())));
  CHECK((sink.log[1].what == 'F' && sink.log[1].c == 60u));
  CHECK((sink.log[2].what == 'F' && sink.log[2].c == 62u));
  sink.log.clear();
  CHECK(rig.app->device_settings().set(ds::MidiInChannel, 3.0f) == model::Status::Ok);
  send_midi(in, 0, {0x90, 60, 100});  // channel 1: not this device's
  send_midi(in, 1, {0x92, 61, 90});   // channel 3 (USB: MIDI IN is DIN + USB by default)
  CHECK(rig.app->poll_midi_notes(in) == 1u);
  NEED(sink.log.size() == 1u);
  CHECK((sink.log[0].what == 'N' && sink.log[0].c == 61u));
  // An all-notes-off of the keyboard, and a lost message that may have been an off.
  sink.log.clear();
  send_midi(in, 1, {0xB2, 123, 0});
  CHECK(rig.app->poll_midi_notes(in) == 0u);
  NEED(sink.log.size() == 1u);
  CHECK(sink.log[0].what == 'M');
  sink.log.clear();
  for (unsigned i = 0; i < midi::CcInput::kNoteRingSize + 2; ++i) {
    send_midi(in, 1, {0x92, 60, 90});
  }
  CHECK(in.note_dropped() == 2u);
  (void)rig.app->poll_midi_notes(in);
  NEED(!sink.log.empty());
  CHECK(sink.log[0].what == 'M');  // before the notes it did get
  // Without a sink the messages are taken and dropped.
  rig.app->attach_preview(nullptr);
  CHECK(rig.app->poll_midi_notes(in) == 0u);
}

// OS-BUG-STEPSND: in STEP (Edit) a pad writes the note AND sounds, on the selected track, stopped or playing; the release comes through as in Browse.
TEST_CASE("OS-BUG-STEPSND: a pad in STEP sounds as well as writes, on the cursor's track; its release ends it") {
  Rig rig;
  LiveLog sink;
  rig.app->attach_preview(&sink);
  rig.edit_mode();
  rig.go(2, 3);  // row 2, track 3
  rig.pad(kPadC3);
  CHECK(rig.step(3, 2).note == 37);  // written
  NEED(sink.log.size() == 2u);
  CHECK((sink.log[0].what == 'D' && sink.log[0].a == kPadC3 && sink.log[0].b == 3u && sink.log[0].c == 1u && sink.log[0].d == 36u));  // sounds: pad, track 3, instrument 1, note
  CHECK((sink.log[1].what == 'U' && sink.log[1].a == kPadC3));
  // the Instr column (a pad picks the instrument and sounds the pad's note on it, OS-AUDIT E-PAT-12)
  sink.log.clear();
  rig.go(5, 3);
  rig.tap(Button::Instr);
  rig.pad(kPadC3 + 2);
  NEED(sink.log.size() == 2u);
  CHECK((sink.log[0].what == 'D' && sink.log[0].b == 3u && sink.log[0].c == 39u && sink.log[0].d == 38u));
  // with a block selected the edit leaves the strip's instrument alone: the pad still sounds the instrument it picked
  sink.log.clear();
  rig.go(8, 3);
  rig.tap(Button::Instr);
  (void)rig.invoke("select.extend", 1);
  rig.pad(kPadC3 + 4);
  NEED(!sink.log.empty());
  CHECK((sink.log[0].what == 'D' && sink.log[0].c == 41u && sink.log[0].d == 40u));
  // Vol and Fx pads are values, not notes: they stay silent
  sink.log.clear();
  rig.go(6, 3);
  rig.tap(Button::Vol);
  rig.pad(10);
  for (const auto& e : sink.log) {
    CHECK(e.what != 'D');  // (the release is always passed on: the sink knows which pads it holds)
  }
  // The write is refused (past the track's own end: PAST TRACK END), the pad sounds all the same
  (void)rig.project().set_track_length(1, 3, 4);
  rig.editor->history_reset();
  sink.log.clear();
  rig.tap(Button::Note);
  rig.go(10, 3);
  rig.pad(kPadC3 + 3);
  CHECK(rig.message() == "PAST TRACK END");
  CHECK(model::is_empty(rig.step(3, 10)));
  NEED(!sink.log.empty());
  CHECK((sink.log[0].what == 'D' && sink.log[0].b == 3u && sink.log[0].d == 39u));
  // A step that already has a note, with another instrument: the pad sounds the current instrument (as PIANO / ISO do), and the write gives the step that instrument too (OS-AUDIT E-PAT-14)
  model::Step held = model::kEmptyStep;
  held.note = 50;
  held.instr = 5;
  (void)rig.project().set_steps(1, 4, 20, &held, 1);
  rig.go(20, 4);
  sink.log.clear();
  rig.pad(kPadC3);
  NEED(!sink.log.empty());
  CHECK((sink.log[0].what == 'D' && sink.log[0].c == static_cast<unsigned>(rig.app->instrument())));
  CHECK((rig.step(4, 20).note == 37 && rig.step(4, 20).instr == rig.app->instrument()));  // the step takes the current instrument (OS-AUDIT E-PAT-14)
}

TEST_CASE("live notes: in Edit a MIDI note goes into the cursor step, no advance, one undo unit; Browse and a block do not write (OS-FU1)") {
  Rig rig;
  LiveLog sink;
  rig.app->attach_preview(&sink);
  midi::CcInput in;
  // Browse: the note sounds (the live note) and writes nothing.
  send_midi(in, 0, {0x90, 60, 100});
  CHECK(rig.app->poll_midi_notes(in) == 1u);
  CHECK(rig.step(0, 0).note == 0);
  CHECK(rig.depth() == 0u);
  // Edit: the note is written at the cursor, the empty step takes the last used instrument, the cursor stays.
  rig.edit_mode();
  rig.go(3, 0);
  const unsigned depth = rig.depth();
  send_midi(in, 0, {0x90, 60, 100});
  CHECK(rig.app->poll_midi_notes(in) == 1u);
  CHECK(rig.step(0, 3).note == 61);
  CHECK(rig.step(0, 3).instr == rig.app->instrument());
  CHECK(rig.view().row() == 3);
  CHECK(rig.depth() == depth + 1u);
  CHECK(sink.log.back().what == 'N');  // and it sounded as well
  // Another key on the same step replaces the note and keeps the step's instrument; a note off writes nothing.
  (void)rig.invoke("step.instrument", 5);  // (the entry moves the cursor on)
  rig.go(3, 0);
  const unsigned depth2 = rig.depth();
  send_midi(in, 0, {0x80, 60, 0, 0x90, 62, 90});
  (void)rig.app->poll_midi_notes(in);
  CHECK(rig.step(0, 3).note == 63);
  CHECK(rig.step(0, 3).instr == 5);
  CHECK(rig.depth() == depth2 + 1u);
  // The undo takes the last note back.
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 3).note == 61);
  // A block of more than one step: the note only sounds.
  rig.go(3, 0);
  (void)rig.invoke("select.extend", 1);
  const unsigned depth3 = rig.depth();
  send_midi(in, 0, {0x90, 70, 90});
  CHECK(rig.app->poll_midi_notes(in) == 1u);
  CHECK(rig.step(0, 3).note == 61);
  CHECK(rig.step(0, 4).note == 0);
  CHECK(rig.depth() == depth3);
  // A key past the keyboard's range and no sink: written all the same (the write does not need the live notes).
  (void)rig.invoke("select.clear");
  rig.go(3, 0);
  rig.app->attach_preview(nullptr);
  send_midi(in, 0, {0x90, 127, 90});
  CHECK(rig.app->poll_midi_notes(in) == 0u);
  CHECK(rig.step(0, 3).note == 128);
}

TEST_CASE("pattern edit: a pad in the Instr column picks instrument pad + 1 and writes no note of its own; undoable (OS-AUDIT E-PAT-12, reverses OS-PADHOLD for this column)") {
  Rig rig;
  rig.edit_mode();
  model::Step held = model::kEmptyStep;
  held.note = 50;
  held.instr = 2;
  held.vol = 80;
  (void)rig.project().set_steps(1, 0, 3, &held, 1);
  rig.go(3);
  rig.tap(Button::Instr);
  rig.pad(9);  // pad 9: instrument 10 on a step that has a note: the note, the volume stay
  CHECK((rig.step(0, 3).note == 50 && rig.step(0, 3).instr == 10 && rig.step(0, 3).vol == 80));
  CHECK(rig.app->instrument() == 10);
  CHECK(rig.view().row() == 4);  // the cursor moved on
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // one undo unit
  CHECK((rig.step(0, 3).note == 50 && rig.step(0, 3).instr == 2));
  rig.go(0);
  rig.tap(Button::Instr);
  rig.pad(0);  // pad 0: instrument 1
  CHECK(rig.step(0, 0).instr == 1);
  CHECK(rig.app->instrument() == 1);
  // the encoder still walks the instrument, and a pad in the Note column is still a note
  (void)rig.invoke("nav.up");
  rig.turn(1);
  CHECK(rig.step(0, 0).instr == 2);
  rig.go(5);
  rig.tap(Button::Note);
  rig.pad(kPadC3 + 2);
  CHECK(rig.step(0, 5).note == 39);
}

TEST_CASE("pattern edit: a note written by hand over a note replaces note and instrument and keeps volume and FX (OS-AUDIT E-PAT-14)") {
  Rig rig;
  rig.edit_mode();
  model::Step held = model::kEmptyStep;
  held.note = 50;
  held.instr = 5;
  held.vol = 99;
  held.fx[0] = {3, 7};
  held.fx[1] = {4, 9};
  (void)rig.project().set_steps(1, 0, 0, &held, 1);
  (void)rig.project().set_steps(1, 0, 1, &held, 1);
  (void)rig.project().set_steps(1, 0, 2, &held, 1);
  const int cur = rig.app->instrument();
  NEED(cur != 5);
  CHECK(rig.invoke("step.note", 61) == DispatchResult::Changed);  // STEP: the note of the cell
  CHECK((rig.step(0, 0).note == 61 && rig.step(0, 0).instr == cur && rig.step(0, 0).vol == 99));
  CHECK((rig.step(0, 0).fx[0].type == 3 && rig.step(0, 0).fx[0].value == 7 && rig.step(0, 0).fx[1].type == 4 && rig.step(0, 0).fx[1].value == 9));
  rig.go(1);
  rig.pad(kPadC3);  // a pad
  CHECK((rig.step(0, 1).note == 37 && rig.step(0, 1).instr == cur && rig.step(0, 1).vol == 99 && rig.step(0, 1).fx[1].value == 9));
  rig.go(3);
  (void)rig.project().set_steps(1, 0, 3, &held, 1);
  (void)rig.invoke("step.note", 254);  // a cut carries no instrument: the instr byte stays as it was
  CHECK((rig.step(0, 3).note == 254 && rig.step(0, 3).instr == 5));
  rig.go(2);
  CHECK(rig.invoke("step.midi_note", 60) == DispatchResult::Changed);  // the MIDI keyboard
  CHECK((rig.step(0, 2).note == 61 && rig.step(0, 2).instr == cur && rig.step(0, 2).vol == 99 && rig.step(0, 2).fx[0].value == 7));
}

TEST_CASE("pattern edit: a held Fx button survives a move from the Note field to the Instr field, and the encoder is still a HoldEncoder") {
  Rig rig;
  rig.edit_mode();
  (void)rig.invoke("step.note", 50);
  rig.go(0);
  rig.column(Column::Fx1);
  (void)rig.invoke("step.fx_type", 4);  // Chance
  rig.go(0);
  const std::uint8_t instr = rig.step(0, 0).instr;
  // The field level of the chain changes under the held button (Fx1 -> Instr): the gesture layer must not forget it.
  rig.tap(Button::Fx);
  rig.press(Button::Fx);
  rig.pass(100);
  rig.press(Button::Instr);  // (the cursor moves to the Instr field while Fx is held)
  rig.pass(1);
  rig.release(Button::Instr);
  rig.pass(1);
  CHECK(rig.view().column() == Column::Instr);
  rig.turn(1);  // Fx is still down: this is "Fx held + encoder", which the Instr field does not bind, so it is a plain turn there
  rig.release(Button::Fx);
  rig.pass(1);
  CHECK(rig.step(0, 0).instr != instr);  // the plain encoder of the Instr field: the instrument walked
  // Back on the FX field with Fx held through a Note -> Fx move: the turn is the type edit.
  rig.go(0);
  rig.column(Column::Note);
  rig.press(Button::Fx);
  rig.pass(100);
  const unsigned before = rig.depth();
  rig.press(Button::Vol);
  rig.pass(1);
  rig.release(Button::Vol);
  rig.pass(1);
  rig.column(Column::Fx1);
  rig.turn(1);
  rig.release(Button::Fx);
  rig.pass(1);
  CHECK(rig.depth() == before + 1u);
  CHECK(rig.step(0, 0).fx[0].type != 4);  // one picker place up from Chance
}

TEST_CASE("pattern edit: in Browse a held Fx and the encoder scroll the steps, nothing is edited and the release is not a tap") {
  for (const PatternMode m : {PatternMode::Browse}) {
    Rig rig;
    rig.edit_mode();
    (void)rig.invoke("step.note", 50);
    rig.go(0);
    rig.column(Column::Fx1);
    (void)rig.invoke("step.fx_type", 4);  // Chance: a step whose FX type the Fx field's hold-encoder row would change
    rig.go(0);
    rig.column(Column::Fx1);
    rig.mode(m);
    const model::Step kept = rig.step(0, 0);
    const unsigned before = rig.depth();
    CHECK(rig.view().row() == 0);
    rig.press(Button::Fx);
    rig.pass(100);
    rig.turn(3);
    rig.release(Button::Fx);
    rig.pass(1);
    CHECK(rig.view().row() == 3);                 // the detents scrolled
    CHECK(rig.view().column() == Column::Fx1);    // and the release did not also tap Fx (that would have gone on to FX2)
    CHECK(rig.depth() == before);                 // no edit, no undo unit
    CHECK(rig.step(0, 0).fx[0].type == kept.fx[0].type);
    CHECK(rig.step(0, 0).note == kept.note);
    // Without the encoder, Fx is still the column button, at its release.
    rig.tap(Button::Fx);
    CHECK(rig.view().column() == Column::Fx2);
  }
}

TEST_CASE("pattern edit: clear by field, and a whole track of 128 steps empties without changing a length") {
  Rig rig(true, 128);
  rig.edit_mode();
  // Fill step 0 of track 1 completely, then clear it element by element.
  (void)rig.invoke("step.note", 50);
  rig.go(0);
  rig.column(Column::Vol);
  (void)rig.invoke("step.vol", 90);
  rig.column(Column::Fx1);
  (void)rig.invoke("step.fx_type", 4);
  rig.column(Column::Fx2);
  (void)rig.invoke("step.fx_type", 27);
  CHECK(rig.invoke("step.clear") == DispatchResult::Changed);  // the FX column clears its FX
  CHECK((rig.step(0, 0).fx[1].type == 0 && rig.step(0, 0).fx[0].type == 4 && rig.step(0, 0).note == 50));
  CHECK(rig.view().row() == 1);  // and the cursor moves on by the edit step
  rig.go(0);
  rig.column(Column::Vol);
  CHECK(rig.invoke("step.clear") == DispatchResult::Changed);
  CHECK((rig.step(0, 0).vol == model::kVolumeUnset && rig.step(0, 0).note == 50));
  rig.go(0);
  rig.column(Column::Note);
  CHECK(rig.invoke("step.clear") == DispatchResult::Changed);  // Note (and Instr): the whole step
  CHECK(model::is_empty(rig.step(0, 0)));
  // Fill a whole column, select it (Shift + Up at step 0) and Delete: nothing is left and the length is the same (PAR-073, PAR-093).
  rig.go(0);
  for (int row = 0; row < 128; ++row) {
    rig.column(Column::Instr);
    (void)rig.invoke("step.instrument", 49);  // a MIDI instrument leaves an instrument behind if a clear misses it
    rig.go(row);
    rig.column(Column::Vol);
    (void)rig.invoke("step.vol", 77);
    rig.go(row + 1 < 128 ? row + 1 : row);
  }
  CHECK(rig.step(0, 127).instr == 49);
  rig.go(0);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Up);  // at step 0: the column
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.edit().selection.active);  // (releasing Shift ends the extension and the column step, not the block)
  CHECK_FALSE(rig.edit().selection.extending);
  const app::Selection::Block block = rig.edit().selection.block(rig.view().row(), rig.view().track());
  CHECK((block.row0 == 0 && block.row1 == 127 && block.track0 == 0 && block.track1 == 0));
  rig.tap(Button::Delete);
  for (int row = 0; row < 128; ++row) {
    CHECK_MESSAGE(model::is_empty(rig.step(0, row)), "row " << row);
  }
  CHECK(rig.project().pattern(1)->track_length[0] == 128);
  CHECK(rig.project().pattern_length(1) == 128);
  CHECK_FALSE(rig.edit().selection.active);  // Delete ends the selection
}

// The digest of the canonical form of all 16 x 128 steps of the pattern the rig edits (support/step_vectors.h).
static std::uint64_t pattern_digest(Rig& rig, bool extra, bool flag) {
  test::vec::Digest d;
  for (int t = 0; t < 16; ++t) {
    for (int s = 0; s < 128; ++s) {
      d.add(test::vec::out_of(rig.step(t, s)));
    }
  }
  if (extra) {
    d.add_u32(flag ? 1u : 0u);
  }
  return d.value();
}

TEST_CASE("pattern edit: insert against the reference vectors, with a block over several tracks; the pull (OS-DELPULL) replaces the old backspace and is checked by its own tests") {
  // Expected values: output of this project's own reference model (maintainer, 2026-10-18), kept as test vectors (data/pattern_edit_vectors.h): per
  // episode, the digest of the pattern after the operation and whether it changed anything.
  test::Rng rng(0x1A5E2Eu);
  for (unsigned episode = 0; episode < 60; ++episode) {
    const unsigned len = 1 + rng.below(128);
    Rig rig(true, len);
    rig.edit_mode();
    std::vector<model::Step> buf(128);
    for (int t = 0; t < 16; ++t) {
      for (int s = 0; s < 128; ++s) {
        model::Step m = model::kEmptyStep;
        if (rng.below(3) == 0) {
          m.note = static_cast<std::uint8_t>(1 + rng.below(120));
          m.instr = static_cast<std::uint8_t>(1 + rng.below(64));
        }
        if (rng.below(5) == 0) {
          m.fx[0] = {static_cast<std::uint8_t>(1 + rng.below(40)), rng.byte()};
        }
        buf[static_cast<std::size_t>(s)] = m;
      }
      (void)rig.project().set_steps(1, static_cast<std::uint32_t>(t), 0, buf.data(), 128);
    }
    rig.editor->history_reset();  // the project was filled behind the editor's back
    // A block of one step over `t0..t1` from `row`.
    const int row = static_cast<int>(rng.below(len));
    const int t0 = static_cast<int>(rng.below(12));
    const int t1 = t0 + static_cast<int>(rng.below(static_cast<unsigned>(12 - t0)));
    rig.go(row, t0);
    rig.extend_tracks(t1 - t0);
    CHECK(rig.view().track() == t1);
    const bool insert = rng.below(2) == 0;
    const std::uint64_t pre = pattern_digest(rig, false, false);
    const unsigned before = rig.depth();
    (void)rig.invoke(insert ? "step.insert" : "step.pull");
    const bool differs = pattern_digest(rig, false, false) != pre;
    if (insert) {  // (the vectors of the episodes that backspaced follow the reference model's backspace, which Shift + Delete no longer does: OS-DELPULL)
      CHECK_MESSAGE(pattern_digest(rig, true, differs) == vectors::kInsertBackspaceDigest[episode], "insert episode " << episode);
    }
    // One undo unit if anything changed, none if not; and it comes back.
    CHECK(rig.depth() == before + (differs ? 1u : 0u));
    if (differs) {
      CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
      CHECK(pattern_digest(rig, false, false) == pre);
    }
  }
}

// ---- the selection ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("pattern edit: Shift and the arrows make a block from where Shift was first pressed, a plain move ends it") {
  Rig rig;
  rig.go(4, 1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap_tracks(1);  // a track to the right (Shift + Right goes a column at a time in the first track, then by tracks: OS-AUDIT-56)
  rig.release(Button::Shift);
  rig.pass(1);
  const app::Selection& sel = rig.edit().selection;
  CHECK(sel.active);
  CHECK_FALSE(sel.extending);  // releasing Shift ends the extension, not the block
  CHECK((sel.start_row == 4 && sel.start_track == 1 && sel.end_row == 6 && sel.end_track == 2));
  CHECK((rig.view().row() == 6 && rig.view().track() == 2));
  const app::Selection::Block b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.row0 == 4 && b.row1 == 6 && b.track0 == 1 && b.track1 == 2));
  // A second Shift + arrow starts a new block at the cursor (our rule: the release resets the selection state).
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Up);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK((sel.start_row == 6 && sel.start_track == 2 && sel.end_row == 5 && sel.end_track == 2));
  // A plain move collapses it, whatever moves the cursor: an arrow, the encoder in Browse, a jump.
  rig.tap(Button::Down);
  CHECK_FALSE(rig.edit().selection.active);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.edit().selection.active);
  rig.turn(1);
  CHECK_FALSE(rig.edit().selection.active);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  rig.pass(1);
  (void)rig.invoke("cursor.jump_fraction", 1);
  CHECK_FALSE(rig.edit().selection.active);
}

TEST_CASE("pattern edit: Shift + Up at step 0 selects the track's steps, again every track; HOME collapses and goes to step 0") {
  Rig rig(true, 32);
  rig.go(0, 2);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Up);
  CHECK(rig.edit().selection.column);
  app::Selection::Block b = rig.edit().selection.block(rig.view().row(), rig.view().track());
  CHECK((b.row0 == 0 && b.row1 == 31 && b.track0 == 2 && b.track1 == 2));
  rig.tap(Button::Up);  // again: every track
  b = rig.edit().selection.block(rig.view().row(), rig.view().track());
  CHECK((b.row0 == 0 && b.row1 == 31 && b.track0 == 0 && b.track1 == 11));
  rig.tap(Button::Right);  // with the column selection on, a track to the right grows it from its first track
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.edit().selection.active);
  // HOME (Shift + Insert) collapses and jumps to step 0.
  rig.go(9, 1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.edit().selection.active);
  rig.shift_tap(Button::Insert);
  CHECK_FALSE(rig.edit().selection.active);
  CHECK(rig.view().row() == 0);
  CHECK(rig.view().track() == 1);
  // The block is Shift + the arrows, from the cursor, in every mode (the block mark whose Select mode extended it went with OS-LIVEREC): it stays when Shift is let go.
  rig.go(2, 0);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap_tracks(1);
  rig.release(Button::Shift);
  rig.pass(1);
  const app::Selection::Block m = rig.edit().selection.block(rig.view().row(), rig.view().track());
  CHECK((m.row0 == 2 && m.row1 == 4 && m.track0 == 0 && m.track1 == 1));
  CHECK(rig.edit().selection.active);
  CHECK_FALSE(rig.edit().selection.extending);
  CHECK(rig.invoke("select.clear") == DispatchResult::Changed);
  CHECK_FALSE(rig.edit().selection.active);
}

// OS-SELCOL: Shift + Up at step 0 steps through the cursor's column, the whole track, every track. A note, a volume and both FX on every row of tracks 0 and 1.
static void fill_columns(Rig& rig, int rows) {
  const model::Step s = {50, 3, 40, {{4, 10}, {27, 20}}};
  for (std::uint32_t t = 0; t < 2; ++t) {
    for (int row = 0; row < rows; ++row) {
      (void)rig.project().set_steps(1, t, static_cast<std::uint32_t>(row), &s, 1);
    }
  }
  rig.go(0);
}

TEST_CASE("OS-SELCOL: Shift + Up at step 0 selects the cursor's column, then the track, then every track; on Note the first press is the track") {
  const Column cols[] = {Column::Instr, Column::Vol, Column::Fx1, Column::Fx2, Column::Note};
  for (const Column col : cols) {
    CAPTURE(static_cast<int>(col));
    Rig rig(true, 32);
    rig.edit_mode();
    rig.go(0, 2);
    rig.column(col);
    rig.press(Button::Shift);
    rig.pass(1);
    rig.tap(Button::Up);
    app::Selection::Block b = rig.edit().selection.block(rig.view().row(), rig.view().track());
    CHECK((b.row0 == 0 && b.row1 == 31 && b.track0 == 2 && b.track1 == 2));
    if (col != Column::Note) {
      CHECK((b.col0 == static_cast<int>(col) && b.col1 == static_cast<int>(col)));
      rig.tap(Button::Up);  // the whole track
      b = rig.edit().selection.block(rig.view().row(), rig.view().track());
      CHECK((b.row0 == 0 && b.row1 == 31 && b.track0 == 2 && b.track1 == 2 && b.whole_steps()));
    } else {
      CHECK(b.whole_steps());
    }
    rig.tap(Button::Up);  // every track
    b = rig.edit().selection.block(rig.view().row(), rig.view().track());
    CHECK((b.row0 == 0 && b.row1 == 31 && b.track0 == 0 && b.track1 > 2 && b.whole_steps()));
    rig.release(Button::Shift);
    rig.pass(1);
    CHECK(rig.edit().selection.active);
  }
}

TEST_CASE("OS-SELCOL: the one-column block outlives Shift; Delete clears only that column; a new press starts at the column again") {
  Rig rig(true, 32);
  rig.edit_mode();
  fill_columns(rig, 32);
  rig.go(0);
  rig.column(Column::Vol);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Up);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.edit().selection.active);
  CHECK_FALSE(rig.edit().selection.column);
  const app::Selection::Block b = rig.edit().selection.block(rig.view().row(), rig.view().track());
  CHECK((b.col0 == static_cast<int>(Column::Vol) && b.col1 == static_cast<int>(Column::Vol) && b.row0 == 0 && b.row1 == 31));
  rig.tap(Button::Delete);
  for (int row = 0; row < 32; ++row) {
    CHECK_MESSAGE(rig.step(0, row).vol == model::kVolumeUnset, "row " << row);
    CHECK_MESSAGE((rig.step(0, row).note == 50 && rig.step(0, row).fx[0].type == 4 && rig.step(0, row).fx[1].type == 27), "row " << row);
    CHECK_MESSAGE(rig.step(1, row).vol == 40, "track 1, row " << row);
  }
  CHECK_FALSE(rig.edit().selection.active);
  // After Shift is let go, a fresh press at step 0 is the column again (not the track).
  rig.go(0);
  rig.column(Column::Fx1);
  rig.shift_tap(Button::Up);
  const app::Selection::Block c = rig.edit().selection.block(rig.view().row(), rig.view().track());
  CHECK((c.col0 == static_cast<int>(Column::Fx1) && c.col1 == static_cast<int>(Column::Fx1)));
  // Shift + Down from the one-column block makes a plain block of whole steps.
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Up);
  rig.tap(Button::Down);
  rig.release(Button::Shift);
  rig.pass(1);
  CHECK(rig.edit().selection.block(rig.view().row(), rig.view().track()).whole_steps());
}

TEST_CASE("OS-SELCOL: with a one-column block the encoder changes that column inside the block only (Vol, Fx1, Fx2, Instr)") {
  Rig rig(true, 32);
  rig.edit_mode();
  fill_columns(rig, 32);
  const model::Step before = rig.step(0, 5);
  // Vol: the real encoder (and Shift + encoder) on the block of track 0's volumes.
  rig.go(0);
  rig.column(Column::Vol);
  rig.shift_tap(Button::Up);
  rig.turn(3);
  for (int row = 0; row < 32; ++row) {
    CHECK_MESSAGE(rig.step(0, row).vol != 40, "row " << row);
    CHECK_MESSAGE((rig.step(0, row).note == 50 && rig.step(0, row).fx[0].value == before.fx[0].value && rig.step(0, row).fx[1].value == before.fx[1].value), "row " << row);
    CHECK_MESSAGE(rig.step(1, row).vol == 40, "track 1, row " << row);
  }
  const unsigned vol = rig.step(0, 7).vol;
  rig.shift_turn(1);  // (Shift + encoder while the block stays)
  CHECK(rig.step(0, 7).vol != vol);
  CHECK(rig.step(1, 7).vol == 40);
  // Fx1 and Fx2: the value of that FX only.
  const Column fxs[] = {Column::Fx1, Column::Fx2};
  for (int f = 0; f < 2; ++f) {
    rig.go(0);
    rig.column(fxs[f]);
    rig.shift_tap(Button::Up);
    const unsigned old_value = rig.step(0, 9).fx[f].value;
    const unsigned other = rig.step(0, 9).fx[1 - f].value;
    CHECK(rig.invoke("step.fx_value_delta", 1) == DispatchResult::Changed);
    for (int row = 0; row < 32; ++row) {
      CHECK_MESSAGE(rig.step(0, row).fx[f].value != old_value, "row " << row);
      CHECK_MESSAGE(rig.step(0, row).fx[1 - f].value == other, "row " << row);
      CHECK_MESSAGE(rig.step(1, row).fx[f].value == old_value, "track 1, row " << row);
    }
    CHECK(rig.step(0, 9).fx[f].value != old_value);
  }
  // Instr: the instrument of the block's steps only.
  rig.go(0, 0);
  rig.column(Column::Instr);
  rig.shift_tap(Button::Up);
  const unsigned instr1 = rig.step(1, 3).instr;
  (void)rig.invoke("step.instrument_delta", 1);
  CHECK(rig.step(0, 3).instr != instr1);
  CHECK(rig.step(1, 3).instr == instr1);
  CHECK((rig.step(0, 3).note == 50 && rig.step(0, 3).fx[0].type == 4));
  // The block's columns, not the cursor's, decide: a block on Fx2 with the cursor moved to Fx1 leaves FX1 alone.
  rig.go(0, 1);
  rig.column(Column::Fx2);
  rig.shift_tap(Button::Up);
  rig.column(Column::Fx1);
  const unsigned fx1 = rig.step(1, 4).fx[0].value;
  (void)rig.invoke("step.fx_value_delta", 1);
  CHECK(rig.step(1, 4).fx[0].value == fx1);
}

TEST_CASE("pattern edit: edits act on the block: a range changes only what it has, a single step takes the single branch") {
  Rig rig;
  rig.edit_mode();
  // Notes on track 1, steps 0..3 and an empty step 4, then a block over 0..4.
  for (int row = 0; row < 4; ++row) {
    (void)rig.invoke("step.note", 50 + row);
  }
  CHECK(rig.view().row() == 4);
  rig.go(0);
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);
  CHECK(rig.edit().selection.active);
  // The encoder moves every note of the block by two; the empty step stays empty (a range does not make notes).
  CHECK(rig.invoke("step.note_delta", 2) == DispatchResult::Changed);
  CHECK((rig.step(0, 0).note == 52 && rig.step(0, 3).note == 55));
  CHECK(rig.step(0, 4).note == 0);
  // A pad sets the note of every step that has one (and leaves the empty step), and the cursor does not move.
  const int row = rig.view().row();
  rig.pad(kPadC3);
  CHECK((rig.step(0, 0).note == 37 && rig.step(0, 2).note == 37 && rig.step(0, 4).note == 0));
  CHECK(rig.view().row() == row);
  CHECK(rig.edit().selection.active);
  // One undo unit for the whole block.
  const unsigned depth = rig.depth();
  CHECK(rig.invoke("step.note_delta", 1) == DispatchResult::Changed);
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 37);
  // With the range gone it is the single step again: a volume entered on a step without one.
  (void)rig.invoke("select.clear");
  rig.column(Column::Vol);
  CHECK(rig.invoke("step.vol_delta", 1) == DispatchResult::Changed);
  CHECK(rig.step(0, rig.view().row()).vol == model::edit::kFirstVolume);
}

TEST_CASE("pattern edit: steps past a track's own length are refused, said once, and drawn as empty") {
  Rig rig(true, 32);
  (void)rig.project().set_track_length(1, 1, 8);
  rig.editor->history_reset();
  rig.edit_mode();
  rig.go(20, 1);
  const unsigned depth = rig.depth();
  CHECK(rig.invoke("step.note", 50) == DispatchResult::Changed);  // the message is what changed
  CHECK(rig.message() == "PAST TRACK END");
  CHECK(rig.step(1, 20).note == 0);
  CHECK(rig.depth() == depth);
  CHECK(rig.view().row() == 20);  // and the cursor did not move on
  CHECK(rig.invoke("step.clear") == DispatchResult::Changed);
  CHECK(rig.invoke("step.insert") == DispatchResult::Changed);
  CHECK(rig.invoke("step.pull") == DispatchResult::Changed);
  CHECK(rig.depth() == depth);
  // A block over the short track and a normal one edits the normal one only (a range changes the steps that have a note).
  rig.go(20, 0);
  (void)rig.invoke("step.note", 50);
  (void)rig.project().set_steps(1, 1, 20, &rig.step(0, 20), 1);  // the short track holds a kept step past its end, too
  rig.editor->history_reset();
  rig.go(20, 0);
  rig.extend_tracks(1);
  CHECK(rig.invoke("step.note", 60) == DispatchResult::Changed);
  CHECK(rig.step(0, 20).note == 60);
  CHECK(rig.step(1, 20).note == 50);  // untouched: it lies past the end of track 2
  // Inside the track it works, and the steps past the end that the model keeps are still readable by the screen.
  rig.go(3, 1);
  CHECK(rig.invoke("step.note", 70) == DispatchResult::Changed);
  CHECK(rig.step(1, 3).note == 70);
}

// OS-QAFIX-17/18 (maintainer, 2026-10-22): Insert and Pull act from the block's first row, not from the cursor at its far end; a track whose length does not reach
// that row is left alone (a track that ends above the block is not shifted), a track that ends inside the block is shifted from the first row.
TEST_CASE("pattern edit: insert and pull over a block act from the block's first row; a track that ends above it does not move") {
  for (const bool insert : {true, false}) {
    Rig rig(true, 32);
    (void)rig.project().set_track_length(1, 1, 8);  // track 2: steps 0-7
    for (int r = 0; r < 20; ++r) {
      model::Step s = model::kEmptyStep;
      s.note = static_cast<std::uint8_t>(30 + r);
      (void)rig.project().set_step(1, 0, static_cast<unsigned>(r), s);
      (void)rig.project().set_step(1, 1, static_cast<unsigned>(r), s);
    }
    rig.editor->history_reset();
    rig.edit_mode();
    // Block rows 10-12 on both tracks: the cursor is at row 12, the block starts at row 10, past the end of track 2.
    rig.go(10, 0);
    (void)rig.invoke("select.extend", 1);
    (void)rig.invoke("select.extend", 1);
    rig.extend_tracks(1);
    CHECK(rig.view().row() == 12);
    CHECK(rig.invoke(insert ? "step.insert" : "step.pull") == DispatchResult::Changed);
    for (int r = 0; r < 20; ++r) {  // track 2 is as it was, kept steps past the end included
      CHECK(rig.step(1, r).note == 30 + r);
    }
    for (int r = 0; r < 10; ++r) {  // track 1: rows before the block's first row are as they were (not from the cursor's row 12)
      CHECK(rig.step(0, r).note == 30 + r);
    }
    CHECK(rig.step(0, 10).note == (insert ? 0 : 31 + 10));
    CHECK(rig.step(0, 11).note == (insert ? 30 + 10 : 31 + 11));
    CHECK(rig.step(0, 12).note == (insert ? 30 + 11 : 31 + 12));
    // Block rows 6-9: it starts inside track 2 (length 8), so track 2 shifts from row 6 as well.
    (void)rig.invoke("select.clear");
    rig.go(6, 0);
    (void)rig.invoke("select.extend", 1);
    (void)rig.invoke("select.extend", 1);
    (void)rig.invoke("select.extend", 1);
    rig.extend_tracks(1);
    CHECK(rig.invoke(insert ? "step.insert" : "step.pull") == DispatchResult::Changed);
    CHECK(rig.step(1, 5).note == 35);
    CHECK(rig.step(1, 6).note == (insert ? 0 : 37));
    CHECK(rig.step(1, 7).note == (insert ? 36 : 38));
    CHECK(rig.step(1, 8).note == (insert ? 37 : 39));
  }
}

TEST_CASE("pattern edit: mute and solo are undoable flags of the track slot (their pads are the TRACKS layer's: pad_modes_test.cpp)") {
  Rig rig;
  const auto flags = [&](unsigned t) { return rig.project().track(t)->flags; };
  (void)rig.invoke("track.mute", 2);
  (void)rig.invoke("track.solo", 2);
  (void)rig.invoke("track.mute", 0);
  CHECK(flags(2) == (model::kTrackMute | model::kTrackSolo));
  CHECK(flags(0) == model::kTrackMute);
  CHECK(flags(1) == 0);
  CHECK(rig.depth() == 3u);
  // They work in Browse (the default mode) and in every mode, and are undone one at a time.
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the pad 0 mute
  CHECK(flags(0) == 0);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(flags(2) == model::kTrackMute);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(flags(2) == (model::kTrackMute | model::kTrackSolo));
  // A track that is not active cannot be muted, and a track index out of range does nothing.
  CHECK(rig.invoke("track.mute", 12) != DispatchResult::Changed);
  CHECK(rig.invoke("track.mute", 99) != DispatchResult::Changed);
  CHECK(rig.invoke("track.solo", -1) != DispatchResult::Changed);
  CHECK(flags(12) == 0);
  // Toggling twice is the same as not toggling (and two units).
  const unsigned depth = rig.depth();
  (void)rig.invoke("track.solo", 5);
  (void)rig.invoke("track.solo", 5);
  CHECK(flags(5) == 0);
  CHECK(rig.depth() == depth + 2u);
}

// ---- the choosers ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("pattern edit: F1, F2, F3: a tap of any toggles its chooser (OS-UXF-PAT item 6: F1 too), a hold opens any of them until the release, F8 closes") {
  Rig rig;
  using app::OverlayId;
  CHECK(rig.state().overlay == OverlayId::None);
  rig.tap(Button::F1);  // OS-UXF-PAT item 6: a tap of F1 opens the PATTERN chooser, as its hold does
  CHECK(rig.state().overlay == OverlayId::PatternChooser);
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.invoke("chooser.toggle", 0) == DispatchResult::Changed);
  CHECK(rig.state().overlay == OverlayId::PatternChooser);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);
  rig.tap(Button::F2);
  CHECK(rig.state().overlay == OverlayId::LengthChooser);
  rig.tap(Button::F3);  // another F-key: switched to it
  CHECK(rig.state().overlay == OverlayId::JumpChooser);
  rig.tap(Button::F1);  // another F-key: switched to it
  CHECK(rig.state().overlay == OverlayId::PatternChooser);
  rig.tap(Button::F3);
  CHECK(rig.state().overlay == OverlayId::JumpChooser);
  rig.tap(Button::F8);  // F8 is Back
  CHECK(rig.state().overlay == OverlayId::None);
  // A hold: open while down, closed by the release.
  rig.press(Button::F1);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK(rig.state().overlay == OverlayId::PatternChooser);
  CHECK(rig.edit().chooser_held);
  rig.release(Button::F1);
  rig.pass(1);
  CHECK(rig.state().overlay == OverlayId::None);
  // A chooser a tap opened is not closed by the release of that tap.
  rig.press(Button::F2);
  rig.pass(10);
  rig.release(Button::F2);
  rig.pass(1);
  CHECK(rig.state().overlay == OverlayId::LengthChooser);
  rig.tap(Button::F8);
  // Shift + F3 is the pads' mode key now (OS-PADS-A, OS-PADOCT), and a held Shift does not open a chooser.
  rig.go(0);
  rig.pad_mode(Button::F3);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.state().padmode.fixed() == app::PadMode::Piano);
  // Learn and the Navigator do not get a chooser over them.
  (void)rig.invoke("learn.toggle");
  CHECK(rig.invoke("chooser.toggle", 0) == DispatchResult::Unchanged);
  CHECK(rig.state().overlay == OverlayId::Learn);
  // The labels (PAR-092): F3 is JUMP, its chooser says STEP JUMP.
  CHECK(std::string(app::kPatternSoftKeys[2]) == "JUMP");
  CHECK(std::string(app::kPatternSoftKeys[0]) == "PATTERN");
  CHECK(std::string(app::kPatternSoftKeys[1]) == "LENGTH");
}

TEST_CASE("pattern edit: the pattern chooser (F1 held): 1..255, the strip follows at once, a pad picks, Left and Right go to the next pattern the project holds") {
  Rig rig;
  rig.hold_key(Button::F1);
  CHECK(rig.edit().chooser_held);
  rig.turn(1);
  CHECK(rig.edit().pattern == 2);
  CHECK(rig.app->view().row() == 0);
  const app::PatternSource src(rig.app->model(), rig.edit().pattern);
  CHECK(src.strip_pattern() == 2);  // PAR-158: the strip's PAT is the pattern in view, at once
  rig.tap(Button::Up);
  CHECK(rig.edit().pattern == 3);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.edit().pattern == 1);
  rig.tap(Button::Down);  // clamped at 1
  CHECK(rig.edit().pattern == 1);
  rig.turn(1000);
  CHECK(rig.edit().pattern == 255);
  rig.turn(-30000);
  CHECK(rig.edit().pattern == 1);
  rig.pad(9);  // pad 10: pattern 10
  CHECK(rig.edit().pattern == 10);
  rig.pad(47);
  CHECK(rig.edit().pattern == 48);
  rig.tap(Button::Right);  // no pattern past 48 in the project: nothing
  CHECK(rig.edit().pattern == 48);
  CHECK(rig.depth() == 0u);  // choosing a pattern is view state: no undo unit
  CHECK(rig.invoke("chooser.pad", 48) != DispatchResult::Changed);
  // The pattern that is not there shows empty; the cursor is kept inside its length; the first edit creates it.
  rig.release_key(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.edit_mode();
  CHECK(rig.invoke("step.note", 60) == DispatchResult::Changed);
  CHECK(rig.project().pattern(48) != nullptr);
  CHECK(rig.depth() == 1u);
}

TEST_CASE("pattern edit: the pattern chooser: Left and Right go to the previous and the next pattern the project holds, skipping the unused ids, and stop at the ends (OS-QAFIX-6)") {
  Rig rig;
  CHECK(rig.project().create_pattern(5, 16) == model::Status::Ok);
  CHECK(rig.project().create_pattern(9, 16) == model::Status::Ok);
  rig.hold_key(Button::F1);
  CHECK(rig.edit().pattern == 1);
  rig.tap(Button::Left);  // nothing before pattern 1: it says so
  CHECK(rig.edit().pattern == 1);
  CHECK(std::string(rig.state().notify.head().text.c_str()) == "NO PREVIOUS PATTERN");  // (queued: the chooser owns the screen)
  rig.tap(Button::Right);
  CHECK(rig.edit().pattern == 5);
  rig.tap(Button::Right);
  CHECK(rig.edit().pattern == 9);
  rig.tap(Button::Right);  // none after 9
  CHECK(rig.edit().pattern == 9);
  CHECK(rig.state().notify.size() == 2u);  // (the second message, "NO NEXT PATTERN", waits behind the first)
  rig.tap(Button::Left);
  CHECK(rig.edit().pattern == 5);
  rig.turn(1);  // an unused id by the encoder, then Left: the nearest used one below
  CHECK(rig.edit().pattern == 6);
  rig.tap(Button::Left);
  CHECK(rig.edit().pattern == 5);
  CHECK(rig.depth() == 0u);  // view state: no undo unit
  CHECK(rig.state().overlay == app::OverlayId::PatternChooser);
}

TEST_CASE("pattern edit: the length chooser sets every track, by 1, by the defined values, by pad, and is one undo unit each") {
  Rig rig(true, 64);
  rig.tap(Button::F2);
  rig.turn(1);
  CHECK(rig.project().pattern_length(1) == 65);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    CHECK(rig.project().pattern(1)->track_length[t] == 65);  // all 16 tracks
  }
  CHECK(rig.depth() == 1u);
  rig.tap(Button::Right);  // the next defined value above 65: 80
  CHECK(rig.project().pattern_length(1) == 80);
  rig.tap(Button::Right);
  CHECK(rig.project().pattern_length(1) == 96);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  CHECK(rig.project().pattern_length(1) == 128);  // the top
  rig.tap(Button::Right);
  CHECK(rig.project().pattern_length(1) == 128);
  rig.tap(Button::Left);  // below: 96
  CHECK(rig.project().pattern_length(1) == 96);
  rig.turn(-200);
  CHECK(rig.project().pattern_length(1) == 1);   // clamped to 1
  rig.tap(Button::Left);
  CHECK(rig.project().pattern_length(1) == 1);
  rig.pad(0);  // 3 + 0 * 188 / 47 = 3, a length of 4
  CHECK(rig.project().pattern_length(1) == 4);
  rig.pad(47);  // 191 clamped to the last step, 127: 128 steps
  CHECK(rig.project().pattern_length(1) == 128);
  rig.pad(24);  // 3 + 24 * 188 / 47 = 99: 100 steps
  CHECK(rig.project().pattern_length(1) == 100);
  // Each change is an undo unit; the cursor stays inside the pattern when it gets shorter.
  rig.tap(Button::F8);  // closes the strip
  rig.go(99);
  CHECK(rig.view().row() == 99);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // pad 24
  CHECK(rig.project().pattern_length(1) == 128);
  rig.tap(Button::F2);
  rig.pad(0);
  CHECK(rig.project().pattern_length(1) == 4);
  CHECK(rig.view().row() == 3);  // the cursor was clamped
  rig.tap(Button::F8);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().pattern_length(1) == 128);
  // A pattern that does not exist yet gets its length from the chooser, created in the same unit.
  Rig fresh(true, 0);
  fresh.tap(Button::F2);
  fresh.turn(-5);
  CHECK(fresh.project().pattern(1) != nullptr);
  CHECK(fresh.project().pattern_length(1) == static_cast<unsigned>(model::kDefaultPatternLength) - 5u);
  CHECK(fresh.depth() == 1u);
  (void)fresh.invoke("chooser.delta", 0);  // a zero turn changes nothing
  CHECK(fresh.depth() == 1u);
}

// OS-MIDIREC-P1 S2 (maintainer, 2026-11-10): a length cut that takes MIDI lane events the undo ring cannot hold asks first (TOO BIG TO UNDO); the same pick again goes on, records no undo
// and forgets the history. The Editor refuses it with Full before that (nothing changes).
TEST_CASE("OS-MIDIREC-P1: a length cut whose lane events do not fit the undo ring asks, and the second pick runs it without undo") {
  static model::LaneStore store;
  static model::LaneEvent big[4096];
  for (std::uint32_t i = 0; i < 4096; ++i) {
    big[i] = model::LaneEvent{};
    big[i].tick = static_cast<std::uint16_t>(48 + i);  // all past the first step: a cut to one step takes every event of every lane
    big[i].len = 1;
    big[i].a = static_cast<std::uint8_t>(i & 0x7F);
    big[i].b = 100;
  }
  // The pool (7,680 events, OS-MIDIREC-P1 decision 2026-11-10) is at most 61 KB of records, which the device's 160 KiB Pattern ring always holds: on the unit this
  // question is a safety net that no length cut reaches. So the rig runs on a 96 KiB undo region (a 40 KiB Pattern ring) to reach it.
  constexpr std::size_t kSmallRing = 96u * 1024u;
  Rig rig(true, 128, true, kSmallRing);
  store.clear();
  store.adopt(rig.project().load_epoch());
  rig.editor->attach_lanes(&store);
  // two lanes, 4096 + 3584 events (the whole pool): 32,784 + 28,688 bytes of records, more than the 40 KiB ring
  NEED(rig.editor->lane_replace(1, 0, 0, 6144, big, 4096) == model::Status::Ok);
  NEED(rig.editor->lane_replace(1, 1, 0, 6144, big, 3584) == model::Status::Ok);
  rig.tap(Button::F2);
  rig.turn(-200);  // to 1 step: every lane loses all its events
  CHECK(rig.project().pattern_length(1) == 128);  // refused: nothing changed
  CHECK(rig.state().edit.undo_ask != 0u);
  CHECK(store.lane(1, 0).count == 4096u);
  CHECK(rig.editor->can_undo());
  rig.turn(-200);  // the same pick again
  CHECK(rig.project().pattern_length(1) == 1);
  CHECK(store.lane(1, 0).count == 0u);
  CHECK(!rig.editor->can_undo());  // no undo, and no older history either
  CHECK(rig.state().edit.undo_ask == 0u);

  // A different pick between the two takes the question back.
  Rig again(true, 128, true, kSmallRing);
  store.clear();
  store.adopt(again.project().load_epoch());
  again.editor->attach_lanes(&store);
  NEED(again.editor->lane_replace(1, 0, 0, 6144, big, 4096) == model::Status::Ok);
  NEED(again.editor->lane_replace(1, 1, 0, 6144, big, 3584) == model::Status::Ok);
  again.tap(Button::F2);
  again.turn(-200);
  CHECK(again.state().edit.undo_ask != 0u);
  again.turn(-1);  // 127 steps: the cut is small, it fits and goes on with undo
  CHECK(again.project().pattern_length(1) == 127);
  CHECK(again.state().edit.undo_ask == 0u);

  // The device's own ring holds the same full pool: the cut goes on at once, undoable, and no question is asked.
  Rig device(true, 128, true);
  store.clear();
  store.adopt(device.project().load_epoch());
  device.editor->attach_lanes(&store);
  NEED(device.editor->lane_replace(1, 0, 0, 6144, big, 4096) == model::Status::Ok);
  NEED(device.editor->lane_replace(1, 1, 0, 6144, big, 3584) == model::Status::Ok);
  device.tap(Button::F2);
  device.turn(-200);
  CHECK(device.state().edit.undo_ask == 0u);
  CHECK(device.project().pattern_length(1) == 1);
  CHECK(store.lane(1, 0).count == 0u);
  CHECK(device.editor->can_undo());
}

TEST_CASE("pattern edit: the jump chooser: the edit step 0..32, the defined values, pads, and what it does to an entry") {
  Rig rig;
  rig.tap(Button::F3);
  CHECK(rig.edit().edit_step == 1);  // the default
  rig.turn(3);
  CHECK(rig.edit().edit_step == 4);
  rig.tap(Button::Right);  // 8
  CHECK(rig.edit().edit_step == 8);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  CHECK(rig.edit().edit_step == 32);  // the top
  rig.tap(Button::Right);
  CHECK(rig.edit().edit_step == 32);
  rig.tap(Button::Left);
  CHECK(rig.edit().edit_step == 16);
  rig.turn(-100);
  CHECK(rig.edit().edit_step == 0);
  rig.pad(5);
  CHECK(rig.edit().edit_step == 5);
  rig.pad(40);  // above 32 is 32
  CHECK(rig.edit().edit_step == 32);
  CHECK(rig.depth() == 0u);  // view state
  // The step is what an entry moves the cursor by: with 0 it stays, with 3 it goes three steps and wraps at the end of the pattern.
  rig.pad(0);
  rig.tap(Button::F8);
  rig.edit_mode();
  rig.go(0);
  (void)rig.invoke("step.note", 50);
  CHECK(rig.view().row() == 0);
  rig.tap(Button::F3);
  rig.pad(3);
  rig.tap(Button::F8);
  (void)rig.invoke("step.note", 51);
  CHECK(rig.view().row() == 3);
  rig.go(62);
  (void)rig.invoke("step.note", 52);
  CHECK(rig.view().row() == 1);  // (62 + 3) mod 64
  // The strip says it: the status shows the step (JUMP) and the length (LEN) of the document.
  CHECK(rig.edit().edit_step == 3);
}

TEST_CASE("pattern edit: a mode and a field change leave the gesture layer alone and the chain has five levels") {
  Rig rig;
  rig.edit_mode();
  // The F-key holds survive a field change: F1 held, the cursor moved by the encoder (Edit edits, so by Right), F1 released: a HoldEnd.
  rig.press(Button::F1);
  rig.pass(core::kGestureTimings.hold_ms + 1);
  CHECK(rig.state().overlay == app::OverlayId::PatternChooser);
  rig.release(Button::F1);
  rig.pass(1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  // The chain: overlay, mode, field, screen, global.
  CHECK(core::ContextChain::kMax == 5u);
  CHECK(app::kPatternScreen.field_context(rig.app->model(), rig.state()) == app::Context::PatternNote);
  rig.column(Column::Instr);
  CHECK(app::kPatternScreen.field_context(rig.app->model(), rig.state()) == app::Context::PatternInstr);
  rig.column(Column::Vol);
  CHECK(app::kPatternScreen.field_context(rig.app->model(), rig.state()) == app::Context::PatternVol);
  rig.column(Column::Fx1);
  CHECK(app::kPatternScreen.field_context(rig.app->model(), rig.state()) == app::Context::PatternFx);
  rig.column(Column::Fx2);
  CHECK(app::kPatternScreen.field_context(rig.app->model(), rig.state()) == app::Context::PatternFx);
  CHECK(app::kPatternScreen.default_mode() == static_cast<unsigned>(PatternMode::Browse));
  CHECK(app::ViewState().mode[kPattern] == static_cast<unsigned>(PatternMode::Browse));
}

// ---- undo ---------------------------------------------------------------------------------------------------------------------------------

namespace {

// What the oracle compares: patterns 1..3 (present, track lengths, name, all steps) and the 16 track slots' flags and names, the song.
struct Snap {
  struct Pat {
    bool present = false;
    std::uint8_t lengths[model::kMaxTracks] = {};
    model::Step steps[model::kMaxTracks][model::kMaxPatternLength];
  };
  Pat pat[3];
  std::uint8_t flags[model::kMaxTracks] = {};
  std::uint32_t song_length = 0;
  std::uint32_t patterns = 0;
};

Snap take(const model::Project& p) {
  Snap s;
  for (unsigned id = 1; id <= 3; ++id) {
    const model::Pattern* src = p.pattern(id);
    Snap::Pat& d = s.pat[id - 1];
    d.present = src != nullptr;
    if (src != nullptr) {
      std::memcpy(d.lengths, src->track_length, sizeof d.lengths);
      std::memcpy(d.steps, src->steps, sizeof d.steps);
    } else {
      std::memset(d.steps, 0, sizeof d.steps);
    }
  }
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    s.flags[t] = p.track(t)->flags;
  }
  s.song_length = p.song_length();
  s.patterns = p.pattern_count();
  return s;
}

bool same(const Snap& a, const Snap& b) {
  for (unsigned i = 0; i < 3; ++i) {
    if (a.pat[i].present != b.pat[i].present) return false;
    if (a.pat[i].present &&
        (std::memcmp(a.pat[i].lengths, b.pat[i].lengths, sizeof a.pat[i].lengths) != 0 || std::memcmp(a.pat[i].steps, b.pat[i].steps, sizeof a.pat[i].steps) != 0)) {
      return false;
    }
  }
  return std::memcmp(a.flags, b.flags, sizeof a.flags) == 0 && a.song_length == b.song_length && a.patterns == b.patterns;
}

struct Op {
  const char* name;
  int lo;
  int hi;
};

// The undoable actions of the slice, with the arguments that reach every branch (and some that do not, which must change nothing).
constexpr Op kOps[] = {
    {"step.note", 0, 130},       {"step.note_pad", -1, 48},   {"step.note_delta", -9, 9},   {"step.note_octave", -3, 3},
    {"step.instrument", 0, 66},  {"step.instrument_delta", -9, 9}, {"step.vol", -1, 128},   {"step.vol_pad", -1, 48},
    {"step.vol_delta", -9, 9},   {"step.fx_type", 0, 44},     {"step.fx_type_delta", -2, 2}, {"step.fx_value", -3, 260},
    {"step.fx_value_delta", -9, 9}, {"step.fx_pad", -1, 48},  {"step.clear", 0, 31},        {"step.insert", 0, 0},
    {"step.pull", 0, 0},    {"track.mute", -1, 13},      {"track.solo", -1, 13},
};
constexpr unsigned kOpCount = sizeof(kOps) / sizeof(kOps[0]);

// A move or a selection or a mode, none of them an undo unit.
void random_navigation(Rig& rig, test::Rng& rng) {
  switch (rng.below(12)) {
    case 0:
      (void)rig.invoke("nav.right");
      break;
    case 1:
      (void)rig.invoke("nav.left");
      break;
    case 2:
      (void)rig.invoke("nav.down");
      break;
    case 3:
      (void)rig.invoke("nav.up");
      break;
    case 4:
      (void)rig.invoke("nav.scroll", static_cast<int>(rng.below(21)) - 10);
      break;
    case 5:
      (void)rig.invoke("select.extend", static_cast<ActionArg>(rng.below(4) == 0 ? 10 : (rng.below(3) == 0 ? -10 : (rng.below(2) == 0 ? 1 : -1))));
      break;
    case 6:
      (void)rig.invoke("select.column_all");
      break;
    case 7:
      (void)rig.invoke("select.clear");
      break;
    case 8:
      (void)rig.invoke("cursor.jump_fraction", static_cast<ActionArg>(rng.below(4)));
      break;
    case 9:
      (void)rig.invoke("pattern.column", static_cast<ActionArg>(rng.below(5)));
      break;
    case 10:
      (void)rig.invoke("select.mark");
      break;
    default:
      (void)rig.invoke("view.mode", static_cast<ActionArg>(rng.below(2) == 0 ? 0 : 2));  // Edit or Select
      break;
  }
}

}  // namespace

TEST_CASE("pattern edit: undo and redo of random sequences of the edit actions, against full snapshots") {
  const unsigned episodes = std::getenv("O3OS_FUZZ_SCALE") != nullptr ? 60u * static_cast<unsigned>(std::atoi(std::getenv("O3OS_FUZZ_SCALE"))) : 60u;
  test::Rng rng(0x0509A1D0u);
  for (unsigned episode = 0; episode < episodes; ++episode) {
    const unsigned length = rng.below(3) == 0 ? 0u : 1u + rng.below(128);
    Rig rig(true, length);
    rig.edit_mode();
    if (rng.below(2) == 0) {
      (void)rig.project().set_track_length(1, 2, 1 + rng.below(40));  // an unequal track: edits past its end are refused
      rig.editor->history_reset();
    }
    std::vector<Snap> levels;  // levels[k]: the project after k undo units
    levels.push_back(take(rig.project()));
    bool walkable = true;      // false once the ring dropped or lost a unit: the oldest levels are gone
    const auto noted = [&] {   // after anything that may have made a unit: one level more, or none
      const unsigned depth = rig.depth();
      if (depth == levels.size()) {
        levels.push_back(take(rig.project()));
      } else if (depth != levels.size() - 1) {
        walkable = false;
      }
    };
    const unsigned ops = 120;
    for (unsigned n = 0; n < ops; ++n) {
      for (unsigned k = rng.below(3); k > 0; --k) {
        random_navigation(rig, rng);
      }
      if (rng.below(25) == 0) {  // a chooser in the middle of it: the pattern, the length or the jump
        const unsigned kind = rng.below(3);
        (void)rig.invoke("chooser.toggle", static_cast<ActionArg>(kind));
        noted();
        (void)rig.invoke("chooser.delta", static_cast<ActionArg>(rng.below(21)) - 10);
        noted();
        (void)rig.invoke("chooser.defined", rng.below(2) == 0 ? 1 : -1);
        noted();
        (void)rig.invoke("chooser.pad", static_cast<ActionArg>(rng.below(50)));
        noted();
        (void)rig.invoke("ui.back");
        if (rig.edit().pattern > 3) {  // the oracle watches patterns 1..3: stay inside them
          (void)rig.invoke("chooser.toggle", 0);
          (void)rig.invoke("chooser.pad", 0);
          (void)rig.invoke("ui.back");
        }
      }
      const Snap before = take(rig.project());
      const unsigned depth_before = rig.depth();
      const bool in_oracle = rig.edit().pattern >= 1 && rig.edit().pattern <= 3;
      if (rng.below(8) == 0) {  // a pad or the encoder through the bindings
        if (rng.below(2) == 0) {
          rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(rng.below(48)), 100));
        } else {
          rig.input.push(core::make_encoder_event(rig.clock.now(), 0, static_cast<std::int16_t>(static_cast<int>(rng.below(15)) - 7)));
        }
        rig.pass(1);
      } else {
        const Op& op = kOps[rng.below(kOpCount)];
        (void)rig.invoke(op.name, op.lo + static_cast<int>(rng.below(static_cast<unsigned>(op.hi - op.lo + 1))));
      }
      const Snap after = take(rig.project());
      CHECK_MESSAGE(rig.journal.damaged() == 0u, "episode " << episode << " op " << n);
      if (same(before, after)) {
        if (in_oracle) {
          CHECK_MESSAGE(rig.depth() == depth_before, "an edit that changed nothing made a unit: episode " << episode << " op " << n);
        }
        continue;
      }
      CHECK_MESSAGE((rig.depth() == depth_before + 1u || !walkable || rig.journal.dropped_units() > 0u),
                    "one action, one unit: episode " << episode << " op " << n);
      noted();
      if (!in_oracle || !walkable) {
        continue;
      }
      // Undo it and redo it at once: the project must read as before, then as after.
      CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
      CHECK_MESSAGE(same(take(rig.project()), before), "undo is not exact: episode " << episode << " op " << n);
      CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
      CHECK_MESSAGE(same(take(rig.project()), after), "redo is not exact: episode " << episode << " op " << n);
    }
    // Back through the whole history and forward again, each level exact.
    if (walkable && rig.journal.dropped_units() == 0u && rig.journal.lost_units() == 0u && rig.depth() == levels.size() - 1) {
      const std::size_t top = levels.size() - 1;
      std::size_t level = top;
      while (level > 0) {
        CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
        --level;
        CHECK_MESSAGE(same(take(rig.project()), levels[level]), "walking back to level " << level << ", episode " << episode);
      }
      CHECK(rig.depth() == 0u);
      while (level < top) {
        CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
        ++level;
        CHECK_MESSAGE(same(take(rig.project()), levels[level]), "walking forward to level " << level << ", episode " << episode);
      }
      CHECK(rig.journal.redo_depth() == 0u);
    }
    CHECK(rig.journal.damaged() == 0u);
  }
}

TEST_CASE("pattern edit: what an action costs the journal, and the history after twenty of the worst") {
  Rig rig(true, 128);
  rig.edit_mode();
  const auto bytes = [&](const char* name, ActionArg arg) {
    const std::size_t before = rig.journal.used_bytes();
    const unsigned depth = rig.depth();
    (void)rig.invoke(name, arg);
    return rig.depth() == depth + 1u ? rig.journal.used_bytes() - before : std::size_t{0};
  };
  const bool report = std::getenv("O3OS_UNDO_REPORT") != nullptr;
  rig.go(0);
  const std::size_t note = bytes("step.note", 50);
  rig.go(0);
  const std::size_t delta = bytes("step.note_delta", 1);
  rig.go(0);
  const std::size_t mute = bytes("track.mute", 3);
  rig.go(0, 0);
  rig.extend_tracks(11);
  const std::size_t block = bytes("step.note_delta", 1);  // a block of 12 tracks: one record per track that has a note
  rig.go(0, 0);
  rig.extend_tracks(11);
  // Make every step a note first so that insert and clear have the whole 128 steps to move.
  rig.go(0);
  rig.mode(PatternMode::Edit);
  std::vector<model::Step> full(128, model::kEmptyStep);
  for (model::Step& s : full) {
    s.note = 40;
    s.instr = 1;
  }
  for (unsigned t = 0; t < 12; ++t) {
    (void)rig.project().set_steps(1, t, 0, full.data(), 128);
  }
  rig.editor->history_reset();
  rig.go(0, 0);
  rig.extend_tracks(11);
  const std::size_t insert = bytes("step.insert", 0);  // the worst: 12 tracks x 128 steps
  const std::size_t clear = bytes("step.clear", 0);
  rig.editor->history_reset();
  rig.go(0, 0);
  rig.extend_tracks(11);
  for (unsigned n = 0; n < 20; ++n) {
    rig.go(0, 0);
    rig.extend_tracks(11);
    (void)rig.invoke("step.insert", 0);
  }
  const unsigned depth = rig.depth();
  if (report) {
    std::fprintf(stderr, "journal bytes: note %zu, delta %zu, mute %zu, block delta (12 tracks) %zu, insert (12 x 128) %zu, clear (12 x 128) %zu; depth after 20 inserts %u, dropped %u\n",
                 note, delta, mute, block, insert, clear, depth, static_cast<unsigned>(rig.journal.dropped_units()));
  }
  CHECK(note > 0);
  CHECK(note <= 64);
  CHECK(delta <= 64);
  CHECK(mute <= 128);
  CHECK(insert <= 29000);   // the spec's figure for a whole-pattern change is about 29 KB
  CHECK(insert > 20000);
  CHECK(depth >= 13u);      // 384 KiB hold at least thirteen of the worst
  CHECK(rig.journal.damaged() == 0u);
  CHECK(rig.journal.lost_units() == 0u);
  // And every one of them undoes.
  for (unsigned n = 0; n < depth; ++n) {
    CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  }
  CHECK(rig.depth() == 0u);
}

TEST_CASE("pattern edit: a macro of an edit run replays as one undo unit") {
  Rig rig;
  rig.edit_mode();
  (void)rig.invoke("macro.record");
  (void)rig.invoke("step.note_pad", 36);
  (void)rig.invoke("step.note_pad", 40);
  (void)rig.invoke("step.note_delta", 2);
  (void)rig.invoke("macro.record");
  CHECK(rig.app->session().macro.count == 3u);
  CHECK(rig.depth() == 3u);
  rig.go(10);
  const unsigned before = rig.depth();
  CHECK(rig.invoke("macro.play") == DispatchResult::Changed);
  CHECK(rig.depth() == before + 1u);  // the whole run is one unit
  CHECK(rig.step(0, 10).note == 37);
  CHECK(rig.step(0, 11).note == 41);
  CHECK(rig.step(0, 12).note == 61);  // (the delta acted on the step the cursor had reached: an empty step takes note 60)
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 10).note == 0);
  CHECK(rig.step(0, 11).note == 0);
  CHECK(rig.depth() == before);
}

TEST_CASE("pattern edit: the disabled and the refused paths change nothing") {
  Rig rig;
  rig.edit_mode();
  const unsigned depth = rig.depth();
  // Reserved and unenterable note bytes, huge deltas on an empty step and on notes, ids out of range.
  for (const int bad : {0, 129, 200, 252, -1, -100, 2000000000}) {
    CHECK_MESSAGE(rig.invoke("step.note", bad) != DispatchResult::Changed, bad);
  }
  CHECK(rig.depth() == depth);
  CHECK(rig.invoke("step.note", 255) == DispatchResult::Changed);  // off is enterable
  rig.go(0);
  (void)rig.invoke("step.note_delta", 2000000000);
  CHECK(rig.step(0, 0).note == 128);  // the rank order is Off < Cut < Fade < Empty < C-0 ..: a huge + from Off is the last note, G-9
  (void)rig.invoke("step.instrument", 2000000000);
  (void)rig.invoke("step.instrument", -5);
  (void)rig.invoke("step.vol", 2000000000);
  (void)rig.invoke("step.fx_type", 2000000000);
  (void)rig.invoke("step.fx_value", 0);  // not on an FX column
  (void)rig.invoke("step.clear", -1);
  (void)rig.invoke("chooser.pad", 2000000000);
  (void)rig.invoke("track.mute", 2000000000);
  // Everything the actions refuse leaves the pattern the way it was (the huge note delta above did edit, once).
  const model::Pattern* p = rig.project().pattern(1);
  NEED(p != nullptr);
  CHECK(p->steps[0][1].note == 0);
  CHECK(rig.project().track(0)->flags == 0);
}

TEST_CASE("pattern view: the cursor, the window and the block go back inside a pattern that got shorter, or has fewer tracks") {
  app::PatternView v;
  const app::PatternView::Dims big{128, 16};
  CHECK(v.scroll_rows(100, big));
  for (int i = 0; i < 5 * 14; ++i) {
    (void)v.move_right(big);
  }
  CHECK((v.row() == 100 && v.track() == 14 && v.top_row() == 93 && v.left_track() == 10));  // OS-CENTER: the cursor's row 100 on the centre slot (93 + 7)
  CHECK(v.clamp_to(app::PatternView::Dims{20, 8}));
  CHECK((v.row() == 19 && v.track() == 7 && v.top_row() == 12 && v.left_track() == 3));  // the window follows the cursor, on the centre slot (OS-PATCENTER: the pattern's end does not stop it)
  CHECK_FALSE(v.clamp_to(app::PatternView::Dims{20, 8}));  // already inside: nothing moved
  CHECK(v.clamp_to(app::PatternView::Dims{1, 1}));
  CHECK((v.row() == 0 && v.track() == 0 && v.top_row() == -app::PatternView::kCentreRow && v.left_track() == 0));  // (row 0 is on the centre slot, the slots above it blank)
  // The moves take the pattern's own size: tracks and steps of a smaller pattern are the limits.
  app::PatternView w;
  const app::PatternView::Dims small{5, 2};
  CHECK(w.scroll_rows(100, small));
  CHECK(w.row() == 4);
  CHECK_FALSE(w.move_down(small));
  CHECK(w.move_track(5, small));
  CHECK(w.track() == 1);
  CHECK(w.end(small));
  CHECK((w.track() == 1 && w.column() == Column::Fx2));
  CHECK_FALSE(w.move_right(small));
  // In the app: a block that no longer fits ends when the pattern gets shorter, and the cursor stays inside.
  Rig rig(true, 64);
  rig.go(5);
  for (int i = 0; i < 25; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  CHECK(rig.edit().selection.active);
  CHECK(rig.view().row() == 30);
  rig.tap(Button::F2);
  rig.pad(0);  // 4 steps
  CHECK(rig.project().pattern_length(1) == 4);
  CHECK_FALSE(rig.edit().selection.active);
  CHECK(rig.view().row() == 3);
  // Undo of the length brings the pattern back and the cursor is where it was put (it does not jump back).
  rig.tap(Button::F8);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().pattern_length(1) == 64);
  CHECK(rig.view().row() == 3);
}

TEST_CASE("pattern edit: the strip, the info line and the Shift layer are what the screen says (the status, not the pixels)") {
  Rig rig(true, 48);
  // The mode chip, the scale and the undo depth reach the picture: two different states draw two different frames.
  const auto words = [&](const app::ViewState& v) {
    platform::host::FakeDisplay sink;
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, ui::theme(ui::ThemeId::Amiga));
    const bool began = r.begin(ui::Color::Pane);
    CHECK(began);
    app::render_scene(rig.app->model(), v, r);
    CHECK(r.end());
    return sink.last_frame();
  };
  app::ViewState a = rig.state();
  app::ViewState b = a;
  CHECK(words(a) == words(b));
  b.mode[kPattern] = static_cast<std::uint8_t>(PatternMode::Edit);
  CHECK(words(a) != words(b));  // WATCH and LIVE
  b = a;
  b.shift = true;
  CHECK(words(a) != words(b));  // the Shift layer of the key row
  b.overlay = app::OverlayId::Learn;
  app::ViewState c = a;
  c.overlay = app::OverlayId::Learn;
  CHECK(words(b) == words(c));  // under an overlay the layer is not drawn: the keys are the overlay's
  b = a;
  b.edit.edit_step = 7;
  CHECK(words(a) != words(b));  // the strip's JUMP
  b = a;
  b.edit.entry.root = 4;        // the info line's scale and root
  b.edit.apply_entry();
  CHECK(words(a) != words(b));
  b = a;
  b.mode[kPattern] = static_cast<std::uint8_t>(PatternMode::Edit);
  c = b;
  c.edit.rec_free = true;       // the info line's chip: LIVE, STEP (OS-RECMODE2)
  CHECK(words(b) != words(c));
  b = a;
  b.edit.selection.active = true;
  b.edit.selection.start_row = 2;
  b.edit.selection.end_row = 6;
  CHECK(words(a) != words(b));  // the block
}

TEST_CASE("pattern edit: the info line names the scale in force, from the one name table, with no trailing space (QA-PAT-433, OS-SCALES28 S2)") {
  // `assert state` reads `diag state`, which has no scale, so the scenario cannot assert this line: the display list is looked at. The Label-face glyphs on the info line's baseline are put back on the
  // character grid from the text's own left edge (a space draws nothing and advances), and the chip that follows starts after the text plus its gap, which a trailing space would move.
  Rig rig(true, 48);
  const ui::Theme& theme = ui::theme(ui::ThemeId::Amiga);
  const auto info_line = [&](const app::ViewState& v, std::string& chip_gap_ok, unsigned want_len) {
    platform::host::FakeDisplay sink;
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    r.use_translate(false);  // a glyph is a VERTEX2II (x < 512)
    const bool began = r.begin(ui::Color::Pane);
    CHECK(began);
    app::render_scene(rig.app->model(), v, r);
    CHECK(r.end());
    const app::Frame f = app::make_frame(theme, app::PatternView::kVisibleRows);
    const int y = ui::caps_y(theme, ui::Face::Label, f.info.y, f.info.h);
    const int cell = theme.geometry(ui::Face::Label).cell_w;
    const int x0 = 2 * theme.metrics.pad + 5 * cell + 2 * theme.metrics.pad + 3 * theme.metrics.gap;  // OS-INFOLINE: the chip (WATCH, here) is first; the text starts after it and its gap
    const std::uint32_t label = ui::font_face(theme.font(ui::Face::Label)).handle;
    std::string line(64, ' ');
    int first_after = -1;  // the first glyph past the text: the chip's
    bool bitmaps = false;
    for (const std::uint32_t w : sink.last_frame()) {
      if ((w >> 24) == (eve::dl::begin(eve::Primitive::Bitmaps) >> 24)) {
        bitmaps = (w & 0xFu) == static_cast<std::uint32_t>(eve::Primitive::Bitmaps);
      } else if (bitmaps && (w >> 30) == eve::opcode::kVertex2iiTag && static_cast<int>((w >> 12) & 0x1FFu) == y && ((w >> 7) & 0x1Fu) == label) {
        const int x = static_cast<int>((w >> 21) & 0x1FFu);
        const int c = (x - x0) / cell;
        if (x >= x0 && (x - x0) % cell == 0 && c >= 0 && c < static_cast<int>(want_len)) {
          line[static_cast<std::size_t>(c)] = static_cast<char>((w & 0x7Fu) + 32u);
        } else if (x >= x0 + static_cast<int>(want_len) * cell && (first_after < 0 || x < first_after)) {
          first_after = x;
        }
      }
    }
    line.resize(want_len);
    chip_gap_ok = first_after == x0 + (static_cast<int>(want_len) + 3) * cell ? "" : "the 'pads' word is not where the text's length puts it (" + std::to_string(first_after) + ")";  // (three spaces, then "pads NOTES")
    return line;
  };
  // The name is the one table's (core::kScaleNames, the official index + 1), for every scale; and the QA row's own line, literally (but for the octave's name, see below).
  for (unsigned scale = 0; scale <= model::kNoteScaleMax; ++scale) {
    const std::string want = std::string("scale ") + core::kScaleNames[scale + 1u] + " C-3   track 1";  // the Rig has no device setting, so the root C in octave 3 is C-3 (the simulator's MIDDLE C default names it C-4, as QA-PAT-433 reads)
    app::ViewState v = rig.state();
    v.edit.entry.scale = static_cast<std::uint8_t>(scale);
    v.edit.apply_entry();
    std::string gap;
    const std::string got = info_line(v, gap, static_cast<unsigned>(want.size()));
    CHECK_MESSAGE(got == want, "scale " << scale << ": '" << got << "'");
    CHECK_MESSAGE(gap.empty(), "scale " << scale << ": " << gap);
    if (scale == 26) {
      CHECK(want == "scale Hirajoshi C-3   track 1");  // QA-PAT-433's line
    }
  }
}

namespace {
struct TransportLog final : app::TransportSink {
  unsigned plays = 0, stops = 0, pattern = 0, songs = 0, row = 0;
  bool accept = true;
  bool play_song(unsigned r) noexcept override {
    if (!accept) {
      return false;
    }
    row = r;
    ++songs;
    return true;
  }
  bool play(unsigned p) noexcept override {
    if (!accept) {
      return false;
    }
    pattern = p;
    ++plays;
    return true;
  }
  bool stop() noexcept override {
    if (!accept) {
      return false;
    }
    ++stops;
    return true;
  }
};
}  // namespace

TEST_CASE("pattern edit: Play plays the pattern being edited and stops it, from any screen; with no transport, or a refusal, nothing changes") {
  Rig rig;
  rig.tap(Button::Play);  // no transport attached (the simulator): the action is Disabled
  CHECK_FALSE(rig.app->state().playing);
  TransportLog t;
  rig.app->attach_transport(&t);
  t.accept = false;
  rig.tap(Button::Play);  // the engine refuses (no audio path): the badge does not change
  CHECK_FALSE(rig.app->state().playing);
  t.accept = true;
  rig.tap(Button::Play);
  CHECK(t.plays == 1u);
  CHECK(t.pattern == rig.app->state().edit.pattern);
  CHECK(rig.app->state().playing);
  rig.tap(Button::Song);  // the music is stopped from another screen
  rig.tap(Button::Play);
  CHECK(t.stops == 1u);
  CHECK(t.plays == 1u);
  CHECK_FALSE(rig.app->state().playing);
}

TEST_CASE("pattern edit: on the Song screen Play plays the song from the selected row, on the Pattern screen it stays the pattern (OS-005e)") {
  Rig rig(false);  // no project: the Song screen is the demo's 24 positions (the project's list is the next test)
  TransportLog t;
  rig.app->attach_transport(&t);
  rig.tap(Button::Play);  // Pattern screen: Pattern mode
  CHECK(t.plays == 1u);
  CHECK(t.songs == 0u);
  rig.tap(Button::Play);
  CHECK(t.stops == 1u);
  rig.tap(Button::Song);
  for (int i = 0; i < 3; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.app->state().song.pos() == 3);
  rig.tap(Button::Play);  // the song, from the selected row
  CHECK(t.songs == 1u);
  CHECK(t.row == 3u);
  CHECK(t.plays == 1u);
  CHECK(rig.app->state().playing);
  rig.tap(Button::Play);  // PLAY SNG while it plays: stops
  CHECK(t.stops == 2u);
  CHECK_FALSE(rig.app->state().playing);
  rig.tap(Button::Down);
  rig.tap(Button::Play);
  CHECK(t.songs == 2u);
  CHECK(t.row == 4u);
  t.accept = false;  // a refusal changes nothing
  rig.tap(Button::Play);
  CHECK(rig.app->state().playing);
}

TEST_CASE("song source: with a project the Song screen lists its order list and Play starts from a row of it; the marker follows the song row (OS-005f)") {
  Rig rig(true, 64);  // pattern 1 (64 steps, no name), order [1]
  model::Project& p = rig.project();
  (void)p.create_pattern(2, 32);
  (void)p.set_pattern_name(2, "VERSE", 5);
  (void)p.append_order(2);
  (void)p.append_order(9);  // pattern 9 does not exist yet: listed, and it plays empty
  const app::SongSource src(rig.app->model());
  CHECK_FALSE(src.is_demo());
  CHECK(src.count() == 3);
  CHECK(src.pattern(1) == 2u);
  CHECK(std::strcmp(src.name(1), "VERSE") == 0);
  CHECK(src.steps(0) == 64);
  CHECK(src.steps(1) == 32);
  CHECK(src.exists(1));
  CHECK_FALSE(src.exists(2));
  CHECK(src.pattern(2) == 9u);
  CHECK(std::strcmp(src.name(2), "") == 0);
  CHECK(src.steps(2) == static_cast<int>(p.default_pattern_length()));  // what the sequencer plays for a missing pattern
  CHECK(src.pattern(3) == 0u);
  CHECK_FALSE(src.exists(3));

  TransportLog t;
  rig.app->attach_transport(&t);
  rig.tap(Button::Song);
  CHECK(rig.app->state().screen == app::ScreenId::Song);
  for (int i = 0; i < 5; ++i) {
    rig.tap(Button::Down);  // the list has 3 rows, not the demo's 24
  }
  CHECK(rig.app->state().song.pos() == 2);
  rig.tap(Button::Play);
  CHECK(t.songs == 1u);
  CHECK(t.row == 2u);

  // The marker is the song row the sequencer publishes while it plays the song: none stopped or in Pattern mode (the demo keeps its fixed one).
  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  rig.app->run_once();
  CHECK(src.marker(rig.state().play) == -1);
  const auto frames = rig.app->frames();
  pos.publish(true, 2, 3, 12000, 1);
  rig.app->run_once();
  CHECK(src.marker(rig.state().play) == 1);
  CHECK(rig.app->frames() > frames);  // the Song screen redraws for the new row
  pos.publish(true, 2, 4, 12000, audio::PlayPosition::kNoOrder);
  rig.app->run_once();
  CHECK(src.marker(rig.state().play) == -1);

  // A pattern that does not exist yet is drawn dimmed: creating it changes the picture.
  const auto picture = [&] {
    platform::host::FakeDisplay sink;
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, ui::theme(ui::ThemeId::Amiga));
    const bool began = r.begin(ui::Color::Pane);
    CHECK(began);
    app::render_scene(rig.app->model(), rig.state(), r);
    CHECK(r.end());
    return sink.last_frame();
  };
  const auto before = picture();
  (void)p.create_pattern(9, 16);
  CHECK(picture() != before);
}

TEST_CASE("song source: an empty song lists nothing and Play starts nothing from it; the cursor stays inside the list when the project changes (OS-005f)") {
  Rig rig;
  model::Project& p = rig.project();
  (void)p.clear_order();
  TransportLog t;
  rig.app->attach_transport(&t);
  rig.tap(Button::Song);
  CHECK(app::SongSource(rig.app->model()).count() == 0);
  rig.tap(Button::Down);
  CHECK(rig.app->state().song.pos() == 0);
  rig.tap(Button::Play);  // no row to play from: a notification, no transport request, no badge
  CHECK(t.songs == 0u);
  CHECK(t.plays == 0u);
  CHECK_FALSE(rig.app->state().playing);
  CHECK(rig.app->state().notify.size() == 1u);
  rig.flush();

  for (int i = 0; i < 4; ++i) {
    (void)p.append_order(1);
  }
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.app->state().song.pos() == 3);
  // The project shrinks behind the view (another one is attached, an order edit is undone): Play takes a row the list has, and the view follows.
  (void)p.clear_order();
  (void)p.append_order(1);
  (void)p.append_order(1);
  CHECK(rig.app->state().song.pos() == 3);
  rig.tap(Button::Play);
  CHECK(t.songs == 1u);
  CHECK(t.row == 1u);
  CHECK(rig.app->state().song.pos() == 1);
  rig.tap(Button::Play);  // stop
  (void)p.clear_order();
  (void)p.append_order(1);
  rig.app->attach_document(rig.editor.get());  // attached again: the cursor is back inside the list at once
  CHECK(rig.app->state().song.pos() == 0);
}

TEST_CASE("pattern edit: open_pattern shows the pattern a loaded project was saved on, when the project has it, and leaves the view alone otherwise (OS-007j)") {
  Rig rig;  // pattern 1
  (void)rig.project().create_pattern(4, 32);
  CHECK(rig.app->open_pattern(4));
  CHECK(rig.edit().pattern == 4);
  CHECK_FALSE(rig.app->open_pattern(9));    // the project has no pattern 9
  CHECK_FALSE(rig.app->open_pattern(0));    // a pattern number is 1 or more
  CHECK_FALSE(rig.app->open_pattern(256));
  CHECK(rig.edit().pattern == 4);
  CHECK(rig.app->open_pattern(1));
  CHECK(rig.edit().pattern == 1);
}

// ---- the step popups (OS-FU3) --------------------------------------------------------------------------------------------------------------

namespace {
void hold_open(Rig& rig, Button b) {
  rig.press(b);
  rig.pass(core::kGestureTimings.hold_ms + 1);
}
void let_go(Rig& rig, Button b) {
  rig.release(b);
  rig.pass(1);
}
}  // namespace

TEST_CASE("step popups: a held Instr opens the instrument list, the encoder and the arrows choose, the release enters it in Edit as one undo unit") {
  using app::OverlayId;
  Rig rig;
  rig.edit_mode();
  rig.go(2);
  const unsigned before = rig.depth();
  hold_open(rig, Button::Instr);
  CHECK(rig.state().overlay == OverlayId::StepPopup);
  CHECK(rig.state().popup.kind == app::StepPopupView::Instr);
  CHECK(rig.view().column() == Column::Instr);
  const int start = rig.state().instrument;
  rig.turn(3);
  CHECK(rig.state().instrument == start + 3);
  rig.tap(Button::Right);  // +12
  CHECK(rig.state().instrument == start + 15);
  rig.tap(Button::Up);  // -1
  CHECK(rig.state().instrument == start + 14);
  CHECK(rig.step(0, 2).instr == 0);  // nothing is entered while it is open
  let_go(rig, Button::Instr);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.step(0, 2).instr == start + 14);
  CHECK(rig.depth() == before + 1);
  CHECK(rig.view().row() == 2);  // the cursor stays
  (void)rig.invoke("edit.undo");
  CHECK(rig.step(0, 2).instr == 0);
}

// OS-INSTRMIDI. The Instr column takes 1..64 (clamps to 0..47 + 16 through this OS's order: the
// MIDI channels 49..64 first, then 1..48), so turning the encoder up from 1 stops at 48 and the way to 49..64 is down from 1.
TEST_CASE("pattern edit: the Instr column's encoder walks 1..64 in this OS's order (MIDI channels first) and stops at both ends") {
  Rig rig;
  rig.edit_mode();
  (void)rig.invoke("step.note", 50);
  rig.go(0);
  rig.column(Column::Instr);
  const auto at = [&](unsigned instrument) {
    (void)rig.invoke("step.instrument", static_cast<ActionArg>(instrument));  // (an entry moves the cursor on by JUMP: back to step 1)
    rig.go(0);
    rig.column(Column::Instr);
  };
  at(48);
  CHECK(rig.step(0, 0).instr == 48);
  rig.turn(1);
  CHECK(rig.step(0, 0).instr == 48);  // the end of the order
  at(49);
  rig.turn(1);
  CHECK(rig.step(0, 0).instr == 1);  // the order goes from the MIDI channel 1 to the first sample
  rig.turn(-1);
  CHECK(rig.step(0, 0).instr == 49);
  rig.turn(-1);
  CHECK(rig.step(0, 0).instr == 50);
  CHECK(rig.state().instrument == 50);  // the last used instrument follows
  at(64);
  rig.turn(-1);
  CHECK(rig.step(0, 0).instr == 64);  // the start of the order
  rig.turn(1);
  CHECK(rig.step(0, 0).instr == 63);
  at(1);
  rig.turn(-1);
  CHECK(rig.step(0, 0).instr == 49);
  rig.turn(-5);
  CHECK(rig.step(0, 0).instr == 54);
  for (unsigned i = 0; i < 70; ++i) {
    rig.turn(-5);
  }
  CHECK(rig.step(0, 0).instr == 64);
  for (unsigned i = 0; i < 30; ++i) {
    rig.turn(5);
  }
  CHECK(rig.step(0, 0).instr == 48);
  at(65);  // out of range: refused, the step keeps what it had
  CHECK(rig.step(0, 0).instr == 48);
}

TEST_CASE("step popups: the instrument list names a MIDI instrument \"MIDI CH n\" on its channel, any other by its name (OS-INSTRMIDI)") {
  core::FixedString<40> text;
  app::step_popup_instrument_text(nullptr, text);
  CHECK(text == "");
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 1;
  std::strcpy(in.name, "MIDI 1");  // what our OG import calls them
  app::step_popup_instrument_text(&in, text);
  CHECK(text == "MIDI CH 1");
  in.midi_channel = 16;
  app::step_popup_instrument_text(&in, text);
  CHECK(text == "MIDI CH 16");
  model::init_instrument(in, model::InstrumentKind::Sample);
  std::strcpy(in.name, "KICK");
  app::step_popup_instrument_text(&in, text);
  CHECK(text == "KICK");
  model::init_instrument(in, model::InstrumentKind::None);
  app::step_popup_instrument_text(&in, text);
  CHECK(text == "");
}

TEST_CASE("step popups: Browse chooses the instrument but writes nothing; F8 closes without entering; a long list is clamped to 1..64") {
  using app::OverlayId;
  Rig rig;  // Browse
  rig.go(1);
  hold_open(rig, Button::Instr);
  CHECK(rig.state().overlay == OverlayId::StepPopup);
  rig.turn(1000);
  CHECK(rig.state().instrument == 64);
  rig.turn(-1000);
  CHECK(rig.state().instrument == 1);
  rig.pad(4);  // a pad: instrument 5
  CHECK(rig.state().instrument == 5);
  let_go(rig, Button::Instr);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.step(0, 1).instr == 0);
  CHECK(rig.depth() == 0);
  rig.edit_mode();
  hold_open(rig, Button::Instr);
  rig.turn(2);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == OverlayId::None);
  let_go(rig, Button::Instr);
  CHECK(rig.step(0, 1).instr == 0);
  CHECK(rig.depth() == 0);
}

TEST_CASE("step popups: a held Fx opens the FX list of FX column 2 on the step's own FX, the release enters the type") {
  using app::OverlayId;
  Rig rig;
  rig.edit_mode();
  rig.go(3);
  rig.column(Column::Fx2);
  (void)rig.invoke("step.fx_type", 4);  // Chance, on the FX2 column
  rig.column(Column::Note);
  hold_open(rig, Button::Fx);
  CHECK(rig.state().overlay == OverlayId::StepPopup);
  CHECK(rig.state().popup.kind == app::StepPopupView::Fx);
  CHECK(rig.view().column() == Column::Fx2);
  CHECK(rig.state().popup.item == model::fx::picker_index(4));
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  const int item = rig.state().popup.item;
  CHECK(item == model::fx::picker_index(4) + 2);
  const unsigned before = rig.depth();
  let_go(rig, Button::Fx);
  CHECK(rig.step(0, 3).fx[1].type == model::fx::type_of_picker(item));
  CHECK(rig.step(0, 3).fx[0].type == 0);  // FX1 untouched
  CHECK(rig.depth() == before + 1);
  // A pad chooses an FX by its index.
  hold_open(rig, Button::Fx);
  rig.pad(2);
  CHECK(rig.state().popup.item == 2);
  let_go(rig, Button::Fx);
  CHECK(rig.step(0, 3).fx[1].type == model::fx::type_of_picker(2));
}

TEST_CASE("step popups: a held Vol (the FX1 button) opens the FX list of FX column 1; pad, encoder and arrows choose, the release enters the FX1 type") {
  using app::OverlayId;
  Rig rig;
  rig.edit_mode();
  rig.go(3);
  rig.column(Column::Fx1);
  (void)rig.invoke("step.fx_type", 4);  // Chance, on the FX1 column
  rig.column(Column::Note);
  hold_open(rig, Button::Vol);
  CHECK(rig.state().overlay == OverlayId::StepPopup);
  CHECK(rig.state().popup.kind == app::StepPopupView::Vol);
  CHECK(rig.view().column() == Column::Fx1);
  CHECK(rig.state().popup.item == model::fx::picker_index(4));  // opens on the step's own FX1
  rig.tap(Button::Down);
  rig.turn(2);
  const int item = rig.state().popup.item;
  CHECK(item == model::fx::picker_index(4) + 3);
  const unsigned before = rig.depth();
  let_go(rig, Button::Vol);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.step(0, 3).fx[0].type == model::fx::type_of_picker(item));
  CHECK(rig.step(0, 3).fx[1].type == 0);  // FX2 untouched
  CHECK(rig.depth() == before + 1);
  // A pad chooses an FX by its index, from the other FX column too (the button names the column, not the cursor).
  rig.column(Column::Fx2);
  hold_open(rig, Button::Vol);
  CHECK(rig.view().column() == Column::Fx1);
  rig.pad(2);
  CHECK(rig.state().popup.item == 2);
  let_go(rig, Button::Vol);
  CHECK(rig.step(0, 3).fx[0].type == model::fx::type_of_picker(2));
  CHECK(rig.step(0, 3).fx[1].type == 0);
  // A held Vol does not select the Vol column the way its tap does, and Browse enters nothing.
  Rig browse;
  browse.go(1);
  hold_open(browse, Button::Vol);
  CHECK(browse.state().overlay == OverlayId::StepPopup);
  let_go(browse, Button::Vol);
  CHECK(browse.depth() == 0);
}

TEST_CASE("step popups: an FX picked from the list keeps the cell's value, clamped; on an empty cell it starts at the table default (OS-PFXFIX, new behaviour; the reference vectors give 0)") {
  // On an EMPTY cell the picked type starts at its table default, as the value edit does (Tune 24 = 0 semitones, Micro-tune 99). A cell that already has a type keeps
  // its value: carried over and constrained (pinned below and against the reference vectors in pattern_edit_rules_test).
  Rig rig;
  rig.edit_mode();
  rig.go(3);
  rig.column(Column::Fx1);
  CHECK(model::fx::row(40).start == 24);
  CHECK(model::fx::row(34).start == 99);
  rig.column(Column::Note);
  hold_open(rig, Button::Vol);
  rig.turn(model::fx::picker_index(40));  // from None to Tune
  let_go(rig, Button::Vol);
  CHECK(rig.step(0, 3).fx[0].type == 40);
  CHECK(rig.step(0, 3).fx[0].value == 24);
  rig.turn(7);  // the encoder is on the FX1 column now: 24 + 7
  CHECK(rig.step(0, 3).fx[0].value == 31);
  hold_open(rig, Button::Vol);  // Tune 31 -> Micro-tune: the value is kept (31 is inside 0..198), not 99
  rig.turn(static_cast<int>(model::fx::picker_index(34)) - static_cast<int>(model::fx::picker_index(40)));
  let_go(rig, Button::Vol);
  CHECK(rig.step(0, 3).fx[0].type == 34);
  CHECK(rig.step(0, 3).fx[0].value == 31);
  hold_open(rig, Button::Vol);  // -> Bit Depth (1..16): clamped to 16
  rig.turn(static_cast<int>(model::fx::picker_index(39)) - static_cast<int>(model::fx::picker_index(34)));
  let_go(rig, Button::Vol);
  CHECK(rig.step(0, 3).fx[0].type == 39);
  CHECK(rig.step(0, 3).fx[0].value == 16);
}

TEST_CASE("step popups: a held Note shows the note-entry settings on its pads and enters nothing (OS-PADS-B)") {
  Rig rig;
  rig.edit_mode();
  rig.go(2);
  hold_open(rig, Button::Note);
  CHECK(rig.state().popup.kind == app::StepPopupView::Note);
  rig.pad(2);  // row 1: ROOT D
  CHECK(rig.state().edit.entry.root == 2);
  rig.pad(15);  // row 2: SCALE 3, Dorian
  CHECK(rig.state().edit.entry.scale == 3);
  rig.pad(26);  // row 3: JUMP 2 (the F3 chooser's setting)
  CHECK(rig.state().edit.edit_step == 2);
  let_go(rig, Button::Note);
  CHECK(rig.step(0, 2).note == model::kNoteEmpty);  // nothing is entered by the release
  CHECK(rig.depth() == 0);
  CHECK(rig.state().overlay == app::OverlayId::None);
}

TEST_CASE("step popups: with Note held the encoder and Up / Down walk the 39 scales as a ring in the official order (OS-SCALES28 S2)") {
  using app::OverlayId;
  Rig rig;
  rig.edit_mode();
  rig.go(2);
  hold_open(rig, Button::Note);
  CHECK(rig.state().popup.kind == app::StepPopupView::Note);
  CHECK(rig.state().edit.entry.scale == 2);  // Major, the default of a new project (OS-DEFSCALE)
  rig.pad(12);                               // pad 1 of row 2: Chromatic, the start of the ring in the counts below
  CHECK(rig.state().edit.entry.scale == 0);
  rig.turn(1);
  CHECK(rig.state().edit.entry.scale == 1);   // Minor: the official order, one a detent
  rig.turn(-2);
  CHECK(rig.state().edit.entry.scale == 38);  // wraps below 0 to the last, Enigmatic
  rig.turn(1);
  CHECK(rig.state().edit.entry.scale == 0);   // and round again to Chromatic
  CHECK(rig.invoke("popup.delta", 39) == DispatchResult::Unchanged);  // a whole ring is no change
  rig.tap(Button::Down);                      // Down is +1, as in every popup list
  CHECK(rig.state().edit.entry.scale == 1);
  rig.tap(Button::Up);
  CHECK(rig.state().edit.entry.scale == 0);
  rig.tap(Button::Up);                        // Up is -1: wraps to 38
  CHECK(rig.state().edit.entry.scale == 38);
  // Shift + Up / Down stay pages: the panel has no list, so they do nothing.
  rig.shift_tap(Button::Up);
  CHECK(rig.state().edit.entry.scale == 38);
  // The encoder reaches one of the 27 (Hirajoshi, the official 26); with SCALE LOCK on the NOTES board follows it.
  rig.turn(-38);
  CHECK(rig.state().edit.entry.scale == 0);
  rig.pad(47);  // row 4, the last pad: SCALE LOCK on
  CHECK(rig.state().edit.entry.lock == 1);
  rig.turn(26);
  CHECK(rig.state().edit.entry.scale == 26);
  CHECK(rig.state().edit.pads.scale() == 27);  // the core index (the official 26 + 1)
  // A pad of row 2 after the encoder: the same twelve, through the model's map (pad 2 is Major, the official 2).
  rig.pad(13);
  CHECK(rig.state().edit.entry.scale == 2);
  CHECK(rig.state().edit.pads.scale() == 3);
  // Note + Left / Right still move a track while the popup is open.
  const int track = rig.state().pattern.track();
  rig.tap(Button::Right);
  CHECK(rig.state().pattern.track() == track + 1);
  rig.tap(Button::Left);
  CHECK(rig.state().pattern.track() == track);
  let_go(rig, Button::Note);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.state().edit.entry.scale == 2);  // the choice stays
}

TEST_CASE("step popups: Shift + encoder does nothing in the Note panel, whatever the scale, and the plain encoder still walks it (QA-PAT-430, OS-SCALES28 S2)") {
  using app::OverlayId;
  Rig rig;
  rig.edit_mode();
  rig.go(2);
  hold_open(rig, Button::Note);
  CHECK(rig.state().popup.kind == app::StepPopupView::Note);
  rig.pad(12);  // Chromatic first (a new project starts in Major, OS-DEFSCALE): the counts below start from it
  rig.turn(5);  // Pent. min: a scale that is neither the first nor the last
  const app::NoteEntry before = rig.state().edit.entry;
  CHECK(before.scale == 5);
  const int row = rig.view().row();
  const int track = rig.state().pattern.track();
  const int jump = rig.state().edit.edit_step;
  for (const int detents : {1, -1, 3, -7, 39, -39, 100}) {
    rig.shift_turn(detents);
    CHECK_MESSAGE(rig.state().edit.entry.scale == before.scale, "Shift + encoder " << detents << " moved the scale");
    CHECK(rig.state().edit.entry.root == before.root);
    CHECK(rig.state().edit.entry.octave == before.octave);
    CHECK(rig.state().edit.entry.lock == before.lock);
    CHECK(rig.state().edit.edit_step == jump);
    CHECK_MESSAGE(rig.view().row() == row, "Shift + encoder " << detents << " moved the cursor (a page, as in Browse)");
    CHECK(rig.state().pattern.track() == track);
    CHECK(rig.step(track, row).note == model::kNoteEmpty);  // and wrote no note
    CHECK(rig.depth() == 0u);
    CHECK(rig.state().overlay == OverlayId::StepPopup);  // the panel is still up
    CHECK(rig.state().popup.kind == app::StepPopupView::Note);
  }
  // The same at the ring's ends (a coarse step must not wrap them either), and the plain encoder is unharmed afterwards.
  rig.turn(-5);
  CHECK(rig.state().edit.entry.scale == 0);
  rig.shift_turn(-1);
  CHECK(rig.state().edit.entry.scale == 0);
  rig.turn(-1);
  CHECK(rig.state().edit.entry.scale == 38);
  rig.shift_turn(1);
  CHECK(rig.state().edit.entry.scale == 38);
  rig.turn(1);
  CHECK(rig.state().edit.entry.scale == 0);
  let_go(rig, Button::Note);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.step(track, row).note == model::kNoteEmpty);
  // The control: with the panel closed the same turn is the Note column's octave step (OS-SHIFTENC), so the check above could see an effect.
  rig.shift_turn(1);
  CHECK(rig.step(track, row).note != model::kNoteEmpty);
}

TEST_CASE("step popups: on the instrument parameters page a held Instr lists the instruments and the choice is the instrument edited") {
  using app::OverlayId;
  Rig rig;
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Instrument));
  hold_open(rig, Button::Instr);
  CHECK(rig.state().overlay == OverlayId::StepPopup);
  rig.pad(6);
  CHECK(rig.state().instrument == 7);
  let_go(rig, Button::Instr);
  CHECK(rig.state().overlay == OverlayId::None);
  CHECK(rig.state().instrument == 7);
  CHECK(rig.depth() == 0);
  // Note and Fx are not popups of this page.
  hold_open(rig, Button::Note);
  CHECK(rig.state().overlay == OverlayId::None);
  let_go(rig, Button::Note);
}

// ---- OS-CFGWIRE: the soft keys that did nothing -------------------------------------------------------------------------------------------------------------------------------

namespace {

struct HoldSink final : app::PreviewSink {
  void preview(unsigned, unsigned) noexcept override {}
  void hold_note(bool on, unsigned track, unsigned instrument, unsigned note) noexcept override {
    log.push_back({on, track, instrument, note});
  }
  struct Call {
    bool on;
    unsigned track, instrument, note;
  };
  std::vector<Call> log;
};

model::Step note_at(std::uint8_t note, std::uint8_t instr) {
  model::Step s = model::kEmptyStep;
  s.note = note;
  s.instr = instr;
  return s;
}

void put(Rig& rig, int track, int row, const model::Step& s) {
  (void)rig.project().set_steps(1, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(row), &s, 1);
}

}  // namespace

TEST_CASE("OS-CFGWIRE F5 PREVIEW: the step under the cursor sounds on its track while the key is down, with its own instrument, and ends with the key") {
  Rig rig;
  HoldSink sink;
  rig.app->attach_preview(&sink);
  put(rig, 0, 0, note_at(61, 5));  // C-5 (MIDI 60) on instrument 5
  put(rig, 0, 1, note_at(40, 0));  // an instrument-less step takes the last used one
  rig.press(Button::F5);
  rig.pass(1);
  NEED(sink.log.size() == 1);
  CHECK((sink.log[0].on && sink.log[0].track == 0 && sink.log[0].instrument == 5 && sink.log[0].note == 60));
  CHECK(rig.state().step_preview);
  rig.release(Button::F5);
  rig.pass(1);
  NEED(sink.log.size() == 2);
  CHECK(!sink.log[1].on);
  CHECK(!rig.state().step_preview);
  CHECK(rig.depth() == 0);  // no edit

  (void)rig.invoke("nav.down");  // row 1, no instrument: the last used one (1 at the start)
  rig.press(Button::F5);
  rig.pass(1);
  NEED(sink.log.size() == 3);
  CHECK((sink.log[2].on && sink.log[2].instrument == static_cast<unsigned>(rig.state().instrument) && sink.log[2].note == 39));
  rig.release(Button::F5);
  rig.pass(1);

  // A step with no note says so and sounds nothing.
  (void)rig.invoke("nav.down");
  const std::size_t before = sink.log.size();
  rig.tap(Button::F5);
  CHECK(sink.log.size() == before);
  CHECK(rig.message() == "PREVIEW: NO NOTE ON THIS STEP");
  rig.flush();

  // Leaving the screen under a held key ends the note.
  (void)rig.invoke("cursor.jump_fraction", 0);
  rig.press(Button::F5);
  rig.pass(1);
  CHECK(rig.state().step_preview);
  (void)rig.invoke("nav.screen", static_cast<ActionArg>(app::ScreenId::Song));
  rig.pass(1);
  CHECK(!rig.state().step_preview);
  CHECK(!sink.log.back().on);
}

TEST_CASE("OS-CFGWIRE F5 PREVIEW: with no audio path it says so") {
  Rig rig;
  put(rig, 0, 0, note_at(61, 5));
  rig.tap(Button::F5);
  CHECK(rig.message() == "PREVIEW: NO AUDIO OUTPUT");
}

TEST_CASE("OS-CFGWIRE INVERT (no key since OS-PTOOLS2 S3: F6 is REPEAT): a block's steps in the reverse order, track by track, whole steps, one undo unit; one step says so; Browse does nothing") {
  Rig rig;
  for (int r = 0; r < 4; ++r) {
    put(rig, 0, r, note_at(static_cast<std::uint8_t>(61 + r), static_cast<std::uint8_t>(1 + r)));
    put(rig, 1, r, note_at(static_cast<std::uint8_t>(70 + r), 9));
  }
  // Browse: nothing is edited and the strip says what the key needs (OS-QAFIX-12).
  (void)rig.invoke("pattern.invert");
  CHECK(rig.message() == "TURN EDIT ON");
  rig.flush();
  CHECK(rig.depth() == 0);
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.step(0, 3).note == 64);
  rig.edit_mode();
  (void)rig.invoke("pattern.invert");
  CHECK(rig.message() == "SELECT A BLOCK TO INVERT");
  rig.flush();
  CHECK(rig.depth() == 0);
  // The block: rows 0..3 of tracks 0 and 1.
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  rig.extend_tracks(1);
  (void)rig.invoke("pattern.invert");
  for (int r = 0; r < 4; ++r) {
    CHECK_MESSAGE(rig.step(0, r).note == 64 - r, "track 0 row " << r);
    CHECK_MESSAGE(rig.step(0, r).instr == 4 - r, "track 0 row " << r);  // whole steps: the instrument goes with its note
    CHECK_MESSAGE(rig.step(1, r).note == 73 - r, "track 1 row " << r);
  }
  CHECK(rig.step(0, 4).note == model::kNoteEmpty);  // outside the block: untouched
  CHECK(rig.depth() == 1);
  (void)rig.invoke("pattern.invert");  // again: back
  for (int r = 0; r < 4; ++r) {
    CHECK(rig.step(0, r).note == 61 + r);
  }
  CHECK(rig.depth() == 2);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 64);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.depth() == 0);
}

TEST_CASE("OS-CFGWIRE FILL (pattern.fill, no key since OS-QJUMP): the current note on every Nth step of the block or the track, N the edit step, one undo unit") {
  Rig rig;
  put(rig, 0, 0, note_at(61, 7));  // the cursor step: C-5 on instrument 7 is the current note
  rig.edit_mode();
  // F3 chooser, the pad sets the JUMP: 4.
  (void)rig.invoke("chooser.toggle", 2);
  (void)rig.invoke("chooser.pad", 4);
  (void)rig.invoke("chooser.toggle", 2);
  NEED(rig.edit().edit_step == 4);
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the selection is the target; Shift + Up at row 1 selects the track
  (void)rig.invoke("pattern.fill");  // (Shift + Fx was its key until OS-QJUMP: it is the 3/4 jump now)
  CHECK(rig.message() == "FILL: EVERY 4 STEPS");
  for (int r = 0; r < 64; ++r) {
    CHECK_MESSAGE((rig.step(0, r).note == 61) == (r % 4 == 0), "row " << r);
  }
  CHECK(rig.step(0, 4).instr == static_cast<std::uint8_t>(rig.state().instrument));  // an empty step takes the last used instrument
  CHECK(rig.step(0, 0).instr == 7);                                                    // one that had its own keeps it
  CHECK(rig.step(1, 0).note == model::kNoteEmpty);                                     // other tracks are not touched
  CHECK(rig.depth() == 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 4).note == model::kNoteEmpty);
  CHECK(rig.step(0, 0).note == 61);
  rig.flush();

  // A block: from its first row, every 4th, inside it only. The cursor step is empty now, so the last note played is the note: none yet, the default C-5 (60).
  (void)rig.invoke("select.clear");
  (void)rig.invoke("nav.scroll", 2);
  (void)rig.invoke("select.extend", 1);
  for (int i = 0; i < 6; ++i) {
    (void)rig.invoke("select.extend", 1);
  }  // rows 2..9
  (void)rig.invoke("pattern.fill");
  for (int r = 0; r < 12; ++r) {
    const bool want = r == 2 || r == 6;
    CHECK_MESSAGE((rig.step(0, r).note != model::kNoteEmpty) == (want || r == 0), "row " << r);
  }
  CHECK(rig.step(0, 2).note == 61);  // kDefaultNote 60, as a step byte
  // Nothing selected: the cursor step alone is the target (OS-PTOOLS2 S3c).
  (void)rig.invoke("select.clear");
  (void)rig.invoke("pattern.fill");
  CHECK(rig.step(0, 9).note == 61);   // the cursor's step (row 9 after the extends)
  CHECK(rig.step(0, 10).note == model::kNoteEmpty);
  CHECK(rig.step(0, 5).note == model::kNoteEmpty);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  // Browse: nothing is edited, and it says what it needs (and where EXPORT, the key's other half, will come from).
  rig.flush();
  rig.mode(PatternMode::Browse);
  const unsigned depth = rig.depth();
  (void)rig.invoke("pattern.fill");
  CHECK(rig.message() == "FILL: TURN EDIT ON (EXPORT: F7 APPS >)");
  CHECK(rig.depth() == depth);
}

TEST_CASE("OS-CFGWIRE FILL with the edit step 0 fills every step; the soft key F7 says nothing, the EXPORT action says where it will come from") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(50, 2));
  (void)rig.invoke("chooser.toggle", 2);
  (void)rig.invoke("chooser.pad", 0);
  (void)rig.invoke("chooser.toggle", 2);
  NEED(rig.edit().edit_step == 0);
  (void)rig.invoke("select.column_all");  // the whole track is the selection's target
  (void)rig.invoke("pattern.fill");
  for (int r = 0; r < 16; ++r) {
    CHECK_MESSAGE(rig.step(0, r).note == 50, "row " << r);
  }
  CHECK(rig.message() == "FILL: EVERY 1 STEP");
  rig.flush();
  CHECK(rig.message().empty());
  rig.tap(Button::F4);
  CHECK(rig.state().overlay == app::OverlayId::Fill);  // OS-PTOOLS: F4 is the Fill dialog now
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.tap(Button::F7);
  rig.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);  // OS-H3A: F7 is TOOLS now; with no module system it lists EXPORT PATTERN (OS-WAVEXP)
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.flush();
  (void)rig.invoke("pattern.export");  // no key: a keymap or a scenario names it
  CHECK(rig.message() == "EXPORT: IN F7 TOOLS");
  rig.flush();
  (void)rig.invoke("ui.softkey", 1);  // a label with a function still says it
  CHECK(rig.message() == "F2 LENGTH");
}

// ---- OS-PTOOLS part 1: the Fill dialog ----------------------------------------------------------------------------------------------------

namespace {

// Moves the dialog's cursor to `row` (FillRow) from the top and changes it by `delta` places.
void fill_row(Rig& rig, int row, int delta) {
  for (unsigned i = 0; i < app::kFillRows; ++i) {
    rig.tap(Button::Up);
  }
  for (int i = 0; i < row; ++i) {
    rig.tap(Button::Down);
  }
  for (int i = 0; i < (delta < 0 ? -delta : delta); ++i) {
    rig.tap(delta < 0 ? Button::Left : Button::Right);
  }
}

// The rows by name (app::FillRow); the dialog has seven (OS-FILL2; no SCOPE since OS-PTOOLS2 S3c). Row 2 is N, EVENTS or DENSITY by TYPE.
constexpr int kRowWhat = static_cast<int>(app::FillRow::What);
constexpr int kRowType = static_cast<int>(app::FillRow::Type);
constexpr int kRowCount = static_cast<int>(app::FillRow::Count);
constexpr int kRowMode = static_cast<int>(app::FillRow::Mode);
constexpr int kRowKey = static_cast<int>(app::FillRow::Key);
constexpr int kRowFrom = static_cast<int>(app::FillRow::From);
constexpr int kRowTo = static_cast<int>(app::FillRow::To);
static_assert(app::kFillRows == 7, "the dialog has seven rows (no SCOPE since OS-PTOOLS2 S3c)");

std::string column_of(Rig& rig, int track, int rows) {
  std::string s;
  for (int r = 0; r < rows; ++r) {
    s += rig.step(track, r).note != model::kNoteEmpty ? 'x' : '.';
  }
  return s;
}

}  // namespace

TEST_CASE("OS-PTOOLS Fill dialog: F4 opens it in Edit only, F8 closes it and changes nothing, the key row is the dialog's") {
  Rig rig;
  rig.tap(Button::F4);  // Browse
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "FILL: TURN EDIT ON");
  rig.flush();
  rig.edit_mode();
  const unsigned depth = rig.depth();
  rig.tap(Button::F4);
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  CHECK(rig.state().overlay_keys());
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.depth() == depth);
  CHECK(column_of(rig, 0, 64) == std::string(64, '.'));
  // The arrows choose a row (clamped), the encoder and Left / Right change it: TYPE rolls over EVERY N, RANDOM, EUCLID, SCALE.
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 0);
  CHECK(rig.state().fill.row == kRowType);
  rig.tap(Button::Left);  // rolls over to the last
  CHECK(rig.state().fill.plan.type == model::fill::Type::Scale);
  rig.turn(1);
  CHECK(rig.state().fill.plan.type == model::fill::Type::Every);
  // The overlay captures: a pad does nothing under it.
  rig.pad(kPadC3);
  CHECK(column_of(rig, 0, 64) == std::string(64, '.'));
  rig.tap(Button::F8);
}

TEST_CASE("OS-PTOOLS Fill dialog: Euclid 3 events over the selected track is one undo unit, the track's steps only, the first step a hit") {
  Rig rig(true, 16);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the selection is the target; this is the track of Shift + Up at row 1
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 2);   // EUCLID
  fill_row(rig, kRowCount, -1);  // EVENTS 4 -> 3
  CHECK(rig.state().fill.plan.events == 3);
  const unsigned depth = rig.depth();
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(column_of(rig, 0, 16) == "x.....x....x....");  // (i * 3) mod 16 < 3: steps 0, 6 and 11
  CHECK(column_of(rig, 1, 16) == std::string(16, '.'));  // the track is the target: the other tracks are not touched
  CHECK(rig.message() == "FILL: EUCLID 3 EVENTS");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(column_of(rig, 0, 16) == std::string(16, '.'));
  CHECK(rig.depth() == depth);
}

TEST_CASE("OS-PTOOLS2 S3c: the target of the fill is the selection — the block, the track of Shift+Up at row 1, the pattern of it again, the cursor step with none; the footer names it") {
  Rig rig(true, 16);
  rig.edit_mode();
  const app::PatternSource src(rig.app->model(), rig.edit().pattern);
  const auto footer = [&] {
    char buf[48];
    (void)app::fill_target_text(rig.state().edit.selection, rig.view().row(), rig.view().track(), src.length(), src.tracks(), buf, sizeof buf);
    return std::string(buf);
  };
  // A block: rows 3..10 of track 1 (nav.scroll 2, then Shift + Down x7), every 2nd step.
  (void)rig.invoke("nav.scroll", 2);
  for (int i = 0; i < 7; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  rig.tap(Button::F4);
  CHECK(footer() == "BLOCK  TRACK 1  ROWS 3-10");  // the dialog's footer names the block
  fill_row(rig, kRowCount, 1);                     // N = 2
  rig.tap(Button::F1);
  CHECK(column_of(rig, 0, 16) == "..x.x.x.x.......");
  CHECK(column_of(rig, 1, 16) == std::string(16, '.'));
  // The track: Shift + Up at row 1 (the whole column), the whole track 1.
  (void)rig.invoke("select.clear");
  (void)rig.invoke("select.column_all");
  rig.tap(Button::F4);
  CHECK(footer() == "TRACK 1  ROWS 1-16");  // 16 steps in this pattern
  rig.tap(Button::F8);
  // The pattern: the column selection over every track.
  (void)rig.invoke("select.column_all");
  rig.tap(Button::F4);
  CHECK(footer() == "PATTERN  ALL TRACKS  ROWS 1-16");
  fill_row(rig, kRowCount, 1);  // N = 3
  rig.tap(Button::F1);
  for (int t = 0; t < 12; ++t) {
    CHECK_MESSAGE(column_of(rig, t, 16) == (t == 0 ? "x.xxx.x.xx..x..x" : "x..x..x..x..x..x"), "track " << t);
  }
  CHECK(rig.step(0, 16).note == model::kNoteEmpty);  // past the pattern's length (the model's rows beyond it are never written)
  // Nothing selected: the cursor step alone.
  (void)rig.invoke("select.clear");
  rig.tap(Button::F4);
  CHECK(footer() == "STEP  TRACK 1  ROW " + std::to_string(rig.view().row() + 1));
  rig.tap(Button::F8);
}

TEST_CASE("OS-PTOOLS Fill dialog: WHAT = INSTR writes the instrument in hand, RANDOM density 100 chooses every step, 0 none and says so") {
  Rig rig(true, 16);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the track selected, so the fill is the track's
  (void)rig.invoke("instrument.select", 7);
  rig.tap(Button::F4);
  fill_row(rig, kRowWhat, 1);  // INSTR
  fill_row(rig, kRowCount, 3);  // N = 4
  rig.tap(Button::F1);
  for (int r = 0; r < 16; ++r) {
    CHECK_MESSAGE((rig.step(0, r).instr == 7) == (r % 4 == 0), "row " << r);
  }
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  rig.flush();
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 1);  // RANDOM
  fill_row(rig, kRowCount, -30);  // DENSITY 30 -> 0
  CHECK(rig.state().fill.plan.density == 0);
  const unsigned depth = rig.depth();
  rig.tap(Button::F1);
  CHECK(rig.message() == "FILL: NOTHING TO CHANGE");
  CHECK(rig.depth() == depth);  // no unit for a fill that wrote nothing
  rig.flush();
  rig.tap(Button::F4);
  fill_row(rig, kRowCount, 100);  // DENSITY 100
  CHECK(rig.state().fill.plan.density == 100);
  rig.tap(Button::F1);
  for (int r = 0; r < 16; ++r) {
    CHECK_MESSAGE(rig.step(0, r).instr == 7, "row " << r);
  }
}

TEST_CASE("OS-PTOOLS Fill dialog: pattern.fill (no key since OS-QJUMP) repeats the last fill of the dialog on the selection as it is now, the same rows and the same result") {
  Rig rig(true, 16);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // the track is the target
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 2);   // EUCLID, 4 events
  rig.tap(Button::F1);
  CHECK(rig.state().fill.valid);
  CHECK(column_of(rig, 0, 16) == "x...x...x...x...");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(column_of(rig, 0, 16) == std::string(16, '.'));
  rig.flush();
  (void)rig.invoke("pattern.fill");
  CHECK(column_of(rig, 0, 16) == "x...x...x...x...");  // the same fill again
  CHECK(rig.message() == "FILL: EUCLID 4 EVENTS");
}

// OS-F6EVENTS: Pattern F6 is EVENTS; the repeat of the last verb is F6 of the verb menu (F7), RE FILL or REPEAT by the last verb.
static void verb_menu_repeat(Rig& rig) {
  rig.tap(Button::F7);
  rig.tap(Button::F6);
}

// The label the verb menu draws on F6, read with the menu open; the menu is closed again (F8 BACK).
static std::string verb_menu_f6_label(Rig& rig) {
  rig.tap(Button::F7);
  const std::string label = test::read_owner_row(rig.app->model(), rig.state()).label[5];
  rig.tap(Button::F8);
  rig.flush();
  return label;
}

TEST_CASE("OS-F6EVENTS Pattern F6 opens EVENTS: the key row reads EVENTS, the verb menu has RE FILL in its place") {
  CHECK(std::strcmp(app::kPatternSoftKeys[5], "EVENTS") == 0);
  Rig rig(true, 16);
  rig.tap(Button::F6);
  CHECK(rig.state().overlay == app::OverlayId::Events);
  rig.tap(Button::F8);  // BACK
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(verb_menu_f6_label(rig) == "RE FILL");
}

TEST_CASE("OS-PTOOLS2 S3 / OS-F6EVENTS F7 then F6 REPEAT (Q1): the verb menu key is tools.repeat and reads RE FILL") {
  {
    Rig fresh(true, 16);
    CHECK(verb_menu_f6_label(fresh) == "RE FILL");  // the label is RE <VERB> (the 8-char key cap over "AGAIN FILL"); FILL until a TOOLS apply
  }
  Rig rig(true, 16);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the track selected, so the fill is the track's
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 2);   // EUCLID, 4 events
  rig.tap(Button::F1);
  CHECK(column_of(rig, 0, 16) == "x...x...x...x...");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  rig.flush();
  verb_menu_repeat(rig);  // REPEAT: the last verb (FILL) again, the same rows and notice as the action
  CHECK(column_of(rig, 0, 16) == "x...x...x...x...");
  CHECK(rig.message() == "FILL: EUCLID 4 EVENTS");
  CHECK(rig.depth() == 1);
}

TEST_CASE("OS-PTOOLS2 S3 tools.repeat under a modal overlay: the Fill dialog open, it refuses and writes nothing") {
  Rig rig(true, 16);
  rig.edit_mode();
  rig.tap(Button::F4);  // the Fill dialog is up (a modal overlay)
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  const unsigned depth = rig.depth();
  CHECK(rig.invoke("tools.repeat") == DispatchResult::Disabled);  // under the modal overlay the action gets no focus; a direct call returns false (the handler checks the overlay first)
  CHECK(rig.depth() == depth);
  CHECK(column_of(rig, 0, 16) == std::string(16, '.'));  // nothing written
}

TEST_CASE("OS-PTOOLS2 S3 tools.repeat with no verb run yet: NOTHING TO REPEAT, nothing written (the OS-CFGWIRE fill stays pattern.fill's own)") {
  Rig rig(true, 16);
  rig.edit_mode();
  const unsigned depth = rig.depth();
  verb_menu_repeat(rig);
  CHECK(rig.message() == "NOTHING TO REPEAT");
  CHECK(rig.depth() == depth);
  CHECK(column_of(rig, 0, 16) == std::string(16, '.'));
  CHECK(rig.invoke("pattern.fill") == DispatchResult::Changed);  // the keyless pattern.fill still does the OS-CFGWIRE fill (every JUMPth step)
  CHECK(column_of(rig, 0, 16) != std::string(16, '.'));
}

TEST_CASE("OS-PTOOLS2 S3 F6 with a random fill as the last verb: the repeat draws again (the rng advanced), one undo unit") {
  Rig rig(true, 16);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // the track is the target
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 1);   // RANDOM
  fill_row(rig, kRowCount, 20); // DENSITY 30 -> 50
  CHECK(rig.state().fill.plan.density == 50);
  rig.tap(Button::F1);
  const std::string first = column_of(rig, 0, 16);
  CHECK(first != std::string(16, '.'));
  CHECK(rig.message() == "FILL: RANDOM 50 %");
  const std::uint32_t rng_before = rig.state().fill.rng.s;
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(column_of(rig, 0, 16) == std::string(16, '.'));
  rig.flush();
  verb_menu_repeat(rig);
  CHECK(rig.state().fill.rng.s != rng_before);  // a new draw
  CHECK(column_of(rig, 0, 16) != first);      // and a different one
  CHECK(rig.message() == "FILL: RANDOM 50 %");
  CHECK(rig.depth() == 1);
}

TEST_CASE("OS-PTOOLS2 S3c: F6 REPEAT with a block active: the last fill's target was the block, the repeat fills the block only") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);  // one row down; three taps make a block of rows 0..3 on the cursor's track
  }
  rig.tap(Button::F4);  // the dialog's target is the block (no SCOPE row: the selection is it)
  fill_row(rig, kRowType, 2);   // EUCLID, 4 events
  rig.tap(Button::F1);
  CHECK(column_of(rig, 0, 16) == "xxxx" + std::string(12, '.'));
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  rig.flush();
  verb_menu_repeat(rig);
  CHECK(column_of(rig, 0, 16) == "xxxx" + std::string(12, '.'));  // the block again, the rest of the track untouched
  CHECK(rig.message() == "FILL: WHOLE STEPS, EUCLID 4 EVENTS");  // a column block fills as whole steps (OS-AUDIT-56)
}

TEST_CASE("OS-PTOOLS2 S3c (review): with the Fill dialog open during playback the target does not follow the playhead — F1 fills the block, or the opening step with no selection") {
  Rig rig(true, 16);
  rig.edit_mode();
  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  pos.publish(true, 1, 4, 12000, audio::PlayPosition::kNoOrder);  // pattern 1 plays, step 4
  rig.pass(1);
  NEED(rig.state().play.playing);
  CHECK(rig.view().row() == 4);
  // A block made while following (Shift + Down x3): it stays (OS-BUG-WATCHSEL), and the dialog's target is it.
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);
  }  // rows 4..7
  NEED(rig.state().edit.selection.active);
  rig.tap(Button::F4);
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  // The playhead steps on: under the dialog the cursor must not move, so the target stays the block.
  pos.publish(true, 1, 12, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  CHECK(rig.view().row() == 4);  // the playhead's row when the dialog opened, not the new step 12
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  for (int r = 0; r < 16; ++r) {
    CHECK_MESSAGE((rig.step(0, r).note != model::kNoteEmpty) == (r >= 4 && r <= 7), "row " << r);
  }
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  rig.flush();
  // No selection: the target is the step the cursor was on when the dialog opened.
  (void)rig.invoke("select.clear");
  pos.publish(true, 1, 2, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  CHECK(rig.view().row() == 2);
  rig.tap(Button::F4);
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  pos.publish(true, 1, 10, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  CHECK(rig.view().row() == 2);  // not the playhead's 10
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 2).note != model::kNoteEmpty);
  for (int r = 0; r < 16; ++r) {
    if (r != 2) {
      CHECK_MESSAGE(rig.step(0, r).note == model::kNoteEmpty, "row " << r);
    }
  }
  rig.app->attach_document(nullptr);  // (a take is open while it plays in Edit: the rig destroys its editor before the app)
}

// ---- OS-PTOOLS2 S3: the F7 verb menu (verb_menu.h; the TOOLS list stays the APPS > row) -------------------------------------------------

TEST_CASE("OS-PTOOLS2 S3 F7 verb menu: opens over Pattern, the arrows move in the 2x8 grid, F8 BACK closes, the last verb is remembered") {
  Rig rig(true, 16);
  rig.edit_mode();
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  CHECK(rig.state().verb_menu.cursor == 0);
  rig.tap(Button::Down);   // one row down
  rig.tap(Button::Right);  // one column right: pad 10
  CHECK(rig.state().verb_menu.cursor == 9);
  rig.tap(Button::F8);     // BACK closes
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.tap(Button::F7);     // again: the menu remembers its last verb
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  CHECK(rig.state().verb_menu.cursor == 9);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
}

TEST_CASE("OS-PTOOLS2 S3p: the verb menu and the TOOLS dialog open in the right-hand panel, and the stored track window is not moved (the draw narrows and scrolls the grid beside the panel)") {
  Rig rig(true, 64);
  rig.edit_mode();
  rig.go(0, 4);          // the cursor on track 5
  rig.extend_tracks(2);  // a block over tracks 5..7, the cursor on the last
  CHECK(rig.view().track() == 6);
  CHECK(rig.view().left_track() == 2);  // follow() kept the cursor of the extended block in the 5-track window
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  CHECK(rig.view().left_track() == 2);  // the stored window stays (BACK leaves the grid where it was)
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.tap(Button::F7);
  rig.pad(0);  // pad 1 = TRANSPOSE opens the TOOLS dialog
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  CHECK(rig.view().left_track() == 2);  // the same for the dialog
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
}

TEST_CASE("OS-PTOOLS2 S3 verb menu, pad 9 (INVERT): the pick runs pattern.invert on the block as ONE undo unit") {
  Rig rig;
  for (int r = 0; r < 4; ++r) {
    put(rig, 0, r, note_at(static_cast<std::uint8_t>(61 + r), 1));
  }
  rig.edit_mode();
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);  // the block: rows 0..3 of the cursor's track
  }
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.pad(8);  // pad 9 = INVERT
  CHECK(rig.state().overlay == app::OverlayId::None);  // the menu closed for the verb
  for (int r = 0; r < 4; ++r) {
    CHECK_MESSAGE(rig.step(0, r).note == 64 - r, "row " << r);
  }
  CHECK(rig.depth() == depth + 1);  // one undo unit
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 61);
}

TEST_CASE("OS-PTOOLS2 S3b verb menu, pad 4 (INTERPOLATE): the pick runs the note ramp over the block as ONE undo unit") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 5));
  put(rig, 0, 8, note_at(69, 7));
  for (int i = 0; i < 8; ++i) {
    (void)rig.invoke("select.extend", 1);  // the block: rows 0..8 of the Note column
  }
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.pad(3);  // pad 4 = INTERPOLATE
  CHECK(rig.state().overlay == app::OverlayId::None);  // the menu closed for the verb
  for (int r = 1; r <= 7; ++r) {
    CHECK_MESSAGE(rig.step(0, r).note == 61 + r, "row " << r);  // the chromatic ramp 61..69
    CHECK(rig.step(0, r).instr == 5);                           // an empty row takes the first anchor's instrument
  }
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.step(0, 8).note == 69);
  CHECK(rig.message() == "INTERPOLATED: 7 STEPS");
  CHECK(rig.depth() == depth + 1);         // one undo unit
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 1).note == model::kNoteEmpty);
  CHECK(rig.step(0, 7).note == model::kNoteEmpty);
}

TEST_CASE("OS-PTOOLS2 S3 verb menu, pad 1 (TRANSPOSE) on a Vol-column block: refused when the dialog would open, with a message, nothing written (the encoder fired only on the Note column)") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  rig.column(Column::Vol);  // the cursor on the Vol column
  (void)rig.invoke("select.extend", 1);  // a block of rows 0..1 on the Vol column only (not whole steps)
  NEED(!rig.state().edit.selection.block(rig.view().row(), rig.view().track()).whole_steps());
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(0);  // pad 1 = TRANSPOSE: it would open the TOOLS dialog (OS-PTOOLS2 S2), but not on this column block
  CHECK(rig.state().overlay == app::OverlayId::None);  // refused at open, not at APPLY after the rows were set
  CHECK(rig.message() == "TRANSPOSE: WHOLE STEPS OR THE NOTE COLUMN");
  CHECK(rig.step(0, 0).note == 61);  // the note outside the selected columns is untouched
  CHECK(rig.depth() == depth);       // no undo unit
}

TEST_CASE("OS-PTOOLS2 S3 verb menu, pad 1 (TRANSPOSE) on a single empty step in the Note column: the dialog's APPLY takes the multi-cell rule, nothing is written (no note inserted)") {
  Rig rig(true, 16);
  rig.edit_mode();  // the cursor on the Note column, row 0 of track 0, an empty step, no selection
  CHECK(rig.step(0, 0).note == model::kNoteEmpty);
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(0);  // pad 1 = TRANSPOSE: the dialog
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  rig.tap(Button::Down);
  rig.turn(1);  // AMOUNT +1
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).note == model::kNoteEmpty);  // the single empty step took the multi-cell rule: no default note, no instrument
  CHECK(rig.step(0, 0).instr == 0);
  CHECK(rig.depth() == depth);  // nothing written, no unit
  CHECK(rig.message() == "NOTHING TO CHANGE");
  // and a single step that holds a note still transposes through the dialog, one unit
  put(rig, 0, 0, note_at(61, 3));
  rig.tap(Button::F7);
  rig.pad(0);
  rig.tap(Button::Down);
  rig.turn(1);  // AMOUNT +2 now (the rows are session state)
  rig.tap(Button::F1);
  CHECK(rig.step(0, 0).note == 63);
  CHECK(rig.depth() == depth + 1);
}

TEST_CASE("OS-PTOOLS2 S3 verb menu refused while another modal is open: with the Fill dialog up, verb.open does not open the menu") {
  Rig rig(true, 16);
  rig.edit_mode();
  rig.tap(Button::F4);
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  CHECK(rig.invoke("verb.open") == DispatchResult::Disabled);  // under the modal overlay the action gets no focus
  CHECK(rig.state().overlay == app::OverlayId::Fill);           // still the Fill dialog, no menu over it
  rig.tap(Button::F8);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
}

TEST_CASE("OS-PTOOLS2 S3 verb menu APPS > (pad 16): opens the old TOOLS list, F8 closes both levels") {
  Rig rig(true, 16);
  rig.edit_mode();
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.pad(15);  // pad 16 = APPS >
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);  // the verb menu's APPS page (OS-PTOOLS2 S6b), the list OS-H3A's F7 opened
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
}

// ---- OS-PTOOLS2 S3b: the five S4 verbs wired as block verbs (model/pattern_verbs.h), one undo unit each, F6 REPEAT runs the last one ----------

TEST_CASE("OS-PTOOLS2 S3b verb menu, pads 7/8 (ROTATE UP / DOWN): the block's steps move one row, wrapping inside the block, one undo unit") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int r = 0; r < 4; ++r) {
    put(rig, 0, r, note_at(static_cast<std::uint8_t>(61 + r), static_cast<std::uint8_t>(1 + r)));
  }
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);  // rows 0..3, the Note column (note + instrument move)
  }
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(6);  // pad 7 = ROTATE UP: row i takes row i+1, the first row goes to the end
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).note == 62);
  CHECK(rig.step(0, 1).note == 63);
  CHECK(rig.step(0, 2).note == 64);
  CHECK(rig.step(0, 3).note == 61);
  CHECK(rig.step(0, 3).instr == 1);
  CHECK(rig.message() == "ROTATED: 4 STEPS");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 61);
  // ROTATE DOWN (pad 8) is the inverse
  rig.tap(Button::F7);
  rig.pad(7);
  CHECK(rig.step(0, 0).note == 64);
  CHECK(rig.step(0, 3).note == 63);
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 61);
}

TEST_CASE("OS-PTOOLS2 S3b verb menu, pad 5 (RANDOMISE): in place on the block's column with the fixed default amount, one undo unit; F6 does not repeat it") {
  Rig rig(true, 16);
  rig.edit_mode();
  rig.column(Column::Vol);
  for (int r = 0; r < 16; ++r) {
    model::Step s = note_at(61, 1);
    s.vol = static_cast<std::uint8_t>(r * 8);  // 0..120, every step holds a volume
    put(rig, 0, r, s);
  }
  for (int i = 0; i < 15; ++i) {
    (void)rig.invoke("select.extend", 1);  // rows 0..15 of the Vol column
  }
  const unsigned depth = rig.depth();
  const std::uint16_t seed0 = rig.state().verb_menu.seed;
  rig.tap(Button::F7);
  rig.pad(4);  // pad 5 = RANDOMISE
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.state().verb_menu.seed == seed0 + 1);
  unsigned moved = 0;
  for (int r = 0; r < 16; ++r) {
    const unsigned old = static_cast<unsigned>(r * 8);
    const unsigned now = rig.step(0, r).vol;
    CHECK(now + 4 >= old);  // AMOUNT 4, clamped: within +-4 of the old value
    CHECK(now <= old + 4);
    if (now != old) {
      ++moved;
    }
    CHECK(rig.step(0, r).note == 61);  // only the column moves
  }
  CHECK(moved > 0u);
  CHECK(rig.message() == "RANDOMISED: " + std::to_string(moved) + " STEPS");
  CHECK(rig.depth() == depth + 1);  // one undo unit
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 5).vol == 40);
  rig.flush();
  // F6 REPEAT stays the last FILL's: with no fill run, it says NOTHING TO REPEAT, writes nothing and does not even advance the seed
  (void)rig.invoke("tools.repeat");
  CHECK(rig.state().verb_menu.seed == seed0 + 1);
  CHECK(rig.depth() == depth);
  CHECK(rig.message() == "NOTHING TO REPEAT");
}

// ---- OS-PTOOLS2 S2: the Pattern TOOLS dialog (pattern_tools_view.h), opened by the menu's TRANSPOSE / INSTRUMENT rows --------------------------

TEST_CASE("OS-PTOOLS2 S2 the menu's TRANSPOSE row opens the TOOLS dialog: the OP row flips with Left / Right, AMOUNT clamps at +-24 and Shift + encoder is an octave, F8 BACK leaves the project untouched") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  put(rig, 0, 1, note_at(63, 3));
  (void)rig.invoke("select.extend", 1);  // the block: rows 0..1 of the Note column
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.pad(0);  // pad 1 = TRANSPOSE
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  CHECK(rig.state().ptools.tool == app::PTool::Transpose);
  CHECK(rig.state().ptools.row == 0);
  // Left / Right on the OP row rolls over the three OPs
  rig.tap(Button::Right);
  CHECK(rig.state().ptools.tool == app::PTool::Instrument);
  rig.tap(Button::Right);
  CHECK(rig.state().ptools.tool == app::PTool::Clear);
  rig.tap(Button::Right);
  CHECK(rig.state().ptools.tool == app::PTool::Transpose);
  rig.tap(Button::Left);
  CHECK(rig.state().ptools.tool == app::PTool::Clear);
  rig.tap(Button::Left);
  CHECK(rig.state().ptools.tool == app::PTool::Instrument);
  rig.tap(Button::Left);
  CHECK(rig.state().ptools.tool == app::PTool::Transpose);
  // Up / Down to the AMOUNT row; the encoder clamps at the ends, Shift + encoder is the octave (the coarse step 12)
  rig.tap(Button::Down);
  CHECK(rig.state().ptools.row == 1);
  rig.turn(100);
  CHECK(rig.state().ptools.amount == 24);
  rig.turn(-100);
  CHECK(rig.state().ptools.amount == -24);
  rig.shift_turn(1);
  CHECK(rig.state().ptools.amount == -12);
  rig.shift_turn(-2);
  CHECK(rig.state().ptools.amount == -24);
  rig.shift_turn(10);
  CHECK(rig.state().ptools.amount == 24);
  rig.turn(-25);
  CHECK(rig.state().ptools.amount == -1);
  // F8 BACK: the dialog closes, nothing was written, no undo unit
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.depth() == depth);
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.step(0, 1).note == 63);
  // pad 2 (INSTRUMENT) opens on INSTRUMENT, with the TO row at the block's first note's instrument (a Note-column block refuses INSTRUMENT at open: take the whole track)
  (void)rig.invoke("select.clear");
  (void)rig.invoke("select.column_all");
  rig.tap(Button::F7);
  rig.pad(1);
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  CHECK(rig.state().ptools.tool == app::PTool::Instrument);
  CHECK(rig.state().ptools.instrument == 3);
  rig.tap(Button::Down);
  rig.turn(100);
  CHECK(rig.state().ptools.instrument == 64);  // TO clamps at 64
  rig.shift_turn(-8);
  CHECK(rig.state().ptools.instrument == 1);  // the coarse step 8, eight detents back: clamped to 1
  rig.tap(Button::F8);
  // F1 RUN on the menu's row (the cursor remembers the last pick: INSTRUMENT) opens the same dialog
  rig.tap(Button::F7);
  rig.tap(Button::F1);  // RUN
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  CHECK(rig.state().ptools.tool == app::PTool::Instrument);
  rig.tap(Button::F8);
  rig.hold_key(Button::F7);
  rig.pad(0);  // F7 held + pad 1
  rig.release_key(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
}

TEST_CASE("OS-PTOOLS2 S2 F1 APPLY TRANSPOSE: one undo unit over the block, `TRANSPOSED: 5 STEPS +12 ST`, cut / fade / off and empty steps untouched (the multi-cell rule)") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  model::Step cut = model::kEmptyStep;
  cut.note = model::kNoteCut;
  put(rig, 0, 1, cut);
  model::Step fade = model::kEmptyStep;
  fade.note = model::kNoteFade;
  put(rig, 0, 2, fade);
  model::Step off = model::kEmptyStep;
  off.note = model::kNoteOff;
  put(rig, 0, 3, off);
  // row 4 stays empty
  for (int i = 0; i < 4; ++i) {
    (void)rig.invoke("select.extend", 1);  // rows 0..4
  }
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(0);  // TRANSPOSE opens the dialog
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  rig.tap(Button::Down);      // the AMOUNT row
  rig.shift_turn(1);          // +12 ST (an octave)
  CHECK(rig.state().ptools.amount == 12);
  rig.tap(Button::F1);        // APPLY
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).note == 73);  // 61 (og 60) + 12
  CHECK(rig.step(0, 0).instr == 3);
  CHECK(rig.step(0, 1).note == model::kNoteCut);   // untouched
  CHECK(rig.step(0, 2).note == model::kNoteFade);  // untouched
  CHECK(rig.step(0, 3).note == model::kNoteOff);   // untouched
  CHECK(rig.step(0, 4).note == model::kNoteEmpty);  // no note made from nothing
  CHECK(rig.step(0, 4).instr == 0);
  CHECK(rig.message() == "TRANSPOSED: 5 STEPS +12 ST");
  CHECK(rig.depth() == depth + 1);  // one undo unit
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.depth() == depth);
  // AMOUNT 0 says NOTHING TO DO and writes nothing
  rig.flush();
  rig.tap(Button::F7);
  rig.pad(0);
  rig.tap(Button::Down);
  rig.turn(-12);  // back to 0
  CHECK(rig.state().ptools.amount == 0);
  rig.tap(Button::F1);
  CHECK(rig.message() == "NOTHING TO DO");
  CHECK(rig.depth() == depth);
}

TEST_CASE("OS-PTOOLS2 S2 F1 APPLY INSTRUMENT: every note of the block takes the TO instrument, empty steps keep none, one undo unit; CLEAR via the OP row clears WHAT") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  put(rig, 0, 1, note_at(63, 7));
  // row 2 empty
  rig.column(Column::Instr);  // the block's column: INSTRUMENT applies on the Instr column (or whole steps)
  for (int i = 0; i < 2; ++i) {
    (void)rig.invoke("select.extend", 1);  // rows 0..2 on the Instr column
  }
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(1);  // INSTRUMENT
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  CHECK(rig.state().ptools.instrument == 3);  // the block's first note's instrument
  rig.tap(Button::Down);
  rig.turn(2);  // TO 5
  CHECK(rig.state().ptools.instrument == 5);
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).instr == 5);
  CHECK(rig.step(0, 1).instr == 5);
  CHECK(rig.step(0, 2).instr == 0);  // an empty step takes no instrument (the multi-cell rule)
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.message() == "INSTRUMENT SET: 3 STEPS 05");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).instr == 3);
  // CLEAR: on a whole-step block (the whole track), flip the OP row, WHAT defaults to ALL, F1 clears the block's steps
  rig.flush();
  (void)rig.invoke("select.clear");
  (void)rig.invoke("select.column_all");
  (void)rig.invoke("select.column_all");  // (OS-SELCOL: the first press is the cursor's column when it is not Note, the second the whole track)
  NEED(rig.state().edit.selection.block(rig.view().row(), rig.view().track()).whole_steps());
  rig.tap(Button::F7);
  rig.pad(0);  // opens on TRANSPOSE (a whole-step block takes any tool)
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  rig.tap(Button::Right);
  rig.tap(Button::Right);  // OP: INSTRUMENT -> CLEAR
  CHECK(rig.state().ptools.tool == app::PTool::Clear);
  rig.tap(Button::Down);   // the WHAT row (ALL)
  CHECK(rig.state().ptools.what == app::SongWhat::All);
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).note == model::kNoteEmpty);
  CHECK(rig.step(0, 1).note == model::kNoteEmpty);
  CHECK(rig.step(0, 0).instr == 0);
  CHECK(rig.message() == "CLEARED: 16 STEPS");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 61);
}

// OS-MIDIREC-P1 S7a, CLEAR LANE (new Community OS behaviour): TOOLS CLEAR of ALL over the whole cursor track also clears that track's MIDI lane, in the one undo unit; another track's lane stays.
TEST_CASE("OS-MIDIREC-P1 S7a: TOOLS CLEAR of the whole track clears the track's MIDI lane in one undo unit, undo brings steps and lane back") {
  static model::LaneStore store;
  Rig rig(true, 16);
  rig.edit_mode();
  store.clear();
  store.adopt(rig.project().load_epoch());
  rig.editor->attach_lanes(&store);
  put(rig, 0, 0, note_at(61, 3));
  model::LaneEvent ev[2]{};
  ev[0].tick = 10;
  ev[0].kind_channel = static_cast<std::uint8_t>(1u << 4);
  ev[0].a = 1;
  ev[0].b = 50;
  ev[1] = ev[0];
  ev[1].tick = 200;
  NEED(rig.editor->lane_replace(1, 0, 0, 768, ev, 2) == model::Status::Ok);
  NEED(rig.editor->lane_replace(1, 1, 0, 768, ev, 2) == model::Status::Ok);
  rig.flush();
  const unsigned depth = rig.depth();
  (void)rig.invoke("select.clear");
  (void)rig.invoke("select.column_all");
  (void)rig.invoke("select.column_all");
  NEED(rig.state().edit.selection.block(rig.view().row(), rig.view().track()).whole_steps());
  rig.tap(Button::F7);
  rig.pad(0);
  rig.tap(Button::Right);
  rig.tap(Button::Right);  // OP: CLEAR
  NEED(rig.state().ptools.tool == app::PTool::Clear);
  rig.tap(Button::F1);
  CHECK(rig.step(0, 0).note == model::kNoteEmpty);
  CHECK(store.lane(1, 0).count == 0u);
  CHECK(store.lane(1, 1).count == 2u);  // another track's lane stays
  CHECK(std::string(rig.message()).find("+ LANE") != std::string::npos);
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 61);
  CHECK(store.lane(1, 0).count == 2u);
  CHECK(store.lane(1, 1).count == 2u);
}

TEST_CASE("OS-PTOOLS2 S2 the dialog's column rule at open: TRANSPOSE on a Vol-column block and INSTRUMENT on a Note-column block open no dialog, with the message, nothing written") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  rig.column(Column::Vol);
  (void)rig.invoke("select.extend", 1);  // a Vol-column block of rows 0..1
  NEED(!rig.state().edit.selection.block(rig.view().row(), rig.view().track()).whole_steps());
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(0);  // TRANSPOSE
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "TRANSPOSE: WHOLE STEPS OR THE NOTE COLUMN");
  rig.flush();
  rig.tap(Button::F7);
  rig.pad(1);  // INSTRUMENT, the Vol column again
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "INSTRUMENT: WHOLE STEPS OR THE INSTR COLUMN");
  CHECK(rig.step(0, 0).note == 61);  // untouched
  CHECK(rig.step(0, 0).instr == 3);
  CHECK(rig.depth() == depth);
}

TEST_CASE("OS-PTOOLS2 S2 the dialog's column rule at APPLY: a Note-column block opens TRANSPOSE, and flipping the OP row to INSTRUMENT or CLEAR is refused at APPLY, nothing written") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  put(rig, 0, 1, note_at(63, 3));
  (void)rig.invoke("select.extend", 1);  // a Note-column block of rows 0..1
  NEED(!rig.state().edit.selection.block(rig.view().row(), rig.view().track()).whole_steps());
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(0);  // TRANSPOSE opens the dialog on the Note column
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  rig.tap(Button::Right);  // OP: TRANSPOSE -> INSTRUMENT
  CHECK(rig.state().ptools.tool == app::PTool::Instrument);
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "INSTRUMENT: WHOLE STEPS OR THE INSTR COLUMN");
  rig.flush();
  rig.tap(Button::F7);
  rig.pad(0);
  rig.tap(Button::Right);
  rig.tap(Button::Right);  // OP: CLEAR
  CHECK(rig.state().ptools.tool == app::PTool::Clear);
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "CLEAR: WHOLE STEPS ONLY");  // edit_selection ignores the columns: it would have wiped whole steps
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.step(0, 0).instr == 3);
  CHECK(rig.step(0, 1).note == 63);
  CHECK(rig.depth() == depth);
}

TEST_CASE("OS-PTOOLS2 S2 the cursor step alone (no selection) is whole steps: the dialog applies there") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(0);
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  rig.tap(Button::Down);
  rig.turn(2);  // AMOUNT +2
  rig.tap(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).note == 63);
  CHECK(rig.message() == "TRANSPOSED: 1 STEP +2 ST");
  CHECK(rig.depth() == depth + 1);
}

TEST_CASE("OS-PTOOLS2 S2 F6 REPEAT repeats the last APPLY with the same amount, on the selection as it is now, one undo unit; outside Edit it says TURN EDIT ON") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int r = 0; r < 4; ++r) {
    put(rig, 0, r, note_at(static_cast<std::uint8_t>(61 + r), 2));
  }
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);  // rows 0..3
  }
  rig.tap(Button::F7);
  rig.pad(0);
  rig.tap(Button::Down);
  rig.shift_turn(1);  // +12
  rig.tap(Button::F1);
  CHECK(rig.step(0, 0).note == 73);
  CHECK(rig.message() == "TRANSPOSED: 4 STEPS +12 ST");
  CHECK(verb_menu_f6_label(rig) == "REPEAT");  // the label is true: what F6 runs is a TOOLS apply, not a fill
  const unsigned depth = rig.depth();
  rig.flush();
  // F7 then F6 REPEAT: the same amount again, one more undo unit
  verb_menu_repeat(rig);
  CHECK(rig.step(0, 0).note == 85);  // 73 + 12
  CHECK(rig.step(0, 3).note == 88);
  CHECK(rig.message() == "TRANSPOSED: 4 STEPS +12 ST");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 73);
  // outside Edit the repeat of the dialog's APPLY says TURN EDIT ON and writes nothing
  rig.flush();
  rig.mode(PatternMode::Browse);
  const unsigned d2 = rig.depth();
  verb_menu_repeat(rig);
  CHECK(rig.message() == "TURN EDIT ON");
  CHECK(rig.depth() == d2);
  // a fill after the APPLY becomes what F6 repeats again (the last verb wins)
  rig.flush();
  rig.edit_mode();
  (void)rig.invoke("select.column_all");
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 2);  // EUCLID, 4 events
  rig.tap(Button::F1);
  CHECK(rig.state().fill.valid);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the track is empty again, so the repeat has something to fill
  rig.flush();
  verb_menu_repeat(rig);
  CHECK(rig.message() == "FILL: EUCLID 4 EVENTS");  // the fill, not the transpose
  CHECK(verb_menu_f6_label(rig) == "RE FILL");  // and the label says so again
}

TEST_CASE("OS-PTOOLS2 S2 the dialog's refusals: outside Edit the menu's row says TURN EDIT ON and no dialog opens; with no document NO PROJECT") {
  Rig rig(true, 16);
  rig.mode(PatternMode::Browse);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);  // the menu itself opens in Browse (S1)
  rig.pad(0);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "TURN EDIT ON");
  rig.tap(Button::F8);
  Rig bare(false);  // no document: the demo
  bare.edit_mode();
  bare.tap(Button::F7);
  bare.pad(0);
  CHECK(bare.state().overlay == app::OverlayId::None);
  CHECK(bare.message() == "NO PROJECT");
}

TEST_CASE("OS-PTOOLS2 S2 the footer's text: the target line and the count (steps, notes) of the block, a column block, and the cursor step") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  put(rig, 0, 2, note_at(63, 3));
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);  // rows 0..2 of the Note column
  const app::Selection::Block b = rig.state().edit.selection.block(rig.view().row(), rig.view().track());
  core::FixedString<48> target;
  app::pattern_tools_target(rig.state().edit.selection, rig.view().row(), rig.view().track(), target);
  CHECK(std::string(target.c_str()) == "BLOCK  TRACK 1  NOTE  ROWS 01-03");
  unsigned steps = 0;
  unsigned notes = 0;
  app::pattern_tools_count(&rig.project(), rig.state().edit.pattern, b, steps, notes);
  CHECK(steps == 3);
  CHECK(notes == 2);
  // the cursor step alone (the cursor is on row 2 after the extends; home resets track and column, not the row)
  (void)rig.invoke("select.clear");
  app::pattern_tools_target(rig.state().edit.selection, rig.view().row(), rig.view().track(), target);
  CHECK(std::string(target.c_str()) == "STEP  TRACK 1  ROW 03");
}

TEST_CASE("OS-PTOOLS2 S2 the footer of a whole-step block (the track column, extended over two tracks)") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 3));
  put(rig, 0, 2, note_at(63, 3));
  (void)rig.invoke("select.column_all");  // the whole track: whole steps, rows 0..15
  (void)rig.invoke("select.extend", 10);  // right: with the column block it steps by tracks -> tracks 0..1
  const app::Selection::Block b2 = rig.state().edit.selection.block(rig.view().row(), rig.view().track());
  NEED(b2.whole_steps());
  core::FixedString<48> target;
  app::pattern_tools_target(rig.state().edit.selection, rig.view().row(), rig.view().track(), target);
  CHECK(std::string(target.c_str()) == "BLOCK  TRACKS 1-2  ROWS 01-16");
  unsigned steps = 0;
  unsigned notes = 0;
  app::pattern_tools_count(&rig.project(), rig.state().edit.pattern, b2, steps, notes);
  CHECK(steps == 32);
  CHECK(notes == 2);
}

TEST_CASE("OS-PTOOLS2 S3b verb menu, pad 6 (HUMANISE): the steps with a note get a micro-move and a volume jitter, one undo unit") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int r = 0; r < 8; ++r) {
    model::Step s = note_at(static_cast<std::uint8_t>(61 + r), 1);
    s.vol = 100;
    put(rig, 0, r, s);
  }
  put(rig, 0, 9, note_at(70, 1));  // a step past the block: untouched
  for (int i = 0; i < 7; ++i) {
    (void)rig.invoke("select.extend", 1);  // rows 0..7, whole steps? no: the Note column; HUMANISE works on notes whatever the columns
  }
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);
  rig.pad(5);  // pad 6 = HUMANISE (fixed defaults TIME 20, VOL 8)
  CHECK(rig.state().overlay == app::OverlayId::None);
  bool any_fx = false;
  for (int r = 0; r < 8; ++r) {
    CHECK(rig.step(0, r).note == 61 + r);  // the notes stay
    CHECK(rig.step(0, r).vol >= 92);  // the volume jitter +-8
    CHECK(rig.step(0, r).vol <= 108);
    any_fx = any_fx || rig.step(0, r).fx[0].type == model::verbs::kFxMicroMove || rig.step(0, r).fx[1].type == model::verbs::kFxMicroMove;
  }
  CHECK(any_fx);  // the micro-move was written
  CHECK(rig.step(0, 9).fx[0].type == model::kFxNone);  // outside the block
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).vol == 100);
  CHECK(rig.step(0, 0).fx[0].type == model::kFxNone);
}

TEST_CASE("OS-PTOOLS2 S3b the five verbs by name (verb.interpolate / rotate_up / randomise / humanise / rotate_down): one undo unit each, refused outside Edit like the other block verbs") {
  Rig rig(true, 16);
  rig.edit_mode();
  put(rig, 0, 0, note_at(61, 5));
  put(rig, 0, 4, note_at(65, 5));
  for (int i = 0; i < 4; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  const unsigned depth = rig.depth();
  CHECK(rig.invoke("verb.interpolate") == DispatchResult::Changed);  // the note ramp 61..65
  CHECK(rig.step(0, 1).note == 62);
  CHECK(rig.step(0, 3).note == 64);
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("verb.rotate_up") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 62);
  CHECK(rig.depth() == depth + 2);
  CHECK(rig.invoke("verb.rotate_down") == DispatchResult::Changed);  // back
  CHECK(rig.step(0, 0).note == 61);
  CHECK(rig.depth() == depth + 3);
  const unsigned before_rand = rig.depth();
  CHECK(rig.invoke("verb.randomise") == DispatchResult::Changed);  // the block's column (Note here): in place, within the default AMOUNT
  CHECK(rig.depth() == before_rand + 1);
  CHECK(rig.invoke("verb.humanise") == DispatchResult::Changed);
  CHECK(rig.depth() == before_rand + 2);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // every verb is its own unit
  // outside Edit the actions are Disabled (the precondition pe::edit_enabled), and run_verb says TURN EDIT ON before dispatching
  Rig browse(true, 16);
  CHECK(browse.invoke("verb.interpolate") == DispatchResult::Disabled);
  CHECK(browse.invoke("verb.rotate_up") == DispatchResult::Disabled);
  browse.tap(Button::F7);
  CHECK(browse.state().overlay == app::OverlayId::VerbMenu);  // the menu opens in Browse too
  const unsigned bdepth = browse.depth();
  browse.pad(3);  // pad 4 = INTERPOLATE
  CHECK(browse.state().overlay == app::OverlayId::None);
  CHECK(browse.message() == "TURN EDIT ON");
  CHECK(browse.depth() == bdepth);  // nothing written, no unit
}

TEST_CASE("OS-PTOOLS2 S3b F6 REPEAT stays the last FILL's, as its RE FILL label says: a menu verb (ROTATE, an export) never becomes the repeat") {
  // ROTATE from the menu, then a fill of the F4 dialog applied, then F6: the FILL repeats (its notice, one unit), the rotation does not run again
  Rig rig(true, 16);
  rig.edit_mode();
  for (int r = 0; r < 4; ++r) {
    put(rig, 0, r, note_at(static_cast<std::uint8_t>(61 + r), 1));
  }
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  rig.tap(Button::F7);
  rig.pad(6);  // pad 7 = ROTATE UP
  CHECK(rig.step(0, 0).note == 62);
  const unsigned depth = rig.depth();
  rig.flush();
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 1);  // RANDOM
  rig.tap(Button::F1);         // apply the fill over the block
  CHECK(rig.message().find("FILL:") == 0);
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  rig.flush();
  verb_menu_repeat(rig);  // REPEAT: the fill again, not the rotate
  CHECK(rig.message().find("FILL:") == 0);
  CHECK(rig.depth() == depth + 1);
  CHECK(verb_menu_f6_label(rig) == "RE FILL");  // the label is true: what F6 ran is a fill
  // after a menu export, F6 says NOTHING TO REPEAT and writes nothing (the export is not re-run, no file rewritten)
  Rig rig2(true, 16);
  rig2.edit_mode();
  const unsigned depth2 = rig2.depth();
  rig2.tap(Button::F7);
  rig2.pad(13);  // pad 14 = EXPORT SELECTION
  CHECK(rig2.state().overlay == app::OverlayId::None);
  rig2.flush();
  CHECK(rig2.invoke("tools.repeat") == DispatchResult::Changed);  // it ran and said NOTHING TO REPEAT
  CHECK(rig2.message() == "NOTHING TO REPEAT");
  CHECK(rig2.depth() == depth2);
}

// ---- OS-PTOOLS2 S3b: F7 held + pad (the same held-key + pad grammar as F1 PATTERN) ------------------------------------------------------------

TEST_CASE("OS-PTOOLS2 S3b F7 held + pad runs the verb without opening the menu; a tap of F7 still opens it; a pad of FILL opens the dialog") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int r = 0; r < 4; ++r) {
    put(rig, 0, r, note_at(static_cast<std::uint8_t>(61 + r), 1));
  }
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);  // rows 0..3
  }
  const unsigned depth = rig.depth();
  rig.tap(Button::F7);  // a tap opens the menu
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  // F7 held + pad 7 (ROTATE UP): the verb runs at once, the menu never opens
  rig.hold_key(Button::F7);  // press + hold_ms + 1: the Hold fires (verb.hold, opens nothing)
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.pad(6);
  CHECK(rig.state().overlay == app::OverlayId::None);  // still no menu
  CHECK(rig.step(0, 0).note == 62);                    // rotated up
  CHECK(rig.step(0, 3).note == 61);
  CHECK(rig.message() == "ROTATED: 4 STEPS");
  CHECK(rig.depth() == depth + 1);  // one undo unit
  rig.release_key(Button::F7);
  // F7 held + pad 3 (FILL, a verb with parameters): opens what the verb opens today, the Fill dialog
  rig.hold_key(Button::F7);
  rig.pad(2);
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  rig.release_key(Button::F7);
  rig.tap(Button::F8);  // BACK closes the dialog
  CHECK(rig.state().overlay == app::OverlayId::None);
  // a quick tap after all that still opens the menu (the hold only made the tap wait)
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
}

// F7 down, a pad inside hold_ms, F7 up: the pad's Tap carries F7 as modifier and runs the verb (verb.hold_pad); the Tap of F7 that fires at the
// release is NOT a menu open (verb.open ignores that one Tap through the pad mark). Same on a FILL pad: the dialog opens and the release does not
// put the menu over it.
TEST_CASE("OS-PTOOLS2 S3b F7 released inside hold_ms after a pad: the verb ran and the F7 Tap does not open the menu; the next tap of F7 does") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int r = 0; r < 4; ++r) {
    put(rig, 0, r, note_at(static_cast<std::uint8_t>(61 + r), 1));
  }
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  const unsigned depth = rig.depth();
  rig.press(Button::F7);
  rig.pass(1);  // well inside hold_ms: no Hold yet
  rig.pad(6);   // pad 7 = ROTATE UP, under the held F7
  CHECK(rig.state().overlay == app::OverlayId::None);  // the menu never opened
  CHECK(rig.step(0, 0).note == 62);                    // the verb ran
  CHECK(rig.depth() == depth + 1);
  rig.release(Button::F7);
  rig.pass(1);  // the Tap of F7 fires here: eaten by the pad mark, no menu
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).note == 62);  // not rotated again, no menu over the result
  rig.tap(Button::F7);  // the next tap of F7 is a real one: the menu opens
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.tap(Button::F8);
  // the same on a FILL pad: the dialog opens, the release does not open the menu over it
  rig.press(Button::F7);
  rig.pass(1);
  rig.pad(2);  // pad 3 = FILL
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  rig.release(Button::F7);
  rig.pass(1);
  CHECK(rig.state().overlay == app::OverlayId::Fill);  // still the dialog, no menu over it
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.tap(Button::F8);
}

TEST_CASE("OS-PTOOLS2 S3b F7 held + pad refused in LIVE like the menu: the strip says THE PLAYHEAD HOLDS THE CURSOR IN LIVE, nothing runs") {  Rig rig(true, 16);
  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  pos.publish(true, 1, 4, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  NEED(rig.state().play.playing);
  NEED(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Browse));  // WATCH
  put(rig, 0, 0, note_at(61, 1));
  put(rig, 0, 1, note_at(62, 1));
  (void)rig.invoke("select.extend", 1);
  rig.shift_tap(Button::Rec);  // LIVE (Edit following the playhead)
  NEED(app::pattern_editable(rig.state()));
  CHECK_FALSE(rig.edit().rec_free);
  const unsigned depth = rig.depth();
  rig.hold_key(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "THE PLAYHEAD HOLDS THE CURSOR IN LIVE");
  rig.pad(6);  // pad 7 = ROTATE UP: refused too
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.step(0, 0).note == 61);  // nothing ran
  CHECK(rig.depth() == depth);
  rig.release_key(Button::F7);
  rig.app->attach_document(nullptr);  // (a LIVE take is open: the rig destroys its editor before the app)
}

// OS-RECSIGN (new Community OS behaviour): the REC signs (the strip's badge, the Rec frame round the grid) show exactly when the edit actions are enabled: one predicate
// (pattern_editable; pe::edit_enabled calls it), so in every mode (WATCH, STEP, LIVE) the picture and the actions agree.
namespace {
struct SignProbe {
  bool frame;  // the grid's top-left pixel is the Rec red
  bool badge;  // some pixel of the status strip is the Rec red
};
SignProbe sign_probe(const Rig& rig) {
  sim::EveRaster raster;
  sim::RasterDisplay display(raster);
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  eve::DisplayListBuffer storage;
  eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
  const ui::Theme& theme = ui::theme(ui::theme_id_at(0));
  ui::Renderer r(builder, theme);
  CHECK(r.begin(ui::Color::Pane));
  app::render_scene(rig.app->model(), rig.state(), r);
  CHECK(r.end());
  const ui::Rgb red = theme.rgb(ui::Color::Rec);
  const std::uint8_t* px = raster.rgba();
  const auto is_red = [&](std::size_t x, std::size_t y) {
    const std::uint8_t* q = px + (y * 800u + x) * 4u;
    return q[0] == red.r && q[1] == red.g && q[2] == red.b;
  };
  const std::size_t grid_y = static_cast<std::size_t>(app::make_pattern_layout(theme, 16, 5).grid.y);
  SignProbe out{is_red(0, grid_y + 1), false};
  for (std::size_t y = 0; y < grid_y && !out.badge; ++y) {
    for (std::size_t x = 0; x < 800u; ++x) {
      if (is_red(x, y)) {
        out.badge = true;
        break;
      }
    }
  }
  return out;
}
}  // namespace

TEST_CASE("OS-RECSIGN: the REC signs follow the edit predicate in WATCH, STEP and LIVE, and the edit actions agree") {
  Rig rig(true, 16);
  rig.mode(PatternMode::Browse);
  CHECK_FALSE(app::pattern_editable(rig.state()));
  SignProbe s = sign_probe(rig);
  CHECK_FALSE(s.frame);
  CHECK_FALSE(s.badge);
  CHECK(rig.invoke("step.note", 40) == DispatchResult::Disabled);

  rig.tap(Button::Rec);  // STEP: Edit, free
  CHECK(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Edit));
  CHECK(rig.edit().rec_free);
  CHECK(app::pattern_editable(rig.state()));
  s = sign_probe(rig);
  CHECK(s.frame);
  CHECK(s.badge);
  CHECK(rig.invoke("step.note", 40) == DispatchResult::Changed);

  rig.shift_tap(Button::Rec);  // LIVE (stopped it may start the transport and count in; the predicate is the judge)
  s = sign_probe(rig);
  CHECK(s.frame == app::pattern_editable(rig.state()));
  CHECK(s.badge == app::pattern_editable(rig.state()));

  rig.mode(PatternMode::Browse);  // and back: the signs go
  s = sign_probe(rig);
  CHECK_FALSE(s.frame);
  CHECK_FALSE(s.badge);
}

// OS-DELREPEAT (new Community OS behaviour): in Pattern, a held Delete repeats and speeds up like the arrows (the same schedule). A repeat is a flagged Tap that reaches only a
// kActionRepeatable action; the press itself always runs.
namespace {
void hold(Rig& rig, Button b, unsigned ms) {
  rig.press(b);
  rig.pass(1);
  for (unsigned t = 0; t < ms; ++t) {
    rig.pass(1);
  }
  rig.release(b);
  rig.pass(1);
}
void fill_notes(Rig& rig, unsigned rows) {
  for (unsigned r = 0; r < rows; ++r) {
    rig.go(static_cast<int>(r));
    CHECK(rig.invoke("step.note", 40) == DispatchResult::Changed);
  }
}
}  // namespace

TEST_CASE("OS-DELREPEAT: a held Delete clears step after step on the arrows' schedule, and a held Shift + Delete pulls") {
  Rig walk(true, 64);
  walk.edit_mode();
  walk.go(0);
  hold(walk, Button::Down, 1500);
  const int arrow_steps = walk.view().row();
  CHECK(arrow_steps >= 8);  // the press and the repeats of the schedule: 0, 400, 700, ... 1470

  Rig rig(true, 64);
  rig.edit_mode();
  fill_notes(rig, 32);
  rig.go(0);
  hold(rig, Button::Delete, 1500);
  CHECK(rig.view().row() == arrow_steps);  // the same number of steps as the arrow
  for (int r = 0; r < 32; ++r) {
    CHECK_MESSAGE((rig.step(0, r).note == model::kEmptyStep.note) == (r < arrow_steps), "row " << r);
  }
  // one tap still clears exactly one step
  rig.go(20);
  rig.tap(Button::Delete);
  CHECK(rig.step(0, 20).note == model::kEmptyStep.note);
  CHECK(rig.step(0, 21).note != model::kEmptyStep.note);

  // Shift + Delete is the pull (OS-DELPULL): the rows below close up over the cursor step, once per repeat
  Rig back(true, 64);
  back.edit_mode();
  fill_notes(back, 32);
  back.go(10);
  const unsigned depth = back.depth();
  back.press(Button::Shift);
  back.pass(1);
  hold(back, Button::Delete, 750);  // the press, 400, 700
  back.release(Button::Shift);
  back.pass(1);
  CHECK(back.depth() - depth == 3u);  // one undo unit per repeat (OS-UNDOGRP will group them)
}

TEST_CASE("OS-DELREPEAT: a repeat reaches only a repeatable action; the press always runs") {
  Rig rig(true, 16);
  rig.edit_mode();
  const app::ActionTable t = app::action_registry();
  const auto repeats = [&](const char* name) {
    const app::ActionDef* a = app::find_action(t, core::action_id(name));
    return a != nullptr && (a->flags & core::kActionRepeatable) != 0;
  };
  CHECK(repeats("step.clear"));
  CHECK(repeats("step.pull"));
  CHECK_FALSE(repeats("file.delete"));
  CHECK_FALSE(repeats("song.delete_slot"));
  CHECK_FALSE(repeats("instrument.delete"));
  CHECK_FALSE(repeats("learn.delete"));
  // a flagged repeat of an action without the flag does nothing; the same call as a press runs it
  rig.go(3);
  CHECK(rig.invoke("step.note", 40) == DispatchResult::Changed);
  CHECK(rig.app->invoke(core::action_id("step.note"), 41, true) == DispatchResult::Unchanged);
  rig.go(3);
  CHECK(rig.step(0, 3).note == 40);  // the repeat did not overwrite it
  CHECK(rig.app->invoke(core::action_id("step.clear"), 0, true) == DispatchResult::Changed);  // repeatable: runs
}

// OS-ROWWRAP (new Community OS behaviour; what the official firmware does is an open research question): a fresh Up on the first row goes to the last row of the same pattern, a fresh Down on the last row to the first. Before: both clamped.
// A held arrow never wraps (the repeats are not fresh presses), the block extension (Shift + the arrows) still clamps, and the length is the pattern's own.
TEST_CASE("OS-ROWWRAP: a fresh Up on the first row goes to the last row, a fresh Down on the last to the first, whatever the pattern's length") {
  for (const unsigned len : {16u, 32u, 64u, 100u}) {
    Rig rig(true, len);
    rig.go(0);
    CHECK(rig.view().row() == 0);
    rig.tap(Button::Up);
    CHECK_MESSAGE(rig.view().row() == static_cast<int>(len) - 1, "length " << len);
    rig.tap(Button::Down);
    CHECK_MESSAGE(rig.view().row() == 0, "length " << len);
    rig.tap(Button::Down);
    CHECK(rig.view().row() == 1);  // an ordinary move is as before
    rig.tap(Button::Up);
    CHECK(rig.view().row() == 0);
  }
  // in Edit too, and the column and the track stay
  Rig rig(true, 16);
  rig.edit_mode();
  rig.go(0, 2);
  rig.column(Column::Vol);
  const int track = rig.view().track();
  rig.tap(Button::Up);
  CHECK(rig.view().row() == 15);
  CHECK(rig.view().track() == track);
  CHECK(rig.view().column() == Column::Vol);
}

TEST_CASE("OS-ROWWRAP: a held arrow stops at the edge; the press that starts at the edge wraps once and the repeats walk on from there") {
  Rig up(true, 16);
  up.go(3);
  hold(up, Button::Up, 3000);  // the press and ~30 repeats: more than enough to reach row 0 and go on if it wrapped
  CHECK(up.view().row() == 0);
  Rig down(true, 16);
  down.go(12);
  hold(down, Button::Down, 3000);
  CHECK(down.view().row() == 15);
  Rig edge(true, 16);
  edge.go(0);
  hold(edge, Button::Up, 600);  // the press wraps to row 15, the repeat at 400 ms moves to row 14: it does not cycle
  CHECK(edge.view().row() == 14);
}

TEST_CASE("OS-ROWWRAP: Shift + the arrows extend the block and clamp at the edge, no wrap") {
  Rig rig(true, 16);
  rig.go(0);
  rig.shift_tap(Button::Up);
  rig.shift_tap(Button::Up);
  CHECK(rig.view().row() == 0);
  rig.go(15);
  rig.shift_tap(Button::Down);
  rig.shift_tap(Button::Down);
  CHECK(rig.view().row() == 15);
}

// A clearing action never wraps, wherever the hold starts: the repeats stop at the last row and never clear from the top, and a press on the last row (which may start a hold) stays there.
TEST_CASE("OS-DELREPEAT: a held Delete stops at the last row, also when the hold starts on it") {
  Rig rig(true, 16);
  rig.edit_mode();
  fill_notes(rig, 16);
  rig.go(11);
  hold(rig, Button::Delete, 4000);  // far more repeats than the 5 rows left
  CHECK(rig.view().row() == 15);
  for (int r = 0; r < 16; ++r) {
    CHECK_MESSAGE((rig.step(0, r).note == model::kEmptyStep.note) == (r >= 11), "row " << r);  // rows 0..10 untouched
  }
  // a hold that starts on the last row: it clears that row and stays, row 0 is never reached
  fill_notes(rig, 16);
  rig.go(15);
  hold(rig, Button::Delete, 4000);
  CHECK(rig.view().row() == 15);
  for (int r = 0; r < 16; ++r) {
    CHECK_MESSAGE((rig.step(0, r).note == model::kEmptyStep.note) == (r == 15), "row " << r);
  }
  // so does a single press on the last row
  fill_notes(rig, 16);
  rig.go(15);
  rig.tap(Button::Delete);
  CHECK(rig.view().row() == 15);
  CHECK(rig.step(0, 0).note != model::kEmptyStep.note);
}

// A JUMP of more than one: the Delete whose step would pass the end stays on the last row (it does not land on row (14 + 4) % 16 = 2).
TEST_CASE("OS-DELREPEAT: a Delete with JUMP 4 from row 14 of 16 stops on row 15") {
  Rig rig(true, 16);
  rig.edit_mode();
  fill_notes(rig, 16);
  rig.tap(Button::F3);  // the JUMP chooser
  rig.turn(3);
  NEED(rig.edit().edit_step == 4);
  rig.tap(Button::F3);
  rig.go(14);
  rig.tap(Button::Delete);
  CHECK(rig.step(0, 14).note == model::kEmptyStep.note);
  CHECK(rig.view().row() == 15);
  rig.tap(Button::Delete);  // and again on the last row: it clears that one and stays
  CHECK(rig.step(0, 15).note == model::kEmptyStep.note);
  CHECK(rig.view().row() == 15);
  CHECK(rig.step(0, 2).note != model::kEmptyStep.note);
}

TEST_CASE("OS-PADHOLD: the Instr hold popup chooses the instrument; Fx and Vol pad entry are as before") {
  Rig rig;
  rig.edit_mode();
  rig.go(0);
  rig.tap(Button::Vol);
  rig.pad(47);  // Vol: the pad maps over 0..127
  CHECK(rig.step(0, 0).vol == 127);
  rig.go(1);
  hold_open(rig, Button::Instr);  // the Instr hold popup
  rig.pad(6);                     // a pad in the popup: instrument 7
  CHECK(rig.state().instrument == 7);
  let_go(rig, Button::Instr);
  CHECK(rig.app->instrument() == 7);
}

TEST_CASE("live notes: VEL> of a sample instrument blends the keyboard's velocity with none: 100 is the velocity as it is (the law of every earlier build), 0 plays 127; a MIDI instrument and a free slot are untouched (OS-INSTUI S9)") {
  Rig rig;
  LiveLog sink;
  rig.app->attach_preview(&sink);
  model::Instrument s;
  model::init_instrument(s, model::InstrumentKind::Sample);
  CHECK(s.vel_sens == 100);  // the default
  CHECK(rig.project().set_instrument(0, s) == model::Status::Ok);
  midi::CcInput in;
  const auto played = [&](std::uint8_t velocity) {
    sink.log.clear();
    send_midi(in, 0, {0x90, 60, velocity});
    (void)rig.app->poll_midi_notes(in);
    CHECK(!sink.log.empty());
    return sink.log.empty() ? 9999u : sink.log[0].d;  // the velocity handed to the sink
  };
  CHECK(rig.app->instrument() == 1);
  CHECK(played(100) == 100u);  // VEL> 100: the keyboard's velocity, whatever the instrument's volume
  CHECK(played(64) == 64u);
  CHECK(played(1) == 1u);
  CHECK(played(127) == 127u);
  s.sampler.volume = 40;  // the volume does not enter the law (it never did: the velocity replaces the gain in the voices)
  CHECK(rig.project().set_instrument(0, s) == model::Status::Ok);
  CHECK(played(64) == 64u);
  s.sampler.volume = 50;
  s.vel_sens = 0;
  CHECK(rig.project().set_instrument(0, s) == model::Status::Ok);
  CHECK(played(100) == 127u);  // VEL> 0: the velocity is ignored, as for a velocity of 127
  CHECK(played(1) == 127u);
  s.vel_sens = 50;
  CHECK(rig.project().set_instrument(0, s) == model::Status::Ok);
  CHECK(played(64) == 96u);    // half way between the velocity and 127
  // a MIDI instrument, a free slot and a note off: the velocity is the keyboard's
  model::Instrument m;
  model::init_instrument(m, model::InstrumentKind::Midi);
  m.midi_channel = 2;
  m.vel_sens = 0;
  CHECK(rig.project().set_instrument(1, m) == model::Status::Ok);
  (void)rig.invoke("instrument.select", 2);
  CHECK(played(100) == 100u);
  (void)rig.invoke("instrument.select", 9);
  CHECK(played(100) == 100u);
}

// ---- OS-PATCOPY: Copy and Shift + Copy ------------------------------------------------------------------------------------------------------

namespace {
// A note on (row, track), written through the model so the cursor and the edit step stay out of it.
void put_note(Rig& rig, int track, int row, unsigned note) {
  model::Step s = model::kEmptyStep;
  s.note = static_cast<std::uint8_t>(note);
  s.instr = 1;
  (void)rig.project().set_steps(1, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(row), &s, 1);
}
}  // namespace

TEST_CASE("OS-PATCOPY: Copy takes the block, Shift + Copy writes it at the cursor, one undo unit, the cursor stays") {
  Rig rig(true, 64);
  rig.edit_mode();
  for (int t = 0; t < 2; ++t) {
    for (int r = 0; r < 3; ++r) {
      put_note(rig, t, 2 + r, 40u + static_cast<unsigned>(t * 10 + r));
    }
  }
  rig.editor->history_reset();
  rig.go(2, 0);
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);
  rig.extend_tracks(1);  // 3 rows x 2 tracks
  const unsigned depth = rig.depth();
  rig.tap(Button::Copy);
  CHECK(rig.message() == "COPIED 6 STEPS");
  CHECK(rig.depth() == depth);  // a copy is no edit
  rig.flush();
  rig.go(10, 3);
  rig.shift_tap(Button::Copy);
  for (int t = 0; t < 2; ++t) {
    for (int r = 0; r < 3; ++r) {
      CHECK_MESSAGE(rig.step(3 + t, 10 + r).note == 40u + static_cast<unsigned>(t * 10 + r), "track " << t << " row " << r);
    }
  }
  CHECK(model::is_empty(rig.step(3, 13)));
  CHECK(rig.depth() == depth + 1u);
  CHECK((rig.view().row() == 10 && rig.view().track() == 3));  // the cursor stays
  // the source is untouched, and one undo takes the whole paste back
  CHECK(rig.step(0, 2).note == 40);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  for (int t = 3; t < 5; ++t) {
    for (int r = 10; r < 13; ++r) {
      CHECK(model::is_empty(rig.step(t, r)));
    }
  }
  CHECK(rig.depth() == depth);
}

TEST_CASE("OS-PATCOPY: with no block the step under the cursor is copied; a paste over a pattern that is not there yet creates it; there is no cut") {
  Rig rig(true, 16);
  put_note(rig, 0, 4, 55);
  rig.editor->history_reset();
  rig.edit_mode();
  rig.go(4, 0);
  rig.tap(Button::Copy);
  CHECK(rig.message() == "COPIED 1 STEP");
  rig.go(9, 2);
  rig.shift_tap(Button::Copy);
  CHECK(rig.step(2, 9).note == 55);
  // Copy then Delete is the cut: the clipboard keeps what the step was
  rig.go(4, 0);
  rig.tap(Button::Delete);
  CHECK(model::is_empty(rig.step(0, 4)));
  rig.go(12, 1);
  rig.shift_tap(Button::Copy);
  CHECK(rig.step(1, 12).note == 55);
  // a paste with nothing copied says so
  Rig fresh(true, 16);
  fresh.edit_mode();
  fresh.shift_tap(Button::Copy);
  CHECK(fresh.message() == "NOTHING TO PASTE");
  CHECK(fresh.depth() == 0u);
}

TEST_CASE("OS-PATCOPY: a paste is clipped at the end of the pattern and at the last track, never wrapped, no length changes") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int t = 0; t < 3; ++t) {
    for (int r = 0; r < 4; ++r) {
      put_note(rig, 9 + t, r, 60u + static_cast<unsigned>(t * 4 + r));  // tracks 9..11 (a new project has 12 tracks)
    }
  }
  rig.editor->history_reset();
  rig.go(0, 9);
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);
  rig.extend_tracks(2);  // 4 rows x 3 tracks
  rig.tap(Button::Copy);
  rig.go(14, 11);  // two rows left, one track
  const unsigned depth = rig.depth();
  rig.shift_tap(Button::Copy);
  CHECK(rig.step(11, 14).note == 60u);  // the clip's first column (track 9's), rows 0 and 1; its other columns and rows are dropped
  CHECK(rig.step(11, 15).note == 61u);
  CHECK(rig.step(11, 0).note == 68u);   // the source is untouched, nothing was written from the top
  CHECK(rig.step(11, 1).note == 69u);
  CHECK(rig.step(11, 2).note == 70u);
  CHECK(rig.step(11, 13).note == 0);
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.project().pattern_length(1) == 16);
  for (int t = 0; t < 12; ++t) {
    CHECK(rig.project().pattern(1)->track_length[t] == 16);
  }
  // a paste whose first row is past a track's own end writes nothing and says so
  (void)rig.project().set_track_length(1, 11, 8);
  rig.editor->history_reset();
  rig.flush();
  rig.go(12, 11);
  rig.shift_tap(Button::Copy);
  CHECK(rig.message() == "PAST TRACK END");
  CHECK(rig.depth() == 0u);
}

TEST_CASE("OS-PATCOPY: Copy works in every mode, Paste only in Edit (STEP or LIVE), and while the pattern plays") {
  Rig rig(true, 32);
  put_note(rig, 0, 1, 50);
  put_note(rig, 0, 2, 51);
  rig.editor->history_reset();
  rig.go(1, 0);
  (void)rig.invoke("select.extend", 1);
  rig.tap(Button::Copy);  // Browse
  CHECK(rig.message() == "COPIED 2 STEPS");
  rig.go(10, 1);
  CHECK(rig.invoke("block.paste") == DispatchResult::Disabled);  // Browse: no edit
  CHECK(model::is_empty(rig.step(1, 10)));

  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  pos.publish(true, 1, 20, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  rig.tap(Button::Rec);  // STEP, playing: the cursor stays where it is
  NEED(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Edit));
  NEED(rig.state().play.playing);
  rig.go(10, 1);
  const unsigned depth = rig.depth();
  rig.shift_tap(Button::Copy);
  CHECK(rig.step(1, 10).note == 50);
  CHECK(rig.step(1, 11).note == 51);
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(model::is_empty(rig.step(1, 10)));

  // LIVE, playing: the cursor rides the playhead, so the paste lands on the row the playhead is on when the key is pressed
  rig.tap(Button::Rec);  // STEP off: back to WATCH
  rig.shift_tap(Button::Rec);  // LIVE
  NEED(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Edit));
  NEED(!rig.edit().rec_free);
  pos.publish(true, 1, 25, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  CHECK(rig.view().row() == 25);
  rig.shift_tap(Button::Copy);
  CHECK(rig.step(rig.view().track(), 25).note == 50);
  CHECK(rig.step(rig.view().track(), 26).note == 51);
  rig.app->attach_document(nullptr);  // (a LIVE take is open: the rig destroys its editor before the app)
}

// ---- OS-DELPULL: Shift + Delete is the inverse of Insert ----------------------------------------------------------------------------------------

TEST_CASE("OS-DELPULL: Insert then Shift + Delete gives the pattern back byte for byte, past-the-length steps included; the cursor and the block stay") {
  test::Rng rng(0xDE1B01u);
  for (unsigned episode = 0; episode < 24; ++episode) {
    const unsigned len = 8 + rng.below(100);
    Rig rig(true, len);
    rig.edit_mode();
    std::vector<model::Step> buf(128);
    for (int t = 0; t < 16; ++t) {
      for (int s = 0; s < 127; ++s) {  // (step 127 is the one Insert pushes out of the pattern: it cannot come back)
        model::Step m = model::kEmptyStep;
        if (rng.below(3) == 0) {
          m.note = static_cast<std::uint8_t>(1 + rng.below(120));
          m.instr = static_cast<std::uint8_t>(1 + rng.below(64));
          m.vol = static_cast<std::uint8_t>(rng.below(128));
        }
        if (rng.below(5) == 0) {
          m.fx[0] = {static_cast<std::uint8_t>(1 + rng.below(40)), rng.byte()};
        }
        buf[static_cast<std::size_t>(s)] = m;
      }
      buf[127] = model::kEmptyStep;
      (void)rig.project().set_steps(1, static_cast<std::uint32_t>(t), 0, buf.data(), 128);
    }
    rig.editor->history_reset();
    const int row = static_cast<int>(rng.below(len));
    const int t0 = static_cast<int>(rng.below(12));
    const int t1 = t0 + static_cast<int>(rng.below(static_cast<unsigned>(12 - t0)));
    rig.go(row, t0);
    rig.extend_tracks(t1 - t0);
    const std::uint64_t pre = pattern_digest(rig, false, false);
    CHECK(rig.invoke("step.insert") == DispatchResult::Changed);
    CHECK(pattern_digest(rig, false, false) != pre);
    CHECK(rig.invoke("step.pull") == DispatchResult::Changed);
    CHECK_MESSAGE(pattern_digest(rig, false, false) == pre, "episode " << episode);
    CHECK(rig.depth() == 2u);  // one undo unit each
    CHECK((rig.view().row() == (row + 1 < static_cast<int>(len) ? row + 1 : row) && rig.view().track() == t1));  // OS-DELJUMP: the cursor went down by the JUMP (1), no wrap, and the block ended
    CHECK(!rig.edit().selection.active);
    CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the pull alone is one unit: back to the inserted pattern
    CHECK(pattern_digest(rig, false, false) != pre);
  }
}

TEST_CASE("OS-DELPULL: the step under the cursor is deleted and the rows below move up; the steps before it stay; a track whose end is above the cursor refuses") {
  Rig rig(true, 16);
  rig.edit_mode();
  for (int r = 0; r < 16; ++r) {
    put_note(rig, 0, r, 30u + static_cast<unsigned>(r));
  }
  rig.editor->history_reset();
  rig.go(5, 0);
  CHECK(rig.invoke("step.pull") == DispatchResult::Changed);
  CHECK(rig.step(0, 4).note == 34);  // before the cursor: as it was
  CHECK(rig.step(0, 5).note == 36);  // the row below moved up over it
  CHECK(rig.step(0, 14).note == 45);
  CHECK(rig.step(0, 15).note == 0);  // the last one is cleared
  CHECK(rig.view().row() == 6);       // the cursor did not follow the rows (the old backspace pulled the rows above); it goes down by the JUMP (1) like Delete: OS-DELJUMP
  // at row 0 too
  rig.go(0, 0);
  CHECK(rig.invoke("step.pull") == DispatchResult::Changed);
  CHECK(rig.step(0, 0).note == 31);
  // a track of 8 steps with the cursor on row 12
  (void)rig.project().set_track_length(1, 2, 8);
  rig.editor->history_reset();
  rig.go(12, 2);
  CHECK(rig.invoke("step.pull") == DispatchResult::Changed);  // (the message is what changed)
  CHECK(rig.message() == "PAST TRACK END");
  CHECK(rig.depth() == 0u);
}

TEST_CASE("OS-DELJUMP: Shift + Delete pulls and then moves the cursor down by the JUMP like Delete; JUMP 0 stays; a refused or unchanged pull does not move") {
  Rig rig(true, 32);
  rig.edit_mode();
  for (int r = 0; r < 32; ++r) {
    put_note(rig, 0, r, 30u + static_cast<unsigned>(r));
  }
  rig.editor->history_reset();
  (void)rig.invoke("chooser.toggle", 2);
  (void)rig.invoke("chooser.pad", 2);
  (void)rig.invoke("chooser.toggle", 2);
  NEED(rig.edit().edit_step == 2);
  rig.go(4, 0);
  rig.shift_tap(Button::Delete);  // the real key: Shift + Delete
  CHECK(rig.step(0, 3).note == 33);
  CHECK(rig.step(0, 4).note == 35);  // the rows below moved up over the cursor's row
  CHECK(rig.step(0, 31).note == 0);
  CHECK(rig.view().row() == 6);      // JUMP 2
  CHECK(!rig.edit().selection.active);
  // a block ends with the move
  rig.go(10, 0);
  (void)rig.invoke("select.extend", 1);
  CHECK(rig.edit().selection.active);
  rig.shift_tap(Button::Delete);
  CHECK(!rig.edit().selection.active);
  CHECK(rig.view().row() == 13);  // from the cursor's row (11) by 2
  // no wrap: near the end it stops on the last row
  rig.go(31, 0);
  put_note(rig, 0, 31, 60);
  rig.shift_tap(Button::Delete);
  CHECK(rig.view().row() == 31);
  // JUMP 0: the cursor stays
  (void)rig.invoke("chooser.toggle", 2);
  (void)rig.invoke("chooser.pad", 0);
  (void)rig.invoke("chooser.toggle", 2);
  NEED(rig.edit().edit_step == 0);
  rig.go(4, 0);
  const int before = rig.step(0, 5).note;
  rig.shift_tap(Button::Delete);
  CHECK(rig.step(0, 4).note == before);
  CHECK(rig.view().row() == 4);
  // nothing to pull (empty tail): no change, no move
  for (int r = 0; r < 32; ++r) {
    (void)rig.project().set_step(1, 0, static_cast<unsigned>(r), model::kEmptyStep);
  }
  (void)rig.invoke("chooser.toggle", 2);
  (void)rig.invoke("chooser.pad", 2);
  (void)rig.invoke("chooser.toggle", 2);
  rig.go(8, 0);
  (void)rig.invoke("step.pull");
  CHECK(rig.view().row() == 8);
  // a track whose end is above the cursor refuses and does not move
  (void)rig.project().set_track_length(1, 2, 8);
  rig.go(12, 2);
  (void)rig.invoke("step.pull");
  CHECK(rig.message() == "PAST TRACK END");
  CHECK(rig.view().row() == 12);
}

// ---- OS-SELSHIFT: Shift + Note/Instr/Vol/Fx jump to a quarter and, in Edit, select the rows they cross ---------------------------------------------------------------------------------------

TEST_CASE("OS-SELSHIFT: in Edit, Shift + Note/Instr/Vol/Fx select the rows between the old and the new row (the new row not included)") {
  Rig rig;  // 64 rows: the quarters are 0, 16, 32, 48
  rig.edit_mode();
  const auto block = [&] { return rig.edit().selection.block(rig.view().row(), rig.view().track()); };
  // up: from row 16 to row 0 selects 0..15
  rig.go(16, 0);
  rig.shift_tap(Button::Note);
  CHECK(rig.view().row() == 0);
  CHECK(rig.edit().selection.active);
  app::Selection::Block b = block();
  CHECK((b.row0 == 0 && b.row1 == 15 && b.track0 == 0 && b.track1 == 0));
  // down: from row 0 to row 16 selects 0..15
  rig.go(0, 0);
  rig.shift_tap(Button::Instr);
  CHECK(rig.view().row() == 16);
  CHECK(rig.edit().selection.active);
  b = block();
  CHECK((b.row0 == 0 && b.row1 == 15));
  // from an inner row: row 20 to row 0 selects 0..19
  rig.go(20, 0);
  rig.shift_tap(Button::Note);
  CHECK(rig.view().row() == 0);
  b = block();
  CHECK((b.row0 == 0 && b.row1 == 19));
  // an active selection is replaced; Vol and Fx jump to 32 and 48
  rig.go(0, 0);
  rig.shift_tap(Button::Vol);
  CHECK(rig.view().row() == 32);
  b = block();
  CHECK((b.row0 == 0 && b.row1 == 31));
  rig.shift_tap(Button::Fx);
  CHECK(rig.view().row() == 48);
  b = block();
  CHECK((b.row0 == 32 && b.row1 == 47));
  // the track follows the cursor
  rig.go(16, 2);
  rig.shift_tap(Button::Note);
  b = block();
  CHECK((b.row0 == 0 && b.row1 == 15 && b.track0 == 2 && b.track1 == 2));
}

TEST_CASE("OS-SELSHIFT: the same row (no move) changes no selection; outside Edit the jump selects nothing") {
  Rig rig;
  rig.edit_mode();
  rig.go(0, 0);
  rig.shift_tap(Button::Note);  // already on row 0
  CHECK(rig.view().row() == 0);
  CHECK_FALSE(rig.edit().selection.active);
  rig.mode(PatternMode::Browse);
  rig.go(16, 0);
  rig.shift_tap(Button::Note);
  CHECK(rig.view().row() == 0);
  CHECK_FALSE(rig.edit().selection.active);
}

TEST_CASE("OS-SELSHIFT: the selected rows are an ordinary range: Delete clears them, and the cursor row is not cleared") {
  Rig rig;
  rig.edit_mode();
  for (int r = 0; r < 24; ++r) {
    put_note(rig, 0, r, 30u + static_cast<unsigned>(r));
  }
  rig.go(16, 0);
  rig.shift_tap(Button::Note);  // selects 0..15
  NEED(rig.edit().selection.active);
  (void)rig.invoke("step.clear");
  for (int r = 0; r < 16; ++r) {
    CHECK(rig.step(0, r).note == 0);
  }
  CHECK(rig.step(0, 16).note == 46);
}

TEST_CASE("OS-SELSHIFT fold: after a downward jump Shift + Down grows the block by one row, not two; after an upward jump Shift + Up grows it by one") {
  Rig rig(true, 8);  // the quarters are 0, 2, 4, 6
  rig.edit_mode();
  const auto block = [&] { return rig.edit().selection.block(rig.view().row(), rig.view().track()); };
  rig.go(0, 0);
  rig.press(Button::Shift);  // Shift stays down: letting it go ends the block's extending
  rig.pass(1);
  rig.tap(Button::Instr);  // row 0 -> 2 selects 0..1 (two rows)
  NEED(rig.view().row() == 2);
  app::Selection::Block b = block();
  CHECK((b.row0 == 0 && b.row1 == 1));
  rig.tap(Button::Down);  // the real key: one row more, 0..2
  CHECK(rig.view().row() == 3);
  b = block();
  CHECK((b.row0 == 0 && b.row1 == 2));
  (void)rig.invoke("select.extend", 1);  // and again: 0..3
  b = block();
  CHECK((b.row0 == 0 && b.row1 == 3));
  CHECK(rig.view().row() == 4);
  (void)rig.invoke("select.extend", -1);  // Shift + Up goes on from the cursor as the arrows do
  (void)rig.invoke("select.extend", -1);
  b = block();
  CHECK((b.row0 == 0 && b.row1 == 2));
  rig.release(Button::Shift);
  rig.pass(1);
  // up: row 4 -> 2 selects 2..3, and Shift + Up grows it to 1..3
  rig.go(4, 0);
  (void)rig.invoke("select.jump", 1);
  NEED(rig.view().row() == 2);
  b = block();
  CHECK((b.row0 == 2 && b.row1 == 3));
  (void)rig.invoke("select.extend", -1);
  b = block();
  CHECK((b.row0 == 1 && b.row1 == 3));
}

TEST_CASE("OS-SELSHIFT fold: a block made by arrows is not touched by the lag, and a second jump replaces a lagging block") {
  Rig rig(true, 8);
  rig.edit_mode();
  const auto block = [&] { return rig.edit().selection.block(rig.view().row(), rig.view().track()); };
  rig.go(0, 0);
  (void)rig.invoke("select.extend", 1);
  (void)rig.invoke("select.extend", 1);  // plain Shift + Down x2: rows 0..2
  app::Selection::Block b = block();
  CHECK((b.row0 == 0 && b.row1 == 2));
  CHECK_FALSE(rig.edit().selection.end_lags);
  rig.go(0, 0);
  rig.shift_tap(Button::Instr);  // 0..1, cursor 2
  CHECK(rig.edit().selection.end_lags);
  rig.shift_tap(Button::Vol);  // row 2 -> 4 selects 2..3, replacing the block
  b = block();
  CHECK((b.row0 == 2 && b.row1 == 3));
  CHECK(rig.view().row() == 4);
  (void)rig.invoke("select.clear");
  CHECK_FALSE(rig.edit().selection.end_lags);
}

TEST_CASE("OS-SELSHIFT fold: follow mode grows by one row; a 1-row pattern selects nothing; at the last row Shift + Down leaves the block alone") {
  {  // the cursor rides the playhead (STEP, playing): the jump's block keeps, the arrows set its end
    Rig rig(true, 16);
    rig.edit_mode();
    audio::PlayPosition pos;
    rig.app->attach_play_position(&pos);
    pos.publish(true, 1, 4, 12000, audio::PlayPosition::kNoOrder);
    rig.pass(1);
    NEED(rig.state().play.playing);
    NEED(rig.view().row() == 4);
    (void)rig.invoke("select.jump", 3);  // 4 -> 12 selects 4..11
    NEED(rig.edit().selection.active);
    app::Selection::Block b = rig.edit().selection.block(rig.view().row(), rig.view().track());
    CHECK((b.row0 == 4 && b.row1 == 11));
    (void)rig.invoke("select.extend", 1);
    b = rig.edit().selection.block(rig.view().row(), rig.view().track());
    CHECK((b.row0 == 4 && b.row1 == 12));
    // the transport stops with the cursor where the jump left it: the follow block is dropped, Shift + Down starts a fresh block and grows it by one row (no stale lag)
    (void)rig.invoke("select.jump", 3);
    pos.publish(false, 1, 12, 12000, audio::PlayPosition::kNoOrder);
    rig.pass(1);
    NEED(!rig.state().play.playing);
    const int stopped_row = rig.view().row();
    NEED(stopped_row == 12);  // (the end sits one row above the cursor: the case the lag would otherwise match)
    (void)rig.invoke("select.extend", 1);
    b = rig.edit().selection.block(rig.view().row(), rig.view().track());
    CHECK((b.row0 == stopped_row && b.row1 == stopped_row + 1));
    CHECK_FALSE(rig.edit().selection.end_lags);
    rig.app->attach_document(nullptr);  // (a take is open while it plays in Edit: the rig destroys its editor before the app)
  }
  {  // one row: every quarter is row 0, nothing moves, nothing is selected
    Rig rig(true, 1);
    rig.edit_mode();
    for (int q = 0; q < 4; ++q) {
      (void)rig.invoke("select.jump", q);
      CHECK(rig.view().row() == 0);
      CHECK_FALSE(rig.edit().selection.active);
    }
  }
  {  // 4 rows: 0 -> 3 selects 0..2; Shift + Down at the last row cannot move, so the block stays
    Rig rig(true, 4);
    rig.edit_mode();
    rig.go(0, 0);
    (void)rig.invoke("select.jump", 3);
    NEED(rig.view().row() == 3);
    (void)rig.invoke("select.extend", 1);
    app::Selection::Block b = rig.edit().selection.block(rig.view().row(), rig.view().track());
    CHECK((b.row0 == 0 && b.row1 == 2));
    CHECK(rig.view().row() == 3);
  }
}

// ---- OS-SHIFTENC: Shift + encoder is a coarse step in the Instr, Vol and FX value columns, a page in Browse, and in the choosers and the Fill dialog -----------------------------------------

TEST_CASE("OS-SHIFTENC: Browse pages with Shift + encoder (16 rows a detent), clamps at the first and the last row, and the plain turn is still one row") {
  Rig rig;
  rig.shift_turn(1);
  CHECK(rig.view().row() == 16);
  rig.shift_turn(2);
  CHECK(rig.view().row() == 48);
  rig.shift_turn(1);
  CHECK(rig.view().row() == 63);  // the last row of 64: clamped, no wrap
  rig.shift_turn(1);
  CHECK(rig.view().row() == 63);
  rig.shift_turn(-100);
  CHECK(rig.view().row() == 0);
  rig.shift_turn(-1);
  CHECK(rig.view().row() == 0);
  rig.turn(1);
  CHECK(rig.view().row() == 1);
  CHECK(rig.depth() == 0u);
}

TEST_CASE("OS-SHIFTENC: Shift + encoder in the Instr column walks eight places of the order a detent, clamped at both ends; the plain turn is still one") {
  Rig rig;
  rig.edit_mode();
  (void)rig.invoke("step.note", 50);
  rig.go(0);
  rig.column(Column::Instr);
  const auto at = [&](unsigned instrument) {
    (void)rig.invoke("step.instrument", static_cast<ActionArg>(instrument));
    rig.go(0);
    rig.column(Column::Instr);
  };
  at(1);  // the order is 64..49, then 1..48: instrument 1 is place 16
  rig.shift_turn(1);
  CHECK(rig.step(0, 0).instr == 9);
  rig.shift_turn(-1);
  CHECK(rig.step(0, 0).instr == 1);
  rig.shift_turn(-1);
  CHECK(rig.step(0, 0).instr == 56);  // 8 places down from 1 is into the MIDI channels: place 8
  rig.shift_turn(-1);
  CHECK(rig.step(0, 0).instr == 64);  // clamped at the start of the order
  rig.shift_turn(-1);
  CHECK(rig.step(0, 0).instr == 64);
  rig.shift_turn(100);
  CHECK(rig.step(0, 0).instr == 48);  // clamped at the end
  rig.shift_turn(1);
  CHECK(rig.step(0, 0).instr == 48);
  rig.turn(-1);
  CHECK(rig.step(0, 0).instr == 47);  // unchanged: the plain turn is one place
  CHECK(rig.state().instrument == 47);  // the last used instrument follows
  (void)rig.invoke("edit.undo");
  CHECK(rig.step(0, 0).instr == 48);
}

TEST_CASE("OS-SHIFTENC: Shift + encoder in the Vol column is ten a detent, clamped at 0 and 127") {
  Rig rig;
  rig.edit_mode();
  (void)rig.invoke("step.note", 50);
  rig.go(0);
  rig.column(Column::Vol);
  (void)rig.invoke("step.vol", 100);
  rig.go(0);
  rig.column(Column::Vol);
  CHECK(rig.step(0, 0).vol == 100);
  rig.shift_turn(1);
  CHECK(rig.step(0, 0).vol == 110);
  rig.shift_turn(-2);
  CHECK(rig.step(0, 0).vol == 90);
  rig.shift_turn(100);
  CHECK(rig.step(0, 0).vol == model::kVolumeMax);
  rig.shift_turn(1);
  CHECK(rig.step(0, 0).vol == model::kVolumeMax);
  rig.shift_turn(-100);
  CHECK(rig.step(0, 0).vol == 0);
  rig.shift_turn(-1);
  CHECK(rig.step(0, 0).vol == 0);
  rig.turn(1);
  CHECK(rig.step(0, 0).vol == 1);  // the plain turn is one
}

TEST_CASE("OS-SHIFTENC: Shift + encoder in the FX value column is sixteen a detent (one nibble), clamped to the type's range") {
  Rig rig;
  rig.edit_mode();
  (void)rig.invoke("step.note", 50);
  rig.go(0);
  rig.column(Column::Fx1);
  (void)rig.invoke("step.fx_type", 27);  // Low-pass
  const model::fx::Row& slot = model::fx::row(27);
  NEED(slot.hi >= 40 && slot.lo == 0);
  const auto at = [&](int value) {
    (void)rig.invoke("step.fx_value", value);
    rig.go(0);
    rig.column(Column::Fx1);
  };
  at(5);
  CHECK(rig.step(0, 0).fx[0].value == 5);
  rig.shift_turn(1);
  CHECK(rig.step(0, 0).fx[0].value == 21);
  rig.shift_turn(-1);
  CHECK(rig.step(0, 0).fx[0].value == 5);
  rig.shift_turn(-1);
  CHECK(rig.step(0, 0).fx[0].value == slot.lo);  // clamped at the bottom
  rig.shift_turn(100);
  CHECK(rig.step(0, 0).fx[0].value == slot.hi);  // and at the top
  rig.shift_turn(1);
  CHECK(rig.step(0, 0).fx[0].value == slot.hi);
  rig.turn(-1);
  CHECK(rig.step(0, 0).fx[0].value == slot.hi - 1);  // the plain turn is one
}

TEST_CASE("OS-SHIFTENC: the Note column keeps Shift + encoder as an octave") {
  Rig rig;
  rig.edit_mode();
  (void)rig.invoke("step.note", 50);
  rig.go(0);
  rig.shift_turn(1);
  CHECK(rig.step(0, 0).note == 62);
  rig.shift_turn(-2);
  CHECK(rig.step(0, 0).note == 38);
}

TEST_CASE("OS-SHIFTENC: Shift + encoder in the pattern, length and jump choosers is eight places a detent, clamped to each one's range") {
  Rig rig(true, 64);
  rig.hold_key(Button::F1);  // PATTERN
  rig.shift_turn(1);
  CHECK(rig.edit().pattern == 9);
  rig.shift_turn(100);
  CHECK(rig.edit().pattern == 255);
  rig.shift_turn(-100);
  CHECK(rig.edit().pattern == 1);
  rig.shift_turn(-1);
  CHECK(rig.edit().pattern == 1);
  rig.turn(1);
  CHECK(rig.edit().pattern == 2);  // the plain turn is one
  rig.shift_turn(-1);
  CHECK(rig.edit().pattern == 1);  // (back on the pattern that exists)
  rig.release_key(Button::F1);
  rig.tap(Button::F2);  // LENGTH
  rig.shift_turn(1);
  CHECK(rig.project().pattern_length(1) == 72);
  rig.shift_turn(100);
  CHECK(rig.project().pattern_length(1) == 128);
  rig.shift_turn(-100);
  CHECK(rig.project().pattern_length(1) == model::kMinPatternLength);
  rig.tap(Button::F2);
  rig.tap(Button::F3);  // JUMP
  rig.shift_turn(-100);
  CHECK(rig.edit().edit_step == 0);
  rig.shift_turn(1);
  CHECK(rig.edit().edit_step == 8);
  rig.shift_turn(100);
  CHECK(rig.edit().edit_step == model::edit::kMaxEditStep);
  rig.shift_turn(-100);
  CHECK(rig.edit().edit_step == 0);
}

TEST_CASE("OS-SHIFTENC: Shift + encoder in the Fill dialog is the row's coarse step; the enumerated rows keep their fine step") {
  Rig rig(true, 16);
  rig.edit_mode();
  rig.tap(Button::F4);
  fill_row(rig, kRowCount, 0);  // N 1..32, coarse 4
  rig.shift_turn(1);
  CHECK(rig.state().fill.plan.n == 5);
  rig.shift_turn(100);
  CHECK(rig.state().fill.plan.n == 32);  // clamped, no roll over
  rig.shift_turn(-100);
  CHECK(rig.state().fill.plan.n == 1);
  rig.shift_turn(-1);
  CHECK(rig.state().fill.plan.n == 1);
  fill_row(rig, kRowType, 1);  // RANDOM: row 4 is DENSITY
  CHECK(rig.state().fill.plan.type == model::fill::Type::Random);
  fill_row(rig, kRowCount, 0);
  rig.shift_turn(1);
  CHECK(rig.state().fill.plan.density == 40);  // 30 + 10
  rig.shift_turn(100);
  CHECK(rig.state().fill.plan.density == 100);
  fill_row(rig, kRowType, 0);
  rig.shift_turn(1);  // TYPE: an enumerated row, coarse 1: the next
  CHECK(rig.state().fill.plan.type == model::fill::Type::Euclid);
  // OS-FILL2: FROM and TO of a note are 12 a detent, KEY steps the root, MODE and FX TYPE keep their fine step.
  fill_row(rig, kRowKey, 0);
  rig.shift_turn(1);
  CHECK(rig.state().fill.plan.root == 1);
  CHECK(rig.state().fill.plan.scale == 0);
  fill_row(rig, kRowFrom, 0);
  const unsigned from = rig.state().fill.plan.from;
  rig.shift_turn(-1);
  CHECK(rig.state().fill.plan.from == from - 12);
  fill_row(rig, kRowMode, 0);
  rig.shift_turn(1);
  CHECK(rig.state().fill.plan.mode == model::fill::Mode::Ramp);
  rig.tap(Button::F8);
}

// ---- OS-FILL2: value modes, FROM and TO, FX ---------------------------------------------------------------------------------------------------

namespace {

// Every step of the first `rows` rows of every track, as bytes: what an undo must give back exactly.
std::string pattern_bytes(Rig& rig, int rows) {
  std::string s;
  for (int t = 0; t < 12; ++t) {
    for (int r = 0; r < rows; ++r) {
      const model::Step st = rig.step(t, r);
      s.append(reinterpret_cast<const char*>(&st), sizeof st);
    }
  }
  return s;
}

}  // namespace

TEST_CASE("OS-FILL2 Fill dialog: VOLUME RAMP fades the track out, the strip says mode and range, and one undo gives every byte back") {
  Rig rig(true, 16);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the track selected, so the fill is the track's
  const std::string before = pattern_bytes(rig, 16);
  const unsigned depth = rig.depth();
  rig.tap(Button::F4);
  fill_row(rig, kRowWhat, 2);   // VOLUME
  CHECK(rig.state().fill.plan.what == model::fill::What::Vol);
  CHECK((rig.state().fill.plan.from == 127 && rig.state().fill.plan.to == 0));  // the dialog opens with the fade: 127 down to 0
  fill_row(rig, kRowMode, 1);   // RAMP
  CHECK(rig.state().fill.plan.mode == model::fill::Mode::Ramp);
  rig.tap(Button::F1);
  CHECK(rig.message() == "FILL: EVERY 1 STEP, VOLUME RAMP 127-0");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.step(0, 0).vol == 127);
  CHECK(rig.step(0, 15).vol == 0);
  for (int r = 1; r < 16; ++r) {
    CHECK_MESSAGE(rig.step(0, r).vol < rig.step(0, r - 1).vol, "row " << r);
  }
  CHECK(rig.step(0, 0).note == model::kNoteEmpty);  // only the volume: no note on an empty step
  CHECK(rig.step(1, 0).vol == model::kVolumeUnset);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(pattern_bytes(rig, 16) == before);
  CHECK(rig.depth() == depth);
  // pattern.fill repeats it with the stored FROM and TO.
  rig.flush();
  (void)rig.invoke("pattern.fill");
  CHECK(rig.step(0, 0).vol == 127);
  CHECK(rig.step(0, 15).vol == 0);
}

TEST_CASE("OS-FILL2 Fill dialog: FX 1 RANDOM writes the FX type and a value inside FROM..TO, the same for the same seed, and undoes byte for byte") {
  std::string first;
  for (int pass = 0; pass < 2; ++pass) {
    Rig rig(true, 16);
    rig.edit_mode();
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the track selected, so the fill is the track's
    put(rig, 0, 3, note_at(50, 4));  // a note stays a note
    const std::string before = pattern_bytes(rig, 16);
    rig.tap(Button::F4);
    fill_row(rig, kRowWhat, 3);   // FX 1
    CHECK(rig.state().fill.plan.what == model::fill::What::Fx1);
    CHECK(rig.state().fill.plan.fx_type == 1);  // no FX under the cursor: the first picker type, Off
    fill_row(rig, kRowKey, 7);    // Off, Volume, Panning, Micro-tune, Glide, Tempo, Micro-move, Chance
    CHECK(rig.state().fill.plan.fx_type == 4);
    fill_row(rig, kRowMode, 2);   // RANDOM
    fill_row(rig, kRowTo, 0);
    rig.shift_turn(10);           // TO 100 (10 a detent; the draw of a Chance RANDOM stays in 0..100, OS-CECHANCE C2)
    CHECK((rig.state().fill.plan.from == 0 && rig.state().fill.plan.to == 100));
    rig.tap(Button::F1);
    CHECK(rig.message() == "FILL: EVERY 1 STEP, FX 1 RANDOM 0-100");
    std::string values;
    for (int r = 0; r < 16; ++r) {
      const model::Step st = rig.step(0, r);
      CHECK_MESSAGE(st.fx[0].type == 4, "row " << r);
      CHECK_MESSAGE(st.fx[0].value <= 100, "row " << r);
      CHECK(st.fx[1].type == 0);
      values += std::to_string(st.fx[0].value) + " ";
    }
    CHECK((rig.step(0, 3).note == 50 && rig.step(0, 3).instr == 4));
    CHECK(rig.step(0, 0).note == model::kNoteEmpty);
    if (pass == 0) {
      first = values;
    } else {
      CHECK(values == first);  // the generator is seeded the same in a new rig
    }
    CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
    CHECK(pattern_bytes(rig, 16) == before);
  }
}

TEST_CASE("OS-FILL2 Fill dialog: the first open takes FROM and TO from the cursor, a reopen keeps the session, a change of WHAT starts them again") {
  Rig rig(true, 16);
  rig.edit_mode();
  model::Step here = note_at(70, 3);  // OG note 69
  here.vol = 40;
  here.fx[1] = {17, 60};             // Swing in FX 2
  put(rig, 0, 0, here);
  rig.tap(Button::F4);
  CHECK((rig.state().fill.plan.from == 69 && rig.state().fill.plan.to == 80));  // the note, and an octave above it
  fill_row(rig, kRowWhat, 2);   // VOLUME
  CHECK((rig.state().fill.plan.from == 40 && rig.state().fill.plan.to == 0));
  fill_row(rig, kRowFrom, 0);
  rig.shift_turn(1);
  CHECK(rig.state().fill.plan.from == 50);
  rig.tap(Button::F8);
  rig.tap(Button::F4);          // OS-TOOLSDEDUP: the session keeps WHAT and the values (FROM stays 50, not the cursor's 40)
  CHECK(rig.state().fill.plan.what == model::fill::What::Vol);
  CHECK((rig.state().fill.plan.from == 50 && rig.state().fill.plan.to == 0));
  fill_row(rig, kRowWhat, 2);   // FX 2 (VOLUME + 2 places)
  CHECK(rig.state().fill.plan.what == model::fill::What::Fx2);
  CHECK(rig.state().fill.plan.fx_type == 17);   // the type under the cursor in that column, with its default
  CHECK((rig.state().fill.plan.from == 50 && rig.state().fill.plan.to == 50));
  rig.tap(Button::F8);
}

TEST_CASE("OS-FILL2 Fill dialog: a NOTE RAMP lands in the scale of KEY; MODE is dim for RANDOM and SCALE, whose window is FROM..TO") {
  Rig rig(true, 16);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the track selected, so the fill is the track's
  rig.tap(Button::F4);
  fill_row(rig, kRowMode, 1);   // RAMP
  fill_row(rig, kRowKey, 3);    // Major
  CHECK((rig.state().fill.plan.from == 60 && rig.state().fill.plan.to == 71));
  rig.tap(Button::F1);
  CHECK(rig.message() == "FILL: EVERY 1 STEP, NOTE RAMP C-5 TO B-5");
  CHECK(rig.step(0, 0).note == 61);   // C-5
  CHECK(rig.step(0, 15).note == 72);  // B-5
  for (int r = 0; r < 16; ++r) {
    const unsigned n = rig.step(0, r).note - 1u;
    CHECK_MESSAGE(core::is_in_scale(static_cast<std::uint8_t>(n), 0, 3), "row " << r);
  }
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  rig.flush();
  // TYPE RANDOM: MODE is dim and does not change; the window is FROM..TO, wider than an octave.
  rig.tap(Button::F4);
  fill_row(rig, kRowMode, -1);     // RAMP -> CONST
  CHECK(rig.state().fill.plan.mode == model::fill::Mode::Const);
  fill_row(rig, kRowType, 1);
  fill_row(rig, kRowCount, 100);   // DENSITY 100
  CHECK(rig.state().fill.plan.density == 100);
  fill_row(rig, kRowMode, 1);
  CHECK(rig.state().fill.plan.mode == model::fill::Mode::Const);  // the row is dim: the turn changed nothing
  fill_row(rig, kRowTo, 0);
  rig.shift_turn(2);               // TO + 24
  CHECK(rig.state().fill.plan.to == 95);
  rig.tap(Button::F1);
  CHECK(rig.message() == "FILL: RANDOM 100 %");
  int lo = 200;
  int hi = 0;
  for (int r = 0; r < 16; ++r) {
    const int n = static_cast<int>(rig.step(0, r).note) - 1;
    lo = n < lo ? n : lo;
    hi = n > hi ? n : hi;
  }
  CHECK(lo >= 60);
  CHECK(hi <= rig.state().fill.plan.to);
  CHECK(hi - lo > 11);
}

// ---- PAT-OPS-A: DUPLICATE x2, EXPAND x2 and SHRINK /2 in the TOOLS list ---------------------------------------------------------------

namespace {
// The verb menu (F7) pad of the length op `item` (1 DUPLICATE x2 = pad 10, 2 EXPAND x2 = pad 11, 3 SHRINK /2 = pad 12: the numbers of PAT-OPS-A).
// OS-TOOLSDEDUP: the fixed pads run them; the APPS page lists tool apps only.
void run_tool(Rig& rig, unsigned item) {
  rig.tap(Button::F7);
  NEED(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.pad(8u + item);
}

std::vector<unsigned> lengths_of(Rig& rig, unsigned id = 1) {
  std::vector<unsigned> v;
  const model::Pattern* p = rig.project().pattern(id);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    v.push_back(p != nullptr ? p->track_length[t] : 0u);
  }
  return v;
}

// Pattern 1 of 16 steps with notes on tracks 0, 1 and 3 (track 3 is 8 long), a kept step inside the doubled length (track 0, row 20) and one past it (row 60).
void fill_ops_pattern(Rig& rig) {
  (void)rig.project().set_track_length(1, 3, 8);
  put_note(rig, 0, 0, 50);
  put_note(rig, 0, 1, 51);
  put_note(rig, 0, 15, 52);
  put_note(rig, 1, 2, 53);
  put_note(rig, 3, 7, 54);
  put_note(rig, 0, 20, 99);
  put_note(rig, 0, 60, 98);
  rig.editor->history_reset();
}
}  // namespace

TEST_CASE("PAT-OPS-A: DUPLICATE x2 doubles every active track by its own length and repeats its steps; one undo unit; the strip says the longest length") {
  Rig rig(true, 16);
  rig.edit_mode();
  fill_ops_pattern(rig);
  const std::uint64_t before = pattern_digest(rig, false, false);
  const std::vector<unsigned> len_before = lengths_of(rig);
  run_tool(rig, 1);
  CHECK(rig.state().overlay == app::OverlayId::None);  // the list closed
  CHECK(rig.depth() == 1u);
  CHECK(rig.message() == "DUPLICATED: 32 STEPS");
  CHECK(rig.project().pattern(1)->track_length[0] == 32);
  CHECK(rig.project().pattern(1)->track_length[1] == 32);
  CHECK(rig.project().pattern(1)->track_length[3] == 16);  // 8 doubled
  CHECK(rig.project().pattern(1)->track_length[12] == 16);  // not an active track: left alone
  for (const auto& [row, note] : {std::pair<int, unsigned>{0, 50}, {1, 51}, {15, 52}, {16, 50}, {17, 51}, {31, 52}}) {
    const int r = row;  // plain locals: doctest's message lambda must not capture a structured binding (C++17)
    const unsigned n = note;
    CHECK_MESSAGE(rig.step(0, r).note == n, "track 0 row " << r);
  }
  CHECK(model::is_empty(rig.step(0, 20)));  // the kept step inside the new length is overwritten by the copy of row 4
  CHECK(rig.step(0, 60).note == 98);         // past both lengths: as it was
  CHECK(rig.step(1, 2).note == 53);
  CHECK(rig.step(1, 18).note == 53);
  CHECK(rig.step(3, 7).note == 54);
  CHECK(rig.step(3, 15).note == 54);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(pattern_digest(rig, false, false) == before);
  CHECK(lengths_of(rig) == len_before);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(rig.project().pattern(1)->track_length[0] == 32);
  CHECK(rig.step(0, 16).note == 50);
}

TEST_CASE("PAT-OPS-A: EXPAND x2 puts step i at 2i with an empty step after it; one undo unit") {
  Rig rig(true, 16);
  rig.edit_mode();
  fill_ops_pattern(rig);
  const std::uint64_t before = pattern_digest(rig, false, false);
  const std::vector<unsigned> len_before = lengths_of(rig);
  run_tool(rig, 2);
  CHECK(rig.depth() == 1u);
  CHECK(rig.message() == "EXPANDED: 32 STEPS");
  CHECK(rig.project().pattern(1)->track_length[0] == 32);
  CHECK(rig.project().pattern(1)->track_length[3] == 16);
  CHECK(rig.step(0, 0).note == 50);
  CHECK(rig.step(0, 2).note == 51);
  CHECK(rig.step(0, 30).note == 52);
  CHECK(model::is_empty(rig.step(0, 1)));
  CHECK(model::is_empty(rig.step(0, 3)));
  CHECK(model::is_empty(rig.step(0, 20)));  // the kept step was inside the new length: row 20 is step 10's place
  CHECK(rig.step(0, 60).note == 98);
  CHECK(rig.step(1, 4).note == 53);
  CHECK(rig.step(3, 14).note == 54);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(pattern_digest(rig, false, false) == before);
  CHECK(lengths_of(rig) == len_before);
}

TEST_CASE("PAT-OPS-A: SHRINK /2 keeps the even steps, clears down to the old length, leaves what is past both; shrink(expand(p)) is p; one undo unit each") {
  Rig rig(true, 16);
  rig.edit_mode();
  fill_ops_pattern(rig);
  const std::uint64_t before = pattern_digest(rig, false, false);
  const std::vector<unsigned> len_before = lengths_of(rig);
  run_tool(rig, 3);
  CHECK(rig.depth() == 1u);
  CHECK(rig.message() == "SHRUNK: 8 STEPS");
  CHECK(rig.project().pattern(1)->track_length[0] == 8);
  CHECK(rig.project().pattern(1)->track_length[3] == 4);
  CHECK(rig.step(0, 0).note == 50);       // step 0 stays
  CHECK(model::is_empty(rig.step(0, 1)));  // old step 1 (odd) is gone: the new step 1 is old step 2
  CHECK(model::is_empty(rig.step(0, 15)));  // the new length up to the old one is cleared
  CHECK(rig.step(0, 20).note == 99);        // past both lengths: as it was
  CHECK(rig.step(0, 60).note == 98);
  CHECK(rig.step(1, 1).note == 53);         // old step 2
  CHECK(model::is_empty(rig.step(3, 7)));   // cleared (old step 7 was odd, and past the new length 4)
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(pattern_digest(rig, false, false) == before);
  CHECK(lengths_of(rig) == len_before);

  // EXPAND then SHRINK: the pattern as it was (no kept step inside the doubled length here), in two units
  Rig rt(true, 16);
  rt.edit_mode();
  (void)rt.project().set_track_length(1, 3, 8);
  put_note(rt, 0, 0, 50);
  put_note(rt, 0, 15, 52);
  put_note(rt, 1, 2, 53);
  put_note(rt, 3, 7, 54);
  rt.editor->history_reset();
  const std::uint64_t pre = pattern_digest(rt, false, false);
  const std::vector<unsigned> len_pre = lengths_of(rt);
  run_tool(rt, 2);
  CHECK(pattern_digest(rt, false, false) != pre);
  run_tool(rt, 3);
  CHECK(pattern_digest(rt, false, false) == pre);
  CHECK(lengths_of(rt) == len_pre);
  CHECK(rt.depth() == 2u);
}

TEST_CASE("PAT-OPS-A: with a block only its tracks are changed, over their whole length, and the block ends; the cursor keeps its row") {
  Rig rig(true, 16);
  rig.edit_mode();
  fill_ops_pattern(rig);
  rig.go(3, 1);
  (void)rig.invoke("select.extend", 1);
  rig.extend_tracks(1);  // rows 3..4, tracks 1..2
  NEED(rig.edit().selection.active);
  const int row = rig.view().row();
  const int track = rig.view().track();
  run_tool(rig, 1);
  CHECK(rig.depth() == 1u);
  CHECK(rig.project().pattern(1)->track_length[0] == 16);  // not in the block
  CHECK(rig.project().pattern(1)->track_length[1] == 32);
  CHECK(rig.project().pattern(1)->track_length[2] == 32);
  CHECK(rig.project().pattern(1)->track_length[3] == 8);
  CHECK(rig.step(1, 18).note == 53);  // the whole track 1 repeated, not only the block's rows
  CHECK(model::is_empty(rig.step(0, 16)));
  CHECK_FALSE(rig.edit().selection.active);
  CHECK(rig.view().row() == row);  // (the cursor is where the extension left it: at the block's far corner)
  CHECK(rig.view().track() == track);
}

TEST_CASE("PAT-OPS-A: a result over 128 steps is refused with a message and nothing is written; SHRINK is never refused; a block can leave the long track out") {
  Rig rig(true, 65);
  rig.edit_mode();
  put_note(rig, 0, 64, 77);
  rig.editor->history_reset();
  const std::uint64_t before = pattern_digest(rig, false, false);
  const std::vector<unsigned> len_before = lengths_of(rig);
  run_tool(rig, 1);
  CHECK(rig.message() == "TOO LONG: 128 STEPS AT MOST");
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.depth() == 0u);
  CHECK(pattern_digest(rig, false, false) == before);
  CHECK(lengths_of(rig) == len_before);
  rig.flush();
  run_tool(rig, 2);
  CHECK(rig.message() == "TOO LONG: 128 STEPS AT MOST");
  CHECK(rig.depth() == 0u);
  CHECK(lengths_of(rig) == len_before);
  rig.flush();
  run_tool(rig, 3);
  CHECK(rig.message() == "SHRUNK: 33 STEPS");  // ceil(65 / 2)
  CHECK(rig.project().pattern(1)->track_length[0] == 33);
  CHECK(rig.depth() == 1u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  // 64 doubles to exactly 128; one track of 70 among them refuses the whole pattern, but not a block that leaves it out
  Rig mix(true, 64);
  mix.edit_mode();
  (void)mix.project().set_track_length(1, 5, 70);
  mix.editor->history_reset();
  run_tool(mix, 1);
  CHECK(mix.message() == "TOO LONG: 128 STEPS AT MOST");
  CHECK(mix.depth() == 0u);
  mix.flush();
  mix.go(0, 0);
  mix.extend_tracks(1);  // tracks 0..1 (whole steps)
  run_tool(mix, 1);
  CHECK(mix.message() == "DUPLICATED: 128 STEPS");
  CHECK(mix.project().pattern(1)->track_length[0] == 128);
  CHECK(mix.project().pattern(1)->track_length[5] == 70);
}

TEST_CASE("PAT-OPS-A: with Edit off the entries say so and write nothing; a pattern the project lacks says PATTERN IS EMPTY; SHRINK of one step has nothing to change") {
  Rig off(true, 16);  // Browse
  fill_ops_pattern(off);
  const std::uint64_t before = pattern_digest(off, false, false);
  run_tool(off, 1);
  CHECK(off.state().overlay == app::OverlayId::None);
  CHECK(off.message() == "TURN EDIT ON");
  CHECK(off.depth() == 0u);
  CHECK(pattern_digest(off, false, false) == before);
  CHECK(off.project().pattern(1)->track_length[0] == 16);
  CHECK(off.invoke("pattern.op", 0) == DispatchResult::Disabled);  // (the action itself is Disabled outside Edit, as a paste)

  Rig none(true, 0);
  none.edit_mode();
  run_tool(none, 2);
  CHECK(none.message() == "PATTERN IS EMPTY");
  CHECK(none.project().pattern(1) == nullptr);
  CHECK(none.depth() == 0u);

  Rig one(true, 1);
  one.edit_mode();
  run_tool(one, 3);
  CHECK(one.message() == "NOTHING TO CHANGE");
  CHECK(one.depth() == 0u);
}

TEST_CASE("PAT-OPS-A: SHRINK clamps the cursor to the new length; a cursor inside it stays on its row") {
  Rig rig(true, 64);
  rig.edit_mode();
  rig.go(40);
  run_tool(rig, 3);
  CHECK(rig.project().pattern_length(1) == 32);
  CHECK(rig.view().row() == 31);
  rig.flush();
  rig.go(10);
  run_tool(rig, 1);
  CHECK(rig.project().pattern_length(1) == 64);
  CHECK(rig.view().row() == 10);
}

// ---- PAT-OPS-B: the whole-pattern clipboard on F1 held ------------------------------------------------------------------------------------

namespace {
// Pattern `id` of `length` steps with a few notes (the track lengths differ), and a name.
void make_pattern(Rig& rig, unsigned id, unsigned length, const char* name, unsigned base) {
  (void)rig.project().create_pattern(id, length);
  (void)rig.project().set_track_length(id, 2, length / 2u);
  (void)rig.project().set_pattern_name(id, name, std::strlen(name));
  for (unsigned t : {0u, 2u, 15u}) {
    model::Step s = model::kEmptyStep;
    s.note = static_cast<std::uint8_t>(base + t);
    s.instr = 1;
    s.fx[0] = {5, static_cast<std::uint8_t>(t)};
    (void)rig.project().set_steps(id, t, 3, &s, 1);
  }
}

bool same_content(const model::Pattern& a, const model::Pattern& b) {
  return std::memcmp(a.steps, b.steps, sizeof a.steps) == 0 && std::memcmp(a.track_length, b.track_length, sizeof a.track_length) == 0 && std::strcmp(a.name, b.name) == 0;
}
}  // namespace

TEST_CASE("PAT-OPS-B: Copy with F1 held takes the chooser's pattern whole, Shift + Copy writes steps, lengths and name over another; one undo unit; the source stays") {
  Rig rig(true, 24);
  make_pattern(rig, 5, 40, "intro", 60);
  (void)rig.project().set_pattern_name(1, "old", 3);
  put_note(rig, 0, 0, 40);
  rig.edit_mode();
  rig.editor->history_reset();
  rig.hold_key(Button::F1);
  rig.pad(4);  // pattern 5
  CHECK(rig.edit().pattern == 5);
  rig.tap(Button::Copy);
  CHECK(rig.depth() == 0u);  // a copy changes nothing
  rig.pad(0);                // pattern 1
  NEED(rig.edit().pattern == 1);
  const model::Pattern old = *rig.project().pattern(1);
  rig.press(Button::Shift);
  rig.pass(1);
  rig.tap(Button::Copy);
  rig.release(Button::Shift);
  rig.pass(1);
  rig.release_key(Button::F1);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.depth() == 1u);
  CHECK(same_content(*rig.project().pattern(1), *rig.project().pattern(5)));
  CHECK(rig.project().pattern(1)->track_length[2] == 20);
  CHECK(rig.project().pattern(1)->track_length[0] == 40);
  CHECK(std::string(rig.project().pattern(1)->name) == "intro");
  CHECK(rig.step(2, 3).note == 62);
  CHECK(rig.step(15, 3).fx[0].value == 15);  // a track past the active ones is carried too
  CHECK(model::is_empty(rig.step(0, 0)));    // the old note is gone
  CHECK(rig.message() == "PATTERN 01 REPLACED BY 05");
  CHECK(rig.project().pattern(5)->track_length[0] == 40);  // the source is untouched
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(std::memcmp(rig.project().pattern(1), &old, sizeof old) == 0);  // byte for byte
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(same_content(*rig.project().pattern(1), *rig.project().pattern(5)));
}

TEST_CASE("PAT-OPS-B: the strip says PATTERN nn COPIED; a pattern the project lacks has nothing to copy; a second paste of the same clip changes nothing") {
  Rig rig(true, 24);
  make_pattern(rig, 5, 40, "intro", 60);
  rig.edit_mode();
  rig.hold_key(Button::F1);
  rig.pad(4);
  rig.tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "PATTERN 05 COPIED");
  rig.flush();
  rig.hold_key(Button::F1);
  rig.pad(8);  // pattern 9: not in the project
  rig.tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "NOTHING TO COPY");
  rig.flush();
  // the earlier clip is still there: it goes over pattern 5 itself (same content) and changes nothing
  rig.hold_key(Button::F1);
  rig.pad(4);
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "NOTHING TO CHANGE");
  CHECK(rig.depth() == 0u);
}

TEST_CASE("PAT-OPS-B: the paste needs Edit; the copy does not; with no whole clip it says so") {
  Rig rig(true, 24);
  make_pattern(rig, 5, 40, "intro", 60);
  rig.editor->history_reset();
  const model::Pattern before = *rig.project().pattern(1);
  rig.hold_key(Button::F1);  // Browse
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "TURN EDIT ON");  // (said before the clip is looked at)
  rig.flush();
  rig.hold_key(Button::F1);
  rig.pad(4);
  rig.tap(Button::Copy);  // works in Browse
  rig.release_key(Button::F1);
  CHECK(rig.message() == "PATTERN 05 COPIED");
  rig.flush();
  rig.hold_key(Button::F1);
  rig.pad(0);
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "TURN EDIT ON");
  CHECK(std::memcmp(rig.project().pattern(1), &before, sizeof before) == 0);
  CHECK(rig.depth() == 0u);
  // Edit on, a block clip of the grid (not a whole pattern): the paste of the chooser has nothing to write
  rig.flush();
  rig.edit_mode();
  rig.go(0, 0);
  rig.tap(Button::Copy);
  CHECK(rig.message() == "COPIED 1 STEP");
  rig.flush();
  rig.hold_key(Button::F1);
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "NO PATTERN COPIED");
  CHECK(rig.depth() == 0u);
}

TEST_CASE("PAT-OPS-B: a paste to a pattern the project lacks makes it in the same unit; undoing it is structural: the Song history goes, with a notice") {
  Rig rig(true, 24, true);
  make_pattern(rig, 5, 40, "intro", 60);
  rig.edit_mode();
  rig.editor->begin_group(core::action_id("song.turn"), 0, model::UndoArea::Song);  // something in the Song ring, which names patterns too
  (void)rig.editor->append_order(5);
  rig.editor->end_group();
  rig.editor->history_reset();
  rig.editor->begin_group(core::action_id("song.turn"), 0, model::UndoArea::Song);
  (void)rig.editor->append_order(5);
  rig.editor->end_group();
  NEED(rig.editor->journal(model::UndoArea::Song).depth() == 1u);
  rig.hold_key(Button::F1);
  rig.pad(4);
  rig.tap(Button::Copy);
  rig.pad(6);  // pattern 7: not in the project
  NEED(rig.edit().pattern == 7);
  NEED(rig.project().pattern(7) == nullptr);
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  NEED(rig.project().pattern(7) != nullptr);
  CHECK(same_content(*rig.project().pattern(7), *rig.project().pattern(5)));
  CHECK(rig.depth() == 1u);  // the pattern and its contents: one unit
  CHECK(rig.message() == "PATTERN 07 REPLACED BY 05");
  rig.flush();
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.project().pattern(7) == nullptr);
  CHECK(rig.message() == "SONG HISTORY CLEARED");
  CHECK(rig.editor->journal(model::UndoArea::Song).depth() == 0u);
  rig.app->attach_document(nullptr);
}

TEST_CASE("PAT-OPS-B: the LENGTH and JUMP choosers share the context and ignore Copy and Shift + Copy") {
  Rig rig(true, 24);
  make_pattern(rig, 5, 40, "intro", 60);
  rig.edit_mode();
  rig.editor->history_reset();
  const model::Pattern before = *rig.project().pattern(1);
  rig.tap(Button::F2);  // LENGTH, opened by a tap
  NEED(rig.state().overlay == app::OverlayId::LengthChooser);
  rig.tap(Button::Copy);
  rig.shift_tap(Button::Copy);
  CHECK(rig.state().overlay == app::OverlayId::LengthChooser);
  CHECK(rig.message() == "");
  rig.tap(Button::F2);
  rig.tap(Button::F3);  // JUMP
  NEED(rig.state().overlay == app::OverlayId::JumpChooser);
  rig.tap(Button::Copy);
  rig.shift_tap(Button::Copy);
  CHECK(rig.state().overlay == app::OverlayId::JumpChooser);
  rig.tap(Button::F3);
  CHECK(rig.depth() == 0u);
  CHECK(std::memcmp(rig.project().pattern(1), &before, sizeof before) == 0);
  // the Copy of the choosers took nothing: the grid's Shift + Copy has nothing to paste
  rig.flush();
  rig.go(0, 0);
  rig.shift_tap(Button::Copy);
  CHECK(rig.message() == "NOTHING TO PASTE");
  // the actions themselves do nothing outside the PATTERN chooser
  CHECK(rig.invoke("pattern.clip_copy") != DispatchResult::Changed);
  CHECK(rig.invoke("pattern.clip_paste") != DispatchResult::Changed);
}

TEST_CASE("PAT-OPS-B: in the grid, Shift + Copy of a whole clip pastes it as a block (steps only, at the cursor, clipped); the grid's Copy makes a block clip again") {
  Rig rig(true, 24);
  make_pattern(rig, 5, 40, "intro", 60);
  rig.edit_mode();
  rig.hold_key(Button::F1);
  rig.pad(4);
  rig.tap(Button::Copy);
  rig.pad(0);
  rig.release_key(Button::F1);
  rig.editor->history_reset();
  rig.go(2, 1);
  rig.shift_tap(Button::Copy);
  CHECK(rig.depth() == 1u);
  CHECK(rig.step(1, 5).note == 60);   // the clip's track 0 row 3 lands on the cursor's track 1, row 2 + 3
  CHECK(rig.step(3, 5).note == 62);   // clip track 2
  CHECK(rig.project().pattern(1)->track_length[0] == 24);  // lengths and name stay the target's
  CHECK(rig.project().pattern(1)->track_length[2] == 24);
  CHECK(std::string(rig.project().pattern(1)->name).empty());
  rig.tap(Button::Copy);  // the grid's Copy: a block clip of one step
  rig.flush();
  rig.hold_key(Button::F1);
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "NO PATTERN COPIED");
}

TEST_CASE("PAT-OPS-B: a paste over the pattern that plays is allowed: the lengths change at once, in one undo unit") {
  Rig rig(true, 32);
  make_pattern(rig, 5, 16, "short", 70);
  rig.editor->history_reset();
  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  pos.publish(true, 1, 20, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  rig.tap(Button::Rec);  // STEP, playing: the cursor and the chooser stay where the user puts them
  NEED(rig.state().play.playing);
  NEED(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Edit));
  rig.hold_key(Button::F1);
  rig.pad(4);
  rig.tap(Button::Copy);
  rig.pad(0);
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.depth() == 1u);
  CHECK(rig.project().pattern(1)->track_length[0] == 16);
  CHECK(rig.project().pattern(1)->track_length[2] == 8);
  CHECK(std::string(rig.project().pattern(1)->name) == "short");
  CHECK(rig.step(0, 3).note == 70);
  rig.app->attach_document(nullptr);  // (a LIVE take may be open: the rig destroys its editor before the app)
}

namespace {
// A transport that takes a pick of the held chooser (a switch) and says which patterns the app asked the sequencer to release.
struct ReleaseLog final : app::TransportSink {
  unsigned releases = 0, last = 0;
  bool play(unsigned) noexcept override { return true; }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
  bool switch_pattern(unsigned) noexcept override { return true; }
  bool release_pattern(unsigned id) noexcept override {
    ++releases;
    last = id;
    return true;
  }
};
}  // namespace

// OS-BUG-PASTEHANG: the paste takes the OFF rows of the pattern that plays, so the notes that sound would hang until Stop; the app asks the sequencer to end them at the next step (as a switch does).
TEST_CASE("PAT-OPS-B: a paste over the pattern that plays asks the sequencer to end its notes; a stopped transport, a paste that changes nothing and a refusal ask nothing") {
  Rig rig(true, 32);
  make_pattern(rig, 5, 16, "short", 70);
  rig.editor->history_reset();
  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  ReleaseLog log;
  rig.app->attach_transport(&log);
  const auto paste_over_1 = [&rig]() {
    rig.hold_key(Button::F1);
    rig.pad(4);
    rig.tap(Button::Copy);
    rig.pad(0);
    rig.shift_tap(Button::Copy);
    rig.release_key(Button::F1);
  };
  // stopped: the paste is made and the sequencer is not asked
  rig.edit_mode();
  paste_over_1();
  CHECK(rig.depth() == 1u);
  CHECK(log.releases == 0u);
  rig.invoke("edit.undo");
  // playing: the paste is made and the pattern that plays is named once
  pos.publish(true, 1, 20, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  NEED(rig.state().play.playing);
  NEED(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Edit));
  paste_over_1();
  CHECK(rig.depth() == 1u);
  CHECK(rig.step(0, 3).note == 70);
  CHECK(log.releases == 1u);
  CHECK(log.last == 1u);
  // the same clip again changes nothing: nothing is asked
  paste_over_1();
  CHECK(rig.message() == "NOTHING TO CHANGE");
  CHECK(log.releases == 1u);
  // a paste that only changes the name changes nothing the sequencer plays: nothing is asked
  (void)rig.project().set_pattern_name(1, "other", 5);
  paste_over_1();
  CHECK(rig.message() == "PATTERN 01 REPLACED BY 05");
  CHECK(std::string(rig.project().pattern(1)->name) == "short");
  CHECK(log.releases == 1u);
  rig.app->attach_document(nullptr);  // (a LIVE take may be open: the rig destroys its editor before the app)
}

TEST_CASE("OS-QAFIX-E-SONG-06: the Pattern button on Song opens the pattern of the slot under the Song cursor") {
  Rig rig;
  for (unsigned id : {2u, 3u}) {
    CHECK(rig.project().create_pattern(id, 16) == model::Status::Ok);
  }
  CHECK(rig.project().append_order(3) == model::Status::Ok);  // slots: 1, 3, 2
  CHECK(rig.project().append_order(2) == model::Status::Ok);
  CHECK(rig.edit().pattern == 1);
  rig.tap(Button::Song);
  CHECK(rig.state().screen == app::ScreenId::Song);
  app::ViewState& vs = const_cast<app::ViewState&>(rig.state());
  CHECK(vs.song.set(1, 3));  // the slot with pattern 3
  rig.pass(1);
  rig.tap(Button::Pattern);
  CHECK(rig.state().screen == app::ScreenId::Pattern);
  CHECK(rig.edit().pattern == 3);
  rig.tap(Button::Song);
  CHECK(vs.song.set(2, 3));
  rig.pass(1);
  rig.tap(Button::Pattern);
  CHECK(rig.edit().pattern == 2);
  rig.tap(Button::Song);
  CHECK(vs.song.set(0, 3));
  rig.pass(1);
  rig.tap(Button::Pattern);
  CHECK(rig.edit().pattern == 1);
  // Not from Song: Pattern from another screen leaves the view where it was.
  rig.tap(Button::Song);
  CHECK(vs.song.set(1, 3));
  rig.pass(1);
  rig.tap(Button::File);
  rig.tap(Button::Pattern);
  CHECK(rig.state().screen == app::ScreenId::Pattern);
  CHECK(rig.edit().pattern == 1);
}

TEST_CASE("OS-QAFIX-E-SONG-06: with an empty song list the Pattern button on Song changes nothing in the view") {
  Rig rig;
  CHECK(rig.project().create_pattern(2, 16) == model::Status::Ok);
  CHECK(rig.project().append_order(2) == model::Status::Ok);  // slots: 1, 2
  // pattern 2 is open in the view first (not the default 1), so that "stays" proves something
  rig.tap(Button::Song);
  app::ViewState& vs = const_cast<app::ViewState&>(rig.state());
  CHECK(vs.song.set(1, 2));
  rig.pass(1);
  rig.tap(Button::Pattern);
  NEED(rig.edit().pattern == 2);
  rig.go(5, 1);
  const unsigned row = rig.view().row();
  const unsigned track = rig.view().track();
  CHECK(rig.project().clear_order() == model::Status::Ok);
  rig.tap(Button::Song);
  CHECK(rig.state().screen == app::ScreenId::Song);
  rig.pass(1);
  rig.tap(Button::Pattern);
  CHECK(rig.state().screen == app::ScreenId::Pattern);
  CHECK(rig.edit().pattern == 2);  // (no slot, so no pattern to open: it stays the one that was open)
  CHECK(rig.view().row() == row);
  CHECK(rig.view().track() == track);
}

TEST_CASE("OS-QAFIX-E-SONG-06: the block of the old pattern is cleared when Song opens another pattern") {
  Rig rig;
  CHECK(rig.project().create_pattern(3, 16) == model::Status::Ok);
  CHECK(rig.project().append_order(3) == model::Status::Ok);  // slots: 1, 3
  rig.go(5);
  (void)rig.invoke("select.extend", 1);
  NEED(rig.edit().selection.active);
  rig.tap(Button::Song);
  app::ViewState& vs = const_cast<app::ViewState&>(rig.state());
  CHECK(vs.song.set(1, 2));
  rig.pass(1);
  rig.tap(Button::Pattern);
  CHECK(rig.edit().pattern == 3);
  CHECK_FALSE(rig.edit().selection.active);  // (its rows belonged to pattern 1)
  // The same slot again: the pattern does not change, so a block made now stays.
  (void)rig.invoke("select.extend", 1);
  NEED(rig.edit().selection.active);
  rig.tap(Button::Song);
  rig.pass(1);
  rig.tap(Button::Pattern);
  CHECK(rig.edit().pattern == 3);
  CHECK(rig.edit().selection.active);
}

TEST_CASE("OS-QAFIX-E-SONG-06: in WATCH while it plays, the follow pulls the view back to the playing pattern at the next step") {
  Rig rig;
  CHECK(rig.project().create_pattern(2, 16) == model::Status::Ok);
  CHECK(rig.project().create_pattern(3, 16) == model::Status::Ok);
  CHECK(rig.project().append_order(3) == model::Status::Ok);  // slots: 1, 3
  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  rig.tap(Button::Song);
  app::ViewState& vs = const_cast<app::ViewState&>(rig.state());
  CHECK(vs.song.set(1, 2));
  rig.pass(1);
  rig.tap(Button::Pattern);
  NEED(rig.edit().pattern == 3);  // (stopped: the cursor's slot)
  NEED(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Browse));  // WATCH: the mode that follows the playhead
  pos.publish(true, 1, 4, 12000, audio::PlayPosition::kNoOrder);  // pattern 1 plays, step 4
  rig.pass(1);
  NEED(rig.state().play.playing);
  CHECK(rig.edit().pattern == 1);  // the follow: the view goes back to the pattern that plays
  CHECK(rig.view().row() == 4);
  pos.publish(true, 1, 5, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  CHECK(rig.edit().pattern == 1);
  CHECK(rig.view().row() == 5);
  rig.app->attach_document(nullptr);
}

// ---- OS-AUDIT-56: the column-level block (new Community OS behaviour) -----------------------------------------------------------------------

namespace {
// A full step (every column set) whose bytes say where it is: `k` makes the note, the instrument, the volume and both FX distinct.
model::Step full_step(unsigned k) {
  model::Step s = model::kEmptyStep;
  s.note = static_cast<std::uint8_t>(40u + k);
  s.instr = static_cast<std::uint8_t>(1u + k % 8u);
  s.vol = static_cast<std::uint8_t>(10u + k);
  s.fx[0] = model::StepFx{1, static_cast<std::uint8_t>(20u + k)};
  s.fx[1] = model::StepFx{2, static_cast<std::uint8_t>(30u + k)};
  return s;
}
void put_full(Rig& rig, int track, int row, unsigned k) {
  const model::Step s = full_step(k);
  (void)rig.project().set_steps(1, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(row), &s, 1);
}
bool same_step(const model::Step& a, const model::Step& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
constexpr ActionArg kDown = 1;
constexpr ActionArg kRight = 10;
constexpr ActionArg kLeft = -10;
}  // namespace

TEST_CASE("OS-AUDIT-56: a block keeps the columns from where it began to the cursor's column inside one track; across tracks it is whole steps") {
  Rig rig;
  rig.go(2, 0);
  rig.column(Column::Vol);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("select.extend", kDown);
  const app::Selection& sel = rig.edit().selection;
  app::Selection::Block b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.row0 == 2 && b.row1 == 4 && b.track0 == 0 && b.track1 == 0 && b.col0 == 2 && b.col1 == 2));  // Shift + Down alone: the one column it began in
  CHECK_FALSE(b.whole_steps());
  (void)rig.invoke("select.extend", kRight);  // Vol -> Fx1
  b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.col0 == 2 && b.col1 == 3 && b.track0 == 0 && b.track1 == 0));
  (void)rig.invoke("select.extend", kLeft);   // back to Vol
  (void)rig.invoke("select.extend", kLeft);   // Instr: the range grows to the left of the start column
  b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.col0 == 1 && b.col1 == 2));
  (void)rig.invoke("select.extend", kRight);
  (void)rig.invoke("select.extend", kRight);
  (void)rig.invoke("select.extend", kRight);  // Fx2
  b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.col0 == 2 && b.col1 == 4 && b.track1 == 0));
  (void)rig.invoke("select.extend", kRight);  // past the track's last column: the Note column of track 1, so the block is whole steps of tracks 0 and 1
  CHECK(rig.view().track() == 1);
  b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.track0 == 0 && b.track1 == 1 && b.whole_steps()));
  (void)rig.invoke("select.extend", kRight);  // across tracks the arrows step by whole tracks: track 2, not the next column
  CHECK((rig.view().track() == 2 && rig.view().column() == Column::Note));
  b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.track0 == 0 && b.track1 == 2 && b.whole_steps()));
  (void)rig.invoke("select.extend", kLeft);
  CHECK(rig.view().track() == 1);
  (void)rig.invoke("select.extend", kLeft);  // back in track 0 (at its Note column): the columns count again, from the start column
  CHECK(rig.view().track() == 0);
  b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.track1 == 0 && b.col0 == 0 && b.col1 == 2));
  (void)rig.invoke("select.extend", kRight);  // and a column at a time again
  CHECK(rig.view().column() == Column::Instr);
  b = sel.block(rig.view().row(), rig.view().track());
  CHECK((b.col0 == 1 && b.col1 == 2));
  // Shift + Up at step 0 (OS-SELCOL): the cursor's column first, then the whole track as whole steps
  Rig col;
  col.go(0, 1);
  col.column(Column::Fx1);
  (void)col.invoke("select.extend", -kDown);
  app::Selection::Block c = col.edit().selection.block(col.view().row(), col.view().track());
  CHECK((c.track0 == 1 && c.track1 == 1 && c.col0 == 3 && c.col1 == 3 && !c.whole_steps()));
  (void)col.invoke("select.extend", -kDown);
  c = col.edit().selection.block(col.view().row(), col.view().track());
  CHECK((c.track0 == 1 && c.track1 == 1 && c.whole_steps()));
}

TEST_CASE("OS-AUDIT-56: Copy of a column block and Shift + Copy write those columns only, in place; the target's other columns stay; one undo unit") {
  Rig rig(true, 32);
  rig.edit_mode();
  for (int r = 0; r < 3; ++r) {
    put_full(rig, 0, 2 + r, static_cast<unsigned>(r));
    put_full(rig, 2, 10 + r, static_cast<unsigned>(50 + r));
  }
  rig.editor->history_reset();
  rig.go(2, 0);
  rig.column(Column::Vol);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("select.extend", kRight);  // Vol..Fx1, rows 2..4
  const unsigned depth = rig.depth();
  rig.tap(Button::Copy);
  CHECK(rig.message() == "COPIED 3 STEPS");
  rig.flush();
  rig.go(10, 2);  // (the cursor's column is Note: the copied columns land where they were)
  rig.shift_tap(Button::Copy);
  for (int r = 0; r < 3; ++r) {
    const model::Step& t = rig.step(2, 10 + r);
    const model::Step want = full_step(static_cast<unsigned>(50 + r));
    const model::Step from = full_step(static_cast<unsigned>(r));
    CHECK_MESSAGE((t.note == want.note && t.instr == want.instr), "note and instrument stay, row " << r);
    CHECK_MESSAGE(t.vol == from.vol, "volume pasted, row " << r);
    CHECK_MESSAGE((t.fx[0].type == from.fx[0].type && t.fx[0].value == from.fx[0].value), "FX1 pasted, row " << r);
    CHECK_MESSAGE((t.fx[1].type == want.fx[1].type && t.fx[1].value == want.fx[1].value), "FX2 stays, row " << r);
  }
  CHECK(rig.depth() == depth + 1u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.depth() == depth);
  CHECK(rig.step(2, 10).vol == full_step(50).vol);
  // a copy without a block is whole steps again, and so is its paste
  rig.go(2, 0);
  rig.tap(Button::Copy);
  rig.flush();
  rig.go(20, 3);
  rig.shift_tap(Button::Copy);
  CHECK(same_step(rig.step(3, 20), rig.step(0, 2)));
  // pasting the same columns again changes nothing: no undo unit
  rig.go(2, 0);
  rig.column(Column::Fx1);
  (void)rig.invoke("select.extend", kDown);
  rig.tap(Button::Copy);
  rig.flush();
  rig.go(2, 0);
  const unsigned before = rig.depth();
  rig.shift_tap(Button::Copy);
  CHECK(rig.depth() == before);
}

TEST_CASE("OS-AUDIT-56: Delete clears the columns of the block; a whole-step block clears everything; one undo unit") {
  Rig rig(true, 32);
  rig.edit_mode();
  for (int t = 0; t < 2; ++t) {
    for (int r = 0; r < 4; ++r) {
      put_full(rig, t, r, static_cast<unsigned>(t * 10 + r));
    }
  }
  rig.editor->history_reset();
  // FX1..FX2 of rows 0..2 of track 0
  rig.go(0, 0);
  rig.column(Column::Fx1);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("select.extend", kRight);
  rig.tap(Button::Delete);
  CHECK(rig.depth() == 1u);
  for (int r = 0; r < 3; ++r) {
    const model::Step& s = rig.step(0, r);
    CHECK_MESSAGE((s.fx[0].type == 0 && s.fx[1].type == 0 && s.fx[0].value == 0 && s.fx[1].value == 0), "row " << r);
    CHECK_MESSAGE((s.note == full_step(static_cast<unsigned>(r)).note && s.vol == full_step(static_cast<unsigned>(r)).vol), "note and volume stay, row " << r);
  }
  CHECK(rig.step(0, 3).fx[0].type == 1);  // below the block
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(rig.depth() == 0u);
  CHECK(rig.step(0, 0).fx[1].type == 2);
  // Note..Instr: the pair goes, the rest stays
  rig.go(1, 1);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("select.extend", kRight);
  rig.tap(Button::Delete);
  CHECK((rig.step(1, 1).note == 0 && rig.step(1, 1).instr == 0 && rig.step(1, 2).note == 0));
  CHECK((rig.step(1, 1).vol == full_step(11).vol && rig.step(1, 2).fx[1].type == 2));
  // a block over two tracks is whole steps, whatever column the cursor is on
  rig.go(0, 0);
  rig.column(Column::Vol);
  for (int i = 0; i < 4; ++i) {
    (void)rig.invoke("select.extend", kRight);  // Fx1, Fx2, then the Note column of track 1: whole steps
  }
  rig.tap(Button::Delete);
  CHECK(model::is_empty(rig.step(0, 0)));
  CHECK(model::is_empty(rig.step(1, 0)));
  CHECK_FALSE(model::is_empty(rig.step(0, 1)));  // the block was one row
}

TEST_CASE("OS-AUDIT-56: Invert, Insert and Pull move the block's columns only") {
  Rig rig(true, 32);
  rig.edit_mode();
  for (int r = 0; r < 4; ++r) {
    put_full(rig, 0, r, static_cast<unsigned>(r));
  }
  rig.editor->history_reset();
  rig.go(0, 0);
  rig.column(Column::Vol);
  for (int i = 0; i < 3; ++i) {
    (void)rig.invoke("select.extend", kDown);
  }
  (void)rig.invoke("pattern.invert");  // INVERT (no key since OS-PTOOLS2 S3): the volumes of rows 0..3 reversed, the notes where they were
  for (int r = 0; r < 4; ++r) {
    CHECK_MESSAGE(rig.step(0, r).vol == full_step(static_cast<unsigned>(3 - r)).vol, "row " << r);
    CHECK_MESSAGE(rig.step(0, r).note == full_step(static_cast<unsigned>(r)).note, "row " << r);
  }
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  // INSERT at row 1 (a block of one row, Vol..Fx1): those columns move down from row 1, the others stay; PULL gives them back
  rig.go(1, 0);
  rig.column(Column::Vol);
  (void)rig.invoke("select.extend", kRight);
  rig.tap(Button::Insert);
  CHECK(rig.step(0, 1).vol == model::kVolumeUnset);
  CHECK(rig.step(0, 1).fx[0].type == 0);
  CHECK(rig.step(0, 2).vol == full_step(1).vol);
  CHECK(rig.step(0, 1).note == full_step(1).note);  // the note column did not move
  CHECK(rig.step(0, 2).note == full_step(2).note);
  rig.shift_tap(Button::Delete);  // Pull
  for (int r = 0; r < 4; ++r) {
    CHECK_MESSAGE(same_step(rig.step(0, r), full_step(static_cast<unsigned>(r))), "row " << r);
  }
  // Note and Instr are one pair: a block over the Instr column alone moves both
  (void)rig.invoke("select.clear");
  rig.go(1, 0);
  rig.column(Column::Instr);
  (void)rig.invoke("select.extend", kDown);
  rig.tap(Button::Insert);
  CHECK((rig.step(0, 1).note == 0 && rig.step(0, 1).instr == 0));
  CHECK((rig.step(0, 2).note == full_step(1).note && rig.step(0, 2).instr == full_step(1).instr));
  CHECK(rig.step(0, 2).vol == full_step(2).vol);  // (the volume did not move)
}

TEST_CASE("OS-AUDIT-56: DUPLICATE and a fill read a column block as whole steps, and the strip says WHOLE STEPS") {
  Rig rig(true, 32);
  rig.edit_mode();
  for (int r = 0; r < 4; ++r) {
    put_full(rig, 0, r, static_cast<unsigned>(r));
  }
  rig.editor->history_reset();
  // A Vol-only block of track 0: DUPLICATE doubles the track with its whole steps, not the Vol column alone
  rig.go(0, 0);
  rig.column(Column::Vol);
  (void)rig.invoke("select.extend", kDown);
  run_tool(rig, 1);
  CHECK(rig.message() == "DUPLICATED: 64 STEPS - WHOLE STEPS");
  CHECK(rig.project().pattern(1)->track_length[0] == 64);
  for (int r = 0; r < 4; ++r) {
    CHECK_MESSAGE(same_step(rig.step(0, 32 + r), full_step(static_cast<unsigned>(r))), "row " << r);
  }
  CHECK(rig.edit().selection.active == false);
  // A whole-step block says nothing of the kind
  rig.flush();
  rig.go(0, 0);
  rig.column(Column::Note);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("select.extend", kRight);
  (void)rig.invoke("select.extend", kRight);
  (void)rig.invoke("select.extend", kRight);
  (void)rig.invoke("select.extend", kRight);
  (void)rig.invoke("select.extend", kRight);  // Note..FX2 and then the next track: whole steps
  run_tool(rig, 1);
  CHECK(rig.message().find("WHOLE STEPS") == std::string::npos);
  // The pattern.fill of a Vol-only block (its steps whole, the note column filled): the strip says so
  rig.flush();
  (void)rig.invoke("select.clear");
  rig.go(0, 0);
  rig.column(Column::Vol);
  (void)rig.invoke("select.extend", kDown);
  (void)rig.invoke("pattern.fill");
  CHECK(rig.message().rfind("FILL: WHOLE STEPS, ", 0) == 0);
}

// ---- OS-PTOOLS2 S1: Fill keeps its rows, F2 VARY; the TOOLS list's row, the block hint -----------------------------------------------------------------------------------

namespace {
model::Step tool_step(unsigned note, unsigned instr, unsigned vol) {
  model::Step s = model::kEmptyStep;
  s.note = static_cast<std::uint8_t>(note);
  s.instr = static_cast<std::uint8_t>(instr);
  s.vol = static_cast<std::uint8_t>(vol);
  return s;
}
void put_step(Rig& rig, int track, int row, const model::Step& s) {
  (void)rig.project().set_steps(1, static_cast<std::uint32_t>(track), static_cast<std::uint32_t>(row), &s, 1);
}
}  // namespace

TEST_CASE("OS-TOOLSDEDUP: Fill reopened in CONST mode keeps FROM; the dialog keeps every row for the session") {
  Rig rig(true, 16);
  rig.edit_mode();
  rig.tap(Button::F4);
  fill_row(rig, kRowWhat, 2);    // VOLUME
  fill_row(rig, kRowType, 2);    // EUCLID
  fill_row(rig, kRowCount, -1);  // EVENTS 3
  fill_row(rig, kRowMode, 1);    // RAMP
  fill_row(rig, kRowTo, 10);     // TO 10
  const model::fill::Plan before = rig.state().fill.plan;
  CHECK(before.what == model::fill::What::Vol);
  CHECK(before.mode == model::fill::Mode::Ramp);
  rig.tap(Button::F8);
  rig.tap(Button::F4);
  const model::fill::Plan& again = rig.state().fill.plan;
  CHECK(again.what == before.what);
  CHECK(again.type == before.type);
  CHECK(again.events == before.events);
  CHECK(again.mode == before.mode);
  CHECK(again.from == before.from);
  CHECK(again.to == before.to);  // (QA-PAT-309 said: back to the cursor's at every open)
  // OS-TOOLSDEDUP: CONST keeps FROM too, whatever volume the cursor holds at the reopen; every other row stays
  fill_row(rig, kRowMode, -1);
  CHECK(rig.state().fill.plan.mode == model::fill::Mode::Const);
  rig.tap(Button::F8);
  put_step(rig, 0, 5, tool_step(61, 1, 0x20));
  rig.go(5, 0);
  rig.tap(Button::F4);
  CHECK(rig.state().fill.plan.mode == model::fill::Mode::Const);
  CHECK(rig.state().fill.plan.from == before.from);
  CHECK(rig.state().fill.plan.from != 0x20);
  CHECK(rig.state().fill.plan.to == before.to);
  CHECK(rig.state().fill.plan.events == before.events);
  rig.tap(Button::F8);
}

TEST_CASE("OS-PTOOLS2 S1: F2 VARY in a random fill takes back the dialog's last fill and draws again, one unit; it does not touch anything else, and is not live in a fixed fill") {
  Rig rig(true, 32);
  rig.edit_mode();
  (void)rig.invoke("select.column_all");  // OS-PTOOLS2 S3c: the track selected (32 steps), so the fill is the track's
  const unsigned d0 = rig.depth();
  rig.tap(Button::F4);
  fill_row(rig, kRowType, 1);  // RANDOM, DENSITY 30
  CHECK(app::fill_random(rig.state().fill));
  rig.tap(Button::F1);
  CHECK(rig.depth() == d0 + 1);
  const std::string first = column_of(rig, 0, 32);
  rig.flush();
  rig.tap(Button::F4);
  rig.tap(Button::F2);
  CHECK(rig.state().overlay == app::OverlayId::Fill);  // VARY keeps the dialog up: F2 again re-rolls
  CHECK(rig.depth() == d0 + 1);  // the first fill was taken back: still one unit
  CHECK(column_of(rig, 0, 32) != first);  // the generator moved on
  rig.tap(Button::F8);  // BACK closes the dialog (VARY left it up)
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(column_of(rig, 0, 32) == std::string(32, '.'));  // one undo takes the whole thing: nothing of the first fill is left
  CHECK(rig.depth() == d0);
  // an edit in between: VARY adds to it, it never undoes what it did not make
  rig.flush();
  rig.tap(Button::F4);
  rig.tap(Button::F1);
  const unsigned d1 = rig.depth();
  put_step(rig, 0, 31, tool_step(70, 1, 0x10));
  CHECK(rig.invoke("step.note", 50) == DispatchResult::Changed);  // a unit of the editor over the same pattern
  const unsigned d2 = rig.depth();
  rig.flush();
  rig.tap(Button::F4);
  rig.tap(Button::F2);
  CHECK(rig.depth() == d2 + 1);
  (void)d1;
  // a fixed fill: F2 is not live, and does nothing
  rig.flush();
  rig.tap(Button::F4);
  fill_row(rig, kRowType, -1);  // EVERY N
  CHECK_FALSE(app::fill_random(rig.state().fill));
  const unsigned d3 = rig.depth();
  rig.tap(Button::F2);
  CHECK(rig.state().overlay == app::OverlayId::Fill);
  CHECK(rig.depth() == d3);
  rig.tap(Button::F8);
}

// ---- OS-PTOOLS2 S1: TOOLS while playing; the block hint -------------------------------------------------------------------------------------------------------------------

TEST_CASE("OS-PTOOLS2 S1: F7 in WATCH while playing with a block opens the tools, on the row they were left on; LIVE refuses and says why") {
  Rig rig(true, 16);
  audio::PlayPosition pos;
  rig.app->attach_play_position(&pos);
  pos.publish(true, 1, 4, 12000, audio::PlayPosition::kNoOrder);
  rig.pass(1);
  NEED(rig.state().play.playing);
  NEED(rig.state().mode[kPattern] == static_cast<std::uint8_t>(PatternMode::Browse));  // WATCH
  for (int i = 0; i < 3; ++i) (void)rig.invoke("select.extend", kDown);  // a block made while following keeps (OS-BUG-WATCHSEL)
  NEED(rig.state().edit.selection.active);
  rig.tap(Button::F7);
  NEED(rig.state().overlay == app::OverlayId::VerbMenu);  // the menu opens in WATCH while playing (S1)
  rig.pad(15);  // APPS > opens the TOOLS list
  NEED(rig.state().overlay == app::OverlayId::VerbMenu);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::F8);
  rig.tap(Button::F7);
  rig.pad(15);
  CHECK(rig.state().overlay == app::OverlayId::VerbMenu);
  CHECK(rig.state().modules.tools == 0);  // (no tool app in this rig: the page is empty, the row stays inside it; the remembered row is tested with apps in module_edit_test)
  rig.tap(Button::F8);
  rig.shift_tap(Button::Rec);  // LIVE
  NEED(app::pattern_editable(rig.state()));
  CHECK_FALSE(rig.edit().rec_free);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "THE PLAYHEAD HOLDS THE CURSOR IN LIVE");
  rig.app->attach_document(nullptr);  // (a LIVE take is open: the rig destroys its editor before the app)
}

// OS-MIDIREC-P1 S2 fold: SHRINK / DUPLICATE and the whole-pattern paste change the track lengths, and a length cut that takes more lane events than the undo ring holds is refused (Full). The whole operation is checked
// before the first write: nothing is half done (no cleared tail with a kept length, no track changed while another is not) and the strip asks for the length chooser.
TEST_CASE("OS-MIDIREC-P1: SHRINK and a whole-pattern paste whose lane cut does not fit the undo ring change nothing, and say so") {
  static model::LaneStore store;
  static model::LaneEvent big[4096];
  for (std::uint32_t i = 0; i < 4096; ++i) {
    big[i] = model::LaneEvent{};
    big[i].tick = static_cast<std::uint16_t>(3100u + (i * 2u) / 3u);  // past the cut of both operations (64 steps, 16 steps)
    big[i].len = 1;
    big[i].a = static_cast<std::uint8_t>(i & 0x7F);
    big[i].b = 100;
  }
  Rig rig(true, 128, true, 96u * 1024u);  // a 40 KiB Pattern ring: the two lanes (61 KB of records) do not fit
  make_pattern(rig, 5, 16, "short", 70);
  put_note(rig, 0, 100, 77);  // a step both operations would clear
  put_note(rig, 1, 90, 78);
  store.clear();
  store.adopt(rig.project().load_epoch());
  rig.editor->attach_lanes(&store);
  NEED(rig.editor->lane_replace(1, 0, 0, 6144, big, 4096) == model::Status::Ok);
  NEED(rig.editor->lane_replace(1, 1, 0, 6144, big, 3584) == model::Status::Ok);
  rig.edit_mode();
  rig.editor->history_reset();
  const std::uint64_t digest = pattern_digest(rig, false, false);
  const std::vector<unsigned> lengths = lengths_of(rig);

  run_tool(rig, 3);  // SHRINK /2
  CHECK(rig.message() == "TOO BIG TO UNDO: USE THE LENGTH CHOOSER");
  CHECK(rig.depth() == 0u);
  CHECK(pattern_digest(rig, false, false) == digest);
  CHECK(lengths_of(rig) == lengths);
  CHECK(store.lane(1, 0).count == 4096u);
  CHECK(store.lane(1, 1).count == 3584u);
  rig.flush();

  rig.hold_key(Button::F1);  // the whole pattern 5 over pattern 1: lengths 16 and 8 cut every lane event
  rig.pad(4);
  rig.tap(Button::Copy);
  rig.pad(0);
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "TOO BIG TO UNDO: USE THE LENGTH CHOOSER");
  CHECK(rig.depth() == 0u);
  CHECK(pattern_digest(rig, false, false) == digest);
  CHECK(lengths_of(rig) == lengths);
  CHECK(std::string(rig.project().pattern(1)->name).empty());
  CHECK(store.lane(1, 0).count == 4096u);
  CHECK(store.lane(1, 1).count == 3584u);
  rig.app->attach_document(nullptr);
}

// ---- OS-MIDIREC-P1 S7c: the lane events follow block Copy / Paste / Delete, row Insert / Pull and the whole-pattern clip (A16, A17; QA-PAT-497..) ---------------------------------------------

namespace {

model::LaneStore g_s7c;  // static: the store is large

model::LaneEvent lane_cc(unsigned tick, unsigned value) {
  model::LaneEvent e{};
  e.tick = static_cast<std::uint16_t>(tick);
  e.kind_channel = static_cast<std::uint8_t>(1u << 4);
  e.a = 1;
  e.b = static_cast<std::uint8_t>(value);
  return e;
}

// `n` events spread over the first `span` ticks.
std::vector<model::LaneEvent> lane_spread(unsigned n, unsigned span) {
  std::vector<model::LaneEvent> v;
  for (unsigned i = 0; i < n; ++i) {
    v.push_back(lane_cc(i * span / n, i & 0x7Fu));
  }
  return v;
}

// "tick:value tick:value ..." of one lane of pattern 1 (or `pattern`).
std::string lane_text(unsigned track, unsigned pattern = 1) {
  std::string s;
  const model::LaneStore::View v = g_s7c.lane(pattern, track);
  for (std::uint32_t i = 0; i < v.count; ++i) {
    s += (i != 0 ? " " : "") + std::to_string(v.events[i].tick) + ":" + std::to_string(v.events[i].b);
  }
  return s;
}

// The lanes of tracks 0..3 of pattern 1 in one string, to compare before and after.
std::string lanes_now() {
  std::string s;
  for (unsigned t = 0; t < 4; ++t) {
    s += "[" + lane_text(t) + "]";
  }
  return s;
}

void lanes_on(Rig& rig) {
  g_s7c.clear();
  g_s7c.adopt(rig.project().load_epoch());
  rig.editor->attach_lanes(&g_s7c);
  rig.app->attach_document(rig.editor.get());  // (the model's lane pointer: render() refreshes it, a Copy before the first frame needs it)
}

// The lane of (pattern, track) over its whole length (`t1` ticks).
bool lane_set(Rig& rig, unsigned track, const std::vector<model::LaneEvent>& ev, unsigned t1 = 768, unsigned pattern = 1) {
  return rig.editor->lane_replace(pattern, track, 0, t1, ev.data(), static_cast<std::uint32_t>(ev.size())) == model::Status::Ok;
}

// A block of `rows` rows from `row` over `tracks` tracks from `track`: whole steps once it spans two tracks.
void select_block(Rig& rig, int row, int track, int rows, int tracks) {
  rig.go(row, track);
  for (int i = 1; i < rows; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  if (tracks > 1) {
    rig.extend_tracks(tracks - 1);
  }
}

}  // namespace

TEST_CASE("OS-MIDIREC-P1 S7c: block Copy and Paste carry the lane events of the block's tracks and rows, the paste replaces the target's lane range, one undo unit, undo and redo restore it") {
  Rig rig(true, 16);
  lanes_on(rig);
  NEED(lane_set(rig, 0, {lane_cc(5, 1), lane_cc(60, 2), lane_cc(150, 3)}));  // step 3 is outside the block
  NEED(lane_set(rig, 1, {lane_cc(90, 4)}));
  NEED(lane_set(rig, 2, {lane_cc(8 * 48 + 20, 9), lane_cc(12 * 48, 10)}));  // the first is in the paste range, the second after it
  put_note(rig, 0, 0, 50);
  put_note(rig, 1, 1, 52);
  rig.edit_mode();
  rig.editor->history_reset();
  select_block(rig, 0, 0, 2, 2);
  rig.tap(Button::Copy);
  CHECK(rig.message().find("COPIED") == 0);
  CHECK(rig.depth() == 0u);  // a copy changes nothing
  rig.flush();
  (void)rig.invoke("select.clear");
  rig.go(8, 2);
  CHECK(rig.invoke("block.paste") == DispatchResult::Changed);
  CHECK(rig.depth() == 1u);
  CHECK(lane_text(2) == "389:1 444:2 576:10");  // rows 8..9 of track 2: the clip's, the old event at tick 404 gone, the one at 576 kept
  CHECK(lane_text(3) == "474:4");
  CHECK(lane_text(0) == "5:1 60:2 150:3");  // the source stays
  CHECK(lane_text(1) == "90:4");
  CHECK(rig.step(2, 8).note == 50);
  CHECK(rig.step(3, 9).note == 52);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // one unit: steps and lanes
  CHECK(lane_text(2) == "404:9 576:10");
  CHECK(lane_text(3) == "");
  CHECK(model::is_empty(rig.step(2, 8)));
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(lane_text(2) == "389:1 444:2 576:10");
  CHECK(lane_text(3) == "474:4");
  CHECK(rig.step(2, 8).note == 50);
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7c: a paste that would not fit the lane pool is refused whole with a strip notice, steps and lanes unchanged; with room it goes") {
  Rig rig(true, 128);
  lanes_on(rig);
  NEED(lane_set(rig, 0, lane_spread(50, 96), 6144));
  NEED(lane_set(rig, 1, lane_spread(50, 96), 6144));
  NEED(lane_set(rig, 8, lane_spread(3750, 6144), 6144));
  NEED(lane_set(rig, 9, lane_spread(3750, 6144), 6144));  // 7,600 of 7,680 events: 80 free
  put_note(rig, 0, 0, 50);
  rig.edit_mode();
  rig.editor->history_reset();
  select_block(rig, 0, 0, 2, 2);
  rig.tap(Button::Copy);  // 100 lane events in the clip
  rig.flush();
  (void)rig.invoke("select.clear");
  rig.go(8, 2);
  const std::uint32_t total = g_s7c.total_events();
  CHECK(total == 7600u);
  (void)rig.invoke("block.paste");
  CHECK(rig.message() == "LANES DO NOT FIT: NOTHING CHANGED");
  CHECK(rig.depth() == 0u);
  CHECK(model::is_empty(rig.step(2, 8)));
  CHECK(g_s7c.total_events() == total);
  CHECK(g_s7c.lane(1, 2).count == 0u);
  CHECK(g_s7c.lane(1, 3).count == 0u);
  rig.flush();
  NEED(rig.editor->lane_clear(1, 9) == model::Status::Ok);  // room for them
  rig.editor->history_reset();
  CHECK(rig.invoke("block.paste") == DispatchResult::Changed);
  CHECK(g_s7c.lane(1, 2).count == 50u);
  CHECK(g_s7c.lane(1, 3).count == 50u);
  CHECK(rig.step(2, 8).note == 50);
  CHECK(rig.depth() == 1u);
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7c: cut (Copy then Delete) takes the lane events of the block's rows with the steps, one undo unit; the cut events paste back") {
  Rig rig(true, 16);
  lanes_on(rig);
  NEED(lane_set(rig, 0, {lane_cc(5, 1), lane_cc(60, 2), lane_cc(150, 3)}));
  NEED(lane_set(rig, 1, {lane_cc(90, 4)}));
  NEED(lane_set(rig, 2, {lane_cc(60, 5)}));  // a track outside the block
  put_note(rig, 0, 0, 50);
  rig.edit_mode();
  rig.editor->history_reset();
  select_block(rig, 0, 0, 2, 2);
  rig.tap(Button::Copy);
  rig.flush();
  CHECK(rig.invoke("step.clear") == DispatchResult::Changed);
  CHECK(rig.depth() == 1u);
  CHECK(lane_text(0) == "150:3");  // rows 0..1 are ticks 0..95
  CHECK(lane_text(1) == "");
  CHECK(lane_text(2) == "60:5");
  CHECK(model::is_empty(rig.step(0, 0)));
  rig.flush();
  (void)rig.invoke("select.clear");
  rig.go(8, 4);
  CHECK(rig.invoke("block.paste") == DispatchResult::Changed);
  CHECK(lane_text(4) == "389:1 444:2");
  CHECK(lane_text(5) == "474:4");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the paste
  CHECK(lane_text(4) == "");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);  // the cut
  CHECK(lane_text(0) == "5:1 60:2 150:3");
  CHECK(lane_text(1) == "90:4");
  CHECK(rig.step(0, 0).note == 50);
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7c: a column block carries no lanes, a block of more than kLaneCapacity (1,024) lane events gives them up with the strip saying so; the paste leaves the target's lanes alone") {
  Rig rig(true, 16);
  lanes_on(rig);
  NEED(lane_set(rig, 0, {lane_cc(5, 1), lane_cc(60, 2)}));
  NEED(lane_set(rig, 1, {lane_cc(400, 7)}));
  put_note(rig, 0, 0, 50);
  rig.edit_mode();
  rig.editor->history_reset();
  // A Note-column block of one track: not whole steps.
  rig.go(0, 0);
  (void)rig.invoke("select.extend", 1);
  NEED(!rig.state().edit.selection.block(rig.view().row(), rig.view().track()).whole_steps());
  rig.tap(Button::Copy);
  rig.flush();
  (void)rig.invoke("select.clear");
  rig.go(8, 1);
  CHECK(rig.invoke("block.paste") == DispatchResult::Changed);
  CHECK(lane_text(1) == "400:7");  // untouched
  CHECK(lane_text(0) == "5:1 60:2");
  rig.flush();
  CHECK(rig.invoke("step.clear") == DispatchResult::Changed);  // the step under the cursor, a column block earlier: lanes stay
  CHECK(lane_text(1) == "400:7");
  // Too many: 2 x 1,500 events across the whole pattern.
  NEED(lane_set(rig, 0, lane_spread(1500, 768)));
  NEED(lane_set(rig, 1, lane_spread(1500, 768)));
  NEED(lane_set(rig, 2, {lane_cc(100, 5)}));
  select_block(rig, 0, 0, 16, 2);
  rig.tap(Button::Copy);
  CHECK(rig.message().find("NO LANES: TOO MANY") != std::string::npos);
  rig.flush();
  (void)rig.invoke("select.clear");
  rig.go(0, 2);
  const unsigned depth = rig.depth();
  CHECK(rig.invoke("block.paste") == DispatchResult::Changed);
  CHECK(rig.depth() == depth + 1u);
  CHECK(lane_text(2) == "100:5");  // the clip has no lanes: the target's stay
  CHECK(g_s7c.lane(1, 3).count == 0u);
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7c: row Insert and Pull move the lane ticks with the rows in the steps' undo unit, what Insert pushes past the track length is deleted (A17)") {
  Rig rig(true, 16);
  lanes_on(rig);
  const auto seed = [&] {
    NEED(lane_set(rig, 0, {lane_cc(5, 1), lane_cc(99, 2), lane_cc(480, 3)}));
    NEED(lane_set(rig, 1, {lane_cc(677, 4), lane_cc(721, 6)}));  // 721 is on step 15: an Insert pushes it out
    NEED(lane_set(rig, 2, {lane_cc(100, 5)}));                    // a track outside the block
    rig.editor->history_reset();
  };
  seed();
  put_note(rig, 0, 2, 50);
  rig.edit_mode();
  rig.editor->history_reset();
  const std::string before = lanes_now();
  select_block(rig, 2, 0, 1, 2);
  CHECK(rig.invoke("step.insert") == DispatchResult::Changed);
  CHECK(rig.depth() == 1u);
  CHECK(lane_text(0) == "5:1 147:2 528:3");
  CHECK(lane_text(1) == "725:4");  // 721 + 48 = 769 is past the 16 steps
  CHECK(lane_text(2) == "100:5");
  CHECK(rig.step(0, 3).note == 50);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(lanes_now() == before);
  CHECK(rig.step(0, 2).note == 50);
  rig.flush();
  select_block(rig, 2, 0, 1, 2);
  CHECK(rig.invoke("step.pull") == DispatchResult::Changed);
  CHECK(lane_text(0) == "5:1 432:3");  // the event on step 2 goes, the later ones move up
  CHECK(lane_text(1) == "629:4 673:6");
  CHECK(lane_text(2) == "100:5");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(lanes_now() == before);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(lane_text(0) == "5:1 432:3");
  rig.app->attach_document(nullptr);
}

// ---- OS-MIDIREC-P1 S7d: EXPAND / SHRINK scale the lane ticks with the rows, TRANSPOSE moves the lane notes (A16, A17; QA-PAT-527..) -----------------------------------------------------

TEST_CASE("OS-MIDIREC-P1 S7d: EXPAND doubles the lane ticks with the rows and SHRINK halves them, in the steps' one undo unit; an event past the new end is deleted and undo restores everything") {
  Rig rig(true, 16);
  lanes_on(rig);
  NEED(lane_set(rig, 0, {lane_cc(5, 1), lane_cc(99, 2), lane_cc(480, 3)}));
  NEED(lane_set(rig, 1, {lane_cc(677, 4)}));
  NEED(lane_set(rig, 2, {lane_cc(100, 5)}));
  put_note(rig, 0, 3, 50);
  rig.edit_mode();
  rig.editor->history_reset();
  const std::string before = lanes_now();
  const std::vector<unsigned> len0 = lengths_of(rig);
  CHECK(rig.invoke("pattern.op", 1) == DispatchResult::Changed);  // EXPAND: 16 -> 32 steps on every track
  CHECK(rig.depth() == 1u);
  CHECK(lane_text(0) == "10:1 198:2 960:3");
  CHECK(lane_text(1) == "1354:4");
  CHECK(lane_text(2) == "200:5");
  CHECK(rig.project().pattern(1)->track_length[0] == 32u);
  CHECK(rig.step(0, 6).note == 50);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(lanes_now() == before);
  CHECK(lengths_of(rig) == len0);
  CHECK(rig.step(0, 3).note == 50);
  rig.flush();
  CHECK(rig.invoke("pattern.op", 2) == DispatchResult::Changed);  // SHRINK: 16 -> 8 steps
  CHECK(lane_text(0) == "2:1 49:2 240:3");
  CHECK(lane_text(1) == "338:4");  // 677 / 2 = 338 is on step 7: it stays
  CHECK(lane_text(2) == "50:5");
  CHECK(rig.project().pattern(1)->track_length[0] == 8u);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(lanes_now() == before);
  CHECK(lengths_of(rig) == len0);
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(lane_text(0) == "2:1 49:2 240:3");
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7d: an EXPAND or SHRINK whose lane records do not fit the undo ring is refused whole with the strip notice, steps, lengths and lanes unchanged") {
  static model::LaneEvent big[4096];
  for (unsigned i = 0; i < 4096; ++i) {
    big[i] = lane_cc(i * 6144u / 4096u, i & 0x7Fu);
  }
  Rig rig(true, 128, true, 96u * 1024u);  // a 40 KiB Pattern ring: the two lanes (the records of a scale are twice 33 KB) do not fit
  g_s7c.clear();
  g_s7c.adopt(rig.project().load_epoch());
  rig.editor->attach_lanes(&g_s7c);
  rig.app->attach_document(rig.editor.get());
  NEED(rig.editor->lane_replace(1, 0, 0, 6144, big, 4096) == model::Status::Ok);
  put_note(rig, 0, 100, 77);
  rig.edit_mode();
  rig.editor->history_reset();
  const std::vector<unsigned> lengths = lengths_of(rig);
  (void)rig.invoke("pattern.op", 2);  // (Changed: the strip text is what is redrawn; the pattern is not)
  CHECK(rig.message() == "TOO BIG TO UNDO: USE THE LENGTH CHOOSER");
  CHECK(rig.depth() == 0u);
  CHECK(lengths_of(rig) == lengths);
  CHECK(g_s7c.lane(1, 0).count == 4096u);
  CHECK(g_s7c.lane(1, 0).events[1].tick == 1u);  // (6144 / 4096 = 1: not scaled)
  CHECK(rig.step(0, 100).note == 77);
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7d: TRANSPOSE of a whole-step block moves the lane notes of its tracks and rows with the steps' notes, CC and the other rows stay, one undo unit") {
  Rig rig(true, 16);
  lanes_on(rig);
  model::LaneEvent n1 = lane_cc(10, 90);
  n1.kind_channel = 0;
  n1.len = 20;
  n1.a = 60;
  model::LaneEvent n2 = n1;
  n2.a = 64;  // a chord
  model::LaneEvent n3 = n1;
  n3.tick = 300;  // step 6: outside the block
  NEED(lane_set(rig, 0, {n1, n2, lane_cc(20, 55), n3}));
  NEED(lane_set(rig, 2, {n1}));  // a track outside the block
  put_note(rig, 0, 0, 61);
  put_note(rig, 1, 0, 63);
  rig.edit_mode();
  rig.editor->history_reset();
  select_block(rig, 0, 0, 2, 2);
  CHECK(rig.invoke("verb.transpose", 5) == DispatchResult::Changed);
  CHECK(rig.depth() == 1u);
  const model::LaneStore::View v1 = g_s7c.lane(1, 0);
  NEED(v1.count == 4u);
  CHECK(v1.events[0].a == 65);
  CHECK(v1.events[1].a == 69);  // the chord stays a chord
  CHECK(v1.events[2].a == 1);   // the CC controller number stays
  CHECK(v1.events[2].b == 55);
  CHECK(v1.events[3].a == 60);  // step 6 is outside the block
  CHECK(g_s7c.lane(1, 2).events[0].a == 60);
  CHECK(rig.step(0, 0).note != 0);
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  const model::LaneStore::View v2 = g_s7c.lane(1, 0);
  CHECK(v2.events[0].a == 60);
  CHECK(v2.events[1].a == 64);
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7d: the TOOLS dialog's TRANSPOSE with a bare cursor (no selection) moves the cursor step only, the lane notes stay") {
  Rig rig(true, 16);
  lanes_on(rig);
  model::LaneEvent n1 = lane_cc(10, 90);
  n1.kind_channel = 0;
  n1.len = 20;
  n1.a = 60;
  NEED(lane_set(rig, 0, {n1}));  // track 0, step 0: under the cursor
  put(rig, 0, 0, note_at(61, 3));
  rig.edit_mode();
  rig.editor->history_reset();
  rig.tap(Button::F7);
  rig.pad(0);  // TRANSPOSE: the dialog
  CHECK(rig.state().overlay == app::OverlayId::PatternTools);
  rig.tap(Button::Down);
  rig.turn(1);  // AMOUNT +1
  CHECK(rig.state().ptools.amount != 0);
  rig.tap(Button::F1);
  CHECK(rig.step(0, 0).note != 61);  // the step moved
  const model::LaneStore::View lv = g_s7c.lane(1, 0);
  NEED(lv.count == 1u);
  CHECK(lv.events[0].a == 60);  // the lane note did not
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7d: FILL (the action,the dialog's EUCLID and RANDOM) and the RANDOMISE verb change steps only, the lanes stay as they were") {
  Rig rig(true, 16);
  lanes_on(rig);
  NEED(lane_set(rig, 0, {lane_cc(5, 1), lane_cc(99, 2), lane_cc(480, 3)}));
  NEED(lane_set(rig, 1, {lane_cc(677, 4)}));
  put(rig, 0, 0, note_at(61, 7));
  for (int r = 0; r < 16; ++r) {
    model::Step s = note_at(61, 1);
    s.vol = static_cast<std::uint8_t>(r * 8);
    put(rig, 1, r, s);
  }
  rig.edit_mode();
  rig.editor->history_reset();
  const std::string before = lanes_now();
  const std::uint32_t total = g_s7c.total_events();
  const auto lanes_stay = [&](const char* what) {
    CHECK_MESSAGE(lanes_now() == before, what);
    CHECK_MESSAGE(g_s7c.total_events() == total, what);
  };

  (void)rig.invoke("select.column_all");
  std::uint64_t digest = pattern_digest(rig, false, false);
  (void)rig.invoke("pattern.fill");
  CHECK(pattern_digest(rig, false, false) != digest);
  CHECK(rig.depth() == 1u);
  lanes_stay("pattern.fill");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  lanes_stay("undo of pattern.fill");
  rig.flush();

  rig.tap(Button::F4);
  fill_row(rig, kRowType, 2);  // EUCLID
  digest = pattern_digest(rig, false, false);
  rig.tap(Button::F1);
  CHECK(pattern_digest(rig, false, false) != digest);
  lanes_stay("EUCLID");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  rig.flush();

  rig.tap(Button::F4);
  fill_row(rig, kRowType, -1);  // EVERY N .. RANDOM (the type rolls: one left of EUCLID)
  rig.tap(Button::F1);
  lanes_stay("RANDOM");
  (void)rig.invoke("edit.undo");
  rig.flush();

  (void)rig.invoke("select.clear");
  rig.column(Column::Vol);
  rig.go(0, 1);
  rig.column(Column::Vol);
  for (int i = 0; i < 15; ++i) {
    (void)rig.invoke("select.extend", 1);
  }
  rig.tap(Button::F7);
  rig.pad(4);  // RANDOMISE
  lanes_stay("RANDOMISE");
  rig.app->attach_document(nullptr);
}

TEST_CASE("OS-MIDIREC-P1 S7c: the whole-pattern clip (F1 held, Copy, Shift + Copy) carries every track's lanes, replaces the target's over each track's new length, one undo unit") {
  Rig rig(true, 24);
  make_pattern(rig, 5, 40, "intro", 60);  // track 2 is 20 steps, the rest 40
  lanes_on(rig);
  NEED(lane_set(rig, 0, {lane_cc(5, 1), lane_cc(39 * 48 + 1, 2)}, 40 * 48, 5));
  NEED(lane_set(rig, 2, {lane_cc(5, 3), lane_cc(19 * 48, 4)}, 20 * 48, 5));
  NEED(lane_set(rig, 0, {lane_cc(700, 8)}, 24 * 48, 1));
  NEED(lane_set(rig, 3, {lane_cc(100, 9)}, 24 * 48, 1));
  rig.edit_mode();
  rig.editor->history_reset();
  rig.hold_key(Button::F1);
  rig.pad(4);  // pattern 5
  rig.tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.message() == "PATTERN 05 COPIED");
  rig.flush();
  rig.hold_key(Button::F1);
  rig.pad(0);  // pattern 1
  rig.shift_tap(Button::Copy);
  rig.release_key(Button::F1);
  CHECK(rig.depth() == 1u);
  CHECK(lane_text(0) == "5:1 1873:2");
  CHECK(lane_text(2) == "5:3 912:4");
  CHECK(lane_text(3) == "");  // pattern 5 has none on track 3: the old one is replaced by nothing
  CHECK(lane_text(0, 5) == "5:1 1873:2");  // the source stays
  CHECK(lane_text(2, 5) == "5:3 912:4");
  CHECK(rig.invoke("edit.undo") == DispatchResult::Changed);
  CHECK(lane_text(0) == "700:8");
  CHECK(lane_text(2) == "");
  CHECK(lane_text(3) == "100:9");
  CHECK(rig.invoke("edit.redo") == DispatchResult::Changed);
  CHECK(lane_text(0) == "5:1 1873:2");
  CHECK(lane_text(3) == "");
  rig.app->attach_document(nullptr);
}
