// SPDX-License-Identifier: GPL-3.0-or-later
//
// The display-list budget harness (sim/budget.h) and its use: every worst-case state of the screen registry
// (sim/screens.cpp) must fit its limit, and the table is printed so a breach is read at a glance. This replaces the
// hand-rolled worst-case test of the pattern view.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/sim/budget.h>
#include <o3os/sim/screens.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace o3os;

namespace {

const sim::BudgetRow* find_row(const std::vector<sim::BudgetRow>& rows, const char* screen, const char* state,
                               const char* theme, bool translate) {
  for (const sim::BudgetRow& row : rows) {
    if (std::string(row.screen) == screen && std::string(row.state) == state && std::string(row.theme) == theme &&
        row.translate == translate) {
      return &row;
    }
  }
  return nullptr;
}

// A synthetic screen: `words_for(variant)` rectangles, so the harness can be shown to catch a breach.
unsigned g_rects = 0;
void draw_rects(ui::Renderer& r, unsigned variant) {
  for (unsigned i = 0; i < g_rects + variant; ++i) {
    r.panel(ui::Rect{static_cast<int>(i % 700), 0, 3, 3}, ui::Color::Chrome);
  }
}
void draw_nothing(ui::Renderer&, unsigned) {}

// The whole table is measured once for the cases below (a few seconds under the sanitizers).
const std::vector<sim::BudgetRow>& all_rows() {
  static const std::vector<sim::BudgetRow> rows = sim::measure_all();
  return rows;
}

}  // namespace

TEST_CASE("budget: every worst-case state fits its limit in every theme and both modes, and every state fits the chip's display list") {
  const auto& rows = all_rows();
  const std::string table = sim::format_table(rows);
  std::fputs(table.c_str(), stdout);  // the per-screen table; ctest shows it with -V or on failure
  CHECK(rows.size() >= 3u * ui::kThemeCount * 2u);
  for (const sim::BudgetRow& row : rows) {
    const char* mode = row.translate ? "/translate" : "/vtx2f";
    CHECK_MESSAGE(row.complete, row.screen << "/" << row.state << "/" << row.theme << mode << " produced no frame");
    CHECK_MESSAGE(row.within(), row.screen << "/" << row.state << "/" << row.theme << mode << ": " << row.words
                                           << " words, limit " << row.limit);
  }
  CHECK(sim::all_within(rows));
}

