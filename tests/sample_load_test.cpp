// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the Sample load screen on the App: the WAV browser over the File screen's listing (folders first, the .wav filter, a folder called kick.wav is a
// folder: PAR-033), Enter and Up through the folders, the one listing shared with the File screen, and Load: the WAV goes through the import job into the
// target instrument (OS-002c), its audio into the sample arena, the instrument made from the file, the peak cache built, and the engine takes it up only
// when playback is stopped (the bank's handover). The bank's own rules are sample_bank_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/state_line.h>
#include <o3os/app/screens/sample_load_screen.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/formats/sample_probe.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/ui/plot/peak_cache.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace o3os;
using app::Button;
using app::ScreenId;
namespace fs = std::filesystem;

namespace {

alignas(8) std::uint8_t g_journal_ring[64 * 1024];

void put16(std::vector<std::uint8_t>& v, unsigned x) {
  v.push_back(static_cast<std::uint8_t>(x & 0xFF));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  put16(v, x & 0xFFFF);
  put16(v, x >> 16);
}
void tag(std::vector<std::uint8_t>& v, const char* t) {
  for (int i = 0; i < 4; ++i) {
    v.push_back(static_cast<std::uint8_t>(t[i]));
  }
}

// The sample `i` of the test files: a ramp that never clips and is not constant.
std::int16_t ramp(std::uint32_t i) { return static_cast<std::int16_t>(static_cast<int>((i * 7u) % 20000u) - 10000); }

// A mono 16-bit WAV of `frames` frames at `rate`.
std::vector<std::uint8_t> wav_bytes(std::uint32_t frames, std::uint32_t rate = 44100) {
  std::vector<std::uint8_t> v;
  tag(v, "RIFF");
  put32(v, 36 + frames * 2);
  tag(v, "WAVE");
  tag(v, "fmt ");
  put32(v, 16);
  put16(v, 1);
  put16(v, 1);
  put32(v, rate);
  put32(v, rate * 2);
  put16(v, 2);
  put16(v, 16);
  tag(v, "data");
  put32(v, frames * 2);
  for (std::uint32_t i = 0; i < frames; ++i) {
    put16(v, static_cast<std::uint16_t>(ramp(i)));
  }
  return v;
}

// OS-SAMPFMT: a mono 16-bit AIFF of `frames` frames at 22,050 Hz (the 80-bit rate 0x400D AC44...), with an INST + MARK forward loop between frames 100 and 300 when `loop`.
void be16(std::vector<std::uint8_t>& v, unsigned x) {
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}
void be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  be16(v, x >> 16);
  be16(v, x & 0xFFFF);
}
void be_size(std::vector<std::uint8_t>& v, std::size_t at) {  // the FORM size, once the file is whole
  const std::uint32_t n = static_cast<std::uint32_t>(v.size() - 8);
  v[at] = static_cast<std::uint8_t>(n >> 24);
  v[at + 1] = static_cast<std::uint8_t>(n >> 16);
  v[at + 2] = static_cast<std::uint8_t>(n >> 8);
  v[at + 3] = static_cast<std::uint8_t>(n);
}
std::vector<std::uint8_t> aiff_bytes(std::uint32_t frames, bool loop = false) {
  std::vector<std::uint8_t> v;
  tag(v, "FORM");
  be32(v, 0);
  tag(v, "AIFF");
  tag(v, "COMM");
  be32(v, 18);
  be16(v, 1);
  be32(v, frames);
  be16(v, 16);
  be16(v, 0x400D);  // 22,050 Hz as an 80-bit extended number
  be32(v, 22050u << 17);
  be32(v, 0);
  if (loop) {
    tag(v, "INST");
    be32(v, 20);
    for (int i = 0; i < 8; ++i) v.push_back(0);
    be16(v, 1);  // forward sustain loop
    be16(v, 1);
    be16(v, 2);
    for (int i = 0; i < 6; ++i) v.push_back(0);
    tag(v, "MARK");
    be32(v, 2 + 2 * 8);
    be16(v, 2);
    be16(v, 1); be32(v, 100); v.push_back(0); v.push_back(0);
    be16(v, 2); be32(v, 300); v.push_back(0); v.push_back(0);
  }
  tag(v, "SSND");
  be32(v, 8 + frames * 2);
  be32(v, 0);
  be32(v, 0);
  for (std::uint32_t i = 0; i < frames; ++i) be16(v, static_cast<std::uint16_t>(ramp(i)));
  be_size(v, 4);
  return v;
}

// An 8SVX of `one_shot` + `repeat` samples at 16,726 Hz named "Amiga bass".
std::vector<std::uint8_t> svx_bytes(std::uint32_t one_shot, std::uint32_t repeat) {
  std::vector<std::uint8_t> v;
  tag(v, "FORM");
  be32(v, 0);
  tag(v, "8SVX");
  tag(v, "VHDR");
  be32(v, 20);
  be32(v, one_shot);
  be32(v, repeat);
  be32(v, 32);
  be16(v, 16726);
  v.push_back(1);
  v.push_back(0);
  be32(v, 0x10000);
  tag(v, "NAME");
  be32(v, 10);
  for (const char* c = "Amiga bass"; *c != '\0'; ++c) v.push_back(static_cast<std::uint8_t>(*c));
  tag(v, "BODY");
  be32(v, one_shot + repeat);
  for (std::uint32_t i = 0; i < one_shot + repeat; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(ramp(i) >> 8)));
  if (((one_shot + repeat) & 1u) != 0) v.push_back(0);
  be_size(v, 4);
  return v;
}

void write_file(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// The card of the tests: a temp directory (DirStorage reads files; FakeStorage has no bytes).
struct Card {
  fs::path root;
  Card() {
    root = fs::temp_directory_path() / ("o3os-sample-load-" + std::to_string(::getpid()) + "-" + std::to_string(counter()++));
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~Card() { fs::remove_all(root); }
  static unsigned& counter() {
    static unsigned n = 0;
    return n;
  }
};

struct Rig {
  Card dir;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  platform::host::DirStorage card{dir.root.string()};
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peaks;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(std::uint32_t arena_samples = 200000, bool with_bank = true, bool with_document = true) : memory(arena_samples + 8), peaks(40000) {
    NEED(card.mount() == hal::storage::Status::Ok);
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), arena_samples, peaks.data(), peaks.size());
    if (with_document) {
      journal.begin(g_journal_ring, sizeof g_journal_ring);
      editor = std::make_unique<model::Editor>(arena->project, journal);
      app->attach_document(editor.get());
    }
    app->attach_storage(&card);
    if (with_bank) {
      app->attach_samples(&bank);
    }
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
  void settle() {  // until the scan has ended
    for (unsigned i = 0; i < 2000; ++i) {
      pass();
      const app::DirScan::State s = app->model().files->state();
      if (s != app::DirScan::State::Scanning && s != app::DirScan::State::Idle) {
        return;
      }
    }
    FAIL_CHECK("the scan did not end");
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void invoke(const char* name, core::ActionArg arg = 0) {
    (void)app->invoke(core::action_id(name), arg);
    pass();
  }
  void expire() {
    for (unsigned i = 0; i < 10 && !std::string(app->message()).empty(); ++i) {
      clock.advance(3000000);
      app->run_once();
    }
  }
  const app::DirIndex& index() const { return app->model().files->index(); }
  std::string name_at(unsigned i) const { return index().name(i); }
  // The cursor on the entry called `name`.
  void select(const std::string& name) {
    const app::Screen& s = app::kSampleLoadScreen;
    for (unsigned i = 0; i < index().count(); ++i) {
      if (name_at(i) == name) {
        app::ViewState& v = const_cast<app::ViewState&>(app->state());
        (void)s.set_position(app->model(), v, static_cast<int>(i));
        CHECK(app->state().wav.list.pos() == static_cast<int>(i));
        return;
      }
    }
    FAIL_CHECK("no such entry: " << name);
  }
  std::string message() const { return app->message(); }
  // Runs passes until the strip has said `text` (the job and the peaks take a few).
  bool until(const std::string& text, unsigned max = 400) {
    std::string seen;
    for (unsigned i = 0; i < max; ++i) {
      pass();
      if (message() == text) {
        return true;
      }
      if (!message().empty() && seen.find(message()) == std::string::npos) {
        seen += "[" + message() + "]";
      }
    }
    MESSAGE("until \"" << text << "\": the strip said " << seen);
    return false;
  }
};

}  // namespace

TEST_CASE("sample load: the listing is the card's folders first, then the .wav files, whatever the case; a folder called kick.wav is a folder (PAR-033)") {
  Rig rig;
  write_file(rig.dir.root / "kick.wav", wav_bytes(100));
  write_file(rig.dir.root / "Loop.WAV", wav_bytes(100));
  write_file(rig.dir.root / "notes.txt", {1, 2, 3});
  write_file(rig.dir.root / ".hidden.wav", wav_bytes(100));
  fs::create_directories(rig.dir.root / "snare.wav");  // a folder with a file's name
  fs::create_directories(rig.dir.root / "drums");
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.index().count() == 4u);
  CHECK(rig.name_at(0) == "drums");
  CHECK(rig.name_at(1) == "snare.wav");
  CHECK(rig.index().is_dir(0));
  CHECK(rig.index().is_dir(1));  // by what the card says, not by the name
  CHECK(rig.name_at(2) == "kick.wav");
  CHECK(rig.name_at(3) == "Loop.WAV");
  CHECK_FALSE(rig.index().is_dir(2));
  CHECK(std::string(rig.app->model().files->folder()).empty());
}

