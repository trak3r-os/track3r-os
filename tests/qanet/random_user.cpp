// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET B1 (docs/specs/OS-QANET.md part B): a seeded random user over every screen, on a large generated card, with invariants checked after every step.
//
//   o3_qanet_random_user --card <dir> [--seed <first>] [--seeds <n>] [--steps <n>] [--shard i/n] [--env] [--baseline <file>] [--min-conclusive <n>] [--min-keys <n>] [--min-sel <n>] [--found-dir <dir>] [--verbose]
//   o3_qanet_random_user --selftest --card <dir>
//   o3_qanet_random_user --shrink-selftest --card <dir> --found-dir <dir> --o3sim <file>
//   o3_qanet_random_user --assert-selftest --card <dir> --found-dir <dir> --o3sim <file>
//
// One run per seed: a Simulator with the demo document and a scratch copy of the card (tools/qanet_card.py: about 2000 files, 50 folders), then `steps` steps drawn from a PRNG of that seed
// (no wall clock, no address, no other source: the same seed is the same run, and the final digest says so). A step is one of: a screen button (File and Sample load 24 and 16 times as likely as each of the rest),
// a function key (alone, or with Shift held), a cursor or edit key, an encoder turn, a pad, a wait, a card pull or insert, or an undo burst (below). Nothing in the generator knows a bug: the weights
// are by kind of input, and no sequence is built by hand.
//
// The invariants, after every step (a finding is recorded once per invariant and seed):
//   display_list   the simulator's healthy-frame check (a frame that did not fit the display list, a raster refusal); a crash or an assertion ends the process (ASan, UBSan, abort)
//   listing_empty  a browser screen (File's PROJECTS or IMPORT page, Sample load) whose scan is Done and lists nothing, while a scan of the same folder and filter, made here on the same card by a
//                  DirScan of our own, lists something: the card holds files and the screen says it does not
//   no_sd_card     a browser screen that says NO SD CARD (its scan is Idle or NoCard for more than kNoCardTicks) while the card is in the slot and mounted
//   job_stuck      the app's card jobs run past their bound (kBounds: a listing 3000 ticks, a WAV export 60000, any other job 20000), or a scan that is Scanning while no job runs (kStuckScanTicks)
//   dir_open       a directory handle still open at rest (no job running, no scan Scanning, the card in): found by opening kMaxOpenDirs of them here and counting the refusals
//   undo           an undo burst (a few steps that edit the document, then as many `edit.undo` as the journal grew): the document's digest differs from the one before the burst
//   audio_probe    C4 (`--audio-probe`, `--audio-session`, tests/qanet/audio_probe.h): after a step that changes an instrument's sample, play mode, slices or loop (a rebind of the voices' descriptor is to come) the walk is replayed on a fork with
//                  the real engine (the walk itself is not touched; both have the engine from the first tick) and the instrument is played from a pad straight after the edit, from a pad after it has settled, and from a Pattern step; pitch (5 cents),
//                  onset (2 ms) and the shape of the sound are compared. With `--audio-session`, at the middle of every walk the fork also runs an edit session (a WAV into an instrument, a loop, EVEN, ADD, AUTO, slice delete, play modes, undo, load over
//                  the chop), probed after each edit. A finding is keyed by (`audio_probe`, the edit, `<pad:fresh|pad:settled>:<what>`): the edit is the kind of change (load, load_over_sliced, mode, slices, loop, undo, even, add, auto, del).
// The allow-list, each entry with its reason (a rule that fails on a correct design wastes time): 1. an undo burst that moved the project's session state (JUMP, note entry, mute modes, Perform,
// mixer, MIDI, USB: persisted in the native file by OS-PERSIST2, never an undo unit) is not compared; 2. a burst that did not stay on the Pattern screen, or whose journal got shorter inside it, is not
// compared (`edit.undo` acts on the area of the screen in force and the depth read is the Pattern area's); 3. a listing after a message that says the app wrote to the card is a stale cache (dir_scan.h)
// until the next scan starts, so listing_empty waits for it. The three are counted in the coverage line or named here, never silent.
// Deterministic across machines: the host card lists a folder in byte order (DirStorage sorts the names at opendir), not in the host file system's, so the scan's passes (a probe costs 8 units, an
// entry 1) and the digest are the same for a seed anywhere. The log prints the inputs of a finding.
// The state the simulator does not expose is not asked of it: the job kind is told by the scan state, open directories are counted by the handles the card still has free, and the
// listing's right answer is a scan of our own. (What is missing for a finer check: a job table with a kind per slot, and open_dirs() on DirCard; both would be a change of product code or of
// the host card, and are left as a follow-up.)
//
// Findings are compared with a baseline file (tests/qanet/baseline_random.txt): `invariant | screen | action | seed@step | reason | known ID or new`. The key is the first three fields; the seed and the
// step are the repro. The run fails on a finding whose key is not in it and on a baseline line of a seed this run covered (with the same step count) whose key did not occur. Shrinking (B2, `--found-dir <dir>`): each finding is replayed from its inputs with
// the invariants checked after every command, reduced by deleting commands (halves, then one at a time, while the same invariant still fails; at most 600 replays), and written as
// `<dir>/found_<seed>_<invariant>.txt` in the scenario grammar (`document`, `card <folder>`, the commands, and the assertion that fails while the bug is there). The file and the one-line `o3sim` command are
// printed; a finding that is in the baseline is not shrunk. The invariants are looked at in a replay where the walk looked (the end of a step), never between two inputs of one step. Assertions of a written scenario:
//   listing_empty, no_sd_card   `wait 3000` (the scan is done), then `assert state sel=~^[^-]`: the listing has an entry. no_sd_card only when a scan of the folder lists something
//   undo                        written whole: `mark model b<n>` where the burst took its digest, `assert model digest=@b<n>` after its undos
//   planted_state               the planted `assert state`
//   job_stuck, dir_open         INPUT ONLY: no state field shows a job that never ends or an open directory handle, so the file has no assertion and o3sim runs it to the end (the walk command in the header finds it)
//   display_list                no assertion: o3sim stops by itself at the unhealthy frame
// The found files are never committed (the build's qanet_found/).
//
// The rules of part A at every step (B4, `--rules-every <n>`, 0 = off): each input of the walk that is one action (a key, a function key with Shift, an encoder turn, a pad; not a wait, a card pull, a burst's undos)
// is a unit. The walk takes qanet::snap_of() before it and after it (the quiet waits between them are recorded inputs: the action's job finishes, 60 ms of nothing moving) and runs the rules: kStepRules is the list, rule 1
// (check_rules, rules.h) is in it, and a later rule is an adapter and one more entry (see the comment at kStepRules). The walk presses keys and the simulator does not say which action a key ran, so the four actions
// that may lower the undo depth are told by effect (classify). A finding is a walk finding: invariant `R1` (`R0`: the native writer failed), the screen after, the input, keyed like the others, shrunk, and written as a
// scenario; a silent change of the project ends in `action edit.undo` and `assert model digest=@<mark before the unit>` (the grammar has no undo depth to assert and does not see samples, so a change of the samples alone has
// no assertion). Limits, the library's and the walk's: the digest is the native file, so the project's session state (JUMP, note entry, mute modes, mixer, Perform ...) is in it and no input that moved it is judged
// (allow-list entry 1; the coverage line counts them; A1's own walk reports the Mixer lines); the walk's units are inputs, so an action that needs two inputs is judged on each; a project replaced by a buggy action
// passes as File NEW.
//
// Rules 2 and 3 at every step (B4b): the same units. The walk reads the action of an input from the binding chain as it was BEFORE the input (rig.h live_chain, core::resolve over the built-in bindings: the walk
// presses keys and the simulator does not say what ran), and builds what the rules of part A ask for.
//   RULE 2 (check_key): for a bound F key or arrow (with or without Shift). The reference is the state before the key, and the control is a WAIT before it: the walk pauses 100 ms before every key (the length of the key's
//   unit, a recorded input like any other, for every key whichever units are judged) and looks whether anything moved by itself; if so the key is not judged (`not judged` in the coverage line), if not the state before
//   is what a control run that waits as long would give. While the transport plays the playhead's fields are left out (`drifting`). A tap of a key with a double-tap binding does its work 300 ms after the tap: the
//   quiet waits for it. Limits: the key row is not read (no label findings: KeyFacts::row_known is false). A2 excuses a silent key by pressing its siblings in the same state, which the walk cannot do; it excuses by what
//   the chain tells: a plain arrow (it may stand at the edge of what it moves), a Shift + arrow that the resolver gives the plain arrow's binding, an F key whose row has another key of the same action with another argument
//   (it selects what is already selected), the placeholder soft key. All of them are counted (`excused`), none is judged; Shift + an arrow with a binding of its own (the selection) is.
//   RULE 3 (check_selection): the images of the document (selection_ops.h image_of) are copied before an input made on a screen the table judges, and after it when the input's action is in the table and applies. The
//   pattern clipboard (the App's is private) is what the last block.copy of the walk took; the instrument clipboard is the slot of the last instrument.copy and is forgotten when the document changes. Limit: an input made
//   in a pattern of the bank that does not exist yet (its first write makes it) is not judged: the table reads that as a pattern made outside the selection (`more not judged` in the coverage line).
// Self-tests: --rules23-selftest (a silent key after the document changed mid-walk, an out-of-span edit after a selection the walk made mid-walk). The walk also presses Shift + an arrow now (the selection keys).
// B5 (--meta [--meta-count n] [--min-meta n], self-test --meta-selftest): metamorphic relations on their own random stream, n of them (10) after the walk's last step (so the walk, and with it every baseline line, is the
// run it was without --meta): save then OPEN of what was saved, transpose +n then -n, octave
// up then down, copy + paste + undo, a slice added on Wave + undo. The digest of the project after is the digest before (a `mark model` and `assert model digest=@<label>` in the written script); a state that cannot
// host one is counted as skipped. Findings are `meta_<relation>`, not shrunk. [--min-save-open n]: the run fails when fewer saves then OPENs than n ended in a compare. The digest is sim::model_digest: the native
// writer's bytes and then the model's fields one by one, so a field the writer drops (mutant M19) reads back as another value.
// B8 (--exhaust [--min-exhaust n], self-test --exhaust-selftest): fixtures that drive a LIMIT, after the walk and after --meta, on their own random stream (the walk and the baseline do not move). A fixture runs on
// the seeds whose number mod 16 is 3, 5, 7 (the undo cap) or 4, 6 (the sample-edit history cap), or both on every seed with --exhaust-all. Where it runs today: seeds 1 and 2 of qanet_random_exhaust (--exhaust-all) and the
// self-test; the eight shards (qanet_random_user_Nof8) do not pass --exhaust, so the seed mapping above is for a long run by hand. The undo cap: 12 tracks selected (select.extend 10 until the block ends in track 12: a column at a time in the first track, then by tracks) and step.insert repeated until the Pattern ring drops units
// (UndoJournal::dropped_units) and holds only the fixture's own; rule 1 holds at every insert, and the undo reaches exactly what was kept (depth() units, back to the digest the oldest kept unit was made from), a
// further undo changes nothing, redo gives the document back. The sample-edit cap: sedit.run effects on the Sample edit screen past SampleBank::kHistory; rule 1 judges every edit (a change outside the history's
// own slot is not credited by the full history: rules.cpp unit_recorded), the history holds kHistory steps, and undo / redo walk the kept versions of the audio (the oldest dropped). Findings are `exhaust_<fixture>`.
// The voice pool (seeds 8, 9 mod 16) and the folder of 2000 files (seeds 10, 11) run on a simulator of their own, after the others: `document sound` with the engine (the test bank's looped sine) for 13 to 16 pads held at once,
// released in a random order, and a card made at test time (ScratchCard: 2000 files in one folder) for Sample load. See exh_voice_pool and exh_big_folder for what they judge. They are not in --exhaust-all (the walk's three);
// `--exhaust --exhaust-only voice_pool|big_folder` runs one (any seed, `--steps 1` is enough: the walk is not theirs), self-test --exhaust-fresh-selftest.
#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/dir_index.h>
#include <o3os/app/dir_scan.h>
#include <o3os/app/model.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/state_line.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/voices.h>
#include <o3os/core/binding.h>
#include <o3os/hal/storage.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/scenario_assert.h>
#include <o3os/sim/sim_audio.h>
#include <o3os/sim/simulator.h>

#include "audio_probe.h"
#include "rig.h"
#include "rules.h"
#include "selection_ops.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace o3os;

namespace {

constexpr unsigned kNoCardTicks = 100;      // a scan that is Idle or NoCard this long on a browser screen with the card in: the screen says NO SD CARD (poll_files starts a scan in the pass it sees Idle)
constexpr unsigned kStuckScanTicks = 100;   // Scanning with no job running this long
constexpr unsigned kScanBoundTicks = 3000;  // a folder of 600 files costs about one tick a file (a probe is 8 of the 8 units of a pass): 3000 is four times that
constexpr unsigned kJobBoundTicks = 20000;  // any other card job (an audition, a peak build): 20 s
constexpr unsigned kExportBoundTicks = 60000;  // a WAV export (File F1 on the EXPORT page): it keeps a note that never dies away until the tail cap (kMaxTailBlocks, 30 s of tail) and writes to the slow card model, so it runs about 20 s and ends by itself: the cap plus the write time (OS-QAFIX-31)
// The end of a written listing_empty or no_sd_card scenario, when the folder holds files: the scan is done by then, and a listing that has an entry puts the cursor on one (`sel=` is `-` on none).
const char* const kListsSomething = "wait 3000\nassert state sel=~^[^-]";
constexpr unsigned kBurstMin = 4;
constexpr unsigned kBurstMax = 12;

// ---- the generator ---------------------------------------------------------------------------------------------------------------------------------------

class Rng {
 public:
  // The seed goes through the mixer before it is the state: splitmix64's state moves by a fixed step a call, so a state of seed * step would make the stream of seed n the stream of seed n - 1 one call later.
  explicit Rng(std::uint64_t seed) : s_(mix(seed + 0x5851F42D4C957F2Dull)) {
    for (int i = 0; i < 4; ++i) {
      next();
    }
  }
  static std::uint64_t mix(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t next() { return mix(s_ += 0x9E3779B97F4A7C15ull); }  // splitmix64
  unsigned below(unsigned n) { return static_cast<unsigned>(next() % n); }

 private:
  std::uint64_t s_;
};

struct Weighted {
  app::Button button;
  unsigned weight;
};

// The screen buttons: File and Sample load 24 and 16 times as likely as each of the others (the spec's weights favour them, and the screens that list the card are the ones with the state to lose).
const Weighted kScreenButtons[] = {
    {app::Button::Params, 1}, {app::Button::Performance, 1}, {app::Button::File, 24},      {app::Button::Config, 1}, {app::Button::Master, 1},
    {app::Button::SampleLoad, 16}, {app::Button::SamplePlay, 1}, {app::Button::SampleEdit, 1}, {app::Button::SampleRec, 1}, {app::Button::Song, 1},
    {app::Button::Pattern, 1}, {app::Button::Note, 1},       {app::Button::Instr, 1},     {app::Button::Vol, 1},     {app::Button::Fx, 1},
};
const app::Button kEditButtons[] = {app::Button::Up, app::Button::Down, app::Button::Left, app::Button::Right, app::Button::Play, app::Button::Copy, app::Button::Insert, app::Button::Delete, app::Button::Rec};

enum class Kind : std::uint8_t { Screen, Fkey, ShiftFkey, Edit, Encoder, Pad, Wait, Card, Burst };
struct KindWeight {
  Kind kind;
  unsigned weight;
};
const KindWeight kKinds[] = {{Kind::Screen, 30}, {Kind::Fkey, 20}, {Kind::ShiftFkey, 6}, {Kind::Edit, 22}, {Kind::Encoder, 6}, {Kind::Pad, 4}, {Kind::Wait, 6}, {Kind::Card, 2}, {Kind::Burst, 3}};

// After a screen button a person uses that screen: the next one to five steps are mostly its function keys and cursor keys (a screen is not left for the next one at once). Same kinds, other weights.
const KindWeight kSessionKinds[] = {{Kind::Fkey, 24}, {Kind::ShiftFkey, 2}, {Kind::Edit, 8}, {Kind::Encoder, 3}, {Kind::Wait, 3}, {Kind::Pad, 1}};

// What a burst does to the document: keys that move the cursor and edit steps, the encoder, the pads.
const KindWeight kBurstKinds[] = {{Kind::Edit, 10}, {Kind::Encoder, 6}, {Kind::Pad, 10}, {Kind::Fkey, 2}, {Kind::ShiftFkey, 1}};

app::Button pick_screen_button(Rng& rng) {
  unsigned total = 0;
  for (const Weighted& w : kScreenButtons) {
    total += w.weight;
  }
  unsigned n = rng.below(total);
  for (const Weighted& w : kScreenButtons) {
    if (n < w.weight) {
      return w.button;
    }
    n -= w.weight;
  }
  return app::Button::Pattern;
}

Kind pick_kind(Rng& rng, const KindWeight* table = kKinds, std::size_t count = sizeof kKinds / sizeof kKinds[0]) {
  unsigned total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    total += table[i].weight;
  }
  unsigned n = rng.below(total);
  for (std::size_t i = 0; i < count; ++i) {
    const KindWeight& k = table[i];
    if (n < k.weight) {
      return k.kind;
    }
    n -= k.weight;
  }
  return Kind::Wait;
}

// ---- findings and the baseline ---------------------------------------------------------------------------------------------------------------------------

// One input of a run as a scenario line, and whether the walk looked at the invariants after it: the end of a step is a rest point (the app has had the step's whole input), the lines inside a step are not.
struct Input {
  std::string line;
  bool rest = false;
  bool begin = false;  // B4: the first line of a unit the rules judge (one input of the walk; a snapshot is taken before it)
  bool end = false;    // B4: the last line of that unit, the quiet waits after the input included (the rules look at the state after it)
};

struct Finding {
  std::string invariant;
  std::uint64_t seed = 0;
  unsigned step = 0;
  std::string screen;  // the screen the step left the app on
  std::string action;  // the input of the step that was the last before the check: `tap file`, `shift+f3`, `pad`, `encoder`, `wait`, `card pull`, `burst` ...
  std::string why;
  std::string repro;  // the inputs up to the finding, one scenario line each, joined by `; ` (after `document` and `card qanet_card`)
  std::vector<Input> lines;  // the same inputs, one scenario line each (B2 shrinks them, and replays them with the invariants looked at where the walk looked)
  std::string match;  // what a replay looks for: the key the run dedups the finding by (the invariant, for a state invariant; the rule, screen, input and reason for a rule)
  std::string mark;   // B4: a `mark model <label>` goes before the line that begins the unit that failed (the tail's assertion compares with it)
  std::string tail;          // the scenario lines that go after the inputs, when the grammar can say the invariant (`wait`, `assert ...`; '\n' between them); empty: the finding is carried by its inputs
};

struct BaselineLine {
  std::string invariant;
  std::string screen;
  std::string action;
  std::uint64_t seed = 0;  // the repro, not the key
  unsigned step = 0;
  std::string reason;
  std::string id;
  bool seen = false;
};

struct Baseline {
  unsigned steps = 0;  // `# steps=N` of the file: the run it was made with (0: none given)
  std::vector<BaselineLine> lines;
};

std::string trim(const std::string& s) {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) {
    ++a;
  }
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) {
    --b;
  }
  return s.substr(a, b - a);
}

bool parse_baseline(const std::string& text, Baseline& out, std::string& error) {
  std::istringstream in(text);
  std::string line;
  unsigned lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (line[0] == '#') {
      const std::size_t at = line.find("steps=");
      if (at != std::string::npos) {
        out.steps = static_cast<unsigned>(std::strtoul(line.c_str() + at + 6, nullptr, 10));
      }
      continue;
    }
    std::vector<std::string> f;
    std::size_t from = 0;
    while (true) {
      const std::size_t bar = line.find('|', from);
      f.push_back(trim(line.substr(from, bar == std::string::npos ? std::string::npos : bar - from)));
      if (bar == std::string::npos) {
        break;
      }
      from = bar + 1;
    }
    if (f.size() != 6) {
      error = "baseline line " + std::to_string(lineno) + ": want `invariant | screen | action | seed@step | reason | known ID or new`";
      return false;
    }
    BaselineLine b;
    b.invariant = f[0];
    b.screen = f[1];
    b.action = f[2];
    char* end = nullptr;
    b.seed = std::strtoull(f[3].c_str(), &end, 10);
    if (end == f[3].c_str() || *end != '@') {
      error = "baseline line " + std::to_string(lineno) + ": the fourth field is `<seed>@<step>`";
      return false;
    }
    b.step = static_cast<unsigned>(std::strtoul(end + 1, nullptr, 10));
    b.reason = f[4];
    b.id = f[5];
    out.lines.push_back(b);
  }
  return true;
}

struct Verdict {
  std::vector<std::string> fresh;  // findings not in the baseline
  std::vector<std::string> stale;  // baseline lines of a covered seed that did not occur
};

bool in_baseline(const Baseline& base, const Finding& f) {
  for (const BaselineLine& b : base.lines) {
    if (b.invariant == f.invariant && b.screen == f.screen && b.action == f.action) {
      return true;
    }
  }
  return false;
}

// A finding is known by its key: the invariant, the screen and the action where it fired. The seed and the step are the repro text after the key, so a change of the walk's weights (slice B2 biases the
// walk towards the screen-to-screen tuples not yet seen) moves them without touching the baseline. A baseline line is stale when this run covered its repro seed (same step count) and no finding of
// the run has its key. `steps`: the step count of this run; a baseline made with another one still names known findings, but its absences prove nothing. `rules`: the run judges the rules of part A
// (--rules-every): a line of a rule (`R0`, `R1` ...) is owned by the runs that do, and a run that does not (the FAT32 image's, D1) cannot have found it, so its absence says nothing there. A new finding fails
// every run. `audio` (--audio-probe): the run has the engine, so a line of the audio probe (`audio_probe`) or one that says `[audio walk]` in its reason (a finding that only a walk with the engine reaches) is owned by
// it; a run without the engine cannot have found it, as the rules' lines above.
Verdict compare(Baseline& base, const std::vector<Finding>& findings, std::uint64_t first, unsigned seeds, unsigned steps, bool rules, bool audio = true) {
  Verdict v;
  for (const Finding& f : findings) {
    bool known = false;
    for (BaselineLine& b : base.lines) {
      if (b.invariant == f.invariant && b.screen == f.screen && b.action == f.action) {
        b.seen = true;
        known = true;
      }
    }
    if (!known) {
      v.fresh.push_back(f.invariant + " | " + f.screen + " | " + f.action + " | " + std::to_string(f.seed) + "@" + std::to_string(f.step) + " | " + f.why);
    }
  }
  for (const BaselineLine& b : base.lines) {
    const bool rule_line = b.invariant.size() >= 2 && b.invariant[0] == 'R' && b.invariant[1] >= '0' && b.invariant[1] <= '9';
    const bool audio_line = b.invariant == "audio_probe" || b.reason.compare(0, 13, "[audio walk] ") == 0;
    if (!b.seen && b.seed >= first && b.seed < first + seeds && (base.steps == 0 || base.steps == steps) && (rules || !rule_line) && (audio || !audio_line)) {
      v.stale.push_back(b.invariant + " | " + b.screen + " | " + b.action + " | " + std::to_string(b.seed) + "@" + std::to_string(b.step) + " | " + b.reason + " | " + b.id);
    }
  }
  return v;
}

// The messages of the strip that say the app wrote to the card (the state line's `msg=`, spaces as `_`): a finished save or export job ("SAVE <name> DONE", "EXPORT <name> DONE", "OG <name> DONE", the
// last with " DONE (CLIPPED)" too), "SAVED ..." (an OG project, a set of stems, a take saved as an instrument) and "DELETED: <project>" (the DELETE marker). Exact words, not a prefix: "SAVE_TIMED_OUT",
// "COPY ...", "IMPORTING ..." and "PURGED ..." wrote nothing to the folders the browsers list.
bool is_write_message(const std::string& msg) {
  const auto starts = [&](const char* w) { return msg.compare(0, std::strlen(w), w) == 0; };
  const auto ends = [&](const char* w) { return msg.size() >= std::strlen(w) && msg.compare(msg.size() - std::strlen(w), std::string::npos, w) == 0; };
  if (starts("SAVED_") || starts("DELETED:_")) {
    return true;
  }
  return (starts("SAVE_") || starts("EXPORT_") || starts("OG_")) && (ends("_DONE") || ends("_DONE_(CLIPPED)"));
}

// ---- one run -----------------------------------------------------------------------------------------------------------------------------------------------

struct Options {
  std::string card;
  std::uint64_t first_seed = 1;
  unsigned seeds = 1;
  unsigned steps = 300;
  std::string baseline;
  bool verbose = false;
  bool trace = false;  // every input of the run, one line each (a repro by hand)
  bool selftest = false;
  std::string found_dir;   // B2: a finding is shrunk and written here as `found_<seed>_<invariant>.txt` (empty: no shrinking)
  std::string plant_hold;  // self-test: the state line must hold these `assert state` words, or the invariant planted_state fails
  std::string o3sim;       // self-test: the o3sim binary that replays the written scenario
  bool states = false;     // print the state line after every step
  bool shrink_selftest = false;
  bool assert_selftest = false;
  bool rules_selftest = false;
  bool plant_r2 = false;     // self-test: after a sample slot holds audio, the first judged key is judged as if it had changed and said nothing
  bool plant_r3 = false;     // self-test: after a pattern selection exists, the first unit is judged as a block copy that also edited a step
  bool rules23_selftest = false;
  std::string rules_case;  // --rules-case R2|R3: the rules23 self-test runs that rule only (empty: both)
  unsigned rules_every = 0;  // B4: the rules of part A judge every n-th input of the walk (1: every one, 0: none)
  bool overwrite_report = false;  // B4 fold: one line for every WAV load over a used slot that changed the slot and that no rule flagged, with why it passed (measurement only)
  bool plant_r1 = false;     // self-test: after a sample slot holds audio, one input is judged as if it had changed the document with no unit
  bool stop_at_plant = false;  // self-test (OS-OPS-CIPOLE): the walk ends after the step that carried a planted breach (what follows proves nothing more and costs minutes under the sanitizers)
  unsigned min_keys = 0;        // the run fails when rule 2 judged fewer keys than this (a walk whose keys are all excused or unquiet proves nothing)
  unsigned min_sel = 0;         // ... and when rule 3 judged fewer inputs than this
  unsigned min_conclusive = 0;  // the run fails when fewer undo bursts than this ended in a compare (a walk that never compares proves nothing)
  int plant_leak_at = -1;  // self-test: at this step the harness opens a directory and never closes it
  bool audio_probe = false;      // C4: the audio probe after a step that rebinds an instrument's descriptor
  bool audio_session = false;    // C4: and the edit session at the middle of the walk (a replay and about four probes a seed)
  unsigned probe_every = 1;      // C4: every k-th rebind is probed (the short run: not every one costs a replay)
  bool probe_selftest = false;
  bool meta = false;           // B5: the metamorphic relations (save then OPEN, a transposition and its inverse, copy + paste + undo, a slice edit + undo), on their own random stream and a fixed cadence
  unsigned meta_count = 10;    // B5: n relations after the walk's last step (the walk itself is not touched: the baseline's seed@step repros hold)
  unsigned min_meta = 0;       // B5: the run fails when fewer relations than this ended in a compare
  unsigned min_save_open = 0;  // B5: ... and when fewer than this save then OPEN (the relation that is the point of B5: a skip rate that hides it must fail the run)
  bool meta_selftest = false;  // B5: each relation is reached, and a save that loses a field is found
  bool plant_meta = false;     // self-test: the document gets one more edit after the save and before OPEN (as if save_project had not written a field), so OPEN cannot give the document back
  int meta_only = -1;          // self-test: only this relation (index into kMetaNames)
  bool exhaust = false;        // B8: the limit fixtures (undo cap, sample-edit history cap), after the walk and after --meta, on their own random stream
  unsigned min_exhaust = 0;    // B8: the run fails when fewer fixtures than this reached their limit and ended in their checks
  bool exhaust_all = false;    // B8: every fixture on every seed (the short run), and not the seeds each is assigned to
  bool exhaust_selftest = false;  // B8: each fixture reaches its limit, and a defect planted at the limit is found
  bool exhaust_fresh_selftest = false;  // B8: the same for the fixtures that run on a simulator of their own (the voice pool, the folder of 2000)
  bool plant_exhaust = false;  // self-test: at the limit a unit goes unrecorded (the undo cap: the ring's count does not move; the sample-edit cap: the pattern changes with the edit)
  int exhaust_only = -1;       // self-test: only this fixture (index into kExhaustNames), whatever the seed
};

