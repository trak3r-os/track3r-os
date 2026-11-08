// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/fixed_string.h>
#include <o3os/hal/storage.h>
#include <o3os/module/ptfx_catalog.h>
#include <o3os/module/ptfx_format.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// OS-CEPTFX S4a (docs/specs/OS-CEPTFX.md 3.7, A4, A15): what the PLUGINS page of Sample edit needs of the plugin it shows, held outside the view state (it lives in SDRAM with the sample bank's
// other big buffers, SampleBankMem): the header and the list strings of the plugin in view (read from the card by the App's main-loop pass, never by a key or the render), and the values the user
// set for the last kSaved plugins (A15: by file, while the OS runs, not saved anywhere). Main loop only.
//
// A list parameter's value is an item index; a list that has no usable string is shown as an integer ('d'). A value is always inside the parameter's range and, for 'd' and 'l', whole.
class PtfxPage {
 public:
  static constexpr unsigned kSaved = 8;  // A15
  static constexpr unsigned kParams = module::kPtfxMaxParams;
  static constexpr unsigned kItems = module::kPtfxMaxListItems;
  static constexpr std::size_t kValueChars = 12;  // the value column of the page: what format_value keeps (it truncates)

  module::PtfxCatalog* cat = nullptr;  // the listing the page shows (the platform's or the simulator's; null: no plugins here)

  enum class Load : std::uint8_t { Ready, Refused, CardError };

  // The listing changed or was thrown away: nothing is loaded.
  void forget() noexcept { loaded_ = kNone; ready_ = false; }
  bool loaded(unsigned index) const noexcept { return loaded_ == index; }
  // Reads the header and the list strings of listing entry `index` (a refused or quarantined entry reads nothing: Refused). Moves the plugin's saved values to the front of the A15 store, or starts them at the defaults.
  Load load(hal::storage::Storage& storage, unsigned index) noexcept;
  bool ready() const noexcept { return ready_; }  // the plugin in view has a header the page can show

  const module::PtfxInfo& info() const noexcept { return info_; }
  unsigned params() const noexcept { return ready_ ? info_.num_params : 0u; }
  char kind(unsigned p) const noexcept { return kind_[p]; }  // 'f', 'd' or 'l' (a 'l' without a usable string is 'd')
  unsigned items(unsigned p) const noexcept { return nitems_[p]; }
  const char* item(unsigned p, unsigned i) const noexcept { return items_[p][i]; }
  float value(unsigned p) const noexcept { return saved_[0].v[p]; }
  const float* values() const noexcept { return saved_[0].v; }  // the plugin's kParams values, for the run (PtfxJob::Plugin::values)
  // The value of parameter p moved by `clicks` steps (x10 when coarse), held to its range; false when nothing changed or the parameter cannot be edited.
  bool bump(unsigned p, int clicks, bool coarse) noexcept;
  bool reset(unsigned p) noexcept;  // the parameter's default
  bool reset_all() noexcept;
  // The value as the page shows it ('f' two decimals, 'd' whole, a list its item), then the unit, cut at kValueChars.
  void format_value(unsigned p, core::FixedString<kValueChars>& out) const noexcept;

 private:
  static constexpr std::uint16_t kNone = 0xFFFF;
  struct Saved {
    std::uint16_t id;  // 0: free (a plugin id has bit 15 set)
    float v[kParams];
  };
  float fix(unsigned p, float v) const noexcept;  // into the range, whole for 'd' and 'l'
  bool read_list(hal::storage::Storage& storage, hal::storage::FileHandle file, unsigned p) noexcept;

  std::uint16_t loaded_ = kNone;  // the listing index the info is of
  bool ready_ = false;
  module::PtfxInfo info_{};
  char kind_[kParams] = {};
  std::uint8_t nitems_[kParams] = {};
  char items_[kParams][kItems][11] = {};
  Saved saved_[kSaved] = {};
  std::uint8_t window_[256] = {};  // the list strings' read window
};

// The short reason of a refused listing (a status other than Ok; Ok with the quarantine flag reads PLUGIN QUARANTINED): what the page shows in place of the parameters and F1 APPLY says (S4b).
const char* ptfx_refusal_text(const module::PtfxEntry& e) noexcept;

}  // namespace o3os::app
