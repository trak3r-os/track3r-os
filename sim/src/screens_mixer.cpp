// SPDX-License-Identifier: GPL-3.0-or-later
// The Mixer screen's gallery and budget states (OS-011b): a project with a mixer that has something to show (levels, pans, a mute and a solo), a board whose
// meters have been fed a few windows of music-like levels, and the pages over them. Everything is a plain object read by the same render the device runs.
#include "screens_app.h"

#include <o3os/app/mix_board.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/screens/scene.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/stem_map.h>
#include <o3os/model/params.h>

#include <memory>

namespace o3os::sim {

using app::MixBoard;
using app::Model;
using app::ScreenId;
using app::ViewState;

namespace {

// A level of the meters' kind: peak and RMS as linear amplitudes, fed a few times so that the ballistics settle to something that is not the first sample.
void feed(app::MixBoard& b, unsigned channel, float peak, float rms) {
  for (unsigned i = 0; i < 6; ++i) {
    b.meter[channel].update(ui::plot::MeterReading{peak, rms});
  }
}

struct MixerWorld {
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  MixBoard board;
  audio::StemMap stems;

  MixerWorld() {
    arena->project.reset();
    for (unsigned t = 0; t < 12; ++t) {
      model::TrackSlot s = *arena->project.track(t);
      s.volume = static_cast<std::uint8_t>(100 - 5 * (t % 5));
      s.pan = static_cast<std::uint8_t>(t % 3 == 0 ? 50 : (t % 3 == 1 ? 20 : 85));
      s.flags = t == 2 ? model::kTrackMute : (t == 5 ? model::kTrackSolo : 0);
      (void)arena->project.set_track(t, s);
    }
    board.stems = &stems;
    const float peaks[12] = {0.9f, 0.7f, 0.0f, 0.5f, 0.8f, 0.95f, 0.3f, 0.6f, 0.2f, 0.1f, 0.45f, 0.0f};
    for (unsigned t = 0; t < 12; ++t) {
      feed(board, audio::meter_ch::kTrack0 + t, peaks[t], peaks[t] * 0.6f);
    }
    feed(board, audio::meter_ch::kInL, 0.25f, 0.12f);
    feed(board, audio::meter_ch::kInR, 0.2f, 0.1f);
    feed(board, audio::meter_ch::kSndA, 0.4f, 0.2f);
    feed(board, audio::meter_ch::kSndB, 0.3f, 0.15f);
    feed(board, audio::meter_ch::kCueL, 0.5f, 0.3f);
    feed(board, audio::meter_ch::kCueR, 0.45f, 0.28f);
    feed(board, audio::meter_ch::kMasterL, 0.98f, 0.5f);
    feed(board, audio::meter_ch::kMasterR, 1.0f, 0.55f);  // the clip lamp
    board.in_level = 35;
    board.cue_level = 80;
    board.route[1] = audio::route::kMain | audio::route::kCue | audio::route::kSndB;
    board.route[3] = audio::route::kMain | audio::route::kRec | audio::route::kSndA | audio::route::kUsb;
  }

  Model model() const {
    Model m{};
    m.project = &arena->project;
    m.mix = &board;
    m.device.init(core::ParamKind::Device);
    return m;
  }
};

MixerWorld& world() {
  static MixerWorld w;
  return w;
}

ViewState mixer_view(app::MixerPage page, int row, int col) {
  ViewState v;
  v.screen = ScreenId::Mixer;
  v.page[app::screen_index(ScreenId::Mixer)] = static_cast<std::uint8_t>(page);
  v.params.row = row;
  v.params.env = col;
  v.mixer.row = static_cast<std::uint8_t>(row);  // the Mixer's cursor is its own pair since OS-011b: the gallery's pictures put it where the state says (they had it at 0, 0 before OS-SENDFX)
  v.mixer.col = static_cast<std::uint8_t>(col);
  return v;
}

void draw(ui::Renderer& r, const Model& m, const ViewState& v) { app::render_scene(m, v, r); }

void draw_mixer_typical(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::Mixer, 0, 3)); }
void draw_mixer_pan(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::Mixer, 1, 1)); }
void draw_mixer_master(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::Mixer, 0, 16)); }

