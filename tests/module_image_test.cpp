// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012a: the module file's checks and the relocation (module/image.h) on hand-made files: a good one is relocated to two far apart addresses and every
// word is what an independent reference says, and every way a hostile or damaged file can be wrong is refused with its own Status, with nothing written
// by a relocation that fails. (The file the packer makes from a real ELF is in module_fixture_test.cpp.) Host and Cortex-M4F.
#include <doctest/doctest.h>

// The build has no exceptions, so no REQUIRE: a failed precondition is recorded and ends the test case.
#define NEED(cond) \
  do {             \
    const bool need_ok_ = static_cast<bool>(cond); \
    CHECK(need_ok_); \
    if (!need_ok_) {  \
      return;         \
    }                 \
  } while (0)

#include "support/module_blob.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {

using namespace o3os::module;
using namespace o3os::module::test;

// A header check of the blob's file as it stands.
Status header_status(const Blob& b) { return check_header(b.h, static_cast<std::uint32_t>(b.file().size())); }

// Relocates `b` into `mem`; returns the Relocator's verdict (the table in one piece).
Status relocate(const Blob& b, Mem& mem) {
  place_image(b, mem.place());
  Relocator r(b.h, mem.place());
  return r.apply(b.relocs.data(), static_cast<std::uint32_t>(b.relocs.size()));
}

}  // namespace

TEST_CASE("module image: the SDK's layout is what the format says") {
  CHECK(sizeof(o3m_file_header) == 64);
  CHECK(offsetof(o3m_file_header, hot_size) == 12);
  CHECK(offsetof(o3m_file_header, view_count) == 34);
  CHECK(offsetof(o3m_file_header, module_version) == 36);  // OS-MODVER: the first of the three reserved words until ABI 1.2
  CHECK(offsetof(o3m_file_header, reserved2) == 40);
  CHECK(offsetof(o3m_file_header, image_crc) == 48);
  CHECK(offsetof(o3m_file_header, reserved3) == 52);
  CHECK(sizeof(Desc32) == 64);  // ABI 1.0..1.7
  CHECK(sizeof(DescExt32) == 16);  // ABI 1.8 appends four words
  CHECK(sizeof(Param32) == 20);
  CHECK((desc_size(0) == 64 && desc_size(7) == 64 && desc_size(8) == 80 && desc_size(10) == 80 && desc_size(11) == 84 && desc_size(12) == 100));  // the descriptor's size is per declared minor (OS-MODPITCH, 1.11, 1.12)
  CHECK((sizeof(DescFx32) == 16 && sizeof(Preset32) == 12));  // ABI 1.12 (OS-INSFX-ABI2 A1): four words appended, a preset is three
  CHECK((sizeof(Page32) == 8 && sizeof(Labels32) == 8));
  CHECK((O3M_MODULE_SIZE(7) == offsetof(o3m_module, pitch) && O3M_MODULE_SIZE(0) == offsetof(o3m_module, pitch) && O3M_MODULE_SIZE(8) == offsetof(o3m_module, importer) &&
         O3M_MODULE_SIZE(11) == offsetof(o3m_module, render_st) && O3M_MODULE_SIZE(12) == sizeof(o3m_module) && O3M_MODULE_SIZE(13) == sizeof(o3m_module)));  // (the descriptor is 84 bytes from 1.11, 100 from 1.12)
  if (sizeof(void*) == 4) {
    CHECK(sizeof(o3m_module) == sizeof(Desc32) + sizeof(DescExt32) + sizeof(DescImp32) + sizeof(DescFx32));  // 100 bytes from 1.12
    CHECK(O3M_MODULE_SIZE(12) == 100u);
    CHECK((offsetof(o3m_module, render_st) == 84 && offsetof(o3m_module, fx_flags) == 88 && offsetof(o3m_module, presets) == 92 && offsetof(o3m_module, preset_count) == 96));
    CHECK((offsetof(o3m_module, render_st) == sizeof(Desc32) + sizeof(DescExt32) + sizeof(DescImp32) + offsetof(DescFx32, render_st) &&
           offsetof(o3m_module, presets) == sizeof(Desc32) + sizeof(DescExt32) + sizeof(DescImp32) + offsetof(DescFx32, presets) &&
           offsetof(o3m_module, preset_count) == sizeof(Desc32) + sizeof(DescExt32) + sizeof(DescImp32) + offsetof(DescFx32, preset_count)));
    CHECK((sizeof(o3m_host) == 60 && offsetof(o3m_host, get_clock) == 52 && offsetof(o3m_host, track_level) == 56));
    CHECK((sizeof(o3m_preset) == sizeof(Preset32) && sizeof(o3m_clock) == 16));
    CHECK(offsetof(o3m_module, pitch) == sizeof(Desc32));              // and the 64 before it are Desc32 (1.0..1.7)
    CHECK(O3M_MODULE_SIZE(7) == 64u);
    CHECK(O3M_MODULE_SIZE(8) == 80u);
    CHECK(O3M_MODULE_SIZE(11) == 84u);
    CHECK(offsetof(o3m_module, importer) == 80);
    CHECK(offsetof(o3m_module, init) == offsetof(Desc32, init));
    CHECK(offsetof(o3m_module, ui) == offsetof(Desc32, ui));  // the UI table reserved for kind app (OS-012b)
    CHECK(offsetof(o3m_module, view_count) == offsetof(Desc32, view_count));
    CHECK(offsetof(o3m_module, pitch) == sizeof(Desc32) + offsetof(DescExt32, pitch));
    CHECK(offsetof(o3m_module, pages) == sizeof(Desc32) + offsetof(DescExt32, pages));
    CHECK(offsetof(o3m_module, page_count) == sizeof(Desc32) + offsetof(DescExt32, page_count));
    CHECK(offsetof(o3m_module, labels) == sizeof(Desc32) + offsetof(DescExt32, labels));
    CHECK(O3M_UI_SIZE(8) == sizeof(Ui32));  // ABI 1.9: `keys` is the sixth word, the table is sized per minor
    CHECK(ui_size(8) == sizeof(Ui32));
    CHECK(ui_size(9) == sizeof(Ui32) + 4u);
    CHECK(sizeof(o3m_param) == sizeof(Param32));
    CHECK(sizeof(o3m_page) == sizeof(Page32));
    CHECK(sizeof(o3m_labels) == sizeof(Labels32));
  }
}

// The 64-byte descriptor of an ABI 1.7 module, written out as the sixteen words of the published layout (not through any struct of this tree): a module built in the 1.7 days has exactly these
// bytes, and the OS of 1.8 reads it unchanged (OS-MODPITCH Done-when). Link addresses of the good blob: name 136, params 96, init 1, render 3, set_param 153, note_on 5, note_off 7, shutdown 9.
constexpr std::uint32_t k17Descriptor[16] = {1, 7, O3M_KIND_INSTRUMENT, 64, 136, 64, 2, 96, 1, 3, 153, 5, 7, 9, 0, 0};

TEST_CASE("module image: a real 1.7 descriptor (64 bytes, minor 7) still loads: its 1.8 tail is zero and nothing past byte 64 is read") {
  Blob b = good_blob();
  CHECK(std::memcmp(b.image.data() + kDesc, k17Descriptor, sizeof k17Descriptor) == 0);  // the fixture IS the published 1.7 layout
  CHECK(b.h.abi_minor == 7);
  CHECK(header_status(b) == Status::Ok);
  Mem mem;
  NEED(relocate(b, mem) == Status::Ok);
  // what follows the 64 bytes is the module's other data (here its parameter table, whose first words are a name pointer and floats): taken for the 1.8 members it would be refused
  Desc32 d{};
  DescExt32 ext{};
  ext.pitch = ext.pages = ext.page_count = ext.labels = 0xDEADBEEFu;
  CHECK(read_desc(b.h, mem.place(), d, &ext) == Status::Ok);
  CHECK((ext.pitch == 0 && ext.pages == 0 && ext.page_count == 0 && ext.labels == 0));
  CHECK(d.struct_size == 64);
  // the size is per minor: a 1.7 file that says 80, and a 1.8 file that says 64, are both refused
  Blob big = good_blob();
  big.put(kDesc + 12, 80);
  Mem m2;
  NEED(relocate(big, m2) == Status::Ok);
  CHECK(read_desc(big.h, m2.place(), d) == Status::BadDescriptor);
  Blob old8 = blob18();
  old8.put(Layout18::desc + 12, 64);
  Mem m3;
  NEED(relocate(old8, m3) == Status::Ok);
  CHECK(read_desc(old8.h, m3.place(), d) == Status::BadDescriptor);
}

