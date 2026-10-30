// SPDX-License-Identifier: GPL-3.0-or-later
//
// The saves on the App (OS-007n): File's F4 SAVE and F5 SAVE AS write the native project through the card's save layer (a job of the main loop, a bounded amount
// a pass, the strip says how it went), into Projects/<name>/project.o3pj, byte-identical to what the native writer makes, never over another file; the refusal on a card
// on a card that may not be written; Sample edit's Shift + F6 writes a WAV of the instrument's audio; the device settings save themselves, quietly, after a change has stayed
// still, and load back at boot. (The job's own rules are atomic_save_test.cpp and save_fatfs_test.cpp, the guard's save_guard_test.cpp.)
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/key_row.h"
#include "support/modimp_fixture.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/app.h>
#include <o3os/app/module_import.h>
#include <o3os/app/native_load.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/audio/voices.h>
#include <o3os/app/screens/scene.h>
#include <o3os/core/input_event.h>
#include <o3os/eve/display_list_builder.h>
#include <o3os/formats/settings.h>
#include <o3os/formats/wav_sample.h>
#include <o3os/hal/audio.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/platform/mk66/disk_share.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::ScreenId;
using test::MemCard;
using Bytes = MemCard::Bytes;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

class FakeProject final : public app::ProjectSink {
 public:
  std::string last_folder;
  bool is_busy = false;
  bool ok = true;  // the last load worked
  unsigned opens = 0;
  Start open_result = Start::NoDocument;
  Start open(const char* folder, Format) noexcept override {
    ++opens;
    if (open_result == Start::Started) {
      last_folder = folder;
      is_busy = true;
    }
    return open_result;
  }
  unsigned news = 0;
  Start new_result = Start::Started;
  Start start_new() noexcept override {
    ++news;
    if (new_result == Start::Started) {
      last_folder.clear();
      is_busy = true;
    }
    return new_result;
  }
  unsigned keeps = 0;  // OS-NEWKEEP: NEW > KEEP INSTRUMENTS
  Start start_new_keeping_instruments() noexcept override {
    ++keeps;
    if (new_result == Start::Started) {
      last_folder.clear();
      is_busy = true;
    }
    return new_result;
  }
  bool busy() const noexcept override { return is_busy; }
  const char* folder() const noexcept override { return last_folder.c_str(); }
  bool loaded() const noexcept override { return ok; }
  const char* detail() const noexcept override { return ""; }
};

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
  FakeProject project;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(40000);
  app::SampleBank bank;

  explicit Rig(bool writable = true) {
    card.gate_status = writable ? hal::storage::WriteStatus::Ok : hal::storage::WriteStatus::WriteProtected;
    model::ModelArena& arena = test::arena(1);
    test::fill_small(arena.project);
    (void)arena.project.set_name("", 0);  // (OS-NAMES: a title is what SAVE AS offers as the folder's name; these tests are about the automatic "Track nn", names_test.cpp about the title)
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena.project, journal);
    app->attach_document(editor.get());
    bank.init(make_pool(), arena, *bank_mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
    app->attach_samples(&bank);
    app->attach_storage(&card);
    app->attach_project_sink(&project);
    saves.attach(card, card);
    app->attach_saves(&saves);
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
  void tap(Button b, bool shift = false) {
    if (shift) {
      input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), true));
      pass(1);
    }
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
    if (shift) {
      input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(Button::Shift), false));
      pass(1);
    }
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  // File F3 NEW, answered ALL (OS-NEWKEEP: the first F3 asks, the second is ALL) or, with `keep`, KEEP INSTRUMENTS (F1).
  // A NEW that ended shows the Pattern view (OS-OPENTOPAT): the tests that go on with the File screen's keys come back to it.
  void back_to_file() {
    if (app->state().screen != ScreenId::File) {
      go(ScreenId::File);
    }
  }
  void new_project(bool keep = false) {
    back_to_file();
    tap(Button::F3);
    tap(keep ? Button::F1 : Button::F3);
  }
  // Passes until no job runs (a bound of the test's own).
  void settle() {
    for (unsigned i = 0; i < 4000; ++i) {
      pass();
      if (app->session().jobs.occupied() == 0u && !saves.busy()) {
        return;
      }
    }
    FAIL_CHECK("the save did not end");
  }
  // Lets the notifications on screen run out (their time is the clock's uptime): the next message is the next one said.
  void expire() {
    for (unsigned i = 0; i < 10 && !message().empty(); ++i) {
      clock.advance(5000000);
      pass();
    }
  }
  std::string message() const { return app->message(); }
  Bytes native_bytes() const {
    std::vector<std::uint8_t> out;
    (void)test::save(test::arena(1).project, out);
    return out;
  }
};

}  // namespace

TEST_CASE("save: F4 on a write protected card is refused with the reason, and nothing is written") {
  Rig rig(false);
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  CHECK(rig.message() == "CARD IS WRITE PROTECTED");
  rig.tap(Button::F5);
  CHECK(rig.message() == "CARD IS WRITE PROTECTED");
  rig.passes(5);
  CHECK(rig.card.log.empty());
  CHECK(rig.card.files.empty());
  CHECK(rig.card.dirs.empty());
  CHECK(rig.app->session().jobs.occupied() == 0u);
}

TEST_CASE("save: SAVE writes the native project into a new folder, byte for byte what the writer makes, in slices, with the strip") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(Button::F4);  // nothing was loaded from Projects/: a new folder
  CHECK(rig.app->session().jobs.occupied() == 1u);
  bool strip = false;
  unsigned passes = 0;
  while (rig.app->session().jobs.occupied() != 0u && passes++ < 4000u) {
    strip = strip || rig.message().find("SAVE TRACK 01") == 0u;
    rig.pass();
  }
  CHECK(strip);
  CHECK(rig.message() == "SAVE TRACK 01 DONE");
  CHECK(passes > 3u);  // not in one gulp
  CHECK(rig.card.same("Projects/Track 01/project.o3pj", rig.native_bytes()));
  CHECK_FALSE(rig.card.has("Projects/Track 01/_o3save.tmp"));
  CHECK_FALSE(rig.card.has("Projects/Track 01/project.o3pj.bak"));
  CHECK(rig.card.violations == 0);
  CHECK(rig.card.removed().empty());
  // The file is a project: the native reader takes it whole.
  const Bytes* file = rig.card.get("Projects/Track 01/project.o3pj");
  NEED(file != nullptr);
  CHECK(test::load(test::arena(2), *file) == formats::native::Progress::Done);

  // SAVE again: the same project's file, the old one kept as the backup.
  const Bytes first = *file;
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 01 DONE");
  CHECK(rig.card.same("Projects/Track 01/project.o3pj", first));
  CHECK(rig.card.same("Projects/Track 01/project.o3pj.bak", first));
  CHECK_FALSE(rig.card.has("Projects/Track 02/project.o3pj"));

  // SAVE AS: the keyboard opens with the next free name, F8 saves under it: a new folder, nothing replaced.
  rig.tap(Button::F5);
  CHECK(rig.app->state().overlay == app::OverlayId::TextEntry);
  CHECK(std::string(rig.app->state().text.entry.text()) == "Track 02");
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 02 DONE");
  CHECK(rig.card.same("Projects/Track 02/project.o3pj", first));
  CHECK(rig.card.same("Projects/Track 01/project.o3pj", first));
  // ... and SAVE now goes on in that one.
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 02 DONE");
  CHECK(rig.card.has("Projects/Track 02/project.o3pj.bak"));
  CHECK_FALSE(rig.card.has("Projects/Track 03/project.o3pj"));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save: SAVE goes into the folder the project was loaded from, beside the OG's files, and a project from Workspace is a new one") {
  Rig rig;
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/Song");
  rig.card.put("Projects/Song/project.mt", Bytes(100, 7));
  rig.project.last_folder = "Projects/Song";
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE SONG DONE");
  CHECK(rig.card.same("Projects/Song/project.o3pj", rig.native_bytes()));
  CHECK(rig.card.same("Projects/Song/project.mt", Bytes(100, 7)));  // the OG's file is not touched
  CHECK_FALSE(rig.card.has("Projects/Track 01/project.o3pj"));
  // Another project is loaded (a Workspace folder): SAVE does not write over the last one, it makes a new folder.
  rig.project.last_folder = "Workspace";
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 01 DONE");
  // Back in Projects/Song: its file again.
  rig.project.last_folder = "Projects/Song";
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE SONG DONE");
  CHECK(rig.card.has("Projects/Song/project.o3pj.bak"));
  // A folder name the card layer does not take is a new project, never a write into it.
  rig.project.last_folder = "Projects/..";
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 02 DONE");
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save: SAVE after OPEN of a folder an interrupted save left (backup only) heals the name and keeps the recovered version as the backup (OS-007o)") {
  Rig rig;
  const Bytes recovered(700, 4);
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/Song");
  rig.card.put("Projects/Song/project.o3pj.bak", recovered);  // the old version, whole, under the backup's name
  rig.card.put("Projects/Song/_o3save.tmp", Bytes(900, 5));    // the new one the cut save had written
  rig.project.last_folder = "Projects/Song";                   // OPEN of that folder (as RECOVER) worked
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE SONG DONE");
  CHECK(rig.card.same("Projects/Song/project.o3pj", rig.native_bytes()));
  CHECK(rig.card.same("Projects/Song/project.o3pj.bak", recovered));
  CHECK_FALSE(rig.card.has("Projects/Song/_o3save.tmp"));
  CHECK_FALSE(rig.card.has("Projects/Track 01/project.o3pj"));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save: after a load that failed (the project is reset) SAVE never replaces the good file of that folder: it makes a new project (OS-007o)") {
  Rig rig;
  const Bytes good(500, 9);
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/Song");
  rig.card.put("Projects/Song/project.o3pj", good);
  rig.project.last_folder = "Projects/Song";
  rig.project.ok = false;
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 01 DONE");
  CHECK(rig.card.same("Projects/Song/project.o3pj", good));
  CHECK_FALSE(rig.card.has("Projects/Song/project.o3pj.bak"));
  CHECK(rig.card.same("Projects/Track 01/project.o3pj", rig.native_bytes()));
}

TEST_CASE("save: the refusals say why: a load running, a save already running, the card that goes read-only, no name left") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.project.is_busy = true;
  rig.expire();
  rig.tap(Button::F4);
  CHECK(rig.message() == "A LOAD IS RUNNING");
  rig.project.is_busy = false;
  rig.tap(Button::F4);
  rig.expire();
  rig.tap(Button::F5);  // the first is still running: the keyboard opens, its F8 is refused (after the job's own strip line, which was first in the queue)
  rig.tap(Button::F8);
  bool said = false;
  for (unsigned i = 0; i < 400 && !said; ++i) {
    said = rig.message() == "A SAVE IS ALREADY RUNNING";
    if (!said) {
      rig.pass();
    }
  }
  CHECK(said);
  rig.settle();
  rig.card.gate_status = hal::storage::WriteStatus::WriteProtected;
  rig.expire();
  rig.tap(Button::F5);
  CHECK(rig.message() == "CARD IS WRITE PROTECTED");
  rig.card.gate_status = hal::storage::WriteStatus::Ok;
  // Every Track NN taken: nothing is replaced, the user is told.
  rig.card.make_dir_now("Projects");
  for (unsigned n = 1; n <= app::SaveService::kMaxTrackNumber; ++n) {
    rig.card.make_dir_now(("Projects/Track " + std::string(n < 10 ? "0" : "") + std::to_string(n)).c_str());
  }
  const std::size_t files = rig.card.files.size();
  rig.expire();
  rig.saves.document_replaced(test::arena(1).project);  // (a new document: SAVE is a new folder again)
  rig.tap(Button::F4);
  CHECK(rig.message() == "NO FREE NAME ON THE CARD");
  CHECK(rig.card.files.size() == files);
  // Save As opens the keyboard with an empty name instead: the user types one (an empty name is refused inside it)
  rig.tap(Button::F5);
  CHECK(rig.app->state().overlay == app::OverlayId::TextEntry);
  CHECK(std::string(rig.app->state().text.entry.text()) == "");
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == app::OverlayId::TextEntry);
  CHECK(std::string(rig.app->state().text.message) == "TYPE A NAME");  // OS-SILENTMSG
  rig.tap(Button::F7);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.card.files.size() == files);
}

TEST_CASE("save: without a card write layer the keys say so") {
  Rig rig;
  rig.app->attach_saves(nullptr);
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  CHECK(rig.message() == "NO CARD WRITE LAYER");
  rig.tap(Button::F5);
  CHECK(rig.message() == "NO CARD WRITE LAYER");
  CHECK(rig.card.log.empty());
}

TEST_CASE("save: a project that changes while it is written is not saved, and nothing is left behind") {
  Rig rig;
  app::NativeSaveSource source;
  model::Project& p = test::arena(1).project;
  source.set(p);
  app::AtomicSave job;
  NEED(job.begin(rig.card, rig.card, "Projects/Track 01", "project.o3pj", app::AtomicSave::Mode::NewOnly, source, "TRACK 01"));
  for (unsigned i = 0; i < 6 && job.running(); ++i) {
    job.step(100);  // (a slice of 512 bytes: folders, the probe, the temp, the sizing of the project, the first bytes)
  }
  NEED(job.running());
  model::Instrument inst = *p.instrument(0);
  inst.name[0] = inst.name[0] == 'Z' ? 'Y' : 'Z';  // an edit between two slices
  NEED(p.set_instrument(0, inst) == model::Status::Ok);
  for (unsigned i = 0; i < 1000 && job.running(); ++i) {
    job.step(100);
  }
  CHECK(job.failed());
  CHECK(job.error() == app::AtomicSave::Error::Source);
  CHECK(std::string(job.detail()) == "CHANGED: TRY AGAIN");
  CHECK_FALSE(rig.card.has("Projects/Track 01/project.o3pj"));
  CHECK_FALSE(rig.card.has("Projects/Track 01/_o3save.tmp"));
}

