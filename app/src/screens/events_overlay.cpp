// SPDX-License-Identifier: GPL-3.0-or-later
// The EVENTS overlay of the Pattern screen (OS-MIDIREC S1b/S1c, docs/specs/OS-MIDIREC.md sections 3.1-3.4, 3.7, 3.8): the TOOLS
// entry EVENTS opens it over the grid pane — the cover rectangle is the track-header band plus the 16 grid rows, so the status
// strip, the info line and the key row stay (3.1) and the base screen draws nothing inside the cover (F11). The rows are
// projected from the steps on this one redraw through model::events::window() from the view's `top` (A15: one projection, at
// most 6,144 cells, about 0.3 ms; the cursor and the window are positions in the projected list, so no key<->index scan runs
// per frame). S1c adds the play marker (3.7: a 4 px Accent bar in the left margin on each row of a track's playing step, the
// song step modulo the track's length, when the pattern on show is the one that plays) and F1 FILTER cycles the list
// (events_actions.cpp). F2 QUANT (S3) and the edit keys (S2b, S2c) are the actions of events_actions.cpp.
#include <o3os/app/events_view.h>
#include <o3os/app/frame.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/app/view_state.h>
#include <o3os/core/fixed_string.h>
#include <o3os/ui/components.h>
#include <o3os/model/event_list.h>
#include <o3os/model/project.h>

namespace o3os::app {

using ui::Color;
using ui::Face;
using ui::Rect;

namespace {

namespace ev = model::events;

// Column x offsets, in Data-face cells from the row's left edge: TIME TRK TYPE CH DATA VAL LEN IN FL FX1 FX2 (spec 3.3, OS-EVFX).
constexpr int kColTime = 0;
constexpr int kColTrk = 7;
constexpr int kColSrc = 9;  // OS-MIDIREC-P1 S6: the SRC column, "L" on a lane row and blank on a step row (between TRK and TYPE); no caption of its own (a letter beside TYPE read as one word)
constexpr int kColType = 10;
constexpr int kColCh = 15;
constexpr int kColData = 18;
constexpr int kColVal = 23;
constexpr int kColLen = 27;
constexpr int kColIn = 35;
constexpr int kColFl = 38;
constexpr int kColFx1 = 41;
constexpr int kColFx2 = 45;
// The width a field's box takes, in the same cells.
constexpr int kFieldW[9] = {6, 4, 2, 4, 3, 6, 2, 3, 3};  // by EventsView::Field: Time, Type, Ch, Data, Val, Len, In, Fx1, Fx2
constexpr int kFieldX[9] = {kColTime, kColType, kColCh, kColData, kColVal, kColLen, kColIn, kColFx1, kColFx2};

const model::Pattern* events_pattern(const Model& model, const ViewState& view) noexcept {
  return model.project != nullptr ? model.project->pattern(view.edit.pattern) : nullptr;
}

ev::Env events_env(const Model& model) noexcept {
  if (model.project == nullptr) {
    return ev::Env{};
  }
  ev::Env env = ev::env_of(*model.project);
  env.lanes = model.lanes;  // S6: the lane rows
  return env;
}

const char* end_text(ev::Kind k) noexcept {
  return k == ev::Kind::Off ? "OFF" : (k == ev::Kind::Cut ? "CUT" : "FADE");
}

// "NOTE", "OFF", ... or "CC A".."CC F".
void type_text(core::FixedString<12>& out, const ev::Row& row) noexcept {
  if (row.key.lane != 0u) {  // S6: a lane event's own kind (model::LaneEvent: note, CC, program, channel AT, poly AT, pitch bend)
    static const char* const kNames[6] = {"NOTE", "CC", "PROG", "CHAT", "PAT", "BEND"};
    out.assign(kNames[row.lane_kind < 6u ? row.lane_kind : 1u]);
  } else if (row.key.kind == ev::Kind::Cc) {
    char t[5] = {'C', 'C', ' ', static_cast<char>('A' + row.cc_letter), '\0'};
    out.assign(t);
  } else if (row.key.kind == ev::Kind::Note) {
    out.assign("NOTE");
  } else {
    out.assign(end_text(row.key.kind));
  }
}

// "#074", "PROG", "CHAT", "PAT", "--" (F3).
void cc_data_text(core::FixedString<12>& out, const ev::Row& row) noexcept {
  switch (row.cc.what) {
    case ev::CcOut::What::Controller:
      out.assign("#");
      (void)append_dec(out, row.cc.number, 3);
      break;
    case ev::CcOut::What::Program: out.assign("PROG"); break;
    case ev::CcOut::What::ChannelAt: out.assign("CHAT"); break;
    case ev::CcOut::What::PolyAt: out.assign("PAT"); break;
    case ev::CcOut::What::Unassigned: out.assign("--"); break;
  }
}

// "001.00" (A1): the step 1-based, three digits, and the percent of a step, two digits.
void time_text(core::FixedString<12>& out, const ev::Row& row) noexcept {
  out.clear();
  (void)append_dec(out, row.key.step + 1u, 3);
  out.append(".");
  (void)append_dec(out, row.percent, 2);
}

// "04.00", ">52.00" (A5).
void len_text(core::FixedString<12>& out, const ev::Row& row) noexcept {
  out.clear();
  if (row.len_runs_on) {
    out.append(">");
  }
  (void)append_dec(out, row.len_steps, 2);
  out.append(".");
  (void)append_dec(out, row.len_percent, 2);
}

}  // namespace

// The cover: the header band and the grid pane, full width, from under the strip to the info line. The strip, the info line
// and the key row stay (3.1).
Rect EventsOverlay::rect(const ui::Theme& theme) const noexcept {
  const ui::Metrics& m = theme.metrics;
  return Rect{0, static_cast<int>(m.strip_h), ui::kScreenW, static_cast<int>(m.header_h) + kFrameRows * m.row_pitch};
}

void EventsOverlay::render(const Model& model, const ViewState& view, ui::Renderer& r) const noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const Rect cover = rect(t);
  r.panel(cover, Color::Pane);  // (the base screen drew nothing wholly inside the cover)
  const int pitch = m.row_pitch;
  const int x0 = 2 * m.pad;
  const int cell = t.geometry(Face::Data).cell_w;
  const int y0 = cover.y;
  const bool multi = view.events.scope.multi_track();

