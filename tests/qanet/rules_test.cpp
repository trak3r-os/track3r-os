// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET A1 (docs/specs/OS-QANET.md part A, rule 1; new Community OS behaviour, test tooling only): every action that a built-in binding reaches is run in the simulator on a state where it has something to act
// on, and a change of the DOCUMENT that has neither a question first nor an undo unit is a finding. The oracle is the rule, not the code:
//
//   RULE 1. An action that changes the document must be undoable (it adds an undo unit), or must have asked a question first (the prompt of app/key_prompt.h was up when it ran: it is the answer). An action
//           that lowers the undo depth of any area is allowed only for File NEW and OPEN, and undo and redo (the sample editor's own undo included).
//
// What is walked: app::builtin_bindings(), one run per (context, action, argument); the action names come from app::action_registry(), the states from action_states.cpp (a context to reach, a document and
// the scenario lines the action needs). Nothing in this file knows an action by name except the five of the exemption above. The run: a fresh simulator, the document and a card (kCard below), three seed
// units on the undo rings (so that a cleared history shows as a lowered depth, and a unit added by the action is not merged into one of them), the context's reach lines, the action's state lines; then
// the action through Simulator::action (the argument of the binding; +1 for an encoder value, pad 0 for a pad); then the simulator runs until the document, the undo depths and the notifications stop moving.
// "Document" is the native project as formats/native writes it (the digest of `mark model`) and the PCM of every sample slot. A question is the prompt in force that the state line reports (`keys=`): when a
// run leaves one up, each of its answer keys is pressed in a fresh run of the same state, and the rule is applied to that second press with "asked first".
//
// The known bugs and every other finding are in baseline_rules.txt (one line each: rule | action | context | reason | ID or `new`). The test fails on a finding that is not in the baseline and on a baseline
// line that no longer occurs, so a fixed bug has to leave the file. allow_rules.txt has the same lines for a behaviour that is a decision (a reason per entry); it starts empty. QANET_WRITE_BASELINE=<path>
// writes the current findings in the baseline's format (the IDs and `new` marks of the lines that exist are kept).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_states.h"
#include "rig.h"
#include "rules.h"

#include <o3os/app/actions.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/overlay.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/view_state.h>
#include <o3os/app/state_line.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/dir_card.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/scenario_assert.h>
#include <o3os/sim/simulator.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace o3os;
using namespace qanet;

namespace {

// ---- Binding names -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

std::string input_name(core::InputRef r) {
  switch (r.kind) {
    case core::InputKind::Button:
      for (const app::ButtonName& b : app::kButtonNames) {
        if (static_cast<std::uint8_t>(b.button) == r.id) {
          std::string n = b.name;
          if (n.size() == 2 && n[0] == 'f') {
            n[0] = 'F';
          } else if (!n.empty()) {
            n[0] = static_cast<char>(n[0] - 'a' + 'A');
          }
          return n;
        }
      }
      return "button" + std::to_string(r.id);
    case core::InputKind::Pad:
      return "pad";
    case core::InputKind::Encoder:
      return "encoder";
    default:
      return "?";
  }
}

std::string trigger_name(const core::Trigger& t) {
  static const char* const kinds[] = {"tap", "hold", "doubletap", "hold_encoder", "chord", "hold_end"};
  std::string s = kinds[static_cast<unsigned>(t.gesture)];
  s += " " + input_name(t.input);
  if (!core::is_none(t.modifier)) {
    const std::string m = input_name(t.modifier);
    s += (t.gesture == core::GestureKind::Chord || t.gesture == core::GestureKind::HoldEncoder) ? " " + m : " +" + m;
  }
  return s;
}

// One run: a binding row of the table (the same action in the same context with the same argument is one run, however many gestures reach it).
struct Row {
  app::Context context;
  std::string action;
  core::ActionId id;
  core::ActionArg arg;
  std::vector<std::string> triggers;
};

std::string arg_text(const Row& r) { return r.arg == 0 ? r.action : r.action + " " + std::to_string(r.arg); }

// A change of the pattern data that no unit records (the Editor told to keep no history for it): one of the classes the undo policy covers (rules.h, rule 1).
void plant_silent_step(sim::Simulator& s) {
  model::Editor* ed = s.app().session().state_editor();
  const model::Pattern* pat = s.project()->pattern(1);
  model::Step st = pat->steps[0][0];
  st.note = st.note == 60 ? 61 : 60;
  ed->set_final(true);
  (void)ed->set_step(1, 0, 0, st);
  ed->set_final(false);
}

// ---- The rig -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

const qanet::State kDefaultState{"default", qanet::Doc::Demo, ""};

// Puts a unit on each undo ring, so that a history that an action clears shows as a lowered depth, and a unit that it adds is not a merge into an older one.
bool seed_history(sim::Simulator& s, std::string& error) {
  model::Editor* ed = s.app().session().state_editor();
  const model::Project* p = s.project();
  if (ed == nullptr || p == nullptr) {
    error = "no document to seed";
    return false;
  }
  if (const model::Pattern* pat = p->pattern(1)) {     // the pattern ring: the last step of the first track
    model::Step st = pat->steps[0][pat->track_length[0] - 1u];
    st.note = st.note == 60 ? 61 : 60;
    (void)ed->set_step(1, 0, pat->track_length[0] - 1u, st);
  }
  (void)ed->append_order(1);                           // the song ring
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    if (p->instrument(i) != nullptr) {
      (void)ed->set_instrument_name(i, "SEED", 4);     // the sound ring
      break;
    }
  }
  for (unsigned i = 0; i < kRings; ++i) {
    static const model::UndoArea areas[kRings] = {model::UndoArea::Pattern, model::UndoArea::Song, model::UndoArea::Sound};
    if (ed->journal(areas[i]).depth() == 0) {
      error = "the seed left undo ring " + std::to_string(i) + " empty";
      return false;
    }
  }
  return true;
}

struct Spec {
  qanet::Doc doc = qanet::Doc::Demo;
  std::string base;  // lines of the base script after the card (`audio on`)
  std::string pre;   // lines before the seed
  std::string reach;
  std::string steps;
};

// A fresh simulator in the state of `spec`. false: `error` says which part did not hold.
bool prepare(std::unique_ptr<sim::Simulator>& out, const Spec& spec, const std::string& card_dir, std::string& error) {
  out = std::make_unique<sim::Simulator>();
  const char* doc = spec.doc == qanet::Doc::Sound ? "document sound\n" : spec.doc == qanet::Doc::Tb303 ? "document tb303\n" : spec.doc == qanet::Doc::DemoLearn ? "document bindings\n" : "document\n";
  if (!run_text(*out, std::string(doc) + "wait 5\ncard \"" + card_dir + "\"\nwait 120\n" + spec.base, error)) {
    error = "base: " + error;
    return false;
  }
  if (!run_text(*out, spec.pre, error)) {
    error = "pre: " + error;
    return false;
  }
  if (!seed_history(*out, error)) {
    return false;
  }
  if (!run_text(*out, spec.reach + "wait 20\n", error)) {
    error = "reach: " + error;
    return false;
  }
  if (!run_text(*out, spec.steps + "wait 20\n", error)) {
    error = "steps: " + error;
    return false;
  }
  return true;
}

