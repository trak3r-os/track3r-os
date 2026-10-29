// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Record screen on the App (OS-011c): the button and its three pages, the rows and what the encoder does to them, F1 arming and stopping a take through the real engine, the
// settle after a source change, the review (trim, discard, save into the first free instrument slot through the sample bank), the radio keys, and what leaving the screen gives back.
// (The recorder's rules are recorder_test.cpp, the render side audio_capture_test.cpp, the pictures the gallery's `record` states.)
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/app.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/pad_board.h>
#include <o3os/app/pad_palette.h>
#include <o3os/app/record_view.h>
#include <o3os/app/recorder.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/record_screen.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/events.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/play_position.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/hal/capture.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include "support/static_slot.h"

#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::RecMode;
using app::ScreenId;
namespace cap = hal::capture;
namespace ha = hal::audio;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];
std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

class FakeHw final : public cap::CaptureHw {
 public:
  bool select_source(cap::Source s, std::uint8_t g) noexcept override {
    ++selects;
    source = s;
    gain = g;
    return true;
  }
  bool set_gain(cap::Source, std::uint8_t g) noexcept override {
    gain = g;
    return true;
  }
  void radio_power(bool) noexcept override {}
  void radio_region(cap::Region r) noexcept override { region = r; }
  void radio_tune(std::uint16_t f) noexcept override { tuned = f; }
  void radio_seek(bool up) noexcept override { seeks += up ? 1 : -1; ++seek_calls; }
  void service(std::uint64_t) noexcept override {}
  cap::RadioStatus radio_status() const noexcept override { return status; }
  unsigned selects = 0;
  cap::Source source = cap::Source::Count;
  std::uint8_t gain = 255;
  cap::Region region = cap::Region::Europe;
  std::uint16_t tuned = 0;
  int seeks = 0;
  unsigned seek_calls = 0;
  cap::RadioStatus status;
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  audio::Engine& engine;
  std::unique_ptr<audio::VoicePool> voices_unused;
  audio::VoicePool* pool = nullptr;  // the pool the bank and the engine share
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory;
  app::SampleBank bank;
  std::unique_ptr<app::MixBoard> mix = std::make_unique<app::MixBoard>();
  std::unique_ptr<app::Recorder> rec = std::make_unique<app::Recorder>();
  FakeHw hw;
  alignas(8) std::int16_t in[ha::kBlockSamples] = {};
  alignas(8) std::int16_t out[ha::kBlockSamples] = {};
  ha::BlockReport report = {};
  std::uint32_t block = 0;
  bool silent = false;                   // OS-SREC-LEVEL: the input is digital silence (a take that listens hears nothing until it is false)
  bool capture = false;                  // OS-LOOPER: keep the left channel of every TX block from `heard_from` on
  std::uint64_t heard_from = 0;
  std::vector<std::int16_t> heard;

  static audio::Engine& make_engine() {
    static o3os::test::StaticSlot<audio::Engine> slot;
    static audio::TapMem taps;
    g_cycles = 0;
    return slot.make(&fake_cycles, taps);
  }
  static audio::VoicePool& make_pool() {
    static o3os::test::StaticSlot<audio::VoicePool> slot;
    return slot.make();
  }

  // `arena_samples`: the sample arena (a loop take holds twice its length and a bitmap, OS-LOOPER L2a: 2 bars of 16,000 frames need 64,032 of it).
  explicit Rig(std::size_t arena_samples = 70000) : engine(make_engine()), memory(arena_samples) {
    model::ModelArena& arena = test::arena(1);
    sim::fill_demo_project(arena.project);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    audio::VoicePool& made = make_pool();
    pool = &made;
    engine.attach_voices(&made);
    bank.init(made, arena, *bank_mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
    app->attach_samples(&bank);
    app->attach_mix(mix.get());
    app->attach_param_ring(&engine.params());
    app::Recorder::Wiring w;
    w.hw = &hw;
    w.graph = &engine.graph();
    w.params = &engine.params();
    w.bank = &bank;
    w.mix = mix.get();
    rec->attach(w);
    app->attach_recorder(rec.get());
  }
  // 16 ms of audio (11 blocks) and the app's frame after it.
  void pass(unsigned ms = 16) {
    for (unsigned b = 0; b < (ms * 44u + 63u) / 64u; ++b) {
      for (std::uint32_t i = 0; i < ha::kBlock; ++i) {
        const int l = silent ? 0 : 1000 + static_cast<int>((block * ha::kBlock + i) % 20000u);
        in[2u * i] = static_cast<std::int16_t>(l);
        in[2u * i + 1u] = static_cast<std::int16_t>(l / 2);
      }
      ++block;
      report = ha::BlockReport{};
      const ha::BlockIo io{in, out, 0, &report};
      engine.render_block(io);
      for (std::uint32_t i = 0; capture && i < ha::kBlock; ++i) {
        heard.push_back(out[2u * i]);
      }
    }
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
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
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  void shift_turn(int detents) {
    press(Button::Shift);
    pass(1);
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
    release(Button::Shift);
    pass(1);
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  void settle() {
    for (int i = 0; i < 20; ++i) {
      pass();
    }
  }
  std::string message() const { return app->message(); }
  const app::ViewState& state() const { return app->state(); }
};

}  // namespace

TEST_CASE("record screen: the button opens it, its context is registered, the button again cycles the three pages, and the rows follow the page") {
  Rig rig;
  rig.tap(Button::SampleRec);
  CHECK(rig.state().screen == ScreenId::Record);
  CHECK(app::kRecordScreen.context() == app::Context::Record);
  CHECK(app::kRecordScreen.pages() == 3);
  CHECK(rig.app->failed_frames() == 0);
  const std::uint8_t* page = &rig.state().page[app::screen_index(ScreenId::Record)];
  CHECK(*page == 0);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 6);  // SOURCE, GAIN, MONITOR, START ON, STOP AT, LOOP
  rig.tap(Button::SampleRec);
  CHECK(*page == 1);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 4);  // TAP, START ON, STOP AT, LOOP
  rig.tap(Button::SampleRec);
  CHECK(*page == 2);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 6);  // FREQUENCY, REGION, GAIN, START ON, STOP AT, LOOP
  rig.tap(Button::SampleRec);
  CHECK(*page == 0);
  // Up and down move the row, clamped.
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.state().record.row[0] == 2);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.state().record.row[0] == 5);
  rig.tap(Button::Up);
  CHECK(rig.state().record.row[0] == 4);
}

TEST_CASE("record screen: entering owns the input path and leaving gives it back; the codec follows the source row, the encoder and Shift + encoder move the rows") {
  Rig rig;
  CHECK(rig.engine.graph().input_feed() == audio::InputFeed::Sum);
  rig.tap(Button::SampleRec);
  rig.settle();
  CHECK(rig.rec->owned());
  CHECK(rig.hw.selects == 1);
  CHECK(rig.hw.source == cap::Source::LineMix);
  CHECK(rig.engine.graph().input_feed() == audio::InputFeed::CodecOnly);
  // SOURCE row: a detent is one source; USB switches the feed, with no analogue part.
  rig.turn(5);
  CHECK(rig.rec->cfg.source == cap::Source::Radio);  // five detents: line L, line R, mic LG, mic HG, radio
  rig.turn(1);
  rig.settle();
  CHECK(rig.rec->cfg.source == cap::Source::Usb);
  CHECK(rig.engine.graph().input_feed() == audio::InputFeed::ReturnOnly);
  rig.turn(-6);
  rig.settle();
  CHECK(rig.rec->cfg.source == cap::Source::LineMix);
  // GAIN: a detent is a percent, Shift five; the hardware has it at the next frame.
  rig.tap(Button::Down);
  rig.turn(3);
  CHECK(rig.rec->gain() == 3);
  rig.shift_turn(2);
  CHECK(rig.rec->gain() == 13);
  CHECK(rig.hw.gain == 13);
  rig.tap(Button::Delete);
  CHECK(rig.rec->gain() == 0);
  // MONITOR: any detent toggles; F2 too.
  rig.tap(Button::Down);
  rig.turn(1);
  CHECK(rig.rec->cfg.monitor);
  rig.tap(Button::F2);
  CHECK_FALSE(rig.rec->cfg.monitor);
  rig.tap(Button::Delete);
  CHECK_FALSE(rig.rec->cfg.monitor);
  // START ON: FREE, STEP, BAR, PATTERN; Delete is BAR.
  rig.tap(Button::Down);
  rig.turn(1);
  CHECK(rig.rec->cfg.align == app::RecAlign::Pattern);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  // Leaving the screen with nothing running gives the engine's input path back.
  rig.tap(Button::Pattern);
  rig.pass();
  rig.pass();
  CHECK_FALSE(rig.rec->owned());
  CHECK(rig.engine.graph().input_feed() == audio::InputFeed::Sum);
}

TEST_CASE("record screen: F1 arms a take after the source has settled, F1 stops it, and F3 saves the window to the first free instrument; F4 throws it away") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.tap(Button::F1);  // too early: the source is still settling
  CHECK(rig.message() == "THE SOURCE IS SETTLING: TRY AGAIN");
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  rig.settle();
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Recording);  // the transport is stopped: the take starts now
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  CHECK(rig.rec->frames() > 10u * ha::kBlock);
  CHECK(rig.app->failed_frames() == 0);
  rig.tap(Button::F1);  // stop
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 8);  // + TRIM START, TRIM END (LOOP is the row before them: OS-LOOPER)
  // The trim rows move the markers.
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  CHECK(rig.state().record.row[0] == 6);
  rig.turn(3);
  CHECK(rig.rec->trim_start() > 0u);
  rig.tap(Button::Delete);
  CHECK(rig.rec->trim_start() == 0u);
  rig.turn(2);
  const std::uint32_t trim_start = rig.rec->trim_start();
  CHECK(trim_start > 0u);
  // SAVE: the demo project has instruments; the take goes to the first slot that has none.
  model::Project& project = test::arena(1).project;
  unsigned want = 0;
  while (project.instrument(want) != nullptr) {
    ++want;
  }
  const std::uint32_t window = rig.rec->trim_end() - rig.rec->trim_start();
  rig.tap(Button::F3);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.state().overlay == app::OverlayId::TextEntry);  // the name keyboard that SAVE opens (OS-AUDIT-71)
  CHECK(project.instrument(want) != nullptr);
  CHECK(project.instrument(want)->kind == model::InstrumentKind::Sample);
  CHECK(rig.bank.pcm(want).frames == window);
  CHECK(project.instrument(want)->sampler.volume == 50);  // OS-BUG-PASTE24: born at 0 dB under the document's law (the 1.2+ law here: level 50)
  CHECK(rig.bank.pending(want));  // the voices get it when playback is stopped and the live notes are idle
  CHECK(rig.state().instrument == static_cast<int>(want) + 1);
  rig.tap(Button::F7);  // keep the name RECORDING
  CHECK(rig.message().find("SAVED AS INSTRUMENT") == 0u);
  // F3 again: nothing to save.
  rig.tap(Button::F3);
  CHECK(rig.message() == "NO TAKE TO SAVE: F1 RECORDS ONE");
  // A second take, thrown away with F4.
  rig.tap(Button::F1);
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Review);  // E-SREC-09: an unsaved take asks first
  CHECK(rig.message() == "TAKE NOT SAVED: F4 AGAIN");
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.message() == "TAKE DISCARDED");
}

// OS-SILENTMSG: F4 with nothing recorded says so (F3 and F7 already say NO TAKE TO SAVE: F1 RECORDS ONE); a take being written is left alone.
TEST_CASE("record screen: F4 with nothing recorded says NOTHING RECORDED") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.message() == "NOTHING RECORDED");
}

// OS-BUG-MEMDISCARD (int.8): the strip's MEM followed a take (78 % idle, 91 % in review) but stayed at 91 % after F4, though the next take reused the room. The arena gives a discarded take back at once.
TEST_CASE("record screen: F4 gives the take's room back at once, the arena's free space and the strip's MEM go back to what they were") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.pass();
  const std::uint32_t free_idle = rig.bank.free_samples();
  const std::int8_t mem_idle = rig.app->model().load.mem;
  rig.tap(Button::F1);
  for (int i = 0; i < 60; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.bank.free_samples() < free_idle - 3000u);  // the take holds its frames
  CHECK(rig.app->model().load.mem > mem_idle);
  rig.tap(Button::F4);
  rig.tap(Button::F4);  // (the question first: E-SREC-09)
  CHECK(rig.message() == "TAKE DISCARDED");
  rig.pass();
  CHECK(rig.bank.free_samples() == free_idle);
  CHECK(rig.app->model().load.mem == mem_idle);
  // A take that is saved keeps its room (F3): the readout stays where the slot's audio is.
  rig.tap(Button::F1);
  for (int i = 0; i < 60; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  const std::uint32_t free_review = rig.bank.free_samples();
  rig.tap(Button::F3);
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.bank.free_samples() <= free_review + 2u);  // (the kept window; no more than the odd frame comes back)
  CHECK(rig.bank.free_samples() < free_idle - 3000u);
}

namespace {
// The transport the Record screen asks for a count-in (OS-METRO): it only records the request.
struct CountSink final : app::TransportSink {
  unsigned counted = 0;
  unsigned plays = 0;
  unsigned bars = 0;
  unsigned pattern = 0;
  bool accept = true;
  bool play(unsigned) noexcept override {
    ++plays;
    return accept;
  }
  bool play_count_in(unsigned p, unsigned b) noexcept override {
    ++counted;
    pattern = p;
    bars = b;
    return accept;
  }
  bool play_song(unsigned) noexcept override { return true; }
  bool stop() noexcept override { return true; }
};
}  // namespace

