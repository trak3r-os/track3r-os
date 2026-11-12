// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-BUG-HOLD: the whole button path across wraps of the 32-bit microsecond tick (2^32 us = 71.6 min), from the key chips (TCA8418
// models) or the console's injector, through the scan's debounce and its stamping, the queue, the app's pass and the gesture
// recogniser, to the action that a Hold or a Tap dispatches (the pattern choosers of F1: a tap toggles one, a hold opens it until the
// release). tests/gesture_test.cpp covers the recogniser alone with timestamps that wrap; this is the path in one piece, with the
// clock the real unit has (64-bit microseconds, now() the low word) and with the idle gaps between two inputs that a wrap makes
// dangerous: a stamp compared with the one before it is only meaningful while they are less than 2^31 us (35.8 min) apart.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/view_state.h>
#include <o3os/core/gesture.h>
#include <o3os/core/input_event.h>
#include <o3os/hal/clock.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/input/input_map.h>
#include <o3os/platform/mk66/input/scanner.h>
#include <o3os/platform/mk66/usb/hil_inject.h>

#include "../platform/mk66/tests/mk66_input_fakes.h"
#include "support/project_fixture.h"

#include <cstdint>
#include <memory>
#include <string>

using namespace o3os;
using app::Button;
using app::OverlayId;
namespace mk66in = o3os::platform::mk66::input;
namespace fake = o3os::platform::mk66::input::fake;
namespace console = o3os::platform::mk66::console;

namespace {

alignas(8) std::uint8_t g_wrap_ring[model::kUndoRegionBytes];

constexpr std::uint64_t kWrapUs = std::uint64_t{1} << 32;  // one wrap of the tick
constexpr std::uint64_t kMs = 1000;
constexpr std::uint64_t kMinute = 60 * 1000 * kMs;

// The unit's clock: 64-bit microseconds, now() the low 32 bits (platform/mk66/src/clock.cpp), uptime_ms() the whole count.
class WrapClock final : public hal::Clock {
 public:
  core::Tick now() const noexcept override { return static_cast<core::Tick>(us_); }
  std::uint64_t uptime_ms() const noexcept override { return us_ / kMs; }
  void set(std::uint64_t us) noexcept { us_ = us; }
  void advance(std::uint64_t us) noexcept { us_ += us; }

 private:
  std::uint64_t us_ = 0;
};

// Which door the keys come in by.
enum class Door { Scan, Console };

struct Rig {
  WrapClock clock;
  fake::Bus pad_bus;
  fake::Bus control_bus;
  fake::KeyChip pad_chip;
  fake::KeyChip button_chip;
  fake::Hardware hardware{pad_chip, button_chip};
  std::unique_ptr<mk66in::Scanner> scanner;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  console::Injector injector;
  Door door;

