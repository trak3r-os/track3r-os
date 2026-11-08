// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control layers (seq/control.h).
#include <o3os/seq/control.h>

namespace o3os::seq {

void ControlTable::attach_perf(PerfStore& store) noexcept {
  store = PerfStore{};
  perf_ = &store;
  for (std::uint32_t t = 0; t < kTracks; ++t) {
    perf_on_[t] = 0;
  }
}

void ControlTable::reset() noexcept {
  for (std::uint32_t t = 0; t < kTracks; ++t) {
    for (std::uint32_t c = 0; c < kColumns; ++c) {
      type_[t][c] = 0;
      value_[t][c] = 0;
    }
    perf_on_[t] = 0;
    if (perf_ != nullptr) {
      for (std::uint32_t g = 0; g < kFxGroupCount; ++g) {
        perf_->val[t][g] = 0;
      }
    }
  }
}

void ControlTable::reset_columns() noexcept {
  for (std::uint32_t t = 0; t < kTracks; ++t) {
    for (std::uint32_t c = 0; c < kColumns; ++c) {
      type_[t][c] = 0;
      value_[t][c] = 0;
    }
  }
}

ModState ControlTable::resolve(std::uint8_t track, FxGroup g) const noexcept {
  ModState s{false, 0, 0, 0, 0};
  if (track >= kTracks || g == FxGroup::None) {
    return s;
  }
  // The sequencer layers: the first column over the second.
  bool seq = false;
  for (std::uint32_t c = 0; c < kColumns && !seq; ++c) {
    if (type_[track][c] != 0u && fx_group(type_[track][c], c) == g) {
      seq = true;
      s.active = true;
      s.layer = static_cast<std::uint8_t>(c == 0u ? audio::kLayerFx1 : audio::kLayerFx2);
      s.fx_type = type_[track][c];
      s.value = value_[track][c];
    }
  }
  if ((perf_on_[track] >> static_cast<std::uint32_t>(g)) & 1u) {
    s.active = true;
    s.layer = audio::kLayerPerf;
    s.perf = perf_ != nullptr ? perf_->val[track][static_cast<std::uint32_t>(g)] : std::int16_t{0};
  }
  return s;
}

std::uint32_t ControlTable::set_column(std::uint8_t track, std::uint8_t column, std::uint8_t type, std::uint8_t value, ModChange out[2]) noexcept {
  if (track >= kTracks || column >= kColumns) {
    return 0;
  }
  // Only a voice FX is held; anything else (None, Off, a row FX, an inert type) leaves the column empty.
  const bool holds = type != 0u && (fx_kind(type) == FxKind::Voice || fx_kind(type) == FxKind::RowVoice);
  const std::uint8_t new_type = holds ? type : std::uint8_t{0};
  const std::uint8_t old_type = type_[track][column];
  const FxGroup old_group = old_type != 0u ? fx_group(old_type, column) : FxGroup::None;
  const FxGroup new_group = new_type != 0u ? fx_group(new_type, column) : FxGroup::None;

  const ModState before_old = resolve(track, old_group);
  const ModState before_new = resolve(track, new_group);
  type_[track][column] = new_type;
  value_[track][column] = holds ? value : std::uint8_t{0};
  std::uint32_t n = 0;
  if (old_group != FxGroup::None) {
    const ModState after = resolve(track, old_group);
    if (after != before_old) {
      out[n++] = ModChange{track, old_group, after};
    }
  }
  if (new_group != FxGroup::None && new_group != old_group) {
    const ModState after = resolve(track, new_group);
    if (after != before_new) {
      out[n++] = ModChange{track, new_group, after};
    }
  }
  return n;
}

std::uint32_t ControlTable::set_columns(std::uint8_t track, std::uint8_t mask, const std::uint8_t type[2], const std::uint8_t value[2], ModChange out[4]) noexcept {
  if (track >= kTracks) {
    return 0;
  }
  FxGroup groups[4];
  ModState before[4];
  std::uint32_t ng = 0;
  const auto touch = [&](FxGroup g) {
    if (g == FxGroup::None) {
      return;
    }
    for (std::uint32_t i = 0; i < ng; ++i) {
      if (groups[i] == g) {
        return;
      }
    }
    groups[ng] = g;
    before[ng] = resolve(track, g);
    ++ng;
  };
  std::uint8_t new_type[2] = {0, 0};
  std::uint8_t new_value[2] = {0, 0};
  for (std::uint32_t c = 0; c < kColumns; ++c) {
    if ((mask & (1u << c)) == 0u) {
      continue;
    }
    const std::uint8_t ty = type[c];
    const bool holds = ty != 0u && (fx_kind(ty) == FxKind::Voice || fx_kind(ty) == FxKind::RowVoice);
    new_type[c] = holds ? ty : std::uint8_t{0};
    new_value[c] = holds ? value[c] : std::uint8_t{0};
    if (type_[track][c] != 0u) {
      touch(fx_group(type_[track][c], c));
    }
    if (new_type[c] != 0u) {
      touch(fx_group(new_type[c], c));
    }
  }
  for (std::uint32_t c = 0; c < kColumns; ++c) {
    if ((mask & (1u << c)) != 0u) {
      type_[track][c] = new_type[c];
      value_[track][c] = new_value[c];
    }
  }
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < ng; ++i) {
    const ModState after = resolve(track, groups[i]);
    if (after != before[i]) {
      out[n++] = ModChange{track, groups[i], after};
    }
  }
  return n;
}

