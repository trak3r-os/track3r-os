// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/simulator.h>

#include <o3os/app/native_load.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/midi/cc_in.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/dir_card.h>
#include <o3os/platform/host/fat_fs_storage.h>
#include <o3os/platform/host/host_card.h>
#include <o3os/platform/mk66/display/ft812_regs.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/usb/screenshot_service.h>
#include <o3os/sim/demo_project.h>
#include <o3os/sim/sim_apps.h>
#include <o3os/sim/sim_audio.h>
#include <o3os/sim/sim_project.h>
#include <o3os/ui/plot/peak_cache.h>
#include <o3os/ui/plot/ram_g_map.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>

namespace o3os::sim {

using core::PowerHold;

// A document the simulator owns: the project arena (zeroed, as SDRAM would be at boot), the undo history (the device's 384 KiB in three rings), the editor over them, the
// ParamChange ring to the app and the list of what the fake audio side drained from it.
struct SimDocument {
  model::ModelArena arena;
  alignas(8) std::uint8_t journal_bytes[model::kUndoRegionBytes];
  model::UndoHistory journal;
  std::unique_ptr<model::Editor> editor;
  core::ParamRing ring;
  std::vector<core::ParamChange> heard;
};

// OS-NEWKEEP: File > NEW on the simulator's document, as the device does it without the voices and the card: the native loader's fresh form, at once (nothing runs after it). OPEN and IMPORT are not
// here: the simulator has no card.
class SimProjectSink final : public app::ProjectSink {
 public:
  explicit SimProjectSink(SimDocument& doc) : doc_(doc) {}
  Start start_new() noexcept override { return fresh(false); }
  Start start_new_keeping_instruments() noexcept override { return fresh(true); }
  Start open(const char*, Format) noexcept override { return Start::NoCard; }
  bool busy() const noexcept override { return false; }
  const char* folder() const noexcept override { return ""; }
  bool loaded() const noexcept override { return true; }
  const char* detail() const noexcept override { return ""; }

