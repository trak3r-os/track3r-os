// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-BUG-PLAYSTOP: on the unit, the HIL console's `key play` tap right after another `key play` tap is lost every other time (the transport stays
// stopped, playing=0). Both taps are the same key (Play toggles: app/src/pattern_actions.cpp transport_play), and the console injects a tap as a
// press now and a release 40 ms later (platform/mk66/usb/src/hil_inject.cpp kInjectTapMs), both into the scanner's queue. This test models the
// device's main loop on the host: the loop wakes every millisecond (SysTick, platform/mk66/src/main.cpp), the console's service runs then (pushing
// the tap releases that are due), the render steps a 64-sample block every 1.45 ms, and the App's pass runs every kFrameMs = 16 ms
// (platform/mk66/src/app_main.cpp). `key <b>` taps are injected exactly as the console's Injector does.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/view_state.h>
#include <o3os/audio/events.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <memory>

#include "support/need.h"
#include "support/project_fixture.h"
#include "support/seq_fixture.h"
#include "support/static_slot.h"

using namespace o3os;
using app::Button;

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];
constexpr std::uint32_t kTickUs = 1000;            // the main loop's SysTick: one wake per millisecond
constexpr unsigned kBlockSamples = 64;             // one render block
constexpr std::uint32_t kBlockUs = 1000000u * kBlockSamples / 44100u;  // 1451 us
constexpr std::uint32_t kInjectTapMs = 40;         // hil_inject.h kInjectTapMs

// The Play key's sink as the platform's SeqTransport is (platform/mk66/src/app_main.cpp): a command on the sequencer's ring.
class Sink final : public app::TransportSink {
 public:
  seq::Sequencer* s = nullptr;
  unsigned plays = 0;
  unsigned stops = 0;
  bool play(unsigned pattern) noexcept override {
    ++plays;
    return s->commands().push(seq::Command{seq::Command::Kind::PlayPattern, static_cast<std::uint8_t>(pattern), 0, 0});
  }
  bool play_song(unsigned row) noexcept override { return s->commands().push(seq::Command{seq::Command::Kind::PlaySong, 0, static_cast<std::uint16_t>(row), 0}); }
  bool stop() noexcept override {
    ++stops;
    return s->commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0});
  }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  seq::Sequencer* s = nullptr;
  Sink sink;
  std::uint64_t sample = 0;       // the render's timeline, in samples
  std::uint32_t render_debt_us = 0;  // us accumulated toward the next block
  std::uint32_t last_pass_ms = 0;
  struct Release {
    bool used = false;
    std::uint32_t due_ms = 0;
    std::uint8_t id = 0;
  };
  Release releases[8];  // the console Injector's release table (hil_inject.h kCapacity)
  std::uint32_t frame_ms;  // app_main.cpp kFrameMs = 16 on the unit; the simulator's pass runs every 1 ms tick

  explicit Rig(std::uint32_t frame = 16) : frame_ms(frame) {
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 64);
    (void)p.create_pattern(2, 64);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    s = &test::seq_slot().make(p);
    sink.s = s;
    app->attach_transport(&sink);
    app->attach_play_position(&s->play_position());
    app->run_once();
    last_pass_ms = static_cast<std::uint32_t>(clock.uptime_ms());
  }

  const app::ViewState& state() const { return app->state(); }

  // One wake of the main loop (1 ms): the console's service (the tap releases that are due), a render block when its 1451 us are up, the App's
  // pass when kFrameMs have passed.
  void tick() {
    clock.advance(kTickUs);
    const std::uint32_t now_ms = static_cast<std::uint32_t>(clock.uptime_ms());
    for (Release& r : releases) {  // console::service -> Injector::service: the releases that are due, in table order
      if (r.used && static_cast<std::int32_t>(now_ms - r.due_ms) >= 0) {
        input.push(core::make_button_event(clock.now(), r.id, false));
        r.used = false;
      }
    }
    render_debt_us += kTickUs;
    while (render_debt_us >= kBlockUs) {
      render_debt_us -= kBlockUs;
      audio::SeqEvent out[audio::kMaxEventsPerBlock];
      (void)s->step(sample, kBlockSamples, out, audio::kMaxEventsPerBlock);
      sample += kBlockSamples;
    }
    if (now_ms - last_pass_ms >= frame_ms) {  // app_main.cpp: the app's tick (kFrameMs = 16); the sim's is every 1 ms tick
      last_pass_ms = now_ms;
      app->run_once();
    }
  }
  void pump_ms(double ms) {
    const unsigned n = static_cast<unsigned>(ms + 0.5);
    for (unsigned i = 0; i < n; ++i) {
      tick();
    }
  }

  // The console's `key <button>` (a tap): the press now, the release kInjectTapMs later (Injector::inject).
  void key_tap(Button b) {
    const std::uint8_t id = static_cast<std::uint8_t>(b);
    input.push(core::make_button_event(clock.now(), id, true));
    for (Release& r : releases) {
      if (!r.used) {
        r.used = true;
        r.due_ms = static_cast<std::uint32_t>(clock.uptime_ms()) + kInjectTapMs;
        r.id = id;
        break;
      }
    }
  }
};

}  // namespace