TEST_CASE("save: Shift + F6 on Sample edit writes the sample as a 16-bit mono WAV, the next free name, never over a file") {
  Rig rig;
  model::Project& project = test::arena(1).project;
  // Slot 0 gets a sample of 3000 frames in the bank's memory, at 22,050 Hz.
  std::int16_t* pcm = rig.memory.data();
  for (unsigned i = 0; i < 3000; ++i) {
    pcm[i] = static_cast<std::int16_t>(static_cast<int>(i) * 7 - 9000);
  }
  rig.bank.adopt(0, pcm, 3000);
  formats::wav::Info info;
  info.sample_rate = 22050;
  info.frames = 3000;
  info.channels = 1;
  model::Instrument inst;
  formats::wav::make_sample_instrument(info, 0, "T", inst);
  NEED(project.set_instrument(0, inst) == model::Status::Ok);
  rig.go(ScreenId::SampleEdit);
  rig.app->invoke(core::action_id("instrument.select"), 1);
  rig.tap(Button::F6, true);
  rig.settle();
  CHECK(rig.message() == "SAVE SMP_001 DONE");
  const Bytes* wav = rig.card.get("Samples/SMP_001.wav");
  NEED(wav != nullptr);
  NEED(wav->size() == 44u + 6000u);
  CHECK(std::memcmp(wav->data(), "RIFF", 4) == 0);
  CHECK(std::memcmp(wav->data() + 8, "WAVEfmt ", 8) == 0);
  const auto le32 = [&](std::size_t at) { return static_cast<std::uint32_t>((*wav)[at] | ((*wav)[at + 1] << 8) | ((*wav)[at + 2] << 16) | (static_cast<std::uint32_t>((*wav)[at + 3]) << 24)); };
  CHECK(le32(4) == 36u + 6000u);
  CHECK(le32(16) == 16u);
  CHECK((*wav)[20] == 1);   // PCM
  CHECK((*wav)[22] == 1);   // mono
  CHECK(le32(24) == 22050u);
  CHECK(le32(28) == 44100u);
  CHECK((*wav)[32] == 2);
  CHECK((*wav)[34] == 16);
  CHECK(std::memcmp(wav->data() + 36, "data", 4) == 0);
  CHECK(le32(40) == 6000u);
  CHECK(std::memcmp(wav->data() + 44, pcm, 6000) == 0);
  // The WAV is a file the importer takes back: same rate, same frames.
  // A second save takes the next number; the first is untouched.
  const Bytes first = *wav;
  rig.tap(Button::F6, true);
  rig.settle();
  CHECK(rig.message() == "SAVE SMP_002 DONE");
  CHECK(rig.card.same("Samples/SMP_001.wav", first));
  CHECK(rig.card.same("Samples/SMP_002.wav", first));
  // A card that is write protected: refused, nothing written.
  rig.card.gate_status = hal::storage::WriteStatus::WriteProtected;
  const std::size_t files = rig.card.files.size();
  rig.expire();
  rig.tap(Button::F6, true);
  CHECK(rig.message() == "CARD IS WRITE PROTECTED");
  CHECK(rig.card.files.size() == files);
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save: the device settings save themselves after a change has stayed still, quietly, once, and load back") {
  Rig rig;
  rig.passes(3);  // the first pass takes the settings as they are
  rig.card.log.clear();
  CHECK_FALSE(rig.card.has("Settings/device.o3st"));
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  rig.pass(500);
  CHECK(rig.card.log.empty());  // not yet: it waits for the change to stay still
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 2.0f) == model::Status::Ok);
  rig.pass(1000);
  CHECK(rig.card.log.empty());  // the second change restarted the wait
  rig.pass(1000);
  rig.pass(16);
  CHECK(rig.app->session().jobs.occupied() <= 1u);
  for (unsigned i = 0; i < 400 && !rig.card.has("Settings/device.o3st"); ++i) {
    CHECK(rig.message().find("SAVE SETTINGS") != 0u);  // quiet
    rig.pass();
  }
  NEED(rig.card.has("Settings/device.o3st"));
  rig.passes(5);
  CHECK(rig.app->session().jobs.occupied() == 0u);
  CHECK(rig.message().find("SETTINGS") == std::string::npos);
  // What was written is the settings file: the reader gives the value back.
  model::Settings back;
  const Bytes* file = rig.card.get("Settings/device.o3st");
  NEED(file != nullptr);
  CHECK(formats::settings::read(file->data(), file->size(), back).error == formats::settings::Error::None);
  CHECK(back.get(model::device_setting::Theme) == 2.0f);
  // Nothing changed since: nothing is written again. A later change makes a second file and keeps the first as the backup.
  rig.card.log.clear();
  rig.pass(5000);
  rig.passes(5);
  CHECK(rig.card.log.empty());
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 0.0f) == model::Status::Ok);
  rig.pass(2000);
  rig.pass(2000);
  rig.settle();
  CHECK(rig.card.has("Settings/device.o3st.bak"));
  CHECK(rig.card.violations == 0);
  // The boot reads it back (read only), the defaults when it is not there or is damaged.
  model::Settings boot;
  CHECK(app::load_device_settings(rig.card, boot));
  CHECK(boot.get(model::device_setting::Theme) == 0.0f);
  rig.card.files["settings/device.o3st"][10] ^= 0xFF;
  model::Settings damaged;
  CHECK_FALSE(app::load_device_settings(rig.card, damaged));
  CHECK(damaged.is_default());
  rig.card.files.erase("settings/device.o3st");
  CHECK(app::load_device_settings(rig.card, damaged));  // OS-007o: no file but its backup (a save cut between its two renames): the backup is read
  CHECK(damaged.get(model::device_setting::Theme) == 2.0f);
  rig.card.files.erase("settings/device.o3st.bak");
  CHECK_FALSE(app::load_device_settings(rig.card, damaged));
  CHECK(damaged.is_default());
}

TEST_CASE("save: the device settings are not written to a card that may not be written, and not asked again until they change") {
  Rig rig(false);
  rig.passes(3);
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  rig.pass(3000);
  rig.pass(3000);  // the change has stayed still: the gate says no, and that is the end of it
  rig.passes(5);
  CHECK(rig.card.log.empty());
  CHECK(rig.card.files.empty());
  CHECK(rig.message().find("SETTINGS") == std::string::npos);  // silent: the settings stay in memory
  rig.card.gate_status = hal::storage::WriteStatus::Ok;
  rig.pass(3000);
  rig.pass(3000);
  rig.passes(5);
  CHECK(rig.card.files.empty());  // (not retried on its own: the next change saves)
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 2.0f) == model::Status::Ok);
  rig.pass(3000);
  rig.pass(3000);
  rig.settle();
  CHECK(rig.card.has("Settings/device.o3st"));
}

// OS-007o: the firmware update path holds the card (an upload, an arm, the hand-over): no save starts then, and the device settings wait.
namespace {
bool g_firmware_has_card = false;
bool firmware_has_card() noexcept { return g_firmware_has_card; }
}  // namespace

TEST_CASE("save: a save is refused while the firmware update path has the card, and the settings wait for it") {
  Rig rig;
  rig.saves.attach_claim(&firmware_has_card);
  rig.go(ScreenId::File);
  g_firmware_has_card = true;
  rig.tap(Button::F4);
  CHECK(rig.message() == "FIRMWARE UPDATE HAS THE CARD: TRY AGAIN");
  rig.expire();
  rig.tap(Button::F5);
  rig.tap(Button::F8);
  CHECK(rig.message() == "FIRMWARE UPDATE HAS THE CARD: TRY AGAIN");
  rig.passes(5);
  CHECK(rig.card.log.empty());
  CHECK(rig.app->session().jobs.occupied() == 0u);
  // The settings: changed and still, but the card is held: nothing is written, and they follow once it is let go.
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  rig.pass(3000);
  rig.pass(3000);
  rig.passes(5);
  CHECK(rig.card.log.empty());
  g_firmware_has_card = false;
  rig.pass(100);
  rig.settle();
  CHECK(rig.card.has("Settings/device.o3st"));
  rig.expire();
  rig.tap(Button::F5);  // and a save goes through again
  rig.tap(Button::F8);
  rig.settle();
  CHECK(rig.card.has("Projects/Track 01/project.o3pj"));
  CHECK(rig.card.violations == 0);
}

// OS-007o: File F3 NEW.
TEST_CASE("new: F3 asks which new project (ALL or KEEP INSTRUMENTS) even with nothing unsaved; the answer starts the platform's reset and says so; the result is told") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(Button::F3);
  CHECK(rig.message() == "NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS");
  {  // OS-KEYROWDLG: the answers are on the keys, inverted; the others keep their labels
    const test::RowRead row = test::read_row(*rig.app);
    CHECK(row.label[0] == "KEEP INS");
    CHECK(row.label[2] == "ALL");
    CHECK(row.hot == 0x05u);
    CHECK(row.label[3] == "SAVE");
  }
  CHECK(rig.project.news == 0u);
  CHECK(rig.project.keeps == 0u);
  rig.tap(Button::F3);  // ALL
  CHECK(rig.project.news == 1u);
  CHECK(rig.project.keeps == 0u);
  CHECK(rig.message() == "NEW PROJECT...");
  CHECK(rig.app->state().file.pending == app::FileView::Pending::New);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.message() == "NEW PROJECT");
  CHECK(rig.app->state().file.pending == app::FileView::Pending::Open);
  rig.expire();
  rig.new_project(true);  // KEEP INSTRUMENTS
  CHECK(rig.project.news == 1u);
  CHECK(rig.project.keeps == 1u);
  CHECK(rig.message() == "NEW PROJECT, KEEPING INSTRUMENTS...");
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.message() == "NEW PROJECT");
  rig.expire();
  rig.project.new_result = app::ProjectSink::Start::Busy;
  rig.new_project();
  CHECK(rig.message() == "A LOAD IS ALREADY RUNNING");
  rig.expire();
  rig.project.new_result = app::ProjectSink::Start::NoDocument;
  rig.new_project();
  CHECK(rig.message() == "NO DOCUMENT TO REPLACE");
  rig.expire();
  rig.new_project(true);
  CHECK(rig.message() == "NO DOCUMENT TO REPLACE");
  CHECK(rig.card.log.empty());  // the card is never touched
}

TEST_CASE("new: the question can be cancelled: another key, a move, leaving the screen or a wait change nothing; F1 is OPEN again afterwards (OS-NEWKEEP)") {
  Rig rig;
  rig.go(ScreenId::File);
  model::Project& project = test::arena(1).project;
  const std::uint32_t generation = project.generation();
  const unsigned instruments = project.instrument_count();
  rig.tap(Button::F3);
  CHECK(rig.app->state().file.new_choice);
  CHECK(test::read_row(*rig.app).marked);
  rig.tap(Button::F2);  // RESCAN
  CHECK_FALSE(rig.app->state().file.new_choice);
  CHECK_FALSE(test::read_row(*rig.app).marked);  // the plain row is back in the same pass
  rig.tap(Button::F1);  // no longer KEEP: an OPEN, with nothing under the cursor
  rig.tap(Button::F3);
  rig.tap(Button::Down);
  CHECK_FALSE(rig.app->state().file.new_choice);
  rig.tap(Button::F3);
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  CHECK_FALSE(rig.app->state().file.new_choice);
  rig.tap(Button::F1);
  rig.tap(Button::F3);  // asked, and left alone longer than the strip shows the question
  rig.clock.advance(core::ticks_from_ms(app::FileView::kNewAskMs + 500u));
  rig.tap(Button::F1);  // an OPEN, not KEEP INSTRUMENTS
  rig.tap(Button::F3);  // asks again, it does not go on
  CHECK(rig.app->state().file.new_choice);
  CHECK(rig.project.news == 0u);
  CHECK(rig.project.keeps == 0u);
  CHECK(rig.project.opens == 0u);  // (the listing is empty: nothing to open)
  CHECK(project.generation() == generation);
  CHECK(project.instrument_count() == instruments);
  CHECK(rig.card.log.empty());
}

TEST_CASE("new: a cancelled question leaves the strip at once, whichever way it was cancelled; an answered one is replaced by the answer's line (OS-NEWKEEP2)") {
  Rig rig;
  rig.go(ScreenId::File);
  model::Project& project = test::arena(1).project;
  const std::string ask = "NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS";
  rig.tap(Button::F3);
  CHECK(rig.message() == ask);
  rig.tap(Button::F2);  // another key (RESCAN)
  CHECK(rig.message().empty());
  CHECK(rig.app->state().notify.empty());
  rig.tap(Button::F3);
  CHECK(rig.message() == ask);
  rig.tap(Button::Down);  // a move
  CHECK(rig.message().empty());
  rig.tap(Button::F3);
  CHECK(rig.message() == ask);
  rig.tap(Button::F6);  // a page change
  CHECK(rig.message().empty());
  rig.tap(Button::F6);
  rig.tap(Button::F3);
  CHECK(rig.message() == ask);
  rig.go(ScreenId::Pattern);  // leaving the screen
  CHECK(rig.message().empty());
  rig.go(ScreenId::File);
  // The same for the question with the unsaved-changes text.
  CHECK(project.set_name("edited", 6) == model::Status::Ok);
  rig.tap(Button::F3);
  CHECK(rig.message() == "UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS");
  rig.tap(Button::F2);
  CHECK(rig.message().empty());
  // An answer is not a cancel: the line is the answer's, and it stays.
  rig.tap(Button::F3);
  rig.tap(Button::F3);
  CHECK(rig.project.news == 1u);
  CHECK(rig.message() == "NEW PROJECT...");
  CHECK(rig.project.keeps == 0u);
  // Asking again right after a cancel (F3 is itself the key that asks) shows the question once more.
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  rig.app->document_replaced();
  rig.back_to_file();
  rig.tap(Button::F3);
  rig.tap(Button::F1);  // KEEP INSTRUMENTS
  CHECK(rig.project.keeps == 1u);
  CHECK(rig.message() == "NEW PROJECT, KEEPING INSTRUMENTS...");
  CHECK(rig.card.log.empty());
}

// OS-OPENTOPAT (maintainer, 2026-10-17; new Community OS behaviour): NEW shows the Pattern view when the platform has made the new document; a NEW that fails stays on File with its error.
TEST_CASE("new: ALL and KEEP INSTRUMENTS show the Pattern view when they have ended, a failed NEW stays on File (OS-OPENTOPAT)") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(Button::F3);  // the question
  rig.tap(Button::F3);  // ALL
  CHECK(rig.project.news == 1u);
  CHECK(rig.app->state().screen == ScreenId::File);  // while the platform replaces the document
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.app->state().screen == ScreenId::Pattern);
  CHECK(rig.message() == "NEW PROJECT");
  rig.expire();
  rig.new_project(true);  // KEEP INSTRUMENTS (from File again)
  CHECK(rig.project.keeps == 1u);
  CHECK(rig.app->state().screen == ScreenId::File);
  rig.project.is_busy = false;
  rig.pass();
  CHECK(rig.app->state().screen == ScreenId::Pattern);
  rig.expire();
  // A NEW that does not come out right (the platform says the document is not loaded): File, the error.
  rig.new_project();
  rig.project.is_busy = false;
  rig.project.ok = false;
  rig.pass();
  CHECK(rig.app->state().screen == ScreenId::File);
  CHECK(rig.message() == "NEW PROJECT FAILED");
  // A start that is refused never leaves File.
  rig.project.ok = true;
  rig.expire();
  rig.project.new_result = app::ProjectSink::Start::Busy;
  rig.new_project();
  CHECK(rig.message() == "A LOAD IS ALREADY RUNNING");
  CHECK(rig.app->state().screen == ScreenId::File);
}

// OS-BUG-PROMPTQ: a question counts only as the head of the strip, so it takes the head at once, ahead of the news of a job (an info), which is dropped and not shown after.
TEST_CASE("new: a message on the strip when F3 asks: the question takes the head at once, the second F3 answers ALL (OS-NEWKEEP, OS-BUG-PROMPTQ)") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(Button::F6, true);  // Shift+F6 NAME, F8 OK: its line (another source than the File screen's own, which the question would replace) is on the strip
  rig.tap(Button::F8);
  const std::string before = rig.message();
  CHECK_FALSE(before.empty());
  rig.tap(Button::F3);  // the question is the line on the strip now, not queued behind the older one
  CHECK(rig.message() == "UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(rig.app->state().notify.size() == 1u);  // (the older line was dropped)
  CHECK(rig.project.news == 0u);
  CHECK(rig.app->state().file.new_choice);
  rig.tap(Button::F3);  // the user has read it: ALL
  CHECK(rig.project.news == 1u);
}

