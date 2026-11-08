// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/fill_view.h>

#include <o3os/app/fx_text.h>
#include <o3os/app/note_entry.h>
#include <o3os/app/pattern_data.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>
#include <o3os/core/scales.h>
#include <o3os/model/fx_table.h>

#include <cstring>

namespace o3os::app {

namespace {

namespace fill = model::fill;

constexpr const char* kWhatNames[5] = {"NOTE", "INSTR", "VOLUME", "FX 1", "FX 2"};
constexpr const char* kTypeNames[4] = {"EVERY N", "RANDOM", "EUCLID", "SCALE"};
constexpr const char* kModeNames[3] = {"CONST", "RAMP", "RANDOM"};

using core::ParamCurve;
using core::ParamDesc;
using core::ParamUnit;

// name, unit text, names, knots, min, max, def, detents, coarse, list_count, unit, curve, flags, group
constexpr ParamDesc kRows[kFillRows] = {
    {"WHAT", "", kWhatNames, nullptr, 0.0f, 4.0f, 0.0f, 4, 1, 5, ParamUnit::Enum, ParamCurve::Linear, 0, 0},
    {"TYPE", "", kTypeNames, nullptr, 0.0f, 3.0f, 0.0f, 3, 1, 4, ParamUnit::Enum, ParamCurve::Linear, 0, 0},
    {"N", "", nullptr, nullptr, 1.0f, 32.0f, 1.0f, 31, 4, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0},
    {"MODE", "", kModeNames, nullptr, 0.0f, 2.0f, 0.0f, 2, 1, 3, ParamUnit::Enum, ParamCurve::Linear, 0, 0},
    {"KEY", "", nullptr, nullptr, 0.0f, 39.0f, 0.0f, 39, 1, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0},  // the scale; the root is Shift + encoder
    {"FROM", "", nullptr, nullptr, 0.0f, 255.0f, 0.0f, 255, 10, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0},  // the range is the domain of WHAT (model::fill::domain)
    {"TO", "", nullptr, nullptr, 0.0f, 255.0f, 0.0f, 255, 10, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0},
};
// Row 2 of TYPE RANDOM, and row 4 of an FX fill (the picker's entries 1..59: None is not one).
constexpr ParamDesc kDensity = {"DENSITY", "%", nullptr, nullptr, 0.0f, 100.0f, 30.0f, 100, 10, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kFxType = {"FX TYPE", "", nullptr, nullptr, 1.0f, static_cast<float>(model::fx::kPickerCount - 1u), 1.0f, model::fx::kPickerCount - 2u, 1, 0,
                               ParamUnit::Int, ParamCurve::Linear, 0, 0};
static_assert(sizeof(kWhatNames) / sizeof(kWhatNames[0]) == 5 && sizeof(kTypeNames) / sizeof(kTypeNames[0]) == 4 && sizeof(kModeNames) / sizeof(kModeNames[0]) == 3,
              "an Enum row has one name for each value");

bool is_fx(const FillView& f) noexcept { return f.plan.what == fill::What::Fx1 || f.plan.what == fill::What::Fx2; }

// A note fill of TYPE RANDOM or SCALE: those two choose the notes, FROM..TO is their window and MODE plays no part (OS-FILL2 Q1).
bool note_window(const FillView& f) noexcept {
  return f.plan.what == fill::What::Note && (f.plan.type == fill::Type::Random || f.plan.type == fill::Type::Scale);
}

const ParamDesc& row_desc(const FillView& f, FillRow row) noexcept {
  if (row == FillRow::Count && f.plan.type == fill::Type::Random) {
    return kDensity;
  }
  if (row == FillRow::Key && is_fx(f)) {
    return kFxType;
  }
  return fill_desc(row);
}

bool enumerated(FillRow row) noexcept { return row == FillRow::What || row == FillRow::Type || row == FillRow::Mode || row == FillRow::Key; }

// Row 2 is the count of the TYPE: EVENTS of EUCLID, DENSITY of RANDOM, N of the others (the three fields stay separate).
std::uint8_t& count_of(FillView& f) noexcept {
  return f.plan.type == fill::Type::Euclid ? f.plan.events : (f.plan.type == fill::Type::Random ? f.plan.density : f.plan.n);
}
unsigned count_of(const FillView& f) noexcept {
  return f.plan.type == fill::Type::Euclid ? f.plan.events : (f.plan.type == fill::Type::Random ? f.plan.density : f.plan.n);
}

}  // namespace

const ParamDesc& fill_desc(FillRow row) noexcept { return kRows[static_cast<unsigned>(row) < kFillRows ? static_cast<unsigned>(row) : 0u]; }

const char* fill_caption(const FillView& f, FillRow row) noexcept {
  switch (row) {
    case FillRow::Count:
      return f.plan.type == fill::Type::Euclid ? "EVENTS" : (f.plan.type == fill::Type::Random ? "DENSITY" : "N");
    case FillRow::Key:
      return is_fx(f) ? "FX TYPE" : "KEY";
    case FillRow::From:
      return f.plan.mode == fill::Mode::Const && !note_window(f) ? "VALUE" : "FROM";
    default:
      return fill_desc(row).name;
  }
}

bool fill_row_used(const FillView& f, FillRow row) noexcept {
  switch (row) {
    case FillRow::Mode: return !note_window(f);
    case FillRow::Key: return f.plan.what == fill::What::Note || is_fx(f);
    case FillRow::To: return f.plan.mode != fill::Mode::Const || note_window(f);
    default: return true;
  }
}

bool fill_random(const FillView& f) noexcept { return f.plan.type == fill::Type::Random || (fill_row_used(f, FillRow::Mode) && f.plan.mode == fill::Mode::Random); }

float fill_get(const FillView& f, FillRow row) noexcept {
  switch (row) {
    case FillRow::What: return static_cast<float>(f.plan.what);
    case FillRow::Type: return static_cast<float>(f.plan.type);
    case FillRow::Count: return static_cast<float>(count_of(f));
    case FillRow::Mode: return static_cast<float>(f.plan.mode);
    case FillRow::Key: {
      const unsigned pick = model::fx::picker_index(f.plan.fx_type);
      return is_fx(f) ? static_cast<float>(pick == 0u ? 1u : pick) : static_cast<float>(f.plan.scale);
    }
    case FillRow::From: return static_cast<float>(f.plan.from);
    case FillRow::To: return static_cast<float>(f.plan.to);
  }
  return 0.0f;
}

void fill_set(FillView& f, FillRow row, float value) noexcept {
  const unsigned v = static_cast<unsigned>(core::param_clamp(row_desc(f, row), value) + 0.5f);
  switch (row) {
    case FillRow::What:
      if (f.plan.what != static_cast<fill::What>(v)) {
        f.plan.what = static_cast<fill::What>(v);
        fill::range_default(f.plan, f.open);
      }
      break;
    case FillRow::Type: f.plan.type = static_cast<fill::Type>(v); break;
    case FillRow::Count: count_of(f) = static_cast<std::uint8_t>(v); break;
    case FillRow::Mode: f.plan.mode = static_cast<fill::Mode>(v); break;
    case FillRow::Key:
      if (is_fx(f)) {
        const std::uint8_t type = model::fx::kPickerOrder[v];
        if (type != f.plan.fx_type) {  // D3: a new type starts at its own default, both ends
          f.plan.fx_type = type;
          f.plan.from = f.plan.to = model::fx::row(type).start;
        }
      } else {
        f.plan.scale = static_cast<std::uint8_t>(v);
      }
      break;
    case FillRow::From:
    case FillRow::To: {
      const fill::Range d = fill::domain(f.plan);
      const float c = value < static_cast<float>(d.lo) ? static_cast<float>(d.lo) : (value > static_cast<float>(d.hi) ? static_cast<float>(d.hi) : value);
      (row == FillRow::From ? f.plan.from : f.plan.to) = static_cast<std::uint8_t>(c + 0.5f);
      break;
    }
  }
}

bool fill_step(FillView& f, FillRow row, int detents) noexcept {
  if (!fill_row_used(f, row) || detents == 0) {
    return false;
  }
  const float now = fill_get(f, row);
  float to;
  if (row == FillRow::From || row == FillRow::To) {
    const fill::Range d = fill::domain(f.plan);
    if (f.plan.what == fill::What::Instr) {  // the instrument order (64..49, then 1..48): stops at its ends
      to = static_cast<float>(model::edit::instrument_walk(static_cast<unsigned>(now), detents, static_cast<int>(model::edit::kInstrumentSlots)));
    } else {
      const int v = static_cast<int>(now) + detents;
      to = static_cast<float>(v < d.lo ? d.lo : (v > d.hi ? d.hi : v));
    }
  } else {
    const ParamDesc& d = row_desc(f, row);
    if (enumerated(row)) {
      const int count = static_cast<int>(d.max - d.min) + 1;
      int k = (static_cast<int>(now - d.min) + detents) % count;
      k = k < 0 ? k + count : k;
      to = d.min + static_cast<float>(k);
    } else {
      to = core::param_step(d, now, detents);
    }
  }
  fill_set(f, row, to);
  return fill_get(f, row) != now;
}

bool fill_step_coarse(FillView& f, FillRow row, int detents) noexcept {
  if (!fill_row_used(f, row) || detents == 0) {
    return false;
  }
  switch (row) {
    case FillRow::Key:
      if (!is_fx(f)) {  // the root, a ring of twelve
        int r = (static_cast<int>(f.plan.root) + detents) % 12;
        r = r < 0 ? r + 12 : r;
        const bool changed = r != f.plan.root;
        f.plan.root = static_cast<std::uint8_t>(r);
        return changed;
      }
      return fill_step(f, row, detents);
    case FillRow::From:
    case FillRow::To:
      return fill_step(f, row, detents * (f.plan.what == fill::What::Note ? 12 : 10));
    case FillRow::Count:
      return fill_step(f, row, detents * static_cast<int>(row_desc(f, row).coarse));
    default:
      return fill_step(f, row, detents);  // the enumerated rows: the fine step
  }
}

namespace {

// FROM and TO in the words of WHAT.
void append_value(core::FixedString<24>& s, const FillView& f, unsigned value, int octave_shift) noexcept {
  switch (f.plan.what) {
    case fill::What::Note: {
      char name[4];
      note_name(value, octave_shift, name);
      s.append(name);
      break;
    }
    case fill::What::Instr:
      append_dec(s, value, 2);
      break;
    case fill::What::Vol:
      append_uint(s, value);
      break;
    case fill::What::Fx1:
    case fill::What::Fx2:
      if (!append_fx_value(s, f.plan.fx_type, static_cast<std::uint8_t>(value))) {
        append_uint(s, value);
      }
      break;
  }
}

}  // namespace

unsigned fill_text(const FillView& f, FillRow row, char* out, unsigned cap, int octave_shift) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  core::FixedString<24> s;
  if (!fill_row_used(f, row)) {
    s.assign("-");
  } else if (row == FillRow::Key) {
    if (is_fx(f)) {
      char name[kFxNameMax];
      fx_picker_name(model::fx::picker_index(f.plan.fx_type), name, sizeof name);
      s.assign(name);
    } else {
      const unsigned scale = f.plan.scale < core::kScaleCount ? f.plan.scale : 0u;
      if (scale > 1u) {  // "No scale" and "Chromatic" take every note: the root plays no part
        s.assign(kRootNames[f.plan.root < 12u ? f.plan.root : 0u]);
        s.append(" ");
      }
      s.append(core::kScaleNames[scale]);
    }
  } else if (row == FillRow::From || row == FillRow::To) {
    append_value(s, f, row == FillRow::From ? f.plan.from : f.plan.to, octave_shift);
  } else {
    char tmp[core::kParamTextChars + 1];
    (void)core::param_format(row_desc(f, row), fill_get(f, row), tmp, sizeof tmp);
    s.assign(tmp);
  }
  const char* text = s.c_str();
  unsigned n = 0;
  while (text[n] != '\0' && n + 1u < cap) {
    out[n] = text[n];
    ++n;
  }
  while (n > 0u && out[n - 1u] == ' ') {  // "Phrygian " carries a trailing space
    --n;
  }
  out[n] = '\0';
  return n;
}

// OS-PTOOLS2 S3c: the footer of the dialog, in the Song TOOLS way: `BLOCK  TRACK 3  ROWS 1-16`. The target is the selection: the cursor step (STEP) with nothing selected or a one-cell selection, a BLOCK range, the whole
// column of Shift + Up at row 1 (TRACK), and PATTERN when the block covers every track. The numbers are the screen's (1-based). (The `column` flag of the selection does not outlive Shift, so the kind comes from the block's extent.)
unsigned fill_target_text(const Selection& sel, int cursor_row, int cursor_track, int steps, int tracks, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  const Selection::Block b = sel.block(cursor_row, cursor_track);
  core::FixedString<48> s;
  const auto rows = [&] {
    if (b.row1 > b.row0) {
      s.append("ROWS ");
      append_uint(s, static_cast<unsigned>(b.row0 + 1));
      s.append("-");
      append_uint(s, static_cast<unsigned>(b.row1 + 1));
    } else {
      s.append("ROW ");
      append_uint(s, static_cast<unsigned>(b.row0 + 1));
    }
  };
  const bool single_cell = b.row0 == b.row1 && b.track0 == b.track1;
  const bool whole_rows = b.row0 == 0 && b.row1 >= steps - 1;
  const bool all_tracks = b.track0 == 0 && tracks > 1 && b.track1 >= tracks - 1;
  if (!sel.active || single_cell) {
    s.assign("STEP  TRACK ");
    append_uint(s, static_cast<unsigned>(b.track0 + 1));
    s.append("  ");
    rows();
  } else if (whole_rows && all_tracks) {
    s.assign("PATTERN  ALL TRACKS  ");
    rows();
  } else if (whole_rows && b.track0 == b.track1) {
    s.assign("TRACK ");
    append_uint(s, static_cast<unsigned>(b.track0 + 1));
    s.append("  ");
    rows();
  } else {
    s.assign("BLOCK  ");
    if (b.track1 > b.track0) {
      s.append("TRACKS ");
      append_uint(s, static_cast<unsigned>(b.track0 + 1));
      s.append("-");
      append_uint(s, static_cast<unsigned>(b.track1 + 1));
    } else {
      s.append("TRACK ");
      append_uint(s, static_cast<unsigned>(b.track0 + 1));
    }
    s.append("  ");
    rows();
  }
  const char* text = s.c_str();
  unsigned n = 0;
  while (text[n] != '\0' && n + 1u < cap) {
    out[n] = text[n];
    ++n;
  }
  out[n] = '\0';
  return n;
}

}  // namespace o3os::app
