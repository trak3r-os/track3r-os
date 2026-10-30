// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/module_ui.h>

#include <o3os/app/actions.h>
#include <o3os/app/app_export.h>
#include <o3os/app/key_prompt.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/eve/dl.h>
#include <o3os/model/params.h>
#include <o3os/ui/plot/curve.h>

#include <atomic>
#include <cstddef>
#include <cstring>

namespace o3os::app {

namespace {

namespace dl = eve::dl;
namespace plot = ui::plot;

// The SDK's colour roles and faces are the theme's tokens in the theme's order: the module never sees an RGB value.
static_assert(O3M_COLOR_COUNT + 1 == ui::kColorCount && O3M_COLOR_CHROME == static_cast<unsigned>(ui::Color::Chrome) && O3M_COLOR_PANE == static_cast<unsigned>(ui::Color::Pane) &&
                  O3M_COLOR_TEXT == static_cast<unsigned>(ui::Color::Text) && O3M_COLOR_INSTR == static_cast<unsigned>(ui::Color::Instr) &&
                  O3M_COLOR_SEL == static_cast<unsigned>(ui::Color::Sel) && O3M_COLOR_ACCENT == static_cast<unsigned>(ui::Color::Accent) &&
                  O3M_COLOR_PLAY_TEXT == static_cast<unsigned>(ui::Color::PlayText),
              "O3M_COLOR_* follow ui::Color (the SDK has the first 16: Rec, the 17th, is the host's alone)");
static_assert(O3M_FACE_COUNT == ui::kFaceCount && O3M_FACE_DISPLAY == static_cast<unsigned>(ui::Face::Display) && O3M_FACE_MICRO == static_cast<unsigned>(ui::Face::Micro),
              "O3M_FACE_* follow ui::Face");
static_assert(static_cast<unsigned>(O3M_AXIS_LOG_FREQ) == static_cast<unsigned>(plot::AxisKind::LogFreq) && static_cast<unsigned>(O3M_AXIS_DB) == static_cast<unsigned>(plot::AxisKind::Db),
              "O3M_AXIS_* follow plot::AxisKind");

constexpr std::uint32_t kMinViewWords = 16;  // a view is not started with less of the page's share than this

constexpr const char* kUnitText[O3M_UNIT_COUNT] = {"", "%", "HZ", "MS", "DB", "ST", "S", "CT"};
constexpr const char* kBoolNames[2] = {"OFF", "ON"};

bool same_ci(const char* a, const char* b) noexcept {
  for (;; ++a, ++b) {
    const char x = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a + 32) : *a;
    const char y = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b + 32) : *b;
    if (x != y) {
      return false;
    }
    if (x == '\0') {
      return true;
    }
  }
}

int compare_ci(const char* a, const char* b) noexcept {
  for (;; ++a, ++b) {
    const char x = *a >= 'A' && *a <= 'Z' ? static_cast<char>(*a + 32) : *a;
    const char y = *b >= 'A' && *b <= 'Z' ? static_cast<char>(*b + 32) : *b;
    if (x != y) {
      return x < y ? -1 : 1;
    }
    if (x == '\0') {
      return 0;
    }
  }
}

void copy_text(char* dst, std::size_t cap, const char* src) noexcept {
  std::size_t n = 0;
  while (src[n] != '\0' && n + 1 < cap) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

// OS-H4: the once-per-boot confirm of an app that edits the project and/or writes files, as ONE message that fits the notification strip (ModuleUi::kConfirmChars, core::kNotifyTextChars) and the TOOLS
// dialog's line. "EDITS THE PROJECT (UNDOABLE): F1 AGAIN" for an edit app (the text it always had), "WRITES FILES TO APPEXPORT/<APP>: F1 AGAIN" for an export app (<APP> is the folder the app writes to, in upper
// case, cut with ".." when the line would be too long), "EDITS (UNDOABLE) + WRITES FILES: F1 AGAIN" for an app with both bits (no room for the folder as well; the app's page and the guide name it).
void compose_confirm(std::uint32_t caps, const char* stem, char* out, std::size_t cap) noexcept {
  const bool edit = (caps & O3M_CAP_EDIT_PROJECT) != 0u;
  const bool write = (caps & O3M_CAP_EXPORT_FILES) != 0u;
  const char* head = edit && write ? "EDITS (UNDOABLE) + WRITES FILES" : (write ? "WRITES FILES TO APPEXPORT/" : "EDITS THE PROJECT (UNDOABLE)");
  AskText tail_text;  // ": F1 AGAIN", from the question's key table (OS-KEYROWDLG)
  ask_tail(Ask::Launch, tail_text);
  const char* tail = tail_text.c_str();
  std::size_t n = 0;
  const auto put = [&](const char* text) {
    for (; *text != '\0' && n + 1 < cap; ++text) {
      out[n++] = *text;
    }
  };
  put(head);
  if (write && !edit) {
    const std::size_t room = cap - 1 - n - std::strlen(tail);  // (cap is kConfirmChars + 1: the head and the tail leave room for a name)
    const std::size_t len = std::strlen(stem);
    const std::size_t take = len <= room ? len : room - 2;
    for (std::size_t i = 0; i < take; ++i) {
      const char c = stem[i];
      out[n++] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
    }
    if (len > room) {
      put("..");
    }
  }
  put(tail);
  out[n] = '\0';
}

std::uint32_t clock_us(void* ctx) noexcept { return static_cast<const hal::Clock*>(ctx)->now(); }

// OS-H1: the app host tables. The functions of an SDK table take no context, so they reach the ModuleUi of the one running app through this pointer: set by launch_app() before init(),
// cleared when the app will not be called again (ModuleUi::release_host). Main loop only (every call comes from the app's own entry points), so it needs no protection.
ModuleUi* g_host = nullptr;

std::uint32_t tramp_get_tempo() { return g_host != nullptr ? g_host->host_tempo() : 0u; }
std::uint32_t tramp_launch_ctx(o3m_launch_ctx* out) { return g_host != nullptr ? g_host->host_launch_ctx(out) : 0u; }
int tramp_set_tempo(std::uint32_t bpm_x100) { return g_host != nullptr ? g_host->host_set_tempo(bpm_x100) : O3M_EDIT_NO_PROJECT; }
std::uint32_t tramp_now_ms() { return g_host != nullptr ? g_host->host_now_ms() : 0u; }
std::uint32_t tramp_midi_read(o3m_midi_event* out, std::uint32_t max) { return g_host != nullptr ? g_host->host_midi_read(out, max) : 0u; }
void tramp_midi_lost(std::uint32_t* overflow, std::uint32_t* sysex) {
  if (g_host != nullptr) {
    g_host->host_midi_lost(overflow, sysex);
  }
}

int tramp_tap_select(std::uint32_t source) { return g_host != nullptr ? g_host->host_tap_select(source) : 1; }
std::uint32_t tramp_tap_latest(std::int16_t* out, std::uint32_t frames) { return g_host != nullptr ? g_host->host_tap_latest(out, frames) : 0u; }
std::uint32_t tramp_tap_stream(std::int16_t* out, std::uint32_t max_frames, std::uint32_t* lost) { return g_host != nullptr ? g_host->host_tap_stream(out, max_frames, lost) : 0u; }
void tramp_tap_levels(o3m_tap_levels* out) {
  if (g_host != nullptr) {
    g_host->host_tap_levels(out);
  }
}

// OS-H3A (ABI 1.5): the project's steps and the edit groups. Without a running app (a stale pointer kept past exit) every answer is "no project".
std::int32_t tramp_pattern_info(std::uint16_t pattern, o3m_pattern_info* out) { return g_host != nullptr ? g_host->host_pattern_info(pattern, out) : O3M_EDIT_NO_PROJECT; }
std::int32_t tramp_get_steps(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, o3m_step* out, std::uint8_t count) {
  return g_host != nullptr ? g_host->host_get_steps(pattern, track, first, out, count) : O3M_EDIT_NO_PROJECT;
}
std::int32_t tramp_edit_begin() { return g_host != nullptr ? g_host->host_edit_begin() : O3M_EDIT_NO_PROJECT; }
std::int32_t tramp_edit_end() { return g_host != nullptr ? g_host->host_edit_end() : O3M_EDIT_NO_PROJECT; }
std::int32_t tramp_set_step(std::uint16_t pattern, std::uint8_t track, std::uint8_t row, const o3m_step* s, std::uint8_t fields) {
  return g_host != nullptr ? g_host->host_set_step(pattern, track, row, s, fields) : O3M_EDIT_NO_PROJECT;
}
std::int32_t tramp_set_steps(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, const o3m_step* s, std::uint8_t count, std::uint8_t fields) {
  return g_host != nullptr ? g_host->host_set_steps(pattern, track, first, s, count, fields) : O3M_EDIT_NO_PROJECT;
}

// OS-H3B (ABI 1.6): the instruments' parameters.
std::int32_t tramp_instrument_info(std::uint8_t instrument, o3m_instrument_info* out) { return g_host != nullptr ? g_host->host_instrument_info(instrument, out) : O3M_EDIT_NO_PROJECT; }
std::int32_t tramp_get_param_desc(std::uint8_t instrument, std::uint16_t index, o3m_param_desc* out) {
  return g_host != nullptr ? g_host->host_get_param_desc(instrument, index, out) : O3M_EDIT_NO_PROJECT;
}
std::int32_t tramp_get_instrument_param(std::uint8_t instrument, std::uint16_t id, float* out) {
  return g_host != nullptr ? g_host->host_get_instrument_param(instrument, id, out) : O3M_EDIT_NO_PROJECT;
}
std::int32_t tramp_set_instrument_param(std::uint8_t instrument, std::uint16_t id, float value) {
  return g_host != nullptr ? g_host->host_set_instrument_param(instrument, id, value) : O3M_EDIT_NO_PROJECT;
}

// OS-H4 (ABI 1.7): the export calls. Without a running app (a stale pointer kept past exit) they answer as an app with no file open would be told.
std::int32_t tramp_export_open(const char* name, const char* ext) { return g_host != nullptr ? g_host->host_export_open(name, ext) : O3M_EXPORT_STATE; }
std::int32_t tramp_export_write(const void* buf, std::uint32_t n) { return g_host != nullptr ? g_host->host_export_write(buf, n) : O3M_EXPORT_STATE; }
std::int32_t tramp_export_close() { return g_host != nullptr ? g_host->host_export_close() : O3M_EXPORT_STATE; }
std::uint32_t tramp_export_status(char* final_name, std::uint32_t cap) { return g_host != nullptr ? g_host->host_export_status(final_name, cap) : O3M_EXPORT_IDLE; }

// OS-APPUX S3 (ABI 1.10): the transport, the app's own stored rows and its notes. Without a running app (a stale pointer kept past exit) they answer as an OS with nothing behind them would.
std::int32_t tramp_get_transport(o3m_transport* out) { return g_host != nullptr ? g_host->host_get_transport(out) : 1; }
std::int32_t tramp_set_own_param(std::uint32_t index, float value) { return g_host != nullptr ? g_host->host_set_own_param(index, value) : O3M_OWN_RANGE; }
std::int32_t tramp_play_note(std::uint32_t instrument, std::uint32_t note, std::uint32_t velocity) { return g_host != nullptr ? g_host->host_play_note(instrument, note, velocity) : O3M_PLAY_NO_AUDIO; }
std::int32_t tramp_stop_note(std::uint32_t instrument) { return g_host != nullptr ? g_host->host_stop_note(instrument) : O3M_PLAY_NO_AUDIO; }

const o3m_read_api kReadApi = {O3M_READ_API_VERSION, sizeof(o3m_read_api), tramp_get_tempo, tramp_launch_ctx, tramp_pattern_info, tramp_get_steps, tramp_instrument_info, tramp_get_param_desc,
                               tramp_get_instrument_param};
const o3m_edit_api kEditApi = {O3M_EDIT_API_VERSION, sizeof(o3m_edit_api), tramp_set_tempo, tramp_edit_begin, tramp_edit_end, tramp_set_step, tramp_set_steps, tramp_set_instrument_param};

ui::Color to_color(std::uint8_t v) noexcept { return static_cast<ui::Color>(v < O3M_COLOR_COUNT ? v : static_cast<std::uint8_t>(ui::Color::Text)); }
ui::Face to_face(std::uint8_t v) noexcept { return static_cast<ui::Face>(v < ui::kFaceCount ? v : static_cast<std::uint8_t>(ui::Face::Data)); }

// The renderer behind a record's replay. Everything is in canvas pixels; `area` is where the canvas sits in the frame.
class RenderSink final : public module::ViewSink {
 public:
  RenderSink(ui::Renderer& r, const ui::Rect& area) noexcept : r_(r), area_(area) {}

  void fill(int x, int y, int w, int h, std::uint8_t color) noexcept override { r_.panel({area_.x + x, area_.y + y, w, h}, to_color(color)); }

  void text(int x, int y, const char* s, std::uint8_t face, std::uint8_t color) noexcept override {
    r_.text(area_.x + x, area_.y + y, s, to_color(color), to_face(face));
  }

  void polyline(const std::int16_t* xy, std::uint32_t points, std::uint8_t color) noexcept override {
    r_.begin_raw();
    r_.raw(dl::scissor_xy(static_cast<std::uint32_t>(area_.x), static_cast<std::uint32_t>(area_.y)));
    r_.raw(dl::scissor_size(static_cast<std::uint32_t>(area_.w), static_cast<std::uint32_t>(area_.h)));
    r_.raw(r_.color_word(to_color(color)));
    r_.raw(dl::line_width(12));
    r_.raw(dl::begin(eve::Primitive::LineStrip));
    for (std::uint32_t i = 0; i < points; ++i) {
      r_.raw(dl::vertex2f(q4(area_.x, xy[2 * i]), q4(area_.y, xy[2 * i + 1])));  // the pixel's centre, in 1/16 px; the scissor cuts what lies outside the canvas
    }
    r_.raw(dl::end());
    r_.end_raw();
  }

  void curve(const module::CurveCall& c) noexcept override {
    const ui::Rect data = (c.flags & O3M_CURVE_AXES) != 0u
                              ? ui::Rect{area_.x + O3M_CURVE_MARGIN_LEFT, area_.y + O3M_CURVE_MARGIN_TOP,
                                         area_.w - O3M_CURVE_MARGIN_LEFT - O3M_CURVE_MARGIN_RIGHT, area_.h - O3M_CURVE_MARGIN_TOP - O3M_CURVE_MARGIN_BOTTOM}
                              : area_;
    if (data.w < 8 || data.h < 8) {
      return;
    }
    const plot::AxisSpec xa{static_cast<plot::AxisKind>(c.x_axis), c.x_lo, c.x_hi};
    const plot::AxisSpec ya{static_cast<plot::AxisKind>(c.y_axis), c.y_lo, c.y_hi};
    plot::CurveStyle style;
    style.line = to_color(c.color);
    style.fill = to_color(c.color);
    style.max_vertices = 128;
    if ((c.flags & O3M_CURVE_AXES) != 0u) {
      plot::draw_axes(r_, data, xa, ya, style);
    }
    plot::CurveSeries series;
    series.x = c.x;
    series.y = c.y;
    series.n = c.n;
    series.x_lo = c.x_lo;
    series.x_hi = c.x_hi;
    (void)plot::draw_series(r_, data, xa, ya, series, (c.flags & O3M_CURVE_FILL) != 0u, style, params_curve_scratch());
  }

 private:
  static std::int32_t q4(int origin, std::int16_t v) noexcept {
    const int q = (origin + v) * 16 + 8;
    return q < -16000 ? -16000 : (q > 16000 ? 16000 : q);
  }
  ui::Renderer& r_;
  ui::Rect area_;
};

}  // namespace

