// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/params_screen.h>

#include "../sample_text.h"

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/governor_watch.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/pad_lights.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/text_format.h>
#include <o3os/app/wave_math.h>
#include <o3os/audio/lfo_sine.h>
#include <o3os/audio/voices.h>
#include <o3os/core/fast_math.h>
#include <o3os/core/fixed_string.h>
#include <o3os/model/params.h>
#include <o3os/ui/components.h>
#include <o3os/ui/plot/curve.h>
#include <o3os/ui/plot/wave_view.h>

#include <cstring>
#include <initializer_list>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace ip = model::instrument_param;
namespace plot = ui::plot;

namespace {

// ---- what the page shows, from the tables and the view -------------------------------------------------------------

struct Rows {
  std::uint16_t field[ParamView::kMaxRows];
  unsigned n;
};

// OS-INSTUI S3: the keys F1..F5 and Left / Right walk the groups in this order (the SOURCE group was appended to the tables, so its number is the last; its key is the first).
constexpr unsigned kKeyOrder[ip::kGroups] = {ip::GroupSource, ip::GroupVoice, ip::GroupFilter, ip::GroupEnvelope, ip::GroupSends};

unsigned key_of_group(unsigned group) noexcept {
  for (unsigned k = 0; k < ip::kGroups; ++k) {
    if (kKeyOrder[k] == group) {
      return k;
    }
  }
  return 0;
}

unsigned slot_of(const ViewState& view) noexcept;

// OS-INSTUI S8a / S8b: the pages of a MIDI instrument are no groups of a sample instrument's tables (CHANNEL is the virtual field ip::MidiChannel, the rest are the rows appended for INST 1.3): F1 MIDI
// (ip::GroupMidi) and F2 CC (ip::GroupCc). group_of answers one of them for a MIDI instrument: the CC page when the view holds it, else MIDI (the page a neighbouring sample instrument was left on is not
// meaningful here, and a MIDI instrument's page left in the view reads as VOICE on a sample instrument).
constexpr unsigned kMidiGroup = ip::GroupMidi;
constexpr unsigned kCcGroup = ip::GroupCc;
constexpr bool is_midi_group(unsigned group) noexcept { return group == kMidiGroup || group == kCcGroup; }

// The page in view as a group of the tables. The source page belongs to a sample instrument: on a free slot or a module (which the page was left on by F6 / F7) the page is VOICE; a MIDI instrument has
// its own page.
unsigned group_of(const Model& m, const ViewState& view) noexcept {
  const model::Instrument* in = m.project != nullptr ? m.project->instrument(slot_of(view)) : nullptr;
  const unsigned p = view.page[screen_index(ScreenId::Instrument)];
  if (in != nullptr && in->kind == model::InstrumentKind::Midi) {
    return p == kCcGroup ? kCcGroup : kMidiGroup;
  }
  if (p >= ip::kGroups) {
    return 0u;
  }
  if (p == ip::GroupSource) {
    return in != nullptr && in->kind == model::InstrumentKind::Sample ? p : 0u;
  }
  return p;
}

constexpr unsigned kEnvWtPos = 3;    // the envelope order (model::envelope_name): amp, pan, cutoff, wavetable position, granular position, finetune
constexpr unsigned kEnvGranPos = 4;
constexpr unsigned kEnvCutoff = 2;

unsigned envelope_of(const ViewState& view) noexcept {
  return view.params.env < 0 ? 0u : (view.params.env >= static_cast<int>(ip::kEnvelopes) ? ip::kEnvelopes - 1u : static_cast<unsigned>(view.params.env));
}

unsigned slot_of(const ViewState& view) noexcept {
  return view.instrument < 1 ? 0u : (view.instrument > static_cast<int>(model::kMaxInstruments) ? static_cast<unsigned>(model::kMaxInstruments) - 1u
                                                                                                  : static_cast<unsigned>(view.instrument - 1));
}

// The instrument of the slot in view is an SD module's (OS-012b): its page is made from the module's own descriptors, eight to a page, and its views are drawn below.
bool is_module_slot(const Model& m, const ViewState& view) noexcept {
  const model::Instrument* in = m.project != nullptr ? m.project->instrument(slot_of(view)) : nullptr;
  return in != nullptr && in->kind == model::InstrumentKind::Module;
}

unsigned module_params(const Model& m, const ViewState& view) noexcept {
  return m.modules != nullptr && m.project != nullptr ? m.modules->param_count(*m.project, slot_of(view)) : 0u;
}

// The pages of a module's parameters in view: the ones it names (a page table, ABI 1.8, OS-INSTUI S10) or eight to a page. 0 for a module with no parameters.
unsigned module_page_total(const Model& m, const ViewState& view) noexcept {
  const unsigned table = m.modules != nullptr && m.project != nullptr ? m.modules->page_table_count(*m.project, slot_of(view)) : 0u;
  return table != 0u ? table : (module_params(m, view) + ParamView::kMaxRows - 1) / ParamView::kMaxRows;
}

// The page of a module's parameters in view: the one the screen's page says, held to the pages the module has.
unsigned module_page(const Model& m, const ViewState& view) noexcept {
  const unsigned pages = module_page_total(m, view);
  const unsigned p = view.page[screen_index(ScreenId::Instrument)];
  return pages == 0u ? 0u : (p < pages ? p : pages - 1u);
}

// Page k of the module in view: its first parameter, how many it holds and its name (null: the numbered page "1-8", "9-16" ...).
struct ModPage {
  unsigned first, count;
  const char* name;
};
ModPage module_page_at(const Model& m, const ViewState& view, unsigned k) noexcept {
  ModPage out{k * ParamView::kMaxRows, 0, nullptr};
  const unsigned count = module_params(m, view);
  const char* name = nullptr;
  unsigned first = 0, n = 0;
  if (m.modules != nullptr && m.project != nullptr && m.modules->page_at(*m.project, slot_of(view), k, name, first, n)) {
    out.first = first;
    out.count = first < count ? (n < count - first ? n : count - first) : 0u;
    out.name = name;
    return out;
  }
  out.count = count > out.first ? (count - out.first < ParamView::kMaxRows ? count - out.first : ParamView::kMaxRows) : 0u;
  return out;
}

// OS-INSTUI S5: the slot in view is empty in an open project: the page is the type chooser.
bool is_chooser(const Model& m, const ViewState& view) noexcept { return m.project != nullptr && m.project->instrument(slot_of(view)) == nullptr; }

// OS-INSTUI S6: what a destination's SOURCE is (ip::SourceOff / SourceEnvelope / SourceLfo) for the instrument `in` (the stored enable and loop bytes: model::param_load, the one decode); Off without one.
unsigned mod_source_of(const model::Instrument* in, unsigned e) noexcept {
  if (in == nullptr || e >= ip::kEnvelopes) {
    return ip::SourceOff;
  }
  model::ParamField f;
  if (!model::find_param(core::param_id(core::ParamKind::Instrument, 0, ip::mod(e, ip::ModSource)), f)) {
    return ip::SourceOff;
  }
  return static_cast<unsigned>(model::param_load(in, f) + 0.5f);
}

// Why destination `e` (0..5, the envelope order) of `in` is not used right now, or nullptr when it is.
const char* mod_dest_why(const model::Instrument* in, unsigned e) noexcept {
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return nullptr;
  }
  if (e == kEnvWtPos && in->sampler.play_mode != kPlayModeWavetable) {
    return "WAVETABLE ONLY";
  }
  if (e == kEnvGranPos && in->sampler.play_mode != kPlayModeGranular) {
    return "GRANULAR ONLY";
  }
  if (e == kEnvCutoff && in->sampler.filter_enable == 0u) {
    return "FILTER OFF";
  }
  return nullptr;
}

// Why a FILTER page row is dimmed (OS-AUDIT E-INST-05): the cutoff and the resonance do nothing while the filter is off. The ENABLE and TYPE rows stay lit (they turn it on). nullptr when the row applies.
const char* filter_row_why(const model::Instrument* in, unsigned field) noexcept {
  if (in != nullptr && in->kind == model::InstrumentKind::Sample && in->sampler.filter_enable == 0u && (field == ip::Cutoff || field == ip::Resonance)) {
    return "FILTER OFF";
  }
  return nullptr;
}

// Why row `row` (0 = SOURCE) of destination `e` is dimmed, or nullptr: the destination is not used (every row, SOURCE included: never hidden), or its SOURCE is Off (the rows below it).
const char* mod_row_why(const model::Instrument* in, unsigned e, unsigned row) noexcept {
  const char* why = mod_dest_why(in, e);
  if (why == nullptr && row != 0u && in != nullptr && in->kind == model::InstrumentKind::Sample && mod_source_of(in, e) == ip::SourceOff) {
    why = "SOURCE OFF: PICK ENVELOPE OR LFO";
  }
  return why;
}

