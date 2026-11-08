// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-H1 (ADR 0014, ABI 1.3): the app host API. What a running app is lent (the read, edit and io tables, each NULL unless its capability was asked for), the clock, the tempo read and
// the queued set_tempo that the App applies as one undoable action, the launch context, the once-per-boot confirm of an app that edits the project, the MIDI-in ring (the monitor in
// the render's control-change object, drained by the app's own midi_read), and the exit chord that an app sees as two presses. Native test apps; the card is a temporary directory.
// Host only.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/screens/screen.h>
#include <o3os/audio/app_tap.h>
#include <o3os/audio/module_voices.h>
#include <o3os/core/input_event.h>
#include <o3os/midi/cc_in.h>
#include <o3os/midi/monitor.h>
#include <o3os/model/editor.h>
#include <o3os/model/limits.h>
#include <o3os/module/loader.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/demo_project.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "support/key_row.h"
#include "support/module_blob.h"
#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using app::Button;
namespace fs = std::filesystem;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

// ---- the test apps ----------------------------------------------------------------------------------------------------------------------------------
struct HState {
  const o3m_host* host;  // what init() was given
  std::uint32_t inits, enters, exits, shutdowns, inputs;
  o3m_ui_event last_event;
};

int h_init(void* s, const o3m_host* host) {
  HState* h = static_cast<HState*>(s);
  h->host = host;
  ++h->inits;
  return host->draw != nullptr ? 0 : 1;
}
void h_shutdown(void* s) { ++static_cast<HState*>(s)->shutdowns; }
void h_enter(void* s) { ++static_cast<HState*>(s)->enters; }
void h_exit(void* s) { ++static_cast<HState*>(s)->exits; }
void h_input(void* s, const o3m_ui_event* e) {
  HState* h = static_cast<HState*>(s);
  ++h->inputs;
  h->last_event = *e;
}
void h_draw(void*, std::uint32_t, o3m_canvas*) {}
void h_set_param(void*, std::uint32_t, float) {}

const o3m_ui kUi = {sizeof(o3m_ui), h_enter, h_input, h_draw, h_exit, nullptr};
// Caps: ABI 1.3 with every capability of it. Bare: ABI 1.3, asks for nothing. Old: declares ABI 1.2 (an app packed before 1.3). Reader: read_project and midi_in, no edit.
const o3m_module kCaps = {O3M_ABI_MAJOR, 3, O3M_KIND_APP, O3M_MODULE_SIZE(3), "Caps", sizeof(HState), 0, nullptr, h_init, nullptr, h_set_param, nullptr, nullptr, h_shutdown, &kUi, 1};
const o3m_module kBare = {O3M_ABI_MAJOR, 3, O3M_KIND_APP, O3M_MODULE_SIZE(3), "Bare", sizeof(HState), 0, nullptr, h_init, nullptr, h_set_param, nullptr, nullptr, h_shutdown, &kUi, 1};
const o3m_module kOld = {O3M_ABI_MAJOR, 2, O3M_KIND_APP, O3M_MODULE_SIZE(2), "Old", sizeof(HState), 0, nullptr, h_init, nullptr, h_set_param, nullptr, nullptr, h_shutdown, &kUi, 1};
// Tapper: ABI 1.4 with the audio tap (OS-H2a). The 1.3 apps above cannot hold it: the bit is a reserved field below 1.4.
const o3m_module kTapper = {O3M_ABI_MAJOR, 4, O3M_KIND_APP, O3M_MODULE_SIZE(4), "Tapper", sizeof(HState), 0, nullptr, h_init, nullptr, h_set_param, nullptr, nullptr, h_shutdown, &kUi, 1};
const o3m_module kReader = {O3M_ABI_MAJOR, 3, O3M_KIND_APP, O3M_MODULE_SIZE(3), "Reader", sizeof(HState), 0, nullptr, h_init, nullptr, h_set_param, nullptr, nullptr, h_shutdown, &kUi, 1};

// Extra apps for the capability tests (Rig(true)): natives that hold one capability each, and real .tmod files whose header asks for a bit this OS does not grant (C4).
const char* const kExtraNative[] = {"OnlyMidi", "OnlyRead", "OnlyEdit", "TapLike", "Tapper"};
struct NeedCase {
  const char* stem;
  std::uint32_t caps;
  const char* text;
};
const NeedCase kNeed[] = {
    {"NeedOut", O3M_CAP_MIDI_OUT, "NEEDS MIDI OUT (NEWER OS)"},
    {"NeedBoth", O3M_CAP_MIDI_IN | O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT | O3M_CAP_MIDI_OUT, "NEEDS MIDI OUT (NEWER OS)"},
};

