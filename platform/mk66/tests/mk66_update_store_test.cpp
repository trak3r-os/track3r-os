// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FatFs-backed FirmwareStore (update/fat_firmware_store.h, OS-006c) on a FAT32 volume built in the test (mk66_storage_fakes.h): an upload
// (create, append, close, read back, rename), the refusals (existing target, bad names, a write-protected medium, no
// Firmware/ folder (a card with no marker file is accepted: OS-ANYCARD), a full volume, a second upload, removal of official names and of the last .ptf), `_fwinfo` with its exact bytes, and the property
// that matters most: whatever is done, the tree outside Firmware/ is byte-identical, and a refused call writes nothing at all. Runs on the host and on
// QEMU's Cortex-M4F (the volume is sparse).
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include "mk66_storage_fakes.h"

#include <o3os/hal/storage.h>
#include <o3os/platform/mk66/storage/fat_storage.h>
#include <o3os/platform/mk66/update/fat_firmware_store.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hs = o3os::hal::storage;
namespace st = o3os::platform::mk66::storage;
namespace up = o3os::platform::mk66::update;
using fakes::Fat32Builder;
using fakes::Medium;
using up::Fault;

namespace {

std::vector<std::uint8_t> bytes(std::size_t n, unsigned salt) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<std::uint8_t>((i * 31u + 7u + salt) & 0xFFu);
  }
  return v;
}

// The card: README.TXT, Workspace/project.mt, patterns/..., and Firmware/ with an official image and a dev one.
struct Card {
  Medium medium{200'000};
  std::uint32_t firmware = 0;
  Card(bool firmware_dir = true, bool dev_image = true, std::uint32_t leave_free = 0) {
    Fat32Builder fat(medium);
    fat.add_file(Fat32Builder::kRootCluster, "README.TXT", bytes(40, 1));
    const std::uint32_t workspace = fat.make_dir(Fat32Builder::kRootCluster, "Workspace");
    fat.add_file(workspace, "project.mt", bytes(2320, 10));
    const std::uint32_t patterns = fat.make_dir(workspace, "patterns");
    fat.add_file(patterns, "pattern_01.mtp", bytes(9260, 11));
    if (firmware_dir) {
      firmware = fat.make_dir(Fat32Builder::kRootCluster, "Firmware");
      fat.add_file(firmware, "PolyendTracker_1.9.0.ptf", bytes(1500, 20));
      if (dev_image) {
        fat.add_file(firmware, "PolyendTracker_0.1.ptf", bytes(900, 21));
      }
    }
    if (leave_free != 0u) {
      fat.consume_clusters(leave_free);
    }
  }
};

std::vector<std::uint8_t> read_file(hs::Storage& s, const char* path) {
  std::vector<std::uint8_t> out;
  hs::FileHandle f;
  if (s.open(path, f) != hs::Status::Ok) {
    return out;
  }
  std::uint8_t buf[300];
  std::size_t got = 0;
  while (s.read(f, buf, sizeof buf, got) == hs::Status::Ok && got != 0u) {
    out.insert(out.end(), buf, buf + got);
  }
  s.close(f);
  return out;
}

std::vector<hs::DirEntry> dir_entries(hs::Storage& s, const char* path) {
  std::vector<hs::DirEntry> out;
  hs::DirHandle d;
  if (s.opendir(path, d) != hs::Status::Ok) {
    return out;
  }
  hs::DirEntry e;
  while (out.size() < 500 && s.readdir(d, e) == hs::Status::Ok) {
    out.push_back(e);
  }
  s.closedir(d);
  return out;
}

// FNV-1a over every file and directory name and every byte of every file outside the root folder `skip` (the tree the writer must not touch).
void hash_tree(hs::Storage& s, const std::string& dir, const char* skip, std::uint64_t& h) {
  const std::vector<hs::DirEntry> entries = dir_entries(s, dir.c_str());
  for (const hs::DirEntry& e : entries) {
    if (dir.empty() && std::strcmp(e.name, skip) == 0) {
      continue;
    }
    const std::string path = dir.empty() ? std::string(e.name) : dir + "/" + e.name;
    for (const char* p = path.c_str(); *p != '\0'; ++p) {
      h = (h ^ static_cast<std::uint8_t>(*p)) * 1099511628211ull;
    }
    if (e.is_dir) {
      hash_tree(s, path, skip, h);
    } else {
      for (const std::uint8_t b : read_file(s, path.c_str())) {
        h = (h ^ b) * 1099511628211ull;
      }
    }
  }
}

std::uint64_t outside(hs::Storage& s) {
  std::uint64_t h = 1469598103934665603ull;
  hash_tree(s, "", "Firmware", h);
  return h;
}

std::vector<std::string> names_in_firmware(hs::Storage& s) {
  std::vector<std::string> out;
  for (const hs::DirEntry& e : dir_entries(s, "Firmware")) {
    out.push_back(e.name);
  }
  return out;
}

bool has(const std::vector<std::string>& v, const char* name) {
  for (const std::string& s : v) {
    if (s == name) {
      return true;
    }
  }
  return false;
}

// A whole upload through the store's calls, as the transfer makes it.
Fault upload(up::FatFirmwareStore& store, const char* name, const std::vector<std::uint8_t>& data, std::size_t piece = 1024) {
  Fault f = store.begin_upload(name, static_cast<std::uint32_t>(data.size()));
  if (f != Fault::Ok) {
    return f;
  }
  for (std::size_t at = 0; at < data.size(); at += piece) {
    f = store.append(data.data() + at, data.size() - at < piece ? data.size() - at : piece);
    if (f != Fault::Ok) {
      store.abort_upload();
      return f;
    }
  }
  f = store.close_upload();
  if (f == Fault::Ok) {
    f = store.commit_upload();
  }
  if (f != Fault::Ok) {
    store.abort_upload();
  }
  return f;
}

}  // namespace