Rows rows_of(const Model& m, const ViewState& view) noexcept {
  Rows r{};
  if (is_module_slot(m, view)) {
    const ModPage page = module_page_at(m, view, module_page(m, view));
    r.n = page.count;
    for (unsigned i = 0; i < r.n; ++i) {
      r.field[i] = static_cast<std::uint16_t>(ip::kModuleFirst + page.first + i);
    }
    return r;
  }
  if (is_chooser(m, view)) {
    return r;  // the type chooser has no rows
  }
  const unsigned group = group_of(m, view);
  if (is_midi_group(group)) {
    // OS-INSTUI S8b: F1 MIDI is CHANNEL (virtual), VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET; F2 CC is CC A .. CC F (the tables list them)
    if (group == kMidiGroup) {
      r.field[r.n++] = ip::MidiChannel;
    }
    r.n += model::param_group_fields(core::ParamKind::Instrument, group, 0, r.field + r.n, ParamView::kMaxRows - r.n);
    return r;
  }
  if (group == ip::GroupSource) {
    // OS-INSTUI O8, the exception of the source page: its rows follow the play mode (PLAY MODE first, then the mode's rows, then PLAY ENGINE), so that it never passes eight.
    const model::Instrument* in = m.project != nullptr ? m.project->instrument(slot_of(view)) : nullptr;
    const unsigned mode = in != nullptr ? in->sampler.play_mode : 0u;
    r.field[r.n++] = ip::PlayMode;
    if (mode == kPlayModeWavetable) {
      r.field[r.n++] = ip::WtPos;
      r.field[r.n++] = ip::WtSize;
    } else if (mode == kPlayModeGranular) {
      r.field[r.n++] = ip::GrainPos;
      r.field[r.n++] = ip::GrainLength;
      r.field[r.n++] = ip::GrainShape;
      r.field[r.n++] = ip::GrainLoop;
    } else if (play_mode_is_slice(mode)) {
      r.field[r.n++] = ip::Slices;
      r.field[r.n++] = ip::Slice;
    }
    r.field[r.n++] = ip::PlayEngine;
    r.field[r.n++] = ip::PlayStereo;  // OS-STEREO step 2 (INST 1.5): after PLAY ENGINE, so the source page is at most seven rows (granular: PLAY MODE, four rows, ENGINE, STEREO)
    return r;
  }
  if (group == ip::GroupEnvelope) {
    // OS-INSTUI S6, the MOD page: a destination's SOURCE row, then the rows of what the source is: LFO (SHAPE, SPEED, AMOUNT), else the envelope (AMOUNT .. RELEASE; shown dimmed while the source is Off). The
    // ENABLE switch the tables still hold is what SOURCE sets (Off = not enabled), so the page does not list it. At most six rows.
    const unsigned e = envelope_of(view);
    const model::Instrument* in = m.project != nullptr ? m.project->instrument(slot_of(view)) : nullptr;
    r.field[r.n++] = static_cast<std::uint16_t>(ip::mod(e, ip::ModSource));
    if (mod_source_of(in, e) == ip::SourceLfo) {
      for (unsigned f = ip::LfoShape; f < ip::kModFields; ++f) {
        r.field[r.n++] = static_cast<std::uint16_t>(ip::mod(e, f));
      }
    } else {
      for (unsigned f = ip::EnvAmount; f < ip::kEnvelopeFields; ++f) {
        r.field[r.n++] = static_cast<std::uint16_t>(ip::envelope(e, f));
      }
    }
    return r;
  }
  r.n = model::param_group_fields(core::ParamKind::Instrument, group, envelope_of(view), r.field, ParamView::kMaxRows);
  return r;
}

unsigned row_of(const ViewState& view, const Rows& rows) noexcept {
  const unsigned row = view.params.row < 0 ? 0u : static_cast<unsigned>(view.params.row);
  return rows.n == 0 ? 0u : (row >= rows.n ? rows.n - 1u : row);
}

core::ParamId id_of_slot(unsigned slot, unsigned field) noexcept { return core::param_id(core::ParamKind::Instrument, slot, field); }

core::ParamId id_of(const ViewState& view, unsigned field) noexcept { return id_of_slot(slot_of(view), field); }

// The value of a parameter as the project has it; the descriptor's default without a project or an instrument.
float value_of(const Model& m, core::ParamId id, const core::ParamDesc& d, bool& live) noexcept {
  if (is_midi_channel_param(id)) {  // OS-INSTUI S8a: the virtual field reads the instrument
    const model::Instrument* in = m.project != nullptr ? m.project->instrument(core::param_index(id)) : nullptr;
    live = in != nullptr && in->kind == model::InstrumentKind::Midi;
    return live ? static_cast<float>(in->midi_channel) : d.def;
  }
  if (is_play_stereo_param(id)) {  // OS-STEREO step 2: the virtual STEREO field reads the instrument
    const model::Instrument* in = m.project != nullptr ? m.project->instrument(core::param_index(id)) : nullptr;
    live = in != nullptr && in->kind == model::InstrumentKind::Sample;
    return live ? static_cast<float>(in->stereo) : d.def;
  }
  if (m.project != nullptr && m.project->has_param(id)) {
    live = true;
    return m.project->get_param(id);
  }
  live = false;
  return d.def;
}

// ---- layout, from the theme -----------------------------------------------------------------------------------------------

struct ParamsLayout {
  Frame frame;
  PatternLayout grid;  // the cell and cursor offsets of the grid rows: the page's rows sit on the same pitch
  int name_x;
  int value_x;         // the value is right-aligned to value_x + value_w
  int value_w;
  int unit_x;
  int bar_x;
  int bar_w;
  int bar_h;
  int tag_x;
  int card_x;
  int card_w;
  int plot_x;          // the curve's data area: left edge and width (its top depends on the rows above it)
  int plot_w;
  bool fits;
};

ParamsLayout make_params_layout(const ui::Theme& theme) noexcept {
  const ui::Metrics& m = theme.metrics;
  const int cell = theme.geometry(Face::Data).cell_w;
  const int label = theme.geometry(Face::Label).cell_w;
  ParamsLayout l{};
  l.frame = make_frame(theme, kFrameRows);
  l.grid = make_pattern_layout(theme, kFrameRows, 5);
  l.name_x = 2 * m.pad;
  l.value_x = l.name_x + static_cast<int>(core::kParamNameChars) * cell + 2 * m.gap;
  l.value_w = 8 * cell;
  l.unit_x = l.value_x + l.value_w + 2 * m.gap;
  l.bar_x = l.unit_x + 4 * label + 2 * m.gap;
  l.bar_w = 160;
  l.bar_h = 2 * m.meter_h;
  l.tag_x = l.bar_x + l.bar_w + 2 * m.gap;
  l.card_x = l.tag_x + 6 * label + 4 * m.gap;
  l.card_w = ui::kScreenW - l.card_x - 2 * m.pad;
  l.plot_x = 40;  // room for the y axis labels (plot::draw_axes: about 24 px to the left of the data area)
  l.plot_w = l.card_x - l.plot_x - 2 * m.pad - 4 * m.gap;
  l.fits = l.tag_x + 6 * label <= l.card_x && l.card_w >= 20 * cell && l.grid.fits;
  return l;
}

constexpr Color ink_of(bool selected, Color normal) noexcept { return selected ? Color::SelText : normal; }

// ---- the mini-curves ------------------------------------------------------------------------------------------------------

// The plots' one working memory (4.5 KB of the main loop's): the FILTER and ENV pages never draw in the same frame, so they share it. A screen is
// drawn from one context only (the main loop), which is what makes a static legal here. OS-SRAMU: on the MK66 image (O3OS_CURVE_SCRATCH_EXTERNAL, set by
// app/CMakeLists.txt) the platform owns it, in the SDRAM slack window (SRAM_U keeps its 32 KiB free floor); it defines params_curve_scratch().
#ifndef O3OS_CURVE_SCRATCH_EXTERNAL
plot::CurveScratch g_scratch;
#endif

constexpr unsigned kFilterPoints = 48;
constexpr float kFilterLoHz = 20.0f;
constexpr float kFilterHiHz = 16000.0f;

// The squared magnitude of this OS's 2-pole state variable filter (the Chamberlin form of the Teensy audio library's filter_variable) at
// x = f / f0 for resonance q: low-pass 1, high-pass x^4, band-pass (x / q)^2, all over (1 - x^2)^2 + (x / q)^2.
float svf_mag2(unsigned type, float x, float q) noexcept {
  const float x2 = x * x;
  const float d = (1.0f - x2) * (1.0f - x2) + x2 / (q * q);
  const float num = type == 1 ? x2 * x2 : (type == 2 ? x2 / (q * q) : 1.0f);
  return d > 1.0e-12f ? num / d : 1.0e12f;
}

// The cutoff of this OS's filter runs from 20 Hz in 477 steps of 1/50 octave (audio/svf.h),
// so a cutoff of 0..1 is 20 Hz * 2^(9.54 c). The resonance is a Q of 0.7 + r, held to 0.7..5.
void draw_filter(Renderer& r, const Rect& area, bool enabled, unsigned type, float cutoff, float resonance) noexcept {
  const plot::AxisSpec fx{plot::AxisKind::LogFreq, kFilterLoHz, kFilterHiHz};
  const plot::AxisSpec dy{plot::AxisKind::Db, -36.0f, 18.0f};
  plot::CurveStyle style;
  style.max_vertices = 64;
  plot::draw_axes(r, area, fx, dy, style);
  float xs[kFilterPoints];
  float ys[kFilterPoints];
  const float c = cutoff < 0.0f ? 0.0f : (cutoff > 1.0f ? 1.0f : cutoff);
  const float f0 = kFilterLoHz * core::exp2_fast(9.54f * c);
  float q = 0.7f + resonance;
  q = q < 0.7f ? 0.7f : (q > 5.0f ? 5.0f : q);
  for (unsigned i = 0; i < kFilterPoints; ++i) {
    xs[i] = plot::axis_value(fx, static_cast<float>(i) / static_cast<float>(kFilterPoints - 1));
    ys[i] = enabled ? plot::db_from_power(svf_mag2(type, xs[i] / f0, q)) : 0.0f;  // a disabled filter passes everything
  }
  plot::CurveSeries series;
  series.x = xs;
  series.y = ys;
  series.n = kFilterPoints;
  (void)plot::draw_series(r, area, fx, dy, series, true, style, params_curve_scratch());
}

// The envelope's shape: from 0 to the amount in the attack, down to the sustain level in the decay, held for a fifth of the rest, then
// down to 0 in the release. Seconds on the x axis, from the times as the model stores them (milliseconds).
void draw_envelope(Renderer& r, const Rect& area, float amount, float attack, float decay, float sustain, float release) noexcept {
  const float total_ms = attack + decay + release;
  const float hold_ms = total_ms * 0.25f + 50.0f;  // the sustain is drawn a little longer than a point, whatever the times are
  const float end_ms = total_ms + hold_ms;
  const plot::AxisSpec xa{plot::AxisKind::Linear, 0.0f, end_ms * 0.001f};
  const plot::AxisSpec ya{plot::AxisKind::Linear, 0.0f, 1.0f};
  plot::CurveStyle style;
  style.max_vertices = 16;
  plot::draw_axes(r, area, xa, ya, style);
  const float a = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
  const float s = sustain < 0.0f ? 0.0f : (sustain > 1.0f ? 1.0f : sustain);
  const float xs[5] = {0.0f, attack * 0.001f, (attack + decay) * 0.001f, (attack + decay + hold_ms) * 0.001f, end_ms * 0.001f};
  const float ys[5] = {0.0f, a, a * s, a * s, 0.0f};
  plot::CurveSeries series;
  series.x = xs;
  series.y = ys;
  series.n = 5;
  (void)plot::draw_series(r, area, xa, ya, series, true, style, params_curve_scratch());
}

