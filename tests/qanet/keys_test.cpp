// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-QANET A2 (docs/specs/OS-QANET.md part A, rule 2; new Community OS behaviour, test tooling only): every key that is bound in a state is pressed in that state, and
//
//   * a bound key that changes nothing (no document, view, selection, transport or sound change) and posts no notification is a silent no-op;
//   * an F key that acts or says something has a label in the key row, and an F key with a label has a binding.
//
// The states are the places of the UI: each context of context_reach() (every screen, page-and-mode context, overlay and dialog the table can bring the simulator to), each other page of every screen, and for the
// screens a transport can run under, each of them again with the sequencer playing (the playhead held at a step: `playhead`; with the engine, the transport). Pattern has its three modes (WATCH, REC free, REC follow),
// each stopped and playing. The screens whose keys act on the engine (Record, Perform, Wave, Sample edit, Mixer, Song) start it (`audio on`); the Song has four slots, the card lists five projects, the learn list two
// bindings, so that the cursors have somewhere to go. Each state is taken ONCE with its own keys, not every state against every key (the deeper states are part B's: the random walk runs the same rule at every step).
//
// The keys: F1..F8 and Shift + F1..F8 (the key row, plain and Shift layer), and the four arrows and Shift + arrows (where the selection of Pattern is made). A key is tried with every gesture the chain binds it
// with: tap, hold, double tap; a key that has only a tap and is silent as one is tried once more pressed and held. "Bound" is core::resolve over the chain the App has in that state (rig.cpp live_chain), the null action
// `ui.none` counting as unbound. The oracle is not the code: a state is judged by comparing the simulator after the key with a CONTROL run, the same state and the same waiting to the tick with no key pressed (so what the
// playhead or a timer moves on its own is not taken for the key's doing). A change is a field of the state line, the document, the selection, a parameter or a note the audio side heard, or the picture (a hash of the frame,
// read only when the key said nothing: the strip is in it). The labels are read from the picture: the key row's glyphs in the rasteriser's log (the plain row, and the Shift layer with Shift held); a dialog draws its
// row with no F-numbers and says its keys in the state line (`keys=`), which is read instead.
//
// What walk_state excuses by a rule (a silent key that the state explains), counted in the summary line: the same action with another argument acts (a key that selects what is already selected; not for the
// actions whose argument names a function: perform.key, dj.key, ui.softkey), an arrow at the edge of what it moves when its opposite moves, any arrow where no plain arrow moves anything, the placeholder soft key (ui.softkey) when it reads "-". A silent F key with no label is not excused by its siblings.
// Everything else is in baseline_keys.txt (rule | key | state | reason | ID or `new`), allow_rules.txt (a decision, or `rule:one-axis`; patterns allowed) or gaps_keys.txt (what the states do not reach: a gap of the net).
// The test fails on a finding that is in none of them and on a line that no longer occurs (QANET_WRITE_BASELINE=<path> writes the current findings; QANET_STATE=<text> runs the states whose name has the text; QANET_VERBOSE
// prints every probe; QANET_SHARD=k/n splits the states for ctest; QANET_HITS=<file> writes the pattern lines the run matched, for the test `every pattern line`, which reads all the shards').
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "action_states.h"
#include "rig.h"
#include "rules.h"

#include <o3os/app/actions.h>
#include <o3os/app/bindings.h>
#include <o3os/app/buttons.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/screen.h>
#include <o3os/app/view_state.h>
#include <o3os/core/binding.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/simulator.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/font.h>
#include <o3os/ui/theme.h>

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

struct KeyState {
  std::string name;  // part of the finding's state column: keep it stable
  Doc doc = Doc::Demo;
  std::string pre;    // lines before the reach (a sample loaded, for the sample screens)
  std::string base;   // lines of the script of the document and the card (`audio on`)
  std::string reach;
  bool playing = false;                      // the sequencer plays the pattern in view (the playhead held at step 4) when the state is taken
  app::ScreenId page_screen = app::ScreenId::Pattern;
  int page = -1;                             // >= 0: the screen's own button is pressed until that page shows
};

// Three more slots on the song (ADD SLOT on the Song screen), so that the cursor of the Song screen has somewhere to go: the demo's song is too short for the quarter jumps and Up and Down to move it.
const char* const kSongSlots = "tap song\ntap f3\nwait 20\ntap f3\nwait 20\ntap f3\nwait 20\naction cursor.jump_fraction 0\nwait 20\ntap pattern\nwait 20\n";

const char* const kSampleLoaded = "tap sampleload\nwait 120\ntap down 2\ntap f1\nwait 50\ntap f1\nwait 300\ntap pattern\n";  // KICK.WAV onto instrument 1 of the sound document (as action_states.cpp kLoaded)

// The screens whose keys act on the engine (the transport, the live notes, the mix and perform boards, the recorder and the preview): without `audio on` these keys have nothing behind them, which is a gap of the
// state and not a finding (OS-QANET A2 review).
const char* const kAudioOn = "audio on\n";

bool needs_engine(app::Context c) {
  switch (c) {
    case app::Context::Wave:
    case app::Context::WaveChop:
    case app::Context::PlayModePicker:
    case app::Context::SampleEdit:
    case app::Context::SampleEditFx:
    case app::Context::Record:
    case app::Context::RecordRadio:
    case app::Context::Perform:
    case app::Context::PerformDj:
    case app::Context::Mixer:
    case app::Context::MixerStems:
    case app::Context::Song:
    case app::Context::SongAsk:
    case app::Context::SongTools:
    case app::Context::PadsTracks:
      return true;
    default:
      return false;
  }
}

const char* button_name(app::Button b) {
  for (const app::ButtonName& n : app::kButtonNames) {
    if (n.button == b) {
      return n.name;
    }
  }
  return "";
}

