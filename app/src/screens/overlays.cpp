// SPDX-License-Identifier: GPL-3.0-or-later
// The three overlays of OS-002b: Learn, the Navigator and the notification strip. Colours are theme tokens; every pair
// they draw over or against each other is in ui/theme.h's kContrastPairs (the Navigator's key-caps are the Pane-on-Text and
// Sel-on-SelText rows: the Text-on-Pane and SelText-on-Sel pairs read the other way round).
#include <o3os/app/screens/overlay.h>

#include <o3os/app/frame.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/learn_list.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/pattern_source.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/text_format.h>
#include <o3os/model/params.h>
#include <o3os/ui/components.h>

#include "dialog.h"

#include <initializer_list>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

const Overlay* overlay_of(OverlayId id) noexcept {
  switch (id) {
    case OverlayId::Learn:
      return &kLearnOverlay;
    case OverlayId::Navigator:
      return &kNavigatorOverlay;
    case OverlayId::Notify:
      return &kNotifyOverlay;
    case OverlayId::PatternChooser:
      return &kPatternChooser;
    case OverlayId::LengthChooser:
      return &kLengthChooser;
    case OverlayId::JumpChooser:
      return &kJumpChooser;
    case OverlayId::ModulePicker:
      return &kModulePickerOverlay;
    case OverlayId::TextEntry:
      return &kTextEntryOverlay;
    case OverlayId::StepPopup:
      return &kStepPopupOverlay;
    case OverlayId::Fill:
      return &kFillOverlay;
    case OverlayId::SongTools:
      return &kSongToolsOverlay;
    case OverlayId::PlayMode:
      return &kPlayModeOverlay;
    case OverlayId::PadsPicker:
      return &kPadsPickerOverlay;  // OS-UXF-PAT 7: the pads' mode list
    case OverlayId::Preset:
      return &kPresetOverlay;
    case OverlayId::VerbMenu:
      return &kVerbMenuOverlay;  // OS-PTOOLS2 S3: the F7 verb menu
    case OverlayId::PatternTools:
      return &kPatternToolsOverlay;  // OS-PTOOLS2 S2: the Pattern TOOLS dialog over the block
    case OverlayId::Events:
      return &kEventsOverlay;
    case OverlayId::LearnMap:
      return &kLearnMapOverlay;  // OS-LEARNMAP
    case OverlayId::ExportProgress:
      return &kExportOverlay;
    case OverlayId::FirmwareUpdate:
      return &kFirmwareOverlay;
    case OverlayId::Help:
      return &kHelpOverlay;
    case OverlayId::None:
      break;
  }
  return nullptr;
}

// ---- Learn ---------------------------------------------------------------------------------------------------------

namespace {

constexpr int kLearnW = 632;  // the key line of eight chips (F7 ENC, OS-UXF-KEYS) is 608 px wide in the Amiga theme's Label face; 480 left the last chips on the pane. Still narrower than the Navigator (640)
constexpr int kLearnListRows = 5;                // bindings in view
constexpr int kLearnRows = 3 + kLearnListRows;   // the instruction, the target, the list, the keys

Dialog learn_dialog(const ui::Theme& theme) noexcept {
  return make_dialog(theme, kLearnW, kLearnRows * theme.metrics.row_pitch);
}

}  // namespace

Rect LearnOverlay::rect(const ui::Theme& theme) const noexcept { return learn_dialog(theme).outer; }

int LearnOverlay::count(const Model& model, const ViewState&) const noexcept { return static_cast<int>(learn_entry_count(model)); }
int LearnOverlay::page() const noexcept { return kLearnListRows; }
bool LearnOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int n = count(model, view);
  const int to = position < 0 ? 0 : (position >= n ? (n > 0 ? n - 1 : 0) : position);
  const bool moved = to != view.learn.cursor || view.learn.confirm;
  view.learn.cursor = static_cast<std::uint8_t>(to);
  view.learn.confirm = false;  // a move disarms Clear all
  return moved;
}

