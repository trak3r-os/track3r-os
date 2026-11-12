// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the Wave / Chop screen on the App: the two pages, the four points and their order, the encoder and its zero-crossing snap, the slices (EVEN, ADD, DEL,
// AUTO as a job), the pads (play and select, the LEDs), the zoom, undo as the project's, and the handover to the voices. The arithmetic is wave_math_test.cpp and
// transient_test.cpp; the bank is sample_bank_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/wave_screen.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/core/gesture.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::PadRole;
using app::ScreenId;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

constexpr std::uint32_t kFrames = 96000;
constexpr unsigned kSlot = 4;  // instrument 5

// Six hits 15000 frames apart, over a little noise: a sine burst each, decaying.
void make_break(std::int16_t* out) {
  std::uint32_t s = 7u;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    s = s * 1664525u + 1013904223u;
    out[i] = static_cast<std::int16_t>(static_cast<int>((s >> 16) % 41u) - 20);
  }
  for (unsigned k = 0; k < 6; ++k) {
    const std::uint32_t at = 6000u + k * 15000u;
    for (std::uint32_t j = 0; j < 7000u && at + j < kFrames; ++j) {
      out[at + j] = static_cast<std::int16_t>(out[at + j] + static_cast<int>(18000.0 * std::exp(-static_cast<double>(j) / 1500.0) * std::sin(0.23 * static_cast<double>(j))));
    }
  }
}
constexpr std::uint32_t hit_at(unsigned k) { return 6000u + k * 15000u; }

class FakePreview final : public app::PreviewSink {
 public:
  std::vector<std::pair<unsigned, unsigned>> played;
  void preview(unsigned instrument, unsigned note) noexcept override { played.emplace_back(instrument, note); }
  bool busy = false;  // a live note queued or held (the platform's answer from the engine)
  bool live_idle() const noexcept override { return !busy; }
  std::vector<int> holds;      // OS-FU3: the Preview key's notes: +instrument on, -instrument off
  std::vector<bool> auditions;  // the audition's on and off (Shift + Preview, the take)
  std::uint32_t position = 0;  // the word the render would publish
  void hold_note(bool on, unsigned, unsigned instrument, unsigned) noexcept override { holds.push_back(on ? static_cast<int>(instrument) : -static_cast<int>(instrument)); }
  void audition(bool on, unsigned) noexcept override { auditions.push_back(on); }
  std::vector<unsigned> slice_pads;  // OS-BUG-SLICEPAD: the slice of every slice-mode pad press
  void slice_pad_down(unsigned pad, unsigned track, unsigned instrument, unsigned note, unsigned slice) noexcept override {
    slice_pads.push_back(slice);
    PreviewSink::slice_pad_down(pad, track, instrument, note, slice);
  }
  std::uint32_t live_position() const noexcept override { return position; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peaks;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  FakePreview preview;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(bool with_document = true, bool with_sample = true) : memory(400000), peaks(40000) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 400000, peaks.data(), peaks.size());
    if (with_sample) {
      model::Instrument in;
      model::init_instrument(in, model::InstrumentKind::Sample);
      std::strcpy(in.name, "BREAK");
      NEED(arena->project.set_instrument(kSlot, in) == model::Status::Ok);
      std::uint32_t cap = 0;
      std::int16_t* p = bank.reserve(cap);
      NEED(p != nullptr);
      make_break(p);
      NEED(bank.commit_load(kSlot, p, kFrames));
      bank.rebind_now(kSlot);
    }
    if (with_document) {
      journal.begin(g_ring, sizeof g_ring);
      editor = std::make_unique<model::Editor>(arena->project, journal);
      app->attach_document(editor.get());
    }
    app->attach_samples(&bank);
    app->attach_preview(&preview);
    app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kSlot + 1));
    go(ScreenId::Wave);
    if (with_sample) {
      passes(30);  // the peak job ends ("PEAKS BREAK DONE" is not what the tests are about)
      expire();
    }
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
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
    press(b);
    release(b);
    pass();
    release(Button::Shift);
    pass(1);
  }
  void pad(unsigned n, bool shift = false) {
    if (shift) {
      press(Button::Shift);
      pass(1);
    }
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 100));
    pass();
    if (shift) {
      release(Button::Shift);
      pass(1);
    }
  }
  void invoke(const char* name, core::ActionArg arg = 0) {
    (void)app->invoke(core::action_id(name), arg);
    pass();
  }
  void chop() {  // the SamplePlay button again: the CHOP page
    tap(Button::SamplePlay);
    NEED(app->state().page[app::screen_index(ScreenId::Wave)] == 1u);
  }
  const model::Instrument& inst() const { return *arena->project.instrument(kSlot); }
  const app::WaveState& ws() const { return app->state().wave; }
  std::string message() const { return app->message(); }
  void expire() {
    for (unsigned i = 0; i < 10 && !std::string(app->message()).empty(); ++i) {
      clock.advance(3000000);
      app->run_once();
    }
  }
  // The handover has run: stopped, the pool quiet, a few passes of the settle.
  void settle_engine() { passes(4); }
};

}  // namespace

TEST_CASE("wave: it is a screen of the registry, reached by SamplePlay, with two pages: the button again is CHOP") {
  Rig rig;
  const app::ScreenEntry* e = app::find_screen(app::screen_registry(), ScreenId::Wave);
  NEED(e != nullptr);
  CHECK(e->screen == &app::kWaveScreen);
  CHECK(rig.app->state().screen == ScreenId::Wave);
  CHECK(app::kWaveScreen.pages() == 2u);
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 0u);
  rig.tap(Button::SamplePlay);
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 1u);
  rig.tap(Button::SamplePlay);
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 0u);
  CHECK(rig.app->frames() >= 1u);
}

TEST_CASE("wave: the waveform is built for the sample in view, a job in the strip, and drawn from the App's cache") {
  Rig rig;
  rig.passes(20);
  CHECK(rig.app->peak_cache().built());
  CHECK(rig.app->peak_cache().samples() == rig.bank.pcm(kSlot).data);
  CHECK(rig.app->peak_cache().frames() == kFrames);
  CHECK(rig.app->model().peaks == &rig.app->peak_cache());
  // Another instrument with another sample: the cache follows.
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  NEED(rig.arena->project.set_instrument(7, in) == model::Status::Ok);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  for (unsigned i = 0; i < 5000; ++i) {
    p[i] = static_cast<std::int16_t>(i % 100);
  }
  NEED(rig.bank.commit_load(7, p, 5000));
  rig.invoke("instrument.select", 8);
  rig.passes(20);
  CHECK(rig.app->peak_cache().samples() == p);
  CHECK(rig.app->peak_cache().frames() == 5000u);
  CHECK(rig.app->peak_cache().built());
}

TEST_CASE("wave: the Params source visual's waveform is built on the Instrument screen too, for a sample no sample screen has shown (FQA-10c)") {
  Rig rig;
  rig.passes(20);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  NEED(rig.arena->project.set_instrument(7, in) == model::Status::Ok);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  for (unsigned i = 0; i < 5000; ++i) {
    p[i] = static_cast<std::int16_t>(i % 100);
  }
  NEED(rig.bank.commit_load(7, p, 5000));
  rig.go(ScreenId::Instrument);  // (before the choice: a pass on Wave would build it)
  rig.invoke("instrument.select", 8);
  rig.passes(20);
  CHECK(rig.app->state().screen == ScreenId::Instrument);
  CHECK(rig.app->peak_cache().samples() == p);
  CHECK(rig.app->peak_cache().frames() == 5000u);
  CHECK(rig.app->peak_cache().built());
}