// OS-HILNAV: `diag state` says the entry under the cursor (sel=), so a HIL scenario walks to a file by its name instead of counting Down presses on a card that gains files.
TEST_CASE("sample load: the state line names the entry under the cursor, spaces as underscores, and `-` off the browsers") {
  Rig rig;
  write_file(rig.dir.root / "my kick.wav", wav_bytes(100));
  write_file(rig.dir.root / "zeta.wav", wav_bytes(100));
  fs::create_directories(rig.dir.root / "drums");
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.index().count() == 3u);
  auto sel = [&]() -> std::string {
    char buf[320];
    (void)app::describe_state(*rig.app, buf, sizeof buf);
    const std::string line = buf;
    const std::size_t at = line.find(" sel=");
    if (at == std::string::npos) {
      return "<missing>";
    }
    const std::size_t end = line.find(' ', at + 1);
    return line.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
  };
  CHECK(sel() == "drums");
  rig.tap(Button::Down);
  CHECK(sel() == "my_kick.wav");
  rig.tap(Button::Down);
  CHECK(sel() == "zeta.wav");
  rig.go(ScreenId::Pattern);
  CHECK(sel() == "-");
}

TEST_CASE("sample load: Enter goes into the folder under the cursor, Up comes back; the cursor starts at the top of an entered folder; at the root Up says so") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "hat.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "deep" / "tom.wav", wav_bytes(100));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("sample.up");
  CHECK(rig.message() == "ALREADY AT THE TOP");
  rig.expire();
  rig.invoke("sample.enter");  // the cursor is on "drums" (the folder first)
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums");
  CHECK(std::string(rig.app->model().files->folder()) == "drums");
  NEED(rig.index().count() == 2u);
  CHECK(rig.name_at(0) == "deep");
  CHECK(rig.name_at(1) == "hat.wav");
  CHECK(rig.app->state().wav.list.pos() == 0);
  rig.invoke("sample.enter");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums/deep");
  NEED(rig.index().count() == 1u);
  CHECK(rig.name_at(0) == "tom.wav");
  rig.invoke("sample.enter");  // a file: not a folder
  CHECK(rig.message() == "NOT A FOLDER");
  rig.invoke("sample.up");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums");
  rig.invoke("sample.up");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder).empty());
  CHECK(rig.name_at(0) == "drums");
}

TEST_CASE("sample load: Up puts the cursor on the folder just left, at any depth, once the parent's scan ends (OS-AUDIT-A1, E-LOAD-06)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(100));
  write_file(rig.dir.root / "aaa" / "x.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "hat.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "deep" / "tom.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "zzz" / "z.wav", wav_bytes(100));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(rig.name_at(0) == "aaa");
  CHECK(rig.name_at(1) == "drums");
  rig.select("drums");
  rig.invoke("sample.enter");
  rig.settle();
  rig.select("zzz");
  rig.invoke("sample.enter");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums/zzz");
  rig.invoke("sample.up");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums");
  NEED(rig.index().count() == 3u);  // deep, zzz, hat.wav
  CHECK(rig.app->state().wav.list.pos() == 1);
  CHECK(rig.name_at(1) == "zzz");
  rig.invoke("sample.up");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder).empty());
  NEED(rig.index().count() == 3u);  // aaa, drums, a.wav
  CHECK(rig.app->state().wav.list.pos() == 1);
  CHECK(rig.name_at(1) == "drums");
  CHECK(rig.app->state().wav.reselect[0] == '\0');  // used once
  // Enter takes a pending name back: the entered folder's cursor starts at the top.
  rig.invoke("sample.enter");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums");
  CHECK(rig.app->state().wav.list.pos() == 0);
}

TEST_CASE("sample load: a visit to File keeps the Sample load cursor: the folder is kept and the entry is found again by name (OS-AUDIT-A1, E-LOAD-23)") {
  Rig rig;
  write_file(rig.dir.root / "drums" / "hat.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "kick.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "snare.wav", wav_bytes(100));
  fs::create_directories(rig.dir.root / "Projects" / "song");
  write_file(rig.dir.root / "Projects" / "song" / "project.mt", {0});
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("drums");
  rig.invoke("sample.enter");
  rig.settle();
  rig.select("snare.wav");
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.index().count() == 1u);  // File took the one listing
  CHECK(rig.name_at(0) == "song");
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums");
  NEED(rig.index().count() == 3u);
  CHECK(rig.app->state().wav.list.pos() == 2);
  CHECK(rig.name_at(2) == "snare.wav");
  CHECK(rig.app->state().wav.reselect[0] == '\0');
}

TEST_CASE("sample load: leaving for File while the folder is still being read remembers nothing: entry 0 of a partial listing is no cursor (OS-AUDIT-A1, E-LOAD-23)") {
  Rig rig;
  for (int i = 0; i < 40; ++i) {
    write_file(rig.dir.root / "big" / ("f" + std::to_string(100 + i) + ".wav"), wav_bytes(100));
  }
  fs::create_directories(rig.dir.root / "Projects" / "song");
  write_file(rig.dir.root / "Projects" / "song" / "project.mt", {0});
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("big");
  rig.invoke("sample.enter");  // the scan of "big" starts over
  for (unsigned i = 0; i < 2000 && rig.app->model().files->state() != app::DirScan::State::Scanning; ++i) {
    rig.pass();
  }
  NEED(rig.app->model().files->state() == app::DirScan::State::Scanning);  // still being read
  rig.go(ScreenId::File);
  CHECK(rig.app->state().wav.reselect[0] == '\0');
  rig.settle();
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "big");
  CHECK(rig.app->state().wav.list.pos() == 0);
  CHECK(rig.app->state().wav.reselect[0] == '\0');
}

TEST_CASE("sample load: the File screen and Sample load share the one listing: entering the other starts its scan over, and each finds its own") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(100));
  fs::create_directories(rig.dir.root / "Projects" / "song");
  write_file(rig.dir.root / "Projects" / "song" / "project.mt", {0});
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.index().count() == 1u);
  CHECK(rig.name_at(0) == "song");
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.index().count() == 2u);  // the root: the Projects folder and a.wav
  CHECK(rig.name_at(0) == "Projects");
  CHECK(rig.name_at(1) == "a.wav");
  rig.go(ScreenId::File);
  rig.settle();
  NEED(rig.index().count() == 1u);
  CHECK(rig.name_at(0) == "song");
  CHECK(rig.app->model().files->index().full() == false);
}

TEST_CASE("sample load: F1 loads the WAV into the selected instrument: the import job, the audio in the arena, the instrument made from the file, the peaks; the engine follows when stopped") {
  Rig rig;
  const std::uint32_t frames = 5000;
  write_file(rig.dir.root / "kick.wav", wav_bytes(frames));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("instrument.select", 3);  // slot 2
  rig.select("kick.wav");
  rig.invoke("sample.load", 0);
  CHECK(rig.message().find("IMPORT KICK.WAV ") == 0u);
  CHECK(rig.bank.wav_open());
  NEED(rig.until("PEAKS KICK.WAV DONE"));
  CHECK_FALSE(rig.bank.wav_open());  // the file is closed
  // The audio is the slot's, byte for byte.
  const app::SampleBank::Pcm pcm = rig.bank.pcm(2);
  NEED(pcm.data != nullptr);
  CHECK(pcm.frames == frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    if (pcm.data[i] != ramp(i)) {
      FAIL_CHECK("sample " << i << " differs");
      break;
    }
  }
  CHECK(rig.bank.free_samples() == 200000u - frames);
  // The instrument was made from the file.
  const model::Instrument* in = rig.arena->project.instrument(2);
  NEED(in != nullptr);
  CHECK(in->kind == model::InstrumentKind::Sample);
  CHECK(std::string(in->name) == "kick");
  CHECK(in->sample_rate == 44100u);
  // The peak cache is the picture of this PCM.
  CHECK(rig.app->peak_cache().built());
  CHECK(rig.app->peak_cache().samples() == pcm.data);
  CHECK(rig.app->peak_cache().frames() == frames);
  // The engine follows when playback is stopped and the pool is quiet (a few passes: the settle is 5 ms).
  rig.passes(4);
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(2).length == frames);
  CHECK(rig.voices->sample(2).data == pcm.data);
  CHECK(rig.voices->sample(1).length == 0u);  // no other slot changed
}

TEST_CASE("sample load: while the transport plays the engine keeps its table; Stop lets the handover through") {
  Rig rig;
  write_file(rig.dir.root / "kick.wav", wav_bytes(2000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("kick.wav");
  struct Transport final : app::TransportSink {
    bool play(unsigned) noexcept override { return true; }
    bool play_song(unsigned) noexcept override { return true; }
    bool stop() noexcept override { return true; }
  } transport;
  rig.app->attach_transport(&transport);
  rig.invoke("transport.play");
  NEED(rig.app->state().playing);
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS KICK.WAV DONE"));
  rig.passes(10);
  CHECK(rig.bank.rebind_pending());              // marked, not written
  CHECK(rig.voices->sample(0).length == 0u);
  rig.invoke("transport.play");                  // stop
  NEED(!rig.app->state().playing);
  rig.passes(4);
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.voices->sample(0).length == 2000u);
}

TEST_CASE("sample load: F4 ADD goes to the first free instrument; a full project says so") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(1000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  model::Instrument taken;
  model::init_instrument(taken, model::InstrumentKind::Sample);
  CHECK(rig.arena->project.set_instrument(0, taken) == model::Status::Ok);
  CHECK(rig.arena->project.set_instrument(1, taken) == model::Status::Ok);
  rig.invoke("sample.load", 1);
  NEED(rig.until("PEAKS A.WAV DONE"));
  CHECK(rig.bank.pcm(2).frames == 1000u);  // slots 0 and 1 were taken
  CHECK(rig.arena->project.instrument(2) != nullptr);
  // Every slot taken: nothing to add into.
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    CHECK(rig.arena->project.set_instrument(i, taken) == model::Status::Ok);
  }
  rig.expire();
  rig.invoke("sample.load", 1);
  CHECK(rig.message() == "NO FREE INSTRUMENT");
}

