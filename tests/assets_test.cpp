// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bitmap assets of the UI (ui/assets.h): tiles composed from the Data face at boot, the icon face, the RAM_G map,
// and upload_assets(), which puts all of it in RAM_G through hal::Display::upload() in word-aligned chunks of at most 4 KB.
#include <doctest/doctest.h>

#include <o3os/app/pattern_data.h>
#include <o3os/core/inflate.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/fx_table.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/boot_logo.h>
#include <o3os/ui/font.h>
#include <o3os/ui/plot/ram_g_map.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace o3os;
using platform::host::FakeDisplay;

namespace {

constexpr std::size_t kPlenty = 1u << 20;  // a recording cap that nothing here reaches

unsigned cell_of(char ch) { return static_cast<unsigned>(ch) - 32u; }

// Pixel (x, y) of a glyph of a face's inflated bitmap (ui::face_bitmap; OS-FLASHDIET A4: the faces are stored deflated).
bool glyph_bit(const std::uint8_t* bitmap, const ui::FontFace& f, char ch, unsigned x, unsigned y) {
  const std::uint8_t b = bitmap[cell_of(ch) * f.stride * f.cell_h + y * f.stride + x / 8];
  return ((b >> (7 - x % 8)) & 1u) != 0;
}
bool tile_bit(const std::uint8_t* tile, unsigned stride, unsigned x, unsigned y) {
  return ((tile[y * stride + x / 8] >> (7 - x % 8)) & 1u) != 0;
}

struct Panel {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  template <typename Fn>
  void draw(Fn fn) {
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, ui::theme(ui::ThemeId::Amiga));
    CHECK(r.begin(ui::Color::Pane));
    fn(r);
    CHECK(r.end());
    CHECK(display.render_failures() == 0u);
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
};

}  // namespace