namespace {

// Relocates a copy of `b` after `edit` changed it and returns read_desc's verdict (the relocation must pass, so an edit keeps every pointer word inside the image); `ext` gets the tail.
Status verdict18(const Blob& b, DescExt32* ext = nullptr) {
  Mem mem;
  if (relocate(b, mem) != Status::Ok) {
    return Status::BadRelocValue;
  }
  Desc32 d{};
  return read_desc(b.h, mem.place(), d, ext);
}

// The blob with the relocation of the word at image offset `at` removed (a word that is to hold zero must not be moved to a load address).
Blob unrelocated(Blob b, std::uint32_t at) {
  b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), at), b.relocs.end());
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  return b;
}

}  // namespace

TEST_CASE("module image: an ABI 1.8 file (80-byte descriptor) is read whole: pitch, pages and labels are where the layout puts them") {
  using L = Layout18;
  Blob b = blob18();
  CHECK(header_status(b) == Status::Ok);
  Mem mem;
  NEED(relocate(b, mem) == Status::Ok);
  Desc32 d{};
  DescExt32 ext{};
  CHECK(read_desc(b.h, mem.place(), d, &ext) == Status::Ok);
  CHECK(d.struct_size == 80);
  CHECK(d.param_count == 10);
  CHECK(ext.pitch == moved(L::pitch_fn));
  CHECK((ext.pitch & 1u) == 1u);  // Thumb
  CHECK(ext.pages == moved(L::pages));
  CHECK(ext.page_count == 2);
  CHECK(ext.labels == moved(L::labels));
  const std::uint8_t* pages = resolve(b.h, mem.place(), ext.pages, 2 * sizeof(Page32));
  NEED(pages != nullptr);
  Page32 p1;
  std::memcpy(&p1, pages + sizeof(Page32), sizeof p1);
  CHECK(p1.first_param == 8);
  CHECK(std::strcmp(reinterpret_cast<const char*>(resolve(b.h, mem.place(), p1.name, 1)), "Env") == 0);
  // the same file without the three optional members is a valid 1.8 module too
  Blob plain = blob18();
  plain.put(L::desc + 64, 0);
  plain.put(L::desc + 68, 0);
  plain.put(L::desc + 72, 0);
  plain.put(L::desc + 76, 0);
  plain = unrelocated(unrelocated(unrelocated(plain, L::desc + 64), L::desc + 68), L::desc + 76);
  CHECK(verdict18(plain) == Status::Ok);
  // a header that says 1.8 with room for less than 80 bytes of descriptor is refused at the header
  Blob tight = blob18();
  tight.h.desc_offset = L::kImage18 - 76;
  CHECK(header_status(tight) == Status::BadSize);
  tight.h.desc_offset = L::kImage18 - 80;
  CHECK(header_status(tight) == Status::Ok);
}