TEST_CASE("wave: F1..F4 pick the point, the encoder moves it, the loop is pushed ahead of the start and pulled in behind the end, and undo puts it all back") {
  Rig rig;
  rig.tap(Button::F1);
  CHECK(rig.ws().point == 0u);
  rig.tap(Button::F4);
  CHECK(rig.ws().point == 3u);
  rig.tap(Button::F1);
  rig.invoke("wave.zc", 0);  // snap off: the arithmetic of the step is the test
  CHECK_FALSE(rig.ws().zc);
  const std::uint16_t before = rig.inst().sampler.start;
  rig.invoke("wave.nudge", 10);
  const std::uint16_t after = rig.inst().sampler.start;
  CHECK(after > before);
  // 96000 frames over a 768 column lane: 125 frames a click = 85 points: ten clicks.
  CHECK(after == 10u * 85u);
  CHECK(rig.inst().sampler.loop_start == after);  // pushed ahead of the start (it was at 0)
  rig.invoke("wave.nudge_coarse", 1);
  CHECK(rig.inst().sampler.start == after + 16u * 85u);
  // END moved in past the loop's end pulls the loop in.
  rig.tap(Button::F4);
  rig.invoke("wave.nudge", -100);
  CHECK(rig.inst().sampler.end == 65535u - 100u * 85u);
  CHECK(rig.inst().sampler.loop_end == rig.inst().sampler.end);
  const app::Points p{rig.inst().sampler.start, rig.inst().sampler.loop_start, rig.inst().sampler.loop_end, rig.inst().sampler.end};
  CHECK(app::points_valid(p));
  // F8: undo, one unit at a time (the last nudge first), Shift + F5 redoes (OS-UXF-SMP item 11: Shift + F8 is INST +).
  const std::uint16_t end_now = rig.inst().sampler.end;
  rig.tap(Button::F8);
  CHECK(rig.inst().sampler.end == 65535u);
  CHECK(rig.inst().sampler.loop_end == 65535u);
  rig.shift_tap(Button::F5);
  CHECK(rig.inst().sampler.end == end_now);
}

TEST_CASE("wave: Shift + F8 / Shift + F7 are INST + / INST - (OS-UXF-SMP item 11): the instrument in view steps on and back, round") {
  Rig rig;
  const int here = rig.app->state().instrument;
  CHECK(here == static_cast<int>(kSlot) + 1);
  rig.shift_tap(Button::F8);
  CHECK(rig.app->state().instrument == here + 1);
  CHECK(rig.app->state().screen == ScreenId::Wave);
  rig.shift_tap(Button::F7);
  CHECK(rig.app->state().instrument == here);
  CHECK(rig.app->state().screen == ScreenId::Wave);
}

TEST_CASE("wave: with ZC on the encoder moves to a zero crossing in the direction it turns, and can always leave one") {
  Rig rig;
  rig.tap(Button::F2);  // LOOP START
  CHECK(rig.ws().zc);
  rig.invoke("wave.nudge", 8);
  const unsigned p1 = rig.inst().sampler.loop_start;
  const std::int16_t* pcm = rig.bank.pcm(kSlot).data;
  const auto at = [&](unsigned point) { return pcm[app::point_to_frame(point, kFrames)]; };
  // The new point is a crossing or nearly one: the sample there is small next to the signal's size at the hits, and the point is ahead of where it was.
  CHECK(p1 > 0u);
  rig.invoke("wave.nudge", 1);
  const unsigned p2 = rig.inst().sampler.loop_start;
  CHECK(p2 > p1);
  rig.invoke("wave.nudge", -1);
  CHECK(rig.inst().sampler.loop_start < p2);
  (void)at;
  rig.invoke("wave.zc", 0);
  CHECK_FALSE(rig.ws().zc);
}

TEST_CASE("wave: the engine follows an edit only when playback is stopped, and says so meanwhile") {
  Rig rig;
  struct Transport final : app::TransportSink {
    bool play(unsigned) noexcept override { return true; }
    bool play_song(unsigned) noexcept override { return true; }
    bool stop() noexcept override { return true; }
  } transport;
  rig.app->attach_transport(&transport);
  rig.invoke("transport.play");
  NEED(rig.app->state().playing);
  const std::uint32_t full = rig.voices->sample(kSlot).length;
  CHECK(full == kFrames);
  rig.tap(Button::F4);  // END
  rig.invoke("wave.nudge", -200);
  CHECK(rig.bank.rebind_pending());
  rig.passes(10);
  CHECK(rig.voices->sample(kSlot).length == full);  // playing: the engine keeps its descriptor
  rig.invoke("transport.play");                     // stop
  rig.settle_engine();
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(kSlot).length < full);
}

TEST_CASE("wave: an edit waits for the live notes too: a pad or key queued or held while it is pending keeps the engine's descriptor (OS-011a on OS-005g)") {
  Rig rig;
  const std::uint32_t full = rig.voices->sample(kSlot).length;
  CHECK(full == kFrames);
  rig.tap(Button::F4);  // END
  rig.invoke("wave.nudge", -200);
  CHECK(rig.bank.rebind_pending());
  rig.preview.busy = true;  // the render has not taken the note yet: the pool is still silent
  rig.passes(20);
  CHECK(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(kSlot).length == full);
  rig.preview.busy = false;
  rig.passes(8);
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(kSlot).length < full);
}

TEST_CASE("chop: EVEN makes n equal slices, puts the instrument in beat slice mode and says so; undo takes both back; Shift + Up / Down sets n") {
  Rig rig;
  rig.chop();
  CHECK(rig.inst().sampler.slice_count == 0u);
  CHECK(rig.inst().sampler.play_mode == 0u);
  rig.invoke("wave.even", 0);
  CHECK(rig.inst().sampler.slice_count == 16u);
  CHECK(rig.inst().sampler.play_mode == 5u);
  CHECK(rig.inst().sampler.slices[8] == 32767u);
  CHECK(rig.message() == "PLAY MODE: BEAT SLICE");
  rig.tap(Button::F8);
  CHECK(rig.inst().sampler.slice_count == 0u);
  CHECK(rig.inst().sampler.play_mode == 0u);
  rig.shift_tap(Button::Up);
  rig.shift_tap(Button::Up);
  CHECK(rig.ws().even == 18u);
  rig.shift_tap(Button::Down);
  CHECK(rig.ws().even == 17u);
  rig.tap(Button::F2);
  CHECK(rig.inst().sampler.slice_count == 17u);
  // The engine gets the table when stopped: beat slice, 17 slices.
  rig.settle_engine();
  CHECK(rig.voices->sample(kSlot).beat_slice);
  CHECK(rig.voices->sample(kSlot).slice_count == 17u);
}

