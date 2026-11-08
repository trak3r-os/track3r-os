// SPDX-License-Identifier: GPL-3.0-or-later
//
// The boot screen (OS-003f, ui/boot_screen.h, ui/boot_logo.h, core/boot_hold.h): the logo's data and flash budget, the display
// list the boot frame is, what is uploaded for it, and how long it stays. Runs on the host and on the Cortex-M4F under QEMU; the
// pixels it draws are checked beside the simulator's rasteriser (boot_screen_pixels_test.cpp, host only) and over the FT812
// driver (display_boot_test.cpp).
#include <doctest/doctest.h>

#include <o3os/core/boot_hold.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/boot_logo.h>
#include <o3os/ui/boot_screen.h>
#include <o3os/ui/plot/ram_g_map.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>
#include <o3os/usb/identity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <string>
#include <vector>

using namespace o3os;
namespace dl = o3os::eve::dl;

namespace {

std::vector<std::uint32_t> boot_frame(const char* name, const char* build) {
  platform::host::FakeDisplay sink;
  CHECK(ui::show_boot_screen(sink, name, build));
  CHECK(sink.frames() == 1);
  return sink.last_frame();
}

std::size_t count_of(const std::vector<std::uint32_t>& words, std::uint32_t word) {
  std::size_t n = 0;
  for (const std::uint32_t w : words) {
    n += w == word ? 1 : 0;
  }
  return n;
}

std::size_t index_of(const std::vector<std::uint32_t>& words, std::uint32_t word) {
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (words[i] == word) {
      return i;
    }
  }
  return words.size();
}

// VERTEX2II words of a bitmap handle: their number, and whether every one has row y.
struct Glyphs {
  std::size_t count = 0;
  bool all_on_row = true;
};
Glyphs glyphs_of(const std::vector<std::uint32_t>& words, unsigned handle, unsigned y) {
  Glyphs g;
  for (const std::uint32_t w : words) {
    if ((w >> 30) == 2u && ((w >> 7) & 31u) == handle) {
      ++g.count;
      g.all_on_row = g.all_on_row && ((w >> 12) & 511u) == y;
    }
  }
  return g;
}

// The nibble of pixel (x, y) of the logo's raw bitmap (L4, the first pixel of a byte in the high nibble).
unsigned logo_level(const std::uint8_t* px, unsigned x, unsigned y) {
  const std::uint8_t b = px[y * ui::kBootLogoStride + x / 2];
  return (x % 2 == 0) ? b >> 4 : b & 0xFu;
}

// FNV-1a, 64-bit: a digest of the inflated bitmap this file can check on the target too (the host's sha256 lives in
// a host-only library, and this test also builds for QEMU).
std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t n) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::size_t i = 0; i < n; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace

// ---- How long it stays ---------------------------------------------------------------------------------------------------

TEST_CASE("boot hold: nothing is held before start() or after release(), and the minimum is the spec's 800 ms") {
  core::BootHold h;
  CHECK_FALSE(h.active());
  CHECK_FALSE(h.holding(0));
  CHECK(core::kBootMinMs == 800);
  h.start(100);
  CHECK(h.active());
  CHECK(h.holding(100));
  h.release();
  CHECK_FALSE(h.active());
  CHECK_FALSE(h.holding(100));
}

TEST_CASE("boot hold: the minimum counts from the first light, to the millisecond, and a later light does not move it") {
  core::BootHold h;
  h.start(5000);
  CHECK(h.holding(5000 + 139));
  h.lit(5000 + 140);  // the first light: the ramp's first step, 140 ms after the commit
  h.lit(5000 + 300);  // seen again on a later pass: not a new light
  CHECK(h.holding(5000 + 140 + 799));
  CHECK_FALSE(h.holding(5000 + 140 + 800));
  CHECK_FALSE(h.holding(5000 + 140 + 801));
  CHECK(h.holding(5000 + 140));
}

TEST_CASE("boot hold: no light before start() is counted, and start() forgets an earlier hold") {
  core::BootHold h;
  h.lit(50);  // no hold yet
  h.start(1000);
  CHECK(h.holding(1000 + 799 + core::kBootLightGraceMs));  // unlit: the long deadline applies, not 50 + 800
  h.lit(1200);
  CHECK_FALSE(h.holding(2000));
  h.start(3000);  // a new hold: the old light is gone
  CHECK(h.holding(3000 + 800));
  CHECK(h.holding(3000 + 800 + core::kBootLightGraceMs - 1));
  CHECK_FALSE(h.holding(3000 + 800 + core::kBootLightGraceMs));
}

