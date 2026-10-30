// SPDX-License-Identifier: GPL-3.0-or-later
// The sample screens' gallery and budget states (OS-011a): the world they draw over (a sample instrument with a break in the sample arena and the peak cache
// of it) and the Sample load screen's states. The listing is read by the real scan job (app::DirScan) from an in-memory card (platform/host FakeStorage), as
// the File screen's states are, so what is measured and shown is what the device draws for a card like it.
#include "screens_app.h"

#include <o3os/app/dir_scan.h>
#include <o3os/app/firmware_view.h>
#include <o3os/app/mix_board.h>
#include <o3os/audio/stem_map.h>
#include <o3os/app/screens/config_screen.h>
#include <o3os/app/screens/sample_edit_screen.h>
#include <o3os/app/screens/sample_load_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/wave_screen.h>
#include <o3os/app/wave_math.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fake_storage.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace o3os::sim {

using app::DirScan;
using app::Model;
using app::OverlayId;
using app::ScreenId;
using app::ViewState;
using platform::host::FakeStorage;

namespace {

constexpr std::uint32_t kFrames = 132300;      // three seconds at 44.1 kHz
constexpr std::uint32_t kBigSlice = 1000000;   // a budget that buys a whole step of the scan

// A break of eight hits: kicks and snares on the beat, hats between. Deterministic (an LCG for the noise).
void make_break(std::int16_t* out) {
  std::uint32_t seed = 12345u;
  const std::uint32_t hit = kFrames / 8u;
  for (std::uint32_t h = 0; h < 8u; ++h) {
    const bool kick = (h % 2u) == 0u;
    for (std::uint32_t n = 0; n < hit; ++n) {
      const float t = static_cast<float>(n) / static_cast<float>(hit);
      const float env = (1.0f - t) * (1.0f - t) * (1.0f - t);
      seed = seed * 1664525u + 1013904223u;
      const float noise = static_cast<float>(static_cast<int>(seed >> 16) - 32768) / 32768.0f;
      const float tone = std::sin(6.2831853f * (kick ? 55.0f + 120.0f * std::exp(-static_cast<float>(n) / 900.0f) : 180.0f) * static_cast<float>(n) / 44100.0f);
      const float v = kick ? 0.85f * tone * env : (0.55f * noise + 0.3f * tone) * env;
      out[h * hit + n] = static_cast<std::int16_t>(v * 30000.0f);
    }
  }
  for (std::uint32_t i = 8u * hit; i < kFrames; ++i) {
    out[i] = 0;
  }
}

}  // namespace

SampleWorld::SampleWorld() : memory(2u * 1024u * 1024u / 2u + 8u), peak_store(ui::plot::PeakCache::elements_required(kFrames, 1) + 8u) {
  arena->project.reset();
  bank.init(*voices, *arena, *mem, memory.data(), static_cast<std::uint32_t>(memory.size() - 8u), peak_store.data(), peak_store.size());
  std::uint32_t cap = 0;
  std::int16_t* pcm = bank.reserve(cap);
  make_break(pcm);
  frames = kFrames;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  std::strcpy(in.name, "BREAK_A");
  in.sampler.play_mode = 5;  // beat slice
  in.sampler.slice_count = 16;
  for (unsigned i = 0; i < 16u; ++i) {
    in.sampler.slices[i] = static_cast<std::uint16_t>(i * 65535u / 16u);
  }
  in.sampler.loop_start = 8000;
  in.sampler.loop_end = 52000;
  (void)arena->project.set_instrument(kSlot, in);
  (void)bank.commit_load(kSlot, pcm, kFrames);
  (void)bank.poll(0, true);  // the handover is done: nothing waits (a state that wants it pending marks it again)
  (void)bank.poll(app::SampleBank::kSettleMs, true);
  cache.init(peak_store.data(), peak_store.size(), pcm, kFrames, 1);
  cache.build();
}

