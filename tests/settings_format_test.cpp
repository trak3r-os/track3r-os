// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002d, the device settings file (docs/formats/o3-settings.md, formats/settings.h): the bytes of the default file against a literal
// written from the spec, write and read round trips, default-fill for what a file lacks or a reader does not know, every refusal, and the
// damage loops. No project arena: also runs on the Cortex-M4F (qemu_params).
#include <doctest/doctest.h>

#include <o3os/formats/crc32.h>
#include <o3os/formats/settings.h>
#include <o3os/model/params.h>
#include <o3os/model/settings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "support/need.h"

using namespace o3os;
namespace fs = o3os::formats::settings;
using Bytes = std::vector<std::uint8_t>;

#if defined(__arm__)
constexpr unsigned kScale = 8;  // the target is an order of magnitude slower: fewer steps, the same cases
#else
constexpr unsigned kScale = 1;
#endif

namespace {

constexpr std::uint32_t id4(char a, char b, char c, char d) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}
constexpr std::uint32_t kSets = id4('S', 'E', 'T', 'S');
constexpr std::uint32_t kLrnb = id4('L', 'R', 'N', 'B');

void put32(Bytes& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<std::uint8_t>(v >> s));
  }
}
void put16(Bytes& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v >> 8));
  b.push_back(static_cast<std::uint8_t>(v));
}
void putf(Bytes& b, float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  put32(b, bits);
}

// A file written from the spec, not by the writer: FORM, the chunks as given (padded to even), then the CRC chunk. `crc_version`
// and `crc_tail` make a newer-minor CRC chunk.
Bytes build(const std::vector<std::pair<std::uint32_t, Bytes>>& chunks, std::uint16_t crc_version = 0x0100, std::size_t crc_tail = 0) {
  Bytes body;
  for (const auto& c : chunks) {
    put32(body, c.first);
    put32(body, static_cast<std::uint32_t>(c.second.size()));
    body.insert(body.end(), c.second.begin(), c.second.end());
    if ((c.second.size() & 1u) != 0) {
      body.push_back(0);
    }
  }
  const std::size_t crc_len = 6 + crc_tail;
  Bytes f = {'F', 'O', 'R', 'M'};
  put32(f, static_cast<std::uint32_t>(4 + body.size() + 8 + crc_len + (crc_len & 1u)));
  f.insert(f.end(), {'O', '3', 'S', 'T'});
  f.insert(f.end(), body.begin(), body.end());
  const std::uint32_t crc = formats::crc32(f.data(), f.size());
  put32(f, id4('C', 'R', 'C', ' '));
  put32(f, static_cast<std::uint32_t>(crc_len));
  put16(f, crc_version);
  put32(f, crc);
  f.insert(f.end(), crc_tail, 0xCD);
  if ((crc_len & 1u) != 0) {
    f.push_back(0);
  }
  return f;
}

// A SETS payload: version, count and the entries; `entry` is (ParamId, value).
Bytes sets_payload(const std::vector<std::pair<std::uint32_t, float>>& entries, std::uint16_t version = 0x0100) {
  Bytes p;
  put16(p, version);
  put16(p, static_cast<std::uint16_t>(entries.size()));
  for (const auto& e : entries) {
    put32(p, e.first);
    putf(p, e.second);
  }
  return p;
}

struct Bind {
  std::uint32_t param;
  std::uint8_t cc, channel, mode, flags;
};
Bytes lrnb_payload(const std::vector<Bind>& entries, std::uint16_t version = 0x0100) {
  Bytes p;
  put16(p, version);
  put16(p, static_cast<std::uint16_t>(entries.size()));
  for (const Bind& b : entries) {
    put32(p, b.param);
    p.insert(p.end(), {b.cc, b.channel, b.mode, b.flags});
  }
  return p;
}

constexpr std::uint32_t dev(unsigned field) { return (5u << 24) | field; }

model::Settings fresh() {
  model::Settings s;
  std::memset(&s, 0, sizeof s);
  s.init(core::ParamKind::Device);
  return s;
}

bool same(const model::Settings& a, const model::Settings& b) {
  for (unsigned f = 0; f < model::device_setting::kCount; ++f) {
    if (a.get(f) != b.get(f)) {
      return false;
    }
  }
  for (unsigned i = 0; i < core::kLearnSlots; ++i) {
    if (std::memcmp(&a.learn(i), &b.learn(i), sizeof(core::LearnBinding)) != 0) {
      return false;
    }
  }
  return true;
}

Bytes written(const model::Settings& s) {
  Bytes out(fs::kMaxFileBytes);
  const std::size_t n = fs::write(s, out.data(), out.size());
  out.resize(n);
  return out;
}

// The default device settings as the spec says they are written: 286 bytes (docs/formats/o3-settings.md section 8; 278 before OS-LEDBPM appended STATUS LED, 270 before OS-MTC B3 appended MTC IN, 254 before OS-MTC A3 appended MTC OUT and MTC START, 246 before OS-MIDICCMAP S1 appended CC MAP, 238 before E-PAT-35 appended PAT SWITCH, 222 before E-CFG-03 appended TRANSPORT IN and OUT, 214 before OS-PADOFFSET appended PAD OFFSET, 198 before OS-RECMICRO appended REC MICRO and REC VELOCITY, 182 before OS-METRO appended METRO and COUNT-IN, 174 before OS-USBAUDIO appended USB AUDIO, 150 before OS-006e appended RETURN LEVEL (and the two rows of OS-LIVEREC), 142 before OS-USBRET appended USB RETURN, 94 with the first seven settings alone). Computed from the description with Python, not by the writer.
const Bytes kDefaultFile = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x01, 0x16, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xfc, 0x01, 0x00, 0x00, 0x1f, 0x05, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00, 0x05, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10,
    0x42, 0xc8, 0x00, 0x00, 0x05, 0x00, 0x00, 0x11, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16, 0x41, 0x40, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x17, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x18, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1c, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1d,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x4a, 0xdc, 0xe6, 0xc9,
};

// The default file as it was written before OS-LEDBPM (30 settings, 278 bytes, valid CRC): the file of a device that never heard of STATUS LED.
const Bytes kFile30 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x01, 0x0e, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xf4, 0x01, 0x00, 0x00, 0x1e, 0x05, 0x00, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x11, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16, 0x41, 0x40, 0x00, 0x00, 0x05, 0x00, 0x00, 0x17, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x18, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
    0x1b, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1c, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00,
    0x00, 0x06, 0x01, 0x00, 0x91, 0x75, 0x9c, 0xc9,
};

// The default file as it was written before OS-MTC B3 (29 settings, 270 bytes, valid CRC): the file of a device that never heard of MTC IN.
const Bytes kFile29 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x01, 0x06, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xec, 0x01, 0x00, 0x00, 0x1d, 0x05, 0x00, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x11, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16, 0x41, 0x40, 0x00, 0x00, 0x05, 0x00, 0x00, 0x17, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x18, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
    0x1b, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1c, 0x3f, 0x80, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x7a, 0x8d, 0xd7, 0xe0,
};

// The default file as it was written before OS-MTC A3 (27 settings, 254 bytes, valid CRC): the file of a device that never heard of MTC OUT and MTC START.
const Bytes kFile27 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xf6, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xdc, 0x01, 0x00, 0x00, 0x1b, 0x05, 0x00, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x11, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16, 0x41, 0x40, 0x00, 0x00, 0x05, 0x00, 0x00, 0x17, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x18, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43,
    0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0xba, 0xde, 0x65, 0x2d,
};

// The default file as it was written before OS-MIDICCMAP S1 (26 settings, 246 bytes, valid CRC): the file of a device that never heard of CC MAP.
const Bytes kFile26 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xee, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xd4, 0x01, 0x00, 0x00, 0x1a, 0x05, 0x00, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00, 0x05, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x11, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16, 0x41, 0x40, 0x00, 0x00, 0x05, 0x00, 0x00, 0x17, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x18, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0xb0,
    0xcd, 0x92, 0x5a,
};


// The default file as it was written before E-PAT-35 (25 settings, 238 bytes, valid CRC): the file of a device that never heard of PAT SWITCH.
const Bytes kFile25 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xe6, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xcc, 0x01, 0x00, 0x00, 0x19,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00, 0x00, 0x11, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16, 0x41, 0x40, 0x00, 0x00, 0x05, 0x00, 0x00, 0x17, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x18, 0x3f, 0x80, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0xd6, 0xf8, 0x96, 0xb9,
};

// The default file as it was written before E-CFG-03 (23 settings, 222 bytes, valid CRC): the file of a device that never heard of TRANSPORT IN or TRANSPORT OUT.
const Bytes kFile23 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xd6, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xbc, 0x01, 0x00, 0x00, 0x17,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00, 0x00, 0x11, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x16, 0x41, 0x40, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06,
    0x01, 0x00, 0x2d, 0x53, 0x7f, 0x54,
};

// The default file as it was written before OS-PADOFFSET (22 settings, 214 bytes, valid CRC): the file of a device that never heard of PAD OFFSET.
const Bytes kFile22 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xce, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xb4, 0x01, 0x00, 0x00, 0x16,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00, 0x00, 0x11, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0xcf, 0x58, 0xdf, 0xac,
};

// The default file as it was written before OS-RECMICRO (20 settings, 198 bytes, valid CRC): the file of a device that never heard of REC MICRO or REC VELOCITY.
const Bytes kFile20 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xbe, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0xa4, 0x01, 0x00, 0x00, 0x14,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00, 0x00, 0x11, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x12, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06,
    0x01, 0x00, 0x75, 0x18, 0xb1, 0xb5,
};

// The default file as it was written before OS-METRO (18 settings, 182 bytes, valid CRC): the file of a device that never heard of METRO or COUNT-IN.
const Bytes kFile18 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xae, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0x94, 0x01, 0x00, 0x00, 0x12,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x05, 0x00, 0x00, 0x11, 0x3f, 0x80, 0x00, 0x00,
    0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0xfe, 0xa8, 0x7a, 0x1e,
};

// The default file as it was written before OS-USBAUDIO (17 settings, 174 bytes, valid CRC): the file of a device that never heard of USB AUDIO.
const Bytes kFile17 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0xa6, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0x8c, 0x01, 0x00, 0x00, 0x11,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10, 0x42, 0xc8, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06,
    0x01, 0x00, 0x77, 0x13, 0x61, 0x81,
};

// The same file as it was written before OS-LIVEREC and OS-006e (14 settings, 150 bytes, valid CRC): the file of a device that never heard of REC POLY, REC QUANTIZE or RETURN LEVEL.
const Bytes kFile14 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0x8e, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0x74, 0x01, 0x00, 0x00, 0x0e,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06,
    0x01, 0x00, 0xbf, 0x8a, 0x1f, 0xcb,
};

// The default file as the OS-LIVEREC branch wrote it before the merge with OS-006e (16 settings, 166 bytes, valid CRC): REC POLY and REC QUANTIZE at 14 and 15, no RETURN LEVEL.
const Bytes kFile16 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0x9e, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0x84, 0x01, 0x00, 0x00, 0x10,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0d, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0x0e, 0x0e, 0x9f, 0xc3,
};

// The same file as it was written before OS-USBRET (13 settings, 142 bytes, valid CRC): the file of a device that never heard of USB RETURN.
const Bytes kFile13 = {
    0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0x86, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0x6c, 0x01, 0x00, 0x00, 0x0d,
    0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x08, 0x40, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x0b, 0x3f, 0x80, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0xce, 0x0e, 0x0d, 0x6d,
};

}  // namespace