TEST_CASE("boot hold: a display that never lights does not hold the app for ever") {
  core::BootHold h;
  h.start(1000);
  CHECK(h.holding(1000 + core::kBootMinMs + core::kBootLightGraceMs - 1));
  CHECK_FALSE(h.holding(1000 + core::kBootMinMs + core::kBootLightGraceMs));
}

TEST_CASE("boot hold: boot work that outlasts the minimum adds no delay: the first pass of the loop releases at once") {
  // The commit at 0, the first light at 150 ms; the audio and USB bring-up keep main() busy and the loop starts at 1,100 ms.
  core::BootHold h;
  h.start(0);
  h.lit(150);
  CHECK_FALSE(h.holding(1100));  // 150 + 800 = 950 has passed: the app draws on this very pass
  // The same boot where the loop starts at 600 ms: it is held for exactly the 350 ms that remain, pass by pass.
  core::BootHold g;
  g.start(0);
  g.lit(150);
  std::uint64_t t = 600;
  while (g.holding(t)) {
    ++t;
  }
  CHECK(t == 950);
}

// ---- The bitmap and its flash budget ---------------------------------------------------------------------------------------

TEST_CASE("boot logo: 124 x 124 L4, 7,688 raw bytes (target 8 KB), stored as a deflate stream, word aligned, and it is the picture") {
  const ui::BootLogo logo = ui::boot_logo();
  CHECK(logo.size == ui::kBootLogoBytes);
  CHECK(ui::kBootLogoBytes == 7688);
  CHECK(ui::kBootLogoBytes <= 8192);  // the spec's budget for an uncompressed bitmap
  CHECK(logo.z_size < logo.size);      // OS-FLASHDIET A5: in flash it is the stream, not the raw bitmap
  CHECK(ui::kBootLogoW == 124);
  CHECK(ui::kBootLogoH == 124);
  CHECK(ui::kBootLogoFormat == eve::BitmapFormat::L4);
  CHECK(ui::kBootLogoStride * ui::kBootLogoH == ui::kBootLogoBytes);
  CHECK(reinterpret_cast<std::uintptr_t>(logo.z) % 4 == 0);  // declared alignas(4) in the generated file, ADR 0011
  // OS-FLASHDIET A5: the stream inflates to exactly the raw bitmap the generated file held before it was compressed
  // (FNV-1a-64 of those 7,688 bytes: 0x45A265B9DBAC0886), through the one shared decoder.
  std::uint8_t px[ui::kBootLogoBytes];
  CHECK(ui::boot_logo_bitmap(px, sizeof px));
  CHECK(fnv1a64(px, sizeof px) == 0x45A265B9DBAC0886ULL);
  // The picture: a floppy with the Tracker on it, a dither of every level. Black corners (the floppy's), a tight crop (picture
  // reaches all four edges), the levels all in use, about a fifth black and a quarter near white.
  unsigned hist[16] = {};
  unsigned top = 0, bottom = 0, left = 0, right = 0;
  for (unsigned y = 0; y < ui::kBootLogoH; ++y) {
    for (unsigned x = 0; x < ui::kBootLogoW; ++x) {
      const unsigned v = logo_level(px, x, y);
      ++hist[v];
      if (v > 1) {
        top += y == 0 ? 1 : 0;
        bottom += y == ui::kBootLogoH - 1 ? 1 : 0;
        left += x == 0 ? 1 : 0;
        right += x == ui::kBootLogoW - 1 ? 1 : 0;
      }
    }
  }
  CHECK(logo_level(px, 0, 0) == 0);
  CHECK(logo_level(px, ui::kBootLogoW - 1, 0) == 0);
  CHECK(logo_level(px, 0, ui::kBootLogoH - 1) == 0);
  CHECK(logo_level(px, ui::kBootLogoW - 1, ui::kBootLogoH - 1) == 0);
  CHECK(top >= 50);
  CHECK(bottom >= 50);
  CHECK(left >= 50);
  CHECK(right >= 20);
  unsigned used = 0, total = 0;
  for (const unsigned h : hist) {
    used += h != 0 ? 1 : 0;
    total += h;
  }
  CHECK(total == ui::kBootLogoW * ui::kBootLogoH);
  CHECK(used >= 14);
  CHECK(hist[0] + hist[1] >= total / 6);
  CHECK(hist[13] + hist[14] + hist[15] >= total / 5);
}