TEST_CASE("assets: the RAM_G map is word aligned, ordered, below 1 MB, and the handles are the coprocessor-safe ones") {
  using namespace ui;
  CHECK(kTileH == 20u);
  CHECK((kHexTileW == 20u && kHexStride == 3u && kHexTileBytes == 60u));
  CHECK((kNoteTileW == 30u && kNoteStride == 4u && kNoteTileBytes == 80u));
  CHECK((kIconSize == 16u && kIconStride == 2u && kIconBytes == 32u && kIconCount == 11u));
  CHECK(kHexTiles == 2u * kHexTilesPerHandle);
  CHECK(kHexTilesPerHandle == 128u);  // VERTEX2II's cell field is 7 bits
  CHECK(kNoteTiles == 133u);
  CHECK(kNoteTilesPerHandle == 128u);  // ... and so are the note tiles': the cells past the 128th have a handle of their own
  struct Region {
    std::uint32_t addr, bytes;
  };
  std::vector<Region> regions;
  for (unsigned i = 0; i < kFontCount; ++i) {
    regions.push_back({font_face(static_cast<Font>(i)).ram_g_addr, font_face(static_cast<Font>(i)).bitmap_bytes});
  }
  std::vector<unsigned> handles;
  for (unsigned i = 0; i < kFontCount; ++i) {
    handles.push_back(font_face(static_cast<Font>(i)).handle);
  }
  const unsigned cells[kTileSetCount] = {kHexTilesPerHandle, kHexTilesPerHandle, kNoteTilesPerHandle, kIconCount};
  for (unsigned i = 0; i < kTileSetCount; ++i) {
    const BitmapSet s = bitmap_set(static_cast<Tiles>(i));
    regions.push_back({s.ram_g, cells[i] * s.stride * s.cell_h});
    handles.push_back(s.handle);
    CHECK(s.stride == l1_stride(s.cell_w));
  }
  // The second note handle, bound by the first tile that needs it: the cells after the first handle's 128, in the same layout.
  const BitmapSet hi = notes_hi_set();
  CHECK((hi.cell_w == kNoteTileW && hi.cell_h == kTileH && hi.stride == kNoteStride));
  CHECK(hi.ram_g == bitmap_set(Tiles::Notes).ram_g + kNoteTilesPerHandle * kNoteTileBytes);
  regions.push_back({hi.ram_g, (kNoteTiles - kNoteTilesPerHandle) * hi.stride * hi.cell_h});
  handles.push_back(hi.handle);
  // OS-ZOOM: the smaller sets of the zoomed pattern grid, 128 cells each; the two note sets share handle 14 (one is bound at a time).
  const SmallTiles small_sets[] = {SmallTiles::LabelHex, SmallTiles::LabelNotes, SmallTiles::MicroNotes};
  for (const SmallTiles set : small_sets) {
    const BitmapSet s = small_bitmap_set(set);
    CHECK(s.stride == l1_stride(s.cell_w));
    regions.push_back({s.ram_g, kSmallTiles * s.stride * s.cell_h});
  }
  // OS-AUDIT E-PAT-03: the FX name tiles, kFxTiles cells of the Micro note layout, on the same handle 14.
  const BitmapSet fx_set = small_bitmap_set(SmallTiles::MicroFx);
  CHECK((fx_set.handle == kHandleSmallNotes && fx_set.cell_w == kMicroNoteW && fx_set.stride == l1_stride(kMicroNoteW)));
  CHECK(kFxTiles <= kSmallTiles);  // a cell fits VERTEX2II's 7 bits
  regions.push_back({fx_set.ram_g, kFxTiles * fx_set.stride * fx_set.cell_h});
  CHECK(small_bitmap_set(SmallTiles::LabelHex).handle == kHandleSmallHex);
  CHECK(small_bitmap_set(SmallTiles::LabelNotes).handle == kHandleSmallNotes);
  CHECK(small_bitmap_set(SmallTiles::MicroNotes).handle == kHandleSmallNotes);
  handles.push_back(kHandleSmallHex);
  handles.push_back(kHandleSmallNotes);
  CHECK(handles == std::vector<unsigned>{1, 2, 3, 4, 5, 6, 7, 8, 12, 13, 14});  // 0 is left alone, 9..11 are the wave lanes and the logo, 15 is the coprocessor's
  std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.addr < b.addr; });
  for (std::size_t i = 0; i < regions.size(); ++i) {
    CHECK(regions[i].addr % hal::kUploadAlign == 0);
    CHECK(regions[i].bytes % hal::kUploadAlign == 0);
    CHECK(regions[i].addr + regions[i].bytes <= hal::kRamGBytes);
    if (i + 1 < regions.size()) {
      CHECK(regions[i].addr + regions[i].bytes <= regions[i + 1].addr);  // no two bitmaps overlap
    }
  }
  CHECK(kRamGEnd == regions.back().addr + regions.back().bytes);
  CHECK(kRamGEnd < 60u * 1024u);  // (OS-AUDIT E-PAT-03: 57 KB became 60 KB for the 63 FX name tiles, 2,268 bytes) about 56 KB in use (the faces up to 16 KB, then 26 KB of tiles and icons and 14.5 KB of the smaller tiles of the zoomed pattern grid, OS-ZOOM), of 1 MB
  // The two hex handles continue one another: a tile is at kRamGHexLo + value * 60 whichever handle draws it.
  CHECK(kRamGHexHi == kRamGHexLo + 128u * kHexTileBytes);
}