struct Card {
  fs::path root;
  explicit Card(bool extras = false) {
    root = fs::temp_directory_path() / ("o3m_host_api_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "modules" / "apps");
    for (const char* n : {"Caps", "Bare", "Old", "Reader"}) {
      std::ofstream f(root / "modules" / "apps" / (std::string(n) + ".tmod"), std::ios::binary);
      f << "x";
    }
    if (extras) {
      for (const char* n : kExtraNative) {
        std::ofstream f(root / "modules" / "apps" / (std::string(n) + ".tmod"), std::ios::binary);
        f << "x";
      }
      for (const NeedCase& c : kNeed) {
        module::test::Blob b = module::test::blob_app(1, 3);
        b.h.reserved2[0] = c.caps;
        b.seal();
        const std::vector<std::uint8_t> bytes = b.file();
        std::ofstream f(root / "modules" / "apps" / (std::string(c.stem) + ".tmod"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      }
    }
  }
  ~Card() {
    std::error_code ec;
    fs::remove_all(root, ec);
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

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  Card card;
  platform::host::DirStorage storage{card.root.string()};
  std::unique_ptr<Modules> mods = std::make_unique<Modules>();
  midi::CcInput cc;
  std::unique_ptr<audio::AppTap> app_tap = std::make_unique<audio::AppTap>();

  explicit Rig(bool extras = false) : card(extras) {
    NEED(storage.mount() == hal::storage::Status::Ok);
    (void)mods->voices.attach_scratch(mods->scratch, sizeof mods->scratch);
    (void)mods->set.cold().init(mods->sdram, 0x70000000u, sizeof mods->sdram);
    mods->ui.attach(mods->set, mods->loader, mods->voices);
    mods->ui.set_storage(&storage);
    mods->ui.set_clock(&clock, nullptr);
    mods->ui.set_midi_monitor(&cc.monitor());
    mods->ui.set_audio_tap(app_tap.get());
    CHECK(mods->set.add_native("Caps", kCaps, O3M_CAPS_ABI_1_3));
    CHECK(mods->set.add_native("Bare", kBare));
    CHECK(mods->set.add_native("Old", kOld));
    CHECK(mods->set.add_native("Reader", kReader, O3M_CAP_READ_PROJECT | O3M_CAP_MIDI_IN));
    CHECK(!mods->set.add_native("Bad", kOld, O3M_CAP_MIDI_IN));  // an ABI 1.2 app cannot ask
    if (extras) {
      CHECK(mods->set.add_native("OnlyMidi", kBare, O3M_CAP_MIDI_IN));
      CHECK(mods->set.add_native("OnlyRead", kBare, O3M_CAP_READ_PROJECT));
      CHECK(mods->set.add_native("OnlyEdit", kBare, O3M_CAP_EDIT_PROJECT));
      CHECK(mods->set.add_native("TapLike", kBare, O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT));  // the capability word of the Tap Calc file (pinned in tapcalc_test)
      CHECK(mods->set.add_native("Tapper", kTapper, O3M_CAP_AUDIO_TAP));
      CHECK(!mods->set.add_native("Greedy", kBare, O3M_CAP_AUDIO_TAP));  // an app of ABI 1.3 cannot hold the audio tap (1.4's)
      CHECK(!mods->set.add_native("Greedy", kBare, O3M_CAP_EXPORT_FILES));  // nor export_files (1.7's)
      CHECK(!mods->set.add_native("Greedy", kBare, O3M_CAP_MIDI_OUT));      // nor one this OS does not grant at all
    }
    model::Project& p = project();
    sim::fill_demo_project(p);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(p, journal);
    app->attach_document(editor.get());
    app->attach_modules(&mods->ui);
    app->run_once();
    mods->ui.scan_apps();
  }

  model::Project& project() { return test::arena(1).project; }
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
  // Launches an app that edits the project: the confirm answers first, the second launch goes through.
  void launch_confirmed(const char* stem) {
    if (launch(stem) == app::ModuleUi::LaunchResult::NeedsConfirm) {
      CHECK(launch(stem) == app::ModuleUi::LaunchResult::Ok);
    }
  }
  HState* state() { return static_cast<HState*>(const_cast<void*>(mods->ui.app_state())); }
  const o3m_host* host() { return mods->ui.app_host(); }
  void pass(unsigned ms = 0) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  // Bytes arriving on a port, as the render's control-change object is fed them.
  void feed(unsigned port, std::initializer_list<std::uint8_t> bytes) {
    for (std::uint8_t b : bytes) {
      midi::RtItem item{};
      item.byte = b;
      item.port = static_cast<std::uint8_t>(port);
      cc.feed(item);
    }
  }
};

std::vector<o3m_midi_event> drain(const o3m_host* h, std::uint32_t chunk = 64) {
  std::vector<o3m_midi_event> all;
  o3m_midi_event buf[64];
  for (;;) {
    const std::uint32_t n = h->io->midi_read(buf, chunk);
    if (n == 0) {
      break;
    }
    all.insert(all.end(), buf, buf + n);
  }
  return all;
}

}  // namespace

// ---- the capability gate ------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("app host: a table is lent only for the capability the app's file asked for, and an ABI 1.2 app is lent nothing new") {
  Rig r;
  NEED(r.mods->ui.app_entry_count() == 4u);
  // Caps: all three, plus the io table with the MIDI calls
  r.launch_confirmed("Caps");
  NEED(r.host() != nullptr);
  CHECK(r.host()->size == sizeof(o3m_host));
  CHECK((r.host()->abi_major == O3M_ABI_MAJOR && r.host()->abi_minor == O3M_ABI_MINOR));
  CHECK((O3M_HOST_HAS(r.host(), read) && O3M_HOST_HAS(r.host(), edit) && O3M_HOST_HAS(r.host(), io)));
  CHECK((r.host()->read != nullptr && r.host()->edit != nullptr && r.host()->io != nullptr));
  CHECK((r.host()->read->version == O3M_READ_API_VERSION && r.host()->read->size == sizeof(o3m_read_api)));
  CHECK((r.host()->edit->version == O3M_EDIT_API_VERSION && r.host()->edit->size == sizeof(o3m_edit_api)));
  CHECK((r.host()->io->version == O3M_IO_API_VERSION && r.host()->io->size == sizeof(o3m_io_api)));
  CHECK((r.host()->io->midi_read != nullptr && r.host()->io->midi_lost != nullptr && r.host()->io->now_ms != nullptr));
  CHECK((r.host()->draw != nullptr && r.host()->log != nullptr));  // the 1.1 services stay
  CHECK(r.state()->host == r.host());                              // init() was given exactly this table
  r.mods->ui.app_exit();
  CHECK(r.host() == nullptr);
  // Bare: an ABI 1.3 app that asks for nothing has the clock and no more
  CHECK(r.launch("Bare") == app::ModuleUi::LaunchResult::Ok);
  CHECK((r.host()->read == nullptr && r.host()->edit == nullptr));
  NEED(r.host()->io != nullptr);
  CHECK((r.host()->io->now_ms != nullptr && r.host()->io->midi_read == nullptr && r.host()->io->midi_lost == nullptr));
  r.mods->ui.app_exit();
  // Reader: read and MIDI, no edit table
  CHECK(r.launch("Reader") == app::ModuleUi::LaunchResult::Ok);  // no confirm: it does not edit
  CHECK((r.host()->read != nullptr && r.host()->edit == nullptr && r.host()->io != nullptr && r.host()->io->midi_read != nullptr));
  r.mods->ui.app_exit();
  // Old: a module that declares ABI 1.2 gets no new table, whatever the OS could give
  CHECK(r.launch("Old") == app::ModuleUi::LaunchResult::Ok);
  CHECK((r.host()->read == nullptr && r.host()->edit == nullptr && r.host()->io == nullptr));
  // R2: and the table it sees ends at `draw`, as the 1.2 one did, announcing ABI 1.2: a 1.2 module that checks `size` finds no 1.3 member at all
  CHECK(r.host()->size == offsetof(o3m_host, read));
  CHECK(r.host()->abi_minor == 2u);
  CHECK((!O3M_HOST_HAS(r.host(), read) && !O3M_HOST_HAS(r.host(), edit) && !O3M_HOST_HAS(r.host(), io) && O3M_HOST_HAS(r.host(), draw)));
  CHECK(r.state()->inits == 1u);
  CHECK(r.state()->enters == 1u);
  r.mods->ui.app_exit();
  CHECK((r.state()->exits == 1u && r.state()->shutdowns == 1u));
}

TEST_CASE("app host: without a MIDI ring in the build an app that asks for midi_in is lent no midi_read") {
  Rig r;
  r.mods->ui.set_midi_monitor(nullptr);
  CHECK(r.launch("Reader") == app::ModuleUi::LaunchResult::Ok);
  NEED(r.host()->io != nullptr);
  CHECK((r.host()->io->midi_read == nullptr && r.host()->io->midi_lost == nullptr));
  CHECK(r.host()->read != nullptr);
}

// ---- the clock, the tempo, the launch context --------------------------------------------------------------------------------------------------------

TEST_CASE("app host: now_ms is the device's uptime in milliseconds, monotonic, as a 32-bit count") {
  Rig r;
  CHECK(r.launch("Bare") == app::ModuleUi::LaunchResult::Ok);
  const o3m_io_api* io = r.host()->io;
  const std::uint32_t t0 = io->now_ms();
  CHECK(t0 == static_cast<std::uint32_t>(r.clock.uptime_ms()));
  r.clock.advance(250 * core::kTicksPerMs);
  const std::uint32_t t1 = io->now_ms();
  CHECK(t1 - t0 == 250u);
  r.clock.advance(1 * core::kTicksPerMs);
  CHECK(io->now_ms() - t1 == 1u);
  // the count is the low 32 bits of the 64-bit uptime (it wraps after 49.7 days; the SDK says to subtract, never to compare)
  CHECK(io->now_ms() == static_cast<std::uint32_t>(r.clock.uptime_ms()));
  r.mods->ui.app_exit();
  CHECK(r.mods->ui.host_now_ms() == static_cast<std::uint32_t>(r.clock.uptime_ms()));
}

TEST_CASE("app host: get_tempo reads the project, set_tempo queues one undoable change that the App applies, the last call of a frame wins") {
  Rig r;
  r.launch_confirmed("Caps");
  const o3m_host* h = r.host();
  NEED(h != nullptr);
  const std::uint32_t before = r.project().tempo_x100();
  CHECK(h->read->get_tempo() == before);
  const std::size_t depth = r.journal.depth();
  // range and state are checked at the call
  CHECK(h->edit->set_tempo(model::kMinTempoX100 - 1u) == O3M_EDIT_RANGE);
  CHECK(h->edit->set_tempo(model::kMaxTempoX100 + 1u) == O3M_EDIT_RANGE);
  CHECK(h->edit->set_tempo(0) == O3M_EDIT_RANGE);
  CHECK(h->edit->set_tempo(0xFFFFFFFFu) == O3M_EDIT_RANGE);
  CHECK(h->read->get_tempo() == before);
  r.pass(40);
  CHECK(r.project().tempo_x100() == before);
  CHECK(r.journal.depth() == depth);
  // two calls in one frame: one unit, the last value; a read after the write sees it before it is applied
  CHECK(h->edit->set_tempo(9000) == O3M_EDIT_OK);
  CHECK(h->read->get_tempo() == 9000u);
  CHECK(h->edit->set_tempo(9100) == O3M_EDIT_OK);
  CHECK(h->read->get_tempo() == 9100u);
  CHECK(r.project().tempo_x100() == before);  // queued, not applied inside the app's call
  r.pass(40);
  CHECK(r.project().tempo_x100() == 9100u);
  CHECK(h->read->get_tempo() == 9100u);
  CHECK(r.journal.depth() == depth + 1u);  // one undo unit
  // the limits themselves are accepted
  CHECK(h->edit->set_tempo(model::kMinTempoX100) == O3M_EDIT_OK);
  r.pass(40);
  CHECK(r.project().tempo_x100() == model::kMinTempoX100);
  CHECK(h->edit->set_tempo(model::kMaxTempoX100) == O3M_EDIT_OK);
  r.pass(40);
  CHECK(r.project().tempo_x100() == model::kMaxTempoX100);
  // setting what it already is changes nothing and leaves no unit
  const std::size_t depth2 = r.journal.depth();
  CHECK(h->edit->set_tempo(model::kMaxTempoX100) == O3M_EDIT_OK);
  r.pass(40);
  CHECK(r.journal.depth() == depth2);
  // undo takes the changes back one unit at a time, down to the project's own tempo
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == model::kMinTempoX100);
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == 9100u);
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == before);
  CHECK(r.editor->redo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == 9100u);
}