// OS-METRO: stopped, with an aligned take (ALIGN BAR is the default) and COUNT-IN on, F1 arms the take to wait for the start point and the transport counts in; the aligned modes used to fall back to
// FREE with the transport stopped (there was no step to wait for), and still do with COUNT-IN Off or ALIGN FREE.
TEST_CASE("record screen: COUNT-IN on and an aligned take: F1 from stop arms the take to wait and starts the transport counting in; ALIGN FREE, COUNT-IN Off and a refused transport do not") {
  Rig rig;
  CountSink transport;
  rig.app->attach_transport(&transport);
  rig.tap(Button::SampleRec);
  rig.settle();
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  // A transport that refuses: the take that waits for it is given up at once, and the screen says so.
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 1.0f) == model::Status::Ok);
  transport.accept = false;
  rig.tap(Button::F1);
  CHECK(rig.message() == "THE COUNT-IN DID NOT START");
  CHECK(transport.counted == 1u);
  CHECK_FALSE(rig.state().playing);
  rig.settle();
  CHECK(rig.engine.graph().rec().state() != audio::RecTap::State::Waiting);
  if (rig.rec->state() == app::Recorder::State::Review) {
    rig.tap(Button::F4);  // (nothing was recorded: the empty take is thrown away)
    rig.tap(Button::F4);  // (the question first: E-SREC-09)
  }
  rig.settle();
  transport.accept = true;
  // Two bars: the take waits and the transport is asked to count in.
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 2.0f) == model::Status::Ok);
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);  // waiting for the first step after the count-in
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Waiting);
  CHECK(transport.counted == 2u);
  CHECK(transport.bars == 2u);
  CHECK(transport.pattern == rig.state().edit.pattern);
  CHECK(transport.plays == 0u);
  CHECK(rig.state().playing);
  rig.tap(Button::F1);  // a second F1 gives the waiting take up
  rig.pass();
  CHECK(rig.engine.graph().rec().state() != audio::RecTap::State::Waiting);
  rig.settle();
  if (rig.rec->state() == app::Recorder::State::Review) {
    rig.tap(Button::F4);
    rig.tap(Button::F4);  // (the question first: E-SREC-09)
  }
  // ALIGN FREE: the take starts at once and the transport is not asked.
  rig.rec->cfg.align = app::RecAlign::Free;
  rig.settle();
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Recording);
  CHECK(transport.counted == 2u);
  rig.tap(Button::F1);
  rig.pass();
  rig.tap(Button::F4);
  rig.tap(Button::F4);  // (the question first: E-SREC-09)
  // COUNT-IN Off with ALIGN BAR: the old behaviour, the take starts at once (FREE) with the transport stopped. (The badge is the Play key's last request: the fake sequencer reports nothing, so it
  // still says playing, and the take is then the aligned one; the count-in is what this case is about.)
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 0.0f) == model::Status::Ok);
  rig.rec->cfg.align = app::RecAlign::Bar;
  rig.settle();
  rig.tap(Button::F1);
  CHECK(transport.counted == 2u);
  CHECK(transport.plays == 0u);
}

// OS-METRO (review finding): a Stop during the count-in's pre-roll left the take armed, and a later Play started it with no F1. The recorder gives a take that waits for its step up when the published
// transport falls (and the same for an aligned take armed while the transport ran).
TEST_CASE("record screen: a take that waits for the count-in's end is given up when the transport stops, and so is one armed on a running transport") {
  Rig rig;
  CountSink transport;
  audio::PlayPosition pos;
  rig.app->attach_transport(&transport);
  rig.app->attach_play_position(&pos);
  rig.tap(Button::SampleRec);
  rig.settle();
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 1.0f) == model::Status::Ok);
  const auto run = [&](bool playing, unsigned count_in) {
    pos.publish(playing, 1, 0, 12000, audio::PlayPosition::kNoOrder, 0);
    pos.publish_count_in(count_in);
    rig.pass();
  };
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Waiting);
  CHECK(transport.counted == 1u);
  run(true, 12);  // the sequencer counts in: the take keeps waiting
  rig.settle();
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Waiting);
  run(false, 0);  // Stop during the pre-roll
  rig.settle();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Idle);  // nothing is armed for a later Play to start
  CHECK(rig.rec->frames() == 0u);
  // The same with the transport already running: the take waits for the bar, the transport stops.
  run(true, 0);
  rig.settle();
  rig.tap(Button::F1);
  CHECK(transport.counted == 1u);  // (no count-in on a running transport)
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Waiting);
  run(false, 0);
  rig.settle();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Idle);
}

// OS-METROALIGN (docs/specs/OS-METRO.md section 2): a take armed on a RUNNING transport whose ALIGN makes it wait shows the count-in's countdown in the strip while a beat or more is left; the take itself
// (its start step, the engine) is what it was.
TEST_CASE("record screen: a take that waits for the bar on a running transport counts down like the count-in, from one beat on, and starts where it always did") {
  Rig rig;
  CountSink transport;
  audio::PlayPosition pos;
  rig.app->attach_transport(&transport);
  rig.app->attach_play_position(&pos);
  rig.tap(Button::SampleRec);
  rig.settle();
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  const unsigned len = test::arena(1).project.pattern_length(1);
  CHECK(len >= 32u);
  const auto counting = [&] { return rig.message().rfind("RECORDING IN", 0) == 0; };  // (the strip may say other things: a discarded take)
  const auto run = [&](bool playing, unsigned step, unsigned count_in = 0) {
    pos.publish(playing, 1, step, 12000, audio::PlayPosition::kNoOrder, 0);
    pos.publish_count_in(count_in);
    rig.pass();
  };
  run(true, 3);
  rig.settle();
  CHECK_FALSE(counting());
  rig.tap(Button::F1);  // armed on step 3 of a running transport: 13 steps to the bar
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.rec->wait_align() == audio::RecTap::Align::Bar);
  CHECK(transport.counted == 0u);  // no count-in of its own on a running transport
  run(true, 3);
  CHECK(rig.message() == "RECORDING IN 4..");
  CHECK(rig.state().count_in.beat == 4u);
  run(true, 4);
  CHECK(rig.message() == "RECORDING IN 4.. 3..");  // 12 left: three beats
  run(true, 8);
  CHECK(rig.message() == "RECORDING IN 4.. 3.. 2..");
  run(true, 12);
  CHECK(rig.message() == "RECORDING IN 4.. 3.. 2.. 1..");
  run(true, 14);  // less than a beat left, the countdown that has begun goes on
  CHECK(rig.message() == "RECORDING IN 4.. 3.. 2.. 1..");
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  // The take starts where it always did: not at step 15, at the bar's step (a Step event of the engine), and the countdown is gone with it.
  rig.engine.graph().rec().on_step(15, 3);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Waiting);
  rig.engine.graph().rec().on_step(16, 5);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Armed);
  rig.pass();
  rig.pass();
  run(true, 16);
  CHECK(rig.rec->state() == app::Recorder::State::Recording);
  CHECK(rig.rec->wait_align() == audio::RecTap::Align::Free);
  CHECK(rig.state().count_in.beat == 0u);
  CHECK_FALSE(counting());
  rig.tap(Button::F1);
  rig.pass();
  rig.settle();
  if (rig.rec->state() == app::Recorder::State::Review) {
    rig.tap(Button::F4);
    rig.tap(Button::F4);  // (the question first: E-SREC-09)
  }
  rig.settle();
  // Less than a beat left when the take is armed: no countdown at all.
  run(true, 14);
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  run(true, 14);
  run(true, 15);
  CHECK_FALSE(counting());
  CHECK(rig.state().count_in.beat == 0u);
  // A stop gives the take up (the recorder) and nothing is said.
  run(false, 0);
  rig.settle();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK_FALSE(counting());
  // ALIGN PATTERN waits for step 0: the countdown is the whole pattern.
  rig.rec->cfg.align = app::RecAlign::Pattern;
  rig.settle();
  run(true, len - 10);
  rig.settle();
  rig.tap(Button::F1);
  run(true, len - 10);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.rec->wait_align() == audio::RecTap::Align::Pattern);
  CHECK(rig.state().count_in.beat == 3u);  // 10 steps left: 3 beats (the strip may still say the discarded take: the beat is what is checked)
  run(false, 0);  // stopped meanwhile
  CHECK_FALSE(counting());
  rig.settle();
  // ALIGN FREE: the take starts at once, nothing waits, nothing is counted.
  rig.rec->cfg.align = app::RecAlign::Free;
  rig.settle();
  run(true, 3);
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Recording);
  run(true, 5);
  CHECK_FALSE(counting());
  CHECK(rig.state().count_in.beat == 0u);
}

// OS-METROALIGN: the pass in which a count-in ends still has the recorder's last word (a take that waits for its start step): a countdown of the wait for the NEXT bar must not flash up for it.
TEST_CASE("record screen: the end of a count-in does not start a countdown of the wait for the next bar") {
  Rig rig;
  CountSink transport;
  audio::PlayPosition pos;
  rig.app->attach_transport(&transport);
  rig.app->attach_play_position(&pos);
  rig.tap(Button::SampleRec);
  rig.settle();
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 1.0f) == model::Status::Ok);
  const auto run = [&](unsigned step, unsigned count_in) {
    pos.publish(true, 1, step, 12000, audio::PlayPosition::kNoOrder, 0);
    pos.publish_count_in(count_in);
    rig.pass();
  };
  rig.tap(Button::F1);  // stopped: the take waits and the transport counts in
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  run(48, 16);
  CHECK(rig.state().count_in.beat == 4u);
  run(60, 4);
  CHECK(rig.state().count_in.beat == 1u);
  run(0, 0);  // the start point: the recorder has not seen the take begin yet
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.state().count_in.beat == 0u);
  CHECK(rig.message().rfind("RECORDING IN", 0) != 0u);
}

TEST_CASE("record screen: with no free instrument the take is kept and the screen says so") {
  Rig rig;
  model::Project& project = test::arena(1).project;
  model::Instrument inst;
  model::init_instrument(inst, model::InstrumentKind::Sample);
  for (unsigned i = 0; i < app::SampleBank::kSlots; ++i) {
    (void)project.set_instrument(i, inst);
  }
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F1);
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F3);
  CHECK(rig.message() == "NO FREE INSTRUMENT SLOT");
  CHECK(rig.rec->state() == app::Recorder::State::Review);
}

TEST_CASE("record screen: the radio keys act on the RADIO page only, the encoder tunes and the region is one of PAR-085's four; F7 says what a WAV needs") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.tap(Button::F5);  // not the RADIO page: nothing
  rig.tap(Button::F6);
  CHECK(rig.hw.seek_calls == 0u);
  rig.tap(Button::SampleRec);
  rig.tap(Button::SampleRec);  // RADIO
  rig.settle();
  CHECK(rig.hw.source == cap::Source::Radio);
  CHECK(rig.rec->cfg.radio_freq == 8750);
  rig.turn(10);  // FREQUENCY row: 10 channels of 100 kHz
  CHECK(rig.rec->cfg.radio_freq == 8850);
  CHECK(rig.hw.tuned == 8850);
  rig.shift_turn(-1);  // ten channels down
  CHECK(rig.rec->cfg.radio_freq == 8750);
  rig.tap(Button::F6);
  rig.tap(Button::F5);
  CHECK(rig.hw.seek_calls == 2u);
  CHECK(rig.hw.seeks == 0);
  rig.tap(Button::Down);  // REGION
  rig.turn(1);
  CHECK(rig.rec->cfg.region == cap::Region::Usa);
  CHECK(rig.hw.region == cap::Region::Usa);
  rig.turn(1);
  rig.turn(1);
  CHECK(rig.rec->cfg.region == cap::Region::Japan);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.region == cap::Region::Europe);
  rig.tap(Button::F7);
  CHECK(rig.message() == "NO CARD WRITE LAYER");  // (F7 saves a WAV through the card's save layer, OS-007n: this rig has none)
}

TEST_CASE("record screen: a take goes on when the screen is left, and a source that is frozen under a take does not change") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F1);
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Recording);
  rig.turn(1);  // SOURCE: frozen while a take runs
  CHECK(rig.rec->cfg.source == cap::Source::LineMix);
  rig.tap(Button::Pattern);
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  CHECK(rig.rec->state() == app::Recorder::State::Recording);
  CHECK(rig.rec->owned());
  CHECK(rig.engine.graph().input_feed() == audio::InputFeed::CodecOnly);
  rig.tap(Button::SampleRec);  // back: the Rec button is F1 here too
  rig.tap(Button::Rec);
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::Pattern);
  rig.pass();
  rig.pass();
  CHECK_FALSE(rig.rec->owned());
}

TEST_CASE("record screen: with no recorder attached the screen opens, says so, and its keys do nothing") {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  auto a = std::make_unique<app::App>(input, clock, display);
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::SampleRec), true));
  input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::SampleRec), false));
  clock.advance(16 * core::kTicksPerMs);
  a->run_once();
  CHECK(a->state().screen == ScreenId::Record);
  CHECK(a->failed_frames() == 0);
  CHECK(a->invoke(core::action_id("record.toggle")) == app::DispatchResult::Unchanged);
  CHECK(a->invoke(core::action_id("record.edit"), 1) == app::DispatchResult::Unchanged);
  CHECK(a->invoke(core::action_id("record.save")) == app::DispatchResult::Unchanged);
}