void draw_mixer_none(ui::Renderer& r, unsigned) {
  Model m{};
  m.device.init(core::ParamKind::Device);
  draw(r, m, mixer_view(app::MixerPage::Mixer, 0, 0));
}

// Sixteen tracks, every one at full level with every mark, every meter at full scale with its clip lamp, the widest numbers: the dearest the MIXER page gets.
struct WorstWorld {
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  MixBoard board;
  WorstWorld() {
    arena->project.reset();
    (void)arena->project.set_active_tracks(16);
    for (unsigned t = 0; t < 16; ++t) {
      model::TrackSlot s = *arena->project.track(t);
      s.volume = 100;
      s.pan = static_cast<std::uint8_t>(t % 2 == 0 ? 0 : 100);
      s.flags = model::kTrackMute | model::kTrackSolo;
      (void)arena->project.set_track(t, s);
    }
    model::MasterSettings ms = arena->project.master();
    ms.flags = model::kMasterReverbMute | model::kMasterDelayMute;
    (void)arena->project.set_master(ms);
    for (unsigned c = 0; c < audio::kMeterChannels; ++c) {
      feed(board, c, 1.0f, 0.9f);
    }
    for (unsigned i = 0; i < app::kMixStrips; ++i) {
      board.route[i] = audio::route::kAll & (i == audio::kInputStrip ? ~audio::route::kUsb : 0xFF);
    }
    board.in_level = 100;
  }
};

void draw_mixer_worst(ui::Renderer& r, unsigned variant) {
  static WorstWorld w;
  Model m{};
  m.project = &w.arena->project;
  m.mix = &w.board;
  m.device.init(core::ParamKind::Device);
  draw(r, m, mixer_view(app::MixerPage::Mixer, variant % 2u, static_cast<int>(variant / 2u)));
}

// The other pages over the same worlds.
void draw_routing_typical(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::Routing, 1, 1)); }
void draw_routing_returns(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::Routing, 13, 1)); }  // RET A: 12 tracks, LINE IN, then the returns
void draw_routing_out(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::Routing, 15, 0)); }

void draw_routing_worst(ui::Renderer& r, unsigned variant) {
  static WorstWorld w;
  Model m{};
  m.project = &w.arena->project;
  m.mix = &w.board;
  m.device.init(core::ParamKind::Device);
  w.board.ret_route[0] = 3;
  w.board.ret_route[1] = 3;
  w.board.out_mode = 1;
  draw(r, m, mixer_view(app::MixerPage::Routing, static_cast<int>(variant / app::kRouteCols), static_cast<int>(variant % app::kRouteCols)));
}

void draw_master_typical(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::MasterFx, 1, 0)); }
void draw_master_cue(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::MasterFx, static_cast<int>(app::kMasterCueRow), 0)); }

// OS-SENDFX: the cursor on the delay's time and on the reverb's size (the page has scrolled to keep the cursor in view)
void draw_master_delay(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::MasterFx, static_cast<int>(app::kMasterFirstFxRow + 3u), 0)); }
void draw_master_reverb(ui::Renderer& r, unsigned) { draw(r, world().model(), mixer_view(app::MixerPage::MasterFx, static_cast<int>(app::kMasterFirstFxRow + 7u), 0)); }

void draw_master_worst(ui::Renderer& r, unsigned variant) {
  static WorstWorld w;
  Model m{};
  m.project = &w.arena->project;
  m.mix = &w.board;
  m.device.init(core::ParamKind::Device);
  model::MasterSettings ms = w.arena->project.master();
  ms.volume = 100;
  ms.dry_volume = 100;
  ms.reverb_volume = 100;
  ms.delay_volume = 100;
  ms.flags = model::kMasterFlagsMask;
  (void)w.arena->project.set_master(ms);
  draw(r, m, mixer_view(app::MixerPage::MasterFx, static_cast<int>(variant), 0));
}

// The stem map: the factory P10 map (typical), every slot with every source (worst, P11: eleven slots), no map.
struct StemWorld {
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  MixBoard board;
  audio::StemMap stems;
  StemWorld() {
    arena->project.reset();
    board.stems = &stems;
  }
};