TEST_CASE("new: the project changes inside the question's 4 s: the answer asks once more, with the unsaved-changes text (OS-NEWKEEP)") {
  Rig rig;
  rig.go(ScreenId::File);
  model::Project& project = test::arena(1).project;
  rig.tap(Button::F3);
  CHECK(rig.message() == "NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(project.set_name("edited", 6) == model::Status::Ok);  // a change no save has taken, after the plain question was asked
  rig.tap(Button::F3);  // ALL: not yet, the warning first
  CHECK(rig.project.news == 0u);
  CHECK(rig.message() == "UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(rig.app->state().file.new_choice);
  rig.tap(Button::F3);  // the warned question is answered
  CHECK(rig.project.news == 1u);
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  rig.app->document_replaced();
  rig.back_to_file();
  // The same for KEEP INSTRUMENTS (F1).
  rig.tap(Button::F3);
  CHECK(rig.message() == "NEW PROJECT: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(project.set_name("edited again", 12) == model::Status::Ok);
  rig.tap(Button::F1);
  CHECK(rig.project.keeps == 0u);
  CHECK(rig.message() == "UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS");
  rig.tap(Button::F1);
  CHECK(rig.project.keeps == 1u);
}

TEST_CASE("new: with unsaved changes the first F3 asks (and says so), the second goes on; a move, another key or leaving the screen takes the question back") {
  Rig rig;
  rig.go(ScreenId::File);
  model::Project& project = test::arena(1).project;
  CHECK(project.set_name("edited", 6) == model::Status::Ok);  // a change no save has taken
  rig.tap(Button::F3);
  CHECK(rig.message() == "UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(rig.project.news == 0u);
  rig.tap(Button::F3);  // (no wait between: the question is only good while its text is on the strip)
  CHECK(rig.project.news == 1u);
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  // The platform made the document new: the save layer takes it as clean.
  rig.app->document_replaced();
  rig.new_project();
  CHECK(rig.project.news == 2u);  // nothing unsaved: the question is the plain one
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  rig.back_to_file();
  // Armed, then disarmed three ways.
  CHECK(project.set_name("edited again", 12) == model::Status::Ok);
  rig.tap(Button::F3);
  CHECK(rig.app->state().file.new_choice);
  rig.tap(Button::Down);
  CHECK_FALSE(rig.app->state().file.new_choice);
  rig.tap(Button::F3);
  CHECK(rig.app->state().file.new_choice);
  rig.tap(Button::F2);  // RESCAN
  CHECK_FALSE(rig.app->state().file.new_choice);
  rig.tap(Button::F3);
  CHECK(rig.app->state().file.new_choice);
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  CHECK_FALSE(rig.app->state().file.new_choice);
  CHECK(rig.project.news == 2u);
}

TEST_CASE("new: a saved project is clean again; a save running or a load running refuses; the next SAVE never replaces the folder of the project before") {
  Rig rig;
  rig.go(ScreenId::File);
  model::Project& project = test::arena(1).project;
  CHECK(project.set_name("x", 1) == model::Status::Ok);
  rig.tap(Button::F4);  // SAVE: Track 01
  rig.tap(Button::F3);  // a save is running
  CHECK(rig.project.news == 0u);
  CHECK_FALSE(rig.app->state().file.new_choice);
  rig.settle();
  CHECK(rig.card.has("Projects/Track 01/project.o3pj"));
  rig.expire();
  rig.new_project();  // saved: clean, the plain question
  CHECK(rig.project.news == 1u);
  rig.project.is_busy = false;
  rig.expire();
  rig.back_to_file();
  rig.tap(Button::F3);  // asks
  rig.project.is_busy = true;  // a load (the sink says busy)
  rig.expire();
  rig.tap(Button::F3);
  CHECK(rig.message() == "A LOAD IS ALREADY RUNNING");
  rig.project.is_busy = false;
  // The project before was saved as Track 01 from a document no folder held (anchor ""); after NEW the platform tells the app, and SAVE makes Track 02.
  rig.app->document_replaced();
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 02 DONE");
  CHECK(rig.card.has("Projects/Track 02/project.o3pj"));
  CHECK_FALSE(rig.card.has("Projects/Track 01/project.o3pj.bak"));
}

TEST_CASE("new: an imported document (document_replaced(true)) is unsaved work: F3 asks, a second document_replaced(false) is clean, a save makes it clean (OS-FILES)") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.app->document_replaced(true);  // File > IMPORT ended: a song that is on no card
  rig.tap(Button::F3);
  CHECK(rig.message() == "UNSAVED CHANGES: F3 ALL, F1 KEEP INSTRUMENTS");
  CHECK(rig.project.news == 0u);
  rig.expire();
  rig.tap(Button::Down);  // a move takes the question back
  rig.tap(Button::F4);    // SAVE: the import is on the card now
  rig.settle();
  CHECK(rig.card.has("Projects/Track 01/project.o3pj"));
  rig.expire();
  rig.new_project();
  CHECK(rig.project.news == 1u);  // saved: clean, the plain question
  rig.project.is_busy = false;
  rig.expire();
  rig.app->document_replaced(true);
  rig.app->document_replaced(false);  // a load that follows is clean again
  rig.new_project();
  CHECK(rig.project.news == 2u);
}

// ---- OS-FU2: the on-screen keyboard for Save As -----------------------------------------------------------------------------------------------------------------

namespace {

void pad(Rig& rig, unsigned index) {
  rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(index), 100));
  rig.pass();
  rig.input.push(core::make_pad_event(rig.clock.now(), static_cast<std::uint8_t>(index), 0));
  rig.pass();
}

std::string typed(const Rig& rig) { return rig.app->state().text.entry.text(); }

}  // namespace

TEST_CASE("save as: F5 opens the keyboard with the next free name selected; pads type over it and F8 saves a folder of that name") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  NEED(rig.app->state().overlay == app::OverlayId::TextEntry);
  CHECK(typed(rig) == "Track 01");
  CHECK(rig.app->state().text.entry.selected());
  pad(rig, 21);  // p (the pads 12..23 are q w e r t y u i o p - +, one a pad)
  pad(rig, 24);  // 'a'
  pad(rig, 36);  // 'z'
  CHECK(typed(rig) == "paz");
  rig.tap(Button::Delete);
  CHECK(typed(rig) == "pa");
  rig.tap(Button::F2);  // caps
  pad(rig, 36);
  CHECK(typed(rig) == "paZ");
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  rig.settle();
  CHECK(rig.message() == "SAVE PAZ DONE");
  CHECK(rig.card.same("Projects/paZ/project.o3pj", rig.native_bytes()));
  CHECK_FALSE(rig.card.has("Projects/Track 01/project.o3pj"));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save as: a name that is taken, or that the card cannot take, is said inside the keyboard, which stays open; F6 puts the automatic name back, F7 cancels") {
  Rig rig;
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/Mine");
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  rig.tap(Button::Delete);  // the selected name goes
  CHECK(typed(rig).empty());
  pad(rig, 37);  // x
  pad(rig, 8);   // 9
  pad(rig, 13);  // w
  CHECK(typed(rig) == "x9w");
  rig.tap(Button::F6);  // the automatic name again, selected
  CHECK(typed(rig) == "Track 01");
  CHECK(rig.app->state().text.entry.selected());
  // "Mine" exists: caps, m (key 40), caps, i (18), n (39), e (13)
  auto& entry = const_cast<app::TextEntry&>(rig.app->state().text.entry);
  entry.set("");
  (void)entry.type(app::kTextKeyCaps);
  (void)entry.type(40);
  (void)entry.type(app::kTextKeyCaps);
  (void)entry.type(18);
  (void)entry.type(39);
  (void)entry.type(13);
  CHECK(typed(rig) == "Mine");
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == app::OverlayId::TextEntry);
  CHECK(std::string(rig.app->state().text.message).find("A PROJECT OF THAT NAME EXISTS") == 0u);
  CHECK(rig.card.files.empty());
  CHECK(rig.app->session().jobs.occupied() == 0u);
  // a character the card does not take is refused when typed ('@' is key 32, under pad 33), and the keyboard says so
  entry.set("");
  pad(rig, 33);
  CHECK(typed(rig).empty());
  CHECK(std::string(rig.app->state().text.message) == "NOT IN A FILE NAME");
  // a name that starts with a space is refused as a whole
  entry.set(" x");
  rig.tap(Button::F8);
  CHECK(std::string(rig.app->state().text.message).find("NOT A NAME") == 0u);
  CHECK(rig.app->state().overlay == app::OverlayId::TextEntry);
  rig.tap(Button::F7);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.card.files.empty());
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save as: the keyboard's focus is moved by the arrows and the encoder, Insert types the key in focus, and nothing leaks to the screen under it") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  CHECK(rig.app->state().text.entry.key() == app::kTextKeyBackspace);
  rig.tap(Button::Right);  // wraps to the first key of the row
  CHECK(rig.app->state().text.entry.key() == 0u);
  rig.tap(Button::Down);
  CHECK(rig.app->state().text.entry.key() == 11u);  // 'q'
  rig.tap(Button::Delete);  // the selection goes
  rig.tap(Button::Insert);
  CHECK(typed(rig) == "q");
  rig.input.push(core::make_encoder_event(rig.clock.now(), 0, 2));
  rig.pass();
  CHECK(rig.app->state().text.entry.key() == 13u);
  rig.tap(Button::F1);
  CHECK(typed(rig) == "qe");
  CHECK(rig.app->state().screen == ScreenId::File);  // the File screen did not see any of it
  CHECK(rig.app->session().jobs.occupied() == 0u);
}

TEST_CASE("save as: the keyboard is drawn within the display-list budget over the File screen, in every theme") {
  Rig rig;
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  for (const ui::ThemeId id : {ui::ThemeId::Amiga, ui::ThemeId::Black, ui::ThemeId::White}) {
    platform::host::FakeDisplay sink;
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(sink, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, ui::theme(id));
    NEED(r.begin(ui::Color::Pane));
    app::render_scene(rig.app->model(), rig.app->state(), r);
    CHECK(r.end());
    CHECK(sink.last_frame().size() <= 1792u);
  }
}

// ---- OS-FU2: a project's samples are saved with it ----------------------------------------------------------------------------------------------------------------

namespace {

// A sample instrument in `slot` (its path "samples/instrNN.wav" unless `path` says otherwise) whose audio, `frames` of a ramp from `seed`, sits in the bank.
void put_sample(Rig& rig, unsigned slot, std::uint32_t frames, int seed, const char* path = nullptr) {
  model::Project& project = test::arena(1).project;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  if (path != nullptr) {
    std::strncpy(in.sample_path, path, sizeof in.sample_path - 1);
  } else {
    std::snprintf(in.sample_path, sizeof in.sample_path, "samples/instr%02u.wav", slot + 1u);
  }
  NEED(project.set_instrument(slot, in) == model::Status::Ok);
  std::int16_t* pcm = rig.memory.data() + slot * 4000u;
  for (std::uint32_t i = 0; i < frames; ++i) {
    pcm[i] = static_cast<std::int16_t>(seed + static_cast<int>(i % 200u) * 7);
  }
  rig.bank.adopt(slot, pcm, frames);
}

// What a WAV save of that audio makes: the 44-byte header and the samples.
Bytes wav_of(const Rig& rig, unsigned slot, std::uint32_t rate = 44100) {
  const app::SampleBank::Pcm pcm = rig.bank.pcm(slot);
  Bytes out(app::WavSaveSource::kHeaderBytes);
  app::WavSaveSource::make_header(out.data(), pcm.frames, 1, rate);
  const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(pcm.data);
  out.insert(out.end(), raw, raw + pcm.frames * 2u);
  return out;
}

std::size_t index_of(const MemCard& card, const std::string& entry) {
  for (std::size_t i = 0; i < card.log.size(); ++i) {
    if (card.log[i] == entry) {
      return i;
    }
  }
  return card.log.size();
}

void save_as(Rig& rig, const char* name) {
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  auto& entry = const_cast<app::TextEntry&>(rig.app->state().text.entry);
  entry.set(name);
  rig.tap(Button::F8);
  rig.settle();
}

}  // namespace

TEST_CASE("save samples: Save As writes every sample of the project into the new folder, each atomically, BEFORE the project file") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  CHECK(rig.message() == "SAVE MINE DONE");
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));
  CHECK(rig.card.same("Projects/Mine/samples/instr03.wav", wav_of(rig, 2)));
  CHECK_FALSE(rig.card.has("Projects/Mine/samples/instr02.wav"));
  CHECK(rig.card.same("Projects/Mine/project.o3pj", rig.native_bytes()));
  CHECK_FALSE(rig.card.has("Projects/Mine/samples/_o3save.tmp"));
  const std::size_t a = index_of(rig.card, "rename Projects/Mine/samples/_o3save.tmp>Projects/Mine/samples/instr01.wav");
  const std::size_t b = index_of(rig.card, "rename Projects/Mine/samples/_o3save.tmp>Projects/Mine/samples/instr03.wav");
  const std::size_t p = index_of(rig.card, "rename Projects/Mine/_o3save.tmp>Projects/Mine/project.o3pj");
  CHECK(a < b);
  CHECK(b < p);
  CHECK(p < rig.card.log.size());
  CHECK(rig.card.violations == 0);
  CHECK(rig.app->session().jobs.occupied() == 0u);
}

TEST_CASE("save samples: a plain Save writes only what changed since; a changed sample replaces its file and keeps the old one as .bak") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  const Bytes first0 = wav_of(rig, 0);
  rig.card.log.clear();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);  // SAVE into the project the user is in: nothing changed in the samples
  rig.settle();
  for (const std::string& line : rig.card.log) {
    CHECK(line.find("samples") == std::string::npos);
  }
  CHECK(rig.card.has("Projects/Mine/project.o3pj.bak"));
  // slot 0's audio is replaced (its version moves): only that file is written again
  put_sample(rig, 0, 650, 500);
  rig.card.log.clear();
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav.bak", first0));
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/instr01.wav>Projects/Mine/samples/instr01.wav.bak") < rig.card.log.size());
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/_o3save.tmp>Projects/Mine/samples/instr03.wav") == rig.card.log.size());  // slot 2 was not written again
  CHECK(rig.card.violations == 0);
  // a Save As to another name copies all of them, whatever was saved before
  save_as(rig, "Other");
  CHECK(rig.card.same("Projects/Other/samples/instr01.wav", wav_of(rig, 0)));
  CHECK(rig.card.same("Projects/Other/samples/instr03.wav", wav_of(rig, 2)));
}

TEST_CASE("save samples: a card swapped between two jobs of one save fails the save and writes no project file (OS-FU5)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  rig.card.swap_after_renames = 1;  // another card right after the first sample is committed
  save_as(rig, "Mine");
  CHECK(rig.card.has("Projects/Mine/samples/instr01.wav"));
  CHECK_FALSE(rig.card.has("Projects/Mine/samples/instr03.wav"));
  CHECK_FALSE(rig.card.has("Projects/Mine/project.o3pj"));  // the project is not put on the second card with one sample missing
  CHECK(rig.card.begins == 1u);  // one begin for the whole save
  CHECK(rig.card.ends == 1u);    // and it ended
  CHECK(rig.app->session().jobs.occupied() == 0u);
}

TEST_CASE("save samples: what an earlier save wrote is forgotten when the card is another (OS-FU5)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  rig.card.log.clear();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);  // the same card: nothing of the samples is written again
  rig.settle();
  for (const std::string& line : rig.card.log) {
    CHECK(line.find("samples") == std::string::npos);
  }
  ++rig.card.id;  // another card (the fake keeps the files: the test is what is written)
  rig.card.log.clear();
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/instr01.wav>Projects/Mine/samples/instr01.wav.bak") < rig.card.log.size());  // (the files were there: they are replaced)
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/instr03.wav>Projects/Mine/samples/instr03.wav.bak") < rig.card.log.size());
}

// OS-BUG-REMOUNT. The card goes out and comes back through the event path the platform uses (App::card_changed); the fake card keeps its serial unless a test says otherwise.
void card_out_and_back(Rig& rig) {
  rig.app->card_changed(app::CardEvent::Removed);
  rig.app->card_changed(app::CardEvent::Reading);
  rig.app->card_changed(app::CardEvent::Ready);
}

// Files whose name is "samples" in the card's log since it was cleared.
std::size_t sample_writes(const Rig& rig) {
  std::size_t n = 0;
  for (const std::string& line : rig.card.log) {
    n += line.find("samples") != std::string::npos ? 1u : 0u;
  }
  return n;
}

TEST_CASE("remount: the SAME card (same serial) back, a computer erased one sample and replaced another: both are written again, the untouched one is checked with one lookup and skipped (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 1, 700, 300);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  rig.card.files.erase("projects/mine/samples/instr01.wav");                                         // erased elsewhere
  rig.card.put("Projects/Mine/samples/instr03.wav", std::string("not the audio any more"));          // replaced elsewhere
  rig.card.log.clear();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);  // the control: no card event, the save trusts what it wrote
  rig.settle();
  CHECK(sample_writes(rig) == 0u);
  card_out_and_back(rig);
  rig.card.log.clear();
  const std::size_t stats = rig.card.stat_calls;
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));  // written again (the file was erased)
  CHECK(rig.card.same("Projects/Mine/samples/instr03.wav", wav_of(rig, 2)));  // and replaced (the file was not the audio)
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/_o3save.tmp>Projects/Mine/samples/instr02.wav") == rig.card.log.size());  // slot 1 was not touched: skipped
  CHECK(rig.card.stat_calls >= stats + 3u);  // one lookup for each sample the save had to be sure of
  CHECK(rig.card.violations == 0);
}

TEST_CASE("remount: the same card back with nothing changed: every sample is confirmed and skipped, the project file is written (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  card_out_and_back(rig);
  rig.card.log.clear();
  rig.expire();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(sample_writes(rig) == 0u);  // (no temp, no rename in samples/: nothing was written)
  CHECK(rig.card.same("Projects/Mine/project.o3pj", rig.native_bytes()));
  CHECK(rig.card.violations == 0);
  // the beliefs are current again: the next plain save needs no lookups
  const std::size_t stats = rig.card.stat_calls;
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.stat_calls == stats);
}