TEST_CASE("module image: the views' table is sized per minor, 20 bytes up to 1.8 and 24 from 1.9: the other size is refused, and the keys word of 1.9 is null or code (OS-KEYROWDLG S5)") {
  constexpr std::uint32_t ui_at = 488;  // blob_app18 puts the table here; the 1.9 `keys` word is its sixth, at ui_at + 20
  CHECK(verdict18(blob_app(1, 8)) == Status::Ok);  // 20 bytes at 1.8
  CHECK(verdict18(blob_app(1, 9)) == Status::Ok);  // 24 bytes at 1.9, keys null
  {
    Blob b = blob_app(1, 8);
    b.put(ui_at, 24);  // a 1.9 table in a file that declares 1.8: the module was built against the new header and said the old minor
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  {
    Blob b = blob_app(1, 9);
    b.put(ui_at, 20);  // a 1.8 table in a file that declares 1.9: no room for `keys`, the OS must not read past it
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  for (const std::uint32_t size : {0u, 16u, 28u}) {
    for (const std::uint8_t minor : {std::uint8_t{8}, std::uint8_t{9}}) {
      Blob b = blob_app(1, minor);
      b.put(ui_at, size);
      CAPTURE(size);
      CAPTURE(static_cast<int>(minor));
      CHECK(verdict18(b) == Status::BadDescriptor);
    }
  }
  {  // a keys function: Thumb code inside the image (odd), relocated like the other pointers
    Blob b = blob_app(1, 9);
    b.put(ui_at + 20, 11);
    b.relocs.push_back(ui_at + 20);
    std::sort(b.relocs.begin(), b.relocs.end());
    b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
    CHECK(verdict18(b) == Status::Ok);
    Blob even = b;
    even.put(ui_at + 20, 10);  // not a Thumb address
    CHECK(verdict18(even) == Status::BadDescriptor);
  }
}

TEST_CASE("module image: every way the 1.8 members can be wrong is refused: pitch, the page table and the labels") {
  using L = Layout18;
  CHECK(verdict18(blob18()) == Status::Ok);
  struct Case {
    const char* what;
    std::uint32_t at, value;
  };
  // each case sets one word of the good 1.8 image to a value that is inside the image (so that the relocation passes) but wrong
  const Case bad[] = {
      {"pitch even (not Thumb)", L::desc + 64, 12},
      {"page count 5", L::desc + 72, 5},
      {"page count 0 with a table", L::desc + 72, 0},
      {"page table unaligned", L::desc + 68, L::pages + 1},
      {"first page not 0", L::pages + 4, 1},
      {"pages descend", L::pages + 12, 0},
      {"page 0 holds nine parameters", L::pages + 12, 9},
      {"last page is empty", L::pages + 12, 10},
      {"page past the parameters", L::pages + 12, 11},
      {"page name empty", L::pages, 500},
      {"labels unaligned", L::desc + 76, L::labels + 2},
      {"label count 4 for a 0..2 parameter", L::labels + 4, 4},
      {"label count 1", L::labels + 4, 1},
      {"label table unaligned", L::labels, L::names0 + 1},
      {"a label string with no text", L::names0, 500},
  };
  for (const Case& c : bad) {
    Blob b = blob18();
    b.put(c.at, c.value);
    CAPTURE(std::string(c.what));
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  {  // labels on an INT that starts above 0 (FM Algorithm 1..8): the names count from the minimum, count = max - min + 1, and the loader takes them (the params page must describe them too)
    const auto bits = [](float f) {
      std::uint32_t u = 0;
      std::memcpy(&u, &f, sizeof u);
      return u;
    };
    Blob b = blob18();
    b.put(L::params + 4, bits(1.0f));   // min 1
    b.put(L::params + 8, bits(3.0f));   // max 3: three values, three names
    b.put(L::params + 12, bits(2.0f));  // def 2
    CHECK(verdict18(b) == Status::Ok);
    b.put(L::params + 8, bits(4.0f));  // four values for three names
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  {  // a label table for a float parameter (parameter 1)
    Blob b = blob18();
    b.put(L::labels + 8, L::names0);  // (the word is not in the relocation list: read_desc sees the raw value, and the INT check fails first)
    b.put(L::labels + 12, 3);
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  // strings that are too long or not printable
  const std::uint32_t kOsc = L::strings + 24, kSaw = L::strings + 32;  // "Osc" (a page name), "Saw" (a label)
  {
    Blob b = blob18();
    std::memcpy(b.image.data() + kOsc, "Abcdefgh", 8);  // no NUL within the 7 characters a page name may have
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  {
    Blob b = blob18();
    std::memcpy(b.image.data() + kOsc, "Abcdefg", 8);  // 7 characters and the NUL: allowed
    CHECK(verdict18(b) == Status::Ok);
  }
  {
    Blob b = blob18();
    std::memcpy(b.image.data() + kSaw, "abcdefghijkl", 12);  // 12 characters: a label has 11 at most (the next string follows it with no NUL between)
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  {
    Blob b = blob18();
    std::memcpy(b.image.data() + kSaw, "abcdefghijk", 12);  // 11 characters and the NUL: allowed
    CHECK(verdict18(b) == Status::Ok);
  }
  {
    Blob b = blob18();
    b.image[kSaw] = 0x01;  // not printable
    CHECK(verdict18(b) == Status::BadDescriptor);
  }
  // pitch past the image does not even relocate; pages and labels may be dropped on their own
  {
    Blob b = blob18();
    b.put(L::desc + 64, 0x3000);
    Mem mem;
    CHECK(relocate(b, mem) == Status::BadRelocValue);
  }
  {
    Blob b = unrelocated(blob18(), L::desc + 76);
    b.put(L::desc + 76, 0);
    CHECK(verdict18(b) == Status::Ok);  // no labels
    b = unrelocated(unrelocated(b, L::desc + 68), L::pages + 0);
    b.put(L::desc + 68, 0);
    b.put(L::desc + 72, 0);
    b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), L::pages + 8), b.relocs.end());
    b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
    CHECK(verdict18(b) == Status::Ok);  // and no pages: the "1-8" pages as before
  }
  // a page table with no parameters, and pages with a count of zero and a table, are refused
  {
    Blob b = unrelocated(unrelocated(unrelocated(blob18(), L::desc + 76), L::desc + 28), L::params);
    b.put(L::desc + 76, 0);
    b.put(L::desc + 24, 0);
    b.put(L::desc + 28, 0);
    b.h.param_count = 0;
    for (unsigned i = 1; i < 10; ++i) {
      b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), L::params + 20u * i), b.relocs.end());
    }
    b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
    CHECK(verdict18(b) == Status::BadDescriptor);  // pages, but no parameters to put on them
  }
}

TEST_CASE("module image: the module version is O3M_VERSION's number, the little-endian word at byte 36 of the file, under the CRC") {
  CHECK(O3M_VERSION(1, 2, 3) == 0x00010203u);
  CHECK(O3M_VERSION(0, 0, 0) == 0u);
  CHECK(O3M_VERSION(65535, 255, 255) == 0xFFFFFFFFu);
  CHECK((O3M_VERSION_MAJOR(0x00010203u) == 1u && O3M_VERSION_MINOR(0x00010203u) == 2u && O3M_VERSION_PATCH(0x00010203u) == 3u));
  CHECK(O3M_VERSION(1, 255, 255) < O3M_VERSION(2, 0, 0));  // the numbers order as versions do: major, then minor, then patch
  CHECK(O3M_VERSION(1, 1, 255) < O3M_VERSION(1, 2, 0));
  const Blob b = blob_version(O3M_VERSION(1, 2, 3));
  const std::vector<std::uint8_t> f = b.file();
  CHECK((f[36] == 3 && f[37] == 2 && f[38] == 1 && f[39] == 0));
  CHECK((f[40] == 0 && f[44] == 0));  // the other reserved words
  CHECK(header_status(b) == Status::Ok);
  Blob c = b;
  c.h.module_version = O3M_VERSION(1, 2, 4);
  c.seal();
  CHECK(c.h.image_crc != b.h.image_crc);  // the version is under the CRC: a file edited by hand and not resealed is refused by the loader (BadCrc)
}

TEST_CASE("module image: a good file passes every check") {
  Blob b = good_blob();
  CHECK(header_status(b) == Status::Ok);
  Mem mem;
  CHECK(relocate(b, mem) == Status::Ok);
  Desc32 d{};
  CHECK(read_desc(b.h, mem.place(), d) == Status::Ok);
  CHECK(d.param_count == 2);
  CHECK(d.name == moved(kHot + 104));
  CHECK(d.set_param == moved(kHot + 120) + 1);  // the Thumb bit survives the move
  CHECK(d.init == moved(0) + 1);
  Param32 p{};
  CHECK(read_param(b.h, mem.place(), d, 1, p));
  CHECK(p.min == -10.0f);
  CHECK(p.flags == O3M_PARAM_LOG);
  CHECK(!read_param(b.h, mem.place(), d, 2, p));
}

TEST_CASE("module image: relocation moves exactly the words in the table, to where the reference says") {
  Blob b = good_blob();
  Mem mem;
  NEED(relocate(b, mem) == Status::Ok);
  std::vector<std::uint8_t> before = b.image;
  for (std::uint32_t off = 0; off < kImage; off += 4) {
    std::uint32_t was, got;
    std::memcpy(&was, before.data() + off, 4);
    std::memcpy(&got, off < kHot ? mem.hot + off : mem.cold + (off - kHot), 4);
    bool listed = false;
    for (std::uint32_t r : b.relocs) {
      listed = listed || r == off;
    }
    if (listed) {
      CHECK_MESSAGE(got == moved(was), "offset " << off);
    } else {
      CHECK_MESSAGE(got == was, "offset " << off);
    }
  }
  // the hot literal now names the cold string, the end pointer the end of the cold part
  std::uint32_t lit, end;
  std::memcpy(&lit, mem.hot + 8, 4);
  std::memcpy(&end, mem.hot + 12, 4);
  CHECK(lit == kColdAddr + 112);
  CHECK(end == kColdAddr + (kImage - kHot));
  // and the pieces: the table applied in many small calls gives the same bytes
  Mem again;
  place_image(b, again.place());
  Relocator r(b.h, again.place());
  for (std::uint32_t i = 0; i < b.relocs.size(); ++i) {
    CHECK(r.apply(&b.relocs[i], 1) == Status::Ok);
  }
  CHECK(r.complete());
  CHECK(std::memcmp(again.hot, mem.hot, sizeof mem.hot) == 0);
  CHECK(std::memcmp(again.cold, mem.cold, sizeof mem.cold) == 0);
}

TEST_CASE("module image: a module with no hot part and one that is all hot but its descriptor") {
  Blob b = good_blob();
  // hot_size 0: link space is all cold; the same bytes, offsets shifted by nothing: values below 32 are now cold addresses
  b.h.hot_size = 0;
  b.seal();
  CHECK(header_status(b) == Status::Ok);
  Mem mem;
  Placement p = mem.place();
  p.hot_mem = nullptr;
  std::memcpy(p.cold_mem, b.image.data(), kImage);
  Relocator r(b.h, p);
  CHECK(r.apply(b.relocs.data(), static_cast<std::uint32_t>(b.relocs.size())) == Status::Ok);
  std::uint32_t lit;
  std::memcpy(&lit, mem.cold + 8, 4);
  CHECK(lit == kColdAddr + kHot + 112);
}

TEST_CASE("module image: every header field is held to its range") {
  struct Case {
    const char* what;
    void (*mutate)(o3m_file_header&);
    Status want;
  };
  const Case cases[] = {
      {"magic", [](o3m_file_header& h) { h.magic ^= 1u; }, Status::BadMagic},
      {"header size", [](o3m_file_header& h) { h.header_size = 60; }, Status::BadHeaderSize},
      {"target", [](o3m_file_header& h) { h.target = 2; }, Status::BadTarget},
      {"kind 0", [](o3m_file_header& h) { h.kind = 0; }, Status::BadKind},
      {"kind importer", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; }, Status::BadKind},  // (at the blob's ABI 1.7: the kind is published at 1.11, OS-FLASHDIET I1)
      {"kind importer at ABI 1.11 with the layout tag", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = 11; h.reserved2[1] = o3os::model::kImportLayoutTag; }, Status::Ok},
      {"kind importer at ABI 1.11 with a wrong tag", [](o3m_file_header& h) { h.kind = O3M_KIND_IMPORTER; h.abi_minor = 11; h.reserved2[1] = o3os::model::kImportLayoutTag ^ 1u; }, Status::LayoutMismatch},
      {"kind app of ABI 1.0 (the app kind is 1.1)", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 0; }, Status::BadKind},
      {"kind app of ABI 1.1", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; }, Status::Ok},
      {"abi major", [](o3m_file_header& h) { h.abi_major = 2; }, Status::AbiMajor},
      {"abi major 0", [](o3m_file_header& h) { h.abi_major = 0; }, Status::AbiMajor},
      {"abi minor", [](o3m_file_header& h) { h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1); }, Status::AbiMinor},
      // OS-MODRATE: a module built against ABI 1.13 (O3M_FX_RATE) is accepted by this OS and refused by one that stops at 1.12 (the rule is `abi_minor > kOsAbiMinor`, as the row above pins for any OS)
      {"abi minor 13 (O3M_FX_RATE), this OS", [](o3m_file_header& h) { h.abi_minor = 13; }, Status::Ok},
      {"abi minor 255", [](o3m_file_header& h) { h.abi_minor = 255; }, Status::AbiMinor},
      {"flags", [](o3m_file_header& h) { h.flags = 1; }, Status::BadReserved},
      {"view count past the limit", [](o3m_file_header& h) { h.view_count = kMaxViews + 1; }, Status::BadSize},
      {"reserved2[0]", [](o3m_file_header& h) { h.reserved2[0] = 1; }, Status::BadReserved},
      {"reserved2[1]", [](o3m_file_header& h) { h.reserved2[1] = 1; }, Status::BadReserved},
      {"module version at ABI 1.2", [](o3m_file_header& h) { h.module_version = O3M_VERSION(1, 2, 3); }, Status::Ok},
      {"module version, every bit, at ABI 1.2", [](o3m_file_header& h) { h.module_version = 0xFFFFFFFFu; }, Status::Ok},
      {"no module version at ABI 1.1", [](o3m_file_header& h) { h.abi_minor = 1; }, Status::Ok},
      {"module version at ABI 1.1 (a reserved word there)", [](o3m_file_header& h) { h.abi_minor = 1; h.module_version = O3M_VERSION(1, 0, 0); }, Status::BadReserved},
      {"module version at ABI 1.1, the lowest value", [](o3m_file_header& h) { h.abi_minor = 1; h.module_version = 1; }, Status::BadReserved},
      {"module version at ABI 1.0", [](o3m_file_header& h) { h.abi_minor = 0; h.module_version = O3M_VERSION(0, 0, 1); }, Status::BadReserved},
      {"no module version at ABI 1.0", [](o3m_file_header& h) { h.abi_minor = 0; }, Status::Ok},
      {"module version above the OS's ABI", [](o3m_file_header& h) { h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1); h.module_version = 1; }, Status::AbiMinor},
      // OS-H1 (ABI 1.3): reserved2[0] is the capability mask of an app. Only an app of ABI 1.3 may set it, and only the bits ABI 1.3 defines.
      {"app of ABI 1.3, no capabilities", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; }, Status::Ok},
      {"app of ABI 1.3, midi_in", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_MIDI_IN; }, Status::Ok},
      {"app of ABI 1.3, all three", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = O3M_CAPS_ABI_1_3; }, Status::Ok},
      {"app of ABI 1.3, a module version and capabilities", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.module_version = O3M_VERSION(1, 0, 0); h.reserved2[0] = O3M_CAP_EDIT_PROJECT; }, Status::Ok},
      {"app of ABI 1.2 asks for a capability (a reserved word there)", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 2; h.reserved2[0] = O3M_CAP_MIDI_IN; }, Status::BadReserved},
      {"app of ABI 1.1 asks for a capability", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 1; h.reserved2[0] = O3M_CAP_READ_PROJECT; }, Status::BadReserved},
      {"instrument of ABI 1.3 asks for a capability", [](o3m_file_header& h) { h.kind = O3M_KIND_INSTRUMENT; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_MIDI_IN; }, Status::BadReserved},
      {"effect of ABI 1.3 asks for a capability", [](o3m_file_header& h) { h.kind = O3M_KIND_EFFECT; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_READ_PROJECT; }, Status::BadReserved},
      // OS-H2a (ABI 1.4): audio_tap is granted, to a file of ABI 1.4. In a file of 1.3 it is a reserved field (the file is wrong, not the OS old).
      {"app of ABI 1.4, audio_tap", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 4; h.reserved2[0] = O3M_CAP_AUDIO_TAP; }, Status::Ok},
      {"app of ABI 1.4, every capability of it", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 4; h.reserved2[0] = O3M_CAPS_ABI_1_4; }, Status::Ok},
      {"app of ABI 1.4, midi_in only", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 4; h.reserved2[0] = O3M_CAP_MIDI_IN; }, Status::Ok},
      {"app of ABI 1.3 asks for audio_tap (1.4's bit: a reserved field there)", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_AUDIO_TAP; }, Status::BadReserved},
      {"app of ABI 1.3 asks for audio_tap and export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_AUDIO_TAP | O3M_CAP_EXPORT_FILES; }, Status::BadReserved},
      {"app of ABI 1.4 asks for export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 4; h.reserved2[0] = O3M_CAP_EXPORT_FILES; }, Status::BadReserved},  // (OS-H4: 1.7's bit: a reserved field below 1.7)
      {"app of ABI 1.4 asks for audio_tap and midi_out", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 4; h.reserved2[0] = O3M_CAP_AUDIO_TAP | O3M_CAP_MIDI_OUT; }, Status::CapUnsupported},
      {"app of ABI 1.4 asks for a bit nobody published", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 4; h.reserved2[0] = O3M_CAP_AUDIO_TAP | 64u; }, Status::BadReserved},
      {"app of ABI 1.4, every bit", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 4; h.reserved2[0] = 0xFFFFFFFFu; }, Status::BadReserved},
      {"instrument of ABI 1.4 asks for audio_tap", [](o3m_file_header& h) { h.kind = O3M_KIND_INSTRUMENT; h.abi_minor = 4; h.reserved2[0] = O3M_CAP_AUDIO_TAP; }, Status::BadReserved},
      {"effect of ABI 1.4 asks for audio_tap", [](o3m_file_header& h) { h.kind = O3M_KIND_EFFECT; h.abi_minor = 4; h.reserved2[0] = O3M_CAP_AUDIO_TAP; }, Status::BadReserved},
      {"app of ABI 1.3 asks for export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_EXPORT_FILES; }, Status::BadReserved},
      {"app of ABI 1.3 asks for midi_out", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_MIDI_OUT; }, Status::CapUnsupported},
      {"app of ABI 1.3 asks for a bit nobody published", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = 64u; }, Status::BadReserved},
      {"app of ABI 1.3 asks for midi_in and export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = O3M_CAP_MIDI_IN | O3M_CAP_EXPORT_FILES; }, Status::BadReserved},
      // OS-H4 (ABI 1.7): export_files is granted, to a file of ABI 1.7. midi_out is still the one published bit nobody grants.
      {"app of ABI 1.7, export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 7; h.reserved2[0] = O3M_CAP_EXPORT_FILES; }, Status::Ok},
      {"app of ABI 1.7, read_project and export_files (SMF EXPORT)", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 7; h.reserved2[0] = O3M_CAP_READ_PROJECT | O3M_CAP_EXPORT_FILES; }, Status::Ok},
      {"app of ABI 1.7, every capability of it", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 7; h.reserved2[0] = O3M_CAPS_ABI_1_7; }, Status::Ok},
      // OS-APPUX S3 (ABI 1.10): play_notes (64) is granted, to a file of ABI 1.10; below it the bit is a reserved field, and a file past the OS's minor is refused by the minor rule first.
      {"app of ABI 1.10, play_notes", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 10; h.reserved2[0] = O3M_CAP_PLAY_NOTES; }, Status::Ok},
      {"app of ABI 1.10, every capability of it", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 10; h.reserved2[0] = O3M_CAPS_ABI_1_10; }, Status::Ok},
      {"app of ABI 1.10, no capability", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 10; h.reserved2[0] = 0; }, Status::Ok},
      {"app of ABI 1.9 asks for play_notes (a reserved field there)", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 9; h.reserved2[0] = O3M_CAP_PLAY_NOTES; }, Status::BadReserved},
      {"app of ABI 1.7 asks for play_notes", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 7; h.reserved2[0] = O3M_CAPS_ABI_1_7 | O3M_CAP_PLAY_NOTES; }, Status::BadReserved},
      {"app of ABI 1.10 asks for play_notes and midi_out", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 10; h.reserved2[0] = O3M_CAP_PLAY_NOTES | O3M_CAP_MIDI_OUT; }, Status::CapUnsupported},
      {"app of ABI 1.10 asks for a bit nobody published", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 10; h.reserved2[0] = O3M_CAP_PLAY_NOTES | 128u; }, Status::BadReserved},
      {"instrument of ABI 1.10 asks for play_notes", [](o3m_file_header& h) { h.kind = O3M_KIND_INSTRUMENT; h.abi_minor = 10; h.reserved2[0] = O3M_CAP_PLAY_NOTES; }, Status::BadReserved},
      {"app of ABI 1.6 asks for export_files (a reserved field there)", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 6; h.reserved2[0] = O3M_CAP_EXPORT_FILES; }, Status::BadReserved},
      {"app of ABI 1.5 asks for export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 5; h.reserved2[0] = O3M_CAP_EDIT_PROJECT | O3M_CAP_EXPORT_FILES; }, Status::BadReserved},
      {"app of ABI 1.7 asks for midi_out", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 7; h.reserved2[0] = O3M_CAP_MIDI_OUT; }, Status::CapUnsupported},
      {"app of ABI 1.7 asks for export_files and midi_out", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 7; h.reserved2[0] = O3M_CAP_EXPORT_FILES | O3M_CAP_MIDI_OUT; }, Status::CapUnsupported},
      {"app of ABI 1.7 asks for a bit nobody published", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 7; h.reserved2[0] = O3M_CAP_EXPORT_FILES | 64u; }, Status::BadReserved},
      {"instrument of ABI 1.7 asks for export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_INSTRUMENT; h.abi_minor = 7; h.reserved2[0] = O3M_CAP_EXPORT_FILES; }, Status::BadReserved},
      {"effect of ABI 1.7 asks for export_files", [](o3m_file_header& h) { h.kind = O3M_KIND_EFFECT; h.abi_minor = 7; h.reserved2[0] = O3M_CAP_EXPORT_FILES; }, Status::BadReserved},
      {"app of ABI 1.3, every bit", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = 3; h.reserved2[0] = 0xFFFFFFFFu; }, Status::BadReserved},
      {"app of a newer minor than the OS's asks for a capability", [](o3m_file_header& h) { h.kind = O3M_KIND_APP; h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1); h.reserved2[0] = 1; }, Status::AbiMinor},
      {"reserved3", [](o3m_file_header& h) { h.reserved3[0] = 1; }, Status::BadReserved},
      {"image zero", [](o3m_file_header& h) { h.image_size = 0; }, Status::BadSize},
      {"image too big", [](o3m_file_header& h) { h.image_size = kMaxImageBytes + 4; }, Status::BadSize},
      {"image unaligned", [](o3m_file_header& h) { h.image_size = kImage + 2; }, Status::BadSize},
      {"hot past image", [](o3m_file_header& h) { h.hot_size = kImage + 4; }, Status::BadSize},
      {"hot is all of it", [](o3m_file_header& h) { h.hot_size = kImage; }, Status::BadSize},
      {"hot unaligned", [](o3m_file_header& h) { h.hot_size = kHot + 2; }, Status::BadSize},
      {"state zero", [](o3m_file_header& h) { h.state_bytes = 0; }, Status::BadSize},
      {"state huge", [](o3m_file_header& h) { h.state_bytes = kMaxStateBytes + 1; }, Status::BadSize},
      {"too many params", [](o3m_file_header& h) { h.param_count = kMaxParams + 1; }, Status::BadSize},
      {"reloc count huge", [](o3m_file_header& h) { h.reloc_count = 0xFFFFFFFFu; }, Status::BadSize},
      {"reloc count past the words", [](o3m_file_header& h) { h.reloc_count = kImage / 4 + 1; }, Status::BadSize},
      {"desc unaligned", [](o3m_file_header& h) { h.desc_offset = kDesc + 2; }, Status::BadSize},
      {"desc in hot", [](o3m_file_header& h) { h.desc_offset = 16; }, Status::BadSize},
      {"desc at the end", [](o3m_file_header& h) { h.desc_offset = kImage - 4; }, Status::BadSize},
      {"desc past the end", [](o3m_file_header& h) { h.desc_offset = kImage; }, Status::BadSize},
      {"reloc count off by one", [](o3m_file_header& h) { h.reloc_count += 1; }, Status::BadFileLength},
      {"reloc count short", [](o3m_file_header& h) { h.reloc_count -= 1; }, Status::BadFileLength},
  };
  for (const Case& c : cases) {
    Blob b = good_blob();
    c.mutate(b.h);
    CAPTURE(std::string(c.what));
    CHECK(header_status(b) == c.want);
  }
  // every minor version below the OS's is fine (every older file still loads: 1.0, 1.1 and 1.2 on an OS of 1.3)
  for (std::uint8_t minor = 0; minor < kOsAbiMinor; ++minor) {
    Blob ok = good_blob();
    ok.h.abi_minor = minor;
    CAPTURE(int{minor});
    CHECK(header_status(ok) == Status::Ok);
  }
}

