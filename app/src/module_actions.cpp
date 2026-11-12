// SPDX-License-Identifier: GPL-3.0-or-later
// The SD modules' actions (OS-012b): the engine picker of the Instrument page, the MODULES screen (launch, rescan), an app's page (exit, its parameters).
#include "action_handlers.h"

#include <o3os/app/module_ui.h>
#include <o3os/app/project_state.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/text_format.h>
#include <o3os/app/tools_list.h>

namespace o3os::app {

namespace {

// OS-USBDISK 4.3: while the computer has the card the news is asked, ahead of the persistent disk strip (save_actions.cpp).
bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  return ctx.view.card_on_computer ? ctx.view.notify.ask(severity, text, "module", ViewState::kMessageMs) : ctx.view.notify.post(severity, text, "module", ViewState::kMessageMs);
}

unsigned instrument_slot(const ViewState& view) noexcept {
  return view.instrument < 1 ? 0u : (view.instrument > static_cast<int>(model::kMaxInstruments) ? static_cast<unsigned>(model::kMaxInstruments) - 1u : static_cast<unsigned>(view.instrument - 1));
}

// The launch behind MODULES F1 and the apps list's F1 and F8: starts catalogue entry `entry` and opens its page. `listed` (the apps list, a modal overlay that hides the strip): a refusal or a question
// goes to the list's own line (tools_msg) and a launch closes the list. MODULES: the strip says it. NeedsConfirm is answered by the same key again.
bool launch_entry(ActionContext& ctx, unsigned entry, bool listed) noexcept {
  const ModuleUi::LaunchResult res = ctx.model.modules->launch_app(entry, ctx.model.project);
  if (res != ModuleUi::LaunchResult::Ok) {
    const core::Severity severity = core::Severity::Warn;
    const char* text = ctx.model.modules->launch_message(res);
    if (listed) {
      ctx.view.modules.tools_msg = text;
      return true;
    }
    if (res == ModuleUi::LaunchResult::NeedsConfirm) {  // a question: the head of the strip, where the key row finds it (ask() rewrites the oldest "module" line in place, so a second F1 does not stack)
      return ctx.view.notify.ask(severity, text, "module", ViewState::kMessageMs);
    }
    return say(ctx, severity, text);
  }
  if (listed) {
    ctx.view.overlay = OverlayId::None;
    ctx.view.modules.tools_msg = "";
  }
  if (ctx.model.project != nullptr) {
    (void)app_params_apply(*ctx.model.modules, *ctx.model.project);  // OS-004d: the values this project remembers for the app (same name and ABI major)
  }
  ctx.view.modules.app_row = 0;
  ctx.view.modules.from_tools = false;
  (void)switch_screen(screen_registry(), ctx.model, ctx.view, ScreenId::ModuleApp);
  return true;
}

}  // namespace

// F1 ENGINE on the Instrument page: the picker over the page, started on the module the slot has (if it has one).
bool module_engine(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.model.modules == nullptr) {
    return say(ctx, core::Severity::Info, "NO MODULE SYSTEM IN THIS BUILD");
  }
  if (ctx.view.overlay != OverlayId::None || ctx.view.screen != ScreenId::Instrument) {
    return false;
  }
  if (ctx.model.project == nullptr || ctx.session == nullptr || ctx.session->document() == nullptr) {  // the pick edits the project: with none open there is nothing to pick for
    return say(ctx, core::Severity::Info, ModuleUi::pick_text(ModuleUi::PickResult::NoProject));
  }
  ModuleUi& mu = *ctx.model.modules;
  mu.scan_instruments(ctx.model.project);
  unsigned at = 0;
  if (ctx.model.project != nullptr) {
    const char* name = ModuleUi::module_name_of(*ctx.model.project, instrument_slot(ctx.view));
    for (unsigned i = 0; name[0] != '\0' && i < mu.instrument_count(); ++i) {
      const char* stem = mu.instrument_at(i)->stem;
      unsigned k = 0;
      while (stem[k] != '\0' && stem[k] == name[k]) {
        ++k;
      }
      if (stem[k] == '\0' && name[k] == '\0') {
        at = i;
      }
    }
  }
  ctx.view.modules.picker = static_cast<std::uint8_t>(at);
  ctx.view.modules.insert_where = ModulesView::kEngine;
  ctx.view.overlay = OverlayId::ModulePicker;
  return true;
}

// F1 in the picker: binds the chosen module to the instrument in view (one undo unit: the instrument and its default parameters; the module table grows when it must). The picker
// closes whatever the answer, which the notification strip then says.
bool module_pick(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::ModulePicker) {
    return false;
  }
  if (ctx.view.modules.insert_where == ModulesView::kApps) {  // OS-APPSNAV: F1 of the apps list launches the app under the cursor; the list stays open when the launch is refused (the confirm of an app that edits or writes files cannot arise here: those are tool apps)
    const unsigned k = ctx.view.modules.picker;
    if (ctx.model.modules == nullptr || k >= apps_count(ctx.model)) {
      return true;
    }
    return launch_entry(ctx, apps_entry(ctx.model, k), true);
  }
  ctx.view.overlay = OverlayId::None;
  if (ctx.model.modules == nullptr) {
    return true;
  }
  if (ctx.edit == nullptr) {
    (void)say(ctx, core::Severity::Info, ModuleUi::pick_text(ModuleUi::PickResult::NoProject));
    return true;
  }
  if (ctx.view.modules.insert_where != ModulesView::kEngine) {  // OS-INSFX S4: the Mixer's INSERTS page asked: an effect for the cell, the module system's one path to the host and the project
    ModuleUi& mu = *ctx.model.modules;
    const CatalogEntry* e = mu.effect_at(ctx.view.modules.picker);
    const unsigned where = ctx.view.modules.insert_where;
    ModuleUi::InsertResult r = ModuleUi::InsertResult::NoEntry;
    if (e != nullptr) {
      r = e->state == CatalogEntry::State::Missing ? ModuleUi::InsertResult::Missing
                                                    : (e->state == CatalogEntry::State::Quarantined ? ModuleUi::InsertResult::Quarantined : mu.insert_assign(*ctx.edit, where / 2u, where % 2u, e->stem));
    }
    (void)say(ctx, r == ModuleUi::InsertResult::Ok ? core::Severity::Info : core::Severity::Warn, ModuleUi::insert_text(r));
    return true;
  }
  const ModuleUi::PickResult res = ctx.model.modules->pick(*ctx.edit, instrument_slot(ctx.view), ctx.view.modules.picker);
  (void)say(ctx, res == ModuleUi::PickResult::Ok ? core::Severity::Info : core::Severity::Warn, ModuleUi::pick_text(res));
  return true;
}

