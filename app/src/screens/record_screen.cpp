// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/screens/record_screen.h>

#include <o3os/app/chrome.h>
#include <o3os/app/frame.h>
#include <o3os/app/pattern_layout.h>
#include <o3os/app/record_view.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>
#include <o3os/ui/plot/meters.h>
#include <o3os/ui/plot/wave_view.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;
using ui::Renderer;

namespace {

constexpr const char* kPageNames[3] = {"SOURCES", "RESAMPLE", "RADIO"};

RecMode mode_of(const ViewState& view) noexcept { return rec_mode_of_page(view.page[screen_index(ScreenId::Record)]); }
bool review_of(const Model& model) noexcept { return model.rec != nullptr && model.rec->state() == Recorder::State::Review; }
bool level_of(const Model& model) noexcept { return model.rec != nullptr && level_rows(model.rec->cfg); }  // OS-SREC-LEVEL: START ON = LEVEL (no LOOP): THRESHOLD and PRE-ROLL are rows
bool loop_review_of(const Model& model) noexcept { return model.rec != nullptr && model.rec->loop_review(); }  // OS-LOOPER: the review of a loop (LOOP VOL in place of the trim rows)

// A length of audio in seconds with milliseconds: "12.345 S" (the engine's rate).
template <std::size_t N>
void append_secs(core::FixedString<N>& s, std::uint32_t frames) noexcept {
  const std::uint32_t ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(frames) * 1000000u / hal::audio::sample_rate_mhz());
  append_uint(s, ms / 1000u);
  s.append(".");
  append_dec(s, ms % 1000u, 3);
  s.append(" S");
}

// The clock of a take: "12.3" seconds in tenths.
template <std::size_t N>
void append_tenths(core::FixedString<N>& s, std::uint32_t tenths) noexcept {
  append_uint(s, tenths / 10u);
  s.append(".");
  append_uint(s, tenths % 10u);
  s.append(" S");
}

const char* row_name(RecRow row) noexcept {
  switch (row) {
    case RecRow::Source: return "SOURCE";
    case RecRow::Gain: return "GAIN";
    case RecRow::Monitor: return "MONITOR";
    case RecRow::Align: return "START ON";
    case RecRow::StopAt: return "STOP AT";
    case RecRow::Tap: return "TAP";
    case RecRow::Freq: return "FREQUENCY";
    case RecRow::Region: return "REGION";
    case RecRow::TrimStart: return "TRIM START";
    case RecRow::TrimEnd: return "TRIM END";
    case RecRow::Loop: return "LOOP";
    case RecRow::LoopVol: return "LOOP VOL";
    case RecRow::Feedback: return "FEEDBACK";
    case RecRow::Threshold: return "THRESHOLD";
    case RecRow::PreRoll: return "PRE-ROLL";
  }
  return "";
}

// STOP AT's value: "MANUAL", "BAR", "4 BARS"; while the transport is stopped there is no timeline to wait for and it acts as MANUAL (recorder.h, RecStop): "BAR (MANUAL)".
template <std::size_t N>
void append_stop(core::FixedString<N>& s, const RecorderConfig& cfg, bool playing) noexcept {
  if (cfg.loop_bars != 0u) {  // OS-LOOPER: the loop's own N bars end the take (the row is dim: LOOP sets it)
    s.append("LOOP");
    return;
  }
  if (cfg.stop == RecStop::Bars) {
    append_uint(s, cfg.stop_bars);
    s.append(cfg.stop_bars == 1u ? " BAR" : " BARS");
  } else {
    s.append(kRecStopNames[static_cast<unsigned>(cfg.stop)]);
  }
  if (!playing && cfg.stop != RecStop::Manual) {
    s.append(" (MANUAL)");
  }
}

