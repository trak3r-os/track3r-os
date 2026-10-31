// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The RAM_G map (ADR 0010 amendment 2): the one header that owns every fixed region of the FT81x's 1 MiB graphics
// memory. ui/assets.h places the faces, tiles and icons (OS-003b); this file adds the wave bitmaps (OS-003d) and names
// what is left as spare, and proves with static_asserts that nothing overlaps, that every region starts on a word
// (hal::Display::upload() asks for it, ADR 0011: alignment is declared, never placed) and that all of it fits.
//
//   0x00000  faces                       kRamGTiles bytes                                  (assets.h)
//   0x04000  hex pairs, notes, icons, the smaller tiles of the zoomed grid   kRamGEnd - kRamGTiles bytes   (assets.h)
//   0x0F000  wave slots 0..3             4 x 96 KiB: 2 lanes x 2 buffers, 800 x 240 at L4 (this file)
//   0x6F000  boot logo                   2 pages: 124 x 124 L4, 7,688 bytes, uploaded with the other assets (OS-003f)
//   0x71000  note map                    255 pictures of 384 B (a pattern's ticks, 128 steps x 24 rows at L1), 96 KiB: the Song screen's TIMELINE page (OS-SONG-VIEW2 V4, this file)
//   0x89000  spare                       the rest, 476 KiB, for the screens to come. In it: the screenshot's bands (OS-HIL, `diag shot`: kRamGShot, all but the last 64 bytes, which
//                                        are the display driver's DMA self-test scratch)
//
// A wave lane draws from one slot while the other is being filled and swaps when the new picture is complete: the chip
// re-reads RAM_G on every scan-out, so uploading over a bitmap that is on screen would tear (hal/display.h).

#include <o3os/hal/display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/boot_logo.h>
#include <o3os/ui/style.h>

#include <cstddef>
#include <cstdint>