 private:
  Start fresh(bool keep) noexcept {
    auto native = std::make_unique<app::NativeFolderLoader>();  // (the reader inside is not a stack object)
    native->begin_fresh(doc_.arena, keep);
    while (native->step(4096) == app::NativeFolderLoader::State::Running) {
    }
    doc_.editor->history_reset();
    return Start::Started;
  }
  SimDocument& doc_;
};

// OS-E2E-1b: what `card <dir>` adds. The card is a scratch copy of a fixture; the voices are a pool of their own that nothing renders (the samples of a loaded project are bound to it and to the
// bank, so the Sample screens and a save see them; `audio on` after the card makes the engine render this pool, E2E-2); the sample memory is the unit's 10,816 KiB sample arena less what the unit carves out of it
// for the peak cache, the bank's tables and the audition buffer (app_main.cpp kArenaSamples; its other small boards, a few tens of KB, are not taken off), so a project that does not fit fails here
// as there: the shared loaders count it as "no room".
struct SimCard {
  static constexpr std::uint32_t kPreviewSamples = 4u * 44100u;  // the platform's audition buffer (app_main.cpp kPreviewSamples), given to the bank by `audio on` (OS-QANET C2: Sample load's F5 needs it)
  static constexpr std::uint32_t kPeakSamples = static_cast<std::uint32_t>((ui::plot::PeakCache::elements_required(std::uint32_t{1} << 22, 1) + 3u) & ~std::size_t{3});  // app_main.cpp kPeakFrames / kPeakSamples
  static constexpr std::uint32_t kBankMemSamples = static_cast<std::uint32_t>((sizeof(app::SampleBankMem) + 7u) & ~std::size_t{7}) / 2u;  // (the sim holds the bank's tables apart; the unit carves them out of the arena)
  static constexpr std::uint32_t kSampleCount = platform::mk66::mem::kSampleArenaSize / static_cast<std::uint32_t>(sizeof(std::int16_t)) - kPeakSamples - kBankMemSamples - kPreviewSamples;
  explicit SimCard(model::ModelArena& arena) { bank.init(*voices, arena, *bank_mem, samples.get(), kSampleCount); }  // (a bank that was never init()ed cannot be destroyed: close_wav() reads its memory)
  platform::host::ScratchDir scratch{"simcard"};
  std::unique_ptr<platform::host::HostCard> card;  // a DirCard over `scratch`, or a FatFsStorage over an image (OS-QANET D1: `fat` says which)
  std::unique_ptr<platform::host::SlowCard> slow;  // OS-QANET D2: what the app, the loader, the apps and the screenshot see: `card` with the slow-card model on top (a pass-through while it is off). The scenario's own reads (`assert file`, card_storage()) and the slot's switch use `card`: they are not the unit's calls and cost no time
  platform::host::FatFsStorage* fat = nullptr;     // card, when it is an image (not owned)
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  std::unique_ptr<std::int16_t[]> samples = std::make_unique<std::int16_t[]>(kSampleCount);  // (zero, like the SDRAM at boot; new[] aligns it to 16)
  std::unique_ptr<std::int16_t[]> preview = std::make_unique<std::int16_t[]>(kPreviewSamples);
  app::SampleBank bank;
  std::unique_ptr<SimProject> project;  // (last: it uses all of the above)
};

// OS-SCREENSHOT: the platform's ScreenshotService over the simulator. The "chip" is the raster: CMD_SNAPSHOT2 of rows y..y+rows is those rows of the visible frame in RGB565 (little endian) written into
// a region of the size of the real one, and read back as the service reads the chip. The bus is always free and a snapshot is done at once, so the capture takes the service's own pace: one 2 KiB piece a tick.
struct SimShot {
  using Port = platform::mk66::console::ShotPort;
  struct ChipPort final : Port {
    explicit ChipPort(Simulator& s) : sim(s), region(ui::plot::kRamGShotBytes, 0) {}
    Simulator& sim;
    std::vector<std::uint8_t> region;
    bool ready() noexcept override { return true; }
    bool bus_free() noexcept override { return true; }
    bool start_capture(std::uint32_t format, std::uint32_t addr, std::uint32_t y, std::uint32_t width_param, std::uint32_t rows) noexcept override {
      const std::uint32_t at = addr - ui::plot::kRamGShot;
      if (format != platform::mk66::display::ft812::kFmtRgb565 || width_param != EveRaster::kWidth || addr < ui::plot::kRamGShot || y + rows > static_cast<std::uint32_t>(EveRaster::kHeight) ||
          at + rows * EveRaster::kWidth * 2u > region.size()) {
        return false;
      }
      const std::uint8_t* const rgba = sim.raster().rgba();
      for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(EveRaster::kWidth); ++x) {
          const std::uint8_t* p = rgba + (static_cast<std::size_t>(y + row) * EveRaster::kWidth + x) * 4u;
          const std::uint32_t v = ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3);
          std::uint8_t* o = region.data() + at + (row * EveRaster::kWidth + x) * 2u;
          o[0] = static_cast<std::uint8_t>(v);
          o[1] = static_cast<std::uint8_t>(v >> 8);
        }
      }
      return true;
    }
    Capture poll_capture() noexcept override { return Capture::Done; }
    void abort_capture() noexcept override {}
    std::uint32_t reinits() noexcept override { return 0; }
    void read(std::uint32_t addr, std::uint8_t* out, std::size_t n) noexcept override { std::memcpy(out, region.data() + (addr - ui::plot::kRamGShot), n); }
    std::uint32_t frames() noexcept override { return static_cast<std::uint32_t>(sim.display().frames()); }
    std::uint32_t now_ms() noexcept override { return static_cast<std::uint32_t>(sim.clock_.uptime_ms()); }
  };
  struct Host final : platform::mk66::console::ScreenshotHost {
    explicit Host(Simulator& s) : sim(s) {}
    Simulator& sim;
    hal::storage::shot::Refusal card_refusal() noexcept override;
    bool console_shot_active() noexcept override { return false; }
    void notice(const char* text, bool warn) noexcept override { sim.app_.shot_notice(text, 3000, warn); }
  };
  explicit SimShot(Simulator& s) : port(s), host(s) {}
  ChipPort port;
  Host host;
  platform::mk66::console::ScreenshotService service;
  alignas(4) std::uint8_t buffer[platform::mk66::console::ScreenshotService::kBufferBytes] = {};
};

Simulator::Simulator(std::size_t app_dl_words, std::size_t sink_words)
    : display_(raster_, sink_words), app_(input_, clock_, display_, app_dl_words) {
  assets_loaded_ = load_assets(display_) == hal::UploadResult::Ok;  // the platform's one job for the faces, tiles and icons
  app_.attach_play_position(&play_);  // stopped until a scenario's `playhead`
}

