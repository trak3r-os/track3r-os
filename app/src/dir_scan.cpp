// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/dir_scan.h>

#include <o3os/formats/sample_probe.h>

#include <cstring>

namespace o3os::app {
namespace {

using hal::storage::DirEntry;
using hal::storage::Status;

bool same_text_or_null(const char* a, const char* b) noexcept {
  if (a == nullptr || b == nullptr) {
    return a == b;
  }
  return std::strcmp(a, b) == 0;
}

// "a/b/c" into `out` (kPathMax + 1 bytes), an empty `a` left out; false when it does not fit.
bool join3(char* out, const char* a, const char* b, const char* c) noexcept {
  std::size_t n = 0;
  const char* parts[3] = {a, b, c};
  for (const char* part : parts) {
    const std::size_t len = std::strlen(part);
    if (len == 0u) {
      continue;
    }
    const std::size_t need = len + (n != 0u ? 1u : 0u);
    if (n + need > hal::storage::kPathMax) {
      return false;
    }
    if (n != 0u) {
      out[n++] = '/';
    }
    std::memcpy(out + n, part, len);
    n += len;
  }
  out[n] = '\0';
  return n != 0u;
}

}  // namespace

bool folder_marked_deleted(hal::storage::Storage& storage, const char* folder) noexcept {
  char path[hal::storage::kPathMax + 1];
  bool is_dir = false;
  return folder != nullptr && join3(path, folder, ".deleted", "") && storage.exists(path, &is_dir) == Status::Ok && !is_dir;
}

namespace {

// formats::ProbeRead over an open file.
struct ProbeFile {
  hal::storage::Storage* storage;
  hal::storage::FileHandle file;
};

bool probe_read(void* ctx, std::uint32_t offset, std::uint8_t* dst, std::size_t n) noexcept {
  ProbeFile& f = *static_cast<ProbeFile*>(ctx);
  std::size_t got = 0;
  return f.storage->seek(f.file, offset) == Status::Ok && f.storage->read(f.file, dst, n, got) == Status::Ok && got == n;
}

// What a storage answer that ended a scan is, as a state.
DirScan::State state_of(Status s) noexcept {
  switch (s) {
    case Status::NoCard:
    case Status::NotMounted:
      return DirScan::State::NoCard;
    case Status::NotFound:
    case Status::NotADirectory:
      return DirScan::State::NoFolder;
    default:
      return DirScan::State::Failed;
  }
}

core::JobStatus job_status_of(DirScan::State s) noexcept {
  switch (s) {
    case DirScan::State::Scanning:
      return core::JobStatus::Running;
    case DirScan::State::Done:
      return core::JobStatus::Done;
    default:
      return core::JobStatus::Failed;
  }
}

}  // namespace

bool DirScan::ends_with_ci(const char* name, const char* tail) noexcept {
  const std::size_t tn = std::strlen(tail);
  const std::size_t nn = std::strlen(name);
  if (tn == 0u || nn <= tn) {
    return false;  // nothing to match, or the name is the extension alone (".wav": no stem, not a sample)
  }
  for (std::size_t i = 0; i < tn; ++i) {
    if (fold_ascii(static_cast<unsigned char>(name[nn - tn + i])) != fold_ascii(static_cast<unsigned char>(tail[i]))) {
      return false;
    }
  }
  return true;
}

ModuleName module_name_kind(const char* name) noexcept {
  if (DirScan::ends_with_ci(name, ".mod")) {
    return ModuleName::Mod;
  }
  if (DirScan::ends_with_ci(name, ".s3m")) {
    return ModuleName::S3m;
  }
  if (DirScan::ends_with_any_ci(name, ".med .mmd0 .mmd1 .mmd2 .mmd3")) {
    return ModuleName::Med;
  }
  // The prefix names ("mod.title"): the word, a '.', and at least one more character.
  struct Prefix {
    const char* word;
    ModuleName kind;
  };
  static constexpr Prefix kPrefixes[] = {{"mod.", ModuleName::Mod},   {"s3m.", ModuleName::S3m},   {"med.", ModuleName::Med},   {"mmd0.", ModuleName::Med},
                                         {"mmd1.", ModuleName::Med}, {"mmd2.", ModuleName::Med}, {"mmd3.", ModuleName::Med}};
  for (const Prefix& p : kPrefixes) {
    const std::size_t n = std::strlen(p.word);
    std::size_t i = 0;
    while (i < n && name[i] != '\0' && fold_ascii(static_cast<unsigned char>(name[i])) == static_cast<unsigned char>(p.word[i])) {
      ++i;
    }
    if (i == n && name[n] != '\0') {
      return p.kind;
    }
  }
  return ModuleName::None;
}

bool DirScan::ends_with_any_ci(const char* name, const char* list) noexcept {
  while (*list != '\0') {
    const char* end = list;
    while (*end != '\0' && *end != ' ') {
      ++end;
    }
    char word[16];
    const std::size_t n = static_cast<std::size_t>(end - list);
    if (n != 0u && n < sizeof word) {
      std::memcpy(word, list, n);
      word[n] = '\0';
      if (ends_with_ci(name, word)) {
        return true;
      }
    }
    list = *end == ' ' ? end + 1 : end;
  }
  return false;
}

void DirScan::close_dir() noexcept {
  if (storage_ != nullptr && dir_.is_open()) {
    storage_->closedir(dir_);
  }
  dir_ = hal::storage::DirHandle{};
}

void DirScan::end(State state, Status status) noexcept {
  close_dir();
  state_ = state;
  error_ = status;
}

DirScan::Begin DirScan::begin(hal::storage::Storage& storage, const char* folder, const ListFilter& filter) noexcept {
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
  if (state_ == State::Done && storage_ == &storage && std::strlen(folder_) == len && std::memcmp(folder_, folder, len) == 0 &&
      filter_ != nullptr && same_text_or_null(filter_->extension, filter.extension) && same_text_or_null(filter_->extension2, filter.extension2) && same_text_or_null(filter_->probe, filter.probe) &&
      same_text_or_null(filter_->skip_probe, filter.skip_probe) && same_text_or_null(filter_->native_probe, filter.native_probe) && same_text_or_null(filter_->recover_probe, filter.recover_probe) &&
      filter_->skip_hidden == filter.skip_hidden && filter_->all_files == filter.all_files && filter_->module_names == filter.module_names) {
    return Begin::Cached;
  }
  close_dir();  // a scan that is running is abandoned (through the old storage: it owns the handle)
  std::memcpy(folder_, folder, len);
  folder_[len] = '\0';
  storage_ = &storage;
  filter_ = &filter;
  index_.clear();
  skipped_ = 0;
  error_ = Status::Ok;
  state_ = State::Scanning;
  return Begin::Started;
}

void DirScan::invalidate() noexcept {
  close_dir();
  index_.clear();
  skipped_ = 0;
  error_ = Status::Ok;
  folder_[0] = '\0';
  state_ = State::Idle;
}

void DirScan::abandon() noexcept { close_dir(); }

void DirScan::cancel() noexcept {
  if (state_ == State::Scanning) {
    end(State::Cancelled, Status::Ok);
  }
}

const char* DirScan::detail() const noexcept {
  switch (state_) {
    case State::NoCard: return "NO CARD";
    case State::NoFolder: return "NO FOLDER";
    case State::Failed: return "CARD ERROR";
    default: return "";
  }
}

bool DirScan::probe_stereo(const char* name) noexcept {
  char path[hal::storage::kPathMax + 1];
  if (!join3(path, folder_, name, "")) {
    return false;
  }
  ProbeFile f{storage_, {}};
  if (storage_->open(path, f.file) != Status::Ok) {
    return false;
  }
  const bool stereo = formats::sample_channels(probe_read, &f) == 2u;
  storage_->close(f.file);
  return stereo;
}

// OS-USBDISK 6.3 (F10): the folder Windows makes on every volume it mounts (ASCII case-insensitive, as FAT names compare). It is the computer's, not a place the user keeps anything, so no listing shows it.
static bool is_volume_folder(const DirEntry& entry) noexcept {
  static constexpr char kName[] = "System Volume Information";
  if (!entry.is_dir) {
    return false;
  }
  for (std::size_t i = 0; i < sizeof kName; ++i) {  // (the NUL of both ends the compare)
    const char c = entry.name[i];
    if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c) != (kName[i] >= 'A' && kName[i] <= 'Z' ? static_cast<char>(kName[i] + ('a' - 'A')) : kName[i])) {
      return false;
    }
    if (c == '\0') {
      break;
    }
  }
  return true;
}

