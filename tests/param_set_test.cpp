// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-ABI2 A8 (docs/specs/OS-INSFX-ABI2.md section 3.6): app::ParamSet, the one parameter-set model of a module (a factory preset now, the card's presets with OS-MODPRESET): the mask of the values
// it gives, the default for every parameter it does not, the clamp to the descriptor and the rounding of a whole-number parameter, and fit_param_value, which the params page's writes share. Pure: no
// module, no project.
#include <doctest/doctest.h>

#include <o3os/app/param_set.h>
#include <o3os/core/param.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {

using namespace o3os;
using app::ParamSet;

// A 0..1 float parameter with the default 0.5, an INT 0..10 with the default 5, a BOOL (0..1, whole) with the default 1, an INT -12..12 with the default 0.
core::ParamDesc desc(float min, float max, float def) {
  core::ParamDesc d{};
  d.name = "P";
  d.unit_text = "";
  d.min = min;
  d.max = max;
  d.def = def;
  d.detents = 100;
  d.coarse = 10;
  d.unit = core::ParamUnit::NormPct;
  d.curve = core::ParamCurve::Linear;
  return d;
}

}  // namespace

TEST_CASE("param set: a new set gives nothing, has no name and is a factory set") {
  ParamSet s;
  CHECK(s.mask == 0u);
  CHECK(s.given() == 0u);
  CHECK(s.name[0] == '\0');
  CHECK(s.source == ParamSet::Source::Factory);
  for (unsigned i = 0; i < app::kParamSetValues; ++i) {
    CHECK(!s.has(i));
  }
  CHECK(!s.has(32u));
}

TEST_CASE("param set: set_value marks the mask bit and keeps the value as given; there are 32 values and no more") {
  ParamSet s;
  CHECK(s.set_value(0, 0.25f));
  CHECK(s.set_value(5, -7.5f));  // not held to anything here: the descriptor does that when the set is applied
  CHECK(s.set_value(31, 9.0f));
  CHECK(!s.set_value(32, 1.0f));
  CHECK(s.has(0));
  CHECK(!s.has(1));
  CHECK(s.has(5));
  CHECK(s.has(31));
  CHECK(!s.has(32));
  CHECK(s.given() == 3u);
  CHECK(s.mask == ((1u << 0) | (1u << 5) | (1u << 31)));
  CHECK(s.values[5] == -7.5f);
  CHECK(s.values[31] == 9.0f);
  CHECK(s.set_value(5, 0.75f));  // a value again: the same bit
  CHECK(s.given() == 3u);
  CHECK(s.values[5] == 0.75f);
}

