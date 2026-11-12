// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-POWER P1 (docs/specs/OS-POWER.md sections 3, 4.1 B, D3, D4): the app's side of the hold of Power and of the shutdown. The words and their fit; the line drawn at its true fill over every
// worst-case screen state and over a modal overlay, in both translate modes, within the chip's display list; the unsaved-work warning from the save layer's own dirty(); the shutdown mode of
// the App (no input is read, no screen polls, the strip is drawn); the save layer's part: the device settings written at once, a save in flight finished, then cancelled with its Temp removed and
// the old file whole, nothing new started, the RECENT file waiting; an export refused and stopped. The state machine itself is platform/mk66/tests/mk66_power_service_test.cpp, the hold
// tests/power_hold_test.cpp.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/app.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/core/power_hold.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/screens.h>
#include <o3os/sim/simulator.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using test::MemCard;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

audio::VoicePool& make_pool() {
  static o3os::test::StaticSlot<audio::VoicePool> slot;
  return slot.make();
}

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  MemCard card;
  app::SaveService saves;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(40000);
  app::SampleBank bank;

  Rig() {
    model::ModelArena& arena = test::arena(1);
    test::fill_small(arena.project);
    arena.project.start_new();  // C major, the runtime's default: the document is not unsaved at the start (OS-DEFSCALE)
    (void)arena.project.set_name("", 0);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    bank.init(make_pool(), arena, *bank_mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
    app->attach_samples(&bank);
    app->attach_storage(&card);
    saves.attach(card, card);
    app->attach_saves(&saves);
    card.make_dir_now("Projects");
    card.make_dir_now("Settings");
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  model::Project& project() { return test::arena(1).project; }
  std::uint64_t now_ms() const { return clock.uptime_ms(); }
};

}  // namespace

TEST_CASE("power: every strip text exists, fits the strip, and the Hold text warns of unsaved work") {
  using core::PowerText;
  CHECK(std::strcmp(app::power_text(PowerText::None, false), "") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::Hold, false), "HOLD TO SWITCH OFF") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::Hold, true), "UNSAVED WORK - LET GO TO KEEP IT") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::Hint, false), "HOLD POWER 2 S TO SWITCH OFF") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::Firmware, false), "FIRMWARE UPDATE RUNNING") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::Export, false), "EXPORT RUNNING - HOLD POWER AGAIN TO STOP IT") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::Saving, false), "SAVING... THEN POWER OFF") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::SaveCancelled, false), "SAVE CANCELLED - POWERING OFF") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::PoweringOff, false), "POWERING OFF") == 0);
  CHECK(std::strcmp(app::power_text(PowerText::DiskWriting, false), "COMPUTER MAY BE WRITING - HOLD AGAIN") == 0);
  for (const PowerText t : {PowerText::Hold, PowerText::Hint, PowerText::Firmware, PowerText::Export, PowerText::Saving, PowerText::SaveCancelled, PowerText::PoweringOff, PowerText::DiskWriting}) {
    CHECK(std::strlen(app::power_text(t, true)) <= core::kNotifyTextChars);
  }
}

