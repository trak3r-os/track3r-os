// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MODPRESET P1, the module preset file (docs/formats/o3-preset.md, formats/preset.h): the bytes of the reference preset against a literal
// written from the spec and against the hex fixture shared with tools/o3preset.py, write and read round trips (0, 1 and 32 entries, every float
// bit pattern the writer may be given), the writer's own refusals, the damage loops (a cut at every byte, a flip of every bit, with the CRC right
// and wrong, random damage) and the corpus: the good and damaged files that tests/o3preset_test.py (ctest preset_python, the fixture preset_corpus)
// writes with the outcome its independent reader gave, which this reader must give too. The reader's refusals are that corpus, not a table here.
// Host only (the fixture and the corpus are files).
#include <doctest/doctest.h>

#include <o3os/formats/crc32.h>
#include <o3os/formats/preset.h>
#include <o3os/module/loader.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support/need.h"

using namespace o3os;
namespace fp = o3os::formats::preset;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr std::uint32_t id4(char a, char b, char c, char d) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}
constexpr std::uint32_t kMpst = id4('M', 'P', 'S', 'T');

void put32(Bytes& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<std::uint8_t>(v >> s));
  }
}
void put16(Bytes& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v >> 8));
  b.push_back(static_cast<std::uint8_t>(v));
}
void putbits(Bytes& b, std::uint32_t bits) { put32(b, bits); }
std::uint32_t bits_of(float f) {
  std::uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  return bits;
}
float float_of(std::uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}
void putstr(Bytes& b, const std::string& s) {
  b.push_back(static_cast<std::uint8_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
}

// A file written from the spec, not by the writer: FORM, the chunks as given (padded to even), then the CRC chunk. `crc_version` and `crc_tail`
// make a newer-minor CRC chunk.
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
  f.insert(f.end(), {'O', '3', 'M', 'P'});
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

struct Ent {
  std::string name;
  std::uint32_t bits;
};

// An MPST payload: version, stem, kind, module version, count, the entries (the count as given, which may differ from the entries) and a tail.
Bytes mpst(const std::string& stem, std::uint8_t kind, std::uint32_t modver, const std::vector<Ent>& ents, int count = -1, std::uint16_t version = 0x0100,
           const Bytes& tail = {}) {
  Bytes p;
  put16(p, version);
  putstr(p, stem);
  p.push_back(kind);
  put32(p, modver);
  p.push_back(static_cast<std::uint8_t>(count < 0 ? ents.size() : static_cast<std::size_t>(count)));
  for (const Ent& e : ents) {
    putstr(p, e.name);
    putbits(p, e.bits);
  }
  p.insert(p.end(), tail.begin(), tail.end());
  return p;
}

std::vector<Ent> ref_entries() { return {{"Cutoff", bits_of(0.5f)}, {"Resonance", bits_of(0.25f)}, {"Env Mod", bits_of(1.0f)}, {"Decay", bits_of(2.5f)}}; }

// The reference file, from the spec: tb303, instrument, version 0x00010000, four entries.
Bytes ref_bytes() { return build({{kMpst, mpst("tb303", 1, 0x00010000u, ref_entries())}}); }

fp::Preset ref_preset() {
  fp::Preset p;
  std::strcpy(p.module, "tb303");
  p.kind = fp::kKindInstrument;
  p.module_version = 0x00010000u;
  const auto ents = ref_entries();
  p.count = static_cast<std::uint8_t>(ents.size());
  for (std::size_t i = 0; i < ents.size(); ++i) {
    std::strcpy(p.entries[i].name, ents[i].name.c_str());
    p.entries[i].value = float_of(ents[i].bits);
  }
  return p;
}

Bytes written(const fp::Preset& p) {
  Bytes out(fp::kMaxFileBytes);
  const std::size_t n = fp::write(p, out.data(), out.size());
  out.resize(n);
  return out;
}

// A copy of `b[0, n)` in its own allocation of exactly n bytes, so that a read past n is a sanitizer error.
std::unique_ptr<std::uint8_t[]> exact(const Bytes& b, std::size_t n) {
  std::unique_ptr<std::uint8_t[]> p(new std::uint8_t[n != 0 ? n : 1]);
  if (n != 0) {
    std::memcpy(p.get(), b.data(), n);
  }
  return p;
}

struct Got {
  fp::Error error;
  fp::Report report;
  fp::Preset preset;
};

Got load(const Bytes& b) {
  Got g;
  const auto buf = exact(b, b.size());
  g.error = fp::read(buf.get(), b.size(), g.preset, g.report);
  return g;
}

// Equal as values: the strings and the bit patterns of the floats (a NaN or -0.0 is not "equal" to anything under ==).
bool same(const fp::Preset& a, const fp::Preset& b) {
  if (std::strcmp(a.module, b.module) != 0 || a.kind != b.kind || a.module_version != b.module_version || a.count != b.count) {
    return false;
  }
  for (unsigned i = 0; i < a.count; ++i) {
    if (std::strcmp(a.entries[i].name, b.entries[i].name) != 0 || bits_of(a.entries[i].value) != bits_of(b.entries[i].value)) {
      return false;
    }
  }
  return true;
}

bool is_zero(const fp::Preset& p) {
  const fp::Preset zero{};
  return std::memcmp(&p, &zero, sizeof p) == 0;
}

// The hex text of tests/data/preset_basic.hex: hex digit pairs, whitespace and lines starting with # ignored.
Bytes read_hex(const char* path) {
  Bytes out;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line[0] == '#') {
      continue;
    }
    std::istringstream words(line);
    std::string w;
    while (words >> w) {
      out.push_back(static_cast<std::uint8_t>(std::stoul(w, nullptr, 16)));
    }
  }
  return out;
}

