// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/module/ptfx_host_table.h>
#include <o3os/module/ptfx_quarantine.h>

#include <cstdint>
#include <string>

namespace o3os::app {
class App;
class PtfxPage;
}  // namespace o3os::app

namespace o3os::sim {

// OS-CEPTFX S4a (scenario `card ptfx`): writes a handful of synthetic header-only plugins (a chorus with a list parameter, a generator, a wavetable generator, a mono tool, one with twelve parameters, a
// refused one of a future version, and one whose name is too long) into Effects/ of the card folder `card_root`. The PLUGINS page of Sample edit reads headers only.
bool write_demo_ptfx(const std::string& card_root, std::string& error);

// OS-CEPTFX S4b: what F1 APPLY calls in the simulator (App::attach_ptfx_runner). There is no plugin code to load here, so a run goes through a host stand-in: only the demo `Mono Tool` has one (its
// AMOUNT is a gain in percent); every other plugin says PLUGINS RUN ON THE TRACKER. The run itself is the App's (run_ptfx, the job, the commit): only the plugin's calls are the stand-in's.
struct SimPtfxRun {
  app::App* app = nullptr;
  app::PtfxPage* page = nullptr;
  module::PtfxHostTableStorage store{};
  module::PtfxHostTable host{store.bytes};
  alignas(4) std::uint8_t scratch[1024] = {};
  module::PtfxQuarantine quarantine;

  static const char* run(void* self, unsigned index, const float* values, int slot) noexcept;
};

}  // namespace o3os::sim
