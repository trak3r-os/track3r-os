// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A .tmod file made by hand (OS-012a), for the loader's tests: the same layout tools/o3m_pack.py writes, with every field reachable so that
// a test can break exactly one thing. Nothing here runs: the functions of the descriptor are addresses inside the image.
//
//   image offsets   0..31    hot part: 32 bytes. Offset 8 holds a pointer into the cold part, offset 12 a pointer one past the end of the image.
//                   32..191  cold part: the descriptor at 32 (64 bytes), two parameters at 96 and 116 (20 bytes each), the strings "Test" at 136,
//                            "Alpha" at 144 and "Beta" at 152, then padding to 192 bytes.
//   relocations     8, 12, 48 (name), 60 (params), 64..84 (the six functions), 96 and 116 (the parameter names)
#include <o3os/model/import_layout.h>
#include <o3os/module/builtin.h>
#include <o3os/module/image.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace o3os::module::test {

// The good blob is a module of ABI 1.7: the last minor with the 64-byte descriptor (ABI 1.8 made the descriptor 80 bytes, blob18() below). The OS's own minor moves on; this file's idea of "an ordinary module" does not.
inline constexpr std::uint8_t kBlobMinor = 7;
inline constexpr std::uint32_t kHot = 32;
inline constexpr std::uint32_t kImage = 192;
inline constexpr std::uint32_t kDesc = 32;
inline constexpr std::uint32_t kHotAddr = 0x1FFFC000u;   // where the tests place the hot part (the slab)
inline constexpr std::uint32_t kColdAddr = 0x70000000u;  // and the cold part (the module region)

struct Blob {
  o3m_file_header h{};
  std::vector<std::uint8_t> image;     // kImage bytes, as linked at 0
  std::vector<std::uint32_t> relocs;   // image offsets

  std::vector<std::uint8_t> file() const {
    std::vector<std::uint8_t> out(sizeof h + image.size() + relocs.size() * 4);
    std::memcpy(out.data(), &h, sizeof h);
    if (!image.empty()) {
      std::memcpy(out.data() + sizeof h, image.data(), image.size());
    }
    if (!relocs.empty()) {
      std::memcpy(out.data() + sizeof h + image.size(), relocs.data(), relocs.size() * 4);
    }
    return out;
  }
  // The CRC the loader wants: the header with its CRC field zero, the image, the table. Call after the last change.
  void seal() {
    h.image_crc = 0;
    std::uint32_t c = crc32_update(0, &h, sizeof h);
    c = crc32_update(c, image.data(), image.size());
    c = crc32_update(c, relocs.data(), relocs.size() * 4);
    h.image_crc = c;
  }
  void put(std::uint32_t offset, std::uint32_t value) { std::memcpy(image.data() + offset, &value, 4); }
  std::uint32_t get(std::uint32_t offset) const {
    std::uint32_t v;
    std::memcpy(&v, image.data() + offset, 4);
    return v;
  }
  void put_param(std::uint32_t offset, std::uint32_t name, float mn, float mx, float def, std::uint32_t flags) {
    Param32 p{name, mn, mx, def, flags};
    std::memcpy(image.data() + offset, &p, sizeof p);
  }
};

inline Blob good_blob() {
  Blob b;
  b.image.assign(kImage, 0);
  for (std::uint32_t i = 0; i < kHot; ++i) {
    b.image[i] = static_cast<std::uint8_t>(0xA0u + i);
  }
  b.put(8, kHot + 112);  // hot code's literal: the address of a cold string ("Alpha")
  b.put(12, kImage);     // an end pointer
  Desc32 d{};
  d.abi_major = O3M_ABI_MAJOR;
  d.abi_minor = kBlobMinor;
  d.kind = O3M_KIND_INSTRUMENT;
  d.struct_size = sizeof(Desc32);
  d.name = kHot + 104;      // 136
  d.state_bytes = 64;
  d.param_count = 2;
  d.params = kHot + 64;     // 96
  d.init = 1;               // hot, Thumb
  d.render = 3;
  d.set_param = kHot + 120 + 1;  // a cold function
  d.note_on = 5;
  d.note_off = 7;
  d.shutdown = 9;
  std::memcpy(b.image.data() + kDesc, &d, sizeof d);
  b.put_param(96, kHot + 112, 0.0f, 1.0f, 0.5f, 0);
  b.put_param(116, kHot + 120, -10.0f, 10.0f, 0.0f, O3M_PARAM_LOG);
  std::memcpy(b.image.data() + 136, "Test", 5);
  std::memcpy(b.image.data() + 144, "Alpha", 6);
  std::memcpy(b.image.data() + 152, "Beta", 5);
  b.relocs = {8, 12, 48, 60, 64, 68, 72, 76, 80, 84, 96, 116};
  b.h.magic = O3M_FILE_MAGIC;
  b.h.header_size = O3M_FILE_HEADER_SIZE;
  b.h.target = O3M_TARGET_ARMV7EM;
  b.h.kind = O3M_KIND_INSTRUMENT;
  b.h.abi_major = O3M_ABI_MAJOR;
  b.h.abi_minor = kBlobMinor;
  b.h.hot_size = kHot;
  b.h.image_size = kImage;
  b.h.desc_offset = kDesc;
  b.h.state_bytes = 64;
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.h.param_count = 2;
  b.seal();
  return b;
}

