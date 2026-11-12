// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET A3 (docs/specs/OS-QANET.md part A, rule 3; new Community OS behaviour, test tooling only): an operation on a selection acts on the selection. The table of selection_ops.cpp lists the actions that take a
// selection and the span each may change, written from the specs as decided; this walk selects a span (a block of Pattern, a range of Song slots, a slice of Wave, a selection of Sample edit, an instrument),
// runs the action, and compares the document outside the span (it must not change) and the result inside it with the spec (selection_ops.h).
//
// The states: the demo document, its first pattern overwritten with steps that are all different (so a shifted or reversed cell is a different cell), track lengths of 16, 16, 12 and 80 steps (a track that ends
// above the cursor, and one that DUPLICATE refuses); a song of six slots with six patterns; the sound document with KICK.WAV loaded for the sample screens. The action is run through Simulator::action with the
// argument of its binding, then the simulator runs until the document, the undo depths and the notifications stop moving (the sample edit's job included).
//
// Findings are compared with baseline_select.txt (rule | action | state | reason | ID or `new`) and allow_rules.txt (R3 lines: a decision); the test fails on a finding that is in neither and on a line that no
// longer occurs. QANET_WRITE_BASELINE=<path> writes the findings in that format, QANET_SHARD=k/n splits the cases, QANET_CASE=<text> runs the cases whose name has the text, QANET_VERBOSE prints every run.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_states.h"
#include "rig.h"
#include "rules.h"
#include "selection_ops.h"

#include <o3os/app/actions.h>
#include <o3os/app/model.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/view_state.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
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
#include <string>
#include <utility>
#include <vector>

using namespace o3os;
using namespace qanet;