struct Step {
  Snap before, after;
  std::string reason;  // rule 1's
};

// The answer keys of a prompt ("F3:ALL,F1:KEEP_INS,SF7:PURGE?"): the scenario lines that press each one.
// `skipped` counts the tokens that are not an F or Shift+F key (a pad, a button): the walk cannot press those and says how many it left.
std::vector<std::pair<std::string, std::string>> answer_keys(const std::string& keys, unsigned& skipped) {
  std::vector<std::pair<std::string, std::string>> out;  // (name, scenario text)
  std::size_t from = 0;
  while (from < keys.size()) {
    std::size_t comma = keys.find(',', from);
    if (comma == std::string::npos) {
      comma = keys.size();
    }
    std::string token = keys.substr(from, comma - from);
    from = comma + 1;
    const std::size_t colon = token.find(':');
    const std::string key = token.substr(0, colon);
    if (key.size() >= 2 && key[0] == 'S' && key[1] == 'F') {
      out.emplace_back("Shift+F" + key.substr(2), "press shift\ntap f" + key.substr(2) + "\nrelease shift\n");
    } else if (key.size() >= 2 && key[0] == 'F') {
      out.emplace_back(key, "tap f" + key.substr(1) + "\n");
    } else {
      ++skipped;
    }
  }
  return out;
}

// ---- The walk ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Report {
  std::vector<Line> findings;
  std::vector<std::string> setup_errors;
  std::vector<std::string> not_exercised;  // contexts, with the reason
  std::set<std::string> changed;           // the actions that changed the document in at least one run
  std::set<std::string> answered;          // the actions that answered DispatchResult::Changed in at least one run
  unsigned bindings = 0, rows = 0, runs = 0, answers = 0;
  unsigned question_moved = 0;             // answer runs where the same action asked a different question the second time: nothing to compare, not pressed
  unsigned answer_skipped = 0;             // answer keys of a question that are not an F or Shift+F key: not pressed
};

const qanet::ActionEntry* entry_of(const std::vector<qanet::ActionEntry>& table, const std::string& action) {
  for (const qanet::ActionEntry& e : table) {
    if (action == e.action) {
      return &e;
    }
  }
  return nullptr;
}

// The scenario lines of a reach for this walk: its `steps`, else the `rules` reach of a context that only this walk reaches (nullptr: not reached).
const char* reach_steps(const qanet::ContextReach& r) noexcept { return r.steps != nullptr ? r.steps : r.rules; }

const qanet::ContextReach* reach_of(const std::vector<qanet::ContextReach>& table, app::Context c) {
  for (const qanet::ContextReach& r : table) {
    if (r.context == c) {
      return &r;
    }
  }
  return nullptr;
}

// The rows of the built-in bindings that a run covers, and the actions the bindings name that the registry does not know or the table has no state for (`gaps`).
std::vector<Row> rows_of(const core::BindingView& view, const app::ActionTable& registry, unsigned& bindings, std::vector<std::string>& gaps) {
  std::vector<Row> rows;
  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < view.count; ++i) {
    const core::Binding& b = view.items[i];
    ++bindings;
    const app::ActionDef* def = nullptr;
    for (std::size_t k = 0; k < registry.count; ++k) {
      if (registry.items[k].id == b.action) {
        def = &registry.items[k];
      }
    }
    if (b.action == core::action_id("ui.none")) {
      continue;  // "explicitly nothing here"
    }
    if (def == nullptr) {
      gaps.push_back("a binding names an action that the registry does not have (id " + std::to_string(static_cast<std::uint32_t>(b.action)) + ")");
      continue;
    }
    core::ActionArg arg = b.arg;
    if (b.arg_from == core::ArgFrom::Value) {
      arg = 1;
    } else if (b.arg_from == core::ArgFrom::InputId) {
      arg = b.arg;  // the argument of a press of pad 0: the binding adds the input's id (core::binding_arg), so a binding with an offset (instrument.select on a pad: +1) is run with its offset, not with 0
    }
    const std::string key = std::to_string(b.context) + "/" + def->name + "/" + std::to_string(arg);
    auto it = index.find(key);
    if (it == index.end()) {
      index[key] = rows.size();
      rows.push_back(Row{static_cast<app::Context>(b.context), def->name, b.action, arg, {}});
      it = index.find(key);
    }
    rows[it->second].triggers.push_back(trigger_name(b.trigger));
  }
  return rows;
}

app::DispatchResult last_result = app::DispatchResult::Unknown;  // what the last real action answered (QANET_VERBOSE prints it)

// The walk of A1. `act` runs the action of a row (the real one: Simulator::action); the self-test hands in a planted one.
using Act = std::function<void(sim::Simulator&, const Row&)>;