  const model::Pattern* pat = events_pattern(model, view);
  const ev::Env env = events_env(model);
  ev::Row rows[kEventsRows];
  std::uint32_t total = 0;
  const std::uint32_t n = pat != nullptr ? ev::window(*pat, env, view.events.scope, view.events.filter, view.events.top, rows, kEventsRows, &total) : 0u;

  // Row 0: the scope header — the pattern, the track or tracks, the rows, the count under the filter, the filter, the position.
  {
    core::FixedString<96> line;
    line.assign("EVENTS  PATTERN ");
    (void)append_dec(line, view.edit.pattern, 2);
    if (multi) {
      line.append("  TRACKS ");
      (void)append_dec(line, view.events.scope.track0 + 1u, 2);
      line.append("-");
      (void)append_dec(line, view.events.scope.track1 + 1u, 2);
    } else {
      line.append("  TRACK ");
      (void)append_dec(line, view.events.scope.track0 + 1u, 2);
    }
    line.append("  STEPS ");
    (void)append_dec(line, view.events.scope.row0 + 1u, 3);
    line.append("-");
    (void)append_dec(line, view.events.scope.row1 + 1u, 3);
    line.append("   ");
    (void)append_uint(line, total);
    line.append(" EVENTS  ");
    line.append(view.events.filter == ev::Filter::All ? "ALL" : (view.events.filter == ev::Filter::Notes ? "NOTES" : "CC"));
    const int y = ui::caps_y(t, Face::Data, y0, pitch);
    r.text(x0, y, line.c_str(), Color::Text, Face::Data);
    core::FixedString<8> pos;
    (void)append_uint(pos, view.events.cursor < total ? view.events.cursor + 1u : 0u);
    pos.append("/");
    (void)append_uint(pos, total);
    r.text(cover.x + cover.w - 2 * m.pad - r.text_width(pos.c_str(), Face::Data), y, pos.c_str(), Color::Text, Face::Data);
  }

