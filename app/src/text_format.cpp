// SPDX-License-Identifier: GPL-3.0-or-later

// The out-of-line text helpers of text_format.h (OS-FLASHDIET T2): they were inline in the header, so every
// instantiation of the FixedString<N> wrappers and every call site linked its own copy of the loops.

#include <o3os/app/text_format.h>

namespace o3os::app {

char* put_dec(char* dst, unsigned value, unsigned digits) noexcept {
  for (unsigned i = digits; i > 0; --i) {
    dst[i - 1] = static_cast<char>('0' + value % 10u);
    value /= 10u;
  }
  return dst + digits;
}

namespace detail {

bool append_dec(char* buf, std::size_t cap, std::size_t& len, unsigned value, unsigned digits) noexcept {
  char tmp[10];
  if (digits > sizeof(tmp)) {
    digits = sizeof(tmp);
  }
  put_dec(tmp, value, digits);
  return core::detail::string_append_n(buf, cap, len, tmp, digits);
}

bool append_uint(char* buf, std::size_t cap, std::size_t& len, unsigned value) noexcept {
  unsigned digits = 1;
  for (unsigned v = value; v >= 10u; v /= 10u) {
    ++digits;
  }
  return append_dec(buf, cap, len, value, digits);
}

}  // namespace detail

}  // namespace o3os::app