void walk_row(const Row& row, const qanet::State& state, const qanet::ContextReach& reach, const std::string& card_dir, const Act& act, Report& report) {
  const std::string context = std::string(reach.name) + " " + [&] {
    std::string t;
    for (std::size_t i = 0; i < row.triggers.size(); ++i) {
      t += (i == 0 ? "" : " / ") + row.triggers[i];
    }
    return t;
  }() + " [" + state.name + "]";
  Spec spec;
  spec.doc = state.doc;
  spec.base = std::string(reach.base) + state.base;
  spec.reach = reach_steps(reach) != nullptr ? reach_steps(reach) : "";
  spec.pre = state.pre;
  spec.steps = state.steps;
  if (std::string(state.base).find("audio on") != std::string::npos && spec.reach.find("apps on") != std::string::npos) {
    const std::string note = std::string(reach.name) + ": the state [" + state.name + "] has the engine on and this context's reach has `apps on` (the simulator refuses the pair)";
    if (std::find(report.not_exercised.begin(), report.not_exercised.end(), note) == report.not_exercised.end()) {
      report.not_exercised.push_back(note);
    }
    return;
  }

  std::unique_ptr<sim::Simulator> s;
  std::string error;
  if (!prepare(s, spec, card_dir, error)) {
    report.setup_errors.push_back(context + ": " + error);
    return;
  }
  if (!context_live(*s, row.context)) {
    report.setup_errors.push_back(context + ": the context is not in the chain after the reach and the state lines");
    return;
  }
  ++report.runs;
  const Snap before = snap_of(*s);
  last_result = app::DispatchResult::Unknown;
  act(*s, row);
  settle(*s);
  const Snap after = snap_of(*s);
  if (before.digest != after.digest) {
    report.changed.insert(row.action);
  }
  if (last_result == app::DispatchResult::Changed) {
    report.answered.insert(row.action);
  }
  if (std::getenv("QANET_VERBOSE") != nullptr) {  // one line per run: what the action answered, whether the document moved, the units before and after, the strip
    static const char* const names[] = {"Unknown", "Disabled", "Unchanged", "Changed"};
    std::printf("run %s | %s | %s | doc %s | units %u -> %u | msg=%s\n", arg_text(row).c_str(), context.c_str(), names[static_cast<unsigned>(last_result)], before.digest != after.digest ? "CHANGED" : "same", before.total(), after.total(), after.msg.c_str());
  }
  for (const Finding& f : check_rules(before, after, row.action)) {
    report.findings.push_back(Line{f.rule, arg_text(row), context, f.reason, "new"});
  }
  // A question left up: press each of its answer keys, in a fresh run of the same state, with "asked first".
  if (question_up(after)) {
    for (const auto& [name, press] : answer_keys(after.keys, report.answer_skipped)) {
      std::unique_ptr<sim::Simulator> t;
      if (!prepare(t, spec, card_dir, error)) {
        report.setup_errors.push_back(context + " > " + name + ": " + error);
        continue;
      }
      last_result = app::DispatchResult::Unknown;
      act(*t, row);
      settle(*t);
      const Snap asked = snap_of(*t);
      if (last_result == app::DispatchResult::Changed) {
        report.answered.insert(row.action);
      }
      if (asked.keys != after.keys) {
        ++report.question_moved;  // not the same question this time: nothing to compare
        continue;
      }
      if (!run_text(*t, press, error)) {
        report.setup_errors.push_back(context + " > " + name + ": " + error);
        continue;
      }
      settle(*t);
      const Snap answered = snap_of(*t);
      ++report.answers;
      if (asked.digest != answered.digest) {
        report.changed.insert(row.action);
      }
      for (const Finding& f : check_rules(asked, answered, row.action, true)) {
        report.findings.push_back(Line{f.rule, arg_text(row) + " > " + name, context, f.reason, "new"});
      }
    }
  }
}

// QANET_SHARD="k/n" (k from 1): this run walks the contexts whose place in context_reach() is k - 1 modulo n, so that ctest can run the walk as n tests in parallel. "1/1" without the variable.
struct Shard {
  unsigned k = 1, n = 1;
  bool has(const std::string& context_name) const {
    const auto& table = qanet::context_reach();
    for (std::size_t i = 0; i < table.size(); ++i) {
      if (context_name == table[i].name) {
        return i % n == k - 1u;
      }
    }
    return k == 1u;  // a context the table does not know is reported by shard 1
  }
};

Shard shard_from_env() {
  Shard sh;
  if (const char* v = std::getenv("QANET_SHARD")) {
    unsigned k = 0, n = 0;
    if (std::sscanf(v, "%u/%u", &k, &n) == 2 && n >= 1 && k >= 1 && k <= n) {
      sh.k = k;
      sh.n = n;
    }
  }
  return sh;
}

// Whether a run saw `action` act: the document changed, or (for an action that does not edit the document: no undoable flag) the dispatcher answered Changed. An action flagged undoable edits the document, so
// only a changed document counts for it.
bool exercised(const Report& report, const std::string& action, bool undoable) {
  return report.changed.count(action) != 0 || (!undoable && report.answered.count(action) != 0);
}

bool undoable_flag(const app::ActionTable& registry, core::ActionId id) {
  for (std::size_t k = 0; k < registry.count; ++k) {
    if (registry.items[k].id == id) {
      return (registry.items[k].flags & (core::kActionUndoable | core::kActionUndoableIfOpen)) != 0;
    }
  }
  return false;
}

// `gap_pass`: the pass of the gap list (never_changed.txt): every context (no shard), and a row whose action has already been seen to act is not run again, so that the pass costs what the actions that never act cost.
Report walk(const core::BindingView& view, const std::string& card_dir, bool gap_pass = false) {
  Report report;
  const app::ActionTable registry = app::action_registry();
  const auto& states = qanet::action_states();
  const auto& reaches = qanet::context_reach();
  std::vector<std::string> gaps;
  const std::vector<Row> rows = rows_of(view, registry, report.bindings, gaps);
  report.rows = static_cast<unsigned>(rows.size());
  for (const std::string& g : gaps) {
    report.setup_errors.push_back(g);
  }
  std::set<std::string> skipped;
  const Act act = [](sim::Simulator& s, const Row& r) { last_result = s.action(r.id, r.arg); };
  const char* only = std::getenv("QANET_ONLY");  // while tuning a state: only the actions whose name contains this (the baseline check then fails, as it should)
  const Shard shard = gap_pass ? Shard{} : shard_from_env();
  for (const Row& row : rows) {
    if (only != nullptr && row.action.find(only) == std::string::npos) {
      continue;
    }
    if (gap_pass && exercised(report, row.action, undoable_flag(registry, row.id))) {
      continue;
    }
    if (const qanet::ContextReach* r = reach_of(reaches, row.context); r != nullptr && !shard.has(r->name)) {
      continue;
    }
    const qanet::ActionEntry* entry = entry_of(states, row.action);
    if (entry == nullptr) {
      report.setup_errors.push_back("no entry in action_states.cpp for the action " + row.action + " (add the state where it has something to act on)");
      continue;
    }
    const qanet::ContextReach* reach = reach_of(reaches, row.context);
    if (reach == nullptr) {
      report.setup_errors.push_back("no entry in context_reach() for the context " + std::to_string(static_cast<unsigned>(row.context)) + " of " + row.action);
      continue;
    }
    if (reach_steps(*reach) == nullptr) {
      if (skipped.insert(reach->name).second) {
        report.not_exercised.push_back(std::string(reach->name) + ": " + reach->skip);
      }
      continue;
    }
    for (const qanet::State& st : entry->states) {
      walk_row(row, st, *reach, card_dir, act, report);
    }
  }
  return report;
}

// The actions that no run of `report` saw act (a pass with `gaps`): the table has no state where they have something to do, or the context that binds them is not reached. A gap of the harness, not a finding.
std::set<std::string> gap_list(const Report& report, const core::BindingView& view) {
  const app::ActionTable registry = app::action_registry();
  std::set<std::string> out;
  for (std::size_t i = 0; i < view.count; ++i) {
    if (view.items[i].action == core::action_id("ui.none")) {
      continue;
    }
    for (std::size_t k = 0; k < registry.count; ++k) {
      if (registry.items[k].id == view.items[i].action && !exercised(report, registry.items[k].name, undoable_flag(registry, view.items[i].action))) {
        out.insert(registry.items[k].name);
      }
    }
  }
  return out;
}

}  // namespace

