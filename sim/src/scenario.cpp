// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/scenario.h>

#include <o3os/app/actions.h>
#include <o3os/app/state_line.h>
#include <o3os/platform/host/sha256.h>
#include <o3os/platform/host/slow_card.h>
#include <o3os/sim/audio_check.h>
#include <o3os/sim/scenario_assert.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace o3os::sim {

namespace {

std::string lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

// Whole-string integer in [lo, hi]; an optional leading + or - is allowed.
bool parse_int(const std::string& s, long lo, long hi, long& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || *end != '\0' || v < lo || v > hi) {
    return false;
  }
  out = v;
  return true;
}

bool find_button(const std::string& name, app::Button& out) {
  const std::string key = lower(name);
  for (const app::ButtonName& b : app::kButtonNames) {
    if (key == b.name) {
      out = b.button;
      return true;
    }
  }
  return false;
}

// The words of a line: split at white space; a run in double quotes is part of a word, spaces included, and the quotes go ("Projects/Track 01/project.o3pj" is one word).
// false: a quote is never closed.
bool split_words(const std::string& line, std::vector<std::string>& out) {
  out.clear();
  std::string word;
  bool in_word = false;
  bool quoted = false;
  for (const char c : line) {
    if (c == '"') {
      quoted = !quoted;
      in_word = true;
    } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
      if (in_word) {
        out.push_back(word);
        word.clear();
        in_word = false;
      }
    } else {
      word += c;
      in_word = true;
    }
  }
  if (quoted) {
    return false;
  }
  if (in_word) {
    out.push_back(word);
  }
  return true;
}

// A render's name: letters, digits and _, up to 64 (no '.', which names a stems render's parts, and no '-' or '+', which a value of `assert audio` reads as an operator).
bool valid_render_name(const std::string& s) {
  if (s.empty() || s.size() > 64) {
    return false;
  }
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
      return false;
    }
  }
  return true;
}

std::string trimmed(const std::string& s) {
  const std::size_t a = s.find_first_not_of(" \t\r");
  const std::size_t b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1u);
}

bool valid_snap_name(const std::string& s) {
  if (s.empty() || s.size() > 64) {
    return false;
  }
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
      return false;
    }
  }
  return true;
}

// Why the assertion `cmd` does not hold now, or "". Reads the simulator and changes nothing. `marks` maps a label to the digest `mark model` took.
std::string assertion_problem(const ScenarioCommand& cmd, const Simulator& sim, const std::map<std::string, std::string>& marks, std::string* summary) {
  using Target = ScenarioCommand::Target;
  std::string why;
  switch (cmd.target) {
    case Target::State: {
      std::vector<StatePair> pairs;
      (void)parse_state_pairs(cmd.args, pairs, why);  // (it parsed: the parser ran the same call)
      char line[768];
      (void)app::describe_state(sim.app(), line, sizeof line);
      return state_problem(line, pairs);
    }
    case Target::Model: {
      std::string path, value;
      (void)split_model_args(cmd.args, path, value);
      const model::Project* project = sim.project();
      if (project == nullptr) {
        return "there is no document (a `document` line opens one)";
      }
      if (path != "digest") {
        return model_problem(*project, path, value);
      }
      const std::string have = model_digest(*project);
      if (have.empty()) {
        return "the native writer failed on the document";
      }
      const bool labelled = value[0] == '@';
      const auto marked = marks.find(value.substr(1));
      if (labelled && marked == marks.end()) {
        return "'mark model " + value.substr(1) + "' has not run yet";
      }
      const std::string want = labelled ? marked->second : lower(value);
      return have == want ? std::string() : "the document's digest is " + have + ", " + (labelled ? "marked as " + value.substr(1) + ": " : "wanted ") + want;
    }
    case Target::File: {
      std::string path;
      FileWant want;
      (void)parse_file_want(cmd.args, path, want, why);
      return file_problem(sim.card_storage(), path, want);
    }
    case Target::Audio: {
      AudioAssert a;
      (void)parse_audio_assert(cmd.args, a, why);  // (it parsed: the parser ran the same call)
      std::uint32_t left = 0;
      const std::string running = sim.render_running(&left);
      const auto uses = [&](const std::string& name) { return !running.empty() && (name == running || name.compare(0, running.size() + 1u, running + ".") == 0); };
      std::string wait_for = uses(a.name) ? a.name : std::string();
      for (const AudioCheck& c : a.checks) {
        if (wait_for.empty() && uses(c.ref)) wait_for = c.ref;
        for (const std::string& other : c.others) {
          if (wait_for.empty() && uses(other)) wait_for = other;
        }
      }
      if (!wait_for.empty()) {
        return "render '" + running + "' is still running (" + std::to_string(left) + " ms more): `wait` for it to end first";
      }
      return audio_problem(a, [&sim](const std::string& name) { return sim.render_result(name); }, summary);
    }
  }
  return std::string();
}

}  // namespace

