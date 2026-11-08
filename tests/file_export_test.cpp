// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WAV export on the App (OS-WAVEXP slice A; docs/specs/OS-WAVEXP.md sections 3.1 and 5): the File screen's EXPORT page and the modal progress overlay, against a fake offline renderer
// (the engine's side is audio_offline_test.cpp and seq_offline_test.cpp, the file's export_wav_test.cpp) and an in-memory card with the real path guard (support/mem_card.h). The claims:
//   * F7 on the PROJECTS page opens the EXPORT page, F7 there goes back, F6 there goes to the IMPORT page; the key labels are the page's;
//   * Up / Down choose SONG and PATTERN nn, the encoder changes nn over the patterns that exist, the page starts at the Pattern screen's pattern, and the card shows the name F1 will make;
//   * F1 starts: the transport is stopped first (the export starts when the sequencer's position says it has), the progress overlay is up, the strip says EXPORTING;
//   * the overlay captures every key but F8: the screen, the transport, the pads, the encoder do nothing under it;
//   * the end: SAVED EXPORT/<PROJECT>/<NAME>.WAV on the strip, the overlay gone, the file on the card, the next export is _0002;
//   * a cancel (F8) at several points, a failure of the render, a full card and a card that goes: no file under the final name and no temporary file, the strip says so, the engine is given back;
//   * the refusals: no card write layer, no export on the build, a card that may not be written, an empty song, no such pattern;
//   * OS-SONGSHARE S5: Song F6 TOOLS / EXPORT WAV exports the slots of the rectangle (the request names them and the tracks), the file is <project>_S<first>-<last>_0001.wav, a second one is _0002, the
//     whole-song export on the EXPORT page is unchanged.
#include <doctest/doctest.h>

#include "support/key_row.h"
#include "support/mem_card.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/app/app.h>
#include <o3os/app/export_source.h>
#include <o3os/app/save_service.h>
#include <o3os/app/tools_list.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/audio/offline.h>
#include <o3os/audio/play_position.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/fakes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::ScreenId;
using test::MemCard;

namespace {

alignas(8) std::uint8_t g_undo[64 * 1024];

// A renderer that makes a short known signal: `active` blocks of a sine, then a tail that halves every block. It can refuse, fail at a block, and it counts opens and closes.
class FakeRenderer final : public audio::OfflineRenderer {
 public:
  std::uint32_t settle = 2;
  std::uint32_t active = 30;
  std::uint32_t fail_block = ~0u;
  unsigned fail_from_open = 0;  // 0: any open() fails at fail_block; n: only the n-th (1-based)
  bool refuse = false;
  const char* refuse_why = "THE RECORDER IS ARMED";
  const char* stopped_text = "";  // OS-EXPNOTE: what the render says it stopped when it took the engine
  unsigned opens = 0;
  unsigned closes = 0;
  bool held = false;
  audio::OfflineRequest request{};
  std::vector<audio::OfflineRequest> requests;  // every open(), in order

  bool open(const audio::OfflineRequest& r) noexcept override {
    request = r;
    requests.push_back(r);
    failure_ = "";
    if (refuse) {
      failure_ = refuse_why;
      return false;
    }
    ++opens;
    held = true;
    waited_ = 0;
    block_ = 0;
    return true;
  }
  Step next(std::int16_t* out) noexcept override {
    if (!held) {
      return Step::Failed;
    }
    if (waited_ < settle) {
      ++waited_;
      return Step::Wait;
    }
    if (block_ == fail_block && (fail_from_open == 0u || opens == fail_from_open)) {
      failure_ = "BOOM";
      return Step::Failed;
    }
    std::int32_t amp = 12000;
    if (block_ >= active) {
      const std::uint32_t k = block_ - active + 1u;
      amp = k >= 15u ? 0 : (12000 >> k);
    }
    for (std::uint32_t i = 0; i < hal::audio::kBlock; ++i) {
      const double n = static_cast<double>(block_) * hal::audio::kBlock + i;
      out[2 * i] = static_cast<std::int16_t>(std::lround(amp * std::sin(2.0 * 3.14159265358979 * 440.0 * n / 44117.647)));
      out[2 * i + 1] = out[2 * i];
    }
    ++block_;
    return Step::Block;
  }
  bool in_tail() const noexcept override { return block_ >= active; }
  unsigned progress() const noexcept override { return block_ >= active ? 999u : block_ * 1000u / active; }
  void close() noexcept override {
    ++closes;
    held = false;
  }
  const char* failure() const noexcept override { return failure_; }
  const char* stopped() const noexcept override { return stopped_text; }

 private:
  const char* failure_ = "";
  std::uint32_t waited_ = 0;
  std::uint32_t block_ = 0;
};

// The transport: play() / stop() as the sequencer would answer them a few passes later, through the published position.
class FakeTransport final : public app::TransportSink {
 public:
  explicit FakeTransport(audio::PlayPosition& p) : pos(p) {}
  bool play(unsigned pattern) noexcept override {
    ++plays;
    pos.publish(true, pattern, 0, 12000);
    return true;
  }
  bool play_song(unsigned) noexcept override {
    ++plays;
    pos.publish(true, 1, 0, 12000, 0);
    return true;
  }
  bool stop() noexcept override {
    ++stops;
    stop_in = stop_delay;
    return true;
  }
  // Once a pass: the sequencer follows a stop a block or more later.
  void tick() noexcept {
    if (stop_in > 0 && --stop_in == 0) {
      pos.publish(false, 1, 0, 12000);
    }
  }
  audio::PlayPosition& pos;
  unsigned plays = 0;
  unsigned stops = 0;
  unsigned stop_delay = 3;
  unsigned stop_in = 0;
};

class FakeProject final : public app::ProjectSink {
 public:
  Start open(const char*, Format) noexcept override { return Start::NoDocument; }
  Start start_new() noexcept override { return Start::NoDocument; }
  bool busy() const noexcept override { return false; }
  const char* folder() const noexcept override { return ""; }
  bool loaded() const noexcept override { return true; }
  const char* detail() const noexcept override { return ""; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  MemCard card;
  app::SaveService saves;
  FakeRenderer renderer;
  FakeProject project;
  audio::PlayPosition position;
  FakeTransport transport{position};
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  alignas(4) std::uint8_t ring[app::ExportSource::kRingBytes];

  explicit Rig(bool writable = true, bool with_export = true) {
    card.gate_status = writable ? hal::storage::WriteStatus::Ok : hal::storage::WriteStatus::WriteProtected;
    model::ModelArena& arena = test::arena(1);
    test::fill_small(arena.project);
    journal.begin(g_undo, sizeof g_undo);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    app->attach_storage(&card);
    app->attach_project_sink(&project);
    app->attach_transport(&transport);
    app->attach_play_position(&position);
    saves.attach(card, card);
    if (with_export) {
      app::ExportSource::Wiring w;
      w.renderer = &renderer;
      w.ring = ring;
      w.ring_bytes = sizeof ring;
      saves.attach_export(w);
    }
    app->attach_saves(&saves);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    transport.tick();
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  void press(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void encoder(int delta) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(delta)));
    pass();
  }
  // OS-SHIFTENC: Shift held, the encoder turned, Shift let go.
  void shift_encoder(int delta) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
    pass();
    encoder(delta);
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
    pass();
  }
  void pad(unsigned index) {
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 100));
    input.push(core::make_pad_event(clock.now(), static_cast<std::uint8_t>(index), 0));
    pass();
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  const app::ViewState& view() const { return app->state(); }
  std::string message() const { return app->message(); }
  bool running() const { return app->session().jobs.occupied() != 0u || saves.busy(); }
  // Passes until the export ends (a bound of the test's own).
  void settle() {
    for (unsigned i = 0; i < 4000 && (running() || view().xport.running); ++i) {
      pass();
    }
    CHECK_FALSE(running());
  }
  // Passes until the bar has moved: the export is in its first pass.
  void until_progress(unsigned permille) {
    for (unsigned i = 0; i < 4000 && view().xport.permille < permille && view().xport.running; ++i) {
      pass();
    }
    CHECK(view().xport.permille >= permille);
  }
  // Opens the EXPORT page.
  void open_page() {
    go(ScreenId::File);
    press(Button::F7);
    NEED(view().xport.page);
  }
  // The export starts, and its overlay is up.
  void start_export() {
    open_page();
    press(Button::F1);
    NEED(view().overlay == app::OverlayId::ExportProgress);
  }
  bool no_leftovers() const {
    for (const auto& f : card.files) {
      if (f.first.find("_o3save.tmp") != std::string::npos) {
        return false;
      }
    }
    return card.violations == 0;
  }
  std::size_t wavs() const {
    std::size_t n = 0;
    for (const auto& f : card.files) {
      n += f.first.find(".wav") != std::string::npos ? 1u : 0u;
    }
    return n;
  }
};

constexpr const char* kSong1 = "Export/Untitled/Untitled_Song0001.wav";
constexpr const char* kPat1 = "Export/Untitled/Untitled_P1_0001.wav";

}  // namespace

TEST_CASE("export page: F7 on PROJECTS opens EXPORT, F7 there goes back, F6 there goes to IMPORT, and the labels are the page's") {
  Rig rig;
  rig.go(ScreenId::File);
  CHECK_FALSE(rig.view().xport.page);
  CHECK(std::strcmp(app::kFileSoftKeys[6], "EXPORT") == 0);
  rig.press(Button::F7);
  CHECK(rig.view().xport.page);
  CHECK_FALSE(rig.view().import.page);
  CHECK(std::strcmp(app::kFileExportSoftKeys[0], "EXPORT") == 0);  // F1
  CHECK(std::strcmp(app::kFileExportSoftKeys[3], "SAVE") == 0);
  CHECK(std::strcmp(app::kFileExportSoftKeys[4], "SAVE AS") == 0);
  CHECK(std::strcmp(app::kFileExportSoftKeys[5], "IMPORT") == 0);    // F6
  CHECK(std::strcmp(app::kFileExportSoftKeys[6], "PROJECTS") == 0);  // F7
  CHECK(app::kFileExportSoftKeys[1][0] == '\0');
  CHECK(app::kFileExportSoftKeys[2][0] == '\0');
  CHECK(app::kFileExportSoftKeys[7][0] == '\0');
  rig.press(Button::F7);
  CHECK_FALSE(rig.view().xport.page);
  rig.press(Button::F7);
  CHECK(rig.view().xport.page);
  rig.press(Button::F6);  // IMPORT
  CHECK_FALSE(rig.view().xport.page);
  CHECK(rig.view().import.page);
  rig.press(Button::F6);  // back to PROJECTS, as before
  CHECK_FALSE(rig.view().import.page);
  CHECK_FALSE(rig.view().xport.page);
  // F2, F3 and F8 do nothing on the page: no rescan, no NEW question
  rig.press(Button::F7);
  rig.press(Button::F3);
  rig.press(Button::F8);
  CHECK(rig.view().xport.page);
  CHECK_FALSE(rig.view().file.confirm_new);
  CHECK(rig.card.files.empty());
}