// ---- descriptors ------------------------------------------------------------------------------------------------------------------------

bool describe_module_param(const o3m_param& p, ParamScratch& out, const o3m_labels* labels) noexcept {
  std::memset(&out, 0, sizeof out);
  unsigned n = 0;
  for (; p.name != nullptr && p.name[n] != '\0' && n < core::kParamNameChars; ++n) {
    const char c = p.name[n];
    out.name[n] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
  }
  const bool boolean = (p.flags & O3M_PARAM_BOOL) != 0u && p.min == 0.0f && p.max == 1.0f;
  const bool whole = (p.flags & O3M_PARAM_INT) != 0u || boolean;
  const unsigned unit = O3M_PARAM_UNIT(p.flags);
  const float span = p.max - p.min;
  core::ParamDesc& d = out.desc;
  d.name = out.name;
  d.unit_text = kUnitText[unit < O3M_UNIT_COUNT ? unit : 0u];
  d.names = boolean ? kBoolNames : nullptr;
  d.knots = nullptr;
  d.min = p.min;
  d.max = p.max;
  d.def = p.def;
  if (whole) {
    d.detents = static_cast<std::uint16_t>(span >= 1.0f ? (span < 65535.0f ? span : 65535.0f) : 1.0f);
    d.coarse = static_cast<std::uint8_t>(span >= 40.0f ? 10 : (span >= 10.0f ? 5 : 1));
  } else {
    d.detents = 100;
    d.coarse = 10;
  }
  d.list_count = boolean ? 2 : 0;
  d.unit = boolean ? core::ParamUnit::Enum : (whole || unit != O3M_UNIT_NONE ? core::ParamUnit::Int : core::ParamUnit::NormPct);
  if (labels != nullptr && labels->names != nullptr && (p.flags & O3M_PARAM_INT) != 0u && !boolean && span >= 1.0f && span + 1.0f == static_cast<float>(labels->count) && labels->count <= O3M_LABELS_MAX) {
    d.names = labels->names;  // OS-INSTUI S10: the value's name, names[round(value - min)] (the module's strings: they live as long as the module)
    d.list_count = static_cast<std::uint8_t>(labels->count);
    d.unit = core::ParamUnit::Enum;
  }
  if (d.unit == core::ParamUnit::NormPct) {
    d.unit_text = kUnitText[O3M_UNIT_PERCENT];  // shown as where it is in its range, 0..100, as every native NormPct row: without the "%" a 0..1 float read as a range of 0..100 (OS-SMALL2)
  }
  d.curve = (p.flags & O3M_PARAM_LOG) != 0u && p.min > 0.0f ? core::ParamCurve::Log : core::ParamCurve::Linear;
  d.flags = static_cast<std::uint8_t>(((p.flags & O3M_PARAM_NO_LEARN) != 0u ? 0u : core::kParamLearnable) | ((p.flags & O3M_PARAM_MOD) != 0u ? core::kParamModulatable : 0u));
  d.group = 0;
  out.whole = whole;
  return n != 0u && core::param_desc_valid(d);
}

bool is_module_param(core::ParamId id) noexcept {
  return core::param_kind(id) == core::ParamKind::Instrument && model::instrument_param::is_module_field(core::param_field(id)) &&
         core::param_index(id) < model::kMaxInstruments;
}

bool is_midi_channel_param(core::ParamId id) noexcept {
  return core::param_kind(id) == core::ParamKind::Instrument && core::param_field(id) == model::instrument_param::MidiChannel && core::param_index(id) < model::kMaxInstruments;
}

bool is_play_stereo_param(core::ParamId id) noexcept {
  return core::param_kind(id) == core::ParamKind::Instrument && core::param_field(id) == model::instrument_param::PlayStereo && core::param_index(id) < model::kMaxInstruments;
}

const core::ParamDesc* describe_param(const Model& model, core::ParamId id, ParamScratch& scratch) noexcept {
  if (is_midi_channel_param(id)) {
    return &model::midi_channel_desc();
  }
  if (is_play_stereo_param(id)) {
    return &model::play_stereo_desc();
  }
  if (!is_module_param(id)) {
    return model::param_desc(id);
  }
  if (model.modules == nullptr || model.project == nullptr) {
    return nullptr;
  }
  return model.modules->describe(*model.project, core::param_index(id), core::param_field(id) - model::instrument_param::kModuleFirst, scratch) ? &scratch.desc : nullptr;
}

// ---- composition --------------------------------------------------------------------------------------------------------------------------

void ModuleUi::set_clock(const hal::Clock* clock, std::uint16_t (*stamp)(std::uint16_t)) noexcept {
  clock_ = clock;
  module::ViewHost::Hooks h;
  h.now_us = clock != nullptr ? clock_us : nullptr;
  h.ctx = const_cast<hal::Clock*>(clock);
  h.enter = stamp;
  views_.set_hooks(h);
}

void ModuleUi::release_host() noexcept {
  stop_app_notes();  // OS-APPUX S3: every exit, a stop for the time budget and a project change end the app's notes
  if (midi_on_ && midi_ != nullptr) {
    midi_->set_enabled(false);
  }
  midi_on_ = false;
  if (tap_on_ && tap_ != nullptr) {
    tap_->set_source(audio::AppTap::Source::Off);  // the render stops writing at its next block
    tap_->set_samples(true);                       // and the tap is back at its default (samples stored) for whoever uses it next
  }
  tap_on_ = false;
  if (export_on_ && export_ != nullptr) {
    export_->end_app();  // OS-H4: a file the app left open or committing is aborted (its Temp removed); the files already DONE stay
  }
  export_on_ = false;
  g_open_ = closed_ = g_over_ = g_tempo_set_ = false;  // an open group of an app that is gone is forgotten, not applied: nothing is left to ask the project (end_call applies what the app left open)
  g_ops_ = g_steps_ = g_calls_ = g_params_ = 0;
  call_ = Call::None;
  if (g_host == this) {
    g_host = nullptr;
  }
}

void ModuleUi::reset() noexcept {
  release_host();
  tempo_req_ = 0;
  project_ = nullptr;
  app_running_ = false;
  app_stopped_ = false;
  app_verdict_ = module::ViewStatus::Ok;
  app_index_ = -1;
  app_state_ = nullptr;
  app_state_bytes_ = 0;
  inst_count_ = 0;
  app_count_ = 0;
}

// ---- the slot's module -------------------------------------------------------------------------------------------------------------------

const o3m_module* ModuleUi::bound_module(const model::Project& project, unsigned slot, int* set_index) const noexcept {
  if (set_ == nullptr || voices_ == nullptr || slot >= model::kMaxInstruments) {
    return nullptr;
  }
  const model::Instrument* in = project.instrument(slot);
  if (in == nullptr || in->kind != model::InstrumentKind::Module) {
    return nullptr;
  }
  const model::ModuleRef* ref = project.module(in->module_index);
  const int idx = ref != nullptr ? set_->find(ref->name) : -1;
  if (idx < 0 || !voices_->bound(slot)) {
    return nullptr;
  }
  const module::LoadedModule* lm = set_->at(static_cast<unsigned>(idx));
  if (lm == nullptr || lm->entry == nullptr) {
    return nullptr;
  }
  if (set_index != nullptr) {
    *set_index = idx;
  }
  return lm->entry;
}

unsigned ModuleUi::param_count(const model::Project& project, unsigned slot) const noexcept {
  const o3m_module* m = bound_module(project, slot);
  return m != nullptr ? m->param_count : 0u;
}

bool ModuleUi::describe(const model::Project& project, unsigned slot, unsigned index, ParamScratch& out) const noexcept {
  const o3m_module* m = bound_module(project, slot);
  // (the members past byte 64 of the descriptor exist only in a module of ABI 1.8)
  return m != nullptr && index < m->param_count && describe_module_param(m->params[index], out, m->abi_minor >= module::kPitchAbiMinor && m->labels != nullptr ? &m->labels[index] : nullptr);
}

unsigned ModuleUi::page_table_count(const model::Project& project, unsigned slot) const noexcept {
  const o3m_module* m = bound_module(project, slot);
  return m != nullptr && m->abi_minor >= module::kPitchAbiMinor && m->pages != nullptr && m->param_count != 0u ? (m->page_count < O3M_PAGES_MAX ? m->page_count : O3M_PAGES_MAX) : 0u;
}

bool ModuleUi::page_at(const model::Project& project, unsigned slot, unsigned k, const char*& name, unsigned& first, unsigned& count) const noexcept {
  const o3m_module* m = bound_module(project, slot);
  if (k >= page_table_count(project, slot)) {
    return false;
  }
  const unsigned total = m->param_count;
  first = m->pages[k].first_param < total ? m->pages[k].first_param : total - 1u;
  const unsigned end = k + 1u < m->page_count && m->pages[k + 1u].first_param <= total ? m->pages[k + 1u].first_param : total;
  count = end > first ? end - first : 1u;
  count = count > O3M_PAGE_PARAMS_MAX ? O3M_PAGE_PARAMS_MAX : count;
  name = m->pages[k].name != nullptr ? m->pages[k].name : "";
  return true;
}

unsigned ModuleUi::view_count(const model::Project& project, unsigned slot) const noexcept {
  int idx = -1;
  const o3m_module* m = bound_module(project, slot, &idx);
  if (m == nullptr || !set_->at(static_cast<unsigned>(idx))->has_views() || set_->views_blocked(static_cast<unsigned>(idx))) {
    return 0;
  }
  return m->view_count;
}

static_assert(model::kMaxInstruments <= 64, "ModuleUi::diag_slots_ is one bit a slot");