// The good blob as a module of ABI 1.`minor` (the header's and the descriptor's words), sealed. The module version stays 0 (a file below ABI 1.2 cannot carry one).
inline Blob blob_minor(std::uint8_t minor) {
  Blob b = good_blob();
  b.h.abi_minor = minor;
  b.put(kDesc + 4, minor);  // Desc32::abi_minor
  b.seal();
  return b;
}

// The good blob as a module of ABI 1.2 (the first to carry a version) with `version` (O3M_VERSION) in its header, sealed.
inline Blob blob_version(std::uint32_t version) {
  Blob b = blob_minor(2);
  b.h.module_version = version;
  b.seal();
  return b;
}

inline void put_desc(Blob& b, const Desc32& d) { std::memcpy(b.image.data() + kDesc, &d, sizeof d); }
inline Desc32 get_desc(const Blob& b) {
  Desc32 d;
  std::memcpy(&d, b.image.data() + kDesc, sizeof d);
  return d;
}

// A module of ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): the 80-byte descriptor with pitch, two pages and labels. 10 parameters (0: an INT 0..2 with three labels, 3: an INT 0..1 with two labels,
// the rest floats 0..1), pages "Osc" (parameters 0..7) and "Env" (8..9). Image 512 bytes, hot 32 as in good_blob(); the offsets below are image offsets, exposed so that a test can break one word.
struct Layout18 {
  static constexpr std::uint32_t kImage18 = 512;
  static constexpr std::uint32_t desc = 32;          // 80 bytes
  static constexpr std::uint32_t params = 112;       // 10 x 20 bytes
  static constexpr std::uint32_t strings = 312;      // "Test" 312, "Alpha" 320, "Beta" 328 (unused names), "Osc" 336, "Env" 340, "Saw" 344, "Sqr" 348, "Tri" 352, "Lo" 356, "Hi" 360
  static constexpr std::uint32_t pages = 368;        // 2 x 8 bytes
  static constexpr std::uint32_t labels = 384;       // 10 x 8 bytes
  static constexpr std::uint32_t names0 = 464;       // parameter 0's three string addresses
  static constexpr std::uint32_t names3 = 480;       // parameter 3's two
  static constexpr std::uint32_t pitch_fn = 11;      // hot, Thumb
};