std::vector<KeyState> make_states() {
  std::vector<KeyState> out;
  std::set<std::string> seen;  // the reach text of a state that is already in (Global and Pattern's default are one state)
  auto add = [&](KeyState s) {
    const std::string id = std::to_string(static_cast<int>(s.doc)) + "|" + s.pre + "|" + s.base + "|" + s.reach + "|" + std::to_string(s.page) + "|" + std::to_string(static_cast<int>(s.page_screen));
    if (seen.insert(id).second) {
      out.push_back(s);
    }
  };
  // The transports the screens can run under: the sequencer plays whatever screen is in view.
  // (the screens whose keys read the transport: the other screens would only repeat their findings)
  static const std::set<std::string> playing_too = {"Pattern WATCH", "Pattern REC free", "Pattern REC follow", "Song", "Perform"};
  for (const ContextReach& r : context_reach()) {
    if (r.steps == nullptr) {
      continue;  // a context that no run reaches: rules_test.cpp prints it
    }
    KeyState s;
    s.name = r.name;
    s.base = r.base;
    s.reach = r.steps;
    if (s.name == "Pattern" || s.name == "Global") {
      s.name = "Pattern WATCH";
    } else if (s.name == "PatternEdit") {
      s.name = "Pattern REC free";
    }
    if (r.context == app::Context::Wave || r.context == app::Context::WaveChop || r.context == app::Context::PlayModePicker || r.context == app::Context::SampleEdit) {
      s.doc = Doc::Sound;
      s.pre = kSampleLoaded;
    }
    if (r.context == app::Context::Song || r.context == app::Context::SongAsk || r.context == app::Context::SongTools || r.context == app::Context::PadsTracks) {
      s.pre = kSongSlots;
    }
    if (r.context == app::Context::Learn) {
      s.doc = Doc::DemoLearn;  // two bindings: UNBIND, DELETE and CLEAR ALL have something to act on
    }
    if (needs_engine(r.context)) {
      s.base += kAudioOn;
    }
    add(s);
  }
  {
    KeyState s;
    s.name = "Pattern REC follow";
    s.reach = "press shift\ntap rec\nrelease shift\n";
    add(s);
  }
  {
    KeyState s;  // the Song screen with its cursor in the map (Shift + Left / Right grow the range of tracks there only: OS-SONGMAP)
    s.name = "Song map";
    s.pre = kSongSlots;
    s.base = kAudioOn;
    s.reach = "tap song\ntap right\n";
    add(s);
  }
  {
    KeyState s;  // the step popup over a LIST (the instrument list): the Note panel, which the table's StepPopup is, has none, and popup.page needs one
    s.name = "StepPopup list";
    s.reach = "press instr\nwait 500\n";
    add(s);
  }
  // The other pages of every screen with pages: the screen's own button, pressed until the page shows.
  const app::ScreenTable table = app::screen_registry();
  for (std::size_t i = 0; i < table.count; ++i) {
    const app::ScreenEntry& e = table.items[i];
    if (e.screen == nullptr || e.hidden || e.buttonless || e.screen->pages() < 2) {
      continue;
    }
    for (unsigned p = 1; p < e.screen->pages(); ++p) {
      KeyState s;
      s.name = std::string(e.name) + " page " + std::to_string(p);
      for (char& c : s.name) {
        c = c == ' ' ? '_' : c;
      }
      s.page_screen = e.id;
      s.page = static_cast<int>(p);
      if (e.id == app::ScreenId::Wave || e.id == app::ScreenId::SampleEdit) {
        s.doc = Doc::Sound;
        s.pre = kSampleLoaded;
      }
      if (e.id == app::ScreenId::Song) {
        s.pre = kSongSlots;
      }
      if (e.id == app::ScreenId::Record || e.id == app::ScreenId::Perform || e.id == app::ScreenId::Wave || e.id == app::ScreenId::SampleEdit || e.id == app::ScreenId::Mixer || e.id == app::ScreenId::Song) {
        s.base += kAudioOn;
      }
      add(s);
    }
  }
  const std::size_t plain = out.size();
  for (std::size_t i = 0; i < plain; ++i) {
    if (playing_too.count(out[i].name) != 0 && (out[i].base.empty() || out[i].base == kAudioOn)) {
      KeyState p = out[i];
      p.name += " playing";
      p.playing = true;
      out.push_back(p);
    }
  }
  return out;
}

// ---- The rig -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// A fresh simulator in `state`. false: `error` says which part did not hold.
bool prepare(std::unique_ptr<sim::Simulator>& out, const KeyState& state, const std::string& card_dir, std::string& error) {
  out = std::make_unique<sim::Simulator>();
  const char* doc = state.doc == Doc::Sound ? "document sound\n" : state.doc == Doc::Tb303 ? "document tb303\n" : state.doc == Doc::DemoLearn ? "document bindings\n" : "document\n";
  if (!run_text(*out, std::string(doc) + "wait 5\ncard \"" + card_dir + "\"\nwait 120\n" + state.base, error)) {
    error = "base: " + error;
    return false;
  }
  if (!run_text(*out, state.pre, error)) {
    error = "pre: " + error;
    return false;
  }
  if (!run_text(*out, state.reach + "wait 20\n", error)) {
    error = "reach: " + error;
    return false;
  }
  if (state.page >= 0) {
    const app::ScreenEntry* e = app::find_screen(app::screen_registry(), state.page_screen);
    const std::string press = std::string("tap ") + (e != nullptr ? button_name(e->button) : "") + "\nwait 20\n";
    for (unsigned i = 0; i < 9 && out->app().state().page[app::screen_index(state.page_screen)] != state.page; ++i) {
      if (!run_text(*out, press, error)) {
        error = "page: " + error;
        return false;
      }
    }
    // A screen whose own button goes back to its first page (Instrument: its F keys are the pages): the F keys in turn.
    for (unsigned n = 1; n <= 8 && out->app().state().page[app::screen_index(state.page_screen)] != state.page; ++n) {
      if (!run_text(*out, "tap f" + std::to_string(n) + "\nwait 20\n", error)) {
        error = "page: " + error;
        return false;
      }
    }
    if (out->app().state().screen != state.page_screen || out->app().state().page[app::screen_index(state.page_screen)] != state.page) {
      error = "the screen's button does not bring page " + std::to_string(state.page) + " into view";
      return false;
    }
  }
  // The playhead is held by `playhead` on a simulator with no engine; with `audio on` it is refused (the sequencer publishes the position), and the real transport plays.
  if (state.playing && !run_text(*out, state.base.empty() ? "playhead 4\nwait 30\n" : "tap play\nwait 60\n", error)) {
    error = "playhead: " + error;
    return false;
  }
  return true;
}

// ---- Labels, from the picture -------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Labels {
  bool ok = false;  // the key row was found in the picture, plain and Shifted, or (a dialog, which draws its own row without the F-numbers) the dialog's own list was read from the state line
  bool from_dialog = false;
  bool partial = false;  // the list is a QUESTION's (a dialog's owns the whole row, a question names only its own keys): the other keys' labels are not known, and not judged
  std::string plain[8], shifted[8];
};

bool blank_label(const std::string& s) {
  for (char c : s) {
    if (c != '-' && c != ' ') {
      return false;
    }
  }
  return true;
}