TEST_CASE("sample load: what cannot be loaded says why and starts nothing: a folder, no project, no sample memory, no file") {
  {
    Rig rig;
    fs::create_directories(rig.dir.root / "snare.wav");
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    rig.invoke("sample.load", 0);
    CHECK(rig.message() == "A FOLDER: PRESS ENTER");
    CHECK(rig.bank.rebinds() == 0u);
    CHECK_FALSE(rig.bank.wav_open());
  }
  {
    Rig rig(200000, true, false);  // no document
    write_file(rig.dir.root / "a.wav", wav_bytes(100));
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    rig.invoke("sample.load", 0);
    CHECK(rig.message() == "NO PROJECT OPEN");
  }
  {
    Rig rig(200000, false, true);  // no bank
    write_file(rig.dir.root / "a.wav", wav_bytes(100));
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    rig.invoke("sample.load", 0);
    CHECK(rig.message() == "NO SAMPLE MEMORY");
  }
  {
    Rig rig;  // an empty card
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    CHECK(rig.index().count() == 0u);
    rig.invoke("sample.load", 0);
    CHECK(rig.message() == "NO FILE TO LOAD");
  }
}

TEST_CASE("sample load: a file that does not fit the arena, or is not a WAV, fails in the strip and leaves the slot, the arena and the file handle as they were") {
  Rig rig(1000);  // 1000 samples of arena
  write_file(rig.dir.root / "big.wav", wav_bytes(5000));
  write_file(rig.dir.root / "bad.wav", {'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v', 'e', 'f', 'i', 'l', 'e'});
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("big.wav");
  rig.invoke("sample.load", 0);
  NEED(rig.until("IMPORT BIG.WAV FAILED: TOO BIG FOR MEMORY"));
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.bank.pcm(0).frames == 0u);
  CHECK(rig.bank.free_samples() == 1000u);
  CHECK(rig.arena->project.instrument(0) == nullptr);
  CHECK_FALSE(rig.bank.rebind_pending());
  rig.expire();
  rig.select("bad.wav");
  rig.invoke("sample.load", 0);
  rig.passes(10);
  CHECK(rig.message().find("IMPORT BAD.WAV FAILED") == 0u);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.bank.free_samples() == 1000u);
  CHECK(rig.card.open_files() == 0u);
}

TEST_CASE("sample load: a file that is not 44.1 kHz is loaded as it is, with no pitch warning, and the voices get its rate factor (OS-RATEPITCH)") {
  Rig rig;
  write_file(rig.dir.root / "slow.wav", wav_bytes(2000, 22050));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS SLOW.WAV DONE"));
  CHECK(rig.arena->project.instrument(0)->sample_rate == 22050u);
  CHECK(rig.bank.pcm(0).frames == 2000u);
  rig.passes(4);  // the engine follows when the pool is quiet
  CHECK(rig.voices->sample(0).length == 2000u);
  CHECK(rig.voices->sample(0).rate_q28 == audio::rate_factor_q28(22050));  // the voice plays it at 0.5 frames a sample at the root
  CHECK(rig.voices->sample(0).rate_q28 == (1u << 27));
}

TEST_CASE("sample load: a second load while one runs is refused; the first completes") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(60000));
  write_file(rig.dir.root / "b.wav", wav_bytes(1000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("a.wav");
  rig.invoke("sample.load", 0);
  CHECK(rig.bank.wav_open());
  rig.select("b.wav");
  rig.invoke("instrument.select", 2);
  rig.invoke("sample.load", 0);  // refused ("AN IMPORT IS ALREADY RUNNING" waits behind the strip): the first goes on, the second never starts
  NEED(rig.until("PEAKS A.WAV DONE", 800));
  CHECK(rig.bank.pcm(0).frames == 60000u);
  CHECK(rig.bank.pcm(1).frames == 0u);
  CHECK(rig.arena->project.instrument(1) == nullptr);
}

TEST_CASE("sample load: F5 with no preview memory says so (the audition itself is audition_test.cpp); F6 reads the folder again") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(100));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.tap(Button::F5);
  CHECK(rig.message() == "NO PREVIEW MEMORY");
  write_file(rig.dir.root / "b.wav", wav_bytes(100));
  CHECK(rig.index().count() == 1u);
  rig.expire();
  rig.tap(Button::F6);
  rig.settle();
  CHECK(rig.index().count() == 2u);
  CHECK(rig.message() == "READ AGAIN: 2 ITEMS");  // OS-QAFIX-RESCAN
}

TEST_CASE("sample load: F6 on an empty folder says NO ITEMS (OS-QAFIX-RESCAN)") {
  Rig rig;
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->state() == app::DirScan::State::Done);
  CHECK(rig.index().count() == 0u);
  rig.expire();
  rig.tap(Button::F6);
  rig.settle();
  CHECK(rig.message() == "READ AGAIN: NO ITEMS");
}

TEST_CASE("sample load: the keys are the screen's: F1 loads, F2 goes up, a pad chooses the target instrument") {
  Rig rig;
  write_file(rig.dir.root / "d" / "a.wav", wav_bytes(1500));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.tap(Button::F3);  // enter "d"
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "d");
  rig.input.push(core::make_pad_event(rig.clock.now(), 6, 100));  // pad 6: instrument 7
  rig.pass();
  CHECK(rig.app->instrument() == 7);
  rig.tap(Button::F1);
  NEED(rig.until("PEAKS A.WAV DONE"));
  CHECK(rig.bank.pcm(6).frames == 1500u);
  rig.tap(Button::F2);
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder).empty());
}

// ---- OS-GRAN: wavetables through the Sample load screen --------------------------------------------------------------------------------------------------------------------------
namespace {

// A WAV of `frames` frames with a Serum 'clm ' chunk (`clm` text) before the audio.
std::vector<std::uint8_t> serum_bytes(std::uint32_t frames, const char* clm) {
  std::vector<std::uint8_t> v = wav_bytes(frames);
  std::vector<std::uint8_t> chunk;
  tag(chunk, "clm ");
  put32(chunk, static_cast<std::uint32_t>(std::strlen(clm)));
  for (const char* c = clm; *c != '\0'; ++c) {
    chunk.push_back(static_cast<std::uint8_t>(*c));
  }
  if (std::strlen(clm) % 2u != 0u) {
    chunk.push_back(0);
  }
  v.insert(v.begin() + 36, chunk.begin(), chunk.end());  // after the fmt chunk (12 + 24 bytes), before data
  const std::uint32_t size = static_cast<std::uint32_t>(v.size() - 8);
  v[4] = static_cast<std::uint8_t>(size);
  v[5] = static_cast<std::uint8_t>(size >> 8);
  v[6] = static_cast<std::uint8_t>(size >> 16);
  v[7] = static_cast<std::uint8_t>(size >> 24);
  return v;
}

// A Surge .wt of `tables` tables of `n` float32 samples.
std::vector<std::uint8_t> surge_bytes(std::uint32_t n, std::uint32_t tables) {
  std::vector<std::uint8_t> v;
  tag(v, "vawt");
  put32(v, n);
  put16(v, tables);
  put16(v, 0);
  for (std::uint32_t t = 0; t < tables; ++t) {
    for (std::uint32_t i = 0; i < n; ++i) {
      const float f = static_cast<float>(t + 1) * 0.1f;
      std::uint32_t bits;
      std::memcpy(&bits, &f, sizeof bits);
      put32(v, bits);
    }
  }
  return v;
}

}  // namespace

TEST_CASE("sample load: a Serum WAV (clm) and a Surge .wt load as wavetables, bound to the engine with their windows; a plain WAV stays a sample") {
  Rig rig;
  write_file(rig.dir.root / "serum.wav", serum_bytes(4096, "<!>1024 20000000 wavetable"));
  write_file(rig.dir.root / "plain.wav", wav_bytes(4096));
  write_file(rig.dir.root / "pad.wt", surge_bytes(256, 6));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(rig.index().count() == 3u);  // the .wt is listed with the .wav files (OS-GRAN)
  rig.invoke("instrument.select", 1);  // slot 0
  rig.select("serum.wav");
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS SERUM.WAV DONE"));
  const model::Instrument* a = rig.arena->project.instrument(0);
  NEED(a != nullptr);
  CHECK(a->sampler.play_mode == 6);
  CHECK(a->sampler.sample_type == 1);
  CHECK(a->sampler.wavetable_window_size == 1024);
  rig.passes(4);
  CHECK(rig.voices->sample(0).wt_size == 1024u);
  CHECK(rig.voices->sample(0).wt_windows == 4u);
  rig.select("plain.wav");
  rig.invoke("instrument.select", 2);  // slot 1
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS PLAIN.WAV DONE"));
  CHECK(rig.arena->project.instrument(1)->sampler.play_mode == 0);
  rig.passes(4);
  CHECK(rig.voices->sample(1).wt_size == 0u);
  rig.select("pad.wt");
  rig.invoke("instrument.select", 3);  // slot 2
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS PAD.WT DONE"));
  const model::Instrument* c = rig.arena->project.instrument(2);
  NEED(c != nullptr);
  CHECK(c->sampler.play_mode == 6);
  CHECK(c->sampler.wavetable_window_size == 256);
  CHECK(std::string(c->name) == "pad");
  rig.passes(4);
  CHECK(rig.voices->sample(2).wt_size == 256u);
  CHECK(rig.voices->sample(2).wt_windows == 6u);
  CHECK(rig.bank.pcm(2).frames == 1536u);
}

TEST_CASE("sample load: F7 cycles how a WAV is read as a wavetable (AUTO, LEN, 256 .. 2048, OFF) and the next load follows it") {
  Rig rig;
  write_file(rig.dir.root / "loop.wav", wav_bytes(2048 * 2));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(rig.app->state().wav.wavetable == formats::kWtAuto);
  rig.tap(Button::F7);
  CHECK(rig.message() == "WAV AS WAVETABLE: FRAME BY LENGTH");
  CHECK(rig.app->state().wav.wavetable == formats::kWtDetect);
  rig.select("loop.wav");
  rig.invoke("instrument.select", 1);
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS LOOP.WAV DONE"));
  CHECK(rig.arena->project.instrument(0)->sampler.wavetable_window_size == 2048);
  CHECK(rig.arena->project.instrument(0)->sampler.play_mode == 6);
  const std::uint16_t expected[] = {256, 512, 1024, 2048, formats::kWtOff, formats::kWtAuto};
  for (const std::uint16_t m : expected) {
    rig.tap(Button::F7);
    CHECK(rig.app->state().wav.wavetable == m);
  }
  rig.tap(Button::F7);  // LEN again
  rig.tap(Button::F7);  // 256
  rig.select("loop.wav");
  rig.invoke("instrument.select", 2);
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS LOOP.WAV DONE"));
  CHECK(rig.arena->project.instrument(1)->sampler.wavetable_window_size == 256);
}