TEST_CASE("settings file: the default block is these 286 bytes, and they read back as the default block") {
  const model::Settings d = fresh();
  CHECK(written(d) == kDefaultFile);
  CHECK(formats::crc32(kDefaultFile.data(), kDefaultFile.size() - 14) == 0x4adce6c9u);  // zlib.crc32 of the first 272 of the 286 bytes (0x91759cc9 over the 264 of the 278 before OS-LEDBPM: kFile30)
  CHECK(formats::crc32(kFile30.data(), kFile30.size() - 14) == 0x91759cc9u);
  CHECK(fs::sniff(kDefaultFile.data(), kDefaultFile.size()));
  CHECK_FALSE(fs::sniff(kDefaultFile.data(), 11));
  CHECK_FALSE(fs::sniff(reinterpret_cast<const std::uint8_t*>("FORM\0\0\0\0O3PJ"), 12));

  model::Settings out = fresh();
  (void)out.set(model::device_setting::MetronomeVolume, 99.0f);  // something to be overwritten
  const fs::Report r = fs::read(kDefaultFile.data(), kDefaultFile.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  CHECK(r.unknown_chunks == 0u);
  CHECK_FALSE(r.newer_minor);
  CHECK(same(out, d));
  CHECK(out.is_default());
  CHECK(std::string(fs::error_name(fs::Error::BadCrc)) == "bad CRC");
}

TEST_CASE("settings file: values and bindings round trip, byte for byte, and the biggest file stays under the limit") {
  model::Settings s = fresh();
  CHECK(s.set(model::device_setting::DisplayBrightness, 0.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::MetronomeVolume, 73.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::ClockIn, 2.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::Numbering, 2.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::MiddleC, 3.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::ClockSync, -6.0f) == model::Status::Ok);
  CHECK(s.set_learn(0, core::LearnBinding{core::param_id(core::ParamKind::Track, 2, 0), 7, 0, 0, 0}) == model::Status::Ok);
  CHECK(s.set_learn(1, core::LearnBinding{core::param_id(core::ParamKind::Master, 0, 0), 11, 16, 1, 0}) == model::Status::Ok);
  CHECK(s.set_learn(2, core::LearnBinding{core::param_id(core::ParamKind::Device, 0, 2), 21, 3, 2, 0}) == model::Status::Ok);
  const Bytes a = written(s);
  NEED(a.size() > 286);
  CHECK(a.size() == 286u + 8u + 4u + 3u * 8u);  // the default 286, an LRNB chunk of three bindings
  model::Settings back = fresh();
  const fs::Report r = fs::read(a.data(), a.size(), back);
  CHECK(r.error == fs::Error::None);
  CHECK(same(s, back));
  // The second save is the first's bytes: the file is a function of the block alone.
  CHECK(written(back) == a);
  // The slots are packed in the file: bindings in slots 0, 4 and 9 come back in slots 0, 1 and 2, and save to the same bytes.
  model::Settings gaps = s;
  CHECK(gaps.set_learn(9, s.learn(2)) == model::Status::Ok);
  CHECK(gaps.set_learn(2, core::LearnBinding{}) == model::Status::Ok);
  CHECK(gaps.set_learn(4, s.learn(1)) == model::Status::Ok);
  CHECK(gaps.set_learn(1, core::LearnBinding{}) == model::Status::Ok);
  const Bytes packed = written(gaps);
  CHECK(packed == written(s));
  model::Settings from_gaps = fresh();
  CHECK(fs::read(packed.data(), packed.size(), from_gaps).error == fs::Error::None);
  CHECK(from_gaps.learn(1).cc == 11);
  CHECK(from_gaps.learn(4).param == core::ParamId::None);

  // The largest: every slot used, every value moved.
  model::Settings big = fresh();
  for (unsigned f = 0; f < model::device_setting::kCount; ++f) {
    const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, f));
    NEED(d != nullptr);
    CHECK(big.set(f, d->max) == model::Status::Ok);
  }
  for (unsigned i = 0; i < core::kLearnSlots; ++i) {
    CHECK(big.set_learn(i, core::LearnBinding{core::param_id(core::ParamKind::Track, i % 16, i / 16), static_cast<std::uint8_t>(i), 0, 0, 0}) == model::Status::Ok);
  }
  const Bytes b = written(big);
  CHECK(b.size() == 12u + (8u + 252u) + (8u + 4u + 32u * 8u) + 14u);  // SETS holds 31 entries now (30 before OS-LEDBPM appended STATUS LED, id 30; 29 before OS-MTC B3 appended MTC IN, id 29)
  CHECK(b.size() <= fs::kMaxFileBytes);
  model::Settings big_back = fresh();
  CHECK(fs::read(b.data(), b.size(), big_back).error == fs::Error::None);
  CHECK(same(big, big_back));
}

