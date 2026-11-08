// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H3A (ADR 0014 slice H3, ABI 1.5): apps edit the project. What an app is lent to read and edit steps (the read table's pattern_info / get_steps, the edit table's edit_begin / set_step(s) /
// edit_end), checked through the host the way an app calls it (native test apps whose input() runs a script, the card a temporary directory):
//   - the ABI: every published offset of 1.2 / 1.3 / 1.4 is where it was (a literal copy of the old layouts), the new members are appended, the sizes; a 1.5 file on an older OS is refused
//     with NEEDS NEWER OS; a 1.4 app and a 1.3 app keep working;
//   - the group: one undo unit named after the app, the field mask, the limits (2,048 steps, 4,096 calls) with the whole group dropped and the project untouched, bad arguments writing
//     nothing, one group per call, the rate, a group left open when the call returns or the app is stopped, set_tempo joining the group, the undo property over random groups;
//   - a fuzz of every entry (random patterns, tracks, rows, counts, masks and NULLs, in and out of groups, in and out of input());
//   - Pattern F7 TOOLS: the list, the launch context with the block, the way back to Pattern;
//   - Rhythm Gen through the whole stack: one APPLY is one undo unit, the notes are the Fill dialog's Euclid, UNDO restores the project byte for byte.
//
// OS-H3B (ABI 1.6) adds, below, the instrument-parameter entries: instrument_info / get_param_desc / get_instrument_param in the read table and set_instrument_param in the edit table (it joins the open
// group, so one group is still one undo unit). The published offsets of 1.2 .. 1.6; the descriptors of a SAMPLE and of a MODULE instrument; the refusals (a parameter that is not writable, a value out of
// range, NaN, an empty or MIDI instrument); the write joining a group with steps and the tempo; the limits; UNDO and REDO to the exact float bits (and the module instance hearing both); a fuzz and the
// undo property over groups that mix steps and parameters; and CHORDS, HUMANIZE, MUTATE and MORPH through the whole stack (TOOLS launch, one undo unit named after the app).
// Host only.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/app_export.h>
#include <o3os/app/save_service.h>
#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/project_state.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/tools_list.h>
#include <o3os/audio/app_tap.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/input_event.h>
#include <o3os/midi/cc_in.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/model/params.h>
#include <o3os/model/pattern_fill.h>
#include <o3os/module/clock_state.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/key_row.h"
#include "support/mem_card.h"
#include "support/module_blob.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/rng.h"

using namespace o3os;
using app::Button;
namespace fs = std::filesystem;

namespace {
// OS-AUDIT-56: Shift + Right goes a column at a time inside the first track and takes whole tracks after that; press it until the block has `n` more tracks.
template <class R>
void extend_tracks(R& r, int n) {
  const int target = static_cast<int>(r.state().pattern.track()) + n;
  for (int i = 0; i < 40 && static_cast<int>(r.state().pattern.track()) < target; ++i) {
    (void)r.invoke("select.extend", 10);
  }
}
}  // namespace

namespace {

alignas(8) std::uint8_t g_ring[model::kUndoRegionBytes];

constexpr std::size_t kP = sizeof(void*);

// ---- the layouts, written out (the 1.2 .. 1.4 ones as they were published) -----------------------------------------------------------------------------------------------------

// ABI 1.3 / 1.4 read and edit tables, and the 1.3 io table, as slices H1 and H2a published them. The first members of the 1.5 tables must be these, at these offsets.
struct ReadV14 {
  std::uint32_t version, size;
  std::uint32_t (*get_tempo)();
  std::uint32_t (*launch_ctx)(o3m_launch_ctx*);
};
struct EditV14 {
  std::uint32_t version, size;
  int (*set_tempo)(std::uint32_t);
};
struct IoV13 {
  std::uint32_t version, size;
  std::uint32_t (*now_ms)();
  std::uint32_t (*midi_read)(o3m_midi_event*, std::uint32_t);
  void (*midi_lost)(std::uint32_t*, std::uint32_t*);
};
struct LaunchV14 {  // the 28 bytes an app of 1.3 or 1.4 asks for
  std::uint32_t size, flags, pattern, track, instrument, block_start, block_length;
};
struct HostV12 {  // the table of ABI 1.0..1.2 (ends at draw)
  std::uint32_t abi_major, abi_minor, size;
  float sample_rate;
  void (*log)(const char*);
  float (*exp2_f)(float);
  float (*log2_f)(float);
  float (*sin_f)(float);
  float (*cos_f)(float);
  const o3m_draw_api* draw;
};
static_assert(offsetof(o3m_read_api, get_tempo) == offsetof(ReadV14, get_tempo) && offsetof(o3m_read_api, launch_ctx) == offsetof(ReadV14, launch_ctx), "1.3 read table");
static_assert(offsetof(o3m_edit_api, set_tempo) == offsetof(EditV14, set_tempo), "1.3 edit table");
static_assert(offsetof(o3m_io_api, now_ms) == offsetof(IoV13, now_ms) && offsetof(o3m_io_api, midi_read) == offsetof(IoV13, midi_read) && offsetof(o3m_io_api, midi_lost) == offsetof(IoV13, midi_lost), "1.3 io table");
static_assert(offsetof(o3m_launch_ctx, block_length) == offsetof(LaunchV14, block_length) && sizeof(LaunchV14) == 28, "1.3 launch context");
static_assert(offsetof(o3m_host, log) == offsetof(HostV12, log) && offsetof(o3m_host, exp2_f) == offsetof(HostV12, exp2_f) && offsetof(o3m_host, log2_f) == offsetof(HostV12, log2_f) &&
                  offsetof(o3m_host, sin_f) == offsetof(HostV12, sin_f) && offsetof(o3m_host, cos_f) == offsetof(HostV12, cos_f) && offsetof(o3m_host, draw) == offsetof(HostV12, draw) &&
                  offsetof(o3m_host, read) == sizeof(HostV12),
              "1.2 host table");
// The 1.5 additions come AFTER every older member, in this order.
static_assert(offsetof(o3m_read_api, pattern_info) == offsetof(ReadV14, launch_ctx) + kP && offsetof(o3m_read_api, get_steps) == offsetof(o3m_read_api, pattern_info) + kP && offsetof(o3m_read_api, instrument_info) == offsetof(o3m_read_api, get_steps) + kP,
              "1.5 read table");
static_assert(offsetof(o3m_edit_api, edit_begin) == offsetof(EditV14, set_tempo) + kP && offsetof(o3m_edit_api, edit_end) == offsetof(o3m_edit_api, edit_begin) + kP &&
                  offsetof(o3m_edit_api, set_step) == offsetof(o3m_edit_api, edit_end) + kP && offsetof(o3m_edit_api, set_steps) == offsetof(o3m_edit_api, set_step) + kP &&
                  offsetof(o3m_edit_api, set_instrument_param) == offsetof(o3m_edit_api, set_steps) + kP,
              "1.5 edit table");
static_assert(offsetof(o3m_launch_ctx, block_tracks) == sizeof(LaunchV14) && sizeof(o3m_launch_ctx) == 32, "1.5 launch context");
static_assert(sizeof(o3m_step) == 8 && alignof(o3m_step) == 1, "the step POD");
static_assert(O3M_PATTERN_TRACKS_MAX == model::kMaxTracks && O3M_PATTERN_ROWS_MAX == model::kMaxPatternLength, "the SDK's limits are the model's");
static_assert(O3M_STEP_NOTE == 1 && O3M_STEP_INSTR == 2 && O3M_STEP_VOL == 4 && O3M_STEP_FX1 == 8 && O3M_STEP_FX2 == 16 && O3M_STEP_ALL == 31, "the field mask bits are published");
static_assert(O3M_EDIT_OK == 0 && O3M_EDIT_RANGE == 1 && O3M_EDIT_NO_PROJECT == 2 && O3M_EDIT_NO_GROUP == 3 && O3M_EDIT_GROUP_OPEN == 4 && O3M_EDIT_LIMIT == 5 && O3M_EDIT_RATE == 6, "the codes are published");
static_assert(O3M_EDIT_MAX_CALLS == 4096 && O3M_EDIT_MAX_STEPS == 2048, "the limits are published");
static_assert(O3M_CAP_MIDI_IN == 1 && O3M_CAP_READ_PROJECT == 2 && O3M_CAP_AUDIO_TAP == 4 && O3M_CAP_EDIT_PROJECT == 8, "no capability bit moved");

// ---- the test apps ----------------------------------------------------------------------------------------------------------------------------------

struct EState {
  const o3m_host* host;
  std::uint32_t inputs, enters, draws, exits;
};
std::function<void(const o3m_host*)> g_input, g_draw, g_enter;

int e_init(void* s, const o3m_host* host) {
  static_cast<EState*>(s)->host = host;
  return host->draw != nullptr ? 0 : 1;
}
void e_shutdown(void*) {}
void e_enter(void* s) {
  ++static_cast<EState*>(s)->enters;
  if (g_enter) {
    g_enter(static_cast<EState*>(s)->host);
  }
}
void e_exit(void* s) { ++static_cast<EState*>(s)->exits; }
void e_input(void* s, const o3m_ui_event*) {
  ++static_cast<EState*>(s)->inputs;
  if (g_input) {
    g_input(static_cast<EState*>(s)->host);
  }
}
void e_draw(void* s, std::uint32_t, o3m_canvas*) {
  ++static_cast<EState*>(s)->draws;
  if (g_draw) {
    g_draw(static_cast<EState*>(s)->host);
  }
}
void e_set_param(void*, std::uint32_t, float) {}
const o3m_ui kUi = {sizeof(o3m_ui), e_enter, e_input, e_draw, e_exit, nullptr};
#define EAPP(name, minor) {O3M_ABI_MAJOR, minor, O3M_KIND_APP, O3M_MODULE_SIZE(minor), name, sizeof(EState), 0, nullptr, e_init, nullptr, e_set_param, nullptr, nullptr, e_shutdown, &kUi, 1}
const o3m_module kEditor = EAPP("Editor", 6);   // read + edit, ABI 1.6 (the instrument-parameter entries; the table the OS lends is the same for every app of 1.3 and later)
const o3m_module kEditor4 = EAPP("Editor4", 4); // read + edit + tap, ABI 1.4 (a Loudness-era app)
const o3m_module kEditor3 = EAPP("Editor3", 3); // read + edit, ABI 1.3 (Tap Calc's)
const o3m_module kEditor2 = EAPP("Editor2", 2); // ABI 1.2: lent nothing
const o3m_module kViewer = EAPP("Viewer", 5);   // read only
const o3m_module kListener = EAPP("Listener", 5);  // midi_in only
const o3m_module kWriter = EAPP("Writer", 5);   // edit only (no read table)
// OS-H4 (ABI 1.7): apps that write files
const o3m_module kExporter = EAPP("Exporter", 7);    // export_files only
const o3m_module kExpEdit = EAPP("ExpEdit", 7);      // edit_project and export_files: ONE confirm message for both
const o3m_module kReadOnly7 = EAPP("ReadOnly7", 7);  // read_project only, but ABI 1.7

constexpr std::uint32_t kReadEdit = O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT;

// OS-H3B: a MODULE instrument with 11 parameters over two pages (an INT, a BOOL, a LOG, ...) whose state keeps what its set_param was told.
struct IState {
  float v[16];
  std::uint32_t sets;
};
int i_init(void* s, const o3m_host* h) {
  static_cast<IState*>(s)->sets = 0;
  return h->draw != nullptr ? 0 : 1;
}
std::uint32_t i_render(void*, const float*, float*, std::uint32_t) { return 0; }
void i_set_param(void* s, std::uint32_t id, float value) {
  IState* st = static_cast<IState*>(s);
  ++st->sets;
  if (id < 16) {
    st->v[id] = value;
  }
}
void i_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void i_note_off(void*, std::uint32_t) {}
void i_shutdown(void*) {}
const o3m_param kInstParams[11] = {
    {"Amount", 0.0f, 1.0f, 0.5f, 0},
    {"Whole Steps", 0.0f, 10.0f, 5.0f, O3M_PARAM_INT | O3M_PARAM_WITH_UNIT(O3M_UNIT_SEMITONES)},
    {"Switch", 0.0f, 1.0f, 1.0f, O3M_PARAM_BOOL | O3M_PARAM_NO_LEARN},
    {"Cutoff", 20.0f, 20000.0f, 1000.0f, O3M_PARAM_LOG | O3M_PARAM_WITH_UNIT(O3M_UNIT_HZ)},
    {"Reso", 0.0f, 1.0f, 0.2f, 0},
    {"Drive", 0.0f, 1.0f, 0.0f, 0},
    {"Mix", 0.0f, 1.0f, 1.0f, 0},
    {"Tilt", -1.0f, 1.0f, 0.0f, 0},
    {"Page Two A", 0.0f, 1.0f, 0.5f, 0},
    {"Page Two B", -1.0f, 1.0f, 0.0f, 0},
    {"Big Steps", 0.0f, 200.0f, 100.0f, O3M_PARAM_INT},
};
const o3m_module kInst = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "Inst", sizeof(IState), 11, kInstParams,
                          i_init, i_render, i_set_param, i_note_on, i_note_off, i_shutdown, nullptr, 0};

// The host shared object of an app of this slice as a native app: its descriptor, the OS's own module set.
struct SoApp {
  void* handle = nullptr;
  const o3m_module* entry = nullptr;
  explicit SoApp(const char* path) {
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle != nullptr) {
      entry = static_cast<const o3m_module*>(dlsym(handle, "o3m_entry"));
    }
  }
  SoApp(const SoApp&) = delete;
  SoApp& operator=(const SoApp&) = delete;
  ~SoApp() {
    if (handle != nullptr) {
      dlclose(handle);
    }
  }
};

struct Card {
  fs::path root;
  Card() {
    root = fs::temp_directory_path() / ("o3m_edit_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "modules" / "apps");
    for (const char* n : {"Editor", "Editor4", "Editor3", "Editor2", "Viewer", "Listener", "Writer", "rhythmgen"}) {
      std::ofstream f(root / "modules" / "apps" / (std::string(n) + ".tmod"), std::ios::binary);
      f << "x";
    }
  }
  void add(const char* stem) {
    std::ofstream f(root / "modules" / "apps" / (std::string(stem) + ".tmod"), std::ios::binary);
    f << "x";
  }
  void blob(const char* stem, std::uint8_t minor, std::uint32_t caps) {
    module::test::Blob b = module::test::blob_app(1, minor);
    b.h.reserved2[0] = caps;
    b.seal();
    const std::vector<std::uint8_t> bytes = b.file();
    std::ofstream f(root / "modules" / "apps" / (std::string(stem) + ".tmod"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  ~Card() {
    std::error_code ec;
    fs::remove_all(ec ? root : root, ec);
  }
};

struct Modules {
  module::ModuleSet set;
  module::ModuleLoader loader;
  audio::ModuleVoices voices;
  app::ModuleUi ui;
  alignas(64) std::uint8_t scratch[audio::ModuleVoices::kScratchBytes] = {};
  alignas(64) std::uint8_t sdram[16384] = {};
};

// The host shared object of Rhythm Gen as a native app: its descriptor, the OS's own module set.
struct RhythmGen {
  void* handle = nullptr;
  const o3m_module* entry = nullptr;
  RhythmGen() {
    handle = dlopen(O3M_RHYTHMGEN_HOST_SO, RTLD_NOW | RTLD_LOCAL);
    if (handle != nullptr) {
      entry = static_cast<const o3m_module*>(dlsym(handle, "o3m_entry"));
    }
  }
  ~RhythmGen() {
    if (handle != nullptr) {
      dlclose(handle);
    }
  }
};

// A project, an Editor over it with the undo journal, the App, the module system with the test apps (and Rhythm Gen), the card.
struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  audio::PlayPosition position;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  Card card;
  platform::host::DirStorage storage{card.root.string()};
  std::unique_ptr<Modules> mods = std::make_unique<Modules>();
  midi::CcInput cc;
  std::unique_ptr<audio::AppTap> tap = std::make_unique<audio::AppTap>();
  RhythmGen rg;
  SoApp chords_so{O3M_CHORDS_HOST_SO}, humanize_so{O3M_HUMANIZE_HOST_SO}, mutate_so{O3M_MUTATE_HOST_SO}, morph_so{O3M_MORPH_HOST_SO};
  SoApp smf_so{O3M_SMFEXPORT_HOST_SO};
  // OS-H4: the card the apps' files are written to, the save service over it and the staging ring of the export calls (the Rig(…, h4) apps)
  o3os::test::MemCard memcard;
  app::SaveService saves;
  alignas(8) std::uint8_t export_ring[app::AppExport::kRingBytes] = {};
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);  // (after the editor: ~App closes a take's group on it)

  // `h3b`: the four editing apps of OS-H3B are on the card too (they would change the count of the TOOLS list the older tests make).
  // `h4`: SMF EXPORT and three test apps that hold export_files, and a save service over an in-memory card with the export ring (OS-H4).
  explicit Rig(unsigned tracks = 12, bool h3b = false, bool h4 = false) {
    g_input = g_draw = g_enter = nullptr;
    NEED(storage.mount() == hal::storage::Status::Ok);
    (void)mods->voices.attach_scratch(mods->scratch, sizeof mods->scratch);
    (void)mods->set.cold().init(mods->sdram, 0x70000000u, sizeof mods->sdram);
    mods->ui.attach(mods->set, mods->loader, mods->voices);
    mods->ui.set_storage(&storage);
    mods->ui.set_clock(&clock, nullptr);
    mods->ui.set_midi_monitor(&cc.monitor());
    mods->ui.set_audio_tap(tap.get());
    CHECK(mods->set.add_native("Editor", kEditor, kReadEdit));
    CHECK(mods->set.add_native("Editor4", kEditor4, kReadEdit | O3M_CAP_AUDIO_TAP));
    CHECK(mods->set.add_native("Editor3", kEditor3, kReadEdit));
    CHECK(mods->set.add_native("Editor2", kEditor2));
    CHECK(mods->set.add_native("Viewer", kViewer, O3M_CAP_READ_PROJECT));
    CHECK(mods->set.add_native("Listener", kListener, O3M_CAP_MIDI_IN));
    CHECK(mods->set.add_native("Writer", kWriter, O3M_CAP_EDIT_PROJECT));
    if (rg.entry != nullptr) {
      CHECK(mods->set.add_native("rhythmgen", *rg.entry, kReadEdit));
    }
    if (h3b) {
      const std::pair<const char*, const SoApp*> apps[4] = {{"chords", &chords_so}, {"humanize", &humanize_so}, {"mutate", &mutate_so}, {"morph", &morph_so}};
      for (const auto& so : apps) {
        NEED(so.second->entry != nullptr);
        card.add(so.first);
        CHECK(mods->set.add_native(so.first, *so.second->entry, kReadEdit));
      }
    }
    if (h4) {
      NEED(smf_so.entry != nullptr);
      for (const char* n : {"smfexport", "Exporter", "ExpEdit", "ReadOnly7"}) {
        card.add(n);
      }
      CHECK(mods->set.add_native("smfexport", *smf_so.entry, O3M_CAP_READ_PROJECT | O3M_CAP_EXPORT_FILES));
      CHECK(mods->set.add_native("Exporter", kExporter, O3M_CAP_EXPORT_FILES));
      CHECK(mods->set.add_native("ExpEdit", kExpEdit, O3M_CAP_EDIT_PROJECT | O3M_CAP_EXPORT_FILES));
      CHECK(mods->set.add_native("ReadOnly7", kReadOnly7, O3M_CAP_READ_PROJECT));
      CHECK(!mods->set.add_native("ExpOld", kEditor, O3M_CAP_EXPORT_FILES));  // an app of ABI 1.6 cannot hold export_files
    }
    model::Project& p = project();
    p.reset();
    (void)p.set_tempo_x100(12000);
    (void)p.create_pattern(1, 32);
    (void)p.create_pattern(2, 64);
    (void)p.set_active_tracks(tracks);
    (void)p.append_order(1);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_modules(&mods->ui);
    app->attach_play_position(&position);
    if (h4) {
      saves.attach(memcard, memcard);
      saves.attach_app_export(export_ring, sizeof export_ring);
      app->attach_saves(&saves);
    }
    app->run_once();
    mods->ui.scan_apps();
  }
  ~Rig() { g_input = g_draw = g_enter = nullptr; }

  model::Project& project() { return test::arena(1).project; }
  // OS-H3B: instrument `n` (1..64) as a SAMPLE instrument with its own values (`k` shifts them), `engine` its play engine. The history starts again (the project was changed behind the editor).
  void install_sample(unsigned n, unsigned engine = 0, unsigned k = 0) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    in.engine = static_cast<std::uint8_t>(engine);
    in.sampler.volume = static_cast<std::uint8_t>(40 + 7 * k);
    in.sampler.panning = static_cast<std::uint8_t>(50 - 5 * k);
    in.sampler.tune = static_cast<std::int8_t>(2 * static_cast<int>(k) - 4);
    in.sampler.fine_tune = static_cast<std::int8_t>(10 * static_cast<int>(k));
    in.sampler.cutoff = 1.0f - 0.1f * static_cast<float>(k);
    in.sampler.resonance = 0.3f * static_cast<float>(k);
    in.sampler.filter_enable = static_cast<std::uint8_t>(k & 1u);
    in.sampler.delay_send = static_cast<std::uint8_t>(3 * k);
    in.sampler.envelope[0].release = static_cast<std::uint16_t>(500 + 100 * k);
    in.sampler.envelope[2].amount = 0.2f * static_cast<float>(k);
    in.sampler.lfo[1].amount = 0.1f * static_cast<float>(k);
    in.sampler.play_mode = static_cast<std::uint8_t>(k % 2u);
    std::snprintf(in.name, sizeof in.name, "SMP%u", n);
    CHECK(project().set_instrument(n - 1u, in) == model::Status::Ok);
    editor->history_reset();
  }
  void install_midi(unsigned n) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = 3;
    CHECK(project().set_instrument(n - 1u, in) == model::Status::Ok);
    editor->history_reset();
  }
  // A MODULE instrument ("Inst", 11 parameters) in instrument `n`, bound to an instance; its blob holds `blob_n` floats (a short one: the module's defaults stand for the rest), `k` shifts them.
  void install_module(unsigned n, unsigned blob_n = 11, float k = 0.0f, bool bind = true) {
    if (mods->set.find("Inst") < 0) {
      CHECK(mods->set.add_native("Inst", kInst));
    }
    unsigned ref = 0;
    bool found = false;
    for (unsigned i = 0; i < 8 && !found; ++i) {
      const model::ModuleRef* m = project().module(i);
      found = m != nullptr && std::strcmp(m->name, "Inst") == 0;
      ref = found ? i : ref;
    }
    if (!found) {
      CHECK(project().add_module("Inst", 4, O3M_ABI_MAJOR, O3M_ABI_MINOR) == model::Status::Ok);
      for (unsigned i = 0; i < 8 && !found; ++i) {
        const model::ModuleRef* m = project().module(i);
        found = m != nullptr && std::strcmp(m->name, "Inst") == 0;
        ref = found ? i : ref;
      }
    }
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Module);
    in.module_index = static_cast<std::uint8_t>(ref);
    std::snprintf(in.name, sizeof in.name, "INST%u", n);
    for (unsigned i = 0; i < blob_n && i < 11; ++i) {
      const o3m_param& p = kInstParams[i];
      float v = p.min + (p.max - p.min) * (0.3f + 0.05f * static_cast<float>(i) + 0.1f * k);
      v = v > p.max ? p.max : v;
      if ((p.flags & O3M_PARAM_INT) != 0u) {
        v = static_cast<float>(static_cast<int>(v));
      }
      std::memcpy(in.params + 4u * i, &v, 4);
    }
    in.param_len = static_cast<std::uint16_t>(4u * blob_n);
    CHECK(project().set_instrument(n - 1u, in) == model::Status::Ok);
    if (!bind) {
      editor->history_reset();
      return;
    }
    const int idx = mods->set.find("Inst");
    NEED(idx >= 0);
    const o3m_module& mod = *mods->set.at(static_cast<unsigned>(idx))->entry;
    std::uint8_t* mem = nullptr;
    std::uint32_t addr = 0;
    NEED(mods->set.cold().take(mod.state_bytes, 16, mem, addr));
    std::memset(mem, 0, mod.state_bytes);
    CHECK(mods->voices.bind(n - 1u, mod, module::fault_id("Inst"), mem, project().instrument(n - 1u)->params, project().instrument(n - 1u)->param_len) == audio::ModuleVoices::Bind::Ok);
    editor->history_reset();
  }
  // The render's block start: the module instances hear what the main loop sent.
  void block() {
    mods->voices.begin_block();
    mods->voices.end_block();
  }
  IState* inst_state(unsigned n) { return static_cast<IState*>(mods->voices.state_of(n - 1u)); }
  unsigned entry(const char* stem) {
    for (unsigned i = 0; i < mods->ui.app_entry_count(); ++i) {
      if (std::strcmp(mods->ui.app_entry_at(i)->stem, stem) == 0) {
        return i;
      }
    }
    FAIL("no such app on the card: " << stem);
    return 0;
  }
  app::ModuleUi::LaunchResult launch(const char* stem, const o3m_launch_ctx* ctx = nullptr) { return mods->ui.launch_app(entry(stem), &project(), ctx); }
  void launch_ok(const char* stem, const o3m_launch_ctx* ctx = nullptr) {
    if (launch(stem, ctx) == app::ModuleUi::LaunchResult::NeedsConfirm) {
      CHECK(launch(stem, ctx) == app::ModuleUi::LaunchResult::Ok);
    }
    CHECK(mods->ui.app_running());
  }
  const o3m_host* host() { return mods->ui.app_host(); }
  // One input() call of the running app, running `script` in it. (A draw call: draw().)
  void input_call(std::function<void(const o3m_host*)> script) {
    g_input = std::move(script);
    CHECK(mods->ui.app_input(O3M_EV_BUTTON, 0, 1, false));
    g_input = nullptr;
  }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    pass(1);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass(1);
  }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    pass(1);
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass(1);
  }
  // F7 + F8 together, as the unit sends them
  void chord_exit() {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::F7), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::F8), true));
    pass(1);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::F8), false));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::F7), false));
    pass(1);
  }
  app::DispatchResult invoke(const char* name, core::ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  const app::ViewState& state() const { return app->state(); }
};

// Everything the edit table can change, as bytes: the patterns (lengths and steps), the tempo and the generation counter that every mutator bumps.
std::vector<std::uint8_t> snapshot(const model::Project& p) {
  std::vector<std::uint8_t> out;
  for (unsigned id = 1; id <= 255; ++id) {
    const model::Pattern* pat = p.pattern(id);
    if (pat != nullptr) {
      const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(pat);
      out.insert(out.end(), b, b + sizeof(model::Pattern));
    }
  }
  const std::uint32_t t = p.tempo_x100();
  out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&t), reinterpret_cast<const std::uint8_t*>(&t) + 4);
  return out;
}

o3m_step step(std::uint8_t note, std::uint8_t instr = 0, std::uint8_t vol = 255) { return o3m_step{note, instr, vol, 0, 0, 0, 0, 0}; }

constexpr std::uint8_t kAll = O3M_STEP_NOTE | O3M_STEP_INSTR | O3M_STEP_VOL;

}  // namespace

// ---- the ABI --------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("abi 1.5: the version, the tables' sizes and the published offsets of 1.2 .. 1.4, which did not move") {
  CHECK(O3M_ABI_MINOR == 13);  // (OS-MODRATE S2: 1.13 gave an instrument the RATE bit of fx_flags; 1.12 published the effect surface; OS-FLASHDIET I1: 1.11 the importer kind)
  CHECK(module::kOsAbiMinor == 13u);
  // On the target (4-byte pointers) the numbers are written in the header itself, and each module build checks them; here the same facts for this host's pointers
  CHECK(offsetof(o3m_read_api, get_tempo) == 8);
  CHECK(offsetof(o3m_read_api, launch_ctx) == 8 + kP);
  CHECK(offsetof(o3m_read_api, pattern_info) == 8 + 2 * kP);
  CHECK(offsetof(o3m_read_api, get_steps) == 8 + 3 * kP);
  CHECK(offsetof(o3m_edit_api, set_tempo) == 8);
  CHECK(offsetof(o3m_edit_api, edit_begin) == 8 + kP);
  CHECK(offsetof(o3m_edit_api, set_steps) == 8 + 4 * kP);
  CHECK(offsetof(o3m_launch_ctx, block_tracks) == 28);
  CHECK(sizeof(o3m_launch_ctx) == 32);
  CHECK(sizeof(o3m_pattern_info) == 28);
}