TEST_CASE("OS-BUG-PLAYSTOP: a Play tap right after a Stop tap starts the transport again, at every gap the console's round trip can give") {
  Rig rig;
  rig.pump_ms(100);
  // The transport runs (a first Play tap, as the scenario's _play does).
  rig.key_tap(Button::Play);
  rig.pump_ms(400);
  NEED(rig.state().playing);
  NEED(rig.s->playing());
  // Stop then Play in quick succession. gap 0: both presses are queued before the next pass (the console answered both commands within one
  // 16 ms frame); larger gaps: the Stop tap is consumed by a pass before the Play tap's press arrives.
  unsigned lost = 0;
  for (unsigned gap = 0; gap <= 80; gap += 4) {
    if (!rig.state().playing) {  // an earlier round left it stopped: start it again as the scenario does
      rig.key_tap(Button::Play);
      rig.pump_ms(400);
      NEED(rig.state().playing);
    }
    const unsigned stops_before = rig.sink.stops;
    const unsigned plays_before = rig.sink.plays;
    rig.key_tap(Button::Play);  // the "stop"
    if (gap != 0u) {
      rig.pump_ms(gap);
    }
    rig.key_tap(Button::Play);  // the "play", right after
    rig.pump_ms(400);
    if (rig.sink.stops != stops_before + 1u || rig.sink.plays != plays_before + 1u || !rig.state().playing || !rig.s->playing()) {
      ++lost;
      MESSAGE("gap ", gap, " ms: stops ", rig.sink.stops - stops_before, " plays ", rig.sink.plays - plays_before, " playing=", rig.state().playing, " seq=", rig.s->playing());
    }
    CHECK(rig.sink.stops == stops_before + 1u);
    CHECK(rig.sink.plays == plays_before + 1u);
    CHECK(rig.state().playing);
    CHECK(rig.s->playing());
  }
  CHECK(lost == 0u);
}

// The same pair with the App's pass on every 1 ms tick, as the simulator's runs (sim/src/simulator.cpp step(): the pass every tick, the render
// block after it), and the tap pair at every gap a scenario's `tap play` twice can give.
TEST_CASE("OS-BUG-PLAYSTOP: the stop-then-play pair with the App's pass every millisecond, as in the simulator") {
  Rig rig(1);
  rig.pump_ms(100);
  rig.key_tap(Button::Play);
  rig.pump_ms(400);
  NEED(rig.state().playing);
  NEED(rig.s->playing());
  unsigned lost = 0;
  for (unsigned gap = 0; gap <= 10; ++gap) {
    if (!rig.state().playing) {
      rig.key_tap(Button::Play);
      rig.pump_ms(400);
      NEED(rig.state().playing);
    }
    const unsigned stops_before = rig.sink.stops;
    const unsigned plays_before = rig.sink.plays;
    rig.key_tap(Button::Play);  // the "stop"
    if (gap != 0u) {
      rig.pump_ms(gap);
    }
    rig.key_tap(Button::Play);  // the "play", right after
    rig.pump_ms(400);
    if (rig.sink.stops != stops_before + 1u || rig.sink.plays != plays_before + 1u || !rig.state().playing || !rig.s->playing()) {
      ++lost;
      MESSAGE("gap ", gap, " ms: stops ", rig.sink.stops - stops_before, " plays ", rig.sink.plays - plays_before, " playing=", rig.state().playing, " seq=", rig.s->playing());
    }
    CHECK(rig.sink.stops == stops_before + 1u);
    CHECK(rig.sink.plays == plays_before + 1u);
    CHECK(rig.state().playing);
    CHECK(rig.s->playing());
  }
  CHECK(lost == 0u);
}

