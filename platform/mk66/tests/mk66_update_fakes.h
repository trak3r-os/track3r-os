// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test doubles of the firmware update path (OS-006c), header-only for the host and QEMU suites: an in-memory FirmwareStore that follows the real one's rules
// (the path guard, existing targets, the last .ptf, the temporary file's place in the directory order) and fails when it is told to, a FwHost that records what
// the image would have done to the outputs and the pin, and a rig that puts the real shell, the real console pump and a receive ring around an FwService, with a
// host that sends at the speed the test says and reads (or does not read) what the device printed.
#include "mk66_update_support.h"

#include <o3os/platform/mk66/update/crc32.h>
#include <o3os/platform/mk66/update/firmware_store.h>
#include <o3os/platform/mk66/update/fw_service.h>
#include <o3os/usb/cdc.h>
#include <o3os/usb/diag_pump.h>
#include <o3os/usb/diag_shell.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace upd_test {

namespace sh = o3os::usb::diag;

class FakeStore final : public up::FirmwareStore {
 public:
  FakeStore() {
    state_.mounted = true;
    state_.dir = true;
    state_.enabled = true;
    state_.why = up::Fault::Ok;
    std::strcpy(state_.fs, "FAT32");
    std::strcpy(state_.serial, "1234-ABCD");
    put_file("PolyendTracker_1.9.0.ptf", std::vector<std::uint8_t>(1500, 0xAB));
  }

  // ---- what the test sets and reads
  std::map<std::string, std::vector<std::uint8_t>> files;  // bare names in Firmware/, the temporary file and _fwinfo included
  std::vector<std::string> order;                          // directory order
  std::vector<std::string> calls;                          // "begin NAME", "append N", "close", "open_read X", "commit", "abort", "write_fwinfo NAME", ...
  up::Fault begin_fault = up::Fault::Ok;
  std::uint32_t space_left = 0x7FFFFFFFu;
  unsigned fail_append_after = 0xFFFFFFFFu;  // the Nth append (0: the next) fails with Io
  bool fail_close = false;
  up::Fault commit_fault = up::Fault::Ok;
  up::Fault remove_fault = up::Fault::Ok;  // remove() of an existing file is refused with this (nothing is removed)
  up::Fault fwinfo_fault = up::Fault::Ok;
  bool corrupt_readback = false;   // one byte of the temporary file reads back different
  std::size_t corrupt_at = 100;
  std::string corrupt_file;        // an image of this name reads back with one byte (at corrupt_at) different (a copy that fails its arming check)
  bool short_readback = false;     // the temporary file reads back one byte shorter
  unsigned appends_this_pass = 0;
  unsigned max_appends_per_pass = 0;
  std::uint32_t reported_size_delta = 0;  // open_read reports this many bytes too many (a file that changed)

  void put_file(const std::string& name, const std::vector<std::uint8_t>& data) {
    if (files.find(name) == files.end()) {
      order.push_back(name);
    }
    files[name] = data;
  }
  void set_state(bool enabled, up::Fault why) {
    state_.enabled = enabled;
    state_.why = why;
  }
  void set_serial(const char* s) { std::strcpy(state_.serial, s); }
  void new_pass() {
    max_appends_per_pass = appends_this_pass > max_appends_per_pass ? appends_this_pass : max_appends_per_pass;
    appends_this_pass = 0;
  }
  bool has(const std::string& name) const { return files.find(name) != files.end(); }
  std::string text_of(const std::string& name) const {
    const auto it = files.find(name);
    return it == files.end() ? std::string("<none>") : std::string(it->second.begin(), it->second.end());
  }