namespace {

// ---- The states ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

enum class Seed { None, Pattern, Song };

struct Act {
  const char* action;
  int arg;
};

struct Case {
  std::string name;  // part of the finding's context column: keep it stable
  Doc doc = Doc::Demo;
  bool card = false;       // the sample screens need the card (KICK.WAV)
  std::string pre;         // lines before the seed (a sample loaded)
  Seed seed = Seed::None;
  std::string reach;       // the screen
  std::string steps;       // the selection and the cursor
  bool copy_block = false;  // press Copy on the block now selected (the clip is recorded), then run `steps2`
  std::string steps2;
  int copy_inst = -1;      // the instrument slot (0-based) that `steps` copies: the paste's source
  std::vector<Act> acts;
};

const char* const kLoadKick = "tap sampleload\nwait 120\ntap down 2\ntap f1\nwait 50\ntap f1\nwait 300\ntap pattern\n";  // KICK.WAV onto instrument 1 of the sound document

// The cursor to (row, track) in the column named: Home, Down, the track, the column's own key.
std::string cursor_to(int row, int track, const char* column_key = "note") {
  std::string s = "action cursor.jump_fraction 0\n";
  if (row > 0) {
    s += "action nav.scroll " + std::to_string(row) + "\n";
  }
  s += "action cursor.track -16\n";  // the cursor moves by tracks (a delta): back to the first, then on
  if (track > 0) {
    s += "action cursor.track " + std::to_string(track) + "\n";
  }
  s += std::string("tap ") + column_key + "\n";
  s += "assert state row=" + std::to_string(row) + " track=" + std::to_string(track) + "\n";
  return s;
}

// The block grows by `rows` steps down and `tracks` tracks right from the cursor (the cursor goes to the far corner). OS-AUDIT-56: Shift + Right goes a column at a time inside the
// first track (`column` is the cursor's column, 0 NOTE .. 3 FX1) and takes whole tracks after that, so the first track takes 5 - column presses.
std::string extend(int rows, int tracks, int column = 0) {
  std::string s;
  for (int i = 0; i < rows; ++i) {
    s += "action select.extend 1\n";
  }
  for (int i = 0; i < (tracks > 0 ? tracks + 4 - column : 0); ++i) {
    s += "action select.extend 10\n";
  }
  return s;
}

const std::vector<Act> kPatternActs = {{"block.copy", 0},        {"step.clear", 0},   {"step.note_delta", 1},       {"step.note_delta", -3}, {"step.note_octave", 1}, {"step.note", 61}, {"step.note_pad", 5},
                                       {"step.instrument_delta", 1}, {"step.instrument", 3}, {"step.insert", 0},   {"step.pull", 0},         {"pattern.invert", 0},   {"pattern.op", 0},
                                       {"pattern.op", 1},        {"pattern.op", 2}};

std::vector<Case> make_cases() {
  std::vector<Case> out;
  auto pat = [&](const char* name, const std::string& steps, std::vector<Act> acts) {
    Case c;
    c.name = name;
    c.seed = Seed::Pattern;
    c.steps = "tap rec\n" + steps;
    c.acts = std::move(acts);
    out.push_back(std::move(c));
  };
  // Pattern: the block.
  pat("Pattern, block 3 steps x 2 tracks (rows 4-6, tracks 1-2), Note column", cursor_to(4, 1) + extend(2, 1), kPatternActs);
  pat("Pattern, no selection (cursor row 6, track 2), Note column", cursor_to(6, 2), {{"block.copy", 0}, {"step.clear", 0}, {"step.note_delta", 1}, {"step.note_octave", -1}, {"step.insert", 0}, {"step.pull", 0}, {"pattern.invert", 0}, {"pattern.op", 0}, {"pattern.op", 1}, {"pattern.op", 2}});
  pat("Pattern, block 4 steps x 1 track (rows 8-11, track 4), Note column", cursor_to(8, 4) + extend(3, 0), {{"step.clear", 0}, {"step.note_delta", 2}, {"step.insert", 0}, {"step.pull", 0}, {"pattern.invert", 0}});
  pat("Pattern, block 1 step x 3 tracks (row 2, tracks 3-5), Note column", cursor_to(2, 3) + extend(0, 2), {{"step.clear", 0}, {"step.note_delta", -1}, {"step.insert", 0}, {"step.pull", 0}, {"pattern.invert", 0}, {"pattern.op", 2}});
  pat("Pattern, block 3 steps x 2 tracks grown up and left (rows 4-6, tracks 1-2, cursor at row 4, track 1), Note column", cursor_to(6, 2) + "action select.extend -1\naction select.extend -1\naction select.extend -10\n",
      {{"step.clear", 0}, {"step.note_delta", 1}, {"step.insert", 0}, {"step.pull", 0}, {"pattern.invert", 0}});
  pat("Pattern, block rows 11-13 x tracks 1-2 (track 2 ends at row 12), Note column", cursor_to(11, 1) + extend(2, 1), {{"step.clear", 0}, {"step.note_delta", 1}, {"step.insert", 0}, {"step.pull", 0}, {"pattern.invert", 0}});
  pat("Pattern, block 2 steps on the 80-step track (rows 0-1, track 3), Note column", cursor_to(0, 3) + extend(1, 0), {{"pattern.op", 0}, {"pattern.op", 1}, {"pattern.op", 2}, {"step.insert", 0}});
  pat("Pattern, block 3 steps x 2 tracks (rows 4-6, tracks 1-2), Instr column", cursor_to(4, 1, "instr") + extend(2, 1, 1), {{"step.clear", 0}, {"step.instrument_delta", 1}, {"step.instrument_coarse", 1}});
  pat("Pattern, block 3 steps x 2 tracks (rows 4-6, tracks 1-2), Vol column", cursor_to(4, 1, "vol") + extend(2, 1, 2), {{"step.clear", 0}, {"step.vol_delta", 1}, {"step.vol_coarse", 1}, {"step.vol", 40}, {"step.vol_pad", 5}});
  pat("Pattern, block 3 steps x 2 tracks (rows 4-6, tracks 1-2), Fx column", cursor_to(4, 1, "fx") + extend(2, 1, 3), {{"step.clear", 0}, {"step.fx_value_delta", 1}, {"step.fx_value_coarse", 1}, {"step.fx_type_delta", 1}, {"step.fx_value", 7}, {"step.fx_pad", 3}, {"step.fx_type", 2}});
  // Pattern: paste. The block rows 2-4 x tracks 0-1 is copied, then the cursor goes to the target.
  auto paste = [&](const char* name, const std::string& target) {
    Case c;
    c.name = name;
    c.seed = Seed::Pattern;
    c.steps = "tap rec\n" + cursor_to(2, 0) + extend(2, 1);
    c.copy_block = true;
    c.steps2 = "action select.clear\n" + target;
    c.acts = {{"block.paste", 0}};
    out.push_back(std::move(c));
  };
  paste("Pattern, a 3 x 2 clip pasted at row 9, track 4", cursor_to(9, 4));
  paste("Pattern, a 3 x 2 clip pasted at row 14 of tracks 1-2 (clipped at the track ends)", cursor_to(14, 1));
  paste("Pattern, a 3 x 2 clip pasted on the last track", cursor_to(6, 99));  // 99: replaced by the last active track
  paste("Pattern, a 3 x 2 clip pasted over itself (row 3, track 0)", cursor_to(3, 0));
  // Pattern: the Fill dialog (F4). OS-PTOOLS2 S3c: no SCOPE row; the target is the selection — the block, the column of Shift + Up at step 1, every
  // track when that is done again, the cursor step with no selection. WHAT is row 0 of the dialog now.
  auto fill = [&](const char* name, const std::string& select, int what_delta) {
    std::string dialog = "action fill.open\n";
    dialog += what_delta != 0 ? "action fill.delta " + std::to_string(what_delta) + "\n" : "";
    pat(name, select + dialog, {{"fill.apply", 0}});
  };
  const std::string block = cursor_to(4, 1) + extend(2, 1);
  fill("Pattern, Fill, the block, NOTE (block rows 4-6, tracks 1-2)", block, 0);
  fill("Pattern, Fill, the block, INSTR (block rows 4-6, tracks 1-2)", block, 1);
  fill("Pattern, Fill, the block, VOLUME (block rows 4-6, tracks 1-2)", block, 2);
  fill("Pattern, Fill, the block, FX 1 (block rows 4-6, tracks 1-2)", block, 3);
  fill("Pattern, Fill, the block, FX 2 (block rows 4-6, tracks 1-2)", block, 4);
  fill("Pattern, Fill, the cursor step, NOTE (no selection, cursor row 6, track 2)", cursor_to(6, 2), 0);
  fill("Pattern, Fill, the track, NOTE (the column of track 2, selected after the block)", block + "action select.column_all\n", 0);
  fill("Pattern, Fill, the track, NOTE (the 12-step track 2's column, cursor at row 3)", cursor_to(3, 2) + "action select.column_all\n", 0);
  fill("Pattern, Fill, the pattern, NOTE (the column selection over every track)", block + "action select.column_all\naction select.column_all\n", 0);
  // Song: the range of slots.
  auto song = [&](const char* name, const std::string& steps) {
    Case c;
    c.name = name;
    c.seed = Seed::Song;
    c.reach = "tap song\n";
    c.steps = steps;
    c.acts = {{"song.copy", 0}, {"song.cut", 0}, {"song.delete_slot", 0}, {"song.clear", 0}, {"song.move", 1}, {"song.move", -1}};
    out.push_back(std::move(c));
  };
  song("Song, 6 slots, range of 2 (slots 2-3)", "action cursor.jump_fraction 0\naction nav.scroll 2\naction song.select 1\n");
  song("Song, 6 slots, the cursor's slot alone (slot 1)", "action cursor.jump_fraction 0\naction nav.scroll 1\n");
  song("Song, 6 slots, range at the top (slots 0-1)", "action cursor.jump_fraction 0\naction song.select 1\n");
  song("Song, 6 slots, range at the bottom (slots 4-5)", "action cursor.jump_fraction 0\naction nav.scroll 4\naction song.select 1\n");
  song("Song, 6 slots, every slot selected", "action cursor.jump_fraction 0\naction song.select 1\naction song.select 1\naction song.select 1\naction song.select 1\naction song.select 1\n");
  // Wave: slices and points.
  auto wave = [&](const char* name, const char* reach, const std::string& steps, std::vector<Act> acts) {
    Case c;
    c.name = name;
    c.doc = Doc::Sound;
    c.card = true;
    c.pre = kLoadKick;
    c.reach = reach;
    c.steps = steps;
    c.acts = std::move(acts);
    out.push_back(std::move(c));
  };
  const char* const chop = "tap sampleplay\ntap sampleplay\n";
  wave("Wave CHOP, no slices, ZC off", chop, "action wave.zc\n", {{"wave.add", 0}, {"wave.del", 0}, {"wave.nudge", 3}});
  wave("Wave CHOP, 16 even slices, slice 3 selected, ZC off", chop, "action wave.even\naction wave.pad_select 3\naction wave.zc\n", {{"wave.add", 0}, {"wave.del", 0}, {"wave.nudge", 3}, {"wave.nudge", -2}});
  wave("Wave CHOP, 16 even slices, slice 3 selected, ZC on", chop, "action wave.even\naction wave.pad_select 3\n", {{"wave.add", 0}, {"wave.del", 0}});
  wave("Wave CHOP, 16 even slices, the last slice selected, ZC off", chop, "action wave.even\naction wave.pad_select 15\naction wave.zc\n", {{"wave.add", 0}, {"wave.del", 0}});
  wave("Wave CHOP, 16 even slices, the first slice selected, ZC off", chop, "action wave.even\naction wave.pad_select 0\naction wave.zc\n", {{"wave.add", 0}, {"wave.del", 0}});
  wave("Wave CHOP, 4 slices, slice 1 moved and selected, ZC off", chop,
       "action wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\naction wave.even_n -1\n"
       "action wave.even_n -1\naction wave.even_n -1\naction wave.even\naction wave.zc\naction wave.pad_select 1\naction wave.nudge 7\n",
       {{"wave.add", 0}, {"wave.del", 0}});
  wave("Wave WAVE page, the start point selected", "tap sampleplay\n", "", {{"wave.nudge", 3}, {"wave.nudge", -3}});
  wave("Wave WAVE page, the loop end point selected", "tap sampleplay\n", "action wave.point 2\n", {{"wave.nudge", 3}});
  // Sample edit: the selection [a, b) of the sample (EditOp: 0 Crop, 1 Normalise, 2 Reverse, 3 FadeIn, 4 FadeOut, 5 Amplify, 7 DcRemove).
  auto sedit = [&](const char* name, const std::string& steps) {
    Case c;
    c.name = name;
    c.doc = Doc::Sound;
    c.card = true;
    c.pre = kLoadKick;
    c.reach = "tap sampleedit\n";
    c.steps = steps;
    c.acts = {{"sedit.run", 1}, {"sedit.run", 2}, {"sedit.run", 3}, {"sedit.run", 4}, {"sedit.run", 5}, {"sedit.run", 7}, {"sedit.run", 0}};
    out.push_back(std::move(c));
  };
  sedit("Sample edit, a selection in the middle", "action sedit.move 150\ntap right\naction sedit.move -200\n");
  sedit("Sample edit, a selection at the start", "tap right\naction sedit.move -500\n");
  sedit("Sample edit, a selection at the end", "action sedit.move 500\n");
  sedit("Sample edit, a selection of a few frames", "action sedit.move 767\n");
  sedit("Sample edit, no selection (the whole sample)", "");
  // Instrument: the selected slot.
  auto inst = [&](const char* name, const std::string& steps, int copied, std::vector<Act> acts) {
    Case c;
    c.name = name;
    c.card = true;  // the sample bank (the instrument clipboard, with its audio) is attached with the card
    c.reach = "tap params\n";
    c.steps = steps;
    c.copy_inst = copied;
    c.acts = std::move(acts);
    out.push_back(std::move(c));
  };
  inst("Instrument, slot 2 selected", "action instrument.select 2\n", -1, {{"instrument.copy", 0}, {"instrument.delete", 0}, {"instrument.init", 0}});
  inst("Instrument, slot 3 copied, slot 5 selected", "action instrument.select 3\naction instrument.copy\naction instrument.select 5\n", 2, {{"instrument.paste", 0}});
  inst("Instrument, slot 3 copied, the empty slot 30 selected", "action instrument.select 3\naction instrument.copy\naction instrument.select 30\n", 2, {{"instrument.paste", 0}});
  return out;
}

// ---- Seeds ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// Pattern 1: every step of every active track different (the note, the instrument, the volume and two effects of each follow from the track and the row), with some empty, Off and Cut steps so that "only steps that have a
// note" is a real distinction. Track lengths 16, 16, 12, 80 and 16 for the rest.
bool seed_pattern(sim::Simulator& s, std::string& error) {
  model::Editor* ed = s.app().session().state_editor();
  const model::Project* p = s.project();
  if (ed == nullptr || p == nullptr || p->pattern(1) == nullptr) {
    error = "no pattern to seed";
    return false;
  }
  for (unsigned t = 0; t < p->active_tracks(); ++t) {
    const unsigned len = t == 2 ? 12u : (t == 3 ? 80u : 16u);
    if (ed->set_track_length(1, t, len) != model::Status::Ok) {
      error = "set_track_length";
      return false;
    }
    model::Step col[model::kMaxPatternLength];
    for (unsigned r = 0; r < model::kMaxPatternLength; ++r) {
      const unsigned k = (t * 5u + r * 7u) % 11u;
      model::Step st = model::kEmptyStep;
      if (k == 0u) {
        // empty
      } else if (k == 1u) {
        st.note = model::kNoteOff;
      } else if (k == 2u) {
        st.note = model::kNoteCut;
      } else {
        st.note = static_cast<std::uint8_t>(21u + (t * 13u + r * 3u) % 80u);
        st.instr = static_cast<std::uint8_t>(1u + (t + r) % 8u);
      }
      st.vol = (k % 3u == 0u) ? model::kVolumeUnset : static_cast<std::uint8_t>((r * 3u + t) % 100u);
      st.fx[0] = {static_cast<std::uint8_t>(1u + (t + r) % 20u), static_cast<std::uint8_t>((t * 31u + r * 17u) % 200u)};
      st.fx[1] = (k % 4u == 0u) ? model::StepFx{0, 0} : model::StepFx{static_cast<std::uint8_t>(21u + (t * 3u + r) % 20u), static_cast<std::uint8_t>((t * 7u + r * 5u) % 100u)};
      col[r] = st;
    }
    if (ed->set_steps(1, t, 0, col, model::kMaxPatternLength) != model::Status::Ok) {
      error = "set_steps";
      return false;
    }
  }
  return true;
}

// The song: six slots, the patterns 1..6.
bool seed_song(sim::Simulator& s, std::string& error) {
  model::Editor* ed = s.app().session().state_editor();
  if (ed == nullptr) {
    error = "no document";
    return false;
  }
  (void)ed->clear_order();
  for (unsigned id = 1; id <= 6; ++id) {
    if (s.project()->pattern(id) == nullptr) {
      (void)ed->create_pattern(id, 16);
    }
    if (ed->append_order(id) != model::Status::Ok) {
      error = "append_order";
      return false;
    }
  }
  return true;
}

// A fresh simulator in the state of `c`. `clip` is filled when the case copies a block.
bool prepare(std::unique_ptr<sim::Simulator>& out, const Case& c, const std::string& card_dir, Clip& clip, bool& copied, std::string& error) {
  out = std::make_unique<sim::Simulator>();
  const char* doc = c.doc == Doc::Sound ? "document sound\n" : "document\n";
  std::string head = std::string(doc) + "wait 5\n";
  if (c.card) {
    head += "card \"" + card_dir + "\"\nwait 120\n";
  }
  if (!run_text(*out, head, error)) {
    error = "base: " + error;
    return false;
  }
  if (!run_text(*out, c.pre, error)) {
    error = "pre: " + error;
    return false;
  }
  if (c.seed == Seed::Pattern && !seed_pattern(*out, error)) {
    return false;
  }
  if (c.seed == Seed::Song && !seed_song(*out, error)) {
    return false;
  }
  if (!run_text(*out, c.reach + "wait 20\n", error)) {
    error = "reach: " + error;
    return false;
  }
  std::string steps = c.steps;
  if (!run_text(*out, steps + "wait 20\n", error)) {
    error = "steps: " + error;
    return false;
  }
  copied = false;
  if (c.copy_block) {
    SelImage im;
    if (!image_of(*out, im)) {
      error = "no image for the copy";
      return false;
    }
    clip.rows = im.row1 - im.row0 + 1;
    clip.tracks = im.track1 - im.track0 + 1;
    clip.steps.clear();
    for (int t = im.track0; t <= im.track1; ++t) {
      for (int r = im.row0; r <= im.row1; ++r) {
        clip.steps.push_back(im.doc->pattern(im.pattern)->steps[t][r]);
      }
    }
    if (!run_text(*out, "action block.copy\nwait 20\n", error)) {
      error = "copy: " + error;
      return false;
    }
    std::string s2 = c.steps2;
    const std::size_t at = s2.find("cursor.track 99");
    if (at != std::string::npos) {
      s2.replace(at + 13, 2, std::to_string(out->project()->active_tracks() - 1u));
      const std::size_t chk = s2.find("track=99");
      if (chk != std::string::npos) {
        s2.replace(chk + 6, 2, std::to_string(out->project()->active_tracks() - 1u));
      }
    }
    if (!run_text(*out, s2 + "wait 20\n", error)) {
      error = "steps2: " + error;
      return false;
    }
    copied = true;
  }
  return true;
}

// ---- The walk ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Report {
  std::vector<Line> findings;
  std::vector<std::string> setup_errors;
  std::set<std::string> walked;
  std::map<std::string, unsigned> judged;  // per action: the runs that were judged (the entry applied)
  unsigned runs = 0, acted = 0;
};

