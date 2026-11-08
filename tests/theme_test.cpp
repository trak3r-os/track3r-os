// SPDX-License-Identifier: GPL-3.0-or-later
//
// Themes (ui/theme.h): the contrast of every pair of tokens that may be drawn over each other, per theme, as ADR 0010
// amendment 1 asks: at least 4.5:1 for data and labels, at least 3:1 for Display text and indicators, about 2:1 for
// the decoration that is meant to be quiet. The same check will gate SD theme packs, so it is written against any
// ui::Theme, and a deliberately broken one proves it can fail.
#include <doctest/doctest.h>

#include <o3os/ui/theme.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>

using namespace o3os;
using ui::Color;
using ui::PairRole;
using ui::Rgb;
using ui::Theme;
using ui::ThemeId;

namespace {

// WCAG 2.x relative luminance and contrast ratio.
double channel(unsigned c) {
  const double s = static_cast<double>(c) / 255.0;
  return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}
double luminance(const Rgb& c) { return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b); }
double contrast(const Rgb& a, const Rgb& b) {
  const double la = luminance(a), lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

const char* token_name(Color c) {
  static const char* const names[] = {"Chrome", "ChromeText", "ChromeDim", "Pane",    "Text",   "TextDim",
                                      "Empty",  "Instr",      "Vol",       "Fx",      "Sel",    "SelText",
                                      "Accent", "AccentText", "Play",      "PlayText", "Rec", "SelDim"};
  return names[static_cast<unsigned>(c)];
}

// Every pair against its role; returns the failures as text (empty: the theme passes).
std::string failures_of(const Theme& theme) {
  std::string out;
  for (unsigned i = 0; i < ui::kContrastPairCount; ++i) {
    const ui::ContrastPair& p = ui::kContrastPairs[i];
    const double ratio = contrast(theme.rgb(p.fg), theme.rgb(p.bg));
    bool ok = false;
    switch (p.role) {
      case PairRole::Text:
        ok = ratio >= static_cast<double>(ui::kMinTextContrast);
        break;
      case PairRole::Large:
        ok = ratio >= static_cast<double>(ui::kMinLargeContrast);
        break;
      case PairRole::Dim:
        ok = ratio >= static_cast<double>(ui::kDimContrastLow) && ratio <= static_cast<double>(ui::kDimContrastHigh);
        break;
    }
    if (!ok) {
      out += std::string(theme.name) + ": " + token_name(p.fg) + " on " + token_name(p.bg) + " is " +
             std::to_string(ratio) + ":1\n";
    }
  }
  return out;
}

const Theme& built_in(unsigned i) { return ui::theme(ui::theme_id_at(i)); }

}  // namespace

TEST_CASE("theme contrast: every text/background pair of every theme meets its ratio") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const std::string failed = failures_of(built_in(t));
    CHECK_MESSAGE(failed.empty(), failed);
  }
}

TEST_CASE("theme contrast: the table is not vacuous: a theme that breaks a pair is caught, per role") {
  Theme broken = built_in(0);
  CHECK(failures_of(broken).empty());
  broken.colour[static_cast<unsigned>(Color::TextDim)] = broken.rgb(Color::Pane);  // data text lost in the pane
  CHECK(failures_of(broken).find("TextDim on Pane") != std::string::npos);
  broken = built_in(0);
  broken.colour[static_cast<unsigned>(Color::Sel)] = broken.rgb(Color::Pane);  // an indicator lost
  CHECK(failures_of(broken).find("Sel on Pane") != std::string::npos);
  broken = built_in(0);
  broken.colour[static_cast<unsigned>(Color::Empty)] = broken.rgb(Color::Text);  // decoration as loud as data
  CHECK(failures_of(broken).find("Empty on Pane") != std::string::npos);
  broken = built_in(0);
  broken.colour[static_cast<unsigned>(Color::Empty)] = broken.rgb(Color::Pane);  // and as quiet as nothing
  CHECK(failures_of(broken).find("Empty on Pane") != std::string::npos);
  broken = built_in(0);
  broken.colour[static_cast<unsigned>(Color::Pane)] = broken.rgb(Color::Text);  // OS-BUG-LEGIB: the letters knocked out of a Text chip (the Shift tags, the Navigator's key-caps) lost in it
  CHECK(failures_of(broken).find("Pane on Text") != std::string::npos);
  broken = built_in(0);
  broken.colour[static_cast<unsigned>(Color::Sel)] = broken.rgb(Color::SelText);  // the chosen cell's key-cap: Sel letters in a SelText box
  CHECK(failures_of(broken).find("Sel on SelText") != std::string::npos);
  // The ratio itself, on values worked by hand: black on white is 21:1, a colour on itself 1:1.
  CHECK(contrast(Rgb{0, 0, 0}, Rgb{255, 255, 255}) == doctest::Approx(21.0));
  CHECK(contrast(Rgb{128, 128, 128}, Rgb{128, 128, 128}) == doctest::Approx(1.0));
  CHECK(contrast(Rgb{119, 119, 119}, Rgb{255, 255, 255}) == doctest::Approx(4.48).epsilon(0.01));  // #777 on white
}

