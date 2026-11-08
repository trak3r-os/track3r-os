// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An in-memory card for the save layer's tests (OS-007n): both sides of the contract, hal::storage::Storage (read) and hal::storage::StorageWriter (write), over one
// map of files, with the faults a card has. It applies the REAL path guard (hal/save_guard.h) to every mutating call and counts the calls the guard refuses
// (`violations`), so a test can assert that the save job never asked for anything unguarded; it follows FatFs's rules (case-insensitive names, rename onto an existing
// name is refused, remove of a missing file is Ok) and keeps the log of every change (`log`) for the "never deletes" assertions.
//
// Power loss: `cut_at` = N makes the N-th mutating call (counted from 0) and every later one fail with Io, the cut call having done nothing, except a write, which lands
// half of its bytes (a torn chunk). revive() is the next boot: the cut is cleared and what is on the "card" is what was left.
#include <o3os/hal/storage.h>
#include <o3os/hal/storage_write.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace o3os::test {

namespace hs = hal::storage;
namespace guard = hal::storage::save;

class MemCard final : public hs::Storage, public hs::StorageWriter {
 public:
  using Bytes = std::vector<std::uint8_t>;

  // ---- what a test sets and reads
  std::map<std::string, Bytes> files;  // key: the path in lower case
  std::set<std::string> dirs;          // likewise
  std::vector<std::string> log;        // "mkdir P", "create P", "write N", "finish", "rename A>B", "remove P", in order
  hs::WriteStatus gate_status = hs::WriteStatus::Ok;
  std::size_t cut_at = ~std::size_t{0};
  std::size_t fail_at = ~std::size_t{0};          // the mutating call with this index fails once with fail_with (nothing is done), later ones work
  hs::WriteStatus fail_with = hs::WriteStatus::Io;
  std::size_t space_left = ~std::size_t{0};       // bytes write() may still take; one more is NoSpace
  bool corrupt_readback = false;                  // a byte of what read() gives for a Temp is flipped
  bool short_readback = false;                    // a Temp reads one byte short
  hs::Status open_status = hs::Status::Ok;        // open() answers this when it is not Ok
  std::string read_fault;                         // read() of this file (its key: lower case) fails with Io; empty: none
  std::size_t read_cap = ~std::size_t{0};         // read() gives at most this many bytes a call (a partial read; 0 gives none: an end of file)
  std::size_t read_over = 0;                      // read() reports this many bytes more than it gave (a backend that over-reports)
  bool seek_fault = false;                        // seek() fails with Io
  std::string size_fault;                         // size() of this file (its key: lower case) fails with Io; empty: none
  std::size_t readdir_fail_at = ~std::size_t{0};  // the readdir call with this index (counted from 0) fails once with Io; default: never
  std::size_t readdir_calls = 0;                  // readdir calls so far
  std::size_t ops = 0;                            // mutating calls so far
  int open_handles = 0;                           // files open() gave and close() has not taken back (a job that ends or is cancelled must leave none)
  std::size_t exists_calls = 0;                   // exists() lookups so far (a directory lookup each on a real card)
  unsigned violations = 0;                        // mutating calls the guard refused
  bool dead = false;
  // OS-FU5: the card's identity as the real writer sees it (the volume serial). begin_save() pins it and every mutating call after refuses with NoCard when it moved;
  // `swap_after_renames`: the id changes after that many renames (another card between two jobs of a save).
  std::uint32_t id = 1;
  std::size_t swap_after_renames = ~std::size_t{0};
  unsigned begins = 0;
  unsigned ends = 0;

  static std::string key(const char* path) {
    std::string k(path);
    for (char& c : k) {
      c = guard::detail::lower(c);
    }
    return k;
  }
  void put(const char* path, const Bytes& data) { files[key(path)] = data; }
  void put(const char* path, const std::string& text) { files[key(path)] = Bytes(text.begin(), text.end()); }
  void make_dir_now(const char* path) { dirs.insert(key(path)); }
  bool has(const char* path) const { return files.count(key(path)) != 0; }
  const Bytes* get(const char* path) const {
    const auto it = files.find(key(path));
    return it == files.end() ? nullptr : &it->second;
  }
  bool same(const char* path, const Bytes& data) const {
    const Bytes* b = get(path);
    return b != nullptr && *b == data;
  }
  void revive() {
    dead = false;
    cut_at = ~std::size_t{0};
    ops = 0;
    log.clear();
    writing_ = false;
  }
  std::vector<std::string> removed() const {
    std::vector<std::string> out;
    for (const std::string& l : log) {
      if (l.rfind("remove ", 0) == 0) {
        out.push_back(l.substr(7));
      }
    }
    return out;
  }