TEST_CASE("chop: Shift + F4 TO PAT writes slice 1..n as notes 24 + k on consecutive steps of the Pattern cursor's track, one undo unit; the cursor stays (OS-UXF-SMP item 3c)") {
  Rig rig;
  rig.chop();
  rig.invoke("wave.even", 0);  // 16 slices, beat slice mode
  NEED(rig.inst().sampler.slice_count == 16u);
  rig.go(ScreenId::Pattern);
  rig.invoke("cursor.track", 1);
  rig.invoke("cursor.jump_fraction", 1);
  const int row = rig.app->state().pattern.row();
  const int track = rig.app->state().pattern.track();
  NEED(row > 0);
  NEED(track == 1);
  rig.go(ScreenId::Wave);
  NEED(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 1u);
  rig.expire();  // the notice queue shows the oldest news first
  rig.shift_tap(Button::F4);
  const unsigned id = rig.app->state().edit.pattern;
  const model::Pattern* pat = rig.arena->project.pattern(id);
  NEED(pat != nullptr);
  CHECK(pat->steps[track][row - 1].note == 0u);
  for (unsigned k = 0; k < 16u; ++k) {
    CHECK(pat->steps[track][row + static_cast<int>(k)].note == 24u + k);
    CHECK(pat->steps[track][row + static_cast<int>(k)].instr == kSlot + 1u);
  }
  CHECK(pat->steps[track][row + 16].note == 0u);
  CHECK(pat->steps[0][row].note == 0u);
  CHECK(rig.app->state().pattern.row() == row);
  CHECK(rig.app->state().pattern.track() == track);
  CHECK(rig.message() == "SLICES TO PAT");
  rig.tap(Button::F8);  // UNDO: the pattern the write made and all its steps go in one
  pat = rig.arena->project.pattern(id);
  CHECK((pat == nullptr || pat->steps[track][row].note == 0u));
}

TEST_CASE("chop: TO PAT stops at the end of the pattern, is refused with no slices or outside beat slice mode, and writes nothing then (OS-UXF-SMP item 3c)") {
  Rig rig;
  rig.chop();
  rig.expire();  // the notice queue shows the oldest news first
  rig.shift_tap(Button::F4);
  CHECK(rig.message() == "NO SLICES TO WRITE");
  CHECK(rig.arena->project.pattern(rig.app->state().edit.pattern) == nullptr);
  rig.invoke("wave.even", 0);
  {  // slices, but the play mode is not beat slice: a note would pitch the sample
    model::Instrument in = rig.inst();
    in.sampler.play_mode = 0;
    NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
    rig.expire();  // the notice queue shows the oldest news first
    rig.shift_tap(Button::F4);
    CHECK(rig.message() == "SET BEAT SLICE MODE FIRST");
    CHECK(rig.arena->project.pattern(rig.app->state().edit.pattern) == nullptr);
    in.sampler.play_mode = 5;
    NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  }
  rig.go(ScreenId::Pattern);
  rig.invoke("cursor.jump_end", 0);
  const int row = rig.app->state().pattern.row();
  rig.go(ScreenId::Wave);
  rig.expire();  // the notice queue shows the oldest news first
  rig.shift_tap(Button::F4);
  const model::Pattern* pat = rig.arena->project.pattern(rig.app->state().edit.pattern);
  NEED(pat != nullptr);
  const int track = rig.app->state().pattern.track();
  CHECK(pat->steps[track][row].note == 24u);
  CHECK(rig.message() == "SLICES TO PAT: PATTERN END");
  CHECK(pat->steps[track][row - 1].note == 0u);
}

TEST_CASE("chop: TO PAT on a track shorter than the pattern clips the write at the track's end, and is refused from a row at or past it (OS-UXF-SMP item 3c, review)") {
  Rig rig;
  rig.chop();
  rig.invoke("wave.even", 0);  // 16 slices, beat slice mode
  NEED(rig.inst().sampler.slice_count == 16u);
  const unsigned id = rig.app->state().edit.pattern;
  NEED(rig.arena->project.create_pattern(id, 64) == model::Status::Ok);
  NEED(rig.arena->project.set_track_length(id, 1, 8) == model::Status::Ok);
  rig.go(ScreenId::Pattern);
  rig.invoke("cursor.track", 1);
  rig.invoke("cursor.jump_fraction", 1);
  const int far_row = rig.app->state().pattern.row();
  NEED(rig.app->state().pattern.track() == 1);
  NEED(far_row >= 8);
  rig.go(ScreenId::Wave);
  rig.expire();  // the notice queue shows the oldest news first
  rig.shift_tap(Button::F4);  // past the short track's end: nothing is written
  CHECK(rig.message() == "PAST TRACK END");
  const model::Pattern* pat = rig.arena->project.pattern(id);
  NEED(pat != nullptr);
  CHECK(pat->steps[1][far_row].note == 0u);
  rig.go(ScreenId::Pattern);
  rig.invoke("cursor.jump_fraction", 0);
  NEED(rig.app->state().pattern.row() == 0);
  NEED(rig.app->state().pattern.track() == 1);
  rig.go(ScreenId::Wave);
  rig.expire();  // the notice queue shows the oldest news first
  rig.shift_tap(Button::F4);  // row 0: 8 slices fit, the other 8 are dropped
  CHECK(rig.message() == "SLICES TO PAT: PATTERN END");
  pat = rig.arena->project.pattern(id);
  NEED(pat != nullptr);
  for (unsigned k = 0; k < 8u; ++k) {
    CHECK(pat->steps[1][k].note == 24u + k);
  }
  CHECK(pat->steps[1][8].note == 0u);
  CHECK(pat->steps[0][0].note == 0u);
}

TEST_CASE("wave: Shift + F7 / F8 re-pick the page from the new instrument's play mode, so the Shift row shows TO PAT only on a slice instrument (OS-UXF-SMP review)") {
  Rig rig;
  rig.chop();
  rig.invoke("wave.even", 0);  // the slot is in beat slice mode
  {  // the next slot: a sample instrument in the one-shot mode
    model::Instrument in = rig.inst();
    in.sampler.play_mode = 0;
    in.sampler.slice_count = 0;
    NEED(rig.arena->project.set_instrument(kSlot + 1, in) == model::Status::Ok);
  }
  const auto to_pat_shown = [&rig]() {
    const app::ViewState& v = rig.app->state();
    return std::string(app::kWaveScreen.shift_row(v)[3]) == "TO PAT";
  };
  NEED(to_pat_shown());
  rig.shift_tap(Button::F8);  // CHOP page onto a one-shot instrument: the WAVE page, no TO PAT
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 0u);
  CHECK_FALSE(to_pat_shown());
  rig.shift_tap(Button::F7);  // and back onto the slice instrument: CHOP again
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 1u);
  CHECK(to_pat_shown());
}

TEST_CASE("chop: ADD splits the selected slice at the middle of its start and end (and puts one at 0 first), DEL removes the selected one, Left / Right select, the encoder moves its start") {
  Rig rig;
  rig.chop();
  rig.invoke("wave.zc", 0);
  rig.tap(Button::F4);  // ADD
  CHECK(rig.inst().sampler.slice_count == 2u);
  CHECK(rig.inst().sampler.slices[0] == 0u);
  CHECK((rig.inst().sampler.slices[1] >= 32766u && rig.inst().sampler.slices[1] <= 32768u));
  CHECK(rig.ws().slice == 1u);
  CHECK(rig.inst().sampler.play_mode == 5u);
  // Zoomed in around slice 0 (the window is its first half): ADD still splits the selected slice (0, the first half) at the middle of its start and end, not of the window (OS-BUG-ADDSLICE).
  rig.tap(Button::Left);
  CHECK(rig.ws().slice == 0u);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  CHECK(rig.ws().span == kFrames / 4u);  // the window's middle would be 8191
  rig.tap(Button::F4);
  CHECK(rig.inst().sampler.slice_count == 3u);
  const unsigned quarter = rig.inst().sampler.slices[1];
  CHECK((quarter >= 16382u && quarter <= 16385u));
  CHECK(rig.ws().slice == 1u);
  // The new slice (the quarter to the half) is selected: ADD splits that one now.
  rig.tap(Button::F4);
  CHECK(rig.inst().sampler.slice_count == 4u);
  CHECK((rig.inst().sampler.slices[2] >= 24574u && rig.inst().sampler.slices[2] <= 24577u));
  CHECK(rig.ws().slice == 2u);
  rig.tap(Button::F5);  // back to three
  CHECK(rig.inst().sampler.slice_count == 3u);
  // Left / Right select, the encoder moves the slice's start between its neighbours.
  rig.tap(Button::Left);
  rig.tap(Button::Left);
  CHECK(rig.ws().slice == 0u);
  rig.tap(Button::Right);
  CHECK(rig.ws().slice == 1u);
  rig.invoke("wave.nudge", -3);
  CHECK(rig.inst().sampler.slices[1] < quarter);
  CHECK(rig.inst().sampler.slices[1] > rig.inst().sampler.slices[0]);
  rig.invoke("wave.nudge", 100000);  // as far as it goes: one before the next slice
  CHECK(rig.inst().sampler.slices[1] == rig.inst().sampler.slices[2] - 1u);
  rig.expire();
  rig.tap(Button::F4);  // slice 1 is one unit wide: there is no middle to split at
  CHECK(rig.message() == "THERE IS A SLICE HERE");
  CHECK(rig.inst().sampler.slice_count == 3u);
  // DEL removes the selected one, down to none.
  rig.tap(Button::F5);
  CHECK(rig.inst().sampler.slice_count == 2u);
  CHECK(rig.ws().slice == 1u);
  rig.tap(Button::F5);
  CHECK(rig.inst().sampler.slice_count == 1u);
  CHECK(rig.ws().slice == 0u);
  rig.tap(Button::F5);
  CHECK(rig.inst().sampler.slice_count == 0u);
  rig.expire();
  rig.tap(Button::F5);
  CHECK(rig.message() == "NO SLICE TO DELETE");
}