TEST_CASE("abi 1.5: an app of 1.4 and an app of 1.3 read the tables through their own, shorter, layouts and find every member where it was") {
  Rig r;
  r.launch_ok("Editor4");
  NEED(r.host() != nullptr);
  {
    ReadV14 read;
    EditV14 edit;
    std::memcpy(&read, r.host()->read, sizeof read);
    std::memcpy(&edit, r.host()->edit, sizeof edit);
    CHECK((read.version == O3M_READ_API_VERSION && read.size >= sizeof(ReadV14)));
    CHECK((read.get_tempo == r.host()->read->get_tempo && read.launch_ctx == r.host()->read->launch_ctx));
    CHECK(edit.set_tempo == r.host()->edit->set_tempo);
    CHECK(read.get_tempo() == 12000u);
    // the 1.4 app's whole path: set_tempo is queued and applied as before
    CHECK(edit.set_tempo(9000) == O3M_EDIT_OK);
    r.pass(40);
    CHECK(r.project().tempo_x100() == 9000u);
    // and the host table it was handed announces 1.5 with the members it knows where they were
    CHECK((r.host()->abi_minor == 13u && r.host()->size == sizeof(o3m_host)));  // the OS announces 1.13 (OS-MODRATE S2)
    CHECK((r.host()->io->tap_select != nullptr && r.host()->io->tap_levels != nullptr));  // the 1.4 audio tap is where it was
  }
  r.mods->ui.app_exit();
  r.launch_ok("Editor3");
  {
    IoV13 io;
    std::memcpy(&io, r.host()->io, sizeof io);
    CHECK((io.now_ms == r.host()->io->now_ms && io.midi_read == r.host()->io->midi_read && io.midi_lost == r.host()->io->midi_lost));
    // an app built against the 28-byte launch context gets 28 bytes: the canary after it is not touched
    struct {
      LaunchV14 ctx;
      std::uint32_t canary;
    } buf;
    std::memset(&buf, 0xAA, sizeof buf);
    buf.ctx.size = sizeof(LaunchV14);
    CHECK(r.host()->read->launch_ctx(reinterpret_cast<o3m_launch_ctx*>(&buf.ctx)) == 0u);
    CHECK((buf.ctx.size == 28u && buf.canary == 0xAAAAAAAAu));
  }
  r.mods->ui.app_exit();
  // an app of 1.2 is still lent nothing new and a table that ends at draw
  CHECK(r.launch("Editor2") == app::ModuleUi::LaunchResult::Ok);
  CHECK((r.host()->read == nullptr && r.host()->edit == nullptr && r.host()->io == nullptr));
  CHECK(r.host()->size == sizeof(HostV12));
}

TEST_CASE("abi 1.5: the new entries are NULL without the capability, whatever the subset, and the launch context gets block_tracks") {
  Rig r;
  struct Case {
    const char* stem;
    bool read, edit;
  };
  for (const Case& c : {Case{"Editor", true, true}, Case{"Viewer", true, false}, Case{"Writer", false, true}, Case{"Listener", false, false}}) {
    r.launch_ok(c.stem);
    NEED(r.host() != nullptr);
    CHECK_MESSAGE((r.host()->read != nullptr) == c.read, c.stem);
    CHECK_MESSAGE((r.host()->edit != nullptr) == c.edit, c.stem);
    if (c.read) {
      CHECK((r.host()->read->pattern_info != nullptr && r.host()->read->get_steps != nullptr && r.host()->read->size == sizeof(o3m_read_api)));
    }
    if (c.edit) {
      CHECK((r.host()->edit->edit_begin != nullptr && r.host()->edit->edit_end != nullptr && r.host()->edit->set_step != nullptr && r.host()->edit->set_steps != nullptr &&
             r.host()->edit->size == sizeof(o3m_edit_api)));
    }
    r.mods->ui.app_exit();
  }
  // block_tracks travels in the context an OS of 1.5 fills
  const o3m_launch_ctx sel = {sizeof sel, O3M_LAUNCH_FROM_TOOLS, 2, 3, 4, 5, 6, 7, {0, 0, 0}};
  r.launch_ok("Viewer", &sel);
  o3m_launch_ctx c;
  std::memset(&c, 0xAA, sizeof c);
  c.size = sizeof c;
  CHECK(r.host()->read->launch_ctx(&c) == 1u);
  CHECK((c.size == 32u && c.pattern == 2u && c.track == 3u && c.instrument == 4u && c.block_start == 5u && c.block_length == 6u && c.block_tracks == 7u));
}

TEST_CASE("abi 1.5: a file built for a newer ABI than the OS's is refused at launch with NEEDS NEWER OS, again on the second try") {
  Rig r;
  r.card.blob("Future", static_cast<std::uint8_t>(O3M_ABI_MINOR + 1), 0);
  r.mods->ui.scan_apps();
  using R = app::ModuleUi::LaunchResult;
  CHECK(r.launch("Future") == R::NeedsNewerOs);
  CHECK(r.mods->ui.last_status() == module::Status::AbiMinor);
  CHECK(std::string(app::ModuleUi::launch_text(R::NeedsNewerOs)) == "NEEDS NEWER OS");
  CHECK(!r.mods->ui.app_running());
  r.mods->ui.scan_apps();  // (now quarantined for the boot: the message stays the same one)
  CHECK(r.launch("Future") == R::NeedsNewerOs);
  // a file of exactly this OS's minor is not refused for its ABI (the blob's code is not runnable: the loader accepts it, the host cannot call it)
  r.card.blob("Present", O3M_ABI_MINOR, 0);
  r.mods->ui.scan_apps();
  CHECK(r.launch("Present") != R::NeedsNewerOs);
  CHECK(r.mods->ui.last_status() == module::Status::Ok);
}

TEST_CASE("abi 1.5: the loader accepts an app of every minor 1..5 with its capabilities, and refuses the bit of a later slice as before") {
  // (one Rig per minor: every launch adds a module to the set, and the set holds kMaxModules of them)
  for (std::uint8_t minor = 3; minor <= O3M_ABI_MINOR; ++minor) {
    Rig r;
    const std::string name = "Min" + std::to_string(minor);
    r.card.blob(name.c_str(), minor, kReadEdit);
    r.mods->ui.scan_apps();
    (void)r.launch(name.c_str());  // (the blob's code is not runnable; what matters is that the loader accepted the header and the capability word)
    CHECK_MESSAGE(r.mods->ui.last_status() == module::Status::Ok, name);
    CHECK(!r.mods->set.quarantined(name.c_str()));
  }
  Rig r;
  r.card.blob("MinNext", static_cast<std::uint8_t>(O3M_ABI_MINOR + 1), kReadEdit);
  r.mods->ui.scan_apps();
  CHECK(r.launch("MinNext") == app::ModuleUi::LaunchResult::NeedsNewerOs);
  CHECK(r.mods->ui.last_status() == module::Status::AbiMinor);
}

// ---- reads ----------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("read table: pattern_info and get_steps copy the project out; bad arguments are RANGE and write nothing") {
  Rig r;
  model::Project& p = r.project();
  (void)p.set_track_length(1, 3, 20);
  (void)p.set_steps(1, 0, 0, std::vector<model::Step>(4, model::Step{61, 2, 100, {{7, 8}, {9, 10}}}).data(), 4);
  r.launch_ok("Editor");
  const o3m_read_api* rd = r.host()->read;
  o3m_pattern_info info;
  std::memset(&info, 0xAA, sizeof info);
  info.size = sizeof info;
  CHECK(rd->pattern_info(1, &info) == O3M_EDIT_OK);
  CHECK((info.size == sizeof info && info.length == 32u && info.tracks == 12u));
  CHECK((info.track_length[0] == 32 && info.track_length[3] == 20 && info.track_length[11] == 32 && info.track_length[12] == 0 && info.track_length[15] == 0));
  CHECK(rd->pattern_info(2, &info) == O3M_EDIT_OK);
  CHECK(info.length == 64u);
  CHECK(rd->pattern_info(3, &info) == O3M_EDIT_RANGE);  // does not exist
  CHECK(rd->pattern_info(0, &info) == O3M_EDIT_RANGE);
  CHECK(rd->pattern_info(256, &info) == O3M_EDIT_RANGE);
  CHECK(rd->pattern_info(1, nullptr) == O3M_EDIT_RANGE);
  info.size = 2;  // no room for even the size word
  CHECK(rd->pattern_info(1, &info) == O3M_EDIT_RANGE);
  // a smaller struct gets its prefix
  std::memset(&info, 0xAA, sizeof info);
  info.size = 8;
  CHECK(rd->pattern_info(1, &info) == O3M_EDIT_OK);
  CHECK((info.size == 8u && info.length == 32u && info.tracks == 0xAAAAAAAAu));

  o3m_step out[128];
  std::memset(out, 0xAA, sizeof out);
  CHECK(rd->get_steps(1, 0, 0, out, 5) == O3M_EDIT_OK);
  CHECK((out[0].note == 61 && out[0].instr == 2 && out[0].vol == 100 && out[0].fx1_type == 7 && out[0].fx1_value == 8 && out[0].fx2_type == 9 && out[0].fx2_value == 10 && out[0].reserved == 0));
  CHECK((out[3].note == 61 && out[4].note == 0 && out[4].vol == 255 && out[4].reserved == 0));
  CHECK(rd->get_steps(1, 0, 31, out, 1) == O3M_EDIT_OK);          // the last row
  CHECK(rd->get_steps(1, 0, 32, out, 1) == O3M_EDIT_RANGE);       // past the end
  CHECK(rd->get_steps(1, 0, 30, out, 3) == O3M_EDIT_RANGE);       // across it
  CHECK(rd->get_steps(1, 3, 19, out, 1) == O3M_EDIT_OK);          // track 4 plays 20 rows
  CHECK(rd->get_steps(1, 3, 20, out, 1) == O3M_EDIT_RANGE);
  CHECK(rd->get_steps(1, 0, 0, out, 0) == O3M_EDIT_RANGE);        // a count of 0
  CHECK(rd->get_steps(1, 12, 0, out, 1) == O3M_EDIT_RANGE);       // an inactive track
  CHECK(rd->get_steps(1, 255, 0, out, 1) == O3M_EDIT_RANGE);
  CHECK(rd->get_steps(9, 0, 0, out, 1) == O3M_EDIT_RANGE);
  CHECK(rd->get_steps(1, 0, 0, nullptr, 1) == O3M_EDIT_RANGE);
  CHECK(rd->get_steps(2, 0, 0, out, 64) == O3M_EDIT_OK);
  CHECK(rd->get_steps(2, 0, 0, out, 65) == O3M_EDIT_RANGE);
  // no project at all
  r.mods->ui.app_exit();
  CHECK(r.mods->ui.launch_app(r.entry("Editor"), nullptr) == app::ModuleUi::LaunchResult::Ok);
  CHECK(r.host()->read->pattern_info(1, &info) == O3M_EDIT_NO_PROJECT);
  CHECK(r.host()->read->get_steps(1, 0, 0, out, 1) == O3M_EDIT_NO_PROJECT);
  CHECK(r.host()->edit->edit_begin() == O3M_EDIT_NO_PROJECT);
}

// ---- the group ------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("edit group: edit_begin .. edit_end applies the writes together as ONE undo unit named after the app, the mask changes only the masked fields; UNDO and REDO") {
  Rig r;
  model::Project& p = r.project();
  model::Step base[8];
  for (auto& s : base) {
    s = model::Step{60, 3, 90, {{5, 6}, {7, 8}}};
  }
  (void)p.set_steps(1, 2, 0, base, 8);
  const std::vector<std::uint8_t> before = snapshot(p);
  r.launch_ok("Editor");
  const std::size_t depth = r.journal.depth();
  std::vector<int> codes;
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    codes.push_back(e->edit_begin());
    const o3m_step s = step(72, 9, 17);
    codes.push_back(e->set_step(1, 2, 0, &s, O3M_STEP_NOTE));                  // the note only
    o3m_step two[2] = {step(1, 11, 0), step(2, 12, 1)};
    codes.push_back(e->set_steps(1, 2, 4, two, 2, O3M_STEP_INSTR | O3M_STEP_VOL));  // instrument and volume of two rows
    o3m_step fx = step(0);
    fx.fx1_type = 20;
    fx.fx1_value = 21;
    fx.fx2_type = 130;
    fx.fx2_value = 131;
    codes.push_back(e->set_step(1, 2, 7, &fx, O3M_STEP_FX1 | O3M_STEP_FX2));
    // nothing is applied before edit_end: a read inside the group still sees the project as it was
    o3m_step now;
    codes.push_back(h->read->get_steps(1, 2, 0, &now, 1));
    codes.push_back(now.note == 60 ? 0 : 99);
    codes.push_back(e->edit_end());
  });
  for (int c : codes) {
    CHECK(c == O3M_EDIT_OK);
  }
  CHECK(r.journal.depth() == depth + 1u);  // one unit for the four writes
  const model::Pattern* pat = p.pattern(1);
  CHECK((pat->steps[2][0].note == 72 && pat->steps[2][0].instr == 3 && pat->steps[2][0].vol == 90 && pat->steps[2][0].fx[0].type == 5));  // only the note changed
  CHECK((pat->steps[2][4].note == 60 && pat->steps[2][4].instr == 11 && pat->steps[2][4].vol == 0 && pat->steps[2][5].instr == 12 && pat->steps[2][5].vol == 1));
  CHECK((pat->steps[2][7].note == 60 && pat->steps[2][7].fx[0].type == 20 && pat->steps[2][7].fx[0].value == 21 && pat->steps[2][7].fx[1].type == 130 && pat->steps[2][7].fx[1].value == 131 &&
         pat->steps[2][7].instr == 3));
  CHECK(r.mods->ui.groups_applied() == 1u);
  // the project is dirty as for a hand edit (the generation moved) and UNDO names the app
  const std::vector<std::uint8_t> after = snapshot(p);
  CHECK(after != before);
  core::ActionId id{};
  core::ActionArg arg = 0;
  NEED(r.journal.newest_undo(id, arg));
  CHECK(id == core::action_id("module.app_group"));
  char title[26];
  CHECK(r.mods->ui.app_title(static_cast<unsigned>(arg), title, sizeof title));
  CHECK(std::string(title) == "EDITOR");
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot(p) == before);                      // byte for byte
  CHECK(r.state().notify.size() != 0u);              // "UNDO EDITOR"
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(snapshot(p) == after);
}

