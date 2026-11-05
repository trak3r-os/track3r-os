// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/app/screens/screen.h>
#include <o3os/core/fixed_string.h>

namespace o3os::ui::plot {
struct CurveScratch;
}  // namespace o3os::ui::plot

namespace o3os::app {

// F1..F8 of the parameter page (OS-INSTUI D1, S1): F1 the source page of a sample instrument (its label follows the instrument; S3), or MIDI for a MIDI instrument, whose one page holds CHANNEL
// (S8a: F2..F5 are dim there and say why until S8b gives F2 the CC page),
// F2..F5 VOICE / FILTER / MOD / SENDS (MOD, S6: a SOURCE row Off / Envelope / LFO per destination), F6 / F7 the previous / next instrument (the labels show the neighbours, `<06` and `08>`, and are dimmed at the ends),
// F8 UNDO. The key row draws F1, F6 and F7 itself; the table is the placeholder text (and what the binding test compares the Shift row with).
// A module instrument's F1..F4 are the module's pages (1-8, 9-16, ...), F5 is not used. An EMPTY slot's F1 opens the module picker until S5's type chooser replaces it.
inline constexpr const char* kParamsSoftKeys[8] = {"SAMPLE", "VOICE", "FILTER", "MOD", "SENDS", "<INS", "INS>", "UNDO"};
// While Shift is held: F1..F5 NAME / COPY / PASTE / DELETE / SAVE (S5, S7; a key that does not apply is drawn dim and says why when pressed), F6 / F7 the previous / next instrument IN USE
// (the labels show their numbers; dimmed when there is none), F8 REDO.
inline constexpr const char* kParamsShiftKeys[8] = {"NAME", "COPY", "PASTE", "DELETE", "SAVE", "<USED", "USED>", "REDO"};

// The key row as the screen draws it (OS-INSTUI D1): the eight labels (F1, F6 and F7 follow the instrument; two of them are numbers in `number`, so the struct is filled in place and not copied)
// and the bit mask of the keys drawn dim (bit n = F(n + 1)): a key that does nothing here or waits for its slice.
struct ParamsKeyRow {
  const char* label[8];
  char number[2][4];
  std::uint32_t dim;
};
void params_key_row(const Model& model, const ViewState& view, ParamsKeyRow& out) noexcept;
// The header's engine field for the sample instrument in `slot`: its own choice (MODERN, AMIGA, 12-BIT, 8-BIT) or, when PLAY ENGINE is Auto, what Auto resolved to (AUTO>AMIGA).
void params_engine_text(const Model& model, unsigned slot, const model::Instrument& in, core::FixedString<16>& out) noexcept;

// The text of a row of the source page for the instrument in `slot` as the page shows it (the window size as the engine plays it, the grain's position in seconds, the selected slice `n of m`, the
// others as the descriptor formats them): the Wave screen's WAVETABLE and GRANULAR pages read the same words (OS-INSTUI S4).
void params_source_text(const Model& model, unsigned slot, unsigned field, char (&out)[core::kParamTextChars + 1]) noexcept;
// OS-INSTUI S6, the MOD page: why row `row` (0 = SOURCE) of the destination in view is dimmed, or nullptr (O8: the row stays, the footer says why): "WAVETABLE ONLY", "GRANULAR ONLY", "FILTER OFF" for a
// destination the instrument does not use (every row), "SOURCE OFF: ..." for the rows below a SOURCE that is Off. And the rows the page shows for it, as field ids (SOURCE first, then the LFO's three or the envelope's five).
const char* params_mod_row_why(const Model& model, const ViewState& view, unsigned row) noexcept;
// OS-AUDIT E-INST-05, the FILTER page: "FILTER OFF" for the cutoff and resonance rows while the filter is off (the rows stay, dimmed), else nullptr.
const char* params_filter_row_why(const Model& model, const ViewState& view, unsigned row) noexcept;
unsigned params_mod_rows(const Model& model, const ViewState& view, std::uint16_t (&fields)[ParamView::kMaxRows]) noexcept;
// Clips `text` (the strip's play mode / engine words) so that a field starting at `row_x` ends before the CPU / MEM readout.
void params_how_clip(ui::Renderer& r, core::FixedString<40>& text, int row_x) noexcept;

// OS-INSTUI S1: the instrument in view, as the key row and the page keys see it.
enum class SlotKind : std::uint8_t { Empty, Sample, Midi, Module };
SlotKind params_slot_kind(const Model& model, const ViewState& view) noexcept;
// The MIDI channel the chooser gives a new MIDI instrument in 0-based slot `slot` (OS-INSTMIDI Q-IM-3): 1 for the slots 1..48, slot - 48 for 49..64.
constexpr unsigned midi_channel_for_slot(unsigned slot) noexcept { return slot >= 48u ? slot - 47u : 1u; }
// The instrument (1..64) before (`dir` < 0) or after (`dir` > 0) `from` that holds something (a sample, a MIDI instrument or a module), or 0 when there is none that way.
int instrument_in_use(const Model& model, int from, int dir) noexcept;

// The one working memory of the plots (ui::plot::CurveScratch, 4.5 KB of the main loop's): the pages that draw a curve share it, since a screen is drawn from one context only.
// The module views' curves use it too (app/module_ui.cpp).
ui::plot::CurveScratch& params_curve_scratch() noexcept;

// "Instrument PARAMS" (OS-002d, docs/design/product-ux.md section 4): the parameters of the selected instrument, generated from the
// descriptor tables, not written parameter by parameter. A page is one group of the tables (VOICE, FILTER, ENV, SENDS); its rows are the
// descriptors of that group in table order: the name, the value as the descriptor formats it, the unit, a bar that follows the descriptor's
// curve and, if a MIDI controller is bound to it, its number. A card on the right says everything the descriptor knows about the row under
// the cursor (range, default, steps, flags, binding). The FILTER page draws the filter's response and the ENV page the envelope's shape as
// ui::plot mini-curves, from the values. A parameter added to the tables appears on its page without a line here.
//
//   Up / Down (the Cursor) a row
//   encoder          param.edit: the fine step        Shift + encoder  param.coarse
//   Delete           param.reset: the default         F1..F5   the page (instrument.page)   PARAMS again   the first page
//   Left / Right     the previous / next page, wrapping (the ENV page: the envelope first, then the page)       F6 / F7   the previous / next instrument; Shift + F6 / F7  in use
//   a pad            plays the instrument (S2)        Shift + a pad   selects it without a sound
//   FX1 + FX2        Learn: the next CC binds to the row under the cursor
//
// A MODULE instrument (an SD module bound to the slot, OS-012b) has the same page made from the module's own descriptors: eight parameters a page (F2.. the pages), the
// standard controls with Learn and undo, and below them the module's declared views (its filter curve, say) drawn through the restricted draw API.
//
// Every edit is an action, so it is undoable (F8) and goes to the audio side as a ParamChange. Without a project the page shows the
// descriptors' defaults and says so. The instrument is ViewState::instrument (a pad selects it).
class ParamsScreen final : public Screen {
 public:
  constexpr ParamsScreen() noexcept = default;
  void enter(const Model& model, ViewState& view) const noexcept override;
  void render(const Model& model, const ViewState& view, ui::Renderer& r, ui::Rect covered) const noexcept override;
  ScreenId id() const noexcept override { return ScreenId::Instrument; }
  Context context() const noexcept override { return Context::Instrument; }
  unsigned pages() const noexcept override;
  const char* const* soft_keys() const noexcept override { return kParamsSoftKeys; }
  const char* const* soft_keys_shifted() const noexcept override { return kParamsShiftKeys; }
  core::ParamId focus_param(const Model& model, const ViewState& view) const noexcept override;
  // The cursor: the rows of the page (their number depends on it), `page` rows a page; the second axis is the envelope of the ENV page.
  int count(const Model& model, const ViewState& view) const noexcept override;
  int position(const ViewState& view) const noexcept override;
  int page() const noexcept override { return ParamView::kMaxRows; }
  bool set_position(const Model& model, ViewState& view, int position) const noexcept override;
  bool step(const Model& model, ViewState& view, int dx, int dy) const noexcept override;
  bool track(const Model& model, ViewState& view, int delta) const noexcept override;
  // OS-INSTUI S1: the Params button again goes to F1 (the first page that exists: a module's first, else VOICE until S3's source page).
  bool repress(const Model& model, ViewState& view) const noexcept override;
  // Left / Right (nav.left / nav.right, and instrument.turn): the ENV page's envelopes first, then the page; `wrap` false stops at the ends (an auto-repeat).
  bool turn(const Model& model, ViewState& view, int dir, bool wrap) const noexcept;
  // The page that Left / Right (dir -1 / +1) turn to, wrapping over the pages of the instrument in view.
  // `wrap` false (an auto-repeat) stops at the first / last page instead of going round.
  bool turn_page(const Model& model, ViewState& view, int dir, bool wrap = true) const noexcept;
  // OS-INSTUI-ARROWS: the F-key `key` (0..4) pressed again on the page it already shows steps the windows of its group, wrapping: a sample instrument's MOD key (F4) the six envelopes. False where the
  // group has one window (nothing moves; a module has four pages at most, O3M_PARAMS_MAX / 8).
  bool next_window(const Model& model, ViewState& view, unsigned key) const noexcept;
  // The pages a module instrument has (its parameters, eight to a page; 0 for a module with no parameters: no page key, F1 does nothing, OS-MODPAGEKEYS follow-up), and the pages of the other kinds (ip::kGroups).
  unsigned module_page_count(const Model& model, const ViewState& view) const noexcept;
  bool pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept override;
  // OS-INSTUI S5: an empty slot of an open project shows the type chooser (F1 SAMPLE, F2 MIDI, F3 PASTE, F4 MODULE, F5 LOAD): the screen's field level is context instrument.empty there.
  Context field_context(const Model& model, const ViewState& view) const noexcept override;
};
extern const ParamsScreen kParamsScreen;

}  // namespace o3os::app
