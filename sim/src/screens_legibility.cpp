// SPDX-License-Identifier: GPL-3.0-or-later
// OS-E2E-5: the worst-case content for the legibility sweep (sim/legibility.h, `o3sim --legibility`). Every limit of the model at once, drawn through the
// app's own scene: a project with the longest name, the highest tempo (800 BPM), instrument 64 with the longest instrument name, pattern 255 with the longest
// pattern name, twelve tracks with the longest track names, both FX cells of every step filled, and (one state) the longest message the strip can carry.
// The other gallery states show typical text and the dearest word counts; these show the widest text, so a name that runs into the next field, or off
// the panel, shows up as a finding. They are gallery states like the others (a picture and a golden each).
#include "screens_app.h"

#include <o3os/app/dir_scan.h>
#include <o3os/app/firmware_view.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/view_state.h>
#include <o3os/core/notify.h>
#include <o3os/model/limits.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/fake_storage.h>

#include <cstring>
#include <memory>
#include <string>

namespace o3os::sim {

namespace {

using app::Model;
using app::PatternMode;
using app::ScreenId;
using app::ViewState;

constexpr std::uint32_t kPatternId = 255;  // the highest pattern id (the OG numbering, model/limits.h)

// The project every state draws: all the limits at once. Allocated on first use (1.9 MB, like the device's arena in SDRAM).
model::Project& project() {
  static std::unique_ptr<model::ModelArena> arena;
  if (!arena) {
    arena = std::make_unique<model::ModelArena>();
    std::memset(arena.get(), 0, sizeof(model::ModelArena));
    model::Project& p = arena->project;
    p.reset();
    // 'W' is the widest letter of a proportional face; the faces are monospace, so it is the name's length that matters, and it has the most ink.
    (void)p.set_name(std::string(model::kProjectNameMax, 'W').c_str());
    (void)p.set_tempo_x100(model::kMaxTempoX100);
    (void)p.create_pattern(kPatternId, static_cast<std::uint32_t>(app::kSteps));
    (void)p.set_pattern_name(kPatternId, std::string(model::kPatternNameMax, 'W').c_str(), model::kPatternNameMax);
    const app::PatternData& data = pattern_worst_data(0);  // every step: a note, an instrument, a volume and both FX cells
    for (unsigned t = 0; t < static_cast<unsigned>(app::kTracks); ++t) {
      (void)p.set_steps(kPatternId, t, 0, data.steps[t], static_cast<std::uint32_t>(app::kSteps));
      model::TrackSlot slot = *p.track(t);
      slot.type = t % 3 == 0 ? model::TrackType::Audio : (t % 3 == 1 ? model::TrackType::Midi : model::TrackType::ExternalAudio);
      (void)p.set_track(t, slot);
      (void)p.set_track_name(t, std::string(model::kTrackNameMax, 'W').c_str(), model::kTrackNameMax);
    }
    (void)p.append_order(kPatternId);
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::memset(in.name, 'W', model::kInstrumentNameMax);
    for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
      (void)p.set_instrument(i, in);
    }
  }
  return arena->project;
}

Model make_model() {
  Model m{};
  m.project = &project();
  m.undo_depth = 99;
  m.device.init(core::ParamKind::Device);
  return m;
}

// Instrument 64 and pattern 255, Edit, the cursor in the middle of the grid.
ViewState pattern_view() {
  ViewState v;
  v.mode[app::screen_index(ScreenId::Pattern)] = static_cast<std::uint8_t>(PatternMode::Edit);
  v.edit.pattern = static_cast<std::uint8_t>(kPatternId);
  v.instrument = static_cast<int>(model::kMaxInstruments);
  v.pattern.scroll_rows(20);
  return v;
}

void draw_pattern(ui::Renderer& r, unsigned) {
  const Model m = make_model();
  app::render_scene(m, pattern_view(), r);
}

// ... and the strip carrying its longest message: 48 characters without a space and a source of 8, posted 99 times (one entry, count x99), as in the notification sweep.
void draw_pattern_notice(ui::Renderer& r, unsigned) {
  const Model m = make_model();
  ViewState v = pattern_view();
  for (unsigned i = 0; i < core::kNotifyMaxCount; ++i) {
    (void)v.notify.post(core::Severity::Error, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKL", "ABCDEFGH", 2000);
  }
  app::render_scene(m, v, r);
}

// The Instrument page of instrument 64: its 32-character name in the strip.
void draw_params(ui::Renderer& r, unsigned) {
  const Model m = make_model();
  ViewState v;
  v.screen = ScreenId::Instrument;
  v.page[app::screen_index(ScreenId::Instrument)] = static_cast<std::uint8_t>(model::instrument_param::GroupVoice);
  v.params.row = 1;
  v.instrument = static_cast<int>(model::kMaxInstruments);
  app::render_scene(m, v, r);
}

// The File screen of that project: its 32-character title in the header, over a card with a few projects.
void draw_file(ui::Renderer& r, unsigned) {
  struct Scene {
    platform::host::FakeStorage card;
    app::DirScan scan;
  };
  static Scene* scene = [] {
    auto* s = new Scene;  // kept for the life of the process, like the other states' statics
    const std::string widest(model::kProjectNameMax, 'W');
    for (const std::string& name : {std::string("acid-jam"), std::string("Breaks 2024"), std::string("demo song"), widest, std::string("t-v16")}) {
      s->card.add_dir("Projects/" + name);
      s->card.add_file("Projects/" + name + "/project.o3pj", 41000);
    }
    (void)s->scan.begin(s->card, app::kProjectsFolder, app::kProjectFilter);
    while (s->scan.step(1000000) == core::JobStatus::Running) {
    }
    return s;
  }();
  Model m = make_model();
  m.files = &scene->scan;
  ViewState v;
  v.screen = ScreenId::File;
  (void)v.file.set(3, scene->scan.index());
  app::render_scene(m, v, r);
}

// OS-FWCONFIG: the firmware update list with twelve files of 52 characters (the widest name the port takes, hal::firmware::kNameCap - 1), the biggest size it can show, RESET TARGET on the first (its
// name is cut with "..." before the tag), the note "MORE FILES" and a line of the full 86 characters (FirmwareView::kMsgChars). 'W' is the widest letter; the faces are monospace, so it is the length that matters.
void draw_firmware(ui::Renderer& r, unsigned) {
  Model m = make_model();
  m.firmware = true;
  m.product = "Track3r Community OS";
  m.build_id = "0123456789ab";
  ViewState v;
  v.screen = ScreenId::Config;
  v.page[app::screen_index(ScreenId::Config)] = static_cast<std::uint8_t>(app::kConfigPageInfo);
  v.overlay = app::OverlayId::FirmwareUpdate;
  app::FirmwareView& f = v.firmware;
  f.phase = app::FirmwareView::Phase::List;
  f.n = app::FirmwareView::kMax;
  f.entries = app::FirmwareView::kScanMax;  // (the widest "FILE n / 999" position line)
  f.bad = true;
  f.cursor = 5;
  for (unsigned i = 0; i < app::FirmwareView::kMax; ++i) {
    std::memset(f.files[i].name, 0, sizeof f.files[i].name);
    std::memset(f.files[i].name, 'W', 48);
    std::memcpy(f.files[i].name + 48, ".ptf", 4);  // (48 + 4 characters)
    f.files[i].size = 4000000u - i;
    f.files[i].reset_target = i == 0;
  }
  f.msg.assign(std::string(app::FirmwareView::kMsgChars, 'W').c_str());
  app::render_scene(m, v, r);
}

}  // namespace

const ScreenState kExtremeStates[5] = {
    {"pattern", true, 1, kBudgetWords, draw_pattern},
    {"pattern_notice", true, 1, kBudgetWords, draw_pattern_notice},
    {"params", true, 1, kBudgetWords, draw_params},
    {"file", true, 1, kBudgetWords, draw_file},
    {"firmware", true, 1, kBudgetWords, draw_firmware},
};

}  // namespace o3os::sim
