// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/wave_screen.h>

#include "../sample_text.h"

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/ui/components.h>
#include <o3os/ui/plot/wave_view.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;
namespace ip = model::instrument_param;

namespace {

constexpr int kLaneX = 16;
constexpr int kLaneW = 768;  // the width one click of the encoder is measured in (wave_actions.cpp kLaneColumns)
constexpr int kLaneTop = 18;
constexpr int kLaneH = 224;

struct Sample {
  unsigned slot = 0;
  const model::Instrument* inst = nullptr;
  SampleBank::Pcm pcm;
  bool valid = false;  // a sample instrument with audio
};

Sample sample_of(const Model& model, const ViewState& view) noexcept {
  Sample s;
  s.slot = wave_slot(view);
  s.inst = model.project != nullptr ? model.project->instrument(s.slot) : nullptr;
  s.pcm = model.samples != nullptr ? model.samples->pcm(s.slot) : SampleBank::Pcm{};
  s.valid = s.inst != nullptr && s.inst->kind == model::InstrumentKind::Sample && s.pcm.frames != 0u;
  return s;
}

Points points_of(const model::Instrument& in) noexcept {
  Points p;
  p.start = in.sampler.start;
  p.loop_start = in.sampler.loop_start;
  p.loop_end = in.sampler.loop_end;
  p.end = in.sampler.end;
  return p;
}

// True when the play mode loops (Loop fwd, Loop bwd, Ping-pong: 1..3), so that LOOP START and LOOP END are used (OS-AUDIT E-PLAY-03).
bool wave_loops(const model::Instrument& in) noexcept {
  const unsigned mode = in.sampler.play_mode & 7u;
  return mode >= 1u && mode <= 3u;
}

unsigned slices_of(const model::Instrument& in) noexcept {
  return in.sampler.slice_count > kMaxSliceCount ? kMaxSliceCount : in.sampler.slice_count;
}

// The x of a frame on the lane, or -1 when the window does not show it.
int lane_x(const Window& w, std::uint32_t frame) noexcept {
  if (frame < w.first || frame > w.first + w.span || w.span == 0u) {
    return -1;
  }
  return kLaneX + static_cast<int>(static_cast<std::uint64_t>(frame - w.first) * static_cast<std::uint64_t>(kLaneW - 1) / w.span);
}

bool peaks_match(const ui::plot::PeakCache* peaks, const SampleBank::Pcm& pcm) noexcept {
  return peaks != nullptr && peaks->valid() && peaks->built() && peaks->samples() == pcm.data && peaks->frames() == pcm.frames;
}

const char* const kPointNames[kPointCount] = {"START", "LOOP START", "LOOP END", "END"};
const char* const kPointFlags[kPointCount] = {"S", "LS", "LE", "E"};

}  // namespace

unsigned wave_slot(const ViewState& view) noexcept {
  return view.instrument >= 1 ? static_cast<unsigned>(view.instrument - 1) : 0u;
}

void wave_view_reset(ViewState& view) noexcept {
  view.wave.first = view.wave.span = 0;
  view.wave.slice = 0;
  view.sedit.first = view.sedit.span = 0;
  view.sedit.a = view.sedit.b = 0;
}

Window wave_window(const WaveState& state, std::uint32_t frames) noexcept {
  if (state.span == 0u) {
    return window_fit(frames);
  }
  return window_clamp(Window{state.first, state.span}, frames);
}

