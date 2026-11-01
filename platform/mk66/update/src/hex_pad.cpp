// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/mk66/update/hex_pad.h>

namespace o3os::platform::mk66::update {
namespace {

// One record of `n` bytes at `dst`: header, the data (zeros: only the base record has a payload, in `hi`/`lo`), the checksum, LF. Returns its length.
std::size_t put_line(std::uint8_t* dst, unsigned count, unsigned offset, unsigned type, unsigned hi, unsigned lo) noexcept {
  static const char kDigits[] = "0123456789ABCDEF";
  std::size_t n = 0;
  unsigned sum = 0;
  const auto byte = [&](unsigned b) noexcept {
    b &= 0xFFu;
    sum += b;
    dst[n++] = static_cast<std::uint8_t>(kDigits[b >> 4]);
    dst[n++] = static_cast<std::uint8_t>(kDigits[b & 0xFu]);
  };
  dst[n++] = ':';
  byte(count);
  byte(offset >> 8);
  byte(offset);
  byte(type);
  if (type == 0x04u) {
    byte(hi);
    byte(lo);
  } else {
    for (unsigned i = 0; i < count; ++i) {
      byte(0);
    }
  }
  const unsigned check = (0u - sum) & 0xFFu;
  dst[n++] = static_cast<std::uint8_t>(kDigits[check >> 4]);
  dst[n++] = static_cast<std::uint8_t>(kDigits[check & 0xFu]);
  dst[n++] = '\n';
  return n;
}

}  // namespace

void HexPadWriter::begin(std::uint32_t length) noexcept {
  at_ = length;
  end_ = length + pad_bytes(length);
  based_ = at_ == end_;  // nothing to write on a boundary: the extended record is not needed either
}

std::size_t HexPadWriter::next(std::uint8_t* dst) noexcept {
  if (at_ == end_) {
    return 0;
  }
  if (!based_) {
    based_ = true;
    return put_line(dst, 2, 0, 0x04u, at_ >> 24, at_ >> 16);
  }
  const std::uint32_t left = end_ - at_;
  const unsigned count = left < 16u ? static_cast<unsigned>(left) : 16u;
  const std::size_t n = put_line(dst, count, at_ & 0xFFFFu, 0x00u, 0, 0);
  at_ += count;
  return n;
}

}  // namespace o3os::platform::mk66::update