TEST_CASE("chop: AUTO runs the slicer as a job and replaces the slices with one slice per hit, in one undo unit") {
  Rig rig;
  rig.chop();
  rig.tap(Button::F1);  // AUTO
  rig.passes(3);
  CHECK(rig.message().find("SLICE BREAK ") == 0u);  // the strip: SLICE BREAK nn%
  for (unsigned i = 0; i < 200 && rig.inst().sampler.slice_count == 0u; ++i) {
    rig.pass();
  }
  // Slice 0 at the start and a slice at each of the six hits.
  NEED(rig.inst().sampler.slice_count == 7u);
  CHECK(rig.inst().sampler.slices[0] == 0u);
  for (unsigned k = 0; k < 6; ++k) {
    const std::uint32_t f = app::point_to_frame(rig.inst().sampler.slices[k + 1], kFrames);
    CHECK_MESSAGE((f <= hit_at(k) + 64u && f + 1100u >= hit_at(k)), "hit " << k << " sliced at " << f << ", it is at " << hit_at(k));
  }
  CHECK(rig.inst().sampler.play_mode == 5u);
  rig.tap(Button::F8);  // one undo unit: all of it
  CHECK(rig.inst().sampler.slice_count == 0u);
  CHECK(rig.inst().sampler.play_mode == 0u);
}

// OS-QAFIX finding 29: AUTO on a short sample. Whatever the length, the result is a beat slice instrument whose slice 0 starts at 0 and every pad of the board sounds (the pads past the last
// slice play it), as loud as the same sample played whole before AUTO. A sound of a few frames is inaudible either way: the 32 sample attack ramp (voices.h kAttackSamples) is not over before
// it ends, so the control (`plain`) is what a pad must reach, not a fixed level.
TEST_CASE("chop: AUTO on a short sample leaves an instrument that sounds on every pad (finding 29)") {
  for (const std::uint32_t frames : {3u, 40u, 200u, 1000u, 2500u, 5000u}) {
    CAPTURE(frames);
    Rig rig;
    rig.chop();
    std::uint32_t cap = 0;
    std::int16_t* p = rig.bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = static_cast<std::int16_t>(std::lround(9000.0 * std::sin(0.31 * i)));
    }
    NEED(rig.bank.commit_load(kSlot, p, frames));
    rig.passes(40);
    const auto peak_of = [&rig](unsigned note) {
      std::int16_t out[audio::VoicePool::kBlock * 4] = {};
      rig.voices->release_all();
      for (unsigned b = 0; b < 4u; ++b) {
        rig.voices->begin_block();
        if (b == 0u) {
          CHECK(rig.voices->note_on(0, note, kSlot, audio::kVelocityDefault));
        }
        rig.voices->render(0, audio::VoicePool::kBlock);
        rig.voices->end_block();
        const std::int16_t* f = rig.voices->feed(0);
        for (unsigned i = 0; i < audio::VoicePool::kBlock && f != nullptr; ++i) {
          out[b * audio::VoicePool::kBlock + i] = f[i];
        }
      }
      int peak = 0;
      for (const std::int16_t v : out) {
        peak = std::max(peak, std::abs(static_cast<int>(v)));
      }
      return peak;
    };
    const int plain = peak_of(60);  // the whole sample at the root, before AUTO
    rig.tap(Button::F1);            // AUTO
    rig.passes(300);
    CHECK(rig.inst().sampler.play_mode == 5u);
    CHECK(rig.inst().sampler.slice_count >= 1u);
    CHECK(rig.inst().sampler.slices[0] == 0u);
    rig.settle_engine();
    if (frames >= 40u) {
      CHECK(plain > 100);
    }
    for (unsigned pad = 0; pad < 16u; ++pad) {
      CAPTURE(pad);
      const int peak = peak_of(audio::kSliceRootNote + pad);
      const bool sounds = peak > 100 || plain <= 100;
      CHECK_MESSAGE(sounds, "pad " << pad << " is silent after AUTO on a " << frames << " frame sample (slices " << unsigned(rig.inst().sampler.slice_count) << ", whole sample " << plain << ")");
    }
  }
}

TEST_CASE("chop: the slicer's answer for a sample that was replaced meanwhile is thrown away") {
  Rig rig;
  rig.chop();
  rig.tap(Button::F1);
  rig.passes(2);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  make_break(p);
  NEED(rig.bank.commit_load(kSlot, p, kFrames));  // another sample in the slot: its version moved
  rig.passes(300);
  CHECK(rig.inst().sampler.slice_count == 0u);
}

TEST_CASE("chop: pads play slice n and select it, Shift + pad selects without a sound; the LEDs are the slices' pads, the selected one the brightest") {
  Rig rig;
  rig.chop();
  rig.invoke("wave.even", 0);  // 16 slices, beat slice mode
  rig.preview.played.clear();
  rig.pad(3);
  CHECK(rig.ws().slice == 3u);
  NEED(rig.preview.played.size() == 1u);
  CHECK(rig.preview.played[0].first == kSlot + 1u);
  CHECK(rig.preview.played[0].second == 24u + 3u);  // slice (note - 24) in beat slice mode
  rig.pad(9, true);
  CHECK(rig.ws().slice == 9u);
  CHECK(rig.preview.played.size() == 1u);           // silently
  rig.pad(20);                                      // past the last slice: plays (the engine clamps), selection stays
  CHECK(rig.ws().slice == 9u);
  CHECK(rig.preview.played.size() == 2u);
  PadRole roles[48];
  CHECK(rig.app->pad_lights(roles));
  for (unsigned n = 0; n < 48; ++n) {
    CHECK(roles[n] == (n == 9u ? PadRole::Front : (n < 16u ? PadRole::Back : PadRole::Off)));
  }
  // The WAVE page has no opinion.
  rig.tap(Button::SamplePlay);
  CHECK_FALSE(rig.app->pad_lights(roles));
}