// B8: the fixtures. The invariant of a finding is `exhaust_<name>`.
constexpr unsigned kExhaustCount = 5;
constexpr const char* kExhaustNames[kExhaustCount] = {"undo_cap", "sedit_cap", "sedit_switch", "voice_pool", "big_folder"};
constexpr unsigned kFixtureFirstFresh = 3;  // the fixtures from this one on run on a simulator of their own (the walk's is dropped): they come last

// B8 big_folder: the card of the fixture, made at test time in a scratch directory and never committed (the generated card of B1 has one folder of 600 files; the listing holds kMaxEntries = 256).
// A tiny valid WAV (16-bit mono, 48 frames): the listing probes the header of every .wav it lists (tag_stereo), and a load from the full listing must read one.
std::string tiny_wav(unsigned n) {
  constexpr unsigned kFrames = 48;
  std::string data;
  for (unsigned i = 0; i < kFrames; ++i) {
    const int v = static_cast<int>((i * (n % 7u + 1u) * 700u) % 8000u) - 4000;
    data.push_back(static_cast<char>(v & 0xFF));
    data.push_back(static_cast<char>((v >> 8) & 0xFF));
  }
  const auto le32 = [](std::uint32_t x) {
    std::string s;
    for (unsigned k = 0; k < 4; ++k) {
      s.push_back(static_cast<char>((x >> (8 * k)) & 0xFF));
    }
    return s;
  };
  const auto le16 = [](std::uint32_t x) {
    std::string s;
    s.push_back(static_cast<char>(x & 0xFF));
    s.push_back(static_cast<char>((x >> 8) & 0xFF));
    return s;
  };
  std::string fmt = le16(1) + le16(1) + le32(22050) + le32(44100) + le16(2) + le16(16);
  std::string body = "WAVEfmt " + le32(static_cast<std::uint32_t>(fmt.size())) + fmt + "data" + le32(static_cast<std::uint32_t>(data.size())) + data;
  return "RIFF" + le32(static_cast<std::uint32_t>(body.size())) + body;
}

// The scratch card of big_folder: `big/` holds kBigFiles files in all: `wavs` of them .wav, the rest .txt files and hidden "._" files (the listing skips those); `small/` holds three .wav. Removed when it goes out of scope.
constexpr unsigned kBigFiles = 2000;
constexpr unsigned kBigJunk = 400;  // 200 .txt and 200 hidden
struct ScratchCard {
  std::filesystem::path root;
  ScratchCard() = default;
  ScratchCard(const ScratchCard&) = delete;
  ScratchCard& operator=(const ScratchCard&) = delete;
  ~ScratchCard() { remove(); }
  void remove() {
    std::error_code ec;
    if (!root.empty()) {
      std::filesystem::remove_all(root, ec);
    }
    root.clear();
  }
  static bool put(const std::filesystem::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
  }
  bool make(std::uint64_t seed, unsigned wavs, std::string& error) {
    std::error_code ec;
    root = std::filesystem::temp_directory_path(ec) / ("o3_qanet_big_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(static_cast<unsigned long long>(seed)));
    std::filesystem::remove_all(root, ec);
    if (!std::filesystem::create_directories(root / "big", ec) || !std::filesystem::create_directories(root / "small", ec)) {
      error = "no scratch directory for the card";
      return false;
    }
    char name[40];
    for (unsigned i = 0; i < wavs; ++i) {
      std::snprintf(name, sizeof name, "s%04u.wav", i);
      if (!put(root / "big" / name, tiny_wav(i))) {
        error = "a file could not be written";
        return false;
      }
    }
    for (unsigned i = 0; i < kBigJunk / 2u; ++i) {
      std::snprintf(name, sizeof name, "n%04u.txt", i);
      bool ok = put(root / "big" / name, "not audio\n");
      std::snprintf(name, sizeof name, "._s%04u.wav", i);
      ok = put(root / "big" / name, "hidden") && ok;
      if (!ok) {
        error = "a file could not be written";
        return false;
      }
    }
    for (unsigned i = 0; i < 3; ++i) {
      std::snprintf(name, sizeof name, "a%u.wav", i);
      if (!put(root / "small" / name, tiny_wav(i))) {
        error = "a file could not be written";
        return false;
      }
    }
    return true;
  }
};

// B5: the relations, in the order the walk cycles through them (the start of the cycle is the seed's). The invariant of a finding is `meta_<name>`.
constexpr unsigned kMetaCount = 5;
constexpr const char* kMetaNames[kMetaCount] = {"save_open", "transpose", "octave", "copy_paste", "slice"};

struct RunStats {
  std::set<std::string> tuples;  // distinct `screen from | action | screen to` of the steps
  std::uint64_t digest = 0;
  unsigned ticks = 0;
  unsigned bursts = 0;
  unsigned bursts_conclusive = 0;
  unsigned bursts_session = 0;  // not compared: the burst moved the project's session state, which is persisted and has no undo unit
  unsigned bursts_with_edits = 0;
  unsigned bursts_shrunk = 0;  // not compared: the journal got shorter than it was before the burst (an undo the burst did not count)
  unsigned new_projects = 0;
  unsigned new_then_load = 0;  // a NEW PROJECT strip, then Sample load with a Done listing, with nothing in between that rescanned (measurement only)
  unsigned screens_seen = 0;
  unsigned questions = 0;  // steps that ended with a question or dialog of the File screen up (its keys= field is in the state line)
  unsigned loads_seen = 0;
  unsigned files_seen = 0;
  unsigned cards_pulled = 0;
  unsigned oracle_runs = 0;
  unsigned rebinds = 0;       // C4: steps after which an instrument's sample, mode, slices or loop was not what it was
  unsigned probe_points = 0;  // C4: of those, the replays that were probed, and the session point
  unsigned probes = 0;        // C4: the instruments played (pad straight after, pad settled, Pattern step or the settled pad as the reference)
  unsigned probes_stepped = 0;
  unsigned probes_skipped = 0;
  unsigned units_checked = 0;       // B4: inputs the rules judged
  unsigned units_session = 0;       // ... and the inputs that moved the session state, which are not (allow-list entry 1)
  std::map<std::string, unsigned> session_by_screen;  // ... the same count per screen the input was made on (a screen that is only ever skipped is not judged at all)
  std::map<std::string, unsigned> judged_by_screen;   // ... and the inputs that were judged, per screen
  unsigned rule_findings = 0;       // ... and the findings of the rules, every one (a run records each distinct one once)
  unsigned keys_seen = 0;           // B4b: a bound F key or arrow the walk pressed (keys_judged, keys_excused and keys_unquiet add up to it)
  unsigned keys_judged = 0;         // ... and rule 2 judged it (the key reached check_key: not excused, and the wait before it was quiet)
  unsigned keys_excused = 0;        // ... not judged: excused by A2's rules as far as the chain tells (an arrow, a key with a sibling, the placeholder soft key)
  unsigned keys_unquiet = 0;        // ... and inputs not judged: something moved by itself during the wait before the key (the control), so the state before is no reference
  unsigned keys_silent = 0;         // ... and whose verdict was silent
  unsigned watchsel_seen = 0;       // OS-BUG-WATCHSEL: select.extend (Shift + an arrow on Pattern) pressed while the transport plays, and judged
  unsigned watchsel_silent = 0;     // ... of which silent (the bug: the selection did not move while playing)
  unsigned watchsel_watch = 0;      // ... of which in WATCH (Browse), the mode the bug was found in (the rest: Edit following, LIVE)
  unsigned sel_judged = 0;          // rule 3 judged the input (its action is in the table and applies)
  unsigned sel_units = 0;           // inputs made with a pattern selection up
  unsigned sel_no_pattern = 0;      // table actions made in a pattern that did not exist yet: not judged (a limit of rule 3)
  std::map<std::string, unsigned> sel_by_action;
  std::map<std::string, unsigned> key_by_action;
  int first_sel_step = -1;
  unsigned slot_fills = 0;          // an input after which a sample slot that held nothing holds audio
  unsigned slot_overwrites = 0;     // an input on Sample load after which a slot that held audio holds other audio (a WAV load over a used slot: OS-BUG-SLDOVER)
  unsigned slot_overwrites_flagged = 0;  // ... of which the rules said something (R1)
  unsigned slot_overwrites_same = 0;     // ... of which left the document as it was (the same audio loaded again: nothing to record)
  int first_fill_step = -1;         // the walk's step of the first fill
  int first_change_step = -1;       // the walk's step of the first input that changed the document
  unsigned meta_ok[kMetaCount] = {};    // B5: relations that ended in a compare, per relation
  unsigned meta_tuned = 0;              // B5: saves that went out with a fine tune off its default in the document (the field a lossy Save must give back)
  unsigned meta_tuned_ok = 0;           // B5: ... and those of them whose OPEN ended in a compare (what --min-save-open counts: a plant that stops reaching FINE TUNE must fail the run)
  unsigned exh_ok[kExhaustCount] = {};    // B8: fixtures that reached their limit and ended in their checks, per fixture
  unsigned exh_skip[kExhaustCount] = {};  // ... and those that did not (no sample, a selection that did not form, the limit not reached within the bound)
  std::string exh_notes;                  // ... and why, per seed
  unsigned exh_units = 0;                 // B8: undo units the undo cap's inserts made before the ring dropped; edits the sample-edit cap made past the cap (the work the fixtures did)
  unsigned meta_skip[kMetaCount] = {};  // ... and those that could not be set up (no note under the cursor, no sample, a refused save ...) or moved the session state: not compared
  std::vector<Input> burst_script;  // the inputs of the run up to the end of the first conclusive undo burst that changed the document (its mark and undos included), and the label of that burst's mark: the proof of the written undo assertion
  std::string burst_label;
};

std::string field(const std::string& line, const char* key) {
  const std::string k = std::string(key) + "=";
  std::size_t at = 0;
  while (true) {
    at = line.find(k, at);
    if (at == std::string::npos) {
      return std::string();
    }
    if (at == 0 || line[at - 1] == ' ') {
      break;
    }
    at += k.size();
  }
  at += k.size();
  const std::size_t end = line.find(' ', at);
  return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The project's session state (OS-PERSIST2, model/project_state.h): the note-entry settings and JUMP, the mute modes, the Perform scenes, the send effects, the mixer, the Perform slots, MIDI and USB. They
// are written into the native file, and none of them is an undo unit (a chooser's JUMP, a mixer fader, a mute mode are the player's setting of the session, as the header says), so a burst that moved
// one changes the digest by design: the burst is then not compared (allow-list entry 1, `session` in the coverage line). The bytes are those of the structs as the project holds them.
std::string session_signature(const model::Project& p) {
  std::string out;
  const auto add = [&out](const void* v, std::size_t n) { out.append(static_cast<const char*>(v), n); };
  const std::uint8_t present = p.state_present();
  add(&present, sizeof present);
  add(&p.midi(), sizeof p.midi());
  add(&p.usb(), sizeof p.usb());
  add(&p.mix(), sizeof p.mix());
  add(&p.perf(), sizeof p.perf());
  add(&p.note(), sizeof p.note());
  add(&p.mute(), sizeof p.mute());
  add(&p.scenes(), sizeof p.scenes());
  add(&p.send_fx(), sizeof p.send_fx());
  return out;
}

// The scenario lines that end a written undo finding: undo what the burst's edits did and the document is where the mark left it. One text for the walk's finding and for the self-test that runs it.
std::string undo_tail(const std::string& label) { return "assert model digest=@" + label; }

// ---- the rules of part A at every step (B4) ------------------------------------------------------------------------------------------------------------------------

// What the walk reads of the simulator beside a qanet::Snap (the rules library is part A's and stays as it is): the document and the redo stacks, so that an undo, a redo and a NEW or OPEN are told by what they
// did (the walk presses keys, and the simulator does not say which action a key ran), and the sample slots, for the report of loads over a used slot.
struct Extra {
  const void* project = nullptr;
  const void* editor = nullptr;
  std::string session;  // session_signature(): what the native file holds besides the document
  unsigned redo[qanet::kRings] = {0, 0, 0};
  unsigned sedit_redo = 0;
  bool used[app::SampleBank::kSlots] = {};
  std::uint32_t version[app::SampleBank::kSlots] = {};
  unsigned slots_used() const {
    unsigned n = 0;
    for (const bool u : used) {
      n += u ? 1u : 0u;
    }
    return n;
  }
};

Extra extra_of(sim::Simulator& s) {
  Extra e;
  e.project = s.project();
  e.editor = s.app().session().document();
  if (s.project() != nullptr) {
    e.session = session_signature(*s.project());
  }
  if (const model::Editor* ed = s.app().session().document()) {
    static const model::UndoArea areas[qanet::kRings] = {model::UndoArea::Pattern, model::UndoArea::Song, model::UndoArea::Sound};
    for (unsigned i = 0; i < qanet::kRings; ++i) {
      e.redo[i] = ed->journal(areas[i]).redo_depth();
    }
  }
  if (const app::SampleBank* bank = s.app().model().samples) {
    for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
      e.used[slot] = bank->pcm(slot).frames != 0u;
      e.version[slot] = bank->version(slot);
      e.sedit_redo += bank->redo_depth(slot);
    }
  }
  return e;
}

// The registry name of the action that ran, as far as the rules need it (they use it for the four that may lower the undo depth and need no unit): told by effect. A new document is File NEW or OPEN (the project object or its editor changed, or the strip says the load ended); a ring that
// lost one unit while its redo stack gained one is an undo, the reverse a redo (the sample editor's history the same). Anything else is "step", which gets no exemption. LIMIT: an action that does that
// by itself (a clear that leaves one unit less and one redo more) would pass as an undo; none exists, and the rule that would catch it is the one that is exempt.
std::string classify(const qanet::Snap& b, const qanet::Snap& a, const Extra& be, const Extra& ae) {
  // A document is replaced in place by a job (the project object stays): the strip then says so, in these words (app.cpp, the pass that sees the load end).
  const auto starts = [&](const char* w) { return a.msg.compare(0, std::strlen(w), w) == 0; };
  const auto ends = [&](const char* w) { return a.msg.size() >= std::strlen(w) && a.msg.compare(a.msg.size() - std::strlen(w), std::string::npos, w) == 0; };
  if (be.project != ae.project || be.editor != ae.editor || (a.posts != b.posts && (a.msg == "NEW_PROJECT" || (starts("PROJECT_") && (ends("_OPENED") || ends("_RECOVERED")))))) {
    return "file.new";
  }
  for (unsigned i = 0; i < qanet::kRings; ++i) {
    if (a.ring[i] + 1u == b.ring[i] && ae.redo[i] == be.redo[i] + 1u) {
      return "edit.undo";
    }
    if (a.ring[i] == b.ring[i] + 1u && ae.redo[i] + 1u == be.redo[i]) {
      return "edit.redo";
    }
  }
  if (a.sedit + 1u == b.sedit && ae.sedit_redo == be.sedit_redo + 1u) {
    return "sedit.undo";
  }
  if (a.sedit == b.sedit + 1u && ae.sedit_redo + 1u == be.sedit_redo) {
    return "sedit.redo";
  }
  return "step";
}

// One input of the walk, as a rule sees it beyond the two snapshots.
struct Step {
  std::string action;  // classify()
  bool answer = false; // the input pressed an answer key of the question that was up before it
  std::string key;     // `F3`, `SF3` (Shift + F3), "" when the input was not a function key
  std::string input;   // the walk's text for it (`tap f3`, `shift+f3`, `pad`, `encoder`): the action of the baseline key
  // B4b: rule 2 and rule 3. `act`/`arg`: the registry name and argument of the action the binding chain gave the input ("" none).
  std::string act;
  int arg = 0;
  bool key_judged = false;                    // rule 2 applies: a bound F key or arrow
  qanet::KeyFacts facts;
  bool drifting = false;                      // the sequencer plays: what the playhead moves by itself is not the key's
  const qanet::Snap* key_after = nullptr;     // self-test plant: what rule 2 sees as the state after (null: the real one)
  const qanet::SelImage* img_before = nullptr;
  const qanet::SelImage* img_after = nullptr;
  qanet::SelContext sel_ctx;
};

// THE PLUG FOR THE RULES OF PART A. A rule is a function of the snapshots before and after one input and of what the walk knows of that input (Step); the findings it returns are walk findings (invariant = the rule's
// name, `R1`...), keyed, shrunk and baselined like the rest. Rule 1 is check_rules() of rules.h, rule 2 check_key() and rule 3 check_selection(): their adapters read Step::facts and Step::img_before/img_after,
// which the walk fills in prepare_rules23(). The walk takes the snapshots with qanet::snap_of (the previous one as `prev`, as the library asks) and judges only the inputs of the kinds that are one action (not a wait, a
// card pull or a burst's undos).
using StepRule = std::vector<qanet::Finding> (*)(const qanet::Snap& before, const qanet::Snap& after, const Step& step);
std::vector<qanet::Finding> rule_check_rules(const qanet::Snap& before, const qanet::Snap& after, const Step& step) { return qanet::check_rules(before, after, step.action, step.answer); }
// Rule 2 (silent no-op key): `Step::key_judged` says the input is a bound F key or arrow. The state before the input is the reference (what the state would be without the key): the walk made sure of it by waiting
// before the key (pre_wait), a control that is the wait of A2's control run, and judged the key only if nothing had moved by itself.
std::vector<qanet::Finding> rule_check_key(const qanet::Snap& before, const qanet::Snap& after, const Step& step) {
  if (!step.key_judged) {
    return {};
  }
  return qanet::check_key(step.facts, before, step.key_after != nullptr ? *step.key_after : after, step.drifting);
}
// Rule 3 (selection span): the images are taken by the walk when the input's action is in the table (selection_ops.h).
std::vector<qanet::Finding> rule_check_selection(const qanet::Snap&, const qanet::Snap&, const Step& step) {
  if (step.img_before == nullptr || step.img_after == nullptr) {
    return {};
  }
  return qanet::check_selection(*step.img_before, *step.img_after, step.act, step.arg, step.sel_ctx);
}
const StepRule kStepRules[] = {rule_check_rules, rule_check_key, rule_check_selection};

// The input of a unit as the binding chain sees it: the first tap, encoder turn or pad of its lines (a held Shift is the modifier), resolved against the chain as it was before the input. The registry name and
// the argument of the action, and whether the input is a function key or an arrow (the keys of rule 2).
struct Resolved {
  bool ok = false;     // the chain binds it to an action (not `none`)
  std::string action;
  int arg = 0;
  bool fkey = false, arrow = false, shift = false;
  bool tapped = false;  // the input is a tap of a button
  app::Button button = app::Button::F1;
  // What A2 excuses by probing the state (keys_test.cpp walk_state) and the walk can tell from the chain alone: `sibling`: another F key of the row (same layer) is bound to the same action with another argument
  // (a key that selects what is already selected: chooser.toggle, instrument.page, param.group, pads.mode ...; not the actions whose argument names a function); `fallback`: Shift + an arrow that the resolver
  // gives the plain arrow's binding (core/binding.h: nothing of its own to do under Shift).
  bool sibling = false;
  bool fallback = false;
};

// The name of the action a gesture resolves to in `chain` ("" none), and its argument.
std::string resolved_name(const core::BindingSet& set, const core::ContextChain& chain, const core::Gesture& g, int& arg) {
  const core::Resolution res = core::resolve(set, chain, g);
  arg = 0;
  if (res.status != core::Resolution::Status::Bound || res.action == core::ActionId::None || res.action == core::action_id("ui.none")) {
    return std::string();
  }
  const app::ActionTable registry = app::action_registry();
  for (std::size_t k = 0; k < registry.count; ++k) {
    if (registry.items[k].id == res.action) {
      arg = static_cast<int>(res.arg);
      return registry.items[k].name;
    }
  }
  return std::string();
}

Resolved resolve_input(const core::ContextChain& chain, const std::vector<Input>& h, std::size_t from) {
  static const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  Resolved r;
  bool shift = false;
  for (std::size_t i = from; i < h.size(); ++i) {
    const std::string& l = h[i].line;
    core::Gesture g{0, core::GestureKind::Tap, core::kNoInput, core::kNoInput, 0};
    if (l == "press shift") {
      shift = true;
      continue;
    }
    if (l.compare(0, 4, "tap ") == 0) {
      const std::string name = l.substr(4);
      bool found = false;
      for (const app::ButtonName& b : app::kButtonNames) {
        if (name == b.name) {
          g.input = app::button_ref(b.button);
          r.button = b.button;
          found = true;
        }
      }
      if (!found || r.button == app::Button::Shift) {
        continue;
      }
      r.tapped = true;
      r.shift = shift;
      g.modifier = shift ? app::button_ref(app::Button::Shift) : core::kNoInput;
      r.fkey = static_cast<unsigned>(r.button) <= static_cast<unsigned>(app::Button::F8);
      r.arrow = r.button == app::Button::Up || r.button == app::Button::Down || r.button == app::Button::Left || r.button == app::Button::Right;
    } else if (l.compare(0, 8, "encoder ") == 0) {
      g.input = app::encoder_ref(0);
      g.value = static_cast<std::int16_t>(std::atoi(l.c_str() + 8));
    } else if (l.compare(0, 4, "pad ") == 0) {
      g.input = app::pad_ref(static_cast<std::uint8_t>(std::atoi(l.c_str() + 4)));
      g.value = 127;
    } else {
      continue;
    }
    r.action = resolved_name(set, chain, g, r.arg);
    r.ok = !r.action.empty();
    if (r.ok && r.tapped && r.fkey) {
      const std::string name = r.action;
      for (unsigned k = 0; k <= static_cast<unsigned>(app::Button::F8); ++k) {
        if (k == static_cast<unsigned>(r.button)) {
          continue;
        }
        core::Gesture o = g;
        o.input = app::button_ref(static_cast<app::Button>(k));
        int oarg = 0;
        const std::string oname = resolved_name(set, chain, o, oarg);
        r.sibling = r.sibling || (oname == name && oarg != r.arg && name != "perform.key" && name != "dj.key" && name != "ui.softkey");
      }
    }
    if (r.ok && r.tapped && r.arrow && r.shift) {
      core::Gesture plain = g;
      plain.modifier = core::kNoInput;
      int parg = 0;
      r.fallback = resolved_name(set, chain, plain, parg) == r.action && parg == r.arg;
    }
    return r;
  }
  return r;
}

// Whether a tap of `b` (with Shift or not) is held back for a double tap in `chain`: the gesture layer decides the tap only when the double-tap window (300 ms) has passed, and the action runs then.
bool tap_delayed(const core::ContextChain& chain, app::Button b, bool shift) {
  static const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  const core::Gesture g{0, core::GestureKind::DoubleTap, app::button_ref(b), shift ? app::button_ref(app::Button::Shift) : core::kNoInput, 0};
  const core::Resolution res = core::resolve(set, chain, g);
  return res.status == core::Resolution::Status::Bound && res.action != core::ActionId::None && res.action != core::action_id("ui.none");
}

// The screens whose actions rule 3's table judges: the image of the document is copied before an input made on one of them.
bool selection_screen(app::ScreenId s) {
  return s == app::ScreenId::Pattern || s == app::ScreenId::Song || s == app::ScreenId::Wave || s == app::ScreenId::SampleEdit || s == app::ScreenId::Instrument;
}

std::string native_part(const std::string& digest) { return digest.substr(0, digest.find('/')); }  // the project's digest without the samples' hash

// The input of a unit by its scenario lines (`from` on): the same text the walk gives its own steps, so that a replay of a shrunk script names the input as the walk did.
std::string describe_unit(const std::vector<Input>& h, std::size_t from, std::string& key) {
  bool shift = false;
  key.clear();
  for (std::size_t i = from; i < h.size(); ++i) {
    const std::string& l = h[i].line;
    if (l == "press shift") {
      shift = true;
    } else if (l.compare(0, 4, "tap ") == 0) {
      const std::string name = l.substr(4);
      if (name.size() == 2 && name[0] == 'f' && name[1] >= '1' && name[1] <= '8') {
        key = std::string(shift ? "SF" : "F") + name.substr(1);
      }
      return shift ? "shift+" + name : l;
    } else if (l.compare(0, 7, "encoder") == 0) {
      return "encoder";
    } else if (l.compare(0, 3, "pad") == 0) {
      return "pad";
    }
  }
  return "input";
}

// Whether `key` is one of the answer keys in a `keys=` field ("F3:ALL,F1:KEEP_INS,SF7:PURGE?").
bool answers_key(const std::string& keys, const std::string& key) {
  if (key.empty()) {
    return false;
  }
  std::size_t at = 0;
  while (at < keys.size()) {
    const std::size_t end = keys.find(',', at);
    const std::string token = keys.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (token.compare(0, key.size() + 1u, key + ":") == 0) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    at = end + 1;
  }
  return false;
}

class Run {
 public:
  Run(const Options& opt, std::uint64_t seed) : opt_(opt), seed_(seed), rng_(seed), meta_rng_(seed ^ 0x6D657461B5B5B5B5ull), exh_rng_(seed ^ 0x45584841B8B8B8B8ull), sim_(std::make_unique<sim::Simulator>()) {
    meta_n_ = meta_rng_.below(kMetaCount);
  }

  // false: the run could not start (`error`).
  bool start(std::string& error) {
    // The order of `o3sim --scenario` (run_scenario: one tick, then `document`, then `card`): a written scenario starts from the same state.
    sim_->set_scenario_dir(std::string());
    sim_->step();
    sim_->open_document();
    if (!sim_->open_card(opt_.card, error)) {
      return false;
    }
    ticks_ = 1;
    if (opt_.audio_probe && !sim_->audio_on(error)) {  // C4: the real engine (the unit has one: Rec starts a live take on its transport, a pad sounds): the walk and its replays have it from the start, in `o3sim`'s order (`audio on` after `card`)
      return false;
    }
    if (!opt_.plant_hold.empty()) {
      std::vector<std::string> words;
      std::istringstream in(opt_.plant_hold);
      for (std::string w; in >> w;) {
        words.push_back(w);
      }
      if (!sim::parse_state_pairs(words, hold_, error)) {
        return false;
      }
    }
    undo_id_ = app::find_action(app::action_registry(), "edit.undo", 9)->id;
    if (opt_.audio_probe && sim_->project() != nullptr) {
      (void)qanet_audio::watch_instruments(watch_, *sim_->project(), false, nullptr);
    }
    return true;
  }