// The key row of the last frame the rasteriser drew: the glyphs of the Label face on the row where "F1" .. "F8" stand, grouped by column; what lies right of each F-number is that key's label.
bool decode_row(const sim::EveRaster& raster, std::string (&out)[8]) {
  const ui::FontFace& face = ui::font_face(ui::theme(ui::ThemeId::Amiga).font(ui::Face::Label));
  struct G {
    int x, y;
    char c;
  };
  std::vector<G> glyphs;
  for (const sim::GlyphDraw& g : raster.glyph_log()) {
    if (g.source == face.ram_g_addr && g.handle == face.handle) {
      glyphs.push_back(G{g.x, g.y, static_cast<char>(g.cell + face.first_char)});
    }
  }
  if (std::getenv("QANET_DEBUG") != nullptr) {
    std::printf("glyph log %zu, label face %zu (source %u handle %u first %u):", raster.glyph_log().size(), glyphs.size(), face.ram_g_addr, face.handle, face.first_char);
    for (const G& g : glyphs) {
      std::printf(" %c@%d,%d", g.c, g.x, g.y);
    }
    std::printf("\n");
    for (std::size_t i = 0; i < raster.glyph_log().size() && i < 6; ++i) {
      std::printf("  g source %u handle %u cell %u\n", raster.glyph_log()[i].source, raster.glyph_log()[i].handle, raster.glyph_log()[i].cell);
    }
  }
  // The last "F" followed by "1" in draw order opens the numbers.
  int at = -1;
  for (std::size_t i = 0; i + 1 < glyphs.size(); ++i) {
    if (glyphs[i].c == 'F' && glyphs[i + 1].c == '1') {
      at = static_cast<int>(i);
    }
  }
  if (at < 0) {
    return false;
  }
  const int row_y = glyphs[static_cast<std::size_t>(at)].y;
  int fx[9];
  unsigned found = 0;
  for (std::size_t i = static_cast<std::size_t>(at); i + 1 < glyphs.size() && found < 8; ++i) {
    if (glyphs[i].y == row_y && glyphs[i].c == 'F' && glyphs[i + 1].c == static_cast<char>('1' + found)) {
      fx[found++] = glyphs[i].x;
    }
  }
  if (found != 8) {
    return false;
  }
  fx[8] = 1 << 20;
  for (unsigned k = 0; k < 8; ++k) {
    std::vector<G> col;
    for (const G& g : glyphs) {
      if (g.y == row_y && g.x >= fx[k] + 2 * face.cell_w && g.x < fx[k + 1]) {
        col.push_back(g);
      }
    }
    std::sort(col.begin(), col.end(), [](const G& a, const G& b) { return a.x < b.x; });
    out[k].clear();
    for (const G& g : col) {
      out[k] += g.c;
    }
  }
  return true;
}

Labels read_labels(sim::Simulator& s) {
  Labels l;
  auto& raster = const_cast<sim::EveRaster&>(s.raster());  // the simulator owns a non-const raster; record_glyphs is the one thing a test switches on
  s.set_theme(ui::ThemeId::Black);  // a redraw needs a change: the theme is set away and back (Amiga, the one whose face decode_row reads; the faces are the same in every theme)
  s.wait_ms(10);
  raster.record_glyphs(ui::kRamGEnd);
  s.set_theme(ui::ThemeId::Amiga);
  s.wait_ms(20);
  const bool plain = decode_row(raster, l.plain);
  raster.record_glyphs(ui::kRamGEnd);
  s.press(app::Button::Shift);
  s.wait_ms(40);
  const bool shifted = decode_row(raster, l.shifted);
  s.release(app::Button::Shift);
  s.wait_ms(20);
  raster.record_glyphs(0);
  l.ok = plain && shifted;
  if (!l.ok) {
    // A dialog owns the row and draws its labels with no F-numbers (key_prompt.cpp): its list is in the state line (`keys=F1:SELECT,F8:BACK,SF7:CLOSE`: the key, and the label the row paints for it).
    const std::string keys = snap_of(s).keys;
    if (!keys.empty()) {
      for (std::size_t at = 0; at < keys.size();) {
        std::size_t end = keys.find(',', at);
        end = end == std::string::npos ? keys.size() : end;
        const std::string item = keys.substr(at, end - at);
        const bool shift = item.size() > 2 && item[0] == 'S' && item[1] == 'F';
        const std::size_t f = shift ? 1 : 0;
        if (item.size() > f + 2 && item[f] == 'F' && item[f + 1] >= '1' && item[f + 1] <= '8' && item[f + 2] == ':') {
          (shift ? l.shifted : l.plain)[item[f + 1] - '1'] = item.substr(f + 3);
        }
        at = end + 1;
      }
      // A dialog with no Shift option does not follow Shift: Shift + Fn acts as Fn there and the row keeps its plain labels (owner_row); one with Shift options shows them, and "-" for the keys it does not list.
      bool shift_layer = false;
      for (const std::string& w : l.shifted) {
        shift_layer = shift_layer || !w.empty();
      }
      for (unsigned k = 0; k < 8; ++k) {
        l.shifted[k] = shift_layer ? (l.shifted[k].empty() ? "-" : l.shifted[k]) : l.plain[k];
      }
      l.ok = true;
      l.from_dialog = true;
      l.partial = question_up(snap_of(s));
    }
  }
  return l;
}

// ---- The keys ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr unsigned kGestureMs = 400;  // after a press: the gesture layer holds a tap back for a double tap (300 ms) and a hold takes 400 ms, so the key has not done its job before this

struct KeySpec {
  std::string name;
  app::Button button;
  bool shift;
  int fkey;  // 0..7 for F1..F8, -1 for an arrow
};

std::vector<KeySpec> make_keys() {
  std::vector<KeySpec> keys;
  static const app::Button f[8] = {app::Button::F1, app::Button::F2, app::Button::F3, app::Button::F4, app::Button::F5, app::Button::F6, app::Button::F7, app::Button::F8};
  for (bool shift : {false, true}) {
    for (int n = 0; n < 8; ++n) {
      keys.push_back(KeySpec{std::string(shift ? "Shift+" : "") + "F" + std::to_string(n + 1), f[n], shift, n});
    }
  }
  static const std::pair<const char*, app::Button> arrows[4] = {{"Up", app::Button::Up}, {"Down", app::Button::Down}, {"Left", app::Button::Left}, {"Right", app::Button::Right}};
  for (bool shift : {false, true}) {
    for (const auto& a : arrows) {
      keys.push_back(KeySpec{std::string(shift ? "Shift+" : "") + a.first, a.second, shift, -1});
    }
  }
  return keys;
}

// The simulated time a block of scenario text takes, in ticks: a press and a release one each, a tap two (a press and a release), a wait its value.
static_assert(sim::Simulator::kTickUs == 1000, "ticks_of counts milliseconds");
unsigned ticks_of(const std::string& text) {
  unsigned n = 0;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    end = end == std::string::npos ? text.size() : end;
    const std::string line = text.substr(at, end - at);
    char verb[16] = {};
    char arg[16] = {};
    const int words = std::sscanf(line.c_str(), "%15s %*s %15s", verb, arg);
    unsigned count = 1;
    if (std::strcmp(verb, "wait") == 0) {
      n += static_cast<unsigned>(std::strtoul(line.c_str() + 5, nullptr, 10));
    } else if (std::strcmp(verb, "tap") == 0) {
      count = words >= 2 ? static_cast<unsigned>(std::strtoul(arg, nullptr, 10)) : 1u;
      n += 2u * (count == 0 ? 1u : count);
    } else if (std::strcmp(verb, "press") == 0 || std::strcmp(verb, "release") == 0) {
      n += 1u;
    }
    at = end + 1;
  }
  return n;
}

// A press of a key with one gesture, and the control that waits as long and presses nothing.
struct Probe {
  std::string name;
  std::string press, control;
  std::string action;  // the registry name of what the chain binds the gesture to (for QANET_VERBOSE)
  // A key that is bound as a tap and nothing else may act only while it is held (HALF and DOUBLE of Perform: the rate changes on the press and goes back on the release, so a tap leaves nothing to see). When the tap
  // is silent such a key is tried once more, pressed and held 500 ms, against a control that waits as long. Empty: the key has a hold or a double tap of its own, whose probe says what holding does.
  std::string held, held_control;
};