// ---- The test ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("qanet rules: every bound action against the destructive-change rule (baseline of known findings)") {
  const auto t0 = std::chrono::steady_clock::now();
  Card card;
  CHECK_MESSAGE(card.ok, "the card fixture could not be made (is the target e2e_card_fixture built?)");
  if (!card.ok) {
    return;
  }
  const Report report = walk(app::builtin_bindings(), card.dir.path());

  std::vector<Line> baseline, allow;
  std::string text, error;
  CHECK_MESSAGE(read_file(O3_QANET_BASELINE, text), "cannot read " << O3_QANET_BASELINE);
  CHECK_MESSAGE(parse_lines(text, baseline, error), "baseline_rules.txt: " << error);
  text.clear();
  CHECK_MESSAGE(read_file(O3_QANET_ALLOW, text), "cannot read " << O3_QANET_ALLOW);
  CHECK_MESSAGE(parse_lines(text, allow, error), "allow_rules.txt: " << error);

  if (const char* out = std::getenv("QANET_WRITE_BASELINE")) {
    std::map<std::string, std::string> ids;
    for (const Line& l : baseline) {
      ids[l.key()] = l.id;
    }
    std::ofstream f(out);
    for (Line l : report.findings) {
      const auto it = ids.find(l.key());
      l.id = it != ids.end() ? it->second : "new";
      f << l.text() << "\n";
    }
  }

  const Shard shard = shard_from_env();  // a shard holds the baseline lines of its own contexts (the first word of the context column)
  auto mine = [&](std::vector<Line>& lines) {
    std::vector<Line> kept;
    for (const Line& l : lines) {
      if (shard.has(l.context.substr(0, l.context.find(' ')))) {
        kept.push_back(l);
      }
    }
    lines = kept;
  };
  mine(baseline);
  allow.erase(std::remove_if(allow.begin(), allow.end(), [](const Line& l) { return l.rule != "R1" && l.rule != "R0"; }), allow.end());  // the file is shared: the other rules' lines are theirs
  mine(allow);
  const Verdict v = compare(report.findings, baseline, allow);
  unsigned fresh = 0;
  for (const Line& l : baseline) {
    fresh += l.id == "new" ? 1u : 0u;
  }
  const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  std::printf("qanet rules: %u bindings, %u rows, %u runs, %u answer presses, %zu findings (%zu in the baseline, %u of them `new`), %zu contexts not exercised, %ld ms\n", report.bindings, report.rows,
              report.runs, report.answers, report.findings.size(), baseline.size(), fresh, report.not_exercised.size(), ms);
  for (const std::string& n : report.not_exercised) {
    std::printf("  not exercised: %s\n", n.c_str());
  }
  std::printf("  answer runs skipped: %u asked a different question the second time; %u answer keys are not F keys\n", report.question_moved, report.answer_skipped);
  CHECK_MESSAGE(report.setup_errors.empty(), "setup problems (a state that did not hold, or a table without an entry):" << join(report.setup_errors));
  CHECK_MESSAGE(v.bad.empty(), "the baseline or the allow list is malformed:" << join(v.bad));
  CHECK_MESSAGE(v.unknown.empty(), "findings that are not in baseline_rules.txt (fix the bug, or add the line, marked `new`, for the maintainer's triage):" << join(v.unknown));
  CHECK_MESSAGE(v.stale.empty(), "baseline lines that no longer occur (the bug is fixed: delete the line):" << join(v.stale));
}

// ---- The gap list --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// never_changed.txt: `count: N` and then one action per line (blank lines and `#` comments are skipped). false: `error` says what is wrong with the file.
bool parse_gaps(const std::string& text, std::set<std::string>& out, unsigned& count, std::string& error) {
  std::istringstream in(text);
  std::string raw;
  bool have_count = false;
  while (std::getline(in, raw)) {
    const std::string t = trim(raw);
    if (t.empty() || t[0] == '#') {
      continue;
    }
    if (t.compare(0, 6, "count:") == 0) {
      count = static_cast<unsigned>(std::strtoul(trim(t.substr(6)).c_str(), nullptr, 10));
      have_count = true;
    } else if (!out.insert(t).second) {
      error = "twice in the list: " + t;
      return false;
    }
  }
  if (!have_count) {
    error = "no `count: N` line";
    return false;
  }
  if (count != out.size()) {
    error = "`count: " + std::to_string(count) + "` but " + std::to_string(out.size()) + " actions are listed";
    return false;
  }
  return true;
}

// The two lists of a gap check: what the pass found that the file does not list (the list grew: a new action with no state, or a state that stopped working), and what the file lists that now acts (delete the line).
void compare_gaps(const std::set<std::string>& found, const std::set<std::string>& listed, std::vector<std::string>& grown, std::vector<std::string>& closed) {
  for (const std::string& a : found) {
    if (listed.count(a) == 0) {
      grown.push_back(a);
    }
  }
  for (const std::string& a : listed) {
    if (found.count(a) == 0) {
      closed.push_back(a);
    }
  }
}

}  // namespace

// An action that no run ever saw act is a hole in this net (its rule is never asked); the holes are listed and counted, so that none is added without being seen, and one that is closed leaves the list. Every
// context in one pass (the shards see some of an action's rows only), and an action that has been seen to act is not run again. QANET_WRITE_GAPS=<path> writes the list.
TEST_CASE("qanet rules gaps: the actions that no run saw act are the ones in never_changed.txt") {
  Card card;
  CHECK_MESSAGE(card.ok, "the card fixture could not be made (is the target e2e_card_fixture built?)");
  if (!card.ok) {
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const core::BindingView view = app::builtin_bindings();
  const Report report = walk(view, card.dir.path(), true);
  const std::set<std::string> found = gap_list(report, view);
  if (const char* out = std::getenv("QANET_WRITE_GAPS")) {
    std::ofstream f(out);
    f << "# OS-QANET A1: actions that no run of the walk saw act (the document changed, or for an action that does not edit it, the dispatcher answered Changed): the table has no state where they have something\n"
         "# to do, or the context that binds them is not reached. Each is a hole in the net (rule 1 is never asked of it), not a finding. The test fails when this list grows and when an action on it starts\n"
         "# to act (delete its line). Written by QANET_WRITE_GAPS.\n"
      << "count: " << found.size() << "\n";
    for (const std::string& a : found) {
      f << a << "\n";
    }
  }
  std::string text, error;
  std::set<std::string> listed;
  unsigned count = 0;
  CHECK_MESSAGE(read_file(O3_QANET_GAPS, text), "cannot read " << O3_QANET_GAPS);
  CHECK_MESSAGE(parse_gaps(text, listed, count, error), "never_changed.txt: " << error);
  std::vector<std::string> grown, closed;
  compare_gaps(found, listed, grown, closed);
  const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  std::printf("qanet gaps: %zu actions never seen to act (%zu listed), %u runs, %ld ms\n", found.size(), listed.size(), report.runs, ms);
  CHECK_MESSAGE(report.setup_errors.empty(), "setup problems:" << join(report.setup_errors));
  CHECK_MESSAGE(grown.empty(), "actions that no run saw act and that never_changed.txt does not list (give them a state in action_states.cpp, or list them):" << join(grown));
  CHECK_MESSAGE(closed.empty(), "actions on never_changed.txt that now act (delete the lines and lower the count):" << join(closed));
}

// ---- Self-tests ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// A planted action over one run of the Pattern screen: `edit` makes the change that the action under test would.
Report plant(const Card& card, const std::function<void(sim::Simulator&)>& edit) {
  Report report;
  const Row row{app::Context::Pattern, "plant.fault", core::action_id("nav.left"), 0, {"tap F1"}};
  const qanet::ContextReach reach{app::Context::Pattern, "Pattern", "tap pattern\n", nullptr};
  const Act act = [&](sim::Simulator& s, const Row&) { edit(s); };
  walk_row(row, kDefaultState, reach, card.dir.path(), act, report);
  return report;
}

}  // namespace