TEST_CASE("app host: a request that is still queued when the app closes is applied once, and a project change drops it") {
  Rig r;
  r.launch_confirmed("Caps");
  const std::uint32_t before = r.project().tempo_x100();
  CHECK(r.host()->edit->set_tempo(15000) == O3M_EDIT_OK);
  r.mods->ui.app_exit();
  r.pass(40);
  CHECK(r.project().tempo_x100() == 15000u);
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == before);
  // the modules were cleared by a project load: what the app had queued is not the new project's
  r.launch_confirmed("Caps");
  CHECK(r.host()->edit->set_tempo(16000) == O3M_EDIT_OK);
  r.mods->ui.reset();
  r.pass(40);
  CHECK(r.project().tempo_x100() == before);
  CHECK(r.mods->ui.app_host() == nullptr);
}

TEST_CASE("app host: the module.app_tempo action is an undoable unit of its own and refuses a bad argument") {
  Rig r;
  const std::uint32_t before = r.project().tempo_x100();
  CHECK(r.app->invoke(core::action_id("module.app_tempo"), 13300) == app::DispatchResult::Changed);
  CHECK(r.project().tempo_x100() == 13300u);
  CHECK(r.app->invoke(core::action_id("module.app_tempo"), 13300) != app::DispatchResult::Changed);  // no change
  CHECK(r.app->invoke(core::action_id("module.app_tempo"), -5) != app::DispatchResult::Changed);
  CHECK(r.app->invoke(core::action_id("module.app_tempo"), 1) != app::DispatchResult::Changed);       // below the model's range: the Editor says no
  CHECK(r.app->invoke(core::action_id("module.app_tempo"), 80001) != app::DispatchResult::Changed);
  CHECK(r.project().tempo_x100() == 13300u);
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == before);
}

TEST_CASE("app host: with no project the tempo reads 0 and set_tempo says so") {
  Rig r;
  CHECK(r.mods->ui.launch_app(r.entry("Reader"), nullptr) == app::ModuleUi::LaunchResult::Ok);
  CHECK(r.host()->read->get_tempo() == 0u);
  r.mods->ui.app_exit();
  CHECK(r.mods->ui.launch_app(r.entry("Caps"), nullptr) == app::ModuleUi::LaunchResult::NeedsConfirm);
  CHECK(r.mods->ui.launch_app(r.entry("Caps"), nullptr) == app::ModuleUi::LaunchResult::Ok);
  CHECK(r.host()->read->get_tempo() == 0u);
  CHECK(r.host()->edit->set_tempo(12000) == O3M_EDIT_NO_PROJECT);
  r.pass(40);
  CHECK(r.project().tempo_x100() != 12000u);
}

TEST_CASE("app host: the launch context is zero from the app list, the selection from TOOLS, and copies no more than the app's struct has room for") {
  Rig r;
  CHECK(r.launch("Reader") == app::ModuleUi::LaunchResult::Ok);
  o3m_launch_ctx c;
  std::memset(&c, 0xAA, sizeof c);
  c.size = sizeof c;
  CHECK(r.host()->read->launch_ctx(&c) == 0u);
  CHECK((c.size == sizeof c && c.flags == 0u && c.pattern == 0u && c.track == 0u && c.instrument == 0u && c.block_start == 0u && c.block_length == 0u));
  r.mods->ui.app_exit();
  // opened from TOOLS (H3 will do it; the host takes the context today)
  const o3m_launch_ctx sel = {sizeof sel, O3M_LAUNCH_FROM_TOOLS, 3, 2, 5, 4, 8, 0, {0, 0, 0}};
  CHECK(r.launch("Reader", &sel) == app::ModuleUi::LaunchResult::Ok);
  std::memset(&c, 0xAA, sizeof c);
  c.size = sizeof c;
  CHECK(r.host()->read->launch_ctx(&c) == 1u);
  CHECK((c.size == sizeof c && c.flags == O3M_LAUNCH_FROM_TOOLS && c.pattern == 3u && c.track == 2u && c.instrument == 5u && c.block_start == 4u && c.block_length == 8u));
  // an app built against a shorter struct gets its prefix only, and says how much it got
  std::memset(&c, 0xAA, sizeof c);
  c.size = 12;
  CHECK(r.host()->read->launch_ctx(&c) == 1u);
  CHECK((c.size == 12u && c.flags == O3M_LAUNCH_FROM_TOOLS && c.pattern == 3u && c.track == 0xAAAAAAAAu));
  // a size below a word, or no struct at all: nothing is written
  std::memset(&c, 0xAA, sizeof c);
  c.size = 3;
  CHECK(r.host()->read->launch_ctx(&c) == 1u);
  CHECK((c.size == 3u && c.flags == 0xAAAAAAAAu));
  CHECK(r.host()->read->launch_ctx(nullptr) == 0u);
  // a context without the TOOLS flag is the list's: all zero
  r.mods->ui.app_exit();
  const o3m_launch_ctx junk = {sizeof junk, 0, 9, 9, 9, 9, 9, 9, {0, 0, 0}};
  CHECK(r.launch("Reader", &junk) == app::ModuleUi::LaunchResult::Ok);
  c.size = sizeof c;
  CHECK(r.host()->read->launch_ctx(&c) == 0u);
  CHECK((c.pattern == 0u && c.block_length == 0u));
  // the next launch from the list does not inherit the previous one's
  r.mods->ui.app_exit();
  CHECK(r.launch("Reader") == app::ModuleUi::LaunchResult::Ok);
  c.size = sizeof c;
  CHECK(r.host()->read->launch_ctx(&c) == 0u);
  CHECK((c.flags == 0u && c.pattern == 0u));
}

// ---- the confirm of an app that edits the project ---------------------------------------------------------------------------------------------------

TEST_CASE("app host: an app that edits the project is confirmed once per boot, by launching it twice in a row") {
  Rig r;
  using R = app::ModuleUi::LaunchResult;
  CHECK(r.launch("Caps") == R::NeedsConfirm);
  CHECK(!r.mods->ui.app_running());
  CHECK(r.mods->ui.last_status() == module::Status::Ok);
  CHECK(std::string(app::ModuleUi::launch_text(R::NeedsConfirm)).find("EDITS THE PROJECT") != std::string::npos);
  // any other launch in between forgets the question
  CHECK(r.launch("Bare") == R::Ok);
  r.mods->ui.app_exit();
  CHECK(r.launch("Caps") == R::NeedsConfirm);
  CHECK(r.launch("Caps") == R::Ok);
  CHECK(r.mods->ui.app_running());
  r.mods->ui.app_exit();
  // confirmed for the boot: it starts at once now, and after a rescan or a project change too
  CHECK(r.launch("Caps") == R::Ok);
  r.mods->ui.app_exit();
  r.mods->ui.reset();
  r.mods->ui.scan_apps();
  CHECK(r.launch("Caps") == R::Ok);
  r.mods->ui.app_exit();
  // an app that only reads, or asks for nothing, never asks
  CHECK(r.launch("Reader") == R::Ok);
  r.mods->ui.app_exit();
  CHECK(r.launch("Old") == R::Ok);
}

