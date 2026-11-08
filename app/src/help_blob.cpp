// SPDX-License-Identifier: GPL-3.0-or-later
// OS-FLASHDIET A6/A7: the packed help pages (docs/specs/OS-FLASHDIET.md). The blob is generated into the build
// tree by tools/help_pack.py from app/help/pages.txt (app/CMakeLists.txt); this file inflates one page out of
// it with the one shared core::Inflater of the UI assets (ui::asset_inflater(), in SDRAM_STATIC on the MK66
// image). help_page() (src/help_text.cpp) serves its pages from here, so the blob is live in the image since
// A7: the old C++ table is gone.
#include <o3os/app/help_blob.h>

#include <o3os/core/inflate.h>
#include <o3os/ui/assets.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

namespace {

// The longest line help_pack allows: "| " plus a paragraph of kHelpKeyCols + kHelpTextCols, and one more
// for the NUL the line buffer never needs but the bound keeps simple.
constexpr std::size_t kMaxLine = 2 + static_cast<std::size_t>(kHelpKeyCols) + static_cast<std::size_t>(kHelpTextCols) + 1;

// Copy [first, last) plus a NUL to dst, advancing dst; nullptr when it does not fit the buffer's text.
char* put_str(char*& dst, const char* first, const char* last, const HelpPageBuf& out) noexcept {
  const std::size_t need = static_cast<std::size_t>(last - first) + 1;
  if (static_cast<std::size_t>(dst - out.text) + need > HelpPageBuf::kTextBytes) {
    return nullptr;
  }
  char* const at = dst;
  while (first < last) {
    *dst++ = *first++;
  }
  *dst++ = '\0';
  return at;
}

// One line of a page (without its '\n'), in the pages.txt encoding, appended to out's table; dst is the
// write cursor into out.text (out.text[0] is the shared empty string).
bool flush_line(HelpPageBuf& out, const char* line, std::size_t n, char*& dst) noexcept {
  if (static_cast<unsigned>(out.count) >= HelpPageBuf::kMaxLines) {
    return false;
  }
  HelpLine& l = out.lines[out.count];
  const char* const eol = line + n;
  const char* key = out.text;   // "" unless a key line stores one
  const char* text = out.text;  // "" for a blank line and a section label
  bool paragraph = false;
  if (n > 0 && line[0] == '|') {  // a paragraph line: "| <text>"
    paragraph = true;
    const char* t = line + 1;
    if (t < eol && *t == ' ') {
      ++t;
    }
    text = put_str(dst, t, eol, out);
    if (text == nullptr) {
      return false;
    }
  } else if (n != 0) {  // a key line: "<key>\t<text>" (help_pack guarantees the tab)
    const char* tab = line;
    while (tab < eol && *tab != '\t') {
      ++tab;
    }
    if (tab == eol) {
      return false;
    }
    if (line != tab) {  // an empty key is a continuation line
      key = put_str(dst, line, tab, out);
      if (key == nullptr) {
        return false;
      }
    }
    if (tab + 1 != eol) {  // no text: a section label
      text = put_str(dst, tab + 1, eol, out);
      if (text == nullptr) {
        return false;
      }
    }
  }  // n == 0: a blank line, key "" and text ""
  l.key = paragraph ? nullptr : key;
  l.text = text;
  ++out.count;
  return true;
}

}  // namespace

bool help_blob_page(unsigned index, HelpPageBuf& out) noexcept {
  if (kHelpBlobPages != kHelpPages) {
    return false;  // pages.txt and help_text.h disagree on the page count
  }
  if (index >= kHelpPages) {
    index = kHelpPages - 1u;  // clamped to the last, like help_page()
  }
  const std::uint32_t begin = kHelpBlobPageOffsets[index];
  const std::uint32_t end = index + 1u < kHelpBlobPages ? kHelpBlobPageOffsets[index + 1u]
                                                        : static_cast<std::uint32_t>(kHelpBlobRawSize);
  if (end <= begin || end > kHelpBlobRawSize) {
    return false;
  }
  const std::size_t want = end - begin;

  core::Inflater& inf = ui::asset_inflater();
  inf.begin(kHelpBlobZ, kHelpBlobZSize, kHelpBlobRawSize);
  std::size_t at = 0;
  while (at < begin) {  // skip to the page's offset (the window advances, a later read can reach back into it)
    at += inf.skip(begin - at);
    if (inf.failed()) {
      return false;
    }
  }

  out.title = kHelpBlobTitles[index];
  out.count = 0;
  out.text[0] = '\0';  // the shared empty string
  char* dst = out.text + 1;

  // Read the page in chunks and parse line by line: the page's raw text is never held whole, only its
  // compacted strings are (the packer validated they fit).
  char staging[128];
  char line[kMaxLine];
  std::size_t n = 0;
  std::size_t done = 0;
  while (done < want) {
    const std::size_t chunk = want - done < sizeof staging ? want - done : sizeof staging;
    const std::size_t r = inf.read(reinterpret_cast<std::uint8_t*>(staging), chunk);
    if (r == 0 || inf.failed()) {
      return false;
    }
    done += r;
    for (std::size_t k = 0; k < r; ++k) {
      const char c = staging[k];
      if (c == '\n') {
        if (!flush_line(out, line, n, dst)) {
          return false;
        }
        n = 0;
      } else {
        if (n == sizeof line) {
          return false;
        }
        line[n++] = c;
      }
    }
  }
  if (n != 0 && !flush_line(out, line, n, dst)) {
    return false;
  }
  return !inf.failed();
}

}  // namespace o3os::app
