// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012b: the SDK's round trip at the loader: manifest (module.json) -> generated header -> module built by the packer -> .tmod -> the real loader's reading of the file against
// what the generator says it must be (`o3m_gen.py expect`, written by CMake beside each module). The templates of the three kinds and the TB-303 (the worked example). The same
// file is also read by the generator's own reader in ctest (o3m_gen_verify_*) and by tools/o3m_pack.py: three readers, one answer. Host only.
#include <doctest/doctest.h>

#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "support/need.h"

namespace {

using namespace o3os::module;
namespace fs = std::filesystem;

std::vector<std::uint8_t> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Expect {
  std::string name;
  unsigned kind = 0, abi_minor = 0, params = 0, views = 0;
  struct P {
    std::string name;
    float min, max, def;
    std::uint32_t flags;
  };
  std::vector<P> p;
  // ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): the lines "pitch N", "page K NAME FIRST" and "labels I a|b|c"
  bool pitch = false;
  struct Page {
    std::string name;
    unsigned first;
  };
  std::vector<Page> pages;
  std::vector<std::pair<unsigned, std::string>> labels;
  // ABI 1.12 (OS-INSFX-ABI2 A2): the lines "stereo none|capable|widens" and "preset K COUNT v0 .. vN NAME"
  std::string stereo = "none";
  bool rate = false;  // ABI 1.13 (OS-MODRATE S2): the line "rate 0|1": an instrument that renders at the half and quarter rate (fx_flags O3M_FX_RATE)
  struct Preset {
    std::string name;
    std::vector<float> values;
  };
  std::vector<Preset> presets;
  bool ok = false;
};

// The lines of `o3m_gen.py expect`: name, kind, abi, params, views, then param i NAME(with spaces) min max def flags.
Expect read_expect(const std::string& path) {
  Expect e;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("name ", 0) == 0) {
      e.name = line.substr(5);
    } else if (line.rfind("kind ", 0) == 0) {
      e.kind = static_cast<unsigned>(std::atoi(line.c_str() + 5));
    } else if (line.rfind("abi ", 0) == 0) {
      const char* dot = std::strchr(line.c_str(), '.');
      e.abi_minor = dot != nullptr ? static_cast<unsigned>(std::atoi(dot + 1)) : 0u;
    } else if (line.rfind("params ", 0) == 0) {
      e.params = static_cast<unsigned>(std::atoi(line.c_str() + 7));
    } else if (line.rfind("views ", 0) == 0) {
      e.views = static_cast<unsigned>(std::atoi(line.c_str() + 6));
    } else if (line.rfind("pitch ", 0) == 0) {
      e.pitch = std::atoi(line.c_str() + 6) != 0;
    } else if (line.rfind("rate ", 0) == 0) {
      e.rate = std::atoi(line.c_str() + 5) != 0;
    } else if (line.rfind("stereo ", 0) == 0) {
      e.stereo = line.substr(7);
    } else if (line.rfind("preset ", 0) == 0) {
      std::istringstream ss(line.substr(7));
      unsigned k = 0, count = 0;
      ss >> k >> count;
      Expect::Preset pr;
      for (unsigned i = 0; i < count; ++i) {
        std::string v;
        ss >> v;
        pr.values.push_back(std::strtof(v.c_str(), nullptr));
      }
      ss >> std::ws;
      std::getline(ss, pr.name);
      e.presets.push_back(pr);
    } else if (line.rfind("page ", 0) == 0) {
      std::istringstream ss(line.substr(5));
      unsigned k = 0, first = 0;
      std::string name;
      ss >> k >> name >> first;
      e.pages.push_back({name, first});
    } else if (line.rfind("labels ", 0) == 0) {
      const std::size_t sp = line.find(' ', 7);
      e.labels.emplace_back(static_cast<unsigned>(std::atoi(line.c_str() + 7)), sp == std::string::npos ? std::string() : line.substr(sp + 1));
    } else if (line.rfind("param ", 0) == 0) {
      // "param <i> <name...> <min> <max> <def> <flags>": the last four fields are numbers, the name is what lies between
      std::vector<std::string> w;
      std::istringstream ss(line);
      std::string t;
      while (ss >> t) {
        w.push_back(t);
      }
      if (w.size() < 7) {
        return e;
      }
      Expect::P p;
      p.flags = static_cast<std::uint32_t>(std::strtoul(w[w.size() - 1].c_str(), nullptr, 10));
      p.def = std::strtof(w[w.size() - 2].c_str(), nullptr);
      p.max = std::strtof(w[w.size() - 3].c_str(), nullptr);
      p.min = std::strtof(w[w.size() - 4].c_str(), nullptr);
      for (std::size_t i = 2; i + 4 < w.size(); ++i) {
        p.name += (i > 2 ? " " : "") + w[i];
      }
      e.p.push_back(p);
    }
  }
  e.ok = !e.name.empty() && e.p.size() == e.params;
  return e;
}

