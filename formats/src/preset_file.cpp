// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/preset.h>

#include "native_codec.h"

#include <o3os/formats/crc32.h>

#include <cstring>

namespace o3os::formats::preset {

using native::codec::Cursor;
using native::codec::Enc;
using native::codec::get16;
using native::codec::get32;

namespace {

constexpr std::size_t kHeader = native::codec::kChunkHeader;
constexpr std::size_t kFormHeader = native::codec::kFormHeader;  // "FORM", length, "O3MP"

// MPST payload without the entries: version, stem length byte, kind, module version, count.
constexpr std::size_t kMpstFixed = 2 + 1 + 1 + 4 + 1;

// The CRC chunk's own version, written and expected as 1.0 (not the project's chunk version: this format's CRC chunk is versioned with it).
constexpr std::uint16_t kCrcVersion = 0x0100;

// The Report's `detail` texts are for people at a computer (o3preset-style dumps, test failures): on the target they are not built in, only the
// Error and the offset are kept (the card side says PRESET DAMAGED or NEWER PRESET FORMAT from the Error alone).
#if defined(__arm__)
#define O3MP_WHY(text) ""
#else
#define O3MP_WHY(text) text
#endif

bool stem_ok(const std::uint8_t* s, std::size_t n) noexcept {
  if (n == 0 || n > kStemMax) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t c = s[i];
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      return false;
    }
  }
  return !(n == 1 && s[0] == '.') && !(n == 2 && s[0] == '.' && s[1] == '.');
}

bool name_ok(const std::uint8_t* s, std::size_t n) noexcept {
  if (n == 0 || n > kNameMax) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (s[i] < 0x20 || s[i] > 0x7E) {
      return false;
    }
  }
  return true;
}

bool finite_bits(std::uint32_t bits) noexcept { return (bits & 0x7F800000u) != 0x7F800000u; }

bool finite(float f) noexcept {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  return finite_bits(bits);
}

// The one place a read fails: the report, a cleared preset, the error. Not inline: it is called from every refusal of read().
__attribute__((noinline)) Error fail(Report& r, Preset& out, Error e, const char* why, std::size_t at) noexcept {
  r.error = e;
  r.detail = why;
  r.offset = static_cast<std::uint32_t>(at);
  out = Preset{};  // never half-read
  return e;
}

#define O3MP_FAIL(e, why, at) return fail(r, out, (e), O3MP_WHY(why), (at))

// Length of a NUL-terminated field of `cap` bytes; cap when it has no NUL (not a valid field).
std::size_t field_len(const char* s, std::size_t cap) noexcept {
  std::size_t n = 0;
  while (n < cap && s[n] != '\0') {
    ++n;
  }
  return n;
}

}  // namespace

const char* error_name(Error e) noexcept {
  switch (e) {
    case Error::None: return "none";
    case Error::Truncated: return "truncated";
    case Error::BadMagic: return "bad magic";
    case Error::BadCrc: return "bad CRC";
    case Error::NewerMajor: return "newer major version";
    case Error::Malformed: return "malformed";
    case Error::TooBig: return "too big";
  }
  return "unknown";
}

bool is_stem(const char* text) noexcept {
  if (text == nullptr) {
    return false;
  }
  const std::size_t n = field_len(text, kStemMax + 1);
  return n <= kStemMax && stem_ok(reinterpret_cast<const std::uint8_t*>(text), n);
}

bool sniff(const std::uint8_t* head, std::size_t n) noexcept {
  return head != nullptr && n >= kFormHeader && std::memcmp(head, "FORM", 4) == 0 && get32(head + 8) == kTypeO3mp;
}

bool valid(const Preset& p) noexcept {
  const std::size_t mlen = field_len(p.module, kStemMax + 1);
  if (mlen > kStemMax || !stem_ok(reinterpret_cast<const std::uint8_t*>(p.module), mlen)) {
    return false;
  }
  if ((p.kind != kKindInstrument && p.kind != kKindEffect) || p.count > kMaxEntries) {
    return false;
  }
  for (std::size_t i = 0; i < p.count; ++i) {
    const Entry& e = p.entries[i];
    const std::size_t n = field_len(e.name, kNameMax + 1);
    if (n > kNameMax || !name_ok(reinterpret_cast<const std::uint8_t*>(e.name), n) || !finite(e.value)) {
      return false;
    }
  }
  return true;
}

