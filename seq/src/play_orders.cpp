// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PERFMODES M1: the accessor over the generated table (seq/play_orders.h; the data is seq/src/play_orders_data.inc).
#include <o3os/seq/play_orders.h>

#include <o3os/core/inflate.h>

#include "play_orders_data.inc"

namespace o3os::seq {

static_assert(sizeof play_orders_data::kEntries == kPlayOrderEntries * sizeof(std::uint16_t), "kPlayOrderEntries is the committed table's size");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the packed table is the little-endian bytes of the std::uint16_t entries");

namespace {
// The unpacked table in use (play_orders_use). Single writer: the main loop at boot, before the render starts; read-only afterwards, so a plain pointer.
const std::uint16_t* g_table = nullptr;
}  // namespace

bool play_orders_unpack(core::Inflater& inflater, std::uint16_t* table) noexcept {
  constexpr std::size_t kBytes = kPlayOrderEntries * sizeof(std::uint16_t);
  if (kPlayOrdersRawSize != kBytes) {
    return false;
  }
  inflater.begin(kPlayOrdersZ, kPlayOrdersZSize, kBytes);
  const std::size_t got = inflater.read(reinterpret_cast<std::uint8_t*>(table), kBytes);
  return got == kBytes && inflater.done() && !inflater.failed();
}

void play_orders_use(const std::uint16_t* table) noexcept { g_table = table; }

PlayOrderEntry play_order_entry(unsigned order, unsigned index) noexcept {
  if (order >= kPlayOrders) {
    order = 0u;  // anything past C20 reads C1
  }
  const play_orders_data::OrderIndex& ix = play_orders_data::kIndex[order];
  unsigned i = index % kPlayOrderLen;
  i %= ix.period;  // within the order's period (a divisor of 128)
  const std::uint16_t* table = g_table;
#if defined(O3OS_PLAY_ORDERS_PACKED)
  if (table == nullptr) {
    return PlayOrderEntry{1u, 0u};  // not attached (the firmware attaches it before anything plays)
  }
#else
  if (table == nullptr) {
    table = play_orders_data::kEntries;
  }
#endif
  const std::uint16_t e = table[ix.offset + i];
  PlayOrderEntry out;
  out.step = static_cast<std::uint8_t>((e & 0x7Fu) + 1u);
  out.option = static_cast<std::uint8_t>((e >> 8) & 0xFu);
  return out;
}

OrderRoll order_roll(unsigned option) noexcept {
  // The option codes are the official enum's order (F3): 0 none; 1-3 flat 1/4, 4-6 dec 1/4, 7-9 dec 1/2, 10-12 flat 1/1, 13-15 dec 1/1,
  // each at 30, 60, 100 %. The Roll value = period + 9 x direction (F8, F15).
  constexpr std::uint8_t kValue[16] = {0, 4, 4, 4, 13, 13, 13, 11, 11, 11, 1, 1, 1, 10, 10, 10};
  constexpr std::uint8_t kPercent[16] = {0, 30, 60, 100, 30, 60, 100, 30, 60, 100, 30, 60, 100, 30, 60, 100};
  if (option > 15u) {
    option = 0u;
  }
  OrderRoll out;
  out.value = kValue[option];
  out.percent = kPercent[option];
  return out;
}

}  // namespace o3os::seq