std::string name_of(unsigned i) {
  std::string s = "P";
  s += static_cast<char>('A' + i / 26);
  s += static_cast<char>('a' + i % 26);
  return s;
}

fp::Preset full_preset(unsigned count, bool long_names) {
  fp::Preset p;
  std::strcpy(p.module, long_names ? "0123456789012345678901234567890" : "fm");  // 31 characters
  p.kind = fp::kKindEffect;
  p.module_version = 0xFFFFFFFFu;
  p.count = static_cast<std::uint8_t>(count);
  for (unsigned i = 0; i < count; ++i) {
    const std::string n = long_names ? std::string(12, 'p') + name_of(i) : name_of(i);  // 15 characters, all different
    std::strcpy(p.entries[i].name, n.c_str());
    p.entries[i].value = float_of(0x3F800000u + i * 0x1234u);
  }
  return p;
}

// The CRC chunk's value made right again for the bytes before it (the last 14 bytes are the CRC chunk of a 1.0 file): a damage that was made to
// the body then gets past the CRC and reaches the MPST parser, which a damaged body otherwise never does.
void refresh_crc(Bytes& f) {
  if (f.size() < 26) {
    return;
  }
  const std::uint32_t crc = formats::crc32(f.data(), f.size() - 14);
  for (unsigned i = 0; i < 4; ++i) {
    f[f.size() - 4 + i] = static_cast<std::uint8_t>(crc >> (24 - 8 * i));
  }
}

// The outcome as the corpus names it (tools/o3preset.py: ok, bad_magic, truncated, bad_CRC, newer_major_version, malformed, too_big).
std::string outcome(fp::Error e) {
  if (e == fp::Error::None) {
    return "ok";
  }
  std::string s = fp::error_name(e);
  for (char& c : s) {
    if (c == ' ') {
      c = '_';
    }
  }
  return s;
}

std::string hex_of(const std::string& text) {
  std::string out;
  char two[3];
  for (const char c : text) {
    std::snprintf(two, sizeof two, "%02x", static_cast<unsigned>(static_cast<unsigned char>(c)));
    out += two;
  }
  return out;
}