TEST_CASE("qanet rules self-test: a planted fault is caught, a clean change and an answered question are not") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  // A change of the document that no unit records: the project's tempo set behind the editor's back (a state setter, not journalled).
  Report silent = plant(card, [](sim::Simulator& s) {
    plant_silent_step(s);
  });
  CHECK(silent.setup_errors.empty());
  CHECK(silent.findings.size() == 1u);
  if (!silent.findings.empty()) {
    CHECK(silent.findings[0].reason == "document changed with no question and no undo unit");
  }

  // The same with the history gone: the reason says so.
  Report cleared = plant(card, [](sim::Simulator& s) {
    plant_silent_step(s);
    s.app().session().state_editor()->history_reset();
  });
  CHECK(cleared.findings.size() == 1u);
  if (!cleared.findings.empty()) {
    CHECK(cleared.findings[0].reason == "document changed with no question and no undo unit; undo history cleared");
  }

  // The project-level parameters are in the classes too: the tempo and the swing set behind the editor's back (no unit) are findings, as one through the editor (below) is not.
  Report tempo = plant(card, [](sim::Simulator& s) {
    model::Editor* ed = s.app().session().state_editor();
    ed->set_final(true);
    (void)ed->set_tempo_x100(s.project()->tempo_x100() + 500u);
    ed->set_final(false);
  });
  CHECK(tempo.setup_errors.empty());
  CHECK(tempo.findings.size() == 1u);
  if (!tempo.findings.empty()) {
    CHECK(tempo.findings[0].reason == "document changed with no question and no undo unit");
  }
  Report swing = plant(card, [](sim::Simulator& s) {
    model::Editor* ed = s.app().session().state_editor();
    ed->set_final(true);
    (void)ed->set_swing(s.project()->swing() == 33 ? 40u : 33u);
    ed->set_final(false);
  });
  CHECK(swing.findings.size() == 1u);

  // A history cleared with the document untouched is a finding of its own.
  Report lowered = plant(card, [](sim::Simulator& s) { s.app().session().state_editor()->history_reset(); });
  CHECK(lowered.findings.size() == 1u);
  if (!lowered.findings.empty()) {
    CHECK(lowered.findings[0].reason == "undo history lowered");
  }

  // A change through the editor is one unit: no finding.
  Report unit = plant(card, [](sim::Simulator& s) { (void)s.app().session().state_editor()->set_swing(33); });
  CHECK(unit.setup_errors.empty());
  CHECK(unit.findings.empty());

  // Nothing changed: no finding.
  Report idle = plant(card, [](sim::Simulator&) {});
  CHECK(idle.findings.empty());

  // The rules themselves, on snapshots: an answered question may change the document with no unit; an exempt action may clear the history.
  auto reason = [](const Snap& x, const Snap& y, const char* action, bool answer = false) {
    const std::vector<Finding> f = check_rules(x, y, action, answer);
    return f.empty() ? std::string() : f[0].reason;
  };
  Snap a, b;
  a.digest = "x";
  b.digest = "y";
  b.pattern_hash = 1;  // (a change of the pattern data: one of the three classes the undo policy covers)
  a.ring[0] = a.added[0] = 1;
  b.ring[0] = b.added[0] = 0;
  Snap asked = a;
  asked.keys = "F1:ALL";
  asked.overlay = "NOTIFY";
  CHECK(reason(asked, b, "sample.load", true) == "undo history lowered");
  CHECK(reason(asked, b, "sample.load") == "document changed with no question and no undo unit; undo history cleared");  // the question was up, but this action is not its answer
  CHECK(reason(a, b, "sample.load", true) == "document changed with no question and no undo unit; undo history cleared");
  CHECK(reason(a, b, "file.new").empty());
  CHECK(reason(a, b, "edit.undo").empty());
  b.ring[0] = b.added[0] = 2;
  CHECK(reason(a, b, "sample.load").empty());
  b.ring[0] = b.added[0] = 1;
  CHECK(reason(asked, b, "sample.load", true).empty());
  asked.overlay = "LEARN";  // a dialog's keys are not a question
  CHECK(reason(asked, b, "sample.load", true) == "document changed with no question and no undo unit");

  // The undo policy (2026-10-22): what a step only adds to an empty slot, and a change outside the three classes, is exempt; a change to what was there is not; Mixer RESET asks.
  {
    Snap p0, p1;
    p0.digest = "x";
    p1.digest = "y";
    p0.ring[0] = p0.added[0] = 3;
    p1.ring[0] = p1.added[0] = 0;  // the load cleared the history
    p1.inst_present[4] = true;  // a new instrument in an empty slot, with its audio
    p1.inst_hash[4] = 99;
    p1.pcm_frames[4] = 1000;
    p1.pcm_hash[4] = 7;
    CHECK(reason(p0, p1, "sample.load").empty());  // a load into a free slot: exempt, even with the history cleared
    p0.inst_present[2] = true;  // an instrument that was there
    p0.inst_hash[2] = 5;
    p1.inst_present[2] = true;
    p1.inst_hash[2] = 5;
    CHECK(reason(p0, p1, "sample.load").empty());  // (untouched)
    p1.inst_hash[2] = 6;  // changed: an instrument or its parameters
    CHECK(reason(p0, p1, "param.set") == "document changed with no question and no undo unit; undo history cleared");
    p1.inst_hash[2] = 5;
    p0.pcm_frames[2] = 500;  // the audio of a used slot replaced: a load over it asks (or leaves a unit)
    p0.pcm_hash[2] = 3;
    p1.pcm_frames[2] = 800;
    p1.pcm_hash[2] = 4;
    CHECK(reason(p0, p1, "sample.load") == "document changed with no question and no undo unit; undo history cleared");
    Snap q0 = p0;
    q0.keys = "F1:ALL";
    q0.overlay = "NOTIFY";
    CHECK(reason(q0, p1, "sample.load", true) == "undo history lowered");  // asked first: the change is fine, the cleared history is the load's own
    p1.pcm_frames[2] = 500;
    p1.pcm_hash[2] = 3;
    p1.inst_present[4] = false;
    p1.pcm_frames[4] = 0;
    // the Mixer: exempt (levels, mutes and solos, a reset of one level), except on STEMS Delete and Shift + Delete, which ask
    Snap m0, m1;
    m0.digest = "x";
    m1.digest = "y";
    CHECK(reason(m0, m1, "mixer.edit").empty());
    CHECK(reason(m0, m1, "mixer.mute_none").empty());
    CHECK(reason(m0, m1, "mixer.reset").empty());  // a reset of one level: exempt
    m0.screen = "MIXER";
    m0.fields["page"] = "3";  // STEMS: Delete and Shift + Delete ask
    CHECK(reason(m0, m1, "mixer.reset") == "document changed with no question and no undo unit");
    Snap mq = m0;
    mq.keys = "F1:ALL";
    mq.overlay = "NOTIFY";
    CHECK(reason(mq, m1, "mixer.reset", true).empty());
    // a project-level parameter (tempo, swing, note, mute mode, scenes) is a parameter change: silent is a finding
    Snap t0, t1;
    t0.digest = "x";
    t1.digest = "y";
    t1.project_params = 1;
    CHECK(reason(t0, t1, "tempo.edit") == "document changed with no question and no undo unit");
    // an exempt change (the Mixer) may not lower the undo depth, unless the step added to a free slot
    Snap h0, h1;
    h0.digest = "x";
    h1.digest = "y";
    h0.ring[0] = h0.added[0] = 3;
    h1.ring[0] = h1.added[0] = 0;
    CHECK(reason(h0, h1, "mixer.edit") == "undo history lowered");
    h1.inst_present[4] = true;
    CHECK(reason(h0, h1, "mixer.edit").empty());
  }

  // A full ring: the new unit evicts the oldest, the depth stays (or falls when a big unit evicts several) and the count of units added moves up by one. Not a missing unit, not a lowered history.
  Snap full0, full1;
  full0.digest = "x";
  full1.digest = "y";
  full1.pattern_hash = 1;
  full0.ring[1] = 10;
  full0.added[1] = 10 + 5;  // 5 dropped before
  full1.ring[1] = 10;
  full1.added[1] = 10 + 6;
  CHECK(reason(full0, full1, "pattern.set").empty());
  full1.ring[1] = 8;  // a big unit evicted three: depth 10 - 3 + 1 = 8, dropped 5 + 3
  full1.added[1] = 8 + 8;
  CHECK(reason(full0, full1, "pattern.set").empty());
  full1.ring[1] = 0;  // the history cleared (a unit too big for the ring): depth 0, dropped as it was
  full1.added[1] = 5;
  CHECK(reason(full0, full1, "pattern.set") == "document changed with no question and no undo unit; undo history cleared");

  // A reopened click group: the document changed, the depth did not, the merge key was up and the parameter it names has a new value: one unit, edited. The key alone is not a merge: a keyed edit followed by a
  // silent change (a mixer move behind the journal) leaves the key up and the keyed value where it was, and that is a finding.
  Snap m0, m1;
  m0.digest = "x";
  m1.digest = "y";
  m1.pattern_hash = 1;
  m0.ring[0] = m1.ring[0] = m0.added[0] = m1.added[0] = 3;
  m0.merge[0] = m1.merge[0] = 77;
  m0.keyed_key[0] = m1.keyed_key[0] = 77;
  m0.keyed_ok[0] = m1.keyed_ok[0] = true;
  m0.keyed[0] = 0.25f;
  m1.keyed[0] = 0.5f;
  CHECK(reason(m0, m1, "param.set").empty());
  m1.keyed[0] = 0.25f;  // the keyed parameter did not move: something else changed the document
  CHECK(reason(m0, m1, "param.set") == "document changed with no question and no undo unit");
  m1.keyed[0] = 0.5f;
  m1.ring[0] = 2;  // the sweep came back to where it began: the unit is gone, which is not a lowered history
  m1.added[0] = 2;
  m1.merge[0] = 0;
  m1.keyed_key[0] = 77;
  CHECK(reason(m0, m1, "param.set").empty());
  m1.ring[0] = m1.added[0] = 0;  // but a whole history gone is
  CHECK(reason(m0, m1, "param.set") == "document changed with no question and no undo unit; undo history cleared");
  m0.merge[0] = m1.merge[0] = 0;
  m0.keyed_ok[0] = m1.keyed_ok[0] = false;
  m1.ring[0] = m1.added[0] = 3;
  CHECK(reason(m0, m1, "param.set") == "document changed with no question and no undo unit");

  // A sample editor whose history is full cannot show an edit as a deeper history: the edit counts as a unit.
  Snap e0, e1;
  e0.digest = "x";
  e1.digest = "y";
  e1.pattern_hash = 1;
  e0.sedit = e1.sedit = app::SampleBank::kHistory;
  e0.sedit_full = e1.sedit_full = true;
  e0.sedit_slot = e1.sedit_slot = 2;
  // B8: ... but only a change confined to the history's own slot. A silent change of the pattern while the history is full used to be credited (the blind spot); now it is judged.
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit");
  e1.pattern_hash = 0;
  e1.pcm_frames[2] = 100;  // the edit: the history slot's audio (and its instrument)
  e1.pcm_hash[2] = 7;
  e1.inst_present[2] = e0.inst_present[2] = true;
  e1.inst_hash[2] = 9;
  CHECK(reason(e0, e1, "sedit.cut").empty());
  e1.sedit = 2;  // the bank freed the oldest step for room (edit_begin on a large sample): the new step is there, older ones gone
  CHECK(reason(e0, e1, "sedit.cut").empty());
  e1.sedit = 0;  // ... but a history that is gone is not
  e1.sedit_slot = -1;
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit; undo history cleared");
  e1.sedit = app::SampleBank::kHistory;
  e1.sedit_slot = 2;
  e1.pcm_frames[3] = 5;  // the audio of ANOTHER slot changed too: not an edit of this screen
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit");
  e1.pcm_frames[3] = 0;
  e1.inst_present[4] = true;  // ... or another instrument
  e1.inst_hash[4] = 3;
  e0.inst_present[4] = true;
  e0.inst_hash[4] = 1;
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit");
  e1.inst_hash[4] = 1;
  e1.project_params = 5;  // ... or a project parameter
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit");
  e1.project_params = 0;
  e1.sedit_slot = 3;  // the history moved to another slot
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit");
  e1.sedit_slot = 2;
  e1.pcm_hash[2] = 0;  // the history slot's AUDIO did not change (only its instrument): not what a sample edit produces
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit");
  e1.pcm_hash[2] = 7;
  // B8 review: the first edit after a slot switch. The user edited slot 2 three times, switches to slot 5 and applies one effect: the bank drops slot 2's history (3 to 1) and a non-crop edit makes no ring unit.
  // The new step is the unit; the old slot's history going is not a lowering to judge.
  e0.pcm_frames[2] = e1.pcm_frames[2] = 50;  // slot 2 held audio before, and slot 5 too (a change of held audio is covered; a load into a free slot would be exempt)
  e0.pcm_hash[2] = e1.pcm_hash[2] = 0;
  e0.pcm_frames[5] = 30;
  e0.pcm_hash[5] = 1;
  e1.inst_hash[2] = 0;
  e1.sedit = 1;
  e1.sedit_full = false;
  e1.sedit_slot = 5;
  e1.pcm_frames[5] = 40;
  e1.pcm_hash[5] = 4;
  CHECK(reason(e0, e1, "sedit.cut").empty());
  e1.pattern_hash = 1;  // the same switch with a silent change of the pattern is judged (and the cleared history says so)
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit; undo history cleared");
  e1.pattern_hash = 0;
  e1.pcm_hash[2] = 3;  // ... and so is a change of the OLD slot's audio beside the edit of the new one
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit; undo history cleared");
  e1.pcm_hash[2] = 0;
  e1.pcm_frames[5] = 0;
  e1.pcm_hash[5] = 0;
  e0.pcm_frames[2] = 0;
  e0.pcm_frames[5] = 0;
  e0.pcm_hash[5] = 0;
  e1.sedit = app::SampleBank::kHistory;
  e1.sedit_full = true;
  e1.sedit_slot = 2;
  e1.pcm_frames[2] = 100;
  e1.pcm_hash[2] = 7;
  e1.inst_hash[2] = 9;
  e0.sedit = e1.sedit = 0;  // no history at all: the same change has no unit
  e0.sedit_slot = e1.sedit_slot = -1;
  e0.sedit_full = e1.sedit_full = false;
  CHECK(reason(e0, e1, "sedit.cut") == "document changed with no question and no undo unit");

  // A document that the native writer cannot digest stops the step: it is a finding, not a pass.
  Snap bad = a;
  bad.digest_error = true;
  const std::vector<Finding> broken = check_rules(a, bad, "nav.left");
  CHECK(broken.size() == 1u);
  if (!broken.empty()) {
    CHECK(broken[0].rule == "R0");
  }
}