TEST_CASE("power: the line and the strip fit the display list over every worst-case screen state, in every theme and both translate modes") {
  struct alignas(8) Words {
    std::uint32_t words[4 * eve::kMaxDisplayListWords];
  };
  static Words buffer;
  std::uint32_t* const words = buffer.words;
  platform::host::FakeDisplay sink(4 * eve::kMaxDisplayListWords);
  app::ViewState view;
  view.power.show = {750, core::PowerText::Export};  // the longest text, with a line
  view.power.unsaved = true;
  unsigned measured = 0;
  std::size_t worst_extra = 0;
  for (unsigned s = 0; s < sim::screen_count(); ++s) {
    const sim::Screen& screen = sim::screens()[s];
    for (unsigned i = 0; i < screen.state_count; ++i) {
      const sim::ScreenState& state = screen.states[i];
      if (!state.worst_case) {
        continue;
      }
      for (unsigned th = 0; th < ui::kThemeCount; ++th) {
        for (const bool translate : {true, false}) {
          std::size_t counts[2] = {};
          for (int with_power = 0; with_power < 2; ++with_power) {
            eve::DisplayListBuilder builder(sink, words, 4 * eve::kMaxDisplayListWords);
            const ui::Theme& theme = state.fixed_theme != nullptr ? state.fixed_theme() : ui::theme(ui::theme_id_at(th));
            ui::Renderer r(builder, theme);
            r.use_translate(translate);
            NEED(r.begin(ui::Color::Pane));
            state.draw(r, state.variants - 1);  // (the variant that needs the most words is not always the last: the margin below is what the budget leaves)
            if (with_power != 0) {
              app::render_power(view, r);
            }
            NEED(r.end());
            counts[with_power] = sink.last_frame().size();
          }
          ++measured;
          worst_extra = counts[1] - counts[0] > worst_extra ? counts[1] - counts[0] : worst_extra;
          CHECK_MESSAGE(counts[1] <= eve::kMaxDisplayListWords, screen.name << "/" << state.name << ": " << counts[1] << " words with the power line");
        }
      }
    }
  }
  CHECK(measured > 20u);
  CHECK(worst_extra < 256u);  // the margin ADR 0010 keeps over the 1792-word budget is enough for the line and its strip
}