TEST_CASE("app host: F1 on MODULES says why a first launch of an editing app did not start, and the second F1 starts it") {
  Rig r;
  r.app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Modules));
  r.pass(1);
  NEED(r.app->state().screen == app::ScreenId::Modules);
  // the cursor on Caps (apps are sorted: Bare, Caps, Old, Reader)
  r.tap(Button::Down);
  CHECK(r.app->state().modules.apps == 1);
  const auto note = [&]() {
    const core::NotifyQueue& q = r.app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  };
  r.tap(Button::F1);
  CHECK(r.app->state().screen == app::ScreenId::Modules);
  CHECK(note() == "EDITS THE PROJECT (UNDOABLE): F1 AGAIN");
  CHECK(!r.mods->ui.app_running());
  CHECK(test::read_row(*r.app).label[0] == "LAUNCH?");  // OS-KEYROWDLG: the second F1 is marked, inverted; RESCAN keeps its label
  CHECK(test::read_row(*r.app).hot == 0x01u);
  CHECK(test::read_row(*r.app).label[1] == "RESCAN");
  r.tap(Button::Down);  // the cursor moves to another app: F1 would ask about that one, so the mark goes
  CHECK_FALSE(test::read_row(*r.app).marked);
  r.tap(Button::Up);
  CHECK(test::read_row(*r.app).label[0] == "LAUNCH?");
  r.tap(Button::F1);
  CHECK(r.app->state().screen == app::ScreenId::ModuleApp);
  CHECK(r.mods->ui.app_running());
  CHECK(std::strcmp(r.mods->ui.app_name(), "Caps") == 0);
  // the app's tempo request is applied from the page, with the app in view, as one undoable unit
  const std::uint32_t before = r.project().tempo_x100();
  CHECK(r.host()->edit->set_tempo(11111) == O3M_EDIT_OK);
  r.pass(40);
  CHECK(r.project().tempo_x100() == 11111u);
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == before);
}

TEST_CASE("app host: F2 RESCAN on MODULES says how many apps the card has (OS-QAFIX-RESCAN)") {
  Rig r;
  r.app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Modules));
  r.pass(1);
  NEED(r.app->state().screen == app::ScreenId::Modules);
  const auto note = [&]() {
    const core::NotifyQueue& q = r.app->state().notify;
    return q.empty() ? std::string() : std::string(q.at(q.size() - 1).text.c_str());
  };
  r.tap(Button::F2);
  CHECK(note() == "READ AGAIN: 4 APPS");  // Bare, Caps, Old, Reader
  fs::remove(r.card.root / "modules" / "apps" / "Old.tmod");
  r.tap(Button::F2);
  CHECK(note() == "READ AGAIN: 3 APPS");
}

// ---- the MIDI-in ring -------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("midi monitor: channel messages become whole events, running status, real-time bytes and SysEx handled, off until asked") {
  midi::MidiMonitor m;
  const auto feed = [&](unsigned port, std::initializer_list<std::uint8_t> bytes) {
    for (std::uint8_t b : bytes) {
      m.feed(port, b);
    }
  };
  midi::MidiMonitor::Event e;
  // off: nothing is kept, nothing counted
  feed(0, {0x90, 60, 100, 0xF0, 1, 0xF7});
  CHECK(!m.pop(e));
  CHECK((m.overflow() == 0u && m.sysex_dropped() == 0u));
  m.set_enabled(true);
  // a note on, running status for a second, a program change (one data byte), pressure, pitch bend, a controller on another channel
  feed(0, {0x90, 60, 100, 62, 90, 0xC3, 7, 0xD1, 33, 0xE0, 0x00, 0x40, 0xB5, 7, 127});
  const midi::MidiMonitor::Event want[] = {{0, 0x90, 60, 100}, {0, 0x90, 62, 90}, {0, 0xC3, 7, 0}, {0, 0xD1, 33, 0}, {0, 0xE0, 0, 0x40}, {0, 0xB5, 7, 127}};
  for (const auto& w : want) {
    NEED(m.pop(e));
    CHECK((e.port == w.port && e.status == w.status && e.data1 == w.data1 && e.data2 == w.data2));
  }
  CHECK(!m.pop(e));
  // running status survives a program change? (it does not repeat: a program change has its own status and continues as running status of ITS kind)
  feed(1, {0xC0, 1, 2, 3});
  NEED(m.pop(e));
  CHECK((e.port == 1u && e.status == 0xC0 && e.data1 == 1u));
  NEED(m.pop(e));
  CHECK((e.status == 0xC0 && e.data1 == 2u));
  NEED(m.pop(e));
  CHECK((e.status == 0xC0 && e.data1 == 3u));
  // a real-time byte inside a message changes nothing
  feed(0, {0x90, 0xF8, 64, 0xFE, 50});
  NEED(m.pop(e));
  CHECK((e.status == 0x90 && e.data1 == 64u && e.data2 == 50u));
  CHECK(!m.pop(e));
  // SysEx: counted once at its F0, its data bytes are not read as a message, and running status does not come back after it
  feed(0, {0x90, 60, 100});
  feed(0, {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7});
  feed(0, {61, 100});  // data bytes after the SysEx: no status, ignored
  feed(0, {0xF0, 0x43, 0x10, 0xF7});
  NEED(m.pop(e));
  CHECK((e.status == 0x90 && e.data1 == 60u));
  CHECK(!m.pop(e));
  CHECK(m.sysex_dropped() == 2u);
  // system common cancels running status too
  feed(0, {0x90, 60, 100, 0xF3, 5, 61, 100});
  NEED(m.pop(e));
  CHECK(!m.pop(e));
  // a data byte with no status is ignored; a status byte followed by another status starts over
  feed(0, {10, 20, 0x90, 0x80, 64, 0});
  NEED(m.pop(e));
  CHECK((e.status == 0x80 && e.data1 == 64u && e.data2 == 0u));
  // each port keeps its own running status, and an unknown port is port 0
  feed(0, {0x90, 60});
  feed(1, {0xB0, 1, 2});
  feed(0, {100});
  NEED(m.pop(e));
  CHECK((e.port == 1u && e.status == 0xB0));
  NEED(m.pop(e));
  CHECK((e.port == 0u && e.status == 0x90 && e.data2 == 100u));
  feed(7, {0xA0, 60, 10});
  NEED(m.pop(e));
  CHECK(e.port == 0u);
  // pack / unpack round trip
  const midi::MidiMonitor::Event ev{1, 0xE5, 0x7F, 0x12};
  const midi::MidiMonitor::Event back = midi::MidiMonitor::unpack(midi::MidiMonitor::pack(ev));
  CHECK((back.port == 1u && back.status == 0xE5 && back.data1 == 0x7F && back.data2 == 0x12));
}

TEST_CASE("midi monitor: a full ring drops the new message and counts it; clear empties the ring") {
  midi::MidiMonitor m;
  m.set_enabled(true);
  for (unsigned i = 0; i < midi::MidiMonitor::kRingSize + 10u; ++i) {
    m.feed(0, 0x90);
    m.feed(0, static_cast<std::uint8_t>(i & 0x7F));
    m.feed(0, 100);
  }
  CHECK(m.overflow() == 10u);
  midi::MidiMonitor::Event e;
  unsigned n = 0;
  while (m.pop(e)) {
    CHECK(e.data1 == (n & 0x7Fu));  // the first 64 are kept, in order; the ten after them were dropped
    ++n;
  }
  CHECK(n == midi::MidiMonitor::kRingSize);
  for (unsigned i = 0; i < 5; ++i) {
    m.feed(0, 0x90);
    m.feed(0, 1);
    m.feed(0, 2);
  }
  m.clear();
  CHECK(!m.pop(e));
}