Simulator::~Simulator() = default;

hal::storage::shot::Refusal SimShot::Host::card_refusal() noexcept {
  using R = hal::storage::shot::Refusal;
  if (sim.card_ == nullptr || !sim.card_->card->mounted()) {
    return R::NoCard;
  }
  if (sim.card_->card->gate() != hal::storage::WriteStatus::Ok) {
    return R::Unwritable;
  }
  return sim.card_->project->busy() ? R::Busy : R::None;
}

bool Simulator::screenshot_running() const { return shot_ != nullptr && shot_->service.active(); }

void Simulator::open_document(bool with_bindings, bool with_new, unsigned tracks, bool sound, bool tb303, bool inserts) {
  app_.attach_document(nullptr);
  app_.attach_project_sink(nullptr);
  project_sink_.reset();
  if (audio_ != nullptr) {  // the rig plays the document that goes (the scenario runner refuses `document` after `audio on`; this keeps the API from dangling)
    app_.attach_transport(nullptr);
    app_.attach_preview(nullptr);
    app_.attach_mix(nullptr);
    app_.attach_perform(nullptr);
    app_.attach_play_position(&play_);
    audio_.reset();
  }
  if (apps_ != nullptr) {  // the apps are files of the card that goes
    app_.attach_modules(nullptr);
    apps_.reset();
  }
  if (card_ != nullptr) {  // the card's loader and bank are bound to the document that goes: the card goes with it (the scenario runner refuses `document` after `card`; this keeps the API from dangling)
    app_.attach_saves(nullptr);
    app_.attach_storage(nullptr);
    app_.attach_samples(nullptr);
    saves_.reset();
    shot_.reset();
    card_.reset();
  }
  app_.attach_param_ring(nullptr);
  document_ = std::make_unique<SimDocument>();
  std::memset(&document_->arena, 0, sizeof document_->arena);
  if (inserts) {
    fill_insert_project(document_->arena.project);
  } else if (tb303) {
    fill_tb303_project(document_->arena.project);
  } else if (sound) {
    fill_sound_project(document_->arena.project);
  } else if (with_bindings) {
    fill_demo_project_with_bindings(document_->arena.project);
  } else {
    fill_demo_project(document_->arena.project);
  }
  if (tracks != 0u) {
    (void)document_->arena.project.set_active_tracks(tracks);  // OS-ZOOM: a project of fewer (or more) tracks than the demo's twelve
  }
  (void)document_->journal.begin(document_->journal_bytes, sizeof document_->journal_bytes);
  document_->editor = std::make_unique<model::Editor>(document_->arena.project, document_->journal);
  app_.attach_document(document_->editor.get());
  app_.attach_param_ring(&document_->ring);
  if (with_new) {
    project_sink_ = std::make_unique<SimProjectSink>(*document_);
    app_.attach_project_sink(project_sink_.get());
  }
  step();
}

void Simulator::playhead(bool playing, unsigned step, unsigned ustep, unsigned count_in, unsigned wait) {
  if (wait != 0u && playing) {
    if (!wait_rec_) {
      wait_rec_ = std::make_unique<app::Recorder>();
    }
    wait_rec_->sim_wait(static_cast<audio::RecTap::Align>(wait));
    app_.attach_recorder(wait_rec_.get());
  } else if (wait_rec_) {
    wait_rec_->sim_wait(audio::RecTap::Align::Free);
    app_.attach_recorder(nullptr);
  }
  play_.publish(playing, app_.state().edit.pattern, step, 12000, audio::PlayPosition::kNoOrder, ustep);
  play_.publish_count_in(playing ? count_in : 0u);
  this->step();
}

const model::Project* Simulator::project() const { return document_ ? &document_->arena.project : nullptr; }