// The LFO's picture (OS-INSTUI S6, OS-LFO-GRAPH): the level A x shape(phase) the engine's generator makes (voices.h AMP LFO MODULATION: saw u, reverse saw 1 - u, triangle 2u then 2 - 2u, square 1 then 0,
// random a level held for a period, sine (1 - cos 2 pi u) / 2, A the amount), over a fixed window of 16 steps on the x axis, so the SPEED row moves the picture: 16 / period cycles, `period` the tempo-sync
// rate in steps (128 steps is an eighth of a cycle, a slow partial ramp; 16 steps one cycle; 1 step sixteen). Past 32 cycles (a period under half a step) a cycle is under 10 px and unreadable: the picture is a
// filled band from the shape's lowest level (0) to its highest (A). Random draws fixed illustrative levels, one a period. A sine has 4 to 48 points a cycle (fewer as the cycles grow) and every shape fits
// kLfoMaxPoints, so the vertices stay within the plot's scratch and their display-list words (2 a vertex) at 32 cycles. New Community OS behaviour (the graph showed two periods whatever the speed).
constexpr float kLfoWindowSteps = 16.0f;
constexpr float kLfoMaxCycles = 32.0f;
constexpr unsigned kLfoMaxPoints = 136;  // 32 cycles of a square (4 points) or a sine (4 points) and the end

void draw_lfo(Renderer& r, const Rect& area, unsigned shape, float amount, float period) noexcept {
  const plot::AxisSpec xa{plot::AxisKind::Linear, 0.0f, kLfoWindowSteps};
  const plot::AxisSpec ya{plot::AxisKind::Linear, 0.0f, 1.0f};
  plot::CurveStyle style;
  const float a = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
  const float cycles = kLfoWindowSteps / (period > 0.0f ? period : 1.0f);
  float xs[kLfoMaxPoints];
  float ys[kLfoMaxPoints];
  unsigned n = 0;
  if (cycles > kLfoMaxCycles + 0.01f) {
    style.fill_alpha = 0x80;  // denser than a curve's fill: the band is the picture; it spans the shape's full range 0..A (random's drawn levels top out lower, at 0.9 A)
    xs[0] = 0.0f;
    ys[0] = a;
    xs[1] = kLfoWindowSteps;
    ys[1] = a;
    n = 2;
  } else {
    constexpr float e = 0.002f;  // a jump is drawn as a near-vertical edge (a phase; the series needs ascending x)
    static constexpr float kRandom[8] = {0.7f, 0.3f, 0.9f, 0.15f, 0.55f, 0.4f, 0.8f, 0.25f};
    const unsigned whole = static_cast<unsigned>(cycles + 0.999f);  // the cycles started in the window
    unsigned ppc = 0;                                                 // sine: points a cycle, a multiple of 4 (the quarter points stay)
    if (shape == 5u) {
      ppc = 96u / whole;
      ppc = ppc > 48u ? 48u : (ppc < 4u ? 4u : (ppc & ~3u));
    }
    bool done = false;
    for (unsigned k = 0; k < whole && !done; ++k) {
      float tu[48];  // the points of this cycle: phase and level
      float ty[48];
      unsigned m = 0;
      const auto tpl = [&](float u, float y) {
        tu[m] = u;
        ty[m++] = y * a;
      };
      switch (shape) {
        case 0u:  // reverse saw
          tpl(0.0f, 1.0f);
          tpl(1.0f - e, 0.0f);
          break;
        case 1u:  // saw
          tpl(0.0f, 0.0f);
          tpl(1.0f - e, 1.0f);
          break;
        case 3u:  // square: high for the first half
          tpl(0.0f, 1.0f);
          tpl(0.5f - e, 1.0f);
          tpl(0.5f, 0.0f);
          tpl(1.0f - e, 0.0f);
          break;
        case 4u:  // random: a level a period
          tpl(0.0f, kRandom[k & 7u]);
          tpl(1.0f - e, kRandom[k & 7u]);
          break;
        case 5u:  // sine (OS-LFOSINE): the engine's own law, from 0 up to 1 at the half
          for (unsigned j = 0; j < ppc; ++j) {
            const float u = static_cast<float>(j) / static_cast<float>(ppc);
            tpl(u, audio::lfo_sine_level(u));
          }
          break;
        default:  // triangle (and any shape the voices read as one)
          tpl(0.0f, 0.0f);
          tpl(0.5f, 1.0f);
          tpl(1.0f - e, 0.0f);
          break;
      }
      for (unsigned j = 0; j < m && !done; ++j) {
        const float x = (static_cast<float>(k) + tu[j]) * period;
        if (x >= kLfoWindowSteps) {  // the window ends inside this stretch: the level where it ends
          const float y0 = n != 0u ? ys[n - 1u] : ty[j];
          const float x0 = n != 0u ? xs[n - 1u] : 0.0f;
          xs[n] = kLfoWindowSteps;
          ys[n] = y0 + (ty[j] - y0) * ((kLfoWindowSteps - x0) / (x - x0));
          ++n;
          done = true;
        } else {
          xs[n] = x;
          ys[n] = ty[j];
          ++n;
        }
      }
    }
    if (!done) {  // a whole number of cycles: the last level to the end
      xs[n] = kLfoWindowSteps;
      ys[n] = ys[n - 1u];
      ++n;
    }
  }
  style.max_vertices = static_cast<std::uint16_t>(n);
  plot::draw_axes(r, area, xa, ya, style);
  plot::CurveSeries series;
  series.x = xs;
  series.y = ys;
  series.n = n;
  (void)plot::draw_series(r, area, xa, ya, series, true, style, params_curve_scratch());
  // the window, said in words above the plot's right edge (the ticks are every 5 steps, from the axis maths); a band says it is one
  const char* const cue = cycles > kLfoMaxCycles + 0.01f ? "16 STEPS  BAND" : "16 STEPS";
  r.text(area.x + area.w - r.text_width(cue, Face::Label), area.y - 16, cue, Color::TextDim, Face::Label);
}

// ---- text -----------------------------------------------------------------------------------------------------------------

// "<value>" in a buffer of kParamTextChars + 1.
void value_text(const core::ParamDesc& d, float v, char (&out)[core::kParamTextChars + 1]) noexcept {
  (void)core::param_format(d, v, out, sizeof out);
}

// OS-INSTUI S1: the header's words (play_mode.h has the modes); the engines are audio::kEngine* (the instrument stores the engine + 1, 0 = Auto).
constexpr const char* kEngineNames[4] = {"MODERN", "AMIGA", "12-BIT", "8-BIT"};

// What the engine field says: the instrument's own choice, or (Auto) the engine Auto resolved to, "AUTO>AMIGA" (SampleBank::engine_for: what the voice actually plays with).
void engine_words(const Model& m, unsigned slot, const model::Instrument& in, core::FixedString<16>& out) noexcept {
  out.clear();
  if (in.engine != model::kInstEngineAuto) {
    out.assign(in.engine <= 4u ? kEngineNames[in.engine - 1u] : "?");
    return;
  }
  out.assign("AUTO");
  if (m.samples != nullptr) {
    const unsigned e = m.samples->engine_for(slot, in);
    out.append(">");
    out.append(e < 4u ? kEngineNames[e] : "?");
  }
}

// ---- the source page (OS-INSTUI S3) ---------------------------------------------------------------------------------------------------------

// What the source page knows about the instrument in view: its sample (a bank may not be attached, and a slot may hold no audio) and the play mode's own numbers.
struct Source {
  const model::Instrument* in = nullptr;
  std::uint32_t frames = 0;
  unsigned mode = 0;
  unsigned slices = 0;
  unsigned wt_size = 0;     // the window the engine plays with
  unsigned wt_windows = 0;  // how many of them the sample holds (0 without a sample)
  bool wt_silent = false;   // OS-INSTUI S6: a sample is there but holds no whole window of the stored size, so the engine plays nothing (wt_play_size 0)
};

Source source_state(const Model& m, const model::Instrument* in, unsigned slot) noexcept {
  Source s;
  if (in == nullptr || in->kind != model::InstrumentKind::Sample) {
    return s;
  }
  s.in = in;
  s.frames = m.samples != nullptr ? m.samples->pcm(slot).frames : 0u;
  s.mode = in->sampler.play_mode & 7u;
  s.slices = in->sampler.slice_count > kMaxSliceCount ? kMaxSliceCount : in->sampler.slice_count;
  const unsigned fit = wt_engine_size(in->sampler.wavetable_window_size, s.frames);
  s.wt_size = fit != 0u ? fit : wt_size_eff(in->sampler.wavetable_window_size);
  const unsigned play = wt_play_size(in->sampler.wavetable_window_size, s.frames);
  s.wt_windows = wt_windows(s.frames, play);
  s.wt_silent = s.frames != 0u && play == 0u;
  return s;
}

// A source row's own text and whether it applies right now. O8: a row that does not apply is dimmed with the reason (the footer says it), never hidden. The window size reads as the engine plays it (an unset
// size is 2048), the grain's position in seconds, the selected slice as "3 of 16".
void source_row(const Source& s, unsigned field, float value, char (&text)[core::kParamTextChars + 1], bool& dim, const char*& why) noexcept {
  core::FixedString<core::kParamTextChars + 1> t;
  switch (field) {
    case ip::WtPos:
    case ip::WtSize:
    case ip::GrainPos:
    case ip::GrainLength:
    case ip::GrainShape:
    case ip::GrainLoop:
      dim = s.frames == 0u;
      why = dim ? "NO SAMPLE IN THIS INSTRUMENT" : nullptr;
      break;
    case ip::Slice:
      dim = s.slices == 0u;
      why = dim ? "NO SLICES: AUTO OR EVEN ON WAVE" : nullptr;
      break;
    default:
      break;
  }
  if (s.wt_silent && s.mode == kPlayModeWavetable && (field == ip::WtPos || field == ip::WtSize)) {
    // OS-INSTUI S6 (the S5 review): a stored window the sample cannot hold plays nothing (the loader's rule, og_samples.cpp); WT SIZE stays editable (it is the way out), WT POS has nothing to point at.
    dim = field == ip::WtPos;
    why = s.frames < 32u ? "NOT PLAYABLE: SAMPLE UNDER 32 FRAMES" : "NOT PLAYABLE: WINDOW LARGER THAN THE SAMPLE (LOWER WT SIZE)";
  }
  if (field == ip::WtSize) {
    append_uint(t, s.wt_size);
  } else if (field == ip::GrainLength) {
    append_uint(t, s.in != nullptr && s.in->sampler.grain_length >= 44u ? s.in->sampler.grain_length : 441u);  // (an unset grain plays 441)
  } else if (field == ip::GrainPos && s.frames != 0u) {  // the grain's position is shown in seconds
    append_seconds(t, point_to_frame(static_cast<unsigned>(value), s.frames), s.in != nullptr ? s.in->sample_rate : 0u);
  } else if (field == ip::Slice) {
    append_uint(t, (s.in != nullptr ? s.in->sampler.selected_slice : 0u) + 1u);
    t.append(" of ");
    append_uint(t, s.slices < 1u ? 1u : s.slices);
  } else {
    return;
  }
  std::memcpy(text, t.c_str(), t.size() + 1u);
}