TEST_CASE("boot logo: upload_assets() inflates it into its own region of the RAM_G map, in chunks of at most 4 KB") {
  platform::host::FakeDisplay sink;
  sink.record_uploads(512 * 1024);
  CHECK(ui::upload_assets(sink) == hal::UploadResult::Ok);
  std::vector<std::uint8_t> region;
  std::size_t chunks = 0;
  for (const auto& u : sink.uploads()) {
    if (u.addr >= ui::plot::kRamGBootLogo && u.addr < ui::plot::kRamGBootLogo + ui::plot::kRamGBootLogoBytes) {
      CHECK(u.addr == ui::plot::kRamGBootLogo + region.size());  // contiguous, in order
      CHECK(u.bytes.size() <= hal::kMaxUploadBytes);
      region.insert(region.end(), u.bytes.begin(), u.bytes.end());
      ++chunks;
    } else {
      CHECK_MESSAGE(u.addr + u.bytes.size() <= ui::plot::kRamGBootLogo, "an asset upload at 0x" << std::hex << u.addr << " lands in or behind the logo's region");
    }
  }
  CHECK(chunks == 5);  // OS-FLASHDIET A5: inflated through the 1,920 B staging chunk: 4 x 1,920 + 8, all below the 4 KB cap
  CHECK(region.size() == ui::kBootLogoBytes);
  // OS-FLASHDIET A5: what went up is the inflated bitmap, byte for byte the raw bytes the stream holds.
  std::uint8_t px[ui::kBootLogoBytes];
  CHECK(ui::boot_logo_bitmap(px, sizeof px));
  CHECK(std::memcmp(region.data(), px, ui::kBootLogoBytes) == 0);
  CHECK(ui::plot::kRamGBootLogo + region.size() <= ui::plot::kRamGSpare);
  CHECK(sink.last_upload_result() == hal::UploadResult::Ok);
}

// ---- The frame -----------------------------------------------------------------------------------------------------------------

TEST_CASE("boot screen: the frame is black, one raw section for the logo and one line of Micro text, in far fewer words than the buffer") {
  const std::vector<std::uint32_t> w = boot_frame(usb::kProductString, "0123456789ab");
  CHECK(w.size() <= ui::kBootScreenMaxWords);
  CHECK(w.size() <= 120);  // measured: 76 (the gallery prints it); the margin is for a longer build id
  CHECK(w.front() == dl::clear_color_rgb(0, 0, 0));  // the boot theme's pane is black, whatever theme is stored
  CHECK(w[1] == dl::clear(true, true, true));
  CHECK(w.back() == dl::display());
  CHECK(&ui::boot_theme() == &ui::theme(ui::ThemeId::Black));
  CHECK(ui::boot_theme().rgb(ui::Color::Pane).r == 0);
  CHECK(ui::boot_theme().rgb(ui::Color::Pane).g == 0);
  CHECK(ui::boot_theme().rgb(ui::Color::Pane).b == 0);
}

TEST_CASE("boot screen: the logo is one scaled bitmap of its own handle and region, in the white of the theme's text colour, centred") {
  const std::vector<std::uint32_t> w = boot_frame(usb::kProductString, "0123456789ab");
  // 124 -> 372: A and E are 256 / 3 = 85 in 8.8 fixed point (guide 4.12), nearest, drawn 372 x 372 from the 124 x 124 L4 layout
  CHECK(ui::kBootLogoTransform == 85);
  CHECK(ui::kBootLogoDrawW == 372);
  CHECK(ui::kBootLogoDrawH == 372);
  CHECK(ui::kBootLogoX == 214);  // (800 - 372) / 2
  CHECK(ui::kBootLogoY == 54);   // (480 - 372) / 2
  CHECK(count_of(w, dl::bitmap_transform_a(85)) == 1);
  CHECK(count_of(w, dl::bitmap_transform_e(85)) == 1);
  CHECK(count_of(w, dl::bitmap_handle(ui::plot::kHandleBootLogo)) == 1);
  CHECK(count_of(w, dl::bitmap_source(ui::plot::kRamGBootLogo)) == 1);
  CHECK(count_of(w, dl::bitmap_layout(eve::BitmapFormat::L4, 62, 124)) == 1);
  CHECK(count_of(w, dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, 372, 372)) == 1);
  CHECK(count_of(w, dl::vertex2f(214 * 16, 54 * 16)) == 1);
  CHECK(count_of(w, dl::color_rgb(255, 255, 255)) >= 1);
  // The section is closed: SAVE_CONTEXT ... transform ... BEGIN(BITMAPS) VERTEX2F END ... RESTORE_CONTEXT, and the transform is
  // not set again after it (the text is drawn at the identity).
  const std::size_t save = index_of(w, dl::save_context());
  const std::size_t a = index_of(w, dl::bitmap_transform_a(85));
  const std::size_t vtx = index_of(w, dl::vertex2f(214 * 16, 54 * 16));
  const std::size_t restore = index_of(w, dl::restore_context());
  CHECK(save < a);
  CHECK(a < vtx);
  CHECK(vtx < restore);
  CHECK(count_of(w, dl::save_context()) == 1);
  CHECK(count_of(w, dl::restore_context()) == 1);
  CHECK(w[vtx - 1] == dl::begin(eve::Primitive::Bitmaps));
  CHECK(w[vtx + 1] == dl::end());
  CHECK(index_of(w, dl::bitmap_transform_a(85)) == a);
}