// What the corpus says of a file that reads: module:kind:module version:duplicates:unknown chunks:newer minor, then name:value bits for each entry.
std::vector<std::string> summary(const Got& g) {
  char buf[96];
  std::snprintf(buf, sizeof buf, ":%u:%u:%u:%u:%d", static_cast<unsigned>(g.preset.kind), static_cast<unsigned>(g.preset.module_version),
                static_cast<unsigned>(g.report.duplicates), static_cast<unsigned>(g.report.unknown_chunks), g.report.newer_minor ? 1 : 0);
  std::vector<std::string> out = {hex_of(g.preset.module) + buf};
  for (unsigned i = 0; i < g.preset.count; ++i) {
    std::snprintf(buf, sizeof buf, ":%08x", static_cast<unsigned>(bits_of(g.preset.entries[i].value)));
    out.push_back(hex_of(g.preset.entries[i].name) + buf);
  }
  return out;
}

Bytes from_hex(const std::string& text) {
  Bytes out;
  if (text != "-") {
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
      out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
  }
  return out;
}

}  // namespace

TEST_CASE("preset: the reference file is the bytes of the spec and of the shared hex fixture") {
  const Bytes spec = ref_bytes();
  CHECK(spec.size() == 96);
  CHECK(written(ref_preset()) == spec);
#ifdef O3_PRESET_BASIC_HEX
  const Bytes hex = read_hex(O3_PRESET_BASIC_HEX);
  CHECK(hex.size() == 96);
  CHECK(hex == spec);
  CHECK(written(ref_preset()) == hex);
  const Got g = load(hex);
  CHECK(g.error == fp::Error::None);
  CHECK(same(g.preset, ref_preset()));
#else
  FAIL("O3_PRESET_BASIC_HEX is not defined: tests/CMakeLists.txt gives o3os_tests the fixture's path");
#endif
  // The stem and the CRC at known places: the first byte of MPST's payload is the version, the file ends with the CRC chunk of zlib's CRC-32.
  CHECK(std::memcmp(spec.data() + 12, "MPST", 4) == 0);
  CHECK(spec[20] == 0x01);
  CHECK(spec[21] == 0x00);
  CHECK(std::memcmp(spec.data() + spec.size() - 14, "CRC ", 4) == 0);
  CHECK(formats::crc32(spec.data(), spec.size() - 14) == ((static_cast<std::uint32_t>(spec[92]) << 24) | (static_cast<std::uint32_t>(spec[93]) << 16) |
                                                          (static_cast<std::uint32_t>(spec[94]) << 8) | spec[95]));
  CHECK(formats::crc32(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0xCBF43926u);
  CHECK(fp::sniff(spec.data(), spec.size()));
  CHECK_FALSE(fp::sniff(spec.data(), 11));
  CHECK_FALSE(fp::sniff(nullptr, 20));
  Bytes other = spec;
  other[8] = 'X';
  CHECK_FALSE(fp::sniff(other.data(), other.size()));
}

TEST_CASE("preset: reading the reference gives every field, and nothing is reported") {
  const Got g = load(ref_bytes());
  CHECK(g.error == fp::Error::None);
  CHECK(g.report.error == fp::Error::None);
  CHECK(std::string(g.report.detail).empty());
  CHECK(g.report.duplicates == 0);
  CHECK(g.report.unknown_chunks == 0);
  CHECK_FALSE(g.report.newer_minor);
  CHECK(std::string(g.preset.module) == "tb303");
  CHECK(g.preset.kind == fp::kKindInstrument);
  CHECK(g.preset.module_version == 0x00010000u);
  NEED(g.preset.count == 4);
  CHECK(std::string(g.preset.entries[0].name) == "Cutoff");
  CHECK(g.preset.entries[0].value == 0.5f);
  CHECK(std::string(g.preset.entries[3].name) == "Decay");
  CHECK(g.preset.entries[3].value == 2.5f);
  CHECK(g.preset.entries[4].name[0] == '\0');
}

TEST_CASE("preset: write then read gives the same preset for 0, 1 and 32 entries") {
  for (const unsigned count : {0u, 1u, 2u, 31u, 32u}) {
    for (const bool long_names : {false, true}) {
      const fp::Preset p = full_preset(count, long_names);
      CHECK(fp::valid(p));
      const Bytes b = written(p);
      NEED(!b.empty());
      CHECK(b == build({{kMpst, mpst(p.module, p.kind, p.module_version, [&] {
                                       std::vector<Ent> e;
                                       for (unsigned i = 0; i < p.count; ++i) {
                                         e.push_back({p.entries[i].name, bits_of(p.entries[i].value)});
                                       }
                                       return e;
                                     }())}}));
      const Got g = load(b);
      CHECK(g.error == fp::Error::None);
      CHECK(same(g.preset, p));
      CHECK(fp::sniff(b.data(), b.size()));
      CHECK(written(g.preset) == b);  // and writing what was read gives the same bytes
    }
  }
}

TEST_CASE("preset: the largest file is 714 bytes and the smallest 44") {
  CHECK(written(full_preset(32, true)).size() == 714);
  CHECK(written(full_preset(32, true)).size() <= fp::kMaxFileBytes);
  fp::Preset p;
  std::strcpy(p.module, "a");
  p.kind = fp::kKindInstrument;
  CHECK(written(p).size() == 12 + 8 + 10 + 14);  // an empty preset, a one-character stem: 2+1+1+1+4+1 = 10
}

TEST_CASE("preset: every float bit pattern the writer may be given comes back bit for bit") {
  const std::uint32_t patterns[] = {0x00000000u, 0x80000000u,                        // +0, -0
                                    0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,  // denormals
                                    0x00800000u, 0x80800000u,                        // smallest normals
                                    0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,  // 1, -1, +- the largest finite
                                    0x7F000000u, 0x3DCCCCCDu, 0x42C80000u, 0x40490FDBu};
  fp::Preset p;
  std::strcpy(p.module, "chip");
  p.kind = fp::kKindInstrument;
  p.count = 16;
  for (unsigned i = 0; i < 16; ++i) {
    std::strcpy(p.entries[i].name, name_of(i).c_str());
    p.entries[i].value = float_of(patterns[i]);
  }
  const Got g = load(written(p));
  CHECK(g.error == fp::Error::None);
  CHECK(same(g.preset, p));
  CHECK(bits_of(g.preset.entries[1].value) == 0x80000000u);  // -0.0 stays -0.0
  CHECK(bits_of(g.preset.entries[2].value) == 0x00000001u);  // the smallest denormal stays
  // A sweep of finite patterns, 32 at a time (a fixed LCG, so the same ones every run).
  std::uint32_t s = 0x12345678u;
  for (unsigned round = 0; round < 2000; ++round) {
    fp::Preset q = full_preset(32, false);
    for (unsigned i = 0; i < 32; ++i) {
      s = s * 1664525u + 1013904223u;
      std::uint32_t bits = s;
      if ((bits & 0x7F800000u) == 0x7F800000u) {
        bits &= ~0x00800000u;  // not an infinity or a NaN
      }
      q.entries[i].value = float_of(bits);
    }
    const Bytes b = written(q);
    NEED(!b.empty());
    const Got r = load(b);
    NEED(r.error == fp::Error::None);
    NEED(same(r.preset, q));
  }
}

TEST_CASE("preset: a module's own stem rule and this layer's are the same") {
  // formats may not include module, so preset.cpp has its own copy of module::is_stem: every one-byte stem, the dots, the long and the awkward.
  for (unsigned c = 0; c < 256; ++c) {
    const char s[2] = {static_cast<char>(c), '\0'};
    CHECK(fp::is_stem(s) == module::is_stem(s));
    const char t[4] = {'a', static_cast<char>(c), 'b', '\0'};
    CHECK(fp::is_stem(t) == module::is_stem(t));
  }
  for (const char* s : {"", ".", "..", "...", "a.b", "tb303", "my module", "0123456789012345678901234567890", "01234567890123456789012345678901",
                        "a/b", "a\\b", "CON:", "x*y", "x?y", "q\"q", "<a>", "a|b", "\x7f", "\xc3\xa9"}) {
    CHECK(fp::is_stem(s) == module::is_stem(s));
  }
  CHECK_FALSE(fp::is_stem(nullptr));
  CHECK(fp::kStemMax == module::kStemMax);
}

TEST_CASE("preset: the writer refuses what the reader would not give back") {
  const fp::Preset good = ref_preset();
  Bytes out(fp::kMaxFileBytes);
  CHECK(fp::write(good, out.data(), out.size()) == 96);
  CHECK(fp::write(good, nullptr, 1024) == 0);
  const auto refused = [&](const char* why, auto&& edit) {
    fp::Preset p = ref_preset();
    edit(p);
    CAPTURE(why);
    CHECK_FALSE(fp::valid(p));
    CHECK(fp::write(p, out.data(), out.size()) == 0);
  };
  refused("empty stem", [](fp::Preset& p) { p.module[0] = '\0'; });
  refused("dot stem", [](fp::Preset& p) { std::strcpy(p.module, ".."); });
  refused("slash in stem", [](fp::Preset& p) { std::strcpy(p.module, "a/b"); });
  refused("stem without a NUL", [](fp::Preset& p) { std::memset(p.module, 'a', sizeof p.module); });
  refused("kind 0", [](fp::Preset& p) { p.kind = 0; });
  refused("kind 3 (importer)", [](fp::Preset& p) { p.kind = 3; });
  refused("count 33", [](fp::Preset& p) { p.count = 33; });
  refused("count 255", [](fp::Preset& p) { p.count = 255; });
  refused("empty name", [](fp::Preset& p) { p.entries[1].name[0] = '\0'; });
  refused("name without a NUL (16 bytes)", [](fp::Preset& p) { std::memset(p.entries[1].name, 'x', sizeof p.entries[1].name); });
  refused("control character in a name", [](fp::Preset& p) { p.entries[1].name[2] = '\x1f'; });
  refused("DEL in a name", [](fp::Preset& p) { p.entries[1].name[2] = '\x7f'; });
  refused("non-ASCII in a name", [](fp::Preset& p) { p.entries[1].name[2] = static_cast<char>(0xC3); });
  refused("NaN", [](fp::Preset& p) { p.entries[2].value = float_of(0x7FC00000u); });
  refused("signalling NaN", [](fp::Preset& p) { p.entries[2].value = float_of(0x7F800001u); });
  refused("+infinity", [](fp::Preset& p) { p.entries[2].value = float_of(0x7F800000u); });
  refused("-infinity", [](fp::Preset& p) { p.entries[2].value = float_of(0xFF800000u); });
  // A name may repeat (a card module may have two parameters of one name): written in order, read back in order.
  fp::Preset twice = ref_preset();
  std::strcpy(twice.entries[3].name, "Cutoff");
  CHECK(fp::valid(twice));
  const Got again = load(written(twice));
  CHECK(again.error == fp::Error::None);
  CHECK(again.report.duplicates == 1);
  CHECK(same(again.preset, twice));
  fp::Preset all_same = full_preset(32, false);
  for (unsigned i = 0; i < 32; ++i) {
    std::strcpy(all_same.entries[i].name, "x");
  }
  const Got same32 = load(written(all_same));
  CHECK(same32.error == fp::Error::None);
  CHECK(same32.report.duplicates == 31);
  CHECK(same(same32.preset, all_same));
  // Entries past the count are not looked at.
  fp::Preset past = ref_preset();
  past.entries[7].value = float_of(0x7FC00000u);
  CHECK(fp::valid(past));
  // The capacity: never one byte more than `cap`, 0 for any smaller one.
  for (std::size_t cap = 0; cap < 96; ++cap) {
    const auto buf = exact(Bytes(cap, 0xAA), cap);
    CHECK(fp::write(good, buf.get(), cap) == 0);
  }
  const auto exactcap = exact(Bytes(96, 0xAA), 96);
  CHECK(fp::write(good, exactcap.get(), 96) == 96);
  CHECK(std::memcmp(exactcap.get(), ref_bytes().data(), 96) == 0);
}

TEST_CASE("preset: a file cut at every byte is Truncated and nothing is read past the end") {
  for (const unsigned count : {0u, 4u, 32u}) {
    const fp::Preset p = count == 4 ? ref_preset() : full_preset(count, true);
    const Bytes b = written(p);
    NEED(!b.empty());
    for (std::size_t n = 0; n < b.size(); ++n) {
      const auto buf = exact(b, n);
      fp::Preset out = ref_preset();
      fp::Report r;
      const fp::Error e = fp::read(buf.get(), n, out, r);
      CAPTURE(n);
      CHECK(e == fp::Error::Truncated);
      CHECK(r.error == e);
      CHECK(is_zero(out));
    }
  }
  fp::Preset out;
  fp::Report r;
  CHECK(fp::read(nullptr, 0, out, r) == fp::Error::Truncated);
  CHECK(fp::read(nullptr, 100, out, r) == fp::Error::Truncated);
}

TEST_CASE("preset: every bit of the reference flipped is refused, or is the one field that may change") {
  const Bytes b = ref_bytes();
  const std::size_t crc_minor = b.size() - 6 + 1;  // the CRC chunk's version, low byte: a newer minor of the CRC chunk is read
  for (std::size_t i = 0; i < b.size(); ++i) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      Bytes f = b;
      f[i] = static_cast<std::uint8_t>(f[i] ^ (1u << bit));
      const Got g = load(f);
      CAPTURE(i);
      CAPTURE(bit);
      if (i == crc_minor) {
        CHECK(g.error == fp::Error::None);
        CHECK(g.report.newer_minor);
        CHECK(same(g.preset, ref_preset()));
      } else {
        CHECK(g.error != fp::Error::None);
        CHECK(is_zero(g.preset));
      }
    }
  }
}