void ModuleUi::sync_slots(const model::Project& project) noexcept {
  if (voices_ == nullptr) {
    return;
  }
  for (unsigned slot = 0; slot < model::kMaxInstruments; ++slot) {
    if (voices_->bound(slot)) {
      const model::Instrument* in = project.instrument(slot);
      // A console bind on a FREE slot stays open (OS-BUGS4). Once the slot holds any instrument the project decides again, for good (the mark is dropped): a sampler or MIDI instrument
      // there is never routed to the bench instance, and a Module put there and deleted closes the gate like any other.
      const bool free_slot = in == nullptr || in->kind == model::InstrumentKind::None;
      if (!free_slot) {
        diag_slots_ &= ~(std::uint64_t{1} << slot);
      }
      voices_->set_gate(slot, free_slot ? (diag_slots_ >> slot & 1u) != 0u : in->kind == model::InstrumentKind::Module);
    }
  }
}

std::uint32_t ModuleUi::voice_cost_of(const char* stem) const noexcept {
  const int idx = set_ != nullptr && voices_ != nullptr ? set_->find(stem) : -1;
  const module::LoadedModule* lm = idx >= 0 ? set_->at(static_cast<unsigned>(idx)) : nullptr;
  std::uint32_t cost = 0;
  for (unsigned i = 0; lm != nullptr && lm->entry != nullptr && i < audio::kMaxModuleInstances; ++i) {
    audio::ModuleVoices::Info in{};
    if (voices_->info(i, in) && in.mod == lm->entry && in.cost > cost) {
      cost = in.cost;
    }
  }
  return cost;
}

const char* ModuleUi::module_name_of(const model::Project& project, unsigned slot) noexcept {
  const model::Instrument* in = slot < model::kMaxInstruments ? project.instrument(slot) : nullptr;
  if (in == nullptr || in->kind != model::InstrumentKind::Module) {
    return "";
  }
  const model::ModuleRef* ref = project.module(in->module_index);
  return ref != nullptr ? ref->name : "";
}

void ModuleUi::fault(int set_index, module::ViewStatus verdict) noexcept {
  (void)verdict;
  if (set_index == app_index_) {
    release_host();  // a stopped app is not called again: nobody would drain its MIDI ring or read its clock
  }
  if (set_ != nullptr && set_index >= 0 && static_cast<unsigned>(set_index) < set_->count()) {
    set_->block_views(static_cast<unsigned>(set_index));
    (void)set_->quarantine(set_->at(static_cast<unsigned>(set_index))->name, module::Status::ViewOverrun);
  }
}

void ModuleUi::replay(ui::Renderer& r, const ui::Rect& area) noexcept {
  RenderSink sink(r, area);
  views_.replay(sink);
  ++drawn_;
}

bool ModuleUi::draw_slot_view(ui::Renderer& r, const model::Project& project, unsigned slot, unsigned view, const ui::Rect& area) noexcept {
  int idx = -1;
  const o3m_module* m = bound_module(project, slot, &idx);
  if (m == nullptr || area.w < module::kViewMinSize || area.h < module::kViewMinSize || view >= m->view_count) {
    return false;
  }
  const module::LoadedModule* lm = set_->at(static_cast<unsigned>(idx));
  if (!lm->has_views() || set_->views_blocked(static_cast<unsigned>(idx))) {
    return false;
  }
  const ui::Theme& t = r.theme();
  module::ViewHost::Metrics metrics;
  for (unsigned f = 0; f < ui::kFaceCount; ++f) {
    metrics.cell_w[f] = static_cast<std::int16_t>(t.geometry(static_cast<ui::Face>(f)).cell_w);
    metrics.cell_h[f] = static_cast<std::int16_t>(t.geometry(static_cast<ui::Face>(f)).cell_h);
  }
  views_.set_metrics(metrics);
  float values[O3M_PARAMS_MAX];
  const unsigned count = m->param_count < O3M_PARAMS_MAX ? m->param_count : O3M_PARAMS_MAX;
  for (unsigned i = 0; i < count; ++i) {
    const core::ParamId id = core::param_id(core::ParamKind::Instrument, slot, model::instrument_param::kModuleFirst + i);
    values[i] = project.has_param(id) ? project.get_param(id) : m->params[i].def;
  }
  const int w = area.w < module::kViewMaxW ? area.w : module::kViewMaxW;
  const int h = area.h < module::kViewMaxH ? area.h : module::kViewMaxH;
  const std::uint32_t room = module::kViewMaxWords > view_words_ ? module::kViewMaxWords - view_words_ : 0u;
  if (room < kMinViewWords) {
    return false;  // the page's share is spent by the views before this one: nothing is drawn, nothing is blamed
  }
  const module::ViewStatus verdict = views_.draw(*m, voices_->state_of(slot), module::fault_id(lm->name), view, w, h, values, count, room);
  if (verdict != module::ViewStatus::Ok) {
    fault(idx, verdict);
    return false;
  }
  view_words_ += views_.words();
  replay(r, ui::Rect{area.x, area.y, w, h});
  return true;
}

bool ModuleUi::send_param(unsigned slot, unsigned index, float value) noexcept {
  return voices_ != nullptr && voices_->set_param(slot, index, value);
}

// ---- loading ---------------------------------------------------------------------------------------------------------------------------

bool ModuleUi::load(const char* stem) noexcept {
  last_status_ = module::Status::Ok;
  if (set_ == nullptr || loader_ == nullptr || card_ == nullptr) {
    last_status_ = module::Status::NotFound;
    return false;
  }
  if (set_->find(stem) >= 0) {
    return true;
  }
  refused_caps_ = 0;
  if (!loader_->begin(*card_, *set_, stem, sync_)) {
    last_status_ = loader_->status();
    return false;
  }
  for (unsigned n = 0; n < 4096 && loader_->state() == module::ModuleLoader::State::Running; ++n) {
    (void)loader_->step(2048);  // a module is a few KB: a handful of reads; bounded by the largest image over this budget
  }
  if (loader_->state() != module::ModuleLoader::State::Done) {
    last_status_ = loader_->status() == module::Status::Ok ? module::Status::Io : loader_->status();
    if (last_status_ == module::Status::CapUnsupported) {
      refused_caps_ = loader_->module().header.reserved2[0];  // (the header stays in the loader after a refusal)
    }
    loader_->cancel();
    return false;
  }
  return true;
}

// ---- the catalogs --------------------------------------------------------------------------------------------------------------------------

void ModuleUi::classify(CatalogEntry& e) const noexcept {
  if (e.state == CatalogEntry::State::Missing) {
    return;
  }
  e.state = set_ != nullptr && set_->quarantined(e.stem) ? CatalogEntry::State::Quarantined
                                                          : (set_ != nullptr && set_->find(e.stem) >= 0 ? CatalogEntry::State::Loaded : CatalogEntry::State::Ready);
}

namespace {

// Adds `stem` to the sorted catalog unless it is there or the catalog is full (the card's files and the built-in modules meet here: a stem is one entry).
void add_sorted(CatalogEntry* out, unsigned& count, const char* stem) noexcept {
  unsigned at = 0;
  while (at < count) {
    const int c = compare_ci(out[at].stem, stem);
    if (c == 0) {
      return;
    }
    if (c > 0) {
      break;
    }
    ++at;
  }
  if (count >= kModuleCatalogMax) {
    return;
  }
  for (unsigned i = count; i > at; --i) {
    out[i] = out[i - 1];
  }
  copy_text(out[at].stem, sizeof out[at].stem, stem);
  out[at].state = CatalogEntry::State::Ready;
  ++count;
}

}  // namespace

void ModuleUi::scan(const char* dir, unsigned kind, CatalogEntry* out, unsigned& count, const model::Project* project) noexcept {
  count = 0;
  if (card_ != nullptr && card_->mounted()) {
    hal::storage::DirHandle d;
    if (card_->opendir(dir, d) == hal::storage::Status::Ok) {
      hal::storage::DirEntry e;  // 264 B on the main loop's stack
      for (unsigned n = 0; n < 256 && card_->readdir(d, e) == hal::storage::Status::Ok; ++n) {
        const std::size_t len = std::strlen(e.name);
        if (e.is_dir || len < 6u || !same_ci(e.name + len - 5, ".tmod")) {
          continue;
        }
        char stem[module::kStemMax + 1];
        if (len - 5 > module::kStemMax) {
          continue;
        }
        std::memcpy(stem, e.name, len - 5);
        stem[len - 5] = '\0';
        if (!module::is_stem(stem)) {
          continue;
        }
        add_sorted(out, count, stem);
      }
      card_->closedir(d);
    }
  }
  // OS-012d: the built-in modules of this kind are in the picker with or without a card (a card copy of the same stem is the same entry).
  for (unsigned i = 0; set_ != nullptr && i < set_->builtin_count(); ++i) {
    const module::BuiltinModule& b = *set_->builtin_at(i);
    if (module::builtin_header(b).kind == kind) {
      add_sorted(out, count, b.stem);
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    classify(out[i]);
  }
  for (unsigned m = 0; project != nullptr && m < project->module_count() && count < kModuleCatalogMax; ++m) {  // what the project names and the card lacks: greyed
    const char* name = project->module(m)->name;
    bool have = false;
    for (unsigned i = 0; i < count; ++i) {
      have = have || same_ci(out[i].stem, name);
    }
    if (!have) {
      copy_text(out[count].stem, sizeof out[count].stem, name);
      out[count].state = CatalogEntry::State::Missing;
      ++count;
    }
  }
}

void ModuleUi::scan_instruments(const model::Project* project) noexcept { scan("modules/instruments", O3M_KIND_INSTRUMENT, inst_, inst_count_, project); }

void ModuleUi::scan_apps() noexcept { scan("modules/apps", O3M_KIND_APP, apps_, app_count_, nullptr); }

// ---- the engine picker ---------------------------------------------------------------------------------------------------------------

const char* ModuleUi::pick_text(PickResult r) noexcept {
  switch (r) {
    case PickResult::Ok: return "ENGINE SET";
    case PickResult::OnComputer: return "CARD ON COMPUTER";
    case PickResult::NoEntry: return "NO SUCH ENGINE";
    case PickResult::Missing: return "MODULE NOT ON THE CARD";
    case PickResult::Quarantined: return "MODULE QUARANTINED UNTIL REBOOT";
    case PickResult::Bound: return "RELOAD TO PUT A MODULE HERE";
    case PickResult::NoProject: return "NO PROJECT OPEN";
    case PickResult::NoCard: return "NO SD CARD";
    case PickResult::LoadFailed: return "MODULE REFUSED";
    case PickResult::NotInstrument: return "NOT AN INSTRUMENT MODULE";
    case PickResult::NotRunnable: return "MODULE CANNOT RUN IN THIS BUILD";
    case PickResult::NoRoom: return "NO ROOM FOR THE MODULE";
    case PickResult::TableFull: return "PROJECT MODULE TABLE FULL";
    case PickResult::BindFailed: return "ENGINE DID NOT START";
    case PickResult::BadInstrument: return "INSTRUMENT REFUSED";
  }
  return "";
}

ModuleUi::PickResult ModuleUi::pick(model::Editor& editor, unsigned slot, unsigned entry) noexcept {
  if (card_on_computer_) {
    return PickResult::OnComputer;
  }
  if (set_ == nullptr || loader_ == nullptr || voices_ == nullptr) {
    return PickResult::NoCard;
  }
  if (entry >= inst_count_ || slot >= model::kMaxInstruments) {
    return PickResult::NoEntry;
  }
  const CatalogEntry e = inst_[entry];
  if (e.state == CatalogEntry::State::Missing) {
    return PickResult::Missing;
  }
  if (e.state == CatalogEntry::State::Quarantined) {
    return PickResult::Quarantined;
  }
  if (voices_->bound(slot)) {
    return PickResult::Bound;
  }
  if (!load(e.stem)) {
    switch (last_status_) {
      case module::Status::NotFound: return card_ == nullptr ? PickResult::NoCard : PickResult::Missing;
      case module::Status::NoMemory: return PickResult::NoRoom;
      case module::Status::Quarantined: return PickResult::Quarantined;
      default: return PickResult::LoadFailed;
    }
  }
  const int idx = set_->find(e.stem);
  const module::LoadedModule* lm = idx >= 0 ? set_->at(static_cast<unsigned>(idx)) : nullptr;
  if (lm == nullptr || lm->header.kind != O3M_KIND_INSTRUMENT) {
    return PickResult::NotInstrument;
  }
  if (lm->entry == nullptr) {
    return PickResult::NotRunnable;
  }
  const o3m_module& mod = *lm->entry;
  const model::Project& project = editor.project();
  int ref = -1;
  for (unsigned i = 0; i < project.module_count(); ++i) {
    if (std::strcmp(project.module(i)->name, e.stem) == 0) {
      ref = static_cast<int>(i);
    }
  }
  if (ref < 0) {
    if (editor.add_module(e.stem, std::strlen(e.stem), lm->header.abi_major, lm->header.abi_minor) != model::Status::Ok) {
      return PickResult::TableFull;
    }
    ref = static_cast<int>(project.module_count()) - 1;
  }
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Module);
  in.module_index = static_cast<std::uint8_t>(ref);
  copy_text(in.name, sizeof in.name, mod.name);
  in.param_len = static_cast<std::uint16_t>(4u * mod.param_count);  // the whole blob, so that every parameter has a value to edit
  for (unsigned i = 0; i < mod.param_count; ++i) {
    std::memcpy(in.params + 4u * i, &mod.params[i].def, 4);
  }
  if (editor.set_instrument(slot, in) != model::Status::Ok) {
    return PickResult::BadInstrument;
  }
  const std::uint32_t mark = set_->cold().mark();
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  if (!set_->cold().take(mod.state_bytes, 16, mem, addr)) {
    return PickResult::NoRoom;
  }
  std::memset(mem, 0, mod.state_bytes);
  const model::Instrument* stored = project.instrument(slot);
  if (stored == nullptr || voices_->bind(slot, mod, module::fault_id(e.stem), mem, stored->params, stored->param_len) != audio::ModuleVoices::Bind::Ok) {
    set_->cold().rollback(mark);
    return PickResult::BindFailed;  // the instrument stays (muted), as a project with a module that cannot start
  }
  return PickResult::Ok;
}