namespace o3os::ui::plot {

struct RamGRegion {
  const char* name;
  std::uint32_t addr;
  std::uint32_t bytes;
};

constexpr std::uint32_t align_up(std::uint32_t v, std::uint32_t a) noexcept { return (v + a - 1u) / a * a; }

// The largest wave bitmap: the full panel width by kWaveMaxHeight rows, at L4 (the deeper of the two formats).
inline constexpr std::uint32_t kWaveMaxWidth = static_cast<std::uint32_t>(kScreenW);
inline constexpr std::uint32_t kWaveMaxHeight = 240;
inline constexpr std::uint32_t kWaveMaxBytes = kWaveMaxWidth * kWaveMaxHeight / 2u;  // L4: two pixels a byte
// A slot is a whole number of 4 KB pages so every slot starts on the upload chunk grid.
inline constexpr std::uint32_t kWaveSlotBytes = align_up(kWaveMaxBytes, 4096u);
inline constexpr unsigned kWaveLanes = 2;          // stereo: left and right
inline constexpr unsigned kWaveSlotsPerLane = 2;   // double buffered
inline constexpr unsigned kWaveSlots = kWaveLanes * kWaveSlotsPerLane;

inline constexpr std::uint32_t kRamGWave = align_up(kRamGEnd, 4096u);
inline constexpr std::uint32_t kRamGWaveEnd = kRamGWave + kWaveSlots * kWaveSlotBytes;
// The boot logo (OS-003f): the maintainer's picture, uploaded by ui::upload_assets() and drawn by the boot screen. A whole number of
// 4 KB pages like the wave slots, so the spare region behind it stays on the upload grid; the bitmap itself is kBootLogoBytes.
inline constexpr std::uint32_t kRamGBootLogo = kRamGWaveEnd;
inline constexpr std::uint32_t kRamGBootLogoBytes = align_up(kBootLogoBytes, 4096u);
inline constexpr std::uint32_t kRamGBootLogoEnd = kRamGBootLogo + kRamGBootLogoBytes;
// The note map (OS-SONG-VIEW2 V4): one L1 picture per pattern id, 128 columns (steps) by 24 rows (two a track: the track's ticks, then a blank row; 12 tracks), 16 bytes a row, written by app::SongNoteMap
// when a pattern's generation moves and drawn by the Song screen's TIMELINE page scaled to the block; picture `id` (1..255) is at kRamGNoteMap + (id - 1) * kNoteMapBytes. The whole table is rounded up
// to 4 KB pages so the spare region behind it stays on the upload grid.
inline constexpr unsigned kNoteMapCols = 128;
inline constexpr unsigned kNoteMapRows = 24;
inline constexpr unsigned kNoteMapStride = kNoteMapCols / 8u;                  // L1: eight pixels a byte
inline constexpr std::uint32_t kNoteMapBytes = kNoteMapStride * kNoteMapRows;  // one picture: 384 bytes, a whole number of words
inline constexpr unsigned kNoteMapPictures = 255;
inline constexpr std::uint32_t kRamGNoteMap = kRamGBootLogoEnd;
inline constexpr std::uint32_t kRamGNoteMapBytes = align_up(kNoteMapPictures * kNoteMapBytes, 4096u);
inline constexpr std::uint32_t kRamGNoteMapEnd = kRamGNoteMap + kRamGNoteMapBytes;
inline constexpr std::uint32_t kRamGSpare = kRamGNoteMapEnd;
inline constexpr std::uint32_t kRamGSpareBytes = hal::kRamGBytes - kRamGSpare;
// The last 64 bytes of RAM_G are the DMA self-test's scratch (platform/mk66/display/display.h kSelfTestBytes, which the composition root checks against this). OS-HIL: the console's
// `diag shot` writes its snapshot bands (CMD_SNAPSHOT2, platform/mk66/usb/shot_stream.h) into the rest of the spare region, nothing else: every band it asks for is sized to fit
// kRamGShotBytes. The spare region began at the boot logo's end; the note map (kRamGNoteMap) took its front, and the screenshot's region is what is left behind it. A further screen that needs memory takes it from the FRONT of
// the region too (after the note map: move kRamGSpare up and the screenshot's region shrinks with it), or from the end (shortening kRamGShotBytes), and the shot tests follow kRamGShotBytes.
inline constexpr std::uint32_t kRamGSelfTestBytes = 64;
inline constexpr std::uint32_t kRamGShot = kRamGSpare;
inline constexpr std::uint32_t kRamGShotBytes = kRamGSpareBytes - kRamGSelfTestBytes;

// Bitmap handles of the two wave lanes: after the eight the renderer binds (assets.h), below the coprocessor's 15.
inline constexpr std::uint8_t kHandleWaveLane0 = 9;
inline constexpr std::uint8_t kHandleWaveLane1 = 10;
// The boot logo's bitmap handle (OS-003f): after the two wave lanes, below the coprocessor's 15. Bound by the boot screen's own
// raw section (ui/boot_screen.h), not by Renderer::begin(): the logo is drawn once, at boot.
inline constexpr std::uint8_t kHandleBootLogo = 11;
// The note map's handle: the second wave lane's. The Song screen draws no wave, and the map's raw section (SAVE_CONTEXT ... RESTORE_CONTEXT) gives the handle back as it found it.
inline constexpr std::uint8_t kHandleNoteMap = kHandleWaveLane1;

// The address of slot `slot` (0..kWaveSlots-1) of lane `lane`: lane 0 owns slots 0 and 1, lane 1 slots 2 and 3.
constexpr std::uint32_t wave_slot_addr(unsigned lane, unsigned slot) noexcept {
  return kRamGWave + (lane * kWaveSlotsPerLane + slot) * kWaveSlotBytes;
}
constexpr std::uint8_t wave_handle(unsigned lane) noexcept { return lane == 0 ? kHandleWaveLane0 : kHandleWaveLane1; }

// Every fixed region, in address order.
inline constexpr RamGRegion kRamGRegions[] = {
    {"faces", 0, kRamGTiles},
    {"tiles", kRamGTiles, kRamGEnd - kRamGTiles},
    {"wave0a", wave_slot_addr(0, 0), kWaveSlotBytes},
    {"wave0b", wave_slot_addr(0, 1), kWaveSlotBytes},
    {"wave1a", wave_slot_addr(1, 0), kWaveSlotBytes},
    {"wave1b", wave_slot_addr(1, 1), kWaveSlotBytes},
    {"boot_logo", kRamGBootLogo, kRamGBootLogoBytes},
    {"note_map", kRamGNoteMap, kRamGNoteMapBytes},
    {"spare", kRamGSpare, kRamGSpareBytes},
};
inline constexpr unsigned kRamGRegionCount = sizeof(kRamGRegions) / sizeof(kRamGRegions[0]);

// True when every region is word aligned in address and size, lies inside RAM_G, and none overlaps another.
constexpr bool ram_g_regions_ok(const RamGRegion* regions, unsigned count) noexcept {
  for (unsigned i = 0; i < count; ++i) {
    const RamGRegion& a = regions[i];
    if (a.addr % hal::kUploadAlign != 0 || a.bytes % hal::kUploadAlign != 0 || a.bytes == 0 ||
        a.addr > hal::kRamGBytes || a.bytes > hal::kRamGBytes - a.addr) {
      return false;
    }
    for (unsigned j = i + 1; j < count; ++j) {
      const RamGRegion& b = regions[j];
      if (a.addr < b.addr + b.bytes && b.addr < a.addr + a.bytes) {
        return false;
      }
    }
  }
  return true;
}
static_assert(ram_g_regions_ok(kRamGRegions, kRamGRegionCount),
              "RAM_G regions are word aligned, inside RAM_G and disjoint");
static_assert(kRamGShot % hal::kUploadAlign == 0 && kRamGShot >= kRamGNoteMapEnd && kRamGShot + kRamGShotBytes + kRamGSelfTestBytes == hal::kRamGBytes,
              "the screenshot's region is the spare region less the self-test's scratch, on a word, behind every fixed region");
static_assert(kRamGWave >= kRamGEnd, "the wave slots start after the tiles and icons");
static_assert(kRamGWaveEnd <= hal::kRamGBytes, "the wave slots fit RAM_G");
static_assert(kWaveSlotBytes >= kWaveMaxBytes && kWaveSlotBytes % 4096u == 0, "a slot holds the largest wave bitmap");
static_assert(kRamGBootLogo % 4096u == 0 && kRamGBootLogo % hal::kUploadAlign == 0 && kRamGBootLogo >= kRamGWaveEnd,
              "the boot logo starts on the upload grid, after the wave slots");
static_assert(kRamGBootLogoBytes >= kBootLogoBytes && kRamGBootLogoBytes % 4096u == 0 && kRamGBootLogoEnd < hal::kRamGBytes,
              "the boot logo's region holds its bitmap and leaves the spare region something");
static_assert(kRamGNoteMap % 4096u == 0 && kRamGNoteMap >= kRamGBootLogoEnd && kNoteMapBytes % hal::kUploadAlign == 0 && kNoteMapPictures * kNoteMapBytes <= kRamGNoteMapBytes &&
                  kRamGNoteMapEnd < hal::kRamGBytes,
              "the note map's pictures are whole words, sit after the boot logo on the upload grid and leave the spare region something");
static_assert(kNoteMapCols % 8u == 0 && kNoteMapStride <= 1023u && kNoteMapRows <= 511u, "BITMAP_LAYOUT fields");
static_assert(kHandleNoteMap <= 14, "the note map's handle stays below the coprocessor's 15");
static_assert(kWaveMaxHeight < 512 && kWaveMaxWidth < 1024, "BITMAP_LAYOUT / BITMAP_SIZE fields (the _H commands cover the rest)");
static_assert(kHandleWaveLane0 > kHandleIcons && kHandleWaveLane0 < kHandleWaveLane1 && kHandleWaveLane1 <= 14,
              "wave handles follow the renderer's eight and stay below the coprocessor's 15");
static_assert(kHandleBootLogo > kHandleWaveLane1 && kHandleBootLogo <= 14, "the logo's handle follows the wave lanes and stays below 15");
static_assert(kHandleSmallHex > kHandleNotesHi && kHandleSmallNotes > kHandleNotesHi && kHandleSmallHex <= 14 && kHandleSmallNotes <= 14 && kHandleSmallHex != kHandleSmallNotes,
              "the smaller tiles' handles (OS-ZOOM) follow the second note-tile handle and stay below the coprocessor's 15");
static_assert(kHandleNotesHi > kHandleBootLogo && kHandleNotesHi <= 14, "the second note-tile handle follows the logo's and stays below 15");

}  // namespace o3os::ui::plot