TEST_CASE("settings file: what a file lacks is the default, what a reader does not know is skipped and counted") {
  const model::Settings d = fresh();
  model::Settings out = fresh();
  // No SETS at all, and no LRNB: only the CRC chunk. Everything is the default.
  fs::Report r = fs::read(build({}).data(), build({}).size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(same(out, d));
  // Fewer entries than there are settings (an older file): the rest are the defaults.
  const Bytes few = build({{kSets, sets_payload({{dev(2), 10.0f}, {dev(3), 1.0f}})}});
  r = fs::read(few.data(), few.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(out.get(model::device_setting::MetronomeVolume) == 10.0f);
  CHECK(out.get(model::device_setting::ClockIn) == 1.0f);
  CHECK(out.get(model::device_setting::DisplayBrightness) == 2.0f);  // default
  CHECK(out.get(model::device_setting::ClockSync) == 0.0f);
  // Entries this version cannot use: an unknown setting (a newer file), a setting of the project's scope, one with an index, a value out of
  // its range, NaN. Each is counted and the setting keeps its default; the valid ones around them are read.
  const Bytes odd = build({{kSets, sets_payload({{dev(2), 20.0f}, {dev(99), 1.0f}, {(4u << 24) | 1u, 1.0f}, {(5u << 24) | (1u << 16) | 3u, 1.0f},
                                                  {dev(4), 7.0f}, {dev(0), std::nanf("")}, {dev(5), 3.0f}})}});
  r = fs::read(odd.data(), odd.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 5u);
  CHECK(out.get(model::device_setting::MetronomeVolume) == 20.0f);
  CHECK(out.get(model::device_setting::MiddleC) == 3.0f);
  CHECK(out.get(model::device_setting::Numbering) == 1.0f);          // 7 is not a numbering: the default
  CHECK(out.get(model::device_setting::DisplayBrightness) == 2.0f);  // NaN: the default
  // The same setting twice: the later one wins. A value of 0.4 is the nearest whole value.
  const Bytes twice = build({{kSets, sets_payload({{dev(2), 10.0f}, {dev(2), 30.0f}, {dev(3), 0.4f}, {dev(1), 1.6f}})}});
  r = fs::read(twice.data(), twice.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(out.get(model::device_setting::MetronomeVolume) == 30.0f);
  CHECK(out.get(model::device_setting::ClockIn) == 0.0f);
  CHECK(out.get(model::device_setting::PadsBrightness) == 2.0f);
  // Bindings: a project-scope parameter, a bad controller and a bad channel are skipped and counted; a second binding of one controller
  // takes it over; the same parameter again replaces its binding.
  const std::uint32_t track0 = core::param_id(core::ParamKind::Track, 0, 0) == core::ParamId::None ? 0u : static_cast<std::uint32_t>(core::param_id(core::ParamKind::Track, 0, 0));
  const std::uint32_t track1 = static_cast<std::uint32_t>(core::param_id(core::ParamKind::Track, 1, 0));
  const std::uint32_t inst = static_cast<std::uint32_t>(core::param_id(core::ParamKind::Instrument, 0, 7));
  const Bytes learn = build({{kLrnb, lrnb_payload({{track0, 7, 0, 0, 0}, {inst, 74, 0, 0, 0}, {track1, 200, 0, 0, 0}, {track1, 8, 17, 0, 0}, {track1, 7, 0, 1, 0},
                                                     {track0, 9, 2, 2, 0}})}});
  r = fs::read(learn.data(), learn.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 3u);
  CHECK(out.learn_count() == 2u);  // track 1 took controller 7 (and slot 0) from track 0, which then bound 9 in the next free slot
  CHECK(out.learn(0).param == static_cast<core::ParamId>(track1));
  CHECK(out.learn(0).cc == 7);
  CHECK(out.learn(0).mode == 1);
  CHECK(out.learn(1).param == static_cast<core::ParamId>(track0));
  CHECK(out.learn(1).cc == 9);
  CHECK(out.learn(1).channel == 2);
  CHECK(out.check_invariants());
  // LRNB before SETS is as good as after it; a chunk of another kind is ignored and counted.
  const Bytes reordered = build({{kLrnb, lrnb_payload({{track0, 7, 0, 0, 0}})}, {id4('X', 'Y', 'Z', 'W'), Bytes{1, 2, 3}}, {kSets, sets_payload({{dev(2), 5.0f}})}});
  r = fs::read(reordered.data(), reordered.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.unknown_chunks == 1u);
  CHECK(out.get(model::device_setting::MetronomeVolume) == 5.0f);
  CHECK(out.learn_count() == 1u);
}

TEST_CASE("settings file: versions follow the project format's rules") {
  model::Settings out = fresh();
  // A newer minor with appended fields (and a CRC chunk that is one too): read as far as this version knows it.
  const Bytes newer = build({{kSets, [] { Bytes p = sets_payload({{dev(2), 12.0f}}, 0x0101); p.insert(p.end(), 40, 0xAB); return p; }()},
                             {kLrnb, [&] { Bytes p = lrnb_payload({{static_cast<std::uint32_t>(core::param_id(core::ParamKind::Track, 0, 0)), 7, 0, 0, 0}}, 0x0105); p.insert(p.end(), 3, 0xCD); return p; }()}},
                            0x0102, 9);
  fs::Report r = fs::read(newer.data(), newer.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.newer_minor);
  CHECK(out.get(model::device_setting::MetronomeVolume) == 12.0f);
  CHECK(out.learn_count() == 1u);
  // A newer minor may carry more entries than this version can name (the cap is 64): it reads the first 64 and counts the rest as
  // skipped, where a chunk of this minor with such a count is malformed (the refusals below).
  std::vector<std::pair<std::uint32_t, float>> many_settings(70, {dev(40), 1.0f});  // no such setting
  many_settings[0] = {dev(2), 33.0f};
  const Bytes more_sets = build({{kSets, sets_payload(many_settings, 0x0101)}});
  r = fs::read(more_sets.data(), more_sets.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.newer_minor);
  CHECK(out.get(model::device_setting::MetronomeVolume) == 33.0f);
  CHECK(r.skipped_entries == 69u);  // 63 unknown among the first 64, and the 6 beyond them
  std::vector<Bind> many_bindings(70, Bind{0, 0, 0, 0, 0});  // empty bindings: nothing to bind
  many_bindings[0] = Bind{static_cast<std::uint32_t>(core::param_id(core::ParamKind::Track, 0, 0)), 7, 0, 0, 0};
  const Bytes more_lrnb = build({{kLrnb, lrnb_payload(many_bindings, 0x0101)}});
  r = fs::read(more_lrnb.data(), more_lrnb.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(out.learn_count() == 1u);
  CHECK(r.skipped_entries == 69u);
  // A newer major is refused, whatever its layout, and the block is the defaults, not half of the file.
  const Bytes major = build({{kSets, [] { Bytes p = sets_payload({{dev(2), 12.0f}}, 0x0200); p.insert(p.end(), 5, 1); return p; }()}});
  (void)out.set(model::device_setting::MetronomeVolume, 3.0f);
  r = fs::read(major.data(), major.size(), out);
  CHECK(r.error == fs::Error::NewerMajor);
  CHECK(out.is_default());
  const Bytes crc_major = build({}, 0x0200, 4);
  CHECK(fs::read(crc_major.data(), crc_major.size(), out).error == fs::Error::NewerMajor);
  // Version 0 is malformed; a short chunk has no version at all.
  const Bytes zero = build({{kSets, sets_payload({}, 0x0000)}});
  CHECK(fs::read(zero.data(), zero.size(), out).error == fs::Error::Malformed);
  const Bytes tiny = build({{kSets, Bytes{1}}});
  CHECK(fs::read(tiny.data(), tiny.size(), out).error == fs::Error::Malformed);
}

TEST_CASE("settings file: every refusal, and the block is the defaults after each") {
  model::Settings out = fresh();
  const auto refused = [&](const Bytes& file, fs::Error want, const char* what) {
    (void)out.set(model::device_setting::MetronomeVolume, 3.0f);
    const fs::Report r = fs::read(file.data(), file.size(), out);
    CHECK_MESSAGE(r.error == want, what << ": " << fs::error_name(r.error));
    CHECK_MESSAGE(out.is_default(), what);
    CHECK_MESSAGE(out.check_invariants(), what);
    CHECK_MESSAGE(r.detail != nullptr, what);
  };
  Bytes wrong_type = kDefaultFile;
  wrong_type[8] = 'P';
  refused(wrong_type, fs::Error::BadMagic, "O3PJ");
  Bytes wrong_form = kDefaultFile;
  wrong_form[0] = 'L';
  refused(wrong_form, fs::Error::BadMagic, "LORM");
  refused(Bytes{}, fs::Error::Truncated, "empty");
  refused(Bytes{'F', 'O', 'R'}, fs::Error::Truncated, "three bytes");
  refused(Bytes{'L', 'I', 'S', 'T', 0}, fs::Error::BadMagic, "five bytes of something else");
  Bytes odd_length = kDefaultFile;
  odd_length[7] = 0x55;
  refused(odd_length, fs::Error::Malformed, "odd FORM length");
  Bytes short_length = kDefaultFile;
  short_length[6] = 0;  // (the length is 262 since OS-MTC A3: two bytes)
  short_length[7] = 2;
  refused(short_length, fs::Error::Malformed, "FORM length 2");
  Bytes huge = kDefaultFile;
  huge[5] = 0x10;
  refused(huge, fs::Error::TooBig, "FORM of 1 MiB");
  Bytes flipped = kDefaultFile;
  flipped[30] ^= 1;
  refused(flipped, fs::Error::BadCrc, "one bit of the body");
  // The CRC chunk must be there, last, and right.
  Bytes no_crc(kDefaultFile.begin(), kDefaultFile.end() - 14);
  no_crc[6] = 0;
  no_crc[7] = 0x48;  // a FORM length that ends inside the SETS chunk (below the file, so not Truncated): 72
  refused(no_crc, fs::Error::Malformed, "no CRC chunk");
  const Bytes after_crc = [] {  // a chunk after the CRC chunk
    Bytes f = kDefaultFile;
    f[7] = static_cast<std::uint8_t>(f[7] + 8);
    f.insert(f.end(), {'J', 'U', 'N', 'K', 0, 0, 0, 0});
    return f;
  }();
  refused(after_crc, fs::Error::Malformed, "a chunk after the CRC");
  refused(build({{kSets, sets_payload({{dev(2), 1.0f}})}, {kSets, sets_payload({{dev(3), 1.0f}})}}), fs::Error::Malformed, "SETS twice");
  refused(build({{kLrnb, lrnb_payload({})}, {kLrnb, lrnb_payload({})}}), fs::Error::Malformed, "LRNB twice");
  Bytes count_high = sets_payload({{dev(2), 1.0f}});
  count_high[3] = 2;
  refused(build({{kSets, count_high}}), fs::Error::Malformed, "SETS count above its length");
  Bytes long_sets = sets_payload({{dev(2), 1.0f}});
  long_sets.push_back(0);
  refused(build({{kSets, long_sets}}), fs::Error::Malformed, "SETS a byte long");
  Bytes count_65 = sets_payload({});
  count_65[3] = 65;
  count_65.insert(count_65.end(), 65 * 8, 0);
  refused(build({{kSets, count_65}}), fs::Error::Malformed, "SETS 65 entries");
  Bytes lrnb_65 = lrnb_payload({});
  lrnb_65[3] = 65;
  lrnb_65.insert(lrnb_65.end(), 65 * 8, 0);
  refused(build({{kLrnb, lrnb_65}}), fs::Error::Malformed, "LRNB 65 entries");
  Bytes lrnb_long = lrnb_payload({});
  lrnb_long.push_back(0);
  refused(build({{kLrnb, lrnb_long}}), fs::Error::Malformed, "LRNB a byte long");
  refused(build({{id4('b', 'a', 'd', 1), Bytes{0}}}), fs::Error::Malformed, "an id that is not printable");
  // A pad byte that is not zero.
  Bytes bad_pad = build({{id4('J', 'U', 'N', 'K'), Bytes{1, 2, 3}}});
  bad_pad[12 + 8 + 3] = 0x55;
  const std::uint32_t crc = formats::crc32(bad_pad.data(), bad_pad.size() - 14);
  for (int i = 0; i < 4; ++i) {
    bad_pad[bad_pad.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (24 - 8 * i));
  }
  refused(bad_pad, fs::Error::Malformed, "pad byte not zero");
  // A file cut anywhere short of its FORM.
  for (std::size_t cut = 0; cut < kDefaultFile.size(); ++cut) {
    (void)out.set(model::device_setting::MetronomeVolume, 3.0f);
    const fs::Report r = fs::read(kDefaultFile.data(), cut, out);
    CHECK_MESSAGE((r.error == fs::Error::Truncated || (cut >= 4 && r.error == fs::Error::BadMagic && false)), "cut at " << cut);
    CHECK(out.is_default());
  }
  // Bytes after the FORM are not looked at.
  Bytes extra = kDefaultFile;
  extra.insert(extra.end(), 20, 0xEE);
  CHECK(fs::read(extra.data(), extra.size(), out).error == fs::Error::None);
  CHECK(fs::read(nullptr, 0, out).error == fs::Error::Truncated);
}

TEST_CASE("settings file: no single flipped bit of the file is accepted, but the CRC chunk's minor version, which a newer file may raise") {
  model::Settings out = fresh();
  Bytes bits = written([] {
    model::Settings s = fresh();
    (void)s.set(model::device_setting::MetronomeVolume, 61.0f);
    (void)s.set_learn(0, core::LearnBinding{core::param_id(core::ParamKind::Track, 3, 0), 7, 2, 1, 0});
    return s;
  }());
  NEED(bits.size() == 306u);  // 298 before OS-LEDBPM: one more SETS entry (290 before OS-MTC B3)
  // The low byte of the CRC chunk's version, which the CRC does not cover (the SETS and LRNB versions at 21 and 273 are in what it covers):
  // a newer minor of the CRC chunk is read, by the rule of the format.
  const auto is_minor = [](std::size_t i) { return i == 301; };  // +8 since OS-LEDBPM appended STATUS LED to SETS (293 before)
  unsigned accepted = 0;
  for (std::size_t i = 0; i < bits.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      bits[i] ^= static_cast<std::uint8_t>(1u << bit);
      const fs::Report r = fs::read(bits.data(), bits.size(), out);
      // Every other flip makes a different file, which the CRC (or the structure before it) refuses.
      if (is_minor(i)) {
        CHECK_MESSAGE((r.error == fs::Error::None && r.newer_minor), "byte " << i << " bit " << bit);
        ++accepted;
      } else {
        CHECK_MESSAGE(r.error != fs::Error::None, "byte " << i << " bit " << bit);
        CHECK(out.is_default());
      }
      bits[i] ^= static_cast<std::uint8_t>(1u << bit);
    }
  }
  CHECK(accepted == 8u);
}

TEST_CASE("settings file: damage with a good CRC never leaves an invalid block, and what is read writes back to what is read") {
  std::uint32_t seed = 987654u;
  const auto next = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  model::Settings s = fresh();
  (void)s.set(model::device_setting::ClockSync, 4.0f);
  (void)s.set_learn(0, core::LearnBinding{core::param_id(core::ParamKind::Track, 3, 0), 7, 2, 1, 0});
  (void)s.set_learn(1, core::LearnBinding{core::param_id(core::ParamKind::Master, 0, 1), 8, 0, 0, 0});
  const Bytes good = written(s);
  NEED(good.size() > 100);
  unsigned read_ok = 0;
  for (unsigned n = 0; n < 6000 / kScale; ++n) {
    Bytes f = good;
    const unsigned edits = 1 + next() % 3;
    for (unsigned e = 0; e < edits; ++e) {
      const std::size_t at = 12 + next() % (f.size() - 12 - 14);  // inside the chunks, before the CRC chunk
      f[at] = static_cast<std::uint8_t>(next());
    }
    const std::uint32_t crc = formats::crc32(f.data(), f.size() - 14);  // a good CRC: the damage is for the parser, not the checksum
    for (int i = 0; i < 4; ++i) {
      f[f.size() - 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (24 - 8 * i));
    }
    model::Settings out = fresh();
    const fs::Report r = fs::read(f.data(), f.size(), out);
    CHECK(out.check_invariants());
    if (r.error == fs::Error::None) {
      ++read_ok;
      const Bytes again = written(out);
      NEED(!again.empty());
      model::Settings back = fresh();
      CHECK(fs::read(again.data(), again.size(), back).error == fs::Error::None);
      CHECK(same(back, out));
    } else {
      CHECK(out.is_default());
    }
  }
  CHECK(read_ok > 100u / kScale);  // plenty of the damaged files still read: the damage landed in values and bindings
}

TEST_CASE("settings file: write refuses what it cannot write") {
  Bytes out(fs::kMaxFileBytes);
  model::Settings project;
  std::memset(&project, 0, sizeof project);
  project.init(core::ParamKind::Project);
  CHECK(fs::write(project, out.data(), out.size()) == 0u);  // the project's settings are in the project file
  model::Settings uninitialised;
  std::memset(&uninitialised, 0, sizeof uninitialised);
  CHECK(fs::write(uninitialised, out.data(), out.size()) == 0u);
  const model::Settings d = fresh();
  CHECK(fs::write(d, out.data(), 285) == 0u);  // one byte short
  CHECK(fs::write(d, out.data(), 286) == 286u);
  CHECK(fs::write(d, nullptr, 286) == 0u);
}

// OS-011a appended six device settings (the MIDI clock and ports, the channels, the theme). The settings file written before it (7 settings, 94 bytes, valid CRC) still
// reads, with no entry skipped: what it does not mention is the default (docs/formats/o3-settings.md section 3).
TEST_CASE("settings file: a file written before the Config settings existed reads, and the new settings are their defaults; a newer file's extra entries are skipped and counted") {
  const Bytes old_default = {
      0x46, 0x4f, 0x52, 0x4d, 0x00, 0x00, 0x00, 0x56, 0x4f, 0x33, 0x53, 0x54, 0x53, 0x45, 0x54, 0x53, 0x00, 0x00, 0x00, 0x3c, 0x01, 0x00, 0x00, 0x07,
      0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x42, 0x48, 0x00, 0x00,
      0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x04, 0x3f, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
      0x05, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x43, 0x52, 0x43, 0x20, 0x00, 0x00, 0x00, 0x06, 0x01, 0x00, 0xe5, 0x4c, 0xcf, 0xba,
  };
  model::Settings out = fresh();
  (void)out.set(model::device_setting::Theme, 2.0f);  // something to be overwritten by the default
  (void)out.set(model::device_setting::MidiOutChannel, 9.0f);
  const fs::Report r = fs::read(old_default.data(), old_default.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  model::Settings expect = fresh();
  (void)expect.set(model::device_setting::MiddleC, 0.0f);  // the file says C-3 (the provisional default of the time; OS-CFGWIRE made it C-5): a file keeps what it holds
  CHECK(same(out, expect));
  CHECK(out.get(model::device_setting::MiddleC) == 0.0f);
  CHECK(out.get(model::device_setting::Theme) == 0.0f);
  CHECK(out.get(model::device_setting::MidiOutChannel) == 1.0f);
  CHECK(out.get(model::device_setting::ClockMode) == 0.0f);
  // The new defaults are what a device that never heard of them did: no clock out, both ports, every channel in, channel 1 out.
  CHECK(out.get(model::device_setting::MidiInPort) == 2.0f);
  CHECK(out.get(model::device_setting::MidiOutPort) == 2.0f);
  CHECK(out.get(model::device_setting::MidiInChannel) == 0.0f);
  // The new settings round trip.
  model::Settings s = fresh();
  CHECK(s.set(model::device_setting::ClockMode, 2.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::MidiInPort, 1.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::MidiInChannel, 16.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::MidiOutChannel, 16.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  CHECK(s.set(model::device_setting::MidiOutChannel, 17.0f) == model::Status::BadValue);
  CHECK(s.set(model::device_setting::MidiOutChannel, 0.0f) == model::Status::BadValue);  // 1..16
  const Bytes bytes = written(s);
  model::Settings back = fresh();
  CHECK(fs::read(bytes.data(), bytes.size(), back).error == fs::Error::None);
  CHECK(same(s, back));
}

// OS-USBRET appended USB RETURN (field 13, On by default: the descriptors before it). The setting is a row appended by the rule of docs/formats/o3-settings.md section 2: no new chunk,
// no version bump. The file written before it (13 settings, 142 bytes) reads with no entry skipped and RETURN On; a file with the setting Off round trips. (An older reader, which knows
// 13 fields, skips the 14th entry and counts it: the rule the OS-011a test above holds for any entry a build does not know.)
TEST_CASE("settings file: USB RETURN is appended without a version bump; a file from before it reads RETURN On; Off round trips") {
  namespace ds = model::device_setting;
  CHECK(ds::UsbReturn == 13u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP
  CHECK(fresh().get(ds::UsbReturn) == 1.0f);  // On by default
  CHECK(kFile13.size() == 142u);
  model::Settings out = fresh();
  (void)out.set(ds::UsbReturn, 0.0f);  // something to be overwritten by the default
  const fs::Report r = fs::read(kFile13.data(), kFile13.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  CHECK_FALSE(r.newer_minor);
  CHECK(out.get(ds::UsbReturn) == 1.0f);  // On: what a device without the setting did
  CHECK(same(out, fresh()));
  // Off and On round trip, byte for byte, and nothing else moves; the value is a switch (0 or 1).
  model::Settings s = fresh();
  CHECK(s.set(ds::UsbReturn, 0.0f) == model::Status::Ok);
  CHECK(s.set(ds::UsbReturn, 2.0f) == model::Status::BadValue);
  CHECK(s.set(ds::UsbReturn, -1.0f) == model::Status::BadValue);
  const Bytes bytes = written(s);
  CHECK(bytes.size() == 286u);
  model::Settings back = fresh();
  CHECK(fs::read(bytes.data(), bytes.size(), back).error == fs::Error::None);
  CHECK(same(s, back));
  CHECK(back.get(ds::UsbReturn) == 0.0f);
  CHECK(written(back) == bytes);
  // The only byte that differs from the default file is the value of entry 13 (and the CRC).
  std::size_t diffs = 0;
  for (std::size_t i = 0; i < bytes.size() - 6; ++i) {
    diffs += bytes[i] != kDefaultFile[i] ? 1u : 0u;
  }
  CHECK(diffs == 2u);  // 0x3f80 -> 0x0000 in the float's two significant bytes
}

// OS-006e appended RETURN LEVEL (field 16, the USB return's level to the outputs, 100 = unity by default) by the same rule, after two reserved rows (14, 15: OS-LIVEREC's ids, params.h). The file
// of the build before it (14 settings, 150 bytes) reads with no entry skipped and RETURN LEVEL 100: a host that plays into the Tracker is heard at the level it sends. A file with the
// setting written by this build and read by the build before has three entries that build does not know (the two reserved ones are 0, the level is 100, all skipped and counted).
TEST_CASE("settings file: RETURN LEVEL is appended without a version bump; a file from before it reads 100 (unity); other levels round trip") {
  namespace ds = model::device_setting;
  CHECK(ds::UsbReturnLevel == 16u);
  CHECK(fresh().get(ds::UsbReturnLevel) == 100.0f);  // unity by default
  CHECK(kFile14.size() == 150u);
  model::Settings out = fresh();
  (void)out.set(ds::UsbReturnLevel, 40.0f);  // something to be overwritten by the default
  const fs::Report r = fs::read(kFile14.data(), kFile14.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  CHECK_FALSE(r.newer_minor);
  CHECK(out.get(ds::UsbReturnLevel) == 100.0f);
  CHECK(same(out, fresh()));
  // A level in range round trips byte for byte; out of range is refused by the block and skipped by the reader (counted), the setting keeping the default.
  model::Settings s = fresh();
  CHECK(s.set(ds::UsbReturnLevel, 40.0f) == model::Status::Ok);
  CHECK(s.set(ds::UsbReturnLevel, 101.0f) == model::Status::BadValue);
  CHECK(s.set(ds::UsbReturnLevel, -1.0f) == model::Status::BadValue);
  const Bytes bytes = written(s);
  CHECK(bytes.size() == 286u);
  model::Settings back = fresh();
  CHECK(fs::read(bytes.data(), bytes.size(), back).error == fs::Error::None);
  CHECK(back.get(ds::UsbReturnLevel) == 40.0f);
  CHECK(written(back) == bytes);
  const Bytes bad = build({{kSets, sets_payload({{dev(ds::UsbReturnLevel), 101.0f}, {dev(ds::MetronomeVolume), 20.0f}})}});
  model::Settings from_bad = fresh();
  const fs::Report rb = fs::read(bad.data(), bad.size(), from_bad);
  CHECK(rb.error == fs::Error::None);
  CHECK(rb.skipped_entries == 1u);
  CHECK(from_bad.get(ds::UsbReturnLevel) == 100.0f);
  CHECK(from_bad.get(ds::MetronomeVolume) == 20.0f);
  // The level is a learnable parameter of the device store (a controller moves it), the reserved rows are not.
  const core::ParamDesc* level = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::UsbReturnLevel));
  NEED(level != nullptr);
  CHECK((level->flags & core::kParamLearnable) != 0);
  CHECK(std::strcmp(level->name, "RETURN LEVEL") == 0);
  CHECK(std::strlen(level->name) <= core::kParamNameChars);
}

// OS-LIVEREC appended REC POLY (field 14, Off) and REC QUANTIZE (field 15, Nearest step) by the same rule, with the defaults 0. OS-006e, written meanwhile, held ids 14 and 15 as two rows nothing
// read (value 0, no page) and appended RETURN LEVEL at 16; the merge makes 14 and 15 the REC rows, and a file of either branch loads: a LIVEREC file has no entry 16 and reads RETURN LEVEL 100; an OS-006e
// file has 0 under ids 14 and 15 and reads Off / Nearest step. The file of the build before both (14 settings, 150 bytes) reads the defaults of all three, and a build that knows 14 fields sees
// entries 14, 15 and 16 as settings it does not have: skipped and counted, everything else read.
TEST_CASE("settings file: REC POLY and REC QUANTIZE are rows 14 and 15; files of the OS-LIVEREC branch and of OS-006e both load; they round trip; an older reader skips them") {
  namespace ds = model::device_setting;
  CHECK(ds::RecPoly == 14u);
  CHECK(ds::RecQuantize == 15u);
  CHECK(ds::UsbReturnLevel == 16u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP
  CHECK(fresh().get(ds::RecPoly) == 0.0f);      // Off
  CHECK(fresh().get(ds::RecQuantize) == 0.0f);  // Nearest step
  // The file before both: nothing skipped, the three at their defaults, and what it says kept.
  model::Settings out = fresh();
  (void)out.set(ds::RecPoly, 1.0f);  // something to be overwritten by the default
  (void)out.set(ds::RecQuantize, 1.0f);
  (void)out.set(ds::UsbReturnLevel, 40.0f);
  fs::Report r = fs::read(kFile14.data(), kFile14.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  CHECK_FALSE(r.newer_minor);
  CHECK(same(out, fresh()));
  // A file of the OS-LIVEREC branch (16 entries, 166 bytes): no RETURN LEVEL in it, which reads 100 (unity); the rest as it says.
  CHECK(kFile16.size() == 166u);
  out = fresh();
  (void)out.set(ds::UsbReturnLevel, 40.0f);
  r = fs::read(kFile16.data(), kFile16.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  CHECK(out.get(ds::UsbReturnLevel) == 100.0f);
  CHECK(same(out, fresh()));
  std::vector<std::pair<std::uint32_t, float>> live;  // the same branch's file with Spill, Playing step and a metronome level
  for (unsigned f = 0; f < ds::RecPoly; ++f) {
    live.push_back({dev(f), f == ds::MetronomeVolume ? 77.0f : fresh().get(f)});
  }
  live.push_back({dev(ds::RecPoly), 1.0f});
  live.push_back({dev(ds::RecQuantize), 1.0f});
  const Bytes live_file = build({{kSets, sets_payload(live)}});
  out = fresh();
  r = fs::read(live_file.data(), live_file.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  CHECK(out.get(ds::RecPoly) == 1.0f);
  CHECK(out.get(ds::RecQuantize) == 1.0f);
  CHECK(out.get(ds::UsbReturnLevel) == 100.0f);
  CHECK(out.get(ds::MetronomeVolume) == 77.0f);
  // A file of the OS-006e branch (17 entries: ids 14 and 15 written as 0 by its reserved rows, RETURN LEVEL 40): REC POLY Off, REC QUANTIZE Nearest step, the level as written.
  std::vector<std::pair<std::uint32_t, float>> six;
  for (unsigned f = 0; f < ds::RecPoly; ++f) {
    six.push_back({dev(f), fresh().get(f)});
  }
  six.push_back({dev(14), 0.0f});
  six.push_back({dev(15), 0.0f});
  six.push_back({dev(16), 40.0f});
  const Bytes six_file = build({{kSets, sets_payload(six)}});
  out = fresh();
  (void)out.set(ds::RecPoly, 1.0f);
  (void)out.set(ds::RecQuantize, 1.0f);
  r = fs::read(six_file.data(), six_file.size(), out);
  CHECK(r.error == fs::Error::None);
  CHECK(r.skipped_entries == 0u);
  CHECK(out.get(ds::RecPoly) == 0.0f);  // the entries say 0: Off, Nearest step, whatever the block held
  CHECK(out.get(ds::RecQuantize) == 0.0f);
  CHECK(out.get(ds::UsbReturnLevel) == 40.0f);
  // Both branches write 0.0 under ids 14 and 15 by default: the merged default file is OS-006e's literal, and entries 14 and 15 of it are zero.
  const Bytes defaults = written(fresh());
  CHECK(defaults == kDefaultFile);
  CHECK((defaults[24 + 8 * 14 + 3] == 14 && defaults[24 + 8 * 14 + 4] == 0 && defaults[24 + 8 * 14 + 5] == 0));
  CHECK((defaults[24 + 8 * 15 + 3] == 15 && defaults[24 + 8 * 15 + 4] == 0 && defaults[24 + 8 * 15 + 5] == 0));
  // Spill and Playing step round trip, byte for byte; each is a choice of two.
  model::Settings s = fresh();
  CHECK(s.set(ds::RecPoly, 1.0f) == model::Status::Ok);
  CHECK(s.set(ds::RecQuantize, 1.0f) == model::Status::Ok);
  CHECK(s.set(ds::RecPoly, 2.0f) == model::Status::BadValue);
  CHECK(s.set(ds::RecQuantize, -1.0f) == model::Status::BadValue);
  CHECK(s.set(ds::MetronomeVolume, 77.0f) == model::Status::Ok);
  const Bytes bytes = written(s);
  CHECK(bytes.size() == 286u);
  model::Settings back = fresh();
  const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
  CHECK(rb.error == fs::Error::None);
  CHECK(rb.skipped_entries == 0u);
  CHECK(same(s, back));
  CHECK(back.get(ds::RecPoly) == 1.0f);
  CHECK(back.get(ds::RecQuantize) == 1.0f);
  CHECK(written(back) == bytes);
  CHECK(bytes[24 + 8 * 16 + 3] == 16);  // RETURN LEVEL follows them (USB AUDIO, OS-USBAUDIO, and METRO and COUNT-IN, OS-METRO, come after)
  CHECK(bytes[24 + 8 * 26 + 3] == 26);  // the last entry is OS-MIDICCMAP S1's, id 26 (E-PAT-35's id 25 is the one before)
  CHECK(bytes[24 + 8 * 31] == 'C');     // the CRC chunk follows
  // The older reader, as the codec sees it: a build of 14 settings has no setting under ids 14, 15 and 16. They are skipped and counted; the rest is read.
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::RecPoly; ++f) {
    entries.push_back({dev(f), s.get(f)});
  }
  entries.push_back({dev(ds::kCount), 1.0f});
  entries.push_back({dev(ds::kCount + 1), 1.0f});
  entries.push_back({dev(ds::kCount + 2), 100.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings seen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), seen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 3u);
  CHECK(seen.get(ds::MetronomeVolume) == 77.0f);
  CHECK(seen.get(ds::UsbReturn) == 1.0f);
  CHECK(seen.get(ds::RecPoly) == 0.0f);  // the defaults: nothing of the unknown entries got in
  CHECK(seen.get(ds::RecQuantize) == 0.0f);
  CHECK(seen.get(ds::UsbReturnLevel) == 100.0f);
  CHECK(seen.check_invariants());
  // Only the two values differ from the default file (0.0 -> 1.0 is 0x3f800000: two bytes each), apart from the CRC.
  model::Settings two = fresh();
  CHECK(two.set(ds::RecPoly, 1.0f) == model::Status::Ok);
  CHECK(two.set(ds::RecQuantize, 1.0f) == model::Status::Ok);
  const Bytes two_bytes = written(two);
  NEED(two_bytes.size() == kDefaultFile.size());
  std::size_t diffs = 0;
  for (std::size_t i = 0; i < two_bytes.size() - 6; ++i) {
    diffs += two_bytes[i] != kDefaultFile[i] ? 1u : 0u;
  }
  CHECK(diffs == 4u);
}

// OS-USBAUDIO appended USB AUDIO (field 17: Off = STANDARD, On = the audio device; On by default, the maintainer's decision) by the same rule: no chunk, no version bump. Every file of a build before it
// reads with no entry skipped and USB AUDIO On: the 17 settings of OS-006e merged with OS-LIVEREC (174 bytes, the previous default file), the 16 of the OS-LIVEREC branch alone (166), the 14 of the build
// before both (150) and the 13 before OS-USBRET (142). Off round trips; a value outside 0..1 is refused. A build that lacks the row (every build before this one) meets entry 17 as an id it does not
// know, which the reader skips and counts: the entry of an id past kCount stands for it.
TEST_CASE("settings file: USB AUDIO is appended without a version bump; files of 13, 14, 16 and 17 settings read USB AUDIO On; Off round trips; an older reader skips it") {
  namespace ds = model::device_setting;
  CHECK(ds::UsbAudio == 17u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP
  CHECK(ds::UsbAudio + 14u == ds::kCount);  // the rows appended behind it (ids 18 to 30: OS-METRO, OS-RECMICRO, OS-PADOFFSET, E-CFG-03, E-PAT-35, OS-MIDICCMAP S1, OS-MTC A3, OS-MTC B3, OS-LEDBPM) keep the ones before at their ids
  CHECK(fresh().get(ds::UsbAudio) == 1.0f);  // On by default
  CHECK(kDefaultFile.size() == 286u);
  CHECK(kFile18.size() == 182u);
  CHECK(kFile17.size() == 174u);
  CHECK(kFile16.size() == 166u);
  CHECK(kFile14.size() == 150u);
  CHECK(kFile13.size() == 142u);
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17}) {
    model::Settings out = fresh();
    (void)out.set(ds::UsbAudio, 0.0f);  // something to be overwritten by the default
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::UsbAudio) == 1.0f, file->size() << " bytes");  // On: the unit comes up as the audio device
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  // A file that has the older rows moved keeps them: only the new row is the default.
  {
    model::Settings out = fresh();
    const fs::Report r = fs::read(kFile17.data(), kFile17.size(), out);
    CHECK(r.error == fs::Error::None);
    CHECK(out.get(ds::UsbReturn) == 1.0f);
    CHECK(out.get(ds::UsbReturnLevel) == 100.0f);
  }
  // Off and On round trip, byte for byte, and nothing else moves; the value is a switch (0 or 1).
  model::Settings s = fresh();
  CHECK(s.set(ds::UsbAudio, 0.0f) == model::Status::Ok);
  CHECK(s.set(ds::UsbAudio, 2.0f) == model::Status::BadValue);
  CHECK(s.set(ds::UsbAudio, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::UsbAudio) == 0.0f);
  const Bytes bytes = written(s);
  CHECK(bytes.size() == 286u);
  model::Settings back = fresh();
  const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
  CHECK(rb.error == fs::Error::None);
  CHECK(rb.skipped_entries == 0u);
  CHECK(same(s, back));
  CHECK(back.get(ds::UsbAudio) == 0.0f);
  CHECK(written(back) == bytes);
  CHECK(bytes[24 + 8 * 17 + 3] == 17);  // USB AUDIO is entry 17 of SETS (an early entry; five follow since OS-RECMICRO)
  CHECK(bytes[24 + 8 * 26 + 3] == 26);  // the last entry is OS-MIDICCMAP S1's, id 26 (E-PAT-35's id 25 is the one before)
  CHECK(bytes[24 + 8 * 31] == 'C');     // the CRC chunk follows
  // The only bytes that differ from the default file are the value of entry 17 (0x3f80 -> 0x0000) and the CRC.
  std::size_t diffs = 0;
  for (std::size_t i = 0; i < bytes.size() - 6; ++i) {
    diffs += bytes[i] != kDefaultFile[i] ? 1u : 0u;
  }
  CHECK(diffs == 2u);
  // Off together with the other USB rows, and every row moved: the largest file still round trips.
  CHECK(s.set(ds::UsbReturn, 0.0f) == model::Status::Ok);
  CHECK(s.set(ds::UsbReturnLevel, 25.0f) == model::Status::Ok);
  const Bytes more = written(s);
  model::Settings more_back = fresh();
  CHECK(fs::read(more.data(), more.size(), more_back).error == fs::Error::None);
  CHECK(same(s, more_back));
  CHECK(more_back.get(ds::UsbAudio) == 0.0f);
  CHECK(more_back.get(ds::UsbReturn) == 0.0f);
  CHECK(more_back.get(ds::UsbReturnLevel) == 25.0f);
  // The older reader, as the codec sees it: a build without the row has no setting under id 17 (here: under kCount, the first id this build lacks). The entry is skipped and counted, the rest is
  // read, and the setting the old build lacks stays at the default whatever the entry said.
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::UsbAudio; ++f) {
    entries.push_back({dev(f), s.get(f)});
  }
  entries.push_back({dev(ds::kCount), 0.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings seen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), seen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 1u);
  CHECK(seen.get(ds::UsbReturn) == 0.0f);
  CHECK(seen.get(ds::UsbReturnLevel) == 25.0f);
  CHECK(seen.get(ds::UsbAudio) == 1.0f);  // the default: nothing of the unknown entry got in
  CHECK(seen.check_invariants());
  // The descriptor: a switch, On, press to apply, so not learnable (a controller must not re-enumerate the USB port), and the name fits a row.
  const core::ParamDesc* audio = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::UsbAudio));
  NEED(audio != nullptr);
  CHECK(std::strcmp(audio->name, "USB AUDIO") == 0);
  CHECK(std::strlen(audio->name) <= core::kParamNameChars);
  CHECK((audio->flags & core::kParamLearnable) == 0);
  CHECK((audio->flags & core::kParamApply) != 0);
  CHECK(audio->def == 1.0f);
  CHECK(model::param_group_name(core::ParamKind::Device, audio->group) == std::string("USB"));
}

// OS-METRO appended METRO (field 18: Off, Cue, Main; the default is Cue) and COUNT-IN (field 19: Off, 1 bar, 2 bars; the default is Off) by the same rule, no chunk, no version bump (docs/formats/o3-settings.md
// section 3, "Fields 18 and 19"). METRO VOL (field 2) is the same row it always was: only its page changed (MIDI to PATTERN), which is not part of the file. Every file written before them reads with no entry
// skipped and Cue / Off; a file written by this build and read by a build that lacks the rows meets two ids it does not know (skipped and counted).
TEST_CASE("settings file: METRO and COUNT-IN are appended without a version bump; every older file reads Cue / Off; they round trip; an older reader skips them") {
  namespace ds = model::device_setting;
  CHECK(ds::MetroMode == 18u);
  CHECK(ds::CountIn == 19u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP
  CHECK(ds::MetronomeVolume == 2u);  // METRO VOL keeps its id and its range: only the page moved
  CHECK(fresh().get(ds::MetroMode) == 1.0f);  // Cue
  CHECK(fresh().get(ds::CountIn) == 0.0f);    // Off
  CHECK(fresh().get(ds::MetronomeVolume) == 50.0f);
  CHECK(kFile18.size() == 182u);
  CHECK(kDefaultFile.size() == 286u);
  // The files of every build before this one: nothing skipped, the two at their defaults, what the file says kept.
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18}) {
    model::Settings out = fresh();
    (void)out.set(ds::MetroMode, 2.0f);  // something to be overwritten by the default
    (void)out.set(ds::CountIn, 2.0f);
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::MetroMode) == 1.0f, file->size() << " bytes");
    CHECK_MESSAGE(out.get(ds::CountIn) == 0.0f, file->size() << " bytes");
    CHECK(out.get(ds::MetronomeVolume) == 50.0f);
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  // Every value of both rows round trips, byte for byte, and nothing else moves; the ranges are 0..2.
  for (unsigned mode = 0; mode <= 2u; ++mode) {
    for (unsigned bars = 0; bars <= 2u; ++bars) {
      model::Settings s = fresh();
      CHECK(s.set(ds::MetroMode, static_cast<float>(mode)) == model::Status::Ok);
      CHECK(s.set(ds::CountIn, static_cast<float>(bars)) == model::Status::Ok);
      CHECK(s.set(ds::MetronomeVolume, 80.0f) == model::Status::Ok);
      const Bytes bytes = written(s);
      CHECK(bytes.size() == 286u);
      model::Settings back = fresh();
      const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
      CHECK(rb.error == fs::Error::None);
      CHECK(rb.skipped_entries == 0u);
      CHECK(same(s, back));
      CHECK(back.get(ds::MetroMode) == static_cast<float>(mode));
      CHECK(back.get(ds::CountIn) == static_cast<float>(bars));
      CHECK(back.get(ds::MetronomeVolume) == 80.0f);
      CHECK(written(back) == bytes);
      CHECK(bytes[24 + 8 * 18 + 3] == 18);  // METRO is entry 18, COUNT-IN entry 19 (REC MICRO and REC VELOCITY follow, OS-RECMICRO)
      CHECK(bytes[24 + 8 * 19 + 3] == 19);
      CHECK(bytes[24 + 8 * 26 + 3] == 26);  // the last entry is OS-MIDICCMAP S1's, id 26 (E-PAT-35's id 25 is the one before)
      CHECK(bytes[24 + 8 * 31] == 'C');  // the CRC chunk follows
    }
  }
  model::Settings s = fresh();
  CHECK(s.set(ds::MetroMode, 3.0f) == model::Status::BadValue);
  CHECK(s.set(ds::MetroMode, -1.0f) == model::Status::BadValue);
  CHECK(s.set(ds::CountIn, 3.0f) == model::Status::BadValue);
  CHECK(s.set(ds::CountIn, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::MetroMode) == 1.0f);
  CHECK(s.get(ds::CountIn) == 0.0f);
  // A value outside the range in a file is skipped and counted, the setting keeping its default.
  {
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::kCount; ++f) {
      entries.push_back({dev(f), fresh().get(f)});
    }
    entries[ds::MetroMode].second = 7.0f;
    entries[ds::CountIn].second = 2.0f;
    const Bytes bad = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report r = fs::read(bad.data(), bad.size(), seen);
    CHECK(r.error == fs::Error::None);
    CHECK(r.skipped_entries == 1u);
    CHECK(seen.get(ds::MetroMode) == 1.0f);
    CHECK(seen.get(ds::CountIn) == 2.0f);
    CHECK(seen.check_invariants());
  }
  // The older reader, as the codec sees it: a build without the rows has no setting under ids 18 and 19 (here: under kCount and kCount + 1, the first ids this build lacks). Both entries are skipped and
  // counted, the rest is read, and the two settings the old build lacks stay at their defaults whatever the entries said.
  {
    model::Settings moved = fresh();
    CHECK(moved.set(ds::UsbAudio, 0.0f) == model::Status::Ok);
    CHECK(moved.set(ds::MetronomeVolume, 20.0f) == model::Status::Ok);
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::MetroMode; ++f) {
      entries.push_back({dev(f), moved.get(f)});
    }
    entries.push_back({dev(ds::kCount), 2.0f});
    entries.push_back({dev(ds::kCount + 1u), 2.0f});
    const Bytes older = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report ro = fs::read(older.data(), older.size(), seen);
    CHECK(ro.error == fs::Error::None);
    CHECK(ro.skipped_entries == 2u);
    CHECK(seen.get(ds::UsbAudio) == 0.0f);
    CHECK(seen.get(ds::MetronomeVolume) == 20.0f);
    CHECK(seen.get(ds::MetroMode) == 1.0f);
    CHECK(seen.get(ds::CountIn) == 0.0f);
    CHECK(seen.check_invariants());
  }
  // The descriptors: two choices, learnable, named to fit a row, and the three metronome rows share the PATTERN page (group 3) with the live recording's two.
  const core::ParamDesc* metro = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::MetroMode));
  const core::ParamDesc* count = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::CountIn));
  const core::ParamDesc* vol = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::MetronomeVolume));
  NEED(metro != nullptr);
  NEED(count != nullptr);
  NEED(vol != nullptr);
  CHECK(std::strcmp(metro->name, "METRO") == 0);
  CHECK(std::strcmp(count->name, "COUNT-IN") == 0);
  CHECK(std::strcmp(vol->name, "METRO VOL") == 0);
  CHECK(std::strlen(count->name) <= core::kParamNameChars);
  CHECK((metro->flags & core::kParamLearnable) != 0);
  CHECK((count->flags & core::kParamLearnable) != 0);
  CHECK(metro->def == 1.0f);
  CHECK(count->def == 0.0f);
  CHECK(metro->max == 2.0f);
  CHECK(count->max == 2.0f);
  CHECK(std::strcmp(metro->names[0], "Off") == 0);
  CHECK(std::strcmp(metro->names[1], "Cue") == 0);
  CHECK(std::strcmp(metro->names[2], "Main") == 0);
  CHECK(std::strcmp(count->names[1], "1 bar") == 0);
  CHECK(std::strcmp(count->names[2], "2 bars") == 0);
  CHECK(metro->group == vol->group);
  CHECK(count->group == vol->group);
  CHECK(model::param_group_name(core::ParamKind::Device, vol->group) == std::string("PATTERN"));
  std::uint16_t fields[12];
  const unsigned n = model::param_group_fields(core::ParamKind::Device, vol->group, 0, fields, 12);
  NEED(n == 9u);  // table order: METRO VOL, REC POLY, REC QUANTIZE, METRO, COUNT-IN, REC MICRO, REC VELOCITY, PAD OFFSET, PAT SWITCH (the Config page puts the rows in the order of the spec)
  CHECK(fields[0] == ds::MetronomeVolume);
  CHECK(fields[1] == ds::RecPoly);
  CHECK(fields[2] == ds::RecQuantize);
  CHECK(fields[3] == ds::MetroMode);
  CHECK(fields[4] == ds::CountIn);
  CHECK(fields[5] == ds::RecMicro);
  CHECK(fields[6] == ds::RecVelocity);
  CHECK(fields[7] == ds::PadOffset);
  CHECK(fields[8] == ds::PatSwitch);
  // The MIDI page lost METRO VOL, the room is for OS-MIDI2.
  const unsigned midi = model::param_group_fields(core::ParamKind::Device, 0, 0, fields, 12);
  for (unsigned i = 0; i < midi; ++i) {
    CHECK(fields[i] != ds::MetronomeVolume);
  }
}