TEST_CASE("assets: every face's deflated stream inflates byte for byte to the raw bitmap (OS-FLASHDIET A4)") {
  for (unsigned i = 0; i < ui::kFontCount; ++i) {
    const ui::Font font = static_cast<ui::Font>(i);
    const ui::FontFace& f = ui::font_face(font);
    CHECK(f.bitmap_bytes == f.glyph_count * f.stride * f.cell_h);
    CHECK(f.z_size < f.bitmap_bytes);  // every face compresses
    CHECK(reinterpret_cast<std::uintptr_t>(f.z) % hal::kUploadAlign == 0);  // declared alignas(4) by the generator
    std::vector<std::uint8_t> want(f.bitmap_bytes);
    CHECK_MESSAGE(ui::face_bitmap(font, want.data(), want.size()), "face " << i << " through face_bitmap");
    CHECK_FALSE(ui::face_bitmap(font, want.data(), want.size() - 1));  // a cap below bitmap_bytes is refused
    // The same stream through a fresh core::Inflater, read in chunks of 1, 7, 64 and 4096 bytes: byte-equal.
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{4096}}) {
      core::Inflater inf;
      inf.begin(f.z, f.z_size, f.bitmap_bytes);
      std::vector<std::uint8_t> got(f.bitmap_bytes, 0xEE);
      std::size_t at = 0;
      while (at < f.bitmap_bytes && !inf.done() && !inf.failed()) {
        at += inf.read(got.data() + at, std::min(chunk, f.bitmap_bytes - at));
      }
      CHECK_MESSAGE(inf.done(), "face " << i << " chunk " << chunk << ": not done");
      CHECK_MESSAGE(!inf.failed(), "face " << i << " chunk " << chunk << ": failed");
      CHECK_MESSAGE(at == f.bitmap_bytes, "face " << i << " chunk " << chunk << ": produced " << at);
      CHECK_MESSAGE(got == want, "face " << i << " chunk " << chunk << ": the inflated bytes differ");
    }
  }
}

TEST_CASE("assets: a composed tile is the glyphs side by side, pixel for pixel") {
  const ui::FontFace& data = ui::font_face(ui::kTileFont);
  std::uint8_t face_px[ui::kFaceScratchBytes];
  CHECK(ui::face_bitmap(ui::kTileFont, face_px, sizeof face_px));
  std::uint8_t tile[ui::kNoteTileBytes];
  std::memset(tile, 0xEE, sizeof(tile));
  ui::compose_tile(data, face_px, "7F", 2, tile);
  for (unsigned y = 0; y < 20; ++y) {
    for (unsigned x = 0; x < 20; ++x) {
      CHECK(tile_bit(tile, 3, x, y) == (x < 10 ? glyph_bit(face_px, data, '7', x, y) : glyph_bit(face_px, data, 'F', x - 10, y)));
    }
    CHECK((tile[y * 3 + 2] & 0x0F) == 0);  // the 4 pad bits of the third byte are clear
  }
  ui::compose_tile(data, face_px, "C#4", 3, tile);
  for (unsigned y = 0; y < 20; ++y) {
    for (unsigned x = 0; x < 30; ++x) {
      const char ch = "C#4"[x / 10];
      CHECK(tile_bit(tile, 4, x, y) == glyph_bit(face_px, data, ch, x % 10, y));
    }
    CHECK((tile[y * 4 + 3] & 0x03) == 0);  // 2 pad bits
  }
  // One glyph, and a character the face does not have (it is '?', like text()).
  ui::compose_tile(data, face_px, "A", 1, tile);
  for (unsigned y = 0; y < 20; ++y) {
    for (unsigned x = 0; x < 10; ++x) {
      CHECK(tile_bit(tile, 2, x, y) == glyph_bit(face_px, data, 'A', x, y));
    }
  }
  ui::compose_tile(data, face_px, "\x01", 1, tile);
  for (unsigned y = 0; y < 20; ++y) {
    for (unsigned x = 0; x < 10; ++x) {
      CHECK(tile_bit(tile, 2, x, y) == glyph_bit(face_px, data, '?', x, y));
    }
  }
  // Too many cells for the accumulator, or nothing to draw: the output is left alone.
  std::memset(tile, 0x5A, sizeof(tile));
  ui::compose_tile(data, face_px, "ABCD", 4, tile);
  ui::compose_tile(data, face_px, "A", 0, tile);
  ui::compose_tile(data, face_px, nullptr, 1, tile);
  ui::compose_tile(data, nullptr, "A", 1, tile);
  for (const std::uint8_t b : tile) {
    CHECK(b == 0x5A);
  }
}

