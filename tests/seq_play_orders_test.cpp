// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-PERFMODES M1: the twenty custom play orders as data (seq/play_orders.h, seq/src/play_orders_data.inc, docs/specs/OS-PERFMODES.md section 3.1) against the
// pinned facts of F12, typed here from the spec, never computed from the table under test: the CRC-32 of every order's expansion over 128 indices as
// (step, option) bytes, of the whole table and of the steps alone; every order begins with step 1; C17 equals C18; the spot values of the M1 acceptance;
// order_roll for all 16 options; an index of 128 + i equals i; an order past 19 reads C1. Host and Cortex-M4F (`qemu_seq`).
#include <doctest/doctest.h>

#include <o3os/seq/play_orders.h>

#include <cstddef>
#include <cstdint>

namespace seq = o3os::seq;

namespace {

// CRC-32 (the reflected 0xEDB88320 polynomial, init and xorout 0xFFFFFFFF), as zlib computes it.
std::uint32_t crc32(const std::uint8_t* data, std::size_t n) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// The pinned CRC-32 of each order over its 256 bytes (step, option) of the 128 entries, C1..C20 (F12, typed).
constexpr std::uint32_t kOrderCrc[seq::kPlayOrders] = {
    0x215FFAADu, 0x77F04872u, 0x1EFA15B0u, 0x77466A79u, 0xB99CDFDFu, 0xA852E05Du, 0xACCAECA6u, 0x8E57FB57u,
    0xC8D7DF9Fu, 0x7E1C050Eu, 0x8F11C234u, 0x1604325Au, 0xB3D6A038u, 0xBF27215Cu, 0x81079E39u, 0xE2847D96u,
    0x053F8CD0u, 0x053F8CD0u, 0x5150340Du, 0xE85DD985u,
};
constexpr std::uint32_t kWholeCrc = 0x9B2DC0FDu;  // the 5,120 bytes (step, option) of C1..C20
constexpr std::uint32_t kStepsCrc = 0x584852DDu;  // the 2,560 bytes of the steps alone

// The expansion of an order over its 128 indices, as (step, option) bytes.
void expand(unsigned order, std::uint8_t (&out)[seq::kPlayOrderLen * 2]) noexcept {
  for (unsigned i = 0; i < seq::kPlayOrderLen; ++i) {
    const seq::PlayOrderEntry e = seq::play_order_entry(order, i);
    out[2 * i] = e.step;
    out[2 * i + 1] = e.option;
  }
}

}  // namespace

TEST_CASE("play orders: the pinned CRCs of F12 over the expansion of every order") {
  static std::uint8_t whole[seq::kPlayOrders * seq::kPlayOrderLen * 2];  // in .bss, not on the stack
  static std::uint8_t steps[seq::kPlayOrders * seq::kPlayOrderLen];
  for (unsigned o = 0; o < seq::kPlayOrders; ++o) {
    std::uint8_t buf[seq::kPlayOrderLen * 2];
    expand(o, buf);
    CHECK(crc32(buf, sizeof(buf)) == kOrderCrc[o]);
    for (unsigned i = 0; i < seq::kPlayOrderLen; ++i) {
      whole[(o * seq::kPlayOrderLen + i) * 2] = buf[2 * i];
      whole[(o * seq::kPlayOrderLen + i) * 2 + 1] = buf[2 * i + 1];
      steps[o * seq::kPlayOrderLen + i] = buf[2 * i];
    }
  }
  CHECK(crc32(whole, sizeof(whole)) == kWholeCrc);
  CHECK(crc32(steps, sizeof(steps)) == kStepsCrc);
}

TEST_CASE("play orders: every order begins with step 1, and C17 equals C18") {
  for (unsigned o = 0; o < seq::kPlayOrders; ++o) {
    CHECK(seq::play_order_entry(o, 0).step == 1u);
  }
  for (unsigned i = 0; i < seq::kPlayOrderLen; ++i) {
    const seq::PlayOrderEntry a = seq::play_order_entry(16, i);  // C17
    const seq::PlayOrderEntry b = seq::play_order_entry(17, i);  // C18
    CHECK(a.step == b.step);
    CHECK(a.option == b.option);
  }
}