TEST_CASE("theme contrast: every token takes part in a pair, and each pair names two different tokens") {
  std::set<unsigned> used;
  for (unsigned i = 0; i < ui::kContrastPairCount; ++i) {
    const ui::ContrastPair& p = ui::kContrastPairs[i];
    CHECK(p.fg != p.bg);
    used.insert(static_cast<unsigned>(p.fg));
    used.insert(static_cast<unsigned>(p.bg));
  }
  CHECK(used.size() == ui::kColorCount);
}

TEST_CASE("theme: three built-in themes, Amiga first and the default, names find them case-insensitively") {
  CHECK(ui::kThemeCount == 3u);
  CHECK(std::string(built_in(0).name) == "amiga");
  CHECK(std::string(built_in(1).name) == "black");
  CHECK(std::string(built_in(2).name) == "white");
  CHECK(&ui::theme(ThemeId::Amiga) == &built_in(0));
  CHECK(&ui::theme(ui::theme_id_at(99)) == &built_in(0));  // out of range: Amiga
  ThemeId id = ThemeId::White;
  CHECK(ui::find_theme("amiga", 5, id));
  CHECK(id == ThemeId::Amiga);
  CHECK(ui::find_theme("BLACK", 5, id));
  CHECK(id == ThemeId::Black);
  CHECK(ui::find_theme("White", 5, id));
  CHECK(id == ThemeId::White);
  id = ThemeId::Black;
  CHECK_FALSE(ui::find_theme("whit", 4, id));   // whole names only
  CHECK_FALSE(ui::find_theme("whites", 6, id));
  CHECK_FALSE(ui::find_theme("", 0, id));
  CHECK_FALSE(ui::find_theme("amiga", 4, id));
  CHECK(id == ThemeId::Black);                    // untouched on failure
  // The cycle of ui.theme.next wraps.
  CHECK(ui::next_theme(ThemeId::Amiga) == ThemeId::Black);
  CHECK(ui::next_theme(ThemeId::Black) == ThemeId::White);
  CHECK(ui::next_theme(ThemeId::White) == ThemeId::Amiga);
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const std::string name = built_in(t).name;
    CHECK(!name.empty());
    for (const char c : name) {
      CHECK(((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'));  // a file name and a golden key
    }
  }
}

TEST_CASE("theme: 17 tokens, at most 17 distinct, the faces and spacing are complete, every theme has them") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const Theme& th = built_in(t);
    std::set<std::uint32_t> distinct;
    for (const Rgb& c : th.colour) {
      distinct.insert((std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b);
    }
    CHECK(distinct.size() <= ui::kColorCount);
    CHECK(distinct.size() >= 12u);  // a theme of a few greys would not be one
    // The metrics the layouts need are all there, and the Data cell fits the row pitch with room for the rule.
    const ui::Metrics& m = th.metrics;
    CHECK((m.border >= 1 && m.pad >= 2 && m.gap >= 2 && m.meter_h >= 2));
    CHECK(m.row_pitch >= th.geometry(ui::Face::Data).cell_h);
    CHECK(m.strip_h >= th.geometry(ui::Face::Display).cap_h);
    CHECK(m.keys_h >= th.geometry(ui::Face::Label).cap_h + 2 * m.border);
    // Roles map to the four compiled faces: Display is the largest, Micro the smallest.
    CHECK(th.font(ui::Face::Display) == ui::Font::F16x32);
    CHECK(th.font(ui::Face::Data) == ui::Font::F10x20);
    CHECK(th.font(ui::Face::Label) == ui::Font::F8x16);
    CHECK(th.font(ui::Face::Micro) == ui::Font::F6x12);
  }
}

TEST_CASE("theme: Rec is the one red (Rec mode, OS-REVIEW): red-dominant in every theme, apart from the orange accent and from every other token") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const Theme& th = built_in(t);
    const Rgb rec = th.rgb(Color::Rec);
    CHECK_MESSAGE((rec.r >= 3 * rec.g && rec.r >= 3 * rec.b && rec.r >= 150), th.name << ": Rec reads as red");
    for (unsigned c = 0; c < ui::kColorCount; ++c) {  // every token but Rec itself
      if (static_cast<Color>(c) == Color::Rec) {
        continue;
      }
      const Rgb o = th.colour[c];
      CHECK_MESSAGE((o.r != rec.r || o.g != rec.g || o.b != rec.b), th.name << ": " << token_name(static_cast<Color>(c)) << " is the same as Rec");
    }
  }
  CHECK(static_cast<unsigned>(Color::Rec) == 16u);  // the 17th id, and SelDim the 18th and last: no existing token (nor the SDK's 16) moved
  CHECK(static_cast<unsigned>(Color::SelDim) == ui::kColorCount - 1u);
}