TEST_CASE("assets: the note tiles say what the pattern data says for every note") {
  char text[4];
  ui::note_tile_text(0, text);
  CHECK(std::string(text) == "---");
  ui::note_tile_text(1, text);
  CHECK(std::string(text) == "C-0");
  ui::note_tile_text(49, text);
  CHECK(std::string(text) == "C-4");
  ui::note_tile_text(120, text);
  CHECK(std::string(text) == "B-9");
  ui::note_tile_text(ui::kNoteTileOff, text);
  CHECK(std::string(text) == "OFF");
  ui::note_tile_text(ui::kNoteTileUnknown, text);
  CHECK(std::string(text) == "???");
  ui::note_tile_text(ui::kNoteTileCut, text);
  CHECK(std::string(text) == "CUT");
  ui::note_tile_text(ui::kNoteTileFade, text);
  CHECK(std::string(text) == "FAD");
  ui::note_tile_text(ui::kNoteTileOctave10, text);
  CHECK(std::string(text) == "C10");
  ui::note_tile_text(ui::kNoteTiles - 1u, text);
  CHECK(std::string(text) == "G10");
  ui::note_tile_text(5000, text);
  CHECK(std::string(text) == "???");
  // The model's note numbers (ADR 0004) map to the tile cells the pattern view uses, and the two agree on the text.
  for (unsigned note = 0; note < 256; ++note) {
    const unsigned cell = app::note_tile_cell(static_cast<std::uint8_t>(note));
    app::Step s = model::kEmptyStep;
    s.note = static_cast<std::uint8_t>(note);
    char field[4];
    app::format_field(s, app::Column::Note, field);
    ui::note_tile_text(cell, text);
    CHECK_MESSAGE(std::string(text) == field, "note " << note);
  }
}

TEST_CASE("assets: every tile drawn from RAM_G is the pixels of the text it stands for") {
  auto panel = std::make_unique<Panel>();
  // All 256 hex pairs as tiles (the left half of the panel) and as text (the right half), 14 to a row.
  panel->draw([](ui::Renderer& r) {
    for (unsigned v = 0; v < 256; ++v) {
      const char pair[3] = {"0123456789ABCDEF"[v >> 4], "0123456789ABCDEF"[v & 15], '\0'};
      const int x = static_cast<int>(v % 14) * 24, y = static_cast<int>(v / 14) * 22;
      r.hex_tile(x, y, v, ui::Color::Text);
      r.text(x + 360, y, pair, ui::Color::Text);
    }
  });
  int diffs = 0;
  for (unsigned v = 0; v < 256; ++v) {
    const int x = static_cast<int>(v % 14) * 24, y = static_cast<int>(v / 14) * 22;
    for (int dy = 0; dy < 20; ++dy) {
      for (int dx = 0; dx < 20; ++dx) {
        diffs += panel->pixel(x + dx, y + dy) != panel->pixel(x + 360 + dx, y + dy) ? 1 : 0;
      }
    }
  }
  CHECK(diffs == 0);
  // All 133 note tiles, the same way (the last five are drawn with the second handle, which the first of them binds).
  panel->draw([](ui::Renderer& r) {
    for (unsigned c = 0; c < ui::kNoteTiles; ++c) {
      char text[4];
      ui::note_tile_text(c, text);
      const int x = static_cast<int>(c % 8) * 34, y = static_cast<int>(c / 8) * 22;
      r.note_tile(x, y, c, ui::Color::Text);
      r.text(x + 300, y, text, ui::Color::Text);
    }
  });
  diffs = 0;
  for (unsigned c = 0; c < ui::kNoteTiles; ++c) {
    const int x = static_cast<int>(c % 8) * 34, y = static_cast<int>(c / 8) * 22;
    for (int dy = 0; dy < 20; ++dy) {
      for (int dx = 0; dx < 30; ++dx) {
        diffs += panel->pixel(x + dx, y + dy) != panel->pixel(x + 300 + dx, y + dy) ? 1 : 0;
      }
    }
  }
  CHECK(diffs == 0);
}