  // ---- FirmwareStore
  up::WriteState state() noexcept override { return state_; }
  up::Fault begin_upload(const char* name, std::uint32_t bytes) noexcept override {
    calls.push_back(std::string("begin ") + (name != nullptr ? name : "?"));
    if (uploading_) {
      return up::Fault::Busy;
    }
    if (!up::valid_image_name(name)) {
      return up::Fault::BadName;
    }
    if (bytes == 0u || bytes > up::kMaxImageFileBytes) {
      return up::Fault::Unsupported;
    }
    if (!state_.enabled) {
      return state_.why;
    }
    if (has(name)) {
      return up::Fault::Exists;
    }
    if (begin_fault != up::Fault::Ok) {
      return begin_fault;
    }
    if (bytes > space_left) {
      return up::Fault::NoSpace;
    }
    if (has(up::kTempName)) {
      erase(up::kTempName);
    }
    put_file(up::kTempName, {});
    target_ = name;
    uploading_ = true;
    return up::Fault::Ok;
  }
  up::Fault append(const std::uint8_t* data, std::size_t n) noexcept override {
    calls.push_back("append " + std::to_string(n));
    ++appends_this_pass;
    if (!uploading_) {
      return up::Fault::Io;
    }
    if (fail_append_after == 0u) {
      return up::Fault::Io;
    }
    if (fail_append_after != 0xFFFFFFFFu) {
      --fail_append_after;
    }
    files[up::kTempName].insert(files[up::kTempName].end(), data, data + n);
    return up::Fault::Ok;
  }
  up::Fault close_upload() noexcept override {
    calls.push_back("close");
    return fail_close ? up::Fault::Io : up::Fault::Ok;
  }
  up::Fault commit_upload() noexcept override {
    calls.push_back("commit");
    if (commit_fault != up::Fault::Ok) {
      return commit_fault;
    }
    if (has(target_)) {
      return up::Fault::Exists;
    }
    files[target_] = files[up::kTempName];
    for (std::string& o : order) {
      if (o == up::kTempName) {
        o = target_;  // the entry keeps its place in the directory
      }
    }
    files.erase(up::kTempName);
    uploading_ = false;
    return up::Fault::Ok;
  }
  void abort_upload() noexcept override {
    if (uploading_) {
      calls.push_back("abort");
      erase(up::kTempName);
    }
    uploading_ = false;
  }
  bool uploading() const noexcept override { return uploading_; }
  up::Fault open_read(const char* name, std::uint32_t& size) noexcept override {
    calls.push_back(std::string("open_read ") + name);
    if (!up::readable_name(name)) {
      return up::Fault::BadName;
    }
    const auto it = files.find(name);
    if (it == files.end()) {
      return up::Fault::NotFound;
    }
    read_copy_ = it->second;
    if (std::strcmp(name, up::kTempName) == 0) {
      if (corrupt_readback && corrupt_at < read_copy_.size()) {
        read_copy_[corrupt_at] = static_cast<std::uint8_t>(read_copy_[corrupt_at] ^ 0x01u);
      }
      if (short_readback && !read_copy_.empty()) {
        read_copy_.pop_back();
      }
    }
    if (!corrupt_file.empty() && corrupt_file == name && corrupt_at < read_copy_.size()) {
      read_copy_[corrupt_at] = static_cast<std::uint8_t>(read_copy_[corrupt_at] ^ 0x01u);
    }
    read_at_ = 0;
    reading_ = true;
    size = static_cast<std::uint32_t>(read_copy_.size()) + reported_size_delta;
    return up::Fault::Ok;
  }
  up::Fault read(std::uint8_t* dst, std::size_t max, std::size_t& got) noexcept override {
    if (!reading_) {
      return up::Fault::Io;
    }
    got = read_copy_.size() - read_at_ < max ? read_copy_.size() - read_at_ : max;
    std::memcpy(dst, read_copy_.data() + read_at_, got);
    read_at_ += got;
    return up::Fault::Ok;
  }
  void close_read() noexcept override { reading_ = false; }
  up::Fault remove(const char* name) noexcept override {
    calls.push_back(std::string("remove ") + name);
    if (!up::removable_name(name)) {
      return up::Fault::BadName;
    }
    if (!state_.enabled) {
      return state_.why;
    }
    if (!has(name)) {
      return up::Fault::NotFound;
    }
    if (remove_fault != up::Fault::Ok) {
      return remove_fault;
    }
    if (up::is_ptf_name(name)) {
      unsigned n = 0;
      for (const auto& kv : files) {
        n += up::is_ptf_name(kv.first.c_str()) ? 1u : 0u;
      }
      if (n < 2u) {
        return up::Fault::LastPtf;
      }
    }
    erase(name);
    return up::Fault::Ok;
  }
  up::Fault write_fwinfo(const char* name) noexcept override {
    calls.push_back(std::string("write_fwinfo ") + name);
    if (fwinfo_fault != up::Fault::Ok) {
      return fwinfo_fault;
    }
    if (!up::valid_image_name(name)) {
      return up::Fault::BadName;
    }
    if (has(up::kFwInfoName)) {
      return up::Fault::Exists;
    }
    if (!has(name)) {
      return up::Fault::NotFound;
    }
    put_file(up::kFwInfoName, std::vector<std::uint8_t>(name, name + std::strlen(name)));
    return up::Fault::Ok;
  }
  up::Fault remove_fwinfo() noexcept override {
    calls.push_back("remove_fwinfo");
    erase(up::kFwInfoName);
    return up::Fault::Ok;
  }
  up::Fault list_begin() noexcept override {
    list_at_ = 0;
    return up::Fault::Ok;
  }
  bool list_next(up::FwEntry& out) noexcept override {
    if (list_at_ >= order.size()) {
      return false;
    }
    const std::string& n = order[list_at_++];
    std::strcpy(out.name, n.c_str());
    out.size = static_cast<std::uint32_t>(files[n].size());
    return true;
  }
  void list_end() noexcept override {}