TEST_CASE("remount: a sample the loader read from the project's folder (the bank's belief) is confirmed by its size and stamp, and written when the file changed (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  rig.card.put("Projects/Mine/samples/instr01.wav", wav_of(rig, 0));
  hal::storage::FileStamp st;
  CHECK(rig.card.stat("Projects/Mine/samples/instr01.wav", st) == hal::storage::Status::Ok);
  rig.bank.mark_on_card(0, st.size, st.stamp);
  rig.project.last_folder = "Projects/Mine";
  rig.go(ScreenId::File);
  card_out_and_back(rig);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(sample_writes(rig) == 0u);  // the file is as the loader saw it
  CHECK(rig.card.has("Projects/Mine/project.o3pj"));
  // the card goes out again, a computer replaces the WAV, it comes back: the belief no longer holds
  card_out_and_back(rig);
  rig.card.put("Projects/Mine/samples/instr01.wav", std::string("another recording"));
  rig.card.log.clear();
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("remount: a belief with nothing recorded (a loader that could not stat) is never confirmed by a remount: the sample is written (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  rig.bank.mark_on_card(0);  // no size, no stamp
  rig.project.last_folder = "Projects/Mine";
  rig.card.put("Projects/Mine/samples/instr01.wav", wav_of(rig, 0));
  rig.go(ScreenId::File);
  rig.tap(Button::F4);  // no card event: the belief stands, nothing is written
  rig.settle();
  CHECK(sample_writes(rig) == 0u);
  card_out_and_back(rig);
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/instr01.wav>Projects/Mine/samples/instr01.wav.bak") < rig.card.log.size());  // rewritten
}

TEST_CASE("remount: another card (another serial) through the event path: everything is written, whatever the files look like (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  rig.app->card_changed(app::CardEvent::Removed);
  ++rig.card.id;  // another volume (the fake keeps the files: what is written is the test)
  rig.app->card_changed(app::CardEvent::Reading);
  rig.app->card_changed(app::CardEvent::Ready);
  rig.card.log.clear();
  rig.expire();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/instr01.wav>Projects/Mine/samples/instr01.wav.bak") < rig.card.log.size());
  CHECK(index_of(rig.card, "rename Projects/Mine/samples/instr03.wav>Projects/Mine/samples/instr03.wav.bak") < rig.card.log.size());
}

TEST_CASE("remount: NotReadable alone, and a Reading alone (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  save_as(rig, "Mine");
  // Reading only: the card is being read, nothing is known yet; the caches stay as they are and no lookup is made
  rig.card.put("Projects/Mine/samples/instr01.wav", std::string("changed behind our back"));
  rig.app->card_changed(app::CardEvent::Reading);
  rig.card.log.clear();
  const std::size_t stats = rig.card.stat_calls;
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(sample_writes(rig) == 0u);
  CHECK(rig.card.stat_calls == stats);
  // NotReadable (a card that did not mount): the beliefs are to be checked at the next save
  rig.app->card_changed(app::CardEvent::NotReadable);
  rig.expire();
  rig.card.log.clear();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));  // the changed file was found out and replaced
  CHECK(rig.card.violations == 0);
}

TEST_CASE("remount: an event in the middle of a save leaves the beliefs to be checked at the next save (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 1, 700, 300);
  put_sample(rig, 2, 900, -2000);
  rig.go(ScreenId::File);
  rig.tap(Button::F4);  // starts the plan: three samples, then the project
  rig.passes(2);
  CHECK(rig.saves.busy());  // (the event really comes in the middle of the save)
  card_out_and_back(rig);  // the card comes back while the save is running (the fake card keeps its serial: the save goes on)
  rig.settle();
  CHECK(rig.card.violations == 0);
  CHECK(rig.app->session().jobs.occupied() == 0u);
  // a computer edits a sample after that; the plan was built before the event, so nothing was confirmed after it
  rig.card.put("Projects/Track 01/samples/instr02.wav", std::string("edited elsewhere"));
  rig.card.log.clear();
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Track 01/samples/instr02.wav", wav_of(rig, 1)));
}

TEST_CASE("remount: a save that cannot fit the samples after a remount says the card is full, and writes no project file (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  rig.card.put("Projects/Mine/samples/instr01.wav", std::string("changed"));
  rig.card.put("Projects/Mine/samples/instr03.wav", std::string("changed"));
  card_out_and_back(rig);
  rig.card.space_left = 100;  // the card is nearly full: the rewrites do not fit
  rig.card.files.erase("projects/mine/project.o3pj.bak");
  const Bytes* before_file = rig.card.get("Projects/Mine/project.o3pj");
  NEED(before_file != nullptr);
  const Bytes project_before = *before_file;  // (a copy: the map may change)
  rig.expire();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message().find("CARD IS FULL") != std::string::npos);
  CHECK(rig.card.same("Projects/Mine/project.o3pj", project_before));  // the project file on the card is untouched
}

TEST_CASE("remount: a save that did not complete (card full) leaves the beliefs to be checked; a later save with no samples to check does not make them current (OS-BUG-REMOUNT)") {
  Rig rig;
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  rig.card.put("Projects/Mine/samples/instr01.wav", std::string("changed on a computer"));
  card_out_and_back(rig);
  rig.card.space_left = 100;
  rig.expire();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message().find("CARD IS FULL") != std::string::npos);  // the save checked the samples, found one changed and could not write it
  rig.card.space_left = ~std::size_t{0};
  // a project-file-only save (no bank): it checks no sample, so it must not mark the beliefs current
  CHECK(rig.saves.save_project(rig.app->session().jobs, test::arena(1).project, "", false, nullptr, nullptr) == app::SaveService::Start::Started);
  rig.settle();
  rig.card.log.clear();
  rig.expire();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));  // the changed file is still found out and replaced
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save samples: audio that was read from the project's own folder is not written back until it changes; one that was not (a .pti's) is") {
  Rig rig;
  put_sample(rig, 0, 600, 100);  // read from Projects/Mine/samples (the loader marks it)
  put_sample(rig, 1, 700, 300);  // came from a .pti: no WAV holds it
  rig.bank.mark_on_card(0);
  rig.project.last_folder = "Projects/Mine";  // the folder the project was loaded from
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK_FALSE(rig.card.has("Projects/Mine/samples/instr01.wav"));
  CHECK(rig.card.same("Projects/Mine/samples/instr02.wav", wav_of(rig, 1)));
  CHECK(rig.card.has("Projects/Mine/project.o3pj"));
  // slot 0 edited: it is written now
  put_sample(rig, 0, 610, 111);
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));
  CHECK(rig.card.violations == 0);
}

// ---- OS-BUG-OGSAVE: F4 on an OG folder leaves the WAVs it already holds alone ------------------------------------------------------------------------------------

namespace {

// The WAV of slot `slot`'s audio as the official firmware keeps it beside a .pti: a 44-byte head whose data chunk is ONE byte longer than the whole frames (a stray half frame) and has
// no pad byte after it, the RIFF size to match.
Bytes official_wav(const Rig& rig, unsigned slot, std::uint32_t rate = 44100) {
  Bytes out = wav_of(rig, slot, rate);
  const std::uint32_t data = static_cast<std::uint32_t>(out.size() - app::WavSaveSource::kHeaderBytes) + 1u;
  for (unsigned i = 0; i < 4; ++i) {
    out[4 + i] = static_cast<std::uint8_t>((36u + data) >> (8 * i));
    out[40 + i] = static_cast<std::uint8_t>(data >> (8 * i));
  }
  out.push_back(0x5A);
  return out;
}

// An OG folder Projects/t-v16 (project.mt) the project was loaded from, with the audio of slots 0 and 1 from their .pti (the bank is told of no WAV) and the official WAVs of both beside them.
void og_folder(Rig& rig) {
  put_sample(rig, 0, 600, 100);
  put_sample(rig, 1, 700, 300);
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/t-v16");
  rig.card.make_dir_now("Projects/t-v16/samples");
  rig.card.put("Projects/t-v16/project.mt", Bytes(100, 7));
  rig.card.put("Projects/t-v16/samples/instr01.wav", official_wav(rig, 0));
  rig.card.put("Projects/t-v16/samples/instr02.wav", official_wav(rig, 1));
  rig.project.last_folder = "Projects/t-v16";
}

}  // namespace

TEST_CASE("save samples: F4 on an OG folder whose WAVs already hold the audio of the .pti writes the project file only, the WAVs are not touched (OS-BUG-OGSAVE)") {
  Rig rig;
  og_folder(rig);
  const Bytes w0 = official_wav(rig, 0);
  const Bytes w1 = official_wav(rig, 1);
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE T-V16 DONE");
  CHECK(rig.card.same("Projects/t-v16/samples/instr01.wav", w0));  // odd data chunk, no pad: as the official firmware wrote it
  CHECK(rig.card.same("Projects/t-v16/samples/instr02.wav", w1));
  CHECK_FALSE(rig.card.has("Projects/t-v16/samples/instr01.wav.bak"));
  CHECK_FALSE(rig.card.has("Projects/t-v16/samples/instr02.wav.bak"));
  CHECK(sample_writes(rig) == 0u);  // no folder, temp, rename or remove in samples/
  CHECK(rig.card.same("Projects/t-v16/project.o3pj", rig.native_bytes()));
  CHECK(rig.card.same("Projects/t-v16/project.mt", Bytes(100, 7)));
  CHECK(rig.card.violations == 0);
  // a second SAVE: the belief is recorded, the files are not read again
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(sample_writes(rig) == 0u);
  CHECK(rig.card.same("Projects/t-v16/samples/instr01.wav", w0));
}

TEST_CASE("save samples: an OG folder's WAV that is not this audio is replaced with a .bak: another sample, another rate, another length, one frame different at the very end, a read that fails (OS-BUG-OGSAVE)") {
  for (unsigned c = 0; c < 6; ++c) {
    CAPTURE(c);
    Rig rig;
    og_folder(rig);
    Bytes theirs = official_wav(rig, 0);
    switch (c) {
      case 0: put_sample(rig, 0, 600, 555); break;                                     // the sample was edited since the load: another audio of the same length
      case 1: theirs = official_wav(rig, 0, 22050); break;                              // another rate
      case 2: theirs.resize(theirs.size() - 2u * 40u); break;                           // shorter (the data chunk says more than the file has)
      case 3: theirs = wav_of(rig, 0); theirs.insert(theirs.end(), 2u * 3u, 0); {       // a data chunk of three frames more than the bank's
          const std::uint32_t data = static_cast<std::uint32_t>(theirs.size() - 44u);
          for (unsigned i = 0; i < 4; ++i) { theirs[40 + i] = static_cast<std::uint8_t>(data >> (8 * i)); }
        } break;
      case 4: theirs[theirs.size() - 3] ^= 0x01; break;                                // the last frame differs
      default: break;                                                                  // 5: the file cannot be read
    }
    rig.card.put("Projects/t-v16/samples/instr01.wav", theirs);
    if (c == 5) {
      rig.card.read_fault = "projects/t-v16/samples/instr01.wav";
    }
    const Bytes w1 = official_wav(rig, 1);
    rig.go(ScreenId::File);
    rig.tap(Button::F4);
    rig.settle();
    CHECK(rig.message() == "SAVE T-V16 DONE");
    CHECK(rig.card.same("Projects/t-v16/samples/instr01.wav", wav_of(rig, 0)));
    CHECK(rig.card.same("Projects/t-v16/samples/instr01.wav.bak", theirs));
    CHECK(rig.card.same("Projects/t-v16/samples/instr02.wav", w1));  // the other slot is still left alone
    CHECK_FALSE(rig.card.has("Projects/t-v16/samples/instr02.wav.bak"));
    CHECK(rig.card.same("Projects/t-v16/project.o3pj", rig.native_bytes()));
    CHECK(rig.card.violations == 0);
  }
}

TEST_CASE("save samples: a WAV of an OG folder that is missing is written, with no .bak, beside the ones left alone (OS-BUG-OGSAVE)") {
  Rig rig;
  og_folder(rig);
  rig.card.files.erase("projects/t-v16/samples/instr02.wav");
  const Bytes w0 = official_wav(rig, 0);
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/t-v16/samples/instr01.wav", w0));
  CHECK(rig.card.same("Projects/t-v16/samples/instr02.wav", wav_of(rig, 1)));
  CHECK_FALSE(rig.card.has("Projects/t-v16/samples/instr02.wav.bak"));
  CHECK_FALSE(rig.card.has("Projects/t-v16/samples/instr01.wav.bak"));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save samples: the native reopen of an OG folder saved in place has the same frames, from the WAVs the OG left (OS-BUG-OGSAVE)") {
  Rig rig;
  og_folder(rig);
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  NEED(rig.card.has("Projects/t-v16/project.o3pj"));
  const app::SampleBank::Pcm a0 = rig.bank.pcm(0);
  const app::SampleBank::Pcm a1 = rig.bank.pcm(1);
  model::ModelArena& arena2 = test::arena(2);
  app::NativeFolderLoader native;
  NEED(native.begin(rig.card, "Projects/t-v16", arena2));
  for (unsigned i = 0; i < 2000 && native.state() == app::NativeFolderLoader::State::Running; ++i) {
    (void)native.step(4096);
  }
  NEED(native.state() == app::NativeFolderLoader::State::Done);
  auto voices = std::make_unique<audio::VoicePool>();
  std::vector<std::int16_t> memory2(60000);
  auto mem2 = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank2;
  bank2.init(*voices, arena2, *mem2, memory2.data(), static_cast<std::uint32_t>(memory2.size()));
  app::OgSampleLoader loader;
  NEED(loader.begin(rig.card, "Projects/t-v16", arena2.project, 0, *voices, memory2.data(), static_cast<std::uint32_t>(memory2.size()), audio::LevelLaw::Og12, &bank2));
  for (unsigned i = 0; i < 2000 && loader.state() == app::OgSampleLoader::State::Running; ++i) {
    (void)loader.step(4096);
  }
  CHECK(loader.report().loaded == 2u);
  const app::SampleBank::Pcm b0 = bank2.pcm(0);
  const app::SampleBank::Pcm b1 = bank2.pcm(1);
  NEED(b0.frames == a0.frames);
  NEED(b1.frames == a1.frames);
  CHECK(std::memcmp(b0.data, a0.data, a0.frames * 2u) == 0);
  CHECK(std::memcmp(b1.data, a1.data, a1.frames * 2u) == 0);
}

TEST_CASE("atomic save: a Replace save with a held check ends Done with nothing written when the file holds the audio, reads chunk by chunk, and a head with extra chunks still counts (OS-BUG-OGSAVE)") {
  Rig rig;
  put_sample(rig, 0, 3000, 100);
  const app::SampleBank::Pcm pcm = rig.bank.pcm(0);
  // the same audio behind a LIST chunk (odd size, so a pad byte) and an extra 'fmt ' tail: not byte for byte what the writer makes
  Bytes plain = wav_of(rig, 0);
  Bytes odd(plain.begin(), plain.begin() + 36);
  const char list[] = {'L', 'I', 'S', 'T', 5, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 0};
  odd.insert(odd.end(), list, list + sizeof list);
  odd.insert(odd.end(), plain.begin() + 36, plain.end());
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/P");
  rig.card.make_dir_now("Projects/P/samples");
  rig.card.put("Projects/P/samples/instr01.wav", odd);
  app::WavSaveSource source;
  NEED(source.set(pcm.data, pcm.frames, 1, 44100, nullptr, nullptr));
  app::AtomicSave job;
  NEED(job.begin(rig.card, rig.card, "Projects/P/samples", "instr01.wav", app::AtomicSave::Mode::Replace, source, "X"));
  job.set_held(&source);
  unsigned steps = 0;
  while (job.running() && steps++ < 1000u) {
    job.step(100);
  }
  CHECK(job.done());
  CHECK(job.unchanged());
  CHECK(steps > 2u);  // read in slices, not at once
  CHECK(rig.card.same("Projects/P/samples/instr01.wav", odd));
  CHECK_FALSE(rig.card.has("Projects/P/samples/instr01.wav.bak"));
  for (const std::string& line : rig.card.log) {
    CHECK(line.rfind("create", 0) != 0u);
    CHECK(line.rfind("rename", 0) != 0u);
  }
  // one byte of the audio differs in the file: not held, the file is replaced and the old one kept
  rig.card.log.clear();
  rig.card.put("Projects/P/samples/instr01.wav", plain);
  rig.card.files["projects/p/samples/instr01.wav"][plain.size() - 1] ^= 0x7F;
  NEED(job.begin(rig.card, rig.card, "Projects/P/samples", "instr01.wav", app::AtomicSave::Mode::Replace, source, "X"));
  job.set_held(&source);
  while (job.running() && steps++ < 2000u) {
    job.step(100);
  }
  CHECK(job.done());
  CHECK_FALSE(job.unchanged());
  CHECK(rig.card.same("Projects/P/samples/instr01.wav", plain));
  CHECK(rig.card.has("Projects/P/samples/instr01.wav.bak"));
}

