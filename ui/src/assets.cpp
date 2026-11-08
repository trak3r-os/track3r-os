// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/ui/assets.h>
#include <o3os/ui/boot_logo.h>
#include <o3os/ui/plot/ram_g_map.h>

#include <o3os/core/inflate.h>

namespace o3os::ui {

// OS-FLASHDIET A4: the UI assets' one decoder and the face scratch. On the MK66 image the platform owns both, in
// SDRAM_STATIC (O3OS_UI_ASSETS_EXTERNAL, set by ui/CMakeLists.txt, drops these definitions; platform/mk66's
// app_main.cpp defines the two accessors); on host and the simulator these are the definitions. Main loop only,
// synchronous, never in the render or an ISR: upload_assets() at boot, face_bitmap() for the few raw readers.
#ifndef O3OS_UI_ASSETS_EXTERNAL
namespace {
core::Inflater g_asset_inflater;
alignas(4) std::uint8_t g_face_scratch[kFaceScratchBytes];
}  // namespace
core::Inflater& asset_inflater() noexcept { return g_asset_inflater; }
std::uint8_t* face_scratch() noexcept { return g_face_scratch; }
#endif

namespace {

// Inflates one whole stream (z, z_size -> raw_size bytes) into dst in a single synchronous call, through the one
// shared decoder. Main loop only. Returns false when the stream fails or does not end exactly at raw_size.
bool inflate_all(const std::uint8_t* z, std::size_t z_size, std::size_t raw_size, std::uint8_t* dst) noexcept {
  core::Inflater& inf = asset_inflater();
  inf.begin(z, z_size, raw_size);
  std::size_t got = 0;
  while (got < raw_size && !inf.done() && !inf.failed()) {
    const std::size_t n = inf.read(dst + got, raw_size - got);
    if (n == 0) {
      break;  // no progress without the stream having ended: a truncated stream
    }
    got += n;
  }
  return got == raw_size && inf.done() && !inf.failed();
}

}  // namespace

bool face_bitmap(Font font, std::uint8_t* dst, std::size_t cap) noexcept {
  const FontFace& f = font_face(font);
  if (dst == nullptr || cap < f.bitmap_bytes) {
    return false;
  }
  return inflate_all(f.z, f.z_size, f.bitmap_bytes, dst);
}

bool boot_logo_bitmap(std::uint8_t* dst, std::size_t cap) noexcept {
  const BootLogo logo = boot_logo();
  if (dst == nullptr || cap < logo.size) {
    return false;
  }
  return inflate_all(logo.z, logo.z_size, logo.size, dst);
}

namespace {

// Staging for generated tiles: composed here, then handed to Display::upload(), whose source must be word aligned
// (the device may read it with LDM/LDRD or DMA, ADR 0011). The alignment is declared on the type and asserted, and
// the chunk is a whole number of tiles of either kind below the 4 KB cap, so every chunk starts on a word of RAM_G.
// OS-FLASHDIET A4/A5: the same staging buffer carries the inflated chunks of the compressed assets (upload_stream:
// the faces and the boot logo), one upload at most 4 KB.
struct alignas(4) Stage {
  std::uint8_t bytes[kStageBytes];
};
static_assert(alignof(Stage) >= hal::kUploadAlign && sizeof(Stage) == kStageBytes,
              "the staging buffer is word aligned and has no padding");
static_assert(kStageBytes <= hal::kMaxUploadBytes && kStageBytes % hal::kUploadAlign == 0,
              "a staging chunk is one upload below the 4 KB cap, a whole number of words");
static_assert(kStageBytes % kHexTileBytes == 0 && kStageBytes % kNoteTileBytes == 0,
              "a staging chunk holds whole tiles, so each chunk starts at a tile");
static_assert(kLabelHexBytes % hal::kUploadAlign == 0 && kLabelNoteBytes % hal::kUploadAlign == 0 && kMicroNoteBytes % hal::kUploadAlign == 0,
              "a smaller tile is a whole number of words, so every chunk of a set starts on a word of RAM_G");

// Data already in flash (the icons): word aligned by the generator, sent in chunks of the cap.
hal::UploadResult upload_flash(hal::Display& display, std::uint32_t addr, const std::uint8_t* bytes,
                               std::size_t count) noexcept {
  static_assert(hal::kMaxUploadBytes % hal::kUploadAlign == 0, "a chunk keeps the word alignment");
  hal::UploadResult result = hal::UploadResult::Ok;
  for (std::size_t offset = 0; offset < count && result == hal::UploadResult::Ok; offset += hal::kMaxUploadBytes) {
    const std::size_t n = count - offset < hal::kMaxUploadBytes ? count - offset : hal::kMaxUploadBytes;
    result = display.upload(addr + static_cast<std::uint32_t>(offset), bytes + offset, n);
  }
  return result;
}

// OS-FLASHDIET A4/A5: a raw-deflate stream in flash (a face, the boot logo) is inflated through the staging chunk and
// uploaded piece by piece: the raw bytes are not in flash any more, so every chunk comes from RAM, word aligned, one
// upload below the 4 KB cap.
hal::UploadResult upload_stream(hal::Display& display, Stage& stage, std::uint32_t addr, const std::uint8_t* z,
                                std::size_t z_size, std::size_t raw_bytes) noexcept {
  core::Inflater& inf = asset_inflater();
  inf.begin(z, z_size, raw_bytes);
  hal::UploadResult result = hal::UploadResult::Ok;
  std::size_t sent = 0;
  while (sent < raw_bytes && result == hal::UploadResult::Ok) {
    const std::size_t want = raw_bytes - sent < kStageBytes ? raw_bytes - sent : kStageBytes;
    const std::size_t n = inf.read(stage.bytes, want);
    if (n == 0) {
      // A stream the build made cannot fail (the converter's self-test round-trips every asset through an independent
      // decoder): stop like a refused chunk rather than upload what is not there.
      return hal::UploadResult::NoData;
    }
    result = display.upload(addr + static_cast<std::uint32_t>(sent), stage.bytes, n);
    sent += n;
  }
  if (result == hal::UploadResult::Ok && (!inf.done() || inf.failed())) {
    return hal::UploadResult::NoData;  // the stream did not end exactly at raw_bytes
  }
  return result;
}

hal::UploadResult upload_face(hal::Display& display, Stage& stage, const FontFace& face) noexcept {
  return upload_stream(display, stage, face.ram_g_addr, face.z, face.z_size, face.bitmap_bytes);
}

// OS-ZOOM: the first kSmallTiles tiles of a smaller set, `chars_per_tile` (2 hex digits, 3 note letters) cells of the
// face `font` each, chunked through the staging buffer. `text(cell, out)` fills the characters of tile `cell`. The
// face is inflated once into the shared face scratch and composed from there.
template <typename Text>
hal::UploadResult upload_small_set(hal::Display& display, Stage& stage, Font font, std::uint32_t addr, unsigned chars_per_tile,
                                   unsigned tile_bytes, Text text, unsigned tiles = kSmallTiles) noexcept {
  const FontFace& face = font_face(font);
  if (face.bitmap_bytes > kFaceScratchBytes || !face_bitmap(font, face_scratch(), kFaceScratchBytes)) {
    return hal::UploadResult::NoData;
  }
  const std::uint8_t* bitmap = face_scratch();
  const unsigned per_chunk = static_cast<unsigned>(kStageBytes) / tile_bytes;
  hal::UploadResult result = hal::UploadResult::Ok;
  for (unsigned first = 0; first < tiles && result == hal::UploadResult::Ok; first += per_chunk) {
    const unsigned n = tiles - first < per_chunk ? tiles - first : per_chunk;
    for (unsigned t = 0; t < n; ++t) {
      char chars[4];
      text(first + t, chars);
      compose_tile(face, bitmap, chars, chars_per_tile, stage.bytes + t * tile_bytes);
    }
    result = display.upload(addr + first * tile_bytes, stage.bytes, n * tile_bytes);
  }
  return result;
}

}  // namespace

void compose_tile(const FontFace& face, const std::uint8_t* bitmap, const char* chars, unsigned count, std::uint8_t* out) noexcept {
  const unsigned width = count * face.cell_w;
  const unsigned stride = l1_stride(width);
  if (count == 0 || width > 32 || chars == nullptr || out == nullptr || bitmap == nullptr) {
    return;  // a tile is built in a 32-bit accumulator
  }
  const unsigned glyph_bytes = static_cast<unsigned>(face.stride) * face.cell_h;
  for (unsigned row = 0; row < face.cell_h; ++row) {
    std::uint32_t acc = 0;
    for (unsigned i = 0; i < count; ++i) {
      unsigned ch = static_cast<std::uint8_t>(chars[i]);
      if (ch < face.first_char || ch >= static_cast<unsigned>(face.first_char) + face.glyph_count) {
        ch = '?';
      }
      const std::uint8_t* line = bitmap + (ch - face.first_char) * glyph_bytes + row * face.stride;
      std::uint32_t bits = 0;
      for (unsigned b = 0; b < face.stride; ++b) {
        bits = (bits << 8) | line[b];
      }
      bits >>= face.stride * 8u - face.cell_w;  // the cell's pixels, right aligned
      acc = (acc << face.cell_w) | bits;
    }
    acc <<= stride * 8u - width;  // left aligned in the row's bytes, pad bits zero
    for (unsigned b = 0; b < stride; ++b) {
      out[row * stride + b] = static_cast<std::uint8_t>(acc >> ((stride - 1u - b) * 8u));
    }
  }
}

bool fx_tile_text(unsigned type, char (&out)[4]) noexcept {
  // By FX type id (model/fx_table.h; the OG ids 1..42 and the tracker FX 46..62): three characters each, unique. "" is a type without a name (None, the importer's 43..45).
  static const char kNames[kFxTiles][4] = {
      "",    "OFF", "MMV", "ROL", "CHN", "RNT", "RNI", "RNV", "CCA", "CCB", "CCC", "CCD", "CCE", "BRK", "CHD", "TMP",  // 0..15
      "RNF", "SWG", "VOL", "GLD", "GAT", "ARP", "POS", "VLF", "PNL", "SLC", "REV", "LPF", "HPF", "BPF", "DLY", "PAN",  // 16..31
      "RVB", "FNL", "MTU", "FLL", "PSL", "PRG", "OVD", "BIT", "TUN", "SLU", "SLD", "",    "",    "",    "TAR", "VSL",  // 32..47
      "FVS", "FPU", "FPD", "PNS", "VBW", "TRW", "GLS", "RTG", "TMR", "CPN", "CVL", "CVS", "GVL", "GVS", "PBR",         // 48..62
  };
  if (type >= kFxTiles || kNames[type][0] == '\0') {
    out[0] = '\0';
    return false;
  }
  for (unsigned i = 0; i < 4; ++i) {
    out[i] = kNames[type][i];
  }
  return true;
}

void note_tile_text(unsigned cell, char (&out)[4]) noexcept {
  static const char kNames[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
  out[3] = '\0';
  if (cell == 0) {
    out[0] = out[1] = out[2] = '-';
  } else if (cell <= 120) {
    const unsigned semitone = (cell - 1u) % 12u;
    out[0] = kNames[semitone * 2];
    out[1] = kNames[semitone * 2 + 1];
    out[2] = static_cast<char>('0' + (cell - 1u) / 12u);
  } else if (cell == kNoteTileOff) {
    out[0] = 'O';
    out[1] = out[2] = 'F';
  } else if (cell == kNoteTileCut) {
    out[0] = 'C';
    out[1] = 'U';
    out[2] = 'T';
  } else if (cell == kNoteTileFade) {
    out[0] = 'F';
    out[1] = 'A';
    out[2] = 'D';
  } else if (cell >= kNoteTileOctave10 && cell < kNoteTiles) {
    // Notes 121..128 are semitones 0..7 of the tenth octave: C C# D D# E F F# G; a sharp is the lowercase letter.
    static const char kLetters[8] = {'C', 'c', 'D', 'd', 'E', 'F', 'f', 'G'};
    out[0] = kLetters[cell - kNoteTileOctave10];
    out[1] = '1';
    out[2] = '0';
  } else {
    out[0] = out[1] = out[2] = '?';
  }
}

hal::UploadResult upload_assets(hal::Display& display) noexcept {
  Stage stage;
  hal::UploadResult result = hal::UploadResult::Ok;
  // The faces first: each stream is inflated through the staging chunk and uploaded as it is produced.
  for (unsigned i = 0; i < kFontCount && result == hal::UploadResult::Ok; ++i) {
    result = upload_face(display, stage, font_face(static_cast<Font>(i)));
  }
  if (result != hal::UploadResult::Ok) {
    return result;
  }
  const IconBitmap icons = icon_bitmap();
  result = upload_flash(display, kRamGIcons, icons.bytes, icons.size);
  if (result != hal::UploadResult::Ok) {
    return result;
  }
  // The boot logo (OS-003f, OS-FLASHDIET A5): inflated from its stream in flash to its own region of the RAM_G map, behind the wave slots.
  const BootLogo logo = boot_logo();
  result = upload_stream(display, stage, plot::kRamGBootLogo, logo.z, logo.z_size, logo.size);
  if (result != hal::UploadResult::Ok) {
    return result;
  }

  // The tiles, composed from the Data face inflated into the shared 4 KiB face scratch.
  const FontFace& data = font_face(kTileFont);
  if (data.bitmap_bytes > kFaceScratchBytes || !face_bitmap(kTileFont, face_scratch(), kFaceScratchBytes)) {
    return hal::UploadResult::NoData;
  }
  const std::uint8_t* data_px = face_scratch();
  // Hex-pair tiles 00..FF are consecutive in RAM_G (the two handles split them at 80): 32 tiles a chunk.
  static const char kDigits[] = "0123456789ABCDEF";
  constexpr unsigned kHexPerChunk = static_cast<unsigned>(kStageBytes / kHexTileBytes);
  for (unsigned first = 0; first < kHexTiles && result == hal::UploadResult::Ok; first += kHexPerChunk) {
    for (unsigned t = 0; t < kHexPerChunk; ++t) {
      const unsigned v = first + t;
      const char pair[2] = {kDigits[(v >> 4) & 0xFu], kDigits[v & 0xFu]};
      compose_tile(data, data_px, pair, 2, stage.bytes + t * kHexTileBytes);
    }
    result = display.upload(kRamGHexLo + first * kHexTileBytes, stage.bytes, kHexPerChunk * kHexTileBytes);
  }
  // Note tiles: 24 a chunk, the last one short.
  constexpr unsigned kNotesPerChunk = static_cast<unsigned>(kStageBytes / kNoteTileBytes);
  for (unsigned first = 0; first < kNoteTiles && result == hal::UploadResult::Ok; first += kNotesPerChunk) {
    const unsigned n = kNoteTiles - first < kNotesPerChunk ? kNoteTiles - first : kNotesPerChunk;
    for (unsigned t = 0; t < n; ++t) {
      char text[4];
      note_tile_text(first + t, text);
      compose_tile(data, data_px, text, 3, stage.bytes + t * kNoteTileBytes);
    }
    result = display.upload(kRamGNotes + first * kNoteTileBytes, stage.bytes, n * kNoteTileBytes);
  }
  // OS-ZOOM: the smaller sets of the zoomed pattern grid, composed from the Label and Micro faces (hex pairs 00..7F, note cells 0..127).
  const auto hex_text = [](unsigned v, char (&out)[4]) {
    out[0] = kDigits[(v >> 4) & 0xFu];
    out[1] = kDigits[v & 0xFu];
    out[2] = out[3] = '\0';
  };
  const auto note_text = [](unsigned cell, char (&out)[4]) { note_tile_text(cell, out); };
  if (result == hal::UploadResult::Ok) {
    result = upload_small_set(display, stage, kLabelTileFont, kRamGLabelHex, 2, kLabelHexBytes, hex_text);
  }
  if (result == hal::UploadResult::Ok) {
    result = upload_small_set(display, stage, kLabelTileFont, kRamGLabelNotes, 3, kLabelNoteBytes, note_text);
  }
  if (result == hal::UploadResult::Ok) {
    result = upload_small_set(display, stage, kMicroTileFont, kRamGMicroNotes, 3, kMicroNoteBytes, note_text);
  }
  if (result == hal::UploadResult::Ok) {  // OS-AUDIT E-PAT-03: the FX names; a type without one is composed as "???" (never drawn)
    const auto fx_text = [](unsigned type, char (&out)[4]) {
      if (!fx_tile_text(type, out)) {
        out[0] = out[1] = out[2] = '?';
        out[3] = '\0';
      }
    };
    result = upload_small_set(display, stage, kMicroTileFont, kRamGMicroFx, 3, kFxTileBytes, fx_text, kFxTiles);
  }
  return result;
}

}  // namespace o3os::ui
