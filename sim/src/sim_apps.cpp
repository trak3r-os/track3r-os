// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/sim_apps.h>

#include <o3os/hal/audio.h>
#include <o3os/module/image.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

// The generated table (sim/CMakeLists.txt, file(GENERATE)): the manifests' descriptor headers and O3SIM_APP_TABLE, one {stem, path of the host object, capability word} for each shipped app.
#include "sim_apps_table.h"

namespace o3os::sim {

namespace {

struct AppFile {
  const char* stem;
  const char* so;
  std::uint32_t caps;
};
const AppFile kApps[] = {O3SIM_APP_TABLE};

constexpr std::uint32_t kArenaBytes = 256u * 1024u;  // the cold arena: the state blocks of the apps (one block serves every app, a larger one takes a new block: the Tuner's 40,000 bytes is the largest)

}  // namespace

std::unique_ptr<SimApps> SimApps::create(hal::storage::Storage& card, const std::string& root, const hal::Clock& clock, std::string& error) {
  std::unique_ptr<SimApps> rig(new SimApps());
  if (!rig->open(card, root, clock, error)) {
    return nullptr;
  }
  return rig;
}

bool SimApps::open(hal::storage::Storage& card, const std::string& root, const hal::Clock& clock, std::string& error) {
  namespace fs = std::filesystem;
  scratch_ = Block(static_cast<std::uint8_t*>(::operator new[](audio::ModuleVoices::kScratchBytes, std::align_val_t{64})));
  arena_ = Block(static_cast<std::uint8_t*>(::operator new[](kArenaBytes, std::align_val_t{64})));
  std::memset(scratch_.get(), 0, audio::ModuleVoices::kScratchBytes);
  std::memset(arena_.get(), 0, kArenaBytes);
  if (!voices_.attach_scratch(scratch_.get(), audio::ModuleVoices::kScratchBytes) || !set_.cold().init(arena_.get(), 0x70000000u, kArenaBytes)) {
    error = "the module arenas did not start";
    return false;
  }
  voices_.set_sample_rate(static_cast<float>(hal::audio::sample_rate_mhz()) * 0.001f);

  // The card: modules/apps/<stem>.tmod (the folder MODULES lists). A copy of an app that is on the card already (a fixture that carries one) is left alone.
  std::error_code ec;
  const fs::path dir = fs::path(root) / "modules" / "apps";
  fs::create_directories(dir, ec);
  if (ec) {
    error = "modules/apps could not be made on the card: " + ec.message();
    return false;
  }
  for (const AppFile& a : kApps) {
    const fs::path file = dir / (std::string(a.stem) + ".tmod");
    if (!fs::exists(file, ec)) {
      std::ofstream out(file, std::ios::binary);
      out << "o3sim: the host object stands for this file (sim/include/o3os/sim/sim_apps.h)\n";
      if (!out) {
        error = file.string() + " could not be written";
        return false;
      }
    }
    void* h = dlopen(a.so, RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
      const char* why = dlerror();
      error = std::string("the host object of '") + a.stem + "' did not open (" + a.so + "): " + (why != nullptr ? why : "unknown");
      return false;
    }
    handles_.push_back(h);
    const o3m_module* entry = static_cast<const o3m_module*>(dlsym(h, "o3m_entry"));
    if (entry == nullptr || entry->kind != O3M_KIND_APP || !set_.add_native(a.stem, *entry, a.caps)) {
      error = std::string("the host object of '") + a.stem + "' is not an app the module set takes (o3m_entry missing, not an app, or a check of ModuleSet::add_native refused it)";
      return false;
    }
    stems_.emplace_back(a.stem);
  }

  // OS-INSFX S4: two effect files for the Mixer INSERTS page's picker (modules/effects: the folder it lists). Placeholders only, like the apps' (no host object: the simulator has no insert host, so nothing binds them).
  const fs::path fx_dir = fs::path(root) / "modules" / "effects";
  fs::create_directories(fx_dir, ec);
  if (ec) {
    error = "modules/effects could not be made on the card: " + ec.message();
    return false;
  }
  for (const char* stem : {"biquad", "chorus"}) {
    const fs::path file = fx_dir / (std::string(stem) + ".tmod");
    if (!fs::exists(file, ec)) {
      std::ofstream out(file, std::ios::binary);
      out << "o3sim: a placeholder for the effect list (sim/src/sim_apps.cpp)\n";
      if (!out) {
        error = file.string() + " could not be written";
        return false;
      }
    }
  }

  ui_.attach(set_, loader_, voices_);
  ui_.set_storage(&card);
  ui_.set_clock(&clock);
  ui_.set_audio_tap(&tap_);
  ui_.set_midi_monitor(&monitor_);
  return true;
}

SimApps::~SimApps() {
  ui_.reset();  // stops the running app without calling it (its object is still open here)
  for (void* h : handles_) {
    dlclose(h);
  }
}

}  // namespace o3os::sim