bool Simulator::open_card(const std::string& fixture, std::string& error) {
  if (audio_ != nullptr) {
    error = "audio is on already: the engine's voices are bound to the document and the card in place, so `card` comes before `audio on`";
    return false;
  }
  if (card_ != nullptr) {
    error = "a card is in the slot already (a run has one card)";
    return false;
  }
  if (!document_) {
    error = "a document must be open first (a `document` line): a project load replaces it";
    return false;
  }
  auto rig = std::make_unique<SimCard>(document_->arena);
  if (!rig->scratch.ok()) {
    error = "no scratch directory could be made for the card";
    return false;
  }
  std::filesystem::path dir(fixture);
  if (dir.is_relative() && !scenario_dir_.empty()) {
    dir = std::filesystem::path(scenario_dir_) / dir;  // relative to --cards or the scenario file, not to wherever o3sim was started
  }
  // OS-QANET D1: a fixture that is a FILE is a FAT32 image (`card name.img`; `card name` finds name.img when there is no folder of that name): the card is the unit's FatFs over it (fat_fs_storage.h), the
  // image itself is never written (a private mapping). Anything else is a folder, copied into the scratch directory as before.
  std::error_code probe;
  std::filesystem::path image;
  if (std::filesystem::is_regular_file(dir, probe)) {
    image = dir;
  } else if (!std::filesystem::exists(dir, probe) && std::filesystem::is_regular_file(dir.string() + ".img", probe)) {
    image = dir.string() + ".img";
  } else if (!std::filesystem::exists(dir, probe) && !scenario_dir_.empty() && std::filesystem::is_regular_file(scenario_dir_, probe)) {
    image = scenario_dir_;  // `o3sim --cards <image>`: that image is the card of every `card` line of the run
  }
  if (!image.empty()) {
    auto fat = std::make_unique<platform::host::FatFsStorage>();
    if (!fat->open_image(image.string(), error)) {
      return false;
    }
    rig->fat = fat.get();
    rig->card = std::move(fat);
  } else {
    if (!rig->scratch.copy_from(dir.string(), error)) {
      if (!std::filesystem::is_directory(dir) && std::filesystem::path(fixture).is_relative()) {
        error += " (a relative fixture is looked up in '" + scenario_dir_ + "', o3sim --cards names another folder, a fixture may be a FAT32 image file; the journeys' card is made by the build target e2e_card_fixture)";
      }
      return false;
    }
    rig->card = std::make_unique<platform::host::DirCard>(rig->scratch.path());
  }
  if (rig->card->mount() != hal::storage::Status::Ok) {
    error = "the card did not mount";
    return false;
  }
  rig->slow = std::make_unique<platform::host::SlowCard>(*rig->card, card_timing_, static_cast<platform::host::CardClock&>(*this));
  rig->project = std::make_unique<SimProject>(document_->arena, *document_->editor, app_, *rig->slow, *rig->voices, rig->bank, rig->samples.get(), SimCard::kSampleCount);
  card_ = std::move(rig);
  app_.attach_project_sink(nullptr);  // (the sink of `document new` has no card: this one does NEW as well)
  project_sink_.reset();
  if (!saves_) {
    saves_ = std::make_unique<app::SaveService>();
  }
  saves_->attach(*card_->slow, *card_->slow);
  app_.attach_storage(card_->slow.get());
  app_.attach_samples(&card_->bank);
  app_.attach_project_sink(card_->project.get());
  app_.attach_saves(saves_.get());
  step();
  return true;
}

// The platform's slot poll (platform/mk66/src/app_main.cpp poll_card): a card taken out is released at once (every call fails, the strip says so); a card put in is announced, and mounted two frames later.
bool Simulator::card_pull(std::string& error) {
  if (card_ == nullptr || !card_->card->card_present()) {
    error = card_ == nullptr ? "there is no card in the slot" : "the card is pulled already";
    return false;
  }
  card_->card->set_present(false);
  app_.card_changed(app::CardEvent::Removed);
  step();
  return true;
}

bool Simulator::card_insert(std::string& error) {
  if (card_ == nullptr || card_->card->card_present()) {
    error = card_ == nullptr ? "there is no card to put in (`card <fixture-dir>` makes one)" : "the card is in the slot";
    return false;
  }
  card_->card->set_present(true);
  app_.card_changed(app::CardEvent::Reading);
  wait_ms(2 * 16);  // kCardMountDelayMs: the announcement is drawn before the mount
  app_.card_changed(card_->card->mount() == hal::storage::Status::Ok ? app::CardEvent::Ready : app::CardEvent::NotReadable);
  step();
  return true;
}