namespace {

// "CC 74 CH 1 PICKUP" (the channel only when the binding is for one); a follow binding ends "FOLLOW" (OS-LEARNFOLLOW 3.4: "CC 74 PICKUP FOLLOW").
void binding_text(core::FixedString<48>& line, const core::LearnBinding& b) noexcept {
  static constexpr const char* kModes[core::kLearnModeCount] = {"PICKUP", "JUMP", "RELATIVE"};
  line.assign("CC ");
  append_uint(line, b.cc);
  if (b.channel != 0) {
    line.append(" CH ");
    append_uint(line, b.channel);
  }
  line.append(" ");
  line.append(kModes[b.mode % core::kLearnModeCount]);
  if ((b.flags & core::kLearnFollow) != 0) {
    line.append(" FOLLOW");
  }
}

}  // namespace

// What owns a parameter, for the list: "INS 03", "INS *" (a follow binding), "TRK 02", "MASTER", "SETTING", and (OS-INSFX S7) "FX 03A" (the insert A of track 3), "FX MST".
void owner_text(core::FixedString<48>& line, const core::LearnBinding& b) noexcept {
  const core::ParamKind kind = core::param_kind(b.param);
  line.clear();
  if (kind == core::ParamKind::Instrument) {
    line.assign("INS ");
    if ((b.flags & core::kLearnFollow) != 0) {
      line.append("*");
    } else {
      append_dec(line, core::param_index(b.param) + 1u, 2);
    }
  } else if (kind == core::ParamKind::Track) {
    line.assign("TRK ");
    append_dec(line, core::param_index(b.param) + 1u, 2);
  } else if (kind == core::ParamKind::Master) {
    line.assign("MASTER");
  } else if (kind == core::ParamKind::Insert) {
    const unsigned where = core::param_index(b.param);
    line.assign("FX ");
    if (where >= core::kInsertIdMaster) {
      line.append("MST");
    } else {
      append_dec(line, where / 2u + 1u, 2);
      line.append((where % 2u) != 0u ? "B" : "A");
    }
  } else {
    line.assign("SETTING");
  }
}

namespace {

// The binding of a parameter, from the store its kind belongs to (the device's block or the project's), or nullptr.
const core::LearnBinding* find_binding(const Model& model, core::ParamId id) noexcept {
  const core::LearnBinding* slots = core::learn_scope(core::param_kind(id)) == core::LearnScope::Device
                                        ? model.device.learn_slots()
                                        : (model.project != nullptr ? model.project->settings().learn_slots() : nullptr);
  const int i = slots != nullptr ? core::learn_find_param(slots, core::kLearnSlots, id) : -1;
  return i >= 0 ? &slots[i] : nullptr;
}

}  // namespace

