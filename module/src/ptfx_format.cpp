// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/module/ptfx_format.h>

#include <cmath>
#include <cstring>

namespace o3os::module {

namespace {

std::uint16_t rd16(const std::uint8_t* p) noexcept { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
float rdf(const std::uint8_t* p) noexcept {
  const std::uint32_t u = rd32(p);
  float f;
  std::memcpy(&f, &u, sizeof f);
  return f;
}

char printable(std::uint8_t c) noexcept { return (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?'; }

// A fixed field of `cap` bytes that may lack its NUL: copied up to the first NUL, a non-printable character shows as '?'.
// `dst_size` is the destination's size, its NUL included.
void text(char* dst, std::size_t dst_size, const std::uint8_t* src, std::size_t cap) noexcept {
  std::size_t i = 0;
  for (; i < cap && i + 1 < dst_size && src[i] != 0; ++i) {
    dst[i] = printable(src[i]);
  }
  dst[i] = '\0';
}

}  // namespace

PtfxStatus ptfx_parse_header(const std::uint8_t* b, std::size_t len, std::uint64_t file_size, PtfxInfo& info) noexcept {
  std::memset(&info, 0, sizeof info);
  info.status = PtfxStatus::NotPtfx;
  if (b == nullptr || len < kPtfxHeaderSize || file_size < kPtfxHeaderSize || rd32(b) != kPtfxMagic) {
    return info.status;
  }
  info.version = rd16(b + 4);
  info.flags = rd16(b + 6);
  if (info.version != kPtfxVersion) {
    return info.status = PtfxStatus::VersionUnsupported;
  }
  if ((info.flags & kPtfxFlagImxOnly) != 0) {
    return info.status = PtfxStatus::NotForThisTracker;
  }
  if ((info.flags & ~0x1Fu) != 0) {
    return info.status = PtfxStatus::NeedsNewerOs;
  }
  text(info.name, sizeof info.name, b + 8, 32);
  text(info.author, sizeof info.author, b + 40, 16);
  info.num_params = b[56] > kPtfxMaxParams ? static_cast<std::uint8_t>(kPtfxMaxParams) : b[56];
  const std::uint32_t scratch = rd32(b + 60);
  info.scratch_size = scratch > kPtfxMaxScratch ? kPtfxMaxScratch : scratch;
  info.code_offset = rd32(b + 64);
  info.code_size = rd32(b + 68);
  info.bss_size = rd32(b + 72);
  info.init_offset = rd32(b + 76) & ~1u;
  info.set_param_offset = rd32(b + 80) & ~1u;
  info.process_block_offset = rd32(b + 84) & ~1u;

  if (info.code_size == 0 || info.code_offset < kPtfxHeaderSize || static_cast<std::uint64_t>(info.code_offset) + info.code_size > file_size) {
    return info.status = PtfxStatus::FileDamaged;
  }
  const std::uint64_t image = ((static_cast<std::uint64_t>(info.code_size) + 3u) & ~3ull) + info.bss_size;
  if (image > kPtfxMaxCode) {
    return info.status = PtfxStatus::TooBig;
  }
  if (info.init_offset >= info.code_size || info.set_param_offset >= info.code_size || info.process_block_offset >= info.code_size ||
      info.init_offset == info.set_param_offset || info.init_offset == info.process_block_offset || info.set_param_offset == info.process_block_offset) {
    return info.status = PtfxStatus::FileDamaged;
  }

  for (std::uint32_t i = 0; i < info.num_params; ++i) {
    const std::uint8_t* q = b + 88 + 32 * i;
    PtfxParam& p = info.params[i];
    text(p.label, sizeof p.label, q, 8);
    p.min = rdf(q + 8);
    p.max = rdf(q + 12);
    p.def = rdf(q + 16);
    p.step = rdf(q + 20);
    const char t = static_cast<char>(q[30]);
    p.type = (t == 'd' || t == 'l') ? t : 'f';
    p.editable = true;
    if (p.type == 'l') {
      p.list_offset = rd16(q + 24);
      p.list_count = q[26] > kPtfxMaxListItems ? static_cast<std::uint8_t>(kPtfxMaxListItems) : q[26];
    } else {
      text(p.unit, sizeof p.unit, q + 24, 6);
    }
    if (!std::isfinite(p.min) || !std::isfinite(p.max) || p.min > p.max) {
      p.type = 'd';
      p.min = p.max = p.def = 0.0f;
      p.step = 1.0f;
      p.editable = false;
      p.list_count = 0;
    } else if (!std::isfinite(p.def) || p.def < p.min) {
      p.def = p.min;
    } else if (p.def > p.max) {
      p.def = p.max;
    }
    if (!std::isfinite(p.step) || p.step <= 0.0f) {
      p.step = 1.0f;  // a zero, negative or non-finite step is not a step
    }
  }
  return info.status = PtfxStatus::Ok;
}

std::uint32_t ptfx_list_count(const std::uint8_t* payload, std::size_t payload_len, const PtfxParam& p) noexcept {
  if (p.type != 'l' || payload == nullptr) {
    return 0;
  }
  std::size_t at = p.list_offset;
  std::uint32_t n = 0;
  while (n < p.list_count && n < kPtfxMaxListItems && at < payload_len) {
    std::size_t e = at;
    while (e < payload_len && payload[e] != 0) {
      ++e;
    }
    if (e >= payload_len) {
      break;  // no NUL inside the payload
    }
    at = e + 1;
    ++n;
  }
  return n;
}

bool ptfx_list_item(const std::uint8_t* payload, std::size_t payload_len, const PtfxParam& p, std::uint32_t index, char (&out)[11]) noexcept {
  out[0] = '\0';
  if (index >= ptfx_list_count(payload, payload_len, p)) {
    return false;
  }
  std::size_t at = p.list_offset;
  for (std::uint32_t i = 0; i < index; ++i) {
    while (payload[at] != 0) {
      ++at;
    }
    ++at;
  }
  std::size_t k = 0;
  while (k < 10 && payload[at + k] != 0) {
    out[k] = printable(payload[at + k]);
    ++k;
  }
  out[k] = '\0';
  return true;
}

}  // namespace o3os::module