TEST_CASE("power: the line is drawn at its fill in Sel on the strip's Chrome, over a modal overlay too, and the strip text on the info line") {
  sim::Simulator sim;
  sim.step();
  const ui::Theme& theme = ui::theme(ui::ThemeId::Amiga);
  const auto pixel = [&](int x, int y) {
    const std::uint8_t* p = sim.raster().rgba() + 4 * (static_cast<std::size_t>(y) * ui::kScreenW + static_cast<std::size_t>(x));
    return ui::Rgb{p[0], p[1], p[2]};
  };
  const auto same = [](const ui::Rgb& a, const ui::Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
  const ui::Rgb sel = theme.rgb(ui::Color::Sel);
  const ui::Rgb chrome = theme.rgb(ui::Color::Chrome);
  const ui::Rgb dim = theme.rgb(ui::Color::ChromeDim);
  CHECK(same(pixel(10, 1), chrome));  // nothing yet
  for (int modal = 0; modal < 2; ++modal) {
    if (modal != 0) {
      (void)sim.action(core::action_id("learn.toggle"), 0);  // the Learn overlay: modal, captures the keys
      NEED(sim.app().state().overlay == app::OverlayId::Learn);
    }
    sim.app().set_power({500, core::PowerText::Hold});  // (the simulator's own switch would tell the app its own show at its next step: the app is run by hand here)
    sim.app().run_once();
    CHECK(same(pixel(10, 1), sel));
    CHECK(same(pixel(399, 3), sel));
    CHECK(same(pixel(401, 1), chrome));  // the half of the line that is not filled yet is the strip's own
    CHECK(same(pixel(799, 1), chrome));
    CHECK(same(pixel(10, 4), dim));      // the rule under the whole length
    CHECK(same(pixel(799, 4), dim));
    sim.app().set_power({1000, core::PowerText::PoweringOff});
    sim.app().run_once();
    CHECK(same(pixel(799, 1), sel));  // full
    sim.app().set_power({});
    sim.app().run_once();
    CHECK(same(pixel(10, 1), chrome));
  }
  CHECK(sim.healthy());
}

TEST_CASE("power: set_power redraws only when what is shown changes") {
  sim::Simulator sim;
  sim.step();
  sim.wait_ms(40);
  app::App& a = sim.app();
  const std::uint32_t frames = a.frames();
  a.set_power({});
  a.run_once();
  CHECK(a.frames() == frames);  // nothing to show, nothing drawn
  a.set_power({100, core::PowerText::Hold});
  a.run_once();
  CHECK(a.frames() == frames + 1u);
  a.set_power({100, core::PowerText::Hold});
  a.run_once();
  CHECK(a.frames() == frames + 1u);  // the same again: no frame
  a.set_power({109, core::PowerText::Hold});
  a.run_once();
  CHECK(a.frames() == frames + 1u);  // the line moves in steps (kPowerFillStep thousandths: 8 px): a redraw per thousandth would be a frame a millisecond
  a.set_power({110, core::PowerText::Hold});
  a.run_once();
  CHECK(a.frames() == frames + 2u);
  a.set_power({110, core::PowerText::Hint});
  a.run_once();
  CHECK(a.frames() == frames + 3u);  // another text is a change at once
}

TEST_CASE("power: the Hold text warns while the project has unsaved work, from the save layer's own dirty()") {
  Rig rig;
  rig.passes(3);
  CHECK_FALSE(rig.app->unsaved_work());
  rig.app->set_power({300, core::PowerText::Hold});
  CHECK_FALSE(rig.app->state().power.unsaved);
  (void)rig.project().set_tempo_x100(rig.project().tempo_x100() + 100u);  // any accepted mutation
  CHECK(rig.app->unsaved_work());
  rig.app->set_power({301, core::PowerText::Hold});
  CHECK(rig.app->state().power.unsaved);
  // other texts do not carry the warning
  rig.app->set_power({0, core::PowerText::Hint});
  CHECK_FALSE(rig.app->state().power.unsaved);
  // a save takes the baseline: no warning any more
  CHECK(rig.saves.save_project(rig.app->session().jobs, rig.project(), "", true, "song", &rig.bank) == app::SaveService::Start::Started);
  for (unsigned i = 0; i < 4000 && rig.saves.busy(); ++i) {
    rig.pass();
  }
  rig.passes(4);
  CHECK_FALSE(rig.app->unsaved_work());
}

TEST_CASE("file: the header's UNSAVED mark follows the save layer's dirty(): it shows after a change and clears after a save, a new or opened document (OS-AUDIT-A6, E-FILE-17)") {
  Rig rig;
  (void)rig.app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::File));
  rig.passes(3);
  CHECK_FALSE(rig.app->model().project_unsaved);
  (void)rig.project().set_tempo_x100(rig.project().tempo_x100() + 100u);  // any accepted mutation
  const std::uint32_t frames = rig.app->frames();
  rig.pass();
  CHECK(rig.app->model().project_unsaved);
  CHECK(rig.app->frames() == frames + 1u);  // the change redraws the screen once
  rig.pass();
  CHECK(rig.app->frames() == frames + 1u);  // and a pass without a change draws nothing
  // a save takes the baseline
  CHECK(rig.saves.save_project(rig.app->session().jobs, rig.project(), "", true, "song", &rig.bank) == app::SaveService::Start::Started);
  for (unsigned i = 0; i < 4000 && rig.saves.busy(); ++i) {
    rig.pass();
  }
  rig.passes(4);
  CHECK_FALSE(rig.app->model().project_unsaved);
  // a new or opened document is the clean baseline (App::document_replaced, what NEW and OPEN end with)
  (void)rig.project().set_tempo_x100(rig.project().tempo_x100() + 100u);
  rig.pass();
  CHECK(rig.app->model().project_unsaved);
  rig.app->document_replaced();
  rig.pass();
  CHECK_FALSE(rig.app->model().project_unsaved);
  // an import's document is unsaved work on no card
  rig.app->document_replaced(true);
  rig.pass();
  CHECK(rig.app->model().project_unsaved);
}

