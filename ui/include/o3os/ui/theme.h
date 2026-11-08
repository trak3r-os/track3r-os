// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Theme tokens (ADR 0010 section 8, amendment 1, docs/ui/style-guide.md): a theme is 16 colours and Rec, the four faces,
// spacing and borders. Three are compiled in and always exist (standalone first, ADR 0009): Amiga (the default:
// Workbench grey-blue chrome, dark data pane, one strong blue selected row, orange only for the cursor), Black
// (high contrast, for the stage) and White (for daylight). SD theme packs will fill the same fixed struct, limited to
// declared knobs, and pass the same contrast check before they are accepted.
//
// The colour values of the built-in themes are written in ui/src/themes.cpp and nowhere else (tools/check_layers.py):
// screens name a ui::Color, and ask the theme for metrics.

#include <o3os/ui/font.h>
#include <o3os/ui/style.h>

#include <cstddef>
#include <cstdint>

namespace o3os::ui {

// Spacing and borders, in pixels. A screen's layout is derived from these and from the faces' geometry; it holds no
// literals of its own (app/pattern_layout.h).
struct Metrics {
  std::uint8_t border;     // thickness of a rule or border
  std::uint8_t pad;        // padding inside a chrome strip or a grid cell
  std::uint8_t gap;        // between fields of a row
  std::uint8_t row_pitch;  // pitch of a grid row; the Data face's cell sits inside it (20 px in 24)
  std::uint8_t strip_h;    // the top strip: Display face values
  std::uint8_t header_h;   // the track header row: type badge, name, meter, selection underline
  std::uint8_t info_h;     // the info line above the key row
  std::uint8_t keys_h;     // the key row
  std::uint8_t index_w;    // the row-number column
  std::uint8_t meter_h;    // the track meter
};

struct Theme {
  const char* name;  // lower case, [a-z0-9_]: scenario `theme <name>`, gallery file names, golden keys
  Rgb colour[kColorCount];
  Font face[kFaceCount];  // which compiled face plays each role, indexed by Face
  Metrics metrics;