bool verbose() { return std::getenv("QANET_VERBOSE") != nullptr; }

std::string arg_text(const Act& a) { return a.arg == 0 ? std::string(a.action) : std::string(a.action) + " " + std::to_string(a.arg); }

using Do = std::function<void(sim::Simulator&, const Act&)>;

void walk_case(const Case& c, const std::string& card_dir, const Do& act, Report& report) {
  for (const Act& a : c.acts) {
    std::unique_ptr<sim::Simulator> s;
    std::string error;
    Clip clip;
    bool copied = false;
    if (!prepare(s, c, card_dir, clip, copied, error)) {
      report.setup_errors.push_back(c.name + " | " + arg_text(a) + ": " + error);
      continue;
    }
    report.walked.insert(c.name);
    SelImage before, after;
    if (!image_of(*s, before) || !before.ok) {
      report.setup_errors.push_back(c.name + " | " + arg_text(a) + ": no document to look at");
      continue;
    }
    const SelOp* op = find_selection_op(a.action);
    if (op == nullptr) {
      report.setup_errors.push_back(c.name + " | " + arg_text(a) + ": the action is not in the table");
      continue;
    }
    if (before.screen != op->screen || !op->applies(before, SelContext{copied ? &clip : nullptr, c.copy_inst, true, nullptr})) {
      report.setup_errors.push_back(c.name + " | " + arg_text(a) + ": the entry does not apply in the state (screen " + std::to_string(static_cast<int>(before.screen)) + ", editable " + (before.editable ? "yes" : "no") + ")");
      continue;
    }
    ++report.runs;
    act(*s, a);
    settle(*s);
    if (!image_of(*s, after) || !after.ok) {
      report.setup_errors.push_back(c.name + " | " + arg_text(a) + ": no document after the action");
      continue;
    }
    std::string detail;
    SelContext ctx;
    ctx.clip = copied ? &clip : nullptr;
    ctx.copied_instrument = c.copy_inst;
    ctx.must_act = true;
    ctx.detail = &detail;
    const std::vector<Finding> found = check_selection(before, after, a.action, a.arg, ctx);
    ++report.judged[a.action];
    report.acted += before.digest != after.digest || before.pcm_hash != after.pcm_hash ? 1u : 0u;
    if (verbose()) {
      std::printf("run %s | %s | %s | block rows %d-%d tracks %d-%d cursor %d,%d col %d | song %d-%d | doc %s%s\n", arg_text(a).c_str(), c.name.c_str(), found.empty() ? "ok" : "FINDING", before.row0, before.row1, before.track0, before.track1,
                  before.cur_row, before.cur_track, before.cur_col, before.song_first, before.song_last, before.digest != after.digest || before.pcm_hash != after.pcm_hash ? "changed" : "same", detail.empty() ? "" : (" | " + detail).c_str());
    }
    for (const Finding& f : found) {
      report.findings.push_back(Line{f.rule, arg_text(a), c.name, f.reason, "new"});
    }
  }
}