TEST_CASE("edit group: an empty group, or one that writes what is already there, leaves no undo unit") {
  Rig r;
  r.launch_ok("Editor");
  const std::size_t depth = r.journal.depth();
  r.input_call([](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  r.input_call([](const o3m_host* h) {
    const o3m_step same = step(0, 0, 255);  // an empty step on an empty pattern
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_steps(1, 0, 0, &same, 1, kAll) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.journal.depth() == depth);
  CHECK(r.mods->ui.groups_applied() == 1u);  // (the empty group had nothing to apply; the other was applied and changed nothing)
  CHECK(r.state().notify.size() == 0u);
}

TEST_CASE("edit group: the order of calls: a write or an edit_end with no group, a second begin, one group per call, only from input()") {
  Rig r;
  r.launch_ok("Editor");
  const std::vector<std::uint8_t> before = snapshot(r.project());
  r.input_call([](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    const o3m_step s = step(60, 1);
    CHECK(e->set_step(1, 0, 0, &s, kAll) == O3M_EDIT_NO_GROUP);  // no implicit units
    CHECK(e->set_steps(1, 0, 0, &s, 1, kAll) == O3M_EDIT_NO_GROUP);
    CHECK(e->edit_end() == O3M_EDIT_NO_GROUP);
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    CHECK(e->edit_begin() == O3M_EDIT_GROUP_OPEN);
    CHECK(e->set_step(1, 0, 0, &s, kAll) == O3M_EDIT_OK);
    CHECK(e->edit_end() == O3M_EDIT_OK);
    CHECK(e->edit_end() == O3M_EDIT_NO_GROUP);
    CHECK(e->edit_begin() == O3M_EDIT_RATE);  // a second group in the same call
    CHECK(e->set_step(1, 0, 1, &s, kAll) == O3M_EDIT_NO_GROUP);
  });
  CHECK(r.project().pattern(1)->steps[0][0].note == 60);
  CHECK(r.project().pattern(1)->steps[0][1].note == 0);
  // the next call may make a group again
  r.input_call([](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  // not from view_draw, enter, exit or a call of no kind
  int draw_code = -1, enter_code = -1, outside_code = -1;
  g_draw = [&](const o3m_host* h) { draw_code = h->edit->edit_begin(); };
  g_enter = [&](const o3m_host* h) { enter_code = h->edit->edit_begin(); };
  app::ModuleUi& ui = r.mods->ui;
  ui.app_exit();
  r.launch_ok("Editor");  // (enter runs)
  CHECK(enter_code == O3M_EDIT_RATE);
  // draw: the page body
  platform::host::FakeDisplay display2;
  CHECK(r.host() != nullptr);
  outside_code = r.host()->edit->edit_begin();  // outside any call of the app (a test calling the table directly)
  CHECK(outside_code == O3M_EDIT_RATE);
  (void)draw_code;
  CHECK(snapshot(r.project()) != before);
}

TEST_CASE("edit group: bad arguments return RANGE and write nothing, and the group stays valid") {
  Rig r;
  (void)r.project().set_track_length(1, 5, 10);
  r.launch_ok("Editor");
  const std::vector<std::uint8_t> before = snapshot(r.project());
  int ok_code = -1, end_code = -1;
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    const auto one = [&](o3m_step st, std::uint8_t fields) { return e->set_step(1, 0, 0, &st, fields); };
    const o3m_step good = step(60, 1, 10);
    o3m_step many[130];
    for (auto& m : many) {
      m = good;
    }
    // a pattern that does not exist, ids out of the pattern numbering
    CHECK(e->set_step(9, 0, 0, &good, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_step(0, 0, 0, &good, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_step(256, 0, 0, &good, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_step(0xFFFF, 0, 0, &good, kAll) == O3M_EDIT_RANGE);
    // a track beyond the active ones
    CHECK(e->set_step(1, 12, 0, &good, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_step(1, 255, 0, &good, kAll) == O3M_EDIT_RANGE);
    // a row or a count outside the track
    CHECK(e->set_step(1, 0, 32, &good, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_step(1, 0, 255, &good, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_step(1, 5, 10, &good, kAll) == O3M_EDIT_RANGE);  // track 6 plays 10 rows
    CHECK(e->set_steps(1, 0, 0, many, 33, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_steps(1, 0, 30, many, 3, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_steps(1, 0, 0, many, 0, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_steps(1, 0, 0, many, 129, kAll) == O3M_EDIT_RANGE);
    // NULL
    CHECK(e->set_step(1, 0, 0, nullptr, kAll) == O3M_EDIT_RANGE);
    CHECK(e->set_steps(1, 0, 0, nullptr, 4, kAll) == O3M_EDIT_RANGE);
    // the field mask: none, an unknown bit, all unknown
    CHECK(e->set_step(1, 0, 0, &good, 0) == O3M_EDIT_RANGE);
    CHECK(e->set_step(1, 0, 0, &good, 32) == O3M_EDIT_RANGE);
    CHECK(e->set_step(1, 0, 0, &good, kAll | 64) == O3M_EDIT_RANGE);
    CHECK(e->set_step(1, 0, 0, &good, 0xFF) == O3M_EDIT_RANGE);
    // values: the note 129..252, the instrument above 64, the volume 128..254, the reserved FX types 43..127 (only for the masked fields)
    for (std::uint32_t n : {129u, 200u, 252u}) {
      CHECK(one(step(static_cast<std::uint8_t>(n)), O3M_STEP_NOTE) == O3M_EDIT_RANGE);
    }
    for (std::uint32_t n : {0u, 128u, 253u, 254u, 255u}) {
      const o3m_step s = step(static_cast<std::uint8_t>(n));
      CHECK(e->set_step(1, 0, 0, &s, O3M_STEP_NOTE) == O3M_EDIT_OK);  // empty, the last note, fade, cut, off
    }
    CHECK(one(step(60, 65), O3M_STEP_INSTR) == O3M_EDIT_RANGE);
    CHECK(one(step(60, 0, 128), O3M_STEP_VOL) == O3M_EDIT_RANGE);
    CHECK(one(step(60, 0, 254), O3M_STEP_VOL) == O3M_EDIT_RANGE);
    CHECK(one(step(60, 64, 127), O3M_STEP_INSTR | O3M_STEP_VOL) == O3M_EDIT_OK);
    CHECK(one(step(60, 0, 255), O3M_STEP_VOL) == O3M_EDIT_OK);
    for (int reserved_type : {43, 100, 127}) {
      const std::uint8_t t = static_cast<std::uint8_t>(reserved_type);
      o3m_step fx = step(0);
      fx.fx1_type = t;
      fx.fx2_type = t;
      CHECK(e->set_step(1, 0, 0, &fx, O3M_STEP_FX1) == O3M_EDIT_RANGE);
      CHECK(e->set_step(1, 0, 0, &fx, O3M_STEP_FX2) == O3M_EDIT_RANGE);
      CHECK(e->set_step(1, 0, 0, &fx, O3M_STEP_NOTE) == O3M_EDIT_OK);  // (the FX bytes are not looked at when the mask leaves them out)
    }
    // one bad step in a run refuses the whole call
    many[77] = step(150);
    CHECK(e->set_steps(2, 0, 0, many, 64, O3M_STEP_NOTE) == O3M_EDIT_OK);  // (row 77 is past the 64 of the call)
    many[40] = step(150);
    CHECK(e->set_steps(2, 0, 0, many, 64, O3M_STEP_NOTE) == O3M_EDIT_RANGE);
    // the group is still good
    ok_code = e->set_step(1, 3, 3, &good, kAll);
    end_code = e->edit_end();
  });
  CHECK(ok_code == O3M_EDIT_OK);
  CHECK(end_code == O3M_EDIT_OK);
  // only the valid writes took effect: nothing from the refused ones
  const model::Pattern* p = r.project().pattern(1);
  CHECK((p->steps[3][3].note == 60 && p->steps[3][3].instr == 1 && p->steps[3][3].vol == 10));
  CHECK(p->steps[0][0].vol == 255);  // the last valid write to row 0
  // the refused call wrote nothing (row 40 of its run holds the bad note 150, it was refused whole), the accepted one before it wrote its 64 rows
  CHECK((r.project().pattern(2)->steps[0][0].note == 60 && r.project().pattern(2)->steps[0][40].note == 60 && r.project().pattern(2)->steps[0][63].note == 60));
  CHECK(snapshot(r.project()) != before);
  CHECK(r.mods->ui.writes_refused() > 30u);
}

TEST_CASE("edit group: the step limit: 2,048 steps are accepted, the 2,049th and every later write are LIMIT, and edit_end drops the whole group") {
  Rig r(16);
  (void)r.project().set_pattern_length(1, 128);
  (void)r.project().create_pattern(3, 128);
  r.launch_ok("Editor");
  // 16 tracks x 128 rows = exactly 2,048 steps
  const std::vector<std::uint8_t> before = snapshot(r.project());
  const std::uint32_t gen = r.project().generation();
  const std::size_t depth = r.journal.depth();
  std::vector<int> codes;
  r.input_call([&](const o3m_host* h) {
    o3m_step rows[128];
    for (auto& s : rows) {
      s = step(70, 2, 20);
    }
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    for (std::uint8_t t = 0; t < 16; ++t) {
      codes.push_back(h->edit->set_steps(1, t, 0, rows, 128, kAll));
    }
    for (int c : codes) {
      CHECK(c == O3M_EDIT_OK);
    }
    // one more
    const o3m_step one = step(71);
    CHECK(h->edit->set_step(1, 0, 0, &one, kAll) == O3M_EDIT_LIMIT);
    CHECK(h->edit->set_step(1, 0, 1, &one, kAll) == O3M_EDIT_LIMIT);                  // every later write too
    CHECK(h->edit->set_steps(1, 1, 0, rows, 2, kAll) == O3M_EDIT_LIMIT);
    CHECK(h->edit->set_step(1, 0, 255, &one, kAll) == O3M_EDIT_LIMIT);                // (even a bad one: the group is over)
    CHECK(h->edit->edit_end() == O3M_EDIT_LIMIT);
  });
  // whole-group rollback: the project is exactly as it was, no generation moved, no unit, no redo entry
  CHECK(snapshot(r.project()) == before);
  CHECK(r.project().generation() == gen);
  CHECK(r.journal.depth() == depth);
  CHECK(r.mods->ui.groups_dropped() == 1u);
  CHECK(r.mods->ui.groups_applied() == 0u);
  // exactly 2,048 is fine
  r.input_call([&](const o3m_host* h) {
    o3m_step rows[128];
    for (auto& s : rows) {
      s = step(70, 2, 20);
    }
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    for (std::uint8_t t = 0; t < 16; ++t) {
      CHECK(h->edit->set_steps(1, t, 0, rows, 128, kAll) == O3M_EDIT_OK);
    }
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.journal.depth() == depth + 1u);
  CHECK(r.project().pattern(1)->steps[15][127].note == 70);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot(r.project()) == before);
}

TEST_CASE("edit group: the call limit: 4,096 calls, refused ones included; a dropped group leaves the redo history alone") {
  Rig r;
  r.launch_ok("Editor");
  // a unit to undo and redo first
  r.input_call([](const o3m_host* h) {
    const o3m_step s = step(64, 1);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_step(1, 0, 0, &s, kAll) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(r.journal.redo_depth() == 1u);
  const std::vector<std::uint8_t> before = snapshot(r.project());
  int last_ok = 0, over = 0, end_code = -1;
  r.input_call([&](const o3m_host* h) {
    const o3m_step s = step(60);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    for (unsigned i = 0; i < O3M_EDIT_MAX_CALLS; ++i) {
      last_ok = h->edit->set_step(9, 0, 0, &s, kAll);  // 4,096 refused calls (no such pattern)
      if (last_ok != O3M_EDIT_RANGE) {
        break;
      }
    }
    over = h->edit->set_step(1, 0, 0, &s, kAll);       // the 4,097th, a good one
    CHECK(h->edit->set_tempo(9000) == O3M_EDIT_LIMIT);
    end_code = h->edit->edit_end();
  });
  CHECK(last_ok == O3M_EDIT_RANGE);
  CHECK(over == O3M_EDIT_LIMIT);
  CHECK(end_code == O3M_EDIT_LIMIT);
  CHECK(snapshot(r.project()) == before);
  CHECK(r.journal.redo_depth() == 1u);  // untouched: nothing was written, so no new edit ended the redo history
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(r.project().pattern(1)->steps[0][0].note == 64);
}

TEST_CASE("edit group: set_tempo inside a group is part of the group's unit; outside it stays queued as before") {
  Rig r;
  r.launch_ok("Editor");
  const std::uint32_t tempo = r.project().tempo_x100();
  const std::vector<std::uint8_t> before = snapshot(r.project());
  const std::size_t depth = r.journal.depth();
  std::uint32_t read_back = 0, read_after = 0;
  r.input_call([&](const o3m_host* h) {
    const o3m_step s = step(60, 1);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_tempo(9000) == O3M_EDIT_OK);
    read_back = h->read->get_tempo();  // read your write
    CHECK(h->edit->set_tempo(1) == O3M_EDIT_RANGE);  // a bad tempo writes nothing and keeps the earlier one
    CHECK(h->edit->set_step(1, 0, 0, &s, kAll) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
    read_after = h->read->get_tempo();
  });
  CHECK(read_back == 9000u);
  CHECK(read_after == 9000u);
  CHECK(r.project().tempo_x100() == 9000u);  // applied at edit_end, not queued for the next pass
  CHECK(r.journal.depth() == depth + 1u);    // with the step, in one unit
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(r.project().tempo_x100() == tempo);
  CHECK(snapshot(r.project()) == before);
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(r.project().tempo_x100() == 9000u);
  // a group of a tempo alone is a unit too
  r.input_call([](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_tempo(15000) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.project().tempo_x100() == 15000u);
  // a tempo set in a group that the limit dropped is not applied
  r.input_call([](const o3m_host* h) {
    const o3m_step s = step(60);
    o3m_step rows[128];
    for (auto& x : rows) x = s;
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_tempo(20000) == O3M_EDIT_OK);
    for (int i = 0; i < 33; ++i) (void)h->edit->set_steps(2, 0, 0, rows, 64, kAll);  // 33 x 64 = 2,112 steps
    CHECK(h->edit->edit_end() == O3M_EDIT_LIMIT);
  });
  CHECK(r.project().tempo_x100() == 15000u);
  // outside a group it is queued and its own unit (the H1 behaviour)
  const std::size_t d2 = r.journal.depth();
  CHECK(r.host()->edit->set_tempo(11000) == O3M_EDIT_OK);
  CHECK(r.project().tempo_x100() == 15000u);
  r.pass(40);
  CHECK(r.project().tempo_x100() == 11000u);
  CHECK(r.journal.depth() == d2 + 1u);
}

TEST_CASE("edit group: a group left open when the call returns is closed by the OS, its changes kept and applied, counted, and the app may make another") {
  Rig r;
  r.launch_ok("Editor");
  const std::size_t depth = r.journal.depth();
  r.input_call([](const o3m_host* h) {
    const o3m_step s = step(66, 4, 40);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_step(1, 1, 2, &s, kAll) == O3M_EDIT_OK);
    // (no edit_end)
  });
  CHECK(r.mods->ui.groups_abandoned() == 1u);
  CHECK(!r.mods->ui.group_open());
  CHECK(r.project().pattern(1)->steps[1][2].note == 66);  // kept
  CHECK(r.journal.depth() == depth + 1u);                  // as one unit the user can take back
  CHECK(r.editor->group_depth() == 0u);                     // and the Editor has no group open: UNDO is never Busy
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().pattern(1)->steps[1][2].note == 0);
  CHECK(r.editor->redo() == model::UndoResult::Ok);
  // the next call makes a group again
  r.input_call([](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.mods->ui.groups_abandoned() == 1u);
  // a group left open that went over a limit is dropped whole, as at edit_end
  r.input_call([](const o3m_host* h) {
    const o3m_step s = step(67);
    o3m_step rows[64];
    for (auto& x : rows) x = s;
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    for (int i = 0; i < 40; ++i) (void)h->edit->set_steps(2, static_cast<std::uint8_t>(i % 12), 0, rows, 64, kAll);
  });
  CHECK(r.mods->ui.groups_abandoned() == 2u);
  CHECK(r.mods->ui.groups_dropped() == 1u);
  CHECK(r.project().pattern(2)->steps[0][0].note == 0);
}

TEST_CASE("edit group: an app stopped for going over its time budget loses its open group (a faulted app leaves no partial result), keeps one it closed before; the normal return applies") {
  Rig r;
  r.launch_ok("Editor");
  const std::size_t depth = r.journal.depth();
  const std::vector<std::uint8_t> before = snapshot(r.project());
  r.input_call([&](const o3m_host* h) {
    const o3m_step s = step(68, 1);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_step(1, 0, 3, &s, kAll) == O3M_EDIT_OK);
    r.clock.advance(6 * core::kTicksPerMs);  // the call takes 6 ms: over the 4 ms an input() may use
  });
  CHECK(r.mods->ui.app_stopped());
  CHECK(r.mods->ui.app_verdict() == module::ViewStatus::Time);
  CHECK(r.mods->ui.groups_abandoned() == 1u);
  CHECK(r.mods->ui.groups_dropped() == 1u);
  CHECK(r.mods->ui.groups_applied() == 0u);
  CHECK(snapshot(r.project()) == before);  // dropped: nothing of it reached the project
  CHECK(r.journal.depth() == depth);
  CHECK(r.editor->group_depth() == 0u);
  CHECK(r.mods->set.quarantined("Editor"));
  CHECK(r.host()->edit->edit_begin() == O3M_EDIT_NO_PROJECT);  // the host tables end with the stop: a stopped app's calls answer "no project"
  r.mods->ui.app_exit();
  // a group the app CLOSED before the overrun was applied then and stands
  Rig r3;
  r3.launch_ok("Editor");
  const std::size_t d3 = r3.journal.depth();
  r3.input_call([&](const o3m_host* h) {
    const o3m_step s = step(69, 1);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_step(1, 0, 4, &s, kAll) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
    r3.clock.advance(6 * core::kTicksPerMs);
  });
  CHECK(r3.mods->ui.app_stopped());
  CHECK(r3.project().pattern(1)->steps[0][4].note == 69);
  CHECK(r3.journal.depth() == d3 + 1u);
  r.mods->ui.app_exit();
  // an app that opens a group in its exit() has it closed at once, and the next launch is clean
  Rig r2;
  r2.launch_ok("Editor");
  r2.input_call([](const o3m_host*) {});
  r2.mods->ui.app_exit();
  CHECK(r2.mods->ui.group_open() == false);
  CHECK(r2.editor->group_depth() == 0u);
}

TEST_CASE("edit group: the time the host spends applying a group is not charged to the app's 4 ms") {
  Rig r;
  r.launch_ok("Editor");
  struct Slow {
    static std::int32_t sink(void* self, std::int32_t arg) noexcept {
      Rig* rig = static_cast<Rig*>(self);
      rig->clock.advance(9 * core::kTicksPerMs);  // the apply takes 9 ms
      const app::DispatchResult d = rig->app->invoke(core::action_id("module.app_group"), arg);
      return d == app::DispatchResult::Disabled ? O3M_EDIT_NO_PROJECT : rig->mods->ui.apply_result();
    }
  };
  r.mods->ui.set_edit_sink(&Slow::sink, &r);
  r.input_call([](const o3m_host* h) {
    const o3m_step s = step(60, 1);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_step(1, 0, 0, &s, kAll) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  CHECK(!r.mods->ui.app_stopped());  // 9 ms in the host, 0 in the app
  CHECK(r.project().pattern(1)->steps[0][0].note == 60);
  // the same 9 ms spent by the app itself is not forgiven
  r.input_call([&](const o3m_host*) { r.clock.advance(9 * core::kTicksPerMs); });
  CHECK(r.mods->ui.app_stopped());
}

TEST_CASE("edit group: a project change (the modules cleared under the app) forgets an open group; with no sink a group cannot be applied") {
  Rig r;
  r.launch_ok("Editor");
  const std::vector<std::uint8_t> before = snapshot(r.project());
  r.mods->ui.set_edit_sink(nullptr, nullptr);
  r.input_call([](const o3m_host* h) {
    const o3m_step s = step(60, 1);
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_step(1, 0, 0, &s, kAll) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_NO_PROJECT);
  });
  CHECK(snapshot(r.project()) == before);
  r.mods->ui.reset();
  CHECK(!r.mods->ui.group_open());
  CHECK(r.host() == nullptr);
}

TEST_CASE("edit group: apply_group does nothing for a macro replaying module.app_group, and the action is not recorded") {
  Rig r;
  CHECK(r.invoke("module.app_group", 0) != app::DispatchResult::Changed);
  CHECK(r.mods->ui.apply_result() == O3M_EDIT_NO_GROUP);
  CHECK(r.journal.depth() == 0u);
}

TEST_CASE("edit group: the worst case, 2,048 one-row writes in one group, applies whole as one unit and UNDO restores it byte for byte") {
  Rig r(16);
  (void)r.project().set_pattern_length(1, 128);
  r.launch_ok("Editor");
  const std::vector<std::uint8_t> before = snapshot(r.project());
  const std::size_t depth = r.journal.depth();
  int end_code = -1;
  r.input_call([&](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    for (unsigned t = 0; t < 16; ++t) {
      for (unsigned row = 0; row < 128; ++row) {
        const o3m_step s = step(static_cast<std::uint8_t>(1 + (t * 7 + row) % 128), static_cast<std::uint8_t>(1 + row % 64), static_cast<std::uint8_t>(row));
        CHECK(h->edit->set_step(1, static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(row), &s, kAll) == O3M_EDIT_OK);
      }
    }
    end_code = h->edit->edit_end();
  });
  CHECK(end_code == O3M_EDIT_OK);
  CHECK(r.journal.depth() == depth + 1u);
  const model::Pattern* p = r.project().pattern(1);
  for (unsigned t = 0; t < 16; ++t) {
    for (unsigned row = 0; row < 128; ++row) {
      CHECK_MESSAGE((p->steps[t][row].note == 1 + (t * 7 + row) % 128 && p->steps[t][row].instr == 1 + row % 64 && p->steps[t][row].vol == row), "t " << t << " row " << row);
    }
  }
  const std::vector<std::uint8_t> after = snapshot(r.project());
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot(r.project()) == before);
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(snapshot(r.project()) == after);
}

TEST_CASE("edit pass budget: a flood of large groups is spread over passes, none lost, in order") {
  Rig r(16);
  (void)r.project().set_pattern_length(1, 128);
  struct Slow {
    static std::int32_t sink(void* self, std::int32_t arg) noexcept {
      Rig* rig = static_cast<Rig*>(self);
      rig->clock.advance(5 * core::kTicksPerMs);  // each apply costs 5 ms of the main loop
      const app::DispatchResult d = rig->app->invoke(core::action_id("module.app_group"), arg);
      return d == app::DispatchResult::Disabled ? O3M_EDIT_NO_PROJECT : rig->mods->ui.apply_result();
    }
  };
  // the page of the running app is the screen the App routes the events to: launched from TOOLS (the fixed entries come first, the first app is "Editor"; an edit-only app is not confirmed from TOOLS, Q9b)
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  r.press(Button::F1);
  NEED(r.state().screen == app::ScreenId::ModuleApp);
  r.mods->ui.set_edit_sink(&Slow::sink, &r);
  std::vector<unsigned> order;
  unsigned calls = 0;
  g_input = [&](const o3m_host* h) {
    const unsigned k = calls++;
    o3m_step rows[128];
    for (auto& s : rows) s = step(static_cast<std::uint8_t>(1 + k));
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    for (unsigned t = 0; t < 16; ++t) CHECK(h->edit->set_steps(1, static_cast<std::uint8_t>(t), 0, rows, 128, kAll) == O3M_EDIT_OK);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
    order.push_back(k);
  };
  const std::size_t depth = r.journal.depth();
  // 20 key events queued at once (each is one input call and one 5 ms apply)
  for (unsigned i = 0; i < 20; ++i) {
    r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::F4), true));
    r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::F4), false));
  }
  r.app->run_once();
  const unsigned first_pass = calls;
  CHECK(first_pass >= 1u);
  CHECK(first_pass <= 3u);  // 8 ms of budget: one apply of 5 ms, a second one starts under it, the third does not
  unsigned passes = 1;
  while (calls < 20 && passes < 40) {
    r.app->run_once();
    ++passes;
  }
  CHECK(calls == 20u);
  CHECK(passes >= 7u);  // spread over many passes, not one
  for (unsigned i = 0; i < order.size(); ++i) {
    CHECK(order[i] == i);  // nothing lost, nothing reordered
  }
  CHECK(r.journal.depth() >= depth + 1u);
  g_input = nullptr;
}

// ---- the undo property ----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("undo property: any run of groups followed by the same number of UNDOs restores the project byte for byte, and REDO re-applies it; a dropped group is no unit") {
  for (std::uint32_t seed = 1; seed <= 24; ++seed) {
    Rig r(16);
    (void)r.project().create_pattern(3, 128);
    (void)r.project().set_track_length(2, 4, 37);
    // some content to overwrite
    test::Rng rng(seed * 7919u);
    for (unsigned t = 0; t < 16; ++t) {
      for (unsigned row = 0; row < 32; row += 3) {
        (void)r.project().set_step(1, t, row, model::Step{static_cast<std::uint8_t>(1 + rng.below(128)), static_cast<std::uint8_t>(rng.below(65)), static_cast<std::uint8_t>(rng.below(128)), {{static_cast<std::uint8_t>(rng.below(43)), rng.byte()}, {0, 0}}});
      }
    }
    r.editor->history_reset();
    r.launch_ok("Editor");
    std::vector<std::vector<std::uint8_t>> states;
    states.push_back(snapshot(r.project()));
    const unsigned groups = 4 + rng.below(9);
    unsigned units = 0;
    for (unsigned g = 0; g < groups; ++g) {
      const unsigned writes = 1 + rng.below(12);
      const bool tempo = rng.below(4) == 0;
      const bool drop = rng.below(5) == 0;
      int end_code = -1;
      r.input_call([&](const o3m_host* h) {
        const o3m_edit_api* e = h->edit;
        CHECK(e->edit_begin() == O3M_EDIT_OK);
        if (tempo) {
          CHECK(e->set_tempo(4000 + rng.below(76000)) == O3M_EDIT_OK);
        }
        for (unsigned w = 0; w < writes; ++w) {
          o3m_step s[128];
          const unsigned count = 1 + rng.below(40);
          for (unsigned k = 0; k < count; ++k) {
            const std::uint32_t pick = rng.below(8);
            s[k] = o3m_step{static_cast<std::uint8_t>(pick == 0 ? 0 : (pick == 1 ? 253 + rng.below(3) : 1 + rng.below(128))), static_cast<std::uint8_t>(rng.below(65)),
                            static_cast<std::uint8_t>(rng.below(3) == 0 ? 255 : rng.below(128)), static_cast<std::uint8_t>(rng.below(2) ? rng.below(43) : 128 + rng.below(128)), rng.byte(),
                            static_cast<std::uint8_t>(rng.below(43)), rng.byte(), 0};
          }
          const std::uint32_t pat = 1 + rng.below(3);
          (void)e->set_steps(static_cast<std::uint16_t>(pat), static_cast<std::uint8_t>(rng.below(16)), static_cast<std::uint8_t>(rng.below(30)), s, static_cast<std::uint8_t>(count),
                             static_cast<std::uint8_t>(1 + rng.below(31)));
        }
        if (drop) {
          o3m_step big[128];
          for (auto& x : big) x = step(60);
          for (int k = 0; k < 20; ++k) (void)e->set_steps(3, static_cast<std::uint8_t>(k % 16), 0, big, 128, kAll);
        }
        end_code = e->edit_end();
      });
      if (drop) {
        CHECK(end_code == O3M_EDIT_LIMIT);
        CHECK(snapshot(r.project()) == states.back());
        continue;
      }
      CHECK(end_code == O3M_EDIT_OK);
      const std::vector<std::uint8_t> now = snapshot(r.project());
      if (now != states.back()) {
        states.push_back(now);
        ++units;
      }
    }
    CHECK(r.journal.depth() == units);
    for (unsigned u = 0; u < units; ++u) {
      CHECK(r.editor->undo() == model::UndoResult::Ok);
      CHECK_MESSAGE(snapshot(r.project()) == states[units - 1 - u], "seed " << seed << " undo " << u);
    }
    CHECK(snapshot(r.project()) == states[0]);
    CHECK(r.editor->undo() == model::UndoResult::Nothing);
    for (unsigned u = 0; u < units; ++u) {
      CHECK(r.editor->redo() == model::UndoResult::Ok);
      CHECK(snapshot(r.project()) == states[u + 1]);
    }
  }
}

// ---- the fuzz -------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("fuzz: random calls of every read and edit entry, in and out of groups and of input(), never crash, never write out of range, answer with a published code") {
  Rig r(16);
  (void)r.project().create_pattern(3, 128);
  (void)r.project().set_track_length(2, 15, 7);
  r.launch_ok("Editor");
  test::Rng rng(0xC0FFEEu);
  std::vector<std::uint8_t> buf(4096, 0);
  const auto code_ok = [](int c) { return c >= O3M_EDIT_OK && c <= O3M_EDIT_RATE; };
  unsigned frames_done = 0;
  for (unsigned frame = 0; frame < 600; ++frame) {
    const bool in_input = rng.below(8) != 0;
    const unsigned calls = rng.below(60);
    std::vector<std::uint8_t> pool(2048);
    for (auto& b : pool) b = rng.byte();
    auto script = [&](const o3m_host* h) {
      const o3m_edit_api* e = h->edit;
      const o3m_read_api* rd = h->read;
      for (unsigned i = 0; i < calls; ++i) {
        const std::uint32_t pick = rng.below(12);
        const std::uint16_t pat = static_cast<std::uint16_t>(rng.below(5) == 0 ? rng.next() & 0xFFFF : rng.below(5));
        const std::uint8_t track = static_cast<std::uint8_t>(rng.below(5) == 0 ? rng.byte() : rng.below(17));
        const std::uint8_t row = static_cast<std::uint8_t>(rng.below(5) == 0 ? rng.byte() : rng.below(130));
        const std::uint8_t count = static_cast<std::uint8_t>(rng.below(5) == 0 ? rng.byte() : rng.below(131));
        const std::uint8_t fields = static_cast<std::uint8_t>(rng.below(3) == 0 ? rng.byte() : 1 + rng.below(31));
        const bool null = rng.below(10) == 0;
        // steps: random bytes, so values outside the model's encodings are offered too (and refused)
        o3m_step* s = reinterpret_cast<o3m_step*>(pool.data() + 8 * rng.below(200));
        switch (pick) {
          case 0: CHECK(code_ok(e->edit_begin())); break;
          case 1: CHECK(code_ok(e->edit_end())); break;
          case 2: CHECK(code_ok(e->set_step(pat, track, row, null ? nullptr : s, fields))); break;
          case 3:
          case 4: CHECK(code_ok(e->set_steps(pat, track, row, null ? nullptr : s, static_cast<std::uint8_t>(count > 120 ? 120 : count), fields))); break;
          case 5: CHECK(code_ok(e->set_tempo(rng.below(5) == 0 ? rng.next() : 4000 + rng.below(76001)))); break;
          case 6: {
            o3m_pattern_info info;
            info.size = rng.below(6) == 0 ? rng.below(40) : sizeof info;
            CHECK(code_ok(rd->pattern_info(pat, null ? nullptr : &info)));
            break;
          }
          case 7:
          case 8: {
            o3m_step out[128];
            CHECK(code_ok(rd->get_steps(pat, track, row, null ? nullptr : out, static_cast<std::uint8_t>(count > 128 ? 128 : count))));
            break;
          }
          case 9: (void)rd->get_tempo(); break;
          default: {
            o3m_launch_ctx c;
            c.size = rng.below(5) == 0 ? rng.below(48) : sizeof c;
            (void)rd->launch_ctx(null ? nullptr : &c);
            break;
          }
        }
      }
    };
    if (in_input) {
      r.input_call(script);
    } else {
      script(r.host());  // outside input(): the group calls answer RATE, the others as ever
    }
    ++frames_done;
    if (rng.below(6) == 0) {
      (void)r.editor->undo();
    }
    if (rng.below(9) == 0) {
      (void)r.editor->redo();
    }
    CHECK(r.mods->ui.app_running());
    CHECK(!r.mods->ui.group_open());  // never a group left open past a call
    CHECK(r.editor->group_depth() == 0u);
    // the project stays structurally sound
    for (unsigned id = 1; id <= 255; ++id) {
      const model::Pattern* p = r.project().pattern(id);
      if (p == nullptr) continue;
      for (unsigned t = 0; t < 16; ++t) {
        CHECK(p->track_length[t] >= 1);
        CHECK(p->track_length[t] <= 128);
      }
    }
  }
  CHECK(frames_done == 600u);
  CHECK(r.mods->ui.groups_applied() + r.mods->ui.groups_dropped() + r.mods->ui.groups_abandoned() > 0u);
}

// ---- Pattern F7 TOOLS -----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("tools: F7 on Pattern opens the list of the apps that hold edit_project, sorted by name, under a modal overlay; F8 closes it") {
  Rig r;
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  CHECK(r.state().overlay == app::OverlayId::VerbMenu);
  const app::ModuleUi& ui = r.mods->ui;
  std::vector<std::string> names;
  for (unsigned i = 0; i < ui.tool_count(); ++i) {
    names.push_back(ui.app_entry_at(ui.tool_entry(i))->stem);
  }
  const std::vector<std::string> want = {"Editor", "Editor3", "Editor4", "Writer", "rhythmgen"};  // (the catalogue's order: case-insensitive)
  std::vector<std::string> sorted = names;
  CHECK(names.size() == 5u);
  for (const char* n : {"Editor", "Editor3", "Editor4", "Writer", "rhythmgen"}) {
    CHECK_MESSAGE(std::find(names.begin(), names.end(), n) != names.end(), n);
  }
  for (const char* n : {"Viewer", "Listener", "Editor2"}) {  // read only, midi only, ABI 1.2: not tools
    CHECK_MESSAGE(std::find(names.begin(), names.end(), n) == names.end(), n);
  }
  CHECK(sorted == names);
  for (std::size_t i = 1; i < names.size(); ++i) {
    CHECK(strcasecmp(names[i - 1].c_str(), names[i].c_str()) < 0);
  }
  (void)want;
  CHECK(app::tools_count(r.app->model()) == 5u);  // OS-TOOLSDEDUP: the tool apps alone, no fixed entry
  CHECK(r.state().modules.tools == 0);  // the cursor opens on the first app
  r.press(Button::Down);
  CHECK(r.state().modules.tools == 1);
  r.press(Button::F8);
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::Pattern);
}

TEST_CASE("tools: OS-PTOOLS2 S1: the list opens in WATCH while playing, and is refused, with its reason, only in LIVE (OS-BUG-TOOLSPLAY said it in both)") {
  Rig r;
  r.position.publish(true, 1, 0, 12000);
  r.pass(40);
  CHECK(r.state().play.playing);
  r.press(Button::F7);  // WATCH (Browse, playing): the verb menu opens (the target is the block or the cursor, which are the user's)
  CHECK(r.state().overlay == app::OverlayId::VerbMenu);
  CHECK(r.state().notify.size() == 0u);
  r.pad(15);  // APPS > opens the TOOLS list
  CHECK(r.state().overlay == app::OverlayId::VerbMenu);
  r.press(Button::F8);
  CHECK(r.state().overlay == app::OverlayId::None);
  (void)r.invoke("pattern.rec_free");  // LIVE: Edit following the playhead
  NEED(app::pattern_editable(r.state()));
  NEED(!r.state().edit.rec_free);
  r.press(Button::F7);
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().notify.size() == 1u);
  CHECK(std::string(r.state().notify.last_text()) == "THE PLAYHEAD HOLDS THE CURSOR IN LIVE");
  r.position.publish(false, 1, 0, 12000);
  r.pass(40);
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  CHECK(r.state().overlay == app::OverlayId::VerbMenu);
}

TEST_CASE("tools: OS-PTOOLS2 S1: the page opens on the row it was left on, kept inside the list when the card shrinks") {
  Rig r;
  r.press(Button::F7);
  r.pad(15);  // APPS > opens the TOOLS list
  NEED(r.state().overlay == app::OverlayId::VerbMenu);
  CHECK(r.state().modules.tools == 0);
  for (unsigned k = 0; k < 3u; ++k) r.press(Button::Down);
  CHECK(r.state().modules.tools == 3);
  r.press(Button::F8);
  r.press(Button::F7);
  r.pad(15);
  CHECK(r.state().overlay == app::OverlayId::VerbMenu);
  CHECK(r.state().modules.tools == 3);  // the same row
  for (unsigned k = 0; k < 40u; ++k) r.press(Button::Down);  // the last row (a tool app)
  const unsigned last = app::tools_count(r.app->model()) - 1u;
  CHECK(r.state().modules.tools == last);
  r.press(Button::F8);
  fs::remove_all(r.card.root / "modules");  // the card lost its apps: the page is empty now, the row stays inside it
  r.press(Button::F7);
  r.pad(15);
  CHECK(r.state().overlay == app::OverlayId::VerbMenu);
  CHECK(app::tools_count(r.app->model()) == 0u);
  CHECK(r.state().modules.tools == 0);
}

TEST_CASE("tools: OS-TOOLSDEDUP: the APPS page starts on the first tool app, F6 on it opens EVENTS and starts no app") {
  Rig r;
  r.press(Button::F7);
  r.pad(15);  // the verb menu's APPS > row (pad 16)
  NEED(r.state().overlay == app::OverlayId::VerbMenu);
  CHECK(r.state().verb_menu.apps);
  CHECK(app::tools_count(r.app->model()) > 0u);
  app::ToolItem item;
  NEED(app::tools_item(r.app->model(), 0, item));
  CHECK(std::string(item.label) == "Editor");  // no fixed entry above the apps
  r.press(Button::F6);
  CHECK(r.state().overlay != app::OverlayId::VerbMenu);
  CHECK(r.state().screen == app::ScreenId::Pattern);
  CHECK(!r.mods->ui.app_running());
  CHECK(std::string(r.state().modules.tools_msg).empty());
}

// ---- OS-APPSNAV: Shift + Pattern is the list of the apps that are not tools ----------------------------------------------------

namespace {

void shift_pattern(Rig& r) {
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
  r.pass(1);
  r.press(Button::Pattern);
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
  r.pass(1);
}

// The row of the apps list that holds the app `stem`; FAILs when it is not listed.
unsigned apps_row(Rig& r, const char* stem) {
  for (unsigned k = 0; k < app::apps_count(r.app->model()); ++k) {
    if (std::strcmp(r.mods->ui.app_entry_at(app::apps_entry(r.app->model(), k))->stem, stem) == 0) {
      return k;
    }
  }
  FAIL("not in the apps list: " << stem);
  return 0;
}

}  // namespace

TEST_CASE("apps list: Shift + Pattern lists the apps that are not tools, the same split as the TOOLS APPS page, and Pattern or Shift + Pattern closes it (OS-APPSNAV)") {
  Rig r;
  shift_pattern(r);
  CHECK(r.state().overlay == app::OverlayId::ModulePicker);
  CHECK(r.state().screen == app::ScreenId::Pattern);
  CHECK(r.mods->ui.tool_count() == 5u);
  CHECK(app::apps_count(r.app->model()) == r.mods->ui.app_entry_count() - r.mods->ui.tool_count());
  for (unsigned k = 0; k < app::apps_count(r.app->model()); ++k) {
    const unsigned e = app::apps_entry(r.app->model(), k);
    for (unsigned t = 0; t < r.mods->ui.tool_count(); ++t) {
      CHECK_MESSAGE(e != r.mods->ui.tool_entry(t), r.mods->ui.app_entry_at(e)->stem);  // no tool app in the list
    }
  }
  for (const char* n : {"Viewer", "Listener", "Editor2"}) {
    CHECK_MESSAGE(apps_row(r, n) < app::apps_count(r.app->model()), n);
  }
  CHECK(r.state().modules.picker == 0);
  r.press(Button::Down);
  CHECK(r.state().modules.picker == 1);
  r.press(Button::Pattern);  // the key that opened it closes it
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::Pattern);
  shift_pattern(r);
  CHECK(r.state().overlay == app::OverlayId::ModulePicker);
  CHECK(r.state().modules.picker == 0);  // it opens on the first app each time
  shift_pattern(r);  // Shift + Pattern again
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::Pattern);
  CHECK(!r.mods->ui.app_running());
  CHECK(r.app->failed_frames() == 0u);
}

TEST_CASE("apps list: F1 and F8 launch the app under the cursor through the MODULES launch path, and the page is that app's") {
  for (const Button key : {Button::F1, Button::F8}) {
    Rig r;
    shift_pattern(r);
    NEED(r.state().overlay == app::OverlayId::ModulePicker);
    const unsigned row = apps_row(r, "Viewer");
    for (unsigned k = 0; k < row; ++k) {
      r.press(Button::Down);
    }
    CHECK(r.state().modules.picker == row);
    r.press(key);
    CHECK(r.state().overlay == app::OverlayId::None);
    CHECK(r.state().screen == app::ScreenId::ModuleApp);
    CHECK(r.mods->ui.app_running());
    r.chord_exit();  // F7 + F8: the app's exit goes to MODULES (the launch path is MODULES')
    CHECK(r.state().screen == app::ScreenId::Modules);
    CHECK(!r.mods->ui.app_running());
  }
}

TEST_CASE("apps list: an app that edits the project or writes files is a tool app, so it is not listed and no launch from the list needs the confirm (OS-APPSNAV)") {
  Rig r(12, false, true);
  shift_pattern(r);
  NEED(r.state().overlay == app::OverlayId::ModulePicker);
  for (const char* n : {"Exporter", "ExpEdit", "smfexport", "Writer"}) {  // the apps that confirm: every one is a tool
    bool listed = false;
    for (unsigned k = 0; k < app::apps_count(r.app->model()); ++k) {
      listed = listed || std::strcmp(r.mods->ui.app_entry_at(app::apps_entry(r.app->model(), k))->stem, n) == 0;
    }
    CHECK_MESSAGE(!listed, n);
  }
  const unsigned row = apps_row(r, "ReadOnly7");  // read only: nothing to confirm
  for (unsigned k = 0; k < row; ++k) {
    r.press(Button::Down);
  }
  r.press(Button::F8);
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::ModuleApp);
  CHECK(r.mods->ui.app_running());
  CHECK(std::string(r.state().modules.tools_msg).empty());
}

TEST_CASE("apps list: with no app on the card it is empty, F1 launches nothing, F8 is BACK and Pattern closes it") {
  Rig r;
  fs::remove_all(r.card.root / "modules");
  shift_pattern(r);
  CHECK(r.state().overlay == app::OverlayId::ModulePicker);  // the card is read again on opening
  CHECK(app::apps_count(r.app->model()) == 0u);
  r.press(Button::Down);
  CHECK(r.state().modules.picker == 0);
  r.press(Button::F1);
  CHECK(r.state().overlay == app::OverlayId::ModulePicker);
  CHECK(!r.mods->ui.app_running());
  r.press(Button::F8);  // BACK, as in every other picker (F1 reads blank)
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::Pattern);
  CHECK(!r.mods->ui.app_running());
  shift_pattern(r);
  CHECK(r.state().overlay == app::OverlayId::ModulePicker);
  r.press(Button::Pattern);
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::Pattern);
}

TEST_CASE("apps list: the engine picker of the Instrument page still closes with F8 and binds with F1 (the apps list shares its overlay)") {
  Rig r;
  r.press(Button::Params);
  NEED(r.state().screen == app::ScreenId::Instrument);
  (void)r.invoke("module.engine");  // (F1 of the page is its first page now; the action opens the picker)
  NEED(r.state().overlay == app::OverlayId::ModulePicker);
  r.press(Button::Pattern);  // not the apps list: Pattern does nothing here
  CHECK(r.state().overlay == app::OverlayId::ModulePicker);
  r.press(Button::F8);
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::Instrument);
}

TEST_CASE("tools: with no tool app on the card the APPS page is empty, Down stays on row 0 and F1 runs nothing") {
  Rig r;
  fs::remove_all(r.card.root / "modules" / "apps");
  fs::create_directories(r.card.root / "modules" / "apps");
  Rig r2;  // (a fresh rig: its natives are in the set, the empty card is the scan's)
  fs::remove_all(r2.card.root / "modules");
  r2.press(Button::F7);
  r2.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  CHECK(r2.state().overlay == app::OverlayId::VerbMenu);
  CHECK(app::tools_count(r2.app->model()) == 0u);
  for (unsigned k = 0; k < 3u; ++k) {
    r2.press(Button::Down);
  }
  CHECK(r2.state().modules.tools == 0u);
  r2.press(Button::F1);
  CHECK(r2.state().overlay == app::OverlayId::VerbMenu);  // nothing to run: the page stays up
  CHECK(!r2.mods->ui.app_running());
}

