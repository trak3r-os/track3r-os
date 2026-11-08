// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/module/ptfx_catalog.h>

#include <cstring>

namespace o3os::module {
namespace {

using hal::storage::DirEntry;
using hal::storage::Status;

constexpr unsigned char fold(unsigned char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c; }

// "a/b" into `out` (kPathMax + 1 bytes); false when it does not fit.
bool join(char* out, const char* folder, const char* name) noexcept {
  std::size_t n = std::strlen(folder);
  if (n > hal::storage::kPathMax) {
    return false;
  }
  std::memcpy(out, folder, n);
  if (n != 0u && out[n - 1u] != '/') {
    if (n + 1u > hal::storage::kPathMax) {
      return false;
    }
    out[n++] = '/';
  }
  const std::size_t len = std::strlen(name);
  if (n + len > hal::storage::kPathMax) {
    return false;
  }
  std::memcpy(out + n, name, len + 1u);
  return true;
}

// What a storage answer that ended a scan is, as a state.
PtfxCatalog::State state_of(Status s) noexcept {
  switch (s) {
    case Status::NoCard:
    case Status::NotMounted:
      return PtfxCatalog::State::NoCard;
    case Status::NotFound:
    case Status::NotADirectory:
    case Status::BadPath:
      return PtfxCatalog::State::NoFolder;
    default:
      return PtfxCatalog::State::Failed;
  }
}

core::JobStatus job_status_of(PtfxCatalog::State s) noexcept {
  switch (s) {
    case PtfxCatalog::State::Scanning:
      return core::JobStatus::Running;
    case PtfxCatalog::State::Done:
      return core::JobStatus::Done;
    default:
      return core::JobStatus::Failed;
  }
}

int compare_text_ci(const char* a, const char* b) noexcept {
  for (std::size_t i = 0;; ++i) {
    const unsigned char ca = fold(static_cast<unsigned char>(a[i]));
    const unsigned char cb = fold(static_cast<unsigned char>(b[i]));
    if (ca != cb) {
      return ca < cb ? -1 : 1;
    }
    if (ca == 0u) {
      return 0;
    }
  }
}

}  // namespace

std::uint16_t ptfx_plugin_id(const char* file_name) noexcept {
  std::uint32_t h = 2166136261u;  // FNV-1a
  for (const char* p = "ptfx:"; *p != '\0'; ++p) {
    h = (h ^ static_cast<std::uint8_t>(*p)) * 16777619u;
  }
  for (const char* p = file_name; *p != '\0'; ++p) {
    h = (h ^ static_cast<std::uint8_t>(*p)) * 16777619u;
  }
  return static_cast<std::uint16_t>(0x8000u | (h & 0x7FFFu));
}

PtfxCatalog::PtfxCatalog(PtfxEntry* entries) noexcept : entries_(entries) {}

bool PtfxCatalog::is_ptfx_name(const char* name) noexcept {
  static constexpr char kExt[] = ".ptfx";
  constexpr std::size_t kExtLen = sizeof kExt - 1u;
  const std::size_t n = std::strlen(name);
  if (n <= kExtLen) {
    return false;  // no stem before the extension
  }
  for (std::size_t i = 0; i < kExtLen; ++i) {
    if (fold(static_cast<unsigned char>(name[n - kExtLen + i])) != fold(static_cast<unsigned char>(kExt[i]))) {
      return false;
    }
  }
  return true;
}

int PtfxCatalog::compare_names(const PtfxEntry& a, const PtfxEntry& b) noexcept {
  const int d = compare_text_ci(a.display_name, b.display_name);
  return d != 0 ? d : compare_text_ci(a.file_name, b.file_name);
}

bool PtfxCatalog::quarantine(std::uint16_t id) noexcept {
  for (unsigned i = 0; i < count_; ++i) {
    if (entries_[i].id == id) {
      entries_[i].quarantined = true;
      return true;
    }
  }
  return false;
}

void PtfxCatalog::close_dir() noexcept {
  if (storage_ != nullptr && dir_.is_open()) {
    storage_->closedir(dir_);
  }
  dir_ = hal::storage::DirHandle{};
}

void PtfxCatalog::end(State state, Status status) noexcept {
  close_dir();
  state_ = state;
  error_ = status;
}

PtfxCatalog::Begin PtfxCatalog::begin(hal::storage::Storage& storage, const char* folder) noexcept {
  if (!hal::storage::valid_path(folder)) {
    return Begin::BadPath;
  }
  while (folder[0] == '/') {
    ++folder;  // a leading '/' means the same as none
  }
  std::size_t len = std::strlen(folder);
  while (len != 0u && folder[len - 1u] == '/') {
    --len;  // and so does a trailing one
  }
  if (len > kFolderMax) {
    return Begin::BadPath;
  }
  close_dir();  // a scan that is running is abandoned (through the old storage: it owns the handle)
  std::memcpy(folder_, folder, len);
  folder_[len] = '\0';
  storage_ = &storage;
  count_ = 0;
  seen_ = 0;
  overflow_ = 0;
  skipped_long_ = 0;
  skipped_other_ = 0;
  card_errors_ = 0;
  error_ = Status::Ok;
  state_ = State::Scanning;
  return Begin::Started;
}

void PtfxCatalog::invalidate() noexcept {
  close_dir();
  count_ = 0;
  seen_ = 0;
  overflow_ = 0;
  skipped_long_ = 0;
  skipped_other_ = 0;
  card_errors_ = 0;
  error_ = Status::Ok;
  folder_[0] = '\0';
  state_ = State::Idle;
}

void PtfxCatalog::abandon() noexcept { close_dir(); }

void PtfxCatalog::cancel() noexcept {
  if (state_ == State::Scanning) {
    end(State::Cancelled, Status::Ok);
  }
}

const char* PtfxCatalog::detail() const noexcept {
  switch (state_) {
    case State::NoCard: return "NO CARD";
    case State::NoFolder: return "NO FOLDER";
    case State::Failed: return "CARD ERROR";
    default: return "";
  }
}

void PtfxCatalog::insert(const PtfxEntry& e) noexcept {
  const bool full = count_ == kPtfxCatalogMax;
  if (full) {
    ++overflow_;  // the list is full: this file is one more than the cap, whether it sorts after the last kept (dropped) or displaces it
    if (compare_names(entries_[count_ - 1u], e) <= 0) {
      return;
    }
  }
  unsigned i = full ? kPtfxCatalogMax - 1u : count_;
  while (i > 0u && compare_names(entries_[i - 1u], e) > 0) {
    entries_[i] = entries_[i - 1u];
    --i;
  }
  entries_[i] = e;
  if (!full) {
    ++count_;
  }
}

unsigned PtfxCatalog::take(const DirEntry& entry) noexcept {
  ++seen_;
  if (entry.is_dir || entry.name[0] == '\0' || entry.name[0] == '.' || !is_ptfx_name(entry.name)) {
    ++skipped_other_;  // a folder, hidden, not a .ptfx file
    return 1;
  }
  const std::size_t name_len = std::strlen(entry.name);
  if (name_len > kPtfxFileNameMax) {
    ++skipped_long_;  // A14: over 47 characters is skipped and counted
    return 1;
  }
  // One 472-byte header read: open, size, one read, close (the directory stays open: that is what kMaxOpenFiles is for).
  char path[hal::storage::kPathMax + 1];
  if (!join(path, folder_, entry.name)) {
    ++skipped_other_;  // the path does not fit: the name alone was fine, the folder made it too long
    return 1;
  }
  hal::storage::FileHandle file{};
  Status s = storage_->open(path, file);
  if (s == Status::NotFound || s == Status::NotAFile) {
    ++skipped_other_;  // gone between the listing and the open
    return 1;
  }
  if (s != Status::Ok) {
    ++card_errors_;  // 3.1: a card error is not a refusal: the file is retried at the next scan
    return kHeaderReadUnits;  // an open that failed still cost the step its header read
  }
  std::uint32_t file_size = 0;
  s = storage_->size(file, file_size);
  std::uint8_t header[kPtfxHeaderSize];
  std::size_t got = 0;
  if (s == Status::Ok) {
    s = storage_->read(file, header, sizeof header, got);
  }
  storage_->close(file);
  if (s != Status::Ok) {
    ++card_errors_;
    return kHeaderReadUnits;
  }
  PtfxInfo info;
  const PtfxStatus status = ptfx_parse_header(header, got, file_size, info);
  PtfxEntry e{};
  std::memcpy(e.file_name, entry.name, name_len + 1u);
  // A refusal before the name (NOT A PTFX PLUGIN) leaves the name empty: the file name is the display name.
  const char* display = info.name[0] != '\0' ? info.name : entry.name;
  std::size_t display_len = std::strlen(display);
  if (display_len > sizeof e.display_name - 1u) {
    display_len = sizeof e.display_name - 1u;
  }
  std::memcpy(e.display_name, display, display_len);
  e.display_name[display_len] = '\0';
  e.flags = info.flags;
  e.status = status;
  e.id = ptfx_plugin_id(e.file_name);
  e.quarantined = qset_ != nullptr && qset_->contains(e.id);
  insert(e);
  return kHeaderReadUnits;  // 3.1: one directory page or one header read a step: this ends the step whatever the budget left
}

core::JobStatus PtfxCatalog::step(std::uint32_t budget_us) noexcept {
  if (state_ != State::Scanning) {
    return job_status_of(state_);
  }
  if (!dir_.is_open()) {
    const Status s = storage_->opendir(folder_[0] != '\0' ? folder_ : "/", dir_);
    if (s != Status::Ok) {
      dir_ = hal::storage::DirHandle{};
      end(state_of(s), s);
      return job_status_of(state_);
    }
  }
  unsigned budget = budget_us / kUnitUs;
  budget = budget < 1u ? 1u : (budget > kMaxUnitsPerStep ? kMaxUnitsPerStep : budget);
  unsigned used = 0;
  DirEntry entry;
  while (state_ == State::Scanning && used < budget) {  // each pass of the loop spends at least one unit: bounded by kMaxUnitsPerStep
    const Status s = storage_->readdir(dir_, entry);
    if (s == Status::End) {
      end(State::Done, Status::Ok);
    } else if (s != Status::Ok) {
      if (s == Status::BadHandle) {
        dir_ = hal::storage::DirHandle{};  // the card was remounted under us: the slot is not ours to close
      }
      end(state_of(s), s);
    } else {
      entry.name[hal::storage::kNameMax] = '\0';  // the contract says terminated; a name is never read past its buffer whatever a storage does
      used += take(entry);
    }
  }
  return job_status_of(state_);
}

unsigned PtfxCatalog::progress() const noexcept {
  return state_ == State::Done ? core::kJobProgressMax : core::permille_of(seen_, kPtfxCatalogMax);
}

}  // namespace o3os::module