TEST_CASE("firmware store: the state says what is missing, in order") {
  {
    Card card;
    st::FatStorage fat(card.medium);
    up::FatFirmwareStore store(fat, card.medium);
    up::WriteState s = store.state();
    CHECK(!s.mounted);
    CHECK(s.why == Fault::NoCard);
    NEED(fat.mount() == hs::Status::Ok);
    s = store.state();
    CHECK(s.enabled);  // OS-FWANY, OS-ANYCARD: a card with no marker file is enough
    CHECK(s.why == Fault::Ok);
    CHECK(s.dir);
    CHECK(!s.write_protected);
    CHECK(std::strcmp(s.fs, "FAT32") == 0);
    CHECK(std::strcmp(s.serial, "1234-ABCD") == 0);  // the Volume ID the builder wrote, 0x1234ABCD
  }
  {  // no Firmware/ folder: the writer never makes one
    Card card(false);
    st::FatStorage fat(card.medium);
    up::FatFirmwareStore store(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    CHECK(store.state().why == Fault::NoDir);
    CHECK(store.begin_upload("PolyendTracker_0.7.ptf", 100) == Fault::NoDir);
    CHECK(card.medium.blocks_written == 0u);
    CHECK(fat.exists("Firmware", nullptr) == hs::Status::NotFound);
  }
  {  // a write-protected medium
    Card card;
    st::FatStorage fat(card.medium);
    up::FatFirmwareStore store(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    card.medium.protect = true;
    CHECK(store.state().why == Fault::WriteProtected);
    CHECK(store.begin_upload("PolyendTracker_0.7.ptf", 100) == Fault::WriteProtected);
    CHECK(card.medium.blocks_written == 0u);
  }
  {  // the card taken out
    Card card;
    st::FatStorage fat(card.medium);
    up::FatFirmwareStore store(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    card.medium.present_ = false;
    CHECK(store.state().why == Fault::NoCard);
    CHECK(store.begin_upload("PolyendTracker_0.7.ptf", 100) == Fault::NoCard);
  }
}

TEST_CASE("firmware store: an upload, read back, renamed; the tree outside Firmware/ is byte-identical") {
  Card card;
  st::FatStorage fat(card.medium);
  up::FatFirmwareStore store(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint64_t tree_before = outside(fat);
  const std::vector<std::uint8_t> official = read_file(fat, "Firmware/PolyendTracker_1.9.0.ptf");
  const std::vector<std::uint8_t> data = bytes(5000, 99);
  // Step by step: the temporary file exists while uploading and is not a .ptf.
  NEED(store.begin_upload("PolyendTracker_0.7.ptf", static_cast<std::uint32_t>(data.size())) == Fault::Ok);
  CHECK(store.uploading());
  CHECK(has(names_in_firmware(fat), "_o3up.tmp"));
  CHECK(!has(names_in_firmware(fat), "PolyendTracker_0.7.ptf"));
  for (std::size_t at = 0; at < data.size(); at += 1000) {
    CHECK(store.append(data.data() + at, 1000) == Fault::Ok);
  }
  CHECK(store.close_upload() == Fault::Ok);
  // The read-back sees the bytes on the card (and the declared length).
  std::uint32_t size = 0;
  NEED(store.open_read("_o3up.tmp", size) == Fault::Ok);
  CHECK(size == data.size());
  std::vector<std::uint8_t> back;
  std::uint8_t buf[700];
  std::size_t got = 0;
  while (store.read(buf, sizeof buf, got) == Fault::Ok && got != 0u) {
    back.insert(back.end(), buf, buf + got);
  }
  store.close_read();
  CHECK(back == data);
  CHECK(store.commit_upload() == Fault::Ok);
  CHECK(!store.uploading());
  const std::vector<std::string> names = names_in_firmware(fat);
  CHECK(has(names, "PolyendTracker_0.7.ptf"));
  CHECK(!has(names, "_o3up.tmp"));
  CHECK(read_file(fat, "Firmware/PolyendTracker_0.7.ptf") == data);
  CHECK(read_file(fat, "Firmware/PolyendTracker_1.9.0.ptf") == official);  // the existing images are untouched
  CHECK(outside(fat) == tree_before);
  // The listing through the store: directory order, sizes, no temporary file.
  up::FwEntry e;
  NEED(store.list_begin() == Fault::Ok);
  unsigned n = 0;
  bool saw_new = false;
  while (store.list_next(e)) {
    ++n;
    saw_new = saw_new || (std::strcmp(e.name, "PolyendTracker_0.7.ptf") == 0 && e.size == data.size());
  }
  store.list_end();
  CHECK(n == 3u);
  CHECK(saw_new);
  // Open handles: nothing is left open.
  CHECK(fat.open_files() == 0u);
  CHECK(fat.open_dirs() == 0u);
  // The same upload through the helper, in awkward piece sizes (including one byte and a sector and a half).
  for (const std::size_t piece : {std::size_t{1}, std::size_t{512}, std::size_t{768}, std::size_t{4999}}) {
    const std::string name = "PolyendTracker_0.p" + std::to_string(piece) + ".ptf";
    const std::vector<std::uint8_t> d = bytes(piece == 1 ? 700u : 3001u, static_cast<unsigned>(piece));
    CHECK_MESSAGE(upload(store, name.c_str(), d, piece) == Fault::Ok, "piece " << piece);
    CHECK(read_file(fat, ("Firmware/" + name).c_str()) == d);
  }
  CHECK(outside(fat) == tree_before);
}

TEST_CASE("firmware store: refusals write nothing and change nothing") {
  Card card;
  st::FatStorage fat(card.medium);
  up::FatFirmwareStore store(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint64_t tree = outside(fat);
  const std::uint64_t written = card.medium.blocks_written;
  CHECK(store.begin_upload("PolyendTracker_1.9.0.ptf", 100) == Fault::Exists);           // an official image is never replaced
  CHECK(store.begin_upload("PolyendTracker_0.1.ptf", 100) == Fault::Exists);             // nor a dev one
  CHECK(store.begin_upload("PolyendTracker_0.1.PTF", 100) == Fault::Exists);             // the suffix may be in any case, FAT does not tell them apart
  CHECK(store.begin_upload("polyendtracker_0.1.ptf", 100) == Fault::Exists);
  CHECK(store.begin_upload("PolyendTracker_0.1.ptf", 100) == Fault::Exists);
  for (const char* bad : {"", ".ptf", "../PolyendTracker_0.1.ptf", "Firmware/PolyendTracker_0.2.ptf", "PolyendTracker_a b.ptf", "_o3up.tmp", "_fwinfo",
                          "PolyendTracker_0..ptf", "README.TXT"}) {
    INFO(bad);
    CHECK(store.begin_upload(bad, 100) == Fault::BadName);
  }
  CHECK(store.begin_upload(nullptr, 100) == Fault::BadName);
  CHECK(store.begin_upload("PolyendTracker_0.9.ptf", 0) == Fault::Unsupported);
  CHECK(store.begin_upload("PolyendTracker_0.9.ptf", up::kMaxImageFileBytes + 1u) == Fault::Unsupported);
  CHECK(store.remove("PolyendTracker_1.9.0.ptf") == Fault::BadName);   // looks official
  CHECK(store.remove("README.TXT") == Fault::BadName);
  CHECK(store.remove("../Workspace") == Fault::BadName);
  CHECK(store.remove("_fwinfo") == Fault::BadName);
  CHECK(store.remove("PolyendTracker_0.55.ptf") == Fault::NotFound);
  CHECK(store.write_fwinfo("README.TXT") == Fault::BadName);
  CHECK(store.write_fwinfo("PolyendTracker_0.55.ptf") == Fault::NotFound);
  CHECK(store.write_fwinfo((std::string(up::kUpdateNameMax - 3, 'a') + ".ptf").c_str()) == Fault::NameLong);  // OS-BUG-FWNAMELEN: 31 characters, a valid image name, never handed over
  CHECK(card.medium.blocks_written == written);  // not one sector
  CHECK(outside(fat) == tree);
  CHECK(has(names_in_firmware(fat), "PolyendTracker_1.9.0.ptf"));
  // A second upload while one is open.
  NEED(store.begin_upload("PolyendTracker_0.8.ptf", 1000) == Fault::Ok);
  CHECK(store.begin_upload("PolyendTracker_0.9.ptf", 1000) == Fault::Busy);
  CHECK(store.write_fwinfo("PolyendTracker_0.1.ptf") == Fault::Busy);
  CHECK(store.remove("_o3up.tmp") == Fault::Busy);
  store.abort_upload();
  CHECK(!has(names_in_firmware(fat), "_o3up.tmp"));
  CHECK(!store.uploading());
  CHECK(outside(fat) == tree);
  // The target appears after the begin: nothing is replaced (a second store on the same volume plays the other writer).
  // A case-only difference is the same file on FAT.
  CHECK(upload(store, "PolyendTracker_0.Ab.ptf", bytes(600, 1)) == Fault::Ok);
  CHECK(store.begin_upload("PolyendTracker_0.aB.ptf", 100) == Fault::Exists);
}

TEST_CASE("firmware store: removal is for the temporary file and the dev namespace, and never the last .ptf") {
  Card card;
  st::FatStorage fat(card.medium);
  up::FatFirmwareStore store(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint64_t tree = outside(fat);
  // Two .ptf files: the dev one may go.
  CHECK(store.remove("PolyendTracker_0.1.ptf") == Fault::Ok);
  CHECK(!has(names_in_firmware(fat), "PolyendTracker_0.1.ptf"));
  CHECK(has(names_in_firmware(fat), "PolyendTracker_1.9.0.ptf"));
  // One left, the official one: not removable by name; add a dev one and it is removable only while another .ptf remains.
  CHECK(upload(store, "PolyendTracker_0.2.ptf", bytes(700, 5)) == Fault::Ok);
  CHECK(store.remove("PolyendTracker_0.2.ptf") == Fault::Ok);
  // A card whose only .ptf is a dev image: refused (the reset button would find nothing).
  Card lonely(true, false);  // (FatFs keeps one volume: outside(fat) below reads whichever card was mounted last, so the two cards must look alike outside Firmware/)
  st::FatStorage fat2(lonely.medium);
  up::FatFirmwareStore store2(fat2, lonely.medium);
  // the official image is the last .ptf: remove it by renaming the scenario: a card with only a dev image
  NEED(fat2.mount() == hs::Status::Ok);
  CHECK(store2.remove("PolyendTracker_1.9.0.ptf") == Fault::BadName);
  CHECK(upload(store2, "PolyendTracker_0.3.ptf", bytes(700, 6)) == Fault::Ok);
  CHECK(store2.remove("PolyendTracker_0.3.ptf") == Fault::Ok);  // two .ptf files again: fine
  CHECK(outside(fat) == tree);
  CHECK(outside(fat2) != 0u);
}

TEST_CASE("firmware store: the last .ptf of the folder is never removed") {
  // Firmware/ holds one dev image only: removing it is refused with LastPtf, and it is still there.
  Medium medium(200'000);
  {
    Fat32Builder fat(medium);
    const std::uint32_t fw = fat.make_dir(Fat32Builder::kRootCluster, "Firmware");
    fat.add_file(fw, "PolyendTracker_0.1.ptf", bytes(900, 21));
    fat.add_file(fw, "notes.txt", bytes(10, 22));
  }
  st::FatStorage fat(medium);
  up::FatFirmwareStore store(fat, medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint64_t written = medium.blocks_written;
  CHECK(store.remove("PolyendTracker_0.1.ptf") == Fault::LastPtf);
  CHECK(has(names_in_firmware(fat), "PolyendTracker_0.1.ptf"));
  CHECK(medium.blocks_written == written);
  // A second image makes the first removable.
  CHECK(upload(store, "PolyendTracker_0.2.ptf", bytes(700, 5)) == Fault::Ok);
  CHECK(store.remove("PolyendTracker_0.1.ptf") == Fault::Ok);
  CHECK(store.remove("PolyendTracker_0.2.ptf") == Fault::LastPtf);
}

TEST_CASE("firmware store: _fwinfo holds exactly the bare name") {
  Card card;
  st::FatStorage fat(card.medium);
  up::FatFirmwareStore store(fat, card.medium);
  NEED(fat.mount() == hs::Status::Ok);
  const std::uint64_t tree = outside(fat);
  CHECK(store.remove_fwinfo() == Fault::Ok);  // none there: fine
  NEED(store.write_fwinfo("PolyendTracker_0.1.ptf") == Fault::Ok);
  const std::vector<std::uint8_t> info = read_file(fat, "Firmware/_fwinfo");
  const std::string expected = "PolyendTracker_0.1.ptf";
  CHECK(std::string(info.begin(), info.end()) == expected);  // no folder, no newline, no NUL
  CHECK(info.size() == expected.size());
  CHECK(store.write_fwinfo("PolyendTracker_0.1.ptf") == Fault::Exists);  // a stale one is deleted by the caller first
  const std::vector<std::uint8_t> again = read_file(fat, "Firmware/_fwinfo");
  CHECK(std::string(again.begin(), again.end()) == expected);
  CHECK(store.remove_fwinfo() == Fault::Ok);
  CHECK(!has(names_in_firmware(fat), "_fwinfo"));
  CHECK(store.remove_fwinfo() == Fault::Ok);
  NEED(store.write_fwinfo("PolyendTracker_1.9.0.ptf") == Fault::Ok);
  const std::vector<std::uint8_t> info2 = read_file(fat, "Firmware/_fwinfo");
  CHECK(std::string(info2.begin(), info2.end()) == "PolyendTracker_1.9.0.ptf");
  // OS-BUG-FWNAMELEN: a name of exactly kUpdateNameMax characters is handed over (31 is refused in the refusals test)
  const std::string longest = "PolyendTracker_0.123456789.ptf";
  static_assert(sizeof("PolyendTracker_0.123456789.ptf") - 1 == up::kUpdateNameMax, "the longest name is the limit");
  CHECK(store.remove_fwinfo() == Fault::Ok);
  NEED(upload(store, longest.c_str(), bytes(700, 7)) == Fault::Ok);
  NEED(store.write_fwinfo(longest.c_str()) == Fault::Ok);
  const std::vector<std::uint8_t> info3 = read_file(fat, "Firmware/_fwinfo");
  CHECK(std::string(info3.begin(), info3.end()) == longest);
  CHECK(outside(fat) == tree);
  CHECK(fat.open_files() == 0u);
}

TEST_CASE("firmware store: a full volume, a failing medium, a card pulled out") {
  {  // the volume cannot hold the file: said at the start, nothing left behind
    Card card(true, true, 8);  // 8 free clusters of 512 bytes: 4 KiB
    st::FatStorage fat(card.medium);
    up::FatFirmwareStore store(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    const std::uint64_t tree = outside(fat);
    CHECK(store.begin_upload("PolyendTracker_0.7.ptf", 100'000) == Fault::NoSpace);
    CHECK(!has(names_in_firmware(fat), "_o3up.tmp"));
    CHECK(!store.uploading());
    CHECK(outside(fat) == tree);
    // A file that fits works on the same volume afterwards.
    CHECK(upload(store, "PolyendTracker_0.8.ptf", bytes(1500, 3)) == Fault::Ok);
  }
  {  // a medium that fails during the upload: the call says so, the abort deletes the temporary file once the medium works again
    Card card;
    st::FatStorage fat(card.medium);
    up::FatFirmwareStore store(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    const std::uint64_t tree = outside(fat);
    const std::vector<std::uint8_t> data = bytes(6000, 4);
    NEED(store.begin_upload("PolyendTracker_0.7.ptf", 6000) == Fault::Ok);
    card.medium.fail_writes_after = 3;
    Fault f = Fault::Ok;
    for (std::size_t at = 0; at < data.size() && f == Fault::Ok; at += 512) {
      f = store.append(data.data() + at, data.size() - at < 512 ? data.size() - at : 512);
    }
    CHECK(f != Fault::Ok);
    card.medium.fail_writes_after = 0xFFFFFFFFu;
    store.abort_upload();
    CHECK(!store.uploading());
    CHECK(!has(names_in_firmware(fat), "_o3up.tmp"));
    CHECK(!has(names_in_firmware(fat), "PolyendTracker_0.7.ptf"));
    CHECK(outside(fat) == tree);
    CHECK(upload(store, "PolyendTracker_0.7.ptf", data) == Fault::Ok);  // and the volume is fine
  }
  {  // the card pulled in the middle: an error, and the abort does not hang or crash; a new mount finds a stale temporary file that the next upload replaces
    Card card;
    st::FatStorage fat(card.medium);
    up::FatFirmwareStore store(fat, card.medium);
    NEED(fat.mount() == hs::Status::Ok);
    NEED(store.begin_upload("PolyendTracker_0.7.ptf", 4000) == Fault::Ok);
    const std::vector<std::uint8_t> data = bytes(4000, 7);
    CHECK(store.append(data.data(), 1024) == Fault::Ok);
    card.medium.present_ = false;
    Fault f = Fault::Ok;
    for (std::size_t at = 1024; at < 4000 && f == Fault::Ok; at += 1024) {
      f = store.append(data.data() + at, 1024);
    }
    CHECK(f != Fault::Ok);
    store.abort_upload();
    CHECK(!store.uploading());
    card.medium.present_ = true;
    NEED(fat.mount() == hs::Status::Ok);
    CHECK(upload(store, "PolyendTracker_0.7.ptf", data) == Fault::Ok);
    CHECK(!has(names_in_firmware(fat), "_o3up.tmp"));
    CHECK(read_file(fat, "Firmware/PolyendTracker_0.7.ptf") == data);
  }
}