// Title, what to do, the target and what it is bound to, the list of bindings (both stores: the project's, then the device's) with the cursor's row marked, and the keys.
// The target is the parameter under the cursor of the screen below (Screen::focus_param; OS-002d), a dash when that screen has none; the dialog is the same size
// either way. F1 unbinds the target, F2 changes how its controller moves it (pickup, jump, relative), F5 toggles whether its binding follows the selected
// instrument (OS-LEARNFOLLOW); the arrows or the encoder choose a binding of the list, F3 deletes it,
// F4 clears them all (OS-013a: pressed twice, the footer asks).
void LearnOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Dialog d = learn_dialog(t);
  draw_dialog(r, d, "LEARN");
  const int x = d.body.x + 2 * m.pad;
  const int right = d.body.x + d.body.w - 2 * m.pad;
  const int pitch = m.row_pitch;
  r.text(x, ui::caps_y(t, Face::Label, d.body.y, pitch), "Move a control or send a MIDI CC.", Color::ChromeText,
         Face::Label);

  const ScreenEntry* entry = find_screen(screen_registry(), view.screen);
  const core::ParamId target = entry != nullptr && entry->screen != nullptr ? entry->screen->focus_param(model, view) : core::ParamId::None;
  ParamScratch target_scratch;
  const core::ParamDesc* desc = describe_param(model, target, target_scratch);

  const int row = d.body.y + pitch;
  const int foot = d.body.y + (kLearnRows - 1) * pitch;
  r.text(x, ui::caps_y(t, Face::Label, row, pitch), "TARGET", Color::ChromeDim, Face::Label);
  const int dash_x = x + r.text_width("TARGET", Face::Label) + 4 * m.gap;
  if (desc == nullptr) {
    const int dash_w = 2 * t.geometry(Face::Data).cell_w;
    r.panel({dash_x, row + (pitch - 2 * m.border) / 2, dash_w, 2 * m.border}, Color::ChromeText);  // the placeholder: a dash
  } else {
    // The parameter's name in the Data face, the instrument it is of beside it, and what it is bound to at the right edge.
    r.text(dash_x, ui::caps_y(t, Face::Data, row, pitch), desc->name, Color::ChromeText, Face::Data);
    const core::LearnBinding* b = find_binding(model, target);
    core::FixedString<48> line;
    if (core::param_kind(target) == core::ParamKind::Instrument) {
      line.assign("INS ");
      if (b != nullptr && (b->flags & core::kLearnFollow) != 0) {
        line.append("*");  // the binding that acts on the target follows the selected instrument (OS-LEARNFOLLOW 3.4)
      } else {
        append_dec(line, core::param_index(target) + 1u, 2);
      }
      r.text(dash_x + static_cast<int>(core::kParamNameChars) * t.geometry(Face::Data).cell_w, ui::caps_y(t, Face::Label, row, pitch), line.c_str(),
             Color::ChromeDim, Face::Label);
    }
    if (b != nullptr) {
      binding_text(line, *b);
    } else {
      line.assign("NOT BOUND");
    }
    r.text(right - r.text_width(line.c_str(), Face::Label), ui::caps_y(t, Face::Label, row, pitch), line.c_str(),
           b != nullptr ? Color::ChromeText : Color::ChromeDim, Face::Label);
  }

  // The list of bindings: a window of kLearnListRows that keeps the cursor's row in view.
  const unsigned total = learn_entry_count(model);
  const unsigned cursor = view.learn.cursor < total ? view.learn.cursor : (total != 0 ? total - 1u : 0u);
  const unsigned first = cursor < static_cast<unsigned>(kLearnListRows) ? 0u : cursor - static_cast<unsigned>(kLearnListRows) + 1u;
  const int cell = t.geometry(Face::Data).cell_w;
  if (total == 0) {
    r.text(x, ui::caps_y(t, Face::Label, row + pitch, pitch), "NO BINDINGS YET", Color::ChromeDim, Face::Label);
  }
  for (unsigned k = 0; k < static_cast<unsigned>(kLearnListRows); ++k) {
    LearnEntry e{};
    if (!learn_entry_at(model, first + k, e)) {
      break;
    }
    const int y = row + static_cast<int>(1 + k) * pitch;
    const bool sel = first + k == cursor;
    const Color text = sel ? Color::ChromeText : Color::ChromeDim;
    ParamScratch entry_scratch;
    const core::ParamDesc* pd = describe_param(model, e.binding->param, entry_scratch);
    if (sel) {
      r.text(x, ui::caps_y(t, Face::Data, y, pitch), ">", Color::ChromeText, Face::Data);
    }
    r.text(x + 2 * cell, ui::caps_y(t, Face::Data, y, pitch), pd != nullptr ? pd->name : "?", text, Face::Data);
    core::FixedString<48> line;
    owner_text(line, *e.binding);
    r.text(x + (2 + static_cast<int>(core::kParamNameChars) + 1) * cell, ui::caps_y(t, Face::Label, y, pitch), line.c_str(), Color::ChromeDim, Face::Label);
    binding_text(line, *e.binding);
    r.text(right - r.text_width(line.c_str(), Face::Label), ui::caps_y(t, Face::Label, y, pitch), line.c_str(), text, Face::Label);
  }

  const int used = draw_prompt_hints(r, x, foot, pitch, prompt_in_force(model, view), 2);  // (OS-KEYROWDLG: the keys of the dialog line are the row's)
  if (desc == nullptr && total == 0) {
    r.text(x + used + 4 * m.gap, ui::caps_y(t, Face::Label, foot, pitch), "or FX1 + FX2 again", Color::ChromeDim, Face::Label);
  }
}

constexpr LearnOverlay kLearnOverlay{};

// ---- Navigator -----------------------------------------------------------------------------------------------------