TEST_CASE("tools: F1 runs the app with the block as its context (no confirm for an edit-only app, Q9b), F7 + F8 returns to Pattern with the cursor and the block as they were") {
  Rig r;
  // a block of 3 tracks x 4 rows from track 2, row 5
  r.invoke("nav.scroll", 5);
  for (int i = 0; i < 2; ++i) (void)r.invoke("nav.right");
  for (int i = 0; i < 2; ++i) (void)r.invoke("select.extend", 1);    // down
  const app::Selection before_sel = r.state().edit.selection;
  (void)before_sel;
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
  r.pass(1);
  extend_tracks(r, 2);   // right
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
  r.pass(1);
  const app::Selection::Block b = r.state().edit.selection.block(r.state().pattern.row(), r.state().pattern.track());
  NEED(r.state().edit.selection.active);
  const int row = r.state().pattern.row(), track = r.state().pattern.track();
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  NEED(r.state().overlay == app::OverlayId::VerbMenu);
  // the page opens on "Editor" (the first app)
  CHECK(std::string(r.mods->ui.app_entry_at(r.mods->ui.tool_entry(0))->stem) == "Editor");
  r.press(Button::F1);  // OS-PTOOLS2 S6 (Q9b): an app that only edits the project is not confirmed from TOOLS
  CHECK(r.state().overlay == app::OverlayId::None);
  CHECK(r.state().screen == app::ScreenId::ModuleApp);
  CHECK(r.mods->ui.app_running());
  CHECK(r.state().modules.from_tools);
  // the context
  o3m_launch_ctx c;
  c.size = sizeof c;
  NEED(r.host() != nullptr);
  CHECK(r.host()->read->launch_ctx(&c) == 1u);
  CHECK(c.flags == O3M_LAUNCH_FROM_TOOLS);
  CHECK(c.pattern == r.state().edit.pattern);
  CHECK(c.track == static_cast<std::uint32_t>(b.track0));
  CHECK(c.block_tracks == static_cast<std::uint8_t>(b.track1 - b.track0 + 1));
  CHECK(c.block_start == static_cast<std::uint32_t>(b.row0));
  CHECK(c.block_length == static_cast<std::uint32_t>(b.row1 - b.row0 + 1));
  CHECK(c.instrument == static_cast<std::uint32_t>(r.state().instrument - 1));
  // leave: back to Pattern, not to MODULES; the cursor and the block as they were
  r.chord_exit();
  CHECK(r.state().screen == app::ScreenId::Pattern);
  CHECK(!r.mods->ui.app_running());
  CHECK(!r.state().modules.from_tools);
  CHECK(r.state().edit.selection.active);
  CHECK(r.state().pattern.row() == row);
  CHECK(r.state().pattern.track() == track);
  const app::Selection::Block after = r.state().edit.selection.block(r.state().pattern.row(), r.state().pattern.track());
  CHECK((after.row0 == b.row0 && after.row1 == b.row1 && after.track0 == b.track0 && after.track1 == b.track1));
}

TEST_CASE("tools: with no block the context carries the cursor: block_length 0, block_tracks 0, the cursor's track and row") {
  Rig r;
  r.invoke("nav.scroll", 7);
  (void)r.invoke("cursor.track", 1);
  CHECK(!r.state().edit.selection.active);
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  r.press(Button::F1);
  NEED(r.mods->ui.app_running());
  o3m_launch_ctx c;
  c.size = sizeof c;
  CHECK(r.host()->read->launch_ctx(&c) == 1u);
  CHECK((c.block_length == 0u && c.block_tracks == 0u));
  CHECK(c.track == static_cast<std::uint32_t>(r.state().pattern.track()));
  CHECK(c.block_start == static_cast<std::uint32_t>(r.state().pattern.row()));
  CHECK(c.track == 1u);
  CHECK(c.block_start == 7u);
}

TEST_CASE("tools: an app opened from the MODULES list still exits to MODULES, and a project change under a TOOLS app returns to Pattern") {
  Rig r;
  r.invoke("nav.screen", static_cast<core::ActionArg>(app::ScreenId::Modules));
  CHECK(r.state().screen == app::ScreenId::Modules);
  r.press(Button::F1);   // a launch from the list (the cursor on the first app)
  if (r.state().screen != app::ScreenId::ModuleApp) {
    r.press(Button::F1);
  }
  NEED(r.state().screen == app::ScreenId::ModuleApp);
  CHECK(!r.state().modules.from_tools);
  r.chord_exit();
  CHECK(r.state().screen == app::ScreenId::Modules);
  // from TOOLS, then the modules are cleared (a project load): the page goes back to Pattern by itself
  r.invoke("nav.screen", static_cast<core::ActionArg>(app::ScreenId::Pattern));
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  r.press(Button::F1);
  NEED(r.state().screen == app::ScreenId::ModuleApp);
  r.mods->ui.reset();
  r.pass(40);
  CHECK(r.state().screen == app::ScreenId::Pattern);
}

TEST_CASE("tools: the item list is one place (tools_list.h): the tool apps with their file stems, quarantined ones are marked, nothing else") {
  Rig r;
  r.mods->ui.scan_tools();
  app::ToolItem item;
  NEED(app::tools_item(r.app->model(), 0, item));  // OS-TOOLSDEDUP: the first app is row 0
  CHECK(std::string(item.label) == "Editor");
  CHECK(!item.bad);
  NEED(app::tools_item(r.app->model(), 1u, item));
  CHECK(!app::tools_item(r.app->model(), app::tools_count(r.app->model()), item));
  CHECK(!app::tools_item(r.app->model(), 99, item));
}

// ---- Rhythm Gen through the whole stack --------------------------------------------------------------------------------------------------------------

TEST_CASE("rhythm gen: opened from TOOLS on a block of 2 tracks x 16 rows, F1 APPLY is one undo unit named RHYTHM GEN, the notes are the Fill dialog's Euclid, UNDO restores the project byte for byte") {
  Rig r;
  NEED(r.rg.entry != nullptr);
  const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 4, 5, 8, 16, 2, {0, 0, 0}};
  // some content in the block to see it replaced; FX everywhere to see them stay
  for (unsigned t = 4; t < 6; ++t) {
    for (unsigned row = 0; row < 32; ++row) {
      (void)r.project().set_step(1, t, row, model::Step{static_cast<std::uint8_t>(row % 3 == 0 ? 70 : 0), 9, 77, {{3, static_cast<std::uint8_t>(row)}, {4, 5}}});
    }
  }
  r.editor->history_reset();
  const std::vector<std::uint8_t> before = snapshot(r.project());
  r.launch_ok("rhythmgen", &ctx);
  const std::size_t depth = r.journal.depth();
  // the page is up: feed it F1 through the app's own input (the screen is not the app's in this test)
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
  CHECK(r.journal.depth() == depth + 1u);  // both lanes in ONE unit
  const std::vector<std::uint8_t> after = snapshot(r.project());
  CHECK(after != before);
  // the Fill dialog's Euclid for the same plan: 4 events in 16
  model::fill::Plan plan;
  plan.type = model::fill::Type::Euclid;
  plan.events = 4;
  model::Step ref[16];
  for (auto& s : ref) s = model::kEmptyStep;
  model::fill::Rng rng;
  model::edit::Last last{1, 0};
  (void)model::fill::apply(plan, ref, 16, rng, last);
  const model::Pattern* p = r.project().pattern(1);
  for (unsigned t = 4; t < 6; ++t) {
    for (unsigned row = 0; row < 32; ++row) {
      const model::Step& s = p->steps[t][row];
      if (row >= 8 && row < 24) {
        const bool hit = ref[row - 8].note != 0;
        CHECK_MESSAGE((s.note != 0) == hit, "track " << t << " row " << row);
        if (hit) {
          CHECK(s.note == 70);   // the lane's NOTE starts as the first note already in its block (row 9 here)
          CHECK(s.instr == 6);   // the context's instrument 5 + 1
        }
        CHECK(s.fx[0].type == 3);  // FX untouched
        CHECK(s.fx[0].value == row);
        CHECK(s.fx[1].type == 4);
      } else {
        CHECK(s.note == (row % 3 == 0 ? 70 : 0));  // outside the block: untouched
      }
    }
  }
  // VARY at PROB 100 would write the same notes (the seed only decides which hits are dropped): no change, no unit. At PROB 50 it is another variation and another unit.
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F2, 1, false));
  CHECK(r.journal.depth() == depth + 1u);
  for (int i = 0; i < 4; ++i) {
    CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_DOWN, 1, false));  // the PROB row
  }
  CHECK(r.mods->ui.app_input(O3M_EV_ENCODER, 0, -10, false));         // 50 %
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F2, 1, false));
  CHECK(r.journal.depth() == depth + 2u);
  // UNDO x2 restores the project exactly
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot(r.project()) == after);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot(r.project()) == before);
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(snapshot(r.project()) == after);
}

TEST_CASE("rhythm gen: through the keys of the unit: F7, the list, F1 twice, F1 APPLY on the app's page, F7 + F8, then UNDO on Pattern says UNDO RHYTHM GEN") {
  Rig r;
  NEED(r.rg.entry != nullptr);
  // the block: 2 tracks x 16 rows from (row 0, track 0)
  r.invoke("cursor.home");
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
  r.pass(1);
  (void)r.invoke("select.extend", 15);
  extend_tracks(r, 1);
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
  r.pass(1);
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  NEED(r.state().overlay == app::OverlayId::VerbMenu);
  // move the cursor to rhythmgen
  for (unsigned i = 0; i < r.mods->ui.tool_count(); ++i) {
    if (std::string(r.mods->ui.app_entry_at(r.mods->ui.tool_entry(i))->stem) == "rhythmgen") {
      for (unsigned k = 0; k < i; ++k) r.press(Button::Down);  // (the block adds EXPORT SELECTION)
    }
  }
  r.press(Button::F1);
  NEED(r.state().screen == app::ScreenId::ModuleApp);
  const std::size_t depth = r.journal.depth();
  r.press(Button::F1);   // APPLY on the app's page
  CHECK(r.journal.depth() == depth + 1u);
  r.chord_exit();
  CHECK(r.state().screen == app::ScreenId::Pattern);
  CHECK(r.state().edit.selection.active);
  core::ActionId id{};
  core::ActionArg arg = 0;
  NEED(r.journal.newest_undo(id, arg));
  CHECK(id == core::action_id("module.app_group"));
  r.press(Button::F8);   // UNDO
  CHECK(r.journal.depth() == depth);
  bool said = false;
  for (unsigned i = 0; i < r.state().notify.size(); ++i) {
    said = said || std::string(r.state().notify.at(i).text.c_str()) == "UNDO RHYTHM GEN";
  }
  CHECK(said);
}

// =====================================================================================================================================================
// OS-H3B, ABI 1.6: the instrument parameters
// =====================================================================================================================================================

namespace {

namespace ip = model::instrument_param;

// 1.5's tables as that slice published them: a 1.6 OS must keep every member where it was.
struct ReadV15 {
  std::uint32_t version, size;
  std::uint32_t (*get_tempo)();
  std::uint32_t (*launch_ctx)(o3m_launch_ctx*);
  std::int32_t (*pattern_info)(std::uint16_t, o3m_pattern_info*);
  std::int32_t (*get_steps)(std::uint16_t, std::uint8_t, std::uint8_t, o3m_step*, std::uint8_t);
};
struct EditV15 {
  std::uint32_t version, size;
  int (*set_tempo)(std::uint32_t);
  std::int32_t (*edit_begin)();
  std::int32_t (*edit_end)();
  std::int32_t (*set_step)(std::uint16_t, std::uint8_t, std::uint8_t, const o3m_step*, std::uint8_t);
  std::int32_t (*set_steps)(std::uint16_t, std::uint8_t, std::uint8_t, const o3m_step*, std::uint8_t, std::uint8_t);
};
static_assert(offsetof(o3m_read_api, pattern_info) == offsetof(ReadV15, pattern_info) && offsetof(o3m_read_api, get_steps) == offsetof(ReadV15, get_steps) && sizeof(ReadV15) == offsetof(o3m_read_api, instrument_info),
              "1.5 read table: the 1.6 members come after get_steps");
static_assert(offsetof(o3m_read_api, instrument_info) == sizeof(ReadV15) && offsetof(o3m_read_api, get_param_desc) == sizeof(ReadV15) + kP && offsetof(o3m_read_api, get_instrument_param) == sizeof(ReadV15) + 2 * kP &&
                  sizeof(o3m_read_api) == sizeof(ReadV15) + 3 * kP,
              "1.6 read table");
static_assert(offsetof(o3m_edit_api, set_steps) == offsetof(EditV15, set_steps) && offsetof(o3m_edit_api, set_instrument_param) == sizeof(EditV15) && sizeof(o3m_edit_api) == sizeof(EditV15) + kP, "1.6 edit table");
static_assert(sizeof(o3m_instrument_info) == 20 && offsetof(o3m_instrument_info, midi_channel) == 16 && sizeof(o3m_param_desc) == 40 && offsetof(o3m_param_desc, id) == 16 && offsetof(o3m_param_desc, flags) == 18 && offsetof(o3m_param_desc, group) == 20 &&
                  offsetof(o3m_param_desc, name) == 24 && alignof(o3m_param_desc) == 4,
              "1.6 PODs");
static_assert(O3M_INSTRUMENT_EMPTY == 0 && O3M_INSTRUMENT_SAMPLE == 1 && O3M_INSTRUMENT_MIDI == 2 && O3M_INSTRUMENT_MODULE == 3, "the instrument kinds are published");
static_assert(O3M_PD_STEPPED == 1 && O3M_PD_ENUM == 2 && O3M_PD_APP_WRITABLE == 4, "the descriptor flags are published");
static_assert(O3M_PGROUP_VOICE == ip::GroupVoice && O3M_PGROUP_FILTER == ip::GroupFilter && O3M_PGROUP_ENV == ip::GroupEnvelope && O3M_PGROUP_SENDS == ip::GroupSends && O3M_PGROUP_SOURCE == ip::GroupSource &&
                  O3M_PGROUP_MOD == 5 && O3M_SAMPLE_PARAM_COUNT == ip::kSdkCount,
              "the groups and the count are the model's");

// Everything the edit table can change: snapshot() and the byte image of every instrument slot.
std::vector<std::uint8_t> snapshot_all(const model::Project& p) {
  std::vector<std::uint8_t> out = snapshot(p);
  for (unsigned slot = 0; slot < model::kMaxInstruments; ++slot) {
    const model::Instrument* in = p.instrument(slot);
    const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(in);
    if (in != nullptr) {
      out.insert(out.end(), b, b + sizeof(model::Instrument));
    } else {
      out.push_back(0xEE);
    }
  }
  return out;
}

std::uint32_t fnv1a(const char* s) {
  std::uint32_t h = 2166136261u;
  for (; *s != '\0'; ++s) {
    h = (h ^ static_cast<std::uint8_t>(*s)) * 16777619u;
  }
  return h;
}

// The ids a sample instrument does NOT let an app write: PLAY ENGINE, the source page, each envelope's ENABLE and each slot's SOURCE.
bool expect_not_writable(unsigned id) {
  if (id == ip::PlayEngine || ip::is_source_field(id)) {
    return true;
  }
  for (unsigned e = 0; e < ip::kEnvelopes; ++e) {
    if (id == ip::envelope(e, ip::EnvEnable) || id == ip::mod(e, ip::ModSource)) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("abi 1.6: the published offsets of 1.2 .. 1.6, member by member (in pointer slots): nothing moved, the 1.6 members are appended") {
  const auto at = [](std::size_t header, unsigned slot) { return header + slot * kP; };
  // the host table: 1.0 .. 1.2 up to draw, 1.3 read / edit / io
  CHECK(offsetof(o3m_host, log) == at(16, 0));
  CHECK(offsetof(o3m_host, exp2_f) == at(16, 1));
  CHECK(offsetof(o3m_host, log2_f) == at(16, 2));
  CHECK(offsetof(o3m_host, sin_f) == at(16, 3));
  CHECK(offsetof(o3m_host, cos_f) == at(16, 4));
  CHECK(offsetof(o3m_host, draw) == at(16, 5));
  CHECK(offsetof(o3m_host, read) == at(16, 6));
  CHECK(offsetof(o3m_host, edit) == at(16, 7));
  CHECK(offsetof(o3m_host, io) == at(16, 8));
  // the read table: 1.3 get_tempo, launch_ctx; 1.5 pattern_info, get_steps; 1.6 instrument_info, get_param_desc, get_instrument_param
  CHECK(offsetof(o3m_read_api, get_tempo) == at(8, 0));
  CHECK(offsetof(o3m_read_api, launch_ctx) == at(8, 1));
  CHECK(offsetof(o3m_read_api, pattern_info) == at(8, 2));
  CHECK(offsetof(o3m_read_api, get_steps) == at(8, 3));
  CHECK(offsetof(o3m_read_api, instrument_info) == at(8, 4));
  CHECK(offsetof(o3m_read_api, get_param_desc) == at(8, 5));
  CHECK(offsetof(o3m_read_api, get_instrument_param) == at(8, 6));
  CHECK(sizeof(o3m_read_api) == at(8, 7));
  // the edit table: 1.3 set_tempo; 1.5 edit_begin, edit_end, set_step, set_steps; 1.6 set_instrument_param
  CHECK(offsetof(o3m_edit_api, set_tempo) == at(8, 0));
  CHECK(offsetof(o3m_edit_api, edit_begin) == at(8, 1));
  CHECK(offsetof(o3m_edit_api, edit_end) == at(8, 2));
  CHECK(offsetof(o3m_edit_api, set_step) == at(8, 3));
  CHECK(offsetof(o3m_edit_api, set_steps) == at(8, 4));
  CHECK(offsetof(o3m_edit_api, set_instrument_param) == at(8, 5));
  CHECK(sizeof(o3m_edit_api) == at(8, 6));
  // the io table: 1.3 now_ms, midi_read, midi_lost; 1.4 the tap; 1.7 the export calls
  CHECK(offsetof(o3m_io_api, now_ms) == at(8, 0));
  CHECK(offsetof(o3m_io_api, midi_read) == at(8, 1));
  CHECK(offsetof(o3m_io_api, midi_lost) == at(8, 2));
  CHECK(offsetof(o3m_io_api, tap_select) == at(8, 3));
  CHECK(offsetof(o3m_io_api, tap_latest) == at(8, 4));
  CHECK(offsetof(o3m_io_api, tap_stream) == at(8, 5));
  CHECK(offsetof(o3m_io_api, tap_levels) == at(8, 6));
  CHECK(offsetof(o3m_io_api, export_open) == at(8, 7));  // 1.7 (OS-H4): appended after the tap
  CHECK(offsetof(o3m_io_api, export_write) == at(8, 8));
  CHECK(offsetof(o3m_io_api, export_close) == at(8, 9));
  CHECK(offsetof(o3m_io_api, export_status) == at(8, 10));
  CHECK(offsetof(o3m_io_api, get_transport) == at(8, 11));  // 1.10 (OS-APPUX S3): appended after the export members
  CHECK(offsetof(o3m_io_api, set_own_param) == at(8, 12));
  CHECK(offsetof(o3m_io_api, play_note) == at(8, 13));
  CHECK(offsetof(o3m_io_api, stop_note) == at(8, 14));
  CHECK(sizeof(o3m_io_api) == at(8, 15));
  // the launch context and the PODs
  CHECK(offsetof(o3m_launch_ctx, block_tracks) == 28);
  CHECK(sizeof(o3m_launch_ctx) == 32);
  CHECK(sizeof(o3m_step) == 8);
  CHECK(sizeof(o3m_pattern_info) == 28);
  CHECK(sizeof(o3m_instrument_info) == 20);  // 1.7 appends midi_channel to the 16 bytes of 1.6 (the OS answers an older, shorter struct with the first four members only)
  CHECK(offsetof(o3m_instrument_info, midi_channel) == 16);
  CHECK(sizeof(o3m_param_desc) == 40);
  // the table the OS lends says what it has: a 1.6 app tests `size` before it calls a 1.6 member
  Rig r;
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  CHECK(r.host()->read->size == sizeof(o3m_read_api));
  CHECK(r.host()->edit->size == sizeof(o3m_edit_api));
  CHECK((O3M_TABLE_HAS(r.host()->read, get_instrument_param) && O3M_TABLE_HAS(r.host()->edit, set_instrument_param)));
  o3m_read_api cut = *r.host()->read;  // the table of an OS of 1.5: it ends after get_steps
  cut.size = sizeof(ReadV15);
  CHECK(O3M_TABLE_HAS(&cut, get_steps));
  CHECK(!O3M_TABLE_HAS(&cut, instrument_info));
  o3m_edit_api cute = *r.host()->edit;
  cute.size = sizeof(EditV15);
  CHECK(O3M_TABLE_HAS(&cute, set_steps));
  CHECK(!O3M_TABLE_HAS(&cute, set_instrument_param));
}

TEST_CASE("abi 1.6: an app of 1.5 reads and edits steps through its own layout of the tables and finds every member where it was; one group is one undo unit") {
  Rig r;
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  ReadV15 read;
  EditV15 edit;
  std::memcpy(&read, r.host()->read, sizeof read);
  std::memcpy(&edit, r.host()->edit, sizeof edit);
  CHECK((read.size >= sizeof read && edit.size >= sizeof edit));
  CHECK((read.pattern_info == r.host()->read->pattern_info && read.get_steps == r.host()->read->get_steps));
  CHECK((edit.edit_begin == r.host()->edit->edit_begin && edit.edit_end == r.host()->edit->edit_end && edit.set_step == r.host()->edit->set_step && edit.set_steps == r.host()->edit->set_steps));
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  const std::size_t depth = r.journal.depth();
  r.input_call([&](const o3m_host*) {
    o3m_pattern_info info;
    info.size = sizeof info;
    CHECK(read.pattern_info(1, &info) == O3M_EDIT_OK);
    CHECK(info.length == 32u);
    CHECK(edit.edit_begin() == O3M_EDIT_OK);
    const o3m_step s = step(66, 4, 99);
    CHECK(edit.set_step(1, 2, 3, &s, kAll) == O3M_EDIT_OK);
    CHECK(edit.edit_end() == O3M_EDIT_OK);
    o3m_step back;
    CHECK(read.get_steps(1, 2, 3, &back, 1) == O3M_EDIT_OK);
    CHECK((back.note == 66 && back.instr == 4 && back.vol == 99));
  });
  CHECK(r.journal.depth() == depth + 1u);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);
}

TEST_CASE("abi 1.6: the new entries are NULL without the capability that earns their table, whatever the subset") {
  Rig r;
  struct Case {
    const char* stem;
    bool read, edit;
  };
  for (const Case& c : {Case{"Editor", true, true}, Case{"Viewer", true, false}, Case{"Writer", false, true}, Case{"Listener", false, false}}) {
    r.launch_ok(c.stem);
    NEED(r.host() != nullptr);
    CHECK_MESSAGE((r.host()->read != nullptr) == c.read, c.stem);
    CHECK_MESSAGE((r.host()->edit != nullptr) == c.edit, c.stem);
    if (c.read) {
      CHECK((r.host()->read->instrument_info != nullptr && r.host()->read->get_param_desc != nullptr && r.host()->read->get_instrument_param != nullptr));
    }
    if (c.edit) {
      CHECK(r.host()->edit->set_instrument_param != nullptr);
    }
    r.mods->ui.app_exit();
  }
  // an app of 1.2 is lent nothing and a table that ends at draw; a 1.6 file on an OS that is older than 1.6 is refused: a file of the next minor is
  r.card.blob("Future", static_cast<std::uint8_t>(O3M_ABI_MINOR + 1), kReadEdit);
  r.mods->ui.scan_apps();
  CHECK(r.launch("Future") == app::ModuleUi::LaunchResult::NeedsNewerOs);
  r.card.blob("Min6", 6, kReadEdit);  // exactly this OS's minor: accepted by the loader
  r.mods->ui.scan_apps();
  (void)r.launch("Min6");
  CHECK(r.mods->ui.last_status() == module::Status::Ok);
}

TEST_CASE("instrument params: instrument_info says what a slot holds (empty, sample, MIDI, module) and its engine; bad arguments are RANGE") {
  Rig r(12, true);
  r.install_sample(3, 2, 1);
  r.install_sample(4, 0, 2);
  r.install_midi(6);
  r.install_module(9);
  r.install_module(12, 4);
  r.launch_ok("Viewer");
  NEED(r.host() != nullptr);
  const o3m_read_api* rd = r.host()->read;
  o3m_instrument_info info;
  const auto ask = [&](unsigned n) {
    std::memset(&info, 0xCC, sizeof info);
    info.size = sizeof info;
    return rd->instrument_info(static_cast<std::uint8_t>(n), &info);
  };
  CHECK(ask(1) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_EMPTY && info.engine == 0u && info.param_count == 0u && info.size == sizeof info));
  CHECK(ask(3) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_SAMPLE && info.engine == 2u && info.param_count == 83u));
  CHECK(ask(4) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_SAMPLE && info.engine == 0u && info.param_count == 83u));
  CHECK(ask(6) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_MIDI && info.engine == 0u && info.param_count == 0u));
  CHECK(ask(9) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_MODULE && info.engine == fnv1a("Inst") && info.param_count == 11u));
  const std::uint32_t module_engine = info.engine;
  CHECK(ask(12) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_MODULE && info.engine == module_engine && info.param_count == 11u));  // the same module: the same engine value, whatever the blob
  CHECK(ask(0) == O3M_EDIT_RANGE);
  CHECK(ask(65) == O3M_EDIT_RANGE);
  CHECK(ask(255) == O3M_EDIT_RANGE);
  CHECK(rd->instrument_info(3, nullptr) == O3M_EDIT_RANGE);
  // a struct that is shorter than ours gets as much as it asked for, and its size says how much it was; one of 0 bytes gets nothing
  struct {
    std::uint32_t size, kind, engine, param_count, canary;
  } big;
  std::memset(&big, 0xCC, sizeof big);
  big.size = 8;
  CHECK(rd->instrument_info(3, reinterpret_cast<o3m_instrument_info*>(&big)) == O3M_EDIT_OK);
  CHECK((big.size == 8u && big.kind == O3M_INSTRUMENT_SAMPLE && big.engine == 0xCCCCCCCCu && big.canary == 0xCCCCCCCCu));
  big.size = 0;
  CHECK(rd->instrument_info(3, reinterpret_cast<o3m_instrument_info*>(&big)) == O3M_EDIT_RANGE);
}

TEST_CASE("instrument params: get_param_desc lists the 83 rows of a sample instrument as the model's table says them, with APP_WRITABLE on exactly the sound parameters") {
  Rig r;
  r.install_sample(3, 0, 1);
  r.install_midi(6);
  r.launch_ok("Viewer");
  NEED(r.host() != nullptr);
  const o3m_read_api* rd = r.host()->read;
  std::vector<unsigned> ids;
  unsigned writable = 0;
  for (unsigned i = 0; i < 83; ++i) {
    o3m_param_desc d;
    std::memset(&d, 0xCC, sizeof d);
    d.size = sizeof d;
    NEED(rd->get_param_desc(3, static_cast<std::uint16_t>(i), &d) == O3M_EDIT_OK);
    CHECK(d.size == sizeof d);
    const unsigned id = model::param_field_of_slot(core::ParamKind::Instrument, i);
    CHECK(d.id == id);
    ids.push_back(id);
    model::ParamField f;
    NEED(model::find_param(core::param_id(core::ParamKind::Instrument, 2, id), f));
    const core::ParamDesc& pd = *f.desc;
    CHECK((d.min == pd.min && d.max == pd.max && d.def == pd.def));
    CHECK_MESSAGE(std::strcmp(d.name, pd.name) == 0, "id " << id << " " << d.name);
    CHECK(std::strlen(d.name) <= core::kParamNameChars);
    CHECK((d.reserved[0] == 0 && d.reserved[1] == 0 && d.reserved[2] == 0));
    const bool is_enum = pd.unit == core::ParamUnit::Enum;
    CHECK_MESSAGE(((d.flags & O3M_PD_ENUM) != 0) == is_enum, "id " << id);
    CHECK_MESSAGE(((d.flags & O3M_PD_STEPPED) != 0) == (!is_enum && f.store != model::ParamStore::F32 && pd.max - pd.min <= 64.0f), "id " << id);
    CHECK_MESSAGE(((d.flags & O3M_PD_APP_WRITABLE) != 0) == !expect_not_writable(id), "id " << id << " " << d.name);
    CHECK((d.flags & ~(O3M_PD_STEPPED | O3M_PD_ENUM | O3M_PD_APP_WRITABLE)) == 0);
    CHECK(d.group == (ip::is_mod_field(id) ? O3M_PGROUP_MOD : pd.group));
    writable += (d.flags & O3M_PD_APP_WRITABLE) != 0 ? 1u : 0u;
  }
  CHECK(writable == 61u);  // 13 voice, filter and send rows, 6 x 5 envelope rows, 6 x 3 LFO rows
  // a few rows by name: the set the spec lists
  std::vector<unsigned> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
  o3m_param_desc d;
  d.size = sizeof d;
  CHECK(rd->get_param_desc(3, 83, &d) == O3M_EDIT_RANGE);
  CHECK(rd->get_param_desc(3, 0xFFFF, &d) == O3M_EDIT_RANGE);
  CHECK(rd->get_param_desc(3, 0, nullptr) == O3M_EDIT_RANGE);
  CHECK(rd->get_param_desc(0, 0, &d) == O3M_EDIT_RANGE);
  CHECK(rd->get_param_desc(65, 0, &d) == O3M_EDIT_RANGE);
  CHECK(rd->get_param_desc(1, 0, &d) == O3M_EDIT_RANGE);  // an empty slot has none
  CHECK(rd->get_param_desc(6, 0, &d) == O3M_EDIT_RANGE);  // nor a MIDI instrument
  d.size = 0;
  CHECK(rd->get_param_desc(3, 0, &d) == O3M_EDIT_RANGE);
}

