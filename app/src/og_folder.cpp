// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/og_folder.h>

#include "text_line.h"

#include <cstring>

namespace o3os::app {
namespace {

using formats::FormatError;
using formats::Progress;
namespace og = formats::og;
using hal::storage::Status;

constexpr std::size_t kMinRoom = 512;  // a slice always moves something, whatever budget it was given

}  // namespace

core::IoResult StorageSource::read(void* dst, std::size_t max) noexcept {
  if (storage_ == nullptr || max == 0u) {
    return {0, core::IoStatus::Failed};
  }
  std::size_t got = 0;
  const Status s = storage_->read(file_, dst, max, got);
  if (s != Status::Ok || got > max) {
    error_ = s != Status::Ok ? s : Status::Io;
    return {0, core::IoStatus::Failed};
  }
  if (got == 0u) {
    return {0, core::IoStatus::End};
  }
  total_ += static_cast<std::uint32_t>(got);
  return {got, core::IoStatus::Ok};
}

// ---- Paths --------------------------------------------------------------------------------------------------------------------------------
// path_ = base_ [/ a [/ b]]; false when it does not fit (nothing is opened then).
bool OgFolderLoader::join(const char* a, const char* b) noexcept {
  std::size_t n = 0;
  const char* parts[3] = {base_, a, b};
  for (const char* part : parts) {
    if (part == nullptr || part[0] == '\0') {
      continue;
    }
    const std::size_t len = std::strlen(part);
    const std::size_t need = len + (n != 0u ? 1u : 0u);
    if (n + need + 1u > sizeof path_) {
      return false;
    }
    if (n != 0u) {
      path_[n++] = '/';
    }
    std::memcpy(path_ + n, part, len);
    n += len;
  }
  path_[n] = '\0';
  return n != 0u;
}

// ---- Control ------------------------------------------------------------------------------------------------------------------------------
void OgFolderLoader::close_all() noexcept {
  // Only what is open is closed, so a loader that is started again after its storage went away (a test's, a card's owner replaced) never calls it.
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  if (storage_ != nullptr && dir_.is_open()) {
    storage_->closedir(dir_);
  }
  file_ = hal::storage::FileHandle{};
  dir_ = hal::storage::DirHandle{};
}

bool OgFolderLoader::begin(hal::storage::Storage& storage, const char* folder, model::ModelArena& arena) noexcept {
  close_all();  // anything running is abandoned
  if (!hal::storage::valid_path(folder)) {
    return false;
  }
  while (folder[0] == '/') {
    ++folder;  // a leading '/' means the same as none
  }
  const std::size_t len = std::strlen(folder);
  std::size_t end = len;
  while (end != 0u && folder[end - 1] == '/') {
    --end;  // a trailing '/' means the same as none
  }
  if (end == 0u || end > kFolderMax) {
    return false;
  }
  std::memcpy(base_, folder, end);
  base_[end] = '\0';
  storage_ = &storage;
  arena_ = &arena;
  report_ = OgLoadReport{};
  source_.reset();
  pti_slots_ = 0;
  entries_ = 0;
  reader_ = nullptr;
  stage_ = Stage::ProjectOpen;
  state_ = State::Running;
  return true;
}

void OgFolderLoader::abort() noexcept {
  close_all();
  if (state_ == State::Running) {
    state_ = State::Idle;
  }
}

void OgFolderLoader::fail(FormatError e, const char* detail) noexcept {
  close_all();
  report_.error = e;
  report_.detail = detail;
  report_.bytes_read = source_.total();
  state_ = State::Failed;
}

void OgFolderLoader::fail_storage(Status s) noexcept {
  report_.storage_error = s;
  fail(FormatError::SourceFailed, hal::storage::status_name(s));
}

// The first file that did not read, kept for the report; the load goes on.
void OgFolderLoader::note_problem(FormatError e, const char* detail) noexcept {
  if (report_.error == FormatError::None) {
    report_.error = e;
    report_.detail = detail;
  }
}

// ---- The slices ---------------------------------------------------------------------------------------------------------------------------
OgFolderLoader::State OgFolderLoader::step(std::size_t byte_budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  const std::size_t budget = byte_budget < kMinRoom ? kMinRoom : byte_budget;
  const std::uint32_t start = source_.total();
  for (unsigned actions = 0; state_ == State::Running && actions < kMaxActionsPerStep; ++actions) {
    const std::size_t used = static_cast<std::uint32_t>(source_.total() - start);
    if (used >= budget) {
      break;
    }
    advance(budget - used);
  }
  report_.bytes_read = source_.total();
  return state_;
}

// Opens `sub` of the folder for listing. Found: the listing starts (stage `when_found`). Missing: straight to `when_missing` (a project
// without a patterns/ or instruments/ folder is an empty one, not an error). Any other answer is the card failing.
void OgFolderLoader::open_directory(const char* sub, Stage when_found, Stage when_missing) noexcept {
  if (!join(sub, nullptr)) {
    stage_ = when_missing;
    return;
  }
  const Status s = storage_->opendir(path_, dir_);
  if (s == Status::Ok) {
    entries_ = 0;
    stage_ = when_found;
  } else if (s == Status::NotFound || s == Status::NotADirectory) {
    stage_ = when_missing;
  } else {
    fail_storage(s);
  }
}

// Opens one file of the folder and starts the matching reader on it. False: the file could not be started (counted by the caller); a card failure
// has ended the load (state_ is Failed).
bool OgFolderLoader::start_file(Kind kind, const char* sub, const char* name, std::uint16_t index, std::uint32_t size) noexcept {
  if (!join(sub, name)) {
    ++report_.files_ignored;  // a name that does not fit a path no card can hold
    return false;
  }
  const Status s = storage_->open(path_, file_);
  if (s != Status::Ok) {
    if (s == Status::Io || s == Status::NotMounted || s == Status::NoCard) {
      fail_storage(s);
    }
    note_problem(FormatError::SourceFailed, hal::storage::status_name(s));
    return false;
  }
  source_.attach(*storage_, file_);
  source_.clear_error();
  formats::ReadParams params;
  params.index = index;
  params.size_hint = size;  // a .pti reader skips its PCM when it knows where the file ends
  kind_ = kind;
  index_ = index;
  switch (kind) {
    case Kind::Pattern: reader_ = &pattern_; break;
    case Kind::InstrumentMti: reader_ = &mti_; break;
    case Kind::InstrumentPti: reader_ = &pti_; break;
  }
  reader_->begin(source_, *arena_, params);
  return true;
}

void OgFolderLoader::finish_file(Progress progress) noexcept {
  const formats::FormatReport& rep = reader_->report();
  storage_->close(file_);
  const bool pattern = kind_ == Kind::Pattern;
  if (progress == Progress::Failed && source_.error() != Status::Ok) {
    reader_ = nullptr;
    fail_storage(source_.error());  // the card failed under the reader (a card pulled out, a sector it would not give): the load ends, it is not a bad file
    return;
  }
  if (progress == Progress::Done) {
    if (pattern) {
      report_.patterns += rep.patterns;
      const og::ImportReport& ir = pattern_.import_report();  // reader_ is pattern_ here
      report_.micromoves_dropped += ir.micromoves_dropped;
      report_.values_clamped += ir.values_clamped;
      report_.ce_fx_cleared += ir.ce_fx_cleared;
    } else {
      report_.instruments += rep.instruments;
      report_.pcm_frames += rep.pcm_frames;
      if (kind_ == Kind::InstrumentPti && rep.instruments != 0u) {
        pti_slots_ |= std::uint64_t{1} << (index_ & 63u);
      }
    }
  } else {
    if (pattern) {
      ++report_.patterns_failed;
    } else {
      ++report_.instruments_failed;
    }
    note_problem(rep.error, rep.detail);
  }
  reader_ = nullptr;
  stage_ = pattern ? Stage::PatternNext : Stage::InstrumentNext;
}

// The next entry of the directory being listed: a file of the wanted kind is started, anything else is counted and skipped.
void OgFolderLoader::next_entry(bool patterns, Stage after_listing) noexcept {
  const Status s = storage_->readdir(dir_, entry_);
  const bool cut = s == Status::Ok && ++entries_ > kMaxEntriesPerDirectory;
  if (s == Status::End || cut) {
    storage_->closedir(dir_);
    stage_ = after_listing;
    return;
  }
  if (s != Status::Ok) {
    fail_storage(s);
    return;
  }
  const og::FileName fn = og::classify_file_name(entry_.name);
  const char* sub = patterns ? "patterns" : "instruments";
  if (entry_.is_dir) {
    ++report_.files_ignored;
    return;
  }
  if (patterns) {
    if (fn.file_class != og::FileClass::Pattern) {
      ++report_.files_ignored;  // the pattern names file (not read in this slice), "._" files, anything else
      return;
    }
    if (start_file(Kind::Pattern, sub, entry_.name, fn.index, entry_.size)) {
      stage_ = Stage::FileRead;
    } else if (state_ == State::Running) {
      ++report_.patterns_failed;
    }
    return;
  }
  if (fn.file_class != og::FileClass::Mti && fn.file_class != og::FileClass::Pti) {
    ++report_.files_ignored;
    return;
  }
  const bool pti = fn.file_class == og::FileClass::Pti;
  if (!pti && (pti_slots_ >> (fn.index & 63u) & 1u) != 0u) {
    ++report_.instruments_superseded;  // the .pti of that slot is already in
    return;
  }
  if (start_file(pti ? Kind::InstrumentPti : Kind::InstrumentMti, sub, entry_.name, fn.index, entry_.size)) {
    stage_ = Stage::FileRead;
  } else if (state_ == State::Running) {
    ++report_.instruments_failed;
  }
}

void OgFolderLoader::advance(std::size_t room) noexcept {
  switch (stage_) {
    case Stage::ProjectOpen: {
      if (!join("project.mt", nullptr)) {
        fail(FormatError::Malformed, "PATH TOO LONG");
        return;
      }
      const Status s = storage_->open(path_, file_);
      if (s != Status::Ok) {
        report_.storage_error = s;
        fail(FormatError::SourceFailed, s == Status::NotFound ? "NO PROJECT FILE" : hal::storage::status_name(s));
        return;
      }
      std::uint32_t size = 0;
      (void)storage_->size(file_, size);
      source_.attach(*storage_, file_);
      formats::ReadParams params;
      params.size_hint = size;
      reader_ = &project_;
      project_.begin(source_, *arena_, params);
      stage_ = Stage::ProjectRead;
      return;
    }
    case Stage::ProjectRead: {
      const Progress p = project_.step(room);
      if (p == Progress::Running) {
        return;
      }
      const og::ImportReport& rep = project_.import_report();
      storage_->close(file_);
      reader_ = nullptr;
      if (p == Progress::Failed) {
        if (source_.error() != Status::Ok) {
          fail_storage(source_.error());  // the card failed under the reader: that is the card's error, not the file's
        } else {
          fail(rep.error, rep.detail);
        }
        return;
      }
      report_.project_ok = true;
      report_.version = rep.version;
      report_.start_pattern = rep.start_pattern;
      report_.unverified_version = rep.unverified_version;
      report_.observed_only = rep.observed_only;
      report_.dropped_groups = rep.dropped_groups;
      stage_ = Stage::PatternsOpen;
      return;
    }
    case Stage::PatternsOpen:
      open_directory("patterns", Stage::PatternNext, Stage::InstrumentsOpen);
      return;
    case Stage::PatternNext:
      next_entry(true, Stage::InstrumentsOpen);
      return;
    case Stage::InstrumentsOpen:
      open_directory("instruments", Stage::InstrumentNext, Stage::Finish);
      return;
    case Stage::InstrumentNext:
      next_entry(false, Stage::Finish);
      return;
    case Stage::FileRead: {
      const Progress p = reader_->step(room);
      if (p != Progress::Running) {
        finish_file(p);
      }
      return;
    }
    case Stage::Finish:
      close_all();
      report_.bytes_read = source_.total();
      state_ = State::Done;
      return;
  }
}

// ---- The summary line ---------------------------------------------------------------------------------------------------------------------
std::size_t describe_load(const OgFolderLoader& loader, char* out, std::size_t cap) noexcept {
  Line line(out, cap);
  const OgLoadReport& r = loader.report();
  switch (loader.state()) {
    case OgFolderLoader::State::Idle:
      return 0;
    case OgFolderLoader::State::Running:
      line.str(loader.folder()).str(": loading");
      break;
    case OgFolderLoader::State::Failed:
      line.str(loader.folder()).str(": FAILED ").str(r.detail);
      break;
    case OgFolderLoader::State::Done: {
      line.str(loader.folder()).str(": v").dec(r.version).str(", ").dec(r.patterns).str(r.patterns == 1u ? " pattern, " : " patterns, ")
          .dec(r.instruments).str(r.instruments == 1u ? " instrument" : " instruments");
      const std::uint32_t failed = r.patterns_failed + r.instruments_failed;
      if (failed != 0u) {
        line.str(", ").dec(failed).str(" failed");
      }
      if (r.unverified_version) {
        line.str(", unverified version");
      }
      if (r.micromoves_dropped != 0u) {  // OS-CEPAT S2 (A10): the strip names what a v6 pattern lost
        line.str(", ").dec(r.micromoves_dropped).str(r.micromoves_dropped == 1u ? " MICRO-MOVE NOT KEPT" : " MICRO-MOVES NOT KEPT");
      }
      const std::uint32_t changed = r.values_clamped + r.ce_fx_cleared;
      if (changed != 0u) {
        line.str(", ").dec(changed).str(" VALUES CHANGED");
      }
      break;
    }
  }
  return line.size();
}

}  // namespace o3os::app
