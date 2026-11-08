// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSFX-ABI2 A6: how the parameters page describes a sidechain KEY parameter (app/src/module_ui.cpp describe_module_param): an INT 0..16 that reads OFF, T1 .. T16, and that MIDI Learn never binds
// (the descriptor is not learnable, the rule param_actions.cpp's learn path applies). Honoured only when the caller says the module is an effect of ABI 1.12 or above; any other parameter is unchanged.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3module.h>

#include <o3os/app/module_ui.h>
#include <o3os/core/param.h>

#include <cstring>

namespace {

using namespace o3os;

const o3m_param kKey = {"Key", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT | O3M_PARAM_KEY};
const o3m_param kPlain = {"Mode", 0.0f, 16.0f, 0.0f, O3M_PARAM_INT};

}  // namespace

TEST_CASE("key param: a KEY parameter of an effect of ABI 1.12 reads OFF, T1 .. T16 and is not a Learn target") {
  app::ParamScratch s;
  NEED(app::describe_module_param(kKey, s, nullptr, true));
  CHECK(s.desc.unit == core::ParamUnit::Enum);
  CHECK(s.desc.list_count == 17);
  CHECK(s.desc.min == 0.0f);
  CHECK(s.desc.max == 16.0f);
  CHECK(s.whole);
  CHECK((s.desc.flags & core::kParamLearnable) == 0u);  // "NO TARGET TO LEARN" in the page
  CHECK(std::strcmp(s.desc.names[0], "OFF") == 0);
  CHECK(std::strcmp(s.desc.names[1], "T1") == 0);
  CHECK(std::strcmp(s.desc.names[3], "T3") == 0);
  CHECK(std::strcmp(s.desc.names[16], "T16") == 0);
  CHECK(core::param_desc_valid(s.desc));
}

TEST_CASE("key param: without the effect-of-1.12 flag the KEY bit is not honoured, and an ordinary INT parameter is as it was") {
  app::ParamScratch s;
  NEED(app::describe_module_param(kKey, s));  // the default: an instrument, an app, a module of 1.11 or below
  CHECK(s.desc.unit == core::ParamUnit::Int);
  CHECK(s.desc.names == nullptr);
  CHECK((s.desc.flags & core::kParamLearnable) != 0u);
  app::ParamScratch p;
  NEED(app::describe_module_param(kPlain, p, nullptr, true));
  CHECK(p.desc.unit == core::ParamUnit::Int);
  CHECK((p.desc.flags & core::kParamLearnable) != 0u);
}
