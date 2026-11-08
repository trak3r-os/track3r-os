// SPDX-License-Identifier: MIT
//
// The portable half of the module libc shim (OS-FLASHDIET I0): the packer refuses an undefined symbol and -ffreestanding still lets GCC emit
// calls to these, so the importer module carries them itself. Plain loops, no libc, no writable global. libc_shim.cpp includes this header and
// gives the functions their extern "C" / __aeabi names (plus the target-only __aeabi_uldivmod asm trampoline); the host test
// tests/module_shim_test.cpp includes it too and checks every function against native 64-bit arithmetic. The __aeabi_* names are what
// arm-none-eabi GCC emits for block copies, 64-bit shifts and 64-bit multiply/divide.
#pragma once

#include <cstddef>
#include <cstdint>

namespace tracker_shim {

inline void* memcpy(void* dst, const void* src, std::size_t n) {
  auto* d = static_cast<std::uint8_t*>(dst);
  const auto* s = static_cast<const std::uint8_t*>(src);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = s[i];
  }
  return dst;
}

inline void* memmove(void* dst, const void* src, std::size_t n) {
  auto* d = static_cast<std::uint8_t*>(dst);
  const auto* s = static_cast<const std::uint8_t*>(src);
  if (d < s) {
    for (std::size_t i = 0; i < n; ++i) {
      d[i] = s[i];
    }
  } else if (d > s) {
    for (std::size_t i = n; i > 0; --i) {
      d[i - 1] = s[i - 1];
    }
  }
  return dst;
}

inline void* memset(void* dst, int c, std::size_t n) {
  auto* d = static_cast<std::uint8_t*>(dst);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = static_cast<std::uint8_t>(c);
  }
  return dst;
}

inline int memcmp(const void* a, const void* b, std::size_t n) {
  const auto* x = static_cast<const std::uint8_t*>(a);
  const auto* y = static_cast<const std::uint8_t*>(b);
  for (std::size_t i = 0; i < n; ++i) {
    if (x[i] != y[i]) {
      return x[i] < y[i] ? -1 : 1;
    }
  }
  return 0;
}

inline std::size_t strlen(const char* s) {
  std::size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

inline int strcmp(const char* a, const char* b) {
  while (*a != '\0' && *a == *b) {
    ++a;
    ++b;
  }
  return static_cast<int>(static_cast<unsigned char>(*a)) - static_cast<int>(static_cast<unsigned char>(*b));
}

inline char* strncpy(char* dst, const char* src, std::size_t n) {
  std::size_t i = 0;
  for (; i < n && src[i] != '\0'; ++i) {
    dst[i] = src[i];
  }
  for (; i < n; ++i) {
    dst[i] = '\0';
  }
  return dst;
}

// 64-bit helpers. A 64-bit variable shift is one __aeabi call on ARM; only the constant shifts of 64-bit values stay
// inline, so every shift below is on a 32-bit value or by a constant.
inline std::uint64_t llsl(std::uint64_t v, int s) {
  const std::uint32_t lo = static_cast<std::uint32_t>(v);
  const std::uint32_t hi = static_cast<std::uint32_t>(v >> 32);
  if (s <= 0) {
    return v;
  }
  if (s >= 64) {
    return 0;
  }
  if (s < 32) {
    const std::uint32_t nlo = lo << s;
    const std::uint32_t nhi = (hi << s) | (lo >> (32 - s));
    return (static_cast<std::uint64_t>(nhi) << 32) | nlo;
  }
  return static_cast<std::uint64_t>(lo << (s - 32)) << 32;
}

inline std::uint64_t llsr(std::uint64_t v, int s) {
  const std::uint32_t lo = static_cast<std::uint32_t>(v);
  const std::uint32_t hi = static_cast<std::uint32_t>(v >> 32);
  if (s <= 0) {
    return v;
  }
  if (s >= 64) {
    return 0;
  }
  if (s < 32) {
    const std::uint32_t nlo = (lo >> s) | (hi << (32 - s));
    const std::uint32_t nhi = hi >> s;
    return (static_cast<std::uint64_t>(nhi) << 32) | nlo;
  }
  return static_cast<std::uint64_t>(hi >> (s - 32));
}

inline std::uint64_t lasr(std::uint64_t v, int s) {
  const std::uint32_t lo = static_cast<std::uint32_t>(v);
  const std::int32_t hi = static_cast<std::int32_t>(v >> 32);
  if (s <= 0) {
    return v;
  }
  if (s >= 64) {
    return hi < 0 ? ~std::uint64_t(0) : std::uint64_t(0);
  }
  const std::uint32_t sign = hi < 0 ? 0xFFFFFFFFu : 0u;
  if (s < 32) {
    const std::uint32_t nlo = (lo >> s) | (static_cast<std::uint32_t>(hi) << (32 - s));
    const std::uint32_t nhi = static_cast<std::uint32_t>(hi >> s);
    return (static_cast<std::uint64_t>(nhi) << 32) | nlo;
  }
  const int t = s - 32;  // 0..31
  const std::uint32_t nlo = t == 0 ? static_cast<std::uint32_t>(hi) : ((static_cast<std::uint32_t>(hi) >> t) | (sign << (32 - t)));
  return (static_cast<std::uint64_t>(sign) << 32) | nlo;
}

// 64x64 -> 64 multiply from 32x32 -> 64 widening products (one umull each on the Cortex-M4).
inline std::uint64_t lmul(std::uint64_t a, std::uint64_t b) {
  const std::uint32_t al = static_cast<std::uint32_t>(a);
  const std::uint32_t ah = static_cast<std::uint32_t>(a >> 32);
  const std::uint32_t bl = static_cast<std::uint32_t>(b);
  const std::uint32_t bh = static_cast<std::uint32_t>(b >> 32);
  const std::uint64_t ll = static_cast<std::uint64_t>(al) * bl;
  const std::uint64_t mid = static_cast<std::uint64_t>(al) * bh + static_cast<std::uint64_t>(ah) * bl;
  return ll + (mid << 32);
}

// The 64/64 division the importer sources use (div_round over uint64). out[0] = quotient, out[1] = remainder; only
// constant shifts of 64-bit values, so no call back into the helpers.
inline void udiv64_impl(std::uint64_t n, std::uint64_t d, std::uint64_t* out) {
  std::uint64_t q = 0;
  std::uint64_t r = 0;
  if (d != 0) {
    std::uint64_t bit = std::uint64_t(1) << 63;
    for (int i = 0; i < 64; ++i) {
      r = (r << 1) | ((n & bit) != 0 ? std::uint64_t(1) : std::uint64_t(0));
      bit >>= 1;
      q <<= 1;
      if (r >= d) {
        r -= d;
        q |= 1;
      }
    }
  }
  out[0] = q;
  out[1] = r;
}

}  // namespace tracker_shim