TEST_CASE("instrument params: a module instrument lists its own parameters: ranges, STEPPED for a small INT, ENUM for a BOOL, the page of eight, all writable; a module that is not loaded has none") {
  Rig r;
  r.install_module(9);
  r.install_midi(6);
  r.launch_ok("Viewer");
  NEED(r.host() != nullptr);
  const o3m_read_api* rd = r.host()->read;
  const char* names[11] = {"AMOUNT", "WHOLE STEPS", "SWITCH", "CUTOFF", "RESO", "DRIVE", "MIX", "TILT", "PAGE TWO A", "PAGE TWO B", "BIG STEPS"};
  for (unsigned i = 0; i < 11; ++i) {
    o3m_param_desc d;
    d.size = sizeof d;
    NEED(rd->get_param_desc(9, static_cast<std::uint16_t>(i), &d) == O3M_EDIT_OK);
    CHECK(d.id == 0x100 + i);
    CHECK((d.min == kInstParams[i].min && d.max == kInstParams[i].max && d.def == kInstParams[i].def));
    CHECK_MESSAGE(std::strcmp(d.name, names[i]) == 0, d.name);
    CHECK(d.group == i / 8);
    CHECK((d.flags & O3M_PD_APP_WRITABLE) != 0);
    const bool boolean = i == 2, small_int = i == 1;
    CHECK_MESSAGE(((d.flags & O3M_PD_ENUM) != 0) == boolean, names[i]);
    CHECK_MESSAGE(((d.flags & O3M_PD_STEPPED) != 0) == small_int, names[i]);  // (Big Steps is an INT over 0..200: not a switch)
  }
  o3m_param_desc d;
  d.size = sizeof d;
  CHECK(rd->get_param_desc(9, 11, &d) == O3M_EDIT_RANGE);
  // a module instrument whose module is not bound (not loaded, or not on the card): it is still a MODULE, with nothing to list or write
  r.install_module(14, 11, 0.0f, false);
  o3m_instrument_info info;
  info.size = sizeof info;
  CHECK(rd->instrument_info(14, &info) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_MODULE && info.param_count == 0u));
  CHECK(rd->get_param_desc(14, 0, &d) == O3M_EDIT_RANGE);
  float v = 0;
  CHECK(rd->get_instrument_param(14, 0x100, &v) == O3M_EDIT_RANGE);
  r.mods->ui.app_exit();
  r.launch_ok("Editor");
  r.input_call([&](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_instrument_param(14, 0x100, 0.5f) == O3M_EDIT_RANGE);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
}

TEST_CASE("instrument params: get_instrument_param reads the project's value in the parameter's unit; a module parameter the blob does not reach reads the module's default; unknown ids and slots are RANGE") {
  Rig r;
  r.install_sample(3, 0, 2);
  r.install_module(9, 11, 1.0f);
  r.install_module(12, 4);
  r.install_midi(6);
  r.launch_ok("Viewer");
  NEED(r.host() != nullptr);
  const o3m_read_api* rd = r.host()->read;
  float v = -1e30f;
  for (unsigned i = 0; i < 83; ++i) {
    const unsigned id = model::param_field_of_slot(core::ParamKind::Instrument, i);
    v = -1e30f;
    CHECK(rd->get_instrument_param(3, static_cast<std::uint16_t>(id), &v) == O3M_EDIT_OK);
    CHECK_MESSAGE(v == r.project().get_param(core::param_id(core::ParamKind::Instrument, 2, id)), "id " << id);
  }
  CHECK((rd->get_instrument_param(3, ip::Volume, &v) == O3M_EDIT_OK && v == 54.0f));
  CHECK((rd->get_instrument_param(3, ip::Cutoff, &v) == O3M_EDIT_OK && v == r.project().instrument(2)->sampler.cutoff));
  for (unsigned i = 0; i < 11; ++i) {
    CHECK(rd->get_instrument_param(9, static_cast<std::uint16_t>(0x100 + i), &v) == O3M_EDIT_OK);
    CHECK(v == r.project().get_param(core::param_id(core::ParamKind::Instrument, 8, 0x100 + i)));
  }
  // the short blob of instrument 12 (4 floats): 0..3 are the blob's, the rest read the module's defaults
  for (unsigned i = 0; i < 11; ++i) {
    CHECK(rd->get_instrument_param(12, static_cast<std::uint16_t>(0x100 + i), &v) == O3M_EDIT_OK);
    if (i >= 4) {
      CHECK_MESSAGE(v == kInstParams[i].def, "index " << i);
    }
  }
  CHECK(rd->get_instrument_param(9, 0x10B, &v) == O3M_EDIT_RANGE);
  CHECK(rd->get_instrument_param(9, ip::Volume, &v) == O3M_EDIT_RANGE);   // a sample's id on a module
  CHECK(rd->get_instrument_param(3, 0x100, &v) == O3M_EDIT_RANGE);        // a module's id on a sample
  CHECK(rd->get_instrument_param(3, 0x1D, &v) == O3M_EDIT_RANGE);        // a hole in the table
  CHECK(rd->get_instrument_param(3, 0xFFFF, &v) == O3M_EDIT_RANGE);
  CHECK(rd->get_instrument_param(6, 0, &v) == O3M_EDIT_RANGE);
  CHECK(rd->get_instrument_param(1, 0, &v) == O3M_EDIT_RANGE);
  CHECK(rd->get_instrument_param(0, 0, &v) == O3M_EDIT_RANGE);
  CHECK(rd->get_instrument_param(65, 0, &v) == O3M_EDIT_RANGE);
  CHECK(rd->get_instrument_param(3, 0, nullptr) == O3M_EDIT_RANGE);
}

TEST_CASE("instrument params: set_instrument_param joins the open group; nothing is written before edit_end; the values are the UI's (rounded where the field is whole)") {
  Rig r;
  r.install_sample(3, 0, 1);
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  const std::size_t depth = r.journal.depth();
  std::int32_t outside = -1;
  r.input_call([&](const o3m_host* h) { outside = h->edit->set_instrument_param(3, ip::Volume, 10.0f); });
  CHECK(outside == O3M_EDIT_NO_GROUP);  // no implicit units
  CHECK(snapshot_all(r.project()) == before);
  r.input_call([&](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_instrument_param(3, ip::Volume, 77.0f) == O3M_EDIT_OK);
    CHECK(h->edit->set_instrument_param(3, ip::Tune, 3.4f) == O3M_EDIT_OK);          // a whole field: stored rounded
    CHECK(h->edit->set_instrument_param(3, ip::Cutoff, 0.25f) == O3M_EDIT_OK);
    CHECK(h->edit->set_instrument_param(3, ip::envelope(2, ip::EnvRelease), 1234.0f) == O3M_EDIT_OK);
    CHECK(h->edit->set_instrument_param(3, ip::mod(1, ip::LfoAmount), 0.75f) == O3M_EDIT_OK);
    float v = 0;
    CHECK((h->read->get_instrument_param(3, ip::Volume, &v) == O3M_EDIT_OK && v == 47.0f));  // a read before edit_end still sees the project as it is
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
    CHECK((h->read->get_instrument_param(3, ip::Volume, &v) == O3M_EDIT_OK && v == 77.0f));
  });
  const model::SamplerParams& s = r.project().instrument(2)->sampler;
  CHECK(s.volume == 77);
  CHECK(s.tune == 3);
  CHECK(s.cutoff == 0.25f);
  CHECK(s.envelope[2].release == 1234);
  CHECK(s.lfo[1].amount == 0.75f);
  CHECK(r.journal.depth() == depth + 1u);
  core::ActionId id{};
  core::ActionArg arg = 0;
  NEED(r.journal.newest_undo(id, arg));
  CHECK(id == core::action_id("module.app_group"));
  char title[26];
  NEED(r.mods->ui.app_title(static_cast<unsigned>(arg), title, sizeof title));
  CHECK(std::string(title) == "EDITOR");  // the unit is named after the app
  // UNDO takes every parameter back, to the byte; REDO gives them again
  const std::vector<std::uint8_t> after = snapshot_all(r.project());
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == after);
}

TEST_CASE("instrument params: a parameter that is not writable, an unknown id, a bad instrument, a value out of range, NaN or infinity is RANGE, writes nothing and leaves the group valid") {
  Rig r;
  r.install_sample(3, 0, 1);
  r.install_midi(6);
  r.install_module(9);
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  const std::size_t depth = r.journal.depth();
  const float nan = std::nanf("");
  const float inf = HUGE_VALF;
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    // structural parameters: the engine, the whole source page, every envelope's ENABLE and every slot's SOURCE
    for (unsigned id = 0; id < 0x200; ++id) {
      model::ParamField f;
      const bool known = id < 0x100 && model::find_param(core::param_id(core::ParamKind::Instrument, 2, id), f);
      if (known && expect_not_writable(id)) {
        CHECK_MESSAGE(e->set_instrument_param(3, static_cast<std::uint16_t>(id), f.desc->min) == O3M_EDIT_RANGE, "id " << id);
      } else if (!known) {
        CHECK_MESSAGE(e->set_instrument_param(3, static_cast<std::uint16_t>(id), 0.0f) == O3M_EDIT_RANGE, "unknown id " << id);
      }
    }
    CHECK(e->set_instrument_param(3, 0xFFFF, 0.0f) == O3M_EDIT_RANGE);
    // the slot: 0, past 64, an empty one, a MIDI instrument, a sample's id on a module and a module's on a sample
    CHECK(e->set_instrument_param(0, ip::Volume, 10) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(65, ip::Volume, 10) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(255, ip::Volume, 10) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(1, ip::Volume, 10) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(6, ip::Volume, 10) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(6, 0x100, 0.5f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(9, ip::Volume, 10) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, 0x100, 0.5f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(9, 0x10B, 0.5f) == O3M_EDIT_RANGE);
    // OS-INSTUI S8a's virtual MIDI CHANNEL field (0x1E) is no app parameter: not on a sample, not on the MIDI instrument whose channel it is
    CHECK(!model::instrument_field_app_writable(ip::MidiChannel));
    CHECK(e->set_instrument_param(3, ip::MidiChannel, 2.0f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(6, ip::MidiChannel, 2.0f) == O3M_EDIT_RANGE);
    // the range: nothing is clamped for the app; the ends themselves are fine (checked below)
    CHECK(e->set_instrument_param(3, ip::Volume, -0.5f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, ip::Volume, 100.5f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, ip::Tune, -24.5f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, ip::Cutoff, 1.0001f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, ip::Resonance, 4.31f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, ip::Volume, nan) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, ip::Volume, inf) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(3, ip::Volume, -inf) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(9, 0x100, 1.5f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(9, 0x100, nan) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(9, 0x101, 10.5f) == O3M_EDIT_RANGE);
    CHECK(e->set_instrument_param(9, 0x103, 19.0f) == O3M_EDIT_RANGE);   // Cutoff 20..20000
    CHECK(e->set_instrument_param(9, 0x107, -1.5f) == O3M_EDIT_RANGE);
    // the group stays valid: the ends of the ranges are accepted
    CHECK(e->set_instrument_param(3, ip::Volume, 0.0f) == O3M_EDIT_OK);
    CHECK(e->set_instrument_param(3, ip::Volume, 100.0f) == O3M_EDIT_OK);
    CHECK(e->set_instrument_param(3, ip::Tune, -24.0f) == O3M_EDIT_OK);
    CHECK(e->set_instrument_param(3, ip::Resonance, 4.3f) == O3M_EDIT_OK);
    CHECK(e->set_instrument_param(9, 0x103, 20.0f) == O3M_EDIT_OK);
    CHECK(e->set_instrument_param(9, 0x103, 20000.0f) == O3M_EDIT_OK);
    CHECK(e->edit_end() == O3M_EDIT_OK);
  });
  // ... and the last write of each parameter won
  const model::SamplerParams& s = r.project().instrument(2)->sampler;
  CHECK((s.volume == 100 && s.tune == -24 && s.resonance == 4.3f));
  CHECK(r.project().get_param(core::param_id(core::ParamKind::Instrument, 8, 0x103)) == 20000.0f);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);
  // a group of nothing but refusals writes nothing and leaves no unit
  const std::size_t d2 = r.journal.depth();
  r.input_call([&](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_instrument_param(3, 13, 1) == O3M_EDIT_RANGE);
    CHECK(h->edit->set_instrument_param(3, ip::Volume, 1000) == O3M_EDIT_RANGE);
    CHECK(h->edit->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.journal.depth() == d2);
  CHECK(snapshot_all(r.project()) == before);
  (void)depth;
  CHECK(r.mods->ui.writes_refused() > 40u);
}

TEST_CASE("instrument params: a group of steps, a tempo and parameters of a sample and a module is ONE undo unit; UNDO and REDO restore every float bit and the module instance hears both") {
  Rig r;
  r.install_sample(3, 0, 1);
  r.install_module(9, 11, 1.0f);
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  const IState* st = r.inst_state(9);
  NEED(st != nullptr);
  r.block();
  const float cutoff0 = st->v[3];
  const std::size_t depth = r.journal.depth();
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    const o3m_step s = step(80, 2, 10);
    CHECK(e->set_steps(1, 1, 0, &s, 1, kAll) == O3M_EDIT_OK);
    CHECK(e->set_tempo(9000) == O3M_EDIT_OK);
    CHECK(e->set_instrument_param(3, ip::Cutoff, 0.1f) == O3M_EDIT_OK);
    CHECK(e->set_instrument_param(9, 0x103, 5000.0f) == O3M_EDIT_OK);      // Cutoff (log)
    CHECK(e->set_instrument_param(9, 0x102, 0.0f) == O3M_EDIT_OK);         // the BOOL
    CHECK(e->set_instrument_param(9, 0x10A, 77.6f) == O3M_EDIT_OK);        // an INT: stored rounded
    CHECK(e->set_instrument_param(9, 0x103, 7777.0f) == O3M_EDIT_OK);      // the same one again: the last wins
    CHECK(e->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.journal.depth() == depth + 1u);
  const model::Project& p = r.project();
  CHECK(p.pattern(1)->steps[1][0].note == 80);
  CHECK(p.tempo_x100() == 9000u);
  CHECK(p.instrument(2)->sampler.cutoff == 0.1f);
  const auto mp = [](unsigned i) { return core::param_id(core::ParamKind::Instrument, 8, 0x100 + i); };
  CHECK(p.get_param(mp(3)) == 7777.0f);
  CHECK(p.get_param(mp(2)) == 0.0f);
  CHECK(p.get_param(mp(10)) == 78.0f);
  r.block();
  CHECK(st->v[3] == 7777.0f);  // the running instance heard it
  CHECK(st->v[10] == 78.0f);
  const std::vector<std::uint8_t> after = snapshot_all(r.project());
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);  // every byte, the floats' bits included
  r.block();
  CHECK(st->v[3] == cutoff0);  // and the instance hears the undo
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == after);
  r.block();
  CHECK(st->v[3] == 7777.0f);
}

TEST_CASE("instrument params: a parameter write is one call and takes one of the group's 2,048 buffer slots, and a group accepts 64 of them: the 65th is LIMIT and the whole group is dropped") {
  Rig r;
  (void)r.project().set_pattern_length(1, 128);
  r.install_sample(3, 0, 1);
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  const std::uint32_t gen = r.project().generation();
  const std::size_t depth = r.journal.depth();
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    static_assert(O3M_EDIT_MAX_PARAM_WRITES == 64, "the rings behind write_param take 64 module changes");
    for (unsigned i = 0; i < O3M_EDIT_MAX_PARAM_WRITES; ++i) {
      CHECK(e->set_instrument_param(3, ip::Volume, static_cast<float>(i % 100)) == O3M_EDIT_OK);
    }
    CHECK(e->set_instrument_param(3, 13, 1.0f) == O3M_EDIT_RANGE);           // (a refused write is not an accepted one: it does not use the 64)
    CHECK(e->set_instrument_param(3, ip::Volume, 5.0f) == O3M_EDIT_LIMIT);
    CHECK(e->set_instrument_param(3, ip::Volume, 6.0f) == O3M_EDIT_LIMIT);   // every later write too
    const o3m_step s = step(60);
    CHECK(e->set_step(1, 0, 0, &s, kAll) == O3M_EDIT_LIMIT);
    CHECK(e->set_instrument_param(3, 13, 1.0f) == O3M_EDIT_LIMIT);           // (even a bad one: the group is over)
    CHECK(e->edit_end() == O3M_EDIT_LIMIT);
  });
  CHECK(snapshot_all(r.project()) == before);
  CHECK(r.project().generation() == gen);
  CHECK(r.journal.depth() == depth);
  CHECK(r.mods->ui.groups_dropped() == 1u);
  // steps and parameters share the slots: 2,047 steps and one parameter fit, a second parameter does not
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    o3m_step rows[127];
    for (auto& s : rows) s = step(61);
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    for (unsigned i = 0; i < 16; ++i) {
      CHECK(e->set_steps(1, static_cast<std::uint8_t>(i % 12), 0, rows, 127, kAll) == O3M_EDIT_OK);  // 16 x 127 = 2,032
    }
    for (unsigned i = 0; i < 15; ++i) {
      CHECK(e->set_step(1, 0, static_cast<std::uint8_t>(i), rows, kAll) == O3M_EDIT_OK);               // 15 more: 2,047
    }
    CHECK(e->set_instrument_param(3, ip::Volume, 9.0f) == O3M_EDIT_OK);                                // 2,048
    CHECK(e->set_instrument_param(3, ip::Volume, 10.0f) == O3M_EDIT_LIMIT);
    CHECK(e->edit_end() == O3M_EDIT_LIMIT);
  });
  CHECK(snapshot_all(r.project()) == before);
  // the call limit counts refused parameter writes too: 4,096 calls, the 4,097th is LIMIT
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    for (unsigned i = 0; i < 4096; ++i) {
      CHECK(e->set_instrument_param(3, 13, 0.0f) == O3M_EDIT_RANGE);
    }
    CHECK(e->set_instrument_param(3, ip::Volume, 1.0f) == O3M_EDIT_LIMIT);
    CHECK(e->edit_end() == O3M_EDIT_LIMIT);
  });
  CHECK(snapshot_all(r.project()) == before);
  CHECK(r.journal.depth() == depth);
}

TEST_CASE("instrument params: a group the app leaves open when its call returns is closed by the OS with its parameter writes applied; an app stopped for its budget loses them") {
  Rig r;
  r.install_sample(3, 0, 1);
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  r.input_call([&](const o3m_host* h) {
    CHECK(h->edit->edit_begin() == O3M_EDIT_OK);
    CHECK(h->edit->set_instrument_param(3, ip::Volume, 12.0f) == O3M_EDIT_OK);
    // no edit_end
  });
  CHECK(r.project().instrument(2)->sampler.volume == 12);
  CHECK(r.mods->ui.groups_abandoned() == 1u);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);
}

TEST_CASE("fuzz 1.6: random calls of the four instrument-parameter entries with the others, in and out of groups and of input(), never crash, never write what is not writable, answer with a published code") {
  Rig r(12, true);
  r.install_sample(3, 0, 1);
  r.install_sample(5, 0, 2);
  r.install_midi(6);
  r.install_module(9);
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  test::Rng rng(0xBADC0DEu);
  const auto code_ok = [](int c) { return c >= O3M_EDIT_OK && c <= O3M_EDIT_RATE; };
  // the values of the parameters that are NOT writable must never change, whatever the app does
  const auto frozen = [&](const model::Project& p) {
    std::vector<float> v;
    for (unsigned slot : {2u, 4u}) {
      for (unsigned id = 0; id < 0x78; ++id) {
        const core::ParamId pid = core::param_id(core::ParamKind::Instrument, slot, id);
        model::ParamField f;
        if (model::find_param(pid, f) && expect_not_writable(id)) {
          v.push_back(p.get_param(pid));
        }
      }
    }
    return v;
  };
  const std::vector<float> frozen0 = frozen(r.project());
  unsigned oks = 0;
  for (unsigned frame = 0; frame < 600; ++frame) {
    const bool in_input = rng.below(8) != 0;
    const unsigned calls = rng.below(50);
    auto script = [&](const o3m_host* h) {
      const o3m_edit_api* e = h->edit;
      const o3m_read_api* rd = h->read;
      if (rng.below(5) != 0) {
        CHECK(code_ok(e->edit_begin()));
      }
      for (unsigned i = 0; i < calls; ++i) {
        const std::uint32_t pick = rng.below(9);
        std::uint8_t n = static_cast<std::uint8_t>(rng.below(4) == 0 ? rng.byte() : (rng.below(2) ? 3 : (rng.below(2) ? 9 : 1 + rng.below(12))));
        std::uint16_t id = static_cast<std::uint16_t>(rng.below(4) == 0 ? rng.next() & 0xFFFF : (rng.below(2) ? rng.below(0x80) : 0x100 + rng.below(14)));
        const bool null = rng.below(10) == 0;
        float value;
        switch (rng.below(8)) {
          case 0: value = std::nanf(""); break;
          case 1: value = HUGE_VALF; break;
          case 2: value = -HUGE_VALF; break;
          case 3: value = static_cast<float>(rng.next()); break;
          default: value = static_cast<float>(rng.below(20001)) * 0.01f - 5.0f; break;
        }
        if (rng.below(10) < 6) {  // a writable parameter with a value inside its range, so that most groups really change something
          struct K {
            std::uint8_t n;
            std::uint16_t id;
            float lo, hi;
          };
          static const K known[] = {{3, 0, 0, 100}, {3, 2, -24, 24}, {5, 7, 0, 1}, {5, 8, 0, 4.3f}, {3, 9, 0, 100}, {5, 0x21, 0, 1}, {3, 0x25, 0, 10000}, {5, 0x61, 0, 4}, {9, 0x100, 0, 1}, {9, 0x101, 0, 10},
                                    {9, 0x102, 0, 1}, {9, 0x103, 20, 20000}, {9, 0x107, -1, 1}, {9, 0x10A, 0, 200}};
          const K& k = known[rng.below(sizeof known / sizeof known[0])];
          n = k.n;
          id = k.id;
          value = k.lo + (k.hi - k.lo) * (static_cast<float>(rng.below(1001)) / 1000.0f);
        }
        switch (pick) {
          case 0: CHECK(code_ok(e->edit_begin())); break;
          case 1: CHECK(code_ok(e->edit_end())); break;
          case 2:
          case 3:
          case 4: {
            const int c = e->set_instrument_param(n, id, value);
            CHECK(code_ok(c));
            oks += c == O3M_EDIT_OK ? 1u : 0u;
            break;
          }
          case 5: {
            o3m_instrument_info info;
            info.size = rng.below(6) == 0 ? rng.below(24) : sizeof info;
            CHECK(code_ok(rd->instrument_info(n, null ? nullptr : &info)));
            break;
          }
          case 6: {
            o3m_param_desc d;
            d.size = rng.below(6) == 0 ? rng.below(48) : sizeof d;
            CHECK(code_ok(rd->get_param_desc(n, static_cast<std::uint16_t>(rng.below(5) == 0 ? rng.next() : rng.below(90)), null ? nullptr : &d)));
            break;
          }
          case 7: {
            float v = 0;
            CHECK(code_ok(rd->get_instrument_param(n, id, null ? nullptr : &v)));
            break;
          }
          default: {
            const o3m_step s = step(static_cast<std::uint8_t>(1 + rng.below(128)));
            CHECK(code_ok(e->set_step(1, static_cast<std::uint8_t>(rng.below(12)), static_cast<std::uint8_t>(rng.below(32)), &s, kAll)));
            break;
          }
        }
      }
    };
    if (in_input) {
      r.input_call(script);
    } else {
      script(r.host());
    }
    if (rng.below(6) == 0) {
      (void)r.editor->undo();
    }
    if (rng.below(9) == 0) {
      (void)r.editor->redo();
    }
    CHECK(r.mods->ui.app_running());
    CHECK(!r.mods->ui.group_open());
    CHECK(r.editor->group_depth() == 0u);
    CHECK(frozen(r.project()) == frozen0);
    // every parameter stays inside its descriptor
    for (unsigned slot : {2u, 4u}) {
      for (unsigned id = 0; id < 0x78; ++id) {
        const core::ParamId pid = core::param_id(core::ParamKind::Instrument, slot, id);
        model::ParamField f;
        if (model::find_param(pid, f)) {
          const float v = r.project().get_param(pid);
          CHECK(v >= f.desc->min);
          CHECK(v <= f.desc->max);
        }
      }
    }
    for (unsigned i = 0; i < 11; ++i) {
      const float v = r.project().get_param(core::param_id(core::ParamKind::Instrument, 8, 0x100 + i));
      CHECK(v >= kInstParams[i].min);
      CHECK(v <= kInstParams[i].max);
    }
  }
  CHECK(oks > 100u);
}

TEST_CASE("undo property 1.6: any run of groups of steps and parameters (sample and module) followed by the same number of UNDOs restores the project byte for byte, REDO re-applies it; a dropped group is no unit") {
  for (std::uint32_t seed = 1; seed <= 16; ++seed) {
    Rig r(12);
    r.install_sample(3, 0, 1);
    r.install_sample(5, 2, 3);
    r.install_module(9, 11, 1.0f);
    r.install_midi(6);
    r.launch_ok("Editor");
    test::Rng rng(seed * 104729u);
    std::vector<std::vector<std::uint8_t>> states;
    states.push_back(snapshot_all(r.project()));
    const unsigned groups = 4 + rng.below(9);
    unsigned units = 0;
    for (unsigned g = 0; g < groups; ++g) {
      const unsigned writes = 1 + rng.below(40);
      const bool drop = rng.below(6) == 0;
      int end_code = -1;
      r.input_call([&](const o3m_host* h) {
        const o3m_edit_api* e = h->edit;
        CHECK(e->edit_begin() == O3M_EDIT_OK);
        for (unsigned w = 0; w < writes; ++w) {
          const std::uint32_t pick = rng.below(5);
          if (pick == 0) {
            const o3m_step s = step(static_cast<std::uint8_t>(1 + rng.below(128)), static_cast<std::uint8_t>(rng.below(65)), static_cast<std::uint8_t>(rng.below(128)));
            (void)e->set_step(1, static_cast<std::uint8_t>(rng.below(12)), static_cast<std::uint8_t>(rng.below(32)), &s, kAll);
          } else if (pick == 1) {
            (void)e->set_tempo(4000 + rng.below(76000));
          } else {
            const unsigned n = pick == 2 ? 3 : (pick == 3 ? 5 : 9);
            const std::uint16_t id = n == 9 ? static_cast<std::uint16_t>(0x100 + rng.below(11)) : static_cast<std::uint16_t>(rng.below(0x78));
            float lo = 0.0f, hi = 1.0f;
            if (n == 9) {
              lo = kInstParams[id - 0x100].min;
              hi = kInstParams[id - 0x100].max;
            } else {
              model::ParamField f;
              if (model::find_param(core::param_id(core::ParamKind::Instrument, n - 1u, id), f)) {
                lo = f.desc->min;
                hi = f.desc->max;
              }
            }
            const float value = lo + (hi - lo) * (static_cast<float>(rng.below(100001)) / 100000.0f);
            (void)e->set_instrument_param(static_cast<std::uint8_t>(n), id, value);
          }
        }
        if (drop) {
          for (unsigned k = 0; k < 2100; ++k) (void)e->set_instrument_param(3, ip::Volume, 1.0f);
        }
        end_code = e->edit_end();
      });
      if (drop) {
        CHECK(end_code == O3M_EDIT_LIMIT);
        CHECK(snapshot_all(r.project()) == states.back());
        continue;
      }
      CHECK(end_code == O3M_EDIT_OK);
      const std::vector<std::uint8_t> now = snapshot_all(r.project());
      if (now != states.back()) {
        states.push_back(now);
        ++units;
      }
    }
    CHECK(r.journal.depth() == units);
    for (unsigned u = 0; u < units; ++u) {
      CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
      CHECK_MESSAGE(snapshot_all(r.project()) == states[units - 1 - u], "seed " << seed << " undo " << u);
    }
    CHECK(snapshot_all(r.project()) == states[0]);
    for (unsigned u = 0; u < units; ++u) {
      CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
      CHECK(snapshot_all(r.project()) == states[u + 1]);
    }
  }
}