  void go(std::vector<Finding>& findings) {
    findings_ = &findings;
    for (step_ = 0; step_ < opt_.steps; ++step_) {
      do_step();
      rest();
      check();
      mix_state();
      if (opt_.audio_probe) {
        audio_probe_step();
      }
      if (opt_.states) {
        std::printf("seed %llu step %u: %s\n", static_cast<unsigned long long>(seed_), step_, state_line().c_str());
      }
      if (opt_.plant_leak_at >= 0 && static_cast<int>(step_) >= opt_.plant_leak_at && !planted_) {
        plant_leak();
      }
      if (opt_.stop_at_plant && (planted_r1_ || planted_r2_ || planted_r3_)) {
        break;
      }
    }
    // B5: the relations come after the walk (they move the document and the screens, so before a step they would move every later step of the walk and the baseline's repros with it).
    for (unsigned n = 0; opt_.meta && n < opt_.meta_count; ++n) {
      meta_step();
      rest();
      check();
      mix_state();
    }
    if (opt_.exhaust) {  // B8: the limit fixtures, after the relations for the same reason
      exhaust_step();
    }
    settle();
    rest();
    check();
    mix_state();
    stats_.ticks = ticks_;
    std::uint64_t frame = 0xCBF29CE484222325ull;  // FNV-1a of the frame's digest text: the same on any library (std::hash is not)
    for (const char c : sim::framebuffer_sha256(sim_->raster())) {
      frame = (frame ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
    }
    stats_.digest = digest_ ^ frame;
  }

  const RunStats& stats() const { return stats_; }

  // B2: the inputs `lines` (scenario lines, as rec() writes them) given to a fresh run, the invariants checked after each. The index of the first line after which `invariant` is found, -1 when
  // none is, -2 when the script is not one `o3sim` would run (a line that does not parse, an action that is not enabled, a card call that fails). Findings go to `sink`.
  // The invariants are looked at after the lines flagged `rest`, where the walk looked: a script cut down from the walk's must not fail at a state in the middle of a step that the walk never judged.
  int replay(const std::vector<Input>& lines, const std::string& invariant, std::vector<Finding>& sink) {
    findings_ = &sink;
    std::string text;
    for (const Input& l : lines) {
      text += l.line + "\n";
    }
    std::vector<sim::ScenarioCommand> commands;
    std::string error;
    if (!sim::parse_scenario(text, commands, error) || commands.size() != lines.size()) {
      return -2;
    }
    using Cmd = sim::ScenarioCommand::Kind;
    for (std::size_t i = 0; i < commands.size(); ++i) {
      const sim::ScenarioCommand& c = commands[i];
      step_ = static_cast<unsigned>(i);
      input_ = lines[i].line;
      history_.push_back(lines[i]);
      if (lines[i].begin && opt_.rules_every != 0u) {  // the unit the rules judge: the state before its first line
        before_ = qanet::snap_of(*sim_);
        before_extra_ = extra_of(*sim_);
        begin_context();
        unit_from_ = i;
        unit_open_ = true;
      }
      switch (c.kind) {
        case Cmd::Wait: sim_->wait_ms(c.value); tick(c.value); break;
        case Cmd::Press: sim_->press(c.button); tick(); break;
        case Cmd::Release: sim_->release(c.button); tick(); break;
        case Cmd::Tap:
          for (std::uint32_t n = 0; n < c.value; ++n) {
            sim_->tap(c.button);
            tick(2);
          }
          break;
        case Cmd::Pad: sim_->pad(c.value); tick(2); break;
        case Cmd::Encoder:
          for (std::uint32_t n = 0; n < (c.value == 0u ? 1u : c.value); ++n) {
            sim_->encoder(c.delta);
            tick();
          }
          break;
        case Cmd::Action:
          if (sim_->action(c.action, c.arg) == app::DispatchResult::Disabled) {
            return -2;
          }
          tick();
          break;
        case Cmd::Card:
          if (c.value == 1u ? !sim_->card_pull(error) : c.value == 2u ? !sim_->card_insert(error) : true) {
            return -2;
          }
          tick(c.value == 1u ? 1u : 2u * 16u + 1u);
          break;
        case Cmd::Mark: break;  // the walk writes `mark model` before an undo burst, for the assertion of that finding: it moves nothing
        default: return -2;     // the walk gives no other input
      }
      if (lines[i].end && unit_open_) {
        unit_open_ = false;
        const qanet::Snap after = qanet::snap_of(*sim_, &before_);
        judge(before_, after, before_extra_, extra_of(*sim_));
      }
      if (lines[i].rest) {
        check();
      }
      if (reported_.count(invariant) != 0u) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

 private:
  // ---- the steps (every one counts the ticks it takes: the simulator's press, release, encoder and pad run one pass each) ----

  void tick(unsigned n = 1) { ticks_ += n; }
  void rest() {  // the invariants are looked at now: the last input is the end of a step
    if (!history_.empty()) {
      history_.back().rest = true;
    }
  }
  // Every input the run gives the simulator, as the scenario line that gives the same (sim/include/o3os/sim/scenario.h): the repro of a finding, and what --trace prints.
  void rec(const std::string& line) {
    history_.push_back(Input{line, false, false, false});
    if (pending_begin_) {
      history_.back().begin = true;
      unit_from_ = history_.size() - 1u;
      pending_begin_ = false;
    }
    if (!in_unit_) {
      snap_valid_ = false;  // an input outside a unit moves the state: the snapshot taken after the last unit is stale
    }
    if (opt_.trace) {
      std::printf("seed %llu step %u: %s\n", static_cast<unsigned long long>(seed_), step_, line.c_str());
    }
  }
  static const char* button_name(app::Button b) {
    for (const app::ButtonName& n : app::kButtonNames) {
      if (n.button == b) {
        return n.name;
      }
    }
    return "?";
  }
  void tap(app::Button b) {
    input_ = std::string("tap ") + button_name(b);
    tapped_ = true;
    tapped_button_ = b;
    tapped_shift_ = shift_held_;
    rec(input_);
    sim_->tap(b);
    tick(2);
  }
  void wait(unsigned ms) {
    rec("wait " + std::to_string(ms));
    sim_->wait_ms(ms);
    tick(ms);
  }

  // ---- the rules at every step (B4) ----

  // The inputs that are one action of the person (not a wait, a card pull or a burst) are units: a snapshot before the first line, the quiet waits that let the action's job finish, a snapshot after, and the rules
  // judge the pair. The waits are inputs of the script like any other (the replay of a shrunk script runs them as they are). Every `rules_every`-th unit is judged; the others run the same waits and only skip the snapshots, so the walk is the same for every n.
  // What rules 2 and 3 need of the state before the input (the walk's units and a replay's): the context chain, and the image of the document when the screen is one the table of rule 3 judges.
  void begin_context() {
    chain_ = qanet::live_chain(*sim_);
    unit_ticks0_ = ticks_;
    img_ok_ = selection_screen(sim_->app().state().screen) && qanet::image_of(*sim_, img_before_) && img_before_.ok;
  }

  void unit_begin() {
    in_unit_ = true;
    unit_active_ = false;
    tapped_ = false;
    if (opt_.rules_every == 0u) {
      return;
    }
    chain_ = qanet::live_chain(*sim_);  // (every unit: quiet() asks it for a delayed tap, judged or not, so that the walk is the same whichever inputs are judged)
    if (++units_ % opt_.rules_every != 0u) {
      return;
    }
    if (!snap_valid_) {
      cur_ = qanet::snap_of(*sim_);
      cur_extra_ = extra_of(*sim_);
      snap_valid_ = true;
    }
    before_ = cur_;
    before_extra_ = cur_extra_;
    begin_context();
    unit_active_ = true;
    pending_begin_ = true;
  }

  struct Pulse {
    std::uint32_t generation = 0;
    unsigned units = 0;
    std::uint64_t audio = 0;
    std::string where;
    bool job = false;
    bool operator==(const Pulse& o) const { return generation == o.generation && units == o.units && audio == o.audio && where == o.where && job == o.job; }
  };
  // What moves while an action's job runs (the project's generation, the undo units, the sample bank, the strip and screen, a card job that is not a listing: a listing is no action's result).
  Pulse pulse() {
    Pulse p;
    p.where = field(state_line(), "screen") + "/" + field(state_line(), "overlay") + "/" + field(state_line(), "posts");
    if (sim_->project() != nullptr) {
      p.generation = sim_->project()->generation();
    }
    if (const model::Editor* ed = sim_->app().session().document()) {
      for (const model::UndoArea a : {model::UndoArea::Pattern, model::UndoArea::Song, model::UndoArea::Sound}) {
        p.units += ed->journal(a).depth() + ed->journal(a).dropped_units();
      }
    }
    if (const app::SampleBank* bank = sim_->app().model().samples) {
      for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
        p.audio = p.audio * 31u + bank->pcm(slot).frames + bank->undo_depth(slot) + bank->version(slot);
      }
    }
    const app::DirScan* files = sim_->app().model().files;
    p.job = (sim_->app().card_jobs_busy() && !(files != nullptr && files->state() == app::DirScan::State::Scanning)) || sim_->app().state().file.opening;  // (a project load runs in slices: `opening` is up until the pass that ends it)
    return p;
  }
  void quiet() {  // as qanet::settle() (60 ms of nothing moving, 600 ms at most), in recorded waits; a card job that runs is never still: the wait goes on while it runs, up to the job bound (a long job's effect lands in its own unit)
    if (tapped_ && tap_delayed(chain_, tapped_button_, tapped_shift_)) {
      wait(320);  // a key with a double-tap binding does its work 300 ms after the tap: an action that lands in the next unit is no action of this one
    }
    Pulse last = pulse();
    unsigned still = 0;
    for (unsigned t = 0; t < kJobBoundTicks && still < 3u && (t < 600u || last.job); t += 20u) {
      wait(20);
      const Pulse now = pulse();
      still = (now == last && !now.job) ? still + 1u : 0u;
      last = now;
    }
  }
  void unit_end(bool in_burst) {
    if (!unit_active_) {
      if (opt_.rules_every != 0u && !in_burst) {
        quiet();  // the walk is the same whichever inputs are judged (a baseline made at one n holds at another)
      }
      in_unit_ = false;
      snap_valid_ = false;
      return;
    }
    if (!in_burst) {  // an edit of a burst is synchronous; a burst's waits are its own
      quiet();
    }
    if (!history_.empty()) {
      history_.back().end = true;
    }
    const qanet::Snap after = qanet::snap_of(*sim_, &before_);
    const Extra after_extra = extra_of(*sim_);
    judge(before_, after, before_extra_, after_extra);
    cur_ = after;
    cur_extra_ = after_extra;
    snap_valid_ = true;
    unit_active_ = false;
    in_unit_ = false;
  }

  // The rules over one unit (the lines of history_ from unit_from_ on): counts what the report wants, and records every finding once per run (the rule, the screen after, the input and the reason are its key).
  void judge(const qanet::Snap& before, qanet::Snap after, const Extra& be, const Extra& ae) {
    ++stats_.units_checked;
    Step st;
    st.input = describe_unit(history_, unit_from_, st.key);
    st.action = classify(before, after, be, ae);
    st.answer = qanet::question_up(before) && answers_key(before.keys, st.key);
    bool planted_now = false;
    if (opt_.plant_r1 && !planted_r1_ && be.slots_used() > 0u && st.action == "step" && !qanet::question_up(before)) {
      planted_r1_ = planted_now = true;  // self-test fault: the document changes with nothing recorded, in the first input after a slot holds audio
      after = before;
      after.digest += "+planted";
      after.pattern_hash += 1;  // (a change of a class the undo policy covers)
    }
    for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
      if (!be.used[slot] && ae.used[slot] && be.project == ae.project) {
        ++stats_.slot_fills;
        if (stats_.first_fill_step < 0) {
          stats_.first_fill_step = static_cast<int>(step_);
        }
      }
    }
    // The project's session state (JUMP, note entry, mute modes, the mixer, Perform ...) is in the native file the digest is taken of, and none of it is an undo unit by design (the allow-list of the header, entry 1): an
    // input that moved it is not judged, and is counted in the coverage line. (A1's walk reports the Mixer lines on its own.)
    if (st.action == "step" && be.session != ae.session) {
      ++stats_.units_session;
      ++stats_.session_by_screen[before.screen];
      return;
    }
    ++stats_.judged_by_screen[before.screen];
    bool overwrite = false;
    for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
      overwrite = overwrite || (be.used[slot] && ae.used[slot] && be.version[slot] != ae.version[slot] && before.screen == "SAMPLE_LOAD" && st.action == "step");
    }
    // Rules 2 and 3 (B4b): the input as the binding chain read it, the facts, the control and the images; kept alive here for the rules of the table.
    qanet::Snap key_after;
    qanet::SelImage img_after;
    std::string planted23;
    if (opt_.rules_every != 0u) {
      prepare_rules23(before, after, st, key_after, img_after, planted23);
    }
    if (stats_.first_change_step < 0 && native_part(before.digest) != native_part(after.digest)) {
      stats_.first_change_step = static_cast<int>(step_);  // (after the rules of this input: a plant triggers on a LATER input)
    }
    bool flagged = false;
    for (const StepRule rule : kStepRules) {
      for (const qanet::Finding& f : rule(before, after, st)) {
        ++stats_.rule_findings;
        flagged = flagged || f.rule == "R1";
        if (f.rule == "R2" || f.rule == "R3") {  // not a change of the project the scenario grammar can assert: the finding is its inputs (the action the chain gave the input is in the reason)
          const std::string why = f.reason + " (" + st.act + (st.arg != 0 ? " " + std::to_string(st.arg) : std::string()) + ")" + (f.rule == "R2" && st.drifting ? " while the transport plays" : "");
          // The baseline keys these by the action the chain gave the input (a key is bound to different actions in different states: `tap f2` is file.rescan on File and transport.play on Song), with Shift in front.
          const std::string key = (st.input.compare(0, 6, "shift+") == 0 ? "shift+" : "") + st.act + (f.rule == "R2" ? " " + std::to_string(st.arg) : std::string());
          record(f.rule, why + planted23 + (f.rule == "R3" && !sel_detail_.empty() ? " [" + sel_detail_ + "]" : std::string()), std::string(), key, f.rule + "|" + field(state_line(), "screen") + "|" + key + "|" + why);
          continue;
        }
        // As a scenario a silent change of the project ends with an undo and the assertion that the document is where a mark before the unit left it (the grammar has no undo depth to assert; it does not see the
        // samples either, so a change of the samples alone has no assertion). It holds if the unit is on the ring of the screen it ended on.
        // (`edit.undo` is disabled while a chooser or a dialog holds the focus: a finding that ended in one has no assertion.)
        const bool says = f.rule == "R1" && f.reason.find("document changed with no question") == 0 && native_part(before.digest) != native_part(after.digest) && (after.overlay == "NONE" || after.overlay == "NOTIFY");
        const std::string label = "r" + std::to_string(units_);
        const std::string screen = field(state_line(), "screen");
        record(f.rule, f.reason + (planted_now ? " (planted)" : ""), says ? "action edit.undo\nwait 1\n" + undo_tail(label) : std::string(), st.input, f.rule + "|" + screen + "|" + st.input + "|" + f.reason, says ? label : std::string());
      }
    }
    if (overwrite) {
      ++stats_.slot_overwrites;
      stats_.slot_overwrites_flagged += flagged ? 1u : 0u;
      stats_.slot_overwrites_same += !flagged && before.digest == after.digest ? 1u : 0u;
      if (opt_.overwrite_report && !flagged && before.digest != after.digest) {
        overwrite_line(before, after, st, be, ae);
      }
    }
  }

  // The rules 2 and 3 inputs of one judged unit.
  void prepare_rules23(const qanet::Snap& before, const qanet::Snap& after, Step& st, qanet::Snap& key_after, qanet::SelImage& img_after, std::string& planted) {
    const Resolved rs = resolve_input(chain_, history_, unit_from_);
    st.act = rs.action;
    st.arg = rs.arg;
    if (rs.ok && rs.tapped && (rs.fkey || rs.arrow)) {  // rule 2: a bound F key or arrow (the keys of A2's walk)
      ++stats_.keys_seen;
      ++stats_.key_by_action[(rs.shift ? "shift+" : "") + rs.action];
      if ((rs.arrow && (!rs.shift || rs.fallback)) || rs.sibling || rs.action == "ui.softkey") {
        ++stats_.keys_excused;  // A2's excuses that the chain tells: an arrow that may be at the edge of what it moves, a key that selects what is already selected, the placeholder soft key
      } else if (!quiet_before_) {
        ++stats_.keys_unquiet;  // something moved by itself in the wait before the key: the state before is no reference
      } else {
        st.key_judged = true;
        ++stats_.keys_judged;
        st.facts.bound = true;
        st.facts.fkey = rs.fkey;
        st.facts.labelled = false;
        st.facts.row_known = false;  // the key row is the picture of A2's walk; here only the silence is judged
        const auto playing = [&](const char* k) { const auto it = before.fields.find(k); return it != before.fields.end() && it->second == "1"; };
        st.drifting = playing("seq") || playing("playing");
        if (opt_.plant_r2 && !planted_r2_ && stats_.first_change_step >= 0) {
          planted_r2_ = true;  // self-test fault: the first key after the document has changed (a state of the walk's own making) changes nothing and says nothing
          key_after = before;
          st.key_after = &key_after;
          planted = " (planted)";
        }
        const bool silent = !qanet::check_key(st.facts, before, st.key_after != nullptr ? *st.key_after : after, st.drifting).empty();
        stats_.keys_silent += silent ? 1u : 0u;
        if (rs.action == "select.extend" && st.drifting && before.screen == "PATTERN") {  // (Shift + Up and Down are cursor.page: a binding of their own, not the selection)
          ++stats_.watchsel_seen;
          stats_.watchsel_silent += silent ? 1u : 0u;
          stats_.watchsel_watch += sim_->app().state().mode[app::screen_index(app::ScreenId::Pattern)] == static_cast<std::uint8_t>(app::PatternMode::Browse) ? 1u : 0u;
        }
      }
    }
    if (img_ok_ && img_before_.selected && img_before_.screen == app::ScreenId::Pattern) {
      ++stats_.sel_units;
      if (stats_.first_sel_step < 0) {
        stats_.first_sel_step = static_cast<int>(step_);
      }
    }
    if (img_ok_ && !rs.action.empty()) {  // rule 3: the action is in the table and applies in this state
      std::string act = rs.action;
      const bool plant = opt_.plant_r3 && !planted_r3_ && img_before_.selected && img_before_.screen == app::ScreenId::Pattern;
      if (plant) {
        act = "block.copy";  // self-test fault: the first unit made with a pattern selection up is judged as a copy (it has no span) that also edited a step
      }
      const qanet::SelOp* op = qanet::find_selection_op(act);
      st.sel_ctx.clip = have_clip_ ? &clip_ : nullptr;
      st.sel_ctx.copied_instrument = copied_instrument_;
      st.sel_ctx.must_act = false;
      sel_detail_.clear();
      st.sel_ctx.detail = &sel_detail_;  // where the finding says what exactly differed: printed with it, never part of the baseline's key
      // LIMIT of rule 3 (selection_ops.cpp): the table was written for patterns that exist. The first write into a pattern of the bank that was never used makes the pattern (an edit in it, a fill), which where_differs()
      // reads as a pattern made outside the selection, and its track lengths as 0 before. The walk does not judge an input made in a pattern that does not exist yet (counted, `sel_no_pattern`).
      const bool pattern_missing = img_before_.screen == app::ScreenId::Pattern && img_before_.doc != nullptr && img_before_.doc->pattern(img_before_.pattern) == nullptr;
      stats_.sel_no_pattern += op != nullptr && pattern_missing ? 1u : 0u;
      if (op != nullptr && !pattern_missing && img_before_.screen == op->screen && op->applies(img_before_, st.sel_ctx) && qanet::image_of(*sim_, img_after) && img_after.ok) {
        if (plant) {
          planted_r3_ = true;
          auto doc = std::make_shared<model::Project>(*img_after.doc);
          model::Step step = doc->pattern(img_after.pattern) != nullptr ? doc->pattern(img_after.pattern)->steps[0][0] : model::kEmptyStep;
          step.note = static_cast<std::uint8_t>(step.note == 70 ? 71 : 70);
          (void)doc->set_step(img_after.pattern, 0, 0, step);
          img_after.doc = doc;
          img_after.digest = sim::model_digest(*doc);
          planted = " (planted)";
        }
        st.act = act;
        st.img_before = &img_before_;
        st.img_after = &img_after;
        ++stats_.sel_judged;
        ++stats_.sel_by_action[act];
      }
      // What the walk knows that the images do not hold: the clipboards.
      if (rs.action == "block.copy" && img_before_.screen == app::ScreenId::Pattern && img_before_.doc != nullptr && img_before_.doc->pattern(img_before_.pattern) != nullptr) {
        clip_.rows = img_before_.row1 - img_before_.row0 + 1;
        clip_.tracks = img_before_.track1 - img_before_.track0 + 1;
        clip_.steps.clear();
        have_clip_ = clip_.rows > 0 && clip_.tracks > 0;
        for (int t = img_before_.track0; t <= img_before_.track1 && have_clip_; ++t) {
          for (int r = img_before_.row0; r <= img_before_.row1; ++r) {
            clip_.steps.push_back(t < static_cast<int>(model::kMaxTracks) && r < static_cast<int>(model::kMaxPatternLength) ? img_before_.doc->pattern(img_before_.pattern)->steps[t][r] : model::kEmptyStep);
          }
        }
      }
      if (rs.action == "instrument.copy" && img_before_.screen == app::ScreenId::Instrument) {
        copied_instrument_ = img_before_.slot;
      } else if (rs.action != "instrument.paste" && native_part(before.digest) != native_part(after.digest)) {
        copied_instrument_ = -1;  // the clipboard holds the instrument as it was when Copy took it: a later edit of the document may have changed the source slot, and the slot is then no longer what the paste should give
      }
    }
  }

  // Measurement: why a WAV load over a used slot that changed the slot passed the rules. The credit that let it pass is named: the answer key of a question, a unit on a ring (which), or none of them.
  void overwrite_line(const qanet::Snap& before, const qanet::Snap& after, const Step& st, const Extra& be, const Extra& ae) const {
    std::string rings;
    for (unsigned i = 0; i < qanet::kRings; ++i) {
      rings += " ring" + std::to_string(i) + "=" + std::to_string(before.ring[i]) + "/" + std::to_string(before.added[i]) + ">" + std::to_string(after.ring[i]) + "/" + std::to_string(after.added[i]);
    }
    const char* credit = st.answer && qanet::question_up(before) ? "answer key" : (after.sedit > before.sedit ? "sample-editor unit" : "none");
    std::string moved;
    for (unsigned i = 0; i < qanet::kRings; ++i) {
      moved += after.added[i] > before.added[i] ? " ring" + std::to_string(i) : std::string();
    }
    unsigned slots = 0;
    for (unsigned slot = 0; slot < app::SampleBank::kSlots; ++slot) {
      slots += be.version[slot] != ae.version[slot] ? 1u : 0u;
    }
    std::printf("overwrite %llu@%u: input \"%s\" screen %s overlay %s question %s keys \"%s\" action %s; credit %s; ring units added on:%s; sedit %u>%u; native %s, samples %s; %u slot versions moved;%s msg %s\n",
                static_cast<unsigned long long>(seed_), step_, st.input.c_str(), before.screen.c_str(), before.overlay.c_str(), qanet::question_up(before) ? "up" : "no", before.keys.c_str(), st.action.c_str(), credit,
                moved.empty() ? " none" : moved.c_str(), before.sedit, after.sedit, native_part(before.digest) != native_part(after.digest) ? "changed" : "same",
                before.digest.substr(before.digest.find('/') == std::string::npos ? 0 : before.digest.find('/')) != after.digest.substr(after.digest.find('/') == std::string::npos ? 0 : after.digest.find('/')) ? "changed" : "same", slots, rings.c_str(),
                after.msg.c_str());
  }

  // Rule 2's control (A2: a control run that waits as long as the key does and presses nothing). In the walk a key is pressed in the state the previous input left, so the walk waits before a key as long as the
  // key's unit takes (the tap and the quiet, 100 ms) and looks whether anything moved by itself (the document, the view, the selection, the sound, the picture; the playhead's fields when the transport plays).
  // If nothing did, the state before the key is what the state would be without it; if something did, the key is not judged. The wait is a recorded input like any other, a pause of the person before the key,
  // and every key of the walk has it whichever inputs are judged (the walk is the same for every n); the snapshots are taken only for a judged unit.
  void pre_wait() {
    const bool judged = (units_ + 1u) % opt_.rules_every == 0u;
    if (!judged) {
      wait(100);
      return;
    }
    const qanet::Snap a = qanet::snap_of(*sim_);
    const auto it = a.fields.find("seq");
    const auto pl = a.fields.find("playing");
    const bool drifting = (it != a.fields.end() && it->second == "1") || (pl != a.fields.end() && pl->second == "1");
    wait(100);
    cur_ = qanet::snap_of(*sim_);
    cur_extra_ = extra_of(*sim_);
    snap_valid_ = true;  // (wait() cleared it: this snapshot is the state now, and the unit that follows takes it as its before)
    quiet_before_ = !qanet::differs(a, cur_, drifting);
  }

  // One input of kind `k`, as a unit for the rules (B4).
  void plain(Kind k, bool in_burst) {
    const bool unit = k != Kind::Card && k != Kind::Burst && k != Kind::Wait;
    quiet_before_ = true;
    if (unit && !in_burst && opt_.rules_every != 0u && (k == Kind::Fkey || k == Kind::ShiftFkey || k == Kind::Edit)) {
      pre_wait();
    }
    if (unit) {
      unit_begin();
    }
    plain_input(k, in_burst);
    if (unit) {
      unit_end(in_burst);
    }
  }