  constexpr const Rgb& rgb(Color c) const noexcept { return colour[static_cast<unsigned>(c)]; }
  constexpr Font font(Face f) const noexcept { return face[static_cast<unsigned>(f)]; }
  constexpr FontGeometry geometry(Face f) const noexcept { return font_geometry(font(f)); }
};

enum class ThemeId : std::uint8_t { Amiga, Black, White };
inline constexpr unsigned kThemeCount = 3;

// The built-in themes. An id out of range gives Amiga.
const Theme& theme(ThemeId id) noexcept;
constexpr ThemeId theme_id_at(unsigned index) noexcept {
  return index < kThemeCount ? static_cast<ThemeId>(index) : ThemeId::Amiga;
}
// The theme after `id`, wrapping: the order of `ui.theme.next`.
constexpr ThemeId next_theme(ThemeId id) noexcept {
  return theme_id_at((static_cast<unsigned>(id) + 1u) % kThemeCount);
}
// By name, case-insensitive; false (out untouched) if there is none.
bool find_theme(const char* name, std::size_t length, ThemeId& out) noexcept;

// The look of the boot screen (OS-003f): the first thing the device shows is the same in every theme, so it is drawn with this
// one fixed table, not the stored one. It is the Black theme's (a black pane, white and cool grey text), which the contrast
// test already holds to its pairs; it is not selectable and not listed above.
const Theme& boot_theme() noexcept;

// What each pair of tokens is drawn as, and so the contrast it must reach (WCAG 2.x relative luminance ratio):
//   Text   data and labels: at least 4.5:1
//   Large  Display-face text and non-text indicators (the selection, the cursor, a meter): at least 3:1
//   Dim    decorative, kept visible but quiet: about 2:1 (empty fields and the track separators, ADR 0010 section 4)
// Every theme, built in or from the SD, must meet every pair (tests/theme_test.cpp). The table lists exactly the
// token pairs that the components and the screens draw over, or directly against, each other, and says where; adding
// a use of two tokens adds the pair, and a theme that cannot meet it is retuned or the drawing is changed (the cursor's
// outline and the PLAY badge's and the meter's frames exist for that reason).
enum class PairRole : std::uint8_t { Text, Large, Dim };

struct ContrastPair {
  Color fg;  // the ink: text, an indicator, a fill drawn over bg
  Color bg;  // what it is drawn on
  PairRole role;
};

inline constexpr float kMinTextContrast = 4.5f;
inline constexpr float kMinLargeContrast = 3.0f;
inline constexpr float kDimContrastLow = 1.6f;   // "about 2:1": neither lost against the pane...
inline constexpr float kDimContrastHigh = 3.0f;  // ...nor as loud as data

inline constexpr ContrastPair kContrastPairs[] = {
    // Chrome (strips, track headers, info line, key row).
    // Display, Data and Label text on every chrome band; a type badge, a key-cap box or the message chip (ChromeText
    // fill) against the band they sit on.
    {Color::ChromeText, Color::Chrome, PairRole::Text},
    // Captions and track names, the group dividers, the rule at the edge of a band, the frame of the PLAY badge and of
    // each meter slot (where the green badge or the dark slot would otherwise melt into the band); the STOP badge's fill.
    {Color::ChromeDim, Color::Chrome, PairRole::Text},
    // The letter of a type badge, the F-number in a key-cap box and the message chip's text, knocked out of ChromeText.
    {Color::Chrome, Color::ChromeText, PairRole::Text},
    // The word and the icon on the STOP badge.
    {Color::Chrome, Color::ChromeDim, PairRole::Text},
    // The selected track's two-pixel underline against the header it closes.
    {Color::Sel, Color::Chrome, PairRole::Large},

    // The data pane (step grid, row numbers, rules).
    // Notes, the beat row numbers (Text); the other row numbers and the bar rules (TextDim); the instrument, volume
    // and FX columns; every glyph of the grid on an unselected row, and the peak tick of a meter on its slot.
    {Color::Text, Color::Pane, PairRole::Text},
    {Color::TextDim, Color::Pane, PairRole::Text},
    {Color::Instr, Color::Pane, PairRole::Text},
    {Color::Vol, Color::Pane, PairRole::Text},
    {Color::Fx, Color::Pane, PairRole::Text},
    // The inverse chips on the pane: Pane letters knocked out of a Text fill (the Shift tags MUTE and SOLO over a track's dimmed rows, the Navigator's key-cap of a cell that is not chosen).
    {Color::Pane, Color::Text, PairRole::Text},
    // Empty fields and the track separators.
    {Color::Empty, Color::Pane, PairRole::Dim},
    // A meter's level against its slot (the slot is Pane; the peak tick is Text on Pane, above).
    {Color::Play, Color::Pane, PairRole::Large},
    // The record-arm icon of the style specimen on the pane (on the strip it would be Accent on Chrome); the dragged handle of a
    // curve, and a clip (a wave's tinted band, a meter's lamp) on the plots' pane (OS-003d: the one orange, no token added).
    {Color::Accent, Color::Pane, PairRole::Large},

    // The selected row and the cursor.
    // The selected row's bar against the pane above and below it.
    {Color::Sel, Color::Pane, PairRole::Large},
    // All the text of the selected row.
    {Color::SelText, Color::Sel, PairRole::Text},
    // The Navigator's chosen cell: its key-cap is a SelText box with the screen button's name knocked out of it in Sel.
    {Color::Sel, Color::SelText, PairRole::Text},
    // The cursor is an Accent block with a one-pixel AccentText outline: the block's text against the block (and the strip's mode word, EDIT / REC / STOPPED, on an Accent chip), and the
    // outline against the selected row it is drawn on (Accent itself is not 3:1 against Sel in any theme, and the
    // outline is what makes the cursor an indicator).
    {Color::AccentText, Color::Accent, PairRole::Text},
    {Color::AccentText, Color::Sel, PairRole::Large},

    // Transport.
    // The word and the icon on the PLAY badge (its frame is ChromeDim on Chrome, above).
    {Color::PlayText, Color::Play, PairRole::Text},

    // Rec mode (the Pattern screen in Edit while it plays, OS-REVIEW): the red frame round the grid against the pane, the REC badge's
    // fill and the playhead's bar against the chrome strip, and SelText (white in every theme) on the red of the badge and the bar.
    {Color::Rec, Color::Pane, PairRole::Large},
    {Color::Rec, Color::Chrome, PairRole::Large},
    {Color::SelText, Color::Rec, PairRole::Text},

    // A multi-track block (OS-SELTINT): the tracks other than the cursor's are tinted SelDim, a little duller than Sel (a desaturated blue, not a darker one, so that it still holds
    // 3:1 against the pane in every theme); the text of those tracks is SelText.
    {Color::SelDim, Color::Pane, PairRole::Large},
    {Color::SelText, Color::SelDim, PairRole::Text},
};
inline constexpr unsigned kContrastPairCount = sizeof(kContrastPairs) / sizeof(kContrastPairs[0]);

}  // namespace o3os::ui