  // ---- hal::storage::Storage (the read side)
  hs::Status mount() noexcept override { return hs::Status::Ok; }
  bool mounted() const noexcept override { return true; }
  bool card_present() const noexcept override { return true; }
  hs::VolumeInfo volume() const noexcept override { return hs::VolumeInfo{hs::FsKind::HostDirectory, 0, 0}; }
  hs::Status exists(const char* path, bool* is_dir) noexcept override {
    ++exists_calls;
    const std::string k = key(path);
    if (files.count(k) != 0) {
      if (is_dir != nullptr) {
        *is_dir = false;
      }
      return hs::Status::Ok;
    }
    if (dirs.count(k) != 0) {
      if (is_dir != nullptr) {
        *is_dir = true;
      }
      return hs::Status::Ok;
    }
    return hs::Status::NotFound;
  }
  // OS-BUG-REMOUNT: size and a stamp that changes when the contents do (a stand-in for the modification time of a FAT entry); `touch` makes a stamp differ without a content change.
  std::map<std::string, std::uint32_t> touched;
  hs::Status stat(const char* path, hs::FileStamp& out) noexcept override {
    ++stat_calls;
    const auto it = files.find(key(path));
    if (it == files.end()) {
      return dirs.count(key(path)) != 0 ? hs::Status::NotAFile : hs::Status::NotFound;
    }
    std::uint32_t h = 2166136261u;
    for (std::uint8_t b : it->second) {
      h = (h ^ b) * 16777619u;
    }
    const auto t = touched.find(key(path));
    out.size = static_cast<std::uint32_t>(it->second.size());
    out.stamp = (h | 1u) ^ (t == touched.end() ? 0u : t->second);
    return hs::Status::Ok;
  }
  std::size_t stat_calls = 0;
  hs::Status open(const char* path, hs::FileHandle& out) noexcept override {
    if (open_status != hs::Status::Ok) {
      return open_status;
    }
    const auto it = files.find(key(path));
    if (it == files.end()) {
      return hs::Status::NotFound;
    }
    read_path_ = it->first;
    read_pos_ = 0;
    out.slot = 0;
    ++open_handles;
    return hs::Status::Ok;
  }
  hs::Status read(hs::FileHandle, void* dst, std::size_t max, std::size_t& got) noexcept override {
    const auto it = files.find(read_path_);
    if (it == files.end() || (!read_fault.empty() && read_path_ == read_fault)) {
      return hs::Status::Io;
    }
    std::size_t size = it->second.size();
    const bool temp = read_path_.size() >= 11 && read_path_.compare(read_path_.size() - 11, 11, "_o3save.tmp") == 0;
    if (temp && short_readback && size != 0) {
      --size;
    }
    const std::size_t cap = max < read_cap ? max : read_cap;
    const std::size_t n = read_pos_ >= size ? 0 : (size - read_pos_ < cap ? size - read_pos_ : cap);
    if (n != 0) {
      std::memcpy(dst, it->second.data() + read_pos_, n);
      if (temp && corrupt_readback && read_pos_ <= 7 && read_pos_ + n > 7) {
        static_cast<std::uint8_t*>(dst)[7 - read_pos_] ^= 0x40;
      }
    }
    read_pos_ += n;
    got = n + read_over;
    return hs::Status::Ok;
  }
  hs::Status seek(hs::FileHandle, std::uint32_t offset) noexcept override {
    if (seek_fault) {
      return hs::Status::Io;
    }
    read_pos_ = offset;
    return hs::Status::Ok;
  }
  hs::Status size(hs::FileHandle, std::uint32_t& out) const noexcept override {
    const auto it = files.find(read_path_);
    if (it == files.end() || (!size_fault.empty() && read_path_ == size_fault)) {
      return hs::Status::Io;
    }
    std::size_t s = it->second.size();
    const bool temp = read_path_.size() >= 11 && read_path_.compare(read_path_.size() - 11, 11, "_o3save.tmp") == 0;
    out = static_cast<std::uint32_t>(temp && short_readback && s != 0 ? s - 1 : s);
    return hs::Status::Ok;
  }
  void close(hs::FileHandle& file) noexcept override {
    if (file.slot != hs::FileHandle::kNone) {
      --open_handles;
    }
    file.slot = hs::FileHandle::kNone;
  }
  // A listing of the immediate children of a folder (OS-007o: the File scan runs over the card the power-loss sweeps leave): files and folders, in key order, names as the
  // keys have them (lower case).
  hs::Status opendir(const char* path, hs::DirHandle& out) noexcept override {
    std::string base = key(path);
    while (!base.empty() && (base.front() == '/' || base.back() == '/')) {
      base.erase(base.front() == '/' ? 0u : base.size() - 1u, 1u);
    }
    if (!base.empty() && dirs.count(base) == 0) {
      return hs::Status::NotFound;
    }
    listing_.clear();
    list_pos_ = 0;
    const std::string prefix = base.empty() ? std::string() : base + "/";
    const auto child = [&](const std::string& k, bool is_dir) {
      if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0 && k.find('/', prefix.size()) == std::string::npos) {
        listing_.push_back({k.substr(prefix.size()), is_dir});
      }
    };
    for (const std::string& d : dirs) {
      child(d, true);
    }
    for (const auto& f : files) {
      child(f.first, false);
    }
    out.slot = 0;
    return hs::Status::Ok;
  }
  hs::Status readdir(hs::DirHandle, hs::DirEntry& out) noexcept override {
    if (readdir_calls++ == readdir_fail_at) {
      return hs::Status::Io;
    }
    if (list_pos_ >= listing_.size()) {
      return hs::Status::End;
    }
    const auto& e = listing_[list_pos_++];
    std::memset(&out, 0, sizeof out);
    std::memcpy(out.name, e.first.c_str(), e.first.size() + 1u);
    out.is_dir = e.second;
    return hs::Status::Ok;
  }
  void closedir(hs::DirHandle& dir) noexcept override { dir.slot = hs::DirHandle::kNone; }

  // ---- hal::storage::StorageWriter
  hs::WriteStatus gate() noexcept override { return gate_status; }
  void begin_save() noexcept override {
    ++begins;
    pinned_ = true;
    pin_id_ = id;
  }
  void end_save() noexcept override {
    ++ends;
    pinned_ = false;
  }
  std::uint32_t card_id() const noexcept override { return id; }
  hs::WriteStatus make_dir(const char* path) noexcept override {
    if (!guard::may_make_dir(path)) {
      ++violations;
      return hs::WriteStatus::BadPath;
    }
    if (const hs::WriteStatus g = pre(); g != hs::WriteStatus::Ok) {
      return g;
    }
    // (the parent must exist)
    const std::string k = key(path);
    const std::size_t slash = k.rfind('/');
    if (slash != std::string::npos && dirs.count(k.substr(0, slash)) == 0) {
      return hs::WriteStatus::NotFound;
    }
    if (files.count(k) != 0) {
      return hs::WriteStatus::Exists;
    }
    if (dirs.insert(k).second) {
      log.push_back(std::string("mkdir ") + path);
    }
    return hs::WriteStatus::Ok;
  }
  hs::WriteStatus create(const char* path) noexcept override {
    if (!guard::may_create(path)) {
      ++violations;
      return hs::WriteStatus::BadPath;
    }
    if (writing_) {
      return hs::WriteStatus::Busy;
    }
    if (const hs::WriteStatus g = pre(); g != hs::WriteStatus::Ok) {
      return g;
    }
    const std::string k = key(path);
    const std::size_t slash = k.rfind('/');
    if (slash != std::string::npos && dirs.count(k.substr(0, slash)) == 0) {
      return hs::WriteStatus::NotFound;
    }
    if (files.count(k) != 0) {
      return hs::WriteStatus::Exists;
    }
    files[k] = Bytes();
    write_path_ = k;
    writing_ = true;
    log.push_back(std::string("create ") + path);
    return hs::WriteStatus::Ok;
  }
  // OS-FILES part B: the empty marker file of a DELETE; the real guard decides the path, nothing is ever written to it.
  hs::WriteStatus create_marker(const char* path) noexcept override {
    if (!guard::may_create_marker(path)) {
      ++violations;
      return hs::WriteStatus::BadPath;
    }
    if (writing_) {
      return hs::WriteStatus::Busy;
    }
    if (const hs::WriteStatus g = pre(); g != hs::WriteStatus::Ok) {
      return g;
    }
    const std::string k = key(path);
    const std::size_t slash = k.rfind('/');
    if (slash != std::string::npos && dirs.count(k.substr(0, slash)) == 0) {
      return hs::WriteStatus::NotFound;
    }
    if (files.count(k) != 0 || dirs.count(k) != 0) {
      return hs::WriteStatus::Exists;
    }
    files[k] = Bytes();
    log.push_back(std::string("marker ") + path);
    return hs::WriteStatus::Ok;
  }
  hs::WriteStatus write(const void* src, std::size_t n) noexcept override {
    if (!writing_) {
      return hs::WriteStatus::NotFound;
    }
    if (dead || ops >= cut_at) {  // the cut lands in this chunk: half of it gets to the card
      dead = true;
      ++ops;
      Bytes& f = files[write_path_];
      f.insert(f.end(), static_cast<const std::uint8_t*>(src), static_cast<const std::uint8_t*>(src) + n / 2);
      return hs::WriteStatus::Io;
    }
    const hs::WriteStatus g = pre();
    if (g != hs::WriteStatus::Ok) {
      return g;
    }
    if (n > space_left) {
      return hs::WriteStatus::NoSpace;
    }
    space_left -= n;
    Bytes& f = files[write_path_];
    f.insert(f.end(), static_cast<const std::uint8_t*>(src), static_cast<const std::uint8_t*>(src) + n);
    log.push_back("write " + std::to_string(n));
    return hs::WriteStatus::Ok;
  }
  hs::WriteStatus finish() noexcept override {
    if (!writing_) {
      return hs::WriteStatus::NotFound;
    }
    writing_ = false;
    if (const hs::WriteStatus g = pre(); g != hs::WriteStatus::Ok) {
      return g;
    }
    log.push_back("finish");
    return hs::WriteStatus::Ok;
  }
  void abandon() noexcept override { writing_ = false; }
  bool writing() const noexcept override { return writing_; }
  hs::WriteStatus rename(const char* from, const char* to) noexcept override {
    if (!guard::may_rename(from, to)) {
      ++violations;
      return hs::WriteStatus::BadPath;
    }
    if (const hs::WriteStatus g = pre(); g != hs::WriteStatus::Ok) {
      return g;
    }
    const std::string kf = key(from);
    const std::string kt = key(to);
    const auto it = files.find(kf);
    const bool is_dir = it == files.end() && dirs.count(kf) != 0;  // OS-FILES part B: a folder moves with everything under it, as one step (FatFs: two directory updates; this fake never stops between them)
    if (it == files.end() && !is_dir) {
      return hs::WriteStatus::NotFound;
    }
    if (files.count(kt) != 0 || dirs.count(kt) != 0) {
      return hs::WriteStatus::Exists;
    }
    const std::size_t slash = kt.rfind('/');
    if (is_dir && slash != std::string::npos && dirs.count(kt.substr(0, slash)) == 0) {
      return hs::WriteStatus::NotFound;  // (FatFs: FR_NO_PATH)
    }
    if (is_dir) {
      const std::string prefix = kf + "/";
      std::set<std::string> moved_dirs;
      std::map<std::string, Bytes> moved_files;
      for (auto d = dirs.begin(); d != dirs.end();) {
        if (*d == kf || d->rfind(prefix, 0) == 0) {
          moved_dirs.insert(kt + d->substr(kf.size()));
          d = dirs.erase(d);
        } else {
          ++d;
        }
      }
      for (auto f = files.begin(); f != files.end();) {
        if (f->first.rfind(prefix, 0) == 0) {
          moved_files[kt + f->first.substr(kf.size())] = f->second;
          f = files.erase(f);
        } else {
          ++f;
        }
      }
      dirs.insert(moved_dirs.begin(), moved_dirs.end());
      files.insert(moved_files.begin(), moved_files.end());
    } else {
      files[kt] = it->second;
      files.erase(kf);
    }
    log.push_back(std::string("rename ") + from + ">" + to);
    if (++renames_ == swap_after_renames) {
      ++id;
    }
    return hs::WriteStatus::Ok;
  }
  hs::WriteStatus remove(const char* path) noexcept override {
    if (!guard::may_remove(path)) {
      ++violations;
      return hs::WriteStatus::BadPath;
    }
    if (writing_) {
      return hs::WriteStatus::Busy;
    }
    if (const hs::WriteStatus g = pre(); g != hs::WriteStatus::Ok) {
      return g;
    }
    if (files.erase(key(path)) != 0) {
      log.push_back(std::string("remove ") + path);
    }
    return hs::WriteStatus::Ok;
  }

 private:
  // The checks every mutating call starts with: the gate, the power cut, the one-off fault. Ok means go on (and counts the call).
  hs::WriteStatus pre() {
    if (gate_status != hs::WriteStatus::Ok) {
      return gate_status;
    }
    if (pinned_ && id != pin_id_) {
      return hs::WriteStatus::NoCard;
    }
    if (dead || ops >= cut_at) {
      dead = true;
      ++ops;
      return hs::WriteStatus::Io;
    }
    const std::size_t index = ops++;
    return index == fail_at ? fail_with : hs::WriteStatus::Ok;
  }

  bool pinned_ = false;
  std::uint32_t pin_id_ = 0;
  std::size_t renames_ = 0;
  std::vector<std::pair<std::string, bool>> listing_;
  std::size_t list_pos_ = 0;
  std::string write_path_;
  std::string read_path_;
  std::size_t read_pos_ = 0;
  bool writing_ = false;
};

}  // namespace o3os::test