TEST_CASE("module image: the file's length is the header's, to the byte") {
  const Blob b = good_blob();
  const std::uint32_t size = static_cast<std::uint32_t>(b.file().size());
  CHECK(check_header(b.h, size) == Status::Ok);
  CHECK(check_header(b.h, size - 1) == Status::BadFileLength);
  CHECK(check_header(b.h, size + 1) == Status::BadFileLength);
  CHECK(check_header(b.h, size - 4) == Status::BadFileLength);
  CHECK(check_header(b.h, 63) == Status::TooShort);
  CHECK(check_header(b.h, 0) == Status::TooShort);
}

TEST_CASE("module image: the relocation table is held to order, range and the image") {
  Mem mem;
  {
    Blob b = good_blob();
    b.relocs[3] = b.relocs[2];  // a duplicate
    CHECK(relocate(b, mem) == Status::BadRelocOrder);
  }
  {
    Blob b = good_blob();
    std::swap(b.relocs[4], b.relocs[5]);  // not ascending
    CHECK(relocate(b, mem) == Status::BadRelocOrder);
  }
  {
    Blob b = good_blob();
    b.relocs[0] = 6;  // unaligned
    CHECK(relocate(b, mem) == Status::BadRelocOrder);
  }
  {
    Blob b = good_blob();
    b.relocs[0] = 10;
    CHECK(relocate(b, mem) == Status::BadRelocOrder);
  }
  {
    Blob b = good_blob();
    b.relocs.back() = kImage;  // the first byte past the image
    CHECK(relocate(b, mem) == Status::BadRelocOffset);
  }
  {
    Blob b = good_blob();
    b.relocs.back() = 0xFFFFFFFCu;
    CHECK(relocate(b, mem) == Status::BadRelocOffset);
  }
  {
    Blob b = good_blob();
    b.put(48, kImage + 1);  // an address one past the end of the end
    CHECK(relocate(b, mem) == Status::BadRelocValue);
  }
  {
    Blob b = good_blob();
    b.put(48, 0xFFFFFFFFu);
    CHECK(relocate(b, mem) == Status::BadRelocValue);
  }
  {
    Blob b = good_blob();
    Relocator r(b.h, mem.place());
    const std::uint32_t extra[1] = {200};
    place_image(b, mem.place());
    CHECK(r.apply(b.relocs.data(), static_cast<std::uint32_t>(b.relocs.size())) == Status::Ok);
    CHECK(r.apply(extra, 1) == Status::BadFileLength);  // more entries than the header counted
  }
  {
    Blob b = good_blob();
    place_image(b, mem.place());
    Relocator r(b.h, mem.place());
    CHECK(r.apply(b.relocs.data(), 5) == Status::Ok);
    CHECK(!r.complete());  // fewer than the header counted: the loader notices at the end
    CHECK(r.applied() == 5);
  }
}

