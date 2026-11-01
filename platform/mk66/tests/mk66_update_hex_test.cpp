// SPDX-License-Identifier: GPL-3.0-or-later
//
// The streaming Intel HEX validator (update/hex_validator.h, OS-006c): the verdicts of tools/check_image.py on the device, and the CRC-32 it reports.
// Every record defect, every byte of the flash configuration field, holes, SP / reset / vector cases, the SRAM_L code window, and every chunking of
// the stream. The cross-check against check_image.py itself (the same files, the same verdicts) is ctest `mk66_fw_hex_cross` (host). Pure; runs on the
// host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"
#include "mk66_update_support.h"

#include <o3os/platform/mk66/update/crc32.h>
#include <o3os/platform/mk66/update/hex_pad.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace up = o3os::platform::mk66::update;
using namespace upd_test;
using up::Fault;

namespace {

std::string hex_of(const std::vector<std::uint8_t>& image) { return to_hex(image); }

std::vector<std::uint8_t> with_byte(std::size_t at, std::uint8_t value) {
  std::vector<std::uint8_t> image = good_image();
  image[at] = value;
  return image;
}

std::vector<std::uint8_t> with_word(std::size_t at, std::uint32_t value) {
  std::vector<std::uint8_t> image = good_image();
  put_word(image, at, value);
  return image;
}

}  // namespace

TEST_CASE("crc32: the zlib value, and the same in any chunking") {
  up::Crc32 c;
  c.add(reinterpret_cast<const std::uint8_t*>("123456789"), 9);
  CHECK(c.value() == 0xCBF43926u);
  up::Crc32 empty;
  CHECK(empty.value() == 0u);
  up::Crc32 split;
  split.add(reinterpret_cast<const std::uint8_t*>("1234"), 4);
  split.add(reinterpret_cast<const std::uint8_t*>("56789"), 5);
  CHECK(split.value() == 0xCBF43926u);
}

