// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The firmware update port with no flash behind it (OS-FWCONFIG): hal::firmware::FirmwareSink for the host tests and the simulator. A fixture list of files, a script
// for what each check finds, and an install() that only records that it was called. No file is read, nothing is written, no pin exists: the dialogs of the App have
// goldens and tests without any access to a card's Firmware/ folder, and the sink can say every outcome the real one can (a long check, a fault at a byte, a refusal, an
// install that the bootloader never takes).
//
// The script of a check, by file name (Script, set with script()); a file with none uses the default: kDefaultSlices Running polls, then Checked (image 0x3F2A0 bytes, CRC
// 0x5EA1B0C4). Each poll_check() is one slice: it advances `done` by total / slices. The install job ends as install_end says: Failed, DidNotStart, or (None) it stays
// Running for good, as on the unit, where the chip has reset.
//
// Not for the target: standard library allowed here (platform/host); nothing below the app layer includes this header.

#include <o3os/hal/firmware.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace o3os::platform::host {

class FakeFirmwareSink final : public hal::firmware::FirmwareSink {
 public:
  using Refusal = hal::firmware::Refusal;
  using State = hal::firmware::CheckStatus::State;

  static constexpr unsigned kDefaultSlices = 3;

  struct Script {
    unsigned slices = kDefaultSlices;   // Running polls before the verdict
    State verdict = State::Checked;     // Checked or Failed
    std::uint32_t image = 0x3F2A0u;     // Checked
    std::uint32_t crc = 0x5EA1B0C4u;    // Checked
    std::uint32_t offset = 0;           // Failed: the byte of the fault
    bool offset_known = true;           // Failed: false for a fault of the whole image (the real sink says so for not padded)
    const char* fault = "bad vector table";  // Failed: a static text
    State install_end = State::Idle;    // after install(): Failed or DidNotStart after `install_slices` polls; Idle: Running for good
    unsigned install_slices = kDefaultSlices;
    const char* install_fault = "i/o error";
  };

  // ---- the fixture
  bool present = true;                  // available()
  Refusal list_refusal = Refusal::None;
  Refusal check_refusal = Refusal::None;
  Refusal install_refusal = Refusal::None;
  void add(const std::string& name, std::uint32_t size) { files_.push_back(Item{name, size}); }
  void clear_files() { files_.clear(); }
  void script(const std::string& name, const Script& s) { scripts_.push_back(Named{name, s}); }
  // The check that waits for install() has lapsed (the 30 s, or the card went to a computer): the next poll_check() says Failed.
  void lapse(const char* fault = "check expired, check again") {
    if (status_.state == State::Checked) {
      status_ = hal::firmware::CheckStatus{};
      status_.state = State::Failed;
      status_.fault = fault;
      checked_ = false;
    }
  }

  // ---- what was asked
  std::vector<std::string> calls;       // "list_begin", "begin_check NAME", "cancel", "install", ...
  unsigned installs = 0;                // install() calls that were taken
  std::string last_name;                // the file of the last begin_check
  unsigned count(const std::string& call) const {
    unsigned n = 0;
    for (const std::string& c : calls) {
      n += c == call ? 1u : 0u;
    }
    return n;
  }

  // ---- FirmwareSink
  bool available() const noexcept override { return present; }

  Refusal list_begin() noexcept override {
    calls.push_back("list_begin");
    at_ = 0;
    return list_refusal;
  }
  bool list_next(hal::firmware::File& out) noexcept override {
    if (at_ >= files_.size()) {
      return false;
    }
    const Item& it = files_[at_];
    std::memset(out.name, 0, sizeof out.name);
    std::strncpy(out.name, it.name.c_str(), sizeof out.name - 1);
    out.size = it.size;
    out.reset_target = at_ == 0;
    ++at_;
    return true;
  }
  void list_end() noexcept override { calls.push_back("list_end"); }

  Refusal begin_check(const char* name) noexcept override {
    calls.push_back(std::string("begin_check ") + name);
    if (status_.state == State::Running) {  // a check or an install is running: like the service, Busy, and nothing is disturbed
      return Refusal::Busy;
    }
    if (check_refusal != Refusal::None) {
      return check_refusal;
    }
    const Item* found = nullptr;
    for (const Item& it : files_) {
      if (it.name == name) {
        found = &it;
        break;
      }
    }
    if (found == nullptr) {
      return Refusal::NotFound;
    }
    last_name = name;
    script_ = Script{};
    for (const Named& n : scripts_) {
      if (n.name == name) {
        script_ = n.script;
      }
    }
    total_ = found->size;
    polls_ = 0;
    installing_ = false;
    checked_ = false;
    status_ = hal::firmware::CheckStatus{};
    status_.state = State::Running;
    status_.total = total_;
    return Refusal::None;
  }

  hal::firmware::CheckStatus poll_check() noexcept override {
    if (status_.state == State::Running) {
      ++polls_;
      if (!installing_ && polls_ >= script_.slices) {
        finish_check();
      } else if (installing_ && script_.install_end != State::Idle && polls_ >= script_.install_slices) {
        status_.state = script_.install_end;
        status_.fault = script_.install_end == State::Failed ? script_.install_fault : nullptr;
      } else {
        const unsigned of = installing_ ? script_.install_slices : script_.slices;
        status_.done = static_cast<std::uint32_t>((static_cast<std::uint64_t>(total_) * (polls_ < of ? polls_ : of)) / (of == 0 ? 1 : of));
      }
    }
    return status_;
  }

  void cancel() noexcept override {
    calls.push_back("cancel");
    if (!(installing_ && status_.state == State::Running)) {  // an install that has asked the bootloader cannot be taken back
      status_ = hal::firmware::CheckStatus{};
      checked_ = false;
    }
  }

  Refusal install() noexcept override {
    calls.push_back("install");
    if (status_.state == State::Running) {  // a re-check or the wait for the bootloader runs: Busy, and the status stays the job's (Running, installing)
      return Refusal::Busy;
    }
    if (install_refusal != Refusal::None) {
      return install_refusal;
    }
    if (!checked_ || status_.state != State::Checked) {
      return Refusal::NotChecked;
    }
    ++installs;
    installing_ = true;
    checked_ = false;
    polls_ = 0;
    status_ = hal::firmware::CheckStatus{};
    status_.state = State::Running;
    status_.installing = true;
    status_.total = total_;
    return Refusal::None;
  }

 private:
  struct Item {
    std::string name;
    std::uint32_t size;
  };
  struct Named {
    std::string name;
    Script script;
  };

  void finish_check() {
    status_ = hal::firmware::CheckStatus{};
    status_.state = script_.verdict;
    if (script_.verdict == State::Checked) {
      status_.image = script_.image;
      status_.crc = script_.crc;
      checked_ = true;
    } else {
      status_.offset = script_.offset;
      status_.offset_known = script_.offset_known;
      status_.fault = script_.fault;
    }
  }

  std::vector<Item> files_;
  std::vector<Named> scripts_;
  std::size_t at_ = 0;
  Script script_;
  hal::firmware::CheckStatus status_;
  std::uint32_t total_ = 0;
  unsigned polls_ = 0;
  bool installing_ = false;
  bool checked_ = false;
};

}  // namespace o3os::platform::host
