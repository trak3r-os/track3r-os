// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The padding of an unpadded image (OS-UPDPAD): the Intel HEX text that extends an image to the next 4 KiB boundary with zero bytes (what the linker's `.flashpad` does for our own
// builds), one line a call, so the update path can write it in slices without a buffer of its own. Pure.
//
// The text, for an image that ends at `length` (the next address; not a multiple of 4096): one extended linear address record for the upper 16 bits of `length`
// (`:02000004HHHHCC`; always written, so the base the original left active does not matter), then data records of 16 zero bytes (the last one shorter) at ascending addresses
// up to the boundary. Upper-case hex, LF line ends. The 4096 boundary divides 65536, so the padding stays inside one 64 KiB segment. A length on a boundary has no padding at all.

#include <o3os/platform/mk66/update/hex_validator.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::update {

inline constexpr std::size_t kPadLineMax = 44;  // ':' + count, offset, type (8) + 16 data bytes (32) + checksum (2) + LF

class HexPadWriter {
 public:
  // The zero bytes that take an image of `length` bytes to the next boundary (0 on one).
  static constexpr std::uint32_t pad_bytes(std::uint32_t length) noexcept { return (kFlashSector - length % kFlashSector) % kFlashSector; }
  // The size of the padding text for an image of `length` bytes (0 on a boundary).
  static constexpr std::uint32_t text_bytes(std::uint32_t length) noexcept {
    const std::uint32_t pad = pad_bytes(length);
    return pad == 0u ? 0u : 16u + (pad / 16u) * static_cast<std::uint32_t>(kPadLineMax) + (pad % 16u != 0u ? 12u + 2u * (pad % 16u) : 0u);
  }

  void begin(std::uint32_t length) noexcept;
  // The next line into `dst` (room for kPadLineMax bytes); its length, or 0 when the padding is complete.
  std::size_t next(std::uint8_t* dst) noexcept;

 private:
  std::uint32_t at_ = 0;   // the address of the next data byte
  std::uint32_t end_ = 0;  // the boundary
  bool based_ = true;      // the extended record has been written
};

}  // namespace o3os::platform::mk66::update
