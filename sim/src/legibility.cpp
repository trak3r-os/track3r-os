// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/legibility.h>

#include <o3os/ui/assets.h>
#include <o3os/ui/font.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

namespace o3os::sim {

namespace {

// ---- The allow list -----------------------------------------------------------------------------------------------------------------
//
// Empty. The sweep's findings on the gallery of 2026-10-18 were fixes in the app or in the contrast table (OS-BUG-LEGIB), and the maintainer's rule is to fix by colour and to list a finding only where a
// fix would make the screen look worse. An entry names the frames (a prefix of the gallery key), the kind, a piece of the finding's text and the reason, e.g.
//   {"gallery/mixer_none", LegibilityKind::Overlap, "'f' at (209, 445)", "OS-BUG-...: the info line meets the key hints; waits for its fix"},
// (The faux bold is not an entry: it is a rule, in check_legibility.)
const std::vector<LegibilityAllow> kAllow = {};

// The reasons of the two structural exceptions of check_legibility (rules, not list entries): see the overlap check and the clipped check.
constexpr const char* kFauxBold = "faux bold: the Label face's M or S mark of the pattern header twice in one colour, a pixel apart";
constexpr const char* kCoveredInPart = "covered in part: a later opaque panel hides some ink of a bitmap that the scissor and the screen edge leave whole (a dialog over the text beneath it)";

// ---- What a bitmap is --------------------------------------------------------------------------------------------------------------------

constexpr const char* kTokenNames[] = {"Chrome", "ChromeText", "ChromeDim", "Pane",   "Text",       "TextDim",
                                                      "Empty",  "Instr",      "Vol",       "Fx",     "Sel",        "SelText",
                                                      "Accent", "AccentText", "Play",      "PlayText", "Rec", "SelDim"};

constexpr const char* kIconNames[] = {"Play", "Stop", "Record", "Loop", "Mute", "Solo", "Lock", "Folder", "Module", "Midi", "Warning"};
// (unsized, so that a token or an icon added to ui/ without its name here fails the build instead of printing a null name)
static_assert(sizeof kTokenNames / sizeof kTokenNames[0] == ui::kColorCount, "kTokenNames: one name per colour token");
static_assert(sizeof kIconNames / sizeof kIconNames[0] == ui::kIconCount, "kIconNames: one name per icon");

std::string hex_pair(unsigned value) {
  char out[8];
  std::snprintf(out, sizeof out, "%02X", value & 0xFFu);
  return out;
}

// A short human name of the bitmap a glyph is: a face's character ("10x20 'A'"), a tile ("hex 3F", "note C-4"), an icon.
std::string describe(const GlyphDraw& g) {
  for (unsigned f = 0; f < ui::kFontCount; ++f) {
    const ui::FontFace& face = ui::font_face(static_cast<ui::Font>(f));
    if (g.source == face.ram_g_addr) {
      char out[40];
      if (g.cell < face.glyph_count) {
        std::snprintf(out, sizeof out, "%ux%u '%c'", face.cell_w, face.cell_h, static_cast<char>(face.first_char + g.cell));
      } else {
        std::snprintf(out, sizeof out, "%ux%u cell %u", face.cell_w, face.cell_h, g.cell);
      }
      return out;
    }
  }
  char note[4];
  const auto note_text = [&note](unsigned cell) {
    ui::note_tile_text(cell, note);
    return std::string(note);
  };
  const std::uint32_t source = g.source;
  if (source == ui::bitmap_set(ui::Tiles::HexLo).ram_g) return "hex " + hex_pair(g.cell);
  if (source == ui::bitmap_set(ui::Tiles::HexHi).ram_g) return "hex " + hex_pair(ui::kHexTilesPerHandle + g.cell);
  if (source == ui::bitmap_set(ui::Tiles::Notes).ram_g) return "note " + note_text(g.cell);
  if (source == ui::notes_hi_set().ram_g) return "note " + note_text(ui::kNoteTilesPerHandle + g.cell);
  if (source == ui::bitmap_set(ui::Tiles::Icons).ram_g) return std::string("icon ") + (g.cell < ui::kIconCount ? kIconNames[g.cell] : "?");
  if (source == ui::small_bitmap_set(ui::SmallTiles::LabelHex).ram_g) return "8x16 hex " + hex_pair(g.cell);
  if (source == ui::small_bitmap_set(ui::SmallTiles::LabelNotes).ram_g) return "8x16 note " + note_text(g.cell);
  if (source == ui::small_bitmap_set(ui::SmallTiles::MicroNotes).ram_g) return "6x12 note " + note_text(g.cell);
  if (source == ui::small_bitmap_set(ui::SmallTiles::MicroFx).ram_g) {
    char fx[4];
    ui::fx_tile_text(g.cell, fx);
    return std::string("6x12 fx ") + fx;
  }
  char out[40];
  std::snprintf(out, sizeof out, "bitmap %u cell %u", static_cast<unsigned>(g.handle), g.cell);
  return out;
}

std::string at(const GlyphDraw& g) {
  char out[40];
  std::snprintf(out, sizeof out, " at (%d, %d)", g.ink_x0, g.ink_y0);
  return out;
}

// ---- Colours ---------------------------------------------------------------------------------------------------------------------------

std::uint32_t pack(unsigned r, unsigned g, unsigned b) { return (r << 16) | (g << 8) | b; }

// The tokens of `theme` that have this colour (several can share one: a pair is judged for any of them), as a bit set.
std::uint32_t tokens_of(const ui::Theme& theme, std::uint32_t rgb) {
  std::uint32_t set = 0;
  for (unsigned t = 0; t < ui::kColorCount; ++t) {
    const ui::Rgb& c = theme.colour[t];
    if (pack(c.r, c.g, c.b) == rgb) {
      set |= 1u << t;
    }
  }
  return set;
}

// The Label face's M and S: the pattern header's mute and solo marks, which app/src/pattern_view.cpp draws twice, a pixel apart, while Shift is held.
bool is_label_ms(const ui::Theme& theme, const GlyphDraw& g) {
  const ui::FontFace& label = ui::font_face(theme.font(ui::Face::Label));
  const unsigned ch = g.cell + label.first_char;
  return g.source == label.ram_g_addr && (ch == static_cast<unsigned>('M') || ch == static_cast<unsigned>('S'));
}

std::string name_of(std::uint32_t tokens, std::uint32_t rgb) {
  std::string out;
  for (unsigned t = 0; t < ui::kColorCount; ++t) {
    if ((tokens >> t) & 1u) {
      out += (out.empty() ? "" : "/") + std::string(kTokenNames[t]);
    }
  }
  if (out.empty()) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "#%06X", rgb);
    out = hex;
  }
  return out;
}

