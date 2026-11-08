// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/screens.h>

#include "screens_app.h"

#include <o3os/app/app.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/pattern_view.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/view_state.h>
#include <o3os/sim/plot_screens.h>
#include <o3os/ui/boot_screen.h>
#include <o3os/ui/components.h>
#include <o3os/usb/identity.h>

namespace o3os::sim {

namespace {

using app::PatternData;

// ---- The pattern view ---------------------------------------------------------------------------------------------

// Every pattern state is drawn through the app's own scene (app::render_scene: the base screen of a ViewState over a Model, the overlay slot
// over it), so what the budget measures and the gallery shows is what the app draws. The two data sets below fill the demo mode (no
// document: the Pattern screen's read-only picture); the document-backed states are in screens_pattern.cpp.

const PatternData& demo_data() {
  static const PatternData data = [] {
    PatternData d;
    app::fill_demo(d);
    return d;
  }();
  return data;
}

// What the grid shows in the worst case. A note is one tile word whatever it says and an FX is a type glyph and a tile
// (two words), but an instrument or a volume is ONE tile word when it has a value and TWO glyph words ("--" or "??")
// when it has none or an invalid one. So there are two contents to try: every field filled, and the most expensive
// mix: a note and both FX filled, the instrument and the volume empty. OS-009 added the third: that mix with every step's note drawn from the
// tiles that cost something more: the notes past the 128th tile (C10 and above, a second bitmap handle, bound once a frame and switched to
// in the fallback), alternating from row to row with the plain ones, fade, cut and off (which show no instrument, so the same two glyph
// words), each one a different cell. The sweep draws all three.
constexpr unsigned kDataSets = kPatternWorstDataSets;

const PatternData& worst_data(unsigned set) {
  static const PatternData filled = [] {
    PatternData d;
    for (auto& track : d.steps) {
      for (app::Step& s : track) {
        s.note = 95;   // A#7
        s.instr = 47;  // "47"
        s.vol = 0x5A;
        s.fx[0] = {35, 0x9F};  // 'Z'
        s.fx[1] = {34, 0x8E};  // 'Y'
      }
    }
    return d;
  }();
  static const PatternData sparse = [] {
    PatternData d = filled;
    for (auto& track : d.steps) {
      for (app::Step& s : track) {
        s.instr = 0;                   // "--": two glyph words instead of one tile
        s.vol = model::kVolumeUnset;  // "--"
      }
    }
    return d;
  }();
  static const PatternData tiles = [] {
    PatternData d = sparse;
    static constexpr std::uint8_t kPlain[5] = {95, model::kNoteFade, model::kNoteCut, model::kNoteOff, 1};
    static constexpr std::uint8_t kHigh[5] = {124, 125, 126, 127, 128};  // the five notes drawn with the second note-tile handle
    for (int t = 0; t < app::kTracks; ++t) {
      for (int row = 0; row < app::kSteps; ++row) {
        const unsigned k = static_cast<unsigned>(row / 2 + t);
        d.steps[t][row].note = row % 2 == 0 ? kHigh[k % 5] : kPlain[k % 5];
      }
    }
    return d;
  }();
  return set == 0 ? filled : (set == 1 ? sparse : tiles);
}

// A model of the demo mode (no document) showing `data`.
const app::Model& demo_mode_model(unsigned set) {
  static const app::Model models[1 + kDataSets] = {
      [] {
        app::Model m{};
        m.pattern = demo_data();
        return m;
      }(),
      [] {
        app::Model m{};
        m.pattern = worst_data(0);
        return m;
      }(),
      [] {
        app::Model m{};
        m.pattern = worst_data(1);
        return m;
      }(),
      [] {
        app::Model m{};
        m.pattern = worst_data(2);
        return m;
      }()};
  return models[set < 1 + kDataSets ? set : 0];
}

void draw_scene(ui::Renderer& r, const app::Model& model, const app::ViewState& view) { app::render_scene(model, view, r); }

// What the app shows first: the demo pattern, cursor at the start, instrument 1, Browse.
void draw_pattern_typical(ui::Renderer& r, unsigned) {
  const app::ViewState view;
  draw_scene(r, demo_mode_model(0), view);
}

// The cursor on the last step of the last track, in its last column: the window is scrolled to both far corners.
void draw_pattern_cursor_end(ui::Renderer& r, unsigned) {
  app::ViewState view;
  view.pattern.scroll_rows(app::kSteps);
  for (int i = 0; i < (app::kTracks - 1) * app::kColumns + (app::kColumns - 1); ++i) {
    view.pattern.move_right();
  }
  draw_scene(r, demo_mode_model(0), view);
}

// The sweep behind the worst case. The status strip shows the instrument number and name, so every instrument (and
// 0, "none") is tried at the first, a middle and the last track window, on each content (part 1); then every horizontal
// window position and several cursor rows (the highlighted row draws in other colours), with the instruments whose names
// are longest (part 2). Part 3 (screens_pattern.cpp) is the same screen over a document, with the header marks, the Shift
// layer and the selected block. (Until OS-002b the info line also carried a message chip, and the sweep tried every message the
// line could carry; the app says things with notifications now, which the overlay states sweep.)
constexpr unsigned kInstrumentSweep = static_cast<unsigned>(app::kInstruments) + 1;
constexpr unsigned kPart1 = kDataSets * kInstrumentSweep * 3;
constexpr unsigned kPart2 = kDataSets * static_cast<unsigned>(app::kTracks) * 5 * 2;
constexpr unsigned kDemoVariants = kPart1 + kPart2;
constexpr unsigned kWorstVariants = kDemoVariants + kPatternDocWorstVariants;

void draw_pattern_worst(ui::Renderer& r, unsigned variant) {
  if (variant >= kDemoVariants) {
    draw_pattern_doc_worst(r, variant - kDemoVariants);
    return;
  }
  static constexpr int kPart1Tracks[3] = {0, 6, 11};
  static constexpr int kPart2Rows[5] = {0, 5, 15, 40, 63};
  int track, row, instrument;
  unsigned data;
  if (variant < kPart1) {
    unsigned v = variant;
    track = kPart1Tracks[v % 3];
    v /= 3;
    instrument = static_cast<int>(v % kInstrumentSweep);
    data = v / kInstrumentSweep;
    row = 5;
  } else {
    unsigned u = variant - kPart1;
    instrument = u % 2 == 0 ? 2 : 9;  // "SNARE", "SYNTH"
    u /= 2;
    row = kPart2Rows[u % 5];
    u /= 5;
    track = static_cast<int>(u % static_cast<unsigned>(app::kTracks));
    data = u / static_cast<unsigned>(app::kTracks);
  }
  app::ViewState view;
  view.pattern.scroll_rows(row);
  for (int i = 0; i < track * app::kColumns; ++i) {
    view.pattern.move_right();
  }
  view.instrument = instrument;
  draw_scene(r, demo_mode_model(1 + data), view);
}

constexpr ScreenState kPatternStates[] = {
    {"typical", false, 1, 0, draw_pattern_typical},
    {"worst", true, kWorstVariants, kBudgetWords, draw_pattern_worst},
    {"cursor_end", false, 1, 0, draw_pattern_cursor_end},
    // OS-009 P1a.1 (screens_pattern.cpp): the document-backed states.
    {"edit_on", true, 1, kBudgetWords, draw_pattern_edit_on},
    {"selection_worst", true, kPatternSelectionVariants, kBudgetWords, draw_pattern_selection_worst},
    {"shift_layer", true, 1, kBudgetWords, draw_pattern_shift_layer},
    {"zoom8", true, kPatternZoomVariants, kBudgetWords, draw_pattern_zoom8},    // OS-ZOOM: Note + Instr, 8 tracks of Note, Instr and Vol in the Label face
    {"zoom12", true, kPatternZoomVariants, kBudgetWords, draw_pattern_zoom12},  // ... and 12 tracks of the Note column in the Micro face
    {"rec", true, 4, kBudgetWords, draw_pattern_rec},  // OS-REVIEW: Rec mode (Edit while it plays); variants 1, 2: Edit stopped, Play in Browse; 3: free Edit (OS-LIVEREC, OS-RECSWAP)
};

// ---- The style specimen -------------------------------------------------------------------------------------------
//
// One page that shows what docs/ui/style-guide.md describes, in the theme it is drawn in: the type scale with the four
// faces, the colour tokens, the tile faces and the icon face, and the components on chrome. It is how a theme
// is judged (the gallery draws it in every theme) and how a face, an icon or a token is checked at a glance.

struct TokenName {
  ui::Color color;
  const char* name;
};
constexpr TokenName kTokenNames[] = {
    {ui::Color::Chrome, "Chrome"},     {ui::Color::ChromeText, "ChromeText"}, {ui::Color::ChromeDim, "ChromeDim"},
    {ui::Color::Pane, "Pane"},         {ui::Color::Text, "Text"},             {ui::Color::TextDim, "TextDim"},
    {ui::Color::Empty, "Empty"},       {ui::Color::Instr, "Instr"},           {ui::Color::Vol, "Vol"},
    {ui::Color::Fx, "Fx"},             {ui::Color::Sel, "Sel"},               {ui::Color::SelText, "SelText"},
    {ui::Color::Accent, "Accent"},     {ui::Color::AccentText, "AccentText"}, {ui::Color::Play, "Play"},
    {ui::Color::PlayText, "PlayText"},   {ui::Color::Rec, "Rec"},       {ui::Color::SelDim, "SelDim"},
};
static_assert(sizeof(kTokenNames) / sizeof(kTokenNames[0]) == ui::kColorCount, "every token is named");

void draw_style_specimen(ui::Renderer& r, unsigned) {
  using ui::Color;
  using ui::Face;
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int left = 2 * m.pad;
  const int note_x = 320;

  // A chrome title strip with the theme's name.
  ui::strip(r, {0, 0, ui::kScreenW, m.strip_h}, ui::Edge::Bottom);
  const int baseline = (m.strip_h + t.geometry(Face::Display).cap_h) / 2;
  ui::FieldRow title(r, left, baseline);
  title.put(Face::Display, "STYLE", Color::ChromeText);
  title.skip(2 * m.pad);
  title.put(Face::Label, "o3 specimen, theme", Color::ChromeDim);
  title.skip(m.gap);
  title.put(Face::Data, t.name, Color::ChromeText);

  // The type scale: a sample in each face, and what it is for.
  int y = m.strip_h + 2 * m.pad;
  r.text(left, y, "TYPE SCALE", Color::TextDim, Face::Label);
  y += 20;
  r.text(left, y, "128.0 03/12", Color::Text, Face::Display);
  r.text(note_x, y + 8, "Display 16x32, caps 20: BPM, pattern number, edited value", Color::TextDim, Face::Label);
  y += 40;
  r.text(left, y, "C-4 01 7F R40 a01", Color::Text, Face::Data);
  r.text(note_x, y + 2, "Data 10x20, caps 13: the grid, lists, parameters", Color::TextDim, Face::Label);
  y += 28;
  r.text(left, y, "PATTERN LENGTH JUMP", Color::Text, Face::Label);
  r.text(note_x, y, "Label 8x16, caps 10: labels", Color::TextDim, Face::Label);
  y += 22;
  r.text(left, y, "0   64   128   192   256", Color::Text, Face::Micro);
  r.text(note_x, y - 2, "Micro 6x12, caps 8: axis ticks only", Color::TextDim, Face::Label);

  // The tokens, five columns of four (the 17th, Rec, starts the fifth): a swatch and its name.
  y += 30;
  r.text(left, y, "COLOUR TOKENS", Color::TextDim, Face::Label);
  y += 20;
  const int col_w = (ui::kScreenW - 2 * left) / 5;
  for (unsigned i = 0; i < ui::kColorCount; ++i) {
    const int x = left + static_cast<int>(i / 4) * col_w;
    const int sy = y + static_cast<int>(i % 4) * 26;
    r.panel({x, sy, 44, 20}, kTokenNames[i].color);
    r.frame({x, sy, 44, 20}, Color::Empty);
    r.text(x + 52, sy + 2, kTokenNames[i].name, Color::Text, Face::Label);
  }

  // The tile faces and the icon face.
  y += 4 * 26 + 8;
  r.text(left, y, "TILES AND ICONS", Color::TextDim, Face::Label);
  y += 20;
  const unsigned hex[] = {0x00, 0x3F, 0x7F, 0x80, 0xBF, 0xFF};
  for (unsigned i = 0; i < 6; ++i) {
    r.hex_tile(left + static_cast<int>(i) * 30, y, hex[i], i < 3 ? Color::Instr : Color::Vol);
  }
  // The notes, then the tiles that are not notes (none, off, "???", cut, fade) and the two ends of the second handle's cells (C10, G10).
  const unsigned notes[] = {1, 14, 61, 120, 0, ui::kNoteTileOff, ui::kNoteTileUnknown, ui::kNoteTileCut, ui::kNoteTileFade, ui::kNoteTileOctave10, ui::kNoteTiles - 1};
  for (unsigned i = 0; i < 11; ++i) {
    r.note_tile(left + 190 + static_cast<int>(i) * 40, y, notes[i], i < 4 || i >= 9 ? Color::Text : Color::Empty);
  }
  y += 28;
  for (unsigned i = 0; i < ui::kIconCount; ++i) {
    r.icon(left + static_cast<int>(i) * 28, y, static_cast<ui::Icon>(i), Color::Text);
  }
  r.icon(left + 11 * 28 + 12, y, ui::Icon::Record, Color::Accent);
  r.text(left + 11 * 28 + 34, y, "record-arm", Color::Accent, Face::Label);

  // The components, on a chrome band.
  const int band_y = ui::kScreenH - 64;
  ui::strip(r, {0, band_y, ui::kScreenW, ui::kScreenH - band_y}, ui::Edge::Top);
  const int row1 = band_y + 8;
  int x = left;
  x += ui::transport_badge(r, x, row1, 22, true) + m.gap * 2;
  x += ui::transport_badge(r, x, row1, 22, false) + m.gap * 2;
  const char kinds[] = {'A', 'M', 'E'};
  for (const char kind : kinds) {
    ui::type_badge(r, x, row1 + 4, 14, kind);
    x += 14 + m.gap;
  }
  x += m.gap * 2;
  const unsigned levels[] = {40, 160, 255};
  for (const unsigned level : levels) {
    ui::meter(r, {x, row1 + 8, 90, m.meter_h + 2 * m.border}, level, level + (level < 255 ? 20u : 0u), 255,
              Color::ChromeDim, Color::Pane, Color::Play, Color::Text);
    x += 90 + m.gap * 2;
  }
  const int chip_w = r.text_width("F3 JUMP", Face::Label) + 2 * m.pad;
  r.panel({x, row1 + 3, chip_w, 16}, Color::ChromeText);
  r.text(x + m.pad, ui::caps_y(t, Face::Label, row1 + 3, 16), "F3 JUMP", Color::Chrome, Face::Label);
  static const char* const kKeys[8] = {"PATTERN", "LENGTH", "JUMP", "FILL", "PREVIEW", "INVERT", "EXPORT", "UNDO"};
  ui::soft_keys(r, {0, ui::kScreenH - 26, ui::kScreenW, 26}, kKeys, 8);
}

constexpr ScreenState kStyleStates[] = {
    {"specimen", true, 1, kBudgetWords, draw_style_specimen},
};

// ---- The boot screen (OS-003f) ------------------------------------------------------------------------------------------
//
// The first frame the device shows: the maintainer's logo and a line under it, drawn with ui::boot_theme() in every theme's slot
// (fixed_theme), so the gallery's three pictures are one and the golden checks that. The build id is a placeholder of the
// length the platform's has (a 12-digit hash).
void draw_boot(ui::Renderer& r, unsigned) { ui::draw_boot_screen(r, usb::kProductString, "0123456789ab"); }

constexpr ScreenState kBootStates[] = {
    {"logo", false, 1, 0, draw_boot, true, ui::boot_theme},
};

constexpr Screen kScreens[] = {
    {"pattern", kPatternStates, sizeof(kPatternStates) / sizeof(kPatternStates[0])},
    {"style", kStyleStates, sizeof(kStyleStates) / sizeof(kStyleStates[0])},
    {"plot", kPlotStates, kPlotStateCount},  // OS-003d: the plot gallery, sim/plot_screens.cpp
    // OS-002b: the Song screen and the overlays (screens_app.cpp), drawn through the app's registry and overlay slot.
    {"song", kSongStates, sizeof(kSongStates) / sizeof(kSongStates[0])},
    {"overlay", kOverlayStates, sizeof(kOverlayStates) / sizeof(kOverlayStates[0])},
    {"file", kFileStates, sizeof(kFileStates) / sizeof(kFileStates[0])},  // OS-010a: the File screen (screens_file.cpp)
    {"export", kExportStates, sizeof(kExportStates) / sizeof(kExportStates[0])},  // OS-WAVEXP: the export progress overlay (screens_file.cpp)
    {"sampleload", kSampleLoadStates, sizeof(kSampleLoadStates) / sizeof(kSampleLoadStates[0])},  // OS-011a: the Sample load screen (screens_sample.cpp)
    {"wave", kWaveStates, sizeof(kWaveStates) / sizeof(kWaveStates[0])},  // OS-011a: the Wave / Chop screen (screens_sample.cpp)
    {"sampleedit", kSampleEditStates, sizeof(kSampleEditStates) / sizeof(kSampleEditStates[0])},  // OS-011a: the Sample edit screen (screens_sample.cpp)
    {"config", kConfigStates, sizeof(kConfigStates) / sizeof(kConfigStates[0])},  // OS-011a: the Config screen (screens_sample.cpp)
    {"mixer", kMixerStates, sizeof(kMixerStates) / sizeof(kMixerStates[0])},  // OS-011b: the Mixer screen (screens_mixer.cpp)
    {"perform", kPerformStates, sizeof(kPerformStates) / sizeof(kPerformStates[0])},  // OS-011b: the Perform screen (screens_mixer.cpp)
    {"record", kRecordStates, sizeof(kRecordStates) / sizeof(kRecordStates[0])},  // OS-011c: the Record screen (screens_record.cpp)
    {"inputtest", kInputTestStates, sizeof(kInputTestStates) / sizeof(kInputTestStates[0])},  // OS-007d: the hidden diagnostic
    {"source", kSourceStates, sizeof(kSourceStates) / sizeof(kSourceStates[0])},  // OS-INSTUI S3: the Params F1 source page per play mode (screens_sample.cpp)
    {"params", kParamsStates, sizeof(kParamsStates) / sizeof(kParamsStates[0])},  // OS-002d: the generated parameter page (screens_params.cpp)
    {"modules", kModulesStates, sizeof(kModulesStates) / sizeof(kModulesStates[0])},  // OS-012b: the SD modules' pages (screens_modules.cpp)
    {"extreme", kExtremeStates, sizeof(kExtremeStates) / sizeof(kExtremeStates[0])},  // OS-E2E-5: every limit of the model at once (screens_legibility.cpp)
    {"boot", kBootStates, sizeof(kBootStates) / sizeof(kBootStates[0])},  // OS-003f: the boot logo
};

}  // namespace

// The pattern screen's worst-case content, for the overlays measured over it (screens_app.cpp).
const app::PatternData& pattern_worst_data(unsigned set) { return worst_data(set); }

const Screen* screens() noexcept { return kScreens; }
unsigned screen_count() noexcept { return sizeof(kScreens) / sizeof(kScreens[0]); }

}  // namespace o3os::sim