void draw_stems_typical(ui::Renderer& r, unsigned) {
  static StemWorld w;
  Model m{};
  m.project = &w.arena->project;
  m.mix = &w.board;
  m.device.init(core::ParamKind::Device);
  (void)w.stems.set_slot(2, w.stems.slot(2) | (1u << 8));  // S3 = T3 + T9: a slot with two sources
  draw(r, m, mixer_view(app::MixerPage::Stems, 3, 8));  // row 0 is OUTPUTS (OS-USBOUTS), so S3 is row 3
}

void draw_stems_outputs(ui::Renderer& r, unsigned) {
  static StemWorld w;
  Model m{};
  m.project = &w.arena->project;
  m.mix = &w.board;
  m.device.init(core::ParamKind::Device);
  ViewState v = mixer_view(app::MixerPage::Stems, 0, 0);
  v.mixer.outputs_pending = 6;  // OS-USBOUTS: the cursor on OUTPUTS with 6 chosen and not yet applied (F6 twice)
  draw(r, m, v);
}

void draw_stems_none(ui::Renderer& r, unsigned) {
  Model m = world().model();
  MixBoard board;  // a board without a map
  m.mix = &board;
  draw(r, m, mixer_view(app::MixerPage::Stems, 0, 0));
}

void draw_stems_worst(ui::Renderer& r, unsigned variant) {
  static StemWorld w;
  (void)w.stems.reset(usb::Profile::P11);
  for (unsigned s = 0; s < w.stems.channels(); ++s) {
    (void)w.stems.set_slot(s, 0xFFFFu);
  }
  Model m{};
  m.project = &w.arena->project;
  m.mix = &w.board;
  m.device.init(core::ParamKind::Device);
  draw(r, m, mixer_view(app::MixerPage::Stems, static_cast<int>(variant / app::kStemSources), static_cast<int>(variant % app::kStemSources)));
}

// ---- Perform -------------------------------------------------------------------------------------------------------------------------------------------------

// A bank with something in it: the filters and the stutter latched on three tracks, a value held. A CommandSink that takes everything (the screen shows state, not commands).
struct DiscardSink final : seq::CommandSink {
  bool push(const seq::Command&) noexcept override { return true; }
};

struct PerformWorld {
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  app::PerformBoard board;
  DiscardSink sink;
  PerformWorld() {
    arena->project.reset();
    model::TrackSlot t = *arena->project.track(2);
    t.flags = model::kTrackMute;
    (void)arena->project.set_track(2, t);
    board.out = &sink;
  }
  seq::PerformBank& bank() { return board.bank; }
  Model model() const {
    Model m{};
    m.project = &arena->project;
    m.perform = &board;
    m.device.init(core::ParamKind::Device);
    return m;
  }
  // Stores `value` in row `row` of `slot` through the pad and the encoder, as a user in EDIT does.
  void store(unsigned slot, unsigned row, int value) {
    const bool was = bank().edit();
    bank().set_edit(true);
    bank().pad_down(row * seq::kPerfSlots + slot, sink);
    bank().turn(value, sink);
    bank().pad_up(row * seq::kPerfSlots + slot, sink);
    bank().set_edit(was);
  }
};

PerformWorld& perform_world() {
  static PerformWorld w;
  static bool built = false;
  if (!built) {
    built = true;
    w.store(0, 1, 30);    // volume +30
    w.store(0, 2, -45);
    w.store(3, 1, 20);    // low-pass +20
    w.store(3, 2, -60);
    w.store(4, 3, 100);   // high-pass +100
    w.store(8, 1, -100);  // sample position
    w.store(11, 1, 23);   // volume LFO speed
    w.bank().toggle_track(0, w.sink);
    w.bank().toggle_track(1, w.sink);
    w.bank().toggle_track(2, w.sink);
    w.bank().pad_down(seq::kPerfSlots * 1 + 3, w.sink);  // slot 3, row 1: the low-pass is on at +20 (and ends the high-pass, which it blocks)
    w.bank().pad_up(seq::kPerfSlots * 1 + 3, w.sink);
    w.bank().pad_down(seq::kPerfSlots * 2 + 0, w.sink);  // slot 0, row 2: the volume at -45, and the pad is still down
  }
  return w;
}

