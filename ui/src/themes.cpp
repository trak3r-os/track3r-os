// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in themes: the only place colour values are written (tools/check_layers.py). Every pair of tokens that a
// component or screen draws over each other is listed in theme.h (kContrastPairs) and checked for each theme by
// tests/theme_test.cpp; change a value here and run that test (and look at the gallery, docs/ui/style-guide.md).
#include <o3os/ui/theme.h>

namespace o3os::ui {

namespace {

// Spacing and borders shared by the built-in themes (a theme may differ within the declared knobs). 24 px row pitch
// with the 20 px Data cell inside it (ADR 0010 section 4); the chrome bands add up with the grid to the 480 px panel
// (app/pattern_layout.h asserts it).
constexpr Metrics kMetrics = {
    1,   // border
    4,   // pad
    4,   // gap
    24,  // row_pitch
    32,  // strip_h
    26,  // header_h
    16,  // info_h
    22,  // keys_h
    30,  // index_w
    4,   // meter_h
};

constexpr Font kFaces[kFaceCount] = {Font::F16x32, Font::F10x20, Font::F8x16, Font::F6x12};

// Amiga (the default): Workbench grey-blue chrome with dark text, a near-black data pane with a faint blue cast, one
// strong blue for the selected row, orange only for the cursor (and record-arm), a single green for PLAY and meters, and the one red (Rec) for Rec mode.
// Soft column hues keep the grid scannable; none is orange.
constexpr Theme kAmiga = {
    "amiga",
    {
        {178, 186, 202},  // Chrome
        {14, 18, 28},     // ChromeText
        {60, 70, 96},     // ChromeDim
        {10, 13, 20},     // Pane
        {230, 234, 242},  // Text
        {128, 136, 156},  // TextDim
        {62, 68, 84},     // Empty
        {126, 196, 240},  // Instr
        {150, 214, 132},  // Vol
        {212, 166, 238},  // Fx
        {28, 92, 208},    // Sel
        {255, 255, 255},  // SelText
        {255, 138, 0},    // Accent
        {14, 14, 18},     // AccentText
        {84, 200, 92},    // Play
        {10, 24, 12},     // PlayText
        {190, 30, 30},    // Rec
        {44, 92, 184},    // SelDim
    },
    {kFaces[0], kFaces[1], kFaces[2], kFaces[3]},
    kMetrics,
};

// Black: high contrast, for the stage. A black pane and white text; chrome is a dark blue-grey, just lifted off it.
constexpr Theme kBlack = {
    "black",
    {
        {38, 41, 50},     // Chrome
        {236, 238, 244},  // ChromeText
        {146, 152, 168},  // ChromeDim
        {0, 0, 0},        // Pane
        {255, 255, 255},  // Text
        {150, 156, 172},  // TextDim
        {70, 74, 86},     // Empty
        {110, 190, 255},  // Instr
        {130, 236, 120},  // Vol
        {230, 150, 255},  // Fx
        {36, 108, 238},   // Sel
        {255, 255, 255},  // SelText
        {255, 150, 20},   // Accent
        {0, 0, 0},        // AccentText
        {70, 220, 90},    // Play
        {0, 20, 0},       // PlayText
        {220, 52, 52},    // Rec
        {44, 98, 200},    // SelDim
    },
    {kFaces[0], kFaces[1], kFaces[2], kFaces[3]},
    kMetrics,
};

// White: for daylight. Light chrome and a near-white pane with dark text; the hues and the orange are darkened until
// they read on white (the contrast test holds them to it).
constexpr Theme kWhite = {
    "white",
    {
        {212, 217, 228},  // Chrome
        {16, 20, 30},     // ChromeText
        {84, 92, 112},    // ChromeDim
        {250, 250, 248},  // Pane
        {16, 20, 30},     // Text
        {96, 104, 124},   // TextDim
        {176, 180, 190},  // Empty
        {10, 92, 170},    // Instr
        {20, 120, 40},    // Vol
        {130, 50, 170},   // Fx
        {36, 92, 200},    // Sel
        {255, 255, 255},  // SelText
        {190, 80, 0},     // Accent
        {255, 255, 255},  // AccentText
        {28, 128, 52},    // Play
        {255, 255, 255},  // PlayText
        {196, 32, 32},    // Rec
        {56, 100, 188},   // SelDim
    },
    {kFaces[0], kFaces[1], kFaces[2], kFaces[3]},
    kMetrics,
};

constexpr const Theme* kThemes[kThemeCount] = {&kAmiga, &kBlack, &kWhite};

bool equals_lower(const char* theme_name, const char* s, std::size_t length) noexcept {
  for (std::size_t i = 0; i < length; ++i) {
    const char c = (s[i] >= 'A' && s[i] <= 'Z') ? static_cast<char>(s[i] + ('a' - 'A')) : s[i];
    if (theme_name[i] == '\0' || theme_name[i] != c) {  // never reads past the terminator
      return false;
    }
  }
  return theme_name[length] == '\0';
}

}  // namespace

const Theme& theme(ThemeId id) noexcept { return *kThemes[static_cast<unsigned>(theme_id_at(static_cast<unsigned>(id)))]; }

const Theme& boot_theme() noexcept { return kBlack; }

bool find_theme(const char* name, std::size_t length, ThemeId& out) noexcept {
  for (unsigned i = 0; i < kThemeCount; ++i) {
    if (equals_lower(kThemes[i]->name, name, length)) {
      out = static_cast<ThemeId>(i);
      return true;
    }
  }
  return false;
}

}  // namespace o3os::ui
