// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The bitmaps the UI draws from RAM_G: the four glyph faces (font.h), the tile faces for the pattern grid (ADR 0010
// section 5) and the icon face (section 4), with their RAM_G map and bitmap handles, and the one function that puts
// them there at boot. Nothing here is drawn by the coprocessor.
//
// RAM_G map (1 MB; about 53 KB used here; ui/plot/ram_g_map.h owns the whole map and adds the wave slots after these). Every address and size is a multiple of 4 so every upload keeps the word
// alignment hal::Display::upload() asks for, and the BITMAP_SOURCEs are aligned for the chip:
//
//   0x0000  the four glyph faces, packed by tools/font_convert.py (12,540 raw bytes, up to kRamGTiles; in flash they are deflated streams, OS-FLASHDIET A4)
//   0x4000  hex-pair tiles 00..7F      128 x 60 B   handle 5   20 x 20, L1, stride 3
//           hex-pair tiles 80..FF      128 x 60 B   handle 6   (VERTEX2II's cell field is 7 bits: two handles)
//           note tiles 0..127          128 x 80 B   handle 7   30 x 20, L1, stride 4
//           note tiles 128..132          5 x 80 B   handle 12  (a second handle past VERTEX2II's 7-bit cell; bound when first drawn)
//           icons                       11 x 32 B   handle 8   16 x 16, L1, stride 2
//           Label hex pairs 00..7F     128 x 32 B   handle 13  16 x 16, L1, stride 2   } the zoomed pattern grid (OS-ZOOM); composed from the Label (8x16) and Micro (6x12) faces,
//           Label note tiles 0..127    128 x 48 B   handle 14  24 x 16, L1, stride 3   } bound by the renderer in the frame that first draws one of them (4 words a set), so
//           Micro note tiles 0..127    128 x 36 B   handle 14  18 x 12, L1, stride 3   } a frame of the default level is unchanged. One of the two note sets is bound at a time.
//           Micro FX name tiles 0..62   63 x 36 B   handle 14  18 x 12, L1, stride 3   (OS-AUDIT E-PAT-03: the 3-letter FX names of the 5-track grid; a third set on handle 14, which that grid does not otherwise use)
//
// Tiles are not stored in flash: they are composed from the Data face at boot (compose_tile) and uploaded in chunks of
// kStageBytes from an aligned staging buffer, so a step of the pattern grid is one display-list word per field.
// Every bitmap that goes through Display::upload() is `alignas(4)` with a static_assert (the generated font and icon
// data say so in their source; the staging buffer in assets.cpp does too).
//
// OS-FLASHDIET A4: the glyph faces are stored in flash as raw-deflate streams (font.h) and are inflated at boot. The
// one decoder the UI assets share (the help pages will use the same one, A6/A7) and the 4 KiB face scratch the tiles
// are composed from live in SDRAM_STATIC on the MK66 image, where the platform defines the two accessors below
// (ui/CMakeLists.txt sets O3OS_UI_ASSETS_EXTERNAL for that image, which drops ui's own copies in assets.cpp); on host
// and the simulator ui defines them itself. Both are main-loop-only, synchronous, never in the render or an ISR.

#include <o3os/core/inflate.h>
#include <o3os/hal/display.h>
#include <o3os/ui/font.h>

#include <cstddef>
#include <cstdint>