TEST_CASE("module image: a relocation that fails leaves its own word as it was") {
  Blob b = good_blob();
  b.put(60, kImage + 8);  // the params pointer: outside
  Mem mem;
  place_image(b, mem.place());
  Relocator r(b.h, mem.place());
  CHECK(r.apply(b.relocs.data(), static_cast<std::uint32_t>(b.relocs.size())) == Status::BadRelocValue);
  std::uint32_t w;
  std::memcpy(&w, mem.cold + (60 - kHot), 4);
  CHECK(w == kImage + 8);
}

TEST_CASE("module image: the descriptor is checked against the header and the image") {
  struct Case {
    const char* what;
    void (*mutate)(Blob&);
  };
  const Case cases[] = {
      {"struct size", [](Blob& b) { b.put(kDesc + 12, 52); }},
      {"abi major differs from the header", [](Blob& b) { b.put(kDesc + 0, 7); }},
      {"abi minor differs from the header", [](Blob& b) { b.put(kDesc + 4, 0); }},
      {"kind differs from the header", [](Blob& b) { b.put(kDesc + 8, O3M_KIND_EFFECT); }},
      {"state differs from the header", [](Blob& b) { b.put(kDesc + 20, 128); }},
      {"param count differs from the header", [](Blob& b) { b.put(kDesc + 24, 1); }},
      {"name at the end of the image", [](Blob& b) { b.put(kDesc + 16, kImage); }},
      {"name empty", [](Blob& b) { b.image[136] = 0; }},
      {"name not printable", [](Blob& b) { b.image[137] = 0x07; }},
      {"name unterminated (to the end of the image)", [](Blob& b) { std::memset(b.image.data() + 160, 'x', 32); b.put(kDesc + 16, kHot + 128); }},
      {"name too long", [](Blob& b) { std::memset(b.image.data() + 136, 'n', 30); b.image[166] = 0; }},
      {"ui table without views", [](Blob& b) { b.put(kDesc + 56, 160); }},
      {"views without a ui table", [](Blob& b) { b.put(kDesc + 60, 2); b.h.view_count = 2; }},
      {"view count differs from the header's", [](Blob& b) { b.put(kDesc + 60, 2); b.put(kDesc + 56, 160); }},
      {"init null", [](Blob& b) { b.put(kDesc + 32, 0); }},
      {"init even", [](Blob& b) { b.put(kDesc + 32, 2); }},
      {"note_on null", [](Blob& b) { b.put(kDesc + 44, 0); }},
      {"note_off even", [](Blob& b) { b.put(kDesc + 48, 8); }},
      {"shutdown is data, not code: the last two bytes of the image fit but it is even", [](Blob& b) { b.put(kDesc + 52, kImage); }},
      {"params unaligned", [](Blob& b) { b.put(kDesc + 28, kHot + 65); }},
      {"params outside", [](Blob& b) { b.put(kDesc + 28, kImage - 8); }},
      {"param name at the end of the image", [](Blob& b) { b.put(96, kImage); }},
      {"param name unterminated", [](Blob& b) { b.put(96, kHot + 144); std::memset(b.image.data() + 176, 'z', 16); }},
      {"param default below min", [](Blob& b) { b.put_param(96, kHot + 112, 0.0f, 1.0f, -0.5f, 0); }},
      {"param default above max", [](Blob& b) { b.put_param(96, kHot + 112, 0.0f, 1.0f, 1.5f, 0); }},
      {"param default NaN", [](Blob& b) { b.put_param(96, kHot + 112, 0.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(), 0); }},
      {"param min NaN", [](Blob& b) { b.put_param(96, kHot + 112, std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.5f, 0); }},
      {"param range inverted", [](Blob& b) { b.put_param(96, kHot + 112, 1.0f, 0.0f, 0.5f, 0); }},
  };
  for (const Case& c : cases) {
    Blob b = good_blob();
    c.mutate(b);
    b.seal();
    Mem mem;
    CAPTURE(std::string(c.what));
    NEED(header_status(b) == Status::Ok);
    NEED(relocate(b, mem) == Status::Ok);  // the words are in range: it is the descriptor's meaning that is wrong
    Desc32 d{};
    CHECK(read_desc(b.h, mem.place(), d) == Status::BadDescriptor);
  }
  // no parameters: params must be null
  Blob none = good_blob();
  none.h.param_count = 0;
  none.put(kDesc + 24, 0);
  none.put(kDesc + 28, 0);
  none.relocs.erase(none.relocs.begin() + 3);  // params pointer
  none.relocs.pop_back();                      // the parameter names
  none.relocs.pop_back();
  none.h.reloc_count = static_cast<std::uint32_t>(none.relocs.size());
  none.seal();
  Mem mem;
  NEED(relocate(none, mem) == Status::Ok);
  Desc32 d{};
  CHECK(read_desc(none.h, mem.place(), d) == Status::Ok);
  none.put(kDesc + 28, kHot + 64);
  none.relocs.insert(none.relocs.begin() + 3, 60);
  none.h.reloc_count = static_cast<std::uint32_t>(none.relocs.size());
  Mem mem2;
  NEED(relocate(none, mem2) == Status::Ok);
  CHECK(read_desc(none.h, mem2.place(), d) == Status::BadDescriptor);  // a table with no parameters
}