// The same pair in LIVE (Shift + REC started the transport in Edit-following: the Stop then also switches the mode to Browse, which changes
// the context chain the gesture layer holds, app/src/app.cpp sync_context).
TEST_CASE("OS-BUG-PLAYSTOP: the stop-then-play pair in LIVE mode, where the Stop changes the Pattern mode") {
  Rig rig;
  rig.pump_ms(100);
  // Shift + REC from stopped starts the transport in LIVE (pattern_rec_free -> transport_start_live).
  auto start_live = [&rig]() {
    rig.key_tap(Button::Shift);
    rig.pump_ms(2);
    rig.key_tap(Button::Rec);  // Shift + REC
    rig.pump_ms(50);           // Shift's tap release goes out, the transport starts
  };
  start_live();
  rig.pump_ms(400);
  NEED(rig.state().playing);
  NEED(rig.s->playing());
  unsigned lost = 0;
  for (unsigned gap = 0; gap <= 80; gap += 8) {
    if (!rig.state().playing) {
      start_live();
      rig.pump_ms(400);
      NEED(rig.state().playing);
    }
    const unsigned stops_before = rig.sink.stops;
    rig.key_tap(Button::Play);  // the "stop" (also LIVE -> Browse)
    if (gap != 0u) {
      rig.pump_ms(gap);
    }
    rig.key_tap(Button::Play);  // the "play", right after
    rig.pump_ms(400);
    if (rig.sink.stops != stops_before + 1u || !rig.state().playing || !rig.s->playing()) {
      ++lost;
      MESSAGE("gap ", gap, " ms: stops ", rig.sink.stops - stops_before, " playing=", rig.state().playing, " seq=", rig.s->playing());
    }
    CHECK(rig.sink.stops == stops_before + 1u);
    CHECK(rig.state().playing);
    CHECK(rig.s->playing());
  }
  CHECK(lost == 0u);
}

// The same pair on the Song screen, where Play starts the song from the selected row (transport_play -> play_song).
TEST_CASE("OS-BUG-PLAYSTOP: the stop-then-play pair on the Song screen") {
  Rig rig;
  rig.pump_ms(100);
  rig.key_tap(Button::Song);  // the Song screen
  rig.pump_ms(400);
  NEED(rig.state().screen == app::ScreenId::Song);
  rig.key_tap(Button::Play);  // PLAY SONG
  rig.pump_ms(400);
  NEED(rig.state().playing);
  NEED(rig.s->playing());
  unsigned lost = 0;
  for (unsigned gap = 0; gap <= 80; gap += 8) {
    if (!rig.state().playing) {
      rig.key_tap(Button::Play);
      rig.pump_ms(400);
      NEED(rig.state().playing);
    }
    const unsigned stops_before = rig.sink.stops;
    rig.key_tap(Button::Play);  // the "stop"
    if (gap != 0u) {
      rig.pump_ms(gap);
    }
    rig.key_tap(Button::Play);  // the "play", right after
    rig.pump_ms(400);
    if (rig.sink.stops != stops_before + 1u || !rig.state().playing || !rig.s->playing()) {
      ++lost;
      MESSAGE("gap ", gap, " ms: stops ", rig.sink.stops - stops_before, " playing=", rig.state().playing, " seq=", rig.s->playing());
    }
    CHECK(rig.sink.stops == stops_before + 1u);
    CHECK(rig.state().playing);
    CHECK(rig.s->playing());
  }
  CHECK(lost == 0u);
}