  bool has_call(const std::string& prefix) const {
    for (const std::string& c : calls) {
      if (c.compare(0, prefix.size(), prefix) == 0) {
        return true;
      }
    }
    return false;
  }
  unsigned count_calls(const std::string& prefix) const {
    unsigned n = 0;
    for (const std::string& c : calls) {
      n += c.compare(0, prefix.size(), prefix) == 0 ? 1u : 0u;
    }
    return n;
  }

 private:
  void erase(const std::string& name) {
    files.erase(name);
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == name) {
        order.erase(order.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
  }
  up::WriteState state_;
  bool uploading_ = false;
  std::string target_;
  std::vector<std::uint8_t> read_copy_;
  std::size_t read_at_ = 0;
  bool reading_ = false;
  std::size_t list_at_ = 0;
};

class FakeHost final : public up::FwHost {
 public:
  std::uint32_t now = 100000;
  std::uint32_t entropy_word = 0x13572468u;
  bool busy = false;
  bool on_host = false;           // the card is handed to a computer
  std::uint32_t sessions = 0;     // the hand-overs so far
  std::vector<std::string> calls;  // "quiesce", "drain 40", "pin low", "pin release"
  std::vector<std::string> logs;
  bool pin_low = false;
  std::size_t out_size_at_pin_low = 0;
  const std::string* out_ref = nullptr;  // the rig's output, to say how much had been printed when the pin went low

  std::uint32_t now_ms() noexcept override { return now; }
  std::uint32_t entropy() noexcept override { return entropy_word; }
  bool storage_busy() noexcept override { return busy; }
  bool card_on_host() noexcept override { return on_host; }
  std::uint32_t host_sessions() noexcept override { return sessions; }
  void quiesce() noexcept override { calls.push_back("quiesce"); }
  void drain_output(std::uint32_t max_ms) noexcept override { calls.push_back("drain " + std::to_string(max_ms)); }
  void bootloader_request(bool low) noexcept override {
    calls.push_back(low ? "pin low" : "pin release");
    pin_low = low;
    if (low && out_ref != nullptr) {
      out_size_at_pin_low = out_ref->size();
    }
  }
  void log(const char* text) noexcept override { logs.push_back(text); }
  bool called(const std::string& what) const {
    for (const std::string& c : calls) {
      if (c == what) {
        return true;
      }
    }
    return false;
  }
};

// The real shell and pump on a receive ring, a host that sends 64-byte packets when the ring has room (the interrupt NAKs it otherwise) and a transmit side
// whose room the test controls.
struct Rig {
  FakeStore store;
  FakeHost host;
  up::FwService svc{store, host};
  o3os::usb::CdcRxRing rx;
  std::string out;                 // everything the device printed (CR LF as sent)
  std::size_t unread = 0;          // bytes printed that the host has not read yet
  std::size_t room_cap = 3900;     // the transmit ring's room when nothing is unread
  bool host_reads = true;
  std::string pending;             // bytes the host wants to send
  std::size_t pending_at = 0;
  std::size_t packet = 64;
  std::uint32_t naks = 0;
  std::uint32_t steps = 0;
  sh::Shell shell;
  sh::ConsolePump pump;