// FQA-10a (review of OS-BUG-OGSAVE): the held check's edges. A file that only looks like the audio is replaced, with the old one kept, and nothing is left open.
TEST_CASE("atomic save: the held check does not count a stereo file with a mono bank's bytes, a file that ends inside the audio, or a cancelled read as held (FQA-10a)") {
  Rig rig;
  put_sample(rig, 0, 3000, 100);
  const app::SampleBank::Pcm pcm = rig.bank.pcm(0);
  const Bytes plain = wav_of(rig, 0);
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/P");
  rig.card.make_dir_now("Projects/P/samples");
  app::WavSaveSource source;
  NEED(source.set(pcm.data, pcm.frames, 1, 44100, nullptr, nullptr));
  const auto run = [&](app::AtomicSave& job) {
    NEED(job.begin(rig.card, rig.card, "Projects/P/samples", "instr01.wav", app::AtomicSave::Mode::Replace, source, "X"));
    job.set_held(&source);
    unsigned steps = 0;
    while (job.running() && steps++ < 4000u) {
      job.step(100);
    }
    CHECK(steps < 4000u);
  };
  // 1) the same data bytes behind a stereo head (channels 2, block align 4, byte rate 176,400): the bytes match, the format does not
  Bytes stereo = plain;
  stereo[22] = 2;
  stereo[28] = 0x10;
  stereo[29] = 0xB1;
  stereo[30] = 0x02;
  stereo[31] = 0;
  stereo[32] = 4;
  rig.card.put("Projects/P/samples/instr01.wav", stereo);
  {
    app::AtomicSave job;
    run(job);
    CHECK(job.done());
    CHECK_FALSE(job.unchanged());
    CHECK(rig.card.same("Projects/P/samples/instr01.wav", plain));
    CHECK(rig.card.same("Projects/P/samples/instr01.wav.bak", stereo));
    CHECK(rig.card.open_handles == 0);
  }
  // 2) the EOF branch: a file of exactly the smallest size a WAV of this audio can have (plain's own size) whose head has an extra chunk, so the file ends 16 bytes inside the audio: the read
  //    that answers 0 bytes is "does not hold it" and the file is written over (no hang, nothing read past the end)
  Bytes short_file(plain.begin(), plain.begin() + 36);
  const char list[] = {'L', 'I', 'S', 'T', 8, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  short_file.insert(short_file.end(), list, list + sizeof list);
  short_file.insert(short_file.end(), plain.begin() + 36, plain.end() - static_cast<std::ptrdiff_t>(sizeof list));
  NEED(short_file.size() == plain.size());
  rig.card.files.erase("projects/p/samples/instr01.wav.bak");
  rig.card.put("Projects/P/samples/instr01.wav", short_file);
  {
    app::AtomicSave job;
    run(job);
    CHECK(job.done());
    CHECK_FALSE(job.unchanged());
    CHECK(rig.card.same("Projects/P/samples/instr01.wav", plain));
    CHECK(rig.card.same("Projects/P/samples/instr01.wav.bak", short_file));
    CHECK(rig.card.open_handles == 0);
  }
  // 3) a cancel while the file is being read: nothing is written, the file stays as it was, the read handle is closed, the job says cancelled (no error)
  rig.card.files.erase("projects/p/samples/instr01.wav.bak");
  rig.card.put("Projects/P/samples/instr01.wav", plain);
  rig.card.log.clear();
  {
    app::AtomicSave job;
    NEED(job.begin(rig.card, rig.card, "Projects/P/samples", "instr01.wav", app::AtomicSave::Mode::Replace, source, "X"));
    job.set_held(&source);
    unsigned steps = 0;
    while (job.running() && job.progress() == 0u && steps++ < 100u) {
      job.step(100);
    }
    NEED(job.running());
    NEED(rig.card.open_handles == 1);  // CompareRead: the file is open, a little of it checked
    job.cancel();
    CHECK(job.failed());
    CHECK(job.error() == app::AtomicSave::Error::None);
    CHECK_FALSE(job.unchanged());
    CHECK(rig.card.open_handles == 0);
    CHECK(rig.card.same("Projects/P/samples/instr01.wav", plain));
    CHECK_FALSE(rig.card.has("Projects/P/samples/instr01.wav.bak"));
    for (const std::string& line : rig.card.log) {
      CHECK(line.rfind("create", 0) != 0u);
      CHECK(line.rfind("rename", 0) != 0u);
      CHECK(line.rfind("remove", 0) != 0u);
    }
  }
}

// FQA-10a: the progress runs from the compare read (0 .. 5 %) into the write (from 5 %) without a step back, also when the compare ends on its last frame and the file is written over.
TEST_CASE("atomic save: the progress never goes back from the held check's read into the write (FQA-10a)") {
  for (unsigned c = 0; c < 2; ++c) {  // 0: the very last byte differs (the whole file was read first), 1: the first audio byte differs (the read stops at once)
    CAPTURE(c);
    Rig rig;
    put_sample(rig, 0, 3000, 100);
    const app::SampleBank::Pcm pcm = rig.bank.pcm(0);
    Bytes theirs = wav_of(rig, 0);
    theirs[c == 0u ? theirs.size() - 1u : 44u] ^= 0x7F;
    rig.card.make_dir_now("Projects");
    rig.card.make_dir_now("Projects/P");
    rig.card.make_dir_now("Projects/P/samples");
    rig.card.put("Projects/P/samples/instr01.wav", theirs);
    app::WavSaveSource source;
    NEED(source.set(pcm.data, pcm.frames, 1, 44100, nullptr, nullptr));
    app::AtomicSave job;
    NEED(job.begin(rig.card, rig.card, "Projects/P/samples", "instr01.wav", app::AtomicSave::Mode::Replace, source, "X"));
    job.set_held(&source);
    unsigned last = 0;
    unsigned steps = 0;
    unsigned peak_compare = 0;
    while (job.running() && steps++ < 4000u) {
      job.step(100);
      const unsigned now = job.progress();
      CHECK_MESSAGE(now >= last, "step " << steps << ": " << last << " -> " << now);
      if (job.bytes() == 0u && now < 50u) {
        peak_compare = now > peak_compare ? now : peak_compare;
      }
      last = now;
    }
    CHECK(job.done());
    CHECK_FALSE(job.unchanged());
    if (c == 0u) {
      CHECK(peak_compare > 40u);  // the compare really did read to the end before the write began
    }
  }
}

TEST_CASE("save samples: a path outside samples/ or a name the card refuses is skipped, the rest is saved; a sample that fails stops the save before the project file") {
  Rig rig;
  put_sample(rig, 0, 600, 100, "elsewhere/x.wav");
  put_sample(rig, 1, 600, 200, "samples/CON.wav");
  put_sample(rig, 2, 600, 300);
  save_as(rig, "Mine");
  CHECK(rig.card.has("Projects/Mine/samples/instr03.wav"));
  CHECK_FALSE(rig.card.has("Projects/Mine/samples/con.wav"));
  CHECK(rig.card.has("Projects/Mine/project.o3pj"));
  // the audio changes while a sample is written: that file fails ("CHANGED WHILE SAVING") and the project file is not written
  Rig rig2;
  put_sample(rig2, 0, 3000, 100);
  put_sample(rig2, 1, 3000, 200);
  rig2.go(ScreenId::File);
  rig2.tap(Button::F5);
  auto& entry = const_cast<app::TextEntry&>(rig2.app->state().text.entry);
  entry.set("Cut");
  rig2.tap(Button::F8);
  rig2.pass();
  put_sample(rig2, 0, 3000, 999);  // the first sample's audio is replaced under the job
  rig2.settle();
  CHECK_FALSE(rig2.card.has("Projects/Cut/project.o3pj"));
  CHECK_FALSE(rig2.card.has("Projects/Cut/samples/instr02.wav"));  // the plan stopped
  CHECK_FALSE(rig2.card.has("Projects/Cut/samples/_o3save.tmp"));
  CHECK(rig2.card.violations == 0);
  CHECK_FALSE(rig2.saves.busy());
}

TEST_CASE("save samples: no other save starts between the jobs of one, and a card that goes read-only drops the rest") {
  Rig rig;
  put_sample(rig, 0, 3000, 100);
  put_sample(rig, 1, 3000, 200);
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  rig.tap(Button::F8);
  CHECK(rig.saves.busy());
  rig.expire();
  rig.tap(Button::F4);  // a second save while the first runs
  bool said = false;
  for (unsigned i = 0; i < 100 && !said; ++i) {
    said = rig.message() == "A SAVE IS ALREADY RUNNING";
    if (!said) {
      rig.pass();
    }
  }
  CHECK(said);
  rig.card.gate_status = hal::storage::WriteStatus::WriteProtected;  // the card is taken away while the first job runs
  rig.settle();
  CHECK_FALSE(rig.saves.busy());
  CHECK_FALSE(rig.card.has("Projects/Track 01/project.o3pj"));
}

TEST_CASE("save samples: a project with audio that lives in no WAV (a .pti's) reopens from the native folder WITH its samples") {
  Rig rig;
  put_sample(rig, 0, 1200, 321);
  put_sample(rig, 3, 800, -1500);
  save_as(rig, "Reopen");
  NEED(rig.card.has("Projects/Reopen/project.o3pj"));
  const app::SampleBank::Pcm a0 = rig.bank.pcm(0);
  const app::SampleBank::Pcm a3 = rig.bank.pcm(3);
  // reopen it the way the device does: the native project, then the sample loader over the same folder
  model::ModelArena& arena2 = test::arena(2);
  app::NativeFolderLoader native;
  NEED(native.begin(rig.card, "Projects/Reopen", arena2));
  for (unsigned i = 0; i < 2000 && native.state() == app::NativeFolderLoader::State::Running; ++i) {
    (void)native.step(4096);
  }
  NEED(native.state() == app::NativeFolderLoader::State::Done);
  NEED(arena2.project.instrument(0) != nullptr);
  CHECK(std::string(arena2.project.instrument(0)->sample_path) == "samples/instr01.wav");
  auto voices = std::make_unique<audio::VoicePool>();
  std::vector<std::int16_t> memory2(60000);
  auto mem2 = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank2;
  bank2.init(*voices, arena2, *mem2, memory2.data(), static_cast<std::uint32_t>(memory2.size()));
  app::OgSampleLoader loader;
  NEED(loader.begin(rig.card, "Projects/Reopen", arena2.project, 0, *voices, memory2.data(), static_cast<std::uint32_t>(memory2.size()), audio::LevelLaw::Og12, &bank2));
  for (unsigned i = 0; i < 2000 && loader.state() == app::OgSampleLoader::State::Running; ++i) {
    (void)loader.step(4096);
  }
  CHECK(loader.report().loaded == 2u);
  CHECK(loader.report().missing == 0u);
  const app::SampleBank::Pcm b0 = bank2.pcm(0);
  const app::SampleBank::Pcm b3 = bank2.pcm(3);
  NEED(b0.frames == a0.frames);
  NEED(b3.frames == a3.frames);
  CHECK(std::memcmp(b0.data, a0.data, a0.frames * 2u) == 0);
  CHECK(std::memcmp(b3.data, a3.data, a3.frames * 2u) == 0);
  // and a Save of the reopened project writes no sample again: the loader read them from this folder
  CHECK(bank2.card_version(0) == bank2.version(0));
  CHECK(bank2.card_version(3) == bank2.version(3));
}

// OS-RATEPITCH: a sample of another rate than 44,100 Hz (a 16,726 Hz 8SVX, an Amiga conversion's 8,287 Hz, a 48 and a 96 kHz one) is saved as a WAV with its rate in the header, and a
// reopen of the native folder must load it as it is: the instrument's rate, the factor the voices scale the pitch by, and the pitch that comes out.
namespace {

// Slot `slot`: a sample instrument of the rate `rate`, the engine `engine` (model::kInstEngine*) and the play mode `play_mode`, whose audio is a sine of 40 frames a period (2000 frames).
void put_sine(Rig& rig, unsigned slot, std::uint32_t rate, std::uint8_t engine, std::uint8_t play_mode = 0) {
  model::Project& project = test::arena(1).project;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  std::snprintf(in.sample_path, sizeof in.sample_path, "samples/instr%02u.wav", slot + 1u);
  in.sample_rate = rate;
  in.engine = engine;
  in.sampler.play_mode = play_mode;
  NEED(project.set_instrument(slot, in) == model::Status::Ok);
  std::int16_t* pcm = rig.memory.data() + slot * 4000u;
  for (std::uint32_t i = 0; i < 2000u; ++i) {
    pcm[i] = static_cast<std::int16_t>(std::lround(20000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(i) / 40.0)));
  }
  rig.bank.adopt(slot, pcm, 2000u);
}

// The mean period, in output samples, of the sine that note 60 of `slot` plays through `voices` (the up-going zero crossings of output samples 192..640: the sample, 2000 frames, plays on past that even at 96 kHz).
double played_period(audio::VoicePool& voices, unsigned slot) {
  constexpr std::uint32_t kBlk = hal::audio::kBlock;
  constexpr std::uint32_t kBlocks = 10;
  static std::int16_t out[kBlocks * kBlk];
  voices.cut(0);
  CHECK(voices.note_on(0, 60, slot, 127));
  for (std::uint32_t b = 0; b < kBlocks; ++b) {
    voices.begin_block();
    voices.render(0, kBlk);
    voices.end_block();
    const std::int16_t* f = voices.feed(0);
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      out[b * kBlk + i] = f != nullptr ? f[i] : std::int16_t{0};
    }
  }
  double first = 0.0, last = 0.0;
  unsigned count = 0;
  for (std::uint32_t i = 3 * kBlk + 1; i < 640u; ++i) {
    if (out[i - 1] < 0 && out[i] >= 0) {
      const double t = static_cast<double>(i - 1) + static_cast<double>(-out[i - 1]) / static_cast<double>(out[i] - out[i - 1]);
      if (count == 0u) {
        first = t;
      }
      last = t;
      ++count;
    }
  }
  voices.cut(0);
  return count >= 3u ? (last - first) / static_cast<double>(count - 1u) : 0.0;
}

}  // namespace

TEST_CASE("save samples: a sample of another rate than 44,100 Hz reopens from the native folder with its rate, its factor and its pitch; an OG folder still refuses it (OS-RATEPITCH)") {
  Rig rig;
  const std::uint32_t rates[5] = {16726u, 8287u, 48000u, 96000u, 44100u};
  for (unsigned i = 0; i < 5; ++i) {
    put_sine(rig, i, rates[i], i == 1 ? model::kInstEngineAmiga : model::kInstEngineAuto);
  }
  save_as(rig, "Rates");
  NEED(rig.card.has("Projects/Rates/project.o3pj"));
  for (unsigned i = 0; i < 5; ++i) {
    char path[64];
    std::snprintf(path, sizeof path, "Projects/Rates/samples/instr%02u.wav", i + 1u);
    CHECK(rig.card.same(path, wav_of(rig, i, rates[i])));  // the file says its rate
  }
  model::ModelArena& arena2 = test::arena(2);
  app::NativeFolderLoader native;
  NEED(native.begin(rig.card, "Projects/Rates", arena2));
  for (unsigned i = 0; i < 2000 && native.state() == app::NativeFolderLoader::State::Running; ++i) {
    (void)native.step(4096);
  }
  NEED(native.state() == app::NativeFolderLoader::State::Done);
  auto voices = std::make_unique<audio::VoicePool>();
  std::vector<std::int16_t> memory2(60000);
  auto mem2 = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank2;
  bank2.init(*voices, arena2, *mem2, memory2.data(), static_cast<std::uint32_t>(memory2.size()));
  app::OgSampleLoader loader;
  NEED(loader.begin(rig.card, "Projects/Rates", arena2.project, 0, *voices, memory2.data(), static_cast<std::uint32_t>(memory2.size()), audio::LevelLaw::Og12, &bank2, true));
  for (unsigned i = 0; i < 4000 && loader.state() == app::OgSampleLoader::State::Running; ++i) {
    (void)loader.step(4096);
  }
  CHECK(loader.report().loaded == 5u);
  CHECK(loader.report().missing == 0u);
  for (unsigned i = 0; i < 5; ++i) {
    const model::Instrument* in = arena2.project.instrument(i);
    NEED(in != nullptr);
    CHECK(in->sample_rate == rates[i]);
    CHECK(voices->sample(i).length == 2000u);
    CHECK(voices->sample(i).rate_q28 == audio::rate_factor_q28(rates[i]));
    CHECK(bank2.pcm(i).frames == 2000u);
    CHECK(std::memcmp(bank2.pcm(i).data, rig.bank.pcm(i).data, 4000u) == 0);
  }
  CHECK(voices->sample(4).rate_q28 == 0u);                         // 44,100 Hz: the plain increment
  CHECK(bank2.engine_for(1, *arena2.project.instrument(1)) == audio::kEngineAmiga);  // the Amiga conversion keeps its engine and its rate
  // the pitch that comes out: 40 frames a period at the root, played at rate / 44,100 frames a sample
  for (unsigned i = 0; i < 5; ++i) {
    if (i == 1) {
      continue;  // (the Amiga engine plays on its own voice; its pitch is the period's: amiga_voices_test)
    }
    const double want = 40.0 * 44100.0 / static_cast<double>(rates[i]);
    const double got = played_period(*voices, i);
    CHECK_MESSAGE(std::fabs(got - want) < want * 0.003, "rate " << rates[i] << ": period " << got << " want " << want);
  }
  // our OG-folder loader takes 44,100 Hz only: the same files, not native, are unusable but the 44,100 Hz one
  const auto reload = [&](bool native_folder, audio::VoicePool& v) -> app::SampleLoadReport {
    app::OgSampleLoader l;
    CHECK(l.begin(rig.card, "Projects/Rates", arena2.project, 0, v, memory2.data(), static_cast<std::uint32_t>(memory2.size()), audio::LevelLaw::Og12, nullptr, native_folder));
    for (unsigned i = 0; i < 4000 && l.state() == app::OgSampleLoader::State::Running; ++i) {
      (void)l.step(4096);
    }
    return l.report();
  };
  {
    auto og = std::make_unique<audio::VoicePool>();
    const app::SampleLoadReport r = reload(false, *og);
    CHECK(r.loaded == 1u);  // the 44,100 Hz file
    CHECK(r.missing == 4u);
    CHECK(r.problems[0].why == app::SampleProblem::Unusable);
    CHECK(og->sample(4).length == 2000u);
    CHECK(og->sample(0).length == 0u);
  }
  // and a native WAV whose header rate is not the one the instrument stores (a file put there by hand) is not the sample that was saved: unusable, the rest load
  rig.card.put("Projects/Rates/samples/instr01.wav", wav_of(rig, 0, 22050));
  {
    auto nat = std::make_unique<audio::VoicePool>();
    const app::SampleLoadReport r = reload(true, *nat);
    CHECK(r.loaded == 4u);
    CHECK(r.missing == 1u);
    CHECK(nat->sample(0).length == 0u);
    CHECK(nat->sample(2).rate_q28 == audio::rate_factor_q28(48000u));
  }
}