// OS-BUG-PASTE24 (docs/specs/OS-BUG-PASTE24.md section 5, D3): the recorder is one of the sites that create a sample instrument. formats::wav::make_sample_instrument leaves the level at init_instrument's 50, which is
// 0 dB under the 1.2+ law and about -20 dB under the 1.1.14 table; the save takes the unity of the project's law.
TEST_CASE("record screen: F3 saves the take at the level that is 0 dB under the project's law: 50 under the 1.2+ law, 100 under the 1.1.14 table (OS-BUG-PASTE24)") {
  for (const std::uint8_t law : {std::uint8_t{1}, std::uint8_t{0}}) {
    CAPTURE(law);
    Rig rig;
    model::Project& project = test::arena(1).project;
    project.set_volume_law(law);
    rig.tap(Button::SampleRec);
    rig.settle();
    rig.tap(Button::F1);
    for (int i = 0; i < 20; ++i) {
      rig.pass();
    }
    rig.tap(Button::F1);  // stop
    rig.pass();
    NEED(rig.rec->state() == app::Recorder::State::Review);
    unsigned want = 0;
    while (project.instrument(want) != nullptr) {
      ++want;
    }
    // an instrument that is already there keeps its level: the save never touches another slot
    const std::uint8_t first = project.instrument(0) != nullptr ? project.instrument(0)->sampler.volume : std::uint8_t{0};
    rig.tap(Button::F3);
    NEED(project.instrument(want) != nullptr);
    CHECK(project.instrument(want)->sampler.volume == (law != 0u ? 100 : 50));
    CHECK(project.instrument(0)->sampler.volume == first);
    rig.pass(); rig.pass(); rig.pass(); rig.pass();
    CHECK(rig.pool->sample(want).gain == 65536);  // and the voices play it at 0 dB once the bank has bound it
  }
}

// OS-QAFIX-E-SREC-09: a take under review is held in the arena only. F4 DISCARD, and F1 / Rec (a new take over it, the key that also stops a take) ask first; the question is the key again within 4 s; a take written
// as a WAV is saved, and so needs none until its window moves.
TEST_CASE("record screen: F4 and F1 over an unsaved take ask first, the same key again does it, a take saved as a WAV needs no question") {
  Rig rig;
  o3os::test::MemCard card;
  app::SaveService saves;
  saves.attach(card, card);
  rig.app->attach_saves(&saves);
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F1);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);  // stop
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  const std::uint32_t frames = rig.rec->frames();
  // a double press of the stop key must not wipe the take
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.rec->frames() == frames);
  CHECK(rig.message() == "TAKE NOT SAVED: F1 AGAIN");
  CHECK(rig.state().take_ask.kind == app::TakeAsk::Kind::Record);
  // another key's question takes its place: F1 asks again
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.message() == "TAKE NOT SAVED: F4 AGAIN");
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.message() == "TAKE NOT SAVED: F1 AGAIN");
  // a question that is not answered in time is asked again
  rig.clock.advance(5000u * core::kTicksPerMs);
  rig.pass();
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  // the same key again within the time: the new take is armed over the old one
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Recording);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);  // stop
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  // written as a WAV: the take is saved, F4 discards it at once
  CHECK_FALSE(rig.rec->take_saved());
  rig.tap(Button::F7);
  CHECK_FALSE(rig.rec->take_saved());  // (E-SREC-09: a job that has started has saved nothing yet)
  for (int i = 0; i < 400 && rig.app->session().jobs.occupied() != 0u; ++i) {
    rig.pass();
  }
  rig.pass();  // (the service takes the job's end in at the next pass)
  CHECK(rig.rec->take_saved());
  rig.tap(Button::F1);  // a new take over a saved one: no question
  CHECK(rig.rec->state() == app::Recorder::State::Recording);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  CHECK_FALSE(rig.rec->take_saved());  // (a new take)
  rig.tap(Button::F7);
  CHECK_FALSE(rig.rec->take_saved());  // (E-SREC-09: a job that has started has saved nothing yet)
  for (int i = 0; i < 400 && rig.app->session().jobs.occupied() != 0u; ++i) {
    rig.pass();
  }
  rig.pass();  // (the service takes the job's end in at the next pass)
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.message() == "TAKE DISCARDED");
  // a saved take whose markers moved is not saved any more
  rig.tap(Button::F1);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F7);
  CHECK_FALSE(rig.rec->take_saved());  // (E-SREC-09: a job that has started has saved nothing yet)
  for (int i = 0; i < 400 && rig.app->session().jobs.occupied() != 0u; ++i) {
    rig.pass();
  }
  rig.pass();  // (the service takes the job's end in at the next pass)
  CHECK(rig.rec->take_saved());
  rig.rec->set_trim_start(100);
  CHECK_FALSE(rig.rec->take_saved());
}

// OS-QAFIX-E-SREC-09, review fix (a): the question of an earlier take is no answer for the next one. F4 asks, F3 SAVE ends that review, a new take is made: F4 inside the 4 s of the old question must ask again.
TEST_CASE("record screen: a question left over from an earlier take does not let F4 discard a new unsaved take") {
  Rig rig;
  model::Project& project = test::arena(1).project;
  (void)project;
  rig.tap(Button::SampleRec);
  rig.settle();
  const auto take = [&rig]() {
    rig.tap(Button::F1);
    for (int i = 0; i < 20; ++i) {
      rig.pass();
    }
    rig.tap(Button::F1);  // stop
    rig.pass();
  };
  take();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F4);  // asks
  CHECK(rig.message() == "TAKE NOT SAVED: F4 AGAIN");
  CHECK(rig.state().take_ask.kind == app::TakeAsk::Kind::Discard);
  rig.tap(Button::F3);  // SAVE: the review ends, the question is stale
  rig.tap(Button::F7);  // (the name keyboard, OS-AUDIT-71)
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Idle);
  take();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F4);  // 4 s have not passed since the old question: it is not an answer
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.message() == "TAKE NOT SAVED: F4 AGAIN");
  rig.tap(Button::F4);  // the new question, answered
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
}

// OS-QAFIX-E-SREC-09, review fix (b): F7 SAVE WAV marks the take saved when the job ends Ok, not when it starts. A write that fails leaves the take unsaved, and F4 still asks.
TEST_CASE("record screen: a WAV that failed to write leaves the take unsaved, F4 still asks") {
  Rig rig;
  o3os::test::MemCard card;
  app::SaveService saves;
  saves.attach(card, card);
  rig.app->attach_saves(&saves);
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F1);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);  // stop
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  card.cut_at = card.ops;  // the card fails from its next write on
  rig.tap(Button::F7);
  for (int i = 0; i < 400 && rig.app->session().jobs.occupied() != 0u; ++i) {
    rig.pass();
  }
  rig.pass();
  rig.pass();
  CHECK_FALSE(rig.rec->take_saved());
  CHECK(rig.message().find("FAILED") != std::string::npos);  // (the strip says the write failed)
  rig.clock.advance(5000u * core::kTicksPerMs);
  rig.pass();
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.message() == "TAKE NOT SAVED: F4 AGAIN");
}

// OS-RECCROP (finding 25): Shift + F3 is CROP in the review. The audio outside TRIM START and TRIM END is lost, so it asks first; the take is then the window (the markers are the whole of it again), the audio of
// the window is untouched, and the arena gives the room behind it back.
TEST_CASE("record screen: Shift + F3 crops the take to its window, after a question; nothing to crop, no take and a loop say so") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.shift_tap(Button::F3);
  CHECK(rig.message() == "NO TAKE TO CROP");
  rig.settle();
  rig.tap(Button::F1);
  for (int i = 0; i < 30; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);  // stop
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  rig.shift_tap(Button::F3);
  CHECK(rig.message() == "NOTHING TO CROP: MOVE TRIM START OR END");
  for (int i = 0; i < 6; ++i) {
    rig.tap(Button::Down);  // ... TRIM START
  }
  rig.turn(3);
  rig.tap(Button::Down);  // TRIM END
  rig.turn(-2);
  const std::uint32_t frames = rig.rec->frames();
  const std::uint32_t start = rig.rec->trim_start();
  const std::uint32_t end = rig.rec->trim_end();
  NEED(start > 0u);
  NEED(end < frames);
  const std::int16_t* data = rig.rec->take_data();
  const std::vector<std::int16_t> window(data + start, data + end);
  const std::uint32_t free_before = rig.bank.free_samples();
  rig.shift_tap(Button::F3);
  CHECK(rig.message() == "CROP KEEPS THE WINDOW ONLY: SHIFT+F3 AGAIN");
  CHECK(rig.rec->frames() == frames);  // nothing happened yet
  rig.shift_tap(Button::F3);
  CHECK(rig.message() == "TAKE CROPPED");
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  const std::uint32_t at = start & ~1u;
  CHECK(rig.rec->frames() == end - at);
  CHECK(rig.rec->trim_start() == start - at);
  CHECK(rig.rec->trim_end() == rig.rec->frames());
  CHECK(std::memcmp(rig.rec->take_data() + rig.rec->trim_start(), window.data(), window.size() * sizeof(std::int16_t)) == 0);
  CHECK(rig.bank.free_samples() > free_before);  // the room behind the window is free again
  // the window is the whole take now: nothing to crop, and SAVE makes the instrument of it
  rig.shift_tap(Button::F3);
  CHECK(rig.message() == "NOTHING TO CROP: MOVE TRIM START OR END");
  model::Project& project = test::arena(1).project;
  unsigned want = 0;
  while (project.instrument(want) != nullptr) {
    ++want;
  }
  rig.tap(Button::F3);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.bank.pcm(want).frames == window.size());
}

TEST_CASE("record screen: F7 saves the take's window as a 16-bit mono WAV in Samples/, the next free name, the take stays; a write protected card is refused") {
  Rig rig;
  o3os::test::MemCard card;
  app::SaveService saves;
  saves.attach(card, card);
  rig.app->attach_saves(&saves);
  rig.tap(Button::SampleRec);
  rig.tap(Button::F7);
  CHECK(rig.message() == "NO TAKE TO SAVE: F1 RECORDS ONE");
  rig.settle();
  rig.tap(Button::F1);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);  // stop
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  for (int i = 0; i < 6; ++i) {
    rig.tap(Button::Down);  // ... LOOP, TRIM START
  }
  rig.turn(3);  // the trim start moves: the WAV is the window, not the whole take
  const std::uint32_t frames = rig.rec->trim_end() - rig.rec->trim_start();
  NEED(rig.rec->trim_start() > 0u);
  const std::int16_t* window = rig.rec->take_data() + rig.rec->trim_start();
  const std::vector<std::int16_t> expected(window, window + frames);
  const auto expire = [&rig] {  // the notifications on screen run out: the next message is the next one said
    for (int i = 0; i < 10 && !rig.message().empty(); ++i) {
      rig.clock.advance(5000000);
      rig.pass();
    }
  };
  expire();
  rig.tap(Button::F7);
  for (int i = 0; i < 400 && rig.app->session().jobs.occupied() != 0u; ++i) {
    rig.pass();
  }
  CHECK(rig.message() == "SAVE REC_001 DONE");
  const o3os::test::MemCard::Bytes* wav = card.get("Samples/REC_001.wav");
  NEED(wav != nullptr);
  NEED(wav->size() == 44u + 2u * frames);
  CHECK(std::memcmp(wav->data(), "RIFF", 4) == 0);
  CHECK((*wav)[22] == 1);  // mono
  CHECK(((*wav)[24] | ((*wav)[25] << 8) | ((*wav)[26] << 16)) == 44100);
  CHECK((*wav)[34] == 16);
  CHECK(std::memcmp(wav->data() + 44, expected.data(), 2u * frames) == 0);
  CHECK(rig.rec->state() == app::Recorder::State::Review);  // the take is still there: F3 can still make an instrument of it
  rig.tap(Button::F7);
  for (int i = 0; i < 400 && rig.app->session().jobs.occupied() != 0u; ++i) {
    rig.pass();
  }
  CHECK(card.same("Samples/REC_002.wav", *wav));
  CHECK(card.same("Samples/REC_001.wav", *wav));
  // A card that may not be written: refused, with the reason.
  card.gate_status = o3os::hal::storage::WriteStatus::WriteProtected;
  expire();
  const std::size_t files = card.files.size();
  rig.tap(Button::F7);
  CHECK(rig.message() == "CARD IS WRITE PROTECTED");
  CHECK(card.files.size() == files);
  CHECK(card.violations == 0);
}

TEST_CASE("record screen: F8 PREVIEW plays the take's window on the audition's descriptor while it is down, once the take has stopped") {
  struct FakePreview final : app::PreviewSink {
    std::vector<bool> auditions;
    void preview(unsigned, unsigned) noexcept override {}
    void audition(bool on, unsigned) noexcept override { auditions.push_back(on); }
  } preview;
  Rig rig;
  rig.app->attach_preview(&preview);
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F8);  // no take yet
  CHECK(rig.message() == "NO TAKE TO PREVIEW");
  CHECK(preview.auditions.empty());
  rig.tap(Button::F1);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);  // stop
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  rig.press(Button::F8);
  rig.pass();
  rig.pass();
  NEED(preview.auditions.size() == 1u);
  CHECK(preview.auditions[0]);
  const audio::SampleDesc& d = rig.pool->sample(audio::kPreviewSlot);
  CHECK(d.length == rig.rec->trim_end() - rig.rec->trim_start());
  CHECK(d.data == rig.rec->take_data() + rig.rec->trim_start());
  rig.tap(Button::F1);  // a new take cannot be armed under the held key
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  rig.release(Button::F8);
  rig.pass();
  NEED(preview.auditions.size() == 2u);
  CHECK_FALSE(preview.auditions[1]);
}

// ---- OS-REC2: the pads play the take under review ----------------------------------------------------------------------------------------------------------------------