const Do kReal = [](sim::Simulator& s, const Act& a) {
  const app::DispatchResult r = s.action(core::action_id(a.action), static_cast<core::ActionArg>(a.arg));
  if (verbose()) {
    static const char* const names[] = {"Unknown", "Disabled", "Unchanged", "Changed"};
    std::printf("  %s -> %s\n", arg_text(a).c_str(), names[static_cast<unsigned>(r)]);
  }
};

struct Shard {
  unsigned k = 1, n = 1;
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

}  // namespace

// ---- The test ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("qanet select: every operation on a selection acts on the selection (baseline of known findings)") {
  const auto t0 = std::chrono::steady_clock::now();
  Card card;
  CHECK_MESSAGE(card.ok, "the card fixture could not be made (is the target e2e_card_fixture built?)");
  if (!card.ok) {
    return;
  }
  const std::vector<Case> cases = make_cases();
  const Shard shard = shard_from_env();
  const char* only = std::getenv("QANET_CASE");
  Report report;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    if (i % shard.n != shard.k - 1u || (only != nullptr && cases[i].name.find(only) == std::string::npos)) {
      continue;
    }
    walk_case(cases[i], card.dir.path(), kReal, report);
  }
  std::vector<Line> baseline, allow;
  std::string text, error;
  CHECK_MESSAGE(read_file(O3_QANET_SELECT_BASELINE, text), "cannot read " << O3_QANET_SELECT_BASELINE);
  CHECK_MESSAGE(parse_lines(text, baseline, error), "baseline_select.txt: " << error);
  text.clear();
  CHECK_MESSAGE(read_file(O3_QANET_ALLOW, text), "cannot read " << O3_QANET_ALLOW);
  CHECK_MESSAGE(parse_lines(text, allow, error), "allow_rules.txt: " << error);
  allow.erase(std::remove_if(allow.begin(), allow.end(), [](const Line& l) { return l.rule != "R3"; }), allow.end());
  if (const char* out = std::getenv("QANET_WRITE_BASELINE")) {
    std::ofstream f(out);
    for (Line l : report.findings) {
      l.id = "new";
      for (const Line& b : baseline) {
        l.id = b.matches(l) ? b.id : l.id;
      }
      f << l.text() << "\n";
    }
  }
  const bool partial = shard.n > 1 || only != nullptr;
  const Verdict v = compare(report.findings, baseline, allow, partial ? &report.walked : nullptr);
  unsigned fresh = 0;
  for (const Line& l : baseline) {
    fresh += l.id.compare(0, 3, "new") == 0 ? 1u : 0u;
  }
  const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  std::printf("qanet select: %zu cases in all, %zu walked, %u runs (%u changed the document), %zu findings (%zu in the baseline, %u of them `new`), %ld ms\n", cases.size(), report.walked.size(), report.runs, report.acted,
              report.findings.size(), baseline.size(), fresh, ms);
  CHECK_MESSAGE(report.setup_errors.empty(), "setup problems (a state that did not hold):" << join(report.setup_errors));
  CHECK_MESSAGE(v.bad.empty(), "the baseline is malformed:" << join(v.bad));
  CHECK_MESSAGE(v.unknown.empty(), "findings that are not in baseline_select.txt (fix the bug, or add the line, marked `new`, for the maintainer's triage):" << join(v.unknown));
  CHECK_MESSAGE(v.stale.empty(), "baseline lines that no longer occur (the bug is fixed: delete the line):" << join(v.stale));
}