TEST_CASE("qanet rules self-test: on a real document a click group is a unit, and a silent change with its key still up is not") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  Spec spec;
  spec.reach = "tap pattern\n";
  std::unique_ptr<sim::Simulator> s;
  std::string error;
  CHECK_MESSAGE(prepare(s, spec, card.dir.path(), error), error);
  if (s == nullptr) {
    return;
  }
  model::Editor* ed = s->app().session().state_editor();
  const std::uint32_t tempo = s->project()->tempo_x100();
  const core::ActionId click = core::action_id("plant.click");
  ed->begin_group(click, 0);
  ed->merge_hint(model::kTempoMergeKey);
  (void)ed->set_tempo_x100(tempo + 500);  // the first click: a unit, with the tempo key
  ed->end_group();
  const Snap first = snap_of(*s);
  ed->begin_group(click, 0);
  ed->merge_hint(model::kTempoMergeKey);
  (void)ed->set_tempo_x100(tempo + 900);  // the next click of the same hold: the unit is reopened
  ed->end_group();
  const Snap second = snap_of(*s, &first);
  unsigned ring = 0;  // the ring the tempo edit went to
  while (ring + 1u < kRings && first.merge[ring] == 0u) {
    ++ring;
  }
  CHECK(first.merge[ring] != 0u);
  CHECK(second.digest != first.digest);
  CHECK(second.added[ring] == first.added[ring]);
  CHECK(check_rules(first, second, "tempo.edit").empty());
  plant_silent_step(*s);  // not journalled: nothing was recorded, and the key is still up
  const Snap third = snap_of(*s, &second);
  CHECK(third.digest != second.digest);
  CHECK(third.merge[ring] == second.merge[ring]);
  const std::vector<Finding> f = check_rules(second, third, "mixer.edit");
  CHECK(f.size() == 1u);
}