TEST_CASE("module image: CRC-32 is the zlib one, in pieces too") {
  // "123456789" -> 0xCBF43926, the standard check value
  CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);
  CHECK(crc32_update(crc32_update(0, "1234", 4), "56789", 5) == 0xCBF43926u);
  CHECK(crc32_update(0, "", 0) == 0u);
  std::uint8_t zeros[32] = {};
  CHECK(crc32_update(0, zeros, 32) == 0x190A55ADu);
}

namespace {

// The blob with a views' table at image offset 160 (the cold part's padding): `count` views, view_draw at hot 8 (Thumb), enter / input / exit as given.
Blob blob_with_views(std::uint32_t count, std::uint32_t draw, std::uint32_t size, std::uint32_t table_at) {
  Blob b = good_blob();
  b.put(kDesc + 56, table_at);
  b.put(kDesc + 60, count);
  b.h.view_count = static_cast<std::uint16_t>(count);
  const Ui32 ui{size, 0, 0, draw, 0};
  std::memcpy(b.image.data() + 160, &ui, sizeof ui);
  b.relocs = {8, 12, 48, 60, 64, 68, 72, 76, 80, 84, 88, 96, 116, 172};  // the table's pointer in the descriptor, and its view_draw word
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.seal();
  return b;
}

}  // namespace

TEST_CASE("module image: a module may declare views (reserved for OS-012b): the table is checked now, nothing calls it") {
  {
    Blob b = blob_with_views(2, 9, sizeof(Ui32), 160);
    Mem mem;
    NEED(header_status(b) == Status::Ok);
    NEED(relocate(b, mem) == Status::Ok);
    Desc32 d{};
    CHECK(read_desc(b.h, mem.place(), d) == Status::Ok);
    CHECK(d.view_count == 2);
    CHECK(d.ui == moved(160));
  }
  struct Case {
    const char* what;
    std::uint32_t draw, size, at;
  };
  const Case bad[] = {{"view_draw null", 0, sizeof(Ui32), 160}, {"view_draw even", 8, sizeof(Ui32), 160}, {"table size wrong", 9, 16, 160},
                      {"table unaligned", 9, sizeof(Ui32), 162}, {"table at the end of the image", 9, sizeof(Ui32), kImage}};
  for (const Case& c : bad) {
    Blob b = blob_with_views(2, c.draw, c.size, c.at);
    Mem mem;
    CAPTURE(std::string(c.what));
    NEED(relocate(b, mem) == Status::Ok);
    Desc32 d{};
    CHECK(read_desc(b.h, mem.place(), d) == Status::BadDescriptor);
  }
}

// ---- ABI 1.12 (OS-INSFX-ABI2 A1, Level C: published module ABI): the descriptor's fourth tail ------------------------------------------------------------

namespace {

using L12 = LayoutFx;

// Relocates `b` and reads its descriptor with every tail; `fx` is pre-filled with junk so that a tail that was not read shows.
Status verdict12(const Blob& b, DescFx32* fx = nullptr, DescImp32* imp = nullptr) {
  Mem mem;
  if (relocate(b, mem) != Status::Ok) {
    return Status::BadRelocValue;
  }
  Desc32 d{};
  DescExt32 ext{};
  DescImp32 i{};
  DescFx32 f{0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu};
  const Status st = read_desc(b.h, mem.place(), d, &ext, &i, &f);
  if (fx != nullptr) {
    *fx = f;
  }
  if (imp != nullptr) {
    *imp = i;
  }
  return st;
}

void put_f(Blob& b, std::uint32_t at, float v) { std::memcpy(b.image.data() + at, &v, 4); }
void kind_to(Blob& b, std::uint32_t kind) {
  b.h.kind = static_cast<std::uint8_t>(kind);
  b.put(L12::desc + 8, kind);
}
// The blob with the word at `at` set to `v` and its relocation dropped (a zero or a wild value that the relocator must not move).
Blob raw_word(Blob b, std::uint32_t at, std::uint32_t v) {
  b = unrelocated(b, at);
  b.put(at, v);
  return b;
}
constexpr std::uint32_t kFxTail = L12::desc + 84;  // render_st; fx_flags +4, presets +8, preset_count +12

}  // namespace