template <std::size_t N>
void row_value(core::FixedString<N>& s, const Recorder& rec, RecRow row, bool playing) noexcept {
  switch (row) {
    case RecRow::Source:
      s.append(source_info(rec.cfg.source).name);
      break;
    case RecRow::Gain:
      if (rec.has_gain()) {
        append_uint(s, rec.gain());
        s.append(" %");
      } else {
        s.append("--");
      }
      break;
    case RecRow::Monitor:
      s.append(rec.cfg.monitor ? "ON" : "OFF");
      break;
    case RecRow::Align:
      s.append(kRecAlignNames[static_cast<unsigned>(rec.cfg.loop_bars != 0u ? RecAlign::Bar : rec.cfg.align)]);  // (a loop starts on a bar)
      break;
    case RecRow::StopAt:
      append_stop(s, rec.cfg, playing);
      break;
    case RecRow::Loop:
      if (rec.cfg.loop_bars == 0u) {
        s.append("OFF");
      } else {
        append_uint(s, rec.cfg.loop_bars);
        s.append(rec.cfg.loop_bars == 1u ? " BAR" : " BARS");
      }
      break;
    case RecRow::LoopVol:
      append_uint(s, rec.cfg.loop_vol);
      break;
    case RecRow::Feedback:
      append_uint(s, rec.cfg.loop_fb);
      break;
    case RecRow::Threshold:
      s.append("-");  // (-60..-6 dB: always below full scale)
      append_uint(s, static_cast<unsigned>(rec.cfg.level_db < 0 ? -rec.cfg.level_db : rec.cfg.level_db));
      s.append(" DB");
      break;
    case RecRow::PreRoll:
      append_uint(s, kRecPreRollMs[rec.cfg.preroll < kRecPreRolls ? rec.cfg.preroll : kRecPreRollDefault]);
      s.append(" MS");
      break;
    case RecRow::Tap:
      s.append(kRecTapNames[static_cast<unsigned>(rec.cfg.tap)]);
      if (rec.cfg.tap == RecTapKind::Track) {
        s.append(" ");
        append_dec(s, rec.cfg.track + 1u, 2);
      }
      break;
    case RecRow::Freq:
      append_uint(s, rec.cfg.radio_freq / 100u);
      s.append(".");
      append_dec(s, rec.cfg.radio_freq % 100u, 2);
      s.append(" MHZ");
      break;
    case RecRow::Region:
      s.append(hal::capture::region_info(rec.cfg.region).name);
      break;
    case RecRow::TrimStart:
      append_secs(s, rec.trim_start());
      break;
    case RecRow::TrimEnd:
      append_secs(s, rec.trim_end());
      break;
  }
}

const char* radio_state_text(hal::capture::RadioStatus::State s) noexcept {
  using St = hal::capture::RadioStatus::State;
  switch (s) {
    case St::Off: return "OFF";
    case St::Starting: return "STARTING";
    case St::Ready: return "LISTENING";
    case St::Tuning: return "TUNING";
    case St::Seeking: return "SEEKING";
    case St::Absent: return "NO RADIO";
    case St::Failed: return "FAILED";
  }
  return "";
}

const char* note_text(Recorder::Note n) noexcept {
  switch (n) {
    case Recorder::Note::None: return "";
    case Recorder::Note::Full: return "THE SAMPLE MEMORY FILLED: THE TAKE ENDED";
    case Recorder::Note::Lost: return "TAKE LOST: A PROJECT WAS LOADED UNDER IT";
    case Recorder::Note::CodecFailed: return "THE CODEC DID NOT ANSWER (TRYING AGAIN)";
    case Recorder::Note::NoRoom: return "NO ROOM IN THE SAMPLE MEMORY";
    case Recorder::Note::Settling: return "THE SOURCE IS SETTLING";
    case Recorder::Note::LoopCut: return "THE LOOP TAKE ENDED BEFORE ITS LAST BAR: GIVEN UP";
  }
  return "";
}

}  // namespace

int RecordScreen::count(const Model& model, const ViewState& view) const noexcept {
  return static_cast<int>(rec_rows(mode_of(view), review_of(model), loop_review_of(model), level_of(model)).n);
}

int RecordScreen::position(const ViewState& view) const noexcept {
  // (the cursor is clamped on use: a take under review has two rows more than one that is not)
  return view.record.row[static_cast<unsigned>(mode_of(view))];
}

bool RecordScreen::step(const Model& model, ViewState& view, int dx, int dy) const noexcept {
  (void)dx;  // OS-CFG-ARROWS: Left / Right edit the row (record.edit); the pages are the screen button
  return dy != 0 && set_position(model, view, position(view) + dy);
}