TEST_CASE("export page: Up / Down choose SONG and PATTERN (the stem rows below them are the next test's), the encoder changes the pattern over the patterns that exist, and the card shows the file F1 will make") {
  Rig rig;
  rig.open_page();
  CHECK(rig.view().xport.row == app::ExportView::Song);
  CHECK(rig.view().xport.pattern == 1u);  // the Pattern screen's pattern
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_SONG0001.WAV") == 0);
  rig.press(Button::Down);
  CHECK(rig.view().xport.row == app::ExportView::Pattern);
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_P1_0001.WAV") == 0);
  rig.encoder(1);  // fill_small has patterns 1, 2 and 7
  CHECK(rig.view().xport.pattern == 2u);
  rig.encoder(1);
  CHECK(rig.view().xport.pattern == 7u);
  rig.encoder(1);  // no further pattern
  CHECK(rig.view().xport.pattern == 7u);
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_P7_0001.WAV") == 0);
  rig.encoder(-2);
  CHECK(rig.view().xport.pattern == 1u);
  rig.press(Button::Up);
  CHECK(rig.view().xport.row == app::ExportView::Song);
  rig.encoder(1);  // the SONG row has no number
  CHECK(rig.view().xport.pattern == 1u);
  // the name moves on once the number is taken
  rig.card.put(kSong1, std::string("x"));
  rig.press(Button::Down);
  rig.press(Button::Up);
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_SONG0002.WAV") == 0);
}

TEST_CASE("OS-SHIFTENC: Shift + encoder on the export page's PATTERN row steps eight existing patterns a detent and stops at the first and the last") {
  Rig rig;
  rig.open_page();
  rig.press(Button::Down);
  NEED(rig.view().xport.row == app::ExportView::Pattern);
  CHECK(rig.view().xport.pattern == 1u);  // fill_small has patterns 1, 2 and 7: eight of them is past the last
  rig.shift_encoder(1);
  CHECK(rig.view().xport.pattern == 7u);
  rig.shift_encoder(1);
  CHECK(rig.view().xport.pattern == 7u);  // the top
  rig.shift_encoder(-1);
  CHECK(rig.view().xport.pattern == 1u);
  rig.shift_encoder(-5);
  CHECK(rig.view().xport.pattern == 1u);  // the bottom
  rig.encoder(1);                         // a plain detent is still one pattern
  CHECK(rig.view().xport.pattern == 2u);
  rig.press(Button::Up);
  rig.shift_encoder(1);  // the SONG row has no number
  CHECK(rig.view().xport.pattern == 2u);
}

TEST_CASE("export: F1 on SONG renders, the overlay shows the pass and the bar, the end says SAVED and the file is on the card; the next one is _0002") {
  Rig rig;
  rig.open_page();
  rig.press(Button::F1);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(rig.view().xport.running);
  CHECK(rig.renderer.request.what == audio::OfflineRequest::What::Song);
  rig.until_progress(10);
  CHECK(rig.view().xport.pass == 0u);
  CHECK(std::strcmp(rig.view().xport.file, "EXPORT/UNTITLED/UNTITLED_SONG0001.WAV") == 0);
  unsigned last = 0;
  bool second_pass = false;
  for (unsigned i = 0; i < 4000 && rig.view().xport.running; ++i) {
    rig.pass();
    CHECK(rig.view().xport.permille >= last);
    last = rig.view().xport.permille;
    second_pass = second_pass || rig.view().xport.pass == 1u;
  }
  CHECK(second_pass);
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.view().overlay == app::OverlayId::None);
  CHECK(rig.message() == "SAVED EXPORT/UNTITLED/UNTITLED_SONG0001.WAV");
  CHECK(rig.card.has(kSong1));
  CHECK(rig.no_leftovers());
  CHECK(rig.renderer.opens == 2u);  // the measuring pass and the writing pass
  CHECK(rig.renderer.closes >= 1u);
  CHECK_FALSE(rig.renderer.held);  // the engine was given back
  const MemCard::Bytes& wav = *rig.card.get(kSong1);
  CHECK(wav.size() > 44u);
  CHECK(std::memcmp(wav.data(), "RIFF", 4) == 0);
  CHECK(std::memcmp(wav.data() + 8, "WAVEfmt ", 8) == 0);
  // the page is as it was left, and the next export is _0002
  CHECK(rig.view().xport.page);
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_SONG0002.WAV") == 0);
  rig.press(Button::F1);
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_Song0002.wav"));
  CHECK(rig.card.has(kSong1));  // never replaced
}

TEST_CASE("export: F1 on PATTERN exports the pattern of the page, and export.pattern exports the pattern in view from anywhere") {
  Rig rig;
  rig.open_page();
  rig.press(Button::Down);
  rig.encoder(1);  // pattern 2
  rig.press(Button::F1);
  rig.until_progress(10);
  CHECK(rig.renderer.request.what == audio::OfflineRequest::What::Pattern);
  CHECK(rig.renderer.request.pattern == 2u);
  rig.settle();
  CHECK(rig.message() == "SAVED EXPORT/UNTITLED/UNTITLED_P2_0001.WAV");
  CHECK(rig.card.has("Export/Untitled/Untitled_P2_0001.wav"));
  // the Pattern screen's pattern, with no page: the action the TOOLS entry will be bound to
  rig.go(ScreenId::Pattern);
  CHECK(rig.app->invoke(core::action_id("export.pattern"), 0) != app::DispatchResult::Disabled);
  rig.pass();
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  rig.until_progress(10);
  CHECK(rig.renderer.request.what == audio::OfflineRequest::What::Pattern);
  CHECK(rig.renderer.request.pattern == 1u);
  rig.settle();
  CHECK(rig.card.has(kPat1));
  CHECK(rig.message() == "SAVED EXPORT/UNTITLED/UNTITLED_P1_0001.WAV");
}

TEST_CASE("export: Pattern F7 TOOLS pad 15 EXPORT PAT closes the menu and starts the export of the pattern in view; the APPS page holds no export row (OS-TOOLSDEDUP)") {
  Rig rig;  // (a bare App: no module system, so no tool apps)
  NEED(rig.app->model().modules == nullptr);
  rig.go(ScreenId::Pattern);
  rig.press(Button::F7);
  rig.pad(15);  // the verb menu's APPS > row (pad 16): the page lists only tool apps, none here
  CHECK(rig.view().overlay == app::OverlayId::VerbMenu);
  CHECK(app::tools_count(rig.app->model()) == 0u);
  rig.press(Button::F8);
  CHECK(rig.view().overlay == app::OverlayId::None);
  rig.press(Button::F7);
  rig.pad(14);  // EXPORT PAT (pad 15)
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);  // the menu closed, the progress overlay is up
  rig.until_progress(10);
  CHECK(rig.renderer.request.what == audio::OfflineRequest::What::Pattern);
  CHECK(rig.renderer.request.pattern == static_cast<std::uint16_t>(rig.view().edit.pattern));
  rig.settle();
  CHECK(rig.card.has(kPat1));
  CHECK(rig.message() == "SAVED EXPORT/UNTITLED/UNTITLED_P1_0001.WAV");
  CHECK(rig.view().overlay == app::OverlayId::None);
}

TEST_CASE("export: EXPORT PAT of a build with no export closes the menu and says so on the strip") {
  Rig rig(true, false);
  rig.go(ScreenId::Pattern);
  rig.press(Button::F7);
  NEED(rig.view().overlay == app::OverlayId::VerbMenu);
  rig.pad(14);  // EXPORT PAT
  CHECK(rig.view().overlay == app::OverlayId::None);
  CHECK(rig.message() == "NO EXPORT ON THIS BUILD");
  CHECK_FALSE(rig.view().xport.running);
}

TEST_CASE("export: starting stops the transport first, the export waits for the sequencer to stop, and the transport stays stopped") {
  Rig rig;
  rig.go(ScreenId::Pattern);
  rig.press(Button::Play);  // the transport plays
  CHECK(rig.transport.plays == 1u);
  rig.pass();
  CHECK(rig.view().playing);
  rig.go(ScreenId::File);
  rig.press(Button::F7);
  rig.press(Button::F1);
  CHECK(rig.transport.stops == 1u);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(rig.view().xport.armed);  // not yet: the sequencer follows the stop a few blocks later
  CHECK(rig.renderer.opens == 0u);
  CHECK(rig.saves.busy() == false);
  rig.settle();
  CHECK_FALSE(rig.view().xport.armed);
  CHECK(rig.card.has(kSong1));
  CHECK_FALSE(rig.view().playing);
  CHECK_FALSE(rig.view().play.playing);
  CHECK(rig.transport.plays == 1u);  // it was not started again
}

TEST_CASE("export: F8 while the export waits for the transport to stop cancels it before it starts; nothing is opened") {
  Rig rig;
  rig.go(ScreenId::Pattern);
  rig.press(Button::Play);
  rig.pass();
  rig.transport.stop_delay = 1000;
  rig.go(ScreenId::File);
  rig.press(Button::F7);
  rig.press(Button::F1);
  CHECK(rig.view().xport.armed);
  rig.press(Button::F8);
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.view().overlay == app::OverlayId::None);
  CHECK(rig.message() == "EXPORT CANCELLED");
  rig.passes(3);
  CHECK(rig.renderer.opens == 0u);
  CHECK(rig.card.files.empty());
}