// Every action of the table is a registry action, and every action the table names as not judged is too; none is in both lists.
TEST_CASE("qanet select: the table names registered actions, and every action walked is in it") {
  const app::ActionTable registry = app::action_registry();
  auto known = [&](const char* name) {
    for (std::size_t i = 0; i < registry.count; ++i) {
      if (std::strcmp(registry.items[i].name, name) == 0) {
        return true;
      }
    }
    return false;
  };
  std::set<std::string> seen;
  for (const SelOp& op : selection_ops()) {
    CHECK_MESSAGE(known(op.action), "the table names an action the registry does not have: " << op.action);
    CHECK_MESSAGE(seen.insert(op.action).second, "twice in the table: " << op.action);
    CHECK_MESSAGE((op.spec != nullptr && op.spec[0] != '\0'), "an entry names its spec: " << op.action);
    CHECK_MESSAGE((op.span != nullptr && op.applies != nullptr), "an entry has a span and a precondition: " << op.action);
    CHECK_MESSAGE((op.inside != nullptr || op.silent[0] != '\0'), "an entry that judges no inside says what the spec leaves open: " << op.action);
  }
  for (const NotJudged& n : selection_ops_not_judged()) {
    CHECK_MESSAGE(known(n.action), "not judged: an action the registry does not have: " << n.action);
    CHECK_MESSAGE(seen.count(n.action) == 0u, "both in the table and in the not-judged list: " << n.action);
    CHECK_MESSAGE(n.reason[0] != '\0', "a not-judged action says why: " << n.action);
  }
  std::set<std::string> walked;
  for (const Case& c : make_cases()) {
    for (const Act& a : c.acts) {
      walked.insert(a.action);
      CHECK_MESSAGE(seen.count(a.action) == 1u, "a case runs an action that is not in the table: " << a.action);
    }
  }
  for (const SelOp& op : selection_ops()) {
    CHECK_MESSAGE(walked.count(op.action) == 1u, "an entry of the table that no case runs: " << op.action);
  }
  std::printf("qanet select: %zu actions judged, %zu named as not judged\n", seen.size(), selection_ops_not_judged().size());
}

// ---- The self-test ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

bool image_of_case(const Case& c, const std::string& card_dir, std::unique_ptr<sim::Simulator>& s, SelImage& im, Clip& clip, bool& copied) {
  std::string error;
  if (!prepare(s, c, card_dir, clip, copied, error)) {
    MESSAGE(error);
    return false;
  }
  return image_of(*s, im);
}

const Case& case_named(const std::vector<Case>& cases, const char* part) {
  for (const Case& c : cases) {
    if (c.name.find(part) != std::string::npos) {
      return c;
    }
  }
  return cases.front();
}

bool has(const std::vector<Finding>& f, const char* reason) {
  return std::any_of(f.begin(), f.end(), [&](const Finding& x) { return x.reason == reason; });
}

}  // namespace