std::uint32_t ControlTable::set_perf(std::uint8_t track, FxGroup g, std::int16_t value, ModChange out[1]) noexcept {
  if (track >= kTracks || g == FxGroup::None || g >= FxGroup::kCount || perf_ == nullptr) {
    return 0;
  }
  const ModState before = resolve(track, g);
  perf_on_[track] |= 1u << static_cast<std::uint32_t>(g);
  perf_->val[track][static_cast<std::uint32_t>(g)] = value;
  const ModState after = resolve(track, g);
  if (after != before) {
    out[0] = ModChange{track, g, after};
    return 1;
  }
  return 0;
}

std::uint32_t ControlTable::end_perf(std::uint8_t track, FxGroup g, ModChange out[1]) noexcept {
  if (track >= kTracks || g == FxGroup::None || g >= FxGroup::kCount || perf_ == nullptr) {
    return 0;
  }
  const ModState before = resolve(track, g);
  perf_on_[track] &= ~(1u << static_cast<std::uint32_t>(g));
  perf_->val[track][static_cast<std::uint32_t>(g)] = 0;
  const ModState after = resolve(track, g);
  if (after != before) {
    out[0] = ModChange{track, g, after};
    return 1;
  }
  return 0;
}

void ControlTable::clear_track(std::uint8_t track) noexcept {
  if (track >= kTracks) {
    return;
  }
  for (std::uint32_t c = 0; c < kColumns; ++c) {
    type_[track][c] = 0;
    value_[track][c] = 0;
  }
  perf_on_[track] = 0;
  if (perf_ != nullptr) {
    for (std::uint32_t g = 0; g < kFxGroupCount; ++g) {
      perf_->val[track][g] = 0;
    }
  }
}

std::uint8_t ControlTable::column_type(std::uint8_t track, std::uint8_t column) const noexcept {
  return track < kTracks && column < kColumns ? type_[track][column] : std::uint8_t{0};
}
std::uint8_t ControlTable::column_value(std::uint8_t track, std::uint8_t column) const noexcept {
  return track < kTracks && column < kColumns ? value_[track][column] : std::uint8_t{0};
}
bool ControlTable::perf_holds(std::uint8_t track, FxGroup g) const noexcept {
  return track < kTracks && g != FxGroup::None && g < FxGroup::kCount && ((perf_on_[track] >> static_cast<std::uint32_t>(g)) & 1u) != 0u;
}
std::uint32_t ControlTable::active_groups(std::uint8_t track) const noexcept {
  if (track >= kTracks) {
    return 0;
  }
  std::uint32_t bits = perf_on_[track];
  for (std::uint32_t c = 0; c < kColumns; ++c) {
    if (type_[track][c] != 0u) {
      bits |= 1u << static_cast<std::uint32_t>(fx_group(type_[track][c], c));
    }
  }
  return bits;
}

}  // namespace o3os::seq