TEST_CASE("midi monitor: the control-change object feeds it without disturbing MIDI Learn's rings") {
  midi::CcInput cc;
  cc.monitor().set_enabled(true);
  const std::uint8_t bytes[] = {0x91, 60, 100, 0xB1, 7, 99, 0xE1, 0, 64};
  for (std::uint8_t b : bytes) {
    midi::RtItem item{};
    item.byte = b;
    cc.feed(item);
  }
  midi::NoteMsg note;
  midi::CcMsg ctl;
  CHECK(cc.pop_note(note));
  CHECK(cc.pop(ctl));
  CHECK((ctl.cc == 7 && ctl.value == 99));
  midi::MidiMonitor::Event e;
  unsigned n = 0;
  while (cc.monitor().pop(e)) {
    ++n;
  }
  CHECK(n == 3u);  // the note, the controller and the pitch bend (which only the monitor sees)
}

TEST_CASE("app host: midi_read and midi_lost see only what arrives while the app runs, in order, oldest first") {
  Rig r;
  r.feed(0, {0x90, 60, 100});  // before the app: nobody asked
  CHECK(!r.cc.monitor().enabled());
  CHECK(r.launch("Reader") == app::ModuleUi::LaunchResult::Ok);
  CHECK(r.cc.monitor().enabled());
  const o3m_host* h = r.host();
  NEED(h != nullptr);
  CHECK(drain(h).empty());
  r.feed(0, {0x90, 60, 100, 0x80, 60, 0, 0xC2, 9});
  r.feed(1, {0xB0, 74, 12});
  r.feed(0, {0xF0, 1, 2, 0xF7});
  const std::vector<o3m_midi_event> got = drain(h);
  NEED(got.size() == 4u);
  CHECK((got[0].port == 0 && got[0].status == 0x90 && got[0].data1 == 60 && got[0].data2 == 100));
  CHECK((got[1].status == 0x80 && got[1].data2 == 0));
  CHECK((got[2].status == 0xC2 && got[2].data1 == 9 && got[2].data2 == 0));
  CHECK((got[3].port == 1 && got[3].status == 0xB0 && got[3].data1 == 74 && got[3].data2 == 12));
  std::uint32_t over = 99, sysex = 99;
  h->io->midi_lost(&over, &sysex);
  CHECK((over == 0u && sysex == 1u));
  h->io->midi_lost(nullptr, &sysex);  // either pointer may be NULL
  h->io->midi_lost(&over, nullptr);
  h->io->midi_lost(nullptr, nullptr);
  // max bounds the call, the rest stays for the next one
  r.feed(0, {0x90, 1, 1, 0x90, 2, 2, 0x90, 3, 3});
  o3m_midi_event two[2];
  CHECK(h->io->midi_read(two, 2) == 2u);
  CHECK((two[0].data1 == 1 && two[1].data1 == 2));
  CHECK(h->io->midi_read(two, 2) == 1u);
  CHECK(two[0].data1 == 3);
  CHECK(h->io->midi_read(two, 2) == 0u);
  CHECK(h->io->midi_read(two, 0) == 0u);
  CHECK(h->io->midi_read(nullptr, 4) == 0u);
  // overflow: the ring holds 64; the count is the messages that did not fit
  for (unsigned i = 0; i < 70; ++i) {
    r.feed(0, {0x90, static_cast<std::uint8_t>(i), 1});
  }
  h->io->midi_lost(&over, &sysex);
  CHECK(over == 6u);
  CHECK(drain(h, 1000).size() == 64u);  // a max above the ring is cut to the ring
  // the app closes: the ring is switched off, and the next launch starts from nothing and from zero counts
  r.mods->ui.app_exit();
  CHECK(!r.cc.monitor().enabled());
  r.feed(0, {0x90, 5, 5});
  CHECK(r.launch("Reader") == app::ModuleUi::LaunchResult::Ok);
  CHECK(r.cc.monitor().enabled());
  CHECK(drain(r.host()).empty());
  r.host()->io->midi_lost(&over, &sysex);
  CHECK((over == 0u && sysex == 0u));
  // a project change that clears the modules switches it off too
  r.mods->ui.reset();
  CHECK(!r.cc.monitor().enabled());
}

TEST_CASE("app host: an app that does not hold midi_in does not switch the ring on") {
  Rig r;
  CHECK(r.launch("Bare") == app::ModuleUi::LaunchResult::Ok);
  CHECK(!r.cc.monitor().enabled());
  r.mods->ui.app_exit();
  r.launch_confirmed("Caps");
  CHECK(r.cc.monitor().enabled());  // Caps holds midi_in
  r.mods->ui.app_exit();
  CHECK(!r.cc.monitor().enabled());
}

// ---- the exit chord -------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("app host: the exit chord F7 + F8 closes the page; the app is handed the F7 press only, never F8 and no release (o3module.h)") {
  Rig r;
  r.app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Modules));
  r.pass(1);
  r.tap(Button::F1);  // the first app of the list: Bare
  NEED(r.app->state().screen == app::ScreenId::ModuleApp);
  HState* s = r.state();
  NEED(s != nullptr);
  const std::uint32_t before = s->inputs;
  r.press(Button::F7);
  r.pass(1);
  CHECK(s->inputs == before + 1u);
  CHECK((s->last_event.kind == O3M_EV_BUTTON && s->last_event.id == O3M_BTN_F7 && s->last_event.value == 1));
  r.press(Button::F8);  // (OS-KEYROWDLG S5: F8 is the OS's BACK, the app is never handed it: it sees the F7 press only)
  r.pass(1);
  CHECK(s->inputs == before + 1u);
  CHECK((s->last_event.kind == O3M_EV_BUTTON && s->last_event.id == O3M_BTN_F7 && s->last_event.value == 1));
  r.release(Button::F7);
  r.release(Button::F8);
  r.pass(1);
  CHECK(r.app->state().screen == app::ScreenId::Modules);  // the page is closed
  CHECK(!r.mods->ui.app_running());
  CHECK(s->exits == 1u);
  CHECK(s->inputs == before + 1u);  // the releases never reached it: exit() had run
}

// ---- the review fold of OS-H1 (ADR 0014 amendment D, conditions C1..C5; review R1) -----------------------------------------------------------------

TEST_CASE("app host (C1): a table, and every entry that needs a capability bit, is NULL in the lent table without that bit") {
  Rig r(true);
  using R = app::ModuleUi::LaunchResult;
  struct Row {
    const char* stem;
    bool read, edit, midi, tap;  // what the capability word grants
  };
  const Row rows[] = {{"Bare", false, false, false, false},     {"OnlyMidi", false, false, true, false},      {"OnlyRead", true, false, false, false},
                      {"OnlyEdit", false, true, false, false}, {"TapLike", true, true, false, false},        {"Caps", true, true, true, false}, {"Reader", true, false, true, false},
                      {"Tapper", false, false, false, true}};
  for (const Row& row : rows) {
    CAPTURE(row.stem);
    r.launch_confirmed(row.stem);
    NEED(r.mods->ui.app_running());
    const o3m_host* h = r.host();
    NEED(h != nullptr);
    CHECK((h->read != nullptr) == row.read);
    CHECK((h->edit != nullptr) == row.edit);
    NEED(h->io != nullptr);  // the io table carries the clock (no bit): always lent to an app of ABI 1.3
    CHECK(h->io->now_ms != nullptr);
    CHECK((h->io->midi_read != nullptr) == row.midi);
    CHECK((h->io->midi_lost != nullptr) == row.midi);
    CHECK((h->io->tap_select != nullptr) == row.tap);  // ABI 1.4: the four tap entries come with the audio_tap bit, all or none
    CHECK((h->io->tap_latest != nullptr) == row.tap);
    CHECK((h->io->tap_stream != nullptr) == row.tap);
    CHECK((h->io->tap_levels != nullptr) == row.tap);
    if (row.read) {
      CHECK((h->read->get_tempo != nullptr && h->read->launch_ctx != nullptr));
    }
    if (row.edit) {
      CHECK(h->edit->set_tempo != nullptr);
    }
    r.mods->ui.app_exit();
  }
  CHECK(r.launch("Old") == R::Ok);  // an app of ABI 1.2: all three NULL
  CHECK((r.host()->read == nullptr && r.host()->edit == nullptr && r.host()->io == nullptr));
}