TEST_CASE("play orders: the spot values typed from the spec (M1 acceptance)") {
  const unsigned kC1[16] = {1, 2, 3, 4, 1, 2, 3, 4, 9, 10, 11, 12, 1, 1, 15, 16};
  const unsigned kC5[16] = {1, 2, 3, 4, 5, 6, 7, 8, 8, 7, 6, 5, 4, 3, 2, 1};
  const unsigned kC11[8] = {1, 1, 1, 1, 2, 2, 2, 2};
  const unsigned kC16[16] = {1, 2, 3, 1, 2, 3, 5, 2, 2, 1, 2, 4, 1, 2, 5, 2};
  const unsigned kC19[8] = {1, 2, 30, 1, 2, 30, 1, 2};
  for (unsigned i = 0; i < 16; ++i) {
    CHECK(seq::play_order_entry(0, i).step == kC1[i]);    // C1, entries 1-16
    CHECK(seq::play_order_entry(4, i).step == kC5[i]);    // C5, entries 1-16
    CHECK(seq::play_order_entry(15, i).step == kC16[i]);  // C16, entries 1-16
  }
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(seq::play_order_entry(10, i).step == kC11[i]);  // C11, entries 1-8
    CHECK(seq::play_order_entry(18, i).step == kC19[i]);  // C19, entries 1-8
  }
  // C2's six rolls of F12 (entries 1-based): 1 dec 1/2 60 %, 23 flat 1/1 30 %, 42 flat 1/1 100 %,
  // 83 flat 1/1 30 %, 118 flat 1/1 60 %, 127 dec 1/4 100 % (options 8, 10, 12, 10, 11, 6).
  const unsigned kC2Entry[6] = {1, 23, 42, 83, 118, 127};
  const unsigned kC2Option[6] = {8, 10, 12, 10, 11, 6};
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(seq::play_order_entry(1, kC2Entry[i] - 1).option == kC2Option[i]);
  }
}

TEST_CASE("play orders: order_roll for all 16 options (section 3.1)") {
  const seq::OrderRoll kExpected[16] = {
      {0, 0},   {4, 30},  {4, 60},  {4, 100},   // 0 none; 1-3 flat 1/4
      {13, 30}, {13, 60}, {13, 100},            // 4-6 dec 1/4
      {11, 30}, {11, 60}, {11, 100},            // 7-9 dec 1/2
      {1, 30},  {1, 60},  {1, 100},             // 10-12 flat 1/1
      {10, 30}, {10, 60}, {10, 100},            // 13-15 dec 1/1
  };
  for (unsigned o = 0; o < 16; ++o) {
    const seq::OrderRoll r = seq::order_roll(o);
    CHECK(r.value == kExpected[o].value);
    CHECK(r.percent == kExpected[o].percent);
  }
  const seq::OrderRoll none = seq::order_roll(16);  // an option above 15 is none
  CHECK(none.value == 0u);
  CHECK(none.percent == 0u);
}

TEST_CASE("play orders: an index of 128 + i equals i, and an order past 19 reads C1") {
  for (unsigned o = 0; o < seq::kPlayOrders; ++o) {
    for (unsigned i = 0; i < seq::kPlayOrderLen; ++i) {
      const seq::PlayOrderEntry a = seq::play_order_entry(o, seq::kPlayOrderLen + i);
      const seq::PlayOrderEntry b = seq::play_order_entry(o, i);
      CHECK(a.step == b.step);
      CHECK(a.option == b.option);
    }
  }
  const unsigned kBadOrders[4] = {20u, 21u, 100u, 0xFFFFFFFFu};
  for (unsigned b = 0; b < 4; ++b) {
    const unsigned bad = kBadOrders[b];
    for (unsigned i = 0; i < seq::kPlayOrderLen; ++i) {
      const seq::PlayOrderEntry a = seq::play_order_entry(bad, i);
      const seq::PlayOrderEntry c1 = seq::play_order_entry(0, i);
      CHECK(a.step == c1.step);
      CHECK(a.option == c1.option);
    }
  }
}