bool in_table(std::uint32_t fg, std::uint32_t bg) {
  for (unsigned i = 0; i < ui::kContrastPairCount; ++i) {
    const ui::ContrastPair& p = ui::kContrastPairs[i];
    if (((fg >> static_cast<unsigned>(p.fg)) & 1u) && ((bg >> static_cast<unsigned>(p.bg)) & 1u)) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* legibility_kind_name(LegibilityKind kind) noexcept {
  switch (kind) {
    case LegibilityKind::Clipped:
      return "clipped";
    case LegibilityKind::Overlap:
      return "overlap";
    case LegibilityKind::Contrast:
      return "contrast";
  }
  return "?";
}

const std::vector<LegibilityAllow>& legibility_allow_list() { return kAllow; }

std::vector<LegibilityFinding> check_legibility(const EveRaster& raster, const ui::Theme& theme, const std::string& shot,
                                                const std::vector<LegibilityAllow>& allow) {
  std::vector<LegibilityFinding> out;
  const std::vector<GlyphDraw>& log = raster.glyph_log();
  const std::vector<GlyphInk>& ink = raster.glyph_ink();
  const auto add = [&](LegibilityKind kind, const std::string& what, const char* reason = nullptr) {
    LegibilityFinding f;
    f.kind = kind;
    f.shot = shot;
    f.what = what;
    f.allowed = reason;
    for (const LegibilityAllow& a : allow) {
      if (f.allowed == nullptr && a.kind == kind && shot.compare(0, std::char_traits<char>::length(a.shot), a.shot) == 0 &&
          what.find(a.what) != std::string::npos) {
        f.allowed = a.reason;
      }
    }
    out.push_back(std::move(f));
  };

  // Overlap and contrast, in one pass over the bitmaps. What the rasteriser logged for each ink pixel says which bitmap's ink it lay on (`under`: -1 if
  // a panel was drawn over it in between, or nothing) and, per picture pixel, which bitmap's ink is on top now (ink_owner). Ink that a later panel or
  // bitmap covered is not on the picture: it is left out of every check. Bitmap B overlaps A when B's ink, on the picture, lies on A's ink; those
  // pixels are not B's contrast (the overlap is the finding), every other pixel is: the colour under it and B's colour must make a pair of the table.
  const std::vector<std::int32_t>& top = raster.ink_owner();
  std::map<std::pair<std::size_t, std::size_t>, unsigned> shared;
  std::vector<std::pair<std::size_t, std::map<std::uint32_t, unsigned>>> backdrops;  // per bitmap: the colours under its ink off the glyph-on-glyph pixels
  std::vector<unsigned> on_picture_count(log.size(), 0);                             // per bitmap: its ink pixels that show on the picture
  std::vector<unsigned> under_panel_count(log.size(), 0);                            // per bitmap: its ink pixels that a later opaque fill covered
  for (std::size_t i = 0; i < log.size(); ++i) {
    const GlyphDraw& b = log[i];
    std::map<std::uint32_t, unsigned> under;
    for (std::uint32_t k = 0; k < b.ink_visible; ++k) {
      const GlyphInk& p = ink[b.ink_first + k];
      if (top[p.pixel] != static_cast<std::int32_t>(i)) {
        under_panel_count[i] += top[p.pixel] < 0 ? 1u : 0u;  // (a later bitmap's ink on it is the overlap check's)
        continue;
      }
      ++on_picture_count[i];
      if (p.under >= 0 && static_cast<std::size_t>(p.under) != i) {
        ++shared[{static_cast<std::size_t>(p.under), i}];
      } else {
        ++under[pack(p.r, p.g, p.b)];
      }
    }
    if (!under.empty()) {
      backdrops.push_back({i, std::move(under)});
    }
  }
  // Clipped: ink the scissor or the screen edge cuts (the findings of a kind are in draw order; overlap and contrast follow).
  for (std::size_t i = 0; i < log.size(); ++i) {
    const GlyphDraw& g = log[i];
    if (g.ink_visible == g.ink_total || (g.ink_visible != 0 && on_picture_count[i] == 0)) {
      // All of it inside, or a bitmap that a later panel hides whole (a bitmap cut away whole, ink_visible 0, is the clipped finding at its worst). A bitmap that
      // a later panel hides in part is not a finding either, but it is counted and named in the run (a dialog edge through a line of the screen below it).
      if (g.ink_visible == g.ink_total && on_picture_count[i] != 0 && under_panel_count[i] != 0) {
        char part[96];
        std::snprintf(part, sizeof part, " %u of %u ink pixels under a later panel", under_panel_count[i], g.ink_total);
        add(LegibilityKind::Clipped, describe(g) + at(g) + ":" + part, kCoveredInPart);
      }
      continue;
    }
    const bool off_screen = g.ink_x0 < 0 || g.ink_y0 < 0 || g.ink_x1 > EveRaster::kWidth || g.ink_y1 > EveRaster::kHeight;
    char by[96];
    if (off_screen) {
      std::snprintf(by, sizeof by, "the screen edge");
    } else {
      std::snprintf(by, sizeof by, "the scissor (%u, %u, %u x %u)", g.scissor_x, g.scissor_y, g.scissor_w, g.scissor_h);
    }
    char what[200];
    std::snprintf(what, sizeof what, " %u of %u ink pixels cut by %s", g.ink_total - g.ink_visible, g.ink_total, by);
    add(LegibilityKind::Clipped, describe(g) + at(g) + ":" + what);
  }

  for (const auto& entry : shared) {
    const GlyphDraw& a = log[entry.first.first];
    const GlyphDraw& b = log[entry.first.second];
    char what[48];
    std::snprintf(what, sizeof what, " share %u pixels", entry.second);
    // The one design that is an overlap on purpose: a faux bold, the same bitmap in the same colour twice, one pixel apart on one line, and only the Label
    // face's M and S (the pattern header's marks while Shift is held, app/src/pattern_view.cpp). It is counted and named in the run, not listed.
    const bool bold = a.source == b.source && a.cell == b.cell && a.r == b.r && a.g == b.g && a.b == b.b && a.y == b.y && std::abs(a.x - b.x) == 1 &&
                      is_label_ms(theme, a);
    add(LegibilityKind::Overlap, describe(a) + at(a) + " and " + describe(b) + at(b) + what, bold ? kFauxBold : nullptr);
  }

  // Contrast: the ink's colour on what it was drawn on. A backdrop under a quarter or less of the ink does not decide (the edge of a block).
  for (const auto& entry : backdrops) {
    const GlyphDraw& g = log[entry.first];
    unsigned total = 0;
    for (const auto& b : entry.second) {
      total += b.second;
    }
    const std::uint32_t fg_rgb = pack(g.r, g.g, g.b);
    const std::uint32_t fg = tokens_of(theme, fg_rgb);
    for (const auto& b : entry.second) {
      if (4u * b.second <= total) {
        continue;
      }
      const std::uint32_t bg = tokens_of(theme, b.first);
      if (g.a == 255 && in_table(fg, bg)) {
        continue;
      }
      std::string what = describe(g) + at(g) + ": " + name_of(fg, fg_rgb);
      if (g.a != 255) {
        what += " at alpha " + std::to_string(g.a);
      }
      add(LegibilityKind::Contrast, what + " on " + name_of(bg, b.first) + " is not a pair of the contrast table");
    }
  }
  return out;
}

}  // namespace o3os::sim
