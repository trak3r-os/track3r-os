// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a whole-project OG export would do, asked before it starts (OS-FILES-C C2c, docs/specs/OS-FILES-C.md section 8): the folder it would make, the files and bytes, and what it would leave out or
// rewrite (formats::og::ExportDrops, the counters of the plan). The EXPORT page's OG PROJECT row shows it and its F1 question is made from it. Plain data, no card access
// (SaveService::og_survey fills it); kept apart from og_export.h so that the view state can hold one without the plan's headers.

#include <o3os/formats/og.h>

#include <cstdint>

namespace o3os::app {

struct OgSurvey {
  static constexpr unsigned kFolderChars = 32;  // "<name> OG 99" at most (the card guard's 31) and the terminator
  char folder[kFolderChars] = "";               // the folder's name inside Export/, "" when none can be made
  unsigned files = 0;                           // the files the export writes, project.mt included
  std::uint32_t bytes = 0;                      // about how many bytes (the instruments' PCM as it will be written, the fixed sizes of the rest)
  formats::og::ExportDrops drops;               // what the plan counts before it starts (the writers add what they leave out as they go)
};

// What F1 says while the page is still counting (source "file", like the question that replaces it; App::poll_export takes it off when the count is done).
inline constexpr const char* kOgCountingText = "COUNTING WHAT THE EXPORT LOSES: WAIT";

// One line of the loss report: `n` things of a kind, said short (the strip) and long (the page).
struct OgLossItem {
  std::uint32_t n = 0;
  const char* brief = "";
  const char* full = "";
};
inline constexpr unsigned kOgLossMax = 17;
// The kinds that were left out, rewritten or clamped, in the order the user should read them, the quietest last; returns how many (items[0..n)). The C2b counters come first: instruments not written
// (no audio, no sample memory, 12 / 8 BIT, over the length), the Amiga engine, the 12 / 8 BIT engines, over the length, clamped levels.
unsigned og_loss_items(const formats::og::ExportDrops& d, OgLossItem (&items)[kOgLossMax]) noexcept;
// The items on one line of at most cap - 1 bytes, "3 MODULES, 14 FX, 2 CLAMPED": as many as fit, then ".." when some did not. False (and "") when nothing was left out.
bool og_loss_line(const formats::og::ExportDrops& d, char* out, unsigned cap) noexcept;
// A number that changes when any counter does (the second F1 of the question compares it with the first's).
std::uint32_t og_loss_signature(const formats::og::ExportDrops& d) noexcept;

}  // namespace o3os::app