// OS-RECMICRO appended REC MICRO (field 20: Off, On) and REC VELOCITY (field 21: Off, External), both Off by default, by the same rule: no chunk, no version bump (docs/formats/o3-settings.md section 3,
// "Fields 20 and 21"). A single "Rec Quantization" setting of the official firmware is not stored: its value is not a field of this format (docs/specs/OS-RECMICRO.md). Every file written before them
// (the literals of 13 to 20 settings) reads with no entry skipped and both Off; a file written by this build and read by one that lacks the rows meets two ids it does not know.
TEST_CASE("settings file: REC MICRO and REC VELOCITY are appended without a version bump; every older file reads Off / Off; they round trip; an older reader skips them") {
  namespace ds = model::device_setting;
  CHECK(ds::RecMicro == 20u);
  CHECK(ds::RecVelocity == 21u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP
  CHECK(fresh().get(ds::RecMicro) == 0.0f);
  CHECK(fresh().get(ds::RecVelocity) == 0.0f);
  CHECK(kFile20.size() == 198u);
  CHECK(kFile22.size() == 214u);
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20}) {
    model::Settings out = fresh();
    (void)out.set(ds::RecMicro, 1.0f);  // something to be overwritten by the default
    (void)out.set(ds::RecVelocity, 1.0f);
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::RecMicro) == 0.0f, file->size() << " bytes");
    CHECK_MESSAGE(out.get(ds::RecVelocity) == 0.0f, file->size() << " bytes");
    CHECK(out.get(ds::MetroMode) == 1.0f);  // what the older files hold, or their defaults, is untouched
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  // Every combination round trips byte for byte and nothing else moves; the values are switches (0 or 1).
  for (unsigned micro = 0; micro <= 1u; ++micro) {
    for (unsigned velocity = 0; velocity <= 1u; ++velocity) {
      model::Settings s = fresh();
      CHECK(s.set(ds::RecMicro, static_cast<float>(micro)) == model::Status::Ok);
      CHECK(s.set(ds::RecVelocity, static_cast<float>(velocity)) == model::Status::Ok);
      CHECK(s.set(ds::MetroMode, 2.0f) == model::Status::Ok);
      const Bytes bytes = written(s);
      CHECK(bytes.size() == 286u);
      model::Settings back = fresh();
      const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
      CHECK(rb.error == fs::Error::None);
      CHECK(rb.skipped_entries == 0u);
      CHECK(same(s, back));
      CHECK(back.get(ds::RecMicro) == static_cast<float>(micro));
      CHECK(back.get(ds::RecVelocity) == static_cast<float>(velocity));
      CHECK(back.get(ds::MetroMode) == 2.0f);
      CHECK(written(back) == bytes);
      CHECK(bytes[24 + 8 * 20 + 3] == 20);  // REC MICRO is entry 20, REC VELOCITY entry 21 (PAD OFFSET, 22, is the last of SETS since OS-PADOFFSET)
      CHECK(bytes[24 + 8 * 21 + 3] == 21);
      CHECK(bytes[24 + 8 * 26 + 3] == 26);  // the last entry is OS-MIDICCMAP S1's, id 26 (E-PAT-35's id 25 is the one before)
      CHECK(bytes[24 + 8 * 31] == 'C');     // the CRC chunk follows
    }
  }
  model::Settings s = fresh();
  CHECK(s.set(ds::RecMicro, 2.0f) == model::Status::BadValue);
  CHECK(s.set(ds::RecVelocity, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::RecMicro) == 0.0f);
  // A value outside the range in a file is skipped and counted, the setting keeping its default.
  {
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::kCount; ++f) {
      entries.push_back({dev(f), fresh().get(f)});
    }
    entries[ds::RecMicro].second = 5.0f;
    entries[ds::RecVelocity].second = 1.0f;
    const Bytes bad = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report r = fs::read(bad.data(), bad.size(), seen);
    CHECK(r.error == fs::Error::None);
    CHECK(r.skipped_entries == 1u);
    CHECK(seen.get(ds::RecMicro) == 0.0f);
    CHECK(seen.get(ds::RecVelocity) == 1.0f);
    CHECK(seen.check_invariants());
  }
  // The older reader, as the codec sees it: a build without the rows has no setting under ids 20 and 21 (here: under kCount and kCount + 1, the first ids this build lacks). Both entries are skipped and
  // counted, the rest is read, and the two settings the old build lacks stay at their defaults whatever the entries said.
  {
    model::Settings moved = fresh();
    CHECK(moved.set(ds::CountIn, 2.0f) == model::Status::Ok);
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::RecMicro; ++f) {
      entries.push_back({dev(f), moved.get(f)});
    }
    entries.push_back({dev(ds::kCount), 1.0f});
    entries.push_back({dev(ds::kCount + 1u), 1.0f});
    const Bytes older = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report ro = fs::read(older.data(), older.size(), seen);
    CHECK(ro.error == fs::Error::None);
    CHECK(ro.skipped_entries == 2u);
    CHECK(seen.get(ds::CountIn) == 2.0f);
    CHECK(seen.get(ds::RecMicro) == 0.0f);
    CHECK(seen.get(ds::RecVelocity) == 0.0f);
    CHECK(seen.check_invariants());
  }
  // The descriptors: two choices, learnable, named to fit a row, on the PATTERN page.
  const core::ParamDesc* micro = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::RecMicro));
  const core::ParamDesc* velocity = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::RecVelocity));
  NEED(micro != nullptr);
  NEED(velocity != nullptr);
  CHECK(std::strcmp(micro->name, "REC MICRO") == 0);
  CHECK(std::strcmp(velocity->name, "REC VELOCITY") == 0);
  CHECK(std::strlen(velocity->name) <= core::kParamNameChars);
  CHECK((micro->flags & core::kParamLearnable) != 0);
  CHECK((velocity->flags & core::kParamLearnable) != 0);
  CHECK(micro->def == 0.0f);
  CHECK(velocity->def == 0.0f);
  CHECK(micro->max == 1.0f);
  CHECK(velocity->max == 1.0f);
  CHECK(std::strcmp(micro->names[1], "On") == 0);
  CHECK(std::strcmp(velocity->names[1], "External") == 0);
  CHECK(model::param_group_name(core::ParamKind::Device, micro->group) == std::string("PATTERN"));
  CHECK(micro->group == velocity->group);
}