bool Simulator::firmware_on(std::string& error) {
  if (firmware_on_) {
    error = "the firmware update port is on already";
    return false;
  }
  using Script = platform::host::FakeFirmwareSink::Script;
  using State = hal::firmware::CheckStatus::State;
  firmware_.clear_files();
  firmware_.add("TRACKER_1.PTF", 812345);  // the first .ptf is the reset target
  firmware_.add("SLOW_CHECK.PTF", 799000);
  firmware_.add("BAD_IMAGE.PTF", 640000);
  firmware_.add("NEVER_ENDS.PTF", 805120);
  Script normal;  // the defaults: 3 passes of check, a good image (0x3F2A0 B, CRC 5EA1B0C4); the bootloader does not take over, after 3 passes
  normal.install_end = State::DidNotStart;
  firmware_.script("TRACKER_1.PTF", normal);
  Script slow;  // a check that takes 600 passes (600 ms of simulated time), so its bar can be looked at
  slow.slices = 600;
  slow.image = 0x3E800u;
  slow.crc = 0x0BADC0DEu;
  firmware_.script("SLOW_CHECK.PTF", slow);
  Script bad;  // the check fails after 2 passes
  bad.slices = 2;
  bad.verdict = State::Failed;
  bad.offset = 1104;
  bad.fault = "flash configuration field is not the required one";
  firmware_.script("BAD_IMAGE.PTF", bad);
  Script never;  // install_end stays Idle: the dialog says DO NOT POWER OFF for good, as the unit's screen does until the chip resets
  never.image = 0x3F4A0u;
  never.crc = 0xC0FFEE11u;
  firmware_.script("NEVER_ENDS.PTF", never);
  firmware_on_ = true;
  app_.attach_firmware(&firmware_);
  step();
  return true;
}

bool Simulator::card_copy(const std::string& from, const std::string& to, std::string& error) {
  namespace fs = std::filesystem;
  if (card_ == nullptr || card_->card->card_present()) {
    error = card_ == nullptr ? "there is no card in the slot" : "copy: pull the card first (a computer copies it, not the unit)";
    return false;
  }
  const auto plain = [](const std::string& p) {
    const fs::path path(p);
    if (p.empty() || path.is_absolute()) {
      return false;
    }
    for (const fs::path& part : path) {
      if (part == ".." || part == ".") {
        return false;
      }
    }
    return true;
  };
  if (!plain(from) || !plain(to)) {
    error = "copy: '" + from + "' and '" + to + "' must be relative paths of the card with no '.' or '..'";
    return false;
  }
  if (card_->fat != nullptr) {  // an image: the computer reads the volume, copies, and writes it again (fat_fs_storage.h)
    return card_->fat->computer_copy(from, to, error);
  }
  const fs::path root(card_->scratch.path());
  std::error_code ec;
  if (!fs::is_directory(root / from, ec) || fs::exists(root / to, ec) || !fs::is_directory((root / to).parent_path(), ec)) {
    error = "copy: '" + from + "' is not a folder of the card, or '" + to + "' exists already, or its parent folder is not there";
    return false;
  }
  fs::copy(root / from, root / to, fs::copy_options::recursive, ec);
  if (ec) {
    error = "copy: " + ec.message();
    return false;
  }
  return true;
}

bool Simulator::apps_on(std::string& error) {
  if (apps_ != nullptr) {
    error = "the apps are on already";
    return false;
  }
  if (card_ == nullptr) {
    error = "a card must be in the slot first (`card <fixture>`): the apps are files of its modules/apps folder";
    return false;
  }
  if (audio_ != nullptr) {
    error = "refused after `audio on` (the apps' audio tap is not the engine's)";
    return false;
  }
  if (card_->fat != nullptr) {
    error = "the apps are host shared objects in a folder of the card: a FAT32 image card has none (use a folder fixture)";
    return false;
  }
  apps_ = SimApps::create(*card_->slow, card_->scratch.path(), clock_, error);
  if (apps_ == nullptr) {
    return false;
  }
  app_.attach_modules(&apps_->ui());
  step();
  return true;
}

