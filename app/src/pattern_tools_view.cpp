// SPDX-License-Identifier: GPL-3.0-or-later
// The TOOLS dialog of the Pattern screen (OS-PTOOLS2 S2): the row model of song_tools_view.cpp over the Pattern verbs — the OP row (TRANSPOSE, INSTRUMENT, CLEAR) and the OP's
// value row (AMOUNT -24..+24 st with an octave as the coarse step, TO 1..64 instruments with 8 as the coarse step, WHAT for CLEAR) — and the footer's two lines: the target
// (the selection's block, or the cursor step) and its counts (steps and notes). Session state only (Q6): nothing here is saved.
#include <o3os/app/pattern_tools_view.h>

#include <o3os/app/pattern_data.h>
#include <o3os/app/text_format.h>
#include <o3os/model/pattern.h>
#include <o3os/model/project.h>
#include <o3os/model/step_edit.h>

namespace o3os::app {

namespace {

constexpr const char* kToolNames[3] = {"TRANSPOSE", "INSTRUMENT", "CLEAR"};
constexpr const char* kWhatNames[5] = {"NOTES", "VOLUME", "FX 1", "FX 2", "ALL"};
constexpr const char* kColumnNames[5] = {"NOTE", "INSTR", "VOL", "FX 1", "FX 2"};

using core::ParamCurve;
using core::ParamDesc;
using core::ParamUnit;

// name, unit text, names, knots, min, max, def, detents, coarse, list_count, unit, curve, flags, group
constexpr ParamDesc kOp = {"OP", "", kToolNames, nullptr, 0.0f, 2.0f, 0.0f, 2, 1, 3, ParamUnit::Enum, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kAmount = {"AMOUNT", "st", nullptr, nullptr, -24.0f, 24.0f, 0.0f, 48, 12, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};  // Shift + encoder: an octave (12 st)
constexpr ParamDesc kTo = {"TO", "", nullptr, nullptr, 1.0f, static_cast<float>(model::edit::kInstrumentSlots), 1.0f, model::edit::kInstrumentSlots - 1, 8, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kWhat = {"WHAT", "", kWhatNames, nullptr, 0.0f, 4.0f, 4.0f, 4, 1, 5, ParamUnit::Enum, ParamCurve::Linear, 0, 0};

bool enumerated(const PatternToolsView& v, PatternToolsRow row) noexcept {
  return row == PatternToolsRow::Op || (row == PatternToolsRow::Value && v.tool == PTool::Clear);
}

}  // namespace

unsigned pattern_tools_rows(const PatternToolsView&) noexcept { return kPatternToolsRows; }

const ParamDesc& pattern_tools_desc(const PatternToolsView& v, PatternToolsRow row) noexcept {
  if (row == PatternToolsRow::Op) {
    return kOp;
  }
  switch (v.tool) {
    case PTool::Transpose: return kAmount;
    case PTool::Instrument: return kTo;
    case PTool::Clear: break;
  }
  return kWhat;
}

const char* pattern_tools_caption(const PatternToolsView& v, PatternToolsRow row) noexcept { return pattern_tools_desc(v, row).name; }

float pattern_tools_get(const PatternToolsView& v, PatternToolsRow row) noexcept {
  if (row == PatternToolsRow::Op) {
    return static_cast<float>(v.tool);
  }
  switch (v.tool) {
    case PTool::Transpose: return static_cast<float>(v.amount);
    case PTool::Instrument: return static_cast<float>(v.instrument);
    case PTool::Clear: return static_cast<float>(v.what);
  }
  return 0.0f;
}

void pattern_tools_set(PatternToolsView& v, PatternToolsRow row, float value) noexcept {
  const float clamped = core::param_clamp(pattern_tools_desc(v, row), value);
  const int n = static_cast<int>(clamped + (clamped < 0.0f ? -0.5f : 0.5f));
  if (row == PatternToolsRow::Op) {
    v.tool = static_cast<PTool>(n);
    return;
  }
  switch (v.tool) {
    case PTool::Transpose: v.amount = static_cast<std::int8_t>(n); break;
    case PTool::Instrument: v.instrument = static_cast<std::uint8_t>(n); break;
    case PTool::Clear: v.what = static_cast<SongWhat>(n); break;
  }
}

bool pattern_tools_step(PatternToolsView& v, PatternToolsRow row, int detents) noexcept {
  if (detents == 0) {
    return false;
  }
  const ParamDesc& d = pattern_tools_desc(v, row);
  const float now = pattern_tools_get(v, row);
  float to;
  if (enumerated(v, row)) {
    const int count = static_cast<int>(d.max - d.min) + 1;
    int k = (static_cast<int>(now - d.min) + detents) % count;
    k = k < 0 ? k + count : k;
    to = d.min + static_cast<float>(k);
  } else {
    to = core::param_step(d, now, detents);
  }
  pattern_tools_set(v, row, to);
  return pattern_tools_get(v, row) != now;
}

unsigned pattern_tools_text(const PatternToolsView& v, PatternToolsRow row, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  char tmp[core::kParamTextChars + 4];
  const char* text = tmp;
  if (row == PatternToolsRow::Value && v.tool == PTool::Transpose) {
    unsigned n = 0;
    const int a = v.amount;
    if (a != 0) {
      tmp[n++] = a < 0 ? '-' : '+';
    }
    const unsigned mag = static_cast<unsigned>(a < 0 ? -a : a);
    if (mag >= 10u) {
      tmp[n++] = static_cast<char>('0' + mag / 10u);
    }
    tmp[n++] = static_cast<char>('0' + mag % 10u);
    tmp[n] = '\0';
  } else {
    (void)core::param_format(pattern_tools_desc(v, row), pattern_tools_get(v, row), tmp, sizeof tmp);
  }
  unsigned n = 0;
  while (text[n] != '\0' && n + 1u < cap) {
    out[n] = text[n];
    ++n;
  }
  out[n] = '\0';
  return n;
}

void pattern_tools_target(const Selection& sel, int cursor_row, int cursor_track, core::FixedString<48>& out) noexcept {
  const Selection::Block b = sel.block(cursor_row, cursor_track);
  if (!sel.active) {
    out.assign("STEP  TRACK ");
    append_uint(out, static_cast<unsigned>(cursor_track + 1));
    out.append("  ROW ");
    append_dec(out, static_cast<unsigned>(cursor_row + 1), 2);
    return;
  }
  out.assign("BLOCK  ");
  if (b.track0 == b.track1) {
    out.append("TRACK ");
    append_uint(out, static_cast<unsigned>(b.track0 + 1));
  } else {
    out.append("TRACKS ");
    append_uint(out, static_cast<unsigned>(b.track0 + 1));
    out.append("-");
    append_uint(out, static_cast<unsigned>(b.track1 + 1));
  }
  if (!b.whole_steps()) {  // a column block names its columns
    out.append("  ");
    out.append(kColumnNames[b.col0 < 0 ? 0 : (b.col0 > 4 ? 4 : b.col0)]);
    if (b.col1 != b.col0) {
      out.append("-");
      out.append(kColumnNames[b.col1 < 0 ? 0 : (b.col1 > 4 ? 4 : b.col1)]);
    }
  }
  out.append("  ROWS ");
  append_dec(out, static_cast<unsigned>(b.row0 + 1), 2);
  if (b.row1 != b.row0) {
    out.append("-");
    append_dec(out, static_cast<unsigned>(b.row1 + 1), 2);
  }
}

void pattern_tools_count(const model::Project* project, unsigned pattern, const Selection::Block& b, unsigned& steps, unsigned& notes) noexcept {
  steps = 0u;
  notes = 0u;
  const model::Pattern* pat = project != nullptr ? project->pattern(pattern) : nullptr;
  const int tracks = project != nullptr ? static_cast<int>(project->active_tracks()) : 0;
  for (int t = b.track0; t <= b.track1 && t < tracks; ++t) {
    const int len = pat != nullptr ? static_cast<int>(pat->track_length[t]) : 0;
    const int r1 = b.row1 < len - 1 ? b.row1 : len - 1;
    if (b.row0 > r1) {
      continue;
    }
    steps += static_cast<unsigned>(r1 - b.row0 + 1);
    if (pat != nullptr) {
      for (int r = b.row0; r <= r1; ++r) {
        const std::uint8_t note = pat->steps[t][r].note;
        if (note >= 1u && note <= 128u) {  // a real note, not empty / fade / cut / off
          ++notes;
        }
      }
    }
  }
  if (pat == nullptr) {  // a pattern that does not exist yet: the whole range counts, with no notes
    steps = static_cast<unsigned>((b.row1 - b.row0 + 1) * (b.track1 - b.track0 + 1));
  }
}

}  // namespace o3os::app