namespace {

constexpr int kNavigatorW = 640;

struct NavigatorGeometry {
  Dialog dialog;
  int rows;
  int cell_w;
  int cell_h;
  int gap;
};

NavigatorGeometry navigator_geometry(const ui::Theme& theme) noexcept {
  const ui::Metrics& m = theme.metrics;
  NavigatorGeometry g{};
  const int count = static_cast<int>(navigator_screens().count);
  g.rows = (count + kNavigatorColumns - 1) / kNavigatorColumns;
  g.gap = m.gap;
  // A cell: a line of the Data face for the icon and the name, a key-cap line for the screen button, a pad round them.
  g.cell_h = theme.geometry(Face::Data).cell_h + theme.geometry(Face::Label).cap_h + 2 * m.pad + 3 * m.pad + 2 * m.border;
  const int grid_h = g.rows * g.cell_h + (g.rows - 1) * g.gap;
  g.dialog = make_dialog(theme, kNavigatorW, grid_h + m.pad + m.row_pitch);  // the grid, then the footer row
  g.cell_w = (g.dialog.body.w - (kNavigatorColumns - 1) * g.gap) / kNavigatorColumns;
  return g;
}

Rect cell_rect(const NavigatorGeometry& g, int index) noexcept {
  const int col = index % kNavigatorColumns;
  const int row = index / kNavigatorColumns;
  return {g.dialog.body.x + col * (g.cell_w + g.gap), g.dialog.body.y + row * (g.cell_h + g.gap), g.cell_w, g.cell_h};
}

enum class Cell : std::uint8_t { Enabled, Disabled, Chosen };

}  // namespace

Rect NavigatorOverlay::rect(const ui::Theme& theme) const noexcept { return navigator_geometry(theme).dialog.outer; }

// The Navigator's cursor: the registry's cells, read row by row, a page being all of them.
int NavigatorOverlay::count(const Model&, const ViewState&) const noexcept { return static_cast<int>(navigator_screens().count); }
int NavigatorOverlay::page() const noexcept { return static_cast<int>(navigator_screens().count); }

bool NavigatorOverlay::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = count(model, view) - 1;
  const int to = position < 0 ? 0 : (position > last ? last : position);
  const bool changed = to != view.navigator;
  view.navigator = to;
  return changed;
}

// Left / right are one cell, up / down one row of kNavigatorColumns; down from a row into a shorter one lands on the last
// cell, and nothing moves past the first row or the last.
bool NavigatorOverlay::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  const int total = count(model, view);
  const int sel = view.navigator;
  if (dx != 0) {
    return set_position(model, view, sel + dx);
  }
  int to = sel + dy * kNavigatorColumns;
  if (dy == 0 || to < 0) {
    return false;
  }
  if (to > total - 1) {
    if (dy < 0 || sel / kNavigatorColumns >= (total - 1) / kNavigatorColumns) {
      return false;
    }
    to = total - 1;
  }
  return set_position(model, view, to);
}