bool Simulator::audio_on(std::string& error) {
  if (audio_ != nullptr) {
    error = "audio is on already";
    return false;
  }
  if (apps_ != nullptr) {
    error = "refused after `apps on` (the apps' audio tap is not the engine's)";
    return false;
  }
  if (!document_) {
    error = "a document must be open first (a `document` line): the sequencer plays it";
    return false;
  }
  if (project_sink_ != nullptr) {  // SimProjectSink::fresh() rewrites the arena the engine plays, with no Stop and no Settle: a NEW with sound is the card's (SimProject::attach_sound)
    error = "refused after `document new` (its File > NEW rewrites the project under the engine, with no Stop and no wait for the voices): leave out `new`, or open a `card` (its NEW does both)";
    return false;
  }
  audio_ = std::make_unique<SimAudio>(document_->arena.project, card_ != nullptr ? card_->voices.get() : nullptr);
  app_.attach_param_ring(&audio_->params());  // the app's ParamChanges go to the engine, which drains them in every block after its voices (a note in the block of an edit starts on the old value, for one block at most; the document's fake consumer has nothing more to hear)
  app_.attach_play_position(&audio_->play_position());
  app_.attach_scratch(&audio_->engine().graph().scratch());  // OS-DJ C1: F5 and TAPE STOP take the ring from the card's bank; the sim has no encoder counter (still)
  app_.attach_transport(&audio_->transport());
  app_.attach_preview(&audio_->preview());
  app_.attach_mix(&audio_->mix());
  app_.attach_perform(&audio_->perform());
  if (card_ != nullptr) {
    card_->bank.set_preview_buffer(card_->preview.get(), SimCard::kPreviewSamples);  // as make_sound() does on the unit: without it F5 says NO PREVIEW MEMORY (only with the engine is there anything to hear it)
    card_->project->attach_sound(&audio_->transport(), &audio_->preview());  // a load stops the transport and lets the voices die away first
  }
  if (saves_ != nullptr) {
    saves_->attach_export(audio_->export_wiring());  // File > EXPORT: the offline render behind the WAV export (the platform's wire_export)
  }
  step();
  return true;
}

// A WAV file of the card as a finished render (scenario `wav`). The format is what the export writes: RIFF, a 16-bit PCM "fmt " of two channels, then "data".
bool Simulator::load_wav(const std::string& name, const std::string& path, std::string& error) {
  if (audio_ == nullptr) {
    error = "audio is off (`audio on` first)";
    return false;
  }
  hal::storage::Storage* card = card_storage();
  if (card == nullptr || !card->mounted()) {
    error = "there is no mounted card";
    return false;
  }
  hal::storage::FileHandle file;
  hal::storage::Status s = card->open(path.c_str(), file);
  if (s != hal::storage::Status::Ok) {
    error = path + " cannot be opened (" + hal::storage::status_name(s) + ")";
    return false;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buf[4096];
  for (;;) {
    std::size_t got = 0;
    s = card->read(file, buf, sizeof buf, got);
    if (s != hal::storage::Status::Ok) {
      card->close(file);
      error = path + ": " + hal::storage::status_name(s);
      return false;
    }
    if (got == 0) {
      break;
    }
    bytes.insert(bytes.end(), buf, buf + got);
  }
  card->close(file);
  const auto u16 = [&](std::size_t at) { return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8); };
  const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
  if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    error = path + " is not a RIFF WAVE file";
    return false;
  }
  Pcm pcm;
  bool have_fmt = false;
  for (std::size_t at = 12; at + 8 <= bytes.size();) {
    const std::uint32_t len = u32(at + 4);
    const std::size_t body = at + 8;
    if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && len >= 16 && body + 16 <= bytes.size()) {
      if (u16(body) != 1u || u16(body + 2) != 2u || u16(body + 14) != 16u) {
        error = path + ": only 16-bit PCM of two channels is read";
        return false;
      }
      pcm.rate = static_cast<double>(u32(body + 4));
      have_fmt = true;
    } else if (std::memcmp(bytes.data() + at, "data", 4) == 0 && have_fmt) {
      const std::size_t n = std::min<std::size_t>(len, bytes.size() - body) / 4u * 2u;
      pcm.lr.resize(n);
      for (std::size_t i = 0; i < n; ++i) {
        pcm.lr[i] = static_cast<std::int16_t>(u16(body + 2u * i));
      }
      return audio_->add_render(name, std::move(pcm), error);
    }
    at = body + len + (len & 1u);
  }
  error = path + " has no data after its format";
  return false;
}

bool Simulator::render_begin(const std::string& name, std::uint32_t ms, bool stems, std::string& error) {
  if (audio_ == nullptr) {
    error = "audio is off (`audio on` first)";
    return false;
  }
  return audio_->begin_render(name, ms, stems, error);
}

const Pcm* Simulator::render_result(const std::string& name) const { return audio_ != nullptr ? audio_->render(name) : nullptr; }