TEST_CASE("app host (C2): set_tempo queues at most one tempo per app frame, the last write wins, and every apply is one undo unit") {
  Rig r;
  r.launch_confirmed("Caps");
  const o3m_host* h = r.host();
  const std::uint32_t before = r.project().tempo_x100();
  const std::size_t depth = r.journal.depth();
  // frame 1: five writes (including one the range refuses, which does not replace the queued one) -> one value, one unit
  CHECK(h->edit->set_tempo(9000) == O3M_EDIT_OK);
  CHECK(h->edit->set_tempo(9100) == O3M_EDIT_OK);
  CHECK(h->edit->set_tempo(1) == O3M_EDIT_RANGE);
  CHECK(h->edit->set_tempo(9200) == O3M_EDIT_OK);
  CHECK(h->edit->set_tempo(9300) == O3M_EDIT_OK);
  CHECK(r.project().tempo_x100() == before);  // nothing applies inside the app's call
  r.pass(40);
  CHECK(r.project().tempo_x100() == 9300u);
  CHECK(r.journal.depth() == depth + 1u);
  // frame 2: two more writes -> a second unit; the pass without a write leaves none
  CHECK(h->edit->set_tempo(9400) == O3M_EDIT_OK);
  CHECK(h->edit->set_tempo(9500) == O3M_EDIT_OK);
  r.pass(40);
  r.pass(40);
  CHECK(r.project().tempo_x100() == 9500u);
  CHECK(r.journal.depth() == depth + 2u);
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == 9300u);  // one undo takes back one apply, not one call
  CHECK(r.editor->undo() == model::UndoResult::Ok);
  CHECK(r.project().tempo_x100() == before);
}

TEST_CASE("app host (C4): an app that asks for a capability this OS does not grant is refused at load, naming it, and never runs") {
  Rig r(true);
  using R = app::ModuleUi::LaunchResult;
  const R want[] = {R::NeedsMidiOut, R::NeedsMidiOut};  // (export_files is granted since ABI 1.7: its refusal is a file of an older minor that asks, a reserved field)
  unsigned i = 0;
  for (const NeedCase& c : kNeed) {
    CAPTURE(c.stem);
    for (unsigned again = 0; again < 2; ++again) {  // twice: a refusal is not a quarantine, so the second launch says the same
      CHECK(r.launch(c.stem) == want[i]);
      CHECK(!r.mods->ui.app_running());
      CHECK(r.host() == nullptr);
      CHECK(r.mods->ui.last_status() == module::Status::CapUnsupported);
      CHECK(!r.mods->set.quarantined(c.stem));
    }
    const char* text = app::ModuleUi::launch_text(want[i]);
    CHECK(std::string(text) == c.text);
    CHECK(std::strlen(text) <= core::kNotifyTextChars);  // fits the notification strip
    ++i;
  }
  CHECK(r.mods->set.find("NeedOut") < 0);  // nothing of it was kept
  // the other apps still start
  CHECK(r.launch("Bare") == R::Ok);
}

TEST_CASE("app host (C4): the capability values are the published ABI, frozen") {
  static_assert(O3M_CAP_MIDI_IN == 1u && O3M_CAP_READ_PROJECT == 2u && O3M_CAP_AUDIO_TAP == 4u && O3M_CAP_EDIT_PROJECT == 8u && O3M_CAP_EXPORT_FILES == 16u && O3M_CAP_MIDI_OUT == 32u,
                "the capability bit values are published ABI (ADR 0014 amendment D, C4)");
  static_assert(O3M_CAPS_ABI_1_3 == (1u | 2u | 8u), "an OS of ABI 1.3 grants midi_in, read_project and edit_project only");
  static_assert(O3M_CAPS_ABI_1_4 == (1u | 2u | 4u | 8u), "an OS of ABI 1.4 adds audio_tap (amendment E)");
  CHECK(O3M_CAPS_ABI_1_3 == 11u);
  CHECK(O3M_CAPS_ABI_1_4 == 15u);
  static_assert(O3M_CAPS_ABI_1_7 == (1u | 2u | 4u | 8u | 16u), "an OS of ABI 1.7 adds export_files (OS-H4, amendment A)");
  CHECK(O3M_CAPS_ABI_1_7 == 31u);
}

TEST_CASE("app host (C5): an app with the Tap Calc capability word (read_project + edit_project) is confirmed in two steps, and MODULES says so") {
  Rig r(true);
  using R = app::ModuleUi::LaunchResult;
  // "TapLike" carries exactly the word of tapcalc.tmod (tapcalc_test pins the file's word to the same constant)
  CHECK(r.launch("TapLike") == R::NeedsConfirm);
  CHECK(!r.mods->ui.app_running());
  CHECK(r.launch("TapLike") == R::Ok);
  CHECK((r.host()->read != nullptr && r.host()->edit != nullptr));
  r.mods->ui.app_exit();
  CHECK(r.launch("TapLike") == R::Ok);  // remembered for the boot
}

TEST_CASE("midi monitor (R1): a new enable never completes the half message or the running status of the last one") {
  midi::MidiMonitor m;
  const auto feed = [&](unsigned port, std::initializer_list<std::uint8_t> bytes) {
    for (std::uint8_t b : bytes) {
      m.feed(port, b);
    }
  };
  midi::MidiMonitor::Event e;
  // a half message (status and its first data byte) is pending when the app closes
  m.set_enabled(true);
  feed(0, {0x90, 60});
  m.set_enabled(false);
  feed(0, {100});  // arrives while off: dropped
  m.set_enabled(true);
  m.clear();
  feed(0, {100});  // the second data byte of the old message: with the old parser it would complete note on 60 / 100
  CHECK(!m.pop(e));
  feed(0, {0x90, 61, 90});
  NEED(m.pop(e));
  CHECK((e.status == 0x90 && e.data1 == 61u && e.data2 == 90u));
  // running status of the last app does not carry over either
  feed(1, {0xB0, 7, 100});
  NEED(m.pop(e));
  m.set_enabled(false);
  m.set_enabled(true);
  feed(1, {8, 101});
  CHECK(!m.pop(e));
  // an enable with the flag already set (a second app straight after) is an edge too
  feed(0, {0x92, 40});
  m.set_enabled(true);
  feed(0, {50});
  CHECK(!m.pop(e));
  CHECK(m.enabled());
  m.set_enabled(false);
  CHECK(!m.enabled());
}

// ---- OS-H2a: the audio tap (ADR 0014 amendment E, ABI 1.4) -------------------------------------------------------------------------------------------

namespace {

// What the render does for one block with the tap on: every sample of block n is `value`.
void write_block(audio::AppTap& tap, std::int16_t value) {
  alignas(8) std::int16_t block[audio::AppTap::kBlock];
  for (std::int16_t& s : block) {
    s = value;
  }
  tap.push(block);
}

}  // namespace