TEST_CASE("sample load: the listing takes .aif .aiff .aifc .8svx .svx .iff with the .wav files, whatever the case") {
  Rig rig;
  for (const char* n : {"a.wav", "b.AIF", "c.aiff", "d.AIFC", "e.8svx", "f.8SVX", "g.svx", "h.IFF"}) write_file(rig.dir.root / n, wav_bytes(100));
  write_file(rig.dir.root / "i.aifx", {1, 2});
  write_file(rig.dir.root / "j.mp3", {1, 2});
  write_file(rig.dir.root / ".aif", {1, 2});  // the extension alone
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(rig.index().count() == 8u);
}

TEST_CASE("sample load: F1 loads an AIFF: the rate kept (and the pitch warning), the audio, the loop from INST and MARK, the peaks; the header decides, not the extension") {
  Rig rig;
  write_file(rig.dir.root / "pad.aif", aiff_bytes(2000, true));
  write_file(rig.dir.root / "disguised.wav", aiff_bytes(500));  // an AIFF called .wav
  write_file(rig.dir.root / "realwav.aiff", wav_bytes(500));    // a WAV called .aiff
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("instrument.select", 1);  // slot 0
  rig.select("pad.aif");
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS PAD.AIF DONE"));  // (the rate is kept: nothing is resampled; the voices play it at its pitch)
  CHECK_FALSE(rig.bank.wav_open());
  const app::SampleBank::Pcm pcm = rig.bank.pcm(0);
  NEED(pcm.data != nullptr);
  CHECK(pcm.frames == 2000u);
  for (std::uint32_t i = 0; i < 2000; ++i) {
    if (pcm.data[i] != ramp(i)) {
      FAIL_CHECK("sample " << i << " differs");
      break;
    }
  }
  const model::Instrument* in = rig.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(std::string(in->name) == "pad");
  CHECK(in->sample_rate == 22050u);
  CHECK(in->sampler.play_mode == 1);
  CHECK(in->sampler.loop_start == static_cast<std::uint16_t>((100u * 65535u + 1000u) / 2000u));
  CHECK(in->sampler.loop_end == static_cast<std::uint16_t>((300u * 65535u + 1000u) / 2000u));
  rig.expire();
  rig.invoke("instrument.select", 2);  // slot 1
  rig.select("disguised.wav");
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS DISGUISED.WAV DONE"));
  CHECK(rig.bank.pcm(1).frames == 500u);
  CHECK(rig.arena->project.instrument(1)->sample_rate == 22050u);
  rig.expire();
  rig.invoke("instrument.select", 3);  // slot 2
  rig.select("realwav.aiff");
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS REALWAV.AIFF DONE"));
  CHECK(rig.bank.pcm(2).frames == 500u);
  CHECK(rig.arena->project.instrument(2)->sample_rate == 44100u);
  CHECK(rig.card.open_files() == 0u);
}

TEST_CASE("sample load: F1 loads an 8SVX at its own rate, the repeat part looping forward, named by its NAME chunk") {
  Rig rig;
  write_file(rig.dir.root / "bass.8svx", svx_bytes(200, 301));
  write_file(rig.dir.root / "broken.iff", {'F', 'O', 'R', 'M', 0, 0, 0, 4, '8', 'S', 'V', 'X'});
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("bass.8svx");
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS BASS.8SVX DONE"));
  const app::SampleBank::Pcm pcm = rig.bank.pcm(0);
  NEED(pcm.data != nullptr);
  CHECK(pcm.frames == 501u);
  const model::Instrument* in = rig.arena->project.instrument(0);
  NEED(in != nullptr);
  CHECK(std::string(in->name) == "Amiga bass");
  CHECK(in->sample_rate == 16726u);
  CHECK(in->sampler.play_mode == 1);
  CHECK(in->sampler.loop_start == static_cast<std::uint16_t>((200u * 65535u + 250u) / 501u));
  CHECK(in->sampler.loop_end == 65535);
  CHECK(pcm.data[7] == (ramp(7) >> 8) * 256);
  // a file with no chunks fails in the strip and leaves the slot and the handle as they were
  rig.expire();
  rig.invoke("instrument.select", 2);
  rig.select("broken.iff");
  rig.invoke("sample.load", 0);
  rig.passes(10);
  CHECK(rig.message().find("IMPORT BROKEN.IFF FAILED") == 0u);
  CHECK(rig.arena->project.instrument(1) == nullptr);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.card.open_files() == 0u);
}

// ---- OS-LOADCH: the CHANNEL of a stereo file (F8), the "ST" tag, and the AIFF whose SSND comes first ----------------------------------------------------------------------

namespace {

// The two channels of the stereo test files: different everywhere, never clipping.
std::int16_t left_of(std::uint32_t i) { return ramp(i); }
std::int16_t right_of(std::uint32_t i) { return static_cast<std::int16_t>(-ramp(i) / 2 + 5); }

// A stereo 16-bit WAV of `frames` frames; `pre` extra bytes of a LIST chunk (odd size: padded) in front of 'fmt ' like a DAW's header.
std::vector<std::uint8_t> stereo_wav_bytes(std::uint32_t frames, std::uint32_t pre = 0) {
  std::vector<std::uint8_t> v;
  tag(v, "RIFF");
  put32(v, 0);
  tag(v, "WAVE");
  if (pre != 0) {
    tag(v, "LIST");
    put32(v, pre);
    for (std::uint32_t i = 0; i < pre + (pre & 1u); ++i) v.push_back('x');
  }
  tag(v, "fmt ");
  put32(v, 16);
  put16(v, 1);
  put16(v, 2);
  put32(v, 44100);
  put32(v, 44100 * 4);
  put16(v, 4);
  put16(v, 16);
  tag(v, "data");
  put32(v, frames * 4);
  for (std::uint32_t i = 0; i < frames; ++i) {
    put16(v, static_cast<std::uint16_t>(left_of(i)));
    put16(v, static_cast<std::uint16_t>(right_of(i)));
  }
  const std::uint32_t n = static_cast<std::uint32_t>(v.size() - 8);
  for (int k = 0; k < 4; ++k) v[4 + k] = static_cast<std::uint8_t>(n >> (8 * k));
  return v;
}

// A stereo 16-bit AIFF of `frames` frames at 22,050 Hz; `ssnd_first` puts SSND in front of COMM (the known limit).
std::vector<std::uint8_t> stereo_aiff_bytes(std::uint32_t frames, bool ssnd_first = false) {
  std::vector<std::uint8_t> v;
  tag(v, "FORM");
  be32(v, 0);
  tag(v, "AIFF");
  const auto comm = [&] {
    tag(v, "COMM");
    be32(v, 18);
    be16(v, 2);
    be32(v, frames);
    be16(v, 16);
    be16(v, 0x400D);
    be32(v, 22050u << 17);
    be32(v, 0);
  };
  const auto ssnd = [&] {
    tag(v, "SSND");
    be32(v, 8 + frames * 4);
    be32(v, 0);
    be32(v, 0);
    for (std::uint32_t i = 0; i < frames; ++i) {
      be16(v, static_cast<std::uint16_t>(left_of(i)));
      be16(v, static_cast<std::uint16_t>(right_of(i)));
    }
  };
  if (ssnd_first) {
    ssnd();
    comm();
  } else {
    comm();
    ssnd();
  }
  be_size(v, 4);
  return v;
}

// The audio of a slot, checked against `want(i)` for every frame.
template <typename F>
bool pcm_is(const app::SampleBank::Pcm& pcm, std::uint32_t frames, F want) {
  if (pcm.data == nullptr || pcm.frames != frames) {
    return false;
  }
  for (std::uint32_t i = 0; i < frames; ++i) {
    if (pcm.data[i] != want(i)) {
      return false;
    }
  }
  return true;
}

struct VecProbe {
  const std::vector<std::uint8_t>& bytes;
  unsigned reads = 0;
};
bool vec_read(void* ctx, std::uint32_t offset, std::uint8_t* dst, std::size_t n) {
  VecProbe& p = *static_cast<VecProbe*>(ctx);
  ++p.reads;
  if (static_cast<std::uint64_t>(offset) + n > p.bytes.size()) {
    return false;
  }
  std::memcpy(dst, p.bytes.data() + offset, n);
  return true;
}
unsigned probe_of(const std::vector<std::uint8_t>& bytes, unsigned* reads = nullptr) {
  VecProbe p{bytes};
  const unsigned c = formats::sample_channels(vec_read, &p);
  if (reads != nullptr) {
    *reads = p.reads;
  }
  return c;
}

}  // namespace

TEST_CASE("sample load: the header probe says 1 or 2 channels for a WAV and an AIFF, past the chunks in front, and 0 for anything it cannot place") {
  CHECK(probe_of(wav_bytes(10)) == 1u);
  CHECK(probe_of(stereo_wav_bytes(10)) == 2u);
  CHECK(probe_of(stereo_wav_bytes(10, 601)) == 2u);  // a bext-sized LIST (odd length, padded) first
  CHECK(probe_of(aiff_bytes(10)) == 1u);
  CHECK(probe_of(stereo_aiff_bytes(10)) == 2u);
  CHECK(probe_of(stereo_aiff_bytes(10, true)) == 0u);  // SSND first: the load refuses it, so it is not tagged
  CHECK(probe_of(svx_bytes(10, 0)) == 0u);              // 8SVX is mono and not probed for
  CHECK(probe_of({1, 2, 3}) == 0u);
  CHECK(probe_of({}) == 0u);
  std::vector<std::uint8_t> six = stereo_wav_bytes(4);
  six[22] = 6;  // six channels: not 1 or 2
  CHECK(probe_of(six) == 0u);
  std::vector<std::uint8_t> cut = stereo_wav_bytes(4);
  cut.resize(20);  // ends inside the fmt chunk
  CHECK(probe_of(cut) == 0u);
  // a header that never comes is given up after kProbeHops chunks, not walked for ever
  std::vector<std::uint8_t> junk = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  for (int i = 0; i < 40; ++i) {
    for (const char* c : {"junk", "\0\0\0\0"}) {
      for (int k = 0; k < 4; ++k) junk.push_back(static_cast<std::uint8_t>(c[k]));
    }
  }
  unsigned reads = 0;
  CHECK(probe_of(junk, &reads) == 0u);
  CHECK(reads <= 1u + formats::kProbeHops);
}