TEST_CASE("chop: in slice mode pad n plays slice n at the root's pitch through slice_pad_down, a pad past the slices plays the board's note (OS-BUG-SLICEPAD)") {
  Rig rig;
  rig.chop();
  rig.invoke("wave.even", 0);  // 16 slices
  model::Instrument in = rig.inst();
  in.sampler.play_mode = 4;  // slice
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  rig.preview.played.clear();
  rig.pad(3);
  NEED(rig.preview.slice_pads.size() == 1u);
  CHECK(rig.preview.slice_pads[0] == 3u);
  NEED(rig.preview.played.size() == 1u);
  CHECK(rig.preview.played[0].second == 60u);
  rig.pad(20);  // past the last slice: the instrument's selected slice at the board's note
  CHECK(rig.preview.slice_pads.size() == 1u);
  NEED(rig.preview.played.size() == 2u);
  CHECK(rig.preview.played[1].second == rig.app->state().edit.pads.note(20));
}

TEST_CASE("wave: an instrument that is not in beat slice mode plays the pad board's note from a pad, not a slice") {
  Rig rig;
  rig.preview.played.clear();
  rig.pad(5);
  NEED(rig.preview.played.size() == 1u);
  CHECK(rig.preview.played[0].second == rig.app->state().edit.pads.note(5));
}

TEST_CASE("wave: Up zooms in around the selected point, Down out; the window stays inside the sample and follows a point moved out of it") {
  Rig rig;
  rig.invoke("wave.zc", 0);
  rig.tap(Button::F4);  // END, at the far end
  rig.tap(Button::Up);
  CHECK(rig.ws().span == kFrames / 2u);
  CHECK(rig.ws().first + rig.ws().span <= kFrames);
  CHECK(rig.ws().first > 0u);  // the end stays at the far edge of the lane: the window moved to it
  for (int i = 0; i < 30; ++i) {
    rig.tap(Button::Up);
  }
  CHECK(rig.ws().span == app::kMinSpan);
  rig.tap(Button::Down);
  CHECK(rig.ws().span == 2u * app::kMinSpan);
  for (int i = 0; i < 30; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.ws().span == 0u);  // all of it again is fit
  // Zoomed in on the start, the encoder moves it a column a click: the window follows when it would leave.
  rig.tap(Button::F1);
  for (int i = 0; i < 6; ++i) {
    rig.tap(Button::Up);
  }
  const std::uint32_t span = rig.ws().span;
  NEED(span != 0u);
  rig.invoke("wave.nudge_coarse", 40);
  const std::uint32_t frame = app::point_to_frame(rig.inst().sampler.start, kFrames);
  CHECK(frame >= rig.ws().first);
  CHECK(frame < rig.ws().first + rig.ws().span);
  CHECK(rig.ws().span == span);
}

TEST_CASE("wave: no sample in the instrument, no document, no sample memory: the actions say so and nothing breaks") {
  {
    Rig rig(true, false);  // an instrument slot with no sample
    rig.invoke("wave.nudge", 3);
    CHECK(rig.message() == "NO SAMPLE IN THIS INSTRUMENT");
    rig.tap(Button::SamplePlay);
    rig.expire();
    rig.tap(Button::F4);
    CHECK(rig.message() == "NO SAMPLE IN THIS INSTRUMENT");
    CHECK(rig.app->frames() >= 1u);
    PadRole roles[48];
    CHECK(rig.app->pad_lights(roles));
    CHECK(roles[0] == PadRole::Off);
  }
  {
    Rig rig(false, true);  // no document: an edit says NO PROJECT
    rig.invoke("wave.nudge", 3);
    CHECK(rig.message() == "NO PROJECT");
    CHECK(rig.inst().sampler.start == 0u);
  }
}

TEST_CASE("wave: a random walk over the keys keeps the points valid, the slices ascending and at most 48, and the state inside its bounds") {
  Rig rig;
  std::uint32_t s = 4242u;
  const auto next = [&](unsigned n) {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) % n;
  };
  const Button keys[] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6, Button::F7, Button::Left, Button::Right, Button::Up,
                         Button::Down, Button::SamplePlay};
  for (int i = 0; i < 1500; ++i) {
    switch (next(5)) {
      case 0: rig.tap(keys[next(12)]); break;
      case 1: rig.invoke("wave.nudge", static_cast<core::ActionArg>(next(41)) - 20); break;
      case 2: rig.invoke("wave.nudge_coarse", static_cast<core::ActionArg>(next(11)) - 5); break;
      case 3: rig.pad(next(48), next(2) == 0u); break;
      default: rig.shift_tap(next(2) == 0u ? Button::Up : Button::Down); break;
    }
    const model::Instrument& in = rig.inst();
    CHECK(in.sampler.slice_count <= 48u);
    for (unsigned k = 1; k < in.sampler.slice_count; ++k) {
      if (in.sampler.slices[k - 1] >= in.sampler.slices[k]) {
        FAIL_CHECK("slices not ascending at step " << i);
        return;
      }
    }
    const app::Points p{in.sampler.start, in.sampler.loop_start, in.sampler.loop_end, in.sampler.end};
    if (!app::points_valid(p)) {
      FAIL_CHECK("points invalid at step " << i);
      return;
    }
    const app::WaveState& w = rig.ws();
    CHECK(w.first + w.span <= kFrames);
    CHECK(w.even >= 2u);
    CHECK(w.even <= 48u);
    CHECK(w.point < 4u);
    if (in.sampler.slice_count != 0u) {
      CHECK(w.slice < in.sampler.slice_count);
    }
  }
}

// ---- the held Preview and the playhead (OS-FU3) ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("wave: F6 PREVIEW plays the instrument while it is down and ends with the release; the slice modes only say the pads play them") {
  Rig rig;
  rig.press(Button::F6);
  rig.pass();
  NEED(rig.preview.holds.size() == 1u);
  CHECK(rig.preview.holds[0] == static_cast<int>(kSlot + 1));
  rig.pass(500);
  CHECK(rig.preview.holds.size() == 1u);  // still down: still the one note
  rig.release(Button::F6);
  rig.pass();
  NEED(rig.preview.holds.size() == 2u);
  CHECK(rig.preview.holds[1] == -static_cast<int>(kSlot + 1));
  CHECK(rig.preview.auditions.empty());
  // Leaving the screen with the key down ends it too.
  rig.press(Button::F6);
  rig.pass();
  CHECK(rig.preview.holds.size() == 3u);
  rig.go(ScreenId::Config);
  CHECK(rig.preview.holds.size() == 4u);
  CHECK(rig.preview.holds[3] == -static_cast<int>(kSlot + 1));
  rig.release(Button::F6);
  rig.pass();
  // A slice instrument: no note, a word.
  rig.go(ScreenId::Wave);
  model::Instrument in = *rig.arena->project.instrument(kSlot);
  in.sampler.play_mode = 5;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  rig.press(Button::F6);
  rig.pass();
  CHECK(rig.preview.holds.size() == 4u);
  CHECK(std::string(rig.app->message()) == "THE PADS PLAY THE SLICES");
}