inline Blob blob18() {
  using L = Layout18;
  Blob b;
  b.image.assign(L::kImage18, 0);
  for (std::uint32_t i = 0; i < kHot; ++i) {
    b.image[i] = static_cast<std::uint8_t>(0xA0u + i);
  }
  const char* const text[] = {"Test", "Alpha", "Beta", "Osc", "Env", "Saw", "Sqr", "Tri", "Lo", "Hi"};
  std::uint32_t at[10];
  std::uint32_t cursor = L::strings;
  for (unsigned i = 0; i < 10; ++i) {
    at[i] = cursor;
    std::memcpy(b.image.data() + cursor, text[i], std::strlen(text[i]) + 1);
    cursor += i < 3 ? 8u : 4u;
  }
  Desc32 d{};
  d.abi_major = O3M_ABI_MAJOR;
  d.abi_minor = 8;
  d.kind = O3M_KIND_INSTRUMENT;
  d.struct_size = sizeof(Desc32) + sizeof(DescExt32);
  d.name = at[0];
  d.state_bytes = 64;
  d.param_count = 10;
  d.params = L::params;
  d.init = 1;
  d.render = 3;
  d.set_param = kHot + 120 + 1;  // a cold function (image offset 152 + 1: inside the image)
  d.note_on = 5;
  d.note_off = 7;
  d.shutdown = 9;
  std::memcpy(b.image.data() + L::desc, &d, sizeof d);
  const DescExt32 t{L::pitch_fn, L::pages, 2, L::labels};
  std::memcpy(b.image.data() + L::desc + sizeof d, &t, sizeof t);
  for (unsigned i = 0; i < 10; ++i) {
    const bool is_int = i == 0u || i == 3u;
    b.put_param(L::params + 20u * i, at[1], 0.0f, i == 0u ? 2.0f : 1.0f, 0.0f, is_int ? O3M_PARAM_INT : 0u);
  }
  const Page32 pg[2] = {{at[3], 0}, {at[4], 8}};
  std::memcpy(b.image.data() + L::pages, pg, sizeof pg);
  const Labels32 l0{L::names0, 3}, l3{L::names3, 2};
  std::memcpy(b.image.data() + L::labels, &l0, sizeof l0);
  std::memcpy(b.image.data() + L::labels + 8u * 3u, &l3, sizeof l3);
  b.put(L::names0, at[5]);
  b.put(L::names0 + 4, at[6]);
  b.put(L::names0 + 8, at[7]);
  b.put(L::names3, at[8]);
  b.put(L::names3 + 4, at[9]);
  std::vector<std::uint32_t> r = {L::desc + 16, L::desc + 28};  // name, params
  for (unsigned i = 0; i < 6; ++i) {
    r.push_back(L::desc + 32u + 4u * i);  // the six functions
  }
  r.push_back(L::desc + 64);  // pitch
  r.push_back(L::desc + 68);  // pages
  r.push_back(L::desc + 76);  // labels
  for (unsigned i = 0; i < 10; ++i) {
    r.push_back(L::params + 20u * i);  // parameter names
  }
  r.push_back(L::pages);
  r.push_back(L::pages + 8);
  r.push_back(L::labels);            // parameter 0's names
  r.push_back(L::labels + 8u * 3u);  // parameter 3's names
  for (unsigned i = 0; i < 3; ++i) {
    r.push_back(L::names0 + 4u * i);
  }
  r.push_back(L::names3);
  r.push_back(L::names3 + 4);
  std::sort(r.begin(), r.end());
  b.relocs = r;
  b.h.magic = O3M_FILE_MAGIC;
  b.h.header_size = O3M_FILE_HEADER_SIZE;
  b.h.target = O3M_TARGET_ARMV7EM;
  b.h.kind = O3M_KIND_INSTRUMENT;
  b.h.abi_major = O3M_ABI_MAJOR;
  b.h.abi_minor = 8;
  b.h.hot_size = kHot;
  b.h.image_size = L::kImage18;
  b.h.desc_offset = L::desc;
  b.h.state_bytes = 64;
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.h.param_count = 10;
  b.seal();
  return b;
}

// An app of ABI 1.8 (the 80-byte descriptor needs blob18()'s roomier layout): its views' table at image offset 488, the sound entries zero, the 1.8 members as blob18() has them.
inline Blob blob_app18(std::uint32_t views, std::uint8_t minor = 8) {
  Blob b = blob18();
  Desc32 d;
  std::memcpy(&d, b.image.data() + Layout18::desc, sizeof d);
  d.kind = O3M_KIND_APP;
  d.abi_minor = minor;  // (a later minor than this OS's is a test of the refusal: the same layout)
  d.render = d.set_param = d.note_on = d.note_off = 0;
  d.ui = views != 0u ? 488u : 0u;
  d.view_count = views;
  std::memcpy(b.image.data() + Layout18::desc, &d, sizeof d);
  for (const std::uint32_t at : {Layout18::desc + 36u, Layout18::desc + 40u, Layout18::desc + 44u, Layout18::desc + 48u}) {
    b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), at), b.relocs.end());
  }
  if (views != 0u) {
    const Ui32 ui{ui_size(minor), 0, 0, 9, 0};  // (ABI 1.9: 24 bytes, the `keys` word past it is zero)
    std::memcpy(b.image.data() + 488, &ui, sizeof ui);
    b.relocs.push_back(Layout18::desc + 56u);
    b.relocs.push_back(488u + 12u);
    std::sort(b.relocs.begin(), b.relocs.end());
  }
  b.h.kind = O3M_KIND_APP;
  b.h.abi_minor = minor;
  b.h.view_count = static_cast<std::uint16_t>(views);
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.seal();
  return b;
}