namespace o3os::ui {

// FT81x bitmap handles 0..14 may be used by us; the coprocessor owns 15 (and the ROM fonts above it). The glyph faces
// take 1..4 (font_convert.py: 1 + the position in ui::Font); handle 0 is left alone.
inline constexpr std::uint8_t kHandleHexLo = 5;
inline constexpr std::uint8_t kHandleHexHi = 6;
inline constexpr std::uint8_t kHandleNotes = 7;
inline constexpr std::uint8_t kHandleIcons = 8;
// The note tiles past the 128th: Renderer::note_tile() binds this handle in the frame that first draws one (4 words), not in every frame.
// 9..11 belong to the wave lanes and the boot logo (plot/ram_g_map.h).
inline constexpr std::uint8_t kHandleNotesHi = 12;
// OS-ZOOM: the smaller tiles of the zoomed pattern grid. Bound in the frame that first draws one (Renderer::hex_tile / note_tile with a Label or Micro face), not by begin().
// 13 and 14 are the last two free handles; the volume and instrument pairs of a smaller grid are all below 0x80 (the volume's range is 0..127, the instrument's BCD at most 0x64), so one handle holds them.
inline constexpr std::uint8_t kHandleSmallHex = 13;
inline constexpr std::uint8_t kHandleSmallNotes = 14;

// Where the tile region starts; the glyph faces must end below it (the generated font_data.cpp asserts it).
inline constexpr std::uint32_t kRamGTiles = 0x4000;

// The tiles are built from the Data face, which is the 10x20 face in every built-in theme.
inline constexpr Font kTileFont = Font::F10x20;
inline constexpr FontGeometry kTileGeometry = font_geometry(kTileFont);

constexpr unsigned l1_stride(unsigned width_px) noexcept { return (width_px + 7u) / 8u; }

inline constexpr unsigned kTileH = kTileGeometry.cell_h;                  // 20
inline constexpr unsigned kHexTileW = 2u * kTileGeometry.cell_w;           // 20: two hex digits
inline constexpr unsigned kNoteTileW = 3u * kTileGeometry.cell_w;          // 30: "C-4", "OFF", "---"
inline constexpr unsigned kHexStride = l1_stride(kHexTileW);               // 3
inline constexpr unsigned kNoteStride = l1_stride(kNoteTileW);             // 4
inline constexpr unsigned kHexTileBytes = kHexStride * kTileH;             // 60
inline constexpr unsigned kNoteTileBytes = kNoteStride * kTileH;           // 80

inline constexpr unsigned kHexTiles = 256;                                 // 00..FF
inline constexpr unsigned kHexTilesPerHandle = 128;                        // VERTEX2II / CELL: 7 bits
// Note tile cells: 0 "---" (no note), 1..120 C-0..B-9 (the model's note numbers, ADR 0004), 121 "OFF", 122 "???", 123 "CUT", 124 "FAD",
// 125..132 the notes 121..128 (C10..G10: cell = note + 4). Cells 0..127 are drawn with handle 7, 128..132 with kHandleNotesHi.
inline constexpr unsigned kNoteTiles = 133;
inline constexpr unsigned kNoteTilesPerHandle = 128;                       // VERTEX2II / CELL: 7 bits
inline constexpr unsigned kNoteTileOff = 121;
inline constexpr unsigned kNoteTileUnknown = 122;
inline constexpr unsigned kNoteTileCut = 123;
inline constexpr unsigned kNoteTileFade = 124;
inline constexpr unsigned kNoteTileOctave10 = 125;                         // note 121 (C10); the 8 notes of the tenth octave follow

// OS-ZOOM: the tiles of the smaller faces, the first 128 cells only (hex pairs 00..7F, note cells 0..127: C10 and above, rare, are drawn as glyphs).
inline constexpr unsigned kSmallTiles = 128;
inline constexpr Font kLabelTileFont = Font::F8x16;
inline constexpr Font kMicroTileFont = Font::F6x12;
inline constexpr FontGeometry kLabelTileGeometry = font_geometry(kLabelTileFont);
inline constexpr FontGeometry kMicroTileGeometry = font_geometry(kMicroTileFont);
inline constexpr unsigned kLabelHexW = 2u * kLabelTileGeometry.cell_w;          // 16
inline constexpr unsigned kLabelNoteW = 3u * kLabelTileGeometry.cell_w;         // 24
inline constexpr unsigned kMicroNoteW = 3u * kMicroTileGeometry.cell_w;         // 18
inline constexpr unsigned kLabelHexBytes = l1_stride(kLabelHexW) * kLabelTileGeometry.cell_h;    // 32
inline constexpr unsigned kLabelNoteBytes = l1_stride(kLabelNoteW) * kLabelTileGeometry.cell_h;  // 48
inline constexpr unsigned kMicroNoteBytes = l1_stride(kMicroNoteW) * kMicroTileGeometry.cell_h;  // 36
// OS-AUDIT E-PAT-03 (new Community OS behaviour): the 3-letter name of an FX type, one Micro tile (18 x 12) a type; the cell is the type id (0..62: the OG ids and the tracker FX, model/fx_table.h). A type without a name
// (None, 43..45, an engine type) has no tile and is drawn as its glyph (fx_tile_text).
inline constexpr unsigned kFxTiles = 63;
inline constexpr unsigned kFxTileBytes = kMicroNoteBytes;  // 36

enum class Icon : std::uint8_t {
  Play,
  Stop,
  Record,
  Loop,
  Mute,
  Solo,
  Lock,
  Folder,
  Module,
  Midi,
  Warning,
};
inline constexpr unsigned kIconCount = 11;
inline constexpr unsigned kIconSize = 16;                                  // 16 x 16, L1
inline constexpr unsigned kIconStride = l1_stride(kIconSize);              // 2
inline constexpr unsigned kIconBytes = kIconStride * kIconSize;            // 32

inline constexpr std::uint32_t kRamGHexLo = kRamGTiles;
inline constexpr std::uint32_t kRamGHexHi = kRamGHexLo + kHexTilesPerHandle * kHexTileBytes;
inline constexpr std::uint32_t kRamGNotes = kRamGHexHi + kHexTilesPerHandle * kHexTileBytes;
inline constexpr std::uint32_t kRamGIcons = kRamGNotes + kNoteTiles * kNoteTileBytes;
inline constexpr std::uint32_t kRamGLabelHex = kRamGIcons + kIconCount * kIconBytes;
inline constexpr std::uint32_t kRamGLabelNotes = kRamGLabelHex + kSmallTiles * kLabelHexBytes;
inline constexpr std::uint32_t kRamGMicroNotes = kRamGLabelNotes + kSmallTiles * kLabelNoteBytes;
inline constexpr std::uint32_t kRamGMicroFx = kRamGMicroNotes + kSmallTiles * kMicroNoteBytes;
inline constexpr std::uint32_t kRamGEnd = kRamGMicroFx + kFxTiles * kFxTileBytes;
static_assert(kRamGHexLo % hal::kUploadAlign == 0 && kRamGHexHi % hal::kUploadAlign == 0 &&
                  kRamGNotes % hal::kUploadAlign == 0 && kRamGIcons % hal::kUploadAlign == 0 &&
                  kRamGLabelHex % hal::kUploadAlign == 0 && kRamGLabelNotes % hal::kUploadAlign == 0 &&
                  kRamGMicroNotes % hal::kUploadAlign == 0 && kRamGMicroFx % hal::kUploadAlign == 0 && kRamGEnd % hal::kUploadAlign == 0,
              "every region of RAM_G starts on a word: an upload keeps its alignment");
static_assert(kRamGEnd <= hal::kRamGBytes, "the assets fit RAM_G");
static_assert(kNoteTiles > kNoteTilesPerHandle && kNoteTiles <= 2 * kNoteTilesPerHandle && kHandleNotesHi > kHandleIcons && kHandleNotesHi <= 14,
              "the note tiles use two handles, the second one below the coprocessor's 15 (plot/ram_g_map.h keeps it clear of the wave lanes and the logo)");
static_assert(kHandleHexLo > 4 && kHandleIcons <= 14 && kHandleHexLo < kHandleHexHi && kHandleHexHi < kHandleNotes &&
                  kHandleNotes < kHandleIcons,
              "tile handles follow the four face handles and stay below the coprocessor's 15");

// One L1 bitmap handle that the renderer binds at the start of every frame (BITMAP_HANDLE, _SOURCE, _LAYOUT, _SIZE).
struct BitmapSet {
  std::uint32_t ram_g;
  std::uint8_t handle;
  std::uint8_t cell_w;
  std::uint8_t cell_h;
  std::uint8_t stride;
};

enum class Tiles : std::uint8_t { HexLo, HexHi, Notes, Icons };
// The smaller sets (OS-ZOOM), bound on first use: the Label face's hex pairs and note tiles, the Micro face's note tiles.
enum class SmallTiles : std::uint8_t { LabelHex, LabelNotes, MicroNotes, MicroFx };
inline constexpr unsigned kTileSetCount = 4;  // the sets Renderer::begin() binds; notes_hi_set() is bound on first use

// The note tiles 128..132: the same layout as Tiles::Notes, directly after its 128 cells in RAM_G.
constexpr BitmapSet notes_hi_set() noexcept {
  return BitmapSet{kRamGNotes + kNoteTilesPerHandle * kNoteTileBytes, kHandleNotesHi, kNoteTileW, kTileH, kNoteStride};
}

constexpr BitmapSet bitmap_set(Tiles set) noexcept {
  switch (set) {
    case Tiles::HexLo:
      return BitmapSet{kRamGHexLo, kHandleHexLo, kHexTileW, kTileH, kHexStride};
    case Tiles::HexHi:
      return BitmapSet{kRamGHexHi, kHandleHexHi, kHexTileW, kTileH, kHexStride};
    case Tiles::Notes:
      return BitmapSet{kRamGNotes, kHandleNotes, kNoteTileW, kTileH, kNoteStride};
    case Tiles::Icons:
      return BitmapSet{kRamGIcons, kHandleIcons, kIconSize, kIconSize, kIconStride};
  }
  return BitmapSet{0, 0, 0, 0, 0};
}

constexpr BitmapSet small_bitmap_set(SmallTiles set) noexcept {
  switch (set) {
    case SmallTiles::LabelHex:
      return BitmapSet{kRamGLabelHex, kHandleSmallHex, kLabelHexW, kLabelTileGeometry.cell_h, l1_stride(kLabelHexW)};
    case SmallTiles::LabelNotes:
      return BitmapSet{kRamGLabelNotes, kHandleSmallNotes, kLabelNoteW, kLabelTileGeometry.cell_h, l1_stride(kLabelNoteW)};
    case SmallTiles::MicroNotes:
      return BitmapSet{kRamGMicroNotes, kHandleSmallNotes, kMicroNoteW, kMicroTileGeometry.cell_h, l1_stride(kMicroNoteW)};
    case SmallTiles::MicroFx:
      return BitmapSet{kRamGMicroFx, kHandleSmallNotes, kMicroNoteW, kMicroTileGeometry.cell_h, l1_stride(kMicroNoteW)};
  }
  return BitmapSet{0, 0, 0, 0, 0};
}

// The icon face as converted by tools/icon_convert.py: kIconCount cells of kIconBytes, word aligned (generated).
struct IconBitmap {
  const std::uint8_t* bytes;
  std::uint16_t size;
};
IconBitmap icon_bitmap() noexcept;

// Composes `count` (1..3) cells of `face` side by side into one tile of count * cell_w pixels: the same pixels the
// glyphs would have drawn one by one. `bitmap` is the face's inflated pixels (face_bitmap), cell n at
// bitmap + n * stride * cell_h. Output: l1_stride(count * cell_w) * cell_h bytes, most significant bit leftmost,
// the pad bits zero. A character outside the face is '?'.
void compose_tile(const FontFace& face, const std::uint8_t* bitmap, const char* chars, unsigned count, std::uint8_t* out) noexcept;

// What note tile `cell` says: "---", "C-0".."B-9", "OFF", "???", "CUT", "FAD", then C10..G10 (cells beyond kNoteTiles are "???"). Octave 10 is
// spelled in full where it fits the three characters of a tile (C10 D10 E10 F10 G10); its sharps are four characters there
// (C#10), so they take a lowercase letter: c10 d10 f10.
void note_tile_text(unsigned cell, char (&out)[4]) noexcept;

// The 3-letter name of FX type `type` ("CHD", "ARP", ...: this OS's own, one per type) in `out`, and true; false (out is "") for a type without a tile (kFxTiles).
bool fx_tile_text(unsigned type, char (&out)[4]) noexcept;

// Uploads every glyph face, the icon face and the generated tiles to RAM_G (main loop, at boot), through
// hal::Display::upload() in chunks of at most kStageBytes from a word-aligned staging buffer on the caller's stack
// (1.9 KB). OS-FLASHDIET A4: a face's stream is inflated chunk by chunk through that same staging buffer, so every
// upload is still at most 4 KB and word aligned; the tiles are composed from the face scratch below. Stops at the
// first chunk the display refuses and returns its result; running it again from the start is harmless (Busy: retry
// later). Ok when everything is in place.
hal::UploadResult upload_assets(hal::Display& display) noexcept;

// The size of one staging chunk of upload_assets(): 32 hex tiles or 24 note tiles, below the 4 KB cap.
inline constexpr std::size_t kStageBytes = 1920;

// The one raw-deflate decoder the UI assets share, and the 4 KiB scratch a face is inflated into before its tiles are
// composed from it (the 10x20 face, 3,800 B, is the largest the tiles use). Main loop only. Defined by the platform
// in SDRAM_STATIC on the MK66 image (O3OS_UI_ASSETS_EXTERNAL), by assets.cpp on host and the simulator.
core::Inflater& asset_inflater() noexcept;
std::uint8_t* face_scratch() noexcept;
inline constexpr std::size_t kFaceScratchBytes = 4096;

}  // namespace o3os::ui