// The frame the zoom keeps still: the selected point's or slice's start when the window shows it, else the middle of the window.
static std::uint32_t anchor_of(const Model& model, const ViewState& view, const Sample& s, const Window& w) noexcept {
  std::uint32_t anchor = w.first + w.span / 2u;
  if (!s.valid) {
    return anchor;
  }
  std::uint32_t frame = 0;
  const WaveView wv = wave_view_of(model, view);
  if (wv == WaveView::Wavetable || wv == WaveView::Granular) {  // the window / the grain: where the zoom keeps still
    const FrameRange range = wv == WaveView::Wavetable ? wt_window_frames(s.pcm.frames, s.inst->sampler.wavetable_window_size, s.inst->sampler.wavetable_window)
                                                      : grain_frames(s.pcm.frames, s.inst->sampler.grain_position, s.inst->sampler.grain_length);
    frame = range.valid ? range.begin + (range.end - range.begin) / 2u : anchor;
  } else {
    std::uint32_t point = 0;
    if (wv == WaveView::Wave) {
      point = get_point(points_of(*s.inst), static_cast<PointId>(view.wave.point < kPointCount ? view.wave.point : 0u));
    } else if (view.wave.slice < slices_of(*s.inst)) {
      point = s.inst->sampler.slices[view.wave.slice];
    } else {
      return anchor;
    }
    frame = point_to_frame(point, s.pcm.frames);
  }
  return (frame >= w.first && frame <= w.first + w.span) ? frame : anchor;
}

bool wave_zoom(const Model& model, ViewState& view, int dir) noexcept {
  const Sample s = sample_of(model, view);
  if (s.pcm.frames == 0u || dir == 0) {
    return false;
  }
  const Window w = wave_window(view.wave, s.pcm.frames);
  const Window z = window_zoom(w, s.pcm.frames, dir, anchor_of(model, view, s, w));
  if (z.first == w.first && z.span == w.span) {
    return false;
  }
  view.wave.first = z.first;
  view.wave.span = z.span >= s.pcm.frames ? 0u : z.span;  // all of it again is fit
  return true;
}

bool wave_sync(const Model& model, ViewState& view) noexcept {
  const Sample s = sample_of(model, view);
  WaveState& ws = view.wave;
  const WaveState before = ws;
  const WaveView wv = wave_view_of(model, view);
  const unsigned rows = wave_source_rows(wv) != 0u ? wave_source_rows(wv) : kPointCount;  // the WAVETABLE and GRANULAR views keep their row here, 2 or 4 of them
  if (ws.point >= rows) {
    ws.point = 0;
  }
  if (rows != kPointCount) {
    view.page[screen_index(ScreenId::Wave)] = 0;  // they are one page
  }
  const unsigned count = s.inst != nullptr ? slices_of(*s.inst) : 0u;
  if (count == 0u) {
    ws.slice = 0;
  } else if (ws.slice >= count) {
    ws.slice = static_cast<std::uint8_t>(count - 1u);
  }
  if (s.pcm.frames == 0u || ws.span >= s.pcm.frames) {
    ws.first = 0;
    ws.span = 0;
  } else if (ws.span != 0u) {
    const Window w = window_clamp(Window{ws.first, ws.span}, s.pcm.frames);
    ws.first = w.first;
    ws.span = w.span;
  }
  return ws.point != before.point || ws.slice != before.slice || ws.first != before.first || ws.span != before.span;
}

const char* const* WaveScreen::shift_row(const ViewState& view) const noexcept {
  return view.page[screen_index(ScreenId::Wave)] == 1u ? kChopShiftKeys : kWaveShiftKeys;  // page 1 is the CHOP view (the WAVETABLE and GRANULAR ones reset to page 0)
}

WaveView wave_view_of(const Model& model, const ViewState& view) noexcept {
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(wave_slot(view)) : nullptr;
  if (in != nullptr && in->kind == model::InstrumentKind::Sample) {
    if (in->sampler.play_mode == kPlayModeWavetable) {
      return WaveView::Wavetable;
    }
    if (in->sampler.play_mode == kPlayModeGranular) {
      return WaveView::Granular;
    }
  }
  return view.page[screen_index(ScreenId::Wave)] == 1u ? WaveView::Chop : WaveView::Wave;
}

unsigned wave_source_rows(WaveView v) noexcept { return v == WaveView::Wavetable ? 2u : (v == WaveView::Granular ? 4u : 0u); }