// An app of ABI 1.11: the 84-byte descriptor (the importer word is 0: an app has no importer table), no parameters, one view;
// the views' table at image offset 160 (cold padding), view_draw at hot 8 (Thumb). (blob18's layout cannot be stretched: its params
// table starts exactly at desc + 80.)
inline Blob blob_app11(std::uint32_t views = 1, std::uint8_t minor = kImporterAbiMinor) {
  Blob b;
  b.image.assign(256, 0);
  for (std::uint32_t i = 0; i < kHot; ++i) {
    b.image[i] = static_cast<std::uint8_t>(0xA0u + i);
  }
  Desc32 d{};
  d.abi_major = O3M_ABI_MAJOR;
  d.abi_minor = minor;
  d.kind = O3M_KIND_APP;
  d.struct_size = desc_size(minor);  // 84 for 1.11, 100 for 1.12 (the tail's four words are zero: an app carries none; image offsets 116..132, cold padding)
  d.name = kHot + 168;  // 200
  d.state_bytes = 64;
  d.param_count = 0;
  d.params = 0;
  d.init = 1;
  d.render = 0;
  d.set_param = 0;
  d.note_on = 0;
  d.note_off = 0;
  d.shutdown = 9;
  d.ui = views != 0u ? kHot + 128 : 0u;  // image offset 160
  d.view_count = views;
  std::memcpy(b.image.data() + kDesc, &d, sizeof d);
  const DescExt32 t{0, 0, 0, 0};
  std::memcpy(b.image.data() + kDesc + sizeof d, &t, sizeof t);
  const DescImp32 imp{0};
  std::memcpy(b.image.data() + kDesc + sizeof d + sizeof t, &imp, sizeof imp);
  std::memcpy(b.image.data() + 200, "Test", 5);
  b.relocs = {kDesc + 16, kDesc + 32, kDesc + 52, kDesc + 56, 160 + 12};
  if (views == 0u) {
    b.relocs.pop_back();
    b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), kDesc + 56), b.relocs.end());
  } else {
    const Ui32 ui{ui_size(minor), 0, 0, 9, 0};
    std::memcpy(b.image.data() + 160, &ui, sizeof ui);
  }
  b.h.magic = O3M_FILE_MAGIC;
  b.h.header_size = O3M_FILE_HEADER_SIZE;
  b.h.target = O3M_TARGET_ARMV7EM;
  b.h.kind = O3M_KIND_APP;
  b.h.abi_major = O3M_ABI_MAJOR;
  b.h.abi_minor = minor;
  b.h.hot_size = kHot;
  b.h.image_size = 256;
  b.h.desc_offset = kDesc;
  b.h.state_bytes = 64;
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.h.param_count = 0;
  b.h.view_count = static_cast<std::uint16_t>(views);
  b.seal();
  return b;
}

// An app (ABI 1.1): kind 4, no sound entries, one view; the views' table at image offset 160 (cold padding), view_draw at hot 8 (Thumb), enter / input / exit zero.
inline Blob blob_app(std::uint32_t views = 1, std::uint8_t minor = 1) {
  if (minor >= kImporterAbiMinor) {
    return blob_app11(views, minor);
  }
  if (minor >= 8) {
    return blob_app18(views, minor);
  }
  Blob b = good_blob();
  Desc32 d = get_desc(b);
  d.abi_minor = minor;
  d.kind = O3M_KIND_APP;
  d.render = d.set_param = d.note_on = d.note_off = 0;
  d.ui = views != 0u ? kHot + 128 : 0u;  // image offset 160
  d.view_count = views;
  put_desc(b, d);
  if (views != 0u) {
    const Ui32 ui{sizeof(Ui32), 0, 0, 9, 0};
    std::memcpy(b.image.data() + 160, &ui, sizeof ui);
    b.relocs = {8, 12, 48, 60, 64, 84, 88, 96, 116, 172};
  } else {
    b.relocs = {8, 12, 48, 60, 64, 84, 96, 116};
  }
  b.h.kind = O3M_KIND_APP;
  b.h.abi_minor = minor;
  b.h.view_count = static_cast<std::uint16_t>(views);
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.seal();
  return b;
}