  // One input of kind `k`. In a burst File is not pressed: NEW and OPEN replace the document and its journal, and a burst compares the digests of one document.
  void plain_input(Kind k, bool in_burst) {
    switch (k) {
      case Kind::Screen: {
        const app::Button b = pick_screen_button(rng_);
        tap(in_burst && b == app::Button::File ? app::Button::Pattern : b);
        if (!in_burst) {
          wait(10 + rng_.below(50));  // a person looks at the new screen before the next key: the listing of a folder takes about a tick an entry
        }
        break;
      }
      case Kind::Fkey: tap(static_cast<app::Button>(rng_.below(in_burst ? 7u : 8u))); break;  // (not F8 in a burst: it is undo on the Pattern screen, and the burst counts its own undos)
      case Kind::ShiftFkey:
        rec("press shift");
        sim_->press(app::Button::Shift);
        tick();
        shift_held_ = true;
        if (!in_burst && rng_.below(4) == 0) {  // Shift + an arrow: the selection of the pattern (OS-009), the Song range
          tap(kEditButtons[rng_.below(4)]);
        } else {
          tap(static_cast<app::Button>(rng_.below(in_burst ? 7u : 8u)));
        }
        shift_held_ = false;
        input_ = "shift+" + input_.substr(4);
        rec("release shift");
        sim_->release(app::Button::Shift);
        tick();
        break;
      case Kind::Edit: tap(kEditButtons[rng_.below(sizeof kEditButtons / sizeof kEditButtons[0])]); break;
      case Kind::Encoder: {
        const int d = static_cast<int>(rng_.below(5)) - 2;
        input_ = "encoder";
        rec("encoder " + std::to_string(d == 0 ? 1 : d));
        sim_->encoder(d == 0 ? 1 : d);
        tick();
        break;
      }
      case Kind::Pad:
        input_ = "pad";
        {
          const unsigned pad = rng_.below(48);
          rec("pad " + std::to_string(pad));
          sim_->pad(pad);
        }
        tick(2);
        break;
      case Kind::Wait: input_ = "wait"; wait(1 + rng_.below(in_burst ? 20u : (rng_.below(4) == 0 ? 300u : 30u))); break;
      case Kind::Card:
      case Kind::Burst: break;  // not inside a burst (and outside one they are the callers')
    }
    if (in_burst) {
      burst_min_depth_ = std::min(burst_min_depth_, sim_->app().model().undo_depth);
      burst_left_pattern_ = burst_left_pattern_ || sim_->app().state().screen != app::ScreenId::Pattern;
    }
  }

  // Self-test (plant_r3): a selection made mid-walk, by real inputs (Shift + Right on Pattern), recorded like any input of the walk so that a shrunk script has it.
  void inject_select() {
    unit_begin();
    rec("press shift");
    sim_->press(app::Button::Shift);
    tick();
    shift_held_ = true;
    tap(app::Button::Right);
    shift_held_ = false;
    input_ = "shift+right";
    rec("release shift");
    sim_->release(app::Button::Shift);
    tick();
    unit_end(false);
  }

  void do_step() {
    if (opt_.plant_r3 && !select_planted_ && opt_.rules_every != 0u && step_ >= 8u && sim_->app().state().screen == app::ScreenId::Pattern && sim_->app().state().overlay == app::OverlayId::None) {
      select_planted_ = true;
      inject_select();
    }
    Kind kind;
    if (session_ > 0) {
      --session_;
      kind = pick_kind(rng_, kSessionKinds, sizeof kSessionKinds / sizeof kSessionKinds[0]);
    } else {
      kind = pick_kind(rng_);
      session_ = kind == Kind::Screen ? 1 + rng_.below(5) : 0u;
    }
    const std::string from = field(state_line(), "screen");
    if (!sim_->card_storage()->card_present() && rng_.below(3) == 0) {  // a pulled card does not stay out for ever: this step puts it back
      kind = Kind::Card;
    }
    if (kind == Kind::Card) {
      card_step();
    } else if (kind == Kind::Burst) {
      burst();
      burst_ = false;
      input_ = "burst";
    } else {
      plain(kind, false);
    }
    stats_.tuples.insert(from + " | " + input_ + " | " + field(state_line(), "screen"));
  }

  // ---- B5: the metamorphic relations ----
  // An operation and its inverse (or a save and the OPEN of what it saved) give the document back: the digest of the project (what formats/native writes, so the session state with it) after is the digest
  // before. The inputs are actions and keys of the walk's own grammar, with a `mark model` before and `assert model digest=@<label>` as the finding's tail, so a finding is a script `o3sim` runs (no shrinking:
  // like the undo burst, the relation compares two instants). A state that cannot host a relation (no note under the cursor, no sample, a refused save, an OPEN that did not end) is counted as skipped, never
  // as a finding. The relations run on their own random stream, after the walk's last step (Options::meta_count): the walk is the same with or without --meta.

  struct MetaMark {
    std::string label;
    std::string digest;
    std::string session;
    unsigned depth = 0;  // the journal's depth at the mark
  };

  // The action `name` as the scenario line `action <name> [<arg>]`; false (and nothing recorded) when it was not enabled.
  bool act(const char* name, bool with_arg = false, int arg = 0) {
    const app::ActionDef* const d = app::find_action(app::action_registry(), name, std::strlen(name));
    if (d == nullptr) {
      return false;
    }
    rec(std::string("action ") + name + (with_arg ? " " + std::to_string(arg) : std::string()));
    const app::DispatchResult r = sim_->action(d->id, arg);
    tick();
    if (r == app::DispatchResult::Disabled || r == app::DispatchResult::Unknown) {
      history_.pop_back();  // not an input that took: `o3sim` would refuse the line
      return false;
    }
    return true;
  }

  // The units of the three journals (Pattern, Song, Sound: the info line's "undo n" is the Pattern's alone, and a Wave edit is in the Sound's).
  unsigned journal_depth() const {
    const model::Editor* const ed = sim_->app().session().document();
    if (ed == nullptr) {
      return 0u;
    }
    return ed->journal(model::UndoArea::Pattern).depth() + ed->journal(model::UndoArea::Song).depth() + ed->journal(model::UndoArea::Sound).depth();
  }

  bool meta_mark(MetaMark& m) {
    wait(1);
    const model::Project* const p = sim_->project();
    if (p == nullptr) {
      return false;
    }
    m.digest = sim::model_digest(*p);
    if (m.digest.empty()) {
      return false;
    }
    m.session = session_signature(*p);
    m.depth = journal_depth();
    m.label = "m" + std::to_string(++meta_marks_);  // unique in a script: one mark a relation
    rec("mark model " + m.label);
    return true;
  }

  // The end of a relation. `skip`: it could not be set up or did not run through. `session_exempt`: a relation whose inputs moved the project's session state (a setting with no undo unit) proves nothing about
  // its own inputs (the undo burst's allow-list entry 1); the save and OPEN keeps no such exemption: the session state is in the file.
  void meta_end(unsigned rel, const MetaMark& m, const std::string& what, bool skip, bool session_exempt) {
    wait(1);
    const model::Project* const p = sim_->project();
    if (skip || p == nullptr || (session_exempt && session_signature(*p) != m.session)) {
      ++stats_.meta_skip[rel];
      return;
    }
    const std::string after = sim::model_digest(*p);
    ++stats_.meta_ok[rel];
    if (after != m.digest) {
      record(std::string("meta_") + kMetaNames[rel], what + ": the document's digest is " + after.substr(0, 12) + ", before it was " + m.digest.substr(0, 12), undo_tail(m.label), std::string("meta ") + kMetaNames[rel]);
    }
  }

  // Undo as many units as the journal grew since the mark; false when an undo was refused or the journal is not back at the mark's depth.
  bool meta_undo_to(const MetaMark& m) {
    wait(1);
    const unsigned depth1 = journal_depth();
    for (unsigned i = m.depth; i < depth1; ++i) {
      if (!act("edit.undo")) {
        return false;
      }
    }
    wait(1);
    return journal_depth() == m.depth;
  }

  // A dialog or chooser the walk left up is closed (F8 is BACK, as in the audio probe's settle): the relation's own keys must reach the screen.
  void meta_settle() {
    for (unsigned i = 0; i < 4u; ++i) {
      const std::string overlay = field(state_line(), "overlay");
      if (overlay == "NONE" || overlay == "NOTIFY") {
        break;
      }
      tap(app::Button::F8);
      wait(50);
    }
  }

  // The Pattern screen in STEP (Edit, the cursor stays where it is, the arrows move it), nothing selected.
  bool meta_edit_mode() {
    meta_settle();
    tap(app::Button::Pattern);
    for (unsigned i = 0; i < 2u && !(app::pattern_editable(sim_->app().state()) && !app::pattern_cursor_follows(sim_->app().state())); ++i) {
      tap(app::Button::Rec);
    }
    const app::ViewState& v = sim_->app().state();
    if (!app::pattern_editable(v) || app::pattern_cursor_follows(v)) {
      return false;
    }
    (void)act("select.clear");
    return !sim_->app().state().edit.selection.active;
  }

  // The step under the cursor; false when the pattern in view does not exist.
  bool meta_cursor_step(model::Step& out) const {
    const model::Project* const p = sim_->project();
    const model::Pattern* const pat = p != nullptr ? p->pattern(sim_->app().state().edit.pattern) : nullptr;
    const app::PatternView& cur = sim_->app().view();
    const int row = cur.row() < 0 ? 0 : cur.row();
    const int track = cur.track() < 0 ? 0 : cur.track();
    if (pat == nullptr || track >= static_cast<int>(model::kMaxTracks) || row >= static_cast<int>(model::kMaxPatternLength)) {
      return false;
    }
    out = pat->steps[track][row];
    return true;
  }

  // STEP mode with a regular note under the cursor (an empty step gets the default one: the rule of a single cell, model/src/step_edit.cpp). `note` is the OG note 0..127.
  bool meta_note(int& note) {
    if (!meta_edit_mode()) {
      return false;
    }
    model::Step s;
    if (!meta_cursor_step(s)) {
      return false;
    }
    if (s.note == model::kNoteEmpty) {
      if (!act("step.note_delta", true, 1) || !meta_cursor_step(s)) {
        return false;
      }
    }
    if (s.note < model::kNoteFirst || s.note > model::kNoteLast) {
      return false;  // a fade, a cut, an off or a reserved byte: no regular note to move
    }
    note = static_cast<int>(s.note) - 1;
    return true;
  }

  void meta_step() {
    unsigned rel = meta_n_++ % kMetaCount;
    if (opt_.meta_only >= 0) {
      rel = static_cast<unsigned>(opt_.meta_only);
    }
    input_ = std::string("meta ") + kMetaNames[rel];
    switch (rel) {
      case 0: meta_save_open(); break;
      case 1: meta_shift(1, false); break;
      case 2: meta_shift(2, true); break;
      case 3: meta_copy_paste(); break;
      default: meta_slice(); break;
    }
  }

  bool any_fine_tune() const {
    const model::Project* const p = sim_->project();
    for (unsigned i = 0; p != nullptr && i < model::kMaxInstruments; ++i) {
      const model::Instrument* const in = p->instrument(i);
      if (in != nullptr && in->sampler.fine_tune != 0) {
        return true;
      }
    }
    return false;
  }

  // A document that has a sampler field off its default before the save (the fine tune of an instrument), so that a Save that does not write it gives another value back: a NEW project holds none, and the
  // relation would pass a writer that drops the field (mutant M19). The instrument is the first there is (the WAV of the card's root into instrument 1 when none holds audio or a sound), the field is
  // reached on the Params screen the way a person does it: the VOICE page (F2), the cursor to the top, three rows down (FINE TUNE), the encoder. Not reached: no extra input is left half done, the relation goes on as it was.
  void meta_plant_tune() {
    if (sim_->project() == nullptr || any_fine_tune()) {
      return;
    }
    unsigned first = model::kMaxInstruments;
    for (unsigned i = 0; i < model::kMaxInstruments && first == model::kMaxInstruments; ++i) {
      first = sim_->project()->instrument(i) != nullptr ? i : first;
    }
    if (first == model::kMaxInstruments) {
      if (!meta_load_sample()) {
        return;
      }
      first = 0;
    }
    meta_settle();
    if (!act("instrument.select", true, static_cast<int>(first) + 1)) {
      return;
    }
    tap(app::Button::Params);
    if (sim_->app().state().screen != app::ScreenId::Instrument) {
      tap(app::Button::Params);
    }
    if (sim_->app().state().screen != app::ScreenId::Instrument) {
      return;
    }
    tap(app::Button::F2);  // the VOICE page: VOLUME, PANNING, TUNE, FINE TUNE (row 3), GLIDE, VEL>
    for (unsigned i = 0; i < 12u; ++i) {
      tap(app::Button::Up);
    }
    for (unsigned i = 0; i < 3u; ++i) {
      tap(app::Button::Down);
    }
    rec("encoder 3");
    sim_->encoder(3);
    tick();
    wait(50);
  }

  // Save, then OPEN what was saved. The mark is taken before the save; with plant_meta one more edit follows the save (the document has a field the file does not), and the mark is taken after it.
  void meta_save_open() {
    constexpr unsigned rel = 0;
    MetaMark m;
    const auto msg = [&]() { return field(state_line(), "msg"); };
    const auto has = [](const std::string& s, const char* head, const char* tail) {
      return s.compare(0, std::strlen(head), head) == 0 && s.size() >= std::strlen(tail) && s.compare(s.size() - std::strlen(tail), std::string::npos, tail) == 0;
    };
    if (!sim_->card_storage()->card_present() || sim_->project() == nullptr) {
      ++stats_.meta_skip[rel];
      return;
    }
    meta_plant_tune();
    const bool tuned = any_fine_tune();
    stats_.meta_tuned += tuned ? 1u : 0u;
    if (!meta_mark(m) || !act("file.save")) {  // (from the screen the walk is on, with the document as the walk left it)
      ++stats_.meta_skip[rel];
      return;
    }
    std::string name;
    for (unsigned i = 0; i < 12u && name.empty(); ++i) {
      wait(100);
      const std::string s = msg();
      if (has(s, "SAVE_", "_DONE")) {
        name = s.substr(5, s.size() - 5 - 5);
      }
    }
    if (name.empty()) {
      ++stats_.meta_skip[rel];
      return;
    }
    if (opt_.plant_meta) {
      int note = 0;
      if (!meta_note(note) || !act("step.note_delta", true, note < 100 ? 1 : -1) || !meta_mark(m)) {
        ++stats_.meta_skip[rel];
        return;
      }
    }
    if (field(state_line(), "screen") != "FILE") {
      tap(app::Button::File);
    }
    // The PROJECTS page (the walk may have left File on its IMPORT or EXPORT page, whose listing is the card root or a sample folder): F6 goes EXPORT -> IMPORT -> PROJECTS.
    for (unsigned i = 0; i < 2u && field(state_line(), "screen") == "FILE" && (sim_->app().state().import.page || sim_->app().state().xport.page); ++i) {
      tap(app::Button::F6);
    }
    tap(app::Button::F2);  // a rescan: the project saved last is the first
    wait(500);
    const auto sel_is_saved = [&]() {
      std::string sel = field(state_line(), "sel");
      for (char& c : sel) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      return sel == name;
    };
    for (unsigned i = 0; i < 4u && !sel_is_saved(); ++i) {
      wait(200);
    }
    // The cursor is where the walk left it (a RECENT row, or an entry further down): from the top, Down to the saved folder (no input of the document; a bounded number of rows).
    if (!sel_is_saved() && field(state_line(), "screen") == "FILE") {
      for (unsigned i = 0; i < 24u; ++i) {
        tap(app::Button::Up);
      }
      for (unsigned i = 0; i < 24u && !sel_is_saved(); ++i) {
        tap(app::Button::Down);
      }
    }
    if (field(state_line(), "screen") != "FILE" || !sel_is_saved()) {
      ++stats_.meta_skip[rel];
      return;
    }
    tap(app::Button::F1);
    wait(300);
    bool again = false;
    bool opened = false;
    for (unsigned i = 0; i < 10u && !opened; ++i) {
      const std::string s = msg();
      if (!again && s.compare(0, 7, "UNSAVED") == 0) {
        again = true;
        tap(app::Button::F1);
        wait(300);
      } else if (has(s, "PROJECT_", "_OPENED")) {
        opened = true;
      } else {
        wait(200);
      }
    }
    const unsigned compared = stats_.meta_ok[rel];
    meta_end(rel, m, "a project saved as " + name + " and opened again", !opened, false);
    stats_.meta_tuned_ok += tuned && stats_.meta_ok[rel] > compared ? 1u : 0u;
  }

  // Transpose by `delta` and back by the opposite (step.note_delta; the octave variant is step.note_octave, whose argument is in octaves): the step is the one it was. Only judged where both moves stay
  // inside 0..127 (the clamp is not an inverse).
  void meta_shift(unsigned rel, bool octave) {
    int note = 0;
    if (!meta_note(note)) {
      ++stats_.meta_skip[rel];
      return;
    }
    const int unit = octave ? 12 : 1;
    int amount = octave ? 1 + static_cast<int>(meta_rng_.below(2)) : 1 + static_cast<int>(meta_rng_.below(12));
    if (meta_rng_.below(2) == 0u) {
      amount = -amount;
    }
    if (note + amount * unit < 0 || note + amount * unit > 127) {
      amount = -amount;
    }
    MetaMark m;
    model::Step s;
    const char* const name = octave ? "step.note_octave" : "step.note_delta";
    bool skip = note + amount * unit < 0 || note + amount * unit > 127 || !meta_mark(m);
    skip = skip || !act(name, true, amount) || !meta_cursor_step(s) || static_cast<int>(s.note) - 1 != note + amount * unit;  // (a first move that did not happen proves nothing)
    skip = skip || !act(name, true, -amount);
    meta_end(rel, m, std::string(name) + " " + std::to_string(amount) + " and " + std::to_string(-amount), skip, true);
  }

  // Copy the step under the cursor, move down, paste, undo the paste: the document is the one before the paste.
  void meta_copy_paste() {
    constexpr unsigned rel = 3;
    int note = 0;
    MetaMark m;
    if (!meta_note(note) || !act("block.copy")) {
      ++stats_.meta_skip[rel];
      return;
    }
    tap(app::Button::Down);
    if (!meta_mark(m) || !act("block.paste")) {
      ++stats_.meta_skip[rel];
      return;
    }
    wait(1);
    const bool changed = journal_depth() > m.depth;  // a paste over the same step changes nothing and has no unit to undo
    meta_end(rel, m, "block.paste and edit.undo", !changed || !meta_undo_to(m), true);
  }

  // Sample load on the card's root (F2 goes up a folder), the first WAV under the cursor into instrument 1 (an instrument that is there asks REPLACE first: the second F1 answers). true: the slot holds audio.
  bool meta_load_sample(unsigned inst = 0) {
    if (!sim_->card_storage()->card_present() || sim_->project() == nullptr) {
      return false;
    }
    tap(app::Button::Pattern);  // (the audio probe's way in: Sample load from the Pattern screen, and not a second press of it)
    if (!act("instrument.select", true, static_cast<int>(inst) + 1)) {
      return false;
    }
    tap(app::Button::SampleLoad);
    wait(400);
    for (unsigned i = 0; i < 6u; ++i) {
      tap(app::Button::F2);
      wait(300);
    }
    wait(1500);
    const auto wav = [&]() {
      const std::string sel = field(state_line(), "sel");
      return sel.size() > 4u && sel.compare(sel.size() - 4u, 4u, ".wav") == 0;
    };
    for (unsigned i = 0; i < 60u && !wav(); ++i) {
      tap(app::Button::Down);
    }
    if (!wav()) {
      return false;
    }
    tap(app::Button::F1);
    if (sim_->project()->instrument(inst) != nullptr) {
      wait(50);
      tap(app::Button::F1);
    }
    for (unsigned i = 0; i < 40u; ++i) {
      wait(100);
      if (extra_of(*sim_).used[inst]) {
        return true;
      }
    }
    return false;
  }

  // Add a slice to a sample instrument on the chop page of Wave, undo it.
  void meta_slice() {
    constexpr unsigned rel = 4;
    meta_settle();
    const Extra e = extra_of(*sim_);
    unsigned first = app::SampleBank::kSlots;
    for (unsigned i = 0; i < app::SampleBank::kSlots; ++i) {
      if (e.used[i]) {
        first = i;
        break;
      }
    }
    if (first == app::SampleBank::kSlots && !meta_load_sample()) {  // (no instrument holds audio yet: a WAV of the card's root goes into instrument 1, the way a person does it)
      ++stats_.meta_skip[rel];
      return;
    }
    if (first == app::SampleBank::kSlots) {
      first = 0;
    }
    if (!act("instrument.select", true, static_cast<int>(first) + 1)) {
      ++stats_.meta_skip[rel];
      return;
    }
    tap(app::Button::Pattern);
    tap(app::Button::SamplePlay);
    tap(app::Button::SamplePlay);
    wait(50);
    MetaMark m;
    if (field(state_line(), "screen") != "WAVE" || !meta_mark(m) || !act("wave.add")) {
      ++stats_.meta_skip[rel];
      return;
    }
    wait(1);
    const bool changed = journal_depth() > m.depth;
    meta_end(rel, m, "wave.add and edit.undo", !changed || !meta_undo_to(m), true);
  }

  // ---- B8: the limit fixtures ----
  // A fixture drives a limit of the OS (the undo ring's bytes, the sample editor's kHistory steps) with the walk's own inputs and judges what happens there: rule 1 at every step, and the undo that follows. They run after
  // the walk and --meta, once, on the seeds exhaust_step() names; a fixture that could not reach its limit is counted as skipped (--min-exhaust fails the run when too few did).

  // The walk may end with the card pulled (card_step): a fixture that loads a sample puts it back, as a person would.
  void exh_card() {
    // A text entry the walk left up (a Save As name) closes with F7 (ui.back): meta_settle's F8 would accept the name.
    for (unsigned i = 0; i < 2u && field(state_line(), "overlay") == "TEXT_ENTRY"; ++i) {
      tap(app::Button::F7);
      wait(50);
    }
    std::string error;
    if (sim_->card_storage() != nullptr && !sim_->card_storage()->card_present()) {
      rec("card insert");
      if (sim_->card_insert(error)) {
        tick(2 * 16 + 1);
      } else {
        history_.pop_back();
      }
    }
  }

  // A fixture that could not reach its limit says why (the coverage line prints it): false.
  bool exh_no(const std::string& why) {
    stats_.exh_notes += std::string(" ") + std::to_string(seed_) + ": " + exh_name_ + " not reached, " + why + ";";
    return false;
  }

  void exh_fail(unsigned fx, const std::string& what, const std::string& tag) {
    const std::string inv = std::string("exhaust_") + kExhaustNames[fx];
    record(inv, what, std::string(), std::string("exhaust ") + kExhaustNames[fx], inv + "|" + tag);
  }

  // Rule 1 over one step of a fixture. `plant` (with --plant-exhaust, the self-test): the step's unit goes unrecorded. At the undo cap the ring's count does not move, in the sample-edit cap the step also changes the
  // pattern (as if the edit had a side effect that no unit covers): a full sample-edit history used to credit any change, so the second plant is what B8 added.
  void exh_rule1(unsigned fx, const qanet::Snap& before, qanet::Snap after, bool plant, const std::string& where = std::string()) {
    const bool planted = plant && opt_.plant_exhaust;
    if (planted) {
      after.digest += "+planted";
      if (fx == 0) {
        after.ring[0] = before.ring[0];
        after.added[0] = before.added[0];
      } else {
        after.pattern_hash += 1;
      }
    }
    for (const qanet::Finding& f : qanet::check_rules(before, after, "step", false)) {
      exh_fail(fx, f.rule + ": " + f.reason + (planted ? " (planted)" : "") + where, f.rule + (planted ? "|planted" : ""));
    }
  }

  void exhaust_step() {
    const unsigned s = static_cast<unsigned>(seed_ % 16u);
    for (unsigned fx = 0; fx < kExhaustCount; ++fx) {
      const bool mine = fx == 0u ? (s == 3u || s == 5u || s == 7u) : fx <= 2u ? (s == 4u || s == 6u) : fx == 3u ? (s == 8u || s == 9u) : (s == 10u || s == 11u);  // (the switch runs with the sample-edit cap)
      // --exhaust-only names one (any seed); --exhaust-all is the walk's three (the short run); the voice pool and the folder of 2000 run on the seeds they are assigned to in a long run by hand, or through --exhaust-only.
      if (opt_.exhaust_only >= 0 ? opt_.exhaust_only != static_cast<int>(fx) : !(opt_.exhaust_all ? fx < kFixtureFirstFresh : mine)) {
        continue;
      }
      input_ = std::string("exhaust ") + kExhaustNames[fx];
      exh_name_ = kExhaustNames[fx];
      exh_card();
      const bool reached = fx == 0u ? exh_undo_cap() : fx == 1u ? exh_sedit_cap() : fx == 2u ? exh_sedit_switch() : fx == 3u ? exh_voice_pool() : exh_big_folder();
      if (reached) {
        ++stats_.exh_ok[fx];
      } else {
        ++stats_.exh_skip[fx];
      }
      rest();
      check();
      mix_state();
    }
  }

  // The undo cap. A note in each of the 12 tracks at the cursor's row, the 12 tracks selected, step.insert until the Pattern ring has dropped units and holds only units of the fixture: the insert shifts 128 steps of every
  // selected track, so a unit is about 12 KiB and the ring (160 KiB) fills in a few dozen. Then: undo takes exactly the units the ring kept (the oldest were dropped whole), to the document that stood before the oldest
  // kept unit; one more undo changes nothing; redo gives the last document back.
  bool exh_undo_cap() {
    constexpr unsigned fx = 0;
    constexpr unsigned kTracks = 12;
    constexpr unsigned kBound = 240;  // inserts
    if (sim_->project() == nullptr || sim_->app().session().document() == nullptr || sim_->project()->active_tracks() < kTracks) {
      return exh_no("no project, or fewer than 12 active tracks");
    }
    if (!meta_edit_mode()) {
      return exh_no("the Pattern screen is not in STEP mode (" + field(state_line(), "screen") + "/" + field(state_line(), "overlay") + "/" + field(state_line(), "mode") + ")");
    }
    (void)act("cursor.home");
    for (unsigned t = 0; t < kTracks; ++t) {
      model::Step st;
      if (!meta_cursor_step(st) || (st.note == model::kNoteEmpty && !act("step.note_delta", true, 1)) || (t + 1u < kTracks && !act("cursor.track", true, 1))) {
        return exh_no("a note could not be entered, or the cursor did not move to the next track");
      }
    }
    if (sim_->app().view().track() != static_cast<int>(kTracks) - 1) {
      return exh_no("the cursor did not reach track 12");
    }
    (void)act("cursor.home");
    // OS-AUDIT-56: Shift + Right grows the block a column at a time inside the anchor track and steps by tracks once it has crossed into the next: press until the end reaches track 12 (bounded).
    const auto& sel = sim_->app().state().edit.selection;
    for (unsigned i = 0; i < 6u * kTracks && !(sel.active && static_cast<int>(sel.end_track) >= static_cast<int>(kTracks) - 1); ++i) {
      if (!act("select.extend", true, 10)) {
        return exh_no("select.extend refused");
      }
    }
    if (!sel.active || static_cast<int>(sel.end_track) - static_cast<int>(sel.start_track) != static_cast<int>(kTracks) - 1) {
      return exh_no("the selection does not cover 12 tracks");
    }
    const model::UndoJournal& j = sim_->app().session().document()->journal(model::UndoArea::Pattern);
    const auto added = [&]() { return j.depth() + j.dropped_units(); };  // units made since begin()
    const auto digest = [&]() { return sim_->project() != nullptr ? sim::model_digest(*sim_->project()) : std::string(); };
    const std::uint32_t dropped0 = j.dropped_units();
    wait(1);
    const unsigned base = added();
    std::map<unsigned, std::string> trail;  // the document after the unit numbered k
    trail[base] = digest();
    bool reached = false;
    for (unsigned n = 0; n < kBound && !reached; ++n) {
      const qanet::Snap before = qanet::snap_of(*sim_);
      if (!act("step.insert")) {
        return exh_no("step.insert refused");
      }
      wait(1);
      const qanet::Snap after = qanet::snap_of(*sim_, &before);
      exh_rule1(fx, before, after, false);
      trail[added()] = digest();
      ++stats_.exh_units;
      reached = j.dropped_units() > dropped0 && j.depth() <= added() - base;  // units dropped, and none of what the ring holds is older than the fixture
    }
    if (!reached) {
      return exh_no("the ring dropped no unit within the bound of inserts");
    }
    {  // one more, at the cap: the unit is recorded and the oldest goes (the planted defect of the self-test is here)
      const qanet::Snap before = qanet::snap_of(*sim_);
      if (!act("step.insert")) {
        return exh_no("step.insert refused at the cap");
      }
      wait(1);
      exh_rule1(fx, before, qanet::snap_of(*sim_, &before), true);
      trail[added()] = digest();
    }
    const unsigned kept = j.depth();
    const unsigned total = added();
    const std::string last = trail[total];
    const auto at = trail.find(total - kept);
    if (kept == 0u || total - kept < base || at == trail.end()) {
      exh_fail(fx, "the ring holds " + std::to_string(kept) + " units of the " + std::to_string(total - base) + " the fixture made: no document to compare the undo with", "kept");
      return true;
    }
    for (unsigned i = 0; i < kept; ++i) {
      if (!act("edit.undo")) {
        exh_fail(fx, "edit.undo refused with " + std::to_string(kept - i) + " of the ring's " + std::to_string(kept) + " units left", "undo_refused");
        return true;
      }
      wait(1);
    }
    wait(1);
    const std::string floor = digest();
    if (j.depth() != 0u || floor != at->second) {
      exh_fail(fx, "undoing the " + std::to_string(kept) + " units the ring kept leaves " + std::to_string(j.depth()) + " in it and the document " + floor.substr(0, 12) + ", the one before the oldest kept unit is " + at->second.substr(0, 12), "floor");
      return true;
    }
    (void)act("edit.undo");  // (nothing is left: refused, or a message; the document and the ring stay)
    wait(1);
    if (j.depth() != 0u || digest() != floor) {
      exh_fail(fx, "an undo past the oldest kept unit changed the document or the ring", "past");
      return true;
    }
    for (unsigned i = 0; i < kept; ++i) {
      if (!act("edit.redo")) {
        exh_fail(fx, "edit.redo refused with " + std::to_string(kept - i) + " units to give back", "redo_refused");
        return true;
      }
      wait(1);
    }
    wait(1);
    if (j.depth() != kept || digest() != last) {
      exh_fail(fx, "redo of the " + std::to_string(kept) + " kept units does not give the last document back", "redo");
    }
    return true;
  }