TEST_CASE("sample load: the list tags a stereo WAV or AIFF with ST from its header; mono, 8SVX, SSND-first and damaged files carry no tag, and no file is left open") {
  Rig rig;
  write_file(rig.dir.root / "a_mono.wav", wav_bytes(100));
  write_file(rig.dir.root / "b_st.wav", stereo_wav_bytes(100));
  write_file(rig.dir.root / "c_st_list.WAV", stereo_wav_bytes(100, 33));
  write_file(rig.dir.root / "d_mono.aif", aiff_bytes(100));
  write_file(rig.dir.root / "e_st.aif", stereo_aiff_bytes(100));
  write_file(rig.dir.root / "f_st.AIFC", stereo_aiff_bytes(100));
  write_file(rig.dir.root / "g_ssnd_first.aif", stereo_aiff_bytes(100, true));
  write_file(rig.dir.root / "h_bass.8svx", svx_bytes(100, 0));
  write_file(rig.dir.root / "i_bad.wav", {'R', 'I', 'F', 'F', 0, 0});
  fs::create_directories(rig.dir.root / "j_dir");
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.index().count() == 10u);
  CHECK(rig.name_at(0) == "j_dir");
  CHECK_FALSE(rig.index().stereo(0));
  const std::string stereo[] = {"b_st.wav", "c_st_list.WAV", "e_st.aif", "f_st.AIFC"};
  const std::string plain[] = {"a_mono.wav", "d_mono.aif", "g_ssnd_first.aif", "h_bass.8svx", "i_bad.wav"};
  for (unsigned i = 1; i < rig.index().count(); ++i) {
    bool want = false;
    for (const std::string& s : stereo) want = want || s == rig.name_at(i);
    CHECK_MESSAGE(rig.index().stereo(i) == want, rig.name_at(i));
  }
  (void)plain;
  CHECK(rig.card.open_files() == 0u);
  CHECK(rig.index().tag(1) == app::EntryTag::None);  // the tag does not touch the File screen's format tag
}

TEST_CASE("sample load: F8 CHANNEL acts only on a stereo file, cycles MIX, LEFT, RIGHT, and the choice is remembered for the session") {
  Rig rig;
  write_file(rig.dir.root / "mono.wav", wav_bytes(100));
  write_file(rig.dir.root / "st.wav", stereo_wav_bytes(100));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(rig.app->state().wav.channel == 0u);
  rig.select("mono.wav");
  rig.tap(Button::F8);
  CHECK(rig.message() == "ONLY FOR A STEREO FILE");  // the option is not there for a mono file
  CHECK(rig.app->state().wav.channel == 0u);
  rig.expire();
  rig.select("st.wav");
  rig.tap(Button::F8);
  CHECK(rig.message() == "STEREO FILE LOADS: LEFT");
  CHECK(rig.app->state().wav.channel == 1u);
  rig.tap(Button::F8);
  CHECK(rig.message() == "STEREO FILE LOADS: RIGHT");
  CHECK(rig.app->state().wav.channel == 2u);
  // the session remembers it: another file, another screen and back
  rig.select("mono.wav");
  rig.go(ScreenId::Pattern);
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  CHECK(rig.app->state().wav.channel == 2u);
  rig.select("st.wav");
  rig.tap(Button::F8);  // OS-STEREO (b), S8: the fourth choice, only here (a stereo file is under the cursor)
  CHECK(rig.message() == "STEREO FILE LOADS: STEREO (MID + SIDE)");
  CHECK(rig.app->state().wav.channel == app::kChannelStereo);
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.message() == "STEREO FILE LOADS: MIX (L+R)/2");
  CHECK(rig.app->state().wav.channel == 0u);
  // the labels
  CHECK(std::string(app::channel_key_label(0)) == "CH MIX");
  CHECK(std::string(app::channel_key_label(1)) == "CH LEFT");
  CHECK(std::string(app::channel_key_label(2)) == "CH RIGHT");
  CHECK(std::string(app::channel_key_label(app::kChannelStereo)) == "STEREO");
  for (std::uint8_t c = 0; c <= app::kChannelStereo; ++c) {
    CHECK(std::strlen(app::channel_key_label(c)) <= 8u);  // style-guide.md: a key label has at most 8 characters
  }
  CHECK(std::string(app::kSampleLoadSoftKeys[7]) == "CH MIX");
}

TEST_CASE("sample load: F1 loads the LEFT or the RIGHT channel of a stereo WAV and AIFF bit for bit, MIX as before; a mono file ignores the choice") {
  Rig rig;
  const std::uint32_t frames = 3000;
  write_file(rig.dir.root / "st.wav", stereo_wav_bytes(frames));
  write_file(rig.dir.root / "st.aif", stereo_aiff_bytes(frames));
  write_file(rig.dir.root / "mono.wav", wav_bytes(frames));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  const auto load = [&](const char* file, unsigned slot, const char* done) {
    rig.invoke("instrument.select", static_cast<core::ActionArg>(slot + 1));
    rig.select(file);
    rig.invoke("sample.load", 0);
    const bool ok = rig.until(done);
    rig.expire();
    return ok;
  };
  const auto choose = [&](unsigned channel) {  // F8, with the cursor on a stereo file, until the channel is the one wanted (as a user does)
    rig.select("st.wav");
    for (unsigned i = 0; i < 3 && rig.app->state().wav.channel != channel; ++i) {
      rig.tap(Button::F8);
    }
    CHECK(rig.app->state().wav.channel == channel);
    rig.expire();
  };
  const auto left = [](std::uint32_t i) { return left_of(i); };
  const auto right = [](std::uint32_t i) { return right_of(i); };
  // MIX (the default) is the mean, as before: within one step of (L + R) / 2 (a sum that is odd is dithered)
  NEED(load("st.wav", 0, "PEAKS ST.WAV DONE"));
  CHECK(pcm_is(rig.bank.pcm(0), frames, [&](std::uint32_t i) {
    const int mean = (left_of(i) + right_of(i)) / 2;
    const int got = rig.bank.pcm(0).data[i];
    return static_cast<std::int16_t>(got >= mean - 1 && got <= mean + 1 ? got : mean + 100);
  }));
  choose(1);
  NEED(load("st.wav", 1, "PEAKS ST.WAV DONE"));
  CHECK(pcm_is(rig.bank.pcm(1), frames, left));
  CHECK(rig.arena->project.instrument(1)->sample_rate == 44100u);
  choose(2);
  NEED(load("st.wav", 2, "PEAKS ST.WAV DONE"));
  CHECK(pcm_is(rig.bank.pcm(2), frames, right));
  // the same for the AIFF (22,050 Hz)
  choose(1);
  NEED(load("st.aif", 3, "PEAKS ST.AIF DONE"));
  CHECK(pcm_is(rig.bank.pcm(3), frames, left));
  CHECK(rig.arena->project.instrument(3)->sample_rate == 22050u);
  choose(2);
  NEED(load("st.aif", 4, "PEAKS ST.AIF DONE"));
  CHECK(pcm_is(rig.bank.pcm(4), frames, right));
  // a mono file ignores the choice
  NEED(load("mono.wav", 5, "PEAKS MONO.WAV DONE"));
  CHECK(pcm_is(rig.bank.pcm(5), frames, [](std::uint32_t i) { return ramp(i); }));
  // a RIGHT or LEFT load takes no more room than the mono file it makes
  CHECK(rig.bank.pcm(2).frames == frames);
  CHECK(rig.card.open_files() == 0u);
}

TEST_CASE("sample load: CH STEREO loads the mid exactly as MIX does and the side signal after it, sets the instrument's flag; a mono file, an AIFF, and a load that does not fit both say so (OS-STEREO (b))") {
  Rig rig;
  const std::uint32_t frames = 3000;
  write_file(rig.dir.root / "st.wav", stereo_wav_bytes(frames));
  write_file(rig.dir.root / "st.aif", stereo_aiff_bytes(frames));
  write_file(rig.dir.root / "mono.wav", wav_bytes(frames));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  const auto load = [&](const char* file, unsigned slot, const char* done) {
    rig.invoke("instrument.select", static_cast<core::ActionArg>(slot + 1));
    rig.select(file);
    rig.invoke("sample.load", 0);
    const bool ok = rig.until(done);
    rig.expire();
    return ok;
  };
  const auto stereo_mode = [&](const char* file) {
    rig.select(file);
    for (unsigned i = 0; i < 4 && rig.app->state().wav.channel != app::kChannelStereo; ++i) {
      rig.tap(Button::F8);
    }
    CHECK(rig.app->state().wav.channel == app::kChannelStereo);
    rig.expire();
  };
  // MIX first: the mid every other mode of this file would make
  NEED(load("st.wav", 0, "PEAKS ST.WAV DONE"));
  CHECK_FALSE(rig.bank.has_side(0));
  CHECK(rig.arena->project.instrument(0)->stereo == 0u);
  const std::vector<std::int16_t> mix(rig.bank.pcm(0).data, rig.bank.pcm(0).data + frames);
  stereo_mode("st.wav");
  NEED(load("st.wav", 1, "PEAKS ST.WAV DONE"));
  const app::SampleBank::Pcm st = rig.bank.pcm(1);
  NEED(st.frames == frames);
  NEED(st.side != nullptr);
  CHECK(st.side == st.data + frames);  // 3000 is even
  CHECK(std::memcmp(st.data, mix.data(), frames * 2u) == 0);  // M is bit-identical to the mono load
  bool side_ok = true;
  for (std::uint32_t i = 0; i < frames; ++i) {
    side_ok = side_ok && st.side[i] == static_cast<std::int16_t>(left_of(i) - st.data[i]);
  }
  CHECK(side_ok);
  CHECK(rig.arena->project.instrument(1)->stereo == 1u);
  CHECK(rig.arena->project.instrument(1)->sample_rate == 44100u);
  CHECK(rig.bank.free_samples() == rig.bank.arena_samples() - 3u * frames);  // the MIX load (3000) and the mid + side (6000)
  // Loading MIX over it again makes a mono instrument (a fresh one: flag 0)
  const_cast<app::ViewState&>(rig.app->state()).wav.channel = 0;
  rig.invoke("instrument.select", 2);
  rig.select("st.wav");
  rig.invoke("sample.load", 0);  // over an instrument that is there: asks first
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS ST.WAV DONE"));
  rig.expire();
  CHECK_FALSE(rig.bank.has_side(1));
  CHECK(rig.arena->project.instrument(1)->stereo == 0u);
  // A mono file ignores the mode: no S, no flag, no message.
  stereo_mode("st.wav");
  NEED(load("mono.wav", 2, "PEAKS MONO.WAV DONE"));
  CHECK_FALSE(rig.bank.has_side(2));
  CHECK(rig.arena->project.instrument(2)->stereo == 0u);
  // An AIFF is read mono by its reader: loaded as such, and the strip says so.
  NEED(load("st.aif", 3, "PEAKS ST.AIF DONE"));
  CHECK_FALSE(rig.bank.has_side(3));
  CHECK(rig.arena->project.instrument(3)->stereo == 0u);
  CHECK(rig.card.open_files() == 0u);
}

