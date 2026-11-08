// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The colour tokens of the UI (ADR 0001, ADR 0010 section 8, docs/ui/style-guide.md). A token is a role, not a
// value: what a role looks like is decided by the theme (theme.h), whose tables are the ONLY place colour values are
// written. Everything else names a Color. Sixteen roles (ADR 0001) and Rec (below); a theme gives each one an RGB.
//
// The tokens come in two materials (ADR 0010 section 4): chrome (strips, track headers, the key row: light,
// desaturated grey-blue with dark text in the Amiga theme) and the data pane (the pattern grid: dark). The pairs that
// may be drawn over each other, and the contrast each must reach, are listed in theme.h (kContrastPairs) and tested
// for every theme.

#include <cstdint>

namespace o3os::ui {

struct Rgb {
  std::uint8_t r;
  std::uint8_t g;
  std::uint8_t b;
};

enum class Color : std::uint8_t {
  // Chrome material.
  Chrome,      // fill of strips, track headers and the key row
  ChromeText,  // primary text on chrome; the fill of inverted key-caps and badges
  ChromeDim,   // secondary text and lines on chrome; the fill of the STOP badge
  // Data pane.
  Pane,        // the data pane; also the slot behind a meter
  Text,        // primary data: notes, beat row numbers, names
  TextDim,     // other row numbers, secondary labels on the pane
  Empty,       // empty fields and rules, about 2:1 against the pane (ADR 0010 section 4)
  Instr,       // instrument column
  Vol,         // volume column
  Fx,          // both FX columns
  // Selection and the one accent.
  Sel,         // the selected row and the selected track's underline: the one strong blue
  SelText,     // text on Sel
  Accent,      // the cursor cell and record-arm: the one orange, nowhere else
  AccentText,  // text on Accent
  // Transport.
  Play,        // the PLAY badge and meter fill: the one green
  PlayText,    // text on Play
  // Record (Rec mode, OS-REVIEW). The 17th token, past the sixteen of ADR 0001, by the maintainer's request of 2026-10-13: the one red. It is a new id at the end, so no id
  // moved and the module SDK's O3M_COLOR_COUNT stays 16 (a module cannot name it).
  Rec,         // the Pattern screen's Rec mode: the frame round the grid, the REC badge and the playhead bar (SelText on it)
  // The 18th token (OS-SELTINT), at the end for the same reason: the selection of a multi-track block outside the cursor's track, a little duller than Sel.
  SelDim,      // the block's highlight on the tracks other than the cursor's (SelText on it)
};

inline constexpr unsigned kColorCount = static_cast<unsigned>(Color::SelDim) + 1;
static_assert(kColorCount == 18, "a theme has 16 colours (ADR 0010 section 8), Rec, the red of Rec mode, and SelDim, the block's tint off the cursor's track");

// The FT812 panel the UI is designed for (the simulator's framebuffer is the same size).
inline constexpr int kScreenW = 800;
inline constexpr int kScreenH = 480;

}  // namespace o3os::ui