Model SampleWorld::model() const {
  Model m{};
  m.project = &arena->project;
  m.samples = &bank;
  m.peaks = &cache;
  m.device.init(core::ParamKind::Device);
  return m;
}

SampleWorld& sample_world() {
  static SampleWorld world;
  return world;
}

namespace {

// ---- Sample load ------------------------------------------------------------------------------------------------------------------

void add_wav(FakeStorage& card, const std::string& path) { card.add_file(path, 400000); }

struct LoadScene {
  FakeStorage card;
  DirScan scan;
  Model model;
  ViewState view;

  void show(int cursor, bool with_world = true) {
    model = with_world ? sample_world().model() : Model{};
    model.files = &scan;
    view.screen = ScreenId::SampleLoad;
    view.instrument = static_cast<int>(SampleWorld::kSlot) + 1;
    (void)view.wav.list.set(cursor, scan.index());
  }
  void read(unsigned steps) {
    (void)scan.begin(card, view.wav.folder, app::kWavFilter);
    for (unsigned i = 0; (steps == 0u || i < steps) && scan.step(kBigSlice) == core::JobStatus::Running; ++i) {
    }
  }
};

void draw_load(ui::Renderer& r, const LoadScene& s) { app::render_scene(s.model, s.view, r); }

// The maintainer's kind of card: folders and files of every habit in the naming, a folder called like a file, a text file, what macOS leaves.
LoadScene& load_typical() {
  static LoadScene s;
  static bool built = false;
  if (!built) {
    built = true;
    static const char* const kFiles[] = {"808 kick.wav", "break_a.wav", "Break_B.WAV", "choir pad.wav", "hat_closed.wav", "hat_open.wav", "kick-01.wav",
                                         "lead stab.wav", "Loop 120bpm.wav", "snare_tight.wav", "sub bass.wav", "vox chop.wav"};
    for (const char* f : kFiles) {
      add_wav(s.card, f);
    }
    s.card.add_dir("Breaks");
    s.card.add_dir("Drum Kits");
    s.card.add_dir("snare.wav");  // a folder with a file's name
    s.card.add_file("readme.txt", 120);
    s.card.add_file("._kick-01.wav", 4096);
    s.read(0);
    s.show(5);
    s.view.wav.set_mark(4, true);  // OS-AUDIT-A7 (SLD-MULTI): three files marked for the multi-load (the folders come first), the cursor on one of them
    s.view.wav.set_mark(5, true);
    s.view.wav.set_mark(7, true);
  }
  return s;
}

// 300 files whose names are all longer than the column (cut to 31 characters and a '~'), some with UTF-8: the list fills up at 256 and says so; the cursor on
// the last row of the last window.
LoadScene& load_worst() {
  static LoadScene s;
  static bool built = false;
  if (!built) {
    built = true;
    for (unsigned i = 0; i < 300; ++i) {
      std::string name = "Sample-" + std::to_string(1000 + i) + "-abcdefghijklmnopqrstuvwxyz";
      if (i % 3 == 0) {
        name += "-caf\xC3\xA9";
      }
      add_wav(s.card, name + ".wav");
    }
    s.read(0);
    s.show(static_cast<int>(s.scan.index().count()) - 1);
  }
  return s;
}

LoadScene& load_empty() {
  static LoadScene s;
  static bool built = false;
  if (!built) {
    built = true;
    s.card.add_file("notes.txt", 10);
    s.read(0);
    s.show(0, false);  // no sample memory either: the card says so
  }
  return s;
}

LoadScene& load_no_card() {
  static LoadScene s;
  static bool built = false;
  if (!built) {
    built = true;
    s.show(0);
  }
  return s;
}

// OS-SAMPFMT: the extensions the list takes besides .wav: AIFF (.aif .aiff .aifc), IFF 8SVX (.8svx .svx .iff) and Surge's .wt, in every case; two folders on
// top (folders first); the cursor on the first file, an 8SVX.
LoadScene& load_formats() {
  static LoadScene s;
  static bool built = false;
  if (!built) {
    built = true;
    static const char* const kFiles[] = {"amiga bass.8svx", "chord.AIFC", "cymbal.svx", "kick.wav", "organ.IFF", "pad.aif", "pad2.AIFF", "saw.wt", "tom.wav"};
    for (const char* f : kFiles) {
      add_wav(s.card, f);
    }
    s.card.add_dir("Breaks");
    s.card.add_dir("Drum Kits");
    s.card.add_file("notes.txt", 120);
    s.card.add_file("take.mp3", 4096);
    s.read(0);
    s.show(2);
  }
  return s;
}

// OS-LOADCH: the headers of a stereo and a mono WAV and AIFF (the scan reads them: DirScan::probe_stereo), as bytes the in-memory card hands back.
std::vector<std::uint8_t> header_wav(unsigned channels) {
  std::vector<std::uint8_t> v = {'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, static_cast<std::uint8_t>(channels), 0,
                                 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
  return v;
}
std::vector<std::uint8_t> header_aiff(unsigned channels) {
  std::vector<std::uint8_t> v = {'F', 'O', 'R', 'M', 0, 0, 0, 46, 'A', 'I', 'F', 'F', 'C', 'O', 'M', 'M', 0, 0, 0, 18, 0, static_cast<std::uint8_t>(channels), 0, 0, 0, 0, 0, 16,
                                 0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0, 'S', 'S', 'N', 'D', 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0};
  return v;
}

// OS-LOADCH: the list tags a stereo WAV or AIFF "ST"; the cursor is on one, and F8 reads CH MIX, the default (the choice is the screen's, WavView::channel).
LoadScene& load_stereo() {
  static LoadScene s;
  static bool built = false;
  if (!built) {
    built = true;
    s.card.add_file("amiga bass.8svx", 4096);
    s.card.add_file("hat mono.wav", header_wav(1));
    s.card.add_file("kick stereo.wav", header_wav(2));
    s.card.add_file("pad mono.aif", header_aiff(1));
    s.card.add_file("pad stereo.aif", header_aiff(2));
    s.card.add_file("pad stereo.aiff", header_aiff(2));
    s.read(0);
    s.show(2);
  }
  return s;
}

void draw_load_typical(ui::Renderer& r, unsigned) { draw_load(r, load_typical()); }
void draw_load_worst(ui::Renderer& r, unsigned) { draw_load(r, load_worst()); }
void draw_load_empty(ui::Renderer& r, unsigned) { draw_load(r, load_empty()); }
void draw_load_no_card(ui::Renderer& r, unsigned) { draw_load(r, load_no_card()); }
void draw_load_formats(ui::Renderer& r, unsigned) { draw_load(r, load_formats()); }
void draw_load_stereo(ui::Renderer& r, unsigned) { draw_load(r, load_stereo()); }

}  // namespace

const ScreenState kSampleLoadStates[6] = {
    {"typical", true, 1, kBudgetWords, draw_load_typical},
    {"worst", true, 1, kBudgetWords, draw_load_worst},
    {"empty", true, 1, kBudgetWords, draw_load_empty},
    {"no_card", true, 1, kBudgetWords, draw_load_no_card},
    {"formats", true, 1, kBudgetWords, draw_load_formats},
    {"stereo", true, 1, kBudgetWords, draw_load_stereo},
};

namespace {

// ---- Wave / Chop ----------------------------------------------------------------------------------------------------------------------

// The world's instrument, rewritten for the state about to be drawn (a state is a pure function of its variant: each one sets what it needs).
void set_world_instrument(unsigned slices, unsigned loop_start, unsigned loop_end, unsigned start, unsigned end) {
  SampleWorld& w = sample_world();
  model::Instrument in = *w.arena->project.instrument(SampleWorld::kSlot);
  in.sampler.start = static_cast<std::uint16_t>(start);
  in.sampler.end = static_cast<std::uint16_t>(end);
  in.sampler.loop_start = static_cast<std::uint16_t>(loop_start);
  in.sampler.loop_end = static_cast<std::uint16_t>(loop_end);
  in.sampler.play_mode = slices != 0u ? 5u : 1u;
  (void)app::slice_even(in.sampler.slices, in.sampler.slice_count, slices == 0u ? 1u : slices);
  if (slices == 0u) {
    in.sampler.slice_count = 0;
  }
  (void)w.arena->project.set_instrument(SampleWorld::kSlot, in);
}

ViewState wave_view(unsigned page) {
  ViewState v;
  v.screen = ScreenId::Wave;
  v.instrument = static_cast<int>(SampleWorld::kSlot) + 1;
  v.page[app::screen_index(ScreenId::Wave)] = static_cast<std::uint8_t>(page);
  return v;
}

void draw_wave_scene(ui::Renderer& r, const Model& m, const ViewState& v) { app::render_scene(m, v, r); }

void draw_wave_typical(ui::Renderer& r, unsigned) {
  set_world_instrument(0, 9000, 52000, 2000, 62000);
  ViewState v = wave_view(0);
  v.wave.point = 1;  // LOOP START
  draw_wave_scene(r, sample_world().model(), v);
}

void draw_wave_zoom(ui::Renderer& r, unsigned) {
  set_world_instrument(0, 9000, 52000, 2000, 62000);
  ViewState v = wave_view(0);
  v.wave.point = 1;
  v.wave.first = 8000;
  v.wave.span = 24000;
  draw_wave_scene(r, sample_world().model(), v);
}

void draw_chop(ui::Renderer& r, unsigned) {
  set_world_instrument(16, 0, 65535, 0, 65535);
  ViewState v = wave_view(1);
  v.wave.slice = 5;
  draw_wave_scene(r, sample_world().model(), v);
}

// 48 slices over the whole sample: every slice line and every number, the 48 pads filled.
void draw_chop_worst(ui::Renderer& r, unsigned) {
  set_world_instrument(48, 0, 65535, 0, 65535);
  ViewState v = wave_view(1);
  v.wave.slice = 47;
  v.wave.even = 48;
  draw_wave_scene(r, sample_world().model(), v);
}

void draw_wave_none(ui::Renderer& r, unsigned) {
  ViewState v = wave_view(0);
  v.instrument = 9;  // no instrument there
  draw_wave_scene(r, sample_world().model(), v);
}

void draw_wave_building(ui::Renderer& r, unsigned) {
  set_world_instrument(0, 9000, 52000, 2000, 62000);
  Model m = sample_world().model();
  m.peaks = nullptr;  // the cache is not the sample's yet
  draw_wave_scene(r, m, wave_view(0));
}

ViewState sedit_view() {
  ViewState v;
  v.screen = ScreenId::SampleEdit;
  v.instrument = static_cast<int>(SampleWorld::kSlot) + 1;
  return v;
}

void draw_sedit_typical(ui::Renderer& r, unsigned) {
  ViewState v = sedit_view();
  v.sedit.a = 30000;
  v.sedit.b = 80000;
  v.sedit.edge = 1;
  draw_wave_scene(r, sample_world().model(), v);
}

// The Shift row on the same selection (Shift held: the key row shows the second layer, as the manual's Sample edit chapter says).
void draw_sedit_shift(ui::Renderer& r, unsigned) {
  ViewState v = sedit_view();
  v.sedit.a = 30000;
  v.sedit.b = 80000;
  v.sedit.edge = 1;
  v.shift = true;
  draw_wave_scene(r, sample_world().model(), v);
}

void draw_sedit_zoom(ui::Renderer& r, unsigned) {
  ViewState v = sedit_view();
  v.sedit.a = 30000;
  v.sedit.b = 80000;
  v.sedit.first = 20000;
  v.sedit.span = 20000;
  draw_wave_scene(r, sample_world().model(), v);
}

void draw_sedit_none(ui::Renderer& r, unsigned) {
  ViewState v = sedit_view();
  v.instrument = 9;
  draw_wave_scene(r, sample_world().model(), v);
}

// ---- Config -----------------------------------------------------------------------------------------------------------------------------

// A card of the maintainer's kind, in memory (FakeStorage: a FAT32 volume of 3,725 MB in 32 KB clusters is what its volume() says).
struct ConfigScene {
  FakeStorage card;
  Model model;
  ViewState view;