TEST_CASE("boot screen: the line is the device name and the build, Micro face, one row, name bright and the rest dim") {
  const std::vector<std::uint32_t> w = boot_frame(usb::kProductString, "0123456789ab");
  // Micro is the fourth face: handle 1 + 3. "Track3r" 7 glyphs, "o3os" 4, the build id 12; spaces draw nothing.
  const Glyphs g = glyphs_of(w, 4, static_cast<unsigned>(ui::kBootTextY));
  CHECK(g.count == 7 + 4 + 12);
  CHECK(g.all_on_row);
  CHECK(ui::kBootTextY == 440);
  CHECK(ui::kBootTextY == ui::kBootLogoY + ui::kBootLogoDrawH + 14);
  const ui::Rgb& dim = ui::boot_theme().rgb(ui::Color::TextDim);
  CHECK(count_of(w, dl::color_rgb(dim.r, dim.g, dim.b)) == 1);  // one colour change for the dim part
  // the name and the rest: the bright colour is set (for the logo and for the name), the dim one after the name's glyphs
  const std::size_t dim_at = index_of(w, dl::color_rgb(dim.r, dim.g, dim.b));
  std::size_t before = 0, after = 0;
  for (std::size_t i = 0; i < w.size(); ++i) {
    if ((w[i] >> 30) == 2u && ((w[i] >> 7) & 31u) == 4u) {
      (i < dim_at ? before : after) += 1;
    }
  }
  CHECK(before == 7);
  CHECK(after == 16);
  // Centred as a whole: 26 characters of 6 px
  const int width = 6 * static_cast<int>(7 + 2 + 5 + 12);
  const int x0 = (ui::kScreenW - width) / 2;
  bool first = false;
  for (const std::uint32_t word : w) {
    if ((word >> 30) == 2u && ((word >> 7) & 31u) == 4u) {
      if (!first) {
        CHECK(static_cast<int>((word >> 21) & 511u) == x0);  // the T of "Track3r"
        first = true;
      }
    }
  }
  CHECK(first);
}

TEST_CASE("boot screen: a long build id is cut, a missing one is nothing, and the frame stays within its bound") {
  std::string long_id(200, 'x');
  const std::vector<std::uint32_t> w = boot_frame(usb::kProductString, long_id.c_str());
  CHECK(glyphs_of(w, 4, static_cast<unsigned>(ui::kBootTextY)).count == 7 + 4 + ui::kBootBuildChars);
  CHECK(w.size() <= ui::kBootScreenMaxWords);
  const std::vector<std::uint32_t> none = boot_frame(nullptr, nullptr);
  CHECK(glyphs_of(none, 4, static_cast<unsigned>(ui::kBootTextY)).count == 4);  // only "o3os"
  const std::vector<std::uint32_t> dirty = boot_frame(usb::kProductString, "0123456789ab-dirty");
  CHECK(glyphs_of(dirty, 4, static_cast<unsigned>(ui::kBootTextY)).count == 7 + 4 + 18);
  CHECK(dirty.size() <= ui::kBootScreenMaxWords);
}

TEST_CASE("boot screen: both ways the renderer reaches x >= 512 give the same frame (the line ends below 512)") {
  const auto words = [](bool translate) {
    platform::host::FakeDisplay sink;
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(sink, storage.data(), storage.size());
    ui::Renderer r(builder, ui::boot_theme());
    r.use_translate(translate);
    CHECK(r.begin(ui::Color::Pane));
    ui::draw_boot_screen(r, usb::kProductString, "0123456789ab-dirty");
    CHECK(r.end());
    return sink.last_frame();
  };
  CHECK(words(true) == words(false));
}

TEST_CASE("boot screen: a display that is busy or too small gets nothing and the screen is as it was") {
  platform::host::FakeDisplay busy;
  busy.set_busy(true);
  CHECK_FALSE(ui::show_boot_screen(busy, usb::kProductString, "x"));
  CHECK(busy.frames() == 0);
  CHECK(busy.aborted() == 0);
  platform::host::FakeDisplay small(40);  // the frame does not fit: aborted, nothing committed
  CHECK_FALSE(ui::show_boot_screen(small, usb::kProductString, "x"));
  CHECK(small.frames() == 0);
  platform::host::FakeDisplay fine;
  CHECK(ui::show_boot_screen(fine, usb::kProductString, "x"));  // and the same call works once the display can take it
  CHECK(fine.frames() == 1);
}