TEST_CASE("audio tap (H2a): the tap is off until the app selects a point, switched off when the app stops, and a bad point changes nothing") {
  Rig r(true);
  using R = app::ModuleUi::LaunchResult;
  using Src = audio::AppTap::Source;
  CHECK(r.launch("Tapper") == R::Ok);  // listening only: no confirm
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  CHECK((io->size == sizeof(o3m_io_api) && O3M_IO_API_VERSION == io->version));
  CHECK(r.app_tap->source() == Src::Off);
  CHECK(io->tap_select(3) == 1);
  CHECK(io->tap_select(0xFFFFFFFFu) == 1);
  CHECK(r.app_tap->source() == Src::Off);
  CHECK(io->tap_select(O3M_TAP_MASTER) == 0);
  CHECK(r.app_tap->source() == Src::Master);
  CHECK(io->tap_select(O3M_TAP_INPUT) == 0);
  CHECK(r.app_tap->source() == Src::Input);
  CHECK(io->tap_select(O3M_TAP_OFF) == 0);
  CHECK(r.app_tap->source() == Src::Off);
  CHECK(io->tap_select(O3M_TAP_MASTER) == 0);
  r.mods->ui.app_exit();
  CHECK(r.app_tap->source() == Src::Off);  // the render stops writing at its next block
  // and a second launch starts with it off again
  CHECK(r.launch("Tapper") == R::Ok);
  CHECK(r.app_tap->source() == Src::Off);
  r.mods->ui.app_exit();
}

TEST_CASE("audio tap (H2a): tap_latest gives the newest frames, zeros for what is older than the select, and refuses bad arguments") {
  Rig r(true);
  CHECK(r.launch("Tapper") == app::ModuleUi::LaunchResult::Ok);
  const o3m_io_api* io = r.host()->io;
  alignas(4) std::int16_t out[2048 + 4];
  CHECK(io->tap_latest(out, 256) == 0u);  // no point selected
  CHECK(!r.app_tap->samples());           // (and a read with no point does not start the sample feed)
  CHECK(io->tap_select(O3M_TAP_MASTER) == 0);
  CHECK(io->tap_latest(out, 256) == 0u);  // the app's first read of samples starts the feed (OS-BUG-TAPWRITE); nothing is stored before it
  CHECK(r.app_tap->samples());
  // blocks written before the select would be older; here the ring was empty: block n (1-based) holds n
  for (int n = 1; n <= 6; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(n));
  }
  // the first two blocks after a select may still be the old point's: 6 blocks written, so 4 are fresh (frames 128..383)
  CHECK(io->tap_latest(out, 256) == 256u);
  CHECK((out[0] == 3 && out[63] == 3 && out[64] == 4 && out[255] == 6));
  std::memset(out, 0x55, sizeof out);
  CHECK(io->tap_latest(out, 512) == 256u);  // only 256 fresh frames: zero in front, the real ones at the end
  CHECK((out[0] == 0 && out[255] == 0 && out[256] == 3 && out[511] == 6));
  // the cap
  CHECK(io->tap_latest(out, 2048) == 256u);
  CHECK(io->tap_latest(out, 2050) == 0u);
  CHECK(io->tap_latest(out, 255) == 0u);  // odd
  CHECK(io->tap_latest(out, 0) == 0u);
  CHECK(io->tap_latest(nullptr, 256) == 0u);
  CHECK(io->tap_latest(out + 1, 256) == 0u);  // not 4-byte aligned
  // plenty written: whole windows, the newest exactly
  for (int n = 7; n <= 80; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(n));
  }
  CHECK(io->tap_latest(out, 2048) == 2048u);
  CHECK((out[2047] == 80 && out[2047 - 63] == 80 && out[2047 - 64] == 79 && out[0] == 49));
  // a new select starts again: nothing before it is returned
  CHECK(io->tap_select(O3M_TAP_INPUT) == 0);
  CHECK(io->tap_latest(out, 256) == 0u);
  write_block(*r.app_tap, 81);
  write_block(*r.app_tap, 82);
  CHECK(io->tap_latest(out, 256) == 0u);  // two blocks may still be the old point's
  write_block(*r.app_tap, 83);
  CHECK(io->tap_latest(out, 256) == 64u);
  CHECK((out[191] == 0 && out[192] == 83 && out[255] == 83));
  r.mods->ui.app_exit();
  CHECK(r.host() == nullptr);
}

TEST_CASE("audio tap (H2a): tap_stream gives everything new once, oldest first, and says what it dropped when the reader was too slow") {
  Rig r(true);
  CHECK(r.launch("Tapper") == app::ModuleUi::LaunchResult::Ok);
  const o3m_io_api* io = r.host()->io;
  alignas(4) std::int16_t out[4032 + 4];
  std::uint32_t lost = 77;
  CHECK(io->tap_stream(out, 4032, &lost) == 0u);  // no point selected
  CHECK(lost == 0u);
  CHECK(io->tap_select(O3M_TAP_MASTER) == 0);
  CHECK(io->tap_stream(out, 4032, &lost) == 0u);
  for (int n = 1; n <= 6; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(n));
  }
  // frames 128.. of the select on: blocks 3..6
  CHECK(io->tap_stream(out, 4032, &lost) == 256u);
  CHECK(lost == 0u);
  CHECK((out[0] == 3 && out[255] == 6));
  CHECK(io->tap_stream(out, 4032, &lost) == 0u);  // nothing new
  // two blocks, read in two bites of one block: gapless
  write_block(*r.app_tap, 7);
  write_block(*r.app_tap, 8);
  CHECK(io->tap_stream(out, 64, &lost) == 64u);  // room for one block: the NEWEST (drop-oldest, as everywhere in the taps), the older one is counted
  CHECK((out[0] == 8 && lost == 64u));
  CHECK(io->tap_stream(out, 64, nullptr) == 0u);
  // a reader that was away for 100 blocks: the ring promises 63 of them, the rest is counted
  for (int n = 1; n <= 100; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(100 + n));
  }
  CHECK(io->tap_stream(out, 4032, &lost) == 4032u);
  CHECK(lost == 37u * 64u);
  CHECK((out[0] == 138 && out[4031] == 200));
  // a block published after the call is read by the next call, with no gap: the range is decided from one load of the head
  write_block(*r.app_tap, 1);
  CHECK(io->tap_stream(out, 4032, &lost) == 64u);
  CHECK(lost == 0u);
  write_block(*r.app_tap, 2);
  write_block(*r.app_tap, 3);
  CHECK(io->tap_stream(out, 4032, &lost) == 128u);
  CHECK((lost == 0u && out[0] == 2 && out[127] == 3));
  // exactly one block past what the ring promises: that one block is counted, the 63 behind it come in order
  for (int n = 1; n <= 64; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(n));
  }
  CHECK(io->tap_stream(out, 4032, &lost) == 4032u);
  CHECK((lost == 64u && out[0] == 2 && out[4031] == 64));
  // the most one call returns is 4032 frames, an odd room is rounded down, a bad pointer is refused
  for (int n = 1; n <= 5; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(n));
  }
  CHECK(io->tap_stream(out, 193, &lost) == 192u);
  CHECK(lost == 2u * 64u);
  CHECK(io->tap_stream(nullptr, 64, &lost) == 0u);
  CHECK(io->tap_stream(out + 1, 64, &lost) == 0u);
  CHECK(io->tap_stream(out, 1, &lost) == 0u);
  r.mods->ui.app_exit();
}