TEST_CASE("preset: every bit of the body flipped with the CRC made right again is refused or is a valid other preset") {
  // The flips above stop at the CRC. Here the CRC is recomputed, so each flip reaches the container's checks and the MPST parser.
  for (const unsigned count : {0u, 4u, 32u}) {
    const Bytes b = count == 4 ? ref_bytes() : written(full_preset(count, true));
    NEED(!b.empty());
    unsigned refused = 0;
    unsigned other = 0;
    for (std::size_t i = 0; i < b.size() - 14; ++i) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        Bytes f = b;
        f[i] = static_cast<std::uint8_t>(f[i] ^ (1u << bit));
        refresh_crc(f);
        const Got g = load(f);
        CAPTURE(count);
        CAPTURE(i);
        CAPTURE(bit);
        if (g.error == fp::Error::None) {
          CHECK(fp::valid(g.preset));  // a different preset that is still a preset: a value, a name character, the module version, the kind 1 <-> 2
          ++other;
        } else {
          CHECK(g.error != fp::Error::BadCrc);  // the CRC is right
          CHECK(is_zero(g.preset));
          ++refused;
        }
      }
    }
    CHECK(refused > 20);
    CHECK(other > 20);
  }
}

TEST_CASE("preset: read leaves the preset cleared on every error, and the report names where") {
  Bytes f = ref_bytes();
  f[f.size() - 1] ^= 1;
  fp::Preset p = ref_preset();
  fp::Report r;
  CHECK(fp::read(f.data(), f.size(), p, r) == fp::Error::BadCrc);
  CHECK(is_zero(p));
  CHECK(r.error == fp::Error::BadCrc);
  CHECK(r.chunk == id4('C', 'R', 'C', ' '));
  CHECK(r.offset == 82);
  CHECK(std::string(r.detail).size() > 0);
  CHECK(std::string(fp::error_name(fp::Error::BadCrc)) == "bad CRC");
  for (const fp::Error e : {fp::Error::None, fp::Error::Truncated, fp::Error::BadMagic, fp::Error::BadCrc, fp::Error::NewerMajor, fp::Error::Malformed, fp::Error::TooBig}) {
    CHECK(std::string(fp::error_name(e)).size() > 0);
  }
  // a read after an error is not affected by it
  CHECK(fp::read(ref_bytes().data(), 96, p, r) == fp::Error::None);
  CHECK(r.error == fp::Error::None);
  CHECK(same(p, ref_preset()));
}