TEST_CASE("theme: SelDim (OS-SELTINT) is a duller Sel in every theme: not Sel itself, still blue, and it holds 3:1 against the pane") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const Theme& th = built_in(t);
    const Rgb sel = th.rgb(Color::Sel);
    const Rgb dim = th.rgb(Color::SelDim);
    CHECK_MESSAGE((dim.r != sel.r || dim.g != sel.g || dim.b != sel.b), th.name << ": SelDim differs from Sel");
    CHECK_MESSAGE((dim.b > dim.r && dim.b > dim.g), th.name << ": SelDim stays blue");
    // fewer than 40 levels on any channel: a tint, not another colour
    const int dr = static_cast<int>(dim.r) - static_cast<int>(sel.r);
    const int dg = static_cast<int>(dim.g) - static_cast<int>(sel.g);
    const int db = static_cast<int>(dim.b) - static_cast<int>(sel.b);
    CHECK_MESSAGE((dr > -40 && dr < 40 && dg > -40 && dg < 40 && db > -40 && db < 40), th.name << ": SelDim is a slight shift of Sel");
  }
}

TEST_CASE("theme: orange is the cursor's alone (ADR 0001, ADR 0010 section 4)") {
  const auto is_orange = [](const Rgb& c) {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
    if (hi < 0.35 || hi - lo < 0.5 * hi) {
      return false;  // dark or grey-ish
    }
    double hue = 0;
    if (hi == r) {
      hue = 60.0 * (g - b) / (hi - lo);
    } else if (hi == g) {
      hue = 60.0 * (2.0 + (b - r) / (hi - lo));
    } else {
      hue = 60.0 * (4.0 + (r - g) / (hi - lo));
    }
    hue = hue < 0 ? hue + 360.0 : hue;
    return hue >= 12.0 && hue <= 45.0;
  };
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    const Theme& th = built_in(t);
    CHECK_MESSAGE(is_orange(th.rgb(Color::Accent)), th.name << ": the accent is orange");
    for (unsigned c = 0; c < ui::kColorCount; ++c) {
      if (static_cast<Color>(c) != Color::Accent) {
        CHECK_MESSAGE(!is_orange(th.colour[c]), th.name << ": " << token_name(static_cast<Color>(c)) << " is orange");
      }
    }
  }
}