// OS-PADOFFSET appended PAD OFFSET (field 22: 0..12, default 12, the row offset of the NOTES pads) by the same rule: no chunk, no version bump (docs/formats/o3-settings.md section 2, "Field 22"). 12 is the layout every
// unit had, so every file written before the row (the literals of 13 to 22 settings) reads with no entry skipped and PAD OFFSET 12; a file written by this build and read by one that lacks the row meets an id it does not
// know (skipped and counted), so saving over it there drops the value.
TEST_CASE("settings file: PAD OFFSET is appended without a version bump; every older file reads 12; 0..12 round trip; an older reader skips it") {
  namespace ds = model::device_setting;
  CHECK(ds::PadOffset == 22u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP
  CHECK(fresh().get(ds::PadOffset) == 12.0f);
  CHECK(kFile22.size() == 214u);
  CHECK(kDefaultFile.size() == 286u);
  // The default file ends its settings with the new entry, 12.0 (0x41400000), and the old default file is the same bytes without it.
  CHECK((kDefaultFile[24 + 8 * 22 + 3] == 22 && kDefaultFile[24 + 8 * 22 + 4] == 0x41 && kDefaultFile[24 + 8 * 22 + 5] == 0x40));
  CHECK(std::equal(kFile22.begin() + 24, kFile22.begin() + 24 + 8 * 22, kDefaultFile.begin() + 24));
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20, &kFile22}) {
    model::Settings out = fresh();
    (void)out.set(ds::PadOffset, 3.0f);  // something to be overwritten by the default
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::PadOffset) == 12.0f, file->size() << " bytes");
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  // An old file that holds other values keeps them: the 22 entries of a unit that had METRO on Main and COUNT-IN 2 bars read as that and PAD OFFSET 12.
  {
    model::Settings had = fresh();
    CHECK(had.set(ds::MetroMode, 2.0f) == model::Status::Ok);
    CHECK(had.set(ds::CountIn, 2.0f) == model::Status::Ok);
    CHECK(had.set(ds::RecVelocity, 1.0f) == model::Status::Ok);
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::PadOffset; ++f) {
      entries.push_back({dev(f), had.get(f)});
    }
    const Bytes old_file = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report r = fs::read(old_file.data(), old_file.size(), seen);
    CHECK(r.error == fs::Error::None);
    CHECK(r.skipped_entries == 0u);
    CHECK(seen.get(ds::MetroMode) == 2.0f);
    CHECK(seen.get(ds::CountIn) == 2.0f);
    CHECK(seen.get(ds::RecVelocity) == 1.0f);
    CHECK(seen.get(ds::PadOffset) == 12.0f);
    CHECK(written(seen).size() == 286u);  // saved by this build it gains the entry
  }
  // Every value round trips byte for byte and nothing else moves; the entry is the last of SETS and holds the value as a big-endian float.
  for (unsigned v = 0; v <= 12u; ++v) {
    model::Settings s = fresh();
    CHECK(s.set(ds::PadOffset, static_cast<float>(v)) == model::Status::Ok);
    CHECK(s.set(ds::MetroMode, 2.0f) == model::Status::Ok);
    const Bytes bytes = written(s);
    CHECK(bytes.size() == 286u);
    model::Settings back = fresh();
    const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
    CHECK(rb.error == fs::Error::None);
    CHECK(rb.skipped_entries == 0u);
    CHECK(same(s, back));
    CHECK(back.get(ds::PadOffset) == static_cast<float>(v));
    CHECK(back.get(ds::MetroMode) == 2.0f);
    CHECK(written(back) == bytes);
    CHECK(bytes[24 + 8 * 22 + 3] == 22);
    float f = 0.0f;
    const std::uint32_t bits = (static_cast<std::uint32_t>(bytes[24 + 8 * 22 + 4]) << 24) | (static_cast<std::uint32_t>(bytes[24 + 8 * 22 + 5]) << 16) | (static_cast<std::uint32_t>(bytes[24 + 8 * 22 + 6]) << 8) |
                               bytes[24 + 8 * 22 + 7];
    std::memcpy(&f, &bits, sizeof f);
    CHECK(f == static_cast<float>(v));
    CHECK(bytes[24 + 8 * 26 + 3] == 26);  // the last entry is OS-MIDICCMAP S1's, id 26 (E-PAT-35's id 25 is the one before)
    CHECK(bytes[24 + 8 * 31] == 'C');  // the CRC chunk follows
  }
  // The block refuses a value outside the range; a file with one skips the entry and counts it, the setting keeping its default.
  model::Settings s = fresh();
  CHECK(s.set(ds::PadOffset, 13.0f) == model::Status::BadValue);  // (the board still takes 13, which lays out as 5)
  CHECK(s.set(ds::PadOffset, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::PadOffset) == 12.0f);
  for (const float bad : {13.0f, -1.0f, 200.0f}) {
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::kCount; ++f) {
      entries.push_back({dev(f), fresh().get(f)});
    }
    entries[ds::PadOffset].second = bad;
    entries[ds::CountIn].second = 1.0f;
    const Bytes file = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report r = fs::read(file.data(), file.size(), seen);
    CHECK(r.error == fs::Error::None);
    CHECK(r.skipped_entries == 1u);
    CHECK(seen.get(ds::PadOffset) == 12.0f);
    CHECK(seen.get(ds::CountIn) == 1.0f);
    CHECK(seen.check_invariants());
  }
  // The older reader, as the codec sees it: a build without the row has no setting under id 22 (here: under kCount, the first id this build lacks). The entry is skipped and counted, the rest is read.
  {
    model::Settings moved = fresh();
    CHECK(moved.set(ds::RecMicro, 1.0f) == model::Status::Ok);
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::PadOffset; ++f) {
      entries.push_back({dev(f), moved.get(f)});
    }
    entries.push_back({dev(ds::kCount), 7.0f});
    const Bytes older = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report ro = fs::read(older.data(), older.size(), seen);
    CHECK(ro.error == fs::Error::None);
    CHECK(ro.skipped_entries == 1u);
    CHECK(seen.get(ds::RecMicro) == 1.0f);
    CHECK(seen.get(ds::PadOffset) == 12.0f);
    CHECK(seen.check_invariants());
  }
  // The descriptor: a number 0..12, default 12, learnable, named to fit a row, on the PATTERN page after the others.
  const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::PadOffset));
  NEED(d != nullptr);
  CHECK(std::strcmp(d->name, "PAD OFFSET") == 0);
  CHECK(std::strlen(d->name) <= core::kParamNameChars);
  CHECK((d->min == 0.0f && d->max == 12.0f && d->def == 12.0f));
  CHECK((d->flags & core::kParamLearnable) != 0);
  CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("PATTERN"));
}

