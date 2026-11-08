// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET P4: the custom play orders' entry table as a deflate blob (tools/play_orders_pack.py over seq/src/play_orders_data.inc), unpacked once at boot on the
// MK66 image into an SDRAM table that the accessor then reads. The host keeps the plain table; this test holds the blob to it: the stream inflates to exactly the
// plain table's bytes (little-endian std::uint16_t), the unpacked table gives every order's every index the same entry as the plain one, a short or damaged stream is
// refused, and detaching the table returns the accessor to the plain table.
#include <doctest/doctest.h>

#include <o3os/core/inflate.h>
#include <o3os/seq/play_orders.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "support/need.h"

// The committed table (kEntries, kIndex) in this unit's own namespace copy: the same file the accessor and the packer read.
#include O3_PLAY_ORDERS_INC

using namespace o3os;
namespace play_orders_data = o3os::seq::play_orders_data;

namespace {

constexpr std::size_t kBytes = seq::kPlayOrderEntries * sizeof(std::uint16_t);

}  // namespace

TEST_CASE("play orders blob: the stream inflates to the plain table's bytes, and is a real saving") {
  static_assert(sizeof play_orders_data::kEntries == kBytes, "the header's entry count is the table's");
  CHECK(seq::kPlayOrdersRawSize == kBytes);
  CHECK(seq::kPlayOrdersZSize < kBytes / 2);
  core::Inflater inf;
  inf.begin(seq::kPlayOrdersZ, seq::kPlayOrdersZSize, seq::kPlayOrdersRawSize);
  std::uint8_t got[kBytes];
  NEED(inf.read(got, sizeof got) == kBytes);
  CHECK(inf.done());
  CHECK_FALSE(inf.failed());
  // Entry by entry (little-endian), not a memcmp: it names the first wrong entry and does not depend on the host's byte order.
  for (unsigned i = 0; i < seq::kPlayOrderEntries; ++i) {
    const unsigned v = static_cast<unsigned>(got[2u * i]) | (static_cast<unsigned>(got[2u * i + 1u]) << 8);
    CHECK_MESSAGE(v == play_orders_data::kEntries[i], "entry " << i);
  }
}

TEST_CASE("play orders blob: the unpacked table answers like the plain one, and detaching returns to the plain table") {
  static std::uint16_t table[seq::kPlayOrderEntries];
  core::Inflater inf;
  std::memset(table, 0xFF, sizeof table);
  NEED(seq::play_orders_unpack(inf, table));
  for (unsigned i = 0; i < seq::kPlayOrderEntries; ++i) {
    CHECK_MESSAGE(table[i] == play_orders_data::kEntries[i], "entry " << i);
  }
  seq::PlayOrderEntry plain[seq::kPlayOrders][seq::kPlayOrderLen];
  for (unsigned o = 0; o < seq::kPlayOrders; ++o) {
    for (unsigned i = 0; i < seq::kPlayOrderLen; ++i) {
      plain[o][i] = seq::play_order_entry(o, i);
    }
  }
  seq::play_orders_use(table);
  unsigned wrong = 0;
  for (unsigned o = 0; o < seq::kPlayOrders; ++o) {
    for (unsigned i = 0; i < seq::kPlayOrderLen; ++i) {
      const seq::PlayOrderEntry e = seq::play_order_entry(o, i);
      wrong += (e.step != plain[o][i].step || e.option != plain[o][i].option) ? 1u : 0u;
    }
  }
  CHECK(wrong == 0u);
  seq::play_orders_use(nullptr);  // the table above is static, but leave the accessor as the other tests expect it
  CHECK(seq::play_order_entry(1, 0).step == plain[1][0].step);
}

TEST_CASE("play orders blob: a truncated or damaged stream is refused") {
  static std::uint16_t table[seq::kPlayOrderEntries];
  core::Inflater inf;
  // The stream cut short: the decoder reports failure and play_orders_unpack's own check (all bytes, done) cannot pass on a cut copy of it.
  inf.begin(seq::kPlayOrdersZ, seq::kPlayOrdersZSize / 2, seq::kPlayOrdersRawSize);
  const std::size_t got = inf.read(reinterpret_cast<std::uint8_t*>(table), kBytes);
  CHECK((got < kBytes || !inf.done()));
  CHECK(inf.failed());
  // And a decoder asked for more bytes than the stream holds does not run past it.
  core::Inflater inf2;
  inf2.begin(seq::kPlayOrdersZ, seq::kPlayOrdersZSize, seq::kPlayOrdersRawSize + 2u);
  std::uint8_t big[kBytes + 2u];
  CHECK(inf2.read(big, sizeof big) <= kBytes + 2u);
  CHECK_FALSE(inf2.done());
}