TEST_CASE("sample load: STEREO is refused with a strip message when another instrument's sample has the side file's name, and nothing is loaded; MIX still loads (OS-STEREO (b))") {
  Rig rig;
  write_file(rig.dir.root / "st.wav", stereo_wav_bytes(3000));
  model::Instrument other;
  model::init_instrument(other, model::InstrumentKind::Sample);
  std::strcpy(other.sample_path, "samples/Instr05_S.wav");  // the name the side file of slot 5 (index 4) would take, in another case
  NEED(rig.arena->project.set_instrument(7, other) == model::Status::Ok);
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("instrument.select", 5);
  rig.select("st.wav");
  const_cast<app::ViewState&>(rig.app->state()).wav.channel = app::kChannelStereo;
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "STEREO REFUSED: SIDE FILE NAME TAKEN");
  rig.settle();
  CHECK(rig.bank.pcm(4).frames == 0u);
  CHECK(rig.card.open_files() == 0u);
  rig.expire();
  const_cast<app::ViewState&>(rig.app->state()).wav.channel = 0;  // MIX has no side file: it loads
  rig.invoke("sample.load", 0);
  CHECK(rig.until("PEAKS ST.WAV DONE"));
  CHECK(rig.bank.pcm(4).frames == 3000u);
}

TEST_CASE("sample load: CH STEREO on a file whose mid fits but whose mid and side do not fails as a too long file does, and the slot is untouched (OS-STEREO (b))") {
  Rig rig(7000);  // 3000 frames: the mid alone fits twice, mid + side (6000) once
  write_file(rig.dir.root / "st.wav", stereo_wav_bytes(3000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("st.wav");
  for (unsigned i = 0; i < 4 && rig.app->state().wav.channel != app::kChannelStereo; ++i) {
    rig.tap(Button::F8);
  }
  rig.expire();
  rig.invoke("instrument.select", 1);
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS ST.WAV DONE"));  // 6000 of 7000: both fit
  CHECK(rig.bank.has_side(0));
  rig.expire();
  rig.invoke("instrument.select", 2);
  rig.invoke("sample.load", 0);
  rig.passes(40);
  CHECK(rig.message().find("IMPORT ST.WAV FAILED") == 0u);  // 1000 left: not even the mid
  CHECK(rig.arena->project.instrument(1) == nullptr);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.card.open_files() == 0u);
}

TEST_CASE("sample load: an AIFF whose SSND comes before COMM is refused with its own words, not DAMAGED FILE, and the slot is untouched") {
  Rig rig;
  write_file(rig.dir.root / "t_ssnd_before_comm.aif", stereo_aiff_bytes(100, true));
  write_file(rig.dir.root / "u_damaged.wav", {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'});
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("instrument.select", 1);
  rig.select("t_ssnd_before_comm.aif");
  rig.invoke("sample.load", 0);
  NEED(rig.until("AIFF: SSND BEFORE COMM NOT SUPPORTED"));
  CHECK(rig.arena->project.instrument(0) == nullptr);
  CHECK_FALSE(rig.bank.wav_open());
  rig.expire();
  rig.select("u_damaged.wav");  // any other damage keeps the generic words
  rig.invoke("sample.load", 0);
  rig.passes(10);
  CHECK(rig.message().find("IMPORT U_DAMAGED.WAV FAILED") == 0u);
  CHECK(rig.card.open_files() == 0u);
}

// OS-BUG-PASTE24 (docs/specs/OS-BUG-PASTE24.md section 5, D3): the WAV, AIFF / 8SVX and Surge readers (formats) make a sample instrument with wav::make_sample_instrument, which leaves the level at init_instrument's 50: 0 dB
// under the 1.2+ law, about -20 dB under the 1.1.14 table. formats does not know the law, so the app gives the loaded instrument the unity of the document's law when the load has ended. (The reader replaces the
// slot's instrument whole, so a load into a slot that held one is a fresh instrument too.)
TEST_CASE("sample load: a loaded sample instrument is born at 0 dB under the project's law: a WAV, an AIFF, an 8SVX and a Surge .wt, in a free slot and over another (OS-BUG-PASTE24)") {
  for (const std::uint8_t law : {std::uint8_t{1}, std::uint8_t{0}}) {
    CAPTURE(law);
    Rig rig;
    rig.arena->project.set_volume_law(law);
    const unsigned unity = law != 0u ? 100u : 50u;
    write_file(rig.dir.root / "kick.wav", wav_bytes(1000));
    write_file(rig.dir.root / "pad.aif", aiff_bytes(800));
    write_file(rig.dir.root / "bass.8svx", svx_bytes(200, 301));
    write_file(rig.dir.root / "pad.wt", surge_bytes(256, 4));
    write_file(rig.dir.root / "snare.wav", wav_bytes(600));
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    const struct {
      const char* file;
      const char* done;
    } loads[] = {{"kick.wav", "PEAKS KICK.WAV DONE"}, {"pad.aif", "PEAKS PAD.AIF DONE"}, {"bass.8svx", "PEAKS BASS.8SVX DONE"}, {"pad.wt", "PEAKS PAD.WT DONE"}, {"snare.wav", "PEAKS SNARE.WAV DONE"}};
    for (unsigned i = 0; i < 5; ++i) {
      CAPTURE(loads[i].file);
      const unsigned slot = i < 4 ? i : 0;  // the last one goes over slot 0, which holds the WAV
      rig.invoke("instrument.select", static_cast<core::ActionArg>(slot + 1));
      rig.select(loads[i].file);
      rig.invoke("sample.load", 0);
      if (i == 4) {  // over an instrument that is there: asked first (OS-BUG-SLDOVER)
        rig.invoke("sample.load", 0);
      }
      NEED(rig.until(loads[i].done));
      const model::Instrument* in = rig.arena->project.instrument(slot);
      NEED(in != nullptr);
      CHECK(in->sampler.volume == unity);
      rig.passes(4);
      CHECK(rig.voices->sample(slot).gain == 65536);  // 0 dB in the voices once the bank has rebuilt the descriptor (the bank follows the document's law)
    }
  }
}

// OS-BUG-SLDOVER (maintainer policy 2026-10-22): a WAV over an instrument that is there asks first and is no undo unit; a free slot asks nothing; the history of the pattern stays.
TEST_CASE("sample load: F1 over an instrument that is there asks first, a free slot asks nothing, and the undo history of the pattern stays (OS-BUG-SLDOVER)") {
  Rig rig;
  write_file(rig.dir.root / "kick.wav", wav_bytes(1000));
  write_file(rig.dir.root / "snare.wav", wav_bytes(600));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("instrument.select", 1);
  rig.select("kick.wav");
  rig.invoke("sample.load", 0);  // slot 1 is free: no question
  CHECK(rig.message().find("REPLACE") == std::string::npos);
  NEED(rig.until("PEAKS KICK.WAV DONE"));
  rig.passes(4);
  NEED(rig.editor->set_tempo_x100(14321u) == model::Status::Ok);  // something to undo
  NEED(rig.editor->has_history());
  rig.select("snare.wav");
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "REPLACE 01 kick: F1 AGAIN");
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.bank.pcm(0).frames == 1000u);  // nothing was replaced
  rig.invoke("sample.load", 0);  // the answer
  CHECK(rig.message().find("REPLACE") == std::string::npos);
  NEED(rig.until("PEAKS SNARE.WAV DONE"));
  CHECK(rig.bank.pcm(0).frames == 600u);
  CHECK(rig.editor->has_history());  // the load is no undo unit and ends no other history
  // Another file under the cursor starts a new question.
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "REPLACE 01 snare: F1 AGAIN");
  rig.select("kick.wav");
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "REPLACE 01 snare: F1 AGAIN");
  CHECK_FALSE(rig.bank.wav_open());
  // The question times out: the next F1 asks again.
  rig.clock.advance(5000u * core::kTicksPerMs);
  rig.pass();
  rig.invoke("sample.load", 0);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.message() == "REPLACE 01 snare: F1 AGAIN");
}