ViewState perform_view() {
  ViewState v;
  v.screen = ScreenId::Perform;
  return v;
}

void draw_perform_typical(ui::Renderer& r, unsigned) { draw(r, perform_world().model(), perform_view()); }

void draw_perform_edit(ui::Renderer& r, unsigned) {
  PerformWorld& w = perform_world();
  w.bank().set_edit(true);
  while (w.bank().edit_slot() < 5) {
    (void)w.bank().move_edit(1);
  }
  draw(r, w.model(), perform_view());
  w.bank().set_edit(false);
  (void)w.bank().move_edit(-100);
}

void draw_perform_none(ui::Renderer& r, unsigned) {
  Model m{};
  m.device.init(core::ParamKind::Device);
  draw(r, m, perform_view());
}

// The engine is wired and no track is on: the info line says so (OS-BUG-PERFINFO: it ran into the hint on its right).
void draw_perform_off(ui::Renderer& r, unsigned) {
  static PerformWorld w;
  draw(r, w.model(), perform_view());
}

// Every slot with the widest name and value it can have, EDIT on, all eight tracks affected and muted.
void draw_perform_worst(ui::Renderer& r, unsigned variant) {
  static PerformWorld w;
  static bool built = false;
  if (!built) {
    built = true;
    for (unsigned s = 0; s < seq::kPerfSlots; ++s) {
      while (w.bank().fx(s) != static_cast<seq::PerfFx>(seq::kPerfFxCount - 1u - s)) {
        w.bank().change_fx(s, 1, w.sink);
      }
      for (unsigned row = 1; row < seq::kPerfRows; ++row) {
        w.store(s, row, -100);
      }
    }
    for (unsigned t = 0; t < seq::kPerfTracks; ++t) {
      w.bank().toggle_track(t, w.sink);
      model::TrackSlot ts = *w.arena->project.track(t);
      ts.flags = model::kTrackMute;
      (void)w.arena->project.set_track(t, ts);
    }
  }
  w.bank().set_edit(true);
  (void)w.bank().move_edit(-100);
  (void)w.bank().move_edit(static_cast<int>(variant));
  draw(r, w.model(), perform_view());
}

}  // namespace

const ScreenState kMixerStates[18] = {
    {"typical", true, 1, kBudgetWords, draw_mixer_typical},
    {"pan", true, 1, kBudgetWords, draw_mixer_pan},
    {"master", true, 1, kBudgetWords, draw_mixer_master},
    {"none", true, 1, kBudgetWords, draw_mixer_none},
    {"worst", true, 2u * 21u, kBudgetWords, draw_mixer_worst},
    {"routing", true, 1, kBudgetWords, draw_routing_typical},
    {"routing_returns", true, 1, kBudgetWords, draw_routing_returns},
    {"routing_out", true, 1, kBudgetWords, draw_routing_out},
    {"routing_worst", true, 20u * app::kRouteCols, kBudgetWords, draw_routing_worst},
    {"masterfx", true, 1, kBudgetWords, draw_master_typical},
    {"masterfx_cue", true, 1, kBudgetWords, draw_master_cue},
    {"masterfx_delay", true, 1, kBudgetWords, draw_master_delay},
    {"masterfx_reverb", true, 1, kBudgetWords, draw_master_reverb},
    {"masterfx_worst", true, app::kMasterFxRows, kBudgetWords, draw_master_worst},
    {"stems", true, 1, kBudgetWords, draw_stems_typical},
    {"stems_none", true, 1, kBudgetWords, draw_stems_none},
    {"stems_outputs", true, 1, kBudgetWords, draw_stems_outputs},
    {"stems_worst", true, 12u *app::kStemSources, kBudgetWords, draw_stems_worst},
};


const ScreenState kPerformStates[5] = {
    {"typical", true, 1, kBudgetWords, draw_perform_typical},
    {"edit", true, 1, kBudgetWords, draw_perform_edit},
    {"none", true, 1, kBudgetWords, draw_perform_none},
    {"off", true, 1, kBudgetWords, draw_perform_off},
    {"worst", true, seq::kPerfSlots, kBudgetWords, draw_perform_worst},
};

}  // namespace o3os::sim
