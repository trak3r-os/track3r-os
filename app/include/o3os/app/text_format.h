// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tiny formatting helpers: no <cstdio>, no heap, no locale.
//   put_*     write into a caller-provided buffer the caller has sized for the fixed widths it asks for, return
//             the pointer past the last character and write no terminator. Only for buffers of known size
//             (field text); anything composed from variable parts uses a core::FixedString and append_dec.
//   append_dec  bounds-checked: truncates like FixedString does.

#include <o3os/core/fixed_string.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// Exactly `digits` decimal digits, zero padded; a larger value keeps its low digits. One out-of-line copy
// (text_format.cpp, OS-FLASHDIET T2), not one per call site.
char* put_dec(char* dst, unsigned value, unsigned digits) noexcept;

// Two upper-case hexadecimal digits.
inline char* put_hex2(char* dst, unsigned value) noexcept {
  static const char kDigits[] = "0123456789ABCDEF";
  dst[0] = kDigits[(value >> 4) & 0xFu];
  dst[1] = kDigits[value & 0xFu];
  return dst + 2;
}

// Copies a NUL-terminated string without its terminator.
inline char* put_str(char* dst, const char* s) noexcept {
  while (*s != '\0') {
    *dst++ = *s++;
  }
  return dst;
}

namespace detail {

// The one implementation behind every FixedString<N> wrapper below (text_format.cpp, OS-FLASHDIET T2), whatever
// N: appends at buf[len], never past cap (buf holds cap + 1 bytes and buf[len] stays '\0'), false when the text
// was cut. append_dec writes exactly `digits` zero-padded digits (at most 10; a larger value keeps its low
// digits); append_uint as many digits as `value` has.
bool append_dec(char* buf, std::size_t cap, std::size_t& len, unsigned value, unsigned digits) noexcept;
bool append_uint(char* buf, std::size_t cap, std::size_t& len, unsigned value) noexcept;

}  // namespace detail

// Appends `digits` (at most 10) zero-padded decimal digits to `s`; false if the text was cut.
// (OS-FLASHDIET P2: a forwarder over the one out-of-line writer, written straight into the string; it was a copy per capacity, 40 bytes each.)
template <std::size_t N>
[[gnu::always_inline]] inline bool append_dec(core::FixedString<N>& s, unsigned value, unsigned digits) noexcept {
  return detail::append_dec(core::detail::FixedStringAccess::buf(s), N, core::detail::FixedStringAccess::len(s), value, digits);
}

// Appends the decimal digits of `value`, as many as it has (no padding); false if the text was cut. (append_dec pads to a fixed width and
// keeps the low digits of a larger number, which is what a field wants and a count does not.)
template <std::size_t N>
[[gnu::always_inline]] inline bool append_uint(core::FixedString<N>& s, unsigned value) noexcept {
  return detail::append_uint(core::detail::FixedStringAccess::buf(s), N, core::detail::FixedStringAccess::len(s), value);
}

// OS-QAFIX-RESCAN: the strip line after a list was read again: "READ AGAIN: 12 ITEMS", "READ AGAIN: 1 ITEM", "READ AGAIN: NO ITEMS" (`more`: the listing
// was cut at its limit, "100+ ITEMS"). `one` and `many` are the unit, upper case, in the singular and the plural.
template <std::size_t N>
void rescan_text(core::FixedString<N>& s, unsigned count, bool more, const char* one, const char* many) noexcept {
  s.assign("READ AGAIN: ");
  if (count == 0) {
    s.append("NO ");
    s.append(many);
    return;
  }
  append_uint(s, count);
  s.append(more ? "+ " : " ");
  s.append(count == 1 && !more ? one : many);
}

// Appends a tempo (BPM x 100) as the strip shows it: one decimal, rounded, five cells wide so that the strip does not move ("128.0", " 40.0", "800.0").
template <std::size_t N>
bool append_bpm(core::FixedString<N>& s, unsigned bpm_x100) noexcept {
  const unsigned tenths = ((bpm_x100 < 99900u ? bpm_x100 : 99900u) + 5u) / 10u;  // 12055 -> 1206; at most 999.0
  bool ok = tenths >= 1000u || s.append(tenths >= 100u ? " " : "  ");
  ok = append_uint(s, tenths / 10u) && ok;
  ok = s.append(".") && ok;
  return append_uint(s, tenths % 10u) && ok;
}

// OS-AUDIT-A3: the sample memory as the sample screens and Config show it: int16 samples as kibibytes, rounded down (512 samples = 1 KB).
constexpr std::uint32_t sample_kb(std::uint32_t samples) noexcept { return samples / 512u; }

// "4908 KB FREE": the free part of the sample memory.
template <std::size_t N>
bool append_free_kb(core::FixedString<N>& s, std::uint32_t free_samples) noexcept {
  const bool ok = append_uint(s, sample_kb(free_samples));
  return s.append(" KB FREE") && ok;
}

// "3284 KB USED  4908 KB FREE": `total` is the whole sample memory; a free part above it counts as all free.
template <std::size_t N>
bool append_used_free_kb(core::FixedString<N>& s, std::uint32_t total, std::uint32_t free_samples) noexcept {
  const bool ok = append_uint(s, sample_kb(free_samples < total ? total - free_samples : 0u));
  return s.append(" KB USED  ") && append_free_kb(s, free_samples) && ok;
}

// Appends at most `max` characters of the NUL-terminated `text` (never past its end).
template <std::size_t N>
bool append_clipped(core::FixedString<N>& s, const char* text, std::size_t max) noexcept {
  std::size_t n = 0;
  while (n < max && text[n] != '\0') {
    ++n;
  }
  return s.append(text, n);
}

}  // namespace o3os::app