// One entry read. Returns what it cost in units; may end the scan (the list filled up, or the probe met a card that failed).
unsigned DirScan::take(const DirEntry& entry) noexcept {
  if (entry.name[0] == '\0' || (filter_->skip_hidden && entry.name[0] == '.') || is_volume_folder(entry)) {
    ++skipped_;
    return 1;
  }
  bool listed = false;
  unsigned cost = 1;
  EntryTag tag = EntryTag::None;
  bool stereo = false;
  if (entry.is_dir) {
    if (filter_->probe == nullptr) {
      listed = true;
    } else {
      // The probes, native first: a folder that holds both files is the native project (the user's own save is the newer state); then the backup of one, which only counts when the
      // file is not there (an interrupted save: the folder is a project to recover); then the OG file. Each is a path walk, a unit cost of its own.
      if (filter_->skip_probe != nullptr) {  // OS-FILES part B: a folder DELETE marked is not listed
        char marked[hal::storage::kPathMax + 1];
        cost += kProbeUnits;
        bool marker_is_dir = false;
        if (join3(marked, folder_, entry.name, filter_->skip_probe) && storage_->exists(marked, &marker_is_dir) == Status::Ok && !marker_is_dir) {
          ++skipped_;
          return cost;
        }
      }
      const char* const probes[3] = {filter_->native_probe, filter_->recover_probe, filter_->probe};
      const EntryTag tags[3] = {EntryTag::Native, EntryTag::Recover, EntryTag::Og};
      for (unsigned p = 0; p < 3 && !listed; ++p) {
        if (probes[p] == nullptr) {
          continue;
        }
        char path[hal::storage::kPathMax + 1];
        cost += kProbeUnits;
        if (!join3(path, folder_, entry.name, probes[p])) {
          continue;
        }
        bool probe_is_dir = false;
        const Status s = storage_->exists(path, &probe_is_dir);
        if (s == Status::Ok) {
          listed = !probe_is_dir;  // the probe must be a file: a folder called project.mt is not a project
          tag = tags[p];
        } else if (s != Status::NotFound && s != Status::NotADirectory && s != Status::BadPath) {
          end(state_of(s), s);  // the card failed: not a verdict on this folder
          return cost;
        }
      }
    }
  } else {
    listed = filter_->all_files || (filter_->extension != nullptr && (ends_with_any_ci(entry.name, filter_->extension) || (filter_->extension2 != nullptr && ends_with_ci(entry.name, filter_->extension2)))) ||
             (filter_->module_names && module_name_kind(entry.name) != ModuleName::None);
    if (listed && filter_->tag_stereo && index_.count() < DirIndex::kMaxEntries && ends_with_any_ci(entry.name, ".wav .aif .aiff .aifc")) {
      cost += kProbeUnits;
      stereo = probe_stereo(entry.name);
    }
  }
  if (!listed) {
    ++skipped_;
  } else if (index_.add(entry.name, entry.is_dir, tag, stereo) == DirIndex::kNone && index_.full()) {
    end(State::Done, Status::Ok);  // the list is full: the rest of the folder is not shown (truncated())
  }
  return cost;
}

core::JobStatus DirScan::step(std::uint32_t budget_us) noexcept {
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

}  // namespace o3os::app