// ---- apps ------------------------------------------------------------------------------------------------------------------------------------

const char* ModuleUi::launch_text(LaunchResult r) noexcept {
  switch (r) {
    case LaunchResult::Ok: return "APP STARTED";
    case LaunchResult::OnComputer: return "CARD ON COMPUTER";
    case LaunchResult::NoEntry: return "NO SUCH APP";
    case LaunchResult::NoCard: return "NO SD CARD";
    case LaunchResult::Quarantined: return "APP QUARANTINED UNTIL REBOOT";
    case LaunchResult::LoadFailed: return "APP REFUSED";
    case LaunchResult::NotApp: return "NOT AN APP MODULE";
    case LaunchResult::NoRoom: return "NO ROOM FOR THE APP";
    case LaunchResult::InitFailed: return "APP DID NOT START";
    case LaunchResult::Busy: return "AN APP IS RUNNING";
    case LaunchResult::NeedsConfirm: return "EDITS THE PROJECT (UNDOABLE): F1 AGAIN";  // (the text of an edit-only app; launch_message() says what the last app did)
    case LaunchResult::NeedsAudioTap: return "NEEDS AUDIO TAP (NEWER OS)";
    case LaunchResult::NeedsExportFiles: return "NEEDS EXPORT FILES (NEWER OS)";
    case LaunchResult::NeedsMidiOut: return "NEEDS MIDI OUT (NEWER OS)";
    case LaunchResult::NeedsNewerOs: return "NEEDS NEWER OS";
  }
  return "";
}

const o3m_module* ModuleUi::app_module() const noexcept {
  if (!app_running_ || set_ == nullptr || app_index_ < 0) {
    return nullptr;
  }
  const module::LoadedModule* lm = set_->at(static_cast<unsigned>(app_index_));
  return lm != nullptr ? lm->entry : nullptr;
}

unsigned ModuleUi::app_param_count() const noexcept {
  const o3m_module* m = app_module();
  return m != nullptr ? m->param_count : 0u;
}

bool ModuleUi::app_describe(unsigned index, ParamScratch& out) const noexcept {
  const o3m_module* m = app_module();
  return m != nullptr && index < m->param_count && describe_module_param(m->params[index], out);
}

bool ModuleUi::edit_acked(const char* stem, std::uint32_t need) const noexcept {
  for (unsigned i = 0; i < kEditAckMax; ++i) {
    if (acked_[i][0] != '\0' && std::strcmp(acked_[i], stem) == 0) {
      return (acked_caps_[i] & need) == need;
    }
  }
  return false;
}

// The stem has been asked about `need`: its slot gains the bits (a file replaced by a version that asks for more is asked again, and then remembered with both), or it takes the next slot of the ring.
void ModuleUi::ack_caps(const char* stem, std::uint32_t need) noexcept {
  for (unsigned i = 0; i < kEditAckMax; ++i) {
    if (acked_[i][0] != '\0' && std::strcmp(acked_[i], stem) == 0) {
      acked_caps_[i] |= need;
      return;
    }
  }
  copy_text(acked_[acked_next_], sizeof acked_[acked_next_], stem);
  acked_caps_[acked_next_] = need;
  acked_next_ = (acked_next_ + 1u) % kEditAckMax;
}

ModuleUi::LaunchResult ModuleUi::launch_app(unsigned entry, const model::Project* project, const o3m_launch_ctx* ctx) noexcept {
  if (card_on_computer_) {
    return LaunchResult::OnComputer;
  }
  if (set_ == nullptr || loader_ == nullptr || voices_ == nullptr || card_ == nullptr) {
    return LaunchResult::NoCard;
  }
  if (app_running_) {
    return LaunchResult::Busy;
  }
  if (entry >= app_count_) {
    return LaunchResult::NoEntry;
  }
  const CatalogEntry e = apps_[entry];
  if (e.state == CatalogEntry::State::Quarantined) {
    return quarantined_for_newer_abi(e.stem) ? LaunchResult::NeedsNewerOs : LaunchResult::Quarantined;
  }
  if (!load(e.stem)) {
    switch (last_status_) {
      case module::Status::NoMemory: return LaunchResult::NoRoom;
      case module::Status::AbiMinor: return LaunchResult::NeedsNewerOs;  // OS-H3A: built for a newer ABI than this OS (a 1.5 app on an OS of 1.4): the file is fine, the OS is old
      case module::Status::Quarantined: return quarantined_for_newer_abi(e.stem) ? LaunchResult::NeedsNewerOs : LaunchResult::Quarantined;
      case module::Status::CapUnsupported:  // names the first capability this OS lacks; the app never runs with half its tables
        return (refused_caps_ & O3M_CAP_AUDIO_TAP) != 0u ? LaunchResult::NeedsAudioTap : ((refused_caps_ & O3M_CAP_EXPORT_FILES) != 0u ? LaunchResult::NeedsExportFiles : LaunchResult::NeedsMidiOut);
      default: return LaunchResult::LoadFailed;
    }
  }
  const int idx = set_->find(e.stem);
  const module::LoadedModule* lm = idx >= 0 ? set_->at(static_cast<unsigned>(idx)) : nullptr;
  if (lm == nullptr || lm->header.kind != O3M_KIND_APP || lm->entry == nullptr || !lm->has_views() || set_->views_blocked(static_cast<unsigned>(idx))) {
    return lm != nullptr && lm->header.kind == O3M_KIND_APP && lm->entry == nullptr ? LaunchResult::LoadFailed : LaunchResult::NotApp;
  }
  const o3m_module& mod = *lm->entry;
  if ((lm->header.reserved2[0] & O3M_CAP_AUDIO_TAP) != 0u && tap_ == nullptr) {
    return LaunchResult::NeedsAudioTap;  // a build with no tap: the app never runs with half its tables (C4)
  }
  // OS-H1: an app that edits the project is confirmed once per boot (ADR 0014 decision 6; the answer is not stored: a persistent record is a format decision). The first launch says so and
  // remembers the stem; the second launch of the same app, straight after, goes through and the stem is kept for the boot. OS-H4: an app that writes files (export_files) is confirmed the same way, and an
  // app with both bits gets ONE message that says both.
  const std::uint32_t caps = lm->header.reserved2[0];
  const std::uint32_t need = caps & (O3M_CAP_EDIT_PROJECT | O3M_CAP_EXPORT_FILES);
  if (need != 0u && !edit_acked(e.stem, need)) {
    if (std::strcmp(confirm_stem_, e.stem) != 0 || confirm_caps_ != need) {
      copy_text(confirm_stem_, sizeof confirm_stem_, e.stem);
      confirm_caps_ = need;
      compose_confirm(caps, e.stem, confirm_text_, sizeof confirm_text_);
      return LaunchResult::NeedsConfirm;
    }
    ack_caps(e.stem, need);
  }
  confirm_stem_[0] = '\0';
  // The state block: one block serves every app launched, whichever it is (the arena has no free: a project change gives it back). Only an app that needs more than the
  // block holds takes a new, larger one (OS-FU1: it used to take a block at every switch from one app to another). The previous app has exited (Busy otherwise) and
  // nothing refers to its state any more; the memset below clears it for the next.
  if (app_state_ == nullptr || app_state_bytes_ < mod.state_bytes) {
    std::uint8_t* mem = nullptr;
    std::uint32_t addr = 0;
    if (!set_->cold().take(mod.state_bytes, 16, mem, addr)) {
      return LaunchResult::NoRoom;
    }
    app_state_ = mem;
    app_state_bytes_ = mod.state_bytes;
  }
  std::memset(app_state_, 0, mod.state_bytes);
  for (unsigned i = 0; i < mod.param_count && i < O3M_PARAMS_MAX; ++i) {
    app_values_[i] = mod.params[i].def;
  }
  app_index_ = idx;
  copy_text(app_stem_, sizeof app_stem_, e.stem);
  app_minor_ = lm->header.abi_minor;
  // OS-APPUX S3: for an app of ABI 1.10 what this project remembers (same name, same ABI major) is its row values from the start: they are handed to set_param() below, BEFORE enter(), so an app that stores
  // its settings with set_own_param finds them back and must not reset a declared value in enter(). An app of ABI 1.2 .. 1.9 keeps the order it always had: enter() sees the defaults and the remembered values
  // reach its set_param() afterwards, through app_params_apply (the launching actions call it): an enter() of that kind may reset a declared value, and a value delivered earlier would be lost.
  if (project != nullptr && lm->header.abi_minor >= 10u) {
    const model::AppParams* saved = project->find_app_params(e.stem, std::strlen(e.stem));
    if (saved != nullptr && saved->abi_major == mod.abi_major) {
      for (unsigned i = 0; i < saved->count && i < mod.param_count; ++i) {
        (void)set_param_value(&mod, i, saved->value[i], false);
      }
    }
  }
  // The host table this app is lent: the voices' (log, math, draw) plus, for a module of ABI 1.3 or later, the tables its capabilities earned. A 1.2 module's file has capability word 0
  // and is lent nothing new.
  app_host_ = voices_->host();
  const bool abi3 = lm->header.abi_minor >= 3u;
  if (!abi3) {
    app_host_.size = static_cast<std::uint32_t>(offsetof(o3m_host, read));  // exactly the 1.2 table: a 1.2 module that checks `size` or O3M_HOST_HAS sees no 1.3 member at all
    app_host_.abi_minor = 2u;
  }
  app_host_.read = abi3 && (caps & O3M_CAP_READ_PROJECT) != 0u ? &kReadApi : nullptr;
  app_host_.edit = abi3 && (caps & O3M_CAP_EDIT_PROJECT) != 0u ? &kEditApi : nullptr;
  const bool midi_in = abi3 && (caps & O3M_CAP_MIDI_IN) != 0u && midi_ != nullptr;
  const bool tap = abi3 && (caps & O3M_CAP_AUDIO_TAP) != 0u && tap_ != nullptr;
  const bool exporting = lm->header.abi_minor >= 7u && (caps & O3M_CAP_EXPORT_FILES) != 0u;  // OS-H4: the calls are lent to an app of ABI 1.7 that asked; without a service they answer NO_CARD
  const bool abi10 = lm->header.abi_minor >= 10u;  // OS-APPUX S3: get_transport and set_own_param are lent to every app of 1.10, play_note and stop_note to one that asked for play_notes
  play_on_ = abi10 && (caps & O3M_CAP_PLAY_NOTES) != 0u;
  io_ = o3m_io_api{O3M_IO_API_VERSION, sizeof(o3m_io_api), tramp_now_ms, midi_in ? tramp_midi_read : nullptr, midi_in ? tramp_midi_lost : nullptr,
                   tap ? tramp_tap_select : nullptr, tap ? tramp_tap_latest : nullptr, tap ? tramp_tap_stream : nullptr, tap ? tramp_tap_levels : nullptr,
                   exporting ? tramp_export_open : nullptr, exporting ? tramp_export_write : nullptr, exporting ? tramp_export_close : nullptr, exporting ? tramp_export_status : nullptr,
                   abi10 ? tramp_get_transport : nullptr, abi10 ? tramp_set_own_param : nullptr, play_on_ ? tramp_play_note : nullptr, play_on_ ? tramp_stop_note : nullptr};
  std::memset(held_, 0, sizeof held_);
  triggers_ = 0;
  app_host_.io = abi3 ? &io_ : nullptr;
  project_ = project;
  tempo_req_ = 0;
  g_open_ = closed_ = g_over_ = g_tempo_set_ = false;
  g_ops_ = g_steps_ = g_calls_ = g_params_ = 0;
  abandon_logged_ = false;
  app_entry_ = static_cast<int>(entry);
  launch_ctx_ = o3m_launch_ctx{};
  launch_ctx_.size = sizeof(o3m_launch_ctx);
  if (ctx != nullptr && (ctx->flags & O3M_LAUNCH_FROM_TOOLS) != 0u) {
    launch_ctx_ = *ctx;
    launch_ctx_.size = sizeof(o3m_launch_ctx);
  }
  g_host = this;
  if (exporting && export_ != nullptr) {
    export_->begin_app(e.stem);  // the app's folder is its stem; its file count starts again
    export_on_ = true;
  }
  if (tap) {
    tap_->set_source(audio::AppTap::Source::Off);  // off until the app selects a point
    tap_->set_samples(false);                       // and levels only until it reads samples (arm_tap_samples)
    tap_fresh_ = tap_stream_cur_ = tap_levels_cur_ = tap_->head();
    tap_on_ = true;
  }
  if (midi_in) {
    midi_->clear();  // what arrived before the app: not its business
    midi_overflow0_ = midi_->overflow();
    midi_sysex0_ = midi_->sysex_dropped();
    midi_->set_enabled(true);
    midi_on_ = true;
  }
  begin_call(Call::Init);
  const int init_status = views_.call_init(mod, app_state_, module::fault_id(e.stem), app_host_);
  end_call();
  if (init_status != 0) {
    release_host();
    return LaunchResult::InitFailed;
  }
  app_running_ = true;
  app_stopped_ = false;
  app_verdict_ = module::ViewStatus::Ok;
  keys_declared_ = false;
  std::memset(keys_, 0, sizeof keys_);
  for (unsigned i = 0; i < mod.param_count && i < O3M_PARAMS_MAX && !app_stopped_; ++i) {
    begin_call(Call::SetParam);
    const module::ViewStatus v = views_.call_set_param(mod, app_state_, module::fault_id(e.stem), i, app_values_[i]);
    end_call(v == module::ViewStatus::Time);
    if (v == module::ViewStatus::Time) {
      app_stopped_ = true;
      app_verdict_ = v;
      fault(idx, v);
    }
  }
  if (!app_stopped_) {
    begin_call(Call::Enter);
    const module::ViewStatus v = views_.call_enter(mod, app_state_, module::fault_id(e.stem));
    end_call(v == module::ViewStatus::Time);
    if (v == module::ViewStatus::Time) {
      app_stopped_ = true;
      app_verdict_ = v;
      fault(idx, v);
    }
  }
  if (!app_stopped_) {
    refresh_keys();  // the first row, before the page draws
  }
  return LaunchResult::Ok;
}