Error read(const std::uint8_t* data, std::size_t size, Preset& out, Report& r) noexcept {
  r = Report{};
  out = Preset{};
  if (data == nullptr || size < kFormHeader) {
    O3MP_FAIL(size >= 4 && data != nullptr && std::memcmp(data, "FORM", 4) != 0 ? Error::BadMagic : Error::Truncated,
              size >= 4 ? "shorter than the FORM header" : "empty", 0);
  }
  if (std::memcmp(data, "FORM", 4) != 0 || get32(data + 8) != kTypeO3mp) {
    O3MP_FAIL(Error::BadMagic, "not an O3MP preset file", 0);
  }
  const std::uint32_t len = get32(data + 4);
  if (size > kMaxFileBytes || len > kMaxFileBytes - 8u) {
    O3MP_FAIL(Error::TooBig, "file larger than the limit", 4);
  }
  if (len < 4 || (len & 1u) != 0) {
    O3MP_FAIL(Error::Malformed, "bad FORM length", 4);
  }
  if (size < static_cast<std::size_t>(len) + 8u) {
    O3MP_FAIL(Error::Truncated, "the file ends before its FORM does", size);
  }

  // Pass 1: the container. Every chunk's header, length, pad and version, then the CRC (the last chunk). Bytes after the FORM are not looked at.
  bool mpst_seen = false;
  std::size_t mpst_at = 0;
  std::size_t mpst_len = 0;
  bool mpst_newer = false;
  const std::size_t end = static_cast<std::size_t>(len) + 8u;
  std::size_t pos = kFormHeader;
  bool crc_seen = false;
  while (pos < end && !crc_seen) {
    if (end - pos < kHeader) {
      O3MP_FAIL(Error::Malformed, "chunk header overruns the FORM", pos);
    }
    const std::uint32_t id = get32(data + pos);
    const std::size_t plen = get32(data + pos + 4);
    const std::size_t pad = plen & 1u;
    r.chunk = id;
    if (plen > end - pos - kHeader || plen + pad > end - pos - kHeader) {
      O3MP_FAIL(Error::Malformed, "chunk overruns the FORM", pos);
    }
    const std::size_t payload = pos + kHeader;
    if (pad != 0 && data[payload + plen] != 0) {
      O3MP_FAIL(Error::Malformed, "pad byte is not zero", payload + plen);
    }
    if (id == native::kChunkCrc || id == kChunkMpst) {
      if (plen < 2) {
        O3MP_FAIL(Error::Malformed, "chunk length out of range", pos);
      }
      const std::uint16_t version = get16(data + payload);
      if ((version >> 8) == 0) {
        O3MP_FAIL(Error::Malformed, "chunk version 0", payload);
      }
      const std::uint16_t known = id == native::kChunkCrc ? kCrcVersion : kVersion;
      if ((version >> 8) > (known >> 8)) {
        O3MP_FAIL(Error::NewerMajor, "chunk written by a newer major version of the format", payload);
      }
      const bool newer = (version & 0xFFu) > (known & 0xFFu);
      r.newer_minor = r.newer_minor || newer;
      if (id == native::kChunkCrc) {
        if (plen < native::codec::kCrcPayload || (!newer && plen != native::codec::kCrcPayload)) {
          O3MP_FAIL(Error::Malformed, "chunk length out of range", pos);
        }
        if (pos + kHeader + plen + pad != end) {
          O3MP_FAIL(Error::Malformed, "the CRC chunk is not the last", pos);
        }
        if (get32(data + payload + 2) != crc32(data, pos)) {
          O3MP_FAIL(Error::BadCrc, "the CRC chunk does not match the file", pos);
        }
        crc_seen = true;
      } else {
        if (mpst_seen) {
          O3MP_FAIL(Error::Malformed, "chunk repeated", pos);
        }
        mpst_seen = true;
        mpst_at = payload;
        mpst_len = plen;
        mpst_newer = newer;
      }
    } else {
      if (!model::is_printable_fourcc(id)) {
        O3MP_FAIL(Error::Malformed, "chunk id is not four printable characters", pos);
      }
      ++r.unknown_chunks;  // a chunk of a newer version of this format, or someone else's: ignored
    }
    pos += kHeader + plen + pad;
  }
  if (!crc_seen) {
    O3MP_FAIL(Error::Malformed, "the FORM ends without a CRC chunk", pos);
  }
  if (!mpst_seen) {
    O3MP_FAIL(Error::Malformed, "no MPST chunk", pos);
  }

  // Pass 2: the preset.
  r.chunk = kChunkMpst;
  Cursor c(data + mpst_at, mpst_len);
  (void)c.u16();  // the version, checked above
  const std::uint8_t* text = nullptr;
  std::size_t tlen = 0;
  if (!c.text(kStemMax, text, tlen) || !stem_ok(text, tlen)) {
    O3MP_FAIL(Error::Malformed, "the module stem is not valid", mpst_at + 2);
  }
  Preset& p = out;  // decoded in place; a failure clears it
  std::memcpy(p.module, text, tlen);
  p.kind = c.u8();
  if (p.kind != kKindInstrument && p.kind != kKindEffect) {
    O3MP_FAIL(Error::Malformed, "the kind is neither instrument nor effect", mpst_at + 3 + tlen);
  }
  p.module_version = c.u32();
  p.count = c.u8();
  if (c.bad() || p.count > kMaxEntries) {
    O3MP_FAIL(Error::Malformed, c.bad() ? "MPST ends in its header" : "more than 32 entries", mpst_at);
  }
  for (std::uint8_t i = 0; i < p.count; ++i) {
    const std::size_t at = mpst_at + (mpst_len - c.left());
    if (!c.text(kNameMax, text, tlen) || !name_ok(text, tlen)) {
      O3MP_FAIL(Error::Malformed, "an entry's name is not valid", at);
    }
    const float value = c.f32();
    if (c.bad()) {
      O3MP_FAIL(Error::Malformed, "MPST does not fit its entry count", at);
    }
    if (!finite(value)) {
      O3MP_FAIL(Error::Malformed, "an entry's value is not finite", at);
    }
    std::memcpy(p.entries[i].name, text, tlen);
    p.entries[i].value = value;
    // Every entry is kept, in file order: a card module may have two parameters of one name, and the card side matches them in that order.
    // A name seen before is only counted.
    for (std::uint8_t j = 0; j < i; ++j) {
      if (std::strcmp(p.entries[j].name, p.entries[i].name) == 0) {
        ++r.duplicates;
        break;
      }
    }
  }
  if (c.left() != 0 && !mpst_newer) {
    O3MP_FAIL(Error::Malformed, "bytes after the last entry", mpst_at + (mpst_len - c.left()));
  }
  return Error::None;
}