TEST_CASE("settings: a file of 23 settings (kFile23, 222 bytes) reads with TRANSPORT IN and OUT at On, nothing skipped") {
  namespace ds = model::device_setting;
  CHECK(ds::TransportIn == 23u);
  CHECK(ds::TransportOut == 24u);
  CHECK(kFile23.size() == 222u);
  CHECK(kDefaultFile.size() == kFile23.size() + 64u);  // eight entries of 8 bytes (E-CFG-03's two, E-PAT-35's one, OS-MIDICCMAP S1's CC MAP, OS-MTC A3's two, OS-MTC B3's MTC IN, OS-LEDBPM's STATUS LED)
  CHECK(std::equal(kFile23.begin() + 24, kFile23.begin() + 24 + 8 * 23, kDefaultFile.begin() + 24));
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20, &kFile22, &kFile23}) {
    model::Settings out = fresh();
    (void)out.set(ds::TransportIn, 0.0f);  // something to be overwritten by the default
    (void)out.set(ds::TransportOut, 0.0f);
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::TransportIn) == 1.0f, file->size() << " bytes");
    CHECK_MESSAGE(out.get(ds::TransportOut) == 1.0f, file->size() << " bytes");
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  // Saved by this build the file gains the two entries (ids 23 and 24, the last of SETS).
  model::Settings seen = fresh();
  const fs::Report r = fs::read(kFile23.data(), kFile23.size(), seen);
  CHECK(r.error == fs::Error::None);
  CHECK(written(seen) == kDefaultFile);
}

TEST_CASE("settings: TRANSPORT IN and OUT round trip Off and On byte for byte") {
  namespace ds = model::device_setting;
  CHECK(fresh().get(ds::TransportIn) == 1.0f);
  CHECK(fresh().get(ds::TransportOut) == 1.0f);
  for (unsigned in = 0; in <= 1u; ++in) {
    for (unsigned out = 0; out <= 1u; ++out) {
      model::Settings s = fresh();
      CHECK(s.set(ds::TransportIn, static_cast<float>(in)) == model::Status::Ok);
      CHECK(s.set(ds::TransportOut, static_cast<float>(out)) == model::Status::Ok);
      CHECK(s.set(ds::MetroMode, 2.0f) == model::Status::Ok);  // something else moved, which must stay
      const Bytes bytes = written(s);
      CHECK(bytes.size() == 286u);
      model::Settings back = fresh();
      const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
      CHECK(rb.error == fs::Error::None);
      CHECK(rb.skipped_entries == 0u);
      CHECK(same(s, back));
      CHECK(back.get(ds::TransportIn) == static_cast<float>(in));
      CHECK(back.get(ds::TransportOut) == static_cast<float>(out));
      CHECK(back.get(ds::MetroMode) == 2.0f);
      CHECK(written(back) == bytes);
      CHECK(bytes[24 + 8 * 23 + 3] == 23);  // the entries are ids 23 and 24 of SETS, then E-PAT-35's id 25, then the CRC chunk
      CHECK(bytes[24 + 8 * 24 + 3] == 24);
      CHECK(bytes[24 + 8 * 26 + 3] == 26);  // the last entry is OS-MIDICCMAP S1's, id 26 (E-PAT-35's id 25 is the one before)
      CHECK(bytes[24 + 8 * 31] == 'C');
      CHECK(bytes[24 + 8 * 23 + 4] == (in != 0u ? 0x3f : 0x00));
      CHECK(bytes[24 + 8 * 24 + 4] == (out != 0u ? 0x3f : 0x00));
    }
  }
}