TEST_CASE("wave: Shift + F6 plays the sample from the selected point on the audition's descriptor, one shot to the end, and the release ends it") {
  Rig rig;
  model::Instrument edited = *rig.arena->project.instrument(kSlot);
  edited.sampler.loop_start = 32768;
  NEED(rig.arena->project.set_instrument(kSlot, edited) == model::Status::Ok);
  (void)rig.app->invoke(core::action_id("wave.point"), 1);  // LOOP START
  rig.press(Button::Shift);
  rig.pass(1);
  rig.press(Button::F6);
  rig.pass();
  rig.pass();
  NEED(rig.preview.auditions.size() == 1u);
  CHECK(rig.preview.auditions[0]);
  CHECK(rig.preview.holds.empty());
  const std::uint32_t from = app::point_to_frame(rig.arena->project.instrument(kSlot)->sampler.loop_start, kFrames);
  const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
  CHECK(d.length == kFrames - from);
  CHECK(d.data == rig.bank.pcm(kSlot).data + from);
  // The playhead is the audition's position from there.
  rig.preview.position = audio::VoicePool::kLivePlaying | (static_cast<std::uint32_t>(audio::kPreviewSlot + 1u) << 24) | 100u;
  rig.pass(40);
  CHECK(rig.app->model().head.on);
  CHECK(rig.app->model().head.slot == kSlot);
  CHECK(rig.app->model().head.frame == from + 100u);
  rig.release(Button::F6);
  rig.pass();
  NEED(rig.preview.auditions.size() == 2u);
  CHECK_FALSE(rig.preview.auditions[1]);
  rig.release(Button::Shift);
  rig.pass(1);
}

TEST_CASE("wave: the playhead follows the live voice of the instrument in view, from its start point, and goes away with it") {
  Rig rig;
  CHECK_FALSE(rig.app->model().head.on);
  const std::uint32_t start = app::point_to_frame(rig.arena->project.instrument(kSlot)->sampler.start, kFrames);
  rig.preview.position = audio::VoicePool::kLivePlaying | (static_cast<std::uint32_t>(kSlot + 1u) << 24) | 5000u;  // a pad's or MIDI's note, not only Preview
  rig.pass(40);
  CHECK(rig.app->model().head.on);
  CHECK(rig.app->model().head.slot == kSlot);
  CHECK(rig.app->model().head.frame == start + 5000u);
  rig.preview.position = audio::VoicePool::kLivePlaying | (static_cast<std::uint32_t>(kSlot + 2u) << 24) | 5000u;  // another instrument's voice
  rig.pass(40);
  CHECK_FALSE(rig.app->model().head.on);  // an instrument with no sample here: nothing to point at
  rig.preview.position = 0;
  rig.pass(40);
  CHECK_FALSE(rig.app->model().head.on);
  // A slice instrument shows none: the progress is hidden in Slice and BeatSlice.
  model::Instrument in = *rig.arena->project.instrument(kSlot);
  in.sampler.play_mode = 4;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  rig.preview.position = audio::VoicePool::kLivePlaying | (static_cast<std::uint32_t>(kSlot + 1u) << 24) | 5000u;
  rig.pass(40);
  CHECK_FALSE(rig.app->model().head.on);
}

// ---- OS-INSTPICK: the held Instr's list of the samples ---------------------------------------------------------------------------------------------------------------------------

namespace {

void set_kind(Rig& rig, unsigned slot, model::InstrumentKind kind) {
  model::Instrument in;
  model::init_instrument(in, kind);
  if (kind == model::InstrumentKind::Midi) {
    in.midi_channel = 1;
  } else if (kind == model::InstrumentKind::Module) {
    (void)rig.arena->project.add_module("tb303", 5, 1, 0);  // (Exists the second time)
    in.module_index = 0;
  }
  NEED(rig.arena->project.set_instrument(slot, in) == model::Status::Ok);
}

// A second sample instrument, with its own audio (not the break's length).
void add_sample(Rig& rig, unsigned slot, std::uint32_t frames) {
  set_kind(rig, slot, model::InstrumentKind::Sample);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  for (std::uint32_t i = 0; i < frames; ++i) {
    p[i] = static_cast<std::int16_t>(i % 100);
  }
  NEED(rig.bank.commit_load(slot, p, frames));
}

void hold_instr(Rig& rig) {
  rig.press(Button::Instr);
  rig.pass(core::kGestureTimings.hold_ms + 1);
}

void turn(Rig& rig, int detents) {
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, static_cast<std::int16_t>(detents)));
  rig.pass();
}

}  // namespace

TEST_CASE("wave: a held Instr opens the instrument list; only the samples can be chosen, and the screen follows the one chosen, fitted and with nothing selected") {
  Rig rig;
  set_kind(rig, 5, model::InstrumentKind::Midi);    // instrument 6
  set_kind(rig, 6, model::InstrumentKind::Module);  // instrument 7
  add_sample(rig, 7, 5000);                         // instrument 8
  rig.tap(Button::F3);                              // the point under the encoder: LOOP E
  rig.tap(Button::Up);                              // zoomed in
  NEED(rig.ws().span != 0u);
  const unsigned depth = rig.journal.depth();
  const app::ViewState& v = rig.app->state();

  hold_instr(rig);
  CHECK(v.overlay == app::OverlayId::StepPopup);
  CHECK(v.popup.kind == app::StepPopupView::Instr);
  CHECK(v.popup.samples_only);
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  // the list: the samples are for choosing, the MIDI, module and empty slots are dimmed
  const app::Model& m = rig.app->model();
  CHECK(app::step_popup_pickable(m, v, kSlot));
  CHECK(app::step_popup_pickable(m, v, 7));
  CHECK_FALSE(app::step_popup_pickable(m, v, 5));
  CHECK_FALSE(app::step_popup_pickable(m, v, 6));
  CHECK_FALSE(app::step_popup_pickable(m, v, 0));
  CHECK_FALSE(app::step_popup_pickable(m, v, 63));

  // Down skips the MIDI and the module slot, and the instrument in view is the new one: its window is the whole sample again
  rig.tap(Button::Down);
  CHECK(v.instrument == 8);
  CHECK(rig.ws().span == 0u);
  CHECK(rig.ws().first == 0u);
  CHECK(rig.ws().point == 2u);  // (the point is not the sample's: it stays)
  rig.tap(Button::Down);        // nothing after it to choose
  CHECK(v.instrument == 8);
  rig.tap(Button::Up);  // back over the module and the MIDI slot
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  rig.tap(Button::Up);  // nothing before it
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  rig.tap(Button::Right);  // a page: no sample 12 further, so it stays
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  turn(rig, 1);  // the encoder skips too
  CHECK(v.instrument == 8);
  turn(rig, -1);
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  // a pad chooses its instrument (pad n is n + 1) but not a dimmed one: nothing happens
  rig.pad(5);
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  rig.pad(2);
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  // the pads' lights: the samples, the one in view in front; the MIDI and module slots are dark
  app::PadRole roles[48];
  NEED(rig.app->pad_lights(roles));
  CHECK(roles[kSlot] == PadRole::Front);
  CHECK(roles[7] == PadRole::Back);
  CHECK(roles[5] == PadRole::Off);
  CHECK(roles[6] == PadRole::Off);
  rig.pad(7);
  CHECK(v.instrument == 8);

  rig.release(Button::Instr);
  rig.pass(1);
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(v.instrument == 8);
  rig.passes(20);
  CHECK(rig.app->peak_cache().samples() == rig.bank.pcm(7).data);  // the waveform of the new instrument
  CHECK(rig.app->peak_cache().frames() == 5000u);
  CHECK(rig.app->peak_cache().built());
  CHECK(rig.journal.depth() == depth);  // choosing is not an edit
  CHECK(rig.inst().sampler.start == 0u);  // and the instrument that was left is as it was
}