TEST_CASE("qanet rules self-test: the baseline comparison fails on a new finding and on a line that no longer occurs") {
  std::vector<Line> baseline, allow, found;
  std::string error;
  CHECK(parse_lines("# a comment\n\nR1 | a.b | Ctx tap F1 [s] | why | OS-BUG-X\nR1 | c.d | Ctx tap F2 [s] | why | new\n", baseline, error));
  CHECK(baseline.size() == 2u);
  CHECK_FALSE(parse_lines("R1 | a.b | Ctx | why\n", baseline, error));  // four fields
  CHECK_FALSE(error.empty());
  CHECK(parse_lines("R1 | e.f | Ctx tap F3 [s] | allowed because it is decided | ADR-1\n", allow, error));

  found.push_back(Line{"R1", "a.b", "Ctx tap F1 [s]", "why", "new"});  // known
  found.push_back(Line{"R1", "c.d", "Ctx tap F2 [s]", "why", "new"});  // known
  found.push_back(Line{"R1", "e.f", "Ctx tap F3 [s]", "allowed because it is decided", "new"});  // allowed
  Verdict ok = compare(found, baseline, allow);
  CHECK(ok.unknown.empty());
  CHECK(ok.stale.empty());
  CHECK(ok.bad.empty());

  found.push_back(Line{"R1", "g.h", "Ctx tap F4 [s]", "why", "new"});  // not in either file
  Verdict fresh = compare(found, baseline, allow);
  CHECK(fresh.unknown.size() == 1u);

  found.pop_back();
  found.erase(found.begin());  // a.b no longer occurs: the bug is fixed and its line has to go
  Verdict fixed = compare(found, baseline, allow);
  CHECK(fixed.stale.size() == 1u);

  baseline.push_back(baseline[0]);  // a line twice
  CHECK(compare(found, baseline, allow).bad.size() == 1u);

  std::vector<Line> allow_new;
  CHECK(parse_lines("R1 | e.f | Ctx tap F3 [s] | allowed because it is decided | new\n", allow_new, error));
  found.push_back(Line{"R1", "e.f", "Ctx tap F3 [s]", "allowed because it is decided", "new"});
  CHECK(compare(found, baseline, allow_new).bad.size() >= 1u);  // `new` is not a decision
}