std::vector<Probe> probes_of(const KeySpec& k, const core::BindingSet& set, const core::ContextChain& chain) {
  std::vector<Probe> out;
  const std::string key = button_name(k.button);
  const char* const wrap_open = k.shift ? "press shift\n" : "";
  const char* const wrap_close = k.shift ? "release shift\n" : "";
  struct Kind {
    core::GestureKind kind;
    const char* suffix;
    std::string press, control;
  };
  // A hold is judged while it is held (a chooser that opens on the press and closes on the release has done its job by then): no release is run, the simulator is dropped. A Shift + F key is tried as a tap only: with
  // Shift held the resolver falls back to a plain key's Hold and Double tap bindings (core/binding.h), which says nothing about the Shift layer.
  // The control waits as long as the press takes, to the tick: a press, a release and a wait each spend simulated time (a tap is two ticks), and what a timer or the playhead does in it is the control's too.
  Kind kinds[] = {
      {core::GestureKind::Tap, "", std::string("tap ") + key + "\n", ""},
      {core::GestureKind::Hold, " hold", "press " + key + "\nwait 500\n", ""},
      {core::GestureKind::DoubleTap, " double tap", std::string("tap ") + key + " 2\n", ""},
  };
  for (Kind& kind : kinds) {
    kind.control = "wait " + std::to_string(ticks_of(kind.press)) + "\n";
  }
  for (const Kind& kind : kinds) {
    if (k.shift && kind.kind != core::GestureKind::Tap) {
      continue;
    }
    const core::Gesture g{0, kind.kind, app::button_ref(k.button), k.shift ? app::button_ref(app::Button::Shift) : core::kNoInput, 0};
    const core::Resolution r = core::resolve(set, chain, g);
    if (r.status == core::Resolution::Status::Bound && r.action != core::action_id("ui.none") && r.action != core::ActionId::None) {
      std::string action = "?";
      const app::ActionTable registry = app::action_registry();
      for (std::size_t i = 0; i < registry.count; ++i) {
        if (registry.items[i].id == r.action) {
          action = registry.items[i].name;
        }
      }
      out.push_back(Probe{k.name + kind.suffix, std::string(wrap_open) + kind.press + wrap_close, std::string(wrap_open) + kind.control + wrap_close, action + " " + std::to_string(r.arg), "", ""});
    }
  }
  if (out.size() == 1 && out[0].name == k.name) {  // the tap is the key's only gesture
    out[0].held = std::string(wrap_open) + "press " + key + "\nwait 500\n";
    out[0].held_control = std::string(wrap_open) + "wait 501\n";
  }
  return out;
}

// ---- The walk ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The names of what differs between two snapshots (QANET_VERBOSE).
std::string diff_names(const Snap& a, const Snap& b) {
  std::string out;
  for (const auto& [name, value] : a.fields) {
    const auto it = b.fields.find(name);
    if (it == b.fields.end() || it->second != value) {
      out += name + "=" + value + ">" + (it == b.fields.end() ? "-" : it->second) + " ";
    }
  }
  out += a.digest != b.digest ? "digest " : "";
  out += a.selection != b.selection ? "selection " : "";
  out += a.heard != b.heard ? "heard " : "";
  return out;
}

struct Report {
  std::vector<std::string> no_key_row;  // the states whose key row is neither in the picture nor in a dialog's list: their keys are tried, their labels are not judged
  std::vector<Line> findings;
  std::vector<std::string> setup_errors;
  std::set<std::string> walked;
  unsigned states = 0, probes = 0, keys_bound = 0, labels_missing = 0, excused_sibling = 0, excused_opposite = 0, excused_unused = 0;
};

// How a probe presses: the real keys (the simulator), or a planted fault in the self-tests.
using Press = std::function<void(sim::Simulator&, const Probe&)>;

Snap run_control(const KeyState& state, const std::string& text, const std::string& card_dir, std::string& error) {
  std::unique_ptr<sim::Simulator> c;
  if (!prepare(c, state, card_dir, error) || !run_text(*c, text, error)) {
    return Snap{};
  }
  c->wait_ms(kGestureMs);
  settle(*c);
  return snap_of(*c);
}

// "name arg" of a probe's action ("wave.zoom -1"): the registry name, and the argument.
std::string action_name(const std::string& action) { return action.substr(0, action.find(' ')); }
std::string action_arg(const std::string& action) { return action.find(' ') == std::string::npos ? std::string() : action.substr(action.find(' ') + 1); }

// What one probe of a state found, kept until the whole state has been walked: whether a finding is excused depends on the other keys of the state.
struct Result {
  std::string key;      // the key's name ("Shift+F2")
  std::string action;   // "wave.zoom -1"
  bool acted = false;
  bool arrow = false;
  bool fkey = false;
  bool labelled = false;     // the key row shows a word for the key
  bool row_known = false;    // and the row could be read
  std::vector<Line> silent;  // the "changes nothing and says nothing" findings of the probe (at most one)
};