  // The sample-edit history cap. sedit.run effects (reverse, fade in, fade out, normalise) on the Sample edit screen, past SampleBank::kHistory: rule 1 judges every edit that changed the audio (the full history shows
  // no deeper history, so the rule asks that nothing outside the history's slot changed with it), the history holds kHistory steps, and sedit.undo / sedit.redo walk the versions that were kept (the oldest dropped).
  bool exh_sedit_cap() {
    constexpr unsigned fx = 1;
    constexpr unsigned kHist = app::SampleBank::kHistory;
    meta_settle();
    unsigned slot = app::SampleBank::kSlots;
    const Extra e = extra_of(*sim_);
    for (unsigned i = 0; i < app::SampleBank::kSlots && slot == app::SampleBank::kSlots; ++i) {
      slot = e.used[i] ? i : slot;
    }
    if (slot == app::SampleBank::kSlots) {
      // (instrument 1 may hold a synth or an empty sampler the walk made: the WAV goes into the first instrument that is not set)
      unsigned inst = 0;
      while (inst + 1u < qanet::kPcmSlots && sim_->project() != nullptr && sim_->project()->instrument(inst) != nullptr) {
        ++inst;
      }
      if (!meta_load_sample(inst)) {
        return exh_no("no sample could be loaded (" + field(state_line(), "screen") + "/" + field(state_line(), "overlay") + ", sel '" + field(state_line(), "sel") + "', card " + std::string(sim_->card_storage() != nullptr && sim_->card_storage()->card_present() ? "in" : "out") + ", instrument 1 " + std::string(sim_->project() != nullptr && sim_->project()->instrument(0) != nullptr ? "set" : "empty") + ")");
      }
      slot = inst;
    }
    if (slot >= qanet::kPcmSlots || !act("instrument.select", true, static_cast<int>(slot) + 1)) {
      return exh_no("instrument.select refused, or the slot is out of the snapshot's range");
    }
    tap(app::Button::Pattern);
    tap(app::Button::SampleEdit);
    if (field(state_line(), "screen") != "SAMPLE_EDIT") {
      return exh_no("the Sample edit screen was not reached");
    }
    static const int kOps[] = {2, 3, 4, 1};  // EditOp: Reverse, FadeIn, FadeOut, Normalise
    const unsigned want = kHist + 3u + exh_rng_.below(2);
    unsigned op_n = exh_rng_.below(4);
    std::vector<std::uint64_t> trail;  // the audio's hash: [0] before the first edit, [k] after the k-th
    trail.push_back(qanet::snap_of(*sim_).pcm_hash[slot]);
    unsigned edits = 0;
    bool full_seen = false;  // an edit was made with the history at kHistory steps (the arena can keep fewer: SampleBank::edit_begin frees the oldest step when it has no room for a copy)
    for (unsigned tries = 0; edits < want && tries < 6u * want; ++tries) {
      const qanet::Snap before = qanet::snap_of(*sim_);
      if (!act("sedit.run", true, kOps[op_n++ % 4u])) {
        continue;
      }
      tapped_ = false;
      quiet();
      qanet::Snap after = qanet::snap_of(*sim_, &before);
      for (unsigned i = 0; i < 40u && after.pcm_hash[slot] == before.pcm_hash[slot]; ++i) {  // (the job may still be running: the effect is a job of its own)
        wait(50);
        after = qanet::snap_of(*sim_, &before);
      }
      if (after.pcm_hash[slot] == before.pcm_hash[slot]) {
        continue;  // the effect left the audio as it was: not an edit
      }
      // every edit is judged, the first one after a slot switch too: the bank drops the other slot's history, the new step is the unit
      exh_rule1(fx, before, after, edits == kHist + 1u, " [edit " + std::to_string(edits + 1u) + " of slot " + std::to_string(slot) + ", sample-edit history " + std::to_string(before.sedit) + " (slot " + std::to_string(before.sedit_slot) + ") > " + std::to_string(after.sedit) + " (slot " + std::to_string(after.sedit_slot) + ")]");
      stats_.exh_units += before.sedit_full ? 1u : 0u;
      full_seen = full_seen || (before.sedit_full && before.sedit_slot == static_cast<int>(slot));
      trail.push_back(after.pcm_hash[slot]);
      ++edits;
    }
    if (edits < kHist + 2u) {
      return exh_no("fewer edits than the cap needs changed the audio");
    }
    if (!full_seen) {
      return exh_no("the history never held kHistory steps before an edit");
    }
    qanet::Snap now = qanet::snap_of(*sim_);
    const unsigned depth = now.sedit;  // kHistory, or fewer when the arena freed steps for room
    if (depth < 1u || depth > kHist || now.sedit_slot != static_cast<int>(slot)) {
      exh_fail(fx, "after " + std::to_string(edits) + " edits the sample editor's history holds " + std::to_string(depth) + " steps (slot " + std::to_string(now.sedit_slot) + "), the bank keeps 1.." + std::to_string(kHist), "depth");
      return true;
    }
    for (unsigned k = 1; k <= depth + 2u; ++k) {
      (void)act("sedit.undo");
      wait(50);
      now = qanet::snap_of(*sim_);
      const unsigned back = k < depth ? k : depth;  // (the history ends after `depth` steps: further undos stay at the oldest version kept)
      if (now.pcm_hash[slot] != trail[edits - back] || now.sedit != depth - back) {
        exh_fail(fx, "sedit.undo number " + std::to_string(k) + " after " + std::to_string(edits) + " edits: the audio is not the version " + std::to_string(back) + " back, or the history is " + std::to_string(now.sedit) + " deep", "undo");
        return true;
      }
    }
    for (unsigned k = 1; k <= depth; ++k) {
      (void)act("sedit.redo");
      wait(50);
    }
    now = qanet::snap_of(*sim_);
    if (now.pcm_hash[slot] != trail[edits] || now.sedit != depth) {
      exh_fail(fx, "sedit.redo of the " + std::to_string(depth) + " kept steps does not give the last version of the audio back", "redo");
      return true;
    }
    return true;
  }

  // The first edit after a slot switch (B8 review): two edits of one sample slot `from` build its history, the user selects another instrument that holds audio and applies one effect. The bank drops `from`'s history
  // (SampleBank::edit_begin), a non-crop edit makes no ring unit, and rule 1 must still take the new step for the unit (rules.cpp sedit_confined). The other slot is a used one, else a WAV is loaded into the first
  // free instrument. A third fixture, counted like the other two (--min-exhaust): every exit says why it did not reach the switch.
  // One effect on the Sample edit screen of `slot` (the instrument selected, the screen reached): false when the audio did not change.
  bool exh_edit_once(unsigned slot, unsigned op, qanet::Snap& before, qanet::Snap& after) {
    static const int kOps[] = {2, 3, 4, 1};  // EditOp: Reverse, FadeIn, FadeOut, Normalise
    before = qanet::snap_of(*sim_);
    if (!act("sedit.run", true, kOps[op % 4u])) {
      return false;
    }
    tapped_ = false;
    quiet();
    after = qanet::snap_of(*sim_, &before);
    for (unsigned i = 0; i < 40u && after.pcm_hash[slot] == before.pcm_hash[slot]; ++i) {
      wait(50);
      after = qanet::snap_of(*sim_, &before);
    }
    return after.pcm_hash[slot] != before.pcm_hash[slot];
  }

  bool exh_to_sedit(unsigned slot) {
    if (slot >= qanet::kPcmSlots || !act("instrument.select", true, static_cast<int>(slot) + 1)) {
      return false;
    }
    tap(app::Button::Pattern);
    tap(app::Button::SampleEdit);
    return field(state_line(), "screen") == "SAMPLE_EDIT";
  }

  bool exh_sedit_switch() {
    constexpr unsigned fx = 2;
    meta_settle();
    if (sim_->project() == nullptr || sim_->card_storage() == nullptr || !sim_->card_storage()->card_present()) {
      return exh_no("no project, or no card");
    }
    Extra e = extra_of(*sim_);
    unsigned from = app::SampleBank::kSlots;
    for (unsigned i = 0; i < qanet::kPcmSlots && from == app::SampleBank::kSlots; ++i) {
      from = e.used[i] ? i : from;
    }
    const auto free_inst = [&](unsigned skip) {
      unsigned inst = 0;
      while (inst + 1u < qanet::kPcmSlots && (inst == skip || (sim_->project()->instrument(inst) != nullptr))) {
        ++inst;
      }
      return inst == skip ? static_cast<unsigned>(app::SampleBank::kSlots) : inst;
    };
    if (from == app::SampleBank::kSlots) {
      const unsigned inst = free_inst(app::SampleBank::kSlots);
      if (inst >= qanet::kPcmSlots || !meta_load_sample(inst)) {
        return exh_no("no sample could be loaded for the first slot");
      }
      from = inst;
      e = extra_of(*sim_);
    }
    unsigned other = app::SampleBank::kSlots;
    for (unsigned i = 0; i < qanet::kPcmSlots && other == app::SampleBank::kSlots; ++i) {
      other = (i != from && e.used[i]) ? i : other;
    }
    if (other == app::SampleBank::kSlots) {
      const unsigned inst = free_inst(from);
      if (inst >= qanet::kPcmSlots || !meta_load_sample(inst)) {
        return exh_no("no second sample could be loaded");
      }
      other = inst;
    }
    if (!exh_to_sedit(from)) {
      return exh_no("the Sample edit screen of the first slot was not reached");
    }
    unsigned op_n = exh_rng_.below(4);
    qanet::Snap before, after;
    for (unsigned edits = 0, tries = 0; edits < 2u && tries < 8u; ++tries) {
      edits += exh_edit_once(from, op_n++, before, after) ? 1u : 0u;
      if (tries == 7u && edits < 2u) {
        return exh_no("two edits of the first slot did not change its audio");
      }
    }
    if (qanet::snap_of(*sim_).sedit_slot != static_cast<int>(from)) {
      return exh_no("the first slot holds no history after its edits");
    }
    if (!exh_to_sedit(other)) {
      return exh_no("the Sample edit screen of the second slot was not reached");
    }
    for (unsigned tries = 0; tries < 4u; ++tries) {
      if (!exh_edit_once(other, op_n++, before, after)) {
        continue;
      }
      if (before.sedit_slot != static_cast<int>(from) || before.sedit < 1u) {
        return exh_no("the history was not the first slot's before the switch");
      }
      exh_rule1(fx, before, after, true, " [first edit of slot " + std::to_string(other) + " after " + std::to_string(before.sedit) + " steps of slot " + std::to_string(from) + ", history " + std::to_string(after.sedit) + " (slot " + std::to_string(after.sedit_slot) + ")]");
      ++stats_.exh_units;
      return true;
    }
    return exh_no("no effect on the second slot changed its audio");
  }


  // ---- B8: the two fixtures that run on a simulator of their own ----
  // The voice pool needs a sustained sound (the generated card's WAVs end in a few milliseconds) and the folder of 2000 needs a card of its own, so each drops the walk's simulator for a fresh one and drives it with the
  // walk's own inputs (tap, wait, act, pad): the invariants of check() judge it too (a stuck job, an open directory, an empty listing). They come after the other fixtures (they run last: the walk's state is gone).
  bool exh_fresh(const std::string& card, bool sound, bool audio, std::string& error) {
    sim_ = std::make_unique<sim::Simulator>();
    sim_->set_scenario_dir(std::string());
    sim_->step();
    sim_->open_document(false, false, 0, sound);
    if (!card.empty() && !sim_->open_card(card, error)) {
      return false;
    }
    if (audio && !sim_->audio_on(error)) {
      return false;
    }
    history_.clear();
    snap_valid_ = false;
    in_unit_ = false;
    unit_active_ = false;
    pending_begin_ = false;
    tapped_ = false;
    shift_held_ = false;
    files_generation_ = 0;
    listing_stale_ = false;
    posts_seen_ = 0;
    busy_ticks_ = 0;
    scan_idle_ticks_ = 0;
    no_card_ticks_ = 0;
    last_screen_.clear();
    last_msg_.clear();
    last_keys_.clear();
    new_pending_ = false;
    checked_at_ = ticks_;
    ticks_ += 1;
    return true;
  }

  // A pad goes down or up as the scenario's `pad <n> down` and `pad <n> up` (held: the next inputs come while it is).
  void exh_pad(unsigned pad, bool down) {
    input_ = std::string("pad ") + std::to_string(pad) + (down ? " down" : " up");
    rec(input_);
    if (down) {
      sim_->pad_down(pad);
    } else {
      sim_->pad_up(pad);
    }
    tick();
  }

  // The voice pool saturated by held pads. `document sound` with the engine: instrument 1 is the test bank's looped sine, so a held pad sounds for ever. 13 to 16 different pads of the NOTES mode are held, one after the other
  // (past kLiveMax = 12 notes held and kMaxVoices = 12 voices: the live note past the limit cuts the oldest, live_notes.h), then released in a random order, one lost on purpose when --plant-exhaust (a lost key-up).
  // Judged: each pad up to the twelfth starts a voice of its own; past the limit the pool stays at 12 in use (never more) and its published peak is 12; a pad pressed again does not take another voice; once every pad is released
  // the pool is quiet within a second (no voice stuck: a held pad must not outlive its release); a pad after that sounds again and goes quiet at its release.
  bool exh_voice_pool() {
    constexpr unsigned fx = 3;
    constexpr unsigned kPool = audio::kMaxVoices;
    constexpr unsigned kPads = 36;  // three octaves of the NOTES mode
    std::string error;
    if (!exh_fresh(std::string(), true, true, error)) {
      return exh_no("a simulator with the engine could not be made: " + error);
    }
    audio::VoicePool& voices = sim_->audio()->voices();
    wait(20);
    if (field(state_line(), "screen") != "PATTERN" || field(state_line(), "pads") != "NOTES" || voices.voices_in_use() != 0u) {
      return exh_no("the sound document is not on the Pattern screen with the NOTES pads and a quiet pool (" + state_line() + ")");
    }
    const bool planted = opt_.plant_exhaust;
    const std::string mark = planted ? " (planted)" : std::string();
    const unsigned n = kPool + 1u + exh_rng_.below(4);  // 13..16 pads
    unsigned pool[kPads];
    for (unsigned i = 0; i < kPads; ++i) {
      pool[i] = i;
    }
    for (unsigned i = 0; i < n; ++i) {  // n different pads, in a random order
      std::swap(pool[i], pool[i + exh_rng_.below(kPads - i)]);
    }
    const std::vector<unsigned> pads(pool, pool + n);
    for (unsigned k = 0; k < n; ++k) {
      exh_pad(pads[k], true);
      wait(k == kPool ? 40u : 6u);  // (the pad past the limit cuts the oldest: its declick ends before the count is read)
      const unsigned in_use = voices.voices_in_use();
      const unsigned want = k + 1u < kPool ? k + 1u : kPool;
      if (in_use != want) {
        exh_fail(fx, "with " + std::to_string(k + 1u) + " pads held (pad " + std::to_string(pads[k]) + " last) " + std::to_string(in_use) + " voices are in use, the pool of " + std::to_string(kPool) + " should have " + std::to_string(want) + mark, "in_use");
        return true;
      }
      ++stats_.exh_units;
    }
    const audio::VoicePool::VoiceSnapshot snap = voices.voice_snapshot();
    if (snap.peak != kPool || snap.in_use != kPool) {
      exh_fail(fx, "with " + std::to_string(n) + " pads held the pool published " + std::to_string(snap.in_use) + " in use and a peak of " + std::to_string(snap.peak) + ", both should be " + std::to_string(kPool) + mark, "peak");
      return true;
    }
    exh_pad(pads[n - 1], true);  // the newest pad again: a held key retriggers its own voice
    wait(20);
    if (voices.voices_in_use() != kPool) {
      exh_fail(fx, "pressing the held pad " + std::to_string(pads[n - 1]) + " again leaves " + std::to_string(voices.voices_in_use()) + " voices in use, not " + std::to_string(kPool) + mark, "retrigger");
      return true;
    }
    std::vector<unsigned> order = pads;
    for (unsigned i = n; i > 1; --i) {
      std::swap(order[i - 1], order[exh_rng_.below(i)]);
    }
    for (const unsigned p : order) {
      if (planted && p == pads[n - 1]) {
        continue;  // the planted defect: this pad's key-up is lost
      }
      exh_pad(p, false);
      wait(1);
    }
    unsigned waited = 0;
    while (voices.voices_in_use() != 0u && waited < 1000u) {
      wait(20);
      waited += 20u;
    }
    if (voices.voices_in_use() != 0u) {
      exh_fail(fx, std::to_string(voices.voices_in_use()) + " voices are still in use " + std::to_string(waited) + " ms after all " + std::to_string(n) + " pads were released" + mark, "stuck");
      return true;
    }
    exh_pad(pads[0], true);  // the pool still works
    wait(20);
    if (voices.voices_in_use() != 1u) {
      exh_fail(fx, "a pad pressed after the pool was saturated and released starts " + std::to_string(voices.voices_in_use()) + " voices, not 1" + mark, "reuse");
      return true;
    }
    exh_pad(pads[0], false);
    for (waited = 0; voices.voices_in_use() != 0u && waited < 1000u; waited += 20u) {
      wait(20);
    }
    if (voices.voices_in_use() != 0u || voices.voice_snapshot().in_use != 0u) {
      exh_fail(fx, "the last pad's voice is still in use " + std::to_string(waited) + " ms after its release" + mark, "reuse_stuck");
    }
    return true;
  }

  // A folder of 2000 files. The card is made here (ScratchCard): `big/` with 2000 files (1600 .wav, 200 .txt, 200 hidden), `small/` with three. Sample load goes into it. The listing holds kMaxEntries = 256 (dir_index.h): it
  // must be Done within the scan bound, hold exactly 256 entries, every one a visible .wav file, sorted, and say it is truncated (the screen's "list full"); the cursor runs the 256 rows and stops at both ends (Down 300 times, Up 300
  // times, Jump end); a file of the last row loads; up to the root the listing is the root's, whole and not truncated; into `small` it is three files and not truncated (the full flag does not outlive the folder); `big` again and
  // RESCAN give the same 256. No directory handle stays open (check()). --plant-exhaust: the folder holds 200 .wav instead of 1600, a listing that lost entries.
  bool exh_big_folder() {
    constexpr unsigned fx = 4;
    constexpr unsigned kCap = app::DirIndex::kMaxEntries;
    constexpr unsigned kWavs = kBigFiles - kBigJunk;
    const bool planted = opt_.plant_exhaust;
    const std::string mark = planted ? " (planted)" : std::string();
    std::string error;
    ScratchCard card;
    if (!card.make(seed_, planted ? 200u : kWavs, error)) {
      return exh_no(error);
    }
    if (!exh_fresh(card.root.string(), false, false, error)) {
      return exh_no("the card could not be opened: " + error);
    }
    card.remove();  // the simulator copied it
    const auto scan = [&]() { return sim_->app().model().files; };
    // Ticks until the listing has ended (Done or a failure) and no card job runs.
    const auto scan_end = [&]() {
      unsigned t = 0;
      for (; t < kScanBoundTicks + 200u; t += 10u) {
        const app::DirScan* const f = scan();
        if (f != nullptr && f->state() != app::DirScan::State::Scanning && f->state() != app::DirScan::State::Idle && !sim_->app().card_jobs_busy()) {
          break;
        }
        wait(10);
      }
      return t;
    };
    const auto pos = [&]() { return sim_->app().state().wav.list.pos(); };
    const auto at_root = [&]() {
      const app::DirScan* const f = scan();
      return f != nullptr && (std::strcmp(f->folder(), "") == 0 || std::strcmp(f->folder(), "/") == 0);
    };
    const auto describe = [&]() { return field(state_line(), "screen") + " folder '" + (scan() != nullptr ? scan()->folder() : "?") + "' sel '" + field(state_line(), "sel") + "'"; };
    tap(app::Button::Pattern);
    tap(app::Button::SampleLoad);
    wait(100);
    if (field(state_line(), "screen") != "SAMPLE_LOAD") {
      return exh_no("the Sample load screen was not reached (" + describe() + ")");
    }
    for (unsigned i = 0; i < 4u; ++i) {
      scan_end();
      if (at_root()) {
        break;
      }
      (void)act("sample.up");
    }
    scan_end();
    if (!at_root()) {
      return exh_no("the Sample load screen did not go up to the card's root (" + describe() + ")");
    }
    // The listing of a folder, judged against what it must hold: `files` listed .wav files, `folders` folders.
    const auto judge = [&](const char* where, unsigned folders, unsigned files, bool full) {
      const app::DirScan* const f = scan();
      if (f == nullptr || f->state() != app::DirScan::State::Done) {
        exh_fail(fx, std::string(where) + ": the scan did not end in Done (" + describe() + ")" + mark, std::string("state_") + where);
        return false;
      }
      const app::DirIndex& ix = f->index();
      const unsigned want = std::min(folders + files, kCap);
      if (ix.count() != want || f->truncated() != full || ix.full() != full) {
        exh_fail(fx, std::string(where) + ": the listing holds " + std::to_string(ix.count()) + " entries" + (f->truncated() ? ", truncated" : ", not truncated") + "; the folder has " + std::to_string(folders) + " folders and " + std::to_string(files) + " listed files: " + std::to_string(want) + (full ? ", truncated" : ", whole") + " expected" + mark, std::string("count_") + where);
        return false;
      }
      for (unsigned i = 0; i < ix.count(); ++i) {
        const std::string name = ix.name(i);
        const bool wav = name.size() > 4u && name.compare(name.size() - 4u, 4u, ".wav") == 0;
        if (name.empty() || name[0] == '.' || (!ix.is_dir(i) && !wav)) {
          exh_fail(fx, std::string(where) + ": entry " + std::to_string(i) + " '" + name + "' is a name the filter must not list" + mark, std::string("entry_") + where);
          return false;
        }
        if (i > 0u && app::DirIndex::compare(ix.is_dir(i - 1u), ix.name(i - 1u), ix.is_dir(i), name.c_str()) >= 0) {
          exh_fail(fx, std::string(where) + ": entries " + std::to_string(i - 1u) + " '" + ix.name(i - 1u) + "' and " + std::to_string(i) + " '" + name + "' are not in order" + mark, std::string("order_") + where);
          return false;
        }
      }
      return true;
    };
    // The cursor goes to the entry called `name` of the listing (a few Down or Up presses: the folders are first).
    const auto go_to = [&](const char* name) {
      for (unsigned i = 0; i < 8u; ++i) {
        if (field(state_line(), "sel") == name) {
          return true;
        }
        tap(app::Button::Down);
      }
      for (unsigned i = 0; i < 8u; ++i) {
        if (field(state_line(), "sel") == name) {
          return true;
        }
        tap(app::Button::Up);
      }
      return field(state_line(), "sel") == name;
    };
    if (!judge("root", 2u, 0u, false)) {
      return true;
    }
    if (!go_to("big")) {
      return exh_no("the cursor did not reach the folder big (" + describe() + ")");
    }
    (void)act("sample.enter");
    const unsigned took = scan_end();
    stats_.exh_units += took;
    if (took >= kScanBoundTicks) {
      exh_fail(fx, "the listing of the folder of " + std::to_string(kBigFiles) + " files took " + std::to_string(took) + " ticks (bound " + std::to_string(kScanBoundTicks) + ")" + mark, "scan_time");
      return true;
    }
    if (!judge("big", 0u, kWavs, true)) {
      return true;
    }
    const unsigned rows = scan()->index().count();
    if (pos() != 0) {
      exh_fail(fx, "entering the folder puts the cursor on row " + std::to_string(pos()) + ", not the first" + mark, "enter_cursor");
      return true;
    }
    for (unsigned i = 0; i < 300u; ++i) {
      tap(app::Button::Down);
    }
    if (pos() != static_cast<int>(rows) - 1) {
      exh_fail(fx, "after 300 presses of Down the cursor is on row " + std::to_string(pos()) + " of " + std::to_string(rows) + mark, "cursor_down");
      return true;
    }
    for (unsigned i = 0; i < 300u; ++i) {
      tap(app::Button::Up);
    }
    if (pos() != 0) {
      exh_fail(fx, "after 300 presses of Up the cursor is on row " + std::to_string(pos()) + ", not the first" + mark, "cursor_up");
      return true;
    }
    (void)act("cursor.jump_end");
    if (pos() != static_cast<int>(rows) - 1) {
      exh_fail(fx, "Jump end puts the cursor on row " + std::to_string(pos()) + " of " + std::to_string(rows) + mark, "jump_end");
      return true;
    }
    // A file of the last row loads (the name's place in the pool is the path the card has).
    const Extra before = extra_of(*sim_);
    const std::string last = scan()->index().name(rows - 1u);
    tap(app::Button::F1);
    if (sim_->project() != nullptr && sim_->project()->instrument(0) != nullptr) {
      wait(50);
      tap(app::Button::F1);  // (REPLACE? answered)
    }
    bool loaded = false;
    for (unsigned i = 0; i < 40u && !loaded; ++i) {
      wait(100);
      loaded = extra_of(*sim_).slots_used() > before.slots_used();
    }
    if (!loaded) {
      exh_fail(fx, "F1 on the last row of the full listing ('" + last + "') loaded nothing (" + describe() + ", msg " + field(state_line(), "msg") + ")" + mark, "load_last");
      return true;
    }
    // Up to the root: a whole listing again (the full flag is the folder's).
    (void)act("sample.up");
    scan_end();
    if (!at_root() || !judge("root_again", 2u, 0u, false)) {
      if (!at_root()) {
        exh_fail(fx, "F2 from the full listing did not go up to the root (" + describe() + ")" + mark, "up");
      }
      return true;
    }
    if (!go_to("small")) {
      return exh_no("the cursor did not reach the folder small (" + describe() + ")");
    }
    (void)act("sample.enter");
    scan_end();
    if (!judge("small", 0u, 3u, false)) {
      return true;
    }
    (void)act("sample.up");
    scan_end();
    if (!go_to("big")) {
      return exh_no("the cursor did not reach the folder big again (" + describe() + ")");
    }
    (void)act("sample.enter");
    scan_end();
    if (!judge("big_again", 0u, kWavs, true)) {
      return true;
    }
    (void)act("sample.rescan");
    wait(50);
    scan_end();
    judge("rescan", 0u, kWavs, true);
    return true;
  }