namespace {

// What the pads asked of the engine: the take's pad notes and the releases.
struct PadSink final : app::PreviewSink {
  struct Down {
    unsigned pad, track, note;
  };
  std::vector<Down> downs;
  std::vector<unsigned> ups;
  bool idle = true;
  void preview(unsigned, unsigned) noexcept override {}
  void take_pad_down(unsigned pad, unsigned track, unsigned note) noexcept override { downs.push_back({pad, track, note}); }
  void pad_up(unsigned pad) noexcept override { ups.push_back(pad); }
  bool live_idle() const noexcept override { return idle; }
};

// The same asks, put on the real engine's live queue as the device's sink does (instrument kPreviewSlot + 1, source pad).
struct LiveSink final : app::PreviewSink {
  audio::LiveBox* box = nullptr;
  audio::Engine* engine = nullptr;
  void preview(unsigned, unsigned) noexcept override {}
  void take_pad_down(unsigned pad, unsigned track, unsigned note) noexcept override {
    CHECK(box->queue.push(audio::LiveEvent{audio::LiveKind::On, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), static_cast<std::uint8_t>(note),
                                           static_cast<std::uint8_t>(audio::kPreviewSlot + 1u), audio::kLiveVelocityDefault, static_cast<std::uint8_t>(track), 0}));
  }
  void pad_up(unsigned pad) noexcept override {
    (void)box->queue.push(audio::LiveEvent{audio::LiveKind::Off, audio::kLiveSourcePad, static_cast<std::uint8_t>(pad), 0, 0, 0, 0, 0});
  }
  bool live_idle() const noexcept override { return engine->live_idle(); }
};

void pad_down(Rig& rig, unsigned pad) { rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(pad), 127)); }
void pad_up(Rig& rig, unsigned pad) { rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(pad), 0)); }

// A take of about 20 passes, under review.
void record_take(Rig& rig) {
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F1);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
}

}  // namespace

TEST_CASE("record screen: the pad layout is the NOTES layout at its defaults, the take at its own pitch on C-4 (pad 12)") {
  const app::PadBoard board;  // scale 0, offset 12, root 36
  for (unsigned pad = 0; pad < app::kRecPads; ++pad) {
    CHECK(app::rec_pad_note(pad) == board.note(pad));
  }
  CHECK(app::rec_pad_note(app::kRecRootPad) == audio::kRootNote);
  CHECK(app::rec_pad_note(36) == 36u);  // the bottom left pad: C-2
  CHECK(app::rec_pad_note(11) == 83u);  // the top right pad: B-5
}

TEST_CASE("record screen: under review the pads play the take through the preview descriptor; a pad waits for the quiet window; a new take is refused while one sounds") {
  PadSink sink;
  Rig rig;
  rig.app->attach_preview(&sink);
  rig.tap(Button::SampleRec);
  rig.settle();
  app::PadRole roles[48];
  CHECK_FALSE(rig.app->pad_lights(roles));  // no take: no opinion
  pad_down(rig, 12);
  rig.pass();
  pad_up(rig, 12);
  rig.pass();
  CHECK(rig.message() == "NO TAKE TO PLAY: F1 RECORDS ONE");
  CHECK(sink.downs.empty());
  record_take(rig);
  CHECK(rig.app->pad_lights(roles));
  for (unsigned n = 0; n < 48; ++n) {
    CHECK(roles[n] == (n == app::kRecRootPad ? app::PadRole::Front : app::PadRole::Back));
  }

  // The first press: the descriptor is not the take's yet and a note still holds the engine (not idle): the pad waits.
  sink.idle = false;
  pad_down(rig, 12);
  rig.pass();
  CHECK(sink.downs.empty());
  CHECK(rig.state().take_pads.pending == (std::uint64_t{1} << 12));
  sink.idle = true;
  rig.pass();
  NEED(sink.downs.size() == 1u);
  CHECK(sink.downs[0].pad == 12u);
  CHECK(sink.downs[0].note == 60u);
  CHECK(sink.downs[0].track == static_cast<unsigned>(rig.state().pattern.track()));
  const std::uint32_t frames = rig.rec->trim_end() - rig.rec->trim_start();
  const audio::SampleDesc& d = rig.pool->sample(audio::kPreviewSlot);
  CHECK(d.length == frames);
  CHECK(d.data == rig.rec->take_data() + rig.rec->trim_start());
  CHECK(rig.state().take_pads.pending == 0u);

  // A second pad while the first is held: the descriptor already is the take's window, so it sounds in the pass that pressed it.
  pad_down(rig, 0);
  rig.pass();
  NEED(sink.downs.size() == 2u);
  CHECK(sink.downs[1].pad == 0u);
  CHECK(sink.downs[1].note == 72u);

  // A new take is refused while a pad is down (its memory is the take's).
  rig.tap(Button::F1);
  CHECK(rig.message() == "THE TAKE IS STILL PLAYING");
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  pad_up(rig, 12);
  pad_up(rig, 0);
  rig.pass();
  CHECK(sink.ups.size() >= 2u);
  CHECK_FALSE(rig.state().take_pads.any());

  // A trim that moved is another window: the next pad rebinds it, in the quiet window.
  rig.rec->set_trim_end(rig.rec->trim_end() - 500);
  pad_down(rig, 5);
  rig.pass();
  NEED(sink.downs.size() == 3u);
  CHECK(sink.downs[2].note == app::rec_pad_note(5));
  CHECK(rig.pool->sample(audio::kPreviewSlot).length == rig.rec->trim_end() - rig.rec->trim_start());

  // Leaving the screen with a pad held ends its note.
  const std::size_t ups = sink.ups.size();
  rig.tap(Button::Pattern);
  rig.pass();
  CHECK(sink.ups.size() > ups);
  CHECK(sink.ups.back() == 5u);
  CHECK_FALSE(rig.state().take_pads.any());
  pad_up(rig, 5);
  rig.pass();
}

// OS-BUG-MEMDISCARD: the room of a thrown-away take is given back only when no note of the take pads can still read it.
TEST_CASE("record screen: F4 gives the take's room back once the take pads' notes are over, not while the engine still holds one") {
  PadSink sink;
  Rig rig;
  rig.app->attach_preview(&sink);
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.pass();
  const std::uint32_t free_idle = rig.bank.free_samples();
  record_take(rig);
  CHECK(rig.bank.free_samples() < free_idle);
  sink.idle = false;  // a note holds the engine (the take pads read the take through the preview descriptor)
  rig.tap(Button::F4);
  rig.tap(Button::F4);  // (the question first: E-SREC-09)
  CHECK(rig.message() == "TAKE DISCARDED");
  for (int i = 0; i < 3; ++i) {
    rig.pass();
  }
  CHECK(rig.bank.free_samples() < free_idle);  // the room waits
  sink.idle = true;
  rig.pass();
  CHECK(rig.bank.free_samples() == free_idle);
}

TEST_CASE("record screen: a pad's note of the take sounds from the preview slot at the pad's pitch, and a new take waits until no voice reads it") {
  static audio::LiveBox box;
  box.notes = audio::LiveNotes{};
  LiveSink sink;
  Rig rig;
  sink.box = &box;
  sink.engine = &rig.engine;
  rig.engine.attach_live(&box);
  rig.app->attach_preview(&sink);
  record_take(rig);
  const unsigned live_slot = audio::kPreviewSlot + 1u;  // the live word names slot + 1

  const auto position_after = [&](unsigned pad) {
    pad_down(rig, pad);
    rig.pass();
    rig.pass();  // the note starts at the next block; 11 blocks of it are rendered by here
    const std::uint32_t w = rig.pool->live_position();
    CHECK((w & audio::VoicePool::kLivePlaying) != 0u);
    CHECK(((w >> 24) & 0x7Fu) == live_slot);
    pad_up(rig, pad);
    rig.pass();
    rig.pass();
    return static_cast<double>(w & 0xFFFFFFu);
  };
  const double root = position_after(app::kRecRootPad);  // C-4: the take at its own pitch
  const double octave = position_after(0);                // C-5
  const double two_below = position_after(36u);  // pad 36: C-2, two octaves below C-4
  CHECK(root > 500.0);
  CHECK(octave / root == doctest::Approx(2.0).epsilon(0.03));
  CHECK(two_below / root == doctest::Approx(0.25).epsilon(0.05));

  // Held: a new take is refused, the voice reads the take's memory.
  pad_down(rig, 12);
  rig.pass();
  rig.pass();
  CHECK(rig.pool->slot_in_use(audio::kPreviewSlot));
  rig.tap(Button::F1);
  CHECK(rig.message() == "THE TAKE IS STILL PLAYING");
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  pad_up(rig, 12);
  rig.pass();
  rig.pass();
  rig.pass();
  CHECK_FALSE(rig.pool->slot_in_use(audio::kPreviewSlot));
  rig.tap(Button::F1);  // the note has ended: the unsaved take asks first (E-SREC-09)
  rig.tap(Button::F1);  // the take is armed
  CHECK((rig.rec->state() == app::Recorder::State::Recording || rig.rec->state() == app::Recorder::State::Armed));
  rig.tap(Button::F1);
}

TEST_CASE("record screen: the STOP AT row (OS-REC2) is the row before LOOP on every page, the encoder steps it, Shift + encoder four choices, Delete is MANUAL, and it is frozen under a take") {
  using app::RecStop;
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.settle();
  for (int i = 0; i < 4; ++i) {
    rig.tap(Button::Down);  // SOURCE, GAIN, MONITOR, START ON, STOP AT
  }
  CHECK(app::rec_rows(RecMode::Sources, false).row[4] == app::RecRow::StopAt);
  CHECK(app::rec_rows(RecMode::Resample, false).row[2] == app::RecRow::StopAt);
  CHECK(app::rec_rows(RecMode::Radio, false).row[4] == app::RecRow::StopAt);
  CHECK(rig.rec->cfg.stop == RecStop::Manual);
  rig.turn(2);
  CHECK(rig.rec->cfg.stop == RecStop::Bar);
  rig.shift_turn(1);  // four choices: BAR, PATTERN, 1 BAR, 2 BARS, 3 BARS
  CHECK(rig.rec->cfg.stop == RecStop::Bars);
  CHECK(rig.rec->cfg.stop_bars == 3);
  rig.turn(-1);
  CHECK(rig.rec->cfg.stop_bars == 2);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.stop == RecStop::Manual);
  rig.turn(3);
  CHECK(rig.rec->cfg.stop == RecStop::Pattern);
  // Frozen while a take runs (as START ON is).
  rig.tap(Button::F1);
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Recording);
  rig.turn(1);
  CHECK(rig.rec->cfg.stop == RecStop::Pattern);
  rig.tap(Button::F1);  // the transport is stopped: STOP AT acts as MANUAL, the take ends at once
  CHECK_FALSE(rig.rec->stop_pending());
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(app::rec_rows(RecMode::Sources, true).n == 8u);
}

// OS-SHIFTROW: Shift + F5 / F6 CH-10 / CH+10 tune ten channels at a time on the RADIO page (the chip's own seek is the plain F5 / F6); Left / Right turn the page, wrapping, each page keeping its row.
TEST_CASE("record screen: Shift + F5 / F6 tune ten channels down / up on the RADIO page only, without the chip's seek") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.shift_tap(Button::F6);  // SOURCES: nothing
  rig.shift_tap(Button::F5);
  CHECK(rig.hw.tuned == 0);
  rig.tap(Button::SampleRec);
  rig.tap(Button::SampleRec);  // RADIO
  rig.settle();
  CHECK(rig.rec->cfg.radio_freq == 8750);
  rig.shift_tap(Button::F6);
  CHECK(rig.rec->cfg.radio_freq == 8850);  // ten channels of 100 kHz
  CHECK(rig.hw.tuned == 8850);
  rig.shift_tap(Button::F6);
  CHECK(rig.rec->cfg.radio_freq == 8950);
  rig.shift_tap(Button::F5);
  rig.shift_tap(Button::F5);
  CHECK(rig.rec->cfg.radio_freq == 8750);
  CHECK(rig.hw.seek_calls == 0u);
  for (int i = 0; i < 40; ++i) {  // the band's end clamps, like the encoder
    rig.shift_tap(Button::F6);
  }
  const unsigned top = rig.rec->cfg.radio_freq;
  rig.shift_tap(Button::F6);
  CHECK(rig.rec->cfg.radio_freq == top);
  CHECK(rig.hw.seek_calls == 0u);
  rig.tap(Button::F6);  // the plain key is still the chip's seek
  CHECK(rig.hw.seek_calls == 1u);
}

// OS-CFG-ARROWS (new Community OS behaviour, replaces OS-SHIFTROW rule 5 on Record): Left / Right are the encoder's detent on the row, Shift + Left / Right its coarse step; the page only turns with the
// screen button (SOURCES, RESAMPLE, RADIO, wrapping, each page keeping its row; kept when the screen is left).
TEST_CASE("record screen: Left / Right change the row under the cursor (Shift: coarse) and keep the page and the row; the screen button turns the pages") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.settle();
  const auto page = [&] { return static_cast<unsigned>(rig.state().page[app::screen_index(ScreenId::Record)]); };
  CHECK(page() == 0);
  CHECK(rig.rec->cfg.source == cap::Source::LineMix);
  rig.tap(Button::Right);  // SOURCE row: one source a press
  CHECK(rig.rec->cfg.source != cap::Source::LineMix);
  rig.tap(Button::Left);
  CHECK(rig.rec->cfg.source == cap::Source::LineMix);
  CHECK(page() == 0);
  CHECK(rig.state().record.row[0] == 0);
  rig.tap(Button::Down);  // GAIN: a percent a press, five with Shift
  CHECK(rig.rec->gain() == 0);
  rig.tap(Button::Right);
  CHECK(rig.rec->gain() == 1);
  rig.shift_tap(Button::Right);
  CHECK(rig.rec->gain() == 6);
  rig.shift_tap(Button::Left);
  CHECK(rig.rec->gain() == 1);
  rig.tap(Button::Left);
  CHECK(rig.rec->gain() == 0);
  CHECK(page() == 0);
  CHECK(rig.state().record.row[0] == 1);
  // The pages are the screen button alone.
  rig.tap(Button::SampleRec);
  CHECK(page() == 1);
  rig.tap(Button::SampleRec);
  CHECK(page() == 2);
  rig.tap(Button::SampleRec);
  CHECK(page() == 0);  // wraps to SOURCES
  CHECK(rig.state().record.row[0] == 1);  // which kept its row
  rig.tap(Button::SampleRec);
  CHECK(page() == 1);
  rig.tap(Button::Pattern);
  rig.tap(Button::SampleRec);
  CHECK(page() == 1);  // kept per screen
}