TEST_CASE("export: the progress overlay captures every key but F8 - the screen, the transport, the pads, the encoder and the export itself do nothing under it") {
  Rig rig;
  rig.start_export();
  rig.until_progress(10);
  const unsigned plays = rig.transport.plays;
  const unsigned stops = rig.transport.stops;
  const std::uint8_t row = rig.view().xport.row;
  const Button keys[] = {Button::F1, Button::F2, Button::F3, Button::F4, Button::F5, Button::F6, Button::F7, Button::Play,  Button::Pattern, Button::Song,
                         Button::Up, Button::Down, Button::Left, Button::Right, Button::Rec, Button::Delete, Button::Master};
  for (const Button b : keys) {
    rig.press(b);
    CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
    CHECK(rig.view().screen == ScreenId::File);
    CHECK(rig.view().xport.running);
  }
  rig.encoder(3);
  rig.pad(5);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(rig.view().xport.page);
  CHECK(rig.view().xport.row == row);
  CHECK(rig.view().xport.pattern == 1u);
  CHECK(rig.transport.plays == plays);
  CHECK(rig.transport.stops == stops);
  CHECK(rig.renderer.opens == 1u);  // F1 did not start a second export (and SAVE did not start a save): one job
  CHECK(rig.app->session().jobs.occupied() == 1u);
  CHECK(rig.view().xport.running);
  // F8 is the way out
  rig.press(Button::F8);
  rig.settle();
  CHECK(rig.view().overlay == app::OverlayId::None);
}

TEST_CASE("export: the strip says EXPORTING in the place of the transport badge while it runs, and the overlay's F-key row is its own") {
  Rig rig;
  rig.start_export();
  CHECK(rig.view().xport.running);
  CHECK(rig.view().overlay_keys());  // the screen under it draws no key row: the overlay's F8 CANCEL
  const test::RowRead row = test::read_owner_row(rig.app->model(), rig.view());  // OS-KEYROWDLG: F8 CANCEL, every other key blank
  CHECK(row.marked);
  CHECK(row.label[7] == "CANCEL");
  for (unsigned i = 0; i < 7; ++i) {
    CHECK(row.label[i].empty());
  }
  rig.settle();
  CHECK_FALSE(rig.view().xport.running);
}

TEST_CASE("export: a cancel at several points leaves no file, no temporary file, the engine given back, and says so") {
  for (const unsigned permille : {0u, 40u, 120u, 260u, 480u, 520u, 700u, 900u}) {
    CAPTURE(permille);
    Rig rig;
    rig.card.put("Export/Untitled/other.wav", std::string("keep me"));
    rig.start_export();
    rig.until_progress(permille);
    rig.press(Button::F8);
    rig.settle();
    CHECK(rig.message() == "EXPORT CANCELLED");
    CHECK(rig.view().overlay == app::OverlayId::None);
    CHECK_FALSE(rig.view().xport.running);
    CHECK_FALSE(rig.card.has(kSong1));
    CHECK(rig.wavs() == 1u);  // only the user's own file
    CHECK(rig.card.same("Export/Untitled/other.wav", MemCard::Bytes{'k', 'e', 'e', 'p', ' ', 'm', 'e'}));
    CHECK(rig.no_leftovers());
    CHECK_FALSE(rig.renderer.held);
    CHECK(rig.renderer.closes >= (rig.renderer.opens != 0u ? 1u : 0u));
    CHECK(rig.card.removed().size() <= 1u);  // at most the job's own temporary file
    // and the page is usable: the same export starts again under the same name
    rig.press(Button::F1);
    rig.settle();
    CHECK(rig.card.has(kSong1));
  }
}

TEST_CASE("export: the notes the render stopped at its start are named on the strip after the result, once (OS-EXPNOTE)") {
  Rig rig;
  rig.renderer.stopped_text = "STOPPED MIDI CH10 C-4";
  rig.start_export();
  rig.settle();
  CHECK(rig.card.has(kSong1));
  CHECK(rig.message() == "SAVED EXPORT/UNTITLED/UNTITLED_SONG0001.WAV");  // the result first
  CHECK(rig.view().xport.stopped[0] == '\0');                              // said once
  CHECK(rig.app->state().notify.size() == 2u);
  CHECK(std::string(rig.app->state().notify.at(1).text.c_str()) == "STOPPED MIDI CH10 C-4");
}

TEST_CASE("export: nothing stopped, nothing said") {
  Rig rig;
  rig.start_export();
  rig.settle();
  CHECK(rig.app->state().notify.size() == 1u);
}

TEST_CASE("export: a render that fails, a refusal of the renderer, a full card and a card that goes leave no file and say why") {
  {  // the renderer refuses (the recorder is armed...)
    Rig rig;
    rig.renderer.refuse = true;
    rig.start_export();
    rig.settle();
    CHECK(rig.message() == "EXPORT FAILED: THE RECORDER IS ARMED");
    CHECK(rig.view().overlay == app::OverlayId::None);
    CHECK(rig.card.files.empty());
    CHECK(rig.no_leftovers());
    CHECK_FALSE(rig.renderer.held);
  }
  {  // the render fails in the middle of the second pass
    Rig rig;
    rig.renderer.fail_block = 20;
    rig.start_export();
    rig.settle();
    CHECK(rig.message() == "EXPORT FAILED: BOOM");
    CHECK_FALSE(rig.card.has(kSong1));
    CHECK(rig.no_leftovers());
    CHECK_FALSE(rig.renderer.held);
  }
  {  // the card is full
    Rig rig;
    rig.card.space_left = 4000;
    rig.start_export();
    rig.settle();
    CHECK(rig.message().find("EXPORT FAILED") == 0u);
    CHECK(rig.view().overlay == app::OverlayId::None);
    CHECK_FALSE(rig.card.has(kSong1));
    CHECK(rig.no_leftovers());
    CHECK_FALSE(rig.renderer.held);
  }
  {  // the card goes at a write
    Rig rig;
    rig.card.cut_at = 6;
    rig.start_export();
    rig.settle();
    CHECK(rig.message().find("EXPORT FAILED") == 0u);
    CHECK_FALSE(rig.card.has(kSong1));
    CHECK_FALSE(rig.renderer.held);
    CHECK(rig.view().overlay == app::OverlayId::None);
  }
}

TEST_CASE("export: the refusals say why on the strip and start nothing") {
  {  // a card that may not be written (write protected)
    Rig rig(false);
    rig.open_page();
    rig.press(Button::F1);
    CHECK(rig.message() == "CARD IS WRITE PROTECTED");
    CHECK(rig.view().overlay == app::OverlayId::None);
    CHECK_FALSE(rig.view().xport.running);
    CHECK(rig.card.files.empty());
  }
  {  // a build with no offline renderer
    Rig rig(true, false);
    rig.open_page();
    rig.press(Button::F1);
    CHECK(rig.message() == "NO EXPORT ON THIS BUILD");
    CHECK(rig.view().overlay == app::OverlayId::None);
  }
  {  // a song with no positions
    Rig rig;
    CHECK(test::arena(1).project.clear_order() == model::Status::Ok);
    rig.open_page();
    rig.press(Button::F1);
    CHECK(rig.message() == "THE SONG IS EMPTY");
    CHECK(rig.view().overlay == app::OverlayId::None);
  }
  {  // a pattern that the project lost between the page and F1
    Rig rig;
    rig.open_page();
    rig.press(Button::Down);
    rig.encoder(2);
    CHECK(rig.view().xport.pattern == 7u);
    CHECK(test::arena(1).project.delete_pattern(7) == model::Status::Ok);
    rig.press(Button::F1);
    CHECK(rig.message() == "NO SUCH PATTERN");
    CHECK(rig.view().overlay == app::OverlayId::None);
  }
  {  // while a save runs
    Rig rig;
    rig.go(ScreenId::File);
    rig.press(Button::F4);
    CHECK(rig.saves.busy());
    rig.press(Button::F7);
    rig.press(Button::F1);
    CHECK(rig.view().overlay == app::OverlayId::None);  // (the strip says "A SAVE IS ALREADY RUNNING" behind the save's own progress line)
    CHECK_FALSE(rig.view().xport.running);
    rig.settle();
  }
}

// ---- OS-WAVEXP slice B: stems and the selection ------------------------------------------------------------------------------------------------------

namespace {

model::Step note_at(std::uint8_t note, std::uint8_t instr) {
  model::Step s{};
  s.note = note;
  s.instr = instr;
  s.vol = model::kVolumeUnset;
  return s;
}

// Pattern 1 of fill_small has a note on track 1 (index 0) only. Notes on tracks 5 and 6 (an audio track each), a reverb send on the sample instrument, and the master's delay mute on (fill_small): the stems
// of pattern 1 are T01, T05, T06, REVERB and the MIX.
void make_stem_project() {
  model::Project& p = test::arena(1).project;
  CHECK(p.set_step(1, 4, 2, note_at(model::kNoteFirst + 30, 1)) == model::Status::Ok);
  CHECK(p.set_step(1, 5, 6, note_at(model::kNoteFirst + 40, 1)) == model::Status::Ok);
  model::Instrument in = *p.instrument(0);
  in.sampler.reverb_send = 35;
  CHECK(p.set_instrument(0, in) == model::Status::Ok);
  model::TrackSlot t = *p.track(3);
  t.flags = 0;  // (fill_small solos track 4 and mutes track 3: every track is heard here)
  CHECK(p.set_track(3, t) == model::Status::Ok);
  t = *p.track(2);
  t.flags = 0;
  CHECK(p.set_track(2, t) == model::Status::Ok);
}

constexpr const char* kSet1 = "Export/Untitled/Untitled_P1_S0001_";

}  // namespace