  void card_step() {
    std::string error;
    if (sim_->card_storage()->card_present()) {
      input_ = "card pull";
      rec("card pull");
      if (sim_->card_pull(error)) {
        tick();
        ++stats_.cards_pulled;
      } else {
        history_.pop_back();
      }
    } else {
      input_ = "card insert";
      rec("card insert");
      if (sim_->card_insert(error)) {
        tick(2 * 16 + 1);
      } else {
        history_.pop_back();
      }
    }
  }

  // An undo burst: the digest of the document, a few steps that may edit it (not File: NEW and OPEN replace the document), then as many undos as the journal grew.
  void burst() {
    input_ = "burst";
    burst_ = true;
    ++stats_.bursts;
    const model::Project* const project = sim_->project();
    if (project == nullptr) {
      return;
    }
    wait(1);
    const std::string before = sim::model_digest(*project);
    const std::string label = "b" + std::to_string(stats_.bursts);  // unique in a script: one mark a burst
    rec("mark model " + label);  // the digest the scenario's assertion compares with, taken at the instant the walk took `before` (no input between)
    const std::string session0 = session_signature(*project);
    const unsigned depth0 = sim_->app().model().undo_depth;
    burst_min_depth_ = depth0;
    burst_left_pattern_ = false;
    const unsigned n = kBurstMin + rng_.below(kBurstMax - kBurstMin + 1);
    tap(app::Button::Pattern);  // the document's own screen, where edits are; in Browse (mode 3) nothing edits, and Rec is what a person presses to edit
    if (field(state_line(), "mode") != "0") {
      tap(app::Button::Rec);
    }
    for (unsigned i = 0; i < n; ++i) {
      plain(pick_kind(rng_, kBurstKinds, sizeof kBurstKinds / sizeof kBurstKinds[0]), true);
    }
    wait(1);
    // `edit.undo` acts on the area of the screen in force and the journal depth read here is the Pattern area's: the compare needs the burst to have stayed on the Pattern screen (a step on Song or Sound
    // edits another area's journal, and the undos below would not reach it).
    if (sim_->project() != project || sim_->app().state().screen != app::ScreenId::Pattern || burst_left_pattern_) {
      return;  // the document was replaced, or the burst was not all on the Pattern screen: nothing to compare
    }
    const unsigned depth1 = sim_->app().model().undo_depth;
    if (depth1 <= depth0) {
      return;  // nothing the journal holds: not an edit burst
    }
    if (burst_min_depth_ < depth0) {
      ++stats_.bursts_shrunk;  // something in the burst undid an edit that was there before it (allow-list entry 2): the burst's own undos would stop short of that edit
      return;
    }
    ++stats_.bursts_with_edits;
    const bool moved = stats_.burst_script.empty() && sim::model_digest(*project) != before;  // the burst changed the document (for the self-test's script: undo has something to undo)
    bool ran = true;
    for (unsigned i = depth0; i < depth1; ++i) {
      rec("action edit.undo");
      const app::DispatchResult r = sim_->action(undo_id_, 0);
      tick();
      if (r == app::DispatchResult::Disabled || r == app::DispatchResult::Unknown) {
        history_.pop_back();  // not an input that took: `o3sim` would refuse the line
        ran = false;  // a dialog holds the focus: the undo cannot run, the burst proves nothing
        break;
      }
    }
    wait(1);
    if (!ran || sim_->project() != project || sim_->app().model().undo_depth != depth0) {
      return;
    }
    if (session_signature(*project) != session0) {
      ++stats_.bursts_session;  // allow-list entry 1 (session_signature above)
      return;
    }
    ++stats_.bursts_conclusive;
    if (moved) {
      stats_.burst_script = history_;
      stats_.burst_label = label;
    }
    const std::string after = sim::model_digest(*project);
    if (after != before) {
      record("undo", "after " + std::to_string(depth1 - depth0) + " undoable steps and as many undos the document's digest is " + after.substr(0, 12) + ", before it was " + before.substr(0, 12),
             undo_tail(label));
    }
  }

  // ---- C4: the audio probe (tests/qanet/audio_probe.h) ----

 public:
  // The probe point of the step that has just ended (the walk's inputs are `history_`): `rebound` is the instrument whose descriptor inputs changed (0: a session point only). Public for the self-test, which feeds
  // a scripted walk through replay() and probes it.
  void probe_point(unsigned rebound, const std::string& label, bool session, const qanet_audio::ProbeOptions& po) {
    if (sim_->project() == nullptr) {
      return;
    }
    qanet_audio::WalkProbe w;
    w.card_dir = opt_.card;
    for (const Input& h : history_) {
      w.lines.push_back(h.line);
    }
    w.instrument = rebound != 0u ? rebound : static_cast<unsigned>(std::strtoul(field(state_line(), "instrument").c_str(), nullptr, 10));
    w.probe_now = rebound != 0u;
    w.label = label;
    w.digest = sim::model_digest(*sim_->project());
    w.seed = seed_;
    w.session = session;
    ++stats_.probe_points;
    const qanet_audio::ProbeReport rep = qanet_audio::probe_walk_state(w, po);
    stats_.probes += rep.probes;
    stats_.probes_stepped += rep.stepped;
    stats_.probes_skipped += rep.skipped + rep.diverged;
    if (opt_.verbose || opt_.trace) {
      for (const std::string& l : rep.log) {
        std::printf("seed %llu step %u: probe %s\n", static_cast<unsigned long long>(seed_), step_, l.c_str());
      }
    }
    if (!rep.error.empty()) {
      std::printf("seed %llu step %u: probe could not run: %s\n", static_cast<unsigned long long>(seed_), step_, rep.error.c_str());
    }
    for (const qanet_audio::Finding& f : rep.findings) {
      record_probe(f);
    }
  }
  const std::vector<Finding>& probe_findings() const { return *findings_; }

 private:
  void audio_probe_step() {
    if (sim_->project() == nullptr) {
      return;
    }
    const std::string last = history_.empty() ? std::string() : history_.back().line;
    const bool undo = last.find("undo") != std::string::npos || last == "tap f8";
    std::string what;
    const unsigned changed = qanet_audio::watch_instruments(watch_, *sim_->project(), undo, &what);
    const bool middle = opt_.audio_session && step_ == opt_.steps / 2u;  // the session point: the walk's state at the middle, whatever it is
    if (changed != 0u) {
      ++stats_.rebinds;
    }
    bool trigger = changed != 0u && stats_.rebinds % std::max(1u, opt_.probe_every) == 0u;
    std::string why;
    if (trigger && !qanet_audio::probe_possible(*sim_, changed, &why)) {  // (the replay is the cost: a state the probe would refuse is not replayed)
      ++stats_.probes_skipped;
      if (opt_.verbose || opt_.trace) {
        std::printf("seed %llu step %u: probe %s: %s\n", static_cast<unsigned long long>(seed_), step_, what.c_str(), why.c_str());
      }
      trigger = false;
    }
    if (trigger || middle) {
      qanet_audio::ProbeOptions po;
      probe_point(trigger ? changed : 0u, what, middle, po);
    }
  }
  void record_probe(const qanet_audio::Finding& f) {
    const std::string action = f.paths.substr(0, f.paths.find(' ')) + ":" + f.what;  // `pad:fresh:pitch`
    if (!reported_.insert("audio_probe|" + f.cell + "|" + action).second) {
      return;
    }
    Finding o;
    o.invariant = "audio_probe";
    o.seed = seed_;
    o.step = step_;
    o.screen = f.cell;
    o.action = action;
    o.why = f.diff;
    for (const Input& h : history_) {
      o.repro += (h.line.empty() ? "" : (o.repro.empty() ? "" : "; ")) + h.line;
    }
    o.lines = history_;
    findings_->push_back(o);
  }
  qanet_audio::InstWatch watch_;

  // ---- the invariants ----

  std::string state_line() const {
    char buf[1024];
    const std::size_t n = app::describe_state(sim_->app(), buf, sizeof buf);
    return std::string(buf, std::min(n, sizeof buf - 1));
  }

  // `action`: the input the finding is keyed by ("" the walk's last input, or `burst` inside one); `match`: what the run dedups it by ("" the invariant's name).
  void record(const std::string& invariant, const std::string& why, const std::string& tail = std::string(), const std::string& action = std::string(), const std::string& match = std::string(),
              const std::string& mark = std::string()) {
    const std::string key = match.empty() ? invariant : match;
    if (!reported_.insert(key).second) {
      return;
    }
    Finding f;
    f.invariant = invariant;
    f.match = key;
    f.mark = mark;
    f.seed = seed_;
    f.step = step_;
    f.screen = field(state_line(), "screen");
    f.action = !action.empty() ? action : (burst_ ? "burst" : input_);
    f.why = why;
    for (const Input& h : history_) {
      f.repro += (f.repro.empty() ? "" : "; ") + h.line;
    }
    f.lines = history_;
    f.tail = tail;
    findings_->push_back(f);
  }

  const app::ListFilter* filter_of(const app::ViewState& v) const {
    if (v.screen == app::ScreenId::File) {
      return v.import.page ? &app::kModuleFilter : &app::kProjectFilter;
    }
    return v.wav.pti ? &app::kPtiFilter : &app::kWavFilter;
  }

  unsigned oracle_count(const char* folder, const app::ListFilter& filter) {
    ++stats_.oracle_runs;
    auto scan = std::make_unique<app::DirScan>();
    hal::storage::Storage& card = *sim_->card_storage();
    if (scan->begin(card, folder, filter) != app::DirScan::Begin::Started) {
      return 0;
    }
    for (unsigned i = 0; i < 100000 && scan->state() == app::DirScan::State::Scanning; ++i) {
      (void)scan->step(1000000);
    }
    scan->abandon();
    return scan->state() == app::DirScan::State::Done ? scan->index().count() : 0u;
  }

  void check() {
    const unsigned dt = ticks_ - checked_at_;
    checked_at_ = ticks_;
    if (!sim_->healthy()) {
      record("display_list", sim_->failure());
    }
    if (!hold_.empty()) {
      const std::string problem = sim::state_problem(state_line(), hold_);
      if (!problem.empty()) {
        record("planted_state", problem, "assert state " + opt_.plant_hold);
      }
    }
    const app::App& app = sim_->app();
    const app::ViewState& v = app.state();
    const app::DirScan* files = app.model().files;
    hal::storage::Storage* const card = sim_->card_storage();
    const bool card_in = card != nullptr && card->card_present() && card->mounted();
    const bool browser = v.screen == app::ScreenId::File || v.screen == app::ScreenId::SampleLoad;
    const app::DirScan::State st = files != nullptr ? files->state() : app::DirScan::State::Idle;
    const bool busy = app.card_jobs_busy();

    // no_sd_card. As a scenario it ends with the assertion that the listing has an entry once the scan has had time, when the folder the screen means holds something (an oracle scan of it: the
    // card is in, so a screen that says NO SD CARD is wrong only if the folder lists something).
    if (card_in && browser && (st == app::DirScan::State::Idle || st == app::DirScan::State::NoCard)) {
      no_card_ticks_ += dt;
      if (no_card_ticks_ > kNoCardTicks && reported_.count("no_sd_card") == 0u) {
        const bool known = files != nullptr && files->folder() != nullptr && *files->folder() != '\0';
        const unsigned expect = known ? oracle_count(files->folder(), *filter_of(v)) : 0u;
        record("no_sd_card", std::string("the card is in and mounted and the scan is ") + (st == app::DirScan::State::Idle ? "Idle" : "NoCard") + " for " + std::to_string(no_card_ticks_) + " ticks", expect > 0u ? kListsSomething : std::string());
      }
    } else {
      no_card_ticks_ = 0;
    }

    // job_stuck
    const bool scanning = st == app::DirScan::State::Scanning;
    busy_ticks_ = busy ? busy_ticks_ + dt : 0u;
    const unsigned bound = scanning ? kScanBoundTicks : v.xport.running ? kExportBoundTicks : kJobBoundTicks;
    if (busy_ticks_ > bound) {
      record("job_stuck", std::string("a card job runs for ") + std::to_string(busy_ticks_) + " ticks (bound " + std::to_string(bound) + (scanning ? ", a listing" : ", not a listing") + ")");
    }
    scan_idle_ticks_ = scanning && !busy ? scan_idle_ticks_ + dt : 0u;
    if (scan_idle_ticks_ > kStuckScanTicks) {
      record("job_stuck", "the listing is Scanning and no job runs, for " + std::to_string(scan_idle_ticks_) + " ticks");
    }

    // dir_open: at rest, kMaxOpenDirs handles must all be free
    if (card_in && !busy && !scanning) {
      hal::storage::DirHandle h[hal::storage::kMaxOpenDirs];
      unsigned got = 0;
      for (unsigned i = 0; i < hal::storage::kMaxOpenDirs; ++i) {
        if (card->opendir("/", h[i]) == hal::storage::Status::Ok) {
          ++got;
        }
      }
      for (unsigned i = 0; i < hal::storage::kMaxOpenDirs; ++i) {
        if (h[i].is_open()) {
          card->closedir(h[i]);
        }
      }
      if (got < hal::storage::kMaxOpenDirs) {
        record("dir_open", std::to_string(hal::storage::kMaxOpenDirs - got) + " of " + std::to_string(hal::storage::kMaxOpenDirs) + " directory handles are open at rest (no job runs, no scan is Scanning)");
      }
    }

    // A listing is a cache (app/dir_scan.h: a Done scan stays until it is invalidated), so a write by the app after the scan leaves it stale until the next scan starts: after a new message that says
    // the app wrote to the card (allow-list entry 3), listing_empty waits for that. A scan that starts moves the listing's generation (clear() on begin, then every add), and that is read here, so a scan
    // that starts and ends inside one step clears the flag as well. A message and a scan in the same step: the flag stays (the write may be the later of the two).
    if (files != nullptr && files->index().generation() != files_generation_) {
      files_generation_ = files->index().generation();
      listing_stale_ = false;
    }
    const unsigned posts_now = v.notify.posts();
    if (posts_now != posts_seen_) {
      posts_seen_ = posts_now;
      if (is_write_message(field(state_line(), "msg"))) {
        listing_stale_ = true;
      }
    }

    // listing_empty
    if (card_in && !listing_stale_ && browser && files != nullptr && st == app::DirScan::State::Done && files->index().count() == 0u && !reported_.count("listing_empty")) {
      const unsigned expect = oracle_count(files->folder(), *filter_of(v));
      if (expect > 0u) {
        record("listing_empty", "the screen lists nothing in \"" + std::string(files->folder()) + "\" and a scan of the same folder and filter lists " + std::to_string(expect) + " [import page " + (v.import.page ? "on" : "off") + ", pti " + (v.wav.pti ? "on" : "off") + ", skipped " + std::to_string(files->skipped()) + ", error " + hal::storage::status_name(files->error()) +
                         ", state line: " + state_line() + "]",
               kListsSomething);
      }
    }

    // measurements: what the run covered
    const std::string line = state_line();
    const std::string screen = field(line, "screen");
    if (screen != last_screen_) {
      ++stats_.screens_seen;
      last_screen_ = screen;
    }
    if (v.screen == app::ScreenId::File && !field(line, "keys").empty() && field(line, "keys") != last_keys_) {
      ++stats_.questions;
    }
    last_keys_ = field(line, "keys");
    const std::string msg = field(line, "msg");
    if (msg == "NEW_PROJECT" && msg != last_msg_) {
      ++stats_.new_projects;
      new_pending_ = true;
    }
    last_msg_ = msg;
    if (new_pending_ && v.screen == app::ScreenId::SampleLoad && st == app::DirScan::State::Done) {
      ++stats_.new_then_load;
      new_pending_ = false;
    }
    if (v.screen == app::ScreenId::SampleLoad && st == app::DirScan::State::Done) {
      ++stats_.loads_seen;
    }
    if (browser && st == app::DirScan::State::Done) {
      stats_.files_seen += files->index().count() != 0u ? 1u : 0u;
    }
  }

  void mix_state() {
    const std::string line = state_line();
    for (const char c : line) {
      digest_ = (digest_ ^ static_cast<unsigned char>(c)) * 0x100000001B3ull;
    }
    digest_ = (digest_ ^ 0xFFu) * 0x100000001B3ull;
  }

  // The end: the app gets time to finish what it started, then the checks run once more (a job that never ends is the finding).
  void settle() {
    for (unsigned i = 0; i < kScanBoundTicks + 200 && sim_->app().card_jobs_busy(); i += 50) {
      wait(50);
      rest();
      check();
    }
  }

  // Self-test fault: a directory opened here and never closed (as a scan that forgot its closedir would leave it). Waits for a card that is in.
  void plant_leak() {
    hal::storage::Storage* const card = sim_->card_storage();
    hal::storage::DirHandle h;
    planted_ = card != nullptr && card->card_present() && card->mounted() && card->opendir("/", h) == hal::storage::Status::Ok;
  }