// ---- OS-LOOPER slice L1: the Record screen's LOOP row, the loop take, the loop under review, SAVE and DISCARD, through the real App, Recorder, Engine and Graph ------------------------------------------
namespace {

// A transport that plays at 1,000 samples a step (a bar is 16,000): Step events for the engine's event source, a Stopped event on request. The sequencer's own timing is audio_loop_test.cpp's.
struct StepScript {
  bool running = false;
  std::uint64_t next = 0;
  std::uint32_t index = 0;
  std::uint64_t spacing = 1000;  // samples a step
  std::uint32_t bump_at = 0xFFFFFFFFu;  // the step whose successor comes one sample late (an odd loop length)
  bool stop_now = false;
  std::vector<std::uint64_t> bars;  // absolute sample of every Step event whose index is a multiple of 16
  audio::SeqEvent buffer[audio::kMaxEventsPerBlock];
  static std::uint32_t step(void* self, std::uint64_t start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept {
    StepScript& s = *static_cast<StepScript*>(self);
    std::uint32_t c = 0;
    if (s.stop_now && s.running && c < cap) {
      audio::SeqEvent e{};
      e.offset = 5;
      e.kind = audio::EventKind::Stopped;
      e.track = audio::kNoTrack;
      out[c++] = e;
      s.running = false;
    }
    while (s.running && s.next < start + n && c < cap) {
      audio::SeqEvent e{};
      e.offset = static_cast<std::uint16_t>(s.next - start);
      e.kind = audio::EventKind::Step;
      e.track = audio::kNoTrack;
      e.value = static_cast<std::int32_t>(s.index % 64u);
      if (s.index % 16u == 0u) {
        s.bars.push_back(s.next);
      }
      out[c++] = e;
      s.next += s.spacing + (s.index == s.bump_at ? 1u : 0u);
      ++s.index;
    }
    return c;
  }
};

struct LoopRig : Rig {
  // 1,000 samples a step at the engine's rate: 661.76 BPM (a loop's bars at the published tempo are what the arena must hold: 16,000 frames a bar)
  static constexpr std::uint32_t kTempo = 66176;
  CountSink transport;
  audio::PlayPosition pos;
  StepScript script;
  explicit LoopRig(std::size_t arena_samples = 70000) : Rig(arena_samples) {
    app->attach_transport(&transport);
    app->attach_play_position(&pos);
    CHECK(test::arena(1).project.set_tempo_x100(kTempo) == model::Status::Ok);  // (the tempo a stopped transport will play at)
    audio::EventSource src;
    src.self = &script;
    src.step = &StepScript::step;
    src.buffer = script.buffer;
    src.capacity = audio::kMaxEventsPerBlock;
    engine.attach_events(src);
    tap(Button::SampleRec);
    settle();
  }
  void play(bool on) {
    if (on) {
      script.running = true;
      script.stop_now = false;
      script.next = engine.sample_clock() + 3000;
      script.index = 3;  // the first bar is 13 steps away
    } else {
      script.stop_now = true;
    }
    pos.publish(on, 1, 0, kTempo, audio::PlayPosition::kNoOrder, 0);
    pass();
  }
  void set_loop(int bars) {
    while (state().record.row[0] != 5) {
      tap(state().record.row[0] < 5 ? Button::Down : Button::Up);
    }
    turn(bars - static_cast<int>(rec->cfg.loop_bars));
  }
  // Passes until `done` holds (bounded).
  template <typename F>
  bool until(F done, int limit = 400) {
    for (int i = 0; i < limit && !done(); ++i) {
      pass();
    }
    return done();
  }
};

}  // namespace

TEST_CASE("loop screen: the LOOP row is the last row of every page, OFF to 16 BARS, and ALIGN and STOP AT are the loop's while it is on") {
  LoopRig rig;
  CHECK(app::rec_rows(RecMode::Sources, false).row[5] == app::RecRow::Loop);
  CHECK(app::rec_rows(RecMode::Resample, false).row[3] == app::RecRow::Loop);
  CHECK(app::rec_rows(RecMode::Radio, false).row[5] == app::RecRow::Loop);
  CHECK(app::rec_rows(RecMode::Sources, true, true).n == 8u);  // a loop under review: LOOP VOL and FEEDBACK in place of the two trim rows
  CHECK(app::rec_rows(RecMode::Sources, true, true).row[6] == app::RecRow::LoopVol);
  CHECK(app::rec_rows(RecMode::Sources, true, true).row[7] == app::RecRow::Feedback);
  CHECK(app::rec_rows(RecMode::Radio, true, true).n <= app::kRecMaxRows);
  CHECK(rig.rec->cfg.loop_bars == 0u);
  rig.set_loop(1);
  CHECK(rig.rec->cfg.loop_bars == 1u);
  rig.turn(100);
  CHECK(rig.rec->cfg.loop_bars == 16u);
  rig.shift_turn(-1);  // coarse: four choices
  CHECK(rig.rec->cfg.loop_bars == 12u);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.loop_bars == 0u);
  rig.turn(2);
  CHECK(rig.rec->cfg.loop_bars == 2u);
  // ALIGN and STOP AT do not change while LOOP is on, and the screen says why.
  rig.tap(Button::Up);  // STOP AT
  rig.turn(2);
  CHECK(rig.rec->cfg.stop == app::RecStop::Manual);
  CHECK(rig.message() == "LOOP ENDS THE TAKE AFTER ITS BARS");
  rig.tap(Button::Up);  // START ON
  rig.turn(1);
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  CHECK(rig.message() == "LOOP STARTS THE TAKE ON A BAR");
}

TEST_CASE("loop screen: F1 records the loop on the bars, it ends by itself and plays in time; LOOP VOL, the mute, SAVE as a LOOP FWD instrument made of the loop, and DISCARD") {
  LoopRig rig;
  rig.set_loop(2);
  rig.play(true);
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);  // waiting for the bar
  CHECK(rig.rec->wait_align() == audio::RecTap::Align::Bar);
  CHECK(rig.rec->loop_take());
  CHECK(rig.rec->ends_by_itself());
  CHECK(rig.transport.plays == 0u);  // the transport was running
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }));
  CHECK(rig.rec->loop_review());
  CHECK(rig.rec->frames() == 32000u);  // two bars of 16 steps of 1,000 samples, to the sample
  CHECK(rig.rec->loop_bars() == 2u);
  CHECK(rig.engine.graph().loop().reading());
  CHECK(rig.engine.graph().loop().frames() == 32000u);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 8);  // + LOOP VOL, FEEDBACK
  // F1 under the review is the overdub (OS-LOOPER L2c), never another take over the loop's memory: on, then off.
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.until([&] { return rig.rec->overdubbing(); }, 20));
  rig.tap(Button::F1);
  CHECK(rig.until([&] { return !rig.rec->overdubbing(); }, 20));
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.rec->loop_review());
  // The loop is on the line out, and F8 mutes it without moving its place.
  rig.capture = true;
  rig.heard_from = rig.engine.sample_clock();
  for (int i = 0; i < 12; ++i) {
    rig.pass();
  }
  unsigned loud = 0;
  for (const std::int16_t v : rig.heard) {
    loud += v != 0 ? 1u : 0u;
  }
  CHECK(loud > 3000u);
  rig.tap(Button::F8);
  CHECK(rig.engine.graph().loop().muted());
  CHECK(rig.rec->loop_muted());
  rig.pass();
  rig.pass();
  const std::size_t muted_from = rig.heard.size();
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  unsigned loud_muted = 0;
  for (std::size_t i = muted_from; i < rig.heard.size(); ++i) {
    loud_muted += rig.heard[i] != 0 ? 1u : 0u;
  }
  CHECK(loud_muted == 0u);
  rig.tap(Button::F8);
  CHECK_FALSE(rig.engine.graph().loop().muted());
  // LOOP VOL is the last row under review: the encoder moves it, Delete is 100, and the render hears it.
  while (rig.state().record.row[0] != 6) {
    rig.tap(Button::Down);
  }
  rig.turn(-30);
  CHECK(rig.rec->cfg.loop_vol == 70u);
  CHECK(rig.engine.graph().loop().volume() == 70u);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.loop_vol == 100u);
  CHECK(rig.engine.graph().loop().volume() == 100u);
  // SAVE: the first free slot gets exactly the two bars as an instrument that loops forward; what is saved is what the loop played.
  model::Project& project = test::arena(1).project;
  unsigned want = 0;
  while (project.instrument(want) != nullptr) {
    ++want;
  }
  rig.heard.clear();
  rig.heard_from = rig.engine.sample_clock();
  for (int i = 0; i < 100; ++i) {  // more than two passes of the loop (32,000 frames each; a pass is 704 frames)
    rig.pass();
  }
  rig.tap(Button::F3);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  rig.tap(Button::F7);  // the name keyboard (OS-AUDIT-71) hides the strip; F7 keeps the name
  CHECK(rig.message().find("SAVED AS INSTRUMENT") == 0u);
  const model::Instrument* inst = project.instrument(want);
  NEED(inst != nullptr);
  CHECK(inst->kind == model::InstrumentKind::Sample);
  CHECK(inst->sampler.play_mode == 1);  // LOOP FWD
  CHECK(inst->sampler.loop_start == 0);
  CHECK(inst->sampler.loop_end == 65535);
  CHECK(std::string(inst->name) == "LOOP 2B");
  CHECK(inst->sampler.volume == 50);  // OS-BUG-PASTE24: born at 0 dB under the 1.2+ law
  const app::SampleBank::Pcm saved = rig.bank.pcm(want);
  CHECK(saved.frames == 32000u);
  NEED(saved.data != nullptr);
  // The samples equal the loop: a whole pass of it in the capture (found by its first, second, middle and last frames) is the instrument's audio, frame for frame.
  std::uint64_t wrong = 0;
  unsigned found = 0;
  for (std::size_t i = 0; i + 32000u <= rig.heard.size() && found == 0u; ++i) {
    if (rig.heard[i] == saved.data[0] && rig.heard[i + 1u] == saved.data[1] && rig.heard[i + 16000u] == saved.data[16000] && rig.heard[i + 31999u] == saved.data[31999]) {
      ++found;
      for (std::uint32_t j = 0; j < 32000u; ++j) {
        wrong += rig.heard[i + j] != saved.data[j] ? 1u : 0u;
      }
    }
  }
  CHECK(found == 1u);
  CHECK(wrong == 0u);
  // The looper is free: the loop lets go within a block or two, and the next take can be armed.
  rig.pass();
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
  CHECK_FALSE(rig.rec->loop_review());
}

TEST_CASE("loop screen: F4 throws the loop away and its memory is the next take's; F1 before the last bar, or a Stop, gives the loop take up") {
  LoopRig rig;
  rig.set_loop(2);
  rig.play(true);
  const std::uint32_t free_idle = rig.bank.free_samples();
  rig.tap(Button::F1);
  const std::uint32_t room = rig.rec->capacity();
  CHECK(room > 32000u);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }));
  CHECK(rig.rec->loop_review());
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Review);  // E-SREC-09: an unsaved take asks first
  CHECK(rig.message() == "TAKE NOT SAVED: F4 AGAIN");
  rig.tap(Button::F4);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.message() == "TAKE DISCARDED");
  rig.pass();
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);  // the render let go of the take
  CHECK_FALSE(rig.bank.take_held());
  rig.pass();
  CHECK(rig.bank.free_samples() == free_idle);  // OS-BUG-MEMDISCARD: the room (the loop, its undo buffer and bitmap) is back in the free-space figure, once the render has let go
  // The whole tail is free again for the next take (the discarded take is reused, nothing was allocated after it).
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.rec->capacity() == room);
  // F1 while it waits for the bar, and again mid-take: the loop take is given up, no loop, nothing kept.
  rig.tap(Button::F1);
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK_FALSE(rig.engine.graph().loop().reading());
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Recording; }));
  rig.pass();
  rig.pass();
  rig.tap(Button::F1);  // before the second bar
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.rec->note() == app::Recorder::Note::LoopCut);
  CHECK_FALSE(rig.engine.graph().loop().reading());
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
  CHECK_FALSE(rig.bank.take_held());
  // The transport stops under a loop take: the take is given up the same way.
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Recording; }));
  rig.play(false);
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK_FALSE(rig.engine.graph().loop().reading());
}

TEST_CASE("loop screen: stopped, F1 starts the transport and the loop take together, after the count-in when COUNT-IN is on") {
  LoopRig rig;
  rig.set_loop(1);
  rig.tap(Button::F1);
  CHECK(rig.transport.plays == 1u);  // COUNT-IN off: the transport starts with the take
  CHECK(rig.transport.counted == 0u);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.rec->wait_align() == audio::RecTap::Align::Bar);
  rig.tap(Button::F1);
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 1.0f) == model::Status::Ok);
  rig.settle();
  rig.tap(Button::F1);
  CHECK(rig.transport.counted == 1u);
  CHECK(rig.transport.bars == 1u);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  rig.tap(Button::F1);
  rig.pass();
}

