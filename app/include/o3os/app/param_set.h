// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-INSFX-ABI2 A8 (docs/specs/OS-INSFX-ABI2.md section 3.6): the one parameter-set model of a module, a named list of values in the module's parameter order. A factory preset is one
// (ModuleUi::factory_presets builds them from the descriptor's o3m_preset table); OS-MODPRESET reuses it for the card's presets (Source::Card). New Community OS behaviour. Pure: no
// module, no project, no clock.
//
// A set does not have to give every value: `mask` says which of the 32 it holds. Applying a set (ModuleUi::apply_preset) writes EVERY parameter of the module: a masked value is held to the
// parameter's descriptor (clamped, and rounded when the parameter is whole numbers), any other parameter takes its default. A value that is not a number reads as the default.

#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kParamSetValues = 32;     // O3M_PARAMS_MAX: a module has at most this many parameters
inline constexpr unsigned kParamSetNameChars = 15;  // O3M_PRESET_NAME_MAX - 1

// A value held to a parameter the way the params page holds an edit: clamped to the descriptor (NaN gives the default), then rounded to a whole number when the module says whole numbers
// (INT or BOOL), then clamped again. The page's writes (param_actions.cpp) and a preset's share this one.
inline float fit_param_value(const core::ParamDesc& d, bool whole, float value) noexcept {
  float v = core::param_clamp(d, value);
  if (whole) {
    v = core::param_clamp(d, static_cast<float>(static_cast<std::int32_t>(v + (v < 0.0f ? -0.5f : 0.5f))));
  }
  return v;
}

struct ParamSet {
  enum class Source : std::uint8_t { Factory, Card };

  char name[kParamSetNameChars + 1] = {};
  Source source = Source::Factory;
  std::uint32_t mask = 0;  // bit i set: values[i] is given
  float values[kParamSetValues] = {};

  // The name, cut to kParamSetNameChars characters (a null `text` is an empty name).
  void set_name(const char* text) noexcept {
    unsigned n = 0;
    for (; text != nullptr && text[n] != '\0' && n < kParamSetNameChars; ++n) {
      name[n] = text[n];
    }
    name[n] = '\0';
  }
  // Gives value `i` (false: there are only kParamSetValues). The value is kept as it is: it is held to the descriptor when the set is applied.
  bool set_value(unsigned i, float value) noexcept {
    if (i >= kParamSetValues) {
      return false;
    }
    values[i] = value;
    mask |= std::uint32_t{1} << i;
    return true;
  }
  bool has(unsigned i) const noexcept { return i < kParamSetValues && ((mask >> i) & 1u) != 0u; }
  // How many values the set gives.
  unsigned given() const noexcept {
    unsigned n = 0;
    for (unsigned i = 0; i < kParamSetValues; ++i) {
      n += (mask >> i) & 1u;
    }
    return n;
  }
  // What parameter `i` (descriptor `d`; `whole`: the module says whole numbers) becomes when the set is applied: the given value held to the descriptor, else the default (held the same way).
  float resolve(unsigned i, const core::ParamDesc& d, bool whole) const noexcept { return fit_param_value(d, whole, has(i) ? values[i] : d.def); }
};

}  // namespace o3os::app
