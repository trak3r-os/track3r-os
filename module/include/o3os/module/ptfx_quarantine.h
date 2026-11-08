// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S2c (docs/specs/OS-CEPTFX.md A7): the `.ptfx` plugins quarantined for this boot. Up to 16 plugin ids (`0x8000 |` the 15-bit FNV-1a of
// "ptfx:" + the file name, ptfx_plugin_id()); it is a set of its own, apart from ModuleSet's quarantine of `.tmod` stems, so a plugin and a module
// of the same name never quarantine each other. It lasts the boot (only a reset empties it). The first 16 stay: a further plugin is refused (its
// id is not kept) and counted in refused(), which `diag ptfx quarantine` shows as the full flag.
//
// Who puts an id in: the boot, from the crash record's module word (a plugin id, or a .tmod's: the two share the 0x8000 bit and are told apart
// only by the catalogue, which lists the plugins' ids; an id no listed file has is inert), and the run job (S3) when the call gate answers
// Timeout, Fault or StackOverflow. Header only, no allocation; main loop only (a single writer, never touched from an ISR).
#pragma once

#include <cstdint>

namespace o3os::module {

class PtfxQuarantine {
 public:
  static constexpr unsigned kMax = 16;  // A7

  // Why an id is in: the previous run died while it ran (the crash record), or the gate's answer to a call of this run.
  enum class Why : std::uint8_t { Crash, Timeout, Fault, StackOverflow };
  enum class Add : std::uint8_t { Added, Already, Full, BadId };

  // Adds `id` (a plugin id has bit 15 set; anything else is BadId and is not kept). A duplicate keeps its first reason. When 16 are in, a new
  // id is refused: Full, counted.
  Add quarantine(std::uint16_t id, Why why) noexcept {
    if ((id & 0x8000u) == 0u) {
      return Add::BadId;
    }
    if (contains(id)) {
      return Add::Already;
    }
    if (count_ >= kMax) {
      if (refused_ < 255u) {
        ++refused_;
      }
      return Add::Full;
    }
    ids_[count_] = id;
    why_[count_] = why;
    ++count_;
    return Add::Added;
  }

  // The boot load: the crash record's module word (a module id has bit 15 set; a stage id is below it). True when the word is a module id, which
  // goes in as Crash (a .tmod's id too: the catalogue tells them apart, S3 checks it before honouring Why::Crash; the accepted clash of a .tmod and
  // a .ptfx id is about 0.4 %, 15 bits).
  bool boot_load(std::uint16_t module_word) noexcept {
    if (module_word < 0x8000u) {
      return false;
    }
    (void)quarantine(module_word, Why::Crash);
    return true;
  }

  bool contains(std::uint16_t id) const noexcept {
    for (unsigned i = 0; i < count_; ++i) {
      if (ids_[i] == id) {
        return true;
      }
    }
    return false;
  }

  unsigned count() const noexcept { return count_; }
  std::uint16_t id(unsigned i) const noexcept { return i < count_ ? ids_[i] : std::uint16_t{0}; }
  Why why(unsigned i) const noexcept { return i < count_ ? why_[i] : Why::Crash; }
  // The ids refused because the set was full (saturates at 255): the full flag is `refused() != 0`.
  unsigned refused() const noexcept { return refused_; }
  // The reason of `id`: why() of its slot, Crash when it is not in (check contains()).
  Why why_of(std::uint16_t id) const noexcept {
    for (unsigned i = 0; i < count_; ++i) {
      if (ids_[i] == id) {
        return why_[i];
      }
    }
    return Why::Crash;
  }

  static const char* why_name(Why w) noexcept {
    switch (w) {
      case Why::Crash: return "crash";
      case Why::Timeout: return "timeout";
      case Why::Fault: return "fault";
      case Why::StackOverflow: return "stack";
    }
    return "?";
  }

 private:
  std::uint16_t ids_[kMax] = {};
  Why why_[kMax] = {};
  std::uint8_t count_ = 0;
  std::uint8_t refused_ = 0;
};

}  // namespace o3os::module