TEST_CASE("qanet select self-test: a planted edit outside the selection fails, a planted wrong result inside it fails, a correct edit does not") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  const std::vector<Case> cases = make_cases();
  std::unique_ptr<sim::Simulator> s;
  SelImage before;
  Clip clip;
  bool copied = false;
  const bool got = image_of_case(case_named(cases, "block 3 steps x 2 tracks (rows 4-6, tracks 1-2), Note column"), card.dir.path(), s, before, clip, copied);
  CHECK(got);
  if (!got || !before.ok) {
    return;
  }
  CHECK(before.row0 == 4);
  CHECK(before.row1 == 6);
  CHECK(before.track0 == 1);
  CHECK(before.track1 == 2);
  SelContext ctx;
  ctx.must_act = true;

  // A correct clear, written by hand: every cell of the block cleared, nothing else touched.
  SelImage good;
  good.doc = std::make_unique<model::Project>(*before.doc);
  for (int t = before.track0; t <= before.track1; ++t) {
    for (int r = before.row0; r <= before.row1; ++r) {
      model::Step st = good.doc->pattern(before.pattern)->steps[t][r];
      st.note = model::kNoteEmpty;
      st.instr = 0;
      CHECK(good.doc->set_step(before.pattern, static_cast<unsigned>(t), static_cast<unsigned>(r), st) == model::Status::Ok);
    }
  }
  auto relook = [&](SelImage& im) {
    im.ok = true;
    im.digest = sim::model_digest(*im.doc);
    im.screen = before.screen;
    im.pattern = before.pattern;
    im.cur_row = before.cur_row;
    im.cur_track = before.cur_track;
    im.cur_col = before.cur_col;
    im.row0 = before.row0;
    im.row1 = before.row1;
    im.track0 = before.track0;
    im.track1 = before.track1;
    im.selected = before.selected;
    im.editable = before.editable;
    im.pcm_hash = before.pcm_hash;
  };
  relook(good);
  CHECK(check_selection(before, good, "step.clear", 0, ctx).empty());

  // The same clear, and one step outside the block changed too: a finding about the outside.
  SelImage leak;
  leak.doc = std::make_unique<model::Project>(*good.doc);
  {
    model::Step st = leak.doc->pattern(before.pattern)->steps[0][0];
    st.note = static_cast<std::uint8_t>(st.note == 70 ? 71 : 70);
    CHECK(leak.doc->set_step(before.pattern, 0, 0, st) == model::Status::Ok);
  }
  relook(leak);
  const std::vector<Finding> f1 = check_selection(before, leak, "step.clear", 0, ctx);
  CHECK_MESSAGE(has(f1, "a pattern step changed outside the selection"), "a step outside the block was changed and the rule did not say so");

  // A step one row past the block: the edge of the span.
  SelImage edge;
  edge.doc = std::make_unique<model::Project>(*good.doc);
  {
    model::Step st = edge.doc->pattern(before.pattern)->steps[before.track0][before.row1 + 1];
    st.vol = static_cast<std::uint8_t>(st.vol == 9 ? 10 : 9);
    CHECK(edge.doc->set_step(before.pattern, static_cast<unsigned>(before.track0), static_cast<unsigned>(before.row1 + 1), st) == model::Status::Ok);
  }
  relook(edge);
  CHECK(has(check_selection(before, edge, "step.clear", 0, ctx), "a pattern step changed outside the selection"));

  // A planted leak in the song order, the instruments and a track length: each named.
  SelImage order;
  order.doc = std::make_unique<model::Project>(*good.doc);
  CHECK(order.doc->append_order(3) == model::Status::Ok);
  relook(order);
  CHECK(has(check_selection(before, order, "step.clear", 0, ctx), "the song order changed outside the selection"));
  SelImage length;
  length.doc = std::make_unique<model::Project>(*good.doc);
  CHECK(length.doc->set_track_length(before.pattern, 5, 7) == model::Status::Ok);
  relook(length);
  CHECK(has(check_selection(before, length, "step.clear", 0, ctx), "a pattern length changed outside the selection"));

  // The block was not cleared: the inside is wrong.
  SelImage unchanged;
  unchanged.doc = std::make_unique<model::Project>(*before.doc);
  {
    model::Step st = unchanged.doc->pattern(before.pattern)->steps[2][3];  // outside the block: only the digest moves, so the walk's "acted" is true
    st.vol = static_cast<std::uint8_t>(st.vol == 5 ? 6 : 5);
    CHECK(unchanged.doc->set_step(before.pattern, 2, 3, st) == model::Status::Ok);
  }
  relook(unchanged);
  const std::vector<Finding> f2 = check_selection(before, unchanged, "step.clear", 0, ctx);
  CHECK(has(f2, "a cleared step still holds its note or instrument"));

  // The span of an action that is not in the table, and a screen it does not work on, are not judged.
  CHECK(check_selection(before, leak, "no.such.action", 0, ctx).empty());
  SelImage elsewhere = before;
  elsewhere.screen = app::ScreenId::Song;
  CHECK(check_selection(elsewhere, leak, "step.clear", 0, ctx).empty());

  // block.copy has no span: any change of the document is outside it.
  CHECK(has(check_selection(before, good, "block.copy", 0, ctx), "a pattern step changed outside the selection"));
}

// The planted case goes through walk_case, the path of the real cases (prepare, the table, the images, the report): the action runs for real, then a step outside the block is edited, and the report must name it.
// The same case with the real action alone is the control: no finding.
TEST_CASE("qanet select self-test: a planted edit outside the block is found through the walk, the real action alone is not") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  const std::vector<Case> cases = make_cases();
  Case c = case_named(cases, "block 3 steps x 2 tracks (rows 4-6, tracks 1-2), Note column");
  c.acts = {{"step.clear", 0}};
  Report control;
  walk_case(c, card.dir.path(), kReal, control);
  CHECK_MESSAGE(control.setup_errors.empty(), "the control case did not run: " << (control.setup_errors.empty() ? "" : control.setup_errors.front()));
  CHECK(control.runs == 1u);
  CHECK_MESSAGE(control.findings.empty(), "the real step.clear of the block has a finding: " << (control.findings.empty() ? "" : control.findings.front().reason));

  const Do planted = [](sim::Simulator& s, const Act& a) {
    kReal(s, a);
    std::string error;
    // Row 0 of track 0 is outside the block (rows 4-6, tracks 1-2): one more step edited there.
    CHECK_MESSAGE(run_text(s, cursor_to(0, 0) + "action step.note_delta 1\nwait 20\n", error), error);
  };
  Report report;
  walk_case(c, card.dir.path(), planted, report);
  CHECK_MESSAGE(report.setup_errors.empty(), "the planted case did not run: " << (report.setup_errors.empty() ? "" : report.setup_errors.front()));
  CHECK(report.runs == 1u);
  CHECK_MESSAGE(std::any_of(report.findings.begin(), report.findings.end(), [](const Line& l) { return l.reason == "a pattern step changed outside the selection" && l.action == "step.clear"; }),
                "an edit outside the block, planted through walk_case, was not found");
}

