// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the Sample edit screen on the App: the selection, the six effects as jobs into a separate buffer, the commit (one project undo unit, one history step), the
// sample's own undo and redo with the points a crop moved, the handover to the voices, and the property the edit protocol rests on: the audio the engine may be reading
// is never written. The kernels are sample_dsp_test.cpp, the history sample_history_test.cpp.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/amiga_convert.h>
#include <o3os/app/app.h>
#include <o3os/app/ptfx_job.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/sample_edit_screen.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/core/gesture.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/module/ptfx_catalog.h>
#include <o3os/module/ptfx_format.h>
#include <o3os/platform/host/fakes.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::ScreenId;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];
constexpr std::uint32_t kFrames = 40000;
constexpr unsigned kSlot = 2;  // instrument 3

// The frames of kFrames converted to a Paula period, worked out from the clock in double (not from the code under test): kFrames x the PAL clock / (period x the rate the sample plays at),
// truncated. The sample plays at `src_hz` (0 = the engine's rate, 44,100 = its own rate) scaled to the engine's rate. At the 44,117.647 Hz clock: 7513 (period 428), 15027 (214); at 44,100 Hz: 7516 and 15033.
constexpr std::uint32_t amiga_len(unsigned period, unsigned src_hz = 0) {
  const double engine = static_cast<double>(hal::audio::sample_rate_mhz()) / 1000.0;
  const double play = src_hz == 0u ? engine : engine * static_cast<double>(src_hz) / 44100.0;
  return static_cast<std::uint32_t>(static_cast<double>(kFrames) * static_cast<double>(audio::paula::kPalClockHz) / (static_cast<double>(period) * play));
}

std::int16_t sample_at(std::uint32_t i) { return static_cast<std::int16_t>(std::lround(8000.0 * std::sin(0.03 * i + 0.4))); }

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
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  std::vector<std::int16_t> original;

  explicit Rig(std::uint32_t rate = 0) : memory(400000), peaks(40000) {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 400000, peaks.data(), peaks.size());
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::strcpy(in.name, "LOOP");
    in.sample_rate = rate;
    in.sampler.play_mode = 5;
    in.sampler.slice_count = 4;
    for (unsigned i = 0; i < 4; ++i) {
      in.sampler.slices[i] = static_cast<std::uint16_t>(i * 16383u);
    }
    NEED(arena->project.set_instrument(kSlot, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
      p[i] = sample_at(i);
    }
    NEED(bank.commit_load(kSlot, p, kFrames));
    bank.rebind_now(kSlot);
    original.assign(p, p + kFrames);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kSlot + 1));
    go(ScreenId::SampleEdit);
    passes(30);
    expire();
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
  void pad(unsigned n) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(n), 100));
    pass();
  }
  void invoke(const char* name, core::ActionArg arg = 0) {
    (void)app->invoke(core::action_id(name), arg);
    pass();
  }
  std::string message() const { return app->message(); }
  void expire() {
    for (unsigned i = 0; i < 10 && !std::string(app->message()).empty(); ++i) {
      clock.advance(3000000);
      app->run_once();
    }
  }
  // Runs the effect to its end: F-key, then passes until the edit job is gone and the strip's DONE has gone by.
  void run(Button b) {
    tap(b);
    passes(40);
    expire();
  }
  const model::Instrument& inst() const { return *arena->project.instrument(kSlot); }
  const app::SampleEditState& st() const { return app->state().sedit; }
  app::SampleBank::Pcm pcm() const { return bank.pcm(kSlot); }
  // The selection [a, b) in frames: the edge by Left / Right, the encoder's clicks of 52 frames (40000 / 768) each.
  void select(std::uint32_t clicks_a, std::uint32_t clicks_b) {
    tap(Button::Left);
    invoke("sedit.move", static_cast<core::ActionArg>(clicks_a));
    tap(Button::Right);
    invoke("sedit.move", -static_cast<core::ActionArg>(clicks_b));
  }
  void settle_engine() { passes(4); }
};

}  // namespace

TEST_CASE("sample edit: it is a screen of the registry, reached by its button, with the waveform built for the instrument in view") {
  Rig rig;
  const app::ScreenEntry* e = app::find_screen(app::screen_registry(), ScreenId::SampleEdit);
  NEED(e != nullptr);
  CHECK(e->screen == &app::kSampleEditScreen);
  CHECK(rig.app->state().screen == ScreenId::SampleEdit);
  CHECK(rig.app->peak_cache().built());
  CHECK(rig.app->peak_cache().samples() == rig.pcm().data);
  CHECK(rig.app->frames() >= 1u);
}

TEST_CASE("sample edit: Shift + F5 (NEXT INS) steps to the next instrument over 1..64 and goes round from 64 to 1 (OS-QAFIX-27, OS-SLD-RANGE-ALL)") {
  Rig rig;
  rig.invoke("instrument.select", 47);
  CHECK(rig.app->state().instrument == 47);
  rig.shift_tap(Button::F5);
  CHECK(rig.app->state().instrument == 48);
  rig.shift_tap(Button::F5);  // the pads reach 1..48 only, the step goes on to the slots 49..64 (empty here: any slot can be any type)
  CHECK(rig.app->state().instrument == 49);
  rig.tap(Button::F2);  // an empty slot: stepped onto, and the effects say why they do nothing
  CHECK(rig.message() == "NO SAMPLE IN THIS INSTRUMENT");
  for (int i = 0; i < 14; ++i) {  // 50..63 (a pad cannot give these: instrument.select refuses above 48)
    rig.shift_tap(Button::F5);
  }
  CHECK(rig.app->state().instrument == 63);
  rig.shift_tap(Button::F5);
  CHECK(rig.app->state().instrument == 64);
  rig.shift_tap(Button::F5);
  CHECK(rig.app->state().instrument == 1);
  rig.shift_tap(Button::F5);
  CHECK(rig.app->state().instrument == 2);
}

TEST_CASE("sample edit: the encoder moves the edge Left / Right picked, the selection starts as all of it and stays ordered, ALL clears it") {
  Rig rig;
  CHECK(rig.st().b == rig.st().a);
  rig.tap(Button::Left);
  rig.invoke("sedit.move", 10);
  CHECK(rig.st().a == 520u);        // 10 clicks of 40000 / 768 = 52 frames
  CHECK(rig.st().b == kFrames);
  rig.invoke("sedit.move_coarse", 1);
  CHECK(rig.st().a == 520u + 16u * 52u);
  rig.tap(Button::Right);
  rig.invoke("sedit.move", -100);
  CHECK(rig.st().b == kFrames - 5200u);
  rig.invoke("sedit.move", -100000);  // never past the start
  CHECK(rig.st().b == rig.st().a + 1u);
  rig.invoke("sedit.all");
  CHECK(rig.st().a == rig.st().b);
}

TEST_CASE("sample edit: reverse of a selection is one effect, a job into another buffer; the audio is the new buffer's, undo brings the original back by pointer") {
  Rig rig;
  const std::int16_t* original_ptr = rig.pcm().data;
  rig.select(100, 300);
  const std::uint32_t a = rig.st().a;
  const std::uint32_t b = rig.st().b;
  NEED(b > a);
  rig.tap(Button::F3);  // REVERSE
  CHECK(rig.message().find("EDIT REVERSE ") == 0u);
  rig.passes(40);
  CHECK(rig.pcm().data != original_ptr);
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    const std::int16_t want = (i >= a && i < b) ? sample_at(a + b - 1 - i) : sample_at(i);
    if (rig.pcm().data[i] != want) {
      FAIL_CHECK("frame " << i);
      break;
    }
  }
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  rig.tap(Button::F8);
  CHECK(rig.pcm().data == original_ptr);
  CHECK(rig.bank.redo_depth(kSlot) == 1u);
  rig.shift_tap(Button::F8);
  CHECK(rig.pcm().data != original_ptr);
  CHECK(rig.bank.redo_depth(kSlot) == 0u);
  // The peak cache follows the audio in view.
  rig.passes(40);
  CHECK(rig.app->peak_cache().samples() == rig.pcm().data);
}