TEST_CASE("loop screen: leaving the Record screen leaves the loop playing (a loop pedal): the review and the loop's phase survive, the screen comes back to them") {
  LoopRig rig;
  rig.set_loop(2);
  rig.play(true);
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }));
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Ready);
  rig.capture = true;
  for (const Button other : {Button::Pattern, Button::Song}) {  // any other screen
    rig.tap(other);
    CHECK(rig.state().screen != ScreenId::Record);
    for (int i = 0; i < 30; ++i) {
      rig.pass();
    }
    CHECK(rig.rec->state() == app::Recorder::State::Review);
    CHECK(rig.rec->loop_review());
    CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Ready);
    CHECK_FALSE(rig.engine.graph().loop().muted());
  }
  unsigned loud = 0;
  for (const std::int16_t v : rig.heard) {
    loud += v != 0 ? 1u : 0u;
  }
  CHECK(loud > 20000u);  // it kept sounding the whole time
  // F8, SAVE and DISCARD are the Record screen's: elsewhere they are not the loop's.
  rig.tap(Button::F8);
  CHECK_FALSE(rig.engine.graph().loop().muted());
  // The Stop of the transport fades it out wherever the user is; the Play brings it back on the bar.
  rig.play(false);
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  rig.heard.clear();
  for (int i = 0; i < 10; ++i) {
    rig.pass();
  }
  unsigned still = 0;
  for (const std::int16_t v : rig.heard) {
    still += v != 0 ? 1u : 0u;
  }
  CHECK(still == 0u);
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Ready);
  rig.tap(Button::SampleRec);
  CHECK(rig.state().screen == ScreenId::Record);
  CHECK(rig.rec->loop_review());
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 8);
}

TEST_CASE("loop screen: SAVE with no free instrument slot keeps the review and the loop playing") {
  LoopRig rig;
  model::Project& project = test::arena(1).project;
  model::Instrument inst;
  model::init_instrument(inst, model::InstrumentKind::Sample);
  for (unsigned i = 0; i < app::SampleBank::kSlots; ++i) {
    (void)project.set_instrument(i, inst);
  }
  rig.set_loop(1);
  rig.play(true);
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }));
  rig.tap(Button::F3);
  CHECK(rig.message() == "NO FREE INSTRUMENT SLOT");
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.rec->loop_review());
  rig.pass();
  rig.pass();
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Ready);  // still reading its take
}

TEST_CASE("loop screen (L2a): a loop take holds its undo buffer and bitmap behind it; need == cap is a loop and cap + 1 is refused; SAVE trims the tail and DISCARD gives it back") {
  const std::uint64_t need = static_cast<std::uint64_t>(ha::sample_rate_mhz()) * 24u * 2u / LoopRig::kTempo;  // two bars at the published tempo
  const auto footprint = [](std::uint32_t c) { return 2u * ((c + 1u) & ~1u) + app::SampleBank::undo_bitmap_samples(c); };
  const std::uint32_t exact = footprint(static_cast<std::uint32_t>(need));  // the smallest arena whose loop capacity reaches `need`
  NEED(app::SampleBank::loop_capacity(exact) >= need);
  // An arena one sample smaller cannot hold the loop: refused at F1, nothing held.
  {
    LoopRig rig(exact - 1u);
    rig.set_loop(2);
    rig.play(true);
    rig.tap(Button::F1);
    CHECK(rig.rec->state() == app::Recorder::State::Idle);
    CHECK(rig.message() == "NO ROOM IN THE SAMPLE MEMORY FOR THE LOOP");
    CHECK_FALSE(rig.bank.take_held());
    CHECK(rig.bank.free_samples() == ((exact - 1u) & ~1u));  // (the bank keeps whole words)
  }
  // The arena of exactly the footprint takes it: the render is given `cap`, not the whole room.
  LoopRig rig(exact);
  rig.set_loop(2);
  rig.play(true);
  rig.tap(Button::F1);
  NEED(rig.rec->state() == app::Recorder::State::Armed);
  const std::uint32_t cap = rig.rec->capacity();
  CHECK(cap == app::SampleBank::loop_capacity(exact));
  CHECK(cap >= need);
  CHECK(2u * cap + app::SampleBank::undo_bitmap_samples(cap) <= exact + 2u);
  rig.tap(Button::F1);  // give it up (a cancel at the next block); the same arena, a loop that fits it to the frame:
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  rig.script.spacing = (cap - 32u) / 32u;  // 32 steps end within `cap` (a take of exactly `cap` frames can be Done by the capacity in the block before the end step: RecTap, recorded as a follow-up)
  rig.script.stop_now = false;
  rig.tap(Button::F1);
  NEED(rig.rec->state() == app::Recorder::State::Armed);
  rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }, 2000);
  INFO("state " << static_cast<int>(rig.rec->state()) << " note " << static_cast<int>(rig.rec->note()) << " cap " << cap << " frames " << rig.rec->frames());
  NEED(rig.rec->state() == app::Recorder::State::Review);
  const std::uint32_t frames = rig.rec->frames();
  CHECK(frames == 32u * ((cap - 32u) / 32u));
  CHECK(rig.rec->loop_review());
  // Under review the bank holds the take, the undo buffer and the bitmap.
  CHECK(rig.bank.take().frames == frames);
  CHECK(rig.bank.free_samples() == exact - ((frames + 1u) & ~1u) - ((frames + app::SampleBank::undo_bitmap_samples(frames) + 1u) & ~1u));
  // DISCARD gives all of it back (nothing was allocated after the tail): the next take is armed over both, with the same capacity.
  rig.tap(Button::F4);
  rig.tap(Button::F4);  // (the question first: E-SREC-09)
  rig.pass();
  rig.tap(Button::F1);
  NEED(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.rec->capacity() == cap);
  // The same again, then SAVE: the tail is trimmed, the slot keeps the loop.
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }, 2000));
  CHECK(rig.rec->frames() == frames);
  rig.tap(Button::F3);
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.bank.free_samples() == exact - ((frames + 1u) & ~1u));
}

TEST_CASE("loop screen (L2a): a loop of an odd number of frames goes through finish() with an odd tail; SAVE trims it to the loop's even end") {
  const auto footprint = [](std::uint32_t c) { return 2u * ((c + 1u) & ~1u) + app::SampleBank::undo_bitmap_samples(c); };
  const std::uint32_t frames = 32001u;  // 32 steps of 1,000 samples and one sample more
  const std::uint32_t arena = footprint(frames);
  LoopRig rig(arena);
  rig.set_loop(2);
  rig.play(true);
  rig.script.bump_at = rig.script.index + 16u;  // a step inside the take (the bar is at most 15 steps away, the take is 32 steps long)
  rig.tap(Button::F1);
  NEED(rig.rec->state() == app::Recorder::State::Armed);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }, 2000));
  CHECK(rig.rec->frames() == frames);
  CHECK(rig.rec->capacity() >= frames);
  CHECK(rig.rec->loop_review());
  const std::uint32_t bitmap = app::SampleBank::undo_bitmap_samples(frames);
  const std::uint32_t tail = (frames + bitmap + 1u) & ~1u;  // an odd sum, taken up to the even sample
  CHECK(rig.bank.free_samples() == ((arena & ~1u) - 32002u - tail));
  rig.tap(Button::F3);
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.bank.free_samples() == ((arena & ~1u) - 32002u));
}

TEST_CASE("loop screen: a loop that does not fit the free arena at the tempo is refused at F1; one that outgrows it (the tempo slows under the take) is given up with the memory-full note") {
  LoopRig rig;
  rig.set_loop(16);  // 16 bars of 16,000 frames: 256,000 against the rig's 60,000
  rig.play(true);
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.message() == "NO ROOM IN THE SAMPLE MEMORY FOR THE LOOP");
  CHECK_FALSE(rig.bank.take_held());
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
  rig.set_loop(2);  // 32,000 fit
  rig.script.spacing = 4000;  // ... but the transport is four times slower than the published tempo: 128,000 do not
  rig.tap(Button::F1);
  NEED(rig.rec->state() == app::Recorder::State::Armed);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Idle; }, 2000));
  CHECK(rig.rec->note() == app::Recorder::Note::Full);
  CHECK_FALSE(rig.rec->loop_review());
  CHECK_FALSE(rig.bank.take_held());
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
}

TEST_CASE("loop screen: a project load or NEW under a loop (the arena emptied) lets the loop go and says the take is lost; the same under a loop take in progress") {
  LoopRig rig;
  rig.set_loop(2);
  rig.play(true);
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }));
  rig.bank.clear();  // what a load does (the hook asks the render to let go before anything is overwritten)
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.rec->note() == app::Recorder::Note::Lost);
  CHECK_FALSE(rig.rec->loop_review());
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
  // A loop take that is recording when the arena goes: the take is gone (take_held false), the loop player is released, nothing plays.
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Recording; }));
  rig.bank.clear();
  rig.pass();
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.rec->note() == app::Recorder::Note::Lost);
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
  CHECK_FALSE(rig.rec->loop_take());
  // ... and the recorder is usable again.
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
}

TEST_CASE("loop screen: Delete on START ON or STOP AT with LOOP on says why, as the encoder does, and changes nothing") {
  LoopRig rig;
  rig.set_loop(2);
  rig.tap(Button::Up);  // STOP AT
  rig.tap(Button::Delete);
  CHECK(rig.message() == "LOOP ENDS THE TAKE AFTER ITS BARS");
  rig.tap(Button::Up);  // START ON
  rig.tap(Button::Delete);
  CHECK(rig.message() == "LOOP STARTS THE TAKE ON A BAR");
  CHECK(rig.rec->cfg.loop_bars == 2u);
}

namespace {
// A feed of one sample of 3,000 frames: what a project load writes into the arena after the clear.
struct OneSampleFeed final : app::PcmFeed {
  std::uint32_t frames(unsigned slot) const noexcept override { return slot == 0u ? 3000u : 0u; }
  std::uint32_t rate(unsigned) const noexcept override { return 44100; }
  bool read(unsigned, std::uint32_t, std::uint32_t n, std::int16_t* dst) noexcept override {
    for (std::uint32_t i = 0; i < n; ++i) {
      dst[i] = 0x1234;
    }
    return true;
  }
};
}  // namespace

TEST_CASE("loop screen: a loader writes nothing over the arena until the render has let go of the loop (the clear's release is seen first)") {
  LoopRig rig;
  rig.set_loop(1);
  rig.play(true);
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }));
  NEED(rig.rec->take_data() != nullptr);
  const std::int16_t* take = rig.rec->take_data();
  CHECK(take == rig.memory.data());  // the take lies at the start of the arena: exactly where a loader writes first
  const std::vector<std::int16_t> before(take, take + 3000);
  rig.pass();  // (the recorder has installed the bank's hook by now)
  OneSampleFeed feed;
  auto loader = std::make_unique<app::OgSampleLoader>();
  NEED(loader->begin_feed(feed, test::arena(1).project, *rig.pool, rig.memory.data(), static_cast<std::uint32_t>(rig.memory.size()), audio::LevelLaw::Og12, &rig.bank));
  CHECK(rig.bank.clear_busy());  // the clear asked the render to release; it has not run since
  for (int i = 0; i < 20; ++i) {
    (void)loader->step(16384);
  }
  CHECK(loader->state() == app::OgSampleLoader::State::Running);
  CHECK(std::vector<std::int16_t>(take, take + 3000) == before);  // not a sample written
  rig.pass();                                                   // a block: the render sees the release and lets go
  CHECK_FALSE(rig.bank.clear_busy());
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
  for (int i = 0; i < 2000 && loader->state() == app::OgSampleLoader::State::Running; ++i) {
    (void)loader->step(16384);
  }
  CHECK(loader->state() == app::OgSampleLoader::State::Done);
  CHECK(std::vector<std::int16_t>(take, take + 3000) != before);  // now the load has written
}

TEST_CASE("loop screen: a clear stops a take that is recording (it writes no more over a load), and NEW that keeps the audio lets a loop go and gives the take up") {
  LoopRig rig;
  rig.tap(Button::F1);  // a plain take, FREE: the transport is stopped
  NEED(rig.rec->state() == app::Recorder::State::Recording);
  rig.pass();
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Armed);
  rig.bank.clear();
  CHECK(rig.bank.clear_busy());  // the tap is still writing until the next block
  rig.pass();
  CHECK_FALSE(rig.bank.clear_busy());
  CHECK(rig.engine.graph().rec().state() != audio::RecTap::State::Armed);
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  // NEW with KEEP INSTRUMENTS: the arena stays but the take is forgotten: the loop under review goes and the recorder says it is lost.
  rig.set_loop(1);
  rig.play(true);
  rig.tap(Button::F1);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Review; }));
  rig.bank.new_project(true);
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.rec->note() == app::Recorder::Note::Lost);
  CHECK(rig.engine.graph().loop().phase() == audio::LoopPlayer::Phase::Idle);
  CHECK_FALSE(rig.bank.clear_busy());
}

TEST_CASE("loop screen: a recorder that is gone takes its hook off the bank") {
  Rig rig;
  rig.pass();
  CHECK(rig.bank.clear_hook_is(rig.rec.get()));
  rig.rec.reset();
  CHECK_FALSE(rig.bank.clear_busy());
  rig.bank.clear();  // no recorder to call: nothing dangles
}

// ---- OS-LOOPER L2c: the overdub session from the keys (spec 3c.2, 3c.3, 3c.5) ------------------------------------------------------------------------------------------------------
namespace {

// A loop of two bars in review, the transport playing.
struct ReviewRig : LoopRig {
  ReviewRig() {
    set_loop(2);
    play(true);
    tap(Button::F1);
    NEED(until([&] { return rec->state() == app::Recorder::State::Review; }));
    CHECK(rec->loop_review());
  }
  bool overdub_on() {
    tap(Button::F1);
    return until([&] { return rec->overdubbing(); }, 30);
  }
  // A key press the app handles with no audio rendered after it (the render has not run: a command it was given is still pending).
  void tap_now(Button b) {
    press(b);
    release(b);
    app->run_once();
  }
  audio::LoopPlayer& loop() { return engine.graph().loop(); }
};

}  // namespace