// A grid of every registered screen, generated from the registry: a dark cell with the icon, the name and the screen
// button as a key-cap; the chosen one in the selection blue; the screens that do not exist yet in the dim ink.
void NavigatorOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const NavigatorGeometry g = navigator_geometry(t);
  const ScreenTable table = navigator_screens();
  const int count = static_cast<int>(table.count);
  draw_dialog(r, g.dialog, "SCREENS");

  const auto kind = [&](int i) {
    return i == view.navigator ? Cell::Chosen : (table.items[i].screen == nullptr ? Cell::Disabled : Cell::Enabled);
  };
  const auto fill_of = [](Cell c) { return c == Cell::Chosen ? Color::Sel : Color::Pane; };
  const auto ink_of = [](Cell c) { return c == Cell::Chosen ? Color::SelText : (c == Cell::Disabled ? Color::TextDim : Color::Text); };
  // (latent: a Disabled cell's key-cap would be Pane letters on a TextDim box, a pair the contrast table lacks; no cell is disabled today: add {Pane, TextDim} to kContrastPairs when one is)
  const auto key_ink_of = [](Cell c) { return c == Cell::Chosen ? Color::Sel : Color::Pane; };
  const int b = m.border;
  const int icon = static_cast<int>(ui::kIconSize);
  const int data_h = t.geometry(Face::Data).cell_h;
  const int key_h = t.geometry(Face::Label).cap_h + 2 * m.pad;

  // The cells' own geometry: the first line (icon, name) and the key-cap line below it.
  const auto line1 = [&](const Rect& c) { return c.y + b + m.pad; };
  const auto line2 = [&](const Rect& c) { return line1(c) + data_h + m.pad; };

  for (int i = 0; i < count; ++i) {  // pass 1: every cell's frame
    r.panel(cell_rect(g, i), Color::ChromeDim);
  }
  for (const Color fill : {Color::Pane, Color::Sel}) {  // pass 2: the fills inside the frames
    for (int i = 0; i < count; ++i) {
      const Rect c = cell_rect(g, i);
      if (fill_of(kind(i)) == fill) {
        r.panel({c.x + b, c.y + b, c.w - 2 * b, c.h - 2 * b}, fill);
      }
    }
  }
  for (const Color ink : {Color::Text, Color::TextDim, Color::SelText}) {  // pass 3: icon, name, the key-cap's box
    for (int i = 0; i < count; ++i) {
      const Cell k = kind(i);
      if (ink_of(k) != ink) {
        continue;
      }
      const Rect c = cell_rect(g, i);
      const ScreenEntry& e = table.items[i];
      r.icon(c.x + b + m.pad, line1(c) + (data_h - icon) / 2, e.icon, ink);
      r.text(c.x + b + m.pad + icon + m.pad, ui::caps_y(t, Face::Data, line1(c), data_h), e.name, ink, Face::Data);
      r.panel({c.x + b + m.pad, line2(c), r.text_width(e.key, Face::Label) + 2 * m.pad, key_h}, ink);
    }
  }
  for (const Color ink : {Color::Pane, Color::Sel}) {  // pass 4: the screen button's name, knocked out of its key-cap
    for (int i = 0; i < count; ++i) {
      if (key_ink_of(kind(i)) == ink) {
        const Rect c = cell_rect(g, i);
        r.text(c.x + b + 2 * m.pad, ui::caps_y(t, Face::Label, line2(c), key_h), table.items[i].key, ink, Face::Label);
      }
    }
  }

  // The footer (OS-NAV2): how to leave and what the chosen cell does. OS-UXF-KEYS item 5: F1 GO and F8 BACK, as in the other overlays.
  const int foot = g.dialog.body.y + g.dialog.body.h - m.row_pitch;
  const int x = g.dialog.body.x + 2 * m.pad;
  const ScreenEntry* chosen = screen_at(table, view.navigator);
  int used = draw_prompt_hints(r, x, foot, m.row_pitch, prompt_in_force(model, view), 4) + 4 * m.gap;  // CLOSE and, on a cell that is built, GO (the gap before a second key-cap, and the same after the last)
  if (chosen == nullptr || chosen->screen == nullptr) {
    r.text(x + used, ui::caps_y(t, Face::Label, foot, m.row_pitch), "NOT AVAILABLE YET", Color::ChromeText, Face::Label);
    used += r.text_width("NOT AVAILABLE YET", Face::Label) + 4 * m.gap;
  }
  r.text(x + used, ui::caps_y(t, Face::Label, foot, m.row_pitch), "arrows or encoder: choose", Color::ChromeDim, Face::Label);
  // (The F-key row is the Navigator's prompt: render_scene paints it on the frame band after the overlay, the screen under it drawing none: OS-NAV2, OS-SMALL2, OS-KEYROWDLG.)
}

constexpr NavigatorOverlay kNavigatorOverlay{};

// ---- The pattern's choosers ----------------------------------------------------------------------------------------------

Rect ChooserOverlay::rect(const ui::Theme& theme) const noexcept { return make_frame(theme, kFrameRows).info; }