  static Rig*& current() {
    static Rig* r = nullptr;
    return r;
  }
  static void cmd_fw(sh::Shell& s, const sh::Args& a) { current()->svc.run(s, a); }
  static void write(void* self, const char* d, std::size_t n) {
    Rig* r = static_cast<Rig*>(self);
    r->out.append(d, n);
    r->unread += n;
  }
  static void flush(void*) {}
  static std::size_t room(void* self) {
    Rig* r = static_cast<Rig*>(self);
    return r->unread >= r->room_cap ? 0u : r->room_cap - r->unread;
  }
  static const sh::Command* table() {
    static const sh::Command t[] = {sh::command("fw", "fw", &Rig::cmd_fw)};
    return t;
  }

  Rig()
      : shell(table(), 1, sh::Output(sh::Sink{&Rig::write, &Rig::flush, this, &Rig::room})),
        pump(shell, rx) {
    current() = this;
    host.out_ref = &out;
  }
  ~Rig() {
    if (current() == this) {
      current() = nullptr;
    }
  }

  void send(const std::string& bytes) { pending += bytes; }
  bool all_sent() const { return pending_at >= pending.size(); }

  // One millisecond: the host's packet (if the ring has room), the main loop's pass, the host's read of what was printed.
  void step(std::uint32_t ms = 1) {
    current() = this;  // several rigs may live in one test: the command table reaches the one that is being stepped
    ++steps;
    store.new_pass();
    if (pending_at < pending.size()) {
      if (rx.size() < o3os::usb::kCdcRxHoldChunks) {
        o3os::usb::ByteChunk c{};
        const std::size_t n = pending.size() - pending_at < packet ? pending.size() - pending_at : packet;
        c.len = static_cast<std::uint32_t>(n);
        std::memcpy(c.data, pending.data() + pending_at, n);
        rx.write(&c, 1);
        pending_at += n;
      } else {
        ++naks;
      }
    }
    pump.service();
    shell.tick();
    host.now += ms;
    if (host_reads) {
      unread = 0;
    }
  }
  bool idle() const { return all_sent() && rx.size() == 0u && pump.held_bytes() == 0u && !shell.streaming() && !shell.raw_active(); }
  void run(unsigned max_steps = 200000) {
    unsigned quiet = 0;
    for (unsigned i = 0; i < max_steps && quiet < 3u; ++i) {
      step();
      quiet = idle() ? quiet + 1u : 0u;
    }
  }
  std::string text() const {  // the output without CRs
    std::string t;
    for (const char ch : out) {
      if (ch != '\r') {
        t += ch;
      }
    }
    return t;
  }
  bool said(const char* needle) const { return text().find(needle) != std::string::npos; }
  std::size_t count(const char* needle) const {
    std::size_t n = 0;
    const std::string t = text();
    for (std::size_t at = t.find(needle); at != std::string::npos; at = t.find(needle, at + 1)) {
      ++n;
    }
    return n;
  }
  void clear() {
    out.clear();
    unread = 0;
  }

  // Types a line and runs until the device is quiet again.
  void command(const std::string& line) {
    send(line + "\r");
    run();
  }
};

inline std::uint32_t file_crc(const std::string& text) {
  up::Crc32 c;
  c.add(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  return c.value();
}

inline std::string hex8s(std::uint32_t v) {
  char b[9];
  std::snprintf(b, sizeof b, "%08X", static_cast<unsigned>(v));
  return b;
}

}  // namespace upd_test