TEST_CASE("sample edit: crop keeps the selection, moves the instrument's points and slices with it as an undo unit, and undo puts the points back with the audio") {
  Rig rig;
  const model::Instrument before = rig.inst();
  rig.select(200, 200);
  const std::uint32_t a = rig.st().a;
  const std::uint32_t b = rig.st().b;
  rig.run(Button::F1);
  CHECK(rig.pcm().frames == b - a);
  for (std::uint32_t i = 0; i < b - a; i += 97) {
    CHECK(rig.pcm().data[i] == sample_at(a + i));
  }
  // The points follow: the slices at 0, 25, 50, 75 %: those inside [a, b) = 26 %..74 %: the ones at 50 % stay (and the 25 % and 75 % are gone).
  CHECK(rig.inst().sampler.slice_count == 1u);
  CHECK(rig.inst().sampler.start == 0u);
  CHECK(rig.inst().sampler.end == 65535u);
  CHECK(rig.st().a == rig.st().b);  // the selection is spent
  // The sample's undo: audio and points together.
  rig.tap(Button::F8);
  CHECK(rig.pcm().frames == kFrames);
  CHECK(rig.inst().sampler.slice_count == before.sampler.slice_count);
  CHECK(std::memcmp(rig.inst().sampler.slices, before.sampler.slices, sizeof before.sampler.slices) == 0);
  // The project's journal saw the crop as a unit of its own: Pattern's F8 would undo it too (the points), and never the audio.
  CHECK(rig.editor->can_undo());
}

TEST_CASE("sample edit: crop needs a selection that is not all of it; the other effects take all of it when there is none") {
  Rig rig;
  rig.tap(Button::F1);
  CHECK(rig.message() == "SELECT WHAT TO KEEP FIRST");
  rig.expire();
  rig.tap(Button::Left);
  rig.invoke("sedit.move", 0);
  rig.invoke("sedit.all");
  rig.run(Button::F3);  // reverse: all of it
  CHECK(rig.pcm().data[0] == sample_at(kFrames - 1));
  CHECK(rig.pcm().data[kFrames - 1] == sample_at(0));
}

TEST_CASE("sample edit: normalise, fades and amplify do what they say on the selection; normalise of what is full scale already says there is nothing to do") {
  Rig rig;
  rig.select(100, 100);
  rig.run(Button::F2);  // normalise the selection: its peak becomes full scale
  std::int32_t peak = 0;
  for (std::uint32_t i = rig.st().a; i < rig.st().b; ++i) {
    peak = std::max<std::int32_t>(peak, std::abs(rig.pcm().data[i]));
  }
  CHECK(peak >= 32766);
  CHECK(rig.pcm().data[0] == sample_at(0));  // outside: untouched
  rig.tap(Button::F2);
  // The strip goes from "EDIT NORMALISE nn%" straight to the notice, the very pass the job ends (FQA-10b2: no DONE in between, none queued before it).
  unsigned n = 0;
  while (n < 200u && rig.message().find("EDIT NORMALISE") == 0u) {
    rig.pass();
    ++n;
  }
  CHECK(n < 200u);
  CHECK(rig.message() == "NOTHING TO NORMALISE");
  rig.passes(5);
  CHECK(rig.message() == "NOTHING TO NORMALISE");
  CHECK(rig.bank.undo_depth(kSlot) == 1u);  // no step was made
  rig.expire();
  rig.expire();
  // Fade in over the selection: its first frame is silent.
  rig.run(Button::F4);
  CHECK(rig.pcm().data[rig.st().a] == 0);
  // Amplify by the screen's percentage: Shift + Up twice is 220 %.
  rig.shift_tap(Button::Up);
  rig.shift_tap(Button::Up);
  CHECK(rig.st().amp == 220u);
  rig.shift_tap(Button::Down);
  CHECK(rig.st().amp == 210u);
  CHECK(rig.bank.undo_depth(kSlot) == 2u);  // the unchanged normalise made no step
}

TEST_CASE("sample edit: Shift + F4 removes the DC offset of the selection (or all of it) as one undo unit; undo restores the bytes exactly; a centred sample says there is nothing to do (OS-SEDFX)") {
  Rig rig;
  // The rig's sine has no offset to speak of; a fade in (a ramp over it) gives the whole sample one.
  rig.run(Button::F4);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  const std::vector<std::int16_t> before(rig.pcm().data, rig.pcm().data + kFrames);
  double mean_before = 0;
  for (const std::int16_t x : before) {
    mean_before += x;
  }
  mean_before /= kFrames;
  CHECK(std::abs(mean_before) > 1.0);
  rig.shift_tap(Button::F4);
  rig.passes(40);
  rig.expire();
  CHECK(rig.bank.undo_depth(kSlot) == 2u);  // one unit
  double mean_after = 0;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    mean_after += rig.pcm().data[i];
  }
  mean_after /= kFrames;
  CHECK(std::abs(mean_after) < 1.0);
  CHECK(rig.pcm().data[0] == before[0] - static_cast<int>(std::lround(mean_before)));
  // Centred already: nothing to do, no step.
  rig.shift_tap(Button::F4);
  rig.passes(40);
  CHECK(rig.bank.undo_depth(kSlot) == 2u);
  rig.expire();
  rig.expire();
  // Undo brings the original bytes back exactly.
  rig.tap(Button::F8);
  rig.passes(10);
  CHECK(std::memcmp(rig.pcm().data, before.data(), kFrames * sizeof(std::int16_t)) == 0);
  // A selection: only it moves.
  rig.select(100, 100);
  const std::uint32_t a = rig.st().a;
  const std::uint32_t b = rig.st().b;
  rig.shift_tap(Button::F4);
  rig.passes(40);
  rig.expire();
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    if (i < a || i >= b) {
      CHECK(rig.pcm().data[i] == before[i]);
    }
  }
  CHECK(rig.pcm().data[a] != before[a]);
  rig.tap(Button::F8);
  rig.passes(10);
  CHECK(std::memcmp(rig.pcm().data, before.data(), kFrames * sizeof(std::int16_t)) == 0);
}

TEST_CASE("sample edit: the audio the engine may be reading is never written: many edits while playback runs leave the original and what it is bound to as they were") {
  Rig rig;
  struct Transport final : app::TransportSink {
    bool play(unsigned) noexcept override { return true; }
    bool play_song(unsigned) noexcept override { return true; }
    bool stop() noexcept override { return true; }
  } transport;
  rig.app->attach_transport(&transport);
  rig.invoke("transport.play");
  NEED(rig.app->state().playing);
  const std::int16_t* bound = rig.voices->sample(kSlot).data;
  NEED(bound == rig.pcm().data);
  const Button effects[] = {Button::F3, Button::F4, Button::F5, Button::F6, Button::F3, Button::F6, Button::F5, Button::F4};
  for (int round = 0; round < 3; ++round) {
    for (const Button b : effects) {
      rig.run(b);
      CHECK(rig.voices->sample(kSlot).data == bound);  // not told: the voices' table is as it was
      CHECK(std::memcmp(bound, rig.original.data(), kFrames * 2u) == 0);
    }
  }
  CHECK(rig.bank.rebind_pending());
  CHECK(rig.pcm().data != bound);
  // Stopped, the engine takes the latest.
  rig.invoke("transport.play");
  rig.settle_engine();
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(kSlot).data == rig.pcm().data);
}

TEST_CASE("sample edit: a result for a sample that was replaced while the job ran is thrown away; no sample, no room: nothing starts") {
  Rig rig;
  rig.tap(Button::F3);
  rig.passes(1);
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  for (std::uint32_t i = 0; i < 2000; ++i) {
    p[i] = 5;
  }
  NEED(rig.bank.commit_load(kSlot, p, 2000));  // another sample in the slot meanwhile
  rig.passes(60);
  CHECK(rig.pcm().data == p);
  CHECK(rig.pcm().frames == 2000u);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  // An instrument with no sample.
  rig.invoke("instrument.select", 9);
  rig.expire();
  rig.tap(Button::F3);
  CHECK(rig.message() == "NO SAMPLE IN THIS INSTRUMENT");
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO");
}