std::size_t write(const Preset& p, std::uint8_t* out, std::size_t cap) noexcept {
  if (out == nullptr || !valid(p)) {
    return 0;
  }
  const std::size_t mlen = std::strlen(p.module);
  std::size_t payload = kMpstFixed + mlen;
  for (std::size_t i = 0; i < p.count; ++i) {
    payload += 1 + std::strlen(p.entries[i].name) + 4;
  }
  const std::size_t total = kFormHeader + native::codec::chunk_total(payload) + native::codec::chunk_total(native::codec::kCrcPayload);
  if (total > cap || total > kMaxFileBytes) {
    return 0;
  }
  Enc e(out, cap);
  e.u32(native::kForm);
  e.u32(static_cast<std::uint32_t>(total - 8));
  e.u32(kTypeO3mp);
  e.u32(kChunkMpst);
  e.u32(static_cast<std::uint32_t>(payload));
  e.u16(kVersion);
  e.text(p.module, mlen);
  e.u8(p.kind);
  e.u32(p.module_version);
  e.u8(p.count);
  for (std::size_t i = 0; i < p.count; ++i) {
    e.text(p.entries[i].name, std::strlen(p.entries[i].name));
    e.f32(p.entries[i].value);
  }
  if ((payload & 1u) != 0) {
    e.u8(0);
  }
  const std::size_t body = e.size();
  e.u32(native::kChunkCrc);
  e.u32(static_cast<std::uint32_t>(native::codec::kCrcPayload));
  e.u16(kCrcVersion);
  e.u32(crc32(out, body));
  return e.overflow() || e.size() != total ? 0 : total;
}

#undef O3MP_FAIL
#undef O3MP_WHY

}  // namespace o3os::formats::preset