// OS-KEYROWDLG S5, ABI 1.9: the labels of the page's key row. keys() is app code: it runs under begin_call / end_call and the input time budget, and an overrun stops and quarantines the app like any other call.
// The table and the strings are the app's pointers, so ViewHost::call_keys copies the 16 labels INSIDE the call, under the app's crash stamp (a wild pointer faults in the app's context, where the quarantine
// finds it): a label is cut to 7 characters, one with a character that is not printable ASCII is blank, a NULL table or a NULL label is blank, and F8's two slots are ignored (the OS draws BACK there, Shift + F8
// stays blank).
void ModuleUi::refresh_keys() noexcept {
  const o3m_module* m = app_module();
  if (m == nullptr || app_stopped_) {
    keys_declared_ = false;
    return;
  }
  begin_call(Call::Keys);
  const module::ViewStatus v = views_.call_keys(*m, app_state_, module::fault_id(app_stem_), keys_);
  keys_declared_ = v == module::ViewStatus::Ok;
  end_call(v == module::ViewStatus::Time);
  if (v == module::ViewStatus::Time) {
    app_stopped_ = true;
    app_verdict_ = v;
    fault(app_index_, v);
  }
}

bool ModuleUi::app_set_param(unsigned index, float value) noexcept { return set_param_value(app_module(), index, value, true); }

// `echo`: tell the app (its set_param) when the value changed. The page's own edits do; the app's set_own_param does not (it holds the value it just stored) and neither does the launch of an app of 1.10 over
// the remembered values (the set_param loop that follows sends every row; an older app gets them through app_params_apply, with the echo).
bool ModuleUi::set_param_value(const o3m_module* m, unsigned index, float value, bool echo) noexcept {
  ParamScratch s;
  if (m == nullptr || index >= m->param_count || index >= O3M_PARAMS_MAX || !describe_module_param(m->params[index], s)) {
    return false;
  }
  float v = core::param_clamp(s.desc, value);
  if (s.whole) {
    v = static_cast<float>(static_cast<std::int32_t>(v + (v < 0.0f ? -0.5f : 0.5f)));
    v = core::param_clamp(s.desc, v);
  }
  if (v == app_values_[index]) {
    return false;
  }
  app_values_[index] = v;
  if (echo && !app_stopped_ && m->set_param != nullptr) {
    begin_call(Call::SetParam);
    const module::ViewStatus st = views_.call_set_param(*m, app_state_, module::fault_id(app_stem_), index, v);
    end_call(st == module::ViewStatus::Time);
    if (st == module::ViewStatus::Time) {
      app_stopped_ = true;
      app_verdict_ = st;
      fault(app_index_, st);
    }
  }
  return true;
}

bool ModuleUi::app_input(std::uint32_t kind, std::uint32_t id, std::int32_t value, bool shift) noexcept {
  const o3m_module* m = app_module();
  if (m == nullptr || app_stopped_) {
    return false;
  }
  const o3m_ui_event ev = {kind, id, value, shift ? O3M_MOD_SHIFT : 0u};
  const std::uint32_t t0 = clock_ != nullptr ? clock_->now() : 0u;
  begin_call(Call::Input);
  const module::ViewStatus v = views_.call_input(*m, app_state_, module::fault_id(app_stem_), ev);
  end_call(v == module::ViewStatus::Time);  // a group the app left open is closed and applied; an app stopped for its time loses it (it must leave no partial result)
  if (v == module::ViewStatus::Time) {
    app_stopped_ = true;
    app_verdict_ = v;
    fault(app_index_, v);
  } else {
    refresh_keys();  // ABI 1.9: what the keys do may have changed with this input (the row is also asked once a frame, draw_app_view)
  }
  if (clock_ != nullptr) {
    pass_us_ += clock_->now() - t0;  // the app's calls and what applying its group cost the main loop: the pass's share (pass_full)
  }
  return true;
}

bool ModuleUi::draw_app_view(ui::Renderer& r, const ui::Rect& area) noexcept {
  const o3m_module* m = app_module();
  if (m == nullptr || app_stopped_ || area.w < module::kViewMinSize || area.h < module::kViewMinSize) {
    return false;
  }
  const ui::Theme& t = r.theme();
  module::ViewHost::Metrics metrics;
  for (unsigned f = 0; f < ui::kFaceCount; ++f) {
    metrics.cell_w[f] = static_cast<std::int16_t>(t.geometry(static_cast<ui::Face>(f)).cell_w);
    metrics.cell_h[f] = static_cast<std::int16_t>(t.geometry(static_cast<ui::Face>(f)).cell_h);
  }
  views_.set_metrics(metrics);
  const int w = area.w < module::kViewMaxW ? area.w : module::kViewMaxW;
  const int h = area.h < module::kViewMaxH ? area.h : module::kViewMaxH;
  const unsigned count = m->param_count < O3M_PARAMS_MAX ? m->param_count : O3M_PARAMS_MAX;
  view_words_ = 0;  // an app's page has one view: all of the words
  refresh_keys();   // ABI 1.9: once a frame, before the page draws
  if (app_stopped_) {
    return false;
  }
  begin_call(Call::Draw);
  const module::ViewStatus verdict = views_.draw(*m, app_state_, module::fault_id(app_stem_), 0, w, h, app_values_, count);
  end_call(verdict != module::ViewStatus::Ok);
  if (verdict != module::ViewStatus::Ok) {
    app_stopped_ = true;
    app_verdict_ = verdict;
    fault(app_index_, verdict);
    return false;
  }
  replay(r, ui::Rect{area.x, area.y, w, h});
  return true;
}

void ModuleUi::app_exit() noexcept {
  const o3m_module* m = app_module();
  if (m == nullptr) {
    release_host();
    app_running_ = false;
    return;
  }
  if (!app_stopped_) {  // a stopped app is not called again, not even to say goodbye: it is what went wrong
    begin_call(Call::Exit);
    const module::ViewStatus xv = views_.call_exit(*m, app_state_, module::fault_id(app_stem_));
    end_call(xv == module::ViewStatus::Time);
    begin_call(Call::Exit);
    views_.call_shutdown(*m, app_state_, module::fault_id(app_stem_));
    end_call();
  }
  release_host();
  app_running_ = false;
}

// ---- the host calls of the running app (OS-H1) -----------------------------------------------------------------------------------------------

std::uint32_t ModuleUi::host_tempo() const noexcept {
  if (g_open_ && g_tempo_set_) {
    return g_tempo_;  // read your write: the open group holds it
  }
  if (tempo_req_ != 0u) {
    return tempo_req_;  // read your write: the request has not been applied yet
  }
  return project_ != nullptr ? project_->tempo_x100() : 0u;
}

int ModuleUi::host_set_tempo(std::uint32_t bpm_x100) noexcept {
  if (project_ == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  if (g_open_) {  // OS-H3A (amendment D.6): inside a group the tempo is part of the group's undo unit, applied with it at edit_end
    if (g_over_) {
      return O3M_EDIT_LIMIT;
    }
    if (++g_calls_ > O3M_EDIT_MAX_CALLS) {
      g_over_ = true;
      return O3M_EDIT_LIMIT;
    }
    if (bpm_x100 < model::kMinTempoX100 || bpm_x100 > model::kMaxTempoX100) {
      ++n_refused_;
      return O3M_EDIT_RANGE;
    }
    g_tempo_ = bpm_x100;
    g_tempo_set_ = true;
    return O3M_EDIT_OK;
  }
  if (bpm_x100 < model::kMinTempoX100 || bpm_x100 > model::kMaxTempoX100) {
    return O3M_EDIT_RANGE;
  }
  tempo_req_ = bpm_x100;
  return O3M_EDIT_OK;
}

bool ModuleUi::take_tempo_request(std::uint32_t& bpm_x100) noexcept {
  if (tempo_req_ == 0u) {
    return false;
  }
  bpm_x100 = tempo_req_;
  tempo_req_ = 0;
  return true;
}

std::uint32_t ModuleUi::host_now_ms() const noexcept { return clock_ != nullptr ? static_cast<std::uint32_t>(clock_->uptime_ms()) : 0u; }

std::uint32_t ModuleUi::host_midi_read(o3m_midi_event* out, std::uint32_t max) noexcept {
  if (out == nullptr || !midi_on_ || midi_ == nullptr) {
    return 0;
  }
  std::uint32_t n = 0;
  midi::MidiMonitor::Event e;
  if (max > midi::MidiMonitor::kRingSize) {
    max = midi::MidiMonitor::kRingSize;  // one call never takes more than the ring holds
  }
  while (n < max && midi_->pop(e)) {
    out[n++] = o3m_midi_event{e.port, e.status, e.data1, e.data2};
  }
  return n;
}

void ModuleUi::host_midi_lost(std::uint32_t* overflow, std::uint32_t* sysex) const noexcept {
  if (!midi_on_ || midi_ == nullptr) {
    return;
  }
  if (overflow != nullptr) {
    *overflow = midi_->overflow() - midi_overflow0_;
  }
  if (sysex != nullptr) {
    *sysex = midi_->sysex_dropped() - midi_sysex0_;
  }
}

// ---- the audio tap (OS-H2a) -------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kTapSettle = 2u * audio::AppTap::kBlock;  // frames after a select that may still carry the previous point's data (the render switches at a block boundary)
}  // namespace

int ModuleUi::host_tap_select(std::uint32_t source) noexcept {
  if (!tap_on_ || tap_ == nullptr || source > static_cast<std::uint32_t>(audio::AppTap::Source::Master)) {
    return 1;
  }
  tap_->set_source(static_cast<audio::AppTap::Source>(source));
  tap_fresh_ = tap_->head() + kTapSettle;
  tap_stream_cur_ = tap_fresh_;
  tap_levels_cur_ = tap_fresh_;
  return 0;
}

// The app reads samples for the first time (since its launch): the render starts storing them, and every position written before the render has seen the flag is stale. The flag goes FIRST
// and the head is read after it, so the render's block in flight (it may have loaded the old flag) is below the freshness point: head + kTapSettle is two blocks on, the first block that
// certainly carries samples is the second. latest and stream share the point (their results are zeros / nothing until it is passed, as after a select). The render is an interrupt on this
// core, so the order that matters is the compiler's: the signal fence keeps the relaxed flag store before the acquire head load (no instruction on the M4, one core).
void ModuleUi::arm_tap_samples() noexcept {
  if (tap_->samples()) {
    return;
  }
  tap_->set_samples(true);
  std::atomic_signal_fence(std::memory_order_seq_cst);
  tap_fresh_ = tap_->head() + kTapSettle;
  tap_stream_cur_ = tap_fresh_;
}

