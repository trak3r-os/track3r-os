// SPDX-License-Identifier: GPL-3.0-or-later
//
// The RAM_G map (ui/plot/ram_g_map.h): every fixed region of the FT81x's graphics memory is word aligned, inside RAM_G and
// disjoint from the others, the wave slots sit on the 4 KB upload grid after the tiles and icons, and the checker that
// proves it rejects what it should.
#include <doctest/doctest.h>

#include <o3os/hal/display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/boot_logo.h>
#include <o3os/ui/plot/ram_g_map.h>
#include <o3os/ui/plot/wave_bitmap.h>

#include <cstring>

#include "../platform/mk66/tests/mk66_shot_region.h"

using namespace o3os;
using namespace o3os::ui::plot;

TEST_CASE("plot ram_g: the map is word aligned, inside RAM_G and disjoint, in address order, and the numbers are the documented ones") {
  CHECK(ram_g_regions_ok(kRamGRegions, kRamGRegionCount));
  CHECK(kRamGRegionCount == 9);
  CHECK(kRamGRegions[0].addr == 0);
  std::uint32_t end = 0;
  for (unsigned i = 0; i < kRamGRegionCount; ++i) {
    const RamGRegion& r = kRamGRegions[i];
    CHECK_MESSAGE(r.addr % hal::kUploadAlign == 0, r.name);
    CHECK_MESSAGE(r.bytes % hal::kUploadAlign == 0, r.name);
    CHECK_MESSAGE(r.addr >= end, r.name << " overlaps its predecessor");  // address order, so adjacent regions touch at most
    end = r.addr + r.bytes;
  }
  CHECK(end == hal::kRamGBytes);  // the spare region runs to the end: every byte of RAM_G is accounted for
  // what the 003b assets own, unchanged
  CHECK(kRamGRegions[0].bytes == ui::kRamGTiles);
  CHECK(kRamGRegions[1].addr == ui::kRamGTiles);
  CHECK(kRamGRegions[1].addr + kRamGRegions[1].bytes == ui::kRamGEnd);
  // the wave slots: after them, on the 4 KB grid, one slot holds the largest picture
  CHECK(kRamGWave == 0xF000);  // (0xB000 until the smaller tiles of the zoomed pattern grid, OS-ZOOM)
  CHECK(kRamGWave >= ui::kRamGEnd);
  CHECK(kRamGWave % 4096 == 0);
  CHECK(kWaveSlotBytes == 98304);
  CHECK(kWaveSlotBytes % 4096 == 0);
  CHECK(WaveBitmap::stride_of(kWaveMaxWidth, WaveDepth::L4) * kWaveMaxHeight == 96000);
  CHECK(WaveBitmap::stride_of(kWaveMaxWidth, WaveDepth::L4) * kWaveMaxHeight <= kWaveSlotBytes);
  // the boot logo (OS-003f): two pages right after the wave slots, then the spare region
  CHECK(kRamGBootLogo == kRamGWaveEnd);
  CHECK(kRamGBootLogo == 0x6F000);
  CHECK(kRamGBootLogo % 4096 == 0);
  CHECK(ui::kBootLogoBytes == 7688);
  CHECK(kRamGBootLogoBytes == 8192);
  CHECK(kRamGBootLogoBytes >= ui::kBootLogoBytes);
  CHECK(std::strcmp(kRamGRegions[6].name, "boot_logo") == 0);
  CHECK(kRamGRegions[6].addr == kRamGBootLogo);
  CHECK(kRamGRegions[6].bytes == kRamGBootLogoBytes);
  // the note map (OS-SONG-VIEW2 V4): 255 pictures of 384 bytes (128 x 24 at L1) right after the logo, rounded up to 4 KB pages, then the spare region
  CHECK(kNoteMapBytes == 384);
  CHECK(kNoteMapStride == 16);
  CHECK(kRamGNoteMap == kRamGBootLogoEnd);
  CHECK(kRamGNoteMap == 0x71000);
  CHECK(kNoteMapPictures * kNoteMapBytes == 97920);
  CHECK(kRamGNoteMapBytes == 98304);
  CHECK(std::strcmp(kRamGRegions[7].name, "note_map") == 0);
  CHECK(kRamGRegions[7].addr == kRamGNoteMap);
  CHECK(kRamGRegions[7].bytes == kRamGNoteMapBytes);
  CHECK(kRamGSpare == 0x89000);
  CHECK(kRamGSpare == kRamGNoteMapEnd);
  CHECK(kRamGSpareBytes == 487424);
  CHECK(std::strcmp(kRamGRegions[8].name, "spare") == 0);
  CHECK(kRamGRegions[8].addr == kRamGSpare);
  // OS-HIL: the screenshot's bands live in the spare region, less the DMA self-test's last 64 bytes
  CHECK(kRamGShot == kRamGSpare);
  CHECK(kRamGShotBytes == 487360);
  CHECK(kRamGShot == platform::mk66::test::kShotRegion);  // the shot tests' region (platform/mk66/tests/mk66_shot_region.h: they may not include ui)
  CHECK(kRamGShotBytes == platform::mk66::test::kShotRegionBytes);
  CHECK(kRamGShotBytes >= 240u * 800u * 2u);  // the screenshot's two bands of 240 rows at RGB565 still fit
  CHECK(kRamGShot + kRamGShotBytes + kRamGSelfTestBytes == hal::kRamGBytes);
  CHECK(kRamGShot >= kRamGNoteMapEnd);
  CHECK(kHandleBootLogo == 11);
  CHECK(kHandleBootLogo > kHandleWaveLane1);
  CHECK(kHandleBootLogo <= 14);
  for (unsigned lane = 0; lane < kWaveLanes; ++lane) {
    for (unsigned slot = 0; slot < kWaveSlotsPerLane; ++slot) {
      const std::uint32_t addr = wave_slot_addr(lane, slot);
      CHECK(addr == kRamGWave + (lane * kWaveSlotsPerLane + slot) * kWaveSlotBytes);
      CHECK(addr % 4096 == 0);
      CHECK(addr + kWaveSlotBytes <= kRamGWaveEnd);
    }
  }
  // the bitmap handles: past the renderer's eight, below the coprocessor's 15, distinct
  CHECK(wave_handle(0) == kHandleWaveLane0);
  CHECK(wave_handle(1) == kHandleWaveLane1);
  CHECK(kHandleWaveLane0 > ui::kHandleIcons);
  CHECK(kHandleWaveLane0 != kHandleWaveLane1);
  CHECK(kHandleWaveLane1 <= 14);
  // a picture of any size the lane may ask for fits its slot, and a chunk of 4 KB is a whole number of words
  CHECK(hal::kMaxUploadBytes % hal::kUploadAlign == 0);
  CHECK(WaveBitmap::kChunkBytes == hal::kMaxUploadBytes);
}