TEST_CASE("sample edit: a random walk over the keys keeps the selection and window inside the sample whatever the effects did to its length") {
  Rig rig;
  std::uint32_t s = 99u;
  const auto next = [&](unsigned n) {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) % n;
  };
  const Button keys[] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6, Button::F7, Button::F8, Button::Left, Button::Right, Button::Up, Button::Down};
  for (int i = 0; i < 300; ++i) {
    switch (next(4)) {
      case 0: rig.tap(keys[next(12)]); break;
      case 1: rig.invoke("sedit.move", static_cast<core::ActionArg>(next(61)) - 30); break;
      case 2: rig.invoke("sedit.move_coarse", static_cast<core::ActionArg>(next(21)) - 10); break;
      default: rig.passes(next(20) + 1); break;
    }
    const std::uint32_t frames = rig.pcm().frames;
    const app::SampleEditState& e = rig.st();
    CHECK(e.b <= frames);
    CHECK(e.a <= e.b);
    CHECK(e.first + e.span <= frames);
    CHECK(frames >= 1u);
    CHECK(rig.bank.undo_depth(kSlot) <= app::SampleBank::kHistory);
  }
}

TEST_CASE("sample edit: F7 PREVIEW plays the instrument while it is down, Shift + F7 from the edge being moved") {
  struct FakePreview final : app::PreviewSink {
    std::vector<int> holds;
    std::vector<bool> auditions;
    void preview(unsigned, unsigned) noexcept override {}
    void hold_note(bool on, unsigned, unsigned instrument, unsigned) noexcept override { holds.push_back(on ? static_cast<int>(instrument) : -static_cast<int>(instrument)); }
    void audition(bool on, unsigned) noexcept override { auditions.push_back(on); }
  } preview;
  Rig rig;
  rig.app->attach_preview(&preview);
  model::Instrument in = *rig.arena->project.instrument(kSlot);
  in.sampler.play_mode = 0;  // not a slice instrument
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  rig.press(Button::F7);
  rig.pass();
  NEED(preview.holds.size() == 1u);
  CHECK(preview.holds[0] == static_cast<int>(kSlot + 1));
  rig.release(Button::F7);
  rig.pass();
  NEED(preview.holds.size() == 2u);
  CHECK(preview.holds[1] == -static_cast<int>(kSlot + 1));
  // Shift + F7 with no selection: from the left edge of the window (the whole sample here), a one shot on the audition's descriptor.
  rig.press(Button::Shift);
  rig.pass(1);
  rig.press(Button::F7);
  rig.pass();
  rig.pass();
  NEED(preview.auditions.size() == 1u);
  CHECK(preview.auditions[0]);
  CHECK(rig.voices->sample(audio::kPreviewSlot).length == kFrames);
  rig.release(Button::F7);
  rig.pass();
  CHECK(preview.auditions.size() == 2u);
  rig.release(Button::Shift);
  rig.pass(1);
}

// ---- OS-AMG1: the AMIGA conversion ----

namespace {

// Shift + F1 / F2 and passes until the audio has changed length (the job: kernel, then slices).
bool convert_amiga(Rig& rig, Button key, std::uint32_t expect_frames) {
  rig.shift_tap(key);
  for (unsigned i = 0; i < 600 && rig.pcm().frames != expect_frames; ++i) {
    rig.pass();
  }
  rig.passes(8);  // (the handover: stopped and quiet)
  rig.expire();
  return rig.pcm().frames == expect_frames;
}

std::vector<std::int16_t> amiga_expected(const std::vector<std::int16_t>& src, unsigned period, bool q8) {
  static app::AmigaKernel k;
  while (!k.ready()) {
    (void)k.build(256);
  }
  const std::uint64_t step = app::amiga_step_q32(period, false);
  std::vector<std::int16_t> out(app::amiga_frames(static_cast<std::uint32_t>(src.size()), step));
  app::amiga_resample(k, src.data(), static_cast<std::uint32_t>(src.size()), step, q8, out.data(), 0, static_cast<std::uint32_t>(out.size()));
  return out;
}

}  // namespace

TEST_CASE("sample edit: AMIGA 8287 resamples the whole sample as one job and one undo unit; engine, rate and points follow; undo brings back the audio and the fields exactly") {
  Rig rig;
  const std::int16_t* original_ptr = rig.pcm().data;
  const model::Instrument before = rig.inst();
  rig.select(100, 300);  // a selection means nothing to a conversion: all of it is converted
  const std::uint32_t want = app::amiga_frames(kFrames, app::amiga_step_q32(app::kAmigaPeriod8, false));
  rig.shift_tap(Button::F1);
  CHECK(rig.message().find("EDIT AMIGA 8287 ") == 0u);  // a job on the strip, with its progress
  for (unsigned i = 0; i < 600 && rig.pcm().frames != want; ++i) {
    rig.pass();
  }
  rig.passes(8);
  rig.expire();
  NEED(rig.pcm().frames == want);
  CHECK(want == amiga_len(app::kAmigaPeriod8));  // 40000 / 5.3215 at 44,100 Hz (5.3236 at 44,117.647 Hz)
  const std::vector<std::int16_t> got(rig.pcm().data, rig.pcm().data + want);
  CHECK(got == amiga_expected(rig.original, app::kAmigaPeriod8, false));
  CHECK(rig.inst().engine == model::kInstEngineAmiga);
  CHECK(rig.inst().sample_rate == 8287u);
  CHECK(rig.inst().sampler.tune == before.sampler.tune);
  CHECK(rig.inst().sampler.loop_end == before.sampler.loop_end);  // the points are fractions of the length
  CHECK(rig.inst().sampler.slices[1] == before.sampler.slices[1]);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  CHECK(rig.st().a == rig.st().b);  // the selection of the old length is gone
  // The voices play it as an Amiga sample at the new length.
  CHECK(rig.voices->sample(kSlot).engine == audio::kEngineAmiga);
  CHECK(rig.voices->sample(kSlot).length == want);
  // Undo: the very same audio (by pointer and by content), the instrument as it was.
  rig.tap(Button::F8);
  rig.passes(8);
  CHECK(rig.pcm().data == original_ptr);
  CHECK(rig.pcm().frames == kFrames);
  CHECK(std::vector<std::int16_t>(rig.pcm().data, rig.pcm().data + kFrames) == rig.original);
  CHECK(std::memcmp(&rig.inst(), &before, sizeof before) == 0);
  CHECK(rig.voices->sample(kSlot).engine == audio::kEngineModern);
  rig.shift_tap(Button::F8);  // redo
  rig.passes(8);
  CHECK(rig.pcm().frames == want);
  CHECK(rig.inst().engine == model::kInstEngineAmiga);
  CHECK(rig.inst().sample_rate == 8287u);
}

