// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 project region against the project model: platform/mk66 may not include the model (layer rule), so mem_map.h
// mirrors the model arena's worst-case size and this test, in a layer that sees both, keeps the mirror honest (host only:
// the QEMU suite of the platform has no model).
#include <doctest/doctest.h>

#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>
#include <o3os/platform/mk66/mem_map.h>

namespace mem = o3os::platform::mk66::mem;

static_assert(mem::kProjectSize >= o3os::model::kModelArenaBytes, "the SDRAM project region holds the whole model arena");

TEST_CASE("mem_map x model: the mirrored arena size is the real one, and the project region holds it") {
  CHECK(mem::kModelArenaWorstCaseBytes == o3os::model::kModelArenaBytes);
  CHECK(mem::kProjectSize >= o3os::model::kModelArenaBytes);
  CHECK(mem::kProjectBase % alignof(o3os::model::ModelArena) == 0u);
}

// OS-MIDIREC-P1 S2 (maintainer decision 2026-11-10): the LaneStore lives in the delay region's spare, after the 4.0 s ring.
static_assert(sizeof(o3os::model::LaneStore) == mem::kLaneStoreBytes, "mem_map.h mirrors sizeof(LaneStore)");

TEST_CASE("mem_map x model: the delay region's spare holds the LaneStore, aligned, with a margin (OS-MIDIREC-P1 S2)") {
  CHECK(mem::kLaneStoreBytes == sizeof(o3os::model::LaneStore));
  CHECK(mem::kLaneStoreBase % alignof(o3os::model::LaneStore) == 0u);
  CHECK(mem::kLaneStoreBase >= mem::kDelayBase + mem::kDelayRingBytes);
  CHECK(mem::kLaneStoreBase + sizeof(o3os::model::LaneStore) <= mem::kDelayBase + mem::kDelaySize);
  CHECK(mem::kDelayBase + mem::kDelaySize - (mem::kLaneStoreBase + sizeof(o3os::model::LaneStore)) >= 1024u);
  CHECK(o3os::model::kMaxLanePoolEvents == 7680u);  // the one constant: limits.h
}
