// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-GATE-BOOTCRASH: boot to the first frame of the app, on the host, under the sanitizers. The composition root of the MK66 image
// (platform/mk66/src/main.cpp, app_main.cpp start_app()) cannot run on the host: it needs the registers. What it does between the
// display coming up and the app's first frame is logic over interfaces that the host can run: the real FT812 driver on its chip
// model, ui::upload_assets, the boot screen, the BootGate hold (idle hook and main-loop passes), the App built in SDRAM-style
// storage (poisoned, then zeroed, as start_app() does) and run until its first frame is on screen with the light on. This test runs
// that sequence in the image's order and reports where it ended; it fails for a boot that never reaches the app's first frame.
//
// What it can see: a crash or UB (ASan and UBSan: the host build of ci.sh) or a hang in the boot sequence, a boot frame that never
// shows, a hold that is never released, an App that cannot draw its first frame (a frame that does not fit, an upload that is
// refused). What it cannot see: anything the image's own bytes or the silicon cause (stale flash, a wrong register value, a linker
// layout, the SD bootloader's partial sector: OS-BUG-COSMBOOT), and the SD, USB and audio parts of main().
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_display_fakes.h"

#include <o3os/app/app.h>
#include <o3os/core/boot_hold.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/boot_gate.h>
#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/boot_screen.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace display = o3os::platform::mk66::display;
using o3os::platform::host::FakeClock;
using o3os::platform::host::FakeInput;
using o3os::platform::mk66::BootGate;
using o3os::platform::mk66::display::fake::FakeBulk;
using o3os::platform::mk66::display::fake::FakeChip;
using Verdict = BootGate::Verdict;

namespace {

// How far a boot got. Every field is true only when the step happened; `first_frame` needs all of the others.
struct BootResult {
  bool chip_up = false;      // Chip::init() returned Ok
  bool assets_up = false;    // ui::upload_assets() returned Ok
  bool boot_frame = false;   // the boot screen was committed
  bool lit = false;          // the boot frame was swapped in and the backlight came on
  bool released = false;     // the hold ended
  bool first_frame = false;  // the app's own frame is the one the chip scans, the light is on, nothing failed
  std::uint64_t released_ms = 0;
  std::uint32_t app_failed_frames = 0;
};

// The first picture of the image, as app_main.cpp builds it. `dl_words` is the App's frame buffer size (a test breaks it on purpose).
BootResult boot_to_first_frame(FakeChip& chip, std::size_t dl_words = o3os::eve::kMaxDisplayListWords) {
  BootResult r;
  FakeBulk bulk(chip);
  display::Chip driver(chip);
  display::Ft812Display disp(driver, bulk);
  r.chip_up = driver.init(display::InitConfig{}).status == display::InitStatus::Ok;
  if (!r.chip_up) {
    return r;
  }
  disp.start(true);
  BootGate gate;
  const auto now_ms = [&] { return static_cast<std::uint64_t>(chip.now_us() / 1000u); };
  const auto wait_ms = [&](std::uint32_t n) {  // main() blocked in boot::delay_ms(): time passes and the idle hook services the display
    for (std::uint32_t i = 0; i < n; ++i) {
      chip.advance_ms(1);
      gate.idle(disp, now_ms());
    }
  };
  {
    BootGate::DriverScope in_driver(gate);
    r.assets_up = o3os::ui::upload_assets(disp) == o3os::hal::UploadResult::Ok;
    if (!r.assets_up) {
      return r;
    }
    r.boot_frame = o3os::ui::show_boot_screen(disp, "Track3r", "gate-test");
  }
  if (!r.boot_frame) {
    return r;
  }
  gate.shown(now_ms());
  wait_ms(700);  // the audio start, the card: the hook alone keeps the display going
  r.lit = disp.backlight() != 0 && chip.swaps() >= 1u;
  const std::vector<std::uint8_t> boot_list = chip.displayed();

  // start_app(): the App in SDRAM, which holds whatever it held (poison), zeroed, then built in place.
  alignas(o3os::app::App) static unsigned char storage[sizeof(o3os::app::App)];
  std::memset(storage, 0xA5, sizeof storage);
  std::memset(storage, 0, sizeof storage);
  FakeInput input;
  FakeClock clock;
  auto* app = new (storage) o3os::app::App(input, clock, disp, dl_words);

  // The main loop, at most 6 s of boot time: pass() services the display inside the driver, the gate says hold or run.
  for (int pass = 0; pass < 6000 && !r.first_frame; ++pass) {
    chip.advance_ms(1);
    clock.advance(1000);
    Verdict v = Verdict::Run;
    {
      BootGate::DriverScope scope(gate);
      disp.service();
      v = gate.pass(disp, now_ms());
    }
    if (v == Verdict::Released) {
      r.released = true;
      r.released_ms = now_ms();
    }
    if (v == Verdict::Hold) {
      continue;  // the app's frame is skipped while the logo is held
    }
    {
      BootGate::DriverScope scope(gate);
      app->run_once();
    }
    r.first_frame = r.lit && !app->dirty() && disp.idle() && chip.displayed() != boot_list && app->failed_frames() == 0u;
  }
  r.app_failed_frames = app->failed_frames();
  r.first_frame = r.first_frame && disp.backlight_on() && !disp.failed() && chip.bus_violations() == 0u;
  app->~App();
  return r;
}

}  // namespace

TEST_CASE("boot to first frame: chip up, assets, boot screen, light, hold released after 800 ms, then the app's own frame") {
  FakeChip chip;
  const BootResult r = boot_to_first_frame(chip);
  CHECK(r.chip_up);
  CHECK(r.assets_up);
  CHECK(r.boot_frame);
  CHECK(r.lit);
  CHECK(r.released);
  CHECK(r.released_ms >= o3os::core::kBootMinMs);
  CHECK(r.app_failed_frames == 0u);
  CHECK(r.first_frame);
  CHECK(chip.bus_violations() == 0u);
}

// The same sequence on inputs that break it on purpose: each must end where it says and never report a first frame.
TEST_CASE("boot to first frame: a boot that breaks is reported, at the step that broke") {
  SUBCASE("no display chip: stops at the chip") {
    FakeChip chip;
    chip.present = false;
    const BootResult r = boot_to_first_frame(chip);
    CHECK_FALSE(r.chip_up);
    CHECK_FALSE(r.first_frame);
  }
  SUBCASE("a chip that does not leave reset: stops at the chip") {
    FakeChip chip;
    chip.cpureset_stuck = true;
    const BootResult r = boot_to_first_frame(chip);
    CHECK_FALSE(r.chip_up);
    CHECK_FALSE(r.first_frame);
  }
  SUBCASE("a scan-out that never swaps: the boot frame never lights, no first frame") {
    FakeChip chip;
    chip.swap_stuck = true;
    const BootResult r = boot_to_first_frame(chip);
    CHECK_FALSE(r.lit);
    CHECK_FALSE(r.first_frame);
  }
  SUBCASE("an app whose frame does not fit its buffer: the boot is up and lit, the app never draws") {
    FakeChip chip;
    const BootResult r = boot_to_first_frame(chip, 8);
    CHECK(r.chip_up);
    CHECK(r.lit);
    CHECK_FALSE(r.first_frame);
    CHECK(r.app_failed_frames > 0u);
  }
}
