// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/platform/mk66/update/hex_validator.h>

#include <o3os/platform/mk66/flash_config.h>
#include <o3os/platform/mk66/mem_map.h>

namespace o3os::platform::mk66::update {

// The required flash configuration field: this file's copy of the bytes, pinned to the header's (flash_config.h) at compile time.
namespace {
constexpr bool config_matches_header() noexcept {
  const FlashConfig& c = kFlashConfig;
  for (unsigned i = 0; i < 8; ++i) {
    if (kRequiredConfig[i] != c.backdoor_key[i]) {
      return false;
    }
  }
  for (unsigned i = 0; i < 4; ++i) {
    if (kRequiredConfig[8 + i] != c.fprot[i]) {
      return false;
    }
  }
  return kRequiredConfig[12] == c.fsec && kRequiredConfig[13] == c.fopt && kRequiredConfig[14] == c.feprot && kRequiredConfig[15] == c.fdprot;
}
static_assert(config_matches_header(), "the validator's flash configuration bytes are flash_config.h's");
static_assert(kFlashSector == mem::kFlashSectorSize, "the padding rule is the flash sector of mem_map.h");
static_assert(kLastSectorStart == kMaxImageBytes - kFlashSector, "the last flash sector is the one that stays empty");
static_assert((kRequiredConfig[12] & 0x03u) == 0x02u, "FSEC & 3 == 2 (unsecured)");
static_assert(kConfigAddress + kConfigBytes == 0x410u && kHeadBytes == 0x410u, "the head holds the whole flash configuration field");
static_assert(kRamfuncBase == mem::kSramLBase && kRamfuncSize == mem::kRamfuncLSize, "the SRAM_L code window is mem_map.h's");
static_assert(kVectorEntries * 4u <= kConfigAddress, "the vector table sits below the flash configuration field");

int hex_value(std::uint8_t c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

bool reserved_vector(unsigned index) noexcept { return index == 7u || index == 8u || index == 9u || index == 10u || index == 13u; }
}  // namespace

void HexValidator::reset() noexcept {
  state_ = State::LineStart;
  cr_ = false;
  have_hi_ = false;
  nibble_ = 0;
  pos_ = 0;
  count_ = 0;
  type_ = 0;
  sum_ = 0;
  offset_ = 0;
  datum_ = 0;
  base_ = 0;
  next_ = 0;
  eof_ = false;
  fault_ = Fault::Ok;
  fault_at_ = 0;
  consumed_ = 0;
  crc_.reset();
  for (std::uint8_t& b : head_) {
    b = 0;
  }
}

void HexValidator::fail(Fault f) noexcept {
  if (fault_ == Fault::Ok) {
    fault_ = f;
    fault_at_ = consumed_;
  }
}

bool HexValidator::feed(const std::uint8_t* data, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n && fault_ == Fault::Ok; ++i) {
    crc_.add(data[i]);
    step(data[i]);
    if (fault_ == Fault::Ok) {
      ++consumed_;
    }
  }
  return fault_ == Fault::Ok;
}

void HexValidator::step(std::uint8_t c) noexcept {
  if (state_ == State::LineStart) {
    if (c == ':') {
      if (eof_) {
        fail(Fault::HexAfterEof);
      } else if (cr_) {
        fail(Fault::HexChar);  // a CR that is not followed by LF
      } else {
        state_ = State::InRecord;
        pos_ = 0;
        sum_ = 0;
        have_hi_ = false;
      }
    } else if (c == '\r') {
      cr_ = true;
    } else if (c == '\n') {
      cr_ = false;
    } else {
      fail(eof_ ? Fault::HexAfterEof : Fault::HexChar);
    }
    return;
  }
  // inside a record
  const int digit = hex_value(c);
  if (digit >= 0) {
    if (!have_hi_) {
      nibble_ = static_cast<std::uint8_t>(digit);
      have_hi_ = true;
    } else {
      have_hi_ = false;
      byte_in(static_cast<std::uint8_t>((nibble_ << 4) | static_cast<unsigned>(digit)));
    }
    return;
  }
  if (c == '\r' || c == '\n') {
    if (have_hi_) {
      fail(Fault::HexDigits);
    } else if (pos_ < 5u || pos_ != static_cast<unsigned>(count_) + 5u) {
      fail(Fault::HexRecord);
    } else {
      end_of_record();
      state_ = State::LineStart;
      cr_ = c == '\r';
    }
    return;
  }
  fail(Fault::HexChar);
}

void HexValidator::byte_in(std::uint8_t b) noexcept {
  sum_ = static_cast<std::uint8_t>(sum_ + b);
  if (pos_ >= 4u && pos_ > 4u + count_) {
    fail(Fault::HexRecord);  // more bytes than the count says
    return;
  }
  switch (pos_) {
    case 0:
      count_ = b;
      break;
    case 1:
      offset_ = static_cast<std::uint16_t>(b << 8);
      break;
    case 2:
      offset_ = static_cast<std::uint16_t>(offset_ | b);
      break;
    case 3:
      type_ = b;
      if (b == 0x00u) {
        break;
      }
      if (b == 0x01u) {
        if (count_ != 0u) {
          fail(Fault::HexRecord);  // an EOF record with data
        }
      } else if (b == 0x02u || b == 0x04u) {
        if (count_ != 2u) {
          fail(Fault::HexRecord);
        }
      } else if (b == 0x03u || b == 0x05u) {
        fail(Fault::HexStart);
      } else {
        fail(Fault::HexType);
      }
      break;
    default:
      if (pos_ < 4u + count_) {
        const unsigned index = pos_ - 4u;
        if (type_ == 0x00u) {
          data_byte(index, b);
        } else if (index == 0u) {
          datum_ = static_cast<std::uint16_t>(b << 8);
        } else {
          datum_ = static_cast<std::uint16_t>(datum_ | b);
        }
      } else if (sum_ != 0u) {  // the checksum byte: the sum of the whole record is 0 mod 256
        fail(Fault::HexChecksum);
      }
      break;
  }
  ++pos_;
}

void HexValidator::data_byte(std::uint32_t index, std::uint8_t b) noexcept {
  const std::uint64_t address = static_cast<std::uint64_t>(base_) + offset_ + index;
  if (address > kFlashLastAddress) {
    fail(Fault::HexRange);
    return;
  }
  if (address != next_) {
    fail(Fault::HexOrder);
    return;
  }
  if (next_ < kHeadBytes) {
    head_[next_] = b;
  }
  ++next_;
}

void HexValidator::end_of_record() noexcept {
  if (type_ == 0x01u) {
    eof_ = true;
  } else if (type_ == 0x02u) {
    base_ = static_cast<std::uint32_t>(datum_) << 4;
  } else if (type_ == 0x04u) {
    base_ = static_cast<std::uint32_t>(datum_) << 16;
  }
}

std::uint32_t HexValidator::head_word(unsigned index) const noexcept {
  const unsigned at = index * 4u;
  return static_cast<std::uint32_t>(head_[at]) | (static_cast<std::uint32_t>(head_[at + 1]) << 8) | (static_cast<std::uint32_t>(head_[at + 2]) << 16) |
         (static_cast<std::uint32_t>(head_[at + 3]) << 24);
}

bool HexValidator::finish() noexcept {
  if (fault_ != Fault::Ok) {
    return false;
  }
  if (state_ == State::InRecord) {  // the last line has no line end
    if (have_hi_) {
      fail(Fault::HexDigits);
    } else if (pos_ < 5u || pos_ != static_cast<unsigned>(count_) + 5u) {
      fail(Fault::HexRecord);
    } else {
      end_of_record();
      state_ = State::LineStart;
    }
    if (fault_ != Fault::Ok) {
      return false;
    }
  }
  if (!eof_) {
    fail(Fault::HexNoEof);
    return false;
  }
  if (next_ < kHeadBytes) {
    fail(Fault::HexShort);
    return false;
  }
  for (unsigned i = 0; i < kConfigBytes; ++i) {
    if (head_[kConfigAddress + i] != kRequiredConfig[i]) {
      fail(Fault::BadConfig);
      return false;
    }
  }
  if ((head_[kConfigAddress + 12] & 0x03u) != 0x02u) {
    fail(Fault::BadConfig);
    return false;
  }
  if (head_word(0) != kRequiredSp) {
    fail(Fault::BadSp);
    return false;
  }
  const std::uint32_t reset = head_word(1);
  const std::uint32_t reset_target = reset & ~1u;
  if ((reset & 1u) == 0u || reset_target < kHeadBytes || reset_target >= next_) {
    fail(Fault::BadReset);
    return false;
  }
  for (unsigned index = 2; index < kVectorEntries; ++index) {
    const std::uint32_t entry = head_word(index);
    if (reserved_vector(index)) {
      if (entry != 0u) {
        fail(Fault::BadVector);
        return false;
      }
      continue;
    }
    const std::uint32_t target = entry & ~1u;
    const bool in_window = target >= kRamfuncBase && target - kRamfuncBase < kRamfuncSize;
    bool good = (entry & 1u) != 0u;
    if (good && in_window) {
      good = target - kRamfuncBase < next_;  // the hot code's load image is part of the image
    } else if (good) {
      good = target >= kHeadBytes && target < next_;
    }
    if (!good) {
      fail(Fault::BadVector);
      return false;
    }
  }
  if (next_ > kLastSectorStart) {  // check_image.py `last_sector_free`: the unit hung after an update with an image that filled the last sector (OS-BUG-LASTSECTOR)
    fail(Fault::LastSector);
    return false;
  }
  if (next_ % kFlashSector != 0u) {  // last the check_image.py `sector_end` rule, after every check that says what is wrong with the image itself
    fail(Fault::NotPadded);
    return false;
  }
  return true;
}

}  // namespace o3os::platform::mk66::update