// O8, the source page's context visual: the sample with what the play mode plays marked on it, in the room the descriptor card has on the other pages. One shot: START and END; the loop modes: the loop as
// well; slices: a line at each start and the selected slice; wavetable: the window in use; granular: the grain around its position. Under it, the numbers in words.
void draw_source_visual(Renderer& r, const Model& m, const Source& s, unsigned slot, const Rect& card) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& mt = t.metrics;
  const model::SamplerParams& p = s.in->sampler;
  r.text(card.x, ui::caps_y(t, Face::Label, card.y, 16), "SOURCE", Color::TextDim, Face::Label);
  r.text(card.x, card.y + 20, kPlayModeNames[s.mode], Color::Text, Face::Display);
  const int lane_y = card.y + 20 + t.geometry(Face::Display).cell_h + mt.pad;
  const Rect lane = {card.x, lane_y, card.w, 110};
  r.frame({lane.x - 1, lane.y - 1, lane.w + 2, lane.h + 2}, Color::TextDim);
  const SampleBank::Pcm pcm = m.samples != nullptr ? m.samples->pcm(slot) : SampleBank::Pcm{};
  const std::uint32_t frames = pcm.frames;
  const std::uint32_t rate = s.in->sample_rate;
  const auto x_of = [&](std::uint32_t frame) { return lane.x + static_cast<int>(static_cast<std::uint64_t>(frame > frames ? frames : frame) * static_cast<std::uint64_t>(lane.w - 1) / (frames != 0u ? frames : 1u)); };
  core::FixedString<48> line;
  if (frames == 0u) {
    const char* none = "NO SAMPLE IN THIS INSTRUMENT";
    r.text(lane.x + (lane.w - r.text_width(none, Face::Label)) / 2, lane.y + lane.h / 2 - 6, none, Color::TextDim, Face::Label);
  } else if (m.peaks == nullptr || !m.peaks->valid() || !m.peaks->built() || m.peaks->samples() != pcm.data || m.peaks->frames() != frames) {
    const char* wait = "BUILDING THE WAVEFORM";
    r.text(lane.x + (lane.w - r.text_width(wait, Face::Label)) / 2, lane.y + lane.h / 2 - 6, wait, Color::TextDim, Face::Label);
  } else {
    ui::plot::WaveMarks marks;
    marks.zero_line = true;
    const std::uint32_t start = point_to_frame(p.start, frames);
    const std::uint32_t end = point_to_frame(p.end, frames);
    if (s.mode >= 1u && s.mode <= 3u) {
      marks.has_loop = true;
      marks.loop_start = point_to_frame(p.loop_start, frames);
      marks.loop_end = point_to_frame(p.loop_end, frames);
    } else if (s.mode == kPlayModeWavetable || s.mode == kPlayModeGranular) {
      const FrameRange range = s.mode == kPlayModeWavetable ? wt_window_frames(frames, p.wavetable_window_size, p.wavetable_window) : grain_frames(frames, p.grain_position, p.grain_length);
      marks.has_selection = range.valid;
      marks.sel_begin = range.begin;
      marks.sel_end = range.end;
    } else if (play_mode_is_slice(s.mode) && s.slices != 0u) {
      const unsigned k = p.selected_slice < s.slices ? p.selected_slice : 0u;
      marks.has_selection = true;
      marks.sel_begin = point_to_frame(p.slices[k], frames);
      marks.sel_end = k + 1u < s.slices ? point_to_frame(p.slices[k + 1u], frames) : frames;
    }
    (void)ui::plot::draw_wave(r, lane, *m.peaks, 0, ui::plot::WaveWindow{0, frames}, ui::plot::WaveMode::Vector, nullptr, 0, 0, marks, ui::plot::WaveStyle{});
    if (s.mode <= 3u) {  // START and END, as Wave draws them
      r.panel({x_of(start) - 1, lane.y, 2, lane.h}, Color::Accent);
      r.panel({x_of(end) - 1, lane.y, 2, lane.h}, Color::Accent);
    } else if (play_mode_is_slice(s.mode)) {
      for (unsigned i = 0; i < s.slices; ++i) {
        r.panel({x_of(point_to_frame(p.slices[i], frames)), lane.y, 1, lane.h}, i == p.selected_slice ? Color::Accent : Color::Text);
      }
    }
  }
  const int pitch = t.geometry(Face::Label).cell_h + 2;
  int y = lane.y + lane.h + 8;
  switch (s.mode) {
    case kPlayModeWavetable:
      if (s.wt_windows == 0u) {
        if (s.wt_silent) {  // OS-INSTUI S6: not playable, and why
          line.assign("NOT PLAYABLE: ");
          if (s.frames < 32u) {
            line.append("UNDER 32 FRAMES");
          } else {
            line.append("SIZE ");
            append_uint(line, s.wt_size);
            line.append(" > ");
            append_uint(line, s.frames);
          }
        } else {
          line.assign("NO WHOLE WINDOW IN THE SAMPLE");
        }
      } else {
        line.assign("WINDOW ");  // (the row's own number: counted from 0)
        append_uint(line, wt_clamp_pos(frames, s.wt_size, p.wavetable_window));
        line.append(" OF 0..");
        append_uint(line, s.wt_windows - 1u);
      }
      r.text(card.x, y, line.c_str(), Color::Text, Face::Label);
      y += pitch;
      line.assign("SIZE ");
      append_uint(line, s.wt_size);
      line.append(" FRAMES");
      break;
    case kPlayModeGranular:
      line.assign("GRAIN ");
      {
        const unsigned tenths = static_cast<unsigned>(p.grain_length) * 100u / 441u;  // samples / 44.1 = ms, in tenths
        append_uint(line, tenths / 10u);
        line.append(".");
        append_uint(line, tenths % 10u);
      }
      line.append(" MS");
      r.text(card.x, y, line.c_str(), Color::Text, Face::Label);
      y += pitch;
      line.assign("AT ");
      append_seconds(line, point_to_frame(p.grain_position, frames), rate);
      break;
    default:
      if (play_mode_is_slice(s.mode)) {
        line.assign("SLICE ");
        append_uint(line, (p.selected_slice < s.slices ? p.selected_slice : 0u) + (s.slices != 0u ? 1u : 0u));
        line.append(" OF ");
        append_uint(line, s.slices);
      } else {
        line.assign("START ");
        append_seconds(line, point_to_frame(p.start, frames), rate);
        r.text(card.x, y, line.c_str(), Color::Text, Face::Label);
        y += pitch;
        line.assign("END ");
        append_seconds(line, point_to_frame(p.end, frames), rate);
        if (s.mode >= 1u && s.mode <= 3u) {
          r.text(card.x, y, line.c_str(), Color::Text, Face::Label);
          y += pitch;
          line.assign("LOOP ");
          append_seconds(line, point_to_frame(p.loop_start, frames), rate);
          line.append(" .. ");
          append_seconds(line, point_to_frame(p.loop_end, frames), rate);
        }
      }
      break;
  }
  r.text(card.x, y, line.c_str(), Color::Text, Face::Label);
}

// F1's label on a sample instrument: the page it will open (S3) by the play mode.
const char* source_label(const model::Instrument& in) noexcept {
  const std::uint8_t pm = in.sampler.play_mode;
  return pm == kPlayModeWavetable ? "WT" : (pm == kPlayModeGranular ? "GRAIN" : (pm == 4u || pm == 5u ? "SLICE" : "SAMPLE"));
}

// "<06" / "08>" (the number is 1..64; the caller dims the label when there is nothing to go to).
void neighbour_label(char (&out)[4], bool back, int n) noexcept {
  const char hi = static_cast<char>('0' + (n / 10) % 10);
  const char lo = static_cast<char>('0' + n % 10);
  out[0] = back ? '<' : hi;
  out[1] = back ? hi : lo;
  out[2] = back ? lo : '>';
  out[3] = '\0';
}

const char* kind_name(const model::Instrument* in) noexcept {
  if (in == nullptr) {
    return "FREE";
  }
  return in->kind == model::InstrumentKind::Sample ? "SAMPLE" : (in->kind == model::InstrumentKind::Midi ? "MIDI" : "MODULE");
}

}  // namespace

#ifndef O3OS_CURVE_SCRATCH_EXTERNAL
plot::CurveScratch& params_curve_scratch() noexcept { return g_scratch; }
#endif

const char* params_filter_row_why(const Model& model, const ViewState& view, unsigned row) noexcept {
  const Rows rows = rows_of(model, view);
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(slot_of(view)) : nullptr;
  return row < rows.n && group_of(model, view) == ip::GroupFilter ? filter_row_why(in, rows.field[row]) : nullptr;
}

const char* params_mod_row_why(const Model& model, const ViewState& view, unsigned row) noexcept {
  return mod_row_why(model.project != nullptr ? model.project->instrument(slot_of(view)) : nullptr, envelope_of(view), row);
}

unsigned params_mod_rows(const Model& model, const ViewState& view, std::uint16_t (&fields)[ParamView::kMaxRows]) noexcept {
  const Rows rows = rows_of(model, view);
  for (unsigned i = 0; i < rows.n; ++i) {
    fields[i] = rows.field[i];
  }
  return rows.n;
}

void params_source_text(const Model& model, unsigned slot, unsigned field, char (&out)[core::kParamTextChars + 1]) noexcept {
  out[0] = '\0';
  const core::ParamId id = id_of_slot(slot, field);
  const core::ParamDesc* d = model::param_desc(id);
  if (d == nullptr) {
    return;
  }
  bool live = false;
  const float v = value_of(model, id, *d, live);
  value_text(*d, v, out);
  const Source s = source_state(model, model.project != nullptr ? model.project->instrument(slot) : nullptr, slot);
  bool dim = false;
  const char* why = nullptr;
  source_row(s, field, v, out, dim, why);
}