// OS-INSFX-ABI2 A1: an effect of ABI 1.12: the 100-byte descriptor with the fourth tail (render_st in the hot part, WIDENS, two presets), three parameters (0: a float 0..1, 1: a KEY, an INT 0..16,
// 2: a float -10..10) and no pages or labels. Image 320 bytes, hot 32 as in good_blob(); the offsets are image offsets, exposed so that a test can break one word.
struct LayoutFx {
  static constexpr std::uint32_t kImageFx = 320;
  static constexpr std::uint32_t desc = 32;       // 100 bytes; the fourth tail at desc + 84
  static constexpr std::uint32_t render_st = 13;  // hot, Thumb
  static constexpr std::uint32_t params = 136;    // 3 x 20 bytes: A (0..1), Key (INT 0..16, O3M_PARAM_KEY), B (-10..10)
  static constexpr std::uint32_t presets = 200;   // 2 x 12 bytes
  static constexpr std::uint32_t values0 = 224;   // "Warm": 0.25, 3, -2 (three values)
  static constexpr std::uint32_t values1 = 236;   // "Wide": 1, 16 (two values; the third parameter takes its default)
  static constexpr std::uint32_t strings = 248;   // "Fx" 248, "A" 252, "Key" 256, "B" 260, "Warm" 264, "Wide" 272
};

inline Blob blob_fx12() {
  using L = LayoutFx;
  Blob b;
  b.image.assign(L::kImageFx, 0);
  for (std::uint32_t i = 0; i < kHot; ++i) {
    b.image[i] = static_cast<std::uint8_t>(0xA0u + i);
  }
  Desc32 d{};
  d.abi_major = O3M_ABI_MAJOR;
  d.abi_minor = kStereoAbiMinor;
  d.kind = O3M_KIND_EFFECT;
  d.struct_size = desc_size(kStereoAbiMinor);
  d.name = L::strings;
  d.state_bytes = 64;
  d.param_count = 3;
  d.params = L::params;
  d.init = 1;  // hot, Thumb
  d.render = 3;
  d.set_param = 5;
  d.note_on = 7;
  d.note_off = 9;
  d.shutdown = 11;
  std::memcpy(b.image.data() + L::desc, &d, sizeof d);
  const DescExt32 t{0, 0, 0, 0};
  std::memcpy(b.image.data() + L::desc + sizeof d, &t, sizeof t);
  const DescImp32 imp{0};
  std::memcpy(b.image.data() + L::desc + sizeof d + sizeof t, &imp, sizeof imp);
  const DescFx32 fx{L::render_st, O3M_FX_WIDENS, L::presets, 2};
  std::memcpy(b.image.data() + L::desc + sizeof d + sizeof t + sizeof imp, &fx, sizeof fx);
  b.put_param(L::params, L::strings + 4, 0.0f, 1.0f, 0.5f, 0);
  b.put_param(L::params + 20, L::strings + 8, 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY);
  b.put_param(L::params + 40, L::strings + 12, -10.0f, 10.0f, 0.0f, 0);
  const Preset32 pr[2] = {{L::strings + 16, L::values0, 3}, {L::strings + 24, L::values1, 2}};
  std::memcpy(b.image.data() + L::presets, pr, sizeof pr);
  const float v0[3] = {0.25f, 3.0f, -2.0f};
  const float v1[2] = {1.0f, 16.0f};
  std::memcpy(b.image.data() + L::values0, v0, sizeof v0);
  std::memcpy(b.image.data() + L::values1, v1, sizeof v1);
  std::memcpy(b.image.data() + L::strings, "Fx", 3);
  std::memcpy(b.image.data() + L::strings + 4, "A", 2);
  std::memcpy(b.image.data() + L::strings + 8, "Key", 4);
  std::memcpy(b.image.data() + L::strings + 12, "B", 2);
  std::memcpy(b.image.data() + L::strings + 16, "Warm", 5);
  std::memcpy(b.image.data() + L::strings + 24, "Wide", 5);
  b.relocs = {L::desc + 16, L::desc + 28, L::desc + 32, L::desc + 36, L::desc + 40, L::desc + 44, L::desc + 48, L::desc + 52,
              L::desc + 84, L::desc + 92,  // render_st, presets
              L::params, L::params + 20, L::params + 40, L::presets, L::presets + 4, L::presets + 12, L::presets + 16};
  b.h.magic = O3M_FILE_MAGIC;
  b.h.header_size = O3M_FILE_HEADER_SIZE;
  b.h.target = O3M_TARGET_ARMV7EM;
  b.h.kind = O3M_KIND_EFFECT;
  b.h.abi_major = O3M_ABI_MAJOR;
  b.h.abi_minor = kStereoAbiMinor;
  b.h.hot_size = kHot;
  b.h.image_size = L::kImageFx;
  b.h.desc_offset = L::desc;
  b.h.state_bytes = 64;
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.h.param_count = 3;
  b.seal();
  return b;
}

