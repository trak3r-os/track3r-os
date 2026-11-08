// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET: the rules over a state before and after one action (rules.h). New Community OS behaviour, test tooling only.
#include "rules.h"

#include <o3os/app/state_line.h>
#include <o3os/app/mixer_view.h>
#include <o3os/app/model.h>
#include <o3os/app/pattern_edit.h>
#include <o3os/app/view_state.h>
#include <o3os/app/sample_bank.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/sim/scenario_assert.h>
#include <o3os/sim/sim_audio.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;

namespace qanet {

// The value of `key=` in the state line ("" when absent).
std::string state_field(const std::string& line, const char* key) {
  const std::string k = std::string(key) + "=";
  std::size_t at = 0;
  while ((at = line.find(k, at)) != std::string::npos) {
    if (at == 0 || line[at - 1] == ' ') {
      const std::size_t end = line.find(' ', at);
      return line.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
    }
    at += k.size();
  }
  return std::string();
}

namespace {

std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {
  const auto* b = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    h = (h ^ b[i]) * 1099511628211ull;
  }
  return h;
}

static_assert(kInstSlots == model::kMaxInstruments && kPcmSlots == app::SampleBank::kSlots, "the snapshot holds one entry per instrument slot and per sample slot");

constexpr std::size_t kLine = 768;  // the state line's buffer (the scenario runner's, sim/src/scenario.cpp): a shorter one cuts the tail off

// The actions that may lower the undo depth, and that need no unit and no question to change the document: File NEW and OPEN replace it, undo and redo move along it.
bool exempt(const std::string& action) {
  return action == "file.new" || action == "file.open" || action == "edit.undo" || action == "edit.redo" || action == "sedit.undo" || action == "sedit.redo" ||
         action == "events.undo" || action == "events.redo";  // OS-MIDIREC S2c: F7 / Shift + F7 inside the EVENTS list run the grid's undo
}

}  // namespace

namespace {

// The value of the parameter a click group is keyed by (Editor::merge_hint: a core::ParamId, or kTempoMergeKey for the project tempo).
bool read_keyed(const model::Project& p, unsigned key, float& out) {
  if (key == static_cast<unsigned>(model::kTempoMergeKey)) {
    out = static_cast<float>(p.tempo_x100());
    return true;
  }
  const core::ParamId id = static_cast<core::ParamId>(key);
  if (!p.has_param(id)) {
    return false;
  }
  out = p.get_param(id);
  return true;
}

}  // namespace