bool module_rescan(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.model.modules == nullptr) {
    return false;
  }
  ctx.model.modules->scan_apps();
  const int last = static_cast<int>(ctx.model.modules->app_entry_count()) - 1;
  if (ctx.view.modules.apps > last) {
    ctx.view.modules.apps = static_cast<std::uint8_t>(last < 0 ? 0 : last);
  }
  core::FixedString<core::kNotifyTextChars> text;  // OS-QAFIX-RESCAN: the strip tells what the card has now
  rescan_text(text, ctx.model.modules->app_entry_count(), false, "APP", "APPS");
  (void)say(ctx, core::Severity::Info, text.c_str());
  return true;
}

// F1 on MODULES: launches the app under the cursor and opens its page.
bool module_launch(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.model.modules == nullptr) {
    return say(ctx, core::Severity::Info, "NO MODULE SYSTEM IN THIS BUILD");
  }
  return launch_entry(ctx, ctx.view.modules.apps, false);
}

// OS-APPSNAV: Shift + Pattern, the list of the apps that are not tools (the picker in apps mode). The card is read again, as MODULES does, so the list is what the card has now.
bool apps_open(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::None) {
    return false;
  }
  if (ctx.model.modules != nullptr) {
    ctx.model.modules->scan_tools();  // (scan_apps, and which entries are tools: the list is the rest)
  }
  ctx.view.modules.picker = 0;
  ctx.view.modules.insert_where = ModulesView::kApps;
  ctx.view.modules.tools_msg = "";
  ctx.view.overlay = OverlayId::ModulePicker;
  return true;
}

// Pattern or Shift + Pattern in the apps list closes it (Pattern is the key that opened it); false in the other pickers, which F8 closes.
bool apps_close(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.overlay != OverlayId::ModulePicker || ctx.view.modules.insert_where != ModulesView::kApps) {
    return false;
  }
  ctx.view.overlay = OverlayId::None;
  return true;
}

// Where the exit chord of an app's page goes (OS-H3A): back to Pattern when the app was opened from its F7 TOOLS list (the cursor and the block are the view state's, untouched), to MODULES when it
// was opened from there. One level of return. Clears the mark.
ScreenId app_exit_screen(ViewState& view) noexcept {
  const bool tools = view.modules.from_tools;
  view.modules.from_tools = false;
  return tools ? ScreenId::Pattern : ScreenId::Modules;
}

// The exit chord F7 + F8 of an app's page (the OS's: it works whatever the module does): back to MODULES, which ends the app (Screen::leave).
bool module_exit(ActionContext& ctx, core::ActionArg) noexcept {
  if (ctx.view.screen != ScreenId::ModuleApp) {
    return false;
  }
  return switch_screen(screen_registry(), ctx.model, ctx.view, app_exit_screen(ctx.view));
}

// Shift + the encoder on an app's page: the parameter under the cursor, by detents.
bool module_app_edit(ActionContext& ctx, core::ActionArg arg) noexcept {
  ModuleUi* mu = ctx.model.modules;
  if (mu == nullptr || ctx.view.screen != ScreenId::ModuleApp || !mu->app_os_column()) {
    return false;
  }
  const unsigned row = ctx.view.modules.app_row < mu->app_param_count() ? ctx.view.modules.app_row : 0u;
  ParamScratch s;
  if (!mu->app_describe(row, s)) {
    return false;
  }
  constexpr int kMaxDetents = 1 << 16;
  const int detents = arg > kMaxDetents ? kMaxDetents : (arg < -kMaxDetents ? -kMaxDetents : arg);
  return mu->app_set_param(row, core::param_step(s.desc, mu->app_param(row), detents));
}

bool module_app_reset(ActionContext& ctx, core::ActionArg) noexcept {
  ModuleUi* mu = ctx.model.modules;
  if (mu == nullptr || ctx.view.screen != ScreenId::ModuleApp || !mu->app_os_column()) {
    return false;
  }
  const unsigned row = ctx.view.modules.app_row < mu->app_param_count() ? ctx.view.modules.app_row : 0u;
  ParamScratch s;
  return mu->app_describe(row, s) && mu->app_set_param(row, s.desc.def);
}

// OS-H1: the tempo an app asked for through its edit table (ADR 0014 amendment B), BPM x 100. The app's call only queued it; App::poll_modules runs this action, so the change is one undo
// unit like any tempo edit (project.tempo), the project is marked changed, and the audio side follows it by the usual path. The model's own range check is the last word.
bool module_app_tempo(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (ctx.edit == nullptr || arg < 0) {
    return false;
  }
  const std::uint32_t value = static_cast<std::uint32_t>(arg);
  return value != ctx.edit->project().tempo_x100() && ctx.edit->set_tempo_x100(value) == model::Status::Ok;
}

}  // namespace o3os::app