TEST_CASE("sample load: a WAV onto a module slot is refused with the way out, and nothing changes (OS-BUG-SLDOVER, OS-INSTUI D9)") {
  Rig rig;
  write_file(rig.dir.root / "kick.wav", wav_bytes(1000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.arena->project.add_module("tb303", 5, 1, 0) == model::Status::Ok);
  model::Instrument mod;
  model::init_instrument(mod, model::InstrumentKind::Module);
  std::strcpy(mod.name, "ACID");
  mod.module_index = 0;
  NEED(rig.arena->project.set_instrument(3, mod) == model::Status::Ok);
  rig.invoke("instrument.select", 4);
  rig.select("kick.wav");
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "SLOT HAS A MODULE: INSTRUMENT, SHIFT+F4 DELETE");
  CHECK_FALSE(rig.bank.wav_open());
  rig.invoke("sample.load", 0);  // asking again does not load either
  CHECK(rig.message() == "SLOT HAS A MODULE: INSTRUMENT, SHIFT+F4 DELETE");
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.arena->project.instrument(3)->kind == model::InstrumentKind::Module);
}

TEST_CASE("sample load: Shift + F7 / F8 move the target instrument round 1..48, and a question about the old target lapses (OS-SLDTARGET)") {
  Rig rig;
  write_file(rig.dir.root / "kick.wav", wav_bytes(1000));
  write_file(rig.dir.root / "snare.wav", wav_bytes(600));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("instrument.select", 1);
  rig.select("kick.wav");
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS KICK.WAV DONE"));
  rig.passes(4);
  rig.select("snare.wav");
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "REPLACE 01 kick: F1 AGAIN");
  CHECK(app::sample_question_live(rig.app->state()));
  rig.invoke("sample.target", 1);
  CHECK(rig.app->state().instrument == 2);
  CHECK_FALSE(app::sample_question_live(rig.app->state()));  // the question was about slot 1
  rig.invoke("sample.target", -1);
  CHECK(rig.app->state().instrument == 1);
  rig.invoke("sample.target", -1);  // round: one back from 1 is 64 (OS-QAFIX-E-LOAD-25: the Instrument screen's range)
  CHECK(rig.app->state().instrument == 64);
  rig.invoke("sample.target", 1);
  CHECK(rig.app->state().instrument == 1);
  CHECK(std::string(app::kSampleLoadShiftKeys[6]) == "INST -");
  CHECK(std::string(app::kSampleLoadShiftKeys[7]) == "INST +");
  // F1 now loads into the new target: slot 2 is free, so no question and the file lands there
  rig.invoke("sample.target", 1);
  rig.invoke("sample.load", 0);
  CHECK(rig.message().find("REPLACE") == std::string::npos);
  NEED(rig.until("PEAKS SNARE.WAV DONE"));
  CHECK(rig.bank.pcm(1).frames == 600u);
  CHECK(rig.bank.pcm(0).frames == 1000u);
}

// The instrument in view becomes `slot` (1..64) the way a person gets to a MIDI slot: the Instrument screen's step key (a pad reaches 1..48 only), then back to Sample load.
static void view_slot(Rig& rig, int slot) {
  rig.go(ScreenId::Instrument);
  for (int guard = 0; guard < 70 && rig.app->state().instrument != slot; ++guard) {
    rig.invoke("instrument.step", rig.app->state().instrument < slot ? 1 : -1);
  }
  rig.go(ScreenId::SampleLoad);
  rig.settle();
}

TEST_CASE("sample load: Shift + F7 / F8 step and wrap over 1..64, the Instrument screen's range (OS-SLDTARGET, OS-QAFIX-E-LOAD-25)") {
  Rig rig;
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  view_slot(rig, 64);
  NEED(rig.app->state().instrument == 64);
  rig.invoke("sample.target", -1);
  CHECK(rig.app->state().instrument == 63);
  rig.invoke("sample.target", 1);
  CHECK(rig.app->state().instrument == 64);
  rig.invoke("sample.target", 1);  // Shift + F8 from 64 is round to 1
  CHECK(rig.app->state().instrument == 1);
  rig.invoke("sample.target", -1);  // and Shift + F7 from 1 to 64
  CHECK(rig.app->state().instrument == 64);
  view_slot(rig, 48);
  rig.invoke("sample.target", 1);  // 48 steps on into the slots 49..
  CHECK(rig.app->state().instrument == 49);
}

TEST_CASE("sample load: a slot 49..64 that is empty or a sample is a valid target, F4 ADD scans all 64, a MIDI-type slot is refused (OS-QAFIX-E-LOAD-25)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(1000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("a.wav");
  // An empty slot 55 takes the file.
  view_slot(rig, 55);
  NEED(rig.app->state().instrument == 55);
  rig.invoke("sample.load", 0);
  CHECK(rig.message().find("REFUSED") == std::string::npos);
  NEED(rig.until("PEAKS A.WAV DONE"));
  rig.passes(4);
  NEED(rig.arena->project.instrument(54) != nullptr);
  CHECK(rig.arena->project.instrument(54)->kind == model::InstrumentKind::Sample);
  // A MIDI-type instrument in slot 60 refuses the load and stays.
  model::Instrument midi;
  model::init_instrument(midi, model::InstrumentKind::Midi);
  std::strcpy(midi.name, "KEYS");
  midi.midi_channel = 3;
  NEED(rig.arena->project.set_instrument(59, midi) == model::Status::Ok);
  view_slot(rig, 60);
  NEED(rig.app->state().instrument == 60);
  rig.expire();
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "SLOT IS MIDI: INSTRUMENT, SHIFT+F4 DELETE");
  CHECK_FALSE(rig.bank.wav_open());
  rig.invoke("sample.load", 0);  // asking again does not load either
  CHECK(rig.message() == "SLOT IS MIDI: INSTRUMENT, SHIFT+F4 DELETE");
  CHECK(rig.arena->project.instrument(59)->kind == model::InstrumentKind::Midi);
  // 1..48 taken, and 49..54 and 56..59 too (55 holds the sample loaded above, 60 is MIDI): the used slots of 49..64 are skipped and ADD takes 61, the first free one.
  model::Instrument taken;
  model::init_instrument(taken, model::InstrumentKind::Sample);
  for (std::uint32_t i = 0; i < 59u; ++i) {
    if (i != 54u) {
      NEED(rig.arena->project.set_instrument(i, taken) == model::Status::Ok);
    }
  }
  rig.expire();
  rig.invoke("sample.load", 1);
  CHECK(rig.message().find("NO FREE") == std::string::npos);
  NEED(rig.until("PEAKS A.WAV DONE"));
  rig.passes(4);
  NEED(rig.arena->project.instrument(60) != nullptr);
  CHECK(rig.bank.pcm(60).frames == 1000u);
  CHECK(rig.arena->project.instrument(59)->kind == model::InstrumentKind::Midi);  // neither the MIDI slot nor the sample in 55 was touched
  CHECK(rig.bank.pcm(54).frames == 1000u);
  CHECK(rig.bank.pcm(48).frames == 0u);
}

TEST_CASE("sample load: a folder change takes the REPLACE question off the strip, not only the flag (OS-SLDTARGET)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "hat.wav", wav_bytes(100));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.invoke("instrument.select", 1);
  rig.select("a.wav");
  rig.invoke("sample.load", 0);
  NEED(rig.until("PEAKS A.WAV DONE"));
  rig.passes(4);
  rig.invoke("sample.load", 0);
  CHECK(rig.message() == "REPLACE 01 a: F1 AGAIN");
  rig.select("drums");
  rig.invoke("sample.enter");
  rig.settle();
  CHECK(std::string(rig.app->state().wav.folder) == "drums");
  CHECK(rig.message().find("REPLACE") == std::string::npos);
}

// ---- OS-AUDIT-A7 (SLD-MULTI, E-LOAD-14 / E-LOAD-15): mark several files and load them in one go ----

namespace {
void mark_files(Rig& rig, std::initializer_list<const char*> names) {
  for (const char* n : names) {
    rig.select(n);
    rig.invoke("sample.mark", 0);
  }
}
void fill_slots(Rig& rig, std::uint32_t from, std::uint32_t to) {  // instruments 'from' .. 'to - 1' are taken
  model::Instrument taken;
  model::init_instrument(taken, model::InstrumentKind::Sample);
  for (std::uint32_t i = from; i < to; ++i) {
    CHECK(rig.arena->project.set_instrument(i, taken) == model::Status::Ok);
  }
}
}  // namespace

TEST_CASE("sample load: Shift + Right marks the file and steps down, again takes the mark back, a folder is not marked, Shift + Left clears; a new scan and leaving the screen clear them (OS-AUDIT-A7)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(100));
  write_file(rig.dir.root / "b.wav", wav_bytes(100));
  write_file(rig.dir.root / "c.wav", wav_bytes(100));
  write_file(rig.dir.root / "drums" / "hat.wav", wav_bytes(100));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  const app::WavView& w = rig.app->state().wav;
  rig.select("drums");
  rig.invoke("sample.mark", 0);
  CHECK(w.mark_count() == 0u);
  CHECK(rig.message() == "A FOLDER: PRESS ENTER");
  rig.select("a.wav");
  rig.invoke("sample.mark", 0);
  CHECK(w.mark_count() == 1u);
  CHECK(w.marked(1));  // (the folder is row 0)
  CHECK(w.list.pos() == 2);  // stepped down onto b.wav
  rig.invoke("sample.mark", 0);
  rig.invoke("sample.mark", 0);
  CHECK(w.mark_count() == 3u);
  CHECK(w.list.pos() == 3);  // c.wav is the last row: the cursor stays
  rig.select("b.wav");
  rig.invoke("sample.mark", 0);  // the mark back
  CHECK(w.mark_count() == 2u);
  CHECK_FALSE(w.marked(2));
  rig.invoke("sample.mark", 1);
  CHECK(w.mark_count() == 0u);
  CHECK(rig.message() == "MARKS CLEARED");
  rig.invoke("sample.mark", 1);
  CHECK(rig.message() == "NO FILE MARKED");
  // A folder change, and RESCAN, throw the marks away with the listing; so does leaving the screen.
  mark_files(rig, {"a.wav", "b.wav"});
  CHECK(w.mark_count() == 2u);
  rig.invoke("sample.rescan");
  rig.settle();
  CHECK(w.mark_count() == 0u);
  mark_files(rig, {"a.wav"});
  rig.select("drums");
  rig.invoke("sample.enter");
  rig.settle();
  CHECK(w.mark_count() == 0u);
  rig.invoke("sample.up");
  rig.settle();
  mark_files(rig, {"a.wav"});
  CHECK(w.mark_count() == 1u);
  rig.go(ScreenId::Pattern);
  rig.passes(2);
  CHECK(w.mark_count() == 0u);
}