Snap snap_of(sim::Simulator& s, const Snap* prev) {
  Snap out;
  char line[kLine];
  app::describe_state(s.app(), line, sizeof line);
  const std::string text = line;
  out.keys = state_field(text, "keys");
  out.overlay = state_field(text, "overlay");
  out.screen = state_field(text, "screen");
  out.msg = state_field(text, "msg");
  out.posts = static_cast<unsigned>(std::strtoul(state_field(text, "posts").c_str(), nullptr, 10));
  for (std::size_t at = 0; at < text.size();) {  // `key=value` words, separated by single spaces (a value has none)
    std::size_t end = text.find(' ', at);
    end = end == std::string::npos ? text.size() : end;
    const std::size_t eq = text.find('=', at);
    if (eq != std::string::npos && eq < end) {
      out.fields[text.substr(at, eq - at)] = text.substr(eq + 1, end - eq - 1);
    }
    at = end + 1;
  }
  {
    const app::Selection& sel = s.app().state().edit.selection;
    out.selection = std::string(sel.active ? "A" : "-") + (sel.extending ? "E" : "-") + (sel.column ? "C" : "-") + ":" + std::to_string(sel.start_row) + "," + std::to_string(sel.start_track) + "," +
                    std::to_string(sel.end_row) + "," + std::to_string(sel.end_track);
  }
  out.heard = static_cast<unsigned>(s.heard_params().size());
  {
    const std::uint8_t* px = s.raster().rgba();  // the picture, byte by byte: the frame is 1.5 MB and a hash of it costs a millisecond or two
    std::uint64_t h = 1469598103934665603ull;
    h = fnv(h, px, sim::EveRaster::kFramebufferBytes);
    out.frame = h;
  }
  if (s.has_audio()) {
    out.note_ons = s.audio()->voices().stats().note_ons;
  }
  if (s.project() != nullptr) {
    out.digest = sim::model_digest(*s.project());
    out.digest_error = out.digest.empty();
  }
  if (s.project() != nullptr) {  // the classes of rule 1 (the undo policy of 2026-10-22): the pattern data and the song, and each instrument slot with its parameters (the struct holds both)
    const model::Project& p = *s.project();
    std::uint64_t pat = 1469598103934665603ull;
    for (unsigned i = 0; i < model::kMaxPatterns; ++i) {
      const std::uint32_t g = p.pattern_generation(i);
      pat = fnv(pat, &g, sizeof g);
    }
    const std::uint32_t song = p.song_generation();
    pat = fnv(pat, &song, sizeof song);
    for (std::uint32_t i = 0; i < p.song_length(); ++i) {
      const std::uint8_t o = p.order(i);
      pat = fnv(pat, &o, sizeof o);
    }
    out.pattern_hash = pat;
    {  // the project-level parameters outside the Mixer's own (D10): tempo, swing, the pattern default and the track count, the track types, the note, mute-mode and scene state
      std::uint64_t h = 1469598103934665603ull;
      const std::uint32_t tempo = p.tempo_x100();
      const std::uint8_t small[3] = {p.swing(), p.default_pattern_length(), p.active_tracks()};
      h = fnv(h, &tempo, sizeof tempo);
      h = fnv(h, small, sizeof small);
      for (unsigned t = 0; t < model::kMaxTracks; ++t) {
        const std::uint8_t type = static_cast<std::uint8_t>(p.track(t)->type);
        h = fnv(h, &type, sizeof type);
      }
      h = fnv(h, &p.note(), sizeof p.note());
      h = fnv(h, &p.mute(), sizeof p.mute());
      h = fnv(h, &p.scenes(), sizeof p.scenes());
      // the project's own settings (AntiAliasing, LimiterMode, SendsMode: the raw value bytes) and its MIDI Learn store, the title, and the APPM sets: the Editor journals them all
      h = fnv(h, p.settings().values(), model::Settings::kValueBytes);
      h = fnv(h, p.settings().learn_slots(), core::kLearnSlots * sizeof(core::LearnBinding));
      h = fnv(h, p.name(), std::strlen(p.name()));
      for (std::uint32_t i = 0; i < p.app_params_count(); ++i) {
        const model::AppParams* ap = p.app_params(i);
        h = fnv(h, ap->name, sizeof ap->name);
        const std::uint8_t abi[3] = {ap->abi_major, ap->abi_minor, ap->count};
        h = fnv(h, abi, sizeof abi);
        h = fnv(h, ap->value, ap->count * sizeof(float));
      }
      out.project_params = h;
    }
    for (unsigned i = 0; i < model::kMaxInstruments; ++i) {
      const model::Instrument* in = p.instrument(i);
      out.inst_present[i] = in != nullptr;
      out.inst_hash[i] = in != nullptr ? fnv(1469598103934665603ull, in, sizeof *in) : 0u;
    }
  }
  const app::SampleBank* bank = s.app().model().samples;
  if (bank != nullptr) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
      const app::SampleBank::Pcm pcm = bank->pcm(slot);
      h = fnv(h, &pcm.frames, sizeof pcm.frames);
      std::uint64_t one = fnv(1469598103934665603ull, &pcm.frames, sizeof pcm.frames);
      if (pcm.data != nullptr) {
        h = fnv(h, pcm.data, pcm.frames * sizeof(std::int16_t));
        one = fnv(one, pcm.data, pcm.frames * sizeof(std::int16_t));
      }
      if (slot < kPcmSlots) {
        out.pcm_frames[slot] = pcm.frames;
        out.pcm_hash[slot] = one;
      }
      out.sedit += bank->undo_depth(slot);
      out.sedit_slot = bank->undo_depth(slot) != 0u ? static_cast<int>(slot) : out.sedit_slot;
      out.sedit_full = out.sedit_full || bank->undo_depth(slot) >= app::SampleBank::kHistory;
    }
    char hex[24];
    std::snprintf(hex, sizeof hex, "/%016llx", static_cast<unsigned long long>(h));
    out.digest += hex;
  }
  const model::Editor* ed = s.app().session().document();
  if (ed != nullptr) {
    static const model::UndoArea areas[kRings] = {model::UndoArea::Pattern, model::UndoArea::Song, model::UndoArea::Sound};
    for (unsigned i = 0; i < kRings; ++i) {
      const model::UndoJournal& j = ed->journal(areas[i]);
      out.ring[i] = j.depth();
      out.added[i] = j.depth() + j.dropped_units();
      out.merge[i] = j.merge_key();
      out.keyed_key[i] = out.merge[i] != 0 ? out.merge[i] : (prev != nullptr ? prev->merge[i] : 0u);
      if (out.keyed_key[i] != 0 && s.project() != nullptr) {
        out.keyed_ok[i] = read_keyed(*s.project(), out.keyed_key[i], out.keyed[i]);
      }
    }
  }
  return out;
}

