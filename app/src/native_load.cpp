// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/native_load.h>

#include "text_line.h"

#include <cstring>

namespace o3os::app {
namespace {

namespace native = formats::native;
using hal::storage::Status;

constexpr std::size_t kMinRoom = 512;  // a slice always moves something, whatever budget it was given
// The names the save layer writes (hal/save_guard.h kProjectFile and kBackupSuffix; the guard is the save layer's alone, so they are spelled here, and the tests keep them together).
constexpr char kProjectFile[] = "project.o3pj";
constexpr char kBackupSuffix[] = ".bak";

}  // namespace

void NativeFolderLoader::close_file() noexcept {
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  file_ = hal::storage::FileHandle{};
  reader_.abandon();  // OS-MIDIREC-P1 S2 fold: a load dropped mid-lane (begin, begin_fresh, abort) must not leave the LaneStore's write window open (the render would skip the lanes)
}

bool NativeFolderLoader::begin(hal::storage::Storage& storage, const char* folder, model::ModelArena& arena, bool backup) noexcept {
  close_file();  // anything running is abandoned
  if (!hal::storage::valid_path(folder)) {
    return false;
  }
  while (folder[0] == '/') {
    ++folder;
  }
  std::size_t end = std::strlen(folder);
  while (end != 0u && folder[end - 1] == '/') {
    --end;
  }
  if (end == 0u || end > OgFolderLoader::kFolderMax) {  // (the file name then always fits the path buffer of step())
    return false;
  }
  std::memcpy(base_, folder, end);
  base_[end] = '\0';
  storage_ = &storage;
  arena_ = &arena;
  report_ = NativeLoadReport{};
  report_.backup = backup;
  source_.reset();
  fresh_ = false;
  keep_instruments_ = false;
  reading_ = false;
  active_ = true;
  state_ = State::Running;
  return true;
}

void NativeFolderLoader::begin_fresh(model::ModelArena& arena, bool keep_instruments) noexcept {
  close_file();
  arena_ = &arena;
  base_[0] = '\0';
  report_ = NativeLoadReport{};
  source_.reset();
  fresh_ = true;
  keep_instruments_ = keep_instruments;
  reading_ = false;
  active_ = true;
  state_ = State::Running;
}

void NativeFolderLoader::abort() noexcept {
  close_file();
  if (state_ == State::Running) {
    state_ = State::Idle;
  }
}

void NativeFolderLoader::fail(native::Error e, const char* detail) noexcept {
  close_file();
  report_.error = e;
  report_.detail = detail;
  report_.bytes_read = source_.total();
  state_ = State::Failed;
}

NativeFolderLoader::State NativeFolderLoader::step(std::size_t byte_budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  if (fresh_) {  // a new project: the model's defaults, one step (the app's frames are held, the transport is stopped and the voices have settled: the platform's phases)
    model::Project& project = arena_->project;
    project.reset(keep_instruments_);
    project.start_new();  // OS-DEFSCALE: a new project starts in C major (a load keeps the reader's defaults)
    if (lanes_ != nullptr) {
      lanes_->sync(project.load_epoch());  // the reset moved the epoch: no lane of the old project survives (OS-MIDIREC-P1)
    }
    if (project.create_pattern(1, model::kDefaultPatternLength) != model::Status::Ok || project.append_order(1) != model::Status::Ok) {
      fail(native::Error::Internal, "NEW PROJECT FAILED");  // (a reset project has room for a pattern: never input, never reached)
      return state_;
    }
    report_.patterns = 1;
    state_ = State::Done;
    return state_;
  }
  if (!reading_) {  // the first slice: open the file (the document is untouched until it is open) and start the reader, which resets the project
    char path[hal::storage::kPathMax + 1];
    const std::size_t base = std::strlen(base_);
    std::memcpy(path, base_, base);
    path[base] = '/';
    std::memcpy(path + base + 1, kProjectFile, sizeof kProjectFile);
    if (report_.backup) {
      std::memcpy(path + base + sizeof kProjectFile, kBackupSuffix, sizeof kBackupSuffix);  // overwrites the terminator
    }
    const Status s = storage_->open(path, file_);
    if (s != Status::Ok) {
      report_.storage_error = s;
      fail(native::Error::SourceFailed, s == Status::NotFound ? "NO PROJECT FILE" : hal::storage::status_name(s));
      return state_;
    }
    source_.attach(*storage_, file_);
    reader_.begin(source_, *arena_, lanes_);
    reading_ = true;
  }
  const native::Progress p = reader_.step(byte_budget < kMinRoom ? kMinRoom : byte_budget);
  if (p == native::Progress::Running) {
    return state_;
  }
  const native::Report& r = reader_.report();
  report_.bytes_read = source_.total();
  report_.patterns = r.patterns;
  report_.instruments = r.instruments;
  report_.unknown_dropped = r.unknown_dropped;
  report_.newer_minor = r.newer_minor;
  report_.mevt_skipped = r.mevt_skipped;
  report_.sanitised = r.settings_skipped;
  if (p == native::Progress::Done) {
    close_file();
    state_ = State::Done;
    return state_;
  }
  if (source_.error() != Status::Ok) {  // a card that fails is not a bad file
    report_.storage_error = source_.error();
    fail(native::Error::SourceFailed, hal::storage::status_name(source_.error()));
    return state_;
  }
  fail(r.error, r.detail[0] != '\0' ? r.detail : native::error_name(r.error));
  return state_;
}

// ---- ProjectVerify (OS-QAFIX-4) -------------------------------------------------------------------------------------------------------------------------------------------------------------

void ProjectVerify::close_file() noexcept {
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  file_ = hal::storage::FileHandle{};
}

void ProjectVerify::refuse(const char* detail) noexcept {
  close_file();
  detail_ = detail;
  state_ = State::Refused;
}

void ProjectVerify::abort() noexcept {
  close_file();
  if (state_ == State::Running) {
    state_ = State::Idle;
  }
}

bool ProjectVerify::begin(hal::storage::Storage& storage, const char* folder, bool og, bool backup) noexcept {
  close_file();
  state_ = State::Idle;
  if (!hal::storage::valid_path(folder)) {
    return false;
  }
  while (folder[0] == '/') {
    ++folder;
  }
  std::size_t end = std::strlen(folder);
  while (end != 0u && folder[end - 1] == '/') {
    --end;
  }
  if (end == 0u || end > OgFolderLoader::kFolderMax) {  // (the same rule as the loaders' begin(): what passes here passes there)
    return false;
  }
  std::memcpy(base_, folder, end);
  base_[end] = '\0';
  storage_ = &storage;
  og_ = og;
  backup_ = backup && !og;
  reading_ = false;
  detail_ = "";
  source_.reset();
  state_ = State::Running;
  return true;
}

namespace {

// What the strip says for a file the verify refused (it has 48 characters: "PROJECT <name> FAILED: " and these words; the Reader's own sentences are for the log and the tools).
const char* refusal_words(formats::native::Error e) noexcept {
  switch (e) {
    case formats::native::Error::Truncated: return "THE FILE IS CUT SHORT";
    case formats::native::Error::BadMagic: return "NOT AN O3PJ PROJECT FILE";
    case formats::native::Error::BadCrc: return "BAD CHECKSUM";
    case formats::native::Error::NewerMajor: return "MADE BY A NEWER OS VERSION";
    case formats::native::Error::LimitExceeded: return "TOO BIG FOR THIS OS";
    default: return "THE FILE IS DAMAGED";
  }
}

}  // namespace

ProjectVerify::State ProjectVerify::step(std::size_t byte_budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  if (!reading_) {
    char path[hal::storage::kPathMax + 1];
    const std::size_t base = std::strlen(base_);
    const char* const name = og_ ? "project.mt" : kProjectFile;
    const std::size_t name_len = std::strlen(name);
    if (base + 1u + name_len + sizeof kBackupSuffix > sizeof path) {
      refuse("PATH TOO LONG");
      return state_;
    }
    std::memcpy(path, base_, base);
    path[base] = '/';
    std::memcpy(path + base + 1, name, name_len + 1u);
    if (backup_) {
      std::memcpy(path + base + 1u + name_len, kBackupSuffix, sizeof kBackupSuffix);
    }
    const Status s = storage_->open(path, file_);
    if (s != Status::Ok) {
      refuse(s == Status::NotFound ? "NO PROJECT FILE" : hal::storage::status_name(s));
      return state_;
    }
    if (og_) {  // the OG project file has no checksum: its size and header are the one check there is (formats::og::project_frame_ok, the reader's own rule)
      std::uint32_t size = 0;
      std::uint8_t head[16] = {};
      std::size_t got = 0;
      Status z = storage_->size(file_, size);
      if (z == Status::Ok) {
        z = storage_->read(file_, head, sizeof head, got);
      }
      const char* why = "";
      if (z != Status::Ok) {
        refuse(hal::storage::status_name(z));
      } else if (!formats::og::project_frame_ok(head, got, size, &why)) {
        refuse(why);
      } else {
        close_file();
        state_ = State::Ok;
      }
      return state_;
    }
    source_.attach(*storage_, file_);
    verifier_.begin(source_);
    reading_ = true;
  }
  const formats::Progress p = verifier_.step(byte_budget < kMinRoom ? kMinRoom : byte_budget);
  if (p == formats::Progress::Running) {
    return state_;
  }
  if (p == formats::Progress::Done) {
    close_file();
    state_ = State::Ok;
    return state_;
  }
  if (source_.error() != Status::Ok) {  // a card that fails is not a bad file
    refuse(hal::storage::status_name(source_.error()));
    return state_;
  }
  refuse(refusal_words(verifier_.report().error));
  return state_;
}

std::size_t describe_native_load(const NativeFolderLoader& loader, char* out, std::size_t cap) noexcept {
  Line line(out, cap);
  const NativeLoadReport& r = loader.report();
  switch (loader.state()) {
    case NativeFolderLoader::State::Idle:
      return 0;
    case NativeFolderLoader::State::Running:
      line.str(loader.folder()).str(": loading");
      break;
    case NativeFolderLoader::State::Failed:
      line.str(loader.folder()).str(": FAILED ").str(r.detail);
      break;
    case NativeFolderLoader::State::Done:
      if (loader.fresh()) {
        line.str("new project");
        break;
      }
      line.str(loader.folder()).str(": native, ").dec(r.patterns).str(r.patterns == 1u ? " pattern, " : " patterns, ").dec(r.instruments)
          .str(r.instruments == 1u ? " instrument" : " instruments");
      if (r.backup) {
        line.str(", from the backup");
      }
      if (r.newer_minor) {
        line.str(", newer fields skipped");
      }
      if (r.mevt_skipped != 0u) {
        line.str(", ").dec(r.mevt_skipped).str(r.mevt_skipped == 1u ? " MIDI lane skipped" : " MIDI lanes skipped");
      }
      if (r.sanitised != 0u) {
        line.str(", ").dec(r.sanitised).str(r.sanitised == 1u ? " value reset" : " values reset");
      }
      if (r.unknown_dropped != 0u) {
        line.str(", ").dec(r.unknown_dropped).str(" chunks dropped");
      }
      break;
  }
  return line.size();
}

}  // namespace o3os::app