bool RecordScreen::set_position(const Model& model, ViewState& view, int position) const noexcept {
  const int last = static_cast<int>(rec_rows(mode_of(view), review_of(model), loop_review_of(model), level_of(model)).n) - 1;
  const int to = position < 0 ? 0 : (position > last ? last : position);
  std::uint8_t& row = view.record.row[static_cast<unsigned>(mode_of(view))];
  const bool changed = to != static_cast<int>(row);
  row = static_cast<std::uint8_t>(to);
  return changed;
}

bool RecordScreen::pad_roles(const Model& model, const ViewState&, PadRole (&roles)[48]) const noexcept {
  if (!review_of(model) || model.rec->trim_end() <= model.rec->trim_start()) {
    return false;
  }
  for (unsigned n = 0; n < 48u; ++n) {
    roles[n] = n == kRecRootPad ? PadRole::Front : PadRole::Back;
  }
  return true;
}

void RecordScreen::render(const Model& model, const ViewState& view, Renderer& r, Rect covered) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Frame f = make_frame(t, kFrameRows);
  const PatternLayout grid = make_pattern_layout(t, kFrameRows, PatternView::kVisibleTracks);
  const Recorder* rec = model.rec;
  const RecMode mode = mode_of(view);
  const unsigned page = static_cast<unsigned>(mode);
  const bool review = review_of(model);
  const bool looped = loop_review_of(model);
  const bool level = level_of(model);
  const RecRows rows = rec_rows(mode, review, looped, level);
  const unsigned cursor = rec_cursor(view.record, mode, review, looped, level);
  const int cell_h = t.geometry(Face::Data).cell_h;
  const int cell_w = t.geometry(Face::Data).cell_w;
  const int name_x = m.index_w + 2 * m.gap;
  const int value_x = name_x + 12 * cell_w;
  const int card_x = value_x + 16 * cell_w + 3 * m.gap;
  const auto hidden = [&](const Rect& rect) { return covers(covered, rect.x, rect.y, rect.w, rect.h); };
  const auto row_y = [&](unsigned i) { return f.pane.y + static_cast<int>(i) * f.row_pitch; };
  const auto cell_y = [&](unsigned i) { return row_y(i) + grid.cell_dy; };
  const Recorder::State state = rec != nullptr ? rec->state() : Recorder::State::Idle;
  const bool live = state == Recorder::State::Armed || state == Recorder::State::Recording;
  const bool listening = state == Recorder::State::Armed && rec->wait_align() == audio::RecTap::Align::Level;  // OS-SREC-LEVEL: armed, listening for the THRESHOLD (rec is not null while a take is armed)
  const Rect lane = {m.pad + 4, row_y(8) + m.pad, ui::kScreenW - 2 * (m.pad + 4), 7 * f.row_pitch - 2 * m.pad};

  draw_chrome_bands(r, f, covered);

  if (!hidden(f.strip)) {
    StatusStrip strip(r, f);
    strip.row().put(Face::Display, "RECORD", Color::ChromeText);
    strip.divider();
    strip.transport(view.playing);
    strip.divider();
    const char* badge = state == Recorder::State::Recording ? "REC" : (state == Recorder::State::Armed ? "ARMED" : (state == Recorder::State::Review ? "TAKE" : "READY"));
    if (state == Recorder::State::Recording) {
      strip.row().field("STATE", Face::Data, badge, Color::ChromeDim, Color::Accent, Color::AccentText);  // (REC on a chip: the orange is not 3:1 on Chrome)
    } else {
      strip.row().field("STATE", Face::Data, badge, Color::ChromeDim, Color::ChromeText);
    }
    strip.cpu(model.load);
  }
  if (!hidden(f.header)) {
    const int cap_y = ui::caps_y(t, Face::Label, f.header.y, f.header.h);
    int x = name_x;
    for (unsigned p = 0; p < 3; ++p) {
      r.text(x, cap_y, kPageNames[p], p == page ? Color::ChromeText : Color::ChromeDim, Face::Label);
      if (p == page) {
        r.panel({x, f.header.y + f.header.h - 2 * m.border, r.text_width(kPageNames[p], Face::Label), 2 * m.border}, Color::Sel);
      }
      x += r.text_width(kPageNames[p], Face::Label) + 4 * m.gap;
    }
    // The header names the state (UI map rule 7). OS-LOOPER: a loop take and the review of a loop say which loop, and whether the tempo moved since (the loop keeps its length in samples).
    core::FixedString<96> head;
    const Recorder::State hs = rec != nullptr ? rec->state() : Recorder::State::Idle;
    if (rec != nullptr && (rec->loop_review() || (rec->loop_take() && (hs == Recorder::State::Armed || hs == Recorder::State::Recording)))) {
      head.assign(hs == Recorder::State::Review ? "REVIEW - LOOP " : (hs == Recorder::State::Armed ? "ARMED - LOOP " : "RECORDING - LOOP "));
      append_uint(head, rec->loop_bars());
      head.append(rec->loop_bars() == 1u ? " BAR" : " BARS");
      if (hs == Recorder::State::Review && rec->loop_muted()) {
        head.append(" - MUTED");
      }
      if (rec->restoring()) {  // OS-LOOPER L2c: the session and the restore are named in the header (UI map rule 7)
        head.append(" - UNDO");
      } else if (rec->overdubbing()) {
        head.append(" - OVERDUB");
      }
      if (hs == Recorder::State::Review && rec->loop_tempo() != 0u && view.play.playing && view.play.tempo_x100 != 0u && view.play.tempo_x100 != rec->loop_tempo()) {
        head.append(" - TEMPO CHANGED");
      }
    } else {
      head.assign("input heard only with MONITOR on");
    }
    r.text(ui::kScreenW - 2 * m.pad - r.text_width(head.c_str(), Face::Label), cap_y, head.c_str(), looped || (rec != nullptr && rec->loop_take()) ? Color::ChromeText : Color::ChromeDim, Face::Label);
  }

  if (rec == nullptr) {
    const char* none = "NO RECORDER IN THIS BUILD";
    if (!hidden({name_x, cell_y(0), 30 * cell_w, cell_h})) {
      r.text(name_x, cell_y(0), none, Color::TextDim);
    }
  } else {
    // The rows: a name and its value; the selected row is the one strong highlight.
    const Rect bar = {0, row_y(cursor), card_x - 2 * m.gap, f.row_pitch};
    if (!hidden(bar)) {
      r.highlight_row(bar);
    }
    core::FixedString<24> text;
    for (unsigned i = 0; i < rows.n; ++i) {
      if (hidden({name_x, cell_y(i), 30 * cell_w, cell_h})) {
        continue;
      }
      const bool sel = i == cursor;
      // While a take is armed or recording only the gain and the monitor can change: the other rows are dim.
      // (OS-LOOPER: with LOOP on, ALIGN and STOP AT are the loop's: dim, whatever the take is doing)
      const bool loop_rows = rec->cfg.loop_bars != 0u && (rows.row[i] == RecRow::Align || rows.row[i] == RecRow::StopAt);
      const bool source_rows = looped && (rows.row[i] == RecRow::Source || rows.row[i] == RecRow::Tap || rows.row[i] == RecRow::Freq || rows.row[i] == RecRow::Region);  // (the loop's source is locked while it exists)
      const bool frozen = loop_rows || source_rows || (live && rows.row[i] != RecRow::Gain && rows.row[i] != RecRow::Monitor && rows.row[i] != RecRow::TrimStart && rows.row[i] != RecRow::TrimEnd &&
                                                       rows.row[i] != RecRow::LoopVol && rows.row[i] != RecRow::Feedback);
      text.clear();
      row_value(text, *rec, rows.row[i], view.play.playing);
      r.text(name_x, cell_y(i), row_name(rows.row[i]), sel ? Color::SelText : Color::TextDim);
      r.text(value_x, cell_y(i), text.c_str(), sel ? Color::SelText : (frozen ? Color::TextDim : Color::Text));
    }

    // The card: the clock, the state in words, the input meters and, on the RADIO page, the chip.
    if (card_x + 20 * cell_w <= ui::kScreenW && !hidden({card_x, row_y(0), ui::kScreenW - card_x, 7 * f.row_pitch})) {
      core::FixedString<32> line;
      const int gap = f.row_pitch / 3;  // the clock's Display face is taller than a row: the lines under it are a little lower
      append_tenths(line, Recorder::tenths(rec->frames()));
      r.text(card_x, cell_y(0), line.c_str(), state == Recorder::State::Recording ? Color::Accent : Color::Text, Face::Display);
      const char* what = "NO TAKE";
      switch (state) {
        case Recorder::State::Idle: what = rec->settling() ? "SETTLING" : "READY"; break;
        case Recorder::State::Armed: what = listening ? "WAITING FOR LEVEL" : "WAITING FOR THE START"; break;
        case Recorder::State::Recording: what = rec->stop_pending() ? "STOPPING AT THE BOUNDARY" : "RECORDING"; break;
        case Recorder::State::Review: what = looped ? (rec->restoring() ? "UNDOING THE LAYER" : (rec->overdubbing() ? "OVERDUB: LAYERING" : "LOOP: PLAYING IN TIME")) : "TAKE: TRIM, THEN SAVE"; break;
      }
      r.text(card_x, cell_y(1) + gap, what, Color::Text, Face::Label);
      line.clear();
      if (state == Recorder::State::Review && looped) {
        line.assign(rec->loop_muted() ? "LOOP MUTED, STILL IN TIME" : "LOOP ON WITH THE TRANSPORT");
      } else if (state == Recorder::State::Review) {
        line.assign("WINDOW ");
        append_secs(line, rec->trim_end() - rec->trim_start());
      } else if (live || state == Recorder::State::Idle) {
        line.assign("START ON ");
        line.append(kRecAlignNames[static_cast<unsigned>(rec->cfg.loop_bars != 0u ? RecAlign::Bar : rec->cfg.align)]);
        if (!view.play.playing && rec->cfg.align != RecAlign::Free && rec->cfg.align != RecAlign::Level && rec->cfg.loop_bars == 0u) {  // (LEVEL needs no timeline: it stays LEVEL while stopped)
          line.append(" (STOPPED: FREE)");
        }
      }
      r.text(card_x, cell_y(2) + gap, line.c_str(), Color::TextDim, Face::Label);
      std::uint32_t room = 0;
      if (mode != RecMode::Radio && record_room_frames(rec, model.samples, room)) {  // (the RADIO page's lines 3 and 4 are the chip's)
        line.assign("TIME LEFT ");
        append_tenths(line, Recorder::tenths(room));
        r.text(card_x, cell_y(3) + gap, line.c_str(), Color::TextDim, Face::Label);
      }
      if (mode == RecMode::Radio) {
        const hal::capture::RadioStatus& rs = rec->radio();
        const bool bad = rs.state == hal::capture::RadioStatus::State::Absent || rs.state == hal::capture::RadioStatus::State::Failed;
        line.clear();
        line.assign("RADIO ");
        line.append(radio_state_text(rs.state));
        if (bad && rs.fail[0] != '\0') {
          line.append(" ");
          line.append(rs.fail);  // the step and the cause of the failure ("OW/N"), for the maintainer's report (si4703.h, `diag radio`)
        }
        r.text(card_x, cell_y(3) + gap, line.c_str(), bad ? Color::Accent : Color::Text, Face::Label);
        line.clear();
        if (rs.state == hal::capture::RadioStatus::State::Ready) {
          line.assign("SIGNAL ");
          append_uint(line, rs.rssi);
          line.append(rs.stereo ? "  STEREO" : "");
          line.append(rs.band_limit ? "  BAND LIMIT" : "");
        }
        r.text(card_x, cell_y(4) + gap, line.c_str(), Color::TextDim, Face::Label);
      }
      // The meters: what is being recorded, two bars. The ballistics are the mixer board's (the App feeds them while this screen is in view).
      if (model.mix != nullptr) {
        const unsigned left = rec->meter_channel();
        ui::Rect rects[2];
        ui::plot::MeterState states[2];
        const int mw = ui::kScreenW - card_x - 2 * m.pad - 3 * cell_w;
        for (unsigned k = 0; k < 2; ++k) {
          rects[k] = {card_x + 2 * cell_w, row_y(5 + k) + 4, mw, f.row_pitch - 8};
          states[k] = model.mix->meter[left + k];
        }
        ui::plot::MeterStyle style;
        style.slot = Color::Empty;
        ui::plot::draw_meters(r, rects, states, 2, ui::Axis::Horizontal, model.mix->scale, style);
        r.text(card_x, cell_y(5), "L", Color::TextDim, Face::Label);
        r.text(card_x, cell_y(6), "R", Color::TextDim, Face::Label);
      }
    }
  }

  // The lane: the take's overview, and its trim window under review.
  if (!hidden(lane)) {
    r.frame({lane.x - 1, lane.y - 1, lane.w + 2, lane.h + 2}, Color::TextDim);
    r.panel(lane, Color::Pane);
    const bool have = rec != nullptr && rec->view_cache().valid() && rec->view_cache().built();
    if (!have) {
      const char* none = state == Recorder::State::Armed ? (listening ? "WAITING FOR LEVEL" : "WAITING FOR THE START") : "NO TAKE";
      r.text(lane.x + (lane.w - r.text_width(none, Face::Data)) / 2, lane.y + lane.h / 2 - 10, none, Color::TextDim, Face::Data);
    } else {
      const TakeOverview& ov = rec->overview();
      ui::plot::WaveMarks marks;
      marks.zero_line = true;
      if (review) {
        marks.has_selection = true;
        marks.sel_begin = ov.env_frame(rec->trim_start());
        marks.sel_end = ov.env_frame(rec->trim_end());
        if (model.head.on && model.head.slot == audio::kPreviewSlot) {  // the take's Preview (OS-FU3)
          marks.has_playhead = true;
          marks.playhead = ov.env_frame(model.head.frame);
        }
      }
      (void)ui::plot::draw_wave(r, lane, rec->view_cache(), 0, ui::plot::WaveWindow{0, ov.samples() != 0u ? ov.samples() : 1u}, ui::plot::WaveMode::Vector, nullptr, 0, 0, marks,
                                ui::plot::WaveStyle{});
    }
  }

  if (!hidden(f.info)) {
    const char* left = rec == nullptr ? "no recorder" : note_text(rec->note());
    if (rec != nullptr && left[0] == '\0') {
      switch (state) {
        case Recorder::State::Idle: left = rec->cfg.loop_bars != 0u ? "F1 records the loop on the bars" : "F1 records; START ON says where it starts"; break;
        case Recorder::State::Armed: left = listening ? "listening for the level; F1 cancels" : "armed: Play starts it on the step; F1 stops"; break;
        case Recorder::State::Recording:
          left = rec->stop_pending() ? "waiting for the STOP AT boundary; F1 again stops now" : (rec->ends_by_itself() ? "recording: ends by itself; F1 stops now" : "recording: F1 stops; gain and monitor move");
          break;
        case Recorder::State::Review: left = looped ? "F1: overdub  F5: undo  F3: save  F4: discard" : "F3: save to a free instrument  F4: discard"; break;
      }
    }
    draw_info_line(r, f, left, "up/down: row  encoder: change  Delete: default");
  }
  if (!hidden(f.keys) && !view.overlay_keys()) {
    // OS-LOOPER: the key row of a loop under review swaps as a whole (F1 is the overdub, in force while a session is on; F5 undoes the layer, dim while there is none; F8 is the loop's switch, named by its state).
    const bool shown_loop = looped && !(view.shift && follows_shift(*this, view));
    const std::uint32_t dim = shown_loop && !rec->layer() ? 16u : 0u;
    const std::uint32_t hot = shown_loop && rec->overdubbing() ? 1u : 0u;
    ui::soft_keys(r, f.keys, key_row(*this, view, looped ? (rec->loop_muted() ? kRecordLoopMutedKeys : kRecordLoopKeys) : kRecordSoftKeys), 8, dim, hot);
  }
}

bool record_room_frames(const Recorder* rec, const SampleBank* bank, std::uint32_t& frames) noexcept {
  if (rec == nullptr || rec->cfg.loop_bars != 0u || rec->loop_review()) {
    return false;
  }
  switch (rec->state()) {
    case Recorder::State::Idle:
      if (bank == nullptr || !bank->ready()) {
        return false;
      }
      frames = bank->free_samples();  // a plain take reserves all of the free arena (SampleBank::take_begin)
      return true;
    case Recorder::State::Armed:
    case Recorder::State::Recording: frames = rec->capacity() > rec->frames() ? rec->capacity() - rec->frames() : 0u; return true;
    case Recorder::State::Review: break;
  }
  return false;
}

Context RecordScreen::field_context(const Model&, const ViewState& view) const noexcept {
  return mode_of(view) == RecMode::Radio ? Context::RecordRadio : Context::None;
}

const char* const* RecordScreen::shift_row(const ViewState& view) const noexcept {
  return mode_of(view) == RecMode::Radio ? kRecordRadioShiftKeys : kRecordShiftKeys;
}

constexpr RecordScreen kRecordScreen{};

}  // namespace o3os::app
