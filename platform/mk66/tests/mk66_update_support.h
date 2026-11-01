// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test support for the firmware writer (OS-006c), header-only so the host and QEMU suites use the same code: a good MK66 image as bytes (what check_image.py's
// self-test calls good_segments: the vector table, the flash configuration field, some code), its Intel HEX text as objcopy writes it (16-byte records,
// an extended linear address record at each 64 KiB), and a few helpers to break either.
#include <o3os/platform/mk66/update/hex_validator.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace upd_test {

namespace up = o3os::platform::mk66::update;

inline void put_word(std::vector<std::uint8_t>& image, std::size_t at, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    image[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
}

// A valid image of `code_bytes` bytes of code after the flash configuration field: SP 0x20030000, reset 0x501, every other vector 0x511, the reserved
// ones 0, the field the required 16 bytes. Length 0x410 + code_bytes, padded with zeros to the next 4 KiB boundary like the linker's .flashpad
// (the validator refuses any other end: Fault::NotPadded); `pad` false leaves the exact length for the tests of that rule.
inline std::vector<std::uint8_t> good_image(std::size_t code_bytes = 512, bool pad = true) {
  std::size_t length = 0x410 + code_bytes;
  if (pad) {
    length = (length + up::kFlashSector - 1u) / up::kFlashSector * up::kFlashSector;
  }
  std::vector<std::uint8_t> image(length, 0);
  std::fill(image.begin(), image.begin() + 0x410 + static_cast<std::ptrdiff_t>(code_bytes), std::uint8_t{0xFF});
  put_word(image, 0, 0x20030000u);
  put_word(image, 4, 0x00000501u);
  for (unsigned i = 2; i < up::kVectorEntries; ++i) {
    const bool reserved = i == 7 || i == 8 || i == 9 || i == 10 || i == 13;
    put_word(image, i * 4u, reserved ? 0u : 0x00000511u);
  }
  for (unsigned i = 0; i < up::kConfigBytes; ++i) {
    image[0x400 + i] = up::kRequiredConfig[i];
  }
  for (std::size_t i = 0; i < code_bytes; ++i) {
    image[0x410 + i] = static_cast<std::uint8_t>(i * 7u + 1u);
  }
  return image;
}

inline std::string record(unsigned type, unsigned offset, const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> body = {static_cast<std::uint8_t>(data.size()), static_cast<std::uint8_t>(offset >> 8), static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(type)};
  body.insert(body.end(), data.begin(), data.end());
  unsigned sum = 0;
  for (const std::uint8_t b : body) {
    sum += b;
  }
  body.push_back(static_cast<std::uint8_t>(-static_cast<int>(sum)));
  std::string s = ":";
  char two[3];
  for (const std::uint8_t b : body) {
    std::snprintf(two, sizeof two, "%02X", b);
    s += two;
  }
  return s;
}

// The HEX text of `image` starting at address 0, with line end `eol`, records of `per` bytes; `eof` false leaves the EOF record out.
inline std::string to_hex(const std::vector<std::uint8_t>& image, const char* eol = "\n", unsigned per = 16, bool eof = true) {
  std::string text;
  std::uint32_t upper = 0xFFFFFFFFu;
  for (std::size_t at = 0; at < image.size(); at += per) {
    const std::size_t n = image.size() - at < per ? image.size() - at : per;
    if ((at >> 16) != upper) {
      upper = static_cast<std::uint32_t>(at >> 16);
      text += record(4, 0, {static_cast<std::uint8_t>(upper >> 8), static_cast<std::uint8_t>(upper)}) + eol;
    }
    text += record(0, static_cast<unsigned>(at & 0xFFFF), std::vector<std::uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(at), image.begin() + static_cast<std::ptrdiff_t>(at + n))) + eol;
  }
  if (eof) {
    text += record(1, 0, {}) + eol;
  }
  return text;
}

// The file the update path's pad (OS-UPDPAD) must write for the unpadded image `image`: the records of to_hex(image) up to the EOF record, one extended linear address record for the
// image's end, zero records of 16 bytes (the last one shorter) to the next 4 KiB boundary with LF line ends, then the EOF record. Built from record(), not from HexPadWriter, so the writer
// is tested against it. A length on a boundary gets no padding records.
inline std::string expected_padded_hex(const std::vector<std::uint8_t>& image, const char* eol = "\n") {
  std::string text = to_hex(image, eol, 16, false);
  const std::size_t length = image.size();
  const std::size_t end = (length + up::kFlashSector - 1u) / up::kFlashSector * up::kFlashSector;
  if (end != length) {
    text += record(4, 0, {static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16)}) + "\n";
    for (std::size_t at = length; at < end; at += 16) {
      const std::size_t n = end - at < 16 ? end - at : 16;
      text += record(0, static_cast<unsigned>(at & 0xFFFF), std::vector<std::uint8_t>(n, 0)) + "\n";
    }
  }
  return text + record(1, 0, {}) + eol;
}

struct Verdict {
  bool ok;
  up::Fault fault;
  std::uint32_t offset;
  std::uint32_t length;
  std::uint32_t crc;
};

// Runs a fresh validator over `text`, fed in pieces of `chunk` bytes (0: all at once).
inline Verdict validate(const std::string& text, std::size_t chunk = 0) {
  up::HexValidator v;
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(text.data());
  if (chunk == 0 || chunk > text.size()) {
    chunk = text.empty() ? 1 : text.size();
  }
  for (std::size_t at = 0; at < text.size() && v.ok(); at += chunk) {
    (void)v.feed(p + at, text.size() - at < chunk ? text.size() - at : chunk);
  }
  const bool ok = v.finish();
  return Verdict{ok, v.fault(), v.fault_offset(), ok ? v.image_length() : 0u, v.crc()};
}

inline bool accepts(const std::string& text) { return validate(text).ok; }

}  // namespace upd_test