TEST_CASE("audio tap (H2a): tap_levels are the peak, the RMS and the clip blocks of everything since the last call, measured whether or not the samples are read") {
  Rig r(true);
  CHECK(r.launch("Tapper") == app::ModuleUi::LaunchResult::Ok);
  const o3m_io_api* io = r.host()->io;
  o3m_tap_levels lv{};
  lv.size = sizeof lv;
  io->tap_levels(&lv);
  CHECK((lv.frames == 0u && lv.peak == 0.0f && lv.rms == 0.0f));  // no point selected
  CHECK(io->tap_select(O3M_TAP_MASTER) == 0);
  CHECK(!r.app_tap->samples());  // an app that reads only the levels never makes the render store the samples
  for (int n = 1; n <= 2; ++n) {
    write_block(*r.app_tap, 0);  // the old point's two blocks: not counted
  }
  write_block(*r.app_tap, 16384);   // half scale
  write_block(*r.app_tap, -8192);   // quarter scale, negative
  lv.size = sizeof lv;
  io->tap_levels(&lv);
  CHECK(lv.size == sizeof lv);
  CHECK(lv.frames == 2u * 64u);
  CHECK((lv.lost_frames == 0u && lv.clip_blocks == 0u));
  CHECK(lv.peak == doctest::Approx(0.5f));
  // rms of {0.5 x 64, 0.25 x 64}: sqrt((0.25 + 0.0625) / 2)
  CHECK(lv.rms == doctest::Approx(std::sqrt((0.25f + 0.0625f) / 2.0f)).epsilon(1e-5));
  // since the last call only
  write_block(*r.app_tap, 32767);  // a clip
  write_block(*r.app_tap, -32768);
  write_block(*r.app_tap, 100);
  io->tap_levels(&lv);
  CHECK((lv.frames == 3u * 64u && lv.clip_blocks == 2u));
  CHECK(lv.peak == doctest::Approx(1.0f));
  io->tap_levels(&lv);
  CHECK((lv.frames == 0u && lv.peak == 0.0f && lv.rms == 0.0f && lv.clip_blocks == 0u));
  // too slow: 200 blocks, 63 kept
  for (int n = 0; n < 200; ++n) {
    write_block(*r.app_tap, 1000);
  }
  io->tap_levels(&lv);
  CHECK(lv.frames == audio::AppTap::kMaxFrames);
  CHECK(lv.lost_frames == 200u * 64u - audio::AppTap::kMaxFrames);
  // a smaller struct gets what fits
  write_block(*r.app_tap, 8192);
  struct Short {
    std::uint32_t size, frames;
  } small{8, 99};
  io->tap_levels(reinterpret_cast<o3m_tap_levels*>(&small));
  CHECK((small.size == 8u && small.frames == 64u));
  io->tap_levels(nullptr);  // no crash
  CHECK(!r.app_tap->samples());
  r.mods->ui.app_exit();
}

TEST_CASE("audio tap (OS-BUG-TAPWRITE): the render stores no samples until the app reads some; the first read (latest or stream) starts the feed two blocks on; a launch starts levels-only again") {
  Rig r(true);
  CHECK(r.app_tap->samples());  // the tap's own default
  CHECK(r.launch("Tapper") == app::ModuleUi::LaunchResult::Ok);
  const o3m_io_api* io = r.host()->io;
  alignas(4) std::int16_t out[256 + 4];
  CHECK(!r.app_tap->samples());  // an audio_tap app starts levels-only
  CHECK(io->tap_select(O3M_TAP_MASTER) == 0);
  CHECK(!r.app_tap->samples());  // a select alone does not start the feed
  for (int n = 1; n <= 6; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(100 + n));
  }
  // six blocks went by: measured, not stored (the ring is as it was built: zeros)
  o3m_tap_levels lv{};
  lv.size = sizeof lv;
  io->tap_levels(&lv);
  CHECK((lv.frames == 4u * 64u && lv.peak > 0.0f));  // (the first two blocks after the select are the old point's: not counted)
  std::memset(out, 0x55, sizeof out);
  CHECK(r.app_tap->read_latest(out, 256).whole);
  bool zeros = true;
  for (int i = 0; i < 256; ++i) {
    zeros = zeros && out[i] == 0;
  }
  CHECK(zeros);
  // the first read starts the feed and returns nothing; the real frames begin two blocks after it
  CHECK(io->tap_latest(out, 256) == 0u);
  CHECK(r.app_tap->samples());
  write_block(*r.app_tap, 7);
  write_block(*r.app_tap, 8);
  CHECK(io->tap_latest(out, 256) == 0u);
  write_block(*r.app_tap, 9);
  CHECK(io->tap_latest(out, 256) == 64u);
  CHECK((out[191] == 0 && out[192] == 9 && out[255] == 9));
  // a second read does not move the freshness point again
  write_block(*r.app_tap, 10);
  CHECK(io->tap_latest(out, 256) == 128u);
  CHECK((out[127] == 0 && out[128] == 9 && out[255] == 10));
  r.mods->ui.app_exit();
  CHECK(r.app_tap->samples());  // the tap is back at its default for whoever uses it next
  // a second launch is levels-only again, and tap_stream is the other way in: the first call starts the feed at "now", not at the select
  CHECK(r.launch("Tapper") == app::ModuleUi::LaunchResult::Ok);
  io = r.host()->io;
  CHECK(!r.app_tap->samples());
  CHECK(io->tap_select(O3M_TAP_MASTER) == 0);
  for (int n = 1; n <= 5; ++n) {
    write_block(*r.app_tap, static_cast<std::int16_t>(n));  // levels only: never delivered
  }
  alignas(4) std::int16_t big[1024 + 4];
  std::uint32_t lost = 77;
  CHECK(io->tap_stream(big, 1024, &lost) == 0u);
  CHECK((lost == 0u && r.app_tap->samples()));
  write_block(*r.app_tap, 6);
  write_block(*r.app_tap, 7);
  CHECK(io->tap_stream(big, 1024, &lost) == 0u);
  write_block(*r.app_tap, 8);
  write_block(*r.app_tap, 9);
  CHECK(io->tap_stream(big, 1024, &lost) == 128u);  // blocks 8 and 9: the feed began two blocks after the first call
  CHECK((lost == 0u && big[0] == 8 && big[127] == 9));
  r.mods->ui.app_exit();
}

TEST_CASE("audio tap (H2a): in a build with no tap an app that asks for it is refused naming the capability; the others are unaffected") {
  Rig r(true);
  using R = app::ModuleUi::LaunchResult;
  r.mods->ui.set_audio_tap(nullptr);
  CHECK(r.launch("Tapper") == R::NeedsAudioTap);
  CHECK(!r.mods->ui.app_running());
  CHECK(std::string(app::ModuleUi::launch_text(R::NeedsAudioTap)) == "NEEDS AUDIO TAP (NEWER OS)");
  CHECK(r.launch("Bare") == R::Ok);
  CHECK(r.host()->io->tap_select == nullptr);
  r.mods->ui.app_exit();
}

TEST_CASE("audio tap (H2a): an app of ABI 1.3 and the tap-less io members: a 1.3 app sees the larger io table and its old members work unchanged") {
  Rig r(true);
  using R = app::ModuleUi::LaunchResult;
  // Reader declares ABI 1.3: its io table is the 1.4 one (additive) with the tap entries NULL; the MIDI calls it asked for are there
  CHECK(r.launch("Reader") == R::Ok);
  const o3m_io_api* io = r.host()->io;
  NEED(io != nullptr);
  CHECK((io->now_ms != nullptr && io->midi_read != nullptr && io->midi_lost != nullptr));
  CHECK((io->tap_select == nullptr && io->tap_latest == nullptr && io->tap_stream == nullptr && io->tap_levels == nullptr));
  // the members of 1.3 sit where they did: the table's first words and the offsets the 1.3 SDK compiled in
  CHECK(offsetof(o3m_io_api, now_ms) == 2 * sizeof(std::uint32_t));
  CHECK(offsetof(o3m_io_api, midi_read) == offsetof(o3m_io_api, now_ms) + sizeof(void*));
  CHECK(offsetof(o3m_io_api, midi_lost) == offsetof(o3m_io_api, midi_read) + sizeof(void*));
  CHECK(offsetof(o3m_io_api, tap_select) == offsetof(o3m_io_api, midi_lost) + sizeof(void*));  // appended after them
  CHECK(offsetof(o3m_io_api, export_open) == offsetof(o3m_io_api, tap_levels) + sizeof(void*));  // OS-H4 (1.7): appended after the tap's
  CHECK(offsetof(o3m_io_api, get_transport) == offsetof(o3m_io_api, export_status) + sizeof(void*));  // OS-APPUX S3 (1.10): appended after the export members
  CHECK(sizeof(o3m_io_api) == offsetof(o3m_io_api, stop_note) + sizeof(void*));
  CHECK(offsetof(o3m_host, io) + sizeof(void*) == offsetof(o3m_host, get_clock));  // and nothing before get_clock moved (1.12 appended get_clock and track_level)
  r.mods->ui.app_exit();
}