void walk_state(const KeyState& state, const std::string& card_dir, const Press& press, Report& report) {
  std::string error;
  std::unique_ptr<sim::Simulator> live;
  if (!prepare(live, state, card_dir, error)) {
    report.setup_errors.push_back(state.name + ": " + error);
    return;
  }
  ++report.states;
  report.walked.insert(state.name);
  const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  const core::ContextChain chain = live_chain(*live);
  const Labels labels = read_labels(*live);
  if (!labels.ok) {
    ++report.labels_missing;
    // Neither the picture nor a dialog says what the keys are: their labels cannot be judged (a gap of the net, tracked in gaps_keys.txt), and the keys are tried all the same.
    report.no_key_row.push_back(state.name);
    report.findings.push_back(Line{"R2", "key row", state.name, "no key row to read the labels from: not in the picture, not in a dialog's list", "new"});
  }
  std::map<std::string, Snap> controls;  // by control text
  const bool verbose = std::getenv("QANET_VERBOSE") != nullptr;
  std::vector<Result> results;
  for (const KeySpec& key : make_keys()) {
    std::vector<Probe> probes = probes_of(key, set, chain);
    if (key.shift && key.fkey < 0 && !probes.empty()) {
      // A Shift + arrow with no Shift binding of its own falls back to the plain arrow (core/binding.h): the same action, and its finding is the plain arrow's.
      const std::vector<Probe> plain = probes_of(KeySpec{std::string(key.name.substr(6)), key.button, false, -1}, set, chain);
      if (!plain.empty() && plain[0].action == probes[0].action) {
        continue;
      }
    }
    KeyFacts facts;
    facts.bound = !probes.empty();
    facts.fkey = key.fkey >= 0;
    facts.labelled = facts.fkey && labels.ok && !blank_label((key.shift ? labels.shifted : labels.plain)[key.fkey]);
    facts.row_known = labels.ok && (!labels.partial || facts.labelled);
    report.keys_bound += facts.bound ? 1u : 0u;
    if (probes.empty() && facts.fkey && key.fkey < 7 && live->app().state().screen == app::ScreenId::ModuleApp) {
      // OS-QAFIX-10: an app's page captures the input (the app gets F1..F7 raw, before the bindings; the OS keeps F8 BACK and the exit chord): the label is the app's own, declared through the ABI, and the
      // chain cannot say whether the app acts on it. It is the app's tests' to prove (tests/sim_apps_test.cpp), not a binding to judge here.
      continue;
    }
    if (probes.empty()) {
      // An unbound key: only the label can be wrong. No press is needed: the picture before and after is the same.
      for (const Finding& f : check_key(facts, Snap{}, Snap{})) {
        report.findings.push_back(Line{f.rule, key.name, state.name, f.reason, "new"});
      }
      continue;
    }
    for (const Probe& probe : probes) {
      auto it = controls.find(probe.control);
      if (it == controls.end()) {
        Snap c = run_control(state, probe.control, card_dir, error);
        it = controls.emplace(probe.control, c).first;
      }
      std::unique_ptr<sim::Simulator> t;
      if (!prepare(t, state, card_dir, error)) {
        report.setup_errors.push_back(state.name + " > " + probe.name + ": " + error);
        continue;
      }
      press(*t, probe);
      t->wait_ms(kGestureMs);
      settle(*t);
      Snap after = snap_of(*t);
      ++report.probes;
      Snap reference = it->second;
      bool acted = key_acted(reference, after);
      if (!acted && reference.posts == after.posts && reference.msg == after.msg && !probe.held.empty()) {
        // Silent as a tap: does it act while it is held?
        std::unique_ptr<sim::Simulator> h;
        if (prepare(h, state, card_dir, error)) {
          Probe held_probe = probe;
          held_probe.press = probe.held;
          held_probe.control = probe.held_control;
          press(*h, held_probe);
          h->wait_ms(kGestureMs);
          settle(*h);
          const Snap held_after = snap_of(*h);
          auto hc = controls.find(probe.held_control);
          if (hc == controls.end()) {
            hc = controls.emplace(probe.held_control, run_control(state, probe.held_control, card_dir, error)).first;
          }
          if (key_acted(hc->second, held_after) || hc->second.posts != held_after.posts || hc->second.msg != held_after.msg) {
            reference = hc->second;
            after = held_after;
            acted = key_acted(reference, after);
          }
        }
      }
      if (verbose) {
        std::printf("probe %s | %s | %s | label '%s' | acted %s | said %s | %s\n", probe.name.c_str(), probe.action.c_str(), state.name.c_str(), facts.fkey ? (key.shift ? labels.shifted : labels.plain)[key.fkey].c_str() : "",
                    acted ? "yes" : "no", reference.posts != after.posts ? "yes" : "no", diff_names(reference, after).c_str());
      }
      Result r;
      r.key = probe.name;
      r.action = probe.action;
      r.acted = acted;
      r.arrow = key.fkey < 0;
      r.fkey = facts.fkey;
      r.labelled = facts.labelled;
      r.row_known = facts.row_known;
      for (const Finding& f : check_key(facts, reference, after)) {
        Line line{f.rule, probe.name + " (" + probe.action + ")", state.name, f.reason, "new"};
        if (f.reason == "bound key changes nothing and says nothing") {
          r.silent.push_back(line);
        } else {
          report.findings.push_back(line);
        }
      }
      results.push_back(r);
    }
  }
  // A key that does nothing is excused when it does what it does elsewhere in the state, and the judgement is about the state, not about the key alone:
  //   * the same action with another argument acts here (param.group 0 when page 0 is the one in view, instrument.step -1 at the first instrument, pads.mode 0 when it is the mode that is on): the key is live, and
  //     it selects what is already selected. Every argument silent is a finding;
  //   * an arrow that does nothing at the edge of what it moves is not silent when its opposite moves (the cursor stood at the edge). Both of a pair silent is a finding;
  //   * the arrows count where the screen uses arrows at all: when no plain arrow does anything in the state there is no cursor to move, and a Shift + arrow that does nothing there is the same case, not a bug. Where a
  //     plain arrow acts, a Shift + arrow that does nothing is a finding (the selection keys of Pattern while the playhead owns the cursor).
  std::map<std::string, bool> arrow_acted;
  for (const Result& r : results) {
    if (r.arrow) {
      arrow_acted[r.key] = r.acted;
    }
  }
  bool arrows_live = false;
  for (const char* n : {"Up", "Down", "Left", "Right"}) {
    const auto it = arrow_acted.find(n);
    arrows_live = arrows_live || (it != arrow_acted.end() && it->second);
  }
  static const std::pair<const char*, const char*> opposite[] = {{"Up", "Down"}, {"Down", "Up"}, {"Left", "Right"}, {"Right", "Left"}};
  for (const Result& r : results) {
    for (const Line& line : r.silent) {
      // (An action whose argument names a function and not a choice, perform.key and dj.key, ui.softkey: F4 HALF is no peer of F6 LATCH, and the one that acts says nothing about the one that does not.)
      const std::string name = action_name(r.action);
      const bool dispatcher = name == "ui.softkey" || (name.size() > 4 && name.compare(name.size() - 4, 4, ".key") == 0);
      // A silent F key with no label on it is not excused by its sibling: a key the row does not promise and that is bound to a real action is judged below (the placeholder only), or is a finding.
      const bool unlabelled_fkey = r.fkey && r.row_known && !r.labelled;
      bool sibling = false;
      for (const Result& o : results) {
        sibling = sibling || (!dispatcher && !unlabelled_fkey && o.acted && action_name(o.action) == name && action_arg(o.action) != action_arg(r.action));
      }
      if (sibling) {
        ++report.excused_sibling;
        if (verbose) {
          std::printf("excused by sibling: %s | %s\n", line.action.c_str(), state.name.c_str());
        }
        continue;
      }
      // The placeholder soft key (ui.softkey) reads "-" (or nothing) and does nothing: it promises nothing and says nothing for a "-" (ui_softkey, OS-CFGWIRE). It is the one excuse by the label. Any other action that
      // is silent under a blank label (the File screen's report key outside the IMPORT page) is a finding, or an allow line with its reason; a silent key with a word on it is a finding too.
      if (r.fkey && r.row_known && !r.labelled && name == "ui.softkey") {
        ++report.excused_unused;
        continue;
      }
      if (r.arrow) {
        const bool shift = r.key.compare(0, 6, "Shift+") == 0;
        const std::string bare = shift ? r.key.substr(6) : r.key;
        std::string other;
        for (const auto& o : opposite) {
          other = bare == o.first ? std::string(shift ? "Shift+" : "") + o.second : other;
        }
        const auto it = arrow_acted.find(other);
        if (!arrows_live || (it != arrow_acted.end() && it->second)) {
          ++report.excused_opposite;
          continue;
        }
      }
      report.findings.push_back(line);
    }
  }
}

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