TEST_CASE("qanet select self-test: the frames outside a sample selection must stay, and the inside follows the effect") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  const std::vector<Case> cases = make_cases();
  std::unique_ptr<sim::Simulator> s;
  SelImage before;
  Clip clip;
  bool copied = false;
  const bool got = image_of_case(case_named(cases, "Sample edit, a selection in the middle"), card.dir.path(), s, before, clip, copied);
  CHECK(got);
  if (!got || !before.pcm_ok) {
    return;
  }
  const std::uint32_t lo = std::min(before.sed_a, before.sed_b);
  const std::uint32_t hi = std::max(before.sed_a, before.sed_b);
  CHECK(lo > 0);
  CHECK(hi < before.pcm.size());
  CHECK(lo + 8 < hi);
  SelContext ctx;
  ctx.must_act = true;
  auto after_with = [&](const std::vector<std::int16_t>& pcm) {
    SelImage a;
    a.doc = std::make_unique<model::Project>(*before.doc);
    a.ok = true;
    a.digest = before.digest;
    a.screen = before.screen;
    a.slot = before.slot;
    a.pcm_hash = before.pcm_hash;
    a.pcm_hash[static_cast<std::size_t>(before.slot)] ^= 1u;  // the slot's audio is not what it was
    a.pcm = pcm;
    a.pcm_ok = true;
    return a;
  };
  std::vector<std::int16_t> reversed = before.pcm;
  std::reverse(reversed.begin() + lo, reversed.begin() + hi);
  CHECK(check_selection(before, after_with(reversed), "sedit.run", 2, ctx).empty());
  // A frame outside the selection moved too.
  std::vector<std::int16_t> leak = reversed;
  leak[lo - 1] = static_cast<std::int16_t>(leak[lo - 1] + 100);
  CHECK(has(check_selection(before, after_with(leak), "sedit.run", 2, ctx), "sample frames outside the selection changed"));
  leak = reversed;
  leak[hi] = static_cast<std::int16_t>(leak[hi] - 100);
  CHECK(has(check_selection(before, after_with(leak), "sedit.run", 2, ctx), "sample frames outside the selection changed"));
  // The effect was not the one asked for: the selection is as it was, or the whole sample was reversed.
  CHECK(has(check_selection(before, after_with(before.pcm), "sedit.run", 2, ctx), "the selection was not reversed"));
  std::vector<std::int16_t> whole = before.pcm;
  std::reverse(whole.begin(), whole.end());
  CHECK(has(check_selection(before, after_with(whole), "sedit.run", 2, ctx), "sample frames outside the selection changed"));
  // A crop keeps the selection, and nothing else of the sample.
  const std::vector<std::int16_t> cropped(before.pcm.begin() + lo, before.pcm.begin() + hi);
  CHECK(check_selection(before, after_with(cropped), "sedit.run", 0, ctx).empty());
  std::vector<std::int16_t> wrong_crop(before.pcm.begin() + lo - 1, before.pcm.begin() + hi);
  CHECK(has(check_selection(before, after_with(wrong_crop), "sedit.run", 0, ctx), "the sample after a crop is not the selection"));
  // The audio of another slot changed: outside every selection.
  SelImage other = after_with(reversed);
  other.pcm_hash[static_cast<std::size_t>(before.slot) + 1u] ^= 1u;
  CHECK(has(check_selection(before, other, "sedit.run", 2, ctx), "the audio of a sample slot changed outside the selection"));
}

TEST_CASE("qanet select self-test: the inside of insert, ADD SLICE and the song's range follow the spec") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  const std::vector<Case> cases = make_cases();
  SelContext ctx;
  ctx.must_act = true;

  // Insert: the block's tracks shift down from the block's first row through step 127, that step is cleared.
  {
    std::unique_ptr<sim::Simulator> s;
    SelImage before;
    Clip clip;
    bool copied = false;
    const bool got = image_of_case(case_named(cases, "block 3 steps x 2 tracks (rows 4-6, tracks 1-2), Note column"), card.dir.path(), s, before, clip, copied);
    CHECK(got);
    if (got) {
    SelImage good;
    good.doc = std::make_unique<model::Project>(*before.doc);
    for (int t = before.track0; t <= before.track1; ++t) {
      for (unsigned r = static_cast<unsigned>(before.row0); r < model::kMaxPatternLength; ++r) {
        const model::Step want = r == static_cast<unsigned>(before.row0) ? model::kEmptyStep : before.doc->pattern(before.pattern)->steps[t][r - 1u];
        CHECK(good.doc->set_step(before.pattern, static_cast<unsigned>(t), r, want) == model::Status::Ok);
      }
    }
    good.ok = true;
    good.digest = sim::model_digest(*good.doc);
    good.screen = before.screen;
    good.pcm_hash = before.pcm_hash;
    CHECK(check_selection(before, good, "step.insert", 0, ctx).empty());
    // One step of the shifted run is the wrong one.
    SelImage bad;
    bad.doc = std::make_unique<model::Project>(*good.doc);
    model::Step st = bad.doc->pattern(before.pattern)->steps[before.track0][before.row0 + 5];
    st.note = static_cast<std::uint8_t>(st.note == 90 ? 91 : 90);
    CHECK(bad.doc->set_step(before.pattern, static_cast<unsigned>(before.track0), static_cast<unsigned>(before.row0 + 5), st) == model::Status::Ok);
    bad.ok = true;
    bad.digest = sim::model_digest(*bad.doc);
    bad.screen = before.screen;
    bad.pcm_hash = before.pcm_hash;
    CHECK(has(check_selection(before, bad, "step.insert", 0, ctx), "the steps were not shifted down from the block's first row with that step cleared"));
    }
  }

  // ADD SLICE: the new slice at the middle of the selected slice (the decision of 2026-10-21), and one at the middle of the window is not.
  {
    std::unique_ptr<sim::Simulator> s;
    SelImage before;
    Clip clip;
    bool copied = false;
    const bool got = image_of_case(case_named(cases, "16 even slices, slice 3 selected, ZC off"), card.dir.path(), s, before, clip, copied);
    CHECK(got);
    const model::Instrument* in = got ? before.doc->instrument(static_cast<unsigned>(before.slot)) : nullptr;
    CHECK(in != nullptr);
    const bool sixteen = in != nullptr && in->sampler.slice_count == 16;
    CHECK(sixteen);
    CHECK(before.wave_slice == 3);
    if (in != nullptr && in->sampler.slice_count == 16) {
    const unsigned start = in->sampler.slices[3];
    const unsigned end = in->sampler.slices[4];
    auto with_slice = [&](unsigned point) {
      SelImage a;
      a.doc = std::make_unique<model::Project>(*before.doc);
      model::Instrument c = *in;
      unsigned all[17];
      for (unsigned i = 0; i < 16; ++i) {
        all[i] = in->sampler.slices[i];
      }
      all[16] = point;
      std::sort(all, all + 17);
      for (unsigned i = 0; i < 17; ++i) {
        c.sampler.slices[i] = static_cast<std::uint16_t>(all[i]);
      }
      c.sampler.slice_count = 17;
      c.sampler.play_mode = 7;
      CHECK(a.doc->set_instrument(static_cast<unsigned>(before.slot), c) == model::Status::Ok);
      a.ok = true;
      a.digest = sim::model_digest(*a.doc);
      a.screen = before.screen;
      a.pcm_hash = before.pcm_hash;
      return a;
    };
    CHECK(check_selection(before, with_slice((start + end) / 2), "wave.add", 0, ctx).empty());
    const std::vector<Finding> wrong = check_selection(before, with_slice(32768), "wave.add", 0, ctx);
    CHECK_MESSAGE(has(wrong, "the new slice is not between the selected slice's start and end"), "a slice at the window's middle, past the selected slice, passed");
    const std::vector<Finding> quarter = check_selection(before, with_slice(start + (end - start) / 4), "wave.add", 0, ctx);
    CHECK_MESSAGE(has(quarter, "the new slice is not at the middle of the selected slice"), "a slice at the quarter of the selected slice passed");
    }
  }

  // Song: the range that left the list is the range.
  {
    std::unique_ptr<sim::Simulator> s;
    SelImage before;
    Clip clip;
    bool copied = false;
    const bool got = image_of_case(case_named(cases, "Song, 6 slots, range of 2 (slots 2-3)"), card.dir.path(), s, before, clip, copied);
    CHECK(got);
    if (got) {
    CHECK(before.song_first == 2);
    CHECK(before.song_last == 3);
    auto order_without = [&](int drop) {
      SelImage a;
      a.doc = std::make_unique<model::Project>(*before.doc);
      std::vector<unsigned> ids;
      for (unsigned i = 0; i < a.doc->song_length(); ++i) {
        ids.push_back(a.doc->order(i));
      }
      ids.erase(ids.begin() + drop, ids.begin() + drop + 2);
      (void)a.doc->clear_order();
      for (unsigned id : ids) {
        (void)a.doc->append_order(id);
      }
      a.ok = true;
      a.digest = sim::model_digest(*a.doc);
      a.screen = before.screen;
      a.pcm_hash = before.pcm_hash;
      return a;
    };
    CHECK(check_selection(before, order_without(2), "song.cut", 0, ctx).empty());
    CHECK(has(check_selection(before, order_without(1), "song.cut", 0, ctx), "the slots that left the list are not the range"));
    }
  }
}