TEST_CASE("qanet rules self-test: a pattern line (`*`) matches every finding it names, is judged only where the run saw its states, and says which patterns it used") {
  CHECK(glob_match("* (nav.*)", "Up (nav.up 0)"));
  CHECK(glob_match("config*", "CONFIG_page_2"));  // case is ignored
  CHECK(glob_match("a*b*c", "aXXbYYc"));
  CHECK_FALSE(glob_match("a*b*c", "aXXbYY"));
  CHECK_FALSE(glob_match("Pattern", "Pattern WATCH"));  // no `*`: the whole text
  CHECK(glob_match("*", ""));

  std::vector<Line> baseline, allow, found;
  std::string error;
  CHECK(parse_lines("R2 | * (nav.*) | CONFIG_page_* | silent | rule:one-axis\nR2 | F2 (a.b 0) | Ctx | silent | new\n", allow, error));
  CHECK(is_pattern(allow[0]));
  CHECK_FALSE(is_pattern(allow[1]));
  found.push_back(Line{"R2", "Up (nav.up 0)", "CONFIG_page_2", "silent", "new"});
  found.push_back(Line{"R2", "Right (nav.right 0)", "CONFIG_page_3", "silent", "new"});
  found.push_back(Line{"R2", "F2 (a.b 0)", "Ctx", "silent", "new"});
  Verdict v = compare(found, baseline, allow);
  CHECK(v.unknown.empty());
  CHECK(v.stale.empty());
  CHECK(v.pattern_hits.size() == 1u);  // the pattern line, once

  found.push_back(Line{"R2", "Up (nav.up 0)", "MIXER_page_2", "silent", "new"});  // another screen: the pattern does not cover it
  CHECK(compare(found, baseline, allow).unknown.size() == 1u);
  found.pop_back();
  CHECK(compare(found, baseline, allow).unknown.empty());

  // A rule other than the line's is not covered; a pattern line is stale in a whole run when nothing matches it, and is left to the shards' hits in a partial one; an exact line is judged where its state was walked.
  found.clear();
  found.push_back(Line{"R1", "Up (nav.up 0)", "CONFIG_page_2", "silent", "new"});
  CHECK(compare(found, baseline, allow).unknown.size() == 1u);
  found.clear();
  Verdict whole = compare(found, baseline, allow);
  CHECK(whole.stale.size() == 2u);
  const std::set<std::string> walked = {"Ctx", "Elsewhere"};
  Verdict part = compare(found, baseline, allow, &walked);
  CHECK(part.stale.size() == 1u);  // the exact line: its state was walked and it did not occur
  const std::set<std::string> other = {"Elsewhere"};
  CHECK(compare(found, baseline, allow, &other).stale.empty());
}

TEST_CASE("qanet rules self-test: the gap list fails when it grows, when an action on it starts to act, and when its count is wrong") {
  std::set<std::string> listed;
  unsigned count = 0;
  std::string error;
  CHECK(parse_gaps("# c\ncount: 2\na.one\nb.two\n", listed, count, error));
  CHECK(listed.size() == 2u);
  std::set<std::string> none;
  CHECK_FALSE(parse_gaps("count: 3\na.one\nb.two\n", none, count, error));  // the count is not the lines
  none.clear();
  CHECK_FALSE(parse_gaps("a.one\n", none, count, error));                     // no count
  none.clear();
  CHECK_FALSE(parse_gaps("count: 2\na.one\na.one\n", none, count, error));  // twice

  std::vector<std::string> grown, closed;
  compare_gaps({"a.one", "b.two"}, listed, grown, closed);
  CHECK(grown.empty());
  CHECK(closed.empty());
  compare_gaps({"a.one", "b.two", "c.three"}, listed, grown, closed);  // an action that no run saw act and the file does not list
  CHECK(grown == std::vector<std::string>{"c.three"});
  grown.clear();
  compare_gaps({"a.one"}, listed, grown, closed);  // b.two now acts: its line has to go
  CHECK(closed == std::vector<std::string>{"b.two"});
}

TEST_CASE("qanet rules self-test: an action without an entry in the table, and a context without a reach, are reported") {
  // A binding of a made-up context for a real action, and one of a real context for an action the table may not have: both end as setup problems.
  const core::Binding items[] = {
      {static_cast<core::ContextId>(app::Context::Pattern), core::ArgFrom::Fixed, core::tap(core::InputRef{core::InputKind::Button, 0}), core::action_id("nav.left"), 0},
      {static_cast<core::ContextId>(200), core::ArgFrom::Fixed, core::tap(core::InputRef{core::InputKind::Button, 1}), core::action_id("nav.left"), 0},
  };
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  const Report r = walk(core::BindingView{items, 2}, card.dir.path());
  CHECK(r.rows == 2u);
  bool no_reach = false;
  for (const std::string& e : r.setup_errors) {
    no_reach = no_reach || e.find("no entry in context_reach()") != std::string::npos;
  }
  CHECK(no_reach);
  CHECK(r.runs == 1u);
}

TEST_CASE("qanet rules: every action of the registry that a binding reaches has an entry in the table") {
  const app::ActionTable registry = app::action_registry();
  const core::BindingView view = app::builtin_bindings();
  std::set<std::string> reached;
  for (std::size_t i = 0; i < view.count; ++i) {
    for (std::size_t k = 0; k < registry.count; ++k) {
      if (registry.items[k].id == view.items[i].action && view.items[i].action != core::action_id("ui.none")) {
        reached.insert(registry.items[k].name);
      }
    }
  }
  const auto& table = qanet::action_states();
  std::set<std::string> listed;
  for (const qanet::ActionEntry& e : table) {
    CHECK_MESSAGE(listed.insert(e.action).second, "twice in action_states.cpp: " << e.action);
    CHECK_MESSAGE(!e.states.empty(), "no state for " << e.action);
    CHECK_MESSAGE(reached.count(e.action) != 0, "action_states.cpp lists " << e.action << ", which no built-in binding reaches (or the registry does not have)");
  }
  for (const std::string& a : reached) {
    CHECK_MESSAGE(listed.count(a) != 0, "no entry in action_states.cpp for the action " << a);
  }
}

TEST_CASE("qanet rules: every context reach brings its own context into the chain") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  std::set<unsigned> seen;
  for (const qanet::ContextReach& r : qanet::context_reach()) {
    CHECK_MESSAGE(seen.insert(static_cast<unsigned>(r.context)).second, "twice in context_reach(): " << std::string(r.name));
    if (reach_steps(r) == nullptr) {
      CHECK_MESSAGE((r.skip != nullptr && r.skip[0] != 0), std::string(r.name) << ": skipped without a reason");
      continue;
    }
    Spec spec;
    spec.base = r.base;
    spec.reach = reach_steps(r);
    std::unique_ptr<sim::Simulator> s;
    std::string error;
    const bool ok = prepare(s, spec, card.dir.path(), error);
    CHECK_MESSAGE(ok, std::string(r.name) << ": " << error);
    if (ok) {
      CHECK_MESSAGE(context_live(*s, r.context), std::string(r.name) << ": the reach does not bring the context into the chain");
    }
  }
  // Every context that a built-in binding names has a reach (or a reason it has none).
  const core::BindingView view = app::builtin_bindings();
  for (std::size_t i = 0; i < view.count; ++i) {
    const auto c = static_cast<app::Context>(view.items[i].context);
    CHECK_MESSAGE(reach_of(qanet::context_reach(), c) != nullptr, "context " << static_cast<unsigned>(c) << " has bindings and no entry in context_reach()");
  }
}