bool question_up(const Snap& s) { return !s.keys.empty() && (s.overlay == "NONE" || s.overlay == "NOTIFY"); }

namespace {

// Whether ring `i` took a click group again in this step: a key was up before, the parameter it names has another value now, and the ring's units did not grow (the unit was reopened and patched; one that
// came back to where it began is gone, which lowers `added` by that one). The key alone proves nothing: it stays up after a keyed edit while anything else changes the document behind the journal.
bool merged(const Snap& before, const Snap& after, unsigned i) {
  return before.merge[i] != 0 && before.keyed_ok[i] && after.keyed_ok[i] && after.keyed_key[i] == before.merge[i] && before.keyed[i] != after.keyed[i] && after.added[i] <= before.added[i] &&
         after.added[i] + 1u >= before.added[i];
}

// Whether the step recorded an undo unit. A new unit raises `added` of a ring (also when a full ring evicted its oldest to make room); a reopened click group is the same unit with new content (merged());
// a sample-edit history that did not grow cannot tell by its depth: the bank keeps no count of evictions, and an edit pushes a step and drops the oldest at the same depth, with the history full (kHistory steps, see
// Snap::sedit_full) or short of room (edit_begin frees the oldest step when the arena has no copy to spare for a large sample, found by the B8 fixture).
// B8 (OS-QANET exhaust, the blind spot of B4): the credit then goes to a change CONFINED to the slot the history belongs to AFTER the step (its audio, which must have changed, and its instrument), which is all an edit
// of the Sample edit screen touches. It used to go to ANY change once the history was full, so a silent change of the pattern, the song, another instrument or another slot's audio passed. The history may have been
// another slot's before (the first edit after a slot switch drops it: the bank's rule, SampleBank::edit_begin), so the slot is the one of `after`. LIMITS that stay: a change of the history slot's own audio by
// another path (a WAV load over it) looks like an edit here, at any depth 1..kHistory (3 of 50 such loads in the 200 seeds of the random user, sedit 3>3, no ring unit, no question, are still credited; lane A owns
// that rule), and so does an eviction for room at a partial depth.
bool sedit_confined(const Snap& before, const Snap& after) {
  const int s = after.sedit_slot;
  if (s < 0 || s >= static_cast<int>(kPcmSlots) || before.pcm_hash[s] == after.pcm_hash[s] || before.pattern_hash != after.pattern_hash || before.project_params != after.project_params) {
    return false;
  }
  for (unsigned i = 0; i < kInstSlots; ++i) {
    if (static_cast<int>(i) != s && (before.inst_present[i] != after.inst_present[i] || before.inst_hash[i] != after.inst_hash[i])) {
      return false;
    }
  }
  for (unsigned i = 0; i < kPcmSlots; ++i) {
    if (static_cast<int>(i) != s && (before.pcm_frames[i] != after.pcm_frames[i] || before.pcm_hash[i] != after.pcm_hash[i])) {
      return false;
    }
  }
  return true;
}

bool unit_recorded(const Snap& before, const Snap& after, bool changed) {
  if (after.sedit > before.sedit || (changed && after.sedit >= 1u && sedit_confined(before, after))) {
    return true;
  }
  for (unsigned i = 0; i < kRings; ++i) {
    if (after.added[i] > before.added[i] || (changed && merged(before, after, i))) {
      return true;
    }
  }
  return false;
}

// Whether the step changed something the undo policy covers: the pattern data or the song, an instrument that was there (changed or removed: its parameters are in the struct), or the audio of a slot that held some.
// What a step adds to an empty slot is not a change of anything that was there.
bool covers(const Snap& before, const Snap& after) {
  if (before.pattern_hash != after.pattern_hash || before.project_params != after.project_params) {
    return true;
  }
  for (unsigned i = 0; i < kInstSlots; ++i) {
    if (before.inst_present[i] && (!after.inst_present[i] || before.inst_hash[i] != after.inst_hash[i])) {
      return true;
    }
  }
  for (unsigned i = 0; i < kPcmSlots; ++i) {
    if (before.pcm_frames[i] != 0u && (after.pcm_frames[i] != before.pcm_frames[i] || after.pcm_hash[i] != before.pcm_hash[i])) {
      return true;
    }
  }
  return false;
}

// Whether the step put something into a slot that was free: a new instrument, or the audio of a sample slot that held none (the only steps whose exempt change may also clear the history).
bool added_to_free_slot(const Snap& before, const Snap& after) {
  for (unsigned i = 0; i < kInstSlots; ++i) {
    if (!before.inst_present[i] && after.inst_present[i]) {
      return true;
    }
  }
  for (unsigned i = 0; i < kPcmSlots; ++i) {
    if (before.pcm_frames[i] == 0u && after.pcm_frames[i] != 0u) {
      return true;
    }
  }
  return false;
}

// The destructive actions outside the three classes: they have no undo unit, so they ask first. The maintainer named two (2026-10-22), both on the Mixer's STEMS page: Delete (the cursor's source leaves its slot)
// and Shift + Delete (the whole map back to the factory slots). The rest of the Mixer is exempt, a reset of one level included. Sample load over a used slot is covered by the audio rule above, not named here.
bool must_ask(const Snap& before, const std::string& action) {
  constexpr unsigned kStemsPage = static_cast<unsigned>(app::MixerPage::Stems);
  const auto page = before.fields.find("page");
  return action == "mixer.reset" && before.screen == "MIXER" && page != before.fields.end() && page->second == std::to_string(kStemsPage);
}

// RULE 1. "" when the step is fine, else the reason (a fixed phrase: it is part of the baseline's line).
std::string rule1(const Snap& before, const Snap& after, const std::string& action, bool answer) {
  if (exempt(action)) {
    return std::string();
  }
  const bool changed = before.digest != after.digest;
  const bool unit = unit_recorded(before, after, changed);
  // The sample editor's history may shrink under an edit: SampleBank::edit_begin frees the oldest step when the arena has no room for another copy of the sample (found by the B8 fixture on a large sample), so the
  // new step is there and older ones are gone. That is an edit confined to the history's slot that leaves a step; any other lowering is one.
  const bool evicted = after.sedit < before.sedit && after.sedit >= 1u && sedit_confined(before, after);  // (also the first edit after a slot switch: the other slot's history is dropped)
  bool lowered = after.sedit < before.sedit && !evicted;
  for (unsigned i = 0; i < kRings; ++i) {
    lowered = lowered || (after.added[i] < before.added[i] && !merged(before, after, i));  // a clear, or a unit too big for its ring; evictions for room and a click group that came back to its start do not count
  }
  // The undo policy (maintainer, 2026-10-22): a change to the pattern data, to an instrument or its parameters, or to the audio of a sample must leave an undo unit; a destructive change outside those asks first;
  // every other change is exempt, and so is what a step only ADDS to an empty slot (a load into a free slot, a new instrument in an empty slot): such a step may also clear the history.
  const bool covered = changed && (covers(before, after) || must_ask(before, action));
  const bool silent = covered && !(answer && question_up(before)) && !unit;
  if (changed && !covered && (!lowered || added_to_free_slot(before, after))) {
    return std::string();  // every change exempt; the history may only go down with a load or a new instrument into a free slot
  }
  if (silent && lowered) {
    return "document changed with no question and no undo unit; undo history cleared";
  }
  if (silent) {
    return "document changed with no question and no undo unit";
  }
  if (lowered) {
    return "undo history lowered";
  }
  return std::string();
}

}  // namespace