// The text for the strip's "how" field: the field starts at `row_x` (a caption of nothing and a gap first) and must end before the CPU / MEM readout and its group gap.
void params_how_clip(Renderer& r, core::FixedString<40>& text, int row_x) noexcept {
  const ui::Metrics& m = r.theme().metrics;
  const int room = ui::kScreenW - 2 * m.pad - StatusStrip::load_width(r) - 4 * m.gap - (row_x + m.gap);
  std::size_t n = text.size();
  while (n > 0 && r.text_width(text.c_str(), Face::Data) > room) {
    core::FixedString<40> shorter;
    (void)shorter.append(text.c_str(), --n);
    text = shorter;
  }
}

void params_engine_text(const Model& model, unsigned slot, const model::Instrument& in, core::FixedString<16>& out) noexcept { engine_words(model, slot, in, out); }

SlotKind params_slot_kind(const Model& model, const ViewState& view) noexcept {
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(slot_of(view)) : nullptr;
  if (in == nullptr) {
    return SlotKind::Empty;
  }
  return in->kind == model::InstrumentKind::Sample ? SlotKind::Sample : (in->kind == model::InstrumentKind::Midi ? SlotKind::Midi : SlotKind::Module);
}

Context ParamsScreen::field_context(const Model& model, const ViewState& view) const noexcept { return is_chooser(model, view) ? Context::InstrumentEmpty : Context::None; }

int instrument_in_use(const Model& model, int from, int dir) noexcept {
  if (model.project == nullptr || dir == 0) {
    return 0;
  }
  const int step = dir < 0 ? -1 : 1;
  for (int n = from + step; n >= 1 && n <= static_cast<int>(model::kMaxInstruments); n += step) {
    if (model.project->instrument(static_cast<std::uint32_t>(n - 1)) != nullptr) {
      return n;
    }
  }
  return 0;
}

unsigned ParamsScreen::pages() const noexcept { return ip::kGroups; }

unsigned ParamsScreen::module_page_count(const Model& model, const ViewState& view) const noexcept {
  const unsigned pages = module_page_total(model, view);
  return pages == 0u ? 1u : pages;
}

// The pages turned to by instrument.turn, which only the empty slot's type chooser binds to Left / Right now (a used slot's arrows edit the row), wrapping. A module's pages are its parameters' (eight to a page); the others' are the four groups, and the
// ENV page keeps its second axis: Left / Right walk its six envelopes first and turn the page past either end (arriving from the left lands on the first envelope, from the right on the last).
bool ParamsScreen::turn_page(const Model& model, ViewState& view, int dir, bool wrap) const noexcept {
  const int up = dir < 0 ? -1 : 1;
  std::uint8_t& page = view.page[screen_index(ScreenId::Instrument)];
  const bool module = is_module_slot(model, view);
  const model::Instrument* inst = model.project != nullptr ? model.project->instrument(slot_of(view)) : nullptr;
  if (is_midi_group(group_of(model, view))) {
    // OS-INSTUI S8b: a MIDI instrument has two pages, MIDI and CC, and either direction goes to the other (an auto-repeat does not go round again)
    const bool at_cc = group_of(model, view) == kCcGroup;
    if (!wrap && (up > 0 ? at_cc : !at_cc)) {
      return false;
    }
    page = static_cast<std::uint8_t>(at_cc ? kMidiGroup : kCcGroup);
    view.params.row = 0;
    view.params.discard_pending();
    return true;
  }
  const unsigned first = (inst != nullptr && inst->kind == model::InstrumentKind::Sample) ? 0u : 1u;  // the source page (the first key) belongs to a sample instrument
  const unsigned count = module ? module_page_count(model, view) : ip::kGroups - first;
  if (count < 2) {
    return false;  // one page: nothing to turn to (and the row and a pending edit stay)
  }
  const unsigned at = module ? module_page(model, view) : key_of_group(group_of(model, view)) - first;
  if (!wrap && (up > 0 ? at + 1u >= count : at == 0u)) {
    return false;  // an auto-repeat stops at the end: only a fresh press wraps (the repeats are plain taps, so instrument.turn tells them by the time between presses)
  }
  const unsigned to = (at + (up > 0 ? 1u : count - 1u)) % count;
  const bool changed = to != at;
  const unsigned group = module ? to : kKeyOrder[first + to];
  page = static_cast<std::uint8_t>(group);
  view.params.row = 0;
  view.params.discard_pending();
  if (!module && group == ip::GroupEnvelope) {
    view.params.env = up > 0 ? 0 : static_cast<int>(ip::kEnvelopes) - 1;
  }
  return changed;
}

bool ParamsScreen::next_window(const Model& model, ViewState& view, unsigned key) const noexcept {
  if (key != 3u || group_of(model, view) != ip::GroupEnvelope) {  // (the sample instrument's MOD key; the other groups, and a module's pages (four at most), are one window)
    return false;
  }
  view.params.env = (envelope_of(view) + 1u) % ip::kEnvelopes;
  return true;
}

// Reached only through Screen::step (OS-INSTUI S1; no Instrument key binds an arrow to it now): the ENV page's envelopes first, then the page. `wrap` false: an auto-repeat, which stops at the ends.
bool ParamsScreen::turn(const Model& model, ViewState& view, int dir, bool wrap) const noexcept { return track(model, view, dir) || turn_page(model, view, dir, wrap); }

bool ParamsScreen::repress(const Model& model, ViewState& view) const noexcept {
  std::uint8_t& page = view.page[screen_index(ScreenId::Instrument)];
  const model::Instrument* inst = model.project != nullptr ? model.project->instrument(slot_of(view)) : nullptr;
  if (inst != nullptr && inst->kind == model::InstrumentKind::Midi) {
    const bool changed = page == ip::GroupCc;  // OS-INSTUI S8b: the Params button is F1: the MIDI page
    page = static_cast<std::uint8_t>(kMidiGroup);
    if (changed) {
      view.params.row = 0;
      view.params.discard_pending();
    }
    return changed;
  }
  const std::uint8_t first = inst != nullptr && inst->kind == model::InstrumentKind::Sample ? static_cast<std::uint8_t>(ip::GroupSource) : std::uint8_t{0};  // F1: the source page of a sample instrument, a module's first page, else VOICE
  const bool changed = page != first;
  page = first;
  if (changed) {
    view.params.row = 0;
    view.params.discard_pending();
  }
  return changed;
}

void params_key_row(const Model& model, const ViewState& view, ParamsKeyRow& out) noexcept {
  out.dim = 0;
  const model::Instrument* inst = model.project != nullptr ? model.project->instrument(slot_of(view)) : nullptr;
  const bool module_slot = inst != nullptr && inst->kind == model::InstrumentKind::Module;
  const bool sample_inst = inst != nullptr && inst->kind == model::InstrumentKind::Sample;
  const Screen& self = kParamsScreen;
  // OS-INSTUI D1: F1 follows the instrument (module: F1..F4 its pages), F6 / F7 show the neighbours (Shift: the nearest in use) and are dimmed where there is none, the keys that wait for
  // their slice (the source page, NAME .. SAVE) are dimmed too.
  const bool shifted = view.shift && follows_shift(self, view);
  for (unsigned i = 0; i < 8; ++i) {
    out.label[i] = (shifted ? kParamsShiftKeys : kParamsSoftKeys)[i];
  }
  const int ins = view.instrument;
  const int prev = shifted ? instrument_in_use(model, ins, -1) : (ins > 1 ? ins - 1 : 0);
  const int next = shifted ? instrument_in_use(model, ins, 1) : (ins < static_cast<int>(model::kMaxInstruments) ? ins + 1 : 0);
  if (prev != 0) {
    neighbour_label(out.number[0], true, prev);
    out.label[5] = out.number[0];
  } else {
    out.dim |= 1u << 5;
  }
  if (next != 0) {
    neighbour_label(out.number[1], false, next);
    out.label[6] = out.number[1];
  } else {
    out.dim |= 1u << 6;
  }
  const bool clip = model.samples != nullptr && model.samples->clip_valid();
  if (shifted) {
    // OS-INSTUI S7: NAME, COPY and DELETE act on a used slot (COPY not on a module: its instance cannot be duplicated); PASTE needs a clipboard and no module in the way; SAVE is for a sample
    // instrument. A dim key still answers when pressed, and says why on the strip.
    out.dim |= (inst == nullptr ? 0x01u : 0u) | (inst == nullptr || module_slot ? 0x02u : 0u) | (!clip || module_slot ? 0x04u : 0u) | (inst == nullptr ? 0x08u : 0u) | (sample_inst ? 0u : 0x10u);
  } else if (is_chooser(model, view)) {
    static constexpr const char* kChooser[5] = {"SAMPLE", "MIDI", "PASTE", "MODULE", "LOAD"};  // the type chooser (D4)
    for (unsigned i = 0; i < 5; ++i) {
      out.label[i] = kChooser[i];
    }
    out.dim |= clip ? 0u : (1u << 2);  // PASTE needs a copied instrument (Shift + F2)
  } else if (module_slot) {
    static constexpr const char* kModulePages[4] = {"1-8", "9-16", "17-24", "25-32"};
    const unsigned pages = kParamsScreen.module_page_count(model, view);
    for (unsigned i = 0; i < 5; ++i) {
      // A key shows only a page the module has (OS-MODPAGEKEYS: a module of two pages has F1 and F2); F3.. and F5 (not bound for a module) are blank, not dim, and the F-number boxes keep the row aligned.
      const char* named = i < 4u && i < pages ? module_page_at(model, view, i).name : nullptr;  // OS-INSTUI S10: the module's own name for the page (at most 7 characters), else the numbers
      out.label[i] = i >= 4u || i >= pages ? "" : (named != nullptr && named[0] != '\0' ? named : kModulePages[i]);
    }
  } else if (inst == nullptr) {
    out.label[0] = "SAMPLE";  // no project open: the chooser is not offered, the keys do nothing
    out.dim |= 1u;
  } else {
    out.label[0] = sample_inst ? source_label(*inst) : "MIDI";
    if (!sample_inst) {
      out.label[1] = "CC";  // OS-INSTUI S8b: a MIDI instrument has two pages, F1 MIDI and F2 CC; FILTER, MOD and SENDS are dim (they answer and say why: instrument_page)
    }
    out.dim |= sample_inst ? 0u : 0x1Cu;
  }
}