std::uint32_t ModuleUi::host_tap_latest(std::int16_t* out, std::uint32_t frames) noexcept {
  if (!tap_on_ || tap_ == nullptr || tap_->source() == audio::AppTap::Source::Off || out == nullptr || (reinterpret_cast<std::uintptr_t>(out) & 3u) != 0u || frames < 2u ||
      frames > O3M_TAP_LATEST_MAX || (frames & 1u) != 0u) {
    return 0;
  }
  arm_tap_samples();
  const audio::AppTap::Ring::Window w = tap_->read_latest(out, frames);
  if (!w.whole) {
    return 0;  // the writer lapped the window on every try: not a signal to hand out
  }
  const std::int32_t since = static_cast<std::int32_t>(w.head - tap_fresh_);  // frames written since the point was fresh (negative: not yet)
  const std::uint32_t fresh = since > 0 ? static_cast<std::uint32_t>(since) : 0u;
  if (fresh >= frames) {
    return frames;
  }
  std::memset(out, 0, static_cast<std::size_t>(frames - fresh) * sizeof(std::int16_t));  // older than the select: silence in front, the real frames at the end
  return fresh & ~1u;
}

std::uint32_t ModuleUi::host_tap_stream(std::int16_t* out, std::uint32_t max_frames, std::uint32_t* lost_frames) noexcept {
  if (lost_frames != nullptr) {
    *lost_frames = 0;
  }
  if (!tap_on_ || tap_ == nullptr || tap_->source() == audio::AppTap::Source::Off || out == nullptr || (reinterpret_cast<std::uintptr_t>(out) & 3u) != 0u) {
    return 0;
  }
  arm_tap_samples();
  const std::uint32_t head = tap_->head();
  if (static_cast<std::int32_t>(head - tap_stream_cur_) <= 0) {
    return 0;  // nothing new (or the point is not fresh yet)
  }
  std::uint32_t cap = max_frames < O3M_TAP_STREAM_MAX ? max_frames : static_cast<std::uint32_t>(O3M_TAP_STREAM_MAX);
  cap &= ~1u;
  const std::uint32_t pending = head - tap_stream_cur_;
  const std::uint32_t take = pending < cap ? pending : cap;
  if (take == 0u) {
    return 0;
  }
  // The range is decided from ONE load of the head: the newest `take` frames up to `head` (any older backlog is dropped, as everywhere in the taps, and counted). A block the render publishes
  // meanwhile moves nothing; it is simply read by the next call.
  const std::uint32_t lost = pending - take;
  tap_stream_cur_ = head;
  if (!tap_->read_range(head - take, out, take)) {  // lapped while we copied: the contents are not trustworthy
    if (lost_frames != nullptr) {
      *lost_frames = lost + take;
    }
    return 0;
  }
  if (lost_frames != nullptr) {
    *lost_frames = lost;
  }
  return take;
}

void ModuleUi::host_tap_levels(o3m_tap_levels* out) noexcept {
  if (out == nullptr || out->size < sizeof(std::uint32_t)) {
    return;
  }
  o3m_tap_levels r{};
  r.size = sizeof r;
  if (tap_on_ && tap_ != nullptr && tap_->source() != audio::AppTap::Source::Off && static_cast<std::int32_t>(tap_->head() - tap_levels_cur_) > 0) {
    audio::AppTap::Levels l;
    (void)tap_->levels_since(tap_levels_cur_, l);
    r.frames = l.frames;
    r.lost_frames = l.lost;
    r.clip_blocks = l.clip_blocks;
    r.peak = audio::AppTap::linear_peak(l);
    r.rms = audio::AppTap::linear_rms(l);
  }
  const std::uint32_t room = out->size < sizeof r ? out->size : static_cast<std::uint32_t>(sizeof r);
  std::memcpy(out, &r, room);
  out->size = room;
}

// ---- instrument parameters (OS-H3B, ABI 1.6) -----------------------------------------------------------------------------------------------------

namespace {

static_assert(O3M_PGROUP_VOICE == model::instrument_param::GroupVoice && O3M_PGROUP_FILTER == model::instrument_param::GroupFilter && O3M_PGROUP_ENV == model::instrument_param::GroupEnvelope &&
                  O3M_PGROUP_SENDS == model::instrument_param::GroupSends && O3M_PGROUP_SOURCE == model::instrument_param::GroupSource,
              "O3M_PGROUP_* follow the model's instrument groups");
static_assert(O3M_SAMPLE_PARAM_COUNT == model::instrument_param::kSdkCount && O3M_PARAMS_MAX == model::instrument_param::kModuleMax, "the SDK's counts are the model's");

// The most values a parameter may have and still be STEPPED (a switch for an app that blends two values): 48 semitones of TUNE, the 12 bit depths; the 100-step levels and the millisecond times are not.
constexpr float kSteppedSpan = 64.0f;

// One parameter as get_param_desc says it.
struct PInfo {
  float min, max, def;
  std::uint16_t id, flags;
  std::uint8_t group;
  char name[O3M_PARAM_DESC_NAME];
};

std::uint32_t fnv1a(const char* s) noexcept {
  std::uint32_t h = 2166136261u;
  for (; *s != '\0'; ++s) {
    h = (h ^ static_cast<std::uint8_t>(*s)) * 16777619u;
  }
  return h;
}

// The instrument of slot `slot` and what kind of thing it is for the ABI.
unsigned instrument_kind(const model::Instrument* in) noexcept {
  if (in == nullptr) {
    return O3M_INSTRUMENT_EMPTY;
  }
  switch (in->kind) {
    case model::InstrumentKind::Sample: return O3M_INSTRUMENT_SAMPLE;
    case model::InstrumentKind::Midi: return O3M_INSTRUMENT_MIDI;
    case model::InstrumentKind::Module: return O3M_INSTRUMENT_MODULE;
    case model::InstrumentKind::None:
    default: return O3M_INSTRUMENT_EMPTY;
  }
}

void fill_sample(const core::ParamDesc& d, const model::ParamField& f, unsigned field, PInfo& out) noexcept {
  out.min = d.min;
  out.max = d.max;
  out.def = d.def;
  out.id = static_cast<std::uint16_t>(field);
  out.flags = static_cast<std::uint16_t>((d.unit == core::ParamUnit::Enum ? O3M_PD_ENUM : (f.store != model::ParamStore::F32 && d.max - d.min <= kSteppedSpan ? O3M_PD_STEPPED : 0u)) |
                                        (model::instrument_field_app_writable(field) ? O3M_PD_APP_WRITABLE : 0u));
  out.group = static_cast<std::uint8_t>(model::instrument_param::is_mod_field(field) ? O3M_PGROUP_MOD : d.group);
  std::memset(out.name, 0, sizeof out.name);
  copy_text(out.name, sizeof out.name, d.name);
}

bool fill_module(const ModuleUi& mu, const model::Project& project, unsigned slot, unsigned index, PInfo& out) noexcept {
  ParamScratch sc;
  if (!mu.describe(project, slot, index, sc)) {
    return false;
  }
  out.min = sc.desc.min;
  out.max = sc.desc.max;
  out.def = sc.desc.def;
  out.id = static_cast<std::uint16_t>(model::instrument_param::kModuleFirst + index);
  out.flags = static_cast<std::uint16_t>((sc.desc.unit == core::ParamUnit::Enum ? O3M_PD_ENUM : (sc.whole && sc.desc.max - sc.desc.min <= kSteppedSpan ? O3M_PD_STEPPED : 0u)) | O3M_PD_APP_WRITABLE);
  out.group = static_cast<std::uint8_t>(index / 8u);
  std::memset(out.name, 0, sizeof out.name);
  copy_text(out.name, sizeof out.name, sc.name);
  return true;
}

// Parameter `id` (the project's field id) of the instrument in `slot`; false when the instrument is not a SAMPLE or a loaded MODULE, or has no such parameter.
bool find_param_by_id(const ModuleUi& mu, const model::Project& project, unsigned slot, unsigned id, PInfo& out) noexcept {
  const model::Instrument* in = slot < model::kMaxInstruments ? project.instrument(slot) : nullptr;
  if (in == nullptr) {
    return false;
  }
  if (in->kind == model::InstrumentKind::Sample) {
    model::ParamField f;
    if (id >= model::instrument_param::kModuleFirst || model::instrument_param::is_midi_row(id) || !model::find_param(core::param_id(core::ParamKind::Instrument, slot, id), f)) {
      return false;
    }
    fill_sample(*f.desc, f, id, out);
    return true;
  }
  return in->kind == model::InstrumentKind::Module && model::instrument_param::is_module_field(id) && fill_module(mu, project, slot, id - model::instrument_param::kModuleFirst, out);
}

// Parameter number `index` of the listing (0 .. param_count - 1).
bool find_param_by_index(const ModuleUi& mu, const model::Project& project, unsigned slot, unsigned index, PInfo& out) noexcept {
  const model::Instrument* in = slot < model::kMaxInstruments ? project.instrument(slot) : nullptr;
  if (in == nullptr) {
    return false;
  }
  if (in->kind == model::InstrumentKind::Sample) {
    const unsigned field = index < model::instrument_param::kSdkCount ? model::param_field_of_slot(core::ParamKind::Instrument, index) : model::kNoParamSlot;
    return field != model::kNoParamSlot && find_param_by_id(mu, project, slot, field, out);
  }
  return in->kind == model::InstrumentKind::Module && fill_module(mu, project, slot, index, out);
}

}  // namespace

// ---- steps and edit groups (OS-H3A, ABI 1.5) ----------------------------------------------------------------------------------------------------------

void ModuleUi::begin_call(Call kind) noexcept {
  call_ = kind;
  group_in_call_ = false;
}

// The app's call returned. A group it left open is closed by the OS: its changes are KEPT and applied (one undo unit, so the user can take them back), counted, and the first one of a launch is logged.
// `faulted` (the call went over its time budget: the app is about to be stopped and quarantined): the open group is DROPPED instead, a faulted app leaves no partial result (a group it closed with
// edit_end before the overrun was applied then and stands). Then the next call may make a group again.
void ModuleUi::end_call(bool faulted) noexcept {
  if (g_open_) {
    ++n_abandoned_;
    if (!abandon_logged_ && app_host_.log != nullptr) {
      abandon_logged_ = true;
      app_host_.log(faulted ? "app: edit group left open by a stopped app, dropped" : "app: edit group left open, closed by the OS");
    }
    if (faulted) {
      g_open_ = g_over_ = g_tempo_set_ = false;
      g_ops_ = g_steps_ = g_calls_ = g_params_ = 0;
      ++n_dropped_;
    } else {
      (void)close_group();
    }
  }
  group_in_call_ = false;
  call_ = Call::None;
}