  const Options& opt_;
  std::uint64_t seed_;
  Rng rng_;
  Rng meta_rng_;         // B5: its own stream (the walk's `rng_` is not touched, so a run without --meta is the run it was)
  unsigned meta_n_ = 0;  // B5: the relation cycle's position
  std::string exh_name_;  // B8: the fixture running (for the note of a fixture that did not reach its limit)
  Rng exh_rng_;          // B8: the fixtures' own stream (after the walk and --meta, so neither moves)
  unsigned meta_marks_ = 0;
  std::unique_ptr<sim::Simulator> sim_;
  std::vector<Finding>* findings_ = nullptr;
  std::set<std::string> reported_;
  unsigned step_ = 0;
  std::vector<Input> history_;  // rec(): the inputs so far
  // B4: the unit the rules are judging
  unsigned units_ = 0;            // units begun
  bool in_unit_ = false;
  bool unit_active_ = false;      // this unit is judged (every rules_every-th)
  bool pending_begin_ = false;    // the next recorded line begins the unit
  std::size_t unit_from_ = 0;     // history_ index of its first line
  bool snap_valid_ = false;       // cur_ is the state now (no input has moved it since it was taken)
  qanet::Snap cur_, before_;
  Extra cur_extra_, before_extra_;
  bool planted_r1_ = false;
  bool planted_r2_ = false;
  bool planted_r3_ = false;
  bool select_planted_ = false;
  std::string sel_detail_;
  // B4b: what rules 2 and 3 need of the state before the input, taken in unit_begin (and in replay): the context chain, the image of the document, the ticks.
  core::ContextChain chain_;
  qanet::SelImage img_before_;
  bool img_ok_ = false;
  unsigned unit_ticks0_ = 0;
  bool tapped_ = false;  // the unit tapped a button (tap()), with Shift held or not: quiet() waits for a delayed tap
  app::Button tapped_button_ = app::Button::F1;
  bool tapped_shift_ = false;
  bool shift_held_ = false;
  qanet::Clip clip_;     // the pattern clipboard as the last block.copy of the walk filled it (the App's is private)
  bool have_clip_ = false;
  int copied_instrument_ = -1;  // the slot instrument.copy took
  bool quiet_before_ = true;    // the wait before the key (pre_wait) found nothing moving by itself
  bool unit_open_ = false;        // replay: a unit began and has not ended
  std::string input_ = "start";  // the last input given
  unsigned burst_min_depth_ = 0;  // the lowest journal depth seen inside the burst
  bool burst_ = false;           // inside burst()
  bool burst_left_pattern_ = false;  // a step of the burst ended on another screen than Pattern
  unsigned session_ = 0;  // steps left of the session that a screen button opened
  unsigned ticks_ = 0;
  unsigned checked_at_ = 0;
  unsigned no_card_ticks_ = 0;
  unsigned busy_ticks_ = 0;
  unsigned scan_idle_ticks_ = 0;
  std::string last_screen_;
  std::string last_msg_;
  std::string last_keys_;
  bool new_pending_ = false;
  bool listing_stale_ = false;  // the app wrote to the card since the last scan started
  std::uint32_t files_generation_ = 0;  // the listing's DirIndex generation at the last check
  unsigned posts_seen_ = 0;             // the notification count at the last check
  bool planted_ = false;  // self-test: the leak was planted
  std::vector<sim::StatePair> hold_;  // self-test: the state the line must hold (Options::plant_hold)
  std::uint64_t digest_ = 0xCBF29CE484222325ull;
  core::ActionId undo_id_ = core::ActionId::None;
  RunStats stats_;
};

std::string hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

unsigned env_number(const char* name, unsigned fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long n = std::strtoul(v, &end, 10);
  return (*end == '\0' && n > 0) ? static_cast<unsigned>(n) : fallback;
}

// ---- shrinking (B2) -----------------------------------------------------------------------------------------------------------------------------------------

struct Shrunk {
  std::vector<Input> lines;
  unsigned replays = 0;
  bool reproduced = false;  // the inputs, replayed on a fresh run, find the invariant again (the walk's own history does for every invariant but `undo`, which a burst compares)
  bool capped = false;      // stopped by the replay cap: still a failing script, maybe not the shortest
};

constexpr unsigned kMaxReplays = 600;

// Deletes inputs from the failing run while the same invariant still fails: halves first (a chunk of half the run, a quarter, ... down to one line, each chunk tried once), then one line at a time until a
// whole pass deletes nothing. A script is cut at the line where the invariant is found, so every accepted candidate ends on the failing line. The result is 1-minimal: no single line can go.
// A finding that is a comparison of two instants of the run (a metamorphic relation, a limit fixture): not shrunk.
bool compares_instants(const std::string& invariant) { return invariant.compare(0, 5, "meta_") == 0 || invariant.compare(0, 8, "exhaust_") == 0; }

Shrunk shrink(const Options& o, const Finding& f) {
  Shrunk r;
  const auto fails = [&](std::vector<Input>& candidate) {
    if (r.replays >= kMaxReplays) {
      r.capped = true;
      return false;
    }
    ++r.replays;
    Run run(o, f.seed);
    std::string error;
    std::vector<Finding> sink;
    if (!run.start(error)) {
      return false;
    }
    const int at = run.replay(candidate, f.match.empty() ? f.invariant : f.match, sink);
    if (at < 0) {
      return false;
    }
    candidate.resize(static_cast<std::size_t>(at) + 1u);
    return true;
  };
  std::vector<Input> cur = f.lines;
  if (cur.empty() || !fails(cur)) {
    r.lines = f.lines;
    return r;
  }
  r.reproduced = true;
  const auto without = [](const std::vector<Input>& v, std::size_t from, std::size_t n) {
    std::vector<Input> out(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(from));
    out.insert(out.end(), v.begin() + static_cast<std::ptrdiff_t>(std::min(v.size(), from + n)), v.end());
    return out;
  };
  for (std::size_t chunk = cur.size() / 2; chunk >= 1; chunk /= 2) {
    for (std::size_t i = 0; i < cur.size();) {
      std::vector<Input> cand = without(cur, i, chunk);
      if (!cand.empty() && fails(cand)) {
        cur = cand;
      } else {
        i += chunk;
      }
    }
  }
  for (bool changed = true; changed && !r.capped;) {
    changed = false;
    for (std::size_t i = cur.size(); i-- > 0;) {
      if (i >= cur.size()) {
        continue;
      }
      std::vector<Input> cand = without(cur, i, 1);
      if (!cand.empty() && fails(cand)) {
        cur = cand;
        changed = true;
      }
    }
  }
  r.lines = cur;
  return r;
}

std::string one_line(std::string text) {
  for (char& c : text) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return text;
}

// The card's folder as `card <name>` says it, and the folder `--cards` names for it.
std::filesystem::path card_path(const Options& o) {
  std::filesystem::path p(o.card);
  return p.filename().empty() ? p.parent_path() : p;
}
std::string replay_command(const Options& o, const std::string& file) {
  return "o3sim --scenario " + file + " --cards " + card_path(o).parent_path().string();
}

// The scenario file of a finding: `document`, `card <the card's folder>`, the inputs, the assertion. `reduced_from` is the length of the walk's own history.
std::string scenario_text(const Options& o, const Finding& f, const Shrunk& sh, const std::string& file) {
  std::string t;
  t += "# OS-QANET B2: a failing walk of the random user (tests/qanet/random_user.cpp), seed " + std::to_string(f.seed) + ", invariant " + f.invariant + ", found at step " + std::to_string(f.step) + ".\n";
  t += sh.reproduced ? "# Reduced from " + std::to_string(f.lines.size()) + " to " + std::to_string(sh.lines.size()) + " commands (" + std::to_string(sh.replays) + " replays" + (sh.capped ? ", stopped at the cap" : "") + ").\n"
                     : "# Not reduced (" + std::string(f.invariant == "undo" || compares_instants(f.invariant) ? "an undo burst, a metamorphic relation or a limit fixture compares the document before and after its steps, which a list of inputs does not say" : "the inputs alone do not find it again") + "): all " + std::to_string(f.lines.size()) + " commands of the walk.\n";
  t += "# why: " + one_line(f.why) + "\n";
  t += "# replay: " + replay_command(o, file) + "\n";
  t += "# walk:   o3_qanet_random_user --card " + o.card + " --seed " + std::to_string(f.seed) + " --seeds 1 --steps " + std::to_string(o.steps) + (o.plant_hold.empty() ? "" : " --plant-hold '" + o.plant_hold + "'") +
       (o.rules_every != 0u ? " --rules-every " + std::to_string(o.rules_every) : std::string()) + (o.plant_r1 ? " --plant-r1" : "") + (o.plant_r2 ? " --plant-r2" : "") + (o.plant_r3 ? " --plant-r3" : "") +
       (o.meta ? " --meta" + (o.meta_count != 10u ? " --meta-count " + std::to_string(o.meta_count) : std::string()) : std::string()) + "\n";
  t += "document\ncard " + card_path(o).filename().string() + "\n";
  // A rule finding's mark goes before the line that begins the unit that failed (the last one: the script ends at its end).
  std::size_t mark_at = sh.lines.size();
  if (!f.mark.empty()) {
    for (std::size_t i = sh.lines.size(); i-- > 0;) {
      if (sh.lines[i].begin) {
        mark_at = i;
        break;
      }
    }
  }
  for (std::size_t i = 0; i < sh.lines.size(); ++i) {
    if (i == mark_at) {
      t += "mark model " + f.mark + "\n";
    }
    t += sh.lines[i].line + "\n";
  }
  if (!f.tail.empty() && (f.mark.empty() || mark_at < sh.lines.size())) {  // what the grammar says of the invariant, recorded with the finding (the walk knows the oracle's answer and the burst's mark)
    t += f.tail + "\n";
  } else {
    t += "# no assertion: " + f.invariant + " reads what a scenario cannot (see the walk command above)\n";
  }
  return t;
}

// Shrinks `f`, writes `<dir>/found_<seed>_<invariant>.txt` and prints where it is, the one-line repro and the script. The path ("" when nothing was written) and the result are for the caller.
std::string write_found(const Options& o, const Finding& f, Shrunk& out) {
  out = f.invariant == "undo" || compares_instants(f.invariant) ? Shrunk() : shrink(o, f);  // (a comparison of two instants: a list of inputs alone does not say it)
  if (!out.reproduced) {
    out.lines = f.lines;
  }
  std::error_code ec;
  std::filesystem::create_directories(o.found_dir, ec);
  static std::set<std::string> written;  // a seed can have several rule findings: one file each
  std::string path = (std::filesystem::path(o.found_dir) / ("found_" + std::to_string(f.seed) + "_" + f.invariant + ".txt")).string();
  for (unsigned n = 2; !written.insert(path).second; ++n) {
    path = (std::filesystem::path(o.found_dir) / ("found_" + std::to_string(f.seed) + "_" + f.invariant + "_" + std::to_string(n) + ".txt")).string();
  }
  const std::string text = scenario_text(o, f, out, path);
  std::ofstream file(path, std::ios::binary);
  file << text;
  if (!file) {
    std::fprintf(stderr, "o3_qanet_random_user: cannot write %s\n", path.c_str());
    return std::string();
  }
  std::printf("  shrunk: %s: %zu -> %zu commands in %u replays%s\n", f.invariant.c_str(), f.lines.size(), out.lines.size(), out.replays, out.reproduced ? "" : " (not reduced)");
  std::printf("  scenario: %s\n", path.c_str());
  std::printf("  repro: %s\n", replay_command(o, path).c_str());
  std::printf("  ----- %s (a remote run leaves the file on the remote machine: this is its text)\n%s  -----\n", path.c_str(), text.c_str());
  return path;
}

// ---- the self-tests ------------------------------------------------------------------------------------------------------------------------------------------

int expect(bool ok, const char* what) {
  std::printf("selftest %-72s %s\n", what, ok ? "ok" : "FAILED");
  return ok ? 0 : 1;
}

int selftest(const Options& base) {
  int bad = 0;
  std::string error;
  // 1. Same seed, same run: the final digest, twice.
  {
    Options o = base;
    o.steps = 70;
    std::vector<Finding> a;
    std::vector<Finding> b;
    Run r1(o, 7);
    Run r2(o, 7);
    bad += expect(r1.start(error) && r2.start(error), "the card starts");
    r1.go(a);
    r2.go(b);
    bad += expect(r1.stats().digest == r2.stats().digest && r1.stats().ticks == r2.stats().ticks && a.size() == b.size(), "the same seed is the same run (digest, ticks, findings)");
    Run r3(o, 8);
    std::vector<Finding> c;
    (void)r3.start(error);
    r3.go(c);
    bad += expect(r3.stats().digest != r1.stats().digest, "another seed is another run");
  }
  // 2. A planted fault (a directory the harness opens and never closes) is found, as dir_open, after the step that planted it.
  {
    Options o = base;
    o.steps = 110;
    o.plant_leak_at = 30;
    std::vector<Finding> f;
    Run r(o, 3);
    (void)r.start(error);
    r.go(f);
    const Finding* hit = nullptr;
    for (const Finding& x : f) {
      if (x.invariant == "dir_open") {
        hit = &x;
      }
    }
    bad += expect(hit != nullptr, "a planted directory leak is found (dir_open)");
    bad += expect(hit != nullptr && hit->step >= 30 && hit->step <= 70, "... within 40 steps of the one that planted it (it is looked for at rest: no job runs, no scan is Scanning)");
    Options clean = base;
    clean.steps = 110;
    std::vector<Finding> g;
    Run c(clean, 3);
    (void)c.start(error);
    c.go(g);
    bool none = true;
    for (const Finding& x : g) {
      none = none && x.invariant != "dir_open";
    }
    bad += expect(none, "the same seed without the plant has no dir_open");
  }
  // 3. The baseline: parse, compare, fresh and stale lines.
  {
    Baseline b;
    bad += expect(parse_baseline("# steps=300\nlisting_empty | SAMPLE_LOAD | tap sampleload | 12@40 | the card lists nothing | OS-BUG-NEWLIST\n\nundo | PATTERN | burst | 15@9 | digest moved | new\n", b, error) && b.lines.size() == 2 && b.steps == 300u, "the baseline parses");
    Baseline broken;
    bad += expect(!parse_baseline("listing_empty | SAMPLE_LOAD | tap f1 | twelve | y | z\n", broken, error), "a line with no seed@step is refused");
    std::vector<Finding> f;
    Finding x;
    x.invariant = "listing_empty";
    x.screen = "SAMPLE_LOAD";
    x.action = "tap sampleload";
    x.seed = 99;  // another seed, the same key: still the known finding
    x.step = 44;
    f.push_back(x);
    Finding y;
    y.invariant = "dir_open";
    y.seed = 12;
    f.push_back(y);
    Verdict v = compare(b, f, 1, 20, 300, true);
    bad += expect(v.fresh.size() == 1u && v.stale.size() == 1u, "a finding of a known key is known whatever its seed; an unknown key is fresh; a covered line that did not occur is stale");
    Baseline b2 = b;
    for (BaselineLine& l : b2.lines) {
      l.seen = false;
    }
    Verdict w = compare(b2, f, 100, 20, 300, true);
    bad += expect(w.stale.empty(), "a line of a seed this run did not cover is not stale");
    Baseline b3 = b;
    for (BaselineLine& l : b3.lines) {
      l.seen = false;
    }
    Verdict z = compare(b3, std::vector<Finding>(), 1, 20, 500, true);
    bad += expect(z.stale.empty(), "a baseline made with another step count proves no absence");
    bad += expect(in_baseline(b, x) && !in_baseline(b, y), "a finding of a known key is not shrunk, an unknown one is");
    // D1 integration: a run that does not judge the rules (the FAT32 image's) cannot have found a rule's line: its absence is not stale there, a new finding is fresh in every run.
    Baseline r;
    bad += expect(parse_baseline("# steps=300\nR1 | SAMPLE_LOAD | tap f1 | 1@113 | a WAV load | new\nundo | PATTERN | burst | 15@9 | digest moved | new\n", r, error), "a baseline with a rule line parses");
    Verdict no_rules = compare(r, std::vector<Finding>(), 1, 20, 300, false);
    bad += expect(no_rules.stale.size() == 1u && no_rules.stale[0].compare(0, 4, "undo") == 0, "a run with no rules does not call a rule line stale, and still calls the others");
    Baseline r2 = r;
    for (BaselineLine& l : r2.lines) {
      l.seen = false;
    }
    bad += expect(compare(r2, std::vector<Finding>(), 1, 20, 300, true).stale.size() == 2u, "a run with the rules calls both stale");
    Finding nf;
    nf.invariant = "R1";
    nf.screen = "WAVE";
    nf.action = "tap f1";
    bad += expect(compare(r, std::vector<Finding>{nf}, 1, 20, 300, false).fresh.size() == 1u, "a new rule finding fails a run that does not judge the rules too");
    // C4: the lines of the audio probe, and those a walk with the engine alone reaches, are owned by the runs that have it (the FAT32 image's walk has not).
    Baseline au;
    bad += expect(parse_baseline("# steps=300\naudio_probe | undo | pad:fresh:shape | 3@150 | shape 0.00 | OS-BUG-SLICEPITCH\njob_stuck | FILE | tap f1 | 2@201 | [audio walk] a card job runs for 20002 ticks | new\nundo | PATTERN | burst | 1@9 | digest moved | new\n", au, error), "a baseline with audio lines parses");
    bad += expect(compare(au, std::vector<Finding>(), 1, 20, 300, true, false).stale.size() == 1u, "a run without the engine calls neither the probe's lines nor an engine-only line stale");
    for (BaselineLine& l : au.lines) {
      l.seen = false;
    }
    bad += expect(compare(au, std::vector<Finding>(), 1, 20, 300, true, true).stale.size() == 3u, "a run with the engine calls all three stale");
  }
  // 4. A replay looks where the walk looked: a state that holds only between two inputs of a step is not a failure, the same state at the end of a step is.
  {
    Options o = base;
    o.plant_hold = "screen=~^[^F]";  // fails on File
    std::vector<Finding> sink;
    const std::vector<Input> mid = {{"tap file", false}, {"tap pattern", true}};
    const std::vector<Input> end = {{"tap file", true}, {"tap pattern", true}};
    Run a(o, 1);
    Run b(o, 1);
    bad += expect(a.start(error) && a.replay(mid, "planted_state", sink) == -1 && sink.empty(), "a state that holds only inside a step is not found");
    bad += expect(b.start(error) && b.replay(end, "planted_state", sink) == 0 && !sink.empty(), "the same state at the end of a step is found, at that line");
  }
  // 5. The written scenario ends with the assertion of its invariant (the grammar: scenario.h), and it parses.
  {
    Options o = base;
    o.card = "/x/qanet_card";
    Finding f;
    f.invariant = "undo";
    f.lines = {{"wait 1", true}, {"mark model b1", false}, {"tap pattern", false}, {"wait 1", true}};
    f.tail = "assert model digest=@b1";
    Shrunk whole;
    whole.lines = f.lines;
    std::string text = scenario_text(o, f, whole, "found.txt");
    std::vector<sim::ScenarioCommand> cmds;
    bad += expect(text.find("mark model b1\ntap pattern\nwait 1\nassert model digest=@b1\n") != std::string::npos && sim::parse_scenario(text, cmds, error), "an undo scenario: the mark, the inputs, the assertion; it parses");
    f.invariant = "listing_empty";
    f.tail = kListsSomething;
    text = scenario_text(o, f, whole, "found.txt");
    cmds.clear();
    bad += expect(text.find("wait 1\nwait 3000\nassert state sel=~^[^-]\n") != std::string::npos && sim::parse_scenario(text, cmds, error), "a listing scenario ends with the wait and `assert state sel=~^[^-]`; it parses");
    f.invariant = "job_stuck";
    f.tail.clear();
    text = scenario_text(o, f, whole, "found.txt");
    bad += expect(text.find("# no assertion: job_stuck") != std::string::npos, "job_stuck stays input-only and says so");
  }
  return bad == 0 ? 0 : 1;
}

// C4's proof: a scripted walk (inputs of the kind a walk gives) is probed at its end, as the walk's hook does, and the probe's edit session makes the edits `mode1,even,undo` on a WAV of the card. With a pad path
// that is planted 25 cents sharp after the undo (the settled pad's render is detuned in the probe, as a fault of the pad path would be) the probe finds it, in the walk, as `audio_probe | undo | pad:settled:pitch`;
// without the plant the settled pad and the Pattern step agree (no `pad:settled` finding at all). The probe's other findings (the pad straight after an edit) are the ones the baseline lists.
int probe_selftest(const Options& base) {
  int bad = 0;
  const char* const script[] = {"tap pattern", "wait 50", "pad 3", "wait 150", "tap sampleplay", "wait 100", "tap pattern", "wait 30"};
  const auto probe = [&](const qanet_audio::ProbeOptions& po, std::vector<Finding>& out, RunStats& st) {
    Options o = base;
    o.audio_probe = true;
    o.steps = 1;
    Run run(o, 7);
    std::string error;
    if (!run.start(error)) {
      std::printf("selftest probe: %s\n", error.c_str());
      return false;
    }
    std::vector<Input> lines;
    for (const char* l : script) {
      lines.push_back(Input{l, true});
    }
    std::vector<Finding> sink;
    if (run.replay(lines, std::string(), sink) == -2) {
      return false;
    }
    run.probe_point(0, std::string(), true, po);
    out = sink;
    st = run.stats();
    return true;
  };
  qanet_audio::ProbeOptions plain;
  plain.session_edits = "mode1,even,undo";  // (the first of the six programs the seeds are dealt)
  qanet_audio::ProbeOptions planted = plain;
  planted.plant_detune_cents = 25.0;
  planted.plant_label = "undo";
  std::vector<Finding> a, b;
  RunStats sa, sb;
  bad += expect(probe(planted, a, sa), "the scripted walk is probed (planted detune after the undo)");
  bad += expect(probe(plain, b, sb), "the scripted walk is probed (no plant)");
  const auto has = [](const std::vector<Finding>& v, const char* edit, const char* action, const char* why_prefix) {
    for (const Finding& f : v) {
      if (f.invariant == "audio_probe" && f.screen == edit && f.action == action && f.why.compare(0, std::strlen(why_prefix), why_prefix) == 0) {
        return true;
      }
    }
    return false;
  };
  const auto settled = [](const std::vector<Finding>& v) {
    std::string s;
    for (const Finding& f : v) {
      if (f.action.compare(0, 11, "pad:settled") == 0) {
        s += " " + f.screen + "/" + f.action;
      }
    }
    return s;
  };
  std::string variants;
  for (const Finding& f : b) {
    variants += "\n    " + f.screen + " | " + f.action + " | " + f.why;
  }
  std::printf("selftest probe: %u probes (%u with a Pattern step), %u skipped, findings of the unplanted run:%s\n", sb.probes, sb.probes_stepped, sb.probes_skipped, variants.empty() ? " none" : variants.c_str());
  bad += expect(sb.probes >= 3u && sb.probes_stepped >= 1u, "the session probes the load, the loop, the chop and the undo (at least one against a Pattern step)");
  bad += expect(has(a, "undo", "pad:settled:pitch", "pitch +2"), "a planted detune after an undo is found mid-walk (audio_probe | undo | pad:settled:pitch)");
  bad += expect(settled(b).empty(), ("no plant: the settled pad and the Pattern step agree" + (settled(b).empty() ? std::string() : " (found:" + settled(b) + ")")).c_str());
  bad += expect(!has(b, "undo", "pad:settled:pitch", ""), "no plant: nothing is found after the undo on the settled pad");
  return bad == 0 ? 0 : 1;
}

}  // namespace

// B2's proof: a fault planted in the state (the Pattern cursor on an FX column, a state a walk reaches first at step 265 of seed 2) is found by the walk, shrunk to a few commands, written as a scenario, and that
// scenario fails under the real `o3sim` with the message the harness gave. Every single command of the result is needed (1-minimal).
constexpr const char* kPlantHold = "col=~^[^F]";
constexpr std::uint64_t kPlantSeed = 2;

int shrink_selftest(const Options& base) {
  int bad = 0;
  if (base.found_dir.empty() || base.o3sim.empty()) {
    std::fprintf(stderr, "--shrink-selftest needs --found-dir and --o3sim\n");
    return 2;
  }
  Options o = base;
  o.steps = 300;
  o.plant_hold = kPlantHold;
  std::string error;
  std::vector<Finding> found;
  Run walk(o, kPlantSeed);
  if (!walk.start(error)) {
    std::fprintf(stderr, "shrink selftest: %s\n", error.c_str());
    return 2;
  }
  walk.go(found);
  const Finding* hit = nullptr;
  for (const Finding& f : found) {
    if (f.invariant == "planted_state") {
      hit = &f;
    }
  }
  bad += expect(hit != nullptr, "the planted fault is found by the walk (planted_state)");
  bad += expect(hit != nullptr && hit->step >= 240u, "... at step 240 or later");
  if (hit == nullptr) {
    return 1;
  }
  Shrunk sh;
  const std::string path = write_found(o, *hit, sh);
  bad += expect(!path.empty() && sh.reproduced, "the inputs find it again on a fresh run");
  bad += expect(!sh.lines.empty() && sh.lines.size() < 15u, "it shrinks to fewer than 15 commands");
  std::printf("selftest: %zu commands of the walk -> %zu, %u replays\n", hit->lines.size(), sh.lines.size(), sh.replays);
  // 1-minimal: without any one command the invariant is not found.
  bool minimal = true;
  for (std::size_t i = 0; i < sh.lines.size(); ++i) {
    std::vector<Input> cand = sh.lines;
    cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(i));
    Run r(o, kPlantSeed);
    std::vector<Finding> sink;
    minimal = minimal && r.start(error) && r.replay(cand, "planted_state", sink) < 0;
  }
  bad += expect(minimal, "every command of the result is needed");
  // The harness's own message for the result, then the scenario under o3sim.
  std::vector<Finding> sink;
  Run again(o, kPlantSeed);
  (void)again.start(error);
  bad += expect(again.replay(sh.lines, "planted_state", sink) == static_cast<int>(sh.lines.size()) - 1 && !sink.empty(), "the result fails on its last command");
  const std::string log = path + ".log";
  const int status = std::system(("'" + o.o3sim + "' --scenario '" + path + "' --cards '" + card_path(o).parent_path().string() + "' > '" + log + "' 2>&1").c_str());
  std::string out;
  (void)read_file(log, out);
  bad += expect(WIFEXITED(status) && WEXITSTATUS(status) == 2, "o3sim replays the scenario and exits 2");
  const std::string want = std::string("assertion failed: assert state ") + kPlantHold + ": " + (sink.empty() ? std::string("?") : sink[0].why);
  bad += expect(out.find(want) != std::string::npos, "... with the message of the harness");
  if (out.find(want) == std::string::npos) {
    std::printf("  wanted: %s\n  o3sim said: %s\n", want.c_str(), out.c_str());
  }
  return bad == 0 ? 0 : 1;
}

// Runs `text` as a scenario under the real o3sim and returns its exit status (-1 when it did not exit).
int run_o3sim(const Options& o, const std::string& name, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(o.found_dir, ec);
  const std::string path = (std::filesystem::path(o.found_dir) / name).string();
  {
    std::ofstream f(path, std::ios::binary);
    f << text;
  }
  const int status = std::system(("'" + o.o3sim + "' --scenario '" + path + "' --cards '" + card_path(o).parent_path().string() + "' > '" + path + ".log' 2>&1").c_str());
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// B2 fold's proof: the assertions a written scenario ends with fail under the real o3sim when the bug is there and pass when it is not, with no product fault needed. Each has a positive control (the state
// the assertion wants: exit 0) and a negative one (the state the bug leaves: exit 2).
int assert_selftest(const Options& base) {
  int bad = 0;
  if (base.found_dir.empty() || base.o3sim.empty()) {
    std::fprintf(stderr, "--assert-selftest needs --found-dir and --o3sim\n");
    return 2;
  }
  const std::string head = "document\ncard " + card_path(base).filename().string() + "\n";
  // 1. The listing: Sample load lists the card's root (an entry under the cursor) once the scan is done; with the card pulled it lists nothing.
  bad += expect(run_o3sim(base, "assert_listing_ok.txt", head + "tap sampleload\n" + kListsSomething + "\n") == 0, "listing: Sample load on the card, then the written assertion: o3sim exits 0");
  bad += expect(run_o3sim(base, "assert_listing_bad.txt", head + "tap sampleload\nwait 3000\ncard pull\nwait 100\nassert state sel=~^[^-]\n") == 2, "listing: the same with the card pulled before the assertion: o3sim exits 2");
  // 2. Undo: the history of a walk up to its first conclusive burst, with the assertion the finding would carry; without the undos the document is not where the mark left it.
  Options o = base;
  RunStats found;
  std::string error;
  for (std::uint64_t seed = 1; seed <= 6 && found.burst_script.empty(); ++seed) {
    std::vector<Finding> sink;
    Run r(o, seed);
    if (r.start(error)) {
      r.go(sink);
      found = r.stats();
    }
  }
  bad += expect(!found.burst_script.empty(), "a walk has a conclusive undo burst to take the history of");
  if (!found.burst_script.empty()) {
    std::string good = head;
    std::string dropped = head;
    unsigned undos = 0;
    for (const Input& l : found.burst_script) {
      good += l.line + "\n";
      if (l.line == "action edit.undo") {
        ++undos;
      } else {
        dropped += l.line + "\n";
      }
    }
    const std::string a = undo_tail(found.burst_label) + "\n";
    std::printf("selftest undo: %zu inputs of the walk, %u undos, mark %s\n", found.burst_script.size(), undos, found.burst_label.c_str());
    bad += expect(undos > 0u && run_o3sim(o, "assert_undo_ok.txt", good + a) == 0, "undo: the burst's history with its assertion: o3sim exits 0");
    bad += expect(run_o3sim(o, "assert_undo_bad.txt", dropped + a) == 2, "undo: the same without the undos: o3sim exits 2");
  }
  return bad == 0 ? 0 : 1;
}

// B5's proof: every relation is reached (ended in a compare) on the unplanted walk, which finds nothing; with a field the save does not write (one more edit between the save and OPEN, so OPEN cannot give
// the document back) the save-and-OPEN relation finds it, and the scenario that is written for it fails under the real o3sim on its assertion (exit 2).
// The run's verdict on --min-meta and --min-save-open: "" when both hold, else the line to print. The save then OPEN count is the compares that carried a fine tune (meta_tuned_ok): a walk whose
// plant stopped reaching FINE TUNE would otherwise pass them with a document the writer cannot lose anything from. Either minimum without --meta is an error (it would pass without running a relation).
std::string meta_shortfall(const Options& o, unsigned meta_total, unsigned tuned_compares, unsigned exhaust_reached = 0) {
  if (!o.meta && (o.min_meta != 0u || o.min_save_open != 0u)) {
    return "--min-meta and --min-save-open need --meta: without it no relation runs and the minimum would pass silently";
  }
  if (!o.exhaust && o.min_exhaust != 0u) {
    return "--min-exhaust needs --exhaust: without it no fixture runs and the minimum would pass silently";
  }
  if (o.exhaust && exhaust_reached < o.min_exhaust) {
    return "TOO FEW FIXTURES: " + std::to_string(exhaust_reached) + " limit fixtures reached their limit, the run wants " + std::to_string(o.min_exhaust) + " (B8 was not exercised)";
  }
  if (o.meta && meta_total < o.min_meta) {
    return "TOO FEW RELATIONS: " + std::to_string(meta_total) + " metamorphic relations ended in a compare, the run wants " + std::to_string(o.min_meta) + " (B5 was not exercised)";
  }
  if (o.meta && tuned_compares < o.min_save_open) {
    return "TOO FEW SAVE+OPEN: " + std::to_string(tuned_compares) + " saves with a fine tune set then OPENs ended in a compare, the run wants " + std::to_string(o.min_save_open) +
           " (the relation of B5 that finds a field Save loses was not exercised)";
  }
  return std::string();
}

// B8's proof. Each fixture reaches its limit in a clean walk's seeds and finds nothing (the undo cap: the ring dropped units and the undo reached what was kept; the sample-edit cap: the history held kHistory steps
// after more edits than that), and a defect planted at the limit is found: at the undo cap a unit that goes unrecorded; at the sample-edit cap a side effect on the pattern that the full history used to credit as
// an undo unit (the blind spot, rules.cpp unit_recorded: the same defect planted before B8 passed).
// The fixtures [first, end): the walk's three with 3 seeds each (--exhaust-selftest), or the two that run on a simulator of their own with one seed each (--exhaust-fresh-selftest: the walk is not theirs, a seed only
// draws the pads, and each run costs seconds).
int exhaust_selftest(const Options& base, unsigned first, unsigned end, std::uint64_t seeds) {
  int bad = 0;
  std::string error;
  Options o = base;
  o.exhaust = true;
  o.steps = first >= kFixtureFirstFresh ? 1 : 24;
  for (unsigned fx = first; fx < end; ++fx) {
    Options one = o;
    one.exhaust_only = static_cast<int>(fx);
    unsigned ok = 0;
    std::string clean;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
      std::vector<Finding> f;
      Run r(one, seed);
      if (!r.start(error)) {
        std::printf("selftest exhaust: %s\n", error.c_str());
        return 1;
      }
      r.go(f);
      ok += r.stats().exh_ok[fx];
      for (const Finding& x : f) {
        if (x.invariant.compare(0, 8, "exhaust_") == 0) {
          clean += " " + x.invariant + "@" + std::to_string(seed) + ": " + x.why;
        }
      }
    }
    std::printf("selftest exhaust: %s reached its limit %u times in %llu seeds\n", kExhaustNames[fx], ok, static_cast<unsigned long long>(seeds));
    bad += expect(ok >= 1u, (std::string("the fixture ") + kExhaustNames[fx] + " reaches its limit").c_str());
    bad += expect(clean.empty(), (std::string("the unplanted fixture ") + kExhaustNames[fx] + " finds nothing broken").c_str());
    if (!clean.empty()) {
      std::printf("selftest exhaust: findings of the unplanted fixture:%s\n", clean.c_str());
    }
    Options p = one;
    p.plant_exhaust = true;
    const std::string inv = std::string("exhaust_") + kExhaustNames[fx];
    bool hit = false;
    for (std::uint64_t seed = 1; seed <= seeds && !hit; ++seed) {
      std::vector<Finding> f;
      Run r(p, seed);
      if (!r.start(error)) {
        return 1;
      }
      r.go(f);
      for (const Finding& x : f) {
        hit = hit || (x.invariant == inv && x.why.find("(planted)") != std::string::npos && (fx >= kFixtureFirstFresh || x.why.compare(0, 2, "R1") == 0));
      }
    }
    bad += expect(hit, (std::string("a defect planted at the limit of ") + kExhaustNames[fx] + (fx >= kFixtureFirstFresh ? " is found" : " is found by rule 1")).c_str());
  }
  return bad == 0 ? 0 : 1;
}

