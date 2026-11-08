// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET I0: the module libc shim's portable half (modules/importers/tracker/libc_shim.h, the functions libc_shim.cpp gives their
// extern "C" / __aeabi names) against the host's native 64-bit arithmetic: the shift and multiply helpers and udiv64_impl over the edge
// values and shifts (0, 1, 31, 32, 33, 63; divisors 1, 3, 2^32-1, 2^32, 2^63 and dividend < divisor), memmove over overlapping ranges
// both ways, and strncpy's padding. The __aeabi_uldivmod asm trampoline is target-only and not covered here. Host only.
#include <doctest/doctest.h>

#include "../modules/importers/tracker/libc_shim.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::vector<std::uint64_t> edge_values() {
  return {0,
          1,
          2,
          3,
          10,
          31,
          32,
          33,
          63,
          64,
          0xFFFFFFFFull,
          0x100000000ull,
          0x123456789ABCDEF0ull,
          0x7FFFFFFFFFFFFFFFull,
          0x8000000000000000ull,
          0xDEADBEEFCAFEBABEull,
          0xFFFFFFFFFFFFFFFFull};
}

std::vector<int> edge_shifts() { return {0, 1, 2, 30, 31, 32, 33, 62, 63}; }

std::vector<std::uint64_t> edge_divisors() { return {1, 3, 0xFFFFFFFFull, 0x100000000ull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull}; }

}  // namespace

TEST_CASE("module shim: the 64-bit shifts match native arithmetic over the edge values and shifts") {
  for (const std::uint64_t v : edge_values()) {
    for (const int s : edge_shifts()) {
      CHECK(tracker_shim::llsl(v, s) == (v << s));
      CHECK(tracker_shim::llsr(v, s) == (v >> s));
      CHECK(tracker_shim::lasr(v, s) == static_cast<std::uint64_t>(static_cast<std::int64_t>(v) >> s));
    }
  }
}

TEST_CASE("module shim: a shift count outside 0..63 follows the shim's contract") {
  for (const std::uint64_t v : edge_values()) {
    CHECK(tracker_shim::llsl(v, -1) == v);
    CHECK(tracker_shim::llsr(v, -5) == v);
    CHECK(tracker_shim::lasr(v, 0) == v);
    CHECK(tracker_shim::llsl(v, 64) == 0);
    CHECK(tracker_shim::llsl(v, 100) == 0);
    CHECK(tracker_shim::llsr(v, 64) == 0);
    const std::uint64_t sign_fill = static_cast<std::int64_t>(v) < 0 ? ~std::uint64_t(0) : std::uint64_t(0);
    CHECK(tracker_shim::lasr(v, 64) == sign_fill);
    CHECK(tracker_shim::lasr(v, 100) == sign_fill);
  }
}

TEST_CASE("module shim: the 64x64 multiply matches native multiplication") {
  for (const std::uint64_t a : edge_values()) {
    for (const std::uint64_t b : edge_values()) {
      CHECK(tracker_shim::lmul(a, b) == a * b);
    }
  }
}

TEST_CASE("module shim: udiv64_impl matches native 64-bit division and remainder") {
  for (const std::uint64_t n : edge_values()) {
    for (const std::uint64_t d : edge_divisors()) {
      std::uint64_t out[2] = {0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull};
      tracker_shim::udiv64_impl(n, d, out);
      CHECK(out[0] == n / d);  // covers dividend < divisor too (quotient 0)
      CHECK(out[1] == n % d);
    }
  }
  // 10 / 3 was the broken case: the quotient never got its bits shifted in.
  std::uint64_t out[2] = {0, 0};
  tracker_shim::udiv64_impl(10, 3, out);
  CHECK(out[0] == 3);
  CHECK(out[1] == 1);
  // 1 << 40 was the broken llsl case; exercise it through the divider's neighbourhood too.
  tracker_shim::udiv64_impl(std::uint64_t(1) << 40, 3, out);
  CHECK(out[0] == (std::uint64_t(1) << 40) / 3);
  CHECK(out[1] == (std::uint64_t(1) << 40) % 3);
  // A zero divisor leaves quotient and remainder zero (native division by zero is UB, so this is the shim's own contract).
  tracker_shim::udiv64_impl(0xFFFFFFFFFFFFFFFFull, 0, out);
  CHECK(out[0] == 0);
  CHECK(out[1] == 0);
}

TEST_CASE("module shim: memmove copies overlapping ranges both ways") {
  const std::size_t n = 16;
  std::uint8_t base[n];
  for (std::size_t i = 0; i < n; ++i) {
    base[i] = static_cast<std::uint8_t>(i + 1);
  }
  // dst < src (forward copy): buf <- buf+4, 8 bytes overlap.
  {
    std::uint8_t a[n], ref[n];
    std::memcpy(a, base, n);
    std::memcpy(ref, base, n);
    CHECK(tracker_shim::memmove(a, a + 4, 8) == a);
    std::memmove(ref, ref + 4, 8);
    CHECK(std::memcmp(a, ref, n) == 0);
  }
  // dst > src (backward copy): buf+4 <- buf, 8 bytes overlap.
  {
    std::uint8_t a[n], ref[n];
    std::memcpy(a, base, n);
    std::memcpy(ref, base, n);
    CHECK(tracker_shim::memmove(a + 4, a, 8) == a + 4);
    std::memmove(ref + 4, ref, 8);
    CHECK(std::memcmp(a, ref, n) == 0);
  }
  // Fully overlapping (dst == src) and a zero length leave the buffer alone.
  {
    std::uint8_t a[n];
    std::memcpy(a, base, n);
    CHECK(tracker_shim::memmove(a, a, n) == a);
    CHECK(tracker_shim::memmove(a, a + 1, 0) == a);
    CHECK(std::memcmp(a, base, n) == 0);
  }
}

TEST_CASE("module shim: strncpy copies the string and pads with NULs, and stops at n without a NUL") {
  char dst[8];
  std::memset(dst, 'X', sizeof dst);
  CHECK(tracker_shim::strncpy(dst, "abc", sizeof dst) == dst);
  CHECK(std::strcmp(dst, "abc") == 0);
  for (std::size_t i = 3; i < sizeof dst; ++i) {
    CHECK(dst[i] == '\0');
  }
  std::memset(dst, 'X', sizeof dst);
  CHECK(tracker_shim::strncpy(dst, "abcdefgh", 4) == dst);
  CHECK(std::memcmp(dst, "abcd", 4) == 0);
  CHECK(dst[4] == 'X');  // no NUL past n
  std::memset(dst, 'X', sizeof dst);
  CHECK(tracker_shim::strncpy(dst, "abc", 0) == dst);
  CHECK(dst[0] == 'X');
}