TEST_CASE("hex validator: a good image is accepted, with its length and the CRC of the file") {
  const std::vector<std::uint8_t> image = good_image(1000);
  const std::string text = hex_of(image);
  const Verdict v = validate(text);
  CHECK(v.ok);
  CHECK(v.length == image.size());
  up::Crc32 c;
  c.add(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  CHECK(v.crc == c.value());
  // CR LF, lower case digits, other record sizes, blank lines, no final line end, a 64 KiB crossing: all fine.
  CHECK(accepts(to_hex(image, "\r\n")));
  CHECK(accepts(to_hex(image, "\n", 32)));
  CHECK(accepts(to_hex(image, "\n", 1)));
  CHECK(accepts(to_hex(image, "\n\n\r\n")));
  std::string lower = text;
  for (char& ch : lower) {
    ch = (ch >= 'A' && ch <= 'F') ? static_cast<char>(ch - 'A' + 'a') : ch;
  }
  CHECK(accepts(lower));
  std::string no_end = text;
  no_end.pop_back();  // the EOF record's LF
  CHECK(accepts(no_end));
  CHECK(accepts(hex_of(good_image(65'000))));
  CHECK(validate(hex_of(good_image(65'000))).length == good_image(65'000).size());  // 0x410 + 65'000, padded to the next 4 KiB boundary
  CHECK(good_image(65'000).size() == 69632u);
  CHECK(validate(hex_of(good_image(0, false))).fault == Fault::BadReset);  // no code for the reset vector to point at (the unpadded end is checked after it)
}

TEST_CASE("hex validator: the verdict does not depend on how the stream is cut") {
  const std::string text = hex_of(good_image(300));
  const Verdict whole = validate(text);
  NEED(whole.ok);
  for (std::size_t chunk = 1; chunk <= 70; ++chunk) {
    const Verdict v = validate(text, chunk);
    CHECK_MESSAGE(v.ok, "chunk " << chunk);
    CHECK(v.length == whole.length);
    CHECK(v.crc == whole.crc);
  }
  // Two pieces at every split point.
  for (std::size_t at = 0; at <= text.size(); ++at) {
    up::HexValidator v;
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(text.data());
    (void)v.feed(p, at);
    (void)v.feed(p + at, text.size() - at);
    CHECK_MESSAGE(v.finish(), "split at " << at);
  }
  // A bad stream fails at the same offset however it is cut.
  std::string bad = text;
  bad[100] = 'Z';
  const Verdict ref = validate(bad);
  CHECK(!ref.ok);
  CHECK(ref.fault == Fault::HexChar);
  CHECK(ref.offset == 100u);
  for (std::size_t chunk = 1; chunk <= 33; ++chunk) {
    const Verdict v = validate(bad, chunk);
    CHECK(v.fault == Fault::HexChar);
    CHECK(v.offset == 100u);
  }
  // Failure is sticky: later bytes change nothing, and reset() starts over.
  up::HexValidator v;
  const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(bad.data());
  CHECK(!v.feed(p, bad.size()));
  CHECK(!v.feed(p, 4));
  CHECK(!v.finish());
  CHECK(v.fault_offset() == 100u);
  v.reset();
  const std::uint8_t* q = reinterpret_cast<const std::uint8_t*>(text.data());
  CHECK(v.feed(q, text.size()));
  CHECK(v.finish());
}

TEST_CASE("hex validator: every kind of record defect is refused, with its own reason") {
  const std::vector<std::uint8_t> image = good_image();
  const std::string good = hex_of(image);
  const std::size_t first_data = good.find('\n') + 1;  // the first record after the extended address record
  auto expect = [&](const char* what, const std::string& text, Fault fault) {
    INFO(what);
    const Verdict v = validate(text);
    CHECK(!v.ok);
    CHECK(v.fault == fault);
  };
  {
    std::string t = good;
    t[first_data + 20] = (t[first_data + 20] == '0') ? '1' : '0';  // a data digit changes: the checksum no longer adds up
    expect("bad checksum", t, Fault::HexChecksum);
  }
  expect("no EOF record", to_hex(image, "\n", 16, false), Fault::HexNoEof);
  expect("data after EOF", good + record(0, 0, {1}) + "\n", Fault::HexAfterEof);
  expect("junk after EOF", good + "x\n", Fault::HexAfterEof);
  expect("unknown record type", good.substr(0, first_data) + record(9, 0, {0}) + "\n" + good.substr(first_data), Fault::HexType);
  {
    const std::string front = good.substr(0, first_data);
    expect("start segment address record (03)", front + record(3, 0, {0, 0, 4, 0x2D}) + "\n" + good.substr(first_data), Fault::HexStart);
    expect("start linear address record (05)", front + record(5, 0, {0, 0, 4, 0x2D}) + "\n" + good.substr(first_data), Fault::HexStart);
    expect("EOF record with data", record(1, 0, {1}) + "\n", Fault::HexRecord);
    expect("extended linear record of 3 bytes", record(4, 0, {0, 0, 0}) + "\n", Fault::HexRecord);
    expect("extended segment record of 1 byte", record(2, 0, {0}) + "\n", Fault::HexRecord);
  }
  expect("not hex", ":zz\n" + record(1, 0, {}) + "\n", Fault::HexChar);
  expect("missing colon", "00000001FF\n", Fault::HexChar);
  expect("empty file", "", Fault::HexNoEof);
  expect("only the EOF record", record(1, 0, {}) + "\n", Fault::HexShort);
  expect("odd number of digits", ":0400000001F\n", Fault::HexDigits);
  expect("count says 4, two bytes follow", ":04000000010201\n", Fault::HexRecord);
  expect("count says 1, a bad checksum comes first", ":01000000AABBCCDD\n", Fault::HexChecksum);
  expect("count says 1, one byte too many after a good checksum", ":0100000000FFAA\n", Fault::HexRecord);
  expect("shorter than a record header", ":0000\n", Fault::HexRecord);
  expect("a lone CR inside a line", ":00000001\rFF\n", Fault::HexRecord);
  expect("a CR not followed by LF", record(1, 0, {}) + "\r:00\n", Fault::HexAfterEof);
  expect("a space", ": 00000001FF\n", Fault::HexChar);
  expect("a NUL", std::string(":00000001FF\n\0", 13), Fault::HexAfterEof);
  expect("Ctrl-C", std::string("\x03", 1), Fault::HexChar);
  expect("a tab", ":00000001FF\t\n", Fault::HexChar);
  expect("a leading space on a line", " :00000001FF\n", Fault::HexChar);
}

TEST_CASE("hex validator: addresses must be the next one, and inside 1 MiB") {
  const std::vector<std::uint8_t> image = good_image(100);
  // A hole anywhere (a record missing from the middle).
  for (const std::size_t hole_at : {std::size_t{0x10}, std::size_t{0x100}, std::size_t{0x3F0}, std::size_t{0x400}, std::size_t{0x410}, std::size_t{0x450}}) {
    std::string text;
    text += record(4, 0, {0, 0}) + "\n";
    for (std::size_t at = 0; at < image.size(); at += 16) {
      if (at == hole_at) {
        continue;
      }
      const std::size_t n = image.size() - at < 16 ? image.size() - at : 16;
      text += record(0, static_cast<unsigned>(at), std::vector<std::uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(at), image.begin() + static_cast<std::ptrdiff_t>(at + n))) + "\n";
    }
    text += record(1, 0, {}) + "\n";
    const Verdict v = validate(text);
    CHECK_MESSAGE(!v.ok, "hole at " << hole_at);
    CHECK(v.fault == Fault::HexOrder);
  }
  // A byte given twice (an overlap), a record that goes back, records in another order (stricter than check_image.py; see the header).
  CHECK(validate(record(0, 0, {1, 2, 3, 4}) + "\n" + record(0, 2, {9}) + "\n").fault == Fault::HexOrder);
  CHECK(validate(record(0, 4, {1}) + "\n").fault == Fault::HexOrder);  // does not start at 0
  {
    const std::string a = record(0, 0, std::vector<std::uint8_t>(image.begin(), image.begin() + 16)) + "\n";
    const std::string b = record(0, 16, std::vector<std::uint8_t>(image.begin() + 16, image.begin() + 32)) + "\n";
    CHECK(validate(b + a).fault == Fault::HexOrder);
  }
  // The first record of a later 64 KiB window with the previous one short: a hole.
  CHECK(validate(record(0, 0, {1, 2}) + "\n" + record(4, 0, {0, 1}) + "\n" + record(0, 0, {3}) + "\n").fault == Fault::HexOrder);
  // Address range: nothing above 0xFFFFF, with its own reason (and the check comes before the order check, so a lone record up there says so). The last legal
  // address is accepted by the range check: the fault there is the missing bytes below it (the cross-check with check_image.py has the whole 1 MiB image).
  CHECK(validate(record(4, 0, {0x00, 0x10}) + "\n" + record(0, 0, {1}) + "\n").fault == Fault::HexRange);   // 0x100000
  CHECK(validate(record(4, 0, {0x00, 0x0F}) + "\n" + record(0, 0xFFFF, {1}) + "\n").fault == Fault::HexOrder);  // 0xFFFFF: in range
  CHECK(validate(record(4, 0, {0xFF, 0xFF}) + "\n" + record(0, 0xFFFF, {1, 2}) + "\n").fault == Fault::HexRange);  // up to the 32-bit end
  // Extended segment address records work like the linear ones.
  {
    const std::string text = record(2, 0, {0x00, 0x00}) + "\n" + hex_of(good_image(600)).substr(hex_of(good_image(600)).find('\n') + 1);
    CHECK(accepts(text));
  }
  // An image that ends before the flash configuration field has been read through.
  const std::vector<std::uint8_t> head = good_image(0);
  CHECK(validate(to_hex(std::vector<std::uint8_t>(head.begin(), head.begin() + 0x40F))).fault == Fault::HexShort);
  CHECK(validate(to_hex(std::vector<std::uint8_t>(head.begin(), head.begin() + 0x100))).fault == Fault::HexShort);
}

TEST_CASE("hex validator: the image must end on a 4 KiB boundary (OS-GATE-BOOTCRASH: the SD bootloader leaves a short final sector as it was)") {
  constexpr std::size_t kSector = 4096;
  // The boundary exactly is accepted, for one sector and for several, and the length is reported as it is.
  for (const std::size_t end : {kSector, 2 * kSector, 5 * kSector}) {
    CAPTURE(end);
    const Verdict v = validate(hex_of(good_image(end - 0x410, false)));
    CHECK(v.ok);
    CHECK(v.fault == Fault::Ok);
    CHECK(v.length == end);
  }
  // One byte short of a boundary, one byte past it, and a few more offsets are refused, with the fault at the end of the stream.
  for (const std::size_t end : {kSector - 1, kSector + 1, 2 * kSector - 1, 2 * kSector + 1, kSector - 0x100, kSector / 2 + 0x410}) {
    CAPTURE(end);
    const std::string text = hex_of(good_image(end - 0x410, false));
    const Verdict v = validate(text);
    CHECK_FALSE(v.ok);
    CHECK(v.fault == Fault::NotPadded);
    CHECK(v.offset == text.size());
    CHECK(v.length == 0u);
  }
  // The refusal does not depend on how the stream is cut up, and a fed validator stays refused after the verdict.
  const std::string text = hex_of(good_image(kSector - 0x410 - 1, false));
  for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{16}, std::size_t{45}, std::size_t{1000}}) {
    CAPTURE(chunk);
    CHECK(validate(text, chunk).fault == Fault::NotPadded);
  }
  up::HexValidator v;
  (void)v.feed(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  CHECK_FALSE(v.finish());
  CHECK_FALSE(v.finish());
  CHECK(v.fault() == Fault::NotPadded);
  // good_image pads by default, and the padding is zeros like .flashpad.
  const std::vector<std::uint8_t> padded = good_image(100);
  CHECK(padded.size() == kSector);
  CHECK(padded.back() == 0);
  CHECK(accepts(hex_of(padded)));
  // Another defect of the image is reported first: the sector rule is the last verdict.
  std::vector<std::uint8_t> bad_sp = good_image(100, false);
  put_word(bad_sp, 0, 0x20020000u);
  CHECK(validate(hex_of(bad_sp)).fault == Fault::BadSp);
  // Its console and screen text names the rule.
  CHECK(std::strcmp(up::fault_name(Fault::NotPadded), "image not padded: may not boot") == 0);
}

TEST_CASE("hex validator: the flash configuration field must be exactly ffffffffffffffffffffffffdef9ffff") {
  const std::vector<std::uint8_t> good = good_image();
  NEED(accepts(hex_of(good)));
  // Every one of the 16 bytes: one bit flipped, inverted, zero, erased.
  for (unsigned position = 0; position < 16; ++position) {
    const std::uint8_t required = up::kRequiredConfig[position];
    for (const std::uint8_t value : {static_cast<std::uint8_t>(required ^ 0x01u), static_cast<std::uint8_t>(required ^ 0xFFu), std::uint8_t{0x00}, std::uint8_t{0xFF}}) {
      if (value == required) {
        continue;
      }
      const Verdict v = validate(hex_of(with_byte(0x400 + position, value)));
      CHECK_MESSAGE(!v.ok, "byte " << position << " = " << unsigned{value});
      CHECK(v.fault == Fault::BadConfig);
    }
    for (unsigned bit = 0; bit < 8; ++bit) {
      CHECK(!accepts(hex_of(with_byte(0x400 + position, static_cast<std::uint8_t>(required ^ (1u << bit))))));
    }
  }
  CHECK(!accepts(hex_of(with_byte(0x40C, 0xDC))));  // a "locked" variant
  CHECK(!accepts(hex_of(with_byte(0x40C, 0xFE))));
  CHECK(!accepts(hex_of(with_byte(0x40C, 0xDF))));
  CHECK(!accepts(hex_of(with_byte(0x40C, 0xDD))));
  // The field is the 16 bytes at 0x400: a one-byte change just outside it is not a fault of the field.
  CHECK(accepts(hex_of(with_byte(0x3FF, 0x55))));
  CHECK(accepts(hex_of(with_byte(0x410, 0x55))));
  // The constant is the documented one.
  const char* const digits = "0123456789abcdef";
  std::string spelled;
  for (const std::uint8_t b : up::kRequiredConfig) {
    spelled += digits[b >> 4];
    spelled += digits[b & 0xF];
  }
  CHECK(spelled == "ffffffffffffffffffffffffdef9ffff");
}

TEST_CASE("hex validator: initial SP, reset vector and the vector table") {
  for (const std::uint32_t sp : {0u, 0x00001000u, 0x1FFEFFF8u, 0x20030008u, 0x20030004u, 0x2002FFF8u, 0x20020000u, 0x40000000u}) {
    const Verdict v = validate(hex_of(with_word(0, sp)));
    CHECK_MESSAGE(!v.ok, "SP " << sp);
    CHECK(v.fault == Fault::BadSp);
  }
  for (const std::uint32_t reset : {0x00000500u, 0x00200001u, 0x00000101u, 0x00000405u, 0x000FF001u, 0u, 0x0000040Fu, 0x00000611u + 0x1000u}) {
    const Verdict v = validate(hex_of(with_word(4, reset)));
    CHECK_MESSAGE(!v.ok, "reset " << reset);
    CHECK(v.fault == Fault::BadReset);
  }
  CHECK(accepts(hex_of(with_word(4, 0x00000411u))));  // the first byte past the field
  for (const unsigned index : {2u, 3u, 15u, 16u, 60u, 115u}) {
    for (const std::uint32_t value : {0x00000510u, 0u, 0x00300001u, 0x00000041u, 0x00000401u, 0xFFFFFFFFu, 0x00001001u}) {
      const Verdict v = validate(hex_of(with_word(index * 4u, value)));
      CHECK_MESSAGE(!v.ok, "vector " << index << " = " << value);
      CHECK(v.fault == Fault::BadVector);
    }
  }
  for (const unsigned index : {7u, 8u, 9u, 10u, 13u}) {
    CHECK(validate(hex_of(with_word(index * 4u, 0x00000511u))).fault == Fault::BadVector);
    CHECK(validate(hex_of(with_word(index * 4u, 1u))).fault == Fault::BadVector);
  }
  CHECK(accepts(hex_of(with_word(11u * 4u, 0x00000511u))));  // SVCall is not reserved: filled
  CHECK(accepts(hex_of(with_word(12u * 4u, 0x00000511u))));
  // The SRAM_L code window: Thumb addresses inside it, at an offset inside the image, are fine; the rest of SRAM is not.
  for (const unsigned index : {16u + 15u, 16u + 64u, 115u}) {
    CHECK(accepts(hex_of(with_word(index * 4u, 0x1FFF0101u))));
    CHECK(accepts(hex_of(with_word(index * 4u, 0x1FFF0000u + 0x411u))));
    for (const std::uint32_t value : {0x1FFF0100u, 0x1FFFC001u, 0x1FFFC000u | 1u, 0x20000101u, 0x1FFF8001u, 0x1FFEFFFFu, 0x1FFF0000u + 0x1000u + 1u}) {  // the last: one past the padded image's 4096 bytes
      CHECK_MESSAGE(!accepts(hex_of(with_word(index * 4u, value))), "vector " << index << " = " << value);
    }
  }
  CHECK(validate(hex_of(with_word(4, 0x1FFF0101u))).fault == Fault::BadReset);  // the reset vector is in flash
}

TEST_CASE("hex validator: nothing the stream can contain is undefined (every byte value in every state)") {
  const std::string good = hex_of(good_image(40));
  // Each byte value at a few positions: a verdict, never a crash (ASan and UBSan are on in the host build).
  for (unsigned value = 0; value < 256; ++value) {
    for (const std::size_t at : {std::size_t{0}, std::size_t{1}, std::size_t{9}, std::size_t{20}, good.size() - 1}) {
      std::string t = good;
      t[at] = static_cast<char>(value);
      (void)validate(t);
      (void)validate(t, 3);
    }
  }
  // A line of 255 data bytes, and one more than its count.
  std::vector<std::uint8_t> data(255, 0xAB);
  CHECK(validate(record(0, 0, data) + "\n").fault == Fault::HexNoEof);
  std::vector<std::uint8_t> huge(300, 1);
  (void)validate(record(0, 0, huge) + "\n");
}

// ---- the padder (OS-UPDPAD) --------------------------------------------------------------------------------------------------------
namespace {

std::string pad_text_of(std::uint32_t length) {
  up::HexPadWriter w;
  w.begin(length);
  std::string text;
  std::uint8_t line[up::kPadLineMax + 8];
  for (std::size_t n = w.next(line); n != 0u; n = w.next(line)) {
    CHECK(n <= up::kPadLineMax);
    text.append(reinterpret_cast<const char*>(line), n);
  }
  return text;
}

}  // namespace

TEST_CASE("hex pad: the exact HEX text for 20 bytes (one full record and one short), and the boundary cases") {
  // 4076 bytes of image: 20 zero bytes to 4096. The extended linear address record for 0x0000, a record of 16 at 0x0FEC, a record of 4 at 0x0FFC (checksums by hand: 6 -> FA, 0x10B -> F5, 0x10F -> F1).
  CHECK(pad_text_of(4076) == ":020000040000FA\n"
                             ":100FEC0000000000000000000000000000000000F5\n"
                             ":040FFC0000000000F1\n");
  // The upper 16 bits of the image's end are the base record's payload: 0x10FFF is one byte short of 0x11000.
  CHECK(pad_text_of(0x10FFF) == ":020000040001F9\n:010FFF0000F1\n");
  // Exactly on a boundary: nothing at all. One byte short of one: one byte. One past: 4095 bytes.
  CHECK(up::HexPadWriter::pad_bytes(4096) == 0u);
  CHECK(up::HexPadWriter::text_bytes(4096) == 0u);
  CHECK(pad_text_of(4096).empty());
  CHECK(pad_text_of(2 * 4096).empty());
  CHECK(up::HexPadWriter::pad_bytes(4095) == 1u);
  CHECK(up::HexPadWriter::pad_bytes(4097) == 4095u);
  CHECK(pad_text_of(4095) == ":020000040000FA\n:010FFF0000F1\n");
  const std::string most = pad_text_of(4097);  // 4095 bytes: 255 records of 16 and one of 15
  CHECK(most.size() == up::HexPadWriter::text_bytes(4097));
  CHECK(most.compare(0, 16, ":020000040000FA\n") == 0);
  CHECK(most.compare(16, 9, ":10100100") == 0);  // the first data record: 16 bytes at 0x1001
  // A writer that is finished stays finished, and begin() starts over.
  up::HexPadWriter w;
  w.begin(4076);
  std::uint8_t line[up::kPadLineMax];
  while (w.next(line) != 0u) {
  }
  CHECK(w.next(line) == 0u);
  w.begin(4095);
  CHECK(w.next(line) == 16u);
}

TEST_CASE("hex pad: text_bytes is the length of the text, for every length near a boundary and a 64 KiB line, and the padded file validates") {
  for (std::uint32_t length = 1; length <= 3 * 4096u; length += (length < 40u || length > 3 * 4096u - 40u || (length > 4096u - 40u && length < 4096u + 40u)) ? 1u : 37u) {
    CAPTURE(length);
    CHECK(pad_text_of(length).size() == up::HexPadWriter::text_bytes(length));
  }
  for (const std::uint32_t length : {0xFFFFu, 0x10000u - 4096u + 1u, 0x10001u, 0x10FFFu, 0xFF001u, 0xFFFFFu}) {
    CAPTURE(length);
    CHECK(pad_text_of(length).size() == up::HexPadWriter::text_bytes(length));
    CHECK(up::HexPadWriter::pad_bytes(length) == ((length + 4095u) & ~4095u) - length);
  }
  // The padded file validates, ends on the boundary and has the length of the padded image, across a 64 KiB line too; it is the original's records, the writer's lines, the EOF record.
  for (const std::size_t length : {std::size_t{4095}, std::size_t{4097}, std::size_t{4096 + 0x100}, std::size_t{0x10000 + 100}, std::size_t{0x20000 - 1}, std::size_t{0x10000 - 1}}) {
    CAPTURE(length);
    const std::vector<std::uint8_t> raw = good_image(length - 0x410, false);
    CHECK(validate(to_hex(raw)).fault == Fault::NotPadded);
    const std::string text = expected_padded_hex(raw);
    const Verdict v = validate(text);
    CHECK(v.ok);
    CHECK(v.length == (length + 4095u) / 4096u * 4096u);
    CHECK(to_hex(raw, "\n", 16, false) + pad_text_of(static_cast<std::uint32_t>(length)) + record(1, 0, {}) + "\n" == text);
    CHECK(validate(text, 1).ok);
  }
}