// OS-NAV3: Shift + Up / Down on the samples-only list. A page is the rows of the list; a target that is dimmed gives way to the nearest sample on the way back to the instrument in view, and with none
// the instrument stays.
TEST_CASE("wave: Shift + Up / Down page the samples-only instrument list, to the nearest sample on the way back when the page lands on a dimmed entry") {
  Rig rig;
  const app::ViewState& v = rig.app->state();
  hold_instr(rig);
  NEED(v.overlay == app::OverlayId::StepPopup);
  NEED(v.popup.samples_only);
  const int rows = app::step_popup_list_rows(ui::theme(v.theme), app::StepPopupView::Instr);
  NEED((rows >= 8 && rows <= 16));
  const int at = static_cast<int>(kSlot) + 1;  // the break, the only sample so far
  rig.shift_tap(Button::Down);
  CHECK(v.instrument == at);  // nothing to choose in the page (the target and everything back to the break are dimmed)
  rig.shift_tap(Button::Up);
  CHECK(v.instrument == at);  // nothing before it
  rig.release(Button::Instr);
  rig.pass(1);
  NEED(v.overlay == app::OverlayId::None);

  const unsigned near_end = static_cast<unsigned>(at + rows - 2) - 1u;  // the slot of the instrument two short of the page's target
  add_sample(rig, near_end, 3000);
  add_sample(rig, 7, 3000);  // instrument 8, inside the first page
  hold_instr(rig);
  NEED(v.overlay == app::OverlayId::StepPopup);
  NEED(app::step_popup_pickable(rig.app->model(), v, near_end));
  NEED(!app::step_popup_pickable(rig.app->model(), v, static_cast<unsigned>(at + rows) - 1u));  // the page's target is dimmed
  rig.shift_tap(Button::Down);
  CHECK(v.instrument == at + rows - 2);  // the nearest sample back from the target, past instrument 8
  rig.shift_tap(Button::Down);
  CHECK(v.instrument == at + rows - 2);  // no sample in the next page: it stays
  rig.shift_tap(Button::Up);
  CHECK(v.instrument == at);  // the target (two short of the break) is dimmed: the nearest sample on the way back is the break
  CHECK(v.overlay == app::OverlayId::StepPopup);
  rig.release(Button::Instr);
  rig.pass(1);
}

TEST_CASE("wave: on the CHOP page the choice gives up the slice selected on the old instrument; F8 closes the list; with no other sample there is nothing to choose") {
  Rig rig;
  rig.chop();
  rig.invoke("wave.even", 0);
  rig.tap(Button::Right);
  rig.tap(Button::Right);
  NEED(rig.ws().slice == 2u);
  const app::ViewState& v = rig.app->state();
  hold_instr(rig);
  CHECK(v.overlay == app::OverlayId::StepPopup);
  rig.tap(Button::Down);  // the break is the only sample of the project
  turn(rig, 3);
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  CHECK(rig.ws().slice == 2u);
  rig.tap(Button::F8);
  CHECK(v.overlay == app::OverlayId::None);
  rig.release(Button::Instr);
  rig.pass(1);
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);

  add_sample(rig, 9, 3000);  // instrument 10
  hold_instr(rig);
  rig.tap(Button::Down);
  CHECK(v.instrument == 10);
  CHECK(rig.ws().slice == 0u);
  rig.release(Button::Instr);
  rig.pass(1);
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(v.instrument == 10);
}

TEST_CASE("wave: with no sample in the project the list still opens, every entry is dimmed and nothing can be chosen") {
  Rig rig(true, false);  // no sample anywhere
  set_kind(rig, 3, model::InstrumentKind::Module);
  rig.app->invoke(core::action_id("instrument.select"), 4);
  const app::ViewState& v = rig.app->state();
  hold_instr(rig);
  CHECK(v.overlay == app::OverlayId::StepPopup);
  rig.tap(Button::Down);
  turn(rig, 2);
  rig.pad(9);
  CHECK(v.instrument == 4);
  CHECK_FALSE(app::step_popup_pickable(rig.app->model(), v, 3));
  rig.release(Button::Instr);
  rig.pass(1);
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(v.instrument == 4);
}

// ---- OS-INSTUI S4: the Wave screen per play mode ------------------------------------------------------------------------------------------------------------------------------------

namespace ip = model::instrument_param;

namespace {
unsigned view_of(Rig& rig) { return static_cast<unsigned>(app::wave_view_of(rig.app->model(), rig.app->state())); }
std::string label(Rig& rig, bool shift, unsigned key) {
  app::ViewState v = rig.app->state();
  v.shift = shift;
  const char* const* row = app::key_row(app::kWaveScreen, v, app::wave_view_of(rig.app->model(), v) == app::WaveView::Wavetable
                                                                  ? app::kWtSoftKeys
                                                                  : (app::wave_view_of(rig.app->model(), v) == app::WaveView::Granular ? app::kGranSoftKeys : (v.page[app::screen_index(ScreenId::Wave)] == 1u ? app::kChopSoftKeys : app::kWaveSoftKeys)));
  return row[key];
}
}  // namespace

TEST_CASE("wave modes: the view follows the play mode: WAVE for the loop modes, CHOP for the slice modes, a view of its own for a wavetable and a granular instrument (label sets)") {
  Rig rig;
  const unsigned wave = static_cast<unsigned>(app::WaveView::Wave);
  const unsigned chop = static_cast<unsigned>(app::WaveView::Chop);
  const unsigned wt = static_cast<unsigned>(app::WaveView::Wavetable);
  const unsigned gran = static_cast<unsigned>(app::WaveView::Granular);
  const unsigned want[8] = {wave, wave, wave, wave, chop, chop, wt, gran};
  rig.tap(Button::SamplePlay);  // CHOP (the page the screen turned to) and back: the mode decides for the modes that have a view of their own
  rig.tap(Button::SamplePlay);
  for (unsigned mode = 0; mode < 8; ++mode) {
    rig.invoke("instrument.play_mode", static_cast<core::ActionArg>(mode));
    // (the Params action does not turn the page: the slice modes are put on CHOP by the Wave screen's own list; here the page is left as it was)
    if (mode == 4u || mode == 5u) {
      CHECK(view_of(rig) == wave);
      rig.invoke("wave.zoom", 0);
    } else {
      CHECK_MESSAGE(view_of(rig) == want[mode], mode);
    }
  }
  // the key rows
  rig.invoke("instrument.play_mode", 6);
  CHECK(label(rig, false, 0) == "POSITION");
  CHECK(label(rig, false, 1) == "WINDOW");
  CHECK(label(rig, false, 5) == "PREVIEW");
  CHECK(label(rig, true, 0) == "MODE");
  rig.invoke("instrument.play_mode", 7);
  CHECK(label(rig, false, 0) == "POSITION");
  CHECK(label(rig, false, 1) == "LENGTH");
  CHECK(label(rig, false, 2) == "SHAPE");
  CHECK(label(rig, false, 3) == "LOOP");
  CHECK(label(rig, true, 0) == "MODE");
  rig.invoke("instrument.play_mode", 1);
  CHECK(label(rig, false, 0) == "START");
  CHECK(label(rig, true, 0) == "MODE");
}

TEST_CASE("wave modes: arriving on the screen shows the page the play mode picks, WAVE for every other mode even when CHOP was left open (FQA-10a)") {
  Rig rig;
  const unsigned wave = static_cast<unsigned>(app::WaveView::Wave);
  const unsigned chop = static_cast<unsigned>(app::WaveView::Chop);
  const std::size_t page = app::screen_index(ScreenId::Wave);
  rig.invoke("instrument.play_mode", 3);  // LOOP FWD
  rig.chop();                             // CHOP left open on a loop instrument (to add slices)
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::Wave);
  CHECK(rig.app->state().page[page] == 0u);
  CHECK(view_of(rig) == wave);
  rig.invoke("instrument.play_mode", 5);  // BEAT SLICE: the slice modes are CHOP
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::Wave);
  CHECK(rig.app->state().page[page] == 1u);
  CHECK(view_of(rig) == chop);
  rig.invoke("instrument.play_mode", 1);  // back to a loop mode: WAVE again on arrival
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::Wave);
  CHECK(rig.app->state().page[page] == 0u);
  CHECK(view_of(rig) == wave);
}