  Rig(std::uint64_t start_us, Door by) : door(by) {
    clock.set(start_us);
    pad_bus.attach(0x34, pad_chip);
    control_bus.attach(0x34, button_chip);
    scanner = std::make_unique<mk66in::Scanner>(pad_bus, control_bus, hardware, clock);
    app = std::make_unique<app::App>(*scanner, clock, display);
    model::Project& p = test::arena(0).project;
    p.reset();
    (void)p.create_pattern(1, 64);
    (void)p.append_order(1);
    journal.begin(g_wrap_ring, sizeof g_wrap_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    scanner->begin();
    pass(2);
  }

  static bool push(void* context, core::InputKind kind, std::uint8_t id, std::int16_t value) {
    return static_cast<mk66in::Scanner*>(context)->inject(kind, id, value);
  }
  // One pass of the main loop: time, the console's due releases (the diag service), the scan, the app.
  void pass(std::uint32_t ms = 1) {
    clock.advance(ms * kMs);
    injector.service(static_cast<std::uint32_t>(clock.uptime_ms()), true, &push, scanner.get());
    scanner->service();
    app->run_once();
  }
  // Time passes for `ms` in passes of 5 ms at most, as the real loop runs (one big step would stamp a chip's press late).
  void run(std::uint32_t ms) {
    while (ms > 0) {
      const std::uint32_t step = ms < 5 ? ms : 5;
      pass(step);
      ms -= step;
    }
  }
  // Nothing happens for `us`: the loop goes on, in steps of a second at most (an hour of 1 ms passes would only slow the test).
  void idle(std::uint64_t us) {
    while (us > 0) {
      const std::uint64_t step = us < 1000 * kMs ? us : 1000 * kMs;
      clock.advance(step);
      injector.service(static_cast<std::uint32_t>(clock.uptime_ms()), true, &push, scanner.get());
      scanner->service();
      app->run_once();
      us -= step;
    }
  }
  void button_down(Button b) {
    if (door == Door::Scan) {
      button_chip.press(mk66in::key_of_button(static_cast<unsigned>(b)));
    } else {
      CHECK(scanner->inject(core::InputKind::Button, static_cast<std::uint8_t>(b), 1));
    }
  }
  void button_up(Button b) {
    if (door == Door::Scan) {
      button_chip.release(mk66in::key_of_button(static_cast<unsigned>(b)));
    } else {
      CHECK(scanner->inject(core::InputKind::Button, static_cast<std::uint8_t>(b), 0));
    }
  }
  // A finger down for `ms` (the console: `key f1 hold <ms>`, its release filed by the injector), then everything settles.
  void hold_and_look(Button b, std::uint32_t ms, OverlayId& while_down, bool& held_flag) {
    if (door == Door::Console) {
      console::InjectRequest r;
      r.kind = core::InputKind::Button;
      r.id = static_cast<std::uint8_t>(b);
      r.gesture = console::InjectGesture::Hold;
      r.hold_ms = ms;
      CHECK(injector.inject(r, static_cast<std::uint32_t>(clock.uptime_ms()), &push, scanner.get()) == console::Injector::Result::Ok);
    } else {
      button_down(b);
    }
    run(ms - 20);  // just before the release is due
    while_down = app->state().overlay;
    held_flag = app->state().edit.chooser_held;
    run(60);  // the release goes out (the console's by the injector, the chip's below)
    if (door == Door::Scan) {
      button_up(b);
      run(10);
    }
    run(10);
  }
  void tap(Button b) {
    if (door == Door::Console) {
      console::InjectRequest r;
      r.kind = core::InputKind::Button;
      r.id = static_cast<std::uint8_t>(b);
      r.gesture = console::InjectGesture::Tap;
      CHECK(injector.inject(r, static_cast<std::uint32_t>(clock.uptime_ms()), &push, scanner.get()) == console::Injector::Result::Ok);
      run(console::kInjectTapMs + 20);
    } else {
      button_down(b);
      run(40);
      button_up(b);
      run(40);
    }
    run(20);
  }
  OverlayId overlay() const { return app->state().overlay; }
};

// One timeline: a first tap of F1 and F1 again at `start_us` (two taps: the first opens the PATTERN chooser, the second closes it; OS-UXF-PAT item 6), `gap_us` of nothing, then a hold of F1 (the
// pattern chooser is open while it is down, closed by the release) and a tap (a Tap again, delivered at the release: it opens, a second closes).
void run_timeline(std::uint64_t start_us, std::uint64_t gap_us, Door door) {
  const std::string door_name = door == Door::Scan ? "scan" : "console";
  INFO("start " << start_us << " us (wrap " << (start_us >> 32) << ", tick " << static_cast<std::uint32_t>(start_us) << "), gap " << gap_us / kMinute << " min, door "
                << door_name);
  Rig rig(start_us, door);
  CHECK(rig.overlay() == OverlayId::None);
  rig.tap(Button::F1);
  CHECK(rig.overlay() == OverlayId::PatternChooser);  // a tap opens the chooser as the hold does
  rig.tap(Button::F1);
  CHECK(rig.overlay() == OverlayId::None);

  rig.idle(gap_us);
  CHECK(rig.overlay() == OverlayId::None);

  OverlayId while_down = OverlayId::None;
  bool held = false;
  rig.hold_and_look(Button::F1, core::kGestureTimings.hold_ms + 300, while_down, held);
  CHECK(while_down == OverlayId::PatternChooser);  // the Hold fired: the chooser is open while the key is down
  CHECK(held);
  CHECK(rig.overlay() == OverlayId::None);          // its release closed it (HoldEnd), it was no tap

  rig.tap(Button::F1);  // a tap, after the hold: it still is one (it opens the chooser, a second tap closes it)
  CHECK(rig.overlay() == OverlayId::PatternChooser);
  rig.tap(Button::F1);
  CHECK(rig.overlay() == OverlayId::None);

  // A hold that starts at the very edge of the gap's end, with no pass between (the first input after a long silence).
  rig.idle(gap_us);
  rig.hold_and_look(Button::F1, core::kGestureTimings.hold_ms + 100, while_down, held);
  CHECK(while_down == OverlayId::PatternChooser);
  CHECK(rig.overlay() == OverlayId::None);
}

}  // namespace

TEST_CASE("input path: baseline, early in the run, both doors") {
  run_timeline(10'000 * kMs, kMinute / 60, Door::Scan);
  run_timeline(10'000 * kMs, kMinute / 60, Door::Console);
}

TEST_CASE("input path: a hold is a Hold and a tap is a Tap across wraps of the tick, from the key chips and from the console") {
  // The clock starts 150 ms before a wrap (the first tap is on one side, the hold on the other), and 0, 1, 2 and 3 wraps in.
  // The gaps: none worth speaking of, then silences that are short, past half a wrap (35.8 min), most of a wrap, and about a wrap.
  const std::uint64_t gaps[] = {kMinute / 60, 20 * kMinute, 36 * kMinute, 50 * kMinute, 71 * kMinute, kWrapUs / 1 + 3 * 1000 * kMs};
  for (unsigned wraps = 0; wraps < 4; ++wraps) {
    const std::uint64_t start = (wraps + 1) * kWrapUs - 150 * kMs;
    for (const std::uint64_t gap : gaps) {
      run_timeline(start, gap, Door::Scan);
      run_timeline(start, gap, Door::Console);
    }
  }
}

TEST_CASE("input path: a hold is a Hold after a silence of any length between two wraps (every gap from 30 to 75 minutes in 1.5 minute steps)") {
  for (std::uint64_t gap = 30 * kMinute; gap <= 75 * kMinute; gap += 90 * 1000 * kMs) {
    run_timeline(3 * kWrapUs + 1000 * kMs, gap, Door::Scan);
    run_timeline(2 * kWrapUs - 40 * kMs, gap, Door::Console);
  }
}
