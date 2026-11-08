// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S1: the .ptfx header parser (module/ptfx_format.h) on synthetic headers built byte by byte: one case per refusal, clamp and sanitising
// rule of docs/specs/OS-CEPTFX.md 3.1, and the list strings at the payload's edge. Host only.
#include <doctest/doctest.h>

#include <o3os/module/ptfx_format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace o3os::module;

namespace {

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
  v[at] = static_cast<std::uint8_t>(x);
  v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  put16(v, at, static_cast<std::uint16_t>(x));
  put16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}
void putf(std::vector<std::uint8_t>& v, std::size_t at, float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  put32(v, at, u);
}

// A good file: header + 64 bytes of payload, one 'f' parameter "Gain", entries 0, 8 and 16 (Thumb bit set on the first).
std::vector<std::uint8_t> good() {
  std::vector<std::uint8_t> v(kPtfxHeaderSize + 64, 0);
  put32(v, 0, kPtfxMagic);
  put16(v, 4, 1);
  put16(v, 6, kPtfxFlagStereo);
  std::memcpy(&v[8], "Test Plugin", 11);
  v[56] = 1;
  put32(v, 60, 1000);
  put32(v, 64, kPtfxHeaderSize);
  put32(v, 68, 64);
  put32(v, 72, 8);
  put32(v, 76, 1);
  put32(v, 80, 8);
  put32(v, 84, 16);
  std::memcpy(&v[88], "Gain", 4);
  putf(v, 88 + 8, 0.0f);
  putf(v, 88 + 12, 10.0f);
  putf(v, 88 + 16, 5.0f);
  putf(v, 88 + 20, 0.5f);
  std::memcpy(&v[88 + 24], "dB", 2);
  v[88 + 30] = 'f';
  return v;
}

PtfxStatus parse(const std::vector<std::uint8_t>& v, PtfxInfo& info) { return ptfx_parse_header(v.data(), v.size(), v.size(), info); }

}  // namespace

TEST_CASE("ptfx_format: a good file parses, entries lose the Thumb bit") {
  PtfxInfo info;
  CHECK(parse(good(), info) == PtfxStatus::Ok);
  CHECK(std::strcmp(info.name, "Test Plugin") == 0);
  CHECK(info.flags == kPtfxFlagStereo);
  CHECK(info.num_params == 1);
  CHECK(info.init_offset == 0);
  CHECK(info.set_param_offset == 8);
  CHECK(info.process_block_offset == 16);
  CHECK(info.scratch_size == 1000);
  CHECK(std::strcmp(info.params[0].label, "Gain") == 0);
  CHECK(std::strcmp(info.params[0].unit, "dB") == 0);
  CHECK(info.params[0].type == 'f');
  CHECK(info.params[0].editable);
  CHECK(info.params[0].def == doctest::Approx(5.0f));
}

TEST_CASE("ptfx_format: refusals") {
  PtfxInfo info;
  auto v = good();
  CHECK(ptfx_parse_header(v.data(), 100, v.size(), info) == PtfxStatus::NotPtfx);  // header cut short
  CHECK(ptfx_parse_header(v.data(), v.size(), 100, info) == PtfxStatus::NotPtfx);  // file shorter than 472
  v = good(); v[0] = 'X';
  CHECK(parse(v, info) == PtfxStatus::NotPtfx);
  v = good(); put16(v, 4, 2);
  CHECK(parse(v, info) == PtfxStatus::VersionUnsupported);
  CHECK(info.version == 2);
  v = good(); put16(v, 6, kPtfxFlagImxOnly);
  CHECK(parse(v, info) == PtfxStatus::NotForThisTracker);
  v = good(); put16(v, 6, 0x20);
  CHECK(parse(v, info) == PtfxStatus::NeedsNewerOs);
  v = good(); put16(v, 6, 0x8000 | kPtfxFlagStereo);
  CHECK(parse(v, info) == PtfxStatus::NeedsNewerOs);
  v = good(); put32(v, 64, kPtfxHeaderSize - 4);
  CHECK(parse(v, info) == PtfxStatus::FileDamaged);
  v = good(); put32(v, 68, 65);  // beyond the file
  CHECK(parse(v, info) == PtfxStatus::FileDamaged);
  v = good(); put32(v, 64, 0xFFFFFFF0u); put32(v, 68, 0x20);  // 32-bit sum would wrap
  CHECK(parse(v, info) == PtfxStatus::FileDamaged);
  v = good(); put32(v, 68, 0);
  CHECK(parse(v, info) == PtfxStatus::FileDamaged);
  v = good(); put32(v, 76, 64);  // entry outside the code
  CHECK(parse(v, info) == PtfxStatus::FileDamaged);
  v = good(); put32(v, 80, 17);  // equal to process_block once bit 0 is masked
  put32(v, 84, 16);
  CHECK(parse(v, info) == PtfxStatus::FileDamaged);
  v = good(); put32(v, 76, 0); put32(v, 80, 0);  // a missing function is written as 0
  CHECK(parse(v, info) == PtfxStatus::FileDamaged);
}