TEST_CASE("settings: a value of 2 for TRANSPORT IN is refused and skipped, the default stays") {
  namespace ds = model::device_setting;
  model::Settings s = fresh();
  CHECK(s.set(ds::TransportIn, 2.0f) == model::Status::BadValue);
  CHECK(s.set(ds::TransportOut, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::TransportIn) == 1.0f);
  CHECK(s.get(ds::TransportOut) == 1.0f);
  for (const float bad : {2.0f, -1.0f, 200.0f}) {
    std::vector<std::pair<std::uint32_t, float>> entries;
    for (unsigned f = 0; f < ds::kCount; ++f) {
      entries.push_back({dev(f), fresh().get(f)});
    }
    entries[ds::TransportIn].second = bad;
    entries[ds::TransportOut].second = 0.0f;
    const Bytes file = build({{kSets, sets_payload(entries)}});
    model::Settings seen = fresh();
    const fs::Report r = fs::read(file.data(), file.size(), seen);
    CHECK(r.error == fs::Error::None);
    CHECK(r.skipped_entries == 1u);
    CHECK(seen.get(ds::TransportIn) == 1.0f);
    CHECK(seen.get(ds::TransportOut) == 0.0f);
    CHECK(seen.check_invariants());
  }
  // The older reader, as the codec sees it: a build without the rows has no setting under ids 23 and 24 (here: under kCount, the first id this build lacks). The entry is skipped and counted.
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::TransportIn; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries.push_back({dev(ds::kCount), 1.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings seen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), seen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 1u);
  CHECK(seen.get(ds::TransportIn) == 1.0f);
  // The descriptors: Off / On, default On, learnable, on the MIDI page.
  for (const unsigned id : {static_cast<unsigned>(ds::TransportIn), static_cast<unsigned>(ds::TransportOut)}) {
    const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, id));
    NEED(d != nullptr);
    CHECK(std::strlen(d->name) <= core::kParamNameChars);
    CHECK((d->min == 0.0f && d->max == 1.0f && d->def == 1.0f));
    CHECK((d->flags & core::kParamLearnable) != 0);
    CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("MIDI"));
  }
}

// E-PAT-35 (row 58, new Community OS behaviour): PAT SWITCH, device setting 25, appended without a version bump.
TEST_CASE("settings: PAT SWITCH is appended without a version bump; every older file reads Step; Step and Pattern end round trip; an older reader skips it") {
  namespace ds = model::device_setting;
  CHECK(ds::PatSwitch == 25u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3
  CHECK(model::Settings::kMaxFields == 32u);    // OS-MIDICCMAP S1 appended CC MAP (id 26) and made the raise 26 -> 32 itself (it landed before OS-MTC, whose ids 26..28 now have room)
  CHECK(model::Settings::kValueBytes == 32u);
  CHECK(fresh().get(ds::PatSwitch) == 0.0f);
  CHECK(kFile25.size() == 238u);
  CHECK(kDefaultFile.size() == kFile25.size() + 48u);  // six entries of 8 bytes (E-PAT-35's PAT SWITCH, OS-MIDICCMAP S1's CC MAP, OS-MTC A3's two, OS-MTC B3's MTC IN, OS-LEDBPM's STATUS LED)
  CHECK(std::equal(kFile25.begin() + 24, kFile25.begin() + 24 + 8 * 25, kDefaultFile.begin() + 24));
  // Default-fill: the file of a device that never heard of the setting reads Step, nothing skipped; saved by this build it gains the entry.
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20, &kFile22, &kFile23, &kFile25, &kFile26, &kFile27, &kFile29, &kFile30}) {
    model::Settings out = fresh();
    (void)out.set(ds::PatSwitch, 1.0f);  // something to be overwritten by the default
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::PatSwitch) == 0.0f, file->size() << " bytes");
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  model::Settings seen = fresh();
  CHECK(fs::read(kFile25.data(), kFile25.size(), seen).error == fs::Error::None);
  CHECK(written(seen) == kDefaultFile);
  // Round trip, with something else moved too.
  for (unsigned v = 0; v <= 1u; ++v) {
    model::Settings s = fresh();
    CHECK(s.set(ds::PatSwitch, static_cast<float>(v)) == model::Status::Ok);
    CHECK(s.set(ds::TransportOut, 0.0f) == model::Status::Ok);
    const Bytes bytes = written(s);
    CHECK(bytes.size() == 286u);
    model::Settings back = fresh();
    const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
    CHECK(rb.error == fs::Error::None);
    CHECK(rb.skipped_entries == 0u);
    CHECK(same(s, back));
    CHECK(back.get(ds::PatSwitch) == static_cast<float>(v));
    CHECK(back.get(ds::TransportOut) == 0.0f);
    CHECK(written(back) == bytes);
    CHECK(bytes[24 + 8 * 25 + 3] == 25);
    CHECK(bytes[24 + 8 * 25 + 4] == (v != 0u ? 0x3f : 0x00));
  }
  // A value outside Step / Pattern end is refused and skipped; the default stays.
  model::Settings s = fresh();
  CHECK(s.set(ds::PatSwitch, 2.0f) == model::Status::BadValue);
  CHECK(s.get(ds::PatSwitch) == 0.0f);
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::kCount; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries[ds::PatSwitch].second = 2.0f;
  const Bytes bad = build({{kSets, sets_payload(entries)}});
  model::Settings badseen = fresh();
  const fs::Report rbad = fs::read(bad.data(), bad.size(), badseen);
  CHECK(rbad.error == fs::Error::None);
  CHECK(rbad.skipped_entries == 1u);
  CHECK(badseen.get(ds::PatSwitch) == 0.0f);
  // The older reader, as the codec sees it: a build without the row has no setting under id 25 (here: under kCount, the first id this build lacks). The entry is skipped and counted.
  entries.clear();
  for (unsigned f = 0; f < ds::PatSwitch; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries.push_back({dev(ds::kCount), 1.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings olderseen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), olderseen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 1u);
  CHECK(olderseen.get(ds::PatSwitch) == 0.0f);
  // The descriptor: Step / Pattern end, default Step, learnable, on the PATTERN page.
  const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::PatSwitch));
  NEED(d != nullptr);
  CHECK(std::strlen(d->name) <= core::kParamNameChars);
  CHECK((d->min == 0.0f && d->max == 1.0f && d->def == 0.0f));
  CHECK((d->flags & core::kParamLearnable) != 0);
  CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("PATTERN"));
}

// OS-MIDICCMAP S1 (docs/specs/OS-MIDICCMAP.md, Q1 a): CC MAP, device setting 26, appended without a version bump, default Off. This slice landed before OS-MTC, so it makes the raise itself: the block's room goes 26 -> 32 (OS-MTC's ids 26..28 have room now).
TEST_CASE("settings: CC MAP is appended without a version bump; every older file reads Off; Off and On round trip; an older reader skips it") {
  namespace ds = model::device_setting;
  CHECK(ds::CcMap == 26u);
  CHECK(ds::kCount == 31u);  // 27 before OS-MTC A3 appended MTC OUT and MTC START
  CHECK(model::Settings::kMaxFields == 32u);    // the raise this slice makes
  CHECK(model::Settings::kValueBytes == 32u);
  CHECK(fresh().get(ds::CcMap) == 0.0f);                                            // Off, the official default
  CHECK(kFile26.size() == 246u);
  CHECK(kDefaultFile.size() == kFile26.size() + 40u);  // five entries of 8 bytes (CC MAP, OS-MTC A3's two, MTC IN, STATUS LED)
  CHECK(std::equal(kFile26.begin() + 24, kFile26.begin() + 24 + 8 * 26, kDefaultFile.begin() + 24));
  // Default-fill: the file of a device that never heard of the setting reads Off, nothing skipped; saved by this build it gains the entry.
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20, &kFile22, &kFile23, &kFile25, &kFile26, &kFile27, &kFile29, &kFile30}) {
    model::Settings out = fresh();
    (void)out.set(ds::CcMap, 1.0f);  // something to be overwritten by the default
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::CcMap) == 0.0f, file->size() << " bytes");
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  model::Settings seen = fresh();
  CHECK(fs::read(kFile26.data(), kFile26.size(), seen).error == fs::Error::None);
  CHECK(written(seen) == kDefaultFile);
  // Round trip, with something else moved too.
  for (unsigned v = 0; v <= 1u; ++v) {
    model::Settings s = fresh();
    CHECK(s.set(ds::CcMap, static_cast<float>(v)) == model::Status::Ok);
    CHECK(s.set(ds::PatSwitch, 1.0f) == model::Status::Ok);
    const Bytes bytes = written(s);
    CHECK(bytes.size() == 286u);
    model::Settings back = fresh();
    const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
    CHECK(rb.error == fs::Error::None);
    CHECK(rb.skipped_entries == 0u);
    CHECK(same(s, back));
    CHECK(back.get(ds::CcMap) == static_cast<float>(v));
    CHECK(back.get(ds::PatSwitch) == 1.0f);
    CHECK(written(back) == bytes);
    CHECK(bytes[24 + 8 * 26 + 3] == 26);                            // the entry after PAT SWITCH is CC MAP, id 26 (MTC OUT and MTC START follow)
    CHECK(bytes[24 + 8 * 26 + 4] == (v != 0u ? 0x3f : 0x00));
  }
  // A value outside Off / On is refused and skipped; the default stays.
  model::Settings s = fresh();
  CHECK(s.set(ds::CcMap, 2.0f) == model::Status::BadValue);
  CHECK(s.get(ds::CcMap) == 0.0f);
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::kCount; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries[ds::CcMap].second = 2.0f;
  const Bytes bad = build({{kSets, sets_payload(entries)}});
  model::Settings badseen = fresh();
  const fs::Report rbad = fs::read(bad.data(), bad.size(), badseen);
  CHECK(rbad.error == fs::Error::None);
  CHECK(rbad.skipped_entries == 1u);
  CHECK(badseen.get(ds::CcMap) == 0.0f);
  // The older reader, as the codec sees it: a build without the row has no setting under id 26 (here: under kCount, the first id this build lacks). The entry is skipped and counted.
  entries.clear();
  for (unsigned f = 0; f < ds::CcMap; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries.push_back({dev(ds::kCount), 1.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings olderseen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), olderseen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 1u);
  CHECK(olderseen.get(ds::CcMap) == 0.0f);
  // The descriptor: Off / On, default Off, not learnable (a device MIDI routing switch), on the MIDI page.
  const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::CcMap));
  NEED(d != nullptr);
  CHECK(std::strlen(d->name) <= core::kParamNameChars);
  CHECK((d->min == 0.0f && d->max == 1.0f && d->def == 0.0f));
  CHECK((d->flags & core::kParamLearnable) == 0);
  CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("MIDI"));
}

// OS-MTC A3 (docs/specs/OS-MTC.md 3.7; the spec numbered them 26 and 27, CC MAP took 26 first): MTC OUT (id 27: Off, 24, 25, 30 fps; default Off) and MTC START (id 28: 0..23 h; default 1) are appended
// by the documented rule: no chunk, no version bump, default-fill. The file of the build before them (27 entries, 254 bytes) is the literal kFile27.
TEST_CASE("settings: MTC OUT and MTC START are appended without a version bump; every older file reads Off / 01; every value round trips byte for byte; an older reader skips both") {
  namespace ds = model::device_setting;
  CHECK(ds::MtcOut == 27u);
  CHECK(ds::MtcStart == 28u);
  CHECK(ds::kCount == 31u);
  CHECK(model::Settings::kMaxFields == 32u);  // the raise of OS-MIDICCMAP S1 holds them: sizeof(Settings) and the arena do not move
  CHECK(model::Settings::kValueBytes == 32u);
  CHECK(fresh().get(ds::MtcOut) == 0.0f);  // Off
  CHECK(fresh().get(ds::MtcStart) == 1.0f);  // 01:00:00:00, Logic's default
  CHECK(kFile27.size() == 254u);
  CHECK(kDefaultFile.size() == kFile27.size() + 32u);  // four entries of 8 bytes
  CHECK(std::equal(kFile27.begin() + 24, kFile27.begin() + 24 + 8 * 27, kDefaultFile.begin() + 24));
  // The container is the same version as before: the SETS chunk version bytes (0x0100) and the FORM tag did not move.
  CHECK(kDefaultFile[20] == 0x01);
  CHECK(kDefaultFile[21] == 0x00);
  CHECK(kFile27[20] == 0x01);
  CHECK(kFile27[21] == 0x00);
  // Default-fill: every older file reads Off / 1, nothing skipped; saved by this build it gains the two entries.
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20, &kFile22, &kFile23, &kFile25, &kFile26, &kFile27, &kFile29, &kFile30}) {
    model::Settings out = fresh();
    (void)out.set(ds::MtcOut, 2.0f);   // something to be overwritten by the default
    (void)out.set(ds::MtcStart, 9.0f);
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::MtcOut) == 0.0f, file->size() << " bytes");
    CHECK_MESSAGE(out.get(ds::MtcStart) == 1.0f, file->size() << " bytes");
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  model::Settings seen = fresh();
  CHECK(fs::read(kFile27.data(), kFile27.size(), seen).error == fs::Error::None);
  CHECK(written(seen) == kDefaultFile);
  // Round trip every value of both, byte for byte, with something else moved too (the CC MAP before them, a binding after).
  for (unsigned out_v = 0; out_v <= 3u; ++out_v) {
    for (unsigned hour = 0; hour <= 23u; ++hour) {
      model::Settings s = fresh();
      CHECK(s.set(ds::MtcOut, static_cast<float>(out_v)) == model::Status::Ok);
      CHECK(s.set(ds::MtcStart, static_cast<float>(hour)) == model::Status::Ok);
      CHECK(s.set(ds::CcMap, 1.0f) == model::Status::Ok);
      const Bytes bytes = written(s);
      CHECK(bytes.size() == 286u);
      model::Settings back = fresh();
      const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
      CHECK(rb.error == fs::Error::None);
      CHECK(rb.skipped_entries == 0u);
      CHECK(same(s, back));
      CHECK(back.get(ds::MtcOut) == static_cast<float>(out_v));
      CHECK(back.get(ds::MtcStart) == static_cast<float>(hour));
      CHECK(back.get(ds::CcMap) == 1.0f);
      CHECK(written(back) == bytes);
      CHECK(bytes[24 + 8 * 27 + 3] == 27);  // the last two entries of SETS are MTC OUT, id 27, and MTC START, id 28
      CHECK(bytes[24 + 8 * 28 + 3] == 28);
    }
  }
  // 4 for MTC OUT and 24 for MTC START are refused by the block and skipped by the reader (counted), the defaults staying.
  model::Settings s = fresh();
  CHECK(s.set(ds::MtcOut, 4.0f) == model::Status::BadValue);
  CHECK(s.set(ds::MtcStart, 24.0f) == model::Status::BadValue);
  CHECK(s.set(ds::MtcStart, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::MtcOut) == 0.0f);
  CHECK(s.get(ds::MtcStart) == 1.0f);
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::kCount; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries[ds::MtcOut].second = 4.0f;
  entries[ds::MtcStart].second = 24.0f;
  const Bytes bad = build({{kSets, sets_payload(entries)}});
  model::Settings badseen = fresh();
  const fs::Report rbad = fs::read(bad.data(), bad.size(), badseen);
  CHECK(rbad.error == fs::Error::None);
  CHECK(rbad.skipped_entries == 2u);
  CHECK(badseen.get(ds::MtcOut) == 0.0f);
  CHECK(badseen.get(ds::MtcStart) == 1.0f);
  // The older reader, as the codec sees it: a build without the rows has no setting under ids 27 and 28 (here: under kCount and kCount + 1, the first ids this build lacks). Both are skipped and counted.
  entries.clear();
  for (unsigned f = 0; f < ds::MtcOut; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries.push_back({dev(ds::kCount), 2.0f});
  entries.push_back({dev(ds::kCount + 1u), 5.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings olderseen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), olderseen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 2u);
  CHECK(olderseen.get(ds::MtcOut) == 0.0f);
  CHECK(olderseen.get(ds::MtcStart) == 1.0f);
  // The descriptors: Off / 24 / 25 / 30 and 0..23, not learnable, on the MIDI page.
  const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::MtcOut));
  NEED(d != nullptr);
  CHECK(std::strlen(d->name) <= core::kParamNameChars);
  CHECK((d->min == 0.0f && d->max == 3.0f && d->def == 0.0f));
  CHECK((d->flags & core::kParamLearnable) == 0);
  CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("MIDI"));
  const core::ParamDesc* h = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::MtcStart));
  NEED(h != nullptr);
  CHECK((h->min == 0.0f && h->max == 23.0f && h->def == 1.0f));
  CHECK((h->flags & core::kParamLearnable) == 0);
  CHECK(model::param_group_name(core::ParamKind::Device, h->group) == std::string("MIDI"));
}

