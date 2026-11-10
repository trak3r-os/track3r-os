// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/param_auto.h>

#include <o3os/model/limits.h>
#include <o3os/model/params.h>

namespace o3os::model {

std::uint8_t auto_code(core::ParamId id) noexcept {
  if (core::param_kind(id) != core::ParamKind::Instrument) {
    return 0;
  }
  const unsigned field = core::param_field(id);
  if (instrument_param::is_module_field(field)) {
    return static_cast<std::uint8_t>(field - instrument_param::kModuleFirst + 1u);
  }
  if (field >= kAutoSampleFields) {
    return 0;
  }
  const core::ParamDesc* d = param_desc(id);
  return d != nullptr && (d->flags & core::kParamLearnable) != 0 ? static_cast<std::uint8_t>(field + 1u) : std::uint8_t{0};
}

core::ParamId auto_param(std::uint32_t slot, InstrumentKind kind, std::uint8_t code) noexcept {
  if (code == 0) {
    return core::ParamId::None;
  }
  if (kind == InstrumentKind::Sample && code <= kAutoSampleFields) {
    return core::param_id(core::ParamKind::Instrument, slot, code - 1u);
  }
  if (kind == InstrumentKind::Module && code <= instrument_param::kModuleMax) {
    return core::param_id(core::ParamKind::Instrument, slot, instrument_param::kModuleFirst + code - 1u);
  }
  return core::ParamId::None;
}

bool inst_has_auto_letter(const Instrument& in) noexcept {
  if (in.kind == InstrumentKind::Midi) {
    return false;
  }
  for (const std::uint8_t cc : in.midi_cc) {
    if (cc != 0) {
      return true;
    }
  }
  return false;
}

void auto_scan(const Project& p, AutoMap& out) noexcept {
  for (core::ParamId& id : out.id) {
    id = core::ParamId::None;
  }
  for (std::uint32_t s = 0; s < kMaxInstruments; ++s) {
    const Instrument* in = p.instrument(s);
    if (in == nullptr || in->kind == InstrumentKind::Midi) {
      continue;
    }
    for (unsigned l = 0; l < kAutoLetters; ++l) {
      if (out.id[l] == core::ParamId::None) {
        out.id[l] = auto_param(s, in->kind, in->midi_cc[l]);
      }
    }
  }
}

int auto_find(const AutoMap& m, core::ParamId id) noexcept {
  for (unsigned l = 0; l < kAutoLetters; ++l) {
    if (m.id[l] == id && id != core::ParamId::None) {
      return static_cast<int>(l);
    }
  }
  return -1;
}

int auto_first_free(const AutoMap& m) noexcept {
  for (unsigned l = 0; l < kAutoLetters; ++l) {
    if (m.id[l] == core::ParamId::None) {
      return static_cast<int>(l);
    }
  }
  return -1;
}

}  // namespace o3os::model