TEST_CASE("loop screen: F1 under the review is the overdub, on and off; the layer is undone by F5 and the restore refuses F1, F3 and a second F5") {
  ReviewRig rig;
  CHECK_FALSE(rig.rec->layer());
  rig.tap(Button::F5);  // no layer yet
  CHECK(rig.message() == "NO LAYER TO UNDO");
  const std::vector<std::int16_t> orig(rig.rec->take_data(), rig.rec->take_data() + rig.rec->frames());
  NEED(rig.overdub_on());
  for (int i = 0; i < 45; ++i) {  // 45 x 704 frames: the layer covers most of the 32,000 frames, so that its restore lasts about ten app frames
    rig.pass();
  }
  CHECK(rig.rec->layer());
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F1);  // punch out
  NEED(rig.until([&] { return !rig.rec->overdubbing(); }, 30));
  CHECK(rig.rec->layer());  // the layer stays after the session
  CHECK_FALSE(std::equal(orig.begin(), orig.end(), rig.rec->take_data()));  // the input of the REC bus went into the loop, through the real engine
  // F5: the restore. F1, F3 are refused while it runs, F5 says there is no layer (one level), F4 and F8 are accepted.
  rig.tap(Button::F5);
  rig.pass();
  NEED(rig.rec->restoring());
  rig.tap(Button::F1);
  CHECK(rig.message() == "UNDO IN PROGRESS");
  CHECK_FALSE(rig.rec->overdubbing());
  rig.tap(Button::F3);
  CHECK(rig.message() == "UNDO IN PROGRESS");
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F5);
  CHECK(rig.message() == "NO LAYER TO UNDO");
  rig.tap(Button::F8);
  CHECK(rig.engine.graph().loop().muted());
  rig.tap(Button::F8);
  CHECK_FALSE(rig.engine.graph().loop().muted());
  NEED(rig.until([&] { return !rig.rec->restoring(); }, 400));
  CHECK_FALSE(rig.rec->layer());
  CHECK(std::equal(orig.begin(), orig.end(), rig.rec->take_data()));  // and UNDO puts every frame back
  rig.tap(Button::F5);  // after the restore: nothing to undo
  CHECK(rig.message() == "NO LAYER TO UNDO");
  // F4 during a second restore is accepted and abandons it.
  NEED(rig.overdub_on());
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  rig.tap(Button::F5);  // UNDO while the session is on: punch out, then restore
  rig.pass();
  CHECK_FALSE(rig.rec->overdubbing());
  NEED(rig.rec->restoring());
  rig.tap(Button::F4);
  rig.tap(Button::F4);  // (the question first: E-SREC-09)
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  NEED(rig.until([&] { return rig.loop().phase() == audio::LoopPlayer::Phase::Idle; }, 30));
}

TEST_CASE("loop screen: SAVE with a session on is two steps: the keys are ignored while it waits, the review stays, and the loop is saved with the session ended and nothing written after") {
  ReviewRig rig;
  NEED(rig.overdub_on());
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  const unsigned slot_free = [] {
    unsigned n = 0;
    while (test::arena(1).project.instrument(n) != nullptr) {
      ++n;
    }
    return n;
  }();
  rig.tap_now(Button::F3);
  CHECK(rig.rec->save_waiting());
  CHECK(rig.rec->state() == app::Recorder::State::Review);   // the review is still drawn
  rig.tap_now(Button::F3);                                   // a second F3, F1, F4 and F5 during the wait: ignored, no message
  rig.tap_now(Button::F1);
  rig.tap_now(Button::F4);
  rig.tap_now(Button::F5);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.rec->loop_review());
  CHECK(rig.rec->save_waiting());
  CHECK(rig.rec->state() != app::Recorder::State::Idle);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Idle; }, 30));
  CHECK(rig.state().overlay == app::OverlayId::TextEntry);  // the second step of the save opens the name keyboard too (OS-AUDIT-71)
  rig.tap(Button::F7);
  CHECK(rig.message().find("SAVED AS INSTRUMENT") == 0u);
  CHECK(test::arena(1).project.instrument(slot_free) != nullptr);
  CHECK_FALSE(rig.rec->save_waiting());
  CHECK_FALSE(rig.rec->overdubbing());
  NEED(rig.until([&] { return rig.loop().phase() == audio::LoopPlayer::Phase::Idle; }, 30));
  // The slot's audio is the loop as it was when the session ended; nothing may change it now (the render has let go).
  const app::SampleBank::Pcm saved = rig.bank.pcm(slot_free);
  NEED(saved.data != nullptr);
  std::vector<std::int16_t> snap(saved.data, saved.data + saved.frames);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  CHECK(std::equal(snap.begin(), snap.end(), saved.data));
}

// L2c review minor: the SaveWait is bounded. A render that never acknowledges (here: no audio is rendered) ends the wait after kSaveWaitMs of UI time with a warning strip; nothing was saved, the review
// stays, and once the render runs again the session is over and F3 saves as usual.
TEST_CASE("loop screen: a SAVE that the render does not acknowledge ends after the wait's bound with a warning, saves nothing, and works again once the render runs") {
  ReviewRig rig;
  NEED(rig.overdub_on());
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  unsigned slot = 0;
  while (test::arena(1).project.instrument(slot) != nullptr) {
    ++slot;
  }
  rig.tap_now(Button::F3);
  NEED(rig.rec->save_waiting());
  unsigned ms = 0;
  while (rig.rec->save_waiting() && ms < 2000u) {  // frames of the app with no audio rendered between them
    rig.clock.advance(16u * core::kTicksPerMs);
    rig.app->run_once();
    ms += 16u;
  }
  CHECK_FALSE(rig.rec->save_waiting());
  CHECK(ms >= app::Recorder::kSaveWaitMs);
  CHECK(ms <= app::Recorder::kSaveWaitMs + 32u);
  CHECK(rig.message() == "SAVE TIMED OUT: TRY AGAIN");
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.rec->loop_review());
  CHECK(test::arena(1).project.instrument(slot) == nullptr);  // nothing half saved
  // The render runs again: the quiesce posted for the SAVE is taken, the session is over, and F3 saves.
  NEED(rig.until([&] { return !rig.rec->overdubbing(); }, 30));
  rig.tap(Button::F3);
  NEED(rig.until([&] { return rig.rec->state() == app::Recorder::State::Idle; }, 30));
  CHECK(test::arena(1).project.instrument(slot) != nullptr);
}

TEST_CASE("loop screen: the input stays owned while a session is on, on any screen, and goes back to the board when it ends") {
  ReviewRig rig;
  NEED(rig.overdub_on());
  CHECK(rig.rec->owned());
  rig.tap(Button::Pattern);
  CHECK(rig.state().screen != ScreenId::Record);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  CHECK(rig.rec->overdubbing());
  CHECK(rig.rec->owned());  // the pedal keeps its input
  rig.tap(Button::SampleRec);
  CHECK(rig.state().screen == ScreenId::Record);
  rig.tap(Button::F1);      // off
  NEED(rig.until([&] { return !rig.rec->overdubbing(); }, 30));
  rig.tap(Button::Pattern);
  for (int i = 0; i < 20; ++i) {
    rig.pass();
  }
  CHECK_FALSE(rig.rec->owned());  // a loop under review without a session does not own it (L1)
}

TEST_CASE("loop screen: a project load or NEW mid-session lets the loop go with TAKE LOST and the session writes nothing after; the FEEDBACK row steps and resets") {
  ReviewRig rig;
  // FEEDBACK is the last row under review.
  while (rig.state().record.row[0] != 7) {
    rig.tap(Button::Down);
  }
  CHECK(rig.rec->cfg.loop_fb == 100u);
  rig.turn(-30);
  CHECK(rig.rec->cfg.loop_fb == 70u);
  CHECK(rig.engine.graph().loop().feedback() == 70u);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.loop_fb == 100u);
  NEED(rig.overdub_on());
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  rig.bank.clear();
  rig.pass();
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.rec->note() == app::Recorder::Note::Lost);
  CHECK_FALSE(rig.rec->loop_review());
  CHECK_FALSE(rig.rec->overdubbing());
  NEED(rig.until([&] { return rig.loop().phase() == audio::LoopPlayer::Phase::Idle; }, 30));
}

TEST_CASE("loop screen: stopped, F1 under the review starts the transport (the overdub begins with the loop's first bar), and the loop's source rows are locked") {
  ReviewRig rig;
  rig.play(false);
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  const unsigned plays = rig.transport.plays;
  rig.tap(Button::F1);
  CHECK(rig.transport.plays + rig.transport.counted == plays + 1u);
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  // the source row does not change under a loop
  rig.tap(Button::F1);
  while (rig.state().record.row[0] != 0) {
    rig.tap(Button::Up);
  }
  const auto source = rig.rec->cfg.source;
  rig.turn(1);
  CHECK(rig.rec->cfg.source == source);
}

// OS-LOOPER Q8 / L3: the same gesture for the overdub. Stopped, F1 under a loop's review with COUNT-IN on counts in (1 or 2 bars of the pattern being edited) and the session begins with the loop's first bar,
// to the sample: the session is armed (published) at once, nothing is written while the transport is stopped, and the first write is the first frame of the resumed loop.
TEST_CASE("loop screen (L3): stopped, F1 under the review counts in when COUNT-IN is on, and the overdub begins with the loop's first bar; a refused transport punches out again") {
  ReviewRig rig;
  rig.play(false);
  for (int i = 0; i < 6; ++i) {
    rig.pass();
  }
  const std::vector<std::int16_t> orig(rig.rec->take_data(), rig.rec->take_data() + rig.rec->frames());
  for (const unsigned bars : {1u, 2u}) {  // COUNT-IN 1 and 2 bars
    CHECK(rig.app->device_settings().set(model::device_setting::CountIn, static_cast<float>(bars)) == model::Status::Ok);
    rig.settle();
    const unsigned counted = rig.transport.counted;
    const unsigned plays = rig.transport.plays;
    rig.tap(Button::F1);
    CHECK(rig.transport.counted == counted + 1u);
    CHECK(rig.transport.bars == bars);
    CHECK(rig.transport.plays == plays);  // the count-in starts the transport, not Play
    NEED(rig.until([&] { return rig.rec->overdubbing(); }, 30));
    CHECK(rig.rec->state() == app::Recorder::State::Review);
    // The transport is not running yet (the count-in): the loop is parked at its start and nothing is written.
    for (int i = 0; i < 10; ++i) {
      rig.pass();
    }
    CHECK_FALSE(rig.rec->layer());
    CHECK(std::equal(orig.begin(), orig.end(), rig.rec->take_data()));
    // The count-in ends: the transport runs, the loop restarts on the first bar, and the write begins there.
    rig.play(true);
    NEED(rig.until([&] { return rig.rec->layer(); }, 400));
    CHECK(rig.rec->overdubbing());
    rig.tap(Button::F1);  // punch out
    NEED(rig.until([&] { return !rig.rec->overdubbing(); }, 60));
    // the layer begins at the loop's start: nothing before the first bar was touched, the first frames are.
    CHECK_FALSE(std::equal(orig.begin(), orig.end(), rig.rec->take_data()));
    rig.tap(Button::F5);  // UNDO puts the original back
    NEED(rig.until([&] { return !rig.rec->restoring() && !rig.rec->layer(); }, 600));
    CHECK(std::equal(orig.begin(), orig.end(), rig.rec->take_data()));
    rig.play(false);
    for (int i = 0; i < 6; ++i) {
      rig.pass();
    }
  }
  // A transport that refuses: the session is punched out again and the strip says why.
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 1.0f) == model::Status::Ok);
  rig.settle();
  rig.transport.accept = false;
  rig.tap(Button::F1);
  CHECK(rig.message() == "THE COUNT-IN DID NOT START");
  NEED(rig.until([&] { return !rig.rec->overdubbing(); }, 60));
  CHECK_FALSE(rig.rec->layer());
  CHECK(std::equal(orig.begin(), orig.end(), rig.rec->take_data()));
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 0.0f) == model::Status::Ok);  // and with COUNT-IN off, the Play key's way
  rig.settle();
  rig.tap(Button::F1);
  CHECK(rig.message() == "THE TRANSPORT DID NOT START");
  NEED(rig.until([&] { return !rig.rec->overdubbing(); }, 60));
  rig.transport.accept = true;
}

// OS-FILES-C C2a: the console's OG export (App::start_og_export) does not start while a take is armed or recording, and starts once the take is gone; one job at a time.
TEST_CASE("record screen: the OG export is refused while a take is in progress and starts after it, writing the project's folder on the card (OS-FILES-C C2a)") {
  Rig rig;
  test::MemCard card;
  app::SaveService saves;
  saves.attach(card, card);
  rig.app->attach_saves(&saves);
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.tap(Button::F1);
  NEED(rig.rec->state() != app::Recorder::State::Idle);
  CHECK(std::string(rig.app->start_og_export()) == "A RECORDING OR A TAKE IS IN PROGRESS");
  CHECK(card.log.empty());
  CHECK(saves.og_state() == app::SaveService::OgState::Idle);
  rig.tap(Button::F1);  // stop
  rig.settle();
  if (rig.rec->state() == app::Recorder::State::Review) {
    rig.tap(Button::F4);  // (the take is thrown away)
    rig.tap(Button::F4);  // (the question first: E-SREC-09)
    rig.settle();
  }
  NEED(rig.rec->state() == app::Recorder::State::Idle);
  const char* why = rig.app->start_og_export();
  CHECK(why == nullptr);
  CHECK(saves.og_state() == app::SaveService::OgState::Running);
  CHECK(std::string(rig.app->start_og_export()) == "A JOB IS RUNNING");  // one at a time
  for (unsigned i = 0; i < 4000 && saves.og_state() == app::SaveService::OgState::Running; ++i) {
    rig.pass();
  }
  CHECK(saves.og_state() == app::SaveService::OgState::Done);
  CHECK(saves.og_files_done() == saves.og_files_total());
  CHECK(card.has((std::string(saves.og_folder()) + "/project.mt").c_str()));
  CHECK(card.violations == 0u);
}