// OS-MTC B3 (docs/specs/OS-MTC.md 4.6; Q7 a: the block already holds 32 fields since OS-MIDICCMAP S1, so no raise): MTC IN (id 29: Off, DIN, USB; default Off) is appended by the documented rule: no chunk,
// no version bump, default-fill. The file of the build before it (29 entries, 270 bytes) is the literal kFile29; the default file is 278 bytes.
TEST_CASE("settings: MTC IN is appended without a version bump; every older file reads Off; every value round trips byte for byte; 3 is refused and skipped; an older reader skips it") {
  namespace ds = model::device_setting;
  CHECK(ds::MtcIn == 29u);
  CHECK(ds::kCount == 31u);
  CHECK(model::Settings::kMaxFields == 32u);  // no raise: the block, sizeof(Settings) and the arena did not move
  CHECK(model::Settings::kValueBytes == 32u);
  CHECK(fresh().get(ds::MtcIn) == 0.0f);  // Off
  CHECK(kFile29.size() == 270u);
  CHECK(kDefaultFile.size() == 286u);
  CHECK(kDefaultFile.size() == kFile29.size() + 16u);  // two entries of 8 bytes (MTC IN, OS-LEDBPM's STATUS LED)
  CHECK(std::equal(kFile29.begin() + 24, kFile29.begin() + 24 + 8 * 29, kDefaultFile.begin() + 24));
  // The container is the same version as before: the SETS chunk version bytes (0x0100) did not move; only the count (31) and the lengths did.
  CHECK(kDefaultFile[20] == 0x01);
  CHECK(kDefaultFile[21] == 0x00);
  CHECK(kDefaultFile[23] == 31);
  CHECK(kFile29[23] == 29);
  // Default-fill: every older file reads Off, nothing skipped; saved by this build it gains the entry.
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20, &kFile22, &kFile23, &kFile25, &kFile26, &kFile27, &kFile29, &kFile30}) {
    model::Settings out = fresh();
    (void)out.set(ds::MtcIn, 2.0f);  // something to be overwritten by the default
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::MtcIn) == 0.0f, file->size() << " bytes");
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  model::Settings seen = fresh();
  CHECK(fs::read(kFile29.data(), kFile29.size(), seen).error == fs::Error::None);
  CHECK(written(seen) == kDefaultFile);
  // Round trip every value, byte for byte, with the settings before it moved too.
  for (unsigned v = 0; v <= 2u; ++v) {
    model::Settings s = fresh();
    CHECK(s.set(ds::MtcIn, static_cast<float>(v)) == model::Status::Ok);
    CHECK(s.set(ds::MtcOut, 2.0f) == model::Status::Ok);
    CHECK(s.set(ds::MtcStart, 0.0f) == model::Status::Ok);
    const Bytes bytes = written(s);
    CHECK(bytes.size() == 286u);
    model::Settings back = fresh();
    const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
    CHECK(rb.error == fs::Error::None);
    CHECK(rb.skipped_entries == 0u);
    CHECK(same(s, back));
    CHECK(back.get(ds::MtcIn) == static_cast<float>(v));
    CHECK(back.get(ds::MtcOut) == 2.0f);
    CHECK(back.get(ds::MtcStart) == 0.0f);
    CHECK(written(back) == bytes);
    CHECK(bytes[24 + 8 * 29 + 3] == 29);  // the last entry of SETS is MTC IN, id 29
  }
  // 3 is refused by the block and skipped by the reader (counted), the default staying.
  model::Settings s = fresh();
  CHECK(s.set(ds::MtcIn, 3.0f) == model::Status::BadValue);
  CHECK(s.set(ds::MtcIn, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::MtcIn) == 0.0f);
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::kCount; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries[ds::MtcIn].second = 3.0f;
  const Bytes bad = build({{kSets, sets_payload(entries)}});
  model::Settings badseen = fresh();
  const fs::Report rbad = fs::read(bad.data(), bad.size(), badseen);
  CHECK(rbad.error == fs::Error::None);
  CHECK(rbad.skipped_entries == 1u);
  CHECK(badseen.get(ds::MtcIn) == 0.0f);
  // The older reader, as the codec sees it: a build without the row has no setting under id 29 (here: under kCount, the first id this build lacks). It is skipped and counted, the rest is read.
  entries.clear();
  for (unsigned f = 0; f < ds::MtcIn; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries.push_back({dev(ds::kCount), 2.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings olderseen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), olderseen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 1u);
  CHECK(olderseen.get(ds::MtcIn) == 0.0f);
  // The descriptor: Off / DIN / USB, not learnable, on the MIDI page.
  const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::MtcIn));
  NEED(d != nullptr);
  CHECK(std::strlen(d->name) <= core::kParamNameChars);
  CHECK(std::string(d->name) == "MTC IN");
  CHECK((d->min == 0.0f && d->max == 2.0f && d->def == 0.0f));
  CHECK((d->flags & core::kParamLearnable) == 0);
  CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("MIDI"));
}

// OS-LEDBPM (Level C, maintainer 2026-11-10 "save it"): STATUS LED (id 30: Blink, BPM, Off; default Blink) is appended by the documented rule: no chunk, no version bump (the block already holds 32
// fields, 31 are used: no raise), default-fill. The file of the build before it (30 entries, 278 bytes) is the literal kFile30; the default file is 286 bytes.
TEST_CASE("settings: STATUS LED is appended without a version bump; every older file reads Blink; every value round trips byte for byte; 3 is refused and skipped; an older reader skips it") {
  namespace ds = model::device_setting;
  CHECK(ds::StatusLed == 30u);
  CHECK(ds::kCount == 31u);
  CHECK(model::Settings::kMaxFields == 32u);  // no raise: the block, sizeof(Settings) and the arena did not move
  CHECK(model::Settings::kValueBytes == 32u);
  CHECK(fresh().get(ds::StatusLed) == 0.0f);  // Blink: what every unit did before the setting existed
  CHECK(kFile30.size() == 278u);
  CHECK(kDefaultFile.size() == 286u);
  CHECK(kDefaultFile.size() == kFile30.size() + 8u);  // one entry of 8 bytes
  CHECK(std::equal(kFile30.begin() + 24, kFile30.begin() + 24 + 8 * 30, kDefaultFile.begin() + 24));
  // The container is the same version: the SETS chunk version bytes (0x0100) did not move; the count (31), the SETS length (244 -> 252) and the FORM length (270 -> 278) did.
  CHECK(kDefaultFile[20] == 0x01);
  CHECK(kDefaultFile[21] == 0x00);
  CHECK(kDefaultFile[23] == 31);
  CHECK(kFile30[23] == 30);
  CHECK(kDefaultFile[19] == 252);
  CHECK(kFile30[19] == 244);
  CHECK(kDefaultFile[24 + 8 * 30 + 3] == 30);
  CHECK(kDefaultFile[24 + 8 * 30 + 4] == 0);  // the default is Blink (0.0f)
  // Default-fill: every older file reads Blink, nothing skipped; saved by this build it gains the entry.
  for (const Bytes* file : {&kFile13, &kFile14, &kFile16, &kFile17, &kFile18, &kFile20, &kFile22, &kFile23, &kFile25, &kFile26, &kFile27, &kFile29, &kFile30}) {
    model::Settings out = fresh();
    (void)out.set(ds::StatusLed, 2.0f);  // something to be overwritten by the default
    const fs::Report r = fs::read(file->data(), file->size(), out);
    CHECK_MESSAGE(r.error == fs::Error::None, file->size() << " bytes");
    CHECK_MESSAGE(r.skipped_entries == 0u, file->size() << " bytes");
    CHECK_FALSE(r.newer_minor);
    CHECK_MESSAGE(out.get(ds::StatusLed) == 0.0f, file->size() << " bytes");
    CHECK(same(out, fresh()));
    CHECK(out.check_invariants());
  }
  model::Settings seen = fresh();
  CHECK(fs::read(kFile30.data(), kFile30.size(), seen).error == fs::Error::None);
  CHECK(written(seen) == kDefaultFile);
  // Round trip every value, byte for byte, with the setting before it moved too.
  for (unsigned v = 0; v <= 2u; ++v) {
    model::Settings s = fresh();
    CHECK(s.set(ds::StatusLed, static_cast<float>(v)) == model::Status::Ok);
    CHECK(s.set(ds::MtcIn, 2.0f) == model::Status::Ok);
    const Bytes bytes = written(s);
    CHECK(bytes.size() == 286u);
    model::Settings back = fresh();
    const fs::Report rb = fs::read(bytes.data(), bytes.size(), back);
    CHECK(rb.error == fs::Error::None);
    CHECK(rb.skipped_entries == 0u);
    CHECK(same(s, back));
    CHECK(back.get(ds::StatusLed) == static_cast<float>(v));
    CHECK(back.get(ds::MtcIn) == 2.0f);
    CHECK(written(back) == bytes);
    CHECK(bytes[24 + 8 * 30 + 3] == 30);  // the last entry of SETS is STATUS LED, id 30
  }
  // 3 and -1 are refused by the block; 3 in a file is skipped by the reader (counted), the default staying.
  model::Settings s = fresh();
  CHECK(s.set(ds::StatusLed, 3.0f) == model::Status::BadValue);
  CHECK(s.set(ds::StatusLed, -1.0f) == model::Status::BadValue);
  CHECK(s.get(ds::StatusLed) == 0.0f);
  std::vector<std::pair<std::uint32_t, float>> entries;
  for (unsigned f = 0; f < ds::kCount; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries[ds::StatusLed].second = 3.0f;
  const Bytes bad = build({{kSets, sets_payload(entries)}});
  model::Settings badseen = fresh();
  const fs::Report rbad = fs::read(bad.data(), bad.size(), badseen);
  CHECK(rbad.error == fs::Error::None);
  CHECK(rbad.skipped_entries == 1u);
  CHECK(badseen.get(ds::StatusLed) == 0.0f);
  // The older reader, as the codec sees it: a build without the row has no setting under id 30 (here: under kCount, the first id this build lacks). It is skipped and counted, the rest is read.
  entries.clear();
  for (unsigned f = 0; f < ds::StatusLed; ++f) {
    entries.push_back({dev(f), fresh().get(f)});
  }
  entries.push_back({dev(ds::kCount), 2.0f});
  const Bytes older = build({{kSets, sets_payload(entries)}});
  model::Settings olderseen = fresh();
  const fs::Report ro = fs::read(older.data(), older.size(), olderseen);
  CHECK(ro.error == fs::Error::None);
  CHECK(ro.skipped_entries == 1u);
  CHECK(olderseen.get(ds::StatusLed) == 0.0f);
  // The descriptor: Blink / BPM / Off, not learnable, on the DISPLAY page.
  const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Device, 0, ds::StatusLed));
  NEED(d != nullptr);
  CHECK(std::strlen(d->name) <= core::kParamNameChars);
  CHECK(std::string(d->name) == "STATUS LED");
  CHECK((d->min == 0.0f && d->max == 2.0f && d->def == 0.0f));
  CHECK((d->flags & core::kParamLearnable) == 0);
  CHECK(model::param_group_name(core::ParamKind::Device, d->group) == std::string("DISPLAY"));
}