bool parse_scenario(const std::string& text, std::vector<ScenarioCommand>& out, std::string& error) {
  out.clear();
  std::set<std::string> snaps;
  std::set<std::string> marks;  // the labels of the `mark model` lines so far
  std::set<std::string> renders;  // the names `render` lines have made so far (a stems render's parts too)
  std::set<std::string> fingerprinted;  // the renders a `fingerprint` line has taken so far
  bool document_seen = false;     // a `document` line came before (`audio on` needs it)
  bool audio_seen = false;        // `audio on` came before
  bool firmware_seen = false;     // `firmware on` came before (OS-FWCONFIG)
  bool apps_seen = false;         // `apps on` came before
  bool document_new = false;      // the last `document` line had the word `new` (a project sink: File > NEW resets the document in place)
  std::istringstream lines(text);
  std::string line;
  int number = 0;
  const auto bad = [&](const std::string& what) {
    error = "line " + std::to_string(number) + ": " + what;
    return false;
  };
  while (std::getline(lines, line)) {
    ++number;
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line.erase(hash);
    }
    std::vector<std::string> w;
    if (!split_words(line, w)) {
      return bad("a double quote is never closed");
    }
    if (w.empty()) {
      continue;
    }
    ScenarioCommand cmd;
    cmd.line = number;
    const std::string verb = lower(w[0]);
    long n = 0;
    if (verb == "wait") {
      if (w.size() != 2 || !parse_int(w[1], 0, 3600000, n) || w[1][0] == '+' || w[1][0] == '-') {
        return bad("wait needs one number of milliseconds, 0..3600000");
      }
      cmd.kind = ScenarioCommand::Kind::Wait;
      cmd.value = static_cast<std::uint32_t>(n);
    } else if (verb == "press" || verb == "release" || verb == "tap") {
      if (w.size() < 2 || w.size() > (verb == "tap" ? 3u : 2u)) {
        return bad(verb + (verb == "tap" ? " needs a button name and an optional count" : " needs one button name"));
      }
      if (!find_button(w[1], cmd.button)) {
        return bad("unknown button '" + w[1] + "'");
      }
      cmd.kind = verb == "press" ? ScenarioCommand::Kind::Press
                                 : (verb == "release" ? ScenarioCommand::Kind::Release : ScenarioCommand::Kind::Tap);
      cmd.value = 1;
      if (w.size() == 3) {
        if (!parse_int(w[2], 1, 1000, n) || w[2][0] == '+' || w[2][0] == '-') {
          return bad("tap count must be 1..1000");
        }
        cmd.value = static_cast<std::uint32_t>(n);
      }
    } else if (verb == "pad") {
      long ms = 0;
      const std::string how = w.size() >= 3 ? lower(w[2]) : std::string();
      const bool hold = w.size() == 4 && how == "hold";
      const bool half = w.size() == 3 && (how == "down" || how == "up");
      if (w.size() < 2 || w.size() > 4 || !parse_int(w[1], 0, 47, n) || w[1][0] == '+' || w[1][0] == '-' || (w.size() > 2 && !hold && !half) ||
          (hold && (!parse_int(w[3], 1, 60000, ms) || w[3][0] == '+' || w[3][0] == '-'))) {
        return bad("pad needs one index, 0..47, and optionally 'hold <ms>' (1..60000), 'down' or 'up'");
      }
      cmd.kind = ScenarioCommand::Kind::Pad;
      cmd.value = static_cast<std::uint32_t>(n);
      cmd.delta = static_cast<std::int32_t>(ms);
      cmd.mode = hold ? std::uint8_t{1} : (half ? (how == "down" ? std::uint8_t{2} : std::uint8_t{3}) : std::uint8_t{0});
    } else if (verb == "encoder") {
      long count = 1, every = 1;
      const bool rep = w.size() >= 3 && lower(w[2]).size() > 1 && lower(w[2])[0] == 'x';
      if (w.size() < 2 || w.size() > 5 || !parse_int(w[1], -1000, 1000, n) || n == 0 ||
          (w.size() >= 3 && (!rep || !parse_int(lower(w[2]).substr(1), 2, 1000, count) || w[2][1] == '+' || w[2][1] == '-')) ||
          (w.size() == 4) || (w.size() == 5 && (lower(w[3]) != "every" || !parse_int(w[4], 2, 255, every) || w[4][0] == '+' || w[4][0] == '-'))) {
        return bad("encoder needs one non-zero number of detents, -1000..1000, and optionally 'x<k>' (2..1000 turns) and then 'every <ms>' (2..255)");
      }
      cmd.kind = ScenarioCommand::Kind::Encoder;
      cmd.delta = static_cast<std::int32_t>(n);
      cmd.value = static_cast<std::uint32_t>(count);
      cmd.mode = static_cast<std::uint8_t>(every);
    } else if (verb == "action") {
      if (w.size() < 2 || w.size() > 3) {
        return bad("action needs a name and an optional integer argument");
      }
      const std::string name = lower(w[1]);
      const app::ActionDef* def = app::find_action(app::action_registry(), name.c_str(), name.size());
      if (def == nullptr) {
        return bad("unknown action '" + w[1] + "'");
      }
      if (w.size() == 3 && !parse_int(w[2], -2147483647L, 2147483647L, n)) {
        return bad("action argument must be an integer");
      }
      cmd.kind = ScenarioCommand::Kind::Action;
      cmd.action = def->id;
      cmd.arg = w.size() == 3 ? static_cast<core::ActionArg>(n) : 0;
      cmd.name = name;
    } else if (verb == "document") {
      std::uint32_t flags = 0;  // 1: bindings, 2: new (OS-NEWKEEP); bits 8..15: the active tracks (OS-ZOOM, `tracks <n>`), 0 the demo's
      for (std::size_t i = 1; i < w.size(); ++i) {
        const std::string word = lower(w[i]);
        if (word == "tracks" && (flags >> 8) == 0u && i + 1 < w.size()) {
          long count = 0;
          if (!parse_int(w[i + 1], 1, 16, count) || w[i + 1][0] == '+' || w[i + 1][0] == '-') {
            return bad("document tracks needs a count 1..16");
          }
          flags |= static_cast<std::uint32_t>(count) << 8;
          ++i;
          continue;
        }
        const std::uint32_t bit = word == "bindings" ? 1u : (word == "new" ? 2u : (word == "sound" ? 4u : (word == "tb303" ? 8u : (word == "inserts" ? 16u : 0u))));
        if (bit == 0u || (flags & bit) != 0u) {
          return bad("document takes nothing, or the words 'bindings', 'new', 'sound', 'tb303', 'inserts' and 'tracks <n>'");
        }
        flags |= bit;
      }
      if (audio_seen) {
        return bad("document: refused after `audio on` (the engine's sequencer plays the document that was open, in place): put `document` before it");
      }
      document_seen = true;
      document_new = (flags & 2u) != 0u;
      cmd.kind = ScenarioCommand::Kind::Document;
      cmd.value = flags;
    } else if (verb == "playhead") {
      if (audio_seen) {
        return bad("playhead: refused after `audio on` (the sequencer publishes the position now; Play starts it)");
      }
      long step = 0, ustep = 0, left = 0, wait = 0;
      std::size_t nwords = w.size();
      if (nwords >= 3 && lower(w[nwords - 2]) == "countin") {  // OS-METRO: `playhead <step> [<ustep>] countin <steps>`: the steps of the count-in still to run, the running one included (1..32)
        if (!parse_int(w[nwords - 1], 1, 32, left) || w[nwords - 1][0] == '+' || w[nwords - 1][0] == '-') {
          return bad("playhead ... countin needs the steps of the count-in left, 1..32");
        }
        nwords -= 2;
      }
      if (nwords >= 3 && lower(w[nwords - 2]) == "wait") {  // OS-METROALIGN: `playhead <step> [<ustep>] wait <step|bar|pattern>`: a take armed on the running transport waits for that alignment
        const std::string what = lower(w[nwords - 1]);
        if (left != 0 || (what != "step" && what != "bar" && what != "pattern")) {
          return bad("playhead ... wait needs step, bar or pattern (and no countin)");
        }
        wait = what == "step" ? 1 : (what == "bar" ? 2 : 3);
        nwords -= 2;
      }
      if (nwords == 2 && lower(w[1]) == "stop" && left == 0) {
        cmd.name = "stop";
      } else if (nwords < 2 || nwords > 3 || !parse_int(w[1], 0, 127, step) || (nwords == 3 && !parse_int(w[2], 0, 47, ustep)) || w[1][0] == '+' || w[1][0] == '-' ||
                 (nwords == 3 && (w[2][0] == '+' || w[2][0] == '-'))) {
        return bad("playhead needs a step 0..127, an optional microstep 0..47 and an optional 'countin <steps>', or the word 'stop'");
      }
      cmd.kind = ScenarioCommand::Kind::Playhead;
      cmd.value = static_cast<std::uint32_t>(step);
      cmd.delta = static_cast<std::int32_t>(ustep);
      cmd.arg = static_cast<core::ActionArg>(left);
      cmd.wait = static_cast<std::uint8_t>(wait);
    } else if (verb == "midicc") {
      long ch = 0, cc = 0, val = 0;
      if (w.size() != 4 || !parse_int(w[1], 1, 16, ch) || !parse_int(w[2], 0, 127, cc) || !parse_int(w[3], 0, 127, val) ||
          w[1][0] == '+' || w[2][0] == '+' || w[3][0] == '+' || w[2][0] == '-' || w[3][0] == '-') {
        return bad("midicc needs a channel 1..16, a controller 0..127 and a value 0..127");
      }
      cmd.kind = ScenarioCommand::Kind::MidiCc;
      cmd.action = core::action_id("midi.cc");
      cmd.arg = static_cast<core::ActionArg>((ch << 16) | (cc << 8) | val);
      cmd.name = "midi.cc";
    } else if (verb == "midinote") {
      long ch = 0, note = 0, vel = 0;
      const bool off = w.size() == 4 && lower(w[3]) == "off";
      if (w.size() != 4 || !parse_int(w[1], 1, 16, ch) || !parse_int(w[2], 0, 127, note) || (!off && !parse_int(w[3], 1, 127, vel)) || w[1][0] == '+' || w[2][0] == '+' || w[2][0] == '-' ||
          (!off && (w[3][0] == '+' || w[3][0] == '-'))) {
        return bad("midinote needs a channel 1..16, a note 0..127 and a velocity 1..127 or the word 'off'");
      }
      cmd.kind = ScenarioCommand::Kind::MidiNote;
      cmd.mode = static_cast<std::uint8_t>(ch);
      cmd.value = static_cast<std::uint32_t>(note);
      cmd.delta = static_cast<std::int32_t>(vel);
    } else if (verb == "power") {
      long ms = 0;
      if (w.size() == 2 && (lower(w[1]) == "down" || lower(w[1]) == "up")) {
        cmd.value = lower(w[1]) == "down" ? 1u : 0u;
      } else if (w.size() == 2 && lower(w[1]) == "unsaved") {
        cmd.value = 3;
      } else if (w.size() == 3 && lower(w[1]) == "hold" && parse_int(w[2], 1, 60000, ms) && w[2][0] != '+' && w[2][0] != '-') {
        cmd.value = 2;
        cmd.delta = static_cast<std::int32_t>(ms);
      } else {
        return bad("power needs 'down', 'up', 'unsaved' or 'hold <ms>' (1..60000)");
      }
      cmd.kind = ScenarioCommand::Kind::Power;
    } else if (verb == "theme") {
      if (w.size() != 2) {
        return bad("theme needs one name: amiga, black or white");
      }
      if (!ui::find_theme(w[1].c_str(), w[1].size(), cmd.theme)) {
        return bad("unknown theme '" + w[1] + "' (amiga, black or white)");
      }
      cmd.kind = ScenarioCommand::Kind::Theme;
      cmd.name = lower(w[1]);
    } else if (verb == "snap") {
      if (w.size() != 2 || !valid_snap_name(w[1])) {
        return bad("snap needs one name of letters, digits, - and _ (up to 64)");
      }
      if (!snaps.insert(w[1]).second) {
        return bad("snap name '" + w[1] + "' is used twice");
      }
      cmd.kind = ScenarioCommand::Kind::Snap;
      cmd.name = w[1];
    } else if (verb == "disk") {
      const std::string word = w.size() == 2 ? lower(w[1]) : std::string();
      if (word != "idle" && word != "working" && word != "out" && word != "audio") {
        return bad("disk needs idle, working, out or audio (the platform's state of the CARD AS DISK row)");
      }
      cmd.kind = ScenarioCommand::Kind::Disk;
      cmd.value = word == "idle" ? 0u : (word == "working" ? 1u : (word == "out" ? 2u : 3u));
    } else if (verb == "card" && w.size() >= 3 && w.size() <= 5 && lower(w[1]) == "slow") {
      // OS-QANET D2: `card slow <bytes-per-ms> <latency-us> [<write-bytes-per-ms>]` or `card slow off` (a fixture folder named `slow` is written `./slow`).
      cmd.kind = ScenarioCommand::Kind::Card;
      if (lower(w[2]) == "off") {
        if (w.size() != 3) {
          return bad("card slow off takes nothing after it");
        }
        cmd.value = 5u;
      } else {
        long rate = 0;
        long latency = 0;
        long write_rate = platform::host::CardTiming::kDefaultWriteBytesPerMs;
        if (w.size() < 4 || !parse_int(w[2], 1, 1000000000L, rate) || !parse_int(w[3], 0, 1000000000L, latency) || (w.size() == 5 && !parse_int(w[4], 1, 1000000000L, write_rate))) {
          return bad("card slow needs <bytes-per-ms> (1..1000000000) and <latency-us> (0..1000000000), and may take <write-bytes-per-ms> (the unit's write rate by default), or the word off");
        }
        cmd.value = 4u;
        cmd.name = std::to_string(rate);
        cmd.args.push_back(std::to_string(latency));
        cmd.args.push_back(std::to_string(write_rate));
      }
    } else if (verb == "card" && (w.size() == 2 || w.size() == 3) && lower(w[1]) == "full") {
      // OS-QANET B8: `card full` / `card full off` (a fixture folder named `full` is written `./full`): the card refuses every write that would grow it, as a full FAT card does.
      cmd.kind = ScenarioCommand::Kind::Card;
      if (w.size() == 3 && lower(w[2]) != "off") {
        return bad("card full takes nothing, or the word off");
      }
      cmd.value = w.size() == 3 ? 7u : 6u;
    } else if (verb == "card") {
      const bool copy = w.size() == 4 && lower(w[1]) == "copy";
      if (w.size() != 2 && !copy) {
        return bad("card needs a fixture folder, or the word pull or insert (or `copy <from> <to>`, or `slow <bytes-per-ms> <latency-us>`)");
      }
      const std::string word = lower(w[1]);
      cmd.kind = ScenarioCommand::Kind::Card;
      cmd.value = copy ? 3u : (word == "pull" ? 1u : (word == "insert" ? 2u : 0u));
      cmd.name = copy ? w[2] : (cmd.value == 0u ? w[1] : word);
      if (copy) {
        cmd.args.push_back(w[3]);  // the folder it is copied to
      }
      if (cmd.value == 0u && audio_seen) {
        return bad("card: refused after `audio on` (the engine's voices are bound to the card's in place): put `card` before it");
      }
      if (cmd.value == 0u) {
        document_new = false;  // the card's own project loader does the NEW from here (the sink of `document new` is dropped)
      }
    } else if (verb == "firmware") {
      if (w.size() != 2 || lower(w[1]) != "on") {
        return bad("firmware needs the word 'on'");
      }
      if (firmware_seen) {
        return bad("firmware on: the update port is on already");
      }
      firmware_seen = true;
      cmd.kind = ScenarioCommand::Kind::Firmware;
    } else if (verb == "apps") {
      if (w.size() != 2 || lower(w[1]) != "on") {
        return bad("apps needs the word 'on'");
      }
      if (apps_seen) {
        return bad("apps on: the apps are on already");
      }
      if (audio_seen) {
        return bad("apps on: refused after `audio on` (the apps' audio tap is not the engine's)");
      }
      apps_seen = true;
      cmd.kind = ScenarioCommand::Kind::Apps;
    } else if (verb == "audio") {
      if (w.size() != 2 || lower(w[1]) != "on") {
        return bad("audio needs the word 'on'");
      }
      if (apps_seen) {
        return bad("audio on: refused after `apps on` (the apps' audio tap is not the engine's)");
      }
      if (!document_seen) {
        return bad("audio on: needs a `document` line before it (the sequencer plays it)");
      }
      if (audio_seen) {
        return bad("audio on: audio is on already");
      }
      if (document_new) {
        return bad("audio on: refused after `document new` (its File > NEW rewrites the project under the engine, with no Stop and no wait for the voices): leave out `new`, or open a `card` (its NEW does both)");
      }
      audio_seen = true;
      cmd.kind = ScenarioCommand::Kind::Audio;
    } else if (verb == "render") {
      long ms = 0;
      if ((w.size() != 3 && w.size() != 4) || !valid_render_name(w[1]) || !parse_int(w[2], 1, 10000, ms) || w[2][0] == '+' || w[2][0] == '-' || (w.size() == 4 && lower(w[3]) != "stems")) {
        return bad("render needs a name of letters, digits and _ (up to 64), a time 1..10000 ms and optionally the word 'stems'");
      }
      if (!audio_seen) {
        return bad("render: needs `audio on` before it");
      }
      if (!renders.insert(w[1]).second) {
        return bad("render name '" + w[1] + "' is used twice");
      }
      cmd.kind = ScenarioCommand::Kind::Render;
      cmd.name = w[1];
      cmd.value = static_cast<std::uint32_t>(ms) | (w.size() == 4 ? 0x80000000u : 0u);
      if (w.size() == 4) {
        for (unsigned t = 1; t <= 8; ++t) {
          renders.insert(w[1] + ".t" + std::to_string(t));
        }
        renders.insert(w[1] + ".m");
      }
    } else if (verb == "wav") {
      if (w.size() != 3 || !valid_render_name(w[1]) || w[2].empty()) {
        return bad("wav needs a name of letters, digits and _ (up to 64) and the path of a WAV file of the card");
      }
      if (!audio_seen) {
        return bad("wav: needs `audio on` before it");
      }
      if (!renders.insert(w[1]).second) {
        return bad("render name '" + w[1] + "' is used twice");
      }
      cmd.kind = ScenarioCommand::Kind::Wav;
      cmd.name = w[1];
      cmd.args.push_back(w[2]);
    } else if (verb == "fingerprint") {
      if (w.size() != 2 && !(w.size() == 3 && lower(w[2]) == "nof0")) {
        return bad("fingerprint needs the name of one render (or of a stems part: <name>.t1 .. <name>.m), and optionally the word 'nof0' (no f0 track)");
      }
      if (renders.count(w[1]) == 0u) {
        return bad("fingerprint: no 'render " + w[1] + "' before this line");
      }
      if (!fingerprinted.insert(w[1]).second) {
        return bad("fingerprint: '" + w[1] + "' is fingerprinted twice");
      }
      cmd.kind = ScenarioCommand::Kind::Fingerprint;
      cmd.name = w[1];
      cmd.value = w.size() == 3 ? 1u : 0u;  // nof0
    } else if (verb == "mark") {
      if (w.size() != 3 || lower(w[1]) != "model" || !valid_snap_name(w[2])) {
        return bad("mark needs 'model' and a label of letters, digits, - and _ (up to 64)");
      }
      if (!marks.insert(w[2]).second) {
        return bad("mark label '" + w[2] + "' is used twice");
      }
      cmd.kind = ScenarioCommand::Kind::Mark;
      cmd.name = w[2];
    } else if (verb == "assert") {
      if (w.size() < 3) {
        return bad("assert needs state, model, file or audio, and what to check");
      }
      const std::string what = lower(w[1]);
      std::string why;
      cmd.kind = ScenarioCommand::Kind::Assert;
      cmd.text = trimmed(line);
      cmd.args.assign(w.begin() + 2, w.end());
      if (what == "state") {
        std::vector<StatePair> pairs;
        if (!parse_state_pairs(cmd.args, pairs, why)) {
          return bad(why);
        }
        cmd.target = ScenarioCommand::Target::State;
      } else if (what == "model") {
        std::string path, value;
        if (!split_model_args(cmd.args, path, value)) {
          return bad("assert model needs one <path>=<value>");
        }
        if (path == "digest") {
          if (value[0] == '@' ? marks.count(value.substr(1)) == 0 : !is_sha256_hex(value)) {
            return bad(value[0] == '@' ? "digest=@" + value.substr(1) + ": no 'mark model " + value.substr(1) + "' before this line" : "digest= wants @<label> or 64 hex digits");
          }
        } else if (!valid_model_path(path, why)) {
          return bad(why);
        }
        cmd.target = ScenarioCommand::Target::Model;
      } else if (what == "file") {
        std::string path;
        FileWant want;
        if (!parse_file_want(cmd.args, path, want, why)) {
          return bad(why);
        }
        cmd.target = ScenarioCommand::Target::File;
      } else if (what == "audio") {
        AudioAssert a;
        if (!parse_audio_assert(cmd.args, a, why)) {
          return bad(why);
        }
        const auto known = [&](const std::string& name) { return name.empty() || renders.count(name) != 0u; };
        if (!known(a.name)) {
          return bad("assert audio: no 'render " + a.name + "' before this line");
        }
        for (const AudioCheck& c : a.checks) {
          if (!known(c.ref)) {
            return bad("assert audio: " + c.text + ": no 'render " + c.ref + "' before this line");
          }
          for (const std::string& other : c.others) {
            if (!known(other)) {
              return bad("assert audio: " + c.text + ": no 'render " + other + "' before this line");
            }
          }
        }
        cmd.target = ScenarioCommand::Target::Audio;
      } else {
        return bad("assert needs state, model, file or audio");
      }
    } else {
      return bad("unknown command '" + w[0] + "'");
    }
    out.push_back(cmd);
  }
  return true;
}

