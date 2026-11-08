// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::ui {

// The type scale (ADR 0010 section 3, docs/ui/style-guide.md): four roles, all L1 pixel faces at native size. Text is
// drawn as glyph bitmaps from RAM_G; there are no coprocessor fonts and no CMD_TEXT.
//
//   Display  16x32  BPM, pattern number, the value being edited
//   Data     10x20  the grid, lists, parameters           (cap height 13 px)
//   Label     8x16  labels                                (cap height 10 px)
//   Micro     6x12  axis ticks only
//
// A theme says which compiled face plays each role (Theme::face). The compiled faces are named by cell size, never
// by the fonts they were converted from: Terminus (OFL) for 16x32 and 10x20, Spleen (BSD-2-Clause) for 8x16 and 6x12.
enum class Font : std::uint8_t {
  F16x32,
  F10x20,
  F8x16,
  F6x12,
};
inline constexpr unsigned kFontCount = 4;

enum class Face : std::uint8_t {
  Display,
  Data,
  Label,
  Micro,
};
inline constexpr unsigned kFaceCount = 4;

// What a layout needs of a face at compile time. The generated font_data.cpp asserts that these are the geometry of
// the converted BDFs, so a face cannot change under a layout.
struct FontGeometry {
  std::uint8_t cell_w;  // also the advance: the faces are monospace
  std::uint8_t cell_h;
  std::uint8_t ascent;  // baseline row from the top of the cell: the row below the last row of a capital
  std::uint8_t cap_h;   // rows of a capital letter, counted from `ascent - cap_h`
};

constexpr FontGeometry font_geometry(Font font) noexcept {
  switch (font) {
    case Font::F16x32:
      return FontGeometry{16, 32, 26, 20};
    case Font::F10x20:
      return FontGeometry{10, 20, 16, 13};
    case Font::F8x16:
      return FontGeometry{8, 16, 12, 10};
    case Font::F6x12:
      return FontGeometry{6, 12, 9, 8};
  }
  return FontGeometry{0, 0, 0, 0};
}

// One face: a contiguous L1 bitmap of `glyph_count` cells, cell n = character first_char + n.
//   * L1: 1 bit per pixel, most significant bit = leftmost pixel, `stride` bytes per row, `cell_h` rows;
//     cell n starts at BITMAP_SOURCE + n * stride * cell_h (FT81x Programmer's Guide section 4.20).
//   * The platform places the inflated bitmap at `ram_g_addr` in RAM_G before the first frame with ui::upload_assets()
//     (assets.h), in chunks of at most 4 KB; the renderer then binds it to bitmap handle `handle` at the start of every frame.
// OS-FLASHDIET A4: the bitmap is stored in flash as `z`, a raw-deflate stream of the L1 bytes (made at build time by
// tools/deflate_pack.py with the 4 KiB window). The table lives in .rodata; the streams are declared word aligned (the
// generator emits the alignas and a static_assert, ADR 0011). The raw pixels are read with face_bitmap(); the upload
// inflates them.
struct FontFace {
  const std::uint8_t* z;       // the raw-deflate stream of the bitmap, word aligned (the generator emits the alignas and a static_assert, ADR 0011)
  std::uint16_t z_size;       // the stream's bytes
  std::uint16_t bitmap_bytes;  // the bitmap's raw bytes: stride * cell_h * glyph_count
  std::uint32_t ram_g_addr;   // 4-byte aligned
  std::uint8_t handle;        // FT81x bitmap handle, 1..4 (assets.h)
  std::uint8_t cell_w;
  std::uint8_t cell_h;
  std::uint8_t stride;
  std::uint8_t first_char;
  std::uint8_t glyph_count;
  std::uint8_t ascent;  // baseline row from the top of the cell
  std::uint8_t cap_h;
};

// Defined by the generated font_data.cpp (build tree), not by a hand-written source.
const FontFace& font_face(Font font) noexcept;

// Inflates a face's bitmap into dst (cap at least bitmap_bytes) and returns true; false when it does not fit or the
// stream fails. For the few readers of the raw pixels (the tile composer in assets.cpp, the simulator's gallery,
// tests): main loop only, through the one shared decoder (assets.h asset_inflater()), synchronously in one call.
bool face_bitmap(Font font, std::uint8_t* dst, std::size_t cap) noexcept;

}  // namespace o3os::ui