TEST_CASE("sample edit: the Amiga conversion of a 22,050 Hz sample converts from its own rate, and undo and redo rebind the voices' rate factor (OS-RATEPITCH)") {
  Rig rig(22050);
  const model::Instrument before = rig.inst();
  CHECK(rig.voices->sample(kSlot).rate_q28 == (1u << 27));  // half the engine's rate: the voice advances half a frame a sample at the root
  CHECK(rig.voices->sample(kSlot).engine == audio::kEngineModern);
  const std::uint32_t want = app::amiga_frames(kFrames, app::amiga_step_q32(app::kAmigaPeriod8, false, 22050));
  CHECK(want == amiga_len(app::kAmigaPeriod8, 22050));  // 40000 x 8,287.14 / 22,050 x the engine's rate over 44,100
  NEED(convert_amiga(rig, Button::F1, want));
  CHECK(rig.inst().engine == model::kInstEngineAmiga);
  CHECK(rig.inst().sample_rate == 8287u);
  CHECK(rig.voices->sample(kSlot).engine == audio::kEngineAmiga);
  CHECK(rig.voices->sample(kSlot).rate_q28 == audio::rate_factor_q28(8287u));  // rebound to the new rate (the Amiga voice does not read it)
  rig.tap(Button::F8);  // undo: the 22,050 Hz sample, the Modern engine and its factor
  rig.passes(8);
  CHECK(std::memcmp(&rig.inst(), &before, sizeof before) == 0);
  CHECK(rig.voices->sample(kSlot).engine == audio::kEngineModern);
  CHECK(rig.voices->sample(kSlot).length == kFrames);
  CHECK(rig.voices->sample(kSlot).rate_q28 == (1u << 27));
  rig.shift_tap(Button::F8);  // redo
  rig.passes(8);
  CHECK(rig.voices->sample(kSlot).rate_q28 == audio::rate_factor_q28(8287u));
  CHECK(rig.voices->sample(kSlot).engine == audio::kEngineAmiga);
}

TEST_CASE("sample edit: AMIGA 16574 keeps the pitch with tune +12, undo takes the tune back; a tune with no room, a second 8287 and the 8-bit option") {
  Rig rig;
  const std::uint32_t want = app::amiga_frames(kFrames, app::amiga_step_q32(app::kAmigaPeriod16, false));
  NEED(convert_amiga(rig, Button::F2, want));
  CHECK(want == amiga_len(app::kAmigaPeriod16));  // 40000 / 2.6608 at 44,100 Hz (2.6618 at 44,117.647 Hz)
  CHECK(rig.inst().sampler.tune == 12);
  CHECK(rig.inst().sample_rate == 16574u);
  CHECK(rig.inst().engine == model::kInstEngineAmiga);
  CHECK(rig.voices->sample(kSlot).tune == 12);
  rig.tap(Button::F8);
  rig.passes(8);
  CHECK(rig.inst().sampler.tune == 0);
  CHECK(rig.inst().sample_rate == 0u);
  CHECK(rig.inst().engine == model::kInstEngineAuto);
  // A tune that cannot take +12 refuses the 16 kHz conversion and starts nothing.
  model::Instrument hi = rig.inst();
  hi.sampler.tune = 13;
  NEED(rig.arena->project.set_instrument(kSlot, hi) == model::Status::Ok);
  rig.shift_tap(Button::F2);
  rig.passes(20);
  CHECK(rig.pcm().frames == kFrames);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  hi.sampler.tune = 12;
  NEED(rig.arena->project.set_instrument(kSlot, hi) == model::Status::Ok);
  // The 8-bit option: every frame is a level of 8 bits (a multiple of 256).
  rig.shift_tap(Button::F3);
  CHECK(rig.st().q8);
  NEED(convert_amiga(rig, Button::F1, amiga_len(app::kAmigaPeriod8)));
  for (std::uint32_t i = 0; i < amiga_len(app::kAmigaPeriod8); ++i) {
    if (rig.pcm().data[i] % 256 != 0) {
      FAIL_CHECK("frame " << i);
      break;
    }
  }
  CHECK(rig.inst().sampler.tune == 12);  // 8,287: the tune is the user's own
  // Converted already: another 8,287 from an Amiga instrument is a no-op and says so.
  rig.shift_tap(Button::F1);
  rig.passes(20);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
}

TEST_CASE("sample edit: after the conversion CONVERT TO AMIGA FIRST no longer refuses AMIGA") {
  Rig rig;
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), model::kInstEngineAmiga) == app::DispatchResult::Unchanged);  // 44.1 kHz: Paula cannot play it
  CHECK(rig.message() == "CONVERT TO AMIGA FIRST");
  rig.expire();
  NEED(convert_amiga(rig, Button::F1, amiga_len(app::kAmigaPeriod8)));
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), model::kInstEngineModern) == app::DispatchResult::Changed);
  rig.expire();
  CHECK(rig.app->invoke(core::action_id("instrument.engine"), model::kInstEngineAmiga) == app::DispatchResult::Changed);
  CHECK(rig.inst().engine == model::kInstEngineAmiga);
  CHECK(rig.message() == "ENGINE AMIGA");
}

// ---- OS-INSTPICK: the held Instr's list of the samples ---------------------------------------------------------------------------------------------------------------------------

namespace {

void add_instrument(Rig& rig, unsigned slot, model::InstrumentKind kind, std::uint32_t frames) {
  model::Instrument in;
  model::init_instrument(in, kind);
  if (kind == model::InstrumentKind::Midi) {
    in.midi_channel = 1;
  } else if (kind == model::InstrumentKind::Module) {
    (void)rig.arena->project.add_module("tb303", 5, 1, 0);
    in.module_index = 0;
  }
  NEED(rig.arena->project.set_instrument(slot, in) == model::Status::Ok);
  if (frames != 0u) {
    std::uint32_t cap = 0;
    std::int16_t* p = rig.bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = static_cast<std::int16_t>(i % 50);
    }
    NEED(rig.bank.commit_load(slot, p, frames));
  }
}

void hold_instr(Rig& rig) {
  rig.press(Button::Instr);
  rig.pass(core::kGestureTimings.hold_ms + 1);
}

}  // namespace

TEST_CASE("sample edit: a held Instr opens the instrument list; only the samples can be chosen, and the new instrument starts with no selection and a fitted window") {
  Rig rig;
  add_instrument(rig, 3, model::InstrumentKind::Midi, 0);     // instrument 4
  add_instrument(rig, 4, model::InstrumentKind::Module, 0);   // instrument 5
  add_instrument(rig, 5, model::InstrumentKind::Sample, 6000);  // instrument 6
  rig.select(100, 300);
  rig.tap(Button::Up);  // zoomed in
  NEED(rig.st().b > rig.st().a);
  NEED(rig.st().span != 0u);
  const unsigned depth = rig.journal.depth();
  const app::ViewState& v = rig.app->state();

  hold_instr(rig);
  CHECK(v.overlay == app::OverlayId::StepPopup);
  CHECK(v.popup.kind == app::StepPopupView::Instr);
  CHECK(v.popup.samples_only);
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  const app::Model& m = rig.app->model();
  CHECK(app::step_popup_pickable(m, v, kSlot));
  CHECK(app::step_popup_pickable(m, v, 5));
  CHECK_FALSE(app::step_popup_pickable(m, v, 3));
  CHECK_FALSE(app::step_popup_pickable(m, v, 4));
  CHECK_FALSE(app::step_popup_pickable(m, v, 0));
  rig.pad(3);  // a pad of a MIDI slot: nothing
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  CHECK(rig.st().b > rig.st().a);
  rig.tap(Button::Up);  // nothing before the sample
  CHECK(v.instrument == static_cast<int>(kSlot) + 1);
  rig.tap(Button::Down);  // over the MIDI and module slots to the next sample
  CHECK(v.instrument == 6);
  CHECK(rig.st().a == 0u);
  CHECK(rig.st().b == 0u);
  CHECK(rig.st().span == 0u);
  CHECK(rig.st().first == 0u);
  rig.release(Button::Instr);
  rig.pass(1);
  CHECK(v.overlay == app::OverlayId::None);
  CHECK(v.instrument == 6);
  rig.passes(20);
  CHECK(rig.app->peak_cache().samples() == rig.bank.pcm(5).data);  // the waveform and the length of the new one
  CHECK(rig.app->peak_cache().frames() == 6000u);
  CHECK(rig.journal.depth() == depth);
  // its own selection is made on it, and an effect acts on it, not on the one left
  const std::int16_t* left = rig.bank.pcm(kSlot).data;
  rig.tap(Button::F3);  // REVERSE (all of it)
  rig.passes(40);
  CHECK(rig.bank.pcm(kSlot).data == left);
  CHECK(rig.bank.undo_depth(5) == 1u);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
}