bool run_scenario(const std::vector<ScenarioCommand>& commands, Simulator& sim, const SnapshotFn& on_snapshot,
                  std::string& error, std::vector<std::string>* log) {
  using Kind = ScenarioCommand::Kind;
  sim.step();
  if (!sim.healthy()) {
    error = "initial frame: " + sim.failure();
    return false;
  }
  std::map<std::string, std::string> marks;  // `mark model` label -> the document's digest then
  for (const ScenarioCommand& cmd : commands) {
    switch (cmd.kind) {
      case Kind::Wait:
        sim.wait_ms(cmd.value);
        break;
      case Kind::Press:
        sim.press(cmd.button);
        break;
      case Kind::Release:
        sim.release(cmd.button);
        break;
      case Kind::Tap:
        for (std::uint32_t i = 0; i < cmd.value; ++i) {
          sim.tap(cmd.button);
        }
        break;
      case Kind::Pad:
        switch (cmd.mode) {
          case 1: sim.pad_hold(cmd.value, static_cast<unsigned>(cmd.delta)); break;
          case 2: sim.pad_down(cmd.value); break;
          case 3: sim.pad_up(cmd.value); break;
          default: sim.pad(cmd.value); break;
        }
        break;
      case Kind::Encoder:
        for (std::uint32_t i = 0; i < (cmd.value == 0u ? 1u : cmd.value); ++i) {
          sim.encoder(cmd.delta);
          if (cmd.mode > 1u && i + 1u < cmd.value) {
            sim.wait_ms(static_cast<std::uint32_t>(cmd.mode) - 1u);  // the spacing of a sweep: the clock runs on between two turns
          }
        }
        break;
      case Kind::Action:
        // Unknown cannot happen: the parser found the name in the registry.
        if (sim.action(cmd.action, cmd.arg) == app::DispatchResult::Disabled) {
          error = "line " + std::to_string(cmd.line) + ": action '" + cmd.name + "' is not enabled here";
          return false;
        }
        break;
      case Kind::Document:
        if (sim.has_card()) {  // the card's loader and bank are bound to the document that was open when it came in
          error = "line " + std::to_string(cmd.line) + ": document: a card is in the slot already, so the document is the card's (put `document` before `card`)";
          return false;
        }
        sim.open_document((cmd.value & 1u) != 0u, (cmd.value & 2u) != 0u, (cmd.value >> 8) & 0xFFu, (cmd.value & 4u) != 0u, (cmd.value & 8u) != 0u, (cmd.value & 16u) != 0u);
        break;
      case Kind::Disk: {
        using Phase = app::ViewState::DiskView::Phase;
        sim.app().set_disk_state(cmd.value == 2u ? Phase::Out : (cmd.value == 1u ? Phase::Working : Phase::Idle), false, cmd.value == 3u, !sim.app().disk_read_only());  // (the disk the computer has follows the row's choice at the time: the simulator has no share to fix it)
        sim.app().set_card_on_computer(cmd.value == 2u);
        if (cmd.value == 2u) {
          sim.app().disk_notice(app::App::kCardOnComputerStrip, 60000);  // (the platform rewrites it every second)
        }
        break;
      }
      case Kind::Card: {
        std::string why;
        if (cmd.value >= 6u) {  // `card full` / `card full off`: the card's room, which needs no card yet
          sim.set_card_full(cmd.value == 6u);
          break;
        }
        if (cmd.value >= 4u) {  // `card slow ...`: the model of the card's time, which needs no card yet
          const bool on = cmd.value == 4u;  // (5: off)
          const auto num = [](const std::string& text) { return static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 10)); };
          if (!sim.set_card_timing(on, on ? num(cmd.name) : 0u, on ? num(cmd.args[0]) : 0u, on ? num(cmd.args[1]) : 0u, why)) {
            error = "line " + std::to_string(cmd.line) + ": card slow: " + why;
            return false;
          }
          break;
        }
        const bool ok = cmd.value == 0u ? sim.open_card(cmd.name, why)
                        : cmd.value == 1u ? sim.card_pull(why)
                        : cmd.value == 2u ? sim.card_insert(why)
                                          : sim.card_copy(cmd.name, cmd.args[0], why);
        if (!ok) {
          error = "line " + std::to_string(cmd.line) + ": card " + (cmd.value == 0u ? "'" + cmd.name + "'" : cmd.name) + ": " + why;
          return false;
        }
        break;
      }
      case Kind::Apps: {
        std::string why;
        if (!sim.apps_on(why)) {
          error = "line " + std::to_string(cmd.line) + ": apps on: " + why;
          return false;
        }
        break;
      }
      case Kind::Firmware: {
        std::string why;
        if (!sim.firmware_on(why)) {
          error = "line " + std::to_string(cmd.line) + ": firmware on: " + why;
          return false;
        }
        break;
      }
      case Kind::Audio: {
        std::string why;
        if (!sim.audio_on(why)) {
          error = "line " + std::to_string(cmd.line) + ": audio on: " + why;
          return false;
        }
        break;
      }
      case Kind::Render: {
        std::string why;
        if (!sim.render_begin(cmd.name, cmd.value & 0x7FFFFFFFu, (cmd.value & 0x80000000u) != 0u, why)) {
          error = "line " + std::to_string(cmd.line) + ": render '" + cmd.name + "': " + why;
          return false;
        }
        break;
      }
      case Kind::Wav: {
        std::string why;
        if (!sim.load_wav(cmd.name, cmd.args[0], why)) {
          error = "line " + std::to_string(cmd.line) + ": wav '" + cmd.name + "': " + why;
          return false;
        }
        break;
      }
      case Kind::Fingerprint:
        if (sim.render_result(cmd.name) == nullptr) {
          error = "line " + std::to_string(cmd.line) + ": fingerprint '" + cmd.name + "': the render has not ended (a `wait` for its window first)";
          return false;
        }
        break;
      case Kind::MidiCc:
        (void)sim.action(cmd.action, cmd.arg);  // not Unknown; Disabled cannot happen (no focus needed, no flag but undoable-if-open)
        break;
      case Kind::MidiNote:
        sim.midi_note(cmd.mode, cmd.value, static_cast<unsigned>(cmd.delta), cmd.delta != 0);
        break;
      case Kind::Playhead:
        sim.playhead(cmd.name != "stop", cmd.value, static_cast<unsigned>(cmd.delta), static_cast<unsigned>(cmd.arg), cmd.wait);
        break;
      case Kind::Theme:
        sim.set_theme(cmd.theme);
        break;
      case Kind::Power:
        if (cmd.value == 3u) {
          sim.mark_unsaved();
        } else if (cmd.value == 2u) {
          sim.power_switch(true);
          sim.wait_ms(static_cast<std::uint32_t>(cmd.delta));
          sim.power_switch(false);
        } else {
          sim.power_switch(cmd.value == 1u);
        }
        break;
      case Kind::Mark: {
        const std::string digest = sim.project() != nullptr ? model_digest(*sim.project()) : std::string();
        if (digest.empty()) {
          error = "line " + std::to_string(cmd.line) + ": mark model '" + cmd.name + "': " + (sim.project() == nullptr ? "there is no document (a `document` line opens one)" : "the native writer failed on the document");
          return false;
        }
        marks[cmd.name] = digest;
        break;
      }
      case Kind::Assert: {
        std::string summary;
        const std::string why = assertion_problem(cmd, sim, marks, &summary);
        if (!why.empty()) {
          error = "line " + std::to_string(cmd.line) + ": assertion failed: " + cmd.text + ": " + why;
          return false;
        }
        if (log != nullptr && cmd.target == ScenarioCommand::Target::Audio) {
          log->push_back("line " + std::to_string(cmd.line) + ": " + cmd.args[0] + ": " + summary);
        }
        break;
      }
      case Kind::Snap:
        if (sim.healthy() && !on_snapshot(cmd.name, sim)) {
          error = "line " + std::to_string(cmd.line) + ": snapshot '" + cmd.name + "' was refused";
          return false;
        }
        break;
    }
    if (!sim.healthy()) {
      error = "line " + std::to_string(cmd.line) + ": " + sim.failure();
      return false;
    }
  }
  std::uint32_t left = 0;
  const std::string running = sim.render_running(&left);
  if (!running.empty()) {
    error = "the script ended while render '" + running + "' was still running (" + std::to_string(left) + " ms more): wait for it";
    return false;
  }
  return true;
}

std::string framebuffer_sha256(const EveRaster& raster) {
  return platform::host::sha256_hex(raster.rgba(), EveRaster::kFramebufferBytes);
}

}  // namespace o3os::sim
