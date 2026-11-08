// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-003f, the boot logo over the FT812 driver, end to end on the host (the chip model and a DMA engine fake instead of the
// hardware): the assets go up through the driver's upload path, the boot frame is the first thing committed, the backlight stays
// off until that frame is on screen, the picture the simulator's rasteriser makes from what the chip holds is the simulator's own
// boot picture bit for bit, and the hold (core::BootHold, which the platform's pass() runs) releases the app 800 ms after the first
// light and not before. Host only (the simulator's rasteriser is megabytes of framebuffer).
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_display_fakes.h"

#include <o3os/core/boot_hold.h>
#include <o3os/platform/host/sha256.h>
#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/boot_logo.h>
#include <o3os/ui/boot_screen.h>
#include <o3os/ui/plot/ram_g_map.h>
#include <o3os/usb/identity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <string>
#include <vector>

namespace display = o3os::platform::mk66::display;
using o3os::platform::mk66::display::fake::FakeBulk;
using o3os::platform::mk66::display::fake::FakeChip;
using o3os::sim::EveRaster;
using o3os::sim::RasterDisplay;

namespace {

std::string picture_hash(const EveRaster& raster) {
  return o3os::platform::host::sha256_hex(raster.rgba(), EveRaster::kFramebufferBytes);
}

std::vector<std::uint32_t> displayed_words(const FakeChip& chip) {
  std::vector<std::uint32_t> words;
  const std::vector<std::uint8_t>& dl = chip.displayed();
  for (std::size_t i = 0; i + 4 <= dl.size(); i += 4) {
    std::uint32_t w;
    std::memcpy(&w, &dl[i], 4);
    words.push_back(w);
    if (w == 0) {
      break;
    }
  }
  return words;
}

constexpr const char* kBuild = "0123456789ab-dirty";

}  // namespace

TEST_CASE("display boot: the logo is the first frame, the backlight waits for it, and the picture is the simulator's") {
  // The reference: the boot screen on the simulator's display.
  EveRaster ref_raster;
  RasterDisplay ref_display(ref_raster);
  CHECK(o3os::sim::load_assets(ref_display) == o3os::hal::UploadResult::Ok);
  CHECK(o3os::ui::show_boot_screen(ref_display, o3os::usb::kProductString, kBuild));

  // The device path: the chip model behind the driver, brought up by the real init, the assets uploaded as main does.
  FakeChip chip;
  FakeBulk bulk(chip);
  display::Chip driver(chip);
  display::Ft812Display disp(driver, bulk);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  disp.start(true);
  CHECK(disp.self_test());
  CHECK(o3os::ui::upload_assets(disp) == o3os::hal::UploadResult::Ok);
  CHECK(disp.stats().frames == 0u);  // nothing has been drawn: the first frame of the driver is the boot frame
  CHECK(disp.backlight() == 0);

  // The logo's raw bytes are in the chip's RAM_G where the display list points (OS-FLASHDIET A5: inflated from its
  // stream by the upload), and the rest of its region is untouched.
  std::uint8_t logo_px[o3os::ui::kBootLogoBytes];
  CHECK(o3os::ui::boot_logo_bitmap(logo_px, sizeof logo_px));
  CHECK(std::memcmp(chip.ram_g_all().data() + o3os::ui::plot::kRamGBootLogo, logo_px, o3os::ui::kBootLogoBytes) == 0);

  CHECK(o3os::ui::show_boot_screen(disp, o3os::usb::kProductString, kBuild));
  CHECK(disp.stats().frames == 1u);
  CHECK(disp.backlight() == 0);  // committed, not yet on screen: no light
  unsigned ms = 0;
  while (disp.backlight() == 0 && ms < 400) {
    chip.advance_ms(1);
    disp.service();
    ++ms;
  }
  CHECK(disp.backlight() != 0);  // the ramp began once the swap had happened ...
  CHECK(disp.stats().swaps_done >= 1u);  // ... and not before it
  CHECK(ms < 100);               // within a frame time or two of the commit
  for (int i = 0; i < 400 && !disp.backlight_on(); ++i) {
    chip.advance_ms(1);
    disp.service();
  }
  CHECK(disp.backlight_on());
  CHECK(chip.reg(display::ft812::reg::kPwmDuty) == display::kBacklightDefault);

  // What the simulator's rasteriser makes of what the chip holds is what it makes of the simulator's own run.
  EveRaster raster;
  CHECK(raster.write_ram_g(0, chip.ram_g_all().data(), o3os::hal::kRamGBytes));
  const std::vector<std::uint32_t> words = displayed_words(chip);
  CHECK(words.size() > 40u);
  CHECK(raster.render(words.data(), words.size()));
  CHECK(picture_hash(raster) == picture_hash(ref_raster));

  CHECK(disp.stats().bulk_failures == 0u);
  CHECK(disp.stats().polled_sends == 0u);  // the boot frame and every asset chunk went over the engine
  CHECK(chip.bus_violations() == 0u);
}

TEST_CASE("display boot: the hold the platform runs lets the app draw 800 ms after the first light, and no later") {
  FakeChip chip;
  FakeBulk bulk(chip);
  display::Chip driver(chip);
  display::Ft812Display disp(driver, bulk);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  disp.start(true);
  CHECK(o3os::ui::upload_assets(disp) == o3os::hal::UploadResult::Ok);

  // The boot as main runs it: the frame is committed at t = 0 (the fake's own clock is the platform's uptime here), the blocking
  // bring-up of audio and USB services the display a millisecond at a time (the idle hook), and the loop starts at `loop_ms`.
  const auto uptime = [&] { return static_cast<std::uint64_t>(driver.link().now_ms()); };
  CHECK(o3os::ui::show_boot_screen(disp, o3os::usb::kProductString, kBuild));
  o3os::core::BootHold hold;
  hold.start(uptime());
  const std::uint64_t committed = uptime();
  std::uint64_t first_light = 0;
  std::uint64_t released = 0;
  for (int step = 0; step < 3000 && released == 0; ++step) {
    chip.advance_ms(1);
    disp.service();
    const std::uint64_t now = uptime();
    if (disp.backlight() != 0) {
      if (first_light == 0) {
        first_light = now;
      }
      hold.lit(now);
    }
    if (!hold.holding(now)) {
      released = now;  // the app's first frame is drawn on this pass
    }
  }
  CHECK(first_light != 0);
  CHECK(first_light > committed);  // after the frame, never before
  CHECK(released != 0);
  if (released == 0) {
    return;
  }
  // The hold counted from the first light the loop saw: released exactly kBootMinMs later (within the fake clock's own ticks,
  // which advance a few microseconds per register access).
  CHECK(released >= first_light + o3os::core::kBootMinMs);
  CHECK(released <= first_light + o3os::core::kBootMinMs + 3);
  CHECK(released - committed >= o3os::core::kBootMinMs);  // never shorter than the minimum from the commit either
}