const Press kReal = [](sim::Simulator& s, const Probe& p) {
  std::string error;
  (void)run_text(s, p.press, error);
};

}  // namespace

namespace {

// The card of the fixture lists one project: the File screen's cursor has nowhere to go and the quarter jumps move nothing. Four more of the same, so that the list is long enough (this test's card only).
bool widen_card(const Card& card) {
  const std::string root = card.dir.path();
  for (int n = 2; n <= 5; ++n) {
    const std::string cmd = "cp -r '" + root + "/Projects/Small Song' '" + root + "/Projects/Small Song " + std::to_string(n) + "'";
    if (std::system(cmd.c_str()) != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---- The test ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("qanet keys: every bound key of every state is labelled when it acts and does something when it is labelled (baseline of known findings)") {
  const auto t0 = std::chrono::steady_clock::now();
  Card card;
  CHECK_MESSAGE(card.ok, "the card fixture could not be made (is the target e2e_card_fixture built?)");
  if (!card.ok) {
    return;
  }
  CHECK(widen_card(card));
  const std::vector<KeyState> states = make_states();
  const Shard shard = shard_from_env();
  const char* only = std::getenv("QANET_STATE");
  Report report;
  for (std::size_t i = 0; i < states.size(); ++i) {
    if (i % shard.n != shard.k - 1u || (only != nullptr && states[i].name.find(only) == std::string::npos)) {
      continue;
    }
    walk_state(states[i], card.dir.path(), kReal, report);
  }

  std::vector<Line> baseline, allow, gaps;
  std::string text, error;
  CHECK_MESSAGE(read_file(O3_QANET_KEYS_BASELINE, text), "cannot read " << O3_QANET_KEYS_BASELINE);
  CHECK_MESSAGE(parse_lines(text, baseline, error), "baseline_keys.txt: " << error);
  // allow_rules.txt (the decisions, with patterns; the file is shared with the other rules: this test reads the lines of rule 2) and gaps_keys.txt (what the states do not reach: a gap of the net, not a finding).
  text.clear();
  CHECK_MESSAGE(read_file(O3_QANET_ALLOW, text), "cannot read " << O3_QANET_ALLOW);
  CHECK_MESSAGE(parse_lines(text, allow, error), "allow_rules.txt: " << error);
  allow.erase(std::remove_if(allow.begin(), allow.end(), [](const Line& l) { return l.rule != "R2"; }), allow.end());
  text.clear();
  CHECK_MESSAGE(read_file(O3_QANET_KEYS_GAPS, text), "cannot read " << O3_QANET_KEYS_GAPS);
  CHECK_MESSAGE(parse_lines(text, gaps, error), "gaps_keys.txt: " << error);
  for (const Line& g : gaps) {
    CHECK_MESSAGE(g.id.compare(0, 4, "gap:") == 0, "a gaps_keys.txt line names what the state lacks, `gap:<what>`: " << g.text());
  }
  allow.insert(allow.end(), gaps.begin(), gaps.end());  // an excused line, and a line that no longer occurs fails the same way
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
  // A shard (or a run of the states that have a name) walks some of the states: a line with no `*` is judged stale only where its state was walked, a pattern line is not judged here: the shard writes the pattern lines
  // it matched (QANET_HITS=<file>) and the test `every pattern line` reads them all (compare).
  const std::vector<Line>& mine = baseline;
  const bool partial = shard.n > 1 || only != nullptr;
  const Verdict v = compare(report.findings, mine, allow, partial ? &report.walked : nullptr);
  if (const char* hits = std::getenv("QANET_HITS")) {
    std::ofstream f(hits);
    for (const std::string& h : v.pattern_hits) {
      f << h << "\n";
    }
  }
  unsigned fresh = 0;
  for (const Line& l : mine) {
    fresh += l.id == "new" ? 1u : 0u;
  }
  const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  std::printf("qanet keys: %zu states in all, %u walked, %u bound keys, %u probes, %zu findings (%zu in the baseline, %u of them `new`), %u excused by a sibling argument, %u by the opposite arrow, %u unused soft keys, %u states with no key row, %ld ms\n",
              states.size(), report.states, report.keys_bound, report.probes, report.findings.size(), mine.size(), fresh, report.excused_sibling, report.excused_opposite, report.excused_unused, report.labels_missing, ms);
  CHECK_MESSAGE(report.setup_errors.empty(), "setup problems (a state that did not hold):" << join(report.setup_errors));
  for (const std::string& n : report.no_key_row) {
    std::printf("  no key row (picture or dialog list): %s\n", n.c_str());
  }
  CHECK_MESSAGE(v.bad.empty(), "the baseline is malformed:" << join(v.bad));
  CHECK_MESSAGE(v.unknown.empty(), "findings that are not in baseline_keys.txt (fix the bug, or add the line, marked `new`, for the maintainer's triage):" << join(v.unknown));
  CHECK_MESSAGE(v.stale.empty(), "baseline lines that no longer occur (the bug is fixed: delete the line):" << join(v.stale));
}

// The pattern lines of baseline_keys.txt, allow_rules.txt and gaps_keys.txt each matched a finding of some shard (QANET_HITS_FILES: the files the shards wrote, `|` apart): a pattern that no state needs any more is a
// line that no longer occurs, which fails as it does for an exact line.
TEST_CASE("qanet keys: every pattern line of the baseline, allow and gaps files matched a finding") {
  const char* files = std::getenv("QANET_HITS_FILES");
  CHECK_MESSAGE(files != nullptr, "QANET_HITS_FILES names the files the shards wrote (ctest sets it)");
  if (files == nullptr) {
    return;
  }
  std::set<std::string> hit;
  const std::string list = files;
  for (std::size_t at = 0; at < list.size();) {
    std::size_t end = list.find('|', at);
    end = end == std::string::npos ? list.size() : end;
    std::ifstream f(list.substr(at, end - at));
    CHECK_MESSAGE(f.good(), "a shard has not written " << list.substr(at, end - at));
    for (std::string line; std::getline(f, line);) {
      hit.insert(line);
    }
    at = end + 1;
  }
  std::vector<Line> lines;
  std::string text, error;
  for (const char* path : {O3_QANET_KEYS_BASELINE, O3_QANET_ALLOW, O3_QANET_KEYS_GAPS}) {
    text.clear();
    CHECK(read_file(path, text));
    std::vector<Line> part;
    CHECK_MESSAGE(parse_lines(text, part, error), path << ": " << error);
    for (const Line& l : part) {
      if (is_pattern(l) && (l.rule == "R2") && hit.count(l.text()) == 0) {
        lines.push_back(l);
      }
    }
  }
  std::vector<std::string> stale;
  for (const Line& l : lines) {
    stale.push_back(l.text());
  }
  CHECK_MESSAGE(stale.empty(), "pattern lines that no state needs any more (delete them):" << join(stale));
}

// ---- Self-tests ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

// One planted key on the Pattern screen: `edit` is what the key does, `facts` what the rule is told about it.
std::vector<Finding> plant(const Card& card, const KeyFacts& facts, const std::function<void(sim::Simulator&)>& edit, bool* ok = nullptr) {
  KeyState state;
  state.name = "plant";
  state.reach = "tap pattern\n";
  std::string error;
  std::unique_ptr<sim::Simulator> control, t;
  const bool prepared = prepare(control, state, card.dir.path(), error) && prepare(t, state, card.dir.path(), error);
  if (ok != nullptr) {
    *ok = prepared;
  }
  if (!prepared) {
    return {};
  }
  settle(*control);
  edit(*t);
  settle(*t);
  return check_key(facts, snap_of(*control), snap_of(*t));
}

// A planted press that does nothing: the control, the same time and the same Shift with no key, so that only the key's own doing is missing.
void quiet(sim::Simulator& s, const Probe& p) {
  std::string error;
  (void)run_text(s, p.control, error);
}

}  // namespace

TEST_CASE("qanet keys self-test: a planted silent key and a planted unlabelled key fail, a key that acts and has a label does not") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  bool ok = false;
  KeyFacts labelled_fkey;
  labelled_fkey.bound = labelled_fkey.fkey = labelled_fkey.labelled = true;
  KeyFacts bare_fkey = labelled_fkey;
  bare_fkey.labelled = false;
  KeyFacts arrow;
  arrow.bound = true;

  // A bound key that does nothing and says nothing (a labelled one too: a label that promises a function is no excuse).
  std::vector<Finding> silent = plant(card, labelled_fkey, [](sim::Simulator&) {}, &ok);
  CHECK(ok);
  CHECK(silent.size() == 1u);
  if (!silent.empty()) {
    CHECK(silent[0].reason == "bound key changes nothing and says nothing");
  }
  CHECK(plant(card, arrow, [](sim::Simulator&) {}).size() == 1u);

  // A bound F key that acts (the document changes) and shows no label.
  std::vector<Finding> unlabelled = plant(card, bare_fkey, [](sim::Simulator& s) { (void)s.app().session().state_editor()->set_swing(33); });
  CHECK(unlabelled.size() == 1u);
  if (!unlabelled.empty()) {
    CHECK(unlabelled[0].reason == "bound key acts or says something and the key row shows no label");
  }

  // A label on a key that is bound to nothing.
  KeyFacts dead = labelled_fkey;
  dead.bound = false;
  std::vector<Finding> promise = plant(card, dead, [](sim::Simulator&) {});
  CHECK(promise.size() == 1u);
  if (!promise.empty()) {
    CHECK(promise[0].reason == "the key row shows a label and the key is bound to nothing");
  }

  // Not findings: a labelled key that acts, an arrow that moves the view, a key that only says something, an unbound key with no label.
  CHECK(plant(card, labelled_fkey, [](sim::Simulator& s) { (void)s.app().session().state_editor()->set_swing(34); }).empty());
  CHECK(plant(card, arrow, [](sim::Simulator& s) { s.tap(app::Button::Right); }).empty());
  CHECK(plant(card, arrow, [](sim::Simulator& s) { s.action(core::action_id("nav.screen"), 3); }).empty());
  CHECK(plant(card, arrow, [](sim::Simulator& s) { s.set_theme(ui::ThemeId::Black); }).empty());  // only the picture moved: a zoom, a cursor, a highlight
  KeyFacts unbound;
  CHECK(plant(card, unbound, [](sim::Simulator&) {}).empty());

  // The rule on snapshots: said without changed is not silent; the follow cursor's row alone is not a change while the transport drifts.
  Snap a, b;
  a.digest = b.digest = "x";
  a.fields["row"] = "3";
  b.fields["row"] = "4";
  CHECK(differs(a, b, false));
  CHECK_FALSE(differs(a, b, true));
  b.fields["row"] = "3";
  b.posts = 1;
  CHECK_FALSE(differs(a, b, false));
  CHECK(check_key(arrow, a, b).empty());
  b.selection = "A-:1,0,2,0";
  CHECK(differs(a, b, true));
}

TEST_CASE("qanet keys self-test: what a key said is not what it changed, the picture and the notes are seen, and a refusal with no label is a finding") {
  Snap a, b;
  a.digest = b.digest = "x";
  a.fields["overlay"] = "NONE";
  b.fields["overlay"] = "NOTIFY";
  b.posts = 1;
  CHECK_FALSE(differs(a, b, false));  // the strip came up: that is the key speaking
  b.fields["overlay"] = "LEARN";
  CHECK(differs(a, b, false));
  b.fields["overlay"] = "NONE";
  b.frame = 7;
  CHECK(differs(a, b, false));          // the picture moved: a zoom, a cursor
  CHECK_FALSE(differs(a, b, false, false));  // but not when the key spoke (the strip is in the picture)
  CHECK_FALSE(differs(a, b, true));     // nor while the transport drifts
  b.frame = 0;
  b.note_ons = 1;
  CHECK(differs(a, b, false));          // a note the voice pool started: a preview
  b.note_ons = 0;

  // A key that only says something and has no label is a finding (a refusal is an answer: the row hides a key that answers); a labelled one that says something is not; a silent one with no label is not judged here.
  KeyFacts bare;
  bare.bound = bare.fkey = true;
  b.posts = 1;
  b.msg = "NOTHING TO APPLY";
  const std::vector<Finding> said = check_key(bare, a, b);
  CHECK(said.size() == 1u);
  KeyFacts labelled = bare;
  labelled.labelled = true;
  CHECK(check_key(labelled, a, b).empty());
  KeyFacts unknown_row = bare;
  unknown_row.row_known = false;
  CHECK(check_key(unknown_row, a, b).empty());  // the labels of this state could not be read: nothing to say about them
  b.posts = a.posts;
  b.msg = a.msg;
  CHECK(check_key(bare, a, b).size() == 1u);  // silent: the placeholder rule of walk_state excuses an unlabelled key, check_key reports it
  CHECK(check_key(bare, a, b)[0].reason == "bound key changes nothing and says nothing");
}

TEST_CASE("qanet keys self-test: a control waits as long as its press, to the tick") {
  CHECK(ticks_of("tap f1\n") == 2u);
  CHECK(ticks_of("tap f1 2\n") == 4u);
  CHECK(ticks_of("press f1\nwait 500\n") == 501u);
  CHECK(ticks_of("press shift\ntap f1\nrelease shift\n") == 4u);
  CHECK(ticks_of("") == 0u);
  const core::BindingSet set{{core::BindingView{}, core::BindingView{}, app::builtin_bindings()}};
  core::ContextChain chain;
  chain.push(app::context_id(app::Context::Global));
  chain.push(app::context_id(app::Context::Pattern));
  const std::vector<KeySpec> keys = make_keys();
  unsigned seen = 0;
  for (const KeySpec& k : keys) {
    for (const Probe& p : probes_of(k, set, chain)) {
      const std::string open = k.shift ? "press shift\n" : "";
      const std::string close = k.shift ? "release shift\n" : "";
      CHECK_MESSAGE(ticks_of(p.press) == ticks_of(p.control), p.name);
      CHECK_MESSAGE(p.control.compare(0, open.size(), open) == 0, p.name);
      ++seen;
    }
  }
  CHECK(seen > 10u);
}

TEST_CASE("qanet keys self-test: the walk excuses a key by its sibling, flags every argument silent, and does not excuse a dispatcher") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  auto names = [](const Report& r) {
    std::set<std::string> out;
    for (const Line& l : r.findings) {
      out.insert(l.action);
    }
    return out;
  };
  KeyState pattern;
  pattern.name = "Pattern WATCH";
  // Nothing happens on any key: every argument of pads.mode is silent, which is a finding for each of them.
  Report none;
  // (A planted press runs the control: the same time, the same Shift, and no key. A timer must not be taken for the key's doing.)
  walk_state(pattern, card.dir.path(), [](sim::Simulator& s, const Probe& p) { quiet(s, p); }, none);
  CHECK(none.setup_errors.empty());
  CHECK(names(none).count("Shift+F2 (pads.mode 0)") == 1u);  // (OS-PADOCT: Shift + F1 is the octave, the modes begin at F2)
  CHECK(names(none).count("Shift+F3 (pads.mode 1)") == 1u);
  // One of them acts: the others select what is already selected.
  Report one;
  walk_state(pattern, card.dir.path(), [](sim::Simulator& s, const Probe& p) {
    quiet(s, p);
    if (p.action == "pads.mode 1") {
      s.action(core::action_id("pads.mode"), 1);
    }
  }, one);
  CHECK(names(one).count("Shift+F2 (pads.mode 0)") == 0u);
  CHECK(names(one).count("Shift+F3 (pads.mode 1)") == 0u);
  CHECK(names(one).count("Shift+F5 (pads.mode 2)") == 0u);
  CHECK(names(one).count("F6 (pattern.invert 0)") == 1u);  // a key with no sibling is still silent
  CHECK(one.excused_sibling >= 3u);
  // An action whose argument names a function is no peer of the others: LATCH acts and RESET, which has nothing to turn off, is still silent.
  KeyState perform;
  perform.name = "Perform";
  perform.base = kAudioOn;
  perform.reach = "tap performance\n";
  Report dispatcher;
  walk_state(perform, card.dir.path(), [](sim::Simulator& s, const Probe& p) {
    quiet(s, p);
    if (p.action == "perform.key 5") {
      s.action(core::action_id("perform.key"), 5);
    }
  }, dispatcher);
  CHECK(dispatcher.setup_errors.empty());
  CHECK(names(dispatcher).count("F8 (perform.key 7)") == 1u);
}

TEST_CASE("qanet keys self-test: the control run is the same state, so a held playhead and the waiting change nothing") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  KeyState state;
  state.name = "Pattern WATCH playing";
  state.playing = true;
  std::string error;
  const Snap one = run_control(state, "wait 500\n", card.dir.path(), error);
  const Snap two = run_control(state, "wait 500\n", card.dir.path(), error);
  CHECK_MESSAGE(error.empty(), error);
  CHECK_FALSE(differs(one, two, false));
  CHECK(one.fields.count("seq") == 1u);
  CHECK(one.fields.at("seq") == "1");

