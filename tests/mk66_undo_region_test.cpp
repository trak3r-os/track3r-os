// SPDX-License-Identifier: GPL-3.0-or-later
//
// The undo journal's region (OS-002c): `model::kUndoRegionBytes` (ADR 0013 D7: 384 KiB) is the size the journal's capacity
// arithmetic is written for, and `mem::kUndoSize` is the region the memory map reserves for it. Neither layer may include the
// other (the model knows no platform, the platform no model), so this test, which sees both, keeps them equal. It runs on the
// host and on QEMU (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3os/model/undo.h>
#include <o3os/platform/mk66/mem_map.h>

#include <cstddef>
#include <cstdint>

namespace mem = o3os::platform::mk66::mem;
namespace model = o3os::model;

static_assert(model::kUndoRegionBytes == mem::kUndoSize, "the journal's region (undo.h) and the memory map's (mem_map.h) differ");
// OS-UNDOGRP: the region is carved into the three rings of the areas, each a multiple of 4, and together exactly the region.
static_assert(model::kUndoPatternBytes + model::kUndoSongBytes + model::kUndoSoundBytes == mem::kUndoSize, "the three rings are the map's region");
static_assert(model::kUndoPatternBytes % 4u == 0u && model::kUndoSongBytes % 4u == 0u && model::kUndoSoundBytes % 4u == 0u, "each ring is a multiple of 4 bytes");

TEST_CASE("mk66 undo region: the journal's size is the memory map's, and it is a region the journal accepts") {
  CHECK(model::kUndoRegionBytes == mem::kUndoSize);
  CHECK(mem::kUndoSize == 384u * 1024u);
  // 4-byte aligned base and a multiple of 4 (the journal's own conditions), and the map's 32-byte rule for every SDRAM region.
  CHECK(mem::kUndoBase % 4u == 0u);
  CHECK(mem::kUndoSize % 4u == 0u);
  CHECK(mem::kUndoBase % 32u == 0u);
  CHECK(mem::kUndoSize <= model::UndoJournal::kMaxRegionBytes);
  CHECK(mem::kUndoSize >= model::UndoJournal::kMinRegionBytes);
  // The region is inside SDRAM, between the project and the ptfx region (OS-CEPTFX S2a took file_job + lut + 64 KiB of ui_cache), as the table lists it.
  CHECK(mem::kUndoBase >= mem::kSdramBase);
  CHECK(mem::kUndoBase + mem::kUndoSize == mem::kPtfxBase);
  CHECK(mem::kProjectBase + mem::kProjectSize == mem::kUndoBase);
  // The three rings (D6), on a small region (this runs on QEMU too; the 160 / 96 / 128 KiB themselves are the constants above): 5/12, 3/12 and 4/12, a multiple of 4 each, tiling the region.
  alignas(8) static std::uint8_t region[12 * 1024];
  model::UndoHistory history;
  CHECK(history.begin(region, sizeof region));
  CHECK(history.ring[0].capacity_bytes() == 5u * 1024u);
  CHECK(history.ring[1].capacity_bytes() == 3u * 1024u);
  CHECK(history.ring[2].capacity_bytes() == 4u * 1024u);
  std::size_t total = 0;
  for (const model::UndoJournal& r : history.ring) {
    CHECK(r.capacity_bytes() % 4u == 0u);
    CHECK(r.capacity_bytes() >= model::UndoJournal::kMinRegionBytes);
    total += r.capacity_bytes();
  }
  CHECK(total == sizeof region);
  CHECK(model::kUndoPatternBytes * 12u == model::kUndoRegionBytes * 5u);  // the same proportions at the real size
  CHECK(model::kUndoSongBytes * 12u == model::kUndoRegionBytes * 3u);
  CHECK(model::kUndoSoundBytes * 12u == model::kUndoRegionBytes * 4u);
}