std::string Simulator::render_running(std::uint32_t* ticks_left) const {
  if (ticks_left != nullptr) {
    *ticks_left = 0;
  }
  return audio_ != nullptr ? audio_->open_render(ticks_left) : std::string();
}

std::vector<std::pair<std::string, std::string>> Simulator::render_digests() const {
  std::vector<std::pair<std::string, std::string>> out;
  if (audio_ != nullptr) {
    for (const SimAudio::Finished& f : audio_->finished()) {
      out.emplace_back(f.name, pcm_sha256(*f.pcm));
    }
  }
  return out;
}

hal::storage::Storage* Simulator::card_storage() const noexcept { return card_ != nullptr ? card_->card.get() : nullptr; }

const std::vector<core::ParamChange>& Simulator::heard_params() const {
  static const std::vector<core::ParamChange> kNone;
  return document_ ? document_->heard : kNone;
}

void Simulator::mark_unsaved() {
  if (!document_) {
    return;
  }
  if (!saves_) {
    saves_ = std::make_unique<app::SaveService>();
    app_.attach_saves(saves_.get());
  }
  (void)document_->arena.project.set_tempo_x100(document_->arena.project.tempo_x100() + 100u);  // any accepted mutation: the project is no longer the one the save layer took as its baseline
  step();
}

void Simulator::power_switch(bool down) {
  power_down_ = down;
  step();
}

// The platform's per-pass work for the Power switch (platform/mk66/src/app_main.cpp power_pass), without the stages: the hold, the app's line, and at the commit the app's shutdown mode.
void Simulator::power_pass() {
  const PowerHold::Result hr = power_hold_.update(static_cast<std::uint32_t>(clock_.uptime_ms()), !power_down_, shift_down_, true);
  if (hr.commit && !power_committed_) {
    power_committed_ = true;
    app_.power_begin();
    if (shot_ != nullptr) {
      shot_->service.cancel(shot_->port);  // OS-SCREENSHOT: as the platform's power_quiesce: after the commit nothing is taken, a screenshot in flight is dropped
    }
  }
  if (hr.screenshot && !power_committed_) {
    if (card_ == nullptr) {
      app_.shot_notice(hal::storage::shot::refusal_text(hal::storage::shot::Refusal::NoCard), 3000, true);  // (no card object: the platform's service would be told the same)
    } else {
      if (shot_ == nullptr) {
        shot_ = std::make_unique<SimShot>(*this);
        shot_->service.attach(shot_->host, *card_->slow, *card_->slow, shot_->buffer, ui::plot::kRamGShot, ui::plot::kRamGShotBytes);
      }
      shot_->service.request(static_cast<std::uint32_t>(clock_.uptime_ms()));
    }
  }
  app_.set_power(power_committed_ ? core::PowerShow{1000, core::PowerText::PoweringOff} : hr.show);
}

bool Simulator::set_card_timing(bool on, std::uint32_t read_bytes_per_ms, std::uint32_t latency_us, std::uint32_t write_bytes_per_ms, std::string& error) {
  if (on && (read_bytes_per_ms == 0u || write_bytes_per_ms == 0u)) {
    error = "a card rate of 0 bytes a millisecond never finishes a read";
    return false;
  }
  card_timing_.on = on;
  if (on) {
    card_timing_.read_bytes_per_ms = read_bytes_per_ms;
    card_timing_.latency_us = latency_us;
    card_timing_.write_bytes_per_ms = write_bytes_per_ms;
  }
  return true;
}

std::uint64_t Simulator::card_charged_us() const { return card_ != nullptr ? card_->slow->charged_us() : 0u; }

// A card call of the app took `us` of the unit's time: the pass stretches, so the clock moves on inside it. The engine is an interrupt on the unit and goes on while the main loop waits, so every whole
// millisecond that passes renders its block here and now (not after the pass: a sound that the pass starts is heard from the moment the call that follows it returns). The clock and the audio stay one block
// a millisecond apart, which is what lets a test read a latency off the clock.
void Simulator::card_time(std::uint32_t us) noexcept {
  clock_.advance(us);
  if (audio_ == nullptr || card_catching_up_) {
    return;
  }
  card_extra_us_ += us;
  card_catching_up_ = true;
  while (card_extra_us_ >= kTickUs) {
    card_extra_us_ -= kTickUs;
    audio_->tick();
  }
  card_catching_up_ = false;
}