TEST_CASE("assets: the icon face has eleven icons of ink, word aligned, with hand-checked cells") {
  const ui::IconBitmap icons = ui::icon_bitmap();
  CHECK(icons.size == ui::kIconCount * ui::kIconBytes);
  CHECK(reinterpret_cast<std::uintptr_t>(icons.bytes) % hal::kUploadAlign == 0);
  for (unsigned i = 0; i < ui::kIconCount; ++i) {
    unsigned ink = 0;
    for (unsigned b = 0; b < ui::kIconBytes; ++b) {
      ink += static_cast<unsigned>(__builtin_popcount(icons.bytes[i * ui::kIconBytes + b]));
    }
    CHECK_MESSAGE((ink > 20 && ink < 190), "icon " << i << " has " << ink << " px");
  }
  const std::uint8_t* stop = icons.bytes + static_cast<unsigned>(ui::Icon::Stop) * ui::kIconBytes;
  CHECK((stop[0] == 0 && stop[6] == 0x1F && stop[7] == 0xF8 && stop[24] == 0x1F && stop[26] == 0));  // rows 3..12, cols 3..12
  const std::uint8_t* play = icons.bytes + static_cast<unsigned>(ui::Icon::Play) * ui::kIconBytes;
  CHECK((play[4] == 0x10 && play[5] == 0x00 && play[14] == 0x1F && play[15] == 0xFC));  // the apex, and the widest rows
}

TEST_CASE("assets: upload_assets sends every bitmap in aligned chunks of at most 4 KB, and nothing twice") {
  FakeDisplay fake;
  fake.record_uploads(kPlenty);
  CHECK(ui::upload_assets(fake) == hal::UploadResult::Ok);
  CHECK(fake.rejected_uploads() == 0u);
  struct Chunk {
    std::uint32_t addr, size;
  };
  std::vector<Chunk> chunks;
  std::size_t total = 0;
  for (const auto& u : fake.uploads()) {
    CHECK(u.bytes.size() <= hal::kMaxUploadBytes);
    CHECK(u.addr % hal::kUploadAlign == 0);
    chunks.push_back({u.addr, static_cast<std::uint32_t>(u.bytes.size())});
    total += u.bytes.size();
  }
  // The faces (12,540 bytes), the icons (352), the boot logo (7,688: OS-003f), 256 hex tiles (15,360), 133 note tiles (10,640) and the smaller tiles of the zoomed grid
  // (OS-ZOOM: 128 Label hex pairs of 32 bytes, 128 Label note tiles of 48, 128 Micro note tiles of 36) and the 63 FX name tiles (OS-AUDIT E-PAT-03: Micro, 36 bytes).
  CHECK(total == 12540u + 352u + 7688u + 15360u + 10640u + 4096u + 6144u + 4608u + 2268u);
  CHECK(ui::kBootLogoBytes == 7688u);
  CHECK(fake.uploaded_bytes() == total);
  std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) { return a.addr < b.addr; });
  for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
    CHECK(chunks[i].addr + chunks[i].size <= chunks[i + 1].addr);  // no byte is sent twice
  }
  // A display that cannot take it says why, and the first chunk is where it stopped.
  FakeDisplay busy;
  busy.set_busy(true);
  CHECK(ui::upload_assets(busy) == hal::UploadResult::Busy);
  CHECK(busy.accepted_uploads() == 0u);
}