// Arriving on the screen: the play mode picks the page (OS-INSTUI S4, ui-map): the slice modes are the CHOP page's and every other mode the WAVE page, whatever page was left (a CHOP page left
// open on a one-shot, or kept across a project load, is not remembered). An instrument that is no sample instrument keeps the page it had.
void WaveScreen::enter(const Model& model, ViewState& view) const noexcept {
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(wave_slot(view)) : nullptr;
  if (in != nullptr && in->kind == model::InstrumentKind::Sample) {
    view.page[screen_index(ScreenId::Wave)] = play_mode_is_slice(in->sampler.play_mode) ? 1 : 0;
  }
  (void)wave_sync(model, view);
}

bool WaveScreen::repress(const Model& model, ViewState& view) const noexcept {
  const WaveView v = wave_view_of(model, view);
  if (v == WaveView::Wavetable || v == WaveView::Granular) {
    return false;  // one page: the play mode picks the view
  }
  std::uint8_t& page = view.page[screen_index(ScreenId::Wave)];
  page = static_cast<std::uint8_t>((page + 1u) % pages());
  view.params.discard_pending();
  return true;
}

Context WaveScreen::field_context(const Model& model, const ViewState& view) const noexcept {
  switch (wave_view_of(model, view)) {
    case WaveView::Chop: return Context::WaveChop;
    case WaveView::Wavetable: return Context::WaveWt;
    case WaveView::Granular: return Context::WaveGran;
    case WaveView::Wave: break;
  }
  return Context::None;
}

core::ParamId WaveScreen::focus_param(const Model& model, const ViewState& view) const noexcept {
  const WaveView v = wave_view_of(model, view);
  static constexpr unsigned kWt[2] = {ip::WtPos, ip::WtSize};
  static constexpr unsigned kGran[4] = {ip::GrainPos, ip::GrainLength, ip::GrainShape, ip::GrainLoop};
  const unsigned at = view.wave.point;
  if (v == WaveView::Wavetable && at < 2u) {
    return core::param_id(core::ParamKind::Instrument, wave_slot(view), kWt[at]);
  }
  if (v == WaveView::Granular && at < 4u) {
    return core::param_id(core::ParamKind::Instrument, wave_slot(view), kGran[at]);
  }
  return core::ParamId::None;
}

int WaveScreen::count(const Model& model, const ViewState& view) const noexcept {
  const WaveView v = wave_view_of(model, view);
  if (wave_source_rows(v) != 0u) {
    return static_cast<int>(wave_source_rows(v));
  }
  if (v == WaveView::Wave) {
    return static_cast<int>(kPointCount);
  }
  const Sample s = sample_of(model, view);
  return s.inst != nullptr ? static_cast<int>(slices_of(*s.inst)) : 0;
}

int WaveScreen::position(const ViewState& view) const noexcept {
  return view.page[screen_index(ScreenId::Wave)] == 1u ? static_cast<int>(view.wave.slice) : static_cast<int>(view.wave.point);
}

bool WaveScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int n = count(model, view);
  if (n <= 0) {
    return false;
  }
  const std::uint8_t to = static_cast<std::uint8_t>(position < 0 ? 0 : (position >= n ? n - 1 : position));
  // The WAVETABLE and GRANULAR views keep their row in `point` (the page value is whatever was left: it does not say which cursor this is).
  std::uint8_t& field = (wave_view_of(model, view) == WaveView::Chop) ? view.wave.slice : view.wave.point;
  const bool changed = field != to;
  field = to;
  return changed;
}

bool WaveScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  if (dy != 0) {
    return dx == 0 && wave_zoom(model, view, dy < 0 ? 1 : -1);  // Up zooms in, Down out
  }
  const int at = wave_view_of(model, view) == WaveView::Chop ? static_cast<int>(view.wave.slice) : static_cast<int>(view.wave.point);
  return dx != 0 && set_position(model, view, at + dx);
}

bool WaveScreen::pad_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) const noexcept {
  if (wave_view_of(model, view) != WaveView::Chop) {
    return false;
  }
  const Sample s = sample_of(model, view);
  const unsigned count = s.valid ? slices_of(*s.inst) : 0u;
  for (unsigned n = 0; n < 48u; ++n) {
    roles[n] = n < count ? (n == view.wave.slice ? PadRole::Front : PadRole::Back) : PadRole::Off;
  }
  return true;
}

void WaveScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = make_frame(t, kFrameRows);
  const Sample s = sample_of(model, view);
  const WaveView wv = wave_view_of(model, view);
  const bool chop = wv == WaveView::Chop;
  const bool src = wv == WaveView::Wavetable || wv == WaveView::Granular;  // OS-INSTUI S4: the window / the grain, edited as the source page's rows
  const WaveState& ws = view.wave;
  const std::uint32_t frames = s.pcm.frames;
  const Window w = wave_window(ws, frames);
  const Points pts = s.valid ? points_of(*s.inst) : Points{};
  const unsigned slices = s.valid ? slices_of(*s.inst) : 0u;
  const unsigned selected_slice = ws.slice < slices ? ws.slice : 0u;
  const unsigned selected_point = ws.point < kPointCount ? ws.point : 0u;
  const bool looped = s.valid && wave_loops(*s.inst);  // OS-AUDIT E-PLAY-03: the loop points count only in the loop modes (1..3): else they are dimmed and their marks are not drawn
  const unsigned rate = s.inst != nullptr ? s.inst->sample_rate : 0u;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const Rect lane = {kLaneX, f.pane.y + kLaneTop, kLaneW, kLaneH};

  draw_chrome_bands(r, f, covered);

  // The status strip: the page, the transport, the instrument, then what the page is about.
  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    ui::FieldRow& row = strip.row();
    core::FixedString<24> text;
    row.put(Face::Display, wv == WaveView::Wavetable ? "WAVETABLE" : (wv == WaveView::Granular ? "GRANULAR" : (chop ? "CHOP" : "WAVE")), Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    append_dec(text, wave_slot(view) + 1u, 2);
    if (s.inst != nullptr && s.inst->name[0] != '\0') {
      text.append(" ");
      append_clipped(text, s.inst->name, 10);
    }
    row.field("INS", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    text.clear();
    if (chop) {
      append_uint(text, slices);
      row.field("SLICES", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    } else {
      append_seconds(text, frames, rate);
      row.field("LENGTH", Face::Data, text.c_str(), Color::ChromeDim, Color::ChromeText);
    }
    if (!src) {
      row.field("ZC", Face::Data, ws.zc ? "ON" : "OFF", Color::ChromeDim, Color::ChromeText);
    }
    strip.cpu(model.load);
  }

  // The header: the two pages, the one in view underlined.
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    const int w_wave = r.text_width("WAVE", Face::Label);
    if (src) {  // one page: the view is the play mode's
      const char* name = wv == WaveView::Wavetable ? "WAVETABLE" : "GRANULAR";
      r.text(kLaneX, cap_y, name, Color::ChromeText, Face::Label);
      r.panel({kLaneX, f.header.y + f.header.h - 2 * m.border, r.text_width(name, Face::Label), 2 * m.border}, Color::Sel);
    } else {
      r.text(kLaneX, cap_y, "WAVE", chop ? Color::ChromeDim : Color::ChromeText, Face::Label);
      r.text(kLaneX + w_wave + 4 * m.gap, cap_y, "CHOP", chop ? Color::ChromeText : Color::ChromeDim, Face::Label);
      const int ux = chop ? kLaneX + w_wave + 4 * m.gap : kLaneX;
      r.panel({ux, f.header.y + f.header.h - 2 * m.border, chop ? r.text_width("CHOP", Face::Label) : w_wave, 2 * m.border}, Color::Sel);
    }
    // The play mode, and the key that changes it (here Shift + F1): the one place the WAVE and CHOP pages say it.
    core::FixedString<48> hint;
    hint.assign("MODE ");
    hint.append(s.inst != nullptr && s.inst->kind == model::InstrumentKind::Sample ? kPlayModeNames[s.inst->sampler.play_mode & 7u] : "-");
    hint.append("   SHIFT+F1: CHANGE");
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(hint.c_str(), Face::Label), cap_y, hint.c_str(), Color::ChromeDim, Face::Label);
  }

  // The lane: the waveform, the loop and the selected slice as marks, then the points and the slices as lines.
  if (!hidden(lane)) {
    r.frame({lane.x - 1, lane.y - 1, lane.w + 2, lane.h + 2}, Color::TextDim);
    if (!s.valid) {
      const char* none = frames == 0u ? "NO SAMPLE IN THIS INSTRUMENT" : "NOT A SAMPLE INSTRUMENT";
      r.text(lane.x + (lane.w - r.text_width(none, Face::Data)) / 2, lane.y + lane.h / 2 - 10, none, Color::TextDim, Face::Data);
      const char* how = "load one on SAMPLE LOAD; a pad chooses the instrument";
      r.text(lane.x + (lane.w - r.text_width(how, Face::Label)) / 2, lane.y + lane.h / 2 + 18, how, Color::TextDim, Face::Label);
    } else if (!peaks_match(model.peaks, s.pcm)) {
      const char* wait = "BUILDING THE WAVEFORM";
      r.text(lane.x + (lane.w - r.text_width(wait, Face::Data)) / 2, lane.y + lane.h / 2 - 10, wait, Color::TextDim, Face::Data);
    } else {
      ui::plot::WaveMarks marks;
      marks.zero_line = true;
      if (model.head.on && model.head.slot == wave_slot(view)) {  // the playhead of the last live voice (OS-FU3)
        marks.has_playhead = true;
        marks.playhead = model.head.frame;
      }
      FrameRange range;  // OS-INSTUI S4: the wavetable's window / the grain, marked as the selection
      if (wv == WaveView::Wavetable) {
        range = wt_window_frames(frames, s.inst->sampler.wavetable_window_size, s.inst->sampler.wavetable_window);
      } else if (wv == WaveView::Granular) {
        range = grain_frames(frames, s.inst->sampler.grain_position, s.inst->sampler.grain_length);
      }
      if (wv == WaveView::Wave) {
        marks.has_loop = looped;
        marks.loop_start = point_to_frame(pts.loop_start, frames);
        marks.loop_end = point_to_frame(pts.loop_end, frames);
      } else if (src) {
        marks.has_selection = range.valid;
        marks.sel_begin = range.begin;
        marks.sel_end = range.end;
      } else if (slices != 0u) {
        marks.has_selection = true;
        marks.sel_begin = point_to_frame(s.inst->sampler.slices[selected_slice], frames);
        marks.sel_end = selected_slice + 1u < slices ? point_to_frame(s.inst->sampler.slices[selected_slice + 1u], frames) : frames;
      }
      (void)ui::plot::draw_wave(r, lane, *model.peaks, 0, ui::plot::WaveWindow{w.first, w.span}, ui::plot::WaveMode::Vector, nullptr, 0, 0, marks,
                                ui::plot::WaveStyle{});
      if (src && range.valid) {  // the window's start / the grain's centre as a line in the playing colour, wider while the position is the row being edited
        const std::uint32_t at = wv == WaveView::Wavetable ? range.begin : range.begin + (range.end - range.begin) / 2u;
        const int x = lane_x(w, at);
        if (x >= 0) {
          const int lw = selected_point == 0u ? 4 : 2;
          r.panel({x - lw / 2, lane.y, lw, lane.h}, Color::Play);
        }
      }
      // START and END, always: a line each (the selected one, on the WAVE page, wider and in the playing colour), and its flag over the lane.
      for (unsigned i = 0; i < kPointCount && !src; ++i) {
        const bool edge = i == static_cast<unsigned>(PointId::Start) || i == static_cast<unsigned>(PointId::End);
        if (!edge && (chop || !looped)) {  // the loop flags only where the play mode loops
          continue;
        }
        const int x = lane_x(w, point_to_frame(get_point(pts, static_cast<PointId>(i)), frames));
        if (x < 0 || !edge) {
          if (x >= 0 && !chop) {
            r.text(x - 4, lane.y - 14, kPointFlags[i], i == selected_point ? Color::Play : Color::Fx, Face::Micro);
          }
          continue;
        }
        const bool sel = !chop && i == selected_point;
        const int lw = sel ? 4 : 2;
        r.panel({x - lw / 2, lane.y, lw, lane.h}, sel ? Color::Play : Color::Accent);
        r.text(x - 3, lane.y - 14, kPointFlags[i], sel ? Color::Play : Color::Accent, Face::Micro);
      }
      if (wv == WaveView::Wave && looped) {  // the loop's two edges are the marks' (draw_wave); the selected one gets its line too
        const unsigned sp = selected_point;
        if (sp == static_cast<unsigned>(PointId::LoopStart) || sp == static_cast<unsigned>(PointId::LoopEnd)) {
          const int x = lane_x(w, point_to_frame(get_point(pts, static_cast<PointId>(sp)), frames));
          if (x >= 0) {
            r.panel({x - 2, lane.y, 4, lane.h}, Color::Play);
          }
        }
      }
      if (chop) {  // the slices: a line at each start, the selected one wider; a number under the lane when there is room
        int last_label_end = -1000;
        for (unsigned i = 0; i < slices; ++i) {
          const int x = lane_x(w, point_to_frame(s.inst->sampler.slices[i], frames));
          if (x < 0) {
            continue;
          }
          const bool sel = i == selected_slice;
          r.panel({x - (sel ? 1 : 0), lane.y, sel ? 3 : 1, lane.h}, sel ? Color::Accent : Color::Text);
          core::FixedString<4> num;
          append_uint(num, i + 1u);
          const int tw = r.text_width(num.c_str(), Face::Micro);
          if (x + 2 >= last_label_end + 3 && x + 2 + tw <= lane.x + lane.w) {
            r.text(x + 2, lane.y + lane.h + 3, num.c_str(), sel ? Color::Accent : Color::TextDim, Face::Micro);
            last_label_end = x + 2 + tw;
          }
        }
      }
    }
  }

  // Under the lane: the window's ends in seconds.
  if (s.valid) {
    core::FixedString<16> a;
    core::FixedString<16> b;
    append_seconds(a, w.first, rate);
    append_seconds(b, w.first + w.span, rate);
    if (!chop && !hidden({kLaneX, lane.y + lane.h + 3, kLaneW, 12})) {  // (the WAVE page and the source views)
      r.text(kLaneX, lane.y + lane.h + 3, a.c_str(), Color::TextDim, Face::Micro);
      r.text(kLaneX + kLaneW - r.text_width(b.c_str(), Face::Micro), lane.y + lane.h + 3, b.c_str(), Color::TextDim, Face::Micro);
    }
  }

  // The readout.
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const int ry = lane.y + lane.h + 22;
  if (src) {
    // OS-INSTUI S4: the rows of the WAVETABLE (position, window) and GRANULAR (position, length, shape, loop) views, named and valued as the source page has them (the same descriptors, the same words).
    const unsigned n = wave_source_rows(wv);
    static constexpr unsigned kWtFields[2] = {ip::WtPos, ip::WtSize};
    static constexpr unsigned kGranFields[4] = {ip::GrainPos, ip::GrainLength, ip::GrainShape, ip::GrainLoop};
    static constexpr const char* kWtNames[2] = {"POSITION", "WINDOW"};
    static constexpr const char* kGranNames[4] = {"POSITION", "LENGTH", "SHAPE", "LOOP"};
    for (unsigned i = 0; i < n; ++i) {
      const Rect cell = {kLaneX + static_cast<int>(i % 2u) * 392, ry + static_cast<int>(i / 2u) * (f.row_pitch + 6), 376, f.row_pitch};
      if (hidden(cell)) {
        continue;
      }
      const bool sel = i == selected_point;
      if (sel) {
        r.highlight_row(cell);
      }
      const int ty = cell.y + (f.row_pitch - cell_h) / 2;
      r.text(cell.x + 6, ty, wv == WaveView::Wavetable ? kWtNames[i] : kGranNames[i], sel ? Color::SelText : Color::TextDim);
      if (s.valid) {
        char value[core::kParamTextChars + 1];
        params_source_text(model, wave_slot(view), wv == WaveView::Wavetable ? kWtFields[i] : kGranFields[i], value);
        core::FixedString<40> text;
        text.assign(value);
        const core::ParamDesc* d = model::param_desc(core::param_id(core::ParamKind::Instrument, wave_slot(view), wv == WaveView::Wavetable ? kWtFields[i] : kGranFields[i]));
        if (d != nullptr && d->unit_text[0] != '\0') {
          text.append(" ");
          text.append(d->unit_text);
        }
        if (wv == WaveView::Wavetable && i == 0u) {  // how far the table goes: the window's number among the windows
          text.append("  OF 0..");
          const unsigned windows = wt_windows(frames, wt_play_size(s.inst->sampler.wavetable_window_size, frames));
          append_uint(text, windows != 0u ? windows - 1u : 0u);
        }
        r.text(cell.x + 6 + 11 * cell_w, ty, text.c_str(), sel ? Color::SelText : Color::Text);
      }
    }
  } else if (!chop) {
    for (unsigned i = 0; i < kPointCount; ++i) {
      const Rect cell = {kLaneX + static_cast<int>(i % 2u) * 392, ry + static_cast<int>(i / 2u) * (f.row_pitch + 6), 376, f.row_pitch};
      if (hidden(cell)) {
        continue;
      }
      const bool sel = i == selected_point;
      if (sel) {
        r.highlight_row(cell);
      }
      const bool unused = s.valid && !looped && (i == static_cast<unsigned>(PointId::LoopStart) || i == static_cast<unsigned>(PointId::LoopEnd));  // OS-AUDIT E-PLAY-03: dim, not hidden
      const Color c = sel ? Color::SelText : (unused ? Color::TextDim : Color::Text);
      const int ty = cell.y + (f.row_pitch - cell_h) / 2;
      r.text(cell.x + 6, ty, kPointNames[i], sel ? Color::SelText : Color::TextDim);
      if (s.valid) {
        core::FixedString<40> text;
        const std::uint16_t pt = get_point(pts, static_cast<PointId>(i));
        append_dec(text, pt, 5);
        text.append("  ");
        append_uint(text, point_to_frame(pt, frames));
        text.append(" fr");
        r.text(cell.x + 6 + 11 * cell_w, ty, text.c_str(), c);
      }
    }
    // The loop's levels (OS-REVIEW, new Community OS behaviour): the sample at LOOP START and at LOOP END and the jump between them, as fractions of full scale, to judge a
    // seamless loop by eye. A row of its own under the points (the cells above are full: 11 columns of name and up to 17 of value in 37). "--" when the instrument does not
    // loop (play mode 1..3) or its loop is one the engine would play as a one shot (og_samples.cpp: start <= loop start < loop end <= end, in frames). Read here, on the main loop.
    const Rect lrow = {kLaneX, ry + 2 * (f.row_pitch + 6), 376, f.row_pitch};
    if (s.valid && !hidden(lrow)) {
      const std::uint32_t fs = point_to_frame(pts.start, frames);
      const std::uint32_t fe = point_to_frame(pts.end, frames);
      const std::uint32_t fls = point_to_frame(pts.loop_start, frames);
      const std::uint32_t fle = point_to_frame(pts.loop_end, frames);
      const bool loops = wave_loops(*s.inst) && fls >= fs && fls < fle && fle <= fe;
      const int ly = lrow.y + (f.row_pitch - cell_h) / 2;
      r.text(lrow.x + 6, ly, "LOOP LEVEL", Color::TextDim);
      core::FixedString<40> text;
      const LoopLevels lv = loops ? loop_levels(s.pcm.data, frames, fls, fle) : LoopLevels{};
      if (lv.valid) {
        char num[7];
        text.append("START ");
        level_text(num, lv.at_start, true);
        text.append(num);
        text.append("   END ");
        level_text(num, lv.at_end, true);
        text.append(num);
        text.append("   JUMP ");
        level_text(num, static_cast<std::int32_t>(lv.jump), false);
        text.append(num);
      } else {
        text.assign("--");
      }
      r.text(lrow.x + 6 + 11 * cell_w, ly, text.c_str(), Color::Text);
    }
  } else {
    const Rect line = {kLaneX, ry, 376, f.row_pitch};
    if (!hidden(line) && s.valid) {
      core::FixedString<48> text;
      text.assign("SLICE ");
      append_dec(text, selected_slice + 1u, 2);
      text.append("/");
      append_uint(text, slices);
      if (slices != 0u) {
        const unsigned at = s.inst->sampler.slices[selected_slice];
        text.append("  START ");
        append_dec(text, at, 5);
        text.append("  LEN ");
        const std::uint32_t from = point_to_frame(at, frames);
        const std::uint32_t to = selected_slice + 1u < slices ? point_to_frame(s.inst->sampler.slices[selected_slice + 1u], frames) : frames;
        append_seconds(text, to > from ? to - from : 0u, rate);
      }
      r.text(line.x + 6, line.y + (f.row_pitch - cell_h) / 2, text.c_str(), Color::Text);
    }
    const char* const hints[3] = {"F1 AUTO: one slice per hit", "F2 EVEN: equal slices (shift + up / down: n)", "pad n = slice n   shift + pad: select"};
    for (int i = 0; i < 3; ++i) {
      const int hy = ry + (i + 1) * (f.row_pitch - 2);
      if (!hidden({kLaneX, hy, 376, 16})) {
        r.text(kLaneX + 6, hy + 2, hints[i], Color::TextDim, Face::Label);
      }
    }
    // The pads, as the board has them: 4 rows of 12; a pad that has a slice is filled, the selected one in the hot colour.
    const int gx = 408;
    const int pitch = 17;
    for (unsigned n = 0; n < 48u; ++n) {
      const Rect pad = {gx + static_cast<int>(n % 12u) * pitch, ry + static_cast<int>(n / 12u) * pitch, 14, 14};
      if (hidden(pad)) {
        continue;
      }
      if (n < slices) {
        r.panel(pad, n == selected_slice ? Color::Accent : Color::Sel);
      } else {
        r.frame(pad, Color::TextDim);
      }
    }
    core::FixedString<24> even;
    even.assign("EVEN ");
    append_uint(even, ws.even);
    r.text(gx, ry + 4 * pitch + 2, even.c_str(), Color::TextDim, Face::Label);
  }

  // The info line and the key row.
  if (!hidden(f.info)) {
    core::FixedString<64> left;
    if (!s.valid) {
      left.assign("no sample");
    } else if (model.samples != nullptr && model.samples->rebind_pending()) {
      left.assign("SOUNDS WHEN STOPPED");
    } else if (chop) {
      left.assign(s.inst->sampler.play_mode == 5u ? "beat slice: pads play the slices" : "not in beat slice mode: ADD or EVEN switch it on");
    } else if (wv == WaveView::Wave && !looped && (selected_point == static_cast<unsigned>(PointId::LoopStart) || selected_point == static_cast<unsigned>(PointId::LoopEnd))) {
      left.assign("LOOP POINTS: LOOP FWD, BWD OR PING-PONG ONLY");  // OS-AUDIT E-PLAY-03 (O8): the row stays, dimmed, and says why
    } else if (src) {
      left.assign(wv == WaveView::Wavetable ? "wavetable: a window as an oscillator" : "granular: a grain at a position");
    } else {
      append_uint(left, frames);
      left.append(" frames   ");
      append_uint(left, rate != 0u ? rate : 44100u);
      left.append(" Hz");
    }
    draw_info_line(r, f, left.c_str(),
                   src ? "left / right: row   up / down: zoom   encoder: edit" : (chop ? "left / right: slice   up / down: zoom   encoder: move" : "left / right: point   up / down: zoom   encoder: move"));
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    ui::soft_keys(r, f.keys, key_row(*this, view, wv == WaveView::Wavetable ? kWtSoftKeys : (wv == WaveView::Granular ? kGranSoftKeys : (chop ? kChopSoftKeys : kWaveSoftKeys))), 8);
  }
}

constexpr WaveScreen kWaveScreen{};

}  // namespace o3os::app