TEST_CASE("ptfx_format: size limit is align4(code) + bss at 32 KiB") {
  PtfxInfo info;
  std::vector<std::uint8_t> v(kPtfxHeaderSize + 32767, 0);
  auto g = good();
  std::memcpy(v.data(), g.data(), kPtfxHeaderSize);
  put32(v, 68, 32767);  // align4 = 32768
  put32(v, 72, 0);
  CHECK(parse(v, info) == PtfxStatus::Ok);
  put32(v, 72, 1);
  CHECK(parse(v, info) == PtfxStatus::TooBig);
  put32(v, 72, 0xFFFFFFFFu);
  CHECK(parse(v, info) == PtfxStatus::TooBig);
}

TEST_CASE("ptfx_format: tolerated and sanitised fields") {
  PtfxInfo info;
  auto v = good();
  v[56] = 200;  // clamped to 12
  put32(v, 60, 0x7FFFFFFFu);
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.num_params == 12);
  CHECK(info.scratch_size == kPtfxMaxScratch);

  v = good(); v[88 + 30] = 'z';  // unknown display type is a float
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].type == 'f');

  v = good(); v[8] = 0x01; v[9] = 0xFF; v[88] = 0x07;  // non-printables show as '?'
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.name[0] == '?');
  CHECK(info.name[1] == '?');
  CHECK(info.params[0].label[0] == '?');

  v = good(); putf(v, 88 + 16, 99.0f);  // default above the range
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].def == doctest::Approx(10.0f));
  putf(v, 88 + 16, -3.0f);
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].def == doctest::Approx(0.0f));

  v = good(); putf(v, 88 + 12, -1.0f);  // reversed range
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].type == 'd');
  CHECK_FALSE(info.params[0].editable);
  CHECK(info.params[0].min == doctest::Approx(0.0f));
  CHECK(info.params[0].max == doctest::Approx(0.0f));

  v = good(); putf(v, 88 + 8, std::numeric_limits<float>::quiet_NaN());  // non-finite
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].type == 'd');
  CHECK_FALSE(info.params[0].editable);
  v = good(); putf(v, 88 + 12, std::numeric_limits<float>::infinity());
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK_FALSE(info.params[0].editable);

  v = good(); putf(v, 88 + 20, 0.0f);  // a zero step is not a step
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].step == doctest::Approx(1.0f));
  CHECK(info.params[0].editable);
  v = good(); putf(v, 88 + 20, -0.25f);  // neither is a negative one
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].step == doctest::Approx(1.0f));
  CHECK(info.params[0].editable);
  v = good(); putf(v, 88 + 20, std::numeric_limits<float>::quiet_NaN());  // nor a non-finite one
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].step == doctest::Approx(1.0f));
}

