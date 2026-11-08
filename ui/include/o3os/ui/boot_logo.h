// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The boot logo's bitmap (OS-003f): the maintainer's pixel-art floppy disk with the Tracker on it, as an FT81x bitmap in flash.
// tools/logo_convert.py turns ui/assets/boot_logo.png (the maintainer's logo.png, copied byte for byte: the one source) into
// src/boot_logo_data.cpp, which is committed; the ctest `logo_convert_fresh` regenerates it and fails when it differs, so the
// two cannot drift. The constants below are what the draw code relies on; the generated file static_asserts that they are the
// ones it was made with, so a different picture (another size or format) fails the build until this header says the same.
//
// The picture is a greyscale checkerboard dither: its pixels are alpha (L4: 16 levels), drawn white-on-black through COLOR_RGB.
// It is shown scaled by the integer kBootLogoScale with BITMAP_TRANSFORM and nearest sampling (ui/boot_screen.h); its place in
// RAM_G is in ui/plot/ram_g_map.h. 124 x 124 L4 is 7,688 raw bytes (the target was 8 KB).
// OS-FLASHDIET A5: in flash the bitmap is a raw-deflate stream of those 7,688 bytes (tools/logo_convert.py deflates them with
// tools/deflate_pack.py, the 4 KiB window, never Python's zlib); the boot upload inflates it like a glyph face through the one
// shared core::Inflater (ui/src/assets.cpp), and boot_logo_bitmap() inflates it for the few raw readers (the tests).

#include <o3os/eve/dl.h>

#include <cstddef>
#include <cstdint>

namespace o3os::ui {

inline constexpr unsigned kBootLogoW = 124;  // native pixels
inline constexpr unsigned kBootLogoH = 124;
inline constexpr eve::BitmapFormat kBootLogoFormat = eve::BitmapFormat::L4;
inline constexpr unsigned kBootLogoStride = 62;   // bytes a row: two L4 pixels a byte, the first in the high nibble
inline constexpr unsigned kBootLogoBytes = 7688;  // stride x height, a whole number of words
inline constexpr unsigned kBootLogoScale = 3;     // shown at 372 x 372 on the 800 x 480 panel

static_assert(kBootLogoStride * kBootLogoH == kBootLogoBytes, "the bitmap is its rows");
static_assert(kBootLogoBytes % 4 == 0, "the bitmap is uploaded as whole words");
static_assert(kBootLogoH * kBootLogoScale >= 360 && kBootLogoH * kBootLogoScale <= 420, "about 360-420 px tall (spec 003f)");
static_assert(kBootLogoW * kBootLogoScale < 512 && kBootLogoH * kBootLogoScale < 512,
              "BITMAP_SIZE's 9-bit fields hold the drawn size (the _H command is not used)");

struct BootLogo {
  const std::uint8_t* z;  // the raw-deflate stream of the bitmap's `size` bytes, word aligned (the generated array is alignas(4))
  std::uint16_t z_size;   // the stream's bytes
  std::uint16_t size;     // the bitmap's raw bytes: kBootLogoBytes, what the upload puts in RAM_G
};
// Defined by the committed, generated boot_logo_data.cpp.
BootLogo boot_logo() noexcept;

// Inflates the logo's bitmap into dst (cap at least kBootLogoBytes) and returns true; false when it does not fit or the
// stream fails. For the few readers of the raw pixels (the tests): main loop only, through the one shared decoder
// (assets.h asset_inflater()), synchronously in one call.
bool boot_logo_bitmap(std::uint8_t* dst, std::size_t cap) noexcept;

}  // namespace o3os::ui