TEST_CASE("preset: random damage never reads past the buffer and never gives a preset the writer would refuse") {
  std::uint32_t s = 0xC0FFEEu;
  const auto next = [&] {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  };
  const Bytes seeds[] = {ref_bytes(), written(full_preset(32, true)), written(full_preset(1, false)), written(full_preset(0, false))};
  unsigned read_ok = 0;
  unsigned malformed = 0;
  for (const Bytes& seed : seeds) {
    for (unsigned round = 0; round < 3000; ++round) {
      Bytes f = seed;
      const unsigned hits = 1 + next() % 4;
      const bool body_only = (next() & 1u) != 0;  // body damage with the CRC made right again, so that it reaches the parser
      for (unsigned h = 0; h < hits; ++h) {
        f[next() % (body_only ? f.size() - 14 : f.size())] = static_cast<std::uint8_t>(next());
      }
      if (body_only) {
        refresh_crc(f);
      }
      if ((next() & 7u) == 0) {
        f.resize(next() % (f.size() + 1));
      }
      const Got g = load(f);
      if (g.error == fp::Error::None) {
        CHECK(fp::valid(g.preset));  // what was read is something the writer would write
        ++read_ok;
      } else {
        NEED(is_zero(g.preset));
        malformed += g.error == fp::Error::Malformed ? 1 : 0;
      }
    }
  }
  CHECK(read_ok > 500);  // damage that is still a preset got through the CRC and the parser
  CHECK(malformed > 500);
}