TEST_CASE("module image: ABI 1.12 round trip: the 100-byte descriptor, render_st, WIDENS, two presets and a KEY are read where the layout puts them") {
  Blob b = blob_fx12();
  CHECK(header_status(b) == Status::Ok);
  Mem mem;
  NEED(relocate(b, mem) == Status::Ok);
  Desc32 d{};
  DescExt32 ext{};
  DescImp32 imp{};
  DescFx32 fx{};
  CHECK(read_desc(b.h, mem.place(), d, &ext, &imp, &fx) == Status::Ok);
  CHECK((d.struct_size == 100 && d.abi_minor == 12 && d.kind == O3M_KIND_EFFECT && d.param_count == 3));
  CHECK((ext.pitch == 0 && ext.pages == 0 && ext.page_count == 0 && ext.labels == 0 && imp.importer == 0));
  CHECK(fx.render_st == moved(L12::render_st));
  CHECK(fx.fx_flags == O3M_FX_WIDENS);
  CHECK(fx.presets == moved(L12::presets));
  CHECK(fx.preset_count == 2);
  // the presets are readable through the placement as the OS will read them
  const std::uint8_t* at = resolve(b.h, mem.place(), fx.presets, 2 * sizeof(Preset32));
  NEED(at != nullptr);
  Preset32 p0, p1;
  std::memcpy(&p0, at, sizeof p0);
  std::memcpy(&p1, at + sizeof p0, sizeof p1);
  CHECK((p0.count == 3 && p1.count == 2));
  const std::uint8_t* v0 = resolve(b.h, mem.place(), p0.values, 12);
  NEED(v0 != nullptr);
  float f[3];
  std::memcpy(f, v0, sizeof f);
  CHECK((f[0] == 0.25f && f[1] == 3.0f && f[2] == -2.0f));
  const std::uint8_t* name1 = resolve(b.h, mem.place(), p1.name, 5);
  NEED(name1 != nullptr);
  CHECK(std::string(reinterpret_cast<const char*>(name1)) == "Wide");
  // the descriptor's size is per minor: the 1.12 blob with a 1.11 size, and with minor 11 and the 1.12 size, are refused
  CHECK(verdict12([] { Blob x = blob_fx12(); x.put(L12::desc + 12, 84); return x; }()) == Status::BadDescriptor);
  CHECK(verdict12([] { Blob x = blob_fx12(); x.h.abi_minor = 11; x.put(L12::desc + 4, 11); x.seal(); return x; }()) == Status::BadDescriptor);
}

TEST_CASE("module image: ABI 1.12 shapes that are accepted: stereo-capable, a mono effect with no tail, presets on an instrument, two KEYs, a 15-character preset name") {
  DescFx32 fx{};
  {  // stereo-capable: render_st, no WIDENS
    Blob b = blob_fx12();
    b.put(kFxTail + 4, 0);
    CHECK(verdict12(b, &fx) == Status::Ok);
    CHECK((fx.render_st != 0 && fx.fx_flags == 0));
  }
  {  // no presets, no render_st: a plain mono effect at 1.12 (a KEY parameter may still be there)
    Blob b = raw_word(raw_word(blob_fx12(), kFxTail, 0), kFxTail + 8, 0);
    b.put(kFxTail + 4, 0);
    b.put(kFxTail + 12, 0);
    CHECK(verdict12(b, &fx) == Status::Ok);
    CHECK((fx.render_st == 0 && fx.presets == 0 && fx.preset_count == 0));
  }
  {  // an instrument with presets, no render_st, no KEY
    Blob b = raw_word(blob_fx12(), kFxTail, 0);
    b.put(kFxTail + 4, 0);
    kind_to(b, O3M_KIND_INSTRUMENT);
    b.put(L12::params + 20 + 16, O3M_PARAM_INT);
    CHECK(verdict12(b, &fx) == Status::Ok);
    CHECK(fx.preset_count == 2);
  }
  {  // two KEY parameters (the limit)
    Blob b = blob_fx12();
    put_f(b, L12::params + 8, 16.0f);
    b.put(L12::params + 16, O3M_PARAM_INT | O3M_PARAM_KEY);
    CHECK(verdict12(b) == Status::Ok);
  }
  {  // a preset name of 15 characters, the longest
    Blob b = blob_fx12();
    std::memset(b.image.data() + L12::strings + 16, 'A', 15);
    b.image[L12::strings + 16 + 15] = 0;
    CHECK(verdict12(b) == Status::Ok);
  }
  {  // a preset with one value, the first parameter alone; the rest take their defaults
    Blob b = blob_fx12();
    b.put(L12::presets + 8, 1);
    CHECK(verdict12(b) == Status::Ok);
  }
  {  // a preset that gives every parameter its range's end
    Blob b = blob_fx12();
    put_f(b, L12::values0, 1.0f);
    put_f(b, L12::values0 + 4, 16.0f);
    put_f(b, L12::values0 + 8, -10.0f);
    CHECK(verdict12(b) == Status::Ok);
  }
}

// ABI 1.13 (OS-MODRATE S2, Level C: published module ABI): the RATE bit of fx_flags is an instrument's, from minor 13; the descriptor stays 100 bytes.
TEST_CASE("module image: ABI 1.13: O3M_FX_RATE is accepted on an instrument of 1.13 and refused on an effect, below 1.13 and with WIDENS") {
  const auto instrument = [](std::uint8_t minor, std::uint32_t flags) {
    Blob b = raw_word(blob_fx12(), kFxTail, 0);  // no render_st
    kind_to(b, O3M_KIND_INSTRUMENT);
    b.put(L12::params + 20 + 16, O3M_PARAM_INT);  // no KEY (an effect's)
    b.put(kFxTail + 4, flags);
    b.h.abi_minor = minor;
    b.put(L12::desc + 4, minor);
    b.seal();
    return b;
  };
  DescFx32 fx{};
  CHECK(verdict12(instrument(13, 0), &fx) == Status::Ok);
  CHECK(fx.fx_flags == 0u);
  CHECK(verdict12(instrument(13, O3M_FX_RATE), &fx) == Status::Ok);
  CHECK(fx.fx_flags == static_cast<std::uint32_t>(O3M_FX_RATE));
  CHECK(verdict12(instrument(12, O3M_FX_RATE)) == Status::BadDescriptor);  // the bit did not exist at 1.12
  CHECK(verdict12(instrument(13, O3M_FX_WIDENS)) == Status::BadDescriptor);  // WIDENS is an effect's (and needs render_st)
  CHECK(verdict12(instrument(13, O3M_FX_RATE | O3M_FX_WIDENS)) == Status::BadDescriptor);
  CHECK(verdict12(instrument(13, 4u)) == Status::BadDescriptor);  // every other bit stays 0
  {  // an effect of 1.13 with RATE
    Blob b = blob_fx12();
    b.h.abi_minor = 13;
    b.put(L12::desc + 4, 13);
    b.put(kFxTail + 4, O3M_FX_RATE);
    b.seal();
    CHECK(verdict12(b) == Status::BadDescriptor);
    b.put(kFxTail + 4, O3M_FX_WIDENS);
    b.seal();
    CHECK(verdict12(b) == Status::Ok);  // WIDENS is still an effect's, at 1.13 too
  }
  CHECK(fx_flags_allowed(O3M_KIND_EFFECT, 12) == static_cast<std::uint32_t>(O3M_FX_WIDENS));
  CHECK(fx_flags_allowed(O3M_KIND_INSTRUMENT, 12) == 0u);
  CHECK(fx_flags_allowed(O3M_KIND_INSTRUMENT, 13) == static_cast<std::uint32_t>(O3M_FX_RATE));
  CHECK(fx_flags_allowed(O3M_KIND_APP, 13) == 0u);
}

