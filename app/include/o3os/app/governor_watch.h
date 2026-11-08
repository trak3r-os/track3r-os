// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-GOV (new Community OS behaviour): what the user is told when the CPU governor of the module instruments (audio/module_governor.h) refuses a note or mutes an instance. (A retrigger of an
// instrument that already sounds is not news: the governor admits it without a word.)
//
// The platform hands the App the governor's counters with the other audio figures (AudioLoad::gov_*, main loop, once a pass). feed() turns a rise of a counter into AT MOST ONE toast every
// kMinGapMs: the first event after a quiet spell is shown at once (leading edge), the ones that follow inside the gap are counted and shown together when the gap is over ("CPU FULL: 4 NOTES
// DROPPED"), so a busy pattern on a full CPU is one message every three seconds, not a flood. The worst kind of the pending ones wins (a mute over a drop). The first feed only takes the
// counters (events from before the App was built are not news). Pure: the clock is a parameter, the toast is data (the App posts it as the source "gov", upserted: one entry on screen, rewritten).

#include <o3os/app/system_load.h>
#include <o3os/app/text_format.h>
#include <o3os/audio/module_governor.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/notify.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// What a module costs, for the pages that pick or show it: the measured cycles of ONE voice for a whole block (ModuleVoices::voice_cost) as "~16% PER VOICE" (whole percent of the block, rounded),
// "<1% PER VOICE" under half a percent, and "UNMEASURED" while no note of it has sounded for two blocks (0).
// OS-INSFX S4: the same figure without the unit of a voice, for an effect insert (one instance): "~2%", "<1%", "UNMEASURED".
template <std::size_t N>
void append_block_cost(core::FixedString<N>& s, std::uint32_t cycles) noexcept {
  if (cycles == 0u) {
    s.append("UNMEASURED");
    return;
  }
  const unsigned percent = (audio::gov::tenths_of_block(cycles) + 5u) / 10u;
  if (percent == 0u) {
    s.append("<1%");
  } else {
    s.append("~");
    append_uint(s, percent);
    s.append("%");
  }
}
// The right-hand text of a cell of the Mixer's INSERTS page: ST first for a stereo effect (OS-INSFX-ABI2 A5), then OVER BUDGET (a net forced the instance to dry), else BYPASS (the record's flag),
// else NOT BOUND (a record with no instance while the build has the host: `expect_bound`), and the measured cost after it ("BYPASS  ~2%") while an instance is bound and has one. Nothing is appended
// for a plain insert that was never measured.
template <std::size_t N>
void append_insert_status(core::FixedString<N>& s, bool bypass, bool bound, bool forced, bool expect_bound, std::uint32_t cost, bool st = false) noexcept {
  if (st) {
    s.append("ST");
  }
  if (bound && forced) {
    s.append(s.size() != 0u ? "  " : "");
    s.append("OVER BUDGET");
  } else if (bypass) {
    s.append(s.size() != 0u ? "  " : "");
    s.append("BYPASS");
  } else if (!bound && expect_bound) {
    s.append(s.size() != 0u ? "  " : "");
    s.append("NOT BOUND");
  }
  if (bound && cost != 0u) {
    s.append(s.size() != 0u ? "  " : "");
    append_block_cost(s, cost);
  }
}
template <std::size_t N>
void append_voice_cost(core::FixedString<N>& s, std::uint32_t cycles) noexcept {
  append_block_cost(s, cycles);
  if (cycles != 0u) {
    s.append(" PER VOICE");
  }
}

class GovernorWatch {
 public:
  static constexpr std::uint32_t kMinGapMs = 3000;   // between two toasts
  static constexpr std::uint32_t kToastMs = 2000;    // how long one is shown (short)

  struct Toast {
    core::Severity severity = core::Severity::Warn;
    core::FixedString<core::kNotifyTextChars> text;
  };

  // True with `out` filled when a toast is due now.
  bool feed(std::uint64_t now_ms, const AudioLoad& a, Toast& out) noexcept {
    if (!primed_) {
      primed_ = true;
      drops_ = a.gov_drops;
      mutes_ = a.gov_mutes;
      return false;
    }
    pend_drops_ += a.gov_drops - drops_;
    pend_mutes_ += a.gov_mutes - mutes_;
    drops_ = a.gov_drops;
    mutes_ = a.gov_mutes;
    if (pend_drops_ + pend_mutes_ == 0u) {
      return false;
    }
    if (shown_ && now_ms - last_ms_ < kMinGapMs) {
      return false;  // inside the gap: the events stay pending and are shown together
    }
    const unsigned slot = a.gov_last & 0xFFu;
    out.text.clear();
    if (pend_mutes_ != 0u) {
      out.severity = core::Severity::Error;
      out.text.append("INS ");
      append_dec(out.text, slot + 1u, 2);
      out.text.append(" MUTED: OVER ITS CPU BUDGET");
    } else {
      out.severity = core::Severity::Warn;
      out.text.append("CPU FULL: ");
      append_count(out.text, pend_drops_, "NOTE", slot, kDrop, a.gov_last);
      out.text.append(" DROPPED");
    }
    pend_drops_ = pend_mutes_ = 0;
    shown_ = true;
    last_ms_ = now_ms;
    return true;
  }

 private:
  // audio::gov::Event values (the app does not include the audio layer's headers here): the high byte of AudioLoad::gov_last
  static constexpr unsigned kDrop = 1;

  // "INS 05 NOTE" for one event whose latest is of this kind (the instrument is known), "3 NOTES" for several.
  static void append_count(core::FixedString<core::kNotifyTextChars>& s, std::uint32_t n, const char* noun, unsigned slot, unsigned kind, std::uint16_t last) noexcept {
    if (n == 1u && (last >> 8) == kind) {
      s.append("INS ");
      append_dec(s, slot + 1u, 2);
      s.append(" ");
      s.append(noun);
    } else {
      append_uint(s, n < 99u ? static_cast<unsigned>(n) : 99u);
      s.append(" ");
      s.append(noun);
      s.append("S");
    }
  }

  bool primed_ = false;
  bool shown_ = false;
  std::uint32_t drops_ = 0;
  std::uint32_t mutes_ = 0;
  std::uint32_t pend_drops_ = 0;
  std::uint32_t pend_mutes_ = 0;
  std::uint64_t last_ms_ = 0;
};

}  // namespace o3os::app