TEST_CASE("sample load: F1 with files marked loads them in list order into consecutive free instruments from the target, never over one that is there (OS-AUDIT-A7)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(1000));
  write_file(rig.dir.root / "b.wav", wav_bytes(1100));
  write_file(rig.dir.root / "c.wav", wav_bytes(1200));
  write_file(rig.dir.root / "d.wav", wav_bytes(1300));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  fill_slots(rig, 1, 2);  // instrument 2 is taken
  mark_files(rig, {"d.wav", "b.wav", "c.wav"});  // (marked out of order: the load goes by the list)
  rig.select("a.wav");  // the cursor is not on a marked file: it does not matter
  rig.invoke("sample.load", 0);
  NEED(rig.until("LOADED 3 OF 3", 3000));
  CHECK(rig.bank.pcm(0).frames == 1100u);  // b
  CHECK(rig.bank.pcm(1).frames == 0u);     // the one that was there stays
  CHECK(rig.bank.pcm(2).frames == 1200u);  // c
  CHECK(rig.bank.pcm(3).frames == 1300u);  // d
  CHECK(rig.app->state().wav.mark_count() == 0u);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.card.open_files() == 0u);
  // The target is where the search starts: instrument 5 and on.
  rig.expire();
  mark_files(rig, {"a.wav", "b.wav"});
  rig.invoke("instrument.select", 5);
  rig.invoke("sample.load", 0);
  NEED(rig.until("LOADED 2 OF 2", 3000));
  CHECK(rig.bank.pcm(4).frames == 1000u);
  CHECK(rig.bank.pcm(5).frames == 1100u);
  // F4 starts at the first free instrument, whatever the target.
  rig.expire();
  fill_slots(rig, 0, 1);
  mark_files(rig, {"c.wav"});
  rig.invoke("sample.load", 1);
  NEED(rig.until("LOADED 1 OF 1", 3000));
  CHECK(rig.bank.pcm(6).frames == 1200u);  // slots 0..5 were taken by now
}

TEST_CASE("sample load: a multi-load that runs out of instruments stops at the last one, says how many came in, and leaves the rest marked; it never wraps (OS-AUDIT-A7, E-LOAD-15)") {
  Rig rig;
  const char* const names[] = {"a.wav", "b.wav", "c.wav", "d.wav", "e.wav"};
  for (unsigned i = 0; i < 5; ++i) {
    write_file(rig.dir.root / names[i], wav_bytes(1000 + 100 * i));
  }
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  constexpr std::uint32_t kLast = model::kMaxInstruments;
  fill_slots(rig, 2, kLast - 3);  // instrument 1 is free (a wrap would find the hole), 2 .. 61 taken, the last three free
  mark_files(rig, {"a.wav", "b.wav", "c.wav", "d.wav", "e.wav"});
  const_cast<app::ViewState&>(rig.app->state()).instrument = static_cast<int>(kLast) - 2;  // the target: the first of the last three (1-based; instrument.select reaches only 1..48)
  rig.invoke("sample.load", 0);
  NEED(rig.until("LOADED 3 OF 5: NO FREE INSTRUMENT", 4000));
  CHECK(rig.bank.pcm(kLast - 3).frames == 1000u);
  CHECK(rig.bank.pcm(kLast - 2).frames == 1100u);
  CHECK(rig.bank.pcm(kLast - 1).frames == 1200u);
  CHECK(rig.arena->project.instrument(0) == nullptr);  // the hole at the start was not used
  const app::WavView& w = rig.app->state().wav;
  CHECK(w.mark_count() == 2u);  // d and e wait
  CHECK(w.marked(3));
  CHECK(w.marked(4));
  CHECK(rig.card.open_files() == 0u);
}

TEST_CASE("sample load: a file that cannot be read is skipped and counted, the others still load, and its slot goes to the next file (OS-AUDIT-A7)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(1000));
  write_file(rig.dir.root / "b.wav", {'n', 'o', 't', ' ', 'a', ' ', 'w', 'a', 'v', 'e', 'f', 'i', 'l', 'e'});
  write_file(rig.dir.root / "c.wav", wav_bytes(1200));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  mark_files(rig, {"a.wav", "b.wav", "c.wav"});
  rig.invoke("sample.load", 0);
  NEED(rig.until("LOADED 2 OF 3: 1 FAILED", 3000));
  CHECK(rig.bank.pcm(0).frames == 1000u);
  CHECK(rig.bank.pcm(1).frames == 1200u);  // the bad file left slot 2 to c
  CHECK(rig.arena->project.instrument(2) == nullptr);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.card.open_files() == 0u);
  CHECK(rig.app->state().wav.mark_count() == 0u);
}

TEST_CASE("sample load: while a multi-load runs, another load is refused (OS-AUDIT-A7)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(1000));
  write_file(rig.dir.root / "b.wav", wav_bytes(1100));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  mark_files(rig, {"a.wav", "b.wav"});
  rig.invoke("sample.load", 0);
  rig.invoke("sample.load", 0);
  // The refusal is queued (the file's PEAKS DONE may be the head by then, so look at the whole queue).
  const core::NotifyQueue& q = rig.app->state().notify;
  bool refused = false;
  for (unsigned i = 0; i < q.size(); ++i) {
    refused = refused || std::string(q.at(i).text.c_str()) == "A MULTI-LOAD IS RUNNING";
  }
  CHECK(refused);
  NEED(rig.until("LOADED 2 OF 2", 3000));
  // The refused request loaded nothing: two files, two instruments, nothing over them.
  CHECK(rig.bank.pcm(0).frames == 1000u);
  CHECK(rig.bank.pcm(1).frames == 1100u);
  CHECK(rig.arena->project.instrument(2) == nullptr);
  CHECK_FALSE(rig.bank.wav_open());
}

TEST_CASE("sample load: while the list is still being read nothing is marked and no marked load starts (OS-AUDIT-A7)") {
  Rig rig;
  for (int i = 0; i < 40; ++i) {
    write_file(rig.dir.root / "big" / ("f" + std::to_string(100 + i) + ".wav"), wav_bytes(100));
  }
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select("big");
  rig.invoke("sample.enter");  // the scan of "big" starts over
  for (unsigned i = 0; i < 2000 && rig.app->model().files->state() != app::DirScan::State::Scanning; ++i) {
    rig.pass();
  }
  NEED(rig.app->model().files->state() == app::DirScan::State::Scanning);  // still being read
  (void)rig.app->invoke(core::action_id("sample.mark"), 0);  // (no pass: the scan stays where it is)
  CHECK(std::string(rig.message()) == "LIST STILL READING");
  CHECK(rig.app->state().wav.mark_count() == 0u);
  // A mark that was there before the scan began to move is not loaded either.
  const_cast<app::ViewState&>(rig.app->state()).wav.set_mark(0, true);
  (void)rig.app->invoke(core::action_id("sample.load"), 0);
  CHECK(rig.app->state().wav.go == app::WavView::Go::None);
  (void)rig.app->invoke(core::action_id("sample.load"), 1);
  CHECK(rig.app->state().wav.go == app::WavView::Go::None);
}

TEST_CASE("sample load: the summary of a multi-load is the last word on the strip: the waveform build of the instrument in view, after the last file, does not take it (OS-AUDIT-A7, unit fqa.11)") {
  Rig rig;
  write_file(rig.dir.root / "a.wav", wav_bytes(1000));
  write_file(rig.dir.root / "b.wav", wav_bytes(1100));
  write_file(rig.dir.root / "c.wav", wav_bytes(1200));
  write_file(rig.dir.root / "d.wav", wav_bytes(1300));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  mark_files(rig, {"a.wav", "b.wav", "c.wav"});
  rig.invoke("sample.load", 0);
  // From the pass that queues the summary on, no PEAKS line of the instrument in view (it is not the last one loaded: "PEAKS A 40%", "PEAKS A DONE") may be queued
  // or shown; the chained line of the last file ("PEAKS C.WAV DONE") is the one before it, and ends first. The summary is then the head, and stays it for its 2 s.
  const core::NotifyQueue& q = rig.app->state().notify;
  const auto queued = [&q](const char* text) {
    for (unsigned k = 0; k < q.size(); ++k) {
      if (std::string(q.at(k).text.c_str()) == text) {
        return true;
      }
    }
    return false;
  };
  const auto stray_peaks = [&q]() {
    for (unsigned k = 0; k < q.size(); ++k) {
      const std::string t = q.at(k).text.c_str();
      if (t.rfind("PEAKS", 0) == 0u && t.find(".WAV") == std::string::npos) {
        return true;
      }
    }
    return false;
  };
  bool posted = false;
  for (unsigned i = 0; i < 3000 && !posted; ++i) {
    rig.pass();
    posted = queued("LOADED 3 OF 3");
  }
  NEED(posted);
  bool shown = false;
  for (unsigned i = 0; i < 400; ++i) {  // (6.4 s: far past the 2 s the line stays, and past the waveform build)
    CHECK_FALSE(stray_peaks());
    shown = shown || rig.message() == "LOADED 3 OF 3";
    rig.pass();
  }
  CHECK(shown);
  CHECK(rig.bank.pcm(2).frames == 1200u);
  // The quiet build ended with the batch: a single load right after it says its own PEAKS line again.
  rig.expire();
  rig.select("d.wav");
  rig.invoke("instrument.select", 4);
  rig.invoke("sample.load", 0);
  CHECK(rig.until("PEAKS D.WAV DONE", 800));
  CHECK(rig.bank.pcm(3).frames == 1300u);
}