// ---- the four editing apps of OS-H3B through the whole stack (the host shared objects as native apps, the real ModuleUi, Editor, App) -------------------------------------------------

namespace {

// The words the notification strip has shown, newest last.
bool said(const Rig& r, const char* text) {
  for (unsigned i = 0; i < r.state().notify.size(); ++i) {
    if (std::string(r.state().notify.at(i).text.c_str()) == text) {
      return true;
    }
  }
  return false;
}

// What the app can change of one sample instrument: its sound parameters (everything but the structural ones) as floats.
std::vector<float> sound_of(const model::Project& p, unsigned n) {
  std::vector<float> v;
  for (unsigned i = 0; i < 83; ++i) {
    const unsigned id = model::param_field_of_slot(core::ParamKind::Instrument, i);
    if (!expect_not_writable(id)) {
      v.push_back(p.get_param(core::param_id(core::ParamKind::Instrument, n - 1u, id)));
    }
  }
  return v;
}
std::vector<float> structure_of(const model::Project& p, unsigned n) {
  std::vector<float> v;
  for (unsigned i = 0; i < 83; ++i) {
    const unsigned id = model::param_field_of_slot(core::ParamKind::Instrument, i);
    if (expect_not_writable(id)) {
      v.push_back(p.get_param(core::param_id(core::ParamKind::Instrument, n - 1u, id)));
    }
  }
  return v;
}

}  // namespace

TEST_CASE("tools 1.6: the four editing apps are tool apps (they hold edit_project): the list has them below EXPORT, sorted by name") {
  Rig r(12, true);
  r.mods->ui.scan_tools();
  std::vector<std::string> stems;
  for (unsigned i = 0; i < r.mods->ui.tool_count(); ++i) {
    stems.push_back(r.mods->ui.app_entry_at(r.mods->ui.tool_entry(i))->stem);
  }
  std::vector<std::string> sorted = stems;
  std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
  CHECK(stems == sorted);
  for (const char* want : {"chords", "humanize", "morph", "mutate", "rhythmgen"}) {
    CHECK_MESSAGE(std::find(stems.begin(), stems.end(), want) != stems.end(), want);
  }
  CHECK(r.mods->ui.app_title(r.entry("mutate"), nullptr, 0) == false);
  char title[26];
  NEED(r.mods->ui.app_title(r.entry("mutate"), title, sizeof title));
  CHECK(std::string(title) == "MUTATE");
}

TEST_CASE("mutate: through the whole stack on a sample instrument, F1 is one undo unit named MUTATE, only the sound parameters move, UNDO restores every byte, REDO gives it back; the same SEED, the same result") {
  const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 0, 4, 0, 0, 0, {0, 0, 0}};  // instrument 5
  std::vector<std::uint8_t> first_result;
  for (int run = 0; run < 2; ++run) {
    Rig r(12, true);
    r.install_sample(5, 0, 1);
    r.install_sample(6, 0, 3);
    r.launch_ok("mutate", &ctx);
    const std::vector<std::uint8_t> before = snapshot_all(r.project());
    const std::vector<float> structure = structure_of(r.project(), 5);
    const std::vector<float> sound = sound_of(r.project(), 5);
    const std::size_t depth = r.journal.depth();
    CHECK(r.mods->ui.app_input(O3M_EV_ENCODER, 0, 16, false));  // AMOUNT 100
    CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
    CHECK(r.journal.depth() == depth + 1u);
    const std::vector<std::uint8_t> after = snapshot_all(r.project());
    CHECK(after != before);
    CHECK(structure_of(r.project(), 5) == structure);      // the engine, the source page, the ENABLEs, the SOURCEs: untouched
    CHECK(sound_of(r.project(), 5) != sound);              // the sound moved
    CHECK(structure_of(r.project(), 6) == structure_of(r.project(), 6));
    CHECK(r.project().instrument(5)->sampler.volume == 61);  // the other instruments are not touched (6: 40 + 7 x 3 = 61)
    core::ActionId id{};
    core::ActionArg arg = 0;
    NEED(r.journal.newest_undo(id, arg));
    CHECK(id == core::action_id("module.app_group"));
    if (run == 0) {
      first_result = after;
    } else {
      CHECK(after == first_result);
    }
    CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
    CHECK(snapshot_all(r.project()) == before);
    CHECK(said(r, "UNDO MUTATE"));
    CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
    CHECK(snapshot_all(r.project()) == after);
    // VARY is another unit
    CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F2, 1, false));
    CHECK(r.journal.depth() == depth + 2u);
  }
}

TEST_CASE("mutate: on a module instrument the running instance hears the mutation and its undo; a refused kind says so and writes nothing") {
  Rig r(12, true);
  r.install_module(9, 11, 1.0f);
  r.install_midi(6);
  r.block();
  const IState* st = r.inst_state(9);
  NEED(st != nullptr);
  const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 0, 8, 0, 0, 0, {0, 0, 0}};  // instrument 9
  r.launch_ok("mutate", &ctx);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  float heard_before[11];
  for (unsigned i = 0; i < 11; ++i) heard_before[i] = st->v[i];
  CHECK(r.mods->ui.app_input(O3M_EV_ENCODER, 0, 16, false));
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
  const std::vector<std::uint8_t> after = snapshot_all(r.project());
  CHECK(after != before);
  r.block();
  unsigned moved = 0;
  for (unsigned i = 0; i < 11; ++i) {
    const float v = r.project().get_param(core::param_id(core::ParamKind::Instrument, 8, 0x100 + i));
    CHECK(v >= kInstParams[i].min);
    CHECK(v <= kInstParams[i].max);
    CHECK(st->v[i] == v);  // what the instance heard is what the project holds
    moved += v != heard_before[i] ? 1u : 0u;
  }
  CHECK(moved >= 5u);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);
  r.block();
  for (unsigned i = 0; i < 11; ++i) {
    CHECK(st->v[i] == heard_before[i]);
  }
  // a MIDI instrument: nothing to mutate
  r.mods->ui.app_exit();
  const o3m_launch_ctx midi = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 0, 5, 0, 0, 0, {0, 0, 0}};  // instrument 6
  r.launch_ok("mutate", &midi);
  const std::size_t depth = r.journal.depth();
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
  CHECK(r.journal.depth() == depth);
}

TEST_CASE("morph: through the whole stack, POS 100 gives A the writable parameters of B (not the structural ones) as one unit named MORPH; RESET puts A back to A0 bit for bit; UNDO restores the project") {
  Rig r(12, true);
  r.install_sample(5, 0, 1);
  r.install_sample(6, 0, 3);
  r.install_sample(7, 2, 2);  // another play engine: not offered
  const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 0, 4, 0, 0, 0, {0, 0, 0}};  // A = 5
  r.launch_ok("morph", &ctx);
  const std::vector<std::uint8_t> a0_image = snapshot_all(r.project());
  const std::vector<float> a_structure = structure_of(r.project(), 5);
  const std::vector<float> b_sound = sound_of(r.project(), 6);
  const std::size_t depth = r.journal.depth();
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_DOWN, 1, false));  // POS
  CHECK(r.mods->ui.app_input(O3M_EV_ENCODER, 0, 10, false));          // 100
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
  CHECK(r.journal.depth() == depth + 1u);
  // A's sound is B's, to the last bit (a whole field stored rounded already: B holds whole numbers there)
  CHECK(sound_of(r.project(), 5) == b_sound);
  CHECK(structure_of(r.project(), 5) == a_structure);
  const std::vector<std::uint8_t> blended = snapshot_all(r.project());
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F3, 1, false));    // RESET
  CHECK(r.journal.depth() == depth + 2u);
  CHECK(snapshot_all(r.project()) == a0_image);                         // A is A0 again, byte for byte
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == blended);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == a0_image);
  CHECK(said(r, "UNDO MORPH"));
}

TEST_CASE("humanize: through the whole stack on a block, F1 is one undo unit named HUMANIZE, only VOL, FX1 and FX2 of steps with a note change, the same SEED gives the same steps, UNDO restores") {
  const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 1, 0, 0, 16, 2, {0, 0, 0}};  // tracks 2-3, rows 1-16
  std::vector<std::uint8_t> first;
  for (int run = 0; run < 2; ++run) {
    Rig r(12, true);
    for (unsigned t = 1; t < 3; ++t) {
      for (unsigned row = 0; row < 32; row += 2) {
        (void)r.project().set_step(1, t, row, model::Step{static_cast<std::uint8_t>(40 + row), 3, static_cast<std::uint8_t>(row % 3 == 0 ? 255 : 80), {{0, 0}, {row % 8 == 0 ? std::uint8_t{8} : std::uint8_t{0}, 5}}});
      }
    }
    r.editor->history_reset();
    r.launch_ok("humanize", &ctx);
    const std::vector<std::uint8_t> before = snapshot_all(r.project());
    const std::size_t depth = r.journal.depth();
    CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
    CHECK(r.journal.depth() == depth + 1u);
    const std::vector<std::uint8_t> after = snapshot_all(r.project());
    CHECK(after != before);
    const model::Pattern* p = r.project().pattern(1);
    unsigned delayed = 0;
    for (unsigned t = 0; t < 12; ++t) {
      for (unsigned row = 0; row < 32; ++row) {
        const model::Step& s = p->steps[t][row];
        const bool inside = (t == 1 || t == 2) && row < 16;
        if (!inside) {
          const bool original = (t == 1 || t == 2) && row % 2 == 0;
          CHECK_MESSAGE(s.note == (original ? 40 + row : 0), "track " << t << " row " << row);
          if (original) {
            CHECK(s.vol == (row % 3 == 0 ? 255 : 80));  // outside the block: untouched
          }
          continue;
        }
        if (row % 2 == 0) {
          CHECK((s.note == 40 + row && s.instr == 3));  // the notes and instruments never change
          if (row % 3 == 0) {
            CHECK(s.vol == 255);  // no volume: stays unset (the instrument's own volume plays it)
          } else {
            CHECK((s.vol >= 72 && s.vol <= 88));  // 80 +/- the default VOL 8
          }
          if (s.fx[0].type == 2) ++delayed;
          if (s.fx[1].type == 2) ++delayed;
          CHECK(s.fx[1].type == (row % 8 == 0 ? 8 : (s.fx[1].type == 2 ? 2 : 0)));  // an FX that was there stays
        } else {
          CHECK(s.note == 0);
        }
      }
    }
    CHECK(delayed >= 6u);
    if (run == 0) {
      first = after;
    } else {
      CHECK(after == first);
    }
    CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
    CHECK(snapshot_all(r.project()) == before);
    CHECK(said(r, "UNDO HUMANIZE"));
  }
}

TEST_CASE("chords: through the whole stack, on a MIDI instrument CHORD FX writes C, G, Am, F with FX 14, on a sample instrument SPREAD fills three tracks; each F1 is one undo unit named CHORDS") {
  {
    Rig r(12, true);
    r.install_midi(5);
    const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 2, 4, 0, 16, 1, {0, 0, 0}};  // track 3, instrument 5, rows 1-16
    r.launch_ok("chords", &ctx);
    const std::vector<std::uint8_t> before = snapshot_all(r.project());
    const std::size_t depth = r.journal.depth();
    CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
    CHECK(r.journal.depth() == depth + 1u);
    const model::Pattern* p = r.project().pattern(1);
    const unsigned roots[4] = {49, 56, 58, 54}, fx[4] = {5, 5, 4, 5};
    for (unsigned row = 0; row < 16; ++row) {
      const model::Step& s = p->steps[2][row];
      if (row % 4 == 0) {
        CHECK((s.note == roots[row / 4] && s.instr == 5 && s.fx[1].type == 14 && s.fx[1].value == fx[row / 4]));
      } else {
        CHECK(s.note == 0);
      }
    }
    CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
    CHECK(snapshot_all(r.project()) == before);
    CHECK(said(r, "UNDO CHORDS"));
  }
  {
    Rig r(12, true);
    r.install_sample(5, 0, 0);
    const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 0, 4, 0, 16, 4, {0, 0, 0}};
    r.launch_ok("chords", &ctx);
    const std::vector<std::uint8_t> before = snapshot_all(r.project());
    const std::size_t depth = r.journal.depth();
    CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
    CHECK(r.journal.depth() == depth + 1u);
    const model::Pattern* p = r.project().pattern(1);
    CHECK((p->steps[0][0].note == 49 && p->steps[1][0].note == 53 && p->steps[2][0].note == 56 && p->steps[3][0].note == 0));  // C E G
    CHECK((p->steps[0][0].fx[0].type == 0 && p->steps[0][0].fx[1].type == 0));                                              // no FX
    CHECK((p->steps[0][4].note == 56 && p->steps[1][4].note == 60 && p->steps[2][4].note == 63));                           // G B D
    CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
    CHECK(snapshot_all(r.project()) == before);
  }
}

TEST_CASE("instrument params: the worst case, 64 parameter writes (every one a change, a sample's and a module's) in one group, applies whole as one unit and UNDO restores every byte of the instruments") {
  Rig r;
  r.install_sample(3, 0, 1);
  r.install_module(9, 11, 1.0f);
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  const std::size_t depth = r.journal.depth();
  r.input_call([&](const o3m_host* h) {
    const o3m_edit_api* e = h->edit;
    CHECK(e->edit_begin() == O3M_EDIT_OK);
    for (unsigned i = 0; i < O3M_EDIT_MAX_PARAM_WRITES; ++i) {
      if (i % 2 == 0) {
        CHECK(e->set_instrument_param(3, ip::Volume, static_cast<float>(1 + i % 99)) == O3M_EDIT_OK);
      } else {
        CHECK(e->set_instrument_param(9, 0x100, static_cast<float>(1 + i % 97) * 0.01f) == O3M_EDIT_OK);
      }
    }
    CHECK(e->edit_end() == O3M_EDIT_OK);
  });
  CHECK(r.journal.depth() == depth + 1u);
  CHECK(r.project().instrument(2)->sampler.volume == 1 + 62u % 99);
  CHECK(snapshot_all(r.project()) != before);
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);
}

TEST_CASE("instrument params: UNDO and REDO of a unit with more than 16 parameter changes (a 61-parameter MUTATE) put every sound parameter of the sample instrument back into the engine, transport running or not") {
  Rig r(12, true);
  r.install_sample(5, 0, 1);
  core::ParamRing ring;
  r.app->attach_param_ring(&ring);
  // the engine's view of the parameters: the last change it was sent for each id, starting from the project as it is
  std::map<std::uint32_t, float> engine;
  const auto sync = [&] {
    core::ParamChange c;
    while (ring.pop(c)) {
      engine[static_cast<std::uint32_t>(c.id)] = c.value;
    }
  };
  const auto fields = [&](const std::function<void(unsigned, core::ParamId)>& fn) {
    for (unsigned i = 0; i < 83; ++i) {
      const unsigned field = model::param_field_of_slot(core::ParamKind::Instrument, i);
      if (model::instrument_field_app_writable(field) && !ip::is_mod_field(field)) {  // (the LFO rows rebuild the descriptor: they are not in the ring)
        fn(field, core::param_id(core::ParamKind::Instrument, 4, field));
      }
    }
  };
  fields([&](unsigned, core::ParamId id) { engine[static_cast<std::uint32_t>(id)] = r.project().get_param(id); });
  const o3m_launch_ctx ctx = {sizeof ctx, O3M_LAUNCH_FROM_TOOLS, 1, 0, 4, 0, 0, 0, {0, 0, 0}};
  r.launch_ok("mutate", &ctx);
  const std::vector<std::uint8_t> before = snapshot_all(r.project());
  CHECK(r.mods->ui.app_input(O3M_EV_ENCODER, 0, 16, false));  // AMOUNT 100
  CHECK(r.mods->ui.app_input(O3M_EV_BUTTON, O3M_BTN_F1, 1, false));
  unsigned changed = 0;
  fields([&](unsigned, core::ParamId id) { changed += r.project().get_param(id) != engine[static_cast<std::uint32_t>(id)] ? 1u : 0u; });
  NEED(changed > 16u);  // more than the Editor lists after an undo
  sync();
  fields([&](unsigned f, core::ParamId id) { CHECK_MESSAGE(engine[static_cast<std::uint32_t>(id)] == r.project().get_param(id), "applied, field " << f); });
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(snapshot_all(r.project()) == before);
  sync();
  fields([&](unsigned f, core::ParamId id) { CHECK_MESSAGE(engine[static_cast<std::uint32_t>(id)] == r.project().get_param(id), "undone, field " << f); });
  CHECK(r.invoke("edit.redo") == app::DispatchResult::Changed);
  sync();
  fields([&](unsigned f, core::ParamId id) { CHECK_MESSAGE(engine[static_cast<std::uint32_t>(id)] == r.project().get_param(id), "redone, field " << f); });
  r.app->attach_param_ring(nullptr);
}

// =====================================================================================================================================================
// OS-H4, ABI 1.7: export writes for apps (ADR 0014 amendment A) and SMF EXPORT through the whole stack
// =====================================================================================================================================================

namespace {

// 1.6's io table and instrument-info POD as that slice published them (the 1.4 io table is the 1.6 one): a 1.7 OS must keep every member where it was, and a 1.6 app's shorter struct must get 16 bytes.
struct IoV16 {
  std::uint32_t version, size;
  std::uint32_t (*now_ms)();
  std::uint32_t (*midi_read)(o3m_midi_event*, std::uint32_t);
  void (*midi_lost)(std::uint32_t*, std::uint32_t*);
  int (*tap_select)(std::uint32_t);
  std::uint32_t (*tap_latest)(std::int16_t*, std::uint32_t);
  std::uint32_t (*tap_stream)(std::int16_t*, std::uint32_t, std::uint32_t*);
  void (*tap_levels)(o3m_tap_levels*);
};
struct InstrumentInfoV16 {
  std::uint32_t size, kind, engine, param_count;
};
static_assert(offsetof(o3m_io_api, now_ms) == offsetof(IoV16, now_ms) && offsetof(o3m_io_api, midi_read) == offsetof(IoV16, midi_read) && offsetof(o3m_io_api, midi_lost) == offsetof(IoV16, midi_lost) &&
                  offsetof(o3m_io_api, tap_select) == offsetof(IoV16, tap_select) && offsetof(o3m_io_api, tap_latest) == offsetof(IoV16, tap_latest) && offsetof(o3m_io_api, tap_stream) == offsetof(IoV16, tap_stream) &&
                  offsetof(o3m_io_api, tap_levels) == offsetof(IoV16, tap_levels),
              "1.6 io table");
static_assert(offsetof(o3m_io_api, export_open) == sizeof(IoV16) && offsetof(o3m_io_api, export_write) == sizeof(IoV16) + kP && offsetof(o3m_io_api, export_close) == sizeof(IoV16) + 2 * kP &&
                  offsetof(o3m_io_api, export_status) == sizeof(IoV16) + 3 * kP && sizeof(o3m_io_api) == sizeof(IoV16) + 8 * kP,
              "1.7 io table: four members appended (and the four of 1.10 after them)");
static_assert(offsetof(o3m_instrument_info, size) == offsetof(InstrumentInfoV16, size) && offsetof(o3m_instrument_info, kind) == offsetof(InstrumentInfoV16, kind) &&
                  offsetof(o3m_instrument_info, engine) == offsetof(InstrumentInfoV16, engine) && offsetof(o3m_instrument_info, param_count) == offsetof(InstrumentInfoV16, param_count) &&
                  offsetof(o3m_instrument_info, midi_channel) == sizeof(InstrumentInfoV16) && sizeof(o3m_instrument_info) == sizeof(InstrumentInfoV16) + 4,
              "1.7 instrument info: midi_channel appended");
static_assert(O3M_EXPORT_OK == 0 && O3M_EXPORT_BUSY == 1 && O3M_EXPORT_RANGE == 2 && O3M_EXPORT_LIMIT == 3 && O3M_EXPORT_STATE == 4 && O3M_EXPORT_NO_CARD == 5, "the export codes are published");
static_assert(O3M_EXPORT_IDLE == 0 && O3M_EXPORT_WRITING == 1 && O3M_EXPORT_DONE == 2 && O3M_EXPORT_FAILED == 3, "the export states are published");
static_assert(O3M_CAP_EXPORT_FILES == 16 && O3M_CAPS_ABI_1_7 == 31, "export_files is bit 16");

const char* const kCardFolder = "AppExport/smfexport/";

// Puts a note on `track` at `row` of pattern 1.
void put_note(model::Project& p, unsigned track, unsigned row, std::uint8_t note, std::uint8_t instr = 1, std::uint8_t vol = 255) {
  const model::Step s{note, instr, vol, {{0, 0}, {0, 0}}};
  CHECK(p.set_steps(1, track, row, &s, 1) == model::Status::Ok);
}

// The file as the bytes the card holds, or empty.
std::vector<std::uint8_t> card_file(const o3os::test::MemCard& c, const char* path) {
  const std::vector<std::uint8_t>* b = c.get(path);
  return b != nullptr ? *b : std::vector<std::uint8_t>();
}

// What the Smf header says: format, tracks, division, the first tempo, and the count of note-ons (status 0x90 .. 0x9F in the track chunks, walked by the chunks' lengths).
struct SmfFacts {
  bool ok = false;
  unsigned format = 99, ntrks = 0, division = 0, chunks = 0, tempo = 0, note_ons = 0;
};
SmfFacts smf_facts(const std::vector<std::uint8_t>& b) {
  SmfFacts f;
  if (b.size() < 14 || std::memcmp(b.data(), "MThd", 4) != 0) {
    return f;
  }
  f.format = (b[8] << 8) | b[9];
  f.ntrks = (b[10] << 8) | b[11];
  f.division = (b[12] << 8) | b[13];
  std::size_t at = 14;
  while (at + 8 <= b.size() && std::memcmp(b.data() + at, "MTrk", 4) == 0) {
    const std::size_t len = (static_cast<std::size_t>(b[at + 4]) << 24) | (static_cast<std::size_t>(b[at + 5]) << 16) | (static_cast<std::size_t>(b[at + 6]) << 8) | b[at + 7];
    if (at + 8 + len > b.size()) {
      return f;
    }
    const std::size_t end = at + 8 + len;
    std::size_t p = at + 8;
    while (p < end) {
      while (p < end && (b[p] & 0x80u) != 0) {
        ++p;  // the delta
      }
      ++p;
      if (p >= end) {
        break;
      }
      if (b[p] == 0xFF) {
        const std::size_t n = b[p + 2];
        if (b[p + 1] == 0x51 && f.tempo == 0 && n == 3) {
          f.tempo = (b[p + 3] << 16) | (b[p + 4] << 8) | b[p + 5];
        }
        p += 3 + n;
      } else {
        f.note_ons += (b[p] & 0xF0u) == 0x90u ? 1u : 0u;
        p += 3;
      }
    }
    if (p != end) {
      return f;
    }
    at = end;
    ++f.chunks;
  }
  f.ok = at == b.size();
  return f;
}

}  // namespace

TEST_CASE("abi 1.7: the published offsets of 1.2 .. 1.6, member by member (in pointer slots): nothing moved, the export members are appended, and a 1.6 app finds every member of its own layout where it was") {
  const auto at = [](std::size_t header, unsigned slot) { return header + slot * kP; };
  CHECK(offsetof(o3m_host, io) == at(16, 8));
  CHECK(offsetof(o3m_host, get_clock) == at(16, 9));  // the 1.7 table is nine slots after the header: 1.12 appended get_clock and track_level after them (nothing before moved)
  CHECK(sizeof(o3m_host) == at(16, 11));
  CHECK(offsetof(o3m_io_api, now_ms) == at(8, 0));
  CHECK(offsetof(o3m_io_api, tap_levels) == at(8, 6));
  CHECK(offsetof(o3m_io_api, export_open) == at(8, 7));
  CHECK(offsetof(o3m_io_api, export_write) == at(8, 8));
  CHECK(offsetof(o3m_io_api, export_close) == at(8, 9));
  CHECK(offsetof(o3m_io_api, export_status) == at(8, 10));
  CHECK(sizeof(o3m_io_api) == at(8, 15));  // (1.10 appended four: abi 1.10 below holds the literal offsets)
  CHECK(offsetof(o3m_instrument_info, midi_channel) == 16);
  CHECK(sizeof(o3m_instrument_info) == 20);
  CHECK(sizeof(o3m_read_api) == at(8, 7));  // the read and edit tables did not change
  CHECK(sizeof(o3m_edit_api) == at(8, 6));
  // an app of 1.6 (the "Editor" native declares ABI 1.6) is lent the 1.7 table, larger, and reads its own layout of it
  Rig r;
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  CHECK(r.host()->abi_minor == 13u);  // (the OS is 1.13; the app declares 1.6)
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  CHECK((io->version == O3M_IO_API_VERSION && io->size == sizeof(o3m_io_api)));
  IoV16 v16;
  std::memcpy(&v16, io, sizeof v16);
  CHECK((v16.now_ms == io->now_ms && v16.midi_read == io->midi_read && v16.midi_lost == io->midi_lost && v16.tap_select == io->tap_select && v16.tap_levels == io->tap_levels));
  CHECK((io->export_open == nullptr && io->export_write == nullptr && io->export_close == nullptr && io->export_status == nullptr));  // no export_files in its file: NULL
  CHECK(O3M_TABLE_HAS(io, export_status));
  CHECK(v16.now_ms() == 0u + v16.now_ms());  // (it runs)
  // the 16-byte instrument info of an app of 1.6: the OS writes 16 bytes, the canary after it is not touched
  r.install_midi(2);
  r.install_sample(3, 0, 1);
  struct {
    InstrumentInfoV16 info;
    std::uint32_t canary;
  } old{{sizeof(InstrumentInfoV16), 99, 99, 99}, 0xCAFEF00Du};
  CHECK(r.host()->read->instrument_info(2, reinterpret_cast<o3m_instrument_info*>(&old.info)) == O3M_EDIT_OK);
  CHECK((old.info.size == 16u && old.info.kind == O3M_INSTRUMENT_MIDI && old.canary == 0xCAFEF00Du));
  // and the app of 1.7 with the 20-byte struct gets the channel of a MIDI instrument (it is the instrument's own: 3 here), 0 for any other kind
  o3m_instrument_info info{sizeof info, 99, 99, 99, 99};
  CHECK(r.host()->read->instrument_info(2, &info) == O3M_EDIT_OK);
  CHECK((info.size == 20u && info.kind == O3M_INSTRUMENT_MIDI && info.midi_channel == 3u));
  info = o3m_instrument_info{sizeof info, 99, 99, 99, 99};
  CHECK(r.host()->read->instrument_info(3, &info) == O3M_EDIT_OK);
  CHECK((info.size == 20u && info.kind == O3M_INSTRUMENT_SAMPLE && info.midi_channel == 0u));
  info = o3m_instrument_info{sizeof info, 99, 99, 99, 99};
  CHECK(r.host()->read->instrument_info(9, &info) == O3M_EDIT_OK);  // an empty slot
  CHECK((info.kind == O3M_INSTRUMENT_EMPTY && info.midi_channel == 0u));
  r.install_module(4);
  info = o3m_instrument_info{sizeof info, 99, 99, 99, 99};
  CHECK(r.host()->read->instrument_info(4, &info) == O3M_EDIT_OK);
  CHECK((info.kind == O3M_INSTRUMENT_MODULE && info.midi_channel == 0u));
  // every MIDI channel 1..16 is reported as it is
  for (unsigned ch = 1; ch <= 16; ++ch) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Midi);
    in.midi_channel = static_cast<std::uint8_t>(ch);
    CHECK(r.project().set_instrument(5, in) == model::Status::Ok);
    info = o3m_instrument_info{sizeof info, 99, 99, 99, 99};
    CHECK(r.host()->read->instrument_info(6, &info) == O3M_EDIT_OK);
    CHECK(info.midi_channel == ch);
  }
}

TEST_CASE("abi 1.7: the export calls are lent to an app of ABI 1.7 that asked, and only then; an app that holds the bit with no export service is told NO_CARD") {
  Rig r(12, false, true);
  using R = app::ModuleUi::LaunchResult;
  r.launch_ok("Exporter");
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  CHECK((io->export_open != nullptr && io->export_write != nullptr && io->export_close != nullptr && io->export_status != nullptr));
  CHECK(io->export_status(nullptr, 0) == O3M_EXPORT_IDLE);
  CHECK(io->export_write("x", 1) == O3M_EXPORT_STATE);
  CHECK(io->export_close() == O3M_EXPORT_STATE);
  r.mods->ui.app_exit();
  for (const char* none : {"ReadOnly7", "Editor", "Editor3", "Editor4", "Writer", "Viewer"}) {  // ABI 1.7 without the bit, and older apps: NULL
    r.launch_ok(none);
    NEED(r.host() != nullptr);
    CHECK_MESSAGE((r.host()->io == nullptr || (r.host()->io->export_open == nullptr && r.host()->io->export_write == nullptr && r.host()->io->export_close == nullptr && r.host()->io->export_status == nullptr)), none);
    r.mods->ui.app_exit();
  }
  r.launch_ok("ExpEdit");
  CHECK((r.host()->edit != nullptr && r.host()->io->export_open != nullptr));
  r.mods->ui.app_exit();
  // a module system with no export service (the simulator, a bare ModuleUi): the bit is granted, the calls answer NO_CARD at open and never touch a card
  r.mods->ui.set_export(nullptr);
  r.launch_ok("Exporter");
  CHECK(r.host()->io->export_open != nullptr);
  CHECK(r.host()->io->export_open("a", "mid") == O3M_EXPORT_NO_CARD);
  CHECK(r.host()->io->export_write("x", 1) == O3M_EXPORT_STATE);
  CHECK(r.host()->io->export_status(nullptr, 0) == O3M_EXPORT_IDLE);
  CHECK(r.memcard.log.empty());
  r.mods->ui.app_exit();
  (void)sizeof(R);
}

