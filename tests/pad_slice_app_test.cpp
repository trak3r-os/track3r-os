// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-SLICEFIX: the Pattern screen's pads on a BEAT SLICE instrument (new Community OS behaviour, app/pad_note.h). The slice a note plays is note - 24, so pad n must be note 24 + n
// (the Chop page's pads were already); the NOTES board's notes (36 and up) all landed on the last slice. In Browse the pad sounds that note, in Edit it writes it, in PIANO and ISO
// the pad is the slice too (the scale and the layout do not apply). Any other instrument keeps the board's notes. The engines' side is amiga_voices_test.cpp and audio_voices_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/note_entry.h>
#include <o3os/app/pad_maps.h>
#include <o3os/app/pad_note.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/audio/voices.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument.h>
#include <o3os/platform/host/fakes.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

// What the pads asked of the voices.
struct PadLog final : app::PreviewSink {
  struct Down {
    unsigned pad, track, instrument, note;
  };
  std::vector<Down> downs;
  void preview(unsigned, unsigned) noexcept override {}
  void pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note) noexcept override { downs.push_back({pad, track, instrument, note}); }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  PadLog log;

  Rig() {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 16);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_preview(&log);
    app->run_once();
  }
  model::Project& project() { return test::arena(0).project; }
  // Instrument 1: a sample in the given play mode (5: beat slice).
  void instrument(unsigned play_mode, model::InstrumentKind kind = model::InstrumentKind::Sample) {
    model::Instrument in;
    model::init_instrument(in, kind);
    if (kind == model::InstrumentKind::Midi) {
      in.midi_channel = 1;
    } else {
      in.sample_path[0] = 'a';
      in.sample_path[1] = '\0';
    }
    in.sampler.play_mode = static_cast<std::uint8_t>(play_mode);
    CHECK(project().set_instrument(0, in) == model::Status::Ok);
  }
  void pass(unsigned ms = 1) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap_pad(unsigned pad) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 100));
    pass();
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(pad), 0));
    pass();
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass();
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void shift_tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
    pass();
    tap(b);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
    pass();
  }
  // OS-UXF-PAT 7: the pads' modes are the PADS picker's (Shift + F2, then F1..F6 = NOTES PIANO ISO TRACKS PATS VU); `old` is the former Shift key of the mode (F2 NOTES .. F7 VU).
  void pad_mode(Button old) {
    shift_tap(Button::F2);
    tap(static_cast<Button>(static_cast<unsigned>(old) - 1u));
  }
  void edit_mode() { (void)app->invoke(core::action_id("view.mode"), static_cast<core::ActionArg>(app::PatternMode::Edit)); }
  model::Step step(unsigned row, unsigned track = 0) { return project().pattern(1)->steps[track][row]; }
};

}  // namespace

TEST_CASE("beat slice pads: the helper") {
  Rig rig;
  const app::PadBoard board;
  rig.instrument(5);
  CHECK(app::beat_slice_instrument(&rig.project(), 1));
  CHECK_FALSE(app::beat_slice_instrument(&rig.project(), 2));     // no instrument there
  CHECK_FALSE(app::beat_slice_instrument(&rig.project(), 0));
  CHECK_FALSE(app::beat_slice_instrument(nullptr, 1));
  for (unsigned pad = 0; pad < 48; ++pad) CHECK(app::pad_note(&rig.project(), 1, board, pad) == audio::kSliceRootNote + pad);
  for (unsigned pad = 0; pad < 48; ++pad) CHECK(app::pad_note(&rig.project(), 2, board, pad) == board.note(pad));
  for (unsigned mode : {0u, 1u, 2u, 3u, 4u, 6u, 7u}) {   // slice mode (4) is not beat slice: it plays the selected slice whatever the note, the pads keep the board
    rig.instrument(mode);
    CHECK_FALSE(app::beat_slice_instrument(&rig.project(), 1));
    CHECK(app::pad_note(&rig.project(), 1, board, 20) == board.note(20));
  }
  rig.instrument(5, model::InstrumentKind::Midi);
  CHECK_FALSE(app::beat_slice_instrument(&rig.project(), 1));
}

TEST_CASE("beat slice pads: in Browse pad n sounds note 24 + n, any other instrument the board's note") {
  Rig rig;
  const app::PadBoard board;
  rig.instrument(5);
  for (unsigned pad : {0u, 1u, 5u, 11u, 12u, 24u, 36u, 47u}) {
    rig.tap_pad(pad);
    NEED(!rig.log.downs.empty());
    CHECK(rig.log.downs.back().pad == pad);
    CHECK(rig.log.downs.back().instrument == 1u);
    CHECK_MESSAGE(rig.log.downs.back().note == 24u + pad, "pad " << pad);
  }
  rig.log.downs.clear();
  rig.instrument(1);   // a one-shot sample: the NOTES board, as before
  for (unsigned pad : {0u, 5u, 47u}) {
    rig.tap_pad(pad);
    NEED(!rig.log.downs.empty());
    CHECK(rig.log.downs.back().note == board.note(pad));
  }
}

TEST_CASE("beat slice pads: in Edit pad n writes note 24 + n into the cursor's step, and the step shows it") {
  Rig rig;
  rig.instrument(5);
  rig.edit_mode();
  rig.tap_pad(0);
  rig.tap_pad(7);
  rig.tap_pad(47);
  // a step stores the note + 1; the cursor moves on after each
  CHECK(rig.step(0).note == 25u);
  CHECK(rig.step(1).note == 32u);
  CHECK(rig.step(2).note == 72u);
  CHECK(rig.step(0).instr == 1u);
  // the same pad on a one-shot instrument writes the board's note
  rig.instrument(0);
  const app::PadBoard board;
  rig.tap_pad(7);
  CHECK(rig.step(3).note == board.note(7) + 1u);
}

TEST_CASE("beat slice pads: PIANO and ISO have no layout on a beat slice instrument, the pad is the slice") {
  for (const bool iso : {false, true}) {
    Rig rig;
    CAPTURE(iso);
    rig.pad_mode(iso ? Button::F4 : Button::F3);
    CHECK(rig.app->state().padmode.fixed() == (iso ? app::PadMode::Iso : app::PadMode::Piano));
    // the first pad of the layout that plays a note, and its note on a one-shot instrument
    unsigned pad = 0;
    while (pad < 48 && (iso ? app::iso_pad(pad) : app::piano_pad(pad)).kind != app::PadKey::Note) ++pad;
    NEED(pad < 48);
    rig.instrument(0);
    rig.tap_pad(pad);
    NEED(!rig.log.downs.empty());
    const unsigned layout = rig.log.downs.back().note;
    CHECK(layout != 24u + pad);   // the layout's note is not the slice's (or this would prove nothing)
    rig.log.downs.clear();
    rig.instrument(5);
    rig.tap_pad(pad);
    NEED(!rig.log.downs.empty());
    CHECK(rig.log.downs.back().note == 24u + pad);
    // the pads that are not notes (octave, navigation) are still not notes
    for (unsigned p = 0; p < 48; ++p) {
      if ((iso ? app::iso_pad(p) : app::piano_pad(p)).kind == app::PadKey::Note) {
        rig.log.downs.clear();
        rig.tap_pad(p);
        NEED(rig.log.downs.size() == 1);
        CHECK(rig.log.downs.back().note == 24u + p);
      }
    }
    // in Edit the pad writes the slice's note too
    rig.edit_mode();
    rig.tap_pad(pad);
    CHECK(rig.step(0).note == 24u + pad + 1u);
  }
}