bool ParamsScreen::pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept {
  // OS-INSTUI S2: the pads' LEDs as on Pattern. NOTES lights nothing of its own (the platform's held-pad policy stands); PIANO and ISO draw their keys, but only the note pads: the jump and
  // octave pads of PIANO do nothing on this screen, so they stay dark.
  const PadMode mode = view.padmode.fixed();
  if (mode != PadMode::Piano && mode != PadMode::Iso) {
    return false;
  }
  static const PatternContent kNoContent{};  // (only PATS reads it)
  mode_roles(model, view, kNoContent, false, roles);
  for (unsigned pad = 0; pad < 48u; ++pad) {
    if ((mode == PadMode::Piano ? piano_pad(pad) : iso_pad(pad)).kind != PadKey::Note) {
      roles[pad] = PadRole::Off;
    }
  }
  return true;
}

void ParamsScreen::enter(const Model& model, ViewState& view) const noexcept {
  const Rows rows = rows_of(model, view);
  view.params.row = static_cast<int>(row_of(view, rows));  // inside the page it opens on
}

// ---- the cursor: the rows of the page, the envelope as the second axis ---------------------------------------------------

int ParamsScreen::count(const Model& model, const ViewState& view) const noexcept { return static_cast<int>(rows_of(model, view).n); }

// (The row as the view holds it: a module page's number of rows depends on the model, which the cursor interface does not pass here; every move clamps and the page
// draws and edits the row held to its rows.)
int ParamsScreen::position(const ViewState& view) const noexcept {
  return view.params.row < 0 ? 0 : (view.params.row >= ParamView::kMaxRows ? ParamView::kMaxRows - 1 : view.params.row);
}

bool ParamsScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = static_cast<int>(rows_of(model, view).n) - 1;
  const int to = position < 0 ? 0 : (position > last ? last : position);
  const bool changed = to != view.params.row;
  view.params.row = to;
  return changed;
}

bool ParamsScreen::track(const Model& model, ViewState& view, int delta) const noexcept {
  if (group_of(model, view) != ip::GroupEnvelope || is_module_slot(model, view)) {
    return false;
  }
  const int last = static_cast<int>(ip::kEnvelopes) - 1;
  const int to = view.params.env + (delta < 0 ? -1 : (delta > 0 ? 1 : 0));
  const int clamped = to < 0 ? 0 : (to > last ? last : to);
  const bool changed = clamped != view.params.env;
  view.params.env = clamped;
  return changed;
}

bool ParamsScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (dx != 0) {
    return turn(model, view, dx, true);
  }
  return dy != 0 && set_position(model, view, position(view) + dy);
}

core::ParamId ParamsScreen::focus_param(const Model& model, const ViewState& view) const noexcept {
  const Rows rows = rows_of(model, view);
  return rows.n == 0 ? core::ParamId::None : id_of(view, rows.field[row_of(view, rows)]);
}

// ---- drawing ----------------------------------------------------------------------------------------------------------

void ParamsScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const ParamsLayout l = make_params_layout(t);
  const Frame& f = l.frame;
  const Rows rows = rows_of(model, view);
  const unsigned group = group_of(model, view);
  const bool midi_page = is_midi_group(group);  // OS-INSTUI S8a / S8b: F1 MIDI or F2 CC of a MIDI instrument
  const unsigned envelope = envelope_of(view);
  const unsigned cursor = row_of(view, rows);
  const unsigned slot = slot_of(view);
  const model::Instrument* inst = model.project != nullptr ? model.project->instrument(slot) : nullptr;
  const bool module_slot = inst != nullptr && inst->kind == model::InstrumentKind::Module;
  const bool chooser = is_chooser(model, view);  // OS-INSTUI S5: an empty slot of an open project: the type chooser takes the body
  const int cell_w = t.geometry(Face::Data).cell_w;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto row_y = [&](unsigned i) { return f.pane.y + static_cast<int>(i) * f.row_pitch; };
  const auto cell_y = [&](unsigned i) { return row_y(i) + l.grid.cell_dy; };
  const auto id_at = [&](unsigned i) { return id_of(view, rows.field[i]); };

  draw_chrome_bands(r, f, covered);

  // The rows' values and what a row needs about the descriptor, once (a module's descriptors are made from its o3m_param on the spot: ParamScratch).
  float values[ParamView::kMaxRows] = {};
  char texts[ParamView::kMaxRows][core::kParamTextChars + 1] = {};
  ParamScratch scratch[ParamView::kMaxRows];
  const core::ParamDesc* descs[ParamView::kMaxRows] = {};
  bool live = false;
  for (unsigned i = 0; i < rows.n; ++i) {
    descs[i] = describe_param(model, id_at(i), scratch[i]);
    const core::ParamDesc* d = descs[i];
    if (d == nullptr) {
      continue;
    }
    values[i] = value_of(model, id_at(i), *d, live);
    value_text(*d, values[i], texts[i]);
  }
  const bool has_value = rows.n != 0 && inst != nullptr;
  const Source src = source_state(model, inst, slot);
  bool row_dim[ParamView::kMaxRows] = {};
  const char* row_why[ParamView::kMaxRows] = {};
  if (group == ip::GroupSource) {
    for (unsigned i = 0; i < rows.n; ++i) {
      source_row(src, rows.field[i], values[i], texts[i], row_dim[i], row_why[i]);
    }
  }
  // OS-GRAN: the wavetable position (4) and granular position (5) envelopes are played: the first for a wavetable instrument only
  // (play mode 6), the second for a granular one only (play mode 7). On any other instrument they are stored and edited but not run: dimmed, and the info line says what the instrument needs.
  const bool sample_inst = inst != nullptr && inst->kind == model::InstrumentKind::Sample;
  // OS-INSTUI S6 (O8): the MOD page dims every row of a destination the instrument does not use, never hides one, and the info line says why: the wavetable position needs WAVETABLE, the granular position
  // GRANULAR, the cutoff a filter that is on (the cutoff modulator runs only then, voices.h FILTER MODULATION). A destination whose SOURCE is Off dims the rows below SOURCE.
  if (group == ip::GroupEnvelope && !module_slot && !chooser) {
    for (unsigned i = 0; i < rows.n; ++i) {
      row_why[i] = mod_row_why(inst, envelope, i);
      row_dim[i] = row_why[i] != nullptr;
    }
  }

  if (group == ip::GroupFilter && !chooser) {  // OS-AUDIT E-INST-05 (O8): cutoff and resonance are dimmed, not hidden, while the filter is off; the footer says why
    for (unsigned i = 0; i < rows.n; ++i) {
      row_why[i] = filter_row_why(inst, rows.field[i]);
      row_dim[i] = row_why[i] != nullptr;
    }
  }

  // The selected row: the one strong highlight, and the cursor block on its value (as the Song screen's).
  if (rows.n != 0) {
    const Rect bar = {0, row_y(cursor), l.card_x - 2 * m.pad, f.row_pitch};  // the list's width: the card stays clear of the highlight
    if (!hidden(bar)) {
      r.highlight_row(bar);
    }
    const int tw = r.text_width(texts[cursor], Face::Data);
    const Rect block = {l.value_x + l.value_w - tw - l.grid.cursor_pad_x, row_y(cursor) + l.grid.cursor_dy,
                        tw + 2 * l.grid.cursor_pad_x, l.grid.cursor_h};
    const int o = l.grid.cursor_outline;
    const Rect outline = {block.x - o, block.y - o, block.w + 2 * o, block.h + 2 * o};
    if (!hidden(outline)) {
      r.panel(outline, Color::AccentText);
      r.panel(block, Color::Accent);
    }
  }

  // The bars, following the descriptors' curves.
  for (unsigned i = 0; i < rows.n; ++i) {
    const core::ParamDesc* d = descs[i];
    const Rect area = {l.bar_x, row_y(i) + (f.row_pitch - l.bar_h) / 2, l.bar_w, l.bar_h};
    if (d != nullptr && !hidden(area)) {
      const bool sel = i == cursor;
      ui::bar(r, area, static_cast<unsigned>(core::param_norm(*d, values[i]) * 1000.0f + 0.5f), 1000, sel ? Color::Sel : Color::Empty,
              sel ? Color::SelText : Color::Instr);
    }
  }

  // The mini-curve under the rows: the filter's response, the envelope's shape.
  const int plot_y = row_y(rows.n) + 2 * m.pad + 8;
  const Rect plot_area = {l.plot_x, plot_y, l.plot_w, f.pane.y + f.pane.h - plot_y - 24};
  if (module_slot) {  // the module's own views, side by side in the room under the rows (ADR 0002 amendment 1)
    const unsigned views = model.modules != nullptr && model.project != nullptr ? model.modules->view_count(*model.project, slot) : 0u;
    if (views != 0u) {
      model.modules->begin_view_frame();  // the views of this page share the display-list words module::kViewMaxWords gives them
    }
    for (unsigned v = 0; v < views && plot_area.h > 40; ++v) {
      const int w = plot_area.w / static_cast<int>(views);
      const Rect area = {plot_area.x + static_cast<int>(v) * w, plot_area.y, w - 2 * m.gap, plot_area.h};
      if (!hidden(area)) {
        (void)model.modules->draw_slot_view(r, *model.project, slot, v, area);
      }
    }
  } else if (!chooser && (group == ip::GroupFilter || group == ip::GroupEnvelope) && plot_area.h > 40 && !hidden({0, plot_y - 4, l.card_x, plot_area.h + 32})) {
    const auto at = [&](unsigned field) {
      const core::ParamId id = id_of(view, field);
      const core::ParamDesc* d = model::param_desc(id);
      bool lv = false;
      return d != nullptr ? value_of(model, id, *d, lv) : 0.0f;
    };
    if (group == ip::GroupFilter) {
      draw_filter(r, plot_area, at(ip::FilterEnable) >= 0.5f, static_cast<unsigned>(at(ip::FilterType) + 0.5f), at(ip::Cutoff),
                  at(ip::Resonance));
    } else {
      const auto env = [&](unsigned f2) { return at(ip::envelope(envelope, f2)); };
      if (mod_source_of(inst, envelope) == ip::SourceLfo) {
        // the period from the table and the index the engine uses (the amp slot's 24 rates are the last 24 of the 29: voices.h kAmpRateBase)
        const unsigned amp = envelope == 0 ? 1u : 0u;
        unsigned rate = static_cast<unsigned>(at(ip::mod(envelope, ip::LfoSpeed)) + 0.5f);
        rate = rate >= audio::kLfoRates - amp * audio::kAmpRateBase ? audio::kLfoRates - amp * audio::kAmpRateBase - 1u : rate;
        draw_lfo(r, plot_area, static_cast<unsigned>(at(ip::mod(envelope, ip::LfoShape)) + 0.5f), at(ip::mod(envelope, ip::LfoAmount)),
                 audio::kLfoRate[rate + amp * audio::kAmpRateBase]);
      } else {
        draw_envelope(r, plot_area, env(ip::EnvAmount), env(ip::EnvAttack), env(ip::EnvDecay), env(ip::EnvSustain), env(ip::EnvRelease));
      }
    }
  }

  // OS-INSTUI S8a / S8b, the MIDI pages: what the instrument does, under the rows.
  if (midi_page && inst != nullptr) {
    const int pitch = t.geometry(Face::Label).cell_h + 2;
    const Rect lines = {l.name_x, plot_y, l.card_x - l.name_x - 2 * m.pad, 3 * pitch};
    if (!hidden(lines)) {
      if (group == kMidiGroup) {
        r.text(lines.x, plot_y, "NOTES OF THE PADS AND THE STEPS GO ON THIS CHANNEL,", Color::TextDim, Face::Label);
        r.text(lines.x, plot_y + pitch, "TO THE PORT OUT NAMES (CONFIG = MIDI OUT OF CONFIG).", Color::TextDim, Face::Label);
        r.text(lines.x, plot_y + 2 * pitch, "PROGRAM IS SENT WHEN IT CHANGES AND AFTER A LOAD.", Color::TextDim, Face::Label);
      } else {
        r.text(lines.x, plot_y, "FX 8..12 AND 37 ARE CC A..F: THEIR VALUE GOES TO THE", Color::TextDim, Face::Label);
        r.text(lines.x, plot_y + pitch, "CONTROLLER SET HERE (CONFIG = THE NUMBER IN CONFIG),", Color::TextDim, Face::Label);
        r.text(lines.x, plot_y + 2 * pitch, "AS PROGRAM CHANGE (PROG) OR AS AFTERTOUCH (AT).", Color::TextDim, Face::Label);
      }
    }
  }

  // The type chooser (OS-INSTUI S5, D3 / D4): one line a key, in the key row's order; PASTE (S7) is dimmed and says why while nothing is copied.
  if (chooser) {
    struct Choice {
      const char* key;
      const char* name;
      const char* text;
      bool dim;
    };
    core::FixedString<40> midi_text;
    midi_text.assign("A MIDI INSTRUMENT ON CHANNEL ");
    append_uint(midi_text, midi_channel_for_slot(slot));
    const bool clip = model.samples != nullptr && model.samples->clip_valid();
    core::FixedString<48> paste_text;
    if (clip) {
      paste_text.assign("PASTE ");
      paste_text.append(model.samples->clip_instrument().name[0] != '\0' ? model.samples->clip_instrument().name : "THE COPIED INSTRUMENT");
    } else {
      paste_text.assign("NOTHING COPIED: SHIFT+F2 ON AN INSTRUMENT");
    }
    const Choice choices[5] = {{"F1", "SAMPLE", "LOAD A SAMPLE FILE INTO THIS SLOT", false},
                               {"F2", "MIDI", midi_text.c_str(), false},
                               {"F3", "PASTE", paste_text.c_str(), !clip},
                               {"F4", "MODULE", "AN INSTRUMENT MODULE FROM THE CARD", false},
                               {"F5", "LOAD", "LOAD AN INSTRUMENT FILE (.PTI) FROM THE CARD", false}};
    for (unsigned i = 0; i < 5; ++i) {
      const int y = cell_y(i);
      const int text_x = l.name_x + 12 * cell_w;
      const Color ink = choices[i].dim ? Color::TextDim : Color::Text;
      if (!hidden({l.name_x, y, ui::kScreenW - l.name_x, t.geometry(Face::Data).cell_h})) {
        r.text(l.name_x, y, choices[i].key, Color::ChromeDim);
        r.text(l.name_x + 4 * cell_w, y, choices[i].name, choices[i].dim ? Color::TextDim : Color::Instr);
        r.text(text_x, y, choices[i].text, ink);
      }
    }
  }

  // The card: everything the descriptor says about the row under the cursor.
  const int card_y = f.pane.y + m.pad;
  const Rect card = {l.card_x, card_y, l.card_w, f.pane.y + f.pane.h - card_y};
  const core::ParamDesc* cd = rows.n != 0 ? descs[cursor] : nullptr;
  if (group == ip::GroupSource && src.in != nullptr) {
    if (!hidden(card)) {
      draw_source_visual(r, model, src, slot, card);  // O8: the context visual takes the card's room; the range and the default are on the info line
    }
  } else if (cd != nullptr && !hidden(card)) {
    const core::ParamId cid = id_at(cursor);
    core::FixedString<48> line;
    r.text(card.x, ui::caps_y(t, Face::Label, card_y, 16), "SELECTED", Color::TextDim, Face::Label);
    r.text(card.x, card_y + 20, cd->name, Color::Text, Face::Display);
    const int value_y = card_y + 20 + t.geometry(Face::Display).cell_h + m.pad;
    r.text(card.x, value_y, texts[cursor], Color::Text, Face::Display);
    r.text(card.x + r.text_width(texts[cursor], Face::Display) + 2 * m.gap, value_y + t.geometry(Face::Display).cap_h - t.geometry(Face::Label).cap_h + 6,
           cd->unit_text, Color::TextDim, Face::Label);
    const int bar_y = value_y + t.geometry(Face::Display).cell_h + m.pad;
    ui::bar(r, {card.x, bar_y, card.w, l.bar_h}, static_cast<unsigned>(core::param_norm(*cd, values[cursor]) * 1000.0f + 0.5f), 1000, Color::Empty,
            Color::Instr);
    int y = bar_y + l.bar_h + 2 * m.pad;
    const int pitch = t.geometry(Face::Label).cell_h + 2;
    char lo[core::kParamTextChars + 1], hi[core::kParamTextChars + 1], df[core::kParamTextChars + 1];
    value_text(*cd, cd->min, lo);
    value_text(*cd, cd->max, hi);
    value_text(*cd, cd->def, df);
    line.assign("RANGE ");
    line.append(lo);
    line.append(" .. ");
    line.append(hi);
    line.append(" ");
    line.append(cd->unit_text);
    r.text(card.x, y, line.c_str(), Color::Text, Face::Label);
    y += pitch;
    line.assign("DEFAULT ");
    line.append(df);
    line.append(" ");
    line.append(cd->unit_text);
    r.text(card.x, y, line.c_str(), Color::Text, Face::Label);
    y += pitch;
    line.assign("STEP 1/");
    append_uint(line, cd->detents);
    line.append("  COARSE x");
    append_uint(line, cd->coarse);
    r.text(card.x, y, line.c_str(), Color::TextDim, Face::Label);
    y += pitch;
    line.clear();
    line.append((cd->flags & core::kParamLearnable) != 0 ? "LEARN " : "");
    line.append((cd->flags & core::kParamModulatable) != 0 ? "MOD " : "");
    line.append((cd->flags & core::kParamSmooth) != 0 ? "SMOOTH" : "");
    r.text(card.x, y, line.c_str(), Color::Fx, Face::Label);
    y += pitch;
    const core::LearnBinding* slots = model.project != nullptr ? model.project->settings().learn_slots() : nullptr;
    const int bound = slots != nullptr ? core::learn_find_param(slots, core::kLearnSlots, cid) : -1;
    if (bound >= 0) {
      static constexpr const char* kModes[core::kLearnModeCount] = {"PICKUP", "JUMP", "RELATIVE"};
      line.assign("CC ");
      append_uint(line, slots[bound].cc);
      line.append(slots[bound].channel != 0 ? " CH " : " ALL CH");
      if (slots[bound].channel != 0) {
        append_uint(line, slots[bound].channel);
      }
      line.append("  ");
      line.append(kModes[slots[bound].mode % core::kLearnModeCount]);
    } else {
      line.assign("NO CONTROLLER BOUND");
    }
    if ((cd->flags & core::kParamLearnable) != 0u) {  // OS-INSTUI S8a: CHANNEL is not learnable (a controller cannot move a note onto another channel), nor is any other row of the MIDI pages or VEL>: nothing to say about them
      r.text(card.x, y, line.c_str(), bound >= 0 ? Color::Instr : Color::TextDim, Face::Label);
    }
  }

  // The status strip: the title, the transport badge, the instrument, the page, CPU at the right edge.
  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    core::FixedString<24> text;
    row.put(Face::Display, "PARAMS", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    append_dec(text, static_cast<unsigned>(slot + 1), 2);
    text.append(" ");
    const bool midi_inst = inst != nullptr && inst->kind == model::InstrumentKind::Midi;
    std::size_t name_max = sample_inst || midi_inst ? 8 : 12;
    core::FixedString<24> page;  // a module's page, "P1" (the page's name is the header's right-hand "TONE 1/2")
    const char* engine_name = "";
    std::size_t engine_max = 0;
    if (module_slot) {
      // OS-BUG-LEGIB: KIND, ENGINE and PAGE follow the name, and the row must end before the load readouts (it ran into VOX). The cells they leave are shared out: the engine's name (a file
      // stem, up to 31 characters) is clipped first, but leaves the instrument's name 8 cells (as a sample's); the instrument's name takes the rest, up to 12.
      page.assign("P");
      append_uint(page, module_page(model, view) + 1u);
      engine_name = ModuleUi::module_name_of(*model.project, slot);
      const auto field_w = [&](const char* label, const char* value) { return r.text_width(label, Face::Label) + m.gap + r.text_width(value, Face::Data) + 4 * m.gap; };
      const int name_x = row.x() + r.text_width("INS", Face::Label) + m.gap + r.text_width("00 ", Face::Data);
      const int rest = field_w("KIND", kind_name(inst)) + field_w("ENGINE", "") + field_w("PAGE", page.c_str());
      const int cells = (ui::kScreenW - 2 * m.pad - StatusStrip::load_width(r) - name_x - 4 * m.gap - rest) / cell_w;  // (the INS field's own group gap follows the name)
      const std::size_t total = cells > 0 ? static_cast<std::size_t>(cells) : 0u;
      engine_max = total > 8u ? total - 8u : 0u;
      std::size_t engine_len = 0;
      while (engine_len < engine_max && engine_name[engine_len] != '\0') {
        ++engine_len;
      }
      engine_max = engine_len;
      name_max = total - engine_len < name_max ? total - engine_len : name_max;
    }
    append_clipped(text, inst != nullptr && inst->name[0] != '\0' ? inst->name : "--", name_max);
    row.field("INS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    if (sample_inst || midi_inst) {
      // OS-INSTUI S1: "07 NAME  ONE SHOT / MODERN" (read-only): the play mode and the engine it plays with (Auto resolved: "AUTO>AMIGA"); MIDI: "MIDI / CH 3". The kind is F1's label, the page the
      // header's right-hand "VOICE 2/5". The longest (BEAT SLICE / AUTO>12-BIT) is clipped to what the CPU / MEM readout leaves (params_how_clip).
      core::FixedString<40> how;
      if (!sample_inst) {
        how.assign(kind_name(inst));
        how.append(" / ");
      }
      if (sample_inst) {
        core::FixedString<16> engine;
        params_engine_text(model, slot, *inst, engine);
        how.assign(kPlayModeNames[inst->sampler.play_mode & 7u]);
        how.append(" / ");
        how.append(engine.c_str());
      } else {
        how.append("CH ");
        append_uint(how, inst->midi_channel);
      }
      params_how_clip(r, how, row.x());
      row.field("", Face::Data, how.c_str(), Color::ChromeDim, Color::ChromeText);
    } else {
      row.field("KIND", Face::Data, kind_name(inst), Color::ChromeDim, Color::ChromeText);
      if (module_slot) {  // the module's name, and the page of its parameters
        core::FixedString<32> engine;
        append_clipped(engine, engine_name, engine_max);
        row.field("ENGINE", Face::Data, engine.c_str(), Color::ChromeDim, Color::ChromeText);
        row.field("PAGE", Face::Data, page.c_str(), Color::ChromeDim, Color::ChromeText);
      } else {
        row.field("PAGE", Face::Data, chooser ? "CHOOSE" : model::param_group_name(core::ParamKind::Instrument, group), Color::ChromeDim, Color::ChromeText);
      }
    }
    strip.cpu(model.load);
  }

  // The header: the columns' names, the envelope of the ENV page, and where we are among the pages.
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    r.panel({l.value_x + l.value_w - 5 * cell_w - l.grid.cursor_pad_x, f.header.y + f.header.h - 2 * m.border, 5 * cell_w + 2 * l.grid.cursor_pad_x,
             2 * m.border},
            Color::Sel);
    if (!chooser) {
      r.text(l.name_x, cap_y, "PARAMETER", Color::ChromeDim, Face::Label);
      r.text(l.value_x + l.value_w - r.text_width("VALUE", Face::Label), cap_y, "VALUE", Color::ChromeText, Face::Label);
      r.text(l.bar_x, cap_y, "LEVEL", Color::ChromeDim, Face::Label);
    } else {
      r.text(l.name_x, cap_y, "EMPTY SLOT: CHOOSE WHAT GOES HERE", Color::ChromeDim, Face::Label);
    }
    core::FixedString<40> head;
    if (!chooser && group == ip::GroupEnvelope) {
      head.assign("DEST ");  // OS-INSTUI S6: the destination of the modulation (the six envelope slots)
      append_uint(head, envelope + 1u);
      head.append("/6 ");
      head.append(model::envelope_name(envelope));
      r.text(l.card_x, cap_y, head.c_str(), Color::ChromeText, Face::Label);
    }
    if (chooser) {
      head.assign("NEW INSTRUMENT");
    } else if (module_slot) {
      const unsigned pages = module_page_total(model, view);
      const char* named = module_page_at(model, view, module_page(model, view)).name;
      head.assign(named != nullptr && named[0] != '\0' ? named : "PARAMETERS");
      head.append(" ");
      append_uint(head, module_page(model, view) + 1u);
      head.append("/");
      append_uint(head, pages == 0u ? 1u : pages);
    } else if (midi_page) {
      head.assign(group == kCcGroup ? "CC 2/2" : "MIDI 1/2");  // OS-INSTUI S8b: the two pages of a MIDI instrument
    } else {
      head.assign(model::param_group_name(core::ParamKind::Instrument, group));
      head.append(" ");
      const unsigned first = sample_inst ? 0u : 1u;  // (the source page is a sample instrument's)
      append_uint(head, key_of_group(group) - first + 1u);
      head.append("/");
      append_uint(head, ip::kGroups - first);
    }
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(head.c_str(), Face::Label), cap_y, head.c_str(), Color::ChromeText, Face::Label);
  }

  // The info line and the key row.
  if (!hidden(f.info)) {
    core::FixedString<64> text;
    if (module_slot && rows.n == 0) {  // a module instrument whose module is not there (or has no parameters): the project keeps it, it plays nothing
      text.assign("MODULE ");
      append_clipped(text, ModuleUi::module_name_of(*model.project, slot), 24);
      text.append(" NOT LOADED: instrument muted");
    } else if (chooser) {
      text.assign("instrument ");
      append_dec(text, static_cast<unsigned>(slot + 1), 2);
      text.append("   empty   F1..F5: choose");
    } else if (!has_value) {
      text.assign(model.project == nullptr ? "NO PROJECT OPEN: the defaults are shown" : "FREE INSTRUMENT SLOT: the defaults are shown");
    } else if (module_slot && model.modules != nullptr && model.modules->bound_module(*model.project, slot) != nullptr) {
      // OS-GOV: what the render measured of the instrument's module: "TB-303 ~16% PER VOICE" ("UNMEASURED" until a note has sounded), or that the safety net muted it.
      text.assign("instrument ");
      append_dec(text, static_cast<unsigned>(slot + 1), 2);
      text.append("   ");
      if (model.modules->slot_muted(slot)) {
        text.append("MUTED (CPU BUDGET)");
      } else {
        append_clipped(text, model.modules->bound_module(*model.project, slot)->name, 10);
        text.append(" ");
        append_voice_cost(text, model.modules->voice_cost(slot));
      }
      text.append("   encoder: edit");
    } else {
      text.assign("instrument ");
      append_dec(text, static_cast<unsigned>(slot + 1), 2);
      text.append("   encoder: edit   shift: coarse");
    }
    if (group == ip::GroupSource && src.in != nullptr && rows.n != 0 && descs[cursor] != nullptr) {
      // O8: the card's range and default move here, with the reason when the row does not apply (the left side says why, in place of the usual hint).
      core::FixedString<64> range;
      char lo[core::kParamTextChars + 1], hi[core::kParamTextChars + 1], df[core::kParamTextChars + 1];
      value_text(*descs[cursor], descs[cursor]->min, lo);
      value_text(*descs[cursor], descs[cursor]->max, hi);
      value_text(*descs[cursor], descs[cursor]->def, df);
      range.assign("RANGE ");
      range.append(lo);
      range.append(" .. ");
      range.append(hi);
      range.append("   DEFAULT ");
      range.append(df);
      draw_info_line(r, f, row_why[cursor] != nullptr ? row_why[cursor] : text.c_str(), range.c_str());
    } else {
      if (group == ip::GroupEnvelope && !module_slot && !chooser && rows.n != 0) {
        // OS-INSTUI S6: the MOD page's footer: why the destination or the row does not apply (the destination's reason first), else the page's own hint; the keys on the right.
        core::FixedString<72> why;
        if (row_why[cursor] != nullptr) {
          why.assign(row_why[cursor]);
        }
        if (why.size() != 0u) {
          why.append("   MOD again: destination");
          draw_info_line(r, f, why.c_str(), "delete: default   FX1 + FX2: learn");
        } else {
          draw_info_line(r, f, text.c_str(), "MOD again: destination   delete: default");
        }
      } else if (group == ip::GroupFilter && !chooser && rows.n != 0 && row_why[cursor] != nullptr) {
        draw_info_line(r, f, row_why[cursor], "delete: default   FX1 + FX2: learn");
      } else if (midi_page && rows.n != 0) {
        // OS-INSTUI S8b: nothing on these pages can be learned. Program Change has no row of its own: a CC slot set to PROG sends one (FX 37 stays CC F)
        draw_info_line(r, f, text.c_str(), group == kMidiGroup ? "PROGRAM CHANGE: SET A CC TO PROG" : "delete: default   MIDI / CC: page");
      } else {
        draw_info_line(r, f, text.c_str(), chooser ? "an instrument can be any type" : "delete: default   FX1 + FX2: learn");
      }
    }
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    ParamsKeyRow keys;
    params_key_row(model, view, keys);
    ui::soft_keys(r, f.keys, keys.label, 8, keys.dim);
  }

  // The rows' text, one colour pass at a time (the renderer skips a colour that is already set).
  for (const Color pass : {Color::Text, Color::TextDim, Color::Instr, Color::Fx, Color::SelText, Color::AccentText}) {
    for (unsigned i = 0; i < rows.n; ++i) {
      const core::ParamDesc* d = descs[i];
      if (d == nullptr) {
        continue;
      }
      const bool sel = i == cursor;
      const int y = cell_y(i);
      const bool dimmed = row_dim[i];
      if (ink_of(sel, dimmed ? Color::TextDim : Color::Text) == pass && !hidden({l.name_x, y, static_cast<int>(core::kParamNameChars) * cell_w, t.geometry(Face::Data).cell_h})) {
        r.text(l.name_x, y, d->name, pass);
      }
      const int tw = r.text_width(texts[i], Face::Data);
      if ((sel ? Color::AccentText : (dimmed ? Color::TextDim : Color::Instr)) == pass && !hidden({l.value_x + l.value_w - tw, y, tw, t.geometry(Face::Data).cell_h})) {
        r.text(l.value_x + l.value_w - tw, y, texts[i], pass);
      }
      if (d->unit_text[0] != '\0' && ink_of(sel, Color::TextDim) == pass) {
        r.text(l.unit_x, ui::caps_y(t, Face::Label, row_y(i), f.row_pitch), d->unit_text, pass, Face::Label);
      }
      // The controller bound to the row, if any.
      const core::LearnBinding* slots = model.project != nullptr ? model.project->settings().learn_slots() : nullptr;
      const int bound = slots != nullptr ? core::learn_find_param(slots, core::kLearnSlots, id_at(i)) : -1;
      if (bound >= 0 && ink_of(sel, Color::Fx) == pass) {
        core::FixedString<8> tag;
        tag.assign("CC");
        append_uint(tag, slots[bound].cc);
        r.text(l.tag_x, ui::caps_y(t, Face::Label, row_y(i), f.row_pitch), tag.c_str(), pass, Face::Label);
      }
    }
  }
}

constexpr ParamsScreen kParamsScreen{};

}  // namespace o3os::app