TEST_CASE("stems: the plan - the audio tracks with a note in the range, the returns that can sound, then the MIX; a MIDI track, an inactive one and an empty one are skipped") {
  model::Project& p = test::arena(2).project;
  p.reset();
  CHECK(p.set_active_tracks(12) == model::Status::Ok);
  CHECK(p.create_pattern(1, 16) == model::Status::Ok);
  CHECK(p.create_pattern(2, 16) == model::Status::Ok);
  CHECK(p.append_order(2) == model::Status::Ok);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  CHECK(p.set_instrument(0, in) == model::Status::Ok);  // no sends
  in.sampler.delay_send = 40;
  CHECK(p.set_instrument(1, in) == model::Status::Ok);  // a delay send
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 2;
  CHECK(p.set_instrument(2, in) == model::Status::Ok);
  model::TrackSlot t = *p.track(3);
  t.type = model::TrackType::Midi;
  CHECK(p.set_track(3, t) == model::Status::Ok);
  CHECK(p.set_master(model::MasterSettings{90, 80, 70, 60, 0}) == model::Status::Ok);
  const auto put = [&](unsigned id, unsigned track, unsigned row, unsigned instr, std::uint8_t note = model::kNoteFirst + 24) {
    CHECK(p.set_step(id, track, row, note_at(note, static_cast<std::uint8_t>(instr))) == model::Status::Ok);
  };
  put(1, 0, 0, 1);    // T01, no send
  put(1, 2, 5, 2);    // T03: the delay instrument
  put(1, 3, 1, 1);    // track 4 is a MIDI track: no stem
  put(1, 5, 2, 3);    // track 6 plays a MIDI instrument: no audio
  put(1, 13, 0, 1);   // track 14 is outside the 12 active ones
  put(1, 7, 4, 1, model::kNoteOff);  // a note off is not a note
  put(2, 9, 3, 1);    // T10, only in pattern 2

  app::StemPlan plan;
  audio::OfflineRequest base;
  base.what = audio::OfflineRequest::What::Pattern;
  base.pattern = 1;
  app::plan_stems(p, base, plan);
  const auto labels = [&] {
    std::string out;
    for (unsigned i = 0; i < plan.count; ++i) {
      char l[app::StemPlan::kLabelChars];
      app::stem_label(plan.part[i], l, sizeof l);
      out += (i ? " " : "");
      out += l;
    }
    return out;
  };
  CHECK(labels() == "T01 T03 DELAY MIX");  // the delay can sound (T03's instrument sends, the master delay is 60); no reverb: no instrument sends to it
  // the requests: a track stem is the track alone with its sends shut; a bus stem is the return alone; the MIX is the base
  CHECK(plan.part[0].rq.track_mask == 0x0001);
  CHECK(plan.part[0].rq.dry_sends);
  CHECK_FALSE(plan.part[0].rq.bus_only);
  CHECK(plan.part[1].rq.track_mask == 0x0004);
  CHECK(plan.part[2].rq.bus_only);
  CHECK(plan.part[2].rq.buses == 1u);
  CHECK(plan.part[2].rq.track_mask == audio::OfflineRequest::kAllTracks);
  CHECK_FALSE(plan.part[2].rq.dry_sends);
  CHECK(plan.part[3].rq.track_mask == audio::OfflineRequest::kAllTracks);
  CHECK_FALSE(plan.part[3].rq.masked());
  for (unsigned i = 0; i < plan.count; ++i) {
    CHECK(plan.part[i].rq.what == audio::OfflineRequest::What::Pattern);
    CHECK(plan.part[i].rq.pattern == 1u);
  }
  // the song: only the patterns it names (pattern 2 is the song; pattern 1 is not in it)
  base.what = audio::OfflineRequest::What::Song;
  app::plan_stems(p, base, plan);
  CHECK(labels() == "T10 MIX");
  CHECK(plan.part[0].rq.what == audio::OfflineRequest::What::Song);
  CHECK(p.append_order(1) == model::Status::Ok);
  app::plan_stems(p, base, plan);
  CHECK(labels() == "T01 T03 T10 DELAY MIX");
  // a muted track does not feed the buses (but its own stem stays: a stem is the track alone), a solo makes the others' sends silent too
  base.what = audio::OfflineRequest::What::Pattern;
  t = *p.track(2);
  t.flags = model::kTrackMute;
  CHECK(p.set_track(2, t) == model::Status::Ok);
  app::plan_stems(p, base, plan);
  CHECK(labels() == "T01 T03 MIX");
  t.flags = 0;
  CHECK(p.set_track(2, t) == model::Status::Ok);
  t = *p.track(0);
  t.flags = model::kTrackSolo;
  CHECK(p.set_track(0, t) == model::Status::Ok);
  app::plan_stems(p, base, plan);
  CHECK(labels() == "T01 T03 MIX");  // track 1 alone is heard, and its instrument does not send
  t.flags = 0;
  CHECK(p.set_track(0, t) == model::Status::Ok);
  // the master: a muted bus and a bus at 0 are skipped; a module instrument may send, and so does a note that names no instrument
  CHECK(p.set_master(model::MasterSettings{90, 80, 70, 60, model::kMasterDelayMute}) == model::Status::Ok);
  app::plan_stems(p, base, plan);
  CHECK(labels() == "T01 T03 MIX");
  CHECK(p.set_master(model::MasterSettings{90, 80, 70, 0, 0}) == model::Status::Ok);
  app::plan_stems(p, base, plan);
  CHECK(labels() == "T01 T03 MIX");
  CHECK(p.set_master(model::MasterSettings{90, 80, 70, 60, 0}) == model::Status::Ok);
  put(1, 1, 7, 0);  // T02: a note that names no instrument: the track's last one, unknown
  app::plan_stems(p, base, plan);
  CHECK(labels() == "T01 T02 T03 DELAY REVERB MIX");
  // 16 audio tracks with notes, every bus: 19 parts, the MIX last
  CHECK(p.set_active_tracks(16) == model::Status::Ok);
  t = *p.track(3);
  t.type = model::TrackType::Audio;
  CHECK(p.set_track(3, t) == model::Status::Ok);
  for (unsigned tr = 0; tr < 16u; ++tr) {
    put(1, tr, 9, 0);
  }
  app::plan_stems(p, base, plan);
  CHECK(plan.count == 19u);
  CHECK(plan.part[18].rq.masked() == false);
  char l[app::StemPlan::kLabelChars];
  app::stem_label(plan.part[15], l, sizeof l);
  CHECK(std::string(l) == "T16");
  app::stem_label(plan.part[17], l, sizeof l);
  CHECK(std::string(l) == "REVERB");
}

TEST_CASE("stems: the EXPORT page has SONG STEMS and PATT STEMS rows - Down reaches them, the encoder picks the pattern of PATT STEMS, and the card shows the set's name") {
  Rig rig;
  rig.open_page();
  for (const unsigned want : {1u, 2u, 3u}) {
    rig.press(Button::Down);
    CHECK(rig.view().xport.row == want);
  }
  CHECK(rig.view().xport.row == app::ExportView::PatternStems);
  rig.press(Button::Down);
  CHECK(rig.view().xport.row == app::ExportView::OgProject);  // OS-FILES-C C2c: the fifth row
  rig.press(Button::Down);
  CHECK(rig.view().xport.row == app::ExportView::OgProject);  // (nothing below it)
  rig.press(Button::Up);
  CHECK(rig.view().xport.row == app::ExportView::PatternStems);
  rig.encoder(1);
  CHECK(rig.view().xport.pattern == 2u);
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_P2_S0001_*.WAV") == 0);
  rig.press(Button::Up);
  CHECK(rig.view().xport.row == app::ExportView::SongStems);
  rig.encoder(1);
  CHECK(rig.view().xport.pattern == 2u);  // the song row has no number
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_SONG_S0001_*.WAV") == 0);
  // a set number that any file of a set uses is taken
  rig.card.put("Export/Untitled/Untitled_Song_S0001_T09.wav", std::string("x"));
  rig.press(Button::Up);
  rig.press(Button::Down);
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_SONG_S0002_*.WAV") == 0);
}

TEST_CASE("stems: F1 on PATT STEMS writes one file per part into Export/<project>/, each the same length, one pass each after the first, the bar counts them all, and the end says how many") {
  Rig rig;
  make_stem_project();
  rig.open_page();
  rig.press(Button::Down);
  rig.press(Button::Down);
  rig.press(Button::Down);
  rig.press(Button::F1);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(rig.view().xport.kind == app::ExportView::Stems);
  rig.pass();
  CHECK(rig.view().xport.stems == 5u);
  CHECK(std::strcmp(rig.view().xport.file, "EXPORT/UNTITLED/UNTITLED_P1_S0001_*.WAV") == 0);
  unsigned last = 0;
  unsigned seen_stem = 0;
  std::vector<unsigned> order;
  for (unsigned i = 0; i < 6000 && rig.view().xport.running; ++i) {
    rig.pass();
    CHECK(rig.view().xport.permille >= last);
    last = rig.view().xport.permille;
    if (rig.view().xport.stem != seen_stem) {
      seen_stem = rig.view().xport.stem;
      order.push_back(seen_stem);
    }
  }
  CHECK(last > 700u);  // (the bar reached the last file; the end closes the overlay before a pass shows 1000)
  CHECK(order == std::vector<unsigned>({1, 2, 3, 4, 5}));
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.view().overlay == app::OverlayId::None);
  const std::string msg = rig.message();
  CHECK(msg.rfind("SAVED 5 STEMS ", 0) == 0u);
  CHECK(msg.find("_S0001_*.WAV") != std::string::npos);
  for (const char* label : {"T01", "T05", "T06", "REVERB", "MIX"}) {
    CAPTURE(label);
    CHECK(rig.card.has((std::string(kSet1) + label + ".wav").c_str()));
  }
  CHECK(rig.wavs() == 5u);
  CHECK(rig.no_leftovers());
  CHECK_FALSE(rig.renderer.held);
  // the same length, a canonical header
  const std::size_t size = rig.card.get((std::string(kSet1) + "MIX.wav").c_str())->size();
  CHECK(size > 44u);
  for (const char* label : {"T01", "T05", "T06", "REVERB", "MIX"}) {
    const MemCard::Bytes& wav = *rig.card.get((std::string(kSet1) + label + ".wav").c_str());
    CHECK(wav.size() == size);
    CHECK(std::memcmp(wav.data(), "RIFF", 4) == 0);
  }
  // the passes: the MIX measured once (the first job), then a pass for each file: the five files make six passes
  NEED(rig.renderer.requests.size() == 6u);
  CHECK_FALSE(rig.renderer.requests[0].masked());  // the measuring pass is the plain MIX
  CHECK(rig.renderer.requests[1].track_mask == 0x0001);
  CHECK(rig.renderer.requests[2].track_mask == 0x0010);
  CHECK(rig.renderer.requests[3].track_mask == 0x0020);
  CHECK(rig.renderer.requests[4].bus_only);
  CHECK(rig.renderer.requests[4].buses == 2u);
  CHECK_FALSE(rig.renderer.requests[5].masked());
  for (const audio::OfflineRequest& r : rig.renderer.requests) {
    CHECK(r.what == audio::OfflineRequest::What::Pattern);
    CHECK(r.pattern == 1u);
  }
  CHECK(rig.renderer.closes == 5u);  // the engine is given back after every file
  // and a second set takes the next number
  rig.pass();
  CHECK(std::strcmp(rig.view().xport.preview, "EXPORT/UNTITLED/UNTITLED_P1_S0002_*.WAV") == 0);
}