TEST_CASE("module image: ABI 1.12 refusals: each bad field of the tail is BadDescriptor") {
  struct Case {
    const char* what;
    Blob blob;
  };
  std::vector<Case> bad;
  const auto add = [&bad](const char* what, Blob b) { bad.push_back({what, std::move(b)}); };
  const Blob base = blob_fx12();
  {
    Blob b = base;
    b.put(kFxTail, 12);  // even: not Thumb
    add("render_st not a Thumb address", b);
  }
  add("render_st outside the image", raw_word(base, kFxTail, 0x7000u | 1u));
  {
    Blob b = base;
    kind_to(b, O3M_KIND_INSTRUMENT);
    add("render_st on an instrument", b);
  }
  {
    Blob b = base;
    b.put(kFxTail + 4, 2);
    add("an unknown fx_flags bit (2)", b);
  }
  {
    Blob b = base;
    b.put(kFxTail + 4, 0x80000001u);
    add("an unknown fx_flags bit (the top one)", b);
  }
  {
    Blob b = raw_word(base, kFxTail, 0);
    add("WIDENS without render_st", b);
  }
  {
    Blob b = base;
    put_f(b, L12::params + 20 + 8, 8.0f);
    add("a KEY of 0..8", b);
  }
  {
    Blob b = base;
    put_f(b, L12::params + 20 + 4, 1.0f);
    put_f(b, L12::params + 20 + 12, 1.0f);
    add("a KEY of 1..16", b);
  }
  {
    Blob b = base;
    b.put(L12::params + 20 + 16, O3M_PARAM_KEY);
    add("a KEY that is not INT", b);
  }
  {
    Blob b = raw_word(base, kFxTail, 0);
    b.put(kFxTail + 4, 0);
    kind_to(b, O3M_KIND_INSTRUMENT);
    add("a KEY on an instrument", b);
  }
  {
    Blob b = base;
    for (const std::uint32_t at : {L12::params, L12::params + 40}) {
      put_f(b, at + 4, 0.0f);
      put_f(b, at + 8, 16.0f);
      put_f(b, at + 12, 0.0f);
      b.put(at + 16, O3M_PARAM_INT | O3M_PARAM_KEY);
    }
    add("three KEYs", b);
  }
  add("presets NULL with a count", raw_word(base, kFxTail + 8, 0));
  {
    Blob b = base;
    b.put(kFxTail + 12, 0);
    add("a table with a count of 0", b);
  }
  {
    Blob b = base;
    b.put(kFxTail + 12, O3M_PRESETS_MAX + 1);
    add("33 presets", b);
  }
  {
    Blob b = base;
    b.put(kFxTail + 8, L12::presets + 2);
    add("the preset table unaligned", b);
  }
  {
    Blob b = base;
    b.put(kFxTail + 8, L12::kImageFx - 8);
    add("the preset table past the image", b);
  }
  {
    Blob b = base;
    b.put(L12::presets + 8, 0);
    add("a preset with no values", b);
  }
  {
    Blob b = base;
    b.put(L12::presets + 8, 4);
    add("a preset with more values than parameters", b);
  }
  {
    Blob b = base;
    b.put(L12::presets + 4, L12::values0 + 1);
    add("a value table unaligned", b);
  }
  {
    Blob b = base;
    b.put(L12::presets + 4, L12::kImageFx - 4);
    add("a value table past the image", b);
  }
  {
    Blob b = base;
    put_f(b, L12::values0, 1.5f);
    add("a value above its range", b);
  }
  {
    Blob b = base;
    put_f(b, L12::values0 + 8, -10.5f);
    add("a value below its range", b);
  }
  {
    Blob b = base;
    put_f(b, L12::values0 + 4, std::numeric_limits<float>::quiet_NaN());
    add("a NaN value", b);
  }
  {
    Blob b = base;
    put_f(b, L12::values1 + 4, std::numeric_limits<float>::infinity());
    add("an infinite value", b);
  }
  {
    Blob b = base;
    put_f(b, L12::values1 + 4, 17.0f);
    add("a KEY value past 16", b);
  }
  {
    Blob b = base;
    b.image[L12::strings + 16] = 0;
    add("an empty preset name", b);
  }
  {
    Blob b = base;
    b.image[L12::strings + 16] = 0x01;
    add("a preset name with a control character", b);
  }
  {
    Blob b = base;
    std::memset(b.image.data() + L12::strings + 16, 'A', 16);
    add("a preset name of 16 characters", b);
  }
  add("a preset name outside the image", raw_word(base, L12::presets, 0x7000u));
  {
    Blob b = raw_word(base, L12::desc + 28, 0);  // no parameter table ...
    b.put(L12::desc + 24, 0);                    // ... and no parameters: presets have nothing to set
    b.h.param_count = 0;
    b.seal();
    add("presets for a module with no parameters", b);
  }
  for (const Case& c : bad) {
    CAPTURE(std::string(c.what));
    CHECK(verdict12(c.blob) == Status::BadDescriptor);
  }
}

TEST_CASE("module image: ABI 1.12: an app and an importer carry the four new members zero (presets are an instrument's and an effect's)") {
  for (const std::uint32_t kind : {O3M_KIND_APP, O3M_KIND_IMPORTER}) {
    const auto make = [kind]() { return kind == O3M_KIND_APP ? blob_app(1, 12) : blob_importer(12); };
    const std::uint32_t tail = (kind == O3M_KIND_APP ? kDesc : LayoutImp::desc) + 84;
    DescFx32 fx{};
    DescImp32 imp{};
    CAPTURE(static_cast<unsigned>(kind));
    CHECK(header_status(make()) == Status::Ok);
    CHECK(verdict12(make(), &fx, &imp) == Status::Ok);  // the tail zero: loads
    CHECK((fx.render_st == 0 && fx.fx_flags == 0 && fx.presets == 0 && fx.preset_count == 0));
    for (unsigned w = 0; w < 4; ++w) {  // any one of the four words set: refused
      Blob b = make();
      b.put(tail + 4u * w, w == 0 ? 9u : w == 1 ? O3M_FX_WIDENS : w == 2 ? 200u : 1u);
      CAPTURE(w);
      CHECK(verdict12(b) == Status::BadDescriptor);
    }
  }
}

TEST_CASE("module image: every module of 1.0 to 1.11 still reads exactly as before: the 1.12 tail is not read below 1.12, whatever bytes follow the descriptor") {
  struct Case {
    const char* what;
    Blob blob;
    std::uint32_t size, minor, kind;
    std::uint32_t junk_at;  // the image offset just past the descriptor's last member, where a 1.12 tail would be
  };
  Blob b0 = blob_minor(0);
  Blob b10 = blob18();
  b10.h.abi_minor = 10;
  b10.put(Layout18::desc + 4, 10);
  b10.seal();
  Blob app11 = blob_app(1, 11);
  Blob imp11 = blob_importer();
  const Case cases[] = {{"1.0", b0, 64, 0, O3M_KIND_INSTRUMENT, kDesc + 64},
                        {"1.7", good_blob(), 64, 7, O3M_KIND_INSTRUMENT, kDesc + 64},
                        {"1.8", blob18(), 80, 8, O3M_KIND_INSTRUMENT, Layout18::desc + 80},
                        {"1.10", b10, 80, 10, O3M_KIND_INSTRUMENT, Layout18::desc + 80},
                        {"1.11 app", app11, 84, 11, O3M_KIND_APP, kDesc + 84},
                        {"1.11 importer", imp11, 84, 11, O3M_KIND_IMPORTER, LayoutImp::desc + 84}};
  for (const Case& c : cases) {
    CAPTURE(std::string(c.what));
    Blob b = c.blob;
    CHECK(header_status(b) == Status::Ok);
    CHECK((b.get(c.junk_at - c.size + 12) == c.size && b.get(c.junk_at - c.size + 4) == c.minor));  // the descriptor's struct_size and minor, as published
    DescFx32 fx{};
    DescImp32 imp{};
    CHECK(verdict12(b, &fx, &imp) == Status::Ok);
    CHECK((fx.render_st == 0 && fx.fx_flags == 0 && fx.presets == 0 && fx.preset_count == 0));  // (the junk the verdict pre-fills is overwritten with zero, not with what follows)
    // below 1.11 what follows the descriptor is the module's parameter table (non-zero words that, taken for a 1.12 tail, would be refused); a 1.11 file has cold padding there: fill it with junk,
    // which must not matter (the 1.11 importer's table starts 12 bytes past its descriptor, so only those 12 bytes are padding)
    if (c.minor >= 11u) {
      std::memset(b.image.data() + c.junk_at, 0xA5, c.kind == O3M_KIND_IMPORTER ? 12u : 16u);
      b.seal();
      DescFx32 fx2{};
      CHECK(verdict12(b, &fx2) == Status::Ok);
      CHECK((fx2.render_st == 0 && fx2.fx_flags == 0 && fx2.presets == 0 && fx2.preset_count == 0));
    }
  }
  // a 1.11 or older module's KEY bit (32) in a parameter's flags is not honoured and not refused: an instrument of 1.7 with it loads
  Blob key17 = good_blob();
  key17.put(116 + 16, O3M_PARAM_KEY);  // the second parameter's flags
  key17.seal();
  CHECK(verdict12(key17) == Status::Ok);
  Blob key17_int = good_blob();
  key17_int.put(116 + 16, O3M_PARAM_KEY | O3M_PARAM_INT);
  key17_int.seal();
  CHECK(verdict12(key17_int) == Status::Ok);
}