// OS-BUG-MODENGINE: a project opened from its native folder plays the engines it was saved with, with no edit and no rebind. The folder is reopened as the platform does (the native
// loader, then the samples into a bank): the voices' descriptors are what a note plays through.
namespace {

struct Reopened {
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(60000);
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  model::ModelArena* arena = nullptr;
  app::SampleLoadReport report;
};

void reopen(Rig& rig, const char* folder, Reopened& out) {
  out.arena = &test::arena(2);
  out.bank.init(*out.voices, *out.arena, *out.mem, out.memory.data(), static_cast<std::uint32_t>(out.memory.size()));  // (first: a bank that was never init()ed cannot be destroyed)
  app::NativeFolderLoader native;
  NEED(native.begin(rig.card, folder, *out.arena));
  for (unsigned i = 0; i < 2000 && native.state() == app::NativeFolderLoader::State::Running; ++i) {
    (void)native.step(4096);
  }
  NEED(native.state() == app::NativeFolderLoader::State::Done);
  app::OgSampleLoader loader;
  NEED(loader.begin(rig.card, folder, out.arena->project, 0, *out.voices, out.memory.data(), static_cast<std::uint32_t>(out.memory.size()), audio::LevelLaw::Og12, &out.bank, true));
  for (unsigned i = 0; i < 4000 && loader.state() == app::OgSampleLoader::State::Running; ++i) {
    (void)loader.step(4096);
  }
  out.report = loader.report();
}

}  // namespace

TEST_CASE("save samples: the engine an instrument was saved with plays at the load, with no edit: AMIGA, 12-BIT, 8-BIT, MODERN and the AUTO rule (OS-BUG-MODENGINE)") {
  Rig rig;
  struct Row {
    std::uint8_t engine;  // model::kInstEngine*
    std::uint32_t rate;
    std::uint8_t want;  // audio::kEngine*
    std::uint8_t mode;  // the sampler's play mode (0 one shot, 1 forward loop, above 1 not what Paula plays)
  };
  const Row rows[] = {
      {model::kInstEngineAmiga, 44100u, audio::kEngineAmiga, 0},    // (a) the user's choice
      {model::kInstEngineBits12, 44100u, audio::kEngineBits12, 0},  // (a)
      {model::kInstEngineBits8, 44100u, audio::kEngineBits8, 0},    // (a)
      {model::kInstEngineModern, 8363u, audio::kEngineModern, 0},   // an explicit MODERN stays Modern at a Paula rate
      {model::kInstEngineAuto, 8363u, audio::kEngineAmiga, 0},      // (b) the AUTO rule: the NTSC Paula rate
      {model::kInstEngineAuto, 8287u, audio::kEngineAmiga, 1},      // (b) the PAL Paula rate, a forward loop
      {model::kInstEngineAuto, 44100u, audio::kEngineModern, 0},    // AUTO at any other rate
      {model::kInstEngineAuto, 8363u, audio::kEngineModern, 3},     // AUTO at a Paula rate in a play mode Paula does not play (ping-pong): Modern, the `plain` condition of engine_for
  };
  constexpr unsigned kRows = sizeof rows / sizeof rows[0];
  for (unsigned i = 0; i < kRows; ++i) {
    put_sine(rig, i, rows[i].rate, rows[i].engine, rows[i].mode);
  }
  save_as(rig, "Engines");
  NEED(rig.card.has("Projects/Engines/project.o3pj"));
  Reopened re;
  reopen(rig, "Projects/Engines", re);
  CHECK(re.report.loaded == kRows);
  for (unsigned i = 0; i < kRows; ++i) {
    const model::Instrument* in = re.arena->project.instrument(i);
    NEED(in != nullptr);
    CHECK(in->engine == rows[i].engine);  // the stored byte is read as it was written
    CHECK_MESSAGE(re.voices->sample(i).length == 2000u, "slot " << i);
    CHECK_MESSAGE(re.voices->sample(i).engine == rows[i].want, "slot " << i << ": engine " << static_cast<unsigned>(re.voices->sample(i).engine) << " want " << static_cast<unsigned>(rows[i].want));
    CHECK_MESSAGE(re.voices->sample(i).engine == re.bank.engine_for(i, *in), "slot " << i);  // what a rebind would give it
  }
}

TEST_CASE("save samples: an imported MOD plays on the Amiga engine after the import and after a save and reload; an S3M imported for the Modern engine stays Modern (OS-BUG-MODENGINE, QA-FIL-005, QA-FIL-006)") {
  Rig rig;
  test::modimp::ModSpec m;
  m.name = "kc test";
  m.samples[0].name = "kick";
  for (unsigned i = 0; i < 64; ++i) {
    m.samples[0].data.push_back(static_cast<std::uint8_t>((i * 3u) & 0x7Fu));
  }
  m.samples[0].volume = 64;
  m.patterns[0][{0, 0}] = test::modimp::note(12, 1);
  rig.card.put("MOD/kc_test.mod", test::modimp::build(m));
  app::ModuleImport import;
  NEED(import.begin(rig.card, "MOD/kc_test.mod", test::arena(1), app::ImportEngine::Auto) == app::ModuleImport::Start::Started);
  for (unsigned i = 0; i < 100000 && import.state() == app::ModuleImport::State::Running; ++i) {
    (void)import.step(16384);
  }
  NEED(import.state() == app::ModuleImport::State::Done);
  audio::VoicePool& pool = make_pool();  // (make() builds the pool again: one call)
  app::OgSampleLoader loader;
  NEED(loader.begin_feed(import, test::arena(1).project, pool, rig.memory.data(), static_cast<std::uint32_t>(rig.memory.size()), audio::LevelLaw::Og12, &rig.bank));
  for (unsigned i = 0; i < 100000 && loader.state() == app::OgSampleLoader::State::Running; ++i) {
    (void)loader.step(16384);
  }
  NEED(loader.state() == app::OgSampleLoader::State::Done);
  import.samples_done(loader.report());
  const model::Instrument* in = test::arena(1).project.instrument(0);
  NEED(in != nullptr);
  CHECK(in->engine == model::kInstEngineAuto);                            // the importer writes AUTO for a module sample ...
  CHECK((in->sample_rate == 8287u || in->sample_rate == 8363u));          // ... at a Paula rate
  CHECK(pool.sample(0).engine == audio::kEngineAmiga);                    // right after the import
  save_as(rig, "Imported");
  NEED(rig.card.has("Projects/Imported/project.o3pj"));
  Reopened re;
  reopen(rig, "Projects/Imported", re);
  const model::Instrument* back = re.arena->project.instrument(0);
  NEED(back != nullptr);
  CHECK(back->engine == model::kInstEngineAuto);
  CHECK(re.voices->sample(0).length != 0u);
  CHECK(re.voices->sample(0).engine == audio::kEngineAmiga);              // and after a save and reload
}

// OS-SAVEABORT: an Open (or a New) while a save runs used to replace the document under it; the platform's end of the load dropped the save's plan, and the first save of a new
// project left a folder with some of its samples and no project file. Now neither starts while the save runs: the save is not touched, the user is told, and Open works after it.
TEST_CASE("save: Open and New are refused while a save runs, the save completes whole, and Open works after it (OS-SAVEABORT)") {
  Rig rig;
  put_sample(rig, 0, 3000, 100);
  put_sample(rig, 1, 3000, 200);
  put_sample(rig, 2, 3000, 300);
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/Old");
  rig.card.put("Projects/Old/project.o3pj", rig.native_bytes());  // something to open under the cursor
  rig.go(ScreenId::File);
  rig.settle();
  rig.project.open_result = app::ProjectSink::Start::Started;
  rig.tap(Button::F5);  // SAVE AS of a new project: three samples, then the project file
  auto& entry = const_cast<app::TextEntry&>(rig.app->state().text.entry);
  entry.set("Fresh");
  rig.tap(Button::F8);
  NEED(rig.saves.busy());
  unsigned asked = 0;
  for (unsigned i = 0; i < 4000 && rig.saves.busy(); ++i) {
    (void)rig.app->invoke(core::action_id("file.open"));
    if (asked == 0u) {  // the user is told (the strip shows the save's progress first; the refusal is queued behind it)
      bool told = false;
      for (unsigned n = 0; n < rig.app->state().notify.size(); ++n) {
        told = told || std::string(rig.app->state().notify.at(n).text.c_str()) == "A SAVE IS RUNNING: TRY AGAIN WHEN IT ENDS";
      }
      CHECK(told);
    }
    (void)rig.app->invoke(core::action_id("file.new"));
    ++asked;
    rig.pass();
  }
  CHECK(asked >= 3u);              // it was asked during the samples and during the project file
  CHECK(rig.project.opens == 0u);  // nothing reached the platform's load
  CHECK(rig.project.news == 0u);
  rig.settle();
  CHECK(rig.card.same("Projects/Fresh/project.o3pj", rig.native_bytes()));
  for (unsigned slot = 0; slot < 3; ++slot) {
    char path[48];
    std::snprintf(path, sizeof path, "Projects/Fresh/samples/instr%02u.wav", slot + 1u);
    CHECK(rig.card.same(path, wav_of(rig, slot)));
  }
  CHECK(rig.card.violations == 0);
  CHECK_FALSE(rig.saves.busy());
  rig.expire();
  (void)rig.app->invoke(core::action_id("file.open"));  // the save is over: the same key opens
  CHECK(rig.project.opens == 1u);
  CHECK(rig.project.last_folder.rfind("Projects/", 0) == 0);
  CHECK(rig.app->state().file.opening);
}

// OS-OPENGUARD: F1 OPEN throws the changes of the project away like F3 NEW and the import's F1 do, so with changes no save has taken it asks first (the same confirm_new).
TEST_CASE("open: with unsaved changes the first F1 only asks, the second opens; a clean project opens at once; a move between the presses asks again (OS-OPENGUARD)") {
  Rig rig;
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/Old");
  rig.card.make_dir_now("Projects/Other");
  rig.card.put("Projects/Old/project.o3pj", rig.native_bytes());
  rig.card.put("Projects/Other/project.o3pj", rig.native_bytes());
  rig.go(ScreenId::File);
  rig.settle();
  rig.project.open_result = app::ProjectSink::Start::Started;
  // Clean: opens at once.
  rig.tap(Button::F1);
  CHECK(rig.project.opens == 1u);
  CHECK_FALSE(rig.app->state().file.confirm_new);
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  CHECK(rig.app->state().screen == ScreenId::Pattern);  // OS-OPENTOPAT: a project that opened shows the Pattern view
  rig.back_to_file();
  // Dirty: the first press asks and loads nothing, the second opens.
  model::Project& project = test::arena(1).project;
  CHECK(project.set_name("edited", 6) == model::Status::Ok);
  rig.tap(Button::F1);
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  CHECK(rig.project.opens == 1u);
  CHECK(rig.app->state().file.confirm_new);
  CHECK(test::read_row(*rig.app).label[0] == "OPEN?");  // OS-KEYROWDLG: the second F1 is marked
  CHECK(test::read_row(*rig.app).hot == 0x01u);
  rig.expire();
  CHECK_FALSE(test::read_row(*rig.app).marked);  // its text ran out of the strip: the key reads OPEN again
  rig.tap(Button::F1);
  CHECK(rig.project.opens == 2u);
  CHECK_FALSE(rig.app->state().file.confirm_new);
  rig.project.is_busy = false;
  rig.pass();
  rig.expire();
  rig.back_to_file();
  // A cursor move between the two presses takes the question back: it asks again.
  CHECK(project.set_name("edited again", 12) == model::Status::Ok);
  rig.tap(Button::F1);
  CHECK(rig.app->state().file.confirm_new);
  rig.tap(Button::Down);
  CHECK_FALSE(rig.app->state().file.confirm_new);
  rig.tap(Button::F1);
  CHECK(rig.project.opens == 2u);
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  // F6 (page change) and leaving the screen take it back too.
  rig.tap(Button::F6);
  CHECK_FALSE(rig.app->state().file.confirm_new);
  rig.tap(Button::F6);
  rig.tap(Button::F1);
  CHECK(rig.app->state().file.confirm_new);
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::File);
  CHECK_FALSE(rig.app->state().file.confirm_new);
  CHECK(rig.project.opens == 2u);
}

// OS-NEWKEEP2: the guard's question leaves the strip when it is taken back, as NEW's does.
TEST_CASE("open: the unsaved-changes question leaves the strip at once when it is taken back, and stays when it is answered into an open (OS-NEWKEEP2)") {
  Rig rig;
  rig.card.make_dir_now("Projects");
  rig.card.make_dir_now("Projects/Old");
  rig.card.put("Projects/Old/project.o3pj", rig.native_bytes());
  rig.go(ScreenId::File);
  rig.settle();
  rig.project.open_result = app::ProjectSink::Start::Started;
  CHECK(test::arena(1).project.set_name("edited", 6) == model::Status::Ok);
  rig.tap(Button::F1);
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  rig.tap(Button::Down);  // a move
  CHECK(rig.message().empty());
  rig.tap(Button::F1);
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  rig.tap(Button::F2);  // another key
  CHECK(rig.message().empty());
  rig.tap(Button::F1);
  CHECK(rig.message() == "UNSAVED CHANGES: F1 AGAIN TO DISCARD THEM");
  rig.go(ScreenId::Pattern);  // leaving the screen
  CHECK(rig.message().empty());
  CHECK(rig.project.opens == 0u);
}

// ---- OS-NEWKEEP: File > NEW, ALL or KEEP INSTRUMENTS --------------------------------------------------------------------------------------------------------------
// The device does the replacing in phases (platform/mk66 app_main.cpp: start_new_project, begin_samples); the pieces it runs are these: the native loader's fresh form (the Project phase) and the
// sample bank's new_project (the Samples phase). run_new() runs them in that order, then tells the App what the platform tells it when the load ends.

namespace {

struct InstrumentTable {
  std::vector<model::Instrument> in;  // the 64 slots (a free one is a default Instrument with kind None)
  std::vector<bool> used;
  std::vector<model::ModuleRef> modules;
};

InstrumentTable table_of(const model::Project& project) {
  InstrumentTable t;
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = project.instrument(i);
    t.used.push_back(in != nullptr);
    t.in.push_back(in != nullptr ? *in : model::Instrument{});
  }
  for (unsigned i = 0; i < project.module_count(); ++i) {
    t.modules.push_back(*project.module(i));
  }
  return t;
}