  void show(unsigned page, bool with_card) {
    model = sample_world().model();
    model.device.init(core::ParamKind::Device);
    (void)model.device.set(model::device_setting::ClockMode, 1.0f);
    (void)model.device.set(model::device_setting::MidiInChannel, 10.0f);
    model.product = "Track3r Community OS";
    model.build_id = "0123456789ab";
    card.set_present(with_card);
    model.card = &card;
    view = ViewState{};
    view.screen = ScreenId::Config;
    view.page[app::screen_index(ScreenId::Config)] = static_cast<std::uint8_t>(page);
    view.params.row = page == 0u ? 3 : 1;
  }
  // OS-USBAUDIO: the USB page with USB AUDIO and USB RETURN both waiting to be applied (Off, starred), the cursor on USB AUDIO; `armed`: the first press of APPLY has been made.
  void show_usb_pending(bool armed) {
    show(4, true);
    view.params.row = 0;
    (void)view.params.set_pending(core::param_id(core::ParamKind::Device, 0, model::device_setting::UsbAudio), 0.0f);
    (void)view.params.set_pending(core::param_id(core::ParamKind::Device, 0, model::device_setting::UsbReturn), 0.0f);
    view.params.armed = armed;
  }
  // OS-USBOUTS O3: the USB page, the cursor on USB RETURN (On), with a stem map of 10 channels: the row reads "On (10 OUT: off)", dim.
  void show_usb_ten() {
    show(4, true);
    view.params.row = 1;
    board.stems = &stems;
    model.mix = &board;
  }
  app::MixBoard board;
  audio::StemMap stems;  // the default is the 10-channel profile
  // OS-FWCONFIG: the INFO page with the firmware update dialog up, in `phase`, over the four files the simulator's `firmware on` lists (sim/src/simulator.cpp). `cursor` is the file the walk is about;
  // `fraction` (permille) is how far the check or the hand-over has read; a `failed` list carries the check's verdict on its line, as the dialog does (firmware_actions.cpp).
  void show_firmware(app::FirmwareView::Phase phase, unsigned cursor, unsigned fraction = 0, bool failed = false) {
    using FV = app::FirmwareView;
    show(3, true);
    model.firmware = true;
    view.overlay = app::OverlayId::FirmwareUpdate;
    FV& v = view.firmware;
    v.close();
    static const struct {
      const char* name;
      std::uint32_t size;
    } kFiles[4] = {{"TRACKER_1.PTF", 812345}, {"SLOW_CHECK.PTF", 799000}, {"BAD_IMAGE.PTF", 640000}, {"NEVER_ENDS.PTF", 805120}};
    for (unsigned i = 0; i < 4; ++i) {
      std::strncpy(v.files[i].name, kFiles[i].name, sizeof v.files[i].name - 1);
      v.files[i].size = kFiles[i].size;
      v.files[i].reset_target = i == 0;
    }
    v.n = 4;
    v.entries = 4;
    v.cursor = static_cast<std::uint8_t>(cursor);
    v.phase = phase;
    v.total = kFiles[cursor].size;
    v.done = static_cast<std::uint32_t>((static_cast<std::uint64_t>(v.total) * fraction) / 1000u);
    v.image = 0x3F2A0u;
    v.crc = 0x5EA1B0C4u;
    if (failed) {
      v.bad = true;
      v.msg.assign("NOT A VALID IMAGE: FLASH CONFIGURATION FIELD IS NOT THE REQUIRED ONE AT 1104");
    }
  }
  // OS-BUG-FWLIST: a folder of 19 files, the second window (entries 13..19) held, the cursor on entry 18: the list shows where it is ("FILE 18 / 19").
  void show_firmware_paged() {
    show_firmware(app::FirmwareView::Phase::List, 0);
    app::FirmwareView& v = view.firmware;
    static const char* const kNames[7] = {"TRACKER_13.PTF", "TRACKER_14.PTF", "TRACKER_15.PTF", "TRACKER_16.PTF", "TRACKER_17.PTF", "TRACKER_18.PTF", "TRACKER_19.PTF"};
    for (unsigned i = 0; i < 7; ++i) {
      std::memset(v.files[i].name, 0, sizeof v.files[i].name);
      std::strncpy(v.files[i].name, kNames[i], sizeof v.files[i].name - 1);
      v.files[i].size = 810000u - 1000u * i;
      v.files[i].reset_target = false;
    }
    v.n = 7;
    v.first = 12;
    v.entries = 19;
    v.cursor = 5;
  }
};

ConfigScene& config_scene() {
  static ConfigScene s;
  return s;
}

void draw_config(ui::Renderer& r, unsigned page, bool with_card) {
  ConfigScene& s = config_scene();
  s.show(page, with_card);
  app::render_scene(s.model, s.view, r);
}
void draw_config_midi(ui::Renderer& r, unsigned) { draw_config(r, 0, true); }
void draw_config_display(ui::Renderer& r, unsigned) { draw_config(r, 1, true); }
void draw_config_sd(ui::Renderer& r, unsigned) { draw_config(r, 2, true); }
void draw_config_sd_none(ui::Renderer& r, unsigned) { draw_config(r, 2, false); }
void draw_config_info(ui::Renderer& r, unsigned) { draw_config(r, 3, true); }
void draw_config_usb(ui::Renderer& r, unsigned) { draw_config(r, 4, true); }  // OS-USBRET: the USB page, with the USB RETURN row
void draw_config_pattern(ui::Renderer& r, unsigned) { draw_config(r, 5, true); }  // OS-LIVEREC: the PATTERN page, REC POLY and REC QUANTIZE
void draw_config_usb_pending(ui::Renderer& r, unsigned) {  // OS-USBAUDIO: two values chosen and not applied (starred)
  ConfigScene& s = config_scene();
  s.show_usb_pending(false);
  app::render_scene(s.model, s.view, r);
}
void draw_config_usb_ten(ui::Renderer& r, unsigned) {  // OS-USBOUTS O3: USB RETURN On at 10 OUT
  ConfigScene& s = config_scene();
  s.show_usb_ten();
  app::render_scene(s.model, s.view, r);
}
void draw_config_usb_armed(ui::Renderer& r, unsigned) {  // OS-USBAUDIO: the first press of APPLY: the info line says the port will reconnect
  ConfigScene& s = config_scene();
  s.show_usb_pending(true);
  app::render_scene(s.model, s.view, r);
}

// OS-FWCONFIG: the six states of the firmware update dialog (F8 UPDATE on INFO), the walk of the scenario qa_config_update.txt.
void draw_config_fw(ui::Renderer& r, app::FirmwareView::Phase phase, unsigned cursor, unsigned fraction = 0, bool failed = false) {
  ConfigScene& s = config_scene();
  s.show_firmware(phase, cursor, fraction, failed);
  app::render_scene(s.model, s.view, r);
}
void draw_config_fw_list(ui::Renderer& r, unsigned) { draw_config_fw(r, app::FirmwareView::Phase::List, 0); }
void draw_config_fw_ask(ui::Renderer& r, unsigned) { draw_config_fw(r, app::FirmwareView::Phase::AskUpdate, 0); }
void draw_config_fw_check(ui::Renderer& r, unsigned) { draw_config_fw(r, app::FirmwareView::Phase::Checking, 1, 400); }
void draw_config_fw_install(ui::Renderer& r, unsigned) { draw_config_fw(r, app::FirmwareView::Phase::AskInstall, 0); }
void draw_config_fw_installing(ui::Renderer& r, unsigned) { draw_config_fw(r, app::FirmwareView::Phase::Installing, 3, 500); }
void draw_config_fw_failed(ui::Renderer& r, unsigned) { draw_config_fw(r, app::FirmwareView::Phase::List, 2, 0, true); }
void draw_config_fw_paged(ui::Renderer& r, unsigned) {
  ConfigScene& s = config_scene();
  s.show_firmware_paged();
  app::render_scene(s.model, s.view, r);
}

}  // namespace

const ScreenState kConfigStates[17] = {
    {"midi", true, 1, kBudgetWords, draw_config_midi},
    {"display", true, 1, kBudgetWords, draw_config_display},
    {"sd", true, 1, kBudgetWords, draw_config_sd},
    {"sd_none", true, 1, kBudgetWords, draw_config_sd_none},
    {"info", true, 1, kBudgetWords, draw_config_info},
    {"usb", true, 1, kBudgetWords, draw_config_usb},
    {"pattern", true, 1, kBudgetWords, draw_config_pattern},
    {"usb_pending", true, 1, kBudgetWords, draw_config_usb_pending},
    {"usb_armed", true, 1, kBudgetWords, draw_config_usb_armed},
    {"usb_ten", true, 1, kBudgetWords, draw_config_usb_ten},
    {"fw_list", true, 1, kBudgetWords, draw_config_fw_list},
    {"fw_ask", true, 1, kBudgetWords, draw_config_fw_ask},
    {"fw_check", true, 1, kBudgetWords, draw_config_fw_check},
    {"fw_install", true, 1, kBudgetWords, draw_config_fw_install},
    {"fw_installing", true, 1, kBudgetWords, draw_config_fw_installing},
    {"fw_failed", true, 1, kBudgetWords, draw_config_fw_failed},
    {"fw_paged", true, 1, kBudgetWords, draw_config_fw_paged},
};

namespace {
}  // namespace

const ScreenState kSampleEditStates[4] = {
    {"typical", true, 1, kBudgetWords, draw_sedit_typical},
    {"zoom", true, 1, kBudgetWords, draw_sedit_zoom},
    {"none", true, 1, kBudgetWords, draw_sedit_none},
    {"shift", true, 1, kBudgetWords, draw_sedit_shift},
};

namespace {

// ---- The Params F1 source page (OS-INSTUI S3) ------------------------------------------------------------------------------------------

// The world's instrument in a play mode, with what the mode shows set: a state is a pure function of its variant.
ViewState source_view(unsigned mode, unsigned row) {
  SampleWorld& w = sample_world();
  model::Instrument in = *w.arena->project.instrument(SampleWorld::kSlot);
  in.sampler.play_mode = static_cast<std::uint8_t>(mode);
  in.sampler.sample_type = mode == 6u ? 1u : 0u;
  in.sampler.start = 2000;
  in.sampler.end = 62000;
  in.sampler.loop_start = 9000;
  in.sampler.loop_end = 52000;
  in.sampler.wavetable_window_size = 2048;
  in.sampler.wavetable_window = 20;
  in.sampler.grain_position = 30000;
  in.sampler.grain_length = 2205;
  in.sampler.grain_shape = 1;
  in.sampler.grain_type = 0;
  in.sampler.selected_slice = 5;
  in.sampler.slice_count = 0;
  if (mode == 4u || mode == 5u) {
    (void)app::slice_even(in.sampler.slices, in.sampler.slice_count, 16);
  }
  (void)w.arena->project.set_instrument(SampleWorld::kSlot, in);
  ViewState v;
  v.screen = ScreenId::Instrument;
  v.instrument = static_cast<int>(SampleWorld::kSlot) + 1;
  v.page[app::screen_index(ScreenId::Instrument)] = static_cast<std::uint8_t>(model::instrument_param::GroupSource);
  v.params.row = static_cast<int>(row);
  return v;
}

void draw_source_oneshot(ui::Renderer& r, unsigned) { draw_wave_scene(r, sample_world().model(), source_view(0, 1)); }
void draw_source_loop(ui::Renderer& r, unsigned) { draw_wave_scene(r, sample_world().model(), source_view(1, 0)); }
void draw_source_slice(ui::Renderer& r, unsigned) { draw_wave_scene(r, sample_world().model(), source_view(5, 2)); }
void draw_source_wavetable(ui::Renderer& r, unsigned) { draw_wave_scene(r, sample_world().model(), source_view(6, 1)); }
void draw_source_granular(ui::Renderer& r, unsigned) { draw_wave_scene(r, sample_world().model(), source_view(7, 2)); }

// No audio behind the instrument: the rows that need it are dimmed, with the reason on the info line.
void draw_source_noaudio(ui::Renderer& r, unsigned) {
  const ViewState v = source_view(6, 2);
  Model m = sample_world().model();
  m.samples = nullptr;
  m.peaks = nullptr;
  draw_wave_scene(r, m, v);
}

// ---- The Wave screen per play mode (OS-INSTUI S4) -----------------------------------------------------------------------------------------

ViewState wave_mode_view(unsigned mode, unsigned row) {
  ViewState v = source_view(mode, 0);  // (sets the world's instrument for the mode)
  v.screen = ScreenId::Wave;
  v.page[app::screen_index(ScreenId::Wave)] = static_cast<std::uint8_t>(mode == 4u || mode == 5u ? 1 : 0);
  v.wave.point = static_cast<std::uint8_t>(row);
  return v;
}

void draw_wave_wavetable(ui::Renderer& r, unsigned) { draw_wave_scene(r, sample_world().model(), wave_mode_view(6, 1)); }
void draw_wave_granular(ui::Renderer& r, unsigned) { draw_wave_scene(r, sample_world().model(), wave_mode_view(7, 1)); }

// The list of the eight modes over the WAVE page of a looping instrument, the cursor on WAVETABLE.
void draw_wave_modes(ui::Renderer& r, unsigned) {
  ViewState v = wave_mode_view(1, 0);
  v.overlay = OverlayId::PlayMode;
  v.wave.mode_pick = 6;
  draw_wave_scene(r, sample_world().model(), v);
}

}  // namespace

const ScreenState kSourceStates[6] = {
    {"oneshot", true, 1, kBudgetWords, draw_source_oneshot},
    {"loop", true, 1, kBudgetWords, draw_source_loop},
    {"slice", true, 1, kBudgetWords, draw_source_slice},
    {"wavetable", true, 1, kBudgetWords, draw_source_wavetable},
    {"granular", true, 1, kBudgetWords, draw_source_granular},
    {"noaudio", true, 1, kBudgetWords, draw_source_noaudio},
};

const ScreenState kWaveStates[9] = {
    {"wavetable", true, 1, kBudgetWords, draw_wave_wavetable},
    {"granular", true, 1, kBudgetWords, draw_wave_granular},
    {"modes", true, 1, kBudgetWords, draw_wave_modes},
    {"typical", true, 1, kBudgetWords, draw_wave_typical},
    {"zoom", true, 1, kBudgetWords, draw_wave_zoom},
    {"chop", true, 1, kBudgetWords, draw_chop},
    {"chop_worst", true, 1, kBudgetWords, draw_chop_worst},
    {"none", true, 1, kBudgetWords, draw_wave_none},
    {"building", true, 1, kBudgetWords, draw_wave_building},
};

}  // namespace o3os::sim
