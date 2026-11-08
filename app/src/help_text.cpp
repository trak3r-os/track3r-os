// SPDX-License-Identifier: GPL-3.0-or-later
// OS-HELP (docs/specs/OS-HELP.md) / OS-FLASHDIET A7 (docs/specs/OS-FLASHDIET.md): the help pages. The text
// lives in the authoring file app/help/pages.txt, packed at build time by tools/help_pack.py into the
// generated help_blob.cpp (app/CMakeLists.txt): one raw-deflate stream, a page index, and the page titles in
// flash. help_page() below serves a page from that stream: it inflates the page into the 4 KiB page buffer
// (help_page_buf(), help_blob.h) only when the index differs from the last call (a one-entry cache), and the
// returned reference is valid until the next call with another index. The decode skips to the page's offset
// in the stream, so a page turn inflates just that page (the last page is the worst case: the whole text).
// Main loop only, synchronous, never in the render or an ISR. The lines are held as before -- ASCII, and no
// line wider than the dialog in any built-in theme (tests/help_test.cpp; tools/help_pack.py validates
// pages.txt the same way at build time). Text condensed from docs/manual/ and checked against
// app/src/bindings.cpp: where they disagree the binding is the truth.
#include <o3os/app/help_blob.h>

namespace o3os::app {

#ifndef O3OS_HELP_PAGE_BUF_EXTERNAL
namespace {
// The 4 KiB page buffer help_page() serves from. On the MK66 image the platform owns it in SDRAM_STATIC
// (app/CMakeLists.txt sets O3OS_HELP_PAGE_BUF_EXTERNAL, which drops this copy, and
// platform/mk66/src/app_main.cpp defines help_page_buf()); host, the simulator and QEMU keep this one.
HelpPageBuf g_help_page_buf;
}  // namespace
HelpPageBuf& help_page_buf() noexcept { return g_help_page_buf; }
#endif

namespace {

// What help_page() returns, pointed at the page buffer's parts, and the one-entry cache: the index of the
// page currently in the buffer (kHelpPages itself means none served yet).
HelpPage g_served;
unsigned g_served_index = kHelpPages;

}  // namespace

const HelpPage& help_page(unsigned index) noexcept {
  if (index >= kHelpPages) {
    index = kHelpPages - 1u;  // clamped to the last
  }
  if (index != g_served_index) {
    HelpPageBuf& buf = help_page_buf();
    if (help_blob_page(index, buf)) {
      g_served.title = buf.title;
      g_served.lines = buf.lines;
      g_served.count = buf.count;
    } else {
      g_served.title = "";  // a malformed stream or an overflow: help_pack.py refuses pages.txt at build time, so this is a guard
      g_served.lines = buf.lines;
      g_served.count = 0;
    }
    g_served_index = index;
  }
  return g_served;
}

unsigned help_page_of(ScreenId screen, bool dj_page) noexcept {
  switch (screen) {
    case ScreenId::Pattern: return 1;
    case ScreenId::Song: return 2;
    case ScreenId::Instrument: return 3;
    case ScreenId::Wave: return 4;
    case ScreenId::SampleEdit: return 5;
    case ScreenId::SampleLoad: return 6;
    case ScreenId::Record: return 7;
    case ScreenId::Mixer: return 8;
    case ScreenId::Perform: return dj_page ? 10 : 9;
    case ScreenId::File: return 11;
    case ScreenId::Config: return 12;
    case ScreenId::Modules: return 13;
    case ScreenId::ModuleApp: return 14;
    case ScreenId::InputTest: break;  // reached only by a boot chord; its input never reaches the binding that opens help
  }
  return 0;
}

}  // namespace o3os::app
