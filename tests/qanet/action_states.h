// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET A1 (docs/specs/OS-QANET.md part A): the states the rules of rules_test.cpp try every bound action in. Two tables, both data:
//   * context_reach(): per binding context, the scenario lines that bring the simulator to a state where the context is in the chain (a screen, a page, an overlay). The lines carry `assert state`
//     checks, so a reach that stops working fails the test with the line that broke, instead of exercising the wrong state.
//   * action_states(): per action, the states it has something to act on: a document, and the scenario lines that put a cursor on a file, select a slot, and so on. The order of a run: the document and the card,
//     `pre`, the seed of the undo history, the reach of the context, `steps`. Every action that a binding
//     reaches must have an entry (an action with no entry fails the test: a new action is added here, with the state where it does something). The entry says WHAT the action needs, never what the
//     test should find.
// Scenario lines are the grammar of sim/include/o3os/sim/scenario.h; the card is the test's own (see rules_test.cpp: kCard), a scratch copy that the simulator removes.
#pragma once

#include <o3os/app/bindings.h>

#include <vector>

namespace qanet {

enum class Doc { Demo, Sound, Tb303, DemoLearn };  // `document`, `document sound`, `document tb303`, `document bindings` (the demo with two MIDI Learn bindings)

// One state an action is tried in.
struct State {
  const char* name;   // short label: part of the finding's context column, so keep it stable
  Doc doc;
  const char* steps;  // scenario lines run after the context was reached ("" for none): a cursor on a file, a selection
  const char* pre = "";  // scenario lines run BEFORE the context is reached and before the undo history is seeded (a sample loaded, so that the sample screens have audio)
  const char* base = "";  // scenario lines that join the script of the document and the card, as ContextReach::base does (`audio on`: the engine, the transport, the perform board and the mixer board exist only with it). A state with `audio on` is not run in a context whose own reach has `apps on` (the simulator refuses the pair): the walk says so
};

struct ActionEntry {
  const char* action;           // the registry name
  std::vector<State> states;    // at least one; the action is run once in each, in every context that binds it
};

// A context that a test run cannot reach is listed with `steps == nullptr` and the reason in `skip`; it is counted and printed, never silently dropped.
struct ContextReach {
  o3os::app::Context context;
  const char* name;
  const char* steps;
  const char* skip;
  const char* base = "";  // scenario lines that must share the script of the document and the card (`audio on` needs the `document` line in its own script)
  const char* rules = nullptr;  // the reach of the rules walk (rules_test.cpp) alone, for a context that has no `steps` (the keys walk, keys_test.cpp, makes its states from `steps` and leaves these two contexts to its own pages)
};

const std::vector<ActionEntry>& action_states();
const std::vector<ContextReach>& context_reach();

}  // namespace qanet