std::vector<Finding> check_rules(const Snap& before, const Snap& after, const std::string& action, bool answer) {
  std::vector<Finding> out;
  if (before.digest_error || after.digest_error) {
    out.push_back(Finding{"R0", "the native writer failed on the document: no digest to compare"});  // a step that cannot be judged must not pass
    return out;
  }
  const std::string r1 = rule1(before, after, action, answer);
  if (!r1.empty()) {
    out.push_back(Finding{"R1", r1});
  }
  return out;
}

bool differs(const Snap& a, const Snap& b, bool drifting, bool picture) {
  if (a.digest != b.digest || a.selection != b.selection || a.heard != b.heard) {
    return true;
  }
  if (!drifting && (a.note_ons != b.note_ons || (picture && a.frame != b.frame))) {
    return true;
  }
  static const char* const kDrift[] = {"row", "play_step", "play_pattern", "order"};
  static const char* const kSaid[] = {"posts", "msg"};  // what the key said, not what it changed
  auto skipped = [&](const std::string& name) {
    for (const char* n : kSaid) {
      if (name == n) {
        return true;
      }
    }
    if (drifting) {
      for (const char* n : kDrift) {
        if (name == n) {
          return true;
        }
      }
    }
    return false;
  };
  // The notification strip is an overlay of the state line (`overlay=NOTIFY`): a key that posts one moves the field from NONE, and that is what the key said, not what it changed.
  auto shown = [](const std::string& name, const std::string& value) { return name == "overlay" && value == "NOTIFY" ? std::string("NONE") : value; };
  for (const auto& [name, value] : a.fields) {
    const auto it = b.fields.find(name);
    if (!skipped(name) && (it == b.fields.end() || shown(name, it->second) != shown(name, value))) {
      return true;
    }
  }
  for (const auto& [name, value] : b.fields) {
    if (!skipped(name) && a.fields.count(name) == 0) {
      return true;
    }
  }
  return false;
}

