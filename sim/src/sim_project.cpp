// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/sim_project.h>

#include <o3os/app/dir_scan.h>

namespace o3os::sim {

SimProject::SimProject(model::ModelArena& arena, model::Editor& editor, app::App& app, hal::storage::Storage& card, audio::VoicePool& voices, app::SampleBank& bank,
                       std::int16_t* samples, std::uint32_t sample_count)
    : arena_(arena),
      editor_(editor),
      app_(app),
      card_(card),
      voices_(voices),
      bank_(bank),
      samples_mem_(samples),
      sample_count_(sample_count),
      og_(std::make_unique<app::OgFolderLoader>()),
      native_loader_(std::make_unique<app::NativeFolderLoader>()),
      verify_(std::make_unique<app::ProjectVerify>()),
      sample_loader_(std::make_unique<app::OgSampleLoader>()),
      module_(std::make_unique<app::ModuleImport>()) {}

SimProject::~SimProject() = default;

// platform/mk66/src/app_main.cpp start_project_load: the folder is verified first (the Verify phase, pump()), then begin_open() starts its loader and begin_replace().
SimProject::Start SimProject::open(const char* folder, Format format) noexcept {
  if (phase_ != Phase::Idle) {
    return Start::Busy;
  }
  if (!card_.mounted()) {
    return Start::NoCard;
  }
  if (app::folder_marked_deleted(card_, folder)) {
    return Start::BadPath;  // a project DELETE hid is not loaded (and so never saved into)
  }
  if (!verify_->begin(card_, folder, format == Format::Og, format == Format::NativeBackup)) {
    return Start::BadPath;
  }
  refused_ = false;
  phase_ = Phase::Verify;
  return Start::Started;
}

// The verified folder's loader begins (it resets nothing yet: its first step does, after the Settle).
SimProject::Start SimProject::begin_open() noexcept {
  if (!verify_->og()) {
    if (!native_loader_->begin(card_, verify_->folder(), arena_, verify_->backup())) {
      return Start::BadPath;
    }
    module_->release();
    native_ = true;
  } else {
    if (!og_->begin(card_, verify_->folder(), arena_)) {
      return Start::BadPath;
    }
    native_loader_->release();  // from here the OG loader's report is the load's
    module_->release();
    native_ = false;
  }
  begin_replace();
  return Start::Started;
}

// platform/mk66/src/app_main.cpp start_new_project: the native loader's fresh form is the Project phase (it resets the document); the phases after it empty the sample memory or leave it.
SimProject::Start SimProject::begin_fresh(bool keep_instruments) noexcept {
  if (phase_ != Phase::Idle) {
    return Start::Busy;
  }
  refused_ = false;
  native_loader_->begin_fresh(arena_, keep_instruments);
  module_->release();
  native_ = true;
  begin_replace();
  return Start::Started;
}

// platform/mk66/src/app_main.cpp start_module_import: the file is opened now (a path that is not a module, or a file that will not open, leaves the document as it was), the importer's first act (it resets
// the project) is its first step() after the Settle; from here the module's report is the load's.
SimProject::Start SimProject::import_module(const char* file, app::ImportEngine engine) noexcept {
  if (phase_ != Phase::Idle) {
    return Start::Busy;
  }
  if (!card_.mounted()) {
    return Start::NoCard;
  }
  refused_ = false;
  if (module_->begin(card_, file, arena_, engine) != app::ModuleImport::Start::Started) {
    return Start::BadPath;
  }
  native_loader_->release();
  begin_replace();
  return Start::Started;
}

bool SimProject::module_file(const char* file) noexcept {
  return !card_.mounted() || app::ModuleImport::probe(card_, file) != app::ModuleImport::Probe::NotModule;
}

// platform/mk66/src/app_main.cpp begin_replace: the transport stops (a Stop on the sequencer's ring) and the badge says so, the live notes are let go, and the voices are let to die away (Settle) before the
// first slice. Without the engine (`audio on` not run) there is the badge only and nothing to wait for.
void SimProject::begin_replace() noexcept {
  if (transport_ != nullptr) {
    (void)transport_->stop();
  }
  if (preview_ != nullptr) {
    preview_->notes_off(false);
  }
  app_.transport_stopped();
  settle_ms_ = 0;
  phase_ = transport_ != nullptr ? Phase::Settle : Phase::Project;
}

const char* SimProject::folder() const noexcept {
  return module_->active() ? "" : (native_ ? native_loader_->folder() : og_->folder());  // (a module is in no project folder)
}

bool SimProject::loaded() const noexcept {
  if (module_->active()) {
    return module_->state() == app::ModuleImport::State::Done;
  }
  return native_ ? native_loader_->state() == app::NativeFolderLoader::State::Done : og_->state() == app::OgFolderLoader::State::Done;
}

const char* SimProject::detail() const noexcept {
  if (refused_) {
    return refused_why_;
  }
  if (module_->active()) {
    return module_->summary().error;
  }
  return native_ ? native_loader_->report().detail : og_->report().detail;
}

void SimProject::pump() noexcept {
  if (phase_ == Phase::Verify) {  // nothing is replaced until the file has passed: a refusal ends the open here, the document untouched (finish() is not run: no history reset, no new baseline)
    const app::ProjectVerify::State state = verify_->step(kBytesPerPass);
    if (state == app::ProjectVerify::State::Running) {
      return;
    }
    if (state == app::ProjectVerify::State::Ok && begin_open() == Start::Started) {
      return;
    }
    refused_ = true;
    refused_why_ = state == app::ProjectVerify::State::Refused ? verify_->detail() : "NOT A PROJECT FOLDER PATH";
    phase_ = Phase::Idle;
    return;
  }
  if (phase_ == Phase::Settle) {  // app_main.cpp advance_load: kSettleMinMs (the Stop is taken at a block start, 1.45 ms: two blocks and a margin), kSettleMaxMs (a generous bound)
    ++settle_ms_;
    if ((settle_ms_ >= 4u && voices_.voices_in_use() == 0u) || settle_ms_ >= 250u) {
      phase_ = Phase::Project;
    }
    return;
  }
  if (phase_ == Phase::Project) {
    bool running = false;
    bool done = false;
    if (module_->active()) {
      const app::ModuleImport::State state = module_->step(kBytesPerPass);
      running = state == app::ModuleImport::State::Running;
      done = state == app::ModuleImport::State::Done;
    } else if (native_) {
      const app::NativeFolderLoader::State state = native_loader_->step(kBytesPerPass);
      running = state == app::NativeFolderLoader::State::Running;
      done = state == app::NativeFolderLoader::State::Done;
    } else {
      const app::OgFolderLoader::State state = og_->step(kBytesPerPass);
      running = state == app::OgFolderLoader::State::Running;
      done = state == app::OgFolderLoader::State::Done;
    }
    if (running) {
      return;
    }
    if (done) {
      begin_samples();
    } else {
      if (module_->active() && module_->document_touched()) {  // a failed import emptied the document (its reader resets it first): the PCM bound to the old one belongs to nothing
        for (unsigned i = 0; i < audio::kSampleSlots; ++i) {
          voices_.set_sample(i, audio::SampleDesc{});
        }
        bank_.clear();
      }
      finish();  // the project did not load: the document stays as the loader left it (a file that read badly leaves it reset)
    }
    return;
  }
  if (phase_ == Phase::Samples) {
    (void)sample_loader_->step(kBytesPerPass);
    if (sample_loader_->state() != app::OgSampleLoader::State::Running) {
      if (module_->active()) {
        module_->samples_done(sample_loader_->report());  // the counts into the report; the file is let go
      }
      finish();
    }
  }
}

// platform/mk66/src/app_main.cpp begin_samples: the voices' table emptied (but for a new project that keeps its instruments), the law of the document, then the PCM from the folder.
void SimProject::begin_samples() noexcept {
  const bool keep_audio = !module_->active() && native_ && native_loader_->keeps_instruments();
  if (!keep_audio) {
    for (unsigned i = 0; i < audio::kSampleSlots; ++i) {
      voices_.set_sample(i, audio::SampleDesc{});
    }
  }
  if (module_->active()) {  // the module's samples are decoded on request into the arena (no folder: nothing is marked as on the card)
    bank_.set_law(audio::LevelLaw::Og12);
    if (sample_loader_->begin_feed(*module_, arena_.project, voices_, samples_mem_, sample_count_, audio::LevelLaw::Og12, &bank_)) {
      phase_ = Phase::Samples;
    } else {
      module_->close();  // (the file is released here: samples_done() will not run)
      finish();
    }
    return;
  }
  if (!native_) {
    (void)app::commit_og_law_if_done(arena_.project, *og_);  // the load has succeeded: the document's law is the file's version
  }
  const audio::LevelLaw law = app::project_law(arena_.project);
  bank_.set_law(law);
  if (native_ && native_loader_->fresh()) {  // a new project has no samples to load; ALL empties the sample memory, KEEP INSTRUMENTS leaves it
    bank_.new_project(keep_audio);
    finish();
    return;
  }
  if (sample_loader_->begin(card_, native_ ? native_loader_->folder() : og_->folder(), arena_.project, native_ ? std::uint64_t{0} : og_->pti_slots(), voices_, samples_mem_,
                            sample_count_, law, &bank_, native_)) {
    phase_ = Phase::Samples;
  } else {
    finish();  // (the folder is not a path: the instruments stay silent)
  }
}

// platform/mk66/src/app_main.cpp finish_load: the undo history no longer matches the document; the app is attached to it again, takes it as the project that is on the card (or fresh), and shows the
// first pattern of a project that loaded (the saved actual-pattern field of an OG project file).
void SimProject::finish() noexcept {
  const bool ok = loaded();
  const bool imported = module_->active();
  editor_.history_reset();
  app_.attach_document(&editor_);
  app_.document_replaced(imported && ok);  // an imported song is on no card: dirty
  if (ok) {
    const model::Project& project = arena_.project;
    (void)app_.open_pattern(imported || native_ ? (project.song_length() != 0u ? project.order(0) : std::uint8_t{1}) : og_->report().start_pattern);
  }
  phase_ = Phase::Idle;
}

}  // namespace o3os::sim
