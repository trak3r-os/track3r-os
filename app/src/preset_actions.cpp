// SPDX-License-Identifier: GPL-3.0-or-later
// F5 PRESET on a module's parameter page (OS-INSFX-ABI2 A9, docs/specs/OS-INSFX-ABI2.md section 3.6): opens the list of presets (preset_list.h) over the page, loads the chosen one, and
// (OS-MODPRESET P4) saves the values in force as a card preset from inside the list.
#include "action_handlers.h"

#include <o3os/app/module_ui.h>
#include <o3os/app/preset_list.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include <cstring>

namespace o3os::app {

namespace {

// OS-USBDISK 4.3: while the computer has the card the news is asked, ahead of the persistent disk strip (save_actions.cpp).
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.card_on_computer ? ctx.view.notify.ask(severity, text, "module", ViewState::kMessageMs) : ctx.view.notify.post(severity, text, "module", ViewState::kMessageMs);
}

}  // namespace

// F5 on an instrument module's page or an insert's page (instrument_page): the list over the page, the cursor on the first preset. A page whose module is not bound does nothing (its F5 cap is
// blank there, and a blank key does nothing); a module with no presets opens the list, which says NO PRESETS, SHIFT+F5 SAVES (the same answer on every module, and the place OS-MODPRESET's card
// presets show). The card's presets of the module are rescanned here, on every open (ModuleUi::scan_presets, the F7 pattern).
bool preset_open(ActionContext& ctx) noexcept {
  if (ctx.view.overlay != OverlayId::None || ctx.view.screen != ScreenId::Instrument) {
    return false;
  }
  if (ctx.model.modules == nullptr) {
    return say(ctx, core::Severity::Info, "NO MODULE SYSTEM IN THIS BUILD");
  }
  if (ctx.model.project == nullptr || ctx.model.modules->bound_module(*ctx.model.project, params_target(ctx.view)) == nullptr) {
    return false;  // unbound: the F5 cap is blank on the page (params_screen.cpp) and a blank key does nothing
  }
  if (ctx.model.modules->param_count(*ctx.model.project, params_target(ctx.view)) == 0u) {
    return false;  // no parameters, so nothing a preset could set: the key is blank
  }
  bool more = false;
  (void)ctx.model.modules->scan_presets(ModuleUi::module_name_of(*ctx.model.project, params_target(ctx.view)), more);  // OS-MODPRESET P4: the CARD section, rescanned on every open
  ctx.view.modules.preset_more = more;
  ctx.view.modules.preset = 0;
  ctx.view.overlay = OverlayId::Preset;
  return true;
}

// Shift + F5 in the list (OS-MODPRESET P4): SAVE opens the on-screen keyboard for a preset's name over the page (preset_name_open in save_actions.cpp; its F8 accept saves the values in force
// as a card preset and says PRESET SAVED: <NAME>).
bool preset_save(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::Preset) {
    return false;
  }
  return preset_name_open(ctx);
}

// F1 in the list: the chosen preset into every parameter of the module (ModuleUi::apply_preset: an instrument's is one undo unit, an insert's none), then the strip says PRESET <NAME> LOADED, or PRESET
// <NAME> PARTIAL (<n> DROPPED) when the instance's ring was full and n changes never reached it (the project got every value), or PRESET <NAME> LOADED, <n> SKIPPED when a card preset named
// parameters the module does not have (OS-MODPRESET P4). A card preset that will not read says why instead: PRESET DAMAGED, PRESET OF <STEM>, NEWER PRESET FORMAT, ... The list closes
// whatever the answer. A list with nothing in it stays open (F1 reads blank there).
bool preset_load(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::Preset) {
    return false;
  }
  ParamSet set;
  ModuleUi::CardPreset card = ModuleUi::CardPreset::Ok;
  unsigned skipped = 0;
  const bool got = preset_list_get(ctx.model, ctx.view, ctx.view.modules.preset, set, &card, &skipped);
  if (card != ModuleUi::CardPreset::Ok) {  // a card preset refused before a byte was applied: the list closes and the strip says why
    ctx.view.overlay = OverlayId::None;
    if (card == ModuleUi::CardPreset::OtherModule && ctx.model.modules != nullptr) {
      core::FixedString<core::kNotifyTextChars> text;
      text.assign("PRESET OF ");
      for (const char* s = ctx.model.modules->card_preset_module(); *s != '\0' && text.size() + 1u < core::kNotifyTextChars; ++s) {
        const char up[2] = {static_cast<char>(*s >= 'a' && *s <= 'z' ? *s - 'a' + 'A' : *s), '\0'};
        text.append(up);
      }
      (void)say(ctx, core::Severity::Warn, text.c_str());
    } else {
      (void)say(ctx, core::Severity::Warn, ModuleUi::card_preset_text(card));
    }
    return true;
  }
  if (!got) {
    return false;  // a blank F1 (an empty list): the list stays open
  }
  ctx.view.overlay = OverlayId::None;
  if (ctx.edit == nullptr || ctx.model.modules == nullptr) {
    (void)say(ctx, core::Severity::Info, ModuleUi::pick_text(ModuleUi::PickResult::NoProject));
    return true;
  }
  unsigned dropped = 0;
  const ModuleUi::PresetResult res = ctx.model.modules->apply_preset(*ctx.edit, params_target(ctx.view), set, &dropped);
  if (res != ModuleUi::PresetResult::Ok) {
    (void)say(ctx, core::Severity::Warn, ModuleUi::preset_text(res));
    return true;
  }
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("PRESET ");
  for (unsigned i = 0; set.name[i] != '\0'; ++i) {
    const char c = set.name[i];
    const char up[2] = {static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c), '\0'};
    text.append(up);
  }
  if (dropped == 0u) {
    text.append(" LOADED");
    if (skipped != 0u) {  // a card preset's unknown names: counted, not applied (OS-MODPRESET P4)
      text.append(", ");
      (void)append_uint(text, skipped);
      text.append(" SKIPPED");
    }
  } else {  // worst case 7 + 15 + 10 + 2 + 9 = 43 characters: it fits the strip's 48
    text.append(" PARTIAL (");
    (void)append_uint(text, dropped);
    text.append(" DROPPED)");
  }
  (void)say(ctx, core::Severity::Info, text.c_str());
  return true;
}

}  // namespace o3os::app