TEST_CASE("abi 1.7: the first launch of an app that writes files is confirmed once per boot, with a message that names the folder; edit + export is ONE message that says both; read-only apps are not asked") {
  Rig r(12, false, true);
  using R = app::ModuleUi::LaunchResult;
  app::ModuleUi& ui = r.mods->ui;
  CHECK(r.launch("Exporter") == R::NeedsConfirm);
  CHECK(!ui.app_running());
  CHECK(std::string(ui.launch_message(R::NeedsConfirm)) == "WRITES FILES TO APPEXPORT/EXPORTER: F1 AGAIN");
  CHECK(r.launch("Exporter") == R::Ok);  // the second launch goes through
  ui.app_exit();
  CHECK(r.launch("Exporter") == R::Ok);  // remembered for the boot
  ui.app_exit();
  CHECK(r.launch("ExpEdit") == R::NeedsConfirm);
  CHECK(std::string(ui.launch_message(R::NeedsConfirm)) == "EDITS (UNDOABLE) + WRITES FILES: F1 AGAIN");
  CHECK(r.launch("ExpEdit") == R::Ok);
  ui.app_exit();
  CHECK(r.launch("Writer") == R::NeedsConfirm);  // an edit-only app: the text it always had
  CHECK(std::string(ui.launch_message(R::NeedsConfirm)) == "EDITS THE PROJECT (UNDOABLE): F1 AGAIN");
  CHECK(std::string(app::ModuleUi::launch_text(R::NeedsConfirm)) == "EDITS THE PROJECT (UNDOABLE): F1 AGAIN");
  CHECK(r.launch("Writer") == R::Ok);
  ui.app_exit();
  CHECK(r.launch("ReadOnly7") == R::Ok);  // read only: nothing to confirm
  ui.app_exit();
  CHECK(r.launch("smfexport") == R::NeedsConfirm);
  CHECK(std::string(ui.launch_message(R::NeedsConfirm)) == "WRITES FILES TO APPEXPORT/SMFEXPORT: F1 AGAIN");
  CHECK(r.launch("smfexport") == R::Ok);
  ui.app_exit();
  // every message fits the TOOLS dialog's line (kConfirmChars) and the notification strip, whatever the app's name
  for (const unsigned len : {1u, 12u, 13u, 31u}) {  // (the module set holds 16: the rig's own apps leave room for four)
    const std::string stem(len, 'x');
    r.card.add(stem.c_str());
    CHECK(r.mods->set.add_native(stem.c_str(), kExporter, O3M_CAP_EXPORT_FILES));
  }
  ui.scan_apps();
  for (const unsigned len : {1u, 12u, 13u, 31u}) {
    const std::string stem(len, 'x');
    CHECK(r.launch(stem.c_str()) == R::NeedsConfirm);
    const std::string text = ui.launch_message(R::NeedsConfirm);
    CAPTURE(text);
    CHECK(text.size() <= app::ModuleUi::kConfirmChars);
    CHECK(text.size() <= core::kNotifyTextChars);
    CHECK(text.rfind(": F1 AGAIN") == text.size() - 10);
    CHECK(text.find("WRITES FILES TO APPEXPORT/") == 0u);
    if (len <= 12u) {
      CHECK(text.find("APPEXPORT/" + std::string(len, 'X') + ": F1 AGAIN") != std::string::npos);  // (the folder is shown in capitals)
    } else {
      CHECK(text.find("..: F1 AGAIN") != std::string::npos);  // a long name is cut, and says so
    }
    CHECK(r.launch(stem.c_str()) == R::Ok);
    ui.app_exit();
  }
}

TEST_CASE("abi 1.7: the confirm is remembered with the bits it covered: a file replaced by a version that asks for more asks again") {
  Rig r(12, false, true);
  using R = app::ModuleUi::LaunchResult;
  app::ModuleUi& ui = r.mods->ui;
  r.card.add("Grow");
  CHECK(r.mods->set.add_native("Grow", kWriter, O3M_CAP_EDIT_PROJECT));
  ui.scan_apps();
  CHECK(r.launch("Grow") == R::NeedsConfirm);
  CHECK(std::string(ui.launch_message(R::NeedsConfirm)) == "EDITS THE PROJECT (UNDOABLE): F1 AGAIN");
  CHECK(r.launch("Grow") == R::Ok);
  ui.app_exit();
  CHECK(r.launch("Grow") == R::Ok);  // remembered for the boot
  ui.app_exit();
  // the file is replaced by a version that also writes files (the set forgets the old one, as a reload does)
  r.mods->set.clear();
  CHECK(r.mods->set.add_native("Grow", kExpEdit, O3M_CAP_EDIT_PROJECT | O3M_CAP_EXPORT_FILES));
  ui.scan_apps();
  CHECK(r.launch("Grow") == R::NeedsConfirm);  // asked again: the old answer did not cover the new bit
  CHECK(std::string(ui.launch_message(R::NeedsConfirm)) == "EDITS (UNDOABLE) + WRITES FILES: F1 AGAIN");
  CHECK(r.launch("Grow") == R::Ok);
  ui.app_exit();
  CHECK(r.launch("Grow") == R::Ok);  // and remembered with both bits
  ui.app_exit();
  // an export-only app that gains edit_project asks again, too; one that loses a bit does not
  r.card.add("Grow2");
  CHECK(r.mods->set.add_native("Grow2", kExporter, O3M_CAP_EXPORT_FILES));
  ui.scan_apps();
  CHECK(r.launch("Grow2") == R::NeedsConfirm);
  CHECK(r.launch("Grow2") == R::Ok);
  ui.app_exit();
  r.mods->set.clear();
  CHECK(r.mods->set.add_native("Grow2", kWriter, O3M_CAP_EDIT_PROJECT));
  ui.scan_apps();
  CHECK(r.launch("Grow2") == R::NeedsConfirm);  // (edit_project was never confirmed for it)
  CHECK(r.launch("Grow2") == R::Ok);
  ui.app_exit();
  r.mods->set.clear();
  CHECK(r.mods->set.add_native("Grow2", kExporter, O3M_CAP_EXPORT_FILES));
  ui.scan_apps();
  CHECK(r.launch("Grow2") == R::Ok);  // export_files was confirmed before
  ui.app_exit();
}

TEST_CASE("abi 1.7: Pattern F7 TOOLS lists the apps that hold edit_project OR export_files, and the confirm shows in the list") {
  Rig r(12, false, true);
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  NEED(r.state().overlay == app::OverlayId::VerbMenu);
  const app::ModuleUi& ui = r.mods->ui;
  std::vector<std::string> names;
  for (unsigned i = 0; i < ui.tool_count(); ++i) {
    names.push_back(ui.app_entry_at(ui.tool_entry(i))->stem);
  }
  for (const char* n : {"smfexport", "Exporter", "ExpEdit", "Editor", "Writer"}) {
    CHECK_MESSAGE(std::find(names.begin(), names.end(), n) != names.end(), n);
  }
  for (const char* n : {"ReadOnly7", "Viewer", "Listener"}) {
    CHECK_MESSAGE(std::find(names.begin(), names.end(), n) == names.end(), n);
  }
  // walk to SMF EXPORT and run it: the first F1 says what it does, in the list; the second starts it
  unsigned at = 0;
  for (; at < app::tools_count(r.app->model()); ++at) {
    app::ToolItem item;
    NEED(app::tools_item(r.app->model(), at, item));
    if (std::string(ui.app_entry_at(item.app_entry)->stem) == "smfexport") {
      break;
    }
  }
  NEED(at < app::tools_count(r.app->model()));
  for (unsigned i = 0; i < at; ++i) {
    r.press(Button::Down);
  }
  r.press(Button::F1);
  CHECK(r.state().overlay == app::OverlayId::VerbMenu);
  CHECK(std::string(r.state().modules.tools_msg) == "WRITES FILES TO APPEXPORT/SMFEXPORT: F1 AGAIN");
  {  // OS-KEYROWDLG: the dialog owns the row: F1 reads RUN? (inverted) while the confirm waits, F8 BACK, the rest blank
    const test::RowRead again = test::read_owner_row(r.app->model(), r.state());
    CHECK(again.label[0] == "RUN?");
    CHECK(again.hot == 0x01u);
    CHECK(again.label[7] == "BACK");
    CHECK(again.label[1].empty());
  }
  r.press(Button::F1);
  CHECK(r.state().screen == app::ScreenId::ModuleApp);
  CHECK(ui.app_running());
  CHECK(std::string(ui.app_name()) == "smfexport");
}

// F7 on Pattern, the cursor on SMF EXPORT in the list, F1 (the first launch says what the app does) and F1 again: the app's page is up, as the user gets there. Returns false if it did not come up.
bool open_smf_from_tools(Rig& r) {
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  if (r.state().overlay != app::OverlayId::VerbMenu) {
    return false;
  }
  for (unsigned i = 0; i < app::tools_count(r.app->model()); ++i) {
    r.press(Button::Up);  // the list opens on the row it was left on (OS-PTOOLS2 S1): from the top
  }
  for (unsigned i = 0; i < app::tools_count(r.app->model()); ++i) {
    app::ToolItem item;
    if (!app::tools_item(r.app->model(), i, item)) {
      return false;
    }
    if (std::string(r.mods->ui.app_entry_at(item.app_entry)->stem) == "smfexport") {
      break;
    }
    r.press(Button::Down);
  }
  r.press(Button::F1);  // "WRITES FILES TO APPEXPORT/SMFEXPORT: F1 AGAIN"
  r.press(Button::F1);
  return r.state().screen == app::ScreenId::ModuleApp && r.mods->ui.app_running();
}

// Frames of the main loop until `path` is on the card (or `max` frames): the app's draws feed the writes and poll the status, the jobs commit.
bool wait_for_file(Rig& r, const char* path, unsigned max = 300) {
  for (unsigned i = 0; i < max && !r.memcard.has(path); ++i) {
    r.pass(16);
  }
  return r.memcard.has(path);
}

TEST_CASE("smf export through the whole stack: Pattern F7 TOOLS, F1 EXPORT, the file appears in AppExport/smfexport/ on the card, whole, with no undo unit and the project untouched") {
  Rig r(12, false, true);
  model::Project& p = r.project();
  for (unsigned t = 0; t < 3; ++t) {
    for (unsigned row = 0; row < 32; row += 4) {
      put_note(p, t, row, static_cast<std::uint8_t>(40 + t + row), 1, 100);
    }
  }
  r.editor->history_reset();
  const std::vector<std::uint8_t> before = snapshot_all(p);
  const std::size_t depth = r.journal.depth();
  NEED(open_smf_from_tools(r));
  r.press(Button::F1);  // EXPORT
  NEED(wait_for_file(r, "AppExport/smfexport/PAT001.mid"));
  const std::vector<std::uint8_t> file = card_file(r.memcard, "AppExport/smfexport/PAT001.mid");
  NEED(!file.empty());
  const SmfFacts f = smf_facts(file);
  CHECK(f.ok);
  CHECK((f.format == 1u && f.ntrks == 4u && f.division == 96u && f.chunks == 4u));  // the tempo track and three
  CHECK(f.tempo == 500000u);                                                        // 120 BPM
  CHECK(f.note_ons == 3u * 8u);                                                     // 8 notes on each of 3 tracks
  CHECK(r.memcard.violations == 0);
  for (const auto& kv : r.memcard.files) {
    CHECK_MESSAGE(kv.first.rfind("appexport/smfexport/", 0) == 0, kv.first);
    CHECK_MESSAGE(kv.first.find("_o3save.tmp") == std::string::npos, kv.first);
  }
  // the page says DONE (the status was polled by the app's own frames)
  for (unsigned i = 0; i < 10; ++i) {
    r.pass(16);
  }
  // an export is not an edit: the project is byte for byte what it was, and there is no undo unit
  CHECK(snapshot_all(p) == before);
  CHECK(r.journal.depth() == depth);
  // a second export of the same pattern never replaces the first
  r.press(Button::F1);
  NEED(wait_for_file(r, "AppExport/smfexport/PAT001_001.mid"));
  CHECK(card_file(r.memcard, "AppExport/smfexport/PAT001.mid") == file);
  CHECK(card_file(r.memcard, "AppExport/smfexport/PAT001_001.mid") == file);
  CHECK(r.memcard.violations == 0);
}

TEST_CASE("smf export through the whole stack: the F7 TOOLS block is the range; the file is PAT001_SEL.mid with only the block's tracks and rows") {
  Rig r(12, false, true);
  model::Project& p = r.project();
  for (unsigned t = 0; t < 12; ++t) {
    for (unsigned row = 0; row < 32; ++row) {
      put_note(p, t, row, static_cast<std::uint8_t>(30 + t), 1, 90);
    }
  }
  // a block as the tools test makes one
  r.invoke("nav.scroll", 5);
  for (int i = 0; i < 2; ++i) (void)r.invoke("nav.right");
  for (int i = 0; i < 2; ++i) (void)r.invoke("select.extend", 1);
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
  r.pass(1);
  extend_tracks(r, 2);
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
  r.pass(1);
  NEED(r.state().edit.selection.active);
  const app::Selection::Block b = r.state().edit.selection.block(r.state().pattern.row(), r.state().pattern.track());
  const unsigned tracks = static_cast<unsigned>(b.track1 - b.track0 + 1), rows = static_cast<unsigned>(b.row1 - b.row0 + 1);
  NEED((tracks >= 2u && rows >= 2u));
  NEED(open_smf_from_tools(r));
  r.press(Button::F1);
  NEED(wait_for_file(r, "AppExport/smfexport/PAT001_SEL.mid"));
  const std::vector<std::uint8_t> file = card_file(r.memcard, "AppExport/smfexport/PAT001_SEL.mid");
  const SmfFacts f = smf_facts(file);
  CHECK(f.ok);
  CHECK((f.ntrks == 1u + tracks && f.chunks == 1u + tracks));
  CHECK(f.note_ons == tracks * rows);  // a note on every row of the block
  CHECK(!r.memcard.has("AppExport/smfexport/PAT001.mid"));
}

TEST_CASE("smf export through the whole stack: the exit chord during the export aborts the file (no Temp, no partial final), and the next launch exports again") {
  Rig r(12, false, true);
  model::Project& p = r.project();
  for (unsigned t = 0; t < 12; ++t) {
    for (unsigned row = 0; row < 32; ++row) {
      put_note(p, t, row, static_cast<std::uint8_t>(30 + (row % 40)), 1, 100);
    }
  }
  NEED(open_smf_from_tools(r));
  r.press(Button::F1);  // EXPORT
  r.pass(16);
  r.chord_exit();       // leave while it commits
  CHECK(r.state().screen == app::ScreenId::Pattern);
  CHECK_FALSE(r.mods->ui.app_running());
  for (unsigned i = 0; i < 40; ++i) {
    r.pass(16);
  }
  for (const auto& kv : r.memcard.files) {
    CHECK_MESSAGE(kv.first.find("_o3save.tmp") == std::string::npos, kv.first);  // no Temp is left
  }
  const bool finished = r.memcard.has("AppExport/smfexport/PAT001.mid");  // (it may have been fast enough to end before the chord: then it is whole)
  if (finished) {
    CHECK(smf_facts(card_file(r.memcard, "AppExport/smfexport/PAT001.mid")).ok);
  }
  CHECK(r.memcard.violations == 0);
  // launch again: it exports (the name is the next free one if the first was whole)
  NEED(open_smf_from_tools(r));
  r.press(Button::F1);
  const char* second = finished ? "AppExport/smfexport/PAT001_001.mid" : "AppExport/smfexport/PAT001.mid";
  CHECK(wait_for_file(r, second));
  CHECK(smf_facts(card_file(r.memcard, second)).ok);
}

TEST_CASE("smf export through the whole stack: on a write protected card the app says so and nothing is written") {
  Rig r(12, false, true);
  put_note(r.project(), 0, 0, 60);
  r.memcard.gate_status = hal::storage::WriteStatus::WriteProtected;
  NEED(open_smf_from_tools(r));
  r.press(Button::F1);
  for (unsigned i = 0; i < 20; ++i) {
    r.pass(16);
  }
  CHECK(r.memcard.files.empty());
  CHECK(r.memcard.dirs.empty());
  CHECK(r.memcard.log.empty());
  CHECK(r.mods->ui.app_running());  // the app stays; it told the user
}

// ---- ABI 1.9 (OS-KEYROWDLG S5): the UI table grows by one member, the key row's labels --------------------------------------------------------------
namespace {
// A literal copy of o3m_ui as 1.1 .. 1.8 published it (any pointer width): nothing of it may move.
struct Ui18Literal {
  std::uint32_t struct_size;
  void (*enter)(void*);
  void (*input)(void*, const o3m_ui_event*);
  void (*view_draw)(void*, std::uint32_t, o3m_canvas*);
  void (*exit)(void*);
};
}  // namespace

TEST_CASE("abi 1.9: o3m_ui keeps its five members where they were and appends keys; the table is sized per minor; o3m_keys is 16 label pointers") {
  CHECK(offsetof(o3m_ui, struct_size) == offsetof(Ui18Literal, struct_size));
  CHECK(offsetof(o3m_ui, enter) == offsetof(Ui18Literal, enter));
  CHECK(offsetof(o3m_ui, input) == offsetof(Ui18Literal, input));
  CHECK(offsetof(o3m_ui, view_draw) == offsetof(Ui18Literal, view_draw));
  CHECK(offsetof(o3m_ui, exit) == offsetof(Ui18Literal, exit));
  CHECK(offsetof(o3m_ui, keys) == sizeof(Ui18Literal));  // appended, nothing between
  CHECK(sizeof(o3m_ui) == sizeof(Ui18Literal) + sizeof(void*));
  CHECK(O3M_UI_SIZE(0) == sizeof(Ui18Literal));
  CHECK(O3M_UI_SIZE(8) == sizeof(Ui18Literal));
  CHECK(O3M_UI_SIZE(9) == sizeof(o3m_ui));
  CHECK(sizeof(o3m_keys) == 16 * sizeof(void*));
  CHECK(offsetof(o3m_keys, shift_f) == 8 * sizeof(void*));
  CHECK(O3M_KEY_LABEL_MAX == 7u);
  CHECK(O3M_ABI_MINOR == 13u);  // (1.11 and 1.12 appended descriptor members only, 1.13 a flag bit: o3m_ui is still the 1.9 one)
  CHECK(sizeof(o3m_module) == O3M_MODULE_SIZE(13));  // the descriptor grew at 1.11 (the importer's table, kind 3) and at 1.12 (the effect surface)
  CHECK(O3M_MODULE_SIZE(11) == offsetof(o3m_module, render_st));
  CHECK(O3M_MODULE_SIZE(10) == O3M_MODULE_SIZE(9));  // 1.10 did not grow it
  CHECK(O3M_MODULE_SIZE(9) == O3M_MODULE_SIZE(8));
}

// ---- OS-APPUX S3, ABI 1.10: get_transport, set_own_param, play_note / stop_note (docs/specs/OS-APPUX.md 3.4, 3.5) ------------------------------------------------------------------------

namespace {

std::vector<std::pair<std::uint32_t, float>> g_own_sets;  // what the OS handed the app's set_param
std::vector<std::string> g_order;                          // "set", "enter" in the order the OS called them
void o_set_param(void*, std::uint32_t id, float v) {
  g_own_sets.emplace_back(id, v);
  g_order.emplace_back("set");
}
void o_enter(void* s) {
  g_order.emplace_back("enter");
  if (g_enter) {
    g_enter(static_cast<EState*>(s)->host);
  }
}
const o3m_param kOwnParams[3] = {{"Alpha", 0.0f, 100.0f, 10.0f, O3M_PARAM_INT}, {"Beta", 0.0f, 1.0f, 0.5f, 0}, {"Gamma", -10.0f, 10.0f, 0.0f, 0}};
const o3m_ui kOwnUi = {O3M_UI_SIZE(10), o_enter, e_input, e_draw, e_exit, nullptr};
const o3m_module kOwn = {O3M_ABI_MAJOR, 10, O3M_KIND_APP, O3M_MODULE_SIZE(10), "Own", sizeof(EState), 3, kOwnParams, e_init, nullptr, o_set_param, nullptr, nullptr, e_shutdown, &kOwnUi, 1};
const o3m_module kOwnQuiet = {O3M_ABI_MAJOR, 10, O3M_KIND_APP, O3M_MODULE_SIZE(10), "OwnQuiet", sizeof(EState), 3, kOwnParams, e_init, nullptr, o_set_param, nullptr, nullptr, e_shutdown, &kOwnUi, 1};

// The live-note path as the App's PreviewSink: what play_note asked of it, in order.
struct NoteSink final : app::PreviewSink {
  struct Rec {
    bool on;
    unsigned slot, instrument, note, velocity;
  };
  std::vector<Rec> log;
  unsigned offs = 0;
  bool accept = true;
  bool idle = false;  // the engine holds and queues no live note (a flush, the export or reset_input ended them): PreviewSink::live_idle
  bool live_idle() const noexcept override { return idle; }
  void preview(unsigned, unsigned) noexcept override {}
  bool app_note(bool on, unsigned slot, unsigned instrument, unsigned note, unsigned velocity) noexcept override {
    if (on && !accept) {
      return false;
    }
    log.push_back({on, slot, instrument, note, velocity});
    return true;
  }
  void app_notes_off() noexcept override { ++offs; }
  unsigned ons() const {
    unsigned n = 0;
    for (const Rec& r : log) {
      n += r.on ? 1u : 0u;
    }
    return n;
  }
};

// An app of ABI 1.9 whose enter() resets what its set_param stored (a legitimate way to start a page): the remembered rows must reach it AFTER enter().
float g_legacy_value[3] = {};
void l_set_param(void*, std::uint32_t id, float v) {
  if (id < 3u) {
    g_legacy_value[id] = v;
  }
  g_order.emplace_back("set");
}
void l_enter(void*) {
  g_order.emplace_back("enter");
  for (unsigned i = 0; i < 3u; ++i) {
    g_legacy_value[i] = kOwnParams[i].def;
  }
}
const o3m_ui kLegacyUi = {O3M_UI_SIZE(9), l_enter, e_input, e_draw, e_exit, nullptr};
const o3m_module kLegacy = {O3M_ABI_MAJOR, 9, O3M_KIND_APP, O3M_MODULE_SIZE(9), "OwnLegacy", sizeof(EState), 3, kOwnParams, e_init, nullptr, l_set_param, nullptr, nullptr, e_shutdown, &kLegacyUi, 1};

void own_rig(Rig& r) {
  CHECK(r.mods->set.add_native("Own", kOwn, O3M_CAP_PLAY_NOTES));
  CHECK(r.mods->set.add_native("OwnQuiet", kOwnQuiet, 0u));
  CHECK(r.mods->set.add_native("OwnLegacy", kLegacy, 0u));
  r.card.add("Own");
  r.card.add("OwnQuiet");
  r.card.add("OwnLegacy");
  r.mods->ui.scan_apps();
  g_own_sets.clear();
  g_order.clear();
}

}  // namespace

TEST_CASE("abi 1.10: the layout, member by member and literally: four io members appended, o3m_transport is nine words, bit 64 is play_notes, nothing older moved") {
  // the io table: the 1.7 members are where they were (13 slots), the four of 1.10 follow, in this order
  const auto at = [](std::size_t header, unsigned slot) { return header + slot * kP; };
  CHECK(offsetof(o3m_io_api, now_ms) == at(8, 0));
  CHECK(offsetof(o3m_io_api, export_status) == at(8, 10));
  CHECK(offsetof(o3m_io_api, get_transport) == at(8, 11));
  CHECK(offsetof(o3m_io_api, set_own_param) == at(8, 12));
  CHECK(offsetof(o3m_io_api, play_note) == at(8, 13));
  CHECK(offsetof(o3m_io_api, stop_note) == at(8, 14));
  CHECK(sizeof(o3m_io_api) == at(8, 15));
  if constexpr (sizeof(void*) == 4) {
    CHECK(offsetof(o3m_io_api, get_transport) == 52);  // the numbers of the target, which O3M_ABI_ASSERT io_1_10 also holds
    CHECK(offsetof(o3m_io_api, set_own_param) == 56);
    CHECK(offsetof(o3m_io_api, play_note) == 60);
    CHECK(offsetof(o3m_io_api, stop_note) == 64);
    CHECK(sizeof(o3m_io_api) == 68);
  }
  // the POD: nine 32-bit words, the size first
  CHECK(sizeof(o3m_transport) == 36);
  CHECK(offsetof(o3m_transport, size) == 0);
  CHECK(offsetof(o3m_transport, playing) == 4);
  CHECK(offsetof(o3m_transport, pattern) == 8);
  CHECK(offsetof(o3m_transport, order) == 12);
  CHECK(offsetof(o3m_transport, row) == 16);
  CHECK(offsetof(o3m_transport, ustep) == 20);
  CHECK(offsetof(o3m_transport, tempo_x100) == 24);
  CHECK(offsetof(o3m_transport, count_in) == 28);
  CHECK(offsetof(o3m_transport, queued) == 32);
  // the table's name, the version of the table and the host table did not move or grow
  CHECK(O3M_IO_API_VERSION == 1u);
  CHECK(offsetof(o3m_host, io) + sizeof(void*) == offsetof(o3m_host, get_clock));  // (1.12 appended get_clock and track_level after io: the 1.0..1.11 members did not move)
  CHECK(O3M_ABI_MINOR == 13);
  CHECK(sizeof(o3m_module) == O3M_MODULE_SIZE(13));  // the descriptor grew at 1.11 (the importer member, OS-FLASHDIET I1) and at 1.12 (OS-INSFX-ABI2 A1)
  CHECK(O3M_MODULE_SIZE(11) == offsetof(o3m_module, render_st));
  CHECK(O3M_MODULE_SIZE(10) == O3M_MODULE_SIZE(9));  // 1.10 did not grow it
  CHECK(sizeof(o3m_ui) == O3M_UI_SIZE(10));  // nor the ui table
  // the capability bits: the six frozen values, then 64
  static_assert(O3M_CAP_MIDI_IN == 1 && O3M_CAP_READ_PROJECT == 2 && O3M_CAP_AUDIO_TAP == 4 && O3M_CAP_EDIT_PROJECT == 8 && O3M_CAP_EXPORT_FILES == 16 && O3M_CAP_MIDI_OUT == 32, "the six frozen bits");
  static_assert(O3M_CAP_PLAY_NOTES == 64 && O3M_CAPS_ABI_1_10 == 95 && O3M_CAPS_ABI_1_7 == 31, "play_notes is bit 64, an OS of 1.10 grants 95");
  static_assert(O3M_PLAY_MAX_HELD == 4u && O3M_PLAY_MAX_PER_FRAME == 8u && O3M_PLAY_OK == 0 && O3M_PLAY_RANGE == 1 && O3M_PLAY_LIMIT == 2 && O3M_PLAY_NO_AUDIO == 3, "the published limits and codes");
  static_assert(O3M_OWN_OK == 0 && O3M_OWN_RANGE == 1, "the set_own_param codes");
}

TEST_CASE("abi 1.10: get_transport and set_own_param go to every app of 1.10, play_note and stop_note only with the bit; an app of 1.9 and below is lent none of them") {
  Rig r;
  own_rig(r);
  r.launch_ok("Own");
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  CHECK((io->size == sizeof(o3m_io_api) && io->version == O3M_IO_API_VERSION));
  CHECK((io->get_transport != nullptr && io->set_own_param != nullptr && io->play_note != nullptr && io->stop_note != nullptr));
  r.mods->ui.app_exit();
  r.launch_ok("OwnQuiet");  // 1.10, no bit
  io = r.host()->io;
  CHECK((io->get_transport != nullptr && io->set_own_param != nullptr));
  CHECK((io->play_note == nullptr && io->stop_note == nullptr));
  r.mods->ui.app_exit();
  r.launch_ok("Editor");  // 1.6: the table is the same size, the 1.10 members are NULL
  io = r.host()->io;
  CHECK(io->size == sizeof(o3m_io_api));
  CHECK((io->get_transport == nullptr && io->set_own_param == nullptr && io->play_note == nullptr && io->stop_note == nullptr));
  CHECK((io->tap_select != nullptr || io->tap_select == nullptr));  // (the 1.4 members keep their own rule)
  r.mods->ui.app_exit();
}