TEST_CASE("assets: on the simulator's display everything lands in RAM_G where the renderer's handles point") {
  auto raster = std::make_unique<sim::EveRaster>();
  sim::RasterDisplay display(*raster);
  CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
  // OS-FLASHDIET A4: the RAM_G bytes after upload_assets equal the old raw faces, byte for byte: each face inflated
  // through face_bitmap (the streams the generator made of the very bitmaps it used to emit raw).
  std::uint8_t face_px[ui::kFaceScratchBytes];
  for (unsigned i = 0; i < ui::kFontCount; ++i) {
    const ui::Font font = static_cast<ui::Font>(i);
    const ui::FontFace& f = ui::font_face(font);
    std::vector<std::uint8_t> px(f.bitmap_bytes);
    CHECK_MESSAGE(ui::face_bitmap(font, px.data(), px.size()), "face " << i);
    CHECK(std::memcmp(raster->ram_g() + f.ram_g_addr, px.data(), f.bitmap_bytes) == 0);
  }
  const ui::FontFace& data = ui::font_face(ui::kTileFont);
  CHECK(ui::face_bitmap(ui::kTileFont, face_px, sizeof face_px));
  const ui::IconBitmap icons = ui::icon_bitmap();
  CHECK(std::memcmp(raster->ram_g() + ui::kRamGIcons, icons.bytes, icons.size) == 0);
  // OS-003f / OS-FLASHDIET A5: in its own region of the RAM_G map, behind the wave slots, inflated from its stream.
  std::uint8_t logo_px[ui::kBootLogoBytes];
  CHECK(ui::boot_logo_bitmap(logo_px, sizeof logo_px));
  CHECK(std::memcmp(raster->ram_g() + ui::plot::kRamGBootLogo, logo_px, ui::kBootLogoBytes) == 0);
  // Hex tile 0xA5 is in the second handle's range, at the same offset from the start of the first.
  std::uint8_t want[ui::kHexTileBytes];
  ui::compose_tile(data, face_px, "A5", 2, want);
  CHECK(std::memcmp(raster->ram_g() + ui::kRamGHexLo + 0xA5u * ui::kHexTileBytes, want, sizeof(want)) == 0);
  CHECK(std::memcmp(raster->ram_g() + ui::kRamGHexHi + (0xA5u - 128u) * ui::kHexTileBytes, want, sizeof(want)) == 0);
  // Every note tile, the ones the second handle draws included, is where its handle points (they continue one another in RAM_G).
  for (unsigned c = 0; c < ui::kNoteTiles; ++c) {
    char text[4];
    ui::note_tile_text(c, text);
    std::uint8_t note[ui::kNoteTileBytes];
    ui::compose_tile(data, face_px, text, 3, note);
    const std::uint32_t at = c < ui::kNoteTilesPerHandle ? ui::bitmap_set(ui::Tiles::Notes).ram_g + c * ui::kNoteTileBytes
                                                         : ui::notes_hi_set().ram_g + (c - ui::kNoteTilesPerHandle) * ui::kNoteTileBytes;
    CHECK_MESSAGE(std::memcmp(raster->ram_g() + at, note, sizeof(note)) == 0, "note tile " << c);
  }
  // The FX name tiles (OS-AUDIT E-PAT-03): the Micro face's three letters, where the MicroFx set points; a type without a name is composed as "???".
  const ui::FontFace& micro = ui::font_face(ui::kMicroTileFont);
  CHECK(ui::face_bitmap(ui::kMicroTileFont, face_px, sizeof face_px));
  for (unsigned t = 0; t < ui::kFxTiles; ++t) {
    char text[4];
    if (!ui::fx_tile_text(t, text)) {
      std::strcpy(text, "???");
    }
    std::uint8_t tile[ui::kFxTileBytes];
    ui::compose_tile(micro, face_px, text, 3, tile);
    CHECK_MESSAGE(std::memcmp(raster->ram_g() + ui::kRamGMicroFx + t * ui::kFxTileBytes, tile, sizeof(tile)) == 0, "FX name tile " << t);
  }
  CHECK(raster->ram_g()[ui::kRamGEnd] == 0);  // the one after the end is not touched: zero
}

TEST_CASE("assets: the FX names are three capitals or digits, one per type, and exist for exactly the types the table knows (OS-AUDIT E-PAT-03)") {
  std::vector<std::string> seen;
  for (unsigned t = 0; t < ui::kFxTiles; ++t) {
    char name[4];
    const bool named = ui::fx_tile_text(t, name);
    const bool core = model::fx::is_core(static_cast<std::uint8_t>(t));
    const bool expect = t >= 1 && (t < model::fx::kTypeCount || core);  // None and the importer's 43..45 (not in the table) have none
    CHECK_MESSAGE(named == expect, "type " << t);
    if (named) {
      CHECK(std::strlen(name) == 3u);
      for (int i = 0; i < 3; ++i) {
        CHECK_MESSAGE(((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= '0' && name[i] <= '9')), "type " << t << " char " << i);
      }
      CHECK_MESSAGE(std::find(seen.begin(), seen.end(), name) == seen.end(), "type " << t << " repeats " << name);
      seen.push_back(name);
    }
  }
  CHECK(ui::kFxTiles == model::fx::kCoreFirst + model::fx::kCoreCount);  // the last tile is the last core FX
  char name[4];
  CHECK_FALSE(ui::fx_tile_text(ui::kFxTiles, name));
  CHECK_FALSE(ui::fx_tile_text(200, name));
}