  // Row 1: the column captions.
  {
    const int y = ui::caps_y(t, Face::Data, y0 + pitch, pitch);
    const Color dim = Color::TextDim;
    r.text(x0 + kColTime * cell, y, "TIME", dim, Face::Data);
    if (multi) {
      r.text(x0 + kColTrk * cell, y, "TRK", dim, Face::Data);
    }
    r.text(x0 + kColType * cell, y, "TYPE", dim, Face::Data);
    r.text(x0 + kColCh * cell, y, "CH", dim, Face::Data);
    r.text(x0 + kColData * cell, y, "DATA", dim, Face::Data);
    r.text(x0 + kColVal * cell, y, "VAL", dim, Face::Data);
    r.text(x0 + kColLen * cell, y, "LEN", dim, Face::Data);
    r.text(x0 + kColIn * cell, y, "IN", dim, Face::Data);
    r.text(x0 + kColFl * cell, y, "FL", dim, Face::Data);
    r.text(x0 + kColFx1 * cell, y, "FX1", dim, Face::Data);
    r.text(x0 + kColFx2 * cell, y, "FX2", dim, Face::Data);
  }

  if (n == 0u) {
    const int y = ui::caps_y(t, Face::Data, y0 + 2 * pitch, pitch);
    r.text(x0, y, "NO EVENTS: INSERT ADDS ONE", Color::TextDim, Face::Data);
    return;
  }