// OS-BUG-R3SEL: the two walk findings that were the net's, not the code's. A single step stepped below C-0 goes on into Fade, Cut and Off (model::edit::note_delta clamps at -4); the net judged it as a
// clamp at C-0, which the OS-009 table gives to several steps only. And the FADERS page of the VU pads (the picker's VU twice) was not in the chain the walk rebuilds, so a pad there (pads.play: a track volume)
// was judged as step.note_pad.
TEST_CASE("qanet select: a single step stepped below C-0 leaves the notes, a note at the top still clamps at 127") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  const std::vector<Case> cases = make_cases();
  std::unique_ptr<sim::Simulator> s;
  SelImage before;
  Clip clip;
  bool copied = false;
  const bool got = image_of_case(case_named(cases, "Pattern, no selection (cursor row 6, track 2), Note column"), card.dir.path(), s, before, clip, copied);
  CHECK(got);
  if (!got || !before.ok) {
    return;
  }
  CHECK(before.row0 == before.row1);
  CHECK(before.track0 == before.track1);
  SelContext ctx;
  ctx.must_act = true;
  const unsigned t = static_cast<unsigned>(before.cur_track);
  const unsigned r = static_cast<unsigned>(before.cur_row);
  // The image with the cursor's step holding `note`.
  auto with_note = [&](const SelImage& base, std::uint8_t note) {
    SelImage im = base;
    im.doc = std::make_shared<model::Project>(*base.doc);
    model::Step st = im.doc->pattern(base.pattern)->steps[t][r];
    st.note = note;
    st.instr = note >= model::kNoteFirst && note <= model::kNoteLast ? 1 : 0;
    CHECK(im.doc->set_step(base.pattern, t, r, st) == model::Status::Ok);
    im.digest = sim::model_digest(*im.doc);
    return im;
  };
  const SelImage at_c0 = with_note(before, model::kNoteFirst);
  CHECK(check_selection(at_c0, with_note(at_c0, model::kNoteFade), "step.note_delta", -2, ctx).empty());   // C-0 less 2: Fade (the model's rank order)
  CHECK(check_selection(at_c0, with_note(at_c0, model::kNoteEmpty), "step.note_delta", -1, ctx).empty());  // less 1: Empty
  CHECK(has(check_selection(at_c0, with_note(at_c0, model::kNoteFirst), "step.note_delta", -2, ctx), "a single step moved below C-0 is still a note"));  // clamped at C-0: the old net's answer
  CHECK(check_selection(at_c0, with_note(at_c0, model::kNoteFirst + 2), "step.note_delta", 2, ctx).empty());  // above: a plain step
  const SelImage at_top = with_note(before, model::kNoteLast);
  CHECK(check_selection(at_top, with_note(at_top, model::kNoteLast), "step.note_delta", 5, ctx).empty());  // the clamp at 127 holds
  CHECK(has(check_selection(at_top, with_note(at_top, model::kNoteLast - 1), "step.note_delta", 5, ctx), "a note was not moved by the delta, clamped to 0..127"));
}

TEST_CASE("qanet select: the FADERS page of the VU pads is the PadsMode context of the chain") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  const std::vector<Case> cases = make_cases();
  Case c = case_named(cases, "Pattern, no selection (cursor row 6, track 2), Note column");
  const std::string pick_vu = "press shift\ntap f2\nrelease shift\ntap f6\n";  // the PADS picker (Shift + F2), then its F6 VU
  c.steps += pick_vu;
  std::unique_ptr<sim::Simulator> s;
  SelImage im;
  Clip clip;
  bool copied = false;
  CHECK(image_of_case(c, card.dir.path(), s, im, clip, copied));
  if (s == nullptr) {
    return;
  }
  CHECK(s->app().state().padmode.fixed() == app::PadMode::Vu);
  CHECK(!s->app().state().padmode.faders());
  CHECK(context_live(*s, app::Context::PadsMode));
  c.steps += pick_vu;  // VU again: the second page
  CHECK(image_of_case(c, card.dir.path(), s, im, clip, copied));
  if (s == nullptr) {
    return;
  }
  CHECK(s->app().state().padmode.faders());
  CHECK(context_live(*s, app::Context::PadsMode));
}
