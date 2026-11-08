// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A streaming Intel HEX validator for the firmware writer (OS-006c): the same rules as tools/check_image.py `check`, run on the device on the bytes
// as they arrive and again on the bytes read back from the card, before the bootloader is ever asked to flash them. Fed bytes in any chunking (a
// state machine over single bytes: the chunk boundaries cannot matter, a test feeds every split of an image), it keeps O(1) state besides the first
// 0x410 bytes of the image (the vector table and the flash configuration field), and reports the length of the image and the CRC-32 of the file.
//
// The rules, each of which is a verdict of check_image.py or a stricter form of one:
//   * record syntax: ':' then hex digits in pairs; byte count, offset, type, data, checksum (the sum of every byte of the record is 0 mod 256); the line
//     ends with LF or CR LF; blank lines are fine; types 00, 01, 02, 04 only (no start-address record: 03 and 05 are refused:
//     the bootloader's handling of one is unknown); an EOF record (01, count 0) ends the data and only line ends may follow;
//   * STRICTER THAN check_image.py in one respect, because O(1) state cannot remember every address: data bytes must arrive at ascending, consecutive
//     addresses starting at 0 (every record starts where the last one ended). That is what `objcopy -O ihex` writes; a HEX that check_image accepts
//     with its records in another order is refused here (`HexOrder`). It also makes "no byte given twice" and "no hole" one check;
//   * the image ends on a 4 KiB boundary (length a multiple of 4096): the check_image.py `sector_end` rule (OS-GATE-BOOTCRASH). The SD bootloader
//     leaves a short final sector as it was, so an unpadded image would boot with stale bytes in its tail (OS-BUG-COSMBOOT). Tested last (`NotPadded`);
//   * the image ends at or below 0xFF000 (`LastSector`): the last 4 KiB flash sector stays empty, the check_image.py `last_sector_free` rule. An image that
//     filled it (end 0x100000) hung the unit after the update (OS-BUG-LASTSECTOR; cause open). No waiver on the device. Tested before `NotPadded`, so an
//     unpadded image that already reaches that sector is not offered a pad that would only end there;
//   * every address is <= 0x000FFFFF (1 MiB of flash), and the image reaches at least 0x410;
//   * initial SP exactly 0x20030000; reset vector a Thumb address inside the image and past the flash configuration (>= 0x410);
//   * the 116 vector entries (16 system, 100 device): the reserved slots 7-10 and 13 are 0, every other entry is a Thumb address inside the image past
//     0x410, or inside the SRAM_L code window (0x1FFF0000, 0xC000: the audio ISRs run from there, copied from flash at reset) at an offset inside the image;
//   * the flash configuration field, 0x400..0x40F, is exactly ffffffffffffffffffffffffdef9ffff and FSEC & 3 == 2. A wrong field can lock the MCU for
//     good. The constant below is this file's own copy; static_asserts pin it to flash_config.h, and check_image.py, the mk66_flashconfig ctest and
//     the flashing-safety rules hold the others.
//
// `tests/mk66_update_hex_test.cpp` checks the verdicts against the rules above (every record defect, every byte of the field, holes, SP, reset and
// vector cases, every chunking) and a host cross-test runs this class and check_image.py on the same files and requires the same verdicts.

#include <o3os/platform/mk66/update/crc32.h>
#include <o3os/platform/mk66/update/fault.h>

#include <cstddef>
#include <cstdint>

namespace o3os::platform::mk66::update {

inline constexpr std::uint32_t kFlashLastAddress = 0x000FFFFFu;
inline constexpr std::uint32_t kHeadBytes = 0x410;  // the vector table, the flash configuration field and the byte after it
inline constexpr std::uint32_t kConfigAddress = 0x400;
inline constexpr unsigned kConfigBytes = 16;
inline constexpr unsigned kVectorEntries = 116;
inline constexpr std::uint32_t kRequiredSp = 0x20030000u;
inline constexpr std::uint32_t kRamfuncBase = 0x1FFF0000u;  // the SRAM_L code window (pinned to mem_map.h by hex_validator.cpp's static_asserts)
inline constexpr std::uint32_t kRamfuncSize = 0xC800u;  // 50 KiB since the module slab went to 14 KiB (OS-PERF3; 49 at 15 KiB); hot code sits in the first 27 KiB, so the validator accepts the images of the older sizes too
inline constexpr std::uint32_t kMaxImageBytes = kFlashLastAddress + 1u;
inline constexpr std::uint32_t kFlashSector = 4096u;  // the SD bootloader leaves a short final sector neither erased nor programmed (OS-BUG-COSMBOOT): the image must end on a boundary
inline constexpr std::uint32_t kLastSectorStart = 0x000FF000u;  // an image ends (last address + 1) at or below this: the last flash sector stays empty (OS-BUG-LASTSECTOR)
inline constexpr std::uint8_t kRequiredConfig[kConfigBytes] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xF9, 0xFF, 0xFF};

class HexValidator {
 public:
  HexValidator() noexcept { reset(); }

  // Starts a new stream (also after a failure).
  void reset() noexcept;
  // Feeds bytes of the file. False as soon as the stream is invalid (the verdict is sticky: later bytes are ignored); true means "so far fine".
  bool feed(const std::uint8_t* data, std::size_t n) noexcept;
  // The end of the file: the last line, the EOF record, and every check of the finished image. True: the image is acceptable.
  bool finish() noexcept;

  bool ok() const noexcept { return fault_ == Fault::Ok; }
  Fault fault() const noexcept { return fault_; }
  // The offset in the stream (0-based) of the byte at which the fault was found (the length of the stream for a fault found at the end).
  std::uint32_t fault_offset() const noexcept { return fault_at_; }
  std::uint32_t consumed() const noexcept { return consumed_; }
  // The image's length in bytes (the next address): valid after finish() returned true.
  std::uint32_t image_length() const noexcept { return next_; }
  // CRC-32 of the bytes fed so far (the whole file after the last feed).
  std::uint32_t crc() const noexcept { return crc_.value(); }

 private:
  enum class State : std::uint8_t { LineStart, InRecord };

  void step(std::uint8_t c) noexcept;
  void byte_in(std::uint8_t b) noexcept;
  void data_byte(std::uint32_t index, std::uint8_t b) noexcept;
  void end_of_record() noexcept;
  void fail(Fault f) noexcept;
  std::uint32_t head_word(unsigned index) const noexcept;

  State state_ = State::LineStart;
  bool cr_ = false;       // a CR was seen on this line: only LF or another CR may follow
  bool have_hi_ = false;  // the first digit of a byte is in nibble_
  std::uint8_t nibble_ = 0;
  std::uint16_t pos_ = 0;  // byte index within the record: 0 count, 1-2 offset, 3 type, 4.. data, then the checksum
  std::uint8_t count_ = 0;
  std::uint8_t type_ = 0;
  std::uint8_t sum_ = 0;
  std::uint16_t offset_ = 0;
  std::uint16_t datum_ = 0;  // the two data bytes of an 02 or 04 record
  std::uint32_t base_ = 0;
  std::uint32_t next_ = 0;   // the address the next data byte must have
  bool eof_ = false;
  Fault fault_ = Fault::Ok;
  std::uint32_t fault_at_ = 0;
  std::uint32_t consumed_ = 0;
  Crc32 crc_;
  std::uint8_t head_[kHeadBytes] = {};
};

}  // namespace o3os::platform::mk66::update