TEST_CASE("budget: no state has an exception: every worst case is held to the standard 1792 words, in every theme and both modes") {
  // OS-003b ended the pattern view's interim limit (1920) by moving its grid onto tile faces: the standard is the only
  // limit a state may have. Both ways of reaching x >= 512 are held to it: VERTEX_TRANSLATE_X (the default) and the
  // VERTEX2F fallback that is plan B if the chip does not translate VERTEX2II.
  static_assert(sim::kBudgetWords == 1792, "ADR 0010 section 9");
  for (unsigned i = 0; i < sim::screen_count(); ++i) {
    const sim::Screen& screen = sim::screens()[i];
    for (unsigned j = 0; j < screen.state_count; ++j) {
      const sim::ScreenState& state = screen.states[j];
      if (state.worst_case) {
        CHECK_MESSAGE(state.limit_words == sim::kBudgetWords, screen.name << "/" << state.name << " has limit " << state.limit_words);
      }
    }
  }
  const auto& rows = all_rows();
  // Every theme and mode has a row for every state, in registry order, and the worst cases are real.
  std::size_t expected = 0;
  for (unsigned i = 0; i < sim::screen_count(); ++i) {
    expected += sim::screens()[i].state_count * ui::kThemeCount * 2;
  }
  CHECK(rows.size() == expected);
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const char* theme = ui::theme(ui::theme_id_at(t)).name;
    for (const bool translate : {true, false}) {
      const std::size_t worst = sim::worst_words(rows, theme, translate);
      CHECK_MESSAGE(worst > 500, theme);
      CHECK_MESSAGE(worst <= sim::kBudgetWords, theme << (translate ? " translate: " : " vtx2f: ") << worst);
      const sim::BudgetRow* pattern = nullptr;
      for (const sim::BudgetRow& row : rows) {
        if (std::string(row.screen) == "pattern" && std::string(row.state) == "worst" && std::string(row.theme) == theme &&
            row.translate == translate) {
          pattern = &row;
        }
      }
      CHECK(pattern != nullptr);
      if (pattern == nullptr) {
        continue;
      }
      CHECK(pattern->budgeted);
      CHECK(pattern->limit == sim::kBudgetWords);
      CHECK(pattern->words <= pattern->limit);
      // The tile faces are what make the pattern view fit (ADR 0010 section 5): a step is one word a field (two for an FX,
      // and two for an empty instrument or volume, which are dashes), so the whole screen stays far below the 1856 words
      // the glyph-by-glyph grid needed. A regression that draws a field twice (a glyph string and its tile) shows here long
      // before the budget does. (This guards the pattern view's own worst case: the overlays over it are held to the
      // standard budget like every state, and are tested below.)
      // (OS-009b: 1300 became 1320 for the M and S chips of the header, two words each, which the marks of the worst case now draw on five tracks.)
      // (OS-PADS-A: 1320 became 1325 and 1650 1665 for the Shift picker's key row: the inverted chip of the mode that stays and the dim colour pass of the reserved keys.)
      // (OS-AUDIT E-PAT-03: 1325 became 1420 translate for the FX names of the 5-track zoom, which draw a name tile and a Micro value instead of a glyph and a Data pair: 1419 translate, 1645 vtx2f; the vtx2f cap holds, the standard 1792 holds at 1711 in the worst screen.)
      // (OS-PTOOLS2 S1: 1420 became 1465 translate and 1665 1710 vtx2f for the block hint of the info line, one text of up to 60 characters, a word each: 1459 / 1701 measured, the standard 1792 holds.)
      // (OS-SELTINT: 1465 became 1530 translate and 1710 1775 vtx2f for the three-rectangle block rows over several tracks (a dim run, the cursor's track, a dim run): 1522 / 1768 measured, the standard 1792 holds.)
      CHECK_MESSAGE(pattern->words <= (translate ? 1530u : 1775u), theme << (translate ? " translate: " : " vtx2f: ") << pattern->words);
      // The test is not vacuous: the worst case is a real sweep (every instrument, every window, two contents), and
      // really the biggest frame of its screen.
      CHECK(pattern->variants > 500u);
      for (const sim::BudgetRow& row : rows) {
        if (std::string(row.screen) == "pattern" && std::string(row.theme) == theme && row.translate == translate) {
          CHECK(row.complete);
          if (std::string(row.state).rfind("zoom", 0) != 0 && std::string(row.state) != "lane_marks") {
            CHECK(row.words <= pattern->words);  // the pattern's own states (typical, cursor_end) are below its worst
          }
        }
      }
      // OS-ZOOM: the zoomed grids are budgeted states of their own (3 contents x marks x block x 3 cursor tracks), above the default level's worst because more fields are drawn: 8 tracks of Note, Instr
      // and Vol cost more than 5 tracks (an empty instrument or volume is two glyph words, a note past G10 three), 12 tracks of Note alone less. Pinned, with a few words of room, so a change shows here
      // long before the 1792 budget does: measured 1403 / 1605 words at 8 tracks and 1158 / 1417 at 12 (translate / VERTEX2F); with the block hint of OS-PTOOLS2 S1 1441 / 1659 and 1197 / 1472.
      const sim::BudgetRow* zoom8 = find_row(rows, "pattern", "zoom8", theme, translate);
      const sim::BudgetRow* zoom12 = find_row(rows, "pattern", "zoom12", theme, translate);
      CHECK((zoom8 != nullptr && zoom12 != nullptr));
      if (zoom8 != nullptr && zoom12 != nullptr) {
        CHECK((zoom8->budgeted && zoom12->budgeted && zoom8->variants == 36u && zoom12->variants == 36u));
        // (OS-SELTINT: 1450 / 1670 became 1545 / 1765 at 8 tracks and 1205 / 1480 1300 / 1580 at 12: 1537 / 1758 and 1293 / 1572 measured.)
        CHECK_MESSAGE(zoom8->words <= (translate ? 1545u : 1765u), theme << (translate ? " translate: " : " vtx2f: ") << zoom8->words);
        CHECK_MESSAGE(zoom12->words <= (translate ? 1300u : 1580u), theme << (translate ? " translate: " : " vtx2f: ") << zoom12->words);
      }
      // OS-MIDIREC-P1 S4: the lane marks (a lane on every track: a bar on every step, `MID nn cc` or `MID` on every empty step) are a budgeted state of their own at the three zooms; the grid
      // degrades (the held steps' "MID" first, then the bars) so it stays inside the standard budget.
      const sim::BudgetRow* lanes = find_row(rows, "pattern", "lane_marks", theme, translate);
      CHECK(lanes != nullptr);
      if (lanes != nullptr) {
        CHECK((lanes->budgeted && lanes->complete && lanes->variants == 108u));
        CHECK_MESSAGE(lanes->words <= eve::kMaxDisplayListWords, theme << (translate ? " translate: " : " vtx2f: ") << lanes->words);
      }
      const sim::BudgetRow* typical = find_row(rows, "pattern", "typical", theme, translate);
      CHECK(typical != nullptr);
      if (typical != nullptr) {
        CHECK_FALSE(typical->budgeted);
        CHECK(typical->limit == eve::kMaxDisplayListWords);  // not budgeted: held to the chip's list only
        CHECK(typical->words > 0);
        CHECK(typical->words < pattern->words);
      }
    }
    // The fallback costs words (CELL and BITMAP_HANDLE where the origin would have moved): never fewer than translating.
    // (OS-MIDIREC-P1 S4: the lane-marked grid draws up to the cap whatever the mode, so it is left out of this comparison: it would make the two worst cases equal.)
    std::vector<sim::BudgetRow> uncapped;
    for (const sim::BudgetRow& row : rows) {
      if (std::string(row.state) != "lane_marks") {
        uncapped.push_back(row);
      }
    }
    CHECK(sim::worst_words(uncapped, theme, false) > sim::worst_words(uncapped, theme, true));
  }
  CHECK(sim::format_table(rows).find("interim") == std::string::npos);
  CHECK(sim::format_table(rows).find("worst case per theme, translate: amiga ") != std::string::npos);
  CHECK(sim::format_table(rows).find("worst case per theme, vtx2f fallback: amiga ") != std::string::npos);
}

