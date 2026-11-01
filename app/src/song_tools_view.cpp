// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_tools_view.h>

#include <o3os/model/step_edit.h>

namespace o3os::app {

namespace {

constexpr const char* kToolNames[6] = {"TRANSPOSE", "INSTRUMENT", "CLEAR", "EXPORT WAV", "FORM", "BRIDGE"};
constexpr const char* kWhatNames[5] = {"NOTES", "VOLUME", "FX 1", "FX 2", "ALL"};
constexpr const char* kFormNames[kSongFormCount] = {"LOOP>SONG", "POP", "TECHNO"};

using core::ParamCurve;
using core::ParamDesc;
using core::ParamUnit;

// name, unit text, names, knots, min, max, def, detents, coarse, list_count, unit, curve, flags, group
constexpr ParamDesc kOp = {"OP", "", kToolNames, nullptr, 0.0f, 5.0f, 0.0f, 5, 1, 6, ParamUnit::Enum, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kAmount = {"AMOUNT", "st", nullptr, nullptr, -24.0f, 24.0f, 0.0f, 48, 12, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kTo = {"TO", "", nullptr, nullptr, 1.0f, static_cast<float>(model::edit::kInstrumentSlots), 1.0f, model::edit::kInstrumentSlots - 1, 8, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kWhat = {"WHAT", "", kWhatNames, nullptr, 0.0f, 4.0f, 4.0f, 4, 1, 5, ParamUnit::Enum, ParamCurve::Linear, 0, 0};

constexpr ParamDesc kForm = {"FORM", "", kFormNames, nullptr, 0.0f, 2.0f, 0.0f, 2, 1, kSongFormCount, ParamUnit::Enum, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kSlots = {"SLOTS", "", nullptr, nullptr, 1.0f, static_cast<float>(kSongBridgeMax), static_cast<float>(kSongBridgeDefault), kSongBridgeMax - 1, 4, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kDrums = {"DRUMS", "", nullptr, nullptr, 1.0f, static_cast<float>(kSongDrumsMax), static_cast<float>(kSongDrumsDefault), kSongDrumsMax - 1, 1, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};

bool enumerated(const SongToolsView& v, SongToolsRow row) noexcept {
  return row == SongToolsRow::Op || (row == SongToolsRow::Value && (v.tool == SongTool::Clear || v.tool == SongTool::Form));
}

}  // namespace

unsigned song_tools_rows(const SongToolsView& v) noexcept { return v.tool == SongTool::ExportWav ? 1u : (v.tool == SongTool::Form ? 3u : 2u); }

const ParamDesc& song_tools_desc(const SongToolsView& v, SongToolsRow row) noexcept {
  if (row == SongToolsRow::Op) {
    return kOp;
  }
  if (row == SongToolsRow::Drums) {
    return kDrums;
  }
  switch (v.tool) {
    case SongTool::Transpose: return kAmount;
    case SongTool::Instrument: return kTo;
    case SongTool::Form: return kForm;
    case SongTool::Bridge: return kSlots;
    case SongTool::Clear:
    case SongTool::ExportWav: break;
  }
  return kWhat;
}

const char* song_tools_caption(const SongToolsView& v, SongToolsRow row) noexcept { return song_tools_desc(v, row).name; }

float song_tools_get(const SongToolsView& v, SongToolsRow row) noexcept {
  if (row == SongToolsRow::Op) {
    return static_cast<float>(v.tool);
  }
  if (row == SongToolsRow::Drums) {
    return static_cast<float>(v.drums);
  }
  switch (v.tool) {
    case SongTool::Transpose: return static_cast<float>(v.amount);
    case SongTool::Instrument: return static_cast<float>(v.instrument);
    case SongTool::Clear: return static_cast<float>(v.what);
    case SongTool::Form: return static_cast<float>(v.form);
    case SongTool::Bridge: return static_cast<float>(v.bridge_slots);
    case SongTool::ExportWav: break;
  }
  return 0.0f;
}

void song_tools_set(SongToolsView& v, SongToolsRow row, float value) noexcept {
  const float clamped = core::param_clamp(song_tools_desc(v, row), value);
  const int n = static_cast<int>(clamped + (clamped < 0.0f ? -0.5f : 0.5f));
  if (row == SongToolsRow::Op) {
    v.tool = static_cast<SongTool>(n);
    return;
  }
  if (row == SongToolsRow::Drums) {
    v.drums = static_cast<std::uint8_t>(n);
    return;
  }
  switch (v.tool) {
    case SongTool::Transpose: v.amount = static_cast<std::int8_t>(n); break;
    case SongTool::Instrument: v.instrument = static_cast<std::uint8_t>(n); break;
    case SongTool::Clear: v.what = static_cast<SongWhat>(n); break;
    case SongTool::Form: v.form = static_cast<SongForm>(n); break;
    case SongTool::Bridge: v.bridge_slots = static_cast<std::uint8_t>(n); break;
    case SongTool::ExportWav: break;
  }
}

bool song_tools_step(SongToolsView& v, SongToolsRow row, int detents) noexcept {
  if (detents == 0) {
    return false;
  }
  const ParamDesc& d = song_tools_desc(v, row);
  const float now = song_tools_get(v, row);
  float to;
  if (enumerated(v, row)) {
    const int count = static_cast<int>(d.max - d.min) + 1;
    int k = (static_cast<int>(now - d.min) + detents) % count;
    k = k < 0 ? k + count : k;
    to = d.min + static_cast<float>(k);
  } else {
    to = core::param_step(d, now, detents);
  }
  song_tools_set(v, row, to);
  return song_tools_get(v, row) != now;
}

unsigned song_tools_text(const SongToolsView& v, SongToolsRow row, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  char tmp[core::kParamTextChars + 4];
  const char* text = tmp;
  if (row == SongToolsRow::Value && v.tool == SongTool::Transpose) {
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
  } else if (row == SongToolsRow::Drums) {
    // "TRACKS 1-4" (the drums are tracks 1..N), "TRACK 1" for N = 1
    unsigned n = 0;
    for (const char* s = v.drums == 1u ? "TRACK 1" : "TRACKS 1-"; *s != '\0'; ++s) {
      tmp[n++] = *s;
    }
    if (v.drums != 1u) {
      tmp[n++] = static_cast<char>('0' + v.drums % 10u);
    }
    tmp[n] = '\0';
  } else {
    (void)core::param_format(song_tools_desc(v, row), song_tools_get(v, row), tmp, sizeof tmp);
  }
  unsigned n = 0;
  while (text[n] != '\0' && n + 1u < cap) {
    out[n] = text[n];
    ++n;
  }
  out[n] = '\0';
  return n;
}

void song_tools_refresh(SongToolsView& v, const model::Project& project) noexcept {
  if (v.tool != SongTool::Form && v.tool != SongTool::Bridge) {
    v.plan = SongBuildInfo{};
    return;
  }
  SongBuildPlan plan;
  if (v.tool == SongTool::Form) {
    song_form_plan(project, v.rect, v.form, v.drums, plan);
  } else {
    song_bridge_plan(project, v.rect, v.bridge_slots, plan, false);  // the footer shows counts and refusals, not bytes: no cell walk on a knob tick (F1 plans in full)
  }
  v.plan = plan.info;
}

std::uint8_t song_tools_elements(SongWhat what) noexcept {
  switch (what) {
    case SongWhat::Notes: return static_cast<std::uint8_t>(model::edit::kElNote);
    case SongWhat::Volume: return static_cast<std::uint8_t>(model::edit::kElVol);
    case SongWhat::Fx1: return static_cast<std::uint8_t>(model::edit::kElFx1);
    case SongWhat::Fx2: return static_cast<std::uint8_t>(model::edit::kElFx2);
    case SongWhat::All: return static_cast<std::uint8_t>(model::edit::kElAll);
  }
  return static_cast<std::uint8_t>(model::edit::kElAll);
}

}  // namespace o3os::app
