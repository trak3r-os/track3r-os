// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace o3os::core {

namespace detail {

// Shared implementation behind every FixedString<N>: one copy of the code regardless of capacity.
// `buf` holds cap + 1 bytes; `len` <= cap; buf[len] == '\0' on entry and exit.
// The appends return true when everything fitted; otherwise the text is cut at capacity (still
// NUL-terminated). append_n copies exactly n bytes from `src` (a null `src` is an empty string);
// append_z reads a NUL-terminated string and never looks further than the room left plus one byte.
// equals_z compares `a` (a_len bytes) with the NUL-terminated `z` without reading past its terminator.
bool string_append_n(char* buf, std::size_t cap, std::size_t& len, const char* src, std::size_t n) noexcept;
bool string_append_z(char* buf, std::size_t cap, std::size_t& len, const char* src) noexcept;
bool string_equals_z(const char* a, std::size_t a_len, const char* z) noexcept;

// For the out-of-line writers that take (buf, cap, len) (app/text_format.h, app/fx_text.h; OS-FLASHDIET P2): a FixedString<N>'s storage without N, so
// a formatter is one function whatever the capacity. A writer keeps the contract above: len <= cap, buf[len] == '\0'.
struct FixedStringAccess;

}  // namespace detail

// String with a compile-time capacity of N characters (plus the terminator) and inline storage:
// no heap, no exceptions. Text that does not fit is truncated and reported through the bool result,
// c_str() is always NUL-terminated. Not thread-safe.
template <std::size_t N>
class FixedString {
 public:
  static constexpr std::size_t capacity() noexcept { return N; }
  std::size_t size() const noexcept { return len_; }
  bool empty() const noexcept { return len_ == 0; }
  const char* c_str() const noexcept { return buf_; }
  const char* data() const noexcept { return buf_; }
  // Precondition: i < size() (unchecked).
  char operator[](std::size_t i) const noexcept { return buf_[i]; }

  void clear() noexcept {
    len_ = 0;
    buf_[0] = '\0';
  }

  bool assign(const char* s) noexcept {
    clear();
    return append(s);
  }
  bool append(const char* s) noexcept { return detail::string_append_z(buf_, N, len_, s); }
  bool append(const char* s, std::size_t n) noexcept { return detail::string_append_n(buf_, N, len_, s, n); }

  bool operator==(const char* s) const noexcept { return s != nullptr && detail::string_equals_z(buf_, len_, s); }
  bool operator!=(const char* s) const noexcept { return !(*this == s); }

 private:
  friend struct detail::FixedStringAccess;
  char buf_[N + 1] = {};
  std::size_t len_ = 0;
};

namespace detail {

struct FixedStringAccess {
  template <std::size_t N>
  static char* buf(FixedString<N>& s) noexcept { return s.buf_; }
  template <std::size_t N>
  static std::size_t& len(FixedString<N>& s) noexcept { return s.len_; }
};

}  // namespace detail

}  // namespace o3os::core