TEST_CASE("param set: the name is cut to 15 characters and is always terminated; a null name is empty") {
  ParamSet s;
  s.set_name("Short");
  CHECK(std::strcmp(s.name, "Short") == 0);
  s.set_name("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  CHECK(std::strlen(s.name) == app::kParamSetNameChars);
  CHECK(std::strcmp(s.name, "ABCDEFGHIJKLMNO") == 0);
  CHECK(s.name[15] == '\0');
  s.set_name(nullptr);
  CHECK(s.name[0] == '\0');
  s.set_name("");
  CHECK(s.name[0] == '\0');
}

TEST_CASE("param set: a parameter the set does not give resolves to its default, the ones past `count` of a preset included") {
  const core::ParamDesc half = desc(0.0f, 1.0f, 0.5f);
  const core::ParamDesc steps = desc(0.0f, 10.0f, 5.0f);
  const core::ParamDesc wide = desc(-12.0f, 12.0f, 0.0f);
  ParamSet s;  // a preset of two values on a module of four parameters
  CHECK(s.set_value(0, 0.9f));
  CHECK(s.set_value(1, 8.0f));
  CHECK(s.resolve(0, half, false) == 0.9f);
  CHECK(s.resolve(1, steps, true) == 8.0f);
  CHECK(s.resolve(2, half, false) == 0.5f);   // past the count: the default
  CHECK(s.resolve(3, wide, true) == 0.0f);    // (a default of 0 is a default, not an absent value)
  // a gap in the middle is a default too
  ParamSet g;
  CHECK(g.set_value(0, 0.1f));
  CHECK(g.set_value(2, 0.3f));
  CHECK(g.resolve(1, steps, true) == 5.0f);
  CHECK(g.resolve(2, half, false) == 0.3f);
  // an empty set resolves every parameter to its default
  const ParamSet none;
  CHECK(none.resolve(0, half, false) == 0.5f);
  CHECK(none.resolve(7, steps, true) == 5.0f);
}

TEST_CASE("param set: a given value is clamped to the descriptor") {
  const core::ParamDesc half = desc(0.0f, 1.0f, 0.5f);
  const core::ParamDesc wide = desc(-12.0f, 12.0f, 0.0f);
  ParamSet s;
  CHECK(s.set_value(0, 3.0f));
  CHECK(s.set_value(1, -3.0f));
  CHECK(s.set_value(2, 99.0f));
  CHECK(s.set_value(3, -99.0f));
  CHECK(s.set_value(4, std::numeric_limits<float>::infinity()));
  CHECK(s.set_value(5, -std::numeric_limits<float>::infinity()));
  CHECK(s.resolve(0, half, false) == 1.0f);
  CHECK(s.resolve(1, half, false) == 0.0f);
  CHECK(s.resolve(2, wide, true) == 12.0f);
  CHECK(s.resolve(3, wide, true) == -12.0f);
  CHECK(s.resolve(4, half, false) == 1.0f);
  CHECK(s.resolve(5, half, false) == 0.0f);
  // inside the range a float is not touched
  CHECK(s.set_value(6, 0.123456f));
  CHECK(s.resolve(6, half, false) == 0.123456f);
}

TEST_CASE("param set: a value that is not a number reads as the default") {
  const core::ParamDesc half = desc(0.0f, 1.0f, 0.5f);
  const core::ParamDesc steps = desc(0.0f, 10.0f, 5.0f);
  ParamSet s;
  CHECK(s.set_value(0, std::numeric_limits<float>::quiet_NaN()));
  CHECK(s.set_value(1, std::numeric_limits<float>::quiet_NaN()));
  CHECK(s.resolve(0, half, false) == 0.5f);
  CHECK(s.resolve(1, steps, true) == 5.0f);
}

TEST_CASE("param set: a whole-number parameter is rounded half away from zero, then held to the range") {
  const core::ParamDesc steps = desc(0.0f, 10.0f, 5.0f);
  const core::ParamDesc wide = desc(-12.0f, 12.0f, 0.0f);
  ParamSet s;
  const float in[] = {2.4f, 2.5f, 2.6f, 9.6f, 10.4f, -0.4f, -0.6f, 0.0f};
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(s.set_value(i, in[i]));
  }
  CHECK(s.resolve(0, steps, true) == 2.0f);
  CHECK(s.resolve(1, steps, true) == 3.0f);
  CHECK(s.resolve(2, steps, true) == 3.0f);
  CHECK(s.resolve(3, steps, true) == 10.0f);
  CHECK(s.resolve(4, steps, true) == 10.0f);  // clamped to 10 first, then it is whole already
  CHECK(s.resolve(5, steps, true) == 0.0f);   // -0.4 is held to 0
  CHECK(s.resolve(6, wide, true) == -1.0f);   // -0.6 in -12..12: -1
  CHECK(s.resolve(7, wide, true) == 0.0f);
  // a float parameter is not rounded
  CHECK(s.resolve(0, desc(0.0f, 10.0f, 5.0f), false) == 2.4f);
  // a range that does not start at a whole number: the rounded value is held to it
  const core::ParamDesc odd = desc(0.5f, 3.5f, 1.5f);
  ParamSet o;
  CHECK(o.set_value(0, 0.6f));
  CHECK(o.set_value(1, 3.4f));
  CHECK(o.resolve(0, odd, true) == 1.0f);
  CHECK(o.resolve(1, odd, true) == 3.0f);
}

TEST_CASE("param set: fit_param_value is the clamp and the rounding the params page writes with") {
  const core::ParamDesc steps = desc(0.0f, 10.0f, 5.0f);
  CHECK(app::fit_param_value(steps, false, 4.4f) == 4.4f);
  CHECK(app::fit_param_value(steps, true, 4.4f) == 4.0f);
  CHECK(app::fit_param_value(steps, true, 99.0f) == 10.0f);
  CHECK(app::fit_param_value(steps, true, std::numeric_limits<float>::quiet_NaN()) == 5.0f);
  CHECK(app::fit_param_value(steps, false, -1.0f) == 0.0f);
}