TEST_CASE("power: the app in its shutdown mode reads no input, polls no screen, and still draws the strip") {
  Rig rig;
  rig.passes(3);
  const app::ScreenId screen = rig.app->state().screen;
  rig.app->power_begin();
  CHECK(rig.app->powering_off());
  rig.app->set_power({1000, core::PowerText::PoweringOff});
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Song), true));
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Song), false));
  rig.passes(10);
  CHECK(rig.app->state().screen == screen);  // the key was not read
  CHECK(std::string(rig.app->message()).empty());
  const std::uint32_t frames = rig.app->frames();
  rig.app->set_power({1000, core::PowerText::SaveCancelled});
  rig.passes(3);
  CHECK(rig.app->frames() > frames);  // the strip still changes on the screen
}

TEST_CASE("power: the device settings are written at once in the shutdown, not 1.5 s after the last change") {
  {
    Rig rig;  // without the shutdown: still waiting after 200 ms
    rig.passes(3);
    CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
    rig.pass(200);
    rig.passes(5);
    CHECK_FALSE(rig.card.has("Settings/device.o3st"));
  }
  Rig rig;
  rig.passes(3);
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  rig.pass(200);
  rig.app->power_begin();
  const std::uint64_t t0 = rig.now_ms();
  CHECK(rig.saves.power_busy());  // the flush is still to be written
  for (unsigned i = 0; i < 400 && rig.saves.power_busy(); ++i) {
    rig.pass();
  }
  CHECK_FALSE(rig.saves.power_busy());
  CHECK(rig.card.has("Settings/device.o3st"));
  CHECK(rig.now_ms() - t0 < 1000u);
  CHECK(rig.card.violations == 0);
}

TEST_CASE("power: a save in flight is finished in the shutdown, and the RECENT file waits") {
  Rig rig;
  rig.passes(3);
  NEED(rig.saves.save_project(rig.app->session().jobs, rig.project(), "", true, "song", &rig.bank) == app::SaveService::Start::Started);
  rig.pass();
  rig.app->power_begin();
  CHECK(rig.saves.power_busy());
  unsigned passes = 0;
  while (rig.saves.power_busy() && passes++ < 4000) {
    rig.pass();
  }
  CHECK_FALSE(rig.saves.power_busy());
  CHECK(rig.card.has("Projects/song/project.o3pj"));  // finished, not cancelled
  rig.passes(20);
  CHECK_FALSE(rig.card.has("Settings/recent.o3rc"));  // the list of recent projects waits: nothing new starts in the shutdown
  CHECK(rig.card.violations == 0);
}

TEST_CASE("power: a save that is cancelled leaves no Temp and no half file, and nothing follows it") {
  Rig rig;
  rig.passes(3);
  NEED(rig.saves.save_project(rig.app->session().jobs, rig.project(), "", true, "song", &rig.bank) == app::SaveService::Start::Started);
  rig.pass();
  rig.app->power_begin();
  CHECK_FALSE(rig.saves.power_committing());
  rig.saves.power_cancel();
  CHECK_FALSE(rig.saves.power_busy());
  rig.passes(20);
  CHECK_FALSE(rig.saves.power_busy());
  CHECK_FALSE(rig.saves.busy());
  CHECK_FALSE(rig.card.has("Projects/song/project.o3pj"));
  CHECK_FALSE(rig.card.has("Projects/song/_o3save.tmp"));
  CHECK_FALSE(rig.card.has("Settings/recent.o3rc"));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("power: an export is stage 0's to refuse and the second hold's to stop, the files already finished staying") {
  Rig rig;
  rig.passes(3);
  CHECK_FALSE(rig.app->export_running());
  NEED(rig.app->start_og_export() == nullptr);
  CHECK(rig.app->export_running());
  rig.pass();
  rig.app->export_stop();
  rig.passes(40);
  CHECK_FALSE(rig.app->export_running());
  CHECK(rig.saves.og_state() == app::SaveService::OgState::Cancelled);
  CHECK_FALSE(rig.card.has((std::string(rig.saves.og_folder()) + "/project.mt").c_str()));  // a stopped OG export is not a project (project.mt, the OG project file, is written last)
  CHECK_FALSE(rig.saves.busy());
  CHECK(rig.card.violations == 0);
}
