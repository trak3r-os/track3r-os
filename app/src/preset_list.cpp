// SPDX-License-Identifier: GPL-3.0-or-later
// The list of the F5 PRESET overlay (preset_list.h): the factory presets of the module in view, then the CARD section (OS-MODPRESET P4): the card's presets of that module, listed by
// the last ModuleUi::scan_presets (preset_open rescans on every open).
#include <o3os/app/module_ui.h>
#include <o3os/app/preset_list.h>
#include <o3os/app/screens/params_screen.h>

namespace o3os::app {

unsigned preset_list_count(const Model& model, const ViewState& view) noexcept {
  if (model.modules == nullptr || model.project == nullptr) {
    return 0u;
  }
  return model.modules->preset_count(*model.project, params_target(view)) + model.modules->card_preset_count();
}

bool preset_list_get(const Model& model, const ViewState& view, unsigned n, ParamSet& out, ModuleUi::CardPreset* card, unsigned* skipped) noexcept {
  if (model.modules == nullptr || model.project == nullptr) {
    return false;
  }
  if (card != nullptr) {
    *card = ModuleUi::CardPreset::Ok;
  }
  if (skipped != nullptr) {
    *skipped = 0;
  }
  const ParamTarget target = params_target(view);
  const unsigned factory = model.modules->preset_count(*model.project, target);
  if (n < factory) {
    return model.modules->factory_preset(*model.project, target, n, out);
  }
  // OS-MODPRESET P4: a card entry is read and resolved now (the load's F1 calls this once; the overlay's render takes the name from the scan instead).
  const char* name = model.modules->card_preset_at(n - factory);
  if (name == nullptr) {
    return false;
  }
  unsigned sk = 0;
  const ModuleUi::CardPreset r = model.modules->read_preset(*model.project, target, name, out, sk);
  if (card != nullptr) {
    *card = r;
  }
  if (skipped != nullptr) {
    *skipped = sk;
  }
  return r == ModuleUi::CardPreset::Ok;
}

}  // namespace o3os::app