  // The key row is read from the picture: the Pattern screen's plain row has labels, and its Shift layer is a different row.
  std::unique_ptr<sim::Simulator> s;
  KeyState pattern;
  CHECK(prepare(s, pattern, card.dir.path(), error));
  if (s == nullptr) {
    return;
  }
  const Labels l = read_labels(*s);
  CHECK(l.ok);
  unsigned words = 0;
  for (const std::string& w : l.plain) {
    words += blank_label(w) ? 0u : 1u;
  }
  CHECK(words >= 4u);
  CHECK(l.plain[0] != l.shifted[0]);
  CHECK_FALSE(l.from_dialog);

  // A dialog draws its row with no F-numbers: its labels come from the state line's list, the Shift layer is the plain row when the dialog declares none, and a question names only its own keys.
  std::unique_ptr<sim::Simulator> d;
  KeyState chooser;
  chooser.name = "Chooser";
  chooser.reach = "press f1\nwait 500\n";
  CHECK(prepare(d, chooser, card.dir.path(), error));
  if (d != nullptr) {
    const Labels dl = read_labels(*d);
    CHECK(dl.ok);
    CHECK(dl.from_dialog);
    CHECK_FALSE(dl.partial);
    CHECK(dl.plain[0] == "PATTERN");
    CHECK(dl.plain[7] == "BACK");
    CHECK(dl.shifted[0] == "PATTERN");
    CHECK(blank_label(dl.plain[4]));
  }
}