void Simulator::step() {
  clock_.advance(kTickUs);
  power_pass();
  if (card_ != nullptr && card_->project->busy()) {
    card_->project->pump();  // a project load: a slice of it, and no app frame until it has ended (the platform's pass holds the frames the same way)
  }
  const bool shot_holds = shot_ != nullptr && shot_->service.step(static_cast<std::uint32_t>(clock_.uptime_ms()), shift_down_ || power_down_, shot_->port);  // OS-SCREENSHOT: the frames wait
  if ((card_ == nullptr || !card_->project->busy()) && !shot_holds) {
    app_.run_once();
  }
  if (document_) {  // the fake audio side: it drains the ring at every tick (the real engine, 64 at most every block)
    core::ParamChange c;
    while (document_->ring.pop(c)) {
      document_->heard.push_back(c);
    }
  }
  if (audio_ != nullptr) {  // the real one: after the app's pass, so that what the app did this tick (a pad, Play) is in this tick's block
    audio_->tick();
  }
}

void Simulator::wait_ms(std::uint32_t ms) {
  for (std::uint32_t i = 0; i < ms; ++i) {
    step();
  }
}

void Simulator::press(app::Button button) {
  shift_down_ = shift_down_ || button == app::Button::Shift;
  input_.push(core::make_button_event(clock_.now(), static_cast<std::uint8_t>(button), true));
  step();
}

void Simulator::release(app::Button button) {
  shift_down_ = shift_down_ && button != app::Button::Shift;
  input_.push(core::make_button_event(clock_.now(), static_cast<std::uint8_t>(button), false));
  step();
}

void Simulator::tap(app::Button button) {
  press(button);
  release(button);
}

void Simulator::pad(unsigned index) {
  input_.push(core::make_pad_event(clock_.now(), static_cast<std::uint8_t>(index), 127));
  step();
  input_.push(core::make_pad_event(clock_.now(), static_cast<std::uint8_t>(index), 0));
  step();
}

void Simulator::pad_down(unsigned index) {
  input_.push(core::make_pad_event(clock_.now(), static_cast<std::uint8_t>(index), 127));
  step();
}

void Simulator::pad_up(unsigned index) {
  input_.push(core::make_pad_event(clock_.now(), static_cast<std::uint8_t>(index), 0));
  step();
}

void Simulator::pad_hold(unsigned index, unsigned ms) {
  input_.push(core::make_pad_event(clock_.now(), static_cast<std::uint8_t>(index), 127));
  wait_ms(ms == 0u ? 1u : ms);
  input_.push(core::make_pad_event(clock_.now(), static_cast<std::uint8_t>(index), 0));
  step();
}

void Simulator::midi_note(unsigned channel, unsigned note, unsigned velocity, bool on) {
  if (midi_in_ == nullptr) {
    midi_in_ = std::make_unique<midi::CcInput>();
  }
  const std::uint8_t bytes[3] = {static_cast<std::uint8_t>((on ? 0x90u : 0x80u) | ((channel - 1u) & 15u)), static_cast<std::uint8_t>(note & 127u), static_cast<std::uint8_t>(velocity & 127u)};
  for (const std::uint8_t b : bytes) {
    midi_in_->feed(midi::RtItem{0u, b, 0u, 0u});
  }
  (void)app_.poll_midi_notes(*midi_in_);
  step();
}

void Simulator::encoder(int delta) {
  input_.push(core::make_encoder_event(clock_.now(), 0, static_cast<std::int16_t>(delta)));
  step();
}

void Simulator::set_theme(ui::ThemeId id) {
  app_.set_theme(id);
  step();
}

app::DispatchResult Simulator::action(core::ActionId id, core::ActionArg arg) {
  const app::DispatchResult result = app_.invoke(id, arg);
  step();
  return result;
}

bool Simulator::healthy() const {
  return assets_loaded_ && display_.render_failures() == 0 && app_.failed_frames() == 0;
}

std::string Simulator::failure() const {
  if (!assets_loaded_) {
    return "the faces, tiles and icons could not be uploaded to RAM_G (the display refused an upload)";
  }
  if (display_.render_failures() != 0) {
    return "rasteriser: " + display_.first_error();
  }
  if (app_.failed_frames() != 0) {
    return std::to_string(app_.failed_frames()) +
           " frame(s) did not fit the display list and were dropped (the previous picture stays)";
  }
  return std::string();
}

}  // namespace o3os::sim