// OS-AUDIT-70 (E-SREC-06): the card says how long a take can run before it is armed and while it records. Idle: all the free arena; recording: the capacity less the frames; review: nothing.
TEST_CASE("record screen: TIME LEFT is the free arena when idle, the capacity less the frames while recording, and absent under review (OS-AUDIT-70)") {
  Rig rig;
  rig.tap(Button::SampleRec);
  rig.settle();
  rig.pass();
  std::uint32_t idle = 0;
  NEED(app::record_room_frames(rig.rec.get(), &rig.bank, idle));
  CHECK(idle == rig.bank.free_samples());
  CHECK(idle > 60000u);
  rig.tap(Button::F1);
  for (int i = 0; i < 60; ++i) {
    rig.pass();
  }
  NEED(rig.rec->state() == app::Recorder::State::Recording);
  std::uint32_t running = 0;
  NEED(app::record_room_frames(rig.rec.get(), &rig.bank, running));
  CHECK(running == rig.rec->capacity() - rig.rec->frames());
  CHECK(running < idle - 3000u);
  rig.tap(Button::F1);
  rig.pass();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  std::uint32_t review = 0;
  CHECK_FALSE(app::record_room_frames(rig.rec.get(), &rig.bank, review));
  CHECK_FALSE(app::record_room_frames(nullptr, &rig.bank, review));
}

// OS-AUDIT-71 (E-SREC-07): F3 SAVE puts the take in the first free slot and opens the keyboard over its name, prefilled RECORDING. F8 keeps the typed name (one undo unit), F7 keeps the saved one.
TEST_CASE("record screen: after F3 the keyboard opens on the new instrument's name; F8 renames it, F7 leaves RECORDING (OS-AUDIT-71)") {
  Rig rig;
  model::Project& project = test::arena(1).project;
  const auto take = [&rig] {
    rig.tap(Button::F1);
    for (int i = 0; i < 6; ++i) {
      rig.pass();
    }
    rig.tap(Button::F1);
    rig.pass();
  };
  unsigned slot = 0;
  while (project.instrument(slot) != nullptr) {
    ++slot;
  }
  rig.tap(Button::SampleRec);
  rig.settle();
  take();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F3);
  CHECK(rig.rec->state() == app::Recorder::State::Idle);  // the take is saved whatever the name does
  NEED(project.instrument(slot) != nullptr);
  CHECK(rig.state().overlay == app::OverlayId::TextEntry);
  CHECK(rig.state().text.target == app::TextEntryView::Target::Instrument);
  CHECK(std::string(rig.state().text.entry.text()) == "RECORDING");
  CHECK(std::string(project.instrument(slot)->name) == "RECORDING");
  // Type a key (it replaces the selected name), then F8.
  (void)rig.app->invoke(core::action_id("text.pad"), 12);
  const std::string typed = rig.state().text.entry.text();
  CHECK(typed != "RECORDING");
  CHECK_FALSE(typed.empty());
  rig.tap(Button::F8);
  CHECK(rig.state().overlay == app::OverlayId::None);
  CHECK(std::string(project.instrument(slot)->name) == typed);
  // A second take: F7 leaves the saved name.
  take();
  NEED(rig.rec->state() == app::Recorder::State::Review);
  rig.tap(Button::F3);
  CHECK(rig.state().overlay == app::OverlayId::TextEntry);
  rig.tap(Button::F7);
  CHECK(rig.state().overlay == app::OverlayId::None);
  NEED(project.instrument(slot + 1u) != nullptr);
  CHECK(std::string(project.instrument(slot + 1u)->name) == "RECORDING");
}

// ---- START ON LEVEL (OS-SREC-LEVEL, E-SREC-11): the screen's side; the tap and the recorder have their own tests (audio_taps_test, recorder_test) ----

namespace {
// Moves the cursor of the SOURCES page to `row` with Up and Down.
void level_goto_row(Rig& rig, unsigned row) {
  for (int guard = 0; guard < 16 && rig.state().record.row[0] != row; ++guard) {
    rig.tap(rig.state().record.row[0] < row ? Button::Down : Button::Up);
  }
  CHECK(rig.state().record.row[0] == row);
}
}  // namespace

TEST_CASE("record screen: START ON steps FREE, STEP, BAR, PATTERN and LEVEL; THRESHOLD and PRE-ROLL are rows only for LEVEL, and edit within their ranges") {
  CHECK(app::kRecAligns == 5u);
  CHECK(std::string(app::kRecAlignNames[static_cast<unsigned>(app::RecAlign::Level)]) == "LEVEL");
  CHECK(static_cast<unsigned>(app::RecAlign::Level) == 4u);
  // The rows, per page: the two follow ALIGN for LEVEL only, and a take under review shows its trim rows instead (no page is longer than eight).
  CHECK(app::rec_rows(RecMode::Sources, false, false, false).n == 6u);
  CHECK(app::rec_rows(RecMode::Sources, false, false, true).n == 8u);
  CHECK(app::rec_rows(RecMode::Sources, false, false, true).row[3] == app::RecRow::Align);
  CHECK(app::rec_rows(RecMode::Sources, false, false, true).row[4] == app::RecRow::Threshold);
  CHECK(app::rec_rows(RecMode::Sources, false, false, true).row[5] == app::RecRow::PreRoll);
  CHECK(app::rec_rows(RecMode::Sources, false, false, true).row[6] == app::RecRow::StopAt);
  CHECK(app::rec_rows(RecMode::Resample, false, false, true).row[2] == app::RecRow::Threshold);
  CHECK(app::rec_rows(RecMode::Resample, false, false, true).n == 6u);
  CHECK(app::rec_rows(RecMode::Radio, false, false, true).n == 8u);
  CHECK(app::rec_rows(RecMode::Radio, false, false, true).row[5] == app::RecRow::PreRoll);
  CHECK(app::rec_rows(RecMode::Sources, true, false, true).n == 8u);
  CHECK(app::rec_rows(RecMode::Sources, true, false, true).row[6] == app::RecRow::TrimStart);
  CHECK(app::rec_rows(RecMode::Sources, true, true, true).row[6] == app::RecRow::LoopVol);
  app::RecorderConfig cfg;
  cfg.align = app::RecAlign::Level;
  CHECK(app::level_rows(cfg));
  cfg.loop_bars = 2;
  CHECK_FALSE(app::level_rows(cfg));  // a LOOP starts on a bar: no THRESHOLD to set
  cfg.loop_bars = 0;
  cfg.align = app::RecAlign::Pattern;
  CHECK_FALSE(app::level_rows(cfg));

  Rig rig;
  rig.tap(Button::SampleRec);
  rig.settle();
  level_goto_row(rig, 3);  // START ON
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 6);
  rig.turn(-1);
  CHECK(rig.rec->cfg.align == app::RecAlign::EachStep);
  rig.turn(-1);
  CHECK(rig.rec->cfg.align == app::RecAlign::Free);
  rig.turn(-1);  // (the first choice: no wrap)
  CHECK(rig.rec->cfg.align == app::RecAlign::Free);
  rig.turn(1);
  CHECK(rig.rec->cfg.align == app::RecAlign::EachStep);  // (a detent is one choice, however many it turns)
  rig.turn(1);
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  rig.turn(1);
  CHECK(rig.rec->cfg.align == app::RecAlign::Pattern);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 6);
  rig.turn(1);
  CHECK(rig.rec->cfg.align == app::RecAlign::Level);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 8);
  rig.turn(1);  // (the last choice: no wrap)
  CHECK(rig.rec->cfg.align == app::RecAlign::Level);
  // THRESHOLD: a detent is 3 dB, Shift nine, -60 .. -6, Delete is -30.
  CHECK(rig.rec->cfg.level_db == -30);
  CHECK(rig.rec->cfg.preroll == app::kRecPreRollDefault);
  rig.tap(Button::Down);
  rig.turn(2);
  CHECK(rig.rec->cfg.level_db == -24);
  rig.turn(100);
  CHECK(rig.rec->cfg.level_db == -6);
  rig.turn(-100);
  CHECK(rig.rec->cfg.level_db == -60);
  rig.shift_turn(1);
  CHECK(rig.rec->cfg.level_db == -51);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.level_db == -30);
  // PRE-ROLL: 0, 10, 20, 50, 100 ms; Delete is 50.
  rig.tap(Button::Down);
  CHECK(app::kRecPreRollMs[rig.rec->cfg.preroll] == 50u);
  rig.turn(1);
  CHECK(app::kRecPreRollMs[rig.rec->cfg.preroll] == 100u);
  rig.turn(1);
  CHECK(app::kRecPreRollMs[rig.rec->cfg.preroll] == 100u);
  rig.turn(-100);
  CHECK(app::kRecPreRollMs[rig.rec->cfg.preroll] == 50u);
  rig.turn(-1);
  rig.turn(-1);
  rig.turn(-1);
  CHECK(app::kRecPreRollMs[rig.rec->cfg.preroll] == 0u);
  rig.tap(Button::Delete);
  CHECK(app::kRecPreRollMs[rig.rec->cfg.preroll] == 50u);
  // START ON's own Delete is BAR, which takes the two rows away again.
  level_goto_row(rig, 3);
  rig.tap(Button::Delete);
  CHECK(rig.rec->cfg.align == app::RecAlign::Bar);
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 6);
  CHECK(rig.rec->cfg.level_db == -30);  // (runtime only: the values stay in the recorder's configuration, nothing is stored)
}

TEST_CASE("record screen: LEVEL armed with the transport stopped stays LEVEL (no fall back to FREE, no count-in), listens, and records from the level") {
  LoopRig rig;
  CHECK(rig.app->device_settings().set(model::device_setting::CountIn, 1.0f) == model::Status::Ok);
  rig.rec->cfg.align = app::RecAlign::Level;
  CHECK_FALSE(rig.state().play.playing);
  rig.silent = true;
  rig.pass();
  rig.tap(Button::F1);
  CHECK(rig.message().empty());
  CHECK(rig.rec->state() == app::Recorder::State::Armed);  // listening is the armed state: the screen says WAITING FOR LEVEL
  CHECK(rig.rec->wait_align() == audio::RecTap::Align::Level);
  CHECK(rig.engine.graph().rec().state() == audio::RecTap::State::Listening);
  CHECK(rig.transport.counted == 0u);  // LEVEL needs no timeline: no count-in, no transport
  CHECK(rig.transport.plays == 0u);
  for (int i = 0; i < 12; ++i) {
    rig.pass();
  }
  CHECK(rig.rec->state() == app::Recorder::State::Armed);  // silence: still listening
  CHECK(rig.rec->frames() == 0u);
  rig.silent = false;
  CHECK(rig.until([&] { return rig.rec->state() == app::Recorder::State::Recording; }, 40));
  for (int i = 0; i < 10; ++i) {
    rig.pass();
  }
  CHECK(rig.rec->frames() > 4u * ha::kBlock);
  rig.tap(Button::F1);  // stop
  rig.pass();
  CHECK(rig.rec->state() == app::Recorder::State::Review);
  CHECK(rig.rec->trim_start() == rig.rec->trim_floor());
  CHECK(rig.app->failed_frames() == 0);
  // Under review the two rows give way to the trim rows (the page is eight rows long).
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 8);
  CHECK(app::rec_rows(RecMode::Sources, true, false, app::level_rows(rig.rec->cfg)).row[6] == app::RecRow::TrimStart);
}

TEST_CASE("record screen: LEVEL with LOOP is a bar-aligned loop: the two rows are hidden, START ON says why it does not change, and F1 waits for the bar") {
  LoopRig rig;
  rig.rec->cfg.align = app::RecAlign::Level;
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 8);
  rig.rec->cfg.loop_bars = 1;
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 6);  // THRESHOLD and PRE-ROLL are gone
  level_goto_row(rig, 3);
  rig.turn(-1);
  CHECK(rig.message() == "LOOP STARTS THE TAKE ON A BAR");
  CHECK(rig.rec->cfg.align == app::RecAlign::Level);  // (untouched: it is back when LOOP goes off)
  rig.play(true);
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  CHECK(rig.rec->loop_take());
  CHECK(rig.rec->wait_align() == audio::RecTap::Align::Bar);  // forced to BAR, never a level
  CHECK(rig.engine.graph().rec().state() != audio::RecTap::State::Listening);
  rig.tap(Button::F1);  // cancel
  rig.rec->cfg.loop_bars = 0;
  CHECK(app::kRecordScreen.count(rig.app->model(), rig.state()) == 8);  // LEVEL again, with its rows
}

TEST_CASE("record screen: F1 while the take listens cancels it with no take, and the level that comes later starts nothing") {
  LoopRig rig;
  rig.rec->cfg.align = app::RecAlign::Level;
  rig.silent = true;
  rig.pass();
  rig.tap(Button::F1);
  NEED(rig.rec->state() == app::Recorder::State::Armed);
  for (int i = 0; i < 4; ++i) {
    rig.pass();
  }
  rig.tap(Button::F1);
  for (int i = 0; i < 4; ++i) {
    rig.pass();
  }
  CHECK(rig.rec->state() == app::Recorder::State::Idle);  // not Review: there is no take
  CHECK(rig.rec->frames() == 0u);
  CHECK(rig.engine.graph().rec().state() != audio::RecTap::State::Listening);
  rig.silent = false;
  for (int i = 0; i < 12; ++i) {
    rig.pass();
  }
  CHECK(rig.rec->state() == app::Recorder::State::Idle);
  CHECK(rig.rec->frames() == 0u);
  // And the key works again: a new take listens.
  rig.silent = true;
  rig.pass();
  rig.tap(Button::F1);
  CHECK(rig.rec->state() == app::Recorder::State::Armed);
  rig.tap(Button::F1);
  rig.pass();
}