TEST_CASE("budget: the harness catches a state over its limit, an oversize screen reports its real size") {
  const ui::Theme& amiga = ui::theme(ui::ThemeId::Amiga);
  // Nothing but the frame: CLEAR colour, CLEAR, LINE_WIDTH and the 32 words that bind the eight bitmap handles = 35, plus DISPLAY.
  static const sim::ScreenState empty = {"empty", true, 1, sim::kBudgetWords, draw_nothing};
  const sim::Screen screen = {"synthetic", &empty, 1};
  const sim::BudgetRow base = sim::measure(screen, empty, amiga);
  CHECK(base.complete);
  CHECK(base.within());
  CHECK(base.words == 3 + 32 + 1);
  CHECK(base.worst_variant == 0u);
  CHECK(std::string(base.theme) == "amiga");

  // Each rectangle costs 2 words, the first adds COLOR_RGB and BEGIN, and the frame closes the primitive with END;
  // sweeping the variants finds the largest, and the first one on a tie is reported.
  g_rects = 1000;
  static const sim::ScreenState sweep = {"sweep", true, 5, sim::kBudgetWords, draw_rects};
  const sim::BudgetRow over = sim::measure(sim::Screen{"synthetic", &sweep, 1}, sweep, amiga);
  CHECK(over.variants == 5u);
  CHECK(over.worst_variant == 4u);                    // variant 4 draws 1004 rectangles
  CHECK(over.words == 36 + 3 + 2 * 1004);             // base 36, COLOR_RGB + BEGIN + END, two vertices each
  CHECK(over.words > sim::kBudgetWords);
  CHECK_FALSE(over.within());
  CHECK_FALSE(sim::all_within({base, over}));
  CHECK(sim::all_within({base}));
  const std::string table = sim::format_table({base, over});
  CHECK(table.find("OVER") != std::string::npos);
  CHECK(table.find("words over") != std::string::npos);
  CHECK(table.find("synthetic") != std::string::npos);
  CHECK(table.find("amiga") != std::string::npos);
  CHECK(sim::worst_words({base, over}, "amiga") == over.words);
  CHECK(sim::worst_words({base, over}, "black") == 0u);

  // Larger than the chip's list: the real size is reported (not "overflow"), and a non-budgeted state is held to 2048.
  g_rects = 1500;
  static const sim::ScreenState huge = {"huge", false, 1, 0, draw_rects};
  const sim::BudgetRow big = sim::measure(sim::Screen{"synthetic", &huge, 1}, huge, amiga);
  CHECK(big.complete);
  CHECK(big.words == 36 + 3 + 2 * 1500);
  CHECK(big.words > eve::kMaxDisplayListWords);
  CHECK_FALSE(big.within());
  CHECK(big.limit == eve::kMaxDisplayListWords);
  g_rects = 0;
}

TEST_CASE("budget: a standard limit is stricter than the chip's, and the registry's names are file-name safe") {
  static_assert(sim::kBudgetWords == 1792, "ADR 0010 section 9");
  static_assert(sim::kBudgetWords < eve::kMaxDisplayListWords, "the budget leaves a margin");
  CHECK(sim::screen_count() >= 1u);
  const auto safe = [](const char* name) {
    const std::string s = name;
    if (s.empty()) {
      return false;
    }
    for (const char c : s) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
        return false;
      }
    }
    return true;
  };
  for (unsigned i = 0; i < sim::screen_count(); ++i) {
    const sim::Screen& screen = sim::screens()[i];
    CHECK_MESSAGE(safe(screen.name), screen.name);
    CHECK(screen.state_count >= 1u);
    for (unsigned j = 0; j < screen.state_count; ++j) {
      const sim::ScreenState& state = screen.states[j];
      CHECK_MESSAGE(safe(state.name), screen.name << "/" << state.name);
      CHECK(state.variants >= 1u);
      CHECK(state.draw != nullptr);
      CHECK((!state.worst_case || state.limit_words >= 1));
      CHECK(state.limit_words <= eve::kMaxDisplayListWords);
    }
  }
}

