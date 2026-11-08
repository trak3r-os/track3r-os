// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET A7: the packed help blob now serves help_page() (app/src/help_text.cpp; the old C++ table is
// gone). The golden of the old text is kept as test data: tests/data/help_old_pages.txt, made once from the
// old table in app/src/help_text.cpp, in the app/help/pages.txt authoring format ("== <title>" starts a
// page, "| <text>" a paragraph line, an empty line a blank line, "<key>\t<text>" a key line, "<key>\t" a
// section label, "\t<text>" a continuation), in the old kPages' order. This test holds everything that is
// served -- the stream, the page index, the titles, help_blob_page() and the live help_page() -- to that
// golden: the same titles, and for every page the same line count and, line by line, the same key (nullptr
// kept apart from "") and text.
#include <doctest/doctest.h>

#include <o3os/app/help_blob.h>
#include <o3os/app/help_text.h>
#include <o3os/core/inflate.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "support/need.h"

using namespace o3os;

namespace {

// One line of the golden, decoded from its packed encoding the way help_blob.cpp's flush_line encodes it.
struct GoldenLine {
  bool paragraph;  // key == nullptr
  std::string key;
  std::string text;
};

struct GoldenPage {
  std::string title;
  std::vector<GoldenLine> lines;
};

// The golden file into `pages` (NEED leaves this void function on a malformed file).
void load_golden(std::vector<GoldenPage>& pages) {
  std::FILE* f = std::fopen(O3_HELP_OLD_PAGES, "rb");
  NEED(f != nullptr);
  std::string text;
  char buf[4096];
  std::size_t r;
  while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) {
    text.append(buf, r);
  }
  std::fclose(f);

  std::vector<std::string> rows;
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t nl = text.find('\n', at);
    rows.push_back(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
    if (nl == std::string::npos) {
      break;
    }
    at = nl + 1;
  }
  if (!rows.empty() && rows.back().empty()) {
    rows.pop_back();  // the file's final newline, not a blank line
  }

  GoldenPage cur;
  bool have = false;
  for (const std::string& row : rows) {
    if (row.rfind("== ", 0) == 0) {
      if (have) {
        pages.push_back(cur);
      }
      cur = GoldenPage();
      cur.title = row.substr(3);
      have = true;
    } else {
      NEED(have);  // no line before the first "== " page
      GoldenLine l;
      if (row.empty()) {
        l = {false, "", ""};  // a blank line
      } else if (row[0] == '|') {
        std::string t = row.substr(1);
        if (!t.empty() && t[0] == ' ') {
          t = t.substr(1);
        }
        l = {true, "", t};  // a paragraph line
      } else {
        const std::size_t tab = row.find('\t');
        NEED(tab != std::string::npos);
        l = {false, row.substr(0, tab), row.substr(tab + 1)};  // a key line, a label or a continuation
      }
      cur.lines.push_back(l);
    }
  }
  if (have) {
    pages.push_back(cur);
  }
}

// The golden's raw stream, the way tools/help_pack.py's pack() builds it: every page's lines, one '\n' each.
std::string golden_raw(const std::vector<GoldenPage>& pages, std::vector<std::uint32_t>* offsets) {
  std::string raw;
  if (offsets != nullptr) {
    offsets->clear();
  }
  for (const GoldenPage& p : pages) {
    if (offsets != nullptr) {
      offsets->push_back(static_cast<std::uint32_t>(raw.size()));
    }
    for (const GoldenLine& l : p.lines) {
      if (l.paragraph) {
        raw += "| ";
        raw += l.text;
      } else if (!l.key.empty() || !l.text.empty()) {
        raw += l.key;
        raw += '\t';
        raw += l.text;
      }  // else a blank line: nothing but the newline
      raw += '\n';
    }
  }
  return raw;
}

// A served page (a HelpPageBuf or the HelpPage help_page() returns) against the golden's page.
void check_served(const char* title, unsigned count, const app::HelpLine* lines, const GoldenPage& g) {
  CHECK(std::string(title) == g.title);
  CHECK(count == static_cast<unsigned>(g.lines.size()));
  const unsigned n = count < static_cast<unsigned>(g.lines.size()) ? count : static_cast<unsigned>(g.lines.size());
  for (unsigned k = 0; k < n; ++k) {
    const GoldenLine& gl = g.lines[k];
    const app::HelpLine& l = lines[k];
    CHECK((l.key == nullptr) == gl.paragraph);  // nullptr against "" kept apart
    if (!gl.paragraph) {
      NEED(l.key != nullptr);
      CHECK(std::string(l.key) == gl.key);
    }
    NEED(l.text != nullptr);
    CHECK(std::string(l.text) == gl.text);
  }
}

}  // namespace

TEST_CASE("help_blob: the stream inflates whole, exactly raw_size, and equals the golden byte for byte") {
  std::vector<GoldenPage> golden;
  load_golden(golden);
  NEED(golden.size() == app::kHelpPages);
  NEED(app::kHelpBlobPages == app::kHelpPages);
  std::vector<std::uint32_t> offsets;
  const std::string raw_golden = golden_raw(golden, &offsets);

  core::Inflater inf;
  inf.begin(app::kHelpBlobZ, app::kHelpBlobZSize, app::kHelpBlobRawSize);
  std::string raw;
  char buf[97];  // read in odd-sized chunks, like a consumer that does not know the size
  for (;;) {
    const std::size_t r = inf.read(reinterpret_cast<std::uint8_t*>(buf), sizeof buf);
    if (inf.failed()) {
      FAIL("the stream ended failed()");
      return;
    }
    if (r == 0) {
      break;
    }
    raw.append(buf, r);
  }
  CHECK(inf.done());
  CHECK(raw.size() == app::kHelpBlobRawSize);
  CHECK(raw == raw_golden);

  // The page index and the titles: one offset per page, in order, each at a line's start; the titles in flash.
  for (unsigned p = 0; p < app::kHelpBlobPages; ++p) {
    const std::uint32_t off = app::kHelpBlobPageOffsets[p];
    CHECK(off == offsets[p]);
    CHECK(off < app::kHelpBlobRawSize);
    if (off > 0) {
      CHECK(raw[off - 1] == '\n');
    }
    CHECK(std::string(app::kHelpBlobTitles[p]) == golden[p].title);
  }
}

TEST_CASE("help_blob: every page help_blob_page and the live help_page serve is the golden's page") {
  std::vector<GoldenPage> golden;
  load_golden(golden);
  NEED(golden.size() == app::kHelpPages);
  NEED(app::kHelpBlobPages == app::kHelpPages);
  for (unsigned p = 0; p < app::kHelpPages; ++p) {
    app::HelpPageBuf buf;
    NEED(app::help_blob_page(p, buf));
    check_served(buf.title, buf.count, buf.lines, golden[p]);

    const app::HelpPage& served = app::help_page(p);  // the live path (A7): served from the same blob
    check_served(served.title, served.count, served.lines, golden[p]);
  }

  // Out of range clamps to the last page, like help_page() always did.
  app::HelpPageBuf buf;
  NEED(app::help_blob_page(app::kHelpPages + 5, buf));
  check_served(buf.title, buf.count, buf.lines, golden.back());
  const app::HelpPage& served = app::help_page(app::kHelpPages + 5);
  check_served(served.title, served.count, served.lines, golden.back());
}