// OS-BUGS3 (qa_wavexp, step "F1 on PATT STEMS"): the second F1 right after a finished set starts the next one (S0002), and the EXPORT page stays open when the File screen is left, so F7 on
// the way back is PROJECTS again: a HIL scenario that opened the page in its first part and pressed F7 once more before the stems rows read the overlay that never came as an export that did
// not start (its Down x3 moved in the project list and its F1 was an OPEN).
TEST_CASE("stems: the second F1 after a finished set starts the next set; the EXPORT page survives leaving the File screen, and F7 on the way back is PROJECTS") {
  Rig rig;
  make_stem_project();
  rig.open_page();
  rig.press(Button::Down);
  rig.press(Button::Down);
  rig.press(Button::Down);
  rig.press(Button::F1);
  NEED(rig.view().overlay == app::OverlayId::ExportProgress);
  for (unsigned i = 0; i < 6000 && rig.view().xport.running; ++i) {
    rig.pass();
  }
  NEED(!rig.view().xport.running);
  CHECK(rig.message().rfind("SAVED 5 STEMS ", 0) == 0u);
  rig.press(Button::F1);  // the same row again, at once
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(rig.view().xport.kind == app::ExportView::Stems);
  CHECK(std::strcmp(rig.view().xport.file, "EXPORT/UNTITLED/UNTITLED_P1_S0002_*.WAV") == 0);
  rig.press(Button::F8);  // cancel: nothing of it is left
  for (unsigned i = 0; i < 6000 && rig.view().xport.running; ++i) {
    rig.pass();
  }
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.no_leftovers());
  // the page is the File screen's own state: leave and come back, and it is still open; F7 there goes back to PROJECTS, where F1 starts no export
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  CHECK(rig.view().xport.page);
  rig.press(Button::F7);
  CHECK_FALSE(rig.view().xport.page);
  rig.press(Button::Down);
  rig.press(Button::F1);
  CHECK(rig.view().overlay != app::OverlayId::ExportProgress);
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.wavs() == 5u);  // (the first set only: no second set, nothing else written)
}

TEST_CASE("stems: a cancel during any file leaves no partial file of it, keeps the files before it, says how many are saved, and gives the engine back") {
  for (const unsigned file : {0u, 1u, 2u, 3u, 4u}) {
    for (const unsigned extra : {0u, 1u, 4u}) {
      CAPTURE(file);
      CAPTURE(extra);
      Rig rig;
      make_stem_project();
      rig.open_page();
      for (unsigned i = 0; i < 3u; ++i) {
        rig.press(Button::Down);
      }
      rig.press(Button::F1);
      for (unsigned i = 0; i < 6000 && rig.saves.stem_index() < file + 1u; ++i) {
        rig.pass();
      }
      NEED(rig.saves.stem_index() == file + 1u);
      rig.passes(extra);
      if (!rig.view().xport.running) {
        continue;  // (the file finished within the extra passes: nothing to cancel)
      }
      rig.press(Button::F8);
      rig.settle();
      if (rig.saves.stems_saved() == 5u) {
        continue;  // (the last file finished as the cancel arrived: the set is whole)
      }
      CHECK(rig.message() == "CANCELLED: " + std::to_string(rig.saves.stems_saved()) + " OF 5 STEMS SAVED");
      CHECK(rig.saves.stems_saved() <= file + 1u);
      CHECK(rig.saves.stems_saved() >= file);
      CHECK(rig.wavs() == rig.saves.stems_saved());  // the files that finished stay, nothing else
      CHECK(rig.no_leftovers());
      CHECK(rig.view().overlay == app::OverlayId::None);
      CHECK_FALSE(rig.view().xport.running);
      CHECK_FALSE(rig.renderer.held);
      CHECK_FALSE(rig.saves.busy());
      CHECK_FALSE(rig.saves.stems_active());
      // the page works again: a new set, numbered after the cancelled one's leftovers
      const std::size_t before = rig.wavs();
      rig.press(Button::F1);
      rig.settle();
      CHECK(rig.message().rfind("SAVED 5 STEMS ", 0) == 0u);
      CHECK(rig.wavs() == before + 5u);
    }
  }
}

TEST_CASE("stems: the card full in the middle of a file leaves the files before it, no partial one, and says how many were saved") {
  std::size_t one = 0;
  {
    Rig rig;
    make_stem_project();
    rig.open_page();
    for (unsigned i = 0; i < 3u; ++i) {
      rig.press(Button::Down);
    }
    rig.press(Button::F1);
    rig.settle();
    NEED(rig.wavs() == 5u);
    one = rig.card.get((std::string(kSet1) + "MIX.wav").c_str())->size();
  }
  for (const unsigned whole : {0u, 1u, 2u, 3u, 4u}) {
    CAPTURE(whole);
    Rig rig;
    make_stem_project();
    rig.card.space_left = one * whole + one / 2u;  // `whole` files fit, the next one runs out half way
    rig.open_page();
    for (unsigned i = 0; i < 3u; ++i) {
      rig.press(Button::Down);
    }
    rig.press(Button::F1);
    rig.settle();
    CHECK(rig.wavs() == whole);
    CHECK(rig.message().rfind("EXPORT FAILED", 0) == 0u);
    if (whole != 0u) {
      CHECK(rig.message().find("(" + std::to_string(whole) + " OF 5)") != std::string::npos);
    }
    CHECK(rig.no_leftovers());
    CHECK(rig.view().overlay == app::OverlayId::None);
    CHECK_FALSE(rig.view().xport.running);
    CHECK_FALSE(rig.renderer.held);
    CHECK_FALSE(rig.saves.busy());
  }
}

TEST_CASE("stems: a render that fails in a later file ends the set there, and the engine is given back") {
  Rig rig;
  make_stem_project();
  rig.renderer.fail_block = 20;
  rig.renderer.fail_from_open = 3;  // the third open() (the second file's write pass) fails at its block 20
  rig.open_page();
  for (unsigned i = 0; i < 3u; ++i) {
    rig.press(Button::Down);
  }
  rig.press(Button::F1);
  rig.settle();
  CHECK(rig.message().rfind("EXPORT FAILED: BOOM (1 OF 5)", 0) == 0u);
  CHECK(rig.wavs() == 1u);
  CHECK(rig.no_leftovers());
  CHECK_FALSE(rig.renderer.held);
}

namespace {

// Shift + Down / Right on the Pattern screen: a block from the cursor (Note column) over `right` more tracks. OS-AUDIT-56: Shift + Right goes
// a column at a time inside the first track (four presses to its last column), then takes the next track whole.
void select_block(Rig& rig, unsigned down, unsigned right) {
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
  rig.pass(1);
  for (unsigned i = 0; i < down; ++i) {
    rig.press(Button::Down);
  }
  for (unsigned i = 0; i < (right > 0 ? right + 4 : 0); ++i) {
    rig.press(Button::Right);
  }
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
  rig.pass(1);
}

}  // namespace

TEST_CASE("selection: EXPORT SELECTION is a TOOLS pad (14) that exports the block's tracks and rows") {
  Rig rig;
  make_stem_project();
  rig.go(ScreenId::Pattern);
  rig.press(Button::F7);
  rig.pad(13);  // EXPORT SEL (pad 14) with no block: refused on the strip, no export starts
  CHECK(rig.view().overlay == app::OverlayId::None);
  CHECK(rig.message() == "NO BLOCK TO EXPORT");
  CHECK_FALSE(rig.view().xport.running);
  rig.pass(2500);  // (the refusal's notice runs out, so the export's own SAVED notice is the one on the strip below)
  // a block of 3 rows and 2 tracks, from the cursor (row 0, track 1)
  select_block(rig, 2, 1);
  NEED(rig.view().edit.selection.active);
  const app::Selection::Block b = rig.view().edit.selection.block(rig.view().pattern.row(), rig.view().pattern.track());
  CHECK((b.row0 == 0 && b.row1 == 2 && b.track0 == 0 && b.track1 == 1));
  rig.press(Button::F7);
  NEED(rig.view().overlay == app::OverlayId::VerbMenu);
  CHECK(rig.app->model().modules == nullptr);
  rig.pad(13);  // EXPORT SEL (pad 14)
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(rig.view().xport.kind == app::ExportView::Selection);
  rig.until_progress(10);
  CHECK(rig.renderer.request.what == audio::OfflineRequest::What::Pattern);
  CHECK(rig.renderer.request.pattern == 1u);
  CHECK(rig.renderer.request.row_first == 0u);
  CHECK(rig.renderer.request.row_last == 2u);
  CHECK(rig.renderer.request.track_mask == 0x0003);
  CHECK(rig.renderer.request.trim_tail);
  CHECK_FALSE(rig.renderer.request.dry_sends);
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_SEL0001.wav"));
  CHECK(rig.message() == "SAVED EXPORT/UNTITLED/UNTITLED_SEL0001.WAV");
  // the block goes (Delete ends it): the pad refuses again
  (void)rig.app->invoke(core::action_id("select.clear"), 0);
  CHECK_FALSE(rig.view().edit.selection.active);
  rig.pass(4500);  // (the export's SAVED notice, a job notice of 4 s, runs out first)
  rig.press(Button::F7);
  rig.pad(13);
  CHECK(rig.message() == "NO BLOCK TO EXPORT");
  // and the next selection is numbered after it
  select_block(rig, 1, 0);
  NEED(rig.view().edit.selection.active);
  rig.press(Button::F7);
  rig.pad(13);  // EXPORT SEL
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_SEL0002.wav"));
  CHECK(rig.card.has("Export/Untitled/Untitled_SEL0001.wav"));
}