// OS-002b: the Song screen and the overlays are states of the registry like any other, so every one is measured in every
// theme and both modes and held to the standard budget; the overlays are measured over the dearest content the screen under
// them can show (the pattern's own worst grid, the song's worst list), with the longest notification.
TEST_CASE("budget: the Song screen and every overlay over the Pattern and Song screens fit the standard budget in every theme and both modes") {
  const auto& rows = all_rows();
  struct Want {
    const char* screen;
    const char* state;
  };
  const Want wanted[] = {
      {"song", "typical"},           {"song", "worst"},
      {"song", "document_map"},      {"song", "document_ask"},        {"overlay", "song_tools"},   {"overlay", "song_tools_form"},   {"overlay", "song_tools_bridge"},   {"overlay", "song_tools_worst"},   // OS-SONGMAP, OS-SONG-BUILD
      {"song", "document_map_empty"}, {"song", "document_map_16"},    {"song", "document_map_churn"},   // OS-BUG-SONGMAPCUR: the empty cells' ticks, 12 and 16 tracks, the dearest sweep
      {"song", "timeline"},          {"song", "timeline_worst"},  // OS-SONG-TIMELINE
      {"song", "timeline_edit"},     {"song", "timeline_edit_worst"},  // OS-SONG3 S2: the editable TIMELINE
      {"song", "timeline_edit_worst_dim"},  // OS-SONG3 S3 review fold: the same sweep with alternating muted tracks (the worst run count)
      {"overlay", "pattern_learn"},  {"overlay", "pattern_learn_worst"},
      {"overlay", "pattern_navigator"}, {"overlay", "pattern_navigator_worst"},
      {"overlay", "pattern_notify"}, {"overlay", "pattern_notify_worst"},
      {"overlay", "song_notify"},    {"overlay", "song_notify_worst"},
  };
  for (const Want& w : wanted) {
    for (unsigned t = 0; t < ui::kThemeCount; ++t) {
      const char* theme = ui::theme(ui::theme_id_at(t)).name;
      for (const bool translate : {true, false}) {
        const sim::BudgetRow* row = find_row(rows, w.screen, w.state, theme, translate);
        CHECK_MESSAGE(row != nullptr, w.screen << "/" << w.state << "/" << theme);
        if (row == nullptr) {
          continue;
        }
        const char* mode = translate ? "/translate" : "/vtx2f";
        CHECK_MESSAGE(row->complete, w.screen << "/" << w.state << "/" << theme << mode);
        CHECK_MESSAGE(row->budgeted, w.screen << "/" << w.state << "/" << theme << mode << " is not a budgeted state");
        CHECK_MESSAGE(row->limit == sim::kBudgetWords, w.screen << "/" << w.state);
        CHECK_MESSAGE(row->within(), w.screen << "/" << w.state << "/" << theme << mode << ": " << row->words << " words");
        CHECK(row->words > 400u);
      }
    }
  }
  // A worst-case sweep is at least its typical state, and the fallback costs words everywhere (the overlays included).
  for (const char* base : {"pattern_learn", "pattern_navigator", "pattern_notify", "song_notify"}) {
    for (unsigned t = 0; t < ui::kThemeCount; ++t) {
      const char* theme = ui::theme(ui::theme_id_at(t)).name;
      const std::string worst = std::string(base) + "_worst";
      for (const bool translate : {true, false}) {
        const sim::BudgetRow* typical = find_row(rows, "overlay", base, theme, translate);
        const sim::BudgetRow* sweep = find_row(rows, "overlay", worst.c_str(), theme, translate);
        CHECK((typical != nullptr && sweep != nullptr));
        if (typical != nullptr && sweep != nullptr) {
          CHECK_MESSAGE(sweep->words >= typical->words, base << " " << theme);
          CHECK(sweep->variants > 10u);
        }
      }
      const sim::BudgetRow* a = find_row(rows, "overlay", worst.c_str(), theme, true);
      const sim::BudgetRow* b = find_row(rows, "overlay", worst.c_str(), theme, false);
      if (a != nullptr && b != nullptr) {
        CHECK(b->words > a->words);
      }
    }
  }
}