TEST_CASE("preset: the corpus of tools/o3preset.py reads here with the same outcome and the same fields") {
#ifndef O3_PRESET_CORPUS
  FAIL("O3_PRESET_CORPUS is not defined: tests/CMakeLists.txt gives o3os_tests the corpus path");
#else
  std::ifstream in(O3_PRESET_CORPUS);
  NEED(in.good());  // the ctest fixture preset_corpus (preset_python) writes it before unit_formats runs
  std::string line;
  std::map<std::string, unsigned> counts;
  unsigned n = 0;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string index, expect, hex;
    words >> index >> expect >> hex;
    NEED(!hex.empty());
    std::vector<std::string> want;
    std::string detail;  // the text the Python reader refused the file with, when the case pins the branch (detail=<hex>)
    bool pinned = false;
    for (std::string w; words >> w;) {
      if (w.rfind("detail=", 0) == 0) {
        const Bytes text = from_hex(w.substr(7));
        detail.assign(text.begin(), text.end());
        pinned = true;
      } else {
        want.push_back(w);
      }
    }
    const Bytes data = from_hex(hex);
    const Got g = load(data);
    CAPTURE(index);
    CAPTURE(hex.substr(0, 80));
    CHECK(outcome(g.error) == expect);
    if (pinned) {
      CHECK(std::string(g.report.detail) == detail);  // the same branch of the reader (the text is host only)
    }
    CHECK(g.report.error == g.error);
    ++counts[expect];
    ++n;
    if (g.error != fp::Error::None) {
      CHECK(is_zero(g.preset));
      continue;
    }
    CHECK(summary(g) == want);
    // and what was read writes and reads back to the same preset
    const Bytes again = written(g.preset);
    NEED(!again.empty());
    const Got h = load(again);
    CHECK(h.error == fp::Error::None);
    CHECK(same(h.preset, g.preset));
  }
  CHECK(n > 900);
  for (const char* kind : {"ok", "bad_magic", "truncated", "bad_CRC", "newer_major_version", "malformed", "too_big"}) {
    CAPTURE(kind);
    CHECK(counts[kind] > 0);
  }
  CHECK(counts["ok"] > 30);
  CHECK(counts["malformed"] > 100);
#endif
}