bool key_acted(const Snap& ref, const Snap& after, bool drifting) {
  const bool said = ref.posts != after.posts || ref.msg != after.msg;
  return differs(ref, after, drifting, !said);
}

std::vector<Finding> check_key(const KeyFacts& key, const Snap& ref, const Snap& after, bool drifting) {
  std::vector<Finding> out;
  if (ref.digest_error || after.digest_error) {
    out.push_back(Finding{"R0", "the native writer failed on the document: no digest to compare"});
    return out;
  }
  const bool said = ref.posts != after.posts || ref.msg != after.msg;
  const bool acted = key_acted(ref, after, drifting);
  if (key.fkey && key.row_known && key.labelled && !key.bound) {
    out.push_back(Finding{"R2", "the key row shows a label and the key is bound to nothing"});
  }
  if (key.bound && !acted && !said) {
    out.push_back(Finding{"R2", "bound key changes nothing and says nothing"});
  }
  if (key.bound && key.fkey && key.row_known && !key.labelled && (acted || said)) {
    out.push_back(Finding{"R2", "bound key acts or says something and the key row shows no label"});
  }
  return out;
}

// What moves while an action's job runs, cheap enough to read every 10 ms (the digest hashes the whole project and is for the end): the project's generation (every accepted mutation), the undo depths,
// the notifications, the screen, and the sample bank's frames.
namespace {

struct Pulse {
  std::uint32_t generation = 0;
  unsigned units = 0;  // depth + dropped over the rings (an eviction for room moves the second)
  std::uint64_t audio = 0;
  std::string where;
  bool operator==(const Pulse& o) const { return generation == o.generation && units == o.units && audio == o.audio && where == o.where; }
};

Pulse pulse_of(sim::Simulator& s) {
  Pulse p;
  char line[kLine];
  app::describe_state(s.app(), line, sizeof line);
  const std::string text = line;
  p.where = state_field(text, "screen") + "/" + state_field(text, "overlay") + "/" + state_field(text, "posts");
  if (s.project() != nullptr) {
    p.generation = s.project()->generation();
  }
  if (const model::Editor* ed = s.app().session().document()) {
    for (const model::UndoArea a : {model::UndoArea::Pattern, model::UndoArea::Song, model::UndoArea::Sound}) {
      p.units += ed->journal(a).depth() + ed->journal(a).dropped_units();
    }
  }
  if (const app::SampleBank* bank = s.app().model().samples) {
    for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
      p.audio = p.audio * 31u + bank->pcm(slot).frames + bank->undo_depth(slot);
    }
  }
  return p;
}

}  // namespace

void settle(sim::Simulator& s, unsigned limit_ms) {
  Pulse last = pulse_of(s);
  unsigned still = 0;
  for (unsigned t = 0; t < limit_ms && still < 6; t += 10) {
    s.wait_ms(10);
    const Pulse now = pulse_of(s);
    still = now == last ? still + 1 : 0;
    last = now;
  }
}

}  // namespace qanet