TEST_CASE("plot ram_g: the checker rejects overlap, misalignment, empty and out-of-range regions") {
  const RamGRegion good[] = {{"a", 0, 64}, {"b", 64, 64}, {"c", 4096, 4}};
  CHECK(ram_g_regions_ok(good, 3));
  CHECK(ram_g_regions_ok(good, 0));  // nothing to check
  const RamGRegion overlap[] = {{"a", 0, 100}, {"b", 96, 64}};
  CHECK_FALSE(ram_g_regions_ok(overlap, 2));
  const RamGRegion inside[] = {{"a", 0, 1000}, {"b", 100, 4}};  // one within the other
  CHECK_FALSE(ram_g_regions_ok(inside, 2));
  const RamGRegion reversed[] = {{"b", 100, 4}, {"a", 0, 1000}};
  CHECK_FALSE(ram_g_regions_ok(reversed, 2));
  const RamGRegion unaligned_addr[] = {{"a", 2, 64}};
  CHECK_FALSE(ram_g_regions_ok(unaligned_addr, 1));
  const RamGRegion unaligned_size[] = {{"a", 0, 62}};
  CHECK_FALSE(ram_g_regions_ok(unaligned_size, 1));
  const RamGRegion empty[] = {{"a", 0, 0}};
  CHECK_FALSE(ram_g_regions_ok(empty, 1));
  const RamGRegion too_big[] = {{"a", hal::kRamGBytes - 64, 128}};
  CHECK_FALSE(ram_g_regions_ok(too_big, 1));
  const RamGRegion past[] = {{"a", hal::kRamGBytes + 4, 4}};
  CHECK_FALSE(ram_g_regions_ok(past, 1));
  const RamGRegion last_word[] = {{"a", hal::kRamGBytes - 4, 4}};
  CHECK(ram_g_regions_ok(last_word, 1));
  CHECK(align_up(1, 4096) == 4096);
  CHECK(align_up(4096, 4096) == 4096);
  CHECK(align_up(0, 4096) == 0);
}