std::int32_t ModuleUi::read_range_ok(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, std::uint8_t count, const model::Pattern*& pat) const noexcept {
  if (project_ == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  const model::Pattern* p = pattern >= 1u && pattern <= 255u ? project_->pattern(pattern) : nullptr;
  if (p == nullptr || track >= project_->active_tracks()) {
    return O3M_EDIT_RANGE;
  }
  const unsigned len = p->track_length[track];
  if (count == 0u || first >= len || count > len - first) {
    return O3M_EDIT_RANGE;
  }
  pat = p;
  return O3M_EDIT_OK;
}

std::int32_t ModuleUi::host_pattern_info(std::uint16_t pattern, o3m_pattern_info* out) const noexcept {
  if (project_ == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  const model::Pattern* p = pattern >= 1u && pattern <= 255u ? project_->pattern(pattern) : nullptr;
  if (p == nullptr || out == nullptr || out->size < sizeof(std::uint32_t)) {
    return O3M_EDIT_RANGE;
  }
  o3m_pattern_info r{};
  r.size = sizeof r;
  r.length = project_->pattern_length(pattern);
  r.tracks = project_->active_tracks();
  for (unsigned t = 0; t < r.tracks && t < O3M_PATTERN_TRACKS_MAX; ++t) {
    r.track_length[t] = p->track_length[t];
  }
  const std::uint32_t room = out->size < sizeof r ? out->size : static_cast<std::uint32_t>(sizeof r);
  std::memcpy(out, &r, room);
  out->size = room;
  return O3M_EDIT_OK;
}

std::int32_t ModuleUi::host_get_steps(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, o3m_step* out, std::uint8_t count) const noexcept {
  const model::Pattern* p = nullptr;
  const std::int32_t r = read_range_ok(pattern, track, first, count, p);
  if (r != O3M_EDIT_OK) {
    return r;
  }
  if (out == nullptr) {
    return O3M_EDIT_RANGE;
  }
  for (unsigned k = 0; k < count; ++k) {
    const model::Step& s = p->steps[track][first + k];
    out[k] = o3m_step{s.note, s.instr, s.vol, s.fx[0].type, s.fx[0].value, s.fx[1].type, s.fx[1].value, 0};
  }
  return O3M_EDIT_OK;
}

std::int32_t ModuleUi::host_edit_begin() noexcept {
  if (project_ == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  if (g_open_) {
    return O3M_EDIT_GROUP_OPEN;
  }
  if (call_ != Call::Input || group_in_call_) {  // one group per call into the app, and only from input()
    return O3M_EDIT_RATE;
  }
  g_open_ = true;
  group_in_call_ = true;
  g_ops_ = g_steps_ = g_calls_ = g_params_ = 0;
  g_over_ = g_tempo_set_ = false;
  return O3M_EDIT_OK;
}

namespace {

// A value the model carries and the Pattern can show (step.h): 129..252 and the reserved FX types 43..127 are refused on a write.
bool step_values_ok(const o3m_step& s, std::uint8_t fields) noexcept {
  if ((fields & O3M_STEP_NOTE) != 0u && s.note > O3M_NOTE_LAST && s.note < O3M_NOTE_FADE) {
    return false;
  }
  if ((fields & O3M_STEP_INSTR) != 0u && s.instr > O3M_INSTR_MAX) {
    return false;
  }
  if ((fields & O3M_STEP_VOL) != 0u && s.vol > O3M_VOL_MAX && s.vol != O3M_VOL_UNSET) {
    return false;
  }
  const auto fx_ok = [](std::uint8_t type) { return type <= O3M_FX_OG_LAST || type >= O3M_FX_ENGINE_FIRST; };
  return ((fields & O3M_STEP_FX1) == 0u || fx_ok(s.fx1_type)) && ((fields & O3M_STEP_FX2) == 0u || fx_ok(s.fx2_type));
}

void merge_step(model::Step& d, const o3m_step& s, std::uint8_t fields) noexcept {
  if ((fields & O3M_STEP_NOTE) != 0u) {
    d.note = s.note;
  }
  if ((fields & O3M_STEP_INSTR) != 0u) {
    d.instr = s.instr;
  }
  if ((fields & O3M_STEP_VOL) != 0u) {
    d.vol = s.vol;
  }
  if ((fields & O3M_STEP_FX1) != 0u) {
    d.fx[0].type = s.fx1_type;
    d.fx[0].value = s.fx1_value;
  }
  if ((fields & O3M_STEP_FX2) != 0u) {
    d.fx[1].type = s.fx2_type;
    d.fx[1].value = s.fx2_value;
  }
}

}  // namespace

std::int32_t ModuleUi::host_set_steps(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, const o3m_step* s, std::uint8_t count, std::uint8_t fields) noexcept {
  if (!g_open_) {
    return O3M_EDIT_NO_GROUP;
  }
  if (g_over_) {
    return O3M_EDIT_LIMIT;  // from the write that crossed a limit on, nothing is stored
  }
  if (++g_calls_ > O3M_EDIT_MAX_CALLS) {
    g_over_ = true;
    return O3M_EDIT_LIMIT;
  }
  const model::Pattern* p = nullptr;
  std::int32_t r = read_range_ok(pattern, track, first, count, p);
  if (r == O3M_EDIT_OK && (s == nullptr || fields == 0u || (fields & ~static_cast<std::uint8_t>(O3M_STEP_ALL)) != 0u)) {
    r = O3M_EDIT_RANGE;
  }
  for (unsigned k = 0; r == O3M_EDIT_OK && k < count; ++k) {
    if (!step_values_ok(s[k], fields)) {
      r = O3M_EDIT_RANGE;
    }
  }
  if (r != O3M_EDIT_OK) {
    ++n_refused_;
    return r;
  }
  if (g_steps_ + count > O3M_EDIT_MAX_STEPS) {
    g_over_ = true;
    return O3M_EDIT_LIMIT;
  }
  // (g_ops_ <= g_steps_ <= 2,048: every op holds at least one step, so the ops table cannot overflow before the pool does)
  ops_[g_ops_++] = EditOp{static_cast<std::uint8_t>(pattern), track, first, count, fields};
  std::memcpy(&pool_[g_steps_], s, static_cast<std::size_t>(count) * sizeof(o3m_step));
  g_steps_ = static_cast<std::uint16_t>(g_steps_ + count);
  return O3M_EDIT_OK;
}

std::int32_t ModuleUi::host_set_step(std::uint16_t pattern, std::uint8_t track, std::uint8_t row, const o3m_step* s, std::uint8_t fields) noexcept {
  return host_set_steps(pattern, track, row, s, 1, fields);
}

// ---- the transport, the app's own rows and its notes (OS-APPUX S3, ABI 1.10) -----------------------------------------------------------------------

std::int32_t ModuleUi::host_get_transport(o3m_transport* out) const noexcept {
  if (out == nullptr || out->size < sizeof(std::uint32_t)) {
    return 1;
  }
  const std::uint32_t want = out->size < sizeof(o3m_transport) ? out->size : static_cast<std::uint32_t>(sizeof(o3m_transport));
  o3m_transport t = {};
  t.size = sizeof t;
  t.order = t.queued = audio::PlayPosition::kNoOrder;
  if (position_ != nullptr) {
    const audio::PlayPosition::Value v = position_->read();
    t.playing = v.playing ? 1u : 0u;
    t.pattern = v.pattern;
    t.order = v.order;
    t.row = v.step;
    t.ustep = v.ustep;
    t.tempo_x100 = v.tempo_x100;
    t.count_in = v.count_in;
    t.queued = v.queued;
  }
  const std::uint32_t keep = out->size;
  std::memcpy(out, &t, want);
  out->size = keep < sizeof(o3m_transport) ? keep : static_cast<std::uint32_t>(sizeof(o3m_transport));
  return position_ != nullptr ? 0 : 1;
}

std::int32_t ModuleUi::host_set_own_param(std::uint32_t index, float value) noexcept {
  const o3m_module* m = app_module();
  if (m == nullptr || index >= m->param_count || index >= O3M_PARAMS_MAX || !(value - value == 0.0f)) {  // (a NaN or an infinity is not a value)
    return O3M_OWN_RANGE;
  }
  (void)set_param_value(m, index, value, false);  // the app holds the value it just stored: no set_param back; app_params_sync keeps it with the project
  return O3M_OWN_OK;
}

std::int32_t ModuleUi::host_play_note(std::uint32_t instrument, std::uint32_t note, std::uint32_t velocity) noexcept {
  if (!play_on_ || !app_running_) {
    return O3M_PLAY_NO_AUDIO;
  }
  if (instrument < 1u || instrument > O3M_INSTR_MAX || note > 127u || velocity < 1u || velocity > 127u) {
    return O3M_PLAY_RANGE;
  }
  if (notes_ == nullptr) {
    return O3M_PLAY_NO_AUDIO;
  }
  forget_ended_notes();
  // The slot: the same instrument and note again retriggers its slot; else a free one. A full table, or the pass's eight calls, refuse (LIMIT) and push nothing.
  unsigned slot = O3M_PLAY_MAX_HELD;
  unsigned free_slot = O3M_PLAY_MAX_HELD;
  for (unsigned i = 0; i < O3M_PLAY_MAX_HELD; ++i) {
    if (held_[i].instrument == instrument && held_[i].note == note) {
      slot = i;
    } else if (held_[i].instrument == 0u && free_slot == O3M_PLAY_MAX_HELD) {
      free_slot = i;
    }
  }
  const unsigned use = slot != O3M_PLAY_MAX_HELD ? slot : free_slot;
  if (use == O3M_PLAY_MAX_HELD || triggers_ >= O3M_PLAY_MAX_PER_FRAME) {
    return O3M_PLAY_LIMIT;
  }
  ++triggers_;
  if (slot != O3M_PLAY_MAX_HELD) {
    (void)notes_->app_note(false, use, instrument, note, velocity);  // started again: the old note first
  }
  if (!notes_->app_note(true, use, instrument, note, velocity)) {
    held_[use] = HeldNote{};
    return O3M_PLAY_NO_AUDIO;  // (a push that did not go: a full ring, no audio running: NO_AUDIO, not LIMIT; the slot is free again)
  }
  held_[use] = HeldNote{static_cast<std::uint8_t>(instrument), static_cast<std::uint8_t>(note)};
  return O3M_PLAY_OK;
}

std::int32_t ModuleUi::host_stop_note(std::uint32_t instrument) noexcept {
  if (!play_on_ || !app_running_) {
    return O3M_PLAY_NO_AUDIO;
  }
  if (instrument < 1u || instrument > O3M_INSTR_MAX) {
    return O3M_PLAY_RANGE;
  }
  forget_ended_notes();
  bool any = false;
  for (const HeldNote& h : held_) {
    any = any || h.instrument == instrument;
  }
  if (!any) {
    return O3M_PLAY_OK;  // nothing held there: not an error, and not a trigger
  }
  if (triggers_ >= O3M_PLAY_MAX_PER_FRAME) {
    return O3M_PLAY_LIMIT;
  }
  ++triggers_;
  for (unsigned i = 0; i < O3M_PLAY_MAX_HELD; ++i) {
    if (held_[i].instrument == instrument) {
      if (notes_ != nullptr) {
        (void)notes_->app_note(false, i, instrument, held_[i].note, 0);
      }
      held_[i] = HeldNote{};
    }
  }
  return O3M_PLAY_OK;
}

// Notes can end without this table being told: App::reset_input()'s and the platform's all-sources all-notes-off (notes_off(false)), the sink's own repeat of a release that was lost, and the export taking the
// engine (Engine::offline_enter ends every live note). When the sink reports no live note queued, held or flushing (PreviewSink::live_idle), the app holds none, so the table is emptied; otherwise a ghost
// would keep the four slots taken (LIMIT for ever) until the page closed. Main loop, at the start of play_note and stop_note.
void ModuleUi::forget_ended_notes() noexcept {
  if (notes_ == nullptr || !notes_->live_idle()) {
    return;
  }
  for (HeldNote& h : held_) {
    h = HeldNote{};
  }
}

// Every note the running app holds ends, with one AllOff of the app's source (a release that did not go is repeated by the sink's own flush). Called from release_host(): every exit path.
void ModuleUi::stop_app_notes() noexcept {
  bool any = false;
  for (HeldNote& h : held_) {
    any = any || h.instrument != 0u;
    h = HeldNote{};
  }
  if (any && notes_ != nullptr) {
    notes_->app_notes_off();
  }
}

// ---- export writes (OS-H4) -----------------------------------------------------------------------------------------------------------------------

std::int32_t ModuleUi::host_export_open(const char* name, const char* ext) noexcept { return export_on_ && export_ != nullptr ? export_->open(name, ext) : O3M_EXPORT_NO_CARD; }
std::int32_t ModuleUi::host_export_write(const void* buf, std::uint32_t n) noexcept { return export_on_ && export_ != nullptr ? export_->write(buf, n) : O3M_EXPORT_STATE; }
std::int32_t ModuleUi::host_export_close() noexcept { return export_on_ && export_ != nullptr ? export_->close() : O3M_EXPORT_STATE; }
std::uint32_t ModuleUi::host_export_status(char* final_name, std::uint32_t cap) noexcept { return export_on_ && export_ != nullptr ? export_->status(final_name, cap) : O3M_EXPORT_IDLE; }

std::int32_t ModuleUi::host_instrument_info(std::uint8_t instrument, o3m_instrument_info* out) const noexcept {
  if (project_ == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  if (instrument < 1u || instrument > model::kMaxInstruments || out == nullptr || out->size < sizeof(std::uint32_t)) {
    return O3M_EDIT_RANGE;
  }
  const unsigned slot = instrument - 1u;
  const model::Instrument* in = project_->instrument(slot);
  o3m_instrument_info r{};
  r.size = sizeof r;
  r.kind = instrument_kind(in);
  if (r.kind == O3M_INSTRUMENT_SAMPLE) {
    r.engine = in->engine;
    r.param_count = model::instrument_param::kSdkCount;
  } else if (r.kind == O3M_INSTRUMENT_MIDI) {
    r.midi_channel = in->midi_channel >= 1u && in->midi_channel <= 16u ? in->midi_channel : 1u;  // OS-H4 (ABI 1.7): the instrument's CHANNEL as the sequencer sends it (0 is channel 1 there)
  } else if (r.kind == O3M_INSTRUMENT_MODULE) {
    const model::ModuleRef* ref = project_->module(in->module_index);
    r.engine = ref != nullptr ? fnv1a(ref->name) : 0u;
    r.param_count = param_count(*project_, slot);
  }
  const std::uint32_t room = out->size < sizeof r ? out->size : static_cast<std::uint32_t>(sizeof r);
  std::memcpy(out, &r, room);
  out->size = room;
  return O3M_EDIT_OK;
}

std::int32_t ModuleUi::host_get_param_desc(std::uint8_t instrument, std::uint16_t index, o3m_param_desc* out) const noexcept {
  if (project_ == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  PInfo pi;
  if (instrument < 1u || instrument > model::kMaxInstruments || out == nullptr || out->size < sizeof(std::uint32_t) || !find_param_by_index(*this, *project_, instrument - 1u, index, pi)) {
    return O3M_EDIT_RANGE;
  }
  o3m_param_desc r{};
  r.size = sizeof r;
  r.min = pi.min;
  r.max = pi.max;
  r.def = pi.def;
  r.id = pi.id;
  r.flags = pi.flags;
  r.group = pi.group;
  std::memcpy(r.name, pi.name, sizeof r.name);
  const std::uint32_t room = out->size < sizeof r ? out->size : static_cast<std::uint32_t>(sizeof r);
  std::memcpy(out, &r, room);
  out->size = room;
  return O3M_EDIT_OK;
}

std::int32_t ModuleUi::host_get_instrument_param(std::uint8_t instrument, std::uint16_t id, float* out) const noexcept {
  if (project_ == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  PInfo pi;
  if (instrument < 1u || instrument > model::kMaxInstruments || out == nullptr || !find_param_by_id(*this, *project_, instrument - 1u, id, pi)) {
    return O3M_EDIT_RANGE;
  }
  const core::ParamId pid = core::param_id(core::ParamKind::Instrument, instrument - 1u, id);
  *out = project_->has_param(pid) ? project_->get_param(pid) : pi.def;  // (a module parameter the blob does not reach yet reads as the module's default, as the Params page shows it)
  return O3M_EDIT_OK;
}

std::int32_t ModuleUi::host_set_instrument_param(std::uint8_t instrument, std::uint16_t id, float value) noexcept {
  if (!g_open_) {
    return O3M_EDIT_NO_GROUP;
  }
  if (g_over_) {
    return O3M_EDIT_LIMIT;  // from the write that crossed a limit on, nothing is stored
  }
  if (++g_calls_ > O3M_EDIT_MAX_CALLS) {
    g_over_ = true;
    return O3M_EDIT_LIMIT;
  }
  PInfo pi;
  if (project_ == nullptr || instrument < 1u || instrument > model::kMaxInstruments || !find_param_by_id(*this, *project_, instrument - 1u, id, pi) || (pi.flags & O3M_PD_APP_WRITABLE) == 0u ||
      !(value >= pi.min && value <= pi.max)) {  // (NaN fails the comparison; nothing is clamped for the app)
    ++n_refused_;
    return O3M_EDIT_RANGE;
  }
  // (the rings behind write_param take 256 (the engine's) and 64 (a module's) changes before they drop one silently: a group may write O3M_EDIT_MAX_PARAM_WRITES parameters, the most a module's ring takes)
  if (g_steps_ + 1u > O3M_EDIT_MAX_STEPS || g_params_ >= O3M_EDIT_MAX_PARAM_WRITES) {
    g_over_ = true;
    return O3M_EDIT_LIMIT;
  }
  ++g_params_;
  ops_[g_ops_++] = EditOp{0, static_cast<std::uint8_t>(instrument - 1u), 0, 1, 0};
  ParamOp po{};
  po.value = value;
  po.field = id;
  std::memcpy(&pool_[g_steps_], &po, sizeof po);
  g_steps_ = static_cast<std::uint16_t>(g_steps_ + 1u);
  return O3M_EDIT_OK;
}

bool ModuleUi::next_param_op(unsigned& op, unsigned& at, core::ParamId& id, float& value) const noexcept {
  for (; op < g_ops_; ++op) {
    const unsigned here = at;
    at += ops_[op].count;
    if (ops_[op].pattern == 0u) {
      ParamOp po;
      std::memcpy(&po, &pool_[here], sizeof po);
      id = core::param_id(core::ParamKind::Instrument, ops_[op].track, po.field);
      value = po.value;
      ++op;
      return true;
    }
  }
  return false;
}

// Closes the group: dropped whole when it went over a limit (nothing was ever written), otherwise handed to the App's action, which applies it as one undo unit. The time that takes is the host's,
// not the app's: it does not count against the app's 4 ms for the call it is in.
std::int32_t ModuleUi::close_group() noexcept {
  if (!g_open_) {
    return O3M_EDIT_NO_GROUP;
  }
  g_open_ = false;
  std::int32_t r = O3M_EDIT_OK;
  if (g_over_) {
    ++n_dropped_;
    r = O3M_EDIT_LIMIT;
  } else if (g_ops_ != 0u || g_tempo_set_) {
    if (sink_ == nullptr || project_ == nullptr) {
      r = O3M_EDIT_NO_PROJECT;
    } else {
      closed_ = true;
      apply_result_ = O3M_EDIT_OK;
      const std::uint32_t t0 = clock_ != nullptr ? clock_->now() : 0u;
      r = sink_(sink_ctx_, app_entry_ >= 0 ? app_entry_ : 0);
      if (clock_ != nullptr && call_ == Call::Input) {
        views_.credit_host_us(clock_->now() - t0);
      }
      closed_ = false;
      if (r == O3M_EDIT_OK) {
        ++n_applied_;
      }
    }
  }
  g_ops_ = g_steps_ = g_calls_ = g_params_ = 0;
  g_over_ = g_tempo_set_ = false;
  return r;
}

std::int32_t ModuleUi::host_edit_end() noexcept { return close_group(); }

bool ModuleUi::apply_group(model::Editor& editor) noexcept {
  apply_result_ = O3M_EDIT_OK;
  extent_ = GroupExtent{};
  if (!closed_) {
    apply_result_ = O3M_EDIT_NO_GROUP;  // not a group of ours: a macro replaying the action
    return false;
  }
  const model::Project& project = editor.project();
  // The whole group is checked against the project before the first write, so that it is applied whole or not at all (the project cannot have changed since the calls, but the Editor's answer is not assumed).
  unsigned vat = 0;  // the pool slot of op i
  for (unsigned i = 0; i < g_ops_; ++i) {
    const EditOp& op = ops_[i];
    if (op.pattern == 0u) {  // a parameter op (OS-H3B): checked against the project again, written by the action
      ParamOp po;
      std::memcpy(&po, &pool_[vat], sizeof po);
      PInfo pi;
      if (!find_param_by_id(*this, project, op.track, po.field, pi) || (pi.flags & O3M_PD_APP_WRITABLE) == 0u || !(po.value >= pi.min && po.value <= pi.max)) {
        apply_result_ = O3M_EDIT_RANGE;
        return false;
      }
      vat += op.count;
      continue;
    }
    const model::Pattern* p = project.pattern(op.pattern);
    if (p == nullptr || op.track >= project.active_tracks() || op.first >= p->track_length[op.track] || op.count > p->track_length[op.track] - op.first) {
      apply_result_ = O3M_EDIT_RANGE;
      return false;
    }
    vat += op.count;
  }
  if (g_tempo_set_ && (g_tempo_ < model::kMinTempoX100 || g_tempo_ > model::kMaxTempoX100)) {
    apply_result_ = O3M_EDIT_RANGE;
    return false;
  }
  bool changed = false;
  if (g_tempo_set_ && g_tempo_ != project.tempo_x100()) {
    changed = editor.set_tempo_x100(g_tempo_) == model::Status::Ok;
  }
  model::Step buf[model::kMaxPatternLength];
  unsigned at = 0;
  for (unsigned i = 0; i < g_ops_; ++i) {
    const EditOp& op = ops_[i];
    if (op.pattern == 0u) {
      ++at;  // (a parameter op holds one pool slot; the action writes it)
      continue;
    }
    const model::Step* cur = &project.pattern(op.pattern)->steps[op.track][op.first];
    std::memcpy(buf, cur, static_cast<std::size_t>(op.count) * sizeof(model::Step));
    for (unsigned k = 0; k < op.count; ++k) {
      merge_step(buf[k], pool_[at + k], op.fields);
    }
    at += op.count;
    if (std::memcmp(buf, cur, static_cast<std::size_t>(op.count) * sizeof(model::Step)) != 0 &&
        editor.set_steps(op.pattern, op.track, op.first, buf, op.count) == model::Status::Ok) {
      changed = true;
      const std::uint16_t last_row = static_cast<std::uint16_t>(op.first + op.count - 1u);
      if (!extent_.valid) {  // the first written step is the first op's (the tools write in track order)
        extent_ = GroupExtent{true, op.pattern, op.track, op.track, op.first, last_row};
      } else if (extent_.pattern == op.pattern) {
        extent_.track0 = op.track < extent_.track0 ? op.track : extent_.track0;
        extent_.track1 = op.track > extent_.track1 ? op.track : extent_.track1;
        extent_.row0 = op.first < extent_.row0 ? op.first : extent_.row0;
        extent_.row1 = last_row > extent_.row1 ? last_row : extent_.row1;
      }
    }
  }
  return changed;
}

// ---- Pattern F7 TOOLS (OS-H3A) ----------------------------------------------------------------------------------------------------------------

bool ModuleUi::quarantined_for_newer_abi(const char* stem) const noexcept {
  for (unsigned i = 0; set_ != nullptr && i < set_->quarantine_count(); ++i) {
    if (std::strcmp(set_->quarantine_name(i), stem) == 0) {
      return set_->quarantine_reason(i) == module::Status::AbiMinor;
    }
  }
  return false;
}

std::uint32_t ModuleUi::app_caps(unsigned entry) const noexcept {
  if (entry >= app_count_ || set_ == nullptr) {
    return 0;
  }
  const char* stem = apps_[entry].stem;
  const int idx = set_->find(stem);
  if (idx >= 0) {  // loaded (this boot): the header the loader read
    const module::LoadedModule* lm = set_->at(static_cast<unsigned>(idx));
    return lm != nullptr && lm->header.kind == O3M_KIND_APP ? lm->header.reserved2[0] : 0u;
  }
  if (card_ == nullptr || !card_->mounted()) {
    return 0;
  }
  char path[24 + module::kStemMax + 8];
  std::size_t n = 0;
  for (const char* c = "modules/apps/"; *c != '\0'; ++c) {
    path[n++] = *c;
  }
  for (const char* c = stem; *c != '\0'; ++c) {
    path[n++] = *c;
  }
  for (const char* c = ".tmod"; *c != '\0'; ++c) {
    path[n++] = *c;
  }
  path[n] = '\0';
  hal::storage::FileHandle f;
  if (card_->open(path, f) != hal::storage::Status::Ok) {
    return 0;
  }
  o3m_file_header h{};
  std::size_t got = 0;
  const bool read_ok = card_->read(f, &h, sizeof h, got) == hal::storage::Status::Ok && got == sizeof h;
  card_->close(f);
  return read_ok && h.magic == O3M_FILE_MAGIC && h.header_size == O3M_FILE_HEADER_SIZE && h.kind == O3M_KIND_APP ? h.reserved2[0] : 0u;
}

void ModuleUi::scan_tools() noexcept {
  scan_apps();
  tool_count_ = 0;
  for (unsigned i = 0; i < app_count_; ++i) {
    if (apps_[i].state != CatalogEntry::State::Missing && (app_caps(i) & (O3M_CAP_EDIT_PROJECT | O3M_CAP_EXPORT_FILES)) != 0u) {  // OS-H4: an app that writes files is a tool too (SMF EXPORT gets the selection as its context)
      tools_[tool_count_++] = static_cast<std::uint8_t>(i);
    }
  }
}

bool ModuleUi::app_title(unsigned entry, char* out, unsigned cap) const noexcept {
  if (out == nullptr || cap == 0u || entry >= app_count_) {
    return false;
  }
  const char* name = apps_[entry].stem;
  const int idx = set_ != nullptr ? set_->find(name) : -1;
  const module::LoadedModule* lm = idx >= 0 ? set_->at(static_cast<unsigned>(idx)) : nullptr;
  if (lm != nullptr && lm->entry != nullptr && lm->entry->name != nullptr) {
    name = lm->entry->name;
  }
  unsigned n = 0;
  for (; name[n] != '\0' && n + 1 < cap; ++n) {
    const char c = name[n];
    out[n] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
  }
  out[n] = '\0';
  return true;
}

std::uint32_t ModuleUi::host_launch_ctx(o3m_launch_ctx* out) const noexcept {
  if (out == nullptr) {
    return 0;
  }
  const std::uint32_t room = out->size < sizeof(o3m_launch_ctx) ? out->size : static_cast<std::uint32_t>(sizeof(o3m_launch_ctx));
  if (room >= sizeof(std::uint32_t)) {
    std::memcpy(out, &launch_ctx_, room);
    out->size = room;
  }
  return (launch_ctx_.flags & O3M_LAUNCH_FROM_TOOLS) != 0u ? 1u : 0u;
}

}  // namespace o3os::app