// The title as an inverted chip (the notification's own look: ChromeText fill, Chrome text), the value after it in ChromeText, and what the
// keys do in ChromeDim at the right edge. PAR-092: the F3 label is JUMP, the chooser it opens says STEP JUMP.
void ChooserOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Rect band = rect(t);
  ui::strip(r, band, ui::Edge::Top);

  const PatternSource src(model, view.edit.pattern);
  const char* title = kind_ == Kind::Pattern ? "PATTERN" : (kind_ == Kind::Length ? "LENGTH" : "STEP JUMP");
  core::FixedString<48> value;
  const char* hint;
  core::FixedString<96> pattern_hint;  // the PATTERN chooser's line names the key it is the hold of: from the chooser's table, so the line cannot say another key than the row
  core::FixedString<64> clip_hint;     // PAT-OPS-B: the same line without the pick, for a value too long to leave room for it (EMPTY, LEAVES THE SONG)
  switch (kind_) {
    case Kind::Pattern: {
      // OS-PATSWITCH: the chooser is the hold of F1. Playing (past the count-in), a pick switches the playing pattern at once; a Song is left; a pattern the project lacks plays empty.
      const bool live = view.play.playing && view.play.count_in == 0u;
      append_dec(value, view.edit.pattern, view.edit.pattern >= 100 ? 3u : 2u);
      if (!src.exists() && !src.is_demo()) {
        value.append("  EMPTY");
      }
      if (live && view.play.order != audio::PlayPosition::kNoOrder) {
        value.append("  LEAVES THE SONG");
      }
      AskText held;  // "F1": the hold that opened it
      append_key_name(held, owner_prompt(OverlayId::PatternChooser)->options[0].key);
      clip_hint.assign("COPY: COPY   SHIFT+COPY: PASTE   ");  // PAT-OPS-B: the whole pattern into the clipboard and over this one
      clip_hint.append(held.c_str());
      clip_hint.append(": close");
      pattern_hint.assign(live ? "pads, encoder: switch now   " : "pads, encoder: +-1   ");
      pattern_hint.append(clip_hint.c_str());
      hint = pattern_hint.c_str();
      break;
    }
    case Kind::Length:
      append_dec(value, static_cast<unsigned>(src.length()), 3);
      value.append(" steps");
      hint = "pads 4-128   Left, Right: defined   encoder, Up, Down: +-1";
      break;
    default:
      append_dec(value, view.edit.edit_step, 2);
      value.append(" steps");
      hint = "pads 0-32   Left, Right: defined   encoder, Up, Down: +-1";
      break;
  }
  const int chip_h = t.geometry(Face::Label).cap_h + 2 * m.border;
  const int chip_y = band.y + (band.h - chip_h) / 2;
  const int text_y = ui::caps_y(t, Face::Label, band.y, band.h);
  const int chip_w = r.text_width(title, Face::Label) + 2 * m.pad;
  r.panel({2 * m.pad, chip_y, chip_w, chip_h}, Color::ChromeText);
  r.text(3 * m.pad, text_y, title, Color::Chrome, Face::Label);
  r.text(2 * m.pad + chip_w + 2 * m.pad, text_y, value.c_str(), Color::ChromeText, Face::Label);
  // What the keys do, at the right edge. LENGTH and JUMP end with F8 BACK as a key-cap (the table's; the names are on the key row), the hint before it. The PATTERN chooser (the hold of F1, OS-PATSWITCH) says
  // how it closes in its own line (release the key) and has the whole width, as before.
  if (kind_ == Kind::Pattern) {
    const int value_end = 4 * m.pad + chip_w + r.text_width(value.c_str(), Face::Label);
    if (value_end + 2 * m.pad + r.text_width(hint, Face::Label) > ui::kScreenW - 2 * m.pad) {
      hint = clip_hint.c_str();  // (the pick is the first to go: the value is the longer, as the keys of the clip and the close matter more)
    }
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(hint, Face::Label), text_y, hint, Color::ChromeDim, Face::Label);
  } else {
    const PromptState prompt = prompt_in_force(model, view);
    const int keys_w = prompt_hints_width(r, prompt, 2, true);
    const int keys_x = ui::kScreenW - 2 * m.pad - keys_w;
    (void)draw_prompt_hints(r, keys_x, band.y, band.h, prompt, 2, true);
    r.text(keys_x - 3 * m.pad - r.text_width(hint, Face::Label), text_y, hint, Color::ChromeDim, Face::Label);
  }
}

constexpr ChooserOverlay kPatternChooser{ChooserOverlay::Kind::Pattern};
constexpr ChooserOverlay kLengthChooser{ChooserOverlay::Kind::Length};
constexpr ChooserOverlay kJumpChooser{ChooserOverlay::Kind::Jump};

// ---- Notifications -------------------------------------------------------------------------------------------------

Rect NotifyOverlay::rect(const ui::Theme& theme) const noexcept { return make_frame(theme, kFrameRows).info; }

namespace {

// A message on the info line: a severity tile (a letter in a square), the text as an inverted chip like the status chip of the pattern view, the source at the right edge in the dim ink.
// Severity is the tile's fill, since the palette has no red: ChromeText for info, ChromeDim for a warning, Sel for an error. Shared by the notification strip and the strip of the hold of Power.
void message_strip(ui::Renderer& r, const Rect& band, core::Severity severity, const char* line, const char* source) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  ui::strip(r, band, ui::Edge::Top);