TEST_CASE("sample edit: an effect still running when the instrument is changed lands on the instrument it was started on, nothing is lost") {
  Rig rig;
  add_instrument(rig, 5, model::InstrumentKind::Sample, 6000);
  const std::int16_t* original_ptr = rig.pcm().data;
  rig.select(100, 300);
  rig.tap(Button::F3);  // REVERSE, a job
  hold_instr(rig);
  rig.tap(Button::Down);
  CHECK(rig.app->state().instrument == 6);
  rig.release(Button::Instr);
  rig.passes(60);
  CHECK(rig.pcm().data != original_ptr);  // the reversed audio of the old instrument
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  CHECK(rig.bank.undo_depth(5) == 0u);
  CHECK(rig.app->state().instrument == 6);
  CHECK(rig.st().b == rig.st().a);  // and the selection in view is the new instrument's: none
}

TEST_CASE("sample edit: the Sample edit button again opens the FX page; the arrows pick a row, the encoder changes it; F3..F6 do nothing there (OS-AUDIT-15)") {
  Rig rig;
  CHECK_FALSE(app::sedit_fx_page(rig.app->state()));
  rig.tap(Button::SampleEdit);
  CHECK(app::sedit_fx_page(rig.app->state()));
  CHECK(rig.app->state().screen == ScreenId::SampleEdit);
  CHECK(rig.st().fx_sel == 0u);
  CHECK(rig.st().fx[app::kFxCutoff] == 1000);
  rig.tap(Button::Down);
  CHECK(rig.st().fx_sel == 1u);
  rig.invoke("sedit.fx", -4);
  CHECK(rig.st().fx[app::kFxCutoff] < 1000);
  CHECK(rig.st().fx[app::kFxCutoff] >= 20);
  rig.invoke("sedit.fx_coarse", 1000000);  // held to the range
  CHECK(rig.st().fx[app::kFxCutoff] == 20000);
  rig.invoke("sedit.fx_coarse", -1000000);
  CHECK(rig.st().fx[app::kFxCutoff] == 20);
  rig.invoke("sedit.fx", 1);
  CHECK(rig.st().fx[app::kFxCutoff] > 20);
  rig.tap(Button::Up);
  rig.invoke("sedit.fx", 1);  // TYPE: HIGH PASS
  CHECK(rig.st().fx[app::kFxType] == 1);
  rig.tap(Button::Up);  // round to the last row
  CHECK(rig.st().fx_sel == app::kFxCount - 1u);
  // F3..F6 are the edit page's keys behind blank labels: they start nothing.
  for (const Button b : {Button::F3, Button::F4, Button::F5, Button::F6}) {
    rig.tap(b);
    rig.passes(30);
  }
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(std::memcmp(rig.pcm().data, rig.original.data(), kFrames * sizeof(std::int16_t)) == 0);
  // The button again opens the PLUGINS page (OS-CEPTFX S4a), and once more goes back to the edit page, where F3 is REVERSE.
  rig.tap(Button::SampleEdit);
  CHECK(app::sedit_plug_page(rig.app->state()));
  CHECK_FALSE(app::sedit_fx_page(rig.app->state()));
  rig.tap(Button::SampleEdit);
  CHECK_FALSE(app::sedit_plug_page(rig.app->state()));
  CHECK_FALSE(app::sedit_fx_page(rig.app->state()));
  rig.run(Button::F3);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
}

TEST_CASE("sample edit: FILTER and COMPRESSOR (F1 / F2 on the FX page) act on the selection or all of the sample as one undo unit each; undo brings the bytes back (OS-AUDIT-15)") {
  Rig rig;
  const std::vector<std::int16_t> before = rig.original;
  rig.tap(Button::SampleEdit);
  // FILTER: the default low pass at 1 kHz over a 210 Hz tone changes little; a cutoff of 20 Hz takes nearly all of it. All of the sample, no selection.
  rig.tap(Button::Down);
  rig.invoke("sedit.fx_coarse", -1000000);
  CHECK(rig.st().fx[app::kFxCutoff] == 20);
  rig.run(Button::F1);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  double rms_before = 0;
  double rms_after = 0;
  for (std::uint32_t i = 20000; i < kFrames; ++i) {
    rms_before += static_cast<double>(before[i]) * before[i];
    rms_after += static_cast<double>(rig.pcm().data[i]) * rig.pcm().data[i];
  }
  CHECK(rms_after < 0.1 * rms_before);
  rig.tap(Button::F8);
  rig.passes(10);
  CHECK(std::memcmp(rig.pcm().data, before.data(), kFrames * sizeof(std::int16_t)) == 0);
  // COMPRESSOR over a selection: only it changes (auto makeup raises it).
  rig.tap(Button::SampleEdit);  // (the edit page: the selection is made there)
  rig.select(100, 100);
  const std::uint32_t a = rig.st().a;
  const std::uint32_t b = rig.st().b;
  CHECK(b > a);
  rig.tap(Button::SampleEdit);
  rig.run(Button::F2);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);  // (the filter's step was undone: its redo was dropped by this edit)
  bool outside_same = true;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    if ((i < a || i >= b) && rig.pcm().data[i] != before[i]) {
      outside_same = false;
    }
  }
  CHECK(outside_same);
  CHECK(std::memcmp(rig.pcm().data + a, before.data() + a, (b - a) * sizeof(std::int16_t)) != 0);
  rig.tap(Button::F8);
  rig.passes(10);
  CHECK(std::memcmp(rig.pcm().data, before.data(), kFrames * sizeof(std::int16_t)) == 0);
}

TEST_CASE("sample edit: F8 cancels a running FX job (OS-SEDIT-CANCEL): the key row reads CANCEL, the sample is bit-identical, no history step, no project undo unit, the strip says CANCELLED") {
  Rig rig;
  const std::vector<std::int16_t> before = rig.original;
  const std::int16_t* const before_ptr = rig.pcm().data;
  rig.tap(Button::SampleEdit);  // the FX page
  rig.passes(2);
  const unsigned units = rig.journal.depth();
  CHECK_FALSE(rig.st().running);
  rig.tap(Button::F2);  // COMPRESSOR over all of it: a job of several passes
  NEED(rig.st().running);
  rig.tap(Button::F1);  // a second effect does not start (the job goes on)
  CHECK(rig.st().running);
  rig.tap(Button::F8);  // CANCEL
  rig.passes(3);
  CHECK_FALSE(rig.st().running);
  CHECK(rig.message() == "EDIT COMPRESSOR CANCELLED");
  CHECK(rig.pcm().data == before_ptr);
  CHECK(std::memcmp(rig.pcm().data, before.data(), kFrames * sizeof(std::int16_t)) == 0);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(rig.journal.depth() == units);
  // F8 is UNDO again, and a new effect starts and completes.
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.message() == "NOTHING TO UNDO");
  rig.run(Button::F1);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
}

// ---- OS-CEPTFX S4a: the PLUGINS page over an in-memory card of header-only plugins -------------------------------------------------------------------------------------------------------------------------------

namespace {

void p16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
  v[at] = static_cast<std::uint8_t>(x);
  v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void p32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  p16(v, at, static_cast<std::uint16_t>(x));
  p16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}
void pf(std::vector<std::uint8_t>& v, std::size_t at, float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  p32(v, at, u);
}

struct PParam {
  const char* label;
  char type;
  float min, max, def, step;
  const char* unit;
  unsigned items;  // 'l': the list SINE / TRI / SQUARE, this many of them
};

