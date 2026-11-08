// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-PERFMODES M1: the twenty custom play orders C1..C20 as data (docs/specs/OS-PERFMODES.md section 3.1). The tables are the official firmware's
// (tracker-ce@7fd6c2c `core/source/playModeDefs.h`, an approved MIT component, data only: the notice is in THIRD_PARTY.md and in the header of the generated
// seq/src/play_orders_data.inc); the encoding, this accessor and every line of code are ours. Nothing calls this yet (M2 plays the orders in the render).
//
// The data (seq/src/play_orders_data.inc, committed; tools/gen_play_orders.py writes it, CI never runs the generator): 20 orders of 128 entries
// {step to play 1..128, FX option 0..15}, each folded to its period (a divisor of 128), one std::uint16_t an entry with bits 0-6 the step minus 1 and
// bits 8-11 the option, plus an index table of 20 {offset, period}. The table is in .rodata, not .ramfunc.
//
// OS-FLASHDIET P4: on the MK66 image (O3OS_PLAY_ORDERS_PACKED, set by seq/CMakeLists.txt) the 2,176 B of entries are not in flash: tools/play_orders_pack.py
// deflates them into a generated blob (kPlayOrdersZ), and the platform inflates it once at boot into a table of its own in SDRAM (play_orders_unpack),
// then hands the table to the accessor (play_orders_use) before the sequencer is built and before the render starts. Until then the accessor reads step 1,
// option 0 (defined, never reached in the firmware: nothing plays before make_sound() has attached the table). The host, the simulator and the tests keep
// the plain table, so the accessor's answers are the same everywhere (tests/seq_play_orders_blob_test.cpp holds the unpacked table to it).
//
// The accessor is pure: no allocation, no state to change, noexcept. The table pointer is a single-writer protocol: written by play_orders_use() from the
// main loop at boot, before the render line is enabled, read-only afterwards.

#include <cstddef>
#include <cstdint>

namespace o3os::core {
class Inflater;
}

namespace o3os::seq {

inline constexpr unsigned kPlayOrders = 20;         // C1..C20
inline constexpr unsigned kPlayOrderLen = 128;      // the indices an order is walked over
inline constexpr unsigned kPlayOrderEntries = 1088;  // std::uint16_t in the folded table (all twenty orders)

// The generated blob (tools/play_orders_pack.py): the raw-deflate stream of the entries' kPlayOrdersRawSize bytes, little-endian.
extern const std::uint8_t* const kPlayOrdersZ;
extern const std::size_t kPlayOrdersZSize;
extern const std::size_t kPlayOrdersRawSize;

// Inflates the packed entries into `table` (kPlayOrderEntries std::uint16_t, 2-aligned, any memory). true: the table is complete. Main loop only (the
// caller's decoder is busy while it runs).
bool play_orders_unpack(core::Inflater& inflater, std::uint16_t* table) noexcept;

// Makes `table` (a complete unpacked table, which must stay alive and unchanged while it is in use) the accessor's data; nullptr back to the built-in one
// (the host) or to "step 1, option 0" (the packed image). Main loop, before the render starts.
void play_orders_use(const std::uint16_t* table) noexcept;

// One entry of an order.
struct PlayOrderEntry {
  std::uint8_t step;    // 1..128, the step to play
  std::uint8_t option;  // 0..15, the entry's FX option (a roll; 0 none)
};

// What an FX option is: the Roll FX value (fx.h: 0..53 = 9 x direction + period) and the probability in percent.
struct OrderRoll {
  std::uint8_t value;    // the Roll value, 0 = none
  std::uint8_t percent;  // 0, 30, 60 or 100
};

// The entry of an order at an index: order 0..19 (anything else reads C1), index any (taken % 128, then within the order's period).
PlayOrderEntry play_order_entry(unsigned order, unsigned index) noexcept;

// The roll an FX option carries (F7, F8 with F15: the Roll value = period + 9 x direction, period 1/1 = 1, 1/2 = 2, 1/4 = 4, direction flat 0,
// volume-down 1): 0 {0, 0}; 1-3 flat 1/4 at 30/60/100 % {4, ...}; 4-6 volume-down 1/4 {13, ...}; 7-9 volume-down 1/2 {11, ...}; 10-12 flat 1/1 {1, ...};
// 13-15 volume-down 1/1 {10, ...}. An option above 15 is none.
OrderRoll order_roll(unsigned option) noexcept;

}  // namespace o3os::seq