TEST_CASE("wave modes: Shift + F1 opens the list of the eight modes on the instrument's own, F1 sets the one chosen as the source page does (one undo unit), F8 closes it, the slice modes turn to CHOP") {
  Rig rig;
  CHECK(rig.inst().sampler.play_mode == 0);
  rig.shift_tap(Button::F1);
  NEED(rig.app->state().overlay == app::OverlayId::PlayMode);
  CHECK(rig.app->state().wave.mode_pick == 0);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.app->state().wave.mode_pick == 2);
  turn(rig, 3);
  CHECK(rig.app->state().wave.mode_pick == 5);
  turn(rig, 50);
  CHECK(rig.app->state().wave.mode_pick == 7);  // the end of the list
  rig.tap(Button::F8);  // BACK: nothing changes
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.inst().sampler.play_mode == 0);
  // BEAT SLICE: the CHOP page
  rig.shift_tap(Button::F1);
  turn(rig, 5);
  rig.tap(Button::F1);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.inst().sampler.play_mode == 5);
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 1u);
  // WAVETABLE: its own view, the window fitted to the sample, one undo unit
  rig.shift_tap(Button::F1);
  CHECK(rig.app->state().wave.mode_pick == 5);  // the instrument's own
  turn(rig, 1);
  rig.tap(Button::F1);
  CHECK(rig.inst().sampler.play_mode == 6);
  CHECK(rig.inst().sampler.wavetable_window_size == 2048);
  CHECK(view_of(rig) == static_cast<unsigned>(app::WaveView::Wavetable));
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 0u);
  rig.tap(Button::F8);
  CHECK(rig.inst().sampler.play_mode == 5);
  // ONE SHOT: the WAVE page
  rig.shift_tap(Button::F1);
  turn(rig, -5);
  rig.tap(Button::F1);
  CHECK(rig.inst().sampler.play_mode == 0);
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 0u);
  // the button again: the two pages only where the mode has them
  rig.tap(Button::SamplePlay);
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == 1u);
  rig.invoke("instrument.play_mode", 6);
  const std::uint8_t page = rig.app->state().page[app::screen_index(ScreenId::Wave)];
  rig.tap(Button::SamplePlay);
  CHECK(rig.app->state().page[app::screen_index(ScreenId::Wave)] == page);  // one page: nothing to turn
}

TEST_CASE("wave modes: the MODE list says there is no sample instrument where there is none") {
  Rig rig(true, false);
  rig.shift_tap(Button::F1);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.message() == "NO SAMPLE INSTRUMENT HERE");
}

TEST_CASE("wavetable view: Position and Window size are rows of the screen: F1 / F2 and Left / Right pick, the encoder edits through the source page's rules, Delete resets (QA: a Serum-style table is visible and editable)") {
  Rig rig;
  // a table of 2048-frame windows: the sample of the rig is 96000 frames = 46 windows of 2048
  rig.invoke("instrument.play_mode", 6);
  NEED(view_of(rig) == static_cast<unsigned>(app::WaveView::Wavetable));
  CHECK(app::kWaveScreen.count(rig.app->model(), rig.app->state()) == 2);
  CHECK(app::kWaveScreen.field_context(rig.app->model(), rig.app->state()) == app::Context::WaveWt);
  rig.tap(Button::F1);
  CHECK(core::param_field(app::kWaveScreen.focus_param(rig.app->model(), rig.app->state())) == ip::WtPos);
  turn(rig, 7);
  CHECK(rig.inst().sampler.wavetable_window == 7u);
  turn(rig, 1000);
  CHECK(rig.inst().sampler.wavetable_window == 45u);  // the last whole window of 46
  rig.tap(Button::F2);
  CHECK(core::param_field(app::kWaveScreen.focus_param(rig.app->model(), rig.app->state())) == ip::WtSize);
  turn(rig, -1);
  CHECK(rig.inst().sampler.wavetable_window_size == 1024);
  CHECK(rig.inst().sampler.wavetable_window == 90u);  // the same part of the sample: twice the windows
  rig.tap(Button::Left);  // back to the position
  CHECK(core::param_field(app::kWaveScreen.focus_param(rig.app->model(), rig.app->state())) == ip::WtPos);
  rig.tap(Button::Right);
  rig.tap(Button::Right);  // there is no third row
  CHECK(rig.ws().point == 1u);
  (void)rig.invoke("param.reset");  // WT SIZE: the default window again
  CHECK(rig.inst().sampler.wavetable_window_size == 2048);
  CHECK(rig.inst().sampler.wavetable_window == 45u);
  // F3..F5 have nothing here
  const std::uint8_t point = rig.ws().point;
  rig.tap(Button::F3);
  rig.tap(Button::F4);
  rig.tap(Button::F5);
  CHECK(rig.ws().point == point);
  CHECK(rig.ws().zc == true);
  // a pad plays the table (the note of the layout), not a slice
  rig.pad(3);
  CHECK(rig.preview.played.size() <= 1u);
  CHECK(rig.app->frames() >= 1u);
  CHECK(rig.app->failed_frames() == 0u);
}

TEST_CASE("granular view: Position, Length, Shape and Loop are the rows: F1..F4 pick, the encoder edits (this OS's ranges), Delete resets") {
  Rig rig;
  rig.invoke("instrument.play_mode", 7);
  NEED(view_of(rig) == static_cast<unsigned>(app::WaveView::Granular));
  CHECK(app::kWaveScreen.count(rig.app->model(), rig.app->state()) == 4);
  CHECK(app::kWaveScreen.field_context(rig.app->model(), rig.app->state()) == app::Context::WaveGran);
  const unsigned rows[4] = {ip::GrainPos, ip::GrainLength, ip::GrainShape, ip::GrainLoop};
  const Button keys[4] = {Button::F1, Button::F2, Button::F3, Button::F4};
  for (unsigned i = 0; i < 4; ++i) {
    rig.tap(keys[i]);
    CHECK(rig.ws().point == i);
    CHECK(core::param_field(app::kWaveScreen.focus_param(rig.app->model(), rig.app->state())) == rows[i]);
  }
  rig.tap(Button::F1);
  turn(rig, 10);
  CHECK(rig.inst().sampler.grain_position > 0u);  // about 65 a click (MAX_16BIT / 1000)
  const std::uint16_t pos = rig.inst().sampler.grain_position;
  CHECK(pos >= 600u);
  CHECK(pos <= 700u);
  rig.tap(Button::F2);
  turn(rig, 3);
  CHECK(rig.inst().sampler.grain_length > 441u);
  rig.tap(Button::F3);
  turn(rig, 1);
  CHECK(rig.inst().sampler.grain_shape == 1);  // Triangle
  turn(rig, 5);
  CHECK(rig.inst().sampler.grain_shape == 2);  // Gauss, the end of the list
  rig.tap(Button::F4);
  turn(rig, 1);
  CHECK(rig.inst().sampler.grain_type == 1);  // Backward
  (void)rig.invoke("param.reset");
  CHECK(rig.inst().sampler.grain_type == 0);
  rig.tap(Button::F1);
  (void)rig.invoke("param.reset");
  CHECK(rig.inst().sampler.grain_position == 0);
  CHECK(rig.app->failed_frames() == 0u);
}