// The control of a state must be the same state when it is run again, with the engine on too: the transport with `audio on` and `tap play` (the playhead is the engine's, not held), and the screens whose picture and
// parameters move by themselves, the meters of Perform and Mixer. A control that drifts between two runs would take a timer for a key's doing, or hide a key's doing in the noise.
TEST_CASE("qanet keys self-test: the control run is the same state with the engine on, the transport playing and the meter screens drawing") {
  Card card;
  CHECK(card.ok);
  if (!card.ok) {
    return;
  }
  std::vector<KeyState> states;
  {
    KeyState engine;  // audio on and tap play on the Pattern screen
    engine.name = "Pattern WATCH engine playing";
    engine.base = kAudioOn;
    engine.playing = true;
    states.push_back(engine);
  }
  const std::set<std::string> wanted = {"Perform", "Perform playing", "Mixer"};
  for (const KeyState& s : make_states()) {
    if (wanted.count(s.name) != 0) {
      states.push_back(s);
    }
  }
  std::string have;
  for (const KeyState& s : states) {
    have += " [" + s.name + "]";
  }
  CHECK_MESSAGE(states.size() == 1u + wanted.size(), "the states of the meter screens are in the table, found:" << have);
  for (const KeyState& state : states) {
    std::string error;
    const Snap one = run_control(state, "wait 500\n", card.dir.path(), error);
    const Snap two = run_control(state, "wait 500\n", card.dir.path(), error);
    CHECK_MESSAGE(error.empty(), state.name << ": " << error);
    CHECK_MESSAGE(one.fields.count("screen") == 1u, state.name << ": the state was read");
    CHECK_FALSE_MESSAGE(differs(one, two, false), state.name << ": two controls differ");
    CHECK_MESSAGE(one.digest == two.digest, state.name << ": the document differs");
    CHECK_MESSAGE(one.frame == two.frame, state.name << ": the picture differs");
    CHECK_MESSAGE(one.note_ons == two.note_ons, state.name << ": the notes differ");
  }
}

TEST_CASE("qanet keys: the states are distinct and every page of a screen has one") {
  const std::vector<KeyState> states = make_states();
  std::set<std::string> names;
  for (const KeyState& s : states) {
    CHECK_MESSAGE(names.insert(s.name).second, "twice: " << s.name);
  }
  CHECK(names.count("Pattern WATCH") == 1u);
  CHECK(names.count("Pattern WATCH playing") == 1u);
  CHECK(names.count("Pattern REC free") == 1u);
  CHECK(names.count("Pattern REC follow playing") == 1u);
}