// Loads <dir>/modules/<sub>/<stem>.tmod through the loader and compares what it read with the generator's expectations.
void round_trip(const char* stem, const char* sub, unsigned header_kind) {
#if defined(O3M_FIXTURE_DIR) && defined(O3M_GEN_OUT_DIR)
  const std::vector<std::uint8_t> file = slurp(std::string(O3M_FIXTURE_DIR) + "/" + stem + ".tmod");
  const Expect e = read_expect(std::string(O3M_GEN_OUT_DIR) + "/" + stem + ".expect");
#else
  const std::vector<std::uint8_t> file;
  const Expect e;
#endif
  if (file.empty()) {
    const char* ci = std::getenv("O3_CI");
    CHECK_MESSAGE(!(ci != nullptr && ci[0] == '1'), "the module fixtures were not built and O3_CI=1");
    MESSAGE("module fixtures not built (no arm-none-eabi-g++): skipped " << stem);
    return;
  }
  NEED(e.ok);
  const fs::path root = fs::temp_directory_path() / (std::string("o3m_gen_test_") + stem + std::to_string(reinterpret_cast<std::uintptr_t>(&file)));
  fs::remove_all(root);
  fs::create_directories(root / "modules" / sub);
  {
    std::ofstream f(root / "modules" / sub / (std::string(stem) + ".tmod"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  }
  o3os::platform::host::DirStorage storage(root.string());
  NEED(storage.mount() == o3os::hal::storage::Status::Ok);
  ModuleSet set;
  ModuleLoader loader;
  alignas(64) static std::uint8_t slab[4096];
  alignas(64) static std::uint8_t sdram[32768];
  set.hot().init(slab, 0x1FFFC000u, sizeof slab);
  set.cold().init(sdram, 0x70000000u, sizeof sdram);
  NEED(loader.begin(storage, set, stem, nullptr));
  for (unsigned n = 0; n < 4000 && loader.state() == ModuleLoader::State::Running; ++n) {
    loader.step(512);
  }
  CHECK_MESSAGE(loader.state() == ModuleLoader::State::Done, stem << ": " << status_name(loader.status()));
  NEED(set.count() == 1u);
  const LoadedModule* m = set.at(0);
  CHECK(m->header.kind == header_kind);
  CHECK(m->header.kind == e.kind);
  CHECK(m->header.abi_major == 1);
  CHECK(m->header.abi_minor == e.abi_minor);
  CHECK(m->desc.kind == e.kind);
  CHECK(m->desc.param_count == e.params);
  CHECK(m->header.param_count == e.params);
  CHECK(m->desc.view_count == e.views);
  CHECK(m->header.view_count == e.views);
  // the name: a NUL-terminated string in the image at desc.name
  const std::uint8_t* name = resolve(m->header, m->place, m->desc.name, 1);
  NEED(name != nullptr);
  CHECK(e.name == reinterpret_cast<const char*>(name));
  for (unsigned i = 0; i < e.params; ++i) {
    Param32 p;
    NEED(read_param(m->header, m->place, m->desc, i, p));
    const Expect::P& want = e.p[i];
    CAPTURE(i);
    CHECK(p.min == want.min);
    CHECK(p.max == want.max);
    CHECK(p.def == want.def);
    CHECK(p.flags == want.flags);
    const std::uint8_t* pn = resolve(m->header, m->place, p.name, 1);
    NEED(pn != nullptr);
    CHECK(want.name == reinterpret_cast<const char*>(pn));
  }
  // ABI 1.8: the three optional members of the descriptor, as the loader read them, against the generator's lines
  if (e.abi_minor >= 8u) {
    CHECK((m->ext.pitch != 0u) == e.pitch);
    CHECK(m->ext.page_count == e.pages.size());
    const std::uint8_t* table = m->ext.page_count != 0u ? resolve(m->header, m->place, m->ext.pages, m->ext.page_count * sizeof(Page32)) : nullptr;
    for (std::size_t k = 0; k < e.pages.size() && table != nullptr; ++k) {
      Page32 page;
      std::memcpy(&page, table + k * sizeof page, sizeof page);
      const std::uint8_t* pname = resolve(m->header, m->place, page.name, 1);
      NEED(pname != nullptr);
      CHECK(e.pages[k].name == reinterpret_cast<const char*>(pname));
      CHECK(e.pages[k].first == page.first_param);
    }
    unsigned labelled = 0;
    const std::uint8_t* ltable = m->ext.labels != 0u ? resolve(m->header, m->place, m->ext.labels, e.params * sizeof(Labels32)) : nullptr;
    for (unsigned i = 0; i < e.params && ltable != nullptr; ++i) {
      Labels32 l;
      std::memcpy(&l, ltable + i * sizeof l, sizeof l);
      if (l.count == 0u) {
        continue;
      }
      ++labelled;
      std::string joined;
      const std::uint8_t* names = resolve(m->header, m->place, l.names, l.count * 4u);
      NEED(names != nullptr);
      for (std::uint32_t v = 0; v < l.count; ++v) {
        std::uint32_t at;
        std::memcpy(&at, names + 4u * v, 4);
        const std::uint8_t* text = resolve(m->header, m->place, at, 1);
        NEED(text != nullptr);
        joined += (v != 0u ? "|" : "") + std::string(reinterpret_cast<const char*>(text));
      }
      bool found = false;
      for (const auto& want : e.labels) {
        found = found || (want.first == i && want.second == joined);
      }
      CHECK_MESSAGE(found, "labels of parameter " << i << ": " << joined);
    }
    CHECK(labelled == e.labels.size());
  } else {
    CHECK((m->ext.pitch == 0u && m->ext.pages == 0u && m->ext.page_count == 0u && m->ext.labels == 0u));
  }
  // ABI 1.12: the fourth tail as read_desc reads it (the loader does not keep it): the stereo render, fx_flags and the factory presets against the generator's lines
  if (e.abi_minor >= static_cast<unsigned>(kStereoAbiMinor)) {
    Desc32 d;
    DescExt32 ext;
    DescImp32 imp;
    DescFx32 fx;
    NEED(read_desc(m->header, m->place, d, &ext, &imp, &fx) == Status::Ok);
    CHECK((fx.render_st != 0u) == (e.stereo != "none"));
    CHECK(fx.fx_flags == ((e.stereo == "widens" ? static_cast<std::uint32_t>(O3M_FX_WIDENS) : 0u) | (e.rate ? static_cast<std::uint32_t>(O3M_FX_RATE) : 0u)));
    CHECK(fx.preset_count == e.presets.size());
    const std::uint8_t* table = fx.preset_count != 0u ? resolve(m->header, m->place, fx.presets, fx.preset_count * static_cast<std::uint32_t>(sizeof(Preset32))) : nullptr;
    for (std::size_t k = 0; k < e.presets.size() && table != nullptr; ++k) {
      Preset32 pr;
      std::memcpy(&pr, table + k * sizeof pr, sizeof pr);
      CAPTURE(k);
      const std::uint8_t* pname = resolve(m->header, m->place, pr.name, 1);
      NEED(pname != nullptr);
      CHECK(e.presets[k].name == reinterpret_cast<const char*>(pname));
      CHECK(pr.count == e.presets[k].values.size());
      const std::uint8_t* values = resolve(m->header, m->place, pr.values, pr.count * 4u);
      NEED(values != nullptr);
      for (std::uint32_t i = 0; i < pr.count && i < e.presets[k].values.size(); ++i) {
        float v;
        std::memcpy(&v, values + 4u * i, sizeof v);
        CHECK(v == e.presets[k].values[i]);
      }
    }
  } else {
    CHECK(e.stereo == "none");
    CHECK_FALSE(e.rate);
    CHECK(e.presets.empty());
  }
  fs::remove_all(root);
}

}  // namespace

TEST_CASE("sdk round trip: the TB-303, the worked example: its manifest is what the loader reads from the packed file") { round_trip("tb303", "instruments", O3M_KIND_INSTRUMENT); }

TEST_CASE("sdk round trip: the Chip and the FM (ABI 1.8): pitch, pages and labels as the generator says they are") {
  round_trip("chip", "instruments", O3M_KIND_INSTRUMENT);
  round_trip("fm", "instruments", O3M_KIND_INSTRUMENT);  // (FM is also ABI 1.13 with the RATE flag: the generator's "rate 1" line)
}

TEST_CASE("sdk round trip: the instrument template") { round_trip("example_instrument", "instruments", O3M_KIND_INSTRUMENT); }

TEST_CASE("sdk round trip: the effect template") { round_trip("example_effect", "effects", O3M_KIND_EFFECT); }

TEST_CASE("sdk round trip: the app template") { round_trip("example_app", "apps", O3M_KIND_APP); }

TEST_CASE("sdk round trip: the stereo effect template (ABI 1.12): render_st, WIDENS, two factory presets and the parameters as the generator says they are") {
  round_trip("example_stereo", "effects", O3M_KIND_EFFECT);
}

TEST_CASE("sdk round trip: the sidechain effect template (ABI 1.12): stereo-capable, a key parameter and the parameters as the generator says they are") {
  round_trip("example_key", "effects", O3M_KIND_EFFECT);
}