  const int octave_shift = pattern_octave_shift(model);
  const auto field_of = static_cast<EventsView::Field>(view.events.field);
  // The play marker (3.7): the step the sequencer is on in the pattern on show, per track modulo that track's length (R5); -1
  // when the pattern on show is not the one that plays. The list redraws when that step changes (app.cpp), not every microstep.
  const int play = play_row(view.play, view.edit.pattern);
  for (std::uint32_t i = 0; i < n; ++i) {
    const ev::Row& row = rows[i];
    const int ry = y0 + (2 + static_cast<int>(i)) * pitch;
    const bool cursor = view.events.top + i == view.events.cursor;
    const Color ink = !row.midi ? Color::TextDim : (cursor ? Color::SelText : Color::Text);
    if (cursor) {
      r.panel(Rect{cover.x, ry, cover.w, pitch}, Color::Sel);
      // The cursor's field is an Accent block under its own words (3.3: the TIME field boxed), drawn before them.
      if (events_field_ok(row, field_of)) {
        const unsigned fi = static_cast<unsigned>(field_of);
        const int fw = row.key.lane != 0u && row.lane_kind == ev::kLaneBend && field_of == EventsView::Field::Val ? 5 : kFieldW[fi];  // a bend's 14 bits take five digits
        r.panel(Rect{x0 + kFieldX[fi] * cell - m.pad / 2, ry + m.border, fw * cell + m.pad, pitch - 2 * m.border}, Color::Accent);
      }
    }
    if (play >= 0 && pat != nullptr) {  // the Accent bar in the left margin, over the cursor's row too
      const std::uint32_t len = pat->track_length[row.key.track];
      if (len > 0u && static_cast<int>(static_cast<unsigned>(play) % len) == row.key.step) {
        r.panel(Rect{cover.x, ry, 4, pitch}, Color::Accent);
      }
    }
    const int y = ui::caps_y(t, Face::Data, ry, pitch);
    const auto cx = [&](int col) { return x0 + col * cell; };
    // The boxed field's words are knocked out of the Accent block.
    const auto field_ink = [&](EventsView::Field f) { return cursor && f == field_of ? Color::AccentText : ink; };

    core::FixedString<12> tmp;
    time_text(tmp, row);
    r.text(cx(kColTime), y, tmp.c_str(), field_ink(EventsView::Field::Time), Face::Data);
    if (multi) {
      core::FixedString<4> trk;
      (void)append_dec(trk, row.key.track + 1u, 2);
      r.text(cx(kColTrk), y, trk.c_str(), ink, Face::Data);
    }
    type_text(tmp, row);
    r.text(cx(kColType), y, tmp.c_str(), field_ink(EventsView::Field::Type), Face::Data);
    core::FixedString<4> ch;
    if (row.channel == 0u) {
      ch.assign("--");
    } else {
      (void)append_dec(ch, row.channel, 2);
    }
    r.text(cx(kColCh), y, ch.c_str(), field_ink(EventsView::Field::Ch), Face::Data);
    if (row.key.lane != 0u) {
      // S6: a lane row — SRC "L", DATA and VAL by kind (the bytes of the model::LaneEvent), the true LEN of a note; no IN, FL or FX
      r.text(cx(kColSrc), y, "L", ink, Face::Data);
      const std::uint8_t k = row.lane_kind;
      if (k == ev::kLaneNote || k == ev::kLanePolyAt) {
        r.note_tile(cx(kColData), ry + (pitch - t.geometry(Face::Data).cell_h) / 2, note_tile_cell(static_cast<std::uint8_t>(row.lane_a + 1u), octave_shift), field_ink(EventsView::Field::Data), Face::Data);
      } else if (k == ev::kLaneCc || k == ev::kLaneProgram) {
        tmp.assign("#");
        (void)append_dec(tmp, row.lane_a, 3);
        r.text(cx(kColData), y, tmp.c_str(), field_ink(EventsView::Field::Data), Face::Data);
      } else {
        r.text(cx(kColData), y, "-", ink, Face::Data);
      }
      if (k == ev::kLaneProgram) {
        r.text(cx(kColVal), y, "-", ink, Face::Data);
      } else {
        tmp.clear();
        if (k == ev::kLaneBend) {
          (void)append_dec(tmp, (static_cast<unsigned>(row.lane_b) << 7) | row.lane_a, 5);
        } else {
          (void)append_dec(tmp, k == ev::kLaneChanAt ? row.lane_a : row.lane_b, 3);
        }
        r.text(cx(kColVal), y, tmp.c_str(), field_ink(EventsView::Field::Val), Face::Data);
      }
      if (k == ev::kLaneNote) {
        len_text(tmp, row);
        r.text(cx(kColLen), y, tmp.c_str(), field_ink(EventsView::Field::Len), Face::Data);
      }
      r.text(cx(kColIn), y, "-", ink, Face::Data);
      continue;
    }
    if (row.key.kind == ev::Kind::Note) {
      const std::uint8_t note = pat != nullptr ? pat->steps[row.key.track][row.key.step].note : 0u;
      r.note_tile(cx(kColData), ry + (pitch - t.geometry(Face::Data).cell_h) / 2, note_tile_cell(note, octave_shift), field_ink(EventsView::Field::Data), Face::Data);
    } else if (row.key.kind == ev::Kind::Cc) {
      cc_data_text(tmp, row);
      r.text(cx(kColData), y, tmp.c_str(), field_ink(EventsView::Field::Data), Face::Data);
    } else {
      r.text(cx(kColData), y, "-", ink, Face::Data);
    }
    if (row.key.kind == ev::Kind::Note || row.key.kind == ev::Kind::Cc) {
      core::FixedString<5> val;
      if (row.key.kind == ev::Kind::Note && row.vol == model::kVolumeUnset) {
        val.assign("---");
      } else {
        (void)append_dec(val, row.vol, 3);
      }
      r.text(cx(kColVal), y, val.c_str(), field_ink(EventsView::Field::Val), Face::Data);
    } else {
      r.text(cx(kColVal), y, "-", ink, Face::Data);
    }
    if (row.key.kind == ev::Kind::Note) {
      len_text(tmp, row);
      r.text(cx(kColLen), y, tmp.c_str(), field_ink(EventsView::Field::Len), Face::Data);
    }
    {
      core::FixedString<4> in;
      if (row.instr == 0u) {
        in.assign("??");  // unknown at the top of the track (A8)
      } else {
        (void)append_dec(in, row.instr, 2);
      }
      r.text(cx(kColIn), y, in.c_str(), row.inherited && !(cursor && field_of == EventsView::Field::In) ? Color::TextDim : field_ink(EventsView::Field::In), Face::Data);
    }
    if (row.key.kind == ev::Kind::Note && pat != nullptr) {
      if (row.gated) {
        r.text(cx(kColFl), y, "G", ink, Face::Data);
      }
      // FX1 and FX2 (OS-EVFX): the grid's own words, glyph and hex value ("V40", "---" when empty).
      const Step& st = pat->steps[row.key.track][row.key.step];
      char fx[4];
      (void)format_field(st, Column::Fx1, fx);
      r.text(cx(kColFx1), y, fx, field_ink(EventsView::Field::Fx1), Face::Data);
      (void)format_field(st, Column::Fx2, fx);
      r.text(cx(kColFx2), y, fx, field_ink(EventsView::Field::Fx2), Face::Data);
    }
  }
}

constexpr EventsOverlay kEventsOverlay{};

}  // namespace o3os::app