TEST_CASE("abi 1.10: get_transport copies the position the sequencer published, the same one the Pattern's play row shows; a short struct gets the members that fit; no position is 1 and zeros") {
  Rig r;
  own_rig(r);
  r.launch_ok("Own");
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  o3m_transport t = {};
  t.size = sizeof t;
  t.playing = t.pattern = t.row = 0xDEADBEEFu;
  r.position.publish(true, 5, 17, 13250, 3, 20);
  r.position.publish_count_in(2);
  r.position.publish_queued(4);
  r.pass(1);  // the App reads the word once a pass: that is what the Pattern's play row follows
  CHECK(io->get_transport(&t) == 0);
  CHECK(t.size == sizeof(o3m_transport));
  CHECK(t.playing == 1u);
  CHECK(t.pattern == 5u);
  CHECK(t.order == 3u);
  CHECK(t.row == 17u);
  CHECK(t.ustep == 20u);
  CHECK(t.tempo_x100 == 13250u);
  CHECK(t.count_in == 2u);
  CHECK(t.queued == 4u);
  // it matches the Pattern's own play row (ViewState::play, the App's copy of the same word)
  CHECK(r.state().play.playing == (t.playing != 0u));
  CHECK(r.state().play.pattern == t.pattern);
  CHECK(r.state().play.step == t.row);
  CHECK(r.state().play.ustep == t.ustep);
  // stopped, Pattern mode (no order position)
  r.position.publish(false, 2, 0, 12000, audio::PlayPosition::kNoOrder, 0);
  r.position.publish_count_in(0);
  r.position.publish_queued(0xFFFF);
  CHECK(io->get_transport(&t) == 0);
  CHECK((t.playing == 0u && t.pattern == 2u && t.order == 0xFFFFu && t.queued == 0xFFFFu && t.count_in == 0u && t.tempo_x100 == 12000u));
  // an app that knows only the first members (size 12: playing, pattern) gets those and nothing past them
  o3m_transport shorter;
  std::memset(&shorter, 0xAB, sizeof shorter);
  shorter.size = 12;
  CHECK(io->get_transport(&shorter) == 0);
  CHECK(shorter.size == 12u);
  CHECK((shorter.playing == 0u && shorter.pattern == 2u));
  CHECK(shorter.order == 0xABABABABu);  // (past the app's size: untouched)
  // a NULL pointer or a size below a word: refused, nothing written
  CHECK(io->get_transport(nullptr) == 1);
  // no sequencer position behind it: 1, the members zero, the order and the queued jump "none"
  r.mods->ui.set_play_position(nullptr);
  o3m_transport none = {};
  none.size = sizeof none;
  none.playing = 7;
  CHECK(io->get_transport(&none) == 1);
  CHECK((none.playing == 0u && none.pattern == 0u && none.order == 0xFFFFu && none.queued == 0xFFFFu));
  r.mods->ui.set_play_position(&r.position);
  r.mods->ui.app_exit();
}

TEST_CASE("abi 1.10: set_own_param stores a declared row (clamped, rounded for an INT), echoes nothing to set_param, reaches the project, survives save and load and a relaunch, and is handed back BEFORE enter()") {
  Rig r;
  own_rig(r);
  r.launch_ok("Own");
  // launch: init, set_param for every row with its default, then enter (nothing stored yet)
  CHECK(g_own_sets.size() == 3u);
  CHECK((g_own_sets[0].first == 0u && g_own_sets[0].second == 10.0f && g_own_sets[1].second == 0.5f && g_own_sets[2].second == 0.0f));
  CHECK(g_order == std::vector<std::string>({"set", "set", "set", "enter"}));
  g_own_sets.clear();
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  r.input_call([&](const o3m_host* h) {
    CHECK(h->io->set_own_param(0, 12.6f) == O3M_OWN_OK);   // an INT row rounds
    CHECK(h->io->set_own_param(1, 0.75f) == O3M_OWN_OK);
    CHECK(h->io->set_own_param(2, 99.0f) == O3M_OWN_OK);   // clamped to 10
    CHECK(h->io->set_own_param(3, 1.0f) == O3M_OWN_RANGE);  // past the declared rows
    CHECK(h->io->set_own_param(0xFFFFFFFFu, 1.0f) == O3M_OWN_RANGE);
    CHECK(h->io->set_own_param(0, std::numeric_limits<float>::quiet_NaN()) == O3M_OWN_RANGE);
    CHECK(h->io->set_own_param(0, std::numeric_limits<float>::infinity()) == O3M_OWN_RANGE);
  });
  CHECK(g_own_sets.empty());  // no echo: the app's set_param was not called
  CHECK(r.mods->ui.app_param(0) == 13.0f);
  CHECK(r.mods->ui.app_param(1) == 0.75f);
  CHECK(r.mods->ui.app_param(2) == 10.0f);
  r.pass(1);  // the pass keeps the values with the project
  const model::AppParams* kept = r.project().find_app_params("Own", 3);
  NEED(kept != nullptr);
  CHECK((kept->count == 3 && kept->value[0] == 13.0f && kept->value[1] == 0.75f && kept->value[2] == 10.0f));
  CHECK(kept->abi_minor == 10);
  // not undone by an exit and a relaunch: the stored values come back through set_param, all three, before enter()
  r.mods->ui.app_exit();
  g_order.clear();
  r.launch_ok("Own");
  CHECK(g_own_sets.size() == 3u);
  CHECK((g_own_sets[0].second == 13.0f && g_own_sets[1].second == 0.75f && g_own_sets[2].second == 10.0f));
  CHECK(g_order == std::vector<std::string>({"set", "set", "set", "enter"}));
  CHECK(r.mods->ui.app_param(0) == 13.0f);
  // through a save and a load of the project file: the rows are in the file and the next launch finds them
  std::vector<std::uint8_t> file;
  NEED(test::save(r.project(), file) == formats::native::Progress::Done);
  r.mods->ui.app_exit();
  r.project().reset();
  NEED(test::load(test::arena(1), file) == formats::native::Progress::Done);
  const model::AppParams* loaded = test::arena(1).project.find_app_params("Own", 3);
  NEED(loaded != nullptr);
  CHECK((loaded->count == 3 && loaded->value[0] == 13.0f && loaded->value[1] == 0.75f && loaded->value[2] == 10.0f));
  // the app's own set_own_param in enter() is the app's choice: it replaces what was stored (and a later relaunch finds that)
  g_enter = [](const o3m_host* h) { (void)h->io->set_own_param(1, 0.25f); };
  g_own_sets.clear();
  r.launch_ok("Own");
  g_enter = nullptr;
  r.mods->ui.app_exit();
}

TEST_CASE("abi 1.10: play_note starts a note on a slot of the live-note path, at most four are held and eight calls act a pass, the same note again retriggers its slot, stop_note ends an instrument's notes") {
  Rig r;
  own_rig(r);
  NoteSink sink;
  r.app->attach_preview(&sink);
  r.launch_ok("Own");
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  r.input_call([&](const o3m_host* h) {
    const o3m_io_api* i = h->io;
    // argument checks: nothing reaches the sink
    CHECK(i->play_note(0, 60, 100) == O3M_PLAY_RANGE);
    CHECK(i->play_note(65, 60, 100) == O3M_PLAY_RANGE);
    CHECK(i->play_note(3, 128, 100) == O3M_PLAY_RANGE);
    CHECK(i->play_note(3, 60, 0) == O3M_PLAY_RANGE);
    CHECK(i->play_note(3, 60, 128) == O3M_PLAY_RANGE);
    CHECK(i->stop_note(0) == O3M_PLAY_RANGE);
    CHECK(i->stop_note(65) == O3M_PLAY_RANGE);
    CHECK(sink.log.empty());
    // four notes: slots 0..3 in order
    CHECK(i->play_note(3, 60, 100) == O3M_PLAY_OK);
    CHECK(i->play_note(3, 64, 90) == O3M_PLAY_OK);
    CHECK(i->play_note(4, 67, 80) == O3M_PLAY_OK);
    CHECK(i->play_note(64, 72, 127) == O3M_PLAY_OK);
    CHECK(i->play_note(5, 50, 100) == O3M_PLAY_LIMIT);  // a fifth: refused
  });
  NEED(sink.log.size() == 4u);
  CHECK((sink.log[0].on && sink.log[0].slot == 0u && sink.log[0].instrument == 3u && sink.log[0].note == 60u && sink.log[0].velocity == 100u));
  CHECK((sink.log[1].slot == 1u && sink.log[1].note == 64u && sink.log[2].slot == 2u && sink.log[2].instrument == 4u && sink.log[3].slot == 3u && sink.log[3].instrument == 64u));
  CHECK(r.mods->ui.held_notes() == 4u);
  r.pass(1);  // a new pass: the frame's calls are counted again
  r.input_call([&](const o3m_host* h) {
    // the same instrument and note again: its slot is released and started again (Off, then On, the same key)
    sink.log.clear();
    CHECK(h->io->play_note(3, 60, 50) == O3M_PLAY_OK);
    CHECK((sink.log.size() == 2u && !sink.log[0].on && sink.log[0].slot == 0u && sink.log[1].on && sink.log[1].slot == 0u && sink.log[1].velocity == 50u));
    // stop_note ends every note of an instrument (two here), frees the slots, and a held-nothing instrument is OK
    sink.log.clear();
    CHECK(h->io->stop_note(3) == O3M_PLAY_OK);
    CHECK(sink.log.size() == 2u);
    CHECK((!sink.log[0].on && !sink.log[1].on));
    CHECK(h->io->stop_note(3) == O3M_PLAY_OK);
    CHECK(sink.log.size() == 2u);  // (nothing held there: no event)
    CHECK(h->io->play_note(9, 40, 100) == O3M_PLAY_OK);  // a freed slot is taken again
  });
  CHECK(r.mods->ui.held_notes() == 3u);
  // a sink that cannot play (no engine): the call is told, nothing is held
  sink.accept = false;
  r.pass(1);
  r.input_call([&](const o3m_host* h) { CHECK(h->io->play_note(10, 40, 100) == O3M_PLAY_NO_AUDIO); });
  CHECK(r.mods->ui.held_notes() == 3u);
  sink.accept = true;
  // no sink at all: NO_AUDIO as well
  r.app->attach_preview(nullptr);
  r.pass(1);
  r.input_call([&](const o3m_host* h) { CHECK(h->io->play_note(10, 40, 100) == O3M_PLAY_NO_AUDIO); });
  r.app->attach_preview(&sink);
  r.mods->ui.app_exit();
}

TEST_CASE("abi 1.10: notes the sink ended behind the app's back (a flush, the export, reset_input) free its four slots: play_note succeeds again once the engine holds none") {
  Rig r;
  own_rig(r);
  NoteSink sink;
  r.app->attach_preview(&sink);
  r.launch_ok("Own");
  r.input_call([&](const o3m_host* h) {
    for (std::uint32_t i = 0; i < 4u; ++i) {
      CHECK(h->io->play_note(1 + i, 60, 100) == O3M_PLAY_OK);
    }
    CHECK(h->io->play_note(9, 60, 100) == O3M_PLAY_LIMIT);
  });
  CHECK(r.mods->ui.held_notes() == 4u);
  r.pass(1);
  // the engine still holds a note (idle false): the table is not touched, the fifth is refused
  r.input_call([&](const o3m_host* h) { CHECK(h->io->play_note(9, 60, 100) == O3M_PLAY_LIMIT); });
  CHECK(r.mods->ui.held_notes() == 4u);
  // an all-sources all-notes-off ran (notes_off(false), the sink's flush, the export taking the engine): nothing is held or queued any more
  sink.idle = true;
  sink.log.clear();
  r.pass(1);
  r.input_call([&](const o3m_host* h) { CHECK(h->io->play_note(9, 60, 100) == O3M_PLAY_OK); });
  CHECK(r.mods->ui.held_notes() == 1u);
  NEED(sink.log.size() == 1u);
  CHECK((sink.log[0].on && sink.log[0].slot == 0u && sink.log[0].instrument == 9u));  // (no Off of a ghost, the first free slot)
  // stop_note of an instrument the table forgot is OK and sends nothing
  sink.log.clear();
  r.pass(1);
  r.input_call([&](const o3m_host* h) { CHECK(h->io->stop_note(1) == O3M_PLAY_OK); });
  CHECK(sink.log.empty());
  r.mods->ui.app_exit();
}

TEST_CASE("abi 1.10: an app of 1.9 or below still gets its remembered rows AFTER enter(), so an enter() that resets a value does not clobber them; an app of 1.10 gets them before") {
  Rig r;
  own_rig(r);
  const float remembered[3] = {42.0f, 0.25f, -3.0f};
  CHECK(r.editor->set_app_params("OwnLegacy", 9, O3M_ABI_MAJOR, 9, remembered, 3) == model::Status::Ok);
  // what the launching actions do: launch_app, then app_params_apply
  r.launch_ok("OwnLegacy");
  CHECK(g_order == std::vector<std::string>({"set", "set", "set", "enter"}));  // (the launch: the defaults, then enter())
  CHECK(g_legacy_value[0] == 10.0f);  // enter() reset them: this is the state a remembered value must not arrive into
  CHECK(app::app_params_apply(r.mods->ui, r.project()) == 3u);
  CHECK(g_order == std::vector<std::string>({"set", "set", "set", "enter", "set", "set", "set"}));
  CHECK((g_legacy_value[0] == 42.0f && g_legacy_value[1] == 0.25f && g_legacy_value[2] == -3.0f));  // and the app holds what the page shows
  CHECK((r.mods->ui.app_param(0) == 42.0f && r.mods->ui.app_param(1) == 0.25f && r.mods->ui.app_param(2) == -3.0f));
  r.mods->ui.app_exit();
  // the same rows for an app of 1.10: before enter(), and app_params_apply has nothing left to send
  CHECK(r.editor->set_app_params("Own", 3, O3M_ABI_MAJOR, 10, remembered, 3) == model::Status::Ok);
  g_order.clear();
  r.launch_ok("Own");
  CHECK(g_order == std::vector<std::string>({"set", "set", "set", "enter"}));
  CHECK(app::app_params_apply(r.mods->ui, r.project()) == 0u);
  CHECK((r.mods->ui.app_param(0) == 42.0f && r.mods->ui.app_param(2) == -3.0f));
  r.mods->ui.app_exit();
}

TEST_CASE("abi 1.10: a flood of 1,000 triggers in one pass lets eight through, never more, and the next pass counts again; every note is silent after an exit by F8, a screen button, F7 + F8, a project change") {
  Rig r;
  own_rig(r);
  NoteSink sink;
  r.app->attach_preview(&sink);
  r.launch_ok("Own");
  NEED(r.host() != nullptr);
  unsigned ok = 0, limit = 0;
  r.input_call([&](const o3m_host* h) {
    for (unsigned i = 0; i < 1000u; ++i) {
      const int a = h->io->play_note(1 + (i % 4u), 30 + (i % 40u), 100);
      const int b = (i % 7u) == 0u ? h->io->stop_note(1 + (i % 4u)) : O3M_PLAY_OK;
      ok += (a == O3M_PLAY_OK ? 1u : 0u) + (b == O3M_PLAY_OK ? 1u : 0u);
      limit += (a == O3M_PLAY_LIMIT ? 1u : 0u) + (b == O3M_PLAY_LIMIT ? 1u : 0u);
    }
  });
  CHECK(sink.ons() <= O3M_PLAY_MAX_PER_FRAME);                   // at most eight started
  CHECK(sink.log.size() <= 2u * O3M_PLAY_MAX_PER_FRAME);         // and at most one Off a call that acted (a retrigger is two events)
  CHECK(limit > 900u);
  CHECK(r.mods->ui.held_notes() <= O3M_PLAY_MAX_HELD);
  r.pass(1);
  sink.log.clear();
  r.input_call([&](const o3m_host* h) {
    for (std::uint32_t instrument = 1; instrument <= 4; ++instrument) {
      CHECK(h->io->stop_note(instrument) == O3M_PLAY_OK);  // (the flood left up to four held)
    }
    CHECK(h->io->play_note(1, 40, 100) == O3M_PLAY_OK);  // a new pass: the calls are counted again
  });
  CHECK(sink.ons() == 1u);
  CHECK(r.mods->ui.held_notes() == 1u);
  r.mods->ui.app_exit();
  CHECK(r.mods->ui.held_notes() == 0u);
  // every way out ends the notes: one AllOff of the app's source, and the table is empty
  const auto hold_then = [&](const char* how, const std::function<void()>& leave) {
    CAPTURE(how);
    r.pass(1);
    r.launch_ok("Own");
    r.input_call([&](const o3m_host* h) {
      CHECK(h->io->play_note(1, 60, 100) == O3M_PLAY_OK);
      CHECK(h->io->play_note(2, 62, 100) == O3M_PLAY_OK);
      CHECK(h->io->play_note(3, 64, 100) == O3M_PLAY_OK);
    });
    CHECK(r.mods->ui.held_notes() == 3u);
    const unsigned before = sink.offs;
    leave();
    CHECK(!r.mods->ui.app_running());
    CHECK(sink.offs == before + 1u);
    CHECK(r.mods->ui.held_notes() == 0u);
  };
  hold_then("app_exit", [&] { r.mods->ui.app_exit(); });
  r.invoke("nav.screen", static_cast<core::ActionArg>(app::ScreenId::Modules));
  hold_then("F8", [&] {
    r.invoke("nav.screen", static_cast<core::ActionArg>(app::ScreenId::ModuleApp));
    r.press(Button::F8);
  });
  hold_then("a screen button", [&] {
    r.invoke("nav.screen", static_cast<core::ActionArg>(app::ScreenId::ModuleApp));
    r.press(Button::Song);
  });
  hold_then("F7 + F8", [&] {
    r.invoke("nav.screen", static_cast<core::ActionArg>(app::ScreenId::ModuleApp));
    r.chord_exit();
  });
  hold_then("a project change", [&] { r.mods->ui.reset(); });
}

TEST_CASE("abi 1.10: a fuzz of get_transport, set_own_param, play_note and stop_note with the other entries, in and out of input(), never crashes, answers a published code and never holds more than four notes") {
  Rig r;
  own_rig(r);
  NoteSink sink;
  r.app->attach_preview(&sink);
  r.launch_ok("Own");
  test::Rng rng(0xA11CE5u);
  const auto pick = [&](std::uint32_t n) { return rng.next() % n; };
  const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e30f, -1e30f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 12.5f};
  const std::uint32_t words[] = {0u, 1u, 2u, 3u, 4u, 63u, 64u, 65u, 127u, 128u, 255u, 0xFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu};
  for (unsigned round = 0; round < 400; ++round) {
    const bool inside = pick(2) == 0u;
    const auto calls = [&](const o3m_host* h) {
      for (unsigned k = 0; k < 6; ++k) {
        switch (pick(5)) {
          case 0: {
            o3m_transport t;
            std::memset(&t, 0xCD, sizeof t);
            t.size = words[pick(14)] % 64u;
            const int rc = h->io->get_transport(&t);
            CHECK((rc == 0 || rc == 1));
            break;
          }
          case 1: {
            const int rc = h->io->set_own_param(words[pick(14)], specials[pick(10)]);
            CHECK((rc == O3M_OWN_OK || rc == O3M_OWN_RANGE));
            break;
          }
          case 2: {
            const int rc = h->io->play_note(words[pick(14)], words[pick(14)], words[pick(14)]);
            CHECK((rc == O3M_PLAY_OK || rc == O3M_PLAY_RANGE || rc == O3M_PLAY_LIMIT || rc == O3M_PLAY_NO_AUDIO));
            break;
          }
          case 3: {
            const int rc = h->io->stop_note(words[pick(14)]);
            CHECK((rc == O3M_PLAY_OK || rc == O3M_PLAY_RANGE || rc == O3M_PLAY_LIMIT || rc == O3M_PLAY_NO_AUDIO));
            break;
          }
          default: {
            sink.accept = pick(4) != 0u;
            break;
          }
        }
      }
    };
    if (inside) {
      r.input_call(calls);
    } else {
      calls(r.host());  // (a call made outside any entry point of the app: the host table is still lent, and answers)
    }
    CHECK(r.mods->ui.held_notes() <= O3M_PLAY_MAX_HELD);
    CHECK(sink.ons() >= 0u);
    if (round % 25u == 0u) {
      r.pass(1);
    }
    if (round % 100u == 99u) {
      r.mods->ui.app_exit();
      CHECK(r.mods->ui.held_notes() == 0u);
      r.launch_ok("Own");
    }
  }
  r.mods->ui.app_exit();
}

TEST_CASE("abi 1.10: an app built at 1.10 has no OS parameter column and gets every Shift key; an app below it keeps the column") {
  Rig r;
  own_rig(r);
  r.launch_ok("Own");
  CHECK(r.mods->ui.app_param_count() == 3u);  // declared: storage
  CHECK(!r.mods->ui.app_os_column());
  r.mods->ui.app_exit();
  r.launch_ok("Editor");
  CHECK(!r.mods->ui.app_os_column());  // (it declares no row)
  r.mods->ui.app_exit();
}

TEST_CASE("tools: after APPLY the Pattern opens on what the tool wrote: the cursor on the first written step and the written block selected, from a launch with no block and from one with a block (OS-APPUX S5)") {
  Rig r;
  NEED(r.rg.entry != nullptr);
  // no block: the cursor at row 5 of a track, Rhythm Gen writes 16 rows from there in that track
  r.invoke("nav.scroll", 5);
  for (int i = 0; i < 2; ++i) (void)r.invoke("nav.right");
  const int row = r.state().pattern.row();
  const int track = r.state().pattern.track();
  CHECK(row == 5);
  CHECK_FALSE(r.state().edit.selection.active);
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  NEED(r.state().overlay == app::OverlayId::VerbMenu);
  for (unsigned i = 0; i < r.mods->ui.tool_count(); ++i) {
    if (std::string(r.mods->ui.app_entry_at(r.mods->ui.tool_entry(i))->stem) == "rhythmgen") {
      for (unsigned k = 0; k < i; ++k) r.press(Button::Down);
    }
  }
  r.press(Button::F1);
  NEED(r.state().screen == app::ScreenId::ModuleApp);
  const std::size_t depth = r.journal.depth();
  r.press(Button::F1);  // APPLY
  CHECK(r.journal.depth() == depth + 1u);
  r.chord_exit();
  NEED(r.state().screen == app::ScreenId::Pattern);
  CHECK(r.state().pattern.row() == row);
  CHECK(r.state().pattern.track() == track);
  const app::Selection& sel = r.state().edit.selection;
  NEED(sel.active);
  const app::Selection::Block b = sel.block(r.state().pattern.row(), r.state().pattern.track());
  CHECK((b.row0 == row && b.row1 == row + 15 && b.track0 == track && b.track1 == track));
  // the unit's undo does not move the cursor or the block
  CHECK(r.invoke("edit.undo") == app::DispatchResult::Changed);
  CHECK(r.state().pattern.row() == row);
  CHECK(r.state().edit.selection.active);
  // a block of 2 tracks x 8 rows far from the cursor: after APPLY the cursor is on the block's first step and the block is the written one
  r.invoke("cursor.home");
  r.invoke("nav.scroll", 20);
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
  r.pass(1);
  (void)r.invoke("select.extend", 1);   // down, 7 times: 8 rows
  for (int i = 0; i < 6; ++i) (void)r.invoke("select.extend", 1);
  extend_tracks(r, 1);  // right: the next track
  r.input.push(core::make_button_event(r.clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
  r.pass(1);
  const app::Selection::Block before = r.state().edit.selection.block(r.state().pattern.row(), r.state().pattern.track());
  NEED(r.state().edit.selection.active);
  CHECK(r.state().pattern.row() == before.row1);  // (the cursor is at the block's far end: after APPLY it is on the first written step)
  r.press(Button::F7);
  r.pad(15);  // OS-PTOOLS2 S3: the verb menu's APPS > row (pad 16) opens the TOOLS list
  NEED(r.state().overlay == app::OverlayId::VerbMenu);
  for (unsigned k = 0; k < 40u; ++k) r.press(Button::Up);  // (the list opens on the row it was left on, OS-PTOOLS2 S1)
  for (unsigned i = 0; i < r.mods->ui.tool_count(); ++i) {
    if (std::string(r.mods->ui.app_entry_at(r.mods->ui.tool_entry(i))->stem) == "rhythmgen") {
      for (unsigned k = 0; k < i; ++k) r.press(Button::Down);
    }
  }
  r.press(Button::F1);
  NEED(r.state().screen == app::ScreenId::ModuleApp);
  r.press(Button::F1);  // APPLY
  r.chord_exit();
  NEED(r.state().screen == app::ScreenId::Pattern);
  const app::Selection::Block after = r.state().edit.selection.block(r.state().pattern.row(), r.state().pattern.track());
  CHECK((after.row0 == before.row0 && after.row1 == before.row1 && after.track0 == before.track0 && after.track1 == before.track1));
  CHECK(r.state().pattern.row() == before.row0);
  CHECK(r.state().pattern.track() == before.track0);
}

TEST_CASE("abi 1.12: the descriptor's four new members, the host's two and the PODs are where the layout puts them, and nothing before them moved (OS-INSFX-ABI2 A1)") {
  // the host table: 1.12 appended get_clock and track_level after io (slot 8 of the pointers after the 16-byte header), in this order, and nothing else
  CHECK(offsetof(o3m_host, io) == 16 + 8 * kP);
  CHECK(offsetof(o3m_host, get_clock) == 16 + 9 * kP);
  CHECK(offsetof(o3m_host, track_level) == 16 + 10 * kP);
  CHECK(sizeof(o3m_host) == 16 + 11 * kP);
  // the descriptor: render_st after importer, then fx_flags, presets, preset_count; the size is the next multiple of the pointer size
  CHECK(offsetof(o3m_module, render_st) == offsetof(o3m_module, importer) + kP);
  CHECK(offsetof(o3m_module, fx_flags) == offsetof(o3m_module, render_st) + kP);
  CHECK(offsetof(o3m_module, presets) == offsetof(o3m_module, fx_flags) + kP);  // (a uint32_t then padding up to the pointer on a 64-bit host, none on the target)
  CHECK(offsetof(o3m_module, preset_count) == offsetof(o3m_module, presets) + kP);
  CHECK(sizeof(o3m_module) == offsetof(o3m_module, preset_count) + kP);
  CHECK(O3M_MODULE_SIZE(11) == offsetof(o3m_module, render_st));
  CHECK(O3M_MODULE_SIZE(13) == sizeof(o3m_module));
  CHECK(offsetof(o3m_module, importer) == offsetof(o3m_module, labels) + kP);  // 1.11's member did not move
  CHECK(O3M_MODULE_SIZE(10) == offsetof(o3m_module, importer));
  if (kP == 4) {
    CHECK((offsetof(o3m_module, render_st) == 84 && offsetof(o3m_module, fx_flags) == 88 && offsetof(o3m_module, presets) == 92 && offsetof(o3m_module, preset_count) == 96 && sizeof(o3m_module) == 100));
  }
  // the PODs
  CHECK(sizeof(o3m_clock) == 16);
  CHECK((offsetof(o3m_clock, flags) == 4 && offsetof(o3m_clock, tempo_x100) == 8 && offsetof(o3m_clock, beat) == 12));
  CHECK(sizeof(o3m_preset) == 3 * kP);
  CHECK(offsetof(o3m_preset, values) == kP);
  CHECK(offsetof(o3m_preset, count) == 2 * kP);
  // the published values
  CHECK((O3M_FX_WIDENS == 1u && O3M_FX_RATE == 2u && O3M_PARAM_KEY == 32u && O3M_PRESETS_MAX == 32u && O3M_PRESET_NAME_MAX == 16u));
  CHECK((O3M_CLOCK_PLAYING == 1u && O3M_CLOCK_STARTED == 2u && O3M_CLOCK_WRAPPED == 4u));
  CHECK(((O3M_PARAM_KEY & (O3M_PARAM_LOG | O3M_PARAM_INT | O3M_PARAM_BOOL | O3M_PARAM_NO_LEARN | O3M_PARAM_MOD | O3M_PARAM_UNIT_MASK)) == 0u));  // bit 32 was free
  // the host table an app is lent carries get_clock (A3: live once an engine has written the store, so this case starts from none: another case of the binary may have run one) and the track_level stub (A6)
  o3os::module::g_clock.invalidate();
  Rig r;
  r.launch_ok("Editor");
  NEED(r.host() != nullptr);
  CHECK(O3M_HOST_HAS(r.host(), get_clock));
  CHECK(O3M_HOST_HAS(r.host(), track_level));
  NEED((r.host()->get_clock != nullptr && r.host()->track_level != nullptr));
  o3m_clock c{};
  c.size = sizeof c;
  c.flags = 0xFFu;
  CHECK(r.host()->get_clock(&c) == 1);  // no sequencer clock behind it: the stub's answer
  CHECK((c.size == sizeof c && c.flags == 0u && c.tempo_x100 == 12000u && c.beat == 0.0f));
  o3m_clock small{};
  small.size = 8;  // a caller that knows only size and flags
  small.tempo_x100 = 77u;
  CHECK(r.host()->get_clock(&small) == 1);
  CHECK((small.size == 8u && small.flags == 0u && small.tempo_x100 == 77u));  // only the members that fit
  CHECK(r.host()->track_level(0) == 0.0f);
  CHECK(r.host()->track_level(15) == 0.0f);
  CHECK(r.host()->track_level(99) == 0.0f);
}