bool same_table(const InstrumentTable& a, const InstrumentTable& b) {
  if (a.used != b.used || a.modules.size() != b.modules.size()) {
    return false;
  }
  for (unsigned i = 0; i < a.in.size(); ++i) {
    if (a.used[i] && std::memcmp(&a.in[i], &b.in[i], sizeof(model::Instrument)) != 0) {
      return false;
    }
  }
  return a.modules.empty() || std::memcmp(a.modules.data(), b.modules.data(), a.modules.size() * sizeof(model::ModuleRef)) == 0;
}

void run_new(Rig& rig, bool keep) {
  model::ModelArena& arena = test::arena(1);
  app::NativeFolderLoader native;
  native.begin_fresh(arena, keep);
  for (unsigned i = 0; i < 10 && native.state() == app::NativeFolderLoader::State::Running; ++i) {
    (void)native.step(4096);
  }
  NEED(native.state() == app::NativeFolderLoader::State::Done);
  CHECK(native.keeps_instruments() == keep);
  rig.bank.new_project(keep);
  rig.editor->history_reset();
  rig.app->attach_document(rig.editor.get());
  rig.app->document_replaced();
}

// Two sample instruments with audio (slots 0 and 3, the second a module-less one) over whatever the fixture has, the audio marked as read from a project folder, the arena in use.
void fill_instruments(Rig& rig) {
  put_sample(rig, 0, 1200, 321);
  put_sample(rig, 3, 800, -1500);
  rig.bank.mark_on_card(0);
  rig.bank.mark_on_card(3);
  rig.bank.set_used(3 * 4000u + 800u);
}

}  // namespace

TEST_CASE("new ALL: no instrument, no module and no sample memory in use afterwards; the patterns, the song and the title are the defaults (OS-NEWKEEP)") {
  Rig rig;
  fill_instruments(rig);
  model::Project& project = test::arena(1).project;
  NEED(project.instrument_count() >= 2u);
  NEED(project.module_count() > 0u);  // the fixture has a Module instrument and the module it names
  NEED(rig.bank.free_samples() < rig.bank.arena_samples());
  CHECK(project.set_name("title", 5) == model::Status::Ok);
  run_new(rig, false);
  CHECK(project.instrument_count() == 0u);
  for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
    CHECK(project.instrument(i) == nullptr);
    CHECK(rig.bank.pcm(i).data == nullptr);
    CHECK(rig.bank.pcm(i).frames == 0u);
  }
  CHECK(project.module_count() == 0u);
  CHECK(rig.bank.free_samples() == rig.bank.arena_samples());  // the sample memory is free again
  CHECK(project.pattern_count() == 1u);
  CHECK(project.song_length() == 1u);
  CHECK(project.order(0) == 1u);
  CHECK(std::string(project.name()).empty());
  // and a save writes a project with no sample at all
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.has("Projects/Track 01/project.o3pj"));
  CHECK_FALSE(rig.card.has("Projects/Track 01/samples/instr01.wav"));
  CHECK(rig.card.same("Projects/Track 01/project.o3pj", rig.native_bytes()));
}

TEST_CASE("new KEEP INSTRUMENTS: every instrument (sample and module) and its audio is byte-identical; the patterns, the song and the title are emptied (OS-NEWKEEP)") {
  Rig rig;
  fill_instruments(rig);
  model::Project& project = test::arena(1).project;
  const InstrumentTable before = table_of(project);
  CHECK(project.set_name("title", 5) == model::Status::Ok);
  // a pattern with notes, a second pattern and a longer song: all of it goes
  CHECK(project.create_pattern(100, 32) == model::Status::Ok);
  CHECK(project.append_order(100) == model::Status::Ok);
  const std::uint32_t patterns_before = project.pattern_count();
  NEED(patterns_before >= 2u);
  const app::SampleBank::Pcm a0 = rig.bank.pcm(0);
  const app::SampleBank::Pcm a3 = rig.bank.pcm(3);
  const std::vector<std::int16_t> keep0(a0.data, a0.data + a0.frames);
  const std::vector<std::int16_t> keep3(a3.data, a3.data + a3.frames);
  const std::uint32_t free_before = rig.bank.free_samples();
  run_new(rig, true);
  CHECK(same_table(before, table_of(project)));
  CHECK(project.instrument_count() == before.in.size() - static_cast<std::size_t>(std::count(before.used.begin(), before.used.end(), false)));
  CHECK(project.module_count() == before.modules.size());
  CHECK(project.pattern_count() == 1u);
  CHECK(project.song_length() == 1u);
  CHECK(project.order(0) == 1u);
  CHECK(std::string(project.name()).empty());
  const app::SampleBank::Pcm b0 = rig.bank.pcm(0);
  const app::SampleBank::Pcm b3 = rig.bank.pcm(3);
  NEED(b0.data == a0.data);
  NEED(b3.data == a3.data);
  CHECK(std::memcmp(b0.data, keep0.data(), keep0.size() * 2u) == 0);
  CHECK(std::memcmp(b3.data, keep3.data(), keep3.size() * 2u) == 0);
  CHECK(rig.bank.free_samples() == free_before);  // the audio keeps its memory
  CHECK(rig.bank.card_version(0) == 0u);          // and no slot counts as held by a folder of the old project
  CHECK(rig.bank.card_version(3) == 0u);
}

TEST_CASE("new KEEP INSTRUMENTS, then SAVE, then reopen: the samples are written into the new project's own folder and the instruments and their audio come back (OS-NEWKEEP)") {
  Rig rig;
  fill_instruments(rig);
  save_as(rig, "Old");  // the old project is a folder of the card, its samples in Projects/Old/samples
  NEED(rig.card.has("Projects/Old/samples/instr01.wav"));
  rig.project.last_folder = "Projects/Old";  // (it is the open project: the platform's folder() says so until NEW)
  const InstrumentTable before = table_of(test::arena(1).project);
  const app::SampleBank::Pcm a0 = rig.bank.pcm(0);
  const app::SampleBank::Pcm a3 = rig.bank.pcm(3);
  rig.bank.mark_on_card(0);  // (as the load of Projects/Old marked them)
  rig.bank.mark_on_card(3);
  rig.project.last_folder.clear();  // NEW: no folder holds the new project
  run_new(rig, true);
  const Bytes old_project = *rig.card.get("Projects/Old/project.o3pj");
  rig.go(ScreenId::File);
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.message() == "SAVE TRACK 01 DONE");
  // its own folder holds the samples, byte for byte what the bank holds, and the project file
  CHECK(rig.card.same("Projects/Track 01/samples/instr01.wav", wav_of(rig, 0)));
  CHECK(rig.card.same("Projects/Track 01/samples/instr04.wav", wav_of(rig, 3)));
  CHECK(rig.card.same("Projects/Track 01/project.o3pj", rig.native_bytes()));
  CHECK(rig.card.same("Projects/Old/project.o3pj", old_project));  // the old folder is as it was
  CHECK(rig.card.violations == 0);
  // reopen it the way the device does: the native project, then the sample loader over the same folder
  model::ModelArena& arena2 = test::arena(2);
  app::NativeFolderLoader native;
  NEED(native.begin(rig.card, "Projects/Track 01", arena2));
  for (unsigned i = 0; i < 2000 && native.state() == app::NativeFolderLoader::State::Running; ++i) {
    (void)native.step(4096);
  }
  NEED(native.state() == app::NativeFolderLoader::State::Done);
  CHECK(same_table(before, table_of(arena2.project)));
  auto voices = std::make_unique<audio::VoicePool>();
  std::vector<std::int16_t> memory2(60000);
  auto mem2 = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank2;
  bank2.init(*voices, arena2, *mem2, memory2.data(), static_cast<std::uint32_t>(memory2.size()));
  app::OgSampleLoader loader;
  NEED(loader.begin(rig.card, "Projects/Track 01", arena2.project, 0, *voices, memory2.data(), static_cast<std::uint32_t>(memory2.size()), audio::LevelLaw::Og12, &bank2));
  for (unsigned i = 0; i < 2000 && loader.state() == app::OgSampleLoader::State::Running; ++i) {
    (void)loader.step(4096);
  }
  CHECK(loader.report().loaded == 2u);
  CHECK(loader.report().missing == 0u);
  const app::SampleBank::Pcm b0 = bank2.pcm(0);
  const app::SampleBank::Pcm b3 = bank2.pcm(3);
  NEED(b0.frames == a0.frames);
  NEED(b3.frames == a3.frames);
  CHECK(std::memcmp(b0.data, a0.data, a0.frames * 2u) == 0);
  CHECK(std::memcmp(b3.data, a3.data, a3.frames * 2u) == 0);
}

// OS-USBDISK 4.3 (U2a-3), the exclusivity of the SaveService over a card the computer has (WriteStatus::Host): the refusal says why, nothing is written, a settings change made meanwhile (Q9) is
// written once after the eject and not before, and the shutdown does not wait for it.
TEST_CASE("save: Host gate, the card path refuses with its message, settings made during Host are written after the eject and not before, the shutdown does not wait") {
  Rig rig;
  rig.passes(3);  // the first pass takes the settings as they are
  rig.card.log.clear();
  rig.card.gate_status = hal::storage::WriteStatus::Host;
  CHECK(rig.saves.gate() == hal::storage::WriteStatus::Host);
  CHECK(std::string(app::SaveService::gate_text(hal::storage::WriteStatus::Host)) == "CARD ON COMPUTER - EJECT FIRST");
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  rig.pass(2000);
  rig.pass(2000);
  rig.passes(5);
  CHECK(rig.card.ops == 0u);  // no write, no temp file, no attempt
  CHECK_FALSE(rig.card.has("Settings/device.o3st"));
  rig.saves.power_begin();
  CHECK_FALSE(rig.saves.power_busy());  // the shutdown while out: nothing to wait for, nothing written
  CHECK(rig.card.ops == 0u);
  rig.card.gate_status = hal::storage::WriteStatus::Ok;  // the remount
  rig.pass(2000);
  rig.pass(2000);
  rig.settle();
  NEED(rig.card.has("Settings/device.o3st"));
  model::Settings back;
  const Bytes* file = rig.card.get("Settings/device.o3st");
  NEED(file != nullptr);
  CHECK(formats::settings::read(file->data(), file->size(), back).error == formats::settings::Error::None);
  CHECK(back.get(model::device_setting::Theme) == 1.0f);
  CHECK(rig.card.violations == 0);
  // The listings and the read refusals say where the card is.
  CHECK(std::string(rig.app->state().card_off_text("NO SD CARD")) == "NO SD CARD");
  rig.app->set_card_on_computer(true);
  CHECK(std::string(rig.app->state().card_off_text("NO SD CARD")) == "CARD ON COMPUTER");
  rig.app->set_card_on_computer(false);
  CHECK_FALSE(rig.app->state().card_on_computer);
}

// OS-USBDISK U2a-3 review fold: the hand-over state machine (platform/mk66/disk_share.h) over the REAL SaveService. The port is a thin fake: the layout changes at once (or never, `slow_layout`), the owner is
// what the card's gate says, and the write layer is the rig's.
namespace {
namespace mk = o3os::platform::mk66;
class SavePort final : public mk::DiskShare::Port {
 public:
  explicit SavePort(Rig& r) : rig(r) {}
  Rig& rig;
  mk::DiskShare::Layout cur{o3os::usb::Mode::Audio, o3os::usb::Profile::P10, true};
  bool changing = false;
  bool slow_layout = false;  // the DISK layout never completes: the machine stays in Layout
  bool held = false;
  bool file_at_flip = false;
  mk::storage::CardOwner owner() const noexcept override { return rig.card.gate_status == hal::storage::WriteStatus::Host ? mk::storage::CardOwner::Host : mk::storage::CardOwner::Tracker; }
  bool card_busy() const noexcept override { return false; }
  bool save_pending() const noexcept override { return rig.saves.write_due(); }
  mk::DiskShare::Layout layout() const noexcept override { return cur; }
  bool layout_changing() const noexcept override { return changing; }
  bool enter_disk_layout(const mk::DiskShare::Layout& to) noexcept override {
    cur = to;
    changing = slow_layout;
    return true;
  }
  bool leave_disk_layout(const mk::DiskShare::Layout& to) noexcept override { return change_layout(to); }
  bool change_layout(const mk::DiskShare::Layout& to) noexcept override {
    cur = to;
    changing = false;
    return true;
  }
  bool configured() const noexcept override { return true; }
  mk::storage::CardSwap::HandOver hand_to_host() noexcept override {
    file_at_flip = rig.card.has("Settings/device.o3st");
    rig.card.gate_status = hal::storage::WriteStatus::Host;
    return mk::storage::CardSwap::HandOver::Ok;
  }
  bool disk_open(bool) noexcept override { return true; }
  void disk_close() noexcept override {}
  hal::storage::Status take_back() noexcept override {
    rig.card.gate_status = hal::storage::WriteStatus::Ok;
    return hal::storage::Status::Ok;
  }
  bool engine_busy() const noexcept override { return false; }
  bool take_eject_request() noexcept override { return false; }
  void unit_eject() noexcept override {}
  void pump(std::uint32_t) noexcept override {}
  void hold_background(bool on) noexcept override {
    held = on;
    rig.saves.hold_background(on);
  }
  void log(const char*) noexcept override {}
};
}  // namespace

TEST_CASE("disk share over the save service: a settings change made just before the share is written first, the hold starts with the layout and not in Quiesce (4.2)") {
  Rig rig;
  SavePort port(rig);
  mk::DiskShare share(port);
  rig.passes(3);
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  rig.pass(16);  // poll() sees the change: a write is due, in its debounce
  NEED(rig.saves.write_due());
  std::uint32_t ms = 0;
  NEED(share.share(ms, true) == mk::DiskShare::Refusal::None);
  bool held_in_quiesce = false;
  for (unsigned i = 0; i < 300 && share.state() != mk::DiskShare::State::Out && !share.idle(); ++i) {  // 16 ms passes: the debounce (1.5 s) and the write fit in the 2 s wait
    rig.pass(16);
    ms += 16;
    share.pass(ms, false);
    held_in_quiesce = held_in_quiesce || (share.state() == mk::DiskShare::State::Quiesce && port.held);
  }
  CHECK_FALSE(held_in_quiesce);  // a hold there keeps the write from starting: the share ends in "CARD BUSY: WAIT FOR THE SAVE" for a save it prevented
  CHECK(share.fail() == mk::DiskShare::Fail::None);
  CHECK(share.state() == mk::DiskShare::State::Out);
  CHECK(port.file_at_flip);  // written BEFORE the owner flipped
  model::Settings back;
  const Bytes* file = rig.card.get("Settings/device.o3st");
  NEED(file != nullptr);
  CHECK(formats::settings::read(file->data(), file->size(), back).error == formats::settings::Error::None);
  CHECK(back.get(model::device_setting::Theme) == 1.0f);
}

TEST_CASE("disk share over the save service: the power latch in the middle of a hand-over lifts the hold, so the shutdown's settings flush is written (the Tracker has the card)") {
  Rig rig;
  SavePort port(rig);
  port.slow_layout = true;
  mk::DiskShare share(port);
  rig.passes(3);
  NEED(share.share(0, true) == mk::DiskShare::Refusal::None);
  share.pass(1, false);  // Quiesce -> Layout: nothing is due
  NEED(share.state() == mk::DiskShare::State::Layout);
  CHECK(port.held);
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 2.0f) == model::Status::Ok);
  rig.card.log.clear();
  rig.pass(2000);
  rig.pass(2000);
  rig.passes(5);
  CHECK_FALSE(rig.card.has("Settings/device.o3st"));  // held
  share.pass(2, true);                                  // the shutdown's stage A: the machine is frozen, the hold lifted (the owner is the Tracker)
  CHECK_FALSE(port.held);
  CHECK(share.state() == mk::DiskShare::State::Layout);
  rig.saves.power_begin();
  rig.pass(16);
  CHECK(rig.saves.power_busy());  // the flush is due: stage B waits for it
  rig.settle();
  NEED(rig.card.has("Settings/device.o3st"));
  model::Settings back;
  const Bytes* file = rig.card.get("Settings/device.o3st");
  NEED(file != nullptr);
  CHECK(formats::settings::read(file->data(), file->size(), back).error == formats::settings::Error::None);
  CHECK(back.get(model::device_setting::Theme) == 2.0f);
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save: the shutdown does not wait for settings that a hold or the Host gate keeps from being written (and waits for them again when neither is on)") {
  Rig rig;
  rig.passes(3);
  CHECK(rig.app->device_settings().set(model::device_setting::Theme, 1.0f) == model::Status::Ok);
  rig.pass(100);  // poll() has seen the change (not clean), the debounce is not over
  rig.saves.hold_background(true);
  rig.saves.power_begin();
  CHECK_FALSE(rig.saves.power_busy());  // hold_: the flush cannot start
  rig.saves.hold_background(false);
  rig.card.gate_status = hal::storage::WriteStatus::Host;
  CHECK_FALSE(rig.saves.power_busy());  // the gate says Host
  rig.card.gate_status = hal::storage::WriteStatus::Ok;
  CHECK(rig.saves.power_busy());        // neither: the flush is due
  rig.settle();
  CHECK(rig.card.has("Settings/device.o3st"));
}