  const int tile = t.geometry(Face::Label).cap_h + 2 * m.border;  // the same height as the chip beside it
  const int tile_y = band.y + (band.h - tile) / 2;
  const char* letter = severity == core::Severity::Info ? "i" : (severity == core::Severity::Warn ? "!" : "X");
  const Color fill = severity == core::Severity::Info ? Color::ChromeText : (severity == core::Severity::Warn ? Color::ChromeDim : Color::Sel);
  const Color ink = severity == core::Severity::Error ? Color::SelText : Color::Chrome;
  ui::badge(r, {2 * m.pad, tile_y, tile, tile}, letter, fill, ink, Face::Label);

  const int text_y = ui::caps_y(t, Face::Label, band.y, band.h);
  const int chip_x = 2 * m.pad + tile + 2 * m.pad;
  r.panel({chip_x, tile_y, r.text_width(line, Face::Label) + 2 * m.pad, tile}, Color::ChromeText);
  r.text(chip_x + m.pad, text_y, line, Color::Chrome, Face::Label);
  r.text(ui::kScreenW - 2 * m.pad - r.text_width(source, Face::Label), text_y, source, Color::ChromeDim, Face::Label);
}

}  // namespace

// The head of the queue on the info line, which it replaces while it is shown.
void NotifyOverlay::render(const Model&, const ViewState& view, ui::Renderer& r) const noexcept {
  if (view.notify.empty()) {
    return;
  }
  const core::Notification& n = view.notify.head();
  // The same news posted again is one entry with a count: "F3 JUMP x3".
  core::FixedString<core::kNotifyTextChars + 4> line;
  line.assign(n.text.c_str());
  if (n.count > 1) {
    line.append(" x");
    append_dec(line, n.count, n.count >= 10 ? 2u : 1u);
  }
  message_strip(r, rect(r.theme()), n.severity, line.c_str(), n.source.c_str());
}

// ---- The hold of Power (OS-POWER P1) ---------------------------------------------------------------------------------------------------------

const char* power_text(core::PowerText text, bool unsaved) noexcept {
  switch (text) {
    case core::PowerText::None: break;
    case core::PowerText::Hold: return unsaved ? "UNSAVED WORK - LET GO TO KEEP IT" : "HOLD TO SWITCH OFF";
    case core::PowerText::Hint: return "HOLD POWER 2 S TO SWITCH OFF";
    case core::PowerText::Firmware: return "FIRMWARE UPDATE RUNNING";
    case core::PowerText::Export: return "EXPORT RUNNING - HOLD POWER AGAIN TO STOP IT";
    case core::PowerText::Saving: return "SAVING... THEN POWER OFF";
    case core::PowerText::SaveCancelled: return "SAVE CANCELLED - POWERING OFF";
    case core::PowerText::PoweringOff: return "POWERING OFF";
    case core::PowerText::DiskWriting: return "COMPUTER MAY BE WRITING - HOLD AGAIN";
  }
  return "";
}

void render_power(const ViewState& view, ui::Renderer& r) noexcept {
  const core::PowerShow& show = view.power.show;
  if (!show.shown()) {
    return;
  }
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  if (show.fill != 0u) {
    // The line: on the top edge of the status strip, above its badges (they start a pad and a border down), full width, the theme's pad high (4 px in every built-in theme).
    const int h = m.pad > 4 ? m.pad : 4;
    ui::bar(r, {0, 0, ui::kScreenW, h}, show.fill, 1000u, Color::Chrome, Color::Sel);
    r.panel({0, h, ui::kScreenW, m.border}, Color::ChromeDim);  // the rule that shows where the line ends
  }
  if (show.text != core::PowerText::None) {
    const bool unsaved = show.text == core::PowerText::Hold && view.power.unsaved;
    const bool warn = unsaved || show.text == core::PowerText::Firmware || show.text == core::PowerText::Export || show.text == core::PowerText::SaveCancelled ||
                      show.text == core::PowerText::DiskWriting;
    message_strip(r, make_frame(t, kFrameRows).info, warn ? core::Severity::Warn : core::Severity::Info, power_text(show.text, unsaved), "POWER");
  }
}

constexpr NotifyOverlay kNotifyOverlay{};

}  // namespace o3os::app