TEST_CASE("selection: a block over all 16 tracks asks for the mask 0xFFFF, a choice of tracks and not kAllTracks (a muted track in it sounds)") {
  Rig rig;
  CHECK(test::arena(1).project.set_active_tracks(16) == model::Status::Ok);
  rig.go(ScreenId::Pattern);
  select_block(rig, 1, 15);  // tracks 1 to 16
  NEED(rig.view().edit.selection.active);
  rig.press(Button::F7);
  rig.pad(13);  // EXPORT SEL
  rig.until_progress(10);
  CHECK(rig.renderer.request.track_mask == 0xFFFF);
  CHECK(rig.renderer.request.track_mask != audio::OfflineRequest::kAllTracks);
  CHECK(rig.renderer.request.masked());
  CHECK(rig.renderer.request.trim_tail);
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_SEL0001.wav"));
}

TEST_CASE("selection: the rows of a block that runs past the pattern are cut to the pattern's length") {
  Rig rig;
  rig.go(ScreenId::Pattern);
  select_block(rig, 20, 0);  // pattern 1 is 16 rows
  NEED(rig.view().edit.selection.active);
  rig.press(Button::F7);
  rig.pad(13);  // EXPORT SEL
  rig.until_progress(10);
  CHECK(rig.renderer.request.row_first == 0u);
  CHECK(rig.renderer.request.row_last == 15u);  // cut to the pattern
  rig.settle();
}

TEST_CASE("export: a learned MIDI CC does nothing while an export runs (a mute or a DRY MUTE under it would change the project and the ring between two files), and works again after") {
  Rig rig;
  make_stem_project();
  const auto cc = [](unsigned channel, unsigned number, unsigned value) { return static_cast<core::ActionArg>((channel << 16) | (number << 8) | value); };
  rig.press(Button::Params);  // the Instrument params page: the cursor is on a row that a controller can bind to
  (void)rig.app->invoke(core::action_id("param.group"), 1);
  rig.press(Button::Down);
  rig.press(Button::Down);
  rig.pass();
  (void)rig.app->invoke(core::action_id("learn.toggle"), 0);
  rig.pass();
  CHECK(rig.app->invoke(core::action_id("midi.cc"), cc(1, 74, 0)) != app::DispatchResult::Disabled);  // binds CC 74 to the parameter under the cursor
  (void)rig.app->invoke(core::action_id("learn.toggle"), 0);
  const auto sweep = [&] {  // (a pickup binding moves the parameter once the controller has reached it: some value of the sweep does)
    unsigned changed = 0;
    for (unsigned v = 0; v < 128u; v += 4) {
      changed += rig.app->invoke(core::action_id("midi.cc"), cc(1, 74, v)) == app::DispatchResult::Changed ? 1u : 0u;
    }
    return changed;
  };
  CHECK(sweep() > 0u);  // the control: it moves the parameter
  const std::uint32_t generation = test::arena(1).project.generation();
  rig.open_page();
  rig.press(Button::F1);
  NEED(rig.view().xport.running);
  rig.pass();
  CHECK(sweep() == 0u);
  CHECK(test::arena(1).project.generation() == generation);  // nothing was written under the export
  rig.settle();
  CHECK(sweep() > 0u);
}

// ---- OS-FILES-C C2c: the OG PROJECT row, the question, the overlay and the strip ------------------------------------------------------------------------------------------------------------------------------
// The files themselves are tests/og_export_test.cpp (T1, T4, T6, T8): these are the keys and the words.

namespace {

// The project of the OG tests: one pattern with one note and nothing the OG format lacks (a lossless export), or with `modules` module instruments (3 kinds of loss counted: modules).
void og_project(unsigned modules) {
  model::Project& p = test::arena(1).project;
  p.reset();
  NEED(p.create_pattern(1, 16) == model::Status::Ok);
  model::Step s = model::kEmptyStep;
  s.note = 60;
  s.instr = 1;
  NEED(p.set_step(1, 0, 0, s) == model::Status::Ok);
  for (unsigned i = 0; i < modules; ++i) {
    model::Instrument mod;
    model::init_instrument(mod, model::InstrumentKind::Module);
    std::strcpy(mod.name, "ACID");
    char name[8] = "mod0x";
    name[3] = static_cast<char>('0' + i);
    NEED(p.add_module(name, 5, 1, 0) == model::Status::Ok);
    mod.module_index = static_cast<std::uint8_t>(i);
    NEED(p.set_instrument(5 + i, mod) == model::Status::Ok);
  }
}

// The survey is made a slice a pass (App::poll_export): passes until it is done; the number of passes it took.
unsigned og_counted(Rig& rig) {
  unsigned passes = 0;
  while (!rig.view().xport.fresh && passes < 2000u) {
    rig.pass();
    ++passes;
  }
  CHECK(rig.view().xport.fresh);
  return passes;
}

void og_row(Rig& rig) {
  rig.open_page();
  for (unsigned i = 0; i < 4; ++i) {
    rig.press(Button::Down);
  }
  NEED(rig.view().xport.row == app::ExportView::OgProject);
  rig.pass();
  og_counted(rig);
}

bool og_folder_done(const Rig& rig, const char* folder) { return rig.card.has((std::string(folder) + "/project.mt").c_str()); }

}  // namespace

TEST_CASE("OG export UI: the fifth row says the folder, the files and the size; F1 with nothing to leave out starts at once, under the overlay, and ends on SAVED") {
  Rig rig;
  og_project(0);
  og_row(rig);
  CHECK(rig.view().xport.og_valid);
  CHECK(std::string(rig.view().xport.og.folder) == "Untitled OG");
  CHECK(rig.view().xport.og.files == 1u + 1u + 1u);  // pattern 1, patternsMetadata, project.mt
  CHECK(rig.view().xport.og.bytes > 9000u);
  CHECK_FALSE(rig.view().xport.og.drops.any_lost());
  rig.press(Button::F1);
  CHECK_FALSE(rig.view().file.og_ask);  // no question
  CHECK(rig.view().xport.running);
  CHECK(rig.view().xport.kind == app::ExportView::Og);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(std::string(rig.view().xport.file) == "EXPORT/UNTITLED OG");
  rig.settle();
  CHECK(og_folder_done(rig, "Export/Untitled OG"));
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.view().overlay == app::OverlayId::None);
  CHECK(rig.message() == "SAVED UNTITLED OG (3 FILES)");
  CHECK(rig.no_leftovers());
  CHECK(rig.renderer.opens == 0u);  // no render: the OG export is files, not audio
  // the next one goes to the next folder, and the row's card follows
  CHECK(std::string(rig.view().xport.og.folder) == "Untitled OG");
  rig.pass();
  og_counted(rig);
  CHECK(std::string(rig.view().xport.og.folder) == "Untitled OG 2");
}

TEST_CASE("OG export UI: with something left out F1 asks first, naming it; a second F1 goes on; the end names it again") {
  Rig rig;
  og_project(2);
  og_row(rig);
  CHECK(rig.view().xport.og.drops.module_instruments == 2u);
  rig.press(Button::F1);
  CHECK(rig.view().file.og_ask);
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.view().overlay == app::OverlayId::None);
  CHECK(rig.message() == "LOSES 2 MODULES: F1 AGAIN");
  CHECK(test::read_row(*rig.app).label[0] == "EXPORT?");  // OS-KEYROWDLG: the second F1 is marked
  CHECK(test::read_row(*rig.app).hot == 0x01u);
  CHECK(std::strcmp(rig.view().notify.head().source.c_str(), "file") == 0);  // the strip's label is the screen's ("file"), not a private name
  CHECK(rig.card.files.empty());
  rig.press(Button::F1);
  CHECK(rig.view().xport.running);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK_FALSE(rig.view().file.og_ask);
  rig.settle();
  CHECK(og_folder_done(rig, "Export/Untitled OG"));
  CHECK(rig.message() == "SAVED UNTITLED OG (3 FILES)");
  // the strip goes on to the second line when the first has run out
  for (unsigned i = 0; i < 400 && rig.message() == "SAVED UNTITLED OG (3 FILES)"; ++i) {
    rig.pass(16);
  }
  CHECK(rig.message() == "2 MODULES NOT KEPT");
  CHECK(std::strcmp(rig.view().notify.head().source.c_str(), "export") == 0);
}

TEST_CASE("OG export UI: any other key, a move, leaving the screen or 4 s take the question back; the strip loses its text at once") {
  Rig rig;
  og_project(1);
  og_row(rig);
  const auto asked = [&] {
    rig.press(Button::F1);
    NEED(rig.view().file.og_ask);
    NEED(rig.message() == "LOSES 1 MODULES: F1 AGAIN");
  };
  asked();
  rig.press(Button::F2);  // another F key of the screen
  CHECK_FALSE(rig.view().file.og_ask);
  CHECK(rig.message() == "");
  rig.press(Button::F1);  // asks again (it did not start)
  CHECK(rig.view().file.og_ask);
  CHECK_FALSE(rig.view().xport.running);
  rig.press(Button::Up);  // a move
  CHECK_FALSE(rig.view().file.og_ask);
  CHECK(rig.message() == "");
  rig.press(Button::Down);
  CHECK(rig.view().xport.row == app::ExportView::OgProject);
  og_counted(rig);  // (a move starts the count again)
  asked();
  rig.go(ScreenId::Pattern);  // leaving the screen
  CHECK_FALSE(rig.view().file.og_ask);
  CHECK(rig.message() == "");
  rig.go(ScreenId::File);
  og_counted(rig);
  asked();
  for (unsigned i = 0; i < 300; ++i) {  // 4.8 s: the text has run out, and F1 only asks again
    rig.pass(16);
  }
  CHECK(rig.message() == "");
  rig.press(Button::F1);
  CHECK(rig.view().file.og_ask);
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.card.files.empty());
}

