// SPDX-License-Identifier: GPL-3.0-or-later
//
// The boot screen in the simulator's pixels (host only; the words it is made of are boot_screen_test.cpp): the logo's 372 x 372
// picture is exactly the bitmap at the guide's transform (every pixel), centred, on black, with the line under it centred; the
// gallery state of the screen is the same picture in every theme.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/host/sha256.h>
#include <o3os/sim/budget.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/boot_logo.h>
#include <o3os/ui/boot_screen.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>
#include <o3os/usb/identity.h>

#include <cstdint>
#include <cstring>
#include <ios>
#include <string>
#include <vector>

using namespace o3os;
using sim::EveRaster;

namespace {

struct Panel {
  EveRaster raster;
  sim::RasterDisplay display{raster};
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * EveRaster::kWidth + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
  std::string hash() const { return platform::host::sha256_hex(raster.rgba(), EveRaster::kFramebufferBytes); }
};

unsigned logo_level(const std::uint8_t* px, unsigned x, unsigned y) {
  const std::uint8_t b = px[y * ui::kBootLogoStride + x / 2];
  return (x % 2 == 0) ? b >> 4 : b & 0xFu;
}

// A pixel of a white bitmap of alpha `a` over black, as the simulator blends it: a grey of that value.
std::uint32_t grey(unsigned a) { return (a << 16) | (a << 8) | a; }

const sim::ScreenState* boot_state() {
  for (unsigned s = 0; s < sim::screen_count(); ++s) {
    const sim::Screen& screen = sim::screens()[s];
    if (std::string(screen.name) == "boot") {
      for (unsigned i = 0; i < screen.state_count; ++i) {
        if (std::string(screen.states[i].name) == "logo") {
          return &screen.states[i];
        }
      }
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("boot screen pixels: the logo is the bitmap at x3 pixel for pixel, centred, on black, and nothing else is on the screen but the line") {
  Panel p;
  CHECK(ui::show_boot_screen(p.display, usb::kProductString, "0123456789ab"));
  CHECK(p.display.render_failures() == 0);
  // OS-FLASHDIET A5: the logo's raw bitmap, inflated from its stream (the pixels the RAM_G upload put there).
  std::uint8_t logo_px[ui::kBootLogoBytes];
  CHECK(ui::boot_logo_bitmap(logo_px, sizeof logo_px));

  // Every pixel of the 372 x 372 rectangle. The coefficient is 85 (8.8, a third is 85.33), so the source pixel of column dx is
  // floor((2 dx + 1) * 85 / 512): the guide's matrix at the pixel centre (the simulator's [A] assumption), worked out here.
  const auto source_of = [](unsigned d) { return ((2 * d + 1) * 85) / 512; };
  unsigned wrong = 0;
  for (unsigned dy = 0; dy < 372; ++dy) {
    for (unsigned dx = 0; dx < 372; ++dx) {
      const unsigned level = logo_level(logo_px, source_of(dx), source_of(dy));
      wrong += p.pixel(ui::kBootLogoX + static_cast<int>(dx), ui::kBootLogoY + static_cast<int>(dy)) == grey(level * 17) ? 0 : 1;
    }
  }
  CHECK(wrong == 0);
  CHECK(source_of(371) == 123);  // the last column is still inside the 124

  // Everything outside the logo's rectangle and the line's rows is black.
  unsigned stray = 0;
  for (int y = 0; y < ui::kScreenH; ++y) {
    for (int x = 0; x < ui::kScreenW; ++x) {
      const bool in_logo = x >= ui::kBootLogoX && x < ui::kBootLogoX + 372 && y >= ui::kBootLogoY && y < ui::kBootLogoY + 372;
      const bool in_line = y >= ui::kBootTextY && y < ui::kBootTextY + 12;
      stray += (!in_logo && !in_line && p.pixel(x, y) != 0) ? 1 : 0;
    }
  }
  CHECK(stray == 0);

  // The logo is centred on the panel: its rectangle has the same margin left and right, above and below.
  CHECK(ui::kBootLogoX == 800 - (ui::kBootLogoX + 372));
  CHECK(ui::kBootLogoY == 480 - (ui::kBootLogoY + 372));
  // ... and so is the picture inside it: its lit pixels reach within one native pixel (3 px) of each edge of the rectangle
  int min_x = 800, max_x = -1, min_y = 480, max_y = -1;
  for (int y = ui::kBootLogoY; y < ui::kBootLogoY + 372; ++y) {
    for (int x = ui::kBootLogoX; x < ui::kBootLogoX + 372; ++x) {
      if (p.pixel(x, y) != 0) {
        min_x = x < min_x ? x : min_x;
        max_x = x > max_x ? x : max_x;
        min_y = y < min_y ? y : min_y;
        max_y = y > max_y ? y : max_y;
      }
    }
  }
  CHECK(min_x - ui::kBootLogoX <= 3);
  CHECK(ui::kBootLogoX + 371 - max_x <= 3);
  CHECK(min_y - ui::kBootLogoY <= 3);
  CHECK(ui::kBootLogoY + 371 - max_y <= 3);
  // The edges where the transform is not 3 px a block: block 42 is 4 px wide (columns 126..129) and the last block 2 px (370, 371)
  const unsigned wide = 42, last = 123;
  CHECK(source_of(125) == wide - 1);
  CHECK(source_of(126) == wide);
  CHECK(source_of(129) == wide);
  CHECK(source_of(130) == wide + 1);
  CHECK(source_of(369) == last - 1);
  CHECK(source_of(370) == last);
  CHECK(source_of(371) == last);
}

TEST_CASE("boot screen pixels: the line is centred under the logo, in the dim and bright colours of the boot theme") {
  Panel p;
  CHECK(ui::show_boot_screen(p.display, usb::kProductString, "0123456789ab"));
  int min_x = 800, max_x = -1;
  bool bright = false, dim = false;
  const ui::Rgb& d = ui::boot_theme().rgb(ui::Color::TextDim);
  const std::uint32_t dim_rgb = (std::uint32_t{d.r} << 16) | (std::uint32_t{d.g} << 8) | d.b;
  for (int y = ui::kBootTextY; y < ui::kBootTextY + 12; ++y) {
    for (int x = 0; x < 800; ++x) {
      const std::uint32_t v = p.pixel(x, y);
      if (v != 0) {
        min_x = x < min_x ? x : min_x;
        max_x = x > max_x ? x : max_x;
        bright = bright || v == 0xFFFFFF;
        dim = dim || v == dim_rgb;
        CHECK_MESSAGE((v == 0xFFFFFF || v == dim_rgb), "pixel " << x << "," << y << " is 0x" << std::hex << v);  // L1 glyphs, two colours
      }
    }
  }
  CHECK(bright);
  CHECK(dim);
  // 26 characters of 6 px, centred as a whole; the glyphs' ink starts and ends a pixel or two inside their cells
  const int left = min_x, right = 799 - max_x;
  CHECK(left >= 322);
  CHECK(left <= 322 + 2);
  CHECK(right >= 322);
  CHECK(right <= 322 + 2);
  // the line is under the logo, with the 14 px gap, and inside the screen
  CHECK(ui::kBootTextY == ui::kBootLogoY + 372 + 14);
  CHECK(ui::kBootTextY + 12 <= 480);
}

TEST_CASE("boot screen pixels: it is the same picture in every theme: the gallery state, the budget words and the device's own call") {
  const sim::ScreenState* state = boot_state();
  CHECK(state != nullptr);
  if (state == nullptr) {
    return;
  }
  CHECK(state->fixed_theme != nullptr);
  CHECK(&state->fixed_theme() == &ui::boot_theme());
  CHECK_FALSE(state->worst_case);
  CHECK(state->in_gallery);

  // What the device's call draws.
  Panel device;
  CHECK(ui::show_boot_screen(device.display, usb::kProductString, "0123456789ab"));
  const std::string want = device.hash();

  // What the gallery draws in each theme's slot (the way sim/gallery.cpp builds it): the same hash three times.
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const ui::Theme& swept = ui::theme(ui::theme_id_at(t));
    Panel p;
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(p.display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, state->fixed_theme != nullptr ? state->fixed_theme() : swept);
    CHECK(r.begin(ui::Color::Pane));
    state->draw(r, 0);
    CHECK(r.end());
    CHECK_MESSAGE(p.hash() == want, "theme " << swept.name);
  }

  // The budget harness: the same words in every theme and both translate modes, far below the chip's 2048.
  std::size_t words = 0;
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    for (const bool translate : {true, false}) {
      bool complete = false;
      const std::size_t n = sim::measure_variant(*state, 0, ui::theme(ui::theme_id_at(t)), translate, &complete);
      CHECK(complete);
      CHECK(n > 0);
      CHECK(n <= ui::kBootScreenMaxWords);
      if (words == 0) {
        words = n;
      }
      CHECK(n == words);
    }
  }
  // The gallery state and the device draw the same words (DISPLAY included) for the same text.
  platform::host::FakeDisplay sink;
  CHECK(ui::show_boot_screen(sink, usb::kProductString, "0123456789ab"));
  CHECK(sink.last_frame().size() == words);
}