// ---- OS-STEREO step 2, slice (b): the side file samples/<stem>_s.wav ----------------------------------------------------------------------------------

namespace {

// A stereo sample instrument in `slot`: the mid as put_sample() writes it, S[i] = -(i % 11) - 1, both in the bank as one block (S at pcm + even(frames)); the instrument's stereo flag is set.
void put_stereo(Rig& rig, unsigned slot, std::uint32_t frames, int seed, bool flag = true, const char* path = nullptr) {
  model::Project& project = test::arena(1).project;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  if (path != nullptr) {
    std::strncpy(in.sample_path, path, sizeof in.sample_path - 1);
  } else {
    std::snprintf(in.sample_path, sizeof in.sample_path, "samples/instr%02u.wav", slot + 1u);
  }
  in.stereo = flag ? 1u : 0u;
  NEED(project.set_instrument(slot, in) == model::Status::Ok);
  std::int16_t* pcm = rig.memory.data() + slot * 4000u;
  const std::uint32_t even = (frames + 1u) & ~1u;
  for (std::uint32_t i = 0; i < frames; ++i) {
    pcm[i] = static_cast<std::int16_t>(seed + static_cast<int>(i % 200u) * 7);
    pcm[even + i] = static_cast<std::int16_t>(-static_cast<int>(i % 11u) - 1);
  }
  rig.bank.adopt(slot, pcm, frames, 2u * even, pcm + even);
}

// What a WAV save of the slot's side signal makes.
Bytes side_wav_of(const Rig& rig, unsigned slot, std::uint32_t rate = 44100) {
  const app::SampleBank::Pcm pcm = rig.bank.pcm(slot);
  Bytes out(app::WavSaveSource::kHeaderBytes);
  app::WavSaveSource::make_header(out.data(), pcm.frames, 1, rate);
  const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(pcm.side);
  out.insert(out.end(), raw, raw + pcm.frames * 2u);
  return out;
}

// The native project and the sample loader over `folder` of the rig's card, as the device reopens a project: into arena 2, a fresh bank (`bank2` in `mem2`, 60,000 samples of `memory2`).
struct Reopen {
  model::ModelArena& arena2 = test::arena(2);
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::vector<std::int16_t> memory2 = std::vector<std::int16_t>(60000);
  std::unique_ptr<app::SampleBankMem> mem2 = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank2;
  app::OgSampleLoader loader;
  bool open(Rig& rig, const char* folder) {
    app::NativeFolderLoader native;
    if (!native.begin(rig.card, folder, arena2)) {
      return false;
    }
    for (unsigned i = 0; i < 2000 && native.state() == app::NativeFolderLoader::State::Running; ++i) {
      (void)native.step(4096);
    }
    if (native.state() != app::NativeFolderLoader::State::Done) {
      return false;
    }
    bank2.init(*voices, arena2, *mem2, memory2.data(), static_cast<std::uint32_t>(memory2.size()));
    if (!loader.begin(rig.card, folder, arena2.project, 0, *voices, memory2.data(), static_cast<std::uint32_t>(memory2.size()), audio::LevelLaw::Og12, &bank2, true)) {
      return false;
    }
    for (unsigned i = 0; i < 4000 && loader.state() == app::OgSampleLoader::State::Running; ++i) {
      (void)loader.step(4096);
    }
    return loader.state() == app::OgSampleLoader::State::Done;
  }
};

}  // namespace

TEST_CASE("save stereo: Save As writes each stereo sample's mid, then its side file, then the project file; a mono sample has no side file") {
  Rig rig;
  put_stereo(rig, 0, 600, 100);
  put_sample(rig, 2, 900, -2000);
  save_as(rig, "Mine");
  CHECK(rig.message() == "SAVE MINE DONE");
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));  // M: the file an older build and any other tool reads, unchanged
  CHECK(rig.card.same("Projects/Mine/samples/instr01_s.wav", side_wav_of(rig, 0)));
  CHECK(rig.card.same("Projects/Mine/samples/instr03.wav", wav_of(rig, 2)));
  CHECK_FALSE(rig.card.has("Projects/Mine/samples/instr03_s.wav"));
  CHECK(rig.card.same("Projects/Mine/project.o3pj", rig.native_bytes()));  // the flag is written: its S file is
  const std::size_t m = index_of(rig.card, "rename Projects/Mine/samples/_o3save.tmp>Projects/Mine/samples/instr01.wav");
  const std::size_t m3 = index_of(rig.card, "rename Projects/Mine/samples/_o3save.tmp>Projects/Mine/samples/instr03.wav");
  const std::size_t s = index_of(rig.card, "rename Projects/Mine/samples/_o3save.tmp>Projects/Mine/samples/instr01_s.wav");
  const std::size_t p = index_of(rig.card, "rename Projects/Mine/_o3save.tmp>Projects/Mine/project.o3pj");
  CHECK(m < s);
  CHECK(m3 < s);  // every mid, then the side files
  CHECK(s < p);
  CHECK(p < rig.card.log.size());
  CHECK(rig.card.violations == 0);
  // A plain Save writes no sample again, the side file included.
  rig.card.log.clear();
  rig.go(ScreenId::File);
  rig.tap(Button::F4);
  rig.settle();
  for (const std::string& line : rig.card.log) {
    CHECK(line.find("samples") == std::string::npos);
  }
  // A new version of the audio replaces both files, the old ones kept as .bak.
  const Bytes old_side = side_wav_of(rig, 0);
  put_stereo(rig, 0, 650, 500);
  rig.card.log.clear();
  rig.expire();
  rig.tap(Button::F4);
  rig.settle();
  CHECK(rig.card.same("Projects/Mine/samples/instr01.wav", wav_of(rig, 0)));
  CHECK(rig.card.same("Projects/Mine/samples/instr01_s.wav", side_wav_of(rig, 0)));
  CHECK(rig.card.same("Projects/Mine/samples/instr01_s.wav.bak", old_side));
  CHECK(rig.card.violations == 0);
}

TEST_CASE("save stereo: the reopen of the folder has the mid and the side bit for bit, the flag, and the loader's belief that the folder holds them") {
  Rig rig;
  put_stereo(rig, 0, 1201, 321);  // odd: S starts at 1202
  put_sample(rig, 3, 800, -1500);
  save_as(rig, "Reopen");
  NEED(rig.card.has("Projects/Reopen/samples/instr01_s.wav"));
  const app::SampleBank::Pcm a0 = rig.bank.pcm(0);
  Reopen re;
  NEED(re.open(rig, "Projects/Reopen"));
  CHECK(re.loader.report().loaded == 2u);
  CHECK(re.loader.report().stereo_lost == 0u);
  const app::SampleBank::Pcm b0 = re.bank2.pcm(0);
  NEED(b0.frames == a0.frames);
  NEED(b0.side != nullptr);
  CHECK(b0.side == b0.data + 1202);
  CHECK(std::memcmp(b0.data, a0.data, a0.frames * 2u) == 0);
  CHECK(std::memcmp(b0.side, a0.side, a0.frames * 2u) == 0);
  CHECK(re.arena2.project.instrument(0)->stereo == 1u);
  CHECK_FALSE(re.bank2.has_side(3));
  CHECK(re.arena2.project.instrument(3)->stereo == 0u);
  CHECK(re.bank2.card_version(0) == re.bank2.version(0));
  // The mono sample after it is where it should be (the slot after a stereo block).
  CHECK(re.bank2.pcm(3).frames == 800u);
  CHECK(std::memcmp(re.bank2.pcm(3).data, rig.bank.pcm(3).data, 1600u) == 0);
}

TEST_CASE("save stereo: a missing, shorter or other-rate side file, or one that is not mono, loads the instrument mono with the flag cleared and the loss counted") {
  const auto reopen_with = [](const char* what, auto&& spoil) {
    INFO(what);
    Rig rig;
    put_stereo(rig, 0, 700, 50);
    save_as(rig, "Spoiled");
    NEED(rig.card.has("Projects/Spoiled/samples/instr01_s.wav"));
    spoil(rig);
    Reopen re;
    NEED(re.open(rig, "Projects/Spoiled"));
    CHECK(re.loader.report().loaded == 1u);
    CHECK(re.loader.report().stereo_lost == 1u);
    CHECK(re.bank2.pcm(0).frames == 700u);
    CHECK(std::memcmp(re.bank2.pcm(0).data, rig.bank.pcm(0).data, 1400u) == 0);  // the mid is the sample, as an older build loads it
    CHECK_FALSE(re.bank2.has_side(0));
    CHECK(re.arena2.project.instrument(0)->stereo == 0u);  // rule 4: a 1 is never kept without its S
  };
  reopen_with("missing", [](Rig& rig) { rig.card.files.erase("projects/spoiled/samples/instr01_s.wav"); });
  reopen_with("shorter", [](Rig& rig) {
    Bytes b(app::WavSaveSource::kHeaderBytes);
    app::WavSaveSource::make_header(b.data(), 600, 1, 44100);
    b.insert(b.end(), 1200u, 0);
    rig.card.put("Projects/Spoiled/samples/instr01_s.wav", std::string(b.begin(), b.end()));
  });
  reopen_with("longer", [](Rig& rig) {
    Bytes b(app::WavSaveSource::kHeaderBytes);
    app::WavSaveSource::make_header(b.data(), 800, 1, 44100);
    b.insert(b.end(), 1600u, 0);
    rig.card.put("Projects/Spoiled/samples/instr01_s.wav", std::string(b.begin(), b.end()));
  });
  reopen_with("other rate", [](Rig& rig) {
    Bytes b(app::WavSaveSource::kHeaderBytes);
    app::WavSaveSource::make_header(b.data(), 700, 1, 22050);
    b.insert(b.end(), 1400u, 0);
    rig.card.put("Projects/Spoiled/samples/instr01_s.wav", std::string(b.begin(), b.end()));
  });
  reopen_with("not mono", [](Rig& rig) {
    Bytes b(app::WavSaveSource::kHeaderBytes);
    app::WavSaveSource::make_header(b.data(), 700, 2, 44100);
    b.insert(b.end(), 2800u, 0);
    rig.card.put("Projects/Spoiled/samples/instr01_s.wav", std::string(b.begin(), b.end()));
  });
  reopen_with("not a WAV", [](Rig& rig) { rig.card.put("Projects/Spoiled/samples/instr01_s.wav", std::string("garbage garbage garbage")); });
}

TEST_CASE("save stereo: rule 4 at save: a flag with no S in the bank is written as 0 and the document keeps it; mono projects are the bytes they were") {
  {
    Rig rig;
    put_stereo(rig, 0, 500, 7, true);
    put_sample(rig, 1, 400, 9);
    const Bytes with_flag_and_side = rig.native_bytes();
    // the audio loses its S (an edit's mono result, a take): the flag stays in the document
    rig.bank.adopt(0, rig.memory.data(), 500);
    NEED(test::arena(1).project.instrument(0)->stereo == 1u);
    save_as(rig, "Rule4");
    CHECK(rig.message() == "SAVE RULE4 DONE");
    CHECK_FALSE(rig.card.has("Projects/Rule4/samples/instr01_s.wav"));
    CHECK(test::arena(1).project.instrument(0)->stereo == 1u);  // the document is untouched
    // The file on the card is the project with the flag at 0.
    model::Instrument cleared = *test::arena(1).project.instrument(0);
    cleared.stereo = 0;
    NEED(test::arena(1).project.set_instrument(0, cleared) == model::Status::Ok);
    const Bytes mono_bytes = rig.native_bytes();
    CHECK(rig.card.same("Projects/Rule4/project.o3pj", mono_bytes));
    CHECK(mono_bytes != with_flag_and_side);
  }
  {  // another instrument names the side file as its own sample: the side file is not written, and no flag either
    Rig rig;
    put_stereo(rig, 0, 300, 3, true);
    model::Instrument other;
    model::init_instrument(other, model::InstrumentKind::Sample);
    std::strcpy(other.sample_path, "samples/instr01_s.wav");
    NEED(test::arena(1).project.set_instrument(5, other) == model::Status::Ok);
    save_as(rig, "Clash");
    CHECK_FALSE(rig.card.has("Projects/Clash/samples/instr01_s.wav"));
    Reopen re;
    NEED(re.open(rig, "Projects/Clash"));
    CHECK(re.arena2.project.instrument(0)->stereo == 0u);
  }
}

TEST_CASE("save stereo: a side name that another instrument has (any case) or that does not fit saves the instrument mono and says so; no other instrument's file is touched") {
  const auto run = [](const char* what, const char* stereo_path, const char* other_path) {
    INFO(what);
    Rig rig;
    put_stereo(rig, 0, 300, 3, true, stereo_path);
    if (other_path != nullptr) {
      put_sample(rig, 5, 400, 77, other_path);  // a mono sample with audio of its own: its file is the side name
    }
    save_as(rig, "Clash");
    CHECK(rig.message().rfind("STEREO SAVED MONO: ", 0) == 0u);
    CHECK(rig.message().size() <= 48u);
    if (other_path != nullptr) {
      CHECK(rig.card.same("Projects/Clash/samples/instr01_s.wav", wav_of(rig, 5)));  // the other instrument's audio, not S
    }
    CHECK(rig.card.violations == 0);
    Reopen re;
    NEED(re.open(rig, "Projects/Clash"));
    CHECK(re.loader.report().stereo_lost == 0u);  // the flag was not written, so nothing is lost at load
    CHECK(re.arena2.project.instrument(0)->stereo == 0u);
    CHECK_FALSE(re.bank2.has_side(0));
    CHECK(re.bank2.pcm(0).frames == 300u);
    if (other_path != nullptr) {
      CHECK(re.bank2.pcm(5).frames == 400u);
    }
  };
  run("sibling", nullptr, "samples/instr01_s.wav");
  run("other case", nullptr, "samples/instr01_S.wav");
  run("all upper case", nullptr, "samples/INSTR01_S.WAV");
  run("stem too long", "samples/abcdefghijklmnopqrstuvwxyz0123.wav", nullptr);  // 30 characters: "_s" makes 32 of the guard's 31
  Rig fits;  // 29 characters is the longest stem that has a side name: no message
  put_stereo(fits, 0, 300, 3, true, "samples/abcdefghijklmnopqrstuvwxyz012.wav");
  save_as(fits, "Fits");
  CHECK(fits.message() == "SAVE FITS DONE");
  CHECK(fits.card.has("Projects/Fits/samples/abcdefghijklmnopqrstuvwxyz012_s.wav"));
}

TEST_CASE("save stereo: the flag is written only where the plan wrote or found the side file, not where the bank has a side signal by the time the project file starts") {
  Rig rig;
  put_stereo(rig, 0, 400, 5, true);
  put_sample(rig, 2, 3500, 9);  // a long mid after it: the plan has several passes to go
  const std::uint32_t even = 400;
  std::int16_t* const pcm0 = rig.memory.data();
  rig.bank.adopt(0, pcm0, 400);  // the side signal is gone at the start (an edit's result): the flag stays in the document
  NEED(test::arena(1).project.instrument(0)->stereo == 1u);
  rig.go(ScreenId::File);
  rig.tap(Button::F5);
  auto& entry = const_cast<app::TextEntry&>(rig.app->state().text.entry);
  entry.set("Undone");
  rig.tap(Button::F8);
  bool restored = false;
  for (unsigned i = 0; i < 400 && !rig.card.has("Projects/Undone/project.o3pj"); ++i) {
    rig.pass();
    if (!restored && rig.card.has("Projects/Undone/samples/instr01.wav") && !rig.card.has("Projects/Undone/project.o3pj")) {
      rig.bank.adopt(0, pcm0, 400, 2u * even, pcm0 + even);  // an undo brings S back between the plan's start and the project job
      restored = true;
    }
  }
  rig.settle();
  NEED(restored);
  CHECK_FALSE(rig.card.has("Projects/Undone/samples/instr01_s.wav"));
  Reopen re;
  NEED(re.open(rig, "Projects/Undone"));
  CHECK(re.loader.report().stereo_lost == 0u);
  CHECK(re.arena2.project.instrument(0)->stereo == 0u);  // never a flag without its file
}