// The header of the format and 64 bytes of code that never runs; a list parameter's strings follow them, inside the code size.
std::vector<std::uint8_t> make_plug(const char* name, const char* author, std::uint16_t flags, const std::vector<PParam>& params, std::uint16_t version = 1) {
  static const char kList[] = "SINE\0TRI\0SQUARE\0";
  std::vector<std::uint8_t> payload(64, 0);
  std::uint16_t at = 0;
  for (const PParam& q : params) {
    if (q.type == 'l') {
      at = static_cast<std::uint16_t>(payload.size());
      const char* s = kList;
      for (unsigned k = 0; k < q.items; ++k) {
        const std::size_t len = std::strlen(s) + 1u;
        payload.insert(payload.end(), s, s + len);
        s += len;
      }
    }
  }
  std::vector<std::uint8_t> v(module::kPtfxHeaderSize + payload.size(), 0);
  p32(v, 0, module::kPtfxMagic);
  p16(v, 4, version);
  p16(v, 6, flags);
  std::memcpy(&v[8], name, std::strlen(name));
  std::memcpy(&v[40], author, std::strlen(author));
  v[56] = static_cast<std::uint8_t>(params.size());
  p32(v, 64, module::kPtfxHeaderSize);
  p32(v, 68, static_cast<std::uint32_t>(payload.size()));
  p32(v, 80, 8);
  p32(v, 84, 16);
  for (std::size_t i = 0; i < params.size(); ++i) {
    const std::size_t o = 88u + 32u * i;
    std::memcpy(&v[o], params[i].label, std::strlen(params[i].label));
    pf(v, o + 8, params[i].min);
    pf(v, o + 12, params[i].max);
    pf(v, o + 16, params[i].def);
    pf(v, o + 20, params[i].step);
    if (params[i].type == 'l') {
      p16(v, o + 24, at);
      v[o + 26] = static_cast<std::uint8_t>(params[i].items);
    } else {
      std::memcpy(&v[o + 24], params[i].unit, std::strlen(params[i].unit));
    }
    v[o + 30] = static_cast<std::uint8_t>(params[i].type);
  }
  std::memcpy(&v[module::kPtfxHeaderSize], payload.data(), payload.size());
  return v;
}

const std::vector<PParam> kAlphaParams = {
    {"RATE", 'f', 0.1f, 10.0f, 1.5f, 0.1f, "Hz", 0},
    {"VOICES", 'd', 1.0f, 6.0f, 3.0f, 1.0f, "", 0},
    {"SHAPE", 'l', 0.0f, 2.0f, 0.0f, 1.0f, "", 3},
};

// The Rig with a card: /Effects holds ALPHA (stereo, 3 parameters: 'f', 'd' and a list), BETA (a generator, one parameter), a file of another version (a refused row) and TWELVE (12 parameters), then `filler` more.
struct PlugRig : Rig {
  test::MemCard card;
  module::PtfxEntry entries[module::kPtfxCatalogMax];
  module::PtfxCatalog cat{entries};

  explicit PlugRig(unsigned filler = 0) {
    card.make_dir_now("Effects");
    card.put("Effects/alpha.ptfx", make_plug("ALPHA", "ANN", module::kPtfxFlagStereo, kAlphaParams));
    card.put("Effects/beta.ptfx", make_plug("BETA", "", module::kPtfxFlagGenerator, {{"LEVEL", 'f', 0.0f, 1.0f, 0.5f, 0.0f, "", 0}}));
    card.put("Effects/future.ptfx", make_plug("FUTURE", "", 0, {}, 2));
    std::vector<PParam> twelve;
    for (unsigned i = 0; i < 12; ++i) {
      twelve.push_back({"P", 'd', 0.0f, 100.0f, 10.0f, 1.0f, "", 0});
    }
    card.put("Effects/twelve.ptfx", make_plug("TWELVE", "", 0, twelve));
    for (unsigned i = 0; i < filler; ++i) {
      const unsigned n = i % 100;  // two digits: the compiler sees the buffers are big enough
      char file[24];
      std::snprintf(file, sizeof file, "Effects/f%02u.ptfx", n);
      char name[8];
      std::snprintf(name, sizeof name, "F%02u", n);
      card.put(file, make_plug(name, "", 0, kAlphaParams));
    }
    app->attach_storage(&card);
    bank.ptfx_page()->cat = &cat;
  }
  app::PtfxPage& page() { return *bank.ptfx_page(); }
  // The page, its scan finished and the first plugin read.
  void open_page() {
    tap(Button::SampleEdit);
    tap(Button::SampleEdit);
    NEED(app::sedit_plug_page(app->state()));
    passes(8);
    for (unsigned guard = 0; cat.state() == module::PtfxCatalog::State::Scanning && guard < 600u; ++guard) {
      pass();  // one header read a step: a long folder takes a pass a file
    }
    passes(2);  // the first plugin's header
  }
  void down(unsigned n = 1) {
    for (unsigned i = 0; i < n; ++i) {
      tap(Button::Down);
      passes(2);
    }
  }
  void up(unsigned n = 1) {
    for (unsigned i = 0; i < n; ++i) {
      tap(Button::Up);
      passes(2);
    }
  }
  std::string shown(unsigned p) {
    core::FixedString<app::PtfxPage::kValueChars> buf;
    page().format_value(p, buf);
    return buf.c_str();
  }
};

}  // namespace

TEST_CASE("sample edit PLUGINS: the page scans /Effects when it opens, lists the files sorted with the refused one in its place, and reads the header of the row in view") {
  PlugRig rig;
  CHECK(rig.cat.state() == module::PtfxCatalog::State::Idle);  // nothing is read before the page is opened
  rig.open_page();
  CHECK(rig.cat.state() == module::PtfxCatalog::State::Done);
  NEED(rig.cat.count() == 4u);
  CHECK(std::strcmp(rig.cat.entry(0).display_name, "ALPHA") == 0);
  CHECK(std::strcmp(rig.cat.entry(1).display_name, "BETA") == 0);
  CHECK(rig.cat.entry(2).status != module::PtfxStatus::Ok);  // version 2
  CHECK(std::strcmp(rig.cat.entry(3).display_name, "TWELVE") == 0);
  NEED(rig.page().ready());
  CHECK(rig.page().params() == 3u);
  CHECK(std::strcmp(rig.page().info().author, "ANN") == 0);
  CHECK(rig.shown(0) == "1.50 Hz");  // 'f': two decimals, the unit
  CHECK(rig.shown(1) == "3");        // 'd': whole, no unit
  CHECK(rig.shown(2) == "SINE");     // a list: its item
  CHECK(rig.st().plug_focus == 0u);
  // The page left and opened again keeps the listing: no second scan.
  rig.tap(Button::SampleEdit);
  rig.passes(4);
  CHECK(rig.cat.state() == module::PtfxCatalog::State::Done);
  CHECK(rig.cat.count() == 4u);
}

TEST_CASE("sample edit PLUGINS: Up / Down pick the row of the focused column, Left / Right the focus; the encoder picks the plugin or changes the value, Shift x10; Delete and F3 put defaults back") {
  PlugRig rig;
  rig.open_page();
  NEED(rig.cat.count() == 4u);
  rig.down();  // BETA
  CHECK(rig.st().plug_sel == 1u);
  CHECK(rig.page().params() == 1u);
  CHECK(rig.shown(0) == "0.50");
  rig.invoke("sedit.plug", -1);  // the encoder picks the plugin while the list has the focus
  CHECK(rig.st().plug_sel == 0u);
  rig.passes(2);
  NEED(rig.page().params() == 3u);
  rig.tap(Button::Right);  // the parameters
  CHECK(rig.st().plug_focus == 1u);
  CHECK(rig.st().plug_param == 0u);
  rig.tap(Button::Down);
  CHECK(rig.st().plug_param == 1u);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // the last row: it stays
  CHECK(rig.st().plug_param == 2u);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  CHECK(rig.st().plug_param == 0u);
  rig.invoke("sedit.plug", 5);  // RATE 1.5 + 5 x 0.1
  CHECK(rig.page().value(0) == doctest::Approx(2.0f));
  CHECK(rig.shown(0) == "2.00 Hz");
  rig.invoke("sedit.plug_coarse", 1);  // x10: + 1.00
  CHECK(rig.shown(0) == "3.00 Hz");
  rig.invoke("sedit.plug_coarse", 1000);  // held to the range
  CHECK(rig.shown(0) == "10.00 Hz");
  rig.tap(Button::Delete);  // this parameter's default
  CHECK(rig.shown(0) == "1.50 Hz");
  rig.tap(Button::Down);  // VOICES: whole numbers, 1..6
  rig.invoke("sedit.plug", 2);
  CHECK(rig.shown(1) == "5");
  rig.invoke("sedit.plug", 99);
  CHECK(rig.shown(1) == "6");
  rig.invoke("sedit.plug", -99);
  CHECK(rig.shown(1) == "1");
  rig.tap(Button::Down);  // SHAPE: a list, one item a click
  rig.invoke("sedit.plug", 1);
  CHECK(rig.shown(2) == "TRI");
  rig.invoke("sedit.plug", 5);  // the last item stays
  CHECK(rig.shown(2) == "SQUARE");
  rig.tap(Button::F3);  // DEFAULTS: every parameter
  CHECK(rig.shown(0) == "1.50 Hz");
  CHECK(rig.shown(1) == "3");
  CHECK(rig.shown(2) == "SINE");
  rig.tap(Button::Left);  // back to the list
  CHECK(rig.st().plug_focus == 0u);
  for (const Button b : {Button::F4, Button::F5, Button::F6}) {  // F4..F6 do nothing here
    rig.tap(b);
  }
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
}

