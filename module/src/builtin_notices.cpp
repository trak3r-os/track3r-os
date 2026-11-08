// SPDX-License-Identifier: GPL-3.0-or-later

// OS-FLASHDIET P1: the licence notices of the built-in modules are a deflate stream (module/builtin.h), read line by line through a caller's core::Inflater, so the
// 7 KB of text is never in RAM: `mod notice [line]` pages it as it decodes.

#include <o3os/module/builtin.h>

namespace o3os::module {

NoticePage notices_page(const BuiltinNotices& n, core::Inflater& inf, unsigned first, unsigned max_lines, NoticeSink sink, void* ctx) noexcept {
  NoticePage page{true, false, 0};
  const unsigned last = first + max_lines;  // the page is lines [first, last)
  unsigned line = 0;                        // the line the next byte belongs to
  bool open = false;                        // that line has bytes already
  inf.begin(n.z, n.z_size, n.size);
  char buf[64];
  for (;;) {
    const std::size_t got = inf.read(reinterpret_cast<std::uint8_t*>(buf), sizeof buf);
    if (got == 0u) {
      break;
    }
    std::size_t i = 0;
    while (i < got) {
      if (line >= last) {  // a byte past the page's last line: there is more
        page.more = true;
        page.next = line;
        return page;
      }
      std::size_t j = i;
      while (j < got && buf[j] != '\n') {
        ++j;
      }
      const bool shown = line >= first;
      if (shown && j > i) {
        sink(ctx, buf + i, j - i, false);
      }
      open = open || j > i;
      if (j < got) {  // the newline ends the line
        if (shown) {
          sink(ctx, nullptr, 0, true);
        }
        ++line;
        open = false;
        i = j + 1;
      } else {
        i = j;
      }
    }
  }
  if (inf.failed() || !inf.done()) {
    page.ok = false;
    return page;
  }
  if (open && line >= first && line < last) {  // a last line without its newline is still a line
    sink(ctx, nullptr, 0, true);
    ++line;
  }
  page.next = line;
  return page;
}

}  // namespace o3os::module
