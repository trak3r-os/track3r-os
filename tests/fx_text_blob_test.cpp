// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET P3: the FX picker's names and descriptions are a deflate stream (app/fx/fx_text.txt packed by tools/fx_text_pack.py), read line by line through the UI's shared decoder.
// The golden of the old C++ table is kept as test data (tests/data/fx_picker_text.txt, "name<TAB>description" per entry, in kPickerOrder order, made once from the old kFx). This test holds
// the stream, fx_picker_name() and fx_picker_desc() to it: every entry's two strings are the golden's, byte for byte; the stream inflates to the names then the descriptions; the authoring
// file is the golden; an index past the list reads as entry 0; a small buffer cuts and terminates.
#include <doctest/doctest.h>

#include <o3os/app/fx_text.h>
#include <o3os/core/inflate.h>
#include <o3os/model/fx_table.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "support/need.h"

using namespace o3os;

namespace {

std::string slurp(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  std::string text;
  if (f == nullptr) {
    return text;
  }
  char buf[4096];
  std::size_t r;
  while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) {
    text.append(buf, r);
  }
  std::fclose(f);
  return text;
}

struct Entry {
  std::string name;
  std::string desc;
};

// The golden's entries (NEED leaves this void function on a malformed file).
void load_golden(const char* path, std::vector<Entry>& out) {
  const std::string text = slurp(path);
  NEED(!text.empty() && text.back() == '\n');
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t eol = text.find('\n', at);
    const std::size_t tab = text.find('\t', at);
    NEED(tab != std::string::npos && tab < eol);
    out.push_back({text.substr(at, tab - at), text.substr(tab + 1, eol - tab - 1)});
    at = eol + 1;
  }
}

}  // namespace

TEST_CASE("fx text blob: the served names and descriptions are the old table's, byte for byte") {
  std::vector<Entry> golden;
  load_golden(O3_FX_PICKER_TEXT, golden);
  NEED(golden.size() == model::fx::kPickerCount);
  for (unsigned i = 0; i < model::fx::kPickerCount; ++i) {
    char name[app::kFxNameMax];
    char desc[app::kFxDescMax];
    const unsigned nn = app::fx_picker_name(i, name, sizeof name);
    const unsigned nd = app::fx_picker_desc(i, desc, sizeof desc);
    CHECK_MESSAGE(nn == golden[i].name.size(), "name length of entry " << i);
    CHECK_MESSAGE(nd == golden[i].desc.size(), "description length of entry " << i);
    CHECK_MESSAGE(golden[i].name == name, "name of entry " << i);
    CHECK_MESSAGE(golden[i].desc == desc, "description of entry " << i);
  }
}

TEST_CASE("fx text blob: the authoring file is the golden, and the stream inflates to the names then the descriptions") {
  CHECK(slurp(O3_FX_PICKER_SOURCE) == slurp(O3_FX_PICKER_TEXT));
  std::vector<Entry> golden;
  load_golden(O3_FX_PICKER_TEXT, golden);
  NEED(golden.size() == model::fx::kPickerCount);
  std::string want;
  for (const Entry& e : golden) {
    want += e.name + "\n";
  }
  for (const Entry& e : golden) {
    want += e.desc + "\n";
  }
  CHECK(app::kFxTextRawSize == want.size());
  CHECK(app::kFxTextZSize < want.size() / 2);  // it is a real saving
  core::Inflater inf;
  inf.begin(app::kFxTextZ, app::kFxTextZSize, app::kFxTextRawSize);
  std::string got(want.size(), '\0');
  CHECK(inf.read(reinterpret_cast<std::uint8_t*>(&got[0]), got.size()) == want.size());
  CHECK(inf.done());
  CHECK_FALSE(inf.failed());
  CHECK(got == want);
}

TEST_CASE("fx text blob: the buffers hold every line, an index past the list is None, a small buffer cuts and terminates") {
  std::vector<Entry> golden;
  load_golden(O3_FX_PICKER_TEXT, golden);
  NEED(golden.size() == model::fx::kPickerCount);
  for (const Entry& e : golden) {
    CHECK(e.name.size() < app::kFxNameMax);
    CHECK(e.desc.size() < app::kFxDescMax);
  }
  char name[app::kFxNameMax];
  char desc[app::kFxDescMax];
  CHECK(app::fx_picker_name(model::fx::kPickerCount, name, sizeof name) == 4u);
  CHECK(std::string(name) == "None");
  CHECK(app::fx_picker_name(1000u, name, sizeof name) == 4u);
  app::fx_picker_desc(1000u, desc, sizeof desc);
  CHECK(std::string(desc) == "No effect on this step.");
  char small[6];
  CHECK(app::fx_picker_name(11u, small, sizeof small) == 5u);  // "Random Instrument"
  CHECK(std::string(small) == "Rando");
  char one[1];
  CHECK(app::fx_picker_name(2u, one, sizeof one) == 0u);
  CHECK(one[0] == '\0');
  CHECK(app::fx_picker_name(2u, nullptr, 0u) == 0u);
}

TEST_CASE("fx text blob: the cache gives the same strings for repeated, out-of-order and interleaved lookups, cold or warm") {
  std::vector<Entry> golden;
  load_golden(O3_FX_PICKER_TEXT, golden);
  NEED(golden.size() == model::fx::kPickerCount);
  const unsigned count = model::fx::kPickerCount;
  app::fx_text_cache() = app::FxTextCache{};  // cold: nothing decoded
  char name[app::kFxNameMax];
  char desc[app::kFxDescMax];
  // A scrambled walk (stride 11 is coprime with the count), each entry's name and description twice in a row, then the same walk backwards.
  for (int pass = 0; pass < 2; ++pass) {
    for (unsigned k = 0; k < count; ++k) {
      const unsigned i = pass == 0 ? (k * 11u) % count : (count - 1u - (k * 11u) % count);
      for (int rep = 0; rep < 2; ++rep) {
        app::fx_picker_name(i, name, sizeof name);
        app::fx_picker_desc(i, desc, sizeof desc);
        CHECK_MESSAGE(golden[i].name == name, "name of entry " << i);
        CHECK_MESSAGE(golden[i].desc == desc, "description of entry " << i);
      }
    }
  }
  CHECK(app::fx_text_cache().names_state == 1u);  // the names fit the cache: the picker never streams per row
  // A description cut by a small buffer must not corrupt the cached full one.
  char small[8];
  app::fx_picker_desc(3u, small, sizeof small);
  CHECK(std::string(small) == golden[3].desc.substr(0, 7));
  app::fx_picker_desc(3u, desc, sizeof desc);
  CHECK(golden[3].desc == desc);
  // A cold description lookup first (names not decoded yet) is the same text.
  app::fx_text_cache() = app::FxTextCache{};
  app::fx_picker_desc(count - 1u, desc, sizeof desc);
  CHECK(golden[count - 1u].desc == desc);
  app::fx_picker_name(count - 1u, name, sizeof name);
  CHECK(golden[count - 1u].name == name);
  // An index past the list reads as entry 0 through the cache too.
  app::fx_picker_name(count + 5u, name, sizeof name);
  app::fx_picker_desc(count + 5u, desc, sizeof desc);
  CHECK(golden[0].name == name);
  CHECK(golden[0].desc == desc);
}