// The address a link-space value has once the module is loaded at kHotAddr / kColdAddr: the reference the loader is held to.
inline std::uint32_t moved(std::uint32_t v) { return v < kHot ? kHotAddr + v : kColdAddr + (v - kHot); }

// OS-FLASHDIET I1/I2: a kind 3 (importer) blob of ABI 1.11: the 84-byte descriptor with the importer table (eight words since I2), word 44 the layout tag. Image 512 bytes,
// hot 32 as in good_blob(); the offsets below are image offsets, exposed so that a test can break one word.
struct LayoutImp {
  static constexpr std::uint32_t kImageImp = 512;
  static constexpr std::uint32_t desc = 32;      // 84 bytes: Desc32 + DescExt32 + DescImp32
  static constexpr std::uint32_t table = 128;    // Importer32, 32 bytes (eight words, OS-FLASHDIET I2)
  static constexpr std::uint32_t formats = 160;  // 3 x Format32
  static constexpr std::uint32_t strings = 176;  // "Tracker" 176, "mod" 184, "s3m" 188, "med" 192
  static constexpr std::uint32_t format_count = 3;
};

// `minor` 12 (OS-INSFX-ABI2 A1) makes the descriptor 100 bytes (the four fx words zero): the table, the formats and the strings move up by blob_importer_shift(12) = 16 bytes, past the wider descriptor.
inline constexpr std::uint32_t blob_importer_shift(std::uint8_t minor) { return minor >= kStereoAbiMinor ? 16u : 0u; }
inline Blob blob_importer(std::uint8_t minor = kImporterAbiMinor) {
  using L = LayoutImp;
  const std::uint32_t sh = blob_importer_shift(minor);
  const std::uint32_t table = L::table + sh, formats = L::formats + sh, strings = L::strings + sh;
  Blob b;
  b.image.assign(L::kImageImp, 0);
  for (std::uint32_t i = 0; i < kHot; ++i) {
    b.image[i] = static_cast<std::uint8_t>(0xA0u + i);
  }
  Desc32 d{};
  d.abi_major = O3M_ABI_MAJOR;
  d.abi_minor = minor;
  d.kind = O3M_KIND_IMPORTER;
  d.struct_size = desc_size(minor);
  d.name = strings;
  d.state_bytes = 64;
  d.param_count = 0;
  d.params = 0;
  d.init = 1;  // hot, Thumb; the sound entries stay 0: an importer makes no sound
  d.render = 0;
  d.set_param = 0;
  d.note_on = 0;
  d.note_off = 0;
  d.shutdown = 9;
  d.ui = 0;
  d.view_count = 0;
  std::memcpy(b.image.data() + L::desc, &d, sizeof d);
  const DescExt32 t{0, 0, 0, 0};
  std::memcpy(b.image.data() + L::desc + sizeof d, &t, sizeof t);
  const DescImp32 imp{table};
  std::memcpy(b.image.data() + L::desc + sizeof d + sizeof t, &imp, sizeof imp);
  // (1.12: the DescFx32 words after it stay zero)
  const Importer32 tab{sizeof(Importer32), formats, L::format_count, 1, 3, 5, 7, 9};
  std::memcpy(b.image.data() + table, &tab, sizeof tab);
  b.put(formats, strings + 8);      // "mod"
  b.put(formats + 4, strings + 12);  // "s3m"
  b.put(formats + 8, strings + 16);  // "med"
  std::memcpy(b.image.data() + strings, "Tracker", 8);
  std::memcpy(b.image.data() + strings + 8, "mod", 4);
  std::memcpy(b.image.data() + strings + 12, "s3m", 4);
  std::memcpy(b.image.data() + strings + 16, "med", 4);
  b.relocs = {L::desc + 16, L::desc + 32, L::desc + 52, L::desc + 80, table + 4, table + 12, table + 16, table + 20, table + 24, table + 28, formats, formats + 4,
              formats + 8};
  b.h.magic = O3M_FILE_MAGIC;
  b.h.header_size = O3M_FILE_HEADER_SIZE;
  b.h.target = O3M_TARGET_ARMV7EM;
  b.h.kind = O3M_KIND_IMPORTER;
  b.h.abi_major = O3M_ABI_MAJOR;
  b.h.abi_minor = minor;
  b.h.hot_size = kHot;
  b.h.image_size = L::kImageImp;
  b.h.desc_offset = L::desc;
  b.h.state_bytes = 64;
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.h.param_count = 0;
  b.h.view_count = 0;
  b.h.reserved2[1] = model::kImportLayoutTag;  // word 44: the layout tag, kind 3 only
  b.seal();
  return b;
}