TEST_CASE("sample edit PLUGINS: 12 parameters in two columns of six; Right and Left move between the columns, Up / Down stay inside one") {
  PlugRig rig;
  rig.open_page();
  rig.down(3);  // TWELVE
  NEED(rig.page().params() == 12u);
  rig.tap(Button::Right);
  CHECK(rig.st().plug_focus == 1u);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.st().plug_param == 2u);
  rig.tap(Button::Right);  // the same row of the second column
  CHECK(rig.st().plug_param == 8u);
  for (unsigned i = 0; i < 9; ++i) {
    rig.tap(Button::Down);
  }
  CHECK(rig.st().plug_param == 11u);  // the end of the second column
  rig.tap(Button::Left);
  CHECK(rig.st().plug_param == 5u);  // the first column's last row
  rig.tap(Button::Left);  // from the first column: the list
  CHECK(rig.st().plug_focus == 0u);
}

TEST_CASE("sample edit PLUGINS: a refused row reads nothing and takes no focus or value; F1 APPLY on it says why (S4b)") {
  PlugRig rig;
  rig.open_page();
  rig.down(2);  // FUTURE, version 2
  CHECK(rig.st().plug_sel == 2u);
  CHECK_FALSE(rig.page().ready());
  CHECK(rig.page().params() == 0u);
  rig.tap(Button::Right);  // no parameters to focus
  CHECK(rig.st().plug_focus == 0u);
  rig.tap(Button::F3);
  rig.tap(Button::Delete);
  CHECK_FALSE(rig.page().ready());
  rig.tap(Button::F1);
  CHECK(rig.message() == "PTFX VERSION NOT SUPPORTED");  // S4b: APPLY says why the row is refused
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(std::memcmp(rig.pcm().data, rig.original.data(), kFrames * sizeof(std::int16_t)) == 0);
  rig.down();  // the row after it is read as usual
  CHECK(rig.page().ready());
  CHECK(rig.page().params() == 12u);
}

namespace {

constexpr std::uintptr_t kPInit = 0x100;
constexpr std::uintptr_t kPSet = 0x200;
constexpr std::uintptr_t kPProc = 0x300;

// A stand-in for the plugin's code: doubles the body (the S3 tests' gain).
hal::code_gate::GateResult double_call(const app::PtfxJob::Plugin&, std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t, float,
                                       std::int32_t* ret) noexcept {
  *ret = 0;
  if (entry == kPProc) {
    const auto* in = reinterpret_cast<const std::int16_t* const*>(r0);
    auto* out = reinterpret_cast<std::int16_t* const*>(r1);
    const std::uint32_t n = static_cast<std::uint32_t>(r2);
    for (std::uint32_t i = 0; i < n; ++i) {
      out[0][i] = static_cast<std::int16_t>(in[0][i] * 2);
    }
    *ret = static_cast<std::int32_t>(n);
  }
  return hal::code_gate::GateResult::Ok;
}

// What F1 APPLY calls in these tests: records the call and, when `run` is set, starts the job through App::run_ptfx with the stand-in above.
struct Runner {
  PlugRig* rig = nullptr;
  unsigned calls = 0;
  unsigned index = 99;
  float first_value = -1.0f;
  bool run = false;
  const char* result = nullptr;
  module::PtfxHostTableStorage store{};
  module::PtfxHostTable host{store.bytes};
  alignas(4) std::uint8_t scratch[1024] = {};
  module::PtfxQuarantine quarantine;

  static const char* go(void* self, unsigned i, const float* values) noexcept {
    auto* r = static_cast<Runner*>(self);
    ++r->calls;
    r->index = i;
    r->first_value = values[0];
    if (!r->run) {
      return r->result;
    }
    app::PtfxJob::Plugin p;
    p.info = &r->rig->page().info();
    p.entries = {kPInit, kPSet, kPProc};
    p.host = &r->host;
    p.host_table = reinterpret_cast<std::uintptr_t>(r->store.bytes);
    p.scratch = r->scratch;
    p.scratch_cap = sizeof r->scratch;
    p.id = module::ptfx_plugin_id("alpha.ptfx");
    p.values = values;
    p.quarantine = &r->quarantine;
    p.call = &double_call;
    return r->rig->app->run_ptfx(p);
  }
};

}  // namespace

TEST_CASE("sample edit PLUGINS (S4b): F1 APPLY with no runner says the plugins run on the tracker and changes nothing") {
  PlugRig rig;
  rig.open_page();
  rig.tap(Button::F1);
  CHECK(rig.message() == "PLUGINS RUN ON THE TRACKER");
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(std::memcmp(rig.pcm().data, rig.original.data(), kFrames * sizeof(std::int16_t)) == 0);
}

TEST_CASE("sample edit PLUGINS (S4b): F1 APPLY runs the picked plugin with the page's values, as one undo unit, and F8 UNDO restores it") {
  PlugRig rig;
  Runner r;
  r.rig = &rig;
  r.run = true;
  rig.app->attach_ptfx_runner(&Runner::go, &r);
  rig.open_page();
  rig.tap(Button::F1);
  CHECK(r.calls == 1u);
  CHECK(r.index == 0u);  // ALPHA
  CHECK(r.first_value == rig.page().values()[0]);
  for (unsigned i = 0; i < 300u && rig.bank.undo_depth(kSlot) == 0u; ++i) {
    rig.pass();
  }
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  bool doubled = true;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    doubled = doubled && rig.pcm().data[i] == static_cast<std::int16_t>(rig.original[i] * 2);
  }
  CHECK(doubled);
  rig.app->invoke(core::action_id("sedit.undo"));
  rig.passes(4);
  CHECK(std::memcmp(rig.pcm().data, rig.original.data(), kFrames * sizeof(std::int16_t)) == 0);
}

TEST_CASE("sample edit PLUGINS (S4b): F1 APPLY acts on the selection only and leaves the rest of the sample alone") {
  PlugRig rig;
  Runner r;
  r.rig = &rig;
  r.run = true;
  rig.app->attach_ptfx_runner(&Runner::go, &r);
  rig.select(200, 200);
  const unsigned start = rig.st().a;
  const unsigned end = rig.st().b;
  NEED(start < end);
  NEED(end - start < kFrames);
  rig.open_page();
  rig.tap(Button::F1);
  for (unsigned i = 0; i < 300u && rig.bank.undo_depth(kSlot) == 0u; ++i) {
    rig.pass();
  }
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    const bool in_body = i >= start && i < end;
    CHECK(rig.pcm().data[i] == (in_body ? static_cast<std::int16_t>(rig.original[i] * 2) : rig.original[i]));
  }
}