TEST_CASE("OG export UI: the second F1 counts again - a project that changed under the question asks once more") {
  Rig rig;
  og_project(1);
  og_row(rig);
  rig.press(Button::F1);
  NEED(rig.view().file.og_ask);
  CHECK(rig.message() == "LOSES 1 MODULES: F1 AGAIN");
  // a second module instrument appears (a change the model makes under the strip): the count is no longer the one that was asked about
  model::Project& p = test::arena(1).project;
  model::Instrument mod;
  model::init_instrument(mod, model::InstrumentKind::Module);
  NEED(p.add_module("mod9x", 5, 1, 0) == model::Status::Ok);
  mod.module_index = 1;
  NEED(p.set_instrument(9, mod) == model::Status::Ok);
  rig.press(Button::F1);  // the survey no longer describes the project: F1 waits for the new count (and the question is not answered by it)
  CHECK_FALSE(rig.view().xport.running);
  CHECK_FALSE(rig.view().file.og_ask);
  CHECK(rig.message() == "COUNTING WHAT THE EXPORT LOSES: WAIT");
  og_counted(rig);
  CHECK(rig.view().xport.og.drops.module_instruments == 2u);
  rig.press(Button::F1);
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.view().file.og_ask);
  CHECK(rig.message() == "LOSES 2 MODULES: F1 AGAIN");
  rig.press(Button::F1);
  CHECK(rig.view().xport.running);
  rig.settle();
  CHECK(og_folder_done(rig, "Export/Untitled OG"));
}

TEST_CASE("OG export UI: the survey is counted a slice a pass (a pattern a pass), the page says COUNTING and F1 waits; the counts are the whole project's") {
  Rig rig;
  og_project(0);
  model::Project& p = test::arena(1).project;
  constexpr unsigned kPatterns = 40;
  for (unsigned id = 2; id <= kPatterns; ++id) {
    NEED(p.create_pattern(id, 16) == model::Status::Ok);
  }
  rig.open_page();
  for (unsigned i = 0; i < 4; ++i) {
    rig.press(Button::Down);
  }
  NEED(rig.view().xport.row == app::ExportView::OgProject);
  CHECK_FALSE(rig.view().xport.fresh);  // counting
  CHECK_FALSE(rig.view().xport.og_valid);
  rig.press(Button::F1);  // refused while counting: nothing asked, nothing started
  CHECK(rig.message() == "COUNTING WHAT THE EXPORT LOSES: WAIT");
  CHECK_FALSE(rig.view().file.og_ask);
  CHECK_FALSE(rig.view().xport.running);
  const unsigned passes = og_counted(rig);
  CHECK(passes >= kPatterns);       // one pattern a pass, not the whole survey in one
  CHECK(passes < kPatterns + 12u);  // and no pass is wasted: the folder, the plan, project.mt, the names
  CHECK(rig.view().xport.og_valid);
  CHECK(rig.view().xport.og.files == kPatterns + 2u);
  // the same counts as the whole survey at once
  app::OgSurvey whole;
  NEED(rig.saves.og_survey(p, nullptr, "", whole) == app::SaveService::Start::Started);
  CHECK(app::og_loss_signature(whole.drops) == app::og_loss_signature(rig.view().xport.og.drops));
  CHECK(whole.files == rig.view().xport.og.files);
  CHECK(whole.bytes == rig.view().xport.og.bytes);
  rig.press(Button::F1);  // counted: it starts
  CHECK(rig.view().xport.running);
  rig.settle();
  CHECK(og_folder_done(rig, "Export/Untitled OG"));
}

TEST_CASE("OG export UI: the folder name is looked up a few at a time while the page counts, and not again when nothing changed") {
  Rig rig;
  og_project(0);
  for (unsigned n = 1; n <= 12; ++n) {  // twelve exports of this name are on the card already
    rig.card.make_dir_now(n == 1 ? "Export/Untitled OG" : ("Export/Untitled OG " + std::to_string(n)).c_str());
  }
  rig.open_page();
  for (unsigned i = 0; i < 4; ++i) {
    rig.press(Button::Down);
  }
  std::size_t worst = 0;
  while (!rig.view().xport.fresh) {
    const std::size_t before = rig.card.exists_calls;
    rig.pass();
    worst = std::max(worst, rig.card.exists_calls - before);
  }
  CHECK(worst <= 4u);  // never the whole run of names in one pass
  CHECK(std::string(rig.view().xport.og.folder) == "Untitled OG 13");
  const std::size_t settled = rig.card.exists_calls;
  rig.passes(20);
  CHECK(rig.card.exists_calls == settled);  // a counted page asks the card for nothing more
}

TEST_CASE("OG export UI: leaving the row before the count ends drops it, and coming back counts again") {
  Rig rig;
  og_project(0);
  model::Project& p = test::arena(1).project;
  for (unsigned id = 2; id <= 30; ++id) {
    NEED(p.create_pattern(id, 16) == model::Status::Ok);
  }
  rig.open_page();
  for (unsigned i = 0; i < 4; ++i) {
    rig.press(Button::Down);
  }
  rig.passes(5);
  CHECK(rig.saves.og_survey_running());
  rig.press(Button::F1);  // refused while counting: the strip says so
  CHECK(rig.message() == "COUNTING WHAT THE EXPORT LOSES: WAIT");
  rig.press(Button::Up);  // another row: the count is dropped
  rig.pass();
  CHECK_FALSE(rig.saves.og_survey_running());
  CHECK(rig.message() == "");  // and COUNTING goes with it (it would otherwise stay until the message time ran out)
  rig.press(Button::Down);
  og_counted(rig);
  CHECK(rig.view().xport.og.files == 30u + 2u);
}

TEST_CASE("OG export UI: a project edit while the page counts starts the count again, and the counts are the edited project's") {
  Rig rig;
  og_project(0);
  model::Project& p = test::arena(1).project;
  for (unsigned id = 2; id <= 30; ++id) {
    NEED(p.create_pattern(id, 16) == model::Status::Ok);
  }
  rig.open_page();
  for (unsigned i = 0; i < 4; ++i) {
    rig.press(Button::Down);
  }
  rig.passes(10);
  NEED(rig.saves.og_survey_running());
  NEED(p.create_pattern(31, 16) == model::Status::Ok);
  og_counted(rig);
  CHECK(rig.view().xport.og.files == 31u + 2u);
  NEED(p.create_pattern(32, 16) == model::Status::Ok);  // after the count: the page notices and counts again
  rig.pass();
  CHECK_FALSE(rig.view().xport.fresh);
  og_counted(rig);
  CHECK(rig.view().xport.og.files == 32u + 2u);
}

TEST_CASE("OG export UI: the transport is stopped first and the set starts when the sequencer has stopped; F8 under the overlay stops it after the file in hand, with no project.mt") {
  Rig rig;
  og_project(0);
  rig.go(ScreenId::Pattern);
  rig.press(Button::Play);
  rig.pass();
  NEED(rig.view().playing);
  rig.go(ScreenId::File);
  og_row(rig);
  rig.press(Button::F1);
  CHECK(rig.transport.stops == 1u);
  CHECK(rig.view().xport.armed);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  CHECK(rig.saves.og_state() == app::SaveService::OgState::Idle);  // not started: the sequencer has not stopped
  // F8 while armed: not started at all
  rig.press(Button::F8);
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.message() == "EXPORT CANCELLED");
  CHECK(rig.card.files.empty());
  // again, and let it start; then F8 after the first file
  og_counted(rig);
  rig.press(Button::F1);
  CHECK(rig.view().xport.running);
  for (unsigned i = 0; i < 100 && rig.saves.og_state() != app::SaveService::OgState::Running; ++i) {
    rig.pass();
  }
  NEED(rig.saves.og_state() == app::SaveService::OgState::Running);
  CHECK_FALSE(rig.view().xport.armed);
  rig.press(Button::F8);
  rig.settle();
  CHECK(rig.saves.og_state() == app::SaveService::OgState::Cancelled);
  CHECK_FALSE(og_folder_done(rig, "Export/Untitled OG"));
  CHECK(rig.view().overlay == app::OverlayId::None);
  for (unsigned i = 0; i < 800 && rig.message() == "EXPORT CANCELLED"; ++i) {  // (the first cancel's line is still on the strip: the next one waits behind it)
    rig.pass();
  }
  CHECK_MESSAGE(rig.message().rfind("OG EXPORT CANCELLED: ", 0) == 0, rig.message());
  CHECK(rig.no_leftovers());
}

TEST_CASE("OG export UI (OS-USBDISK 4.3): an armed export that the share refuses at the stop shows its refusal ahead of the persistent CARD ON COMPUTER strip") {
  Rig rig;
  og_project(0);
  rig.go(ScreenId::Pattern);
  rig.press(Button::Play);
  rig.pass();
  NEED(rig.view().playing);
  rig.go(ScreenId::File);
  og_row(rig);
  rig.press(Button::F1);
  NEED(rig.view().xport.armed);  // the transport is told to stop; the set waits for the sequencer
  rig.card.gate_status = hal::storage::WriteStatus::Host;  // the card is shared while the export waits
  rig.app->set_card_on_computer(true);
  rig.app->disk_notice(app::App::kCardOnComputerStrip, 60000);
  for (unsigned i = 0; i < 8 && rig.view().xport.running; ++i) {
    rig.pass();
  }
  CHECK_FALSE(rig.view().xport.running);
  CHECK(rig.view().overlay == app::OverlayId::None);
  CHECK(rig.message() == "CARD ON COMPUTER - EJECT FIRST");  // asked: the head, not queued behind the strip
  CHECK(rig.card.files.empty());
  rig.app->set_card_on_computer(false);
}

