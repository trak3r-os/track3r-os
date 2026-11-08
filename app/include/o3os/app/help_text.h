// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-HELP (docs/specs/OS-HELP.md): the text of the help pages (Q7). The authoring file is app/help/pages.txt; tools/help_pack.py packs it at build time into one
// raw-deflate stream plus a page index, with the page titles in flash (OS-FLASHDIET A6/A7, docs/specs/OS-FLASHDIET.md), and help_page() below inflates a page out of that stream.
// Shift + Config opens the page of the screen in view; the pages are the Global page, then one per screen, in this order.
// A page is a list of lines, each already wrapped to the body of the dialog (the author's job, held by tests/help_test.cpp: ASCII, and no line wider than the Label face allows in any built-in theme).
// A line is one of four things, told by its two pointers:
//   key == nullptr          a paragraph line, from the left margin across the body (what the screen is for)
//   key "" and text ""      a blank line
//   key "KEYS", text ""     a section label (KEYS, PADS), drawn as a key-cap
//   key "F1 PLAY", text ... the key in the left column and the explanation in the right one; a line that wraps goes on with an empty key
// The three parts of a page (spec item 7) are a paragraph or two, the section KEYS and the section PADS.

#include <o3os/app/screens/ids.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kHelpPages = 15;  // Global, then Pattern, Song, Instrument, Wave, Sample edit, Sample load, Record, Mixer, Perform (the slots, then the DJ page), File, Config, Modules and Module app (INPUT TEST has none)
inline constexpr int kHelpKeyCols = 22;     // characters of the left column (a key is at most one less)
inline constexpr int kHelpTextCols = 70;    // characters of the right column; a paragraph line may use kHelpKeyCols + kHelpTextCols

struct HelpLine {
  const char* key;
  const char* text;
};

struct HelpPage {
  const char* title;  // "HELP: SONG", the dialog's title (the page number is drawn beside it)
  const HelpLine* lines;
  std::uint16_t count;
};

// The page `index` (clamped to the last). The returned reference lives in the 4 KiB page buffer
// (help_blob.h's help_page_buf()) and is valid until the next help_page() call with another index.
const HelpPage& help_page(unsigned index) noexcept;
// The page of a screen; the Global page for a screen that has none (INPUT TEST). Perform has two pages and two help pages: the slots' page, and the DJ page when `dj_page` (ViewState::dj_page()).
unsigned help_page_of(ScreenId screen, bool dj_page = false) noexcept;

}  // namespace o3os::app