TEST_CASE("sample edit PLUGINS (S4b): F8 while the plugin runs cancels it and keeps the audio") {
  PlugRig rig;
  Runner r;
  r.rig = &rig;
  r.run = true;
  rig.app->attach_ptfx_runner(&Runner::go, &r);
  rig.open_page();
  rig.press(Button::F1);
  rig.release(Button::F1);
  rig.clock.advance(core::kTicksPerMs);  // one short pass: the run has begun, and a job of 40000 frames takes several
  rig.app->run_once();
  NEED(rig.st().running);
  rig.tap(Button::F8);
  rig.passes(20);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(std::memcmp(rig.pcm().data, rig.original.data(), kFrames * sizeof(std::int16_t)) == 0);
  CHECK_FALSE(rig.st().running);
}

TEST_CASE("sample edit PLUGINS (S4b): a file that is for the IMX only is greyed, APPLY says NOT FOR THIS TRACKER, and RESCAN finds a file put on the card later") {
  PlugRig rig;
  rig.card.put("Effects/imx.ptfx", make_plug("IMX", "", module::kPtfxFlagImxOnly, {}));
  rig.open_page();
  NEED(rig.cat.count() == 5u);
  unsigned imx = rig.cat.count();
  for (unsigned i = 0; i < rig.cat.count(); ++i) {
    if (std::strcmp(rig.cat.entry(i).file_name, "imx.ptfx") == 0) {  // refused before the name: the row shows the file name
      imx = i;
    }
  }
  NEED(imx < rig.cat.count());
  rig.down(imx);
  rig.tap(Button::F1);
  CHECK(rig.message() == "NOT FOR THIS TRACKER");
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  rig.card.put("Effects/zeta.ptfx", make_plug("ZETA", "", 0, kAlphaParams));
  rig.tap(Button::F2);
  for (unsigned guard = 0; rig.cat.state() == module::PtfxCatalog::State::Scanning && guard < 600u; ++guard) {
    rig.pass();
  }
  rig.passes(4);
  CHECK(rig.cat.count() == 6u);
}

TEST_CASE("sample edit PLUGINS (S4b): an effect on an empty slot says NO SAMPLE, a generator reaches the runner; a refused runner text is the strip's") {
  PlugRig rig;
  Runner r;
  r.rig = &rig;
  r.result = "BUSY TEST";
  rig.app->attach_ptfx_runner(&Runner::go, &r);
  rig.open_page();
  rig.app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kSlot + 2));
  rig.passes(4);
  NEED(rig.bank.pcm(kSlot + 1u).frames == 0u);
  rig.tap(Button::F1);  // ALPHA, an effect
  CHECK(rig.message() == "NO SAMPLE");
  CHECK(r.calls == 0u);
  rig.down();  // BETA, a generator
  rig.tap(Button::F1);
  CHECK(r.calls == 1u);
  CHECK(r.index == 1u);
  CHECK(rig.message() == "BUSY TEST");
}

TEST_CASE("sample edit PLUGINS (S4b): F2 and F1 pressed in one pass say SCANNING..., not that the file is damaged, and run nothing") {
  PlugRig rig;
  Runner r;
  r.rig = &rig;
  rig.app->attach_ptfx_runner(&Runner::go, &r);
  rig.open_page();
  rig.press(Button::F2);
  rig.release(Button::F2);
  rig.press(Button::F1);
  rig.release(Button::F1);
  rig.pass();  // the scan begins in this pass, before the APPLY is looked at
  NEED(rig.cat.state() == module::PtfxCatalog::State::Scanning);
  CHECK(rig.message() == "SCANNING...");
  CHECK(r.calls == 0u);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
}

TEST_CASE("sample edit PLUGINS (S4b): leaving the screen, or a sample that changes, while the plugin runs commits nothing") {
  {
    PlugRig rig;
    Runner r;
    r.rig = &rig;
    r.run = true;
    rig.app->attach_ptfx_runner(&Runner::go, &r);
    rig.open_page();
    rig.tap(Button::F1);
    NEED(rig.st().running);
    rig.go(ScreenId::Pattern);
    rig.passes(40);
    rig.expire();
    CHECK(rig.bank.undo_depth(kSlot) == 0u);
    CHECK(std::memcmp(rig.pcm().data, rig.original.data(), kFrames * sizeof(std::int16_t)) == 0);
    CHECK_FALSE(rig.st().running);
  }
  {
    PlugRig rig;
    Runner r;
    r.rig = &rig;
    r.run = true;
    rig.app->attach_ptfx_runner(&Runner::go, &r);
    rig.open_page();
    rig.tap(Button::F1);
    NEED(rig.st().running);
    std::uint32_t cap = 0;
    std::int16_t* p = rig.bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
      p[i] = static_cast<std::int16_t>(sample_at(i) + 7);  // another audio takes the slot: its version moves
    }
    NEED(rig.bank.commit_load(kSlot, p, kFrames));
    rig.bank.rebind_now(kSlot);
    rig.passes(40);
    CHECK(rig.bank.undo_depth(kSlot) == 0u);
    CHECK(rig.pcm().data[10] == sample_at(10) + 7);  // the load's audio, not the plugin's result
    CHECK_FALSE(rig.st().running);
  }
}

TEST_CASE("sample edit PLUGINS (A15): the values of the last 8 plugins are kept while the OS runs; the ninth one pushes the oldest out") {
  PlugRig rig(10);  // sorted by name: ALPHA, BETA, F00..F09, FUTURE, TWELVE
  rig.open_page();
  NEED(rig.cat.count() == 14u);
  rig.tap(Button::Right);
  rig.tap(Button::Down);
  rig.invoke("sedit.plug", 2);  // ALPHA: VOICES 5
  CHECK(rig.shown(1) == "5");
  rig.tap(Button::Left);
  rig.down(7);  // eight plugins in all (ALPHA, BETA, F00..F05)
  CHECK(rig.st().plug_sel == 7u);
  rig.up(7);
  CHECK(rig.st().plug_sel == 0u);
  CHECK(rig.shown(1) == "5");  // still there
  rig.down(9);  // nine others since ALPHA was read: its values are the oldest and gone
  rig.up(9);
  CHECK(rig.st().plug_sel == 0u);
  CHECK(rig.shown(1) == "3");  // the default again
}

TEST_CASE("sample edit PLUGINS: F2 RESCAN reads the folder again as a job, the keys that change the page say BUSY while it runs, and the new file is listed afterwards") {
  PlugRig rig(100);
  rig.open_page();
  const unsigned before = rig.cat.count();
  NEED(before == 104u);
  rig.card.put("Effects/aaa.ptfx", make_plug("AAA", "", 0, kAlphaParams));
  rig.tap(Button::F2);
  CHECK(rig.cat.state() == module::PtfxCatalog::State::Scanning);
  rig.invoke("sedit.plug", 1);  // held while the scan runs
  CHECK(rig.message() == "BUSY");
  rig.tap(Button::F3);
  CHECK(rig.message() == "BUSY");
  rig.passes(400);
  CHECK(rig.cat.state() == module::PtfxCatalog::State::Done);
  CHECK(rig.cat.count() == before + 1u);
  CHECK(std::strcmp(rig.cat.entry(0).display_name, "AAA") == 0);
  CHECK(rig.page().ready());  // the row in view was read again
  CHECK(rig.card.open_handles == 0);
}

TEST_CASE("sample edit PLUGINS: with no plugin listing (no catalogue attached) the page shows without a fault and the keys say so") {
  Rig rig;
  rig.tap(Button::SampleEdit);
  rig.tap(Button::SampleEdit);
  NEED(app::sedit_plug_page(rig.app->state()));
  rig.passes(6);
  rig.tap(Button::F2);
  CHECK(rig.message() == "NO PLUGINS HERE");
  rig.invoke("sedit.plug", 1);
  rig.tap(Button::Down);
  rig.tap(Button::Right);
  CHECK(rig.st().plug_focus == 0u);
  rig.tap(Button::SampleEdit);  // the edit page again
  CHECK_FALSE(app::sedit_plug_page(rig.app->state()));
}
