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

// OS-INSFX S5: a native effect module under the stem the insert document names (fill_insert_project: T1 INS B's record names "reverb", and the card has no such file, so the effects'
// picker keeps its greyed MISSING FROM THE CARD line). Registered here so the insert's parameter page has a module to generate from; the simulator has no insert host, so no view is
// drawn and no ParamChange is sent (an edit writes the record's blob, which the page then shows).
struct SimFxState {
  float values[8];
};
int simfx_init(void* s, const o3m_host*) {
  std::memset(s, 0, sizeof(SimFxState));
  return 0;
}
void simfx_shutdown(void*) {}
void simfx_set_param(void* s, std::uint32_t id, float v) {
  if (id < 8u) {
    static_cast<SimFxState*>(s)->values[id] = v;
  }
}
std::uint32_t simfx_render(void*, const float* in, float* out, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = in[i];
  }
  return 0;  // idle
}
void simfx_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void simfx_note_off(void*, std::uint32_t) {}
const o3m_param kSimFxParams[4] = {
    {"Mode", 0.0f, 3.0f, 0.0f, O3M_PARAM_INT},
    {"Cutoff", 20.0f, 20000.0f, 1000.0f, O3M_PARAM_LOG | O3M_PARAM_WITH_UNIT(O3M_UNIT_HZ)},
    {"Resonance", 0.0f, 100.0f, 10.0f, O3M_PARAM_WITH_UNIT(O3M_UNIT_PERCENT)},
    {"Mix", 0.0f, 100.0f, 100.0f, O3M_PARAM_WITH_UNIT(O3M_UNIT_PERCENT)},
};
// OS-INSFX-ABI2 A9: three factory presets, so the F5 PRESET list of the insert's page has something to show (qa_presets): two that give every value and one that gives only Mode (the others take their defaults).
const float kSimBright[4] = {1.0f, 12000.0f, 20.0f, 60.0f};
const float kSimDark[4] = {2.0f, 2000.0f, 5.0f, 40.0f};
const float kSimModeOnly[1] = {3.0f};
const o3m_preset kSimPresets[3] = {{"Bright Room", kSimBright, 4}, {"Dark Hall", kSimDark, 4}, {"Mode Only", kSimModeOnly, 1}};
const o3m_module kSimReverb = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "reverb", sizeof(SimFxState), 4, kSimFxParams,
                               simfx_init, simfx_render, simfx_set_param, simfx_note_on, simfx_note_off, simfx_shutdown, nullptr, 0,
                               nullptr, nullptr, 0, nullptr, nullptr, nullptr, 0, kSimPresets, 3};

// OS-INSFX-ABI2 A5: a native WIDENS effect (ABI 1.12, render_st with O3M_FX_WIDENS) with a card file, so the INSERTS picker lists it Loaded and its line is marked ST.
std::uint32_t simfx_widen_render(void*, const float* il, const float* ir, float* ol, float* orr, std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    const float m = (il[i] + ir[i]) * 0.5f;
    ol[i] = m * 1.5f;
    orr[i] = m * 0.5f;
  }
  return 0;  // idle
}
const o3m_module kSimWiden = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_EFFECT, sizeof(o3m_module), "widen", sizeof(SimFxState), 4, kSimFxParams,
                              simfx_init, simfx_render, simfx_set_param, simfx_note_on, simfx_note_off, simfx_shutdown, nullptr, 0,
                              nullptr, nullptr, 0, nullptr, nullptr, simfx_widen_render, O3M_FX_WIDENS, nullptr, 0};

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
  // OS-INSFX-ABI2 A5: "widen" is the third one: native in the set below, so the picker lists it Loaded and marks its line ST.
  const fs::path fx_dir = fs::path(root) / "modules" / "effects";
  fs::create_directories(fx_dir, ec);
  if (ec) {
    error = "modules/effects could not be made on the card: " + ec.message();
    return false;
  }
  for (const char* stem : {"biquad", "chorus", "widen"}) {
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
  // OS-INSFX S5: the insert document's T1 INS B record names "reverb"; native in the set so its parameter page is generated (no card file: the picker's greyed line is unchanged).
  if (!set_.add_native("reverb", kSimReverb)) {
    error = "the simulator's native effect 'reverb' was refused by the module set";
    return false;
  }
  // OS-INSFX-ABI2 A5: the WIDENS effect, native and on the card (its placeholder above), so the picker lists it Loaded with ST on its line.
  if (!set_.add_native("widen", kSimWiden)) {
    error = "the simulator's native effect 'widen' was refused by the module set";
    return false;
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