// The importer's table of a placed blob, read through resolve() as the OS reads it (the host fake of the read table): false when it is not wholly inside the image.
inline bool read_importer32(const o3m_file_header& h, const Placement& p, const DescImp32& imp, Importer32& out) {
  const std::uint8_t* at = (imp.importer & 3u) == 0u ? resolve(h, p, imp.importer, sizeof(Importer32)) : nullptr;
  if (at == nullptr) {
    return false;
  }
  std::memcpy(&out, at, sizeof out);
  return true;
}

// Memory for a placement, aligned like an arena's.
struct Mem {
  alignas(64) std::uint8_t hot[256] = {};
  alignas(64) std::uint8_t cold[1024] = {};
  Placement place() { return Placement{hot, kHotAddr, cold, kColdAddr}; }
};

// Copies the image into the placement as the loader does (hot part, then cold).
inline void place_image(const Blob& b, Placement p) {
  std::memcpy(p.hot_mem, b.image.data(), b.h.hot_size);
  std::memcpy(p.cold_mem, b.image.data() + b.h.hot_size, b.h.image_size - b.h.hot_size);
}

// OS-FLASHDIET A3: a whole .tmod file in the built-in table's form (builtin.h): the 64-byte header raw, the rest as a raw-deflate stream of STORED
// blocks (RFC 1951 BTYPE 0 -- trivial to write in C++, and core::Inflater reads it), so the loader's tests run the real inflate path without a Python run.
struct PackedFile {
  std::vector<std::uint8_t> header;  // sizeof(o3m_file_header) bytes, raw
  std::vector<std::uint8_t> z;       // the stored-block stream of the rest of the file
  std::uint32_t size = 0;            // the whole file's size
  BuiltinModule entry(const char* stem) const {
    return BuiltinModule{stem, header.data(), z.data(), static_cast<std::uint32_t>(z.size()), size};
  }
};

// A raw-deflate stream of `n` bytes as stored blocks (BFINAL on the last): each block is one header byte (BFINAL | BTYPE 00), then the byte-aligned LEN, NLEN and data.
inline std::vector<std::uint8_t> stored_deflate(const std::uint8_t* data, std::size_t n) {
  std::vector<std::uint8_t> out;
  std::size_t pos = 0;
  do {
    const std::size_t chunk = n - pos > 65535u ? 65535u : n - pos;
    const bool final = pos + chunk == n;
    out.push_back(final ? 1u : 0u);
    out.push_back(static_cast<std::uint8_t>(chunk & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((chunk >> 8) & 0xFFu));
    const std::uint16_t nlen = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(chunk));
    out.push_back(static_cast<std::uint8_t>(nlen & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xFFu));
    out.insert(out.end(), data + pos, data + pos + chunk);
    pos += chunk;
  } while (pos < n);
  return out;
}

inline PackedFile pack_builtin_file(const std::vector<std::uint8_t>& file) {
  PackedFile p;
  p.size = static_cast<std::uint32_t>(file.size());
  const std::size_t hlen = file.size() < sizeof(o3m_file_header) ? file.size() : sizeof(o3m_file_header);
  p.header.assign(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(hlen));
  p.z = stored_deflate(file.data() + hlen, file.size() - hlen);
  return p;
}

}  // namespace o3os::module::test
