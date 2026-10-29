// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3os::app {

// The Mixer screen's cursor (OS-011b, mixer_view.h says what the two numbers mean on each page): two bytes of the view, clamped to the page whenever they are read.
struct MixerView {
  std::uint8_t row = 0;
  std::uint8_t col = 0;
  // OS-QAFIX-E-GLOB-18: the STEMS page's Delete (1) and Shift + Delete (2) are final (the Mixer has no undo), so the first press asks and the same key again within kResetWindowMs does it. 0: nothing asked.
  static constexpr std::uint32_t kResetWindowMs = 2000;
  std::uint8_t reset_asked = 0;
  std::uint8_t reset_row = 0;  // the cursor the question was asked on: a Delete elsewhere asks again
  std::uint8_t reset_col = 0;
  std::uint32_t reset_at = 0;
  // OS-USBOUTS: the OUTPUTS row of the STEMS page (row 0) holds a PENDING channel count (4, 6, 8, 10; 0 none) until the apply key (F6) is pressed twice; leaving the page drops it. Starred on screen.
  // The apply question uses reset_asked = 3 and reset_at above.
  std::uint8_t outputs_pending = 0;
};

}  // namespace o3os::app