int meta_selftest(const Options& base) {
  int bad = 0;
  {  // the verdict on the minimums: a run that never reached the fine tune, or that asked for a minimum without --meta, fails
    Options v = base;
    v.meta = true;
    v.min_meta = 1;
    v.min_save_open = 1;
    bad += expect(meta_shortfall(v, 5, 1).empty(), "the minimums hold: no shortfall");
    bad += expect(meta_shortfall(v, 5, 0).compare(0, 18, "TOO FEW SAVE+OPEN:") == 0, "no save then OPEN with a fine tune compared: TOO FEW SAVE+OPEN");
    bad += expect(meta_shortfall(v, 0, 1).compare(0, 14, "TOO FEW RELATI") == 0, "no relation compared: TOO FEW RELATIONS");
    v.meta = false;
    bad += expect(!meta_shortfall(v, 0, 0).empty(), "--min-save-open without --meta is an error");
    v.meta = true;
    v.min_exhaust = 1;
    bad += expect(!meta_shortfall(v, 5, 1, 1).empty(), "--min-exhaust without --exhaust is an error");
    v.exhaust = true;
    bad += expect(meta_shortfall(v, 5, 1, 1).empty(), "a fixture reached its limit: no shortfall");
    bad += expect(meta_shortfall(v, 5, 1, 0).compare(0, 16, "TOO FEW FIXTURES") == 0, "no fixture reached its limit: TOO FEW FIXTURES");
  }
  std::string error;
  Options o = base;
  o.meta = true;
  o.meta_count = 12;
  o.steps = 48;
  std::string clean;  // the relations' findings of the unplanted walks
  for (unsigned k = 0; k < kMetaCount; ++k) {
    Options one = o;
    one.meta_only = static_cast<int>(k);
    unsigned ok = 0;
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
      std::vector<Finding> f;
      Run r(one, seed);
      if (!r.start(error)) {
        std::printf("selftest meta: %s\n", error.c_str());
        return 1;
      }
      r.go(f);
      ok += r.stats().meta_ok[k];
      for (const Finding& x : f) {
        if (x.invariant.compare(0, 5, "meta_") == 0) {
          clean += " " + x.invariant + "@" + std::to_string(seed) + ":" + std::to_string(x.step);
        }
      }
    }
    std::printf("selftest meta: %s compared %u times in 3 seeds\n", kMetaNames[k], ok);
    bad += expect(ok >= 1u, (std::string("the relation ") + kMetaNames[k] + " is reached (ends in a compare)").c_str());
  }
  bad += expect(clean.empty(), "the unplanted walks find no relation broken");
  if (!clean.empty()) {
    std::printf("selftest meta: findings of the unplanted walks:%s\n", clean.c_str());
  }
  Options p = o;
  p.plant_meta = true;
  p.meta_only = 0;
  const Finding* hit = nullptr;
  std::vector<Finding> found;
  for (std::uint64_t seed = 1; seed <= 4 && hit == nullptr; ++seed) {
    Run r(p, seed);
    if (!r.start(error)) {
      return 1;
    }
    found.clear();
    r.go(found);
    for (const Finding& x : found) {
      if (x.invariant == "meta_save_open") {
        hit = &x;
        break;
      }
    }
    if (hit != nullptr) {
      bad += expect(!hit->tail.empty() && hit->lines.size() > 4u, "the planted loss is found as meta_save_open, with its inputs and its assertion");
      if (!base.found_dir.empty() && !base.o3sim.empty()) {
        Shrunk sh;
        sh.lines = hit->lines;
        const std::string text = scenario_text(p, *hit, sh, "meta_planted.txt");
        bad += expect(run_o3sim(p, "meta_planted.txt", text) == 2, "the scenario written for it fails under o3sim (exit 2)");
      }
    }
  }
  bad += expect(hit != nullptr, "a field the save does not write is found by the save and OPEN relation");
  return bad == 0 ? 0 : 1;
}

// B4's proof: a rule-1 breach planted in the walk (the document changes with no unit in the first input after a sample slot holds audio, which a walk reaches at step 113 of seed 1) is found by the rules at that
// step, not before the slot was filled, shrunk to a few inputs, and written as a scenario.
// A rule's planted breach is found at the step the state it needs first stands (`trigger` reads that step from the walk's statistics), shrunk and written, and a walk cut before that step does not find it.
int plant_check(const Options& o, const char* rule, int (*trigger)(const RunStats&), const char* state) {
  int bad = 0;
  std::string error;
  std::vector<Finding> found;
  Run walk(o, 1);
  if (!walk.start(error)) {
    std::fprintf(stderr, "rules selftest: %s\n", error.c_str());
    return 2;
  }
  walk.go(found);
  const Finding* hit = nullptr;
  for (const Finding& f : found) {
    if (f.invariant == rule && f.why.find("(planted)") != std::string::npos) {
      hit = &f;
    }
  }
  const int at = trigger(walk.stats());
  bad += expect(at >= 0, (std::string("the walk reached the state the breach waits for (") + state + "): if not, the walk's stream moved (see the comment at the self-test)").c_str());
  bad += expect(at >= 0 && hit != nullptr, (std::string("the planted ") + rule + " breach is found (" + rule + ")").c_str());
  bad += expect(hit != nullptr && at >= 0 && static_cast<int>(hit->step) >= at, (std::string("... not before the step where ") + state).c_str());
  // Cut the walk before that step: the same breach is planted, and nothing triggers it.
  if (at > 0) {
    Options early = o;
    early.steps = static_cast<unsigned>(at);
    std::vector<Finding> none;
    Run r(early, 1);
    (void)r.start(error);
    r.go(none);
    bool planted = false;
    for (const Finding& f : none) {
      planted = planted || f.why.find("(planted)") != std::string::npos;
    }
    bad += expect(!planted, "a walk that stops before that state does not find it");
  }
  if (hit == nullptr) {
    return 1;
  }
  Shrunk sh;
  const std::string path = write_found(o, *hit, sh);
  bad += expect(!path.empty() && sh.reproduced, "the inputs find it again on a fresh run");
  bad += expect(!sh.lines.empty() && sh.lines.size() < 40u, "it shrinks to fewer than 40 commands");
  std::printf("selftest %s: %zu commands of the walk -> %zu, %u replays\n", rule, hit->lines.size(), sh.lines.size(), sh.replays);
  bool minimal = true;
  for (std::size_t i = 0; i < sh.lines.size(); ++i) {
    std::vector<Input> cand = sh.lines;
    cand.erase(cand.begin() + static_cast<std::ptrdiff_t>(i));
    Run r(o, 1);
    std::vector<Finding> sink;
    minimal = minimal && r.start(error) && r.replay(cand, hit->match, sink) < 0;
  }
  bad += expect(minimal, "every command of the result is needed");
  return bad == 0 ? 0 : 1;
}

int rules_selftest(const Options& base) {
  if (base.found_dir.empty()) {
    std::fprintf(stderr, "--rules-selftest needs --found-dir\n");
    return 2;
  }
  Options o = base;
  // FRAGILE BY DESIGN: the state the breach waits for (a sample slot filled by the walk's own inputs) is made by the walk, not planted, so the test depends on the walk's random stream: a change of the
  // stream (a new input kind, a new weight, a new wait) moves the step of the first fill, and it may then lie past the last step. 300 steps of seed 1 reach it at the time of writing (149 s under the
  // sanitizers; 2026-10-23, the first fill is late because the walk presses Shift + arrows and waits before keys too). If it fails with "no slot was filled", the stream moved: raise `steps` or the seed here.
  o.steps = 300;
  o.rules_every = 1;
  o.plant_r1 = true;
  o.stop_at_plant = true;
  return plant_check(o, "R1", [](const RunStats& s) { return s.first_fill_step < 0 ? -1 : s.first_fill_step + 1; }, "a sample slot was filled");  // (the breach is planted in the input after the fill)
}

// Rules 2 and 3 at every step (B4b): a silent key that appears only after the document changed mid-walk, and an out-of-span edit made after a selection that the walk made mid-walk.
int rules23_selftest(const Options& base) {
  if (base.found_dir.empty()) {
    std::fprintf(stderr, "--rules23-selftest needs --found-dir\n");
    return 2;
  }
  if (!base.rules_case.empty() && base.rules_case != "R2" && base.rules_case != "R3") {
    std::fprintf(stderr, "--rules-case is R2 or R3\n");
    return 2;
  }
  Options o = base;
  o.steps = 150;
  o.rules_every = 1;
  o.plant_r2 = true;
  o.stop_at_plant = true;
  int bad = 0;
  if (base.rules_case.empty() || base.rules_case == "R2") {  // (OS-OPS-CIPOLE: `--rules-case R2|R3` runs one rule, so that the two are ctests of their own and run side by side)
    bad += plant_check(o, "R2", [](const RunStats& s) { return s.first_change_step + 1; }, "the document first changed");
  }
  Options o3 = base;
  o3.steps = 150;
  o3.rules_every = 1;
  o3.plant_r3 = true;
  o3.stop_at_plant = true;
  if (base.rules_case.empty() || base.rules_case == "R3") {
    bad += plant_check(o3, "R3", [](const RunStats& s) { return s.first_sel_step; }, "a pattern selection first stood");
  }
  return bad == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  Options o;
  bool use_env = false;
  unsigned shard = 0;
  unsigned shards = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--card") {
      o.card = next();
    } else if (a == "--seed") {
      o.first_seed = std::strtoull(next(), nullptr, 10);
    } else if (a == "--seeds") {
      o.seeds = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--steps") {
      o.steps = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--baseline") {
      o.baseline = next();
    } else if (a == "--verbose") {
      o.verbose = true;
    } else if (a == "--env") {  // the three numbers are what the environment says, the flags before it being the defaults (the long runs on the remote machine set it, tools/remote_ci.sh passes it on)
      use_env = true;
    } else if (a == "--trace") {
      o.trace = true;
    } else if (a == "--found-dir") {
      o.found_dir = next();
    } else if (a == "--plant-hold") {
      o.plant_hold = next();
    } else if (a == "--o3sim") {
      o.o3sim = next();
    } else if (a == "--states") {
      o.states = true;
    } else if (a == "--min-keys") {
      o.min_keys = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--min-sel") {
      o.min_sel = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--min-conclusive") {
      o.min_conclusive = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--meta") {
      o.meta = true;
    } else if (a == "--meta-count") {
      o.meta_count =static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--min-meta") {
      o.min_meta = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--min-save-open") {
      o.min_save_open = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--exhaust") {
      o.exhaust = true;
    } else if (a == "--min-exhaust") {
      o.min_exhaust = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--exhaust-all") {
      o.exhaust_all = true;
    } else if (a == "--exhaust-selftest") {
      o.exhaust_selftest = true;
    } else if (a == "--exhaust-fresh-selftest") {
      o.exhaust_fresh_selftest = true;
    } else if (a == "--exhaust-only") {
      const std::string name = next();
      o.exhaust_only = -1;
      for (unsigned k = 0; k < kExhaustCount; ++k) {
        if (name == kExhaustNames[k]) {
          o.exhaust_only = static_cast<int>(k);
        }
      }
      if (o.exhaust_only < 0) {
        std::fprintf(stderr, "o3_qanet_random_user: --exhaust-only: no fixture called '%s'\n", name.c_str());
        return 2;
      }
    } else if (a == "--plant-exhaust") {
      o.plant_exhaust = true;
    } else if (a == "--meta-selftest") {
      o.meta_selftest = true;
    } else if (a == "--audio-probe") {
      o.audio_probe = true;
    } else if (a == "--audio-session") {
      o.audio_probe = true;
      o.audio_session = true;
    } else if (a == "--audio-probe-every") {
      o.probe_every = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--audio-probe-selftest") {
      o.probe_selftest = true;
    } else if (a == "--selftest") {
      o.selftest = true;
    } else if (a == "--shrink-selftest") {
      o.shrink_selftest = true;
    } else if (a == "--rules-selftest") {
      o.rules_selftest = true;
    } else if (a == "--rules-every") {
      o.rules_every = static_cast<unsigned>(std::strtoul(next(), nullptr, 10));
    } else if (a == "--overwrite-report") {
      o.overwrite_report = true;
    } else if (a == "--plant-r2") {
      o.plant_r2 = true;
    } else if (a == "--rules-case") {
      o.rules_case = next();
    } else if (a == "--stop-at-plant") {
      o.stop_at_plant = true;
    } else if (a == "--plant-r3") {
      o.plant_r3 = true;
    } else if (a == "--rules23-selftest") {
      o.rules23_selftest = true;
    } else if (a == "--plant-r1") {
      o.plant_r1 = true;
    } else if (a == "--assert-selftest") {
      o.assert_selftest = true;
    } else if (a == "--shard") {  // i/n: the run's seeds are cut into n slices of the range and this one runs slice i (1-based): the ctest shards
      const std::string s = next();
      shard = static_cast<unsigned>(std::strtoul(s.c_str(), nullptr, 10));
      const std::size_t slash = s.find('/');
      shards = slash == std::string::npos ? 0u : static_cast<unsigned>(std::strtoul(s.c_str() + slash + 1, nullptr, 10));
      if (shard < 1 || shards < 1 || shard > shards) {
        std::fprintf(stderr, "--shard wants i/n with 1 <= i <= n\n");
        return 2;
      }
    } else {
      std::fprintf(stderr, "usage: o3_qanet_random_user --card <dir> [--seed <first>] [--seeds <n>] [--steps <n>] [--shard i/n] [--env] [--baseline <file>] [--min-conclusive <n>] [--min-keys <n>] [--min-sel <n>] [--rules-every <n>] [--found-dir <dir>] [--audio-probe [--audio-probe-every <k>]] [--audio-session] [--verbose] [--trace] [--states] | --selftest --card <dir> | --audio-probe-selftest --card <dir> | --shrink-selftest|--assert-selftest|--rules-selftest --card <dir> --found-dir <dir> --o3sim <file>\n"
                           "       (--env: O3OS_QANET_SEED, O3OS_QANET_SEEDS and O3OS_QANET_STEPS replace the three numbers)\n");
      return 2;
    }
  }
  if (use_env) {
    o.first_seed = env_number("O3OS_QANET_SEED", static_cast<unsigned>(o.first_seed));
    o.seeds = env_number("O3OS_QANET_SEEDS", o.seeds);
    o.steps = env_number("O3OS_QANET_STEPS", o.steps);
  }
  if ((!o.meta && (o.min_meta != 0u || o.min_save_open != 0u)) || (!o.exhaust && o.min_exhaust != 0u)) {
    std::fprintf(stderr, "o3_qanet_random_user: %s\n", meta_shortfall(o, 0, 0, 0).c_str());
    return 2;
  }
  if (o.card.empty() || o.seeds == 0 || o.steps == 0) {
    std::fprintf(stderr, "o3_qanet_random_user: --card <dir>, seeds and steps are needed\n");
    return 2;
  }
  if (o.selftest) {
    return selftest(o);
  }
  if (o.probe_selftest) {
    return probe_selftest(o);
  }
  if (o.meta_selftest) {
    return meta_selftest(o);
  }
  if (o.exhaust_fresh_selftest) {
    return exhaust_selftest(o, kFixtureFirstFresh, kExhaustCount, 1);
  }
  if (o.exhaust_selftest) {
    return exhaust_selftest(o, 0, kFixtureFirstFresh, 3);
  }
  if (o.shrink_selftest) {
    return shrink_selftest(o);
  }
  if (o.assert_selftest) {
    return assert_selftest(o);
  }
  if (o.rules23_selftest) {
    return rules23_selftest(o);
  }
  if (o.rules_selftest) {
    return rules_selftest(o);
  }
  if (shards > 1) {  // the seeds are dealt out whole: slice i takes [first + (i-1)*n/shards, first + i*n/shards)
    const unsigned lo = static_cast<unsigned>(static_cast<std::uint64_t>(o.seeds) * (shard - 1) / shards);
    const unsigned hi = static_cast<unsigned>(static_cast<std::uint64_t>(o.seeds) * shard / shards);
    o.first_seed += lo;
    o.seeds = hi - lo;
  }

  Baseline base;
  if (!o.baseline.empty()) {
    std::string text;
    std::string error;
    if (!read_file(o.baseline, text) || !parse_baseline(text, base, error)) {
      std::fprintf(stderr, "o3_qanet_random_user: baseline %s: %s\n", o.baseline.c_str(), error.empty() ? "cannot be read" : error.c_str());
      return 2;
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<Finding> findings;
  RunStats sum;
  std::set<std::string> tuples;  // over all seeds
  unsigned conclusive = 0;
  unsigned session = 0;
  unsigned bursts = 0;
  unsigned burst_edits = 0;
  unsigned shrunk = 0;
  std::set<std::string> shrunk_keys;
  for (std::uint64_t s = o.first_seed; s < o.first_seed + o.seeds; ++s) {
    const auto t1 = std::chrono::steady_clock::now();
    const std::size_t before = findings.size();
    Run run(o, s);
    std::string error;
    if (!run.start(error)) {
      std::fprintf(stderr, "o3_qanet_random_user: seed %llu: %s\n", static_cast<unsigned long long>(s), error.c_str());
      return 2;
    }
    run.go(findings);
    const RunStats& st = run.stats();
    tuples.insert(st.tuples.begin(), st.tuples.end());
    sum.ticks += st.ticks;
    sum.new_projects += st.new_projects;
    sum.new_then_load += st.new_then_load;
    sum.loads_seen += st.loads_seen;
    sum.cards_pulled += st.cards_pulled;
    sum.questions += st.questions;
    sum.oracle_runs += st.oracle_runs;
    sum.rebinds += st.rebinds;
    sum.probe_points += st.probe_points;
    sum.probes += st.probes;
    sum.probes_stepped += st.probes_stepped;
    sum.probes_skipped += st.probes_skipped;
    for (unsigned k = 0; k < kMetaCount; ++k) {
      sum.meta_ok[k] += st.meta_ok[k];
      sum.meta_skip[k] += st.meta_skip[k];
    }
    sum.meta_tuned += st.meta_tuned;
    sum.meta_tuned_ok += st.meta_tuned_ok;
    for (unsigned k = 0; k < kExhaustCount; ++k) {
      sum.exh_ok[k] += st.exh_ok[k];
      sum.exh_skip[k] += st.exh_skip[k];
    }
    sum.exh_notes += st.exh_notes;
    sum.exh_units += st.exh_units;
    conclusive += st.bursts_conclusive;
    session += st.bursts_session;
    bursts += st.bursts;
    burst_edits += st.bursts_with_edits;
    shrunk += st.bursts_shrunk;
    sum.units_checked += st.units_checked;
    sum.units_session += st.units_session;
    sum.rule_findings += st.rule_findings;
    sum.slot_fills += st.slot_fills;
    sum.slot_overwrites += st.slot_overwrites;
    sum.slot_overwrites_flagged += st.slot_overwrites_flagged;
    sum.slot_overwrites_same += st.slot_overwrites_same;
    sum.keys_seen += st.keys_seen;
    sum.keys_judged += st.keys_judged;
    sum.keys_unquiet += st.keys_unquiet;
    sum.keys_excused += st.keys_excused;
    sum.keys_silent += st.keys_silent;
    sum.watchsel_seen += st.watchsel_seen;
    sum.watchsel_silent += st.watchsel_silent;
    sum.watchsel_watch += st.watchsel_watch;
    sum.sel_judged += st.sel_judged;
    sum.sel_units += st.sel_units;
    sum.sel_no_pattern += st.sel_no_pattern;
    for (const auto& kv : st.sel_by_action) {
      sum.sel_by_action[kv.first] += kv.second;
    }
    for (const auto& kv : st.key_by_action) {
      sum.key_by_action[kv.first] += kv.second;
    }
    for (const auto& kv : st.session_by_screen) {
      sum.session_by_screen[kv.first] += kv.second;
    }
    for (const auto& kv : st.judged_by_screen) {
      sum.judged_by_screen[kv.first] += kv.second;
    }
    if (o.verbose || findings.size() != before) {
      std::printf("seed %llu: %u steps, %u ticks, %zu findings, %zu distinct (screen, action, screen) tuples, digest %s, %lld ms\n", static_cast<unsigned long long>(s), o.steps, st.ticks, findings.size() - before,
                  st.tuples.size(), hex(st.digest).c_str(),
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count()));
    }
  }
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

  std::printf("\nfindings (invariant | screen | action | seed@step | why):\n");
  for (const Finding& f : findings) {
    std::printf("  %s | %s | %s | %llu@%u | %s\n", f.invariant.c_str(), f.screen.c_str(), f.action.c_str(), static_cast<unsigned long long>(f.seed), f.step, f.why.c_str());
    std::printf("    repro (%zu lines, `document`, `card qanet_card`, then): %s\n", static_cast<std::size_t>(std::count(f.repro.begin(), f.repro.end(), ';')) + 1u, f.repro.c_str());
    if (!o.found_dir.empty()) {
      if (in_baseline(base, f)) {  // a known finding is not worth 600 replays: its baseline line has the repro
        std::printf("  known (in the baseline): not shrunk\n");
      } else if (!shrunk_keys.insert(f.invariant + " | " + f.screen + " | " + f.action).second) {  // the first of a key in this run is shrunk and written; the others of it are the same finding
        std::printf("  same key as an earlier finding of this run: not shrunk\n");
      } else {
        Shrunk sh;
        (void)write_found(o, f, sh);
      }
    }
  }
  const Verdict v = compare(base, findings, o.first_seed, o.seeds, o.steps, o.rules_every != 0u, o.audio_probe);
  std::printf("\nqanet random user: seeds %llu..%llu, %u steps each, %llu ms; %zu findings (%zu not in the baseline, %zu baseline lines that did not occur)\n",
              static_cast<unsigned long long>(o.first_seed), static_cast<unsigned long long>(o.first_seed + o.seeds - 1), o.steps, static_cast<unsigned long long>(ms), findings.size(), v.fresh.size(),
              v.stale.size());
  if (o.verbose) {
    for (const std::string& t : tuples) {
      std::printf("  tuple %s\n", t.c_str());
    }
  }
  std::printf("tuples: %zu distinct (screen from | action | screen to) over the run\n", tuples.size());
  std::printf("coverage: %u File questions, %u ticks, %u NEW PROJECT strips (%u then a Sample load listing), %u Sample load listings, %u card pulls, %u oracle scans; undo bursts %u (%u edited, %u moved the session state and %u shortened the journal, not compared, %u conclusive)\n", sum.questions, sum.ticks,
              sum.new_projects, sum.new_then_load, sum.loads_seen, sum.cards_pulled, sum.oracle_runs, bursts, burst_edits, session, shrunk, conclusive);
  unsigned meta_total = 0;
  if (o.meta) {
    std::printf("metamorphic relations (B5, %u after the last step of each walk), compared (not set up or session state moved):", o.meta_count);
    for (unsigned k = 0; k < kMetaCount; ++k) {
      std::printf(" %s %u (%u);", kMetaNames[k], sum.meta_ok[k], sum.meta_skip[k]);
      meta_total += sum.meta_ok[k];
    }
    std::printf(" %u saves with a fine tune set (%u of them compared after OPEN)", sum.meta_tuned, sum.meta_tuned_ok);
    std::printf("\n");
  }
  unsigned exh_total = 0;
  if (o.exhaust) {
    std::printf("limit fixtures (B8, after the last step of each walk and of --meta), reached their limit (not reached):");
    for (unsigned k = 0; k < kExhaustCount; ++k) {
      std::printf(" %s %u (%u);", kExhaustNames[k], sum.exh_ok[k], sum.exh_skip[k]);
      exh_total += sum.exh_ok[k];
    }
    std::printf(" %u inserts or edits at or toward the limit\n", sum.exh_units);
    if (!sum.exh_notes.empty()) {
      std::printf("limit fixtures not reached:%s\n", sum.exh_notes.c_str());
    }
  }
  if (o.audio_probe) {
    std::printf("audio probe: %u steps rebound an instrument's descriptor, %u probe points (replays, every %u-th rebind and the middle of each walk), %u instruments played (%u against a Pattern step), %u not played\n", sum.rebinds,
                sum.probe_points, std::max(1u, o.probe_every), sum.probes, sum.probes_stepped, sum.probes_skipped);
  }
  if (o.rules_every != 0u) {
    std::printf("rule 2 (B4b): %u bound F keys and arrows seen (%u judged, %u excused by what the chain tells, %u not judged because something moved by itself in the wait before them, %u silent); rule 3: %u inputs judged (%u made with a pattern selection up, %u more not judged: the pattern in view did not exist yet); by action:", sum.keys_seen,
                sum.keys_judged, sum.keys_excused, sum.keys_unquiet, sum.keys_silent, sum.sel_judged, sum.sel_units, sum.sel_no_pattern);
    for (const auto& kv : sum.sel_by_action) {
      std::printf(" %s %u;", kv.first.c_str(), kv.second);
    }
    std::printf("\n");
    std::printf("mid-walk (B4): OS-BUG-WATCHSEL %s (%u select.extend inputs judged on Pattern while the transport plays, %u of them in WATCH, %u silent); OS-BUG-SLDOVER %s (%u WAV loads over a used slot, %u flagged by rule 1)\n",
                sum.watchsel_silent != 0u ? "FIRES" : (sum.watchsel_seen != 0u ? "does not fire" : "NOT REACHED"), sum.watchsel_seen, sum.watchsel_watch, sum.watchsel_silent,
                sum.slot_overwrites_flagged != 0u ? "FIRES" : (sum.slot_overwrites != 0u ? "does not fire" : "NOT REACHED"), sum.slot_overwrites, sum.slot_overwrites_flagged);
    if (o.verbose) {
      std::printf("rule 2 keys by action:");
      for (const auto& kv : sum.key_by_action) {
        std::printf(" %s %u;", kv.first.c_str(), kv.second);
      }
      std::printf("\n");
    }
    std::printf("session inputs skipped per screen (skipped of judged):");
    std::set<std::string> screens;
    for (const auto& kv : sum.session_by_screen) {
      screens.insert(kv.first);
    }
    for (const auto& kv : sum.judged_by_screen) {
      screens.insert(kv.first);
    }
    for (const std::string& sc : screens) {
      const auto a = sum.session_by_screen.find(sc);
      const auto j = sum.judged_by_screen.find(sc);
      std::printf(" %s %u of %u;", sc.c_str(), a == sum.session_by_screen.end() ? 0u : a->second, (a == sum.session_by_screen.end() ? 0u : a->second) + (j == sum.judged_by_screen.end() ? 0u : j->second));
    }
    std::printf("\n");
    std::printf("rules (part A, every %u-th input): %u inputs judged (%u moved the session state: not judged), %u findings; %u inputs filled a sample slot, %u loaded over a used slot on Sample load (OS-BUG-SLDOVER), %u of those flagged by a rule, %u left the document as it was\n", o.rules_every, sum.units_checked, sum.units_session, sum.rule_findings,
                sum.slot_fills, sum.slot_overwrites, sum.slot_overwrites_flagged, sum.slot_overwrites_same);
  }
  for (const std::string& s : v.fresh) {
    std::printf("NEW FINDING: %s\n", s.c_str());
  }
  for (const std::string& s : v.stale) {
    std::printf("STALE BASELINE LINE (no longer occurs): %s\n", s.c_str());
  }
  {
    const std::string shortfall = meta_shortfall(o, meta_total, sum.meta_tuned_ok, exh_total);
    if (!shortfall.empty()) {
      std::printf("%s\n", shortfall.c_str());
      return 1;
    }
  }
  if (conclusive < o.min_conclusive) {
    std::printf("TOO FEW COMPARES: %u undo bursts ended in a compare, the run wants %u (the undo invariant was not exercised)\n", conclusive, o.min_conclusive);
    return 1;
  }
  if (o.rules_every != 0u && (sum.keys_judged < o.min_keys || sum.sel_judged < o.min_sel)) {
    std::printf("TOO FEW JUDGED: rule 2 judged %u keys (the run wants %u), rule 3 judged %u inputs (wants %u): the rules were not exercised\n", sum.keys_judged, o.min_keys, sum.sel_judged, o.min_sel);
    return 1;
  }
  return v.fresh.empty() && v.stale.empty() ? 0 : 1;
}