TEST_CASE("OG export UI: the refusals say why and start nothing (no card write layer, a card that may not be written, a take in progress)") {
  {
    Rig rig(false);  // write protected: the card may not be written
    og_project(0);
    og_row(rig);
    rig.press(Button::F1);
    CHECK_FALSE(rig.view().xport.running);
    CHECK(rig.view().overlay == app::OverlayId::None);
    CHECK(rig.message() == "CARD IS WRITE PROTECTED");
    CHECK(rig.card.files.empty());
  }
  {
    Rig rig;
    rig.app->attach_saves(nullptr);
    og_project(0);
    rig.open_page();
    for (unsigned i = 0; i < 4; ++i) {
      rig.press(Button::Down);
    }
    rig.press(Button::F1);
    CHECK_FALSE(rig.view().xport.running);
    CHECK(rig.message() == "NO CARD WRITE LAYER");
  }
}

// ---- OS-SONGSHARE S5: EXPORT WAV of the Song's slots ---------------------------------------------------------------------------------------------------------------

namespace {

// Song F6 TOOLS, the OP turned to EXPORT WAV (three detents from TRANSPOSE, the dialog's OP of a fresh session; the dialog keeps its OP between openings), F1.
void tools_export_wav(Rig& rig) {
  rig.press(Button::F6);
  NEED(rig.view().overlay == app::OverlayId::SongTools);
  rig.encoder((3 + 4 - static_cast<int>(rig.view().tools.tool)) % 4);
  NEED(rig.view().tools.tool == app::SongTool::ExportWav);
  rig.press(Button::F1);
}

// Shift held for a key: the Song screen's range.
void shift_press(Rig& rig, Button b, unsigned n = 1) {
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
  rig.pass(1);
  for (unsigned i = 0; i < n; ++i) {
    rig.press(b);
  }
  rig.input.push(core::make_button_event(rig.clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
  rig.pass(1);
}

}  // namespace

TEST_CASE("slots export: Song F6 TOOLS / EXPORT WAV exports the cursor's slot with every track, then a range of slots, each to its own file; the whole-song export is untouched") {
  Rig rig;
  rig.go(ScreenId::Song);
  // no range: the cursor's slot (slot 0), every track, the project's own mixer
  tools_export_wav(rig);
  CHECK(rig.view().overlay == app::OverlayId::ExportProgress);
  rig.until_progress(10);
  CHECK(rig.renderer.request.what == audio::OfflineRequest::What::Song);
  CHECK(rig.renderer.request.order_first == 0u);
  CHECK(rig.renderer.request.order_last == 0u);
  CHECK(rig.renderer.request.track_mask == audio::OfflineRequest::kAllTracks);
  CHECK_FALSE(rig.renderer.request.trim_tail);  // the tail is kept (D4)
  CHECK(std::strcmp(rig.view().xport.file, "EXPORT/UNTITLED/UNTITLED_S0-0_0001.WAV") == 0);
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_S0-0_0001.wav"));
  CHECK(rig.message() == "SAVED EXPORT/UNTITLED/UNTITLED_S0-0_0001.WAV");
  CHECK(rig.no_leftovers());
  CHECK(rig.view().overlay == app::OverlayId::None);
  // a range of slots 1 to 3 (Shift + Down twice from slot 1), on PAT: every track; the same name for another range is another file
  rig.press(Button::Down);
  shift_press(rig, Button::Down, 2);
  tools_export_wav(rig);
  rig.until_progress(10);
  CHECK(rig.renderer.request.order_first == 1u);
  CHECK(rig.renderer.request.order_last == 3u);
  CHECK(rig.renderer.request.track_mask == audio::OfflineRequest::kAllTracks);
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_S1-3_0001.wav"));
  tools_export_wav(rig);  // the dialog keeps its OP; the range is still there
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_S1-3_0002.wav"));  // never over the first
  CHECK(rig.card.has("Export/Untitled/Untitled_S1-3_0001.wav"));
  // the EXPORT page's SONG row is the whole song, with the range left alone, and its name is the old one
  rig.open_page();
  rig.press(Button::Up);
  rig.press(Button::Up);
  CHECK(rig.view().xport.row == app::ExportView::Song);
  rig.press(Button::F1);
  rig.until_progress(10);
  CHECK(rig.renderer.request.what == audio::OfflineRequest::What::Song);
  CHECK(rig.renderer.request.order_first == 0u);
  CHECK(rig.renderer.request.order_last == audio::OfflineRequest::kToEnd);
  CHECK(rig.renderer.request.track_mask == audio::OfflineRequest::kAllTracks);
  rig.settle();
  CHECK(rig.card.has(kSong1));
}

TEST_CASE("slots export: in the map, the rectangle's tracks are the request's track_mask, and nothing selected there is every track") {
  Rig rig;
  rig.go(ScreenId::Song);
  rig.press(Button::Down);
  rig.press(Button::Right);  // into the map, track 1
  rig.press(Button::Right);  // track 2
  tools_export_wav(rig);     // nothing selected: the cursor's cell is no choice of tracks
  rig.until_progress(10);
  CHECK(rig.renderer.request.order_first == 1u);
  CHECK(rig.renderer.request.order_last == 1u);
  CHECK(rig.renderer.request.track_mask == audio::OfflineRequest::kAllTracks);
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_S1-1_0001.wav"));
  // tracks 2 and 3 (Shift + Right) of slots 1 and 2 (Shift + Down): a mask of those two tracks
  shift_press(rig, Button::Right);
  shift_press(rig, Button::Down);
  tools_export_wav(rig);
  rig.until_progress(10);
  CHECK(rig.renderer.request.order_first == 1u);
  CHECK(rig.renderer.request.order_last == 2u);
  CHECK(rig.renderer.request.track_mask == 0x0006);
  CHECK_FALSE(rig.renderer.request.dry_sends);
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_S1-2_0001.wav"));
}

TEST_CASE("slots export: a range over all 16 tracks is a choice of tracks (mask 0xFFFF), not 'every track with the project's mixer', so a muted track in it sounds (D8)") {
  Rig rig;
  CHECK(test::arena(1).project.set_active_tracks(16) == model::Status::Ok);
  rig.go(ScreenId::Song);
  rig.press(Button::Right);  // into the map, track 1
  shift_press(rig, Button::Right, 15);  // tracks 1 to 16
  tools_export_wav(rig);
  rig.until_progress(10);
  CHECK(rig.renderer.request.track_mask == 0xFFFF);
  CHECK(rig.renderer.request.track_mask != audio::OfflineRequest::kAllTracks);
  CHECK(rig.renderer.request.masked());
  rig.settle();
  CHECK(rig.card.has("Export/Untitled/Untitled_S0-0_0001.wav"));
}

TEST_CASE("slots export: a rectangle that starts before track 0 shifts nothing out of range (UBSan), and one with no track in 0..15 starts nothing") {
  // The map's rectangle never starts below track 0; the dialog's copy is set by hand to prove that the mask loop clamps its start.
  const auto open_export_wav = [](Rig& rig, int t0, int t1) {
    rig.go(ScreenId::Song);
    rig.press(Button::F6);
    NEED(rig.view().overlay == app::OverlayId::SongTools);
    rig.encoder((3 + 4 - static_cast<int>(rig.view().tools.tool)) % 4);
    NEED(rig.view().tools.tool == app::SongTool::ExportWav);
    app::ViewState& v = const_cast<app::ViewState&>(rig.app->state());
    v.tools.whole = false;
    v.tools.rect.t0 = t0;
    v.tools.rect.t1 = t1;
  };
  {
    Rig rig;
    open_export_wav(rig, -3, 1);  // tracks -3..1: the ones in range are 0 and 1
    rig.press(Button::F1);
    rig.until_progress(10);
    CHECK(rig.renderer.request.track_mask == 0x0003);
    rig.settle();
  }
  {
    Rig rig;
    open_export_wav(rig, -5, -1);  // tracks -5..-1: none in range, so not 'every track' (mask 0 is kAllTracks)
    rig.press(Button::F1);
    CHECK_FALSE(rig.view().xport.running);
    CHECK(rig.view().overlay == app::OverlayId::None);
    CHECK(rig.message() == "NO TRACK TO EXPORT");
    CHECK(rig.card.files.empty());
  }
}

TEST_CASE("slots export: the file name has the slot numbers unpadded, one to three digits each, the project part cut so that the name fits 31 characters, and a number that is taken is skipped") {
  Rig rig;
  const auto name = [&](unsigned first, unsigned last, const char* folder = "") {
    app::ExportRequest er;
    er.what.what = audio::OfflineRequest::What::Song;
    er.what.order_first = static_cast<std::uint16_t>(first);
    er.what.order_last = static_cast<std::uint16_t>(last);
    er.loaded_folder = folder;
    char path[hal::storage::save::kPathCap] = {};
    CHECK(rig.saves.export_name(er, path, sizeof path));
    return std::string(path);
  };
  CHECK(name(0, 0) == "Export/Untitled/Untitled_S0-0_0001.wav");
  CHECK(name(7, 12) == "Export/Untitled/Untitled_S7-12_0001.wav");
  CHECK(name(12, 255) == "Export/Untitled/Untitled_S12-255_0001.wav");
  CHECK(name(100, 200) == "Export/Untitled/Untitled_S100-200_0001.wav");
  // a project name of 31 characters leaves room for "_S100-255_0001" (14): its tail is cut
  const std::string longest = name(100, 255, "Projects/ABCDEFGHIJKLMNOPQRSTUVWXYZabcde");
  const std::string file = longest.substr(longest.rfind('/') + 1);
  CHECK(file.size() - 4u <= 31u);  // the stem, without ".wav"
  CHECK(file.find("_S100-255_0001.wav") == file.size() - std::strlen("_S100-255_0001.wav"));
  CHECK(file.rfind("ABCDEFGHIJKLM", 0) == 0u);
  // the pattern and song names are as they were
  app::ExportRequest er;
  er.what.what = audio::OfflineRequest::What::Song;
  char path[hal::storage::save::kPathCap] = {};
  CHECK(rig.saves.export_name(er, path, sizeof path));
  CHECK(std::string(path) == kSong1);
}