TEST_CASE("ptfx_format: list strings are read only inside the payload and capped at 16") {
  PtfxInfo info;
  auto v = good();
  v[88 + 30] = 'l';
  put16(v, 88 + 24, 4);  // list at payload offset 4
  v[88 + 26] = 3;
  CHECK(parse(v, info) == PtfxStatus::Ok);
  const PtfxParam& p = info.params[0];
  CHECK(p.type == 'l');
  CHECK(p.list_count == 3);

  const std::uint8_t pay[] = {0, 0, 0, 0, 'A', 'B', 0, 'C', 0, 'D', 'E', 'F'};  // third item has no NUL
  CHECK(ptfx_list_count(pay, sizeof pay, p) == 2);
  char item[11];
  CHECK(ptfx_list_item(pay, sizeof pay, p, 0, item));
  CHECK(std::strcmp(item, "AB") == 0);
  CHECK(ptfx_list_item(pay, sizeof pay, p, 1, item));
  CHECK(std::strcmp(item, "C") == 0);
  CHECK_FALSE(ptfx_list_item(pay, sizeof pay, p, 2, item));

  const std::uint8_t pay2[] = {0, 0, 0, 0, 'A', 0};  // the NUL is the last payload byte: still inside
  CHECK(ptfx_list_count(pay2, sizeof pay2, p) == 1);
  const std::uint8_t pay3[] = {0, 0, 0, 0};  // list starts at the payload's end: no valid item
  CHECK(ptfx_list_count(pay3, sizeof pay3, p) == 0);

  v[88 + 26] = 200;  // capped at 16
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(info.params[0].list_count == 16);
  std::uint8_t big[4 + 40];
  std::memset(big, 0, sizeof big);
  for (int i = 0; i < 20; ++i) {
    big[4 + 2 * i] = 'x';
  }
  CHECK(ptfx_list_count(big, sizeof big, info.params[0]) == 16);

  const std::uint8_t longitem[] = {0, 0, 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'A', 'B', 0};  // 12 chars: cut to 10
  v[88 + 26] = 1;
  CHECK(parse(v, info) == PtfxStatus::Ok);
  CHECK(ptfx_list_item(longitem, sizeof longitem, info.params[0], 0, item));
  CHECK(std::strlen(item) == 10);
}

// OS-CEPTFX S0: ctest `ptfx_fixtures` runs this case and compares the dump below, field by field, with what
// tools/ptfx_inspect.py parses out of the same built files, so the Python inspector and this parser cannot drift
// apart. Tab-separated key=value lines, so names with spaces survive. Skipped (a message, no failure) when
// build-fixtures/ptfx is not built; the ctest is the one that insists on its presence.
TEST_CASE("ptfx_format: dump the built fixtures (ctest ptfx_fixtures compares this against ptfx_inspect.py)") {
  const std::filesystem::path dir = std::string(O3_BUILD_FIXTURES_DIR) + "/ptfx";
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    MESSAGE("build-fixtures/ptfx is not built: nothing to dump");
    return;
  }
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && e.path().extension() == ".ptfx") {
      files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    const std::vector<std::uint8_t> v((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PtfxInfo info;
    const PtfxStatus st = ptfx_parse_header(v.data(), v.size(), v.size(), info);
    std::printf("PTFXDUMP\tfile=%s\tstatus=%d\tversion=%u\tflags=%u\tname=%s\tauthor=%s\tnum_params=%u\tscratch=%u\t"
                "code_offset=%u\tcode_size=%u\tbss=%u\tfilesize=%zu\tinit=%u\tset_param=%u\tprocess_block=%u\n",
                f.filename().string().c_str(), static_cast<int>(st), info.version, info.flags, info.name, info.author,
                static_cast<unsigned>(info.num_params), info.scratch_size, info.code_offset, info.code_size,
                info.bss_size, v.size(), info.init_offset, info.set_param_offset, info.process_block_offset);
    if (st == PtfxStatus::Ok) {
      const std::uint8_t* payload = v.data() + info.code_offset;
      for (std::uint32_t i = 0; i < info.num_params; ++i) {
        const PtfxParam& p = info.params[i];
        const auto bits = [](float x) {
          std::uint32_t u;
          std::memcpy(&u, &x, sizeof u);
          return u;
        };
        std::printf("PTFXDUMP\tparam=%u\tlabel=%s\ttype=%c\tmin=%08x\tmax=%08x\tdef=%08x\tstep=%08x\tunit=%s\teditable=%d\t"
                    "list_offset=%u\tlist_count=%u\titems=",
                    i, p.label, p.type, bits(p.min), bits(p.max), bits(p.def), bits(p.step),
                    p.type == 'l' ? "" : p.unit, static_cast<int>(p.editable), p.list_offset, p.list_count);
        const std::uint32_t n = ptfx_list_count(payload, info.code_size, p);
        for (std::uint32_t k = 0; k < n; ++k) {
          char item[11];
          if (ptfx_list_item(payload, info.code_size, p, k, item)) {
            std::printf("%s%s", k == 0 ? "" : "|", item);
          }
        }
        std::printf("\n");
      }
    }
    std::printf("PTFXDUMP\tend\n");
  }
}
